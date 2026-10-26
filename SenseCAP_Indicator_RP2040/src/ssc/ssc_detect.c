/*
 * ssc_detect.c - Sauna-Abend-Detektor. Siehe ssc_detect.h.
 *
 * Merkmale (aus den Sessions der Saison 2026 abgeleitet):
 *  - Aufguss (Kelle): absolute Feuchte springt um +15..+40 g/m3 binnen
 *    ~20 s. Atmen/Schwitzen der Gaeste: ~+1.5 g/m3 pro Minute -> Schwelle
 *    +10 g/m3 in 30 s hat >6x Abstand. Nur bei heisser Kabine (>= 50 °C),
 *    damit Anhauchen bei Raumtemperatur nie als Aufguss zaehlt.
 *  - Lueften: T faellt minutenlang (>= 1.2 K in 2 min) UND die absolute
 *    Feuchte faellt nahe an die trockene Basis der letzten 10 min. Waehrend
 *    eines Aufgusses faellt T ebenfalls (Wedeln), aber die Feuchte bleibt
 *    hoch - das unterscheidet die beiden. Bestaetigung nach 120 s.
 *  - Lueften-Ende: Nachheizen, T steigt >= 0.8 K/min fuer 60 s.
 *  - Ende: T < 45 °C fallend, oder T > 25 K unter Session-Peak und < 60 °C
 *    und weiter fallend.
 *  - Auto-Start: T >= 40 °C und +3 K in 5 min, oder >= 60 °C seit 2 min.
 *
 * Alles laeuft auf 1-s- und 5-s-Mittelwerten, damit 1 Hz und 2 Hz Eingang
 * identisch behandelt werden.
 */
#include "ssc_detect.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Physik                                                              */
/* ------------------------------------------------------------------ */
static float es_hpa(float t) {
    /* Arden-Buck ueber Wasser, gut bis ~100 °C */
    return 6.1121f * expf((18.678f - t / 234.5f) * (t / (257.14f + t)));
}

float ssc_abs_humidity(float t_c, float rh_pct) {
    if (isnan(t_c) || isnan(rh_pct)) return NAN;
    return 216.7f * (rh_pct / 100.0f * es_hpa(t_c)) / (273.15f + t_c);
}

float ssc_dewpoint(float t_c, float rh_pct) {
    if (isnan(t_c) || isnan(rh_pct) || rh_pct <= 0.1f) return NAN;
    float e = rh_pct / 100.0f * es_hpa(t_c);
    float g = logf(e / 6.1121f);
    /* Magnus-Inverse (b=17.62, c=243.12) - Genauigkeit reicht fuer Health */
    return 243.12f * g / (17.62f - g);
}

const char *ssc_ev_name(uint8_t k) {
    switch (k) {
    case SSC_EV_AUTO_START:    return "AUTO_START";
    case SSC_EV_GANG_START:    return "GANG";
    case SSC_EV_AUFGUSS:       return "AUFGUSS";
    case SSC_EV_LUEFTEN_START: return "LUEFTEN_START";
    case SSC_EV_LUEFTEN_END:   return "LUEFTEN_END";
    case SSC_EV_AUTO_END:      return "AUTO_END";
    case SSC_EV_RH_SUSPECT:    return "RH_SUSPECT";
    case SSC_EV_RH_OK:         return "RH_OK";
    default:                   return "?";
    }
}

static int emit(ssc_det_event_t *out, int n, int max, const ssc_det_event_t *e);

/* ------------------------------------------------------------------ */
/* Ring-Helfer                                                         */
/* ------------------------------------------------------------------ */
/* k Schritte zurueck (0 = neuester). Aufrufer prueft k < fill.        */
#define R5(arr, k) (d->arr[(uint8_t)((d->r5_head + SSC_DET_R5_N - 1 - (k)) % SSC_DET_R5_N)])
#define R1(arr, k) (d->arr[(uint8_t)((d->r1_head + SSC_DET_R1_N - 1 - (k)) % SSC_DET_R1_N)])

void ssc_det_init(ssc_det_t *d) {
    memset(d, 0, sizeof(*d));
    d->start_t_min        = 40.0f;
    d->start_rise_5min    = 3.0f;
    d->hot_t              = 55.0f;
    d->pour_dah           = 10.0f;
    d->end_t_abs          = 45.0f;
    d->end_drop_from_peak = 25.0f;
    d->max_session_ms     = 8UL * 3600UL * 1000UL;
    d->phase              = SSC_PH_IDLE;
    d->peak_t = d->peak_rh = -1000.0f;
    d->hot_armed          = true;
}

void ssc_det_session_begin(ssc_det_t *d, uint32_t t_ms, bool manual) {
    d->session        = true;
    d->manual         = manual;
    d->sess_start_ms  = t_ms;
    d->was_hot        = false;
    d->n_pour = d->n_luef = d->gang = 0;
    d->pour_active    = false;
    d->luef_cand_ms   = 0;
    d->reheat_since_ms = 0;
    d->cool_since_ms  = 0;
    d->heat_hold_ms   = 0;
    d->peak_t = d->peak_rh = -1000.0f;
    d->heatup_start_ms = d->heatup_end_ms = 0;
    d->last_pour_end_ms = 0;
    d->last_valid_ms  = t_ms;
    d->end_sent       = false;
    d->phase          = SSC_PH_HEATUP;   /* wird beim naechsten 5-s-Tick
                                            ggf. sofort auf GANG gehoben */
}

int ssc_det_finish(ssc_det_t *d, ssc_det_event_t *out, int max) {
    int n = 0;
    if (d->pour_active) {
        d->pour_active = false;
        d->pour.t_end_ms = d->last_ms;
        d->pour.no   = ++d->n_pour;
        d->pour.gang = d->gang;
        d->last_pour_end_ms = d->last_ms;
        n = emit(out, n, max, &d->pour);
    }
    if (d->phase == SSC_PH_LUEFTEN) {
        d->luef.kind = SSC_EV_LUEFTEN_END;
        d->luef.t_end_ms = d->last_ms;
        n = emit(out, n, max, &d->luef);
    }
    return n;
}

void ssc_det_session_end(ssc_det_t *d) {
    d->session     = false;
    d->phase       = SSC_PH_IDLE;
    d->pour_active = false;
    d->ended_ms    = d->last_ms;
    d->ever_ended  = true;
    d->hot_armed   = false;   /* "heiss"-Trigger erst nach Abkuehlen unter 40 °C */
}

static int emit(ssc_det_event_t *out, int n, int max, const ssc_det_event_t *e) {
    if (n < max) out[n] = *e;
    return (n < max) ? n + 1 : n;
}

static void ev_basic(ssc_det_event_t *e, uint8_t kind, uint8_t no, uint8_t gang,
                     uint32_t t, float T, float rh) {
    memset(e, 0, sizeof(*e));
    e->kind = kind; e->no = no; e->gang = gang;
    e->t_start_ms = e->t_peak_ms = e->t_end_ms = t;
    e->t0 = e->tpk = T;
    e->rh0 = e->rhpk = rh;
    e->ah0 = e->ahpk = ssc_abs_humidity(T, rh);
}

/* ------------------------------------------------------------------ */
/* 1-s-Tick: Aufguss-Erkennung                                         */
/* ------------------------------------------------------------------ */
static int tick1(ssc_det_t *d, uint32_t t, ssc_det_event_t *out, int n, int max) {
    float ah  = R1(r1_ah, 0);
    float T   = R1(r1_t, 0);
    float rh  = R1(r1_rh, 0);
    if (isnan(ah)) return n;

    if (!d->pour_active) {
        if (!d->session || T < 50.0f) return n;
        /* Minimum der letzten 30 s */
        uint8_t lim = d->r1_fill < 31 ? d->r1_fill : 31;
        float mn = ah; uint8_t kmin = 0;
        for (uint8_t k = 1; k < lim; k++) {
            float v = R1(r1_ah, k);
            if (v < mn) { mn = v; kmin = k; }
        }
        if (ah - mn >= d->pour_dah) {
            d->pour_active = true;
            memset(&d->pour, 0, sizeof(d->pour));
            d->pour.kind       = SSC_EV_AUFGUSS;
            d->pour.t_start_ms = t - (uint32_t)kmin * 1000UL;
            d->pour.t0  = R1(r1_t, kmin);
            d->pour.rh0 = R1(r1_rh, kmin);
            d->pour.ah0 = mn;
            d->pour.t_peak_ms = t;
            d->pour.tpk = T; d->pour.rhpk = rh; d->pour.ahpk = ah;
            d->pour_min_since_peak = ah;
            d->pour_min_since_peak_ms = t;
        }
        return n;
    }

    /* Aufguss laeuft. "next" VOR dem Peak-Update pruefen: eine neue Kelle
     * uebersteigt oft den Peak der vorigen und wuerde sonst verschluckt.
     * Neue Kelle erst >= 90 s nach Beginn der aktuellen - die Turbulenz
     * einer Kelle (Wedeln) erzeugt mehrere Zacken, echte Kellen kommen
     * im Abstand von ~2 min (Daten 04/05-2026).                          */
    bool next    = (ah - d->pour_min_since_peak >= d->pour_dah) &&
                   (d->pour_min_since_peak_ms > d->pour.t_peak_ms) &&
                   (t - d->pour.t_start_ms >= 90000UL);
    if (!next) {
        if (ah > d->pour.ahpk) {
            d->pour.ahpk = ah; d->pour.rhpk = rh; d->pour.tpk = T;
            d->pour.t_peak_ms = t;
            d->pour_min_since_peak = ah;
            d->pour_min_since_peak_ms = t;
        } else if (ah < d->pour_min_since_peak) {
            d->pour_min_since_peak = ah;
            d->pour_min_since_peak_ms = t;
        }
    }

    bool decayed = (t - d->pour.t_start_ms >= 90000UL) &&
                   (t - d->pour.t_peak_ms >= 20000UL) &&
                   (ah <= d->pour.ahpk - 0.5f * (d->pour.ahpk - d->pour.ah0));
    bool timeout = (t - d->pour.t_peak_ms >= 90000UL);
    if (decayed || timeout || next) {
        d->pour_active = false;
        d->pour.t_end_ms = next ? d->pour_min_since_peak_ms : t;
        d->pour.no   = ++d->n_pour;
        d->pour.gang = d->gang;
        d->last_pour_end_ms = d->pour.t_end_ms;
        n = emit(out, n, max, &d->pour);
        if (next) {
            /* direkt naechste Kelle starten, Basis = Minimum dazwischen */
            float mn = d->pour_min_since_peak;
            uint32_t tmn = d->pour_min_since_peak_ms;
            d->pour_active = true;
            memset(&d->pour, 0, sizeof(d->pour));
            d->pour.kind = SSC_EV_AUFGUSS;
            d->pour.t_start_ms = tmn;
            d->pour.t0 = T; d->pour.rh0 = rh; d->pour.ah0 = mn;
            d->pour.t_peak_ms = t;
            d->pour.tpk = T; d->pour.rhpk = rh; d->pour.ahpk = ah;
            d->pour_min_since_peak = ah;
            d->pour_min_since_peak_ms = t;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 5-s-Tick: Phasen, Start/Ende, Lueften, Plausibilitaet               */
/* ------------------------------------------------------------------ */
static int tick5(ssc_det_t *d, uint32_t t, ssc_det_event_t *out, int n, int max) {
    float T  = R5(r5_t, 0);
    float rh = R5(r5_rh, 0);
    float ah = R5(r5_ah, 0);
    ssc_det_event_t e;

    /* ---- Auto-Start-Erkennung (nur ohne Session) ---- */
    if (!d->session) {
        bool guard = d->ever_ended && (t - d->ended_ms < 30UL * 60000UL);
        if (T < d->start_t_min) d->hot_armed = true;
        bool rising = d->r5_fill > 60 && T >= d->start_t_min &&
                      (T - R5(r5_t, 60)) >= d->start_rise_5min;
        bool hot = d->r5_fill > 24 && T >= 60.0f;
        if (hot) for (uint8_t k = 1; k <= 24; k++) if (R5(r5_t, k) < 60.0f) { hot = false; break; }
        /* Nach einem Ende (z.B. manueller Stopp bei heissem Ofen) nie wegen
         * "noch heiss" neu starten - nur echtes Neu-Aufheizen (rising) oder
         * nach Abkuehlen unter start_t_min (Review H4).                   */
        if (!d->hot_armed || guard) hot = false;
        if (guard) rising = false;
        if (rising || hot) {
            /* Aufheiz-Beginn: aeltestes Minimum im Ring */
            uint8_t kmin = 0; float mn = T;
            for (uint8_t k = 0; k < d->r5_fill; k++) {
                float v = R5(r5_t, k);
                if (v <= mn + 0.05f) { mn = v; kmin = k; }
            }
            ev_basic(&e, SSC_EV_AUTO_START, 0, 0, t, T, rh);
            e.t_start_ms = t - (uint32_t)kmin * 5000UL;
            e.t0 = mn;
            n = emit(out, n, max, &e);
        }
        return n;
    }

    if (T >= d->hot_t) {
        if (!d->was_hot) d->hot_since_ms = t;
        d->was_hot = true;
    }

    /* ---- Aufheizen -> Gang 1 ---- */
    if (d->phase == SSC_PH_HEATUP) {
        if (d->heatup_start_ms == 0) d->heatup_start_ms = d->sess_start_ms;
        bool plateau = T >= d->hot_t && d->r5_fill > 36 && (T - R5(r5_t, 36)) < 1.5f;
        if (plateau || d->pour_active || T >= 75.0f) {
            d->heatup_end_ms = t;
            d->phase = SSC_PH_GANG;
            d->gang = 1;
            ev_basic(&e, SSC_EV_GANG_START, 1, 1, t, T, rh);
            n = emit(out, n, max, &e);
        }
    }

    /* ---- Lueften ---- */
    if (d->phase == SSC_PH_GANG && d->r5_fill > 24) {
        float mn10 = ah, mx5 = ah;
        for (uint8_t k = 0; k < d->r5_fill; k++) {
            float v = R5(r5_ah, k);
            if (v < mn10) mn10 = v;
            if (k <= 60 && v > mx5) mx5 = v;
        }
        float dT2 = T - R5(r5_t, 24);
        bool cond = !d->pour_active &&
                    (d->last_pour_end_ms == 0 || t - d->last_pour_end_ms >= 60000UL) &&
                    dT2 <= -1.2f &&
                    ah <= mn10 + 0.35f * (mx5 - mn10);
        if (cond) {
            if (d->luef_cand_ms == 0) d->luef_cand_ms = t;
            if (t - d->luef_cand_ms >= 120000UL) {
                /* Beginn: letzter Zeitpunkt mit AH >= 95 % vom 5-min-Max */
                uint8_t kb = 0;
                for (uint8_t k = 0; k <= 60 && k < d->r5_fill; k++) {
                    if (R5(r5_ah, k) >= 0.95f * mx5) { kb = k; break; }
                }
                memset(&d->luef, 0, sizeof(d->luef));
                d->luef.kind = SSC_EV_LUEFTEN_START;
                d->luef.no   = ++d->n_luef;
                d->luef.gang = d->gang;
                d->luef.t_start_ms = t - (uint32_t)kb * 5000UL;
                d->luef.t0  = R5(r5_t, kb);
                d->luef.rh0 = R5(r5_rh, kb);
                d->luef.ah0 = R5(r5_ah, kb);
                d->luef.tpk = T; d->luef.rhpk = rh; d->luef.ahpk = ah;
                d->luef.t_peak_ms = t;
                d->luef.t_end_ms  = t;
                n = emit(out, n, max, &d->luef);
                d->phase = SSC_PH_LUEFTEN;
                d->luef_cand_ms = 0;
                d->reheat_since_ms = 0;
            }
        } else {
            d->luef_cand_ms = 0;
        }
    } else if (d->phase == SSC_PH_LUEFTEN) {
        if (T < d->luef.tpk) { d->luef.tpk = T; d->luef.t_peak_ms = t; }
        if (ah < d->luef.ahpk) { d->luef.ahpk = ah; d->luef.rhpk = rh; }
        float dT1 = (d->r5_fill > 12) ? T - R5(r5_t, 12) : 0.0f;
        if (dT1 >= 0.8f) {
            if (d->reheat_since_ms == 0) d->reheat_since_ms = t - 60000UL;
            if (t - d->reheat_since_ms >= 120000UL) {
                d->luef.kind = SSC_EV_LUEFTEN_END;
                d->luef.t_end_ms = d->reheat_since_ms;
                n = emit(out, n, max, &d->luef);
                d->gang++;
                d->phase = SSC_PH_GANG;
                ev_basic(&e, SSC_EV_GANG_START, d->gang, d->gang,
                         d->reheat_since_ms, T, rh);
                n = emit(out, n, max, &e);
            }
        } else {
            d->reheat_since_ms = 0;
        }
    }

    /* ---- Abkuehlen wieder verlassen, wenn nachgeheizt wird (M6) ---- */
    if (d->phase == SSC_PH_COOLDOWN && d->r5_fill > 12 && (T - R5(r5_t, 12)) >= 0.8f) {
        d->phase = d->gang ? SSC_PH_GANG : SSC_PH_HEATUP;
        d->end_sent = false;
    }

    /* ---- Ende ---- */
    if (!d->end_sent) {
        bool fall10 = d->r5_fill > 119 && (T - R5(r5_t, 119)) <= -2.0f;
        bool fall2  = d->r5_fill > 24  && (T - R5(r5_t, 24)) < 0.0f;
        bool end = false;
        if (d->was_hot) {
            if (T <= d->end_t_abs && fall2) end = true;
            if (T <= d->peak_t - d->end_drop_from_peak && T <= 60.0f && fall10) end = true;
        } else if (t - d->sess_start_ms >= 3UL * 3600000UL) {
            end = true;   /* nie heiss geworden - vergessene Test-Session */
        }
        if (t - d->sess_start_ms >= d->max_session_ms) end = true;
        if (end) {
            ev_basic(&e, SSC_EV_AUTO_END, 0, d->gang, t, T, rh);
            e.t_start_ms = d->sess_start_ms;
            e.tpk = d->peak_t; e.rhpk = d->peak_rh;
            n = emit(out, n, max, &e);
            d->phase = SSC_PH_COOLDOWN;
            d->end_sent = true;   /* nur einmal melden (Recorder entscheidet) */
        }
    }

    /* ---- RH-Plausibilitaet (blinder Fuehler) ---- */
    if (d->r5_fill >= SSC_DET_R5_N && (t - d->rh_eval_ms) >= 60000UL) {
        d->rh_eval_ms = t;
        float tmin = 1e9f, tmax = -1e9f, hmin = 1e9f, hmax = -1e9f;
        for (uint8_t k = 0; k < SSC_DET_R5_N; k++) {
            float a = R5(r5_t, k), b = R5(r5_rh, k);
            if (a < tmin) tmin = a;
            if (a > tmax) tmax = a;
            if (b < hmin) hmin = b;
            if (b > hmax) hmax = b;
        }
        /* Erwartete RH-Aenderung bei konstanter abs. Feuchte ueber den
         * T-Hub des Fensters. Ein gesunder Fuehler zeigt mindestens ein
         * Viertel davon (meist viel mehr, weil Leute schwitzen/giessen). */
        float rh_mid = 0.5f * (hmin + hmax);
        float expect = rh_mid * (1.0f - es_hpa(tmin) / es_hpa(tmax));
        if (!d->rh_suspect && tmin >= 55.0f && (tmax - tmin) >= 2.5f &&
            expect >= 2.0f && (hmax - hmin) < 0.25f * expect) {
            d->rh_suspect = true;
            ev_basic(&e, SSC_EV_RH_SUSPECT, 0, d->gang, t, T, rh);
            n = emit(out, n, max, &e);
        } else if (d->rh_suspect && (hmax - hmin) >= 3.0f) {
            d->rh_suspect = false;
            ev_basic(&e, SSC_EV_RH_OK, 0, d->gang, t, T, rh);
            n = emit(out, n, max, &e);
        }
    }
    return n;
}

int ssc_det_update(ssc_det_t *d, uint32_t t, float T, float rh, bool valid,
                   ssc_det_event_t *out, int max) {
    int n = 0;
    if (d->have_last && (int32_t)(t - d->last_ms) < 0) return 0;  /* rueckwaerts */
    /* Grosse Luecke (Resume/Sensorausfall): Akkus neu starten, Ringe behalten */
    if (d->have_last && t - d->last_ms > 30000UL) {
        d->s1_n = 0; d->s5_n = 0;
        d->s1_t0 = t; d->s5_t0 = t;
    }
    d->last_ms = t;
    if (!d->have_last) { d->have_last = true; d->s1_t0 = t; d->s5_t0 = t; }

    if (!valid || isnan(T) || isnan(rh)) {
        /* Fuehler tot: Session darf trotzdem enden (Review H3) - nach
         * 30 min ohne gueltigen Wert oder bei Ueberschreiten der Maximaldauer. */
        if (d->session && !d->end_sent &&
            ((t - d->last_valid_ms) > 30UL * 60000UL || (t - d->sess_start_ms) >= d->max_session_ms)) {
            ssc_det_event_t e;
            ev_basic(&e, SSC_EV_AUTO_END, 0, d->gang, t, NAN, NAN);
            e.t_start_ms = d->sess_start_ms;
            e.tpk = d->peak_t; e.rhpk = d->peak_rh;
            d->end_sent = true;
            d->phase = SSC_PH_COOLDOWN;
            return emit(out, 0, max, &e);
        }
        return 0;
    }
    d->last_valid_ms = t;

    if (d->session) {
        if (T > d->peak_t)  d->peak_t = T;
        if (rh > d->peak_rh) d->peak_rh = rh;
    }

    /* 1-s-Mittel */
    d->s1_sum_t += T; d->s1_sum_rh += rh; d->s1_n++;
    if (t - d->s1_t0 >= 1000UL) {
        float mt = d->s1_sum_t / d->s1_n, mr = d->s1_sum_rh / d->s1_n;
        d->r1_t[d->r1_head] = mt; d->r1_rh[d->r1_head] = mr;
        d->r1_ah[d->r1_head] = ssc_abs_humidity(mt, mr);
        d->r1_head = (uint8_t)((d->r1_head + 1) % SSC_DET_R1_N);
        if (d->r1_fill < SSC_DET_R1_N) d->r1_fill++;
        d->s1_sum_t = d->s1_sum_rh = 0; d->s1_n = 0;
        d->s1_t0 = t;
        n = tick1(d, t, out, n, max);
    }
    /* 5-s-Mittel */
    d->s5_sum_t += T; d->s5_sum_rh += rh; d->s5_n++;
    if (t - d->s5_t0 >= 5000UL) {
        float mt = d->s5_sum_t / d->s5_n, mr = d->s5_sum_rh / d->s5_n;
        d->r5_t[d->r5_head] = mt; d->r5_rh[d->r5_head] = mr;
        d->r5_ah[d->r5_head] = ssc_abs_humidity(mt, mr);
        d->r5_head = (uint8_t)((d->r5_head + 1) % SSC_DET_R5_N);
        if (d->r5_fill < SSC_DET_R5_N) d->r5_fill++;
        d->s5_sum_t = d->s5_sum_rh = 0; d->s5_n = 0;
        d->s5_t0 = t;
        n = tick5(d, t, out, n, max);
    }
    return n;
}
