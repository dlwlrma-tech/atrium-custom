#include "bar_tray.h"

#include "bar_entries.h"
#include "log.h"
#include "settings_server.h"
#include "shell/tray_model.h"
#include "shell/tray_pins.h"
#include "ui/tray_menu_follow.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace atrium {

namespace {

std::optional<TrayIcon> FindPinnedTrayIcon(const TrayModel& model, const std::wstring& pinKey)
{
    const std::wstring folded = FoldCase(pinKey);
    for (const TrayIcon& icon : model.Icons()) {
        if (TrayPinKey(icon) == folded)
            return icon;
    }
    return std::nullopt;
}

} // namespace

std::vector<BarTrayIcon> BuildBarTrayIcons(const TrayModel& model, const std::vector<std::wstring>& pinned)
{
    std::vector<BarTrayIcon> out;
    if (pinned.empty())
        return out;
    const std::vector<TrayIcon> icons = model.Icons();
    out.reserve(std::min(pinned.size(), icons.size()));
    for (const std::wstring& entry : pinned) {
        const std::wstring folded = FoldCase(entry);
        const auto it = std::ranges::find_if(icons, [&](const TrayIcon& icon) { return TrayPinKey(icon) == folded; });
        if (it == icons.end())
            continue;
        BarTrayIcon item;
        item.pinKey = folded;
        item.key = std::wstring(kTrayItemPrefix) + folded;
        item.tip = it->tip;
        item.image = it->image;
        item.inked = it->inked;
        item.inkedOnLight = it->inkedOnLight;
        item.imageVersion = it->imageVersion;
        item.flashing = it->flashing;
        out.push_back(std::move(item));
    }
    return out;
}

void TogglePinnedTrayIcon(SettingsServer* server, const std::wstring& pinKey)
{
    if (!server || pinKey.empty())
        return;
    bool nowPinned = false;
    server->Modify([&](Settings& s) { nowPinned = ToggleTrayPin(s.trayPinned, pinKey); });
    Log(L"tray: %s %s the taskbar", pinKey.c_str(), nowPinned ? L"pinned to" : L"unpinned from");
}

void ConfigureBarTray(Bar& bar, TrayModel& model, std::function<SettingsServer*()> getServer)
{
    bar.onTrayIconClick = [&bar, &model](const std::wstring& pinKey, RECT cell) {
        if (const auto icon = FindPinnedTrayIcon(model, pinKey)) {
            const RECT barRect = bar.BarRect();
            const POINT anchor{(cell.left + cell.right) / 2, barRect.top - 6};
            model.SetHostPlaces(RECT{cell.left, barRect.top - 24, cell.right, barRect.top - 4});
            FollowTrayMenu(anchor, icon->exe, barRect, [&bar] { return bar.BarRect(); });
            model.Click(*icon, false, cell);
        }
    };
    bar.onTrayIconDoubleClick = [&bar, &model](const std::wstring& pinKey, RECT cell) {
        if (const auto icon = FindPinnedTrayIcon(model, pinKey)) {
            const RECT barRect = bar.BarRect();
            model.SetHostPlaces(RECT{cell.left, barRect.top - 24, cell.right, barRect.top - 4});
            model.DoubleClick(*icon, true, cell);
        }
    };
    bar.onTrayIconRightClick = [&bar, &model](const std::wstring& pinKey, RECT cell, POINT anchor) {
        if (const auto icon = FindPinnedTrayIcon(model, pinKey)) {
            const RECT barRect = bar.BarRect();
            model.SetHostPlaces(RECT{cell.left, barRect.top - 24, cell.right, barRect.top - 4});
            FollowTrayMenu(anchor, icon->exe, barRect, [&bar] { return bar.BarRect(); });
            model.Click(*icon, true, cell);
        }
    };
    bar.onTrayIconMiddleClick = [&model](const std::wstring& pinKey, RECT cell) {
        if (const auto icon = FindPinnedTrayIcon(model, pinKey); icon && !icon->hostKey.empty())
            model.MiddleClick(*icon, cell);
    };
    bar.onTrayIconUnpin = [getServer = std::move(getServer)](const std::wstring& pinKey) {
        SettingsServer* server = getServer ? getServer() : nullptr;
        if (!server || pinKey.empty())
            return;
        server->Modify([&](Settings& s) {
            const std::wstring folded = FoldCase(pinKey);
            std::erase_if(s.trayPinned, [&](const std::wstring& entry) { return FoldCase(entry) == folded; });
        });
        Log(L"tray: %s unpinned from the taskbar", pinKey.c_str());
    };
}

} // namespace atrium
