#!/bin/bash
# Run the PilotLight bridge viewer standalone
# This starts the viewer binary if it exists

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INTEGRATION_ROOT="$(dirname "$SCRIPT_DIR")"
VENDOR_DIR="$INTEGRATION_ROOT/pilotlight_vendor"
BINARY="$VENDOR_DIR/out/pilot_light"

if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found: $BINARY"
    echo "Run ./build_bridge_viewer.sh first"
    exit 1
fi

echo "Starting PilotLight viewer: $BINARY"
exec "$BINARY"
