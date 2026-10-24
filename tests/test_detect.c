/* Host-Test fuer ssc_detect: spielt Session-CSVs durch und prueft
 * synthetische Grenzfaelle. Build+Run: make -C tests                   */
#include "../SenseCAP_Indicator_RP2040/src/ssc/ssc_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int n_aufguss, n_luef, n_gang, n_start, n_end, n_suspect;
                 uint32_t luef_start[8], luef_end[8], auf_t[64]; uint8_t auf_gang[64]; } res_t;

static void handle(ssc_det_t *d, ssc_det_event_t *ev, int ne, res_t *r, int verbose, int auto_mode) {
    for (int i = 0; i < ne; i++) {
        ssc_det_event_t *e = &ev[i];
        if (verbose)
            printf("  %7.1fmin %-14s no=%u gang=%u start=%.1fmin end=%.1fmin T0=%.1f Tpk=%.1f AH0=%.1f AHpk=%.1f\n",
                   e->t_peak_ms / 60000.0, ssc_ev_name(e->kind), e->no, e->gang,
                   e->t_start_ms / 60000.0, e->t_end_ms / 60000.0, e->t0, e->tpk, e->ah0, e->ahpk);
        switch (e->kind) {
        case SSC_EV_AUTO_START: r->n_start++; if (auto_mode) ssc_det_session_begin(d, e->t_start_ms, false); break;
        case SSC_EV_AUTO_END: {
            r->n_end++;
            ssc_det_event_t fe[4]; int nf = ssc_det_finish(d, fe, 4);
            ssc_det_session_end(d);
            handle(d, fe, nf, r, verbose, auto_mode);
            break; }
        case SSC_EV_AUFGUSS:    if (r->n_aufguss < 64) { r->auf_t[r->n_aufguss] = e->t_start_ms; r->auf_gang[r->n_aufguss] = e->gang; } r->n_aufguss++; break;
        case SSC_EV_LUEFTEN_START: if (r->n_luef < 8) r->luef_start[r->n_luef] = e->t_start_ms; break;
        case SSC_EV_LUEFTEN_END:   if (r->n_luef < 8) r->luef_end[r->n_luef] = e->t_end_ms; r->n_luef++; break;
        case SSC_EV_GANG_START: r->n_gang++; break;
        case SSC_EV_RH_SUSPECT: r->n_suspect++; break;
        }
    }
}

/* CSV v1 (t_elapsed_s, ggf. doppelt bei 2 Hz) oder v2 (t_ms) */
static res_t replay_csv(const char *path, int manual, int verbose) {
    res_t r; memset(&r, 0, sizeof(r));
    FILE *f = fopen(path, "r");
    if (!f) { printf("  kann %s nicht oeffnen\n", path); g_fail++; return r; }
    char line[256];
    static float T[100000], H[100000]; static double ts[100000]; int n = 0;
    int v2 = 0;
    if (fgets(line, sizeof line, f)) v2 = strncmp(line, "t_ms", 4) == 0;
    while (fgets(line, sizeof line, f) && n < 100000) {
        double t; float a, b;
        if (sscanf(line, "%lf,%f,%f", &t, &a, &b) == 3) { ts[n] = t; T[n] = a; H[n] = b; n++; }
    }
    fclose(f);
    int dup = (!v2 && n > 2 && ts[1] == ts[0]);
    ssc_det_t d; ssc_det_init(&d);
    ssc_det_event_t ev[8];
    for (int i = 0; i < n; i++) {
        uint32_t tms = v2 ? (uint32_t)ts[i] : dup ? (uint32_t)(i * 500) : (uint32_t)(ts[i] * 1000);
        tms += 3600000UL; /* Startoffset, damit Ruecksuchen nicht unter 0 faellt */
        if (i == 0 && manual) ssc_det_session_begin(&d, tms, true);
        int ne = ssc_det_update(&d, tms, T[i], H[i], 1, ev, 8);
        handle(&d, ev, ne, &r, verbose, !manual);
    }
    if (d.session) { int ne = ssc_det_finish(&d, ev, 8); handle(&d, ev, ne, &r, verbose, 0); }
    return r;
}

/* Synthetischer Abend: kalt -> Aufheizen -> 2 Gaenge mit je 3 Kellen ->
 * Lueften -> Abkuehlen. Optional Heizer-Artefakt + Anhauchen kalt.     */
typedef struct { float T, ah; } st_t;
static float rh_from_ah(float T, float ah) {
    float es = 6.1121f * expf((18.678f - T / 234.5f) * (T / (257.14f + T)));
    float rh = ah * (273.15f + T) / 216.7f / es * 100.0f;
    return rh > 100 ? 100 : rh;
}
static res_t synth(int verbose, int breath_only) {
    res_t r; memset(&r, 0, sizeof(r));
    ssc_det_t d; ssc_det_init(&d);
    ssc_det_event_t ev[8];
    float T = 18.0f, ah = 9.0f;
    srand(1);
    for (uint32_t s = 0; s < 6 * 3600 * 2; s++) {  /* 6 h @ 2 Hz */
        double tm = s / 120.0;   /* Minuten */
        if (breath_only) {
            T = 23.0f; ah = 9.0f;
            if (fmod(tm, 20.0) > 10.0 && fmod(tm, 20.0) < 10.3) ah = 25.0f; /* anhauchen */
        } else if (tm < 20) { T = 18; ah = 9; }
        else if (tm < 70) { T = 18 + (tm - 20) * 1.4f; if (T > 85) T = 85; ah = 9 + (tm - 20) * 0.22f; }
        else if (tm < 200) {
            double g = fmod(tm - 70, 65.0);  /* 65-min-Zyklus: 30 min Gang, 15 Lueften, 20 Nachheizen */
            if (g < 30) {
                T = 85 - (g > 10 && g < 20 ? (g - 10) * 0.6f : (g >= 20 ? 6 - (g - 20) * 0.1f : 0));
                ah = 20 + g * 0.8f;
                for (int k = 0; k < 3; k++) { double p = 10 + k * 2.2; if (g > p && g < p + 1.5) ah += 30 * (1 - (g - p) / 1.5); }
            } else if (g < 45) { T = 79 - (g - 30) * 1.0f; if (T < 62) T = 62; ah = 18; }
            else { T = 64 + (g - 45) * 1.6f; if (T > 85) T = 85; ah = 18 + (g - 45) * 0.2f; }
        } else { T = 85 - (tm - 200) * 0.45f; if (T < 20) T = 20; ah = 12; }
        float tt = T + ((rand() % 100) - 50) / 1000.0f;
        float h  = rh_from_ah(tt, ah) + ((rand() % 100) - 50) / 500.0f;
        /* Heizer-Artefakt bei 30 min (kalt, +3 K / -5 %RH fuer 60 s) */
        if (!breath_only && tm > 30 && tm < 31) { tt += 3; h -= 5; }
        int ne = ssc_det_update(&d, 3600000UL + s * 500UL, tt, h, 1, ev, 8);
        handle(&d, ev, ne, &r, verbose, 1);
    }
    return r;
}

int main(int argc, char **argv) {
    int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    const char *dir = "../sessions_extracted/";
    char p[256];
    printf("== S20260511_165849 (3 Aufguesse, Lueften 20-44 min)\n");
    snprintf(p, sizeof p, "%sS20260511_165849.csv", dir);
    res_t r = replay_csv(p, 1, verbose);
    CHECK(r.n_aufguss == 3, "aufguss=%d erwartet 3", r.n_aufguss);
    CHECK(r.n_luef == 1, "lueften=%d erwartet 1", r.n_luef);
    if (r.n_luef >= 1) {
        double ls = (r.luef_start[0] - 3600000UL) / 60000.0, le = (r.luef_end[0] - 3600000UL) / 60000.0;
        CHECK(ls > 18.5 && ls < 22.0, "lueften-start %.1f min (erwartet ~20)", ls);
        CHECK(le > 42.0 && le < 46.0, "lueften-ende %.1f min (erwartet ~44)", le);
    }
    CHECK(r.n_gang == 2, "gaenge=%d erwartet 2", r.n_gang);
    CHECK(r.n_end == 0, "auto_end=%d erwartet 0 (Lueften != Ende)", r.n_end);

    printf("== S20260427_180047 (3 Aufguesse laut README)\n");
    snprintf(p, sizeof p, "%sS20260427_180047.csv", dir);
    r = replay_csv(p, 1, verbose);
    CHECK(r.n_aufguss == 3, "aufguss=%d erwartet 3", r.n_aufguss);
    CHECK(r.n_end == 0, "auto_end=%d", r.n_end);

    const char *others[] = { "S20260427_164658", "S20260427_191623", "S20260427_201327",
                             "S20260504_170646", "S20260504_191051", "S20260511_175812",
                             "S20260511_190057", "S20260512_204220" };
    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        snprintf(p, sizeof p, "%s%s.csv", dir, others[i]);
        r = replay_csv(p, 1, verbose);
        printf("== %s: aufguss=%d lueften=%d gaenge=%d end=%d rh_suspect=%d\n", others[i],
               r.n_aufguss, r.n_luef, r.n_gang, r.n_end, r.n_suspect);
        CHECK(r.n_end == 0, "%s: kein auto-ende in heisser kurz-session", others[i]);
    }
    /* Blinder Fuehler muss erkannt werden */
    snprintf(p, sizeof p, "%sS20260512_204220.csv", dir);
    r = replay_csv(p, 1, 0);
    CHECK(r.n_suspect >= 1, "12.05. (RH starr) nicht als RH_SUSPECT erkannt");
    snprintf(p, sizeof p, "%sS20260511_190057.csv", dir);
    r = replay_csv(p, 1, 0);
    printf("   (11.05. 19:00 rh_suspect=%d - grenzwertig, T bewegt sich kaum)\n", r.n_suspect);

    /* Kalte Leerlauf-Aufnahmen duerfen nie eine Session starten */
    const char *cold[] = { "S20260510_093048", "S20260511_131615" };
    for (unsigned i = 0; i < 2; i++) {
        snprintf(p, sizeof p, "%s%s.csv", dir, cold[i]);
        r = replay_csv(p, 0, verbose);
        printf("== %s (kalt, auto): start=%d aufguss=%d\n", cold[i], r.n_start, r.n_aufguss);
        CHECK(r.n_start == 0 && r.n_aufguss == 0, "%s: Fehlstart im Leerlauf", cold[i]);
    }

    printf("== synthetischer Abend (auto)\n");
    r = synth(verbose, 0);
    printf("   start=%d gaenge=%d aufguss=%d lueften=%d end=%d\n", r.n_start, r.n_gang, r.n_aufguss, r.n_luef, r.n_end);
    CHECK(r.n_start == 1, "auto-start=%d erwartet 1", r.n_start);
    CHECK(r.n_end == 1, "auto-ende=%d erwartet 1", r.n_end);
    CHECK(r.n_aufguss == 6, "aufguss=%d erwartet 6", r.n_aufguss);
    CHECK(r.n_luef == 2, "lueften=%d erwartet 2", r.n_luef);
    CHECK(r.n_gang == 3, "gaenge=%d erwartet 3", r.n_gang);

    printf("== Anhauchen bei Raumtemperatur (auto)\n");
    r = synth(verbose, 1);
    CHECK(r.n_start == 0 && r.n_aufguss == 0, "Anhauchen erzeugte start=%d aufguss=%d", r.n_start, r.n_aufguss);

    printf(g_fail ? "\n%d FEHLER\n" : "\nALLE TESTS OK\n", g_fail);
    return g_fail ? 1 : 0;
}
