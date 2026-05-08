#ifndef VIEW_DATA_H
#define VIEW_DATA_H

#include "config.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum start_screen{
    SCREEN_SENSECAP_LOG, //todo
    SCREEN_WIFI_CONFIG,
};


#define WIFI_SCAN_LIST_SIZE  15

struct view_data_wifi_st
{
    bool   is_connected;
    bool   is_connecting;
    bool   is_network;  //is connect network
    char   ssid[32];
    int8_t rssi;
};


struct view_data_wifi_config
{
    char    ssid[32];
    uint8_t password[64];
    bool    have_password;
};

struct view_data_wifi_item
{
    char   ssid[32];
    bool   auth_mode;
    int8_t rssi;
};

struct view_data_wifi_list
{
    bool  is_connect;
    struct view_data_wifi_item  connect;
    uint16_t cnt;
    struct view_data_wifi_item aps[WIFI_SCAN_LIST_SIZE];
};

struct view_data_wifi_connet_ret_msg 
{
    uint8_t ret; //0:successfull , 1: failure
    char    msg[64];
};

struct view_data_display
{
    int   brightness; //0~100
    bool  sleep_mode_en;       //Turn Off Screen
    int   sleep_mode_time_min;  
};

struct view_data_time_cfg
{
    bool    time_format_24;

    bool    auto_update; //time auto update
    time_t  time;       // use when auto_update is true
    bool    set_time; 

    bool    auto_update_zone;  // use when  auto_update  is true
    int8_t  zone;       // use when auto_update_zone is true
    
    bool    daylight;   // use when auto_update is true  
}__attribute__((packed));

struct sensor_data_average
{
    float   data;  //Average over the past hour
    time_t  timestamp;
    bool    valid;
};

struct sensor_data_minmax
{
    float   max;
    float   min;
    time_t  timestamp;
    bool    valid;
};

enum sensor_data_type{
    SENSOR_DATA_CO2,
    SENSOR_DATA_TVOC,
    SENSOR_DATA_TEMP,
    SENSOR_DATA_HUMIDITY,
    /* Extended sensors (legacy, Grove-Sensoren des Vorgaengerprojekts) */
    SENSOR_DATA_TEMP_EXT,
    SENSOR_DATA_HUMIDITY_EXT,
    SENSOR_DATA_PM1_0,
    SENSOR_DATA_PM2_5,
    SENSOR_DATA_PM10,
    SENSOR_DATA_NO2,
    SENSOR_DATA_C2H5OH,
    SENSOR_DATA_VOC,
    SENSOR_DATA_CO,
    /* Sauna-Fuehler (SHT85 in der Kabine, via 2m-Kabel) */
    SENSOR_DATA_SAUNA_TEMP,
    SENSOR_DATA_SAUNA_RH,
};

struct view_data_sensor_data
{
    enum sensor_data_type sensor_type;
    float  vaule;
};

struct view_data_sensor_history_data
{
    enum sensor_data_type sensor_type;
    struct sensor_data_average data_day[96];
    struct sensor_data_minmax data_week[7];
    uint8_t resolution;

    float day_min;
    float day_max;

    float week_min;
    float week_max;
};

struct view_data_sensor {
    float co2;
    float temp_internal;
    float humidity_internal;
    float tvoc;
    float temp_external;
    float humidity_external;
    float pm1_0;
    float pm2_5;
    float pm10;
    float multigas_gm102b[2];  // [0]=ppm(eq), [1]=voltage (NO2)
    float multigas_gm302b[2];  // [0]=ppm(eq), [1]=voltage (C2H5OH)
    float multigas_gm502b[2];  // [0]=ppm(eq), [1]=voltage (VOC)
    float multigas_gm702b[2];  // [0]=ppm(eq), [1]=voltage (CO)
    /* Sauna-Fuehler (SHT85). Nur diese Werte fliessen in Session-Log/Chart,
     * der Rest ist nur Vorraum-Anzeige. */
    float sauna_temp;
    float sauna_rh;
};

/* ======================================================================= */
/* Sauna-Session-Datenmodell                                                */
/* ======================================================================= */

#define SSC_OPERATOR_MAXLEN      16
#define SSC_AUFGUSS_NAME_MAXLEN  48
#define SSC_NOTES_MAXLEN        192
#define SSC_SESSION_ID_LEN       24
#define SSC_AUFGUSS_MAX_PER_SESS 16

/* Ein einzelner Aufguss innerhalb einer Session. */
struct view_data_aufguss {
    uint32_t ts_s;                              /* Sekunden seit Session-Start */
    char     name[SSC_AUFGUSS_NAME_MAXLEN];
};

/* Kompakter Metadaten-Record fuer NVS/History-Liste (~256B).
 * Kommt ohne den vollen Sample-Strom aus - der liegt auf SD. */
struct view_data_session_meta {
    char     id[SSC_SESSION_ID_LEN];            /* z.B. "S20260421_143205" */
    time_t   start_ts;
    time_t   end_ts;
    char     operator_tag[SSC_OPERATOR_MAXLEN];
    char     aufguss_headline[SSC_AUFGUSS_NAME_MAXLEN];
    uint16_t participants;
    char     notes[SSC_NOTES_MAXLEN];
    float    peak_temp;
    float    peak_rh;
    uint8_t  aufguss_count;
};

/* v0.3.0: hybrid-storage wire-struct. UART-protokoll-format zwischen
 * ESP32 und RP2040 fuer META_RESP/META_PUSH. MUSS byte-identisch zum
 * ssc_meta_wire_t in SenseCAP_Indicator_RP2040.ino sein. Packed,
 * little-endian. 302 bytes total. Nicht zu verwechseln mit
 * view_data_session_meta - das ist der ESP32-interne speicher-record
 * mit ggf. abweichenden feld-laengen.                                 */
struct __attribute__((packed)) ssc_meta_wire {
    char     id[24];
    int32_t  start_ts;
    int32_t  end_ts;
    char     operator_tag[16];
    char     aufguss_headline[48];
    uint16_t participants;
    char     notes[192];
    float    peak_temp;
    float    peak_rh;
    uint8_t  aufguss_count;
    uint8_t  _reserved[3];
};
typedef struct ssc_meta_wire ssc_meta_wire_t;

/* Live-Sample: pro Sekunde vom Session-Modul an die View gepusht. */
struct view_data_session_live {
    uint32_t t_elapsed_s;
    float    temp;
    float    rh;
    float    peak_temp;
    float    peak_rh;
    uint8_t  aufguss_count;
};

/* Ein History-Listen-Ausschnitt fuer die View (paged loading). */
struct view_data_session_list {
    uint16_t total;
    uint16_t start_index;
    uint16_t count;
    struct view_data_session_meta *items;       /* zeigt in den Heap des Model-Moduls */
};

/* Ein Sample-Chunk fuer die History-Detailansicht (von SD via RP2040). */
#define SSC_SAMPLE_CHUNK_MAX 128
struct view_data_session_sample {
    uint32_t t_elapsed_s;
    float    temp;
    float    rh;
    uint8_t  has_aufguss_marker;
};
struct view_data_session_samples_chunk {
    char     session_id[SSC_SESSION_ID_LEN];
    uint16_t offset;
    uint16_t count;
    uint16_t total;
    struct view_data_session_sample samples[SSC_SAMPLE_CHUNK_MAX];
};

/* Signal: alle chunks fuer eine detail-request sind empfangen + geparsed.
 * UI verwendet das um den detail-chart final zu rendern (einmalig
 * set_point_count + alle akkumulierten samples auf einmal schreiben). */
struct view_data_session_detail_done {
    char     session_id[SSC_SESSION_ID_LEN];
};

/* HTTP-Endpoint-Konfiguration (wird per NVS persistiert). */
struct view_data_http_cfg {
    bool enabled;
    char url[128];
    char bearer_token[96];
};

/* Operator-Preset-Liste (bis zu 8 Kuerzel). */
#define SSC_OPERATOR_PRESETS_MAX 24   /* genug fuer vereinsliste */
struct view_data_operator_presets {
    uint8_t count;
    char    items[SSC_OPERATOR_PRESETS_MAX][SSC_OPERATOR_MAXLEN];
};


enum {
    VIEW_EVENT_SCREEN_START = 0,  // uint8_t, enum start_screen, which screen when start

    VIEW_EVENT_TIME,  //  bool time_format_24
    
    VIEW_EVENT_WIFI_ST,   //view_data_wifi_st_t
    VIEW_EVENT_CITY,      // char city[32], max display 24 char

    VIEW_EVENT_SENSOR_DATA, // struct view_data_sensor_data

    VIEW_EVENT_SENSOR_TEMP,  
    VIEW_EVENT_SENSOR_HUMIDITY,
    VIEW_EVENT_SENSOR_TVOC,
    VIEW_EVENT_SENSOR_CO2,

    VIEW_EVENT_SENSOR_TEMP_HISTORY,
    VIEW_EVENT_SENSOR_HUMIDITY_HISTORY,
    VIEW_EVENT_SENSOR_TVOC_HISTORY,
    VIEW_EVENT_SENSOR_CO2_HISTORY,
    /* Extended sensor history events */
    VIEW_EVENT_SENSOR_TEMP_EXT_HISTORY,
    VIEW_EVENT_SENSOR_HUMIDITY_EXT_HISTORY,
    VIEW_EVENT_SENSOR_PM1_0_HISTORY,
    VIEW_EVENT_SENSOR_PM2_5_HISTORY,
    VIEW_EVENT_SENSOR_PM10_HISTORY,
    VIEW_EVENT_SENSOR_NO2_HISTORY,
    VIEW_EVENT_SENSOR_C2H5OH_HISTORY,
    VIEW_EVENT_SENSOR_VOC_HISTORY,
    VIEW_EVENT_SENSOR_CO_HISTORY,

    VIEW_EVENT_SENSOR_DATA_HISTORY, //struct view_data_sensor_history_data


    VIEW_EVENT_WIFI_LIST,       //view_data_wifi_list_t
    VIEW_EVENT_WIFI_LIST_REQ,   // NULL
    VIEW_EVENT_WIFI_CONNECT,    // struct view_data_wifi_config

    VIEW_EVENT_WIFI_CONNECT_RET,   // struct view_data_wifi_connet_ret_msg


    VIEW_EVENT_WIFI_CFG_DELETE,


    VIEW_EVENT_TIME_CFG_UPDATE,  //  struct view_data_time_cfg
    VIEW_EVENT_TIME_CFG_APPLY,   //  struct view_data_time_cfg

    VIEW_EVENT_DISPLAY_CFG,         // struct view_data_display
    VIEW_EVENT_BRIGHTNESS_UPDATE,   // uint8_t brightness
    VIEW_EVENT_DISPLAY_CFG_APPLY,   // struct view_data_display. will save


    VIEW_EVENT_SHUTDOWN,      //NULL
    VIEW_EVENT_FACTORY_RESET, //NULL
    VIEW_EVENT_SCREEN_CTRL,   // bool  0:disable , 1:enable

    /* === Sauna-Session-Events ========================================== */
    VIEW_EVENT_SESSION_START,           // char operator_tag[SSC_OPERATOR_MAXLEN]
    VIEW_EVENT_SESSION_LIVE,            // struct view_data_session_live
    VIEW_EVENT_SESSION_AUFGUSS,         // char name[SSC_AUFGUSS_NAME_MAXLEN]
    VIEW_EVENT_SESSION_END_REQUEST,     // NULL  (user druekt 'beenden')
    VIEW_EVENT_SESSION_SUMMARY_READY,   // struct view_data_session_meta  (session->UI: summary zeigen)
    VIEW_EVENT_SESSION_SAVE,            // struct view_data_session_meta  (UI->session: final speichern)
    VIEW_EVENT_SESSION_DISCARD,         // NULL

    VIEW_EVENT_HISTORY_LIST_REQ,        // NULL
    VIEW_EVENT_HISTORY_LIST,            // struct view_data_session_list
    VIEW_EVENT_HISTORY_DETAIL_REQ,      // char id[SSC_SESSION_ID_LEN]
    VIEW_EVENT_HISTORY_DETAIL_CHUNK,    // struct view_data_session_samples_chunk
    VIEW_EVENT_HISTORY_DELETE,          // char id[SSC_SESSION_ID_LEN]
    VIEW_EVENT_HISTORY_WIPE_ALL,        // NULL  (alle sessions aus NVS loeschen)
    VIEW_EVENT_HISTORY_WIPE_TEST,       // NULL  (alle sessions mit peak<40C, NVS+SD)
    VIEW_EVENT_HISTORY_FIX_LEGACY,      // NULL  (one-shot fix: TZ + operator-namen v0.3 recovered)
    VIEW_EVENT_HISTORY_DETAIL_DONE,     // struct view_data_session_detail_done  (alle chunks empfangen)
    VIEW_EVENT_SESSION_EDIT,            // struct view_data_session_meta  (edit-mode save)

    VIEW_EVENT_HTTP_CFG_UPDATE,         // struct view_data_http_cfg
    VIEW_EVENT_HTTP_CFG_APPLY,          // struct view_data_http_cfg

    VIEW_EVENT_OPERATOR_PRESETS_UPDATE, // struct view_data_operator_presets
    VIEW_EVENT_OPERATOR_PRESETS,        // struct view_data_operator_presets

    VIEW_EVENT_PROBE_STATE,             // struct view_data_probe_state

    /* === Zeit-Quality + Crash-Observability (v0.2.7) ================== */
    VIEW_EVENT_TIME_STATE_UPDATE,       // struct view_data_time_state
    VIEW_EVENT_TIME_MANUAL_SET,         // struct view_data_time_manual
    VIEW_EVENT_BOOT_INFO,               // struct view_data_boot_info (ESP32)
    VIEW_EVENT_RP_BOOT_INFO,            // struct view_data_rp_boot_info (vom RP2040)

    /* === v0.3.0: Hybrid-Storage SD<->NVS sync ========================= */
    VIEW_EVENT_SD_META_RESP,            // ssc_meta_wire_t  (RP -> ESP, ein eintrag)
    VIEW_EVENT_SD_META_DONE,            // uint16_t         (count)

    VIEW_EVENT_ALL,
};

/* Probe-State vom RP2040: welcher Sauna-Fuehler laeuft, SD-Status.   */
struct view_data_probe_state {
    uint8_t probe;         /* 0=NONE, 1=SHT85, 2=AHT20, 3=SCD41-PROXY */
    uint8_t sd_init;       /* 0 oder 1 */
};

/* ======================================================================= */
/* Zeit-Quality: wie vertrauenswuerdig ist time(NULL) gerade?              */
/* ======================================================================= */
typedef enum {
    TIME_SRC_UNKNOWN       = 0,  /* init-default, sollte nie sichtbar sein */
    TIME_SRC_COMPILE_TIME  = 1,  /* Firmware-Build-Zeit, schlechtester Fall */
    TIME_SRC_NVS_FALLBACK  = 2,  /* letzte persistierte Zeit + Sicherheitspuffer */
    TIME_SRC_MANUAL        = 3,  /* user hat im Settings-Dialog gesetzt */
    TIME_SRC_NTP_SYNCED    = 4,  /* frisch von NTP (pool.ntp.org) */
} time_source_t;

struct view_data_time_state {
    uint8_t source;        /* time_source_t */
    time_t  last_sync_ts;  /* wann zuletzt NTP oder MANUAL - in UTC */
};

/* Payload fuer VIEW_EVENT_TIME_MANUAL_SET (UI -> indicator_time) */
struct view_data_time_manual {
    int16_t year;   /* 2020..2099 */
    int8_t  mon;    /* 1..12 */
    int8_t  day;    /* 1..31 */
    int8_t  hour;   /* 0..23 */
    int8_t  minute; /* 0..59 */
    int8_t  sec;    /* 0..59 */
};

/* ======================================================================= */
/* Crash-Observability: reset-reason + boot-counter beim Boot              */
/* ======================================================================= */
struct view_data_boot_info {
    uint32_t boot_count;   /* monoton steigend, persistiert in NVS */
    uint8_t  reset_reason; /* esp_reset_reason_t als uint8_t */
};

/* Gleiche struct-form vom RP2040 (via CMD 0xA8 angefordert). */
struct view_data_rp_boot_info {
    uint32_t boot_count;
    uint8_t  reset_reason; /* 0=normal, 1=watchdog, sonst raw */
};



#ifdef __cplusplus
}
#endif

#endif
