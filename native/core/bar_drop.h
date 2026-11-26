// OLE drop target on the bar's input window.
#pragma once

#include <ole2.h>
#include <winrt/Windows.Foundation.h>

namespace atrium {

class Bar;

// OLE drop target on the input window: files dragged over an app bring it to
// the front after a moment; dropping opens them with that app.
struct BarDropTarget : winrt::implements<BarDropTarget, IDropTarget> {
    Bar* bar;
    bool files = false;
    explicit BarDropTarget(Bar* b) : bar(b) {}

    DWORD Effect(POINTL pt);
    HRESULT __stdcall DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override;
    HRESULT __stdcall DragOver(DWORD, POINTL pt, DWORD* effect) override;
    HRESULT __stdcall DragLeave() override;
    HRESULT __stdcall Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override;
};

} // namespace atrium
