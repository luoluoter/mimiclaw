#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "discord/discord_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "bridge/bridge_client.h"
#include "net/net_mutex.h"
#include "wecom/wecom_bot.h"
#include "media/media_driver.h"
#include "tools/tool_registry.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "buttons/button_driver.h"
#include "imu/imu_manager.h"
#include "skills/skill_loader.h"

static const char *TAG = "mimi";

static bool time_is_sane(void)
{
    time_t now = time(NULL);
    return now >= (time_t)MIMI_TIME_SYNC_MIN_VALID_TS;
}

static void time_sync_start_sntp(void)
{
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    esp_sntp_init();
}

static esp_err_t time_sync_wait(uint32_t timeout_ms)
{
    if (time_is_sane()) return ESP_OK;

    ESP_LOGI(TAG, "Syncing time via SNTP...");
    time_sync_start_sntp();

    int64_t start = esp_timer_get_time();
    while (!time_is_sane()) {
        if ((esp_timer_get_time() - start) / 1000 > timeout_ms) {
            ESP_LOGW(TAG, "Time sync timed out");
            esp_sntp_stop();
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_sntp_stop();

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
    ESP_LOGI(TAG, "Time synced: %s", buf);
    return ESP_OK;
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MIMI_SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            if (wecom_is_configured()) {
                esp_err_t wc_err = wecom_send_message(msg.content);
                if (wc_err != ESP_OK) {
                    ESP_LOGE(TAG, "WeCom send failed for %s: %s", msg.chat_id, esp_err_to_name(wc_err));
                }
            } else {
                esp_err_t send_err = telegram_send_message(msg.chat_id, msg.content);
                if (send_err != ESP_OK) {
                    ESP_LOGE(TAG, "Telegram send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
                } else {
                    ESP_LOGI(TAG, "Telegram send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
                }
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_DISCORD) == 0) {
            esp_err_t dc_err = bridge_client_is_enabled()
                ? bridge_send_message(msg.chat_id, msg.content)
                : discord_send_message(msg.chat_id, msg.content);
            if (dc_err != ESP_OK) {
                ESP_LOGE(TAG, "Discord send failed for %s: %s", msg.chat_id, esp_err_to_name(dc_err));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            esp_err_t ws_err = ws_server_send(msg.chat_id, msg.content);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_WECOM) == 0) {
            esp_err_t wc_err = wecom_send_message(msg.content);
            if (wc_err != ESP_OK) {
                ESP_LOGE(TAG, "WeCom send failed: %s", esp_err_to_name(wc_err));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_CLI) == 0) {
            printf("AI> %s\n", msg.content);
            if (wecom_is_configured()) {
                esp_err_t wc_err = wecom_send_message(msg.content);
                if (wc_err != ESP_OK) {
                    ESP_LOGE(TAG, "WeCom send failed for CLI: %s", esp_err_to_name(wc_err));
                }
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

void app_main(void)
{
    /* Reduce noisy IMU logs during CLI input */
    esp_log_level_set("imu", ESP_LOG_WARN);
    /* Silence noisy components */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MimiClaw - ESP32-S3 AI Agent");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info */
    ESP_LOGI(TAG, "Internal free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Input */
    button_Init();

    /* Timezone for logs + localtime */
    setenv("TZ", MIMI_TIMEZONE, 1);
    tzset();

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());
    ESP_ERROR_CHECK(net_mutex_init());

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(media_init());
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(discord_bot_init());
    ESP_ERROR_CHECK(bridge_client_init());
    ESP_ERROR_CHECK(wecom_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi */
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
            time_sync_wait(MIMI_TIME_SYNC_TIMEOUT_MS);

            /* Outbound dispatch task should start first to avoid dropping early replies. */
            ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
                outbound_dispatch_task, "outbound",
                MIMI_OUTBOUND_STACK, NULL,
                MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE) == pdPASS)
                ? ESP_OK : ESP_FAIL);

            /* Start network-dependent services */
            ESP_ERROR_CHECK(agent_loop_start());
            ESP_ERROR_CHECK(telegram_bot_start());
            if (bridge_client_is_enabled()) {
                ESP_ERROR_CHECK(bridge_client_start());
            } else {
                ESP_ERROR_CHECK(discord_bot_start());
            }
            cron_service_start();
            heartbeat_start();
            ESP_ERROR_CHECK(ws_server_start());

            ESP_LOGI(TAG, "All services started!");
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Check MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
    }

    ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}
