/*
 * Copyright (C) 2026 Uli Tessel
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef _TIME_H_
#define _TIME_H_
#endif

extern "C" {
#include <pebble.h>
}

#include "Matrix2D.hpp"
#include "TextLayout.hpp"
#include "SdfRenderer.hpp"

static Window *s_window = nullptr;
static Layer *s_canvas_layer = nullptr;
static AppTimer *s_anim_timer = nullptr;

static TextLayout s_layout1;
static TextLayout s_layout2;
static TextLayout s_layout_dim;
static uint32_t s_frame_count = 0;

/* Set to 1 to enable rolling 1-second performance logging via APP_LOG */
#ifndef ENABLE_PROFILING
#define ENABLE_PROFILING 0
#endif

#if ENABLE_PROFILING
static uint32_t get_time_ms_u32(void)
{
    time_t sec = 0;
    uint16_t ms = 0;
    time_ms(&sec, &ms);
    return static_cast<uint32_t>(sec) * 1000u + static_cast<uint32_t>(ms);
}

struct FrameProfileData
{
    uint32_t sample_count = 0;
    uint32_t sum_frame_ms = 0;
    uint32_t min_frame_ms = 0xFFFFFFFF;
    uint32_t max_frame_ms = 0;
    uint32_t sum_clear_ms = 0;
    uint32_t sum_raster_ms = 0;
    uint32_t sum_interval_ms = 0;
    uint32_t sum_scanned_px = 0;
    uint32_t sum_sampled_px = 0;
    float current_scale = 0.0f;
};

static FrameProfileData s_prof;
static uint32_t s_last_frame_start_ms = 0;
#endif

static void anim_timer_callback(void *data)
{
    (void)data;
    if (!s_canvas_layer)
    {
        s_anim_timer = nullptr;
        return;
    }
    s_frame_count++;
    layer_mark_dirty(s_canvas_layer);
    s_anim_timer = app_timer_register(33, anim_timer_callback, nullptr); /* ~30 FPS */
}

static void canvas_update_proc(Layer *layer, GContext *ctx)
{
#if ENABLE_PROFILING
    uint32_t t_frame_start = get_time_ms_u32();
    if (s_last_frame_start_ms > 0)
    {
        uint32_t interval = t_frame_start - s_last_frame_start_ms;
        s_prof.sum_interval_ms += interval;
    }
    s_last_frame_start_ms = t_frame_start;
#endif

    GRect bounds = layer_get_bounds(layer);

    /* Fill background black */
#if ENABLE_PROFILING
    uint32_t t_clear_start = get_time_ms_u32();
#endif
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
#if ENABLE_PROFILING
    uint32_t clear_ms = get_time_ms_u32() - t_clear_start;
#endif

    /* Capture hardware framebuffer */
    GBitmap *fb = graphics_capture_frame_buffer(ctx);
    if (!fb)
    {
        return;
    }

    uint8_t *fb_data = gbitmap_get_data(fb);
    int row_bytes = gbitmap_get_bytes_per_row(fb);

    /* Dynamic counter string for line 2 (live layout on every frame!) */
    char counter_str[32];
    snprintf(counter_str, sizeof(counter_str), "Zähler: %lu", (unsigned long)s_frame_count);
    s_layout2.Layout(counter_str);

    /* Stable reference string replacing digits with '0' to avoid jitter from proportional '1' */
    char dim_str[32];
    size_t i = 0;
    for (; counter_str[i] != '\0' && i < sizeof(dim_str) - 1; ++i)
    {
        char c = counter_str[i];
        dim_str[i] = (c >= '0' && c <= '9') ? '0' : c;
    }
    dim_str[i] = '\0';
    s_layout_dim.Layout(dim_str);

    /* Calculate animation parameters from elapsed frames */
    float t = static_cast<float>(s_frame_count) * 0.033f;

    /* Base scale adapted to widest line */
    float max_w = (s_layout1.GetWidth() > s_layout_dim.GetWidth()) ? s_layout1.GetWidth() : s_layout_dim.GetWidth();
    float target_scale = static_cast<float>(bounds.size.w) / (max_w + 24.0f);
    if (target_scale < 1.0f)
    {
        target_scale = 1.0f;
    }

    float scale = target_scale * (0.85f + 0.20f * Matrix2D::FastSin(t * 2.2f));
    float angle_deg = 10.0f * Matrix2D::FastSin(t * 3.0f);
#if ENABLE_PROFILING
    s_prof.current_scale = scale;
#endif

    /* Construct shared 2D transformation matrix:
     * ScreenCenter * Rotate * Scale
     */
    Matrix2D base_M = Matrix2D::Translation(static_cast<float>(bounds.size.w) * 0.5f,
                                            static_cast<float>(bounds.size.h) * 0.5f)
                    * Matrix2D::RotationDeg(angle_deg)
                    * Matrix2D::UniformScaling(scale);

    /* Line 1: slightly above center (-11 font pixels)
     * Line 2: slightly below center (+11 font pixels), anchored to stable reference center
     */
    Matrix2D M1 = base_M * Matrix2D::Translation(-s_layout1.center_x, -s_layout1.center_y - 11.0f);
    Matrix2D M2 = base_M * Matrix2D::Translation(-s_layout_dim.center_x, -s_layout_dim.center_y + 11.0f);

    /* Render both text lines into framebuffer with sub-pixel anti-aliasing.
     * Line 1 uses a bolder weight (+0.25px dilation) for clear typographic hierarchy,
     * while Line 2 renders at regular weight (0.0px) using the exact same font table!
     */
#if ENABLE_PROFILING
    RenderStats stats1;
    RenderStats stats2;
    uint32_t t_raster_start = get_time_ms_u32();
    SdfRenderer::DrawToFramebuffer8Bit(fb_data, bounds.size.w, bounds.size.h, row_bytes,
                                       s_layout1, M1, 1.0f, 0.45f, &stats1);
    SdfRenderer::DrawToFramebuffer8Bit(fb_data, bounds.size.w, bounds.size.h, row_bytes,
                                       s_layout2, M2, 1.0f, 0.0f, &stats2);
    uint32_t raster_ms = get_time_ms_u32() - t_raster_start;
#else
    SdfRenderer::DrawToFramebuffer8Bit(fb_data, bounds.size.w, bounds.size.h, row_bytes,
                                       s_layout1, M1, 1.0f, 0.45f);
    SdfRenderer::DrawToFramebuffer8Bit(fb_data, bounds.size.w, bounds.size.h, row_bytes,
                                       s_layout2, M2, 1.0f, 0.0f);
#endif

    graphics_release_frame_buffer(ctx, fb);

#if ENABLE_PROFILING
    uint32_t frame_total_ms = get_time_ms_u32() - t_frame_start;

    /* Accumulate profiling stats */
    s_prof.sample_count++;
    s_prof.sum_frame_ms += frame_total_ms;
    s_prof.sum_clear_ms += clear_ms;
    s_prof.sum_raster_ms += raster_ms;
    s_prof.sum_scanned_px += (stats1.pixels_scanned + stats2.pixels_scanned);
    s_prof.sum_sampled_px += (stats1.pixels_sampled + stats2.pixels_sampled);
    if (frame_total_ms < s_prof.min_frame_ms)
    {
        s_prof.min_frame_ms = frame_total_ms;
    }
    if (frame_total_ms > s_prof.max_frame_ms)
    {
        s_prof.max_frame_ms = frame_total_ms;
    }
    if (frame_total_ms > s_prof.max_frame_ms)
    {
        s_prof.max_frame_ms = frame_total_ms;
    }

    /* Emit log every 30 frames (~1 sec) */
    if (s_prof.sample_count >= 30)
    {
        uint32_t avg_frame = s_prof.sum_frame_ms / s_prof.sample_count;
        uint32_t avg_raster = s_prof.sum_raster_ms / s_prof.sample_count;
        uint32_t avg_clear = s_prof.sum_clear_ms / s_prof.sample_count;
        uint32_t avg_scanned = s_prof.sum_scanned_px / s_prof.sample_count;
        uint32_t avg_sampled = s_prof.sum_sampled_px / s_prof.sample_count;
        uint32_t avg_interval = (s_prof.sum_interval_ms > 0) ? (s_prof.sum_interval_ms / s_prof.sample_count) : 33;
        uint32_t fps_x10 = (avg_interval > 0) ? (10000u / avg_interval) : 0;
        int scale_int = static_cast<int>(s_prof.current_scale);
        int scale_dec = static_cast<int>((s_prof.current_scale - static_cast<float>(scale_int)) * 100.0f);
        if (scale_dec < 0)
        {
            scale_dec = 0;
        }

        APP_LOG(APP_LOG_LEVEL_INFO,
                "[S:%d.%02d] %lu.%lufps | %lums (rast:%lu,clr:%lu) | scn:%lu,smp:%lu",
                scale_int, scale_dec,
                (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10),
                (unsigned long)avg_frame, (unsigned long)avg_raster, (unsigned long)avg_clear,
                (unsigned long)avg_scanned, (unsigned long)avg_sampled);

        /* Reset accumulator */
        s_prof.sample_count = 0;
        s_prof.sum_frame_ms = 0;
        s_prof.min_frame_ms = 0xFFFFFFFF;
        s_prof.max_frame_ms = 0;
        s_prof.sum_clear_ms = 0;
        s_prof.sum_raster_ms = 0;
        s_prof.sum_interval_ms = 0;
        s_prof.sum_scanned_px = 0;
        s_prof.sum_sampled_px = 0;
    }
#endif
}

static void window_load(Window *window)
{
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(window_layer, s_canvas_layer);

    /* Initialize layout EXACTLY ONCE! */
    s_layout1.Layout("Hallo Pebble!");

    /* Start animation timer (~30 FPS) */
    s_anim_timer = app_timer_register(33, anim_timer_callback, nullptr);
}

static void window_unload(Window *window)
{
    if (s_anim_timer)
    {
        app_timer_cancel(s_anim_timer);
        s_anim_timer = nullptr;
    }
    if (s_canvas_layer)
    {
        layer_destroy(s_canvas_layer);
        s_canvas_layer = nullptr;
    }
#if ENABLE_PROFILING
    s_last_frame_start_ms = 0;
    s_prof = FrameProfileData();
#endif
}

static void init(void)
{
    s_window = window_create();
    window_set_background_color(s_window, GColorBlack);
    window_set_window_handlers(s_window, (WindowHandlers){
        .load = window_load,
        .appear = nullptr,
        .disappear = nullptr,
        .unload = window_unload
    });
    window_stack_push(s_window, true);
}

static void deinit(void)
{
    if (s_anim_timer)
    {
        app_timer_cancel(s_anim_timer);
        s_anim_timer = nullptr;
    }
    if (s_window)
    {
        window_destroy(s_window);
        s_window = nullptr;
    }
}

int main(void)
{
    init();
    app_event_loop();
    deinit();
    return 0;
}
