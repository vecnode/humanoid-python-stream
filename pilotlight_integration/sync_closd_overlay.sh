#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OVERLAY_DIR="${SCRIPT_DIR}/closd_overlay"
TARGET_DIR="${ROOT_DIR}/CLoSD"

if [[ ! -d "${OVERLAY_DIR}" ]]; then
  echo "Overlay directory not found: ${OVERLAY_DIR}"
  exit 1
fi

if [[ ! -d "${TARGET_DIR}" ]]; then
  echo "CLoSD directory not found: ${TARGET_DIR}"
  exit 1
fi

count=0
while IFS= read -r -d '' src; do
  rel="${src#${OVERLAY_DIR}/}"
  dst="${TARGET_DIR}/${rel}"
  mkdir -p "$(dirname "${dst}")"
  cp -f "${src}" "${dst}"
  count=$((count + 1))
done < <(find "${OVERLAY_DIR}" -type f -print0)

echo "synced ${count} file(s)"
