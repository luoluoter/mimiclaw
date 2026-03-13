#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t bridge_client_init(void);
esp_err_t bridge_client_start(void);

bool bridge_client_is_enabled(void);

esp_err_t bridge_set_url(const char *url);
esp_err_t bridge_set_device_id(const char *device_id);
esp_err_t bridge_set_device_token(const char *token);
esp_err_t bridge_clear_config(void);

esp_err_t bridge_send_message(const char *channel_id, const char *text);
esp_err_t bridge_send_typing(const char *channel_id);
esp_err_t bridge_send_file(const char *channel_id, const char *path, const char *caption);
