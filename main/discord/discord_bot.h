#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    char id[32];
    uint64_t last_seen;
} discord_channel_t;

esp_err_t discord_bot_init(void);
esp_err_t discord_bot_start(void);
esp_err_t discord_send_message(const char *channel_id, const char *text);
esp_err_t discord_send_file(const char *channel_id, const char *path, const char *caption);
esp_err_t discord_send_typing(const char *channel_id);
esp_err_t discord_set_token(const char *token);
esp_err_t discord_clear_token(void);
esp_err_t discord_add_channel(const char *channel_id);
esp_err_t discord_remove_channel(const char *channel_id);
esp_err_t discord_clear_channels(void);
esp_err_t discord_get_channels(discord_channel_t *out, size_t max, size_t *out_count);
bool discord_is_configured(void);
