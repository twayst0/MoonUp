#!/bin/bash
# Precompiles every shader with Microsoft's d3dcompiler_47 (under Wine) and embeds the DXBC blobs
# into src/engine/shaders_bin.h, so MoonUp never compiles shaders on the user's machine.
# Run before tools/build.sh whenever a shader changes.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$ROOT/tools/embed_shaders.py" >/dev/null
python3 "$ROOT/tools/embed_shader_blobs.py" --empty >/dev/null   # the compiler tool itself needs no blobs
bash "$ROOT/tools/bench/build_bench.sh" >/dev/null
rm -rf "$ROOT/build/shader_bin" && mkdir -p "$ROOT/build/shader_bin"
WINEPREFIX=${WINEPREFIX:-/tmp/claude-0/wineprefix} wineserver -k 2>/dev/null || true
"$ROOT/tools/bench/run.sh" compileall "Z:${ROOT//\//\\}\\build\\shader_bin" | tail -3
python3 "$ROOT/tools/embed_shader_blobs.py"
