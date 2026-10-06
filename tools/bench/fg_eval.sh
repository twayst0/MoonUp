#!/bin/bash
# Frame generation quality on all synthetic scenes (PSNR of the generated t=0.5 frame vs truth).
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SC=${SCENES:-/tmp/claude-0/scenes}; OUT=${OUT:-/tmp/claude-0/gen}; Q=${Q:-1}
export STATWAYS_SHADER_DIR='Z:'$(echo "$ROOT/src/engine/shaders" | sed 's#/#\\#g')
tot=0; for s in ${LIST:-parallax fps orbit fast}; do mkdir -p $OUT/$s
  r=$("$ROOT/tools/bench/run.sh" interp 'Z:'$(echo $SC/$s | sed 's#/#\\#g') 'Z:'$(echo $OUT/$s | sed 's#/#\\#g') $Q 2>&1 | grep -E "MEAN|rror|FAIL" )
  echo "$s: $r"; done
