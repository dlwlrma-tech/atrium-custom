// The input method entry's letters as outlines (tools/generate-ime-glyphs.py; do not edit):
// 中 drawn by the script; the others from Noto Sans CJK SC Version 2.004, SIL Open Font License 1.1. Em units, centred on the ink, y down.
#pragma once

#include <span>
#include <string_view>

namespace atrium::ime_glyph {

enum class Op : unsigned char { Move, Line, Cubic, Close };

// A step of an outline: Move and Line use (x1, y1); Cubic its two controls and its end.
struct Cmd {
    Op op;
    float x1, y1, x2, y2, x3, y3;
};

struct Glyph {
    wchar_t ch;
    std::span<const Cmd> cmds;
};

// clang-format off: one step a line, as generated
// NOLINTBEGIN(modernize-use-std-numbers): outline coordinates, some of which merely resemble constants
inline constexpr Cmd kZhong[] = {
    {Op::Move, -0.29f, -0.27f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.29f, -0.27f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.34523f, -0.27f, 0.39f, -0.22523f, 0.39f, -0.17f},
    {Op::Line, 0.39f, 0.07f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.39f, 0.12523f, 0.34523f, 0.17f, 0.29f, 0.17f},
    {Op::Line, -0.29f, 0.17f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.34523f, 0.17f, -0.39f, 0.12523f, -0.39f, 0.07f},
    {Op::Line, -0.39f, -0.17f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.39f, -0.22523f, -0.34523f, -0.27f, -0.29f, -0.27f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.288f, -0.188f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.29905f, -0.188f, -0.308f, -0.17905f, -0.308f, -0.168f},
    {Op::Line, -0.308f, 0.068f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.308f, 0.07905f, -0.29905f, 0.088f, -0.288f, 0.088f},
    {Op::Line, 0.288f, 0.088f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.29905f, 0.088f, 0.308f, 0.07905f, 0.308f, 0.068f},
    {Op::Line, 0.308f, -0.168f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.308f, -0.17905f, 0.29905f, -0.188f, 0.288f, -0.188f},
    {Op::Line, -0.288f, -0.188f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, 0.f, -0.46f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.f, -0.46f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.02264f, -0.46f, 0.041f, -0.44164f, 0.041f, -0.419f},
    {Op::Line, 0.041f, 0.419f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.041f, 0.44164f, 0.02264f, 0.46f, 0.f, 0.46f},
    {Op::Line, 0.f, 0.46f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.02264f, 0.46f, -0.041f, 0.44164f, -0.041f, 0.419f},
    {Op::Line, -0.041f, -0.419f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.041f, -0.44164f, -0.02264f, -0.46f, 0.f, -0.46f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Cmd kA[] = {
    {Op::Move, 0.106f, -0.069f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.064f, 0.043f, 0.003f, 0.124f, -0.063f, 0.187f},
    {Op::Cubic, -0.074f, 0.129f, -0.081f, 0.068f, -0.081f, 0.004f},
    {Op::Line, -0.08f, -0.037f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.034f, -0.054f, 0.024f, -0.069f, 0.089f, -0.069f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, 0.22f, -0.179f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.141f, -0.199f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.14f, -0.182f, 0.135f, -0.156f, 0.13f, -0.141f},
    {Op::Line, 0.127f, -0.131f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.09f, -0.132f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.039f, -0.132f, -0.022f, -0.123f, -0.078f, -0.107f},
    {Op::Cubic, -0.075f, -0.149f, -0.072f, -0.191f, -0.068f, -0.23f},
    {Op::Cubic, 0.055f, -0.236f, 0.188f, -0.25f, 0.293f, -0.268f},
    {Op::Line, 0.292f, -0.342f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.19f, -0.318f, 0.068f, -0.305f, -0.059f, -0.299f},
    {Op::Line, -0.047f, -0.375f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.044f, -0.389f, -0.04f, -0.407f, -0.035f, -0.42f},
    {Op::Line, -0.119f, -0.422f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.118f, -0.41f, -0.12f, -0.392f, -0.121f, -0.374f},
    {Op::Line, -0.129f, -0.297f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.197f, -0.296f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.24f, -0.296f, -0.327f, -0.303f, -0.362f, -0.309f},
    {Op::Line, -0.36f, -0.234f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.319f, -0.231f, -0.241f, -0.227f, -0.198f, -0.227f},
    {Op::Line, -0.137f, -0.228f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.141f, -0.181f, -0.146f, -0.131f, -0.148f, -0.081f},
    {Op::Cubic, -0.286f, -0.017f, -0.398f, 0.114f, -0.398f, 0.243f},
    {Op::Cubic, -0.398f, 0.328f, -0.346f, 0.369f, -0.28f, 0.369f},
    {Op::Cubic, -0.225f, 0.369f, -0.165f, 0.347f, -0.11f, 0.314f},
    {Op::Line, -0.094f, 0.37f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.022f, 0.348f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.03f, 0.323f, -0.038f, 0.296f, -0.046f, 0.267f},
    {Op::Cubic, 0.039f, 0.195f, 0.12f, 0.084f, 0.177f, -0.058f},
    {Op::Cubic, 0.27f, -0.031f, 0.321f, 0.037f, 0.321f, 0.113f},
    {Op::Cubic, 0.321f, 0.243f, 0.209f, 0.336f, 0.028f, 0.355f},
    {Op::Line, 0.071f, 0.422f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.303f, 0.385f, 0.398f, 0.261f, 0.398f, 0.117f},
    {Op::Cubic, 0.398f, 0.007f, 0.324f, -0.085f, 0.199f, -0.118f},
    {Op::Line, 0.2f, -0.122f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.205f, -0.138f, 0.214f, -0.165f, 0.22f, -0.179f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.151f, -0.006f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.151f, 0.012f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.151f, 0.087f, -0.141f, 0.168f, -0.127f, 0.239f},
    {Op::Cubic, -0.178f, 0.275f, -0.226f, 0.292f, -0.265f, 0.292f},
    {Op::Cubic, -0.303f, 0.292f, -0.322f, 0.271f, -0.322f, 0.23f},
    {Op::Cubic, -0.322f, 0.148f, -0.248f, 0.049f, -0.151f, -0.006f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Cmd kGa[] = {
    {Op::Move, 0.19f, -0.452f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.19f, 0.452f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.273f, 0.452f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.273f, -0.016f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.417f, -0.016f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.417f, -0.085f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.273f, -0.085f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.273f, -0.452f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.375f, -0.355f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.375f, -0.286f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.043f, -0.286f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.062f, -0.072f, -0.187f, 0.101f, -0.417f, 0.217f},
    {Op::Line, -0.371f, 0.281f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.078f, 0.135f, 0.04f, -0.098f, 0.04f, -0.355f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Cmd kLatinUpper[] = {
    {Op::Move, -0.3f, 0.3665f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.207f, 0.3665f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.136f, 0.1425f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.132f, 0.1425f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.202f, 0.3665f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.3f, 0.3665f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.051f, -0.3665f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.052f, -0.3665f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.113f, 0.0695f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.077f, -0.0435f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.051f, -0.1265f, -0.027f, -0.2055f, -0.004f, -0.2915f},
    {Op::Line, 0.f, -0.2915f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.024f, -0.2065f, 0.047f, -0.1265f, 0.074f, -0.0435f},
    {Op::Line, 0.109f, 0.0695f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Cmd kLatinLower[] = {
    {Op::Move, -0.054f, 0.285f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.013f, 0.285f, 0.074f, 0.25f, 0.126f, 0.207f},
    {Op::Line, 0.129f, 0.207f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.137f, 0.272f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.212f, 0.272f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.212f, -0.062f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.212f, -0.197f, 0.157f, -0.285f, 0.024f, -0.285f},
    {Op::Cubic, -0.064f, -0.285f, -0.14f, -0.246f, -0.189f, -0.214f},
    {Op::Line, -0.154f, -0.151f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.111f, -0.18f, -0.054f, -0.209f, 0.009f, -0.209f},
    {Op::Cubic, 0.098f, -0.209f, 0.121f, -0.142f, 0.121f, -0.072f},
    {Op::Cubic, -0.11f, -0.046f, -0.212f, 0.013f, -0.212f, 0.131f},
    {Op::Cubic, -0.212f, 0.229f, -0.145f, 0.285f, -0.054f, 0.285f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.028f, 0.211f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.082f, 0.211f, -0.124f, 0.187f, -0.124f, 0.125f},
    {Op::Cubic, -0.124f, 0.055f, -0.062f, 0.01f, 0.121f, -0.011f},
    {Op::Line, 0.121f, 0.14f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.068f, 0.187f, 0.024f, 0.211f, -0.028f, 0.211f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Glyph kGlyphs[] = {
    {L'中', kZhong},
    {L'あ', kA},
    {L'가', kGa},
    {L'A', kLatinUpper},
    {L'a', kLatinLower},
};
// NOLINTEND(modernize-use-std-numbers)
// clang-format on

// The outline for `text` when it is one of these letters; nullptr otherwise.
inline const Glyph* Find(std::wstring_view text)
{
    if (text.size() != 1)
        return nullptr;
    for (const Glyph& g : kGlyphs)
        if (g.ch == text[0])
            return &g;
    return nullptr;
}

} // namespace atrium::ime_glyph
