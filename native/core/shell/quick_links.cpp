// Windows Win+X quick link menu items and their launch actions (quick_links.h).
#include "shell/quick_links.h"

#include "localization.h"
#include "log.h"
#include "shell/system_status.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <memory>

namespace atrium {

namespace {

enum QuickLinkId {
    InstalledApps = 1,
    MobilityCenter,
    PowerOptions,
    EventViewer,
    System,
    DeviceManager,
    NetworkConnections,
    DiskManagement,
    ComputerManagement,
    Terminal,
    TerminalAdmin,
    TaskManager,
    Settings,
    FileExplorer,
    Search,
    Run,
    SignOut,
    Sleep,
    Hibernate,
    ShutDown,
    Restart,
    Desktop,
};

bool HasMobilityCenter()
{
    wchar_t sys[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return false;
    const std::wstring path = std::wstring(sys, n) + L"\\mblctr.exe";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

const wchar_t* TerminalTarget() { return SearchPathW(nullptr, L"wt.exe", nullptr, 0, nullptr, nullptr) ? L"wt.exe" : L"powershell.exe"; }

void ShellOpen(const wchar_t* target, bool elevated)
{
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = elevated ? L"runas" : nullptr;
    sei.lpFile = target;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (!elevated || err != ERROR_CANCELLED)
            Log(L"quick_links: launch %s failed (%lu)", target, err);
    }
}

struct LaunchJob {
    const wchar_t* target = nullptr;
    bool elevated = false;
    bool terminal = false;
};

void CALLBACK LaunchWorker(PTP_CALLBACK_INSTANCE, void* ctx)
{
    const std::unique_ptr<LaunchJob> job(static_cast<LaunchJob*>(ctx));
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const wchar_t* target = job->terminal ? TerminalTarget() : job->target;
    if (target)
        ShellOpen(target, job->elevated);
    if (SUCCEEDED(hr))
        CoUninitialize();
}

void DispatchShellOpen(const wchar_t* target, bool elevated, bool terminal)
{
    auto job = std::make_unique<LaunchJob>(LaunchJob{target, elevated, terminal});
    if (TrySubmitThreadpoolCallback(LaunchWorker, job.get(), nullptr)) {
        [[maybe_unused]] LaunchJob* raw = job.release();
        return;
    }
    Log(L"quick_links: threadpool dispatch failed (%lu)", GetLastError());
}

void SendWinKey(WORD vk)
{
    INPUT in[4]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = vk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

} // namespace

std::vector<QuickLink> QuickLinks()
{
    std::vector<QuickLink> items;
    items.push_back({InstalledApps, Tr(L"Installed apps")});
    if (HasMobilityCenter())
        items.push_back({MobilityCenter, Tr(L"Mobility Center")});
    items.push_back({PowerOptions, Tr(L"Power Options")});
    items.push_back({EventViewer, Tr(L"Event Viewer")});
    items.push_back({System, Tr(L"System")});
    items.push_back({DeviceManager, Tr(L"Device Manager")});
    items.push_back({NetworkConnections, Tr(L"Network Connections")});
    items.push_back({DiskManagement, Tr(L"Disk Management")});
    items.push_back({ComputerManagement, Tr(L"Computer Management")});
    items.push_back({Terminal, Tr(L"Terminal")});
    items.push_back({TerminalAdmin, Tr(L"Terminal (Admin)")});
    items.push_back({});
    items.push_back({TaskManager, Tr(L"Task Manager")});
    items.push_back({Settings, Tr(L"Settings")});
    items.push_back({FileExplorer, Tr(L"File Explorer")});
    items.push_back({Search, Tr(L"Search")});
    items.push_back({Run, Tr(L"Run")});
    items.push_back({});
    items.push_back({0, Tr(L"Shut down or sign out"), true});
    items.push_back({SignOut, Tr(L"Sign out")});
    items.push_back({Sleep, Tr(L"Sleep")});
    if (CanHibernate())
        items.push_back({Hibernate, Tr(L"Hibernate")});
    items.push_back({ShutDown, Tr(L"Shut down")});
    items.push_back({Restart, Tr(L"Restart")});
    items.push_back({});
    items.push_back({Desktop, Tr(L"Desktop")});
    return items;
}

void RunQuickLink(int id)
{
    if (id == Terminal || id == TerminalAdmin) {
        DispatchShellOpen(nullptr, id == TerminalAdmin, true);
        return;
    }
    struct LaunchSpec {
        int id;
        const wchar_t* target;
    };
    static constexpr LaunchSpec kLaunches[] = {
        {InstalledApps, L"ms-settings:appsfeatures"},
        {MobilityCenter, L"mblctr.exe"},
        {PowerOptions, L"ms-settings:powersleep"},
        {EventViewer, L"eventvwr.msc"},
        {System, L"ms-settings:about"},
        {DeviceManager, L"devmgmt.msc"},
        {NetworkConnections, L"ms-settings:network"},
        {DiskManagement, L"diskmgmt.msc"},
        {ComputerManagement, L"compmgmt.msc"},
        {TaskManager, L"taskmgr.exe"},
        {Settings, L"ms-settings:"},
        {FileExplorer, L"explorer.exe"},
        {Run, L"shell:::{2559a1f3-21d7-11d4-bdaf-00c04f60b9f0}"},
    };
    for (const LaunchSpec& spec : kLaunches) {
        if (spec.id == id) {
            DispatchShellOpen(spec.target, false, false);
            return;
        }
    }
    switch (id) {
    case Search: SendWinKey('S'); break;
    case Desktop: SendWinKey('D'); break;
    case SignOut: ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER); break;
    case Sleep: RunPowerAction(PowerAction::Sleep); break;
    case Hibernate: RunPowerAction(PowerAction::Hibernate); break;
    case ShutDown: RunPowerAction(PowerAction::Shutdown); break;
    case Restart: RunPowerAction(PowerAction::Restart); break;
    default: break;
    }
}

} // namespace atrium
