#include "render.h"
#include <gl/GL.h>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace render {
namespace {
// opengl32.lib only exports OpenGL 1.1; buffer objects come from the driver.
using GlGenBuffers = void(APIENTRY*)(GLsizei, GLuint*);
using GlBindBuffer = void(APIENTRY*)(GLenum, GLuint);
using GlBufferData = void(APIENTRY*)(GLenum, ptrdiff_t, const void*, GLenum);
using GlDeleteBuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using WglSwapInterval = BOOL(WINAPI*)(int);
using WglChooseFormat = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
constexpr GLenum GlArrayBuffer = 0x8892, GlStaticDraw = 0x88E4;

template <class T> T GlProc(const char* name) {
    const PROC proc = wglGetProcAddress(name);
    const auto value = reinterpret_cast<INT_PTR>(proc);
    if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1) return nullptr;
    return reinterpret_cast<T>(proc);
}

PIXELFORMATDESCRIPTOR BasicFormat() {
    PIXELFORMATDESCRIPTOR format{sizeof(format), 1};
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cAlphaBits = 8;
    format.cDepthBits = 24;
    format.iLayerType = PFD_MAIN_PLANE;
    return format;
}

// Finds an accelerated, multisampled pixel format through WGL_ARB_pixel_format. That needs a
// live context, so a hidden throwaway window is used once. Returns 0 if unsupported.
int FindMultisampleFormat() {
    static const int cached = [] {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        constexpr wchar_t ProbeClass[] = L"StlQuickView.Probe";
        WNDCLASSW probeClass{CS_OWNDC, DefWindowProcW, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, ProbeClass};
        if (!RegisterClassW(&probeClass)) return 0;
        int found = 0;
        if (const HWND probe = CreateWindowExW(0, ProbeClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                               instance, nullptr)) {
            const HDC dc = GetDC(probe);
            auto format = BasicFormat();
            const int basic = ChoosePixelFormat(dc, &format);
            if (basic && SetPixelFormat(dc, basic, &format)) {
                if (const HGLRC context = wglCreateContext(dc)) {
                    if (wglMakeCurrent(dc, context)) {
                        if (const auto choose = GlProc<WglChooseFormat>("wglChoosePixelFormatARB")) {
                            for (const int samples : {8, 4}) {
                                const int attributes[] = {
                                    0x2001, 1,        // WGL_DRAW_TO_WINDOW_ARB
                                    0x2010, 1,        // WGL_SUPPORT_OPENGL_ARB
                                    0x2011, 1,        // WGL_DOUBLE_BUFFER_ARB
                                    0x2013, 0x202B,   // WGL_PIXEL_TYPE_ARB = RGBA
                                    0x2003, 0x2027,   // WGL_ACCELERATION_ARB = FULL
                                    0x2014, 32,       // WGL_COLOR_BITS_ARB
                                    0x201B, 8,        // WGL_ALPHA_BITS_ARB
                                    0x2022, 24,       // WGL_DEPTH_BITS_ARB
                                    0x2041, 1,        // WGL_SAMPLE_BUFFERS_ARB
                                    0x2042, samples,  // WGL_SAMPLES_ARB
                                    0};
                                int index = 0;
                                UINT matches = 0;
                                if (choose(dc, attributes, nullptr, 1, &index, &matches) && matches && index) {
                                    found = index;
                                    break;
                                }
                            }
                        }
                        wglMakeCurrent(nullptr, nullptr);
                    }
                    wglDeleteContext(context);
                }
            }
            ReleaseDC(probe, dc);
            DestroyWindow(probe);
        }
        UnregisterClassW(ProbeClass, instance);
        return found;
    }();
    return cached;
}

class OpenGLRenderer final : public Renderer {
public:
    ~OpenGLRenderer() override {
        if (gl) {
            wglMakeCurrent(dc, gl);
            if (vbo) deleteBuffers(1, &vbo);
            if (glyphs) glDeleteLists(glyphs, 3);
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(gl);
        }
        if (font) DeleteObject(font);
        if (dc) ReleaseDC(window, dc);
    }

    bool Create(HWND target, bool allowSoftware) {
        window = target;
        dc = GetDC(window);   // the window class uses CS_OWNDC, so this DC stays valid
        if (!dc) return false;
        auto format = BasicFormat();
        int index = FindMultisampleFormat();
        if (index && !SetPixelFormat(dc, index, &format)) index = 0;
        if (!index) {
            index = ChoosePixelFormat(dc, &format);
            if (!index || !SetPixelFormat(dc, index, &format)) return false;
        }
        PIXELFORMATDESCRIPTOR got{};
        DescribePixelFormat(dc, index, sizeof(got), &got);
        software = (got.dwFlags & PFD_GENERIC_FORMAT) && !(got.dwFlags & PFD_GENERIC_ACCELERATED);
        if (software && !allowSoftware) return false;
        gl = wglCreateContext(dc);
        // The context stays current on this window for the renderer's lifetime (UI thread only).
        if (!gl || !wglMakeCurrent(dc, gl)) return false;

        genBuffers = GlProc<GlGenBuffers>("glGenBuffers");
        bindBuffer = GlProc<GlBindBuffer>("glBindBuffer");
        bufferData = GlProc<GlBufferData>("glBufferData");
        deleteBuffers = GlProc<GlDeleteBuffers>("glDeleteBuffers");
        if (!Buffers()) {
            genBuffers = GlProc<GlGenBuffers>("glGenBuffersARB");
            bindBuffer = GlProc<GlBindBuffer>("glBindBufferARB");
            bufferData = GlProc<GlBufferData>("glBufferDataARB");
            deleteBuffers = GlProc<GlDeleteBuffers>("glDeleteBuffersARB");
        }
        if (const auto swapInterval = GlProc<WglSwapInterval>("wglSwapIntervalEXT")) swapInterval(1);
        if (const auto* name = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) {
            wchar_t wide[256]{};
            MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide));
            adapter = wide;
        }

        const UINT dpi = GetDpiForWindow(window);
        font = CreateFontW(-MulDiv(15, dpi ? dpi : 96, 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        const auto previous = SelectObject(dc, font);
        glyphs = glGenLists(3);
        if (glyphs && !wglUseFontBitmapsW(dc, L'X', 3, glyphs)) {
            glDeleteLists(glyphs, 3);
            glyphs = 0;
        }
        SelectObject(dc, previous);
        return true;
    }

    bool Upload(std::vector<Vertex> data) override {
        count = GLsizei(data.size());
        vertices.clear();
        if (vbo) {
            deleteBuffers(1, &vbo);
            vbo = 0;
        }
        if (data.empty()) return true;
        if (Buffers()) {
            genBuffers(1, &vbo);
            if (vbo) {
                while (glGetError() != GL_NO_ERROR) {}
                bindBuffer(GlArrayBuffer, vbo);
                bufferData(GlArrayBuffer, ptrdiff_t(data.size() * sizeof(Vertex)), data.data(), GlStaticDraw);
                const bool ok = glGetError() == GL_NO_ERROR;
                bindBuffer(GlArrayBuffer, 0);
                if (ok) return true;
                deleteBuffers(1, &vbo);
                vbo = 0;
            }
        }
        vertices = std::move(data);   // no buffer objects: draw from client memory
        return true;
    }

    void Resize(int, int) override {}

    bool Draw(const Scene& scene) override {
        const int width = scene.width, height = scene.height;
        if (width <= 0 || height <= 0) return true;
        glViewport(0, 0, width, height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        DrawBackdrop(scene.background);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        const auto& view = scene.view;
        const double unit = 0.89 * std::min(width, height) * view.zoom;
        const double w = width / unit, h = height / unit;
        glOrtho(w * (-0.5 - view.panX), w * (0.5 - view.panX), h * (-0.5 + view.panY), h * (0.5 + view.panY), -2, 2);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        const auto light = Lights();
        const GLfloat key[] = {light.key[0], light.key[1], light.key[2], 0};
        const GLfloat keyDiffuse[] = {light.keyDiffuse, light.keyDiffuse, light.keyDiffuse, 1};
        const GLfloat keySpecular[] = {0.28f, 0.28f, 0.28f, 1};
        const GLfloat fill[] = {light.fill[0], light.fill[1], light.fill[2], 0};
        const GLfloat fillDiffuse[] = {light.fillDiffuse[0], light.fillDiffuse[1], light.fillDiffuse[2], 1};
        const GLfloat ambient[] = {light.ambient, light.ambient, light.ambient, 1};
        const GLfloat none[] = {0, 0, 0, 1};
        const auto fg = scene.model;
        const GLfloat material[] = {GetRValue(fg) / 255.f, GetGValue(fg) / 255.f, GetBValue(fg) / 255.f, 1};
        const GLfloat shine[] = {0.35f, 0.35f, 0.35f, 1};
        glLightfv(GL_LIGHT0, GL_POSITION, key);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, keyDiffuse);
        glLightfv(GL_LIGHT0, GL_SPECULAR, keySpecular);
        glLightfv(GL_LIGHT1, GL_POSITION, fill);
        glLightfv(GL_LIGHT1, GL_DIFFUSE, fillDiffuse);
        glLightfv(GL_LIGHT1, GL_SPECULAR, none);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, material);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, shine);
        glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, light.shininess);
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glEnable(GL_LIGHT1);

        float rotation[4][4], transform[4][4];
        ViewMatrices(view, width, height, rotation, transform);
        glLoadMatrixf(&rotation[0][0]);   // row-major for row vectors == OpenGL column-major

        if (count > 0 && (vbo || !vertices.empty())) {
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_NORMAL_ARRAY);
            // With a bound VBO the "pointers" are byte offsets into the buffer.
            uintptr_t base = reinterpret_cast<uintptr_t>(vertices.data());
            if (vbo) {
                bindBuffer(GlArrayBuffer, vbo);
                base = 0;
            }
            glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<const void*>(base + offsetof(Vertex, p)));
            glNormalPointer(GL_FLOAT, sizeof(Vertex), reinterpret_cast<const void*>(base + offsetof(Vertex, n)));
            const GLint first = GLint(std::min<uint32_t>(scene.first, uint32_t(count)));
            const GLsizei drawn = GLsizei(std::min<int64_t>(scene.count, int64_t(count) - first));
            glDrawArrays(GL_TRIANGLES, first, drawn);
            if (vbo) bindBuffer(GlArrayBuffer, 0);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_NORMAL_ARRAY);
        }
        glDisable(GL_LIGHTING);

        if (!scene.grid.empty()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glLineWidth(1);
            glBegin(GL_LINES);
            for (const auto& v : scene.grid) {
                glColor4f(v.r, v.g, v.b, v.a);
                glVertex3f(v.x, v.y, v.z);
            }
            glEnd();
            glDisable(GL_BLEND);
        }
        if (scene.axes) DrawAxes(scene);
        SwapBuffers(dc);
        glFinish();   // keep the driver from queueing frames ahead, so dragging stays responsive
        return true;
    }

    std::wstring Api() const override { return L"OpenGL"; }
    std::wstring Adapter() const override { return adapter; }
    bool Software() const override { return software; }

private:
    bool Buffers() const { return genBuffers && bindBuffer && bufferData && deleteBuffers; }

    static void DrawBackdrop(COLORREF background) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        float top[3], bottom[3];
        Backdrop(background, top, bottom);
        glBegin(GL_QUADS);
        glColor3fv(bottom);
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glColor3fv(top);
        glVertex2f(1, 1);
        glVertex2f(-1, 1);
        glEnd();
    }

    void DrawAxes(const Scene& scene) const {
        const auto o = scene.origin;
        glLineWidth(2.5f);
        glBegin(GL_LINES);
        for (int axis = 0; axis < 3; ++axis) {
            float tip[3] = {o.x, o.y, o.z};
            tip[axis] += scene.axisLength;
            glColor3fv(AxisColors[axis]);
            glVertex3f(o.x, o.y, o.z);
            glVertex3fv(tip);
        }
        glEnd();
        glLineWidth(1);
        if (!glyphs) return;
        glDisable(GL_DEPTH_TEST);
        for (int axis = 0; axis < 3; ++axis) {
            float tip[3] = {o.x, o.y, o.z};
            tip[axis] += scene.axisLength * 1.1f;
            glColor3fv(AxisColors[axis]);
            glRasterPos3fv(tip);
            glCallList(glyphs + axis);
        }
        glEnable(GL_DEPTH_TEST);
    }

    HWND window{};
    HDC dc{};
    HGLRC gl{};
    HFONT font{};
    GLuint vbo{}, glyphs{};
    GLsizei count{};
    std::vector<Vertex> vertices;
    GlGenBuffers genBuffers{};
    GlBindBuffer bindBuffer{};
    GlBufferData bufferData{};
    GlDeleteBuffers deleteBuffers{};
    bool software{};
    std::wstring adapter;
};
}

std::unique_ptr<Renderer> CreateOpenGL(HWND window, bool allowSoftware) {
    auto renderer = std::make_unique<OpenGLRenderer>();
    if (!renderer->Create(window, allowSoftware)) return nullptr;
    return renderer;
}
}
