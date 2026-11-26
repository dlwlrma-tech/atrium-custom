#include "notice_actions.h"

#include "desktop_helpers.h"
#include "localization.h"
#include "shell/task_model.h"
#include "shell/tray_model.h"
#include "shell/wechat_source.h"
#include "ui/notifications.h"
#include "win32_path.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <cwctype>

namespace atrium {

namespace {

std::wstring StemOf(const std::wstring& s)
{
    std::wstring v = FoldCase(s);
    if (const size_t bang = v.find(L'!'); bang != std::wstring::npos)
        v.resize(bang);
    v = v.substr(v.find_last_of(L"\\/") + 1);
    return (v.size() > 4 && v.ends_with(L".exe")) ? v.substr(0, v.size() - 4) : v;
}

std::optional<TrayIcon> TrayIconOf(const TrayModel* tray, const Notice& n)
{
    if (!tray)
        return std::nullopt;
    const std::wstring a = StemOf(n.app), b = StemOf(n.appId);
    for (const TrayIcon& icon : tray->Icons()) {
        const std::wstring stem = icon.exe.empty() ? L"" : StemOf(icon.exe), tip = FoldCase(TrayModel::Title(icon.tip));
        for (const std::wstring& name : {a, b})
            if (name.size() >= 2 && (stem == name || (tip.starts_with(name) && (tip.size() == name.size() || !iswalnum(tip[name.size()])))))
                return icon;
    }
    return std::nullopt;
}

bool SameAppItem(const TaskItem& t, const PinnedApp& pin, const std::wstring& key, Messenger m, const TaskModel* tasks)
{
    if (t.key == key || (!pin.aumid.empty() && FoldCase(pin.aumid) == key) || (!pin.path.empty() && FoldCase(pin.path) == key) ||
        (tasks && FoldCase(tasks->FileLocation(t.key)) == key))
        return true;
    const std::wstring aumid = FoldCase(!pin.aumid.empty() ? pin.aumid : t.key);
    return m != Messenger::None && (MessengerOfProgram(pin.path) == m || MessengerOfProgram(t.key) == m || MessengerOfNotice(L"windows", t.name, aumid) == m ||
                                    (m == Messenger::QQ && (aumid.find(L"tencentqq") != std::wstring::npos || aumid.find(L".qq") != std::wstring::npos ||
                                                            aumid.find(L"qqdesktop") != std::wstring::npos || aumid.find(L"qqnt") != std::wstring::npos)));
}

bool ShellExecuteApp(const std::wstring& aumid)
{
    const std::wstring path = (aumid.size() > 2 && aumid[1] == L':') ? aumid : L"shell:AppsFolder\\" + aumid;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool ActivateAumidApp(const std::wstring& aumid)
{
    AllowSetForegroundWindow(ASFW_ANY);
    winrt::com_ptr<IApplicationActivationManager> aam;
    DWORD pid = 0;
    return SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(aam.put()))) &&
           SUCCEEDED(aam->ActivateApplication(aumid.c_str(), nullptr, AO_NONE, &pid));
}

} // namespace

NoticeAppRoute ChooseNoticeAppRoute(const NoticeAppRouteInput& in)
{
    if (in.hasMatchingTitleWindow)
        return NoticeAppRoute::ActivateTitleWindow;
    if (in.isWeChatChat)
        return NoticeAppRoute::WeChatOpenChat;
    if (in.hasTrayIcon && (in.trayFlashing || !in.hasAppWindow))
        return NoticeAppRoute::ClickTrayIcon;
    return in.hasAppWindow ? NoticeAppRoute::ShowAppWindow : (in.isPackagedAumid ? NoticeAppRoute::ActivateAumid : NoticeAppRoute::ShellOpen);
}

std::pair<std::wstring, HWND> FindAppTarget(const TaskModel* tasks, const std::wstring& key, Messenger messenger, const std::wstring& title)
{
    if (!tasks)
        return {};
    std::wstring appTaskKey;
    HWND titleHwnd = nullptr;
    for (const TaskItem& t : tasks->Snapshot(true)) {
        if (t.windows.empty() || !SameAppItem(t, tasks->PinFor(t.key), key, messenger, tasks))
            continue;
        if (appTaskKey.empty())
            appTaskKey = t.key;
        for (HWND h : t.windows) {
            wchar_t buf[512]{};
            // InternalGetWindowText never sends WM_GETTEXT, so a hung window cannot block the UI thread.
            if (!title.empty() && !titleHwnd && InternalGetWindowText(h, buf, ARRAYSIZE(buf)) > 0 && buf == title)
                titleHwnd = h;
        }
    }
    return {appTaskKey, titleHwnd};
}

bool RunNoticeAction(const NoticeActionEnv& env, const Notice& n)
{
    if (n.action == L"settings:updates")
        return env.openSettings ? (env.openSettings(L"about"), true) : true;
    if (n.action.starts_with(L"uri:"))
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", n.action.substr(4).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    if (!n.action.starts_with(L"app:"))
        return true;

    const std::wstring aumid = n.action.substr(4);
    const std::optional<TrayIcon> trayIcon = TrayIconOf(env.tray, n);
    const auto [appTaskKey, titleHwnd] = FindAppTarget(env.tasks, FoldCase(aumid), MessengerOfNotice(n.source, n.app, n.appId), n.title);
    auto clickTray = [&] {
        if (env.tray && trayIcon)
            AllowSetForegroundWindow(ASFW_ANY), env.tray->Click(*trayIcon, false);
    };
    switch (ChooseNoticeAppRoute({titleHwnd != nullptr, n.source == L"wechat" && (n.title != Tr(L"New message") || !n.body.empty()), trayIcon.has_value(),
                                  trayIcon && trayIcon->flashing, !appTaskKey.empty(), aumid.find(L'!') != std::wstring::npos})) {
    case NoticeAppRoute::ActivateTitleWindow: TaskModel::ActivateWindow(titleHwnd); return true;
    case NoticeAppRoute::WeChatOpenChat:
        if (!appTaskKey.empty() && env.tasks)
            env.tasks->Show(appTaskKey);
        else if (trayIcon && env.tray)
            clickTray();
        else if (!(env.tray && env.tray->OpenExisting(aumid)))
            ShellExecuteApp(aumid);
        if (env.wechat)
            env.wechat->OpenChat(n.title);
        return true;
    case NoticeAppRoute::ClickTrayIcon: clickTray(); return true;
    case NoticeAppRoute::ShowAppWindow: return env.tasks ? (env.tasks->Show(appTaskKey), true) : true;
    case NoticeAppRoute::ActivateAumid: return ActivateAumidApp(aumid) || ShellExecuteApp(aumid);
    case NoticeAppRoute::ShellOpen: return ShellExecuteApp(aumid);
    }
    return true;
}

void WithdrawForegroundMessenger(MessengerAlerts& alerts, Notifications* notes)
{
    const HWND fg = GetForegroundWindow();
    if (const Messenger m = fg ? MessengerOfProgram(ProgramOf(fg)) : Messenger::None; m != Messenger::None) {
        alerts.Cancel(m);
        if (notes)
            notes->Post(Notice{.source = InfoOf(m).key, .title = Tr(L"New message"), .unread = false});
    }
}

} // namespace atrium
