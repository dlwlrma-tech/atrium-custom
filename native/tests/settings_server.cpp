#include "host_messages.h"
#include "settings.h"
#include "settings_server.h"

#include <windows.h>
#include <aclapi.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace json = winrt::Windows::Data::Json;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct TempDir {
    fs::path path;
    explicit TempDir(const wchar_t* prefix)
    {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        path = fs::path(base) / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::error_code ec;
        fs::create_directories(path, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

struct HandleCloser {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~HandleCloser()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    HandleCloser() = default;
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
};

std::string ReadBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::wstring Widen(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool WaitForServerReady(const atrium::SettingsServer& server)
{
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!server.Ready()) {
        if (GetTickCount64() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

HANDLE ConnectTestPipe(const std::wstring& pipeName)
{
    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) {
                CloseHandle(h);
                return INVALID_HANDLE_VALUE;
            }
            return h;
        }
        const DWORD err = GetLastError();
        if (GetTickCount64() >= deadline)
            return INVALID_HANDLE_VALUE;
        if (err == ERROR_PIPE_BUSY || err == ERROR_FILE_NOT_FOUND)
            WaitNamedPipeW(pipeName.c_str(), 100);
        else
            return INVALID_HANDLE_VALUE;
    }
}

bool Transact(HANDLE pipe, const json::JsonObject& req, json::JsonObject& out)
{
    const std::string payload = Narrow(std::wstring(req.Stringify()));
    DWORD written = 0;
    if (!WriteFile(pipe, payload.data(), DWORD(payload.size()), &written, nullptr) || written != payload.size())
        return false;
    std::string reply;
    std::string chunk(64 * 1024, '\0');
    for (;;) {
        DWORD read = 0;
        const bool ok = ReadFile(pipe, chunk.data(), DWORD(chunk.size()), &read, nullptr);
        const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err != ERROR_MORE_DATA)
            return false;
        reply.append(chunk.data(), read);
        if (err != ERROR_MORE_DATA)
            break;
    }
    return json::JsonObject::TryParse(Widen(reply), out);
}

void CheckSettingsServerLifecycle()
{
    TempDir temp(L"atrium-pipe-test-");
    const fs::path cfgPath = temp.path / L"settings.json";
    atrium::Settings initial;
    initial.height = 60;
    initial.margin = 10;
    Check(atrium::SaveSettingsTo(cfgPath.wstring(), initial), "seed initial temp settings.json");

    const std::wstring pipeName = L"\\\\.\\pipe\\Atrium.Core.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    atrium::SettingsServer server(nullptr, initial, pipeName, cfgPath.wstring());
    Check(WaitForServerReady(server), "SettingsServer becomes ready on custom pipe name");

    HandleCloser client;
    client.handle = ConnectTestPipe(pipeName);
    Check(client.handle != INVALID_HANDLE_VALUE, "same-user client connects to DACL-protected pipe");
    if (client.handle == INVALID_HANDLE_VALUE)
        return;

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    Check(GetSecurityInfo(client.handle, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) == ERROR_SUCCESS &&
              dacl != nullptr,
          "pipe object has an explicit DACL");
    if (sd)
        LocalFree(sd);

    json::JsonObject getReq;
    getReq.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"get"));
    json::JsonObject state{nullptr};
    Check(Transact(client.handle, getReq, state), "get request succeeds");
    Check(state.GetNamedString(L"op", L"") == L"state", "get returns state");
    Check(state.GetNamedNumber(L"protocol", 0) == 1, "get returns protocol 1");
    const std::wstring instanceId(state.GetNamedString(L"instanceId", L""));
    Check(!instanceId.empty(), "get returns non-empty instanceId");
    Check(state.GetNamedNumber(L"revision", 0) == 1, "get returns initial revision 1");
    Check(state.GetNamedString(L"path", L"") == cfgPath.wstring(), "get returns configured settings path");

    json::JsonObject commitPatch;
    commitPatch.SetNamedValue(L"height", json::JsonValue::CreateNumberValue(72));
    json::JsonObject commitReq;
    commitReq.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"commit"));
    commitReq.SetNamedValue(L"instanceId", json::JsonValue::CreateStringValue(instanceId));
    commitReq.SetNamedValue(L"expectedRevision", json::JsonValue::CreateNumberValue(1));
    commitReq.SetNamedValue(L"settings", commitPatch);
    json::JsonObject commitOk{nullptr};
    Check(Transact(client.handle, commitReq, commitOk), "matching commit succeeds");
    Check(commitOk.GetNamedString(L"op", L"") == L"ok" && commitOk.GetNamedString(L"instanceId", L"") == instanceId &&
              commitOk.GetNamedNumber(L"revision", 0) == 2,
          "commit returns ok and advances revision to 2");
    Check(atrium::LoadSettingsFrom(cfgPath.wstring()).height == 72, "commit persisted height 72 to temp file");
    const std::string savedBytes = ReadBytes(cfgPath);

    json::JsonObject stalePatch;
    stalePatch.SetNamedValue(L"height", json::JsonValue::CreateNumberValue(88));
    json::JsonObject staleReq;
    staleReq.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"commit"));
    staleReq.SetNamedValue(L"instanceId", json::JsonValue::CreateStringValue(instanceId));
    staleReq.SetNamedValue(L"expectedRevision", json::JsonValue::CreateNumberValue(1));
    staleReq.SetNamedValue(L"settings", stalePatch);
    json::JsonObject conflict1{nullptr};
    Check(Transact(client.handle, staleReq, conflict1), "stale expectedRevision commit returns reply");
    Check(conflict1.GetNamedString(L"op", L"") == L"conflict" && conflict1.GetNamedString(L"instanceId", L"") == instanceId &&
              conflict1.GetNamedNumber(L"revision", 0) == 2 && conflict1.GetNamedString(L"message", L"") == L"settings changed since last read" &&
              conflict1.GetNamedObject(L"saved").GetNamedNumber(L"height", 0) == 72,
          "stale expectedRevision returns conflict with latest snapshot");
    Check(ReadBytes(cfgPath) == savedBytes, "stale expectedRevision conflict does not overwrite settings file");

    json::JsonObject wrongInstReq;
    wrongInstReq.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"commit"));
    wrongInstReq.SetNamedValue(L"instanceId", json::JsonValue::CreateStringValue(L"wrong-instance-id"));
    wrongInstReq.SetNamedValue(L"expectedRevision", json::JsonValue::CreateNumberValue(2));
    wrongInstReq.SetNamedValue(L"settings", stalePatch);
    json::JsonObject conflict2{nullptr};
    Check(Transact(client.handle, wrongInstReq, conflict2), "wrong instanceId commit returns reply");
    Check(conflict2.GetNamedString(L"op", L"") == L"conflict" && conflict2.GetNamedNumber(L"revision", 0) == 2, "wrong instanceId returns conflict");
    Check(ReadBytes(cfgPath) == savedBytes, "wrong instanceId conflict does not overwrite settings file");

    Check(server.Modify([](atrium::Settings& s) { s.margin = 24; }), "Modify() saves and succeeds");
    json::JsonObject afterModify{nullptr};
    Check(Transact(client.handle, getReq, afterModify), "get after Modify succeeds");
    Check(afterModify.GetNamedNumber(L"revision", 0) == 3 && afterModify.GetNamedObject(L"saved").GetNamedNumber(L"margin", 0) == 24 &&
              afterModify.GetNamedObject(L"current").GetNamedNumber(L"margin", 0) == 24,
          "Modify() advances revision to 3 and updates saved/current");

    json::JsonObject legacyPatch;
    legacyPatch.SetNamedValue(L"height", json::JsonValue::CreateNumberValue(64));
    json::JsonObject legacyReq;
    legacyReq.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"commit"));
    legacyReq.SetNamedValue(L"settings", legacyPatch);
    json::JsonObject legacyOk{nullptr};
    Check(Transact(client.handle, legacyReq, legacyOk), "legacy commit without instanceId/expectedRevision succeeds");
    Check(legacyOk.GetNamedString(L"op", L"") == L"ok" && legacyOk.GetNamedNumber(L"revision", 0) == 4 && server.Current().height == 64,
          "legacy commit advances revision to 4 and updates current");
}

struct ReadyTestState {
    bool received = false;
    bool readyWhenReceived = false;
    const atrium::SettingsServer* server = nullptr;
};

LRESULT CALLBACK ReadyTestProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == atrium::WM_ATRIUM_SERVER_READY) {
        if (auto* state = reinterpret_cast<ReadyTestState*>(GetWindowLongPtrW(h, GWLP_USERDATA))) {
            state->received = true;
            state->readyWhenReceived = state->server && state->server->Ready();
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void CheckSettingsServerPostsReadyMessage()
{
    TempDir temp(L"atrium-ready-test-");
    const fs::path cfgPath = temp.path / L"settings.json";
    atrium::Settings initial;
    Check(atrium::SaveSettingsTo(cfgPath.wstring(), initial), "seed initial temp settings.json for ready message test");

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t kClassName[] = L"AtriumSettingsReadyTest";
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ReadyTestProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    Check(hwnd != nullptr, "creates message-only host window for ready handshake test");
    if (!hwnd) {
        UnregisterClassW(kClassName, instance);
        return;
    }

    ReadyTestState state;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    {
        const std::wstring pipeName =
            L"\\\\.\\pipe\\Atrium.Core.ReadyTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        atrium::SettingsServer server(hwnd, initial, pipeName, cfgPath.wstring());
        state.server = &server;

        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (!state.received) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (state.received)
                break;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                break;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(deadline - now), QS_ALLINPUT);
        }
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, instance);

    Check(state.received, "SettingsServer posts WM_ATRIUM_SERVER_READY to host window");
    Check(state.readyWhenReceived, "SettingsServer::Ready() is true when WM_ATRIUM_SERVER_READY arrives");
}

} // namespace

int main()
{
    winrt::init_apartment();
    CheckSettingsServerLifecycle();
    CheckSettingsServerPostsReadyMessage();
    if (g_failures) {
        std::fprintf(stderr, "%d settings server check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("settings server checks passed");
    return 0;
}
