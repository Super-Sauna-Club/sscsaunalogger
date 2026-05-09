#include "indicator_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps */
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "esp_event.h"
#include "ping/ping_sock.h"


#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

struct indicator_wifi
{
    struct view_data_wifi_st  st;
    bool is_cfg;
    int wifi_reconnect_cnt;  
};

static struct indicator_wifi _g_wifi_model;
static SemaphoreHandle_t   __g_wifi_mutex;
static SemaphoreHandle_t   __g_data_mutex;
static SemaphoreHandle_t   __g_net_check_sem;


static int s_retry_num = 0;
static int wifi_retry_max  = 3;
static bool __g_ping_done = true;

/* v0.2.9: AP-fallback. Wenn STA-reconnect ueber laengere zeit fehlschlaegt
 * (kein router erreichbar / wifi-flapping), schaltet das geraet automatisch
 * auf softAP-modus um, damit a) der user weiter zugreifen kann (settings,
 * webconfig) und b) keine wifi-events mehr im event-loop spammen, was
 * sonst zu UI-freezes fuehren kann (siehe debug-session 2026-05-03).
 *
 * Threshold: __indicator_wifi_task tickt alle 5 s und zaehlt
 * wifi_reconnect_cnt hoch; bei > 30 (=150 s) wird fallback aktiv. Im
 * fallback wird is_cfg = false gesetzt, damit der task uns nicht wieder
 * auf STA umschmeisst. Der flag wird zurueckgesetzt sobald user via UI
 * eine neue STA-config triggert oder ein STA_GOT_IP event hereinkommt. */
#define SSC_AP_FALLBACK_THRESHOLD 30
#define SSC_AP_FALLBACK_SSID      "sscsauna-AP"
static bool __g_softap_fallback_active = false;
static esp_netif_t *__g_ap_netif = NULL;

static EventGroupHandle_t __wifi_event_group;

static const char *TAG = "wifi-model";

static int min(int a, int b) { return (a < b) ? a : b; }

static void __wifi_st_set( struct view_data_wifi_st *p_st )
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    memcpy( &_g_wifi_model.st, p_st, sizeof(struct view_data_wifi_st));
    xSemaphoreGive(__g_data_mutex);
}

static void __wifi_st_get(struct view_data_wifi_st *p_st )
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    memcpy(p_st, &_g_wifi_model.st, sizeof(struct view_data_wifi_st));
    xSemaphoreGive(__g_data_mutex);
}

static void __wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{

    switch (event_id)
    {
        case WIFI_EVENT_STA_START: {
            ESP_LOGI(TAG, "wifi event: WIFI_EVENT_STA_START");
            struct view_data_wifi_st st;
            st.is_connected = false;
            st.is_network   = false;
            st.is_connecting = true;
            memset(st.ssid, 0,  sizeof(st.ssid));
            st.rssi = 0;
            __wifi_st_set(&st);

            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_STA_CONNECTED: {
            ESP_LOGI(TAG, "wifi event: WIFI_EVENT_STA_CONNECTED");
            wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t*) event_data;
            struct view_data_wifi_st st;

            __wifi_st_get(&st);
            memset(st.ssid, 0,  sizeof(st.ssid));
            memcpy(st.ssid, event->ssid, event->ssid_len);
            st.rssi = -50; //todo
            st.is_connected = true;
            st.is_connecting = false;
            __wifi_st_set(&st);
            
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST, &st, sizeof(struct view_data_wifi_st ), pdMS_TO_TICKS(50));
            
            struct view_data_wifi_connet_ret_msg msg;
            msg.ret = 0;
            strcpy(msg.msg, "Connection successful");
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_CONNECT_RET, &msg, sizeof(msg), pdMS_TO_TICKS(50));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            ESP_LOGI(TAG, "wifi event: WIFI_EVENT_STA_DISCONNECTED");

            if ( (wifi_retry_max == -1) || s_retry_num < wifi_retry_max) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "retry to connect to the AP");

            } else {
                
                // update list  todo
                struct view_data_wifi_st st;
        
                __wifi_st_get(&st);
                st.is_connected = false;
                st.is_network   = false;
                st.is_connecting = false;
                __wifi_st_set(&st);

                esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST, &st, sizeof(struct view_data_wifi_st ), pdMS_TO_TICKS(50));
                
                char *p_str = "";
                struct view_data_wifi_connet_ret_msg msg;
                msg.ret = 0;
                strcpy(msg.msg, "Connection failure");
                esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_CONNECT_RET, &msg, sizeof(msg), pdMS_TO_TICKS(50));
            }
            break;
        }
    default:
        break;
    }
}

static void __ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if ( event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        /* v0.2.9: bug-fix + fallback-clear. wifi_reconnect_cnt wurde frueher
         * nie zurueckgesetzt, dadurch konnte der STA-reconnect-task im
         * worst-case staendig retries ausloesen obwohl gerade eine
         * verbindung steht. Mit AP-fallback in v0.2.9 muss er ZUSAETZLICH
         * den fallback-flag clearen, damit ein erfolgreicher reconnect
         * den AP-modus wieder verlaesst. */
        _g_wifi_model.wifi_reconnect_cnt = 0;
        __g_softap_fallback_active = false;

        //xEventGroupSetBits(__wifi_event_group, WIFI_CONNECTED_BIT);
        xSemaphoreGive(__g_net_check_sem);  //goto check network
    }
}


// bool indicator_wifi_is_connect(char *p_ssid, int *p_rssi)
// {
//     return true; //todo
// }

static int __wifi_scan(wifi_ap_record_t *p_ap_info, uint16_t number)
{
    /* v0.3.3: alle ESP_ERROR_CHECKs durch graceful return ersetzt.
     * Wifi-scan ist UI-feature (settings -> wlan -> scan-list). Unter
     * DRAM-druck (gleicher trigger wie v0.3.2 wifi-init) konnten die
     * esp_wifi_*-calls failen und ESP_ERROR_CHECK -> abort -> reboot.
     * Jetzt: scan gibt -1 zurueck, UI zeigt leere liste, kein crash.    */
    uint16_t ap_count = 0;
    esp_err_t err;

    /* Bei jedem fehler return 0 (= keine APs gefunden). Caller (zeile ~543)
     * castet result in uint16_t und nutzt es als loop-bound; -1 wuerde
     * zu garbage-iteration ueber uninitialized ap_info[] fuehren.        */
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_set_mode fehlgeschlagen: %s", esp_err_to_name(err));
        return 0;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_start fehlgeschlagen: %s", esp_err_to_name(err));
        return 0;
    }
    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_scan_start fehlgeschlagen: %s", esp_err_to_name(err));
        return 0;
    }
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_scan_get_ap_num fehlgeschlagen: %s", esp_err_to_name(err));
        return 0;
    }
    err = esp_wifi_scan_get_ap_records(&number, p_ap_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_scan_get_ap_records fehlgeschlagen: %s", esp_err_to_name(err));
        return 0;
    }
    ESP_LOGI(TAG, " scan ap cont: %d", ap_count);

    for (int i = 0; (i < number) && (i < ap_count); i++) {
        ESP_LOGI(TAG, "SSID: %s, RSSI:%d, Channel: %d", p_ap_info[i].ssid, p_ap_info[i].rssi, p_ap_info[i].primary);
    }
    return ap_count;
}


static int __wifi_connect(const char *p_ssid, const char *p_password, int retry_num)
{
    wifi_retry_max = retry_num; //todo
    s_retry_num =0;

    /* v0.2.9: user hat ueber UI eine neue STA-config getriggert.
     * Damit verlassen wir explizit den AP-fallback-modus. */
    __g_softap_fallback_active = false;
    _g_wifi_model.wifi_reconnect_cnt = 0;

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, p_ssid, sizeof(wifi_config.sta.ssid));
    ESP_LOGI(TAG, "ssid: %s", p_ssid);
    if( p_password ) {
        ESP_LOGI(TAG, "password: %s", p_password);
        strlcpy((char *)wifi_config.sta.password, p_password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; //todo
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );

    _g_wifi_model.is_cfg = true;

    struct view_data_wifi_st st = {0};
    st.is_connected = false;
    st.is_connecting = false;
    st.is_network   = false;
    __wifi_st_set(&st);

    ESP_ERROR_CHECK(esp_wifi_start());
    //esp_wifi_connect();
    
    ESP_LOGI(TAG, "connect...");
}

static void __wifi_cfg_restore(void) 
{
    _g_wifi_model.is_cfg = false;
    
    struct view_data_wifi_st st = {0};
    st.is_connected = false;
    st.is_connecting = false;
    st.is_network   = false;
    __wifi_st_set(&st);

    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST, &st, sizeof(struct view_data_wifi_st ), pdMS_TO_TICKS(50));

    // restore and stop
    esp_wifi_restore();
}

static void __wifi_shutdown(void)
{
    _g_wifi_model.is_cfg = false;  //disable reconnect

    struct view_data_wifi_st st = {0};
    st.is_connected = false;
    st.is_connecting = false;
    st.is_network   = false;
    __wifi_st_set(&st);

    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST, &st, sizeof(struct view_data_wifi_st ), pdMS_TO_TICKS(50));

    esp_wifi_stop();
}

/* v0.2.14: oeffentliche API fuer settings-toggle "WLAN ON/OFF".
 * en=false: shutdown (esp_wifi_stop + is_cfg=false damit task nicht
 *           reconnected).
 * en=true:  esp_wifi_start; auto-reconnect uebernimmt die existierenden
 *           credentials wenn vorhanden. is_cfg bleibt false bis user
 *           explizit "VERBINDEN" druckt - sonst wuerde reconnect-loop
 *           die kurz nach reboot kaputten APs hammern.                */
void indicator_wifi_set_enabled(bool en)
{
    if (en) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "wifi_start in enable: %d", err);
        }
    } else {
        __wifi_shutdown();
    }
}

static void __ping_end(esp_ping_handle_t hdl, void *args)
{
    ip_addr_t target_addr;
    uint32_t transmitted =0;
    uint32_t received =0;
    uint32_t total_time_ms =0 ;
    uint32_t loss = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));
    
    if( transmitted > 0 ) {
        loss = (uint32_t)((1 - ((float)received) / transmitted) * 100);
    } else {
        loss = 100;
    }
     
    if (IP_IS_V4(&target_addr)) {
        printf("\n--- %s ping statistics ---\n", inet_ntoa(*ip_2_ip4(&target_addr)));
    } else {
        printf("\n--- %s ping statistics ---\n", inet6_ntoa(*ip_2_ip6(&target_addr)));
    }
    printf("%d packets transmitted, %d received, %d%% packet loss, time %dms\n",
           transmitted, received, loss, total_time_ms);

    esp_ping_delete_session(hdl);

    struct view_data_wifi_st st;
    if( received > 0) {
        __wifi_st_get(&st);
        st.is_network = true;
        __wifi_st_set(&st);
    } else {
        __wifi_st_get(&st);
        st.is_network = false;
        __wifi_st_set(&st);
    }
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST, &st, sizeof(struct view_data_wifi_st ), pdMS_TO_TICKS(50));
    __g_ping_done = true;
}

static void __ping_start(void)
{
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();

    ip_addr_t target_addr;
    ipaddr_aton("1.1.1.1", &target_addr);

    config.target_addr = target_addr;

    esp_ping_callbacks_t cbs = {
        .cb_args = NULL,
        .on_ping_success = NULL,
        .on_ping_timeout = NULL,
        .on_ping_end = __ping_end
    };
    /* v0.2.7: error-handling fuer esp_ping_new_session. Wenn der heap
     * fragmentiert ist und die interne ping-task nicht erstellt werden
     * kann, liefert new_session ESP_ERR_NO_MEM - aber `ping` bleibt
     * uninitialisiert. esp_ping_start mit garbage-handle → assert in
     * xTaskGenericNotify → PANIC-reboot-loop. Beobachtet auf v0.2.7 nach
     * WIFI_EVENT_STA_CONNECTED. Defensiver fix: initialisieren + checken. */
    esp_ping_handle_t ping = NULL;
    esp_err_t err = esp_ping_new_session(&config, &cbs, &ping);
    if (err != ESP_OK || ping == NULL) {
        ESP_LOGW(TAG, "ping new_session failed: %s - skipping ping",
                 esp_err_to_name(err));
        /* Als "Netz-OK" werten auch ohne ping-verify, damit NTP/TZ-lookup
         * weiterlaufen. Andere-falls haengt der User in is_network=false
         * fest und bekommt nie eine NTP-Sync.                            */
        struct view_data_wifi_st st;
        __wifi_st_get(&st);
        st.is_network = st.is_connected;
        __wifi_st_set(&st);
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_WIFI_ST, &st, sizeof(st),
                          pdMS_TO_TICKS(100));
        __g_ping_done = true;
        return;
    }
    __g_ping_done = false;
    err = esp_ping_start(ping);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ping_start failed: %s", esp_err_to_name(err));
        esp_ping_delete_session(ping);
        __g_ping_done = true;
    }
}

/* v0.2.9: STA -> softAP fallback. Wechselt das WIFI-subsystem in den
 * AP-modus mit fixer SSID. Wird vom __indicator_wifi_task gerufen, wenn
 * STA-reconnects ueber laengere zeit fehlschlagen (siehe Threshold).
 *
 * Race-anmerkung: laeuft im wifi-task-context, nicht im event-handler.
 * Die esp_wifi_*-aufrufe sind blocking, aber das ist ok hier - der
 * task hat nur diese eine arbeit. Wichtig: vor dem AP-modus-wechsel
 * den default-AP-netif anlegen, falls noch nicht geschehen (init macht
 * nur den STA-netif).
 *
 * Sicherheit: AP ist OPEN ohne password. Akzeptabel weil fallback-szenario
 * (wenn user den hotspot wirklich nutzen will, kann er WLAN-config
 * eingeben). Keine offene services dahinter ausser dem ESP32-DHCP-server. */
static void __wifi_softap_start_fallback(void)
{
    ESP_LOGW(TAG, "STA-connect failed for >%us — switching to softAP fallback (%s)",
             SSC_AP_FALLBACK_THRESHOLD * 5, SSC_AP_FALLBACK_SSID);

    esp_wifi_stop();

    if (__g_ap_netif == NULL) {
        __g_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, SSC_AP_FALLBACK_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len      = strlen(SSC_AP_FALLBACK_SSID);
    ap_cfg.ap.channel       = 6;
    ap_cfg.ap.max_connection = 2;
    ap_cfg.ap.authmode      = WIFI_AUTH_OPEN;
    ap_cfg.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(AP) failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config(AP) failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start(AP) failed: %s", esp_err_to_name(err));
        return;
    }

    __g_softap_fallback_active = true;
    /* is_cfg=false verhindert dass der periodische STA-reconnect-task
     * uns wieder zurueckschmeisst. Wenn der user spaeter im UI ein
     * neues WIFI konfiguriert, ruft __view_event_handler::VIEW_EVENT_WIFI_CONNECT
     * → __wifi_connect, das setzt is_cfg=true und schaltet zurueck auf STA. */
    _g_wifi_model.is_cfg = false;

    /* UI mitteilen dass wir im AP-modus sind: SSID-feld zeigt den AP-namen
     * damit user das im wifi-status-screen sieht. */
    struct view_data_wifi_st st;
    __wifi_st_get(&st);
    st.is_connected  = false;
    st.is_network    = false;
    st.is_connecting = false;
    strlcpy(st.ssid, SSC_AP_FALLBACK_SSID, sizeof(st.ssid));
    __wifi_st_set(&st);
    /* timeout statt portMAX_DELAY: wenn die queue grad voll ist, lieber
     * den status-update droppen als hier blocken. */
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST,
                      &st, sizeof(st), pdMS_TO_TICKS(50));
}

// net check
static void __indicator_wifi_task(void *p_arg)
{
    int cnt = 0;
    struct view_data_wifi_st st;

    while(1) {

        xSemaphoreTake(__g_net_check_sem, pdMS_TO_TICKS(5000));
        __wifi_st_get(&st);

        // Periodically check the network connection status
        if( st.is_connected) {

            if(__g_ping_done ) {
                if( st.is_network ) {
                    cnt++;
                    //5min check network
                    if( cnt > 60) {
                        cnt = 0;
                        ESP_LOGI(TAG, "Network normal last time, retry check network...");
                        __ping_start();
                    }
                } else {
                    ESP_LOGI(TAG, "Last network exception, check network...");
                    __ping_start();
                }
            }

        } else if(  _g_wifi_model.is_cfg && !st.is_connecting) {
            // Periodically check the wifi connection status

            /* v0.2.9: AP-fallback wenn STA-reconnect zu lange fehlschlaegt.
             * Die alte logik hat nach >5 ticks (~25s) den STA-stack neu
             * gestartet, das war aber reine wiederholung mit dem gleichen
             * router der nicht erreichbar ist. Jetzt: nach
             * SSC_AP_FALLBACK_THRESHOLD ticks (~150s) auf softAP umstellen. */
            if (!__g_softap_fallback_active &&
                _g_wifi_model.wifi_reconnect_cnt > SSC_AP_FALLBACK_THRESHOLD) {
                __wifi_softap_start_fallback();
                _g_wifi_model.wifi_reconnect_cnt = 0;
                continue;
            }

            // 5min retry connect
            if( _g_wifi_model.wifi_reconnect_cnt > 5 ) {
                ESP_LOGI(TAG, " Wifi reconnect...");
                _g_wifi_model.wifi_reconnect_cnt =0;
                wifi_retry_max = 3;
                s_retry_num =0;

                /* v0.3.3: war ESP_ERROR_CHECK -> abort. Periodischer
                 * reconnect-tick alle 5s, bei DRAM-druck nach laengerer
                 * uptime + heap-fragmentation konnte das einen reboot
                 * ausloesen. Jetzt graceful: log + naechster tick
                 * versucht es erneut.                                  */
                esp_wifi_stop();
                esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "reconnect: set_mode fehlgeschlagen: %s",
                             esp_err_to_name(err));
                } else {
                    err = esp_wifi_start();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "reconnect: wifi_start fehlgeschlagen: %s",
                                 esp_err_to_name(err));
                    }
                }
            }
            _g_wifi_model.wifi_reconnect_cnt++;
        }

    }
}



static void __view_event_handler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    switch (id)
    {
        case VIEW_EVENT_WIFI_LIST_REQ: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_WIFI_LIST_REQ");

            uint16_t number = WIFI_SCAN_LIST_SIZE;
            uint16_t ap_count = 0;
            wifi_ap_record_t ap_info[WIFI_SCAN_LIST_SIZE];
            ap_count = __wifi_scan(ap_info, number);

            struct view_data_wifi_list list;
            struct view_data_wifi_st st;

            memset(&list, 0 , sizeof(struct view_data_wifi_list ));
            
            __wifi_st_get(&st);

            list.is_connect = st.is_connected;
            if( st.is_connected ) {
                strlcpy((char *)list.connect.ssid, (char *)st.ssid, sizeof(list.connect.ssid));
                list.connect.auth_mode =false;
                list.connect.rssi = st.rssi;
            }
            
            ap_count= min(number, ap_count);
            
            bool is_exist =  false;
            int list_cnt = 0;
            for(int i = 0; i <  ap_count; i++ ) {

                is_exist = false;
                for( int j = 0; j < list_cnt; j++ ) {
                    if(strcmp(list.aps[j].ssid, ap_info[i].ssid) == 0) {
                        ESP_LOGI(TAG, "list exit ap:%s", ap_info[i].ssid);
                        is_exist = true;
                        break;
                    }
                }
                if(!is_exist) {
                    strcpy(list.aps[list_cnt].ssid, ap_info[i].ssid);
                    list.aps[list_cnt].rssi = ap_info[i].rssi;
                    if( ap_info[i].authmode == WIFI_AUTH_OPEN) {
                        list.aps[list_cnt].auth_mode = false;
                    } else {
                        list.aps[list_cnt].auth_mode = true;
                    } 
                    list_cnt++;
                }
            }
            list.cnt = list_cnt;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_LIST, &list, sizeof(struct view_data_wifi_list ), pdMS_TO_TICKS(50));

            break;
        }
        case VIEW_EVENT_WIFI_CONNECT: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_WIFI_CONNECT");
            struct view_data_wifi_config * p_cfg = (struct view_data_wifi_config *)event_data;

            if( p_cfg->have_password) {
                __wifi_connect(p_cfg->ssid, p_cfg->password, 3);
            } else {
                __wifi_connect(p_cfg->ssid, NULL, 3);
            }
            break;
        }  
        case VIEW_EVENT_WIFI_CFG_DELETE: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_WIFI_CFG_DELETE");
            __wifi_cfg_restore();
            break;
        }
        case VIEW_EVENT_SHUTDOWN: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SHUTDOWN");
            __wifi_shutdown();
            break;
        }
    default:
        break;
    }
}

static void __wifi_model_init(void)
{
    memset(&_g_wifi_model, 0, sizeof(_g_wifi_model));
}

int indicator_wifi_init(void)
{
    __g_wifi_mutex  = xSemaphoreCreateMutex( );
    __g_data_mutex  =  xSemaphoreCreateMutex();
    __g_net_check_sem = xSemaphoreCreateBinary();
    //__wifi_event_group = xEventGroupCreate();

    __wifi_model_init();
    
    /* Stack in PSRAM (DRAM zu eng). */
    xTaskCreateWithCaps(&__indicator_wifi_task, "__indicator_wifi_task", 1024 * 5, NULL, 10, NULL, MALLOC_CAP_SPIRAM);


    /* WiFi-init darf NICHT fatal sein. Hintergrund: am 2026-05-09
     * gefunden dass esp_wifi_init() unter DRAM-druck regelmaessig
     * mit ESP_ERR_NO_MEM scheitert (allokiert 10 statische rx-buffer
     * a 1600 byte = 16KB im internen DRAM). Vorher per ESP_ERROR_CHECK
     * gewrappt -> abort -> bootloop. Sauna-logger braucht WiFi nur
     * fuer NTP-sync und settings-page; sessions/SD/UI laufen ohne.
     * Bei fail: fruehzeitig raus, gerat bootet weiter ohne WiFi. */
    esp_err_t ne = esp_netif_init();
    if (ne != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s - WiFi disabled, sauna-logger continues",
                 esp_err_to_name(ne));
        return -1;
    }
    esp_err_t ee = esp_event_loop_create_default();
    if (ee != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s - WiFi disabled",
                 esp_err_to_name(ee));
        return -1;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t we = esp_wifi_init(&cfg);
    if (we != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s - WiFi disabled, sauna-logger continues",
                 esp_err_to_name(we));
        return -1;
    }

    /* Defense-in-depth: alle weiteren WiFi-init-calls non-fatal.
     * Falls einer fail't (theoretisch unter DRAM-druck moeglich) bricht
     * indicator_wifi_init nur ab statt das geraet zu crashen. */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &__wifi_event_handler,
                                                        0,
                                                        &instance_any_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT handler failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &__ip_event_handler,
                                              0,
                                              &instance_got_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = esp_event_handler_instance_register_with(view_event_handle,
                                                   VIEW_EVENT_BASE, VIEW_EVENT_WIFI_LIST_REQ,
                                                   __view_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register VIEW_EVENT_WIFI_LIST_REQ failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = esp_event_handler_instance_register_with(view_event_handle,
                                                   VIEW_EVENT_BASE, VIEW_EVENT_WIFI_CONNECT,
                                                   __view_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register VIEW_EVENT_WIFI_CONNECT failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = esp_event_handler_instance_register_with(view_event_handle,
                                                   VIEW_EVENT_BASE, VIEW_EVENT_WIFI_CFG_DELETE,
                                                   __view_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register VIEW_EVENT_WIFI_CFG_DELETE failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = esp_event_handler_instance_register_with(view_event_handle,
                                                   VIEW_EVENT_BASE, VIEW_EVENT_SHUTDOWN,
                                                   __view_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register VIEW_EVENT_SHUTDOWN failed: %s", esp_err_to_name(err));
        return -1;
    }

    wifi_config_t wifi_cfg;
    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

    if (strlen((const char *) wifi_cfg.sta.ssid)) {
        _g_wifi_model.is_cfg = true;
        ESP_LOGI(TAG, "last config ssid: %s",  wifi_cfg.sta.ssid);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
            return -1;
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s - WiFi disabled", esp_err_to_name(err));
            return -1;
        }
    } else {
        ESP_LOGI(TAG, "Not config wifi, Entry wifi config screen");
        uint8_t screen = SCREEN_WIFI_CONFIG;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
            return -1;
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s - WiFi disabled", esp_err_to_name(err));
            return -1;
        }
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_START, &screen, sizeof(screen), pdMS_TO_TICKS(50));
    }

    return 0;
}
