#include "render/shutdown_wait.h"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

using namespace atrium;

namespace {

constexpr UINT kCaptureMsg = WM_APP + 0x70;
constexpr UINT kOtherMsg = WM_APP + 99;
constexpr UINT kSyncMsg = WM_APP + 100;

struct WindowState {
    HANDLE ack = nullptr;
    bool captureHandled = false;
    bool otherHandled = false;
    bool syncHandled = false;
};

LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state && msg == kCaptureMsg) {
        state->captureHandled = true;
        if (state->ack)
            SetEvent(state->ack);
        return 0;
    }
    if (state && msg == kOtherMsg) {
        state->otherHandled = true;
        return 0;
    }
    if (state && msg == kSyncMsg) {
        state->syncHandled = true;
        return 42;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

bool TestWaitCaptureAckOrQuit()
{
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ack = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE hold = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Require(quit && ack && hold, "CreateEventW must succeed")) {
        if (quit)
            CloseHandle(quit);
        if (ack)
            CloseHandle(ack);
        if (hold)
            CloseHandle(hold);
        return false;
    }

    SetEvent(quit);
    const ULONGLONG t0 = GetTickCount64();
    const bool immediateQuit = !WaitCaptureAckOrQuit(quit, ack, 1000);
    const ULONGLONG dt0 = GetTickCount64() - t0;

    ResetEvent(quit);
    std::thread quitter([quit, hold] {
        WaitForSingleObject(hold, 20);
        SetEvent(quit);
    });
    const ULONGLONG t1 = GetTickCount64();
    const bool midQuit = !WaitCaptureAckOrQuit(quit, ack, 1000);
    const ULONGLONG dt1 = GetTickCount64() - t1;
    quitter.join();

    ResetEvent(quit);
    SetEvent(ack);
    const bool acked = WaitCaptureAckOrQuit(quit, ack, 1000);

    CloseHandle(quit);
    CloseHandle(ack);
    CloseHandle(hold);
    return Require(immediateQuit && dt0 < 250, "already-signaled quit must abort WaitCaptureAckOrQuit immediately") &&
           Require(midQuit && dt1 < 250, "mid-wait quit must abort WaitCaptureAckOrQuit well under 1000 ms") &&
           Require(acked, "signaled ack without quit must return true");
}

bool TestPreserveQuitAndFilterMessages(HWND hwnd, WindowState& state)
{
    HANDLE hold = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Require(hold != nullptr, "CreateEventW(hold) must succeed"))
        return false;

    PostQuitMessage(77);
    std::thread quick([hold] { WaitForSingleObject(hold, 20); });
    const bool quitRunOk = WaitRenderShutdown(quick.native_handle(), hwnd, kCaptureMsg, 2000);
    quick.join();
    CloseHandle(hold);

    MSG emptyQuit{};
    const bool emptyQuitPreserved = PeekMessageW(&emptyQuit, nullptr, 0, 0, PM_REMOVE) != 0 && emptyQuit.message == WM_QUIT && emptyQuit.wParam == 77;

    ResetEvent(state.ack);
    state.captureHandled = false;
    state.otherHandled = false;

    PostQuitMessage(88);
    if (!Require(PostMessageW(hwnd, kOtherMsg, 0, 0) != 0, "PostMessageW(kOtherMsg) must succeed") ||
        !Require(PostMessageW(hwnd, kCaptureMsg, 1, 0) != 0, "PostMessageW(kCaptureMsg) must succeed"))
        return false;

    std::thread worker([ack = state.ack] { WaitForSingleObject(ack, 2000); });
    const bool stopped = WaitRenderShutdown(worker.native_handle(), hwnd, kCaptureMsg, 2000);
    worker.join();

    const bool ackSet = WaitForSingleObject(state.ack, 0) == WAIT_OBJECT_0;
    MSG other{};
    const bool otherQueued = PeekMessageW(&other, hwnd, kOtherMsg, kOtherMsg, PM_REMOVE) != 0;
    MSG mixedQuit{};
    const bool mixedQuitPreserved =
        PeekMessageW(&mixedQuit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != 0 && mixedQuit.message == WM_QUIT && mixedQuit.wParam == 88;

    return Require(quitRunOk && emptyQuitPreserved, "WaitRenderShutdown must leave pre-posted WM_QUIT untouched") &&
           Require(stopped, "WaitRenderShutdown must succeed when captureMsg unblocks worker") &&
           Require(state.captureHandled && ackSet, "WaitRenderShutdown must dispatch captureMsg and set ack") &&
           Require(!state.otherHandled && otherQueued, "WaitRenderShutdown must not dispatch unrelated queued messages") &&
           Require(mixedQuitPreserved, "WaitRenderShutdown must preserve WM_QUIT alongside queued messages");
}

bool TestSyncSendMessageAndTimeout(HWND hwnd, WindowState& state)
{
    state.syncHandled = false;
    std::atomic<LRESULT> reply{0};
    std::thread sender([hwnd, &reply] { reply = SendMessageW(hwnd, kSyncMsg, 0, 0); });
    const bool syncStopped = WaitRenderShutdown(sender.native_handle(), hwnd, kCaptureMsg, 2000);
    sender.join();

    state.otherHandled = false;
    if (!Require(PostMessageW(hwnd, kOtherMsg, 0, 0) != 0, "PostMessageW(kOtherMsg) before timeout must succeed"))
        return false;
    HANDLE block = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Require(block != nullptr, "CreateEventW(block) must succeed"))
        return false;
    std::thread stuck([block] { WaitForSingleObject(block, 5000); });
    unsigned iterations = 0;
    const ULONGLONG t0 = GetTickCount64();
    const bool timedOut = !WaitRenderShutdown(stuck.native_handle(), hwnd, kCaptureMsg, 80, &iterations);
    const ULONGLONG dt = GetTickCount64() - t0;
    SetEvent(block);
    stuck.join();
    CloseHandle(block);

    MSG other{};
    const bool otherQueued = PeekMessageW(&other, hwnd, kOtherMsg, kOtherMsg, PM_REMOVE) != 0;

    return Require(syncStopped && state.syncHandled && reply == 42, "WaitRenderShutdown must service synchronous cross-thread SendMessage") &&
           Require(timedOut && dt < 1000, "WaitRenderShutdown must return false on bounded timeout") &&
           Require(iterations > 0 && iterations < 20, "WaitRenderShutdown must not busy-spin with unrelated posted messages queued") &&
           Require(!state.otherHandled && otherQueued, "WaitRenderShutdown timeout must leave queued unrelated message intact");
}

} // namespace

int main()
{
    const std::wstring className = L"AtriumBarShutdownTest_" + std::to_wstring(GetCurrentProcessId());
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = &TestWndProc;
    wc.lpszClassName = className.c_str();
    if (!Require(RegisterClassExW(&wc) != 0, "RegisterClassExW must succeed"))
        return 1;

    HANDLE ack = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, className.c_str(), L"BarShutdownTest", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!Require(ack != nullptr && hwnd != nullptr, "test window and event creation must succeed")) {
        if (hwnd)
            DestroyWindow(hwnd);
        if (ack)
            CloseHandle(ack);
        UnregisterClassW(className.c_str(), instance);
        return 1;
    }

    WindowState state{ack};
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    const bool ok = TestWaitCaptureAckOrQuit() && TestPreserveQuitAndFilterMessages(hwnd, state) && TestSyncSendMessageAndTimeout(hwnd, state);

    DestroyWindow(hwnd);
    CloseHandle(ack);
    UnregisterClassW(className.c_str(), instance);
    if (!ok)
        return 1;

    std::cout << "bar shutdown: all checks passed\n";
    return 0;
}
