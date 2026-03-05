#include "wecom_bot.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "net/net_mutex.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "wecom";

#define WECOM_WEBHOOK_MAX_LEN 256

static char s_webhook[WECOM_WEBHOOK_MAX_LEN] = MIMI_SECRET_WECOM_WEBHOOK;

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool wecom_is_configured(void)
{
    return s_webhook[0] != '\0';
}

esp_err_t wecom_bot_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_WECOM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[WECOM_WEBHOOK_MAX_LEN] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_WECOM_WEBHOOK, tmp, &len) == ESP_OK && tmp[0]) {
            safe_copy(s_webhook, sizeof(s_webhook), tmp);
        }
        nvs_close(nvs);
    }

    if (s_webhook[0]) {
        ESP_LOGI(TAG, "WeCom webhook configured");
    } else {
        ESP_LOGW(TAG, "No WeCom webhook. Use CLI: set_wecom_webhook <URL>");
    }
    return ESP_OK;
}

esp_err_t wecom_set_webhook(const char *url)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_WECOM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_WECOM_WEBHOOK, url));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_webhook, sizeof(s_webhook), url);
    ESP_LOGI(TAG, "WeCom webhook saved");
    return ESP_OK;
}

esp_err_t wecom_send_message(const char *text)
{
    if (!s_webhook[0]) {
        ESP_LOGW(TAG, "Cannot send: no WeCom webhook configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!text) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "msgtype", "text");
    cJSON *text_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(text_obj, "content", text);
    cJSON_AddItemToObject(root, "text", text_obj);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return ESP_ERR_NO_MEM;

    esp_err_t lock_err = net_mutex_lock(pdMS_TO_TICKS(MIMI_NET_MUTEX_TIMEOUT_MS));
    if (lock_err != ESP_OK) {
        ESP_LOGW(TAG, "WeCom HTTP lock failed: %s", esp_err_to_name(lock_err));
        free(payload);
        return lock_err;
    }
    esp_http_client_config_t config = {
        .url = s_webhook,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        net_mutex_unlock();
        free(payload);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    net_mutex_unlock();
    free(payload);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "WeCom send failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    ESP_LOGI(TAG, "WeCom message sent");
    return ESP_OK;
}
