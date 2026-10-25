// SPDX-License-Identifier: GPL-3.0-only
#include "gpu_presenter.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <stdexcept>
#include <string>
namespace dust {
using Microsoft::WRL::ComPtr;
namespace {
void checked(HRESULT result, const char *operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed (" +
                                 std::to_string(uint32_t(result)) + ")");
}
constexpr const char *shader = R"(
Texture2D<float4> image : register(t0);
Texture2D<float4> overlay : register(t1);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V vertex(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 pixel(V input) : SV_Target {
    uint width, height; image.GetDimensions(width, height);
    int2 size = int2(width, height);
    int2 nearest = clamp(int2(input.uv * size), int2(0,0), size-1);
    float4 text = overlay.Load(int3(nearest,0));
    // Text, caption plates and selection markers keep their original crisp pixels.
    if (text.a > 0.5) return float4(text.rgb,1);
    return float4(image.Load(int3(nearest,0)).rgb,1);
}
)";
} // namespace
struct GpuPresenter::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Texture2D> image, overlay;
    ComPtr<ID3D11Texture2D> captured;
    ComPtr<ID3D11ShaderResourceView> imageView, overlayView;
    HWND window = nullptr;
    int width = 0, height = 0, sourceWidth = 0, sourceHeight = 0;
    uint64_t count = 0;
    void initialize(HWND host) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = host;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        checked(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                              UINT(std::size(levels)), D3D11_SDK_VERSION, &desc,
                                              &swap, &device, nullptr, &context),
                "GPU initialization");
        ComPtr<IDXGIFactory> factory;
        checked(swap->GetParent(IID_PPV_ARGS(&factory)), "DXGI factory");
        checked(factory->MakeWindowAssociation(host, DXGI_MWA_NO_ALT_ENTER), "window association");
        auto compile = [&](const char *entry, const char *profile) {
            ComPtr<ID3DBlob> code, errors;
            const auto status = D3DCompile(
                shader, std::strlen(shader), "DustSpatialUpscale", nullptr, nullptr, entry, profile,
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(status))
                throw std::runtime_error(errors
                                             ? static_cast<const char *>(errors->GetBufferPointer())
                                             : "GPU shader compilation failed");
            return code;
        };
        auto vs = compile("vertex", "vs_4_0"), ps = compile("pixel", "ps_4_0");
        checked(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                           &vertex),
                "vertex shader");
        checked(
            device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel),
            "pixel shader");
        window = host;
    }
    void resize(int w, int h) {
        if (width == w && height == h)
            return;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        checked(swap->ResizeBuffers(1, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, 0), "GPU resize");
        ComPtr<ID3D11Texture2D> buffer;
        checked(swap->GetBuffer(0, IID_PPV_ARGS(&buffer)), "GPU back buffer");
        checked(device->CreateRenderTargetView(buffer.Get(), nullptr, &target),
                "GPU render target");
        width = w;
        height = h;
    }
    void textures(int w, int h) {
        if (sourceWidth == w && sourceHeight == h)
            return;
        context->PSSetShaderResources(0, 0, nullptr);
        imageView.Reset();
        overlayView.Reset();
        image.Reset();
        overlay.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(w);
        desc.Height = UINT(h);
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        checked(device->CreateTexture2D(&desc, nullptr, &image), "image texture");
        checked(device->CreateTexture2D(&desc, nullptr, &overlay), "overlay texture");
        checked(device->CreateShaderResourceView(image.Get(), nullptr, &imageView), "image view");
        checked(device->CreateShaderResourceView(overlay.Get(), nullptr, &overlayView),
                "overlay view");
        sourceWidth = w;
        sourceHeight = h;
    }
};
GpuPresenter::GpuPresenter() : impl(std::make_unique<Impl>()) {}
GpuPresenter::~GpuPresenter() = default;
void GpuPresenter::reset() {
    impl = std::make_unique<Impl>();
}
uint64_t GpuPresenter::frames() const {
    return impl->count;
}
void GpuPresenter::present(HWND window, std::span<const uint32_t> image,
                           std::span<const uint32_t> overlay, int sw, int sh, RECT viewport,
                           int width, int height, bool capture) {
    if (sw < 1 || sh < 1 || width < 1 || height < 1 || image.size() != size_t(sw) * sh ||
        overlay.size() != image.size())
        throw std::runtime_error("invalid GPU presentation dimensions");
    if (impl->window && impl->window != window)
        reset();
    if (!impl->device)
        impl->initialize(window);
    impl->resize(width, height);
    impl->textures(sw, sh);
    impl->context->UpdateSubresource(impl->image.Get(), 0, nullptr, image.data(), UINT(sw * 4), 0);
    impl->context->UpdateSubresource(impl->overlay.Get(), 0, nullptr, overlay.data(), UINT(sw * 4),
                                     0);
    const float black[] = {0, 0, 0, 1};
    impl->context->ClearRenderTargetView(impl->target.Get(), black);
    auto *target = impl->target.Get();
    impl->context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT view{float(viewport.left),
                        float(viewport.top),
                        float(viewport.right - viewport.left),
                        float(viewport.bottom - viewport.top),
                        0,
                        1};
    impl->context->RSSetViewports(1, &view);
    impl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl->context->VSSetShader(impl->vertex.Get(), nullptr, 0);
    impl->context->PSSetShader(impl->pixel.Get(), nullptr, 0);
    ID3D11ShaderResourceView *views[] = {impl->imageView.Get(), impl->overlayView.Get()};
    impl->context->PSSetShaderResources(0, 2, views);
    impl->context->Draw(3, 0);
    if (capture) {
        ComPtr<ID3D11Texture2D> buffer;
        checked(impl->swap->GetBuffer(0, IID_PPV_ARGS(&buffer)), "capture back buffer");
        D3D11_TEXTURE2D_DESC desc{};
        buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        impl->captured.Reset();
        checked(impl->device->CreateTexture2D(&desc, nullptr, &impl->captured),
                "capture staging texture");
        impl->context->CopyResource(impl->captured.Get(), buffer.Get());
    }
    checked(impl->swap->Present(1, 0), "GPU presentation");
    ++impl->count;
}
std::vector<uint32_t> GpuPresenter::readback(int &width, int &height) {
    if (!impl->captured)
        throw std::runtime_error("no GPU frame requested for capture");
    D3D11_TEXTURE2D_DESC desc{};
    impl->captured->GetDesc(&desc);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(impl->context->Map(impl->captured.Get(), 0, D3D11_MAP_READ, 0, &mapped), "capture map");
    width = int(desc.Width);
    height = int(desc.Height);
    std::vector<uint32_t> result(size_t(width) * height);
    for (int y = 0; y < height; ++y)
        std::memcpy(result.data() + size_t(y) * width,
                    static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                    size_t(width) * 4);
    impl->context->Unmap(impl->captured.Get(), 0);
    return result;
}
} // namespace dust
