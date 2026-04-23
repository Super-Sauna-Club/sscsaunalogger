/*
 * ui_sauna.c - Sauna Logger (supersauna.club) komplettes UI.
 *
 * Branding:   "SAUNA LOGGER"  (kleingedruckt: supersauna.club)
 * Auflösung:  480 x 480 (D1 SCREEN_GX)
 * Theme:      dark, gold-akzent (theme.h)
 *
 * Screens:
 *   HOME      - live dashboard (temp, rh, co2, peak, vorraum) +
 *               "SESSION STARTEN" + letzte sessions + settings/history
 *   LIVE      - running session: live-kurve, aufguss-button, beenden
 *   SUMMARY   - session-ende: form (operator, aufguss, teilnehmer, notizen)
 *   HISTORY   - scrollable liste aller sessions
 *   DETAIL    - eine session mit voller kurve (von SD via RP2040)
 *   SETTINGS  - wifi, mariadb, http-endpoint, operator-presets, info
 *
 * WICHTIG: Alle LVGL-Aufrufe aus Event-Handlern laufen ueber
 * lv_port_sem_take/give, weil der esp-event-loop in eigenem Task
 * ausgefuehrt wird und LVGL single-threaded ist. Ohne Lock → race
 * und Crash innerhalb weniger Minuten.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "lvgl.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <sys/time.h>
#include <stdint.h>

#include "view_data.h"
#include "ui_sauna.h"
#include "../app_version.h"
#include "../lv_port.h"
#include "../ui/theme.h"

/* Forward-decl aus indicator_session.h (wird hier gebraucht um edit-
 * button waehrend einer live-session zu blocken). */
bool indicator_session_is_active(void);

static const char *TAG = "UI_SAUNA";

extern esp_event_loop_handle_t view_event_handle;

/* ======================================================================= */
/*   Forward-decls                                                          */
/* ======================================================================= */

static void build_home(void);
static void build_live(void);
static void build_summary(void);
static void build_history(void);
static void build_detail(void);
static void build_settings(void);
static void home_update_recent(const struct view_data_session_list *list);
static void history_apply(const struct view_data_session_list *L);
static void on_history_row_clicked(lv_event_t *e);
static void on_detail_delete_clicked(lv_event_t *e);
static void on_detail_edit_clicked(lv_event_t *e);
static void rebuild_operator_dropdown(void);
typedef void (*confirm_cb_t)(void *user_data);
static void show_confirm(const char *title, const char *body,
                         const char *confirm_label,
                         confirm_cb_t on_ok, void *user_data);

/* ======================================================================= */
/*   Screen-Objekte                                                          */
/* ======================================================================= */

static lv_obj_t *scr_home;
static lv_obj_t *scr_live;
static lv_obj_t *scr_summary;
static lv_obj_t *scr_history;
static lv_obj_t *scr_detail;
static lv_obj_t *scr_settings;

/* HOME-Labels (werden vom sensor/session-event aktualisiert) */
static lv_obj_t *home_sauna_temp_val;     /* Legacy: wird set aber ist hidden */
static lv_obj_t *home_sauna_rh_val;       /* Legacy: wird set aber ist hidden */
static lv_obj_t *home_sauna_temp_int;     /* Integer-Teil (F_HUGE 80pt) */
static lv_obj_t *home_sauna_temp_frac;    /* ".X" + " °C" (F_XL 48pt)    */
static lv_obj_t *home_sauna_rh_int;
static lv_obj_t *home_sauna_rh_frac;
static lv_obj_t *home_vorraum_temp_val;
static lv_obj_t *home_vorraum_rh_val;
static lv_obj_t *home_vorraum_co2_val;
static lv_obj_t *home_tvoc_val;
static lv_obj_t *home_clock_val;
static lv_obj_t *home_recent_list;
static lv_obj_t *home_status_pill;
static lv_obj_t *home_probe_badge;   /* DEV/AHT20-FB/SD-warn-Hinweis */

/* LIVE */
static lv_obj_t *live_temp_val;
static lv_obj_t *live_rh_val;
static lv_obj_t *live_peak_val;       /* erste Zeile PEAK: Temp */
static lv_obj_t *live_peak_val2;      /* zweite Zeile PEAK: RH */
static lv_obj_t *live_timer_val;
static lv_obj_t *live_aufguss_count_val;
static lv_obj_t *live_chart;
static lv_chart_series_t *live_ser_temp;
static lv_chart_series_t *live_ser_rh;
static lv_obj_t *live_aufguss_btn;
static lv_obj_t *live_start_btn;      /* READY -> session starten */
static lv_obj_t *live_stop_btn;       /* RUNNING -> session beenden (summary) */
static lv_obj_t *live_cancel_btn;     /* beide states -> abbrechen, zurueck home */
static lv_obj_t *live_status_label;   /* "BEREIT" / "SESSION LAEUFT" */
static bool     live_running = false;

static void live_set_ready_state(void);
static void live_set_running_state(void);
static void live_set_button_states(bool start_v, bool aufguss_v,
                                    bool stop_v, bool cancel_v);

/* SUMMARY */
static lv_obj_t *sum_operator_dd;
static lv_obj_t *sum_aufguss_ta;
static lv_obj_t *sum_aufg_count_label;
static int       sum_aufg_count_n = 0;
static lv_obj_t *sum_participants_sb;
static lv_obj_t *sum_participants_label;
static int       sum_participants_n = 0;
static lv_obj_t *sum_notes_ta;
static struct view_data_session_meta sum_current_meta;
static bool      sum_edit_mode = false;  /* true = bestehende session editieren */
static lv_obj_t *sum_title_label = NULL;  /* header-titel, wird in edit-mode umbenannt */
static lv_obj_t *sum_save_btn_label = NULL;
static lv_obj_t *sum_discard_btn_label = NULL;
/* Index (in hist_cached[]) der session die gerade auf dem detail-screen
 * angezeigt/bearbeitet wird. -1 = keine. */
static int sum_edit_hist_idx = -1;
static lv_obj_t *sum_meta_summary;   /* kleiner infoblock oben */

/* HISTORY */
static lv_obj_t *hist_list;
static struct view_data_session_meta hist_cached[32];
static uint16_t hist_cache_count;

/* DETAIL */
static lv_obj_t *detail_title;
static lv_obj_t *detail_sub;
static lv_obj_t *detail_chart;
static lv_obj_t *detail_spinner;      /* loading-indikator */
static lv_chart_series_t *detail_ser_temp;
static lv_chart_series_t *detail_ser_rh;
static lv_chart_series_t *detail_ser_aufg;  /* aufguss-marker als spikes */
/* 3600 points = 1h bei 1-Hz-sampling. Bei laengeren sessions macht
 * die decimation im akkumulator den step. */
#define DETAIL_CHART_POINTS 3600

/* Akkumulator fuer den readback: samples kommen in chunks rein, wir
 * sammeln alle hier und schreiben sie EINMAL am ende in den chart
 * (via DONE-event). Das umgeht LVGL-quirks mit repeated set_point_count. */
static struct {
    int16_t *temps;        /* PSRAM, cap slots */
    int16_t *rhs;
    uint8_t *markers;
    uint16_t count;
    uint16_t cap;
    char     sid[SSC_SESSION_ID_LEN];
} s_detail_buf;

static lv_obj_t *sum_keyboard;      /* bottom-sliding keyboard fuer Summary */
static lv_obj_t *set_keyboard;      /* bottom-sliding keyboard fuer Settings */

/* SETTINGS (nur die eingabefelder die wir dynamisch lesen/schreiben) */
static lv_obj_t *set_wifi_info;
static lv_obj_t *set_wifi_ssid_ta;
static lv_obj_t *set_wifi_pw_ta;
static lv_obj_t *set_http_url_ta;
static lv_obj_t *set_http_token_ta;
static lv_obj_t *set_http_enabled_sw;
static lv_obj_t *set_db_host_ta;
static lv_obj_t *set_db_port_sb;
static lv_obj_t *set_db_user_ta;
static lv_obj_t *set_db_pw_ta;
static lv_obj_t *set_db_database_ta;
static lv_obj_t *set_db_table_ta;
static lv_obj_t *set_db_enabled_sw;
static lv_obj_t *set_operators_ta;   /* 1 kürzel pro zeile */

/* ======================================================================= */
/*   Style-Helper                                                            */
/* ======================================================================= */

/* ui_font_font4 ist extern definiert in main/ui/ui_font_font4.c -
 * line_height 102px (ca. 80pt), aber enthaelt NUR Ziffern 0-9.    */
LV_FONT_DECLARE(ui_font_font4);
/* Custom 72pt montserrat (main/ui/ssc_montserrat_72.c) - ASCII + ° */
LV_FONT_DECLARE(ssc_montserrat_72);

static const lv_font_t *F_HUGE;  /* 80pt ziffern-only fuer kabine */
static const lv_font_t *F_XL;    /* peak-zahl ~48-64pt */
static const lv_font_t *F_LG;    /* wert-karten ~36pt */
static const lv_font_t *F_MD;    /* action-buttons ~20pt */
static const lv_font_t *F_SM;    /* body ~14-16pt */
static const lv_font_t *F_XS;    /* labels ~12-14pt */

static void font_init(void) {
    /* Split-ansatz: integer-teil als font4 (80pt, ziffer-only) GROSS,
     * dezimal+unit separat F_MD klein daneben. Kein zoom-hack mehr.
     * Gibt thermostat-style: "24" riesig, ".5 °C" klein rechts.       */
    F_HUGE = &ui_font_font4;
    F_XL   = &lv_font_montserrat_48;
    F_LG   = &lv_font_montserrat_36;
    F_MD   = &lv_font_montserrat_20;
    F_SM   = &lv_font_montserrat_16;
    F_XS   = &lv_font_montserrat_14;
}

static void style_screen(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, SSC_C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 12, 0);
    lv_obj_set_style_text_color(scr, SSC_C_TEXT, 0);
    lv_obj_set_style_text_font(scr, F_SM, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_card(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, SSC_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, SSC_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, SSC_C_BORDER, 0);
    lv_obj_set_style_pad_all(obj, 10, 0);
}

static void style_primary_btn(lv_obj_t *btn) {
    lv_obj_set_style_bg_color(btn, SSC_C_ACCENT, 0);
    lv_obj_set_style_bg_color(btn, SSC_C_ACCENT_HOV, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, SSC_RADIUS_BTN, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x0a0a0a), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 8, 0);
    lv_obj_set_style_shadow_color(btn, SSC_C_ACCENT, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, 0);
}

static void style_ghost_btn(lv_obj_t *btn) {
    lv_obj_set_style_bg_color(btn, SSC_C_ELEVATED, 0);
    lv_obj_set_style_bg_color(btn, SSC_C_OVERLAY, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, SSC_RADIUS_BTN, 0);
    lv_obj_set_style_text_color(btn, SSC_C_TEXT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, SSC_C_BORDER, 0);
}

static void style_icon_btn(lv_obj_t *btn) {
    lv_obj_set_style_bg_color(btn, SSC_C_ELEVATED, 0);
    lv_obj_set_style_bg_color(btn, SSC_C_OVERLAY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_text_color(btn, SSC_C_TEXT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, SSC_C_BORDER, 0);
}

static void label_muted(lv_obj_t *lbl) {
    lv_obj_set_style_text_color(lbl, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lbl, F_XS, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
}

static void label_accent(lv_obj_t *lbl) {
    lv_obj_set_style_text_color(lbl, SSC_C_ACCENT, 0);
}

/* ======================================================================= */
/*   Keyboard-Binding                                                        */
/* ======================================================================= */

static void kb_ta_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
    if (!kb) return;

    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(kb);
        /* Falls die Textarea hinter dem Keyboard verschwindet,
         * im Scroll-Container sichtbar machen. */
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY
               || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *kb_create(lv_obj_t *screen) {
    lv_obj_t *kb = lv_keyboard_create(screen);
    lv_obj_set_size(kb, 480, 200);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, SSC_C_ELEVATED, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(kb, SSC_C_TEXT, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    return kb;
}

static void kb_attach(lv_obj_t *ta, lv_obj_t *kb) {
    if (!ta || !kb) return;
    lv_obj_add_event_cb(ta, kb_ta_event, LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(ta, kb_ta_event, LV_EVENT_DEFOCUSED, kb);
    lv_obj_add_event_cb(ta, kb_ta_event, LV_EVENT_READY,     kb);
    lv_obj_add_event_cb(ta, kb_ta_event, LV_EVENT_CANCEL,    kb);
}

static void fmt_duration(char *buf, size_t n, uint32_t seconds) {
    uint32_t m = seconds / 60, s = seconds % 60;
    snprintf(buf, n, "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
}

static void fmt_datetime_short(char *buf, size_t n, time_t ts) {
    struct tm tmv;
    localtime_r(&ts, &tmv);
    strftime(buf, n, "%d.%m. %H:%M", &tmv);
}

static void fmt_clock(char *buf, size_t n) {
    time_t now = 0; time(&now);
    struct tm tmv; localtime_r(&now, &tmv);
    strftime(buf, n, "%d.%m.  %H:%M", &tmv);
}

/* ======================================================================= */
/*   Reusable: statkachel "GROSSE ZAHL + LABEL + UNIT"                      */
/* ======================================================================= */

static lv_obj_t *big_stat_card(lv_obj_t *parent, const char *topic,
                                const char *unit, lv_color_t color,
                                lv_obj_t **out_val,
                                lv_coord_t w, lv_coord_t h) {
    lv_obj_t *c = lv_obj_create(parent);
    style_card(c);
    lv_obj_set_size(c, w, h);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_label_create(c);
    lv_label_set_text(top, topic);
    label_muted(top);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *val = lv_label_create(c);
    lv_label_set_text(val, "-");
    lv_obj_set_style_text_font(val, F_LG, 0);
    lv_obj_set_style_text_color(val, color, 0);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, -6);
    if (out_val) *out_val = val;

    lv_obj_t *u = lv_label_create(c);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_color(u, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(u, F_SM, 0);
    lv_obj_align(u, LV_ALIGN_BOTTOM_RIGHT, 0, -10);
    return c;
}

static lv_obj_t *mini_stat_card(lv_obj_t *parent, const char *topic,
                                 const char *unit, lv_obj_t **out_val,
                                 lv_coord_t w, lv_coord_t h) {
    lv_obj_t *c = lv_obj_create(parent);
    style_card(c);
    lv_obj_set_size(c, w, h);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(c, 8, 0);

    lv_obj_t *top = lv_label_create(c);
    lv_label_set_text(top, topic);
    lv_obj_set_style_text_color(top, SSC_C_TEXT_FAINT, 0);
    lv_obj_set_style_text_font(top, F_XS, 0);
    lv_obj_set_style_text_letter_space(top, 1, 0);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(row, 4, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "-");
    lv_obj_set_style_text_font(val, F_MD, 0);
    lv_obj_set_style_text_color(val, SSC_C_TEXT, 0);
    if (out_val) *out_val = val;

    lv_obj_t *u = lv_label_create(row);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_color(u, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(u, F_XS, 0);
    return c;
}

/* ======================================================================= */
/*   HOME                                                                    */
/* ======================================================================= */

static void on_home_start_clicked(lv_event_t *e) {
    ESP_LOGI(TAG, "on_home_start_clicked -> scr_live");
    live_set_ready_state();
    lv_scr_load_anim(scr_live, LV_SCR_LOAD_ANIM_FADE_ON, 240, 0, false);
}

static void on_home_history_clicked(lv_event_t *e) {
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0, portMAX_DELAY);
    lv_scr_load_anim(scr_history, LV_SCR_LOAD_ANIM_OVER_LEFT, 220, 0, false);
}

static void on_home_settings_clicked(lv_event_t *e) {
    lv_scr_load_anim(scr_settings, LV_SCR_LOAD_ANIM_OVER_TOP, 220, 0, false);
}

static void build_home(void) {
    scr_home = lv_obj_create(NULL);
    style_screen(scr_home);
    /* Reduziertes pad damit alles in 480 px reinpasst */
    lv_obj_set_style_pad_all(scr_home, 8, 0);

    /* ===== Header (36 px): Brand + Clock + Settings ===== */
    lv_obj_t *hdr = lv_obj_create(scr_home);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, 464, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *brand = lv_label_create(hdr);
    lv_label_set_text(brand, "SAUNA LOGGER");
    lv_obj_set_style_text_color(brand, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(brand, F_MD, 0);
    lv_obj_set_style_text_letter_space(brand, 4, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 0, -6);

    lv_obj_t *dom = lv_label_create(hdr);
    lv_label_set_text(dom, "supersauna.club");
    label_muted(dom);
    lv_obj_align(dom, LV_ALIGN_LEFT_MID, 0, 12);

    home_clock_val = lv_label_create(hdr);
    lv_label_set_text(home_clock_val, "--.--  --:--");
    /* 28pt: maximal ohne brand-text zu ueberlappen und noch in 44 px
     * header rein (vertikal).                                        */
    lv_obj_set_style_text_font(home_clock_val, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(home_clock_val, SSC_C_TEXT, 0);
    lv_obj_align(home_clock_val, LV_ALIGN_RIGHT_MID, -44, 0);

    lv_obj_t *btn_settings = lv_btn_create(hdr);
    style_icon_btn(btn_settings);
    lv_obj_set_size(btn_settings, 36, 36);
    lv_obj_align(btn_settings, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *gear = lv_label_create(btn_settings);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gear, F_MD, 0);
    lv_obj_center(gear);
    lv_obj_add_event_cb(btn_settings, on_home_settings_clicked,
                        LV_EVENT_CLICKED, NULL);

    /* ===== Kabinen-Box (100 px): Titel oben + zwei Spalten =====
     * Spalten: Temperatur (groß gold) | Feuchte (groß sage).
     * Labels ober den Zahlen, Einheit rechts neben der Zahl -
     * alles per Flex-Row, keine Überlappungen mehr.             */
    lv_obj_t *cabin = lv_obj_create(scr_home);
    style_card(cabin);
    /* 160 px - eine spur kompakter, start-button wird groesser. */
    lv_obj_set_size(cabin, 464, 160);
    lv_obj_align_to(cabin, hdr, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    lv_obj_clear_flag(cabin, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cabin, 8, 0);

    /* Header-Row: KABINE links, Status-Pill rechts, Badge darunter */
    lv_obj_t *cab_hdr = lv_obj_create(cabin);
    lv_obj_remove_style_all(cab_hdr);
    lv_obj_set_size(cab_hdr, LV_PCT(100), 18);
    lv_obj_align(cab_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Kombinierter header links: "KABINE (SHT3X: OK)" - probe-state
     * zeigt direkt im label statt als pill rechts. Sauberer look. */
    home_status_pill = lv_label_create(cab_hdr);
    lv_label_set_text(home_status_pill, "KABINE (SHT3X: OK)");
    lv_obj_set_style_text_color(home_status_pill, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(home_status_pill, F_XS, 0);
    lv_obj_set_style_text_letter_space(home_status_pill, 3, 0);
    lv_obj_align(home_status_pill, LV_ALIGN_LEFT_MID, 0, 0);

    /* Probe-Badge unsichtbar gemacht - Info ist jetzt in status_pill */
    home_probe_badge = lv_label_create(cabin);
    lv_label_set_text(home_probe_badge, "");
    lv_obj_add_flag(home_probe_badge, LV_OBJ_FLAG_HIDDEN);

    /* Zwei-Spalten-Body: exakt symmetrisch 50/50 mit vertikaler
     * Trennlinie in der Mitte. space-evenly statt space-between
     * fuer saubere gleichmaessige Verteilung.                    */
    lv_obj_t *cab_body = lv_obj_create(cabin);
    lv_obj_remove_style_all(cab_body);
    lv_obj_set_size(cab_body, LV_PCT(100), 128);
    lv_obj_align(cab_body, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(cab_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cab_body, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cab_body, LV_OBJ_FLAG_SCROLLABLE);

    /* Senkrechte Trennlinie in der Mitte */
    lv_obj_t *cab_sep = lv_obj_create(cabin);
    lv_obj_remove_style_all(cab_sep);
    lv_obj_set_size(cab_sep, 1, 120);
    lv_obj_align(cab_sep, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(cab_sep, SSC_C_BORDER, 0);
    lv_obj_set_style_bg_opa(cab_sep, LV_OPA_COVER, 0);

    /* Linke Spalte: Temperatur */
    lv_obj_t *col_temp = lv_obj_create(cab_body);
    lv_obj_remove_style_all(col_temp);
    lv_obj_set_size(col_temp, 220, 128);
    lv_obj_clear_flag(col_temp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *temp_cap = lv_label_create(col_temp);
    lv_label_set_text(temp_cap, "TEMPERATUR");
    label_muted(temp_cap);
    lv_obj_align(temp_cap, LV_ALIGN_TOP_MID, 0, 12);

    /* Split-label row: ssc_montserrat_72 (~86 px line_height).
     * Row 94 px mit bottom-alignment, 8 px pad unten.              */
    lv_obj_t *tv_row = lv_obj_create(col_temp);
    lv_obj_remove_style_all(tv_row);
    lv_obj_set_size(tv_row, 212, 94);
    lv_obj_align(tv_row, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(tv_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tv_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(tv_row, 2, 0);
    lv_obj_clear_flag(tv_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Integer-teil: custom ssc_montserrat_72 (64pt, ASCII + °). Echt
     * 64pt font ohne zoom-hacks - clean rendering.                  */
    home_sauna_temp_int = lv_label_create(tv_row);
    lv_label_set_text(home_sauna_temp_int, "0");
    lv_obj_set_style_text_font(home_sauna_temp_int, &ssc_montserrat_72, 0);
    lv_obj_set_style_text_color(home_sauna_temp_int, SSC_C_ACCENT, 0);

    /* Dezimal + Einheit: daneben (F_LG 36pt - eine spur groesser) */
    home_sauna_temp_frac = lv_label_create(tv_row);
    lv_label_set_text(home_sauna_temp_frac, ".0 °C");
    lv_obj_set_style_text_font(home_sauna_temp_frac, F_LG, 0);
    lv_obj_set_style_text_color(home_sauna_temp_frac, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_pad_bottom(home_sauna_temp_frac, 8, 0);

    /* Legacy-label als hidden dummy damit on_sensor_data nicht crasht */
    home_sauna_temp_val = lv_label_create(col_temp);
    lv_label_set_text(home_sauna_temp_val, "-");
    lv_obj_add_flag(home_sauna_temp_val, LV_OBJ_FLAG_HIDDEN);

    /* Rechte Spalte: Feuchte */
    lv_obj_t *col_rh = lv_obj_create(cab_body);
    lv_obj_remove_style_all(col_rh);
    lv_obj_set_size(col_rh, 220, 128);
    lv_obj_clear_flag(col_rh, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *rh_cap = lv_label_create(col_rh);
    lv_label_set_text(rh_cap, "FEUCHTE");
    label_muted(rh_cap);
    lv_obj_align(rh_cap, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *rv_row = lv_obj_create(col_rh);
    lv_obj_remove_style_all(rv_row);
    lv_obj_set_size(rv_row, 212, 94);
    lv_obj_align(rv_row, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(rv_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rv_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(rv_row, 6, 0);
    lv_obj_clear_flag(rv_row, LV_OBJ_FLAG_SCROLLABLE);

    /* RH: custom ssc_montserrat_72 (64pt) */
    home_sauna_rh_int = lv_label_create(rv_row);
    lv_label_set_text(home_sauna_rh_int, "0");
    lv_obj_set_style_text_font(home_sauna_rh_int, &ssc_montserrat_72, 0);
    lv_obj_set_style_text_color(home_sauna_rh_int, SSC_C_CHART_RH, 0);

    lv_obj_t *unit_rh = lv_label_create(rv_row);
    lv_label_set_text(unit_rh, "% RH");
    lv_obj_set_style_text_color(unit_rh, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(unit_rh, F_LG, 0);
    lv_obj_set_style_pad_bottom(unit_rh, 8, 0);

    /* Legacy dummy hidden */
    home_sauna_rh_val = lv_label_create(col_rh);
    lv_label_set_text(home_sauna_rh_val, "-");
    lv_obj_add_flag(home_sauna_rh_val, LV_OBJ_FLAG_HIDDEN);
    home_sauna_rh_frac = NULL;

    /* ===== Geraete-Sensoren-Sammelbox (70 px) =====
     * Professionell: Titel zentriert oben, drei spalten gleich gross
     * zentriert drunter (caption oben, wert gross, einheit klein).  */
    lv_obj_t *vbox = lv_obj_create(scr_home);
    style_card(vbox);
    lv_obj_set_size(vbox, 464, 70);
    lv_obj_align_to(vbox, cabin, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    lv_obj_clear_flag(vbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(vbox, 4, 0);

    lv_obj_t *vb_title = lv_label_create(vbox);
    lv_label_set_text(vb_title, "GERAET (INTERN)");
    lv_obj_set_style_text_color(vb_title, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(vb_title, F_XS, 0);
    lv_obj_set_style_text_letter_space(vb_title, 2, 0);
    lv_obj_align(vb_title, LV_ALIGN_TOP_LEFT, 4, 0);

    /* 3 gleichgrosse Zellen, zentriert */
    lv_obj_t *vb_row = lv_obj_create(vbox);
    lv_obj_remove_style_all(vb_row);
    lv_obj_set_size(vb_row, 456, 46);
    lv_obj_align(vb_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(vb_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vb_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(vb_row, LV_OBJ_FLAG_SCROLLABLE);

    struct {
        const char *caption;
        const char *unit;
        lv_obj_t **out;
    } items[] = {
        { "TEMPERATUR", "°C",  &home_vorraum_temp_val },
        { "FEUCHTE",    "%",   &home_vorraum_rh_val   },
        { "CO2",        "ppm", &home_vorraum_co2_val  },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *cell = lv_obj_create(vb_row);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 140, 46);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        /* Caption hoeher damit deutliche trennung zum wert -
         * per user-feedback: "bezeichnungen etwas höher machen". */
        lv_obj_t *cap = lv_label_create(cell);
        lv_label_set_text(cap, items[i].caption);
        lv_obj_set_style_text_color(cap, SSC_C_TEXT_FAINT, 0);
        lv_obj_set_style_text_font(cap, F_XS, 0);
        lv_obj_set_style_text_letter_space(cap, 1, 0);
        lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 2);

        /* Value + Einheit gemeinsam in flex-row, bottom center */
        lv_obj_t *valrow = lv_obj_create(cell);
        lv_obj_remove_style_all(valrow);
        lv_obj_set_size(valrow, LV_PCT(100), 28);
        lv_obj_align(valrow, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_flex_flow(valrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(valrow, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(valrow, 4, 0);

        lv_obj_t *val = lv_label_create(valrow);
        lv_label_set_text(val, "-");
        lv_obj_set_style_text_font(val, F_MD, 0);
        lv_obj_set_style_text_color(val, SSC_C_TEXT, 0);
        *items[i].out = val;

        lv_obj_t *u = lv_label_create(valrow);
        lv_label_set_text(u, items[i].unit);
        lv_obj_set_style_text_color(u, SSC_C_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(u, F_XS, 0);
        lv_obj_set_style_pad_bottom(u, 3, 0);
    }

    /* ===== Session-Start-Button (56 px) ===== */
    lv_obj_t *start = lv_btn_create(scr_home);
    style_primary_btn(start);
    lv_obj_set_size(start, 464, 76);
    lv_obj_align_to(start, vbox, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_t *sl = lv_label_create(start);
    lv_label_set_text(sl, LV_SYMBOL_PLAY "  SESSION STARTEN");
    lv_obj_set_style_text_font(sl, F_MD, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(start, on_home_start_clicked, LV_EVENT_CLICKED, NULL);

    /* ===== Letzte-Sessions-Header (20 px) ===== */
    lv_obj_t *rhdr = lv_obj_create(scr_home);
    lv_obj_remove_style_all(rhdr);
    lv_obj_set_size(rhdr, 464, 20);
    lv_obj_align_to(rhdr, start, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    lv_obj_t *rl = lv_label_create(rhdr);
    lv_label_set_text(rl, "LETZTE SESSIONS");
    label_muted(rl);
    lv_obj_align(rl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *histbtn = lv_btn_create(rhdr);
    style_ghost_btn(histbtn);
    lv_obj_set_size(histbtn, 96, 20);
    lv_obj_set_style_radius(histbtn, 10, 0);
    lv_obj_align(histbtn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *hl = lv_label_create(histbtn);
    lv_label_set_text(hl, "ALLE  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(hl, F_XS, 0);
    lv_obj_center(hl);
    lv_obj_add_event_cb(histbtn, on_home_history_clicked, LV_EVENT_CLICKED, NULL);

    /* ===== Recent-sessions-Liste (~140 px, passt in rest der höhe) ===== */
    home_recent_list = lv_list_create(scr_home);
    style_card(home_recent_list);
    /* 68 px - exakt 2 rows, unterkante hat bildschirmabstand */
    lv_obj_set_size(home_recent_list, 464, 68);
    lv_obj_set_scrollbar_mode(home_recent_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align_to(home_recent_list, rhdr, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    lv_obj_set_style_pad_all(home_recent_list, 0, 0);
    lv_obj_set_style_pad_row(home_recent_list, 0, 0);
    lv_obj_add_flag(home_recent_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(home_recent_list, LV_DIR_VER);
}

static void home_update_recent(const struct view_data_session_list *list) {
    if (!home_recent_list) return;
    lv_obj_clean(home_recent_list);
    /* Nur die 2 letzten sessions zeigen; volle history unter "historie". */
    uint16_t n = list->count < 2 ? list->count : 2;
    for (uint16_t i = 0; i < n; i++) {
        const struct view_data_session_meta *m = &list->items[i];
        char head[64];
        char peak[24];
        char ts[20];
        fmt_datetime_short(ts, sizeof(ts), m->start_ts);
        char middle[48] = {0};
        if (m->aufguss_headline[0]) {
            snprintf(middle, sizeof(middle), "%s%s",
                     m->operator_tag[0] ? "  |  " : "", m->aufguss_headline);
        }
        char tn[16] = {0};
        if (m->participants > 0) {
            snprintf(tn, sizeof(tn), "  \xE2\x80\xA2  %u TN",
                     (unsigned)m->participants);
        }
        snprintf(head, sizeof(head), "%s   %s%s%s",
                 ts,
                 m->operator_tag[0] ? m->operator_tag : "",
                 middle, tn);
        snprintf(peak, sizeof(peak), "%.0f C  %.0f%%  %u aufg.",
                 isnan(m->peak_temp) ? 0.0f : m->peak_temp,
                 isnan(m->peak_rh)   ? 0.0f : m->peak_rh,
                 (unsigned)m->aufguss_count);
        lv_obj_t *row = lv_list_add_btn(home_recent_list, NULL, head);
        lv_obj_set_style_bg_color(row, SSC_C_SURFACE, 0);
        lv_obj_set_style_bg_color(row, SSC_C_ELEVATED, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(row, SSC_C_TEXT, 0);
        lv_obj_set_style_text_font(row, F_SM, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        /* WICHTIG: cache fuer den detail-klick aus home ist dasselbe
         * hist_cached[] wie im history-screen - damit on_history_row_clicked
         * mit dem idx identisch weiterarbeiten kann. (Der cache wird in
         * history_apply VOR home_update_recent befuellt.)              */
        lv_obj_add_event_cb(row, on_history_row_clicked,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        /* kleiner subtitle als extra label */
        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_text(sub, peak);
        lv_obj_set_style_text_color(sub, SSC_C_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(sub, F_XS, 0);
        lv_obj_align(sub, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (n == 0) {
        lv_obj_t *e = lv_label_create(home_recent_list);
        lv_label_set_text(e, "  noch keine sessions");
        lv_obj_set_style_text_color(e, SSC_C_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(e, F_SM, 0);
        lv_obj_align(e, LV_ALIGN_CENTER, 0, 0);
    }
}

/* ======================================================================= */
/*   LIVE                                                                    */
/* ======================================================================= */

/* 3-Minuten-Fenster bei 1-s-Sample-Rate: 180 Punkte, alle 1 s einer. */
#define LIVE_CHART_POINTS 180

static void on_live_aufguss_clicked(lv_event_t *e) {
    if (!live_running) return;
    char name[SSC_AUFGUSS_NAME_MAXLEN] = {0};
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_SESSION_AUFGUSS, name, sizeof(name),
                      portMAX_DELAY);
}

static void on_live_start_clicked(lv_event_t *e) {
    ESP_LOGI(TAG, "on_live_start_clicked: running=%d", live_running);
    /* Guard gegen doppel-tap - wenn schon running, ignorieren. */
    if (live_running) return;
    /* Chart reset */
    if (live_chart) {
        if (live_ser_temp)
            lv_chart_set_all_value(live_chart, live_ser_temp, LV_CHART_POINT_NONE);
        if (live_ser_rh)
            lv_chart_set_all_value(live_chart, live_ser_rh, LV_CHART_POINT_NONE);
        lv_chart_refresh(live_chart);
    }
    char op[SSC_OPERATOR_MAXLEN] = {0};
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_SESSION_START, op, sizeof(op),
                      portMAX_DELAY);
    live_set_running_state();
}

static void on_live_stop_clicked(lv_event_t *e) {
    ESP_LOGI(TAG, "on_live_stop_clicked: running=%d", live_running);
    if (!live_running) return;
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_SESSION_END_REQUEST, NULL, 0,
                      portMAX_DELAY);
    live_running = false;
    live_set_button_states(false, false, false, true);
    if (live_status_label) lv_label_set_text(live_status_label, "SPEICHERE...");

    /* Direkt auf summary wechseln - nicht auf event-roundtrip warten.
     * Der VIEW_EVENT_SESSION_SUMMARY_READY-event befuellt dann die
     * felder via summary_prefill().                                 */
    lv_scr_load_anim(scr_summary, LV_SCR_LOAD_ANIM_FADE_ON, 240, 0, false);
    ESP_LOGI(TAG, "scr_summary geladen");
}

static void on_live_cancel_clicked(lv_event_t *e) {
    /* Wenn session schon laeuft, Discard feuern (schliesst SD etc). */
    if (live_running) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_SESSION_DISCARD, NULL, 0,
                          portMAX_DELAY);
        live_running = false;
    }
    lv_scr_load_anim(scr_home, LV_SCR_LOAD_ANIM_FADE_ON, 240, 0, false);
}

/* State-transitions setzen ALLE 4 buttons EXPLIZIT auf den richtigen
 * state - egal in welchem state das system vorher war. So gibt es kein
 * "leak" von versteckten buttons zwischen states.                    */
static void live_set_button_states(bool start_v, bool aufguss_v,
                                    bool stop_v, bool cancel_v) {
    if (live_start_btn) {
        if (start_v) lv_obj_clear_flag(live_start_btn, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(live_start_btn,   LV_OBJ_FLAG_HIDDEN);
    }
    if (live_aufguss_btn) {
        if (aufguss_v) lv_obj_clear_flag(live_aufguss_btn, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(live_aufguss_btn,   LV_OBJ_FLAG_HIDDEN);
    }
    if (live_stop_btn) {
        if (stop_v) lv_obj_clear_flag(live_stop_btn, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(live_stop_btn,   LV_OBJ_FLAG_HIDDEN);
    }
    if (live_cancel_btn) {
        if (cancel_v) lv_obj_clear_flag(live_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(live_cancel_btn,   LV_OBJ_FLAG_HIDDEN);
    }
}

static void live_set_ready_state(void) {
    live_running = false;
    if (live_status_label) lv_label_set_text(live_status_label, "BEREIT");
    if (live_timer_val)     lv_label_set_text(live_timer_val, "00:00");
    if (live_aufguss_count_val)
        lv_label_set_text(live_aufguss_count_val, "0 aufg.");
    if (live_peak_val)  lv_label_set_text(live_peak_val,  "-");
    if (live_peak_val2) lv_label_set_text(live_peak_val2, "-");
    /* READY: START + CANCEL sichtbar, AUFGUSS + STOP hidden */
    live_set_button_states(true, false, false, true);
}

static void live_set_running_state(void) {
    live_running = true;
    if (live_status_label) lv_label_set_text(live_status_label, "SESSION LAEUFT");
    /* RUNNING: AUFGUSS + STOP + CANCEL sichtbar, START hidden */
    live_set_button_states(false, true, true, true);
}

static void build_live(void) {
    scr_live = lv_obj_create(NULL);
    style_screen(scr_live);

    /* Statusleiste oben */
    lv_obj_t *top = lv_obj_create(scr_live);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, 456, 28);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *dot = lv_obj_create(top);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, SSC_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);

    live_status_label = lv_label_create(top);
    lv_label_set_text(live_status_label, "BEREIT");
    lv_obj_set_style_text_color(live_status_label, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(live_status_label, F_XS, 0);
    lv_obj_set_style_text_letter_space(live_status_label, 3, 0);
    lv_obj_align(live_status_label, LV_ALIGN_LEFT_MID, 22, 0);

    live_timer_val = lv_label_create(top);
    lv_label_set_text(live_timer_val, "00:00");
    lv_obj_set_style_text_color(live_timer_val, SSC_C_TEXT, 0);
    lv_obj_set_style_text_font(live_timer_val, F_MD, 0);
    lv_obj_align(live_timer_val, LV_ALIGN_CENTER, 0, 0);

    live_aufguss_count_val = lv_label_create(top);
    lv_label_set_text(live_aufguss_count_val, "0 Aufg.");
    lv_obj_set_style_text_color(live_aufguss_count_val, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(live_aufguss_count_val, F_XS, 0);
    lv_obj_align(live_aufguss_count_val, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Value-Zeile: Temp | RH | Peak */
    lv_obj_t *vrow = lv_obj_create(scr_live);
    lv_obj_remove_style_all(vrow);
    lv_obj_set_size(vrow, 456, 84);
    lv_obj_align_to(vrow, top, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);

    big_stat_card(vrow, "TEMP KABINE",    "C", SSC_C_ACCENT,   &live_temp_val, 148, 84);
    big_stat_card(vrow, "FEUCHTE KABINE", "%", SSC_C_CHART_RH, &live_rh_val,   148, 84);

    /* PEAK-Karte zweizeilig: oben max-temp in Gold, unten max-RH in Sage.
     * Eine einzige Zeile wie "88C/70%" wird bei F_LG (36pt) in 148px Breite
     * abgeschnitten - daher auf zwei Zeilen F_MD splitten.            */
    lv_obj_t *peak_card = lv_obj_create(vrow);
    style_card(peak_card);
    lv_obj_set_size(peak_card, 148, 84);
    lv_obj_clear_flag(peak_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *peak_lbl = lv_label_create(peak_card);
    lv_label_set_text(peak_lbl, "PEAK");
    label_muted(peak_lbl);
    lv_obj_align(peak_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    live_peak_val = lv_label_create(peak_card);
    lv_label_set_text(live_peak_val, "-");
    lv_obj_set_style_text_font(live_peak_val, F_MD, 0);
    lv_obj_set_style_text_color(live_peak_val, SSC_C_ACCENT, 0);
    lv_obj_align(live_peak_val, LV_ALIGN_BOTTOM_LEFT, 0, -22);

    live_peak_val2 = lv_label_create(peak_card);
    lv_label_set_text(live_peak_val2, "-");
    lv_obj_set_style_text_font(live_peak_val2, F_MD, 0);
    lv_obj_set_style_text_color(live_peak_val2, SSC_C_CHART_RH, 0);
    lv_obj_align(live_peak_val2, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    /* ===== Professionelles Chart-Layout mit MANUELLEN Achsen-Labels
     * ==================================================================
     * Statt auf lv_chart_set_axis_tick zu vertrauen (das die Labels in
     * LVGL v8 oft abschneidet), zeichnen wir die Beschriftungen selbst
     * als lv_label. Damit kontrollieren wir exakt wo sie stehen.
     *
     * Layout des 456x232-Containers:
     *   [ Y-links 38px ][      CHART 360px      ][ Y-rechts 38px ]
     *   [                                                        ]
     *   [                 X-unten 22px                           ]
     * Chart-zeichenfläche: 360 x 192
     * ================================================================= */
    lv_obj_t *chart_wrap = lv_obj_create(scr_live);
    style_card(chart_wrap);
    /* 258 px = maximaler platz zwischen vrow und arow (bottom-pinned).
     * Gesamt: 28 top + 8 + 84 vrow + 8 + 258 chart + 8 + 62 arow = 456. */
    lv_obj_set_size(chart_wrap, 456, 258);
    lv_obj_align_to(chart_wrap, vrow, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_clear_flag(chart_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(chart_wrap, 0, 0);

    /* Y-Achsenbeschriftungen LINKS (°C, 0-120 in 30er-Schritten).
     * Chart-hoehe 222 px → labels bei y = 12 + i*222/4 - 7           */
    const int y_left_values[] = {120, 90, 60, 30, 0};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *l = lv_label_create(chart_wrap);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", y_left_values[i]);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_color(l, SSC_C_CHART_TEMP, 0);
        lv_obj_set_style_text_font(l, F_XS, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        int y_pos = 12 + (i * 222) / 4 - 7;
        lv_obj_set_pos(l, 4, y_pos);
        lv_obj_set_size(l, 34, 14);
    }

    /* Y-Achsenbeschriftungen RECHTS (%, 0-100 in 25er-Schritten) */
    const int y_right_values[] = {100, 75, 50, 25, 0};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *l = lv_label_create(chart_wrap);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", y_right_values[i]);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_color(l, SSC_C_CHART_RH, 0);
        lv_obj_set_style_text_font(l, F_XS, 0);
        int y_pos = 12 + (i * 222) / 4 - 7;
        lv_obj_set_pos(l, 456 - 36, y_pos);
        lv_obj_set_size(l, 32, 14);
    }

    /* Einheiten-Labels oben an Y-Achsen */
    lv_obj_t *lbl_unit_c = lv_label_create(chart_wrap);
    lv_label_set_text(lbl_unit_c, "°C");
    lv_obj_set_style_text_color(lbl_unit_c, SSC_C_CHART_TEMP, 0);
    lv_obj_set_style_text_font(lbl_unit_c, F_XS, 0);
    lv_obj_set_pos(lbl_unit_c, 4, 0);

    lv_obj_t *lbl_unit_rh = lv_label_create(chart_wrap);
    lv_label_set_text(lbl_unit_rh, "%");
    lv_obj_set_style_text_color(lbl_unit_rh, SSC_C_CHART_RH, 0);
    lv_obj_set_style_text_font(lbl_unit_rh, F_XS, 0);
    lv_obj_set_pos(lbl_unit_rh, 456 - 16, 0);

    /* X-Achsenbeschriftungen UNTEN - 3-Min-Sliding-Window. Labels
     * zeigen "wie lange her" (negativ), rechts ist jetzt. So bleibt
     * die achse statisch auch wenn session > 3 min laeuft: das fenster
     * scrollt automatisch.                                            */
    const char *x_labels[] = {"-3:00", "-2:15", "-1:30", "-0:45", "jetzt"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *l = lv_label_create(chart_wrap);
        lv_label_set_text(l, x_labels[i]);
        lv_obj_set_style_text_color(l, SSC_C_TEXT_FAINT, 0);
        lv_obj_set_style_text_font(l, F_XS, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        int x_pos = 40 + (i * 360) / 4 - 20;
        lv_obj_set_pos(l, x_pos, 240);   /* unter dem 222px-chart */
        lv_obj_set_size(l, 40, 14);
    }

    /* Eigentliches Chart in der Mitte (ohne eigene axis-ticks) */
    live_chart = lv_chart_create(chart_wrap);
    lv_obj_set_size(live_chart, 380, 222);
    lv_obj_set_pos(live_chart, 38, 14);
    lv_chart_set_type(live_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(live_chart, LIVE_CHART_POINTS);
    lv_chart_set_range(live_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 120);
    lv_chart_set_range(live_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    lv_chart_set_div_line_count(live_chart, 5, 5);
    lv_chart_set_update_mode(live_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_pad_all(live_chart, 0, 0);
    lv_obj_set_style_bg_color(live_chart, SSC_C_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(live_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(live_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(live_chart, SSC_C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_line_color(live_chart, SSC_C_CHART_GRID, LV_PART_MAIN);
    lv_obj_set_style_line_width(live_chart, 1, LV_PART_MAIN);

    live_ser_temp = lv_chart_add_series(live_chart, SSC_C_CHART_TEMP,
                                        LV_CHART_AXIS_PRIMARY_Y);
    live_ser_rh   = lv_chart_add_series(live_chart, SSC_C_CHART_RH,
                                        LV_CHART_AXIS_SECONDARY_Y);
    lv_obj_set_style_size(live_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(live_chart, 3, LV_PART_ITEMS);

    /* Action row: Bereich fuer 3 Buttons (Aufguss | Stoppen | Abbrechen)
     * im RUNNING-State; im READY-State werden nur Starten | Abbrechen
     * gezeigt. Die Sichtbarkeit steuert live_set_ready/running_state. */
    lv_obj_t *arow = lv_obj_create(scr_live);
    lv_obj_remove_style_all(arow);
    lv_obj_set_size(arow, 456, 62);
    /* gleicher gap zum chart wie chart zu den val-karten (8 px) */
    lv_obj_align_to(arow, chart_wrap, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(arow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(arow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_gap(arow, 8, 0);

    /* STARTEN (nur im READY-state sichtbar) */
    live_start_btn = lv_btn_create(arow);
    style_primary_btn(live_start_btn);
    lv_obj_set_size(live_start_btn, 292, 60);
    lv_obj_t *lst = lv_label_create(live_start_btn);
    lv_label_set_text(lst, LV_SYMBOL_PLAY "  STARTEN");
    lv_obj_set_style_text_font(lst, F_MD, 0);
    lv_obj_center(lst);
    lv_obj_add_event_cb(live_start_btn, on_live_start_clicked,
                        LV_EVENT_CLICKED, NULL);

    /* AUFGUSS (nur im RUNNING-state sichtbar) - gleiche Position wie Starten */
    live_aufguss_btn = lv_btn_create(arow);
    style_primary_btn(live_aufguss_btn);
    lv_obj_set_size(live_aufguss_btn, 200, 60);
    lv_obj_t *la = lv_label_create(live_aufguss_btn);
    /* Klarerer Text: "AUFGUSS MARKIEREN" - es wird ein Zeitstempel-
     * Marker gesetzt (der Aufguss-Name wird spaeter im Summary
     * eingegeben). Der Counter oben rechts zaehlt hoch.            */
    lv_label_set_text(la, LV_SYMBOL_PLUS "  AUFGUSS MARKIEREN");
    lv_obj_set_style_text_font(la, F_MD, 0);
    lv_obj_center(la);
    lv_obj_add_event_cb(live_aufguss_btn, on_live_aufguss_clicked,
                        LV_EVENT_CLICKED, NULL);

    /* STOPPEN (nur im RUNNING-state) */
    live_stop_btn = lv_btn_create(arow);
    style_ghost_btn(live_stop_btn);
    lv_obj_set_size(live_stop_btn, 120, 60);
    lv_obj_t *lstop = lv_label_create(live_stop_btn);
    lv_label_set_text(lstop, "STOPPEN");
    lv_obj_set_style_text_font(lstop, F_SM, 0);
    lv_obj_center(lstop);
    lv_obj_add_event_cb(live_stop_btn, on_live_stop_clicked,
                        LV_EVENT_CLICKED, NULL);

    /* ABBRECHEN (immer sichtbar) */
    live_cancel_btn = lv_btn_create(arow);
    style_ghost_btn(live_cancel_btn);
    lv_obj_set_size(live_cancel_btn, 124, 60);
    lv_obj_t *lcan = lv_label_create(live_cancel_btn);
    lv_label_set_text(lcan, "ABBRECHEN");
    lv_obj_set_style_text_font(lcan, F_SM, 0);
    lv_obj_center(lcan);
    lv_obj_add_event_cb(live_cancel_btn, on_live_cancel_clicked,
                        LV_EVENT_CLICKED, NULL);

    /* Initial: READY-State */
    live_set_ready_state();
}

/* ======================================================================= */
/*   SUMMARY                                                                 */
/* ======================================================================= */

static void update_participants_label(void) {
    if (!sum_participants_label) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", sum_participants_n);
    lv_label_set_text(sum_participants_label, buf);
}

static void on_participants_minus_clicked(lv_event_t *e) {
    if (sum_participants_n > 0) sum_participants_n--;
    update_participants_label();
}

static void on_participants_plus_clicked(lv_event_t *e) {
    if (sum_participants_n < 99) sum_participants_n++;
    update_participants_label();
}

static void update_aufg_count_label(void) {
    if (!sum_aufg_count_label) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", sum_aufg_count_n);
    lv_label_set_text(sum_aufg_count_label, buf);
}

static void on_aufg_count_minus_clicked(lv_event_t *e) {
    if (sum_aufg_count_n > 0) sum_aufg_count_n--;
    update_aufg_count_label();
}

static void on_aufg_count_plus_clicked(lv_event_t *e) {
    if (sum_aufg_count_n < 50) sum_aufg_count_n++;
    update_aufg_count_label();
}

static void on_summary_save_clicked(lv_event_t *e) {
    ESP_LOGI(TAG, "on_summary_save_clicked: id=%s edit=%d",
             sum_current_meta.id, sum_edit_mode ? 1 : 0);
    struct view_data_session_meta meta = sum_current_meta;
    char tmp[SSC_OPERATOR_MAXLEN] = {0};
    if (sum_operator_dd)
        lv_dropdown_get_selected_str(sum_operator_dd, tmp, sizeof(tmp));
    if (tmp[0] && strcmp(tmp, "-") != 0)
        strncpy(meta.operator_tag, tmp, SSC_OPERATOR_MAXLEN - 1);
    strncpy(meta.aufguss_headline, lv_textarea_get_text(sum_aufguss_ta),
            SSC_AUFGUSS_NAME_MAXLEN - 1);
    meta.participants = (uint16_t)sum_participants_n;
    /* aufguss_count aus dem stepper: nur im edit-mode wird das gebraucht
     * um nachtraeglich korrigieren zu koennen. Beim normalen save bleibt
     * der wert aus der live-session erhalten.                           */
    if (sum_edit_mode) {
        meta.aufguss_count = (uint8_t)sum_aufg_count_n;
    }
    strncpy(meta.notes, lv_textarea_get_text(sum_notes_ta),
            SSC_NOTES_MAXLEN - 1);

    if (sum_edit_mode) {
        /* Edit-flow: session_store_update, zurueck auf detail */
        ESP_LOGI(TAG, "edit-save: id=%s op=%s aufg='%s' p=%u notes_len=%u",
                 meta.id, meta.operator_tag, meta.aufguss_headline,
                 (unsigned)meta.participants,
                 (unsigned)strlen(meta.notes));
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_SESSION_EDIT, &meta, sizeof(meta),
                          portMAX_DELAY);
        /* Detail-screen-labels sofort mit den neuen werten aktualisieren,
         * damit der user die aenderung ohne screen-wechsel sieht. Die
         * hist_cached[]-tabelle wird kurz darauf vom HISTORY_LIST-event
         * asynchron refreshed, aber das reicht nicht fuer den aktuellen
         * detail-screen-inhalt.                                         */
        if (detail_title) lv_label_set_text(detail_title, meta.id);
        if (detail_sub) {
            char sub[96], ts[20];
            fmt_datetime_short(ts, sizeof(ts), meta.start_ts);
            snprintf(sub, sizeof(sub),
                     "%s  |  %s  |  Peak %.0f C / %.0f%%  |  %u Aufg.",
                     ts,
                     meta.operator_tag[0] ? meta.operator_tag : "-",
                     isnan(meta.peak_temp) ? 0.0f : meta.peak_temp,
                     isnan(meta.peak_rh)   ? 0.0f : meta.peak_rh,
                     (unsigned)meta.aufguss_count);
            lv_label_set_text(detail_sub, sub);
        }
        sum_edit_mode = false;
        lv_scr_load_anim(scr_detail, LV_SCR_LOAD_ANIM_FADE_ON, 240, 0, false);
    } else {
        /* Save-flow (frische session beenden) */
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_SESSION_SAVE, &meta, sizeof(meta),
                          portMAX_DELAY);
        /* History neu laden damit home-recent + history-liste aktualisiert. */
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_HISTORY_LIST_REQ, NULL, 0, portMAX_DELAY);
        lv_scr_load_anim(scr_home, LV_SCR_LOAD_ANIM_FADE_ON, 240, 0, false);
    }
}

static void on_summary_discard_clicked(lv_event_t *e) {
    if (sum_edit_mode) {
        /* Edit abbrechen: keine aenderungen, zurueck zu detail */
        sum_edit_mode = false;
        lv_scr_load_anim(scr_detail, LV_SCR_LOAD_ANIM_FADE_ON, 240, 0, false);
        return;
    }
    /* Neue session verwerfen */
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_SESSION_DISCARD, NULL, 0, portMAX_DELAY);
    lv_scr_load_anim(scr_home, LV_SCR_LOAD_ANIM_FADE_ON, 240, 0, false);
}

static void build_summary(void) {
    scr_summary = lv_obj_create(NULL);
    style_screen(scr_summary);

    sum_title_label = lv_label_create(scr_summary);
    lv_label_set_text(sum_title_label, "SESSION BEENDET");
    label_muted(sum_title_label);
    label_accent(sum_title_label);
    lv_obj_set_style_text_font(sum_title_label, F_MD, 0);
    lv_obj_align(sum_title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    sum_meta_summary = lv_label_create(scr_summary);
    lv_label_set_text(sum_meta_summary, "-");
    lv_obj_set_style_text_color(sum_meta_summary, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(sum_meta_summary, F_XS, 0);
    lv_obj_align(sum_meta_summary, LV_ALIGN_TOP_LEFT, 0, 30);

    /* Form container (scrollable) */
    lv_obj_t *form = lv_obj_create(scr_summary);
    style_card(form);
    lv_obj_set_size(form, 456, 320);
    lv_obj_align(form, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(form, 8, 0);
    lv_obj_set_style_pad_all(form, 12, 0);
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(form, LV_DIR_VER);

    lv_obj_t *op_l = lv_label_create(form);
    lv_label_set_text(op_l, "SAUNAMEISTER");
    label_muted(op_l);

    sum_operator_dd = lv_dropdown_create(form);
    lv_dropdown_set_options(sum_operator_dd, "-");
    lv_obj_set_width(sum_operator_dd, 420);
    lv_obj_set_style_bg_color(sum_operator_dd, SSC_C_ELEVATED, 0);
    lv_obj_set_style_text_color(sum_operator_dd, SSC_C_TEXT, 0);
    lv_obj_set_style_border_color(sum_operator_dd, SSC_C_BORDER, 0);

    lv_obj_t *af_l = lv_label_create(form);
    lv_label_set_text(af_l, "AUFGUSS / RITUAL");
    label_muted(af_l);

    sum_aufguss_ta = lv_textarea_create(form);
    lv_textarea_set_one_line(sum_aufguss_ta, true);
    lv_textarea_set_placeholder_text(sum_aufguss_ta, "z.B. birke minze");
    lv_obj_set_size(sum_aufguss_ta, 420, 40);
    lv_obj_set_style_bg_color(sum_aufguss_ta, SSC_C_ELEVATED, 0);
    lv_obj_set_style_text_color(sum_aufguss_ta, SSC_C_TEXT, 0);

    /* TEILNEHMER + AUFGUSS-ANZAHL nebeneinander in einer row, je eine
     * col mit label ueber stepper. 420 px / 2 = 210 px pro zelle.   */
    lv_obj_t *pa_row = lv_obj_create(form);
    lv_obj_remove_style_all(pa_row);
    lv_obj_set_size(pa_row, 420, 90);
    lv_obj_set_flex_flow(pa_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pa_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(pa_row, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Zelle links: TEILNEHMER --- */
    lv_obj_t *pp_col = lv_obj_create(pa_row);
    lv_obj_remove_style_all(pp_col);
    lv_obj_set_size(pp_col, 200, 90);
    lv_obj_clear_flag(pp_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pp_l = lv_label_create(pp_col);
    lv_label_set_text(pp_l, "TEILNEHMER");
    label_muted(pp_l);
    lv_obj_align(pp_l, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *stepper = lv_obj_create(pp_col);
    lv_obj_remove_style_all(stepper);
    lv_obj_set_size(stepper, 200, 56);
    lv_obj_align(stepper, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(stepper, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_minus = lv_btn_create(stepper);
    style_ghost_btn(btn_minus);
    lv_obj_set_size(btn_minus, 48, 48);
    lv_obj_t *lminus = lv_label_create(btn_minus);
    lv_label_set_text(lminus, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lminus, F_MD, 0);
    lv_obj_center(lminus);
    lv_obj_add_event_cb(btn_minus, on_participants_minus_clicked,
                        LV_EVENT_CLICKED, NULL);

    sum_participants_label = lv_label_create(stepper);
    lv_label_set_text(sum_participants_label, "0");
    lv_obj_set_style_text_font(sum_participants_label, F_LG, 0);
    lv_obj_set_style_text_color(sum_participants_label, SSC_C_TEXT, 0);

    lv_obj_t *btn_plus = lv_btn_create(stepper);
    style_primary_btn(btn_plus);
    lv_obj_set_size(btn_plus, 48, 48);
    lv_obj_t *lplus = lv_label_create(btn_plus);
    lv_label_set_text(lplus, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lplus, F_MD, 0);
    lv_obj_center(lplus);
    lv_obj_add_event_cb(btn_plus, on_participants_plus_clicked,
                        LV_EVENT_CLICKED, NULL);

    /* Spinbox als hidden-fallback damit bestehender code nicht kaputt
     * geht der sum_participants_sb referenziert */
    sum_participants_sb = lv_spinbox_create(form);
    lv_spinbox_set_range(sum_participants_sb, 0, 99);
    lv_obj_add_flag(sum_participants_sb, LV_OBJ_FLAG_HIDDEN);

    /* --- Zelle rechts: AUFGUSS-ANZAHL --- */
    lv_obj_t *ac_col = lv_obj_create(pa_row);
    lv_obj_remove_style_all(ac_col);
    lv_obj_set_size(ac_col, 200, 90);
    lv_obj_clear_flag(ac_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ac_l = lv_label_create(ac_col);
    lv_label_set_text(ac_l, "AUFGUSS-ANZAHL");
    label_muted(ac_l);
    lv_obj_align(ac_l, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *ac_stepper = lv_obj_create(ac_col);
    lv_obj_remove_style_all(ac_stepper);
    lv_obj_set_size(ac_stepper, 200, 56);
    lv_obj_align(ac_stepper, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(ac_stepper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ac_stepper, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ac_stepper, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ac_minus = lv_btn_create(ac_stepper);
    style_ghost_btn(ac_minus);
    lv_obj_set_size(ac_minus, 48, 48);
    lv_obj_t *ac_lminus = lv_label_create(ac_minus);
    lv_label_set_text(ac_lminus, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(ac_lminus, F_MD, 0);
    lv_obj_center(ac_lminus);
    lv_obj_add_event_cb(ac_minus, on_aufg_count_minus_clicked,
                        LV_EVENT_CLICKED, NULL);

    sum_aufg_count_label = lv_label_create(ac_stepper);
    lv_label_set_text(sum_aufg_count_label, "0");
    lv_obj_set_style_text_font(sum_aufg_count_label, F_LG, 0);
    lv_obj_set_style_text_color(sum_aufg_count_label, SSC_C_TEXT, 0);

    lv_obj_t *ac_plus = lv_btn_create(ac_stepper);
    style_primary_btn(ac_plus);
    lv_obj_set_size(ac_plus, 48, 48);
    lv_obj_t *ac_lplus = lv_label_create(ac_plus);
    lv_label_set_text(ac_lplus, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(ac_lplus, F_MD, 0);
    lv_obj_center(ac_lplus);
    lv_obj_add_event_cb(ac_plus, on_aufg_count_plus_clicked,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *nt_l = lv_label_create(form);
    lv_label_set_text(nt_l, "NOTIZEN");
    label_muted(nt_l);

    sum_notes_ta = lv_textarea_create(form);
    lv_obj_set_size(sum_notes_ta, 420, 70);
    lv_textarea_set_placeholder_text(sum_notes_ta, "optional...");
    lv_obj_set_style_bg_color(sum_notes_ta, SSC_C_ELEVATED, 0);
    lv_obj_set_style_text_color(sum_notes_ta, SSC_C_TEXT, 0);

    /* Aktionen */
    lv_obj_t *act = lv_obj_create(scr_summary);
    lv_obj_remove_style_all(act);
    lv_obj_set_size(act, 456, 56);
    lv_obj_align(act, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(act, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sv = lv_btn_create(act);
    style_primary_btn(sv);
    lv_obj_set_size(sv, 300, 52);
    sum_save_btn_label = lv_label_create(sv);
    lv_label_set_text(sum_save_btn_label, LV_SYMBOL_SAVE "  SPEICHERN");
    lv_obj_set_style_text_font(sum_save_btn_label, F_MD, 0);
    lv_obj_center(sum_save_btn_label);
    lv_obj_add_event_cb(sv, on_summary_save_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dc = lv_btn_create(act);
    style_ghost_btn(dc);
    lv_obj_set_size(dc, 146, 52);
    sum_discard_btn_label = lv_label_create(dc);
    lv_label_set_text(sum_discard_btn_label, "VERWERFEN");
    lv_obj_set_style_text_font(sum_discard_btn_label, F_SM, 0);
    lv_obj_center(sum_discard_btn_label);
    lv_obj_add_event_cb(dc, on_summary_discard_clicked, LV_EVENT_CLICKED, NULL);

    /* On-Screen Keyboard fuer die Textareas des Summary-Forms. */
    sum_keyboard = kb_create(scr_summary);
    kb_attach(sum_aufguss_ta, sum_keyboard);
    kb_attach(sum_notes_ta,   sum_keyboard);
}

static void summary_prefill(const struct view_data_session_meta *m) {
    sum_current_meta = *m;
    char info[96];
    uint32_t dur = (uint32_t)(m->end_ts - m->start_ts);
    char dbuf[16]; fmt_duration(dbuf, sizeof(dbuf), dur);
    snprintf(info, sizeof(info),
             "Dauer %s   Peak %.0f C / %.0f%%   %u Aufg.",
             dbuf,
             isnan(m->peak_temp) ? 0.0f : m->peak_temp,
             isnan(m->peak_rh)   ? 0.0f : m->peak_rh,
             (unsigned)m->aufguss_count);
    lv_label_set_text(sum_meta_summary, info);
    lv_textarea_set_text(sum_aufguss_ta, m->aufguss_headline);
    lv_textarea_set_text(sum_notes_ta, m->notes);
    sum_participants_n = m->participants;
    update_participants_label();
    sum_aufg_count_n = m->aufguss_count;
    update_aufg_count_label();
}

/* ======================================================================= */
/*   HISTORY                                                                 */
/* ======================================================================= */

static void on_history_row_clicked(lv_event_t *e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "on_history_row_clicked idx=%u cache_count=%u",
             (unsigned)idx, (unsigned)hist_cache_count);
    if (idx >= hist_cache_count) return;
    const struct view_data_session_meta *m = &hist_cached[idx];
    /* Index merken: detail-screen nutzt ihn fuer edit/delete */
    sum_edit_hist_idx = (int)idx;
    char sid[SSC_SESSION_ID_LEN];
    strncpy(sid, m->id, SSC_SESSION_ID_LEN - 1);
    sid[SSC_SESSION_ID_LEN - 1] = 0;
    ESP_LOGI(TAG, "  -> DETAIL_REQ post sid='%s'", sid);
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_HISTORY_DETAIL_REQ,
                      sid, SSC_SESSION_ID_LEN, portMAX_DELAY);

    if (detail_title) lv_label_set_text(detail_title, m->id);
    if (detail_sub) {
        char sub[96], ts[20]; fmt_datetime_short(ts, sizeof(ts), m->start_ts);
        snprintf(sub, sizeof(sub),
                 "%s  |  Peak %.0f C / %.0f%%  |  %u Aufg.",
                 ts,
                 isnan(m->peak_temp) ? 0.0f : m->peak_temp,
                 isnan(m->peak_rh)   ? 0.0f : m->peak_rh,
                 (unsigned)m->aufguss_count);
        lv_label_set_text(detail_sub, sub);
    }
    /* Buffer + chart reset bevor der readback startet. */
    s_detail_buf.count = 0;
    s_detail_buf.sid[0] = 0;
    if (detail_chart) {
        lv_chart_set_all_value(detail_chart, detail_ser_temp, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(detail_chart, detail_ser_rh,   LV_CHART_POINT_NONE);
        if (detail_ser_aufg)
            lv_chart_set_all_value(detail_chart, detail_ser_aufg, LV_CHART_POINT_NONE);
        lv_chart_refresh(detail_chart);
    }
    /* Loading-spinner einblenden bis DONE-event kommt. */
    if (detail_spinner) lv_obj_clear_flag(detail_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load_anim(scr_detail, LV_SCR_LOAD_ANIM_OVER_LEFT, 220, 0, false);
}

static void on_history_back_clicked(lv_event_t *e) {
    lv_scr_load_anim(scr_home, LV_SCR_LOAD_ANIM_OVER_RIGHT, 220, 0, false);
}

static void build_history(void) {
    scr_history = lv_obj_create(NULL);
    style_screen(scr_history);

    lv_obj_t *t = lv_label_create(scr_history);
    lv_label_set_text(t, "HISTORIE");
    lv_obj_set_style_text_color(t, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(t, F_LG, 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *sub = lv_label_create(scr_history);
    lv_label_set_text(sub, "alle aufgezeichneten saunagaenge");
    lv_obj_set_style_text_color(sub, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(sub, F_SM, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 48);

    hist_list = lv_list_create(scr_history);
    style_card(hist_list);
    lv_obj_set_size(hist_list, 456, 354);
    lv_obj_align(hist_list, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_obj_set_style_pad_all(hist_list, 0, 0);
    lv_obj_add_flag(hist_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(hist_list, LV_DIR_VER);

    lv_obj_t *back = lv_btn_create(scr_history);
    style_ghost_btn(back);
    lv_obj_set_size(back, 456, 38);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *lb = lv_label_create(back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT "  ZURUECK");
    lv_obj_set_style_text_font(lb, F_SM, 0);
    lv_obj_center(lb);
    lv_obj_add_event_cb(back, on_history_back_clicked, LV_EVENT_CLICKED, NULL);
}

static void history_apply(const struct view_data_session_list *L) {
    if (!hist_list) return;
    lv_obj_clean(hist_list);
    uint16_t n = L->count < 32 ? L->count : 32;
    hist_cache_count = n;
    for (uint16_t i = 0; i < n; i++) {
        hist_cached[i] = L->items[i];
        const struct view_data_session_meta *m = &hist_cached[i];
        char line[96], ts[20];
        fmt_datetime_short(ts, sizeof(ts), m->start_ts);
        /* Row-text: datum   operator  |  aufguss   •  N TN
         * aufguss-name nur wenn vorhanden, teilnehmer nur wenn >0.      */
        char middle[64] = {0};
        if (m->aufguss_headline[0]) {
            snprintf(middle, sizeof(middle), "%s%s",
                     m->operator_tag[0] ? "  |  " : "", m->aufguss_headline);
        }
        char tn[16] = {0};
        if (m->participants > 0) {
            snprintf(tn, sizeof(tn), "  \xE2\x80\xA2  %u TN",
                     (unsigned)m->participants);
        }
        snprintf(line, sizeof(line), "%s   %s%s%s",
                 ts,
                 m->operator_tag[0] ? m->operator_tag : "",
                 middle, tn);
        lv_obj_t *row = lv_list_add_btn(hist_list, NULL, line);
        lv_obj_set_style_bg_color(row, SSC_C_SURFACE, 0);
        lv_obj_set_style_bg_color(row, SSC_C_ELEVATED, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(row, SSC_C_TEXT, 0);
        lv_obj_set_style_text_font(row, F_MD, 0);   /* 20pt fuer lesbarkeit */
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_color(row, SSC_C_BORDER, LV_STATE_DEFAULT);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);

        lv_obj_t *right = lv_label_create(row);
        char peak[32];
        snprintf(peak, sizeof(peak), "%.0f C  %.0f%%   %u",
                 isnan(m->peak_temp) ? 0.0f : m->peak_temp,
                 isnan(m->peak_rh)   ? 0.0f : m->peak_rh,
                 (unsigned)m->aufguss_count);
        lv_label_set_text(right, peak);
        lv_obj_set_style_text_color(right, SSC_C_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(right, F_SM, 0);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_add_event_cb(row, on_history_row_clicked,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    if (n == 0) {
        lv_obj_t *e = lv_label_create(hist_list);
        lv_label_set_text(e, "noch keine sessions aufgezeichnet");
        lv_obj_set_style_text_color(e, SSC_C_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(e, F_SM, 0);
        lv_obj_center(e);
    }
    home_update_recent(L);
    /* Neue session-liste → operator-dropdown nach haeufigkeit re-sortieren. */
    rebuild_operator_dropdown();
}

/* ======================================================================= */
/*   DETAIL                                                                  */
/* ======================================================================= */

static void on_detail_back_clicked(lv_event_t *e) {
    lv_scr_load_anim(scr_history, LV_SCR_LOAD_ANIM_OVER_RIGHT, 220, 0, false);
}

static void do_detail_delete(void *user_data) {
    if (sum_edit_hist_idx < 0 || sum_edit_hist_idx >= hist_cache_count) return;
    const char *id = hist_cached[sum_edit_hist_idx].id;
    ESP_LOGI(TAG, "do_detail_delete sid=%s", id);
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_HISTORY_DELETE, id, SSC_SESSION_ID_LEN,
                      portMAX_DELAY);
    /* nach kurzer wartezeit wird die neue liste automatisch gepostet
     * vom session-modul-handler (delete loescht + on_history_list_req). */
    sum_edit_hist_idx = -1;
    lv_scr_load_anim(scr_history, LV_SCR_LOAD_ANIM_OVER_RIGHT, 220, 0, false);
}

static void on_detail_delete_clicked(lv_event_t *e) {
    (void)e;
    if (sum_edit_hist_idx < 0 || sum_edit_hist_idx >= hist_cache_count) return;
    char body[128];
    snprintf(body, sizeof(body),
             "session %s\nwirklich unwiderruflich loeschen?",
             hist_cached[sum_edit_hist_idx].id);
    show_confirm("SESSION LOESCHEN", body, "LOESCHEN",
                 do_detail_delete, NULL);
}

static void on_detail_edit_clicked(lv_event_t *e) {
    (void)e;
    if (sum_edit_hist_idx < 0 || sum_edit_hist_idx >= hist_cache_count) return;
    if (indicator_session_is_active()) {
        /* Defensive: edit waehrend live-session blocken */
        ESP_LOGW(TAG, "edit denied: session is live");
        return;
    }
    const struct view_data_session_meta *m = &hist_cached[sum_edit_hist_idx];
    ESP_LOGI(TAG, "on_detail_edit_clicked sid=%s", m->id);
    sum_edit_mode = true;
    summary_prefill(m);
    /* Labels an edit-mode anpassen */
    if (sum_title_label) lv_label_set_text(sum_title_label, "SESSION BEARBEITEN");
    if (sum_save_btn_label) lv_label_set_text(sum_save_btn_label,
                                              LV_SYMBOL_OK "  SPEICHERN");
    if (sum_discard_btn_label) lv_label_set_text(sum_discard_btn_label,
                                                 "ABBRECHEN");
    lv_scr_load_anim(scr_summary, LV_SCR_LOAD_ANIM_FADE_ON, 240, 0, false);
}

/* ---- Confirm-Modal (wiederverwendbar fuer delete, wipe-all, ...) ---- */

static lv_obj_t     *s_confirm_overlay = NULL;
static confirm_cb_t  s_confirm_cb = NULL;
static void         *s_confirm_user_data = NULL;

static void confirm_close(void) {
    if (s_confirm_overlay) {
        lv_obj_del(s_confirm_overlay);
        s_confirm_overlay = NULL;
    }
    s_confirm_cb = NULL;
    s_confirm_user_data = NULL;
}

static void on_confirm_cancel(lv_event_t *e) { (void)e; confirm_close(); }

static void on_confirm_ok(lv_event_t *e) {
    (void)e;
    confirm_cb_t cb = s_confirm_cb;
    void *ud = s_confirm_user_data;
    confirm_close();
    if (cb) cb(ud);
}

static void show_confirm(const char *title, const char *body,
                         const char *confirm_label,
                         confirm_cb_t on_ok, void *user_data) {
    if (s_confirm_overlay) confirm_close();
    s_confirm_cb = on_ok;
    s_confirm_user_data = user_data;

    /* Fullscreen modal-overlay mit semi-transparentem backdrop */
    lv_obj_t *active = lv_scr_act();
    s_confirm_overlay = lv_obj_create(active);
    lv_obj_remove_style_all(s_confirm_overlay);
    lv_obj_set_size(s_confirm_overlay, 480, 480);
    lv_obj_align(s_confirm_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_confirm_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_confirm_overlay, 180, 0);
    lv_obj_clear_flag(s_confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Zentrierte card */
    lv_obj_t *card = lv_obj_create(s_confirm_overlay);
    lv_obj_set_size(card, 360, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, SSC_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, SSC_C_BORDER, 0);
    lv_obj_set_style_radius(card, SSC_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(t, F_MD, 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *b = lv_label_create(card);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_label_set_text(b, body);
    lv_obj_set_style_text_color(b, SSC_C_TEXT, 0);
    lv_obj_set_style_text_font(b, F_SM, 0);
    lv_obj_set_width(b, 328);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, 38);

    /* Cancel-button (muted) links */
    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 150, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, SSC_C_ELEVATED, 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_border_color(cancel, SSC_C_BORDER, 0);
    lv_obj_set_style_radius(cancel, SSC_RADIUS_BTN, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "ABBRECHEN");
    lv_obj_set_style_text_color(cl, SSC_C_TEXT, 0);
    lv_obj_set_style_text_font(cl, F_SM, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, on_confirm_cancel, LV_EVENT_CLICKED, NULL);

    /* Confirm-button (rot) rechts */
    lv_obj_t *ok = lv_btn_create(card);
    lv_obj_set_size(ok, 150, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x7a2828), 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x9a3434), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 1, 0);
    lv_obj_set_style_border_color(ok, lv_color_hex(0xa04040), 0);
    lv_obj_set_style_radius(ok, SSC_RADIUS_BTN, 0);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, confirm_label);
    lv_obj_set_style_text_color(okl, lv_color_hex(0xffe0e0), 0);
    lv_obj_set_style_text_font(okl, F_SM, 0);
    lv_obj_center(okl);
    lv_obj_add_event_cb(ok, on_confirm_ok, LV_EVENT_CLICKED, NULL);
}

static void build_detail(void) {
    scr_detail = lv_obj_create(NULL);
    style_screen(scr_detail);

    detail_title = lv_label_create(scr_detail);
    lv_label_set_text(detail_title, "SESSION");
    lv_obj_set_style_text_color(detail_title, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(detail_title, F_MD, 0);
    lv_obj_set_style_text_letter_space(detail_title, 2, 0);
    lv_obj_align(detail_title, LV_ALIGN_TOP_LEFT, 0, 0);

    detail_sub = lv_label_create(scr_detail);
    lv_label_set_text(detail_sub, "");
    lv_obj_set_style_text_color(detail_sub, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(detail_sub, F_SM, 0);  /* F_XS war zu klein */
    lv_obj_align(detail_sub, LV_ALIGN_TOP_LEFT, 0, 30);

    /* Gleiche Strategie wie Live-Chart: Wrapper-Container mit
     * manuellen Labels + echtes Chart innen drin.                 */
    lv_obj_t *dchart_wrap = lv_obj_create(scr_detail);
    style_card(dchart_wrap);
    lv_obj_set_size(dchart_wrap, 456, 356);
    lv_obj_align(dchart_wrap, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_clear_flag(dchart_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(dchart_wrap, 0, 0);

    const int dyL[] = {120, 90, 60, 30, 0};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *l = lv_label_create(dchart_wrap);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", dyL[i]);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_color(l, SSC_C_CHART_TEMP, 0);
        lv_obj_set_style_text_font(l, F_XS, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        int y_pos = 12 + (i * 316) / 4 - 7;
        lv_obj_set_pos(l, 4, y_pos);
        lv_obj_set_size(l, 34, 14);
    }
    const int dyR[] = {100, 75, 50, 25, 0};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *l = lv_label_create(dchart_wrap);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", dyR[i]);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_color(l, SSC_C_CHART_RH, 0);
        lv_obj_set_style_text_font(l, F_XS, 0);
        int y_pos = 12 + (i * 316) / 4 - 7;
        lv_obj_set_pos(l, 456 - 36, y_pos);
        lv_obj_set_size(l, 32, 14);
    }
    lv_obj_t *dlu_c = lv_label_create(dchart_wrap);
    lv_label_set_text(dlu_c, "°C");
    lv_obj_set_style_text_color(dlu_c, SSC_C_CHART_TEMP, 0);
    lv_obj_set_style_text_font(dlu_c, F_XS, 0);
    lv_obj_set_pos(dlu_c, 4, 0);
    lv_obj_t *dlu_rh = lv_label_create(dchart_wrap);
    lv_label_set_text(dlu_rh, "%");
    lv_obj_set_style_text_color(dlu_rh, SSC_C_CHART_RH, 0);
    lv_obj_set_style_text_font(dlu_rh, F_XS, 0);
    lv_obj_set_pos(dlu_rh, 456 - 16, 0);

    detail_chart = lv_chart_create(dchart_wrap);
    lv_obj_set_size(detail_chart, 380, 320);
    lv_obj_set_pos(detail_chart, 38, 14);
    lv_chart_set_type(detail_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(detail_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(detail_chart, DETAIL_CHART_POINTS);
    lv_chart_set_range(detail_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 120);
    lv_chart_set_range(detail_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    lv_chart_set_div_line_count(detail_chart, 5, 5);
    lv_obj_set_style_pad_all(detail_chart, 0, 0);
    lv_obj_set_style_line_color(detail_chart, SSC_C_CHART_GRID, LV_PART_MAIN);
    lv_obj_set_style_bg_color(detail_chart, SSC_C_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(detail_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(detail_chart, SSC_C_BORDER, LV_PART_MAIN);

    detail_ser_temp = lv_chart_add_series(detail_chart, SSC_C_CHART_TEMP,
                                          LV_CHART_AXIS_PRIMARY_Y);
    detail_ser_rh   = lv_chart_add_series(detail_chart, SSC_C_CHART_RH,
                                          LV_CHART_AXIS_SECONDARY_Y);
    /* Aufguss-marker als duenne spikes von 0 bis 100 (auf RH-achse),
     * akzent-farbe. An aufguss-positionen value=100, sonst NONE.  */
    detail_ser_aufg = lv_chart_add_series(detail_chart, SSC_C_ACCENT,
                                          LV_CHART_AXIS_SECONDARY_Y);
    lv_obj_set_style_line_width(detail_chart, 2, LV_PART_ITEMS);

    /* Loading-indikator: simples label statt lv_spinner, weil spinner
     * in bestimmten render-konstellationen LVGL shadow_blur_corner
     * crashen lies (LoadProhibited).                                 */
    detail_spinner = lv_label_create(scr_detail);
    lv_label_set_text(detail_spinner, LV_SYMBOL_LOOP "  LADE DATEN...");
    lv_obj_set_style_text_color(detail_spinner, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(detail_spinner, F_MD, 0);
    lv_obj_align(detail_spinner, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(detail_spinner, LV_OBJ_FLAG_HIDDEN);

    /* Button-row unten: BEARBEITEN | LOESCHEN | ZURUECK */
    lv_obj_t *btn_edit = lv_btn_create(scr_detail);
    lv_obj_set_size(btn_edit, 142, 38);
    lv_obj_align(btn_edit, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn_edit, SSC_C_ACCENT, 0);
    lv_obj_set_style_bg_color(btn_edit, SSC_C_ACCENT_HOV, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_edit, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_edit, SSC_RADIUS_BTN, 0);
    lv_obj_set_style_text_color(btn_edit, lv_color_hex(0x0a0a0a), 0);
    lv_obj_set_style_border_width(btn_edit, 0, 0);
    lv_obj_t *le = lv_label_create(btn_edit);
    lv_label_set_text(le, LV_SYMBOL_EDIT "  BEARBEITEN");
    lv_obj_set_style_text_font(le, F_XS, 0);
    lv_obj_center(le);
    lv_obj_add_event_cb(btn_edit, on_detail_edit_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_del = lv_btn_create(scr_detail);
    lv_obj_set_size(btn_del, 142, 38);
    lv_obj_align(btn_del, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_del, lv_color_hex(0x5c1e1e), 0);
    lv_obj_set_style_bg_color(btn_del, lv_color_hex(0x7a2828), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_del, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_del, SSC_RADIUS_BTN, 0);
    lv_obj_set_style_text_color(btn_del, lv_color_hex(0xffd0d0), 0);
    lv_obj_set_style_border_width(btn_del, 1, 0);
    lv_obj_set_style_border_color(btn_del, lv_color_hex(0x8a3a3a), 0);
    lv_obj_t *ld = lv_label_create(btn_del);
    lv_label_set_text(ld, LV_SYMBOL_TRASH "  LOESCHEN");
    lv_obj_set_style_text_font(ld, F_XS, 0);
    lv_obj_center(ld);
    lv_obj_add_event_cb(btn_del, on_detail_delete_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(scr_detail);
    style_ghost_btn(back);
    lv_obj_set_size(back, 142, 38);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_t *lb = lv_label_create(back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT "  ZURUECK");
    lv_obj_set_style_text_font(lb, F_XS, 0);
    lv_obj_center(lb);
    lv_obj_add_event_cb(back, on_detail_back_clicked, LV_EVENT_CLICKED, NULL);
}

/* ======================================================================= */
/*   SETTINGS                                                                */
/* ======================================================================= */

static void on_settings_back_clicked(lv_event_t *e) {
    lv_scr_load_anim(scr_home, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 220, 0, false);
}

static void on_wifi_connect_clicked(lv_event_t *e) {
    struct view_data_wifi_config cfg = {0};
    const char *ssid = lv_textarea_get_text(set_wifi_ssid_ta);
    const char *pw   = lv_textarea_get_text(set_wifi_pw_ta);
    if (!ssid || !ssid[0]) return;
    strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
    if (pw && pw[0]) {
        strncpy((char *)cfg.password, pw, sizeof(cfg.password) - 1);
        cfg.have_password = true;
    }
    if (set_wifi_info) {
        lv_label_set_text(set_wifi_info, "verbinde...");
        lv_obj_set_style_text_color(set_wifi_info, SSC_C_ACCENT, 0);
    }
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_WIFI_CONNECT, &cfg, sizeof(cfg),
                      portMAX_DELAY);
}

static void on_http_apply_clicked(lv_event_t *e) {
    struct view_data_http_cfg c = {0};
    c.enabled = lv_obj_has_state(set_http_enabled_sw, LV_STATE_CHECKED);
    strncpy(c.url, lv_textarea_get_text(set_http_url_ta), sizeof(c.url) - 1);
    strncpy(c.bearer_token, lv_textarea_get_text(set_http_token_ta),
            sizeof(c.bearer_token) - 1);
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_HTTP_CFG_APPLY, &c, sizeof(c), portMAX_DELAY);
}

/* Parst operator-liste aus textarea - entweder komma-getrennt oder
 * newline-getrennt (beide funktionieren, whitespace wird getrimmt). */
static void parse_operators(const char *txt,
                             struct view_data_operator_presets *p) {
    memset(p, 0, sizeof(*p));
    char line[SSC_OPERATOR_MAXLEN];
    size_t li = 0;
    for (size_t i = 0; ; i++) {
        char c = txt[i];
        bool is_sep = (c == '\n' || c == ',' || c == 0);
        if (is_sep) {
            /* trim leading/trailing whitespace */
            size_t start = 0;
            while (start < li && (line[start] == ' ' || line[start] == '\t')) start++;
            size_t end = li;
            while (end > start && (line[end-1] == ' ' || line[end-1] == '\t')) end--;
            if (end > start && p->count < SSC_OPERATOR_PRESETS_MAX) {
                size_t len = end - start;
                if (len > SSC_OPERATOR_MAXLEN - 1) len = SSC_OPERATOR_MAXLEN - 1;
                memcpy(p->items[p->count], &line[start], len);
                p->items[p->count][len] = 0;
                p->count++;
            }
            li = 0;
            if (c == 0) break;
        } else if (li < SSC_OPERATOR_MAXLEN - 1) {
            line[li++] = c;
        }
    }
}

static void on_operators_apply_clicked(lv_event_t *e) {
    struct view_data_operator_presets p = {0};
    parse_operators(lv_textarea_get_text(set_operators_ta), &p);
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_OPERATOR_PRESETS_UPDATE, &p, sizeof(p),
                      portMAX_DELAY);
}

static void do_wipe_all(void *user_data) {
    (void)user_data;
    ESP_LOGW(TAG, "user confirmed WIPE ALL sessions");
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_HISTORY_WIPE_ALL, NULL, 0, portMAX_DELAY);
}

static void on_wipe_all_clicked(lv_event_t *e) {
    (void)e;
    show_confirm("ALLE SESSIONS LOESCHEN",
                 "wirklich alle sessions aus der historie loeschen?\n"
                 "die daten auf SD bleiben, NVS wird geleert.",
                 "JA, LOESCHEN", do_wipe_all, NULL);
}

/* Default-Liste von supersauna.club/mitglieder abgerufen.
 * Wird beim boot verwendet wenn NVS noch leer ist.                */
static const char *SSC_DEFAULT_OPERATORS =
    "Thomas, Alexander, Christopher, Robert, Benjamin, Kevin, Heinz, "
    "Bernhard, Franz, Patrick, Lukas, Bernhard V., Marco, Mario, "
    "Patrick P., Gabriel";

static lv_obj_t *section_title(lv_obj_t *parent, const char *txt) {
    /* Container mit oben-pad + trennlinie fuer sichtbar getrennte
     * sektionen. Titel drin als F_MD (groesser + deutlicher).        */
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, 420, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(wrap, 22, 0);
    lv_obj_set_style_pad_bottom(wrap, 6, 0);
    lv_obj_set_style_border_width(wrap, 1, 0);
    lv_obj_set_style_border_color(wrap, SSC_C_BORDER, 0);
    lv_obj_set_style_border_side(wrap, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(wrap);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(l, F_MD, 0);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 12);
    return wrap;
}

static lv_obj_t *kv_label(lv_obj_t *parent, const char *k) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, k);
    label_muted(l);
    return l;
}

static lv_obj_t *make_textarea(lv_obj_t *parent, const char *placeholder,
                                bool pw) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, pw);
    lv_obj_set_size(ta, 420, 36);
    lv_obj_set_style_bg_color(ta, SSC_C_ELEVATED, 0);
    lv_obj_set_style_text_color(ta, SSC_C_TEXT, 0);
    lv_obj_set_style_text_font(ta, F_SM, 0);
    return ta;
}

static void build_settings(void) {
    scr_settings = lv_obj_create(NULL);
    style_screen(scr_settings);

    lv_obj_t *t = lv_label_create(scr_settings);
    lv_label_set_text(t, "EINSTELLUNGEN");
    lv_obj_set_style_text_color(t, SSC_C_ACCENT, 0);
    lv_obj_set_style_text_font(t, F_MD, 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Scrollbarer body */
    lv_obj_t *body = lv_obj_create(scr_settings);
    style_card(body);
    lv_obj_set_size(body, 456, 388);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_style_pad_all(body, 14, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    /* ---- Saunameister-Presets (ganz oben - tagesgeschaeft) ---- */
    section_title(body, "SAUNAMEISTER");
    lv_obj_t *op_hint = lv_label_create(body);
    lv_label_set_text(op_hint, "namen komma-getrennt (max 24)");
    lv_obj_set_style_text_color(op_hint, SSC_C_TEXT_FAINT, 0);
    lv_obj_set_style_text_font(op_hint, F_XS, 0);

    set_operators_ta = lv_textarea_create(body);
    lv_obj_set_size(set_operators_ta, 420, 120);
    lv_obj_set_style_bg_color(set_operators_ta, SSC_C_ELEVATED, 0);
    lv_obj_set_style_text_color(set_operators_ta, SSC_C_TEXT, 0);
    lv_textarea_set_placeholder_text(set_operators_ta, "Thomas, Alexander, Robert, ...");

    lv_obj_t *op_apply = lv_btn_create(body);
    style_primary_btn(op_apply);
    lv_obj_set_size(op_apply, 420, 42);
    lv_obj_t *opl = lv_label_create(op_apply);
    lv_label_set_text(opl, LV_SYMBOL_OK "  SAUNAMEISTER SPEICHERN");
    lv_obj_set_style_text_font(opl, F_SM, 0);
    lv_obj_center(opl);
    lv_obj_add_event_cb(op_apply, on_operators_apply_clicked,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *sep0 = lv_obj_create(body);
    lv_obj_remove_style_all(sep0);
    lv_obj_set_size(sep0, 420, 1);
    lv_obj_set_style_bg_color(sep0, SSC_C_BORDER, 0);
    lv_obj_set_style_bg_opa(sep0, LV_OPA_COVER, 0);

    /* ---- WLAN-Eingabe: SSID + Passwort + Verbinden ---- */
    section_title(body, "WLAN");
    set_wifi_info = lv_label_create(body);
    lv_label_set_text(set_wifi_info, "status: unbekannt");
    lv_obj_set_style_text_color(set_wifi_info, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(set_wifi_info, F_SM, 0);

    kv_label(body, "SSID");
    set_wifi_ssid_ta = make_textarea(body, "netzwerk-name", false);
    kv_label(body, "PASSWORT");
    set_wifi_pw_ta = make_textarea(body, "passwort", true);

    lv_obj_t *wifi_connect = lv_btn_create(body);
    style_primary_btn(wifi_connect);
    lv_obj_set_size(wifi_connect, 420, 42);
    lv_obj_t *wcl = lv_label_create(wifi_connect);
    lv_label_set_text(wcl, LV_SYMBOL_WIFI "  VERBINDEN");
    lv_obj_set_style_text_font(wcl, F_SM, 0);
    lv_obj_center(wcl);
    lv_obj_add_event_cb(wifi_connect, on_wifi_connect_clicked,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *sep1 = lv_obj_create(body);
    lv_obj_remove_style_all(sep1);
    lv_obj_set_size(sep1, 420, 1);
    lv_obj_set_style_bg_color(sep1, SSC_C_BORDER, 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);

    /* HINWEIS: Die Supersauna.club-API-Section ist nach unten verschoben
     * und ausgegraut ("ZUKUNFTS-FEATURE"), siehe weiter unten nach INFO.
     * Die globalen set_http_*-Variablen werden trotzdem dort allokiert
     * damit on_http_apply_clicked / existierende Callbacks nicht segfaulten. */

    /* ---- MariaDB ---- */
    section_title(body, "MARIADB");
    kv_label(body, "AKTIV");
    set_db_enabled_sw = lv_switch_create(body);
    lv_obj_set_style_bg_color(set_db_enabled_sw, SSC_C_ELEVATED, 0);
    lv_obj_set_style_bg_color(set_db_enabled_sw, SSC_C_ACCENT, LV_STATE_CHECKED|LV_PART_INDICATOR);

    kv_label(body, "HOST");
    set_db_host_ta = make_textarea(body, "postl.ai", false);
    kv_label(body, "PORT");
    set_db_port_sb = lv_spinbox_create(body);
    lv_spinbox_set_range(set_db_port_sb, 1, 65535);
    lv_spinbox_set_digit_format(set_db_port_sb, 5, 0);
    lv_spinbox_set_value(set_db_port_sb, 3308);
    lv_obj_set_size(set_db_port_sb, 120, 36);
    kv_label(body, "BENUTZER");
    set_db_user_ta = make_textarea(body, "user", false);
    kv_label(body, "PASSWORT");
    set_db_pw_ta = make_textarea(body, "***", true);
    kv_label(body, "DATENBANK");
    set_db_database_ta = make_textarea(body, "sscsauna", false);
    kv_label(body, "TABELLE (legacy sensor_data)");
    set_db_table_ta = make_textarea(body, "sensor_data", false);

    lv_obj_t *sep3 = lv_obj_create(body);
    lv_obj_remove_style_all(sep3);
    lv_obj_set_size(sep3, 420, 1);
    lv_obj_set_style_bg_color(sep3, SSC_C_BORDER, 0);
    lv_obj_set_style_bg_opa(sep3, LV_OPA_COVER, 0);

    /* ---- Info ---- */

    section_title(body, "INFO");
    lv_obj_t *info = lv_label_create(body);
    lv_label_set_text(info,
        "Super Sauna Club - Sauna Logger\n"
        "Version " SSC_APP_VERSION "\n"
        "Open Source | Apache 2.0\n"
        "supersauna.club | Oberes Piestingtal\n"
        "SenseCAP D1S | ESP32-S3 + RP2040\n"
        "KI-assistierte Entwicklung");
    lv_obj_set_style_text_color(info, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(info, F_XS, 0);

    /* ---- ZUKUNFTS-FEATURE: supersauna.club API (ausgegraut) ------------
     * Endpoint zum Push von Session-Daten an ein zentrales Vereins-
     * Dashboard. Server-Seite existiert noch nicht, daher visuell
     * disabled. Variablen werden angelegt damit on_http_apply_clicked
     * nicht auf NULL greift - Klick macht aber nichts Sinnvolles. */
    lv_obj_t *sep_future = lv_obj_create(body);
    lv_obj_remove_style_all(sep_future);
    lv_obj_set_size(sep_future, 420, 24);
    lv_obj_set_style_bg_opa(sep_future, LV_OPA_TRANSP, 0);

    section_title(body, "ZUKUNFTS-FEATURE:\nSUPERSAUNA.CLUB API");

    lv_obj_t *future_hint = lv_label_create(body);
    lv_label_set_long_mode(future_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(future_hint,
        "Automatischer Push der Session-Daten an das Vereins-Dashboard. "
        "Server noch in Planung - UI aktuell nur Platzhalter.");
    lv_obj_set_style_text_color(future_hint, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(future_hint, F_XS, 0);
    lv_obj_set_width(future_hint, 420);

    kv_label(body, "AKTIV");
    set_http_enabled_sw = lv_switch_create(body);
    lv_obj_set_style_bg_color(set_http_enabled_sw, SSC_C_ELEVATED, 0);
    lv_obj_set_style_bg_color(set_http_enabled_sw, SSC_C_ACCENT, LV_STATE_CHECKED|LV_PART_INDICATOR);
    lv_obj_add_state(set_http_enabled_sw, LV_STATE_DISABLED);
    lv_obj_set_style_opa(set_http_enabled_sw, LV_OPA_40, 0);

    kv_label(body, "URL");
    set_http_url_ta = make_textarea(body, "https://supersauna.club/api/session", false);
    lv_obj_add_state(set_http_url_ta, LV_STATE_DISABLED);
    lv_obj_set_style_opa(set_http_url_ta, LV_OPA_40, 0);

    kv_label(body, "BEARER TOKEN");
    set_http_token_ta = make_textarea(body, "optional", true);
    lv_obj_add_state(set_http_token_ta, LV_STATE_DISABLED);
    lv_obj_set_style_opa(set_http_token_ta, LV_OPA_40, 0);

    lv_obj_t *http_apply = lv_btn_create(body);
    style_primary_btn(http_apply);
    lv_obj_set_size(http_apply, 420, 42);
    lv_obj_t *hal = lv_label_create(http_apply);
    lv_label_set_text(hal, LV_SYMBOL_OK "  HTTP-KONFIG SPEICHERN");
    lv_obj_set_style_text_font(hal, F_SM, 0);
    lv_obj_center(hal);
    lv_obj_add_event_cb(http_apply, on_http_apply_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(http_apply, LV_STATE_DISABLED);
    lv_obj_set_style_opa(http_apply, LV_OPA_40, 0);

    /* ---- DANGER ZONE (ganz unten, extra abstand, rot markiert) ---- */
    lv_obj_t *sep_dz = lv_obj_create(body);
    lv_obj_remove_style_all(sep_dz);
    lv_obj_set_size(sep_dz, 420, 24);
    lv_obj_set_style_bg_opa(sep_dz, LV_OPA_TRANSP, 0);

    lv_obj_t *dz_card = lv_obj_create(body);
    lv_obj_set_size(dz_card, 420, 128);
    lv_obj_set_style_bg_color(dz_card, lv_color_hex(0x2a1010), 0);
    lv_obj_set_style_bg_opa(dz_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dz_card, 1, 0);
    lv_obj_set_style_border_color(dz_card, lv_color_hex(0x8a3a3a), 0);
    lv_obj_set_style_radius(dz_card, SSC_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(dz_card, 12, 0);
    lv_obj_clear_flag(dz_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dz_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dz_card, 4, 0);

    lv_obj_t *dz_t = lv_label_create(dz_card);
    lv_label_set_text(dz_t, LV_SYMBOL_WARNING "  DANGER ZONE");
    lv_obj_set_style_text_color(dz_t, lv_color_hex(0xff8080), 0);
    lv_obj_set_style_text_font(dz_t, F_MD, 0);
    lv_obj_set_style_text_letter_space(dz_t, 2, 0);

    lv_obj_t *dz_body = lv_label_create(dz_card);
    lv_label_set_long_mode(dz_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(dz_body,
        "kann nicht rueckgaengig gemacht werden.\n"
        "alle sessions in der historie verschwinden (SD bleibt).");
    lv_obj_set_style_text_color(dz_body, SSC_C_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(dz_body, F_XS, 0);
    lv_obj_set_width(dz_body, 396);

    lv_obj_t *wipe_btn = lv_btn_create(dz_card);
    lv_obj_set_size(wipe_btn, 396, 40);
    lv_obj_set_style_bg_color(wipe_btn, lv_color_hex(0x5c1e1e), 0);
    lv_obj_set_style_bg_color(wipe_btn, lv_color_hex(0x7a2828), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(wipe_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wipe_btn, SSC_RADIUS_BTN, 0);
    lv_obj_set_style_border_width(wipe_btn, 1, 0);
    lv_obj_set_style_border_color(wipe_btn, lv_color_hex(0xa04040), 0);
    lv_obj_t *wipe_l = lv_label_create(wipe_btn);
    lv_label_set_text(wipe_l, LV_SYMBOL_TRASH "  ALLE SESSIONS LOESCHEN");
    lv_obj_set_style_text_color(wipe_l, lv_color_hex(0xffd0d0), 0);
    lv_obj_set_style_text_font(wipe_l, F_SM, 0);
    lv_obj_center(wipe_l);
    lv_obj_add_event_cb(wipe_btn, on_wipe_all_clicked,
                        LV_EVENT_CLICKED, NULL);

    /* Back-button */
    lv_obj_t *back = lv_btn_create(scr_settings);
    style_ghost_btn(back);
    lv_obj_set_size(back, 456, 34);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *lb = lv_label_create(back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT "  ZURUECK");
    lv_obj_set_style_text_font(lb, F_SM, 0);
    lv_obj_center(lb);
    lv_obj_add_event_cb(back, on_settings_back_clicked, LV_EVENT_CLICKED, NULL);

    /* On-Screen Keyboard fuer alle Settings-Textareas. */
    set_keyboard = kb_create(scr_settings);
    kb_attach(set_wifi_ssid_ta,   set_keyboard);
    kb_attach(set_wifi_pw_ta,     set_keyboard);
    kb_attach(set_http_url_ta,    set_keyboard);
    kb_attach(set_http_token_ta,  set_keyboard);
    kb_attach(set_db_host_ta,     set_keyboard);
    kb_attach(set_db_user_ta,     set_keyboard);
    kb_attach(set_db_pw_ta,       set_keyboard);
    kb_attach(set_db_database_ta, set_keyboard);
    kb_attach(set_db_table_ta,    set_keyboard);
    kb_attach(set_operators_ta,   set_keyboard);
}

/* ======================================================================= */
/*   Event-Bridge (vom esp_event-Loop-Task)                                  */
/* ======================================================================= */

static void on_session_live(const struct view_data_session_live *L) {
    /* Erste 3 events + dann alle 10s loggen, damit man im monitor sieht
     * dass die events das UI erreichen und der chart sollte tickern.  */
    static uint32_t live_rx_cnt = 0;
    live_rx_cnt++;
    if (live_rx_cnt <= 3 || (live_rx_cnt % 10) == 0) {
        ESP_LOGI(TAG, "on_session_live rx=%lu dt=%lu temp=%.1f rh=%.1f",
                 (unsigned long)live_rx_cnt, (unsigned long)L->t_elapsed_s,
                 L->temp, L->rh);
    }
    char buf[32];
    if (!isnan(L->temp)) snprintf(buf, sizeof(buf), "%.1f", L->temp);
    else                 snprintf(buf, sizeof(buf), "-");
    if (live_temp_val) lv_label_set_text(live_temp_val, buf);

    if (!isnan(L->rh))   snprintf(buf, sizeof(buf), "%.0f", L->rh);
    else                 snprintf(buf, sizeof(buf), "-");
    if (live_rh_val) lv_label_set_text(live_rh_val, buf);

    if (!isnan(L->peak_temp))
        snprintf(buf, sizeof(buf), "%.0f C", L->peak_temp);
    else snprintf(buf, sizeof(buf), "- C");
    if (live_peak_val) lv_label_set_text(live_peak_val, buf);

    if (!isnan(L->peak_rh))
        snprintf(buf, sizeof(buf), "%.0f %%", L->peak_rh);
    else snprintf(buf, sizeof(buf), "- %%");
    if (live_peak_val2) lv_label_set_text(live_peak_val2, buf);

    fmt_duration(buf, sizeof(buf), L->t_elapsed_s);
    if (live_timer_val) lv_label_set_text(live_timer_val, buf);

    snprintf(buf, sizeof(buf), "%u aufg.", (unsigned)L->aufguss_count);
    if (live_aufguss_count_val) lv_label_set_text(live_aufguss_count_val, buf);

    /* Push chart-Point jede sekunde - 180 slots * 1 s = 3-min-fenster,
     * "schoen mitgeschrieben" wie vom user gewuenscht.               */
    if (live_chart) {
        if (!isnan(L->temp))
            lv_chart_set_next_value(live_chart, live_ser_temp,
                                    (lv_coord_t)L->temp);
        if (!isnan(L->rh))
            lv_chart_set_next_value(live_chart, live_ser_rh,
                                    (lv_coord_t)L->rh);
    }
}

static void on_sensor_data(const struct view_data_sensor_data *d) {
    char buf[24];
    switch (d->sensor_type) {
    case SENSOR_DATA_SAUNA_TEMP:
        if (!isnan(d->vaule)) {
            snprintf(buf, sizeof(buf), "%.1f", d->vaule);
            if (live_temp_val) lv_label_set_text(live_temp_val, buf);
            /* Split-labels: int-teil GROSS, ".X °C" klein. */
            if (home_sauna_temp_int) {
                int i = (int)d->vaule;
                int frac = (int)((d->vaule - i) * 10.0f + 0.5f);
                if (frac < 0) frac = -frac;
                if (frac >= 10) { i += 1; frac = 0; }
                char sbuf[8];
                snprintf(sbuf, sizeof(sbuf), "%d", i);
                lv_label_set_text(home_sauna_temp_int, sbuf);
                char fbuf[12];
                snprintf(fbuf, sizeof(fbuf), ".%d \xC2\xB0""C", frac);
                if (home_sauna_temp_frac)
                    lv_label_set_text(home_sauna_temp_frac, fbuf);
            }
        }
        break;
    case SENSOR_DATA_SAUNA_RH:
        if (!isnan(d->vaule)) {
            snprintf(buf, sizeof(buf), "%.0f", d->vaule);
            if (live_rh_val) lv_label_set_text(live_rh_val, buf);
            if (home_sauna_rh_int) {
                lv_label_set_text(home_sauna_rh_int, buf);
            }
        }
        break;
    case SENSOR_DATA_TEMP:
        snprintf(buf, sizeof(buf), "%.1f", d->vaule);
        if (home_vorraum_temp_val) lv_label_set_text(home_vorraum_temp_val, buf);
        break;
    case SENSOR_DATA_HUMIDITY:
        snprintf(buf, sizeof(buf), "%.0f", d->vaule);
        if (home_vorraum_rh_val) lv_label_set_text(home_vorraum_rh_val, buf);
        break;
    case SENSOR_DATA_CO2:
        snprintf(buf, sizeof(buf), "%.0f", d->vaule);
        if (home_vorraum_co2_val) lv_label_set_text(home_vorraum_co2_val, buf);
        break;
    default: break;
    }
}

static void on_history_list(const struct view_data_session_list *L) {
    ESP_LOGI(TAG, "on_history_list: count=%u total=%u items=%p",
             (unsigned)L->count, (unsigned)L->total, (const void *)L->items);
    history_apply(L);
}

/* Akkumulator: samples werden NICHT direkt in den chart geschrieben
 * (LVGL resettet bei jedem set_point_count den write-cursor, samples
 * wuerden ueberschrieben). Stattdessen hier in PSRAM buffern, chart
 * wird in on_history_detail_done einmalig final befuellt.               */
static void on_history_detail_chunk(const struct view_data_session_samples_chunk *c) {
    if (!c) return;
    ESP_LOGI(TAG, "detail_chunk: sid=%s off=%u cnt=%u total=%u",
             c->session_id, (unsigned)c->offset, (unsigned)c->count,
             (unsigned)c->total);
    if (!s_detail_buf.temps || !s_detail_buf.rhs) {
        ESP_LOGE(TAG, "detail_chunk: PSRAM-buffer nicht alloziert, drop");
        return;
    }
    if (c->offset == 0) {
        s_detail_buf.count = 0;
        strncpy(s_detail_buf.sid, c->session_id, SSC_SESSION_ID_LEN - 1);
        s_detail_buf.sid[SSC_SESSION_ID_LEN - 1] = 0;
    }
    /* Session-id sanity check - bei multi-click oder stale chunks. */
    if (strncmp(s_detail_buf.sid, c->session_id, SSC_SESSION_ID_LEN) != 0) {
        ESP_LOGW(TAG, "detail_chunk: sid mismatch buf='%s' chunk='%s', drop",
                 s_detail_buf.sid, c->session_id);
        return;
    }
    for (uint16_t i = 0; i < c->count && s_detail_buf.count < s_detail_buf.cap; i++) {
        const struct view_data_session_sample *s = &c->samples[i];
        s_detail_buf.temps[s_detail_buf.count]   =
            isnan(s->temp) ? INT16_MIN : (int16_t)s->temp;
        s_detail_buf.rhs[s_detail_buf.count]     =
            isnan(s->rh)   ? INT16_MIN : (int16_t)s->rh;
        if (s_detail_buf.markers) {
            s_detail_buf.markers[s_detail_buf.count] = s->has_aufguss_marker;
        }
        s_detail_buf.count++;
    }
}

/* Signal: readback komplett. Chart einmalig final befuellen. */
static void on_history_detail_done(const struct view_data_session_detail_done *d) {
    if (!d || !detail_chart) return;
    if (!s_detail_buf.temps || !s_detail_buf.rhs) {
        ESP_LOGE(TAG, "detail_done: PSRAM-buffer nicht alloziert - abort");
        if (detail_spinner) lv_obj_add_flag(detail_spinner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    ESP_LOGI(TAG, "detail_done: sid=%s count=%u", d->session_id,
             (unsigned)s_detail_buf.count);
    if (strncmp(s_detail_buf.sid, d->session_id, SSC_SESSION_ID_LEN) != 0) {
        ESP_LOGW(TAG, "detail_done: sid mismatch, ignore");
        return;
    }
    uint16_t n = s_detail_buf.count;
    if (n < 2) n = 2;   /* LVGL braucht >=2 points fuer line-render */
    lv_chart_set_point_count(detail_chart, n);
    lv_chart_set_all_value(detail_chart, detail_ser_temp, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(detail_chart, detail_ser_rh,   LV_CHART_POINT_NONE);
    if (detail_ser_aufg)
        lv_chart_set_all_value(detail_chart, detail_ser_aufg, LV_CHART_POINT_NONE);
    for (uint16_t i = 0; i < s_detail_buf.count; i++) {
        lv_coord_t t  = (s_detail_buf.temps[i] == INT16_MIN)
                        ? LV_CHART_POINT_NONE : s_detail_buf.temps[i];
        lv_coord_t rh = (s_detail_buf.rhs[i]   == INT16_MIN)
                        ? LV_CHART_POINT_NONE : s_detail_buf.rhs[i];
        lv_chart_set_next_value(detail_chart, detail_ser_temp, t);
        lv_chart_set_next_value(detail_chart, detail_ser_rh, rh);
        /* Aufguss-marker als spike 0..100 auf RH-skala - zeigt dort wo
         * ein aufguss war eine deutliche vertikale linie.             */
        lv_coord_t mk = LV_CHART_POINT_NONE;
        if (s_detail_buf.markers && s_detail_buf.markers[i]) mk = 100;
        if (detail_ser_aufg) {
            lv_chart_set_next_value(detail_chart, detail_ser_aufg, mk);
        }
    }
    lv_chart_refresh(detail_chart);
    /* Loading-spinner ausblenden */
    if (detail_spinner) lv_obj_add_flag(detail_spinner, LV_OBJ_FLAG_HIDDEN);
}

/* ----- Operator-Presets NVS-Persistenz ---------------------------- */
#define SSC_OPS_NVS_NS   "sauna_ops"
#define SSC_OPS_NVS_KEY  "presets"

static void operators_nvs_save(const struct view_data_operator_presets *p) {
    if (!p) return;
    nvs_handle_t h;
    if (nvs_open(SSC_OPS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, SSC_OPS_NVS_KEY, p, sizeof(*p));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "operator presets saved: count=%u", (unsigned)p->count);
}

static int operators_nvs_load(struct view_data_operator_presets *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(SSC_OPS_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -2;
    size_t sz = sizeof(*out);
    esp_err_t e = nvs_get_blob(h, SSC_OPS_NVS_KEY, out, &sz);
    nvs_close(h);
    if (e != ESP_OK) return -3;
    return 0;
}

static void on_probe_state(const struct view_data_probe_state *ps) {
    if (!home_status_pill) return;

    /* EIN zusammenhaengender Status-Pill rechts oben in der Kabinen-
     * Box. Form: "<probe> - <status>". bg-farbe = state-color.       */
    const char *probe_name = "?";
    bool has_external = false;
    switch (ps->probe) {
    case 1: probe_name = "SHT3X";       has_external = true;  break;
    case 2: probe_name = "AHT20 FB";    has_external = true;  break;
    case 3: probe_name = "SCD41 PROXY"; has_external = false; break;
    default: probe_name = "KEIN FUEHLER"; has_external = false; break;
    }

    /* Inline-format: "KABINE (PROBE: STATUS)" - kein separates pill,
     * links oben in der cabin-card.                                 */
    char txt[80];
    if (has_external) {
        if (ps->sd_init)
            snprintf(txt, sizeof(txt), "KABINE (%s: OK)", probe_name);
        else
            snprintf(txt, sizeof(txt), "KABINE (%s: OK, SD FEHLT)", probe_name);
    } else {
        if (ps->sd_init)
            snprintf(txt, sizeof(txt), "KABINE (KEIN FUEHLER)");
        else
            snprintf(txt, sizeof(txt), "KABINE (KEIN FUEHLER, SD FEHLT)");
    }
    lv_label_set_text(home_status_pill, txt);

    /* Farben */
    /* Nur text-farbe aendern (kein pill-background mehr). */
    lv_color_t text_col;
    if (has_external && ps->sd_init)       text_col = SSC_C_ACCENT;
    else if (has_external)                 text_col = SSC_C_ACCENT_HOV;
    else                                   text_col = SSC_C_WARNING;
    lv_obj_set_style_text_color(home_status_pill, text_col, 0);

    if (!has_external) {
        if (home_sauna_temp_val) lv_label_set_text(home_sauna_temp_val, "-");
        if (home_sauna_rh_val)   lv_label_set_text(home_sauna_rh_val,   "-");
    }
}

/* Letzter preset-zustand fuer re-sort bei neuer history-liste. */
static struct view_data_operator_presets s_preset_cache;
static bool s_preset_cache_valid = false;

/* Zaehlt wie oft `name` als operator in hist_cached[] vorkommt. */
static uint16_t operator_session_count(const char *name) {
    if (!name || !name[0]) return 0;
    uint16_t c = 0;
    for (uint16_t i = 0; i < hist_cache_count; i++) {
        if (strncmp(hist_cached[i].operator_tag, name,
                    SSC_OPERATOR_MAXLEN) == 0) c++;
    }
    return c;
}

/* Baut die dropdown-optionen aus s_preset_cache, sortiert nach
 * session-haeufigkeit (haeufigste zuerst). Bei gleicher count bleibt
 * die eingabe-reihenfolge erhalten (stable bubble-sort).              */
static void rebuild_operator_dropdown(void) {
    if (!sum_operator_dd || !s_preset_cache_valid) return;
    /* Lokale kopie fuer den sort - presets selbst nicht modifizieren. */
    char sorted[SSC_OPERATOR_PRESETS_MAX][SSC_OPERATOR_MAXLEN];
    uint16_t count[SSC_OPERATOR_PRESETS_MAX];
    uint8_t n = s_preset_cache.count;
    if (n > SSC_OPERATOR_PRESETS_MAX) n = SSC_OPERATOR_PRESETS_MAX;
    for (uint8_t i = 0; i < n; i++) {
        strncpy(sorted[i], s_preset_cache.items[i], SSC_OPERATOR_MAXLEN);
        count[i] = operator_session_count(sorted[i]);
    }
    /* Stable bubble-sort nach count desc */
    for (uint8_t i = 0; i + 1 < n; i++) {
        for (uint8_t j = 0; j + 1 < n - i; j++) {
            if (count[j] < count[j + 1]) {
                uint16_t tc = count[j];
                count[j] = count[j + 1]; count[j + 1] = tc;
                char tn[SSC_OPERATOR_MAXLEN];
                memcpy(tn, sorted[j], SSC_OPERATOR_MAXLEN);
                memcpy(sorted[j], sorted[j + 1], SSC_OPERATOR_MAXLEN);
                memcpy(sorted[j + 1], tn, SSC_OPERATOR_MAXLEN);
            }
        }
    }
    char opts[SSC_OPERATOR_PRESETS_MAX * (SSC_OPERATOR_MAXLEN + 1) + 8];
    opts[0] = 0;
    if (n == 0) {
        strncpy(opts, "-", sizeof(opts));
    } else {
        for (uint8_t i = 0; i < n; i++) {
            if (i > 0) strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
            strncat(opts, sorted[i], sizeof(opts) - strlen(opts) - 1);
        }
    }
    lv_dropdown_set_options(sum_operator_dd, opts);
}

static void on_operator_presets(const struct view_data_operator_presets *p) {
    if (!sum_operator_dd) return;
    s_preset_cache = *p;
    s_preset_cache_valid = true;
    rebuild_operator_dropdown();
    /* Textarea mit komma-format befuellen (leichter zum editieren als
     * newline-format, per user-request).                             */
    if (set_operators_ta) {
        char buf[SSC_OPERATOR_PRESETS_MAX * (SSC_OPERATOR_MAXLEN + 3) + 1] = {0};
        for (uint8_t i = 0; i < p->count; i++) {
            if (i > 0) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, p->items[i], sizeof(buf) - strlen(buf) - 1);
        }
        lv_textarea_set_text(set_operators_ta, buf);
    }
}

/* ======================================================================= */
/*   Clock-Tick (UI-Timer, alle 10 s)                                        */
/* ======================================================================= */

/* Watchdog-timer: prueft ob der aktuelle screen einer unserer sauna-
 * screens ist. Wenn nicht (legacy hat was geladen), scr_home erzwingen.
 * Nach dem ersten erfolg wird sich selbst entfernt.                  */
static void scr_watchdog_tick(lv_timer_t *t) {
    lv_obj_t *cur = lv_scr_act();
    if (cur == scr_home || cur == scr_live || cur == scr_summary ||
        cur == scr_history || cur == scr_detail || cur == scr_settings) {
        /* alles gut - watchdog kann sich nach einer minute beruhigen */
        return;
    }
    ESP_LOGW(TAG, "watchdog: legacy screen aktiv - force scr_home");
    lv_scr_load(scr_home);
}

static void clock_tick(lv_timer_t *t) {
    char buf[32];
    fmt_clock(buf, sizeof(buf));
    if (home_clock_val) lv_label_set_text(home_clock_val, buf);
    /* Heartbeat: zeig welcher screen aktiv ist, damit wir tracken
     * koennen ob mein sauna-UI ueberhaupt laeuft.                */
    lv_obj_t *cur = lv_scr_act();
    const char *sname =
        (cur == scr_home)     ? "HOME" :
        (cur == scr_live)     ? "LIVE" :
        (cur == scr_summary)  ? "SUMMARY" :
        (cur == scr_history)  ? "HISTORY" :
        (cur == scr_detail)   ? "DETAIL" :
        (cur == scr_settings) ? "SETTINGS" : "LEGACY!!";
    ESP_LOGI(TAG, "heartbeat: scr=%s running=%d", sname, live_running);
}

/* ======================================================================= */
/*   esp_event-Handler (LAEUFT IM EVENT-TASK - LVGL-LOCK NEHMEN!)            */
/* ======================================================================= */

static void on_view_event(void *arg, esp_event_base_t base,
                          int32_t id, void *ev_data) {
    lv_port_sem_take();
    switch (id) {
    case VIEW_EVENT_SESSION_LIVE:
        if (ev_data) on_session_live(ev_data);
        break;
    case VIEW_EVENT_SENSOR_DATA:
        if (ev_data) on_sensor_data(ev_data);
        break;
    case VIEW_EVENT_SESSION_SUMMARY_READY:
        if (ev_data) {
            /* Frische session beendet - edit-mode aus, labels auf save-text */
            sum_edit_mode = false;
            if (sum_title_label)
                lv_label_set_text(sum_title_label, "SESSION BEENDET");
            if (sum_save_btn_label)
                lv_label_set_text(sum_save_btn_label,
                                  LV_SYMBOL_SAVE "  SPEICHERN");
            if (sum_discard_btn_label)
                lv_label_set_text(sum_discard_btn_label, "VERWERFEN");
            summary_prefill(ev_data);
            lv_scr_load_anim(scr_summary, LV_SCR_LOAD_ANIM_FADE_ON,
                             240, 0, false);
        }
        break;
    case VIEW_EVENT_HISTORY_LIST:
        if (ev_data) on_history_list(ev_data);
        break;
    case VIEW_EVENT_HISTORY_DETAIL_CHUNK:
        if (ev_data) on_history_detail_chunk(ev_data);
        break;
    case VIEW_EVENT_HISTORY_DETAIL_DONE:
        if (ev_data) on_history_detail_done(ev_data);
        break;
    case VIEW_EVENT_OPERATOR_PRESETS:
        if (ev_data) on_operator_presets(ev_data);
        break;
    case VIEW_EVENT_OPERATOR_PRESETS_UPDATE:
        if (ev_data) {
            const struct view_data_operator_presets *p = ev_data;
            operators_nvs_save(p);
            /* UI (dropdown + textarea) mit neuen Werten reflektieren */
            on_operator_presets(p);
        }
        break;
    case VIEW_EVENT_PROBE_STATE:
        if (ev_data) on_probe_state(ev_data);
        break;
    case VIEW_EVENT_WIFI_ST:
        if (ev_data && set_wifi_info) {
            const struct view_data_wifi_st *st = ev_data;
            char buf[80];
            if (st->is_connected) {
                snprintf(buf, sizeof(buf), "verbunden: %s  (%d dBm)",
                         st->ssid, st->rssi);
                lv_label_set_text(set_wifi_info, buf);
                lv_obj_set_style_text_color(set_wifi_info, SSC_C_ACCENT, 0);
            } else if (st->is_connecting) {
                lv_label_set_text(set_wifi_info, "verbinde...");
                lv_obj_set_style_text_color(set_wifi_info, SSC_C_TEXT_MUTED, 0);
            } else {
                lv_label_set_text(set_wifi_info, "nicht verbunden");
                lv_obj_set_style_text_color(set_wifi_info, SSC_C_WARNING, 0);
            }
        }
        break;
    case VIEW_EVENT_WIFI_CONNECT_RET:
        if (ev_data && set_wifi_info) {
            const struct view_data_wifi_connet_ret_msg *m = ev_data;
            char buf[80];
            if (m->ret == 0) {
                lv_label_set_text(set_wifi_info, "erfolgreich verbunden");
                lv_obj_set_style_text_color(set_wifi_info, SSC_C_ACCENT, 0);
            } else {
                snprintf(buf, sizeof(buf), "fehler: %s", m->msg);
                lv_label_set_text(set_wifi_info, buf);
                lv_obj_set_style_text_color(set_wifi_info, SSC_C_WARNING, 0);
            }
        }
        break;
    default: break;
    }
    lv_port_sem_give();
}

/* ======================================================================= */
/*   Init                                                                    */
/* ======================================================================= */

/* Parst __DATE__ "Apr 22 2026" + __TIME__ "15:30:45" in time_t, damit
 * wir bei fehlendem NTP wenigstens ein realistisches Datum haben.  */
static time_t boot_compile_time(void) {
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;   /* "Mmm DD YYYY" */
    const char *t = __TIME__;   /* "HH:MM:SS" */
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

void ui_sauna_init(void) {
    ESP_LOGI(TAG, "ui_sauna_init (480x480 dark-CI)");

    /* Wenn kein NTP schon gelaufen ist (time=1970), setze compile-time
     * als fallback. Die Uhr tickt dann ab da - besser als 01.01. 00:00.
     * Sobald WiFi und NTP greifen, wird von dort ueberschrieben.    */
    time_t now = 0; time(&now);
    if (now < 1700000000) {
        time_t ct = boot_compile_time();
        struct timeval tv = { .tv_sec = ct, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "RTC fallback auf compile-time (%ld)", (long)ct);
    }

    font_init();

    /* Detail-buffer in PSRAM: 3600 samples × 2 (temp, rh) × 2 bytes +
     * markers = ~14.4 KB. PSRAM hat >5 MB frei. */
    s_detail_buf.cap = DETAIL_CHART_POINTS;
    s_detail_buf.temps = heap_caps_malloc(s_detail_buf.cap * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
    s_detail_buf.rhs   = heap_caps_malloc(s_detail_buf.cap * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
    s_detail_buf.markers = heap_caps_malloc(s_detail_buf.cap, MALLOC_CAP_SPIRAM);
    if (!s_detail_buf.temps || !s_detail_buf.rhs || !s_detail_buf.markers) {
        ESP_LOGE(TAG, "detail-buffer PSRAM-alloc failed");
    } else {
        ESP_LOGI(TAG, "detail-buffer alloziert: cap=%u slots in PSRAM",
                 (unsigned)s_detail_buf.cap);
    }

    build_home();
    build_live();
    build_summary();
    build_history();
    build_detail();
    build_settings();

    /* Eventloop-Subscriptions: wir halten das schlank - nur die
     * events die wir UI-seitig reflektieren. */
    static const int32_t listened[] = {
        VIEW_EVENT_SENSOR_DATA,
        VIEW_EVENT_SESSION_LIVE,
        VIEW_EVENT_SESSION_SUMMARY_READY,
        VIEW_EVENT_HISTORY_LIST,
        VIEW_EVENT_HISTORY_DETAIL_CHUNK,
        VIEW_EVENT_HISTORY_DETAIL_DONE,
        VIEW_EVENT_OPERATOR_PRESETS,
        VIEW_EVENT_OPERATOR_PRESETS_UPDATE,
        VIEW_EVENT_PROBE_STATE,
        VIEW_EVENT_WIFI_ST,
        VIEW_EVENT_WIFI_CONNECT_RET,
    };
    for (size_t i = 0; i < sizeof(listened)/sizeof(listened[0]); i++) {
        esp_err_t e = esp_event_handler_register_with(
            view_event_handle, VIEW_EVENT_BASE, listened[i],
            on_view_event, NULL);
        if (e != ESP_OK)
            ESP_LOGE(TAG, "subscribe %ld failed: %d", (long)listened[i], e);
    }

    /* Clock-Tick alle 10s */
    lv_timer_create(clock_tick, 10000, NULL);
    /* einmal initial ausloesen */
    clock_tick(NULL);

    /* KEIN initial HISTORY_LIST_REQ mehr hier - das feuert der
     * session-modul selbst am ende von indicator_session_init,
     * wenn sein handler garantiert schon registriert ist. Vorher:
     * race-condition weil view_init vor model_init laeuft, und
     * der event ist ins leere gegangen (home_recent_list leer
     * nach boot bis zum ersten save).                              */

    /* Operator-Presets aus NVS laden. Wenn leer: default von
     * supersauna.club verwenden (kann user dann editieren).     */
    struct view_data_operator_presets p = {0};
    if (operators_nvs_load(&p) != 0 || p.count == 0) {
        parse_operators(SSC_DEFAULT_OPERATORS, &p);
        operators_nvs_save(&p);   /* persistieren damit user ab jetzt editiert */
        ESP_LOGI(TAG, "operator-defaults geladen: %u namen", (unsigned)p.count);
    }
    on_operator_presets(&p);

    /* Home-Screen als Default laden */
    lv_scr_load(scr_home);

    /* Periodischer watchdog-timer: wenn der aktuelle screen KEIN
     * sauna-screen ist (z.B. weil legacy-code ui_screen_time o.ä.
     * geladen hat), zwinge zurueck auf scr_home.                  */
    lv_timer_create(scr_watchdog_tick, 2000, NULL);
}

void ui_sauna_show_home(void) {
    if (scr_home) {
        lv_port_sem_take();
        lv_scr_load(scr_home);
        lv_port_sem_give();
    }
}
