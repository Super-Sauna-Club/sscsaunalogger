# Bootloop-Recovery + NVS-Korruption Root-Cause + Hardening

> **Für agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (empfohlen) oder superpowers:executing-plans. Steps in checkbox-syntax (`- [ ]`).

**Goal:** ESP32 aus dem bootloop holen (NVS-partition korrupt), root-cause der korruption finden, gegen wiederkehr härten — ohne dass der user mehr daten verliert als unbedingt nötig.

**Architecture:** 5 phasen, nicht-zerstörerisch zuerst. forensische sicherung → minimal-invasive recovery (nur NVS, nicht firmware) → stabilitäts-test → root-cause-untersuchung der korruption → code-hardening → langzeit-verifikation. user-gates vor allen destructive ops.

**Tech Stack:** esptool.py (NVS-erase), idf.py monitor (serial), python (NVS-binary-analyse via `nvs_partition_gen.py` aus esp-idf), arduino-cli (RP2040), bash + grep für code-audits.

**Aktueller stand am 2026-05-04 17:00:**
- v0.2.11 geflasht (beide chips, branch clean, kein stash gepoppt)
- ESP32 boot_count=1185 (laut letztem panic-log), RP2040 boot_count=1188 — beide rebooten synchron
- Capture in `/tmp/esp32_bootloop.log` zeigt 9 verschiedene crash-stellen in 90s, davon 2 direkt in nvs-internals mit garbage-pointern (EXCVADDR=0xfaaaaac2 in `nvs::Page::alterEntryRangeState`, 0x00060f23 in `nvs::NVSPartitionManager::lookup_storage_from_name`)
- internal heap free nach init = 4559 byte (sehr tight, aber zweitrangig)
- gerät läuft aktuell wieder — also fenster für forensik ist offen

---

## Phase 0: Forensische Sicherung (non-destructive, jetzt sofort)

**Why:** bevor wir NVS erasen sind die korrupten daten weg. wenn wir den root-cause der korruption verstehen wollen brauchen wir das binary-image. außerdem sicherung der heap- und uptime-trends.

**Files:**
- Create: `/tmp/forensik/nvs_dump_2026-05-04.bin` (NVS-partition raw, 128 KB)
- Create: `/tmp/forensik/esp32_runtime_2026-05-04.log` (60-min serial-capture)
- Create: `/tmp/forensik/rp2040_runtime_2026-05-04.log` (60-min serial-capture)
- Create: `/tmp/forensik/heap_trace.txt` (heap-snapshots aus log extrahiert)

### Task 0.1: bestehende serial-capture stoppen falls noch was läuft

- [ ] **Step 1: laufende `cat /dev/ttyUSB0`-prozesse killen**

```bash
pgrep -a "cat /dev/ttyUSB0\|cat /dev/ttyACM0" && pkill -f "cat /dev/ttyUSB0" ; pkill -f "cat /dev/ttyACM0"
sleep 1
pgrep -a "cat /dev/tty" || echo "alle clean"
```
Expected: "alle clean"

### Task 0.2: 60-min serial-capture auf beiden ports starten (parallel, im hintergrund)

- [ ] **Step 1: forensik-verzeichnis anlegen**

```bash
mkdir -p /tmp/forensik
```

- [ ] **Step 2: ESP32 capture (60 min)**

```bash
stty -F /dev/ttyUSB0 raw 115200 -echo -echoe -echok
timeout 3600 cat /dev/ttyUSB0 > /tmp/forensik/esp32_runtime_2026-05-04.log 2>&1 &
echo "ESP32-capture PID=$!"
```

- [ ] **Step 3: RP2040 capture (60 min)**

```bash
stty -F /dev/ttyACM0 raw 115200 -echo -echoe -echok
timeout 3600 cat /dev/ttyACM0 > /tmp/forensik/rp2040_runtime_2026-05-04.log 2>&1 &
echo "RP2040-capture PID=$!"
```

Expected: zwei PIDs ausgegeben, beide laufen im hintergrund.

### Task 0.3: NVS-partition als raw-binary auslesen (KRITISCH — read_flash unterbricht running firmware kurz!)

**Why:** esptool muss esp32 in download-mode setzen (DTR/RTS-reset) → running firmware wird gestoppt → read_flash → nach erase reset zurück → firmware bootet wieder. wenn das gerät wieder im bootloop landet, haben wir aber dann das nvs-image und können trotzdem mit phase 1 weiter.

- [ ] **Step 1: user-gate**

text an user: "ich lese jetzt die 128 KB nvs-partition als raw-binary aus. esptool unterbricht dazu kurz die laufende firmware (~5 sek). danach versucht der chip selbst neu zu booten — wenn er wieder bootloopt, gehen wir zu phase 1 (erase). geht das ok?"

WAIT FOR USER YES BEFORE PROCEEDING.

- [ ] **Step 2: NVS-region auslesen**

```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 read_flash 0x9000 0x20000 /tmp/forensik/nvs_dump_2026-05-04.bin
```
Expected: "Read 131072 bytes at 0x00009000 in N seconds" + "Hard resetting via RTS pin..."

- [ ] **Step 3: dump-integrität prüfen**

```bash
ls -la /tmp/forensik/nvs_dump_2026-05-04.bin
file /tmp/forensik/nvs_dump_2026-05-04.bin
xxd /tmp/forensik/nvs_dump_2026-05-04.bin | head -5
```
Expected: 131072 bytes, "data", erste bytes sollten NVS-page-headers sein (0xFE/0xFF/sequence numbers).

- [ ] **Step 4: serial-capture-status prüfen — booted das gerät wieder?**

```bash
sleep 8
tail -50 /tmp/forensik/esp32_runtime_2026-05-04.log | grep -E "boot_count|Guru|reset="
```
Expected entweder:
- (a) neuer normaler boot (`reset=USB_UART_CHIP_PWR(?)` oder `reset=UNKNOWN(0)`) ohne folgendem panic → **gerät läuft wieder, weiter zu phase 0.4**
- (b) wieder panics → **gerät wieder in bootloop, sofort weiter zu phase 1 erase**

### Task 0.4: heap-trend extrahieren aus laufender capture

- [ ] **Step 1: nach 5 minuten capture: heap-werte rauspulen**

```bash
sleep 300
grep -nE "pre-worker heap|heap_caps|free|SSTORE" /tmp/forensik/esp32_runtime_2026-05-04.log | tail -30 > /tmp/forensik/heap_trace.txt
cat /tmp/forensik/heap_trace.txt
```
Expected: zumindest 1 `pre-worker heap`-zeile pro boot-zyklus. wenn zahlen monoton fallen über boots → leak. wenn konstant ~4559 → fragmentation.

### Task 0.5: capture-snapshot vor recovery committen

**Why:** post-mortem-evidence soll im git landen (referenz für künftige sessions). nicht den bin-dump committen (binary, ggf. sensitive nvs-inhalte) — nur das auswertete log.

- [ ] **Step 1: auswertung schreiben**

```bash
mkdir -p docs/forensik
cp /tmp/esp32_bootloop.log docs/forensik/2026-05-04-bootloop-initial-90s.log
grep -nE "boot_count|Guru|reset=|EXCVADDR|EXCCAUSE|Backtrace" /tmp/forensik/esp32_runtime_2026-05-04.log > docs/forensik/2026-05-04-runtime-crashes.txt 2>&1 || echo "keine weiteren panics in runtime-log"
```

- [ ] **Step 2: NICHT committen jetzt** — erst nach phase 5 alles zusammen committen.

---

## Phase 1: NVS-Erase + boot-recovery (DESTRUCTIVE — user-gate erforderlich)

**Why:** wenn die nvs-partition garbage-pointer in den page-metadaten hat, ist der einzige saubere weg die partition zu wipen. firmware bleibt geflasht, nur nvs-inhalt weg.

**Was verloren geht (laut analyse vom log):**
- wifi config (Tommy SSID — user muss neu eingeben)
- 3 session-metadaten-einträge in der historie-liste (CSV-files unter `/sessions/<id>.csv` auf SD bleiben, aber UI zeigt sie nicht mehr ohne re-import)
- operator-presets (16 default-namen kommen zurück, custom verloren wenn welche)
- last-active-screen-state (trivial)
- time-persist (NTP holt sich's wieder)
- boot-counter beider chips (counter startet bei 1)
- mariadb/http-config (war eh per default deaktiviert)

**Was bleibt:**
- firmware v0.2.11 (kein reflash)
- alle CSV-files auf SD-karte
- alle hardware-configs

### Task 1.1: user-go für NVS-erase

- [ ] **Step 1: ankündigen + bestätigung holen**

text an user: "ich starte jetzt phase 1: NVS-erase. das gerät verliert wifi-config, 16 operator-defaults kommen zurück, 3 session-metadaten-einträge sind in der UI-historie weg (CSVs auf SD bleiben). firmware bleibt drauf, kein reflash. ok?"

WAIT FOR USER YES.

### Task 1.2: NVS-partition erasen

- [ ] **Step 1: erase ausführen**

```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 erase_region 0x9000 0x20000
```
Expected: "Erasing region (may be slow depending on size)... Erase completed successfully in N seconds. Hard resetting via RTS pin..."

- [ ] **Step 2: chip wird automatisch resettet → erste 20s capture beobachten**

```bash
sleep 20
tail -80 /tmp/forensik/esp32_runtime_2026-05-04.log
```
Expected:
- boot-banner mit `Version: v1.0.0 May 3 2026 19:22:23`
- `BOOT: SSC v0.2.11 | reset=POWERON_RESET(?) boot_count=1` (frischer counter!)
- `SSTORE: NVS: used=0` oder `used=N` mit kleinem N (frische default-namespaces)
- KEINE `Guru Meditation` zeile in den letzten 80 zeilen

### Task 1.3: 5-min stabilitätsfenster

- [ ] **Step 1: 5 minuten warten + capture-tail prüfen**

```bash
sleep 300
grep -c "Guru\|reset=PANIC\|reset=INT_WDT" /tmp/forensik/esp32_runtime_2026-05-04.log
```
Expected: 0 neue panics seit dem erase (oder genau die alten aus phase 0).

- [ ] **Step 2: post-erase boot-stats**

```bash
grep -E "BOOT: SSC v0.2.11" /tmp/forensik/esp32_runtime_2026-05-04.log | tail -5
```
Expected: nur 1 boot, nicht mehrere.

### Task 1.4: WiFi neu konfigurieren

- [ ] **Step 1: user-instruction**

text an user: "wifi am gerät neu eingeben: settings → WLAN → Tommy + passwort → speichern. dann melden wenn fertig."

WAIT FOR USER CONFIRMATION.

- [ ] **Step 2: nach user-bestätigung im log nach `wifi:connected with Tommy` suchen**

```bash
grep "wifi:connected with" /tmp/forensik/esp32_runtime_2026-05-04.log | tail -3
grep "wifi-model: wifi event: WIFI_EVENT_STA_GOT_IP\|got ip:" /tmp/forensik/esp32_runtime_2026-05-04.log | tail -3
```
Expected: `connected with Tommy`-zeile + `STA_GOT_IP`/`got ip` mit IPv4-adresse.

---

## Phase 2: 30-min Stabilitäts-Verifikation post-recovery

**Why:** wenn das problem rein NVS-korruption war, sollte das gerät jetzt stabil laufen. wenn nicht, tut sich ein neuer code-bug auf den wir vorher nicht sehen konnten weil der bootloop den verdeckt hat.

### Task 2.1: stabilitätsfenster erweitern

- [ ] **Step 1: nochmal 25 min warten (gesamt 30 min seit erase)**

```bash
sleep 1500
```

- [ ] **Step 2: panic-count über die 30 minuten**

```bash
PANIC_COUNT=$(grep -cE "Guru Meditation|reset=PANIC|reset=INT_WDT|reset=TG" /tmp/forensik/esp32_runtime_2026-05-04.log)
BOOTS=$(grep -cE "BOOT: SSC v0.2.11" /tmp/forensik/esp32_runtime_2026-05-04.log)
echo "panics_total=$PANIC_COUNT boots_total=$BOOTS"
```
Expected post-erase:
- panics_total = 8 (die alten aus phase 0, keine neuen)
- boots_total = 9-10 (8 alte bootloop-boots + 1-2 post-erase boots)

WENN panics_total > 8 (= neue panics nach erase): **STOP. neuer fehler. gehe zu phase 3 mit höherer priorität.**

### Task 2.2: heap-stabilität

- [ ] **Step 1: heap-werte über zeit**

```bash
grep "pre-worker heap" /tmp/forensik/esp32_runtime_2026-05-04.log
```
Expected: alle heap-werte ähnlich (4500-5500 byte internal). wenn werte stark schwanken oder fallen → leak unter beobachtung.

### Task 2.3: live-feature-test (user)

- [ ] **Step 1: user-instruction**

text an user: "kannst du am gerät einmal: home → live → start drücken → 30 sek warten → aufguss drücken → stop → summary speichern → home? das triggert NVS-writes (session-store) und sollte ohne crash durchgehen."

WAIT FOR USER REPORT (gut/crash/hängt).

- [ ] **Step 2: bei "gut" — phase 1+2 erfolgreich, weiter zu phase 3 (root-cause)**

- [ ] **Step 3: bei "crash" — capture-tail anschauen, neuen bug einkreisen**

```bash
tail -100 /tmp/forensik/esp32_runtime_2026-05-04.log | grep -E "Guru|Backtrace|EXCVADDR"
```

---

## Phase 3: Root-Cause-Analyse — warum war NVS korrupt?

**Why:** "NVS erase" ist pflaster, nicht heilung. wenn die korruptions-ursache im code liegt kommt sie wieder. wenn sie ein einmaliges power-event war können wir's beruhigt akzeptieren (mit besserer recovery für nächstes mal).

**Hypothesen-tabelle (priorisiert):**

| H | Hypothese | Wahrscheinlichkeit | Wie verifizieren |
|---|-----------|-------------------|------------------|
| H1 | power-glitch während NVS-write (user hat stecker gezogen, brownout, etc.) | hoch | user fragen, brownout-detector-events im log? |
| H2 | concurrent NVS-writes von mehreren tasks ohne mutex | mittel | code-audit: alle nvs_set_*-call-sites + ihre tasks |
| H3 | oversized blob-write (write länger als page → seitenwechsel mid-write → atomic-flag-bug) | mittel | nvs_dump-binary-analyse |
| H4 | wear-leveling-exhaustion (1185+ writes → flash-zellen tot) | niedrig | esptool flash-info, JEDEC-status, sektor-vergleich |
| H5 | bug in v0.2.7 last-screen-persist oder time-persist (zu kleine struct? wrong size? race?) | mittel | code-review von clock_tick + indicator_time.c persist |
| H6 | heap-corruption schreibt garbage in nvs-write-puffer | mittel | heap-tracing einbauen, bounds-checks |

### Task 3.1: H1 — user befragen

- [ ] **Step 1: user-frage**

text an user: "drei fragen für root-cause:
1. wann ungefähr hat der bootloop angefangen — vorgestern abend, heute morgen, gerade eben?
2. was war die letzte normale aktion vor dem bootloop — eine session aufgenommen, gerät umgestöpselt, neu geflasht, einfach laufen lassen?
3. ist mal kurz strom weg gewesen (USB-kabel raus, hub aus, PD-source aus, stromausfall)?"

WAIT FOR USER ANTWORT — die info entscheidet wie tief wir in H2-H6 reingehen müssen.

- [ ] **Step 2: user-info dokumentieren**

zusammenfassung in `docs/forensik/2026-05-04-rootcause-context.md` schreiben (nur lokale notiz, später committen):

```markdown
# Root-Cause Context für 2026-05-04 Bootloop

**User-input:**
- Onset: <user-antwort>
- Letzte aktion: <user-antwort>
- Power-event: <user-antwort>

**Implikationen:**
- wenn power-event ja → H1 wahrscheinlich, H2-H6 niedrig priorisieren
- wenn nein und letzte aktion = normal-laufen → H2/H5 priorisieren
- wenn letzte aktion = flash → kein root-cause-search-bedarf, das war's
```

### Task 3.2: H1 — brownout-detector-events im historischen log suchen

- [ ] **Step 1: BROWN_OUT-resets über alle captures suchen**

```bash
grep -E "BROWN_OUT|brownout|rst:0x.. \(BROWNOUT" /tmp/esp32_bootloop.log /tmp/forensik/esp32_runtime_2026-05-04.log 2>&1 | head
```
Expected: brownout-resets sehen so aus: `rst:0xf (BROWNOUT_RST)`. wenn keine brownout-resets im log → power war stabil → H1 unwahrscheinlich → tiefer H2-H6 graben.

### Task 3.3: H3 — nvs-dump-binary analysieren

**Why:** der `0xfaaaaac2` und `0x00060f23` aus den crash-EXCVADDRs sind irgendwo in der nvs-partition gelandet. wenn wir sie im binary finden, sehen wir welche page korrupt war + was vorher reingeschrieben werden sollte.

- [ ] **Step 1: nach den exakten garbage-bytes im dump suchen (little-endian)**

```bash
# 0xfaaaaac2 little-endian = c2 aa aa fa
xxd -p /tmp/forensik/nvs_dump_2026-05-04.bin | tr -d '\n' | grep -obP "c2aaaafa" | head -10
# 0x00060f23 little-endian = 23 0f 06 00
xxd -p /tmp/forensik/nvs_dump_2026-05-04.bin | tr -d '\n' | grep -obP "230f0600" | head -10
```
Expected:
- (a) match → byte-offset notieren, mit `xxd -s OFFSET-32 -l 64` umfeld lesen
- (b) kein match → die garbage-werte waren keine direkten daten sondern berechnete pointer → tieferes problem (eher heap-corruption als nvs-page-corruption)

- [ ] **Step 2: nvs-page-headers durchgehen — gibt's pages mit ungültigen state-bytes?**

```bash
# jede nvs-page ist 0x1000 (4096) bytes, header in den ersten 32 bytes
# bytes 0-3 = state (0xFFFFFFFF=empty, 0xFFFFFFFE=active, 0xFFFFFFFC=full, 0xFFFFFFF8=freeing, 0xFFFFFFF0=corrupt, 0x00000000=invalid)
# bytes 4-7 = sequence number
for i in $(seq 0 31); do
  off=$((i * 0x1000))
  state=$(xxd -s $off -l 4 -p /tmp/forensik/nvs_dump_2026-05-04.bin)
  seq=$(xxd -s $((off+4)) -l 4 -p /tmp/forensik/nvs_dump_2026-05-04.bin)
  echo "page $i (off=$(printf '0x%05x' $off)): state=$state seq=$seq"
done
```
Expected: alle pages haben state in {ffffffff, fffffffe, fffffffc, fffffff8}. wenn eine page state=0x00000000 oder andere garbage → **das war die korrupte page**.

### Task 3.4: H2/H5 — code-audit aller NVS-write-sites + thread-context

- [ ] **Step 1: alle NVS-writes auflisten mit task-context**

```bash
grep -nE "nvs_set_|nvs_commit|nvs_open" SenseCAP_Indicator_ESP32/main/**/*.c \
  | grep -v "//\s*nvs" \
  > /tmp/forensik/nvs_writes_audit.txt
wc -l /tmp/forensik/nvs_writes_audit.txt
cat /tmp/forensik/nvs_writes_audit.txt
```
Expected: ~50 zeilen. für jede zeile prüfen welcher task die aufruft.

- [ ] **Step 2: die kritischen schreib-pfade einzeln review'en**

priorität (häufige writer zuerst):
- `ui_sauna.c::clock_tick` last-screen-persist (lvgl-task, ~alle 10s wenn screen-wechsel)
- `indicator_time.c` time-persist (eigener task, alle 60s)
- `main.c::increment_boot_counter` (im app_main vor task-start, no race)
- `indicator_session_store.c` session-append (ui-callback, on save)
- `indicator_storage.c` (wifi-config saves)
- `__sensor_history_data_save` legacy (eigener task `sensor_history_data_updata_task`, periodic)
- `indicator_http_export.c` retry-queue (eigener worker)

für jeden:
  - läuft er unter mutex?
  - überlappt er sich potentiell mit anderen NVS-writern?
  - schreibt er in den gleichen namespace wie andere writer? (cross-namespace ist threadsafe in NVS, intra-namespace race ist problematisch)

- [ ] **Step 3: mutex-coverage matrix erstellen**

ergebnis in `docs/forensik/2026-05-04-nvs-writers.md`:

```markdown
| caller | task | namespace | freq | mutex? | risk |
|--------|------|-----------|------|--------|------|
| clock_tick last_scr | lvgl_task | indicator | ~10s wenn screen wechsel | nein* | mid |
| time-persist | time_task | time | 60s | nein | low |
| boot_counter | main_task | boot_info | 1× boot | n/a | n/a |
| session-append | event_task | sauna_sess | on user-save | sess-mutex | low |
| http-export retry | http_task | httpx | on tx-fail | http-mutex | low |
| sensor_history_save (LEGACY) | sensor_history_updata | sensor_data | periodic | nein | mid |
| wifi-config-save | wifi-event-task | wifi_config | on user-save | nein | low |
```

* lvgl_task und event_task laufen beide unter dem `lv_port_sem`, aber das ist UI-mutex, nicht NVS-mutex.

### Task 3.5: H6 — heap-pressure messen + leak-check

- [ ] **Step 1: temp-instrumentation einbauen — nicht committen, nur lokal**

`SenseCAP_Indicator_ESP32/main/main.c` ergänzen (vor task-create, am ende von app_main):

```c
/* DEBUG NUR LOKAL: heap-monitor task — alle 30s heap-snapshot in log */
static void __debug_heap_task(void *arg) {
    while (1) {
        ESP_LOGW("HEAPMON", "free internal=%u largest=%u psram=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
```
+ task-create im app_main.

- [ ] **Step 2: lokal flashen + 60 min uptime + heap-trend zu csv**

```bash
cd SenseCAP_Indicator_ESP32 && idf.py build && idf.py -p /dev/ttyUSB0 flash
# 60 min capture
timeout 3600 cat /dev/ttyUSB0 > /tmp/forensik/heap_60min.log &
sleep 3700
grep "HEAPMON" /tmp/forensik/heap_60min.log | awk -F'free internal=|largest=|psram=' '{print $2","$3","$4}'
```
Expected:
- (a) werte stabil (+/- 1 KB) → kein leak, einfach niedriges baseline-niveau
- (b) werte fallen monoton → leak. richtung der allokationsquelle weiterforschen.

- [ ] **Step 3: instrumentation rausnehmen vor commits zum produktionscode**

### Task 3.6: zusammenfassung der root-cause-erkenntnisse

- [ ] **Step 1: post-mortem-dokument**

`docs/forensik/2026-05-04-postmortem.md` schreiben mit:
- onset-zeit (user-input)
- crash-signature-katalog (aus phase 0)
- nvs-binary-analyse (aus phase 3.3)
- writer-matrix (aus phase 3.4)
- heap-trend (aus phase 3.5)
- **conclusion:** root-cause-hypothese mit confidence-level
- **konsequenzen für phase 4** — was muss gehärtet werden?

---

## Phase 4: Code-Hardening (basierend auf phase-3-erkenntnissen)

**Why:** sicherstellen dass auch wenn nvs nochmal korrupt wird, das gerät nicht in einen self-perpetuating bootloop fällt. plus: die menge der NVS-writes reduzieren um wear + race-fenster zu verkleinern.

**WICHTIG:** welche tasks unten konkret ausgeführt werden hängt von phase-3-erkenntnissen ab. wenn z.b. H1 (power-glitch) bestätigt ist und H2-H6 verworfen, fallen 4.2-4.4 weg. wenn H5 (bug in v0.2.7) bestätigt, ist 4.5 die hauptaufgabe. user-gate vor jeder task entscheidet.

### Task 4.1: NVS-corruption-recovery beim boot — **definitiv** machen

**Why:** auch wenn die korruption von außen kommt (power-glitch), soll die nächste boot-runde nicht 1185× crashen sondern selbst recovern. einmal nvs-init-failure → komplett-erase + retry. user verliert config aber gerät bootet.

**Files:**
- Modify: `SenseCAP_Indicator_ESP32/main/main.c` (app_main, vor allen anderen module-inits)

- [ ] **Step 1: failing-test-szenario beschreiben (kein automatisierter test möglich, manuell-test über simulierte korruption)**

manueller test-plan: nach erase nvs-partition mit `dd if=/dev/urandom of=... bs=4096 count=2` korrupten ersten 2 pages, flash, beobachten ob recovery greift.

- [ ] **Step 2: code-änderung in main.c**

der bestehende nvs-init-call ersetzen durch:

```c
/* v0.2.12: NVS-recovery — wenn init failed (corrupted partition), einmal
 * komplett erasen und neu initialisieren. das verhindert dass eine
 * korrupte nvs-page einen self-perpetuating bootloop erzeugt der das
 * gerät unbrauchbar macht.                                              */
static void __nvs_init_with_recovery(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND ||
        err == ESP_ERR_NVS_CORRUPT_KEY_PART ||
        err == ESP_ERR_INVALID_STATE) {
        ESP_LOGE("BOOT", "NVS init failed: %s — erasing partition", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI("BOOT", "NVS init ok");
}
```

und im app_main vor allen anderen calls:

```c
__nvs_init_with_recovery();
```

ACHTUNG: nicht alle korruptions-modi werden von `nvs_flash_init` selbst erkannt — manche treten erst beim ersten read/write auf. dieser recovery-pfad fängt nur das was das init-API explizit als fehler meldet. tiefere recovery (catch panic im handler) ist nicht trivial im ESP-IDF.

- [ ] **Step 3: build + flash + manueller test**

```bash
cd SenseCAP_Indicator_ESP32 && idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
```

danach in einem 2. terminal NVS-partition komprimieren um init-failure zu provozieren:

```bash
# 2 erste pages mit garbage füllen
dd if=/dev/urandom of=/tmp/garbage.bin bs=4096 count=2
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 write_flash 0x9000 /tmp/garbage.bin
```

dann reset → expected im monitor: `NVS init failed: ESP_ERR_NVS_NO_FREE_PAGES — erasing partition` → `NVS init ok` → normaler boot. KEIN bootloop.

- [ ] **Step 4: commit**

```bash
git add SenseCAP_Indicator_ESP32/main/main.c
git commit -m "feat(v0.2.12): NVS-recovery beim boot - korrupte partition autonom erasen statt bootloop"
```

### Task 4.2: clock_tick last-screen-persist reduzieren — **wenn H5 bestätigt**

**Why:** alle 10s nvs schreiben (wenn screen wechselt) ist überzogen. das ist quasi sinnloser flash-wear für eine debug-info. entweder ganz raus oder nur on-demand.

**Files:**
- Modify: `SenseCAP_Indicator_ESP32/main/view/ui_sauna.c:3310-3322`

- [ ] **Step 1: option A (sanft) — frequenz auf 5 minuten cappen**

```c
/* v0.2.12: persist-frequenz von 10s auf 300s cappen — alle 10s NVS schreiben
 * ist flash-wear-relevant und hat kein realen recovery-nutzen.            */
static const char *s_last_persisted = NULL;
static int64_t s_last_persist_us = 0;
const int64_t now_us = esp_timer_get_time();
if (sname != s_last_persisted && (now_us - s_last_persist_us) > 300000000LL) {
    s_last_persisted = sname;
    s_last_persist_us = now_us;
    nvs_handle_t h;
    if (nvs_open("indicator", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "last_scr", sname);
        nvs_commit(h);
        nvs_close(h);
    }
}
```

option B (radikal) — block ganz löschen. user-entscheidung.

- [ ] **Step 2: build + flash + test**

screen wechseln, log prüfen ob nvs nur noch alle 5 min geschrieben wird.

- [ ] **Step 3: commit**

### Task 4.3: legacy `sensor_history_data_updata_task` evaluieren — **wenn H2 bestätigt**

**Why:** dieser task läuft seit ur-projekt und schreibt alle paar minuten in den `sensor_data`-namespace. UI verwendet das nicht mehr (legacy-luftqualitäts-screens entfernt). wenn niemand das mehr liest, ist es nur noise + flash-wear + race-quelle.

**Files:**
- Modify: `SenseCAP_Indicator_ESP32/main/model/indicator_sensor.c:1602` (xTaskCreate aufruf)

- [ ] **Step 1: prüfen ob jemand den `sensor_data`-namespace liest**

```bash
grep -rn "sensor_data\|sensor_history_data_get\|SENSOR_HISTORY" SenseCAP_Indicator_ESP32/main --include="*.c" --include="*.h" | grep -v "sensor_history_data_save\|sensor_history_data_update\|sensor_history_data_week_update\|sensor_history_data_updata_task"
```

- [ ] **Step 2: wenn keine reader → task disablen**

```c
// LEGACY: sensor_history-task war für luftqualitäts-screens. mit sauna-fork
// werden die nicht mehr gerendert. task disabled v0.2.12 wegen NVS-pressure.
// xTaskCreate(sensor_history_data_updata_task, "sensor_history_data_updata_task", 1024*8, NULL, 6, NULL);
```

- [ ] **Step 3: build + flash + 30 min uptime**

heap-trace erwarten: deutlich entspannter da 8 KB stack + periodische allocs weg.

- [ ] **Step 4: commit**

### Task 4.4: NVS-write-mutex einführen — **wenn H2 race bestätigt**

**Why:** ESP-IDF NVS hat intern eigene mutexes für die selbe namespace, aber nicht über tasks die völlig unterschiedliche code-pfade haben. wenn unsere root-cause-analyse zeigt dass clock_tick und time-persist sich gegenseitig in die quere kommen, einen globalen sscsauna-NVS-mutex einführen.

(skip wenn 4.2 + 4.3 die schreibfrequenz schon stark reduzieren — dann ist race-fenster zu klein um relevant zu sein.)

- [ ] **Step 1: implementation skizze**

`SenseCAP_Indicator_ESP32/main/util/ssc_nvs.c|h` neu anlegen — wrapper `ssc_nvs_set_str/blob/i32(...)` der intern einen globalen mutex hält.

- [ ] **Step 2: alle nvs_set_*-aufrufe in ssc_nvs_*-aufrufe umbauen**

(wenn 4.4 nicht gemacht wird, skip)

### Task 4.5: heap-budget aufräumen — **immer**, niedrige priorität

**Why:** 4559 byte total internal frei ist tight. nicht direkt crash-ursache aber genug puffer für allocs in stress-momenten (wifi-storm, NVS-wear, tcp-stack) sollte da sein.

- [ ] **Step 1: heap-trace aus 3.5 anschauen — wo geht der speicher hin?**

```bash
# bei laufender firmware ESP-IDF heap-trace activieren (wenn aktiviert in sdkconfig):
# heap_trace_start(HEAP_TRACE_ALL);
# ... nach 60s ...
# heap_trace_dump();
```

- [ ] **Step 2: kandidaten für PSRAM-migration:**
- LVGL display-buffer (sollte schon in PSRAM sein, prüfen)
- session-ringbuffer (ist schon in PSRAM)
- weitere große buffer wenn welche da sind

- [ ] **Step 3: mariadb-task disablen wenn user nicht aktiv nutzt** (laut log: `Failed to create DB task` — heißt der wird nichtmal erstellt; kann komplett raus aus boot-init wenn config leer ist)

(zeitbudget: nur wenn 4.1-4.4 schnell durchgehen — sonst auf eigene session schieben)

---

## Phase 5: Langzeit-Verifikation

**Why:** 30 min in phase 2 ist nur initial-test. echte stabilitäts-bestätigung braucht stunden + stress.

### Task 5.1: 6h-uptime-baseline

- [ ] **Step 1: nach allen phase-4-commits clean reflashen**

```bash
cd SenseCAP_Indicator_ESP32 && idf.py build && idf.py -p /dev/ttyUSB0 flash
cd ../SenseCAP_Indicator_RP2040 && arduino-cli compile -b rp2040:rp2040:seeed_xiao_rp2040 . && arduino-cli upload -p /dev/ttyACM0 -b rp2040:rp2040:seeed_xiao_rp2040 .
```

- [ ] **Step 2: 6h-capture starten**

```bash
mkdir -p /tmp/forensik/postfix
timeout 21600 cat /dev/ttyUSB0 > /tmp/forensik/postfix/esp32_6h.log 2>&1 &
timeout 21600 cat /dev/ttyACM0 > /tmp/forensik/postfix/rp2040_6h.log 2>&1 &
```

- [ ] **Step 3: nach 6h ergebnis**

```bash
sleep 21700
PANICS=$(grep -cE "Guru|reset=PANIC|reset=INT_WDT" /tmp/forensik/postfix/esp32_6h.log)
BOOTS=$(grep -cE "BOOT: SSC v" /tmp/forensik/postfix/esp32_6h.log)
echo "panics=$PANICS boots=$BOOTS"
```
Expected: panics=0 boots=1 (kein crash, kein reboot in 6h).

### Task 5.2: WiFi-flap-stress

**Why:** v0.2.9-fix zielt auf wifi-flap-pfad. das hat dieselbe NVS-pressure-quelle. test ob hardening das stabilisiert.

- [ ] **Step 1: router 5× kurz aus/an im 30s-takt**

ggf. user-instruction. erwartung: AP-fallback triggert nach ~150s, kein crash, kein bootloop.

### Task 5.3: simulierte unplug-during-write (H1-test)

**Why:** wenn root-cause vermutlich power-glitch war, simulieren ob 4.1 (NVS-recovery beim boot) das auffängt.

- [ ] **Step 1: live-session starten**

am gerät: home → live → start.

- [ ] **Step 2: während worker NVS schreibt: USB-stecker raus für 2s, wieder rein**

(timing: wenn user gerade summary speichert, ist NVS-write garantiert aktiv. alternative: time-persist-fenster — alle 60s.)

- [ ] **Step 3: erwartung**
- (a) gerät bootet normal weiter, ggf. mit `NVS init failed → erasing partition` log → recovery hat gegriffen
- (b) bootloop wieder → recovery-pfad fängt diesen modus nicht. weitere härtung nötig.

### Task 5.4: alle forensik-files committen + memory updaten

- [ ] **Step 1: commit forensik-docs**

```bash
git add docs/forensik/2026-05-04-bootloop-initial-90s.log
git add docs/forensik/2026-05-04-runtime-crashes.txt
git add docs/forensik/2026-05-04-rootcause-context.md
git add docs/forensik/2026-05-04-postmortem.md
git add docs/forensik/2026-05-04-nvs-writers.md
git add docs/superpowers/plans/2026-05-04-bootloop-recovery-hardening.md
git commit -m "docs(forensik): 2026-05-04 NVS-bootloop postmortem + recovery-plan"
```

KEIN nvs-binary-dump committen (kann sensitive daten haben — wifi-passwort etc.).

- [ ] **Step 2: memory updaten**

neue memory-datei `project_v0212_nvs_recovery.md` mit:
- crash-katalog
- root-cause-conclusion
- hardening-fixes (4.1-4.5)
- NVS-recovery-pattern für künftige sessions

dann `MEMORY.md` index aktualisieren.

- [ ] **Step 3: version-bump auf v0.2.12**

```bash
sed -i 's/SSC_APP_VERSION "0.2.11"/SSC_APP_VERSION "0.2.12"/' SenseCAP_Indicator_ESP32/main/app_version.h
sed -i 's/VERSION "ssc-v0.2.11"/VERSION "ssc-v0.2.12"/' SenseCAP_Indicator_RP2040/SenseCAP_Indicator_RP2040.ino
git add SenseCAP_Indicator_ESP32/main/app_version.h SenseCAP_Indicator_RP2040/SenseCAP_Indicator_RP2040.ino
git commit -m "chore(v0.2.12): version bump nach NVS-recovery-hardening"
```

---

## Self-Review-Checkliste

**Spec-coverage:**
- [x] forensische sicherung vor destruktiven ops — phase 0 task 0.2-0.5
- [x] minimal-invasive recovery (nur nvs, nicht firmware) — phase 1
- [x] stabilitäts-verifikation post-recovery — phase 2
- [x] root-cause-analyse — phase 3 mit hypothesen-tabelle
- [x] hardening — phase 4 (4.1 immer, 4.2-4.4 conditional auf phase-3-erkenntnissen)
- [x] langzeit-verifikation + stress — phase 5

**User-gates (wo destructive/wartezeit):**
- [x] vor read_flash (task 0.3 step 1)
- [x] vor erase_region (task 1.1)
- [x] nach erase: WiFi reconfig (task 1.4)
- [x] live-feature-test (task 2.3)
- [x] root-cause-fragen (task 3.1)
- [x] stress-tests phase 5 brauchen user-physical-action

**Risiken & mitigations:**
- read_flash kann firmware kicken → ist in phase-0 ok weil danach eh erase kommt
- nvs-erase verliert wifi-config → user reconfig in task 1.4
- 4.4 (mutex einführen) hat hohes refactor-risiko → conditional, nicht default
- 4.5 (heap-cleanup) ist long-tail → optional, kann auf eigene session

**Pflaster-versuchung:**
- nicht "einfach NVS-erase und feddich" — phase 3 + 4.1 sind pflicht für defensiv-programmierung-feedback. user hat das explizit gewünscht.

---

**Plan abgeschlossen. zwei execution-optionen:**

**1. Subagent-Driven (empfohlen)** — ich dispatche pro task einen frischen subagent, review zwischen tasks, schnelle iteration. ideal für phasen 0-2 wo viel sequentielles capture-then-analyze.

**2. Inline-Execution** — wir arbeiten phase-für-phase in dieser session durch, mit checkpoints zum review.

**welcher ansatz?**
