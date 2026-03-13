# MimiClaw Discord Bridge

这个服务的作用很简单：

- 设备不再直接访问 Discord
- 设备只访问你本地运行的 Python bridge
- `frpc` 把本地 bridge 映射到你的云端 `frps`
- Discord 机器人只跑在你本地这台能访问 Discord 的机器上

## 1. 服务端环境变量

先准备这些变量：

```bash
export MIMI_BRIDGE_DISCORD_TOKEN="你的 Discord Bot Token"
export MIMI_BRIDGE_SHARED_TOKEN="给设备用的共享密钥"
export MIMI_BRIDGE_PORT="8787"
```

说明：

- `MIMI_BRIDGE_DISCORD_TOKEN`：bridge 服务访问 Discord 用
- `MIMI_BRIDGE_SHARED_TOKEN`：设备访问 bridge 用
- `MIMI_BRIDGE_PORT`：本地 bridge 监听端口，默认 `8787`

如果你以后有多台设备，也可以不用共享密钥，改成：

```bash
export MIMI_BRIDGE_DEVICE_TOKENS="mimi-001:secret1,mimi-002:secret2"
```

## 2. 启动 bridge 服务

```bash
cd bridge_service
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
python app.py
```

更省事的方式：

```bash
cd bridge_service
cp .env.example .env
# 修改 .env 里的 token
./start.sh
```

启动后本机检查：

```bash
curl http://127.0.0.1:8787/healthz
```

## 3. 设备端配置

设备端配置的是 bridge 地址，不是 Discord 地址：

```text
mimi> set_bridge_url http://你的公网地址:YOUR_REMOTE_PORT
mimi> set_bridge_device mimi-001
mimi> set_bridge_token 你的共享密钥
mimi> discord_channel_add 123456789012345678
mimi> restart
```

说明：

- `set_bridge_url`：填 `frpc` 映射后的公网入口
- `set_bridge_device`：设备编号，自己定义，建议固定
- `set_bridge_token`：和服务端的 `MIMI_BRIDGE_SHARED_TOKEN` 一致
- `discord_channel_add`：设备允许接收和回复的 Discord 频道 ID

## 4. FRP 映射

你需要填的是你自己的 `frps` 信息：

- 服务器：`YOUR_FRPS_HOST`
- 端口：`7000`
- 远端映射端口：`YOUR_REMOTE_PORT`

本地 `frpc` 配置可以写成：

```toml
serverAddr = "YOUR_FRPS_HOST"
serverPort = 7000

[[proxies]]
name = "mimi-bridge"
type = "tcp"
localIP = "127.0.0.1"
localPort = 8787
remotePort = YOUR_REMOTE_PORT
```

如果你的 `frps` 开了鉴权，还要加：

```toml
auth.token = "你的 frp token"
```

## 5. 启动顺序

建议顺序：

1. 先启动 `bridge_service/app.py`
2. 再启动 `frpc`
3. 最后在设备上执行 `set_bridge_url ...` 并重启

## 6. 常见问题

### 设备连不上 bridge

先在设备网络环境里确认能访问：

```text
http://你的公网地址:YOUR_REMOTE_PORT/healthz
```

如果你前面还挂了域名或 HTTPS，就改成对应地址。

### frpc 连不上 frps

重点检查：

- `YOUR_FRPS_HOST:7000` 是否可达
- `remotePort=YOUR_REMOTE_PORT` 是否被占用
- 是否缺少 `auth.token`

## 7. 一键脚本

启动：

```bash
cd bridge_service
./start.sh
```

停止：

```bash
cd bridge_service
./stop.sh
```

脚本特性：

- 幂等：已经在运行就不会重复启动
- 会自动读取 `bridge_service/.env`
- 首次运行会自动创建 `.venv`
- `requirements.txt` 更新后会自动重新安装依赖

### bridge 启动了但 Discord 不收消息

通常检查这几项：

- Discord Bot Token 是否正确
- Bot 是否已经被拉进目标服务器
- Bot 是否有目标频道的读写权限
- Discord 开发者后台是否打开了 `Message Content Intent`
