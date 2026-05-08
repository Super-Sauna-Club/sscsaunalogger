#include "indicator_time.h"
#include "indicator_storage.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include<stdlib.h>
#include<string.h>
#include<sys/time.h>
#include "nvs.h"
#include "esp_timer.h"

#define TIME_CFG_STORAGE      "time-cfg"
/* v0.2.7: persistierte wall-clock + source fuer boot-fallback.
 * Key im gemeinsamen 'indicator'-NVS-namespace via indicator_storage_*.   */
#define TIME_STATE_STORAGE    "time_state"
#define TIME_NET_TZ_STORAGE   "time_net_tz"

/* NVS-persist-Rate: alle 60s sichern, solange source == NTP oder MANUAL.
 * 60s reicht: im worst case verlieren wir nach Stromausfall <60s an Zeit-
 * Genauigkeit, was fuer Sauna-Sessions irrelevant ist. Haeufiger waere
 * unnoetiger flash-wear.                                                */
#define TIME_STATE_PERSIST_INTERVAL_S  60
/* Max-Alter einer NVS-gespeicherten Zeit bevor wir ihr nicht mehr trauen.
 * 7 Tage: wenn das Geraet eine Woche aus war, ist die Zeit garantiert
 * so weit gedriftet dass sie keinen Mehrwert gegenueber Compile-Time hat. */
#define TIME_STATE_MAX_AGE_S       (7 * 24 * 3600)
/* NTP-Watchdog: wenn 5 min nach Boot kein NTP-Erfolg, SNTP neu starten. */
#define TIME_NTP_WATCHDOG_MS        (5 * 60 * 1000)

struct indicator_time
{
    struct view_data_time_cfg cfg;
    char net_zone[64];

    /* v0.2.7: zeit-quality tracking */
    uint8_t  src;              /* time_source_t */
    time_t   last_sync_ts;     /* wann zuletzt NTP oder MANUAL, in UTC */
    uint32_t last_persist_s;   /* sekunden-epoch der letzten NVS-persist */
};

/* Persistiertes Format fuer NVS (passt in ein nvs_set_blob). */
struct time_state_nvs {
    uint8_t  version;   /* 1 */
    uint8_t  src;
    uint8_t  _pad[2];
    int64_t  last_sync_ts;  /* nutzen int64 damit 2038-safe */
} __attribute__((packed));

static const char *TAG = "time";

static struct indicator_time __g_time_model;
static SemaphoreHandle_t       __g_data_mutex;

static esp_timer_handle_t   view_update_timer_handle;
static esp_timer_handle_t   ntp_watchdog_timer_handle;

/* forward-decls fuer interne state-helpers */
static void __time_state_set(uint8_t src, time_t last_sync_ts);
static void __time_state_broadcast(void);
static void __time_state_persist_if_trusted(void);
static void __time_state_load_and_apply_fallback(void);
static void __time_net_tz_save(const char *tz);
static int  __time_net_tz_load(char *buf, size_t buflen);

/* Parst __DATE__ "Apr 22 2026" + __TIME__ "15:30:45" in time_t als UTC.
 * (bisher lag der compile-fallback in ui_sauna.c - umgezogen damit das
 * gesamte zeit-handling an einer stelle lebt.)                         */
static time_t __boot_compile_time(void) {
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    const char *t = __TIME__;
    struct tm tmv = {0};
    char mon[4] = { d[0], d[1], d[2], 0 };
    const char *p = strstr(months, mon);
    tmv.tm_mon  = p ? (int)((p - months) / 3) : 0;
    tmv.tm_mday = atoi(d + 4);
    tmv.tm_year = atoi(d + 7) - 1900;
    tmv.tm_hour = atoi(t + 0);
    tmv.tm_min  = atoi(t + 3);
    tmv.tm_sec  = atoi(t + 6);
    return mktime(&tmv);
}

static void __time_cfg_set(struct view_data_time_cfg *p_cfg )
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    memcpy( &__g_time_model.cfg, p_cfg, sizeof(struct view_data_time_cfg));
    xSemaphoreGive(__g_data_mutex);
}

static void __time_cfg_get(struct view_data_time_cfg *p_cfg )
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    memcpy(p_cfg, &__g_time_model.cfg, sizeof(struct view_data_time_cfg));
    xSemaphoreGive(__g_data_mutex);
}


static void __time_cfg_save(struct view_data_time_cfg *p_cfg )
{
    esp_err_t ret = 0;
    ret = indicator_storage_write(TIME_CFG_STORAGE, (void *)p_cfg, sizeof(struct view_data_time_cfg));
    if( ret != ESP_OK ) {
        ESP_LOGI(TAG, "cfg write err:%d", ret);
    } else {
        ESP_LOGI(TAG, "cfg write successful");
    }
}

static void __time_cfg_print(struct view_data_time_cfg *p_cfg )
{
    printf( "time_format_24:%d, auto_update:%d, time:%d, auto_update_zone:%d, zone:%d, daylight:%d\n",  \
      (bool) p_cfg->time_format_24, (bool)p_cfg->auto_update, (long)p_cfg->time, (bool)p_cfg->auto_update_zone, (int8_t)p_cfg->zone, (bool)p_cfg->daylight);
}


static void __time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI("ntp", "Notification of a time synchronization event");
    struct view_data_time_cfg cfg;
    __time_cfg_get(&cfg);
    bool time_format_24 = cfg.time_format_24;
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_TIME, &time_format_24, sizeof(time_format_24), portMAX_DELAY);

    /* v0.2.7: frisch synced - quality auf NTP_SYNCED heben. tv->tv_sec
     * ist die neue wall-clock (utc).                                   */
    time_t now_ts = (tv && tv->tv_sec > 0) ? tv->tv_sec : time(NULL);
    __time_state_set(TIME_SRC_NTP_SYNCED, now_ts);
    __time_state_broadcast();
    /* Sofort persistieren damit direkt nach NTP ein Stromausfall uns
     * nicht auf compile-time zurueckwirft.                             */
    __time_state_persist_if_trusted();
}

static void __time_set(time_t time)
{
    struct tm tm = {4, 14, 3, 19, 0, 138, 0, 0, 0};
    struct timeval timestamp = { time, 0 };
    settimeofday(&timestamp, NULL);
}

static void __time_sync_enable(void)
{
    sntp_init();
}

static void __time_sync_stop(void)
{
    sntp_stop();
}

/* v0.2.14: oeffentliche API fuer settings-toggle "NTP ON/OFF".
 * en=false: sntp_stop - manuelle zeit bleibt erhalten, wird nicht
 *           ueberschrieben.
 * en=true:  sntp_init - poll laeuft sobald wifi up ist.              */
void indicator_time_set_ntp_enabled(bool en)
{
    if (en) {
        __time_sync_enable();
    } else {
        __time_sync_stop();
    }
}

static void __time_zone_set(struct view_data_time_cfg *p_cfg)
{
    char zone_str[64] = {0};

    if ( !p_cfg->auto_update_zone) {
        /* Manual timezone setting */
        int8_t zone = p_cfg->zone;

        /* Daylight saving adds 1 hour to the offset */
        if( p_cfg->daylight) {
            zone += 1;
        }
        /* POSIX TZ format: UTC-X means local time is X hours ahead of UTC */
        if( zone >= 0) {
            snprintf(zone_str, sizeof(zone_str) - 1, "UTC-%d", zone);
        } else {
            snprintf(zone_str, sizeof(zone_str) - 1, "UTC+%d", -zone);
        }
        ESP_LOGI(TAG, "Manual timezone: zone=%d, daylight=%d, TZ=%s",
                 p_cfg->zone, p_cfg->daylight, zone_str);
    } else {
        /* Auto timezone from network */
        xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
        strncpy(zone_str, __g_time_model.net_zone, sizeof(zone_str) - 1);
        xSemaphoreGive(__g_data_mutex);

        if( strlen(zone_str) == 0 ) {
            /* v0.2.7: Default auf Europe/Vienna (Oberes Piestingtal, AT)
             * statt UTC-0. Deployment-Standort ist fest. Wenn indicator_city
             * spaeter eine andere Zone liefert, wird sie ueber
             * indicator_time_net_zone_set() ueberschrieben + persistiert.
             * POSIX-TZ "CET-1CEST,M3.5.0,M10.5.0/3": winter=UTC+1 (CET),
             * sommer=UTC+2 (CEST), DST start letzter So im Maerz, Ende
             * letzter So im Oktober um 03:00.                              */
            ESP_LOGW(TAG, "Auto timezone: net_zone leer, default Europe/Vienna");
            strncpy(zone_str, "CET-1CEST,M3.5.0,M10.5.0/3",
                    sizeof(zone_str) - 1);
        } else {
            ESP_LOGI(TAG, "Auto timezone from network: TZ=%s", zone_str);
        }
    }

    /* Apply timezone */
    ESP_LOGI(TAG, "Applying TZ environment variable: %s", zone_str);
    setenv("TZ", zone_str, 1);
    tzset();
}

static void __time_cfg(struct view_data_time_cfg *p_cfg, bool set_time)
{
    /* v0.2.14: TZ IMMER setzen (auch im manual-mode) - ohne TZ-env
     * defaultet mktime/localtime auf UTC was die manuelle zeit-eingabe
     * verschiebt. __time_zone_set ist idempotent.                     */
    __time_zone_set(p_cfg);

    if( p_cfg->auto_update ) {
        __time_sync_enable();
    } else {
        __time_sync_stop();
        struct timeval timestamp = { p_cfg->time, 0 };
        if( set_time ) {
            settimeofday(&timestamp, NULL);
        }
    }
}

static void __time_view_update_callback(void* arg)
{
    static int last_min = 60;
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    if( timeinfo.tm_min != last_min) {
        last_min = timeinfo.tm_min;

        struct view_data_time_cfg cfg;
        __time_cfg_get(&cfg);
        bool time_format_24 = cfg.time_format_24;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_TIME, &time_format_24, sizeof(time_format_24), portMAX_DELAY);
        ESP_LOGI(TAG, "need update time view");
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG, "%s", strftime_buf);
    }

    /* v0.2.7: alle 60s NVS-persist + state-broadcast, damit die UI
     * das "vor X min synced"-badge aktualisiert. Wird nur persisted
     * wenn source NTP oder MANUAL ist.                              */
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    uint32_t last_p = __g_time_model.last_persist_s;
    xSemaphoreGive(__g_data_mutex);
    if ((uint32_t)now - last_p >= TIME_STATE_PERSIST_INTERVAL_S) {
        __time_state_persist_if_trusted();
        __time_state_broadcast();
    }
}

static __time_view_update_init(void)
{
    const esp_timer_create_args_t timer_args = {
            .callback = &__time_view_update_callback,
            /* argument specified here will be passed to timer callback function */
            .arg = (void*) view_update_timer_handle,
            .name = "time update"
    };
    ESP_ERROR_CHECK( esp_timer_create(&timer_args, &view_update_timer_handle));
    ESP_ERROR_CHECK(esp_timer_start_periodic(view_update_timer_handle, 1000000)); //1s
}


static void __view_event_handler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    switch (id)
    {
        case VIEW_EVENT_TIME_CFG_APPLY: {
            struct view_data_time_cfg * p_cfg = (struct view_data_time_cfg *)event_data;
            ESP_LOGI(TAG, "event: VIEW_EVENT_TIME_CFG_APPLY");
            __time_cfg_print(p_cfg);
            __time_cfg_set(p_cfg);
            __time_cfg_save(p_cfg);
            __time_cfg(p_cfg, p_cfg->set_time);  //config;

            bool time_format_24 = p_cfg->time_format_24;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_TIME, &time_format_24, sizeof(time_format_24), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_WIFI_ST: {
            /* v0.2.7: fist-flag ENTFERNT. Bisher wurde NTP nur beim ERSTEN
             * WiFi-Connect versucht - wenn der Server damals nicht antwortete
             * gab es keinen Retry bei Reconnect, Zeit blieb ewig auf
             * compile-time. Jetzt triggern wir bei JEDEM "is_network"-Event
             * sntp_stop+sntp_enable, aber nur wenn wir noch nicht frisch
             * synced sind - sonst sparen wir das Netz-Geraffel.          */
            ESP_LOGI(TAG, "event: VIEW_EVENT_WIFI_ST");
            struct view_data_wifi_st *p_st = ( struct view_data_wifi_st *)event_data;
            if( p_st && p_st->is_network) {
                uint8_t cur_src;
                time_t  cur_last;
                xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
                cur_src  = __g_time_model.src;
                cur_last = __g_time_model.last_sync_ts;
                xSemaphoreGive(__g_data_mutex);

                time_t nowt = 0; time(&nowt);
                bool stale = (cur_src != TIME_SRC_NTP_SYNCED) ||
                             (cur_last == 0) ||
                             ((nowt - cur_last) > 600);
                if (!stale) {
                    ESP_LOGI(TAG, "WIFI_ST: NTP noch frisch (vor %lds), skip retry",
                             (long)(nowt - cur_last));
                    break;
                }
                struct view_data_time_cfg cfg;
                __time_cfg_get(&cfg);
                if( cfg.auto_update ) {
                    ESP_LOGW(TAG, "WIFI_ST: network up, NTP stale (src=%u) - retry sntp",
                             (unsigned)cur_src);
                    __time_sync_stop();
                    __time_sync_enable();
                }
            }
            break;
        }
        case VIEW_EVENT_TIME_MANUAL_SET: {
            /* User hat im Settings-Dialog eine Zeit gesetzt - sofort
             * uebernehmen, source=MANUAL, persistieren, broadcast.  */
            struct view_data_time_manual *m = (struct view_data_time_manual *)event_data;
            if (!m) break;
            struct tm tmv = {0};
            tmv.tm_year = m->year - 1900;
            tmv.tm_mon  = m->mon - 1;
            tmv.tm_mday = m->day;
            tmv.tm_hour = m->hour;
            tmv.tm_min  = m->minute;
            tmv.tm_sec  = m->sec;
            /* v0.2.14 BUG-FIX: tm_isdst=-1 sagt mktime "guess DST aus den
             * TZ-rules". Vorher war tm_isdst=0 (struct nullinit) und
             * mktime nahm winter-zeit (CET) an - das hat im sommer (CEST)
             * die uhr 1h zu spaet gesetzt (17:23 -> display 18:23).
             * v0.2.14 ZUSATZ: Falls TZ-env nicht gesetzt war (auto_update=
             * false plus net_zone leer), explizit Vienna setzen damit
             * mktime nicht UTC annimmt (17:23 -> display 19:23).         */
            const char *cur_tz = getenv("TZ");
            if (!cur_tz || !cur_tz[0]) {
                setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
                tzset();
                ESP_LOGW(TAG, "TIME_MANUAL_SET: TZ war leer - default Vienna");
            }
            tmv.tm_isdst = -1;
            time_t ts = mktime(&tmv);
            if (ts < 1700000000) {
                ESP_LOGW(TAG, "TIME_MANUAL_SET: unplausible ts=%ld - ignored", (long)ts);
                break;
            }
            struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGW(TAG, "TIME_MANUAL_SET: applied ts=%ld (%04d-%02d-%02d %02d:%02d:%02d)",
                     (long)ts, m->year, m->mon, m->day, m->hour, m->minute, m->sec);
            __time_state_set(TIME_SRC_MANUAL, ts);
            __time_state_persist_if_trusted();
            __time_state_broadcast();
            /* Nach manual-set laufendes SNTP stoppen damit es nicht
             * silent ueberschreibt - wird beim naechsten WiFi-connect
             * wieder aktiviert wenn auto_update an ist.              */
            break;
        }
    default:
        break;
    }
}


/* ======================================================================= */
/* v0.2.7: Time-Quality-State + NVS-Persist                                 */
/* ======================================================================= */

static void __time_state_set(uint8_t src, time_t last_sync_ts)
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    __g_time_model.src = src;
    if (last_sync_ts > 0) __g_time_model.last_sync_ts = last_sync_ts;
    xSemaphoreGive(__g_data_mutex);
}

void indicator_time_state_get(struct view_data_time_state *out)
{
    if (!out) return;
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    out->source       = __g_time_model.src;
    out->last_sync_ts = __g_time_model.last_sync_ts;
    xSemaphoreGive(__g_data_mutex);
}

static void __time_state_broadcast(void)
{
    struct view_data_time_state st;
    indicator_time_state_get(&st);
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_TIME_STATE_UPDATE,
                      &st, sizeof(st), pdMS_TO_TICKS(100));
}

/* Persistiert die aktuelle wall-clock + source in NVS, aber NUR wenn die
 * Quelle vertrauenswuerdig ist (NTP oder MANUAL). Ein Fallback-Zustand
 * darf nicht ueberschrieben werden - sonst zementiert man die Falschzeit. */
static void __time_state_persist_if_trusted(void)
{
    uint8_t src;
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    src = __g_time_model.src;
    xSemaphoreGive(__g_data_mutex);

    if (src != TIME_SRC_NTP_SYNCED && src != TIME_SRC_MANUAL) {
        return;
    }

    time_t now = 0;
    time(&now);
    if (now < 1700000000) {
        /* Zusatz-Guard: falls trotz source-flag die wall-clock ungueltig
         * ist (z.B. kurz nach settimeofday mit schlechten daten), nicht
         * persistieren.                                                   */
        return;
    }

    struct time_state_nvs rec = {0};
    /* v0.2.14: bump zu v2. v1-records wurden eventuell mit falscher TZ
     * geschrieben (mktime nahm UTC an wenn TZ nicht gesetzt war oder
     * tm_isdst=0 in CEST-zeit). Auf load ignorieren wir v1 - user muss
     * einmalig zeit neu setzen mit der gefixten firmware.              */
    rec.version = 2;
    rec.src = src;
    rec.last_sync_ts = (int64_t)now;

    esp_err_t err = indicator_storage_write(TIME_STATE_STORAGE, &rec, sizeof(rec));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "time_state persist err=%d", err);
    } else {
        xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
        __g_time_model.last_persist_s = (uint32_t)now;
        xSemaphoreGive(__g_data_mutex);
    }
}

/* Beim Boot aufgerufen BEVOR NTP-sync Chancen bekommt. Setzt system-time
 * auf die beste verfuegbare Schaetzung:
 *   max(current_time, nvs_last_persist_ts + 60s, compile_time)
 * und markiert die source-quality. Damit liegen wir niemals vor der
 * Firmware und niemals hinter der letzten bekannten Welt-Zeit.      */
static void __time_state_load_and_apply_fallback(void)
{
    time_t compile_t = __boot_compile_time();
    time_t now_pre = 0;
    time(&now_pre);

    /* Wenn UI (ui_sauna_init) schon compile-time gesetzt hat, ist now_pre
     * bereits >= compile_t. Wir bauen die "beste Schaetzung" trotzdem
     * von Grund auf, damit NVS eine chance hat zu gewinnen.             */
    time_t picked = (now_pre > compile_t) ? now_pre : compile_t;
    uint8_t src   = TIME_SRC_COMPILE_TIME;

    struct time_state_nvs rec = {0};
    size_t len = sizeof(rec);
    esp_err_t err = indicator_storage_read(TIME_STATE_STORAGE, &rec, &len);
    if (err == ESP_OK && len == sizeof(rec) && rec.version == 1) {
        /* v0.2.14: legacy v1-record - eventuell mit falscher TZ
         * geschrieben. Verwerfen damit user explizit neu setzen muss.   */
        ESP_LOGW(TAG, "time NVS v1 verworfen (firmware-update) - "
                      "bitte zeit manuell neu setzen.");
        err = ESP_ERR_NVS_NOT_FOUND;
    }
    if (err == ESP_OK && len == sizeof(rec) && rec.version == 2 &&
        rec.last_sync_ts > 1700000000) {
        /* Sanity: NVS-Zeit muss mind so neu wie compile-time minus 7 Tage
         * sein (wenn's viel aelter ist, hat der User das Geraet lang
         * nicht gebootet und die NVS-Zeit ist nutzlos).                 */
        if (rec.last_sync_ts + TIME_STATE_MAX_AGE_S >= (int64_t)compile_t) {
            time_t nvs_t = (time_t)rec.last_sync_ts + 60;  /* +60s Sicherheitspuffer */
            if (nvs_t > picked) {
                picked = nvs_t;
                src    = TIME_SRC_NVS_FALLBACK;
            }
        } else {
            ESP_LOGW(TAG, "time NVS stale (last=%lld, compile=%ld), using compile",
                     (long long)rec.last_sync_ts, (long)compile_t);
        }
    }

    struct timeval tv = { .tv_sec = picked, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    __time_state_set(src, (src == TIME_SRC_NVS_FALLBACK) ? (time_t)rec.last_sync_ts : 0);

    char bufc[32];
    struct tm tmv;
    localtime_r(&picked, &tmv);
    strftime(bufc, sizeof(bufc), "%Y-%m-%d %H:%M:%S", &tmv);
    ESP_LOGW(TAG, "boot fallback: source=%s, applied=%s (ts=%ld)",
             (src == TIME_SRC_NVS_FALLBACK ? "NVS_FALLBACK" : "COMPILE_TIME"),
             bufc, (long)picked);
}

static void __time_net_tz_save(const char *tz)
{
    if (!tz || !*tz) return;
    /* max 63 chars + NUL, zero-padded damit read spaeter eindeutig ist. */
    char buf[64] = {0};
    strncpy(buf, tz, sizeof(buf) - 1);
    esp_err_t err = indicator_storage_write(TIME_NET_TZ_STORAGE, buf, sizeof(buf));
    if (err != ESP_OK) ESP_LOGW(TAG, "net_tz persist err=%d", err);
}

static int __time_net_tz_load(char *buf, size_t buflen)
{
    if (!buf || buflen < 2) return -1;
    char rec[64] = {0};
    size_t len = sizeof(rec);
    esp_err_t err = indicator_storage_read(TIME_NET_TZ_STORAGE, rec, &len);
    if (err != ESP_OK || len == 0 || rec[0] == 0) return -1;
    rec[sizeof(rec) - 1] = 0;
    strncpy(buf, rec, buflen - 1);
    buf[buflen - 1] = 0;
    return 0;
}

/* NTP-Watchdog: wenn 5 min nach Boot noch kein NTP-Sync angekommen ist,
 * sntp stop+start erzwingen. Hilft bei lwIP-SNTP-State-Verklemmungen
 * und macht das "der kommt nicht hoch"-Problem deutlich kuerzer.      */
static void __ntp_watchdog_callback(void* arg)
{
    uint8_t src;
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    src = __g_time_model.src;
    xSemaphoreGive(__g_data_mutex);

    if (src == TIME_SRC_NTP_SYNCED) {
        /* Alles gut - watchdog selbst stoppen, war ein one-shot. */
        return;
    }

    struct view_data_time_cfg cfg;
    __time_cfg_get(&cfg);
    if (!cfg.auto_update) {
        /* User hat auto_update aus - nicht nerven. */
        return;
    }

    ESP_LOGW(TAG, "NTP-watchdog: noch kein sync nach 5min - sntp restart");
    __time_sync_stop();
    __time_sync_enable();
}

static void __ntp_watchdog_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = &__ntp_watchdog_callback,
        .arg = NULL,
        .name = "ntp_wdt"
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &ntp_watchdog_timer_handle));
    /* Periodic alle 5 min: falls NTP nicht synced ist (z.B. WiFi erst
     * spaeter da, oder NTP-Server war beim Boot nicht erreichbar),
     * immer wieder retry statt nur einmalig. Der Callback tut nichts
     * wenn source schon NTP_SYNCED ist, also kein Netz-Traffic im
     * Normalbetrieb.                                                 */
    ESP_ERROR_CHECK(esp_timer_start_periodic(ntp_watchdog_timer_handle,
                                             (uint64_t)TIME_NTP_WATCHDOG_MS * 1000));
}

static void __time_cfg_restore(void)
{

    esp_err_t ret = 0;
    struct view_data_time_cfg cfg;
    
    size_t len = sizeof(cfg);
    
    ret = indicator_storage_read(TIME_CFG_STORAGE, (void *)&cfg, &len);
    if( ret == ESP_OK  && len== (sizeof(cfg)) ) {
        ESP_LOGI(TAG, "cfg read successful");
        __time_cfg_set(&cfg);
    } else {
        // err or not find
        if( ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "cfg not find");
        }else {
            ESP_LOGI(TAG, "cfg read err:%d", ret);
        }

        cfg.auto_update = true;
        cfg.auto_update_zone = true;
        cfg.daylight = true;
        cfg.time_format_24 = true;
        cfg.zone = 0;
        cfg.time = 0;
        __time_cfg_set(&cfg);
    }
}

int indicator_time_init(void)
{
    __g_data_mutex  =  xSemaphoreCreateMutex();

    memset(__g_time_model.net_zone, 0 , sizeof(__g_time_model.net_zone));
    __g_time_model.src = TIME_SRC_UNKNOWN;
    __g_time_model.last_sync_ts = 0;
    __g_time_model.last_persist_s = 0;

    __time_cfg_restore();

    /* v0.2.7: boot-fallback VOR sntp-init. settimeofday wird auf max(
     * NVS-persistierte-zeit+60s, compile-time) gesetzt - so startet die
     * UI mit einer plausiblen Zeit, auch ohne WLAN.                     */
    __time_state_load_and_apply_fallback();

    /* Falls zuletzt im auto-mode eine TZ erkannt wurde, diese schon
     * applizieren damit der erste localtime_r korrekte Zone nutzt.    */
    {
        char tz_buf[64] = {0};
        if (__time_net_tz_load(tz_buf, sizeof(tz_buf)) == 0) {
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            strncpy(__g_time_model.net_zone, tz_buf,
                    sizeof(__g_time_model.net_zone) - 1);
            xSemaphoreGive(__g_data_mutex);
            setenv("TZ", tz_buf, 1);
            tzset();
            ESP_LOGI(TAG, "net_tz aus NVS restauriert: TZ=%s", tz_buf);
        }
    }

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "cn.ntp.org.cn");
    sntp_set_time_sync_notification_cb(__time_sync_notification_cb);

    struct view_data_time_cfg cfg;
    __time_cfg_get(&cfg);
    __time_cfg(&cfg, true);
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_TIME_CFG_UPDATE, &cfg, sizeof(cfg), portMAX_DELAY);

    __time_view_update_init();
    __ntp_watchdog_init();

    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_TIME_CFG_APPLY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_TIME_MANUAL_SET,
                                                            __view_event_handler, NULL, NULL));

    /* Initial-broadcast damit die UI sofort ihren badge richtig setzt. */
    __time_state_broadcast();
    return 0;
}

int indicator_time_net_zone_set( char *p)
{
    ESP_LOGI(TAG, "Setting network timezone: %s", p);

    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    strncpy(__g_time_model.net_zone, p, sizeof(__g_time_model.net_zone) - 1);
    xSemaphoreGive(__g_data_mutex);

    /* v0.2.7: auch persistieren damit naechster Boot ohne WLAN die
     * richtige Zone direkt kennt.                                       */
    __time_net_tz_save(p);

    struct view_data_time_cfg cfg;
    __time_cfg_get(&cfg);

    /* Always apply timezone when received from network */
    __time_zone_set(&cfg);

    /* Log current time after timezone change */
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Local time after TZ set: %s", strftime_buf);

    /* Force display update */
    bool time_format_24 = cfg.time_format_24;
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_TIME, &time_format_24, sizeof(time_format_24), portMAX_DELAY);
    return 0;
}
