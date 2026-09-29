/*
Small demo program to render a SDF font in the terminal with live scaling and rotation.
*/

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

#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <ctime>
#include <unistd.h>
#include <csignal>
#include <sys/ioctl.h>
#include <vector>
#include <string>

#include "sdf_font.h"
#include "Matrix2D.hpp"
#include "TextLayout.hpp"
#include "SdfRenderer.hpp"

static volatile bool g_running = true;

static void SigintHandler(int sig)
{
    (void)sig;
    g_running = false;
}

static void RenderBufferToTerminal(const ColorRGB *buffer, int width, int height)
{
    for (int y = 0; y < height; y += 2)
    {
        for (int x = 0; x < width; ++x)
        {
            ColorRGB top = buffer[y * width + x];
            ColorRGB bottom = (y + 1 < height) ? buffer[(y + 1) * width + x] : ColorRGB{ 0, 0, 0 };

            std::printf("\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm\xe2\x96\x80",
                        top.r, top.g, top.b,
                        bottom.r, bottom.g, bottom.b);
        }
        std::printf("\033[0m\n");
    }
}

struct FrameProfile
{
    uint32_t count = 0;
    double sum_clear_us = 0.0;
    double sum_render_us = 0.0;
    double sum_output_us = 0.0;
    double sum_total_us = 0.0;
    double min_render_us = 1e18;
    double max_render_us = 0.0;
    double min_total_us = 1e18;
    double max_total_us = 0.0;
    uint64_t sum_scanned = 0;
    uint64_t sum_sampled = 0;
    uint64_t sum_written = 0;
};

static double TimespecDiffUs(const struct timespec &a, const struct timespec &b)
{
    return (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) * 1e-3;
}

static void PrintProfile(const FrameProfile &prof, int width, int height)
{
    if (prof.count == 0)
    {
        return;
    }

    double n = static_cast<double>(prof.count);
    double avg_total = prof.sum_total_us / n;
    double avg_clear = prof.sum_clear_us / n;
    double avg_render = prof.sum_render_us / n;
    double avg_output = prof.sum_output_us / n;
    double fps = (avg_total > 0.0) ? 1e6 / avg_total : 0.0;

    std::printf("\n");
    std::printf("=== Profiling Summary (%u frames, %dx%d) ===\n\n", prof.count, width, height);
    std::printf("  Frame total:   %8.0f us avg  (min: %.0f, max: %.0f)\n",
                avg_total, prof.min_total_us, prof.max_total_us);
    std::printf("    Clear:       %8.0f us avg  (%4.1f%%)\n", avg_clear, avg_clear / avg_total * 100.0);
    std::printf("    Render:      %8.0f us avg  (%4.1f%%)  (min: %.0f, max: %.0f)\n",
                avg_render, avg_render / avg_total * 100.0, prof.min_render_us, prof.max_render_us);
    std::printf("    Term output: %8.0f us avg  (%4.1f%%)\n", avg_output, avg_output / avg_total * 100.0);
    std::printf("  FPS:           %8.1f (without usleep)\n\n", fps);
    std::printf("  Pixels/frame:  scanned=%lu  sampled=%lu  written=%lu\n",
                static_cast<unsigned long>(prof.sum_scanned / prof.count),
                static_cast<unsigned long>(prof.sum_sampled / prof.count),
                static_cast<unsigned long>(prof.sum_written / prof.count));
    if (prof.sum_scanned > 0)
    {
        std::printf("  Hit rate:      %.1f%% of scanned pixels needed sampling\n",
                    static_cast<double>(prof.sum_sampled) / static_cast<double>(prof.sum_scanned) * 100.0);
    }
    std::printf("\n");
}

static void RunAnimation(const char *text, bool profiling, float opacity)
{
    TextLayout layout;
    layout.Layout(text);

    int width = 80;
    int height = 40;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 5)
    {
        width = ws.ws_col;
        height = (ws.ws_row - 2) * 2;
    }
    if (height % 2 != 0) height++;

    std::vector<ColorRGB> buffer(width * height, ColorRGB{ 0, 0, 0 });

    std::printf("\033[?25l\033[2J\033[H");
    std::fflush(stdout);
    std::signal(SIGINT, SigintHandler);

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    FrameProfile prof;

    while (g_running)
    {
        struct timespec t_frame_start;
        if (profiling) clock_gettime(CLOCK_MONOTONIC, &t_frame_start);

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 5)
        {
            int new_w = ws.ws_col;
            int new_h = (ws.ws_row - 2) * 2;
            if (new_h % 2 != 0) new_h++;
            if (new_w != width || new_h != height)
            {
                width = new_w;
                height = new_h;
                buffer.assign(width * height, ColorRGB{ 0, 0, 0 });
                std::printf("\033[2J\033[H");
            }
        }

        struct timespec cur_time;
        clock_gettime(CLOCK_MONOTONIC, &cur_time);
        double t = (cur_time.tv_sec - start_time.tv_sec) +
                   (cur_time.tv_nsec - start_time.tv_nsec) * 1e-9;

        float target_scale = (static_cast<float>(width) / (layout.GetWidth() + 10.0f));
        if (target_scale < 1.0f) target_scale = 1.0f;

        float scale = target_scale * (0.85f + 0.25f * std::sin(static_cast<float>(t * 2.2)));
        float angle_deg = 11.0f * std::sin(static_cast<float>(t * 3.2));

        Matrix2D M = Matrix2D::Translation(static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f)
                   * Matrix2D::RotationDeg(angle_deg)
                   * Matrix2D::UniformScaling(scale)
                   * Matrix2D::Translation(-layout.center_x, -layout.center_y);

        struct timespec t_clear_start;
        if (profiling) clock_gettime(CLOCK_MONOTONIC, &t_clear_start);

        std::fill(buffer.begin(), buffer.end(), ColorRGB{ 0, 0, 0 });

        struct timespec t_render_start;
        if (profiling) clock_gettime(CLOCK_MONOTONIC, &t_render_start);

        RenderStats render_stats;
        SdfRenderer::DrawText(buffer.data(), width, height, layout, M,
                              ColorRGB{ 255, 255, 255 }, opacity, 1.0f, 0.0f,
                              profiling ? &render_stats : nullptr);

        struct timespec t_output_start;
        if (profiling) clock_gettime(CLOCK_MONOTONIC, &t_output_start);

        std::printf("\033[H");
        RenderBufferToTerminal(buffer.data(), width, height);

        std::printf("\033[33m[SDF C++ Demo] \"%s\" | Terminal: %dx%d | Scale: %.2fx | Rot: %+.1f° | Ctrl+C to exit\033[0m\n",
                    text, ws.ws_col, ws.ws_row, scale, angle_deg);
        std::fflush(stdout);

        if (profiling)
        {
            struct timespec t_frame_end;
            clock_gettime(CLOCK_MONOTONIC, &t_frame_end);

            double render_us = TimespecDiffUs(t_render_start, t_output_start);
            double total_us = TimespecDiffUs(t_frame_start, t_frame_end);

            prof.count++;
            prof.sum_clear_us += TimespecDiffUs(t_clear_start, t_render_start);
            prof.sum_render_us += render_us;
            prof.sum_output_us += TimespecDiffUs(t_output_start, t_frame_end);
            prof.sum_total_us += total_us;
            prof.sum_scanned += render_stats.pixels_scanned;
            prof.sum_sampled += render_stats.pixels_sampled;
            prof.sum_written += render_stats.pixels_written;

            if (render_us < prof.min_render_us) prof.min_render_us = render_us;
            if (render_us > prof.max_render_us) prof.max_render_us = render_us;
            if (total_us < prof.min_total_us) prof.min_total_us = total_us;
            if (total_us > prof.max_total_us) prof.max_total_us = total_us;
        }

        usleep(33000);
    }

    std::printf("\033[?25h\033[0m");

    if (profiling)
    {
        PrintProfile(prof, width, height);
    }
    else
    {
        std::printf("\n");
    }
}

static void RunBenchmark(const char *text, int num_frames, int width, int height,
                         bool fb8, bool checksum_enabled, float fixed_scale, float opacity)
{
    TextLayout layout;
    layout.Layout(text);

    std::vector<ColorRGB> buffer(width * height, ColorRGB{ 0, 0, 0 });
    std::vector<uint8_t> fb(fb8 ? width * height : 0, 0xC0);

    float target_scale = (static_cast<float>(width) / (layout.GetWidth() + 10.0f));
    if (target_scale < 1.0f) target_scale = 1.0f;
    if (fixed_scale > 0.0f) target_scale = fixed_scale;

    std::printf("Benchmark: %d frames, %dx%d px, text=\"%s\", base_scale=%.2f, opacity=%.2f, renderer=%s\n",
                num_frames, width, height, text, target_scale, opacity,
                fb8 ? "DrawToFramebuffer8Bit (Pebble)" : "DrawText (RGB)");

    uint64_t total_scanned = 0;
    uint64_t total_sampled = 0;
    uint64_t total_written = 0;

    double clear_us = 0.0;
    double render_us = 0.0;
    uint64_t checksum = 1469598103934665603ULL; /* FNV-1a over all frames, not timed */

    for (int frame = 0; frame < num_frames; ++frame)
    {
        double t = frame * (1.0 / 30.0);

        float scale = target_scale * (0.85f + 0.25f * std::sin(static_cast<float>(t * 2.2)));
        float angle_deg = 11.0f * std::sin(static_cast<float>(t * 3.2));

        Matrix2D base_M = Matrix2D::Translation(static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f)
                        * Matrix2D::RotationDeg(angle_deg)
                        * Matrix2D::UniformScaling(scale);

        struct timespec tc0, tc1, tr1;
        RenderStats stats;

        if (fb8)
        {
            /* Same setup as the Pebble demo: bold line above, regular line below */
            Matrix2D M1 = base_M * Matrix2D::Translation(-layout.center_x, -layout.center_y - 11.0f);
            Matrix2D M2 = base_M * Matrix2D::Translation(-layout.center_x, -layout.center_y + 11.0f);

            clock_gettime(CLOCK_MONOTONIC, &tc0);
            std::fill(fb.begin(), fb.end(), static_cast<uint8_t>(0xC0));
            clock_gettime(CLOCK_MONOTONIC, &tc1);

            SdfRenderer::DrawToFramebuffer8Bit(fb.data(), width, height, width,
                                               layout, M1, 1.0f, 0.45f, &stats);
            SdfRenderer::DrawToFramebuffer8Bit(fb.data(), width, height, width,
                                               layout, M2, 1.0f, 0.0f, &stats);
            clock_gettime(CLOCK_MONOTONIC, &tr1);
        }
        else
        {
            Matrix2D M = base_M * Matrix2D::Translation(-layout.center_x, -layout.center_y);

            clock_gettime(CLOCK_MONOTONIC, &tc0);
            std::fill(buffer.begin(), buffer.end(), ColorRGB{ 0, 0, 0 });
            clock_gettime(CLOCK_MONOTONIC, &tc1);

            SdfRenderer::DrawText(buffer.data(), width, height, layout, M,
                                  ColorRGB{ 255, 255, 255 }, opacity, 1.0f, 0.0f, &stats);
            clock_gettime(CLOCK_MONOTONIC, &tr1);
        }

        clear_us += TimespecDiffUs(tc0, tc1);
        render_us += TimespecDiffUs(tc1, tr1);

        total_scanned += stats.pixels_scanned;
        total_sampled += stats.pixels_sampled;
        total_written += stats.pixels_written;

        if (checksum_enabled)
        {
            const uint8_t *bytes = fb8 ? fb.data() : reinterpret_cast<const uint8_t *>(buffer.data());
            size_t num_bytes = fb8 ? fb.size() : buffer.size() * sizeof(ColorRGB);
            for (size_t i = 0; i < num_bytes; ++i)
            {
                checksum = (checksum ^ bytes[i]) * 1099511628211ULL;
            }
        }
    }

    double total_us = clear_us + render_us;
    double per_frame_us = total_us / num_frames;

    std::printf("\n=== Benchmark Result (%d frames, %dx%d) ===\n\n", num_frames, width, height);
    std::printf("  Clear:       %10.1f us/frame  (%4.1f%%)\n",
                clear_us / num_frames, clear_us / total_us * 100.0);
    std::printf("  Render:      %10.1f us/frame  (%4.1f%%)\n",
                render_us / num_frames, render_us / total_us * 100.0);
    std::printf("  Total:       %10.1f us/frame  (= %.0f fps)\n",
                per_frame_us, 1e6 / per_frame_us);
    std::printf("\n  Pixels/frame: scanned=%lu  sampled=%lu  written=%lu\n",
                static_cast<unsigned long>(total_scanned / num_frames),
                static_cast<unsigned long>(total_sampled / num_frames),
                static_cast<unsigned long>(total_written / num_frames));
    if (total_scanned > 0)
    {
        std::printf("  Hit rate:    %.1f%% sampled/scanned\n",
                    static_cast<double>(total_sampled) / static_cast<double>(total_scanned) * 100.0);
    }
    if (checksum_enabled)
    {
        std::printf("  Checksum:    %016llx\n", static_cast<unsigned long long>(checksum));
    }
    std::printf("\n");
}

int main(int argc, char **argv)
{
    std::string text = "Hello World";
    bool profiling = false;
    int benchmark_frames = 0;
    int bench_w = 192, bench_h = 80;
    bool bench_fb8 = false;
    bool bench_check = true;
    float bench_scale = 0.0f;
    float opacity = 1.0f;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--profile") == 0 || std::strcmp(argv[i], "-p") == 0)
        {
            profiling = true;
        }
        else if (std::strcmp(argv[i], "--bench") == 0 || std::strcmp(argv[i], "-b") == 0)
        {
            benchmark_frames = 1000;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
            {
                benchmark_frames = std::atoi(argv[++i]);
            }
        }
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
        {
            if (std::sscanf(argv[++i], "%dx%d", &bench_w, &bench_h) != 2)
            {
                std::fprintf(stderr, "Usage: --size WxH (e.g. 192x80)\n");
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--fb8") == 0)
        {
            bench_fb8 = true;
        }
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
        {
            bench_scale = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--opacity") == 0 && i + 1 < argc)
        {
            opacity = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--no-check") == 0)
        {
            bench_check = false;
        }
        else if (std::strcmp(argv[i], "--animate") == 0 || std::strcmp(argv[i], "-a") == 0)
        {
            /* kept for backwards compat */
        }
        else
        {
            text = argv[i];
        }
    }

    if (benchmark_frames > 0)
    {
        RunBenchmark(text.c_str(), benchmark_frames, bench_w, bench_h, bench_fb8, bench_check,
                     bench_scale, opacity);
    }
    else
    {
        RunAnimation(text.c_str(), profiling, opacity);
    }
    return 0;
}
