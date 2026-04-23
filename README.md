# sscsaunalogger — Sauna-Logger für den SenseCAP Indicator D1/D1S

Ein Zwei-Prozessor-Datenlogger für die finnische Sauna — misst Kabinentemperatur, Luftfeuchte, Aufguss-Spitzen und Vorraumklima im Sekundentakt, schreibt pro Session eine CSV auf SD-Karte und exportiert optional nach MariaDB. Basiert auf dem [SenseCAP Indicator D1/D1S](https://www.seeedstudio.com/SenseCAP-Indicator-D1S-p-5643.html) von Seeed Studio.

Gebaut und betrieben vom **[SuperSaunaClub](https://supersauna.club)**.

---

## Was das Ding macht

- **Sample-Rate:** Basistakt **1 Hz** (alle 1000 ms ein Mess-Tick). Beim `AUFGUSS`-Kommando schaltet der RP2040 für **120 Sekunden** auf **2 Hz** (alle 500 ms) und fällt dann automatisch zurück auf 1 Hz. Der SHT85 in der Kabine wird **jeden Tick** gelesen (1 bzw. 2 Hz); die Vorraum-Sensoren SCD41 und SGP40 nur **jeden zweiten Tick** (0.5 bzw. 1 Hz) — sie messen intern eh nur alle ~5 s neu. Das Basis-Intervall lässt sich via `COLLECT_INTERVAL`-Kommando (0xA0) zur Laufzeit zwischen 250 ms und 60000 ms umstellen.
- **SHT85 am 2 m Silikonkabel** misst in der Kabine (bis 105 °C spec), interner SCD41 misst CO₂/Temp/RH im Vorraum, SGP40 liefert VOC-Index (nach 5 min Warmup).
- **Session-basierte Aufzeichnung:** Start → (mehrere Aufgüsse mit Namen/Notiz) → End. Jede Session bekommt eine CSV auf SD und einen Metadaten-Eintrag in NVS. UI für Live-Anzeige, History-Liste, Detail-Chart, Bearbeiten, Löschen.
- **Saunameister-Verwaltung:** Operator-Dropdown mit den Stammgästen, Teilnehmer-Zähler, Notizen-Feld.
- **Optional MariaDB-Export:** automatischer Push in eine zentrale Datenbank (Default `postl.ai:3308`), damit mehrere Anlagen gemeinsam ausgewertet werden können. Per Default seit fw_mig=3 **aus** — explizit einschalten.
- **Extrem defensive Firmware:** I²C-Bus-Unlock bei Kabel-Glitch, I²C-Clock-Stretch-Timeout 25 ms mit Controller-Reset, SD-Recovery-Scan beim Boot, Sensor-Range-Checks nach CRC, Watchdog 8 s mit kurzem Feed-Abstand, Reset-Reason-Logging. Seit v0.2.4 crasht das Gerät im Saunabetrieb nicht mehr.

---

## Hardware

| Komponente | Details |
|---|---|
| Board | Seeed SenseCAP Indicator D1 oder D1S (ESP32-S3 + RP2040 + 4" 480×480 IPS Touch) |
| Externer Sauna-Fühler | Sensirion SHT85 am 2 m **Silikon-Kabel** (200 °C rated, PVC taugt nicht bei >80 °C) |
| Interne Sensoren | SCD41 (CO₂/T/RH @ 0x62), SGP40 (VOC @ 0x59) — auf PCB verbaut |
| Fallback-Fühler | AHT20 @ 0x38 (optional, wenn kein SHT85 verfügbar) |
| Speicher | microSD-Karte, **FAT32 formatiert** (exFAT funktioniert nicht, Arduino-SD-Lib kann es nicht) |
| Stromversorgung | 5 V über USB-C, ≥ 2 A empfohlen |

### Pin-Belegung am RP2040 (fix durchs D1-Board)

| Funktion | Pin |
|---|---|
| Wire SDA / SCL | GP20 / GP21 |
| SPI1 (SD) SCK / MOSI / MISO / CS | GP10 / GP11 / GP12 / GP13 |
| Serial1 (UART zum ESP32) TX / RX | GP16 / GP17 |
| Sensor-Power-Enable | GP18 |

### Hinweise zum Aufbau der Sauna-Probe

- **Silikon-Kabel** (4-adrig, geschirmt, 200 °C rated) für den Kabinen-Teil. PVC weicht über Monate bei Sauna-Temperaturen auf.
- **Drip-Loop** am Kabel-Durchgang durch die Kabinenwand: U-Bogen direkt außerhalb, Tiefpunkt unter dem Stecker. Kondensat tropft ab statt in die Pins zu wandern.
- I²C-Bus läuft firmware-seitig auf **50 kHz** — das Rise-Time-Budget bei 2 m Kabel + den On-Board-4.7 kΩ-Pullups reicht damit komfortabel. Wer noch robuster fahren will: zusätzliche 2.2 kΩ-Pullups am Sauna-Ende, dann kann die Clock zurück auf 100 kHz.
- Der SHT85-Sensor-Kopf sollte _nicht_ im Aufgussstrahl sitzen — Flüssigwasser zerstört den Polymer-Feuchtesensor. Montagehöhe typ. 10–20 cm unter der Decke, nicht über den Steinen.

---

## Architektur

Zwei-Prozessor-Aufteilung:

```
┌─────────────────────┐   UART1 @ 115200, COBS   ┌─────────────────────┐
│      ESP32-S3       │ ◄──────────────────────► │       RP2040        │
│                     │                           │                     │
│ · LVGL-UI 480×480   │                           │ · I²C-Sensoren      │
│ · Session-Logik     │                           │ · SHT85 @ 2m Kabel  │
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
  | `0xBF` | `SAUNA_TEMP` | 4 B Float (°C) | RP → ESP32 |
  | `0xC0` | `SAUNA_RH` | 4 B Float (%) | RP → ESP32 |
  | `0xC1` | `SD_READBACK_CHUNK` | 13 B Header + Daten | RP → ESP32 |

- **SD-Readback:** Der ESP32 liest historische CSVs seitenweise zurück, damit das Detail-Chart auf dem Display die Original-Samples zeigt ohne dass der ESP32 die Karte direkt anfasst. Max-Payload RP-seitig 200 B pro Chunk, ESP32-Parser-Robustheit limitiert es praktisch auf **64 B Payload** pro Chunk (größere Frames fragmentieren den COBS-Decoder bei 128-B-UART-FIFO).
- **Live-Sensor-Daten** werden via 0xBF/0xC0 bei jedem Tick gepusht (kein Polling vom ESP32 aus).

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
[SAUNA] BOOT ssc-v0.2.4 reset_reason=POWER/NORMAL
[SAUNA] I2C-Scan (boot): 0x38 0x44 0x59 0x62
[SAUNA] SD: initialisiert
[SAUNA] SHT3x ready @ 0x44 (SHT35/SHT85 kompatibel): 24.53 degC, 32.10 %RH
[SAUNA] SGP40 ready
[SAUNA] SCD41 ASC disabled (closed-room drift protection)
[SAUNA] RP2040 ssc-v0.2.4 ready: SD=1 SCD41=1 SGP40=1 SHT85=1 AHT20fb=0 probe=SHT3x
[SAUNA] watchdog armed (8 s)
...
[SAUNA] health: intvl=1000ms probe=SHT3x temp=25.5 rh=31.0 fails=0 session=0 sd=1 rp_die=41.2
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

- **Saunameister-Liste:** Namen der Stammgäste verwalten (für das Operator-Dropdown in der Summary)
- **WLAN:** SSID + Passwort, landet in NVS
- **HTTP-Endpunkt + Token** für den optionalen HTTP-POST-Export
- **MariaDB-Endpunkt** (Default `postl.ai:3308`) + Credentials
- **Info:** Versionen, Uptime, freier NVS-Platz

MariaDB und HTTP sind seit `fw_mig=3` **per Default aus** — explizit einschalten. Ohne Endpunkt-Config passiert kein Netzwerk-Traffic.

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
- `temp`: Kabinentemperatur (°C, SHT85)
- `rh`: relative Luftfeuchte (%, SHT85)
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
| `probe=NONE` nach Boot | SHT85 wird nicht erkannt, AHT20 auch nicht | I²C-Scan im Boot-Log anschauen, welche Adressen gefunden werden |
| `reset_reason=WATCHDOG` nach Crash | Code blockiert > 8 s | Zeilen vor dem Reset in Serial-Capture ansehen |
| `reset_reason=POWER/NORMAL` in Schleife | Brownout | Netzteil auf ≥ 2 A wechseln, USB-Hub entfernen, kurzes dickes Kabel |
| `sd=0` dauerhaft | Karte nicht erkannt oder exFAT | Karte neu in **FAT32** formatieren (Arduino-SD kann kein exFAT) |
| Touch reagiert nicht | FT6336U Hardware-Problem | Display-Flex neu einstecken, 2+ min stromlos |
| VOC-Index = NaN in den ersten 5 min | Normal | 5 min warten, Sensirion-Baseline-Learning |
| RH stuck bei 100 % nach Aufguss | Polymer-Kondensat | Firmware triggert automatisch Heater-Cycle, 10-min-Lockout |

---

## Lizenz & Dank

Apache License 2.0. Fork von [Seeed-Studio/SenseCAP_Indicator_ESP32](https://github.com/Seeed-Studio/SenseCAP_Indicator_ESP32) und [Seeed-Studio/SenseCAP_Indicator_RP2040](https://github.com/Seeed-Studio/SenseCAP_Indicator_RP2040).

Sensor-Bibliotheken: Sensirion (SCD4x, SGP40, Gas-Index-Algorithmus), Seeed AHT20, PacketSerial, Arduino SD/SdFat, LVGL v8.x.

Web: [supersauna.club](https://supersauna.club)

---

*"Das Wasser zischt. Die Kurve springt. Der Logger hält mit."*
