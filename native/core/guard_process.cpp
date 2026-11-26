#include "guard_process.h"

#include "log.h"
#include "settings.h"
#include "shell/shell_taskbar.h"
#include "shell/system_chrome.h"

#include <objbase.h>

#include <iterator>
#include <memory>
#include <string>

namespace atrium {

namespace {

constexpr wchar_t kGuardMutex[] = L"Local\\Atrium.Guard"; // held by the guard while it runs

HWND FindProcessWindow(const wchar_t* className, DWORD pid)
{
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, className, nullptr)) != nullptr;) {
        DWORD owner = 0;
        if (GetWindowThreadProcessId(w, &owner); owner == pid)
            return w;
    }
    return nullptr;
}

} // namespace

// While Windows' taskbar or its banners are hidden, a second Atrium.exe (`--guard`, RunGuard)
// watches this one: however it ends (a crash past the exception filter, abort, being killed),
// what it changed in Windows is put back at once; while it does not respond, the taskbar shows.
void EnsureGuard(HANDLE& guard)
{
    if (guard && WaitForSingleObject(guard, 0) == WAIT_TIMEOUT)
        return;
    if (guard) {
        CloseHandle(guard);
        guard = nullptr;
    }
    wchar_t exe[MAX_PATH * 2];
    if (!GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe))))
        return;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --guard " + std::to_wstring(GetCurrentProcessId()) + DataDirectoryArgs();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"guard: could not start (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    guard = pi.hProcess;
    Log(L"guard: started (%lu)", pi.dwProcessId);
}

int RunGuard(DWORD pid, const wchar_t* hostClass)
{
    HANDLE held = CreateMutexW(nullptr, TRUE, kGuardMutex);
    if (held && GetLastError() == ERROR_ALREADY_EXISTS)
        if (const DWORD wait = WaitForSingleObject(held, 5000); wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(held);
            held = nullptr;
        }
    auto release = [](HANDLE h) {
        ReleaseMutex(h);
        CloseHandle(h);
    };
    std::unique_ptr<void, decltype(release)> guard(held, release);
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return 1;
    HWND host = nullptr;
    int silent = 0, traySilent = 0; // checks in a row without an answer (and Atrium's tray host)
    bool shown = false, trayBack = false;
    while (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT) {
        // Apps' tray calls go to the tray host first and wait for it. Not answering
        // (Atrium hung, or its host thread), Explorer's taskbar goes in front of it so
        // that they reach Explorer; the host moves back once it runs again.
        HWND trayHost = FindProcessWindow(L"Shell_TrayWnd", pid);
        DWORD_PTR answer = 0;
        const bool trayAlive = !trayHost || SendMessageTimeoutW(trayHost, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &answer);
        traySilent = trayAlive ? 0 : traySilent + 1;
        if (traySilent >= 2) {
            if (HWND shell = ShellTaskbar(pid))
                SetWindowPos(shell, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            if (!trayBack)
                Log(L"guard: Atrium's tray host (%lu) is not responding: Explorer's tray in front", pid);
            trayBack = true;
        } else if (trayAlive && trayBack) {
            Log(L"guard: Atrium's tray host (%lu) responds again", pid);
            trayBack = false;
        }
        if (!host || !IsWindow(host))
            host = FindProcessWindow(hostClass, pid);
        const bool alive = !host || SendMessageTimeoutW(host, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &answer);
        silent = alive ? 0 : silent + 1;
        if (silent >= 4 && !shown) {
            // About 15 s without an answer: the taskbar back (Atrium hides it again if it recovers).
            Log(L"guard: Atrium (%lu) is not responding: Windows' taskbar shown", pid);
            SystemChrome::ShowTaskbarsNow();
            shown = true;
        } else if (alive && shown) {
            Log(L"guard: Atrium (%lu) responds again", pid);
            shown = false;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    // A clean exit put everything back (system.json is gone); anything else is put back here.
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        if (SystemChrome::UndoLeftover())
            Log(L"guard: Atrium (%lu) ended without putting Windows back (exit 0x%08lX): done here", pid, code);
        CoUninitialize();
    }
    return 0;
}

// The guard of a copy that quits: it puts Windows' taskbar back as that copy ends.
void WaitForGuard()
{
    if (HANDLE guard = OpenMutexW(SYNCHRONIZE, FALSE, kGuardMutex)) {
        DWORD done = WaitForSingleObject(guard, 5000);
        if (done == WAIT_OBJECT_0 || done == WAIT_ABANDONED)
            ReleaseMutex(guard);
        CloseHandle(guard);
    }
}

} // namespace atrium
