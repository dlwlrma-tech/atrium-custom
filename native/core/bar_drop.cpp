#include "bar_drop.h"

#include "bar.h"

#include <shellapi.h>

#include <string>
#include <utility>
#include <vector>

namespace atrium {

DWORD BarDropTarget::Effect(POINTL pt)
{
    std::wstring key = bar->FileDragOver(POINT{pt.x, pt.y});
    return files && !key.empty() && bar->canOpenFiles && bar->canOpenFiles(key) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}
HRESULT __stdcall BarDropTarget::DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect)
{
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    files = data && data->QueryGetData(&fe) == S_OK;
    *effect = Effect(pt);
    return S_OK;
}
HRESULT __stdcall BarDropTarget::DragOver(DWORD, POINTL pt, DWORD* effect)
{
    *effect = Effect(pt);
    return S_OK;
}
HRESULT __stdcall BarDropTarget::DragLeave()
{
    bar->FileDragLeave();
    return S_OK;
}
HRESULT __stdcall BarDropTarget::Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect)
{
    *effect = Effect(pt);
    std::wstring key = bar->fileHoverKey_;
    bar->FileDragLeave();
    if (*effect == DROPEFFECT_NONE || !data)
        return S_OK;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(data->GetData(&fe, &sm)))
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
