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

#include "view_data.h"
#include "indicator_session.h"
#include "indicator_sensor.h"
#include "indicator_session_store.h"
#include "indicator_mariadb.h"
#include "indicator_http_export.h"

static const char *TAG = "SESSION";

/* ======================================================================= */
/* Konfiguration                                                            */
/* ======================================================================= */

/* Maximale Session-Dauer im RAM: 60 min a 1 Hz. Reicht fuer 99 % aller
 * Saunagaenge; laengere Sessions ueberschreiben den Ringbuffer ab dem
 * Ende (SD behaelt den vollstaendigen Verlauf).                        */
#define SSC_LIVE_SAMPLES_MAX  3600

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
    int rc = indicator_sensor_rp2040_cmd(SSC_CMD_SD_READBACK, payload, sizeof(payload));
    ESP_LOGI(TAG, "rp2040 -> SD_READBACK id='%s' off=%u len=%u req_id=%u (uart_bytes=%d)",
             id, (unsigned)offset, (unsigned)maxlen, (unsigned)s_readback_req_id, rc);
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
                      sizeof(preview), portMAX_DELAY);
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
    static struct view_data_session_meta items[32];
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
                      portMAX_DELAY);
}

static void on_history_detail_req(const char *id) {
    if (!id) return;
    ESP_LOGI(TAG, "on_history_detail_req id='%s'", id);
    LOCK();
    strncpy(s_readback_sid, id, SSC_SESSION_ID_LEN - 1);
    s_readback_sid[SSC_SESSION_ID_LEN - 1] = 0;
    s_readback_next_off = 0;
    s_readback_active   = true;
    UNLOCK();
    /* Max-len 200 bytes (RP2040-ceiling). Der ESP32-UART-parser hat
     * jetzt einen akkumulator-state (siehe indicator_sensor.c), der
     * packets robust re-assembliert - kein chunk-size-workaround mehr
     * noetig. 200 B statt 64 B = 3x weniger round-trips pro session,
     * detail-chart lade-zeit ~2 sek statt ~6 sek.                    */
    rp_request_sd_readback(s_readback_sid, 0, 200);
}

/* ======================================================================= */
/* SD-Readback Chunk-Handler                                                */
/* ======================================================================= */

/* Aus indicator_sensor.c via forward-decl in view_data.h aufgerufen.
 * Payload-Layout: [2B req_id][4B total][4B offset][2B len][data...]  */
void indicator_session_rx_sd_chunk(const uint8_t *payload, size_t n) {
    ESP_LOGI(TAG, "rx_sd_chunk: %u bytes from rp2040", (unsigned)n);
    if (n < 12) { ESP_LOGW(TAG, "  -> too short, drop"); return; }
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

    /* Rohdaten sind CSV. Der einfachste Weg fuer die View: die
     * Samples hier parsen und in 128er-Chunks an VIEW_EVENT_
     * HISTORY_DETAIL_CHUNK-Subscriber schicken. Das halten wir
     * einfach: wir akkumulieren in einem kleinen Linebuffer und
     * feuern bei jedem "\n" einen Sample-Eintrag ab.               */
    static char   linebuf[160];
    static size_t lb = 0;
    static struct view_data_session_samples_chunk chunk;
    if (offset == 0) {
        memset(&chunk, 0, sizeof(chunk));
        strncpy(chunk.session_id, s_readback_sid, SSC_SESSION_ID_LEN - 1);
        chunk.offset = 0;
        chunk.count  = 0;
        lb = 0;
    }
    chunk.total = (uint16_t)((total / 32) & 0xFFFF); /* grobe Schaetzung */

    for (uint16_t i = 0; i < dlen; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (lb == 0) continue;
            linebuf[lb] = 0;
            /* Skip header-Zeile */
            if (strncmp(linebuf, "t_elapsed_s", 11) != 0) {
                unsigned long t_elapsed; float t, rh; char marker[16] = {0};
                int r = sscanf(linebuf, "%lu,%f,%f,%15s",
                               &t_elapsed, &t, &rh, marker);
                if (r >= 3) {
                    struct view_data_session_sample *s =
                        &chunk.samples[chunk.count];
                    s->t_elapsed_s        = (uint32_t)t_elapsed;
                    s->temp               = t;
                    s->rh                 = rh;
                    s->has_aufguss_marker = (r == 4 && marker[0]) ? 1 : 0;
                    chunk.count++;
                    if (chunk.count >= SSC_SAMPLE_CHUNK_MAX) {
                        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                                          VIEW_EVENT_HISTORY_DETAIL_CHUNK,
                                          &chunk, sizeof(chunk),
                                          portMAX_DELAY);
                        chunk.offset += chunk.count;
                        chunk.count = 0;
                    }
                }
            }
            lb = 0;
        } else {
            if (lb < sizeof(linebuf) - 1) linebuf[lb++] = c;
        }
    }

    /* Mehr Daten da? Nachfragen. */
    if (offset + dlen < total) {
        LOCK();
        s_readback_next_off = offset + dlen;
        UNLOCK();
        rp_request_sd_readback(s_readback_sid, offset + dlen, 200);
    } else {
        /* Fertig - Rest-Chunk absenden. */
        if (chunk.count > 0) {
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                              VIEW_EVENT_HISTORY_DETAIL_CHUNK,
                              &chunk, sizeof(chunk),
                              portMAX_DELAY);
        }
        /* DONE-signal: alle chunks durch, UI kann den chart finalisieren. */
        struct view_data_session_detail_done done = {0};
        strncpy(done.session_id, s_readback_sid, SSC_SESSION_ID_LEN - 1);
        ESP_LOGI(TAG, "readback DONE sid=%s", done.session_id);
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_HISTORY_DETAIL_DONE,
                          &done, sizeof(done), portMAX_DELAY);
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
            /* History-liste aktualisieren damit UI die row weg sieht. */
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
    case VIEW_EVENT_SESSION_EDIT: {
        if (!ev_data) break;
        const struct view_data_session_meta *m =
            (const struct view_data_session_meta *)ev_data;
        int rc = indicator_session_store_update(m);
        ESP_LOGI(TAG, "SESSION_EDIT id=%s rc=%d", m->id, rc);
        /* Refresh history damit UI die neuen werte sieht. */
        if (rc == 0) on_history_list_req();
        break;
    }
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
        VIEW_EVENT_SESSION_EDIT,
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

    /* Stack in PSRAM allozieren - internal DRAM ist nach LVGL+WiFi zu
     * fragmentiert fuer einen 8-KB-block. xTaskCreateWithCaps aus
     * freertos/idf_additions.h nimmt die caps fuer den stack.        */
    BaseType_t rc = xTaskCreateWithCaps(
        session_worker, "ssc_session",
        SSC_TASK_STACK, NULL, SSC_TASK_PRIO, &s_worker,
        MALLOC_CAP_SPIRAM);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed (psram) rc=%d, "
                 "heap internal=%u psram=%u - fallback auf DRAM-stack",
                 (int)rc,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        /* Fallback: klassischer xTaskCreate mit halbierter stack-groesse.
         * Besser ein kleinerer DRAM-stack als gar keine session-worker. */
        rc = xTaskCreate(session_worker, "ssc_session",
                         SSC_TASK_STACK / 2, NULL, SSC_TASK_PRIO, &s_worker);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "worker task create failed (dram) rc=%d",
                     (int)rc);
            return -3;
        }
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
                      portMAX_DELAY);
    return 0;
}
