#include "bar_drop.h"

#include "bar.h"
#include "bar_entries.h"

#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <utility>
#include <vector>

namespace atrium {

bool BarDropTarget::EnsureDesktopTarget()
{
    if (desktopTarget)
        return true;
    winrt::com_ptr<IShellFolder> desktop;
    if (FAILED(SHGetDesktopFolder(desktop.put())) || !desktop ||
        FAILED(desktop->CreateViewObject(bar->input_, __uuidof(IDropTarget), desktopTarget.put_void())) || !desktopTarget)
        return false;
    return true;
}

void BarDropTarget::LeaveDesktopTarget()
{
    if (desktopEntered && desktopTarget)
        desktopTarget->DragLeave();
    desktopEntered = false;
    desktopTarget = nullptr;
}

DWORD BarDropTarget::Effect(DWORD keys, POINTL pt)
{
    const std::wstring key = bar->FileDragOver(POINT{pt.x, pt.y});
    if (!files)
        return DROPEFFECT_NONE;
    if (key == kDesktopKey && EnsureDesktopTarget() && data) {
        DWORD effect = DROPEFFECT_NONE;
        HRESULT hr = S_OK;
        if (desktopEntered)
            hr = desktopTarget->DragOver(keys, pt, &effect);
        else {
            hr = desktopTarget->DragEnter(data.get(), keys, pt, &effect);
            desktopEntered = SUCCEEDED(hr);
        }
        return SUCCEEDED(hr) ? effect : DROPEFFECT_NONE;
    }
    LeaveDesktopTarget();
    return !key.empty() && bar->canOpenFiles && bar->canOpenFiles(key) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}
HRESULT __stdcall BarDropTarget::DragEnter(IDataObject* object, DWORD keys, POINTL pt, DWORD* effect)
{
    LeaveDesktopTarget();
    data = nullptr;
    if (object)
        data.copy_from(object);
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    files = object && object->QueryGetData(&fe) == S_OK;
    *effect = Effect(keys, pt);
    return S_OK;
}
HRESULT __stdcall BarDropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
    *effect = Effect(keys, pt);
    return S_OK;
}
HRESULT __stdcall BarDropTarget::DragLeave()
{
    LeaveDesktopTarget();
    data = nullptr;
    files = false;
    bar->FileDragLeave();
    return S_OK;
}
HRESULT __stdcall BarDropTarget::Drop(IDataObject* object, DWORD keys, POINTL pt, DWORD* effect)
{
    *effect = Effect(keys, pt);
    if (bar->fileHoverKey_ == kDesktopKey && desktopEntered && desktopTarget) {
        const HRESULT hr = desktopTarget->Drop(object, keys, pt, effect);
        LeaveDesktopTarget();
        data = nullptr;
        files = false;
        bar->FileDragLeave();
        return hr;
    }
    std::wstring key = bar->fileHoverKey_;
    LeaveDesktopTarget();
    data = nullptr;
    files = false;
    bar->FileDragLeave();
    if (*effect == DROPEFFECT_NONE || !object)
        return S_OK;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(object->GetData(&fe, &sm)))
        return S_OK;
    std::vector<std::wstring> paths;
    if (auto* drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && i < 64; ++i) {
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
    if (!paths.empty() && bar->onDropFiles)
        bar->onDropFiles(key, paths);
    return S_OK;
}

} // namespace atrium
