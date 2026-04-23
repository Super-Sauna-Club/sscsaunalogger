#ifndef INDICATOR_SESSION_STORE_H
#define INDICATOR_SESSION_STORE_H

#include "config.h"
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Persistenter Index der letzten Sauna-Sessions. Speichert pro Session
 * einen kompakten Metadaten-Record (view_data_session_meta) in NVS.
 * Die eigentlichen Samples liegen auf der SD-Karte am RP2040 und werden
 * per Readback-Kommando zurueckgeholt, wenn jemand den Verlauf oeffnet.
 *
 * NVS-Namespace:  "sauna_sess"
 * Key-Schema:     "c"        -> uint16 count der gespeicherten Sessions
 *                 "s_<nnnn>" -> view_data_session_meta (blob)
 *
 * Der NVS-Standardpartitioning des D1S (0xF000 = 60 KB) reicht fuer
 * ~230 Sessions a 256 Byte. Reicht fuer ein Verein-Jahr mit 1-2
 * Sessions pro Tag. Kommt die Nachfrage hoeher, migrieren wir auf
 * eine eigene LittleFS-Partition.
 */

int  indicator_session_store_init(void);

/**
 * Haengt einen Session-Metadaten-Record ans Ende des Index an.
 * @return 0 bei Erfolg, <0 bei NVS-Fehler.
 */
int  indicator_session_store_append(const struct view_data_session_meta *meta);

/**
 * Laedt bis zu `count` Eintraege ab `start_index` in out->items (das
 * der Caller bereitstellt). Gibt die tatsaechliche Anzahl und den
 * Gesamt-Count zurueck.
 */
int  indicator_session_store_list(struct view_data_session_list *out,
                                  uint16_t start_index, uint16_t count);

int  indicator_session_store_get(const char *id,
                                 struct view_data_session_meta *out);

int  indicator_session_store_delete(const char *id);

/**
 * Bearbeitet einen existierenden session-record. Findet slot mit
 * match auf meta->id und ersetzt NUR die user-editierbaren felder
 * (operator_tag, aufguss_headline, participants, notes). Die gemessenen
 * werte (peak_temp, peak_rh, aufguss_count, start_ts, end_ts) bleiben
 * erhalten.
 * @return 0 bei Erfolg, -1 wenn id nicht gefunden, -2 bei NVS-fehler.
 */
int  indicator_session_store_update(const struct view_data_session_meta *meta);

/** Alles loeschen (fuer factory reset). */
int  indicator_session_store_wipe(void);

/** Aktuelle Anzahl der persistierten Sessions. */
uint16_t indicator_session_store_count(void);

#ifdef __cplusplus
}
#endif

#endif
