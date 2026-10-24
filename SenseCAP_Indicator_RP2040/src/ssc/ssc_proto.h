/*
 * ssc_proto.h - UART-Protokoll v2 zwischen ESP32 und RP2040.
 *
 * EINZIGE Quelle fuer Paket-Codes und Wire-Structs. Wird vom RP2040-
 * Sketch (src/ssc/), vom ESP32 (main/CMakeLists.txt INCLUDE_DIRS) und
 * von den Host-Tests (tests/) eingebunden. Alle Structs packed, little-
 * endian, Groessen per static assert festgenagelt.
 *
 * Framing: COBS mit 0x00-Delimiter (PacketSerial auf dem RP2040,
 * util/cobs.c auf dem ESP32), Paket = [typ:1][payload].
 *
 * Regeln:
 *  - Jedes Kommando hat eine EXAKTE Payload-Laenge (ssc_cmd_len()).
 *    Abweichung -> verwerfen, keine Seiteneffekte.
 *  - Zustandsaendernde Kommandos werden mit SSC_PKT_ACK beantwortet.
 *  - Grosse Payloads (META_PUSH, EVENT_LABEL) tragen CRC16-CCITT.
 */
#ifndef SSC_PROTO_H
#define SSC_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSC_PROTO_VERSION 2

#if defined(__cplusplus)
#define SSC_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define SSC_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

#define SSC_ID_LEN 24

/* ---------------- ESP32 -> RP2040 ---------------------------------- */
#define SSC_CMD_COLLECT_INTERVAL 0xA0  /* [u32 BE ms]                     */
#define SSC_CMD_SHUTDOWN         0xA3  /* legacy, wird ignoriert          */
#define SSC_CMD_SESSION_START    0xA4  /* [origin:1] (ID vergibt der RP)  */
#define SSC_CMD_ADD_EVENT        0xA5  /* ssc_add_event_t                 */
#define SSC_CMD_SESSION_END      0xA6  /* [reason:1]                      */
#define SSC_CMD_SD_READBACK      0xA7  /* [u16 req][id24][u32 off][u16 len] BE */
#define SSC_CMD_GET_RP_STATUS    0xA8  /* -                               */
#define SSC_CMD_LIST_SESSIONS    0xA9  /* -                               */
#define SSC_CMD_META_PUSH        0xAA  /* ssc_meta_wire_t                 */
#define SSC_CMD_DELETE_FILE      0xAB  /* [id24]                          */
#define SSC_CMD_DELETE_JSON      0xAC  /* [id24]                          */
#define SSC_CMD_SENSOR_SELFTEST  0xAD  /* [mode:1]                        */
#define SSC_CMD_TIME_SYNC        0x90  /* ssc_time_sync_t (kein ACK)      */
#define SSC_CMD_AUTO_CFG         0x91  /* ssc_auto_cfg_t                  */
#define SSC_CMD_EVENT_LABEL      0x92  /* ssc_event_label_t               */
#define SSC_CMD_GET_EVENTS       0x93  /* [id24]                          */
#define SSC_CMD_HELLO            0x94  /* [proto:1]                       */
#define SSC_CMD_GET_STATE        0x95  /* -  (RP sendet STATE sofort)     */

/* ---------------- RP2040 -> ESP32 ---------------------------------- */
#define SSC_PKT_SCD41_CO2        0xB2  /* float */
#define SSC_PKT_SCD41_TEMP       0xB3  /* float */
#define SSC_PKT_SCD41_RH         0xB4  /* float */
#define SSC_PKT_TVOC             0xB5  /* float */
#define SSC_PKT_SAUNA_TEMP       0xBF  /* float, NaN = ungueltig */
#define SSC_PKT_SAUNA_RH         0xC0  /* float, NaN = ungueltig */
#define SSC_PKT_RBK_CHUNK        0xC1
#define SSC_PKT_PROBE_STATE      0xC2  /* [probe][sd] */
#define SSC_PKT_RP_STATUS        0xC3  /* [u32 boot][rst] */
#define SSC_PKT_META_RESP        0xC4  /* ssc_meta_wire_t */
#define SSC_PKT_META_DONE        0xC5  /* [u16 count] */
#define SSC_PKT_SENSOR_HEALTH    0xC6  /* ssc_sensor_health_t */
#define SSC_PKT_SESSION_STATE    0xC7  /* ssc_session_state_t */
#define SSC_PKT_EVENT            0xC8  /* ssc_event_msg_t (live) */
#define SSC_PKT_EVENT_REC        0xC9  /* ssc_event_full_t (Liste) */
#define SSC_PKT_EVENTS_DONE      0xCA  /* [id24][count][rc] */
#define SSC_PKT_ACK              0xCB  /* ssc_ack_t */
#define SSC_PKT_HELLO_RESP       0xCC  /* ssc_hello_resp_t */

/* ACK-Return-Codes */
enum {
    SSC_RC_OK = 0, SSC_RC_BAD_LEN = 1, SSC_RC_BAD_CRC = 2, SSC_RC_BAD_ID = 3,
    SSC_RC_SD_ERR = 4, SSC_RC_REFUSED = 5, SSC_RC_NOT_FOUND = 6, SSC_RC_STATE = 7,
};

/* Session-Start/Ende-Ursachen */
enum { SSC_ORIGIN_MANUAL = 1, SSC_ORIGIN_AUTO = 2, SSC_ORIGIN_RESUME = 3 };
enum { SSC_END_MANUAL = 1, SSC_END_AUTO = 2, SSC_END_CANCEL = 3 };

/* Recorder-Zustand */
enum { SSC_REC_IDLE = 0, SSC_REC_RECORDING = 1 };

/* Flags in ssc_session_state_t.flags */
#define SSC_SF_SD_OK        0x01
#define SSC_SF_WRITE_OK     0x02
#define SSC_SF_PROBE_OK     0x04
#define SSC_SF_TIME_SYNCED  0x08
#define SSC_SF_AUTO         0x10
#define SSC_SF_RESUMED      0x20
#define SSC_SF_RH_SUSPECT   0x40
#define SSC_SF_HEATER       0x80

/* Event-Arten in Records/.evt (entspricht ssc_ev_kind_t) */
enum {
    SSC_EK_GANG = 2, SSC_EK_AUFGUSS = 3, SSC_EK_LUEFTEN = 4,
    SSC_EK_RESUME = 9, SSC_EK_SENSOR = 10,
};
#define SSC_EVF_MANUAL   0x01
#define SSC_EVF_DELETED  0x02
#define SSC_EVF_LABELED  0x04
#define SSC_EVF_OPEN     0x08   /* noch laufend (z.B. Lueften) */

/* Sensor-Health */
enum { SSC_H_OK = 0, SSC_H_CHECK = 1, SSC_H_RH_INVALID = 2, SSC_H_FAULT = 3 };
enum { SSC_ST_NONE = 0, SSC_ST_PASS = 1, SSC_ST_INCONCLUSIVE = 2,
       SSC_ST_BLIND = 3, SSC_ST_REFUSED = 4, SSC_ST_RUNNING = 5, SSC_ST_ABORTED = 6 };
/* Selbsttest-Ablehnungsgruende (health_reason bei REFUSED) */
enum { SSC_STR_SESSION = 1, SSC_STR_TEMP = 2, SSC_STR_RH = 3, SSC_STR_UNSTABLE = 4,
       SSC_STR_TOO_SOON = 5, SSC_STR_BUDGET = 6, SSC_STR_NO_PROBE = 7 };

#if defined(_MSC_VER)
#error "packed structs: nur GCC/Clang"
#endif
#define SSC_PACKED __attribute__((packed))

typedef struct SSC_PACKED {
    uint32_t utc;          /* Unix-Zeit                                 */
    int32_t  tz_off_s;     /* aktueller Offset lokal-UTC inkl. DST      */
    uint8_t  source;       /* time_source_t des ESP32 (0..4)            */
} ssc_time_sync_t;
SSC_STATIC_ASSERT(sizeof(ssc_time_sync_t) == 9, "time_sync");

typedef struct SSC_PACKED {
    uint8_t  enabled;      /* Auto-Start/-Ende an                       */
    uint8_t  reserved[7];
    uint16_t crc;          /* CRC16 ueber die ersten 8 Bytes            */
} ssc_auto_cfg_t;
SSC_STATIC_ASSERT(sizeof(ssc_auto_cfg_t) == 10, "auto_cfg");

typedef struct SSC_PACKED {
    uint8_t  kind;         /* SSC_EK_AUFGUSS                            */
    uint32_t t_ms;         /* 0 = jetzt                                 */
} ssc_add_event_t;
SSC_STATIC_ASSERT(sizeof(ssc_add_event_t) == 5, "add_event");

typedef struct SSC_PACKED {
    uint8_t  cmd;
    uint8_t  rc;
    uint8_t  extra;        /* z.B. Event-Nr., Ablehnungsgrund           */
} ssc_ack_t;
SSC_STATIC_ASSERT(sizeof(ssc_ack_t) == 3, "ack");

typedef struct SSC_PACKED {
    uint8_t  proto;
    char     fw[16];
    uint32_t boot_count;
    uint8_t  reset_reason;
    uint8_t  caps;         /* bit0 detector, bit1 selftest, bit2 sim    */
} ssc_hello_resp_t;
SSC_STATIC_ASSERT(sizeof(ssc_hello_resp_t) == 23, "hello");

typedef struct SSC_PACKED {
    uint8_t  ver;          /* = SSC_PROTO_VERSION                       */
    uint8_t  rec_state;    /* SSC_REC_*                                 */
    uint8_t  flags;        /* SSC_SF_*                                  */
    uint8_t  phase;        /* ssc_phase_t                               */
    char     id[SSC_ID_LEN];
    uint32_t start_utc;    /* 0 = unbekannt                             */
    uint32_t elapsed_s;
    uint32_t rows;
    uint16_t write_errs;
    uint8_t  gang;
    uint8_t  n_aufguss;
    uint8_t  n_lueften;
    uint8_t  n_events;
    uint8_t  health;       /* SSC_H_*                                   */
    uint8_t  origin;       /* SSC_ORIGIN_*                              */
    int16_t  peak_t_x10;
    uint16_t peak_rh_x10;
    uint32_t boot_count;   /* RP-Neustart erkennen                      */
} ssc_session_state_t;
SSC_STATIC_ASSERT(sizeof(ssc_session_state_t) == 56, "session_state");

typedef struct SSC_PACKED {
    uint8_t  no;           /* 1..255, eindeutig pro Session             */
    uint8_t  kind;         /* SSC_EK_*                                  */
    uint8_t  flags;        /* SSC_EVF_*                                 */
    uint8_t  gang;
    uint8_t  seq;          /* Aufguss-Nr. bzw. Lueften-Nr.              */
    uint8_t  reserved;
    uint32_t t_start_ms;
    uint32_t t_peak_ms;
    uint32_t t_end_ms;
    int16_t  t0_x10, tpk_x10;
    uint16_t rh0_x10, rhpk_x10;
    uint16_t ah0_x10, ahpk_x10;   /* abs. Feuchte g/m3 x10             */
} ssc_event_rec_t;
SSC_STATIC_ASSERT(sizeof(ssc_event_rec_t) == 30, "event_rec");

#define SSC_LBL_OP    16
#define SSC_LBL_NAME  32
#define SSC_LBL_AROMA 24
typedef struct SSC_PACKED {
    uint8_t  participants; /* 0 = unbekannt                             */
    char     operator_tag[SSC_LBL_OP];
    char     name[SSC_LBL_NAME];
    char     aroma[SSC_LBL_AROMA];
} ssc_event_label_body_t;
SSC_STATIC_ASSERT(sizeof(ssc_event_label_body_t) == 73, "label_body");

typedef struct SSC_PACKED {
    char     id[SSC_ID_LEN];
    uint8_t  no;
    uint8_t  delete_flag;  /* 1 = Event als geloescht markieren         */
    ssc_event_label_body_t l;
    uint16_t crc;          /* CRC16 ueber alles davor                   */
} ssc_event_label_t;
SSC_STATIC_ASSERT(sizeof(ssc_event_label_t) == 101, "event_label");

typedef struct SSC_PACKED {
    char     id[SSC_ID_LEN];
    ssc_event_rec_t e;
} ssc_event_msg_t;
SSC_STATIC_ASSERT(sizeof(ssc_event_msg_t) == 54, "event_msg");

typedef struct SSC_PACKED {
    char     id[SSC_ID_LEN];
    ssc_event_rec_t e;
    ssc_event_label_body_t l;
} ssc_event_full_t;
SSC_STATIC_ASSERT(sizeof(ssc_event_full_t) == 127, "event_full");

typedef struct SSC_PACKED ssc_sensor_health {
    uint8_t  health;          /* SSC_H_*                                */
    uint8_t  reason;          /* bitmaske: 1 rh_suspect, 2 heater_stuck,
                                 4 i2c_errors, 8 no_probe              */
    uint8_t  heater_on;       /* Status-Register Bit 13                 */
    uint8_t  st_state;        /* SSC_ST_* des letzten Selbsttests       */
    uint8_t  st_reason;       /* SSC_STR_* bei REFUSED                  */
    uint16_t st_k_x100;       /* Selbsttest-Kennzahl k x100             */
    uint16_t st_age_min;      /* Minuten seit letztem Test, 0xFFFF=nie  */
    uint16_t status_reg;      /* letzter SHT3x-Status                   */
    uint16_t i2c_fails;       /* seit Boot (saturiert)                  */
} ssc_sensor_health_t;
SSC_STATIC_ASSERT(sizeof(ssc_sensor_health_t) == 13, "sensor_health");

/* Bestehender Meta-Record (v0.3, byte-identisch beibehalten).
 * _reserved[0] = Format-Version (2), _reserved[1..2] = CRC16 LE ueber
 * die Bytes 0..298 (alles vor _reserved).                             */
typedef struct SSC_PACKED {
    char     id[SSC_ID_LEN];
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
} ssc_meta_wire_t;
SSC_STATIC_ASSERT(sizeof(ssc_meta_wire_t) == 302, "meta_wire");
SSC_STATIC_ASSERT(offsetof(ssc_meta_wire_t, _reserved) == 299, "meta_wire off");

/* ---------------- Helfer ------------------------------------------- */
static inline uint16_t ssc_crc16(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static inline void ssc_meta_seal(ssc_meta_wire_t *m) {
    uint16_t c = ssc_crc16(m, offsetof(ssc_meta_wire_t, _reserved));
    m->_reserved[0] = 2;
    m->_reserved[1] = (uint8_t)(c & 0xFF);
    m->_reserved[2] = (uint8_t)(c >> 8);
}
/* 1 = gueltig v2, 0 = legacy (keine CRC), -1 = CRC falsch */
static inline int ssc_meta_check(const ssc_meta_wire_t *m) {
    if (m->_reserved[0] != 2) return 0;
    uint16_t c = ssc_crc16(m, offsetof(ssc_meta_wire_t, _reserved));
    return (m->_reserved[1] == (uint8_t)(c & 0xFF) &&
            m->_reserved[2] == (uint8_t)(c >> 8)) ? 1 : -1;
}

/* Erlaubte Zeichen in Session-IDs: [A-Za-z0-9_-], 1..23 Zeichen. */
static inline int ssc_id_valid(const char *id, size_t maxlen) {
    size_t i = 0;
    for (; i < maxlen && id[i]; i++) {
        char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return i > 0 && i < maxlen;
}

/* Exakte Payload-Laenge eines Kommandos (ohne Typ-Byte), -1 = unbekannt.
 * SESSION_START akzeptiert zusaetzlich die Legacy-Form [id24] -> 24.    */
static inline int ssc_cmd_len(uint8_t cmd) {
    switch (cmd) {
    case SSC_CMD_COLLECT_INTERVAL: return 4;
    case SSC_CMD_SHUTDOWN:         return 0;
    case SSC_CMD_SESSION_START:    return 1;
    case SSC_CMD_ADD_EVENT:        return (int)sizeof(ssc_add_event_t);
    case SSC_CMD_SESSION_END:      return 1;
    case SSC_CMD_SD_READBACK:      return 2 + SSC_ID_LEN + 4 + 2;
    case SSC_CMD_GET_RP_STATUS:    return 0;
    case SSC_CMD_LIST_SESSIONS:    return 0;
    case SSC_CMD_META_PUSH:        return (int)sizeof(ssc_meta_wire_t);
    case SSC_CMD_DELETE_FILE:      return SSC_ID_LEN;
    case SSC_CMD_DELETE_JSON:      return SSC_ID_LEN;
    case SSC_CMD_SENSOR_SELFTEST:  return 1;
    case SSC_CMD_TIME_SYNC:        return (int)sizeof(ssc_time_sync_t);
    case SSC_CMD_AUTO_CFG:         return (int)sizeof(ssc_auto_cfg_t);
    case SSC_CMD_EVENT_LABEL:      return (int)sizeof(ssc_event_label_t);
    case SSC_CMD_GET_EVENTS:       return SSC_ID_LEN;
    case SSC_CMD_HELLO:            return 1;
    case SSC_CMD_GET_STATE:        return 0;
    default:                       return -1;
    }
}

#ifdef __cplusplus
}
#endif
#endif
