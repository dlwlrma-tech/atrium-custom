// Wires pinned tray icons between TrayModel, SettingsServer, and the taskbar.
#pragma once

#include "bar.h"

#include <functional>
#include <string>
#include <vector>

namespace atrium {

class SettingsServer;
class TrayModel;

std::vector<BarTrayIcon> BuildBarTrayIcons(const TrayModel& model, const std::vector<std::wstring>& pinned);
void ConfigureBarTray(Bar& bar, TrayModel& model, std::function<SettingsServer*()> getServer);
void TogglePinnedTrayIcon(SettingsServer* server, const std::wstring& pinKey);

} // namespace atrium
