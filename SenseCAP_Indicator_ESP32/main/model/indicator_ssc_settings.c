#include "indicator_ssc_settings.h"
#include "indicator_storage.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ssc_set";
#define NVS_KEY  "ssc_set"

ssc_settings_t g_ssc_settings = {
    .version             = SSC_SETTINGS_VERSION,
    .ntp_enabled         = 1,
    .wifi_enabled        = 1,
    .dash_intern_visible = 1,
    .brightness_pct      = 100,
};

void ssc_settings_init(void)
{
    ssc_settings_t loaded = {0};
    size_t len = sizeof(loaded);
    esp_err_t err = indicator_storage_read(NVS_KEY, &loaded, &len);
    if (err == ESP_OK && len == sizeof(loaded) &&
        loaded.version == SSC_SETTINGS_VERSION) {
        g_ssc_settings = loaded;
        /* clamp brightness in den erlaubten bereich falls NVS-bytes
         * ausserhalb sind (kann nach manueller migration vorkommen).  */
        if (g_ssc_settings.brightness_pct < SSC_BRIGHTNESS_MIN)
            g_ssc_settings.brightness_pct = SSC_BRIGHTNESS_MIN;
        if (g_ssc_settings.brightness_pct > SSC_BRIGHTNESS_MAX)
            g_ssc_settings.brightness_pct = SSC_BRIGHTNESS_MAX;
        ESP_LOGI(TAG, "loaded: ntp=%u wifi=%u dash=%u bri=%u",
                 g_ssc_settings.ntp_enabled,
                 g_ssc_settings.wifi_enabled,
                 g_ssc_settings.dash_intern_visible,
                 g_ssc_settings.brightness_pct);
    } else {
        ESP_LOGW(TAG, "no/invalid NVS entry (err=%d len=%u v=%u) - defaults",
                 err, (unsigned)len,
                 err == ESP_OK ? loaded.version : 0);
        ssc_settings_save();
    }
}

void ssc_settings_save(void)
{
    g_ssc_settings.version = SSC_SETTINGS_VERSION;
    esp_err_t err = indicator_storage_write(NVS_KEY,
                                            &g_ssc_settings,
                                            sizeof(g_ssc_settings));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save fail: %d", err);
    } else {
        ESP_LOGI(TAG, "saved: ntp=%u wifi=%u dash=%u bri=%u",
                 g_ssc_settings.ntp_enabled,
                 g_ssc_settings.wifi_enabled,
                 g_ssc_settings.dash_intern_visible,
                 g_ssc_settings.brightness_pct);
    }
}

/* Forward-decls aus den jeweiligen modulen - vermeidet zirkulaere
 * header-includes (settings ist low-level, model-module sind high). */
extern void indicator_wifi_set_enabled(bool en);
extern void indicator_time_set_ntp_enabled(bool en);
extern void ssc_settings_apply_brightness(uint8_t pct);       /* in ui_sauna.c */
extern void ssc_settings_apply_dashboard_visibility(void);    /* in ui_sauna.c */

void ssc_settings_apply_runtime(void)
{
    indicator_wifi_set_enabled((bool)g_ssc_settings.wifi_enabled);
    indicator_time_set_ntp_enabled((bool)g_ssc_settings.ntp_enabled);
    ssc_settings_apply_brightness(g_ssc_settings.brightness_pct);
    ssc_settings_apply_dashboard_visibility();
}
