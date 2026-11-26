// Stage Manager's strip geometry, hit-testing, and group merge rules.
#pragma once

#include "stage_excluded.h"

#include <windows.h>
#include <d2d1.h>

#include <algorithm>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace atrium {

constexpr float kStageEdge = 22, kStageBoxW = 172, kStageBoxH = 112, kStageSlotGap = 36, kStageStripW = kStageEdge + kStageBoxW + 44;

inline D2D1_RECT_F StageBoxRect(float stripWidth, float y, bool onRight)
{
    const float x = onRight ? stripWidth - kStageEdge - kStageBoxW : kStageEdge;
    return D2D1::RectF(x, y, x + kStageBoxW, y + kStageBoxH);
}

// Which card in the strip is under (x, y) in strip content DIP; -1 when outside every card.
inline int StageEntryAt(std::span<const D2D1_RECT_F> boxes, float x, float y)
{
    for (size_t i = 0; i < boxes.size(); ++i) {
        const D2D1_RECT_F& r = boxes[i];
        if (x >= r.left - 14 && x < r.right + 10 && y >= r.top - 10 && y < r.bottom + 24)
            return int(i);
    }
    return -1;
}

// Always shown, the strip stays out: a window over its place moves beside it (narrower if it must).
inline bool StageBesideRect(RECT& frame, const RECT& zone, const RECT& wa, bool onRight)
{
    if (frame.bottom <= zone.top || frame.top >= zone.bottom)
        return false;
    if (onRight) {
        if (frame.right <= zone.left)
            return false;
        OffsetRect(&frame, zone.left - frame.right, 0);
        frame.left = std::max(frame.left, wa.left);
    } else {
        if (frame.left >= zone.right)
            return false;
        OffsetRect(&frame, zone.right - frame.left, 0);
        frame.right = std::min(frame.right, wa.right);
    }
    return true;
}

// Merges `windowKey` into the strip group matching `target`: `windowKey` leaves its prior group
// (which disappears if left empty), any remaining active group moves to the front of `recent`, and
// the merged target group becomes `stage`. Excluded windows never merge.
inline bool StageMergeGroups(std::vector<std::wstring>& stage, std::vector<std::vector<std::wstring>>& recent, const std::vector<std::wstring>& excluded,
                             const std::wstring& windowKey, const std::vector<std::wstring>& target)
{
    if (windowKey.empty() || target.empty() || StageAppExcluded(excluded, windowKey))
        return false;
    auto has = [](const std::vector<std::wstring>& set, const std::wstring& key) { return std::ranges::find(set, key) != set.end(); };
    auto hit = std::ranges::find_if(recent, [&](const auto& set) { return std::ranges::any_of(set, [&](const std::wstring& k) { return has(target, k); }); });
    if (hit == recent.end())
        return false;
    std::vector<std::wstring> merged = *hit;
    std::erase(merged, windowKey);
    if (merged.empty())
        return false;
    merged.push_back(windowKey);
    std::erase(stage, windowKey);
    for (auto& set : recent)
        std::erase_if(set, [&](const std::wstring& k) { return k == windowKey || has(merged, k); });
    if (!stage.empty())
        recent.insert(recent.begin(), std::move(stage));
    std::erase_if(recent, [](const auto& set) { return set.empty(); });
    stage = std::move(merged);
    return true;
}

} // namespace atrium
