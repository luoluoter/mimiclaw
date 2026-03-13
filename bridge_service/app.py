import asyncio
import io
import logging
import os
import time
from collections import deque
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from typing import Deque, Dict, List, Optional, Set

import discord
import uvicorn
from fastapi import FastAPI, File, Form, Header, HTTPException, UploadFile
from pydantic import BaseModel, Field


LOG_LEVEL = os.getenv("MIMI_BRIDGE_LOG_LEVEL", "INFO").upper()
logging.basicConfig(level=LOG_LEVEL, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger("mimi_bridge")


def split_csv_map(raw: str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    for item in raw.split(","):
        item = item.strip()
        if not item or ":" not in item:
            continue
        key, value = item.split(":", 1)
        key = key.strip()
        value = value.strip()
        if key and value:
            result[key] = value
    return result


def split_discord_message(text: str, limit: int = 2000) -> List[str]:
    text = text or ""
    if len(text) <= limit:
        return [text]
    chunks: List[str] = []
    remaining = text
    while remaining:
        if len(remaining) <= limit:
            chunks.append(remaining)
            break
        cut = remaining.rfind("\n", 0, limit)
        if cut <= 0:
            cut = limit
        chunks.append(remaining[:cut])
        remaining = remaining[cut:].lstrip("\n")
    return chunks


@dataclass
class QueuedMessage:
    channel: str
    chat_id: str
    content: str
    message_id: str
    created_at: float = field(default_factory=time.time)


@dataclass
class DeviceState:
    discord_channels: Set[str] = field(default_factory=set)
    queue: Deque[QueuedMessage] = field(default_factory=deque)
    last_seen_at: float = field(default_factory=time.time)


class PullSubscriptions(BaseModel):
    discord_channels: List[str] = Field(default_factory=list)


class PullRequest(BaseModel):
    subscriptions: PullSubscriptions = Field(default_factory=PullSubscriptions)
    max_messages: int = Field(default=8, ge=1, le=32)


class SendMessageRequest(BaseModel):
    channel: str
    chat_id: str
    content: str


class TypingRequest(BaseModel):
    channel: str
    chat_id: str


class BridgeRuntime:
    def __init__(self) -> None:
        self.shared_token = os.getenv("MIMI_BRIDGE_SHARED_TOKEN", "").strip()
        self.device_tokens = split_csv_map(os.getenv("MIMI_BRIDGE_DEVICE_TOKENS", ""))
        self.max_queue = int(os.getenv("MIMI_BRIDGE_MAX_QUEUE", "200"))
        self.devices: Dict[str, DeviceState] = {}
        self.lock = asyncio.Lock()

        intents = discord.Intents.default()
        intents.message_content = True
        intents.guilds = True
        intents.messages = True
        self.discord_client = DiscordBridgeClient(runtime=self, intents=intents)
        self.discord_task: Optional[asyncio.Task] = None

    def validate_device(self, device_id: str, device_token: str) -> None:
        if not device_id:
            raise HTTPException(status_code=401, detail="missing device id")
        if not device_token:
            raise HTTPException(status_code=401, detail="missing device token")

        if self.device_tokens:
            if self.device_tokens.get(device_id) != device_token:
                raise HTTPException(status_code=403, detail="invalid device credentials")
            return

        if self.shared_token:
            if self.shared_token != device_token:
                raise HTTPException(status_code=403, detail="invalid device token")
            return

        logger.warning("No bridge auth configured; accepting device %s without verification", device_id)

    async def update_device(self, device_id: str, channels: List[str]) -> DeviceState:
        async with self.lock:
            state = self.devices.setdefault(device_id, DeviceState())
            state.discord_channels = {str(ch) for ch in channels if str(ch).strip()}
            state.last_seen_at = time.time()
            return state

    async def pull_messages(self, device_id: str, max_messages: int) -> List[QueuedMessage]:
        async with self.lock:
            state = self.devices.setdefault(device_id, DeviceState())
            state.last_seen_at = time.time()
            messages: List[QueuedMessage] = []
            while state.queue and len(messages) < max_messages:
                messages.append(state.queue.popleft())
            return messages

    async def enqueue_discord_message(self, channel_id: str, content: str, message_id: str) -> None:
        async with self.lock:
            for state in self.devices.values():
                if channel_id not in state.discord_channels:
                    continue
                if len(state.queue) >= self.max_queue:
                    state.queue.popleft()
                state.queue.append(
                    QueuedMessage(
                        channel="discord",
                        chat_id=channel_id,
                        content=content,
                        message_id=message_id,
                    )
                )

    async def start_discord(self) -> None:
        token = os.getenv("MIMI_BRIDGE_DISCORD_TOKEN", "").strip()
        if not token:
            raise RuntimeError("MIMI_BRIDGE_DISCORD_TOKEN is required")

        async def runner() -> None:
            await self.discord_client.start(token)

        self.discord_task = asyncio.create_task(runner(), name="discord-bridge-bot")
        await asyncio.sleep(0.5)
        if self.discord_task.done():
            self.discord_task.result()

    async def stop_discord(self) -> None:
        if self.discord_client.is_ready():
            await self.discord_client.close()
        if self.discord_task:
            try:
                await self.discord_task
            except Exception:
                logger.exception("Discord task exited with error")

    async def get_channel(self, channel_id: str):
        try:
            discord_id = int(channel_id)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=f"invalid channel id: {channel_id}") from exc

        channel = self.discord_client.get_channel(discord_id)
        if channel is None:
            try:
                channel = await self.discord_client.fetch_channel(discord_id)
            except Exception as exc:
                raise HTTPException(status_code=404, detail=f"channel not found: {channel_id}") from exc
        return channel


class DiscordBridgeClient(discord.Client):
    def __init__(self, runtime: BridgeRuntime, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.runtime = runtime

    async def on_ready(self) -> None:
        logger.info("Discord bridge logged in as %s", self.user)

    async def on_message(self, message: discord.Message) -> None:
        if message.author.bot:
            return

        parts: List[str] = []
        if message.content and message.content.strip():
            parts.append(message.content.strip())
        for attachment in message.attachments[:3]:
            parts.append(attachment.url)
        if not parts:
            return

        await self.runtime.enqueue_discord_message(
            channel_id=str(message.channel.id),
            content="\n".join(parts),
            message_id=str(message.id),
        )


runtime = BridgeRuntime()


@asynccontextmanager
async def lifespan(app: FastAPI):
    await runtime.start_discord()
    yield
    await runtime.stop_discord()


app = FastAPI(title="MimiClaw Discord Bridge", lifespan=lifespan)


def require_device(
    x_mimi_device_id: Optional[str],
    x_mimi_device_token: Optional[str],
) -> str:
    device_id = (x_mimi_device_id or "").strip()
    device_token = (x_mimi_device_token or "").strip()
    runtime.validate_device(device_id, device_token)
    return device_id


@app.get("/healthz")
async def healthz():
    return {
        "ok": True,
        "discord_ready": runtime.discord_client.is_ready(),
        "device_count": len(runtime.devices),
    }


@app.post("/api/v1/device/pull")
async def device_pull(
    payload: PullRequest,
    x_mimi_device_id: Optional[str] = Header(default=None),
    x_mimi_device_token: Optional[str] = Header(default=None),
):
    device_id = require_device(x_mimi_device_id, x_mimi_device_token)
    await runtime.update_device(device_id, payload.subscriptions.discord_channels)
    messages = await runtime.pull_messages(device_id, payload.max_messages)
    return {
        "messages": [
            {
                "channel": item.channel,
                "chat_id": item.chat_id,
                "content": item.content,
                "message_id": item.message_id,
                "created_at": item.created_at,
            }
            for item in messages
        ]
    }


@app.post("/api/v1/device/message")
async def device_message(
    payload: SendMessageRequest,
    x_mimi_device_id: Optional[str] = Header(default=None),
    x_mimi_device_token: Optional[str] = Header(default=None),
):
    require_device(x_mimi_device_id, x_mimi_device_token)
    if payload.channel != "discord":
        raise HTTPException(status_code=400, detail=f"unsupported channel: {payload.channel}")

    channel = await runtime.get_channel(payload.chat_id)
    for chunk in split_discord_message(payload.content):
        await channel.send(chunk)
    return {"ok": True}


@app.post("/api/v1/device/typing")
async def device_typing(
    payload: TypingRequest,
    x_mimi_device_id: Optional[str] = Header(default=None),
    x_mimi_device_token: Optional[str] = Header(default=None),
):
    require_device(x_mimi_device_id, x_mimi_device_token)
    if payload.channel != "discord":
        raise HTTPException(status_code=400, detail=f"unsupported channel: {payload.channel}")

    channel = await runtime.get_channel(payload.chat_id)
    trigger = getattr(channel, "trigger_typing", None)
    if callable(trigger):
        await trigger()
    else:
        async with channel.typing():
            await asyncio.sleep(0.2)
    return {"ok": True}


@app.post("/api/v1/device/file")
async def device_file(
    channel: str = Form(...),
    chat_id: str = Form(...),
    caption: str = Form(default=""),
    file: UploadFile = File(...),
    x_mimi_device_id: Optional[str] = Header(default=None),
    x_mimi_device_token: Optional[str] = Header(default=None),
):
    require_device(x_mimi_device_id, x_mimi_device_token)
    if channel != "discord":
        raise HTTPException(status_code=400, detail=f"unsupported channel: {channel}")

    discord_channel = await runtime.get_channel(chat_id)
    data = await file.read()
    discord_file = discord.File(io.BytesIO(data), filename=file.filename or "upload.bin")
    send_kwargs = {"file": discord_file}
    if caption:
        send_kwargs["content"] = caption[:2000]
    await discord_channel.send(**send_kwargs)
    return {"ok": True, "size": len(data)}


if __name__ == "__main__":
    uvicorn.run(
        "app:app",
        host=os.getenv("MIMI_BRIDGE_HOST", "0.0.0.0"),
        port=int(os.getenv("MIMI_BRIDGE_PORT", "8787")),
        reload=False,
        log_level=LOG_LEVEL.lower(),
    )
