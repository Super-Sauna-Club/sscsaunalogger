#include "indicator_sensor.h"
#include "driver/uart.h"
#include "cobs.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdlib.h>
#include <math.h>
#include "time.h"

#define SENSOR_HISTORY_DATA_DEBUG  0
#define SENSOR_COMM_DEBUG    1

#define HISTORY_INTERVAL_SECONDS  900

/* Validate float value - reject NaN and Inf */
#define FLOAT_IS_VALID(f) (isfinite(f))


#define ESP32_RP2040_TXD (19)
#define ESP32_RP2040_RXD (20)
#define ESP32_RP2040_RTS (UART_PIN_NO_CHANGE)
#define ESP32_RP2040_CTS (UART_PIN_NO_CHANGE)

#define ESP32_COMM_PORT_NUM      (2)
#define ESP32_COMM_BAUD_RATE     (115200)
#define ESP32_RP2040_COMM_TASK_STACK_SIZE    (1024*4)
#define BUF_SIZE (512)

uint8_t buf[BUF_SIZE];   //recv 
uint8_t data[BUF_SIZE];  //decode

enum  pkt_type {

    PKT_TYPE_CMD_COLLECT_INTERVAL = 0xA0, //uin32_t 
    PKT_TYPE_CMD_BEEP_ON  = 0xA1,  //uin32_t  ms: on time 
    PKT_TYPE_CMD_BEEP_OFF = 0xA2,
    PKT_TYPE_CMD_SHUTDOWN = 0xA3, //uin32_t 
    PKT_TYPE_CMD_POWER_ON = 0xA4,

    PKT_TYPE_SENSOR_SCD41_TEMP  = 0xB0, // float
    PKT_TYPE_SENSOR_SCD41_HUMIDITY = 0xB1, // float
    PKT_TYPE_SENSOR_SCD41_CO2 = 0xB2, // float

    PKT_TYPE_SENSOR_SHT41_TEMP = 0xB3, // float
    PKT_TYPE_SENSOR_SHT41_HUMIDITY = 0xB4, // float

    PKT_TYPE_SENSOR_TVOC_INDEX = 0xB5, // float
    PKT_TYPE_SENSOR_PM1_0 = 0xB6,
    PKT_TYPE_SENSOR_PM2_5 = 0xB7,
    PKT_TYPE_SENSOR_PM10 = 0xB8,
    PKT_TYPE_SENSOR_GM102B_NO2 = 0xB9,
    PKT_TYPE_SENSOR_GM302B_C2H5OH = 0xBA,
    PKT_TYPE_SENSOR_GM502B_VOC = 0xBB,
    PKT_TYPE_SENSOR_GM702B_CO = 0xBC,
    PKT_TYPE_SENSOR_TEMP_EXTERNAL = 0xBD,
    PKT_TYPE_SENSOR_HUMIDITY_EXTERNAL = 0xBE,

    /* Sauna-Fuehler (SHT85 in der Kabine, via 2m-Kabel) */
    PKT_TYPE_SENSOR_SAUNA_TEMP = 0xBF, // float, °C
    PKT_TYPE_SENSOR_SAUNA_RH   = 0xC0, // float, %RH

    /* SD-Readback-Antwort vom RP2040 (History-Detail).
     * Payload-Layout: [2B req_id][4B total][4B offset][2B len][data] */
    PKT_TYPE_SD_READBACK_CHUNK = 0xC1,

    /* Probe-State: welcher Sauna-Fuehler laeuft, SD-Status.
     * Payload: [1B probe_type][1B sd_init_flag] */
    PKT_TYPE_PROBE_STATE = 0xC2,
};

/* Session-Steuerung ESP -> RP (als defines, damit sie ausserhalb des
 * pkt_type-Enums eindeutig bleiben und in den Hex-Kommando-Bereich
 * 0xA0-0xAF passen, der vom RP2040 konsumiert wird).                 */
#define SSC_CMD_SESSION_START    0xA4
#define SSC_CMD_SESSION_AUFGUSS  0xA5
#define SSC_CMD_SESSION_END      0xA6
#define SSC_CMD_SD_READBACK      0xA7


struct sensor_present_data
{
    float average;
    float sum;
    int per_hour_cnt;
    //time_t hour_timestamp;

    float  day_min;
    float  day_max;
    //time_t day_timestamp;
    int    per_day_cnt;
};
struct sensor_history_data
{
    struct sensor_data_average data_day[96];
    struct sensor_data_minmax data_week[7];
};

struct indicator_sensor_present_data
{
    struct sensor_present_data temp;
    struct sensor_present_data humidity;
    struct sensor_present_data co2;
    struct sensor_present_data tvoc;
    /* Extended sensors */
    struct sensor_present_data temp_ext;
    struct sensor_present_data humidity_ext;
    struct sensor_present_data pm1_0;
    struct sensor_present_data pm2_5;
    struct sensor_present_data pm10;
    struct sensor_present_data no2;
    struct sensor_present_data c2h5oh;
    struct sensor_present_data voc;
    struct sensor_present_data co;
};


struct indicator_sensor_history_data
{
    struct sensor_history_data temp;
    struct sensor_history_data humidity;
    struct sensor_history_data co2;
    struct sensor_history_data tvoc;
    /* Extended sensors */
    struct sensor_history_data temp_ext;
    struct sensor_history_data humidity_ext;
    struct sensor_history_data pm1_0;
    struct sensor_history_data pm2_5;
    struct sensor_history_data pm10;
    struct sensor_history_data no2;
    struct sensor_history_data c2h5oh;
    struct sensor_history_data voc;
    struct sensor_history_data co;
};

struct updata_queue_msg
{
    uint8_t flag; //1  day data, 2 week data
    time_t  time;
};

#define SENSOR_HISTORY_DATA_STORAGE  "sensor-data"

static const char *TAG = "sensor-model";

static SemaphoreHandle_t       __g_data_mutex;

static struct indicator_sensor_history_data  __g_sensor_history_data;
static struct indicator_sensor_present_data  __g_sensor_present_data;
static struct view_data_sensor_history_data  __g_history_event_buf;

static esp_timer_handle_t   sensor_history_data_timer_handle;

static QueueHandle_t updata_queue_handle = NULL;

static struct view_data_sensor __g_current_sensor_data = {0};

/*
 * Grove Multichannel Gas Sensor V2 - ppm(eq) ranges
 * These are QUALITATIVE/UNCALIBRATED equivalent ppm estimates.
 * See: https://wiki.seeedstudio.com/Grove-Multichannel-Gas-Sensor-V2/
 */
#define GM102B_PPM_MIN  0.05f   /* NO2: 0.05-10 ppm range */
#define GM102B_PPM_MAX  10.0f
#define GM302B_PPM_MIN  10.0f   /* C2H5OH (Ethanol): 10-500 ppm range */
#define GM302B_PPM_MAX  500.0f
#define GM502B_PPM_MIN  1.0f    /* VOC: 1-500 ppm range */
#define GM502B_PPM_MAX  500.0f
#define GM702B_PPM_MIN  1.0f    /* CO: 1-1000 ppm range */
#define GM702B_PPM_MAX  1000.0f

static void __sensor_history_data_get( struct sensor_history_data  *p_history, struct view_data_sensor_history_data *p_data)
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    
    struct sensor_data_average *p_data_day = &p_history->data_day[0];
    struct sensor_data_minmax  *p_data_week = &p_history->data_week[0];

    memcpy(p_data->data_day, (uint8_t *)p_data_day, sizeof( struct sensor_data_average) * 96);
    memcpy(p_data->data_week, (uint8_t *)p_data_week, sizeof( struct sensor_data_minmax) * 7);
    

    float min=10000;
    float max=-10000;

    for(int i =0; i < 96; i++ ) {
        struct sensor_data_average *p_item = p_data_day + i;
        if( p_item->valid ) {
            if( min > p_item->data ){
                min = p_item->data;
            }
            if( max < p_item->data ){
                max = p_item->data;
            }
        }   
    }
    p_data->day_max = max;
    p_data->day_min = min;

    min=10000;
    max=-10000;

    for(int i =0; i < 7; i++ ) {
        struct sensor_data_minmax *p_item = p_data_week + i;
        if( p_item->valid ) {
            if( min > p_item->min ){
                min = p_item->min;
            }
            if( max < p_item->max ){
                max = p_item->max;
            }
        }
    }

    if ( max < min) {
        max=4;
        min=0;
    }

    p_data->week_max = max;
    p_data->week_min = min;

    xSemaphoreGive(__g_data_mutex);
}

static void __sensor_history_data_save(void)
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    esp_err_t ret = 0;
    ret = indicator_storage_write(SENSOR_HISTORY_DATA_STORAGE, (void *)&__g_sensor_history_data, sizeof(struct indicator_sensor_history_data));
    xSemaphoreGive(__g_data_mutex);

    if( ret != ESP_OK ) {
        ESP_LOGI(TAG, "sensor history data save err:%d", ret);
    } else {
        ESP_LOGI(TAG, "sensor history data save successful");
    }   
}

static void __sensor_history_data_day_check(struct sensor_data_average p_data_day[], const char *p_sensor_name, time_t now)
{
    int history_interval = 0;
    int cur_interval = 0;

    history_interval = p_data_day[95].timestamp / HISTORY_INTERVAL_SECONDS;
    cur_interval = now / HISTORY_INTERVAL_SECONDS;

    for( int i =0;  i < 96; i++) {
        if( p_data_day[i].valid) {
            ESP_LOGI(TAG, "%s index:%d, data:%.0f, time:%d", p_sensor_name, i, p_data_day[i].data, p_data_day[i].timestamp);
        }
    }

    if( history_interval  >  cur_interval) {
        ESP_LOGI(TAG, "%s History day data pull ahead, clear data", p_sensor_name);
        memset(p_data_day, 0, sizeof(struct sensor_data_average)*96);
        return;
    }

    for(int i =0; i < 95; i++ ) {
       int interval1 = p_data_day[i].timestamp / HISTORY_INTERVAL_SECONDS;
       int interval2 = p_data_day[i+1].timestamp / HISTORY_INTERVAL_SECONDS;
       if( (interval2 - interval1) < 1 || (interval2 - interval1) > 3) {
            ESP_LOGI(TAG, "%s History day data error (gap=%d), clear data", p_sensor_name, interval2-interval1);
            memset(p_data_day, 0, sizeof(struct sensor_data_average)*96);
            return;
       }
    }

    if( history_interval == cur_interval) {
        ESP_LOGI(TAG, "%s History day data valid", p_sensor_name);
        return;
    }

    if( history_interval < ( cur_interval - 95) ) {
        ESP_LOGI(TAG, "%s History day data expired, clear data!", p_sensor_name);
        memset(p_data_day, 0, sizeof(struct sensor_data_average)*96);
    } else {

        int overlap_cnt = history_interval - ( cur_interval - 95) + 1;

        ESP_LOGI(TAG, "%s History day data  %d overlap !", p_sensor_name, overlap_cnt);

        for(int i =0; i < 96; i++ ) {
            if( i < overlap_cnt) {
                p_data_day[i].data = p_data_day[96-overlap_cnt+i].data;
                p_data_day[i].valid = p_data_day[96-overlap_cnt+i].valid;
                p_data_day[i].timestamp = p_data_day[96-overlap_cnt+i].timestamp;
            } else {
                p_data_day[i].data = 0;
                p_data_day[i].valid = false;
                p_data_day[i].timestamp = now - (95 -i) * HISTORY_INTERVAL_SECONDS;
            }
        }
    }
}
static void __sensor_history_data_week_check(struct sensor_data_minmax p_data_week[], const char *p_sensor_name, time_t now)
{
    //check history data week
    int history_day =0;
    int cur_day = 0;

    history_day =p_data_week[6].timestamp/ (3600 * 24);
    cur_day = now/ (3600 * 24);

    for( int i =0;  i < 7; i++) {
        if( p_data_week[i].valid) {
            ESP_LOGI(TAG, "%s, index:%d, min:%.0f, max:%.0f, time:%d", p_sensor_name, i, p_data_week[i].min, p_data_week[i].max , p_data_week[i].timestamp);
        }
    }

    if( history_day  >  cur_day){
        ESP_LOGI(TAG, "%s History week data pull ahead, clear data", p_sensor_name);
        memset(p_data_week, 0, sizeof(struct sensor_data_minmax)*7);
        return;
    }


    for(int i =0; i < 6; i++ ) {
       int  day1 =p_data_week[i].timestamp/ (3600 * 24);
       int  day2 =p_data_week[i+1].timestamp/ (3600 * 24);
       if( (day2-day1) != 1) {
            ESP_LOGI(TAG, "%s History week data error, clear data", p_sensor_name);
            memset(p_data_week, 0, sizeof(struct sensor_data_minmax)*7);
            return;
       }
    }

    if( history_day  == cur_day){
        ESP_LOGI(TAG, "%s History week data valid", p_sensor_name);
        return;
    }

    if( history_day < ( cur_day -6) ) {
        ESP_LOGI(TAG, "%s History week data expired, clear data!", p_sensor_name);
        memset(p_data_week, 0, sizeof(struct sensor_data_minmax)*7);

    } else {

        int overlap_cnt = history_day - ( cur_day -6)+1;

        ESP_LOGI(TAG, "%s History week data , %d overlap!", p_sensor_name, overlap_cnt);

        for(int i =0; i < 7; i++ ) {
            if( i < overlap_cnt) {
                p_data_week[i].max = p_data_week[7-overlap_cnt+i].max;
                p_data_week[i].min = p_data_week[7-overlap_cnt+i].min;
                p_data_week[i].timestamp = p_data_week[7-overlap_cnt+i].timestamp;
                p_data_week[i].valid = p_data_week[7-overlap_cnt+i].valid;
            } else {

                p_data_week[i].max = 0;
                p_data_week[i].min = 0;
                p_data_week[i].timestamp = now - (6 -i) * 3600 * 24;
                p_data_week[i].valid = false;
            }
        }
    }
}



static void __sensor_history_data_day_insert(struct sensor_data_average p_data_day[],  struct sensor_present_data  *p_cur,  time_t now)
{
    int history_interval = 0;
    int cur_interval = 0;
    struct tm timeinfo;

    localtime_r( &now, &timeinfo);
    cur_interval = (timeinfo.tm_hour * 60 + timeinfo.tm_min) / 15;

    localtime_r( &(p_data_day[95].timestamp), &timeinfo);
    history_interval = (timeinfo.tm_hour * 60 + timeinfo.tm_min) / 15;

    if( cur_interval == history_interval) {
        return;
    }

    for( int i =0;  i < 95; i++) {
        p_data_day[i].data = p_data_day[i+1].data;
        p_data_day[i].valid = p_data_day[i+1].valid;
        p_data_day[i].timestamp = p_data_day[i+1].timestamp;

        if( !p_data_day[i].valid) {
            p_data_day[i].timestamp = now - (95 -i) * HISTORY_INTERVAL_SECONDS;
        }
    }
    if( p_cur->per_hour_cnt >=1) {
        p_data_day[95].valid = true;
        p_data_day[95].data = p_cur->average;

        p_cur->per_hour_cnt = 0;
        p_cur->sum = 0.0;

    }  else {
        p_data_day[95].valid = false;
    }
    p_data_day[95].timestamp = now;

#if SENSOR_HISTORY_DATA_DEBUG
    for( int i =0;  i < 96; i++) {
        if( p_data_day[i].valid) {
            ESP_LOGI(TAG, "index:%d, data:%.0f, time:%d", i, p_data_day[i].data, p_data_day[i].timestamp);
        }
    }
#endif

}

static void __sensor_history_data_week_insert(struct sensor_data_minmax p_data_week[],  struct sensor_present_data  *p_cur, time_t now)
{
    int history_mday =0;
    int cur_mday = 0;
    struct tm timeinfo;

    localtime_r( &now, &timeinfo);
    cur_mday = timeinfo.tm_mday;
    
    localtime_r( &(p_data_week[6].timestamp), &timeinfo);
    history_mday= timeinfo.tm_mday;

    if( history_mday ==  cur_mday) {
        return;
    }

    for( int i =0;  i < 6; i++) {
        p_data_week[i].max = p_data_week[i+1].max;
        p_data_week[i].min = p_data_week[i+1].min;
        p_data_week[i].timestamp = p_data_week[i+1].timestamp;
        p_data_week[i].valid = p_data_week[i+1].valid;

        if( !p_data_week[i].valid) {
            p_data_week[i].timestamp = now - (6 -i) * 3600 * 24;
        }
    }
    if( p_cur->per_day_cnt >=1) {
        p_data_week[6].valid = true;
        p_data_week[6].max = p_cur->day_max;
        p_data_week[6].min = p_cur->day_min;

        //clear present data
        p_cur->day_max = 0.0;
        p_cur->day_min = 0.0;
        p_cur->per_day_cnt = 0;

    }  else {
        p_data_week[6].valid = false;
    }
    p_data_week[6].timestamp = now;

#if SENSOR_HISTORY_DATA_DEBUG
    for( int i =0;  i < 7; i++) {
        if( p_data_week[i].valid) {
            ESP_LOGI(TAG, "index:%d, min:%.0f, max:%.0f, time:%d", i, p_data_week[i].min, p_data_week[i].max , p_data_week[i].timestamp);
        }
    }
#endif
}


static void __sensor_history_data_check(time_t now)
{
    static bool  check_flag = false;
    if(check_flag) {
        return;
    }
    check_flag =  true;

    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    __sensor_history_data_day_check(__g_sensor_history_data.temp.data_day,   "Temp", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.temp.data_week, "Temp",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.humidity.data_day,   "Humidity", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.humidity.data_week, "Humidity",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.co2.data_day,   "CO2", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.co2.data_week, "CO2",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.tvoc.data_day,   "TVOC", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.tvoc.data_week, "TVOC",  now - 3600 * 24);

    /* Extended sensors */
    __sensor_history_data_day_check(__g_sensor_history_data.temp_ext.data_day,   "TempExt", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.temp_ext.data_week, "TempExt",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.humidity_ext.data_day,   "HumExt", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.humidity_ext.data_week, "HumExt",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.pm1_0.data_day,   "PM1.0", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.pm1_0.data_week, "PM1.0",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.pm2_5.data_day,   "PM2.5", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.pm2_5.data_week, "PM2.5",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.pm10.data_day,   "PM10", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.pm10.data_week, "PM10",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.no2.data_day,   "NO2", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.no2.data_week, "NO2",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.c2h5oh.data_day,   "C2H5OH", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.c2h5oh.data_week, "C2H5OH",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.voc.data_day,   "VOC", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.voc.data_week, "VOC",  now - 3600 * 24);

    __sensor_history_data_day_check(__g_sensor_history_data.co.data_day,   "CO", now - HISTORY_INTERVAL_SECONDS);
    __sensor_history_data_week_check(__g_sensor_history_data.co.data_week, "CO",  now - 3600 * 24);

    xSemaphoreGive(__g_data_mutex);
}

static void __sensor_history_data_day_update(time_t now)
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);

    __sensor_history_data_day_insert( __g_sensor_history_data.temp.data_day, &__g_sensor_present_data.temp, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.humidity.data_day, &__g_sensor_present_data.humidity, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.co2.data_day, &__g_sensor_present_data.co2, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.tvoc.data_day, &__g_sensor_present_data.tvoc, now);
    /* Extended sensors */
    __sensor_history_data_day_insert( __g_sensor_history_data.temp_ext.data_day, &__g_sensor_present_data.temp_ext, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.humidity_ext.data_day, &__g_sensor_present_data.humidity_ext, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.pm1_0.data_day, &__g_sensor_present_data.pm1_0, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.pm2_5.data_day, &__g_sensor_present_data.pm2_5, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.pm10.data_day, &__g_sensor_present_data.pm10, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.no2.data_day, &__g_sensor_present_data.no2, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.c2h5oh.data_day, &__g_sensor_present_data.c2h5oh, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.voc.data_day, &__g_sensor_present_data.voc, now);
    __sensor_history_data_day_insert( __g_sensor_history_data.co.data_day, &__g_sensor_present_data.co, now);

    xSemaphoreGive(__g_data_mutex);

    __sensor_history_data_save();
}

static void __sensor_history_data_week_update(time_t now)
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);

    __sensor_history_data_week_insert(__g_sensor_history_data.temp.data_week, &__g_sensor_present_data.temp, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.humidity.data_week, &__g_sensor_present_data.humidity, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.co2.data_week, &__g_sensor_present_data.co2, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.tvoc.data_week, &__g_sensor_present_data.tvoc, now);
    /* Extended sensors */
    __sensor_history_data_week_insert(__g_sensor_history_data.temp_ext.data_week, &__g_sensor_present_data.temp_ext, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.humidity_ext.data_week, &__g_sensor_present_data.humidity_ext, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.pm1_0.data_week, &__g_sensor_present_data.pm1_0, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.pm2_5.data_week, &__g_sensor_present_data.pm2_5, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.pm10.data_week, &__g_sensor_present_data.pm10, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.no2.data_week, &__g_sensor_present_data.no2, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.c2h5oh.data_week, &__g_sensor_present_data.c2h5oh, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.voc.data_week, &__g_sensor_present_data.voc, now);
    __sensor_history_data_week_insert(__g_sensor_history_data.co.data_week, &__g_sensor_present_data.co, now);

    xSemaphoreGive(__g_data_mutex);

    __sensor_history_data_save();
}


static void __sensor_history_data_timer_callback(void* arg)
{
    struct updata_queue_msg msg = {
        .flag = 3,
        .time = 0,
    };
    xQueueSendFromISR(updata_queue_handle, &msg, NULL);
}

static void __sensor_history_data_update_check(void)
{
    static int last_interval = -1;
    static int last_day  = 32;
    static time_t  last_timestamp1 = 0;
    static time_t  last_timestamp2 = 0;
    time_t now = 0;

    now = time(NULL);

    struct tm timeinfo = { 0 };
    localtime_r( &now, &timeinfo);

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);

    int cur_interval = (timeinfo.tm_hour * 60 + timeinfo.tm_min) / 15;
    int cur_day  = timeinfo.tm_mday;

    ESP_LOGI(TAG, "__sensor_history_data_update_check: %s (interval=%d)", strftime_buf, cur_interval);

    if( timeinfo.tm_year  < 120) {
        ESP_LOGI(TAG, "The time is not right!!!");
        return;
    }

    __sensor_history_data_check( now);

    if( cur_interval != last_interval  &&  ((now - last_timestamp1) >= HISTORY_INTERVAL_SECONDS) ) {
        last_interval = cur_interval;

        if( last_timestamp1 == 0) {
            last_timestamp1 = ((now - HISTORY_INTERVAL_SECONDS) / HISTORY_INTERVAL_SECONDS) * HISTORY_INTERVAL_SECONDS;
        }

        ESP_LOGI(TAG, "Storing sensor data (15-min interval)");
        __sensor_history_data_day_update(last_timestamp1);

        last_timestamp1 = (now / HISTORY_INTERVAL_SECONDS) * HISTORY_INTERVAL_SECONDS;
    }

    if( cur_day != last_day  &&  ((now - last_timestamp2) > (3600*24))) {
        last_day = cur_day;
        if( last_timestamp2 == 0) {
            last_timestamp2 = ((now - 3600 * 24) / (3600 * 24)) * (3600 * 24);
        }

        __sensor_history_data_week_update(last_timestamp2);

        last_timestamp2 = ((now) / (3600 * 24)) * (3600 * 24); //Sample at the day
    }
}

static void __sensor_history_data_update_init(void)
{
    const esp_timer_create_args_t timer_args = {
            .callback = &__sensor_history_data_timer_callback,
            .arg = NULL,
            .name = "sensor data update"
    };
    ESP_ERROR_CHECK( esp_timer_create(&timer_args, &sensor_history_data_timer_handle));
    ESP_ERROR_CHECK(esp_timer_start_periodic(sensor_history_data_timer_handle, 1000000 * 10));
}

static void sensor_history_data_updata_task(void *arg)
{
    struct updata_queue_msg msg = {};
    while(1) {
        if(xQueueReceive(updata_queue_handle, &msg, portMAX_DELAY)) {
            if( msg.flag == 1) {
                ESP_LOGI(TAG, "update day history data");
                __sensor_history_data_day_update(msg.time);
            } else if( msg.flag == 2) {
                ESP_LOGI(TAG, "update week history data");
                __sensor_history_data_week_update(msg.time);
            } else if( msg.flag == 3) {
                __sensor_history_data_update_check();
            }
        }
    }
}

static void __sensor_history_data_restore(void)
{
    esp_err_t ret = 0;

    size_t len = sizeof(__g_sensor_history_data);
    
    ret = indicator_storage_read(SENSOR_HISTORY_DATA_STORAGE, (void *)&__g_sensor_history_data, &len);
    if( ret == ESP_OK  && len== (sizeof(__g_sensor_history_data)) ) {
        ESP_LOGI(TAG, "sensor history data read successful");
    } else {
        // err or not find
        if( ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "sensor history data not find");
        } else {
            ESP_LOGI(TAG, "sensor history data read err:%d", ret);
        }
        memset(&__g_sensor_history_data, 0 ,sizeof(__g_sensor_history_data));
    }
}


static void __sensor_present_data_update(struct sensor_present_data *p_data, float vaule)
{
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    p_data->per_hour_cnt++;
    p_data->sum += vaule;
    p_data->average = p_data->sum / p_data->per_hour_cnt;


    p_data->per_day_cnt++;
    if( p_data->per_day_cnt != 1) {
        if( p_data->day_min > vaule) {
            p_data->day_min = vaule;
        }
        if( p_data->day_max < vaule) {
            p_data->day_max = vaule;
        }
    } else {
        p_data->day_min = vaule;
        p_data->day_max = vaule;
    }
    xSemaphoreGive(__g_data_mutex);
}

/*
 * Calculate ppm(eq) from raw sensor value for MultiGas sensors.
 *
 * The Grove Multichannel Gas Sensor V2 GM-x02B sensors output voltage
 * proportional to gas concentration. This function converts to ppm(eq).
 *
 * @param raw_value   Raw float from sensor (voltage 0-3.3V or ADC 0-1023)
 * @param ppm_min     Minimum detection range in ppm
 * @param ppm_max     Maximum detection range in ppm
 * @return ppm_eq     Equivalent ppm estimate (qualitative, not calibrated)
 */
/* Baseline voltage threshold - voltages below this are considered "clean air" (0 ppm)
 * The Grove Multichannel Gas Sensor V2 outputs relatively high voltages at clean air.
 * Typical clean air baseline is around 2.0-2.5V depending on sensor warm-up. */
#define GAS_SENSOR_BASELINE_VOLTAGE  2.2f
#define GAS_SENSOR_MAX_VOLTAGE       3.3f

static float __calculate_multigas_ppm(float raw_value, float ppm_min, float ppm_max)
{
    float voltage;

    /* Determine if input is voltage (0-3.3V) or ADC (>3.3 means ADC 0-1023) */
    if (raw_value > 3.3f) {
        /* ADC value - convert to voltage */
        int adc = (int)(raw_value + 0.5f);
        if (adc < 0) adc = 0;
        if (adc > 1023) adc = 1023;
        voltage = (adc * 3.3f) / 1023.0f;
    } else {
        /* Already voltage */
        voltage = raw_value;
        if (voltage < 0.0f) voltage = 0.0f;
    }

    /* Below baseline voltage = clean air, return 0 */
    if (voltage <= GAS_SENSOR_BASELINE_VOLTAGE) {
        return 0.0f;
    }

    /* Calculate ratio above baseline (0.0 to 1.0)
     * baseline_voltage maps to 0, max_voltage maps to 1 */
    float effective_range = GAS_SENSOR_MAX_VOLTAGE - GAS_SENSOR_BASELINE_VOLTAGE;
    float ratio = (voltage - GAS_SENSOR_BASELINE_VOLTAGE) / effective_range;

    if (ratio > 1.0f) ratio = 1.0f;

    /* Use cubic function for very gradual increase at low concentrations
     * ratio^3 gives much lower values at slightly elevated voltages */
    ratio = ratio * ratio * ratio;

    /* Linear interpolation with the cubed ratio */
    float ppm_eq = ppm_min + ratio * (ppm_max - ppm_min);

    /* Clamp to valid range */
    if (ppm_eq < 0.0f) ppm_eq = 0.0f;
    if (ppm_eq > ppm_max) ppm_eq = ppm_max;

    return ppm_eq;
}

static int __data_parse_handle(uint8_t *p_data, ssize_t len)
{
    uint8_t pkt_type = p_data[0];
    switch ( pkt_type)
    {
        case PKT_TYPE_SENSOR_SCD41_CO2: {
            struct view_data_sensor_data data;
            if( len < (sizeof(data.vaule) +1)) {
                break;
            }
            data.sensor_type = SENSOR_DATA_CO2;
            memcpy(&data.vaule, &p_data[1], sizeof(data.vaule));
            __sensor_present_data_update(&__g_sensor_present_data.co2, data.vaule);
            __g_current_sensor_data.co2 = data.vaule;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA, \
                           &data, sizeof(struct view_data_sensor_data ), portMAX_DELAY);
            break;
        } 
        
        case PKT_TYPE_SENSOR_SHT41_TEMP: {
            struct view_data_sensor_data data;
            if( len < (sizeof(data.vaule) +1)) {
                break;
            }
            data.sensor_type = SENSOR_DATA_TEMP;
            memcpy(&data.vaule, &p_data[1], sizeof(data.vaule));
            __sensor_present_data_update(&__g_sensor_present_data.temp, data.vaule);
            __g_current_sensor_data.temp_internal = data.vaule;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA, \
                           &data, sizeof(struct view_data_sensor_data ), portMAX_DELAY);
            break;
        } 

        case PKT_TYPE_SENSOR_SHT41_HUMIDITY: {
            struct view_data_sensor_data data;
            if( len < (sizeof(data.vaule) +1)) {
                break;
            }
            data.sensor_type = SENSOR_DATA_HUMIDITY;
            memcpy(&data.vaule, &p_data[1], sizeof(data.vaule));
            __sensor_present_data_update(&__g_sensor_present_data.humidity, data.vaule);
            __g_current_sensor_data.humidity_internal = data.vaule;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA, \
                           &data, sizeof(struct view_data_sensor_data ), portMAX_DELAY);
            break;
        } 

        case PKT_TYPE_SENSOR_TVOC_INDEX: {
            struct view_data_sensor_data data;
            if( len < (sizeof(data.vaule) +1)) {
                break;
            }
            data.sensor_type = SENSOR_DATA_TVOC;
            memcpy(&data.vaule, &p_data[1], sizeof(data.vaule));
            __sensor_present_data_update(&__g_sensor_present_data.tvoc, data.vaule);
            __g_current_sensor_data.tvoc = data.vaule;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA, \
                           &data, sizeof(struct view_data_sensor_data ), portMAX_DELAY);
            break;
        }

        case PKT_TYPE_SENSOR_PM1_0: {
            if (len < (sizeof(float) + 1)) break;
            float value;
            memcpy(&value, &p_data[1], sizeof(value));
            if (!FLOAT_IS_VALID(value)) {
                ESP_LOGW(TAG, "PM1.0: invalid float, dropping");
                break;
            }
            ESP_LOGI(TAG, "PM1.0: %.1f ug/m3", value);
            __sensor_present_data_update(&__g_sensor_present_data.pm1_0, value);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.pm1_0 = value;
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_PM2_5: {
            if (len < (sizeof(float) + 1)) break;
            float value;
            memcpy(&value, &p_data[1], sizeof(value));
            if (!FLOAT_IS_VALID(value)) {
                ESP_LOGW(TAG, "PM2.5: invalid float, dropping");
                break;
            }
            ESP_LOGI(TAG, "PM2.5: %.1f ug/m3", value);
            __sensor_present_data_update(&__g_sensor_present_data.pm2_5, value);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.pm2_5 = value;
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_PM10: {
            if (len < (sizeof(float) + 1)) break;
            float value;
            memcpy(&value, &p_data[1], sizeof(value));
            if (!FLOAT_IS_VALID(value)) {
                ESP_LOGW(TAG, "PM10: invalid float, dropping");
                break;
            }
            ESP_LOGI(TAG, "PM10: %.1f ug/m3", value);
            __sensor_present_data_update(&__g_sensor_present_data.pm10, value);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.pm10 = value;
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_GM102B_NO2: {
            if (len < (sizeof(float) + 1)) break;
            float raw_value;
            memcpy(&raw_value, &p_data[1], sizeof(raw_value));
            if (!FLOAT_IS_VALID(raw_value)) {
                ESP_LOGW(TAG, "NO2: invalid float, dropping");
                break;
            }
            float ppm_eq = __calculate_multigas_ppm(raw_value, GM102B_PPM_MIN, GM102B_PPM_MAX);
            __sensor_present_data_update(&__g_sensor_present_data.no2, ppm_eq);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.multigas_gm102b[0] = ppm_eq;
            __g_current_sensor_data.multigas_gm102b[1] = raw_value;
            ESP_LOGI(TAG, "NO2: ppm(eq)=%.2f (raw=%.2f)", ppm_eq, raw_value);
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_GM302B_C2H5OH: {
            if (len < (sizeof(float) + 1)) break;
            float raw_value;
            memcpy(&raw_value, &p_data[1], sizeof(raw_value));
            if (!FLOAT_IS_VALID(raw_value)) {
                ESP_LOGW(TAG, "C2H5OH: invalid float, dropping");
                break;
            }
            float ppm_eq = __calculate_multigas_ppm(raw_value, GM302B_PPM_MIN, GM302B_PPM_MAX);
            __sensor_present_data_update(&__g_sensor_present_data.c2h5oh, ppm_eq);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.multigas_gm302b[0] = ppm_eq;
            __g_current_sensor_data.multigas_gm302b[1] = raw_value;
            ESP_LOGI(TAG, "C2H5OH: ppm(eq)=%.1f (raw=%.2f)", ppm_eq, raw_value);
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_GM502B_VOC: {
            if (len < (sizeof(float) + 1)) break;
            float raw_value;
            memcpy(&raw_value, &p_data[1], sizeof(raw_value));
            if (!FLOAT_IS_VALID(raw_value)) {
                ESP_LOGW(TAG, "VOC: invalid float, dropping");
                break;
            }
            float ppm_eq = __calculate_multigas_ppm(raw_value, GM502B_PPM_MIN, GM502B_PPM_MAX);
            __sensor_present_data_update(&__g_sensor_present_data.voc, ppm_eq);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.multigas_gm502b[0] = ppm_eq;
            __g_current_sensor_data.multigas_gm502b[1] = raw_value;
            ESP_LOGI(TAG, "VOC: ppm(eq)=%.1f (raw=%.2f)", ppm_eq, raw_value);
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_GM702B_CO: {
            if (len < (sizeof(float) + 1)) break;
            float raw_value;
            memcpy(&raw_value, &p_data[1], sizeof(raw_value));
            if (!FLOAT_IS_VALID(raw_value)) {
                ESP_LOGW(TAG, "CO: invalid float, dropping");
                break;
            }
            float ppm_eq = __calculate_multigas_ppm(raw_value, GM702B_PPM_MIN, GM702B_PPM_MAX);
            __sensor_present_data_update(&__g_sensor_present_data.co, ppm_eq);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.multigas_gm702b[0] = ppm_eq;
            __g_current_sensor_data.multigas_gm702b[1] = raw_value;
            ESP_LOGI(TAG, "CO: ppm(eq)=%.1f (raw=%.2f)", ppm_eq, raw_value);
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_TEMP_EXTERNAL: {
            if (len < (sizeof(float) + 1)) break;
            float value;
            memcpy(&value, &p_data[1], sizeof(value));
            if (!FLOAT_IS_VALID(value)) {
                ESP_LOGW(TAG, "Temp external: invalid float, dropping");
                break;
            }
            ESP_LOGI(TAG, "Temp external: %.1f C", value);
            __sensor_present_data_update(&__g_sensor_present_data.temp_ext, value);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.temp_external = value;
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_HUMIDITY_EXTERNAL: {
            if (len < (sizeof(float) + 1)) break;
            float value;
            memcpy(&value, &p_data[1], sizeof(value));
            if (!FLOAT_IS_VALID(value)) {
                ESP_LOGW(TAG, "Humidity external: invalid float, dropping");
                break;
            }
            ESP_LOGI(TAG, "Humidity external: %.1f %%", value);
            __sensor_present_data_update(&__g_sensor_present_data.humidity_ext, value);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.humidity_external = value;
            xSemaphoreGive(__g_data_mutex);
            break;
        }

        case PKT_TYPE_SENSOR_SAUNA_TEMP: {
            if (len < (sizeof(float) + 1)) break;
            float value;
            memcpy(&value, &p_data[1], sizeof(value));
            if (!FLOAT_IS_VALID(value)) {
                ESP_LOGW(TAG, "Sauna temp: invalid float, dropping");
                break;
            }
            /* Sanity-check: Sauna kann von -5 (Winter-Kabine kalt) bis
             * ca. 120 C gehen. Aliese Werte deuten auf Fuehlerbruch
             * oder Kabelfehler - wir verwerfen sie still, damit sie
             * keine Alarm- oder Peak-Logik verderben.                */
            if (value < -10.0f || value > 130.0f) {
                ESP_LOGW(TAG, "Sauna temp out of plausible range: %.1f", value);
                break;
            }
            ESP_LOGI(TAG, "Sauna temp: %.1f C", value);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.sauna_temp = value;
            xSemaphoreGive(__g_data_mutex);
            /* Event an die View/Session: diese sensor_type-Variante
             * wird vom Session-Modul abonniert und in den Live-Chart
             * eingespeist. */
            struct view_data_sensor_data evt = {
                .sensor_type = SENSOR_DATA_SAUNA_TEMP,
                .vaule       = value,
            };
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                              VIEW_EVENT_SENSOR_DATA, &evt, sizeof(evt),
                              portMAX_DELAY);
            break;
        }

        case PKT_TYPE_SENSOR_SAUNA_RH: {
            if (len < (sizeof(float) + 1)) break;
            float value;
            memcpy(&value, &p_data[1], sizeof(value));
            if (!FLOAT_IS_VALID(value)) {
                ESP_LOGW(TAG, "Sauna RH: invalid float, dropping");
                break;
            }
            if (value < 0.0f || value > 101.0f) {
                ESP_LOGW(TAG, "Sauna RH out of range: %.1f", value);
                break;
            }
            if (value > 100.0f) value = 100.0f;
            ESP_LOGI(TAG, "Sauna RH: %.1f %%", value);
            xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
            __g_current_sensor_data.sauna_rh = value;
            xSemaphoreGive(__g_data_mutex);
            struct view_data_sensor_data evt = {
                .sensor_type = SENSOR_DATA_SAUNA_RH,
                .vaule       = value,
            };
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                              VIEW_EVENT_SENSOR_DATA, &evt, sizeof(evt),
                              portMAX_DELAY);
            break;
        }

        case PKT_TYPE_SD_READBACK_CHUNK: {
            /* Handover zur indicator_session-Schicht. Die bekommt den
             * ganzen Payload-Block (ohne Typbyte) und matched die
             * req_id gegen ausstehende History-Detail-Requests.      */
            extern void indicator_session_rx_sd_chunk(const uint8_t *payload, size_t n);
            indicator_session_rx_sd_chunk(&p_data[1], len - 1);
            break;
        }

        case PKT_TYPE_PROBE_STATE: {
            if (len < 3) break;
            struct view_data_probe_state ps = {
                .probe   = p_data[1],
                .sd_init = p_data[2],
            };
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                              VIEW_EVENT_PROBE_STATE, &ps, sizeof(ps),
                              portMAX_DELAY);
            break;
        }

        default:
            break;
    }

}

static int __cmd_send(uint8_t cmd, void *p_data, uint8_t len);  /* fwd decl */

int indicator_sensor_rp2040_cmd(uint8_t cmd, const void *data, uint8_t len)
{
    return __cmd_send(cmd, (void *)data, len);
}

static int __cmd_send(uint8_t cmd, void *p_data, uint8_t len)
{
    /* 80 statt 32 damit sauna-commands mit groesserem payload durchkommen:
     *   AUFGUSS   = 48 B name (payload)
     *   SD_READBACK = 32 B (req_id + session_id + offset + len)
     * Das alte limit 31 hat AUFGUSS + SD_READBACK kommentarlos verworfen
     * -> keine aufguss-marker in der CSV, kein detail-chart im replay.
     * COBS-overhead max ceil(N/254)+1 ≈ 2 B bei 64 B -> 80 reicht.       */
    uint8_t buf[80] = {0};
    uint8_t data[80] = {0};

    if (len > 78) {
        return -1;
    }
    
    uint8_t index =1;

    data[0] = cmd;

    if( len > 0 && p_data != NULL) {
        memcpy(&data[1], p_data, len);
        index += len;
    }
    cobs_encode_result ret = cobs_encode(buf, sizeof(buf),  data, index);
#if 1//SENSOR_COMM_DEBUG
    ESP_LOGI(TAG, "encode status:%d, len:%d",  ret.status,  ret.out_len);
    for(int i=0; i < ret.out_len; i++ ) {
        printf( "0x%x ", buf[i] );
    }
    printf("\r\n");
#endif

    if( ret.status == COBS_ENCODE_OK ) {
        return uart_write_bytes(ESP32_COMM_PORT_NUM,  buf, ret.out_len+1);
    }
    return -1;
}

static void esp32_rp2040_comm_task(void *arg)
{
    uart_config_t uart_config = {
        .baud_rate = ESP32_COMM_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

    ESP_ERROR_CHECK(uart_driver_install(ESP32_COMM_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ESP32_COMM_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ESP32_COMM_PORT_NUM, ESP32_RP2040_TXD, ESP32_RP2040_RXD, ESP32_RP2040_RTS, ESP32_RP2040_CTS));

    __cmd_send(PKT_TYPE_CMD_POWER_ON, NULL, 0);
    cobs_decode_result ret;

    /* Akkumulator fuer packet-reassembly:
     * Der urspruengliche parser ging davon aus dass jedes COBS-packet
     * komplett in einem uart_read_bytes-aufruf ankommt. Bei packets
     * > UART-FIFO-groesse (typ. 128 B) wurde das packet zerrissen und
     * verworfen - genau das hat SD_READBACK-chunks > 64 B verschluckt
     * und den history-detail-chart auf 6 sek pro session-ladung gebremst.
     *
     * Jetzt: byte-weise akkumulieren bis 0x00-terminator, dann decoden.
     * Safe fuer beliebige packet-groessen bis BUF_SIZE, egal in wie viele
     * reads sie aufgeteilt kommen. Overflow → drop, next 0x00 re-sync.  */
    static uint8_t accum[BUF_SIZE];
    static size_t  accum_len = 0;

    while (1) {
        int n = uart_read_bytes(ESP32_COMM_PORT_NUM, buf, (BUF_SIZE - 1),
                                1 / portTICK_PERIOD_MS);
        if (n <= 0) continue;

#if SENSOR_COMM_DEBUG
        printf("recv:  ");
        for (int i = 0; i < n; i++) printf("0x%x ", buf[i]);
        printf("\r\n");
#endif

        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (b == 0x00) {
                /* packet-ende */
                if (accum_len > 0) {
                    memset(data, 0, sizeof(data));
                    ret = cobs_decode(data, sizeof(data), accum, accum_len);
#if SENSOR_COMM_DEBUG
                    ESP_LOGI(TAG, "decode status:%d, len:%d, type:0x%x",
                             ret.status, ret.out_len, data[0]);
                    printf("decode: ");
                    for (int j = 0; j < ret.out_len; j++) printf("0x%x ", data[j]);
                    printf("\r\n");
#endif
                    if (ret.out_len > 1 && ret.status == COBS_DECODE_OK) {
                        __data_parse_handle((uint8_t *)data, ret.out_len);
                    }
                }
                accum_len = 0;
            } else {
                if (accum_len < sizeof(accum)) {
                    accum[accum_len++] = b;
                } else {
                    /* overflow → drop und resync am naechsten 0x00 */
                    accum_len = 0;
                }
            }
        }
    }
}

static void __sensor_shutdown(void)
{
    int ret = 0;
    ret = __cmd_send(PKT_TYPE_CMD_SHUTDOWN, NULL, 0);
    if( ret <= 0) {
        ESP_LOGI(TAG, "sensor shutdown fail!. %d", ret);
    }
}

static void __view_event_handler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    int i;
    switch (id)
    {
        case VIEW_EVENT_SENSOR_TEMP_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_TEMP_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.temp,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_TEMP;
            __g_history_event_buf.resolution  = 1;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }

        case VIEW_EVENT_SENSOR_HUMIDITY_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_HUMIDITY_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.humidity,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_HUMIDITY;
            __g_history_event_buf.resolution  = 0;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_CO2_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_CO2_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.co2,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_CO2;
            __g_history_event_buf.resolution  = 0;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_TVOC_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_TVOC_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.tvoc,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_TVOC;
            __g_history_event_buf.resolution  = 0;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }

        /* Extended sensor history events */
        case VIEW_EVENT_SENSOR_TEMP_EXT_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_TEMP_EXT_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.temp_ext,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_TEMP_EXT;
            __g_history_event_buf.resolution  = 1;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_HUMIDITY_EXT_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_HUMIDITY_EXT_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.humidity_ext,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_HUMIDITY_EXT;
            __g_history_event_buf.resolution  = 0;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_PM1_0_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_PM1_0_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.pm1_0,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_PM1_0;
            __g_history_event_buf.resolution  = 1;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_PM2_5_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_PM2_5_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.pm2_5,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_PM2_5;
            __g_history_event_buf.resolution  = 1;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_PM10_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_PM10_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.pm10,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_PM10;
            __g_history_event_buf.resolution  = 1;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_NO2_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_NO2_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.no2,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_NO2;
            __g_history_event_buf.resolution  = 2;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_C2H5OH_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_C2H5OH_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.c2h5oh,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_C2H5OH;
            __g_history_event_buf.resolution  = 2;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_VOC_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_VOC_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.voc,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_VOC;
            __g_history_event_buf.resolution  = 2;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_CO_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_CO_HISTORY");
            __sensor_history_data_get( &__g_sensor_history_data.co,  &__g_history_event_buf);
            __g_history_event_buf.sensor_type = SENSOR_DATA_CO;
            __g_history_event_buf.resolution  = 2;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }

        case VIEW_EVENT_SHUTDOWN: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SHUTDOWN");
            __sensor_shutdown();
            break;
        }
//debug ui 
#if 0
        case VIEW_EVENT_SENSOR_TEMP_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_TEMP_HISTORY");
            __g_history_event_buf.sensor_type = SENSOR_DATA_TEMP;
            __g_history_event_buf.resolution = 1;

            //test
            float min=100;
            float max=0;
            
            time_t now = 0;
            time(&now);
            time_t time1 = (time_t)(now / 3600) * 3600;
            time_t time2 = (time_t)(now / 3600 / 24) * 3600 * 24;
            
            for(i = 0; i < 24; i++) { 
                data.data_day[i].data = (float)lv_rand(10, 40);
                data.data_day[i].timestamp = time1 + i *3600;
                data.data_day[i].valid = true;

                if( min > data.data_day[i].data) {
                    min = data.data_day[i].data;
                }
                if( max < data.data_day[i].data) {
                    max = data.data_day[i].data;
                }
            }
            data.day_max = max;
            data.day_min = min;

            min=100;
            max=0;

            for(i = 0; i < 7; i++) { 
                data.data_week[i].min = (float)lv_rand(10, 20);
                data.data_week[i].max = (float)lv_rand(20, 40);
                data.data_week[i].timestamp =  time2 + i * 3600 * 24;;
                data.data_week[i].valid = true;
                
                if( min > data.data_week[i].min) {
                    min = data.data_week[i].min;
                }
                if( max < data.data_week[i].max) {
                    max = data.data_week[i].max;
                }
            }
            data.week_max = max;
            data.week_min = min;

            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_HUMIDITY_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_HUMIDITY_HISTORY");
            __g_history_event_buf.sensor_type = SENSOR_DATA_HUMIDITY;
            __g_history_event_buf.resolution = 0;

            //test
            float min=100;
            float max=0;

            time_t now = 0;
            time(&now);
            time_t time1 = (time_t)(now / 3600) * 3600;
            time_t time2 = (time_t)(now / 3600 / 24) * 3600 * 24;
            
            for(i = 0; i < 24; i++) { 
                data.data_day[i].data = (float)lv_rand(30, 50);
                data.data_day[i].timestamp = time1 + i *3600;
                data.data_day[i].valid = true;
                if( min > data.data_day[i].data) {
                    min = data.data_day[i].data;
                }
                if( max < data.data_day[i].data) {
                    max = data.data_day[i].data;
                }
            }
            data.day_max = max;
            data.day_min = min;

            min=100;
            max=0;
            for(i = 0; i < 7; i++) { 
                data.data_week[i].max = (float)lv_rand(40, 50);
                data.data_week[i].min = (float)lv_rand(30, 40);
                data.data_week[i].timestamp =  time2 + i * 3600 * 24;;
                data.data_week[i].valid = true;
                if( min > data.data_week[i].min) {
                    min = data.data_week[i].min;
                }
                if( max < data.data_week[i].max) {
                    max = data.data_week[i].max;
                }
            }
            data.week_max = max;
            data.week_min = min;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_TVOC_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_TVOC_HISTORY");
            __g_history_event_buf.sensor_type = SENSOR_DATA_TVOC;
            __g_history_event_buf.resolution = 0;

            //test
            float min=120;
            float max=0;

            time_t now = 0;
            time(&now);
            time_t time1 = (time_t)(now / 3600) * 3600;
            time_t time2 = (time_t)(now / 3600 / 24) * 3600 * 24;
            
            for(i = 0; i < 24; i++) { 
                data.data_day[i].data = (float)lv_rand(100, 120);
                data.data_day[i].timestamp = time1 + i *3600;
                data.data_day[i].valid = true;
                if( min > data.data_day[i].data) {
                    min = data.data_day[i].data;
                }
                if( max < data.data_day[i].data) {
                    max = data.data_day[i].data;
                }
            }
            data.day_max = max;
            data.day_min = min;

            min=120;
            max=0;
            for(i = 0; i < 7; i++) { 
                data.data_week[i].max = (float)lv_rand(100, 120);
                data.data_week[i].min = (float)lv_rand(0, 100);
                data.data_week[i].timestamp =  time2 + i * 3600 * 24;;
                data.data_week[i].valid = true;
                if( min > data.data_week[i].min) {
                    min = data.data_week[i].min;
                }
                if( max < data.data_week[i].max) {
                    max = data.data_week[i].max;
                }
            }
            data.week_max = max;
            data.week_min = min;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
        case VIEW_EVENT_SENSOR_CO2_HISTORY: {
            ESP_LOGI(TAG, "event: VIEW_EVENT_SENSOR_CO2_HISTORY");
            __g_history_event_buf.sensor_type = SENSOR_DATA_CO2;
            __g_history_event_buf.resolution = 0;

            //test
            float min=700;
            float max=0;

            time_t now = 0;
            time(&now);
            time_t time1 = (time_t)(now / 3600) * 3600;
            time_t time2 = (time_t)(now / 3600 / 24) * 3600 * 24;
            
            for(i = 0; i < 24; i++) { 
                data.data_day[i].data = (float)lv_rand(500, 600);
                data.data_day[i].timestamp = time1 + i *3600;
                data.data_day[i].valid = true;
                if( min > data.data_day[i].data) {
                    min = data.data_day[i].data;
                }
                if( max < data.data_day[i].data) {
                    max = data.data_day[i].data;
                }
            }
            data.day_max = max;
            data.day_min = min;

            min=700;
            max=0;
            for(i = 0; i < 7; i++) { 
                data.data_week[i].min = (float)lv_rand(500, 600);
                data.data_week[i].max = (float)lv_rand(600, 700);
                data.data_week[i].timestamp =  time2 + i * 3600 * 24;;
                data.data_week[i].valid = true;
                if( min > data.data_week[i].min) {
                    min = data.data_week[i].min;
                }
                if( max < data.data_week[i].max) {
                    max = data.data_week[i].max;
                }
            }
            data.week_max = max;
            data.week_min = min;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_DATA_HISTORY, &__g_history_event_buf, sizeof(struct view_data_sensor_history_data ), portMAX_DELAY);
            break;
        }
#endif

    default:
        break;
    }
}

int indicator_sensor_init(void)
{
    __g_data_mutex  =  xSemaphoreCreateMutex();

    updata_queue_handle = xQueueCreate(4, sizeof( struct updata_queue_msg));

    __sensor_history_data_restore();
    
    __sensor_history_data_update_init();

    xTaskCreate(esp32_rp2040_comm_task, "esp32_rp2040_comm_task", ESP32_RP2040_COMM_TASK_STACK_SIZE, NULL, 2, NULL);

    xTaskCreate(sensor_history_data_updata_task, "sensor_history_data_updata_task", 1024*8, NULL, 6, NULL);

    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle, 
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_TEMP_HISTORY, 
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle, 
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_HUMIDITY_HISTORY, 
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle, 
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_TVOC_HISTORY, 
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_CO2_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    /* Extended sensor history events */
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_TEMP_EXT_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_HUMIDITY_EXT_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_PM1_0_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_PM2_5_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_PM10_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_NO2_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_C2H5OH_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_VOC_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SENSOR_CO_HISTORY,
                                                            __view_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle,
                                                            VIEW_EVENT_BASE, VIEW_EVENT_SHUTDOWN,
                                                            __view_event_handler, NULL, NULL));
}

int indicator_sensor_get_data(struct view_data_sensor *out_data)
{
    if (!out_data) return -1;
    xSemaphoreTake(__g_data_mutex, portMAX_DELAY);
    memcpy(out_data, &__g_current_sensor_data, sizeof(struct view_data_sensor));
    xSemaphoreGive(__g_data_mutex);
    return 0;
}

