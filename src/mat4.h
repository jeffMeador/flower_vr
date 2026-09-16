#pragma once
#include <cstring>
#include <cmath>
#include <algorithm>

// Row-major 4x4, matching HLSL's default cbuffer packing for float4x4
// (each row is one float4). v' = v * M (row-vector convention).
struct Mat4
{
    float m[4][4];
};

inline Mat4 Mat4Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// General 4x4 inverse via Gauss-Jordan elimination with partial pivoting.
// Convention-agnostic: works on whatever in.m[row][col] means, since it
// operates on the augmented [in | Identity] matrix directly.
inline bool Mat4Inverse(const Mat4& in, Mat4& out)
{
    float a[4][8];
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c) a[r][c] = in.m[r][c];
        for (int c = 0; c < 4; ++c) a[r][4 + c] = (r == c) ? 1.0f : 0.0f;
    }

    for (int col = 0; col < 4; ++col)
    {
        int pivot = col;
        float best = fabsf(a[col][col]);
        for (int r = col + 1; r < 4; ++r)
        {
            float v = fabsf(a[r][col]);
            if (v > best) { best = v; pivot = r; }
        }
        if (best < 1e-9f) return false;

        if (pivot != col)
            for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);

        float pivotVal = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= pivotVal;

        for (int r = 0; r < 4; ++r)
        {
            if (r == col) continue;
            float factor = a[r][col];
            if (factor == 0.0f) continue;
            for (int c = 0; c < 8; ++c) a[r][c] -= factor * a[col][c];
        }
    }

    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[r][c] = a[r][4 + c];
    return true;
}
