# SDF-Demo: Ultra-Compact Signed Distance Field Font Engine

A lightweight, high-performance **Signed Distance Field (SDF)** font generation and rendering toolkit designed for microcontrollers and embedded systems (such as Pebble smartwatches, ARM Cortex-M, and low-power LCD displays).

Instead of storing dozens of megabytes of pre-rasterized bitmap fonts for different sizes and rotations, this engine generates **4-bit SDFs** from TrueType fonts (`.ttf`). A complete 104-character font—including ASCII, punctuation, and German umlauts—occupies **less than 8 KB of flash memory**, while enabling continuous scaling, arbitrary rotation, and sub-pixel anti-aliasing in real time.

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
├── Matrix2D.hpp       # Header-only 2D affine transformation matrix
├── TextLayout.hpp/cpp # UTF-8 text metrics and layout geometry
├── SdfRenderer.hpp/cpp# Generic matrix-driven SDF rasterizer with alpha blending
├── view_sdf.cpp       # Animated terminal viewer and inspection tool
├── ttf2sdf.py         # TrueType to 4-bit SDF generator (Cardano solver)
├── sdf_config.ini     # Configuration: font path, EM height, spread, characters
├── Makefile           # Automated build with dependency tracking on config & generator
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

### Build & Run
Simply clone the repository and build with `make`:

```bash
make
./view_sdf
```

To animate custom text:
```bash
./view_sdf "Pebble SDF Engine"
```
or with special characters / umlauts:
```bash
./view_sdf "Zehn nach Zehn"
```

To inspect single glyphs and view the raw 4-bit distance matrix:
```bash
./view_sdf ä
```

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
