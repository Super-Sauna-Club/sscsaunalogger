#include "ui_sensor.h"
#include <stdio.h>

#define SENSOR_PAGE_COUNT 4

static lv_obj_t *sensor_pages[SENSOR_PAGE_COUNT];
static lv_obj_t *page_indicator;
static int current_page = 0;

// Page 1: Environment (External Temp, External Humidity, CO2, TVOC)
static lv_obj_t *lbl_temp_ext, *lbl_hum_ext, *lbl_co2, *lbl_tvoc;
// Page 2: Particulate Matter (PM1.0, PM2.5, PM10)
static lv_obj_t *lbl_pm1_0, *lbl_pm2_5, *lbl_pm10;
// Page 3: Gas Sensors (NO2, C2H5OH, VOC, CO)
static lv_obj_t *lbl_gm102b, *lbl_gm302b, *lbl_gm502b, *lbl_gm702b;
// Page 4: Internal Sensors (Internal Temp, Internal Humidity)
static lv_obj_t *lbl_temp_int, *lbl_hum_int;

static lv_obj_t *container;

static void create_sensor_label_sized(lv_obj_t *parent, const char *title, lv_obj_t **value_label, int y_offset, const lv_font_t *value_font)
{
    lv_obj_t *title_lbl = lv_label_create(parent);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 20, y_offset);

    *value_label = lv_label_create(parent);
    lv_label_set_text(*value_label, "---");
    lv_obj_set_style_text_font(*value_label, value_font, 0);
    lv_obj_set_style_text_color(*value_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(*value_label, LV_ALIGN_TOP_LEFT, 20, y_offset + 22);
}

static void create_sensor_label(lv_obj_t *parent, const char *title, lv_obj_t **value_label, int y_offset)
{
    create_sensor_label_sized(parent, title, value_label, y_offset, &lv_font_montserrat_24);
}

static void create_page_environment(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Environment");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00BFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Temperature and Humidity with larger font (32px) */
    create_sensor_label_sized(parent, "Temperature (\xC2\xB0C)", &lbl_temp_ext, 50, &lv_font_montserrat_32);
    create_sensor_label_sized(parent, "Humidity (%)", &lbl_hum_ext, 115, &lv_font_montserrat_32);
    create_sensor_label(parent, "CO2 (ppm)", &lbl_co2, 180);
    create_sensor_label(parent, "TVOC Index", &lbl_tvoc, 240);
}

static void create_page_particulate(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Particulate Matter");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF6347), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    create_sensor_label(parent, "PM1.0 (ug/m3)", &lbl_pm1_0, 70);
    create_sensor_label(parent, "PM2.5 (ug/m3)", &lbl_pm2_5, 140);
    create_sensor_label(parent, "PM10 (ug/m3)", &lbl_pm10, 210);
}

static void create_page_gas_sensors(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Gas Sensors");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFD700), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    create_sensor_label(parent, "NO2 (V)", &lbl_gm102b, 50);
    create_sensor_label(parent, "C2H5OH (V)", &lbl_gm302b, 110);
    create_sensor_label(parent, "VOC (V)", &lbl_gm502b, 170);
    create_sensor_label(parent, "CO (V)", &lbl_gm702b, 230);
}

static void create_page_internal(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Internal Sensors");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF7F), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Temperature and Humidity with larger font (32px) */
    create_sensor_label_sized(parent, "Temperature (\xC2\xB0C)", &lbl_temp_int, 80, &lv_font_montserrat_32);
    create_sensor_label_sized(parent, "Humidity (%)", &lbl_hum_int, 160, &lv_font_montserrat_32);
}

static void update_page_indicator(void)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", current_page + 1, SENSOR_PAGE_COUNT);
    lv_label_set_text(page_indicator, buf);
}

static void show_current_page(void)
{
    for (int i = 0; i < SENSOR_PAGE_COUNT; i++) {
        if (i == current_page) {
            lv_obj_clear_flag(sensor_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sensor_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_page_indicator();
}

void ui_sensor_init(lv_obj_t *parent)
{
    container = lv_obj_create(parent);
    lv_obj_set_size(container, 480, 320);
    lv_obj_center(container);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);

    for (int i = 0; i < SENSOR_PAGE_COUNT; i++) {
        sensor_pages[i] = lv_obj_create(container);
        lv_obj_set_size(sensor_pages[i], 480, 290);
        lv_obj_align(sensor_pages[i], LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(sensor_pages[i], lv_color_hex(0x1a1a2e), 0);
        lv_obj_set_style_border_width(sensor_pages[i], 0, 0);
        lv_obj_add_flag(sensor_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    create_page_environment(sensor_pages[0]);
    create_page_particulate(sensor_pages[1]);
    create_page_gas_sensors(sensor_pages[2]);
    create_page_internal(sensor_pages[3]);

    page_indicator = lv_label_create(container);
    lv_obj_set_style_text_font(page_indicator, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(page_indicator, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(page_indicator, LV_ALIGN_BOTTOM_MID, 0, -5);

    current_page = 0;
    show_current_page();
}

void ui_sensor_update(const struct view_data_sensor *data)
{
    if (!data) return;
    static char buf[64];

    // Page 1: Environment
    snprintf(buf, sizeof(buf), "%.1f", data->temp_external);
    lv_label_set_text(lbl_temp_ext, buf);

    snprintf(buf, sizeof(buf), "%.1f", data->humidity_external);
    lv_label_set_text(lbl_hum_ext, buf);

    snprintf(buf, sizeof(buf), "%.0f", data->co2);
    lv_label_set_text(lbl_co2, buf);

    snprintf(buf, sizeof(buf), "%.0f", data->tvoc);
    lv_label_set_text(lbl_tvoc, buf);

    // Page 2: Particulate Matter
    snprintf(buf, sizeof(buf), "%.1f", data->pm1_0);
    lv_label_set_text(lbl_pm1_0, buf);

    snprintf(buf, sizeof(buf), "%.1f", data->pm2_5);
    lv_label_set_text(lbl_pm2_5, buf);

    snprintf(buf, sizeof(buf), "%.1f", data->pm10);
    lv_label_set_text(lbl_pm10, buf);

    // Page 3: Gas Sensors (only ppm)
    snprintf(buf, sizeof(buf), "%.2f", data->multigas_gm102b[0]);
    lv_label_set_text(lbl_gm102b, buf);

    snprintf(buf, sizeof(buf), "%.2f", data->multigas_gm302b[0]);
    lv_label_set_text(lbl_gm302b, buf);

    snprintf(buf, sizeof(buf), "%.2f", data->multigas_gm502b[0]);
    lv_label_set_text(lbl_gm502b, buf);

    snprintf(buf, sizeof(buf), "%.2f", data->multigas_gm702b[0]);
    lv_label_set_text(lbl_gm702b, buf);

    // Page 4: Internal Sensors
    snprintf(buf, sizeof(buf), "%.1f", data->temp_internal);
    lv_label_set_text(lbl_temp_int, buf);

    snprintf(buf, sizeof(buf), "%.1f", data->humidity_internal);
    lv_label_set_text(lbl_hum_int, buf);
}

void ui_sensor_next_page(void)
{
    current_page = (current_page + 1) % SENSOR_PAGE_COUNT;
    show_current_page();
}

void ui_sensor_prev_page(void)
{
    current_page = (current_page - 1 + SENSOR_PAGE_COUNT) % SENSOR_PAGE_COUNT;
    show_current_page();
}
