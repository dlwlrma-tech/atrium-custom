#include "shell/wifi.h"

#include "log.h"

#include <windows.h>
#include <wlanapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>

namespace atrium {

namespace {

// One client handle per call; the API is cheap to open and this keeps every
// function usable from any thread.
class Wlan {
public:
    Wlan()
    {
        DWORD version = 0;
        openError_ = WlanOpenHandle(2, nullptr, &version, &handle_);
        if (openError_ != ERROR_SUCCESS)
            handle_ = nullptr;
    }
    ~Wlan()
    {
        if (list_)
            WlanFreeMemory(list_);
        if (handle_)
            WlanCloseHandle(handle_, nullptr);
    }
    Wlan(const Wlan&) = delete;
    Wlan& operator=(const Wlan&) = delete;
    HANDLE Handle() const { return handle_; }
    DWORD EnumInterfaces(std::vector<GUID>& out)
    {
        if (!handle_)
            return openError_;
        if (DWORD r = list_ ? ERROR_SUCCESS : WlanEnumInterfaces(handle_, nullptr, &list_); r != ERROR_SUCCESS)
            return r;
        for (DWORD i = 0; list_ && i < list_->dwNumberOfItems; ++i)
            out.push_back(list_->InterfaceInfo[i].InterfaceGuid);
        return ERROR_SUCCESS;
    }
    const WLAN_INTERFACE_INFO_LIST* InterfaceList()
    {
        if (!handle_)
            return nullptr;
        if (!list_ && WlanEnumInterfaces(handle_, nullptr, &list_) != ERROR_SUCCESS)
            return nullptr;
        return list_;
    }
    // The connected Wi-Fi interface, or the first if none is connected.
    const GUID* Interface()
    {
        if (!handle_)
            return nullptr;
        if (!list_ && WlanEnumInterfaces(handle_, nullptr, &list_) != ERROR_SUCCESS)
            return nullptr;
        if (!list_ || !list_->dwNumberOfItems)
            return nullptr;
        for (DWORD i = 0; i < list_->dwNumberOfItems; ++i)
            if (list_->InterfaceInfo[i].isState == wlan_interface_state_connected)
                return &list_->InterfaceInfo[i].InterfaceGuid;
        return &list_->InterfaceInfo[0].InterfaceGuid;
    }

private:
    HANDLE handle_ = nullptr;
    DWORD openError_ = ERROR_SUCCESS;
    WLAN_INTERFACE_INFO_LIST* list_ = nullptr;
};

std::wstring Ssid(const DOT11_SSID& s)
{
    std::string bytes(reinterpret_cast<const char*>(s.ucSSID), std::min<ULONG>(s.uSSIDLength, DOT11_SSID_MAX_LENGTH));
    // SSIDs are bytes; nearly always UTF-8.
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0);
    UINT cp = n > 0 ? CP_UTF8 : CP_ACP;
    n = MultiByteToWideChar(cp, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring out(size_t(std::max(n, 0)), L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), int(bytes.size()), out.data(), n);
    return out;
}

} // namespace

WifiState QueryWifi()
{
    WifiState s;
    Wlan w;
    const GUID* itf = w.Interface();
    if (!itf)
        return s;
    s.adapter = true;
    DWORD size = 0;
    WLAN_CONNECTION_ATTRIBUTES* conn = nullptr;
    if (WlanQueryInterface(w.Handle(), itf, wlan_intf_opcode_current_connection, nullptr, &size, reinterpret_cast<void**>(&conn), nullptr) == ERROR_SUCCESS &&
        conn) {
        if (conn->isState == wlan_interface_state_connected) {
            s.connected = true;
            s.ssid = Ssid(conn->wlanAssociationAttributes.dot11Ssid);
            s.signal = int(conn->wlanAssociationAttributes.wlanSignalQuality);
        }
        WlanFreeMemory(conn);
    }
    return s;
}

std::vector<WifiConnection> QueryConnectedWifi()
{
    std::vector<WifiConnection> out;
    Wlan w;
    const WLAN_INTERFACE_INFO_LIST* list = w.InterfaceList();
    for (DWORD i = 0; list && i < list->dwNumberOfItems; ++i) {
        const GUID& g = list->InterfaceInfo[i].InterfaceGuid;
        DWORD sz = 0;
        WLAN_CONNECTION_ATTRIBUTES* c = nullptr;
        if (list->InterfaceInfo[i].isState != wlan_interface_state_connected ||
            WlanQueryInterface(w.Handle(), &g, wlan_intf_opcode_current_connection, nullptr, &sz, reinterpret_cast<void**>(&c), nullptr) != ERROR_SUCCESS || !c)
            continue;
        if (c->isState == wlan_interface_state_connected) {
            std::wstring s = Ssid(c->wlanAssociationAttributes.dot11Ssid);
            out.push_back({g, s.empty() ? c->strProfileName : std::move(s), int(c->wlanAssociationAttributes.wlanSignalQuality)});
        }
        WlanFreeMemory(c);
    }
    return out;
}

std::vector<WifiNetwork> MergeWifiNetworks(std::vector<WifiNetwork> networks)
{
    std::map<std::wstring, WifiNetwork> bySsid;
    for (WifiNetwork& n : networks) {
        if (n.ssid.empty())
            continue;
        auto [it, inserted] = bySsid.emplace(n.ssid, n);
        if (inserted)
            continue;
        WifiNetwork& m = it->second;
        int sig = std::max(m.signal, n.signal);
        if (n.connected && !m.connected)
            m = std::move(n);
        else if (n.connected == m.connected && m.profile.empty() && !n.profile.empty())
            m = {m.ssid, std::move(n.profile), sig, n.secured, m.connected, n.interfaceGuid};
        else
            m.secured |= n.secured;
        m.signal = sig;
    }
    std::vector<WifiNetwork> out;
    out.reserve(bySsid.size());
    for (auto& [ssid, n] : bySsid)
        out.push_back(std::move(n));
    std::ranges::sort(out, [](const WifiNetwork& a, const WifiNetwork& b) { return a.connected != b.connected ? a.connected : a.signal > b.signal; });
    return out;
}

WifiListStatus ClassifyWifiListStatus(int interfaceCount, bool anySuccess, bool anyAccessDenied, DWORD enumError)
{
    if (enumError != ERROR_SUCCESS)
        return enumError == ERROR_ACCESS_DENIED ? WifiListStatus::AccessDenied : WifiListStatus::Error;
    if (interfaceCount <= 0)
        return WifiListStatus::NoAdapter;
    if (anySuccess)
        return WifiListStatus::Ok;
    return anyAccessDenied ? WifiListStatus::AccessDenied : WifiListStatus::Error;
}

namespace {

void ReadInterface(HANDLE h, const GUID& guid, std::vector<WifiNetwork>& raw, bool& anyOk, bool& denied, DWORD& err)
{
    auto note = [&](DWORD r) {
        denied |= (r == ERROR_ACCESS_DENIED);
        err = r;
    };
    DWORD size = 0;
    WLAN_CONNECTION_ATTRIBUTES* conn = nullptr;
    DWORD qr = WlanQueryInterface(h, &guid, wlan_intf_opcode_current_connection, nullptr, &size, reinterpret_cast<void**>(&conn), nullptr);
    if (qr == ERROR_SUCCESS && conn) {
        anyOk = true;
        if (conn->isState == wlan_interface_state_connected) {
            std::wstring ssid = Ssid(conn->wlanAssociationAttributes.dot11Ssid);
            if (ssid.empty())
                ssid = conn->strProfileName;
            if (!ssid.empty())
                raw.push_back({std::move(ssid), conn->strProfileName, int(conn->wlanAssociationAttributes.wlanSignalQuality),
                               conn->wlanSecurityAttributes.bSecurityEnabled != FALSE, true, guid});
        }
        WlanFreeMemory(conn);
    } else if (qr != ERROR_INVALID_STATE)
        note(qr);
    WLAN_AVAILABLE_NETWORK_LIST* list = nullptr;
    DWORD lr = WlanGetAvailableNetworkList(h, &guid, WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES, nullptr, &list);
    if (lr == ERROR_SUCCESS && list) {
        anyOk = true;
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
            const WLAN_AVAILABLE_NETWORK& a = list->Network[i];
            if (std::wstring ssid = Ssid(a.dot11Ssid); !ssid.empty())
                raw.push_back({std::move(ssid), (a.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) ? a.strProfileName : L"", int(a.wlanSignalQuality),
                               a.bSecurityEnabled != FALSE, (a.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0, guid});
        }
        WlanFreeMemory(list);
    } else
        note(lr);
}

} // namespace

WifiListResult ListWifi(bool rescan, HANDLE stopEvent)
{
    WifiListResult result;
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
        return result;
    Wlan w;
    std::vector<GUID> itfs;
    DWORD enumErr = w.EnumInterfaces(itfs), lastErr = enumErr;
    bool anySuccess = false, anyAccessDenied = false, anyScanOk = false;
    if (enumErr == ERROR_SUCCESS && !itfs.empty()) {
        if (rescan) {
            for (const GUID& guid : itfs)
                if (DWORD sr = WlanScan(w.Handle(), &guid, nullptr, nullptr, nullptr); sr == ERROR_SUCCESS)
                    anyScanOk = true;
                else if (lastErr = sr; sr == ERROR_ACCESS_DENIED)
                    anyAccessDenied = true;
            if (anyScanOk) {
                HANDLE waitEvent = stopEvent ? stopEvent : CreateEventW(nullptr, TRUE, FALSE, nullptr);
                const bool stopped = waitEvent && WaitForSingleObject(waitEvent, 3500) == WAIT_OBJECT_0;
                if (!stopEvent && waitEvent)
                    CloseHandle(waitEvent);
                if (stopped)
                    return result;
            }
        }
        std::vector<WifiNetwork> raw;
        for (const GUID& guid : itfs)
            ReadInterface(w.Handle(), guid, raw, anySuccess, anyAccessDenied, lastErr);
        result.networks = MergeWifiNetworks(std::move(raw));
    }
    result.status = ClassifyWifiListStatus(int(itfs.size()), anySuccess, anyAccessDenied, enumErr);
    DWORD logErr = result.status == WifiListStatus::Ok ? ERROR_SUCCESS : (result.status == WifiListStatus::AccessDenied ? ERROR_ACCESS_DENIED : lastErr);
    static std::atomic<uint64_t> lastLogged{~uint64_t{0}};
    if (uint64_t key = (uint64_t(result.status) << 32) | uint64_t(logErr); lastLogged.exchange(key) != key)
        Log(L"wifi: list status=%d err=%lu", int(result.status), logErr);
    return result;
}

bool ConnectWifi(const WifiNetwork& network)
{
    if (network.profile.empty())
        return false;
    Wlan w;
    const GUID* itf = !IsEqualGUID(network.interfaceGuid, GUID{}) ? &network.interfaceGuid : w.Interface();
    if (!itf)
        return false;
    WLAN_CONNECTION_PARAMETERS p{};
    p.wlanConnectionMode = wlan_connection_mode_profile;
    p.strProfile = network.profile.c_str();
    p.dot11BssType = dot11_BSS_type_any;
    DWORD r = WlanConnect(w.Handle(), itf, &p, nullptr);
    Log(L"wifi: connect to a saved network: %lu", r);
    return r == ERROR_SUCCESS;
}

bool DisconnectWifi(const GUID* interfaceGuid)
{
    Wlan w;
    const GUID* itf = interfaceGuid && !IsEqualGUID(*interfaceGuid, GUID{}) ? interfaceGuid : w.Interface();
    DWORD r = itf ? WlanDisconnect(w.Handle(), itf, nullptr) : ERROR_NOT_FOUND;
    Log(L"wifi: disconnect: %lu", r);
    return r == ERROR_SUCCESS;
}

} // namespace atrium
