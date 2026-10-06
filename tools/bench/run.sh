#!/bin/bash
# Runs bench.exe under Wine (wined3d on Mesa llvmpipe) with a virtual X display.
export WINEDLLOVERRIDES="d3dcompiler_47=n" WINEDEBUG=${WINEDEBUG:--all} WINEPREFIX=${WINEPREFIX:-/tmp/claude-0/wineprefix} DISPLAY=:99
pgrep -x Xvfb >/dev/null || (Xvfb :99 -screen 0 1920x1080x24 >/dev/null 2>&1 &) ; sleep 0.3
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec wine "$ROOT/build/bench/bench.exe" "$@"
