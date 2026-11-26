// Bounded shutdown and capture-ack waits for the render and UI threads.
#pragma once

#include <windows.h>

#include <algorithm>

namespace atrium {

inline bool WaitCaptureAckOrQuit(HANDLE quit, HANDLE ack, DWORD timeoutMs)
{
    if (!quit || !ack)
        return false;
    HANDLE handles[] = {quit, ack};
    return WaitForMultipleObjects(2, handles, FALSE, timeoutMs) == WAIT_OBJECT_0 + 1;
}

inline bool WaitRenderShutdown(HANDLE thread, HWND visual, UINT captureMsg, DWORD timeoutMs, unsigned* iterations = nullptr)
{
    if (iterations)
        *iterations = 0;
    if (!thread)
        return true;
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    for (;;) {
        if (iterations)
            ++(*iterations);
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0)
            return true;
        static_cast<void>(GetQueueStatus(QS_SENDMESSAGE | QS_POSTMESSAGE));
        MSG msg{};
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        if (visual && captureMsg) {
            while (PeekMessageW(&msg, visual, captureMsg, captureMsg, PM_NOREMOVE) && msg.message == captureMsg) {
                PeekMessageW(&msg, visual, captureMsg, captureMsg, PM_REMOVE);
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0)
            return true;
        const ULONGLONG now = GetTickCount64();
        if (now >= until)
            return false;
        const DWORD remaining = static_cast<DWORD>(std::min<ULONGLONG>(50, until - now));
        const DWORD wait = MsgWaitForMultipleObjects(1, &thread, FALSE, remaining, QS_SENDMESSAGE | QS_POSTMESSAGE);
        if (wait == WAIT_OBJECT_0)
            return true;
        if (wait == WAIT_FAILED)
            return false;
    }
}

} // namespace atrium
