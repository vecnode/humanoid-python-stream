#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ISAACGYM_BINDINGS_DIR="${ROOT_DIR}/isaacgym/python/isaacgym/_bindings/linux-x86_64"

BUILD_VIEWER=0
KEEP_VIEWER=0
DEBUG_HML=0
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
    --debug-hml)
      DEBUG_HML=1
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

VIEWER_DIR="${SCRIPT_DIR}/pilotlight_vendor/out"
VIEWER_BIN="${VIEWER_DIR}/pilot_light"
VIEWER_PID=""

if [[ -x "${VIEWER_BIN}" ]]; then
  (
    cd "${VIEWER_DIR}"
    LD_LIBRARY_PATH="${VIEWER_DIR}:${LD_LIBRARY_PATH:-}" ./pilot_light
  ) > /tmp/pilotlight_viewer.log 2>&1 &
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
  if [[ -n "${CONDA_PREFIX:-}" && -d "${CONDA_PREFIX}/lib" ]]; then
    export LD_LIBRARY_PATH="${CONDA_PREFIX}/lib:${LD_LIBRARY_PATH:-}"
  fi

  if [[ -d "${ISAACGYM_BINDINGS_DIR}" ]]; then
    export LD_LIBRARY_PATH="${ISAACGYM_BINDINGS_DIR}:${LD_LIBRARY_PATH:-}"
  fi

  # Use the known-good runtime defaults for this integration.
  DEFAULT_ARGS=(
    "learning=im_big"
    "robot=smpl_humanoid"
    "epoch=-1"
    "test=True"
    "no_virtual_display=True"
    "headless=False"
    "env.num_envs=1"
    "env=closd_t2m"
    "exp_name=CLoSD_t2m_finetune"
    "env.dip.debug_hml=False"
    "env.dip.save_debug_mp4=False"
    "env.dip.planning_horizon_multiplyer=4"
    "env.viewer.backend=pilotlight"
    "env.viewer.bridge_enabled=True"
    "env.viewer.bridge_host=127.0.0.1"
    "env.viewer.bridge_port=45678"
    "env.viewer.bridge_env_idx=0"
    "env.viewer.bridge_publish_every_n_steps=1"
    "env.viewer.bridge_include_rot=True"
    "env.viewer.bridge_include_predicted=True"
  )

  if [[ ${DEBUG_HML} -eq 1 ]]; then
    DEFAULT_ARGS+=("env.dip.debug_hml=True")
  fi

  cd "${ROOT_DIR}/CLoSD"
  python closd/run.py "${DEFAULT_ARGS[@]}" "${CLOSD_ARGS[@]}"
else
  echo "missing CLoSD entrypoint: ${ROOT_DIR}/CLoSD/closd/run.py"
  exit 1
fi
