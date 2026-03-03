#pragma once

#include "esp_err.h"

esp_err_t tool_media_init(void);

esp_err_t tool_camera_capture_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_audio_record_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_vision_analyze_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_audio_transcribe_execute(const char *input_json, char *output, size_t output_size);
