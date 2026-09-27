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
#include <cstdint>
#include <algorithm>

static inline int FastFloor(float x)
{
    int i = static_cast<int>(x);
    return (x < static_cast<float>(i)) ? i - 1 : i;
}

static inline int FastCeil(float x)
{
    int i = static_cast<int>(x);
    return (x > static_cast<float>(i)) ? i + 1 : i;
}

static inline int FastRound(float x)
{
    return FastFloor(x + 0.5f);
}

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

    int x0 = FastFloor(tx);
    int y0 = FastFloor(ty);

    float fx = tx - static_cast<float>(x0);
    float fy = ty - static_cast<float>(y0);

    const uint8_t *data = font ? font->data : nullptr;
    if (!data)
    {
        return 0.0f;
    }

    const uint8_t *glyph_data = data + glyph->byte_offset;
    int shift = glyph->shift;

    float n00, n10, n01, n11;

    /* Fast path for interior texels without clamping */
    if (x0 >= 0 && x0 < w - 1 && y0 >= 0 && y0 < h - 1)
    {
        const uint8_t *r0 = glyph_data + y0 * w + x0;
        const uint8_t *r1 = r0 + w;
        n00 = static_cast<float>((r0[0] >> shift) & 0x0F);
        n10 = static_cast<float>((r0[1] >> shift) & 0x0F);
        n01 = static_cast<float>((r1[0] >> shift) & 0x0F);
        n11 = static_cast<float>((r1[1] >> shift) & 0x0F);
    }
    else
    {
        auto get_nibble = [&](int gx, int gy) -> float
        {
            int cx = (gx < 0) ? 0 : ((gx >= w) ? w - 1 : gx);
            int cy = (gy < 0) ? 0 : ((gy >= h) ? h - 1 : gy);
            return static_cast<float>((glyph_data[cy * w + cx] >> shift) & 0x0F);
        };
        n00 = get_nibble(x0, y0);
        n10 = get_nibble(x0 + 1, y0);
        n01 = get_nibble(x0, y0 + 1);
        n11 = get_nibble(x0 + 1, y0 + 1);
    }

    float top = n00 + fx * (n10 - n00);
    float bot = n01 + fx * (n11 - n01);
    float interp_nibble = top + fy * (bot - top);

    float spread = font ? font->spread : 1.5f;
    return -(interp_nibble - 7.5f) * (spread * (1.0f / 7.5f));
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
        int step = static_cast<int>(alpha * 3.0f + 0.5f);
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
        Vec2 scr0 = matrix * corners[0];
        float min_sx = scr0.x, max_sx = scr0.x;
        float min_sy = scr0.y, max_sy = scr0.y;

        for (int c = 1; c < 4; ++c)
        {
            Vec2 scr = matrix * corners[c];
            if (scr.x < min_sx) min_sx = scr.x;
            if (scr.x > max_sx) max_sx = scr.x;
            if (scr.y < min_sy) min_sy = scr.y;
            if (scr.y > max_sy) max_sy = scr.y;
        }

        int start_x = std::max(0, FastFloor(min_sx - filter_width_px));
        int end_x   = std::min(screen_w, FastCeil(max_sx + filter_width_px));
        int start_y = std::max(0, FastFloor(min_sy - filter_width_px));
        int end_y   = std::min(screen_h, FastCeil(max_sy + filter_width_px));

        if (start_x >= end_x || start_y >= end_y)
        {
            continue;
        }

        /* Forward differencing for affine transformation */
        float du = inv_matrix.a;
        float dv = inv_matrix.c;
        float du_row = inv_matrix.b;
        float dv_row = inv_matrix.d;

        Vec2 row_p = inv_matrix * Vec2(static_cast<float>(start_x) + 0.5f, static_cast<float>(start_y) + 0.5f);

        float gw = static_cast<float>(g->width);
        float gh = static_cast<float>(g->height);

        /* Rasterize screen-space AABB */
        for (int sy = start_y; sy < end_y; ++sy)
        {
            ColorRGB *row = &buffer[sy * screen_w];
            float cur_u = row_p.x - gx0;
            float cur_v = row_p.y - gy0;

            for (int sx = start_x; sx < end_x; ++sx)
            {
                if (cur_u >= 0.0f && cur_u <= gw &&
                    cur_v >= 0.0f && cur_v <= gh)
                {
                    float dist_font = SampleSdfBilinear(layout.font, g, cur_u, cur_v);
                    float dist_screen = (dist_font - weight_bias_px) * scale;
                    float alpha = Smoothstep(half_filter, -half_filter, dist_screen);

                    BlendPixel(&row[sx], color, alpha, pebble_shades);
                }

                cur_u += du;
                cur_v += dv;
            }

            row_p.x += du_row;
            row_p.y += dv_row;
        }
    }
}

void SdfRenderer::DrawToFramebuffer8Bit(uint8_t *fb_data, int screen_w, int screen_h, int row_bytes,
                                        const TextLayout &layout, const Matrix2D &matrix,
                                        float filter_width_px, float weight_bias_px,
                                        RenderStats *stats)
{
    if (!fb_data || layout.glyph_count == 0)
    {
        return;
    }

    Matrix2D inv_matrix = matrix.Inverted();
    float scale = matrix.GetScaleX();
    if (scale < 1e-4f)
    {
        return;
    }

    const SdfFont *font = layout.font;
    const uint8_t *font_data = font ? font->data : nullptr;
    float spread = font ? font->spread : 1.5f;
    float C = 7.5f / spread;
    /* Positive weight_bias_px expands glyph outline (bold), negative shrinks it (light) */
    float interp_mid_f = 7.5f - (weight_bias_px * C) / scale;
    float delta_f = (0.25f * filter_width_px * C) / scale;

    for (int gi = 0; gi < layout.glyph_count; ++gi)
    {
        const SdfGlyph *g = layout.items[gi].glyph;
        if (!g || g->width == 0 || g->height == 0)
        {
            continue;
        }

        const uint8_t *glyph_data = font_data ? (font_data + g->byte_offset) : nullptr;
        uint8_t mask = (g->shift == 0) ? 0x0F : 0xF0;

        /* Precompute integer thresholds for unshifted nibbles:
         * Scale by 256 for shift 0, or by 4096 (256 * 16) for shift 4.
         */
        float M = (g->shift == 0) ? 256.0f : 4096.0f;
        int32_t th_solid = static_cast<int32_t>((interp_mid_f + delta_f) * M + 0.5f);
        int32_t th_mid   = static_cast<int32_t>(interp_mid_f * M + 0.5f);
        int32_t th_dark  = static_cast<int32_t>((interp_mid_f - delta_f) * M + 0.5f);

        int32_t gw_fp = static_cast<int32_t>(static_cast<float>(g->width) * 65536.0f);
        int32_t gh_fp = static_cast<int32_t>(static_cast<float>(g->height) * 65536.0f);
        int32_t gw = g->width;
        int32_t gh = g->height;

        float gx0 = layout.items[gi].cursor_offset + static_cast<float>(g->bearing_x);
        float gy0 = -static_cast<float>(g->bearing_y);
        float gx1 = gx0 + static_cast<float>(g->width);
        float gy1 = gy0 + static_cast<float>(g->height);

        Vec2 corners[4] = {
            { gx0, gy0 }, { gx1, gy0 },
            { gx0, gy1 }, { gx1, gy1 }
        };

        Vec2 scr0 = matrix * corners[0];
        float min_sx = scr0.x, max_sx = scr0.x;
        float min_sy = scr0.y, max_sy = scr0.y;

        for (int c = 1; c < 4; ++c)
        {
            Vec2 scr = matrix * corners[c];
            if (scr.x < min_sx) min_sx = scr.x;
            if (scr.x > max_sx) max_sx = scr.x;
            if (scr.y < min_sy) min_sy = scr.y;
            if (scr.y > max_sy) max_sy = scr.y;
        }

        float pad = filter_width_px + (weight_bias_px > 0.0f ? weight_bias_px : 0.0f);
        int start_x = std::max(0, FastFloor(min_sx - pad));
        int end_x   = std::min(screen_w, FastCeil(max_sx + pad));
        int start_y = std::max(0, FastFloor(min_sy - pad));
        int end_y   = std::min(screen_h, FastCeil(max_sy + pad));

        if (start_x >= end_x || start_y >= end_y)
        {
            continue;
        }

        if (stats)
        {
            stats->pixels_scanned += static_cast<uint32_t>((end_x - start_x) * (end_y - start_y));
        }

        /* 16.16 Fixed-Point forward differencing:
         * Pure integer addition cur_u += du_fp, cur_v += dv_fp per pixel!
         */
        int32_t du_fp = static_cast<int32_t>(inv_matrix.a * 65536.0f);
        int32_t dv_fp = static_cast<int32_t>(inv_matrix.c * 65536.0f);
        int32_t du_row_fp = static_cast<int32_t>(inv_matrix.b * 65536.0f);
        int32_t dv_row_fp = static_cast<int32_t>(inv_matrix.d * 65536.0f);

        Vec2 p0 = inv_matrix * Vec2(static_cast<float>(start_x) + 0.5f, static_cast<float>(start_y) + 0.5f);
        int32_t row_u_fp = static_cast<int32_t>((p0.x - gx0) * 65536.0f);
        int32_t row_v_fp = static_cast<int32_t>((p0.y - gy0) * 65536.0f);

        for (int sy = start_y; sy < end_y; ++sy)
        {
            uint8_t *row = fb_data + (sy * row_bytes);
            int32_t cur_u = row_u_fp;
            int32_t cur_v = row_v_fp;

            for (int sx = start_x; sx < end_x; ++sx)
            {
                if (cur_u >= 0 && cur_u <= gw_fp &&
                    cur_v >= 0 && cur_v <= gh_fp)
                {
                    if (stats)
                    {
                        stats->pixels_sampled++;
                    }

                    int32_t tx = cur_u - 32768; /* -0.5 in 16.16 */
                    int32_t ty = cur_v - 32768;

                    int32_t x0 = tx >> 16;
                    int32_t y0 = ty >> 16;
                    int32_t fx = (tx >> 8) & 0xFF;
                    int32_t fy = (ty >> 8) & 0xFF;

                    int32_t n00, n10, n01, n11;

                    /* Fast path for interior texels without clamping */
                    if (x0 >= 0 && x0 < gw - 1 && y0 >= 0 && y0 < gh - 1)
                    {
                        const uint8_t *r0 = glyph_data + y0 * gw + x0;
                        const uint8_t *r1 = r0 + gw;
                        n00 = r0[0] & mask;
                        n10 = r0[1] & mask;
                        n01 = r1[0] & mask;
                        n11 = r1[1] & mask;
                    }
                    else
                    {
                        auto get_n = [&](int32_t gx, int32_t gy) -> int32_t
                        {
                            int32_t cx = (gx < 0) ? 0 : ((gx >= gw) ? gw - 1 : gx);
                            int32_t cy = (gy < 0) ? 0 : ((gy >= gh) ? gh - 1 : gy);
                            return glyph_data ? (glyph_data[cy * gw + cx] & mask) : 0;
                        };
                        n00 = get_n(x0, y0);
                        n10 = get_n(x0 + 1, y0);
                        n01 = get_n(x0, y0 + 1);
                        n11 = get_n(x0 + 1, y0 + 1);
                    }

                    int32_t top = n00 * (256 - fx) + n10 * fx;
                    int32_t bot = n01 * (256 - fx) + n11 * fx;
                    int32_t interp = ((top >> 8) * (256 - fy) + (bot >> 8) * fy);

                    if (interp >= th_solid)
                    {
                        row[sx] = 0xFF;
                        if (stats)
                        {
                            stats->pixels_written++;
                        }
                    }
                    else if (interp >= th_mid)
                    {
                        if (0xEA > row[sx])
                        {
                            row[sx] = 0xEA;
                            if (stats)
                            {
                                stats->pixels_written++;
                            }
                        }
                    }
                    else if (interp >= th_dark)
                    {
                        if (0xD5 > row[sx])
                        {
                            row[sx] = 0xD5;
                            if (stats)
                            {
                                stats->pixels_written++;
                            }
                        }
                    }
                }

                cur_u += du_fp;
                cur_v += dv_fp;
            }

            row_u_fp += du_row_fp;
            row_v_fp += dv_row_fp;
        }
    }
}
