#ifndef INDICATOR_TIME_H
#define INDICATOR_TIME_H

#include "config.h"
#include "view_data.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif


//ntp sync
int indicator_time_init(void);

// set TZ
int indicator_time_net_zone_set( char *p);

/* Abfrage aktueller Zeit-Quality-State (fuer UI beim boot, bevor erstes
 * VIEW_EVENT_TIME_STATE_UPDATE eintrifft). Thread-safe. */
void indicator_time_state_get(struct view_data_time_state *out);

#ifdef __cplusplus
}
#endif

#endif
