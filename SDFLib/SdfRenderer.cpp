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
#include <cstring>

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

static inline int32_t Clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* Two adjacent bytes as p[0] | p[1] << 8, as a single (possibly unaligned) 16-bit load */
static inline uint32_t Load16(const uint8_t *p)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
#else
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
#endif
}

static inline void FloorDivMod(int32_t a, int32_t b, int32_t &q, int32_t &r)
{
    /* b > 0, 0 <= r < b */
    q = a / b;
    r = a % b;
    if (r < 0)
    {
        r += b;
        q--;
    }
}

/* Value range [lo, hi) along one axis, prepared for SpanAxis::Clip */
struct SpanRange
{
    int32_t lo, hi;
    int32_t q_lo, r_lo, q_hi, r_hi;
};

/* Exact per-row span along one axis: which steps k give lo <= base + k * d < hi?
 * base advances by d_row per row. Instead of dividing per row, base is kept as
 * quotient/remainder of d and updated incrementally (like a DDA), because
 * ceil((C - base) / d) = qc - qb + (rb < rc) for C = qc * d + rc, base = qb * d + rb.
 * For d < 0 the axis is mirrored, so only d > 0 has to be handled.
 * The renderers limit the scale (MinScale), which keeps all 16.16 values far below 2^31.
 */
class SpanAxis
{
public:
    void Init(int32_t base, int32_t d, int32_t d_row)
    {
        zero_ = (d == 0);
        neg_ = (d < 0);
        d_ = neg_ ? -d : d;
        base_ = base;
        d_row_ = d_row;
        qb_ = rb_ = qr_ = rr_ = 0;
        if (!zero_)
        {
            FloorDivMod(neg_ ? -base : base, d_, qb_, rb_);
            FloorDivMod(neg_ ? -d_row : d_row, d_, qr_, rr_);
        }
    }

    void MakeRange(int32_t lo, int32_t hi, SpanRange &range) const
    {
        range.lo = lo;
        range.hi = hi;
        range.q_lo = range.r_lo = range.q_hi = range.r_hi = 0;
        if (!zero_)
        {
            /* Mirrored: lo <= x < hi  <=>  1 - hi <= -x < 1 - lo */
            FloorDivMod(neg_ ? 1 - hi : lo, d_, range.q_lo, range.r_lo);
            FloorDivMod(neg_ ? 1 - lo : hi, d_, range.q_hi, range.r_hi);
        }
    }

    /* Narrow [k_lo, k_hi) to the steps inside range, for the current row */
    void Clip(const SpanRange &range, int &k_lo, int &k_hi) const
    {
        if (zero_)
        {
            if (base_ < range.lo || base_ >= range.hi)
            {
                k_hi = k_lo;
            }
            return;
        }
        int32_t first = range.q_lo - qb_ + (rb_ < range.r_lo ? 1 : 0);
        int32_t last_excl = range.q_hi - qb_ + (rb_ < range.r_hi ? 1 : 0);
        if (first > k_lo) k_lo = std::min(first, static_cast<int32_t>(k_hi));
        if (last_excl < k_hi) k_hi = std::max(last_excl, static_cast<int32_t>(k_lo));
    }

    void NextRow()
    {
        base_ += d_row_;
        if (!zero_)
        {
            qb_ += qr_;
            rb_ += rr_;
            if (rb_ >= d_)
            {
                rb_ -= d_;
                qb_++;
            }
        }
    }

private:
    bool zero_, neg_;
    int32_t d_, base_, d_row_;
    int32_t qb_, rb_, qr_, rr_;
};

/* Below this scale a glyph is far smaller than a pixel; skipping it keeps the fixed-point math in range */
static const float MinScale = 1.0f / 1024.0f;

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
                           ColorRGB color, float opacity, float filter_width_px,
                           float weight_bias_px,
                           RenderStats *stats)
{
    if (layout.glyph_count == 0)
    {
        return;
    }

    /* Opacity belongs on the coverage, not on interp: interp is a signed distance, so scaling
     * it would move the edge (thinner glyph) instead of making it translucent. 256 = opaque,
     * and then (a * 256) >> 8 == a, so the opaque case stays bit-exact and branch-free.
     */
    uint32_t opacity_a = static_cast<uint32_t>(Clamp32(FastRound(opacity * 256.0f), 0, 256));
    if (opacity_a == 0)
    {
        return;
    }
    uint32_t solid_inv = 256u - opacity_a;

    Matrix2D inv_matrix = matrix.Inverted();
    float scale = matrix.GetScaleX();
    if (scale < MinScale)
    {
        return;
    }

    const SdfFont *font = layout.font;
    const uint8_t *font_data = font ? font->data : nullptr;
    float spread = font ? font->spread : 1.5f;
    float C = 7.5f / spread;
    float interp_mid_f = 7.5f - (weight_bias_px * C) / scale;
    float delta_f = (0.5f * filter_width_px * C) / scale;

    int32_t th_solid = static_cast<int32_t>((interp_mid_f + delta_f) * 256.0f + 0.5f);
    int32_t th_dark  = static_cast<int32_t>((interp_mid_f - delta_f) * 256.0f + 0.5f);
    int32_t th_range = th_solid - th_dark;
    int32_t inv_range = (th_range > 0) ? (256 * 65536 / th_range) : 0;

    int32_t du_fp = static_cast<int32_t>(inv_matrix.a * 65536.0f);
    int32_t dv_fp = static_cast<int32_t>(inv_matrix.c * 65536.0f);
    int32_t du_row_fp = static_cast<int32_t>(inv_matrix.b * 65536.0f);
    int32_t dv_row_fp = static_cast<int32_t>(inv_matrix.d * 65536.0f);

    /* Early reject: interp is a weighted average of the four texels, so it can never exceed
     * their maximum. If every texel is at or below reject_lim, no interpolation can lift the
     * pixel above th_dark, and the whole sample can be skipped. Exact, never drops a visible pixel.
     */
    int32_t reject_lim = Clamp32(th_dark >> 8, -1, 15);
    uint32_t reject_bias = 0x01010101u * static_cast<uint32_t>(127 - reject_lim);

    float pad = filter_width_px + (weight_bias_px > 0.0f ? weight_bias_px : 0.0f);
    uint32_t color_rb = (static_cast<uint32_t>(color.r) << 16) | color.b;
    uint32_t color_g = color.g;

    for (int gi = 0; gi < layout.glyph_count; ++gi)
    {
        const SdfGlyph *g = layout.items[gi].glyph;
        if (!g || g->width < 2 || g->height < 2 || !font_data)
        {
            continue;
        }

        const uint8_t *glyph_data = font_data + g->byte_offset;

        /* Mask the nibble in place instead of shifting each of the 4 texels: values are then
         * 16x larger for shift 4 (max 240 * 256 still fits a 16-bit lane), and the single
         * final shift by (8 + shift) gives exactly the same result.
         */
        int shift = g->shift;
        uint32_t mask = 0x0Fu << shift;
        uint32_t mask2 = (mask << 16) | mask;
        int interp_shift = 8 + shift;

        int32_t gw = g->width;
        int32_t gh = g->height;

        /* Sampling area is 0 <= u <= gw, 0 <= v <= gh */
        int32_t gw_fp1 = (gw << 16) + 1;
        int32_t gh_fp1 = (gh << 16) + 1;

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

        Vec2 p0 = inv_matrix * Vec2(static_cast<float>(start_x) + 0.5f, static_cast<float>(start_y) + 0.5f);
        int32_t row_u_fp = static_cast<int32_t>((p0.x - gx0) * 65536.0f);
        int32_t row_v_fp = static_cast<int32_t>((p0.y - gy0) * 65536.0f);

        SpanAxis axis_u, axis_v;
        axis_u.Init(row_u_fp, du_fp, du_row_fp);
        axis_v.Init(row_v_fp, dv_fp, dv_row_fp);
        SpanRange u_range, v_range;
        axis_u.MakeRange(0, gw_fp1, u_range);
        axis_v.MakeRange(0, gh_fp1, v_range);

        uint32_t n_sampled = 0;
        uint32_t n_written = 0;

        for (int sy = start_y; sy < end_y; ++sy)
        {
            /* Per-row span [k_lo, k_hi) inside the sampling area: no bounds test per pixel */
            int k_lo = 0;
            int k_hi = end_x - start_x;
            axis_u.Clip(u_range, k_lo, k_hi);
            axis_v.Clip(v_range, k_lo, k_hi);

            ColorRGB *row = &buffer[sy * screen_w];
            int32_t cur_u = row_u_fp + k_lo * du_fp;
            int32_t cur_v = row_v_fp + k_lo * dv_fp;
            n_sampled += static_cast<uint32_t>(k_hi - k_lo);

            for (int sx = start_x + k_lo; sx < start_x + k_hi;
                 ++sx, cur_u += du_fp, cur_v += dv_fp)
            {
                int32_t tx = cur_u - 32768;
                int32_t ty = cur_v - 32768;

                /* Clamp the coordinates instead of the four texel positions: a clamped pair
                 * (c, c) equals the border texel with weight 0 or 256 on the missing neighbour,
                 * because c * (256 - f) + c * f = c * 256. Inside the glyph both clamps are
                 * no-ops, so one code path serves border and interior alike.
                 */
                int32_t x0 = Clamp32(tx >> 16, 0, gw - 2);
                int32_t y0 = Clamp32(ty >> 16, 0, gh - 2);

                /* Horizontal neighbours are adjacent bytes: arrange all four texels in one word
                 * (bytes: n01, n11, n00, n10), then one AND per lane pair yields the packed
                 * left (n00 << 16 | n01) and right (n10 << 16 | n11) values.
                 */
                const uint8_t *r0 = glyph_data + y0 * gw + x0;
                const uint8_t *r1 = r0 + gw;
                uint32_t quad = (Load16(r0) << 16) | Load16(r1);
                /* SWAR max test: one nibble per byte, then adding (127 - reject_lim) carries
                 * into bit 7 exactly for the bytes above reject_lim. No bit set means all four
                 * texels are too dark, so this pixel needs no interpolation at all.
                 */
                uint32_t nibbles = (quad >> shift) & 0x0F0F0F0Fu;
                if (((nibbles + reject_bias) & 0x80808080u) == 0)
                {
                    continue;
                }

                int32_t fx = Clamp32((tx - (x0 << 16)) >> 8, 0, 256);
                int32_t fy = Clamp32((ty - (y0 << 16)) >> 8, 0, 256);

                uint32_t left  = quad & mask2;
                uint32_t right = (quad >> 8) & mask2;
                uint32_t top_bot = left * (256 - fx) + right * fx;
                int32_t top = static_cast<int32_t>(top_bot >> 16);
                int32_t bot = static_cast<int32_t>(top_bot & 0xFFFF);
                int32_t interp = (top * (256 - fy) + bot * fy) >> interp_shift;

                if (interp > th_dark)
                {
                    n_written++;

                    uint32_t a;
                    if (interp >= th_solid)
                    {
                        if (solid_inv == 0)
                        {
                            /* Fully opaque interior: no read of the destination needed */
                            row[sx].r = static_cast<uint8_t>(color_rb >> 16);
                            row[sx].g = static_cast<uint8_t>(color_g);
                            row[sx].b = static_cast<uint8_t>(color_rb);
                            continue;
                        }
                        a = opacity_a;
                    }
                    else
                    {
                        int32_t t_i = ((interp - th_dark) * inv_range) >> 16;
                        a = static_cast<uint32_t>((t_i * t_i * (768 - 2 * t_i)) >> 16);
                        a = (a * opacity_a) >> 8;
                    }

                    uint32_t inv_a = 256 - a;
                    uint32_t dst_rb = (static_cast<uint32_t>(row[sx].r) << 16) | row[sx].b;
                    uint32_t blend_rb = (color_rb * a + dst_rb * inv_a) >> 8;
                    row[sx].r = static_cast<uint8_t>(blend_rb >> 16);
                    row[sx].b = static_cast<uint8_t>(blend_rb);
                    row[sx].g = static_cast<uint8_t>((color_g * a + row[sx].g * inv_a) >> 8);
                }
            }

            row_u_fp += du_row_fp;
            row_v_fp += dv_row_fp;
            axis_u.NextRow();
            axis_v.NextRow();
        }

        if (stats)
        {
            stats->pixels_sampled += n_sampled;
            stats->pixels_written += n_written;
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
    if (scale < MinScale)
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

    /* Precompute integer thresholds for unshifted nibbles, for both nibble positions:
     * scaled by 256 for shift 0, or by 4096 (256 * 16) for shift 4.
     */
    int32_t th_solid_n[2], th_mid_n[2], th_dark_n[2];
    for (int n = 0; n < 2; ++n)
    {
        float M = (n == 0) ? 256.0f : 4096.0f;
        th_solid_n[n] = static_cast<int32_t>((interp_mid_f + delta_f) * M + 0.5f);
        th_mid_n[n]   = static_cast<int32_t>(interp_mid_f * M + 0.5f);
        th_dark_n[n]  = static_cast<int32_t>((interp_mid_f - delta_f) * M + 0.5f);
    }

    /* 16.16 Fixed-Point forward differencing:
     * Pure integer addition cur_u += du_fp, cur_v += dv_fp per pixel!
     */
    int32_t du_fp = static_cast<int32_t>(inv_matrix.a * 65536.0f);
    int32_t dv_fp = static_cast<int32_t>(inv_matrix.c * 65536.0f);
    int32_t du_row_fp = static_cast<int32_t>(inv_matrix.b * 65536.0f);
    int32_t dv_row_fp = static_cast<int32_t>(inv_matrix.d * 65536.0f);

    float pad = filter_width_px + (weight_bias_px > 0.0f ? weight_bias_px : 0.0f);

    for (int gi = 0; gi < layout.glyph_count; ++gi)
    {
        const SdfGlyph *g = layout.items[gi].glyph;
        if (!g || g->width < 2 || g->height < 2 || !font_data)
        {
            continue;
        }

        const uint8_t *glyph_data = font_data + g->byte_offset;
        int n = (g->shift == 0) ? 0 : 1;
        int shift = g->shift;
        uint32_t mask = (n == 0) ? 0x0Fu : 0xF0u;
        uint32_t mask2 = (mask << 16) | mask;

        /* See DrawText: skip samples whose four texels are all at or below reject_lim */
        int32_t reject_lim = Clamp32((th_dark_n[n] - 1) >> (8 + shift), -1, 15);
        uint32_t reject_bias = 0x01010101u * static_cast<uint32_t>(127 - reject_lim);
        int32_t th_solid = th_solid_n[n];
        int32_t th_mid   = th_mid_n[n];
        int32_t th_dark  = th_dark_n[n];

        int32_t gw = g->width;
        int32_t gh = g->height;

        /* Sampling area is 0 <= u <= gw, 0 <= v <= gh */
        int32_t gw_fp1 = (gw << 16) + 1;
        int32_t gh_fp1 = (gh << 16) + 1;

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

        int start_x = std::max(0, FastFloor(min_sx - pad));
        int end_x   = std::min(screen_w, FastCeil(max_sx + pad));
        int start_y = std::max(0, FastFloor(min_sy - pad));
        int end_y   = std::min(screen_h, FastCeil(max_sy + pad));

        if (start_x >= end_x || start_y >= end_y)
        {
            continue;
        }

        Vec2 p0 = inv_matrix * Vec2(static_cast<float>(start_x) + 0.5f, static_cast<float>(start_y) + 0.5f);
        int32_t row_u_fp = static_cast<int32_t>((p0.x - gx0) * 65536.0f);
        int32_t row_v_fp = static_cast<int32_t>((p0.y - gy0) * 65536.0f);

        SpanAxis axis_u, axis_v;
        axis_u.Init(row_u_fp, du_fp, du_row_fp);
        axis_v.Init(row_v_fp, dv_fp, dv_row_fp);
        SpanRange u_range, v_range;
        axis_u.MakeRange(0, gw_fp1, u_range);
        axis_v.MakeRange(0, gh_fp1, v_range);

        uint32_t n_sampled = 0;
        uint32_t n_written = 0;

        for (int sy = start_y; sy < end_y; ++sy)
        {
            /* Per-row span [k_lo, k_hi) inside the sampling area: no bounds test per pixel */
            int k_lo = 0;
            int k_hi = end_x - start_x;
            axis_u.Clip(u_range, k_lo, k_hi);
            axis_v.Clip(v_range, k_lo, k_hi);

            uint8_t *row = fb_data + (sy * row_bytes);
            int32_t cur_u = row_u_fp + k_lo * du_fp;
            int32_t cur_v = row_v_fp + k_lo * dv_fp;
            n_sampled += static_cast<uint32_t>(k_hi - k_lo);

            for (int sx = start_x + k_lo; sx < start_x + k_hi;
                 ++sx, cur_u += du_fp, cur_v += dv_fp)
            {
                int32_t tx = cur_u - 32768; /* -0.5 in 16.16 */
                int32_t ty = cur_v - 32768;

                /* Clamp the coordinates instead of the four texel positions: a clamped pair
                 * (c, c) equals the border texel with weight 0 or 256 on the missing neighbour,
                 * because c * (256 - f) + c * f = c * 256. Inside the glyph both clamps are
                 * no-ops, so one code path serves border and interior alike.
                 */
                int32_t x0 = Clamp32(tx >> 16, 0, gw - 2);
                int32_t y0 = Clamp32(ty >> 16, 0, gh - 2);

                /* Horizontal neighbours are adjacent bytes: arrange all four texels in one word
                 * (bytes: n01, n11, n00, n10), then one AND per lane pair yields the packed
                 * left (n00 << 16 | n01) and right (n10 << 16 | n11) values.
                 */
                const uint8_t *r0 = glyph_data + y0 * gw + x0;
                const uint8_t *r1 = r0 + gw;
                uint32_t quad = (Load16(r0) << 16) | Load16(r1);
                /* SWAR max test: one nibble per byte, then adding (127 - reject_lim) carries
                 * into bit 7 exactly for the bytes above reject_lim. No bit set means all four
                 * texels are too dark, so this pixel needs no interpolation at all.
                 */
                uint32_t nibbles = (quad >> shift) & 0x0F0F0F0Fu;
                if (((nibbles + reject_bias) & 0x80808080u) == 0)
                {
                    continue;
                }

                int32_t fx = Clamp32((tx - (x0 << 16)) >> 8, 0, 256);
                int32_t fy = Clamp32((ty - (y0 << 16)) >> 8, 0, 256);

                uint32_t left  = quad & mask2;
                uint32_t right = (quad >> 8) & mask2;

                /* Top and bottom row interpolated together, max 240 * 256 per 16-bit lane */
                uint32_t top_bot = left * (256 - fx) + right * fx;
                int32_t top = static_cast<int32_t>(top_bot >> 16);
                int32_t bot = static_cast<int32_t>(top_bot & 0xFFFF);
                int32_t interp = ((top >> 8) * (256 - fy) + (bot >> 8) * fy);

                if (interp >= th_solid)
                {
                    row[sx] = 0xFF;
                    n_written++;
                }
                else if (interp >= th_mid)
                {
                    if (0xEA > row[sx])
                    {
                        row[sx] = 0xEA;
                        n_written++;
                    }
                }
                else if (interp >= th_dark)
                {
                    if (0xD5 > row[sx])
                    {
                        row[sx] = 0xD5;
                        n_written++;
                    }
                }
            }

            row_u_fp += du_row_fp;
            row_v_fp += dv_row_fp;
            axis_u.NextRow();
            axis_v.NextRow();
        }

        if (stats)
        {
            stats->pixels_scanned += static_cast<uint32_t>((end_x - start_x) * (end_y - start_y));
            stats->pixels_sampled += n_sampled;
            stats->pixels_written += n_written;
        }
    }
}
