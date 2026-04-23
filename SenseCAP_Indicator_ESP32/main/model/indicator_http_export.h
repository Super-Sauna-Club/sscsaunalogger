#ifndef INDICATOR_HTTP_EXPORT_H
#define INDICATOR_HTTP_EXPORT_H

#include "config.h"
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HTTP-JSON-Export an den Vereinsendpunkt (supersauna.club/api/session o.ae.).
 * Konfiguration (URL, optional Bearer-Token) kommt aus NVS und wird via
 * VIEW_EVENT_HTTP_CFG_APPLY aktualisiert.
 */

int indicator_http_export_init(void);

/**
 * Exportiert eine abgeschlossene Session als JSON-POST.
 *
 * Bei Netz-/HTTP-Fehlern wird die Session-ID in eine NVS-Retry-Queue
 * gelegt und beim naechsten erfolgreichen Export oder Boot nachgeholt.
 * Die Samples-Quelle dafuer ist indicator_session_store_get() + ein
 * SD-Readback-Aufruf beim RP2040.
 *
 * @return 0 bei Erfolg, <0 bei Fehler (Session wurde dann gequeued).
 */
int indicator_http_export_session(const struct view_data_session_meta *meta,
                                  const struct view_data_session_sample *samples,
                                  uint16_t sample_count);

#ifdef __cplusplus
}
#endif

#endif
