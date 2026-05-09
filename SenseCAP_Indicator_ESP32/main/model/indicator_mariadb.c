#include "indicator_mariadb.h"
#include "indicator_sensor.h"
#include "indicator_storage.h"
#include "indicator_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/sha1.h"
#include <string.h>
#include <time.h>

#define MARIADB_CFG_STORAGE  "mariadb-cfg"
#define MYSQL_TIMEOUT_SEC    10
#define MARIADB_TASK_STACK   (8 * 1024)  /* 8KB stack for DB operations */

static const char *TAG = "mariadb";

static struct mariadb_config __g_config;
static SemaphoreHandle_t __g_mutex;
static esp_timer_handle_t __g_export_timer;
static int __g_last_status = 0;
static time_t __g_last_export_time = 0;
static bool __g_initialized = false;
static volatile bool __g_test_pending = false;
static volatile bool __g_export_pending = false;
static TaskHandle_t __g_db_task_handle = NULL;

/* Forward declarations */
static void __export_timer_callback(void *arg);
static int __do_export(void);

/* MySQL Protocol Constants */
#define MYSQL_PACKET_HEADER_SIZE 4
#define MYSQL_MAX_PACKET_SIZE    1024

/* Simple MySQL packet structure */
typedef struct {
    uint8_t data[MYSQL_MAX_PACKET_SIZE];
    int length;
    uint8_t sequence;
} mysql_packet_t;

static void __config_get(struct mariadb_config *config)
{
    xSemaphoreTake(__g_mutex, portMAX_DELAY);
    memcpy(config, &__g_config, sizeof(struct mariadb_config));
    xSemaphoreGive(__g_mutex);
}

static void __config_set(const struct mariadb_config *config)
{
    xSemaphoreTake(__g_mutex, portMAX_DELAY);
    memcpy(&__g_config, config, sizeof(struct mariadb_config));
    xSemaphoreGive(__g_mutex);
}

static int __config_save(void)
{
    esp_err_t ret;
    struct mariadb_config config;
    __config_get(&config);

    ret = indicator_storage_write(MARIADB_CFG_STORAGE, &config, sizeof(config));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %d", ret);
        return -1;
    }
    ESP_LOGI(TAG, "Config saved successfully");
    return 0;
}

static int __config_restore(void)
{
    esp_err_t ret;
    struct mariadb_config config;
    size_t len = sizeof(config);

    ret = indicator_storage_read(MARIADB_CFG_STORAGE, &config, &len);
    if (ret == ESP_OK && len == sizeof(config)) {
        ESP_LOGI(TAG, "Config restored: enabled=%d, host=%s, port=%d, interval=%d min",
                 config.enabled, config.host, config.port, config.interval_minutes);
        __config_set(&config);
        return 0;
    }

    /* Default configuration with user's credentials */
    ESP_LOGI(TAG, "No saved config, using defaults");
    memset(&config, 0, sizeof(config));
    config.enabled = false;
    config.port = 3308;
    config.interval_minutes = 5;
    strncpy(config.host, "postl.ai", sizeof(config.host) - 1);
    strncpy(config.user, "sensors", sizeof(config.user) - 1);
    strncpy(config.database, "sensors", sizeof(config.database) - 1);
    strncpy(config.table, "sensor_data", sizeof(config.table) - 1);
    /* Password left empty for security */
    __config_set(&config);
    return 0;
}

static void __update_timer(void)
{
    struct mariadb_config config;
    __config_get(&config);

    if (__g_export_timer) {
        esp_timer_stop(__g_export_timer);
    }

    if (config.enabled && config.interval_minutes > 0) {
        uint64_t interval_us = (uint64_t)config.interval_minutes * 60 * 1000000;
        esp_timer_start_periodic(__g_export_timer, interval_us);
        ESP_LOGI(TAG, "Export timer started: every %d minutes", config.interval_minutes);
    } else {
        ESP_LOGI(TAG, "Export timer stopped (disabled)");
    }
}

/* ========== MySQL Protocol Implementation ========== */

static int mysql_read_packet(int sock, mysql_packet_t *pkt)
{
    uint8_t header[4];
    int n = recv(sock, header, 4, 0);
    if (n != 4) {
        ESP_LOGE(TAG, "Failed to read packet header");
        return -1;
    }

    pkt->length = header[0] | (header[1] << 8) | (header[2] << 16);
    pkt->sequence = header[3];

    if (pkt->length > MYSQL_MAX_PACKET_SIZE - 4) {
        ESP_LOGE(TAG, "Packet too large: %d", pkt->length);
        return -1;
    }

    n = recv(sock, pkt->data, pkt->length, 0);
    if (n != pkt->length) {
        ESP_LOGE(TAG, "Failed to read packet data");
        return -1;
    }

    return 0;
}

static int mysql_send_packet(int sock, const uint8_t *data, int len, uint8_t seq)
{
    uint8_t header[4];
    header[0] = len & 0xFF;
    header[1] = (len >> 8) & 0xFF;
    header[2] = (len >> 16) & 0xFF;
    header[3] = seq;

    if (send(sock, header, 4, 0) != 4) return -1;
    if (send(sock, data, len, 0) != len) return -1;
    return 0;
}

/* Native password auth (mysql_native_password) - SHA1 based
 * Algorithm: SHA1(password) XOR SHA1(scramble + SHA1(SHA1(password)))
 */
static void mysql_native_auth(const char *password, const uint8_t *scramble, uint8_t *out)
{
    uint8_t stage1[20];  /* SHA1(password) */
    uint8_t stage2[20];  /* SHA1(stage1) */
    uint8_t combined[40]; /* scramble (20) + stage2 (20) */
    uint8_t stage3[20];  /* SHA1(combined) */

    /* Stage 1: SHA1(password) */
    mbedtls_sha1((const unsigned char *)password, strlen(password), stage1);

    /* Stage 2: SHA1(SHA1(password)) */
    mbedtls_sha1(stage1, 20, stage2);

    /* Combine: scramble + stage2 */
    memcpy(combined, scramble, 20);
    memcpy(combined + 20, stage2, 20);

    /* Stage 3: SHA1(scramble + stage2) */
    mbedtls_sha1(combined, 40, stage3);

    /* Result: stage1 XOR stage3 */
    for (int i = 0; i < 20; i++) {
        out[i] = stage1[i] ^ stage3[i];
    }
}

static int mysql_connect(const char *host, uint16_t port, const char *user,
                         const char *password, const char *database)
{
    struct sockaddr_in server_addr;
    struct hostent *server;
    int sock;
    mysql_packet_t pkt;

    /* Resolve hostname */
    server = gethostbyname(host);
    if (!server) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", host);
        return -1;
    }

    /* Create socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed");
        return -1;
    }

    /* Set timeout */
    struct timeval tv;
    tv.tv_sec = MYSQL_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Connect */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Connection failed to %s:%d", host, port);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Connected to MySQL server %s:%d", host, port);

    /* Read initial handshake packet */
    if (mysql_read_packet(sock, &pkt) < 0) {
        close(sock);
        return -1;
    }

    /* Check for error */
    if (pkt.data[0] == 0xFF) {
        ESP_LOGE(TAG, "Server error during handshake");
        close(sock);
        return -1;
    }

    /* Parse handshake - extract scramble (auth data) */
    uint8_t protocol_version = pkt.data[0];
    ESP_LOGI(TAG, "MySQL protocol version: %d", protocol_version);

    /* Find end of server version string */
    int i = 1;
    while (i < pkt.length && pkt.data[i] != 0) i++;
    i++; /* Skip null terminator */

    /* Skip connection id (4 bytes) */
    i += 4;

    /* Auth plugin data part 1 (8 bytes) */
    uint8_t scramble[20];
    memset(scramble, 0, sizeof(scramble));
    memcpy(scramble, &pkt.data[i], 8);
    i += 8;

    /* Skip filler (1 byte) */
    i++;

    /* Capability flags lower 2 bytes */
    uint32_t server_caps = pkt.data[i] | (pkt.data[i+1] << 8);
    i += 2;

    /* If there's more data, parse extended handshake */
    if (i < pkt.length) {
        /* Character set (1 byte) */
        i++;
        /* Status flags (2 bytes) */
        i += 2;
        /* Capability flags upper 2 bytes */
        server_caps |= (pkt.data[i] | (pkt.data[i+1] << 8)) << 16;
        i += 2;
        /* Auth plugin data length or 0 (1 byte) */
        int auth_plugin_data_len = pkt.data[i];
        i++;
        /* Reserved (10 bytes) */
        i += 10;
        /* Auth plugin data part 2 (rest of scramble, 12 bytes typically) */
        if (auth_plugin_data_len > 8 && i + 12 <= pkt.length) {
            memcpy(scramble + 8, &pkt.data[i], 12);
        }
    }

    ESP_LOGI(TAG, "Server capabilities: 0x%08lx", (unsigned long)server_caps);

    /* Build handshake response */
    uint8_t response[512];
    int resp_len = 0;

    /* Client capabilities (4 bytes) */
    uint32_t client_caps = 0x000FA685; /* CLIENT_PROTOCOL_41 + CLIENT_SECURE_CONNECTION + others */
    if (database && strlen(database) > 0) {
        client_caps |= 0x00000008; /* CLIENT_CONNECT_WITH_DB */
    }
    client_caps |= 0x00080000; /* CLIENT_PLUGIN_AUTH */
    response[resp_len++] = client_caps & 0xFF;
    response[resp_len++] = (client_caps >> 8) & 0xFF;
    response[resp_len++] = (client_caps >> 16) & 0xFF;
    response[resp_len++] = (client_caps >> 24) & 0xFF;

    /* Max packet size (4 bytes) */
    response[resp_len++] = 0x00;
    response[resp_len++] = 0x00;
    response[resp_len++] = 0x00;
    response[resp_len++] = 0x01; /* 16MB */

    /* Character set (1 byte) - utf8mb4 */
    response[resp_len++] = 45; /* utf8mb4_general_ci */

    /* Reserved (23 bytes) */
    memset(&response[resp_len], 0, 23);
    resp_len += 23;

    /* Username (null terminated) */
    strcpy((char *)&response[resp_len], user);
    resp_len += strlen(user) + 1;

    /* Password - use mysql_native_password auth */
    if (password && strlen(password) > 0) {
        uint8_t auth_response[20];
        mysql_native_auth(password, scramble, auth_response);
        response[resp_len++] = 20;  /* Length of auth response */
        memcpy(&response[resp_len], auth_response, 20);
        resp_len += 20;
    } else {
        response[resp_len++] = 0;
    }

    /* Database (if specified) */
    if (database && strlen(database) > 0) {
        strcpy((char *)&response[resp_len], database);
        resp_len += strlen(database) + 1;
    }

    /* Auth plugin name */
    strcpy((char *)&response[resp_len], "mysql_native_password");
    resp_len += strlen("mysql_native_password") + 1;

    /* Send handshake response */
    if (mysql_send_packet(sock, response, resp_len, 1) < 0) {
        ESP_LOGE(TAG, "Failed to send auth response");
        close(sock);
        return -1;
    }

    /* Read auth result */
    if (mysql_read_packet(sock, &pkt) < 0) {
        close(sock);
        return -1;
    }

    if (pkt.data[0] == 0x00) {
        ESP_LOGI(TAG, "MySQL authentication successful");
        return sock;
    } else if (pkt.data[0] == 0xFF) {
        uint16_t err_code = pkt.data[1] | (pkt.data[2] << 8);
        ESP_LOGE(TAG, "MySQL auth error %d: %.*s", err_code, pkt.length - 9, &pkt.data[9]);
        close(sock);
        return -1;
    } else if (pkt.data[0] == 0xFE) {
        /* Auth switch request - server wants different auth method */
        ESP_LOGI(TAG, "Server requests auth switch");
        /* Read auth method name */
        char auth_method[64] = {0};
        int j = 1;
        while (j < pkt.length && pkt.data[j] != 0 && j < 64) {
            auth_method[j-1] = pkt.data[j];
            j++;
        }
        ESP_LOGI(TAG, "Requested auth method: %s", auth_method);

        /* Get new scramble if provided */
        if (j + 1 < pkt.length) {
            memcpy(scramble, &pkt.data[j + 1], 20);
        }

        /* Send auth response for mysql_native_password */
        if (password && strlen(password) > 0) {
            uint8_t auth_response[20];
            mysql_native_auth(password, scramble, auth_response);
            if (mysql_send_packet(sock, auth_response, 20, 3) < 0) {
                ESP_LOGE(TAG, "Failed to send auth switch response");
                close(sock);
                return -1;
            }
        } else {
            uint8_t empty = 0;
            mysql_send_packet(sock, &empty, 0, 3);
        }

        /* Read final auth result */
        if (mysql_read_packet(sock, &pkt) < 0) {
            close(sock);
            return -1;
        }

        if (pkt.data[0] == 0x00) {
            ESP_LOGI(TAG, "MySQL authentication successful (after switch)");
            return sock;
        } else if (pkt.data[0] == 0xFF) {
            uint16_t err_code = pkt.data[1] | (pkt.data[2] << 8);
            ESP_LOGE(TAG, "MySQL auth error after switch %d: %.*s", err_code, pkt.length - 9, &pkt.data[9]);
            close(sock);
            return -1;
        }
    }

    close(sock);
    return -1;
}

static int mysql_query(int sock, const char *query)
{
    mysql_packet_t pkt;
    int query_len = strlen(query);
    uint8_t *cmd = malloc(query_len + 1);
    if (!cmd) return -1;

    cmd[0] = 0x03; /* COM_QUERY */
    memcpy(&cmd[1], query, query_len);

    if (mysql_send_packet(sock, cmd, query_len + 1, 0) < 0) {
        free(cmd);
        return -1;
    }
    free(cmd);

    /* Read response */
    if (mysql_read_packet(sock, &pkt) < 0) {
        return -1;
    }

    if (pkt.data[0] == 0x00) {
        ESP_LOGI(TAG, "Query OK");
        return 0;
    } else if (pkt.data[0] == 0xFF) {
        uint16_t err_code = pkt.data[1] | (pkt.data[2] << 8);
        ESP_LOGE(TAG, "Query error %d: %.*s", err_code, pkt.length - 9, &pkt.data[9]);
        return -1;
    }

    return 0;
}

static int __do_export(void)
{
    struct mariadb_config config;
    struct view_data_sensor sensor_data;
    int ret = -1;
    int sock = -1;
    char *query = NULL;
    bool is_test = __g_test_pending;

    __config_get(&config);

    /* For regular export, check if enabled */
    if (!is_test && !config.enabled) {
        ESP_LOGW(TAG, "Export skipped: disabled");
        return -1;
    }

    /* Check configuration */
    if (strlen(config.host) == 0) {
        ESP_LOGE(TAG, "Export failed: no host configured");
        return -1;
    }
    if (strlen(config.user) == 0) {
        ESP_LOGE(TAG, "Export failed: no user configured");
        return -1;
    }
    if (strlen(config.password) == 0) {
        ESP_LOGE(TAG, "Export failed: no password configured");
        return -1;
    }

    ESP_LOGI(TAG, "Connecting to %s:%d as %s...", config.host, config.port, config.user);

    /* Allocate query buffer on heap to save stack */
    query = malloc(1024);
    if (!query) {
        ESP_LOGE(TAG, "Failed to allocate query buffer");
        return -2;
    }

    /* Get current sensor data */
    if (indicator_sensor_get_data(&sensor_data) != 0) {
        ESP_LOGE(TAG, "Failed to get sensor data");
        free(query);
        return -2;
    }

    /* Connect to MySQL/MariaDB */
    sock = mysql_connect(config.host, config.port, config.user,
                         config.password, config.database);
    if (sock < 0) {
        ESP_LOGE(TAG, "MySQL connection failed to %s:%d", config.host, config.port);
        free(query);
        return -3;
    }

    ESP_LOGI(TAG, "Connected successfully!");

    /* Create table if not exists */
    snprintf(query, 1024,
        "CREATE TABLE IF NOT EXISTS %s ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "timestamp BIGINT NOT NULL,"
        "received_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "temp_internal FLOAT,humidity_internal FLOAT,"
        "co2 FLOAT,tvoc FLOAT,"
        "temp_external FLOAT,humidity_external FLOAT,"
        "pm1_0 FLOAT,pm2_5 FLOAT,pm10 FLOAT,"
        "no2_ppm FLOAT,c2h5oh_ppm FLOAT,voc_ppm FLOAT,co_ppm FLOAT"
        ")", config.table);

    if (mysql_query(sock, query) < 0) {
        ESP_LOGW(TAG, "Create table query failed (may already exist)");
    }

    /* Insert data */
    time_t now = time(NULL);
    snprintf(query, 1024,
        "INSERT INTO %s (timestamp,temp_internal,humidity_internal,co2,tvoc,"
        "temp_external,humidity_external,pm1_0,pm2_5,pm10,"
        "no2_ppm,c2h5oh_ppm,voc_ppm,co_ppm) VALUES "
        "(%ld,%.2f,%.2f,%.1f,%.1f,%.2f,%.2f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f)",
        config.table,
        (long)now,
        sensor_data.temp_internal, sensor_data.humidity_internal,
        sensor_data.co2, sensor_data.tvoc,
        sensor_data.temp_external, sensor_data.humidity_external,
        sensor_data.pm1_0, sensor_data.pm2_5, sensor_data.pm10,
        sensor_data.multigas_gm102b[0], sensor_data.multigas_gm302b[0],
        sensor_data.multigas_gm502b[0], sensor_data.multigas_gm702b[0]);

    if (mysql_query(sock, query) == 0) {
        ret = 0;
        __g_last_export_time = now;
        ESP_LOGI(TAG, "Data exported to MariaDB successfully");
    } else {
        ret = -4;
        ESP_LOGE(TAG, "Failed to insert data");
    }

    close(sock);
    free(query);
    __g_last_status = ret;
    return ret;
}

/* ===================================================================== */
/* Sauna-Session Export                                                  */
/*                                                                       */
/* Schreibt zwei Tabellen:                                               */
/*   sauna_sessions  (eine Zeile pro Session mit Metadaten + Peaks)      */
/*   sauna_samples   (ein Sample pro Zeile, aufguss_marker-Spalte)       */
/*                                                                       */
/* Die aufguss-Namen liegen in den Samples nicht mit; die Kernkurve      */
/* bekommt ein BOOLEAN-Flag, die Headline des ersten Aufgusses ist in    */
/* der session-Zeile. Fuer detaillierte Aufguss-Zeitstempel + Namen      */
/* kann die SD-CSV direkt abgegriffen werden.                            */
/* ===================================================================== */

static void sql_escape_into(char *dst, size_t dstlen, const char *src) {
    /* Minimaler Escaper fuer Single-Quotes + Backslash. Bewusst in-line
     * statt extern, damit der Sauna-Export autark bleibt.            */
    size_t i = 0, j = 0;
    if (!src) { dst[0] = 0; return; }
    while (src[i] && j + 2 < dstlen) {
        char c = src[i++];
        if (c == '\'' || c == '\\') {
            if (j + 3 >= dstlen) break;
            dst[j++] = '\\';
        }
        dst[j++] = c;
    }
    dst[j] = 0;
}

int indicator_mariadb_export_session(
        const struct view_data_session_meta *meta,
        const struct view_data_session_sample *samples,
        uint16_t n)
{
    if (!meta) return -1;

    struct mariadb_config cfg;
    if (indicator_mariadb_get_config(&cfg) != 0) return -2;
    if (!cfg.enabled)                          return 0;  /* nicht konfig. */

    int sock = mysql_connect(cfg.host, cfg.port, cfg.user,
                             cfg.password, cfg.database);
    if (sock < 0) {
        ESP_LOGE(TAG, "sauna-export: connect failed");
        return -3;
    }

    char *q = (char *)malloc(2048);
    if (!q) { close(sock); return -4; }

    /* Tabellen anlegen - ohne FK, damit das Schema verzeihlich bleibt. */
    snprintf(q, 2048,
        "CREATE TABLE IF NOT EXISTS sauna_sessions ("
        "id VARCHAR(32) PRIMARY KEY,"
        "start_ts BIGINT NOT NULL,"
        "end_ts   BIGINT NOT NULL,"
        "operator_tag VARCHAR(32),"
        "aufguss_headline VARCHAR(64),"
        "participants INT,"
        "notes TEXT,"
        "peak_temp FLOAT,"
        "peak_rh FLOAT,"
        "aufguss_count INT,"
        "received_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ")");
    if (mysql_query(sock, q) < 0)
        ESP_LOGW(TAG, "CREATE sauna_sessions: ignored (exists?)");

    snprintf(q, 2048,
        "CREATE TABLE IF NOT EXISTS sauna_samples ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "session_id VARCHAR(32) NOT NULL,"
        "t_elapsed_s INT NOT NULL,"
        "temp FLOAT, rh FLOAT,"
        "aufguss_marker TINYINT(1) DEFAULT 0,"
        "INDEX idx_session (session_id, t_elapsed_s)"
        ")");
    if (mysql_query(sock, q) < 0)
        ESP_LOGW(TAG, "CREATE sauna_samples: ignored (exists?)");

    /* Session-Metadaten einfuegen (upsert-aehnlich via REPLACE). */
    char op_esc[64], head_esc[128], notes_esc[512];
    sql_escape_into(op_esc,    sizeof(op_esc),    meta->operator_tag);
    sql_escape_into(head_esc,  sizeof(head_esc),  meta->aufguss_headline);
    sql_escape_into(notes_esc, sizeof(notes_esc), meta->notes);

    snprintf(q, 2048,
        "REPLACE INTO sauna_sessions "
        "(id,start_ts,end_ts,operator_tag,aufguss_headline,participants,"
        "notes,peak_temp,peak_rh,aufguss_count) VALUES "
        "('%s',%ld,%ld,'%s','%s',%u,'%s',%.2f,%.2f,%u)",
        meta->id,
        (long)meta->start_ts, (long)meta->end_ts,
        op_esc, head_esc,
        (unsigned)meta->participants,
        notes_esc,
        isnan(meta->peak_temp) ? 0.0f : meta->peak_temp,
        isnan(meta->peak_rh)   ? 0.0f : meta->peak_rh,
        (unsigned)meta->aufguss_count);
    int rc = mysql_query(sock, q);
    if (rc < 0) {
        ESP_LOGE(TAG, "INSERT sauna_sessions failed");
        free(q); close(sock); return -5;
    }

    /* Samples in 100er-Chunks batch-inserten. */
    const uint16_t BATCH = 100;
    for (uint16_t off = 0; off < n; off += BATCH) {
        uint16_t take = (n - off < BATCH) ? (n - off) : BATCH;
        /* groben Platz berechnen: 1 Zeile ~80 chars */
        char *big = (char *)malloc(200 + take * 80);
        if (!big) { ESP_LOGE(TAG, "samples alloc fail"); break; }
        int pos = snprintf(big, 200 + take * 80,
            "INSERT INTO sauna_samples "
            "(session_id,t_elapsed_s,temp,rh,aufguss_marker) VALUES ");
        for (uint16_t i = 0; i < take; i++) {
            const struct view_data_session_sample *s = &samples[off + i];
            pos += snprintf(big + pos, 200 + take * 80 - pos,
                "%s('%s',%u,%.2f,%.2f,%u)",
                (i == 0) ? "" : ",",
                meta->id,
                (unsigned)s->t_elapsed_s,
                isnan(s->temp) ? 0.0f : s->temp,
                isnan(s->rh)   ? 0.0f : s->rh,
                (unsigned)s->has_aufguss_marker);
        }
        if (mysql_query(sock, big) < 0) {
            ESP_LOGW(TAG, "samples batch insert failed (off=%u)", off);
            free(big);
            break;
        }
        free(big);
    }

    close(sock);
    free(q);
    ESP_LOGI(TAG, "sauna export ok: session=%s samples=%u", meta->id, n);
    return 0;
}

/* ===================================================================== */
/* Live-Streaming Helpers (begin / append / finalize)                    */
/* ===================================================================== */

static int __ensure_tables(int sock) {
    char *q = (char *)malloc(2048);
    if (!q) return -1;
    /* Schema-migration fuer bestehende tabellen mit alter NOT-NULL-
     * constraint auf end_ts. Best effort, ignoriert error wenn die
     * spalte schon nullable ist.                                   */
    snprintf(q, 2048,
        "ALTER TABLE sauna_sessions MODIFY COLUMN end_ts BIGINT DEFAULT NULL");
    mysql_query(sock, q);
    snprintf(q, 2048,
        "CREATE TABLE IF NOT EXISTS sauna_sessions ("
        "id VARCHAR(32) PRIMARY KEY,"
        "start_ts BIGINT NOT NULL,"
        "end_ts   BIGINT DEFAULT NULL,"
        "operator_tag VARCHAR(32),"
        "aufguss_headline VARCHAR(64),"
        "participants INT,"
        "notes TEXT,"
        "peak_temp FLOAT DEFAULT NULL,"
        "peak_rh FLOAT DEFAULT NULL,"
        "aufguss_count INT DEFAULT 0,"
        "received_at DATETIME DEFAULT CURRENT_TIMESTAMP)");
    mysql_query(sock, q);
    snprintf(q, 2048,
        "CREATE TABLE IF NOT EXISTS sauna_samples ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "session_id VARCHAR(32) NOT NULL,"
        "t_elapsed_s INT NOT NULL,"
        "temp FLOAT, rh FLOAT,"
        "aufguss_marker TINYINT(1) DEFAULT 0,"
        "INDEX idx_session (session_id, t_elapsed_s))");
    mysql_query(sock, q);
    free(q);
    return 0;
}

int indicator_mariadb_begin_session(const char *id, time_t start_ts,
                                    const char *operator_tag) {
    struct mariadb_config cfg;
    if (indicator_mariadb_get_config(&cfg) != 0) return -2;
    if (!cfg.enabled) return 0;
    int sock = mysql_connect(cfg.host, cfg.port, cfg.user,
                             cfg.password, cfg.database);
    if (sock < 0) return -3;
    __ensure_tables(sock);
    char op_esc[64];
    sql_escape_into(op_esc, sizeof(op_esc),
                    operator_tag ? operator_tag : "");
    char q[512];
    snprintf(q, sizeof(q),
        "REPLACE INTO sauna_sessions "
        "(id,start_ts,operator_tag,aufguss_count) VALUES "
        "('%s',%ld,'%s',0)",
        id, (long)start_ts, op_esc);
    int rc = mysql_query(sock, q);
    close(sock);
    if (rc < 0) { ESP_LOGE(TAG, "begin_session failed"); return -4; }
    ESP_LOGI(TAG, "mariadb: begin session %s", id);
    return 0;
}

int indicator_mariadb_append_samples(const char *id,
        const struct view_data_session_sample *samples, uint16_t n) {
    if (!id || !samples || n == 0) return 0;
    struct mariadb_config cfg;
    if (indicator_mariadb_get_config(&cfg) != 0) return -2;
    if (!cfg.enabled) return 0;
    int sock = mysql_connect(cfg.host, cfg.port, cfg.user,
                             cfg.password, cfg.database);
    if (sock < 0) return -3;
    const uint16_t BATCH = 100;
    int total_rc = 0;
    for (uint16_t off = 0; off < n; off += BATCH) {
        uint16_t take = (n - off < BATCH) ? (n - off) : BATCH;
        char *big = (char *)malloc(200 + take * 80);
        if (!big) { total_rc = -4; break; }
        int pos = snprintf(big, 200 + take * 80,
            "INSERT INTO sauna_samples "
            "(session_id,t_elapsed_s,temp,rh,aufguss_marker) VALUES ");
        for (uint16_t i = 0; i < take; i++) {
            const struct view_data_session_sample *s = &samples[off + i];
            pos += snprintf(big + pos, 200 + take * 80 - pos,
                "%s('%s',%u,%.2f,%.2f,%u)",
                (i == 0) ? "" : ",",
                id, (unsigned)s->t_elapsed_s,
                isnan(s->temp) ? 0.0f : s->temp,
                isnan(s->rh)   ? 0.0f : s->rh,
                (unsigned)s->has_aufguss_marker);
        }
        if (mysql_query(sock, big) < 0) total_rc = -5;
        free(big);
    }
    close(sock);
    if (total_rc == 0)
        ESP_LOGI(TAG, "mariadb: live-push %u samples for %s", n, id);
    return total_rc;
}

int indicator_mariadb_finalize_session(
        const struct view_data_session_meta *meta) {
    if (!meta) return -1;
    struct mariadb_config cfg;
    if (indicator_mariadb_get_config(&cfg) != 0) return -2;
    if (!cfg.enabled) return 0;
    int sock = mysql_connect(cfg.host, cfg.port, cfg.user,
                             cfg.password, cfg.database);
    if (sock < 0) return -3;
    char op_esc[64], head_esc[128], notes_esc[512];
    sql_escape_into(op_esc,    sizeof(op_esc),    meta->operator_tag);
    sql_escape_into(head_esc,  sizeof(head_esc),  meta->aufguss_headline);
    sql_escape_into(notes_esc, sizeof(notes_esc), meta->notes);
    char q[1024];
    snprintf(q, sizeof(q),
        "UPDATE sauna_sessions SET end_ts=%ld, operator_tag='%s', "
        "aufguss_headline='%s', participants=%u, notes='%s', "
        "peak_temp=%.2f, peak_rh=%.2f, aufguss_count=%u "
        "WHERE id='%s'",
        (long)meta->end_ts, op_esc, head_esc,
        (unsigned)meta->participants, notes_esc,
        isnan(meta->peak_temp) ? 0.0f : meta->peak_temp,
        isnan(meta->peak_rh)   ? 0.0f : meta->peak_rh,
        (unsigned)meta->aufguss_count,
        meta->id);
    int rc = mysql_query(sock, q);
    close(sock);
    if (rc < 0) return -4;
    ESP_LOGI(TAG, "mariadb: finalize session %s", meta->id);
    return 0;
}

static void __export_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "Export timer triggered");
    __g_export_pending = true;
    if (__g_db_task_handle) {
        xTaskNotifyGive(__g_db_task_handle);
    }
}

/* Database task - runs DB operations with larger stack */
static void __db_task(void *arg)
{
    ESP_LOGI(TAG, "MariaDB task started (stack: %d bytes free)",
             uxTaskGetStackHighWaterMark(NULL) * 4);

    while (1) {
        /* Wait for notification with timeout */
        uint32_t notify = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

        if (notify > 0 || __g_test_pending || __g_export_pending) {
            ESP_LOGI(TAG, "DB operation triggered: test=%d, export=%d, notify=%lu",
                     __g_test_pending, __g_export_pending, (unsigned long)notify);

            bool was_test = __g_test_pending;

            int result = __do_export();
            __g_last_status = result;

            ESP_LOGI(TAG, "DB operation complete: result=%d, was_test=%d", result, was_test);

            __g_test_pending = false;
            __g_export_pending = false;

            /* Log stack usage after operation */
            ESP_LOGI(TAG, "Stack high water mark: %d bytes",
                     uxTaskGetStackHighWaterMark(NULL) * 4);
        }
    }
}

int indicator_mariadb_init(void)
{
    if (__g_initialized) {
        return 0;
    }

    __g_mutex = xSemaphoreCreateMutex();
    if (!__g_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return -1;
    }

    /* Restore configuration */
    __config_restore();

    /* Create database task with larger stack */
    BaseType_t ret = xTaskCreate(__db_task, "mariadb_task", MARIADB_TASK_STACK,
                                  NULL, 5, &__g_db_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DB task");
        return -1;
    }

    /* Create export timer.
     * v0.3.3: war ESP_ERROR_CHECK -> abort bei DRAM-druck. mariadb ist
     * optional, init laeuft aber bei JEDEM boot ueber indicator_model.c.
     * Bei knappem DRAM (gleicher trigger wie v0.3.2 wifi-NO_MEM) wuerde
     * das einen bootloop ausloesen. Graceful fail: db-task laeuft weiter,
     * timer-getriebener export ist bis zum reboot deaktiviert.            */
    const esp_timer_create_args_t timer_args = {
        .callback = __export_timer_callback,
        .arg = NULL,
        .name = "mariadb_export"
    };
    esp_err_t err = esp_timer_create(&timer_args, &__g_export_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create fehlgeschlagen: %s - mariadb-export deaktiviert",
                 esp_err_to_name(err));
        __g_export_timer = NULL;
        __g_initialized = true;  /* db-task + manuelle exports bleiben nutzbar */
        return -1;
    }

    /* Start timer if enabled */
    __update_timer();

    __g_initialized = true;
    ESP_LOGI(TAG, "MariaDB module initialized");
    return 0;
}

int indicator_mariadb_get_config(struct mariadb_config *config)
{
    if (!config) return -1;
    __config_get(config);
    return 0;
}

int indicator_mariadb_set_config(const struct mariadb_config *config)
{
    if (!config) return -1;

    ESP_LOGI(TAG, "Setting config: enabled=%d, host=%s, port=%d, interval=%d min",
             config->enabled, config->host, config->port, config->interval_minutes);

    __config_set(config);
    __config_save();
    __update_timer();
    return 0;
}

int indicator_mariadb_test_connection(void)
{
    if (!__g_db_task_handle) {
        ESP_LOGE(TAG, "DB task not running");
        __g_last_status = -1;
        return -1;
    }

    /* Wait if a test is already in progress */
    if (__g_test_pending || __g_export_pending) {
        ESP_LOGW(TAG, "Operation already in progress, waiting...");
        for (int i = 0; i < 50 && (__g_test_pending || __g_export_pending); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* Reset status and trigger test (async) */
    __g_last_status = -99;  /* Pending - caller should poll get_last_status() */
    __g_test_pending = true;
    __g_export_pending = false;

    ESP_LOGI(TAG, "Test connection triggered (async), notifying task...");
    xTaskNotifyGive(__g_db_task_handle);

    return 0;  /* Triggered successfully, result will be in get_last_status() */
}

int indicator_mariadb_export_now(void)
{
    if (!__g_db_task_handle) {
        ESP_LOGE(TAG, "DB task not running");
        return -1;
    }

    __g_export_pending = true;
    xTaskNotifyGive(__g_db_task_handle);
    return 0;  /* Triggered, actual result available via get_last_status */
}

int indicator_mariadb_get_last_status(void)
{
    return __g_last_status;
}

time_t indicator_mariadb_get_last_export_time(void)
{
    return __g_last_export_time;
}
