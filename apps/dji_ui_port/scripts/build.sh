#!/bin/sh
# Build the MaixCAM2 action-camera app (cross compile, aarch64).
set -e
cd "$(dirname "$0")/.."
: "${MAIXCDK_PATH:=$(dirname "$(pwd)")/MaixCDK}"
export MAIXCDK_PATH
export PATH="$HOME/.local/bin:$PATH"
maixcdk build -p maixcam2 -G Ninja
echo "binary: $(pwd)/build/dji_ui"
