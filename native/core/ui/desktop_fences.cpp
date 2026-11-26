#include "ui/desktop_fences.h"

#include "clock.h"
#include "localization.h"
#include "log.h"
#include "posted.h"
#include "settings.h"
#include "shell/shell_icon.h"
#include "ui/surface.h"
#include "ui/widget_stacking.h"
#include "win32_path.h"

#include <ole2.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr wchar_t kFenceHostClass[] = L"AtriumFenceHost";
constexpr UINT WM_FENCE_LOADED = WM_APP + 0x81;
constexpr UINT_PTR kCollapseTimer = 0x5F01;
constexpr UINT_PTR kRefreshTimer = 0x5F02;
constexpr UINT_PTR kRestackTimer = 0x5F03;
constexpr UINT kCollapseDelayMs = 260;
constexpr UINT kRefreshMs = 5000;
constexpr float kHeaderH = 34.f;
constexpr float kPadX = 10.f;
constexpr float kPadY = 8.f;
constexpr float kCellW = 76.f;
constexpr float kCellH = 78.f;
constexpr float kIconSz = 36.f;
constexpr float kMinW = 220.f;
constexpr float kMaxW = 920.f;
constexpr float kMinH = 160.f;
constexpr float kMaxH = 820.f;
constexpr float kGrip = 16.f;

DesktopFences* g_fences = nullptr;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool InRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

std::wstring FencesJsonPath() { return DataDirectory() + L"\\fences.json"; }
std::wstring DefaultFencesDir() { return DataDirectory() + L"\\Fences"; }

std::wstring BaseName(const std::wstring& path)
{
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    const wchar_t* name = PathFindFileNameW(p.c_str());
    return name && *name ? std::wstring(name) : p;
}

std::wstring DisplayNameFor(const std::wstring& fileName, bool isDir)
{
    if (!isDir && fileName.size() > 4 && _wcsicmp(fileName.c_str() + fileName.size() - 4, L".lnk") == 0)
        return fileName.substr(0, fileName.size() - 4);
    return fileName;
}

bool IsImageExtension(const std::wstring& name)
{
    const wchar_t* ext = PathFindExtensionW(name.c_str());
    if (!ext || !*ext)
        return false;
    return _wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 || _wcsicmp(ext, L".bmp") == 0 ||
           _wcsicmp(ext, L".webp") == 0;
}

std::wstring UniqueDestination(const std::wstring& dir, const std::wstring& fileName)
{
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
        base += L'\\';
    std::wstring candidate = base + fileName;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
        return candidate;
    const wchar_t* ext = PathFindExtensionW(fileName.c_str());
    const std::wstring extStr = ext ? ext : L"";
    const std::wstring stem = fileName.substr(0, fileName.size() - extStr.size());
    for (int n = 2; n < 1000; ++n) {
        candidate = base + stem + L" (" + std::to_wstring(n) + L")" + extStr;
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return base + fileName;
}

bool CopyOrMovePath(const std::wstring& src, const std::wstring& dstDir, bool copy)
{
    if (src.empty() || dstDir.empty())
        return false;
    const std::wstring dst = UniqueDestination(dstDir, BaseName(src));
    if (FoldCase(src) == FoldCase(dst))
        return false;
    if (!copy) {
        if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
            return true;
    }
    SHFILEOPSTRUCTW op{};
    std::wstring from = src;
    from.push_back(L'\0');
    from.push_back(L'\0');
    std::wstring to = dst;
    to.push_back(L'\0');
    to.push_back(L'\0');
    op.wFunc = copy ? FO_COPY : FO_MOVE;
    op.pFrom = from.c_str();
    op.pTo = to.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

std::wstring DesktopFolderPath()
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

void LaunchFile(const std::wstring& path)
{
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RevealInExplorer(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        const std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

} // namespace

struct FenceDropTarget : winrt::implements<FenceDropTarget, IDropTarget> {
    DesktopFences* owner;
    std::wstring fenceId;
    bool hasFiles = false;

    FenceDropTarget(DesktopFences* o, std::wstring id) : owner(o), fenceId(std::move(id)) {}

    HRESULT __stdcall DragEnter(IDataObject* data, DWORD keyState, POINTL, DWORD* effect) override
    {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        hasFiles = data && data->QueryGetData(&fe) == S_OK;
        *effect = hasFiles ? ((keyState & MK_CONTROL) ? DROPEFFECT_COPY : DROPEFFECT_MOVE) : DROPEFFECT_NONE;
        if (hasFiles && owner)
            if (auto* f = owner->FindFence(fenceId))
                owner->HandleDragEnter(*f);
        return S_OK;
    }
    HRESULT __stdcall DragOver(DWORD keyState, POINTL, DWORD* effect) override
    {
        *effect = hasFiles ? ((keyState & MK_CONTROL) ? DROPEFFECT_COPY : DROPEFFECT_MOVE) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT __stdcall DragLeave() override
    {
        if (owner)
            if (auto* f = owner->FindFence(fenceId))
                owner->HandleDragLeave(*f);
        return S_OK;
    }
    HRESULT __stdcall Drop(IDataObject* data, DWORD keyState, POINTL, DWORD* effect) override
    {
        const bool copy = (keyState & MK_CONTROL) != 0;
        *effect = hasFiles ? (copy ? DROPEFFECT_COPY : DROPEFFECT_MOVE) : DROPEFFECT_NONE;
        if (owner)
            if (auto* f = owner->FindFence(fenceId))
                owner->HandleDragLeave(*f);
        if (!hasFiles || !data || !owner)
            return S_OK;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (FAILED(data->GetData(&fe, &sm)))
            return S_OK;
        std::vector<std::wstring> paths;
        if (auto* drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count && i < 128; ++i) {
                if (const UINT len = DragQueryFileW(drop, i, nullptr, 0); len > 0) {
                    std::wstring p(len + 1, L'\0');
                    const UINT copied = DragQueryFileW(drop, i, p.data(), UINT(p.size()));
                    if (copied > 0) {
                        p.resize(copied);
                        paths.push_back(std::move(p));
                    }
                }
            }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
        if (!paths.empty())
            if (auto* f = owner->FindFence(fenceId))
                owner->HandleDropFiles(*f, paths, copy);
        return S_OK;
    }
};

struct DesktopFences::Fence {
    std::wstring id;
    std::wstring title;
    std::wstring folder;
    float x = 80.f, y = 80.f, w = 360.f, h = 420.f;
    bool locked = false;
    bool alwaysExpanded = true;
    bool expanded = true;
    bool dragHighlight = false;
    bool tracking = false;
    bool moving = false;
    bool resizing = false;
    POINT dragStartScreen{};
    float dragStartX = 0, dragStartY = 0, dragStartW = 0, dragStartH = 0;
    float scrollY = 0;
    float maxScrollY = 0;
    int hoverItem = -1;
    std::wstring hoverBtn;
    int lastClickItem = -1;
    ULONGLONG lastClickMs = 0;
    uint64_t loadGen = 0;
    std::vector<Item> items;
    std::unique_ptr<Surface> surface;
    winrt::com_ptr<IDropTarget> dropTarget;
    D2D1_RECT_F lockRect{}, pinRect{}, menuRect{};
};

DesktopFences::DesktopFences(HINSTANCE instance) : instance_(instance)
{
    g_fences = this;
    oleReady_ = SUCCEEDED(OleInitialize(nullptr));
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = kFenceHostClass;
    RegisterClassExW(&wc);
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW, kFenceHostClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    eventHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    worker_ = std::thread([this] { WorkerLoop(); });
    Load();
    SetTimer(host_, kRefreshTimer, kRefreshMs, nullptr);
}

DesktopFences::~DesktopFences()
{
    KillTimer(host_, kRefreshTimer);
    KillTimer(host_, kRestackTimer);
    if (eventHook_)
        UnhookWinEvent(eventHook_);
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    Clear();
    if (host_)
        DestroyWindow(host_);
    if (oleReady_)
        OleUninitialize();
    g_fences = nullptr;
}

LRESULT CALLBACK DesktopFences::HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_fences) {
        if (m == WM_FENCE_LOADED) {
            if (auto res = TakePosted<LoadResult>(l))
                g_fences->ApplyLoadResult(std::move(*res));
            return 0;
        }
        if (m == WM_TIMER && w == kRefreshTimer) {
            for (const auto& f : g_fences->fences_)
                if (f->surface && f->surface->Visible())
                    g_fences->QueueReload(*f);
            return 0;
        }
        if (m == WM_TIMER && w == kRestackTimer) {
            KillTimer(h, kRestackTimer);
            g_fences->Restack();
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void CALLBACK DesktopFences::OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
    if (g_fences && g_fences->host_ && !g_fences->fences_.empty())
        SetTimer(g_fences->host_, kRestackTimer, 60, nullptr);
}

float DesktopFences::Scale() const
{
    HMONITOR mon = monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    if (mon)
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    return float(dx) / 96.f;
}

RECT DesktopFences::WorkArea() const
{
    HMONITOR mon = monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (mon && GetMonitorInfoW(mon, &mi))
        return mi.rcWork;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

RECT DesktopFences::FenceScreenRect(const Fence& f) const
{
    const RECT wa = WorkArea();
    const float s = Scale();
    const float visH = f.expanded ? f.h : kHeaderH;
    const LONG l = wa.left + LONG(std::lround(f.x * s));
    const LONG t = wa.top + LONG(std::lround(f.y * s));
    const LONG w = LONG(std::lround(f.w * s));
    const LONG h = LONG(std::lround(visH * s));
    return RECT{l, t, l + w, t + h};
}

void DesktopFences::PlaceFence(Fence& f, bool pop)
{
    if (!f.surface || !enabled_)
        return;
    const RECT r = FenceScreenRect(f);
    if (f.surface->Visible()) {
        f.surface->Move(r);
        f.surface->Invalidate();
    } else {
        f.surface->Show(r, false, POINT{(r.left + r.right) / 2, r.top}, pop);
    }
}

void DesktopFences::SetExpanded(Fence& f, bool expanded)
{
    if (f.expanded == expanded)
        return;
    f.expanded = expanded;
    if (!f.expanded)
        f.hoverItem = -1;
    PlaceFence(f, false);
    Restack();
}

void DesktopFences::SetEnabled(bool on)
{
    enabled_ = on;
    if (!on) {
        for (const auto& f : fences_)
            if (f->surface && f->surface->Visible())
                f->surface->Hide(false);
        return;
    }
    Build();
}

void DesktopFences::SetMonitor(HMONITOR monitor, bool refresh)
{
    if (monitor_ == monitor && !refresh)
        return;
    monitor_ = monitor;
    for (const auto& f : fences_)
        PlaceFence(*f, false);
    Restack();
}

void DesktopFences::SetRadius(float dip)
{
    radius_ = std::clamp(dip * 0.65f, 12.f, 22.f);
    for (const auto& f : fences_)
        if (f->surface)
            f->surface->SetRadius(radius_);
}

void DesktopFences::OnShellRestart() { Restack(); }

void DesktopFences::Restack()
{
    if (!enabled_ || fences_.empty())
        return;
    std::vector<HWND> hwnds;
    for (const auto& f : fences_)
        if (f->surface && f->surface->Visible())
            hwnds.push_back(f->surface->Hwnd());
    if (hwnds.empty())
        return;
    HMONITOR mon = monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi))
        return;
    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
    DWORD fgPid = 0;
    if (fgRoot)
        GetWindowThreadProcessId(fgRoot, &fgPid);
    const auto snapshot = CollectStackSnapshot(hwnds);
    const WidgetStackPlan plan = ComputeWidgetStackPlan(snapshot, mi.rcMonitor, fgRoot, fgPid == GetCurrentProcessId(), wasShowDesktop_);
    wasShowDesktop_ = plan.showDesktop;
    DWORD swpErr = 0;
    ApplyWidgetZOrder(hwnds, plan, swpErr);
}

void DesktopFences::Clear()
{
    for (const auto& f : fences_) {
        if (f->surface) {
            if (f->dropTarget)
                RevokeDragDrop(f->surface->Hwnd());
            f->surface->Hide(false);
        }
    }
    fences_.clear();
}

DesktopFences::Fence* DesktopFences::FindFence(const std::wstring& id)
{
    for (const auto& f : fences_)
        if (f->id == id)
            return f.get();
    return nullptr;
}

void DesktopFences::Load()
{
    const std::wstring text = ReadTextFile(FencesJsonPath());
    if (text.empty())
        return;
    json::JsonObject root{nullptr};
    if (!json::JsonObject::TryParse(text, root) || !root.HasKey(L"fences") || root.GetNamedValue(L"fences").ValueType() != json::JsonValueType::Array)
        return;
    for (const auto& v : root.GetNamedArray(L"fences")) {
        if (v.ValueType() != json::JsonValueType::Object)
            continue;
        auto o = v.GetObject();
        auto str = [&](const wchar_t* k) {
            return o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::String ? std::wstring(o.GetNamedString(k)) : std::wstring();
        };
        auto num = [&](const wchar_t* k, float def) {
            return o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Number ? float(o.GetNamedNumber(k)) : def;
        };
        auto flag = [&](const wchar_t* k, bool def) {
            return o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean ? o.GetNamedBoolean(k) : def;
        };
        std::wstring id = str(L"id");
        std::wstring folder = str(L"folder");
        std::wstring title = str(L"title");
        if (id.empty() || folder.empty())
            continue;
        if (title.empty())
            title = BaseName(folder);
        CreateFence(id, title, folder, num(L"x", 80.f), num(L"y", 80.f), std::clamp(num(L"w", 360.f), kMinW, kMaxW),
                    std::clamp(num(L"h", 420.f), kMinH, kMaxH), flag(L"locked", false), flag(L"alwaysExpanded", true));
    }
}

void DesktopFences::Save() const
{
    json::JsonArray arr;
    for (const auto& f : fences_) {
        json::JsonObject o;
        o.SetNamedValue(L"id", json::JsonValue::CreateStringValue(f->id));
        o.SetNamedValue(L"title", json::JsonValue::CreateStringValue(f->title));
        o.SetNamedValue(L"folder", json::JsonValue::CreateStringValue(f->folder));
        o.SetNamedValue(L"x", json::JsonValue::CreateNumberValue(f->x));
        o.SetNamedValue(L"y", json::JsonValue::CreateNumberValue(f->y));
        o.SetNamedValue(L"w", json::JsonValue::CreateNumberValue(f->w));
        o.SetNamedValue(L"h", json::JsonValue::CreateNumberValue(f->h));
        o.SetNamedValue(L"locked", json::JsonValue::CreateBooleanValue(f->locked));
        o.SetNamedValue(L"alwaysExpanded", json::JsonValue::CreateBooleanValue(f->alwaysExpanded));
        arr.Append(o);
    }
    json::JsonObject root;
    root.SetNamedValue(L"fences", arr);
    (void)WriteTextFile(FencesJsonPath(), std::wstring(root.Stringify()));
}

void DesktopFences::Build()
{
    for (const auto& f : fences_) {
        PlaceFence(*f, false);
        QueueReload(*f);
    }
    Restack();
}

DesktopFences::Fence& DesktopFences::CreateFence(const std::wstring& id, const std::wstring& title, const std::wstring& folder, float x, float y, float w,
                                                 float h, bool locked, bool alwaysExpanded)
{
    auto f = std::make_unique<Fence>();
    f->id = id;
    f->title = title;
    f->folder = folder;
    f->x = x;
    f->y = y;
    f->w = w;
    f->h = h;
    f->locked = locked;
    f->alwaysExpanded = alwaysExpanded;
    f->expanded = alwaysExpanded;
    f->surface = std::make_unique<Surface>(instance_, L"Atrium Fence", true, 12.f, true);
    f->surface->SetRadius(radius_);
    Fence* raw = f.get();
    f->surface->paint = [this, raw](Canvas& c) { PaintFence(*raw, c); };
    f->surface->message = [this, raw](UINT m, WPARAM wp, LPARAM lp, LRESULT& r) { return HandleFenceMessage(*raw, m, wp, lp, r); };
    if (oleReady_) {
        f->dropTarget = winrt::make<FenceDropTarget>(this, id);
        RegisterDragDrop(f->surface->Hwnd(), f->dropTarget.get());
    }
    fences_.push_back(std::move(f));
    if (enabled_) {
        PlaceFence(*raw, true);
        QueueReload(*raw);
        Restack();
    }
    return *raw;
}

void DesktopFences::RemoveFence(const std::wstring& id)
{
    for (auto it = fences_.begin(); it != fences_.end(); ++it) {
        if ((*it)->id != id)
            continue;
        if ((*it)->surface) {
            if ((*it)->dropTarget)
                RevokeDragDrop((*it)->surface->Hwnd());
            (*it)->surface->Hide(false);
        }
        fences_.erase(it);
        Save();
        return;
    }
}

void DesktopFences::AddNewFence()
{
    const std::wstring baseDir = DefaultFencesDir();
    SHCreateDirectoryExW(nullptr, baseDir.c_str(), nullptr);
    const std::wstring id = L"fence-" + std::to_wstring(GetTickCount64());
    const int num = int(fences_.size()) + 1;
    const std::wstring title = TrFormat(L"Desktop Box %d", num);
    const std::wstring folder = baseDir + L"\\" + id;
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const float offset = float((fences_.size() % 5) * 32);
    CreateFence(id, title, folder, 120.f + offset, 60.f + offset, 360.f, 420.f, false, true);
    Save();
}

std::wstring DesktopFences::PickFolderDialog(const std::wstring& initial) const
{
    winrt::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!initial.empty()) {
        winrt::com_ptr<IShellItem> startItem;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(startItem.put()))))
            dlg->SetFolder(startItem.get());
    }
    if (FAILED(dlg->Show(nullptr)))
        return {};
    winrt::com_ptr<IShellItem> chosen;
    if (FAILED(dlg->GetResult(chosen.put())) || !chosen)
        return {};
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(chosen->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        out = path;
        CoTaskMemFree(path);
    }
    return out;
}

void DesktopFences::AddFolderFence()
{
    const std::wstring folder = PickFolderDialog({});
    if (folder.empty())
        return;
    const std::wstring id = L"fence-" + std::to_wstring(GetTickCount64());
    const float offset = float((fences_.size() % 5) * 32);
    CreateFence(id, BaseName(folder), folder, 140.f + offset, 70.f + offset, 380.f, 440.f, false, true);
    Save();
}

void DesktopFences::QueueReload(Fence& f)
{
    const uint64_t gen = nextGen_++;
    f.loadGen = gen;
    const int iconPx = std::clamp(int(std::lround(kIconSz * Scale())), 32, 96);
    {
        std::lock_guard g(mutex_);
        std::erase_if(jobs_, [&](const LoadJob& j) { return j.id == f.id; });
        jobs_.push_back({f.id, f.folder, gen, iconPx});
    }
    cv_.notify_one();
}

void DesktopFences::WorkerLoop()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        LoadJob job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_)
                break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        LoadResult res;
        res.id = job.id;
        res.gen = job.gen;
        std::wstring pattern = job.folder;
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
            pattern += L'\\';
        const std::wstring base = pattern;
        pattern += L'*';
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            uint64_t seq = 1;
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
                    continue;
                Item item;
                item.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                item.path = base + fd.cFileName;
                item.name = DisplayNameFor(fd.cFileName, item.isDir);
                IconImage img;
                if (LoadShellImage(item.path, job.iconPx, img, !item.isDir && IsImageExtension(fd.cFileName))) {
                    item.icon = std::make_shared<const IconImage>(std::move(img));
                    item.iconId = (job.gen << 16) + seq++;
                }
                res.items.push_back(std::move(item));
            } while (FindNextFileW(h, &fd) && res.items.size() < 256);
            FindClose(h);
        }
        std::ranges::sort(res.items, [](const Item& a, const Item& b) {
            if (a.isDir != b.isDir)
                return a.isDir > b.isDir;
            return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        if (host_)
            PostOwned(host_, WM_FENCE_LOADED, 0, std::make_unique<LoadResult>(std::move(res)));
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}

void DesktopFences::ApplyLoadResult(LoadResult res)
{
    Fence* f = FindFence(res.id);
    if (!f || res.gen != f->loadGen)
        return;
    f->items = std::move(res.items);
    if (f->hoverItem >= int(f->items.size()))
        f->hoverItem = -1;
    if (f->surface)
        f->surface->Invalidate();
}

void DesktopFences::PaintFence(Fence& f, Canvas& c)
{
    const Theme& t = c.theme;
    const float w = f.w;
    const float h = f.expanded ? f.h : kHeaderH;
    if (c.panel)
        c.Fill(D2D1::RectF(0, 0, w, h), radius_, t.panel);
    if (f.dragHighlight)
        c.Stroke(D2D1::RectF(1.5f, 1.5f, w - 1.5f, h - 1.5f), std::max(4.f, radius_ - 1.5f), D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.85f), 2.f);

    // Header bar controls: lock on left, title centered, expand-mode pin and menu on right.
    const float btnSz = 24.f;
    const float btnY = (kHeaderH - btnSz) / 2.f;
    f.lockRect = D2D1::RectF(8.f, btnY, 8.f + btnSz, btnY + btnSz);
    f.menuRect = D2D1::RectF(w - 8.f - btnSz, btnY, w - 8.f, btnY + btnSz);
    f.pinRect = D2D1::RectF(f.menuRect.left - 4.f - btnSz, btnY, f.menuRect.left - 4.f, btnY + btnSz);
    auto drawHeaderBtn = [&](const D2D1_RECT_F& r, const wchar_t* id, const wchar_t* glyph, bool active) {
        if (f.hoverBtn == id)
            c.Fill(r, 7.f, t.hover);
        else if (active)
            c.Fill(r, 7.f, D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.22f));
        c.Glyph(glyph, r, 12.f, active ? D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.95f) : t.text);
    };
    drawHeaderBtn(f.lockRect, L"lock", f.locked ? L"\uE72E" : L"\uE785", f.locked);
    drawHeaderBtn(f.pinRect, L"pin", f.alwaysExpanded ? L"\uE840" : L"\uE70D", f.alwaysExpanded);
    drawHeaderBtn(f.menuRect, L"menu", L"\uE712", false);
    c.Text(f.title, D2D1::RectF(f.lockRect.right + 6.f, 0.f, f.pinRect.left - 6.f, kHeaderH), 12.5f, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);

    if (!f.expanded)
        return;

    c.Line(kPadX, kHeaderH, w - kPadX, kHeaderH, t.divider);
    const D2D1_RECT_F body = D2D1::RectF(kPadX, kHeaderH + kPadY, w - kPadX, h - kPadY);
    const float bodyW = std::max(kCellW, body.right - body.left);
    const float bodyH = std::max(kCellH, body.bottom - body.top);
    const int cols = std::max(1, int(bodyW / kCellW));
    const float colW = bodyW / float(cols);
    const int rows = f.items.empty() ? 0 : int((f.items.size() + size_t(cols) - 1) / size_t(cols));
    f.maxScrollY = std::max(0.f, float(rows) * kCellH - bodyH);
    f.scrollY = std::clamp(f.scrollY, 0.f, f.maxScrollY);

    c.Clip(body);
    if (f.items.empty()) {
        c.Text(Tr(L"Drag desktop icons or files here"), body, 12.f, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1, true);
    } else {
        for (size_t i = 0; i < f.items.size(); ++i) {
            const int col = int(i) % cols;
            const int row = int(i) / cols;
            const float x0 = body.left + float(col) * colW;
            const float y0 = body.top + float(row) * kCellH - f.scrollY;
            if (y0 + kCellH < body.top || y0 > body.bottom)
                continue;
            const D2D1_RECT_F cell = D2D1::RectF(x0 + 2.f, y0 + 2.f, x0 + colW - 2.f, y0 + kCellH - 2.f);
            if (int(i) == f.hoverItem)
                c.Fill(cell, 10.f, t.hover);
            const float icx = (cell.left + cell.right) / 2.f;
            const D2D1_RECT_F ir = D2D1::RectF(icx - kIconSz / 2.f, cell.top + 6.f, icx + kIconSz / 2.f, cell.top + 6.f + kIconSz);
            if (f.items[i].icon && !f.items[i].icon->Empty()) {
                wchar_t key[64];
                swprintf_s(key, L"fence|%llu", f.items[i].iconId);
                c.Image(key, *f.items[i].icon, ir);
            } else {
                c.Fill(ir, 8.f, Alpha(t.hover, 1.3f));
                c.Glyph(f.items[i].isDir ? L"\uE8B7" : L"\uE8A5", ir, 18.f, t.text);
            }
            const D2D1_RECT_F tr = D2D1::RectF(cell.left + 2.f, ir.bottom + 3.f, cell.right - 2.f, cell.bottom - 2.f);
            c.Text(f.items[i].name, tr, 10.5f, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1, true);
        }
    }
    c.Unclip();

    if (!f.locked) {
        const D2D1_COLOR_F gripColor = Alpha(t.subtext, 0.55f);
        c.Line(w - 11.f, h - 5.f, w - 5.f, h - 11.f, gripColor, 1.2f);
        c.Line(w - 7.f, h - 5.f, w - 5.f, h - 7.f, gripColor, 1.2f);
    }
}

void DesktopFences::HandleDragEnter(Fence& f)
{
    KillTimer(f.surface->Hwnd(), kCollapseTimer);
    f.dragHighlight = true;
    if (!f.expanded)
        SetExpanded(f, true);
    else if (f.surface)
        f.surface->Invalidate();
}

void DesktopFences::HandleDragLeave(Fence& f)
{
    f.dragHighlight = false;
    if (f.surface)
        f.surface->Invalidate();
    if (!f.alwaysExpanded && f.surface)
        SetTimer(f.surface->Hwnd(), kCollapseTimer, kCollapseDelayMs, nullptr);
}

void DesktopFences::HandleDropFiles(Fence& f, const std::vector<std::wstring>& paths, bool copy)
{
    SHCreateDirectoryExW(nullptr, f.folder.c_str(), nullptr);
    int moved = 0;
    for (const std::wstring& path : paths) {
        if (CopyOrMovePath(path, f.folder, copy))
            ++moved;
    }
    Log(L"fence: %s %d/%zu item(s) into '%s'", copy ? L"copied" : L"moved", moved, paths.size(), f.folder.c_str());
    QueueReload(f);
}

void DesktopFences::MoveItemToDesktop(Fence& f, const Item& item)
{
    const std::wstring desktop = DesktopFolderPath();
    if (desktop.empty())
        return;
    if (CopyOrMovePath(item.path, desktop, false))
        QueueReload(f);
}

void DesktopFences::ShowFenceMenu(Fence& f, POINT screen, int itemIndex)
{
    if (!menu)
        return;
    const std::wstring fenceId = f.id;
    std::vector<MenuEntry> entries;
    if (itemIndex >= 0 && size_t(itemIndex) < f.items.size()) {
        const Item chosen = f.items[size_t(itemIndex)];
        entries = {
            {chosen.name, 0, false, true, true},
            {Tr(L"Open"), 1},
            {Tr(L"Open in Explorer"), 2},
            {Tr(L"Move to Desktop"), 3},
        };
        menu->Open(std::move(entries), screen, MenuPlace::Below, [this, fenceId, chosen](int cmd) {
            Fence* target = FindFence(fenceId);
            if (!target)
                return;
            if (cmd == 1)
                LaunchFile(chosen.path);
            else if (cmd == 2)
                RevealInExplorer(chosen.path);
            else if (cmd == 3)
                MoveItemToDesktop(*target, chosen);
        });
        return;
    }
    entries = {
        {f.title, 0, false, true, true},
        {Tr(L"Always expanded"), 10, f.alwaysExpanded},
        {Tr(L"Lock position"), 11, f.locked},
        {},
        {Tr(L"Change mapped folder…"), 12},
        {Tr(L"Open in Explorer"), 13},
        {Tr(L"Refresh"), 14},
        {},
        {Tr(L"New desktop fence"), 15},
        {Tr(L"Map folder to desktop fence…"), 16},
        {Tr(L"Remove this fence"), 17},
    };
    menu->Open(std::move(entries), screen, MenuPlace::Below, [this, fenceId](int cmd) {
        Fence* target = FindFence(fenceId);
        if (cmd == 15) {
            AddNewFence();
            return;
        }
        if (cmd == 16) {
            AddFolderFence();
            return;
        }
        if (!target)
            return;
        if (cmd == 10) {
            target->alwaysExpanded = !target->alwaysExpanded;
            SetExpanded(*target, target->alwaysExpanded);
            Save();
        } else if (cmd == 11) {
            target->locked = !target->locked;
            if (target->surface)
                target->surface->Invalidate();
            Save();
        } else if (cmd == 12) {
            const std::wstring folder = PickFolderDialog(target->folder);
            if (!folder.empty()) {
                target->folder = folder;
                target->title = BaseName(folder);
                target->scrollY = 0;
                QueueReload(*target);
                Save();
            }
        } else if (cmd == 13) {
            RevealInExplorer(target->folder);
        } else if (cmd == 14) {
            QueueReload(*target);
        } else if (cmd == 17) {
            RemoveFence(fenceId);
        }
    });
}

bool DesktopFences::HandleFenceMessage(Fence& f, UINT m, WPARAM wp, LPARAM lp, LRESULT& r)
{
    const float s = f.surface->Scale();
    const float x = float(GET_X_LPARAM(lp)) / s;
    const float y = float(GET_Y_LPARAM(lp)) / s;
    auto hitItem = [&]() -> int {
        if (!f.expanded || y < kHeaderH + kPadY || y > f.h - kPadY || x < kPadX || x > f.w - kPadX)
            return -1;
        const float bodyW = std::max(kCellW, f.w - 2 * kPadX);
        const int cols = std::max(1, int(bodyW / kCellW));
        const float colW = bodyW / float(cols);
        const int col = std::clamp(int((x - kPadX) / colW), 0, cols - 1);
        const int row = int((y - (kHeaderH + kPadY) + f.scrollY) / kCellH);
        const int idx = row * cols + col;
        return idx >= 0 && size_t(idx) < f.items.size() ? idx : -1;
    };
    auto hitBtn = [&]() -> std::wstring {
        if (InRect(f.lockRect, x, y))
            return L"lock";
        if (InRect(f.pinRect, x, y))
            return L"pin";
        if (InRect(f.menuRect, x, y))
            return L"menu";
        return {};
    };

    switch (m) {
    case WM_TIMER:
        if (wp == kCollapseTimer) {
            KillTimer(f.surface->Hwnd(), kCollapseTimer);
            if (!f.alwaysExpanded && !f.moving && !f.resizing && !f.dragHighlight && (!menu || !menu->Visible()))
                SetExpanded(f, false);
            return true;
        }
        return false;
    case WM_MOUSEMOVE: {
        KillTimer(f.surface->Hwnd(), kCollapseTimer);
        if (!f.tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, f.surface->Hwnd(), 0};
            f.tracking = TrackMouseEvent(&tme) != FALSE;
        }
        if (!f.expanded)
            SetExpanded(f, true);
        if (f.moving || f.resizing) {
            POINT cur{};
            GetCursorPos(&cur);
            const float dx = float(cur.x - f.dragStartScreen.x) / s;
            const float dy = float(cur.y - f.dragStartScreen.y) / s;
            const RECT wa = WorkArea();
            const float maxW = float(wa.right - wa.left) / s;
            const float maxH = float(wa.bottom - wa.top) / s;
            if (f.moving) {
                f.x = std::clamp(f.dragStartX + dx, 0.f, std::max(0.f, maxW - f.w));
                f.y = std::clamp(f.dragStartY + dy, 0.f, std::max(0.f, maxH - kHeaderH));
            } else {
                f.w = std::clamp(f.dragStartW + dx, kMinW, std::min(kMaxW, maxW - f.x));
                f.h = std::clamp(f.dragStartH + dy, kMinH, std::min(kMaxH, maxH - f.y));
            }
            PlaceFence(f, false);
            return true;
        }
        const std::wstring btn = hitBtn();
        const int item = btn.empty() ? hitItem() : -1;
        if (btn != f.hoverBtn || item != f.hoverItem) {
            f.hoverBtn = btn;
            f.hoverItem = item;
            f.surface->Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        f.tracking = false;
        f.hoverBtn.clear();
        f.hoverItem = -1;
        f.surface->Invalidate();
        if (!f.alwaysExpanded && !f.moving && !f.resizing)
            SetTimer(f.surface->Hwnd(), kCollapseTimer, kCollapseDelayMs, nullptr);
        return true;
    case WM_MOUSEWHEEL:
        if (f.expanded && f.maxScrollY > 0.f) {
            const float delta = float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA) * 48.f;
            f.scrollY = std::clamp(f.scrollY - delta, 0.f, f.maxScrollY);
            f.surface->Invalidate();
        }
        return true;
    case WM_LBUTTONDOWN: {
        const std::wstring btn = hitBtn();
        if (!btn.empty()) {
            if (btn == L"lock") {
                f.locked = !f.locked;
                f.surface->Invalidate();
                Save();
            } else if (btn == L"pin") {
                f.alwaysExpanded = !f.alwaysExpanded;
                SetExpanded(f, f.alwaysExpanded);
                Save();
            } else if (btn == L"menu") {
                POINT cur{};
                GetCursorPos(&cur);
                ShowFenceMenu(f, cur, -1);
            }
            return true;
        }
        if (!f.locked && f.expanded && x >= f.w - kGrip && y >= f.h - kGrip) {
            f.resizing = true;
            GetCursorPos(&f.dragStartScreen);
            f.dragStartW = f.w;
            f.dragStartH = f.h;
            SetCapture(f.surface->Hwnd());
            return true;
        }
        if (!f.locked && y < kHeaderH) {
            f.moving = true;
            GetCursorPos(&f.dragStartScreen);
            f.dragStartX = f.x;
            f.dragStartY = f.y;
            SetCapture(f.surface->Hwnd());
            return true;
        }
        const int idx = hitItem();
        if (idx >= 0) {
            const ULONGLONG now = GetTickCount64();
            if (idx == f.lastClickItem && now - f.lastClickMs <= GetDoubleClickTime()) {
                f.lastClickItem = -1;
                LaunchFile(f.items[size_t(idx)].path);
            } else {
                f.lastClickItem = idx;
                f.lastClickMs = now;
            }
        }
        return true;
    }
    case WM_LBUTTONUP:
        if (f.moving || f.resizing) {
            f.moving = f.resizing = false;
            if (GetCapture() == f.surface->Hwnd())
                ReleaseCapture();
            Save();
        }
        return true;
    case WM_CAPTURECHANGED:
        if (f.moving || f.resizing) {
            f.moving = f.resizing = false;
            Save();
        }
        return false;
    case WM_RBUTTONUP: {
        POINT cur{};
        GetCursorPos(&cur);
        ShowFenceMenu(f, cur, hitItem());
        return true;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && !f.locked && f.expanded) {
            POINT cur{};
            GetCursorPos(&cur);
            const POINT pt = f.surface->ScreenToContent(cur);
            if (float(pt.x) / s >= f.w - kGrip && float(pt.y) / s >= f.h - kGrip) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                r = TRUE;
                return true;
            }
        }
        return false;
    default: break;
    }
    (void)r;
    return false;
}

} // namespace atrium
