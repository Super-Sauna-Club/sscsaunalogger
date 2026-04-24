#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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


    esp_event_loop_args_t view_event_task_args = {
        /* 32 statt 10: bei 1-Hz session-worker + sensor-events mit
         * lv_port_sem_take in den handlern kann die queue sonst voll
         * laufen und post_live_event verliert events (20ms timeout) -
         * timer bleibt dann auf 0:00 weil UI nie einen live-event sieht. */
        .queue_size = 32,
        .task_name = "view_event_task",
        .task_priority = uxTaskPriorityGet(NULL),
        .task_stack_size = 16384,   /* 16 KB fuer session-save-flow
                                     * (mariadb-TCP + cJSON + NVS +
                                     * lv_scr_load) - 10 KB haben zu
                                     * stack-overflow gefuehrt.      */
        .task_core_id = tskNO_AFFINITY
    };

    ESP_ERROR_CHECK(esp_event_loop_create(&view_event_task_args, &view_event_handle));

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

    static char buffer[128];    /* Make sure buffer is enough for `sprintf` */
    while (1) {
        // sprintf(buffer, "   Biggest /     Free /    Total\n"
        //         "\t  DRAM : [%8d / %8d / %8d]\n"
        //         "\t PSRAM : [%8d / %8d / %8d]",
        //         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        //         heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        //         heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
        //         heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        //         heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        //         heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        // ESP_LOGI("MEM", "%s", buffer);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
