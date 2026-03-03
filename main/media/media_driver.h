#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    esp_err_t (*camera_capture)(const char *path, char *out_path, size_t out_size);
    esp_err_t (*audio_record)(const char *path, int duration_ms, char *out_path, size_t out_size);
    bool (*camera_ready)(void);
    bool (*mic_ready)(void);
} media_driver_t;

/**
 * Initialize media subsystem (registers a stub driver by default).
 */
esp_err_t media_init(void);

/**
 * Register a media driver. Call from hardware-specific module init.
 */
esp_err_t media_register_driver(const media_driver_t *driver);

/**
 * Camera capture wrapper (calls driver).
 */
esp_err_t media_camera_capture(const char *path, char *out_path, size_t out_size);

/**
 * Audio record wrapper (calls driver).
 */
esp_err_t media_audio_record(const char *path, int duration_ms, char *out_path, size_t out_size);

bool media_camera_ready(void);
bool media_mic_ready(void);
