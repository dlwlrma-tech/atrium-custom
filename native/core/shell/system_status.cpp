// winsock2.h before windows.h: iphlpapi.h requires it and ws2ipdef.h pulls in netioapi.h.
#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

#include "shell/system_status.h"

#include "log.h"
#include "shell/wifi.h"
#include "../posted.h"

#include <endpointvolume.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <netlistmgr.h>
#include <powrprof.h>
#include <shellapi.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace atrium {

namespace {

winrt::com_ptr<IAudioEndpointVolume> DefaultVolume(std::wstring* name = nullptr, std::wstring* id = nullptr)
{
    winrt::com_ptr<IMMDeviceEnumerator> devices;
    winrt::com_ptr<IMMDevice> device;
    winrt::com_ptr<IAudioEndpointVolume> volume;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(devices.put()))) ||
        FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put())) ||
        FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, volume.put_void())))
        return nullptr;
    if (id) {
        LPWSTR rawId = nullptr;
        if (SUCCEEDED(device->GetId(&rawId)) && rawId)
            *id = rawId;
        CoTaskMemFree(rawId);
    }
    winrt::com_ptr<IPropertyStore> props;
    if (name && SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put()))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            *name = v.pwszVal;
        PropVariantClear(&v);
    }
    return volume;
}

// Forwards Core Audio's volume notifications (on its own thread) as a message.
class VolumeCallback : public winrt::implements<VolumeCallback, IAudioEndpointVolumeCallback> {
public:
    VolumeCallback(HWND hwnd, UINT msg) : hwnd_(hwnd), msg_(msg) {}
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override
    {
        PostMessageW(hwnd_, msg_, 0, 0);
        return S_OK;
    }

private:
    HWND hwnd_;
    UINT msg_;
};

int KindRank(NetworkLink::Kind k) { return k == NetworkLink::Kind::Ethernet ? 0 : k == NetworkLink::Kind::Wifi ? 1 : 2; }

std::optional<NetworkLink::Kind> UsableKind(const MIB_IF_ROW2& r)
{
    if (r.MediaConnectState != MediaConnectStateConnected || r.OperStatus != IfOperStatusUp || !r.InterfaceAndOperStatusFlags.HardwareInterface ||
        r.InterfaceAndOperStatusFlags.FilterInterface || r.TunnelType != TUNNEL_TYPE_NONE || r.Type == IF_TYPE_SOFTWARE_LOOPBACK)
        return std::nullopt;
    return r.Type == IF_TYPE_IEEE80211                                                                                    ? NetworkLink::Kind::Wifi :
           (r.Type == IF_TYPE_WWANPP || r.Type == IF_TYPE_WWANPP2 || r.PhysicalMediumType == NdisPhysicalMediumBluetooth) ? NetworkLink::Kind::Other :
           r.Type == IF_TYPE_ETHERNET_CSMACD                                                                              ? NetworkLink::Kind::Ethernet :
                                                                                                                            NetworkLink::Kind::Other;
}

std::wstring NetName(INetwork* net)
{
    BSTR b = nullptr;
    std::wstring n = net && SUCCEEDED(net->GetName(&b)) && b ? b : L"";
    SysFreeString(b);
    return n;
}

std::unordered_map<uint64_t, uint64_t> DefaultRouteMetrics()
{
    std::unordered_map<uint64_t, uint64_t> best;
    std::vector<std::tuple<uint64_t, ADDRESS_FAMILY, ULONG>> ifm;
    if (PMIB_IPINTERFACE_TABLE t = nullptr; GetIpInterfaceTable(AF_UNSPEC, &t) == NO_ERROR && t) {
        for (ULONG i = 0; i < t->NumEntries; ++i)
            ifm.emplace_back(t->Table[i].InterfaceLuid.Value, t->Table[i].Family, t->Table[i].Metric);
        FreeMibTable(t);
    }
    if (PMIB_IPFORWARD_TABLE2 t = nullptr; GetIpForwardTable2(AF_UNSPEC, &t) == NO_ERROR && t) {
        for (ULONG i = 0; i < t->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2& r = t->Table[i];
            if (r.DestinationPrefix.PrefixLength != 0)
                continue;
            const uint64_t luid = r.InterfaceLuid.Value;
            auto it =
                std::ranges::find_if(ifm, [&](const auto& m) { return std::get<0>(m) == luid && std::get<1>(m) == r.DestinationPrefix.Prefix.si_family; });
            const uint64_t eff = uint64_t(r.Metric) + (it != ifm.end() ? std::get<2>(*it) : 0);
            if (auto [bit, ins] = best.emplace(luid, eff); !ins && eff < bit->second)
                bit->second = eff;
        }
        FreeMibTable(t);
    }
    return best;
}

struct NlmAdapter {
    GUID guid{};
    std::wstring name;
    bool internet = false;
};

void ReadNlm(bool needGlobalNet, bool& online, bool& connected, std::wstring& globalNet, std::vector<NlmAdapter>& byAdapter)
{
    winrt::com_ptr<INetworkListManager> nlm;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(nlm.put()))))
        return;
    if (NLM_CONNECTIVITY c{}; SUCCEEDED(nlm->GetConnectivity(&c))) {
        online = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
        connected = c != NLM_CONNECTIVITY_DISCONNECTED;
    }
    if (winrt::com_ptr<IEnumNetworkConnections> conns; SUCCEEDED(nlm->GetNetworkConnections(conns.put())))
        for (winrt::com_ptr<INetworkConnection> conn; conns->Next(1, conn.put(), nullptr) == S_OK; conn = nullptr) {
            VARIANT_BOOL ok = VARIANT_FALSE;
            GUID gid{};
            NLM_CONNECTIVITY nc{};
            if (FAILED(conn->get_IsConnected(&ok)) || !ok || FAILED(conn->GetAdapterId(&gid)) || FAILED(conn->GetConnectivity(&nc)))
                continue;
            bool inet = (nc & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
            winrt::com_ptr<INetwork> net;
            std::wstring n = SUCCEEDED(conn->GetNetwork(net.put())) ? NetName(net.get()) : L"";
            if (auto it = std::ranges::find_if(byAdapter, [&](const NlmAdapter& e) { return IsEqualGUID(e.guid, gid); }); it == byAdapter.end())
                byAdapter.push_back({gid, std::move(n), inet});
            else if (inet && !it->internet) {
                it->internet = true;
                it->name = n.empty() ? it->name : std::move(n);
            } else if (it->name.empty() && !n.empty())
                it->name = std::move(n);
        }
    if (winrt::com_ptr<IEnumNetworks> nets; needGlobalNet && SUCCEEDED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, nets.put())))
        for (winrt::com_ptr<INetwork> net; nets->Next(1, net.put(), nullptr) == S_OK; net = nullptr) {
            NLM_CONNECTIVITY nc{};
            net->GetConnectivity(&nc);
            std::wstring n = NetName(net.get());
            if (nc & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) {
                globalNet = std::move(n);
                return;
            }
            globalNet = globalNet.empty() ? std::move(n) : globalNet;
        }
}

void QueryNetwork(SystemStatus& s)
{
    std::vector<GUID> guids;
    if (PMIB_IF_TABLE2 t = nullptr; GetIfTable2(&t) == NO_ERROR && t) {
        for (ULONG i = 0; i < t->NumEntries; ++i)
            if (auto k = UsableKind(t->Table[i])) {
                s.links.push_back({*k, t->Table[i].InterfaceLuid.Value, t->Table[i].Alias[0] ? t->Table[i].Alias : t->Table[i].Description});
                guids.push_back(t->Table[i].InterfaceGuid);
            }
        FreeMibTable(t);
    }
    bool online = false, connected = false;
    std::wstring globalNet;
    std::vector<NlmAdapter> nlm;
    ReadNlm(s.links.empty(), online, connected, globalNet, nlm);
    if (s.links.empty()) {
        DeriveNetworkStatus(s, online, connected, globalNet, QueryWifi());
        return;
    }
    const auto wifi = QueryConnectedWifi();
    const auto metrics = DefaultRouteMetrics();
    std::vector<LinkRouteInfo> routes;
    for (size_t i = 0; i < s.links.size(); ++i) {
        NetworkLink& l = s.links[i];
        std::wstring nlmName;
        if (auto it = std::ranges::find_if(nlm, [&](const NlmAdapter& e) { return IsEqualGUID(e.guid, guids[i]); }); it != nlm.end()) {
            l.internet = it->internet;
            nlmName = it->name;
        }
        if (l.kind == NetworkLink::Kind::Wifi)
            if (auto it = std::ranges::find_if(wifi, [&](const WifiConnection& w) { return IsEqualGUID(w.interfaceGuid, guids[i]); }); it != wifi.end()) {
                l.signal = it->signal;
                l.network = it->ssid;
            }
        l.network = !l.network.empty() ? l.network : !nlmName.empty() ? nlmName : l.alias;
        routes.push_back({l.luid, l.kind, l.internet, metrics.contains(l.luid) ? metrics.at(l.luid) : ~uint64_t{0}});
    }
    if (int p = PickPrimaryLink(routes); p >= 0)
        s.links[size_t(p)].primary = true;
    std::ranges::sort(s.links, [](const NetworkLink& a, const NetworkLink& b) {
        return std::tuple(!a.primary, !a.internet, a.luid) < std::tuple(!b.primary, !b.internet, b.luid);
    });
    DeriveNetworkStatus(s, online, connected, globalNet, {});
}

} // namespace

int PickPrimaryLink(const std::vector<LinkRouteInfo>& c)
{
    auto k = [](const LinkRouteInfo& x) { return std::tuple(!x.internet, x.routeMetric, KindRank(x.kind), x.luid); };
    return c.empty() ? -1 : int(std::ranges::min_element(c, [&](const auto& a, const auto& b) { return k(a) < k(b); }) - c.begin());
}

void DeriveNetworkStatus(SystemStatus& s, bool globalOnline, bool globalConnected, const std::wstring& globalNetwork, const WifiState& fallbackWifi)
{
    if (s.links.empty()) {
        s.online = globalOnline;
        s.connected = globalConnected;
        s.wifi = fallbackWifi.connected && s.connected;
        s.signal = s.wifi ? fallbackWifi.signal : 0;
        s.network = s.wifi && !fallbackWifi.ssid.empty() && (globalNetwork.empty() || !s.online) ? fallbackWifi.ssid : globalNetwork;
        return;
    }
    auto k = [](const NetworkLink& l) { return std::tuple(!l.primary, !l.internet, KindRank(l.kind), l.luid); };
    const NetworkLink& p = *std::ranges::min_element(s.links, [&](const auto& a, const auto& b) { return k(a) < k(b); });
    s.connected = true;
    s.online = globalOnline || p.internet;
    s.wifi = p.kind == NetworkLink::Kind::Wifi;
    s.signal = s.wifi ? p.signal : 0;
    s.network = !p.network.empty() ? p.network : !p.alias.empty() ? p.alias : globalNetwork;
}

void SystemStatusReader::UnwatchVolume()
{
    if (watched_ && callback_)
        watched_->UnregisterControlChangeNotify(callback_.get());
    watched_ = nullptr;
    watchedEndpointId_.clear();
}

void SystemStatusReader::WatchVolume()
{
    std::wstring endpointId;
    winrt::com_ptr<IAudioEndpointVolume> volume = DefaultVolume(nullptr, &endpointId);
    if (!volume || endpointId.empty()) {
        UnwatchVolume();
        return;
    }
    float level = 0;
    if (watched_ && endpointId == watchedEndpointId_ && SUCCEEDED(watched_->GetMasterVolumeLevelScalar(&level)))
        return;
    UnwatchVolume();
    if (!callback_)
        callback_ = winrt::make<VolumeCallback>(notify_, volumeMessage_);
    if (SUCCEEDED(volume->RegisterControlChangeNotify(callback_.get()))) {
        watched_ = std::move(volume);
        watchedEndpointId_ = std::move(endpointId);
    }
}

SystemStatusReader::SystemStatusReader(HWND notify, UINT readyMessage, UINT volumeMessage)
    : notify_(notify),
      readyMessage_(readyMessage),
      volumeMessage_(volumeMessage),
      quit_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      thread_([this] { Run(); })
{
}

SystemStatusReader::~SystemStatusReader()
{
    SetEvent(quit_);
    thread_.join();
    CloseHandle(wake_);
    CloseHandle(quit_);
}

void SystemStatusReader::Refresh() { SetEvent(wake_); }

void SystemStatusReader::Run()
{
    SetThreadDescription(GetCurrentThread(), L"Atrium system status");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    HANDLE events[] = {quit_, wake_};
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        auto status = std::make_unique<SystemStatus>(QuerySystemStatus());
        WatchVolume();
        if (WaitForSingleObject(quit_, 0) == WAIT_OBJECT_0)
            break;
        PostOwned(notify_, readyMessage_, 0, std::move(status));
    }
    UnwatchVolume();
    callback_ = nullptr;
    winrt::uninit_apartment();
}

SystemStatus QuerySystemStatus()
{
    SystemStatus s;
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        // 128: no system battery; 255: unknown.
        s.battery = ps.BatteryFlag != 128 && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100;
        s.batteryPercent = s.battery ? ps.BatteryLifePercent : -1;
        s.charging = ps.ACLineStatus == 1;
        s.batteryMinutes = s.battery && !s.charging && ps.BatteryLifeTime != DWORD(-1) ? int(ps.BatteryLifeTime / 60) : -1;
    }
    try {
        QueryNetwork(s);
        // Read interactive volume last: a slower network query must not
        // deliver a volume value captured before the user's latest input.
        if (auto v = DefaultVolume(&s.audioDevice)) {
            float level = 0;
            BOOL mute = FALSE;
            v->GetMasterVolumeLevelScalar(&level);
            v->GetMute(&mute);
            s.audio = true;
            s.volume = int(std::lround(level * 100));
            s.muted = mute != FALSE;
        }
    } catch (...) {
        Log(L"status: query failed");
    }
    return s;
}

int AdjustVolume(int delta)
{
    auto v = DefaultVolume();
    if (!v)
        return -1;
    float level = 0;
    v->GetMasterVolumeLevelScalar(&level);
    level = std::clamp(level + float(delta) / 100.f, 0.f, 1.f);
    v->SetMasterVolumeLevelScalar(level, nullptr);
    if (delta > 0)
        v->SetMute(FALSE, nullptr);
    return int(std::lround(level * 100));
}

void SetVolume(int percent)
{
    if (auto v = DefaultVolume()) {
        v->SetMasterVolumeLevelScalar(float(std::clamp(percent, 0, 100)) / 100.f, nullptr);
        if (percent > 0)
            v->SetMute(FALSE, nullptr);
    }
}

bool ToggleMute()
{
    auto v = DefaultVolume();
    BOOL mute = FALSE;
    if (!v || FAILED(v->GetMute(&mute)))
        return false;
    v->SetMute(!mute, nullptr);
    return true;
}

bool CanHibernate()
{
    SYSTEM_POWER_CAPABILITIES c{};
    return GetPwrCapabilities(&c) && c.SystemS4 && c.HiberFilePresent;
}

bool RunPowerAction(PowerAction action)
{
    const wchar_t* names[] = {L"sleep", L"hibernate", L"restart", L"shutdown"};
    Log(L"power: %s", names[int(action)]);
    if (action == PowerAction::Sleep)
        return SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
    if (action == PowerAction::Hibernate) {
        if (CanHibernate()) {
            const bool ok = SetSuspendState(TRUE, FALSE, FALSE) != FALSE;
            if (!ok)
                Log(L"power: SetSuspendState(hibernate) failed (%lu)", GetLastError());
            return ok;
        }
        Log(L"power: hibernation is disabled; requesting elevation to enable and hibernate");
        HINSTANCE launched = ShellExecuteW(nullptr, L"runas", L"cmd.exe", L"/c powercfg /h on && shutdown /h", nullptr, SW_HIDE);
        const bool ok = reinterpret_cast<INT_PTR>(launched) > 32;
        if (!ok)
            Log(L"power: failed to elevate hibernate enablement (%Id)", reinterpret_cast<INT_PTR>(launched));
        return ok;
    }
    // Shutting down needs SeShutdownPrivilege enabled in the token (every
    // interactive user has it, disabled by default).
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    UINT flags = action == PowerAction::Restart ? EWX_REBOOT : EWX_SHUTDOWN | EWX_POWEROFF;
    bool ok = ExitWindowsEx(flags, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED) != FALSE;
    if (!ok)
        Log(L"power: ExitWindowsEx failed (%lu)", GetLastError());
    return ok;
}

} // namespace atrium
