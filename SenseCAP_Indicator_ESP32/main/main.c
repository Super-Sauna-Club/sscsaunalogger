#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "bsp_board.h"
#include "lv_port.h"
#include "esp_event.h"
#include "esp_event_base.h"

#include "indicator_model.h"
#include "indicator_view.h"
#include "indicator_storage.h"
#include "view_data.h"
#include "app_version.h"
//#include "indicator_controller.h"

static const char *TAG = "app_main";

#define VERSION   "v1.0.0"

/* NVS-key fuer boot-observability. Liegt im gemeinsamen 'indicator'-
 * namespace via indicator_storage_read/write.                           */
#define BOOT_INFO_STORAGE  "boot_info"

struct boot_info_nvs {
    uint8_t  version;   /* 1 */
    uint8_t  _pad[3];
    uint32_t boot_count;
} __attribute__((packed));

static const char *__reset_reason_str(esp_reset_reason_t rr)
{
    switch (rr) {
        case ESP_RST_POWERON:  return "POWERON";
        case ESP_RST_EXT:      return "EXT";
        case ESP_RST_SW:       return "SW";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "UNKNOWN";
    }
}

static uint32_t __boot_counter_tick_and_get(void)
{
    struct boot_info_nvs rec = {0};
    size_t len = sizeof(rec);
    esp_err_t err = indicator_storage_read(BOOT_INFO_STORAGE, &rec, &len);
    if (err != ESP_OK || len != sizeof(rec) || rec.version != 1) {
        memset(&rec, 0, sizeof(rec));
        rec.version = 1;
    }
    rec.boot_count++;
    indicator_storage_write(BOOT_INFO_STORAGE, &rec, sizeof(rec));
    return rec.boot_count;
}

#define SENSECAP  "\n\
   _____                      _________    ____         \n\
  / ___/___  ____  ________  / ____/   |  / __ \\       \n\
  \\__ \\/ _ \\/ __ \\/ ___/ _ \\/ /   / /| | / /_/ /   \n\
 ___/ /  __/ / / (__  )  __/ /___/ ___ |/ ____/         \n\
/____/\\___/_/ /_/____/\\___/\\____/_/  |_/_/           \n\
--------------------------------------------------------\n\
 Version: %s %s %s\n\
--------------------------------------------------------\n\
"

ESP_EVENT_DEFINE_BASE(VIEW_EVENT_BASE);
esp_event_loop_handle_t view_event_handle;

/* Eigener event-loop-runner. Wird von xTaskCreateWithCaps mit PSRAM-
 * stack erzeugt damit die 16 KB nicht aus DRAM kommen. Loop laeuft
 * forever - esp_event_loop_run blockiert bis ein event reinkommt
 * oder timeout (portMAX_DELAY = forever). Returns ESP_OK nach jedem
 * abgearbeiteten event-batch.                                       */
void __view_event_loop_task(void *arg)
{
    while (1) {
        esp_event_loop_run(view_event_handle, portMAX_DELAY);
    }
}

/* v0.2.7: capturen vor event-loop-create, broadcasten sobald event-loop da. */
static struct view_data_boot_info g_boot_info;


void app_main(void)
{
    ESP_LOGI("", SENSECAP, VERSION, __DATE__, __TIME__);

    /* v0.2.7: Reset-Reason + Boot-Counter SOFORT capturen. NVS-init muss
     * vorher laufen, deshalb kommt es noch vor bsp_board_init.           */
    indicator_storage_init();
    esp_reset_reason_t g_reset_reason = esp_reset_reason();
    uint32_t g_boot_count = __boot_counter_tick_and_get();
    g_boot_info.boot_count   = g_boot_count;
    g_boot_info.reset_reason = (uint8_t)g_reset_reason;
    ESP_LOGW("BOOT", "SSC v%s | reset=%s(%d) boot_count=%u",
             SSC_APP_VERSION, __reset_reason_str(g_reset_reason),
             (int)g_reset_reason, (unsigned)g_boot_count);

    ESP_ERROR_CHECK(bsp_board_init());
    lv_port_init();


    /* v0.2.14: event-loop OHNE auto-task, damit wir die task selbst mit
     * PSRAM-stack via xTaskCreateWithCaps erzeugen koennen.
     * task_name=NULL signalisiert ESP-IDF "no automatic task" - der
     * caller muss esp_event_loop_run() periodisch aufrufen.
     * 16 KB stack in PSRAM = 16 KB DRAM gewinn (vorher in DRAM via
     * esp_event_loop_create internal allocation).                       */
    esp_event_loop_args_t view_event_task_args = {
        .queue_size = 32,           /* 32 statt 10: bei 1-Hz session-worker +
                                       sensor-events. Volle queue fuehrt zu
                                       gedroppten live-events (20ms timeout). */
        .task_name = NULL,
    };
    ESP_ERROR_CHECK(esp_event_loop_create(&view_event_task_args, &view_event_handle));

    /* Eigene task fuer den event-loop - stack in PSRAM. 16 KB ist needed
     * weil session-save-flow mariadb-TCP + cJSON + NVS + lv_scr_load
     * triggert (10 KB hat zu stack-overflow gefuehrt).                  */
    extern void __view_event_loop_task(void *arg);
    TaskHandle_t s_view_loop_task = NULL;
    BaseType_t r = xTaskCreateWithCaps(
        __view_event_loop_task, "view_event_task",
        16384, NULL, uxTaskPriorityGet(NULL), &s_view_loop_task,
        MALLOC_CAP_SPIRAM);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "view_event_task PSRAM-stack create failed (rc=%d) "
                      "- fallback DRAM-stack via xTaskCreate", (int)r);
        if (xTaskCreate(__view_event_loop_task, "view_event_task",
                        16384, NULL, uxTaskPriorityGet(NULL),
                        &s_view_loop_task) != pdPASS) {
            ESP_LOGE(TAG, "DRAM-fallback fail too - critical");
        }
    }

    /* v0.2.7: boot-info broadcasten sobald event-loop up. UI kann sich
     * subscriben und im INFO-Screen / Home-Toast darauf reagieren.      */
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_BOOT_INFO,
                      &g_boot_info, sizeof(g_boot_info),
                      pdMS_TO_TICKS(100));


    lv_port_sem_take();
    indicator_view_init();
    lv_port_sem_give();

    indicator_model_init();
    indicator_controller_init();

    /* v0.2.14: heap-observability nach init + periodisch.
     * Hintergrund: am 2026-05-04 hatten wir nach init nur 4559 byte
     * internal heap free (sollte ~30 KB sein). Ohne log keine ground-
     * truth fuer fixes. Boot-banner zeigt post-init-state, periodische
     * 60s-zeile zeigt drift waehrend wifi/lvgl/session-arbeitslast.    */
    static char buffer[160];
    snprintf(buffer, sizeof(buffer),
             "post-init: DRAM big=%u free=%u total=%u | "
             "PSRAM big=%u free=%u total=%u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI("MEM", "%s", buffer);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        snprintf(buffer, sizeof(buffer),
                 "DRAM big=%u free=%u | PSRAM big=%u free=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGI("MEM", "%s", buffer);
    }
}
