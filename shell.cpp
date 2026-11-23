#include "formats.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <stdexcept>

namespace {
// These CLSIDs are also used by the registration script.
constexpr GUID PreviewClsid{0x8fa42d75, 0x2473, 0x4d80, {0x93, 0xba, 0xd7, 0x3b, 0xe4, 0xa5, 0x77, 0xc2}};
constexpr GUID ThumbnailClsid{0x7746619e, 0x43c9, 0x4fd6, {0x8a, 0xac, 0x78, 0x7e, 0x3f, 0x39, 0x8a, 0xb1}};
constexpr GUID PlateThumbnailClsid{0xc3930db2, 0xad88, 0x4e7f, {0x92, 0x0b, 0x47, 0x54, 0x4e, 0x80, 0x5c, 0x39}};
constexpr uint64_t ShellBytes = 64ull * 1024 * 1024;
constexpr stl::View Overview{0.65, 0.62};
constexpr int Supersample = 2;
// Explorer previews and thumbnails keep the light-blue model regardless of the viewer setting.
constexpr stl::Palette ShellPalette{RGB(18, 26, 40), RGB(105, 217, 245)};

// Renders a fitted, anti-aliased overview. Transparent frames are premultiplied ARGB.
stl::Frame RenderOverview(const stl::Mesh& mesh, int side, double fill, bool transparent) {
    const auto view = stl::FitView(mesh, Overview, fill);
    const auto frame = stl::Render(mesh, side * Supersample, side * Supersample, view, stl::MaxFaces,
                                   24'000'000, ShellPalette, transparent);
    return stl::Downsample(frame, Supersample);
}
std::atomic<long> live{};

class RefCounted {
public:
    RefCounted() { ++live; }
protected:
    virtual ~RefCounted() { --live; }
    ULONG Retain() { return ++refs; }
    ULONG Drop() {
        const ULONG remaining = --refs;
        if (!remaining) delete this;
        return remaining;
    }
private:
    std::atomic<ULONG> refs{1};
};

class Input {
public:
    ~Input() { Clear(); }
    HRESULT Set(IStream* source) {
        if (!source) return E_POINTER;
        if (stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream = source;
        stream->AddRef();
        return S_OK;
    }
    void Clear() {
        if (stream) stream->Release();
        stream = nullptr;
    }
    IStream* Get() const { return stream; }
    stl::Mesh Read() {
        if (!stream) throw std::runtime_error("No stream.");
        STATSTG stat{};
        if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) throw std::runtime_error("No stream size.");
        if (stat.cbSize.QuadPart > ShellBytes) throw std::length_error("Preview exceeds 64 MiB.");
        LARGE_INTEGER start{};
        if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) throw std::runtime_error("Unseekable STL.");
        const auto length = static_cast<ULONG>(stat.cbSize.QuadPart);
        std::vector<char> bytes(length);
        for (ULONG offset = 0; offset < length;) {
            ULONG received{};
            const ULONG requested = std::min<ULONG>(length - offset, 1 << 20);
            if (FAILED(stream->Read(bytes.data() + offset, requested, &received)) || received == 0 ||
                received > requested) throw std::runtime_error("Incomplete STL stream.");
            offset += received;
        }
        return stl::Parse(bytes);
    }
private:
    IStream* stream{};
};

class Preview final : public IPreviewHandler, public IInitializeWithStream, public IObjectWithSite,
                      private RefCounted {
public:
    IFACEMETHODIMP QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id == IID_IUnknown || id == IID_IPreviewHandler) *output = static_cast<IPreviewHandler*>(this);
        else if (id == IID_IInitializeWithStream) *output = static_cast<IInitializeWithStream*>(this);
        else if (id == IID_IObjectWithSite) *output = static_cast<IObjectWithSite*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return Retain(); }
    IFACEMETHODIMP_(ULONG) Release() override { return Drop(); }
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override { return input.Set(stream); }
    IFACEMETHODIMP SetWindow(HWND parentWindow, const RECT* client) override {
        if (!parentWindow || !client) return E_INVALIDARG;
        parent = parentWindow;
        if (window) SetParent(window, parent);
        return SetRect(client);
    }
    IFACEMETHODIMP SetRect(const RECT* client) override {
        if (!client) return E_POINTER;
        bounds = *client;
        if (window) {
            MoveWindow(window, bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
                       std::max(0L, bounds.bottom - bounds.top), TRUE);
            InvalidateRect(window, nullptr, FALSE);
        }
        return S_OK;
    }
    IFACEMETHODIMP DoPreview() override {
        if (!parent || window) return E_UNEXPECTED;
        error = nullptr;
        try {
            const auto mesh = input.Read();
            frame = RenderOverview(mesh, 400, 0.86, false);
        } catch (const std::length_error&) {
            error = L"檔案超過 64 MiB，請用 STL Quick View 開啟。";
        } catch (...) {
            error = L"無法預覽這個 STL 檔案。";
        }
        input.Clear();
        window = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                                0, 0, 1, 1, parent, nullptr, nullptr, nullptr);
        if (!window) return HRESULT_FROM_WIN32(GetLastError());
        if (!SetWindowSubclass(window, Paint, 1, reinterpret_cast<DWORD_PTR>(this))) {
            DestroyWindow(window);
            window = nullptr;
            return E_FAIL;
        }
        SetRect(&bounds);
        ShowWindow(window, SW_SHOWNA);
        return S_OK;
    }
    IFACEMETHODIMP Unload() override {
        if (window) DestroyWindow(window);
        window = nullptr;
        input.Clear();
        frame = {};
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override {
        if (!window) return S_FALSE;
        ::SetFocus(window);
        return S_OK;
    }
    IFACEMETHODIMP QueryFocus(HWND* focused) override {
        if (!focused) return E_POINTER;
        *focused = GetFocus();
        return S_OK;
    }
    IFACEMETHODIMP TranslateAccelerator(MSG* message) override {
        if (!message) return E_POINTER;
        IPreviewHandlerFrame* host{};
        if (!site || FAILED(site->QueryInterface(IID_PPV_ARGS(&host)))) return S_FALSE;
        const auto result = host->TranslateAccelerator(message);
        host->Release();
        return result;
    }
    IFACEMETHODIMP SetSite(IUnknown* newSite) override {
        if (newSite) newSite->AddRef();
        if (site) site->Release();
        site = newSite;
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return site ? site->QueryInterface(id, output) : E_FAIL;
    }
private:
    ~Preview() override { Unload(); SetSite(nullptr); }

    static LRESULT CALLBACK Paint(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                  UINT_PTR id, DWORD_PTR ref) {
        auto& preview = *reinterpret_cast<Preview*>(ref);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, Paint, id);
            preview.window = nullptr;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message != WM_PAINT) return DefSubclassProc(window, message, wp, lp);
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        SetDCBrushColor(dc, RGB(18, 26, 40));
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (preview.error) {
            InflateRect(&client, -14, 0);
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(223, 235, 246));
            RECT textBounds = client;
            constexpr UINT wrap = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
            DrawTextW(dc, preview.error, -1, &textBounds, wrap | DT_CALCRECT);
            client.top = std::max(0L, (client.bottom - textBounds.bottom) / 2);
            DrawTextW(dc, preview.error, -1, &client, wrap);
        } else {
            const LONG side = std::min(client.right, client.bottom);
            const LONG x = (client.right - side) / 2, y = (client.bottom - side) / 2;
            const RECT output{x, y, x + side, y + side};
            SetStretchBltMode(dc, HALFTONE);
            stl::PaintFrame(dc, output, preview.frame);
        }
        EndPaint(window, &paint);
        return 0;
    }

    Input input;
    IUnknown* site{};
    HWND parent{}, window{};
    RECT bounds{};
    stl::Frame frame;
    const wchar_t* error{};
};

HBITMAP ThumbnailBitmap(UINT width, UINT height, void** bits) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, bits, nullptr, 0);
}

HRESULT DecodePlateThumbnail(std::string& png, UINT side, HBITMAP* bitmap) {
    if (png.empty()) return E_FAIL;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    if (FAILED(hr = factory->CreateStream(&stream)) ||
        FAILED(hr = stream->InitializeFromMemory(reinterpret_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))) ||
        FAILED(hr = factory->CreateDecoder(GUID_ContainerFormatPng, &GUID_VendorMicrosoft, &decoder)) ||
        FAILED(hr = decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand)) ||
        FAILED(hr = decoder->GetFrame(0, &frame))) return hr;

    UINT width{}, height{};
    if (FAILED(hr = frame->GetSize(&width, &height))) return hr;
    if (!width || !height || width > 2048 || height > 2048) return E_FAIL;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter)) ||
        FAILED(hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return hr;
    IWICBitmapSource* pixels = converter.Get();
    ComPtr<IWICBitmapScaler> scaler;
    const UINT edge = std::max(width, height);
    if (edge > side) {
        width = std::max(1u, width * side / edge);
        height = std::max(1u, height * side / edge);
        if (FAILED(hr = factory->CreateBitmapScaler(&scaler)) ||
            FAILED(hr = scaler->Initialize(pixels, width, height, WICBitmapInterpolationModeFant))) return hr;
        pixels = scaler.Get();
    }
    void* bits{};
    const auto image = ThumbnailBitmap(width, height, &bits);
    if (!image) return E_OUTOFMEMORY;
    hr = pixels->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(bits));
    if (FAILED(hr)) {
        DeleteObject(image);
        return hr;
    }
    *bitmap = image;
    return S_OK;
}

class Thumbnail final : public IThumbnailProvider, public IInitializeWithStream, private RefCounted {
public:
    explicit Thumbnail(bool firstPlate) : firstPlate(firstPlate) {}
    IFACEMETHODIMP QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id == IID_IUnknown || id == IID_IThumbnailProvider) *output = static_cast<IThumbnailProvider*>(this);
        else if (id == IID_IInitializeWithStream) *output = static_cast<IInitializeWithStream*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return Retain(); }
    IFACEMETHODIMP_(ULONG) Release() override { return Drop(); }
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override { return input.Set(stream); }
    IFACEMETHODIMP GetThumbnail(UINT requested, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
        if (!bitmap || !alpha) return E_POINTER;
        *bitmap = nullptr;
        *alpha = WTSAT_UNKNOWN;
        if (!requested) return E_INVALIDARG;
        try {
            if (firstPlate) {
                if (!input.Get()) return E_UNEXPECTED;
                auto png = model::ReadFirstPlateImage(*input.Get());
                const auto result = DecodePlateThumbnail(png, std::min(requested, 256u), bitmap);
                if (SUCCEEDED(result)) *alpha = WTSAT_ARGB;
                return result;
            }
            const auto mesh = input.Read();
            const int side = static_cast<int>(std::min(requested, 256u));
            const auto frame = RenderOverview(mesh, side, 0.94, true);
            void* bits{};
            *bitmap = ThumbnailBitmap(side, side, &bits);
            if (!*bitmap) return E_OUTOFMEMORY;
            std::memcpy(bits, frame.pixels.data(), frame.pixels.size() * sizeof(uint32_t));
            *alpha = WTSAT_ARGB;
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }
private:
    bool firstPlate;
    Input input;
};

class Factory final : public IClassFactory, private RefCounted {
public:
    explicit Factory(REFCLSID clsid) : clsid(clsid) {}
    IFACEMETHODIMP QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id != IID_IUnknown && id != IID_IClassFactory) return E_NOINTERFACE;
        *output = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return Retain(); }
    IFACEMETHODIMP_(ULONG) Release() override { return Drop(); }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        IUnknown* instance{};
        if (clsid == PreviewClsid) instance = static_cast<IPreviewHandler*>(new (std::nothrow) Preview);
        else instance = static_cast<IThumbnailProvider*>(new (std::nothrow) Thumbnail(clsid == PlateThumbnailClsid));
        if (!instance) return E_OUTOFMEMORY;
        const auto result = instance->QueryInterface(id, output);
        instance->Release();
        return result;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++live;
        else --live;
        return S_OK;
    }
private:
    GUID clsid;
};
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID id, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != PreviewClsid && clsid != ThumbnailClsid && clsid != PlateThumbnailClsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) Factory(clsid);
    if (!factory) return E_OUTOFMEMORY;
    const auto result = factory->QueryInterface(id, output);
    factory->Release();
    return result;
}

STDAPI DllCanUnloadNow() { return live == 0 ? S_OK : S_FALSE; }
