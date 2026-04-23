#include "indicator_view.h"
#include "indicator_wifi.h"

#include "ui.h"
#include "ui_helpers.h"
#include "indicator_util.h"
#include "indicator_sensor.h"
#include "indicator_mariadb.h"
#include "indicator_city.h"

#include "esp_wifi.h"
#include <time.h>




static const char *TAG = "view";

/*****************************************************************/
// Extended sensor panels - added to main sensor screen
/*****************************************************************/

/* Extended sensor data labels */
static lv_obj_t *lbl_pm1_0_data = NULL;
static lv_obj_t *lbl_pm2_5_data = NULL;
static lv_obj_t *lbl_pm10_data = NULL;
static lv_obj_t *lbl_temp_ext_data = NULL;
static lv_obj_t *lbl_hum_ext_data = NULL;
static lv_obj_t *lbl_no2_data = NULL;
static lv_obj_t *lbl_c2h5oh_data = NULL;
static lv_obj_t *lbl_voc_data = NULL;
static lv_obj_t *lbl_co_data = NULL;

/* Extended sensor panel buttons */
static lv_obj_t *btn_pm1_0 = NULL;
static lv_obj_t *btn_pm2_5 = NULL;
static lv_obj_t *btn_pm10 = NULL;
static lv_obj_t *btn_temp_ext = NULL;
static lv_obj_t *btn_hum_ext = NULL;
static lv_obj_t *btn_no2 = NULL;
static lv_obj_t *btn_c2h5oh = NULL;
static lv_obj_t *btn_voc = NULL;
static lv_obj_t *btn_co = NULL;

/* Sensor scroll container */
static lv_obj_t *sensor_scroll_cont = NULL;

/* Timer for updating extended sensor values */
static lv_timer_t *sensor_ext_update_timer = NULL;

/* Color definitions for sensor panels - harmonized palette */
#define COLOR_TEMP_EXT 0xEEBF41  /* Warm yellow - matches original temp */
#define COLOR_HUM_EXT  0x4EACE4  /* Blue - matches original humidity */
#define COLOR_PM       0xE06D3D  /* Orange-red for particulate - matches TVOC style */
#define COLOR_CO2      0x529D53  /* Green - matches original CO2 */
#define COLOR_GAS      0x7B68EE  /* Medium slate blue for other gases */

/* Unified panel creation - consistent layout for all sensor boxes:
 * - Title: top-left
 * - Value + Unit: centered
 * - Same font sizes everywhere
 */
static lv_obj_t* create_sensor_panel(lv_obj_t *parent, const char *title,
                                      const char *unit, uint32_t color,
                                      lv_obj_t **data_label_out)
{
    lv_obj_t *panel = lv_btn_create(parent);
    lv_obj_set_size(panel, 140, 90);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, 255, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Title - top left */
    lv_obj_t *lbl_title = lv_label_create(panel);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 8, 6);

    /* Container for value + unit centered */
    lv_obj_t *val_cont = lv_obj_create(panel);
    lv_obj_set_size(val_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(val_cont, 0, 0);
    lv_obj_set_style_border_width(val_cont, 0, 0);
    lv_obj_set_style_pad_all(val_cont, 0, 0);
    lv_obj_align(val_cont, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_flex_flow(val_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(val_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(val_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Value */
    lv_obj_t *lbl_data = lv_label_create(val_cont);
    lv_label_set_text(lbl_data, "---");
    lv_obj_set_style_text_font(lbl_data, &lv_font_montserrat_26, 0);

    /* Unit */
    lv_obj_t *lbl_unit = lv_label_create(val_cont);
    lv_label_set_text(lbl_unit, unit);
    lv_obj_set_style_text_font(lbl_unit, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(lbl_unit, 4, 0);

    if (data_label_out) *data_label_out = lbl_data;
    return panel;
}

static void sensor_ext_update_timer_cb(lv_timer_t *timer)
{
    struct view_data_sensor data;
    if (indicator_sensor_get_data(&data) != 0) return;

    char buf[32];

    /* PM sensors */
    if (lbl_pm1_0_data) {
        snprintf(buf, sizeof(buf), "%.0f", data.pm1_0);
        lv_label_set_text(lbl_pm1_0_data, buf);
    }
    if (lbl_pm2_5_data) {
        snprintf(buf, sizeof(buf), "%.0f", data.pm2_5);
        lv_label_set_text(lbl_pm2_5_data, buf);
    }
    if (lbl_pm10_data) {
        snprintf(buf, sizeof(buf), "%.0f", data.pm10);
        lv_label_set_text(lbl_pm10_data, buf);
    }

    /* External temp/humidity */
    if (lbl_temp_ext_data) {
        snprintf(buf, sizeof(buf), "%.1f", data.temp_external);
        lv_label_set_text(lbl_temp_ext_data, buf);
    }
    if (lbl_hum_ext_data) {
        snprintf(buf, sizeof(buf), "%.0f", data.humidity_external);
        lv_label_set_text(lbl_hum_ext_data, buf);
    }

    /* Gas sensors - ppm(eq) values */
    if (lbl_no2_data) {
        snprintf(buf, sizeof(buf), "%.2f", data.multigas_gm102b[0]);  /* 0.05-10 ppm range */
        lv_label_set_text(lbl_no2_data, buf);
    }
    if (lbl_c2h5oh_data) {
        snprintf(buf, sizeof(buf), "%.0f", data.multigas_gm302b[0]);  /* 10-500 ppm range */
        lv_label_set_text(lbl_c2h5oh_data, buf);
    }
    if (lbl_voc_data) {
        snprintf(buf, sizeof(buf), "%.0f", data.multigas_gm502b[0]);  /* 1-500 ppm range */
        lv_label_set_text(lbl_voc_data, buf);
    }
    if (lbl_co_data) {
        snprintf(buf, sizeof(buf), "%.0f", data.multigas_gm702b[0]);  /* 1-1000 ppm range */
        lv_label_set_text(lbl_co_data, buf);
    }
}

static void restyle_original_panel(lv_obj_t *panel)
{
    /* Hide all children of original panel - we'll add new styled ones */
    uint32_t child_cnt = lv_obj_get_child_cnt(panel);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_add_flag(lv_obj_get_child(panel, i), LV_OBJ_FLAG_HIDDEN);
    }
}

#define SENSOR_CLICK_DEBOUNCE_MS 500
static uint32_t last_sensor_click_time = 0;

static bool sensor_click_debounce_check(void)
{
    uint32_t now = lv_tick_get();
    if ((now - last_sensor_click_time) < SENSOR_CLICK_DEBOUNCE_MS) {
        return false;
    }
    last_sensor_click_time = now;
    return true;
}

static void sensor_temp_ext_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_TEMP_EXT_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_hum_ext_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_HUMIDITY_EXT_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_pm1_0_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_PM1_0_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_pm2_5_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_PM2_5_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_pm10_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_PM10_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_no2_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_NO2_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_c2h5oh_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_C2H5OH_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_voc_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_VOC_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_co_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_CO_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_co2_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_CO2_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void sensor_tvoc_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && sensor_click_debounce_check()) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_TVOC_HISTORY, NULL, 0, portMAX_DELAY);
        _ui_screen_change(ui_screen_sensor_chart, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void extend_sensor_screen(void)
{
    /* No scrolling needed - everything fits on one screen */
    lv_obj_clear_flag(ui_screen_sensor, LV_OBJ_FLAG_SCROLLABLE);

    /* Hide scroll dots and original internal sensor panels */
    lv_obj_add_flag(ui_scrolldots2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_temp2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_humidity2, LV_OBJ_FLAG_HIDDEN);

    /* Layout constants - 3 columns, full screen height (480x480 display) */
    const int margin_left = 22;
    const int panel_w = 140;
    const int panel_h = 100;  /* Tall panels for 480px height */
    const int gap_x = 8;
    const int gap_y = 10;
    const int col1 = margin_left;
    const int col2 = margin_left + panel_w + gap_x;
    const int col3 = margin_left + 2 * (panel_w + gap_x);

    int y_pos = 50;  /* Start lower from top */

    /*=== Row 1: External Temp & Humidity (2 wider panels) ===*/
    btn_temp_ext = create_sensor_panel(ui_screen_sensor, "Temp", "C",
                                        COLOR_TEMP_EXT, &lbl_temp_ext_data);
    lv_obj_set_size(btn_temp_ext, 215, panel_h);
    lv_obj_set_pos(btn_temp_ext, col1, y_pos);
    lv_obj_add_event_cb(btn_temp_ext, sensor_temp_ext_click_cb, LV_EVENT_CLICKED, NULL);

    btn_hum_ext = create_sensor_panel(ui_screen_sensor, "Humidity", "%",
                                       COLOR_HUM_EXT, &lbl_hum_ext_data);
    lv_obj_set_size(btn_hum_ext, 215, panel_h);
    lv_obj_set_pos(btn_hum_ext, 245, y_pos);
    lv_obj_add_event_cb(btn_hum_ext, sensor_hum_ext_click_cb, LV_EVENT_CLICKED, NULL);
    y_pos += panel_h + gap_y;

    /*=== Row 2: PM1.0, PM2.5, PM10 ===*/
    btn_pm1_0 = create_sensor_panel(ui_screen_sensor, "PM1.0", "ug/m3",
                                        COLOR_PM, &lbl_pm1_0_data);
    lv_obj_set_size(btn_pm1_0, panel_w, panel_h);
    lv_obj_set_pos(btn_pm1_0, col1, y_pos);
    lv_obj_add_event_cb(btn_pm1_0, sensor_pm1_0_click_cb, LV_EVENT_CLICKED, NULL);

    btn_pm2_5 = create_sensor_panel(ui_screen_sensor, "PM2.5", "ug/m3",
                                     COLOR_PM, &lbl_pm2_5_data);
    lv_obj_set_size(btn_pm2_5, panel_w, panel_h);
    lv_obj_set_pos(btn_pm2_5, col2, y_pos);
    lv_obj_add_event_cb(btn_pm2_5, sensor_pm2_5_click_cb, LV_EVENT_CLICKED, NULL);

    btn_pm10 = create_sensor_panel(ui_screen_sensor, "PM10", "ug/m3",
                                        COLOR_PM, &lbl_pm10_data);
    lv_obj_set_size(btn_pm10, panel_w, panel_h);
    lv_obj_set_pos(btn_pm10, col3, y_pos);
    lv_obj_add_event_cb(btn_pm10, sensor_pm10_click_cb, LV_EVENT_CLICKED, NULL);
    y_pos += panel_h + gap_y;

    /*=== Row 3: CO2, TVOC, NO2 ===*/
    /* Restyle and reposition original CO2 panel */
    restyle_original_panel(ui_co2);
    lv_obj_clear_flag(ui_co2, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_align(ui_co2, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(ui_co2, panel_w, panel_h);
    lv_obj_set_pos(ui_co2, col1, y_pos);
    /* Add new styled labels to CO2 */
    lv_obj_t *co2_title = lv_label_create(ui_co2);
    lv_label_set_text(co2_title, "CO2");
    lv_obj_set_style_text_font(co2_title, &lv_font_montserrat_16, 0);
    lv_obj_align(co2_title, LV_ALIGN_TOP_LEFT, 8, 6);
    /* Value container centered */
    lv_obj_t *co2_val_cont = lv_obj_create(ui_co2);
    lv_obj_set_size(co2_val_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(co2_val_cont, 0, 0);
    lv_obj_set_style_border_width(co2_val_cont, 0, 0);
    lv_obj_set_style_pad_all(co2_val_cont, 0, 0);
    lv_obj_align(co2_val_cont, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_flex_flow(co2_val_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(co2_val_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(co2_val_cont, LV_OBJ_FLAG_SCROLLABLE);
    /* Reset and reparent CO2 data label */
    lv_obj_set_align(ui_co2_data, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(ui_co2_data, 0, 0);
    lv_obj_set_style_text_align(ui_co2_data, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_co2_data, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(ui_co2_data, &lv_font_montserrat_26, 0);
    lv_obj_set_parent(ui_co2_data, co2_val_cont);
    lv_obj_clear_flag(ui_co2_data, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *co2_unit = lv_label_create(co2_val_cont);
    lv_label_set_text(co2_unit, "ppm");
    lv_obj_set_style_text_font(co2_unit, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(co2_unit, 4, 0);
    /* Add click handler for CO2 history */
    lv_obj_add_event_cb(ui_co2, sensor_co2_click_cb, LV_EVENT_CLICKED, NULL);

    /* Restyle and reposition original TVOC panel */
    restyle_original_panel(ui_tvoc_2);
    lv_obj_clear_flag(ui_tvoc_2, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_align(ui_tvoc_2, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(ui_tvoc_2, panel_w, panel_h);
    lv_obj_set_pos(ui_tvoc_2, col2, y_pos);
    /* Add new styled labels to TVOC */
    lv_obj_t *tvoc_title = lv_label_create(ui_tvoc_2);
    lv_label_set_text(tvoc_title, "tVOC");
    lv_obj_set_style_text_font(tvoc_title, &lv_font_montserrat_16, 0);
    lv_obj_align(tvoc_title, LV_ALIGN_TOP_LEFT, 8, 6);
    /* Value container centered */
    lv_obj_t *tvoc_val_cont = lv_obj_create(ui_tvoc_2);
    lv_obj_set_size(tvoc_val_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tvoc_val_cont, 0, 0);
    lv_obj_set_style_border_width(tvoc_val_cont, 0, 0);
    lv_obj_set_style_pad_all(tvoc_val_cont, 0, 0);
    lv_obj_align(tvoc_val_cont, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_flex_flow(tvoc_val_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tvoc_val_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tvoc_val_cont, LV_OBJ_FLAG_SCROLLABLE);
    /* Reset and reparent TVOC data label */
    lv_obj_set_align(ui_tvoc_data, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(ui_tvoc_data, 0, 0);
    lv_obj_set_style_text_align(ui_tvoc_data, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_tvoc_data, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(ui_tvoc_data, &lv_font_montserrat_26, 0);
    lv_obj_set_parent(ui_tvoc_data, tvoc_val_cont);
    lv_obj_clear_flag(ui_tvoc_data, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *tvoc_unit = lv_label_create(tvoc_val_cont);
    lv_label_set_text(tvoc_unit, "idx");
    lv_obj_set_style_text_font(tvoc_unit, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(tvoc_unit, 4, 0);
    /* Add click handler for TVOC history */
    lv_obj_add_event_cb(ui_tvoc_2, sensor_tvoc_click_cb, LV_EVENT_CLICKED, NULL);

    /* NO2 - ppm(eq) */
    btn_no2 = create_sensor_panel(ui_screen_sensor, "NO2", "ppm",
                                        COLOR_GAS, &lbl_no2_data);
    lv_obj_set_size(btn_no2, panel_w, panel_h);
    lv_obj_set_pos(btn_no2, col3, y_pos);
    lv_obj_add_event_cb(btn_no2, sensor_no2_click_cb, LV_EVENT_CLICKED, NULL);
    y_pos += panel_h + gap_y;

    /*=== Row 4: C2H5OH, VOC, CO - ppm(eq) ===*/
    btn_c2h5oh = create_sensor_panel(ui_screen_sensor, "C2H5OH", "ppm",
                                        COLOR_GAS, &lbl_c2h5oh_data);
    lv_obj_set_size(btn_c2h5oh, panel_w, panel_h);
    lv_obj_set_pos(btn_c2h5oh, col1, y_pos);
    lv_obj_add_event_cb(btn_c2h5oh, sensor_c2h5oh_click_cb, LV_EVENT_CLICKED, NULL);

    btn_voc = create_sensor_panel(ui_screen_sensor, "VOC", "ppm",
                                        COLOR_GAS, &lbl_voc_data);
    lv_obj_set_size(btn_voc, panel_w, panel_h);
    lv_obj_set_pos(btn_voc, col2, y_pos);
    lv_obj_add_event_cb(btn_voc, sensor_voc_click_cb, LV_EVENT_CLICKED, NULL);

    btn_co = create_sensor_panel(ui_screen_sensor, "CO", "ppm",
                                        COLOR_GAS, &lbl_co_data);
    lv_obj_set_size(btn_co, panel_w, panel_h);
    lv_obj_set_pos(btn_co, col3, y_pos);
    lv_obj_add_event_cb(btn_co, sensor_co_click_cb, LV_EVENT_CLICKED, NULL);

    /* Start update timer for extended sensors */
    sensor_ext_update_timer = lv_timer_create(sensor_ext_update_timer_cb, 1000, NULL);

    ESP_LOGI(TAG, "Sensor screen: 11 panels in 4 rows, balanced layout");
}

/*****************************************************************/
// Database Settings Screen
/*****************************************************************/

static lv_obj_t *ui_screen_database = NULL;
static lv_obj_t *ui_db_host_ta = NULL;
static lv_obj_t *ui_db_port_ta = NULL;
static lv_obj_t *ui_db_user_ta = NULL;
static lv_obj_t *ui_db_pass_ta = NULL;
static lv_obj_t *ui_db_name_ta = NULL;
static lv_obj_t *ui_db_table_ta = NULL;
static lv_obj_t *ui_db_interval_ta = NULL;
static lv_obj_t *ui_db_enabled_sw = NULL;
static lv_obj_t *ui_db_status_lbl = NULL;
static lv_obj_t *ui_db_last_export_lbl = NULL;
static lv_obj_t *ui_db_keyboard = NULL;
static lv_timer_t *ui_db_status_update_timer = NULL;

#define COLOR_DATABASE  0x9370DB  /* Medium purple */

static void db_back_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        _ui_screen_change(ui_screen_setting, LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0);
    }
}

static void db_save_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        struct mariadb_config config;

        /* Get values from UI */
        const char *host = lv_textarea_get_text(ui_db_host_ta);
        const char *port_str = lv_textarea_get_text(ui_db_port_ta);
        const char *user = lv_textarea_get_text(ui_db_user_ta);
        const char *pass = lv_textarea_get_text(ui_db_pass_ta);
        const char *dbname = lv_textarea_get_text(ui_db_name_ta);
        const char *table = lv_textarea_get_text(ui_db_table_ta);
        const char *interval_str = lv_textarea_get_text(ui_db_interval_ta);

        memset(&config, 0, sizeof(config));
        config.enabled = lv_obj_has_state(ui_db_enabled_sw, LV_STATE_CHECKED);
        strncpy(config.host, host, sizeof(config.host) - 1);
        strncpy(config.user, user, sizeof(config.user) - 1);
        strncpy(config.password, pass, sizeof(config.password) - 1);
        strncpy(config.database, dbname, sizeof(config.database) - 1);
        strncpy(config.table, table, sizeof(config.table) - 1);
        config.port = atoi(port_str);
        config.interval_minutes = atoi(interval_str);

        if (config.port == 0) config.port = 3306;
        if (config.interval_minutes == 0) config.interval_minutes = 5;
        if (strlen(config.table) == 0) strncpy(config.table, "sensor_data", sizeof(config.table) - 1);
        if (strlen(config.database) == 0) strncpy(config.database, "sensors", sizeof(config.database) - 1);

        indicator_mariadb_set_config(&config);

        lv_label_set_text(ui_db_status_lbl, "Config saved!");
        lv_obj_set_style_text_color(ui_db_status_lbl, lv_color_hex(0x00FF00), 0);
        ESP_LOGI(TAG, "Database config saved: host=%s, port=%d, user=%s, db=%s, table=%s",
                 config.host, config.port, config.user, config.database, config.table);
    }
}

static lv_timer_t *db_test_timer = NULL;
static int db_test_timeout_counter = 0;

static void db_test_timer_cb(lv_timer_t *timer)
{
    int status = indicator_mariadb_get_last_status();

    db_test_timeout_counter++;

    /* Timeout after 30 seconds (150 * 200ms) */
    if (db_test_timeout_counter > 150) {
        lv_timer_del(db_test_timer);
        db_test_timer = NULL;
        lv_label_set_text(ui_db_status_lbl, "Timeout - check logs");
        lv_obj_set_style_text_color(ui_db_status_lbl, lv_color_hex(0xFF4444), 0);
        return;
    }

    if (status == -99) {
        /* Still pending, keep waiting */
        int dots = db_test_timeout_counter % 4;
        const char *msgs[] = {"Testing", "Testing.", "Testing..", "Testing..."};
        lv_label_set_text(ui_db_status_lbl, msgs[dots]);
        return;
    }

    /* Test complete, stop timer */
    lv_timer_del(db_test_timer);
    db_test_timer = NULL;

    if (status == 0) {
        lv_label_set_text(ui_db_status_lbl, "OK! Data exported");
        lv_obj_set_style_text_color(ui_db_status_lbl, lv_color_hex(0x00FF00), 0);
    } else {
        char buf[48];
        const char *err_msg = "Unknown error";
        switch (status) {
            case -1: err_msg = "Not configured"; break;
            case -2: err_msg = "Sensor/memory error"; break;
            case -3: err_msg = "Connection failed"; break;
            case -4: err_msg = "Query failed"; break;
            case -5: err_msg = "Timeout"; break;
        }
        snprintf(buf, sizeof(buf), "Error %d: %s", status, err_msg);
        lv_label_set_text(ui_db_status_lbl, buf);
        lv_obj_set_style_text_color(ui_db_status_lbl, lv_color_hex(0xFF4444), 0);
    }
}

static void db_test_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        /* Stop any existing timer */
        if (db_test_timer) {
            lv_timer_del(db_test_timer);
            db_test_timer = NULL;
        }

        /* First save current config */
        db_save_click_cb(e);

        lv_label_set_text(ui_db_status_lbl, "Testing...");
        lv_obj_set_style_text_color(ui_db_status_lbl, lv_color_hex(0xFFFF00), 0);

        /* Reset timeout counter */
        db_test_timeout_counter = 0;

        /* Trigger async test */
        int ret = indicator_mariadb_test_connection();
        if (ret < 0) {
            lv_label_set_text(ui_db_status_lbl, "Error: Task not running");
            lv_obj_set_style_text_color(ui_db_status_lbl, lv_color_hex(0xFF4444), 0);
            return;
        }

        /* Start timer to check result */
        db_test_timer = lv_timer_create(db_test_timer_cb, 200, NULL);
    }
}

static void db_ta_focus_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        if (ui_db_keyboard == NULL) {
            ui_db_keyboard = lv_keyboard_create(ui_screen_database);
        }
        lv_keyboard_set_textarea(ui_db_keyboard, ta);
        lv_obj_clear_flag(ui_db_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        if (ui_db_keyboard) {
            lv_obj_add_flag(ui_db_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void db_status_update_timer_cb(lv_timer_t *timer)
{
    if (ui_db_last_export_lbl == NULL) return;

    time_t last_export = indicator_mariadb_get_last_export_time();
    int last_status = indicator_mariadb_get_last_status();

    if (last_export == 0) {
        lv_label_set_text(ui_db_last_export_lbl, "Last Export: Never");
        lv_obj_set_style_text_color(ui_db_last_export_lbl, lv_color_hex(0x888888), 0);
    } else {
        time_t now;
        time(&now);
        int seconds_ago = (int)(now - last_export);

        char buf[64];
        if (seconds_ago < 60) {
            snprintf(buf, sizeof(buf), "Last Export: %ds ago", seconds_ago);
        } else if (seconds_ago < 3600) {
            snprintf(buf, sizeof(buf), "Last Export: %dm ago", seconds_ago / 60);
        } else {
            snprintf(buf, sizeof(buf), "Last Export: %dh %dm ago",
                     seconds_ago / 3600, (seconds_ago % 3600) / 60);
        }

        if (last_status == 0) {
            strncat(buf, " - OK", sizeof(buf) - strlen(buf) - 1);
            lv_obj_set_style_text_color(ui_db_last_export_lbl, lv_color_hex(0x00FF00), 0);
        } else if (last_status == -99) {
            strncat(buf, " - Running...", sizeof(buf) - strlen(buf) - 1);
            lv_obj_set_style_text_color(ui_db_last_export_lbl, lv_color_hex(0xFFFF00), 0);
        } else {
            strncat(buf, " - Failed", sizeof(buf) - strlen(buf) - 1);
            lv_obj_set_style_text_color(ui_db_last_export_lbl, lv_color_hex(0xFF4444), 0);
        }
        lv_label_set_text(ui_db_last_export_lbl, buf);
    }
}

static void create_database_screen(void)
{
    /* Create database settings screen - 480x480 display */
    ui_screen_database = lv_obj_create(NULL);
    lv_obj_set_size(ui_screen_database, 480, 480);
    lv_obj_set_style_bg_color(ui_screen_database, lv_color_hex(0x1a1a2e), 0);

    const int margin = 15;
    const int content_width = 480 - (margin * 2);

    /* Header bar */
    lv_obj_t *header = lv_obj_create(ui_screen_database);
    lv_obj_set_size(header, 480, 50);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x252545), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 80, 36);
    lv_obj_set_pos(back_btn, margin, 7);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x3a3a5a), 0);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, db_back_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Database Export");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 30, 0);

    /* Main content area */
    lv_obj_t *content = lv_obj_create(ui_screen_database);
    lv_obj_set_size(content, content_width, 340);
    lv_obj_set_pos(content, margin, 60);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x222244), 0);
    lv_obj_set_style_border_width(content, 1, 0);
    lv_obj_set_style_border_color(content, lv_color_hex(0x3a3a5a), 0);
    lv_obj_set_style_radius(content, 10, 0);
    lv_obj_set_style_pad_all(content, 15, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    int y = 0;
    int fh = 36;   /* Field height */
    int rs = 50;   /* Row spacing */
    int field_w = content_width - 30;  /* Full width minus padding */

    /* Row 1: Enable switch */
    lv_obj_t *enable_lbl = lv_label_create(content);
    lv_label_set_text(enable_lbl, "Enable Export");
    lv_obj_set_style_text_font(enable_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(enable_lbl, 0, y + 5);

    ui_db_enabled_sw = lv_switch_create(content);
    lv_obj_set_size(ui_db_enabled_sw, 50, 25);
    lv_obj_set_pos(ui_db_enabled_sw, field_w - 50, y + 2);
    y += rs;

    /* Row 2: Host + Port */
    lv_obj_t *host_lbl = lv_label_create(content);
    lv_label_set_text(host_lbl, "Host");
    lv_obj_set_style_text_font(host_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(host_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(host_lbl, 0, y);

    ui_db_host_ta = lv_textarea_create(content);
    lv_obj_set_size(ui_db_host_ta, field_w - 100, fh);
    lv_obj_set_pos(ui_db_host_ta, 0, y + 18);
    lv_textarea_set_placeholder_text(ui_db_host_ta, "hostname or IP");
    lv_textarea_set_one_line(ui_db_host_ta, true);
    lv_obj_add_event_cb(ui_db_host_ta, db_ta_focus_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *port_lbl = lv_label_create(content);
    lv_label_set_text(port_lbl, "Port");
    lv_obj_set_style_text_font(port_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(port_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(port_lbl, field_w - 90, y);

    ui_db_port_ta = lv_textarea_create(content);
    lv_obj_set_size(ui_db_port_ta, 90, fh);
    lv_obj_set_pos(ui_db_port_ta, field_w - 90, y + 18);
    lv_textarea_set_text(ui_db_port_ta, "3306");
    lv_textarea_set_one_line(ui_db_port_ta, true);
    lv_textarea_set_accepted_chars(ui_db_port_ta, "0123456789");
    lv_obj_add_event_cb(ui_db_port_ta, db_ta_focus_cb, LV_EVENT_ALL, NULL);
    y += rs + 8;

    /* Row 3: User + Password */
    lv_obj_t *user_lbl = lv_label_create(content);
    lv_label_set_text(user_lbl, "User");
    lv_obj_set_style_text_font(user_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(user_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(user_lbl, 0, y);

    ui_db_user_ta = lv_textarea_create(content);
    lv_obj_set_size(ui_db_user_ta, (field_w - 10) / 2, fh);
    lv_obj_set_pos(ui_db_user_ta, 0, y + 18);
    lv_textarea_set_placeholder_text(ui_db_user_ta, "username");
    lv_textarea_set_one_line(ui_db_user_ta, true);
    lv_obj_add_event_cb(ui_db_user_ta, db_ta_focus_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *pass_lbl = lv_label_create(content);
    lv_label_set_text(pass_lbl, "Password");
    lv_obj_set_style_text_font(pass_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pass_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(pass_lbl, (field_w + 10) / 2, y);

    ui_db_pass_ta = lv_textarea_create(content);
    lv_obj_set_size(ui_db_pass_ta, (field_w - 10) / 2, fh);
    lv_obj_set_pos(ui_db_pass_ta, (field_w + 10) / 2, y + 18);
    lv_textarea_set_placeholder_text(ui_db_pass_ta, "password");
    lv_textarea_set_one_line(ui_db_pass_ta, true);
    lv_textarea_set_password_mode(ui_db_pass_ta, true);
    lv_obj_add_event_cb(ui_db_pass_ta, db_ta_focus_cb, LV_EVENT_ALL, NULL);
    y += rs + 8;

    /* Row 4: Database + Table */
    lv_obj_t *db_lbl = lv_label_create(content);
    lv_label_set_text(db_lbl, "Database");
    lv_obj_set_style_text_font(db_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(db_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(db_lbl, 0, y);

    ui_db_name_ta = lv_textarea_create(content);
    lv_obj_set_size(ui_db_name_ta, (field_w - 10) / 2, fh);
    lv_obj_set_pos(ui_db_name_ta, 0, y + 18);
    lv_textarea_set_placeholder_text(ui_db_name_ta, "database name");
    lv_textarea_set_one_line(ui_db_name_ta, true);
    lv_obj_add_event_cb(ui_db_name_ta, db_ta_focus_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *table_lbl = lv_label_create(content);
    lv_label_set_text(table_lbl, "Table");
    lv_obj_set_style_text_font(table_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(table_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(table_lbl, (field_w + 10) / 2, y);

    ui_db_table_ta = lv_textarea_create(content);
    lv_obj_set_size(ui_db_table_ta, (field_w - 10) / 2, fh);
    lv_obj_set_pos(ui_db_table_ta, (field_w + 10) / 2, y + 18);
    lv_textarea_set_placeholder_text(ui_db_table_ta, "table name");
    lv_textarea_set_one_line(ui_db_table_ta, true);
    lv_obj_add_event_cb(ui_db_table_ta, db_ta_focus_cb, LV_EVENT_ALL, NULL);
    y += rs + 8;

    /* Row 5: Interval */
    lv_obj_t *int_lbl = lv_label_create(content);
    lv_label_set_text(int_lbl, "Export Interval (minutes)");
    lv_obj_set_style_text_font(int_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(int_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(int_lbl, 0, y);

    ui_db_interval_ta = lv_textarea_create(content);
    lv_obj_set_size(ui_db_interval_ta, 80, fh);
    lv_obj_set_pos(ui_db_interval_ta, 0, y + 18);
    lv_textarea_set_text(ui_db_interval_ta, "5");
    lv_textarea_set_one_line(ui_db_interval_ta, true);
    lv_textarea_set_accepted_chars(ui_db_interval_ta, "0123456789");
    lv_obj_add_event_cb(ui_db_interval_ta, db_ta_focus_cb, LV_EVENT_ALL, NULL);

    /* Buttons */
    lv_obj_t *save_btn = lv_btn_create(content);
    lv_obj_set_size(save_btn, 110, 40);
    lv_obj_set_pos(save_btn, field_w - 230, y + 14);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x529D53), 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, LV_SYMBOL_SAVE " Save");
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(save_btn, db_save_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *test_btn = lv_btn_create(content);
    lv_obj_set_size(test_btn, 110, 40);
    lv_obj_set_pos(test_btn, field_w - 110, y + 14);
    lv_obj_set_style_bg_color(test_btn, lv_color_hex(0x4EACE4), 0);
    lv_obj_t *test_lbl = lv_label_create(test_btn);
    lv_label_set_text(test_lbl, LV_SYMBOL_UPLOAD " Test");
    lv_obj_set_style_text_font(test_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(test_lbl);
    lv_obj_add_event_cb(test_btn, db_test_click_cb, LV_EVENT_CLICKED, NULL);

    /* Status bar at bottom */
    lv_obj_t *status_bar = lv_obj_create(ui_screen_database);
    lv_obj_set_size(status_bar, content_width, 60);
    lv_obj_set_pos(status_bar, margin, 410);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x202040), 0);
    lv_obj_set_style_border_width(status_bar, 1, 0);
    lv_obj_set_style_border_color(status_bar, lv_color_hex(0x3a3a5a), 0);
    lv_obj_set_style_radius(status_bar, 10, 0);
    lv_obj_set_style_pad_all(status_bar, 10, 0);

    ui_db_status_lbl = lv_label_create(status_bar);
    lv_label_set_text(ui_db_status_lbl, "Ready");
    lv_obj_set_style_text_font(ui_db_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui_db_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(ui_db_status_lbl, 0, 0);

    ui_db_last_export_lbl = lv_label_create(status_bar);
    lv_label_set_text(ui_db_last_export_lbl, "Last Export: Never");
    lv_obj_set_style_text_font(ui_db_last_export_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ui_db_last_export_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(ui_db_last_export_lbl, 0, 20);

    /* Start status update timer */
    if (ui_db_status_update_timer == NULL) {
        ui_db_status_update_timer = lv_timer_create(db_status_update_timer_cb, 1000, NULL);
    }

    /* Load current config */
    struct mariadb_config config;
    if (indicator_mariadb_get_config(&config) == 0) {
        if (config.enabled) {
            lv_obj_add_state(ui_db_enabled_sw, LV_STATE_CHECKED);
        }
        if (strlen(config.host) > 0) {
            lv_textarea_set_text(ui_db_host_ta, config.host);
        }
        if (strlen(config.user) > 0) {
            lv_textarea_set_text(ui_db_user_ta, config.user);
        }
        if (strlen(config.password) > 0) {
            lv_textarea_set_text(ui_db_pass_ta, config.password);
        }
        if (strlen(config.database) > 0) {
            lv_textarea_set_text(ui_db_name_ta, config.database);
        }
        if (strlen(config.table) > 0) {
            lv_textarea_set_text(ui_db_table_ta, config.table);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", config.port);
        lv_textarea_set_text(ui_db_port_ta, buf);
        snprintf(buf, sizeof(buf), "%d", config.interval_minutes);
        lv_textarea_set_text(ui_db_interval_ta, buf);
    }

    /* Trigger initial status update */
    db_status_update_timer_cb(NULL);

    ESP_LOGI(TAG, "Database settings screen created (480x480)");
}

static void db_settings_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (ui_screen_database == NULL) {
            create_database_screen();
        }
        _ui_screen_change(ui_screen_database, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

static void loc_settings_click_cb(lv_event_t *e);

static void extend_settings_screen(void)
{
    /* The original settings screen has 3 buttons at x: -148, 0, 148
     * We need to rearrange to fit 4 buttons, or add a scroll
     * Let's add the 4th button below the existing ones */

    /* Create Database settings button */
    lv_obj_t *btn_database = lv_btn_create(ui_screen_setting);
    lv_obj_set_size(btn_database, 140, 80);
    lv_obj_align(btn_database, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(btn_database, lv_color_hex(COLOR_DATABASE), 0);
    lv_obj_clear_flag(btn_database, LV_OBJ_FLAG_SCROLLABLE);

    /* Database icon (using symbol) */
    lv_obj_t *db_icon = lv_label_create(btn_database);
    lv_label_set_text(db_icon, LV_SYMBOL_UPLOAD);
    lv_obj_set_style_text_font(db_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(db_icon, LV_ALIGN_CENTER, 0, -10);

    /* Database title */
    lv_obj_t *db_title = lv_label_create(btn_database);
    lv_label_set_text(db_title, "Database");
    lv_obj_set_style_text_font(db_title, &lv_font_montserrat_14, 0);
    lv_obj_align(db_title, LV_ALIGN_CENTER, 0, 18);

    lv_obj_add_event_cb(btn_database, db_settings_click_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "Database settings button added to settings screen");

    lv_obj_t *btn_location = lv_btn_create(ui_screen_setting);
    lv_obj_set_size(btn_location, 140, 80);
    lv_obj_align(btn_location, LV_ALIGN_BOTTOM_MID, 148, -40);
    lv_obj_set_style_bg_color(btn_location, lv_color_hex(0xE07020), 0);
    lv_obj_clear_flag(btn_location, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *loc_icon = lv_label_create(btn_location);
    lv_label_set_text(loc_icon, LV_SYMBOL_GPS);
    lv_obj_set_style_text_font(loc_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(loc_icon, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *loc_title = lv_label_create(btn_location);
    lv_label_set_text(loc_title, "Location");
    lv_obj_set_style_text_font(loc_title, &lv_font_montserrat_14, 0);
    lv_obj_align(loc_title, LV_ALIGN_CENTER, 0, 18);

    lv_obj_add_event_cb(btn_location, loc_settings_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_align(btn_database, LV_ALIGN_BOTTOM_MID, -74, -40);
    lv_obj_align(btn_location, LV_ALIGN_BOTTOM_MID, 74, -40);
}

static lv_obj_t *ui_screen_location = NULL;
static lv_obj_t *ui_loc_name_ta = NULL;
static lv_obj_t *ui_loc_status_lbl = NULL;

static void loc_ta_focus_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        lv_obj_t *kb = lv_keyboard_create(lv_scr_act());
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_set_user_data(ta, kb);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_t *kb = (lv_obj_t *)lv_obj_get_user_data(ta);
        if (kb) {
            lv_keyboard_set_textarea(kb, NULL);
            lv_obj_del(kb);
            lv_obj_set_user_data(ta, NULL);
        }
    }
}

static void loc_save_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *name = lv_textarea_get_text(ui_loc_name_ta);
    indicator_city_set_custom_name(name);
    ESP_LOGI("view", "Location saved: '%s'", name);

    if (strlen(name) > 0) {
        lv_label_set_text(ui_city, name);
    }

    if (ui_loc_status_lbl) {
        char buf[48];
        if (strlen(name) > 0) {
            snprintf(buf, sizeof(buf), "Saved: %s", name);
            lv_obj_set_style_text_color(ui_loc_status_lbl, lv_color_hex(0x00FF00), 0);
        } else {
            snprintf(buf, sizeof(buf), "Cleared - using auto-detect");
            lv_obj_set_style_text_color(ui_loc_status_lbl, lv_color_hex(0x888888), 0);
        }
        lv_label_set_text(ui_loc_status_lbl, buf);
    }
}

static void loc_clear_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_textarea_set_text(ui_loc_name_ta, "");
    indicator_city_set_custom_name(NULL);
    ESP_LOGI("view", "Location cleared, back to auto-detect");
}

static void loc_back_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        _ui_screen_change(ui_screen_setting, LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0);
    }
}

static void create_location_screen(void)
{
    ui_screen_location = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_screen_location, lv_color_hex(0x1A1A1A), 0);
    lv_obj_clear_flag(ui_screen_location, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_btn_create(ui_screen_location);
    lv_obj_set_size(back_btn, 50, 50);
    lv_obj_set_pos(back_btn, 10, 15);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_img_src(back_btn, &ui_img_back_png, 0);
    lv_obj_add_event_cb(back_btn, loc_back_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(ui_screen_location);
    lv_label_set_text(title, "Location");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    lv_obj_t *desc = lv_label_create(ui_screen_location);
    lv_label_set_text(desc, "Override the auto-detected city name.\nLeave empty to use auto-detect.");
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(desc, lv_color_hex(0x999999), 0);
    lv_obj_set_width(desc, 400);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *name_lbl = lv_label_create(ui_screen_location);
    lv_label_set_text(name_lbl, "Display Name");
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(name_lbl, 40, 130);

    ui_loc_name_ta = lv_textarea_create(ui_screen_location);
    lv_obj_set_size(ui_loc_name_ta, 400, 45);
    lv_obj_set_pos(ui_loc_name_ta, 40, 160);
    lv_textarea_set_placeholder_text(ui_loc_name_ta, "e.g. Pernitz");
    lv_textarea_set_one_line(ui_loc_name_ta, true);
    lv_textarea_set_max_length(ui_loc_name_ta, 24);
    lv_obj_add_event_cb(ui_loc_name_ta, loc_ta_focus_cb, LV_EVENT_ALL, NULL);

    char current[32] = {0};
    indicator_city_get_custom_name(current, sizeof(current));
    if (strlen(current) > 0) {
        lv_textarea_set_text(ui_loc_name_ta, current);
    }

    lv_obj_t *save_btn = lv_btn_create(ui_screen_location);
    lv_obj_set_size(save_btn, 160, 55);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_LEFT, 40, -30);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x529D53), 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, LV_SYMBOL_SAVE "  Save");
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(save_btn, loc_save_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear_btn = lv_btn_create(ui_screen_location);
    lv_obj_set_size(clear_btn, 160, 55);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_RIGHT, -40, -30);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0x666666), 0);
    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, LV_SYMBOL_TRASH "  Clear");
    lv_obj_set_style_text_font(clear_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(clear_lbl);
    lv_obj_add_event_cb(clear_btn, loc_clear_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *status_bar = lv_obj_create(ui_screen_location);
    lv_obj_set_size(status_bar, 400, 40);
    lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -100);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 8, 0);

    ui_loc_status_lbl = lv_label_create(status_bar);
    lv_label_set_text(ui_loc_status_lbl, "");
    lv_obj_set_style_text_font(ui_loc_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui_loc_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_center(ui_loc_status_lbl);

    ESP_LOGI("view", "Location settings screen created");
}

static void loc_settings_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (ui_screen_location == NULL) {
            create_location_screen();
        }
        _ui_screen_change(ui_screen_location, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
    }
}

/*****************************************************************/
// sensor chart
/*****************************************************************/

typedef struct sensor_chart_display
{
    lv_color_t color;
    
    char name[32];
    char units[32];
    struct view_data_sensor_history_data *p_info;
} sensor_chart_display_t;

static char date_hour[96][6] = { };
static lv_obj_t *ui_sensor_stats_label = NULL;
static char date_day[7][6] = { };
static uint16_t sensor_data_resolution = 0;
static uint16_t sensor_data_multiple = 1;

static void event_chart_week_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    if(code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED) {
        lv_obj_invalidate(obj); /*To make the value boxes visible*/
    }
    else if(code == LV_EVENT_DRAW_PART_BEGIN) {
        lv_obj_draw_part_dsc_t * dsc = lv_event_get_param(e);
        /*Set the markers' text*/
        if(dsc->part == LV_PART_TICKS && dsc->id == LV_CHART_AXIS_PRIMARY_X) {
            lv_snprintf(dsc->text, dsc->text_length, "%s", (char *)&date_day[dsc->value][0]);
        } else if(dsc->part == LV_PART_ITEMS) {

            const lv_chart_series_t * ser = dsc->sub_part_ptr;

            if(lv_chart_get_type(obj) == LV_CHART_TYPE_LINE) {
                dsc->rect_dsc->outline_color = lv_color_white();
                dsc->rect_dsc->outline_width = 2;
            }
            else {
                dsc->rect_dsc->shadow_color = ser->color;
                dsc->rect_dsc->shadow_width = 15;
                dsc->rect_dsc->shadow_spread = 0;
            }

            char buf[8];
            snprintf(buf, sizeof(buf), "%.*f", sensor_data_resolution, (float)dsc->value / sensor_data_multiple);

            lv_point_t text_size;
            lv_txt_get_size(&text_size, buf,  &ui_font_font1, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

            lv_area_t txt_area;

            txt_area.x1 = dsc->draw_area->x1 + lv_area_get_width(dsc->draw_area) / 2 - text_size.x / 2;
            txt_area.x2 = txt_area.x1 + text_size.x;
            txt_area.y2 = dsc->draw_area->y1 - LV_DPX(15);
            if( ser == ui_sensor_chart_week_series_low ) {
                txt_area.y2 += LV_DPX(70);
            }
            txt_area.y1 = txt_area.y2 - text_size.y;
            
            lv_draw_label_dsc_t label_dsc;
            lv_draw_label_dsc_init(&label_dsc);
            label_dsc.color = lv_color_white();
            label_dsc.font = &lv_font_montserrat_16;

            if(lv_chart_get_pressed_point(obj) == dsc->id) {
                    label_dsc.font = &lv_font_montserrat_20;
            } else {
                label_dsc.font = &lv_font_montserrat_16;
            }
            lv_draw_label(dsc->draw_ctx, &label_dsc, &txt_area,  buf, NULL);
        }
    }
}


static void event_chart_day_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    if(code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED) {
        lv_obj_invalidate(obj); /*To make the value boxes visible*/
    }
    else if(code == LV_EVENT_DRAW_PART_BEGIN) {
        lv_obj_draw_part_dsc_t * dsc = lv_event_get_param(e);
        /*Set the markers' text*/
        if(dsc->part == LV_PART_TICKS && dsc->id == LV_CHART_AXIS_PRIMARY_X) {
            int idx = (dsc->value * 95) / 23;
            if(idx < 0) idx = 0;
            if(idx > 95) idx = 95;
            lv_snprintf(dsc->text, dsc->text_length, "%s", (char *)&date_hour[idx][0]);
        } else if(dsc->part == LV_PART_ITEMS) {

            /*Add  a line mask that keeps the area below the line*/
            if(dsc->p1 && dsc->p2 ) {
                lv_draw_mask_line_param_t line_mask_param;
                lv_draw_mask_line_points_init(&line_mask_param, dsc->p1->x, dsc->p1->y, dsc->p2->x, dsc->p2->y,
                                                LV_DRAW_MASK_LINE_SIDE_BOTTOM);
                int16_t line_mask_id = lv_draw_mask_add(&line_mask_param, NULL);

                /*Add a fade effect: transparent bottom covering top*/
                lv_coord_t h = lv_obj_get_height(obj);
                lv_draw_mask_fade_param_t fade_mask_param;
                lv_draw_mask_fade_init(&fade_mask_param, &obj->coords, LV_OPA_COVER, obj->coords.y1 + h / 8, LV_OPA_TRANSP,
                                        obj->coords.y2);
                int16_t fade_mask_id = lv_draw_mask_add(&fade_mask_param, NULL);

                /*Draw a rectangle that will be affected by the mask*/
                lv_draw_rect_dsc_t draw_rect_dsc;
                lv_draw_rect_dsc_init(&draw_rect_dsc);
                draw_rect_dsc.bg_opa = LV_OPA_50;
                draw_rect_dsc.bg_color = dsc->line_dsc->color;

                lv_area_t obj_clip_area;
                _lv_area_intersect(&obj_clip_area, dsc->draw_ctx->clip_area, &obj->coords);
                const lv_area_t * clip_area_ori = dsc->draw_ctx->clip_area;
                dsc->draw_ctx->clip_area = &obj_clip_area;
                lv_area_t a;
                a.x1 = dsc->p1->x;
                a.x2 = dsc->p2->x - 1;
                a.y1 = LV_MIN(dsc->p1->y, dsc->p2->y);
                a.y2 = obj->coords.y2;
                lv_draw_rect(dsc->draw_ctx, &draw_rect_dsc, &a);
                dsc->draw_ctx->clip_area = clip_area_ori;
                /*Remove the masks*/
                lv_draw_mask_remove_id(line_mask_id);
                lv_draw_mask_remove_id(fade_mask_id);
            }


            const lv_chart_series_t * ser = dsc->sub_part_ptr;

            if(lv_chart_get_type(obj) == LV_CHART_TYPE_LINE) {
                dsc->rect_dsc->outline_color = lv_color_white();
                dsc->rect_dsc->outline_width = 2;
            }
            else {
                dsc->rect_dsc->shadow_color = ser->color;
                dsc->rect_dsc->shadow_width = 15;
                dsc->rect_dsc->shadow_spread = 0;
            }

            char buf[8];
            snprintf(buf, sizeof(buf), "%.*f", sensor_data_resolution, (float)dsc->value / sensor_data_multiple);

            lv_point_t text_size;
            lv_txt_get_size(&text_size, buf,  &ui_font_font1, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

            lv_area_t txt_area;

            txt_area.x1 = dsc->draw_area->x1 + lv_area_get_width(dsc->draw_area) / 2 - text_size.x / 2;
            txt_area.x2 = txt_area.x1 + text_size.x;
            txt_area.y2 = dsc->draw_area->y1 - LV_DPX(15);
            txt_area.y1 = txt_area.y2 - text_size.y;
            
            lv_draw_label_dsc_t label_dsc;
            lv_draw_label_dsc_init(&label_dsc);
            label_dsc.color = lv_color_white();
            label_dsc.font = &lv_font_montserrat_16;

            if(lv_chart_get_pressed_point(obj) == dsc->id) {
                    label_dsc.font = &lv_font_montserrat_20;
            } else {
                label_dsc.font = &lv_font_montserrat_16;
            }
            lv_draw_label(dsc->draw_ctx, &label_dsc, &txt_area,  buf, NULL);
        }
    }
}

void sensor_chart_update(sensor_chart_display_t *p_display)
{
	int i = 0;
	struct view_data_sensor_history_data *p_info = p_display->p_info;

    sensor_data_resolution = p_info->resolution;
    sensor_data_multiple = pow(10, p_info->resolution);

    float diff = 0;
    float chart_day_min =0;
    float chart_day_max =0;
    float chart_week_min =0;
    float chart_week_max =0;

    diff = p_info->day_max - p_info->day_min;
    if( diff <= 2 ) {
        diff = 4;
    }
    chart_day_min = (p_info->day_min - diff / 2.0); // sub quad
    chart_day_max = (p_info->day_max + diff / 2.0); //add quad

    ESP_LOGI(TAG, "data max:%.1f, min:%.1f, char max:%.1f, min:%.1f ", p_info->day_max, p_info->day_min,chart_day_max,chart_day_min);

    diff = p_info->week_max - p_info->week_min;
    if( diff <= 2) {
        diff = 4;
    }
    chart_week_min = (p_info->week_min - diff / 2.0); // sub quad
    chart_week_max = (p_info->week_max + diff / 2.0); //add quad

	lv_label_set_text(ui_sensor_data_title,p_display->name);

	lv_chart_set_series_color(ui_sensor_chart_day, ui_sensor_chart_day_series, p_display->color);
	lv_chart_set_range(ui_sensor_chart_day, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)chart_day_min * sensor_data_multiple, (lv_coord_t)chart_day_max * sensor_data_multiple);


	lv_chart_set_range(ui_sensor_chart_week, LV_CHART_AXIS_PRIMARY_Y,  (lv_coord_t)chart_week_min * sensor_data_multiple, (lv_coord_t)chart_week_max * sensor_data_multiple);
	lv_chart_set_series_color(ui_sensor_chart_week, ui_sensor_chart_week_series_low, p_display->color);
	lv_chart_set_series_color(ui_sensor_chart_week, ui_sensor_chart_week_series_hight, p_display->color);

    float avg_sum = 0;
    int avg_cnt = 0;
    for(i = 0; i < 96; i++) {
    	if( p_info->data_day[i].valid ) {
    		lv_chart_set_value_by_id(ui_sensor_chart_day, ui_sensor_chart_day_series, i,  sensor_data_multiple * p_info->data_day[i].data );
    		avg_sum += p_info->data_day[i].data;
    		avg_cnt++;
    	} else {
    		lv_chart_set_value_by_id(ui_sensor_chart_day, ui_sensor_chart_day_series, i, LV_CHART_POINT_NONE);
    	}
    	struct tm timeinfo = { 0 };
    	localtime_r(&p_info->data_day[i].timestamp, &timeinfo);
    	lv_snprintf((char*)&date_hour[i][0], 6, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    }

    float avg_val = avg_cnt > 0 ? avg_sum / avg_cnt : 0;
    if (ui_sensor_stats_label == NULL) {
        ui_sensor_stats_label = lv_label_create(ui_screen_sensor_chart);
        lv_obj_set_style_text_font(ui_sensor_stats_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui_sensor_stats_label, lv_color_hex(0xAAAAAA), 0);
        lv_obj_align(ui_sensor_stats_label, LV_ALIGN_TOP_MID, 0, 75);
    }
    if (avg_cnt > 0) {
        char stats_buf[64];
        snprintf(stats_buf, sizeof(stats_buf), "Min: %.1f   Max: %.1f   Avg: %.1f",
                 p_info->day_min, p_info->day_max, avg_val);
        lv_label_set_text(ui_sensor_stats_label, stats_buf);
    } else {
        lv_label_set_text(ui_sensor_stats_label, "Min: --   Max: --   Avg: --");
    }

    for(i = 0; i < 7; i++) {

    	if( p_info->data_week[i].valid ) {
            lv_chart_set_value_by_id(ui_sensor_chart_week, ui_sensor_chart_week_series_hight, i,  sensor_data_multiple * p_info->data_week[i].max );
            lv_chart_set_value_by_id(ui_sensor_chart_week, ui_sensor_chart_week_series_low, i,  sensor_data_multiple * p_info->data_week[i].min );
    	} else {
    		lv_chart_set_value_by_id(ui_sensor_chart_week, ui_sensor_chart_week_series_hight, i, LV_CHART_POINT_NONE);
    		lv_chart_set_value_by_id(ui_sensor_chart_week, ui_sensor_chart_week_series_low, i, LV_CHART_POINT_NONE);
    	}

    	struct tm timeinfo = { 0 };
    	localtime_r(&p_info->data_week[i].timestamp, &timeinfo);

    	lv_snprintf((char*)&date_day[i][0], 6, "%02d/%02d",timeinfo.tm_mon + 1, timeinfo.tm_mday);
    }

    lv_chart_refresh(ui_sensor_chart_day);
    lv_chart_refresh(ui_sensor_chart_week);

    /* Scroll day chart to the right (most recent data at index 47) */
    lv_obj_scroll_to_x(ui_sensor_chart_day, LV_COORD_MAX, LV_ANIM_OFF);
}

void sensor_chart_event_init(void)
{
    int i = 0;
    struct view_data_sensor_history_data  default_sensor_info;

    default_sensor_info.resolution = 1;

    time_t now = 0;
    time(&now);
    time_t time1 = (time_t)(now / 3600) * 3600;
    time_t time2 = (time_t)(now / 3600 / 24) * 3600 * 24;
    
    float min=90;
    float max=10;

    for(i = 0; i < 96; i++) { 
        default_sensor_info.data_day[i].data = (float)lv_rand(10, 90);
        default_sensor_info.data_day[i].timestamp = time1 + i * 900;
        default_sensor_info.data_day[i].valid = true;
        
        if( min > default_sensor_info.data_day[i].data) {
            min = default_sensor_info.data_day[i].data;
        }
        if( max < default_sensor_info.data_day[i].data) {
            max = default_sensor_info.data_day[i].data;
        }
    }
    default_sensor_info.day_max = max;
    default_sensor_info.day_min = min;


    min=90;
    max=10;

    for(i = 0; i < 7; i++) { 
        default_sensor_info.data_week[i].max = (float)lv_rand(60, 90);
        default_sensor_info.data_week[i].min = (float)lv_rand(10, 40);
        default_sensor_info.data_week[i].timestamp =  time2 + i * 3600 * 24;;
        default_sensor_info.data_week[i].valid = true;

        if( min > default_sensor_info.data_week[i].min) {
            min = default_sensor_info.data_week[i].min;
        }
        if( max < default_sensor_info.data_week[i].max) {
            max = default_sensor_info.data_week[i].max;
        }
    }
    default_sensor_info.week_max = max;
    default_sensor_info.week_min = min;

    lv_obj_add_event_cb(ui_sensor_chart_day, event_chart_day_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_sensor_chart_week, event_chart_week_cb, LV_EVENT_ALL, NULL);
}

/*****************************************************************/
// wifi config 
/*****************************************************************/
LV_IMG_DECLARE( ui_img_wifi_1_png);
LV_IMG_DECLARE( ui_img_wifi_2_png);
LV_IMG_DECLARE( ui_img_wifi_3_png);
LV_IMG_DECLARE( ui_img_lock_png);

static void wifi_list_init(void);

static lv_obj_t * ui_wifi_scan_wait;

static lv_obj_t * ui_wifi_list = NULL;
static lv_obj_t * ui_wifi_connecting;
static lv_obj_t * password_kb;
static lv_obj_t * ui_password_input;
static lv_obj_t * ui_wifi_connect_ret;

static char __g_cur_wifi_ssid[32];;

static uint8_t password_ready = false;

static void event_wifi_connect_cancel(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if(code == LV_EVENT_CLICKED) {
    	if( ui_wifi_connecting != NULL) {
            lv_obj_del(ui_wifi_connecting);
             ui_wifi_connecting = NULL;
    	     lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    	     lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
    	}
        
      if( password_kb != NULL) {
            lv_obj_del(password_kb);
    	    password_kb = NULL;
      }
    }
}

static void event_wifi_connect_join(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if(code == LV_EVENT_CLICKED) {
        struct view_data_wifi_config cfg;
        
        
        if( password_kb != NULL) {
            cfg.have_password = true;
            const char *password = lv_textarea_get_text(ui_password_input);
            strncpy((char *)cfg.password, password, sizeof(cfg.ssid));
  
            ESP_LOGI(TAG, "lv_obj_del: password_kb");
            lv_obj_del(password_kb);
            password_kb = NULL;
        } else {
            cfg.have_password = false;
        }

        if( ui_wifi_connecting != NULL) {
            strncpy((char *)cfg.ssid, (char *)__g_cur_wifi_ssid, sizeof(cfg.ssid));
            ESP_LOGI(TAG, "ssid: %s", cfg.ssid);

            ESP_LOGI(TAG, "lv_obj_del: ui_wifi_connecting");
            lv_obj_t* o = ui_wifi_connecting;
            ui_wifi_connecting = NULL;
            lv_obj_del(o);

            ESP_LOGI(TAG, "lv_obj_del: ui_wifi_connecting  end");

            lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
            
        }
        
        lv_obj_clear_flag( ui_wifi_scan_wait, LV_OBJ_FLAG_HIDDEN );
        if( ui_wifi_list ) {
            lv_obj_add_flag( ui_wifi_list, LV_OBJ_FLAG_HIDDEN );
        }
        //wifi_list_init();
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_CONNECT, &cfg, sizeof(cfg), portMAX_DELAY);
    }
}

static void event_wifi_password_input(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if(code == LV_EVENT_VALUE_CHANGED) {
    	if( ui_wifi_connecting != NULL  &&  ta != NULL) {

    		lv_obj_t * join_btn=(lv_obj_t * )e->user_data;

    		const char *password = lv_textarea_get_text(ta);
    		if( password != NULL) {
    			if( (strlen(password) >= 8)  && !password_ready) {
    				password_ready=true;
    				lv_obj_add_flag( join_btn, LV_OBJ_FLAG_CLICKABLE );
    				lv_obj_set_style_text_color( lv_obj_get_child(join_btn, 0), lv_color_hex(0x529d53), LV_PART_MAIN | LV_STATE_DEFAULT );
    			}
    			if( (strlen(password) < 8)  && password_ready) {
    				password_ready=false;
    				lv_obj_clear_flag( join_btn, LV_OBJ_FLAG_CLICKABLE );
    				lv_obj_set_style_text_color( lv_obj_get_child(join_btn, 0), lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
    			}
    		}
    	}
    }
}

static void event_keyboard_ready(lv_event_t * e)
{
    if( password_ready == true) {
        lv_event_send((lv_obj_t * )e->user_data, LV_EVENT_CLICKED, NULL);
    }
}

static lv_obj_t * create_wifi_connecting(lv_obj_t * parent, const char *p_ssid, bool have_password)
{
	lv_obj_t * wifi_con = lv_obj_create(parent);
	lv_obj_set_width( wifi_con, 420);
	lv_obj_set_height( wifi_con, 420);
	lv_obj_set_x( wifi_con, 0 );
	lv_obj_set_y( wifi_con, 0 );
	lv_obj_set_align( wifi_con, LV_ALIGN_CENTER );
	lv_obj_clear_flag( wifi_con, LV_OBJ_FLAG_SCROLLABLE );    /// Flags


	lv_obj_t * wifi_connect_cancel = lv_btn_create(wifi_con);
	lv_obj_set_width( wifi_connect_cancel, 100);
	lv_obj_set_height( wifi_connect_cancel, 50);
    lv_obj_set_align( wifi_connect_cancel, LV_ALIGN_TOP_LEFT );
    lv_obj_set_style_bg_color(wifi_connect_cancel, lv_color_hex(0x292831), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_bg_opa(wifi_connect_cancel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_add_event_cb(wifi_connect_cancel, event_wifi_connect_cancel, LV_EVENT_CLICKED, NULL);

	lv_obj_t * wifi_connect_cancel_title = lv_label_create(wifi_connect_cancel);
    lv_obj_set_width( wifi_connect_cancel_title, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( wifi_connect_cancel_title, LV_SIZE_CONTENT);   /// 1
    lv_label_set_text(wifi_connect_cancel_title,"Cancel");
    lv_obj_set_style_text_font(wifi_connect_cancel_title, &ui_font_font0, LV_PART_MAIN| LV_STATE_DEFAULT);


	lv_obj_t * wifi_connect_join = lv_btn_create(wifi_con);
	lv_obj_set_width( wifi_connect_join, 70);
	lv_obj_set_height( wifi_connect_join, 50);
    lv_obj_set_align( wifi_connect_join, LV_ALIGN_TOP_RIGHT );
    lv_obj_set_style_bg_color(wifi_connect_join, lv_color_hex(0x292831), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_bg_opa(wifi_connect_join, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_add_event_cb(wifi_connect_join, event_wifi_connect_join, LV_EVENT_CLICKED, NULL);

	lv_obj_t * wifi_connect_join_title = lv_label_create(wifi_connect_join);
    lv_obj_set_width( wifi_connect_join_title, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( wifi_connect_join_title, LV_SIZE_CONTENT);   /// 1
    lv_label_set_text(wifi_connect_join_title,"Join");
    lv_obj_set_style_text_font(wifi_connect_join_title, &ui_font_font0, LV_PART_MAIN| LV_STATE_DEFAULT);

    if( !have_password ){
    	lv_obj_set_style_text_color(wifi_connect_join_title, lv_color_hex(0x529d53), LV_PART_MAIN | LV_STATE_DEFAULT );
    } else {
    	lv_obj_clear_flag( wifi_connect_join, LV_OBJ_FLAG_CLICKABLE );
    }


	lv_obj_t * wifi_connect_ssid = lv_label_create(wifi_con);
    lv_obj_set_width( wifi_connect_ssid, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( wifi_connect_ssid, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( wifi_connect_ssid, LV_ALIGN_TOP_MID );
    lv_obj_set_y( wifi_connect_ssid, 50);

    lv_label_set_text_fmt( wifi_connect_ssid, "%s", p_ssid);
    lv_obj_set_style_text_font(wifi_connect_ssid, &ui_font_font1, LV_PART_MAIN| LV_STATE_DEFAULT);


    if( have_password ) {
		lv_obj_t * input_password_title = lv_label_create(wifi_con);
		lv_obj_set_width( input_password_title, LV_SIZE_CONTENT);  /// 1
		lv_obj_set_height( input_password_title, LV_SIZE_CONTENT);   /// 1
		lv_obj_set_align( input_password_title, LV_ALIGN_TOP_MID );
		lv_obj_set_y( input_password_title, 100);
		lv_obj_set_x( input_password_title, -80);

		lv_label_set_text_fmt( input_password_title, "Input password");
		//lv_obj_set_style_text_font(input_password_title, &ui_font_font0, LV_PART_MAIN| LV_STATE_DEFAULT);


		password_ready = false;

		ui_password_input = lv_textarea_create(wifi_con);
		lv_textarea_set_text(ui_password_input, "");
		//lv_textarea_set_password_mode(ui_password_input, true);  todo
		lv_textarea_set_one_line(ui_password_input, true);
		lv_obj_set_width(ui_password_input, lv_pct(80));
		lv_obj_set_align( ui_password_input, LV_ALIGN_TOP_MID );
		lv_obj_set_y( ui_password_input, 130);
		//lv_obj_set_x( ui_password_input, -80);
		lv_obj_set_style_bg_color(ui_password_input, lv_color_hex(0x6F6F6F), LV_PART_MAIN | LV_STATE_DEFAULT );
		lv_obj_set_style_bg_opa(ui_password_input, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
		lv_obj_add_event_cb(ui_password_input, event_wifi_password_input, LV_EVENT_VALUE_CHANGED, (void*)wifi_connect_join);

		password_kb = lv_keyboard_create(parent);
		lv_keyboard_set_textarea(password_kb, ui_password_input);
        lv_keyboard_set_popovers(password_kb, true);
        lv_obj_add_event_cb(password_kb, event_keyboard_ready, LV_EVENT_READY, wifi_connect_join);
    }

	return wifi_con;
}

static void event_wifi_connect(lv_event_t * e)
{
    bool have_password=false;
    char *p_wifi_ssid="";

    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);

    if(code == LV_EVENT_CLICKED) {
        if(lv_indev_get_type(lv_indev_get_act()) == LV_INDEV_TYPE_POINTER) {

        	// todo connect wifi
            if(ui_wifi_connecting == NULL) {
            	p_wifi_ssid= lv_list_get_btn_text(ui_wifi_list, ta);
                if( lv_obj_get_child_cnt(ta) > 2) {
                	have_password = true;
                }
                strncpy((char *)__g_cur_wifi_ssid, p_wifi_ssid, sizeof(__g_cur_wifi_ssid));
                lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
                ui_wifi_connecting = create_wifi_connecting(lv_layer_top(), (const char *) p_wifi_ssid, have_password);
                lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_50, 0);
                lv_obj_set_style_bg_color(lv_layer_top(), lv_palette_main(LV_PALETTE_GREY), 0);
            }
        }
    }
}


void timer_wifi_connect_ret_close(lv_timer_t * timer)
{
    if( ui_wifi_connect_ret != NULL) {
        lv_obj_del(ui_wifi_connect_ret);
        ui_wifi_connect_ret = NULL;
        lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
    }
}

static void ui_wifi_connect_ret_init( struct view_data_wifi_connet_ret_msg *p_msg)
{
    ui_wifi_connect_ret = lv_obj_create(lv_layer_top());
	lv_obj_set_width( ui_wifi_connect_ret, 300);
	lv_obj_set_height( ui_wifi_connect_ret, 150);
	lv_obj_set_x( ui_wifi_connect_ret, 0 );
	lv_obj_set_y( ui_wifi_connect_ret, 0 );
	lv_obj_set_align( ui_wifi_connect_ret, LV_ALIGN_CENTER );
	lv_obj_clear_flag( ui_wifi_connect_ret, LV_OBJ_FLAG_SCROLLABLE );    /// Flags
    lv_obj_set_style_bg_color( ui_wifi_connect_ret, lv_palette_main(LV_PALETTE_GREY), 0);

    lv_obj_t * wifi_connect_ret_msg = lv_label_create(ui_wifi_connect_ret);
    lv_obj_set_width( wifi_connect_ret_msg, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( wifi_connect_ret_msg, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( wifi_connect_ret_msg, LV_ALIGN_CENTER );

    lv_label_set_text_fmt( wifi_connect_ret_msg, "%s", p_msg->msg);
    lv_obj_set_style_text_font(wifi_connect_ret_msg, &ui_font_font0, LV_PART_MAIN| LV_STATE_DEFAULT);

    lv_timer_t * timer = lv_timer_create(timer_wifi_connect_ret_close, 1500, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

static void event_wifi_delete(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if(code == LV_EVENT_CLICKED) {

        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_CFG_DELETE, NULL, 0, portMAX_DELAY);

        if( ui_wifi_connecting != NULL) {
            lv_obj_del(ui_wifi_connecting);
             ui_wifi_connecting = NULL;
    	     lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    	     lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
    	}

        lv_obj_clear_flag( ui_wifi_scan_wait, LV_OBJ_FLAG_HIDDEN );
        if( ui_wifi_list ) {
            lv_obj_add_flag( ui_wifi_list, LV_OBJ_FLAG_HIDDEN );
        }
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_LIST_REQ, NULL, 0, portMAX_DELAY); //updata wifi list
    }
}

static lv_obj_t * ui_wifi_details(lv_obj_t * parent, const char *p_ssid)
{
	lv_obj_t * wifi_con = lv_obj_create(parent);
	lv_obj_set_width( wifi_con, 300);
	lv_obj_set_height( wifi_con, 200);
	lv_obj_set_x( wifi_con, 0 );
	lv_obj_set_y( wifi_con, 0 );
	lv_obj_set_align( wifi_con, LV_ALIGN_CENTER );
	lv_obj_clear_flag( wifi_con, LV_OBJ_FLAG_SCROLLABLE );    /// Flags


	lv_obj_t * wifi_connect_cancel = lv_btn_create(wifi_con);
	lv_obj_set_width( wifi_connect_cancel, 100);
	lv_obj_set_height( wifi_connect_cancel, 50);
    lv_obj_set_align( wifi_connect_cancel, LV_ALIGN_BOTTOM_RIGHT );
    lv_obj_set_style_bg_color(wifi_connect_cancel, lv_color_hex(0x292831), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_bg_opa(wifi_connect_cancel, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_add_event_cb(wifi_connect_cancel, event_wifi_connect_cancel, LV_EVENT_CLICKED, NULL);

	lv_obj_t * wifi_connect_cancel_title = lv_label_create(wifi_connect_cancel);
    lv_obj_set_width( wifi_connect_cancel_title, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( wifi_connect_cancel_title, LV_SIZE_CONTENT);   /// 1
    lv_label_set_text(wifi_connect_cancel_title,"Cancel");
    lv_obj_set_style_text_font(wifi_connect_cancel_title, &ui_font_font0, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_color( wifi_connect_cancel_title,  lv_color_hex(0x529d53), LV_PART_MAIN | LV_STATE_DEFAULT );

	lv_obj_t * wifi_connect_delete = lv_btn_create(wifi_con);
	lv_obj_set_width( wifi_connect_delete, 100);
	lv_obj_set_height( wifi_connect_delete, 50);
    lv_obj_set_align( wifi_connect_delete, LV_ALIGN_BOTTOM_LEFT );
    lv_obj_set_style_bg_color(wifi_connect_delete, lv_color_hex(0x292831), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_bg_opa(wifi_connect_delete, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_add_event_cb(wifi_connect_delete, event_wifi_delete, LV_EVENT_CLICKED, NULL); //todo

	lv_obj_t * wifi_connect_delete_title = lv_label_create(wifi_connect_delete);
    lv_obj_set_width( wifi_connect_delete_title, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( wifi_connect_delete_title, LV_SIZE_CONTENT);   /// 1
    lv_label_set_text(wifi_connect_delete_title,"Delete");
    lv_obj_set_style_text_font(wifi_connect_delete_title, &ui_font_font0, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_color( wifi_connect_delete_title, lv_color_hex(0xff0000), LV_PART_MAIN | LV_STATE_DEFAULT );

    lv_obj_t * wifi_connect_ssid = lv_label_create(wifi_con);
    lv_obj_set_width( wifi_connect_ssid, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( wifi_connect_ssid, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( wifi_connect_ssid, LV_ALIGN_CENTER );
    lv_obj_set_y( wifi_connect_ssid, -20);

    lv_label_set_text_fmt( wifi_connect_ssid, "%s", p_ssid);
    lv_obj_set_style_text_font(wifi_connect_ssid, &ui_font_font0, LV_PART_MAIN| LV_STATE_DEFAULT);

    return wifi_con;
}


static void event_wifi_details(lv_event_t * e)
{
    char *p_wifi_ssid="";

    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if(code == LV_EVENT_CLICKED) {
        if(lv_indev_get_type(lv_indev_get_act()) == LV_INDEV_TYPE_POINTER) {


            if(ui_wifi_connecting == NULL) {
            	p_wifi_ssid= lv_list_get_btn_text(ui_wifi_list, ta);

                strncpy((char *)__g_cur_wifi_ssid, p_wifi_ssid, sizeof(__g_cur_wifi_ssid));
                lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
                ui_wifi_connecting = ui_wifi_details(lv_layer_top(), (const char *) p_wifi_ssid);

                lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_50, 0);
                lv_obj_set_style_bg_color(lv_layer_top(), lv_palette_main(LV_PALETTE_GREY), 0);
            }
        }
    }
}

static void create_wifi_item(lv_obj_t * parent, const char *p_ssid, bool have_password, int rssi, bool is_connect)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_width(btn, 380);
    lv_obj_set_height(btn, 50);
    lv_obj_set_align(btn, LV_ALIGN_CENTER );
    if(is_connect) {
    	lv_obj_set_style_bg_color(btn, lv_color_hex(0x529d53), LV_PART_MAIN | LV_STATE_DEFAULT );
        lv_obj_add_event_cb(btn, event_wifi_details, LV_EVENT_CLICKED, NULL);
    } else {
    	lv_obj_set_style_bg_color(btn, lv_color_hex(0x2c2c2c), LV_PART_MAIN | LV_STATE_DEFAULT );
        lv_obj_add_event_cb(btn, event_wifi_connect, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t * wifi_ssid = lv_label_create(btn);
    lv_label_set_text_fmt(wifi_ssid, "%s", p_ssid);
    lv_obj_set_width( wifi_ssid, LV_SIZE_CONTENT);
    lv_obj_set_height( wifi_ssid, LV_SIZE_CONTENT);
    lv_obj_set_align( wifi_ssid, LV_ALIGN_LEFT_MID );
    lv_obj_set_style_text_font(wifi_ssid, &ui_font_font0, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_x( wifi_ssid, 10 );

    lv_obj_t *  wifi_rssi_icon= lv_img_create(btn);
    lv_obj_set_width( wifi_rssi_icon, LV_SIZE_CONTENT);  /// 22
    lv_obj_set_height( wifi_rssi_icon, LV_SIZE_CONTENT);
    lv_obj_set_align( wifi_rssi_icon, LV_ALIGN_RIGHT_MID );
    lv_obj_set_x( wifi_rssi_icon, -10 );


    switch (wifi_rssi_level_get(rssi)) {
		case 1:
			lv_img_set_src(wifi_rssi_icon, &ui_img_wifi_1_png);
			break;
		case 2:
			lv_img_set_src(wifi_rssi_icon, &ui_img_wifi_2_png);
			break;
		case 3:
			lv_img_set_src(wifi_rssi_icon, &ui_img_wifi_3_png);
			break;
		default:
			break;
	}

    if( have_password ) {
        lv_obj_t *  wifi_lock_icon= lv_img_create(btn);
        lv_img_set_src(wifi_lock_icon, &ui_img_lock_png);
        lv_obj_set_width( wifi_lock_icon, LV_SIZE_CONTENT);  /// 22
        lv_obj_set_height( wifi_lock_icon, LV_SIZE_CONTENT);
        lv_obj_set_align( wifi_lock_icon, LV_ALIGN_RIGHT_MID );
        lv_obj_set_x( wifi_lock_icon, -60 );
    }
}

static void wifi_list_init(void)
{
	if( ui_wifi_list != NULL){
        ESP_LOGI(TAG, "lv_obj_del: ui_wifi_list");
        //lv_obj_clean(ui_wifi_list);
        lv_obj_del(ui_wifi_list);
        ui_wifi_list=NULL;
	}
    ui_wifi_list = lv_list_create(ui_screen_wifi);
    lv_obj_set_style_pad_row(ui_wifi_list, 8, 0);

    lv_obj_set_align( ui_wifi_list, LV_ALIGN_CENTER );
    lv_obj_set_width( ui_wifi_list, 420);
    lv_obj_set_height( ui_wifi_list, 330);
    lv_obj_set_x( ui_wifi_list, 0 );
    lv_obj_set_y( ui_wifi_list, 35 );

    lv_obj_set_style_bg_color(ui_wifi_list, lv_color_hex(0x101418), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_bg_grad_color(ui_wifi_list, lv_color_hex(0x101418), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_border_color(ui_wifi_list, lv_color_hex(0x101418), LV_PART_MAIN | LV_STATE_DEFAULT );

    lv_obj_add_flag( ui_wifi_list, LV_OBJ_FLAG_HIDDEN );
}


void __ui_event_wifi_config( lv_event_t * e) {
    lv_event_code_t event_code = lv_event_get_code(e);lv_obj_t * target = lv_event_get_target(e);
    if( event_code == LV_EVENT_SCREEN_LOAD_START) {
        lv_obj_clear_flag( ui_wifi_scan_wait, LV_OBJ_FLAG_HIDDEN );
        if( ui_wifi_list ) {
            lv_obj_add_flag( ui_wifi_list, LV_OBJ_FLAG_HIDDEN );
        }
        //wifi_list_init();
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_LIST_REQ, NULL, 0, portMAX_DELAY);
    }
}


void wifi_list_event_init(void)
{
    ui_wifi_scan_wait = lv_spinner_create(ui_screen_wifi, 1000, 60);
    lv_obj_set_size(ui_wifi_scan_wait, 50, 50);
    lv_obj_center(ui_wifi_scan_wait);
    lv_obj_add_event_cb(ui_screen_wifi, __ui_event_wifi_config, LV_EVENT_SCREEN_LOAD_START, NULL);
}

/***********************************************************************************************************/

static void __view_event_handler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    lv_port_sem_take();
    switch (id)
    {
        case VIEW_EVENT_SCREEN_START: {
            uint8_t screen = *( uint8_t *)event_data;
            if( screen == SCREEN_WIFI_CONFIG) {
                lv_disp_load_scr( ui_screen_wifi);
            }
        }
        case VIEW_EVENT_TIME: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_TIME");
            bool time_format_24 = true;
            if( event_data) {
                time_format_24 = *( bool *)event_data;
            } 
            
            time_t now = 0;
            struct tm timeinfo = { 0 };
            char *p_wday_str;

            time(&now);
            localtime_r(&now, &timeinfo);
            char buf_h[3];
            char buf_m[3];
            char buf[6];
            int hour = timeinfo.tm_hour;

            if( ! time_format_24 ) {
                if( hour>=13 && hour<=23) {
                    hour = hour-12;
                }
            } 
            lv_snprintf(buf_h, sizeof(buf_h), "%02d", hour);
            lv_label_set_text(ui_hour_dis, buf_h);
            lv_snprintf(buf_m, sizeof(buf_m), "%02d", timeinfo.tm_min);
            lv_label_set_text(ui_min_dis, buf_m);

            lv_snprintf(buf, sizeof(buf), "%02d:%02d", hour, timeinfo.tm_min);
            lv_label_set_text(ui_time2, buf);
            lv_label_set_text(ui_time3, buf);

            switch (timeinfo.tm_wday)
            {
                case 0: p_wday_str="Sunday";break;
                case 1: p_wday_str="Monday";break;
                case 2: p_wday_str="Tuesday";break;
                case 3: p_wday_str="Wednesday";break;
                case 4: p_wday_str="Thursday";break;
                case 5: p_wday_str="Friday";break;
                case 6: p_wday_str="Saturday";break;
                default: p_wday_str="";break;
            }
            char buf1[32];
            lv_snprintf(buf1, sizeof(buf1), "%s, %02d / %02d / %04d", p_wday_str,  timeinfo.tm_mday, timeinfo.tm_mon+1, timeinfo.tm_year+1900);
            lv_label_set_text(ui_date, buf1);
            break;
        }

        case VIEW_EVENT_TIME_CFG_UPDATE: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_TIME_CFG_UPDATE");
            struct view_data_time_cfg *p_cfg = ( struct view_data_time_cfg *)event_data;
            
            if(  p_cfg->time_format_24 ) {
                lv_dropdown_set_selected(ui_time_format_cfg, 0);
            } else {
                lv_dropdown_set_selected(ui_time_format_cfg, 1);
            }

            if(  p_cfg->auto_update ) {
                lv_obj_add_state( ui_auto_update_cfg, LV_STATE_CHECKED);
                lv_obj_add_flag( ui_date_time, LV_OBJ_FLAG_HIDDEN );
            } else {
                lv_obj_clear_state( ui_auto_update_cfg, LV_STATE_CHECKED);
                lv_obj_clear_flag( ui_date_time, LV_OBJ_FLAG_HIDDEN );
            }
            

            struct tm *p_tm = localtime(&p_cfg->time);
            char buf[32];
            if( p_tm->tm_year+1900 > 1970 ) {
                lv_snprintf(buf, sizeof(buf), "%02d/%02d/%d", p_tm->tm_mday, p_tm->tm_mon +1, p_tm->tm_year+1900);
                lv_textarea_set_text(ui_date_cfg, buf);
            }

            lv_roller_set_selected(ui_hour_cfg, p_tm->tm_hour,LV_ANIM_OFF);
            lv_roller_set_selected(ui_min_cfg, p_tm->tm_min,LV_ANIM_OFF);
            lv_roller_set_selected(ui_sec_cfg, p_tm->tm_sec,LV_ANIM_OFF);

            if(  p_cfg->auto_update_zone ) {
                lv_obj_add_state( ui_zone_auto_update_cfg, LV_STATE_CHECKED);
                lv_obj_add_flag( ui_time_zone, LV_OBJ_FLAG_HIDDEN );
            } else {
                lv_obj_clear_state( ui_zone_auto_update_cfg, LV_STATE_CHECKED);
                lv_obj_clear_flag( ui_time_zone, LV_OBJ_FLAG_HIDDEN );
            }

            if(  p_cfg->zone >= 0) {
                lv_dropdown_set_selected(ui_time_zone_sign_cfg_, 0);
                lv_dropdown_set_selected(ui_time_zone_num_cfg, p_cfg->zone);
            } else {
                lv_dropdown_set_selected(ui_time_zone_sign_cfg_, 1);
                lv_dropdown_set_selected(ui_time_zone_num_cfg, 0 - p_cfg->zone);
            }

            if(  p_cfg->daylight ) {
                lv_obj_add_state( ui_daylight_cfg, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state( ui_daylight_cfg, LV_STATE_CHECKED);
            }
            break;
        }

        case VIEW_EVENT_CITY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_CITY");
            char *p_data = ( char *)event_data;
            lv_label_set_text(ui_city, p_data);
            break;
        }

        case VIEW_EVENT_DISPLAY_CFG: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_DISPLAY_CFG");
            struct view_data_display *p_cfg = ( struct view_data_display *)event_data;
            
            lv_slider_set_value(ui_brighness_cfg, p_cfg->brightness, LV_ANIM_OFF);
            if( p_cfg->sleep_mode_en ) {
                lv_obj_clear_state( ui_screen_always_on_cfg, LV_STATE_CHECKED);
                lv_obj_clear_flag( ui_turn_off_after_time, LV_OBJ_FLAG_HIDDEN );
                char buf[32];
                lv_snprintf(buf, sizeof(buf), "%d", p_cfg->sleep_mode_time_min);
                lv_textarea_set_text(ui_turn_off_after_time_cfg, buf);
                //lv_label_set_text_fmt(ui_turn_off_after_time_cfg, "%"LV_PRIu32, p_cfg->sleep_mode_time_min);
            } else {
                lv_obj_add_state( ui_screen_always_on_cfg, LV_STATE_CHECKED);
                lv_obj_add_flag( ui_turn_off_after_time, LV_OBJ_FLAG_HIDDEN );
            }
            
            break;
        }

        case VIEW_EVENT_WIFI_ST: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_WIFI_ST");
            struct view_data_wifi_st *p_st = ( struct view_data_wifi_st *)event_data;
            
            uint8_t *p_src =NULL;
            //todo is_network
            if ( p_st->is_connected ) {
                switch (wifi_rssi_level_get( p_st->rssi )) {
                    case 1:
                        p_src = &ui_img_wifi_1_png;
                        break;
                    case 2:
                        p_src = &ui_img_wifi_2_png;
                        break;
                    case 3:
                        p_src = &ui_img_wifi_3_png;
                        break;
                    default:
                        break;
                }
    
            } else {
                p_src = &ui_img_wifi_disconet_png;
            }

            lv_img_set_src(ui_wifi_st_1 , (void *)p_src);
            lv_img_set_src(ui_wifi_st_2 , (void *)p_src);
            lv_img_set_src(ui_wifi_st_3 , (void *)p_src);
            lv_img_set_src(ui_wifi_st_4 , (void *)p_src);
            lv_img_set_src(ui_wifi_st_5 , (void *)p_src);
            lv_img_set_src(ui_wifi_st_6 , (void *)p_src);
            lv_img_set_src(ui_wifi_st_7 , (void *)p_src);
            break;
        }
        case VIEW_EVENT_WIFI_LIST: {
            ESP_LOGI(TAG, "event: VIEW_DATA_WIFI_LIST");

            wifi_list_init(); // clear and init

            lv_obj_clear_flag( ui_wifi_list, LV_OBJ_FLAG_HIDDEN );
            lv_obj_add_flag( ui_wifi_scan_wait, LV_OBJ_FLAG_HIDDEN );

            if( event_data == NULL) {
                //lv_obj_add_flag( ui_wifi_scan_wait, LV_OBJ_FLAG_HIDDEN );
                break;
            }
            struct view_data_wifi_list *p_list = ( struct view_data_wifi_list *)event_data;
            bool have_password = true;
        
            if( p_list->is_connect) {
                create_wifi_item(ui_wifi_list,  p_list->connect.ssid, p_list->connect.auth_mode, p_list->connect.ssid, true);
            }
            for( int i = 0; i < p_list->cnt; i++ ) {
                ESP_LOGI(TAG, "ssid:%s, rssi:%d, auth mode:%d", p_list->aps[i].ssid, p_list->aps[i].rssi, p_list->aps[i].auth_mode);
                if( p_list->is_connect ) {
                    if( strcmp(p_list->aps[i].ssid, p_list->connect.ssid)  != 0) {
                        create_wifi_item(ui_wifi_list, p_list->aps[i].ssid, p_list->aps[i].auth_mode, p_list->aps[i].rssi, false);
                    }
                } else {
                    create_wifi_item(ui_wifi_list, p_list->aps[i].ssid, p_list->aps[i].auth_mode, p_list->aps[i].rssi, false);
                }
            }
            break;
        }
        case VIEW_EVENT_WIFI_CONNECT_RET: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_WIFI_CONNECT_RET");
            
            lv_obj_t * cur_screen = lv_scr_act();
        
            if( cur_screen !=  ui_screen_wifi) {
                break;
            }
            struct view_data_wifi_connet_ret_msg  *p_data = ( struct view_data_wifi_connet_ret_msg *) event_data;

            // update wifi list
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_LIST_REQ, NULL, 0, portMAX_DELAY);

            // show connect result
            ui_wifi_connect_ret_init( p_data);

            break;
        }

        case VIEW_EVENT_SENSOR_DATA: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_DATA");
            
            struct view_data_sensor_data  *p_data = (struct view_data_sensor_data *) event_data;
            char data_buf[32];

            memset(data_buf, 0, sizeof(data_buf));
            switch (p_data->sensor_type)
            {
                case SENSOR_DATA_CO2: {
                    snprintf(data_buf, sizeof(data_buf), "%d", (int)p_data->vaule);
                    ESP_LOGI(TAG, "update co2:%s", data_buf);
                    lv_label_set_text(ui_co2_data, data_buf);
                    break;
                }
                case SENSOR_DATA_TVOC: {
                    snprintf(data_buf, sizeof(data_buf), "%d", (int)p_data->vaule);
                    ESP_LOGI(TAG, "update tvoc:%s", data_buf);
                    lv_label_set_text(ui_tvoc_data, data_buf);
                    break;
                }           
                case SENSOR_DATA_TEMP: {
                    snprintf(data_buf, sizeof(data_buf), "%.1f", p_data->vaule);
                    ESP_LOGI(TAG, "update temp:%s", data_buf);
                    lv_label_set_text(ui_temp_data_2, data_buf);
                    break;
                }
                case SENSOR_DATA_HUMIDITY: {
                    snprintf(data_buf, sizeof(data_buf), "%d",(int) p_data->vaule);
                    ESP_LOGI(TAG, "update humidity:%s", data_buf);
                    lv_label_set_text(ui_humidity_data_2, data_buf);
                    break;
                }
            default:
                break;
            }
            break;
        }
        case VIEW_EVENT_SENSOR_DATA_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_DATA_HISTORY");
            struct view_data_sensor_history_data  *p_data = (struct view_data_sensor_history_data *) event_data;
            sensor_chart_display_t sensor_chart;

            switch (p_data->sensor_type)
            {
                case SENSOR_DATA_CO2: {
                    sensor_chart.color = lv_color_hex(0x529D53);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "CO2");
                    break;
                }
                case SENSOR_DATA_TVOC: {
                    sensor_chart.color = lv_color_hex(0xE06D3D);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "tVOC");
                    break;
                }
                case SENSOR_DATA_TEMP: {
                    sensor_chart.color = lv_color_hex(0xEEBF41);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "Temp");
                    break;
                }
                case SENSOR_DATA_HUMIDITY: {
                    sensor_chart.color = lv_color_hex(0x4EACE4);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "Humidity");
                    break;
                }
                /* Extended sensor types */
                case SENSOR_DATA_TEMP_EXT: {
                    sensor_chart.color = lv_color_hex(COLOR_TEMP_EXT);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "Temp Ext");
                    break;
                }
                case SENSOR_DATA_HUMIDITY_EXT: {
                    sensor_chart.color = lv_color_hex(COLOR_HUM_EXT);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "Humidity Ext");
                    break;
                }
                case SENSOR_DATA_PM1_0: {
                    sensor_chart.color = lv_color_hex(COLOR_PM);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "PM1.0");
                    break;
                }
                case SENSOR_DATA_PM2_5: {
                    sensor_chart.color = lv_color_hex(COLOR_PM);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "PM2.5");
                    break;
                }
                case SENSOR_DATA_PM10: {
                    sensor_chart.color = lv_color_hex(COLOR_PM);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "PM10");
                    break;
                }
                case SENSOR_DATA_NO2: {
                    sensor_chart.color = lv_color_hex(COLOR_GAS);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "NO2");
                    break;
                }
                case SENSOR_DATA_C2H5OH: {
                    sensor_chart.color = lv_color_hex(COLOR_GAS);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "C2H5OH");
                    break;
                }
                case SENSOR_DATA_VOC: {
                    sensor_chart.color = lv_color_hex(COLOR_GAS);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "VOC");
                    break;
                }
                case SENSOR_DATA_CO: {
                    sensor_chart.color = lv_color_hex(COLOR_GAS);
                    sensor_chart.p_info = p_data;
                    strcpy(sensor_chart.name, "CO");
                    break;
                }
            default:
                break;
            }
            sensor_chart_update( &sensor_chart);

            break;
        }
        case VIEW_EVENT_SCREEN_CTRL: {
            bool  *p_st = (bool *) event_data;

            if ( *p_st == 1) {
                lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    	        lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
            } else {
                lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(lv_layer_top(), lv_color_black(), 0);
            }
            break;
        }
        case VIEW_EVENT_FACTORY_RESET: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_FACTORY_RESET");
            lv_disp_load_scr(ui_screen_factory);
            break;
        }

        default:
            break;
    }
    lv_port_sem_give();
}



/* Vorwaerts-Deklaration fuer das Sauna-UI (ui_sauna.c). */
extern void ui_sauna_init(void);

int indicator_view_init(void)
{
    ui_init();

    lv_obj_t *branding = lv_label_create(ui_background);
    lv_label_set_text(branding, "supersauna.club");
    lv_obj_set_style_text_font(branding, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(branding, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_align(branding, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(branding, -10);

    wifi_list_event_init();
    sensor_chart_event_init();
    extend_sensor_screen();   /* Add extended sensors to main sensor screen */
    extend_settings_screen(); /* Add database settings to settings screen */

    /* Legacy-Event-Handler KOMPLETT abgehaengt - er registriert keine
     * events mehr. Alles laeuft jetzt ueber ui_sauna.c. So koennen
     * die alten screens (wifi/sensor/factory) keine lv_scr_load's mehr
     * machen und unseren Sauna-UI ueberschreiben.                    */

    ui_sauna_init();

    return 0;
}