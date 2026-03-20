#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_VERSION="${IDF_VERSION:-v5.5.2}"
ESP_ROOT="${ESP_ROOT:-$HOME/.espressif}"
DEFAULT_IDF_DIR="$ESP_ROOT/esp-idf-$IDF_VERSION"
ALT_IDF_DIR="$(cd "$PROJECT_ROOT/.." && pwd)/esp-idf"
IDF_DIR="${IDF_DIR:-${IDF_PATH:-}}"
OUTPUT_BIN="${OUTPUT_BIN:-$PROJECT_ROOT/build/mimiclaw-merged.bin}"

if [[ -z "$IDF_DIR" ]]; then
  if [[ -f "$DEFAULT_IDF_DIR/export.sh" ]]; then
    IDF_DIR="$DEFAULT_IDF_DIR"
  elif [[ -f "$ALT_IDF_DIR/export.sh" ]]; then
    IDF_DIR="$ALT_IDF_DIR"
  else
    echo "ESP-IDF not found. Set IDF_DIR or IDF_PATH, or install ESP-IDF first." >&2
    exit 1
  fi
fi

if [[ ! -f "$IDF_DIR/export.sh" ]]; then
  echo "ESP-IDF export.sh not found at: $IDF_DIR" >&2
  exit 1
fi

# shellcheck source=/dev/null
. "$IDF_DIR/export.sh"

cd "$PROJECT_ROOT"

idf.py set-target esp32s3 >/dev/null
idf.py build

FLASH_SIZE="$(
  python - <<'PY'
import json
from pathlib import Path

data = json.loads(Path("build/flasher_args.json").read_text())
print(data["flash_settings"]["flash_size"])
PY
)"

mkdir -p "$(dirname "$OUTPUT_BIN")"

idf.py merge-bin -o "$OUTPUT_BIN" --fill-flash-size "$FLASH_SIZE"

BIN_SIZE="$(stat -f%z "$OUTPUT_BIN" 2>/dev/null || stat -c%s "$OUTPUT_BIN" 2>/dev/null)"

cat <<EOF
Merged firmware created:
  $OUTPUT_BIN

Flash it at offset 0x0:
  esptool.py --chip esp32s3 -p PORT write_flash 0x0 $OUTPUT_BIN

Current flash size:
  $FLASH_SIZE

Merged binary size:
  $(( BIN_SIZE / 1024 )) KB
EOF
