#ifndef INDICATOR_MARIADB_H
#define INDICATOR_MARIADB_H

#include <stdbool.h>
#include <stdint.h>
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MariaDB configuration structure */
struct mariadb_config {
    bool enabled;
    char host[64];
    uint16_t port;
    char user[32];
    char password[64];
    char database[32];
    char table[32];
    uint16_t interval_minutes;  /* Export interval in minutes */
};

/* Initialize the MariaDB module */
int indicator_mariadb_init(void);

/* Get current MariaDB configuration */
int indicator_mariadb_get_config(struct mariadb_config *config);

/* Set and save MariaDB configuration */
int indicator_mariadb_set_config(const struct mariadb_config *config);

/* Test the database connection */
int indicator_mariadb_test_connection(void);

/* Manually trigger a data export */
int indicator_mariadb_export_now(void);

/* Get last export status (0 = success, negative = error code) */
int indicator_mariadb_get_last_status(void);

/* Get timestamp of last successful export */
time_t indicator_mariadb_get_last_export_time(void);

/**
 * Exportiert eine abgeschlossene Sauna-Session. Legt die Tabellen
 * `sauna_sessions` und `sauna_samples` an falls nicht vorhanden,
 * schreibt den Metadaten-Record und fuegt die Samples per batch-INSERT
 * (100er-Chunks) ein. Wird synchron ausgefuehrt (kann bis einige
 * Sekunden dauern) und ist als best-effort gedacht - Fehler werden
 * geloggt, aber nicht gequeued (die NVS/SD-Persistenz bleibt als
 * fallback erhalten).
 *
 * @param samples  Zeiger auf linearisierte Samples (chronologisch).
 * @param n        Anzahl Samples (darf 0 sein, dann nur Meta exportiert).
 * @return         0 ok, <0 Fehler-Code.
 */
int indicator_mariadb_export_session(
        const struct view_data_session_meta *meta,
        const struct view_data_session_sample *samples,
        uint16_t n);

/**
 * Live-Streaming APIs (ab ssc-v0.3): Session-Zeile wird schon beim
 * Start angelegt, Samples werden in 30s-Batches waehrend der Session
 * gepusht, und beim Save wird die Zeile final aktualisiert.
 *
 * Alle drei Funktionen sind no-ops wenn MariaDB-Export deaktiviert ist.
 */
int indicator_mariadb_begin_session(const char *id, time_t start_ts,
                                    const char *operator_tag);

int indicator_mariadb_append_samples(const char *id,
        const struct view_data_session_sample *samples, uint16_t n);

int indicator_mariadb_finalize_session(const struct view_data_session_meta *meta);

#ifdef __cplusplus
}
#endif

#endif /* INDICATOR_MARIADB_H */
