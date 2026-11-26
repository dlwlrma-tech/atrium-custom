// Desktop fences (Tencent-style glass organizer boxes on the desktop):
// mount any local folder (including other drives) or a dedicated fence folder,
// accept dragged desktop icons/files, expand on hover or stay pinned open,
// lock/unlock position and size, and persist in %LOCALAPPDATA%\Atrium\fences.json.
#pragma once

#include "icon_image.h"
#include "ui/glass_menu.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

class DesktopFences {
public:
    explicit DesktopFences(HINSTANCE instance);
    ~DesktopFences();
    DesktopFences(const DesktopFences&) = delete;
    DesktopFences& operator=(const DesktopFences&) = delete;

    void SetEnabled(bool on);
    void SetMonitor(HMONITOR monitor, bool refresh = false);
    void SetRadius(float dip);
    void OnShellRestart();
    void AddNewFence();
    void AddFolderFence();

    GlassMenu* menu = nullptr;

private:
    struct Item {
        std::wstring name;
        std::wstring path;
        bool isDir = false;
        std::shared_ptr<const IconImage> icon;
        uint64_t iconId = 0;
    };
    struct Fence;
    struct LoadJob {
        std::wstring id;
        std::wstring folder;
        uint64_t gen = 0;
        int iconPx = 48;
    };
    struct LoadResult {
        std::wstring id;
        uint64_t gen = 0;
        std::vector<Item> items;
    };

    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    void Load();
    void Save() const;
    void Build();
    void Clear();
    Fence& CreateFence(const std::wstring& id, const std::wstring& title, const std::wstring& folder, float x, float y, float w, float h, bool locked,
                       bool alwaysExpanded, bool listView = false);
    void RemoveFence(const std::wstring& id);
    Fence* FindFence(const std::wstring& id);
    float Scale() const;
    RECT WorkArea() const;
    RECT FenceScreenRect(const Fence& f) const;
    void PlaceFence(Fence& f, bool pop = false);
    void SetExpanded(Fence& f, bool expanded);
    void Restack();
    void QueueReload(Fence& f);
    void WorkerLoop();
    void ApplyLoadResult(LoadResult res);
    void PaintFence(Fence& f, Canvas& c);
    bool HandleFenceMessage(Fence& f, UINT m, WPARAM wp, LPARAM lp, LRESULT& r);
    int HitFenceItem(const Fence& f, float x, float y) const;
    int HitFenceItem(const Fence& f, POINTL screen) const;
    bool IsApplicationItem(const Item& item) const;
    bool IsApplicationItemAt(const Fence& f, int itemIndex) const;
    void LaunchItemWithFiles(Fence& f, int itemIndex, const std::vector<std::wstring>& paths);
    void SetDropHover(Fence& f, int itemIndex);
    void HandleDropFiles(Fence& f, const std::vector<std::wstring>& paths, bool copy);
    void HandleDragEnter(Fence& f);
    void HandleDragLeave(Fence& f);
    void ShowFenceMenu(Fence& f, POINT screen, int itemIndex);
    std::wstring PickFolderDialog(const std::wstring& initial) const;
    void MoveItemToDesktop(Fence& f, const Item& item);
    void BeginRename(Fence& f);
    void EndRename(Fence& f, bool commit);

    friend struct FenceDropTarget;

    HINSTANCE instance_;
    HWND host_ = nullptr;
    HWINEVENTHOOK eventHook_ = nullptr;
    HMONITOR monitor_ = nullptr;
    bool enabled_ = true;
    bool oleReady_ = false;
    bool wasShowDesktop_ = false;
    float radius_ = 18.f;
    uint64_t nextGen_ = 1;
    std::vector<std::unique_ptr<Fence>> fences_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<LoadJob> jobs_;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace atrium
