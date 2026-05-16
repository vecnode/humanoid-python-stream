#!/bin/bash
# Build PilotLight bridge viewer with app_bridge.c integration
# This script syncs the canonical app source to vendor and builds

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INTEGRATION_ROOT="$(dirname "$SCRIPT_DIR")"
ROOT_DIR="$(dirname "$INTEGRATION_ROOT")"
VENDOR_DIR="$INTEGRATION_ROOT/pilotlight_vendor"

# Paths
APP_BRIDGE_SRC="$SCRIPT_DIR/src/app_bridge.c"
APP_BRIDGE_DST="$VENDOR_DIR/internal/sandbox/app_bridge.c"

echo "=== PilotLight Bridge Viewer Build ==="
echo "Root: $ROOT_DIR"
echo "App source: $APP_BRIDGE_SRC"
echo "Vendor dest: $APP_BRIDGE_DST"

# Verify source exists
if [ ! -f "$APP_BRIDGE_SRC" ]; then
    echo "ERROR: App source not found: $APP_BRIDGE_SRC"
    exit 1
fi

# Sync canonical app source to vendor before building
echo "Syncing app_bridge.c to vendor..."
mkdir -p "$(dirname "$APP_BRIDGE_DST")"
cp -f "$APP_BRIDGE_SRC" "$APP_BRIDGE_DST"
echo "Sync complete: $APP_BRIDGE_DST"

# Build vendor
echo "Building PilotLight vendor..."
if [ -f "$VENDOR_DIR/src/build_linux.sh" ]; then
    cd "$VENDOR_DIR/src"
    bash ./build_linux.sh
    cd "$SCRIPT_DIR"
    echo "Build complete. Binary: $VENDOR_DIR/out/pilot_light"
else
    echo "ERROR: Vendor build script not found"
    exit 1
fi
