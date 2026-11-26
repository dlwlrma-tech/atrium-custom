// Windows notifications as Atrium cards, through UserNotificationListener
// (on by default since v0.6, D24). Unpackaged processes cannot subscribe to its
// change event, so a worker thread reads the toast list every second and posts
// new toasts, and toasts whose text changed, to the UI thread. The first read
// only records what is already there. While access is unavailable, it checks
// for access changes every ten seconds instead of ending the reader.
// It never dismisses or changes anything
// in Windows, so the system banners still appear unless the user turns an
// app's banners off in Windows (keeping it in the notification centre).
#pragma once

#include "notice.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct SeenToast {
    size_t hash = 0;
    bool posted = false;
    std::wstring app;
    std::wstring appId;
};

// Withdrawals for posted toasts absent from `now`, skipped on `first` or under `notifyAlways` unless in front.
std::vector<Notice> DiffGoneToasts(const std::map<uint32_t, SeenToast>& seen, const std::map<uint32_t, SeenToast>& now, bool first, bool notifyAlways,
                                   const std::function<bool(const SeenToast&)>& senderInFront);

class NotificationListener {
public:
    // Posts `msg` to `notify` with a heap-allocated Notice* in lParam (the receiver deletes it).
    NotificationListener(HWND notify, UINT msg);
    ~NotificationListener();
    NotificationListener(const NotificationListener&) = delete;
    NotificationListener& operator=(const NotificationListener&) = delete;
    void SetAlways(bool on) { always_.store(on, std::memory_order_relaxed); }

private:
    void Run();
    HWND notify_;
    UINT msg_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> always_{false};
    HANDLE wake_ = nullptr;
    std::thread thread_;
};

} // namespace atrium
