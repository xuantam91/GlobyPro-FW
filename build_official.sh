#!/usr/bin/env bash
set -euo pipefail

# Make sure esp-idf is active
if ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py not found. Please run: source ~/esp/esp-idf/export.sh or export.sh first"
  exit 1
fi

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_DIR"

PYTHON_BIN="python3"
OUT_DIR="releases"
mkdir -p "$OUT_DIR"

build_release() {
  local board="$1"
  local theme_val="$2" # 0 = Pink (Rose), 1 = Orange (Beige)
  local asset_file="$3"
  local output_name="$4" # e.g. "Jiuchuan-2.1.6-opt-V1-Rose"
  
  local build_dir="build_${board}"
  local sdkconfig="sdkconfig.${board}"
  
  echo "=============================================="
  echo "Building: $output_name"
  echo "Board   : $board"
  echo "Theme   : $theme_val"
  echo "Asset   : $asset_file"
  echo "=============================================="
  
  # Run build with DEFAULT_THEME_VAL and default LCD driver 2
  idf.py -B "$build_dir" -D "SDKCONFIG=$sdkconfig" -D "DEFAULT_THEME_VAL=$theme_val" -D "JIUCHUAN_DEFAULT_LCD_DRIVER=2" build
  
  local bootloader="$build_dir/bootloader/bootloader.bin"
  local partition="$build_dir/partition_table/partition-table.bin"
  local ota_data="$build_dir/ota_data_initial.bin"
  local app_bin="$build_dir/xiaozhi_vn.bin"
  
  for f in "$bootloader" "$partition" "$ota_data" "$app_bin" "$asset_file"; do
    if [ ! -f "$f" ]; then
      echo "Error: Missing file $f"
      exit 1
    fi
  done
  
  # Copy OTA bin
  cp "$app_bin" "$OUT_DIR/${output_name}-OTA.bin"
  
  # Merge full flash bin
  "$PYTHON_BIN" -m esptool --chip esp32s3 merge_bin \
    --flash_mode dio \
    --flash_freq 80m \
    --flash_size 16MB \
    -o "$OUT_DIR/${output_name}.bin" \
    0x0       "$bootloader" \
    0x8000    "$partition" \
    0xd000    "$ota_data" \
    0x20000   "$app_bin" \
    0xa00000  "$asset_file"
    
  echo "Successfully generated:"
  echo "- $OUT_DIR/${output_name}-OTA.bin"
  echo "- $OUT_DIR/${output_name}.bin"
  echo "----------------------------------------------"
}

# 1. V1 Rose: Board=jiuchuan, Theme=0 (Pink/Rose), Asset=assets_bin/Assets-Rose-HiTelly.bin
build_release "jiuchuan" 0 "assets_bin/Assets-Rose-HiTelly.bin" "Jiuchuan-2.1.6-opt-V1-Rose"

# 2. V1 Beige: Board=jiuchuan, Theme=1 (Orange/Beige), Asset=assets_bin/Assets-Beige-HiTelly.bin
build_release "jiuchuan" 1 "assets_bin/Assets-Beige-HiTelly.bin" "Jiuchuan-2.1.6-opt-V1-Beige"

# 3. V2 Rose: Board=jiuchuan_v2, Theme=0 (Pink/Rose), Asset=assets_bin/Assets-Rose-HiTelly.bin
build_release "jiuchuan_v2" 0 "assets_bin/Assets-Rose-HiTelly.bin" "Jiuchuan-V2-2.1.6-opt-V2-Rose"

# 4. V2 Purple: Board=jiuchuan_v2, Theme=0 (Pink/Rose), Asset=assets_bin/Assets-Purple-HiTelly.bin
build_release "jiuchuan_v2" 0 "assets_bin/Assets-Purple-HiTelly.bin" "Jiuchuan-V2-2.1.6-opt-V2-Purple"

echo "ALL BUILDS COMPLETED SUCCESSFULLY!"
