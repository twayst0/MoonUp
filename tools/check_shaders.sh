#!/bin/bash
# Validates every HLSL entry point with glslang's HLSL front end (syntax + semantics).
cd "$(dirname "$0")/../src/engine/shaders"
fail=0
check() { # file entry stage
  cat common.hlsli "$1" > /tmp/_sw_check.hlsl
  out=$(glslangValidator -D -V -S $3 -e $2 /tmp/_sw_check.hlsl -o /tmp/_sw_check.spv 2>&1)
  if [ $? -ne 0 ]; then echo "FAIL $1:$2"; echo "$out" | grep -v "^/tmp" | head -20; fail=1; else echo "ok   $1:$2"; fi
}
for e in CSCopy CSBilinear CSNearest CSPixel CSBicubic CSLanczos CSEdge; do check upscale.hlsl $e comp; done
for e in CSSharpen CSDown4 CSBlur CSVision; do check post.hlsl $e comp; done
for e in CSLuma CSDown2 CSFlow CSPropagate CSRefine CSMedian CSScene; do check flow.hlsl $e comp; done
for e in CSStatic CSSelect CSCompose; do check interp.hlsl $e comp; done
for e in CSDownIn CSDownHalf CSConv CSStructIn CSStructMid CSStructOut CSGlobal CSFuse CSStructBlend CSApply; do check render.hlsl $e comp; done
for e in CSEasu CSRcas; do check fsr.hlsl $e comp; done
check nis.hlsl CSNis comp
for e in CSConvIn CSConvMid CSConvOut; do check neural.hlsl $e comp; done
check present.hlsl VSFull vert; check present.hlsl PSFrame frag; check present.hlsl VSQuad vert; check present.hlsl PSQuad frag
exit $fail
