#!/bin/sh
# Render art/preview.html to art/previewimage.png (1024x1024) with headless Edge.
set -e
cd "$(dirname "$0")"
EDGE="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
"$EDGE" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
  --window-size=1024,1024 --virtual-time-budget=2000 \
  --screenshot="$(cygpath -w "$PWD/previewimage.png")" "file:///$(cygpath -m "$PWD/preview.html")"
echo "[render] art/previewimage.png"
