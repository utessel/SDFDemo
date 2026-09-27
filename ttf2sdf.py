#!/usr/bin/env python3
"""
ttf2sdf.py - High-Performance TTF to 4-bit Signed Distance Field (SDF) Font Generator.
Generates ultra-compact 4-bit SDF fonts with analytical Cardano cubic Bézier distance solving.

Copyright (C) 2026 Uli Tessel

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

Alternative Licensing:
If you wish to use this software or components under terms other than
the GNU GPLv3, please contact the author to request an alternative license.
"""
# flake8: noqa: F541

import math
import sys
import configparser
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import DecomposingRecordingPen


def solve_cubic_cardano(c3, c2, c1, c0):
    """
    Solves c3*t^3 + c2*t^2 + c1*t + c0 = 0 using Cardano's formula.
    Returns a list of real roots in interval (0, 1).
    """
    if abs(c3) < 1e-9:
        # Fallback to quadratic: c2*t^2 + c1*t + c0 = 0
        if abs(c2) < 1e-9:
            if abs(c1) < 1e-9:
                return []
            t = -c0 / c1
            return [t] if 0.0 < t < 1.0 else []
        disc = c1 * c1 - 4 * c2 * c0
        if disc < 0:
            return []
        sqrt_disc = math.sqrt(disc)
        roots = [(-c1 - sqrt_disc) / (2 * c2), (-c1 + sqrt_disc) / (2 * c2)]
        return [t for t in roots if 0.0 < t < 1.0]

    # Normalize to t^3 + a*t^2 + b*t + c = 0
    a = c2 / c3
    b = c1 / c3
    c = c0 / c3

    # Substitute t = u - a/3 to get depressed cubic: u^3 + p*u + q = 0
    a3 = a / 3.0
    p = b - a * a3
    q = c - a3 * b + 2.0 * (a3 ** 3)

    p3 = p / 3.0
    q2 = q / 2.0
    discriminant = q2 * q2 + p3 * p3 * p3

    roots = []
    if discriminant > 1e-9:
        # One real root
        sqrt_d = math.sqrt(discriminant)
        u1 = -q2 + sqrt_d
        u2 = -q2 - sqrt_d
        r1 = math.copysign(abs(u1) ** (1.0 / 3.0), u1)
        r2 = math.copysign(abs(u2) ** (1.0 / 3.0), u2)
        roots.append((r1 + r2) - a3)
    elif abs(discriminant) <= 1e-9:
        # All roots real, at least two are equal
        if abs(q2) < 1e-9:
            roots.append(-a3)
        else:
            r = math.copysign(abs(q2) ** (1.0 / 3.0), -q2)
            roots.append(2.0 * r - a3)
            roots.append(-r - a3)
    else:
        # Three distinct real roots (Casus irreducibilis, trigonometric solution)
        m = math.sqrt(-p3)
        arg = -q2 / (m * m * m)
        arg = max(-1.0, min(1.0, arg))  # clamp for safety
        phi = math.acos(arg) / 3.0
        two_m = 2.0 * m
        roots.append(two_m * math.cos(phi) - a3)
        roots.append(two_m * math.cos(phi + 2.0 * math.pi / 3.0) - a3)
        roots.append(two_m * math.cos(phi + 4.0 * math.pi / 3.0) - a3)

    return [t for t in roots if 0.0 < t < 1.0]


class LineSegment:
    def __init__(self, p0, p1):
        self.p0 = p0
        self.p1 = p1
        self.dx = p1[0] - p0[0]
        self.dy = p1[1] - p0[1]
        self.len_sq = self.dx * self.dx + self.dy * self.dy
        # AABB with safety padding
        self.min_x = min(p0[0], p1[0])
        self.max_x = max(p0[0], p1[0])
        self.min_y = min(p0[1], p1[1])
        self.max_y = max(p0[1], p1[1])

    def dist_sq_interior(self, qx, qy):
        """Returns dist_sq if closest point is strictly interior (0 < t < 1), else inf."""
        if self.len_sq < 1e-9:
            return float('inf')
        t = ((qx - self.p0[0]) * self.dx + (qy - self.p0[1]) * self.dy) / self.len_sq
        if 0.0 < t < 1.0:
            proj_x = self.p0[0] + t * self.dx
            proj_y = self.p0[1] + t * self.dy
            return (qx - proj_x) ** 2 + (qy - proj_y) ** 2
        return float('inf')


class BezierSegment:
    def __init__(self, p0, p1, p2):
        self.p0 = p0
        self.p1 = p1
        self.p2 = p2
        # Parametric quadratic bezier: B(t) = A*t^2 + B*t + C
        self.ax = p0[0] - 2.0 * p1[0] + p2[0]
        self.ay = p0[1] - 2.0 * p1[1] + p2[1]
        self.bx = 2.0 * (p1[0] - p0[0])
        self.by = 2.0 * (p1[1] - p0[1])
        self.cx = p0[0]
        self.cy = p0[1]

        # Precompute derivative polynomial coefficients:
        # c3 = 2 * (A . A)
        self.c3 = 2.0 * (self.ax * self.ax + self.ay * self.ay)
        # c2 = 3 * (A . B)
        self.c2 = 3.0 * (self.ax * self.bx + self.ay * self.by)
        self.bb = self.bx * self.bx + self.by * self.by

    def dist_sq_interior(self, qx, qy):
        """Calculates minimum distance squared to interior of bezier curve via Cardano."""
        cq_x = self.cx - qx
        cq_y = self.cy - qy
        c1 = 2.0 * (self.ax * cq_x + self.ay * cq_y) + self.bb
        c0 = self.bx * cq_x + self.by * cq_y

        roots = solve_cubic_cardano(self.c3, self.c2, c1, c0)
        if not roots:
            return float('inf')

        min_d2 = float('inf')
        for t in roots:
            bx = self.ax * t * t + self.bx * t + self.cx
            by = self.ay * t * t + self.by * t + self.cy
            d2 = (qx - bx) ** 2 + (qy - by) ** 2
            if d2 < min_d2:
                min_d2 = d2
        return min_d2


class GlyphContours:
    def __init__(self, raw_contours, scale, offset_x, offset_y, spread_clamp):
        self.points = []
        self.lines = []
        self.beziers = []
        self.raw_contours = []  # for winding number / ray-casting

        for raw_c in raw_contours:
            scaled_c = [((p[0] * scale) + offset_x, (p[1] * scale) + offset_y) for p in raw_c]
            self.raw_contours.append(scaled_c)
            # Add points
            for p in scaled_c:
                self.points.append(p)

            # Decompose into segments
            n = len(raw_c)
            i = 0
            while i < n:
                p0 = scaled_c[i]
                p1 = scaled_c[(i + 1) % n]
                flag1 = raw_c[(i + 1) % n][2] if len(raw_c[(i + 1) % n]) > 2 else True

                if flag1:  # On-curve: line segment
                    self.lines.append(LineSegment(p0, p1))
                    i += 1
                else:  # Off-curve: quadratic bezier
                    p2 = scaled_c[(i + 2) % n]
                    self.beziers.append(BezierSegment(p0, p1, p2))
                    i += 2

    def contains_point(self, qx, qy):
        """Even-Odd ray casting rule for point inside contour."""
        inside = False
        for contour in self.raw_contours:
            n = len(contour)
            for i in range(n):
                p1 = contour[i]
                p2 = contour[(i + 1) % n]
                if ((p1[1] > qy) != (p2[1] > qy)) and \
                   (qx < (p2[0] - p1[0]) * (qy - p1[1]) / (p2[1] - p1[1] + 1e-12) + p1[0]):
                    inside = not inside
        return inside


def rasterize_sdf(contours, width, height, spread_clamp):
    """
    Rasters the glyph: direct min over all elements.
    Returns 2D grid [height][width] with signed distances clamped to [-spread, +spread].
    """
    grid = [[spread_clamp for _ in range(width)] for _ in range(height)]
    pts = contours.points
    lines = contours.lines
    beziers = contours.beziers

    for y in range(height):
        qy = y + 0.5
        for x in range(width):
            qx = x + 0.5
            min_d2 = spread_clamp * spread_clamp

            # 1. Endpoints
            for px, py in pts:
                d2 = (qx - px) ** 2 + (qy - py) ** 2
                if d2 < min_d2:
                    min_d2 = d2

            # 2. Line segments interior
            for line in lines:
                d2 = line.dist_sq_interior(qx, qy)
                if d2 < min_d2:
                    min_d2 = d2

            # 3. Bezier segments interior
            for bz in beziers:
                d2 = bz.dist_sq_interior(qx, qy)
                if d2 < min_d2:
                    min_d2 = d2

            dist = math.sqrt(min_d2)
            # Apply sign
            if contours.contains_point(qx, qy):
                grid[y][x] = -dist
            else:
                grid[y][x] = dist

    return grid


def parse_ttf_glyph(tt, glyph_name):
    """Extracts raw contours with on/off curve flags from TTF."""
    glyph_set = tt.getGlyphSet()
    if glyph_name not in glyph_set:
        return []
    glyph = glyph_set[glyph_name]
    pen = DecomposingRecordingPen(glyph_set)
    glyph.draw(pen)

    contours = []
    current_contour = []

    for cmd, args in pen.value:
        if cmd == 'moveTo':
            if current_contour:
                contours.append(current_contour)
                current_contour = []
            current_contour.append((args[0][0], args[0][1], True))
        elif cmd == 'lineTo':
            current_contour.append((args[0][0], args[0][1], True))
        elif cmd == 'qCurveTo':
            # TTF quadratic bezier: intermediate points are off-curve
            for pt in args[:-1]:
                if pt is not None:
                    current_contour.append((pt[0], pt[1], False))
            if args[-1] is not None:
                current_contour.append((args[-1][0], args[-1][1], True))
        elif cmd == 'closePath':
            if current_contour:
                contours.append(current_contour)
                current_contour = []

    if current_contour:
        contours.append(current_contour)

    return contours


def main():
    config_file = sys.argv[1] if len(sys.argv) > 1 else 'sdf_config.ini'
    cfg = configparser.ConfigParser()
    cfg.read(config_file)

    ttf_path = cfg.get('font', 'ttf_path')
    em_height = cfg.getint('font', 'em_height')
    spread = cfg.getfloat('font', 'spread_pixels')

    c_header = cfg.get('output', 'c_header')
    c_source = cfg.get('output', 'c_source')
    font_name = cfg.get('output', 'font_name')

    include_ascii = cfg.getboolean('characters', 'include_ascii', fallback=True)
    extra_chars = cfg.get('characters', 'extra_characters', fallback='')

    print(f"Loading TTF: {ttf_path}")
    tt = TTFont(ttf_path)
    units_per_em = tt['head'].unitsPerEm
    scale = em_height / units_per_em

    # Build character list
    chars_to_render = []
    if include_ascii:
        for c in range(32, 127):
            chars_to_render.append(chr(c))
    for c in extra_chars:
        if c not in chars_to_render:
            chars_to_render.append(c)

    cmap = tt.getBestCmap()
    glyphs_data = []

    print(f"Generating SDF for {len(chars_to_render)} characters (Target EM: {em_height}px, Spread: ±{spread}px)...")

    for ch in chars_to_render:
        cp = ord(ch)
        glyph_name = cmap.get(cp)
        if not glyph_name or glyph_name not in tt.getGlyphSet():
            print(f"Warning: Glyph for '{ch}' (U+{cp:04X}) not found in font.")
            continue

        hmtx = tt['hmtx'][glyph_name]
        advance_width = int(round(hmtx[0] * scale))

        raw_contours = parse_ttf_glyph(tt, glyph_name)
        if not raw_contours or ch == ' ':
            # Space or blank glyph
            glyphs_data.append({
                'char': ch,
                'codepoint': cp,
                'width': 0,
                'height': 0,
                'bearing_x': 0,
                'bearing_y': 0,
                'advance': advance_width,
                'sdf_4bit': []
            })
            continue

        # Compute bounding box in font units
        all_x = [p[0] for c in raw_contours for p in c]
        all_y = [p[1] for c in raw_contours for p in c]
        min_x = min(all_x) * scale
        max_x = max(all_x) * scale
        min_y = min(all_y) * scale
        max_y = max(all_y) * scale

        # Add spread margin around glyph
        pad = math.ceil(spread)
        grid_x0 = int(math.floor(min_x)) - pad
        grid_x1 = int(math.ceil(max_x)) + pad
        grid_y0 = int(math.floor(min_y)) - pad
        grid_y1 = int(math.ceil(max_y)) + pad

        w = max(1, grid_x1 - grid_x0)
        h = max(1, grid_y1 - grid_y0)

        # Contours in local grid coordinates (0, 0 at top-left of grid)
        # Note: TTF Y is up, screen Y is down!
        # local_x = x_font*scale - grid_x0
        # local_y = grid_y1 - y_font*scale
        # Transform contours:
        transformed_contours = []
        for c in raw_contours:
            tc = []
            for p in c:
                lx = p[0] * scale - grid_x0
                ly = grid_y1 - p[1] * scale
                flag = p[2] if len(p) > 2 else True
                tc.append((lx, ly, flag))
            transformed_contours.append(tc)

        c_obj = GlyphContours(transformed_contours, scale=1.0, offset_x=0.0, offset_y=0.0, spread_clamp=spread)
        sdf_grid = rasterize_sdf(c_obj, w, h, spread)

        # Convert float distances [-spread, +spread] to 4-bit [0..15]
        # norm: -1.0 (+spread outside) .. 0.0 (contour) .. +1.0 (-spread inside)
        grid_4bit = [[0 for _ in range(w)] for _ in range(h)]
        for y in range(h):
            for x in range(w):
                d = sdf_grid[y][x]
                norm = -d / spread
                val = int(round(norm * 7.5 + 7.5))
                grid_4bit[y][x] = max(0, min(15, val))

        # Tighten bounding box: find active region where val > 0
        min_gx, max_gx = w, -1
        min_gy, max_gy = h, -1
        for y in range(h):
            for x in range(w):
                if grid_4bit[y][x] > 0:
                    if x < min_gx:
                        min_gx = x
                    if x > max_gx:
                        max_gx = x
                    if y < min_gy:
                        min_gy = y
                    if y > max_gy:
                        max_gy = y

        if max_gx < min_gx or max_gy < min_gy:
            # Completely empty
            tw, th = 0, 0
            flat_4bit = []
            tight_x0 = 0
            tight_y1 = 0
        else:
            tw = max_gx - min_gx + 1
            th = max_gy - min_gy + 1
            flat_4bit = []
            for y in range(min_gy, max_gy + 1):
                for x in range(min_gx, max_gx + 1):
                    flat_4bit.append(grid_4bit[y][x])
            tight_x0 = grid_x0 + min_gx
            tight_y1 = grid_y1 - min_gy

        glyphs_data.append({
            'char': ch,
            'codepoint': cp,
            'width': tw,
            'height': th,
            'bearing_x': tight_x0,
            'bearing_y': tight_y1,  # baseline distance from top
            'advance': advance_width,
            'sdf_4bit': flat_4bit
        })
        print(f"  Rendered '{ch}' (U+{cp:04X}): {tw}x{th} px (trimmed from {w}x{h}), adv={advance_width}")

    # Pack 4-bit nibbles: Pair up glyphs into shared byte buffers!
    # Even-indexed non-empty glyphs use Low-Nibble (shift=0, 0x0F),
    # Odd-indexed non-empty glyphs use High-Nibble (shift=4, 0xF0).
    
    # We pack pairs of glyphs
    non_empty = [g for g in glyphs_data if g['width'] > 0 and g['height'] > 0]
    
    # Simple pairing:
    packed_storage = []
    offset = 0
    for i in range(0, len(non_empty), 2):
        g1 = non_empty[i]
        g2 = non_empty[i + 1] if (i + 1) < len(non_empty) else None

        len1 = len(g1['sdf_4bit'])
        len2 = len(g2['sdf_4bit']) if g2 else 0
        max_len = max(len1, len2)

        g1['byte_offset'] = offset
        g1['nibble_shift'] = 0  # Low nibble

        if g2:
            g2['byte_offset'] = offset
            g2['nibble_shift'] = 4  # High nibble

        # Merge bytes
        for b_idx in range(max_len):
            v1 = g1['sdf_4bit'][b_idx] if b_idx < len1 else 0
            v2 = (g2['sdf_4bit'][b_idx] << 4) if (g2 and b_idx < len2) else 0
            packed_storage.append(v1 | v2)

        offset += max_len

    # For empty glyphs (like space)
    for g in glyphs_data:
        if g['width'] == 0 or g['height'] == 0:
            g['byte_offset'] = 0
            g['nibble_shift'] = 0

    print(f"\nTotal packed bitmap data: {len(packed_storage)} Bytes ({len(packed_storage)/1024:.2f} KB)!")

    # Write C-Header and C-Source
    write_c_output(c_header, c_source, font_name, em_height, spread, glyphs_data, packed_storage)
    print(f"Successfully generated {c_header} and {c_source}!")


def write_c_output(header_path, source_path, font_name, em_height, spread, glyphs, packed_data):
    # Separate ASCII 32..126 for zero-overhead direct table indexing
    ascii_glyphs = [g for g in glyphs if 32 <= g['codepoint'] <= 126]
    extra_glyphs = [g for g in glyphs if g['codepoint'] < 32 or g['codepoint'] > 126]

    with open(header_path, 'w', encoding='utf-8') as h:
        h.write(f"/* Auto-generated by ttf2sdf.py - Do not edit directly */\n")
        h.write(f"#pragma once\n\n")
        h.write(f"#include <stdint.h>\n\n")
        h.write(f"#define SDF_EM_HEIGHT {em_height}\n")
        h.write(f"#define SDF_SPREAD    {spread}f\n\n")
        h.write(f"/* Ultra-compact 8-byte glyph descriptor */\n")
        h.write(f"typedef struct\n")
        h.write(f"{{\n")
        h.write(f"    uint8_t  width;\n")
        h.write(f"    uint8_t  height;\n")
        h.write(f"    int8_t   bearing_x;\n")
        h.write(f"    int8_t   bearing_y;\n")
        h.write(f"    uint8_t  advance;\n")
        h.write(f"    uint8_t  shift;       /* 0 for low nibble (0x0F), 4 for high nibble (0xF0) */\n")
        h.write(f"    uint16_t byte_offset; /* Offset into sdf_font_data */\n")
        h.write(f"}} SdfGlyph;\n\n")
        h.write(f"typedef struct\n")
        h.write(f"{{\n")
        h.write(f"    uint16_t codepoint;\n")
        h.write(f"    SdfGlyph glyph;\n")
        h.write(f"}} SdfExtraGlyph;\n\n")
        h.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        h.write(f"extern const uint8_t {font_name}_data[{len(packed_data)}];\n")
        h.write(f"extern const SdfGlyph {font_name}_ascii[95]; /* ASCII 32..126 direct index */\n")
        if extra_glyphs:
            h.write(f"extern const SdfExtraGlyph {font_name}_extra[{len(extra_glyphs)}];\n")
            h.write(f"#define {font_name.upper()}_EXTRA_COUNT {len(extra_glyphs)}\n\n")
        h.write(f"/* Lookup function */\n")
        h.write(f"const SdfGlyph* {font_name}_get_glyph(uint32_t codepoint);\n\n")
        h.write(f"/* Font Descriptor */\n")
        h.write(f"typedef struct\n")
        h.write(f"{{\n")
        h.write(f"    uint8_t  em_height;\n")
        h.write(f"    float    spread;\n")
        h.write(f"    const uint8_t *data;\n")
        h.write(f"    const SdfGlyph* (*get_glyph)(uint32_t codepoint);\n")
        h.write(f"}} SdfFont;\n\n")
        h.write(f"extern const SdfFont {font_name};\n\n")
        h.write("#ifdef __cplusplus\n}\n#endif\n")

    with open(source_path, 'w', encoding='utf-8') as s:
        s.write(f'#include "{header_path}"\n#include <stddef.h>\n\n')
        s.write(f"/* Shared packed 4-bit SDF pixel storage */\n")
        s.write(f"const uint8_t {font_name}_data[{len(packed_data)}] =\n{{\n")
        for i in range(0, len(packed_data), 16):
            chunk = packed_data[i:i+16]
            hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
            comma = "," if (i + 16) < len(packed_data) else ""
            s.write(f"    {hex_str}{comma}\n")
        s.write(f"}};\n\n")

        # ASCII direct table
        s.write(f"const SdfGlyph {font_name}_ascii[95] =\n{{\n")
        for cp in range(32, 127):
            match = next((g for g in ascii_glyphs if g['codepoint'] == cp), None)
            if match:
                ch_repr = match['char'] if match['char'] not in ["'", "\\"] else f"\\{match['char']}"
                s.write(f"    /* '{ch_repr}' ({cp}) */ {{ {match['width']}, {match['height']}, {match['bearing_x']}, {match['bearing_y']}, {match['advance']}, {match['nibble_shift']}, {match['byte_offset']} }},\n")
            else:
                s.write(f"    /* {cp} */ {{ 0, 0, 0, 0, 0, 0, 0 }},\n")
        s.write(f"}};\n\n")

        # Extra table
        if extra_glyphs:
            s.write(f"const SdfExtraGlyph {font_name}_extra[{len(extra_glyphs)}] =\n{{\n")
            for g in extra_glyphs:
                s.write(f"    {{ 0x{g['codepoint']:04X}, {{ {g['width']}, {g['height']}, {g['bearing_x']}, {g['bearing_y']}, {g['advance']}, {g['nibble_shift']}, {g['byte_offset']} }} }},\n")
            s.write(f"}};\n\n")

        # Lookup function
        s.write(f"const SdfGlyph* {font_name}_get_glyph(uint32_t codepoint)\n")
        s.write(f"{{\n")
        s.write(f"    if (codepoint >= 32 && codepoint <= 126)\n")
        s.write(f"    {{\n")
        s.write(f"        return &{font_name}_ascii[codepoint - 32];\n")
        s.write(f"    }}\n")
        if extra_glyphs:
            s.write(f"    for (size_t i = 0; i < {len(extra_glyphs)}; ++i)\n")
            s.write(f"    {{\n")
            s.write(f"        if ({font_name}_extra[i].codepoint == codepoint)\n")
            s.write(f"        {{\n")
            s.write(f"            return &{font_name}_extra[i].glyph;\n")
            s.write(f"        }}\n")
            s.write(f"    }}\n")
        s.write(f"    return NULL;\n")
        s.write(f"}}\n\n")

        # Font Descriptor instance
        s.write(f"const SdfFont {font_name} =\n{{\n")
        s.write(f"    {em_height},\n")
        s.write(f"    {spread}f,\n")
        s.write(f"    {font_name}_data,\n")
        s.write(f"    {font_name}_get_glyph\n")
        s.write(f"}};\n")


if __name__ == '__main__':
    main()
