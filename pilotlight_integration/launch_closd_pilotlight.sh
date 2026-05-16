#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_VIEWER=0
KEEP_VIEWER=0
CLOSD_ARGS=()

while (($#)); do
  case "$1" in
    --build-viewer)
      BUILD_VIEWER=1
      shift
      ;;
    --keep-viewer)
      KEEP_VIEWER=1
      shift
      ;;
    *)
      CLOSD_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ ${BUILD_VIEWER} -eq 1 ]]; then
  "${SCRIPT_DIR}/pilotlight_app/build_bridge_viewer.sh"
fi

"${SCRIPT_DIR}/sync_closd_overlay.sh"

VIEWER_BIN="${SCRIPT_DIR}/pilotlight_vendor/out/pilot_light"
VIEWER_PID=""

if [[ -x "${VIEWER_BIN}" ]]; then
  "${VIEWER_BIN}" > /tmp/pilotlight_viewer.log 2>&1 &
  VIEWER_PID=$!
  echo "viewer started pid=${VIEWER_PID} log=/tmp/pilotlight_viewer.log"
else
  echo "viewer binary not found at ${VIEWER_BIN}"
  echo "run with --build-viewer or build manually first"
fi

cleanup() {
  if [[ -n "${VIEWER_PID}" && ${KEEP_VIEWER} -eq 0 ]]; then
    kill "${VIEWER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

if [[ -f "${ROOT_DIR}/CLoSD/closd/run.py" ]]; then
  cd "${ROOT_DIR}/CLoSD"
  python closd/run.py "${CLOSD_ARGS[@]}"
else
  echo "missing CLoSD entrypoint: ${ROOT_DIR}/CLoSD/closd/run.py"
  exit 1
fi
