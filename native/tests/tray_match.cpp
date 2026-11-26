#include "shell/tray_match.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
using namespace atrium::tray;
void Check(bool ok, const char* name)
{
    if (!ok)
        throw std::runtime_error(name);
}
Record R(const std::wstring& name, size_t order, bool promoted = false)
{
    Record r;
    r.id = std::to_wstring(order);
    r.exe = name + L".exe";
    r.tip = name;
    r.identity = L"guid:" + name;
    r.live = true;
    r.promoted = promoted;
    r.order = order;
    r.names = {name};
    r.bounds = {900, 1000, 920, 1020};
    return r;
}
void TestMojibakeAndKnownShell()
{
    Check(IsShellExplorerExe(L"C:\\Windows\\explorer.exe") && IsShellExplorerExe(L"EXPLORER.EXE"), "explorer exe detection");
    Check(!IsShellExplorerExe(L"C:\\Tools\\myexplorer.exe") && !IsShellExplorerExe(L""), "non-explorer exe rejected");
    const auto bt = KnownShellTray(L"{6DA68F06-00F1-4E6E-8158-7F1FFD4F9DB9}");
    Check(bt && std::wstring(bt->iconDll) == L"bthprops.cpl" && bt->aliases.size() == 3, "known bluetooth shell tray");
    const auto usb = KnownShellTray(L"guid:{7820ae78-23e3-4229-82c1-e41cb67d5b9c}");
    Check(usb && std::wstring(usb->iconDll) == L"hotplug.dll" && usb->aliases.size() == 2, "known safely remove shell tray");
    Check(!KnownShellTray(L"{00000000-0000-0000-0000-000000000000}"), "unknown guid returns nullopt");

    for (const std::wstring& clean : {std::wstring(L"蓝牙设备"), std::wstring(L"安全删除硬件并弹出媒体"), std::wstring(L"模式"),
                                      std::wstring(L"Steam - syncing"), std::wstring(L"100% ↑↓ •"), std::wstring()})
        Check(RepairMojibakeTip(clean) == clean, "clean UTF-16 and ASCII tooltips stay untouched");

    const UINT acp = GetACP();
    if (acp != CP_UTF8) {
        const std::wstring expected = L"VPN - 中文测试";
        const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, expected.data(), int(expected.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size_t(utf8Len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, expected.data(), int(expected.size()), utf8.data(), utf8Len, nullptr, nullptr);
        const int wLen = MultiByteToWideChar(acp, MB_ERR_INVALID_CHARS, utf8.data(), utf8Len, nullptr, 0);
        if (wLen > 0) {
            std::wstring mojibake(size_t(wLen), L'\0');
            if (MultiByteToWideChar(acp, MB_ERR_INVALID_CHARS, utf8.data(), utf8Len, mojibake.data(), wLen) == wLen) {
                const int rtLen = WideCharToMultiByte(acp, WC_NO_BEST_FIT_CHARS, mojibake.data(), wLen, nullptr, 0, nullptr, nullptr);
                std::string rt(size_t(rtLen), '\0');
                BOOL usedDef = FALSE;
                if (rtLen == utf8Len && WideCharToMultiByte(acp, WC_NO_BEST_FIT_CHARS, mojibake.data(), wLen, rt.data(), rtLen, nullptr, &usedDef) == rtLen &&
                    !usedDef && rt == utf8)
                    Check(RepairMojibakeTip(mojibake) == expected, "UTF-8-as-ACP mojibake Chinese+English round-trip repaired");
            }
        }
    }
}
void TestOrderAndDedup()
{
    auto rWork = R(L"WorkBuddy", 0);
    auto result = Match({{L"Tencent WorkBuddy (Online)", false}}, {rWork});
    Check(result.items.size() == 1 && result.items[0].record == 0 && result.items[0].method == Method::Name, "delimited contains score 60 match");

    auto a = R(L"Alpha", 0), b = R(L"Beta", 1), c = R(L"Charlie", 2);
    result = Match({{L"Alpha", false}, {L"Custom Tip", false}, {L"Charlie", false}}, {a, b, c});
    Check(result.items.size() == 3 && result.items[1].record == 1 && result.items[1].method == Method::Order, "remainder Order after partial Name anchors");

    auto top = R(L"TopApp", 0, true);
    result = Match({{L"TopApp", false}, {L"X1", false}, {L"X2", false}}, {top, a, b});
    Check(result.items.size() == 3 && result.items[1].record == 1 && result.items[2].record == 2 && result.items[1].method == Method::Order,
          "remainder Order ignores cross-partition anchor");

    auto wb = R(L"WorkBuddy", 0);
    wb.order = kNone;
    result = Match({{L"", false}}, {wb});
    Check(result.items.size() == 1 && result.items[0].button == 0 && result.items[0].record == 0,
          "single empty button attaches single unmatched record instead of gray disc");

    std::vector<HostedEntry> hosted{
        {L"hwnd:1:1", L"c:\\vpn\\vpn.exe", L"VPN - Connected", L"VPN Client", L"VPN App", 1, true, 10},
        {L"hwnd:2:2", L"", L"NVIDIA Settings", L"", L"", 2, true},
        {L"hwnd:3:3", L"c:\\app\\workbuddy.exe", L"", L"WorkBuddy", L"Tencent WorkBuddy", 3, true},
    };
    std::vector<ExplorerEntry> explorer{
        {L"uid:10:1:1", L"c:\\vpn\\vpn.exe", L"VPN - Connected", true},
        {L"uid:10:9:9", L"c:\\vpn\\vpn.exe", L"VPN - Idle", true},
        {L"guid:{11111111-1111-1111-1111-111111111111}", L"c:\\vpn\\vpn.exe", L"VPN Helper", true},
        {L"uid:20:2:2", L"c:\\nvidia\\nv.exe", L"NVIDIA Settings - GPU", true},
        {L"", L"", L"WorkBuddy - Online", false},
        {L"", L"", L"Tencent WorkBuddy - Status", false},
        {L"", L"", L"", false},
        {L"guid:{22222222-2222-2222-2222-222222222222}", L"c:\\other\\other.exe", L"Other", true},
    };
    const auto merged = MergeHostedIcons(hosted, explorer);
    Check(merged.hostedFallback[0] == 0, "hosted VPN paired by uid");
    Check(merged.hostedFallback[1] == 3, "elevated empty-exe hosted paired by delimited-prefix tip");
    Check(merged.hostedFallback[2] == 4, "empty-exe explorer paired against hosted fallbackTip");
    Check(merged.extraExplorer.size() == 2 && merged.extraExplorer[0] == 2 && merged.extraExplorer[1] == 7,
          "suppresses duplicate VPN uid, ghost empty tip, productName gray disc, and keeps distinct unpaired GUID and unrelated app");
}
void TestAttachRemainingAndMultiInstance()
{
    auto r1 = R(L"DupApp", kNone);
    r1.id = L"r1";
    r1.identity = L"uid:10:1:0";
    r1.bounds = {10, 10, 30, 30};
    auto r2 = R(L"DupApp", kNone);
    r2.id = L"r2";
    r2.identity = L"uid:20:2:0";
    r2.bounds = {40, 40, 60, 60};

    auto res = Match({{L"DupApp", false}}, {r1, r2});
    Check(res.items.size() == 3 && res.items[0].button == 0 && res.items[0].record == kNone && res.items[1].method == Method::Record &&
              res.items[2].method == Method::Record,
          "AttachRemaining refuses to match when two unused live records tie on one button");

    res = Match({{L"Geom", false, r1.bounds}, {L"DupApp", false}, {L"DupApp", true}}, {r1, r2});
    Check(res.items.size() == 3 && res.items[0].record == 0 && res.items[0].method == Method::Geometry && res.items[1].record == 1 &&
              res.items[1].method == Method::Name && res.items[2].record == kNone,
          "AttachRemaining matches remaining record once tying peer is already used");

    auto rTop = r1;
    rTop.promoted = true;
    res = Match({{L"DupApp", true}, {L"Other", false}}, {rTop, r2});
    Check(res.items[0].record == 0 && res.items[0].method == Method::Name, "AttachRemaining matches unique record in same promoted partition");

    const std::wstring multiExe = L"c:\\app\\multi.exe";
    std::vector<HostedEntry> hostedHwnd{
        {L"hwnd:0000000000001000:0", multiExe, L"", L"", L"", 0, true, 100},
        {L"hwnd:0000000000002000:0", multiExe, L"", L"", L"", 0, true, 200},
    };
    std::vector<ExplorerEntry> explorerHwnd{
        {L"uid:200:8192:0", multiExe, L"Instance B", true},
        {L"uid:100:4096:0", multiExe, L"Instance A", true},
    };
    const auto mergedHwnd = MergeHostedIcons(hostedHwnd, explorerHwnd);
    Check(mergedHwnd.hostedFallback[0] == 1 && mergedHwnd.hostedFallback[1] == 0 && mergedHwnd.extraExplorer.empty(),
          "MergeHostedIcons pairs same-exe uID=0 instances by exact (hwnd, uid) in Tier 0");

    std::vector<HostedEntry> hostedTip{
        {L"hwnd:100:0", multiExe, L"Alpha Profile", L"", L"", 0, true, 100},
        {L"hwnd:200:0", multiExe, L"Beta Profile", L"", L"", 0, true, 200},
    };
    std::vector<ExplorerEntry> explorerTip{
        {L"uid:200:999:0", multiExe, L"Beta Profile", true},
        {L"uid:100:888:0", multiExe, L"Alpha Profile", true},
    };
    const auto mergedTip = MergeHostedIcons(hostedTip, explorerTip);
    Check(mergedTip.hostedFallback[0] == 1 && mergedTip.hostedFallback[1] == 0 && mergedTip.extraExplorer.empty(),
          "MergeHostedIcons defers colliding bare uid to Tier 2 tooltip match");

    std::vector<HostedEntry> hostedPartial{
        {L"hwnd:1000:0", multiExe, L"Other Tip", L"", L"", 0, true, 100},
        {L"hwnd:200:0", multiExe, L"Remaining", L"", L"", 0, true, 200},
    };
    std::vector<ExplorerEntry> explorerPartial{
        {L"uid:200:999:0", multiExe, L"Different Tip", true},
        {L"uid:100:4096:0", multiExe, L"Alpha", true},
    };
    const auto mergedPartial = MergeHostedIcons(hostedPartial, explorerPartial);
    Check(mergedPartial.hostedFallback[0] == 1 && mergedPartial.hostedFallback[1] == 0,
          "MergeHostedIcons allows Tier 1 bare uid once prior tiers leave a unique unpaired uid");

    const std::wstring clientExe = L"c:\\app\\client.exe";
    std::vector<HostedEntry> hostedPid{
        {L"hwnd:1000:1", clientExe, L"Client A", L"", L"", 1, true, 100},
    };
    std::vector<ExplorerEntry> explorerPid{
        {L"uid:100:4096:1", clientExe, L"Client A", true},
        {L"uid:100:4096:2", clientExe, L"Client A Extra", true},
        {L"uid:200:8192:1", clientExe, L"Client B", true},
        {L"", clientExe, L"Legacy Ghost", true},
    };
    const auto mergedPid = MergeHostedIcons(hostedPid, explorerPid);
    Check(mergedPid.hostedFallback[0] == 0 && mergedPid.extraExplorer.size() == 1 && mergedPid.extraExplorer[0] == 2,
          "MergeHostedIcons preserves distinct-PID uid icon of same exe while suppressing same-PID and pid=0 duplicates");
}
int main()
{
    try {
        auto steam = R(L"Steam", 0, true), qq = R(L"QQ", 1), tailscale = R(L"Tailscale", 2);
        steam.bounds = {20, 20, 40, 40};
        std::vector<Record> rs{steam, qq, tailscale};
        auto result = Match({{L"Steam", true, steam.bounds}}, rs);
        Check(result.items.size() == 3 && result.items[1].button == kNone, "unopened overflow supplements live records");
        Check(result.sharedPlace[1] && result.sharedPlace[2], "different exe sharedPlace regression");
        result = Match({{L"Steam", true, steam.bounds}, {L"QQ", false}, {L"Tailscale", false}}, rs);
        Check(result.items.size() == 3 && result.items[2].record == 2, "opened overflow has no duplicates");
        result = Match({{L"QQ", false}}, rs);
        Check(result.items.size() == 3, "partial overflow still supplements missing identities");
        auto nvidia = R(L"NVIDIA", 0), second = nvidia;
        second.id = L"1";
        second.identity = L"uid:42:100:2";
        second.order = 1;
        result = Match({}, {nvidia, second});
        Check(result.items.size() == 2 && result.sharedPlace[0], "same exe GUID and UID survive shared rectangle and identical names");
        result = Match({{L"NVIDIA", false}, {L"NVIDIA", false}}, {nvidia, second});
        Check(result.items.size() == 2 && result.items[0].method == Method::Order, "same exe distinct icons use consistent full order");
        auto old = qq;
        old.id = L"old";
        old.order = 100;
        result = Match({}, {old, qq});
        Check(result.items.size() == 1 && result.items[0].record == 1 && result.aliases[0] == 1, "re-registration aliases deduplicate by live identity");
        old.identity = L"guid:obsolete";
        old.live = false;
        result = Match({}, {old, qq});
        Check(result.items.size() == 1, "dead registration never resurrects");
        qq.live = false;
        result = Match({{L"QQ", false}}, {qq});
        Check(result.items.size() == 1 && result.items[0].record == kNone, "stale tooltip cannot claim live UIA button");
        result = Match({{L"Steam - 正在同步", false}}, {}, {{L"steam.exe", {L"Steam"}}});
        Check(result.items.size() == 1 && result.items[0].method == Method::Running, "Windows 10 running program fallback and changing tooltip");
        result = Match({{L"", false}, {L"", false}}, {nvidia, second});
        Check(result.items.size() == 2 && result.items[1].record == 1, "empty tooltip complete ordering");
        result = Match({{L"QQ", false}}, {}, {{L"a.exe", {L"QQ"}}, {L"b.exe", {L"QQ"}}});
        Check(result.items[0].program == kNone, "ambiguous program name is not ownership");
        result = Match({{L"Steam", false}}, {}, {{L"browser.exe", {L"Steam - website"}, 0}});
        Check(result.items[0].program == kNone, "window title prefix cannot claim another app");
        for (const auto& tip : {L"SteamVR", L"SteamHelper", L"QQMusic"}) {
            result = Match({{tip, false}}, {}, {{L"steam.exe", {L"Steam"}}, {L"qq.exe", {L"QQ"}}});
            Check(result.items[0].program == kNone, "program prefix needs a real status delimiter");
        }
        result = Match({{L"STEAM：正在同步", false}}, {}, {{L"steam.exe", {L"Steam"}}});
        Check(result.items[0].program == 0, "localized status delimiter and case remain supported");
        auto unnamedOrder = R(L"Steam", kNone);
        result = Match({{L"SteamVR", false}}, {unnamedOrder});
        Check(result.items[0].record == kNone, "registration name prefix also needs delimiter");
        auto a = R(L"Alpha", 0), b = R(L"Beta", 1);
        result = Match({{L"Beta", false}, {L"Alpha", false}}, {a, b});
        Check(result.items.size() == 2 && result.items[0].record == 1, "names override obsolete order");
        result = Match({{L"Alpha", false}, {L"当前线路：香港", false}}, {a, b});
        Check(result.items.size() == 2 && result.items[1].method == Method::Order, "fully changed tooltip uses consistent complete order");
        result = Match({{L"", false, a.bounds}}, {a, b});
        Check(result.items[0].record == kNone, "shared rectangle never becomes unique after consumption");
        Check(Title(L" \r\n Steam \r\n status") == L"Steam", "tooltip first line");
        TestMojibakeAndKnownShell();
        TestOrderAndDedup();
        TestAttachRemainingAndMultiInstance();
        std::cout << "tray matching: all scenarios passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
