#include "shell/wechat_source.h"

#include "log.h"
#include "shell/uia_client.h"

#include <uiautomation.h>

#include <winrt/base.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

namespace {

enum OpenLogBit : uint32_t {
    LogInvoke = 1u << 0,
    LogSelect = 1u << 1,
    LogLegacy = 1u << 2,
    LogDialog = 1u << 3,
    LogHung = 1u << 4,
    LogTimeout = 1u << 5,
    LogNotFound = 1u << 6,
    LogNoPattern = 1u << 7,
};

void LogOpenOnce(uint32_t& mask, OpenLogBit bit, const wchar_t* message)
{
    if ((mask & bit) == 0) {
        mask |= bit;
        Log(L"%s", message);
    }
}

DWORD WindowPid(HWND h)
{
    DWORD pid = 0;
    if (h)
        GetWindowThreadProcessId(h, &pid);
    return pid;
}

bool IsVisibleNonIconic(HWND h) { return h && IsWindowVisible(h) && !IsIconic(h); }

HWND FindVisibleWeChatMain(const std::vector<std::pair<DWORD, std::wstring>>& procs, HWND lastMainHwnd, bool& anyHung)
{
    if (IsVisibleNonIconic(lastMainHwnd)) {
        auto it = std::ranges::find_if(procs, [pid = WindowPid(lastMainHwnd)](const auto& p) { return p.first == pid; });
        if (it != procs.end() && detail::ShouldReadWeChatWindow(lastMainHwnd, it->first, &anyHung))
            return lastMainHwnd;
    }
    for (const auto& [pid, program] : procs)
        for (HWND h : detail::WeChatWindows(pid, anyHung))
            if (IsVisibleNonIconic(h))
                return h;
    return nullptr;
}

bool ActivateChatCell(IUIAutomationElement* e, uint32_t& loggedMask)
{
    winrt::com_ptr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke && SUCCEEDED(invoke->Invoke())) {
        LogOpenOnce(loggedMask, LogInvoke, L"wechat: open chat via InvokePattern");
        return true;
    }
    winrt::com_ptr<IUIAutomationSelectionItemPattern> select;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(select.put()))) && select && SUCCEEDED(select->Select())) {
        LogOpenOnce(loggedMask, LogSelect, L"wechat: open chat via SelectionItemPattern");
        return true;
    }
    winrt::com_ptr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(legacy.put()))) && legacy && SUCCEEDED(legacy->DoDefaultAction())) {
        LogOpenOnce(loggedMask, LogLegacy, L"wechat: open chat via LegacyIAccessible");
        return true;
    }
    LogOpenOnce(loggedMask, LogNoPattern, L"wechat: open chat failed (no supported UIA pattern)");
    return false;
}

enum class OpenAttempt { Retry, Done };

OpenAttempt TryOpenChatInWindow(IUIAutomation* uia, IUIAutomationCondition* cells, HWND target, const std::wstring& chat, uint32_t& loggedMask)
{
    winrt::com_ptr<IUIAutomationElement> root;
    winrt::com_ptr<IUIAutomationElementArray> all;
    if (FAILED(uia->ElementFromHandle(target, root.put())) || !root || FAILED(root->FindAll(TreeScope_Descendants, cells, all.put())) || !all)
        return OpenAttempt::Retry;
    int n = 0;
    all->get_Length(&n);
    if (n == 0)
        return OpenAttempt::Retry;
    for (int i = 0; i < n; ++i) {
        winrt::com_ptr<IUIAutomationElement> e;
        all->GetElement(i, e.put());
        if (!e)
            continue;
        BSTR name = nullptr;
        const bool matched = SUCCEEDED(e->get_CurrentName(&name)) && name && MatchesWeChatCellName(name, chat);
        SysFreeString(name);
        if (matched) {
            ActivateChatCell(e.get(), loggedMask);
            return OpenAttempt::Done;
        }
    }
    LogOpenOnce(loggedMask, LogNotFound, L"wechat: open chat skipped (cell not in visible list)");
    return OpenAttempt::Done;
}

} // namespace

bool MatchesWeChatCellName(const std::wstring& cellName, const std::wstring& chat) { return !chat.empty() && ParseWeChatRow(cellName).chat == chat; }

void WeChatSource::OpenChat(const std::wstring& chat)
{
    if (chat.empty() || stop_)
        return;
    {
        std::scoped_lock lock(openMu_);
        pendingOpenChat_ = chat;
    }
    if (wake_)
        SetEvent(wake_);
}

namespace detail {

void HandlePendingWeChatOpen(IUIAutomation* uia, IUIAutomationCondition* cells, HANDLE wake, const std::atomic<bool>& stop, std::mutex& openMu,
                             std::wstring& pendingOpenChat, std::vector<std::pair<DWORD, std::wstring>>& procs, HWND& lastMainHwnd, uint32_t& loggedMask)
{
    std::wstring chat;
    {
        std::scoped_lock lock(openMu);
        if (pendingOpenChat.empty())
            return;
        chat = std::move(pendingOpenChat);
        pendingOpenChat.clear();
    }
    constexpr DWORD kOpenWaitMs = 1500;
    const ULONGLONG deadline = GetTickCount64() + kOpenWaitMs;
    while (!stop) {
        if (procs.empty())
            DiscoverWeChatProcesses(procs);
        if (!procs.empty()) {
            if (QueryWeChatBusyState(procs) == WeChatBusyState::Dialog) {
                LogOpenOnce(loggedMask, LogDialog, L"wechat: open chat skipped (modal dialog)");
                return;
            }
            bool anyHung = false;
            HWND target = FindVisibleWeChatMain(procs, lastMainHwnd, anyHung);
            if (anyHung) {
                LogOpenOnce(loggedMask, LogHung, L"wechat: open chat skipped (hung window)");
                return;
            }
            if (target && TryOpenChatInWindow(uia, cells, target, chat, loggedMask) == OpenAttempt::Done) {
                lastMainHwnd = target;
                return;
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        WaitForUiaWake(wake, DWORD(std::min<ULONGLONG>(150, deadline - now)));
    }
    LogOpenOnce(loggedMask, LogTimeout, L"wechat: open chat timed out waiting for visible window");
}

} // namespace detail

} // namespace atrium
