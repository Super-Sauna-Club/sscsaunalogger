#include "indicator_storage.h"
#include "nvs_flash.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

#define STORAGE_NAMESPACE "indicator"

static const char *NVS_TAG = "NVS_REC";

/* v0.2.12: panic-streak counter in RTC slow memory.
 * Hintergrund: am 2026-05-04 hatten wir 1185+ reboots durch NVS-page-
 * korruption (garbage-pointer in nvs::Page metadata). Crashes traten
 * NICHT in nvs_flash_init() auf (das kommt sauber durch), sondern erst
 * spaeter bei nvs_set/nvs_get-zugriffen durch korrupte storage-listen.
 * Self-perpetuating, weil jeder reboot wieder NVS schreibt.
 *
 * Recovery-pattern: counter ueberlebt soft-reset (RTC_NOINIT_ATTR), bei
 * power-on initialisieren wir ihn neu (magic-check). Bei jedem panic-
 * reset wird er hochgezaehlt, bei normalem boot zurueckgesetzt. Nach
 * THRESHOLD panics in folge gehen wir davon aus dass NVS korrupt ist
 * und erasen die ganze partition. Verliert config (wifi etc) aber
 * holt das geraet aus dem bootloop raus.                                  */
RTC_NOINIT_ATTR static uint32_t s_panic_streak;
RTC_NOINIT_ATTR static uint32_t s_panic_streak_magic;
#define PANIC_STREAK_MAGIC      0x5C5A5751U
/* v0.3.1: war 5. Hochgesetzt damit bei einem panic-bootloop nicht
 * automatisch NVS-erase + datenverlust passiert. Eigene erfahrung:
 * panic-loop hat in v0.3.0 wifi-config + sessions gewiped bevor
 * der bug gefunden war.                                             */
#define PANIC_STREAK_THRESHOLD  100

int indicator_storage_init(void)
{
    /* RTC-counter pflegen */
    if (s_panic_streak_magic != PANIC_STREAK_MAGIC) {
        s_panic_streak = 0;
        s_panic_streak_magic = PANIC_STREAK_MAGIC;
    }
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
        rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
        s_panic_streak++;
    } else {
        s_panic_streak = 0;
    }

    /* erste verteidigung: dokumentierte init-failure-modes (no-free-pages,
     * version-mismatch). war schon im original drin, hier nur erweitert
     * um CORRUPT_KEY_PART (ESP-IDF v5.1+).                                  */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND
#ifdef ESP_ERR_NVS_CORRUPT_KEY_PART
        || ret == ESP_ERR_NVS_CORRUPT_KEY_PART
#endif
        ) {
        ESP_LOGW(NVS_TAG, "nvs_flash_init returned %s -> erase+retry",
                 esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    /* zweite verteidigung: panic-streak. wenn die letzten N boots alle
     * panic'd haben, ist NVS sehr wahrscheinlich korrupt - erasen.         */
    if (s_panic_streak >= PANIC_STREAK_THRESHOLD) {
        ESP_LOGE(NVS_TAG, "panic-streak=%u >= %u: NVS-corruption assumed, erasing",
                 (unsigned)s_panic_streak, (unsigned)PANIC_STREAK_THRESHOLD);
        s_panic_streak = 0;
        if (ret == ESP_OK) {
            nvs_flash_deinit();
        }
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(NVS_TAG, "nvs_flash_init final fail: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);  /* loud panic - dann sieht man's wenigstens */
    }
    ESP_LOGI(NVS_TAG, "NVS ok (rr=%d panic_streak=%u)",
             (int)rr, (unsigned)s_panic_streak);
    return 0;
}

esp_err_t indicator_storage_write(char *p_key, void *p_data, size_t len)
{
    nvs_handle_t my_handle;
    esp_err_t err;
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(my_handle,  p_key, p_data, len);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t indicator_storage_read(char *p_key, void *p_data, size_t *p_len)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_get_blob(my_handle, p_key, p_data, p_len);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }
    nvs_close(my_handle);
    return ESP_OK;
}

