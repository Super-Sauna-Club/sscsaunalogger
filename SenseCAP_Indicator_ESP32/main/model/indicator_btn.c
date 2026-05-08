#include "indicator_btn.h"
#include "indicator_display.h"
#include "bsp_btn.h"
#include "esp_timer.h"

/* v0.3.1: hardware-button entschaerft. Original-seeed-code hatte:
 *   - single-click im sleep mode -> esp_restart()
 *   - 3 sek halten -> sleep mode
 *   - single-click nach sleep mode -> esp_restart()
 *   - 10 sek halten -> nvs_flash_erase() + esp_restart() (FACTORY RESET!)
 * Bei transport oder versehentlichem antippen war das ein bootloop-
 * + datenverlust-risiko (insbesondere wifi-NVS verloren). Fuer den
 * sauna-logger gibt es keinen use-case fuer factory-reset oder sleep-
 * mode am hardware-button. Display-on/off bleibt als einziger toggle.
 * Settings (incl. wifi-reset) sind ueber das touchdisplay erreichbar. */

static void __btn_click_callback(void* arg)
{
    bool st=0;
    if( indicator_display_st_get()) {
        ESP_LOGI("btn", "click, off");
        indicator_display_off();
        st = 0;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_CTRL, &st, sizeof(st), portMAX_DELAY);
    } else {
        ESP_LOGI("btn", "click, on");
        st = 1;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_CTRL, &st, sizeof(st), portMAX_DELAY);
        indicator_display_on();
    }
}

int indicator_btn_init(void)
{
    /* Nur single-click registriert. Long-press, double-click und
     * press-up sind explizit deaktiviert um versehentliche resets +
     * factory-resets durch button-noise zu verhindern.               */
    bsp_btn_register_callback( BOARD_BTN_ID_USER, BUTTON_SINGLE_CLICK,
                               __btn_click_callback, NULL);
    return 0;
}
