/*
 * Matrix2D.hpp - 2D Affine Transformation Matrix
 *
 * This file is released into the public domain (or CC0 / Unlicense).
 * Free for any use, modification, and integration without restriction.
 */

#pragma once

#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
};

/* 2x3 Affine 2D Transformation Matrix:
 * [ x' ]   [ a  b  tx ] [ x ]
 * [ y' ] = [ c  d  ty ] [ y ]
 * [ 1  ]   [ 0  0  1  ] [ 1 ]
 */
class Matrix2D
{
public:
    float a  = 1.0f;
    float b  = 0.0f;
    float tx = 0.0f;
    float c  = 0.0f;
    float d  = 1.0f;
    float ty = 0.0f;

    Matrix2D() = default;

    Matrix2D(float a_, float b_, float tx_, float c_, float d_, float ty_)
        : a(a_), b(b_), tx(tx_), c(c_), d(d_), ty(ty_)
    {
    }

    static inline float FastSqrt(float x)
    {
        if (x <= 0.0f)
        {
            return 0.0f;
        }
        union { float f; uint32_t i; } conv = { x };
        conv.i = 0x5f3759df - (conv.i >> 1);
        float y = conv.f;
        y = y * (1.5f - 0.5f * x * y * y);
        y = y * (1.5f - 0.5f * x * y * y);
        return x * y;
    }

    static inline float FastSin(float rad)
    {
        constexpr float PI = 3.14159265358979323846f;
        constexpr float TWO_PI = 2.0f * PI;
        constexpr float INV_TWO_PI = 1.0f / TWO_PI;

        float k = rad * INV_TWO_PI;
        int ki = static_cast<int>(k >= 0.0f ? k + 0.5f : k - 0.5f);
        rad -= static_cast<float>(ki) * TWO_PI;

        if (rad > PI * 0.5f)
        {
            rad = PI - rad;
        }
        else if (rad < -PI * 0.5f)
        {
            rad = -PI - rad;
        }

        float x2 = rad * rad;
        return rad * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f - x2 * (1.0f / 5040.0f))));
    }

    static inline float FastCos(float rad)
    {
        constexpr float HALF_PI = 1.5707963267948966f;
        return FastSin(rad + HALF_PI);
    }

    static Matrix2D Identity()
    {
        return Matrix2D(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    }

    static Matrix2D Translation(float dx, float dy)
    {
        return Matrix2D(1.0f, 0.0f, dx, 0.0f, 1.0f, dy);
    }

    static Matrix2D Rotation(float rad)
    {
        float cos_a = FastCos(rad);
        float sin_a = FastSin(rad);
        return Matrix2D(cos_a, -sin_a, 0.0f, sin_a, cos_a, 0.0f);
    }

    static Matrix2D RotationDeg(float deg)
    {
        return Rotation(-deg * static_cast<float>(M_PI / 180.0));
    }

    static Matrix2D Scaling(float sx, float sy)
    {
        return Matrix2D(sx, 0.0f, 0.0f, 0.0f, sy, 0.0f);
    }

    static Matrix2D UniformScaling(float s)
    {
        return Scaling(s, s);
    }

    Matrix2D operator*(const Matrix2D &rhs) const
    {
        return Matrix2D(
            a * rhs.a + b * rhs.c,
            a * rhs.b + b * rhs.d,
            a * rhs.tx + b * rhs.ty + tx,
            c * rhs.a + d * rhs.c,
            c * rhs.b + d * rhs.d,
            c * rhs.tx + d * rhs.ty + ty
        );
    }

    Vec2 Transform(const Vec2 &p) const
    {
        return Vec2(
            a * p.x + b * p.y + tx,
            c * p.x + d * p.y + ty
        );
    }

    Vec2 operator*(const Vec2 &p) const
    {
        return Transform(p);
    }

    float Determinant() const
    {
        return a * d - b * c;
    }

    Matrix2D Inverted() const
    {
        float det = Determinant();
        float abs_det = (det < 0.0f) ? -det : det;
        if (abs_det < 1e-8f)
        {
            return Identity();
        }
        float inv = 1.0f / det;
        return Matrix2D(
             d * inv,
            -b * inv,
            (b * ty - d * tx) * inv,
            -c * inv,
             a * inv,
            (c * tx - a * ty) * inv
        );
    }

    float GetScaleX() const
    {
        return FastSqrt(a * a + c * c);
    }
};
