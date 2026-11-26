#include "shell/games.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace atrium {

namespace {

// Library folders of the game stores (folded): what runs from under them is a game.
constexpr const wchar_t* kStoreFolders[] = {L"\\steamapps\\common\\", L"\\epic games\\",      L"\\xboxgames\\",
                                            L"\\riot games\\",        L"\\gog games\\",       L"\\gog galaxy\\games\\",
                                            L"\\wegameapps\\",        L"\\hoyoplay\\games\\", L"\\ubisoft game launcher\\games\\",
                                            L"\\ea games\\"};
constexpr ULONGLONG kStoreReadMs = 30000;   // Game Bar's list, refreshed asynchronously at most this often
constexpr ULONGLONG kPidHitTtlMs = 2000;    // short TTL on PID hits so recycled PIDs expire promptly
constexpr ULONGLONG kPidMissTtlMs = 1500;   // short TTL on negative/empty lookups

struct PidEntry {
    std::wstring path;
    bool isGame = false;
    ULONGLONG expiresAt = 0;
    uint64_t gen = 0;
};

std::mutex g_lock;
std::vector<std::wstring> g_user, g_known; // folded paths: marked by the user; Game Bar's
ULONGLONG g_knownAt = 0;
uint64_t g_gen = 1;
bool g_refreshing = false;
std::unordered_map<DWORD, PidEntry> g_byPid;

std::wstring Fold(std::wstring s)
{
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

// Game Bar's games: HKCU\System\GameConfigStore\Children\*\MatchedExeFullPath.
std::vector<std::wstring> ReadGameBarList(const std::atomic<bool>& stop)
{
    std::vector<std::wstring> paths;
    HKEY children = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &children) != ERROR_SUCCESS)
        return paths;
    wchar_t name[256];
    for (DWORD i = 0; !stop.load(); ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(children, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = sizeof(path);
        if (RegGetValueW(children, name, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS && path[0])
            paths.push_back(Fold(path));
    }
    RegCloseKey(children);
    return paths;
}

struct GameStoreWorker {
    std::mutex wakeLock;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    bool pending = false;
    std::thread thread;

    GameStoreWorker() = default;
    GameStoreWorker(const GameStoreWorker&) = delete;
    GameStoreWorker& operator=(const GameStoreWorker&) = delete;

    ~GameStoreWorker()
    {
        {
            std::lock_guard g(wakeLock);
            stop = true;
        }
        cv.notify_all();
        if (thread.joinable())
            thread.join();
    }
};

GameStoreWorker& StoreWorker()
{
    static GameStoreWorker worker;
    return worker;
}

void StoreWorkerLoop(GameStoreWorker& w)
{
    for (;;) {
        {
            std::unique_lock lk(w.wakeLock);
            w.cv.wait(lk, [&w] { return w.stop.load() || w.pending; });
            if (w.stop.load())
                return;
            w.pending = false;
        }
        std::vector<std::wstring> paths = ReadGameBarList(w.stop);
        if (w.stop.load())
            return;
        const ULONGLONG now = GetTickCount64();
        std::lock_guard g(g_lock);
        g_knownAt = now ? now : 1;
        if (paths != g_known) {
            g_known = std::move(paths);
            ++g_gen;
        }
        g_refreshing = false;
    }
}

void RequestStoreRefresh()
{
    GameStoreWorker& w = StoreWorker();
    std::lock_guard g(w.wakeLock);
    if (w.stop.load())
        return;
    w.pending = true;
    if (!w.thread.joinable())
        w.thread = std::thread([&w] { StoreWorkerLoop(w); });
    w.cv.notify_one();
}

bool IsGamePath(const std::wstring& path)
{
    if (path.empty())
        return false;
    if (std::ranges::any_of(kStoreFolders, [&](const wchar_t* f) { return path.find(f) != std::wstring::npos; }))
        return true;
    auto matches = [&](const std::wstring& entry) { return entry.ends_with(L'\\') ? path.starts_with(entry) : path == entry; };
    return std::ranges::any_of(g_known, matches) || std::ranges::any_of(g_user, matches);
}

std::wstring ProgramOf(DWORD pid)
{
    std::wstring path;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buffer[MAX_PATH * 2] = {};
        DWORD size = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(process, 0, buffer, &size))
            path = Fold(buffer);
        CloseHandle(process);
    }
    return path;
}

bool Exclusive()
{
    QUERY_USER_NOTIFICATION_STATE state{};
    return SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

} // namespace

void SetUserGames(const std::vector<std::wstring>& entries)
{
    std::vector<std::wstring> paths;
    for (const std::wstring& e : entries)
        if (e.find(L'\\') != std::wstring::npos)
            paths.push_back(Fold(e));
    bool needRefresh = false;
    {
        std::lock_guard g(g_lock);
        if (paths != g_user) {
            g_user = std::move(paths);
            ++g_gen;
        }
        if (!g_knownAt && !g_refreshing) {
            g_refreshing = true;
            needRefresh = true;
        }
    }
    if (needRefresh)
        RequestStoreRefresh();
}

bool IsGameWindow(HWND window)
{
    DWORD pid = 0;
    if (!window || !GetWindowThreadProcessId(window, &pid) || pid == GetCurrentProcessId())
        return false;
    const ULONGLONG now = GetTickCount64();
    bool needRefresh = false;
    {
        std::lock_guard g(g_lock);
        if (const auto it = g_byPid.find(pid); it != g_byPid.end() && IsGamePidCacheValid(now, it->second.expiresAt, it->second.gen, g_gen))
            return it->second.isGame;
        if (!g_refreshing && (!g_knownAt || now - g_knownAt > kStoreReadMs)) {
            g_refreshing = true;
            needRefresh = true;
        }
    }
    if (needRefresh)
        RequestStoreRefresh();
    const std::wstring path = ProgramOf(pid);
    std::lock_guard g(g_lock);
    const bool isGame = IsGamePath(path);
    if (g_byPid.size() > 512) {
        std::erase_if(g_byPid, [&](const auto& kv) { return !IsGamePidCacheValid(now, kv.second.expiresAt, kv.second.gen, g_gen); });
        if (g_byPid.size() > 512)
            g_byPid.clear();
    }
    const ULONGLONG ttl = path.empty() ? kPidMissTtlMs : kPidHitTtlMs;
    g_byPid.insert_or_assign(pid, PidEntry{path, isGame, now + ttl, g_gen});
    return isGame;
}

bool GameInFront(HMONITOR monitor)
{
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
    if (!root || IsIconic(root) || MonitorFromWindow(root, MONITOR_DEFAULTTONULL) != monitor)
        return false;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(root, &pid) || pid == GetCurrentProcessId())
        return false;
    if (Exclusive())
        return true;
    if (!IsGameWindow(root))
        return false;
    MONITORINFO mi{sizeof(mi)};
    RECT r{};
    if (!GetMonitorInfoW(monitor, &mi) || !GetWindowRect(root, &r))
        return false;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(root, GWL_STYLE));
    return IsFullscreenGameBounds(r, mi.rcMonitor, style, IsZoomed(root) != FALSE);
}

} // namespace atrium
