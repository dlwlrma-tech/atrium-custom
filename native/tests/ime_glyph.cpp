#include "render/ime_glyph.h"
#include "render/ime_glyph_data.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace atrium;

namespace {

int g_failures = 0;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

} // namespace

int main()
{
    for (const wchar_t* letter : {L"中", L"あ", L"가", L"A", L"a"}) {
        const ime_glyph::Glyph* g = ime_glyph::Find(letter);
        Require(g != nullptr, "each of the five letters has an outline");
        if (!g)
            continue;
        Require(!g->cmds.empty() && g->cmds.front().op == ime_glyph::Op::Move, "an outline starts with a move");
        float left = 1, right = -1, top = 1, bottom = -1;
        for (const auto& c : g->cmds) {
            if (c.op == ime_glyph::Op::Close)
                continue;
            const float x = c.op == ime_glyph::Op::Cubic ? c.x3 : c.x1, y = c.op == ime_glyph::Op::Cubic ? c.y3 : c.y1;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
        Require(left >= -0.6f && right <= 0.6f && top >= -0.6f && bottom <= 0.6f, "an outline fits in an em");
        Require(std::abs(left + right) < 0.02f && std::abs(top + bottom) < 0.02f, "an outline is centred on its ink");
    }
    Require(ime_glyph::Find(L"中")->cmds.size() > ime_glyph::Find(L"A")->cmds.size(), "each letter has its own outline");
    Require(ime_glyph::Find(L"р") == nullptr, "other letters have none (drawn as text)");
    Require(ime_glyph::Find(L"中文") == nullptr && ime_glyph::Find(L"") == nullptr, "only a single letter has an outline");

    Require(HasImeOutline(L"中") && HasImeOutline(L"A"), "HasImeOutline matches built-in letters");
    Require(!HasImeOutline(L"р") && !HasImeOutline(L""), "HasImeOutline rejects non-outline text");

    // Verify DrawImeGlyph caches into caller-owned ImeGlyphCache and releases all COM refs on Clear().
    winrt::init_apartment();
    {
        winrt::com_ptr<ID3D11Device> d3d;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, d3d.put(),
                                        nullptr, nullptr))) {
            winrt::com_ptr<IDXGIDevice> dxgi;
            winrt::com_ptr<ID2D1Factory1> factory;
            winrt::com_ptr<ID2D1Device> d2dDevice;
            winrt::com_ptr<ID2D1DeviceContext> d2d;
            winrt::com_ptr<IDWriteFactory> dwrite;
            if (d3d.try_as(dxgi) && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put())) &&
                SUCCEEDED(factory->CreateDevice(dxgi.get(), d2dDevice.put())) &&
                SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d.put())) &&
                SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.put())))) {
                const D2D1_BITMAP_PROPERTIES1 bp =
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                winrt::com_ptr<ID2D1Bitmap1> target;
                winrt::com_ptr<ID2D1SolidColorBrush> brush;
                winrt::com_ptr<IDWriteTextFormat> format;
                if (SUCCEEDED(d2d->CreateBitmap(D2D1::SizeU(32, 32), nullptr, 0, &bp, target.put())) &&
                    SUCCEEDED(d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.put())) &&
                    SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                       16.f, L"", format.put()))) {
                    d2d->SetTarget(target.get());
                    ImeGlyphCache cache;
                    d2d->BeginDraw();
                    const D2D1_RECT_F area = D2D1::RectF(0, 0, 32, 32);
                    DrawImeGlyph(d2d.get(), dwrite.get(), format.get(), L"中", area, brush.get(), D2D1::ColorF(1, 1, 1, 1), 1.f, &cache);
                    DrawImeGlyph(d2d.get(), dwrite.get(), format.get(), L"A", area, brush.get(), D2D1::ColorF(1, 1, 1, 1), 1.f, &cache);
                    DrawImeGlyph(d2d.get(), dwrite.get(), format.get(), L"р", area, brush.get(), D2D1::ColorF(1, 1, 1, 1), 1.f, &cache);
                    DrawImeGlyph(d2d.get(), dwrite.get(), format.get(), L"あ", area, brush.get(), D2D1::ColorF(1, 1, 1, 1), 1.f, nullptr);
                    Require(SUCCEEDED(d2d->EndDraw()), "DrawImeGlyph renders outlines and fallback text");
                    Require(cache.factory.get() == factory.get(), "ImeGlyphCache binds to the device factory");
                    Require(cache.byLetter.size() == 2 && cache.byLetter.contains(L'中') && cache.byLetter.contains(L'A'),
                            "ImeGlyphCache stores only outline glyphs drawn through it");
                    d2d->SetTarget(nullptr);
                    brush = nullptr;
                    target = nullptr;
                    d2d = nullptr;
                    d2dDevice = nullptr;
                    cache.Clear();
                    Require(!cache.factory && cache.byLetter.empty(), "ImeGlyphCache::Clear drops all cached COM objects");
                    factory->AddRef();
                    const ULONG refs = factory->Release();
                    Require(refs == 1, "no thread_local COM reference keeps the D2D factory alive");
                }
            }
        }
    }
    winrt::uninit_apartment();

    std::cout << (g_failures ? "ime_glyph: FAILED" : "ime_glyph: ok") << '\n';
    return g_failures ? 1 : 0;
}
