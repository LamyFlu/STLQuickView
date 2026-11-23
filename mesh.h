#pragma once

#include <windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace stl {
constexpr size_t MaxBytes = 128ull * 1024 * 1024;
constexpr size_t MaxFaces = 2'000'000;

struct Point { float x, y, z; };
struct Triangle { Point a, b, c; };
struct Mesh {
    std::vector<Triangle> triangles;
    double width{}, depth{}, height{};
    // Triangles are stored as (file - center) / diameter.
    double center[3]{}, diameter{};
    Point ToModel(double x, double y, double z) const {
        return {float((x - center[0]) / diameter), float((y - center[1]) / diameter),
                float((z - center[2]) / diameter)};
    }
};
struct View {
    double yaw = 0.65, pitch = 0.50, zoom = 1, panX = 0, panY = 0;
};
struct Palette {
    COLORREF background = RGB(18, 26, 40);
    COLORREF model = RGB(204, 207, 212);   // light grey
};
inline constexpr wchar_t SettingsKey[] = L"Software\\StlQuickView";
struct Frame {
    int width{}, height{};
    std::vector<uint32_t> pixels;
};

// Extra facts about an STL file, for the viewer's parts list and colour notice.
struct StlInfo {
    struct Solid { std::string name; size_t first; };   // ASCII "solid <name>" blocks
    std::vector<Solid> solids;
    bool color{};                                         // binary colour attributes present
};
Mesh Parse(std::span<const char> data, StlInfo* info = nullptr);
// Validates the triangles, records size and centre, and rescales them to a unit diameter.
void Normalize(Mesh& mesh);
Palette ReadPalette();
// transparent: background pixels get alpha 0, model pixels are opaque (premultiplied ARGB).
Frame Render(const Mesh& mesh, int width, int height, const View& view,
             size_t faceBudget, uint64_t pixelBudget, Palette palette = {}, bool transparent = false,
             size_t first = 0, size_t count = SIZE_MAX);
// Adjusts zoom and pan so the projected model fills `fill` (0..1) of a square frame.
View FitView(const Mesh& mesh, View view, double fill);
// Box-filters a frame rendered at `factor` times the output size.
Frame Downsample(const Frame& frame, int factor);
void PaintFrame(HDC dc, const RECT& bounds, const Frame& frame);
}
