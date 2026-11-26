#include "shell/audio_meter.h"

#include "win32_path.h"

#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace atrium {

namespace {

constexpr uint64_t kSessionRefreshIntervalMs = 1000;
constexpr uint64_t kDeviceRetryMs = 1000;

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

struct DeviceNotifyClient : winrt::implements<DeviceNotifyClient, IMMNotificationClient, IAgileObject> {
    explicit DeviceNotifyClient(std::shared_ptr<std::atomic<bool>> dirty) : dirty_(std::move(dirty)) {}

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) noexcept override
    {
        if (dirty_)
            dirty_->store(true, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) noexcept override
    {
        if (dirty_)
            dirty_->store(true, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR /*pwstrDefaultDeviceId*/) noexcept override
    {
        if (flow == eRender && (role == eMultimedia || role == eConsole) && dirty_)
            dirty_->store(true, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) noexcept override { return S_OK; }

private:
    std::shared_ptr<std::atomic<bool>> dirty_;
};

bool SessionMatchesApp(IAudioSessionControl2* control, const std::wstring& targetFolded, const std::wstring& targetStem)
{
    if (!control || targetFolded.empty() || control->IsSystemSoundsSession() == S_OK)
        return false;
    DWORD pid = 0;
    if (FAILED(control->GetProcessId(&pid)) || pid == 0)
        return false;
    const ScopedHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.handle)
        return false;
    // A packaged player's sound may come from another app of its package (Apple Music plays from
    // AMPLibraryAgent, "…!LibraryServer", while its media session is the window's "…!App"): the package counts.
    const std::wstring aumid = FoldCase(ProcessAumid(process.handle));
    const size_t bang = targetFolded.find(L'!');
    if (!aumid.empty() && (aumid == targetFolded || (bang != std::wstring::npos && aumid.compare(0, bang + 1, targetFolded, 0, bang + 1) == 0)))
        return true;
    const std::wstring path = FoldCase(ProcessImagePath(process.handle));
    if (path.empty())
        return false;
    if (path == targetFolded)
        return true;
    return !targetStem.empty() && ExeStemFolded(path) == targetStem;
}

winrt::com_ptr<IMMDevice> GetDefaultRenderDevice(IMMDeviceEnumerator* enumerator, std::wstring& outId)
{
    outId.clear();
    if (!enumerator)
        return {};
    winrt::com_ptr<IMMDevice> dev;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.put()))) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())))
            return {};
    }
    if (!dev)
        return {};
    LPWSTR rawId = nullptr;
    if (SUCCEEDED(dev->GetId(&rawId)) && rawId) {
        outId = rawId;
        CoTaskMemFree(rawId);
    }
    return dev;
}

} // namespace

struct AudioPeakMeter::Impl {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    winrt::com_ptr<DeviceNotifyClient> notifyClient;
    std::shared_ptr<std::atomic<bool>> deviceDirty = std::make_shared<std::atomic<bool>>(false);
    winrt::com_ptr<IMMDevice> device;
    std::wstring deviceId;
    winrt::com_ptr<IAudioSessionManager2> sessionManager;
    winrt::com_ptr<IAudioMeterInformation> masterMeter;
    std::vector<winrt::com_ptr<IAudioMeterInformation>> sessionMeters;
    std::wstring cachedAppId;
    uint64_t lastRefreshMs = 0;
    uint64_t nextRetryMs = 0;

    void ClearDevice();
    bool ActivateDevice(winrt::com_ptr<IMMDevice> dev, std::wstring id);
    bool EnsureDevice();
    bool RefreshSessions(const std::wstring& appIdOrExe);
};

void AudioPeakMeter::Impl::ClearDevice()
{
    device = nullptr;
    deviceId.clear();
    sessionManager = nullptr;
    masterMeter = nullptr;
    sessionMeters.clear();
    lastRefreshMs = 0;
}

bool AudioPeakMeter::Impl::ActivateDevice(winrt::com_ptr<IMMDevice> dev, std::wstring id)
{
    if (!dev)
        return false;
    winrt::com_ptr<IAudioSessionManager2> manager;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr, manager.put_void())) || !manager)
        return false;

    winrt::com_ptr<IAudioMeterInformation> meter;
    if (FAILED(dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_INPROC_SERVER, nullptr, meter.put_void())) || !meter)
        return false;

    device = std::move(dev);
    deviceId = std::move(id);
    sessionManager = std::move(manager);
    masterMeter = std::move(meter);
    sessionMeters.clear();
    lastRefreshMs = 0;
    nextRetryMs = 0;
    return true;
}

bool AudioPeakMeter::Impl::EnsureDevice()
{
    if (deviceDirty->exchange(false, std::memory_order_acq_rel)) {
        ClearDevice();
        nextRetryMs = 0;
    }
    if (device && sessionManager && masterMeter)
        return true;

    const uint64_t now = GetTickCount64();
    if (now < nextRetryMs)
        return false;

    if (!enumerator) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(enumerator.put()))) || !enumerator) {
            nextRetryMs = now + kDeviceRetryMs;
            return false;
        }
        auto client = winrt::make_self<DeviceNotifyClient>(deviceDirty);
        if (SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(client.get())))
            notifyClient = std::move(client);
    }

    std::wstring devId;
    winrt::com_ptr<IMMDevice> dev = GetDefaultRenderDevice(enumerator.get(), devId);
    if (!ActivateDevice(std::move(dev), std::move(devId))) {
        ClearDevice();
        nextRetryMs = now + kDeviceRetryMs;
        return false;
    }
    return true;
}

bool AudioPeakMeter::Impl::RefreshSessions(const std::wstring& appIdOrExe)
{
    if (enumerator) {
        std::wstring currentId;
        winrt::com_ptr<IMMDevice> currentDev = GetDefaultRenderDevice(enumerator.get(), currentId);
        if (!currentDev)
            return false;
        if (currentId != deviceId && !ActivateDevice(std::move(currentDev), std::move(currentId)))
            return false;
    }

    sessionMeters.clear();
    cachedAppId = appIdOrExe;
    lastRefreshMs = GetTickCount64();

    if (appIdOrExe.empty() || !sessionManager)
        return true;

    winrt::com_ptr<IAudioSessionEnumerator> sessions;
    const HRESULT enumHr = sessionManager->GetSessionEnumerator(sessions.put());
    if (enumHr == AUDCLNT_E_DEVICE_INVALIDATED)
        return false;
    if (FAILED(enumHr) || !sessions)
        return true;

    const std::wstring targetFolded = FoldCase(appIdOrExe);
    const std::wstring targetStem = ExeStemFolded(appIdOrExe);
    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        winrt::com_ptr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, control.put())) || !control)
            continue;
        AudioSessionState state = AudioSessionStateExpired;
        if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired)
            continue;
        auto control2 = control.try_as<IAudioSessionControl2>();
        if (!SessionMatchesApp(control2.get(), targetFolded, targetStem))
            continue;
        if (auto meter = control.try_as<IAudioMeterInformation>())
            sessionMeters.push_back(std::move(meter));
    }
    return true;
}

AudioPeakMeter::AudioPeakMeter() : impl_(std::make_unique<Impl>()) {}
AudioPeakMeter::~AudioPeakMeter() { Reset(); }
AudioPeakMeter::AudioPeakMeter(AudioPeakMeter&&) noexcept = default;
AudioPeakMeter& AudioPeakMeter::operator=(AudioPeakMeter&&) noexcept = default;

void AudioPeakMeter::Reset()
{
    if (!impl_)
        return;
    impl_->ClearDevice();
    if (impl_->enumerator && impl_->notifyClient)
        impl_->enumerator->UnregisterEndpointNotificationCallback(impl_->notifyClient.get());
    impl_->notifyClient = nullptr;
    impl_->enumerator = nullptr;
    impl_->cachedAppId.clear();
    impl_->nextRetryMs = 0;
    impl_->deviceDirty->store(false, std::memory_order_release);
}

float AudioPeakMeter::Sample(const std::wstring& appIdOrExe)
{
    if (!impl_ || !impl_->EnsureDevice())
        return 0.0f;

    const uint64_t now = GetTickCount64();
    if (appIdOrExe != impl_->cachedAppId || impl_->lastRefreshMs == 0 || (now - impl_->lastRefreshMs) >= kSessionRefreshIntervalMs) {
        if (!impl_->RefreshSessions(appIdOrExe)) {
            impl_->ClearDevice();
            impl_->nextRetryMs = now + kDeviceRetryMs;
            return 0.0f;
        }
    }

    if (!impl_->sessionMeters.empty()) {
        float maxPeak = 0.0f;
        for (const auto& meter : impl_->sessionMeters) {
            float peak = 0.0f;
            const HRESULT hr = meter->GetPeakValue(&peak);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                impl_->ClearDevice();
                return 0.0f;
            }
            if (SUCCEEDED(hr) && std::isfinite(peak))
                maxPeak = std::max(maxPeak, peak);
        }
        return std::clamp(maxPeak, 0.0f, 1.0f);
    }

    float masterPeak = 0.0f;
    const HRESULT hr = impl_->masterMeter->GetPeakValue(&masterPeak);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        impl_->ClearDevice();
        return 0.0f;
    }
    return SUCCEEDED(hr) && std::isfinite(masterPeak) ? std::clamp(masterPeak, 0.0f, 1.0f) : 0.0f;
}

} // namespace atrium
