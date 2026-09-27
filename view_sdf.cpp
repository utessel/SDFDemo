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
                        bottom.r, bottom.b, bottom.b);
        }
        std::printf("\033[0m\n");
    }
}

static void RunAnimation(const char *text)
{
    /* 1. Calculate layout EXACTLY ONCE */
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

    while (g_running)
    {
        /* Live terminal resize */
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

        /* Dynamic scale & rotation angle */
        float target_scale = (static_cast<float>(width) / (layout.GetWidth() + 10.0f));
        if (target_scale < 1.0f) target_scale = 1.0f;

        float scale = target_scale * (0.85f + 0.25f * std::sin(static_cast<float>(t * 2.2)));
        float angle_deg = 11.0f * std::sin(static_cast<float>(t * 3.2));

        /* 2. Build transformation matrix:
         * M = Translate(ScreenCenter) * Rotate(angle) * Scale(scale) * Translate(-LayoutCenter)
         */
        Matrix2D M = Matrix2D::Translation(static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f)
                   * Matrix2D::RotationDeg(angle_deg)
                   * Matrix2D::UniformScaling(scale)
                   * Matrix2D::Translation(-layout.center_x, -layout.center_y);

        /* Clear buffer */
        std::fill(buffer.begin(), buffer.end(), ColorRGB{ 0, 0, 0 });

        /* 3. Invoke generic renderer */
        SdfRenderer::DrawText(buffer.data(), width, height, layout, M,
                              ColorRGB{ 255, 255, 255 }, 1.0f, 0.0f, true);

        /* Terminal output */
        std::printf("\033[H");
        RenderBufferToTerminal(buffer.data(), width, height);
        std::printf("\033[33m[SDF C++ Demo] \"%s\" | Terminal: %dx%d | Scale: %.2fx | Rot: %+.1f° | Ctrl+C to exit\033[0m\n",
                    text, ws.ws_col, ws.ws_row, scale, angle_deg);
        std::fflush(stdout);

        usleep(33000); /* ~30 FPS */
    }

    std::printf("\033[?25h\033[0m\n");
}

int main(int argc, char **argv)
{
    std::string text = "Hello World";
    if (argc > 1)
    {
        if (std::strcmp(argv[1], "--animate") == 0 || std::strcmp(argv[1], "-a") == 0)
        {
            if (argc > 2) text = argv[2];
        }
        else
        {
            text = argv[1];
        }
    }

    RunAnimation(text.c_str());
    return 0;
}
