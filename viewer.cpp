#include "mesh.h"
#include "version.h"
#include "render.h"
#include "formats.h"
#include <commdlg.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// Ask hybrid-graphics laptops for the discrete GPU from the first frame, instead of
// waiting for the NVIDIA/AMD driver to build an application profile.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {
constexpr WORD OpenButton = 101, ResetButton = 102, SettingsButton = 104, AxesCommand = 105, GridCommand = 106,
               ConfirmButton = 108, CancelLoadButton = 109;
constexpr UINT_PTR AnimTimer = 107;
constexpr UINT LoadDone = WM_APP + 1, RendererLost = WM_APP + 2;
constexpr double Pi = 3.141592653589793, HalfTurn = Pi, QuarterTurn = Pi / 2, FullTurn = 2 * Pi;

struct Unit { const wchar_t* menu; const wchar_t* suffix; };
constexpr Unit Units[] = {
    {L"毫米 (mm)", L" mm"}, {L"公分 (cm)", L" cm"}, {L"公尺 (m)", L" m"}, {L"英寸 (in)", L" in"}, {L"不顯示單位", L""}
};

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value{}, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, stl::SettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size)
        == ERROR_SUCCESS ? value : fallback;
}
void WriteSetting(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, stl::SettingsKey, name, REG_DWORD, &value, sizeof(value));
}
constexpr wchar_t AppClass[] = L"StlQuickView.Window";
constexpr wchar_t CanvasClass[] = L"StlQuickView.Canvas";
constexpr wchar_t CubeClass[] = L"StlQuickView.Cube";
constexpr wchar_t SurfaceClass[] = L"StlQuickView.Surface";
constexpr wchar_t PartsClass[] = L"StlQuickView.Parts";
constexpr COLORREF Background = RGB(18, 26, 40);
constexpr COLORREF Panel = RGB(24, 35, 52);
constexpr COLORREF Primary = RGB(232, 241, 251);
constexpr COLORREF Secondary = RGB(168, 189, 210);

struct LoadResult {
    std::atomic<bool> ready{};
    model::Model model;
    std::vector<render::Vertex> vertices;
    std::wstring error;
    // The file is over the normal limits: ask before loading it with the extended ones.
    bool tooLarge{};
    uint64_t faces{}, bytes{};
};

// Renderer preference, stored as the "Renderer" setting.
enum : UINT { RendererAuto = 0, RendererD3D11 = 1, RendererOpenGL = 2 };
struct RendererOption { const wchar_t* menu; };
constexpr RendererOption RendererOptions[] = {
    {L"自動（Direct3D 11 優先）"}, {L"Direct3D 11"}, {L"OpenGL"}
};

struct App {
    HWND window{}, canvas{}, surface{}, cube{}, parts{}, open{}, reset{}, settings{};
    HFONT bodyFont{}, titleFont{};
    HBRUSH panelBrush{};
    UINT dpi = 96;
    std::unique_ptr<render::Renderer> renderer;   // draws into `surface`; null means CPU fallback
    UINT rendererChoice = std::min<UINT>(ReadSetting(L"Renderer", RendererAuto), RendererOpenGL);
    bool gpuModel{};                   // the current model lives on the GPU
    stl::Mesh mesh;                    // triangles are freed once the GPU owns the geometry
    size_t faces{};
    std::vector<model::Part> partList; // at least one part once a model is loaded
    int selectedPart = -1;             // -1 shows every part
    bool hasColor{};                   // the file carries colours we did not load
    std::wstring format, fileUnit;     // e.g. L"3MF", L" mm" (empty when the file has no unit)
    int partsScroll{}, partsHover = -2;   // -2: nothing hovered, -1: "全部顯示"
    stl::View view;
    stl::Palette palette = stl::ReadPalette();
    std::wstring path, filename, message, queued;
    bool keepView{};                   // reload after a renderer switch keeps the camera
    HWND confirm{}, cancelLoad{};      // "load anyway" / "cancel" for oversized files
    bool confirmPending{};             // waiting for the user to confirm an oversized load
    bool extendedLoad{};               // the current file was loaded with the extended limits
    std::shared_ptr<LoadResult> loading;
    POINT last{};
    UINT drag{};
    bool showAxes = ReadSetting(L"ShowAxes", 1) != 0;
    bool showGrid = ReadSetting(L"ShowGrid", 1) != 0;
    UINT unit = std::min<UINT>(ReadSetting(L"Unit", 0), ARRAYSIZE(Units) - 1);
    stl::View animFrom, animTo;
    ULONGLONG animStart{};
    bool animating{};
    int hoverFace = -1;
    // View-cube drawing objects, created once (font creation is slow enough to stall dragging).
    std::unique_ptr<Gdiplus::FontFamily> cubeFamily;
    std::unique_ptr<Gdiplus::Font> cubeFont;
    std::unique_ptr<Gdiplus::StringFormat> cubeFormat;
    std::unique_ptr<Gdiplus::FontFamily> partsFamily;
    std::unique_ptr<Gdiplus::Font> partsFont, partsBold, partsSmall;
    int Px(int n) const { return MulDiv(n, dpi, 96); }
};

void LayoutParts(App& app);

// Read-only memory mapping of the whole file: no heap copy before parsing.
class MappedFile {
public:
    MappedFile(const std::wstring& path, uint64_t limit) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("無法開啟這個檔案。");
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file, &length) || length.QuadPart < 0) throw std::runtime_error("無法讀取檔案大小。");
        if (uint64_t(length.QuadPart) > limit) throw stl::TooLarge("檔案超過大小上限。", 0, uint64_t(length.QuadPart));
        size = size_t(length.QuadPart);
        if (size == 0) return;
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) throw std::runtime_error("讀取 STL 失敗。");
        view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (!view) throw std::runtime_error("讀取 STL 失敗。");
    }
    ~MappedFile() {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    std::span<const char> Bytes() const { return {static_cast<const char*>(view), view ? size : 0}; }

private:
    HANDLE file = INVALID_HANDLE_VALUE, mapping{};
    const void* view{};
    size_t size{};
};

std::vector<render::Vertex> BuildVertices(const stl::Mesh& mesh) {
    std::vector<render::Vertex> vertices(mesh.triangles.size() * 3);
    render::Vertex* out = vertices.data();
    for (const auto& t : mesh.triangles) {
        const float ax = t.b.x - t.a.x, ay = t.b.y - t.a.y, az = t.b.z - t.a.z;
        const float bx = t.c.x - t.a.x, by = t.c.y - t.a.y, bz = t.c.z - t.a.z;
        const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        const stl::Point n = length > 0 ? stl::Point{nx / length, ny / length, nz / length}
                                        : stl::Point{0, 0, 1};
        *out++ = {t.a, n};
        *out++ = {t.b, n};
        *out++ = {t.c, n};
    }
    return vertices;
}

void ReadModel(const std::wstring& path, const std::shared_ptr<LoadResult>& result, HWND notify,
               bool extended) noexcept {
    try {
        const stl::Limits limits = extended ? stl::ExtendedLimits : stl::Limits{};
        {
            const MappedFile file(path, limits.bytes);
            result->model = model::Load(path, file.Bytes(), 0, limits);
        }
        result->vertices = BuildVertices(result->model.mesh);
    } catch (const stl::TooLarge& error) {
        if (extended) {
            result->error = L"這個模型超過可載入的上限（3,000 萬個三角面或 2 GiB）。";
        } else {
            result->tooLarge = true;
            result->faces = error.faces;
            std::error_code ignored;
            result->bytes = std::filesystem::file_size(std::filesystem::path(path), ignored);
        }
    } catch (const std::bad_alloc&) {
        result->error = L"記憶體不足，無法載入這個模型。";
    } catch (const std::exception& error) {
        wchar_t detail[256]{};
        MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, detail, ARRAYSIZE(detail));
        result->error = detail;
    }
    result->ready.store(true, std::memory_order_release);
    PostMessageW(notify, LoadDone, 0, 0);
}

void ShowSurface(App& app) {
    if (app.surface) ShowWindow(app.surface, app.gpuModel ? SW_SHOWNA : SW_HIDE);
}

void Refresh(App& app) {
    InvalidateRect(app.window, nullptr, FALSE);
    InvalidateRect(app.canvas, nullptr, FALSE);
    if (app.surface) InvalidateRect(app.surface, nullptr, FALSE);
    InvalidateRect(app.cube, nullptr, FALSE);
}

// Repaints just the 3D view.
void RedrawModel(App& app) {
    InvalidateRect(app.gpuModel ? app.surface : app.canvas, nullptr, FALSE);
}

void DestroySurface(App& app) {
    app.renderer.reset();
    app.gpuModel = false;
    if (app.surface) DestroyWindow(app.surface);
    app.surface = nullptr;
}

// Builds the renderer, falling back in order. Each attempt gets a fresh child window, because
// an OpenGL pixel format can only be set once per window.
void CreateRenderer(App& app) {
    DestroySurface(app);
    struct Attempt { bool d3d, software; };
    std::vector<Attempt> order;
    if (app.rendererChoice == RendererOpenGL) order = {{false, false}, {true, false}, {false, true}, {true, true}};
    else if (app.rendererChoice == RendererD3D11) order = {{true, false}, {true, true}, {false, true}};
    else order = {{true, false}, {false, false}, {true, true}, {false, true}};
    RECT area{};
    GetClientRect(app.canvas, &area);
    for (const auto attempt : order) {
        const HWND surface = CreateWindowExW(0, SurfaceClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                                             std::max(1L, area.right), std::max(1L, area.bottom), app.canvas,
                                             nullptr, GetModuleHandleW(nullptr), &app);
        if (!surface) return;
        auto renderer = attempt.d3d ? render::CreateD3D11(surface, attempt.software)
                                    : render::CreateOpenGL(surface, attempt.software);
        if (renderer) {
            app.surface = surface;
            app.renderer = std::move(renderer);
            return;
        }
        DestroyWindow(surface);
    }
}

void ShowConfirm(App& app, bool show) {
    app.confirmPending = show;
    for (const HWND button : {app.confirm, app.cancelLoad}) {
        if (!button) continue;
        if (show) SetWindowPos(button, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        else ShowWindow(button, SW_HIDE);
    }
}

// Explains why a file needs confirmation, with a memory estimate when the size is known.
std::wstring OversizeMessage(uint64_t faces, uint64_t bytes) {
    const auto grouped = [](uint64_t value) {
        std::wstring text = std::to_wstring(value);
        for (int i = int(text.size()) - 3; i > 0; i -= 3) text.insert(size_t(i), 1, L',');
        return text;
    };
    std::wstring text;
    if (faces) text = L"這個模型有 " + grouped(faces) + L" 個三角面，超過建議上限 200 萬。";
    else if (bytes > stl::MaxBytes) text = L"這個檔案有 " + grouped(bytes >> 20) + L" MiB，超過建議上限 128 MiB。";
    else text = L"這個模型超過建議上限 200 萬個三角面。";
    // Roughly 200 bytes per face: triangles, GPU vertices and the driver's copy.
    const uint64_t estimate = faces ? faces * 200 : bytes ? bytes * 4 : 0;
    if (estimate) {
        wchar_t line[160]{};
        swprintf_s(line, L"預估需要約 %.1f GB 記憶體", estimate / 1073741824.0);
        text += line;
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory) && estimate > memory.ullAvailPhys) {
            swprintf_s(line, L"，目前可用約 %.1f GB，可能不足", memory.ullAvailPhys / 1073741824.0);
            text += line;
        }
        text += L"。";
    }
    return text + L"載入需要較多時間，確定要載入嗎？";
}

void OpenModel(App& app, std::wstring path, bool extended = false) {
    if (app.loading) {
        app.queued = std::move(path);
        return;
    }
    ShowConfirm(app, false);
    app.extendedLoad = extended;
    try {
        app.filename = std::filesystem::path(path).filename().wstring();
        app.path = path;
        app.mesh = {};
        app.faces = 0;
        app.partList.clear();
        app.selectedPart = -1;
        app.hasColor = false;
        app.gpuModel = false;
        ShowWindow(app.parts, SW_HIDE);
        if (app.renderer) app.renderer->Upload({});
        ShowSurface(app);
        auto result = std::make_shared<LoadResult>();
        std::thread(ReadModel, std::move(path), result, app.window, extended).detach();
        app.loading = std::move(result);
        app.message = L"正在載入模型…";
    } catch (const std::exception&) {
        app.message = L"無法開始載入模型。";
    }
    EnableWindow(app.reset, FALSE);
    Refresh(app);
}

void FinishLoad(App& app) {
    if (!app.loading || !app.loading->ready.load(std::memory_order_acquire)) return;
    auto result = std::move(app.loading);
    if (!app.queued.empty()) {
        auto path = std::move(app.queued);
        app.queued.clear();
        OpenModel(app, std::move(path));
        return;
    }
    if (result->tooLarge) {
        app.message = OversizeMessage(result->faces, result->bytes);
        ShowConfirm(app, true);
        EnableWindow(app.reset, FALSE);
        Refresh(app);
        return;
    }
    auto& loaded = result->model;
    app.mesh = std::move(loaded.mesh);
    app.faces = app.mesh.triangles.size();
    app.partList = std::move(loaded.parts);
    app.hasColor = loaded.hasColor;
    app.format = loaded.format;
    app.fileUnit = loaded.unit;
    app.selectedPart = -1;
    app.partsScroll = 0;
    app.gpuModel = app.faces > 0 && app.renderer && app.renderer->Upload(std::move(result->vertices));
    // The CPU renderer is the only other reader of the triangles.
    if (app.gpuModel) std::vector<stl::Triangle>{}.swap(app.mesh.triangles);
    app.message = std::move(result->error);
    if (!app.keepView) app.view = {};
    app.keepView = false;
    EnableWindow(app.reset, app.faces > 0);
    ShowSurface(app);
    LayoutParts(app);
    Refresh(app);
}

// Rebuilds the renderer (after a settings change or a lost device) and reloads the model.
void RestartRenderer(App& app) {
    CreateRenderer(app);
    if (!app.path.empty() && !app.loading && !app.confirmPending) {
        app.keepView = true;
        OpenModel(app, app.path, app.extendedLoad);
    }
    Refresh(app);
}

void Browse(App& app) {
    std::wstring path(32768, L'\0');
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = app.window;
    dialog.lpstrFilter = model::DialogFilter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        path.resize(wcslen(path.c_str()));
        OpenModel(app, std::move(path));
    }
}

void DrawTextLine(HDC dc, std::wstring_view text, RECT box, HFONT font, COLORREF color, UINT flags) {
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &box, DT_NOPREFIX | flags);
    SelectObject(dc, previous);
}

// The unit the on-screen numbers are in: a unit stated in the file (3MF) wins over the setting.
enum class UnitKind { Metric, Inch, Foot };

const wchar_t* UnitSuffix(const App& app) {
    return !app.fileUnit.empty() ? app.fileUnit.c_str() : Units[app.unit].suffix;
}

UnitKind EffectiveUnit(const App& app) {
    const std::wstring_view suffix = UnitSuffix(app);
    if (suffix == L" in") return UnitKind::Inch;
    if (suffix == L" ft") return UnitKind::Foot;
    return UnitKind::Metric;
}

// Grid spacing that reads naturally in the unit, and how often a line is emphasised:
// metric 1-2-5 steps with heavier lines every 5 or 10 units; inches in binary fractions with
// heavier lines on whole inches (or every foot); feet in inch fractions up to round feet.
struct GridSpacing {
    double step;
    int major;
};

GridSpacing MetricGrid(double raw) {
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    if (raw <= magnitude) return {magnitude, 5};
    if (raw <= 2 * magnitude) return {2 * magnitude, 5};
    if (raw <= 5 * magnitude) return {5 * magnitude, 2};
    return {10 * magnitude, 5};
}

GridSpacing NiceGrid(double raw, UnitKind kind) {
    if (!(raw > 0)) return {1, 5};
    if (kind == UnitKind::Inch) {
        static constexpr double steps[] = {1.0 / 64, 1.0 / 32, 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2,
                                           1, 2, 3, 6, 12, 24, 36, 60, 120, 240, 600, 1200};
        for (const double step : steps) {
            if (step < raw) continue;
            const int major = step < 1 ? int(std::lround(1 / step)) : step < 12 ? int(std::lround(12 / step)) : 5;
            return {step, std::max(2, major)};
        }
        return {MetricGrid(raw / 12).step * 12, 5};
    }
    if (kind == UnitKind::Foot) {
        static constexpr double steps[] = {1.0 / 12, 1.0 / 6, 1.0 / 4, 1.0 / 2, 1, 2, 5, 10, 20, 50, 100};
        for (const double step : steps)
            if (step >= raw) return {step, step < 1 ? std::max(2, int(std::lround(1 / step))) : 5};
    }
    return MetricGrid(raw);
}

// Inches below one read better as fractions ("1/4"); everything else uses plain decimals.
std::wstring FormatLength(double value, UnitKind kind) {
    if (kind == UnitKind::Inch && value < 1) {
        for (int denominator = 2; denominator <= 64; denominator *= 2) {
            const double numerator = value * denominator;
            if (std::abs(numerator - std::round(numerator)) < 1e-9)
                return std::to_wstring(std::lround(numerator)) + L"/" + std::to_wstring(denominator);
        }
    }
    wchar_t text[32]{};
    swprintf_s(text, L"%.5g", value);
    return text;
}

// What is on screen: the whole model, or the selected part shown on its own (centred and
// rescaled as if it had been opened by itself).
struct ShownFrame {
    double center[3], size[3], diameter;   // file units
    stl::Point offset;                     // placement in normalised model space
    float scale;
};

ShownFrame Shown(const App& app) {
    const auto& m = app.mesh;
    ShownFrame f{{m.center[0], m.center[1], m.center[2]}, {m.width, m.depth, m.height}, m.diameter, {}, 1};
    if (app.selectedPart < 0 || size_t(app.selectedPart) >= app.partList.size()) return f;
    const auto& part = app.partList[size_t(app.selectedPart)];
    const double mid[3] = {(part.min.x + part.max.x) * 0.5, (part.min.y + part.max.y) * 0.5,
                           (part.min.z + part.max.z) * 0.5};
    const double dx = part.max.x - part.min.x, dy = part.max.y - part.min.y, dz = part.max.z - part.min.z;
    const double length = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-9);
    for (int i = 0; i < 3; ++i) {
        f.center[i] = m.center[i] + mid[i] * m.diameter;
        f.size[i] = part.size[i];
    }
    f.diameter = length * m.diameter;
    f.offset = {float(mid[0]), float(mid[1]), float(mid[2])};
    f.scale = float(1 / length);
    return f;
}

GridSpacing GridFor(const App& app) {
    if (!app.showGrid || app.faces == 0) return {0, 5};
    const auto f = Shown(app);
    return NiceGrid(std::max({f.size[0], f.size[1], f.diameter * 0.2}) * 1.6 / 12, EffectiveUnit(app));
}

// Everything the GPU back ends need for one frame.
render::Scene BuildScene(const App& app, int width, int height) {
    render::Scene scene;
    scene.view = app.view;
    scene.width = width;
    scene.height = height;
    scene.background = app.palette.background;
    scene.model = app.palette.model;
    if (app.selectedPart >= 0 && size_t(app.selectedPart) < app.partList.size()) {
        const auto& part = app.partList[size_t(app.selectedPart)];
        scene.first = uint32_t(part.first * 3);
        scene.count = uint32_t(part.count * 3);
    }
    const auto& m = app.mesh;
    const auto shown = Shown(app);
    scene.offset = shown.offset;
    scene.scale = shown.scale;
    // Floor grid under what is shown, aligned to the file's coordinate system.
    if (const auto grid = GridFor(app); grid.step > 0) {
        const double step = grid.step;
        const double half = std::max({shown.size[0], shown.size[1], shown.diameter * 0.2}) * 0.8;
        const long long x0 = std::llround(std::floor((shown.center[0] - half) / step));
        const long long x1 = std::llround(std::ceil((shown.center[0] + half) / step));
        const long long y0 = std::llround(std::floor((shown.center[1] - half) / step));
        const long long y1 = std::llround(std::ceil((shown.center[1] + half) / step));
        const double floorZ = shown.center[2] - shown.size[2] / 2 - shown.diameter * 0.002;
        const auto bg = app.palette.background;
        const bool dark = 299 * GetRValue(bg) + 587 * GetGValue(bg) + 114 * GetBValue(bg) < 128000;
        const float tone = dark ? 1.f : 0.f;
        const auto line = [&](double ax, double ay, double bx, double by, long long index) {
            const float alpha = index % grid.major == 0 ? 0.20f : 0.08f;
            const auto a = m.ToModel(ax, ay, floorZ), b = m.ToModel(bx, by, floorZ);
            scene.grid.push_back({a.x, a.y, a.z, tone, tone, tone, alpha});
            scene.grid.push_back({b.x, b.y, b.z, tone, tone, tone, alpha});
        };
        for (long long i = x0; i <= x1; ++i) line(i * step, y0 * step, i * step, y1 * step, i);
        for (long long i = y0; i <= y1; ++i) line(x0 * step, i * step, x1 * step, i * step, i);
    }
    // Axes at the file origin, or at the shown box's corner when the origin is far away.
    scene.axes = app.showAxes;
    scene.axisLength = 0.42f / shown.scale;
    scene.origin = m.ToModel(0, 0, 0);
    const stl::Point o{(scene.origin.x - shown.offset.x) * shown.scale, (scene.origin.y - shown.offset.y) * shown.scale,
                       (scene.origin.z - shown.offset.z) * shown.scale};
    if (std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z) > 0.9f)
        scene.origin = m.ToModel(shown.center[0] - shown.size[0] / 2, shown.center[1] - shown.size[1] / 2,
                                 shown.center[2] - shown.size[2] / 2);
    return scene;
}

void DrawScene(App& app) {
    if (!app.renderer || !app.gpuModel) return;
    RECT area{};
    GetClientRect(app.surface, &area);
    if (!app.renderer->Draw(BuildScene(app, area.right, area.bottom)))
        PostMessageW(app.window, RendererLost, 0, 0);
}

// The canvas shows the empty / loading / error state, or the CPU-rendered model when no GPU
// renderer could be created.
void PaintCanvas(App& app, HDC dc, RECT bounds) {
    const int width = bounds.right, height = bounds.bottom;
    if (width <= 0 || height <= 0) return;
    if (!app.gpuModel && !app.mesh.triangles.empty()) {
        const double shrink = std::min(1.0, 900.0 / std::max(width, height));
        size_t first = 0, count = SIZE_MAX;
        if (app.selectedPart >= 0 && size_t(app.selectedPart) < app.partList.size()) {
            first = app.partList[size_t(app.selectedPart)].first;
            count = app.partList[size_t(app.selectedPart)].count;
        }
        const int frameWidth = std::max(1, int(width * shrink)), frameHeight = std::max(1, int(height * shrink));
        // Fold the part placement into the view: zoom by its scale and pan its centre to the middle.
        stl::View view = app.view;
        if (const auto shown = Shown(app); shown.scale != 1) {
            const double cy = std::cos(view.yaw), sy = std::sin(view.yaw);
            const double cp = std::cos(view.pitch), sp = std::sin(view.pitch);
            const auto& o = shown.offset;
            const double across = cy * o.x - sy * o.y, up = sp * (sy * o.x + cy * o.y) + cp * o.z;
            view.zoom *= shown.scale;
            const double unit = 0.89 * std::min(frameWidth, frameHeight) * view.zoom;
            view.panX -= across * unit / frameWidth;
            view.panY += up * unit / frameHeight;
        }
        const auto frame = stl::Render(app.mesh, frameWidth, frameHeight, view, 100'000, 30'000'000,
                                      app.palette, false, first, count);
        stl::PaintFrame(dc, bounds, frame);
        return;
    }
    SetDCBrushColor(dc, app.palette.background);
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if (app.gpuModel) return;
    const auto center = height / 2;
    RECT heading{app.Px(24), center - app.Px(54), width - app.Px(24), center + app.Px(6)};
    RECT detail{app.Px(24), center + app.Px(15), width - app.Px(24), center + app.Px(app.confirmPending ? 85 : 65)};
    const bool idle = app.message.empty();
    const auto bg = app.palette.background;
    const bool dark = 299 * GetRValue(bg) + 587 * GetGValue(bg) + 114 * GetBValue(bg) < 128000;
    DrawTextLine(dc, idle ? L"拖放 3D 模型檔案" : app.filename, heading,
                 app.titleFont, dark ? Primary : Background, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawTextLine(dc, idle ? L"支援 STL、3MF、OBJ、PLY，也可以使用 Ctrl+O 開啟" : app.message, detail,
                 app.bodyFont, dark ? Secondary : Panel, DT_CENTER | DT_WORDBREAK);
}

LRESULT CALLBACK SurfaceProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;   // mouse input goes to the canvas underneath
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        if (!app || app->surface != window || !app->renderer) return 0;
        // Wait for the GPU first, then apply the mouse moves that arrived meanwhile, so the frame
        // shows where the pointer is now rather than where it was a frame ago.
        app->renderer->WaitForFrame();
        MSG pending{};
        while (PeekMessageW(&pending, app->canvas, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) DispatchMessageW(&pending);
        ValidateRect(window, nullptr);   // this frame already includes those moves
        DrawScene(*app);
        return 0;
    }
    }
    return DefWindowProcW(window, msg, wp, lp);
}

void RefreshView(App& app) {
    RedrawModel(app);
    InvalidateRect(app.cube, nullptr, FALSE);
}

// Smoothly moves the camera to `target`, taking the short way round on both angles.
void AnimateTo(App& app, stl::View target) {
    app.animFrom = app.view;
    target.yaw = app.view.yaw + std::remainder(target.yaw - app.view.yaw, FullTurn);
    target.pitch = app.view.pitch + std::remainder(target.pitch - app.view.pitch, FullTurn);
    app.animTo = target;
    app.animStart = GetTickCount64();
    app.animating = true;
    SetTimer(app.window, AnimTimer, 10, nullptr);
}

void StepAnimation(App& app) {
    if (!app.animating) {
        KillTimer(app.window, AnimTimer);
        return;
    }
    const double t = std::min(1.0, (GetTickCount64() - app.animStart) / 280.0);
    const double e = 1 - std::pow(1 - t, 3);   // ease-out cubic
    const auto mix = [e](double a, double b) { return a + (b - a) * e; };
    const auto& a = app.animFrom;
    const auto& b = app.animTo;
    app.view = {mix(a.yaw, b.yaw), mix(a.pitch, b.pitch), std::exp(mix(std::log(a.zoom), std::log(b.zoom))),
                mix(a.panX, b.panX), mix(a.panY, b.panY)};
    if (t >= 1) {
        app.view.yaw = std::remainder(b.yaw, FullTurn);
        app.view.pitch = std::remainder(b.pitch, FullTurn);
        app.animating = false;
        KillTimer(app.window, AnimTimer);
    }
    RefreshView(app);
}

void StopAnimation(App& app) {
    app.animating = false;
    KillTimer(app.window, AnimTimer);
}

void Reset(App& app) { AnimateTo(app, {}); }

void ToggleAxes(App& app) {
    app.showAxes = !app.showAxes;
    WriteSetting(L"ShowAxes", app.showAxes);
    RefreshView(app);
}

void ToggleGrid(App& app) {
    app.showGrid = !app.showGrid;
    WriteSetting(L"ShowGrid", app.showGrid);
    Refresh(app);
}

void Settings(App& app) {
    const HMENU menu = CreatePopupMenu();
    const HMENU units = CreatePopupMenu();
    const HMENU engines = CreatePopupMenu();
    if (!menu || !units || !engines) {
        for (const HMENU m : {menu, units, engines}) if (m) DestroyMenu(m);
        return;
    }
    constexpr UINT EngineBase = 30;
    for (UINT i = 0; i < ARRAYSIZE(RendererOptions); ++i)
        AppendMenuW(engines, MF_STRING | (i == app.rendererChoice ? MF_CHECKED : 0), EngineBase + i,
                    RendererOptions[i].menu);
    AppendMenuW(engines, MF_SEPARATOR, 0, nullptr);
    std::wstring current = L"目前：CPU 繪圖";
    if (app.renderer) {
        current = L"目前：" + app.renderer->Api();
        if (const auto adapter = app.renderer->Adapter(); !adapter.empty()) current += L" · " + adapter;
    }
    AppendMenuW(engines, MF_STRING | MF_GRAYED, 0, current.c_str());
    constexpr UINT UnitBase = 20;
    for (UINT i = 0; i < ARRAYSIZE(Units); ++i)
        AppendMenuW(units, MF_STRING | (i == app.unit ? MF_CHECKED : 0), UnitBase + i, Units[i].menu);
    AppendMenuW(units, MF_SEPARATOR, 0, nullptr);
    if (!app.fileUnit.empty()) {
        const std::wstring note = L"目前檔案已標示單位：" + app.fileUnit.substr(1) + L"（以檔案為準）";
        AppendMenuW(units, MF_STRING | MF_GRAYED, 0, note.c_str());
    } else if (app.faces > 0) {
        AppendMenuW(units, MF_STRING | MF_GRAYED, 0, L"目前檔案未標示單位，格線依上方選擇調整");
    }
    AppendMenuW(menu, MF_STRING | (app.showAxes ? MF_CHECKED : 0), 10, L"顯示座標軸\tA");
    AppendMenuW(menu, MF_STRING | (app.showGrid ? MF_CHECKED : 0), 11, L"顯示地板格線\tG");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(units), L"尺寸單位");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(engines), L"繪圖引擎");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1, L"背景顏色…");
    AppendMenuW(menu, MF_STRING, 2, L"模型顏色…");
    AppendMenuW(menu, MF_STRING, 3, L"還原預設顏色");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"STL Quick View 版本 " STLQV_VERSION_WSTR);
    RECT anchor{};
    GetWindowRect(app.settings, &anchor);
    const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_NONOTIFY,
                                       anchor.right, anchor.bottom, 0, app.window, nullptr);
    DestroyMenu(menu);   // also destroys the submenus
    if (!choice) return;
    if (choice == 10) return ToggleAxes(app);
    if (choice == 11) return ToggleGrid(app);
    if (choice >= int(EngineBase) && choice < int(EngineBase + ARRAYSIZE(RendererOptions))) {
        if (UINT(choice) - EngineBase != app.rendererChoice) {
            app.rendererChoice = UINT(choice) - EngineBase;
            WriteSetting(L"Renderer", app.rendererChoice);
            RestartRenderer(app);
        }
        return;
    }
    if (choice >= int(UnitBase) && choice < int(UnitBase + ARRAYSIZE(Units))) {
        app.unit = UINT(choice) - UnitBase;
        WriteSetting(L"Unit", app.unit);
        Refresh(app);
        return;
    }
    if (choice == 3) {
        app.palette = {};
    } else {
        static COLORREF custom[16]{};
        auto& color = choice == 1 ? app.palette.background : app.palette.model;
        CHOOSECOLORW picker{sizeof(picker)};
        picker.hwndOwner = app.window;
        picker.rgbResult = color;
        picker.lpCustColors = custom;
        picker.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColorW(&picker)) return;
        color = picker.rgbResult;
    }
    const auto background = RegSetKeyValueW(HKEY_CURRENT_USER, stl::SettingsKey, L"Background", REG_DWORD,
                                            &app.palette.background, sizeof(COLORREF));
    const auto model = RegSetKeyValueW(HKEY_CURRENT_USER, stl::SettingsKey, L"Model", REG_DWORD,
                                       &app.palette.model, sizeof(COLORREF));
    if (background != ERROR_SUCCESS || model != ERROR_SUCCESS)
        MessageBoxW(app.window, L"顏色已套用，系統無法儲存這次的設定。", L"STL Quick View", MB_OK | MB_ICONINFORMATION);
    Refresh(app);
}

struct CubeFace {
    const wchar_t* label;
    int axis, sign;
    double yaw, pitch;
    double right[3], up[3];   // label orientation on the face, in model axes
};
constexpr CubeFace Faces[] = {
    {L"前", 1, -1, 0, 0, {1, 0, 0}, {0, 0, 1}},
    {L"後", 1, 1, HalfTurn, 0, {-1, 0, 0}, {0, 0, 1}},
    {L"右", 0, 1, -QuarterTurn, 0, {0, 1, 0}, {0, 0, 1}},
    {L"左", 0, -1, QuarterTurn, 0, {0, -1, 0}, {0, 0, 1}},
    {L"上", 2, 1, 0, QuarterTurn, {1, 0, 0}, {0, 1, 0}},
    {L"下", 2, -1, 0, -QuarterTurn, {1, 0, 0}, {0, -1, 0}}
};

void PickFace(App& app, const CubeFace& face) {
    AnimateTo(app, {face.yaw, face.pitch, app.view.zoom, 0, 0});
}

// Orthographic projection of the view cube into its window.
struct CubeProjection {
    double cy, sy, cp, sp, center, radius;
    CubeProjection(const stl::View& view, const RECT& area)
        : cy(std::cos(view.yaw)), sy(std::sin(view.yaw)), cp(std::cos(view.pitch)), sp(std::sin(view.pitch)),
          center(area.right * 0.5), radius(area.right * 0.25) {}
    // Screen-space image of a direction (no translation).
    Gdiplus::PointF Vector(const double v[3]) const {
        const double away = sy * v[0] + cy * v[1];
        return {float(radius * (cy * v[0] - sy * v[1])), float(-radius * (sp * away + cp * v[2]))};
    }
    Gdiplus::PointF Point(const double v[3]) const {
        const auto p = Vector(v);
        return {float(center) + p.X, float(center) + p.Y};
    }
    double Facing(const CubeFace& face) const {
        const double toward[] = {-cp * sy, -cp * cy, sp};
        return toward[face.axis] * face.sign;
    }
    void Corners(const CubeFace& face, Gdiplus::PointF out[4]) const {
        constexpr int corners[][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i = 0; i < 4; ++i) {
            double coord[3]{};
            coord[face.axis] = face.sign;
            coord[(face.axis + 1) % 3] = corners[i][0];
            coord[(face.axis + 2) % 3] = corners[i][1];
            out[i] = Point(coord);
        }
    }
};

int CubeFaceAt(const App& app, HWND window, POINT point) {
    RECT area{};
    GetClientRect(window, &area);
    const CubeProjection projection(app.view, area);
    for (int i = 0; i < int(ARRAYSIZE(Faces)); ++i) {
        if (projection.Facing(Faces[i]) <= 0.001) continue;
        Gdiplus::PointF corners[4];
        projection.Corners(Faces[i], corners);
        Gdiplus::GraphicsPath path;
        path.AddPolygon(corners, 4);
        if (path.IsVisible(float(point.x), float(point.y))) return i;
    }
    return -1;
}

Gdiplus::Color Gdi(COLORREF c, BYTE alpha = 255) { return {alpha, GetRValue(c), GetGValue(c), GetBValue(c)}; }

void PaintCube(App& app, HWND window, HDC target) {
    RECT area{};
    GetClientRect(window, &area);
    const int size = area.right;
    // Paint off-screen so the cube never flickers while it spins.
    const HDC dc = CreateCompatibleDC(target);
    const HBITMAP bitmap = CreateCompatibleBitmap(target, size, area.bottom);
    const auto previous = SelectObject(dc, bitmap);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const auto bg = app.palette.background;
        // Continue the canvas backdrop gradient (see DrawBackdrop) behind the cube.
        RECT canvas{};
        GetClientRect(app.canvas, &canvas);
        POINT offset{};
        MapWindowPoints(window, app.canvas, &offset, 1);
        const auto shade = [&](int y) {
            const double t = std::clamp(double(offset.y + y) / std::max(1L, canvas.bottom), 0.0, 1.0);
            const auto lerp = [t](int value, int top, int bottom) {
                return BYTE(std::clamp(int(std::lround(value + top + (bottom - top) * t)), 0, 255));
            };
            return Gdiplus::Color(255, lerp(GetRValue(bg), 22, -8), lerp(GetGValue(bg), 24, -8),
                                  lerp(GetBValue(bg), 28, -8));
        };
        Gdiplus::LinearGradientBrush backdrop(Gdiplus::Point(0, -1), Gdiplus::Point(0, area.bottom + 1),
                                              shade(0), shade(area.bottom));
        // The gradient only exists while a model is drawn; otherwise the canvas is a flat colour.
        Gdiplus::SolidBrush flat(Gdi(bg));
        if (app.gpuModel) g.FillRectangle(&backdrop, -1, -1, size + 2, area.bottom + 2);
        else g.FillRectangle(&flat, -1, -1, size + 2, area.bottom + 2);
        const bool dark = 299 * GetRValue(bg) + 587 * GetGValue(bg) + 114 * GetBValue(bg) < 128000;
        // Soft disc behind the cube.
        Gdiplus::SolidBrush disc(dark ? Gdiplus::Color(28, 255, 255, 255) : Gdiplus::Color(20, 0, 0, 0));
        const float inset = size * 0.06f;
        g.FillEllipse(&disc, inset, inset, size - 2 * inset, size - 2 * inset);

        const CubeProjection projection(app.view, area);
        constexpr float Local = 50;   // label space: [-Local, Local] square per face
        if (!app.cubeFont) {
            app.cubeFamily = std::make_unique<Gdiplus::FontFamily>(L"Microsoft JhengHei UI");
            app.cubeFont = std::make_unique<Gdiplus::Font>(app.cubeFamily.get(), 46.f, Gdiplus::FontStyleBold,
                                                           Gdiplus::UnitPixel);
            app.cubeFormat = std::make_unique<Gdiplus::StringFormat>();
            app.cubeFormat->SetAlignment(Gdiplus::StringAlignmentCenter);
            app.cubeFormat->SetLineAlignment(Gdiplus::StringAlignmentCenter);
        }
        const auto& font = *app.cubeFont;
        const auto& centered = *app.cubeFormat;
        Gdiplus::Pen edge(Gdiplus::Color(150, 14, 28, 44), 1.2f);
        edge.SetLineJoin(Gdiplus::LineJoinRound);
        for (int i = 0; i < int(ARRAYSIZE(Faces)); ++i) {
            const auto& face = Faces[i];
            const double facing = projection.Facing(face);
            if (facing <= 0.001) continue;
            Gdiplus::PointF corners[4];
            projection.Corners(face, corners);
            const int tint = int(facing * 40);
            const bool hot = i == app.hoverFace;
            Gdiplus::SolidBrush fill(hot ? Gdiplus::Color(255, 105, 217, 245)
                                         : Gdiplus::Color(255, BYTE(116 + tint), BYTE(160 + tint), BYTE(188 + tint)));
            g.FillPolygon(&fill, corners, 4);
            g.DrawPolygon(&edge, corners, 4);
            if (facing < 0.12) continue;
            // Map the label square onto the face so the text turns and foreshortens with it.
            double center[3]{};
            center[face.axis] = face.sign;
            const auto c = projection.Point(center);
            const auto r = projection.Vector(face.right);
            const auto u = projection.Vector(face.up);
            Gdiplus::Matrix transform(r.X / Local, r.Y / Local, -u.X / Local, -u.Y / Local, c.X, c.Y);
            g.SetTransform(&transform);
            const BYTE alpha = BYTE(std::clamp((facing - 0.12) / 0.25, 0.0, 1.0) * 255);
            Gdiplus::SolidBrush ink(Gdiplus::Color(alpha, 16, 30, 46));
            g.DrawString(face.label, -1, &font, Gdiplus::RectF(-Local, -Local, 2 * Local, 2 * Local), &centered, &ink);
            g.ResetTransform();
        }
        if (GetFocus() == window) {
            Gdiplus::Pen focus(Gdiplus::Color(200, 105, 217, 245), 1.5f);
            g.DrawEllipse(&focus, inset, inset, size - 2 * inset, size - 2 * inset);
        }
    }
    BitBlt(target, 0, 0, size, area.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT CALLBACK CubeProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!app) return DefWindowProcW(window, msg, wp, lp);
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        const int face = CubeFaceAt(*app, window, {short(LOWORD(lp)), short(HIWORD(lp))});
        if (face != app->hoverFace) {
            app->hoverFace = face;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        app->hoverFace = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        PaintCube(*app, window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        const int face = CubeFaceAt(*app, window, {short(LOWORD(lp)), short(HIWORD(lp))});
        if (face >= 0) {
            SetFocus(window);
            PickFace(*app, Faces[face]);
        }
        return 0;
    }
    if (msg == WM_CONTEXTMENU || (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_SPACE))) {
        const HMENU menu = CreatePopupMenu();
        if (!menu) return 0;
        for (UINT i = 0; i < ARRAYSIZE(Faces); ++i)
            AppendMenuW(menu, MF_STRING, i + 1, Faces[i].label);
        RECT anchor{};
        GetWindowRect(window, &anchor);
        const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN,
                                           anchor.right, anchor.bottom, 0, app->window, nullptr);
        DestroyMenu(menu);
        if (choice >= 1 && choice <= ARRAYSIZE(Faces)) PickFace(*app, Faces[choice - 1]);
        return 0;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(window, nullptr, FALSE);
    return DefWindowProcW(window, msg, wp, lp);
}

// ---- Parts panel ------------------------------------------------------------------------------
// Shown on the left of the view when a file holds several parts, or carries colours we did not
// load. Clicking a part shows only that part and frames it; "全部顯示" brings everything back.

struct PartsLayout {
    int pad, notice, header, row;
    int rowsTop;        // y of the first row, before scrolling
    int rows;           // number of rows ("全部顯示" + parts), 0 when no list
    int contentHeight;  // full height of the panel content
};

PartsLayout MeasureParts(const App& app) {
    PartsLayout l{app.Px(8), app.hasColor ? app.Px(54) : 0, 0, app.Px(32), 0, 0, 0};
    if (app.partList.size() > 1) {
        l.header = app.Px(28);
        l.rows = int(app.partList.size()) + 1;
    }
    l.rowsTop = l.pad + l.notice + l.header;
    l.contentHeight = l.rowsTop + l.rows * l.row + l.pad;
    return l;
}

void LayoutParts(App& app) {
    if (!app.parts) return;
    const bool show = app.faces > 0 && (app.partList.size() > 1 || app.hasColor);
    if (!show) {
        ShowWindow(app.parts, SW_HIDE);
        return;
    }
    RECT canvas{};
    GetWindowRect(app.canvas, &canvas);
    MapWindowPoints(nullptr, app.window, reinterpret_cast<POINT*>(&canvas), 2);
    const auto l = MeasureParts(app);
    const int margin = app.Px(12);
    const int height = std::min(l.contentHeight, int(canvas.bottom - canvas.top) - 2 * margin);
    const int maxScroll = std::max(0, l.contentHeight - height);
    app.partsScroll = std::clamp(app.partsScroll, 0, maxScroll);
    SetWindowPos(app.parts, HWND_TOP, canvas.left + margin, canvas.top + margin, app.Px(240), std::max(0, height),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(app.parts, nullptr, FALSE);
}

void SelectPart(App& app, int index) {
    if (index == app.selectedPart) return;
    app.selectedPart = index;
    StopAnimation(app);
    // Show the part as if it had been opened on its own: centred and framed, same viewing angle.
    app.view = {app.view.yaw, app.view.pitch, 1, 0, 0};
    Refresh(app);
}

// Row under a client-area point: -1 for "全部顯示", 0.. for parts, -2 for none.
int PartAt(const App& app, int y) {
    const auto l = MeasureParts(app);
    if (!l.rows) return -2;
    const int row = (y + app.partsScroll - l.rowsTop);
    if (row < 0) return -2;
    const int index = row / l.row;
    return index < l.rows ? index - 1 : -2;
}

std::wstring Grouped(size_t value) {
    std::wstring text = std::to_wstring(value);
    for (int i = int(text.size()) - 3; i > 0; i -= 3) text.insert(size_t(i), 1, L',');
    return text;
}

void PaintParts(App& app, HWND window, HDC target) {
    RECT area{};
    GetClientRect(window, &area);
    const int width = area.right, height = area.bottom;
    if (width <= 0 || height <= 0) return;
    const HDC dc = CreateCompatibleDC(target);
    const HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    const auto previous = SelectObject(dc, bitmap);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        const auto bg = app.palette.background;
        const bool dark = 299 * GetRValue(bg) + 587 * GetGValue(bg) + 114 * GetBValue(bg) < 128000;
        // Continue the view's backdrop gradient, then lay a translucent card over it.
        RECT canvas{};
        GetClientRect(app.canvas, &canvas);
        POINT offset{};
        MapWindowPoints(window, app.canvas, &offset, 1);
        float top[3], bottom[3];
        render::Backdrop(bg, top, bottom);
        const auto shade = [&](int y) {
            const float t = std::clamp(float(offset.y + y) / float(std::max(1L, canvas.bottom)), 0.f, 1.f);
            return Gdiplus::Color(255, BYTE((top[0] + (bottom[0] - top[0]) * t) * 255),
                                  BYTE((top[1] + (bottom[1] - top[1]) * t) * 255),
                                  BYTE((top[2] + (bottom[2] - top[2]) * t) * 255));
        };
        Gdiplus::LinearGradientBrush backdrop(Gdiplus::Point(0, -1), Gdiplus::Point(0, height + 1), shade(0), shade(height));
        g.FillRectangle(&backdrop, -1, -1, width + 2, height + 2);

        const float radius = float(app.Px(10));
        Gdiplus::GraphicsPath card;
        const Gdiplus::RectF box(0.5f, 0.5f, float(width) - 1.5f, float(height) - 1.5f);
        card.AddArc(box.X, box.Y, 2 * radius, 2 * radius, 180, 90);
        card.AddArc(box.GetRight() - 2 * radius, box.Y, 2 * radius, 2 * radius, 270, 90);
        card.AddArc(box.GetRight() - 2 * radius, box.GetBottom() - 2 * radius, 2 * radius, 2 * radius, 0, 90);
        card.AddArc(box.X, box.GetBottom() - 2 * radius, 2 * radius, 2 * radius, 90, 90);
        card.CloseFigure();
        Gdiplus::SolidBrush cardFill(dark ? Gdiplus::Color(150, 10, 16, 26) : Gdiplus::Color(170, 255, 255, 255));
        g.FillPath(&cardFill, &card);
        Gdiplus::Pen cardEdge(dark ? Gdiplus::Color(40, 255, 255, 255) : Gdiplus::Color(40, 0, 0, 0), 1);
        g.DrawPath(&cardEdge, &card);
        g.SetClip(&card);

        if (!app.partsFont) {
            app.partsFamily = std::make_unique<Gdiplus::FontFamily>(L"Microsoft JhengHei UI");
            app.partsFont = std::make_unique<Gdiplus::Font>(app.partsFamily.get(), float(app.Px(13)),
                                                            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            app.partsBold = std::make_unique<Gdiplus::Font>(app.partsFamily.get(), float(app.Px(13)),
                                                            Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            app.partsSmall = std::make_unique<Gdiplus::Font>(app.partsFamily.get(), float(app.Px(11)),
                                                             Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        }
        Gdiplus::SolidBrush primary(dark ? Gdi(Primary) : Gdi(RGB(24, 32, 44)));
        Gdiplus::SolidBrush secondary(dark ? Gdi(Secondary) : Gdi(RGB(80, 92, 108)));
        Gdiplus::StringFormat leftText, rightText;
        leftText.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        leftText.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        leftText.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        rightText.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        rightText.SetAlignment(Gdiplus::StringAlignmentFar);
        rightText.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        const auto l = MeasureParts(app);
        const float inset = float(app.Px(12));
        const float scroll = float(app.partsScroll);
        if (l.notice) {
            // Colour notice: the file has colours / materials, but only the shape is shown.
            const float y = float(l.pad) - scroll;
            const float dot = float(app.Px(8));
            Gdiplus::SolidBrush amber(Gdiplus::Color(255, 245, 182, 66));
            g.FillEllipse(&amber, inset, y + float(app.Px(11)), dot, dot);
            g.DrawString(L"此檔案含有顏色資訊", -1, app.partsBold.get(),
                         Gdiplus::RectF(inset + dot + float(app.Px(8)), y + float(app.Px(3)), float(width) - 2 * inset, float(app.Px(22))),
                         &leftText, &primary);
            g.DrawString(L"僅載入外形，未套用顏色與材質", -1, app.partsSmall.get(),
                         Gdiplus::RectF(inset + dot + float(app.Px(8)), y + float(app.Px(24)), float(width) - 2 * inset - dot, float(app.Px(20))),
                         &leftText, &secondary);
            if (l.rows) {
                Gdiplus::Pen divider(dark ? Gdiplus::Color(36, 255, 255, 255) : Gdiplus::Color(36, 0, 0, 0), 1);
                const float line = float(l.pad + l.notice) - scroll - 0.5f;
                g.DrawLine(&divider, inset, line, float(width) - inset, line);
            }
        }
        if (l.rows) {
            const float y = float(l.pad + l.notice) - scroll;
            g.DrawString((L"物件（" + std::to_wstring(app.partList.size()) + L"）").c_str(), -1, app.partsSmall.get(),
                         Gdiplus::RectF(inset, y, float(width) - 2 * inset, float(l.header)), &leftText, &secondary);
            for (int r = 0; r < l.rows; ++r) {
                const int index = r - 1;
                const float rowTop = float(l.rowsTop + r * l.row) - scroll;
                if (rowTop + l.row < 0 || rowTop > height) continue;
                const Gdiplus::RectF rowBox(float(app.Px(6)), rowTop + 1, float(width - app.Px(12)), float(l.row - 2));
                const bool selected = index == app.selectedPart, hover = index == app.partsHover;
                if (selected || hover) {
                    Gdiplus::SolidBrush highlight(selected ? Gdiplus::Color(dark ? 70 : 90, 105, 217, 245)
                                                           : (dark ? Gdiplus::Color(22, 255, 255, 255) : Gdiplus::Color(18, 0, 0, 0)));
                    g.FillRectangle(&highlight, rowBox);
                }
                if (selected) {
                    Gdiplus::SolidBrush accent(Gdiplus::Color(255, 105, 217, 245));
                    g.FillRectangle(&accent, rowBox.X, rowBox.Y + 4, 3.f, rowBox.Height - 8);
                }
                const std::wstring name = index < 0 ? L"全部顯示" : app.partList[size_t(index)].name;
                const size_t faces = index < 0 ? app.faces : app.partList[size_t(index)].count;
                const std::wstring count = Grouped(faces) + L" 面";
                const float countWidth = float(app.Px(78));
                g.DrawString(name.c_str(), -1, selected || index < 0 ? app.partsBold.get() : app.partsFont.get(),
                             Gdiplus::RectF(inset, rowTop, float(width) - 2 * inset - countWidth, float(l.row)), &leftText, &primary);
                g.DrawString(count.c_str(), -1, app.partsSmall.get(),
                             Gdiplus::RectF(float(width) - inset - countWidth, rowTop, countWidth, float(l.row)), &rightText, &secondary);
            }
        }
        g.ResetClip();
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT CALLBACK PartsProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!app) return DefWindowProcW(window, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        PaintParts(*app, window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        const int row = PartAt(*app, short(HIWORD(lp)));
        const int hover = row >= -1 ? row : -2;
        if (hover != app->partsHover) {
            app->partsHover = hover;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        app->partsHover = -2;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const int row = PartAt(*app, short(HIWORD(lp)));
        if (row >= -1) SelectPart(*app, row);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        RECT area{};
        GetClientRect(window, &area);
        const int maxScroll = std::max(0, MeasureParts(*app).contentHeight - int(area.bottom));
        app->partsScroll = std::clamp(app->partsScroll - GET_WHEEL_DELTA_WPARAM(wp) * app->Px(32) / 120, 0, maxScroll);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

LRESULT CALLBACK CanvasProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!app) return DefWindowProcW(window, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        if (app->surface) MoveWindow(app->surface, 0, 0, LOWORD(lp), HIWORD(lp), FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        try {
            PaintCanvas(*app, dc, bounds);
        } catch (const std::exception&) {
            FillRect(dc, &bounds, app->panelBrush);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (app->faces > 0) {
            app->drag = msg;
            app->last = {short(LOWORD(lp)), short(HIWORD(lp))};
            SetFocus(window);
            SetCapture(window);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app->drag && GetCapture() == window) {
            const POINT current{short(LOWORD(lp)), short(HIWORD(lp))};
            const double dx = current.x - app->last.x, dy = current.y - app->last.y;
            StopAnimation(*app);
            if (app->drag == WM_LBUTTONDOWN) {
                // Free orbit: pitch may go over the top. Once upside down, horizontal
                // dragging is mirrored so the model still follows the mouse.
                const double turn = std::cos(app->view.pitch) < 0 ? -dx : dx;
                app->view.yaw = std::remainder(app->view.yaw + turn * 0.009, FullTurn);
                app->view.pitch = std::remainder(app->view.pitch + dy * 0.009, FullTurn);
                InvalidateRect(app->cube, nullptr, FALSE);
            } else {
                RECT rect{};
                GetClientRect(window, &rect);
                app->view.panX = std::clamp(app->view.panX + dx / std::max(1L, rect.right), -50.0, 50.0);
                app->view.panY = std::clamp(app->view.panY + dy / std::max(1L, rect.bottom), -50.0, 50.0);
            }
            app->last = current;
            RedrawModel(*app);
        }
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (GetCapture() == window) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED: app->drag = 0; return 0;
    case WM_LBUTTONDBLCLK: Reset(*app); return 0;
    case WM_MOUSEWHEEL:
        StopAnimation(*app);
        app->view.zoom = std::clamp(app->view.zoom * std::pow(1.15, GET_WHEEL_DELTA_WPARAM(wp) / 120.0), 0.05, 100.0);
        RedrawModel(*app);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        DestroySurface(*app);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

void Layout(App& app) {
    RECT box{};
    GetClientRect(app.window, &box);
    const int w = box.right, h = box.bottom;
    MoveWindow(app.open, w - app.Px(307), app.Px(17), app.Px(98), app.Px(34), TRUE);
    MoveWindow(app.reset, w - app.Px(199), app.Px(17), app.Px(93), app.Px(34), TRUE);
    MoveWindow(app.settings, w - app.Px(96), app.Px(17), app.Px(76), app.Px(34), TRUE);
    // Confirmation buttons sit under the canvas message (see PaintCanvas).
    const int canvasTop = app.Px(68), canvasHeight = std::max(0, h - app.Px(114));
    const int buttonsY = canvasTop + canvasHeight / 2 + app.Px(96);
    SetWindowPos(app.confirm, HWND_TOP, w / 2 - app.Px(106), buttonsY, app.Px(100), app.Px(34), SWP_NOACTIVATE);
    SetWindowPos(app.cancelLoad, HWND_TOP, w / 2 + app.Px(6), buttonsY, app.Px(100), app.Px(34), SWP_NOACTIVATE);
    MoveWindow(app.canvas, 0, app.Px(68), w, std::max(0, h - app.Px(114)), TRUE);
    SetWindowPos(app.cube, HWND_TOP, w - app.Px(122), app.Px(78), app.Px(110), app.Px(110),
                 SWP_NOACTIVATE);
    LayoutParts(app);
    Refresh(app);
}

void Fonts(App& app) {
    if (app.bodyFont) DeleteObject(app.bodyFont);
    if (app.titleFont) DeleteObject(app.titleFont);
    app.bodyFont = CreateFontW(-app.Px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
    app.titleFont = CreateFontW(-app.Px(24), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
}

LRESULT CALLBACK MainProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!app) return DefWindowProcW(window, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        app->window = window;
        app->dpi = GetDpiForWindow(window);
        app->panelBrush = CreateSolidBrush(Panel);
        Fonts(*app);
        app->canvas = CreateWindowExW(0, CanvasClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                     0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), app);
        app->confirm = CreateWindowExW(0, L"BUTTON", L"仍要載入", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(ConfirmButton)), nullptr, nullptr);
        app->cancelLoad = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                         0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(CancelLoadButton)), nullptr, nullptr);
        app->open = CreateWindowExW(0, L"BUTTON", L"開啟模型", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(OpenButton)), nullptr, nullptr);
        app->reset = CreateWindowExW(0, L"BUTTON", L"重設視角", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(ResetButton)), nullptr, nullptr);
        app->settings = CreateWindowExW(0, L"BUTTON", L"設定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(SettingsButton)), nullptr, nullptr);
        app->cube = CreateWindowExW(0, CubeClass, L"視角立方體", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), app);
        app->parts = CreateWindowExW(0, PartsClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                                     0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), app);
        if (!app->canvas || !app->open || !app->reset || !app->settings || !app->cube || !app->parts) return -1;
        EnableWindow(app->reset, FALSE);
        DragAcceptFiles(window, TRUE);
        CreateRenderer(*app);
        return 0;
    case WM_SIZE: Layout(*app); return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {app->Px(600), app->Px(340)};
        return 0;
    }
    case WM_DPICHANGED: {
        app->dpi = HIWORD(wp);
        Fonts(*app);
        const auto& rect = *reinterpret_cast<RECT*>(lp);
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        Layout(*app);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == OpenButton) Browse(*app);
        if (LOWORD(wp) == ConfirmButton && app->confirmPending) OpenModel(*app, app->path, true);
        if (LOWORD(wp) == CancelLoadButton && app->confirmPending) {
            ShowConfirm(*app, false);
            app->message = L"已取消載入。";
            Refresh(*app);
        }
        if (LOWORD(wp) == ResetButton) Reset(*app);
        if (LOWORD(wp) == SettingsButton) Settings(*app);
        if (LOWORD(wp) == AxesCommand) ToggleAxes(*app);
        if (LOWORD(wp) == GridCommand) ToggleGrid(*app);
        return 0;
    case WM_TIMER:
        if (wp == AnimTimer) StepAnimation(*app);
        return 0;
    case RendererLost:
        RestartRenderer(*app);
        return 0;
    case LoadDone:
        FinishLoad(*app);
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(app->canvas, msg, wp, lp);
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wp);
        const UINT size = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(size + 1, L'\0');
        if (size && DragQueryFileW(drop, 0, path.data(), size + 1)) {
            path.resize(size);
            OpenModel(*app, std::move(path));
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        const bool main = item.CtlID == OpenButton || item.CtlID == ConfirmButton;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const auto fill = main ? (pressed ? RGB(88, 197, 226) : RGB(120, 218, 244))
                               : (pressed ? RGB(50, 69, 91) : RGB(34, 49, 69));
        SetDCBrushColor(item.hDC, fill);
        FillRect(item.hDC, &item.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const wchar_t* label = item.CtlID == OpenButton ? L"開啟模型" : item.CtlID == ConfirmButton ? L"仍要載入"
                             : item.CtlID == CancelLoadButton ? L"取消" : item.CtlID == ResetButton ? L"重設視角" : L"設定";
        DrawTextLine(item.hDC, label, item.rcItem, app->bodyFont,
                     main ? Background : (disabled ? RGB(120, 140, 158) : Primary),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (item.itemState & ODS_FOCUS) {
            RECT focus = item.rcItem;
            InflateRect(&focus, -3, -3);
            DrawFocusRect(item.hDC, &focus);
        }
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(dc, &paint.rcPaint, app->panelBrush);
        DrawTextLine(dc, L"STL Quick View", {app->Px(20), 0, rect.right - app->Px(325), app->Px(68)},
                     app->titleFont, Primary, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        std::wstring footer = L"左鍵旋轉  ·  右鍵平移  ·  滾輪縮放  ·  右鍵點立方體選取視角";
        if (app->faces > 0) {
            // Describe the shown part, or the whole model.
            const bool single = app->selectedPart >= 0 && size_t(app->selectedPart) < app->partList.size();
            const auto* part = single ? &app->partList[size_t(app->selectedPart)] : nullptr;
            const double dims[3] = {part ? part->size[0] : app->mesh.width, part ? part->size[1] : app->mesh.depth,
                                    part ? part->size[2] : app->mesh.height};
            // A unit stated in the file (3MF) wins over the setting.
            const wchar_t* suffix = UnitSuffix(*app);
            wchar_t metadata[240]{};
            swprintf_s(metadata, L"   |   %ls 個三角面   ·   X %.5g × Y %.5g × Z %.5g%ls",
                       Grouped(part ? part->count : app->faces).c_str(), dims[0], dims[1], dims[2], suffix);
            footer = app->filename + (part ? L" › " + part->name : std::wstring()) + metadata;
            if (const auto grid = GridFor(*app); grid.step > 0)
                footer += L"   ·   格線 " + FormatLength(grid.step, EffectiveUnit(*app)) + suffix;
            footer += L"   ·   " + app->format;
            if (app->hasColor) footer += L"（含顏色資訊，未載入）";
            if (app->gpuModel) {
                footer += L"   ·   " + app->renderer->Api();
                if (app->renderer->Software()) footer += L"（軟體繪圖，未啟用顯示卡加速）";
            } else {
                footer += L"   ·   CPU 繪圖";
            }
        }
        DrawTextLine(dc, footer, {app->Px(20), rect.bottom - app->Px(46), rect.right - app->Px(20), rect.bottom},
                     app->bodyFont, Secondary, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        app->cubeFont.reset();   // GDI+ objects must go before GdiplusShutdown
        app->cubeFamily.reset();
        app->cubeFormat.reset();
        app->partsFont.reset();
        app->partsBold.reset();
        app->partsSmall.reset();
        app->partsFamily.reset();
        DeleteObject(app->bodyFont);
        DeleteObject(app->titleFont);
        DeleteObject(app->panelBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ULONG_PTR gdiplus{};
    const Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&gdiplus, &gdiplusInput, nullptr) != Gdiplus::Ok) return 1;
    const HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    WNDCLASSW canvas{CS_DBLCLKS, CanvasProc, 0, 0, instance, nullptr, arrow, nullptr, nullptr, CanvasClass};
    WNDCLASSW cube{0, CubeProc, 0, 0, instance, nullptr, LoadCursorW(nullptr, IDC_HAND), nullptr, nullptr, CubeClass};
    WNDCLASSW main{0, MainProc, 0, 0, instance, icon, arrow, nullptr, nullptr, AppClass};
    WNDCLASSW surface{CS_OWNDC, SurfaceProc, 0, 0, instance, nullptr, arrow, nullptr, nullptr, SurfaceClass};
    WNDCLASSW parts{0, PartsProc, 0, 0, instance, nullptr, arrow, nullptr, nullptr, PartsClass};
    if (!RegisterClassW(&canvas) || !RegisterClassW(&cube) || !RegisterClassW(&main) || !RegisterClassW(&surface) ||
        !RegisterClassW(&parts))
        return 1;
    App app;
    app.dpi = GetDpiForSystem();
    const HWND window = CreateWindowExW(0, AppClass, L"STL Quick View",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        app.Px(920), app.Px(660), nullptr, nullptr, instance, &app);
    if (!window) return 1;
    ShowWindow(window, show);
    int count{};
    if (auto** args = CommandLineToArgvW(GetCommandLineW(), &count)) {
        if (count > 1) OpenModel(app, args[1]);
        LocalFree(args);
    }
    ACCEL shortcuts[] = {{FVIRTKEY | FCONTROL, 'O', OpenButton}, {FVIRTKEY, VK_HOME, ResetButton},
                         {FVIRTKEY, 'A', AxesCommand}, {FVIRTKEY, 'G', GridCommand}};
    HACCEL keys = CreateAcceleratorTableW(shortcuts, ARRAYSIZE(shortcuts));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(window, keys, &message)) continue;
        if ((message.message == WM_KEYDOWN && message.wParam == VK_TAB) &&
            IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyAcceleratorTable(keys);
    Gdiplus::GdiplusShutdown(gdiplus);
    return static_cast<int>(message.wParam);
}
