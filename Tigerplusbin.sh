#!/usr/bin/env bash
set -e

# ==================================================
#   GLOBY TIGER PLUS — AUTO BUILD & MERGE BIN FILE
# ==================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
CMAKELIST="$PROJECT_DIR/CMakeLists.txt"

echo "🔍 Đang đọc version từ CMakeLists.txt..."

# ------------------------------------
# 1. Tự động lấy VERSION
# ------------------------------------
VERSION=$(grep -Eo 'set\s*\(\s*PROJECT_VER\s*"[0-9]+\.[0-9]+\.[0-9]+"\s*\)' $CMAKELIST | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+')

if [ -z "$VERSION" ]; then
    VERSION=$(grep -Eo 'VERSION\s+[0-9]+\.[0-9]+\.[0-9]+' $CMAKELIST | awk '{print $2}')
fi

if [ -z "$VERSION" ]; then
    echo "❌ Không tìm thấy VERSION trong CMakeLists.txt!"
    exit 1
fi

echo "🌟 VERSION tìm được: $VERSION"
VERSION="${VERSION_OVERRIDE:-$VERSION}"
echo "🌟 VERSION sử dụng để đặt tên file: $VERSION"


# ------------------------------------
# 2. Build firmware
# ------------------------------------
echo "🚀 Đang build firmware..."
IDF_PY="${IDF_PY_OVERRIDE:-idf.py}"
SDKCONFIG_ARG="${SDKCONFIG_ARG:-}"
if [ -n "$SDKCONFIG_ARG" ]; then
  "$IDF_PY" -D "SDKCONFIG=$SDKCONFIG_ARG" build || { echo "❌ Build thất bại!"; exit 1; }
else
  "$IDF_PY" build || { echo "❌ Build thất bại!"; exit 1; }
fi


# ------------------------------------
# 3. Kiểm tra file build tồn tại
# ------------------------------------
BOOTLOADER="build/bootloader/bootloader.bin"
PARTITION="build/partition_table/partition-table.bin"
OTA="build/ota_data_initial.bin"
APP="build/xiaozhi_vn.bin"
ASSETS="${ASSETS_FILE_OVERRIDE:-$PROJECT_DIR/main/Plus-Snoopy-HiLily.bin}"

echo "📦 Kiểm tra file cần merge..."

for f in $BOOTLOADER $PARTITION $OTA $APP $ASSETS; do
    if [ ! -f "$f" ]; then
        echo "❌ Thiếu file: $f"
        exit 1
    fi
done

echo "📦 Asset dùng để merge: $ASSETS"


# ------------------------------------
# 4. Tạo merge BIN (không flash)
# ------------------------------------
OTA_OUTPUT="Globy-TigerPlus-${VERSION}-Vi-OTA.bin"
OUTPUT="Globy-TigerPlus-${VERSION}-Vi-merged.bin"

echo "📤 Tạo OTA app bin: $OTA_OUTPUT"
cp "$APP" "$OTA_OUTPUT" || { echo "❌ Không thể tạo OTA bin!"; exit 1; }

echo "🔧 Đang tạo file merge: $OUTPUT"

PYTHON_BIN=""
if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
    PYTHON_BIN="$IDF_PYTHON_ENV_PATH/bin/python"
elif [ -x "$HOME/.espressif/python_env/idf5.5_py3.9_env/bin/python" ]; then
    PYTHON_BIN="$HOME/.espressif/python_env/idf5.5_py3.9_env/bin/python"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="python3"
elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN="python"
else
    echo "❌ Không tìm thấy Python runtime để chạy esptool!"
    exit 1
fi

"$PYTHON_BIN" -m esptool --chip esp32s3 merge_bin \
  -o $OUTPUT \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  0x0      $BOOTLOADER \
  0x8000   $PARTITION \
  0xd000   $OTA \
  0x20000  $APP \
  0x800000 $ASSETS

echo "🎉 DONE! File firmware đã tạo:"
echo "👉 OTA:    $OTA_OUTPUT"
echo "👉 MERGED: $OUTPUT"
