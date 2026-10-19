#!/bin/sh
# Build railreverse.dll and deploy it as an RML Development project.
#   ./build.sh          build + deploy
#   ./build.sh nodeploy build only
set -e
cd "$(dirname "$0")"

CXX=C:/WRSR-Dev/tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++
GAME=D:/SteamLibrary/steamapps/common/SovietRepublic
PROJECT="$GAME/media_soviet/workshop_wip/9784150"

mkdir -p build
"$CXX" -O2 -Wall -Wno-unused-function -static -shared -o build/railreverse.dll src/railreverse.cpp
cp -f src/railreverse.ini build/railreverse.ini
echo "[build] build/railreverse.dll"

[ "$1" = "nodeploy" ] && exit 0

mkdir -p "$PROJECT/plugins"
cp -f workshopconfig.ini "$PROJECT/workshopconfig.ini"
cp -f build/railreverse.dll build/railreverse.ini "$PROJECT/plugins/"
echo "[deploy] $PROJECT"
