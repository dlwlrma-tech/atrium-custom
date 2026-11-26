// Launchpad's own order, %LOCALAPPDATA%\Atrium\launchpad.json: the tiles' ids (apps' catalog ids and the
// system folder's) as the user dragged them. Until the first drag the apps keep the order they came in.
#pragma once

#include <string>
#include <vector>

namespace atrium {

std::vector<std::wstring> LoadLaunchpadOrder();
void SaveLaunchpadOrder(const std::vector<std::wstring>& ids);

} // namespace atrium
