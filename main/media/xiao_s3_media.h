#pragma once

#include "esp_err.h"

/**
 * Initialize XIAO ESP32S3 camera/mic driver and register with media subsystem.
 */
esp_err_t media_xiao_s3_init(void);
