// Quick system switches for the control centre: Energy saver, Dark mode and
// Mobile hotspot (Night light, no longer one of its modules, is kept readable). Every call may block (registry, powrprof,
// SendMessageTimeout, WinRT async): run them on a worker thread (MTA), never on
// the UI thread.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace atrium {

// Pure CloudStore bluelightreductionstate record helpers (exposed for tests).
std::optional<bool> IsNightLightOn(const std::vector<uint8_t>& data);
std::vector<uint8_t> ToggleNightLightData(const std::vector<uint8_t>& data, bool on, uint64_t timestampSec);

std::optional<bool> ReadNightLight();
bool SetNightLight(bool on);

std::optional<bool> ReadEnergySaver();
bool SetEnergySaver(bool on);

std::optional<bool> ReadDarkMode();
bool SetDarkMode(bool on);

std::optional<bool> ReadMobileHotspot();
bool SetMobileHotspot(bool on);

} // namespace atrium
