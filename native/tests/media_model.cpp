#include "shell/media_players.h"
#include "shell/media_timeline.h"
#include "shell/netease_tracks.h"
#include "shell/shell_icon.h"

#include <roapi.h>

#include <iostream>
#include <limits>

using namespace atrium;

namespace {

int g_failures = 0;

void Require(bool condition, const char* message)
{
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void CheckPlayers()
{
    Require(PlayerOf(L"cloudmusic.exe") == MediaPlayer::Netease, "NetEase Cloud Music by its exe");
    Require(PlayerOf(L"QQMusic.exe") == MediaPlayer::QqMusic && PlayerOf(L"Tencent.QQMusic") == MediaPlayer::QqMusic, "QQ Music, old and new names");
    Require(PlayerOf(L"Tencent.QQ") == MediaPlayer::Other && PlayerOf(L"QQMusic.exe.bak") == MediaPlayer::Other, "not QQ Music");
    Require(PlayerOf(L"Spotify.exe") == MediaPlayer::Spotify && PlayerOf(L"C:\\Users\\u\\AppData\\Roaming\\Spotify\\Spotify.exe") == MediaPlayer::Spotify &&
                PlayerOf(L"SPOTIFY.EXE") == MediaPlayer::Spotify && PlayerOf(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify") == MediaPlayer::Spotify,
            "Spotify, desktop path and Store AUMID");
    Require(PlayerOf(L"SpotifyAB.SpotifyMusicFake") == MediaPlayer::Other && PlayerOf(L"spot.exe") == MediaPlayer::Other, "not Spotify");
    Require(PlayerOf(L"AppleInc.AppleMusicWin_nzyj5cx40ttqa!App") == MediaPlayer::AppleMusic, "Apple Music");
    Require(PlayerOf(L"msedge.exe") == MediaPlayer::Other, "a browser is another player");
    std::wstring artist = L"\u738B\u83F2 \u2014 \u53EA\u7231\u964C\u751F\u4EBA", album;
    SplitAppleArtist(artist, album);
    Require(artist == L"\u738B\u83F2" && album == L"\u53EA\u7231\u964C\u751F\u4EBA", "Apple Music's artist and album apart");
    std::wstring named = L"Artist \u2014 Alias", other = L"Other Album";
    SplitAppleArtist(named, other);
    Require(named == L"Artist \u2014 Alias", "a dash in a name with another album stays");
}

void CheckEstimate()
{
    TimelineEstimate e;
    MediaTimeline t;
    e.Update(L"a", true, 180'000'000, 0, t);
    Require(!t.Seekable(), "a track under way when first seen has no timeline");
    e.Update(L"b", true, 200'000'000, 1000, t);
    Require(t.Seekable() && t.endUs == 200'000'000, "a track seen to start gets the length");
    Require(t.PositionUs(11'000) == 10'000'000, "its position counts from the change");
    MediaTimeline paused;
    e.Update(L"b", false, 200'000'000, 21'000, paused);
    Require(!paused.playing && paused.PositionUs(90'000) == 20'000'000, "a pause holds the position");
    MediaTimeline resumed;
    e.Update(L"b", true, 200'000'000, 50'000, resumed);
    Require(resumed.PositionUs(55'000) == 25'000'000, "playing again goes on from there");
    MediaTimeline again;
    e.Update(L"b", true, 200'000'000, 300'000, again);
    Require(again.PositionUs(300'000) == 0, "past the end on the same track: played again from 0");
    MediaTimeline unknown;
    e.Update(L"c", true, 0, 400'000, unknown);
    Require(!unknown.Seekable(), "no length: no timeline");

    TimelineEstimate emptyFirst;
    MediaTimeline emptyOut;
    emptyFirst.Update(L"", true, 180'000'000, 0, emptyOut);
    emptyFirst.Update(L"\nSome Artist", true, 180'000'000, 500, emptyOut);
    emptyFirst.Update(L"First Real\nSome Artist", true, 180'000'000, 1000, emptyOut);
    Require(!emptyOut.Seekable(), "initial empty title does not mark first real track as seen from start");
    emptyFirst.Update(L"Second Real\nSome Artist", true, 190'000'000, 2000, emptyOut);
    Require(emptyOut.Seekable() && emptyOut.endUs == 190'000'000, "second real track after empty initial titles gets timeline");
}

void CheckTimeline()
{
    MediaTimeline tl;
    tl.startUs = 10'000'000;
    tl.endUs = 70'000'000;
    tl.anchorPosUs = 20'000'000;
    tl.anchorMs = 1000.0;
    tl.rate = 1.0;
    tl.playing = false;

    Require(tl.Seekable(), "positive range is seekable");
    Require(tl.PositionUs(5000.0) == 20'000'000, "paused timeline stays at anchor");

    tl.playing = true;
    Require(tl.PositionUs(2500.0) == 21'500'000, "1x playback advances linearly");

    tl.rate = 1.5;
    Require(tl.PositionUs(3000.0) == 23'000'000, "1.5x playback scales elapsed time");

    Require(tl.PositionUs(100'000.0) == 70'000'000, "timeline clamps to endUs");
    Require(tl.PositionUs(-100'000.0) == 10'000'000, "timeline clamps to startUs");

    MediaTimeline live;
    live.startUs = 0;
    live.endUs = 0;
    live.anchorPosUs = 5'000'000;
    live.anchorMs = 1000.0;
    live.playing = true;
    Require(!live.Seekable(), "endUs <= startUs is live and not seekable");
    Require(live.PositionUs(2000.0) == 6'000'000, "live timeline still advances");
    Require(live.PositionUs(-10'000.0) == 0, "live timeline clamps at startUs");

    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    MediaTimeline extreme = tl;
    extreme.playing = true;
    extreme.rate = inf;
    Require(extreme.PositionUs(2000.0) == 20'000'000, "+Inf playback rate is ignored safely");
    extreme.rate = qnan;
    Require(extreme.PositionUs(2000.0) == 20'000'000, "NaN playback rate is ignored safely");
    extreme.rate = -1.0;
    Require(extreme.PositionUs(2000.0) == 20'000'000, "non-positive playback rate is ignored safely");
    extreme.rate = 1e300;
    Require(extreme.PositionUs(2000.0) == 70'000'000, "extreme finite playback rate clamps cleanly to endUs");
    extreme.rate = 1.0;
    Require(extreme.PositionUs(qnan) == 20'000'000, "NaN nowMs is ignored safely");
    Require(extreme.PositionUs(inf) == 20'000'000, "+Inf nowMs is ignored safely");
    Require(extreme.PositionUs(1e300) == 70'000'000, "extreme finite nowMs clamps cleanly to endUs");

    MediaTimeline a;
    a.startUs = 0;
    a.endUs = 180'000'000;
    a.anchorPosUs = 0;
    a.anchorMs = 1000.0;
    a.rate = 1.0;
    a.playing = true;
    MediaTimeline b = a;
    b.anchorMs = 1050.0;
    Require(!TimelineChanged(a, b), "small anchorMs clock jitter while playing is ignored");
    b.anchorMs = 2000.0;
    Require(TimelineChanged(a, b), "Repeat-One / seek-to-0 anchorMs jump while playing is detected");
    a.playing = false;
    b.playing = false;
    b.anchorMs = 9000.0;
    Require(!TimelineChanged(a, b), "anchorMs drift while paused does not trigger timeline change");
}

void CheckAppFilter()
{
    Require(MediaAppAllowed(L"Spotify.exe", L"all", {L"Spotify.exe"}), "all mode ignores list");
    Require(MediaAppAllowed(L"Spotify.exe", L"", {L"Spotify.exe"}), "empty mode defaults to all");

    const std::vector<std::wstring> list{
        L"spotify", L"cloudmusic.exe", L"Microsoft.ZuneMusic_", L"SpotifyAB.SpotifyMusic", L"C:\\Apps\\\u00c9diteur\\Player.exe",
    };

    Require(MediaAppAllowed(L"C:\\Users\\a\\AppData\\Spotify.exe", L"allow", list), "stem matches full path exe");
    Require(MediaAppAllowed(L"Spotify.EXE", L"allow", list), "case-insensitive exe matches stem");
    Require(MediaAppAllowed(L"D:\\Music\\CloudMusic.exe", L"allow", list), "exe name matches different install path");
    Require(MediaAppAllowed(L"cloudmusic", L"allow", list), "appId without .exe matches .exe entry");
    Require(MediaAppAllowed(L"microsoft.zunemusic_8wekyb3d8bbwe!App", L"allow", list), "trailing underscore package prefix matches");
    Require(MediaAppAllowed(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", L"allow", list), "package name prefix matches before underscore");
    Require(MediaAppAllowed(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", L"allow", {L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!"}),
            "Store package family prefix with ! matches");
    Require(!MediaAppAllowed(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", L"block", {L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!"}),
            "block mode rejects Store package family prefix");
    Require(MediaAppAllowed(L"c:\\apps\\\u00e9diteur\\player.exe", L"allow", list), "non-ASCII path matches case-insensitively");

    Require(!MediaAppAllowed(L"D:\\Other\\Player.exe", L"allow", list), "full path entry does not match different path");
    Require(!MediaAppAllowed(L"spot.exe", L"allow", list), "partial name does not false-match");
    Require(!MediaAppAllowed(L"Microsoft.Zune_8wekyb3d8bbwe!App", L"allow", list), "shorter package name does not false-match");
    Require(!MediaAppAllowed(L"", L"allow", list), "empty appId is rejected in allow mode");

    Require(!MediaAppAllowed(L"Spotify.exe", L"block", list), "block mode rejects listed app");
    Require(MediaAppAllowed(L"vlc.exe", L"block", list), "block mode allows unlisted app");
    Require(MediaAppAllowed(L"", L"block", list), "empty appId is not blocked");
}

void CheckPickSession()
{
    Require(PickMediaSession({}, true, L"all", {}, L"") == -1, "empty sessions returns -1");

    const std::vector<MediaSessionSummary> sessions{
        {L"closed", L"spotify.exe", L"Song A", MediaPlaybackState::Closed, true, 900.0},
        {L"empty_paused", L"spotify.exe", L"", MediaPlaybackState::Paused, false, 800.0},
        {L"paused_old", L"vlc.exe", L"Movie", MediaPlaybackState::Paused, false, 100.0},
        {L"paused_new", L"foobar2000.exe", L"Track", MediaPlaybackState::Paused, false, 500.0},
        {L"playing_recent", L"chrome.exe", L"Video", MediaPlaybackState::Playing, false, 700.0},
        {L"playing_current", L"spotify.exe", L"Song B", MediaPlaybackState::Playing, true, 400.0},
    };

    Require(PickMediaSession(sessions, true, L"all", {}, L"") == 5, "Playing + isSystemCurrent wins");
    Require(PickMediaSession(sessions, true, L"block", {L"spotify"}, L"") == 4, "blocked current falls back to recent Playing");
    Require(PickMediaSession(sessions, true, L"allow", {L"vlc", L"foobar2000"}, L"") == 3, "no Playing falls back to most recent Paused");
    Require(PickMediaSession(sessions, false, L"allow", {L"vlc", L"foobar2000"}, L"") == -1, "showPaused=false skips Paused");
    Require(PickMediaSession(sessions, true, L"all", {}, L"paused_old") == 2, "pinnedId selects eligible session");
    Require(PickMediaSession(sessions, false, L"all", {}, L"paused_old") == 5, "ineligible pinnedId falls back to normal priority");
    Require(PickMediaSession(sessions, true, L"all", {}, L"empty_paused") == 5, "empty-title paused session is ignored");

    const std::vector<MediaSessionSummary> emptyTitlePlaying{
        {L"radio", L"foobar2000.exe", L"", MediaPlaybackState::Playing, false, 10.0},
    };
    Require(PickMediaSession(emptyTitlePlaying, true, L"all", {}, L"") == 0, "Playing session without title is still eligible");
}

void CheckNeteaseQueueCache()
{
    const HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
    Require(SUCCEEDED(roHr) || roHr == RPC_E_CHANGED_MODE, "RoInitialize succeeds for NeteaseQueueCache");

    NeteaseQueueCache cache;
    const std::wstring validJson = LR"({"list":[{"track":{"name":"Blue Bird","duration":215000,"artists":[{"name":"Ikimonogakari"}]}},)"
                                   LR"({"track":{"name":"BadZero","duration":0}},{"track":{"name":"BadHuge","duration":90000000}},)"
                                   LR"({"track":{"name":"BadNeg","duration":-500}}]})";
    Require(cache.NeedsRead(100, 200), "fresh cache needs read");
    Require(cache.Update(validJson, 100, 200), "valid queue JSON parses");
    Require(!cache.NeedsRead(100, 200), "matching stamp/size skips re-read");
    Require(cache.FindLengthUs(L"Blue Bird", L"Ikimonogakari") == 215'000'000, "valid track duration parsed");
    Require(cache.FindLengthUs(L"BadZero", L"") == 0, "zero duration ignored");
    Require(cache.FindLengthUs(L"BadHuge", L"") == 0, "out-of-range duration ignored");
    Require(cache.FindLengthUs(L"BadNeg", L"") == 0, "negative duration ignored");

    Require(!cache.Update(L"", 101, 0), "empty text fails update");
    Require(!cache.Update(LR"({"list":)", 101, 9), "truncated JSON fails update");
    Require(!cache.Update(LR"({"other":1})", 101, 11), "JSON missing list array fails update");
    Require(!cache.NeedsRead(100, 200), "transient failure preserves previous file stamp/size");
    Require(cache.FindLengthUs(L"Blue Bird", L"Ikimonogakari") == 215'000'000, "transient failure preserves previous tracks");

    if (SUCCEEDED(roHr))
        RoUninitialize();
}

void CheckSparseGsmtcTimeline()
{
    GsmtcTimelineTracker tr;
    MediaTimeline tl = tr.Update(0, 180'000'000, 10'000'000, 100'000'000, 120'000'000, 5000.0, 1.0, true);
    Require(tl.PositionUs(8000.0) == 15'000'000, "sparse GSMTC advances linearly from LastUpdatedTime");
    tl = tr.Update(0, 180'000'000, 10'000'000, 100'000'000, 150'000'000, 8000.0, 1.5, true);
    Require(tl.PositionUs(10'000.0) == 18'000'000, "podcast rate 1.5x advances from rebased position");
    tl = tr.Update(0, 180'000'000, 10'000'000, 100'000'000, 170'000'000, 10'000.0, 1.5, false);
    Require(!tl.playing && tl.PositionUs(50'000.0) == 18'000'000, "pause freezes at extrapolated position");
    tl = tr.Update(0, 180'000'000, 10'000'000, 100'000'000, 570'000'000, 50'000.0, 1.0, true);
    Require(tl.PositionUs(55'000.0) == 23'000'000, "resume continues from frozen position without pause time");
    tl = tr.Update(0, 180'000'000, 0, 600'000'000, 599'000'000, 60'000.0, 1.0, true);
    Require(tl.PositionUs(60'000.0) == 0 && tl.PositionUs(61'000.0) == 1'000'000, "raw change resets and negative age clamps to 0");
    tr.Reset();
    tl = tr.Update(0, 120'000'000, 5'000'000, 0, 100'000'000, 1000.0, 1.0, true);
    tl = tr.Update(0, 120'000'000, 5'000'000, 0, 105'000'000, 1500.0, 1.0, true);
    Require(tl.PositionUs(2000.0) == 6'000'000, "updated==0 keeps single anchor across refreshes");
    tl = tr.Update(0, 0, 0, 0, 110'000'000, 3000.0, 1.0, true);
    Require(!tl.Seekable(), "endUs==0 is not seekable");
}

void CheckCropToSquare()
{
    for (const auto& [w, h] : {std::pair{8, 8}, std::pair{16, 8}, std::pair{8, 16}}) {
        IconImage img{w, h, std::vector<uint8_t>(size_t(w) * size_t(h) * 4, 255)};
        const int side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                img.bgra[(size_t(y) * size_t(w) + size_t(x)) * 4 + 2] = (x >= x0 && x < x0 + side && y >= y0 && y < y0 + side) ? 220 : 10;
        const IconImage out = CropToSquare(img, 4);
        Require(out.width == 4 && out.height == 4 && out.bgra[2] == 220 && out.bgra[3] == 255, "CropToSquare crops centred square");
    }
}

} // namespace

int main()
{
    CheckTimeline();
    CheckEstimate();
    CheckPlayers();
    CheckAppFilter();
    CheckPickSession();
    CheckNeteaseQueueCache();
    CheckSparseGsmtcTimeline();
    CheckCropToSquare();
    if (g_failures > 0)
        return 1;
    std::cout << "media model: all checks passed\n";
    return 0;
}
