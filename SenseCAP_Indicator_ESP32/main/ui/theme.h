#ifndef THEME_H
#define THEME_H

/**
 * Globale Farb- und Typo-Palette fuer den sscsaunalogger.
 *
 * Werte sind 1:1 aus der Corporate Identity von supersauna.club
 * extrahiert (HTML-Farb-Audit 2026-04-21). Jeder UI-Pfad MUSS
 * diese Konstanten verwenden, damit Re-Brandings an einer Stelle
 * passieren koennen.
 *
 * Hex-Werte werden als Integer bereitgestellt, damit sie direkt
 * an lv_color_hex() gereicht werden koennen. Fuer Stellen, die
 * schon eine lv_color_t brauchen, sind die SSC_C_*-Helfer da.
 */

#include "lvgl.h"

/* --- Hintergrund- und Flaechen-Stufen (von dunkel nach hell) ------------- */
#define SSC_HEX_BG          0x0a0a0a  /* Screen-Grund */
#define SSC_HEX_SURFACE     0x1a1a1a  /* Karten, Panels */
#define SSC_HEX_ELEVATED    0x222222  /* angehobene Karten, Buttons idle */
#define SSC_HEX_BORDER      0x2a2a2a  /* Rahmen, dezente Trennlinien */
#define SSC_HEX_OVERLAY     0x313131  /* Hover-/Press-State */

/* --- Text --------------------------------------------------------------- */
#define SSC_HEX_TEXT        0xf5f5f5  /* Primaer-Text (warmes Offwhite) */
#define SSC_HEX_TEXT_MUTED  0x9ca3af  /* Sekundaer-Text */
#define SSC_HEX_TEXT_FAINT  0x6b7280  /* Tertiaer-Text, Labels */
#define SSC_HEX_TEXT_DIM    0x4b5563  /* Deaktiviert */

/* --- Akzent (das "Blattgold" aus dem SSC-Logo) -------------------------- */
#define SSC_HEX_ACCENT      0xc9a84c  /* Primaer-Akzent */
#define SSC_HEX_ACCENT_HOV  0xb8943f  /* Hover / gedrueckt */
#define SSC_HEX_ACCENT_DIM  0x7a6530  /* Deaktiviert / Ghost */

/* --- Semantik ----------------------------------------------------------- */
#define SSC_HEX_WARNING     0xc25544  /* Ueberschreitung Grenzwert */
#define SSC_HEX_LIVE_DOT    0xc9a84c  /* "aufnahme laeuft"-Indikator */

/* --- Chart-Serien ------------------------------------------------------- */
#define SSC_HEX_CHART_TEMP  0xc9a84c  /* Temperatur: Gold (Primaer) */
#define SSC_HEX_CHART_RH    0x8aa39f  /* Luftfeuchte: gedaempftes Sage */
#define SSC_HEX_CHART_GRID  0x2a2a2a  /* Gitterlinien, nahe am Surface */
#define SSC_HEX_CHART_AXIS  0x6b7280  /* Achsen-Beschriftung */
#define SSC_HEX_AUFGUSS_MARK 0xc9a84c /* Vertikale Linie bei Aufguss */

/* --- lv_color_t-Helfer fuer schnelle Verwendung ------------------------- */
#define SSC_C_BG            lv_color_hex(SSC_HEX_BG)
#define SSC_C_SURFACE       lv_color_hex(SSC_HEX_SURFACE)
#define SSC_C_ELEVATED      lv_color_hex(SSC_HEX_ELEVATED)
#define SSC_C_OVERLAY       lv_color_hex(SSC_HEX_OVERLAY)
#define SSC_C_BORDER        lv_color_hex(SSC_HEX_BORDER)
#define SSC_C_TEXT          lv_color_hex(SSC_HEX_TEXT)
#define SSC_C_TEXT_MUTED    lv_color_hex(SSC_HEX_TEXT_MUTED)
#define SSC_C_TEXT_FAINT    lv_color_hex(SSC_HEX_TEXT_FAINT)
#define SSC_C_ACCENT        lv_color_hex(SSC_HEX_ACCENT)
#define SSC_C_ACCENT_HOV    lv_color_hex(SSC_HEX_ACCENT_HOV)
#define SSC_C_WARNING       lv_color_hex(SSC_HEX_WARNING)
#define SSC_C_CHART_TEMP    lv_color_hex(SSC_HEX_CHART_TEMP)
#define SSC_C_CHART_RH      lv_color_hex(SSC_HEX_CHART_RH)
#define SSC_C_CHART_GRID    lv_color_hex(SSC_HEX_CHART_GRID)
#define SSC_C_AUFGUSS       lv_color_hex(SSC_HEX_AUFGUSS_MARK)

/* --- Layout-Konstanten fuer 480x480-Display ---------------------------- */
#define SSC_SCREEN_W        480
#define SSC_SCREEN_H        480
#define SSC_PAD_SCREEN      16    /* Aussenabstand Screen-Rand */
#define SSC_PAD_CARD        12    /* Innenabstand Karten */
#define SSC_GAP             8     /* Abstand zwischen Elementen */
#define SSC_RADIUS_CARD     10    /* Ecken-Radius Karten */
#define SSC_RADIUS_BTN      8     /* Ecken-Radius Buttons */
#define SSC_BORDER_W        1

/* --- Typographie-Groessen (Manrope) ------------------------------------ */
/* Die eigentlichen Font-Objekte werden in ui_font_manrope_*.c generiert
 * und in indicator_view.c als extern eingebunden.                         */

#endif /* THEME_H */
