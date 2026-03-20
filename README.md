# MimiClaw Deployment Guide

<p align="center">
  <strong><a href="README.md">English</a> | <a href="README_CN.md">中文</a> | <a href="README_JA.md">日本語</a></strong>
</p>

MimiClaw (`dev/seeed-xiao-s3`) deployment guide for the `lzx` commit range.  
Default minimum working path: `Zhipu API + Discord`.

Applicable commit range: `8354932` ~ `46dff47`

This document only covers the changes in this commit range:
- Web UI + WebSocket + bench
- Default path: Zhipu + Discord, other channels are optional
- Optional OpenAI / Anthropic / WeCom / Telegram configuration
- XIAO ESP32S3 Sense media tools (photo / audio / recognition)

# Quick Start

## 0) Prepare the hardware

Required:
- Development board: Seeed XIAO ESP32S3
  - Buy: https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html
  - Wiki: https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- USB-C data cable (not a charge-only cable)
- 2.4GHz Wi-Fi

If you want photo/audio tools:
- XIAO ESP32S3 Sense is recommended (camera and microphone path included)
- A regular XIAO ESP32S3 may report `camera/audio not supported`

## 1) Clone the repo and switch branches

```bash
git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw
git switch dev/seeed-xiao-s3
```

## 2) Install ESP-IDF (one time)

Ubuntu:

```bash
./scripts/setup_idf_ubuntu.sh
. "$HOME/.espressif/esp-idf-v5.5.2/export.sh"
```

macOS:

```bash
./scripts/setup_idf_macos.sh
. "$HOME/.espressif/esp-idf-v5.5.2/export.sh"
```

## 3) Fill in the config (default: Zhipu + Discord)

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

Edit `main/mimi_secrets.h` with the minimum working setup:

```c
#define MIMI_SECRET_WIFI_SSID       "YOUR_WIFI"
#define MIMI_SECRET_WIFI_PASS       "YOUR_WIFI_PASSWORD"

#define MIMI_SECRET_API_KEY         "YOUR_API_KEY"
#define MIMI_SECRET_MODEL_PROVIDER  "zhipu"     // zhipu | openai | anthropic
#define MIMI_SECRET_MODEL           "GLM-4-FlashX-250414"

#define MIMI_SECRET_DISCORD_TOKEN   "YOUR_DISCORD_BOT_TOKEN"
#define MIMI_SECRET_TG_TOKEN        ""          // optional
#define MIMI_SECRET_WECOM_WEBHOOK   ""          // optional
```

API key sources:
- Zhipu: https://open.bigmodel.cn/
- OpenAI (optional): https://platform.openai.com/
- Anthropic (optional): https://console.anthropic.com/

Common pitfalls:
- `MODEL_PROVIDER` and `MODEL` must match, or requests will fail.
- If no Discord channel is configured, the bot will not reply in your target channel.
- If you only test from the CLI, Discord/Telegram can stay empty.
- In restricted networks, set a proxy from the CLI first: `set_proxy HOST PORT [http|socks5]`.
- If you use the new bridge mode, the device does not need a Discord token; configure `set_bridge_url`, `set_bridge_device`, and `set_bridge_token` instead.

## 4) Build and flash

```bash
idf.py set-target esp32s3
idf.py fullclean
idf.py build
```

Find the serial port:

```bash
# Linux
ls /dev/ttyACM* /dev/ttyUSB*

# macOS
ls /dev/cu.usb*
```

Flash and watch logs:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

If you want a single full-flash image for cloud distribution, build a merged bin:

```bash
./scripts/build_merged_bin.sh
```

Default output:

```text
build/mimiclaw-merged.bin
```

Users can then flash the full image at `0x0`:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 build/mimiclaw-merged.bin
```

Notes:
- `build/mimiclaw.bin` is the app image for OTA, not the first-flash full image.
- `build/mimiclaw-merged.bin` includes bootloader, partition table, otadata, app, and SPIFFS.
- The current project build is configured for `8MB` flash, and the merged image is generated against that size.

Common pitfalls:
- If your board has two Type-C ports, use the native USB/JTAG port for flashing.
- If you cannot enter commands in `monitor`, switch to the UART/COM port and reopen the monitor.

## 5) Override settings at runtime (no code changes, CLI only)

```text
mimi> set_wifi YOUR_WIFI YOUR_WIFI_PASSWORD
mimi> set_api_key YOUR_API_KEY
mimi> set_model_provider zhipu
mimi> set_model GLM-4-FlashX-250414
mimi> set_discord_token YOUR_DISCORD_BOT_TOKEN
mimi> discord_channel_add 123456789012345678
mimi> discord_channel_list
mimi> config_show
mimi> restart
```

Bridge mode example:

```text
mimi> set_bridge_url https://bridge.example.com
mimi> set_bridge_device mimi-001
mimi> set_bridge_token YOUR_DEVICE_SHARED_SECRET
mimi> discord_channel_add 123456789012345678
mimi> restart
```

# Examples

## A) Verify the basic path first

```text
mimi> wifi_status
mimi> chat hello
mimi> bench all
mimi> bench llm
```

## B) Verify Web UI / WebSocket

```text
http://<device_ip>:18789/
ws://<device_ip>:18789/ws
```

## C) Verify media tools (Sense)

```text
mimi> tool_exec observe_scene '{"prompt":"Describe the image."}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":3000}'
mimi> tool_exec device_cli '{"command":"cam_get"}'
mimi> tool_exec device_cli '{"command":"cam_set","framesize":"VGA","quality":15}'
```

## D) Other channels (optional)

```text
mimi> set_tg_token 123456:ABCDEF...
mimi> set_wecom_webhook https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=YOUR_KEY
```

## E) Useful scenarios

1. Remote image/audio capture plus cloud model analysis  
Send a natural-language command through Discord, capture locally on the device, then hand the result to the cloud model:

```text
mimi> tool_exec observe_scene '{"prompt":"Is anything unusual happening at the door right now?"}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":5000}'
```

2. Smart home control hub (next step)  
Right now the device can do perception and understanding. Add a light or curtain control tool over HTTP/MQTT and you have a closed loop:

```text
# Current stage: let the device judge the environment first
mimi> tool_exec observe_scene '{"prompt":"Is the room too dark? Give a recommendation about turning on the lights."}'
```

3. Pet point-of-view Q&A  
Attach the device to a collar or pet bag and ask remotely what the pet is seeing or hearing:

```text
mimi> tool_exec observe_scene '{"prompt":"What is this pet doing right now?"}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":3000}'
```

4. Pet activity analysis (paired with IMU data)  
With three-axis sensor data you can extend this further:
- Calorie estimation based on activity intensity and duration
- Gait detection for abnormal movement alerts
- Behavior recognition such as running, idle, or lying down

5. Remote inspection node  
Use it as a low-power observation point that captures on schedule and pushes a summary to Discord:
- Whether there is a person or sound
- Whether the environment looks abnormal
- Let a human step in only when needed

# How It Works

1. On boot, the device reads `mimi_secrets.h`, then loads runtime overrides from NVS.
2. Discord / CLI / WebSocket messages (plus optional Telegram) all enter the same agent loop.
3. `set_model_provider` selects `zhipu` / `openai` / `anthropic`.
4. `observe_scene` and `listen_and_transcribe` go through the media driver plus multimodal APIs.
5. The UI and WebSocket server run on port `18789`; bench can be triggered from CLI or WebSocket.

# Links

- Seeed XIAO ESP32S3 purchase page: https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html
- Seeed Wiki (getting started): https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- Secrets template: `main/mimi_secrets.h.example`
- CLI command definitions: `main/cli/serial_cli.c`
- Tool registration: `main/tools/tool_registry.c`
- Media tool implementation: `main/tools/tool_media.c`
- XIAO S3 media driver: `main/media/xiao_s3_media.c`
- WS/UI service: `main/gateway/ws_server.c`
- Bench implementation: `main/bench/bench.c`
