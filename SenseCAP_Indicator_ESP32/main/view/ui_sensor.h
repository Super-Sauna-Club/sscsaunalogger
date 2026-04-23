#ifndef UI_SENSOR_H
#define UI_SENSOR_H

#include "lvgl.h"
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_sensor_init(lv_obj_t *parent);
void ui_sensor_update(const struct view_data_sensor *data);
void ui_sensor_next_page(void);
void ui_sensor_prev_page(void);

#ifdef __cplusplus
}
#endif

#endif
