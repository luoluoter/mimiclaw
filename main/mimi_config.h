#pragma once

/* MimiClaw Global Configuration */

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("mimi_secrets.h")
#include "mimi_secrets.h"
#endif

#ifndef MIMI_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_SSID       ""
#endif
#ifndef MIMI_SECRET_WIFI_PASS
#define MIMI_SECRET_WIFI_PASS       ""
#endif
#ifndef MIMI_SECRET_TG_TOKEN
#define MIMI_SECRET_TG_TOKEN        ""
#endif
#ifndef MIMI_SECRET_DISCORD_TOKEN
#define MIMI_SECRET_DISCORD_TOKEN   ""
#endif
#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY         ""
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL           ""
#endif
#ifndef MIMI_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST      ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT      ""
#endif
#ifndef MIMI_SECRET_PROXY_TYPE
#define MIMI_SECRET_PROXY_TYPE      ""
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY      ""
#endif
#ifndef MIMI_SECRET_WECOM_WEBHOOK
#define MIMI_SECRET_WECOM_WEBHOOK   ""
#endif
#ifndef MIMI_SECRET_BRIDGE_URL
#define MIMI_SECRET_BRIDGE_URL      ""
#endif
#ifndef MIMI_SECRET_BRIDGE_DEVICE_ID
#define MIMI_SECRET_BRIDGE_DEVICE_ID ""
#endif
#ifndef MIMI_SECRET_BRIDGE_DEVICE_TOKEN
#define MIMI_SECRET_BRIDGE_DEVICE_TOKEN ""
#endif

/* WiFi */
#define MIMI_WIFI_MAX_RETRY          10
#define MIMI_WIFI_RETRY_BASE_MS      1000
#define MIMI_WIFI_RETRY_MAX_MS       30000

/* Telegram Bot */
#define MIMI_TG_POLL_TIMEOUT_S       30
#define MIMI_TG_MAX_MSG_LEN          4096
#define MIMI_TG_POLL_STACK           (12 * 1024)
#define MIMI_TG_POLL_PRIO            5
#define MIMI_TG_POLL_CORE            0
#define MIMI_TG_CARD_SHOW_MS         3000
#define MIMI_TG_CARD_BODY_SCALE      3

/* Discord (HTTP polling) */
#define MIMI_DISCORD_POLL_INTERVAL_MS   3000
#define MIMI_DISCORD_POLL_STACK         (12 * 1024)
#define MIMI_DISCORD_POLL_PRIO          5
#define MIMI_DISCORD_POLL_CORE          0
#define MIMI_DISCORD_MAX_MSG_LEN        2000
#define MIMI_DISCORD_MAX_CHANNELS       5
#define MIMI_DISCORD_SAVE_STEP          5
#define MIMI_DISCORD_SAVE_INTERVAL_US   (5LL * 1000 * 1000)
#define MIMI_DISCORD_TYPING_COOLDOWN_MS 8000

/* Bridge */
#define MIMI_BRIDGE_POLL_INTERVAL_MS    2000
#define MIMI_BRIDGE_POLL_STACK          (12 * 1024)
#define MIMI_BRIDGE_POLL_PRIO           5
#define MIMI_BRIDGE_POLL_CORE           0

/* Agent Loop */
#define MIMI_AGENT_STACK             (24 * 1024)
#define MIMI_AGENT_PRIO              6
#define MIMI_AGENT_CORE              1
#define MIMI_AGENT_MAX_HISTORY       16
#define MIMI_AGENT_MAX_TOOL_ITER     10
#define MIMI_MAX_TOOL_CALLS          4
#define MIMI_AGENT_SEND_WORKING_STATUS 1

/* Timezone (POSIX TZ format) */
#define MIMI_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"
#define MIMI_TIME_SYNC_TIMEOUT_MS    15000
#define MIMI_TIME_SYNC_MIN_VALID_TS  1672531200  /* 2023-01-01 */

/* LLM */
#define MIMI_LLM_DEFAULT_MODEL       "GLM-4-FlashX-250414"
#define MIMI_LLM_PROVIDER_DEFAULT    "zhipu"
#define MIMI_LLM_MAX_TOKENS          768
#define MIMI_VISION_MAX_TOKENS       512
#define MIMI_AGENT_REQUEST_GAP_MS    80
#define MIMI_NET_MUTEX_TIMEOUT_MS   60000
#define MIMI_NET_MIN_INTERVAL_MS     120
#define MIMI_HTTP_RETRY_BASE_MS      500
#define MIMI_HTTP_RETRY_MAX_MS       8000
#define MIMI_DISCORD_HTTP_MAX_RETRY  3
#define MIMI_LLM_HTTP_MAX_RETRY      3
#define MIMI_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define MIMI_OPENAI_API_URL          "https://api.openai.com/v1/chat/completions"
#define MIMI_ZHIPU_CODING_API_URL    "https://open.bigmodel.cn/api/coding/paas/v4/chat/completions"
#define MIMI_ZHIPU_CODING_API_HOST   "open.bigmodel.cn"
#define MIMI_ZHIPU_CODING_API_PATH   "/api/coding/paas/v4/chat/completions"
#define MIMI_ZHIPU_API_URL           "https://open.bigmodel.cn/api/paas/v4/chat/completions"
#define MIMI_ZHIPU_API_HOST          "open.bigmodel.cn"
#define MIMI_ZHIPU_API_PATH          "/api/paas/v4/chat/completions"
#define MIMI_ZHIPU_ASR_URL           "https://open.bigmodel.cn/api/paas/v4/audio/transcriptions"
#define MIMI_ZHIPU_ASR_HOST          "open.bigmodel.cn"
#define MIMI_ZHIPU_ASR_PATH          "/api/paas/v4/audio/transcriptions"
#define MIMI_ZHIPU_ASR_MODEL         "glm-asr-2512"
#define MIMI_ZHIPU_VISION_MODEL      "glm-4.6v"
#define MIMI_LLM_API_VERSION         "2023-06-01"
#define MIMI_LLM_STREAM_BUF_SIZE     (32 * 1024)
#define MIMI_LLM_LOG_VERBOSE_PAYLOAD 0
#define MIMI_LLM_LOG_PREVIEW_BYTES   96

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN           16
#define MIMI_OUTBOUND_STACK          (12 * 1024)
#define MIMI_OUTBOUND_PRIO           5
#define MIMI_OUTBOUND_CORE           0

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE             "/spiffs"
#define MIMI_SPIFFS_CONFIG_DIR       MIMI_SPIFFS_BASE "/config"
#define MIMI_SPIFFS_MEMORY_DIR       MIMI_SPIFFS_BASE "/memory"
#define MIMI_SPIFFS_SESSION_DIR      MIMI_SPIFFS_BASE "/sessions"
#define MIMI_MEMORY_FILE             MIMI_SPIFFS_MEMORY_DIR "/MEMORY.md"
#define MIMI_SOUL_FILE               MIMI_SPIFFS_CONFIG_DIR "/SOUL.md"
#define MIMI_USER_FILE               MIMI_SPIFFS_CONFIG_DIR "/USER.md"
#define MIMI_CONTEXT_BUF_SIZE        (16 * 1024)
#define MIMI_SESSION_MAX_MSGS        20

/* Cron / Heartbeat */
#define MIMI_CRON_FILE               MIMI_SPIFFS_BASE "/cron.json"
#define MIMI_CRON_MAX_JOBS           16
#define MIMI_CRON_CHECK_INTERVAL_MS  (60 * 1000)
#define MIMI_HEARTBEAT_FILE          MIMI_SPIFFS_BASE "/HEARTBEAT.md"
#define MIMI_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* Skills */
#define MIMI_SKILLS_PREFIX           MIMI_SPIFFS_BASE "/skills/"

/* WebSocket Gateway */
#define MIMI_WS_PORT                 18789
#define MIMI_WS_MAX_CLIENTS          4

/* Serial CLI */
#define MIMI_CLI_STACK               (4 * 1024)
#define MIMI_CLI_PRIO                3
#define MIMI_CLI_CORE                0

/* NVS Namespaces */
#define MIMI_NVS_WIFI                "wifi_config"
#define MIMI_NVS_TG                  "tg_config"
#define MIMI_NVS_DISCORD             "discord_config"
#define MIMI_NVS_WECOM               "wecom_config"
#define MIMI_NVS_LLM                 "llm_config"
#define MIMI_NVS_PROXY               "proxy_config"
#define MIMI_NVS_SEARCH              "search_config"
#define MIMI_NVS_BRIDGE              "bridge_config"

/* NVS Keys */
#define MIMI_NVS_KEY_SSID            "ssid"
#define MIMI_NVS_KEY_PASS            "password"
#define MIMI_NVS_KEY_TG_TOKEN        "bot_token"
#define MIMI_NVS_KEY_DISCORD_TOKEN   "bot_token"
#define MIMI_NVS_KEY_WECOM_WEBHOOK   "webhook"
#define MIMI_NVS_KEY_API_KEY         "api_key"
#define MIMI_NVS_KEY_MODEL           "model"
#define MIMI_NVS_KEY_PROVIDER        "provider"
#define MIMI_NVS_KEY_PROXY_HOST      "host"
#define MIMI_NVS_KEY_PROXY_PORT      "port"
#define MIMI_NVS_KEY_BRIDGE_URL      "url"
#define MIMI_NVS_KEY_BRIDGE_DEVICE_ID "device_id"
#define MIMI_NVS_KEY_BRIDGE_DEVICE_TOKEN "device_tok"
