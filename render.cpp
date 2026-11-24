#include "render.h"
#include <algorithm>
#include <cmath>

namespace render {
void ViewMatrices(const stl::View& view, int width, int height, float rotation[4][4], float transform[4][4]) {
    const double cy = std::cos(view.yaw), sy = std::sin(view.yaw);
    const double cp = std::cos(view.pitch), sp = std::sin(view.pitch);
    // eye = (cy x - sy y,  sp (sy x + cy y) + cp z,  -cp (sy x + cy y) + sp z)
    const double r[4][4] = {
        {cy, sp * sy, -cp * sy, 0},
        {-sy, sp * cy, -cp * cy, 0},
        {0, cp, sp, 0},
        {0, 0, 0, 1}};
    // Orthographic: one model unit spans 0.89 * min(width, height) * zoom pixels; depth maps
    // eye z in [2, -2] (near to far) onto [0, 1].
    const double unit = 0.89 * std::max(1, std::min(width, height)) * view.zoom;
    const double sx = 2 * unit / std::max(1, width), syy = 2 * unit / std::max(1, height);
    const double p[4][4] = {
        {sx, 0, 0, 0},
        {0, syy, 0, 0},
        {0, 0, -0.25, 0},
        {2 * view.panX, -2 * view.panY, 0.5, 1}};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            rotation[i][j] = float(r[i][j]);
            double sum = 0;
            for (int k = 0; k < 4; ++k) sum += r[i][k] * p[k][j];
            transform[i][j] = float(sum);
        }
    }
}

void SceneMatrices(const Scene& scene, float rotation[4][4], float transform[4][4]) {
    float view[4][4];
    ViewMatrices(scene.view, scene.width, scene.height, rotation, view);
    // Placement rows (row vectors): scale on the diagonal, -offset * scale as translation.
    const float s = scene.scale;
    const float place[4][4] = {
        {s, 0, 0, 0},
        {0, s, 0, 0},
        {0, 0, s, 0},
        {-scene.offset.x * s, -scene.offset.y * s, -scene.offset.z * s, 1}};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += place[i][k] * view[k][j];
            transform[i][j] = sum;
        }
    }
}

void Project(const float m[4][4], stl::Point p, float clip[3]) {
    for (int j = 0; j < 3; ++j) clip[j] = p.x * m[0][j] + p.y * m[1][j] + p.z * m[2][j] + m[3][j];
}

void Backdrop(COLORREF background, float top[3], float bottom[3]) {
    const int channel[3] = {GetRValue(background), GetGValue(background), GetBValue(background)};
    const int lift[3] = {22, 24, 28};
    for (int i = 0; i < 3; ++i) {
        top[i] = std::clamp(channel[i] + lift[i], 0, 255) / 255.f;
        bottom[i] = std::clamp(channel[i] - 8, 0, 255) / 255.f;
    }
}

Lighting Lights() {
    Lighting light{{0.32f, 0.53f, 0.78f}, {-0.55f, -0.25f, 0.45f}, 0.74f, 0.28f * 0.35f,
                   {0.20f, 0.22f, 0.26f}, 0.30f, 40.f};
    for (float* v : {light.key, light.fill}) {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int i = 0; i < 3; ++i) v[i] /= length;
    }
    return light;
}
}
