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

#define DEBUG 0
#define VERSION "ssc-v0.2.11"
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
static void session_end(void);
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
#define SSC_INTERVAL_NORMAL_MS   1000   /* 1 Hz Messzyklus idle          */
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
static uint32_t g_boost_until_ms = 0;                      /* 0=inaktiv */
static uint32_t g_last_tick_ms = 0;

/* Zaehler fuer I2C-Fehler - hilft beim Debuggen von Kabelproblemen. */
static uint32_t g_sht85_fails = 0;
static uint32_t g_scd_fails   = 0;

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

    if (SD.begin(chipSelect, 1000000, SPI1)) {
        slog("SD: initialisiert");
        return true;
    }
    slog("SD: begin() fehlgeschlagen (karte nicht erkannt / falsches FS? "
         "128GB-Karten muessen FAT32 formatiert sein - Arduino-SD-Lib "
         "kann kein exFAT)");
    return false;
}

/* Leichter Health-Check: verifiziert dass der `sd_init_flag` nicht
 * stale ist (Karte koennte gezogen worden sein). Gibt true nur wenn
 * /sessions als Directory oeffnet. Schuetzt SD.remove() davor, auf
 * einen unmounted-oder-neu-eingelegten Card zu schreiben. */
static bool sd_health_probe(void) {
    if (!sd_init_flag) return false;
    File d = SD.open("/sessions");
    if (!d) { sd_init_flag = false; return false; }
    d.close();
    return true;
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
    if (sht_try_addr(0x44)) return true;
    if (sht_try_addr(0x45)) return true;
    slog("SHT3x not found on I2C 0x44 or 0x45");
    return false;
}

/* ------------------------------------------------------------------ */
/* SHT85 Heater-Cycle fuer RH-Stuck-at-100 nach Aufguss              */
/* ------------------------------------------------------------------ */
/* Sensirion Heater-Commands (SHT85 datasheet Tab. 10):
 *   0x3039 Heater 200 mW, 1  s (high power, long)
 *   0x306D Heater  33 mW, 1  s (low  power, long) <- unser Default
 * Low-power-long reicht fuer Kondensat-Abbau am Polymer-Kappen, ohne
 * die Messung durch Selbsterwaermung zu verfaelschen. Nach Aufguss
 * (RH >= 99.5 % fuer 60+ Samples) einmal feuern und dann 3 s kuehlen. */
#define SHT85_CMD_HEATER_LP_LONG  0x306D
static uint32_t g_sht_heater_last_ms = 0;
static uint16_t g_sht_rh100_count    = 0;

static void sht85_heater_cycle(void) {
    slog("SHT3x heater cycle (RH stuck at 100%)");
    sht85_write_cmd(SHT85_CMD_HEATER_LP_LONG);
    /* Heater laeuft ca. 1 s, dann 3 s Polymer kuehlen, WDT-sicher. */
    for (int i = 0; i < 16; i++) {
        delay(250);
        rp2040.wdt_reset();
    }
    g_sht_heater_last_ms = millis();
    g_sht_rh100_count = 0;
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

static void aht20_read_and_send(void) {
    float h = 0, t = 0;
    int ok = aht20_fallback.getSensor(&h, &t);
    if (!ok) {
        g_sht85_fails++;  /* reused counter */
        return;
    }
    float rh = h * 100.0f;
    /* Physical sanity (gleiche Envelope wie SHT85: -10..+120 / 0..100).
     * AHT20 spec ist 0..85 degC aber wir pruefen defensiv weiter - ein
     * CRC-glitch kann die gleichen Muell-Werte produzieren. */
    if (t < -10.0f || t > 120.0f) { g_sht85_fails++; return; }
    if (rh < 0.0f)   rh = 0.0f;
    if (rh > 100.0f) rh = 100.0f;
    g_sauna_temp = t;
    g_sauna_rh   = rh;
    sensor_data_send(PKT_TYPE_SENSOR_SAUNA_TEMP, t);
    sensor_data_send(PKT_TYPE_SENSOR_SAUNA_RH, rh);
}

static void sht85_read_and_send(void) {
    float t = NAN, rh = NAN;
    bool ok = false;
    /* Retry: I2C ist am 2m-Kabel anfaellig fuer stoerungen, kurze
     * retries bei CRC-fail erhoehen die datenrate dramatisch.       */
    for (uint8_t r = 0; r < SSC_SHT85_RETRY_MAX; r++) {
        if (sht85_measure(&t, &rh)) { ok = true; break; }
        delay(8);
    }
    if (!ok) {
        g_sht85_fails++;
        if ((g_sht85_fails % 10) == 1) {
            Serial.print(LOG_TAG);
            Serial.print("SHT3x measure failed (total fails: ");
            Serial.print(g_sht85_fails);
            Serial.println(")");
        }
        /* Kurz-Recovery: nach 5 consecutive fails ein Soft-Reset-Kommando,
         * ohne gleich die ganze Init-Chain zu durchlaufen. Billig und
         * hilft bei transient brownouts des Kabel-Sensors.               */
        if ((g_sht85_fails % 5) == 0) {
            Wire.beginTransmission(g_sht_addr);
            Wire.write(0x30); Wire.write(0xA2);   /* Soft-Reset */
            Wire.endTransmission();
            delay(5);
        }
        /* Bei dauerhaften Fehlern (alle 20 fails): volle Re-Init mit
         * Wire.end()/begin() und Bus-Unlock-Preamble. Loest auch
         * peripheral-jammed states.                                      */
        if ((g_sht85_fails % 20) == 0) {
            slog("SHT3x recovery attempt (bus-unlock + re-init)");
            i2c_bus_unlock(20, 21);
            if (sht85_init()) {
                sht_ready = true;
                g_sht85_fails = 0;   /* clean slate nach erfolgreichem re-init */
            } else {
                sht_ready = false;
            }
        }
        return;
    }
    g_sauna_temp = t;
    g_sauna_rh   = rh;
    sensor_data_send(PKT_TYPE_SENSOR_SAUNA_TEMP, t);
    sensor_data_send(PKT_TYPE_SENSOR_SAUNA_RH, rh);

    /* RH-Stuck-at-100-Detektion: Nach Aufguss haelt sich der Polymer-Kappen
     * oft minutenlang bei 100 %RH (Kondensat haengt physisch am Sensor),
     * obwohl die Luft schon trocknet. Wenn wir 60 consecutive Samples bei
     * >=99.5 % sehen UND kein Aufguss-Boost laeuft (dort ist 100 % legitim)
     * UND der letzte Heat >=10 min her ist, einen Heater-Zyklus feuern. */
    if (rh >= 99.5f) g_sht_rh100_count++; else g_sht_rh100_count = 0;
    if (g_sht_rh100_count >= 60 &&
        g_boost_until_ms == 0 &&
        (millis() - g_sht_heater_last_ms) > 600000UL) {
        sht85_heater_cycle();
    }
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
/* Session-SD-Gateway                                                 */
/* ================================================================== */
#define SSC_SESSION_ID_LEN  24
#define SSC_AUFGUSS_LEN     48

static bool  g_session_active = false;
static char  g_session_id[SSC_SESSION_ID_LEN] = {0};
static char  g_session_path[40] = {0};
static File  g_session_file;
static uint32_t g_session_started_ms = 0;

/* Readback-Downsample-Buffer: Beim ersten Request einer Session lesen
 * wir die CSV einmal komplett + decimieren auf ~300 Samples (Aufguss-
 * Marker IMMER behalten). Resultat landet in diesem DRAM-Buffer, aus
 * dem die Chunks gelesen werden. Vorteile ggue. direktem File-Read:
 *   - Kleine SD-Lese-Zeit nur einmal (~100-500 ms upfront statt pro Chunk)
 *   - Grosse Sessions werden drastisch kleiner (60 KB -> 4 KB) -> weniger
 *     UART-Chunks -> ESP32-UART-Parser-Overflow-Risiko minimiert
 *   - Kurze Sessions: kein Decimate, volle Daten (Buffer enthaelt copy) */
#define SSC_RBK_DS_BUF_SIZE  6144
#define SSC_RBK_TARGET_SAMPLES 60    /* aggressiv-downsampling: 60 Punkte fuer die
                                        Chart-Uebersicht. 60 samples * ~15 bytes = ~900 bytes
                                        = ~15 UART-Chunks @ 64B = <3s bei gutem UART */
static char     g_rbk_ds_buf[SSC_RBK_DS_BUF_SIZE];
static uint32_t g_rbk_ds_size = 0;
static char     g_rbk_ds_sid[SSC_SESSION_ID_LEN] = {0};

static bool rbk_build_downsampled(const char *sid) {
    char path[40];
    snprintf(path, sizeof(path), "/sessions/%s.csv", sid);
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    rp2040.wdt_reset();

    uint32_t fs = f.size();
    g_rbk_ds_size = 0;

    if (fs <= 4000 && fs < SSC_RBK_DS_BUF_SIZE) {
        /* Kleine Session: volles Kopieren. */
        while (f.available() && g_rbk_ds_size < SSC_RBK_DS_BUF_SIZE) {
            int c = f.read();
            if (c < 0) break;
            g_rbk_ds_buf[g_rbk_ds_size++] = (char)c;
            if ((g_rbk_ds_size & 0x3FF) == 0) rp2040.wdt_reset();
        }
        f.close();
        rp2040.wdt_reset();
        return true;
    }

    /* Grosse Session: decimieren. N = ceil(samples / TARGET).
     * Samples-Schaetzung: ~20 bytes pro Zeile avg. */
    uint32_t est_samples = fs / 20;
    uint16_t N = (est_samples + SSC_RBK_TARGET_SAMPLES - 1) / SSC_RBK_TARGET_SAMPLES;
    if (N < 2) N = 2;

    char line[128];
    uint16_t lb = 0;
    uint32_t line_num = 0;    /* 0 = header, dann sample lines */

    while (f.available()) {
        int c = f.read();
        if (c < 0) break;

        if (c == '\n' || c == '\r') {
            if (lb > 0) {
                line[lb] = 0;
                bool keep = false;
                if (line_num == 0) {
                    keep = true;        /* header immer behalten */
                } else {
                    /* Aufguss-Marker (letzte spalte nicht leer) -> immer behalten */
                    const char *last_comma = strrchr(line, ',');
                    bool has_marker = (last_comma && last_comma[1] != 0);
                    if (has_marker) keep = true;
                    else if ((line_num - 1) % N == 0) keep = true;
                }
                if (keep && g_rbk_ds_size + lb + 1 < SSC_RBK_DS_BUF_SIZE) {
                    memcpy(g_rbk_ds_buf + g_rbk_ds_size, line, lb);
                    g_rbk_ds_size += lb;
                    g_rbk_ds_buf[g_rbk_ds_size++] = '\n';
                }
                line_num++;
                lb = 0;
                if ((line_num & 0x3F) == 0) rp2040.wdt_reset();
            }
        } else if (lb < sizeof(line) - 1) {
            line[lb++] = (char)c;
        }
    }
    f.close();
    rp2040.wdt_reset();
    return true;
}

static void session_path_from_id(const char *id) {
    /* "/sessions/<id>.csv" */
    snprintf(g_session_path, sizeof(g_session_path), "/sessions/%s.csv", id);
}

static bool session_ensure_dir(void) {
    if (!sd_init_flag) return false;
    if (SD.exists("/sessions")) {
        /* Verifizieren dass es wirklich eine funktionsfaehige Directory ist.
         * Nach brownout mid-mkdir kann dir-entry da sein, aber der FAT-
         * cluster-chain unvollstaendig - exists=true aber open=false. */
        File d = SD.open("/sessions");
        if (!d) {
            Serial.println("session: /sessions exists but unopenable, recreate");
            SD.rmdir("/sessions");  /* best effort */
            if (!SD.mkdir("/sessions")) {
                Serial.println("session: mkdir failed after cleanup");
                return false;
            }
        } else {
            d.close();
        }
        return true;
    }
    if (!SD.mkdir("/sessions")) {
        Serial.println("session: mkdir /sessions failed");
        return false;
    }
    return true;
}

static void session_start(const char *id) {
    if (g_session_active) {
        /* State-desync handling: ESP32 kann rebootet sein waehrend
         * unsere session noch aktiv war, schickt dann einen neuen
         * START. Wir schliessen die alte sauber ab und starten die
         * neue - sonst wird keine CSV auf SD angelegt und die ESP-
         * seite hat metadata ohne samples.                          */
        Serial.println("session: already active - auto-end then restart");
        session_end();
    }
    if (!session_ensure_dir()) return;

    strncpy(g_session_id, id, SSC_SESSION_ID_LEN - 1);
    g_session_id[SSC_SESSION_ID_LEN - 1] = 0;

    /* Leere ID ablehnen: ESP32 koennte bei leerem Payload ein SESSION_START
     * schicken, sonst wuerde eine Datei "/sessions/.csv" entstehen. */
    if (g_session_id[0] == 0) {
        Serial.println("session: empty id, abort");
        return;
    }

    /* Sanity check: nur [A-Za-z0-9_-] erlauben, damit kein Pfad-
     * escape via ".." oder "/" moeglich ist. */
    for (size_t i = 0; g_session_id[i]; i++) {
        char c = g_session_id[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            Serial.print("session: invalid char in id, abort: ");
            Serial.println(g_session_id);
            memset(g_session_id, 0, sizeof(g_session_id));
            return;
        }
    }

    session_path_from_id(g_session_id);

    /* Health-Probe vor write-mutating Operation: verhindert SD.remove/SD.open
     * auf einer gezogenen oder neu-eingesteckten Karte, was den in-memory FAT-
     * cache inkonsistent zur physischen Karte machen wuerde. */
    if (!sd_health_probe()) {
        Serial.println("session: SD vanished before open, abort");
        return;
    }

    /* Arduino-SD oeffnet FILE_WRITE im Append-Modus. Wenn der letzte Lauf
     * abgebrochen war (Watchdog/Brownout) und die CSV existiert, wuerde der
     * Header ein zweites Mal drangehaengt. Sauber vorher loeschen. */
    if (SD.exists(g_session_path)) {
        SD.remove(g_session_path);
    }

    g_session_file = SD.open(g_session_path, FILE_WRITE);
    if (!g_session_file) {
        Serial.print("session: cannot open ");
        Serial.println(g_session_path);
        return;
    }
    g_session_file.println("t_elapsed_s,temp,rh,aufguss");
    g_session_file.flush();
    g_session_started_ms = millis();
    g_session_active = true;
    Serial.print("session start: ");
    Serial.println(g_session_id);
}

static void session_write_line(uint32_t dt, const char *marker) {
    /* CSV-Format: t_elapsed_s,temp,rh,aufguss
     * Wir nutzen bewusst print()/println() statt printf() - printf auf
     * Arduino-File-Objekten ist nicht ueberall verfuegbar (abhaengig
     * von der SD-Library). Das hier baut sich mit String-Konkatenation
     * sauber zusammen. */
    float t  = isnan(g_sauna_temp) ? 0.0f : g_sauna_temp;
    float rh = isnan(g_sauna_rh)   ? 0.0f : g_sauna_rh;
    g_session_file.print(dt);
    g_session_file.print(',');
    g_session_file.print(t, 2);
    g_session_file.print(',');
    g_session_file.print(rh, 2);
    g_session_file.print(',');
    if (marker) g_session_file.print(marker);
    g_session_file.print('\n');
}

static void session_write_sample(void) {
    if (!g_session_active || !g_session_file) return;
    uint32_t dt = (millis() - g_session_started_ms) / 1000;
    session_write_line(dt, nullptr);

    /* Defensive: wenn der SD-Write intern fehlschlug (z.B. SPI-Glitch hat den
     * FAT-Handle invalidiert), ist getWriteError() gesetzt. Dann nicht weiter
     * auf dem kaputten Handle arbeiten - sonst faulty deref moeglich. Karte
     * ebenfalls als kaputt markieren, damit der main-loop sd_try_init erneut
     * aufruft (alle 15 s). */
    if (g_session_file.getWriteError()) {
        Serial.println("[SAUNA] session: write error -> closing session, remount SD");
        g_session_file.close();
        g_session_file = File();
        g_session_active = false;
        sd_init_flag = false;     /* triggert sd_try_init re-retry im loop */
        return;
    }

    /* Flush einmal pro 10s, um SD-Lebensdauer zu schonen und Datenverluste bei
     * Stromausfall zu begrenzen. Im Boost-Modus laeuft der Zyklus aber mit
     * 2 Hz, d.h. `dt % 10 == 0` wuerde 2x/s fuer eine Sekunde feuern. Daher
     * auf den zuletzt-geflushten Sekunden-Stempel konditionieren.
     * WDT nach flush kicken: billige cards koennen GC-Pause bis ~1 s haben,
     * das frisst einen merklichen Anteil des 8 s-Budgets.                 */
    static uint32_t last_flush_s = UINT32_MAX;
    if ((dt % 10) == 0 && dt != last_flush_s) {
        last_flush_s = dt;
        g_session_file.flush();
        rp2040.wdt_reset();
    }
}

static void session_mark_aufguss(const char *name) {
    if (!g_session_active || !g_session_file) return;
    uint32_t dt = (millis() - g_session_started_ms) / 1000;
    session_write_line(dt, name);
    g_session_file.flush();
    Serial.print(LOG_TAG);
    Serial.print("aufguss: ");
    Serial.println(name);

    /* Aufguss-Boost: waehrend der naechsten 2 Minuten doppelt so oft
     * messen, damit der RH-Peak und Temp-Drop feiner aufgezeichnet
     * werden. Nach Boost-Ende faellt der Takt automatisch zurueck.   */
    g_boost_until_ms = millis() + SSC_BOOST_DURATION_MS;
    g_interval_ms = SSC_INTERVAL_BOOST_MS;
    slog("aufguss-boost: 2 Hz fuer 120 s");
}

static void session_end(void) {
    if (!g_session_active) return;
    if (g_session_file) {
        g_session_file.flush();
        g_session_file.close();
    }
    g_session_file = File();  /* handle zuruecksetzen, damit spaetere truthy-checks sauber sind */
    Serial.print("session end: ");
    Serial.println(g_session_id);
    memset(g_session_id, 0, sizeof(g_session_id));
    memset(g_session_path, 0, sizeof(g_session_path));
    g_session_active = false;
}

/* ------------------------------------------------------------------ */
/* SD-Readback (fuer ESP32 History-Detail-View)                       */
/* ------------------------------------------------------------------ */
/* Request-Layout (payload nach type-byte):
 *   [2B req_id][24B session_id + NUL-pad][4B byte_offset][2B max_len]
 * Antwort (0xC1):
 *   [2B req_id][4B total_size][2B offset][2B len][data...]
 */
/* Seit v0.2.5: Stream-Modus.
 *   max_len == 0         -> ESP32 will alles bis EOF; RP2040 pumpt alle
 *                           Chunks in einer Schleife ohne auf Per-Request
 *                           zu warten. Keine Debug-Prints im Hot-Path.
 *   max_len > 0          -> Legacy-Single-Chunk-Mode (abwaertskompatibel).
 * Ergebnis: 25 KB Session in ~3-5 s statt >60 s bzw. stuck. */
static void handle_sd_readback(const uint8_t *payload, size_t len) {
    rp2040.wdt_reset();
    if (!sd_init_flag) return;
    if (len < 2 + SSC_SESSION_ID_LEN + 4 + 2) return;

    uint16_t req_id = (uint16_t(payload[0]) << 8) | payload[1];
    char sid[SSC_SESSION_ID_LEN];
    memcpy(sid, payload + 2, SSC_SESSION_ID_LEN - 1);
    sid[SSC_SESSION_ID_LEN - 1] = 0;

    /* Pfad-Escape-Sanity (". ." / "/" / leer). */
    if (sid[0] == 0) return;
    for (size_t i = 0; sid[i]; i++) {
        char c = sid[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return;
    }

    uint32_t offset =
        (uint32_t(payload[2 + SSC_SESSION_ID_LEN]) << 24) |
        (uint32_t(payload[3 + SSC_SESSION_ID_LEN]) << 16) |
        (uint32_t(payload[4 + SSC_SESSION_ID_LEN]) << 8)  |
         uint32_t(payload[5 + SSC_SESSION_ID_LEN]);
    uint16_t max_len =
        (uint16_t(payload[6 + SSC_SESSION_ID_LEN]) << 8) |
         uint16_t(payload[7 + SSC_SESSION_ID_LEN]);

    /* Concurrent-Access-Guard gegen aktive Session (siehe v0.2.3 Analyse). */
    if (g_session_active && strncmp(sid, g_session_id, SSC_SESSION_ID_LEN) == 0) {
        if (g_session_file) g_session_file.flush();
        uint8_t err[13] = { PKT_TYPE_SD_READBACK_CHUNK,
                            (uint8_t)(req_id>>8),(uint8_t)req_id,
                            0,0,0,0,  0,0,0,0,  0,0 };
        raw_packet_send(err, sizeof(err));
        return;
    }

    /* Downsample-Buffer: bei neuer Session einmalig aufbauen (decimieren
     * falls gross), danach liest jeder Chunk aus dem DRAM-Buffer. */
    if (strncmp(sid, g_rbk_ds_sid, SSC_SESSION_ID_LEN) != 0 || g_rbk_ds_size == 0) {
        g_rbk_ds_sid[0] = 0;
        if (!rbk_build_downsampled(sid)) {
            Serial.print(LOG_TAG);
            Serial.print("readback: downsample build FAILED for sid=");
            Serial.println(sid);
            return;
        }
        strncpy(g_rbk_ds_sid, sid, SSC_SESSION_ID_LEN - 1);
        g_rbk_ds_sid[SSC_SESSION_ID_LEN - 1] = 0;
        Serial.print(LOG_TAG);
        Serial.printf("readback: downsampled sid=%s size=%u\n",
                      sid, (unsigned)g_rbk_ds_size);
    }
    uint32_t total = g_rbk_ds_size;
    if (offset >= total) return;

    uint8_t chunk[256];
    chunk[0] = PKT_TYPE_SD_READBACK_CHUNK;
    chunk[1] = uint8_t(req_id >> 8);
    chunk[2] = uint8_t(req_id & 0xFF);
    chunk[3] = uint8_t(total >> 24);
    chunk[4] = uint8_t(total >> 16);
    chunk[5] = uint8_t(total >> 8);
    chunk[6] = uint8_t(total);

    /* Per-chunk-mode: lese aus downsampled-Buffer (DRAM), nicht File. */
    uint16_t per_call_limit = (max_len == 0 || max_len > 200) ? 200 : max_len;
    uint32_t available = total - offset;
    uint16_t got = (uint16_t)(available < per_call_limit ? available : per_call_limit);
    memcpy(chunk + 13, g_rbk_ds_buf + offset, got);
    chunk[7]  = uint8_t(offset >> 24);
    chunk[8]  = uint8_t(offset >> 16);
    chunk[9]  = uint8_t(offset >> 8);
    chunk[10] = uint8_t(offset);
    chunk[11] = uint8_t(got >> 8);
    chunk[12] = uint8_t(got & 0xFF);
    raw_packet_send(chunk, 13 + got);
    rp2040.wdt_reset();
}

/* ================================================================== */
/* ESP32 -> RP: Kommandoverarbeitung                                  */
/* ================================================================== */
static bool shutdown_flag = false;

static void onPacketReceived(const uint8_t *buffer, size_t size) {
    if (size < 1) return;
    g_pkt_rx++;   /* v0.2.8: jedes packet das die size-hurde passiert */

#if DEBUG
    Serial.printf("<--- recv len:%u, type=0x%02X\n", (unsigned)size, buffer[0]);
#endif

    switch (buffer[0]) {
    case PKT_TYPE_CMD_SHUTDOWN:
        Serial.println("cmd: shutdown");
        shutdown_flag = true;
        session_end();
        break;

    case PKT_TYPE_CMD_COLLECT_INTERVAL:
        if (size >= 5) {
            uint32_t ival = (uint32_t(buffer[1]) << 24) |
                            (uint32_t(buffer[2]) << 16) |
                            (uint32_t(buffer[3]) << 8)  |
                             uint32_t(buffer[4]);
            if (ival >= 250 && ival <= 60000) {
                g_interval_base = ival;
                /* Wenn kein Boost laeuft, sofort uebernehmen.
                 * Rollover-sicher per signed diff. */
                if (!g_boost_until_ms || (int32_t)(millis() - g_boost_until_ms) >= 0)
                    g_interval_ms = ival;
                Serial.print(LOG_TAG);
                Serial.print("cmd interval=");
                Serial.print(ival);
                Serial.println(" ms");
            }
        }
        break;

    case PKT_TYPE_CMD_SESSION_START: {
        char id[SSC_SESSION_ID_LEN] = {0};
        size_t cp = (size - 1) < (SSC_SESSION_ID_LEN - 1)
                    ? (size - 1) : (SSC_SESSION_ID_LEN - 1);
        memcpy(id, buffer + 1, cp);
        session_start(id);
        break;
    }

    case PKT_TYPE_CMD_SESSION_AUFGUSS: {
        char name[SSC_AUFGUSS_LEN] = {0};
        size_t cp = (size - 1) < (SSC_AUFGUSS_LEN - 1)
                    ? (size - 1) : (SSC_AUFGUSS_LEN - 1);
        memcpy(name, buffer + 1, cp);
        session_mark_aufguss(name);
        break;
    }

    case PKT_TYPE_CMD_SESSION_END:
        session_end();
        break;

    case PKT_TYPE_CMD_SD_READBACK:
        handle_sd_readback(buffer + 1, size - 1);
        break;

    case PKT_TYPE_CMD_GET_RP_STATUS:
        /* v0.2.7: ESP32 fragt nach Boot-Counter + Reset-Reason */
        rp2040_send_status();
        break;

    default:
        g_pkt_rx_unk++;   /* v0.2.8: unknown cmd-byte */
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

    /* Reset-Reason SOFORT einfangen (bevor wdt_begin sie zuruecksetzen
     * koennte). watchdog_caused_reboot() ist die SDK-API des RP2040.     */
    bool was_wdt_reset = watchdog_caused_reboot();

    Serial1.setRX(17);
    Serial1.setTX(16);
    Serial1.begin(115200);
    myPacketSerial.setStream(&Serial1);
    myPacketSerial.setPacketHandler(&onPacketReceived);

    uint32_t t_power_on = millis();
    sensor_power_on();

    /* I²C-Bus-Unlock-Preamble: falls ein Slave (SHT85 auf dem 2m-Kabel)
     * bei einem frueheren Brownout/WDT-Reset mid-transaction abgebrochen
     * wurde, haelt er evtl. SDA LOW. Wire.begin() kann das NICHT loesen -
     * es wuerde naiv START-Bit senden und stuck-bleiben. Manuelles 9-Clock-
     * Recovery BEVOR Wire den Controller uebernimmt ist Pflicht, sonst
     * Boot-Loop bis Power-Cycle. */
    i2c_bus_unlock(20, 21);

    Wire.setSDA(20);
    Wire.setSCL(21);
    Wire.begin();
    /* 50 kHz I2C fuer 2m-Silikonkabel zum externen SHT85:
     * Rise-time-Budget 2 us statt 1 us. Mit on-board 4.7k-Pullups und
     * ~300 pF Kabelkapazitaet liegen wir bei real ~1.4 us - bei 100 kHz
     * marginal, bei 50 kHz komfortabel in-spec. I²C-Bandbreiten-Verlust
     * ist irrelevant (Sensor-Wartezeiten dominieren die Messdauer).    */
    Wire.setClock(50000);
    /* Clock-Stretch-Timeout: SCD41 stretched bis 1.5 ms, SGP40 bis ~12 ms.
     * 25 ms mit reset_on_timeout=true sprengt das 8 s-WDT auch bei 3 retries
     * (3*25 = 75 ms) nicht, fuettert aber nicht-mehr-antwortende Slaves nicht
     * in einen unbegrenzten busy-loop. WICHTIG: arduino-pico-API heisst
     * setTimeout(ms, reset_with_timeout) - NICHT das AVR-`setWireTimeout`
     * mit us-Einheit. Ohne diesen Call bleibt Stream-Default 1000 ms ohne
     * Controller-Reset aktiv - reicht bei 2m-SHT85-Kabel fuer WDT-Loop. */
    Wire.setTimeout(25, true);

    /* Serial-Init etwas warten damit USB-CDC enumeriert ist bevor wir
     * die ersten diagnose-messages schicken (sonst weg).             */
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) { delay(10); }

    /* v0.2.7: Reset-Reason + Boot-Counter capturen BEVOR wir was
     * anderes machen. EEPROM-tick ist flash-write, schlaegt bei einem
     * I2C-Crash vorher ohnehin niemals zu, daher frueh hier.
     * v0.2.8: scratch-canary-detection gibt 3-state statt bool.          */
    g_last_reset_reason = (uint8_t)rp2040_detect_reset_reason(was_wdt_reset);
    g_boot_count        = rp2040_boot_counter_tick_and_get();

    const char *rr_boot_txt =
        g_last_reset_reason == SSC_RST_POR  ? "POR"  :
        g_last_reset_reason == SSC_RST_WDT  ? "WDT"  :
        g_last_reset_reason == SSC_RST_SOFT ? "SOFT" : "???";

    /* Sehr frueher Boot-Banner mit explizitem flush() - garantiert dass bei
     * einem Crash-Loop mindestens DIESE Zeile pro Boot durchkommt. Reset-
     * reason gibt uns die Diagnose: WDT (code haengt/missed-feed), SOFT
     * (hardfault-handler, NVIC_SystemReset, external-reset), POR
     * (power-loss/brown-out - echter hw-power-cycle).                    */
    Serial.println();
    Serial.print(LOG_TAG); Serial.print("BOOT ");  Serial.print(VERSION);
    Serial.print(" reset_reason="); Serial.print(rr_boot_txt);
    Serial.print(" boot_count=");
    Serial.println(g_boot_count);
    Serial.flush();

    slog("boot start, I2C-bus klar");
    i2c_scan_log("boot-early");  /* sehr frueh - zeigt was vor sensor-init da ist */

    /* SD-Karte am SPI1 (SCK=10, TX=11, RX=12, CS=13).
     * CS explizit HIGH treiben BEVOR SPI1.setSCK/TX/RX aktiv werden - sonst
     * floatet GP13 waehrend der 74-Clock-Wakeup-Sequenz von SD.begin() und
     * die Karte interpretiert das als Command-Start. */
    pinMode(13, OUTPUT);
    digitalWrite(13, HIGH);
    SPI1.setSCK(10);
    SPI1.setTX(11);
    SPI1.setRX(12);
    sd_init_flag = sd_try_init();
    if (sd_init_flag) session_recovery_scan();

    /* I2C-Scan zur Diagnose - zeigt beim Boot, welche Slaves da sind */
    i2c_scan_log("boot");

    /* SCD41 braucht nach power-on bis zu 1000 ms bevor er I²C-Kommandos
     * akzeptiert. Wenn wir frueher begin()en, NACKt er und die Wire-Impl
     * kann den Bus fuer >50 ms blockieren. Das bisherige delay(25) war zu
     * kurz. Hier ziehen wir das volle 1s-Wakeup-Fenster, abzueglich der
     * Zeit die USB-Wait + SD-Init + I2C-Scan bereits verbraucht haben.    */
    while (millis() - t_power_on < 1000) {
        delay(5);
        rp2040.wdt_reset();
    }
    sensor_sgp40_init();
    sensor_scd4x_init();

    /* Sauna-Fuehler: zuerst SHT85 probieren (bevorzugt, bis 105 °C),
     * dann AHT20 als Fallback (bis 85 °C - gut fuers Entwicklungs-
     * testen, aber in der echten Sauna sollte ein SHT85/SHT31 hin). */
    sht_ready = sht85_init();
    if (sht_ready) {
        g_sauna_probe = SAUNA_PROBE_SHT85;
    } else {
        slog("kein SHT85 gefunden - versuche AHT20-Fallback...");
        aht_ready = aht20_fallback_init();
        if (aht_ready) {
            g_sauna_probe = SAUNA_PROBE_AHT20;
            slog("Fallback-Betrieb: AHT20 als sauna-fuehler "
                 "(Spec bis 85 degC - nur fuer Test!)");
        } else if (scd_ready) {
            g_sauna_probe = SAUNA_PROBE_SCD41;
            slog("!!! kein externer Fuehler - interner SCD41 "
                 "wird parallel als Sauna-Proxy gesendet (Dev-Mode)");
        } else {
            slog("!!! kein Sauna-Fuehler gefunden - kurve bleibt leer");
        }
    }

    Serial.print(LOG_TAG);
    Serial.print("RP2040 ");
    Serial.print(VERSION);
    Serial.print(" ready: SD=");  Serial.print(sd_init_flag);
    Serial.print(" SCD41=");      Serial.print(scd_ready);
    Serial.print(" SGP40=");      Serial.print(sgp_ready);
    Serial.print(" SHT85=");      Serial.print(sht_ready);
    Serial.print(" AHT20fb=");    Serial.print(aht_ready);
    Serial.print(" probe=");      Serial.println(
        g_sauna_probe == SAUNA_PROBE_SHT85 ? "SHT3x" :
        g_sauna_probe == SAUNA_PROBE_AHT20 ? "AHT20" :
        g_sauna_probe == SAUNA_PROBE_SCD41 ? "SCD41-PROXY" : "NONE");

    /* Watchdog-Timer starten - wenn loop() mal haengen sollte
     * (z.B. durch defekten I2C-pullup an der Sauna-Kabelstrecke),
     * resetet sich der Chip nach 8 s selbst.                       */
    rp2040.wdt_begin(SSC_WATCHDOG_MS);
    slog("watchdog armed (8 s)");

    g_last_tick_ms = millis();
}

void loop() {
    uint32_t now = millis();

    /* Watchdog alive halten - wird jeden Loop-Durchlauf aufgefrischt.
     * Wenn sht85_read_and_send() oder sensor_scd4x_read_and_send()
     * mal laenger als SSC_WATCHDOG_MS haengen, triggert der WDT den
     * Chip-Reset - erwuenschtes Verhalten, Session-File wird beim
     * naechsten Boot bereinigt, ESP32 bekommt sauberen Restart.     */
    rp2040.wdt_reset();

    /* Aufguss-Boost-Ablauf: nach 120 s zurueck auf Basis-Intervall.
     * Signed-diff gegen millis()-Rollover (nach 49.7 Tagen Dauerbetrieb). */
    if (g_boost_until_ms && (int32_t)(now - g_boost_until_ms) >= 0) {
        g_boost_until_ms = 0;
        g_interval_ms = g_interval_base;
        slog("aufguss-boost ende, zurueck zu basistakt");
    }

    if (now - g_last_tick_ms >= g_interval_ms) {
        g_last_tick_ms = now;

        /* Messzyklus: Sauna zuerst (wichtigste Daten), dann Vorraum.
         * Sauna-Fuehler ist entweder SHT85 oder als Fallback AHT20.
         * Vorraum-Sensoren bekommen nur jeden 2. Tick (weniger Daten-
         * Traffic, SCD41 misst eh nur alle ~5 s neu).                */
        static uint8_t slow_divider = 0;
        if (g_sauna_probe == SAUNA_PROBE_SHT85) sht85_read_and_send();
        else if (g_sauna_probe == SAUNA_PROBE_AHT20) aht20_read_and_send();
        if ((slow_divider++ & 1) == 0) {
            if (scd_ready) sensor_scd4x_read_and_send();
            if (sgp_ready) sensor_sgp40_read_and_send();
        }

        /* Fuer laufende Session: Sample auf SD schreiben */
        if (g_session_active) session_write_sample();

        /* Probe-State alle 15 s an ESP32 senden - damit UI ggf. einen
         * DEV-MODE-Badge oder SD-Warnung anzeigen kann.             */
        static uint32_t last_probe_send = 0;
        if (now - last_probe_send > 15000) {
            last_probe_send = now;
            uint8_t ps[3] = { PKT_TYPE_PROBE_STATE,
                              (uint8_t)g_sauna_probe,
                              (uint8_t)(sd_init_flag ? 1 : 0) };
            raw_packet_send(ps, sizeof(ps));
        }

        /* Alle 30 s: I2C-Scan bei nicht-ready-Fuehler, damit wir im
         * Log sehen koennen, welche Slaves tatsaechlich antworten. */
        static uint32_t last_scan = 0;
        if (!sht_ready && !aht_ready && now - last_scan > 30000) {
            last_scan = now;
            i2c_scan_log("periodic");
        }

        /* Alle 8 s: wenn kein externer Fuehler da ODER der Fuehler viele
         * fails akkumuliert hat (>30 = mindestens 30 s hart kaputt), neu
         * probieren. So faengt der RP2040 einen spaeter angesteckten oder
         * nach Brownout recovernden SHT35 automatisch ohne Reboot auf. */
        static uint32_t last_reprobe = 0;
        bool need_reprobe = (!sht_ready && !aht_ready) ||
                            (g_sauna_probe == SAUNA_PROBE_SHT85 && g_sht85_fails > 30);
        if (need_reprobe && now - last_reprobe > 8000) {
            last_reprobe = now;
            if (sht85_init()) {
                sht_ready = true;
                g_sht85_fails = 0;
                g_sauna_probe = SAUNA_PROBE_SHT85;
                slog("re-probe: SHT3x jetzt da -> wechsle zu echtem Fuehler");
            } else if (aht20_fallback_init()) {
                aht_ready = true;
                g_sauna_probe = SAUNA_PROBE_AHT20;
                slog("re-probe: AHT20 fallback erkannt");
            }
        }

        /* Alle 15 s: wenn SD nicht bereit, versuch neu zu mounten. */
        static uint32_t last_sd_retry = 0;
        if (!sd_init_flag && now - last_sd_retry > 15000) {
            last_sd_retry = now;
            sd_init_flag = sd_try_init();
        }

        /* Alle 60 s health-summary: Fehlerzaehler + Interval.
         * WDT sowohl vor als auch nach dem Block fuettern: wenn der USB-CDC
         * Host-Puffer voll ist, kann Serial.print() einige hundert ms blocken,
         * im worst-case zusammen mit einem langsamen SD-flush das 8s-Budget
         * sprengen. NAN-Werte explizit durch 0.0 ersetzen - manche Arduino-
         * Cores haben in der dtostrf-Pfad fuer NAN noch Probleme. */
        static uint32_t last_health = 0;
        if ((int32_t)(now - last_health) >= 60000) {
            last_health = now;
            rp2040.wdt_reset();
            float t_out  = isnan(g_sauna_temp) ? 0.0f : g_sauna_temp;
            float rh_out = isnan(g_sauna_rh)   ? 0.0f : g_sauna_rh;
            const char *probe_str =
                g_sauna_probe == SAUNA_PROBE_SHT85 ? "SHT3x" :
                g_sauna_probe == SAUNA_PROBE_AHT20 ? "AHT20" :
                g_sauna_probe == SAUNA_PROBE_SCD41 ? "SCD41-PROXY" : "NONE";
            /* RP2040 die-temp (arduino-pico analogReadTemp): zeigt ob der
             * ESP32-Nachbar den RP2040 thermisch mitreisst. Idle ~30-35 degC,
             * WiFi-TX-burst ~50-55 degC, >80 degC = thermal runaway. */
            float rp_die_c = analogReadTemp();
            Serial.print(LOG_TAG);
            Serial.print("health: intvl=");     Serial.print(g_interval_ms);
            Serial.print("ms probe=");          Serial.print(probe_str);
            Serial.print(" temp=");             Serial.print(t_out, 1);
            Serial.print(" rh=");               Serial.print(rh_out, 1);
            Serial.print(" fails=");            Serial.print(g_sht85_fails);
            Serial.print(" session=");          Serial.print(g_session_active);
            Serial.print(" sd=");               Serial.print(sd_init_flag);
            Serial.print(" rp_die=");           Serial.print(rp_die_c, 1);
            /* v0.2.7: boot-counter + last-reset-reason jede minute im
             * health-log. Einfach zu grep'en, sichtbar ohne INFO-screen.
             * v0.2.8: rst zeigt 3-state (POR/WDT/SOFT), plus UART-counter
             * pkt (total) + unk (unknown cmd-byte). Wenn bei einem
             * reboot-loop `unk` bei jedem boot hochgeht, ist UART-traffic
             * die ursache. Wenn unk=0 aber rst=SOFT, war's vermutlich
             * hardfault aus anderer quelle.                               */
            const char *rr_h_txt =
                g_last_reset_reason == SSC_RST_POR  ? "POR"  :
                g_last_reset_reason == SSC_RST_WDT  ? "WDT"  :
                g_last_reset_reason == SSC_RST_SOFT ? "SOFT" : "???";
            Serial.print(" boot=");             Serial.print(g_boot_count);
            Serial.print(" rst=");              Serial.print(rr_h_txt);
            Serial.print(" pkt=");              Serial.print(g_pkt_rx);
            Serial.print(" unk=");              Serial.println(g_pkt_rx_unk);
            rp2040.wdt_reset();
        }
    }

    myPacketSerial.update();
    if (myPacketSerial.overflow()) {
        /* Overflow = PacketSerial-Decoder ist desynced und schluckt alles
         * bis zum naechsten 0x00-Frame-Terminator. Nach einem laengeren
         * Stream-Readback oder einem Glitch kann das "permanent stuck"
         * werden - RP2040 ignoriert dann alle ESP32-Commands. Recovery:
         * Instanz via placement-new neu aufbauen, Stream + Handler neu
         * binden. Cooldown 2 s damit wir nicht in einer re-init-Schleife
         * landen wenn ESP32 gerade wirklich viel schickt. */
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
}
