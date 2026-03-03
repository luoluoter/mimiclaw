#include "tools/tool_media.h"
#include "mimi_config.h"
#include "media/media_driver.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "media_tool";

#define MEDIA_MAX_IMAGE_BYTES (512 * 1024)
#define MEDIA_MAX_AUDIO_BYTES (1024 * 1024)

static char s_api_key[320] = {0};

/* ── Helpers ─────────────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    esp_err_t err;
} resp_buf_t;

static esp_err_t resp_buf_init(resp_buf_t *rb, size_t initial_cap)
{
    rb->data = calloc(1, initial_cap);
    if (!rb->data) return ESP_ERR_NO_MEM;
    rb->len = 0;
    rb->cap = initial_cap;
    rb->err = ESP_OK;
    return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len)
{
    if (rb->len + len + 1 > rb->cap) {
        size_t new_cap = rb->cap * 2;
        while (new_cap < rb->len + len + 1) new_cap *= 2;
        char *tmp = realloc(rb->data, new_cap);
        if (!tmp) {
            rb->err = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static void resp_buf_free(resp_buf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb && evt->data_len > 0) {
        if (resp_buf_append(rb, (const char *)evt->data, evt->data_len) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static size_t append_line(char *out, size_t out_size, size_t off, const char *fmt, ...)
{
    if (off >= out_size) return off;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + off, out_size - off, fmt, args);
    va_end(args);
    if (n < 0) return off;
    size_t added = (size_t)n;
    if (off + added >= out_size) {
        out[out_size - 1] = '\0';
        return out_size;
    }
    return off + added;
}

static const char *json_get_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

static int json_get_int(cJSON *root, const char *key, int def)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsNumber(item)) return item->valueint;
    return def;
}

static esp_err_t read_file(const char *path, uint8_t **out_buf, size_t *out_len, size_t max_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0 || (size_t)size > max_len) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = calloc(1, (size_t)size);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }
    *out_buf = buf;
    *out_len = (size_t)size;
    return ESP_OK;
}

static esp_err_t http_post_json(const char *url, const char *host, const char *path,
                                const char *payload, char **out_body, int *out_status)
{
    if (http_proxy_is_enabled()) {
        proxy_conn_t *conn = proxy_conn_open(host, 443, 30000);
        if (!conn) return ESP_ERR_HTTP_CONNECT;

        int body_len = strlen(payload);
        char header[1024];
        int hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            path, host, s_api_key, body_len);

        if (proxy_conn_write(conn, header, hlen) < 0 ||
            proxy_conn_write(conn, payload, body_len) < 0) {
            proxy_conn_close(conn);
            return ESP_ERR_HTTP_WRITE_DATA;
        }

        size_t cap = 16 * 1024;
        size_t len = 0;
        char *buf = calloc(1, cap);
        if (!buf) {
            proxy_conn_close(conn);
            return ESP_ERR_NO_MEM;
        }
        char tmp[2048];
        while (1) {
            int n = proxy_conn_read(conn, tmp, sizeof(tmp), 30000);
            if (n <= 0) break;
            if (len + n >= cap) {
                size_t new_cap = cap * 2;
                char *nb = realloc(buf, new_cap);
                if (!nb) break;
                buf = nb;
                cap = new_cap;
            }
            memcpy(buf + len, tmp, n);
            len += n;
            buf[len] = '\0';
        }
        proxy_conn_close(conn);

        *out_status = 0;
        if (len > 5 && strncmp(buf, "HTTP/", 5) == 0) {
            const char *sp = strchr(buf, ' ');
            if (sp) *out_status = atoi(sp + 1);
        }
        char *body = strstr(buf, "\r\n\r\n");
        if (body) {
            body += 4;
            size_t blen = len - (body - buf);
            memmove(buf, body, blen);
            buf[blen] = '\0';
        }
        *out_body = buf;
        return ESP_OK;
    }

    resp_buf_t rb;
    if (resp_buf_init(&rb, 16 * 1024) != ESP_OK) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &rb,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (s_api_key[0]) {
        char auth[360];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (rb.err != ESP_OK && err == ESP_OK) {
        err = rb.err;
    }
    if (out_status) *out_status = status;
    if (out_body) {
        *out_body = rb.data;
    } else {
        resp_buf_free(&rb);
    }
    return err;
}

static esp_err_t http_post_multipart(const char *url, const char *host, const char *path,
                                     const char *content_type,
                                     const uint8_t *body, size_t body_len,
                                     char **out_body, int *out_status)
{
    if (http_proxy_is_enabled()) {
        proxy_conn_t *conn = proxy_conn_open(host, 443, 30000);
        if (!conn) return ESP_ERR_HTTP_CONNECT;
        char header[1024];
        int hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n\r\n",
            path, host, content_type, s_api_key, (unsigned)body_len);
        if (proxy_conn_write(conn, header, hlen) < 0 ||
            proxy_conn_write(conn, (const char *)body, (int)body_len) < 0) {
            proxy_conn_close(conn);
            return ESP_ERR_HTTP_WRITE_DATA;
        }
        size_t cap = 16 * 1024;
        size_t len = 0;
        char *buf = calloc(1, cap);
        if (!buf) {
            proxy_conn_close(conn);
            return ESP_ERR_NO_MEM;
        }
        char tmp[2048];
        while (1) {
            int n = proxy_conn_read(conn, tmp, sizeof(tmp), 30000);
            if (n <= 0) break;
            if (len + n >= cap) {
                size_t new_cap = cap * 2;
                char *nb = realloc(buf, new_cap);
                if (!nb) break;
                buf = nb;
                cap = new_cap;
            }
            memcpy(buf + len, tmp, n);
            len += n;
            buf[len] = '\0';
        }
        proxy_conn_close(conn);
        *out_status = 0;
        if (len > 5 && strncmp(buf, "HTTP/", 5) == 0) {
            const char *sp = strchr(buf, ' ');
            if (sp) *out_status = atoi(sp + 1);
        }
        char *body_ptr = strstr(buf, "\r\n\r\n");
        if (body_ptr) {
            body_ptr += 4;
            size_t blen = len - (body_ptr - buf);
            memmove(buf, body_ptr, blen);
            buf[blen] = '\0';
        }
        *out_body = buf;
        return ESP_OK;
    }

    resp_buf_t rb;
    if (resp_buf_init(&rb, 16 * 1024) != ESP_OK) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &rb,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        resp_buf_free(&rb);
        return ESP_FAIL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (s_api_key[0]) {
        char auth[360];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, (const char *)body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (rb.err != ESP_OK && err == ESP_OK) {
        err = rb.err;
    }
    if (out_status) *out_status = status;
    if (out_body) {
        *out_body = rb.data;
    } else {
        resp_buf_free(&rb);
    }
    return err;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_media_init(void)
{
    if (MIMI_SECRET_API_KEY[0] != '\0') {
        strncpy(s_api_key, MIMI_SECRET_API_KEY, sizeof(s_api_key) - 1);
    }
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[sizeof(s_api_key)] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_api_key, tmp, sizeof(s_api_key) - 1);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0]) {
        ESP_LOGI(TAG, "Media tools initialized (API key configured)");
    } else {
        ESP_LOGW(TAG, "No API key. Media tools will fail.");
    }
    return ESP_OK;
}

/* ── Tools ───────────────────────────────────────────────────── */

esp_err_t tool_camera_capture_execute(const char *input_json, char *output, size_t output_size)
{
    const char *path = MIMI_SPIFFS_BASE "/media/cam.jpg";
    cJSON *root = input_json ? cJSON_Parse(input_json) : NULL;
    if (root) {
        const char *p = json_get_string(root, "path");
        if (p && p[0]) path = p;
    }

    char out_path[128] = {0};
    esp_err_t err = media_camera_capture(path, out_path, sizeof(out_path));
    if (err == ESP_ERR_NOT_SUPPORTED) {
        snprintf(output, output_size, "camera capture not supported on this build");
    } else if (err == ESP_OK) {
        snprintf(output, output_size, "{\"path\":\"%s\"}", out_path[0] ? out_path : path);
    } else {
        snprintf(output, output_size, "camera capture failed: %s", esp_err_to_name(err));
    }
    if (root) cJSON_Delete(root);
    return err;
}

esp_err_t tool_audio_record_execute(const char *input_json, char *output, size_t output_size)
{
    const char *path = MIMI_SPIFFS_BASE "/media/audio.wav";
    int duration_ms = 3000;
    cJSON *root = input_json ? cJSON_Parse(input_json) : NULL;
    if (root) {
        const char *p = json_get_string(root, "path");
        if (p && p[0]) path = p;
        duration_ms = json_get_int(root, "duration_ms", duration_ms);
    }

    char out_path[128] = {0};
    esp_err_t err = media_audio_record(path, duration_ms, out_path, sizeof(out_path));
    if (err == ESP_ERR_NOT_SUPPORTED) {
        snprintf(output, output_size, "audio record not supported on this build");
    } else if (err == ESP_OK) {
        snprintf(output, output_size, "{\"path\":\"%s\",\"duration_ms\":%d}",
                 out_path[0] ? out_path : path, duration_ms);
    } else {
        snprintf(output, output_size, "audio record failed: %s", esp_err_to_name(err));
    }
    if (root) cJSON_Delete(root);
    return err;
}

esp_err_t tool_vision_analyze_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_api_key[0] == '\0') {
        snprintf(output, output_size, "no API key configured");
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = input_json ? cJSON_Parse(input_json) : NULL;
    const char *path = root ? json_get_string(root, "path") : NULL;
    const char *url = root ? json_get_string(root, "url") : NULL;
    const char *prompt = root ? json_get_string(root, "prompt") : NULL;
    const char *model = root ? json_get_string(root, "model") : NULL;
    if (!prompt || prompt[0] == '\0') prompt = "Describe the image.";
    if (!model || model[0] == '\0') model = MIMI_ZHIPU_VISION_MODEL;

    char *data_url = NULL;
    if (!url || url[0] == '\0') {
        if (!path || path[0] == '\0') {
            snprintf(output, output_size, "missing 'path' or 'url'");
            if (root) cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t *buf = NULL;
        size_t len = 0;
        esp_err_t r = read_file(path, &buf, &len, MEDIA_MAX_IMAGE_BYTES);
        if (r != ESP_OK) {
            snprintf(output, output_size, "failed to read image: %s", esp_err_to_name(r));
            if (root) cJSON_Delete(root);
            return r;
        }
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, buf, len);
        char *b64 = calloc(1, b64_len + 1);
        if (!b64) {
            free(buf);
            if (root) cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        if (mbedtls_base64_encode((unsigned char *)b64, b64_len + 1, &b64_len, buf, len) != 0) {
            free(buf);
            free(b64);
            if (root) cJSON_Delete(root);
            return ESP_FAIL;
        }
        free(buf);
        size_t url_len = strlen("data:image/jpeg;base64,") + b64_len + 1;
        data_url = calloc(1, url_len);
        if (!data_url) {
            free(b64);
            if (root) cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        snprintf(data_url, url_len, "data:image/jpeg;base64,%s", b64);
        free(b64);
        url = data_url;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model);
    cJSON *msgs = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON *content = cJSON_CreateArray();
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", prompt);
    cJSON_AddItemToArray(content, text);
    cJSON *img = cJSON_CreateObject();
    cJSON_AddStringToObject(img, "type", "image_url");
    cJSON *img_url = cJSON_CreateObject();
    cJSON_AddStringToObject(img_url, "url", url);
    cJSON_AddItemToObject(img, "image_url", img_url);
    cJSON_AddItemToArray(content, img);
    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(msgs, msg);
    cJSON_AddItemToObject(body, "messages", msgs);

    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    int status = 0;
    char *resp = NULL;
    esp_err_t err = http_post_json(MIMI_ZHIPU_API_URL, MIMI_ZHIPU_API_HOST, MIMI_ZHIPU_API_PATH,
                                   payload, &resp, &status);
    free(payload);
    if (data_url) free(data_url);
    if (root) cJSON_Delete(root);

    if (err != ESP_OK) {
        snprintf(output, output_size, "vision request failed: %s", esp_err_to_name(err));
        free(resp);
        return err;
    }
    if (status != 200) {
        snprintf(output, output_size, "vision API error %d: %.200s", status, resp ? resp : "");
        free(resp);
        return ESP_FAIL;
    }
    if (!resp || resp[0] == '\0') {
        snprintf(output, output_size, "vision API empty response (status %d)", status);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *j = cJSON_Parse(resp);
    if (!j) {
        snprintf(output, output_size, "vision API returned invalid JSON (len=%u): %.200s",
                 (unsigned)strlen(resp), resp);
        free(resp);
        return ESP_FAIL;
    }
    cJSON *choices = cJSON_GetObjectItem(j, "choices");
    cJSON *choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    cJSON *content_node = message ? cJSON_GetObjectItem(message, "content") : NULL;
    if (content_node && cJSON_IsString(content_node)) {
        snprintf(output, output_size, "%s", content_node->valuestring);
    } else {
        snprintf(output, output_size, "%s", resp);
    }
    cJSON_Delete(j);
    free(resp);
    return ESP_OK;
}

esp_err_t tool_audio_transcribe_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_api_key[0] == '\0') {
        snprintf(output, output_size, "no API key configured");
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = input_json ? cJSON_Parse(input_json) : NULL;
    const char *path = root ? json_get_string(root, "path") : NULL;
    const char *model = root ? json_get_string(root, "model") : NULL;
    if (!model || model[0] == '\0') model = MIMI_ZHIPU_ASR_MODEL;
    if (!path || path[0] == '\0') {
        if (root) cJSON_Delete(root);
        snprintf(output, output_size, "missing 'path'");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buf = NULL;
    size_t len = 0;
    esp_err_t r = read_file(path, &buf, &len, MEDIA_MAX_AUDIO_BYTES);
    if (r != ESP_OK) {
        snprintf(output, output_size, "failed to read audio: %s", esp_err_to_name(r));
        if (root) cJSON_Delete(root);
        return r;
    }

    const char *boundary = "----mimiFormBoundary9B4hSDJ8lP";
    const char *part1_tmpl =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n";
    const char *part2_tmpl =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    const char *part3_tmpl = "\r\n--%s--\r\n";

    char part1[256];
    char part2[256];
    char part3[64];
    snprintf(part1, sizeof(part1), part1_tmpl, boundary, model);
    snprintf(part2, sizeof(part2), part2_tmpl, boundary);
    snprintf(part3, sizeof(part3), part3_tmpl, boundary);

    size_t body_len = strlen(part1) + strlen(part2) + len + strlen(part3);
    uint8_t *body = calloc(1, body_len);
    if (!body) {
        free(buf);
        if (root) cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    size_t off = 0;
    memcpy(body + off, part1, strlen(part1)); off += strlen(part1);
    memcpy(body + off, part2, strlen(part2)); off += strlen(part2);
    memcpy(body + off, buf, len); off += len;
    memcpy(body + off, part3, strlen(part3)); off += strlen(part3);
    free(buf);

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    int status = 0;
    char *resp = NULL;
    esp_err_t err = http_post_multipart(MIMI_ZHIPU_ASR_URL, MIMI_ZHIPU_ASR_HOST, MIMI_ZHIPU_ASR_PATH,
                                        content_type, body, body_len, &resp, &status);
    free(body);
    if (root) cJSON_Delete(root);

    if (err != ESP_OK) {
        snprintf(output, output_size, "asr request failed: %s", esp_err_to_name(err));
        free(resp);
        return err;
    }
    if (status != 200) {
        snprintf(output, output_size, "asr API error %d: %.200s", status, resp ? resp : "");
        free(resp);
        return ESP_FAIL;
    }
    if (!resp || resp[0] == '\0') {
        snprintf(output, output_size, "asr API empty response (status %d)", status);
        free(resp);
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(resp);
    if (!j) {
        snprintf(output, output_size, "asr API returned invalid JSON (len=%u): %.200s",
                 (unsigned)strlen(resp), resp);
        free(resp);
        return ESP_FAIL;
    }
    cJSON *text = cJSON_GetObjectItem(j, "text");
    if (text && cJSON_IsString(text)) {
        snprintf(output, output_size, "%s", text->valuestring);
    } else {
        snprintf(output, output_size, "%s", resp);
    }
    cJSON_Delete(j);
    free(resp);
    return ESP_OK;
}
