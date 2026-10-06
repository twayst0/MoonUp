# Third-party notices

MoonUp's own code is licensed under the PolyForm Noncommercial License 1.0.0 (see `LICENSE`).
The components below keep their own licenses; those licenses apply to them, not the MoonUp license.

## Included in this repository / in the release package

| Component | Where | License |
|---|---|---|
| nlohmann/json | `src/third_party/json.hpp` | MIT, © 2013-2022 Niels Lohmann |
| miniz | `src/third_party/miniz` | MIT (see `src/third_party/miniz/LICENSE`) |
| AMD FidelityFX Super Resolution 1 (EASU + RCAS), ported to HLSL | `src/engine/shaders/fsr.hlsl` | MIT, © 2021 Advanced Micro Devices, Inc. |
| NVIDIA Image Scaling SDK v1.0.3 | `src/engine/shaders/nis.hlsl`, `src/third_party/nis/NIS_Config.h` | MIT, © 2022 NVIDIA CORPORATION & AFFILIATES |
| ArtCNN C4F16 weights and architecture | `src/engine/neural_weights.h` (`kArtCNN`), MoonUp Neural v3 is fine-tuned from it | MIT, © 2024 João Chrisóstomo — https://github.com/Artoriuz/ArtCNN |
| ReShade FX compiler (test tool only) | `tools/third_party/reshadefx` | BSD 3-Clause, © 2014 Patrick Mours (see `LICENSE.md` there) |
| Inter, JetBrains Mono fonts | `ui/fonts` | SIL Open Font License 1.1 (`OFL-*.txt`) |
| Microsoft Edge WebView2 loader | `redist/WebView2Loader.dll` | Microsoft WebView2 SDK license (redistributable) |

Ideas and references: AMD FidelityFX CAS (contrast-adaptive sharpening, MIT), AMD FidelityFX SDK (FSR 3
frame interpolation / optical flow, MIT), Gharbi et al. "Deep Bilateral Learning for Real-Time Image
Enhancement" (2017), Yu & Koltun "Multi-Scale Context Aggregation by Dilated Convolutions" (2016),
Chen et al. "Fast Image Processing with Fully-Convolutional Networks" (2017).

The Upgraph install flow (game scan, journaled install/restore, DLSS 5 routes) is adapted from
**DLSS5-Swapper** by Rakan Alkhaldi — https://github.com/rakanki911/DLSS5-Swapper — MIT License:

> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
> associated documentation files (the "Software"), to deal in the Software without restriction,
> including without limitation the rights to use, copy, modify, merge, publish, distribute,
> sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions: The above copyright notice and this
> permission notice shall be included in all copies or substantial portions of the Software.
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

## Downloaded at run time (never bundled with MoonUp)

Upgraph downloads these from their own projects onto the user's computer when the user asks for them.
They are governed by their own licenses and terms:

- ReShade (standard and add-on builds) — BSD 3-Clause — https://reshade.me
- OptiScaler — GPL-3.0 — https://github.com/optiscaler/OptiScaler
- DLSS5-Feeder (MIT), RenoDX DLSS 5 add-on, LumeniteFX, dgVoodoo2 — their own licenses
- NVIDIA DLSS runtime files — NVIDIA's license terms

MoonUp is not affiliated with, endorsed by or sponsored by NVIDIA, AMD, Intel, Microsoft or the
projects above. All trademarks belong to their owners.
