#include "shell/media_session.h"

#include "clock.h"
#include "log.h"
#include "posted.h"
#include "shell/audio_meter.h"
#include "shell/media_app.h"
#include "shell/media_players.h"
#include "shell/netease_tracks.h"
#include "shell/shell_icon.h"
#include "win32_path.h"

#include <windows.h>
#include <appmodel.h>
#include <roapi.h>
#include <shcore.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <asyncinfo.h>
#include <windows.foundation.h>
#include <windows.media.control.h>
#include <windows.storage.streams.h>
#include <winstring.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atrium {

namespace {

namespace Wmc = ABI::Windows::Media::Control;
namespace Wf = ABI::Windows::Foundation;
namespace Wss = ABI::Windows::Storage::Streams;

using ManagerOp = Wf::IAsyncOperation<Wmc::GlobalSystemMediaTransportControlsSessionManager*>;
using ManagerHandler = Wf::IAsyncOperationCompletedHandler<Wmc::GlobalSystemMediaTransportControlsSessionManager*>;
using MediaPropsOp = Wf::IAsyncOperation<Wmc::GlobalSystemMediaTransportControlsSessionMediaProperties*>;
using MediaPropsHandler = Wf::IAsyncOperationCompletedHandler<Wmc::GlobalSystemMediaTransportControlsSessionMediaProperties*>;
using StreamOp = Wf::IAsyncOperation<Wss::IRandomAccessStreamWithContentType*>;
using StreamHandler = Wf::IAsyncOperationCompletedHandler<Wss::IRandomAccessStreamWithContentType*>;
using BoolOp = Wf::IAsyncOperation<bool>;
using BoolHandler = Wf::IAsyncOperationCompletedHandler<bool>;

using SessionsChangedHandler = Wf::ITypedEventHandler<Wmc::GlobalSystemMediaTransportControlsSessionManager*, Wmc::SessionsChangedEventArgs*>;
using CurrentChangedHandler = Wf::ITypedEventHandler<Wmc::GlobalSystemMediaTransportControlsSessionManager*, Wmc::CurrentSessionChangedEventArgs*>;
using PlaybackChangedHandler = Wf::ITypedEventHandler<Wmc::GlobalSystemMediaTransportControlsSession*, Wmc::PlaybackInfoChangedEventArgs*>;
using MediaChangedHandler = Wf::ITypedEventHandler<Wmc::GlobalSystemMediaTransportControlsSession*, Wmc::MediaPropertiesChangedEventArgs*>;
using TimelineChangedHandler = Wf::ITypedEventHandler<Wmc::GlobalSystemMediaTransportControlsSession*, Wmc::TimelinePropertiesChangedEventArgs*>;

constexpr DWORD kAsyncTimeoutMs = 2000;
constexpr DWORD kSampleIntervalMs = 33;
constexpr DWORD kDebounceMs = 30;
constexpr double kRefreshMs = 500; // the sessions read again while only the level is sampled
constexpr size_t kMaxArtworkBytes = 16 * 1024 * 1024;
constexpr int kArtworkSizePx = 128;

struct ScopedHandle {
    HANDLE handle = nullptr;
    explicit ScopedHandle(HANDLE h) : handle(h) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle()
    {
        if (handle)
            CloseHandle(handle);
    }
};

struct RoInitScope {
    bool ok = SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED));
    RoInitScope() = default;
    RoInitScope(const RoInitScope&) = delete;
    RoInitScope& operator=(const RoInitScope&) = delete;
    ~RoInitScope()
    {
        if (ok)
            RoUninitialize();
    }
};

// Each wait has its own event, owned by the handler: a completion that comes after its wait gave up
// signals nothing anyone else waits on, and never a handle closed since.
template<typename HandlerT, typename OpT> struct AsyncWaitHandler : winrt::implements<AsyncWaitHandler<HandlerT, OpT>, HandlerT, IAgileObject> {
    winrt::handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HRESULT STDMETHODCALLTYPE Invoke(OpT* /*asyncInfo*/, Wf::AsyncStatus /*status*/) noexcept override
    {
        SetEvent(done.get());
        return S_OK;
    }
};

template<typename HandlerT, typename SenderT, typename ArgsT> struct WakeEventHandler
    : winrt::implements<WakeEventHandler<HandlerT, SenderT, ArgsT>, HandlerT, IAgileObject> {
    HANDLE wake = nullptr;
    std::atomic<bool>* flag = nullptr;
    explicit WakeEventHandler(HANDLE w, std::atomic<bool>* f = nullptr) : wake(w), flag(f) {}
    HRESULT STDMETHODCALLTYPE Invoke(SenderT* /*sender*/, ArgsT* /*args*/) noexcept override
    {
        if (flag)
            flag->store(true, std::memory_order_release);
        SetEvent(wake);
        return S_OK;
    }
};

template<typename HandlerT, typename OpT> bool AwaitAsync(OpT* op, HANDLE stopEvent, DWORD timeoutMs)
{
    if (!op)
        return false;
    auto handler = winrt::make_self<AsyncWaitHandler<HandlerT, OpT>>();
    if (!handler->done || FAILED(op->put_Completed(handler.get())))
        return false;
    HANDLE handles[2] = {stopEvent, handler->done.get()};
    if (WaitForMultipleObjects(2, handles, FALSE, timeoutMs) == WAIT_OBJECT_0 + 1)
        return true;
    // Given up (stopping, or the session never answered): the operation is not wanted any more.
    winrt::com_ptr<Wf::IAsyncInfo> info;
    if (SUCCEEDED(op->QueryInterface(IID_PPV_ARGS(info.put()))))
        info->Cancel();
    return false;
}

std::wstring HStringToWide(HSTRING hs)
{
    UINT32 len = 0;
    const wchar_t* raw = hs ? WindowsGetStringRawBuffer(hs, &len) : nullptr;
    return raw ? std::wstring(raw, len) : std::wstring{};
}

uint64_t HashBytesFnv1a(const std::vector<uint8_t>& data)
{
    uint64_t h = 14695981039346656037ULL;
    for (uint8_t b : data)
        h = (h ^ b) * 1099511628211ULL;
    return h;
}

MediaPlaybackState ToPlaybackState(Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus s)
{
    using enum MediaPlaybackState;
    switch (s) {
    case Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Opened: return Opened;
    case Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Changing: return Changing;
    case Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Stopped: return Stopped;
    case Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing: return Playing;
    case Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Paused: return Paused;
    default: return Closed;
    }
}

int64_t CurrentFileTime100Ns()
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

bool ReadStreamBytes(IStream* stream, std::vector<uint8_t>& bytes)
{
    bytes.clear();
    uint8_t chunk[16384]{};
    HRESULT hr = S_OK;
    for (ULONG read = 0; SUCCEEDED(hr = stream->Read(chunk, sizeof(chunk), &read)) && read > 0;) {
        if (bytes.size() + read > kMaxArtworkBytes)
            return false;
        bytes.insert(bytes.end(), chunk, chunk + read);
    }
    return SUCCEEDED(hr) && !bytes.empty();
}

bool DecodeWicSquare(IWICImagingFactory* wic, std::vector<uint8_t>& bytes, IconImage& out)
{
    if (!wic || bytes.empty())
        return false;
    winrt::com_ptr<IWICStream> wicStream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateStream(wicStream.put())) || !wicStream || FAILED(wicStream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(wicStream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) || !decoder ||
        FAILED(decoder->GetFrame(0, frame.put())) || !frame || FAILED(wic->CreateFormatConverter(converter.put())) || !converter ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || w > 8192 || h > 8192)
        return false;
    IconImage raw;
    raw.width = static_cast<int>(w);
    raw.height = static_cast<int>(h);
    raw.bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(raw.bgra.size()), raw.bgra.data())))
        return false;
    out = CropToSquare(raw, kArtworkSizePx);
    return !out.Empty();
}

bool SnapshotChanged(const MediaSnapshot& a, const MediaSnapshot& b)
{
    return TimelineChanged(a.timeline, b.timeline) || a.active != b.active || a.sessionId != b.sessionId || a.appId != b.appId || a.appName != b.appName ||
           a.title != b.title || a.artist != b.artist || a.album != b.album || a.state != b.state || a.canPlayPause != b.canPlayPause ||
           a.canNext != b.canNext || a.canPrevious != b.canPrevious || a.canSeek != b.canSeek || a.canLike != b.canLike || a.liked != b.liked ||
           a.canLyrics != b.canLyrics || a.artworkVersion != b.artworkVersion || a.sessionIndex != b.sessionIndex || a.sessionCount != b.sessionCount;
}

bool IsRpcError(HRESULT hr)
{
    if (SUCCEEDED(hr))
        return false;
    return HRESULT_FACILITY(hr) == FACILITY_RPC || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE) || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_TOO_BUSY);
}

struct TrackedSession {
    winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSession> session;
    winrt::com_ptr<IUnknown> identity;
    std::wstring id, appId, title, artist, album;
    EventRegistrationToken playbackToken{}, mediaToken{}, timelineToken{};
    std::atomic<bool> mediaDirty{true}, timelineDirty{false};
    double lastPlayingMs = 0.0;
    std::shared_ptr<const IconImage> artwork;
    uint64_t artworkHash = 0;
    size_t artworkBytes = 0;
    GsmtcTimelineTracker gsmtc;
    NeteaseTimeline netease; // NetEase Cloud Music's, which it does not report
};

struct FilterConfig {
    bool showPaused = true;
    std::wstring appMode = L"all";
    std::vector<std::wstring> apps;
};

struct QueuedAction {
    enum class Kind { Command, Cycle };
    Kind kind = Kind::Command;
    MediaCommand command = MediaCommand::PlayPause;
    int64_t positionUs = 0;
    int delta = 0;
};

} // namespace

struct MediaSessionWatcher::Impl {
    Impl(HWND n, UINT msg, std::function<void(float, bool)> cb) : notify_(n), stateMsg_(msg), onPeak_(std::move(cb))
    {
        thread_ = std::thread([this] { Run(); });
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl()
    {
        stop_.store(true, std::memory_order_release);
        SetEvent(stopEvent_.handle);
        SetEvent(wake_.handle);
        if (thread_.joinable())
            thread_.join();
    }

    void Enqueue(QueuedAction action)
    {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(action);
        }
        SetEvent(wake_.handle);
    }
    void SetFilter(bool showPaused, std::wstring appMode, std::vector<std::wstring> apps)
    {
        {
            std::lock_guard lock(mutex_);
            filter_ = {showPaused, std::move(appMode), std::move(apps)};
            filterDirty_ = true;
        }
        SetEvent(wake_.handle);
    }
    void SetSamplingActive(bool barVisible)
    {
        samplingActive_.store(barVisible, std::memory_order_release);
        SetEvent(wake_.handle);
    }
    void SetCardVisible(bool visible)
    {
        const bool was = cardVisible_.exchange(visible, std::memory_order_acq_rel);
        if (visible && !was)
            SetEvent(wake_.handle);
    }

private:
    HWND notify_;
    UINT stateMsg_;
    std::function<void(float, bool)> onPeak_;
    std::atomic<bool> stop_{false}, samplingActive_{true}, cardVisible_{false};
    ScopedHandle stopEvent_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ScopedHandle wake_{CreateEventW(nullptr, FALSE, TRUE, nullptr)};
    std::mutex mutex_;
    FilterConfig filter_;
    bool filterDirty_ = true;
    std::deque<QueuedAction> queue_;
    AudioPeakMeter meter_;
    winrt::com_ptr<IWICImagingFactory> wic_;
    winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSessionManager> manager_;
    EventRegistrationToken sessionsToken_{}, currentToken_{};
    int managerFailStreak_ = 0;
    std::vector<std::unique_ptr<TrackedSession>> sessions_;
    std::unordered_map<std::wstring, std::wstring> appNameCache_;
    uint64_t nextSessionUid_ = 1, artworkVersion_ = 0;
    std::wstring pinnedId_;
    std::shared_ptr<const IconImage> lastPostedArt_;
    MediaSnapshot lastPosted_;
    bool hasPosted_ = false, lastPeakPlaying_ = false;
    std::thread thread_;

    void UnsubscribeSession(TrackedSession& ts)
    {
        if (!ts.session)
            return;
        if (ts.playbackToken.value)
            ts.session->remove_PlaybackInfoChanged(ts.playbackToken);
        if (ts.mediaToken.value)
            ts.session->remove_MediaPropertiesChanged(ts.mediaToken);
        if (ts.timelineToken.value)
            ts.session->remove_TimelinePropertiesChanged(ts.timelineToken);
    }

    void ResetManager()
    {
        for (auto& ts : sessions_)
            UnsubscribeSession(*ts);
        sessions_.clear();
        if (manager_) {
            if (sessionsToken_.value) {
                manager_->remove_SessionsChanged(sessionsToken_);
                sessionsToken_ = {};
            }
            if (currentToken_.value) {
                manager_->remove_CurrentSessionChanged(currentToken_);
                currentToken_ = {};
            }
            manager_ = nullptr;
        }
        managerFailStreak_ = 0;
    }

    void RunBoolCommand(Wmc::IGlobalSystemMediaTransportControlsSession* session,
                        HRESULT (STDMETHODCALLTYPE Wmc::IGlobalSystemMediaTransportControlsSession::*fn)(BoolOp**))
    {
        winrt::com_ptr<BoolOp> op;
        if (session && SUCCEEDED((session->*fn)(op.put())) && op)
            AwaitAsync<BoolHandler>(op.get(), stopEvent_.handle, kAsyncTimeoutMs);
    }

    bool EnsureManager()
    {
        if (manager_)
            return true;
        HSTRING_HEADER header{};
        HSTRING classId = nullptr;
        const wchar_t* name = RuntimeClass_Windows_Media_Control_GlobalSystemMediaTransportControlsSessionManager;
        if (FAILED(WindowsCreateStringReference(name, static_cast<UINT32>(wcslen(name)), &header, &classId)))
            return false;
        winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSessionManagerStatics> statics;
        winrt::com_ptr<ManagerOp> op;
        if (FAILED(RoGetActivationFactory(classId, IID_PPV_ARGS(statics.put()))) || !statics || FAILED(statics->RequestAsync(op.put())) || !op ||
            !AwaitAsync<ManagerHandler>(op.get(), stopEvent_.handle, kAsyncTimeoutMs) || stop_.load(std::memory_order_acquire) ||
            FAILED(op->GetResults(manager_.put())) || !manager_)
            return false;

        auto sessHandler =
            winrt::make<WakeEventHandler<SessionsChangedHandler, Wmc::IGlobalSystemMediaTransportControlsSessionManager, Wmc::ISessionsChangedEventArgs>>(
                wake_.handle);
        auto curHandler =
            winrt::make<WakeEventHandler<CurrentChangedHandler, Wmc::IGlobalSystemMediaTransportControlsSessionManager, Wmc::ICurrentSessionChangedEventArgs>>(
                wake_.handle);
        manager_->add_SessionsChanged(sessHandler.get(), &sessionsToken_);
        manager_->add_CurrentSessionChanged(curHandler.get(), &currentToken_);
        managerFailStreak_ = 0;
        return true;
    }

    bool SyncSessionList()
    {
        if (!manager_)
            return false;
        winrt::com_ptr<__FIVectorView_1_Windows__CMedia__CControl__CGlobalSystemMediaTransportControlsSession> list;
        UINT32 count = 0;
        const HRESULT hr = manager_->GetSessions(list.put());
        const HRESULT sizeHr = (SUCCEEDED(hr) && list) ? list->get_Size(&count) : hr;
        if (FAILED(hr) || !list || FAILED(sizeHr)) {
            const HRESULT err = FAILED(hr) ? hr : sizeHr;
            if (IsRpcError(err) || ++managerFailStreak_ >= 3) {
                Log(L"media: resetting GSMTC manager (hr=%08x)", static_cast<unsigned>(err));
                ResetManager();
            }
            return false;
        }
        managerFailStreak_ = 0;

        std::vector<std::unique_ptr<TrackedSession>> next;
        next.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSession> s;
            if (FAILED(list->GetAt(i, s.put())) || !s)
                continue;
            winrt::com_ptr<IUnknown> id;
            s->QueryInterface(IID_PPV_ARGS(id.put()));

            auto existing = std::ranges::find_if(sessions_, [&](const auto& p) { return p->identity.get() == id.get(); });
            if (existing != sessions_.end()) {
                next.push_back(std::move(*existing));
                sessions_.erase(existing);
                continue;
            }

            auto ts = std::make_unique<TrackedSession>();
            ts->session = s;
            ts->identity = id;
            HSTRING appHs = nullptr;
            if (SUCCEEDED(s->get_SourceAppUserModelId(&appHs)) && appHs) {
                ts->appId = HStringToWide(appHs);
                WindowsDeleteString(appHs);
            }
            ts->id = ts->appId + L"#" + std::to_wstring(nextSessionUid_++);

            auto pbHandler =
                winrt::make<WakeEventHandler<PlaybackChangedHandler, Wmc::IGlobalSystemMediaTransportControlsSession, Wmc::IPlaybackInfoChangedEventArgs>>(
                    wake_.handle);
            auto mdHandler =
                winrt::make<WakeEventHandler<MediaChangedHandler, Wmc::IGlobalSystemMediaTransportControlsSession, Wmc::IMediaPropertiesChangedEventArgs>>(
                    wake_.handle, &ts->mediaDirty);
            auto tlHandler = winrt::make<
                WakeEventHandler<TimelineChangedHandler, Wmc::IGlobalSystemMediaTransportControlsSession, Wmc::ITimelinePropertiesChangedEventArgs>>(
                wake_.handle, &ts->timelineDirty);
            s->add_PlaybackInfoChanged(pbHandler.get(), &ts->playbackToken);
            s->add_MediaPropertiesChanged(mdHandler.get(), &ts->mediaToken);
            s->add_TimelinePropertiesChanged(tlHandler.get(), &ts->timelineToken);
            next.push_back(std::move(ts));
        }

        for (auto& removed : sessions_)
            UnsubscribeSession(*removed);
        sessions_ = std::move(next);
        return true;
    }

    void ReadMediaProperties(TrackedSession& ts)
    {
        winrt::com_ptr<MediaPropsOp> op;
        winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSessionMediaProperties> props;
        if (FAILED(ts.session->TryGetMediaPropertiesAsync(op.put())) || !op || !AwaitAsync<MediaPropsHandler>(op.get(), stopEvent_.handle, kAsyncTimeoutMs) ||
            stop_.load(std::memory_order_acquire) || FAILED(op->GetResults(props.put())) || !props)
            return;
        ts.mediaDirty.store(false, std::memory_order_release);

        auto readStr = [&](HRESULT (STDMETHODCALLTYPE Wmc::IGlobalSystemMediaTransportControlsSessionMediaProperties::*fn)(HSTRING*)) {
            HSTRING hs = nullptr;
            std::wstring out;
            if (SUCCEEDED((props.get()->*fn)(&hs)) && hs) {
                out = HStringToWide(hs);
                WindowsDeleteString(hs);
            }
            return out;
        };
        ts.title = readStr(&Wmc::IGlobalSystemMediaTransportControlsSessionMediaProperties::get_Title);
        ts.artist = readStr(&Wmc::IGlobalSystemMediaTransportControlsSessionMediaProperties::get_Artist);
        ts.album = readStr(&Wmc::IGlobalSystemMediaTransportControlsSessionMediaProperties::get_AlbumTitle);

        auto clearArt = [&] {
            ts.artwork.reset();
            ts.artworkHash = 0;
            ts.artworkBytes = 0;
        };
        winrt::com_ptr<Wss::IRandomAccessStreamReference> thumbRef;
        winrt::com_ptr<StreamOp> streamOp;
        winrt::com_ptr<Wss::IRandomAccessStreamWithContentType> stream;
        winrt::com_ptr<IStream> comStream;
        if (FAILED(props->get_Thumbnail(thumbRef.put())) || !thumbRef) {
            clearArt();
            return;
        }
        if (FAILED(thumbRef->OpenReadAsync(streamOp.put())) || !streamOp || !AwaitAsync<StreamHandler>(streamOp.get(), stopEvent_.handle, kAsyncTimeoutMs) ||
            stop_.load(std::memory_order_acquire) || FAILED(streamOp->GetResults(stream.put())) || !stream ||
            FAILED(CreateStreamOverRandomAccessStream(stream.get(), IID_PPV_ARGS(comStream.put()))) || !comStream) {
            clearArt();
            return;
        }

        std::vector<uint8_t> bytes;
        if (!ReadStreamBytes(comStream.get(), bytes)) {
            clearArt();
            return;
        }
        const uint64_t hash = HashBytesFnv1a(bytes);
        if (ts.artwork && hash == ts.artworkHash && bytes.size() == ts.artworkBytes)
            return;
        if (!wic_)
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.put()));
        IconImage img;
        if (DecodeWicSquare(wic_.get(), bytes, img)) {
            ts.artwork = std::make_shared<const IconImage>(std::move(img));
            ts.artworkHash = hash;
            ts.artworkBytes = bytes.size();
        } else {
            clearArt();
        }
    }

    bool ReadSessionSnapshot(TrackedSession& ts, bool isSystemCurrent, MediaSnapshot& outSnap, MediaSessionSummary& outSummary)
    {
        const bool timelineUpdated = ts.timelineDirty.exchange(false, std::memory_order_acq_rel);
        if (ts.mediaDirty.load(std::memory_order_acquire))
            ReadMediaProperties(ts);

        outSnap.sessionId = ts.id;
        outSnap.appId = ts.appId;
        outSnap.appName = ResolveAppName(ts.appId, appNameCache_);
        outSnap.title = ts.title;
        outSnap.artist = ts.artist;
        outSnap.album = ts.album;
        outSnap.artwork = ts.artwork;

        double rate = 1.0;
        winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSessionPlaybackInfo> pb;
        if (SUCCEEDED(ts.session->GetPlaybackInfo(pb.put())) && pb) {
            Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus rawStatus{};
            if (SUCCEEDED(pb->get_PlaybackStatus(&rawStatus)))
                outSnap.state = ToPlaybackState(rawStatus);
            winrt::com_ptr<__FIReference_1_double> rateRef;
            double r = 1.0;
            if (SUCCEEDED(pb->get_PlaybackRate(rateRef.put())) && rateRef && SUCCEEDED(rateRef->get_Value(&r)) && std::isfinite(r) && r > 0.0 && r <= 16.0)
                rate = r;
            winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSessionPlaybackControls> ctrl;
            if (SUCCEEDED(pb->get_Controls(ctrl.put())) && ctrl) {
                boolean play = FALSE, pause = FALSE, toggle = FALSE, next = FALSE, prev = FALSE, seek = FALSE;
                ctrl->get_IsPlayEnabled(&play);
                ctrl->get_IsPauseEnabled(&pause);
                ctrl->get_IsPlayPauseToggleEnabled(&toggle);
                ctrl->get_IsNextEnabled(&next);
                ctrl->get_IsPreviousEnabled(&prev);
                ctrl->get_IsPlaybackPositionEnabled(&seek);
                outSnap.canPlayPause = (toggle != FALSE) || (outSnap.state == MediaPlaybackState::Playing ? pause != FALSE : play != FALSE);
                outSnap.canNext = next != FALSE;
                outSnap.canPrevious = prev != FALSE;
                outSnap.canSeek = seek != FALSE;
            }
        }

        const double nowMs = ClockMs();
        const bool playing = outSnap.state == MediaPlaybackState::Playing;
        if (playing)
            ts.lastPlayingMs = nowMs;

        winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSessionTimelineProperties> tl;
        if (SUCCEEDED(ts.session->GetTimelineProperties(tl.put())) && tl) {
            Wf::TimeSpan start{}, end{}, pos{};
            Wf::DateTime updated{};
            tl->get_StartTime(&start);
            tl->get_EndTime(&end);
            tl->get_Position(&pos);
            tl->get_LastUpdatedTime(&updated);
            outSnap.timeline =
                ts.gsmtc.Update(start.Duration / 10, end.Duration / 10, pos.Duration / 10, updated.UniversalTime, CurrentFileTime100Ns(), nowMs, rate, playing);
        } else {
            ts.gsmtc.Reset();
            outSnap.timeline.rate = rate;
            outSnap.timeline.playing = playing;
        }

        // NetEase Cloud Music reports no timeline: its own (netease_tracks.h).
        const MediaPlayer player = PlayerOf(ts.appId);
        if (!outSnap.timeline.Seekable() && player == MediaPlayer::Netease)
            ts.netease.Fill(ts.title, ts.artist, playing, nowMs, outSnap.timeline);
        // What the players do beyond Windows' controls (media_players.h).
        outSnap.canLike = PlayerLikes(player);
        if (player == MediaPlayer::Netease)
            outSnap.liked = ts.netease.Liked(ts.title, ts.artist);
        outSnap.canLyrics = PlayerShowsLyrics(player);
        if (player == MediaPlayer::QqMusic)
            outSnap.canSeek = outSnap.canSeek || outSnap.timeline.Seekable(); // /seekto, where Windows' seek is refused
        if (player == MediaPlayer::AppleMusic)
            SplitAppleArtist(outSnap.artist, outSnap.album);
        if (outSnap.artist.empty())
            outSnap.artist = outSnap.album;
        outSummary = {ts.id, ts.appId, ts.title, outSnap.state, isSystemCurrent, ts.lastPlayingMs};
        return timelineUpdated;
    }

    void ExecuteAction(const QueuedAction& action, const std::vector<int>& eligible, int activeIdx)
    {
        auto cycleBy = [&](int delta) {
            if (eligible.size() <= 1 || activeIdx < 0)
                return;
            const auto posIt = std::ranges::find(eligible, activeIdx);
            const int cur = posIt == eligible.end() ? 0 : static_cast<int>(posIt - eligible.begin());
            const int n = static_cast<int>(eligible.size());
            pinnedId_ = sessions_[eligible[((cur + delta) % n + n) % n]]->id;
        };
        if (action.kind == QueuedAction::Kind::Cycle) {
            cycleBy(action.delta);
            return;
        }
        if (activeIdx < 0 || activeIdx >= static_cast<int>(sessions_.size()))
            return;
        TrackedSession& active = *sessions_[activeIdx];
        switch (action.command) {
        case MediaCommand::PlayPause: {
            auto fn = &Wmc::IGlobalSystemMediaTransportControlsSession::TryTogglePlayPauseAsync;
            winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSessionPlaybackInfo> pb;
            winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSessionPlaybackControls> ctrl;
            boolean toggle = TRUE;
            if (SUCCEEDED(active.session->GetPlaybackInfo(pb.put())) && pb && SUCCEEDED(pb->get_Controls(ctrl.put())) && ctrl &&
                SUCCEEDED(ctrl->get_IsPlayPauseToggleEnabled(&toggle)) && toggle == FALSE) {
                Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus status{};
                const bool playing =
                    SUCCEEDED(pb->get_PlaybackStatus(&status)) && status == Wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing;
                fn = playing ? &Wmc::IGlobalSystemMediaTransportControlsSession::TryPauseAsync : &Wmc::IGlobalSystemMediaTransportControlsSession::TryPlayAsync;
            }
            RunBoolCommand(active.session.get(), fn);
            break;
        }
        case MediaCommand::Next: RunBoolCommand(active.session.get(), &Wmc::IGlobalSystemMediaTransportControlsSession::TrySkipNextAsync); break;
        case MediaCommand::Previous: RunBoolCommand(active.session.get(), &Wmc::IGlobalSystemMediaTransportControlsSession::TrySkipPreviousAsync); break;
        case MediaCommand::Seek: {
            winrt::com_ptr<BoolOp> op;
            if (SUCCEEDED(active.session->TryChangePlaybackPositionAsync(action.positionUs * 10, op.put())) && op)
                AwaitAsync<BoolHandler>(op.get(), stopEvent_.handle, kAsyncTimeoutMs);
            // QQ Music's newer versions answer Windows' seek and stay put: its own command as well.
            if (PlayerOf(active.appId) == MediaPlayer::QqMusic)
                QqMusicSeek(action.positionUs);
            break;
        }
        case MediaCommand::Like: LikeTrack(PlayerOf(active.appId)); break;
        case MediaCommand::Lyrics: ToggleLyrics(PlayerOf(active.appId)); break;
        case MediaCommand::NextSession: cycleBy(1); break;
        case MediaCommand::ActivateApp: ActivatePlayerApp(active.appId); break;
        }
    }

    void RefreshAndPost()
    {
        FilterConfig filter;
        std::deque<QueuedAction> actions;
        {
            std::lock_guard lock(mutex_);
            filter = filter_;
            filterDirty_ = false;
            actions.swap(queue_);
        }

        SyncSessionList();
        winrt::com_ptr<Wmc::IGlobalSystemMediaTransportControlsSession> sysCur;
        winrt::com_ptr<IUnknown> sysCurId;
        if (manager_) {
            const HRESULT curHr = manager_->GetCurrentSession(sysCur.put());
            if (SUCCEEDED(curHr) && sysCur)
                sysCur->QueryInterface(IID_PPV_ARGS(sysCurId.put()));
            else if (IsRpcError(curHr))
                ResetManager();
        }

        std::vector<MediaSnapshot> snaps(sessions_.size());
        std::vector<MediaSessionSummary> summaries(sessions_.size());
        std::vector<uint8_t> tlUpdated(sessions_.size(), 0);
        std::vector<int> eligible;
        for (size_t i = 0; i < sessions_.size(); ++i) {
            const bool isSys = sysCurId && sessions_[i]->identity.get() == sysCurId.get();
            tlUpdated[i] = ReadSessionSnapshot(*sessions_[i], isSys, snaps[i], summaries[i]) ? 1 : 0;
            const auto& s = summaries[i];
            if (s.state != MediaPlaybackState::Closed && (!s.title.empty() || s.state == MediaPlaybackState::Playing) &&
                (s.state == MediaPlaybackState::Playing || (filter.showPaused && s.state == MediaPlaybackState::Paused)) &&
                MediaAppAllowed(s.appId, filter.appMode, filter.apps))
                eligible.push_back(static_cast<int>(i));
        }

        int picked = PickMediaSession(summaries, filter.showPaused, filter.appMode, filter.apps, pinnedId_);
        if (!pinnedId_.empty() && (picked < 0 || summaries[picked].id != pinnedId_))
            pinnedId_.clear();

        for (const auto& act : actions) {
            ExecuteAction(act, eligible, picked);
            if (act.kind == QueuedAction::Kind::Cycle || (act.kind == QueuedAction::Kind::Command && act.command == MediaCommand::NextSession))
                picked = PickMediaSession(summaries, filter.showPaused, filter.appMode, filter.apps, pinnedId_);
        }
        MediaSnapshot result;
        bool activeTimelineUpdated = false;
        if (picked >= 0 && picked < static_cast<int>(snaps.size())) {
            result = snaps[picked];
            result.active = true;
            result.sessionCount = static_cast<int>(eligible.size());
            const auto posIt = std::ranges::find(eligible, picked);
            result.sessionIndex = posIt == eligible.end() ? 0 : static_cast<int>(posIt - eligible.begin());
            activeTimelineUpdated = tlUpdated[picked] != 0;
            if (result.artwork.get() != lastPostedArt_.get()) {
                ++artworkVersion_;
                lastPostedArt_ = result.artwork;
            }
            result.artworkVersion = artworkVersion_;
        } else if (lastPostedArt_) {
            ++artworkVersion_;
            lastPostedArt_.reset();
            result.artworkVersion = artworkVersion_;
        }
        if (!hasPosted_ || activeTimelineUpdated || SnapshotChanged(lastPosted_, result)) {
            lastPosted_ = result;
            hasPosted_ = true;
            PostOwned(notify_, stateMsg_, 0, std::make_unique<MediaSnapshot>(std::move(result)));
        }
    }

    void Run()
    {
        const RoInitScope ro;
        if (!ro.ok) {
            Log(L"media: RoInitialize failed");
            return;
        }
        bool woke = true;
        double refreshedMs = 0;
        while (!stop_.load(std::memory_order_acquire)) {
            if (!EnsureManager()) {
                WaitForSingleObject(stopEvent_.handle, 5000);
                continue;
            }
            bool hasCommands = false;
            {
                std::lock_guard lock(mutex_);
                hasCommands = !queue_.empty() || filterDirty_;
            }
            // News (an event, a command) is gathered for a moment, then read; while sampling, the sessions
            // are read again only every kRefreshMs (the level is sampled every round).
            if (woke && !hasCommands && hasPosted_ && WaitForSingleObject(stopEvent_.handle, kDebounceMs) == WAIT_OBJECT_0)
                break;
            const double nowMs = ClockMs();
            if (woke || hasCommands || nowMs - refreshedMs >= kRefreshMs) {
                RefreshAndPost();
                refreshedMs = nowMs;
            }
            if (stop_.load(std::memory_order_acquire))
                break;

            const bool hasActivePlayingSession = lastPosted_.active && lastPosted_.state == MediaPlaybackState::Playing;
            const bool wantSample = hasActivePlayingSession && samplingActive_.load(std::memory_order_acquire);
            if (wantSample || lastPeakPlaying_) {
                const float peak = wantSample ? meter_.Sample(lastPosted_.appId) : 0.0f;
                lastPeakPlaying_ = wantSample;
                if (onPeak_)
                    onPeak_(peak, wantSample);
            }
            // A player whose heart is read from its files (NetEase) tells no event when it changes: poll
            // every second while active/playing or while the card is open.
            const bool heartRead = lastPosted_.active && PlayerOf(lastPosted_.appId) == MediaPlayer::Netease && cardVisible_.load(std::memory_order_acquire);
            const DWORD waitMs = wantSample ? kSampleIntervalMs : hasActivePlayingSession ? static_cast<DWORD>(kRefreshMs) : heartRead ? 1000 : INFINITE;
            woke = WaitForSingleObject(wake_.handle, waitMs) == WAIT_OBJECT_0;
        }
        ResetManager();
        wic_ = nullptr;
        meter_.Reset();
    }
};

MediaSessionWatcher::MediaSessionWatcher(HWND notify, UINT stateMsg, std::function<void(float peak, bool playing)> onPeak)
    : impl_(std::make_unique<Impl>(notify, stateMsg, std::move(onPeak)))
{
}
MediaSessionWatcher::~MediaSessionWatcher() = default;

void MediaSessionWatcher::SetFilter(bool showPaused, std::wstring appMode, std::vector<std::wstring> apps)
{
    impl_->SetFilter(showPaused, std::move(appMode), std::move(apps));
}
void MediaSessionWatcher::SetSamplingActive(bool barVisible) { impl_->SetSamplingActive(barVisible); }
void MediaSessionWatcher::SetCardVisible(bool visible) { impl_->SetCardVisible(visible); }

void MediaSessionWatcher::Command(MediaCommand command, int64_t positionUs) { impl_->Enqueue({QueuedAction::Kind::Command, command, positionUs, 0}); }
void MediaSessionWatcher::CycleSession(int delta) { impl_->Enqueue({QueuedAction::Kind::Cycle, MediaCommand::NextSession, 0, delta}); }

} // namespace atrium
