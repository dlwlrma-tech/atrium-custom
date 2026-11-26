// Windows Win+X quick link menu items and their launch actions for the start logo right-click.
#pragma once

#include <string>
#include <vector>

namespace atrium {

struct QuickLink {
    int id = 0; // 0 for separators and headers
    std::wstring text;
    bool header = false;
};

std::vector<QuickLink> QuickLinks();
void RunQuickLink(int id);

} // namespace atrium
