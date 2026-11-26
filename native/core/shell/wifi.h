// Wi-Fi through the native WLAN API (wlanapi): the current connection and its
// signal, the networks in range, connecting to a network that has a saved
// profile and disconnecting. Networks without a profile (a password to enter)
// are left to the system's network list. Calls block (scanning takes seconds):
// call them from a worker thread.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace atrium {

struct WifiState {
    bool adapter = false;   // a Wi-Fi interface exists
    bool connected = false;
    std::wstring ssid;
    int signal = 0;         // 0..100
};

struct WifiConnection {
    GUID interfaceGuid{};
    std::wstring ssid;
    int signal = -1;        // 0..100, or -1 unknown
};

struct WifiNetwork {
    std::wstring ssid;
    std::wstring profile;   // saved profile name; empty: none
    int signal = 0;         // 0..100
    bool secured = false;
    bool connected = false;
    GUID interfaceGuid{};
};

enum class WifiListStatus { Ok, NoAdapter, AccessDenied, Error };

struct WifiListResult {
    WifiListStatus status = WifiListStatus::Ok;
    std::vector<WifiNetwork> networks;
};

WifiState QueryWifi();
std::vector<WifiConnection> QueryConnectedWifi();
std::vector<WifiNetwork> MergeWifiNetworks(std::vector<WifiNetwork> networks);
WifiListStatus ClassifyWifiListStatus(int interfaceCount, bool anySuccess, bool anyAccessDenied, DWORD enumError = ERROR_SUCCESS);
// Networks in range across all adapters, strongest first, one entry per SSID.
WifiListResult ListWifi(bool rescan, HANDLE stopEvent = nullptr);
bool ConnectWifi(const WifiNetwork& network);
bool DisconnectWifi(const GUID* interfaceGuid = nullptr);

} // namespace atrium
