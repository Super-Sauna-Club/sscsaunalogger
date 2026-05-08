#pragma once

#include <stdint.h>
#include <stdbool.h>

/* SSC Settings - zentrale toggle-zustaende fuer das geraet.
 *
 * Persistierung: NVS-key "ssc_set" im namespace "indicator". Eine
 * versionierte struct, atomic write, geladen einmal beim boot. Lese-
 * zugriffe gehen direkt gegen `g_ssc_settings`. Aenderungen via
 * ssc_settings_save() schreiben sofort in NVS und feuern ein
 * VIEW_EVENT_SSC_SETTINGS_CHANGED damit andere module reagieren.
 *
 * v2 felder (v0.2.14):
 *   ntp_enabled         - NTP automatisch syncen (1) oder nur manuell (0)
 *   wifi_enabled        - WLAN ueberhaupt einschalten (1) oder aus (0)
 *   dash_intern_visible - "GERAET (INTERN)" sektion am dashboard zeigen
 *   brightness_pct      - LCD-backlight helligkeit 5..100 %. PWM via
 *                         LEDC ch0/timer0/5kHz/10-bit. Default 100.
 *                         Min slider-wert ist 5% damit display immer
 *                         lesbar bleibt - hardware-power-button macht
 *                         echtes off, nicht der slider.
 *
 * v1 -> v2 migration: defaults werden gesetzt (v1 hatte backlight_on
 * statt brightness_pct - migration ist trivial weil v0.2.14 sowieso
 * der erste release mit dem feld ist).
 */

typedef struct __attribute__((packed)) {
    uint8_t version;             /* 2 */
    uint8_t ntp_enabled;
    uint8_t wifi_enabled;
    uint8_t dash_intern_visible;
    uint8_t brightness_pct;      /* 5..100 */
    uint8_t _reserved[3];
} ssc_settings_t;

#define SSC_SETTINGS_VERSION  2
#define SSC_BRIGHTNESS_MIN    5
#define SSC_BRIGHTNESS_MAX    100

extern ssc_settings_t g_ssc_settings;

/* Laedt settings aus NVS. Falls nicht vorhanden oder version-mismatch
 * werden defaults gesetzt (alles enabled, dashboard sichtbar, backlight
 * an) und SOFORT zurueckgeschrieben.                                   */
void ssc_settings_init(void);

/* Persistiert g_ssc_settings nach NVS. Idempotent - kann oft aufgerufen
 * werden. Loggt fehler aber crashed nicht.                            */
void ssc_settings_save(void);

/* Wendet die aktuellen settings auf die laufenden module an:
 *   - WLAN start/stop entsprechend wifi_enabled
 *   - SNTP start/stop entsprechend ntp_enabled
 *   - Backlight on/off entsprechend backlight_on
 * Wird einmal nach init aufgerufen + nach jedem save() von handlern
 * die mehr als nur das eine modul betreffen.                          */
void ssc_settings_apply_runtime(void);
