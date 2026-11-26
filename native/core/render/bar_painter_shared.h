// Shared layout-frame and ink helpers used across BarPainter translation units.
#pragma once

#include "bar_painter.h"

namespace atrium {

inline D2D1_COLOR_F BarInkColor(float dark) { return D2D1::ColorF(1.f - 0.93f * dark, 1.f - 0.93f * dark, 1.f - 0.91f * dark, 0.96f - 0.04f * dark); }

inline EdgeFrame BarFrameFor(const BarScene& scene)
{
    return {scene.edge, Vertical(scene.edge) ? D2D1::RectF(0, 0, scene.windowH, scene.windowW) : D2D1::RectF(0, 0, scene.windowW, scene.windowH)};
}

inline D2D1_POINT_2F BarVectorToArea(const EdgeFrame& frame, float du, float dv)
{
    const auto m = frame.FromAreaMatrix();
    return {du * m._11 + dv * m._12, du * m._21 + dv * m._22};
}

} // namespace atrium
