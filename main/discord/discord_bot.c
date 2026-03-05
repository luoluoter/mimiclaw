#include "discord_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
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

    int attempt = 0;
    int backoff_ms = 500;

    while (attempt < 3) {
        esp_err_t lock_err = net_mutex_lock(pdMS_TO_TICKS(MIMI_NET_MUTEX_TIMEOUT_MS));
        if (lock_err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP lock failed: %s", esp_err_to_name(lock_err));
            return NULL;
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
            .timeout_ms = 30000,
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
        esp_http_client_cleanup(client);
        net_mutex_unlock();

        if (out_status) *out_status = status;

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
            free(resp.buf);
            return NULL;
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
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2;
            if (backoff_ms > 30000) backoff_ms = 30000;
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

esp_err_t discord_send_typing(const char *channel_id)
{
    if (!s_bot_token[0]) {
        ESP_LOGW(TAG, "Cannot send typing: no discord bot token");
        return ESP_ERR_INVALID_STATE;
    }
    if (!channel_id || !channel_id[0]) return ESP_ERR_INVALID_ARG;

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
