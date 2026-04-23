#ifndef INDICATOR_CITY_H
#define INDICATOR_CITY_H

#include "config.h"
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

int indicator_city_init(void);
int indicator_city_set_custom_name(const char *name);
int indicator_city_get_custom_name(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
