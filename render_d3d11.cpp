#include "render.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "shader_mesh_vs.h"
#include "shader_mesh_ps.h"
#include "shader_color_vs.h"
#include "shader_color_ps.h"

using Microsoft::WRL::ComPtr;

namespace render {
namespace {
struct FrameConstants {
    float transform[4][4];
    float rotation[4][4];
    float material[4];
    float keyDirection[4];
    float fillDirection[4];
    float fillColor[4];
    float terms[4];
};
static_assert(sizeof(FrameConstants) % 16 == 0);

// Immutable vertex buffers are split so no single resource exceeds the 128 MiB minimum that
// every Direct3D 11 device must support.
constexpr UINT ChunkVertices = 3 * 1'000'000;
constexpr DXGI_FORMAT ColorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT DepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

ComPtr<IDXGIAdapter1> HighPerformanceAdapter() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> modern;
    if (SUCCEEDED(factory.As(&modern))) {
        for (UINT i = 0; SUCCEEDED(modern->EnumAdapterByGpuPreference(
                 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))); ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return adapter;
        }
    }
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return adapter;
    }
    return nullptr;
}

class D3D11Renderer final : public Renderer {
public:
    bool Create(HWND target, bool allowSoftware) {
        window = target;
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0};
        constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // needed for Direct2D labels
        if (const auto adapter = HighPerformanceAdapter()) {
            D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels),
                              D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        if (!device && allowSoftware) {
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                              D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        if (!device) return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        adapterName = desc.Description;
        // Microsoft Basic Render Driver (WARP) reports vendor 0x1414.
        software = desc.VendorId == 0x1414;
        if (software && !allowSoftware) return false;

        RECT client{};
        GetClientRect(window, &client);
        DXGI_SWAP_CHAIN_DESC1 chain{};
        chain.Width = UINT(std::max(1L, client.right));
        chain.Height = UINT(std::max(1L, client.bottom));
        chain.Format = ColorFormat;
        chain.SampleDesc.Count = 1;
        chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        chain.BufferCount = 2;
        chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        chain.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        chain.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), window, &chain, nullptr, nullptr, &swapChain))) {
            chain.BufferCount = 1;
            chain.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            chain.Flags = 0;
            if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), window, &chain, nullptr, nullptr, &swapChain)))
                return false;
        }
        chainFlags = chain.Flags;
        // Keep at most one frame in flight: dragging then shows the newest pose, not one that is
        // two or three frames old.
        ComPtr<IDXGISwapChain2> waitable;
        if (chainFlags && SUCCEEDED(swapChain.As(&waitable)) && SUCCEEDED(waitable->SetMaximumFrameLatency(1)))
            frameReady = waitable->GetFrameLatencyWaitableObject();
        ComPtr<IDXGIDevice1> latency;
        if (SUCCEEDED(device.As(&latency))) latency->SetMaximumFrameLatency(1);
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

        for (const UINT count : {8u, 4u, 2u}) {
            UINT colorQuality = 0, depthQuality = 0;
            if (SUCCEEDED(device->CheckMultisampleQualityLevels(ColorFormat, count, &colorQuality)) &&
                SUCCEEDED(device->CheckMultisampleQualityLevels(DepthFormat, count, &depthQuality)) &&
                colorQuality > 0 && depthQuality > 0) {
                samples = count;
                break;
            }
        }
        return CreatePipeline() && CreateText();
    }

    bool Upload(std::vector<Vertex> vertices) override {
        meshes.clear();
        for (size_t first = 0; first < vertices.size(); first += ChunkVertices) {
            const UINT count = UINT(std::min<size_t>(ChunkVertices, vertices.size() - first));
            D3D11_BUFFER_DESC desc{count * UINT(sizeof(Vertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
            const D3D11_SUBRESOURCE_DATA data{vertices.data() + first};
            Chunk chunk{nullptr, count};
            if (FAILED(device->CreateBuffer(&desc, &data, &chunk.buffer))) {
                meshes.clear();
                return false;
            }
            meshes.push_back(std::move(chunk));
        }
        return true;
    }

    void Resize(int newWidth, int newHeight) override {
        if (newWidth == width && newHeight == height && backBuffer) return;
        ReleaseTargets();
        width = newWidth;
        height = newHeight;
        if (width <= 0 || height <= 0) return;
        if (FAILED(swapChain->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN, chainFlags))) return;
        CreateTargets();
    }

    bool Draw(const Scene& scene) override {
        if (scene.width <= 0 || scene.height <= 0) return true;
        Resize(scene.width, scene.height);
        if (!backBuffer) return true;
        ID3D11RenderTargetView* target = samples > 1 ? msaaView.Get() : backView.Get();
        context->OMSetRenderTargets(1, &target, depthView.Get());
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);

        FrameConstants frame{};
        SceneMatrices(scene, frame.rotation, frame.transform);

        // Backdrop: two triangles straight in clip space.
        float top[3], bottom[3];
        Backdrop(scene.background, top, bottom);
        const ColorVertex backdrop[] = {
            {-1, -1, 0.5f, bottom[0], bottom[1], bottom[2], 1}, {-1, 1, 0.5f, top[0], top[1], top[2], 1},
            {1, 1, 0.5f, top[0], top[1], top[2], 1}, {-1, -1, 0.5f, bottom[0], bottom[1], bottom[2], 1},
            {1, 1, 0.5f, top[0], top[1], top[2], 1}, {1, -1, 0.5f, bottom[0], bottom[1], bottom[2], 1}};
        DrawColor(backdrop, ARRAYSIZE(backdrop), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Identity(frame),
                  depthOff.Get(), nullptr);

        // Mesh.
        const auto light = Lights();
        const auto model = scene.model;
        frame.material[0] = GetRValue(model) / 255.f;
        frame.material[1] = GetGValue(model) / 255.f;
        frame.material[2] = GetBValue(model) / 255.f;
        frame.material[3] = 1;
        std::memcpy(frame.keyDirection, light.key, sizeof(light.key));
        frame.keyDirection[3] = light.keyDiffuse;
        std::memcpy(frame.fillDirection, light.fill, sizeof(light.fill));
        std::memcpy(frame.fillColor, light.fillDiffuse, sizeof(light.fillDiffuse));
        frame.terms[0] = light.ambient;
        frame.terms[1] = light.keySpecular;
        frame.terms[2] = light.shininess;
        context->UpdateSubresource(constants.Get(), 0, nullptr, &frame, 0, 0);
        context->IASetInputLayout(meshLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(meshVS.Get(), nullptr, 0);
        context->PSSetShader(meshPS.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->OMSetDepthStencilState(depthOn.Get(), 0);
        context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        // Draw the requested vertex range, which may span several chunks.
        const uint64_t rangeStart = scene.first;
        const uint64_t rangeEnd = scene.count == UINT32_MAX ? UINT64_MAX : uint64_t(scene.first) + scene.count;
        uint64_t chunkStart = 0;
        for (const auto& chunk : meshes) {
            const uint64_t chunkEnd = chunkStart + chunk.count;
            const uint64_t from = std::max(rangeStart, chunkStart), to = std::min(rangeEnd, chunkEnd);
            if (from < to) {
                constexpr UINT stride = sizeof(Vertex), offset = 0;
                context->IASetVertexBuffers(0, 1, chunk.buffer.GetAddressOf(), &stride, &offset);
                context->Draw(UINT(to - from), UINT(from - chunkStart));
            }
            chunkStart = chunkEnd;
        }

        // Floor grid (model space, alpha blended).
        if (!scene.grid.empty())
            DrawColor(scene.grid.data(), UINT(scene.grid.size()), D3D11_PRIMITIVE_TOPOLOGY_LINELIST, frame,
                      depthOn.Get(), blend.Get());

        // Axes: Direct3D 11 lines are one pixel wide, so each axis is a screen-aligned quad.
        float tips[3][2]{};
        if (scene.axes) {
            const float thickness = 2.5f * dpiScale;
            ColorVertex quads[18]{};
            float origin[3];
            Project(frame.transform, scene.origin, origin);
            for (int axis = 0; axis < 3; ++axis) {
                stl::Point tip = scene.origin, label = scene.origin;
                (&tip.x)[axis] += scene.axisLength;
                (&label.x)[axis] += scene.axisLength * 1.1f;
                float end[3], labelClip[3];
                Project(frame.transform, tip, end);
                Project(frame.transform, label, labelClip);
                tips[axis][0] = (labelClip[0] + 1) * 0.5f * width;
                tips[axis][1] = (1 - labelClip[1]) * 0.5f * height;
                float dx = (end[0] - origin[0]) * width * 0.5f, dy = (end[1] - origin[1]) * height * 0.5f;
                const float length = std::max(1e-4f, std::sqrt(dx * dx + dy * dy));
                const float nx = -dy / length * thickness / width, ny = dx / length * thickness / height;
                const auto* c = AxisColors[axis];
                const ColorVertex a{origin[0] + nx, origin[1] + ny, origin[2], c[0], c[1], c[2], 1};
                const ColorVertex b{origin[0] - nx, origin[1] - ny, origin[2], c[0], c[1], c[2], 1};
                const ColorVertex d{end[0] + nx, end[1] + ny, end[2], c[0], c[1], c[2], 1};
                const ColorVertex e{end[0] - nx, end[1] - ny, end[2], c[0], c[1], c[2], 1};
                ColorVertex* q = quads + axis * 6;
                q[0] = a; q[1] = d; q[2] = e; q[3] = a; q[4] = e; q[5] = b;
            }
            DrawColor(quads, ARRAYSIZE(quads), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Identity(frame),
                      depthOn.Get(), nullptr);
        }

        if (samples > 1) context->ResolveSubresource(backBuffer.Get(), 0, msaa.Get(), 0, ColorFormat);

        if (scene.axes && text) {
            text->BeginDraw();
            for (int axis = 0; axis < 3; ++axis) {
                const auto* c = AxisColors[axis];
                brush->SetColor(D2D1::ColorF(c[0], c[1], c[2]));
                const float size = 24 * dpiScale;
                const D2D1_RECT_F box{tips[axis][0] - size / 2, tips[axis][1] - size / 2,
                                      tips[axis][0] + size / 2, tips[axis][1] + size / 2};
                text->DrawText(&AxisNames[axis], 1, labelFormat.Get(), box, brush.Get());
            }
            if (text->EndDraw() == D2DERR_RECREATE_TARGET) ReleaseTargets();
        }

        const HRESULT presented = swapChain->Present(1, 0);
        return presented != DXGI_ERROR_DEVICE_REMOVED && presented != DXGI_ERROR_DEVICE_RESET;
    }

    void WaitForFrame() override {
        if (frameReady) WaitForSingleObjectEx(frameReady, 100, TRUE);
    }

    ~D3D11Renderer() override {
        if (frameReady) CloseHandle(frameReady);
    }

    std::wstring Api() const override { return L"Direct3D 11"; }
    std::wstring Adapter() const override { return adapterName; }
    bool Software() const override { return software; }

private:
    struct Chunk {
        ComPtr<ID3D11Buffer> buffer;
        UINT count{};
    };

    static FrameConstants Identity(FrameConstants frame) {
        std::memset(frame.transform, 0, sizeof(frame.transform));
        for (int i = 0; i < 4; ++i) frame.transform[i][i] = 1;
        return frame;
    }

    bool CreatePipeline() {
        if (FAILED(device->CreateVertexShader(MeshVSCode, sizeof(MeshVSCode), nullptr, &meshVS)) ||
            FAILED(device->CreatePixelShader(MeshPSCode, sizeof(MeshPSCode), nullptr, &meshPS)) ||
            FAILED(device->CreateVertexShader(ColorVSCode, sizeof(ColorVSCode), nullptr, &colorVS)) ||
            FAILED(device->CreatePixelShader(ColorPSCode, sizeof(ColorPSCode), nullptr, &colorPS)))
            return false;
        const D3D11_INPUT_ELEMENT_DESC meshInput[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        const D3D11_INPUT_ELEMENT_DESC colorInput[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        if (FAILED(device->CreateInputLayout(meshInput, 2, MeshVSCode, sizeof(MeshVSCode), &meshLayout)) ||
            FAILED(device->CreateInputLayout(colorInput, 2, ColorVSCode, sizeof(ColorVSCode), &colorLayout)))
            return false;
        const D3D11_BUFFER_DESC constantDesc{sizeof(FrameConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
        if (FAILED(device->CreateBuffer(&constantDesc, nullptr, &constants))) return false;

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.MultisampleEnable = samples > 1;
        raster.AntialiasedLineEnable = samples <= 1;
        if (FAILED(device->CreateRasterizerState(&raster, &rasterizer))) return false;

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(device->CreateDepthStencilState(&depth, &depthOn))) return false;
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(device->CreateDepthStencilState(&depth, &depthOff))) return false;

        D3D11_BLEND_DESC blendDesc{};
        auto& rt = blendDesc.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(device->CreateBlendState(&blendDesc, &blend));
    }

    bool CreateText() {
        const UINT dpi = GetDpiForWindow(window);
        dpiScale = (dpi ? dpi : 96) / 96.f;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))) ||
            FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 15 * dpiScale, L"en-us", &labelFormat)))
            return true;   // labels are optional; the scene still renders without them
        labelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return true;
    }

    void CreateTargets() {
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return;
        if (samples > 1) {
            D3D11_TEXTURE2D_DESC color{UINT(width), UINT(height), 1, 1, ColorFormat, {samples, 0},
                                       D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
            if (FAILED(device->CreateTexture2D(&color, nullptr, &msaa)) ||
                FAILED(device->CreateRenderTargetView(msaa.Get(), nullptr, &msaaView))) {
                samples = 1;   // fall back to drawing straight into the back buffer
                msaa.Reset();
                msaaView.Reset();
                ReleaseTargets();
                CreateTargets();
                return;
            }
        } else if (FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &backView))) {
            backBuffer.Reset();
            return;
        }
        D3D11_TEXTURE2D_DESC depthDesc{UINT(width), UINT(height), 1, 1, DepthFormat, {samples, 0},
                                       D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL};
        ComPtr<ID3D11Texture2D> depthTexture;
        if (FAILED(device->CreateTexture2D(&depthDesc, nullptr, &depthTexture)) ||
            FAILED(device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depthView))) {
            backBuffer.Reset();
            return;
        }
        ComPtr<IDXGISurface> surface;
        if (d2d && labelFormat && SUCCEEDED(backBuffer.As(&surface))) {
            const auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(ColorFormat, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (SUCCEEDED(d2d->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &text))) {
                text->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                if (FAILED(text->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush))) text.Reset();
            }
        }
    }

    void ReleaseTargets() {
        if (context) {
            context->OMSetRenderTargets(0, nullptr, nullptr);
            context->Flush();
        }
        brush.Reset();
        text.Reset();
        depthView.Reset();
        msaaView.Reset();
        msaa.Reset();
        backView.Reset();
        backBuffer.Reset();
    }

    void DrawColor(const ColorVertex* vertices, UINT count, D3D11_PRIMITIVE_TOPOLOGY topology,
                   const FrameConstants& frame, ID3D11DepthStencilState* depth, ID3D11BlendState* blending) {
        if (!count) return;
        if (count > colorCapacity) {
            colorCapacity = std::max(count, colorCapacity * 2);
            colorBuffer.Reset();
            D3D11_BUFFER_DESC desc{colorCapacity * UINT(sizeof(ColorVertex)), D3D11_USAGE_DYNAMIC,
                                   D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE};
            if (FAILED(device->CreateBuffer(&desc, nullptr, &colorBuffer))) {
                colorCapacity = 0;
                return;
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(colorBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        std::memcpy(mapped.pData, vertices, count * sizeof(ColorVertex));
        context->Unmap(colorBuffer.Get(), 0);
        context->UpdateSubresource(constants.Get(), 0, nullptr, &frame, 0, 0);
        constexpr UINT stride = sizeof(ColorVertex), offset = 0;
        context->IASetInputLayout(colorLayout.Get());
        context->IASetPrimitiveTopology(topology);
        context->IASetVertexBuffers(0, 1, colorBuffer.GetAddressOf(), &stride, &offset);
        context->VSSetShader(colorVS.Get(), nullptr, 0);
        context->PSSetShader(colorPS.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->OMSetDepthStencilState(depth, 0);
        context->OMSetBlendState(blending, nullptr, 0xFFFFFFFF);
        context->Draw(count, 0);
    }

    HWND window{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11Texture2D> backBuffer, msaa;
    ComPtr<ID3D11RenderTargetView> backView, msaaView;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> meshVS, colorVS;
    ComPtr<ID3D11PixelShader> meshPS, colorPS;
    ComPtr<ID3D11InputLayout> meshLayout, colorLayout;
    ComPtr<ID3D11Buffer> constants, colorBuffer;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depthOn, depthOff;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> labelFormat;
    ComPtr<ID2D1RenderTarget> text;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::vector<Chunk> meshes;
    UINT colorCapacity{}, samples = 1, chainFlags{};
    HANDLE frameReady{};
    int width{}, height{};
    float dpiScale = 1;
    bool software{};
    std::wstring adapterName;
};
}

std::unique_ptr<Renderer> CreateD3D11(HWND window, bool allowSoftware) {
    auto renderer = std::make_unique<D3D11Renderer>();
    if (!renderer->Create(window, allowSoftware)) return nullptr;
    return renderer;
}
}
