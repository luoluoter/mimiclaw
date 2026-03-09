# MimiClaw デプロイガイド

<p align="center">
  <strong><a href="README.md">English</a> | <a href="README_CN.md">中文</a> | <a href="README_JA.md">日本語</a></strong>
</p>

MimiClaw（`dev/seeed-xiao-s3`）の `lzx` コミット範囲向けデプロイガイドです。  
最小構成のデフォルト経路は `Zhipu API + Discord` です。

対象コミット範囲：`8354932` ~ `46dff47`

この文書では、このコミット範囲で追加・変更された内容だけを扱います。
- Web UI + WebSocket + bench
- デフォルト構成は Zhipu + Discord、他チャネルは任意
- OpenAI / Anthropic / WeCom / Telegram の任意設定
- XIAO ESP32S3 Sense 向けメディアツール（撮影 / 録音 / 認識）

# Quick Start

## 0) ハードウェアを用意する

必須：
- 開発ボード：Seeed XIAO ESP32S3
  - 購入先：https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html
  - Wiki：https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- USB-C データケーブル（充電専用ではないもの）
- 2.4GHz Wi-Fi

写真や音声ツールを使う場合：
- XIAO ESP32S3 Sense を推奨します（カメラとマイクの経路を含む）
- 通常の XIAO ESP32S3 では `camera/audio not supported` と表示される場合があります

## 1) リポジトリを取得してブランチを切り替える

```bash
git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw
git switch dev/seeed-xiao-s3
```

## 2) ESP-IDF をインストールする（初回のみ）

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

## 3) 設定を記入する（デフォルト：Zhipu + Discord）

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

最小構成として `main/mimi_secrets.h` を次のように編集します。

```c
#define MIMI_SECRET_WIFI_SSID       "YOUR_WIFI"
#define MIMI_SECRET_WIFI_PASS       "YOUR_WIFI_PASSWORD"

#define MIMI_SECRET_API_KEY         "YOUR_API_KEY"
#define MIMI_SECRET_MODEL_PROVIDER  "zhipu"     // zhipu | openai | anthropic
#define MIMI_SECRET_MODEL           "GLM-4-FlashX-250414"

#define MIMI_SECRET_DISCORD_TOKEN   "YOUR_DISCORD_BOT_TOKEN"
#define MIMI_SECRET_TG_TOKEN        ""          // 任意
#define MIMI_SECRET_WECOM_WEBHOOK   ""          // 任意
```

API キーの取得先：
- Zhipu：https://open.bigmodel.cn/
- OpenAI（任意）：https://platform.openai.com/
- Anthropic（任意）：https://console.anthropic.com/

よくある落とし穴：
- `MODEL_PROVIDER` と `MODEL` は一致している必要があります。不一致だとリクエストは失敗します。
- Discord チャンネルを設定していない場合、ボットは対象チャンネルに返信しません。
- CLI だけで試すなら Discord / Telegram は空でも構いません。
- 制限のあるネットワークでは、先に CLI でプロキシを設定してください：`set_proxy HOST PORT [http|socks5]`

## 4) ビルドして書き込む

```bash
idf.py set-target esp32s3
idf.py fullclean
idf.py build
```

シリアルポートを確認：

```bash
# Linux
ls /dev/ttyACM* /dev/ttyUSB*

# macOS
ls /dev/cu.usb*
```

書き込みとログ監視：

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

よくある落とし穴：
- Type-C ポートが 2 つある基板では、書き込みにはネイティブ USB/JTAG ポートを使ってください。
- `monitor` でコマンド入力できない場合は、UART/COM ポートに切り替えて再度開いてください。

## 5) 実行時に設定を上書きする（コード変更なし、CLI のみ）

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

## A) まず基本経路を確認する

```text
mimi> wifi_status
mimi> chat hello
mimi> bench all
mimi> bench llm
```

## B) Web UI / WebSocket を確認する

```text
http://<device_ip>:18789/
ws://<device_ip>:18789/ws
```

## C) メディアツール（Sense）を確認する

```text
mimi> tool_exec observe_scene '{"prompt":"Describe the image."}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":3000}'
mimi> tool_exec device_cli '{"command":"cam_get"}'
mimi> tool_exec device_cli '{"command":"cam_set","framesize":"VGA","quality":15}'
```

## D) その他のチャネル（任意）

```text
mimi> set_tg_token 123456:ABCDEF...
mimi> set_wecom_webhook https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=YOUR_KEY
```

## E) 価値のある活用シナリオ

1. リモート画像/音声取得 + クラウドモデル解析  
Discord から自然言語で指示を送り、デバイスがローカルで取得し、その結果をクラウドモデルに渡します。

```text
mimi> tool_exec observe_scene '{"prompt":"玄関で今、何か異常はありますか？"}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":5000}'
```

2. スマートホーム制御ハブ（次の拡張）  
現状でも「認識 + 理解」までは実行できます。照明やカーテンを制御する HTTP/MQTT ツールを追加すれば閉ループになります。

```text
# 現段階では、まず環境判断をさせる
mimi> tool_exec observe_scene '{"prompt":"部屋は暗すぎますか？電気をつけるべきか提案してください。"}'
```

3. ペット視点の Q&A  
首輪やバッグに付けて、今何を見ているか、何を聞いているかを遠隔で尋ねられます。

```text
mimi> tool_exec observe_scene '{"prompt":"このペットは今何をしていますか？"}'
mimi> tool_exec listen_and_transcribe '{"duration_ms":3000}'
```

4. ペット運動分析（IMU と組み合わせ）
- 活動強度と時間に基づく消費カロリー推定
- 異常動作を検出する歩容解析
- 走行、静止、伏せなどの行動認識

5. リモート巡回ノード
- 低消費電力の観測点として定期取得し、要約を Discord に送る
- 人や物音の有無を判定する
- 異常時だけ人が介入する

# How It Works

1. 起動時に `mimi_secrets.h` を読み込み、その後 NVS の実行時オーバーライドを読み込みます。
2. Discord / CLI / WebSocket のメッセージ（および任意の Telegram）が同じ agent loop に入ります。
3. `set_model_provider` で `zhipu` / `openai` / `anthropic` を切り替えます。
4. `observe_scene` と `listen_and_transcribe` はメディアドライバとマルチモーダル API を使います。
5. UI と WebSocket サーバは `18789` 番ポートで動作し、bench は CLI または WebSocket から実行できます。

# Links

- Seeed XIAO ESP32S3 購入ページ：https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html
- Seeed Wiki（入門）：https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- Secrets テンプレート：`main/mimi_secrets.h.example`
- CLI コマンド定義：`main/cli/serial_cli.c`
- ツール登録：`main/tools/tool_registry.c`
- メディアツール実装：`main/tools/tool_media.c`
- XIAO S3 メディアドライバ：`main/media/xiao_s3_media.c`
- WS/UI サービス：`main/gateway/ws_server.c`
- Bench 実装：`main/bench/bench.c`
