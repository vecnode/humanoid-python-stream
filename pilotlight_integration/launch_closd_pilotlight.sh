#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ISAACGYM_BINDINGS_DIR="${ROOT_DIR}/isaacgym/python/isaacgym/_bindings/linux-x86_64"

START_TS="$(date +%s)"
PHASE_TS="${START_TS}"
PHASE_ROWS=()

phase_end() {
  local name="$1"
  local now elapsed
  now="$(date +%s)"
  elapsed=$((now - PHASE_TS))
  PHASE_ROWS+=("${name}=${elapsed}s")
  PHASE_TS="${now}"
}

log_warn() {
  echo "[startup][warn] $*"
}

log_error() {
  echo "[startup][error] $*" >&2
}

require_file() {
  local path="$1"
  local label="$2"
  if [[ ! -f "${path}" ]]; then
    log_error "missing required ${label}: ${path}"
    exit 1
  fi
}

require_dir() {
  local path="$1"
  local label="$2"
  if [[ ! -d "${path}" ]]; then
    log_error "missing required ${label}: ${path}"
    exit 1
  fi
}

require_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    log_error "required command not found: ${cmd}"
    exit 1
  fi
}

BUILD_VIEWER=0
KEEP_VIEWER=0
DEBUG_HML=0
AB_SMOOTHING=0
CLOSD_ARGS=()

VIEWER_STATUS="skipped"
CLOSD_STATUS="pending"

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
    --ab-smoothing)
      AB_SMOOTHING=1
      shift
      ;;
    *)
      CLOSD_ARGS+=("$1")
      shift
      ;;
  esac
done

require_cmd "python3"
require_file "${SCRIPT_DIR}/sync_closd_overlay.sh" "sync script"
require_dir "${ROOT_DIR}/CLoSD" "CLoSD root"
require_file "${ROOT_DIR}/CLoSD/closd/run.py" "CLoSD entrypoint"
phase_end "preflight"

if [[ ${BUILD_VIEWER} -eq 1 ]]; then
  "${SCRIPT_DIR}/pilotlight_app/build_bridge_viewer.sh"
  phase_end "build_viewer"
fi

"${SCRIPT_DIR}/sync_closd_overlay.sh"
phase_end "sync_overlay"

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
  if kill -0 "${VIEWER_PID}" >/dev/null 2>&1; then
    VIEWER_STATUS="ready"
  else
    VIEWER_STATUS="failed_optional"
    log_warn "viewer failed to stay alive; continuing without viewer"
  fi
else
  echo "viewer binary not found at ${VIEWER_BIN}"
  echo "run with --build-viewer or build manually first"
  VIEWER_STATUS="missing_optional"
fi
phase_end "viewer_launch"

cleanup() {
  if [[ -n "${VIEWER_PID}" && ${KEEP_VIEWER} -eq 0 ]]; then
    kill "${VIEWER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

echo "[startup] health summary: preflight=ok viewer=${VIEWER_STATUS}"

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
  "env.viewer.bridge_character_wrapper_enabled=True"
  "env.viewer.bridge_character_wrapper_map=assets/character3_smpl24_wrapper.json"
)

if [[ ${DEBUG_HML} -eq 1 ]]; then
  DEFAULT_ARGS+=("env.dip.debug_hml=True")
fi

if [[ ${AB_SMOOTHING} -eq 1 ]]; then
  echo "[startup] enabling A/B smoothing profile"
  DEFAULT_ARGS+=(
    "env.dip.context_switch_prob=0.0"
    "env.dip.transition_blend_frames=8"
    "env.dip.inference_continuity_lock=True"
    "env.dip.inference_continuity_frames=12"
    "env.dip.seam_smoothing_mode=True"
    "env.dip.same_prompt_blend_frames=4"
    "env.dip.full_body_continuity_blend=True"
    "env.dip.prompt_debounce_enabled=True"
    "env.dip.prompt_debounce_replans=2"
  )
fi

phase_end "prepare_closd"

cd "${ROOT_DIR}/CLoSD"
set +e
python3 closd/run.py "${DEFAULT_ARGS[@]}" "${CLOSD_ARGS[@]}"
CLOSD_EXIT=$?
set -e

if [[ ${CLOSD_EXIT} -eq 0 ]]; then
  CLOSD_STATUS="ok"
else
  CLOSD_STATUS="failed"
fi

phase_end "closd_runtime"

TOTAL_ELAPSED=$(( $(date +%s) - START_TS ))
echo "[startup] phase timings: ${PHASE_ROWS[*]}"
echo "[startup] final health: preflight=ok viewer=${VIEWER_STATUS} closd=${CLOSD_STATUS} total=${TOTAL_ELAPSED}s"

exit ${CLOSD_EXIT}
