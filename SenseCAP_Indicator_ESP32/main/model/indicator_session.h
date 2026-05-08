#ifndef INDICATOR_SESSION_H
#define INDICATOR_SESSION_H

#include "config.h"
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialisiert die Sauna-Session-State-Machine.
 *
 * Haengt sich in den view-event-loop ein, um auf UI-Events
 * (SESSION_START/AUFGUSS/END_REQUEST/SAVE/DISCARD, HISTORY_*)
 * zu reagieren, und abonniert VIEW_EVENT_SENSOR_DATA fuer
 * SAUNA_TEMP/SAUNA_RH-Samples. Reserviert einen PSRAM-Buffer
 * fuer die Live-Samples und startet einen Worker-Task, der
 * einmal pro Sekunde ein VIEW_EVENT_SESSION_LIVE-Event
 * absetzt solange eine Session laeuft.
 *
 * Muss NACH indicator_sensor_init() aufgerufen werden, damit
 * der UART/COBS-Kanal zum RP2040 schon steht.
 */
int indicator_session_init(void);

/**
 * @brief Callback-Haken fuer SD-Readback-Chunks vom RP2040.
 *
 * Wird von indicator_sensor.c im COBS-Empfaenger aufgerufen,
 * wenn ein Paket mit Typcode 0xC1 (PKT_TYPE_SD_READBACK_CHUNK)
 * empfangen wird. Der Payload enthaelt: [2B req_id][4B total]
 * [4B offset][2B len][data]. Das Modul matcht req_id gegen
 * offene History-Detail-Anfragen und stueckelt die samples
 * chunk-weise via VIEW_EVENT_HISTORY_DETAIL_CHUNK an die View.
 */
void indicator_session_rx_sd_chunk(const uint8_t *payload, size_t n);

/**
 * @brief Ob gerade eine Session aufgezeichnet wird.
 *        Thread-safe, kann aus jedem Kontext gelesen werden.
 */
bool indicator_session_is_active(void);

/**
 * @brief v0.3.0: Hybrid-Storage SD-list trigger.
 *
 * Schickt SSC_CMD_LIST_SESSIONS an den RP2040. Der streamt dann pro
 * SD-session ein PKT_TYPE_SESSION_META_RESP zurueck, abschliessend
 * ein META_DONE mit count. ESP32-seitig werden die metadata async
 * via VIEW_EVENT_SD_META_RESP-handler in NVS einsortiert (nur wenn
 * id noch nicht da ist - non-destructive).
 *
 * Wird einmal automatisch 3s nach init gefeuert. UI kann manuell
 * triggern fuer expliziten "AUS SD WIEDERHERSTELLEN"-button.
 */
void indicator_session_request_sd_list(void);

#ifdef __cplusplus
}
#endif

#endif /* INDICATOR_SESSION_H */
