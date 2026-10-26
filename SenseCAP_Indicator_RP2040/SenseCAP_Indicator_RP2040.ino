/*
 * sscsaunalogger — RP2040-Sketch fuer SenseCAP Indicator D1 (Sauna-Fork).
 *
 * Rolle des RP2040:
 *   1. Sensor-Aggregator fuer die auf der Platine verbauten Sensoren:
 *        - SCD41  (CO2/Temp/RH, I2C 0x62, nur fuer Vorraum-Anzeige)
 *        - SGP40  (TVOC,         I2C 0x59, Vorraum-Anzeige)
 *        - SHT85  (Sauna T/RH,   I2C 0x44, ueber 2m-Kabel in die Kabine,
 *                  eigener I2C-Driver weiter unten)
 *   2. SD-Gateway: die SD-Karte ist am RP2040-SPI1 angeschlossen, nicht
 *      am ESP32. Der ESP32 sendet Kommandos (COBS ueber UART1), der
 *      RP2040 fuehrt sie aus und schreibt ein CSV pro Session auf SD.
 *
 * Messzyklus: 1 Hz (aufgussturbulenz braucht Aufloesung). Die alten
 * Grove-Sensoren des Vorgaengerprojekts (AHT20, HM3301, MultiGas V2)
 * sind nicht mehr teil des Saunaloggers und werden hier nicht mehr
 * initialisiert - der Code bleibt als "#if SSC_LEGACY_GROVE" auskommentiert,
 * damit eine spaetere Rollback-Variante moeglich ist.
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <EEPROM.h>                 /* v0.2.7: boot-counter in flash-emuliertem EEPROM */
#include <PacketSerial.h>
#include <SensirionI2CSgp40.h>
#include <SensirionI2cScd4x.h>
#include <VOCGasIndexAlgorithm.h>
#include "AHT20.h"
#include "hardware/watchdog.h"    /* watchdog_caused_reboot() fuer post-crash diagnose */
#include "src/ssc/ssc_proto.h"     /* v0.4.0: gemeinsames UART-Protokoll (ESP32 + Host-Tests) */
#include "src/ssc/ssc_detect.h"    /* v0.4.0: Auto-Modus-Detektor (Host-getestet) */

#define DEBUG 0
#define VERSION "ssc-v0.4.0"
/* v0.2.7: EEPROM layout. 256 B reserviert, erste 8B belegt.
 *  [0..3] uint32_t magic = 0x55AA5AA5
 *  [4..7] uint32_t boot_count                                      */
#define SSC_EEPROM_SIZE     256
#define SSC_EEPROM_MAGIC    0x55AA5AA5u
#define SSC_EEPROM_OFF_MAGIC 0
#define SSC_EEPROM_OFF_BC    4
#define SSC_LEGACY_GROVE 0   /* 1 = AHT20/HM3301/MultiGas wieder aktivieren */

/* v0.2.8: Reset-Reason via watchdog-scratch[0] canary.
 * RP2040-scratch-register (hardware/structs/watchdog.h, datasheet 2.8.1.1):
 * 8 x uint32 die WDT-reset und NVIC_SystemReset UEBERLEBEN, aber bei echtem
 * POR (inkl. brown-out) auf 0 genullt werden. Mit einem magic-canary in
 * scratch[0] unterscheiden wir damit 3 zustaende beim boot:
 *   canary != MAGIC             -> POR (oder brown-out)
 *   canary == MAGIC, was_wdt    -> watchdog reset (hang/missed-feed)
 *   canary == MAGIC, !was_wdt   -> soft reset (hardfault-handler, NVIC,
 *                                  external-reset-button etc.)
 * Ein echter hardfault auf dem M0+ springt in den default-handler der
 * einen chip-reset macht - das erscheint hier als SOFT, nicht als POR.
 * Bisher (v0.2.7) wurde beides undifferenziert als "POR" gelogged.     */
#include "hardware/structs/watchdog.h"
#define SSC_SCRATCH_CANARY_VAL   0x5A1E4CA7u
typedef enum {
    SSC_RST_POR  = 0,
    SSC_RST_WDT  = 1,
    SSC_RST_SOFT = 2,
} ssc_reset_reason_t;

/* Forward-declarations der file-static Funktionen. .ino-Files bekommen zwar
 * von Arduino auto-prototypes, aber fuer `static` mit nicht-trivialer
 * Signatur ist der Auto-Prototyper unzuverlaessig. Sauber explizit machen. */
static void sht_after_init(void);
static bool sht85_init(void);
static bool aht20_fallback_init(void);
static void i2c_bus_unlock(uint8_t sda_pin, uint8_t scl_pin);
static void session_recovery_scan(void);

/* ------------------------------------------------------------------ */
/* I²C-INVARIANTE                                                     */
/* ------------------------------------------------------------------ */
/* Alle I²C-Transaktionen laufen ausschliesslich auf core0 im loop()-
 * Context. Keine ISR und kein core1-Task darf `Wire` anfassen - sonst
 * sind Lock- und Race-Freiheit nicht mehr gegeben. Die Sensirion-Libs
 * sind blocking-polled, sht85_measure() ebenso. PacketSerial liest nur
 * UART (Serial1) - kein I2C.                                          */

/* ------------------------------------------------------------------ */
/* Runtime-Parameter                                                  */
/* ------------------------------------------------------------------ */
#define SSC_INTERVAL_NORMAL_MS    500   /* 2 Hz dauerhaft (v0.3.1: war
                                         * 1 Hz mit aufguss-boost - jetzt
                                         * dauer-2Hz weil aufguss-button
                                         * komplett raus ist).          */
/* SCD41 sitzt direkt neben dem ESP32 auf der PCB - Abwaerme der CPU
 * + des LCD-Panels bringt ca. 7-8 °C Offset (Kalibrier-Messung mit
 * externem SHT35 als Referenz: intern=31.1 C, extern=23.5 C).       */
#define SSC_SCD41_TEMP_OFFSET_C  (-7.6f)
#define SSC_INTERVAL_BOOST_MS     500   /* 2 Hz waehrend Aufguss (2 min) */
#define SSC_BOOST_DURATION_MS  120000   /* 120s Aufguss-Boost            */
#define SSC_SHT85_RETRY_MAX         3   /* CRC-/Read-Retries             */
#define SSC_WATCHDOG_MS          8000   /* 8s Watchdog-Timeout           */

/* Log-Prefix fuer besseres Debug-Grep */
#define LOG_TAG  "[SAUNA] "
static inline void slog(const char *msg) {
    Serial.print(LOG_TAG); Serial.println(msg);
}
static inline void slogf(const char *lbl, float v, int decimals = 2) {
    Serial.print(LOG_TAG); Serial.print(lbl); Serial.print(": ");
    Serial.println(v, decimals);
}

/* ------------------------------------------------------------------ */
/* Paket-Typcodes                                                     */
/* ------------------------------------------------------------------ */
/* Von RP -> ESP (legacy, werden weiter gesendet wenn Sensoren da sind) */
#define PKT_TYPE_SENSOR_SCD41_CO2       0xB2
#define PKT_TYPE_SENSOR_SHT41_TEMP      0xB3  /* alt benannter Slot: fuehrt SCD41-Temp */
#define PKT_TYPE_SENSOR_SHT41_HUMIDITY  0xB4  /* alt benannter Slot: fuehrt SCD41-RH   */
#define PKT_TYPE_SENSOR_TVOC_INDEX      0xB5

/* Neu fuer den Saunalogger */
#define PKT_TYPE_SENSOR_SAUNA_TEMP      0xBF  /* SHT85 Kabinentemp   (float) */
#define PKT_TYPE_SENSOR_SAUNA_RH        0xC0  /* SHT85 Kabinenfeuchte (float) */
#define PKT_TYPE_SD_READBACK_CHUNK      0xC1  /* Antwort auf 0xA7:
                                                 [2B req_id][2B len][data] */
#define PKT_TYPE_PROBE_STATE            0xC2  /* 1B: sauna_probe_t enum +
                                                 1B: sd_init_flag */

/* Von ESP -> RP                                                        */
#define PKT_TYPE_CMD_COLLECT_INTERVAL   0xA0  /* 4B Millisekunden */
#define PKT_TYPE_CMD_BEEP_ON            0xA1
#define PKT_TYPE_CMD_SHUTDOWN           0xA3
#define PKT_TYPE_CMD_SESSION_START      0xA4  /* payload: session-id (<=23B + NUL) */
#define PKT_TYPE_CMD_SESSION_AUFGUSS    0xA5  /* payload: name (<=47B + NUL) */
#define PKT_TYPE_CMD_SESSION_END        0xA6  /* kein payload */
#define PKT_TYPE_CMD_SD_READBACK        0xA7  /* payload: [2B req_id][4B byte_offset][2B len] */
/* v0.2.7: ESP32 fragt RP2040-Bootstatus an, RP antwortet mit 0xC3 */
#define PKT_TYPE_CMD_GET_RP_STATUS      0xA8  /* kein payload */
#define PKT_TYPE_RP_STATUS              0xC3  /* 5B: [boot_count:4B][reset_reason:1B] */

/* v0.3.0: hybrid storage protocol - SD ist authoritative, NVS auf ESP32
 * ist nur cache. ESP32 fragt LIST_SESSIONS, RP2040 streamt META_RESP
 * pro session, abschliessend META_DONE. ESP32 sendet META_PUSH bei
 * jedem session-save, RP2040 schreibt /sessions/<id>.json.            */
#define PKT_TYPE_CMD_LIST_SESSIONS       0xA9  /* kein payload */
#define PKT_TYPE_CMD_SESSION_META_PUSH   0xAA  /* payload: ssc_meta_wire_t */
#define PKT_TYPE_CMD_SESSION_DELETE_FILE 0xAB  /* payload: id (24B null-term) */
#define PKT_TYPE_CMD_SESSION_DELETE_JSON 0xAC  /* payload: id (24B null-term), nur .json sidecar */
#define PKT_TYPE_CMD_SHT85_HEATER        0xAD  /* payload: 1B power (0=33mW, 1=200mW) */
#define PKT_TYPE_SESSION_META_RESP       0xC4  /* payload: ssc_meta_wire_t */
#define PKT_TYPE_SESSION_META_DONE       0xC5  /* 2B: count */
#define PKT_TYPE_SENSOR_HEALTH           0xC6  /* SHT85 filter-health, 8B:
                                                  [state][last_hp][2B min_since LE]
                                                  [2B count LE][2B std_x100 LE] */

/* ssc_meta_wire_t + alle v2-Pakete: siehe src/ssc/ssc_proto.h */

/* ------------------------------------------------------------------ */
/* Geraete-Objekte                                                    */
/* ------------------------------------------------------------------ */
SensirionI2CSgp40 sgp40;
SensirionI2cScd4x scd4x;
VOCGasIndexAlgorithm voc_algorithm;
/* Groesserer COBS-Puffer (default=256). Unsere SD-Readback-Chunks sind heute
 * ca. 213 B, dazu kommt COBS-Overhead - gegen Overflow-Desync ueber grosszuegige
 * 512 B haerten. Wird nur 1x pro Board-Boot allokiert. */
PacketSerial_<COBS, 0, 512> myPacketSerial;
AHT20 aht20_fallback;

/* Welcher Sauna-Fuehler-Typ ist aktiv? Auto-Erkennung in setup(). */
enum sauna_probe_t {
    SAUNA_PROBE_NONE  = 0,
    SAUNA_PROBE_SHT85 = 1,   /* bevorzugt, spec bis 105 °C / 100 %RH */
    SAUNA_PROBE_AHT20 = 2,   /* Fallback fuer Testbetrieb, bis 85 °C */
    SAUNA_PROBE_SCD41 = 3,   /* Letzter Fallback: interner Vorraum-Sensor
                                wird parallel als "Sauna" gesendet.
                                Nur fuer Dev/Demo ohne externen Fuehler. */
};
static uint8_t g_sauna_probe = SAUNA_PROBE_NONE;

static bool scd_ready = false;
static bool sgp_ready = false;
static bool sht_ready = false;
static bool aht_ready = false;
static bool sd_init_flag = false;

/* Mess-Kompensation fuer SGP40 (Feuchte/Temp vom SCD41). */
static uint16_t compensationRh = 0x8000;
static uint16_t compensationT  = 0x6666;

/* Aktuelle Sauna-Werte (werden sekundengenau erneuert). */
static float g_sauna_temp = NAN;
static float g_sauna_rh   = NAN;

/* Messintervall in ms (1 Hz default, per 0xA0 zur Laufzeit aenderbar,
 * und beim Aufguss-Kommando temporaer auf SSC_INTERVAL_BOOST_MS).   */
static uint32_t g_interval_ms   = SSC_INTERVAL_NORMAL_MS;
static uint32_t g_interval_base = SSC_INTERVAL_NORMAL_MS;  /* user-choice */
static uint32_t g_last_tick_ms = 0;

/* Zaehler fuer I2C-Fehler - hilft beim Debuggen von Kabelproblemen. */
static uint32_t g_sht85_fails = 0;

/* v0.2.7: boot-observability - wird in setup() befuellt, bleibt danach
 * stabil bis zum naechsten reset.                                        */
static uint32_t g_boot_count    = 0;
static uint8_t  g_last_reset_reason = 0;  /* ssc_reset_reason_t enum    */

/* v0.2.8: UART packet-health-counter. Laufen pro boot bei 0 los und
 * wrappen alle 65k events. Im 60-s-health-log sichtbar damit wir beim
 * naechsten reboot-loop sofort sehen ob UART-traffic beteiligt ist.     */
static uint16_t g_pkt_rx     = 0;   /* total packets dispatched        */
static uint16_t g_pkt_rx_unk = 0;   /* unknown cmd-byte (subset of rx) */

/* ------------------------------------------------------------------ */
/* Senden eines typisierten Float-Pakets                              */
/* ------------------------------------------------------------------ */
static void sensor_data_send(uint8_t type, float data) {
    uint8_t buf[5];
    buf[0] = type;
    memcpy(&buf[1], &data, sizeof(float));
    myPacketSerial.send(buf, sizeof(buf));
}

static void raw_packet_send(const uint8_t *buf, size_t len) {
    myPacketSerial.send(buf, len);
}

/* ------------------------------------------------------------------ */
/* v0.2.7: Boot-Counter in EEPROM (flash-emuliert)                    */
/* ------------------------------------------------------------------ */
static uint32_t rp2040_boot_counter_tick_and_get(void) {
    EEPROM.begin(SSC_EEPROM_SIZE);
    uint32_t magic = 0;
    uint32_t bc    = 0;
    EEPROM.get(SSC_EEPROM_OFF_MAGIC, magic);
    EEPROM.get(SSC_EEPROM_OFF_BC,    bc);
    if (magic != SSC_EEPROM_MAGIC) {
        magic = SSC_EEPROM_MAGIC;
        bc    = 0;
    }
    bc++;
    EEPROM.put(SSC_EEPROM_OFF_MAGIC, magic);
    EEPROM.put(SSC_EEPROM_OFF_BC,    bc);
    EEPROM.commit();
    /* EEPROM.end() ruht die Maschine nicht - die Lib kopiert einen RAM-
     * puffer auf flash nur bei commit(). Wir lassen begin()-state aktiv,
     * das braucht keine weitere Ressource.                              */
    return bc;
}

/* v0.2.8: 3-state Reset-Reason via watchdog-scratch[0] canary.
 * Muss SEHR FRUEH im setup() aufgerufen werden - insbesondere bevor der
 * pico-sdk irgendein scratch-register selbst beschreibt. Setzt den
 * canary fuer den naechsten boot-check neu.                             */
static ssc_reset_reason_t rp2040_detect_reset_reason(bool was_wdt) {
    uint32_t canary = watchdog_hw->scratch[0];
    ssc_reset_reason_t rr;
    if (canary != SSC_SCRATCH_CANARY_VAL) {
        rr = SSC_RST_POR;
    } else if (was_wdt) {
        rr = SSC_RST_WDT;
    } else {
        rr = SSC_RST_SOFT;
    }
    watchdog_hw->scratch[0] = SSC_SCRATCH_CANARY_VAL;
    return rr;
}

/* Antwort auf CMD 0xA8: Format [0xC3][boot_count:4B little-endian]
 * [reset_reason:1B]. ESP32 parst in view_data_rp_boot_info.            */
static void rp2040_send_status(void) {
    uint8_t pkt[6];
    pkt[0] = PKT_TYPE_RP_STATUS;
    pkt[1] = (uint8_t)(g_boot_count         & 0xFF);
    pkt[2] = (uint8_t)((g_boot_count >>  8) & 0xFF);
    pkt[3] = (uint8_t)((g_boot_count >> 16) & 0xFF);
    pkt[4] = (uint8_t)((g_boot_count >> 24) & 0xFF);
    pkt[5] = g_last_reset_reason;
    raw_packet_send(pkt, sizeof(pkt));
}

/* ================================================================== */
/* SHT3x (SHT35/SHT85) — eigene I2C-Routine                            */
/*                                                                     */
/* Adresse 0x44 ist Standard (ADDR-pin LOW). 0x45 = ADDR-pin HIGH.     */
/* FS400-SHT35-Kabel-Module koennen entweder Variante sein; wir        */
/* probieren beide automatisch.                                        */
/* ================================================================== */
#define SHT85_CMD_SINGLE_HI     0x2400   /* single shot, high repeatability */
static uint8_t g_sht_addr = 0x44;        /* wird beim init ggf. auf 0x45 gesetzt */

static uint8_t sht85_crc8(const uint8_t *data, uint8_t len) {
    /* Sensirion-CRC8: init 0xFF, poly 0x31, no reflect, no xor-out */
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

static bool sht85_write_cmd(uint16_t cmd) {
    Wire.beginTransmission(g_sht_addr);
    Wire.write(uint8_t(cmd >> 8));
    Wire.write(uint8_t(cmd & 0xFF));
    return (Wire.endTransmission() == 0);
}

static bool sht85_measure(float *temp_c, float *rh_pct) {
    if (!sht85_write_cmd(SHT85_CMD_SINGLE_HI)) return false;
    /* High-Repeatability-Messung braucht bis 15 ms */
    delay(16);

    if (Wire.requestFrom(g_sht_addr, (uint8_t)6) != 6) return false;
    /* Defensiv: auch nach erfolgreichem requestFrom kann available() kleiner
     * sein wenn ein Kabelglitch dazwischenkam. Ohne den check gibt Wire.read()
     * -1 zurueck, (uint8_t)-1 = 0xFF - CRC-check faellt dann zwar, aber wir
     * haben vorher 6 * Wire.read() durchlaufen, bei gewissen Wire-Impls
     * kann das einen ISR-Deadlock triggern. Lieber sauber rausspringen. */
    if (Wire.available() < 6) return false;
    uint8_t raw[6];
    for (uint8_t i = 0; i < 6; i++) raw[i] = (uint8_t)Wire.read();

    if (sht85_crc8(raw, 2) != raw[2]) return false;
    if (sht85_crc8(raw + 3, 2) != raw[5]) return false;

    uint16_t t_raw  = (uint16_t(raw[0]) << 8) | raw[1];
    uint16_t rh_raw = (uint16_t(raw[3]) << 8) | raw[4];
    float t  = -45.0f + 175.0f * ((float)t_raw / 65535.0f);
    float rh = 100.0f * ((float)rh_raw / 65535.0f);

    /* Physical-sanity: Finnische Sauna-Envelope mit ca. 10 degC Rand. Der SHT85
     * raw scale koennte -45..+130 degC liefern, aber physikalisch in einer
     * Sauna-Umgebung sind wir bei -10..+120. CRC-valid-aber-geglitchte reads
     * (EMI, Kabel-Transiente) sollen nicht in die CSV landen - lieber ins
     * retry/recovery laufen lassen. */
    if (t < -10.0f || t > 120.0f) return false;
    if (rh < 0.0f   || rh > 105.0f) return false;  /* 105 toleriert RH-Overshoot */
    if (rh > 100.0f) rh = 100.0f;                  /* dann clippen */
    if (rh < 0.0f)   rh = 0.0f;
    *temp_c = t;
    *rh_pct = rh;
    return true;
}

/* ------------------------------------------------------------------ */
/* I2C-Scan fuer Diagnose - zeigt beim Boot ALLE gefundenen Adressen */
/* ------------------------------------------------------------------ */
static void i2c_scan_log(const char *when) {
    /* Wir bauen eine Einzel-String zusammen und printen sie AM STUECK,
     * damit USB-CDC die Nachricht nicht in mitten zerstueckelt. */
    char buf[128];
    int pos = snprintf(buf, sizeof(buf), "%sI2C-Scan (%s):", LOG_TAG, when);
    if (pos < 0 || pos >= (int)sizeof(buf)) {                 /* snprintf-fail */
        Serial.println("[SAUNA] i2c_scan_log: snprintf tag failed"); return;
    }
    int n = 0;
    for (uint8_t a = 1; a < 127; a++) {
        /* Scan kann bei bus-hang theoretisch laenger dauern als erwartet.
         * Watchdog zwischendurch fuettern (Scan laeuft nur in setup vor
         * wdt_begin und dann selten im loop - trotzdem safe). */
        rp2040.wdt_reset();
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            /* Guard VOR snprintf - sonst unterlaeuft `sizeof(buf) - pos`
             * wenn pos bereits >= sizeof(buf) ist (size_t wraparound). */
            if (pos + 8 >= (int)sizeof(buf)) break;
            pos += snprintf(buf + pos, sizeof(buf) - pos, " 0x%02X", a);
            n++;
        }
    }
    if (n == 0 && pos + 20 < (int)sizeof(buf)) {
        snprintf(buf + pos, sizeof(buf) - pos, " (nichts gefunden)");
    }
    Serial.println(buf);
    Serial.flush();   /* sicher in den USB-CDC-Puffer druecken */
}

/* ------------------------------------------------------------------ */
/* I²C Bus-Recovery - 9-Clock manuelles Unlock wenn SDA stuck LOW     */
/* ------------------------------------------------------------------ */
/* Ein Slave der mid-transaction brownout-t kann SDA LOW halten bis er
 * den 9. Clock-Pulse sieht, mit dem er die aktuelle Read-Response
 * beendet. Wire.begin() selbst kann das NICHT - es wuerde naiv START
 * senden und stuck-bleiben. Diese Funktion togglet SCL als GPIO so
 * lange bis SDA wieder HIGH ist, dann STOP-Kondition manuell. Aufruf:
 *   - einmal in setup() vor Wire.begin() (nach brownout/WDT-recovery)
 *   - optional nach N consecutive SHT85-fails als Heilmittel
 * Die Pins werden danach als INPUT_PULLUP released, damit Wire.begin()
 * sie ohne Konflikt uebernehmen kann.                                */
static void i2c_bus_unlock(uint8_t sda_pin, uint8_t scl_pin) {
    pinMode(scl_pin, OUTPUT_OPENDRAIN);
    pinMode(sda_pin, INPUT_PULLUP);
    digitalWrite(scl_pin, HIGH);
    delayMicroseconds(10);

    /* Wenn SDA bereits HIGH ist, kein Slave haelt die Bus - nichts zu tun. */
    if (digitalRead(sda_pin) == HIGH) {
        pinMode(scl_pin, INPUT_PULLUP);
        return;
    }

    for (int i = 0; i < 9; i++) {
        digitalWrite(scl_pin, LOW);  delayMicroseconds(10);  /* ~50 kHz-konforme half-period */
        digitalWrite(scl_pin, HIGH); delayMicroseconds(10);
        if (digitalRead(sda_pin) == HIGH) break;             /* Slave hat losgelassen */
    }

    /* Manual STOP: SDA low -> high waehrend SCL high. */
    pinMode(sda_pin, OUTPUT_OPENDRAIN);
    digitalWrite(sda_pin, LOW);  delayMicroseconds(10);
    digitalWrite(scl_pin, HIGH); delayMicroseconds(10);
    digitalWrite(sda_pin, HIGH); delayMicroseconds(10);

    /* Beide wieder als Inputs, damit Wire.begin() sauber uebernimmt. */
    pinMode(sda_pin, INPUT_PULLUP);
    pinMode(scl_pin, INPUT_PULLUP);
}

/* ------------------------------------------------------------------ */
/* SD-Initialisierung (kann nachtraeglich erneut aufgerufen werden)   */
/* ------------------------------------------------------------------ */
static bool sd_try_init(void) {
    const int chipSelect = 13;
    /* CS VOR SD.begin() explizit HIGH treiben. Ohne diesen Drive floatet
     * GP13 zwischen SPI1.setSCK/TX/RX und dem internen pinMode das SD.begin()
     * aufruft. Waehrend dieser Luecke generiert SD.begin()'s 74-Clock-Wakeup
     * Flanken die bei schwebendem CS als Command-Byte-Anfang interpretiert
     * werden koennen - dann scheitern Folge-Initialisierungen bis Power-Cycle.
     * Besonders wichtig nach WDT-Reset, wo die Karte noch in einem undefinierten
     * Zustand ist. */
    pinMode(chipSelect, OUTPUT);
    digitalWrite(chipSelect, HIGH);
    delayMicroseconds(50);

    /* v0.4.0: 12 MHz statt 1 MHz (Readback/LIST blockierten sonst
     * sekundenlang); Fallback auf langsamere Takte falls die Karte
     * zickt.                                                          */
    static const uint32_t speeds[] = { 12000000UL, 4000000UL, 1000000UL };
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        if (SD.begin(chipSelect, speeds[i], SPI1)) {
            Serial.print(LOG_TAG); Serial.print("SD: initialisiert @ ");
            Serial.print(speeds[i] / 1000000UL); Serial.println(" MHz");
            return true;
        }
        SD.end(false);
        delay(5);
        rp2040.wdt_reset();
    }
    slog("SD: begin() fehlgeschlagen (karte nicht erkannt / falsches FS? "
         "128GB-Karten muessen FAT32 formatiert sein - Arduino-SD-Lib "
         "kann kein exFAT)");
    return false;
}

/* Boot-Recovery-Scan: sucht nach kaputten CSV-Dateien unter /sessions/
 * und loescht sie. Eine Datei gilt als kaputt, wenn sie kleiner als der
 * CSV-Header (32 Bytes) ist - das passiert wenn der letzte Lauf mid-
 * flush einen Brownout hatte. Ohne diese Bereinigung wachsen orphan
 * clusters ueber Wochen an, bis die Karte voll ist.
 * Bounded: max 100 eintraege, wdt_reset pro iteration.               */
static void session_recovery_scan(void) {
    if (!sd_init_flag) return;
    File dir = SD.open("/sessions");
    if (!dir) return;
    int scanned = 0, removed = 0;
    File entry;
    while (scanned < 100 && (entry = dir.openNextFile())) {
        rp2040.wdt_reset();
        scanned++;
        uint32_t sz = entry.size();
        const char *nm = entry.name();
        if (sz < 32 && nm != nullptr) {
            /* Pfad zusammenbauen - openNextFile gibt je nach Lib nur
             * basename oder den vollen Pfad. Wir pruefen beide. */
            char p[48];
            if (nm[0] == '/') {
                snprintf(p, sizeof(p), "%s", nm);
            } else {
                snprintf(p, sizeof(p), "/sessions/%s", nm);
            }
            entry.close();
            if (SD.remove(p)) {
                removed++;
                Serial.print(LOG_TAG); Serial.print("recovery: removed empty ");
                Serial.println(p);
            }
            continue;
        }
        entry.close();
    }
    dir.close();
    if (scanned > 0) {
        Serial.print(LOG_TAG); Serial.print("recovery scan: scanned=");
        Serial.print(scanned); Serial.print(" removed=");
        Serial.println(removed);
    }
}

static bool sht_try_addr(uint8_t addr) {
    g_sht_addr = addr;
    Wire.beginTransmission(addr);
    Wire.write(0x30);
    Wire.write(0xA2);   /* Soft Reset */
    if (Wire.endTransmission() != 0) return false;
    delay(5);
    float t, rh;
    if (!sht85_measure(&t, &rh)) return false;
    Serial.print(LOG_TAG);
    Serial.print("SHT3x ready @ 0x");
    Serial.print(addr, HEX);
    Serial.print(" (SHT35/SHT85 kompatibel): ");
    Serial.print(t, 2);   Serial.print(" degC, ");
    Serial.print(rh, 2);  Serial.println(" %RH");
    return true;
}

static bool sht85_init(void) {
    /* Wire-Controller beim Init-after-recovery sauber zuruecksetzen.
     * Bei hot-plug/unplug des 2m-Kabels kann das I²C-Peripheral in einem
     * gejammten state sein den ein einfaches 0x30A2 soft-reset-Kommando
     * nicht mehr loesen kann - es IST selbst eine I²C-Transaktion. Ein
     * voller Wire.end()/Wire.begin() macht das Peripheral frei.       */
    Wire.end();
    delay(2);
    Wire.setSDA(20);
    Wire.setSCL(21);
    Wire.begin();
    Wire.setClock(50000);
    Wire.setTimeout(25, true);

    /* Soft-Reset + Probe auf 0x44 ODER 0x45 (FS400-Varianten).      */
    if (sht_try_addr(0x44)) { sht_after_init(); return true; }
    if (sht_try_addr(0x45)) { sht_after_init(); return true; }
    slog("SHT3x not found on I2C 0x44 or 0x45");
    return false;
}

/* ------------------------------------------------------------------ */
/* SHT3x Status-Register + Heater-Sicherheit (v0.4.0)                 */
/* ------------------------------------------------------------------ */
/* Erkenntnisse (Sensirion SHT3x-DIS Datenblatt §4.9/4.10, Research
 * 2026-10-24):
 *  - 0x306D/0x3066 ist ein TOGGLE: der Heater bleibt an bis OFF oder
 *    Soft-Reset. Der 9-Clock-Bus-Unlock schaltet ihn NICHT ab.
 *  - Der Heater ist nur "for plausibility checking" gedacht. Gegen
 *    Kontamination (Aroma-Oele) hilft er im heissen Zustand nicht.
 *    Deshalb: KEIN automatisches Heizen mehr, nur ein bewachter
 *    Selbsttest im kalten Zustand (sensor_care weiter unten).
 *  - Nach jedem Init/Soft-Reset und jede Minute wird per Status-
 *    Register (0xF32D, Bit 13) geprueft, dass der Heater AUS ist.     */
#define SHT_CMD_HEATER_ON   0x306D
#define SHT_CMD_HEATER_OFF  0x3066
#define SHT_CMD_STATUS      0xF32D
#define SHT_CMD_CLR_STATUS  0x3041
#define SHT_STATUS_HEATER   0x2000

static uint16_t g_sht_status      = 0;
static bool     g_sht_heater_on   = false;   /* letzter verifizierter Zustand */
static bool     g_sht_heater_stuck = false;  /* OFF liess sich nicht verifizieren */

static bool sht_status_read(uint16_t *st) {
    if (!sht85_write_cmd(SHT_CMD_STATUS)) return false;
    delay(1);
    if (Wire.requestFrom(g_sht_addr, (uint8_t)3) != 3) return false;
    if (Wire.available() < 3) return false;
    uint8_t b[3];
    for (int i = 0; i < 3; i++) b[i] = (uint8_t)Wire.read();
    if (sht85_crc8(b, 2) != b[2]) return false;
    *st = (uint16_t)((b[0] << 8) | b[1]);
    return true;
}

/* Heater AUS schalten und per Status-Register verifizieren. Eskalation:
 * 3x OFF, dann Soft-Reset (setzt laut Datenblatt den Heater zurueck).
 * Gibt true zurueck wenn der Heater nachweislich aus ist.            */
static bool sht_heater_force_off(const char *why) {
    for (int i = 0; i < 3; i++) {
        sht85_write_cmd(SHT_CMD_HEATER_OFF);
        delay(2);
        uint16_t st;
        if (sht_status_read(&st)) {
            g_sht_status = st;
            if (!(st & SHT_STATUS_HEATER)) {
                g_sht_heater_on = false;
                g_sht_heater_stuck = false;
                return true;
            }
        }
        delay(10);
    }
    sht85_write_cmd(0x30A2);   /* Soft-Reset: Heater-Default = aus */
    delay(5);
    uint16_t st;
    if (sht_status_read(&st) && !(st & SHT_STATUS_HEATER)) {
        g_sht_status = st;
        g_sht_heater_on = false;
        g_sht_heater_stuck = false;
        Serial.print(LOG_TAG); Serial.print("heater-off via soft-reset ("); Serial.print(why); Serial.println(")");
        return true;
    }
    g_sht_heater_stuck = true;
    Serial.print(LOG_TAG); Serial.print("!!! HEATER-OFF NICHT VERIFIZIERBAR ("); Serial.print(why); Serial.println(")");
    return false;
}

/* Nach Init/Re-Init: Heater sicher aus, Status-Flags loeschen. */
static void sht_after_init(void) {
    sht_heater_force_off("init");
    sht85_write_cmd(SHT_CMD_CLR_STATUS);
    delay(1);
}


/* ================================================================== */
/* AHT20 — Fallback-Fuehler, wenn kein SHT85 gefunden wird             */
/* ================================================================== */
static bool aht20_fallback_init(void) {
    /* AHT20 sitzt auf 0x38. Wir pingen zuerst, die Library selbst
     * laesst sich nicht gut nach einem Fehler abfragen.              */
    Wire.beginTransmission(0x38);
    if (Wire.endTransmission() != 0) {
        slog("AHT20 fallback nicht gefunden (0x38)");
        return false;
    }
    aht20_fallback.begin();
    delay(40);
    float h = 0, t = 0;
    if (aht20_fallback.getSensor(&h, &t) == 0) {
        slog("AHT20 fallback: erste messung fehlgeschlagen");
        return false;
    }
    Serial.print(LOG_TAG);
    Serial.print("AHT20 fallback ready: ");
    Serial.print(t); Serial.print(" degC, ");
    Serial.print(h * 100.0f); Serial.println(" %RH");
    return true;
}


/* Eine SHT3x-Messung mit Retries + Recovery. true = frischer, gueltiger
 * Wert in *t/*rh. Schreibt NICHT mehr selbst in globale Variablen oder
 * auf den UART - das macht measure_tick(), damit ungueltige Samples
 * nie als "frisch" weiterlaufen (v0.4.0: H3 aus dem Audit).          */
static uint16_t g_sht_consec_fails = 0;
static bool sht85_read(float *t_out, float *rh_out) {
    float t = NAN, rh = NAN;
    bool ok = false;
    for (uint8_t r = 0; r < SSC_SHT85_RETRY_MAX; r++) {
        if (sht85_measure(&t, &rh)) { ok = true; break; }
        delay(8);
    }
    if (ok) {
        g_sht_consec_fails = 0;
        *t_out = t; *rh_out = rh;
        return true;
    }
    g_sht85_fails++;
    g_sht_consec_fails++;
    if ((g_sht_consec_fails % 10) == 1) {
        Serial.print(LOG_TAG);
        Serial.print("SHT3x measure failed (consecutive: ");
        Serial.print(g_sht_consec_fails);
        Serial.print(", total: ");
        Serial.print(g_sht85_fails);
        Serial.println(")");
    }
    /* Kurz-Recovery nach 5 aufeinanderfolgenden Fehlern: Soft-Reset
     * (schaltet nebenbei auch einen evtl. haengenden Heater ab).      */
    if ((g_sht_consec_fails % 5) == 0) {
        Wire.beginTransmission(g_sht_addr);
        Wire.write(0x30); Wire.write(0xA2);
        Wire.endTransmission();
        delay(5);
    }
    /* Nach 20 aufeinanderfolgenden Fehlern: Bus-Unlock + volle Re-Init. */
    if ((g_sht_consec_fails % 20) == 0) {
        slog("SHT3x recovery attempt (bus-unlock + re-init)");
        i2c_bus_unlock(20, 21);
        sht_ready = sht85_init();
        if (sht_ready) g_sht_consec_fails = 0;
    }
    return false;
}


/* ================================================================== */
/* SCD41 (intern) - CO2 + Temp + RH fuer Vorraum-Anzeige              */
/* ================================================================== */
static void sensor_scd4x_init(void) {
    uint16_t err;
    char msg[128];
    scd4x.begin(Wire, 0x62);
    scd4x.stopPeriodicMeasurement();
    /* Sensirion-Konvention: 0 = Erfolg, !=0 = Fehler. `if (!call)` hiess vorher
     * "nur beim Fehler drucken" - wir wollten aber das Serial-Log bei Erfolg. */
    uint64_t serial = 0;
    if (scd4x.getSerialNumber(serial) == 0) {
        Serial.printf("[SAUNA] SCD41 Serial: %08X%08X\n",
                      (uint32_t)(serial >> 32), (uint32_t)(serial & 0xFFFFFFFF));
    } else {
        Serial.println("[SAUNA] SCD41 getSerialNumber failed");
    }

    /* ASC (Automatic Self-Calibration) ausschalten: das Feature setzt voraus
     * dass der Sensor mindestens 1x pro Woche 400 ppm Frischluft sieht. In
     * einem geschlossenen Vorraum (Wintermonaten, geschlossene Tuer, keine
     * Lueftung) sieht er nie 400 ppm, und ASC kalibriert sich dann falsch
     * auf den woechentlichen Minimum-Wert (z.B. 700 ppm) als "neuer 400-ppm-
     * Referenzwert". Das driftet die Kalibrierung permanent. Bei Bedarf
     * manuell FRC (scd4x.performForcedRecalibration) an der frischen Luft. */
    uint16_t asc_err = scd4x.setAutomaticSelfCalibrationEnabled(false);
    if (asc_err) {
        char ascmsg[64]; errorToString(asc_err, ascmsg, sizeof(ascmsg));
        Serial.printf("[SAUNA] SCD41 ASC off failed: %s\n", ascmsg);
    } else {
        slog("SCD41 ASC disabled (closed-room drift protection)");
    }

    err = scd4x.startPeriodicMeasurement();
    if (err) {
        errorToString(err, msg, sizeof(msg));
        Serial.printf("SCD41 start err: %s\n", msg);
        scd_ready = false;
        return;
    }
    scd_ready = true;
}

static void sensor_scd4x_read_and_send(void) {
    if (!scd_ready) return;
    uint16_t co2;
    float temperature, humidity;
    uint16_t err = scd4x.readMeasurement(co2, temperature, humidity);
    if (err || co2 == 0) return;  /* still warming up */

    /* Temp-Offset fuer PCB-Abwaerme kompensieren */
    float temp_corr = temperature + SSC_SCD41_TEMP_OFFSET_C;
    sensor_data_send(PKT_TYPE_SENSOR_SCD41_CO2, (float)co2);
    sensor_data_send(PKT_TYPE_SENSOR_SHT41_TEMP, temp_corr);
    sensor_data_send(PKT_TYPE_SENSOR_SHT41_HUMIDITY, humidity);

    /* Kompensation fuer SGP40 aus SCD41-Daten ableiten */
    compensationT  = (uint16_t)((temperature + 45.0f) * 65535.0f / 175.0f);
    compensationRh = (uint16_t)(humidity * 65535.0f / 100.0f);

    /* Dev-Mode: Im SCD41-PROXY-Modus NICHT mehr die internen Werte
     * zusaetzlich als Sauna senden - das fuehrte zu doppelter Anzeige
     * (Kabine == Vorraum). Die Kabinen-Box im UI zeigt dann explizit
     * "KEIN EXTERNER FUEHLER". Vorraum bekommt die echten SCD41-Werte
     * ganz normal. Probe-State bleibt 'SCD41' gemeldet fuer das UI.  */
    /* SGP40-Kompensation: wir nutzen die UNKORRIGIERTE Temperatur,
     * weil der SGP40 direkt neben dem SCD41 sitzt (gleiches thermisches
     * Millieu). Offset ist nur fuer die UI-Anzeige gedacht.            */
}

/* ================================================================== */
/* SGP40 (intern) - TVOC-Index fuer Vorraum-Anzeige                   */
/* ================================================================== */
static void sensor_sgp40_init(void) {
    sgp40.begin(Wire);
    uint16_t testResult;
    if (sgp40.executeSelfTest(testResult) == 0 && testResult == 0xD400) {
        sgp_ready = true;
        Serial.println("SGP40 ready");
    } else {
        Serial.println("SGP40 self-test failed");
    }
}

/* Sensirion VOC-Gas-Index-Algorithmus braucht ~5 Minuten baseline-learning
 * bis sein output aussagekraeftig ist. In den ersten 5 Minuten nach boot
 * melden wir NaN (gleiche Konvention wie sauna_temp=NaN = "noch kein Wert"),
 * damit die UI nicht Muellwerte anzeigt. */
#define SGP40_WARMUP_MS 300000UL
static uint32_t g_sgp_first_ok_ms = 0;

static void sensor_sgp40_read_and_send(void) {
    if (!sgp_ready) return;
    uint16_t sraw;
    /* Sensirion-Konvention: 0=Erfolg, !=0 = Fehler. Vorher war `if (call) return`
     * verkehrt rum: wir sind auf Erfolg rausgesprungen und haben auf Fehler
     * einen uninitialisierten sraw an den VOC-Algorithmus geschickt -> Index
     * war entweder mueggel oder 0. */
    if (sgp40.measureRawSignal(compensationRh, compensationT, sraw) != 0) return;
    int32_t voc_index = voc_algorithm.process(sraw);

    if (g_sgp_first_ok_ms == 0) g_sgp_first_ok_ms = millis();
    bool warm = (millis() - g_sgp_first_ok_ms) >= SGP40_WARMUP_MS;
    float v_out = warm ? (float)voc_index : NAN;
    sensor_data_send(PKT_TYPE_SENSOR_TVOC_INDEX, v_out);
}

/* ================================================================== */
/* v0.4.0: Rekorder (Wahrheit), Auto-Modus, Protokoll v2              */
/* ================================================================== */
/* Der RP2040 besitzt die Aufzeichnung komplett: Start/Ende (manuell
 * oder automatisch durch den Detektor), Ereignisse, Statistik, Resume
 * nach Reset. Der ESP32 ist nur noch Anzeige + Metadaten + Uhr. Ein
 * ESP32-Neustart beendet deshalb keine Aufzeichnung mehr (v0.3.3-Bug:
 * das Legacy-POWER_ON 0xA4 rief session_end()).                       */

static ssc_det_t g_det;

/* ---------------- Zeit (vom ESP32 per TIME_SYNC) ------------------- */
static uint32_t g_ts_utc = 0, g_ts_ms = 0;
static int32_t  g_ts_tz  = 3600;
static uint8_t  g_ts_src = 0;
static bool     g_ts_have = false;

static bool time_valid(void) {
    /* source >= 2: NVS-Fallback, manuell oder NTP. Compile-Time reicht
     * nicht fuer Session-IDs.                                          */
    return g_ts_have && g_ts_src >= 2 && g_ts_utc > 1700000000UL;
}
static uint32_t utc_now(void) {
    if (!g_ts_have) return 0;
    return g_ts_utc + (millis() - g_ts_ms) / 1000UL;
}

/* ---------------- Simulations-/Replay-Modus (USB ?F) --------------- */
/* tools/replay_session.py speist echte CSVs mit beliebiger Geschwindig-
 * keit ein. Die Sample-Zeit kommt dann aus der Datei, nicht aus millis.
 * Endet automatisch 5 s nach dem letzten ?F.                          */
static bool     g_sim = false;
static uint32_t g_sim_t = 0;          /* aktuelle Sim-Zeit (ms)         */
static uint32_t g_sim_last_feed = 0;  /* millis() des letzten ?F        */

static uint32_t clk_ms(void) { return g_sim ? g_sim_t : millis(); }

/* ---------------- Sensor-Pflege-Zustand ---------------------------- */
enum { ST_IDLE = 0, ST_BASE, ST_HEAT, ST_SETTLE };
static uint8_t  g_st_phase = ST_IDLE;
static uint32_t g_st_phase_ms = 0;
static uint8_t  g_st_result = SSC_ST_NONE, g_st_reason = 0;
static uint16_t g_st_k_x100 = 0;
static uint32_t g_st_last_ms = 0;      /* 0 = noch nie                   */
static float    g_st_t0 = NAN, g_st_rh0 = NAN, g_st_tend = NAN, g_st_rhend = NAN;
static float    g_st_acc_t = 0, g_st_acc_rh = 0; static uint16_t g_st_acc_n = 0;

/* Letzter gueltiger Sauna-Messwert (fuer Anzeige/Interlocks) */
static uint32_t g_sauna_ok_ms = 0;     /* millis() der letzten gueltigen Messung */

/* ---------------- Rekorder-Zustand --------------------------------- */
#define SSC_MAX_EVENTS 64
struct rec_state_t {
    bool     active, file_ok, resumed, sim, resume_checked;
    char     id[SSC_ID_LEN];
    char     path[44];
    File     f;
    uint8_t  origin;
    uint32_t start_utc;       /* 0 = unbekannt                        */
    uint32_t t0_clk, t_off;   /* t_ms = t_off + (clk_ms() - t0_clk)    */
    uint32_t rows;
    uint16_t write_errs;
    uint32_t last_flush_ms, last_save_ms, last_probe_ms;
    float    peak_t, peak_rh;
    uint8_t  gang, n_aufguss, n_luef;
    uint8_t  n_ev;
    ssc_event_rec_t        ev[SSC_MAX_EVENTS];
    ssc_event_label_body_t lbl[SSC_MAX_EVENTS];
    ssc_meta_wire_t user;     /* User-Felder (vom ESP32 per META_PUSH) */
    bool     have_user;
    char     marker[24];      /* naechstes CSV-Event-Token             */
};
static rec_state_t g_rec;

/* Hilfs-Puffer fuer Events vergangener Sessions (GET_EVENTS/LABEL). */
static ssc_event_rec_t        g_tmp_ev[SSC_MAX_EVENTS];
static ssc_event_label_body_t g_tmp_lbl[SSC_MAX_EVENTS];

static uint8_t  g_auto_enabled = 1;    /* persistiert in /ssc_auto.cfg   */
static bool     g_state_dirty = true;
static uint16_t g_bad_len = 0, g_bad_crc = 0;
static uint16_t g_uart_ovf = 0;

static void rt_service(void);          /* 2-Hz-Takt aus langen Schleifen */
static void send_session_state(void);
static void sensor_health_send(void);

static uint32_t rec_t_ms(void) { return g_rec.t_off + (clk_ms() - g_rec.t0_clk); }

static void send_ack(uint8_t cmd, uint8_t rc, uint8_t extra) {
    ssc_ack_t a = { cmd, rc, extra };
    uint8_t pkt[1 + sizeof(a)];
    pkt[0] = SSC_PKT_ACK;
    memcpy(pkt + 1, &a, sizeof(a));
    raw_packet_send(pkt, sizeof(pkt));
}

static void set_marker(const char *m) {
    /* mehrere Events im selben Sample: mit ';' verketten */
    size_t l = strlen(g_rec.marker);
    if (l && l + 1 < sizeof(g_rec.marker)) { g_rec.marker[l++] = ';'; g_rec.marker[l] = 0; }
    strncat(g_rec.marker, m, sizeof(g_rec.marker) - 1 - strlen(g_rec.marker));
}

/* Strings fuer CSV/.evt/JSON entschaerfen. */
static void sanitize(char *s, size_t n) {
    for (size_t i = 0; i < n && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ',' ) s[i] = ';';
        else if (c == '"' || c == '\\') s[i] = '\'';
        else if (c < 0x20) s[i] = ' ';
    }
    s[n - 1] = 0;
}

/* ---------------- SD-Grundfunktionen ------------------------------- */
static void rec_sd_fail(const char *why) {
    Serial.print(LOG_TAG); Serial.print("SD-Schreibfehler: "); Serial.println(why);
    if (g_rec.f) g_rec.f.close();
    g_rec.f = File();
    g_rec.file_ok = false;
    if (g_rec.write_errs < 0xFFFF) g_rec.write_errs++;
    sd_init_flag = false;     /* loop() mountet neu (SD.end + begin) */
    g_state_dirty = true;
}

static bool rec_write(const char *buf, size_t n) {
    if (!g_rec.file_ok || !g_rec.f) return false;
    size_t w = g_rec.f.write((const uint8_t *)buf, n);
    if (w != n) { rec_sd_fail("write"); return false; }
    return true;
}

static void active_file_save(void) {
    if (!g_rec.active || !sd_init_flag) return;
    SD.remove("/ssc_active.tmp");   /* FILE_WRITE haengt an - frisch anlegen */
    File a = SD.open("/ssc_active.tmp", FILE_WRITE);
    if (!a) return;
    char b[160];
    int n = snprintf(b, sizeof(b), "%s\n%lu\n%lu\n%u\n%u %u %u %.2f %.2f\n%u\n",
                     g_rec.id, (unsigned long)g_rec.start_utc,
                     (unsigned long)rec_t_ms(), (unsigned)g_rec.origin,
                     (unsigned)g_rec.gang, (unsigned)g_rec.n_aufguss,
                     (unsigned)g_rec.n_luef, (double)g_rec.peak_t,
                     (double)g_rec.peak_rh, (unsigned)g_rec.sim);
    bool ok = n > 0 && a.write((const uint8_t *)b, (size_t)n) == (size_t)n;
    a.close();
    /* atomar ersetzen: tmp -> active */
    if (ok) {
        SD.remove("/ssc_active.txt");
        SD.rename("/ssc_active.tmp", "/ssc_active.txt");
    }
}

/* ---------------- .evt-Datei --------------------------------------- */
static void evt_path(const char *id, char *p, size_t n) {
    snprintf(p, n, "/sessions/%s.evt", id);
}

static int evt_write(const char *id, const ssc_event_rec_t *ev,
                     const ssc_event_label_body_t *lb, uint8_t n) {
    if (!sd_init_flag) return -1;
    char p[48]; evt_path(id, p, sizeof(p));
    char tp[48]; snprintf(tp, sizeof(tp), "/sessions/%s.evx", id);
    SD.remove(tp);
    File f = SD.open(tp, FILE_WRITE);
    if (!f) return -2;
    bool ok = true;
    for (uint8_t i = 0; i < n && ok; i++) {
        const ssc_event_rec_t *e = &ev[i];
        ssc_event_label_body_t l = lb[i];
        sanitize(l.operator_tag, sizeof(l.operator_tag));
        sanitize(l.name, sizeof(l.name));
        sanitize(l.aroma, sizeof(l.aroma));
        char b[220];
        int k = snprintf(b, sizeof(b),
            "%u,%u,%u,%u,%u,%lu,%lu,%lu,%d,%d,%u,%u,%u,%u,%u,%s,%s,%s\n",
            e->no, e->kind, e->flags, e->gang, e->seq,
            (unsigned long)e->t_start_ms, (unsigned long)e->t_peak_ms,
            (unsigned long)e->t_end_ms, e->t0_x10, e->tpk_x10,
            e->rh0_x10, e->rhpk_x10, e->ah0_x10, e->ahpk_x10,
            l.participants, l.operator_tag, l.name, l.aroma);
        ok = k > 0 && f.write((const uint8_t *)b, (size_t)k) == (size_t)k;
    }
    f.close();
    if (!ok) return -3;
    SD.remove(p);
    return SD.rename(tp, p) ? 0 : -4;
}

/* Liest <id>.evt. Rueckgabe Anzahl Events oder <0. */
static int evt_read(const char *id, ssc_event_rec_t *ev, ssc_event_label_body_t *lb) {
    if (!sd_init_flag) return -1;
    char p[48]; evt_path(id, p, sizeof(p));
    File f = SD.open(p, FILE_READ);
    if (!f) {   /* Strom weg zwischen remove und rename: .evx ist aktuell */
        snprintf(p, sizeof(p), "/sessions/%s.evx", id);
        f = SD.open(p, FILE_READ);
    }
    if (!f) return -2;
    int n = 0;
    char line[224];
    while (f.available() && n < SSC_MAX_EVENTS) {
        size_t len = 0;
        while (f.available() && len < sizeof(line) - 1) {
            int c = f.read();
            if (c < 0 || c == '\n') break;
            line[len++] = (char)c;
        }
        line[len] = 0;
        if (len < 10) continue;
        unsigned a[15]; unsigned long ts[3]; int t0, tpk;
        int pos = 0;
        int got = sscanf(line, "%u,%u,%u,%u,%u,%lu,%lu,%lu,%d,%d,%u,%u,%u,%u,%u,%n",
                         &a[0], &a[1], &a[2], &a[3], &a[4], &ts[0], &ts[1], &ts[2],
                         &t0, &tpk, &a[5], &a[6], &a[7], &a[8], &a[9], &pos);
        if (got < 15 || pos <= 0) continue;
        ssc_event_rec_t *e = &ev[n];
        memset(e, 0, sizeof(*e));
        e->no = (uint8_t)a[0]; e->kind = (uint8_t)a[1]; e->flags = (uint8_t)a[2];
        e->gang = (uint8_t)a[3]; e->seq = (uint8_t)a[4];
        e->t_start_ms = ts[0]; e->t_peak_ms = ts[1]; e->t_end_ms = ts[2];
        e->t0_x10 = (int16_t)t0; e->tpk_x10 = (int16_t)tpk;
        e->rh0_x10 = (uint16_t)a[5]; e->rhpk_x10 = (uint16_t)a[6];
        e->ah0_x10 = (uint16_t)a[7]; e->ahpk_x10 = (uint16_t)a[8];
        ssc_event_label_body_t *l = &lb[n];
        memset(l, 0, sizeof(*l));
        l->participants = (uint8_t)a[9];
        /* drei Strings, kommagetrennt */
        char *s = line + pos;
        char *fld[3] = { l->operator_tag, l->name, l->aroma };
        size_t cap[3] = { sizeof(l->operator_tag), sizeof(l->name), sizeof(l->aroma) };
        for (int k = 0; k < 3; k++) {
            char *c = strchr(s, ',');
            size_t m = c ? (size_t)(c - s) : strlen(s);
            if (m >= cap[k]) m = cap[k] - 1;
            memcpy(fld[k], s, m); fld[k][m] = 0;
            if (!c) break;
            s = c + 1;
        }
        n++;
        if ((n & 7) == 0) rt_service();
    }
    f.close();
    return n;
}

/* ---------------- JSON-Sidecar ------------------------------------- */
static void json_extract_str(const char *src, const char *key, char *dst, size_t dst_sz) {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(src, needle);
    if (!p) return;
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != '"' && i < dst_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        dst[i++] = *p++;
    }
    dst[i] = 0;
}
static long json_extract_long(const char *src, const char *key, long def) {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(src, needle);
    if (!p) return def;
    p += strlen(needle);
    while (*p == ' ') p++;
    return atol(p);
}
static float json_extract_float(const char *src, const char *key, float def) {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(src, needle);
    if (!p) return def;
    p += strlen(needle);
    while (*p == ' ') p++;
    return (float)atof(p);
}

/* Liest <id>.json. measured=true wenn der RP2040 (v0.4+) die Messwerte
 * selbst geschrieben hat ("fw"-Key vorhanden).                          */
static int read_session_json(const char *id, ssc_meta_wire_t *out, bool *measured) {
    if (!sd_init_flag) return -1;
    char path[48];
    snprintf(path, sizeof(path), "/sessions/%s.json", id);
    File f = SD.open(path, FILE_READ);
    if (!f) return -2;
    static char buf[900];
    size_t n = f.read((uint8_t *)buf, sizeof(buf) - 1);
    f.close();
    if (n == 0) return -3;
    buf[n] = 0;
    memset(out, 0, sizeof(*out));
    strncpy(out->id, id, sizeof(out->id) - 1);
    out->start_ts      = (int32_t)json_extract_long(buf, "start_ts", 0);
    out->end_ts        = (int32_t)json_extract_long(buf, "end_ts", 0);
    out->participants  = (uint16_t)json_extract_long(buf, "participants", 0);
    out->peak_temp     = json_extract_float(buf, "peak_temp", 0.0f);
    out->peak_rh       = json_extract_float(buf, "peak_rh", 0.0f);
    out->aufguss_count = (uint8_t)json_extract_long(buf, "aufguss_count", 0);
    json_extract_str(buf, "operator", out->operator_tag, sizeof(out->operator_tag));
    json_extract_str(buf, "aufguss_headline", out->aufguss_headline, sizeof(out->aufguss_headline));
    json_extract_str(buf, "notes", out->notes, sizeof(out->notes));
    if (measured) *measured = strstr(buf, "\"fw\":") != NULL;
    return 0;
}

/* Schreibt <id>.json atomar (tmp + rename). m = vollstaendiger Record.
 * extra_* nur fuer v0.4-Felder (Gaenge etc.), <0 = weglassen.          */
static int write_session_json(const ssc_meta_wire_t *m, int gaenge, int lueften,
                              int origin, int sim, uint32_t rows, uint16_t werrs) {
    if (!sd_init_flag) return -1;
    char path[48], tmp[48];
    snprintf(path, sizeof(path), "/sessions/%s.json", m->id);
    snprintf(tmp, sizeof(tmp), "/sessions/%s.jsx", m->id);
    ssc_meta_wire_t c = *m;
    c.id[sizeof(c.id) - 1] = 0;
    c.operator_tag[sizeof(c.operator_tag) - 1] = 0;
    c.aufguss_headline[sizeof(c.aufguss_headline) - 1] = 0;
    c.notes[sizeof(c.notes) - 1] = 0;
    sanitize(c.operator_tag, sizeof(c.operator_tag));
    sanitize(c.aufguss_headline, sizeof(c.aufguss_headline));
    sanitize(c.notes, sizeof(c.notes));
    static char buf[900];
    int n = snprintf(buf, sizeof(buf),
        "{\"id\":\"%s\",\"start_ts\":%ld,\"end_ts\":%ld,\"operator\":\"%s\","
        "\"aufguss_headline\":\"%s\",\"participants\":%u,\"notes\":\"%s\","
        "\"peak_temp\":%.2f,\"peak_rh\":%.2f,\"aufguss_count\":%u",
        c.id, (long)c.start_ts, (long)c.end_ts, c.operator_tag, c.aufguss_headline,
        (unsigned)c.participants, c.notes, (double)c.peak_temp, (double)c.peak_rh,
        (unsigned)c.aufguss_count);
    if (n < 0 || n >= (int)sizeof(buf)) return -5;
    if (gaenge >= 0) {
        int k = snprintf(buf + n, sizeof(buf) - n,
            ",\"gaenge\":%d,\"lueften\":%d,\"origin\":%d,\"sim\":%d,\"rows\":%lu,"
            "\"write_errs\":%u,\"fw\":\"%s\",\"det\":%d",
            gaenge, lueften, origin, sim, (unsigned long)rows, (unsigned)werrs,
            VERSION, SSC_DET_VERSION);
        if (k < 0 || k >= (int)(sizeof(buf) - n)) return -5;
        n += k;
    }
    if (n + 3 >= (int)sizeof(buf)) return -5;
    buf[n++] = '}'; buf[n++] = '\n';
    SD.remove(tmp);
    File f = SD.open(tmp, FILE_WRITE);
    if (!f) return -2;
    bool ok = f.write((const uint8_t *)buf, (size_t)n) == (size_t)n;
    f.close();
    if (!ok) return -3;
    SD.remove(path);
    return SD.rename(tmp, path) ? 0 : -4;
}

/* Messwerte der laufenden/gerade beendeten Session in einen Record. */
static void rec_fill_measured(ssc_meta_wire_t *m, bool final_) {
    strncpy(m->id, g_rec.id, sizeof(m->id) - 1);
    m->start_ts = (int32_t)g_rec.start_utc;
    m->end_ts   = g_rec.start_utc ? (int32_t)(g_rec.start_utc + rec_t_ms() / 1000UL) : 0;
    (void)final_;
    m->peak_temp = g_rec.peak_t > -100.0f ? g_rec.peak_t : 0.0f;
    m->peak_rh   = g_rec.peak_rh > -100.0f ? g_rec.peak_rh : 0.0f;
    uint8_t na = 0;
    for (uint8_t i = 0; i < g_rec.n_ev; i++)
        if (g_rec.ev[i].kind == SSC_EK_AUFGUSS && !(g_rec.ev[i].flags & SSC_EVF_DELETED)) na++;
    m->aufguss_count = na;
}

static void rec_write_json(bool final_) {
    ssc_meta_wire_t m;
    memset(&m, 0, sizeof(m));
    if (g_rec.have_user) m = g_rec.user;
    rec_fill_measured(&m, final_);
    int rc = write_session_json(&m, g_rec.gang, g_rec.n_luef, g_rec.origin,
                                g_rec.sim, g_rec.rows, g_rec.write_errs);
    if (rc != 0) { Serial.print(LOG_TAG); Serial.print("json write rc="); Serial.println(rc); }
}

/* ---------------- Events ------------------------------------------- */
static void event_send_live(const ssc_event_rec_t *e) {
    ssc_event_msg_t m;
    memset(&m, 0, sizeof(m));
    strncpy(m.id, g_rec.id, sizeof(m.id) - 1);
    m.e = *e;
    uint8_t pkt[1 + sizeof(m)];
    pkt[0] = SSC_PKT_EVENT;
    memcpy(pkt + 1, &m, sizeof(m));
    raw_packet_send(pkt, sizeof(pkt));
}

static int16_t x10s(float v) { return isnan(v) ? 0 : (int16_t)lroundf(v * 10.0f); }
static uint16_t x10u(float v) { return (isnan(v) || v < 0) ? 0 : (uint16_t)lroundf(v * 10.0f); }

static ssc_event_rec_t *rec_new_event(uint8_t kind) {
    if (g_rec.n_ev >= SSC_MAX_EVENTS) return NULL;
    uint8_t i = g_rec.n_ev++;
    memset(&g_rec.ev[i], 0, sizeof(g_rec.ev[i]));
    memset(&g_rec.lbl[i], 0, sizeof(g_rec.lbl[i]));
    g_rec.ev[i].no = (uint8_t)(i + 1);
    g_rec.ev[i].kind = kind;
    g_rec.ev[i].gang = g_rec.gang;
    return &g_rec.ev[i];
}

static void rec_event_commit(ssc_event_rec_t *e, const char *token) {
    set_marker(token);
    event_send_live(e);
    evt_write(g_rec.id, g_rec.ev, g_rec.lbl, g_rec.n_ev);
    active_file_save();
    g_state_dirty = true;
    Serial.print(LOG_TAG); Serial.print("event "); Serial.print(token);
    Serial.print(" no="); Serial.println(e->no);
}

/* Detektor-Zeiten (clk) in Session-Zeit (t_ms) umrechnen. */
static uint32_t det_to_t(uint32_t clk) {
    int32_t d = (int32_t)(clk - g_rec.t0_clk);
    int64_t t = (int64_t)g_rec.t_off + d;
    return t < 0 ? 0 : (uint32_t)t;
}

static void fill_from_det(ssc_event_rec_t *r, const ssc_det_event_t *e) {
    r->t_start_ms = det_to_t(e->t_start_ms);
    r->t_peak_ms  = det_to_t(e->t_peak_ms);
    r->t_end_ms   = det_to_t(e->t_end_ms);
    r->t0_x10 = x10s(e->t0);   r->tpk_x10 = x10s(e->tpk);
    r->rh0_x10 = x10u(e->rh0); r->rhpk_x10 = x10u(e->rhpk);
    r->ah0_x10 = x10u(e->ah0); r->ahpk_x10 = x10u(e->ahpk);
}

static void rec_on_aufguss(const ssc_det_event_t *de, bool manual, float T, float RH) {
    ssc_event_rec_t *r = rec_new_event(SSC_EK_AUFGUSS);
    if (!r) return;
    if (de) fill_from_det(r, de);
    else {
        uint32_t t = rec_t_ms();
        r->t_start_ms = r->t_peak_ms = r->t_end_ms = t;
        r->t0_x10 = r->tpk_x10 = x10s(T);
        r->rh0_x10 = r->rhpk_x10 = x10u(RH);
        r->ah0_x10 = r->ahpk_x10 = x10u(ssc_abs_humidity(T, RH));
    }
    if (manual) r->flags |= SSC_EVF_MANUAL;
    r->seq = ++g_rec.n_aufguss;
    /* Vorbelegung: Labels vom vorigen Aufguss im selben Gang uebernehmen
     * (der Saunameister macht meist alle Kellen eines Gangs). Nicht als
     * LABELED markiert - die UI zeigt "vorbelegt, bitte bestaetigen".  */
    uint8_t idx = (uint8_t)(r->no - 1);
    for (int i = (int)idx - 1; i >= 0; i--) {
        if (g_rec.ev[i].kind == SSC_EK_AUFGUSS && g_rec.ev[i].gang == r->gang) {
            g_rec.lbl[idx] = g_rec.lbl[i];
            break;
        }
    }
    char tok[12]; snprintf(tok, sizeof(tok), "A%u", r->seq);
    rec_event_commit(r, tok);
}

/* ---------------- Session Start/Ende -------------------------------- */
static void make_session_id(char *out, bool sim) {
    if (time_valid()) {
        time_t lt = (time_t)((int64_t)utc_now() + g_ts_tz);
        struct tm tmv;
        gmtime_r(&lt, &tmv);
        snprintf(out, SSC_ID_LEN, "%c%04d%02d%02d_%02d%02d%02d", sim ? 'X' : 'S',
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        /* Uhr unbekannt: eindeutig ueber Boot-Zaehler + Uptime */
        snprintf(out, SSC_ID_LEN, "%c%05lu_%06lu", sim ? 'Y' : 'U',
                 (unsigned long)(g_boot_count % 100000UL),
                 (unsigned long)((millis() / 1000UL) % 1000000UL));
    }
}

static bool session_ensure_dir(void) {
    if (!sd_init_flag) return false;
    if (SD.exists("/sessions")) {
        File d = SD.open("/sessions");
        if (!d) {
            SD.rmdir("/sessions");
            return SD.mkdir("/sessions");
        }
        d.close();
        return true;
    }
    return SD.mkdir("/sessions");
}

static void rec_reset_counters(void) {
    g_rec.rows = 0; g_rec.write_errs = 0;
    g_rec.peak_t = g_rec.peak_rh = -1000.0f;
    g_rec.gang = 0; g_rec.n_aufguss = 0; g_rec.n_luef = 0; g_rec.n_ev = 0;
    g_rec.have_user = false;
    memset(&g_rec.user, 0, sizeof(g_rec.user));
    g_rec.marker[0] = 0;
}

/* Startet eine neue Session. Rueckgabe SSC_RC_*. Existierende Dateien
 * werden NIE geloescht - bei ID-Kollision gibt es ein Suffix _2.._9.  */
static uint8_t rec_start(uint8_t origin) {
    if (g_rec.active) return SSC_RC_STATE;
    if (!sd_init_flag || !session_ensure_dir()) return SSC_RC_SD_ERR;
    char id[SSC_ID_LEN];
    make_session_id(id, g_sim);
    char path[44];
    snprintf(path, sizeof(path), "/sessions/%s.csv", id);
    if (SD.exists(path)) {
        size_t bl = strlen(id);
        bool found = false;
        for (char s = '2'; s <= '9' && !found; s++) {
            id[bl] = '_'; id[bl + 1] = s; id[bl + 2] = 0;
            snprintf(path, sizeof(path), "/sessions/%s.csv", id);
            if (!SD.exists(path)) found = true;
        }
        if (!found) return SSC_RC_STATE;
    }
    File f = SD.open(path, FILE_WRITE);
    if (!f) return SSC_RC_SD_ERR;
    const char *hdr = "t_ms,temp,rh,q,event\n";
    if (f.write((const uint8_t *)hdr, strlen(hdr)) != strlen(hdr)) { f.close(); return SSC_RC_SD_ERR; }
    f.flush();

    rec_reset_counters();
    g_rec.f = f;
    g_rec.file_ok = true;
    g_rec.active = true;
    g_rec.resumed = false;
    g_rec.resume_checked = true;
    g_rec.sim = g_sim;
    g_rec.origin = origin;
    strncpy(g_rec.id, id, sizeof(g_rec.id) - 1);
    g_rec.id[sizeof(g_rec.id) - 1] = 0;
    strncpy(g_rec.path, path, sizeof(g_rec.path) - 1);
    g_rec.start_utc = time_valid() ? utc_now() : 0;
    g_rec.t0_clk = clk_ms();
    g_rec.t_off = 0;
    g_rec.last_flush_ms = g_rec.last_save_ms = g_rec.last_probe_ms = millis();
    ssc_det_session_begin(&g_det, clk_ms(), origin == SSC_ORIGIN_MANUAL);
    if (sht_ready) sht_heater_force_off("session-start");
    set_marker(origin == SSC_ORIGIN_AUTO ? "START_AUTO" : "START");
    rec_write_json(false);
    active_file_save();
    g_state_dirty = true;
    Serial.print(LOG_TAG); Serial.print("session start: "); Serial.print(g_rec.id);
    Serial.print(" origin="); Serial.println(origin);
    return SSC_RC_OK;
}

static void rec_handle_det_events(ssc_det_event_t *ev, int n, float T, float RH);

static void rec_end(uint8_t reason) {
    if (!g_rec.active) return;
    /* offene Aufguesse / Lueften abschliessen */
    ssc_det_event_t fe[4];
    int nf = ssc_det_finish(&g_det, fe, 4);
    rec_handle_det_events(fe, nf, NAN, NAN);
    ssc_det_session_end(&g_det);
    for (uint8_t i = 0; i < g_rec.n_ev; i++) g_rec.ev[i].flags &= (uint8_t)~SSC_EVF_OPEN;
    char tok[12]; snprintf(tok, sizeof(tok), reason == SSC_END_AUTO ? "END_AUTO" : "END");
    set_marker(tok);
    /* letzte Zeile mit Marker schreiben (ohne Messwert) */
    char b[48];
    int k = snprintf(b, sizeof(b), "%lu,,,0,%s\n", (unsigned long)rec_t_ms(), g_rec.marker);
    if (k > 0) rec_write(b, (size_t)k);
    g_rec.marker[0] = 0;
    if (g_rec.f) { g_rec.f.flush(); g_rec.f.close(); }
    g_rec.f = File();
    g_rec.file_ok = false;
    if (sht_ready) sht_heater_force_off("session-end");
    evt_write(g_rec.id, g_rec.ev, g_rec.lbl, g_rec.n_ev);
    rec_write_json(true);
    SD.remove("/ssc_active.txt");
    SD.remove("/ssc_active.tmp");
    Serial.print(LOG_TAG); Serial.print("session end: "); Serial.print(g_rec.id);
    Serial.print(" reason="); Serial.print(reason);
    Serial.print(" rows="); Serial.println(g_rec.rows);
    g_rec.active = false;
    g_state_dirty = true;
}

/* Beim Boot: unterbrochene Session fortsetzen (WDT, Brownout, Strom). */
static void rec_try_resume(void) {
    if (!sd_init_flag) return;
    File a = SD.open("/ssc_active.txt", FILE_READ);
    /* Strom weg genau zwischen remove und rename: tmp ist dann aktuell */
    if (!a) a = SD.open("/ssc_active.tmp", FILE_READ);
    if (!a) return;
    char buf[200];
    size_t n = a.read((uint8_t *)buf, sizeof(buf) - 1);
    a.close();
    buf[n] = 0;
    char id[SSC_ID_LEN] = {0};
    unsigned long start_utc = 0, t_ms = 0;
    unsigned origin = 0, gang = 0, na = 0, nl = 0, sim = 0;
    float pt = -1000, prh = -1000;
    int got = sscanf(buf, "%23s %lu %lu %u %u %u %u %f %f %u",
                     id, &start_utc, &t_ms, &origin, &gang, &na, &nl, &pt, &prh, &sim);
    char path[44];
    snprintf(path, sizeof(path), "/sessions/%s.csv", id);
    if (got < 4 || !ssc_id_valid(id, SSC_ID_LEN) || !SD.exists(path) || sim) {
        Serial.print(LOG_TAG); Serial.println("resume: verworfen (ungueltig/sim)");
        SD.remove("/ssc_active.txt");
        SD.remove("/ssc_active.tmp");
        return;
    }
    /* Letzte Zeitmarke aus dem CSV-Ende lesen: _active.txt wird nur
     * jede Minute geschrieben, die CSV alle 5 s geflusht. So bleibt
     * t_ms nach dem Resume monoton (v0.4.0-Test: sprang sonst zurueck). */
    {
        File r = SD.open(path, FILE_READ);
        if (r) {
            uint32_t sz = r.size();
            r.seek(sz > 300 ? sz - 300 : 0);
            char ln[96]; size_t l = 0;
            while (r.available()) {
                int c = r.read();
                if (c < 0) break;
                if (c == '\n') {
                    ln[l] = 0;
                    if (l > 0 && ln[0] >= '0' && ln[0] <= '9') {
                        unsigned long v = strtoul(ln, NULL, 10);
                        if (v > t_ms) t_ms = v;
                    }
                    l = 0;
                } else if (l < sizeof(ln) - 1) ln[l++] = (char)c;
            }
            r.close();
        }
    }
    File f = SD.open(path, FILE_WRITE);   /* Append-Modus */
    if (!f) return;
    rec_reset_counters();
    {   /* Events/Metadaten laden BEVOR active gesetzt wird (evt_read ruft
         * rt_service - dort duerfen noch keine Zeilen geschrieben werden). */
        int ne0 = evt_read(id, g_rec.ev, g_rec.lbl);
        g_rec.n_ev = ne0 > 0 ? (uint8_t)ne0 : 0;
        for (uint8_t i = 0; i < g_rec.n_ev; i++) g_rec.ev[i].flags &= (uint8_t)~SSC_EVF_OPEN;
    }
    g_rec.f = f;
    g_rec.file_ok = true;
    g_rec.active = true;
    g_rec.resumed = true;
    g_rec.resume_checked = false;
    g_rec.sim = false;
    g_rec.origin = (uint8_t)origin;
    strncpy(g_rec.id, id, sizeof(g_rec.id) - 1);
    strncpy(g_rec.path, path, sizeof(g_rec.path) - 1);
    g_rec.start_utc = (uint32_t)start_utc;
    g_rec.t0_clk = clk_ms();
    g_rec.t_off = (uint32_t)t_ms + 1000UL;
    g_rec.gang = (uint8_t)gang; g_rec.n_aufguss = (uint8_t)na; g_rec.n_luef = (uint8_t)nl;
    g_rec.peak_t = pt; g_rec.peak_rh = prh;
    ssc_meta_wire_t um; bool meas;
    if (read_session_json(id, &um, &meas) == 0) { g_rec.user = um; g_rec.have_user = true; }
    g_rec.last_flush_ms = g_rec.last_save_ms = g_rec.last_probe_ms = millis();
    ssc_det_session_begin(&g_det, clk_ms(), g_rec.origin == SSC_ORIGIN_MANUAL);
    g_det.gang = g_rec.gang; g_det.n_pour = g_rec.n_aufguss; g_det.n_luef = g_rec.n_luef;
    if (g_rec.gang > 0) g_det.phase = SSC_PH_GANG;
    g_det.was_hot = pt >= 55.0f;
    ssc_event_rec_t *r = rec_new_event(SSC_EK_RESUME);
    if (r) {
        r->t_start_ms = r->t_peak_ms = r->t_end_ms = g_rec.t_off;
        rec_event_commit(r, "RESUME");
    }
    Serial.print(LOG_TAG); Serial.print("session RESUME: "); Serial.print(id);
    Serial.print(" t_ms="); Serial.println(g_rec.t_off);
}

/* Nach TIME_SYNC: Luecke eines Resumes auf echte Zeit korrigieren. */
static void rec_check_resume_gap(void) {
    /* Nur mit wirklich bekannter Zeit (manuell gesetzt oder NTP). Die
     * NVS-Fallback-Zeit des ESP32 kennt den Stromausfall selbst nicht
     * und wuerde die Luecke als 0 messen (Review H1).                  */
    if (!g_rec.active || g_rec.resume_checked || !time_valid() || g_ts_src < 3) return;
    g_rec.resume_checked = true;
    uint32_t now_utc = utc_now();
    if (g_rec.start_utc == 0 || now_utc < g_rec.start_utc) return;
    uint64_t real64 = (uint64_t)(now_utc - g_rec.start_utc) * 1000ULL;
    uint32_t cur = rec_t_ms();
    if (real64 > (uint64_t)cur + 5000ULL) {
        uint64_t gap64 = real64 - cur;
        uint32_t gap = gap64 > 0xF0000000ULL ? 0xF0000000UL : (uint32_t)gap64;
        if (gap > 2UL * 3600000UL) {
            /* Strom war Stunden weg: alte Session abschliessen */
            Serial.print(LOG_TAG); Serial.println("resume: Luecke > 2 h -> Session beenden");
            rec_end(SSC_END_AUTO);
            return;
        }
        g_rec.t_off += gap;
        char tok[16]; snprintf(tok, sizeof(tok), "GAP%lus", (unsigned long)(gap / 1000UL));
        set_marker(tok);
    }
}

/* ---------------- Detektor-Events -> Rekorder ----------------------- */
static void rec_handle_det_events(ssc_det_event_t *ev, int n, float T, float RH) {
    for (int i = 0; i < n; i++) {
        ssc_det_event_t *e = &ev[i];
        switch (e->kind) {
        case SSC_EV_AUTO_START:
            if (!g_rec.active && g_auto_enabled && (g_sauna_probe == SAUNA_PROBE_SHT85 || g_sim)) {
                uint8_t rc = rec_start(SSC_ORIGIN_AUTO);
                if (rc != SSC_RC_OK) { Serial.print(LOG_TAG); Serial.print("auto-start rc="); Serial.println(rc); }
            }
            break;
        case SSC_EV_AUTO_END:
            /* Auto-Ende bei Auto-Modus/Auto-Session - und immer, wenn der
             * Fuehler seit 30 min nichts liefert (t0 = NaN, Review H3). */
            if (g_rec.active && (g_auto_enabled || g_rec.origin == SSC_ORIGIN_AUTO || isnan(e->t0))) rec_end(SSC_END_AUTO);
            break;
        case SSC_EV_GANG_START:
            if (!g_rec.active) break;
            g_rec.gang = e->gang;
            { ssc_event_rec_t *r = rec_new_event(SSC_EK_GANG);
              if (r) { fill_from_det(r, e); r->seq = e->gang; r->gang = e->gang;
                       char tok[8]; snprintf(tok, sizeof(tok), "G%u", e->gang); rec_event_commit(r, tok); } }
            break;
        case SSC_EV_AUFGUSS:
            if (g_rec.active) rec_on_aufguss(e, false, T, RH);
            break;
        case SSC_EV_LUEFTEN_START:
            if (!g_rec.active) break;
            { ssc_event_rec_t *r = rec_new_event(SSC_EK_LUEFTEN);
              if (r) { fill_from_det(r, e); r->seq = ++g_rec.n_luef; r->flags |= SSC_EVF_OPEN;
                       char tok[8]; snprintf(tok, sizeof(tok), "L%u+", r->seq); rec_event_commit(r, tok); } }
            break;
        case SSC_EV_LUEFTEN_END:
            if (!g_rec.active) break;
            for (int k = (int)g_rec.n_ev - 1; k >= 0; k--) {
                ssc_event_rec_t *r = &g_rec.ev[k];
                if (r->kind == SSC_EK_LUEFTEN && (r->flags & SSC_EVF_OPEN)) {
                    r->flags &= (uint8_t)~SSC_EVF_OPEN;
                    r->t_end_ms = det_to_t(e->t_end_ms);
                    r->t_peak_ms = det_to_t(e->t_peak_ms);
                    r->tpk_x10 = x10s(e->tpk);
                    r->rhpk_x10 = x10u(e->rhpk);
                    r->ahpk_x10 = x10u(e->ahpk);
                    char tok[8]; snprintf(tok, sizeof(tok), "L%u-", r->seq);
                    rec_event_commit(r, tok);
                    break;
                }
            }
            break;
        case SSC_EV_RH_SUSPECT:
            if (g_rec.active) {
                ssc_event_rec_t *r = rec_new_event(SSC_EK_SENSOR);
                if (r) { fill_from_det(r, e); rec_event_commit(r, "RH_SUSPECT"); }
            }
            Serial.print(LOG_TAG); Serial.println("!!! Feuchtekanal reagiert nicht (RH_SUSPECT)");
            g_state_dirty = true;
            break;
        case SSC_EV_RH_OK:
            g_state_dirty = true;
            break;
        }
    }
}

/* ---------------- Ein Sample verarbeiten ---------------------------- */
#define Q_HEATER   0x01   /* Selbsttest heizt / klingt ab: RH/T verfaelscht */
#define Q_RH_SUSP  0x02
#define Q_RH_WET   0x04   /* RH >= 98 %: evtl. Wasser am Fuehler          */
#define Q_SIM      0x08

static void process_sample(uint32_t t_clk, float T, float RH, bool valid) {
    uint8_t q = 0;
    /* Heizer betrifft nur den echten Fuehler, nicht Replay-Daten */
    bool heat = !g_sim && (g_st_phase == ST_HEAT || g_st_phase == ST_SETTLE);
    if (heat) q |= Q_HEATER;
    if (g_det.rh_suspect) q |= Q_RH_SUSP;
    if (valid && RH >= 98.0f) q |= Q_RH_WET;
    if (g_sim) q |= Q_SIM;

    ssc_det_event_t ev[8];
    int n = ssc_det_update(&g_det, t_clk, T, RH, valid && !heat, ev, 8);
    if (n > 0) rec_handle_det_events(ev, n, T, RH);

    if (!g_rec.active) return;
    if (valid && !heat) {
        if (T > g_rec.peak_t) g_rec.peak_t = T;
        if (RH > g_rec.peak_rh) g_rec.peak_rh = RH;
    }
    char b[64];
    int k;
    unsigned long t = (unsigned long)rec_t_ms();
    if (valid) k = snprintf(b, sizeof(b), "%lu,%.2f,%.2f,%u,%s\n", t, (double)T, (double)RH, q, g_rec.marker);
    else       k = snprintf(b, sizeof(b), "%lu,,,%u,%s\n", t, q, g_rec.marker);
    if (k >= (int)sizeof(b)) { k = (int)sizeof(b) - 1; b[k - 1] = '\n'; }
    if (k > 0 && rec_write(b, (size_t)k)) {
        g_rec.rows++;
        g_rec.marker[0] = 0;
    }
    uint32_t now = millis();
    if (g_rec.file_ok && now - g_rec.last_flush_ms >= 5000UL) {
        g_rec.last_flush_ms = now;
        g_rec.f.flush();
        rp2040.wdt_reset();
    }
    if (now - g_rec.last_save_ms >= 60000UL) {
        g_rec.last_save_ms = now;
        active_file_save();
    }
    /* SD-Health alle 30 s: erkennt gezogene Karte (write() meldet das
     * bei gepufferten Writes nicht sofort).                            */
    if (g_rec.file_ok && now - g_rec.last_probe_ms >= 30000UL) {
        g_rec.last_probe_ms = now;
        File d = SD.open("/sessions");
        if (!d) rec_sd_fail("probe");
        else d.close();
    }
}

/* ---------------- Messtakt (2 Hz) ----------------------------------- */
static bool g_in_tick = false;

static void measure_tick(uint32_t now) {
    g_in_tick = true;
    static uint8_t slow_divider = 0;
    float T = NAN, RH = NAN;
    bool valid = false;
    if (!g_sim) {
        if (g_sauna_probe == SAUNA_PROBE_SHT85 && sht_ready) {
            valid = sht85_read(&T, &RH);
        } else if (g_sauna_probe == SAUNA_PROBE_AHT20) {
            float h = 0, t = 0;
            if (aht20_fallback.getSensor(&h, &t) && t > -10.0f && t < 120.0f) {
                T = t; RH = constrain(h * 100.0f, 0.0f, 100.0f); valid = true;
            } else g_sht85_fails++;
        }
        if (valid) {
            g_sauna_ok_ms = now;
            g_sauna_temp = T; g_sauna_rh = RH;
            sensor_data_send(SSC_PKT_SAUNA_TEMP, T);
            sensor_data_send(SSC_PKT_SAUNA_RH, RH);
            if (g_st_phase != ST_IDLE) {
                g_st_acc_t += T; g_st_acc_rh += RH; g_st_acc_n++;
            }
        } else if (now - g_sauna_ok_ms > 3000UL) {
            g_sauna_temp = NAN; g_sauna_rh = NAN;
        }
        process_sample(clk_ms(), T, RH, valid);
    }
    if ((slow_divider++ & 1) == 0) {
        if (scd_ready) sensor_scd4x_read_and_send();
        if (sgp_ready) sensor_sgp40_read_and_send();
    }
    /* Heizer-30-s-Limit auch waehrend langer Operationen (LIST/RBK/?D)
     * durchsetzen - measure_tick laeuft dort via rt_service weiter.   */
    if (g_st_phase == ST_HEAT) selftest_service();
    g_in_tick = false;
}

/* Aus langen Schleifen (LIST, Readback, Dump, Event-Streams) aufrufen:
 * haelt WDT, 2-Hz-Messung und CSV am Laufen.                          */
static void rt_service(void) {
    rp2040.wdt_reset();
    if (g_in_tick) return;
    uint32_t now = millis();
    if (now - g_last_tick_ms >= g_interval_ms) {
        g_last_tick_ms += g_interval_ms;
        if (now - g_last_tick_ms >= g_interval_ms) g_last_tick_ms = now;
        measure_tick(now);
    }
}

/* ---------------- Sensor-Selbsttest (Heater, bewacht) --------------- */
/* Einzige Stelle, die den Heater je einschaltet. Interlocks gemaess
 * Sensirion-Research 2026-10-24: keine Session, Kabine 5..40 °C,
 * RH 20..85 %, T stabil, >= 6 h seit letztem Test, max 30 s Heizen.
 * Bewertung: k = gemessene RH-Abnahme / erwartete (bei konstanter
 * abs. Feuchte und gemessenem T-Hub). k >= 0.6 gesund, < 0.3 blind.  */
static uint8_t selftest_refuse_reason(void) {
    if (g_sauna_probe != SAUNA_PROBE_SHT85 || !sht_ready) return SSC_STR_NO_PROBE;
    if (g_rec.active) return SSC_STR_SESSION;
    if (isnan(g_sauna_temp) || g_sauna_temp < 5.0f || g_sauna_temp > 40.0f) return SSC_STR_TEMP;
    if (isnan(g_sauna_rh) || g_sauna_rh < 20.0f || g_sauna_rh > 85.0f) return SSC_STR_RH;
    if (g_det.r5_fill >= 60) {
        float d = g_det.r5_t[(g_det.r5_head + SSC_DET_R5_N - 1) % SSC_DET_R5_N] -
                  g_det.r5_t[(g_det.r5_head + SSC_DET_R5_N - 61) % SSC_DET_R5_N];
        if (fabsf(d) > 0.5f) return SSC_STR_UNSTABLE;   /* > 0.1 K/min */
    }
    if (g_st_last_ms != 0 && millis() - g_st_last_ms < 6UL * 3600000UL) return SSC_STR_TOO_SOON;
    return 0;
}

static void selftest_start(void) {
    g_st_phase = ST_BASE;
    g_st_phase_ms = millis();
    g_st_acc_t = g_st_acc_rh = 0; g_st_acc_n = 0;
    g_st_result = SSC_ST_RUNNING;
    g_st_reason = 0;
    sht_heater_force_off("selftest-pre");
    slog("Selbsttest: Basislinie 30 s");
}

static void selftest_abort(const char *why) {
    sht_heater_force_off(why);
    g_st_phase = ST_SETTLE;
    g_st_phase_ms = millis();
    g_st_result = SSC_ST_ABORTED;
    Serial.print(LOG_TAG); Serial.print("Selbsttest abgebrochen: "); Serial.println(why);
}

static void selftest_service(void) {
    if (g_st_phase == ST_IDLE) return;
    uint32_t el = millis() - g_st_phase_ms;
    if (g_st_phase == ST_BASE) {
        if (g_rec.active) { selftest_abort("session"); return; }
        if (el >= 30000UL) {
            if (g_st_acc_n < 20) { selftest_abort("keine messwerte"); return; }
            g_st_t0 = g_st_acc_t / g_st_acc_n; g_st_rh0 = g_st_acc_rh / g_st_acc_n;
            g_st_acc_t = g_st_acc_rh = 0; g_st_acc_n = 0;
            if (!sht85_write_cmd(SHT_CMD_HEATER_ON)) { selftest_abort("enable i2c"); return; }
            uint16_t st;
            if (sht_status_read(&st) && (st & SHT_STATUS_HEATER)) g_sht_heater_on = true;
            g_st_phase = ST_HEAT;
            g_st_phase_ms = millis();
            slog("Selbsttest: Heater AN (max 30 s)");
        }
    } else if (g_st_phase == ST_HEAT) {
        /* Hart begrenzt auf 30 s - egal was passiert. */
        if (g_rec.active || el >= 30000UL) {
            bool ok = sht_heater_force_off("selftest-end");
            g_st_last_ms = millis();
            /* Mittel der letzten Sekunden (acc wird in ST_HEAT gesammelt,
             * wir nehmen die gesamte Heizphase - konservativ) */
            g_st_tend = g_st_acc_n ? g_st_acc_t / g_st_acc_n : NAN;
            g_st_rhend = g_st_acc_n ? g_st_acc_rh / g_st_acc_n : NAN;
            float dT = g_st_tend - g_st_t0;
            if (!ok) g_st_result = SSC_ST_ABORTED;
            else if (isnan(dT) || dT < 0.3f) g_st_result = SSC_ST_INCONCLUSIVE;
            else {
                /* erwartete RH bei konstanter abs. Feuchte */
                float ah = ssc_abs_humidity(g_st_t0, g_st_rh0);
                float rh_exp = ah * (273.15f + g_st_tend) / 216.7f /
                               (6.1121f * expf((18.678f - g_st_tend / 234.5f) *
                                               (g_st_tend / (257.14f + g_st_tend)))) * 100.0f;
                float d_exp = g_st_rh0 - rh_exp, d_meas = g_st_rh0 - g_st_rhend;
                float k = (d_exp > 0.05f) ? d_meas / d_exp : 0.0f;
                g_st_k_x100 = (uint16_t)constrain(k * 100.0f, 0.0f, 999.0f);
                g_st_result = (k >= 0.6f) ? SSC_ST_PASS : (k < 0.3f ? SSC_ST_BLIND : SSC_ST_INCONCLUSIVE);
            }
            Serial.print(LOG_TAG); Serial.print("Selbsttest Ergebnis="); Serial.print(g_st_result);
            Serial.print(" dT="); Serial.print(dT, 2); Serial.print(" k="); Serial.println(g_st_k_x100 / 100.0f, 2);
            g_st_phase = ST_SETTLE;
            g_st_phase_ms = millis();
            sensor_health_send();
        }
    } else if (g_st_phase == ST_SETTLE) {
        /* 5 min abklingen: Samples bleiben fuer den Detektor geflaggt. */
        if (el >= 300000UL) { g_st_phase = ST_IDLE; sensor_health_send(); }
    }
}

static void sensor_health_send(void) {
    ssc_sensor_health_t h;
    memset(&h, 0, sizeof(h));
    uint8_t reason = 0;
    if (g_det.rh_suspect) reason |= 1;
    if (g_sht_heater_stuck) reason |= 2;
    if (g_sht_consec_fails >= 10) reason |= 4;
    if (g_sauna_probe != SAUNA_PROBE_SHT85) reason |= 8;
    h.reason = reason;
    h.health = g_sht_heater_stuck ? SSC_H_FAULT :
               (g_det.rh_suspect || g_st_result == SSC_ST_BLIND) ? SSC_H_RH_INVALID :
               (reason ? SSC_H_CHECK : SSC_H_OK);
    h.heater_on = (g_sht_heater_on || g_st_phase == ST_HEAT) ? 1 : 0;
    h.st_state = g_st_result;
    h.st_reason = g_st_reason;
    h.st_k_x100 = g_st_k_x100;
    h.st_age_min = g_st_last_ms ? (uint16_t)min((millis() - g_st_last_ms) / 60000UL, 0xFFFEUL) : 0xFFFF;
    h.status_reg = g_sht_status;
    h.i2c_fails = (uint16_t)min(g_sht85_fails, (uint32_t)0xFFFF);
    uint8_t pkt[1 + sizeof(h)];
    pkt[0] = SSC_PKT_SENSOR_HEALTH;
    memcpy(pkt + 1, &h, sizeof(h));
    raw_packet_send(pkt, sizeof(pkt));
}

/* ---------------- Zustand an den ESP32 ------------------------------ */
static void send_session_state(void) {
    ssc_session_state_t s;
    memset(&s, 0, sizeof(s));
    s.ver = SSC_PROTO_VERSION;
    s.rec_state = g_rec.active ? SSC_REC_RECORDING : SSC_REC_IDLE;
    uint8_t f = 0;
    if (sd_init_flag) f |= SSC_SF_SD_OK;
    if (g_rec.active && g_rec.file_ok) f |= SSC_SF_WRITE_OK;
    if ((g_sauna_probe == SAUNA_PROBE_SHT85 || g_sauna_probe == SAUNA_PROBE_AHT20) &&
        millis() - g_sauna_ok_ms < 5000UL) f |= SSC_SF_PROBE_OK;
    if (g_sim) f |= SSC_SF_PROBE_OK;
    if (time_valid()) f |= SSC_SF_TIME_SYNCED;
    if (g_auto_enabled) f |= SSC_SF_AUTO;
    if (g_rec.active && g_rec.resumed) f |= SSC_SF_RESUMED;
    if (g_det.rh_suspect) f |= SSC_SF_RH_SUSPECT;
    if (g_st_phase == ST_HEAT || g_sht_heater_on) f |= SSC_SF_HEATER;
    s.flags = f;
    s.phase = g_det.phase;
    s.boot_count = g_boot_count;
    if (g_rec.active) {
        memcpy(s.id, g_rec.id, sizeof(s.id));
        s.start_utc = g_rec.start_utc;
        s.elapsed_s = rec_t_ms() / 1000UL;
        s.rows = g_rec.rows;
        s.write_errs = g_rec.write_errs;
        s.gang = g_rec.gang;
        uint8_t na = 0, nl = 0;
        for (uint8_t i = 0; i < g_rec.n_ev; i++) {
            if (g_rec.ev[i].flags & SSC_EVF_DELETED) continue;
            if (g_rec.ev[i].kind == SSC_EK_AUFGUSS) na++;
            if (g_rec.ev[i].kind == SSC_EK_LUEFTEN) nl++;
        }
        s.n_aufguss = na; s.n_lueften = nl;
        s.n_events = g_rec.n_ev;
        s.origin = g_rec.origin;
        s.peak_t_x10 = g_rec.peak_t > -100 ? x10s(g_rec.peak_t) : 0;
        s.peak_rh_x10 = g_rec.peak_rh > -100 ? x10u(g_rec.peak_rh) : 0;
    }
    s.health = g_sht_heater_stuck ? SSC_H_FAULT : (g_det.rh_suspect ? SSC_H_RH_INVALID : SSC_H_OK);
    uint8_t pkt[1 + sizeof(s)];
    pkt[0] = SSC_PKT_SESSION_STATE;
    memcpy(pkt + 1, &s, sizeof(s));
    raw_packet_send(pkt, sizeof(pkt));
}

/* ---------------- Readback: Huellkurve "RBK2" ----------------------- */
/* Statt der alten 60-Punkte-Dezimierung (Spitzen verschwanden) liefert
 * der RP2040 240 Buckets mit Min/Max von T und RH. Format (LE):
 *   "RBK2" u16 count u32 dur_ms  dann count x { u32 t_ms, i16 tmin,
 *   i16 tmax, u16 rhmin, u16 rhmax }  (Werte x10, 0x7FFF = leer)      */
#define RBK_BUCKETS 240
#define RBK_HDR 10
#define RBK_REC 12
#define SSC_RBK_BUF_SIZE (RBK_HDR + RBK_BUCKETS * RBK_REC)
static uint8_t  g_rbk_buf[SSC_RBK_BUF_SIZE];
static uint32_t g_rbk_size = 0;
static char     g_rbk_sid[SSC_ID_LEN] = {0};

/* Zeile parsen: v2 "t_ms,temp,rh,q,event" / v1 "t_s,temp,rh,mark" */
static bool csv_parse_line(const char *line, bool v2, uint32_t *t_ms, float *T, float *RH,
                           bool *hasv) {
    char *e;
    double tv = strtod(line, &e);
    if (e == line || *e != ',') return false;
    *t_ms = v2 ? (uint32_t)tv : (uint32_t)(tv * 1000.0);
    const char *p = e + 1;
    if (*p == ',') { *hasv = false; return true; }
    double a = strtod(p, &e);
    if (e == p || *e != ',') { *hasv = false; return true; }
    p = e + 1;
    double b = strtod(p, &e);
    if (e == p) { *hasv = false; return true; }
    *T = (float)a; *RH = (float)b; *hasv = true;
    return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static bool rbk_build(const char *sid) {
    char path[44];
    snprintf(path, sizeof(path), "/sessions/%s.csv", sid);
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t fs = f.size();
    char line[96];
    /* Header lesen */
    size_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
        int c = f.read(); if (c < 0 || c == '\n') break; line[len++] = (char)c;
    }
    line[len] = 0;
    bool v2 = strncmp(line, "t_ms", 4) == 0;
    uint32_t data_start = f.position();
    /* Dauer aus dem Dateiende */
    uint32_t dur = 0;
    if (fs > data_start + 2) {
        uint32_t from = fs > 400 ? fs - 400 : data_start;
        if (from < data_start) from = data_start;
        f.seek(from);
        len = 0;
        while (f.available()) {
            int c = f.read();
            if (c < 0) break;
            if (c == '\n') {
                line[len] = 0;
                uint32_t t; float a, b; bool hv;
                if (len > 0 && csv_parse_line(line, v2, &t, &a, &b, &hv) && t > dur) dur = t;
                len = 0;
            } else if (len < sizeof(line) - 1) line[len++] = (char)c;
        }
    }
    uint32_t bw = dur / RBK_BUCKETS + 1;
    if (bw < 500) bw = 500;
    uint16_t nb = (uint16_t)min((uint32_t)RBK_BUCKETS, dur / bw + 1);
    int16_t  tmin[RBK_BUCKETS], tmax[RBK_BUCKETS];
    uint16_t hmin[RBK_BUCKETS], hmax[RBK_BUCKETS];
    for (int i = 0; i < RBK_BUCKETS; i++) { tmin[i] = 0x7FFF; tmax[i] = (int16_t)0x8000; hmin[i] = 0xFFFF; hmax[i] = 0; }
    f.seek(data_start);
    len = 0;
    uint32_t idx = 0, prev_t = 0;
    while (f.available()) {
        int c = f.read();
        if (c < 0) break;
        if (c == '\n') {
            line[len] = 0;
            uint32_t t; float a = 0, b = 0; bool hv = false;
            if (len > 0 && csv_parse_line(line, v2, &t, &a, &b, &hv)) {
                /* v1 bei 2 Hz: doppelte Sekunden -> nach Reihenfolge */
                if (!v2 && t == prev_t && idx > 0) t += 500;
                prev_t = t; idx++;
                if (hv) {
                    uint32_t k = t / bw; if (k >= nb) k = nb - 1;
                    int16_t tv = x10s(a); uint16_t rv = x10u(b);
                    if (tv < tmin[k]) tmin[k] = tv;
                    if (tv > tmax[k]) tmax[k] = tv;
                    if (rv < hmin[k]) hmin[k] = rv;
                    if (rv > hmax[k]) hmax[k] = rv;
                }
            }
            len = 0;
            if ((idx & 0x7F) == 0) rt_service();
        } else if (len < sizeof(line) - 1) line[len++] = (char)c;
    }
    f.close();
    uint8_t *p = g_rbk_buf;
    memcpy(p, "RBK2", 4); put16(p + 4, nb); put32(p + 6, dur);
    p += RBK_HDR;
    for (uint16_t i = 0; i < nb; i++, p += RBK_REC) {
        put32(p, i * bw);
        bool empty = tmin[i] == 0x7FFF;
        put16(p + 4, empty ? 0x7FFF : (uint16_t)tmin[i]);
        put16(p + 6, empty ? 0x7FFF : (uint16_t)tmax[i]);
        put16(p + 8, empty ? 0x7FFF : hmin[i]);
        put16(p + 10, empty ? 0x7FFF : hmax[i]);
    }
    g_rbk_size = RBK_HDR + (uint32_t)nb * RBK_REC;
    return true;
}

static void handle_sd_readback(const uint8_t *payload, size_t len) {
    rt_service();
    if (!sd_init_flag || len != (size_t)ssc_cmd_len(SSC_CMD_SD_READBACK)) return;
    uint16_t req_id = (uint16_t)((payload[0] << 8) | payload[1]);
    char sid[SSC_ID_LEN];
    memcpy(sid, payload + 2, SSC_ID_LEN - 1);
    sid[SSC_ID_LEN - 1] = 0;
    uint32_t offset = ((uint32_t)payload[26] << 24) | ((uint32_t)payload[27] << 16) |
                      ((uint32_t)payload[28] << 8) | payload[29];
    uint16_t max_len = (uint16_t)((payload[30] << 8) | payload[31]);
    uint8_t chunk[256];
    chunk[0] = SSC_PKT_RBK_CHUNK;
    chunk[1] = (uint8_t)(req_id >> 8);
    chunk[2] = (uint8_t)req_id;
    bool ok = ssc_id_valid(sid, SSC_ID_LEN);
    /* aktive Session: Huelle immer frisch bauen (waechst) */
    bool active = g_rec.active && strncmp(sid, g_rec.id, SSC_ID_LEN) == 0;
    if (ok && (active || strncmp(sid, g_rbk_sid, SSC_ID_LEN) != 0 || g_rbk_size == 0)) {
        if (offset == 0 || !active) {
            if (active && g_rec.file_ok) g_rec.f.flush();
            g_rbk_sid[0] = 0;
            if (rbk_build(sid)) { strncpy(g_rbk_sid, sid, SSC_ID_LEN - 1); g_rbk_sid[SSC_ID_LEN - 1] = 0; }
            else ok = false;
        }
    }
    uint32_t total = ok ? g_rbk_size : 0;
    if (offset > total) offset = total;
    uint16_t lim = (max_len == 0 || max_len > 200) ? 200 : max_len;
    uint32_t avail = total - offset;
    uint16_t got = (uint16_t)(avail < lim ? avail : lim);
    chunk[3] = (uint8_t)(total >> 24); chunk[4] = (uint8_t)(total >> 16);
    chunk[5] = (uint8_t)(total >> 8);  chunk[6] = (uint8_t)total;
    chunk[7] = (uint8_t)(offset >> 24); chunk[8] = (uint8_t)(offset >> 16);
    chunk[9] = (uint8_t)(offset >> 8);  chunk[10] = (uint8_t)offset;
    chunk[11] = (uint8_t)(got >> 8);    chunk[12] = (uint8_t)got;
    if (got) memcpy(chunk + 13, g_rbk_buf + offset, got);
    /* Immer antworten - auch bei Fehler (total=0), damit der ESP32 nie
     * auf ein Paket wartet das nicht kommt.                            */
    raw_packet_send(chunk, 13 + got);
}

/* ---------------- LIST / META -------------------------------------- */
static int synthesize_meta_from_csv(const char *id, ssc_meta_wire_t *out) {
    char path[44];
    snprintf(path, sizeof(path), "/sessions/%s.csv", id);
    File f = SD.open(path, FILE_READ);
    if (!f) return -2;
    memset(out, 0, sizeof(*out));
    strncpy(out->id, id, sizeof(out->id) - 1);
    static bool s_tz_set = false;
    if (!s_tz_set) { setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset(); s_tz_set = true; }
    if ((id[0] == 'S' || id[0] == 'X') && strlen(id) >= 16) {
        struct tm tmv; memset(&tmv, 0, sizeof(tmv));
        char tmp[5];
        strncpy(tmp, id + 1, 4); tmp[4] = 0; tmv.tm_year = atoi(tmp) - 1900;
        strncpy(tmp, id + 5, 2); tmp[2] = 0; tmv.tm_mon = atoi(tmp) - 1;
        strncpy(tmp, id + 7, 2); tmp[2] = 0; tmv.tm_mday = atoi(tmp);
        strncpy(tmp, id + 10, 2); tmp[2] = 0; tmv.tm_hour = atoi(tmp);
        strncpy(tmp, id + 12, 2); tmp[2] = 0; tmv.tm_min = atoi(tmp);
        strncpy(tmp, id + 14, 2); tmp[2] = 0; tmv.tm_sec = atoi(tmp);
        tmv.tm_isdst = -1;
        out->start_ts = (int32_t)mktime(&tmv);
    }
    char line[96]; size_t len = 0;
    bool v2 = false, first = true;
    float pt = -1000, prh = -1000; uint32_t last_t = 0, n = 0;
    while (f.available()) {
        int c = f.read();
        if (c < 0) break;
        if (c != '\n') { if (len < sizeof(line) - 1) line[len++] = (char)c; continue; }
        line[len] = 0; len = 0;
        if (first) { first = false; v2 = strncmp(line, "t_ms", 4) == 0; continue; }
        uint32_t t; float a = 0, b = 0; bool hv = false;
        if (!csv_parse_line(line, v2, &t, &a, &b, &hv)) continue;
        if (hv) { if (a > pt) pt = a; if (b > prh) prh = b; }
        last_t = t;
        if ((++n & 0xFF) == 0) rt_service();
    }
    f.close();
    out->peak_temp = pt > -500 ? pt : 0;
    out->peak_rh = prh > -500 ? prh : 0;
    out->end_ts = out->start_ts ? out->start_ts + (int32_t)(last_t / 1000UL) : 0;
    return 0;
}

static void send_session_meta(ssc_meta_wire_t *m) {
    ssc_meta_seal(m);
    uint8_t pkt[1 + sizeof(ssc_meta_wire_t)];
    pkt[0] = SSC_PKT_META_RESP;
    memcpy(pkt + 1, m, sizeof(*m));
    raw_packet_send(pkt, sizeof(pkt));
}

static void handle_list_sessions(void) {
    uint16_t count = 0;
    if (sd_init_flag) {
        File dir = SD.open("/sessions");
        if (dir) {
            File entry;
            while ((entry = dir.openNextFile())) {
                rt_service();
                const char *name = entry.name();
                size_t nl = name ? strlen(name) : 0;
                bool is_csv = nl > 4 && strcmp(name + nl - 4, ".csv") == 0;
                if (!is_csv) { entry.close(); continue; }
                char id[SSC_ID_LEN] = {0};
                size_t cp = nl - 4;
                if (cp >= sizeof(id)) { entry.close(); continue; }
                memcpy(id, name, cp);
                entry.close();
                if (!ssc_id_valid(id, SSC_ID_LEN)) continue;
                ssc_meta_wire_t meta; bool meas;
                if (read_session_json(id, &meta, &meas) != 0) {
                    if (synthesize_meta_from_csv(id, &meta) != 0) continue;
                }
                if (g_rec.active && strcmp(id, g_rec.id) == 0) rec_fill_measured(&meta, false);
                send_session_meta(&meta);
                count++;
                /* ESP32 verarbeitet jedes META in NVS - nicht fluten */
                uint32_t t0 = millis();
                while (millis() - t0 < 25) rt_service();   /* KEIN update(): wir sind im Callback */
            }
            dir.close();
        }
    }
    uint8_t done[3] = { SSC_PKT_META_DONE, (uint8_t)(count & 0xFF), (uint8_t)(count >> 8) };
    raw_packet_send(done, sizeof(done));
    Serial.print(LOG_TAG); Serial.print("LIST_SESSIONS sent "); Serial.print(count); Serial.println(" entries");
}

static uint8_t handle_meta_push(const uint8_t *p) {
    ssc_meta_wire_t m;
    memcpy(&m, p, sizeof(m));
    if (ssc_meta_check(&m) != 1) { g_bad_crc++; return SSC_RC_BAD_CRC; }
    m.id[sizeof(m.id) - 1] = 0;
    if (!ssc_id_valid(m.id, SSC_ID_LEN)) return SSC_RC_BAD_ID;
    if (!sd_init_flag) return SSC_RC_SD_ERR;
    if (g_rec.active && strcmp(m.id, g_rec.id) == 0) {
        g_rec.user = m;
        g_rec.have_user = true;
        rec_write_json(false);
        return SSC_RC_OK;
    }
    char path[44];
    snprintf(path, sizeof(path), "/sessions/%s.csv", m.id);
    if (!SD.exists(path)) return SSC_RC_NOT_FOUND;
    /* vergangene Session: Messwerte aus bestehender v0.4-JSON behalten */
    ssc_meta_wire_t old; bool meas = false;
    int gaenge = -1, lueften = 0;
    if (read_session_json(m.id, &old, &meas) == 0 && meas) {
        m.start_ts = old.start_ts; m.end_ts = old.end_ts;
        m.peak_temp = old.peak_temp; m.peak_rh = old.peak_rh;
        m.aufguss_count = old.aufguss_count;
        /* Gaenge/Lueften aus .evt neu zaehlen */
        int ne = evt_read(m.id, g_tmp_ev, g_tmp_lbl);
        gaenge = 0;
        for (int i = 0; i < ne; i++) {
            if (g_tmp_ev[i].flags & SSC_EVF_DELETED) continue;
            if (g_tmp_ev[i].kind == SSC_EK_GANG) gaenge++;
            if (g_tmp_ev[i].kind == SSC_EK_LUEFTEN) lueften++;
        }
    }
    int rc = write_session_json(&m, gaenge, lueften, 0, m.id[0] == 'X', 0, 0);
    return rc == 0 ? SSC_RC_OK : SSC_RC_SD_ERR;
}

static uint8_t handle_delete(const uint8_t *p, bool json_only) {
    char id[SSC_ID_LEN];
    memcpy(id, p, SSC_ID_LEN - 1);
    id[SSC_ID_LEN - 1] = 0;
    if (!ssc_id_valid(id, SSC_ID_LEN)) return SSC_RC_BAD_ID;
    if (g_rec.active && strcmp(id, g_rec.id) == 0) return SSC_RC_STATE;  /* nie die aktive */
    if (!sd_init_flag) return SSC_RC_SD_ERR;
    char path[48];
    const char *ext_all[] = { ".csv", ".json", ".evt" };
    int n = json_only ? 1 : 3;
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "/sessions/%s%s", id, json_only ? ".json" : ext_all[i]);
        if (SD.exists(path)) SD.remove(path);
    }
    if (strcmp(g_rbk_sid, id) == 0) { g_rbk_sid[0] = 0; g_rbk_size = 0; }
    Serial.print(LOG_TAG); Serial.print(json_only ? "DELJSON " : "DELETE "); Serial.println(id);
    return SSC_RC_OK;
}

/* ---------------- Events lesen / labeln ----------------------------- */
static void handle_get_events(const uint8_t *p) {
    char id[SSC_ID_LEN];
    memcpy(id, p, SSC_ID_LEN - 1);
    id[SSC_ID_LEN - 1] = 0;
    const ssc_event_rec_t *ev = NULL; const ssc_event_label_body_t *lb = NULL;
    int n = 0; uint8_t rc = SSC_RC_OK;
    if (!ssc_id_valid(id, SSC_ID_LEN)) rc = SSC_RC_BAD_ID;
    else if (g_rec.active && strcmp(id, g_rec.id) == 0) { ev = g_rec.ev; lb = g_rec.lbl; n = g_rec.n_ev; }
    else {
        n = evt_read(id, g_tmp_ev, g_tmp_lbl);
        if (n < 0) { n = 0; rc = SSC_RC_NOT_FOUND; }
        ev = g_tmp_ev; lb = g_tmp_lbl;
    }
    for (int i = 0; i < n; i++) {
        ssc_event_full_t m;
        memset(&m, 0, sizeof(m));
        memcpy(m.id, id, SSC_ID_LEN);
        m.e = ev[i]; m.l = lb[i];
        uint8_t pkt[1 + sizeof(m)];
        pkt[0] = SSC_PKT_EVENT_REC;
        memcpy(pkt + 1, &m, sizeof(m));
        raw_packet_send(pkt, sizeof(pkt));
        rt_service();
    }
    uint8_t done[1 + SSC_ID_LEN + 2];
    done[0] = SSC_PKT_EVENTS_DONE;
    memcpy(done + 1, id, SSC_ID_LEN);
    done[1 + SSC_ID_LEN] = (uint8_t)n;
    done[2 + SSC_ID_LEN] = rc;
    raw_packet_send(done, sizeof(done));
}

static uint8_t handle_event_label(const uint8_t *p) {
    ssc_event_label_t m;
    memcpy(&m, p, sizeof(m));
    if (ssc_crc16(&m, offsetof(ssc_event_label_t, crc)) != m.crc) { g_bad_crc++; return SSC_RC_BAD_CRC; }
    m.id[SSC_ID_LEN - 1] = 0;
    if (!ssc_id_valid(m.id, SSC_ID_LEN)) return SSC_RC_BAD_ID;
    m.l.operator_tag[SSC_LBL_OP - 1] = 0;
    m.l.name[SSC_LBL_NAME - 1] = 0;
    m.l.aroma[SSC_LBL_AROMA - 1] = 0;
    bool act = g_rec.active && strcmp(m.id, g_rec.id) == 0;
    ssc_event_rec_t *ev = act ? g_rec.ev : g_tmp_ev;
    ssc_event_label_body_t *lb = act ? g_rec.lbl : g_tmp_lbl;
    int n = act ? g_rec.n_ev : evt_read(m.id, g_tmp_ev, g_tmp_lbl);
    if (n <= 0 || m.no == 0 || m.no > n) return SSC_RC_NOT_FOUND;
    int i = m.no - 1;
    if (m.delete_flag == 1) ev[i].flags |= SSC_EVF_DELETED;
    else if (m.delete_flag == 2) ev[i].flags &= (uint8_t)~SSC_EVF_DELETED;
    else if (m.delete_flag == 3) lb[i] = m.l;   /* nur vorbelegen, nicht bestaetigt */
    else { lb[i] = m.l; ev[i].flags |= SSC_EVF_LABELED; }
    int rc = evt_write(m.id, ev, lb, (uint8_t)n);
    if (act) { g_state_dirty = true; rec_write_json(false); }
    return rc == 0 ? SSC_RC_OK : SSC_RC_SD_ERR;
}

static void auto_cfg_load(void) {
    if (!sd_init_flag) return;
    File f = SD.open("/ssc_auto.cfg", FILE_READ);
    if (!f) return;
    int c = f.read();
    f.close();
    if (c == '0') g_auto_enabled = 0;
    else if (c == '1') g_auto_enabled = 1;
}
static void auto_cfg_save(void) {
    if (!sd_init_flag) return;
    SD.remove("/ssc_auto.cfg");
    File f = SD.open("/ssc_auto.cfg", FILE_WRITE);
    if (!f) return;
    f.write((uint8_t)(g_auto_enabled ? '1' : '0'));
    f.close();
}

static void send_hello(void) {
    ssc_hello_resp_t h;
    memset(&h, 0, sizeof(h));
    h.proto = SSC_PROTO_VERSION;
    strncpy(h.fw, VERSION, sizeof(h.fw) - 1);
    h.boot_count = g_boot_count;
    h.reset_reason = g_last_reset_reason;
    h.caps = 0x07;
    uint8_t pkt[1 + sizeof(h)];
    pkt[0] = SSC_PKT_HELLO_RESP;
    memcpy(pkt + 1, &h, sizeof(h));
    raw_packet_send(pkt, sizeof(pkt));
}

/* ================================================================== */
/* ESP32 -> RP: Kommandoverarbeitung (Protokoll v2)                   */
/* ================================================================== */
static void onPacketReceived(const uint8_t *buffer, size_t size) {
    if (size < 1) return;
    g_pkt_rx++;
    uint8_t cmd = buffer[0];
    int want = ssc_cmd_len(cmd);
    const uint8_t *p = buffer + 1;
    size_t len = size - 1;
    if (want < 0) { g_pkt_rx_unk++; return; }
    if ((int)len != want) {
        /* exakte Laenge: verschmolzene/abgeschnittene Frames haben keine
         * Seiteneffekte mehr (v0.3.3: kaputte Sidecar, .csv-Datei).   */
        g_bad_len++;
        Serial.print(LOG_TAG); Serial.print("cmd 0x"); Serial.print(cmd, HEX);
        Serial.print(" bad len "); Serial.println(len);
        if (cmd != SSC_CMD_SESSION_START || len != 0) send_ack(cmd, SSC_RC_BAD_LEN, 0);
        return;
    }
    switch (cmd) {
    case SSC_CMD_SHUTDOWN:
        slog("cmd shutdown ignoriert (v0.4: Aufzeichnung laeuft weiter)");
        break;
    case SSC_CMD_COLLECT_INTERVAL: {
        uint32_t iv = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        if (iv >= 250 && iv <= 60000) { g_interval_ms = iv; g_interval_base = iv; }
        break;
    }
    case SSC_CMD_SESSION_START: {
        uint8_t origin = (p[0] == SSC_ORIGIN_AUTO) ? SSC_ORIGIN_AUTO : SSC_ORIGIN_MANUAL;
        uint8_t rc = rec_start(origin);
        send_ack(cmd, rc, 0);
        send_session_state();
        break;
    }
    case SSC_CMD_SESSION_END: {
        if (!g_rec.active) { send_ack(cmd, SSC_RC_STATE, 0); break; }
        rec_end(p[0] == SSC_END_AUTO ? SSC_END_AUTO : SSC_END_MANUAL);
        send_ack(cmd, SSC_RC_OK, 0);
        send_session_state();
        break;
    }
    case SSC_CMD_ADD_EVENT: {
        ssc_add_event_t a; memcpy(&a, p, sizeof(a));
        if (!g_rec.active || a.kind != SSC_EK_AUFGUSS) { send_ack(cmd, SSC_RC_STATE, 0); break; }
        rec_on_aufguss(NULL, true, g_sauna_temp, g_sauna_rh);
        send_ack(cmd, SSC_RC_OK, g_rec.n_ev);
        break;
    }
    case SSC_CMD_SD_READBACK:   handle_sd_readback(p, len); break;
    case SSC_CMD_GET_RP_STATUS: rp2040_send_status(); break;
    case SSC_CMD_LIST_SESSIONS: handle_list_sessions(); break;
    case SSC_CMD_META_PUSH:     send_ack(cmd, handle_meta_push(p), 0); break;
    case SSC_CMD_DELETE_FILE:   send_ack(cmd, handle_delete(p, false), 0); break;
    case SSC_CMD_DELETE_JSON:   send_ack(cmd, handle_delete(p, true), 0); break;
    case SSC_CMD_SENSOR_SELFTEST: {
        uint8_t why = selftest_refuse_reason();
        if (g_st_phase != ST_IDLE) why = SSC_STR_TOO_SOON;
        if (why) {
            g_st_result = SSC_ST_REFUSED; g_st_reason = why;
            send_ack(cmd, SSC_RC_REFUSED, why);
        } else {
            selftest_start();
            send_ack(cmd, SSC_RC_OK, 0);
        }
        sensor_health_send();
        break;
    }
    case SSC_CMD_TIME_SYNC: {
        ssc_time_sync_t t; memcpy(&t, p, sizeof(t));
        if (t.utc > 1600000000UL) {
            /* monoton: nie hinter eine bereits bekannte gute Zeit zurueck */
            uint32_t cur = utc_now();
            if (!(time_valid() && t.source < g_ts_src && t.utc + 120 < cur)) {
                g_ts_utc = t.utc; g_ts_ms = millis(); g_ts_tz = t.tz_off_s;
                g_ts_src = t.source; g_ts_have = true;
            }
            if (g_rec.active && g_rec.start_utc == 0 && time_valid())
                g_rec.start_utc = utc_now() - rec_t_ms() / 1000UL;
            rec_check_resume_gap();
        }
        break;
    }
    case SSC_CMD_AUTO_CFG: {
        ssc_auto_cfg_t c; memcpy(&c, p, sizeof(c));
        if (ssc_crc16(&c, offsetof(ssc_auto_cfg_t, crc)) != c.crc) { send_ack(cmd, SSC_RC_BAD_CRC, 0); break; }
        uint8_t en = c.enabled ? 1 : 0;
        if (en != g_auto_enabled) { g_auto_enabled = en; auto_cfg_save(); g_state_dirty = true; }
        send_ack(cmd, SSC_RC_OK, en);
        break;
    }
    case SSC_CMD_EVENT_LABEL:   send_ack(cmd, handle_event_label(p), p[SSC_ID_LEN]); break;
    case SSC_CMD_GET_EVENTS:    handle_get_events(p); break;
    case SSC_CMD_HELLO:         send_hello(); send_session_state(); break;
    case SSC_CMD_GET_STATE:     send_session_state(); sensor_health_send(); break;
    default:
        g_pkt_rx_unk++;
        break;
    }
}


/* ================================================================== */
/* Setup / Loop                                                       */
/* ================================================================== */
static void sensor_power_on(void) {
    pinMode(18, OUTPUT);
    digitalWrite(18, HIGH);
}

void setup() {
    Serial.begin(115200);
    bool was_wdt_reset = watchdog_caused_reboot();

    Serial1.setRX(17);
    Serial1.setTX(16);
    /* v0.4.0 (Audit C1): Core-Default ist 32 B RX-FIFO = 2.7 ms bei
     * 115200. Jede laengere loop()-Phase hat Bytes verloren und COBS-
     * Frames verschmolzen (Ursache der kaputten JSON-Sidecar). 4 KB
     * puffern ~350 ms Dauerstrom.                                     */
    Serial1.setFIFOSize(4096);
    Serial1.begin(115200);
    myPacketSerial.setStream(&Serial1);
    myPacketSerial.setPacketHandler(&onPacketReceived);

    uint32_t t_power_on = millis();
    sensor_power_on();

    i2c_bus_unlock(20, 21);
    Wire.setSDA(20);
    Wire.setSCL(21);
    Wire.begin();
    Wire.setClock(50000);       /* 2-m-Silikonkabel: Rise-Time-Budget  */
    Wire.setTimeout(25, true);  /* arduino-pico: ms + Controller-Reset  */

    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) { delay(10); }

    g_last_reset_reason = (uint8_t)rp2040_detect_reset_reason(was_wdt_reset);
    g_boot_count        = rp2040_boot_counter_tick_and_get();
    const char *rr_boot_txt =
        g_last_reset_reason == SSC_RST_POR  ? "POR"  :
        g_last_reset_reason == SSC_RST_WDT  ? "WDT"  :
        g_last_reset_reason == SSC_RST_SOFT ? "SOFT" : "???";
    Serial.println();
    Serial.print(LOG_TAG); Serial.print("BOOT ");  Serial.print(VERSION);
    Serial.print(" reset_reason="); Serial.print(rr_boot_txt);
    Serial.print(" boot_count=");
    Serial.println(g_boot_count);
    Serial.flush();

    ssc_det_init(&g_det);   /* g_rec ist global null-initialisiert (enthaelt File - kein memset!) */

    slog("boot start, I2C-bus klar");
    i2c_scan_log("boot-early");

    pinMode(13, OUTPUT);
    digitalWrite(13, HIGH);
    SPI1.setSCK(10);
    SPI1.setTX(11);
    SPI1.setRX(12);
    sd_init_flag = sd_try_init();
    if (sd_init_flag) session_recovery_scan();

    i2c_scan_log("boot");

    while (millis() - t_power_on < 1000) {
        delay(5);
        rp2040.wdt_reset();
    }
    sensor_sgp40_init();
    sensor_scd4x_init();

    sht_ready = sht85_init();
    if (sht_ready) {
        g_sauna_probe = SAUNA_PROBE_SHT85;
    } else {
        slog("kein SHT85 gefunden - versuche AHT20-Fallback...");
        aht_ready = aht20_fallback_init();
        if (aht_ready) {
            g_sauna_probe = SAUNA_PROBE_AHT20;
            slog("Fallback-Betrieb: AHT20 als sauna-fuehler (Spec bis 85 degC - nur fuer Test!)");
        } else if (scd_ready) {
            g_sauna_probe = SAUNA_PROBE_SCD41;
            slog("!!! kein externer Fuehler - keine Kabinenwerte (Dev-Mode)");
        } else {
            slog("!!! kein Sauna-Fuehler gefunden");
        }
    }

    if (sd_init_flag) {
        auto_cfg_load();
        rec_try_resume();
    }

    Serial.print(LOG_TAG);
    Serial.print("RP2040 ");
    Serial.print(VERSION);
    Serial.print(" ready: SD=");  Serial.print(sd_init_flag);
    Serial.print(" SCD41=");      Serial.print(scd_ready);
    Serial.print(" SGP40=");      Serial.print(sgp_ready);
    Serial.print(" SHT=");        Serial.print(sht_ready);
    Serial.print(" AHT20fb=");    Serial.print(aht_ready);
    Serial.print(" auto=");       Serial.print(g_auto_enabled);
    Serial.print(" rec=");        Serial.println(g_rec.active ? g_rec.id : "-");

    rp2040.wdt_begin(SSC_WATCHDOG_MS);
    slog("watchdog armed (8 s)");
    g_last_tick_ms = millis();
}

/* Health-Zeile (60 s und auf ?H) */
static void print_health(void) {
    float t_out  = isnan(g_sauna_temp) ? 0.0f : g_sauna_temp;
    float rh_out = isnan(g_sauna_rh)   ? 0.0f : g_sauna_rh;
    const char *probe_str =
        g_sauna_probe == SAUNA_PROBE_SHT85 ? "SHT3x" :
        g_sauna_probe == SAUNA_PROBE_AHT20 ? "AHT20" :
        g_sauna_probe == SAUNA_PROBE_SCD41 ? "SCD41-PROXY" : "NONE";
    const char *rr =
        g_last_reset_reason == SSC_RST_POR  ? "POR"  :
        g_last_reset_reason == SSC_RST_WDT  ? "WDT"  :
        g_last_reset_reason == SSC_RST_SOFT ? "SOFT" : "???";
    char b[360];
    snprintf(b, sizeof(b),
        "%shealth: up=%lus probe=%s temp=%.1f rh=%.1f fails=%lu heater=%d stuck=%d "
        "rec=%s rows=%lu werr=%u phase=%u gang=%u aufg=%u auto=%u sim=%d sd=%d "
        "time=%d rp_die=%.1f boot=%lu rst=%s pkt=%u unk=%u badlen=%u badcrc=%u ovf=%u st=%u/%u k=%u",
        LOG_TAG, (unsigned long)(millis() / 1000UL), probe_str, (double)t_out, (double)rh_out,
        (unsigned long)g_sht85_fails, g_sht_heater_on, g_sht_heater_stuck,
        g_rec.active ? g_rec.id : "-", (unsigned long)g_rec.rows, g_rec.write_errs,
        g_det.phase, g_rec.gang, g_rec.n_aufguss, g_auto_enabled, g_sim, sd_init_flag,
        time_valid(), (double)analogReadTemp(), (unsigned long)g_boot_count, rr,
        g_pkt_rx, g_pkt_rx_unk, g_bad_len, g_bad_crc, g_uart_ovf, g_st_result, g_st_phase, g_st_k_x100);
    Serial.println(b);
}

static void periodic(uint32_t now) {
    /* Zustand alle 2 s (und sofort bei Aenderung) an den ESP32 */
    static uint32_t last_state = 0;
    if (g_state_dirty || now - last_state >= 2000) {
        g_state_dirty = false;
        last_state = now;
        send_session_state();
    }
    static uint32_t last_probe_send = 0;
    if (now - last_probe_send > 15000) {
        last_probe_send = now;
        uint8_t ps[3] = { PKT_TYPE_PROBE_STATE, (uint8_t)g_sauna_probe, (uint8_t)(sd_init_flag ? 1 : 0) };
        raw_packet_send(ps, sizeof(ps));
    }
    static uint32_t last_health = 0;
    if (now - last_health >= 5000) {
        last_health = now;
        sensor_health_send();
    }
    /* Heater jede Minute verifizieren (ausser waehrend Selbsttest-Heizen) */
    static uint32_t last_hv = 0;
    if (now - last_hv >= 60000UL && g_st_phase != ST_HEAT &&
        g_sauna_probe == SAUNA_PROBE_SHT85 && sht_ready && !g_sim) {
        last_hv = now;
        uint16_t st;
        if (sht_status_read(&st)) {
            g_sht_status = st;
            if (st & SHT_STATUS_HEATER) {
                slog("!!! Heater unerwartet AN -> force off");
                sht_heater_force_off("periodic");
                if (g_rec.active) set_marker("HEATER_OFF");
            } else {
                g_sht_heater_on = false;
                g_sht_heater_stuck = false;
            }
        }
    }
    selftest_service();

    /* Automatisches taegliches Ausheizen (Sensirion-Plausibilitaetstest):
     * einmal pro 24 h, nur bei kalter, ruhiger Kabine ohne Session -
     * dieselben Interlocks wie der manuelle Selbsttest. Nach dem Boot
     * frhestens nach 30 min (Kabine muss stabil sein).                */
    static uint32_t last_auto_st_check = 0;
    if (now - last_auto_st_check >= 60000UL) {
        last_auto_st_check = now;
        bool due = g_st_last_ms ? (now - g_st_last_ms >= 24UL * 3600000UL)
                                : (now >= 30UL * 60000UL);
        if (due && g_st_phase == ST_IDLE && !g_sim && selftest_refuse_reason() == 0) {
            slog("Auto-Ausheizen (taeglicher Selbsttest) startet");
            selftest_start();
            sensor_health_send();
        }
    }

    /* Sim-Modus endet 5 s nach dem letzten ?F */
    if (g_sim && now - g_sim_last_feed > 5000UL) {
        slog("SIM-Modus beendet");
        if (g_rec.active && g_rec.sim) rec_end(SSC_END_MANUAL);
        g_sim = false;
        ssc_det_init(&g_det);
        g_state_dirty = true;
    }

    static uint32_t last_scan = 0;
    if (!sht_ready && !aht_ready && now - last_scan > 30000) {
        last_scan = now;
        i2c_scan_log("periodic");
    }
    static uint32_t last_reprobe = 0;
    bool need_reprobe = (!sht_ready && !aht_ready) ||
                        (g_sauna_probe == SAUNA_PROBE_SHT85 && g_sht_consec_fails > 30) ||
                        (g_sauna_probe == SAUNA_PROBE_AHT20);   /* SHT bevorzugt */
    if (need_reprobe && now - last_reprobe > 8000) {
        last_reprobe = now;
        if (sht85_init()) {
            sht_ready = true;
            g_sht_consec_fails = 0;
            g_sauna_probe = SAUNA_PROBE_SHT85;
            slog("re-probe: SHT3x da");
        } else if (!aht_ready && aht20_fallback_init()) {
            aht_ready = true;
            g_sauna_probe = SAUNA_PROBE_AHT20;
        }
    }
    /* SD neu mounten (echter Remount: SD.end + begin) und laufende
     * Session wieder anhaengen.                                       */
    static uint32_t last_sd_retry = 0;
    if (!sd_init_flag && now - last_sd_retry > 15000) {
        last_sd_retry = now;
        SD.end(false);
        sd_init_flag = sd_try_init();
        if (sd_init_flag && !g_rec.active) {
            /* rec_end lief evtl. mit toter SD: Altlast entfernen, sonst
             * wuerde ein spaeterer Reset die beendete Session fortsetzen. */
            SD.remove("/ssc_active.txt");
            SD.remove("/ssc_active.tmp");
        }
        if (sd_init_flag && g_rec.active && !g_rec.file_ok) {
            g_rec.f = SD.open(g_rec.path, FILE_WRITE);
            if (g_rec.f) {
                g_rec.file_ok = true;
                set_marker("SD_GAP");
                slog("SD wieder da - Session wird fortgesetzt");
            }
        }
        g_state_dirty = true;
    }
    static uint32_t last_hlog = 0;
    if (now - last_hlog >= 60000) {
        last_hlog = now;
        rp2040.wdt_reset();
        print_health();
        rp2040.wdt_reset();
    }
}

/* USB-CDC-Debug: strikt '?'-Prefix (feedback-sicher, siehe v0.2.14).
 *   ?L            Dateiliste       ?D <datei>  Datei dumpen
 *   ?H            Health-Zeile     ?S          Session-Zustand
 *   ?F t T RH     Sim-Sample (t in ms, Replay)  ?X  WDT-Test (haengt)
 *   ?B / ?E       Test: Session starten / beenden                  */
static void usb_cdc_service(void) {
    static char buf[96];
    static uint8_t idx = 0;
    static bool discard = false;
    int budget = 512;
    while (Serial.available() && budget-- > 0) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (discard) continue;
            if (idx == 0 && c != '?') { discard = true; continue; }
            if (idx < sizeof(buf) - 1) buf[idx++] = c; else { discard = true; idx = 0; }
            continue;
        }
        bool ok = !discard && idx > 1 && buf[0] == '?';
        buf[idx] = 0;
        idx = 0; discard = false;
        if (!ok) continue;
        char op = buf[1];
        if (op == 'L' || op == 'l') {
            Serial.println(F("[SAUNA] LIST_BEGIN"));
            if (sd_init_flag) {
                File dir = SD.open("/sessions");
                if (dir) {
                    File f;
                    while ((f = dir.openNextFile())) {
                        Serial.print(F("[SAUNA] FILE "));
                        Serial.print(f.name()); Serial.print(' '); Serial.println(f.size());
                        f.close();
                        rt_service();
                    }
                    dir.close();
                }
            } else Serial.println(F("[SAUNA] SD not ready"));
            Serial.println(F("[SAUNA] LIST_END"));
        } else if ((op == 'D' || op == 'd') && buf[2] == ' ' && sd_init_flag) {
            const char *nm = buf + 3;
            bool safe = nm[0] && !strstr(nm, "..") && !strchr(nm, '/');
            char path[100];
            snprintf(path, sizeof(path), "/sessions/%s", nm);
            File f = safe ? SD.open(path) : File();
            if (f) {
                Serial.print(F("[SAUNA] DUMP_BEGIN ")); Serial.println(nm);
                uint8_t blk[256];
                while (f.available()) {
                    int n = f.read(blk, sizeof(blk));
                    if (n <= 0) break;
                    Serial.write(blk, (size_t)n);
                    rt_service();
                }
                f.close();
                Serial.println();
                Serial.print(F("[SAUNA] DUMP_END ")); Serial.println(nm);
            } else {
                Serial.print(F("[SAUNA] DUMP_ERR not found ")); Serial.println(path);
            }
        } else if (op == 'H' || op == 'h') {
            print_health();
        } else if (op == 'S' || op == 's') {
            Serial.print(F("[SAUNA] STATE rec=")); Serial.print(g_rec.active ? g_rec.id : "-");
            Serial.print(F(" t_ms=")); Serial.print(g_rec.active ? rec_t_ms() : 0);
            Serial.print(F(" phase=")); Serial.print(g_det.phase);
            Serial.print(F(" gang=")); Serial.print(g_rec.gang);
            Serial.print(F(" aufguss=")); Serial.print(g_rec.n_aufguss);
            Serial.print(F(" lueften=")); Serial.print(g_rec.n_luef);
            Serial.print(F(" events=")); Serial.print(g_rec.n_ev);
            Serial.print(F(" auto=")); Serial.print(g_auto_enabled);
            Serial.print(F(" sim=")); Serial.println(g_sim);
        } else if (op == 'F' || op == 'f') {
            /* ?F <t_ms> <temp> <rh> - Replay-Sample. Nur wenn keine echte
             * Session laeuft (sonst wuerden echte Daten verfaelscht).   */
            unsigned long t; float T, RH;
            if (sscanf(buf + 2, "%lu %f %f", &t, &T, &RH) == 3) {
                if (!g_sim) {
                    if (g_rec.active && !g_rec.sim) { Serial.println(F("[SAUNA] SIM_REFUSED session")); continue; }
                    g_sim = true;
                    ssc_det_init(&g_det);
                    slog("SIM-Modus aktiv (Replay)");
                }
                g_sim_t = (uint32_t)t;
                g_sim_last_feed = millis();
                sensor_data_send(SSC_PKT_SAUNA_TEMP, T);
                sensor_data_send(SSC_PKT_SAUNA_RH, RH);
                process_sample(g_sim_t, T, RH, true);
            }
        } else if (op == 'B' || op == 'b') {
            /* Test: echte Session manuell starten (wie START-Knopf) */
            uint8_t rc = rec_start(SSC_ORIGIN_MANUAL);
            Serial.print(F("[SAUNA] ?B rc=")); Serial.println(rc);
        } else if (op == 'E' || op == 'e') {
            rec_end(SSC_END_MANUAL);
            Serial.println(F("[SAUNA] ?E ok"));
        } else if ((op == 'K' || op == 'k') && buf[2] == ' ') {
            /* Test-/Wartung: Session-Dateien loeschen (nie die aktive) */
            uint8_t idb[SSC_ID_LEN] = {0};
            strncpy((char *)idb, buf + 3, SSC_ID_LEN - 1);
            Serial.print(F("[SAUNA] ?K rc=")); Serial.println(handle_delete(idb, false));
        } else if (op == 'X' || op == 'x') {
            slog("WDT-Test: haenge loop() absichtlich");
            Serial.flush();
            while (1) { }
        }
    }
}

void loop() {
    uint32_t now = millis();
    rp2040.wdt_reset();

    /* Fester 2-Hz-Raster (v0.4.0: last += interval statt = now, damit
     * der Takt nicht driftet).                                         */
    if (now - g_last_tick_ms >= g_interval_ms) {
        g_last_tick_ms += g_interval_ms;
        if (now - g_last_tick_ms >= g_interval_ms) g_last_tick_ms = now;
        measure_tick(now);
    }
    periodic(now);

    myPacketSerial.update();
    if (myPacketSerial.overflow()) {
        static uint32_t last_recovery = 0;
        if (millis() - last_recovery > 2000) {
            last_recovery = millis();
            slog("packetserial overflow -> re-init");
            myPacketSerial.~PacketSerial_();
            new (&myPacketSerial) PacketSerial_<COBS, 0, 512>();
            myPacketSerial.setStream(&Serial1);
            myPacketSerial.setPacketHandler(&onPacketReceived);
        }
    }
    if (Serial1.overflow()) {
        if (g_uart_ovf < 0xFFFF) g_uart_ovf++;
        slog("UART1 RX overflow");
    }
    usb_cdc_service();
}
