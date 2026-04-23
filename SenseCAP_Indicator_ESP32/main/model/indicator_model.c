#include "indicator_model.h"
#include "indicator_storage.h"
#include "indicator_wifi.h"
#include "indicator_display.h"
#include "indicator_time.h"
#include "indicator_btn.h"
#include "indicator_city.h"
#include "indicator_mariadb.h"
#include "indicator_sensor.h"

/* Sauna-spezifische Module (supersauna.club-Fork) */
#include "indicator_session.h"
#include "indicator_session_store.h"
#include "indicator_http_export.h"

int indicator_model_init(void)
{
    indicator_storage_init();
    indicator_sensor_init();
    indicator_wifi_init();
    indicator_time_init();
    indicator_city_init();
    indicator_display_init();  // lcd bl on
    indicator_btn_init();
    indicator_mariadb_init();

    /* Sauna-Pipeline: Reihenfolge wichtig.
     * 1) session_store liefert die History-Liste, wird vom session-
     *    Modul beim HISTORY_LIST_REQ-Event abgefragt.
     * 2) http_export haengt sich in den view_event_loop fuer
     *    HTTP_CFG_APPLY-Events und wird vom session-Modul nach
     *    jedem Session-Save aufgerufen.
     * 3) session initialisiert zuletzt, weil es auf die o.g.
     *    Module via extern-Symbole zugreift.                     */
    indicator_session_store_init();
    indicator_http_export_init();
    indicator_session_init();

    return 0;
}