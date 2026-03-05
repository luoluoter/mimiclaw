#include "net/net_mutex.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "net_mutex";
static SemaphoreHandle_t s_net_mutex = NULL;
static net_mutex_stats_t s_stats = {0};
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t net_mutex_init(void)
{
    if (s_net_mutex) return ESP_OK;
    s_net_mutex = xSemaphoreCreateMutex();
    if (!s_net_mutex) {
        ESP_LOGE(TAG, "Failed to create net mutex");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Net mutex initialized");
    return ESP_OK;
}

esp_err_t net_mutex_lock(TickType_t ticks_to_wait)
{
    if (!s_net_mutex) {
        esp_err_t err = net_mutex_init();
        if (err != ESP_OK) return err;
    }
    int64_t start = esp_timer_get_time();
    if (xSemaphoreTake(s_net_mutex, ticks_to_wait) != pdTRUE) {
        portENTER_CRITICAL(&s_stats_mux);
        s_stats.timeout_count++;
        portEXIT_CRITICAL(&s_stats_mux);
        return ESP_ERR_TIMEOUT;
    }
    int64_t end = esp_timer_get_time();
    uint32_t wait_ms = (uint32_t)((end - start) / 1000);
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.lock_count++;
    s_stats.last_wait_ms = wait_ms;
    s_stats.total_wait_ms += wait_ms;
    if (wait_ms > s_stats.max_wait_ms) s_stats.max_wait_ms = wait_ms;
    portEXIT_CRITICAL(&s_stats_mux);

    if (wait_ms >= 200) {
        ESP_LOGW(TAG, "Net mutex wait %u ms", (unsigned)wait_ms);
    }
    return ESP_OK;
}

void net_mutex_unlock(void)
{
    if (s_net_mutex) {
        xSemaphoreGive(s_net_mutex);
    }
}

void net_mutex_get_stats(net_mutex_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}

void net_mutex_dump_stats(void)
{
    net_mutex_stats_t s;
    net_mutex_get_stats(&s);
    uint32_t avg = (s.lock_count > 0)
                       ? (uint32_t)(s.total_wait_ms / s.lock_count)
                       : 0;
    ESP_LOGI(TAG, "Net mutex stats: locks=%u timeouts=%u avg_wait_ms=%u max_wait_ms=%u last_wait_ms=%u",
             (unsigned)s.lock_count, (unsigned)s.timeout_count,
             (unsigned)avg, (unsigned)s.max_wait_ms, (unsigned)s.last_wait_ms);
}
