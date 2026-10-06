#!/bin/bash
# Cross-compiles MoonUp.exe with mingw-w64 (posix threads, static runtime).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build"
mkdir -p "$OUT/obj"
python3 "$ROOT/tools/embed_shaders.py" >/dev/null
[ "$ROOT/tools/upgraph/MoonUpUpgraph.fx.in" -nt "$ROOT/src/upgraph/upgraph_assets.h" ] && python3 "$ROOT/tools/gen_upgraph.py"
CXX=x86_64-w64-mingw32-g++-posix
WINDRES=x86_64-w64-mingw32-windres
FLAGS="-std=c++20 -O2 -g -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers -Wno-unused-function -Wno-cast-function-type"
SRCS=$(cd "$ROOT/src" && find . -name '*.cpp' | sort)
pids=()
for s in $SRCS; do
  o="$OUT/obj/$(echo $s | sed 's#^\./##; s#/#_#g; s#\.cpp$#.o#')"
  if [ ! -f "$o" ] || [ "$ROOT/src/$s" -nt "$o" ] || [ -n "$(find "$ROOT/src" -name '*.h' -newer "$o" | head -1)" ]; then
    ( $CXX $FLAGS -c "$ROOT/src/$s" -o "$o" ) &
    pids+=($!)
    if [ ${#pids[@]} -ge 2 ]; then wait ${pids[0]}; pids=("${pids[@]:1}"); fi
  fi
done
for p in "${pids[@]}"; do wait $p; done
if [ -f "$ROOT/src/res/app.rc" ]; then
  (cd "$ROOT/src/res" && $WINDRES -O coff app.rc -o "$OUT/obj/app_res.o")
fi
$CXX -o "$OUT/MoonUp.exe" "$OUT"/obj/*.o -static -static-libgcc -static-libstdc++ -mwindows -municode \
  -ld3d11 -ldxgi -ld2d1 -ldwrite -ldwmapi -lruntimeobject -lole32 -loleaut32 -luuid -lshell32 -lshlwapi \
  -luser32 -lgdi32 -lwinmm -lwindowscodecs -lshcore -luxtheme -lversion -ladvapi32 -lcomctl32 -lpsapi -lwinhttp
cp "$OUT/MoonUp.exe" "$OUT/MoonUp.debug.exe"; x86_64-w64-mingw32-strip "$OUT/MoonUp.exe"
ls -la "$OUT/MoonUp.exe"
