#ifndef INDICATOR_SENSOR_H
#define INDICATOR_SENSOR_H

#include "config.h"
#include "view_data.h"
#include "driver/uart.h"


#ifdef __cplusplus
extern "C" {
#endif

int indicator_sensor_init(void);
int indicator_sensor_get_data(struct view_data_sensor *out_data);

/**
 * Sendet ein (cmd, payload)-Paket ueber die COBS/UART-Leitung an den
 * RP2040. Wrapper um die interne __cmd_send-Routine, damit das
 * Session-Modul (indicator_session.c) Session-Steuerungs-Kommandos
 * ausloesen kann, ohne direkten Zugriff auf die privaten Helpers
 * in indicator_sensor.c zu brauchen.
 *
 * @param cmd    Kommando-Typcode (z.B. SSC_CMD_SESSION_START)
 * @param data   Zeiger auf Payload oder NULL
 * @param len    Anzahl Payload-Bytes (max 510 in v0.3.0)
 * @return       >=0: Anzahl versendeter Bytes, <0: Fehler
 */
int indicator_sensor_rp2040_cmd(uint8_t cmd, const void *data, uint16_t len);

/**
 * v0.4.0: zustandsaenderndes Kommando an den RP2040 mit ACK + 3 Retries.
 * Nicht-blockierend (Queue); Ergebnis kommt als VIEW_EVENT_RP_CMD_RESULT.
 * @return 0 = eingereiht, <0 = Queue voll / zu gross.
 */
int  indicator_sensor_rp_cmd_acked(uint8_t cmd, const void *data, uint16_t len);

/** Zeit sofort (statt erst in <=60 s) an den RP2040 schicken. */
void indicator_sensor_request_time_sync(void);
/** Auto-Modus-Einstellung an den RP2040 schicken. */
void indicator_sensor_send_auto_cfg(void);

#ifdef __cplusplus
}
#endif

#endif
