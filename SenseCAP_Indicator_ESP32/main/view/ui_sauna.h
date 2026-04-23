#ifndef UI_SAUNA_H
#define UI_SAUNA_H

#include "config.h"
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Baut alle Sauna-UI-Screens auf (Home, Live, Summary, History,
 * History-Detail, Sauna-Settings) und registriert Event-Handler auf
 * den view_event_handle. Muss aus dem LVGL-Thread aufgerufen werden.
 *
 * Nach dem Aufruf wird der Home-Screen angezeigt.
 */
void ui_sauna_init(void);

/**
 * Wechselt explizit zum Home-Screen (wird nach Shutdown oder
 * Factory-Reset aufgerufen).
 */
void ui_sauna_show_home(void);

#ifdef __cplusplus
}
#endif

#endif
