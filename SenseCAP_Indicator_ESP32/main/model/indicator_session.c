/*
 * indicator_session.c - Sauna-Session-Modell (v0.4.0: Spiegel des RP2040)
 *
 * Seit v0.4.0 besitzt der RP2040 die Aufzeichnung komplett (Start/Ende
 * manuell oder automatisch, Ereignis-Erkennung, Statistik, Resume nach
 * Reset). Dieses Modul:
 *   - spiegelt den Rekorder-Zustand (SSC_PKT_SESSION_STATE, alle 2 s)
 *     und baut daraus den 1-Hz-Live-Snapshot fuer die UI,
 *   - legt beim Start den NVS-History-Record an und setzt beim Ende die
 *     gemessenen Werte (Upsert - ein ESP32-Neustart verliert nichts),
 *   - haelt die Ereignisliste (Aufguesse/Lueften/Gaenge) der aktuell
 *     angezeigten Session und schickt Benennungen (Saunameister, Name,
 *     Duft, Teilnehmer) an den RP2040 (.evt auf SD),
 *   - leitet Start/Stopp/Abbrechen/Metadaten als ACK-gesicherte Kommandos
 *     weiter (indicator_sensor_rp_cmd_acked),
 *   - holt History-Details als Huellkurve (RBK2) vom RP2040.
 */

#include <string.h>
#include <time.h>
#include <math.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include "view_data.h"
#include "indicator_session.h"
#include "indicator_sensor.h"
#include "indicator_session_store.h"
#include "indicator_ssc_settings.h"

static const char *TAG = "SESSION";
extern esp_event_loop_handle_t view_event_handle;

#define SSC_TASK_STACK   (8 * 1024)
#define SSC_TASK_PRIO    5
#define SSC_MAX_EV       128

/* ======================================================================= */
/* Zustand                                                                  */
/* ======================================================================= */
typedef struct {
    ssc_session_state_t st;        /* letzter State vom RP2040         */
    bool     have_st;
    int64_t  st_rx_us;             /* Empfangszeit                      */
    bool     recording;
    char     rec_id[SSC_SESSION_ID_LEN];
    float    last_temp, last_rh;
    bool     have_temp, have_rh;
    bool     user_stop;            /* Stopp kam vom Knopf -> UI zeigt Formular */
    char     cancel_id[SSC_SESSION_ID_LEN];   /* nach END loeschen      */
    /* Ereignisliste der angezeigten Session */
    char     ev_id[SSC_SESSION_ID_LEN];
    uint8_t  ev_n;
    bool     ev_loading;
    int64_t  ev_req_us;
    int64_t  cancel_us;
} sess_t;

static sess_t s;
EXT_RAM_BSS_ATTR static ssc_event_full_t s_ev[SSC_MAX_EV];
static SemaphoreHandle_t s_mutex;
static TaskHandle_t      s_worker;
static QueueHandle_t     s_jobs;
static bool              s_inited = false;

#define LOCK()   xSemaphoreTake(s_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mutex)

/* Jobs fuer den Worker (NVS-Arbeit nicht im UART-Task erledigen) */
enum { JOB_NEW = 1, JOB_ENDED = 2 };
typedef struct {
    uint8_t kind;
    ssc_session_state_t st;
} job_t;

/* ======================================================================= */
/* Helfer                                                                   */
/* ======================================================================= */
static void post(int32_t id, const void *data, size_t len, int ms) {
    esp_err_t e = esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, id,
                                    (void *)data, len, pdMS_TO_TICKS(ms));
    if (e != ESP_OK) ESP_LOGW(TAG, "post %ld dropped: 0x%x", (long)id, e);
}

static void id24(uint8_t out[SSC_ID_LEN], const char *id) {
    memset(out, 0, SSC_ID_LEN);
    strncpy((char *)out, id, SSC_ID_LEN - 1);
}

static void meta_pack_to_wire(const struct view_data_session_meta *src, ssc_meta_wire_t *dst) {
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->id, src->id, sizeof(dst->id) - 1);
    dst->start_ts = (int32_t)src->start_ts;
    dst->end_ts   = (int32_t)src->end_ts;
    strncpy(dst->operator_tag, src->operator_tag, sizeof(dst->operator_tag) - 1);
    strncpy(dst->aufguss_headline, src->aufguss_headline, sizeof(dst->aufguss_headline) - 1);
    dst->participants = src->participants;
    strncpy(dst->notes, src->notes, sizeof(dst->notes) - 1);
    dst->peak_temp = src->peak_temp;
    dst->peak_rh = src->peak_rh;
    dst->aufguss_count = src->aufguss_count;
    ssc_meta_seal(dst);
}

static void meta_unpack_from_wire(const ssc_meta_wire_t *src, struct view_data_session_meta *dst) {
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->id, src->id, SSC_SESSION_ID_LEN - 1);
    dst->start_ts = (time_t)src->start_ts;
    dst->end_ts   = (time_t)src->end_ts;
    strncpy(dst->operator_tag, src->operator_tag, SSC_OPERATOR_MAXLEN - 1);
    strncpy(dst->aufguss_headline, src->aufguss_headline, SSC_AUFGUSS_NAME_MAXLEN - 1);
    dst->participants = src->participants;
    strncpy(dst->notes, src->notes, SSC_NOTES_MAXLEN - 1);
    dst->peak_temp = src->peak_temp;
    dst->peak_rh = src->peak_rh;
    dst->aufguss_count = src->aufguss_count;
}

static void request_events(const char *id) {
    uint8_t p[SSC_ID_LEN];
    id24(p, id);
    LOCK();
    if (strncmp(s.ev_id, id, SSC_SESSION_ID_LEN) != 0) memset(s_ev, 0, sizeof(s_ev));
    strncpy(s.ev_id, id, SSC_SESSION_ID_LEN - 1);
    s.ev_id[SSC_SESSION_ID_LEN - 1] = 0;
    s.ev_loading = true;
    s.ev_req_us = esp_timer_get_time();
    UNLOCK();
    indicator_sensor_rp2040_cmd(SSC_CMD_GET_EVENTS, p, SSC_ID_LEN);
}

static uint8_t count_unlabeled_locked(void) {
    if (strcmp(s.ev_id, s.rec_id) != 0) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < s.ev_n; i++) {
        const ssc_event_rec_t *e = &s_ev[i].e;
        if (e->kind == SSC_EK_AUFGUSS && !(e->flags & (SSC_EVF_LABELED | SSC_EVF_DELETED))) n++;
    }
    return n;
}

static void build_live_locked(struct view_data_session_live *L) {
    memset(L, 0, sizeof(*L));
    uint32_t el = s.st.elapsed_s;
    if (s.recording && s.have_st) {
        int64_t d = (esp_timer_get_time() - s.st_rx_us) / 1000000;
        if (d > 0 && d < 6) el += (uint32_t)d;   /* zwischen State-Paketen interpolieren */
    }
    L->t_elapsed_s   = el;
    L->temp          = s.have_temp ? s.last_temp : NAN;
    L->rh            = s.have_rh ? s.last_rh : NAN;
    L->peak_temp     = s.st.peak_t_x10 ? s.st.peak_t_x10 / 10.0f : NAN;
    L->peak_rh       = s.st.peak_rh_x10 ? s.st.peak_rh_x10 / 10.0f : NAN;
    L->aufguss_count = s.st.n_aufguss;
    L->rec_state     = s.recording ? SSC_REC_RECORDING : SSC_REC_IDLE;
    L->flags         = s.st.flags;
    L->phase         = s.st.phase;
    L->gang          = s.st.gang;
    L->n_lueften     = s.st.n_lueften;
    L->n_unlabeled   = count_unlabeled_locked();
    L->origin        = s.st.origin;
    L->rp_link       = (s.have_st && esp_timer_get_time() - s.st_rx_us < 6000000) ? 1 : 0;
    strncpy(L->id, s.rec_id, SSC_SESSION_ID_LEN - 1);
}

/* ======================================================================= */
/* Vom UART-Task (indicator_sensor.c) aufgerufen                            */
/* ======================================================================= */
void indicator_session_on_state(const ssc_session_state_t *st) {
    if (!s_inited || !st) return;
    job_t job = {0};
    bool fire = false, want_events = false;
    char evreq[SSC_SESSION_ID_LEN] = {0};
    LOCK();
    bool was = s.recording;
    char prev_id[SSC_SESSION_ID_LEN];
    memcpy(prev_id, s.rec_id, sizeof(prev_id));
    ssc_session_state_t prev_st = s.st;   /* letzter RECORDING-Stand */
    s.st = *st;
    s.have_st = true;
    s.st_rx_us = esp_timer_get_time();
    bool now_rec = st->rec_state == SSC_REC_RECORDING;
    if (now_rec && (!was || strcmp(prev_id, st->id) != 0)) {
        if (was) {   /* ID-Wechsel ohne IDLE dazwischen: alte abschliessen */
            job.kind = JOB_ENDED; job.st = prev_st;
            strncpy(job.st.id, prev_id, SSC_ID_LEN - 1);
            xQueueSend(s_jobs, &job, 0);
        }
        strncpy(s.rec_id, st->id, SSC_SESSION_ID_LEN - 1);
        s.rec_id[SSC_SESSION_ID_LEN - 1] = 0;
        s.recording = true;
        job.kind = JOB_NEW; job.st = *st; fire = true;
        want_events = true;
    } else if (!now_rec && was) {
        s.recording = false;
        /* Messwerte aus dem letzten RECORDING-State uebernehmen */
        job.kind = JOB_ENDED; job.st = prev_st;
        strncpy(job.st.id, prev_id, SSC_ID_LEN - 1);
        fire = true;
    }
    /* Antwort verloren? Ladezustand nach 5 s freigeben. */
    if (s.ev_loading && esp_timer_get_time() - s.ev_req_us > 5000000) s.ev_loading = false;
    /* Neue Events auf dem RP2040 (z.B. nach ESP32-Neustart) -> Liste holen.
     * Hat die UI zwischendurch eine andere Session angesehen, nach 15 s
     * wieder auf die laufende zurueckschalten (die UI hat ihre Kopie).  */
    if (now_rec && !s.ev_loading) {
        bool same = strcmp(s.ev_id, s.rec_id) == 0;
        if ((same && st->n_events != s.ev_n) ||
            (!same && esp_timer_get_time() - s.ev_req_us > 15000000))
            want_events = true;
    }
    if (want_events) strncpy(evreq, s.rec_id, SSC_SESSION_ID_LEN - 1);
    UNLOCK();
    if (fire) xQueueSend(s_jobs, &job, 0);
    if (want_events && evreq[0]) request_events(evreq);
    LOCK();
    if (s.cancel_id[0] && s.cancel_us && esp_timer_get_time() - s.cancel_us > 15000000) {
        ESP_LOGW(TAG, "Abbrechen von %s nicht ausgefuehrt - verworfen", s.cancel_id);
        s.cancel_id[0] = 0;
    }
    UNLOCK();
}

void indicator_session_on_event(const ssc_event_msg_t *m) {
    if (!s_inited || !m) return;
    post(VIEW_EVENT_REC_EVENT, m, sizeof(*m), 20);
    /* Liste neu holen (enthaelt vorbelegte Labels vom RP2040) */
    request_events(m->id);
}

void indicator_session_on_event_rec(const ssc_event_full_t *m) {
    if (!s_inited || !m) return;
    LOCK();
    if (strcmp(m->id, s.ev_id) == 0 && m->e.no >= 1 && m->e.no <= SSC_MAX_EV) {
        s_ev[m->e.no - 1] = *m;
    }
    UNLOCK();
}

void indicator_session_on_events_done(const char *id, uint8_t n, uint8_t rc) {
    if (!s_inited) return;
    char idc[SSC_SESSION_ID_LEN] = {0};
    LOCK();
    if (strcmp(id, s.ev_id) == 0) {
        s.ev_n = (rc == SSC_RC_OK) ? (n > SSC_MAX_EV ? SSC_MAX_EV : n) : 0;
        s.ev_loading = false;
        strncpy(idc, id, SSC_SESSION_ID_LEN - 1);
    }
    UNLOCK();
    if (idc[0]) post(VIEW_EVENT_EVENTS_UPDATED, idc, sizeof(idc), 50);
}

int indicator_session_events_copy(const char *id, ssc_event_full_t *out, int max) {
    if (!s_inited || !id || !out) return -1;
    int n = -1;
    LOCK();
    if (strcmp(id, s.ev_id) == 0 && !s.ev_loading) {
        n = s.ev_n < max ? s.ev_n : max;
        memcpy(out, s_ev, (size_t)n * sizeof(*out));
    }
    UNLOCK();
    return n;
}

bool indicator_session_is_active(void) {
    if (!s_inited) return false;
    LOCK();
    bool a = s.recording;
    UNLOCK();
    return a;
}

/* ======================================================================= */
/* Worker: NVS-Jobs + 1-Hz-Live-Snapshot + Readback-Watchdog                */
/* ======================================================================= */
static void meta_from_state(const ssc_session_state_t *st, struct view_data_session_meta *m, bool ended) {
    memset(m, 0, sizeof(*m));
    strncpy(m->id, st->id, SSC_SESSION_ID_LEN - 1);
    time_t now = time(NULL);
    m->start_ts = st->start_utc ? (time_t)st->start_utc : now - (time_t)st->elapsed_s;
    m->end_ts   = ended ? m->start_ts + (time_t)st->elapsed_s : 0;
    m->peak_temp = st->peak_t_x10 ? st->peak_t_x10 / 10.0f : NAN;
    m->peak_rh   = st->peak_rh_x10 ? st->peak_rh_x10 / 10.0f : NAN;
    m->aufguss_count = st->n_aufguss;
}

/* --- Readback (History-Detail) --- */
static uint16_t s_rbk_req_id = 0;
static char     s_rbk_sid[SSC_SESSION_ID_LEN];
static bool     s_rbk_active = false;
static uint32_t s_rbk_next_off = 0;
static uint32_t s_rbk_chunks = 0;
static int64_t  s_rbk_t0 = 0;
static uint8_t *s_rbk_buf = NULL;
static size_t   s_rbk_size = 0;

static void rbk_request(uint32_t off) {
    uint8_t p[2 + SSC_SESSION_ID_LEN + 4 + 2] = {0};
    s_rbk_req_id++;
    p[0] = (uint8_t)(s_rbk_req_id >> 8); p[1] = (uint8_t)s_rbk_req_id;
    strncpy((char *)&p[2], s_rbk_sid, SSC_SESSION_ID_LEN - 1);
    p[26] = (uint8_t)(off >> 24); p[27] = (uint8_t)(off >> 16);
    p[28] = (uint8_t)(off >> 8);  p[29] = (uint8_t)off;
    p[30] = 0; p[31] = 200;
    indicator_sensor_rp2040_cmd(SSC_CMD_SD_READBACK, p, sizeof(p));
}

static void rbk_done(void) {
    struct view_data_session_detail_done done = {0};
    strncpy(done.session_id, s_rbk_sid, SSC_SESSION_ID_LEN - 1);
    post(VIEW_EVENT_HISTORY_DETAIL_DONE, &done, sizeof(done), 100);
    LOCK(); s_rbk_active = false; UNLOCK();
}

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* RBK2-Huellkurve -> Sample-Chunks fuer den Detail-Chart. Pro Bucket zwei
 * Punkte (Min, Max), damit Aufguss-Spitzen und Lueften-Dips sichtbar
 * bleiben.                                                              */
static void rbk_parse_emit(const uint8_t *b, size_t n) {
    static struct view_data_session_samples_chunk chunk;
    memset(&chunk, 0, sizeof(chunk));
    strncpy(chunk.session_id, s_rbk_sid, SSC_SESSION_ID_LEN - 1);
    if (n < 10 || memcmp(b, "RBK2", 4) != 0) { ESP_LOGW(TAG, "RBK: kein RBK2-Format"); return; }
    uint16_t cnt = (uint16_t)(b[4] | (b[5] << 8));
    chunk.total = (uint16_t)(cnt * 2);
    const uint8_t *p = b + 10;
    for (uint16_t i = 0; i < cnt && (size_t)(p - b) + 12 <= n; i++, p += 12) {
        uint32_t t = rd32(p);
        int16_t tmin = rd16(p + 4), tmax = rd16(p + 6);
        int16_t hmin = rd16(p + 8), hmax = rd16(p + 10);
        if (tmin == 0x7FFF) continue;
        for (int k = 0; k < 2; k++) {
            struct view_data_session_sample *sm = &chunk.samples[chunk.count++];
            sm->t_elapsed_s = t / 1000;
            sm->temp = (k ? tmax : tmin) / 10.0f;
            sm->rh   = (uint16_t)(k ? hmax : hmin) / 10.0f;
            sm->has_aufguss_marker = 0;
            if (chunk.count >= SSC_SAMPLE_CHUNK_MAX) {
                post(VIEW_EVENT_HISTORY_DETAIL_CHUNK, &chunk, sizeof(chunk), 100);
                chunk.offset += chunk.count;
                chunk.count = 0;
            }
        }
    }
    if (chunk.count) post(VIEW_EVENT_HISTORY_DETAIL_CHUNK, &chunk, sizeof(chunk), 100);
}

void indicator_session_rx_sd_chunk(const uint8_t *pl, size_t n) {
    if (n < 12) return;
    uint16_t req = (uint16_t)((pl[0] << 8) | pl[1]);
    uint32_t total = ((uint32_t)pl[2] << 24) | ((uint32_t)pl[3] << 16) | ((uint32_t)pl[4] << 8) | pl[5];
    uint32_t off   = ((uint32_t)pl[6] << 24) | ((uint32_t)pl[7] << 16) | ((uint32_t)pl[8] << 8) | pl[9];
    uint16_t dlen  = (uint16_t)((pl[10] << 8) | pl[11]);
    if ((size_t)12 + dlen > n) return;
    LOCK();
    bool match = s_rbk_active && req == s_rbk_req_id;
    UNLOCK();
    if (!match) return;
    if (total == 0) {   /* RP2040: Datei fehlt/Fehler -> sofort fertig (L7) */
        rbk_done();
        return;
    }
    if (off == 0) {
        if (s_rbk_buf) { heap_caps_free(s_rbk_buf); s_rbk_buf = NULL; }
        s_rbk_size = 0;
        if (total < 64 * 1024) {
            s_rbk_buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
            if (s_rbk_buf) s_rbk_size = total;
        }
    }
    s_rbk_chunks++;
    /* N3: wrap-sicherer Bounds-Check */
    if (s_rbk_buf && dlen <= s_rbk_size && off <= s_rbk_size - dlen)
        memcpy(s_rbk_buf + off, pl + 12, dlen);
    s_rbk_next_off = off + dlen;
    if (dlen == 0 && off < total) {   /* Lesefehler am RP2040: nicht endlos neu anfragen */
        ESP_LOGW(TAG, "RBK: leerer chunk bei off=%u - abbruch", (unsigned)off);
        rbk_done();
        return;
    }
    if (off + dlen < total) { rbk_request(off + dlen); return; }
    if (s_rbk_buf) rbk_parse_emit(s_rbk_buf, s_rbk_size);
    if (s_rbk_buf) { heap_caps_free(s_rbk_buf); s_rbk_buf = NULL; s_rbk_size = 0; }
    ESP_LOGI(TAG, "RBK fertig sid=%s chunks=%u %ums", s_rbk_sid, (unsigned)s_rbk_chunks,
             (unsigned)((esp_timer_get_time() - s_rbk_t0) / 1000));
    rbk_done();
}

static void on_history_detail_req(const char *id) {
    if (!id || !id[0]) return;
    LOCK();
    strncpy(s_rbk_sid, id, SSC_SESSION_ID_LEN - 1);
    s_rbk_sid[SSC_SESSION_ID_LEN - 1] = 0;
    s_rbk_active = true;
    s_rbk_next_off = 0;
    s_rbk_chunks = 0;
    UNLOCK();
    s_rbk_t0 = esp_timer_get_time();
    rbk_request(0);
    request_events(id);
}

static void on_history_list_req(void) {
    EXT_RAM_BSS_ATTR static struct view_data_session_meta items[32];
    struct view_data_session_list list = {0};
    list.items = items;
    if (indicator_session_store_list(&list, 0, 32) != 0) return;
    post(VIEW_EVENT_HISTORY_LIST, &list, sizeof(list), 100);
}

static void send_delete(const char *id);
static void do_job(const job_t *j) {
    struct view_data_session_meta m;
    if (j->kind == JOB_NEW) {
        meta_from_state(&j->st, &m, false);
        int rc = indicator_session_store_put_measured(&m);
        ESP_LOGI(TAG, "Session gestartet (RP2040): %s origin=%u nvs=%d", m.id, j->st.origin, rc);
        post(VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0, 200);   /* Liste nur im Event-Task bauen */
    } else if (j->kind == JOB_ENDED) {
        /* ABBRECHEN: hier (nach dem Ende) loeschen - im ACK-Pfad konnte der
         * Record danach vom Ende-Job wieder angelegt werden (Review M1). */
        LOCK();
        bool cancelled = s.cancel_id[0] && strncmp(s.cancel_id, j->st.id, SSC_ID_LEN) == 0;
        if (cancelled) s.cancel_id[0] = 0;
        UNLOCK();
        if (cancelled) {
            ESP_LOGW(TAG, "Session %s abgebrochen -> loeschen", j->st.id);
            indicator_session_store_delete(j->st.id);
            send_delete(j->st.id);
            post(VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0, 200);
            return;
        }
        meta_from_state(&j->st, &m, true);
        int rc = indicator_session_store_put_measured(&m);
        ESP_LOGI(TAG, "Session beendet: %s dauer=%lus aufguss=%u nvs=%d", m.id,
                 (unsigned long)j->st.elapsed_s, j->st.n_aufguss, rc);
        struct view_data_session_meta full;
        if (indicator_session_store_get(m.id, &full) == 0) m = full;
        LOCK();
        bool by_user = s.user_stop;
        s.user_stop = false;
        UNLOCK();
        /* Hinweis auf dem Home-Screen immer; das Formular nur, wenn der
         * Stopp per Knopf kam (Auto-Ende stoert niemanden).              */
        post(VIEW_EVENT_SESSION_ENDED, &m, sizeof(m), 100);
        if (by_user) post(VIEW_EVENT_SESSION_SUMMARY_READY, &m, sizeof(m), 100);
        post(VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0, 200);
    }
}

static QueueHandle_t s_meta_q = NULL;   /* SD-Liste: ssc_meta_wire_t, PSRAM */
static void on_sd_meta_resp(const ssc_meta_wire_t *w);
/* Vom UART-Task: META_RESP nicht durch die Event-Loop (die kopiert jede
 * Nutzlast ins interne DRAM, Review H3) - direkt in eine PSRAM-Queue,
 * der Worker sortiert sie in NVS ein.                                  */
void indicator_session_on_meta_resp(const ssc_meta_wire_t *w) {
    if (!s_meta_q || !w) return;
    if (xQueueSend(s_meta_q, w, pdMS_TO_TICKS(100)) != pdTRUE)
        ESP_LOGW(TAG, "META_RESP verworfen (queue voll) %s", w->id);
}

static void session_worker(void *arg) {
    int64_t next_live = esp_timer_get_time();
    uint32_t rbk_last_chunks = 0; uint8_t rbk_stall = 0, rbk_retries = 0;
    while (1) {
        job_t j;
        if (xQueueReceive(s_jobs, &j, pdMS_TO_TICKS(200)) == pdTRUE) do_job(&j);
        {
            static ssc_meta_wire_t mw;
            int nm = 0;
            static int pending = 0;
            int got = 0;
            while (nm++ < 8 && s_meta_q && xQueueReceive(s_meta_q, &mw, 0) == pdTRUE) { on_sd_meta_resp(&mw); got++; }
            pending += got;
            if (!got && pending) { pending = 0; post(VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0, 200); }
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_live) {
            next_live = now + 1000000;
            struct view_data_session_live L;
            LOCK();
            build_live_locked(&L);
            UNLOCK();
            post(VIEW_EVENT_SESSION_LIVE, &L, sizeof(L), 50);

            /* Readback-Watchdog: 2 s ohne Fortschritt -> ab letztem Offset
             * neu anfragen, max 10x, danach mit dem was da ist fertig.   */
            LOCK();
            bool act = s_rbk_active;
            UNLOCK();
            if (act) {
                if (s_rbk_chunks == rbk_last_chunks) rbk_stall++;
                else { rbk_stall = 0; rbk_last_chunks = s_rbk_chunks; }
                if (rbk_stall >= 2) {
                    rbk_stall = 0;
                    if (rbk_retries++ < 10) {
                        ESP_LOGW(TAG, "RBK stall -> retry %u off=%u", rbk_retries, (unsigned)s_rbk_next_off);
                        rbk_request(s_rbk_next_off);
                    } else {
                        ESP_LOGE(TAG, "RBK aufgegeben");
                        /* Puffer gehoert dem UART-Task (Review M3) - nur beenden;
                         * er wird beim naechsten Readback (offset 0) ersetzt. */
                        rbk_done();
                        rbk_retries = 0;
                    }
                }
            } else {
                rbk_retries = 0; rbk_stall = 0; rbk_last_chunks = 0;
            }
        }
    }
}

/* ======================================================================= */
/* SD -> NVS Wiederherstellung                                              */
/* ======================================================================= */
static void on_sd_meta_resp(const ssc_meta_wire_t *w) {
    if (!w || w->id[0] == 0) return;
    struct view_data_session_meta existing;
    struct view_data_session_meta meta;
    meta_unpack_from_wire(w, &meta);
    if (indicator_session_store_get(w->id, &existing) == 0) {
        /* Offener Record (Ende verpasst, z.B. ESP32 war aus): Messwerte vom
         * RP2040 uebernehmen, aber nie die laufende Session (Review H5). */
        LOCK();
        bool active = s.recording && strcmp(w->id, s.rec_id) == 0;
        UNLOCK();
        if (existing.end_ts == 0 && meta.end_ts != 0 && !active) {
            indicator_session_store_put_measured(&meta);
            ESP_LOGI(TAG, "SD_META_RESP: offener Record %s geschlossen", meta.id);
        }
        return;
    }
    int rs = indicator_session_store_append(&meta);
    ESP_LOGI(TAG, "SD_META_RESP recovered id=%s rc=%d", meta.id, rs);
}

/* ======================================================================= */
/* Event-Dispatcher (Event-Task)                                             */
/* ======================================================================= */
static void send_delete(const char *id) {
    uint8_t p[SSC_ID_LEN];
    id24(p, id);
    indicator_sensor_rp_cmd_acked(SSC_CMD_DELETE_FILE, p, SSC_ID_LEN);
}

static void push_meta(const struct view_data_session_meta *m) {
    ssc_meta_wire_t w;
    meta_pack_to_wire(m, &w);
    indicator_sensor_rp_cmd_acked(SSC_CMD_META_PUSH, &w, sizeof(w));
}

static void on_view_event(void *arg, esp_event_base_t base, int32_t id, void *d) {
    switch (id) {
    case VIEW_EVENT_SENSOR_DATA: {
        const struct view_data_sensor_data *ev = d;
        if (!ev) break;
        LOCK();
        if (ev->sensor_type == SENSOR_DATA_SAUNA_TEMP) { s.last_temp = ev->vaule; s.have_temp = true; }
        else if (ev->sensor_type == SENSOR_DATA_SAUNA_RH) { s.last_rh = ev->vaule; s.have_rh = true; }
        UNLOCK();
        break;
    }
    case VIEW_EVENT_SESSION_START: {
        uint8_t origin = SSC_ORIGIN_MANUAL;
        indicator_sensor_request_time_sync();
        indicator_sensor_rp_cmd_acked(SSC_CMD_SESSION_START, &origin, 1);
        break;
    }
    case VIEW_EVENT_SESSION_END_REQUEST: {
        LOCK(); s.user_stop = true; UNLOCK();
        uint8_t r = SSC_END_MANUAL;
        indicator_sensor_rp_cmd_acked(SSC_CMD_SESSION_END, &r, 1);
        break;
    }
    case VIEW_EVENT_SESSION_DISCARD: {
        /* ABBRECHEN: Session beenden und danach komplett loeschen */
        LOCK();
        bool rec = s.recording;
        char rid[SSC_SESSION_ID_LEN];
        memcpy(rid, s.rec_id, sizeof(rid));
        if (rec) { memcpy(s.cancel_id, rid, sizeof(rid)); s.cancel_us = esp_timer_get_time(); }
        UNLOCK();
        if (rec) {
            uint8_t r = SSC_END_CANCEL;
            indicator_sensor_rp_cmd_acked(SSC_CMD_SESSION_END, &r, 1);
        }
        break;
    }
    case VIEW_EVENT_RP_CMD_RESULT: {
        const ssc_ack_t *a = d;
        if (!a) break;
        /* Abbrechen wird im Worker nach JOB_ENDED erledigt (siehe do_job). */
        if (a->cmd == SSC_CMD_SESSION_END && a->rc != SSC_RC_OK && a->rc != 0xFF) {
            /* Ende abgelehnt (lief schon nicht mehr): Abbrechen/Formular-
             * Absicht verwerfen, sonst trifft sie spaeter eine andere
             * Session (Review H1, M1).                                 */
            LOCK(); s.cancel_id[0] = 0; s.user_stop = false; UNLOCK();
        }
        if (a->cmd == SSC_CMD_EVENT_LABEL || a->cmd == SSC_CMD_ADD_EVENT) {
            char eid[SSC_SESSION_ID_LEN];
            LOCK(); memcpy(eid, s.ev_id, sizeof(eid)); bool busy = s.ev_loading; UNLOCK();
            if (eid[0] && !busy) request_events(eid);
        }
        break;
    }
    case VIEW_EVENT_SESSION_SAVE:
    case VIEW_EVENT_SESSION_EDIT: {
        const struct view_data_session_meta *m = d;
        if (!m || !m->id[0]) break;
        struct view_data_session_meta cur;
        if (indicator_session_store_get(m->id, &cur) != 0) {
            /* Nur SAVE darf neu anlegen - EDIT einer geloeschten Session
             * wuerde sie sonst zurueckholen (Review M3).              */
            if (id != VIEW_EVENT_SESSION_SAVE) { ESP_LOGW(TAG, "edit: %s existiert nicht mehr", m->id); break; }
            indicator_session_store_append(m);
        } else {
            indicator_session_store_update(m);
        }
        struct view_data_session_meta full;
        if (indicator_session_store_get(m->id, &full) == 0) push_meta(&full);
        on_history_list_req();
        break;
    }
    case VIEW_EVENT_EVENTS_REQ:
        if (d) request_events((const char *)d);
        break;
    case VIEW_EVENT_EVENT_LABEL_SET: {
        if (!d) break;
        ssc_event_label_t l = *(const ssc_event_label_t *)d;
        l.crc = ssc_crc16(&l, offsetof(ssc_event_label_t, crc));
        indicator_sensor_rp_cmd_acked(SSC_CMD_EVENT_LABEL, &l, sizeof(l));
        break;
    }
    case VIEW_EVENT_EVENT_ADD: {
        ssc_add_event_t a = { SSC_EK_AUFGUSS, 0 };
        indicator_sensor_rp_cmd_acked(SSC_CMD_ADD_EVENT, &a, sizeof(a));
        break;
    }
    case VIEW_EVENT_AUTO_MODE_SET: {
        if (!d) break;
        g_ssc_settings._reserved[0] = (*(const uint8_t *)d) ? 0 : 1;
        ssc_settings_save();
        indicator_sensor_send_auto_cfg();
        break;
    }
    case VIEW_EVENT_SENSOR_SELFTEST_REQ: {
        uint8_t mode = 0;
        indicator_sensor_rp_cmd_acked(SSC_CMD_SENSOR_SELFTEST, &mode, 1);
        break;
    }
    case VIEW_EVENT_HISTORY_LIST_REQ:
        on_history_list_req();
        break;
    case VIEW_EVENT_HISTORY_DETAIL_REQ:
        on_history_detail_req(d ? (const char *)d : "");
        break;
    case VIEW_EVENT_HISTORY_DELETE:
        if (d) {
            const char *hid = d;
            LOCK();
            bool active = s.recording && strcmp(hid, s.rec_id) == 0;
            UNLOCK();
            if (active) { ESP_LOGW(TAG, "aktive Session wird nicht geloescht"); break; }
            indicator_session_store_delete(hid);
            send_delete(hid);
            on_history_list_req();
        }
        break;
    case VIEW_EVENT_HISTORY_WIPE_ALL:
        LOCK(); s_rbk_active = false; UNLOCK();
        indicator_session_store_wipe();
        on_history_list_req();
        break;
    case VIEW_EVENT_HISTORY_WIPE_TEST: {
        /* Test-Sessions: Peak < 40 °C oder Simulation (ID X.../Y...) */
        EXT_RAM_BSS_ATTR static struct view_data_session_meta wt[64];
        struct view_data_session_list list = {0};
        list.items = wt;
        if (indicator_session_store_list(&list, 0, 64) != 0) break;
        uint16_t n = 0;
        for (uint16_t i = 0; i < list.count; i++) {
            const struct view_data_session_meta *m = &wt[i];
            bool sim = m->id[0] == 'X' || m->id[0] == 'Y';
            LOCK();
            bool active = s.recording && strcmp(m->id, s.rec_id) == 0;
            UNLOCK();
            if (active) continue;
            if (sim || (!isnan(m->peak_temp) && m->peak_temp < 40.0f)) {
                indicator_session_store_delete(m->id);
                uint8_t p[SSC_ID_LEN];
                id24(p, m->id);
                indicator_sensor_rp2040_cmd(SSC_CMD_DELETE_FILE, p, SSC_ID_LEN);
                n++;
                vTaskDelay(pdMS_TO_TICKS(40));
            }
        }
        ESP_LOGI(TAG, "WIPE_TEST: %u sessions geloescht", n);
        on_history_list_req();
        break;
    }
    case VIEW_EVENT_SD_META_RESP:
        if (d) on_sd_meta_resp((const ssc_meta_wire_t *)d);
        break;
    case VIEW_EVENT_SD_META_DONE:
        on_history_list_req();
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
    memset(&s, 0, sizeof(s));
    s.last_temp = s.last_rh = NAN;
    s_mutex = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(8, sizeof(job_t));
    s_meta_q = xQueueCreateWithCaps(32, sizeof(ssc_meta_wire_t), MALLOC_CAP_SPIRAM);
    if (!s_mutex || !s_jobs) return -2;

    static const int32_t listened[] = {
        VIEW_EVENT_SENSOR_DATA,
        VIEW_EVENT_SESSION_START, VIEW_EVENT_SESSION_END_REQUEST,
        VIEW_EVENT_SESSION_SAVE, VIEW_EVENT_SESSION_DISCARD, VIEW_EVENT_SESSION_EDIT,
        VIEW_EVENT_HISTORY_LIST_REQ, VIEW_EVENT_HISTORY_DETAIL_REQ,
        VIEW_EVENT_HISTORY_DELETE, VIEW_EVENT_HISTORY_WIPE_ALL, VIEW_EVENT_HISTORY_WIPE_TEST,
        VIEW_EVENT_SD_META_RESP, VIEW_EVENT_SD_META_DONE,
        VIEW_EVENT_RP_CMD_RESULT, VIEW_EVENT_EVENTS_REQ, VIEW_EVENT_EVENT_LABEL_SET,
        VIEW_EVENT_EVENT_ADD, VIEW_EVENT_AUTO_MODE_SET, VIEW_EVENT_SENSOR_SELFTEST_REQ,
    };
    for (size_t i = 0; i < sizeof(listened) / sizeof(listened[0]); i++) {
        esp_err_t err = esp_event_handler_register_with(view_event_handle, VIEW_EVENT_BASE,
                                                        listened[i], on_view_event, NULL);
        if (err != ESP_OK) ESP_LOGE(TAG, "register evt %ld failed: %d", (long)listened[i], err);
    }
    if (xTaskCreateWithCaps(session_worker, "ssc_session", SSC_TASK_STACK, NULL,
                            SSC_TASK_PRIO, &s_worker, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        return -3;
    }
    s_inited = true;
    post(VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0, 100);
    ESP_LOGI(TAG, "indicator_session v0.4 (RP2040-Spiegel) initialisiert");
    return 0;
}

void indicator_session_request_sd_list(void) {
    int rc = indicator_sensor_rp2040_cmd(SSC_CMD_LIST_SESSIONS, NULL, 0);
    ESP_LOGI(TAG, "SD list request sent rc=%d", rc);
}
