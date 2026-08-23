#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SDKCONFIG_TEMPLATE="$PROJECT_DIR/sdkconfig.jiuchuan"
SDKCONFIG_FILE="$PROJECT_DIR/sdkconfig"
PORT_DEFAULT="/dev/cu.usbmodem101"

usage() {
  cat <<USAGE
Usage:
  $(basename "$0") build [port]
  $(basename "$0") flash [port]
  $(basename "$0") monitor [port]
  $(basename "$0") all [port]      # build + flash + monitor
  $(basename "$0") reconfig [port] # apply Jiuchuan sdkconfig + reconfigure
USAGE
}

ACTION="${1:-all}"
PORT="${2:-$PORT_DEFAULT}"

if [[ ! -f "$SDKCONFIG_TEMPLATE" ]]; then
  echo "Missing template: $SDKCONFIG_TEMPLATE"
  exit 1
fi

if [[ -f "/Users/tamtran/esp-idf-5.5.1/export.sh" ]]; then
  # shellcheck source=/dev/null
  source /Users/tamtran/esp-idf-5.5.1/export.sh >/dev/null 2>&1
elif [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
  # shellcheck source=/dev/null
  source "$IDF_PATH/export.sh" >/dev/null 2>&1
else
  echo "ESP-IDF export.sh not found. Set IDF_PATH or install ESP-IDF."
  exit 1
fi

if ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py not found after sourcing ESP-IDF environment"
  exit 1
fi

apply_jiuchuan_sdkconfig() {
  cp "$SDKCONFIG_TEMPLATE" "$SDKCONFIG_FILE"
}

run_reconfig() {
  apply_jiuchuan_sdkconfig
  (cd "$PROJECT_DIR" && idf.py reconfigure)
}

case "$ACTION" in
  build)
    run_reconfig
    (cd "$PROJECT_DIR" && idf.py build)
    ;;
  flash)
    run_reconfig
    (cd "$PROJECT_DIR" && idf.py -p "$PORT" build flash)
    ;;
  monitor)
    (cd "$PROJECT_DIR" && idf.py -p "$PORT" monitor)
    ;;
  all)
    run_reconfig
    (cd "$PROJECT_DIR" && idf.py -p "$PORT" build flash monitor)
    ;;
  reconfig)
    run_reconfig
    ;;
  *)
    usage
    exit 1
    ;;
esac
