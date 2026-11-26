// The players the now-playing card knows beyond what Windows tells of them: NetEase Cloud Music, QQ
// Music, Spotify and Apple Music, by their media session's app (an exe, or a packaged app's AUMID).
// NetEase likes a track through its own global shortcut (its default: Ctrl+Alt+L), its heart read from its
// own records; QQ Music keeps its likes where nothing can read them, so the card offers none for it. Both show
// their desktop lyrics by their shortcuts (Ctrl+Alt+D, Ctrl+Alt+W); QQ Music's newer versions
// accept a position only on their own command line (/seekto); Apple Music writes the album into the
// artist ("Artist — Album"). Spotify tells Windows all there is.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace atrium {

enum class MediaPlayer { Other, Netease, QqMusic, Spotify, AppleMusic };

inline MediaPlayer PlayerOf(std::wstring appId)
{
    if (!appId.empty())
        CharLowerBuffW(appId.data(), DWORD(appId.size()));
    const std::wstring_view id = appId, file = id.substr(id.find_last_of(L"\\/") == std::wstring_view::npos ? 0 : id.find_last_of(L"\\/") + 1);
    if (file.find(L"cloudmusic") != std::wstring_view::npos)
        return MediaPlayer::Netease;
    if (file == L"qqmusic" || file == L"qqmusic.exe" || file == L"tencent.qqmusic" || file.starts_with(L"tencent.qqmusic_"))
        return MediaPlayer::QqMusic;
    if (file == L"spotify.exe" || file == L"spotify" || file.starts_with(L"spotifyab.spotifymusic_"))
        return MediaPlayer::Spotify;
    if (file == L"applemusic.exe" || file.starts_with(L"appleinc.applemusicwin"))
        return MediaPlayer::AppleMusic;
    return MediaPlayer::Other;
}

inline bool PlayerLikes(MediaPlayer p) { return p == MediaPlayer::Netease; }
inline bool PlayerShowsLyrics(MediaPlayer p) { return p == MediaPlayer::Netease || p == MediaPlayer::QqMusic; }

// Apple Music's "Artist — Album" in the artist: the artist alone, and the album when it has none.
inline void SplitAppleArtist(std::wstring& artist, std::wstring& album)
{
    constexpr std::wstring_view kDash = L" — ";
    const size_t at = artist.find(kDash);
    if (at == std::wstring::npos || at == 0 || at + kDash.size() >= artist.size())
        return;
    const std::wstring record = artist.substr(at + kDash.size());
    if (!album.empty() && album != record)
        return; // an artist with a dash in the name
    album = record;
    artist.resize(at);
}

// The player's shortcut: like (or unlike) the track playing.
bool LikeTrack(MediaPlayer p);
// The player's shortcut: its desktop lyrics on or off.
bool ToggleLyrics(MediaPlayer p);
// QQ Music's own command (QQMusic.exe /seekto <s>), passed to the running player; false if none runs.
bool QqMusicSeek(int64_t positionUs);

} // namespace atrium
