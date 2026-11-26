// The length of a track in NetEase Cloud Music's play queue. The player tells Windows the track but
// no timeline; it keeps its queue, each track with its length and id, in its own files
// (%LOCALAPPDATA%\NetEase\CloudMusic\webdata\file\playingList), read again only when that changes.
// And whether the user likes it: its liked list lives in the player's local database. Media worker thread.
#pragma once

#include "shell/media_timeline.h"
#include "shell/netease_memory.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace atrium {

struct NeteaseQueuedTrack {
    std::wstring title, artists; // artists: their names joined by "/", as the player tells Windows
    int64_t lengthUs = 0;
    std::wstring id; // the player's track id
};

class NeteaseQueueCache {
public:
    bool NeedsRead(uint64_t writeTime, uint64_t sizeBytes) const { return writeTime != writeTime_ || sizeBytes != sizeBytes_; }
    bool Update(const std::wstring& text, uint64_t writeTime, uint64_t sizeBytes);
    int64_t FindLengthUs(const std::wstring& title, const std::wstring& artist) const;
    const NeteaseQueuedTrack* Find(const std::wstring& title, const std::wstring& artist) const;

private:
    uint64_t writeTime_ = 0;
    uint64_t sizeBytes_ = 0;
    std::vector<NeteaseQueuedTrack> tracks_;
};

// The queued track named `title` (by `artist`, when the queue has several of that name); 0: not found.
int64_t NeteaseTrackLengthUs(const std::wstring& title, const std::wstring& artist);

// The tracks the user likes (the player's 我喜欢的音乐, its playlist of specialType 5), as the player keeps
// them in its local database (Library\webdb.dat, SQLite, read with Windows' own winsqlite3): read again
// whenever that file is written, so a heart given in the player shows on the card a moment later.
class NeteaseLikes {
public:
    std::optional<bool> Liked(const std::wstring& title, const std::wstring& artist); // nullopt: not known

private:
    bool Read(const std::wstring& path, bool& retryable);
    std::wstring CurrentId(const std::wstring& title, const std::wstring& artist) const;
    uint64_t writeTime_ = 0;
    unsigned long long readAt_ = 0; // GetTickCount64 of the last read: a failed one is tried again
    std::wstring recentId_, recentName_; // the track the player last began (its history), id and name
    std::wstring playlist_; // the liked playlist's id, once found
    std::unordered_set<std::wstring> ids_;
    bool ok_ = false, retryable_ = false;
    std::wstring logged_; // the last answer logged
};

// The timeline the player does not report: its own position, read from its memory twice a second,
// else estimated from when the track was seen to start (TimelineEstimate).
class NeteaseTimeline {
public:
    void Fill(const std::wstring& title, const std::wstring& artist, bool playing, double nowMs, MediaTimeline& t);
    std::optional<bool> Liked(const std::wstring& title, const std::wstring& artist) { return likes_.Liked(title, artist); }

private:
    NeteaseMemory memory_;
    NeteaseLikes likes_;
    TimelineEstimate estimate_;
    std::wstring track_;
    int64_t lengthUs_ = 0;
    double lookedUpMs_ = -1e9, readMs_ = -1e9;
    bool playing_ = false, fromMemory_ = false;
    MediaTimeline last_; // from the memory: kept while the readings agree with it, so the card is not told again
};

} // namespace atrium
