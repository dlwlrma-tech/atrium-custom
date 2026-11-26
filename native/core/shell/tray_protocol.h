// The notification-area callback protocol, as the shell speaks it to an app
// (NOTIFYICONDATA uCallbackMessage, NOTIFYICON_VERSION 0..4). Pure: tested on its
// own (native/tests/tray_protocol.cpp); the tray host sends what this returns.
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace atrium::tray {

// Open: a left click without the pointer's arrival, for opening an app from the bar (the pointer is
// not on its icon: a mouse move would open a messenger's hover message box there).
enum class HostAction { Left, Right, Middle, Double, DoubleContinuation, HoverIn, PopupOpen, PopupClose, Open };

// The notifications, in order, the shell sends for `action` to an icon of `version`.
// HoverIn is the pointer arriving; PopupOpen/PopupClose are the rich-tooltip
// requests a version 4 app without NIF_SHOWTIP gets after a dwell and on leaving.
inline std::vector<UINT> CallbackSequence(HostAction action, UINT version)
{
    const bool v3 = version >= 3, v4 = version >= 4;
    switch (action) {
    case HostAction::HoverIn: return {WM_MOUSEMOVE};
    case HostAction::PopupOpen:
    case HostAction::PopupClose:
        if (!v4)
            return {};
        return {action == HostAction::PopupOpen ? UINT(NIN_POPUPOPEN) : UINT(NIN_POPUPCLOSE)};
    case HostAction::Right:
        if (v3)
            return {WM_MOUSEMOVE, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_CONTEXTMENU};
        return {WM_MOUSEMOVE, WM_RBUTTONDOWN, WM_RBUTTONUP};
    case HostAction::Middle: return {WM_MOUSEMOVE, WM_MBUTTONDOWN, WM_MBUTTONUP};
    case HostAction::Open:
        if (v3)
            return {WM_LBUTTONDOWN, WM_LBUTTONUP, NIN_SELECT};
        return {WM_LBUTTONDOWN, WM_LBUTTONUP};
    case HostAction::DoubleContinuation: return {WM_LBUTTONDBLCLK, WM_LBUTTONUP};
    case HostAction::Double:
        if (v3)
            return {WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, NIN_SELECT, WM_LBUTTONDBLCLK, WM_LBUTTONUP};
        return {WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP};
    case HostAction::Left:
    default:
        if (v3)
            return {WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, NIN_SELECT};
        return {WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP};
    }
}

// wParam and lParam of one callback: version 4 puts the pointer (screen
// coordinates) in wParam and the icon's id in lParam's high word; older
// versions pass the id as wParam and the notification as lParam.
inline void EncodeCallback(UINT version, UINT uid, UINT notification, POINT at, WPARAM& w, LPARAM& l)
{
    if (version >= 4) {
        w = MAKEWPARAM(WORD(SHORT(at.x)), WORD(SHORT(at.y)));
        l = MAKELPARAM(WORD(notification), WORD(uid));
    } else {
        w = WPARAM(uid);
        l = LPARAM(notification);
    }
}

// Outcome of a forwarded Shell_NotifyIcon call: Explorer's answer wins when non-zero;
// when Explorer is absent or refuses, the host still accepts a valid live window's NIM_ADD
// or an already-known icon's NIM_MODIFY/NIM_SETVERSION/NIM_DELETE (unknown NIM_MODIFY stays 0 for probing).
inline LRESULT DecideHostForwardResult(DWORD message, bool shellPresent, LRESULT forwardResult, bool knownIcon, bool validWindow)
{
    if (forwardResult)
        return forwardResult;
    if (message == NIM_DELETE && knownIcon)
        return TRUE;
    if (!shellPresent)
        return message != NIM_DELETE ? TRUE : 0;
    if (message == NIM_ADD && validWindow)
        return TRUE;
    if ((message == NIM_MODIFY || message == NIM_SETVERSION) && knownIcon)
        return TRUE;
    return 0;
}

struct IconFlashState {
    static constexpr ULONGLONG kWindowMs = 4500;
    static constexpr ULONGLONG kHoldMs = 4000;
    static constexpr size_t kMinChanges = 3;

    struct Change {
        ULONGLONG at = 0;
        uint64_t hash = 0;
    };
    uint64_t lastHash = 0;
    bool hasPicture = false;
    std::vector<Change> changes;
    ULONGLONG until = 0;
};

// Records an icon picture update at `now` and returns whether it is flashing.
// Only real picture changes count; >= 3 changes within 4500 ms across at least
// two distinct pictures trigger flashing, held for 4000 ms after the last change.
inline bool RecordIconPicture(IconFlashState& state, ULONGLONG now, uint64_t hash)
{
    if (state.hasPicture && hash == state.lastHash)
        return state.until > now;
    state.hasPicture = true;
    state.lastHash = hash;
    std::erase_if(state.changes, [&](const IconFlashState::Change& c) { return now - c.at > IconFlashState::kWindowMs; });
    state.changes.push_back({now, hash});
    bool twoPictures = false;
    for (const auto& c : state.changes) {
        if (c.hash != hash) {
            twoPictures = true;
            break;
        }
    }
    if (state.changes.size() >= IconFlashState::kMinChanges && twoPictures)
        state.until = now + IconFlashState::kHoldMs;
    return state.until > now;
}

// Which rectangle to return for Shell_NotifyIconGetRect: empty when unknown (forward to Explorer),
// else the clicked cell, else the hovered cell, else the bar's tray anchor.
inline RECT ResolveHostIconRect(std::wstring_view matchedKey, std::wstring_view clickedKey, const RECT& clicked, std::wstring_view hoverKey,
                                const RECT& hovered, const RECT& anchor)
{
    if (matchedKey.empty())
        return {};
    if (matchedKey == clickedKey && !IsRectEmpty(&clicked))
        return clicked;
    if (matchedKey == hoverKey && !IsRectEmpty(&hovered))
        return hovered;
    return anchor;
}

inline RECT CellOrBox(RECT cell, POINT at) { return !IsRectEmpty(&cell) ? cell : RECT{at.x - 16, at.y - 16, at.x + 16, at.y + 16}; }

} // namespace atrium::tray
