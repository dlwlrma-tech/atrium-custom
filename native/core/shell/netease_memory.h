// NetEase Cloud Music's own position and length of the playing track, read (read only) from the
// player's memory: the player tells Windows no timeline but keeps both as doubles (seconds) in
// cloudmusic.dll's data. Their places move from version to version: known ones are listed; for any
// other the length is
// found by its value (the queue's length of the track), the position as the one double that advances
// with the clock while the track plays. Nothing is trusted before that. Media worker thread.
#pragma once

#include <windows.h>
#include <unknwn.h> // before C++/WinRT: its handlers implement classic COM interfaces

#include <winrt/base.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace atrium {

class NeteaseMemory {
public:
    // The track's position and length (s); false when the player, its places or the track are not
    // known yet. lengthSec: the queue's length of the track (0: unknown), which the player's must match.
    bool Read(double lengthSec, bool playing, double nowMs, double& position, double& length);

private:
    using Doubles = std::vector<std::pair<uintptr_t, double>>; // rva, value; by rva

    bool Attach();
    bool TryAttachProcess(DWORD pid);
    bool LoadPeSections(HMODULE module, DWORD pid);
    void Detach();
    void ResetMatch();
    bool Locate(double lengthSec);
    void Verify(bool playing, double nowMs, double length);
    bool ReadDouble(uintptr_t rva, double& value) const;
    void ScanRegion(uintptr_t rva, uintptr_t avail, double lo, double hi, std::vector<uint8_t>& bytes, Doubles& found) const;
    Doubles Scan(double lo, double hi) const; // every double in [lo, hi] in the writable sections

    winrt::handle process_;
    DWORD attachedPid_ = 0;
    DWORD avoidPid_ = 0;
    uintptr_t base_ = 0;
    uint32_t stamp_ = 0, imageSize_ = 0;
    std::vector<std::pair<uintptr_t, uintptr_t>> sections_; // writable: rva, size
    uintptr_t lengthRva_ = 0, positionRva_ = 0;
    bool trusted_ = false; // positionRva_ seen to advance with the clock
    double retryMs_ = 0;   // nothing found: not before then
    double mismatchSinceMs_ = 0;
    // Verifying: the candidates' values when last read, and when.
    Doubles candidates_;
    double probeMs_ = 0;
    int failures_ = 0;
};

} // namespace atrium
