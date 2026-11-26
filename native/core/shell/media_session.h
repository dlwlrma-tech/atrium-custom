// Event-driven GSMTC media session watcher for the Now Playing taskbar entry and card.
#pragma once

#include "shell/media_snapshot.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class MediaSessionWatcher {
public:
    // onPeak is invoked on the watcher's background thread only.
    MediaSessionWatcher(HWND notify, UINT stateMsg, std::function<void(float peak, bool playing)> onPeak);
    ~MediaSessionWatcher();
    MediaSessionWatcher(const MediaSessionWatcher&) = delete;
    MediaSessionWatcher& operator=(const MediaSessionWatcher&) = delete;

    void SetFilter(bool showPaused, std::wstring appMode, std::vector<std::wstring> apps);
    void SetSamplingActive(bool barVisible);
    void SetCardVisible(bool visible);

    void Command(MediaCommand command, int64_t positionUs = 0);
    void PlayPause() { Command(MediaCommand::PlayPause, 0); }
    void Next() { Command(MediaCommand::Next, 0); }
    void Previous() { Command(MediaCommand::Previous, 0); }
    void Seek(int64_t positionUs) { Command(MediaCommand::Seek, positionUs); }
    void CycleSession(int delta);
    void ActivateApp() { Command(MediaCommand::ActivateApp, 0); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atrium
