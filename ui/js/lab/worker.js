// Runs the CPU algorithm ports off the UI thread.
import { run, sharpen, fsrRcas, vision } from "./algos.js";
import { neuralRender } from "./render.js";

let renderWeights = null;
function loadRender() {
  if (!renderWeights)
    renderWeights = import("../render_weights.js").then((m) => ({ flat: new Float32Array(m.default), off: m.OFFSETS })).catch(() => null);
  return renderWeights;
}

self.onmessage = async (e) => {
  const { id, op, src, W, H, name, sharp, visionParams, renderParams, weights } = e.data;
  let out;
  if (op === "vision") out = vision(src, visionParams);
  else if (op === "render") {
    const rw = await loadRender();
    out = rw ? neuralRender(src, renderParams, rw.flat, rw.off) : { w: src.w, h: src.h, data: new Float32Array(src.data) };
    if (visionParams && visionParams.enabled) out = vision(out, visionParams);
  } else {
    out = run(name, src, W, H, { weights });
    if (sharp > 0.01) out = name === "fsr" ? fsrRcas(out, sharp) : sharpen(out, sharp);
  }
  self.postMessage({ id, out }, [out.data.buffer]);
};
