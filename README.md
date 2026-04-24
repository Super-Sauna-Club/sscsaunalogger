# sscsaunalogger — Sauna-Logger für den SenseCAP Indicator D1/D1S

Ein Zwei-Prozessor-Datenlogger für die finnische Sauna — misst Kabinentemperatur, Luftfeuchte, Aufguss-Spitzen und Vorraumklima im Sekundentakt, schreibt pro Session eine CSV auf SD-Karte und exportiert optional nach MariaDB. Basiert auf dem [SenseCAP Indicator D1/D1S](https://www.seeedstudio.com/SenseCAP-Indicator-D1S-p-5643.html) von Seeed Studio.

Als Kabinen-Fühler kommt ein **FS400-SHT35** zum Einsatz — das ist der Sensirion **SHT35**-Chip in einem IP68-Edelstahl-Gehäuse mit fest angegossenem **2 m Silikonkabel** (200 °C-rated). Der SHT35 gehört zur SHT3x-Familie (SHT30/SHT31/SHT35/SHT85) und ist I²C-protokollkompatibel, deshalb redet die Firmware intern von „SHT85"-Code-Pfaden — dasselbe Protokoll, dasselbe Datenblatt-Timing.

Gebaut und betrieben vom **[SuperSaunaClub](https://supersauna.club)**.

---

## Was das Ding macht

- **Sample-Rate:** Basistakt **1 Hz** (alle 1000 ms ein Mess-Tick). Beim `AUFGUSS`-Kommando schaltet der RP2040 für **120 Sekunden** auf **2 Hz** (alle 500 ms) und fällt dann automatisch zurück auf 1 Hz. Der SHT85 in der Kabine wird **jeden Tick** gelesen (1 bzw. 2 Hz); die Vorraum-Sensoren SCD41 und SGP40 nur **jeden zweiten Tick** (0.5 bzw. 1 Hz) — sie messen intern eh nur alle ~5 s neu. Das Basis-Intervall lässt sich via `COLLECT_INTERVAL`-Kommando (0xA0) zur Laufzeit zwischen 250 ms und 60000 ms umstellen.
- **FS400-SHT35 am 2 m Silikonkabel** misst in der Kabine (Sensor bis 105 °C spec, Kabel 200 °C rated), interner SCD41 misst CO₂/Temp/RH im Vorraum, SGP40 liefert VOC-Index (nach 5 min Warmup).
- **Session-basierte Aufzeichnung:** Start → (mehrere Aufgüsse mit Namen/Notiz) → End. Jede Session bekommt eine CSV auf SD und einen Metadaten-Eintrag in NVS. UI für Live-Anzeige, History-Liste, Detail-Chart, Bearbeiten, Löschen.
- **Saunameister-Verwaltung:** Operator-Dropdown mit den Stammgästen, Teilnehmer-Zähler, Notizen-Feld.
- **Optional MariaDB-Export:** automatischer Push in eine zentrale Datenbank (Default `postl.ai:3308`), damit mehrere Anlagen gemeinsam ausgewertet werden können. Per Default seit fw_mig=3 **aus** — explizit einschalten.
- **Extrem defensive Firmware:** I²C-Bus-Unlock bei Kabel-Glitch, I²C-Clock-Stretch-Timeout 25 ms mit Controller-Reset, SD-Recovery-Scan beim Boot, Sensor-Range-Checks nach CRC, Watchdog 8 s mit kurzem Feed-Abstand, Reset-Reason-Logging. Seit v0.2.4 crasht das Gerät im Saunabetrieb nicht mehr.
- **Zeit-sicher ohne RTC** (v0.2.7): NVS-persistierte Last-Known-Time, NTP-Auto-Retry bei jedem WLAN-Reconnect, Europe/Vienna-Default-TZ, `⚠`-Badge im Home-Header wenn Zeit-Quelle nicht NTP-synced, Session-Start-Gate gegen falsche Timestamps. Manuelles Zeit-Setzen im Settings-Dialog als Fallback.
- **Live-Crash-Diagnose** (v0.2.7): Boot-Counter + Reset-Reason beider Chips im Settings → `ZEIT + DIAGNOSE` direkt am Gerät sichtbar. Ohne Serial-Capture sofort erkennbar ob ESP32 oder RP2040 zuletzt gecrasht ist und warum (POR, WATCHDOG, PANIC, BROWNOUT, …).

---

## Hardware

| Komponente | Details |
|---|---|
| Board | Seeed SenseCAP Indicator D1 oder D1S (ESP32-S3 + RP2040 + 4" 480×480 IPS Touch) |
| Externer Sauna-Fühler | **FS400-SHT35** — Sensirion SHT35-Chip in IP68-Edelstahl-Hülse mit 2 m Silikon-Kabel (200 °C rated, PVC taugt bei > 80 °C nicht). I²C-Adresse 0x44. |
| Interne Sensoren | SCD41 (CO₂/T/RH @ 0x62), SGP40 (VOC @ 0x59) — auf PCB verbaut |
| Fallback-Fühler | AHT20 @ 0x38 (optional, wenn kein SHT3x verfügbar — nur bis 85 °C!) |
| Speicher | microSD-Karte, **FAT32 formatiert** (exFAT funktioniert nicht, Arduino-SD-Lib kann es nicht) |
| Stromversorgung | 5 V über USB-C, ≥ 2 A empfohlen (65 W PD Power Bank getestet) |

### Kaufteile & grobe Kosten

Stand 2026-04, ca.-Preise inkl. Versand nach AT/DE:

| Teil | Bezugsquelle (Beispiel) | Richtpreis |
|---|---|---|
| SenseCAP Indicator D1S | [Seeed Studio Bazaar](https://www.seeedstudio.com/SenseCAP-Indicator-D1S-p-5643.html) | ~85 € |
| FS400-SHT35 mit 2 m Silikonkabel | AliExpress-Suche: [FS400-SHT35](https://www.aliexpress.com/w/wholesale-FS400-SHT35.html) | ~25 € |
| microSD-Karte 32 GB Class 10 | beliebig (SanDisk, Kingston, …) | ~8 € |
| USB-C-Netzteil ≥ 2 A oder PD Power Bank | beliebig | ~10 € |
| **Gesamt** | | **~130 €** |

Preise stark zeit- und versandabhängig, Bandbreite ±20 %. Bei mehreren Anlagen im Verein lohnt sich Sammelbestellung.

### Pin-Belegung am RP2040 (fix durchs D1-Board)

| Funktion | Pin |
|---|---|
| Wire SDA / SCL | GP20 / GP21 |
| SPI1 (SD) SCK / MOSI / MISO / CS | GP10 / GP11 / GP12 / GP13 |
| Serial1 (UART zum ESP32) TX / RX | GP16 / GP17 |
| Sensor-Power-Enable | GP18 |

### Verkabelung der Sauna-Probe (FS400-SHT35)

Der FS400-SHT35 wird typischerweise mit 4 Drähten geliefert. Drahtfarben variieren zwischen Händlern — immer das Datenblatt des gekauften Exemplars prüfen. Die häufigste Belegung:

| FS400-SHT35 Draht | Funktion | Pin am Indicator D1S (RP2040) | Typ. Farbe* |
|---|---|---|---|
| VDD | 3.3 V | gespeist über **GP18** (Sensor-Power-Enable, HIGH = on) → 3V3-Pad | rot |
| GND | Masse | GND-Pad | schwarz |
| SDA | I²C-Daten | **GP20** (Wire SDA) | gelb oder grün |
| SCL | I²C-Takt | **GP21** (Wire SCL) | grün oder weiß |

\* Farben NUR als Orientierung — der Händler-Druck auf dem Steckeretikett zählt. Falsch verdrahtet = SHT-Scan findet 0x44 nicht, Log zeigt `SHT3x not found on I2C 0x44 or 0x45`.

### Hinweise zum Aufbau

- **Silikonkabel belassen** wie geliefert (4-adrig, 200 °C rated). Ein Verlängern über Lüsterklemmen/Schraubterminal in der heißen Zone ist schlechte Idee — dort kondensiert Wasser, die Klemmen oxidieren.
- **Drip-Loop** am Kabel-Durchgang durch die Kabinenwand: U-Bogen direkt außerhalb, Tiefpunkt unter dem Stecker. Kondensat tropft ab statt in die Pins zu wandern.
- I²C-Bus läuft firmware-seitig auf **50 kHz** — das Rise-Time-Budget bei 2 m Kabel + den On-Board-4.7 kΩ-Pullups reicht damit komfortabel. Wer noch robuster fahren will: zusätzliche 2.2 kΩ-Pullups am Sauna-Ende, dann kann die Clock zurück auf 100 kHz.
- Der FS400-SHT35-Kopf sollte _nicht_ im Aufgussstrahl sitzen — Flüssigwasser zerstört langfristig den Polymer-Feuchtesensor. Montagehöhe typisch 10–20 cm unter der Decke, nicht direkt über den Steinen.
- Seit Firmware v0.2.4 mit `Wire.setTimeout(25, true)` ist das Setup tolerant gegen Clock-Stretching — auch bei Kabel-Glitchen durch WLAN-Bursts oder Bewegung.

---

## Architektur

Zwei-Prozessor-Aufteilung:

```
┌─────────────────────┐   UART1 @ 115200, COBS   ┌─────────────────────┐
│      ESP32-S3       │ ◄──────────────────────► │       RP2040        │
│                     │                           │                     │
│ · LVGL-UI 480×480   │                           │ · I²C-Sensoren      │
│ · Session-Logik     │                           │ · FS400-SHT35 @ 2m  │
│ · Historie in NVS   │                           │ · SD-Schreiben      │
│ · WLAN / NTP / HTTP │                           │ · CSV pro Session   │
│ · MariaDB-Export    │                           │ · Watchdog-gesichert│
└─────────────────────┘                           └─────────────────────┘
         │                                                 │
         │                                         ┌──────────┐
    ┌─────────┐                                    │ microSD  │
    │ Display │                                    │ (FAT32)  │
    └─────────┘                                    └──────────┘
```

- Der **ESP32-S3** hält das UI, die Session-Metadaten (NVS), das WLAN, die MariaDB/HTTP-Anbindung und kommandiert den RP2040.
- Der **RP2040** macht die Sensor-Aufnahme, schreibt die Roh-CSV auf SD und reagiert auf Kommandos vom ESP32.
- **Protokoll:** COBS-gerahmt über UART1 @ 115200 baud, Pakete `[type(1B)][payload…]`. PacketSerial-Buffer 512 B auf dem RP2040. Kommando-Typen:

  | Byte | Name | Payload | Richtung |
  |---|---|---|---|
  | `0xA0` | `COLLECT_INTERVAL` | 4 B BE-uint32 (ms, 250…60000) | ESP32 → RP |
  | `0xA3` | `SHUTDOWN` | — | ESP32 → RP |
  | `0xA4` | `SESSION_START` | ≤23 B Session-ID (`[A-Za-z0-9_-]`) | ESP32 → RP |
  | `0xA5` | `SESSION_AUFGUSS` | ≤47 B Aufguss-Name | ESP32 → RP |
  | `0xA6` | `SESSION_END` | — | ESP32 → RP |
  | `0xA7` | `SD_READBACK` | 12 B: req_id(2) + sid(24, NUL-pad) + offset(4) + max_len(2) | ESP32 → RP |
  | `0xA8` | `GET_RP_STATUS` (v0.2.7) | — | ESP32 → RP |
  | `0xBF` | `SAUNA_TEMP` | 4 B Float (°C) | RP → ESP32 |
  | `0xC0` | `SAUNA_RH` | 4 B Float (%) | RP → ESP32 |
  | `0xC1` | `SD_READBACK_CHUNK` | 13 B Header + Daten | RP → ESP32 |
  | `0xC2` | `PROBE_STATE` | 2 B: probe_type + sd_init_flag | RP → ESP32 |
  | `0xC3` | `RP_STATUS` (v0.2.7) | 5 B: boot_count(4 LE) + reset_reason(1) | RP → ESP32 |

- **SD-Readback:** Der ESP32 liest historische CSVs seitenweise zurück, damit das Detail-Chart auf dem Display die Original-Samples zeigt ohne dass der ESP32 die Karte direkt anfasst. Max-Payload RP-seitig 200 B pro Chunk, ESP32-Parser-Robustheit limitiert es praktisch auf **64 B Payload** pro Chunk (größere Frames fragmentieren den COBS-Decoder bei 128-B-UART-FIFO).
- **Live-Sensor-Daten** werden via 0xBF/0xC0 bei jedem Tick gepusht (kein Polling vom ESP32 aus).

### Session-Historie-Performance (v0.2.6)

Historische Sessions lassen sich in **≤ 5 Sekunden** aus der SD zurücklesen und als Chart anzeigen — egal wie lang die Session war:

| Mechanismus | Zweck |
|---|---|
| **Downsampling auf RP2040** (target 60 Samples) | Bei großen Sessions (> 4 KB CSV) wird vor dem Senden decimiert: jeder N-te Sample + **alle Aufguss-Marker** behalten. 30-min-Session mit 1800 Samples → 60 Chart-Punkte → ca. 1 KB UART-Traffic. |
| **File-Cache auf RP2040** | SD.open() kostet 50-150 ms. Beim ersten Chunk-Request einer Session wird die decimierte CSV in einen 6 KB DRAM-Buffer geladen. Folge-Chunks lesen aus dem Buffer → ~30 ms pro Chunk statt 300. |
| **PSRAM-Akkumulator auf ESP32** | `rx_sd_chunk` macht nur noch `memcpy` in einen PSRAM-Buffer. CSV-Parse erst am Ende einmal statt pro Chunk. Hält den UART-Hot-Path blitz-schnell. |
| **Non-blocking Event-Posts** im UART-Task | `esp_event_post_to(..., pdMS_TO_TICKS(5))` statt `portMAX_DELAY` — verhindert dass die UART-RX-Task bei voller Event-Queue blockiert und die UART-FIFO überläuft. |
| **Watchdog mit Resume-Retry** | Bei UART-Stall (3 s ohne Chunk-Fortschritt) wird der Request vom letzten bekannten Offset neu gesendet (max 10 Retries). |
| **5 s Hard-Limit + Partial-DONE** | Unabhängig vom Retry-Status postet der Watchdog nach 5 s das DONE-Event. Die UI zeigt die Teilkurve statt ewig "Lade Daten..." zu hängen. |
| **Dynamische Zeitachsen-Labels** im Detail-Chart | Unter dem Chart 5 Labels adaptiv zur Session-Dauer: `0 \| 4min \| 8min \| 12min \| 17min` für 17-min, `0h00 \| 0h22 \| 0h45 \| 1h07 \| 1h30` für 90-min, `0:00 \| 2:00 \| 4:00 \| 6:00 \| 8:00` mit Sekunden-Auflösung bei <10 min. |
| **Duration in der History-Liste** | Jede Session zeigt jetzt vorne die Dauer (`15min  85 C  65%  3`) — sowohl im Home-Screen als auch in der vollen History-Liste, plus im Detail-Screen-Header. |

Kompromiss: Bei extrem langen Sessions (> 30 min) geht Sample-Auflösung verloren. Die Roh-CSV bleibt auf SD vollständig — dort kann man jederzeit manuell nachschauen.

### Defensiv-Mechanismen der RP2040-Firmware (v0.2.4)

| Mechanismus | Zweck |
|---|---|
| **I²C-Clock 50 kHz** (statt 100 kHz) | Rise-Time-Budget 2 µs statt 1 µs — komfortabel in-spec für 2 m Silikonkabel mit On-Board-4.7-kΩ-Pullups und ~300 pF Kabelkapazität. |
| **`Wire.setTimeout(25, true)`** — **der Fix in v0.2.4** | Setzt auf arduino-pico die Clock-Stretch-Timeout-API (in Millisekunden). `reset_with_timeout=true` setzt den I²C-Controller nach Timeout zurück, sonst bleibt er in undefined state hängen. **Ohne diesen Call blockierte Wire bis 1000 ms (Stream-Default)**, was bei SHT85-Clock-Stretch am 2 m Kabel WDT-Reboot-Loops ausgelöst hat. Der alte `#ifdef WIRE_HAS_TIMEOUT`-Gate in v0.2.3 war auf arduino-pico toter Code. |
| **I²C-Bus-Unlock** (9-Clock-Recovery + STOP vor jedem `Wire.begin()`) | Löst SDA-stuck-LOW nach Slave-Brownout. Ohne das gibt es Boot-Loop-Perma-Blockade. Läuft beim Boot und im SHT85-Retry-Pfad nach 20 konsekutiven Fehlern. |
| **SD-CS pre-drive HIGH** vor SPI1-Init | Verhindert Karten-Undefined-State nach WDT-Reset. Ohne das floatet GP13 während der 74-Clock-Wakeup-Sequenz von `SD.begin()`. |
| **SCD41 1 s-Wake-Delay** (interleaved mit USB-Wait + SD-Init) | Respektiert Datenblatt-Anforderung, verhindert NACK-getriggerten Bus-Lock. |
| **WDT-Bracket um `handle_sd_readback`** (v0.2.4) | SD-Seek + COBS-Encode können 100-300 ms dauern, synergetisch mit dem Wire-Timeout. |
| **Concurrent-Access-Guard** im SD-Readback | Verweigert gleichzeitiges Read/Write auf dieselbe CSV → verhindert FAT-Corruption. |
| **Boot-Recovery-Scan** `/sessions/*.csv < 32 Bytes` löschen (bounded auf 100 Einträge, WDT-gefüttert) | Keine Orphan-Cluster-Akkumulation über Monate. |
| **SHT85 physical-sanity Range-Check** (−10 … +120 °C / 0 … 105 %RH) nach CRC | CRC-valid-aber-geglitchte Werte fliegen raus und zählen als Fail. |
| **SHT85 Heater-Cycle** (0x306D, 33 mW, 1 s) bei RH stuck ≥ 60 % für 60 s | Löst Polymer-Kondensat nach Aufguss, 10-min-Lockout, nie während Boost. |
| **SCD41 ASC deaktiviert** (`setAutomaticSelfCalibrationEnabled(false)`) | Verhindert dauerhafte CO₂-Kalibrierungs-Drift in geschlossenem Vorraum. |
| **SGP40 5-min-Warmup** (NaN im VOC bis Baseline-Learning fertig) | Keine Müllwerte in den ersten 5 min nach Boot. |
| **Watchdog 8 s** + Reset-Reason im Boot-Banner (`watchdog_caused_reboot()`) | Bei Crash sofort sichtbar ob WDT oder Brownout. |
| **RP2040 Die-Temp** (`analogReadTemp()`) im Health-Log | Frühwarnung bei thermal runaway durch ESP32-Nachbarn. |

Alle Änderungen ggü. dem Upstream sind minimal-invasiv und rückwärtskompatibel zum Seeed-Indicator — Kommandoprotokoll zum ESP32 ist unverändert.

### Zeit-Härtung + Crash-Observability (v0.2.7)

Das SenseCAP D1S hat **keine battery-backed RTC** — ohne Strom ist die Uhrzeit beim Boot weg. Dazu kommt dass zuvor ein NTP-Retry-Loop-Bug Zeit-Sync nach dem ersten WLAN-Connect silent aufgab. Beides ist in v0.2.7 adressiert:

| Mechanismus | Zweck |
|---|---|
| **NVS-persistierte Last-Known-Time** (`indicator_time.c`) | Alle 60 s wird die aktuelle Wall-Clock + `time_source_t`-State (NTP_SYNCED / MANUAL) in NVS (Key `time_state`) geschrieben — aber nur wenn die Quelle vertrauenswürdig ist. Beim nächsten Boot ohne WLAN wird `max(NVS+60s, compile_time)` als Start-Zeit gesetzt statt einer Jahr-alten Compile-Zeit. |
| **NTP-Retry bei jedem WLAN-Reconnect** | Der pre-existing `fist`-Flag-Bug (NTP-Sync nur beim allerersten Connect) ist entfernt. Jetzt triggert **jedes** `is_network=true`-Event einen SNTP-Restart — aber nur wenn die Source stale ist (>10 min oder nicht NTP_SYNCED). Dazu periodischer Watchdog alle 5 min der `sntp_stop+init` macht wenn Source noch nicht NTP ist. |
| **Timezone-Default Europe/Vienna** (`CET-1CEST,M3.5.0,M10.5.0/3`) | Bevor `indicator_city` via geoIP eine genauere Zone liefert, wird CEST/CET mit DST-Regeln angewendet. Standort ist fix für dieses Projekt (Oberes Piestingtal). Die auto-erkannte Zone wird nach dem Lookup in NVS persistiert — spätere Boots haben die TZ sofort ohne Netzwerk-Delay. |
| **⚠ Warn-Badge links neben der Uhrzeit** (Home-Screen) | Sichtbar wenn `time_source != NTP_SYNCED` oder letzte Sync > 10 min alt. Signal an den User: "Die Zeit oben könnte falsch sein." |
| **Session-Start-Gate** | Beim Druck auf `STARTEN` prüft die UI ob die Zeit frisch NTP-synced ist. Wenn nicht, erscheint ein Confirm-Dialog `ZEIT NICHT GEPRÜFT` — User kann explizit abbrechen und zuerst Zeit fixen, oder trotzdem starten. Schützt die History vor Sessions mit falschem Timestamp. |
| **Manual Time-Set Dialog** (Settings → `ZEIT MANUELL SETZEN`) | LVGL-Modal mit 6 Rollern (Jahr 2024–2099 / Monat / Tag / Std / Min / Sek). Apply setzt `source=MANUAL`, persistiert sofort in NVS. Fallback für Fälle ohne WLAN. |
| **ESP32-Boot-Counter + Reset-Reason** (`main.c`) | NVS-Key `boot_info` zählt jeden Boot hoch, `esp_reset_reason()` wird sofort in `app_main()` gecaptured. Event `VIEW_EVENT_BOOT_INFO` postet's an die UI. |
| **RP2040-Boot-Counter in EEPROM** (flash-emuliert, 256 B reserved mit magic 0x55AA5AA5) + **`watchdog_caused_reboot()`** | Boot-Banner zeigt `boot_count=N`, Health-Log jede Minute `boot=N rst=WDT/POR`. |
| **Neuer UART-Command 0xA8 `GET_RP_STATUS`** / Response 0xC3 | ESP32 fragt den RP2040 alle 60 s nach Boot-Status an, RP2040 antwortet mit `[boot_count:4B LE][reset_reason:1B]`. UI-Event `VIEW_EVENT_RP_BOOT_INFO` zeigt's in Settings → ZEIT + DIAGNOSE. |
| **Last-Screen-Persist** | Bei jedem Screen-Wechsel schreibt die UI `last_scr` in NVS. Bei unclean Reboot kann man so nachvollziehen auf welchem Screen das Gerät war. |

**Architektur-Entscheidung:** Keine Hardware-RTC (z. B. DS3231) nachrüsten — reine Software-Lösung war dem User lieber, das Silikonkabel ist bereits verlegt und weitere Hardware-Änderungen wurden ausgeschlossen. Die NVS-basierte Persistierung ist nach 1× NTP-Sync praktisch immun gegen Stromausfall (worst case: 60 s Zeit-Drift, für Session-Granularität irrelevant).

**Settings → ZEIT + DIAGNOSE** (neu) zeigt live:

```
ZEIT-QUELLE: NTP (vor 2min)
BOOT-COUNTER ESP32: 42   LETZTER RESET: POWERON
BOOT-COUNTER RP2040: 17  LETZTER RESET: POR/NORMAL
```

— plus den Button `ZEIT MANUELL SETZEN`. Sichtbar zwischen der „Zukunfts-Feature"-API-Section und der Danger Zone.

### Ping-Handle-Fix (v0.2.7.1)

Latent-Bug im Seeed-Fork entdeckt: `indicator_wifi.c::__ping_start` hat den Return von `esp_ping_new_session` nicht geprüft. Bei Heap-Fragmentation (die sich durch die v0.2.7-Ergänzungen leicht verschoben hat) gab `new_session` einen NULL-Handle zurück, `esp_ping_start(NULL)` hat dann im `xTaskGenericNotify`-Assert einen `PANIC(4)`-Reboot-Loop ausgelöst — direkt nach jedem `WIFI_STA_CONNECTED`. Fix: NULL-Init, Fehler-Check, Fallback `is_network=is_connected` wenn Session-Create scheitert damit NTP/TZ-Lookup trotzdem triggern. Andere Funktionen im Seeed-Fork die Return-Werte ignorieren könnten ähnliche latente Bugs haben — bei zukünftigen Audits mitchecken.

---

## Build & Flash

### Voraussetzungen (Fedora / Linux)

Es gibt zwei Setup-Scripts die alles automatisch installieren:

```bash
./setup-fedora.sh           # ESP-IDF v5.1 + Toolchain
./setup-arduino-cli.sh      # Arduino-CLI + RP2040-Core + Libraries
```

**Nicht mit anderen ESP-IDF-Versionen bauen** — die v5.2+-Header haben breaking changes, v5.0 fehlt einiges.

### RP2040 zuerst flashen

```bash
cd SenseCAP_Indicator_RP2040
arduino-cli compile --fqbn rp2040:rp2040:rpipico --output-dir ./build .
```

Wenn der Pico läuft, kann `arduino-cli` direkt ohne BOOTSEL-Taste flashen (nutzt 1200-bps-Magic-Reset):

```bash
arduino-cli upload --fqbn rp2040:rp2040:rpipico -p /dev/ttyACM0 --input-dir ./build .
```

Wenn der Pico in einem Boot-Loop steckt: BOOTSEL gedrückt halten beim Einstecken, dann die `build/SenseCAP_Indicator_RP2040.ino.uf2` auf den erscheinenden `RPI-RP2`-Mount ziehen.

### ESP32 danach flashen

```bash
source ~/esp-idf/export.sh
cd SenseCAP_Indicator_ESP32
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**PSRAM Octal 120 MHz Patch** einmalig anwenden laut [Seeed-Wiki](https://wiki.seeedstudio.com/SenseCAP_Indicator_ESP32_Flash/) — sonst bleibt das Display dunkel.

### Diagnose am RP2040

Serial-Output auf `/dev/ttyACM0` @ 115200 baud:

```
[SAUNA] BOOT ssc-v0.2.7 reset_reason=POWER/NORMAL boot_count=42
[SAUNA] I2C-Scan (boot): 0x38 0x44 0x59 0x62
[SAUNA] SD: initialisiert
[SAUNA] SHT3x ready @ 0x44 (SHT35/SHT85 kompatibel): 24.53 degC, 32.10 %RH
[SAUNA] SGP40 ready
[SAUNA] SCD41 ASC disabled (closed-room drift protection)
[SAUNA] RP2040 ssc-v0.2.7 ready: SD=1 SCD41=1 SGP40=1 SHT85=1 AHT20fb=0 probe=SHT3x
[SAUNA] watchdog armed (8 s)
...
[SAUNA] health: intvl=1000ms probe=SHT3x temp=25.5 rh=31.0 fails=0 session=0 sd=1 rp_die=41.2 boot=42 rst=POR
```

Der `reset_reason` beim Boot sagt dir bei einem Crash was los war:
- `WATCHDOG` → Firmware-Problem, Code hat >8 s geblockt. Die letzten Zeilen vor dem Reset zeigen wo.
- `POWER/NORMAL` → Brownout, ESP32-Reset-Trigger oder normaler Reboot. Netzteil / Kabel / USB-Hub prüfen.

---

## Bedienung

### Live-Flow (während einer Session)

1. **HOME-Screen** zeigt Kabine (groß) + Vorraum (kompakt) + die letzten Sessions + den START-Button.
2. **START** → Live-Screen: Status-Pill oben, Val-Cards für Temp/RH/Peak, 3-Min-Chart, Buttons `AUFGUSS MARKIEREN` / `STOPPEN` / `ABBRECHEN`. Ab diesem Moment legt der RP2040 die CSV an und schreibt jede Sekunde eine Zeile.
3. **AUFGUSS MARKIEREN** (Button im Live-Screen) — was genau passiert:
   - setzt einen Zeitstempel-Marker in der **aktuellen** CSV-Zeile (Spalte `aufguss`)
   - fordert den Namen des Aufgusses (optionaler Text, z. B. "Eukalyptus")
   - schaltet die Sample-Rate **für 120 s auf 2 Hz** (Boost), dann automatisch zurück auf 1 Hz
   - der kleine `N Aufg.`-Zähler oben rechts zählt mit
4. **STOPPEN** öffnet die **Summary**-Form: Saunameister aus Dropdown, Teilnehmerzahl (± Stepper), Notizen-Textfeld. `SAVE` legt die Metadaten in NVS an (Namespace `sauna_sess`) und stößt — falls aktiviert — den MariaDB-/HTTP-Export an.
5. **ABBRECHEN** verwirft die Session komplett: keine Metadaten, keine CSV (die Teil-CSV wird vom RP2040 gelöscht).

### Nachträglich editieren (History-Flow)

Vom HOME-Screen oder SETTINGS → Detail-Screen einer Session:

- **Detail-Screen** zeigt die volle Kurve aus der CSV (per SD-Readback vom RP2040 zurückgelesen), Aufguss-Marker als Spikes, Peak-Werte, Operator, Notizen.
- **EDIT** öffnet die Summary-Form im Edit-Mode und erlaubt **nachträglich**:
  - Saunameister umsetzen
  - Teilnehmerzahl ändern
  - Notizen ergänzen / korrigieren
  - **Aufguss-Zähler** hochstellen (`aufguss_count`-Stepper, nur im Edit-Mode sichtbar) — z. B. wenn während der Session mal vergessen wurde zu drücken
- **DELETE** löscht die Session (Metadaten in NVS + CSV auf SD).

**Was nachträgliches Editieren _nicht_ kann:**
- keine neuen Marker in die CSV-Kurve nachsetzen (die Samples auf SD sind fertig geschrieben und werden nicht verändert — nur der Metadaten-Zähler geht hoch)
- **kein 2-Hz-Boost rückwirkend** — der Boost galt nur live während der Aufnahme. Eine Session die nur mit 1 Hz aufgezeichnet wurde, bleibt bei 1 Hz. Wer den 2-Hz-Peak in der Kurve haben will, muss live den Button drücken.
- keine Korrektur falscher Sensor-Werte — die CSV ist write-once.

### SETTINGS (Zahnrad-Icon)

In der Reihenfolge von oben nach unten:

- **Saunameister-Liste:** Namen der Stammgäste verwalten (für das Operator-Dropdown in der Summary)
- **WLAN:** SSID + Passwort, landet in NVS
- **MariaDB-Endpunkt** (Default `postl.ai:3308`) + Credentials
- **INFO:** Hersteller/Version/Open-Source-Hinweis, statischer Text
- **ZUKUNFTS-FEATURE: supersauna.club API** (ausgegraut) — Platzhalter für künftigen HTTP-Push der Session-Daten an das Vereins-Dashboard. Server-seitig noch nicht gebaut, UI visuell disabled.
- **ZEIT + DIAGNOSE** (v0.2.7): Live-Diagnostik-Block zeigt aktuelle Zeit-Quelle (NTP/Manuell/NVS-Fallback/Compile-Time) mit Alter + Boot-Counter beider Chips + letzte Reset-Reason. Darunter Button `ZEIT MANUELL SETZEN` → öffnet Modal mit 6 Rollern für Jahr/Monat/Tag/Stunde/Minute/Sekunde.
- **DANGER ZONE** (rot, ganz unten): `ALLE SESSIONS LÖSCHEN` wipet sämtliche Metadaten aus NVS. CSV auf SD bleibt, kann aber nicht mehr aus der UI aufgerufen werden. Confirm-Dialog pflicht.

MariaDB ist seit `fw_mig=3` **per Default aus** — explizit einschalten. Ohne Endpunkt-Config passiert kein Netzwerk-Traffic. HTTP-Export ist als Zukunfts-Feature gerahmt und aktuell ohne Funktion.

### Datenformat der Session-CSV

```
t_elapsed_s,temp,rh,aufguss
0,25.50,31.10,
1,25.51,31.08,
...
42,78.20,55.30,Eukalyptus
43,78.45,62.10,
...
```

- `t_elapsed_s`: Sekunden seit SESSION_START
- `temp`: Kabinentemperatur (°C, FS400-SHT35)
- `rh`: relative Luftfeuchte (%, FS400-SHT35)
- `aufguss`: Name des Aufgusses in der Zeile in der der Marker gesetzt wurde (sonst leer)

### Datenmodell in MariaDB

Die Tabelle wird automatisch angelegt:

```sql
CREATE TABLE IF NOT EXISTS sauna_sessions (
    session_id VARCHAR(32) NOT NULL PRIMARY KEY,
    started_at TIMESTAMP,
    operator VARCHAR(32),
    participant_count INT,
    peak_temp FLOAT,
    peak_rh FLOAT,
    aufguss_count INT,
    notes TEXT
);

CREATE TABLE IF NOT EXISTS sauna_samples (
    session_id VARCHAR(32) NOT NULL,
    t_elapsed_s INT,
    temp FLOAT,
    rh FLOAT,
    aufguss_marker VARCHAR(48),
    INDEX idx_session (session_id, t_elapsed_s)
);
```

---

## Projektstruktur

```
sscsaunalogger/
├── SenseCAP_Indicator_ESP32/       # ESP-IDF-Projekt für die UI-Seite
│   ├── main/
│   │   ├── main.c                  # app_main()
│   │   ├── app_version.h           # SSC_APP_VERSION — zentrale ESP32-Version
│   │   ├── model/                  # Sensor, Session, WiFi, NVS, MariaDB, Time
│   │   ├── view/                   # LVGL-Screens, ui_sauna.c (Haupt-UI)
│   │   ├── controller/
│   │   └── util/cobs.c             # COBS-Enkodierung, gemeinsam mit RP2040
│   ├── components/                 # LVGL, BSP, eingebundene Libs
│   ├── sdkconfig                   # ESP-IDF-Konfig (WiFi-Credentials NICHT hier, sondern in NVS)
│   └── partitions.csv
├── SenseCAP_Indicator_RP2040/      # Arduino-Sketch für den Sensor-Koprozessor
│   └── SenseCAP_Indicator_RP2040.ino
├── flash-guide.sh                  # Kurzanleitung Flash-Reihenfolge
├── setup-fedora.sh
├── setup-arduino-cli.sh
└── ssc-logo.svg
```

---

## Fehlerbilder & Troubleshooting

| Symptom | Wahrscheinliche Ursache | Prüfung |
|---|---|---|
| `fails=N` im Health-Log steigt | I²C-CRC-Fehler am 2 m Kabel | Stecker prüfen, Silikon-Kabel statt PVC, ggf. 2.2 kΩ-Pullups am Sauna-Ende |
| `probe=NONE` nach Boot | FS400-SHT35 wird nicht erkannt, AHT20 auch nicht | I²C-Scan im Boot-Log anschauen, welche Adressen gefunden werden — 0x44 muss da sein; wenn nicht: Verkabelung SDA/SCL vertauscht? |
| `reset_reason=WATCHDOG` nach Crash | Code blockiert > 8 s | Zeilen vor dem Reset in Serial-Capture ansehen |
| `reset_reason=POWER/NORMAL` in Schleife | Brownout | Netzteil auf ≥ 2 A wechseln, USB-Hub entfernen, kurzes dickes Kabel |
| `sd=0` dauerhaft | Karte nicht erkannt oder exFAT | Karte neu in **FAT32** formatieren (Arduino-SD kann kein exFAT) |
| Touch reagiert nicht | FT6336U Hardware-Problem | Display-Flex neu einstecken, 2+ min stromlos |
| VOC-Index = NaN in den ersten 5 min | Normal | 5 min warten, Sensirion-Baseline-Learning |
| RH stuck bei 100 % nach Aufguss | Polymer-Kondensat | Firmware triggert automatisch Heater-Cycle, 10-min-Lockout |
| Uhrzeit beim Start falsch | Strom war weg, kein WLAN seither | `⚠` im Home-Header zeigt „Zeit ungeprüft". Einmal mit WLAN booten → NTP synct. Alternative: `Settings → ZEIT MANUELL SETZEN`. |

### Wenn das Gerät rebootet (v0.2.7+)

Seit v0.2.7 zeigt der **Settings → ZEIT + DIAGNOSE** Block direkt am Gerät:

```
ZEIT-QUELLE: NTP (vor 2min)
BOOT-COUNTER ESP32: 42   LETZTER RESET: POWERON
BOOT-COUNTER RP2040: 17  LETZTER RESET: POR/NORMAL
```

Steigt der Boot-Counter ohne dass du rebootet hast, ist was faul. Der Last-Reset zeigt welcher Chip:
- `WATCHDOG` am RP2040 → I²C-Hang oder SD-Stall, siehe Health-Log-Zeilen VOR dem Reboot
- `PANIC` / `INT_WDT` / `TASK_WDT` am ESP32 → Firmware-Crash, Stack-Trace im Serial-Log nötig
- `BROWNOUT` → Stromversorgung zu schwach, auf USB-PD mit ≥ 2 A wechseln

Zum Serial-Capture beim nächsten Reboot:
- ESP32: `cd SenseCAP_Indicator_ESP32 && idf.py -p /dev/ttyUSB0 monitor`
- RP2040: `arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200`

Beide Logs parallel capturen (zwei Terminals), mind. 60 s laufen lassen. Der Log enthält Reset-Reason + Boot-Counter in der ersten `BOOT`-Zeile direkt nach dem Wiederanlauf.

---

## Versionierung

Die Firmware hat **zwei Versionskonstanten** — eine pro Prozessor, weil RP2040 und ESP32 separat kompilieren und in der Praxis auch einzeln geflasht werden:

| Prozessor | Variable | Datei | Format |
|---|---|---|---|
| ESP32-S3 (UI) | `SSC_APP_VERSION` | `SenseCAP_Indicator_ESP32/main/app_version.h` | Plain SemVer, z. B. `"0.2.7"` |
| RP2040 (Sensor/SD) | `VERSION` | `SenseCAP_Indicator_RP2040/SenseCAP_Indicator_RP2040.ino` (Zeile 33) | mit Prefix: `"ssc-v0.2.7"` |

**Bump-Workflow:** Beide Variablen gleichzeitig anheben, dann RP2040 flashen (Arduino-CLI), dann ESP32 flashen (idf.py). Die ESP32-UI-Info-Zeile (`Settings → INFO`) und der RP2040-Serial-Boot-Banner zeigen die jeweilige Version — so ist sofort sichtbar, ob ein Prozessor den Flash verpasst hat.

Der `SSC_APP_VERSION`-Header ist bewusst der einzige zentrale Ort für die ESP32-Seite — zukünftig wird der String auch im HTTP-POST-User-Agent und im ESP_LOG-Boot-Banner verwendet, damit jeder Export und jedes Log die Version mitliefert.

---

## Lizenz & Dank

Der komplette Code steht unter der **[Apache License 2.0](LICENSE)** — eine freizügige Open-Source-Lizenz.

**Was das praktisch heißt:**

- **Nutzen, Verändern, Weitergeben, auch kommerziell** — alles erlaubt. Keine Copyleft-Pflicht, Änderungen am Code müssen nicht offengelegt werden.
- **Zwei kleine Auflagen bei Weitergabe:** Die [`LICENSE`](LICENSE)-Datei mitgeben, und Urheber im Copyright-Vermerk nennen (nicht so tun als hätte man's selbst geschrieben).
- **Patent-Klausel:** Wer den Code benutzt, bekommt auch automatisch die Nutzungsrechte an Patenten, die die Autoren darauf haben. Wer anschließend die Autoren wegen eben dieser Patente verklagt, verliert die Lizenz — Schutz vor Patent-Trollen.
- **Kein Gewährleistungsanspruch:** Die Firmware wird *"as is"* bereitgestellt. Wenn der Logger deinen Saunaofen nicht absichert oder Messwerte falsch sind, ist das dein Risiko. Wir geben keine Garantien ab, auch keine implizierten.

Kurz: *"Benutz es wie du willst, nenn uns als Autoren, beschwer dich nicht wenn's kaputtgeht."*

Warum Apache 2.0: Der Upstream (Seeed Studio) steht unter dieser Lizenz, Forks müssen dieselbe oder eine kompatible verwenden. Ein Pluspunkt ggü. MIT ist die explizite Patent-Klausel — im Hobby-Kontext unwichtig, bei kommerzieller Nachnutzung aber Gold wert.

**Fork-Basis:** [Seeed-Studio/SenseCAP_Indicator_ESP32](https://github.com/Seeed-Studio/SenseCAP_Indicator_ESP32) und [Seeed-Studio/SenseCAP_Indicator_RP2040](https://github.com/Seeed-Studio/SenseCAP_Indicator_RP2040) (Copyright Seeed Studio, Apache 2.0).

**Sensor-Bibliotheken:** Sensirion (SCD4x, SGP40, VOC-Gas-Index-Algorithm), Seeed AHT20, PacketSerial, Arduino SD/SdFat, LVGL v8.x.

**Web:** [supersauna.club](https://supersauna.club)

---

*"Das Wasser zischt. Die Kurve springt. Der Logger hält mit."*
