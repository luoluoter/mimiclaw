# MimiClaw 部署说明

<p align="center">
  <strong><a href="README.md">English</a> | <a href="README_CN.md">中文</a> | <a href="README_JA.md">日本語</a></strong>
</p>

MimiClaw（`dev/seeed-xiao-s3`）在 `lzx` 提交范围内的部署文档。  
默认最小可运行路径：`Zhipu API + Discord`。

适用提交范围：`8354932` ~ `46dff47`

本文只覆盖这批提交里的改动：
- Web UI + WebSocket + bench
- 默认路径为 Zhipu + Discord，其他渠道按需开启
- 可选的 OpenAI / Anthropic / WeCom / Telegram 配置
- XIAO ESP32S3 Sense 媒体工具（拍照 / 录音 / 识别）

# Quick Start

## 0) 准备硬件

必需：
- 开发板：Seeed XIAO ESP32S3
  - 购买链接：https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html
  - Wiki：https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- USB-C 数据线（不是纯充电线）
- 2.4GHz Wi-Fi

如果你要使用拍照或录音工具：
- 建议使用 XIAO ESP32S3 Sense（自带摄像头和麦克风链路）
- 普通 XIAO ESP32S3 可能会提示 `camera/audio not supported`

## 1) 拉代码并切换分支

```bash
git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw
git switch dev/seeed-xiao-s3
```

## 2) 安装 ESP-IDF（一次即可）

Ubuntu：

```bash
./scripts/setup_idf_ubuntu.sh
. "$HOME/.espressif/esp-idf-v5.5.2/export.sh"
```

macOS：

```bash
./scripts/setup_idf_macos.sh
. "$HOME/.espressif/esp-idf-v5.5.2/export.sh"
```

## 3) 填配置（默认：Zhipu + Discord）

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

编辑 `main/mimi_secrets.h`，最小可运行配置如下：

```c
#define MIMI_SECRET_WIFI_SSID       "YOUR_WIFI"
#define MIMI_SECRET_WIFI_PASS       "YOUR_WIFI_PASSWORD"

#define MIMI_SECRET_API_KEY         "YOUR_API_KEY"
#define MIMI_SECRET_MODEL_PROVIDER  "zhipu"     // zhipu | openai | anthropic
#define MIMI_SECRET_MODEL           "GLM-4-FlashX-250414"

#define MIMI_SECRET_DISCORD_TOKEN   "YOUR_DISCORD_BOT_TOKEN"
#define MIMI_SECRET_TG_TOKEN        ""          // 可选
#define MIMI_SECRET_WECOM_WEBHOOK   ""          // 可选
```

API key 获取入口：
- Zhipu：https://open.bigmodel.cn/
- OpenAI（可选）：https://platform.openai.com/
- Anthropic（可选）：https://console.anthropic.com/

常见坑点：
- `MODEL_PROVIDER` 和 `MODEL` 必须匹配，否则请求会失败。
- 如果没有配置 Discord 频道，机器人不会在目标频道里回消息。
- 如果你只通过 CLI 测试，可以不填 Discord / Telegram。
- 网络受限时，先在 CLI 里设置代理：`set_proxy HOST PORT [http|socks5]`。

## 4) 编译与烧录

```bash
idf.py set-target esp32s3
idf.py fullclean
idf.py build
```

查串口：

```bash
# Linux
ls /dev/ttyACM* /dev/ttyUSB*

# macOS
ls /dev/cu.usb*
```

烧录并查看日志：

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

常见坑点：
- 如果板子有两个 Type-C 口，烧录请使用原生 USB/JTAG 口。
- 如果 `monitor` 里无法输入命令，切到 UART/COM 口重新打开串口监视。

## 5) 运行时覆盖配置（不改代码，直接走 CLI）

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

# Examples

## A) 先验证基础链路

```text
mimi> wifi_status
mimi> chat hello
mimi> bench all
mimi> bench llm
```

## B) 验证 Web UI / WebSocket

```text
http://<device_ip>:18789/
ws://<device_ip>:18789/ws
```

## C) 验证媒体工具（Sense）

```text
mimi> tool_exec observe_scene '{"prompt":"Describe the image."}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":3000}'
mimi> tool_exec device_cli '{"command":"cam_get"}'
mimi> tool_exec device_cli '{"command":"cam_set","framesize":"VGA","quality":15}'
```

## D) 其他渠道（可选）

```text
mimi> set_tg_token 123456:ABCDEF...
mimi> set_wecom_webhook https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=YOUR_KEY
```

## E) 有价值的使用场景

1. 远程图像/音频采集 + 云端模型分析  
通过 Discord 发自然语言命令，设备本地采集，再交给云端模型理解：

```text
mimi> tool_exec observe_scene '{"prompt":"门口现在有什么异常？"}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":5000}'
```

2. 智能家居中控（下一步扩展）  
当前阶段设备已经能做“感知 + 理解”，再补一个灯光或窗帘控制工具（HTTP/MQTT）就能闭环：

```text
# 当前阶段：先让设备判断环境
mimi> tool_exec observe_scene '{"prompt":"房间是否太暗？给出开灯建议。"}'
```

3. 宠物第一视角问答  
把设备挂在宠物项圈或背包上，远程询问它现在在看什么、听到什么：

```text
mimi> tool_exec observe_scene '{"prompt":"这只宠物现在在做什么？"}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":3000}'
```

4. 宠物运动分析（可结合 IMU）
- 根据活动强度和时长估算热量消耗
- 做步态检测，提前发现异常动作
- 识别奔跑、静止、趴卧等行为

5. 远程巡检节点
- 作为低功耗观察点，定时采集并把摘要推到 Discord
- 判断是否有人、是否有声响
- 发现环境异常时再让人工介入

# How It Works

1. 启动后先读取 `mimi_secrets.h`，再加载 NVS 里的运行时覆盖项。
2. Discord / CLI / WebSocket 消息（以及可选的 Telegram）都会进入同一个 agent loop。
3. `set_model_provider` 决定走 `zhipu` / `openai` / `anthropic`。
4. `observe_scene` 和 `listen_and_transcribe` 会走媒体驱动和多模态 API。
5. UI 和 WebSocket 服务跑在 `18789` 端口；bench 可从 CLI 或 WebSocket 触发。

# Links

- Seeed XIAO ESP32S3 购买页：https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html
- Seeed Wiki（入门）：https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- Secrets 模板：`main/mimi_secrets.h.example`
- CLI 命令定义：`main/cli/serial_cli.c`
- 工具注册：`main/tools/tool_registry.c`
- 媒体工具实现：`main/tools/tool_media.c`
- XIAO S3 媒体驱动：`main/media/xiao_s3_media.c`
- WS/UI 服务：`main/gateway/ws_server.c`
- Bench 实现：`main/bench/bench.c`
