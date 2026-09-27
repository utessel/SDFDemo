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

#include "TextLayout.hpp"
#include <cstring>

uint32_t TextLayout::NextUtf8(const char **ptr)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(*ptr);
    if (!p || *p == '\0')
    {
        return 0;
    }

    uint32_t cp = 0;
    if (*p < 0x80)
    {
        cp = *p++;
    }
    else if ((*p & 0xE0) == 0xC0 && *(p + 1))
    {
        cp = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
        p += 2;
    }
    else if ((*p & 0xF0) == 0xE0 && *(p + 1) && *(p + 2))
    {
        cp = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
        p += 3;
    }
    else
    {
        cp = *p++;
    }

    *ptr = reinterpret_cast<const char *>(p);
    return cp;
}

void TextLayout::Layout(const char *utf8_text, const SdfFont *font_)
{
    font = font_ ? font_ : &sdf_font;
    glyph_count = 0;
    total_advance = 0.0f;
    bb_min_x = 1e6f;
    bb_max_x = -1e6f;
    bb_min_y = 1e6f;
    bb_max_y = -1e6f;

    const char *p = utf8_text;
    while (*p && glyph_count < static_cast<int>(MaxGlyphs))
    {
        uint32_t cp = NextUtf8(&p);
        const SdfGlyph *g = font->get_glyph(cp);
        if (g)
        {
            items[glyph_count].glyph = g;
            items[glyph_count].cursor_offset = total_advance;
            total_advance += static_cast<float>(g->advance);

            if (g->width > 0 && g->height > 0)
            {
                float x0 = items[glyph_count].cursor_offset + static_cast<float>(g->bearing_x);
                float x1 = x0 + static_cast<float>(g->width);
                float y0 = -static_cast<float>(g->bearing_y);
                float y1 = y0 + static_cast<float>(g->height);

                if (x0 < bb_min_x) bb_min_x = x0;
                if (x1 > bb_max_x) bb_max_x = x1;
                if (y0 < bb_min_y) bb_min_y = y0;
                if (y1 > bb_max_y) bb_max_y = y1;
            }
            glyph_count++;
        }
    }

    if (bb_min_x > bb_max_x)
    {
        bb_min_x = 0.0f;
        bb_max_x = total_advance;
        bb_min_y = -12.0f;
        bb_max_y = 0.0f;
    }

    center_x = (bb_min_x + bb_max_x) * 0.5f;
    center_y = (bb_min_y + bb_max_y) * 0.5f;
}
