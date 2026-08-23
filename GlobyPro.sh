#!/usr/bin/env bash
set -e

echo "=============================================="
echo " ESP32-S3 MERGE BIN (from write_flash layout)"
echo "=============================================="

# --------------------------------------------------
# Resolve script directory (run from anywhere)
# --------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build"
ASSETS_FILE="${ASSETS_FILE_OVERRIDE:-$PROJECT_DIR/main/Pro-Snoopy-Hitelly.bin}"
VERSION="$(grep -E '^set\(PROJECT_VER \"[^\"]+\"\)' "$PROJECT_DIR/CMakeLists.txt" | sed -E 's/^set\(PROJECT_VER \"([^\"]+)\"\)/\1/' | head -n 1)"
if [ -z "$VERSION" ]; then
  echo "❌ Could not read PROJECT_VER from CMakeLists.txt"
  exit 1
fi
VERSION="${VERSION_OVERRIDE:-$VERSION}"
OTA_BIN="$PROJECT_DIR/Globy-RabbitPro-${VERSION}-OTA.bin"
OUTPUT_BIN="$PROJECT_DIR/Globy-RabbitPro-${VERSION}.bin"

echo "📁 Project dir : $PROJECT_DIR"
echo "📁 Build dir   : $BUILD_DIR"
echo "📦 Assets file : $ASSETS_FILE"
echo "📤 OTA bin     : $OTA_BIN"
echo "📤 Output bin  : $OUTPUT_BIN"
echo "----------------------------------------------"

# --------------------------------------------------
# Check directories
# --------------------------------------------------
if [ ! -d "$BUILD_DIR" ]; then
  echo "❌ build/ directory not found!"
  exit 1
fi

# --------------------------------------------------
# Check required files
# --------------------------------------------------
REQUIRED_FILES=(
  "$BUILD_DIR/bootloader/bootloader.bin"
  "$BUILD_DIR/partition_table/partition-table.bin"
  "$BUILD_DIR/ota_data_initial.bin"
  "$BUILD_DIR/xiaozhi_vn.bin"
  "$ASSETS_FILE"
)

for f in "${REQUIRED_FILES[@]}"; do
  if [ ! -f "$f" ]; then
    echo "❌ Missing file: $f"
    exit 1
  fi
done

echo "✅ All required files found"
echo "➡️  Creating OTA BIN..."
cp "$BUILD_DIR/xiaozhi_vn.bin" "$OTA_BIN"
ls -lh "$OTA_BIN"
echo "----------------------------------------------"
echo "➡️  Merging firmware into ONE BIN..."

# --------------------------------------------------
# Resolve Python runtime that has esptool
# --------------------------------------------------
PYTHON_BIN=""
if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
  PYTHON_BIN="$IDF_PYTHON_ENV_PATH/bin/python"
elif [ -x "$HOME/.espressif/python_env/idf5.5_py3.9_env/bin/python" ]; then
  PYTHON_BIN="$HOME/.espressif/python_env/idf5.5_py3.9_env/bin/python"
elif command -v python >/dev/null 2>&1 && python -c "import esptool" >/dev/null 2>&1; then
  PYTHON_BIN="python"
elif command -v python3 >/dev/null 2>&1 && python3 -c "import esptool" >/dev/null 2>&1; then
  PYTHON_BIN="python3"
else
  echo "❌ Could not find a Python runtime with esptool module"
  exit 1
fi

# --------------------------------------------------
# MERGE BIN (equivalent to write_flash layout)
# --------------------------------------------------
"$PYTHON_BIN" -m esptool \
  --chip esp32s3 \
  merge_bin \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  -o "$OUTPUT_BIN" \
  0x0        "$BUILD_DIR/bootloader/bootloader.bin" \
  0x8000     "$BUILD_DIR/partition_table/partition-table.bin" \
  0xd000     "$BUILD_DIR/ota_data_initial.bin" \
  0x20000    "$BUILD_DIR/xiaozhi_vn.bin" \
  0xa00000   "$ASSETS_FILE"

echo "----------------------------------------------"
echo "✅ OTA DONE!"
ls -lh "$OTA_BIN"
echo "----------------------------------------------"
echo "✅ MERGE DONE!"
ls -lh "$OUTPUT_BIN"
echo "=============================================="
