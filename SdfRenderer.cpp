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

#include "SdfRenderer.hpp"
#include <cmath>
#include <algorithm>

float SdfRenderer::SampleSdfBilinear(const SdfFont *font, const SdfGlyph *glyph, float u, float v)
{
    int w = glyph->width;
    int h = glyph->height;

    if (w <= 0 || h <= 0)
    {
        return font ? font->spread : 1.5f;
    }

    float tx = u - 0.5f;
    float ty = v - 0.5f;

    int x0 = static_cast<int>(std::floor(tx));
    int y0 = static_cast<int>(std::floor(ty));
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float fx = tx - static_cast<float>(x0);
    float fy = ty - static_cast<float>(y0);

    float spread = font ? font->spread : 1.5f;
    const uint8_t *data = font ? font->data : nullptr;

    auto get_val = [&](int gx, int gy) -> float
    {
        int cx = std::max(0, std::min(w - 1, gx));
        int cy = std::max(0, std::min(h - 1, gy));
        uint8_t byte = data ? data[glyph->byte_offset + cy * w + cx] : 0;
        uint8_t nibble = (byte >> glyph->shift) & 0x0F;
        return -( (static_cast<float>(nibble) - 7.5f) / 7.5f ) * spread;
    };

    float v00 = get_val(x0, y0);
    float v10 = get_val(x1, y0);
    float v01 = get_val(x0, y1);
    float v11 = get_val(x1, y1);

    float top = (1.0f - fx) * v00 + fx * v10;
    float bot = (1.0f - fx) * v01 + fx * v11;
    return (1.0f - fy) * top + fy * bot;
}

float SdfRenderer::Smoothstep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    t = std::max(0.0f, std::min(1.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

void SdfRenderer::BlendPixel(ColorRGB *dst, ColorRGB src, float alpha, bool pebble_shades)
{
    if (alpha <= 0.005f)
    {
        return;
    }

    if (pebble_shades)
    {
        /* Quantize to 4 levels for Pebble display */
        int step = static_cast<int>(std::round(alpha * 3.0f));
        alpha = static_cast<float>(step) / 3.0f;
    }

    float inv_a = 1.0f - alpha;
    dst->r = static_cast<uint8_t>(alpha * static_cast<float>(src.r) + inv_a * static_cast<float>(dst->r));
    dst->g = static_cast<uint8_t>(alpha * static_cast<float>(src.g) + inv_a * static_cast<float>(dst->g));
    dst->b = static_cast<uint8_t>(alpha * static_cast<float>(src.b) + inv_a * static_cast<float>(dst->b));
}

void SdfRenderer::DrawText(ColorRGB *buffer, int screen_w, int screen_h,
                           const TextLayout &layout, const Matrix2D &matrix,
                           ColorRGB color, float filter_width_px,
                           float weight_bias_px, bool pebble_shades)
{
    if (layout.glyph_count == 0)
    {
        return;
    }

    Matrix2D inv_matrix = matrix.Inverted();
    float scale = matrix.GetScaleX();
    if (scale < 1e-4f)
    {
        return;
    }

    float half_filter = filter_width_px * 0.5f;

    for (int gi = 0; gi < layout.glyph_count; ++gi)
    {
        const SdfGlyph *g = layout.items[gi].glyph;
        if (!g || g->width == 0 || g->height == 0)
        {
            continue;
        }

        /* 4 glyph corners in font space */
        float gx0 = layout.items[gi].cursor_offset + static_cast<float>(g->bearing_x);
        float gy0 = -static_cast<float>(g->bearing_y);
        float gx1 = gx0 + static_cast<float>(g->width);
        float gy1 = gy0 + static_cast<float>(g->height);

        Vec2 corners[4] = {
            { gx0, gy0 }, { gx1, gy0 },
            { gx0, gy1 }, { gx1, gy1 }
        };

        /* Transform to screen space to compute tight AABB */
        float min_sx = static_cast<float>(screen_w), max_sx = 0.0f;
        float min_sy = static_cast<float>(screen_h), max_sy = 0.0f;

        for (int c = 0; c < 4; ++c)
        {
            Vec2 scr = matrix * corners[c];
            if (scr.x < min_sx) min_sx = scr.x;
            if (scr.x > max_sx) max_sx = scr.x;
            if (scr.y < min_sy) min_sy = scr.y;
            if (scr.y > max_sy) max_sy = scr.y;
        }

        int start_x = std::max(0, static_cast<int>(std::floor(min_sx - filter_width_px)));
        int end_x   = std::min(screen_w, static_cast<int>(std::ceil(max_sx + filter_width_px)));
        int start_y = std::max(0, static_cast<int>(std::floor(min_sy - filter_width_px)));
        int end_y   = std::min(screen_h, static_cast<int>(std::ceil(max_sy + filter_width_px)));

        /* Rasterize screen-space AABB */
        for (int sy = start_y; sy < end_y; ++sy)
        {
            for (int sx = start_x; sx < end_x; ++sx)
            {
                /* Inverse transform: screen pixel -> font coordinates */
                Vec2 font_p = inv_matrix * Vec2(static_cast<float>(sx) + 0.5f, static_cast<float>(sy) + 0.5f);

                /* Glyph-local texture coordinate */
                float u = font_p.x - gx0;
                float v = font_p.y - gy0;

                if (u >= 0.0f && u <= static_cast<float>(g->width) &&
                    v >= 0.0f && v <= static_cast<float>(g->height))
                {
                    float dist_font = SampleSdfBilinear(layout.font, g, u, v);
                    float dist_screen = (dist_font + weight_bias_px) * scale;
                    float alpha = Smoothstep(half_filter, -half_filter, dist_screen);

                    BlendPixel(&buffer[sy * screen_w + sx], color, alpha, pebble_shades);
                }
            }
        }
    }
}
