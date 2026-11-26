// Where the desktop widgets go in the z-order: below every app window on their monitor, above the desktop.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace atrium {

struct StackWindow {
    HWND hwnd = nullptr;
    std::wstring cls;
    RECT rect{};
    bool visible = false, cloaked = false, iconic = false, topmost = false, toolwindow = false;
    bool owned = false, appWindow = false, clickThrough = false, hasCaption = false, hasTitle = false;
    bool hasDefView = false, ours = false, widget = false;
};

struct WidgetStackPlan {
    HWND after = HWND_BOTTOM, lowestApp = nullptr, desktopHost = nullptr;
    std::wstring desktopClass, lowestClass;
    bool showDesktop = false;
};

WidgetStackPlan ComputeWidgetStackPlan(const std::vector<StackWindow>& windows, const RECT& monitorRect, HWND fgRoot, bool fgIsOurs, bool wasShowDesktop);
std::wstring WindowClassOf(HWND h);
std::vector<StackWindow> CollectStackSnapshot(const std::vector<HWND>& widgetHwnds);
void ApplyWidgetZOrder(const std::vector<HWND>& widgetHwnds, const WidgetStackPlan& plan, DWORD& swpErr);

} // namespace atrium
