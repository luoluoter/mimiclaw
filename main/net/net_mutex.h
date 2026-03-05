#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    uint32_t lock_count;
    uint32_t timeout_count;
    uint32_t last_wait_ms;
    uint32_t max_wait_ms;
    uint64_t total_wait_ms;
} net_mutex_stats_t;

esp_err_t net_mutex_init(void);
esp_err_t net_mutex_lock(TickType_t ticks_to_wait);
void net_mutex_unlock(void);
void net_mutex_get_stats(net_mutex_stats_t *out);
void net_mutex_dump_stats(void);
