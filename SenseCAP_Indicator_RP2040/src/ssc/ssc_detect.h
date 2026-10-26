/*
 * ssc_detect.h - Sauna-Abend-Detektor (Auto-Modus), reines C, kein malloc.
 *
 * Wird auf dem RP2040 pro Sample (2 Hz) aufgerufen, ist aber auch auf dem
 * Linux-Host kompilierbar (tests/test_detect.c spielt echte Session-CSVs
 * durch). Arbeitet ausschliesslich auf den uebergebenen Sample-Zeitstempeln
 * (t_ms), nie auf millis() - dadurch funktionieren Replay und Simulation
 * mit beliebiger Geschwindigkeit.
 *
 * Modell eines Saunaabends:
 *   IDLE -> HEATUP (Kabine heizt auf) -> GANG n (Aufguesse = einzelne
 *   Kellen/Ausgiessungen, erkannt am Sprung der absoluten Feuchte) ->
 *   LUEFTEN (Tuer/Fenster auf: T faellt minutenlang, Feuchte faellt) ->
 *   Nachheizen -> GANG n+1 ... -> COOLDOWN (Ofen aus) -> ENDE.
 */
#ifndef SSC_DETECT_H
#define SSC_DETECT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSC_DET_VERSION 1

typedef enum {
    SSC_PH_IDLE = 0,     /* kalt, keine Session                        */
    SSC_PH_HEATUP,       /* Session laeuft, Kabine heizt auf           */
    SSC_PH_GANG,         /* Kabine heiss, Saunagang                    */
    SSC_PH_LUEFTEN,      /* Lueften erkannt                            */
    SSC_PH_COOLDOWN,     /* Abkuehlen erkannt, Ende steht bevor        */
} ssc_phase_t;

typedef enum {
    SSC_EV_NONE = 0,
    SSC_EV_AUTO_START,     /* Aufheizen erkannt -> Session starten       */
    SSC_EV_GANG_START,     /* neuer Gang (nach Aufheizen bzw. Lueften)   */
    SSC_EV_AUFGUSS,        /* Aufguss (Kelle) abgeschlossen, mit Stats   */
    SSC_EV_LUEFTEN_START,
    SSC_EV_LUEFTEN_END,
    SSC_EV_AUTO_END,       /* Abkuehlen erkannt -> Session beenden       */
    SSC_EV_RH_SUSPECT,     /* Feuchtekanal reagiert nicht (blind?)       */
    SSC_EV_RH_OK,          /* Feuchtekanal wieder plausibel              */
} ssc_ev_kind_t;

typedef struct {
    uint8_t  kind;         /* ssc_ev_kind_t */
    uint8_t  no;           /* laufende Nummer pro Art (Aufguss 1,2,..)   */
    uint8_t  gang;         /* Gang-Nummer zum Zeitpunkt                  */
    uint32_t t_start_ms;
    uint32_t t_peak_ms;
    uint32_t t_end_ms;
    float    t0, tpk;      /* Temperatur Start / Peak (bzw. Min bei L.)  */
    float    rh0, rhpk;
    float    ah0, ahpk;    /* absolute Feuchte g/m3                      */
} ssc_det_event_t;

/* Ring der letzten 10 min in 5-s-Schritten (Mittelwerte). */
#define SSC_DET_R5_N   120
/* Ring der letzten 40 s in 1-s-Schritten (abs. Feuchte). */
#define SSC_DET_R1_N   40

typedef struct {
    /* Konfiguration (Defaults per ssc_det_init) */
    float start_t_min;        /* 40 °C  */
    float start_rise_5min;    /* +3 K in 5 min */
    float hot_t;              /* 55 °C: darueber gilt Kabine als heiss    */
    float pour_dah;           /* +10 g/m3 innerhalb 30 s = Aufguss        */
    float end_t_abs;          /* 45 °C  */
    float end_drop_from_peak; /* 25 K   */
    uint32_t max_session_ms;  /* 8 h    */

    /* Zustand */
    uint8_t  phase;
    bool     session;         /* vom Recorder gesetzt (manuell/auto)      */
    bool     manual;          /* Session manuell gestartet                */
    bool     was_hot;
    uint32_t sess_start_ms;
    uint32_t last_ms;
    bool     have_last;

    /* Glaettung */
    float    ema_t, ema_rh;
    bool     ema_init;

    /* 1-s-Akkumulator */
    uint32_t s1_t0;
    float    s1_sum_t, s1_sum_rh; uint16_t s1_n;
    float    r1_ah[SSC_DET_R1_N], r1_t[SSC_DET_R1_N], r1_rh[SSC_DET_R1_N];
    uint8_t  r1_head, r1_fill;

    /* 5-s-Akkumulator */
    uint32_t s5_t0;
    float    s5_sum_t, s5_sum_rh, s5_sum_ah; uint16_t s5_n;
    float    r5_t[SSC_DET_R5_N], r5_rh[SSC_DET_R5_N], r5_ah[SSC_DET_R5_N];
    uint8_t  r5_head, r5_fill;

    /* Aufguss-Tracking */
    bool     pour_active;
    ssc_det_event_t pour;
    float    pour_min_since_peak; uint32_t pour_min_since_peak_ms;
    uint32_t last_pour_end_ms;
    uint8_t  n_pour;

    /* Lueften */
    ssc_det_event_t luef;
    uint32_t reheat_since_ms;
    uint32_t luef_cand_ms;      /* 0 = kein Kandidat */
    uint8_t  n_luef;

    /* Gaenge / Heizen */
    uint8_t  gang;
    uint32_t heat_hold_ms;      /* Plateau-Erkennung */
    uint32_t hot_since_ms;
    uint32_t cool_since_ms;
    uint32_t ended_ms;          /* Ende der letzten Session (Restart-Sperre) */
    bool     ever_ended;
    bool     hot_armed;          /* Hot-Trigger erst wieder nach Abkuehlen */
    uint32_t last_valid_ms;      /* letztes gueltiges Sample */
    bool     end_sent;

    /* Session-Stats */
    float    peak_t, peak_rh;
    uint32_t heatup_start_ms, heatup_end_ms;

    /* RH-Plausibilitaet */
    bool     rh_suspect;
    uint32_t rh_eval_ms;
} ssc_det_t;

void ssc_det_init(ssc_det_t *d);

/* Recorder meldet Session-Start/-Ende (manuell oder als Folge von
 * AUTO_START/AUTO_END). t_ms = Sample-Zeitbasis.                      */
void ssc_det_session_begin(ssc_det_t *d, uint32_t t_ms, bool manual);
void ssc_det_session_end(ssc_det_t *d);
/* Vor session_end aufrufen: schliesst laufenden Aufguss / Lueften ab
 * und liefert die Events dafuer.                                      */
int  ssc_det_finish(ssc_det_t *d, ssc_det_event_t *out, int max_ev);

/* Ein Sample verarbeiten. valid=false bei Sensorfehler (Sample wird
 * ignoriert, Zeit laeuft weiter). Schreibt bis max_ev Events nach out,
 * Rueckgabe = Anzahl Events.                                          */
int  ssc_det_update(ssc_det_t *d, uint32_t t_ms, float t, float rh,
                    bool valid, ssc_det_event_t *out, int max_ev);

/* Hilfsfunktionen (auch fuer UI/Stats) */
float ssc_abs_humidity(float t_c, float rh_pct);   /* g/m3 */
float ssc_dewpoint(float t_c, float rh_pct);        /* °C  */
const char *ssc_ev_name(uint8_t kind);

#ifdef __cplusplus
}
#endif
#endif
