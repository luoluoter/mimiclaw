#include "bridge/bridge_client.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "discord/discord_bot.h"
#include "bus/message_bus.h"
#include "net/net_mutex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "bridge";

static char s_bridge_url[192] = MIMI_SECRET_BRIDGE_URL;
static char s_device_id[64] = MIMI_SECRET_BRIDGE_DEVICE_ID;
static char s_device_token[128] = MIMI_SECRET_BRIDGE_DEVICE_TOKEN;
static bool s_started = false;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool has_discord_channels(void)
{
    discord_channel_t channels[MIMI_DISCORD_MAX_CHANNELS] = {0};
    size_t count = 0;
    if (discord_get_channels(channels, MIMI_DISCORD_MAX_CHANNELS, &count) != ESP_OK) {
        return false;
    }
    return count > 0;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (!resp || evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }

    if (resp->len + evt->data_len + 1 > resp->cap) {
        size_t new_cap = resp->cap ? (resp->cap * 2) : 1024;
        size_t need = resp->len + evt->data_len + 1;
        if (new_cap < need) new_cap = need;
        char *tmp = realloc(resp->buf, new_cap);
        if (!tmp) return ESP_ERR_NO_MEM;
        resp->buf = tmp;
        resp->cap = new_cap;
    }

    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += evt->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static esp_err_t build_url(char *out, size_t out_size, const char *path)
{
    if (!out || !out_size || !path || !path[0]) return ESP_ERR_INVALID_ARG;
    if (!s_bridge_url[0]) return ESP_ERR_INVALID_STATE;

    size_t base_len = strlen(s_bridge_url);
    bool trim_base = (base_len > 0 && s_bridge_url[base_len - 1] == '/');
    bool trim_path = (path[0] == '/');
    int written = 0;
    if (trim_base && trim_path) {
        written = snprintf(out, out_size, "%.*s%s",
                           (int)(base_len - 1), s_bridge_url, path);
    } else if (!trim_base && !trim_path) {
        written = snprintf(out, out_size, "%s/%s", s_bridge_url, path);
    } else {
        written = snprintf(out, out_size, "%s%s", s_bridge_url, path);
    }
    if (written <= 0 || written >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void set_auth_headers(esp_http_client_handle_t client)
{
    esp_http_client_set_header(client, "X-Mimi-Device-Id", s_device_id);
    esp_http_client_set_header(client, "X-Mimi-Device-Token", s_device_token);
}

static esp_err_t bridge_http_json(const char *path,
                                  const char *payload,
                                  int timeout_ms,
                                  int *out_status,
                                  char **out_body)
{
    if (out_status) *out_status = 0;
    if (out_body) *out_body = NULL;

    if (!bridge_client_is_enabled()) return ESP_ERR_INVALID_STATE;
    if (!wifi_manager_is_connected()) return ESP_ERR_INVALID_STATE;

    esp_err_t lock_err = net_mutex_lock(pdMS_TO_TICKS(MIMI_NET_MUTEX_TIMEOUT_MS));
    if (lock_err != ESP_OK) return lock_err;

    char url[256];
    esp_err_t err = build_url(url, sizeof(url), path);
    if (err != ESP_OK) {
        net_mutex_unlock();
        return err;
    }

    http_resp_t resp = {
        .buf = calloc(1, 1024),
        .len = 0,
        .cap = 1024,
    };
    if (!resp.buf) {
        net_mutex_unlock();
        return ESP_ERR_NO_MEM;
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
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    set_auth_headers(client);
    if (payload) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
    } else {
        esp_http_client_set_post_field(client, "", 0);
    }

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    net_mutex_unlock();

    if (out_status) *out_status = status;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Bridge request %s failed: %s", path, esp_err_to_name(err));
        free(resp.buf);
        return err;
    }

    if (out_body) {
        *out_body = resp.buf;
    } else {
        free(resp.buf);
    }
    return ESP_OK;
}

static const char *guess_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    return "application/octet-stream";
}

static esp_err_t bridge_upload_file(const char *path,
                                    const char *channel_id,
                                    const char *caption)
{
    if (!bridge_client_is_enabled()) return ESP_ERR_INVALID_STATE;
    if (!wifi_manager_is_connected()) return ESP_ERR_INVALID_STATE;
    if (!path || !path[0] || !channel_id || !channel_id[0]) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char url[256];
    esp_err_t err = build_url(url, sizeof(url), "/api/v1/device/file");
    if (err != ESP_OK) return err;

    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    const char *mime = guess_mime_type(path);
    const char *boundary = "----mimiBridgeBoundaryK9nA2";
    const char *safe_caption = caption ? caption : "";

    char part1[1024];
    char part2[512];
    char part3[512];
    char closing[64];
    int p1 = snprintf(part1, sizeof(part1),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"channel\"\r\n\r\n"
                      "discord\r\n"
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
                      "%s\r\n",
                      boundary, boundary, channel_id);
    int p2 = snprintf(part2, sizeof(part2),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
                      "%s\r\n",
                      boundary, safe_caption);
    int p3 = snprintf(part3, sizeof(part3),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                      "Content-Type: %s\r\n\r\n",
                      boundary, filename, mime);
    int p4 = snprintf(closing, sizeof(closing), "\r\n--%s--\r\n", boundary);

    if (p1 <= 0 || p1 >= (int)sizeof(part1) ||
        p2 <= 0 || p2 >= (int)sizeof(part2) ||
        p3 <= 0 || p3 >= (int)sizeof(part3) ||
        p4 <= 0 || p4 >= (int)sizeof(closing)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total_len = (size_t)p1 + (size_t)p2 + (size_t)p3 + (size_t)st.st_size + (size_t)p4;
    if (total_len > INT32_MAX) return ESP_ERR_INVALID_SIZE;

    esp_err_t lock_err = net_mutex_lock(pdMS_TO_TICKS(MIMI_NET_MUTEX_TIMEOUT_MS));
    if (lock_err != ESP_OK) return lock_err;

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
    set_auth_headers(client);
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    err = esp_http_client_open(client, (int)total_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        net_mutex_unlock();
        return err;
    }

    bool write_ok = true;
    if (esp_http_client_write(client, part1, p1) != p1 ||
        esp_http_client_write(client, part2, p2) != p2 ||
        esp_http_client_write(client, part3, p3) != p3) {
        write_ok = false;
    }

    FILE *f = NULL;
    if (write_ok) {
        f = fopen(path, "rb");
        if (!f) write_ok = false;
    }

    if (write_ok && f) {
        char buf[2048];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            int w = esp_http_client_write(client, buf, (int)n);
            if (w != (int)n) {
                write_ok = false;
                break;
            }
        }
        fclose(f);
    }

    if (write_ok && esp_http_client_write(client, closing, p4) != p4) {
        write_ok = false;
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
        return ESP_FAIL;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Bridge file upload failed: status=%d body=%.120s",
                 status, resp.buf ? resp.buf : "(null)");
        free(resp.buf);
        return ESP_FAIL;
    }

    free(resp.buf);
    return ESP_OK;
}

static char *build_pull_payload(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON *subs = cJSON_AddObjectToObject(root, "subscriptions");
    cJSON *channels = cJSON_AddArrayToObject(subs, "discord_channels");
    discord_channel_t dc_channels[MIMI_DISCORD_MAX_CHANNELS] = {0};
    size_t count = 0;
    if (discord_get_channels(dc_channels, MIMI_DISCORD_MAX_CHANNELS, &count) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            cJSON_AddItemToArray(channels, cJSON_CreateString(dc_channels[i].id));
        }
    }
    cJSON_AddNumberToObject(root, "max_messages", 8);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static void bridge_poll_once(void)
{
    if (!bridge_client_is_enabled() || !has_discord_channels() || !wifi_manager_is_connected()) {
        return;
    }

    char *payload = build_pull_payload();
    if (!payload) return;

    int status = 0;
    char *body = NULL;
    esp_err_t err = bridge_http_json("/api/v1/device/pull", payload, 15000, &status, &body);
    free(payload);
    if (err != ESP_OK || !body) return;

    if (status != 200) {
        ESP_LOGW(TAG, "Bridge pull failed: status=%d body=%.120s", status, body);
        free(body);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (cJSON_IsArray(messages)) {
        int count = cJSON_GetArraySize(messages);
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(messages, i);
            cJSON *channel = cJSON_GetObjectItem(item, "channel");
            cJSON *chat_id = cJSON_GetObjectItem(item, "chat_id");
            cJSON *content = cJSON_GetObjectItem(item, "content");
            if (!cJSON_IsString(channel) || !channel->valuestring ||
                !cJSON_IsString(chat_id) || !chat_id->valuestring ||
                !cJSON_IsString(content) || !content->valuestring) {
                continue;
            }

            mimi_msg_t in = {0};
            copy_str(in.channel, sizeof(in.channel), channel->valuestring);
            copy_str(in.chat_id, sizeof(in.chat_id), chat_id->valuestring);
            in.content = strdup(content->valuestring);
            if (!in.content) continue;
            if (message_bus_push_inbound(&in) != ESP_OK) {
                ESP_LOGW(TAG, "Inbound queue full, drop bridge message");
                free(in.content);
            }
        }
    }

    cJSON_Delete(root);
}

static void bridge_poll_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Bridge polling task started");
    while (1) {
        bridge_poll_once();
        vTaskDelay(pdMS_TO_TICKS(MIMI_BRIDGE_POLL_INTERVAL_MS));
    }
}

bool bridge_client_is_enabled(void)
{
    return s_bridge_url[0] != '\0' &&
           s_device_id[0] != '\0' &&
           s_device_token[0] != '\0';
}

esp_err_t bridge_client_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_BRIDGE, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[192] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_BRIDGE_URL, tmp, &len) == ESP_OK && tmp[0]) {
            copy_str(s_bridge_url, sizeof(s_bridge_url), tmp);
        }
        len = sizeof(tmp);
        memset(tmp, 0, sizeof(tmp));
        if (nvs_get_str(nvs, MIMI_NVS_KEY_BRIDGE_DEVICE_ID, tmp, &len) == ESP_OK && tmp[0]) {
            copy_str(s_device_id, sizeof(s_device_id), tmp);
        }
        len = sizeof(tmp);
        memset(tmp, 0, sizeof(tmp));
        if (nvs_get_str(nvs, MIMI_NVS_KEY_BRIDGE_DEVICE_TOKEN, tmp, &len) == ESP_OK && tmp[0]) {
            copy_str(s_device_token, sizeof(s_device_token), tmp);
        }
        nvs_close(nvs);
    }

    if (bridge_client_is_enabled()) {
        ESP_LOGI(TAG, "Bridge enabled: %s (device=%s)", s_bridge_url, s_device_id);
    } else {
        ESP_LOGI(TAG, "Bridge disabled");
    }
    return ESP_OK;
}

esp_err_t bridge_client_start(void)
{
    if (!bridge_client_is_enabled()) {
        ESP_LOGI(TAG, "Bridge not configured, polling disabled");
        return ESP_OK;
    }
    if (s_started) return ESP_OK;

    BaseType_t ret = xTaskCreatePinnedToCore(
        bridge_poll_task, "bridge_poll",
        MIMI_BRIDGE_POLL_STACK, NULL,
        MIMI_BRIDGE_POLL_PRIO, NULL, MIMI_BRIDGE_POLL_CORE);
    if (ret == pdPASS) {
        s_started = true;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t bridge_set_url(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_BRIDGE, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_BRIDGE_URL, url));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    copy_str(s_bridge_url, sizeof(s_bridge_url), url);
    return ESP_OK;
}

esp_err_t bridge_set_device_id(const char *device_id)
{
    if (!device_id || !device_id[0]) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_BRIDGE, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_BRIDGE_DEVICE_ID, device_id));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    copy_str(s_device_id, sizeof(s_device_id), device_id);
    return ESP_OK;
}

esp_err_t bridge_set_device_token(const char *token)
{
    if (!token || !token[0]) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_BRIDGE, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_BRIDGE_DEVICE_TOKEN, token));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    copy_str(s_device_token, sizeof(s_device_token), token);
    return ESP_OK;
}

esp_err_t bridge_clear_config(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_BRIDGE, NVS_READWRITE, &nvs));
    nvs_erase_key(nvs, MIMI_NVS_KEY_BRIDGE_URL);
    nvs_erase_key(nvs, MIMI_NVS_KEY_BRIDGE_DEVICE_ID);
    nvs_erase_key(nvs, MIMI_NVS_KEY_BRIDGE_DEVICE_TOKEN);
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    s_bridge_url[0] = '\0';
    s_device_id[0] = '\0';
    s_device_token[0] = '\0';
    return ESP_OK;
}

esp_err_t bridge_send_message(const char *channel_id, const char *text)
{
    if (!channel_id || !channel_id[0] || !text) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "channel", "discord");
    cJSON_AddStringToObject(root, "chat_id", channel_id);
    cJSON_AddStringToObject(root, "content", text);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return ESP_ERR_NO_MEM;

    int status = 0;
    char *body = NULL;
    esp_err_t err = bridge_http_json("/api/v1/device/message", payload, 30000, &status, &body);
    free(payload);
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Bridge send failed: status=%d body=%.120s", status, body ? body : "(null)");
        free(body);
        return ESP_FAIL;
    }
    free(body);
    return ESP_OK;
}

esp_err_t bridge_send_typing(const char *channel_id)
{
    if (!channel_id || !channel_id[0]) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "channel", "discord");
    cJSON_AddStringToObject(root, "chat_id", channel_id);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return ESP_ERR_NO_MEM;

    int status = 0;
    char *body = NULL;
    esp_err_t err = bridge_http_json("/api/v1/device/typing", payload, 15000, &status, &body);
    free(payload);
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Bridge typing failed: status=%d body=%.120s", status, body ? body : "(null)");
        free(body);
        return ESP_FAIL;
    }
    free(body);
    return ESP_OK;
}

esp_err_t bridge_send_file(const char *channel_id, const char *path, const char *caption)
{
    return bridge_upload_file(path, channel_id, caption);
}
