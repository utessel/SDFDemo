# SDF-Demo: Ultra-Compact Signed Distance Field Font Engine

A lightweight, high-performance **Signed Distance Field (SDF)** font generation and rendering toolkit designed for microcontrollers and embedded systems (such as Pebble smartwatches, ARM Cortex-M, and low-power LCD displays).

This engine generates **4-bit Signed Distance Fields (SDFs)** from TrueType fonts (`.ttf`). A complete 104-character font—including ASCII, punctuation, and German umlauts—occupies **less than 8 KB of flash memory**, enabling continuous scaling, arbitrary rotation, sub-pixel anti-aliasing, and dynamic weight adjustments in real time on resource-constrained hardware.

### Direct Sideload
Download the ready-to-use [`PebbleDemo.pbw`](https://github.com/utessel/SDFDemo/releases/latest) and open it on your phone with the Pebble app.

---

## Key Features

* **Analytical Cardano Bézier Solver:** 
  The offline Python generator (`ttf2sdf.py`) extracts TrueType quadratic Bézier curves and analytically computes exact point-to-curve distances using Cardano's cubic polynomial solution.
* **4-Bit Nibble Packing per Glyph:** 
  To eliminate expensive per-pixel bit shifts in embedded rasterizer loops, glyphs are stored in shared byte arrays where each glyph occupies either the low nibble (`0x0F`) or high nibble (`0xF0`) across its entire bitmap.
* **Tight Bounding-Box Trimming:** 
  Zero-distance outer borders are automatically stripped at build time, shrinking glyph footprints by 25–30%.
* **Modular Clean Architecture (C++14):**
  * `Matrix2D.hpp` (Header-Only): 2D affine transformations (`Translation`, `Rotation`, `Scaling`, inversion, operator overloading).
  * `TextLayout`: UTF-8 decoding, advance metrics, aggregate bounding boxes, and optical centering (computed once upfront).
  * `SdfRenderer`: Mathematical rasterizer with bilinear sampling, screen-space smoothstep anti-aliasing, and alpha-blending.
* **Interactive Terminal Demo:** 
  Visualizes SDF fonts directly inside the terminal at 30+ FPS using Unicode Half-Blocks (`▀`) and 24-bit TrueColor (`\033[38;2;...m`), fully adapting to the terminal window size.
* **Pebble-Optimized:** 
  Supports optional 4-level quantization (0%, 33%, 66%, 100%) matching the 64-color / 4-shade memory LCD of Pebble Time and Pebble Time 2.

---

## Project Structure

```
SDF-Demo/
├── SDFLib/            # Core SDF engine and font generator
│   ├── Matrix2D.hpp   # Header-only 2D affine transformation matrix (Public Domain)
│   ├── TextLayout.*   # UTF-8 text metrics and layout geometry (GPLv3)
│   ├── SdfRenderer.*  # Matrix-driven SDF rasterizer & framebuffer blitter (GPLv3)
│   ├── ttf2sdf.py     # TrueType to 4-bit SDF generator (Cardano solver)
│   └── sdf_config.ini # Font generation configuration
├── TerminalDemo/      # Terminal viewer application
│   └── view_sdf.cpp   # Animated terminal viewer using Half-Blocks & TrueColor
├── PebbleDemo/        # Complete Pebble OS smartwatch application
│   ├── package.json   # Pebble app configuration
│   ├── wscript        # Waf build script with C++ cross-compiler setup
│   └── src/main.cpp   # Pebble OS app with rotating & scaling text animation
├── Makefile           # Top-level build for Terminal Demo & font regeneration
├── LICENSE            # GNU General Public License v3.0
└── README.md
```

---

## Quick Start

### Prerequisites
* A C++14 compiler (`g++` or `clang++`)
* `make`
* Python 3 with `fontTools`:
  ```bash
  pip install fonttools
  ```

### Build & Run Terminal Demo
Build from the root directory with `make`:

```bash
make
./TerminalDemo/view_sdf
```

To animate custom text:
```bash
./TerminalDemo/view_sdf "Pebble SDF Engine"
```
or with special characters / umlauts:
```bash
./TerminalDemo/view_sdf "Zehn nach Zehn"
```

To inspect single glyphs and view the raw 4-bit distance matrix:
```bash
./TerminalDemo/view_sdf ä
```

---

## Pebble Smartwatch Demo (`PebbleDemo`)

The `PebbleDemo` subproject compiles a native Pebble OS watchapp running on ARM Cortex-M hardware (supporting all Pebble platforms: `basalt`, `chalk`, `emery`, `diorite`, and `aplite`).

### Building and Running on Pebble Emulator
```bash
cd PebbleDemo
pebble build
pebble install --emulator basalt
```

### Sideloading to a Physical Pebble Watch
```bash
cd PebbleDemo
pebble install --phone <PHONE_IP_OR_WATCH_IP>
```

---

## Performance & Architecture

### Real-Time Embedded Optimizations

To achieve smooth rendering on a 100 MHz ARM Cortex-M4 microcontroller without floating-point overhead, the rasterizer incorporates several key architectural optimizations:

1. **Affine Forward Differencing:**
   Because 2D affine transformations are linear, moving horizontally along a scanline ($sx \to sx + 1$) changes font coordinates by a constant delta $(du, dv)$. Per-pixel 2D matrix-vector multiplications are replaced with single-cycle incremental additions (`cur_u += du_fp; cur_v += dv_fp;`).
2. **Texel Fast-Path:**
   Interior texels ($0 \le x_0 < w-1, 0 \le y_0 < h-1$) skip bounds-clamping branches and index directly into consecutive memory rows (`r0[x0]`, `r0[x0+1]`, `r1[x0]`, `r1[x0+1]`).
3. **Discrete Threshold Shading:**
   Instead of evaluating a floating-point Hermite polynomial (`smoothstep`) across the entire raster domain, precalculated distance thresholds classify pixels with integer comparisons. Interior glyph pixels write solid color immediately.
4. **16.16 Fixed-Point & Unshifted Nibble Masking:**
   The entire rasterizer inner loop operates exclusively in 16.16 integer fixed-point math. Sub-pixel coordinates `cur_u`, `cur_v` and floor/fractions are computed with arithmetic shifts and bitmasks (`tx >> 16`, `(tx >> 8) & 0xFF`). Unshifted nibble bytes (`row[x] & mask`) are interpolated directly without per-pixel bit shifts, and compared against pre-scaled integer thresholds. The hardware FPU remains completely idle during rasterization, minimizing CPU power draw and achieving render times of **1–5 ms**.

### Measured Hardware Benchmarks (Pebble Time 2 / `emery`)

Benchmarked directly on physical hardware (`emery`, STM32F4 Cortex-M4 @ 100 MHz):

| Optimization Stage | Small/Medium Text (Frame Time) | Large / Rotated (Frame Time) | FPS |
| :--- | :--- | :--- | :--- |
| **Naive Implementation** (Per-pixel 2D matrix + float Smoothstep) | **65 ms** (Raster: 64 ms) | **135 ms** (Raster: 135 ms) | **7 – 14 FPS** *(stuttering)* |
| **+ Forward Differencing & Fast Bilinear** | **28 ms** (Raster: 27 ms) | **67 ms** (Raster: 66 ms) | **14 – 26 FPS** |
| **+ Discrete Threshold Shading & Texel Fast-Path** | **21 – 24 ms** (Raster: 21 ms) | **45 – 53 ms** (Raster: 44 ms) | **20 – 27 FPS** |
| **+ 16.16 Fixed-Point & Unshifted Nibble Masking** | **2 – 3 ms** (Raster: **1 – 2 ms**) | **4 – 6 ms** (Raster: **4 – 5 ms**) | **27 FPS** *(33ms timer cap!)* |

### Enabling the Built-in Profiler

In `PebbleDemo/src/main.cpp`, set:
```cpp
#define ENABLE_PROFILING 1
```
This enables rolling 1-second performance metrics output via `APP_LOG`. View the live hardware telemetry over Bluetooth with:
```bash
pebble logs --phone <PHONE_IP_OR_WATCH_IP>
```
When set to `0` (default), all timing and logging code is stripped out at compile time, saving RAM and eliminating runtime overhead.

---

## Configuration (`sdf_config.ini`)

You can customize font parameters, export names, and character sets directly in `sdf_config.ini`:

```ini
[font]
ttf_path = /usr/share/fonts/truetype/noto/NotoSans-Regular.ttf
em_height = 16
spread_pixels = 1.5

[output]
c_header = sdf_font.h
c_source = sdf_font.c
font_name = sdf_font

[characters]
include_ascii = true
extra_characters = äöüÄÖÜß°€
```

When you edit `sdf_config.ini`, running `make` will automatically regenerate the font tables before compiling the C++ sources.

---

## Mathematical Background

### Distance Calculation
Each glyph's distance field is computed directly against the TrueType quadratic Bézier spline segments $B(t) = (1-t)^2 P_0 + 2(1-t)t P_1 + t^2 P_2$:

1. All contour vertices are tested as discrete points.
2. For line segments, the orthogonal projection is evaluated strictly within $0 < t < 1$.
3. For quadratic Béziers, the derivative $\frac{d}{dt} \|B(t) - Q\|^2 = 0$ yields a cubic polynomial $c_3 t^3 + c_2 t^2 + c_1 t + c_0 = 0$, solved analytically via Cardano's formula for roots in $(0, 1)$.
4. Inside/outside sign is determined via even-odd ray casting.

### Screen-Space Anti-Aliasing (Smoothstep)
The signed distance is sampled bilinearly and transformed to target screen pixels:
$$\text{dist}_{\text{screen}} = (\text{dist}_{\text{font}} + \text{bias}) \times \text{scale}$$

Coverage $\alpha$ is derived using a Hermite smoothstep across a configurable filter width (typically 1.0 screen pixel):
$$\alpha = \text{smoothstep}\left(+\frac{W}{2}, -\frac{W}{2}, \text{dist}_{\text{screen}}\right)$$

The resulting $\alpha$ is alpha-blended into the destination buffer:
$$C_{\text{result}} = \alpha \cdot C_{\text{text}} + (1 - \alpha) \cdot C_{\text{buffer}}$$

---

## License & Alternative Licensing

* **Core Engine & Tools:** Licensed under the [GNU General Public License v3.0](LICENSE) (GPLv3).
* **Matrix2D.hpp:** Released into the **Public Domain** (or CC0 / Unlicense at your option) for unrestricted use in any project.
* **Alternative Licensing:**
  If you wish to integrate this engine or its components into proprietary, commercial, or non-GPL projects, please contact the author to negotiate an alternative license.

Copyright (C) 2026 Uli Tessel.
