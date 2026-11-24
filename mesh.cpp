#include "mesh.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace stl {
namespace {
bool Space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

class Text {
public:
    explicit Text(std::string_view source) : input(source) {}

    std::string_view Token() {
        while (!input.empty() && Space(input.front())) input.remove_prefix(1);
        size_t length = 0;
        while (length < input.size() && !Space(input[length])) ++length;
        const auto token = input.substr(0, length);
        input.remove_prefix(length);
        return token;
    }

    void Expect(std::string_view expected) {
        if (Token() != expected) throw std::runtime_error("ASCII STL 結構不正確。");
    }

    std::string_view RestOfLine() {
        const auto end = std::min(input.find_first_of("\r\n"), input.size());
        auto line = input.substr(0, end);
        input.remove_prefix(end);
        while (!line.empty() && Space(line.front())) line.remove_prefix(1);
        while (!line.empty() && Space(line.back())) line.remove_suffix(1);
        return line;
    }

    void SkipLine() {
        const auto end = input.find_first_of("\r\n");
        input.remove_prefix(std::min(end, input.size()));
    }

    float Number() {
        auto token = Token();
        if (token.starts_with('+')) token.remove_prefix(1);
        if (token.empty()) throw std::runtime_error("STL 缺少座標。");
        float value{};
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value))
            throw std::runtime_error("STL 座標無效。");
        return value;
    }

    Point Vertex() {
        Expect("vertex");
        return {Number(), Number(), Number()};
    }

private:
    std::string_view input;
};

Point Cross(Point a, Point b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Point Subtract(Point a, Point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

float Edge(Point a, Point b, float x, float y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}
}

void Normalize(Mesh& mesh) {
    const double inf = std::numeric_limits<double>::infinity();
    double minimum[] = {inf, inf, inf};
    double maximum[] = {-inf, -inf, -inf};
    for (const auto& triangle : mesh.triangles) {
        for (const Point p : {triangle.a, triangle.b, triangle.c}) {
            const double coords[] = {p.x, p.y, p.z};
            for (int axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(coords[axis])) throw std::runtime_error("模型座標無效。");
                minimum[axis] = std::min(minimum[axis], coords[axis]);
                maximum[axis] = std::max(maximum[axis], coords[axis]);
            }
        }
    }
    if (mesh.triangles.empty()) throw std::runtime_error("這個模型沒有三角面。");
    mesh.width = maximum[0] - minimum[0];
    mesh.depth = maximum[1] - minimum[1];
    mesh.height = maximum[2] - minimum[2];
    const double diameter = std::sqrt(mesh.width * mesh.width + mesh.depth * mesh.depth +
                                      mesh.height * mesh.height);
    if (diameter == 0) throw std::runtime_error("這個模型沒有可顯示的表面。");
    const double cx = (minimum[0] + maximum[0]) * 0.5;
    const double cy = (minimum[1] + maximum[1]) * 0.5;
    const double cz = (minimum[2] + maximum[2]) * 0.5;
    mesh.center[0] = cx;
    mesh.center[1] = cy;
    mesh.center[2] = cz;
    mesh.diameter = diameter;
    const double inverse = 1 / diameter;
    bool visible = false;
    for (auto& triangle : mesh.triangles) {
        for (Point* p : {&triangle.a, &triangle.b, &triangle.c}) {
            *p = {float((p->x - cx) * inverse), float((p->y - cy) * inverse),
                  float((p->z - cz) * inverse)};
        }
        if (!visible) {
            const auto n = Cross(Subtract(triangle.b, triangle.a), Subtract(triangle.c, triangle.a));
            visible = n.x != 0 || n.y != 0 || n.z != 0;
        }
    }
    if (!visible) throw std::runtime_error("這個模型沒有可顯示的表面。");
}

Mesh Parse(std::span<const char> data, StlInfo* info, Limits limits) {
    if (data.empty()) throw std::runtime_error("這個檔案是空的。");
    if (data.size() > limits.bytes) throw TooLarge("STL 檔案超過大小上限。", 0, data.size());
    Mesh mesh;
    uint32_t count{};
    if (data.size() >= 84) std::memcpy(&count, data.data() + 80, sizeof count);
    if (data.size() >= 84 && 84ull + 50ull * count == data.size()) {
        if (count > limits.faces) throw TooLarge("STL 三角面數超過上限。", count, 0);
        mesh.triangles.resize(count);
        static_assert(sizeof(Triangle) == 36);
        for (size_t i = 0; i < count; ++i)
            std::memcpy(&mesh.triangles[i], data.data() + 84 + 50 * i + 12, sizeof(Triangle));
        if (info) {
            // Colour conventions: Materialise puts "COLOR=" in the header; VisCAM/SolidView set
            // bit 15 of a facet's attribute word.
            const std::string_view header(data.data(), 80);
            info->color = header.find("COLOR=") != std::string_view::npos;
            for (size_t i = 0; i < count && !info->color; ++i) {
                uint16_t attribute{};
                std::memcpy(&attribute, data.data() + 84 + 50 * i + 48, sizeof attribute);
                info->color = (attribute & 0x8000) != 0;
            }
        }
    } else {
        auto source = std::string_view(data.data(), data.size());
        if (source.starts_with("\xef\xbb\xbf")) source.remove_prefix(3);
        Text text(source);
        for (auto token = text.Token(); !token.empty(); token = text.Token()) {
            if (token != "solid") throw std::runtime_error("STL 格式無法辨識，或檔案不完整。");
            const auto name = text.RestOfLine();
            if (info) info->solids.push_back({std::string(name), mesh.triangles.size()});
            for (token = text.Token(); token != "endsolid"; token = text.Token()) {
                if (token != "facet") throw std::runtime_error("ASCII STL 三角面結構不正確。");
                text.Expect("normal");
                for (int axis = 0; axis < 3; ++axis) text.Number();
                text.Expect("outer");
                text.Expect("loop");
                const Triangle face{text.Vertex(), text.Vertex(), text.Vertex()};
                text.Expect("endloop");
                text.Expect("endfacet");
                if (mesh.triangles.size() >= limits.faces) throw TooLarge("STL 三角面數超過上限。", 0, 0);
                mesh.triangles.push_back(face);
            }
            text.SkipLine();
        }
    }
    Normalize(mesh);
    return mesh;
}

Palette ReadPalette() {
    Palette palette;
    DWORD size = sizeof(DWORD);
    const auto background = RegGetValueW(HKEY_CURRENT_USER, SettingsKey, L"Background", RRF_RT_REG_DWORD,
                                         nullptr, &palette.background, &size);
    if (background != ERROR_SUCCESS) palette.background = Palette{}.background;
    size = sizeof(DWORD);
    const auto model = RegGetValueW(HKEY_CURRENT_USER, SettingsKey, L"Model", RRF_RT_REG_DWORD,
                                    nullptr, &palette.model, &size);
    if (model != ERROR_SUCCESS) palette.model = Palette{}.model;
    return palette;
}

Frame Render(const Mesh& mesh, int width, int height, const View& view,
             size_t faceBudget, uint64_t pixelBudget, Palette palette, bool transparent,
             size_t first, size_t count) {
    width = std::clamp(width, 1, 1600);
    height = std::clamp(height, 1, 1600);
    const size_t pixels = size_t(width) * height;
    const uint32_t background = transparent ? 0u : (uint32_t(GetRValue(palette.background)) << 16) |
                                (uint32_t(GetGValue(palette.background)) << 8) | GetBValue(palette.background);
    const uint32_t opaque = transparent ? 0xFF000000u : 0u;
    Frame frame{width, height, std::vector<uint32_t>(pixels, background)};
    std::vector<float> depth(pixels, -std::numeric_limits<float>::infinity());
    const float cy = float(std::cos(view.yaw)), sy = float(std::sin(view.yaw));
    const float cp = float(std::cos(view.pitch)), sp = float(std::sin(view.pitch));
    const float scale = float(0.89 * std::min(width, height) * view.zoom);
    const float ox = float(width * (0.5 + view.panX)), oy = float(height * (0.5 + view.panY));
    const auto project = [&](Point point) {
        const float across = cy * point.x - sy * point.y;
        const float away = sy * point.x + cy * point.y;
        return Point{ox + scale * across, oy - scale * (sp * away + cp * point.z),
                     sp * point.z - cp * away};
    };

    first = std::min(first, mesh.triangles.size());
    const size_t total = std::min(count, mesh.triangles.size() - first);
    const size_t faceCount = std::min(total, faceBudget);
    for (size_t i = 0; i < faceCount && pixelBudget > 0; ++i) {
        const auto& face = mesh.triangles[first + size_t(uint64_t(i) * total / faceCount)];
        const Point a = project(face.a), b = project(face.b), c = project(face.c);
        const float area = Edge(a, b, c.x, c.y);
        if (std::abs(area) < 0.0001f) continue;
        const int left = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int top = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        const int right = std::min(width - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int bottom = std::min(height - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        if (left > right || top > bottom) continue;
        const uint64_t work = uint64_t(right - left + 1) * (bottom - top + 1);
        const auto normal = Cross(Subtract(b, a), Subtract(c, a));
        const double nx = normal.x / scale, ny = normal.y / scale, nz = normal.z / (scale * scale);
        const double magnitude = std::sqrt(nx * nx + ny * ny + nz * nz);
        const float light = float(0.38 + 0.62 * std::clamp(std::abs(
            (0.32 * normal.x / scale - 0.53 * normal.y / scale + 0.78 * normal.z / (scale * scale))
            / magnitude), 0.0, 1.0));
        const uint32_t color = opaque | (uint32_t(GetRValue(palette.model) * light) << 16) |
                               (uint32_t(GetGValue(palette.model) * light) << 8) |
                               uint32_t(GetBValue(palette.model) * light);
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                const float u = Edge(b, c, x + 0.5f, y + 0.5f) / area;
                const float v = Edge(c, a, x + 0.5f, y + 0.5f) / area;
                const float w = 1 - u - v;
                if (u < 0 || v < 0 || w < 0) continue;
                const float z = u * a.z + v * b.z + w * c.z;
                const size_t offset = size_t(y) * width + x;
                if (z <= depth[offset]) continue;
                depth[offset] = z;
                frame.pixels[offset] = color;
            }
        }
        pixelBudget -= std::min(pixelBudget, work);
    }
    return frame;
}

View FitView(const Mesh& mesh, View view, double fill) {
    if (mesh.triangles.empty()) return view;
    const double cy = std::cos(view.yaw), sy = std::sin(view.yaw);
    const double cp = std::cos(view.pitch), sp = std::sin(view.pitch);
    const double inf = std::numeric_limits<double>::infinity();
    double left = inf, right = -inf, low = inf, high = -inf;
    for (const auto& t : mesh.triangles) {
        for (const Point& p : {t.a, t.b, t.c}) {
            const double across = cy * p.x - sy * p.y;
            const double up = sp * (sy * p.x + cy * p.y) + cp * p.z;
            left = std::min(left, across);
            right = std::max(right, across);
            low = std::min(low, up);
            high = std::max(high, up);
        }
    }
    const double extent = std::max({right - left, high - low, 1e-9});
    // Render maps one model unit to 0.89 * side * zoom pixels.
    view.zoom = fill / (0.89 * extent);
    const double unit = 0.89 * view.zoom;
    view.panX = -unit * (left + right) * 0.5;
    view.panY = unit * (low + high) * 0.5;
    return view;
}

Frame Downsample(const Frame& frame, int factor) {
    if (factor <= 1) return frame;
    Frame out{frame.width / factor, frame.height / factor, {}};
    out.pixels.resize(size_t(out.width) * out.height);
    const uint32_t samples = uint32_t(factor * factor);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            uint32_t sum[4]{};
            for (int dy = 0; dy < factor; ++dy) {
                const uint32_t* row = frame.pixels.data() + size_t(y * factor + dy) * frame.width + size_t(x) * factor;
                for (int dx = 0; dx < factor; ++dx) {
                    for (int c = 0; c < 4; ++c) sum[c] += (row[dx] >> (8 * c)) & 0xFF;
                }
            }
            uint32_t pixel = 0;
            for (int c = 0; c < 4; ++c) pixel |= ((sum[c] + samples / 2) / samples) << (8 * c);
            out.pixels[size_t(y) * out.width + x] = pixel;
        }
    }
    return out;
}

void PaintFrame(HDC dc, const RECT& bounds, const Frame& frame) {
    if (frame.pixels.empty()) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = frame.width;
    info.bmiHeader.biHeight = -frame.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                  0, 0, frame.width, frame.height, frame.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}
}
