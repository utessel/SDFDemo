#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "sdf_font.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================= */
/* 1. Grundlegende Typen: Farben & 2D Affine Matrix                          */
/* ========================================================================= */

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ColorRGB;

/* 2x3 affine Transformationsmatrix:
 * [ x' ]   [ a  b  tx ] [ x ]
 * [ y' ] = [ c  d  ty ] [ y ]
 * [ 1  ]   [ 0  0  1  ] [ 1 ]
 */
typedef struct
{
    float a, b, tx;
    float c, d, ty;
} Matrix2D;

static inline Matrix2D matrix2d_identity(void)
{
    return (Matrix2D){
        .a = 1.0f, .b = 0.0f, .tx = 0.0f,
        .c = 0.0f, .d = 1.0f, .ty = 0.0f
    };
}

static inline Matrix2D matrix2d_multiply(Matrix2D A, Matrix2D B)
{
    return (Matrix2D){
        .a  = A.a * B.a + A.b * B.c,
        .b  = A.a * B.b + A.b * B.d,
        .tx = A.a * B.tx + A.b * B.ty + A.tx,
        .c  = A.c * B.a + A.d * B.c,
        .d  = A.c * B.b + A.d * B.d,
        .ty = A.c * B.tx + A.d * B.ty + A.ty
    };
}

static inline Matrix2D matrix2d_invert(Matrix2D M)
{
    float det = M.a * M.d - M.b * M.c;
    if (fabsf(det) < 1e-8f)
    {
        return matrix2d_identity();
    }
    float inv_det = 1.0f / det;

    return (Matrix2D){
        .a  =  M.d * inv_det,
        .b  = -M.b * inv_det,
        .tx = (M.b * M.ty - M.d * M.tx) * inv_det,
        .c  = -M.c * inv_det,
        .d  =  M.a * inv_det,
        .ty = (M.c * M.tx - M.a * M.ty) * inv_det
    };
}

static inline void matrix2d_transform_point(Matrix2D M, float x, float y, float *out_x, float *out_y)
{
    *out_x = M.a * x + M.b * y + M.tx;
    *out_y = M.c * x + M.d * y + M.ty;
}

/* ========================================================================= */
/* 2. Text Metrik & Layout (Unabhängig von Transformation & Renderer)        */
/* ========================================================================= */

#define MAX_LAYOUT_GLYPHS 128

typedef struct
{
    int glyph_count;
    const SdfGlyph *glyphs[MAX_LAYOUT_GLYPHS];
    float cursor_offsets[MAX_LAYOUT_GLYPHS];
    float total_advance;

    /* Aggregate Bounding Box im unskalierten Font-Raum */
    float bb_min_x, bb_max_x;
    float bb_min_y, bb_max_y;

    /* Optisches Zentrum relativ zum String-Ursprung */
    float center_x;
    float center_y;
} TextLayout;

/* UTF-8 Decoder */
static uint32_t next_utf8(const char **ptr)
{
    const unsigned char *p = (const unsigned char *)*ptr;
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

    *ptr = (const char *)p;
    return cp;
}

/* Berechnet einmalig die Geometrie und das Layout eines UTF-8 Strings */
static void text_layout_create(TextLayout *layout, const char *text)
{
    memset(layout, 0, sizeof(TextLayout));

    layout->bb_min_x = 1e6f;
    layout->bb_max_x = -1e6f;
    layout->bb_min_y = 1e6f;
    layout->bb_max_y = -1e6f;

    const char *p = text;
    while (*p && layout->glyph_count < MAX_LAYOUT_GLYPHS)
    {
        uint32_t cp = next_utf8(&p);
        const SdfGlyph *g = notosans_16_get_glyph(cp);
        if (g)
        {
            int idx = layout->glyph_count;
            layout->glyphs[idx] = g;
            layout->cursor_offsets[idx] = layout->total_advance;
            layout->total_advance += (float)g->advance;

            if (g->width > 0 && g->height > 0)
            {
                float x0 = layout->cursor_offsets[idx] + (float)g->bearing_x;
                float x1 = x0 + (float)g->width;
                float y0 = -(float)g->bearing_y; /* Oberkante bzgl. Baseline */
                float y1 = y0 + (float)g->height;

                if (x0 < layout->bb_min_x) layout->bb_min_x = x0;
                if (x1 > layout->bb_max_x) layout->bb_max_x = x1;
                if (y0 < layout->bb_min_y) layout->bb_min_y = y0;
                if (y1 > layout->bb_max_y) layout->bb_max_y = y1;
            }
            layout->glyph_count++;
        }
    }

    if (layout->bb_min_x > layout->bb_max_x)
    {
        layout->bb_min_x = 0.0f;
        layout->bb_max_x = layout->total_advance;
        layout->bb_min_y = -12.0f;
        layout->bb_max_y = 0.0f;
    }

    layout->center_x = (layout->bb_min_x + layout->bb_max_x) * 0.5f;
    layout->center_y = (layout->bb_min_y + layout->bb_max_y) * 0.5f;
}

/* ========================================================================= */
/* 3. SDF-Sampling & Mathematischer Renderer mit Alpha-Blending              */
/* ========================================================================= */

static float sample_sdf_bilinear(const SdfGlyph *glyph, float u, float v)
{
    int w = glyph->width;
    int h = glyph->height;

    if (w <= 0 || h <= 0)
    {
        return SDF_SPREAD; /* outside */
    }

    float tx = u - 0.5f;
    float ty = v - 0.5f;

    int x0 = (int)floorf(tx);
    int y0 = (int)floorf(ty);
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float fx = tx - (float)x0;
    float fy = ty - (float)y0;

    #define GET_SDF_VAL(gx, gy) ({ \
        int cx = (gx) < 0 ? 0 : ((gx) >= w ? w - 1 : (gx)); \
        int cy = (gy) < 0 ? 0 : ((gy) >= h ? h - 1 : (gy)); \
        uint8_t byte = notosans_16_data[glyph->byte_offset + cy * w + cx]; \
        uint8_t nibble = (byte >> glyph->shift) & 0x0F; \
        -( ((float)nibble - 7.5f) / 7.5f ) * SDF_SPREAD; \
    })

    float v00 = GET_SDF_VAL(x0, y0);
    float v10 = GET_SDF_VAL(x1, y0);
    float v01 = GET_SDF_VAL(x0, y1);
    float v11 = GET_SDF_VAL(x1, y1);

    #undef GET_SDF_VAL

    float top = (1.0f - fx) * v00 + fx * v10;
    float bot = (1.0f - fx) * v01 + fx * v11;
    return (1.0f - fy) * top + fy * bot;
}

static inline float smoothstep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Alpha-Blending einer Pixelfarbe in den Zielpuffer */
static inline void blend_pixel(ColorRGB *dst, ColorRGB src, float alpha, bool pebble_shades)
{
    if (alpha <= 0.005f)
    {
        return;
    }

    if (pebble_shades)
    {
        /* Quantisierung auf 4 Stufen (Pebble Display) */
        int step = (int)roundf(alpha * 3.0f);
        alpha = (float)step / 3.0f;
    }

    float inv_a = 1.0f - alpha;
    dst->r = (uint8_t)(alpha * (float)src.r + inv_a * (float)dst->r);
    dst->g = (uint8_t)(alpha * (float)src.g + inv_a * (float)dst->g);
    dst->b = (uint8_t)(alpha * (float)src.b + inv_a * (float)dst->b);
}

/* Generischer Text-Renderer mit 2D Transformationsmatrix */
static void draw_sdf_text(ColorRGB *buffer, int screen_w, int screen_h,
                          const TextLayout *layout, Matrix2D matrix,
                          ColorRGB color, float filter_width_px, float weight_bias_px,
                          bool pebble_shades)
{
    if (layout->glyph_count == 0)
    {
        return;
    }

    /* Inverse Matrix für Screen -> Font-Raum Abtastung */
    Matrix2D inv_matrix = matrix2d_invert(matrix);

    /* Skalierungsfaktor aus Matrix für Screen-Space Distanz ableiten */
    float scale = sqrtf(matrix.a * matrix.a + matrix.c * matrix.c);
    if (scale < 1e-4f)
    {
        return;
    }

    float half_filter = filter_width_px * 0.5f;

    for (int gi = 0; gi < layout->glyph_count; ++gi)
    {
        const SdfGlyph *g = layout->glyphs[gi];
        if (g->width == 0 || g->height == 0)
        {
            continue;
        }

        /* 4 Ecken der Glyphe im Font-Raum */
        float gx0 = layout->cursor_offsets[gi] + (float)g->bearing_x;
        float gy0 = -(float)g->bearing_y;
        float gx1 = gx0 + (float)g->width;
        float gy1 = gy0 + (float)g->height;

        float corners[4][2] = {
            { gx0, gy0 }, { gx1, gy0 },
            { gx0, gy1 }, { gx1, gy1 }
        };

        /* Transformation nach Screen-Space für AABB */
        float min_sx = (float)screen_w, max_sx = 0.0f;
        float min_sy = (float)screen_h, max_sy = 0.0f;

        for (int c = 0; c < 4; ++c)
        {
            float sx, sy;
            matrix2d_transform_point(matrix, corners[c][0], corners[c][1], &sx, &sy);
            if (sx < min_sx) min_sx = sx;
            if (sx > max_sx) max_sx = sx;
            if (sy < min_sy) min_sy = sy;
            if (sy > max_sy) max_sy = sy;
        }

        int start_x = (int)floorf(min_sx - filter_width_px);
        int end_x   = (int)ceilf(max_sx + filter_width_px);
        int start_y = (int)floorf(min_sy - filter_width_px);
        int end_y   = (int)ceilf(max_sy + filter_width_px);

        if (start_x < 0) start_x = 0;
        if (end_x > screen_w) end_x = screen_w;
        if (start_y < 0) start_y = 0;
        if (end_y > screen_h) end_y = screen_h;

        /* Rasterung der Screen-AABB */
        for (int sy = start_y; sy < end_y; ++sy)
        {
            for (int sx = start_x; sx < end_x; ++sx)
            {
                /* Inverse Transformation: Screen-Pixel -> Font-Koordinaten */
                float font_x, font_y;
                matrix2d_transform_point(inv_matrix, (float)sx + 0.5f, (float)sy + 0.5f, &font_x, &font_y);

                /* Glyphen-lokale Texturkoordinate */
                float u = font_x - gx0;
                float v = font_y - gy0;

                if (u >= 0.0f && u <= (float)g->width && v >= 0.0f && v <= (float)g->height)
                {
                    float dist_font = sample_sdf_bilinear(g, u, v);
                    float dist_screen = (dist_font + weight_bias_px) * scale;
                    float alpha = smoothstep(half_filter, -half_filter, dist_screen);

                    blend_pixel(&buffer[sy * screen_w + sx], color, alpha, pebble_shades);
                }
            }
        }
    }
}

/* ========================================================================= */
/* 4. Terminal-Ausgabe mit Half-Block '\u2580'                               */
/* ========================================================================= */

static void render_buffer_to_terminal(const ColorRGB *buffer, int width, int height)
{
    for (int y = 0; y < height; y += 2)
    {
        for (int x = 0; x < width; ++x)
        {
            ColorRGB top = buffer[y * width + x];
            ColorRGB bottom = (y + 1 < height) ? buffer[(y + 1) * width + x] : (ColorRGB){ 0, 0, 0 };

            printf("\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm\xe2\x96\x80",
                   top.r, top.g, top.b,
                   bottom.r, bottom.b, bottom.b);
        }
        printf("\033[0m\n");
    }
}

/* ========================================================================= */
/* 5. Demo / Animation (Berechnet Matrix & ruft generischen Renderer)        */
/* ========================================================================= */

static volatile bool g_running = true;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = false;
}

static void run_animation(const char *text)
{
    /* 1. Text-Layout & Geometrie GENAU EINMAL berechnen! */
    TextLayout layout;
    text_layout_create(&layout, text);

    int width = 80;
    int height = 40;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 5)
    {
        width = ws.ws_col;
        height = (ws.ws_row - 2) * 2;
    }
    if (height % 2 != 0) height++;

    ColorRGB *buffer = (ColorRGB *)calloc(width * height, sizeof(ColorRGB));
    if (!buffer) return;

    printf("\033[?25l\033[2J\033[H");
    fflush(stdout);
    signal(SIGINT, sigint_handler);

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    while (g_running)
    {
        /* Terminalgröße live anpassen */
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 5)
        {
            int new_w = ws.ws_col;
            int new_h = (ws.ws_row - 2) * 2;
            if (new_h % 2 != 0) new_h++;
            if (new_w != width || new_h != height)
            {
                width = new_w;
                height = new_h;
                free(buffer);
                buffer = (ColorRGB *)calloc(width * height, sizeof(ColorRGB));
                printf("\033[2J\033[H");
            }
        }

        struct timespec cur_time;
        clock_gettime(CLOCK_MONOTONIC, &cur_time);
        double t = (cur_time.tv_sec - start_time.tv_sec) +
                   (cur_time.tv_nsec - start_time.tv_nsec) * 1e-9;

        /* Dynamische Skalierung & Drehung berechnen */
        float target_scale = ((float)width / (layout.bb_max_x - layout.bb_min_x + 10.0f));
        if (target_scale < 1.0f) target_scale = 1.0f;

        float scale = target_scale * (0.85f + 0.25f * sinf((float)(t * 2.2)));
        float angle_deg = 11.0f * sinf((float)(t * 3.2));
        float rad = -angle_deg * ((float)M_PI / 180.0f);

        /* 2. Transformationsmatrix aufbauen:
         * Matrix = Translate(Center) * Rotate(angle) * Scale(scale) * Translate(-LayoutCenter)
         */
        Matrix2D M_center_font = { 1, 0, -layout.center_x,  0, 1, -layout.center_y };
        Matrix2D M_scale       = { scale, 0, 0,  0, scale, 0 };
        Matrix2D M_rot         = { cosf(rad), -sinf(rad), 0,  sinf(rad), cosf(rad), 0 };
        Matrix2D M_center_scr  = { 1, 0, (float)width * 0.5f,  0, 1, (float)height * 0.5f };

        Matrix2D M = matrix2d_multiply(M_center_scr,
                     matrix2d_multiply(M_rot,
                     matrix2d_multiply(M_scale, M_center_font)));

        /* Hintergrund löschen */
        memset(buffer, 0, width * height * sizeof(ColorRGB));

        /* 3. Generischen Renderer mit Matrix aufrufen */
        draw_sdf_text(buffer, width, height, &layout, M,
                      (ColorRGB){ 255, 255, 255 }, 1.0f, 0.0f, true);

        /* Terminal aktualisieren */
        printf("\033[H");
        render_buffer_to_terminal(buffer, width, height);
        printf("\033[33m[SDF Demo] Text: \"%s\" | Terminal: %dx%d | Scale: %.2fx | Rot: %+.1f°\033[0m\n",
               text, ws.ws_col, ws.ws_row, scale, angle_deg);
        fflush(stdout);

        usleep(33000); /* ~30 FPS */
    }

    printf("\033[?25h\033[0m\n");
    free(buffer);
}

/* ========================================================================= */
/* 6. Einzel-Glyphen Inspektion                                              */
/* ========================================================================= */

static void show_single_glyph(const char *char_to_test, float scale, float filter_width, float weight_bias)
{
    const char *p = char_to_test;
    uint32_t codepoint = next_utf8(&p);

    const SdfGlyph *glyph = notosans_16_get_glyph(codepoint);
    if (!glyph)
    {
        printf("Glyphe für '%s' (Codepoint 0x%04X) nicht gefunden!\n", char_to_test, codepoint);
        return;
    }

    int w = glyph->width;
    int h = glyph->height;
    printf("\n=== 1. Rohes 4-Bit SDF: '%s' (%dx%d Pixel, Shift=%d, Offset=%u) ===\n",
           char_to_test, w, h, glyph->shift, glyph->byte_offset);

    printf("\n--- Hex Matrix (0=Aussen, 8=Kontur, F=Innen) ---\n");
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            uint8_t byte = notosans_16_data[glyph->byte_offset + y * w + x];
            uint8_t val = (byte >> glyph->shift) & 0x0F;
            printf("%X ", val);
        }
        printf("\n");
    }
    printf("-------------------------------------------------\n\n");

    TextLayout layout;
    text_layout_create(&layout, char_to_test);

    int out_w = (int)ceilf((float)w * scale * 1.5f);
    int out_h = (int)ceilf((float)h * scale * 1.5f);
    if (out_h % 2 != 0) out_h++;

    ColorRGB *buffer = (ColorRGB *)calloc(out_w * out_h, sizeof(ColorRGB));
    if (buffer)
    {
        Matrix2D M_center_font = { 1, 0, -layout.center_x,  0, 1, -layout.center_y };
        Matrix2D M_scale       = { scale, 0, 0,  0, scale, 0 };
        Matrix2D M_center_scr  = { 1, 0, (float)out_w * 0.5f,  0, 1, (float)out_h * 0.5f };
        Matrix2D M = matrix2d_multiply(M_center_scr, matrix2d_multiply(M_scale, M_center_font));

        draw_sdf_text(buffer, out_w, out_h, &layout, M,
                      (ColorRGB){ 255, 255, 255 }, filter_width, weight_bias, true);

        printf("=== 2. Rasterung: '%s' (Pebble 4-Graustufen LCD) ===\n", char_to_test);
        render_buffer_to_terminal(buffer, out_w, out_h);
        free(buffer);
    }
}

int main(int argc, char **argv)
{
    if (argc <= 1)
    {
        run_animation("Hello World");
        return 0;
    }

    if (strcmp(argv[1], "--animate") == 0 || strcmp(argv[1], "-a") == 0)
    {
        const char *text = (argc > 2) ? argv[2] : "Hello World";
        run_animation(text);
        return 0;
    }

    if (strlen(argv[1]) > 4 || strchr(argv[1], ' ') != NULL)
    {
        run_animation(argv[1]);
        return 0;
    }

    float scale = (argc > 2) ? strtof(argv[2], NULL) : 4.0f;
    float filter_width = (argc > 3) ? strtof(argv[3], NULL) : 1.0f;
    float weight_bias = (argc > 4) ? strtof(argv[4], NULL) : 0.0f;

    show_single_glyph(argv[1], scale, filter_width, weight_bias);
    return 0;
}
