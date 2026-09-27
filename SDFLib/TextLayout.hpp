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
#include <cstddef>
#include "sdf_font.h"
#include "Matrix2D.hpp"

class TextLayout
{
public:
    static constexpr size_t MaxGlyphs = 128;

    struct GlyphItem
    {
        const SdfGlyph *glyph = nullptr;
        float cursor_offset = 0.0f;
    };

    int glyph_count = 0;
    GlyphItem items[MaxGlyphs];
    float total_advance = 0.0f;

    /* Font-space bounding box (untransformed) */
    float bb_min_x = 0.0f;
    float bb_max_x = 0.0f;
    float bb_min_y = 0.0f;
    float bb_max_y = 0.0f;

    /* Optical center relative to text origin */
    float center_x = 0.0f;
    float center_y = 0.0f;

    const SdfFont *font = &sdf_font;

    TextLayout() = default;

    /* Compute layout once for a UTF-8 string with a given SdfFont */
    void Layout(const char *utf8_text, const SdfFont *font_ = &sdf_font);

    float GetWidth() const
    {
        return bb_max_x - bb_min_x;
    }

    float GetHeight() const
    {
        return bb_max_y - bb_min_y;
    }

private:
    static uint32_t NextUtf8(const char **ptr);
};
