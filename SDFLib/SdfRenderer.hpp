/*
 * Copyright (C) 2026 Uli Tessel
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * Alternative Licensing:
 * If you wish to use this software or components under terms other than
 * the GNU GPLv3, please contact the author to request an alternative license.
 */

#pragma once

#include <cstdint>
#include "Matrix2D.hpp"
#include "TextLayout.hpp"

struct ColorRGB
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    ColorRGB() = default;
    ColorRGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
};

struct RenderStats
{
    uint32_t pixels_scanned = 0;
    uint32_t pixels_sampled = 0;
    uint32_t pixels_written = 0;
};

class SdfRenderer
{
public:
    /* Render layout text with 2D transformation matrix and alpha blending into buffer.
     * opacity scales the coverage of the whole glyph (1.0 = opaque, 0.0 = invisible); it
     * multiplies the anti-aliasing alpha and therefore keeps the edge position unchanged.
     */
    static void DrawText(ColorRGB *buffer, int screen_w, int screen_h,
                         const TextLayout &layout, const Matrix2D &matrix,
                         ColorRGB color, float opacity = 1.0f,
                         float filter_width_px = 1.0f,
                         float weight_bias_px = 0.0f,
                         RenderStats *stats = nullptr);

    /* Direct 8-bit framebuffer renderer (Pebble GBitmapFormat8Bit: Basalt, Chalk, Emery) */
    static void DrawToFramebuffer8Bit(uint8_t *fb_data, int screen_w, int screen_h, int row_bytes,
                                      const TextLayout &layout, const Matrix2D &matrix,
                                      float filter_width_px = 1.0f, float weight_bias_px = 0.0f,
                                      RenderStats *stats = nullptr);

private:
    static float SampleSdfBilinear(const SdfFont *font, const SdfGlyph *glyph, float u, float v);
    static float Smoothstep(float edge0, float edge1, float x);
    static void BlendPixel(ColorRGB *dst, ColorRGB src, float alpha, bool pebble_shades);
};
