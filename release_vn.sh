#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./release_vn.sh jiuchuan 2.1.4
#   ./release_vn.sh luxiaoban 2.1.0

if [ "$#" -lt 2 ]; then
  echo "Usage: $0 <jiuchuan|luxiaoban> <version> [jiuchuan_lcd_driver]"
  echo "Example: $0 jiuchuan 2.1.4 2"
  echo "Example: $0 luxiaoban 2.1.0"
  exit 1
fi

BOARD="$(echo "$1" | tr '[:upper:]' '[:lower:]')"
VERSION="$2"
JIUCHUAN_LCD_DRIVER="${3:-2}"

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_DIR"

case "$BOARD" in
  jiuchuan)
    BOARD_LABEL="Jiuchuan"
    BUILD_DIR="build_jiuchuan"
    SDKCONFIG_FILE="sdkconfig.jiuchuan"
    ASSET_FILE="main/Pro-Snoopy-Hitelly.bin"
    APP_EXTRA_ARGS=(-D "JIUCHUAN_DEFAULT_LCD_DRIVER=${JIUCHUAN_LCD_DRIVER}")
    ;;
  jiuchuan_v2)
    BOARD_LABEL="Jiuchuan-V2"
    BUILD_DIR="build_jiuchuan_v2"
    SDKCONFIG_FILE="sdkconfig.jiuchuan_v2"
    ASSET_FILE="main/Pro-Snoopy-Hitelly.bin"
    APP_EXTRA_ARGS=(-D "JIUCHUAN_DEFAULT_LCD_DRIVER=2")
    ;;
  luxiaoban)
    BOARD_LABEL="Luxiaoban"
    BUILD_DIR="build_luxiaoban"
    SDKCONFIG_FILE="sdkconfig.luxiaoban"
    ASSET_FILE="main/Plus-Snoopy-HiLily.bin"
    APP_EXTRA_ARGS=(-D "LUXIAOBAN_BUILD=1")
    ;;
  *)
    echo "Unsupported board: $BOARD (only: jiuchuan, jiuchuan_v2, luxiaoban)"
    exit 1
    ;;
esac

if [ ! -f "$SDKCONFIG_FILE" ]; then
  echo "Missing $SDKCONFIG_FILE"
  exit 1
fi

if [ ! -f "$ASSET_FILE" ]; then
  echo "Missing asset: $ASSET_FILE"
  exit 1
fi

if ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py not found. Please run: source ~/esp/esp-idf/export.sh"
  exit 1
fi

PYTHON_BIN=""
if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
  PYTHON_BIN="$IDF_PYTHON_ENV_PATH/bin/python"
elif command -v python3 >/dev/null 2>&1; then
  PYTHON_BIN="python3"
elif command -v python >/dev/null 2>&1; then
  PYTHON_BIN="python"
else
  echo "Python not found"
  exit 1
fi

OUT_DIR="releases"
mkdir -p "$OUT_DIR"

TAG="${BOARD_LABEL}-${VERSION}"
OTA_BIN="$OUT_DIR/${TAG}-OTA.bin"
MERGED_BIN="$OUT_DIR/${TAG}.bin"
SOURCE_ZIP="$OUT_DIR/${TAG}-source.zip"

echo "=============================================="
echo "Release tag : $TAG"
echo "Board       : $BOARD"
echo "Build dir   : $BUILD_DIR"
echo "Sdkconfig   : $SDKCONFIG_FILE"
echo "Asset       : $ASSET_FILE"
echo "=============================================="

# Build firmware
idf.py -B "$BUILD_DIR" -D "SDKCONFIG=$SDKCONFIG_FILE" "${APP_EXTRA_ARGS[@]}" build

# Validate build outputs
BOOTLOADER="$BUILD_DIR/bootloader/bootloader.bin"
PARTITION="$BUILD_DIR/partition_table/partition-table.bin"
OTA_DATA="$BUILD_DIR/ota_data_initial.bin"
APP_BIN="$BUILD_DIR/xiaozhi_vn.bin"

for f in "$BOOTLOADER" "$PARTITION" "$OTA_DATA" "$APP_BIN" "$ASSET_FILE"; do
  if [ ! -f "$f" ]; then
    echo "Missing file: $f"
    exit 1
  fi
done

# Export OTA bin
cp "$APP_BIN" "$OTA_BIN"

# Export merged full-flash bin
"$PYTHON_BIN" -m esptool --chip esp32s3 merge_bin \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  -o "$MERGED_BIN" \
  0x0       "$BOOTLOADER" \
  0x8000    "$PARTITION" \
  0xd000    "$OTA_DATA" \
  0x20000   "$APP_BIN" \
  0xa00000  "$ASSET_FILE"

# Export source package
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git archive --format=zip --output="$SOURCE_ZIP" HEAD
else
  # fallback if not a git working tree
  zip -rq "$SOURCE_ZIP" . \
    -x "./build/*" "./build_*/**" "./.git/*" "./releases/*" "*.DS_Store"
fi

echo "----------------------------------------------"
echo "DONE:"
echo "- Source  : $SOURCE_ZIP"
echo "- OTA bin : $OTA_BIN"
echo "- Full bin: $MERGED_BIN"
ls -lh "$SOURCE_ZIP" "$OTA_BIN" "$MERGED_BIN"
