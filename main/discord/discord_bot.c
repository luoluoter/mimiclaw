#include "discord_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "net/net_mutex.h"

static const char *TAG = "discord";

static char s_bot_token[128] = MIMI_SECRET_DISCORD_TOKEN;
static char s_bot_user_id[32] = {0};

#define DISCORD_KEY_CHAN_FMT "chan_%d"
#define DISCORD_KEY_LAST_FMT "last_%d"

static char s_channels[MIMI_DISCORD_MAX_CHANNELS][32] = {{0}};
static uint64_t s_last_seen[MIMI_DISCORD_MAX_CHANNELS] = {0};
static uint64_t s_last_saved[MIMI_DISCORD_MAX_CHANNELS] = {0};
static int64_t s_last_save_us[MIMI_DISCORD_MAX_CHANNELS] = {0};

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static void log_http_client_error(esp_http_client_handle_t client,
                                  const char *label,
                                  esp_err_t err,
                                  int status)
{
    int sock_errno = esp_http_client_get_errno(client);
    int tls_code = 0;
    int tls_flags = 0;
    esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(
        client, &tls_code, &tls_flags);
    ESP_LOGE(TAG,
             "%s failed: err=%s status=%d errno=%d tls_err=%s tls_code=0x%x tls_flags=0x%x",
             label, esp_err_to_name(err), status, sock_errno,
             esp_err_to_name(tls_err), tls_code, tls_flags);
}

static bool retryable_http_err(esp_err_t err)
{
    return err != ESP_ERR_NO_MEM && err != ESP_ERR_INVALID_ARG;
}

static void backoff_delay_ms(int *backoff_ms)
{
    int jitter = (int)(esp_random() % 200);
    int delay = *backoff_ms + jitter;
    vTaskDelay(pdMS_TO_TICKS(delay));
    *backoff_ms = (*backoff_ms < MIMI_HTTP_RETRY_MAX_MS / 2)
                      ? (*backoff_ms * 2)
                      : MIMI_HTTP_RETRY_MAX_MS;
}

static const char *guess_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    return "application/octet-stream";
}

static void make_key(char *out, size_t out_size, const char *fmt, int index)
{
    snprintf(out, out_size, fmt, index);
}

static uint64_t str_to_u64(const char *s)
{
    if (!s) return 0;
    return (uint64_t)strtoull(s, NULL, 10);
}

static void save_last_seen_if_needed(int idx, bool force)
{
    if (idx < 0 || idx >= MIMI_DISCORD_MAX_CHANNELS) return;
    if (s_last_seen[idx] == 0) return;

    int64_t now = esp_timer_get_time();
    bool should_save = force;
    if (!should_save && s_last_saved[idx] > 0) {
        if ((s_last_seen[idx] - s_last_saved[idx]) >= MIMI_DISCORD_SAVE_STEP) {
            should_save = true;
        } else if ((now - s_last_save_us[idx]) >= MIMI_DISCORD_SAVE_INTERVAL_US) {
            should_save = true;
        }
    } else if (!should_save) {
        should_save = true;
    }
    if (!should_save) return;

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_DISCORD, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    char key[16];
    make_key(key, sizeof(key), DISCORD_KEY_LAST_FMT, idx);
    if (nvs_set_i64(nvs, key, (int64_t)s_last_seen[idx]) == ESP_OK &&
        nvs_commit(nvs) == ESP_OK) {
        s_last_saved[idx] = s_last_seen[idx];
        s_last_save_us[idx] = now;
    }
    nvs_close(nvs);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len >= resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1) {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

static int parse_retry_after_ms(const char *body)
{
    if (!body) return -1;
    cJSON *root = cJSON_Parse(body);
    if (!root) return -1;
    cJSON *retry = cJSON_GetObjectItem(root, "retry_after");
    int ms = -1;
    if (cJSON_IsNumber(retry)) {
        double seconds = retry->valuedouble;
        if (seconds < 0) seconds = 0;
        ms = (int)(seconds * 1000.0);
    }
    cJSON_Delete(root);
    return ms;
}

static char *discord_http_request(const char *method, const char *path,
                                  const char *post_data, int *out_status)
{
    if (out_status) *out_status = 0;
    if (!s_bot_token[0]) return NULL;
    if (!wifi_manager_is_connected()) {
        return NULL;
    }

    const bool is_poll_get = (method && strcmp(method, "GET") == 0 && post_data == NULL);
    const int timeout_ms = is_poll_get ? 12000 : 30000;
    const int max_retry = is_poll_get ? 1 : MIMI_DISCORD_HTTP_MAX_RETRY;
    int attempt = 0;
    int backoff_ms = MIMI_HTTP_RETRY_BASE_MS;

    while (attempt < max_retry) {
        if (!wifi_manager_is_connected()) {
            return NULL;
        }
        esp_err_t lock_err = net_mutex_lock(pdMS_TO_TICKS(MIMI_NET_MUTEX_TIMEOUT_MS));
        if (lock_err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP lock failed: %s", esp_err_to_name(lock_err));
            if (!retryable_http_err(lock_err) || attempt + 1 >= max_retry) {
                return NULL;
            }
            backoff_delay_ms(&backoff_ms);
            attempt++;
            continue;
        }
        char url[256];
        snprintf(url, sizeof(url), "https://discord.com/api/v10%s", path);

        http_resp_t resp = {
            .buf = calloc(1, 4096),
            .len = 0,
            .cap = 4096,
        };
        if (!resp.buf) {
            net_mutex_unlock();
            return NULL;
        }

        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .user_data = &resp,
            .timeout_ms = timeout_ms,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            free(resp.buf);
            net_mutex_unlock();
            return NULL;
        }

        if (method && strcmp(method, "POST") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
        } else {
            esp_http_client_set_method(client, HTTP_METHOD_GET);
        }

        char auth[160];
        snprintf(auth, sizeof(auth), "Bot %s", s_bot_token);
        esp_http_client_set_header(client, "Authorization", auth);
        if (post_data) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, post_data, strlen(post_data));
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (err != ESP_OK) {
            log_http_client_error(client, "HTTP request", err, status);
        }
        esp_http_client_cleanup(client);
        net_mutex_unlock();

        if (out_status) *out_status = status;

        if (err != ESP_OK) {
            free(resp.buf);
            if (!retryable_http_err(err) || attempt + 1 >= max_retry) {
                return NULL;
            }
            backoff_delay_ms(&backoff_ms);
            attempt++;
            continue;
        }

        if (status == 429) {
            int retry_ms = parse_retry_after_ms(resp.buf);
            if (retry_ms < 0) retry_ms = 1000;
            ESP_LOGW(TAG, "Rate limited, retry after %d ms", retry_ms);
            free(resp.buf);
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            attempt++;
            continue;
        }

        if (status >= 500) {
            ESP_LOGW(TAG, "Discord 5xx (%d), backing off %d ms", status, backoff_ms);
            free(resp.buf);
            backoff_delay_ms(&backoff_ms);
            attempt++;
            continue;
        }

        return resp.buf;
    }

    return NULL;
}

static bool load_channels_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_DISCORD, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    bool any = false;
    for (int i = 0; i < MIMI_DISCORD_MAX_CHANNELS; i++) {
        char key[16];
        size_t len = sizeof(s_channels[i]);
        make_key(key, sizeof(key), DISCORD_KEY_CHAN_FMT, i);
        if (nvs_get_str(nvs, key, s_channels[i], &len) == ESP_OK && s_channels[i][0]) {
            any = true;
            make_key(key, sizeof(key), DISCORD_KEY_LAST_FMT, i);
            int64_t last = 0;
            if (nvs_get_i64(nvs, key, &last) == ESP_OK && last > 0) {
                s_last_seen[i] = (uint64_t)last;
                s_last_saved[i] = s_last_seen[i];
            }
        } else {
            s_channels[i][0] = '\0';
            s_last_seen[i] = 0;
            s_last_saved[i] = 0;
        }
    }
    nvs_close(nvs);
    return any;
}

static bool channel_exists(const char *channel_id, int *out_idx)
{
    for (int i = 0; i < MIMI_DISCORD_MAX_CHANNELS; i++) {
        if (s_channels[i][0] && strcmp(s_channels[i], channel_id) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

static bool has_channels(void)
{
    for (int i = 0; i < MIMI_DISCORD_MAX_CHANNELS; i++) {
        if (s_channels[i][0]) return true;
    }
    return false;
}

static esp_err_t discord_fetch_bot_id(void)
{
    int status = 0;
    char *resp = discord_http_request("GET", "/users/@me", NULL, &status);
    if (!resp) return ESP_FAIL;

    if (status != 200) {
        ESP_LOGE(TAG, "get_me failed: status=%d body=%.120s", status, resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return ESP_FAIL;
    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id) && id->valuestring) {
        strncpy(s_bot_user_id, id->valuestring, sizeof(s_bot_user_id) - 1);
        s_bot_user_id[sizeof(s_bot_user_id) - 1] = '\0';
        ESP_LOGI(TAG, "Discord bot id: %s", s_bot_user_id);
    }
    cJSON_Delete(root);
    return s_bot_user_id[0] ? ESP_OK : ESP_FAIL;
}

static void process_channel_messages(int idx)
{
    const char *channel_id = s_channels[idx];
    if (!channel_id[0]) return;

    bool bootstrap = (s_last_seen[idx] == 0);

    char path[256];
    if (bootstrap) {
        snprintf(path, sizeof(path),
                 "/channels/%s/messages?limit=1", channel_id);
    } else {
        snprintf(path, sizeof(path),
                 "/channels/%s/messages?limit=50&after=%" PRIu64,
                 channel_id, (uint64_t)s_last_seen[idx]);
    }

    int status = 0;
    char *resp = discord_http_request("GET", path, NULL, &status);
    if (!resp) return;

    if (status != 200) {
        ESP_LOGW(TAG, "Discord get messages failed: status=%d body=%.120s", status, resp);
        free(resp);
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    int count = cJSON_GetArraySize(root);
    uint64_t max_id = s_last_seen[idx];

    for (int i = count - 1; i >= 0; i--) {
        cJSON *msg = cJSON_GetArrayItem(root, i);
        if (!msg) continue;

        cJSON *id = cJSON_GetObjectItem(msg, "id");
        if (!cJSON_IsString(id) || !id->valuestring) continue;
        uint64_t msg_id = str_to_u64(id->valuestring);
        if (msg_id > max_id) max_id = msg_id;

        if (bootstrap) {
            continue; /* Do not process old messages on first run */
        }

        cJSON *author = cJSON_GetObjectItem(msg, "author");
        if (author) {
            cJSON *is_bot = cJSON_GetObjectItem(author, "bot");
            if (cJSON_IsTrue(is_bot)) {
                continue;
            }
            cJSON *author_id = cJSON_GetObjectItem(author, "id");
            if (cJSON_IsString(author_id) && author_id->valuestring &&
                s_bot_user_id[0] && strcmp(author_id->valuestring, s_bot_user_id) == 0) {
                continue;
            }
        }

        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!cJSON_IsString(content) || !content->valuestring || content->valuestring[0] == '\0') {
            continue;
        }

        mimi_msg_t in = {0};
        strncpy(in.channel, MIMI_CHAN_DISCORD, sizeof(in.channel) - 1);
        strncpy(in.chat_id, channel_id, sizeof(in.chat_id) - 1);
        in.content = strdup(content->valuestring);
        if (in.content) {
            if (message_bus_push_inbound(&in) != ESP_OK) {
                ESP_LOGW(TAG, "Inbound queue full, drop discord message");
                free(in.content);
            }
        }
    }

    if (max_id > s_last_seen[idx]) {
        s_last_seen[idx] = max_id;
        save_last_seen_if_needed(idx, bootstrap);
    }

    cJSON_Delete(root);
}

static void discord_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Discord polling task started");
    discord_fetch_bot_id();

    while (1) {
        if (!s_bot_token[0] || !has_channels()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        for (int i = 0; i < MIMI_DISCORD_MAX_CHANNELS; i++) {
            if (s_channels[i][0]) {
                process_channel_messages(i);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MIMI_DISCORD_POLL_INTERVAL_MS));
    }
}

/* --- Public API --- */

bool discord_is_configured(void)
{
    return s_bot_token[0] != '\0' && has_channels();
}

esp_err_t discord_bot_init(void)
{
    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_DISCORD, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_DISCORD_TOKEN, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_bot_token, tmp, sizeof(s_bot_token) - 1);
            s_bot_token[sizeof(s_bot_token) - 1] = '\0';
        }
        nvs_close(nvs);
    }

    load_channels_from_nvs();

    if (s_bot_token[0]) {
        ESP_LOGI(TAG, "Discord bot token loaded (len=%d)", (int)strlen(s_bot_token));
    } else {
        ESP_LOGW(TAG, "No Discord bot token. Use CLI: set_discord_token <TOKEN>");
    }
    if (!has_channels()) {
        ESP_LOGW(TAG, "No Discord channels configured. Use CLI: discord_channel_add <ID>");
    }

    return ESP_OK;
}

esp_err_t discord_bot_start(void)
{
    if (!discord_is_configured()) {
        ESP_LOGW(TAG, "Discord not configured, polling task disabled");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        discord_poll_task, "discord_poll",
        MIMI_DISCORD_POLL_STACK, NULL,
        MIMI_DISCORD_POLL_PRIO, NULL, MIMI_DISCORD_POLL_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t discord_send_message(const char *channel_id, const char *text)
{
    if (!s_bot_token[0]) {
        ESP_LOGW(TAG, "Cannot send: no discord bot token");
        return ESP_ERR_INVALID_STATE;
    }
    if (!channel_id || !text) return ESP_ERR_INVALID_ARG;

    size_t text_len = strlen(text);
    size_t offset = 0;
    int all_ok = 1;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_DISCORD_MAX_MSG_LEN) {
            chunk = MIMI_DISCORD_MAX_MSG_LEN;
        }

        char *segment = malloc(chunk + 1);
        if (!segment) return ESP_ERR_NO_MEM;
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "content", segment);
        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        free(segment);
        if (!json_str) return ESP_ERR_NO_MEM;

        char path[256];
        snprintf(path, sizeof(path), "/channels/%s/messages", channel_id);

        int status = 0;
        char *resp = discord_http_request("POST", path, json_str, &status);
        free(json_str);

        bool ok = (resp && status >= 200 && status < 300);
        if (!ok) {
            ESP_LOGE(TAG, "Discord send failed: status=%d body=%.120s", status, resp ? resp : "(null)");
            all_ok = 0;
        }
        free(resp);

        offset += chunk;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t discord_send_file(const char *channel_id, const char *path, const char *caption)
{
    if (!s_bot_token[0]) {
        ESP_LOGW(TAG, "Cannot send file: no discord bot token");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "Cannot send file: WiFi disconnected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!channel_id || !channel_id[0] || !path || !path[0]) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "File not found or empty: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    const char *mime = guess_mime_type(path);

    cJSON *payload = cJSON_CreateObject();
    if (caption && caption[0]) {
        cJSON_AddStringToObject(payload, "content", caption);
    }
    char *payload_json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!payload_json) return ESP_ERR_NO_MEM;

    const char *boundary = "----mimiBoundary9B4hSDJ8lP";
    char part1[512];
    char part2[512];
    char closing[64];
    int p1 = snprintf(part1, sizeof(part1),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"payload_json\"\r\n"
        "Content-Type: application/json\r\n\r\n"
        "%s\r\n",
        boundary, payload_json);
    int p2 = snprintf(part2, sizeof(part2),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"files[0]\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n",
        boundary, filename, mime);
    int p3 = snprintf(closing, sizeof(closing),
        "\r\n--%s--\r\n", boundary);

    free(payload_json);

    if (p1 <= 0 || p1 >= (int)sizeof(part1) ||
        p2 <= 0 || p2 >= (int)sizeof(part2) ||
        p3 <= 0 || p3 >= (int)sizeof(closing)) {
        ESP_LOGE(TAG, "Multipart header too large");
        return ESP_FAIL;
    }

    size_t total_len = (size_t)p1 + (size_t)p2 + (size_t)st.st_size + (size_t)p3;
    if (total_len > INT32_MAX) {
        ESP_LOGE(TAG, "File too large for upload: %u bytes", (unsigned)total_len);
        return ESP_ERR_INVALID_ARG;
    }

    int attempt = 0;
    int backoff_ms = MIMI_HTTP_RETRY_BASE_MS;

    while (attempt < MIMI_DISCORD_HTTP_MAX_RETRY) {
        if (!wifi_manager_is_connected()) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t lock_err = net_mutex_lock(pdMS_TO_TICKS(MIMI_NET_MUTEX_TIMEOUT_MS));
        if (lock_err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP lock failed: %s", esp_err_to_name(lock_err));
            if (!retryable_http_err(lock_err) || attempt + 1 >= MIMI_DISCORD_HTTP_MAX_RETRY) {
                return lock_err;
            }
            backoff_delay_ms(&backoff_ms);
            attempt++;
            continue;
        }

        char url[256];
        snprintf(url, sizeof(url), "https://discord.com/api/v10/channels/%s/messages", channel_id);

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 60000,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            net_mutex_unlock();
            return ESP_FAIL;
        }

        esp_http_client_set_method(client, HTTP_METHOD_POST);

        char auth[160];
        snprintf(auth, sizeof(auth), "Bot %s", s_bot_token);
        esp_http_client_set_header(client, "Authorization", auth);
        char content_type[128];
        snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
        esp_http_client_set_header(client, "Content-Type", content_type);
        esp_http_client_set_header(client, "Accept", "application/json");

        esp_err_t err = esp_http_client_open(client, (int)total_len);
        if (err != ESP_OK) {
            log_http_client_error(client, "HTTP upload open", err, 0);
            esp_http_client_cleanup(client);
            net_mutex_unlock();
            if (!retryable_http_err(err) || attempt + 1 >= MIMI_DISCORD_HTTP_MAX_RETRY) {
                return err;
            }
            backoff_delay_ms(&backoff_ms);
            attempt++;
            continue;
        }

        bool write_ok = true;
        if (esp_http_client_write(client, part1, p1) != p1 ||
            esp_http_client_write(client, part2, p2) != p2) {
            write_ok = false;
        }

        FILE *f = NULL;
        if (write_ok) {
            f = fopen(path, "rb");
            if (!f) {
                write_ok = false;
            }
        }

        if (write_ok && f) {
            char buf[2048];
            size_t n = 0;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                int w = esp_http_client_write(client, buf, (int)n);
                if (w < 0 || w != (int)n) {
                    write_ok = false;
                    break;
                }
            }
            fclose(f);
        }

        if (write_ok) {
            if (esp_http_client_write(client, closing, p3) != p3) {
                write_ok = false;
            }
        }

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        http_resp_t resp = {
            .buf = calloc(1, 512),
            .len = 0,
            .cap = 512,
        };
        if (resp.buf) {
            char tmp[256];
            int r = 0;
            while ((r = esp_http_client_read(client, tmp, sizeof(tmp))) > 0) {
                if (resp.len + r + 1 > resp.cap) {
                    size_t new_cap = resp.cap * 2;
                    char *nb = realloc(resp.buf, new_cap);
                    if (!nb) break;
                    resp.buf = nb;
                    resp.cap = new_cap;
                }
                memcpy(resp.buf + resp.len, tmp, r);
                resp.len += r;
                resp.buf[resp.len] = '\0';
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        net_mutex_unlock();

        if (!write_ok) {
            free(resp.buf);
            if (attempt + 1 >= MIMI_DISCORD_HTTP_MAX_RETRY) {
                return ESP_FAIL;
            }
            backoff_delay_ms(&backoff_ms);
            attempt++;
            continue;
        }

        if (status == 429) {
            int retry_ms = parse_retry_after_ms(resp.buf);
            if (retry_ms < 0) retry_ms = backoff_ms;
            free(resp.buf);
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            attempt++;
            continue;
        }
        if (status >= 500) {
            ESP_LOGW(TAG, "Discord upload 5xx (%d), backing off %d ms", status, backoff_ms);
            free(resp.buf);
            backoff_delay_ms(&backoff_ms);
            attempt++;
            continue;
        }
        if (status < 200 || status >= 300) {
            ESP_LOGE(TAG, "Discord upload failed: status=%d body=%.120s",
                     status, resp.buf ? resp.buf : "(null)");
            free(resp.buf);
            return ESP_FAIL;
        }

        free(resp.buf);
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t discord_send_typing(const char *channel_id)
{
    static int64_t s_last_typing_us = 0;

    if (!s_bot_token[0]) {
        ESP_LOGW(TAG, "Cannot send typing: no discord bot token");
        return ESP_ERR_INVALID_STATE;
    }
    if (!channel_id || !channel_id[0]) return ESP_ERR_INVALID_ARG;

    int64_t now = esp_timer_get_time();
    if (s_last_typing_us > 0 &&
        (now - s_last_typing_us) < (int64_t)MIMI_DISCORD_TYPING_COOLDOWN_MS * 1000) {
        return ESP_OK;
    }
    s_last_typing_us = now;

    char path[256];
    snprintf(path, sizeof(path), "/channels/%s/typing", channel_id);

    int status = 0;
    char *resp = discord_http_request("POST", path, NULL, &status);
    bool ok = (resp && status >= 200 && status < 300);
    if (!ok) {
        ESP_LOGW(TAG, "Discord typing failed: status=%d body=%.120s",
                 status, resp ? resp : "(null)");
    }
    free(resp);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t discord_set_token(const char *token)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_DISCORD, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_DISCORD_TOKEN, token));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_bot_token, token, sizeof(s_bot_token) - 1);
    s_bot_token[sizeof(s_bot_token) - 1] = '\0';
    ESP_LOGI(TAG, "Discord bot token saved");
    return ESP_OK;
}

esp_err_t discord_clear_token(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_DISCORD, NVS_READWRITE, &nvs));
    nvs_erase_key(nvs, MIMI_NVS_KEY_DISCORD_TOKEN);
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    s_bot_token[0] = '\0';
    s_bot_user_id[0] = '\0';
    ESP_LOGI(TAG, "Discord bot token cleared");
    return ESP_OK;
}

esp_err_t discord_add_channel(const char *channel_id)
{
    if (!channel_id || !channel_id[0]) return ESP_ERR_INVALID_ARG;

    int existing = -1;
    if (channel_exists(channel_id, &existing)) {
        return ESP_OK;
    }

    int target = -1;
    for (int i = 0; i < MIMI_DISCORD_MAX_CHANNELS; i++) {
        if (!s_channels[i][0]) {
            target = i;
            break;
        }
    }
    if (target < 0) return ESP_ERR_NO_MEM;

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_DISCORD, NVS_READWRITE, &nvs));
    char key[16];
    make_key(key, sizeof(key), DISCORD_KEY_CHAN_FMT, target);
    ESP_ERROR_CHECK(nvs_set_str(nvs, key, channel_id));
    make_key(key, sizeof(key), DISCORD_KEY_LAST_FMT, target);
    ESP_ERROR_CHECK(nvs_set_i64(nvs, key, 0));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_channels[target], channel_id, sizeof(s_channels[target]) - 1);
    s_channels[target][sizeof(s_channels[target]) - 1] = '\0';
    s_last_seen[target] = 0;
    s_last_saved[target] = 0;
    ESP_LOGI(TAG, "Discord channel added: %s (slot=%d)", channel_id, target);
    return ESP_OK;
}

esp_err_t discord_remove_channel(const char *channel_id)
{
    int idx = -1;
    if (!channel_exists(channel_id, &idx)) {
        return ESP_ERR_NOT_FOUND;
    }

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_DISCORD, NVS_READWRITE, &nvs));
    char key[16];
    make_key(key, sizeof(key), DISCORD_KEY_CHAN_FMT, idx);
    nvs_erase_key(nvs, key);
    make_key(key, sizeof(key), DISCORD_KEY_LAST_FMT, idx);
    nvs_erase_key(nvs, key);
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    s_channels[idx][0] = '\0';
    s_last_seen[idx] = 0;
    s_last_saved[idx] = 0;
    ESP_LOGI(TAG, "Discord channel removed: %s", channel_id);
    return ESP_OK;
}

esp_err_t discord_clear_channels(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_DISCORD, NVS_READWRITE, &nvs));
    for (int i = 0; i < MIMI_DISCORD_MAX_CHANNELS; i++) {
        char key[16];
        make_key(key, sizeof(key), DISCORD_KEY_CHAN_FMT, i);
        nvs_erase_key(nvs, key);
        make_key(key, sizeof(key), DISCORD_KEY_LAST_FMT, i);
        nvs_erase_key(nvs, key);
        s_channels[i][0] = '\0';
        s_last_seen[i] = 0;
        s_last_saved[i] = 0;
    }
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    ESP_LOGI(TAG, "Discord channels cleared");
    return ESP_OK;
}

esp_err_t discord_get_channels(discord_channel_t *out, size_t max, size_t *out_count)
{
    if (!out || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;
    for (int i = 0; i < MIMI_DISCORD_MAX_CHANNELS && *out_count < max; i++) {
        if (s_channels[i][0]) {
            strncpy(out[*out_count].id, s_channels[i], sizeof(out[*out_count].id) - 1);
            out[*out_count].id[sizeof(out[*out_count].id) - 1] = '\0';
            out[*out_count].last_seen = s_last_seen[i];
            (*out_count)++;
        }
    }
    return ESP_OK;
}
