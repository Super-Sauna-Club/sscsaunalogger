/*
 * indicator_session_store.c - NVS-Index fuer Sauna-Sessions.
 *
 * Sehr bewusst simpel gehalten: ein uint16-count + pro Session ein
 * binary-blob-key "s_XXXX". Bei delete wird das Array zusammen-
 * geschoben, damit die Liste kontinuierlich ist. Das ist O(n) bei
 * jedem Delete aber bei <500 Eintraegen kein Problem.
 */

#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "indicator_session_store.h"

static const char *TAG = "SSTORE";
#define NS   "sauna_sess"
#define K_CNT "c"

static SemaphoreHandle_t s_lock = NULL;
#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void key_for_slot(uint16_t slot, char *buf, size_t n) {
    snprintf(buf, n, "s_%04X", slot);
}

static esp_err_t open_rw(nvs_handle_t *h) {
    return nvs_open(NS, NVS_READWRITE, h);
}

static esp_err_t read_count(nvs_handle_t h, uint16_t *out) {
    uint16_t v = 0;
    esp_err_t err = nvs_get_u16(h, K_CNT, &v);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        v = 0;
        err = ESP_OK;
    }
    *out = v;
    return err;
}

static esp_err_t write_count(nvs_handle_t h, uint16_t v) {
    esp_err_t err = nvs_set_u16(h, K_CNT, v);
    if (err != ESP_OK) return err;
    return nvs_commit(h);
}

static void wipe_legacy_namespaces(void) {
    /* Alte Luftqualitaets-Firmware hatte massive Sensor-History-
     * Blobs in NVS - die fressen die Partition voll, sodass unser
     * Session-Store nix mehr schreiben kann. Einmalig alle eintraege
     * in diesen namespaces erase.                                    */
    const char *legacy[] = {
        "sensor_hist", "sensor_his", "indicator_st",
        "sensor_data", "history",
        /* MariaDB-Export per default DEAKTIVIERT - user soll in
         * Settings explizit aktivieren. Auch den HTTP-Export. */
        "mariadb", "httpx"
    };
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        nvs_handle_t hh;
        if (nvs_open(legacy[i], NVS_READWRITE, &hh) == ESP_OK) {
            nvs_erase_all(hh);
            nvs_commit(hh);
            nvs_close(hh);
            ESP_LOGI(TAG, "legacy namespace '%s' geleert", legacy[i]);
        }
    }
}

int indicator_session_store_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return -1;
    }
    nvs_handle_t h;
    esp_err_t oe = open_rw(&h);
    if (oe != ESP_OK) {
        /* NICHT gleich `nvs_flash_erase()` - das wuerde auch die
         * Sauna-Sessions loeschen. Erst versuchen, ob das nur an
         * einem fehlenden namespace liegt (der beim ersten open
         * automatisch angelegt wird).                              */
        ESP_LOGW(TAG, "nvs_open failed: %d - versuche namespace-create", oe);
        /* Simple retry */
        vTaskDelay(pdMS_TO_TICKS(10));
        oe = open_rw(&h);
        if (oe != ESP_OK) {
            ESP_LOGE(TAG, "NVS persist broken (%d) - weitere sessions "
                     "gehen verloren, aber alte bleiben auf flash erhalten", oe);
            return -2;
        }
    }
    uint16_t cnt;
    read_count(h, &cnt);
    nvs_close(h);

    /* One-time migration: beim ersten boot mit neuer firmware die
     * legacy-config und mariadb/http-cfg wipen. Version-flag im
     * sauna_sess-namespace verhindert wiederholung.              */
    nvs_handle_t h2;
    uint8_t fw_ver = 0;
    if (nvs_open(NS, NVS_READWRITE, &h2) == ESP_OK) {
        nvs_get_u8(h2, "fw_mig", &fw_ver);
        if (fw_ver < 3) {
            ESP_LOGW(TAG, "first-boot migration: wipe legacy + mariadb/http cfg");
            nvs_close(h2);
            wipe_legacy_namespaces();
            if (nvs_open(NS, NVS_READWRITE, &h2) == ESP_OK) {
                nvs_set_u8(h2, "fw_mig", 3);
                nvs_commit(h2);
                nvs_close(h2);
            }
        } else {
            nvs_close(h2);
        }
    }

    /* Einmal NVS-stats loggen, damit wir sehen wie voll es ist. */
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        ESP_LOGI(TAG, "NVS: used=%u free=%u total=%u (ns-cnt=%u)",
                 (unsigned)stats.used_entries, (unsigned)stats.free_entries,
                 (unsigned)stats.total_entries,
                 (unsigned)stats.namespace_count);
    }

    ESP_LOGI(TAG, "session store ready, %u entries", cnt);
    return 0;
}

uint16_t indicator_session_store_count(void) {
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return 0;
    uint16_t cnt = 0;
    read_count(h, &cnt);
    nvs_close(h);
    return cnt;
}

int indicator_session_store_append(const struct view_data_session_meta *meta) {
    if (!meta) return -1;
    LOCK();
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) { UNLOCK(); return -2; }

    uint16_t cnt;
    if (read_count(h, &cnt) != ESP_OK) { nvs_close(h); UNLOCK(); return -3; }

    char key[8];
    key_for_slot(cnt, key, sizeof(key));
    esp_err_t err = nvs_set_blob(h, key, meta, sizeof(*meta));

    /* NVS voll? Zuerst legacy namespaces raeumen, dann retry ohne
     * unsere sessions zu verlieren. Nur als letzte option die
     * aelteste SESSION opfern (drop=1 statt 10).                  */
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGW(TAG, "NVS voll - raeume legacy-namespaces (sessions bleiben)");
        nvs_close(h);
        wipe_legacy_namespaces();
        if (open_rw(&h) == ESP_OK) {
            /* retry ohne sessions zu loeschen */
            key_for_slot(cnt, key, sizeof(key));
            err = nvs_set_blob(h, key, meta, sizeof(*meta));
            /* Edge-case: cnt==0 und immer noch voll - da gibts keine
             * aelteste session zum opfern, aber irgendwas anderes in
             * dem namespace belegt entries. Loeschen wir EVERYTHING
             * im sauna_sess ausser dem fw_mig-flag und retry.       */
            if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE && cnt == 0) {
                ESP_LOGW(TAG, "NVS voll bei cnt=0 - wipe sauna_sess"
                         " namespace (fw_mig wird neu gesetzt)");
                nvs_close(h);
                nvs_handle_t hw;
                if (nvs_open(NS, NVS_READWRITE, &hw) == ESP_OK) {
                    nvs_erase_all(hw);
                    nvs_set_u8(hw, "fw_mig", 3);
                    nvs_commit(hw);
                    nvs_close(hw);
                }
                if (open_rw(&h) == ESP_OK) {
                    key_for_slot(0, key, sizeof(key));
                    err = nvs_set_blob(h, key, meta, sizeof(*meta));
                } else {
                    UNLOCK();
                    return -4;
                }
            }

            /* cnt>0 und immer noch voll: BATCH alteste N sessions opfern
             * + commit dazwischen, damit NVS garbage-collection durchlaufen
             * kann und die freigewordenen pages tatsaechlich nutzbar werden.
             * Vorher wurde NUR 1 session geopfert - das reichte nicht weil
             * nach dem erase_key die page noch "dirty" war und der nachfolgende
             * write_count mit NVS_NOT_ENOUGH_SPACE fehlschlug.              */
            if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE && cnt > 0) {
                uint16_t drop = cnt < 5 ? cnt : 5;  /* max 5 alte opfern */
                ESP_LOGW(TAG, "weiterhin voll - opfere %u aelteste sessions"
                         " (von %u)", (unsigned)drop, (unsigned)cnt);
                for (uint16_t i = 0; i < drop; i++) {
                    char k0[8]; key_for_slot(i, k0, sizeof(k0));
                    nvs_erase_key(h, k0);
                }
                nvs_commit(h);   /* gc-triggern */
                /* shift: slot[drop..cnt-1] -> slot[0..cnt-drop-1] */
                for (uint16_t i = drop; i < cnt; i++) {
                    char kold[8], knew[8];
                    key_for_slot(i, kold, sizeof(kold));
                    key_for_slot(i - drop, knew, sizeof(knew));
                    struct view_data_session_meta tmp;
                    size_t sz = sizeof(tmp);
                    if (nvs_get_blob(h, kold, &tmp, &sz) == ESP_OK) {
                        nvs_set_blob(h, knew, &tmp, sizeof(tmp));
                        nvs_erase_key(h, kold);
                    }
                }
                nvs_commit(h);
                cnt -= drop;
                write_count(h, cnt);
                key_for_slot(cnt, key, sizeof(key));
                err = nvs_set_blob(h, key, meta, sizeof(*meta));
            }
        } else {
            UNLOCK();
            return -4;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_blob %s: %d (final)", key, err);
        nvs_close(h); UNLOCK(); return -4;
    }
    cnt++;
    err = write_count(h, cnt);
    nvs_close(h);
    UNLOCK();
    if (err != ESP_OK) { ESP_LOGE(TAG, "write_count: %d", err); return -5; }
    ESP_LOGI(TAG, "appended session %s (slot=%u, total=%u)",
             meta->id, cnt - 1, cnt);
    return 0;
}

int indicator_session_store_list(struct view_data_session_list *out,
                                 uint16_t start_index, uint16_t count) {
    if (!out || !out->items) return -1;
    LOCK();
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) { UNLOCK(); return -2; }

    uint16_t total;
    if (read_count(h, &total) != ESP_OK) { nvs_close(h); UNLOCK(); return -3; }
    out->total = total;
    out->start_index = start_index;
    out->count = 0;

    /* v0.3.0+: ALLE slots laden, dann nach start_ts desc sortieren,
     * dann das gewuenschte fenster ausgeben. Vorher: simple slot-
     * reverse-order, was bei SD-recovery (append-order != chronolog)
     * sessions falsch sortiert hat - testsession nach recovery lag
     * zwischen alten sessions oben/unten je nach append-zeitpunkt. */
    if (start_index >= total) { nvs_close(h); UNLOCK(); return 0; }

    /* Alle slots in einen temp-buffer laden. Bounded durch UI-cap
     * (out->items hat capacity 32 als haupt-konsument).             */
    static struct view_data_session_meta tmp[32];
    uint16_t loaded = 0;
    uint16_t cap_tmp = sizeof(tmp) / sizeof(tmp[0]);
    for (uint16_t slot = 0; slot < total && loaded < cap_tmp; slot++) {
        char key[8];
        key_for_slot(slot, key, sizeof(key));
        size_t sz = sizeof(struct view_data_session_meta);
        esp_err_t err = nvs_get_blob(h, key, &tmp[loaded], &sz);
        if (err == ESP_OK && sz == sizeof(struct view_data_session_meta)) {
            loaded++;
        } else {
            ESP_LOGW(TAG, "skip broken slot %u: %d", slot, err);
        }
    }
    nvs_close(h);
    UNLOCK();

    /* Insertion-sort by start_ts desc (newest first). N <= 32, OK. */
    for (uint16_t i = 1; i < loaded; i++) {
        struct view_data_session_meta key = tmp[i];
        int j = (int)i - 1;
        while (j >= 0 && tmp[j].start_ts < key.start_ts) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }

    /* Pagination: start_index..start_index+count nach sort kopieren. */
    for (uint16_t i = 0; i < count; i++) {
        uint16_t logical = start_index + i;
        if (logical >= loaded) break;
        out->items[out->count++] = tmp[logical];
    }
    return 0;
}

int indicator_session_store_get(const char *id,
                                struct view_data_session_meta *out) {
    if (!id || !out) return -1;
    LOCK();
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) { UNLOCK(); return -2; }
    uint16_t total;
    read_count(h, &total);
    for (uint16_t i = 0; i < total; i++) {
        char key[8];
        key_for_slot(i, key, sizeof(key));
        size_t sz = sizeof(*out);
        esp_err_t err = nvs_get_blob(h, key, out, &sz);
        if (err == ESP_OK && strcmp(out->id, id) == 0) {
            nvs_close(h); UNLOCK();
            return 0;
        }
    }
    nvs_close(h); UNLOCK();
    return -3;  /* not found */
}

int indicator_session_store_delete(const char *id) {
    if (!id) return -1;
    LOCK();
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) { UNLOCK(); return -2; }
    uint16_t total;
    read_count(h, &total);
    if (total == 0) { nvs_close(h); UNLOCK(); return -3; }

    /* Slot finden */
    struct view_data_session_meta tmp;
    int found = -1;
    for (uint16_t i = 0; i < total; i++) {
        char key[8]; key_for_slot(i, key, sizeof(key));
        size_t sz = sizeof(tmp);
        if (nvs_get_blob(h, key, &tmp, &sz) == ESP_OK &&
            strcmp(tmp.id, id) == 0) { found = i; break; }
    }
    if (found < 0) { nvs_close(h); UNLOCK(); return -4; }

    /* Array zusammenschieben: slot[i] = slot[i+1] fuer i=found..total-2 */
    for (uint16_t i = (uint16_t)found; i + 1 < total; i++) {
        char ka[8], kb[8];
        key_for_slot(i,   ka, sizeof(ka));
        key_for_slot(i+1, kb, sizeof(kb));
        size_t sz = sizeof(tmp);
        if (nvs_get_blob(h, kb, &tmp, &sz) != ESP_OK) break;
        nvs_set_blob(h, ka, &tmp, sizeof(tmp));
    }
    /* letzten Slot entfernen */
    char klast[8]; key_for_slot(total - 1, klast, sizeof(klast));
    nvs_erase_key(h, klast);
    write_count(h, total - 1);
    nvs_close(h); UNLOCK();
    ESP_LOGI(TAG, "deleted session %s", id);
    return 0;
}

int indicator_session_store_update(const struct view_data_session_meta *meta) {
    if (!meta) return -1;
    LOCK();
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) { UNLOCK(); return -2; }
    uint16_t total;
    read_count(h, &total);

    /* slot mit match auf meta->id finden */
    struct view_data_session_meta old;
    int found = -1;
    char key[8];
    for (uint16_t i = 0; i < total; i++) {
        key_for_slot(i, key, sizeof(key));
        size_t sz = sizeof(old);
        if (nvs_get_blob(h, key, &old, &sz) == ESP_OK &&
            strcmp(old.id, meta->id) == 0) {
            found = (int)i;
            break;
        }
    }
    if (found < 0) {
        nvs_close(h); UNLOCK();
        ESP_LOGW(TAG, "update: id=%s not found", meta->id);
        return -1;
    }

    /* Merge: gemessene peak + timestamps bewahren, editierbare felder
     * aus meta uebernehmen. aufguss_count ist editierbar (user kann
     * nachtraeglich korrigieren wenn er mal einen aufguss vergessen
     * hat zu markieren).                                              */
    struct view_data_session_meta merged = old;
    strncpy(merged.operator_tag, meta->operator_tag, SSC_OPERATOR_MAXLEN - 1);
    merged.operator_tag[SSC_OPERATOR_MAXLEN - 1] = 0;
    strncpy(merged.aufguss_headline, meta->aufguss_headline,
            SSC_AUFGUSS_NAME_MAXLEN - 1);
    merged.aufguss_headline[SSC_AUFGUSS_NAME_MAXLEN - 1] = 0;
    strncpy(merged.notes, meta->notes, SSC_NOTES_MAXLEN - 1);
    merged.notes[SSC_NOTES_MAXLEN - 1] = 0;
    merged.participants = meta->participants;
    merged.aufguss_count = meta->aufguss_count;

    key_for_slot((uint16_t)found, key, sizeof(key));
    esp_err_t err = nvs_set_blob(h, key, &merged, sizeof(merged));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update set_blob %s failed: %d", key, err);
        nvs_close(h); UNLOCK();
        return -2;
    }
    nvs_commit(h);
    nvs_close(h); UNLOCK();
    ESP_LOGI(TAG, "updated session %s (slot=%d): op='%s' aufg='%s' p=%u",
             meta->id, found, merged.operator_tag, merged.aufguss_headline,
             (unsigned)merged.participants);
    return 0;
}

int indicator_session_store_wipe(void) {
    LOCK();
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) { UNLOCK(); return -2; }
    uint16_t total;
    read_count(h, &total);
    for (uint16_t i = 0; i < total; i++) {
        char key[8]; key_for_slot(i, key, sizeof(key));
        nvs_erase_key(h, key);
    }
    write_count(h, 0);
    nvs_close(h); UNLOCK();
    ESP_LOGI(TAG, "store wiped");
    return 0;
}
