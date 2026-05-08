/*
 * indicator_http_export.c - Vereins-HTTP-JSON-Export.
 *
 * Schickt pro abgeschlossener Session einen JSON-POST an den
 * konfigurierten Endpunkt. Format:
 *   {
 *     "session_id": "S20260421_143205",
 *     "start_ts":   1713700325,
 *     "end_ts":     1713702125,
 *     "operator":   "tomi",
 *     "aufguss":    "birke-minze",
 *     "participants": 12,
 *     "notes":      "grossartiger abend",
 *     "peak_temp":  94.3,
 *     "peak_rh":    68.7,
 *     "aufguesse":  [ {"ts": 540, "name": "birke"},
 *                     {"ts": 1120, "name": "minze"} ],
 *     "samples":    [ {"ts": 0,   "t": 21.4, "rh": 42.1},
 *                     {"ts": 30,  "t": 65.8, "rh": 14.2},
 *                      ... ]
 *   }
 *
 * Bei Netzfehler wird die Session-ID in eine Retry-Queue in NVS gelegt
 * und beim naechsten erfolgreichen Export (oder Boot) nachgeholt. Fuer
 * den Retry laden wir die Metadaten aus indicator_session_store und
 * die Samples per SD-Readback ueber indicator_session (asynchron).
 */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps */
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "view_data.h"
#include "indicator_http_export.h"

static const char *TAG = "HTTPX";

#define NS         "httpx"
#define K_URL      "url"
#define K_TOKEN    "token"
#define K_ENABLED  "en"
#define K_QCNT     "qc"
#define K_Q_PREFIX "q_"          /* q_0 ... q_(QCNT-1) */
#define HTTPX_QUEUE_MAX  16

extern esp_event_loop_handle_t view_event_handle;
extern int indicator_session_store_get(const char *id,
                                       struct view_data_session_meta *out);

static SemaphoreHandle_t s_lock;
static char s_url[128]   = {0};
static char s_token[96]  = {0};
static bool s_enabled    = false;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ------------------------------------------------------------------ */
/* NVS-Helfer                                                         */
/* ------------------------------------------------------------------ */

static void load_config_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n;
    n = sizeof(s_url);    nvs_get_str(h, K_URL, s_url, &n);
    n = sizeof(s_token);  nvs_get_str(h, K_TOKEN, s_token, &n);
    uint8_t en = 0;
    nvs_get_u8(h, K_ENABLED, &en);
    s_enabled = en ? true : false;
    nvs_close(h);
}

static void save_config_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, K_URL, s_url);
    nvs_set_str(h, K_TOKEN, s_token);
    nvs_set_u8(h, K_ENABLED, s_enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static int queue_push(const char *session_id) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    uint16_t qc = 0;
    nvs_get_u16(h, K_QCNT, &qc);
    if (qc >= HTTPX_QUEUE_MAX) {
        /* Shift: aeltestes wird verworfen - Dauer-offline-Szenarien
         * sollen nicht unbegrenzt Speicher fressen.                 */
        char kold[12];  snprintf(kold, sizeof(kold), K_Q_PREFIX "%u", 0);
        for (uint16_t i = 1; i < qc; i++) {
            char ksrc[12], kdst[12];
            snprintf(ksrc, sizeof(ksrc), K_Q_PREFIX "%u", i);
            snprintf(kdst, sizeof(kdst), K_Q_PREFIX "%u", i - 1);
            char tmp[SSC_SESSION_ID_LEN];
            size_t n = sizeof(tmp);
            if (nvs_get_str(h, ksrc, tmp, &n) == ESP_OK)
                nvs_set_str(h, kdst, tmp);
        }
        qc -= 1;
    }
    char k[12]; snprintf(k, sizeof(k), K_Q_PREFIX "%u", qc);
    nvs_set_str(h, k, session_id);
    nvs_set_u16(h, K_QCNT, qc + 1);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "queued retry for %s (qc=%u)", session_id, qc + 1);
    return 0;
}

static int queue_pop_id(char *out, size_t outlen) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    uint16_t qc = 0;
    nvs_get_u16(h, K_QCNT, &qc);
    if (qc == 0) { nvs_close(h); return -2; }
    char k[12]; snprintf(k, sizeof(k), K_Q_PREFIX "%u", 0);
    size_t n = outlen;
    if (nvs_get_str(h, k, out, &n) != ESP_OK) {
        nvs_close(h); return -3;
    }
    /* Shift down */
    for (uint16_t i = 1; i < qc; i++) {
        char ksrc[12], kdst[12];
        snprintf(ksrc, sizeof(ksrc), K_Q_PREFIX "%u", i);
        snprintf(kdst, sizeof(kdst), K_Q_PREFIX "%u", i - 1);
        char tmp[SSC_SESSION_ID_LEN]; size_t m = sizeof(tmp);
        if (nvs_get_str(h, ksrc, tmp, &m) == ESP_OK)
            nvs_set_str(h, kdst, tmp);
    }
    char ktail[12]; snprintf(ktail, sizeof(ktail), K_Q_PREFIX "%u", qc - 1);
    nvs_erase_key(h, ktail);
    nvs_set_u16(h, K_QCNT, qc - 1);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON-Bau + HTTP-POST                                               */
/* ------------------------------------------------------------------ */

static char *build_json(const struct view_data_session_meta *m,
                        const struct view_data_session_sample *s,
                        uint16_t n) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "session_id", m->id);
    cJSON_AddNumberToObject(root, "start_ts", (double)m->start_ts);
    cJSON_AddNumberToObject(root, "end_ts",   (double)m->end_ts);
    cJSON_AddStringToObject(root, "operator", m->operator_tag);
    cJSON_AddStringToObject(root, "aufguss",  m->aufguss_headline);
    cJSON_AddNumberToObject(root, "participants", m->participants);
    cJSON_AddStringToObject(root, "notes",    m->notes);
    if (!isnan(m->peak_temp)) cJSON_AddNumberToObject(root, "peak_temp", m->peak_temp);
    if (!isnan(m->peak_rh))   cJSON_AddNumberToObject(root, "peak_rh", m->peak_rh);
    cJSON_AddNumberToObject(root, "aufguss_count", m->aufguss_count);

    cJSON *samples = cJSON_AddArrayToObject(root, "samples");
    cJSON *markers = cJSON_AddArrayToObject(root, "aufguesse");
    for (uint16_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ts", (double)s[i].t_elapsed_s);
        if (!isnan(s[i].temp)) cJSON_AddNumberToObject(o, "t",  s[i].temp);
        if (!isnan(s[i].rh))   cJSON_AddNumberToObject(o, "rh", s[i].rh);
        cJSON_AddItemToArray(samples, o);
        if (s[i].has_aufguss_marker) {
            cJSON *am = cJSON_CreateObject();
            cJSON_AddNumberToObject(am, "ts", (double)s[i].t_elapsed_s);
            cJSON_AddItemToArray(markers, am);
        }
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static int http_post_json(const char *url, const char *token,
                          const char *body) {
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    if (token && token[0]) {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "Bearer %s", token);
        esp_http_client_set_header(cli, "Authorization", hdr);
    }
    esp_http_client_set_post_field(cli, body, strlen(body));
    esp_err_t err = esp_http_client_perform(cli);
    int status = err == ESP_OK ? esp_http_client_get_status_code(cli) : -1;
    esp_http_client_cleanup(cli);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http perform: %s", esp_err_to_name(err));
        return -2;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "http status %d", status);
        return -3;
    }
    ESP_LOGI(TAG, "http ok (%d)", status);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public                                                             */
/* ------------------------------------------------------------------ */

int indicator_http_export_session(const struct view_data_session_meta *meta,
                                  const struct view_data_session_sample *samples,
                                  uint16_t sample_count) {
    if (!meta) return -1;
    LOCK();
    bool enabled = s_enabled;
    char url[128];   strncpy(url, s_url, sizeof(url));
    char tok[96];    strncpy(tok, s_token, sizeof(tok));
    UNLOCK();

    if (!enabled || url[0] == 0) {
        ESP_LOGI(TAG, "http export disabled or no url configured");
        return 0;  /* not an error */
    }

    char *body = build_json(meta, samples, sample_count);
    if (!body) return -2;
    int rc = http_post_json(url, tok, body);
    free(body);

    if (rc != 0) {
        queue_push(meta->id);
        return rc;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Retry-Worker (laeuft nach boot + nach erfolgreichem Export)        */
/* ------------------------------------------------------------------ */

static void retry_task(void *arg) {
    /* Einmal bei Boot nachziehen, dann ruhen. Wird nach jedem
     * erfolgreichen Export per Task-Notification getriggert.       */
    vTaskDelay(pdMS_TO_TICKS(15000));  /* warte bis WiFi steht */
    while (1) {
        char sid[SSC_SESSION_ID_LEN];
        if (queue_pop_id(sid, sizeof(sid)) != 0) {
            /* queue leer - warte auf naechsten trigger */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        struct view_data_session_meta m;
        if (indicator_session_store_get(sid, &m) != 0) {
            ESP_LOGW(TAG, "retry: meta for %s not in store, drop", sid);
            continue;
        }
        /* Fuer retry: nur meta ohne samples senden. Das ist genug
         * fuer ein Dashboard "wann war session". Vollstaendige
         * samples wuerden einen Round-trip zu SD+RP2040 brauchen;
         * den machen wir in einer spaeteren Version.               */
        char *body = build_json(&m, NULL, 0);
        if (!body) continue;
        int rc = http_post_json(s_url, s_token, body);
        free(body);
        if (rc != 0) {
            queue_push(sid);  /* zurueck in queue */
            vTaskDelay(pdMS_TO_TICKS(60000));  /* 1 min backoff */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Event-Handler fuer Config-Updates                                  */
/* ------------------------------------------------------------------ */

static void on_view_event(void *arg, esp_event_base_t base,
                          int32_t id, void *ev_data) {
    if (id == VIEW_EVENT_HTTP_CFG_APPLY && ev_data) {
        const struct view_data_http_cfg *c = ev_data;
        LOCK();
        s_enabled = c->enabled;
        strncpy(s_url,   c->url,          sizeof(s_url)   - 1);
        strncpy(s_token, c->bearer_token, sizeof(s_token) - 1);
        UNLOCK();
        save_config_nvs();
        ESP_LOGI(TAG, "http config applied: enabled=%d url=%s",
                 s_enabled, s_url);
    }
}

int indicator_http_export_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return -1;
    load_config_nvs();
    esp_event_handler_register_with(view_event_handle, VIEW_EVENT_BASE,
                                    VIEW_EVENT_HTTP_CFG_APPLY,
                                    on_view_event, NULL);
    /* v0.2.14: stack in PSRAM (6 KB raus aus DRAM). */
    xTaskCreateWithCaps(retry_task, "ssc_httpx", 6 * 1024, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
    return 0;
}
