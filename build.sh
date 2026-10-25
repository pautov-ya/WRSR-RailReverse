#!/bin/sh
# Build railreverse.dll and deploy it as an RML Development project.
#   ./build.sh          build + deploy
#   ./build.sh nodeploy build only
set -e
cd "$(dirname "$0")"

CXX=C:/WRSR-Dev/tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++
GAME=D:/SteamLibrary/steamapps/common/SovietRepublic
PROJECT="$GAME/media_soviet/workshop_wip/3823129138"

mkdir -p build
"$CXX" -O2 -Wall -Wno-unused-function -static -shared -o build/railreverse.dll src/railreverse.cpp -luser32 -lshlwapi
cp -f src/railreverse.ini build/railreverse.ini
echo "[build] build/railreverse.dll"

[ "$1" = "nodeploy" ] && exit 0

mkdir -p "$PROJECT/plugins"
# The game writes $ITEM_ID/$OWNER_ID into the project's workshopconfig.ini on the
# first upload. Keep them, or the next upload would publish a new Workshop item.
ids=$(grep -E '^\$(ITEM_ID|OWNER_ID)' "$PROJECT/workshopconfig.ini" 2>/dev/null || true)
if [ -n "$ids" ] && ! grep -q '^\$ITEM_ID' workshopconfig.ini; then
  { printf '%s\n\n' "$ids"; cat workshopconfig.ini; } > "$PROJECT/workshopconfig.ini.new"
  mv -f "$PROJECT/workshopconfig.ini.new" "$PROJECT/workshopconfig.ini"
else
  cp -f workshopconfig.ini "$PROJECT/workshopconfig.ini"
fi
cp -f art/previewimage.png "$PROJECT/previewimage.png"
# the Scripts menu buttons sit in the item's root, the icons next to the dll
cp -f scripts/* "$PROJECT/"
cp -f build/railreverse.dll build/railreverse.ini art/button_*.png "$PROJECT/plugins/"
echo "[deploy] $PROJECT"
