// Pure backdrop source selection, retry backoff, and black-capture fallback policy.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace atrium {

class CapturePolicy {
public:
    enum class InitialSource { Wallpaper, Duplication, MonitorCapture };
    enum class BlackAction { RestartCapture, SwitchToMonitorCapture, FallbackToWallpaper };
    enum class DuplicationFailure { Transient, Elsewhere };

    static constexpr uint64_t kRetryBackoffMs = 30000;
    static constexpr uint64_t kMaxRetryBackoffMs = 300000;
    static constexpr uint64_t kBlackDetectMs = 1500;
    static constexpr uint64_t kBlackWindowMs = 30000;
    static constexpr float kBlackLumaThreshold = 0.01f;
    static constexpr int kMaxBlackRestarts = 2;

    void BeginSession(uintptr_t monitorId = 0, unsigned captureGeneration = 0)
    {
        blackSince_.reset();
        if (elsewhere_ && (monitorId != elsewhereMonitor_ || captureGeneration != elsewhereGeneration_)) {
            elsewhere_ = false;
            if (!blackGeneration_ || captureGeneration != *blackGeneration_) {
                duplicationFailed_ = false;
                retryBackoffMs_ = kRetryBackoffMs;
            }
        }
    }

    void ResetBlackTimer() { blackSince_.reset(); }

    InitialSource ChooseInitialSource(bool remote, uint64_t nowMs) const
    {
        if (remote || nowMs < wallpaperUntil_)
            return InitialSource::Wallpaper;
        return duplicationFailed_ ? InitialSource::MonitorCapture : InitialSource::Duplication;
    }

    void OnDuplicationUnavailable(uint64_t nowMs, DuplicationFailure reason = DuplicationFailure::Transient, uintptr_t monitorId = 0,
                                  unsigned captureGeneration = 0)
    {
        duplicationFailed_ = true;
        if (reason == DuplicationFailure::Elsewhere) {
            elsewhere_ = true;
            elsewhereMonitor_ = monitorId;
            elsewhereGeneration_ = captureGeneration;
            return;
        }
        elsewhere_ = false;
        duplicationRetryAt_ = nowMs + retryBackoffMs_;
        retryBackoffMs_ = std::min<uint64_t>(retryBackoffMs_ * 2, kMaxRetryBackoffMs);
    }

    bool ShouldProbeDuplication(bool needScreen, bool remote, bool captureKeepsOwnOut, uintptr_t monitorId, unsigned captureGeneration, uint64_t nowMs)
    {
        if (!needScreen || remote || !duplicationFailed_ || captureKeepsOwnOut)
            return false;
        if (elsewhere_ && monitorId == elsewhereMonitor_ && captureGeneration == elsewhereGeneration_)
            return false;
        if (blackGeneration_ && *blackGeneration_ == captureGeneration)
            return false;
        if (nowMs < duplicationRetryAt_)
            return false;
        duplicationRetryAt_ = nowMs + retryBackoffMs_;
        retryBackoffMs_ = std::min<uint64_t>(retryBackoffMs_ * 2, kMaxRetryBackoffMs);
        return true;
    }

    bool ShouldProbeDuplication(bool needScreen, bool remote, bool captureKeepsOwnOut, unsigned captureGeneration, uint64_t nowMs)
    {
        return ShouldProbeDuplication(needScreen, remote, captureKeepsOwnOut, 0, captureGeneration, nowMs);
    }

    void OnDuplicationRecovered()
    {
        duplicationFailed_ = false;
        elsewhere_ = false;
        blackGeneration_.reset();
        retryBackoffMs_ = kRetryBackoffMs;
    }

    bool ShouldExitBlackoutWallpaper(bool blackOut, uint64_t nowMs) const { return blackOut && nowMs >= wallpaperUntil_; }

    // Keep the GDI ScreenLitAbove() sample gated behind the 1500 ms dark-backdrop timer.
    bool PollBlackBackdrop(bool eligible, float brightestLuma, uint64_t nowMs)
    {
        if (!eligible || brightestLuma > kBlackLumaThreshold) {
            blackSince_.reset();
            if (blackRestartMs_ && nowMs >= *blackRestartMs_ && nowMs - *blackRestartMs_ > kBlackWindowMs) {
                blackRestarts_ = 0;
                blackRestartMs_.reset();
            }
            return false;
        }
        if (!blackSince_) {
            blackSince_ = nowMs;
            return false;
        }
        if (nowMs < *blackSince_ || nowMs - *blackSince_ <= kBlackDetectMs)
            return false;
        blackSince_.reset();
        return true;
    }

    BlackAction OnBlackOverLitScreen(unsigned captureGeneration, uint64_t nowMs)
    {
        blackRestartMs_ = nowMs;
        if (++blackRestarts_ < kMaxBlackRestarts)
            return BlackAction::RestartCapture;
        blackRestarts_ = 0;
        blackRestartMs_.reset();
        if (duplicationFailed_) {
            wallpaperUntil_ = nowMs + kRetryBackoffMs;
            return BlackAction::FallbackToWallpaper;
        }
        duplicationFailed_ = true;
        duplicationRetryAt_ = nowMs + kRetryBackoffMs;
        retryBackoffMs_ = kRetryBackoffMs;
        blackGeneration_ = captureGeneration;
        return BlackAction::SwitchToMonitorCapture;
    }

private:
    bool duplicationFailed_ = false;
    bool elsewhere_ = false;
    uintptr_t elsewhereMonitor_ = 0;
    unsigned elsewhereGeneration_ = 0;
    uint64_t duplicationRetryAt_ = 0;
    uint64_t retryBackoffMs_ = kRetryBackoffMs;
    std::optional<unsigned> blackGeneration_;
    uint64_t wallpaperUntil_ = 0;
    int blackRestarts_ = 0;
    std::optional<uint64_t> blackRestartMs_;
    std::optional<uint64_t> blackSince_;
};

} // namespace atrium
