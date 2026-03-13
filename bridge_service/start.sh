#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_DIR="$SCRIPT_DIR/.run"
LOG_DIR="$SCRIPT_DIR/logs"
VENV_DIR="$SCRIPT_DIR/.venv"
APP_PID_FILE="$RUN_DIR/bridge.pid"
FRPC_PID_FILE="$RUN_DIR/frpc.pid"
REQ_STAMP="$RUN_DIR/requirements.stamp"
BASE_FRPC_CONFIG="$SCRIPT_DIR/frpc.toml"
RUNTIME_FRPC_CONFIG="$RUN_DIR/frpc.runtime.toml"
FRPC_BIN="$SCRIPT_DIR/.frp/frpc"

mkdir -p "$RUN_DIR" "$LOG_DIR"

if [[ -f "$SCRIPT_DIR/.env" ]]; then
  # shellcheck disable=SC1091
  set -a
  source "$SCRIPT_DIR/.env"
  set +a
fi

is_running() {
  local pid_file="$1"
  local expect="$2"
  [[ -f "$pid_file" ]] || return 1
  local pid
  pid="$(cat "$pid_file" 2>/dev/null || true)"
  [[ -n "$pid" ]] || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  ps -p "$pid" -o command= 2>/dev/null | grep -q "$expect"
}

require_file() {
  local path="$1"
  [[ -f "$path" ]] || {
    echo "missing file: $path" >&2
    exit 1
  }
}

require_cmd() {
  local name="$1"
  command -v "$name" >/dev/null 2>&1 || {
    echo "missing command: $name" >&2
    exit 1
  }
}

prepare_python() {
  require_cmd python3
  if [[ ! -x "$VENV_DIR/bin/python" ]]; then
    python3 -m venv "$VENV_DIR"
  fi

  if [[ ! -f "$REQ_STAMP" || "$SCRIPT_DIR/requirements.txt" -nt "$REQ_STAMP" ]]; then
    "$VENV_DIR/bin/pip" install -r "$SCRIPT_DIR/requirements.txt"
    date +"%Y-%m-%d %H:%M:%S" > "$REQ_STAMP"
  fi
}

render_frpc_config() {
  require_file "$BASE_FRPC_CONFIG"

  if grep -q '^[[:space:]]*auth\.token[[:space:]]*=' "$BASE_FRPC_CONFIG"; then
    cp "$BASE_FRPC_CONFIG" "$RUNTIME_FRPC_CONFIG"
    return
  fi

  if [[ -n "${FRP_AUTH_TOKEN:-}" ]]; then
    awk -v token="$FRP_AUTH_TOKEN" '
      BEGIN { inserted = 0 }
      {
        print
        if (!inserted && $0 ~ /^serverPort[[:space:]]*=/) {
          print "auth.token = \"" token "\""
          inserted = 1
        }
      }
    ' "$BASE_FRPC_CONFIG" > "$RUNTIME_FRPC_CONFIG"
  else
    cp "$BASE_FRPC_CONFIG" "$RUNTIME_FRPC_CONFIG"
  fi
}

start_bridge() {
  if is_running "$APP_PID_FILE" "app.py"; then
    echo "bridge already running: pid $(cat "$APP_PID_FILE")"
    return
  fi

  : "${MIMI_BRIDGE_DISCORD_TOKEN:?MIMI_BRIDGE_DISCORD_TOKEN is required}"
  nohup "$VENV_DIR/bin/python" "$SCRIPT_DIR/app.py" > "$LOG_DIR/bridge.log" 2>&1 &
  echo $! > "$APP_PID_FILE"
  sleep 2

  if ! is_running "$APP_PID_FILE" "app.py"; then
    echo "bridge failed to start" >&2
    tail -n 40 "$LOG_DIR/bridge.log" >&2 || true
    exit 1
  fi

  echo "bridge started: pid $(cat "$APP_PID_FILE")"
}

start_frpc() {
  if is_running "$FRPC_PID_FILE" "frpc"; then
    echo "frpc already running: pid $(cat "$FRPC_PID_FILE")"
    return
  fi

  require_file "$FRPC_BIN"
  render_frpc_config

  nohup "$FRPC_BIN" -c "$RUNTIME_FRPC_CONFIG" > "$LOG_DIR/frpc.log" 2>&1 &
  echo $! > "$FRPC_PID_FILE"
  sleep 2

  if ! is_running "$FRPC_PID_FILE" "frpc"; then
    echo "frpc failed to start" >&2
    tail -n 40 "$LOG_DIR/frpc.log" >&2 || true
    exit 1
  fi

  echo "frpc started: pid $(cat "$FRPC_PID_FILE")"
}

prepare_python
start_bridge
start_frpc

echo "done"
