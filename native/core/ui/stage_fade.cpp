#include "ui/stage_fade.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace atrium {

namespace {

constexpr float kMaxBlurDip = 7;                                    // at the bottom, in the card's DIP-ish px
constexpr std::array<float, 6> kLevels = {0, 0.1f, 0.22f, 0.4f, 0.65f, 1}; // the blurs made, of the deepest

float SmoothStep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

// Premultiplied channels as floats, w x h.
struct Plane {
    int w = 0, h = 0;
    std::vector<float> px; // 4 a pixel
    float* Row(int y) { return px.data() + size_t(y) * size_t(w) * 4; }
    const float* Row(int y) const { return px.data() + size_t(y) * size_t(w) * 4; }
};

// The source scaled to `w` px across (an area average: it only shrinks), `pad` transparent px around.
Plane Shrink(const IconImage& image, int w, int pad)
{
    const float k = float(image.width) / float(w);
    const int h = std::max(1, int(std::lround(float(image.height) / k)));
    Plane out{w + 2 * pad, h + 2 * pad, {}};
    out.px.assign(size_t(out.w) * size_t(out.h) * 4, 0.f);
    for (int y = 0; y < h; ++y) {
        const int y0 = std::min(image.height - 1, int(float(y) * k)), y1 = std::max(y0 + 1, std::min(image.height, int(float(y + 1) * k)));
        float* row = out.Row(y + pad) + size_t(pad) * 4;
        for (int x = 0; x < w; ++x) {
            const int x0 = std::min(image.width - 1, int(float(x) * k)), x1 = std::max(x0 + 1, std::min(image.width, int(float(x + 1) * k)));
            std::array<float, 4> sum{};
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* s = image.bgra.data() + (size_t(sy) * size_t(image.width) + size_t(x0)) * 4;
                for (int sx = x0; sx < x1; ++sx, s += 4)
                    for (size_t c = 0; c < 4; ++c)
                        sum[c] += float(s[c]);
            }
            const float n = float((y1 - y0) * (x1 - x0));
            for (size_t c = 0; c < 4; ++c)
                row[size_t(x) * 4 + c] = sum[c] / n;
        }
    }
    return out;
}

// One box pass of half width r along a line of n pixels `step` floats apart (outside is transparent).
void BoxLine(float* line, int n, size_t step, int r, std::vector<float>& tmp)
{
    tmp.assign(size_t(n) * 4, 0.f);
    std::array<float, 4> sum{};
    const float inv = 1.f / float(2 * r + 1);
    for (int i = 0; i <= r && i < n; ++i)
        for (size_t c = 0; c < 4; ++c)
            sum[c] += line[size_t(i) * step + c];
    for (int i = 0; i < n; ++i) {
        for (size_t c = 0; c < 4; ++c)
            tmp[size_t(i) * 4 + c] = sum[c] * inv;
        const int add = i + r + 1, drop = i - r;
        for (size_t c = 0; c < 4; ++c) {
            if (add < n)
                sum[c] += line[size_t(add) * step + c];
            if (drop >= 0)
                sum[c] -= line[size_t(drop) * step + c];
        }
    }
    for (int i = 0; i < n; ++i)
        for (size_t c = 0; c < 4; ++c)
            line[size_t(i) * step + c] = tmp[size_t(i) * 4 + c];
}

// Near a Gaussian of deviation `sigma`: three box passes each way.
Plane Blurred(const Plane& p, float sigma)
{
    Plane out = p;
    if (sigma < 0.3f)
        return out;
    const int r = std::max(1, int(std::lround((std::sqrt(4 * sigma * sigma + 1) - 1) / 2)));
    std::vector<float> tmp;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < out.h; ++y)
            BoxLine(out.Row(y), out.w, 4, r, tmp);
        for (int x = 0; x < out.w; ++x)
            BoxLine(out.px.data() + size_t(x) * 4, out.h, size_t(out.w) * 4, r, tmp);
    }
    return out;
}

} // namespace

float StageFadeOpacity(float v)
{
    const float t = SmoothStep(0.42f, 1.f, v);
    return 1 - t * t * (2 - t); // slow to start, gone softly
}

float StageFadeBlur(float v) { return std::pow(SmoothStep(0.12f, 0.96f, v), 1.35f); }

StageFade StageFadeFor(int sourceWidth, float cardPx)
{
    StageFade f;
    f.width = std::clamp(int(std::lround(cardPx * 1.12f)), 16, std::max(16, sourceWidth));
    const float k = float(sourceWidth) / float(f.width); // source px a faded px
    f.maxBlur = kMaxBlurDip * cardPx / 172.f * k;
    f.margin = std::ceil(f.maxBlur / k * 2.6f + 2) * k;
    return f;
}

IconImage FadeAway(const IconImage& image, float top, float bottom, const StageFade& fade, bool up)
{
    if (image.Empty() || fade.width <= 0 || bottom <= top)
        return {};
    const float k = float(image.width) / float(fade.width);
    const int pad = int(std::lround(fade.margin / k));
    const Plane base = Shrink(image, fade.width, pad);
    std::vector<Plane> levels;
    levels.reserve(kLevels.size());
    for (float l : kLevels)
        levels.push_back(Blurred(base, l * fade.maxBlur / k));
    IconImage out{base.w, base.h, std::vector<uint8_t>(size_t(base.w) * size_t(base.h) * 4)};
    for (int y = 0; y < base.h; ++y) {
        const float down = ((float(y - pad) + 0.5f) * k - top) / (bottom - top), v = up ? 1 - down : down;
        const float want = StageFadeBlur(v), o = StageFadeOpacity(v);
        // Between the two blurs made nearest this row's.
        size_t i = 0;
        while (i + 2 < kLevels.size() && kLevels[i + 1] < want)
            ++i;
        const float t = std::clamp((want - kLevels[i]) / (kLevels[i + 1] - kLevels[i]), 0.f, 1.f);
        const float* a = levels[i].Row(y);
        const float* c = levels[i + 1].Row(y);
        uint8_t* d = out.bgra.data() + size_t(y) * size_t(base.w) * 4;
        for (size_t n = 0; n < size_t(base.w) * 4; ++n)
            d[n] = uint8_t(std::clamp(std::lround((a[n] + (c[n] - a[n]) * t) * o), 0L, 255L));
    }
    return out;
}

} // namespace atrium
