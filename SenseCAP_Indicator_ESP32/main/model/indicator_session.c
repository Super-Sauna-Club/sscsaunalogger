/*
 * indicator_session.c - Sauna-Session State Machine & Live-Recording
 *
 * Zentrale Logik fuer den Super-Sauna-Club-Logger. Haelt die aktuelle
 * Session im RAM (Samples als Ringpuffer, Aufguss-Liste, Peaks) und
 * orchestriert:
 *   - ESP32 -> RP2040:  Session-Start/Aufguss-Marker/End-Kommandos ueber
 *                       die bereits existierende COBS/UART-Pipe.
 *   - Sensor-Events:    Abonniert VIEW_EVENT_SENSOR_DATA fuer die
 *                       SHT85-Werte (SAUNA_TEMP / SAUNA_RH) und fuettert
 *                       daraus den Live-Chart.
 *   - UI-Events:        Antwortet auf SESSION_START/AUFGUSS/END_REQUEST/
 *                       SAVE/DISCARD, HISTORY_LIST_REQ, HISTORY_DETAIL_REQ.
 *   - Persistenz:       Delegiert Session-Metadaten an indicator_session_store,
 *                       HTTP-Export an indicator_http_export,
 *                       MariaDB-Push an indicator_mariadb.
 *
 * Der RP2040 schreibt die kompletten Samples separat auf SD. Das ESP
 * haelt nur die aktuelle Session + Metadaten-Historie in NVS.
 */

#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps (IDF v5.1+) */
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"    /* esp_timer_get_time() fuer Readback-Instrumentation */

#include "view_data.h"
#include "indicator_session.h"
#include "indicator_sensor.h"
#include "indicator_session_store.h"
#include "indicator_mariadb.h"
#include "indicator_http_export.h"

static const char *TAG = "SESSION";

/* v0.3.0: forward-decls fuer hybrid-storage helpers, damit on_session_save
 * und SESSION_EDIT-handler sie weiter oben im file rufen koennen.       */
static void meta_pack_to_wire(const struct view_data_session_meta *src,
                              ssc_meta_wire_t *dst);
static void meta_unpack_from_wire(const ssc_meta_wire_t *src,
                                  struct view_data_session_meta *dst);
static void on_sd_meta_resp(const ssc_meta_wire_t *w);
static void on_sd_meta_done(uint16_t count);

/* ======================================================================= */
/* Konfiguration                                                            */
/* ======================================================================= */

/* Maximale Session-Dauer im RAM: 60 min a 1 Hz. Reicht fuer 99 % aller
 * Saunagaenge; laengere Sessions ueberschreiben den Ringbuffer ab dem
 * Ende (SD behaelt den vollstaendigen Verlauf).                        */
/* v0.3.1: 7200. 7200 × 12 byte = 86 KB in PSRAM (heap_caps_malloc
 * unten). Bei 2Hz reicht das 1h, bei 1Hz 2h.                        */
#define SSC_LIVE_SAMPLES_MAX  7200

/* Maximal die letzten N Chunks pro History-Detail-Request weiterleiten. */
#define SSC_READBACK_CHUNK_TIMEOUT_MS 5000

/* Worker-Task-Parameter.
 *
 * Stack 8 KB reicht jetzt, weil MariaDB/HTTP-calls aus dem worker raus
 * sind; er macht nur noch time()/push_sample/post_live_event/ESP_LOGI.
 *
 * WICHTIG: Stack wird via xTaskCreateWithCaps in PSRAM alloziert, weil
 * das interne DRAM nach LVGL+WiFi+Sensor-init so fragmentiert ist dass
 * 12 KB dort nicht mehr raus fallen - xTaskCreate scheitert dann still
 * (wir hatten "worker task create failed" im log, kein tick, kein LIVE
 * event, UI-timer blieb auf 00:00).                                   */
#define SSC_TASK_STACK   (8 * 1024)
#define SSC_TASK_PRIO    5

extern esp_event_loop_handle_t view_event_handle;

/* ======================================================================= */
/* Interner Zustand                                                         */
/* ======================================================================= */

enum session_state {
    SESSION_IDLE = 0,
    SESSION_LIVE,
    SESSION_SUMMARY_PENDING,   /* Session beendet, aber noch nicht gespeichert */
};

typedef struct {
    uint32_t t_elapsed_s;
    float    temp;
    float    rh;
} live_sample_t;

typedef struct {
    enum session_state state;

    /* Identitaet */
    char     id[SSC_SESSION_ID_LEN];
    time_t   start_ts;
    time_t   end_ts;
    char     operator_tag[SSC_OPERATOR_MAXLEN];

    /* Live-Sample-Ringbuffer (aus PSRAM) */
    live_sample_t *samples;
    uint16_t samples_cap;
    uint16_t samples_count;
    uint16_t samples_head;   /* schreibt vorwaerts, wrappt */

    /* Aufguss-Liste */
    struct view_data_aufguss aufguesse[SSC_AUFGUSS_MAX_PER_SESS];
    uint8_t  aufguss_count;

    /* Peaks */
    float    peak_temp;
    float    peak_rh;

    /* Letzter gueltiger Sensorwert (wird vom LIVE-Tick gelesen) */
    float    last_temp;
    float    last_rh;
    bool     have_temp;
    bool     have_rh;

    /* Live-MariaDB-Push: wieviele Samples schon gepusht wurden */
    uint16_t mdb_pushed_count;
    uint32_t mdb_last_push_ms;
} session_ctx_t;

static session_ctx_t        s_ctx;
static SemaphoreHandle_t    s_mutex;
static TaskHandle_t         s_worker;
static bool                 s_inited = false;

/* Fuer History-Detail-Requests: aktuelle req_id + session-id */
static uint16_t s_readback_req_id   = 0;
static char     s_readback_sid[SSC_SESSION_ID_LEN];
static uint32_t s_readback_next_off = 0;
static bool     s_readback_active   = false;

/* ======================================================================= */
/* Helpers                                                                  */
/* ======================================================================= */

#define LOCK()    xSemaphoreTake(s_mutex, portMAX_DELAY)
#define UNLOCK()  xSemaphoreGive(s_mutex)

static void make_session_id(char *out, size_t n) {
    time_t now = 0;
    time(&now);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(out, n, "S%04d%02d%02d_%02d%02d%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static void reset_ctx_live(void) {
    s_ctx.samples_count = 0;
    s_ctx.samples_head = 0;
    s_ctx.aufguss_count = 0;
    s_ctx.peak_temp = -INFINITY;
    s_ctx.peak_rh   = -INFINITY;
    s_ctx.have_temp = s_ctx.have_rh = false;
    s_ctx.last_temp = s_ctx.last_rh = NAN;
    s_ctx.start_ts = s_ctx.end_ts = 0;
    s_ctx.mdb_pushed_count = 0;
    s_ctx.mdb_last_push_ms = 0;
    memset(s_ctx.id, 0, sizeof(s_ctx.id));
    memset(s_ctx.operator_tag, 0, sizeof(s_ctx.operator_tag));
}

/* Baut die snapshot-struktur. MUSS unter LOCK() aufgerufen werden
 * (liest s_ctx-felder). Posten selber dann OHNE lock.              */
static void build_live_snapshot(uint32_t t_elapsed_s,
                                struct view_data_session_live *out) {
    out->t_elapsed_s  = t_elapsed_s;
    out->temp         = s_ctx.have_temp ? s_ctx.last_temp : NAN;
    out->rh           = s_ctx.have_rh   ? s_ctx.last_rh   : NAN;
    out->peak_temp    = (s_ctx.peak_temp == -INFINITY) ? NAN : s_ctx.peak_temp;
    out->peak_rh      = (s_ctx.peak_rh   == -INFINITY) ? NAN : s_ctx.peak_rh;
    out->aufguss_count = s_ctx.aufguss_count;
}

/* Posten OHNE lock - esp_event_post_to kann bis 200ms blocken wenn die
 * queue voll ist, und wir wollen NICHT den session-mutex dafuer halten.
 * 200ms statt 20ms weil die UI-handler mit lv_port_sem_take laenger
 * brauchen koennen als 1 tick.                                       */
static void post_live_snapshot(const struct view_data_session_live *live) {
    esp_err_t err = esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_SESSION_LIVE, live, sizeof(*live),
                      pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post LIVE dropped: 0x%x (queue full?)", err);
    }
}

/* Push eines Samples in den Ringpuffer. */
static void push_sample(uint32_t t_elapsed_s, float t, float rh) {
    if (!s_ctx.samples || s_ctx.samples_cap == 0) return;
    s_ctx.samples[s_ctx.samples_head].t_elapsed_s = t_elapsed_s;
    s_ctx.samples[s_ctx.samples_head].temp        = t;
    s_ctx.samples[s_ctx.samples_head].rh          = rh;
    s_ctx.samples_head = (s_ctx.samples_head + 1) % s_ctx.samples_cap;
    if (s_ctx.samples_count < s_ctx.samples_cap) s_ctx.samples_count++;
}

/* Samples chronologisch in ein lineares Array kopieren (fuer Export). */
static uint16_t samples_linearize(struct view_data_session_sample *out,
                                  uint16_t cap) {
    uint16_t n = s_ctx.samples_count < cap ? s_ctx.samples_count : cap;
    if (n == 0) return 0;
    uint16_t head  = s_ctx.samples_head;
    uint16_t start = (s_ctx.samples_count < s_ctx.samples_cap)
                     ? 0 : head;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t src = (start + i) % s_ctx.samples_cap;
        out[i].t_elapsed_s        = s_ctx.samples[src].t_elapsed_s;
        out[i].temp               = s_ctx.samples[src].temp;
        out[i].rh                 = s_ctx.samples[src].rh;
        out[i].has_aufguss_marker = 0;
    }
    /* Aufguss-Marker einstempeln */
    for (uint8_t a = 0; a < s_ctx.aufguss_count; a++) {
        uint32_t ats = s_ctx.aufguesse[a].ts_s;
        /* binary search in out[0..n) nach t_elapsed_s nahe ats */
        uint16_t lo = 0, hi = n;
        while (lo < hi) {
            uint16_t mid = (lo + hi) / 2;
            if (out[mid].t_elapsed_s < ats) lo = mid + 1;
            else                             hi = mid;
        }
        if (lo < n) out[lo].has_aufguss_marker = 1;
    }
    return n;
}

/* ======================================================================= */
/* Kommandos Richtung RP2040                                                */
/* ======================================================================= */

/* Wir halten uns an den in indicator_sensor.c definierten
 * SSC_CMD_SESSION_*-Kommandosatz. Payload-Encoder hier.  */
#define SSC_CMD_SESSION_START    0xA4
/* v0.3.0: hybrid-storage cmd-codes - matched in indicator_sensor.c. */
#define SSC_CMD_LIST_SESSIONS         0xA9
#define SSC_CMD_SESSION_META_PUSH     0xAA
#define SSC_CMD_SESSION_DELETE_FILE   0xAB
#define SSC_CMD_SESSION_DELETE_JSON   0xAC
#define SSC_CMD_SESSION_AUFGUSS  0xA5
#define SSC_CMD_SESSION_END      0xA6
#define SSC_CMD_SD_READBACK      0xA7

static void rp_send_session_start(const char *id) {
    uint8_t payload[SSC_SESSION_ID_LEN] = {0};
    strncpy((char *)payload, id, SSC_SESSION_ID_LEN - 1);
    int rc = indicator_sensor_rp2040_cmd(SSC_CMD_SESSION_START, payload, sizeof(payload));
    ESP_LOGI(TAG, "rp2040 -> SESSION_START id=%s (uart_bytes=%d)", id, rc);
}

static void rp_send_aufguss(const char *name) {
    uint8_t payload[SSC_AUFGUSS_NAME_MAXLEN] = {0};
    strncpy((char *)payload, name, SSC_AUFGUSS_NAME_MAXLEN - 1);
    int rc = indicator_sensor_rp2040_cmd(SSC_CMD_SESSION_AUFGUSS, payload, sizeof(payload));
    ESP_LOGI(TAG, "rp2040 -> AUFGUSS name='%s' (uart_bytes=%d)", name, rc);
}

static void rp_send_session_end(void) {
    int rc = indicator_sensor_rp2040_cmd(SSC_CMD_SESSION_END, NULL, 0);
    ESP_LOGI(TAG, "rp2040 -> SESSION_END (uart_bytes=%d)", rc);
}

/* RBK-INSTRUMENTATION: Start-Zeit + letzter Request, genutzt vom rx-chunk-
 * Handler um dt zu messen. Wird vom Readback-Flow bewirtschaftet (Start
 * in on_history_detail_req, Update in rp_request_sd_readback, Auswertung
 * in indicator_session_rx_sd_chunk). */
static uint64_t s_rbk_t_start_us  = 0;
static uint64_t s_rbk_t_last_req_us = 0;
static uint32_t s_rbk_chunks_rx   = 0;
static uint32_t s_rbk_bytes_rx    = 0;

static void rp_request_sd_readback(const char *id, uint32_t offset, uint16_t maxlen) {
    uint8_t payload[2 + SSC_SESSION_ID_LEN + 4 + 2] = {0};
    s_readback_req_id++;
    payload[0] = (uint8_t)(s_readback_req_id >> 8);
    payload[1] = (uint8_t)(s_readback_req_id & 0xFF);
    strncpy((char *)&payload[2], id, SSC_SESSION_ID_LEN - 1);
    payload[2 + SSC_SESSION_ID_LEN + 0] = (uint8_t)(offset >> 24);
    payload[2 + SSC_SESSION_ID_LEN + 1] = (uint8_t)(offset >> 16);
    payload[2 + SSC_SESSION_ID_LEN + 2] = (uint8_t)(offset >> 8);
    payload[2 + SSC_SESSION_ID_LEN + 3] = (uint8_t)(offset);
    payload[2 + SSC_SESSION_ID_LEN + 4] = (uint8_t)(maxlen >> 8);
    payload[2 + SSC_SESSION_ID_LEN + 5] = (uint8_t)(maxlen & 0xFF);
    s_rbk_t_last_req_us = esp_timer_get_time();
    uint32_t t_ms = (uint32_t)((s_rbk_t_last_req_us - s_rbk_t_start_us) / 1000);
    int rc = indicator_sensor_rp2040_cmd(SSC_CMD_SD_READBACK, payload, sizeof(payload));
    ESP_LOGW(TAG, "RBK REQ off=%u len=%u t=%ums (uart_bytes=%d)",
             (unsigned)offset, (unsigned)maxlen, (unsigned)t_ms, rc);
}

/* ======================================================================= */
/* Public API                                                                */
/* ======================================================================= */

bool indicator_session_is_active(void) {
    if (!s_inited) return false;
    LOCK();
    bool active = (s_ctx.state == SESSION_LIVE);
    UNLOCK();
    return active;
}

/* ======================================================================= */
/* Event-Handler: Sensor-Daten (SAUNA_TEMP/RH)                              */
/* ======================================================================= */

static void on_sensor_data(const struct view_data_sensor_data *ev) {
    if (!ev) return;
    LOCK();
    if (ev->sensor_type == SENSOR_DATA_SAUNA_TEMP) {
        s_ctx.last_temp = ev->vaule;
        s_ctx.have_temp = true;
        if (ev->vaule > s_ctx.peak_temp) s_ctx.peak_temp = ev->vaule;
    } else if (ev->sensor_type == SENSOR_DATA_SAUNA_RH) {
        s_ctx.last_rh = ev->vaule;
        s_ctx.have_rh = true;
        if (ev->vaule > s_ctx.peak_rh) s_ctx.peak_rh = ev->vaule;
    }
    UNLOCK();
}

/* ======================================================================= */
/* Event-Handler: UI / View                                                  */
/* ======================================================================= */

static void on_session_start(const char *operator_tag_in) {
    ESP_LOGI(TAG, "on_session_start ENTER op='%s'",
             operator_tag_in ? operator_tag_in : "");
    LOCK();
    if (s_ctx.state != SESSION_IDLE) {
        ESP_LOGW(TAG, "session_start ignoriert: state=%d (nicht IDLE). "
                 "Auto-reset auf IDLE um neue session zu erlauben",
                 (int)s_ctx.state);
        /* Auto-recovery: falls UI aus vorherigem versuch im non-IDLE-
         * state stecken geblieben ist, resetten wir hier. User klickt
         * "STARTEN" → wir erwarten neue session, nicht verweigerung. */
        reset_ctx_live();
        s_ctx.state = SESSION_IDLE;
    }
    reset_ctx_live();
    make_session_id(s_ctx.id, sizeof(s_ctx.id));
    time(&s_ctx.start_ts);
    if (operator_tag_in && operator_tag_in[0]) {
        strncpy(s_ctx.operator_tag, operator_tag_in, SSC_OPERATOR_MAXLEN - 1);
    }
    s_ctx.state = SESSION_LIVE;
    ESP_LOGI(TAG, "session START id=%s op='%s' start_ts=%ld state->LIVE",
             s_ctx.id, s_ctx.operator_tag, (long)s_ctx.start_ts);

    /* Sofort einen initialen LIVE-snapshot bauen, damit timer und chart
     * nicht 1s auf den worker-tick warten muessen. Posten out-of-lock. */
    struct view_data_session_live snap0 = {0};
    build_live_snapshot(0, &snap0);
    UNLOCK();

    post_live_snapshot(&snap0);

    /* RP2040 informieren: CSV anlegen */
    rp_send_session_start(s_ctx.id);
    ESP_LOGI(TAG, "on_session_start EXIT (rp2040 cmd gesendet)");

    /* KEIN mariadb-call hier - der blockierte den event-loop (DNS +
     * TCP-handshake), wodurch chart/buttons einfroren. MariaDB-sync
     * passiert jetzt nur noch am session-ende als ein shot.         */
}

static void on_session_aufguss(const char *name_in) {
    LOCK();
    if (s_ctx.state != SESSION_LIVE) {
        UNLOCK();
        return;
    }
    if (s_ctx.aufguss_count >= SSC_AUFGUSS_MAX_PER_SESS) {
        ESP_LOGW(TAG, "aufguss-Limit erreicht (%d)", SSC_AUFGUSS_MAX_PER_SESS);
        UNLOCK();
        return;
    }
    uint32_t dt = (uint32_t)(time(NULL) - s_ctx.start_ts);
    struct view_data_aufguss *a = &s_ctx.aufguesse[s_ctx.aufguss_count++];
    a->ts_s = dt;
    memset(a->name, 0, sizeof(a->name));
    if (name_in) strncpy(a->name, name_in, SSC_AUFGUSS_NAME_MAXLEN - 1);
    ESP_LOGI(TAG, "aufguss #%d: %s (+%us)", s_ctx.aufguss_count, a->name, dt);
    /* Sample mit marker ablegen, damit die Kurve den peak sauber zeigt */
    push_sample(dt,
                s_ctx.have_temp ? s_ctx.last_temp : NAN,
                s_ctx.have_rh   ? s_ctx.last_rh   : NAN);
    UNLOCK();

    rp_send_aufguss(name_in ? name_in : "");
}

static void on_session_end_request(void) {
    LOCK();
    if (s_ctx.state != SESSION_LIVE) { UNLOCK(); return; }
    time(&s_ctx.end_ts);
    s_ctx.state = SESSION_SUMMARY_PENDING;
    ESP_LOGI(TAG, "session END pending (duration=%lds, aufgusse=%d, peak=%.1fC/%.1f%%)",
             (long)(s_ctx.end_ts - s_ctx.start_ts),
             s_ctx.aufguss_count, s_ctx.peak_temp, s_ctx.peak_rh);

    /* Der View baut jetzt den Summary-Screen und laesst den User
     * participant-count + notes + aufguss-headline eingeben. */
    struct view_data_session_meta preview = {0};
    strncpy(preview.id, s_ctx.id, SSC_SESSION_ID_LEN - 1);
    preview.start_ts = s_ctx.start_ts;
    preview.end_ts   = s_ctx.end_ts;
    strncpy(preview.operator_tag, s_ctx.operator_tag, SSC_OPERATOR_MAXLEN - 1);
    if (s_ctx.aufguss_count > 0) {
        strncpy(preview.aufguss_headline,
                s_ctx.aufguesse[0].name, SSC_AUFGUSS_NAME_MAXLEN - 1);
    }
    preview.peak_temp     = (s_ctx.peak_temp == -INFINITY) ? NAN : s_ctx.peak_temp;
    preview.peak_rh       = (s_ctx.peak_rh   == -INFINITY) ? NAN : s_ctx.peak_rh;
    preview.aufguss_count = s_ctx.aufguss_count;
    UNLOCK();

    /* SD-File schliessen */
    rp_send_session_end();

    /* Preview an die View schicken, damit sie den Summary-Screen baut.
     * WICHTIG: Wir nutzen hier VIEW_EVENT_SESSION_SUMMARY_READY (nicht
     * SESSION_SAVE), damit der UI-Handler das Event abfaengt und die
     * Save-Kette nicht versehentlich auslöst. SESSION_SAVE ist
     * strikt UI->session gerichtet.                                   */
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_SESSION_SUMMARY_READY, &preview,
                      sizeof(preview), pdMS_TO_TICKS(100));
}

static void on_session_save(const struct view_data_session_meta *m_in) {
    if (!m_in) return;
    LOCK();
    if (s_ctx.state != SESSION_SUMMARY_PENDING) { UNLOCK(); return; }

    /* finale Metadaten aus dem Summary-Formular + unseren in-RAM-Werten */
    struct view_data_session_meta meta = *m_in;
    strncpy(meta.id, s_ctx.id, SSC_SESSION_ID_LEN - 1);
    meta.start_ts = s_ctx.start_ts;
    meta.end_ts   = s_ctx.end_ts;
    strncpy(meta.operator_tag,
            m_in->operator_tag[0] ? m_in->operator_tag : s_ctx.operator_tag,
            SSC_OPERATOR_MAXLEN - 1);
    meta.peak_temp     = (s_ctx.peak_temp == -INFINITY) ? NAN : s_ctx.peak_temp;
    meta.peak_rh       = (s_ctx.peak_rh   == -INFINITY) ? NAN : s_ctx.peak_rh;
    meta.aufguss_count = s_ctx.aufguss_count;

    /* Samples linearisieren fuer Export */
    uint16_t n = s_ctx.samples_count;
    struct view_data_session_sample *lin = NULL;
    if (n > 0) {
        lin = heap_caps_malloc(n * sizeof(*lin), MALLOC_CAP_SPIRAM);
        if (lin) {
            n = samples_linearize(lin, n);
        } else {
            ESP_LOGE(TAG, "linearize alloc failed, exporting without samples");
            n = 0;
        }
    }
    UNLOCK();

    /* 1) NVS-Index updaten (kompakter Metadaten-Record). */
    int rs = indicator_session_store_append(&meta);
    if (rs != 0) {
        ESP_LOGE(TAG, "session_store_append failed: %d - session %s "
                 "ist NICHT in der history (samples liegen noch auf SD)",
                 rs, meta.id);
    } else {
        ESP_LOGI(TAG, "session %s in NVS gespeichert", meta.id);
    }

    /* v0.3.0: Hybrid-storage push - auch JSON-sidecar auf SD schreiben
     * (RP2040-managed). Damit ist SD die authoritative source und
     * NVS-loss kann via SD-list-rebuild aufgeholt werden.              */
    {
        ssc_meta_wire_t w;
        meta_pack_to_wire(&meta, &w);
        int rp = indicator_sensor_rp2040_cmd(SSC_CMD_SESSION_META_PUSH,
                                             &w, sizeof(w));
        if (rp < 0) {
            ESP_LOGW(TAG, "META_PUSH to RP2040 failed rc=%d (NVS still ok)",
                     rp);
        } else {
            ESP_LOGI(TAG, "META_PUSH to RP2040 ok (%d byte sent)", rp);
        }
    }

    /* 2)+3) Mariadb-finalize + HTTP-export aus dem save-pfad entfernt.
     * Die synchronen Network-Calls blockierten den Event-Task und
     * fuehrten bei Stack-Overflow zum Crash. Session-Save ist jetzt
     * ein reiner NVS-Schreibvorgang - sofort fertig, kein Hang.     */
    (void)lin; (void)n;

    if (lin) free(lin);

    LOCK();
    reset_ctx_live();
    s_ctx.state = SESSION_IDLE;
    UNLOCK();
    ESP_LOGI(TAG, "session-save flow done, back to IDLE (nvs_ok=%d)", rs == 0);
}

/* v0.3.0: pack ESP32-internal meta-record into UART wire-format. Klemmt
 * die strings auf das wire-format-limit. Empfangsseitig (RP2040) kommt
 * der gleiche packed struct an.                                        */
static void meta_pack_to_wire(const struct view_data_session_meta *src,
                              ssc_meta_wire_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->id, src->id, sizeof(dst->id) - 1);
    dst->start_ts = (int32_t)src->start_ts;
    dst->end_ts   = (int32_t)src->end_ts;
    strncpy(dst->operator_tag, src->operator_tag, sizeof(dst->operator_tag) - 1);
    strncpy(dst->aufguss_headline, src->aufguss_headline,
            sizeof(dst->aufguss_headline) - 1);
    dst->participants = src->participants;
    strncpy(dst->notes, src->notes, sizeof(dst->notes) - 1);
    dst->peak_temp     = src->peak_temp;
    dst->peak_rh       = src->peak_rh;
    dst->aufguss_count = src->aufguss_count;
}

/* Inverse: wire-format -> internal record. */
static void meta_unpack_from_wire(const ssc_meta_wire_t *src,
                                  struct view_data_session_meta *dst)
{
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->id, src->id, SSC_SESSION_ID_LEN - 1);
    dst->start_ts = (time_t)src->start_ts;
    dst->end_ts   = (time_t)src->end_ts;
    strncpy(dst->operator_tag, src->operator_tag, SSC_OPERATOR_MAXLEN - 1);
    strncpy(dst->aufguss_headline, src->aufguss_headline,
            SSC_AUFGUSS_NAME_MAXLEN - 1);
    dst->participants = src->participants;
    strncpy(dst->notes, src->notes, SSC_NOTES_MAXLEN - 1);
    dst->peak_temp     = src->peak_temp;
    dst->peak_rh       = src->peak_rh;
    dst->aufguss_count = src->aufguss_count;
}

/* v0.3.0: ein einzelner SD->ESP META_RESP, sofort in NVS einsortieren
 * falls noch nicht vorhanden. Update-pfad nicht implementiert - wenn
 * NVS schon ein record fuer die id hat, gewinnt der NVS-record (SD-
 * recovery bleibt non-destructive).                                   */
static void on_sd_meta_resp(const ssc_meta_wire_t *w)
{
    if (!w || w->id[0] == 0) return;
    struct view_data_session_meta existing = {0};
    int rc = indicator_session_store_get(w->id, &existing);
    if (rc == 0) {
        ESP_LOGD(TAG, "SD_META_RESP id=%s already in NVS - skip", w->id);
        return;
    }
    struct view_data_session_meta meta;
    meta_unpack_from_wire(w, &meta);
    int rs = indicator_session_store_append(&meta);
    ESP_LOGI(TAG, "SD_META_RESP recovered id=%s rc=%d (peak %.1fC %.1f%%)",
             meta.id, rs, meta.peak_temp, meta.peak_rh);
}

/* v0.3.0+: legacy-fix mapping. Hard-coded fuer die 6 sessions die vor
 * dem TZ-fix recovered wurden. start_ts kommt automatisch korrekt aus
 * der re-synthesize beim resync (TZ-fix ist seit v0.3.0 in RP2040).
 * Hier nur die operator/aufguss-zuordnung anwenden.                    */
struct legacy_fix_entry {
    char id[SSC_SESSION_ID_LEN];
    char operator_tag[SSC_OPERATOR_MAXLEN];
    char aufguss_headline[SSC_AUFGUSS_NAME_MAXLEN];
};
static const struct legacy_fix_entry s_legacy_fix[] = {
    {"S20260427_164658", "Kevin",       ""},
    {"S20260427_180047", "Bernhard G.", ""},
    {"S20260427_191623", "Bernhard V.", ""},
    {"S20260427_201327", "Thomas",      ""},
    {"S20260504_170646", "",            ""},
    {"S20260504_191051", "Bernhard V.", "Partysauna"},
};
static bool s_legacy_fix_pending = false;

static void on_sd_meta_done(uint16_t count)
{
    ESP_LOGI(TAG, "SD_META_DONE: %u sessions enumerated from SD - "
             "history-list refresh queued", (unsigned)count);

    /* v0.3.0+: legacy-fix-flow phase 2. Nach dem resync ist NVS jetzt
     * mit korrekten zeiten gefuellt (synthesize_meta_from_csv im RP2040
     * mit TZ-fix). Pro mapping-eintrag: operator+aufguss patchen und
     * neue JSON-sidecar an SD pushen.                                  */
    if (s_legacy_fix_pending) {
        s_legacy_fix_pending = false;
        size_t total = sizeof(s_legacy_fix) / sizeof(s_legacy_fix[0]);
        uint16_t patched = 0;
        for (size_t i = 0; i < total; i++) {
            const struct legacy_fix_entry *f = &s_legacy_fix[i];
            if (f->operator_tag[0] == 0 && f->aufguss_headline[0] == 0) {
                ESP_LOGI(TAG, "fix_legacy[%u] id=%s -> kein patch (leer)",
                         (unsigned)i, f->id);
                continue;
            }
            struct view_data_session_meta m;
            int rc = indicator_session_store_get(f->id, &m);
            if (rc != 0) {
                ESP_LOGW(TAG, "fix_legacy[%u] id=%s nicht in NVS gefunden rc=%d",
                         (unsigned)i, f->id, rc);
                continue;
            }
            strncpy(m.operator_tag, f->operator_tag,
                    SSC_OPERATOR_MAXLEN - 1);
            m.operator_tag[SSC_OPERATOR_MAXLEN - 1] = 0;
            strncpy(m.aufguss_headline, f->aufguss_headline,
                    SSC_AUFGUSS_NAME_MAXLEN - 1);
            m.aufguss_headline[SSC_AUFGUSS_NAME_MAXLEN - 1] = 0;
            int us = indicator_session_store_update(&m);
            ssc_meta_wire_t w;
            meta_pack_to_wire(&m, &w);
            int rp = indicator_sensor_rp2040_cmd(SSC_CMD_SESSION_META_PUSH,
                                                 &w, sizeof(w));
            ESP_LOGI(TAG, "fix_legacy[%u] id=%s op='%s' update=%d push=%d",
                     (unsigned)i, f->id, f->operator_tag, us, rp);
            patched++;
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        ESP_LOGW(TAG, "fix_legacy: %u/%u sessions gepatched",
                 (unsigned)patched, (unsigned)total);
    }

    /* History-liste neu rendern damit UI die wiederhergestellten
     * eintraege sieht.                                              */
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0,
                      pdMS_TO_TICKS(50));
}

/* v0.3.0+: legacy-fix-flow phase 1. NVS leer machen, alle JSONs auf
 * SD weg (damit synthesize_meta_from_csv re-laeuft mit TZ-fix), dann
 * resync triggern. Phase 2 (operator-patch) macht on_sd_meta_done.   */
static void start_legacy_fix(void) {
    ESP_LOGW(TAG, "fix_legacy: starting - wipe NVS + drop SD-JSONs + resync");
    LOCK();
    s_readback_active = false;
    UNLOCK();
    indicator_session_store_wipe();
    size_t total = sizeof(s_legacy_fix) / sizeof(s_legacy_fix[0]);
    for (size_t i = 0; i < total; i++) {
        const struct legacy_fix_entry *f = &s_legacy_fix[i];
        int rp = indicator_sensor_rp2040_cmd(
                    SSC_CMD_SESSION_DELETE_JSON,
                    f->id,
                    (uint16_t)strnlen(f->id, SSC_SESSION_ID_LEN) + 1);
        ESP_LOGI(TAG, "fix_legacy: del-json %s rc=%d", f->id, rp);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    s_legacy_fix_pending = true;
    indicator_session_request_sd_list();
}

static void on_session_discard(void) {
    LOCK();
    if (s_ctx.state == SESSION_LIVE) {
        rp_send_session_end();  /* SD-File schliessen */
    }
    reset_ctx_live();
    s_ctx.state = SESSION_IDLE;
    UNLOCK();
    ESP_LOGI(TAG, "session discarded");
}

static void on_history_list_req(void) {
    /* Bis zu 32 Eintraege in einem Rutsch liefern - das ist die
     * Seitengroesse fuer die UI-History-Liste. Die View scrollt
     * ggf. weiter und fragt dann mit start_index>0 nach.        */
    EXT_RAM_BSS_ATTR static struct view_data_session_meta items[32];
    struct view_data_session_list list = {0};
    list.items = items;
    int rc = indicator_session_store_list(&list, 0, 32);
    if (rc != 0) {
        ESP_LOGE(TAG, "history_list failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "history_list -> UI: count=%u total=%u items=%p",
             (unsigned)list.count, (unsigned)list.total, list.items);
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_HISTORY_LIST, &list, sizeof(list),
                      pdMS_TO_TICKS(100));
}

static void on_history_detail_req(const char *id) {
    if (!id) return;
    ESP_LOGW(TAG, "RBK START id='%s' ----------------", id);
    s_rbk_t_start_us  = esp_timer_get_time();
    s_rbk_chunks_rx   = 0;
    s_rbk_bytes_rx    = 0;
    LOCK();
    strncpy(s_readback_sid, id, SSC_SESSION_ID_LEN - 1);
    s_readback_sid[SSC_SESSION_ID_LEN - 1] = 0;
    s_readback_next_off = 0;
    s_readback_active   = true;
    UNLOCK();
    /* Per-chunk Request-Response mit 64 B chunks. 64 data + 13 header =
     * 77 bytes < UART-FIFO-128 / < parser-safe-100. Memory-belegt: der
     * ESP32-UART-Parser in indicator_sensor.c ist nicht robust gegen
     * packet-fragmentation bei >~100 B. Nach 6 chunks @ 215 bytes zeigte
     * sich im Log dass chunks verlorengehen - daher Rollback auf 64 B. */
    rp_request_sd_readback(s_readback_sid, 0, 64);
}

/* ======================================================================= */
/* SD-Readback Chunk-Handler                                                */
/* ======================================================================= */

/* Seit v0.2.5: Hot-Path-Architektur fuer Readback.
 *   rx_sd_chunk macht NUR memcpy in einen PSRAM-Buffer - kein CSV-Parse
 *   pro Chunk (das hat im ersten Wurf 140 ms pro Chunk gekostet und
 *   Packet-Loss auf UART-Seite verursacht, weil der RP2040 in der
 *   Zwischenzeit weitergestreamt hat).
 *   Erst bei EOF parsen wir den gesamten Buffer einmal und feuern die
 *   VIEW_EVENT_HISTORY_DETAIL_CHUNK-Events. Buffer kommt aus PSRAM
 *   (wir haben 8 MB - 50 KB Session passt locker). */
static uint8_t *s_rbk_buf       = NULL;
static size_t   s_rbk_buf_size  = 0;

static void parse_and_emit_rbk(const uint8_t *buf, size_t size) {
    /* Struct ~2 KB (128 samples) - gehoert NICHT auf den Stack,
     * der UART-comm-Task hat nur 4 KB. static = BSS. */
    static struct view_data_session_samples_chunk chunk;
    memset(&chunk, 0, sizeof(chunk));
    strncpy(chunk.session_id, s_readback_sid, SSC_SESSION_ID_LEN - 1);
    chunk.offset = 0;
    chunk.count  = 0;
    chunk.total  = (uint16_t)((size / 32) & 0xFFFF);

    char linebuf[160];
    size_t lb = 0;
    for (size_t i = 0; i < size; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (lb == 0) continue;
            linebuf[lb] = 0;
            if (strncmp(linebuf, "t_elapsed_s", 11) != 0) {
                unsigned long t_elapsed; float t, rh; char marker[16] = {0};
                int r = sscanf(linebuf, "%lu,%f,%f,%15s", &t_elapsed, &t, &rh, marker);
                if (r >= 3) {
                    struct view_data_session_sample *s = &chunk.samples[chunk.count];
                    s->t_elapsed_s        = (uint32_t)t_elapsed;
                    s->temp               = t;
                    s->rh                 = rh;
                    s->has_aufguss_marker = (r == 4 && marker[0]) ? 1 : 0;
                    chunk.count++;
                    if (chunk.count >= SSC_SAMPLE_CHUNK_MAX) {
                        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                                          VIEW_EVENT_HISTORY_DETAIL_CHUNK,
                                          &chunk, sizeof(chunk), pdMS_TO_TICKS(100));
                        chunk.offset += chunk.count;
                        chunk.count = 0;
                    }
                }
            }
            lb = 0;
        } else if (lb < sizeof(linebuf) - 1) {
            linebuf[lb++] = c;
        }
    }
    if (chunk.count > 0) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_HISTORY_DETAIL_CHUNK,
                          &chunk, sizeof(chunk), pdMS_TO_TICKS(100));
    }
}

void indicator_session_rx_sd_chunk(const uint8_t *payload, size_t n) {
    if (n < 12) return;
    uint16_t req_id = (uint16_t)(payload[0] << 8) | payload[1];
    uint32_t total  = ((uint32_t)payload[2] << 24) | ((uint32_t)payload[3] << 16)
                    | ((uint32_t)payload[4] << 8)  | payload[5];
    uint32_t offset = ((uint32_t)payload[6] << 24) | ((uint32_t)payload[7] << 16)
                    | ((uint32_t)payload[8] << 8)  | payload[9];
    uint16_t dlen   = (uint16_t)(payload[10] << 8) | payload[11];
    const uint8_t *data = payload + 12;
    if (12 + dlen > n) return;

    LOCK();
    bool match = (s_readback_active && req_id == s_readback_req_id);
    UNLOCK();
    if (!match) return;

    /* Erster Chunk: PSRAM-Buffer allokieren. */
    if (offset == 0) {
        if (s_rbk_buf) { heap_caps_free(s_rbk_buf); s_rbk_buf = NULL; s_rbk_buf_size = 0; }
        if (total > 0 && total < 1024u * 1024u) {   /* 1 MB Sanity-Limit */
            s_rbk_buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
            s_rbk_buf_size = total;
        }
        if (!s_rbk_buf) {
            ESP_LOGE(TAG, "RBK PSRAM-alloc %u B fehlgeschlagen", (unsigned)total);
            s_rbk_buf_size = 0;
        }
    }
    s_rbk_chunks_rx++;
    s_rbk_bytes_rx += dlen;

    /* Instrumentation wieder drin - bei jedem Chunk loggen, damit
     * wir die STUCK-Position exakt sehen (REQ vs RX divergiert). */
    uint32_t t_rx_ms = (uint32_t)((esp_timer_get_time() - s_rbk_t_start_us) / 1000);
    ESP_LOGW(TAG, "RBK RX  #%u off=%u/%u dlen=%u t=%ums buf=%p size=%u",
             (unsigned)s_rbk_chunks_rx, (unsigned)offset, (unsigned)total,
             (unsigned)dlen, (unsigned)t_rx_ms,
             (void *)s_rbk_buf, (unsigned)s_rbk_buf_size);

    /* Pure memcpy - der heisseste Pfad hier, NICHTS anderes. */
    if (s_rbk_buf && offset + dlen <= s_rbk_buf_size) {
        memcpy(s_rbk_buf + offset, data, dlen);
    }

    /* Fortschritts-Tracking: watchdog braucht last-known offset um nach
     * Stall vom richtigen Stand weiterzumachen. */
    LOCK();
    s_readback_next_off = offset + dlen;
    UNLOCK();

    /* Per-chunk-mode: direkt naechsten Chunk anfragen wenn noch nicht
     * alles da. Kein Stream, deshalb muss ESP32 das Polling treiben. */
    if (offset + dlen < total) {
        rp_request_sd_readback(s_readback_sid, offset + dlen, 64);
        return;
    }

    if (offset + dlen >= total) {
        uint32_t total_ms = (uint32_t)((esp_timer_get_time() - s_rbk_t_start_us) / 1000);
        ESP_LOGW(TAG, "RBK DONE sid=%s chunks=%u bytes=%u/%u total=%ums",
                 s_readback_sid, (unsigned)s_rbk_chunks_rx,
                 (unsigned)s_rbk_bytes_rx, (unsigned)total, (unsigned)total_ms);

        if (s_rbk_buf && s_rbk_buf_size > 0) {
            parse_and_emit_rbk(s_rbk_buf, s_rbk_buf_size);
            heap_caps_free(s_rbk_buf);
            s_rbk_buf = NULL;
            s_rbk_buf_size = 0;
        }

        struct view_data_session_detail_done done = {0};
        strncpy(done.session_id, s_readback_sid, SSC_SESSION_ID_LEN - 1);
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_HISTORY_DETAIL_DONE,
                          &done, sizeof(done), pdMS_TO_TICKS(100));
        LOCK();
        s_readback_active = false;
        UNLOCK();
    }
}

/* ======================================================================= */
/* Worker-Task: 1 Hz Live-Event + Sample-Push                               */
/* ======================================================================= */

static void session_worker(void *arg) {
    TickType_t tick_period = pdMS_TO_TICKS(1000);
    TickType_t last = xTaskGetTickCount();
    uint32_t heartbeat_cnt = 0;

    ESP_LOGI(TAG, "session_worker task started, period=1000ms");

    while (1) {
        vTaskDelayUntil(&last, tick_period);
        heartbeat_cnt++;

        struct view_data_session_live snap = {0};
        bool have_snap = false;
        uint32_t dt = 0;
        enum session_state st_now;
        float t = NAN, rh = NAN;

        LOCK();
        st_now = s_ctx.state;
        if (st_now == SESSION_LIVE) {
            dt = (uint32_t)(time(NULL) - s_ctx.start_ts);
            t  = s_ctx.have_temp ? s_ctx.last_temp : NAN;
            rh = s_ctx.have_rh   ? s_ctx.last_rh   : NAN;
            push_sample(dt, t, rh);
            build_live_snapshot(dt, &snap);
            have_snap = true;
        }
        UNLOCK();

        /* Poste LIVE-event AUSSER-mutex, damit das event-post-timeout
         * keinen anderen lock-nehmer blockiert.                      */
        if (have_snap) {
            post_live_snapshot(&snap);
        }

        /* Heartbeat-log: ersten 5 ticks jeden tick (hilft beim boot-
         * debugging), danach alle 10 s.                               */
        bool do_log = (heartbeat_cnt <= 5) || ((heartbeat_cnt % 10) == 0);
        if (do_log) {
            ESP_LOGI(TAG, "worker hb=%lu state=%d dt=%lu temp=%.1f rh=%.1f"
                     " have_snap=%d",
                     (unsigned long)heartbeat_cnt, (int)st_now,
                     (unsigned long)dt, t, rh, have_snap ? 1 : 0);
        }

        /* Readback-Watchdog: wenn ein Readback aktiv ist und nach 3 s
         * noch keine einzige Chunk-Response eingetrudelt ist (RP2040
         * stuck / PacketSerial-Overflow), den Request nochmal senden.
         * Maximal 3 Retries, danach aufgeben und DONE-Event posten
         * damit UI nicht ewig "Lade Daten..." zeigt. */
        LOCK();
        bool rbk_active = s_readback_active;
        uint32_t chunks_rx_now = s_rbk_chunks_rx;
        char sid_copy[SSC_SESSION_ID_LEN];
        memcpy(sid_copy, s_readback_sid, SSC_SESSION_ID_LEN);
        UNLOCK();

        if (rbk_active) {
            static uint32_t last_chunks = 0;
            static uint32_t stall_ticks = 0;
            static uint8_t  retry_count = 0;
            static uint32_t last_rbk_start_us = 0;

            uint64_t now_us = esp_timer_get_time();
            if (last_rbk_start_us != s_rbk_t_start_us) {
                last_rbk_start_us = s_rbk_t_start_us;
                last_chunks = 0;
                stall_ticks = 0;
                retry_count = 0;
            }

            /* ABSOLUTES Zeit-Limit: 60 s. v0.2.11: 5 s war fuer lange
             * sessions viel zu knapp - bei 64-byte UART-chunks und
             * request-response-protokoll braucht eine 30-min-session
             * realistisch 30-60 s zum vollstaendigen einlesen. Das alte
             * limit hat regelmaessig "halbe session" beim detail-view
             * ausgeloest. Der stall-watchdog (3 s ohne progress, max 10
             * retries) faengt echte haenger ohnehin viel frueher ab -
             * dieses 60-s-limit ist nur safety-net fuer extrem-faelle. */
            uint32_t total_ms = (uint32_t)((now_us - s_rbk_t_start_us) / 1000);
            bool time_up = total_ms > 60000;

            if (chunks_rx_now == last_chunks) {
                stall_ticks++;
            } else {
                stall_ticks = 0;
                last_chunks = chunks_rx_now;
                /* retry_count NICHT resetten - sonst gilt das 10er-Limit nicht */
            }

            /* v0.3.1 (2026-05-09): stall-tick 3->1 + retries 10->30. Vorher
             * 30 sekunden warten bei chunk-loss, jetzt 1 sek/retry × 30
             * versuche = max ~30 s (gleicher safety-cap), aber bei
             * intermittentem loss kommt der erste retry nach 1 s statt 3 s
             * und die wahrscheinlichkeit dass alle versuche fehlschlagen
             * sinkt drastisch. Symptom-fix fuer "session laedt nur partial". */
            if (stall_ticks >= 1 || time_up) {
                stall_ticks = 0;
                if (!time_up && retry_count < 30) {
                    retry_count++;
                    LOCK();
                    uint32_t retry_off = s_readback_next_off;
                    UNLOCK();
                    ESP_LOGW(TAG, "RBK WATCHDOG: stall, retry %u/30 resume off=%u",
                             retry_count, (unsigned)retry_off);
                    rp_request_sd_readback(sid_copy, retry_off, 64);
                } else {
                    if (time_up) {
                        ESP_LOGE(TAG, "RBK WATCHDOG: 60s Zeit-Limit erreicht, partial DONE (%u chunks)",
                                 (unsigned)chunks_rx_now);
                    } else {
                        ESP_LOGE(TAG, "RBK WATCHDOG: 30 retries erschoepft, partial DONE");
                    }
                    /* Partial-Parse: was bis hierhin im Buffer ist, der UI
                     * liefern - sonst bleibt "Lade Daten..." unnoetig. */
                    if (s_rbk_buf && s_rbk_buf_size > 0 && s_rbk_bytes_rx > 0) {
                        parse_and_emit_rbk(s_rbk_buf, s_rbk_bytes_rx);
                    }
                    if (s_rbk_buf) {
                        heap_caps_free(s_rbk_buf);
                        s_rbk_buf = NULL;
                        s_rbk_buf_size = 0;
                    }
                    struct view_data_session_detail_done done = {0};
                    strncpy(done.session_id, sid_copy, SSC_SESSION_ID_LEN - 1);
                    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                                      VIEW_EVENT_HISTORY_DETAIL_DONE,
                                      &done, sizeof(done), pdMS_TO_TICKS(100));
                    LOCK();
                    s_readback_active = false;
                    UNLOCK();
                    retry_count = 0;
                }
            }
        }
    }
}

/* ======================================================================= */
/* Event-Dispatcher (vom esp_event-Loop aufgerufen)                          */
/* ======================================================================= */

static void on_view_event(void *arg, esp_event_base_t base,
                          int32_t id, void *ev_data) {
    switch (id) {
    case VIEW_EVENT_SENSOR_DATA:
        on_sensor_data((const struct view_data_sensor_data *)ev_data);
        break;
    case VIEW_EVENT_SESSION_START:
        on_session_start(ev_data ? (const char *)ev_data : "");
        break;
    case VIEW_EVENT_SESSION_AUFGUSS:
        on_session_aufguss(ev_data ? (const char *)ev_data : "");
        break;
    case VIEW_EVENT_SESSION_END_REQUEST:
        on_session_end_request();
        break;
    case VIEW_EVENT_SESSION_SAVE:
        on_session_save((const struct view_data_session_meta *)ev_data);
        break;
    case VIEW_EVENT_SESSION_DISCARD:
        on_session_discard();
        break;
    case VIEW_EVENT_HISTORY_LIST_REQ:
        on_history_list_req();
        break;
    case VIEW_EVENT_HISTORY_DETAIL_REQ:
        on_history_detail_req(ev_data ? (const char *)ev_data : "");
        break;
    case VIEW_EVENT_HISTORY_DELETE:
        if (ev_data) {
            const char *id = (const char *)ev_data;
            int rc = indicator_session_store_delete(id);
            ESP_LOGI(TAG, "HISTORY_DELETE id='%s' rc=%d", id, rc);
            /* v0.3.0+: SD-side auch loeschen damit storage konsistent
             * bleibt. RP2040 entfernt /sessions/<id>.csv + .json. */
            (void)indicator_sensor_rp2040_cmd(SSC_CMD_SESSION_DELETE_FILE,
                                              id, (uint16_t)strnlen(id, SSC_SESSION_ID_LEN) + 1);
            on_history_list_req();
        }
        break;
    case VIEW_EVENT_HISTORY_WIPE_ALL: {
        ESP_LOGW(TAG, "WIPE_ALL: loesche ALLE sessions aus NVS");
        /* Laufenden readback abbrechen, damit der UI nicht noch chunks
         * einer nun-geloeschten session bekommt. */
        LOCK();
        s_readback_active = false;
        UNLOCK();
        int rc = indicator_session_store_wipe();
        ESP_LOGI(TAG, "wipe_all rc=%d", rc);
        /* Sofort neue (leere) history-liste an UI posten. */
        on_history_list_req();
        break;
    }
    case VIEW_EVENT_HISTORY_FIX_LEGACY:
        start_legacy_fix();
        break;
    case VIEW_EVENT_HISTORY_WIPE_TEST: {
        /* v0.3.0+: atomar alle sessions mit peak<40C loeschen (NVS+SD).
         * Vorher: UI hat n× HISTORY_DELETE gepostet, jeder handler hat
         * on_history_list_req() retriggert -> kaskade von HISTORY_LIST
         * broadcasts in den LVGL-thread -> UI hung. Jetzt komplett im
         * model, am ende EIN broadcast.                                */
        ESP_LOGW(TAG, "WIPE_TEST: loesche sessions mit peak<40C");
        LOCK();
        s_readback_active = false;
        UNLOCK();

        /* IDs vorher sammeln - delete invalidiert die liste-indizes,
         * deshalb erst snapshot, dann iterieren. 32 ist die liste-cap
         * der UI-pagination, deckt alle praktisch relevanten faelle. */
        enum { WIPE_TEST_CAP = 32 };
        EXT_RAM_BSS_ATTR static struct view_data_session_meta wt_items[WIPE_TEST_CAP];
        struct view_data_session_list list = {0};
        list.items = wt_items;
        int lc = indicator_session_store_list(&list, 0, WIPE_TEST_CAP);
        if (lc != 0) {
            ESP_LOGE(TAG, "wipe_test: store_list rc=%d", lc);
            on_history_list_req();
            break;
        }
        char ids[WIPE_TEST_CAP][SSC_SESSION_ID_LEN];
        uint16_t n = 0;
        for (uint16_t i = 0; i < list.count && n < WIPE_TEST_CAP; i++) {
            const struct view_data_session_meta *m = &list.items[i];
            if (!isnan(m->peak_temp) && m->peak_temp < 40.0f) {
                strncpy(ids[n], m->id, SSC_SESSION_ID_LEN);
                ids[n][SSC_SESSION_ID_LEN - 1] = 0;
                n++;
            }
        }
        ESP_LOGI(TAG, "wipe_test: %u kandidaten gefunden (peak<40C)",
                 (unsigned)n);

        for (uint16_t i = 0; i < n; i++) {
            int rc = indicator_session_store_delete(ids[i]);
            ESP_LOGI(TAG, "wipe_test del[%u] id='%s' rc=%d",
                     (unsigned)i, ids[i], rc);
            (void)indicator_sensor_rp2040_cmd(SSC_CMD_SESSION_DELETE_FILE,
                                              ids[i],
                                              (uint16_t)strnlen(ids[i],
                                                  SSC_SESSION_ID_LEN) + 1);
            /* Kurzer yield damit UART-tx + NVS-erase nicht den watchdog
             * triggern bei vielen sessions hintereinander.              */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        /* Genau EIN history-list-rebroadcast am ende. */
        on_history_list_req();
        break;
    }
    case VIEW_EVENT_SESSION_EDIT: {
        if (!ev_data) break;
        const struct view_data_session_meta *m =
            (const struct view_data_session_meta *)ev_data;
        int rc = indicator_session_store_update(m);
        ESP_LOGI(TAG, "SESSION_EDIT id=%s rc=%d", m->id, rc);
        /* v0.3.0: edit auch nach SD pushen damit SD-truth aktuell bleibt. */
        if (rc == 0) {
            ssc_meta_wire_t w;
            meta_pack_to_wire(m, &w);
            (void)indicator_sensor_rp2040_cmd(SSC_CMD_SESSION_META_PUSH,
                                              &w, sizeof(w));
            on_history_list_req();
        }
        break;
    }
    case VIEW_EVENT_SD_META_RESP:
        if (ev_data) on_sd_meta_resp((const ssc_meta_wire_t *)ev_data);
        break;
    case VIEW_EVENT_SD_META_DONE:
        if (ev_data) on_sd_meta_done(*(const uint16_t *)ev_data);
        break;
    default:
        break;
    }
}

/* ======================================================================= */
/* Init                                                                      */
/* ======================================================================= */

int indicator_session_init(void) {
    if (s_inited) return 0;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = SESSION_IDLE;
    s_ctx.samples_cap = SSC_LIVE_SAMPLES_MAX;
    s_ctx.samples = heap_caps_malloc(
        s_ctx.samples_cap * sizeof(live_sample_t), MALLOC_CAP_SPIRAM);
    if (!s_ctx.samples) {
        ESP_LOGE(TAG, "samples buffer alloc failed (%u bytes)",
                 (unsigned)(s_ctx.samples_cap * sizeof(live_sample_t)));
        return -1;
    }
    reset_ctx_live();

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return -2;

    /* Bei den Sensor-Daten-Events interessieren uns nur unsere neuen
     * SAUNA_*-Samples; wir filtern im Handler per sensor_type.     */
    esp_err_t err;
    err = esp_event_handler_register_with(
            view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA,
            on_view_event, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "register SENSOR_DATA: %d", err);

    static const int32_t listened[] = {
        VIEW_EVENT_SESSION_START, VIEW_EVENT_SESSION_AUFGUSS,
        VIEW_EVENT_SESSION_END_REQUEST, VIEW_EVENT_SESSION_SAVE,
        VIEW_EVENT_SESSION_DISCARD,
        VIEW_EVENT_HISTORY_LIST_REQ, VIEW_EVENT_HISTORY_DETAIL_REQ,
        VIEW_EVENT_HISTORY_DELETE, VIEW_EVENT_HISTORY_WIPE_ALL,
        VIEW_EVENT_HISTORY_WIPE_TEST,
        VIEW_EVENT_HISTORY_FIX_LEGACY,
        VIEW_EVENT_SESSION_EDIT,
        /* v0.3.0: hybrid-storage SD->NVS rebuild stream */
        VIEW_EVENT_SD_META_RESP, VIEW_EVENT_SD_META_DONE,
    };
    for (size_t i = 0; i < sizeof(listened) / sizeof(listened[0]); i++) {
        err = esp_event_handler_register_with(
                view_event_handle, VIEW_EVENT_BASE, listened[i],
                on_view_event, NULL);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "register evt %ld failed: %d",
                     (long)listened[i], err);
    }

    /* Heap-status vorher loggen, damit man bei kuenftigen create-
     * failures direkt sieht ob DRAM oder PSRAM knapp wird.           */
    ESP_LOGI(TAG, "pre-worker heap: internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Stack in PSRAM (DRAM ist zu knapp - lessons learned 2026-05-08). */
    BaseType_t rc = xTaskCreateWithCaps(
        session_worker, "ssc_session",
        SSC_TASK_STACK, NULL, SSC_TASK_PRIO, &s_worker,
        MALLOC_CAP_SPIRAM);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed rc=%d", (int)rc);
        return -3;
    }
    ESP_LOGI(TAG, "session_worker task erstellt, handle=%p", (void *)s_worker);

    s_inited = true;
    ESP_LOGI(TAG, "indicator_session initialised (cap=%u samples)",
             s_ctx.samples_cap);

    /* Trigger initialen history-list request an UNS selbst. Jetzt ist
     * garantiert dass wir den handler registriert haben - der UI-part
     * in ui_sauna_init postet diesen event NICHT mehr, weil dort der
     * session-handler noch nicht existiert und der event verpuffte.
     * Resultat: nach boot war home_recent_list leer bis zum ersten
     * manuellen history-klick oder save.                               */
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0,
                      pdMS_TO_TICKS(100));

    /* v0.3.1: auto-sd-list-kick beim boot DEAKTIVIERT. War ein peak-load-
     * spike (META_RESP-stream + n× NVS-write + UART traffic) der bei
     * v0.3.0 auf demselben zeitfenster wie ein anderer panic lag und
     * den bootloop verstaerkt hat. User kann's manuell triggern via
     * Settings -> Speicher -> AUS SD WIEDERHERSTELLEN.                  */
    return 0;
}

/* v0.3.0: oeffentliche API damit user-trigger (z.b. SPEICHER-submenu-
 * button "AUS SD WIEDERHERSTELLEN") jederzeit eine neue list-anfrage
 * feuern kann. RP2040 antwortet stream META_RESP -> on_sd_meta_resp
 * fuellt NVS, on_sd_meta_done broadcasted HISTORY_LIST_REQ.            */
void indicator_session_request_sd_list(void)
{
    int rc = indicator_sensor_rp2040_cmd(SSC_CMD_LIST_SESSIONS, NULL, 0);
    ESP_LOGI(TAG, "SD list request sent rc=%d", rc);
}
