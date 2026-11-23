#pragma once

// GPU back ends for the viewer. Direct3D 11 is the primary renderer; OpenGL is the fallback.
// Both draw the same Scene: a lit mesh over a gradient backdrop, an optional floor grid and
// optional X/Y/Z axes with labels.

#include "mesh.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace render {
// Interleaved position + normal, built once per model on the loader thread.
struct Vertex { stl::Point p, n; };
static_assert(sizeof(Vertex) == 24);

struct ColorVertex { float x, y, z, r, g, b, a; };

struct Scene {
    stl::View view;
    int width{}, height{};
    COLORREF background{}, model{};
    std::vector<ColorVertex> grid;   // line list in model space, empty when hidden
    bool axes{};
    stl::Point origin{};             // axis origin in model space
    float axisLength{};
    uint32_t first = 0, count = UINT32_MAX;   // vertex range to draw (one part, or everything)
};

class Renderer {
public:
    virtual ~Renderer() = default;
    // Takes ownership of the vertices; returns false if the GPU could not hold them.
    virtual bool Upload(std::vector<Vertex> vertices) = 0;
    virtual void Resize(int width, int height) = 0;
    // Blocks until the GPU can take another frame (at most one queued), so the frame about to
    // be drawn uses the newest input instead of piling up behind older frames.
    virtual void WaitForFrame() {}
    // Returns false when the device was lost and the renderer must be recreated.
    virtual bool Draw(const Scene& scene) = 0;
    virtual std::wstring Api() const = 0;       // "Direct3D 11" or "OpenGL"
    virtual std::wstring Adapter() const = 0;   // GPU name reported by the driver
    virtual bool Software() const = 0;          // WARP or the Windows GDI OpenGL
};

// Each returns nullptr when the API is unavailable. `allowSoftware` permits WARP / GDI OpenGL.
std::unique_ptr<Renderer> CreateD3D11(HWND window, bool allowSoftware);
std::unique_ptr<Renderer> CreateOpenGL(HWND window, bool allowSoftware);

// ---- Shared scene math -------------------------------------------------------------------

// Row-major matrices for row vectors (p * M). `rotation` maps model to eye space (x right,
// y up, z toward the viewer); `transform` maps model space to clip space with depth in [0, 1].
void ViewMatrices(const stl::View& view, int width, int height, float rotation[4][4], float transform[4][4]);
void Project(const float transform[4][4], stl::Point p, float clip[3]);

// Top and bottom colours of the backdrop gradient.
void Backdrop(COLORREF background, float top[3], float bottom[3]);

// Light rig shared by both back ends (eye-space directions, normalised).
struct Lighting {
    float key[3], fill[3];
    float keyDiffuse, keySpecular, fillDiffuse[3], ambient, shininess;
};
Lighting Lights();

inline constexpr float AxisColors[3][3] = {{0.94f, 0.36f, 0.36f}, {0.45f, 0.84f, 0.38f}, {0.36f, 0.58f, 1.f}};
inline constexpr wchar_t AxisNames[3] = {L'X', L'Y', L'Z'};
}
