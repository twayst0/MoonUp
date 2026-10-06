#!/bin/bash
# Builds bench.exe (console) from the engine sources. Run it under Wine: tools/bench/run.sh ...
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/bench"
mkdir -p "$OUT/obj"
python3 "$ROOT/tools/embed_shaders.py" >/dev/null
CXX=x86_64-w64-mingw32-g++-posix
FLAGS="-std=c++20 -O1 -g -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -w"
SRCS="$ROOT/src/common.cpp $ROOT/src/settings.cpp $ROOT/src/engine_client.cpp $(ls $ROOT/src/engine/*.cpp) $ROOT/tools/bench/bench.cpp $ROOT/tools/bench/fxrun.cpp $ROOT/src/upgraph/upgraph.cpp $ROOT/src/third_party/miniz/miniz_impl.cpp $(ls $ROOT/tools/third_party/reshadefx/*.cpp)"
objs=()
pids=()
for s in $SRCS; do
  o="$OUT/obj/$(basename $s .cpp).o"; objs+=($o)
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ -n "$(find "$ROOT/src" -name '*.h' -newer "$o" | head -1)" ]; then
    ( F="$FLAGS"; case "$s" in *reshadefx*) F="${FLAGS/-std=c++20/-std=c++17} -include share.h";; esac; $CXX $F -c "$s" -o "$o" ) & pids+=($!)
    if [ ${#pids[@]} -ge 2 ]; then wait ${pids[0]}; pids=("${pids[@]:1}"); fi
  fi
done
for p in "${pids[@]}"; do wait $p; done
$CXX -o "$OUT/bench.exe" "${objs[@]}" -static -static-libgcc -static-libstdc++ -municode \
  -ld3d11 -ldxgi -ld2d1 -ldwrite -ldwmapi -lruntimeobject -lole32 -loleaut32 -luuid -lshell32 -lshlwapi \
  -luser32 -lgdi32 -lwinmm -lwindowscodecs -lshcore -luxtheme -lversion -ladvapi32 -lcomctl32 -lpsapi -ld3dcompiler -lwinhttp
echo built $OUT/bench.exe
