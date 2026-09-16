#include "gfx.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace gfx {
namespace {

// ---- HLSL --------------------------------------------------------------
// One fullscreen triangle generated from SV_VertexID; the destination
// rectangle is expressed as a D3D viewport, so no vertex buffer is needed.
const char* kShaderSource = R"HLSL(
cbuffer VideoCB : register(b0)
{
    float2 texSize;      // width of the sampled texture, in texels
    int    colorSpace;   // 601 or 709
    int    useAlpha;     // 1 when a separate alpha plane is bound
};

Texture2D    srcTex   : register(t0);
Texture2D    alphaTex : register(t1);
SamplerState pointSmp : register(s0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 YuvToRgb(float y, float u, float v, int cs)
{
    // Inputs arrive as limited-range 8 bit values normalised to 0..1.
    y = (y - 0.0627451) * 1.1643836;
    u = u - 0.5019608;
    v = v - 0.5019608;

    float3 rgb;
    if (cs == 601)
    {
        rgb.r = y + 1.5960268 * v;
        rgb.g = y - 0.3917623 * u - 0.8129676 * v;
        rgb.b = y + 2.0172321 * u;
    }
    else
    {
        rgb.r = y + 1.7927411 * v;
        rgb.g = y - 0.2132486 * u - 0.5329093 * v;
        rgb.b = y + 2.1124018 * u;
    }
    return saturate(rgb);
}

// Source texture is UYVY reinterpreted as RGBA8 at half width:
// R = U, G = Y0, B = V, A = Y1.
float4 PSUyvy(VSOut i) : SV_Target
{
    float fullWidth = texSize.x * 2.0;
    float px        = floor(i.uv.x * fullWidth);
    float pair      = floor(px * 0.5);

    float2 uv  = float2((pair + 0.5) / texSize.x, i.uv.y);
    float4 t   = srcTex.SampleLevel(pointSmp, uv, 0);

    float y = (fmod(px, 2.0) < 1.0) ? t.g : t.a;
    float3 rgb = YuvToRgb(y, t.r, t.b, colorSpace);

    float a = 1.0;
    if (useAlpha != 0)
        a = alphaTex.SampleLevel(pointSmp, i.uv, 0).r;

    return float4(rgb, a);
}

float4 PSBgra(VSOut i) : SV_Target
{
    float4 c = srcTex.SampleLevel(pointSmp, i.uv, 0);
    if (useAlpha == 0) c.a = 1.0;
    return c;
}
)HLSL";

struct VideoCB {
    float texSize[2];
    int   colorSpace;
    int   useAlpha;
};

// ---- device state ------------------------------------------------------
ComPtr<ID3D11Device>          g_d3d;
ComPtr<ID3D11DeviceContext>   g_ctx;
ComPtr<ID2D1Factory1>         g_d2d_factory;
ComPtr<ID2D1Device>           g_d2d_device;
ComPtr<IDWriteFactory>        g_dwrite;
ComPtr<IDXGIFactory2>         g_dxgi_factory;
ComPtr<ID3D11VertexShader>    g_vs;
ComPtr<ID3D11PixelShader>     g_ps_uyvy;
ComPtr<ID3D11PixelShader>     g_ps_bgra;
ComPtr<ID3D11SamplerState>    g_smp_point;
ComPtr<ID3D11SamplerState>    g_smp_linear;
ComPtr<ID3D11Buffer>          g_video_cb;
ComPtr<ID3D11BlendState>      g_blend_opaque;
ComPtr<ID3D11RasterizerState> g_raster;
ComPtr<IDWriteTextFormat>     g_fonts[6];
bool          g_ready = false;
bool          g_warp  = false;
std::wstring  g_error;

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
                                        const D3D_SHADER_MACRO*, ID3DInclude*,
                                        LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob**, ID3DBlob**);

bool compile_shaders() {
    // d3dcompiler_47.dll ships with Windows 8.1 and later. Load it lazily so a
    // missing compiler degrades to a clear message rather than a load failure.
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_46.dll");
    if (!compiler) {
        g_error = L"d3dcompiler_47.dll could not be loaded.";
        return false;
    }
    auto compile = reinterpret_cast<PFN_D3DCompile>(
        reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")));
    if (!compile) {
        g_error = L"d3dcompiler_47.dll is missing D3DCompile.";
        return false;
    }

    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    const SIZE_T len = std::strlen(kShaderSource);

    auto build = [&](const char* entry, const char* target, ID3DBlob** out) -> bool {
        ComPtr<ID3DBlob> errors;
        HRESULT hr = compile(kShaderSource, len, "omtmini.hlsl", nullptr, nullptr,
                             entry, target, flags, 0, out, errors.put());
        if (FAILED(hr)) {
            const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer())
                                     : "(no compiler output)";
            util::logf("gfx: shader %s failed hr=0x%08lx: %s", entry,
                       static_cast<unsigned long>(hr), msg);
            g_error = L"Shader compilation failed for ";
            g_error += util::widen(entry);
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vs_blob, ps_uyvy_blob, ps_bgra_blob;
    if (!build("VSMain", "vs_4_0", vs_blob.put()))      return false;
    if (!build("PSUyvy", "ps_4_0", ps_uyvy_blob.put())) return false;
    if (!build("PSBgra", "ps_4_0", ps_bgra_blob.put())) return false;

    if (FAILED(g_d3d->CreateVertexShader(vs_blob->GetBufferPointer(),
                                         vs_blob->GetBufferSize(), nullptr, g_vs.put())))
        return false;
    if (FAILED(g_d3d->CreatePixelShader(ps_uyvy_blob->GetBufferPointer(),
                                        ps_uyvy_blob->GetBufferSize(), nullptr, g_ps_uyvy.put())))
        return false;
    if (FAILED(g_d3d->CreatePixelShader(ps_bgra_blob->GetBufferPointer(),
                                        ps_bgra_blob->GetBufferSize(), nullptr, g_ps_bgra.put())))
        return false;
    return true;
}

bool create_pipeline_state() {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(g_d3d->CreateSamplerState(&sd, g_smp_point.put()))) return false;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(g_d3d->CreateSamplerState(&sd, g_smp_linear.put()))) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(VideoCB);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_d3d->CreateBuffer(&bd, nullptr, g_video_cb.put()))) return false;

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_d3d->CreateBlendState(&bld, g_blend_opaque.put()))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(g_d3d->CreateRasterizerState(&rd, g_raster.put()))) return false;

    return true;
}

bool create_fonts() {
    struct Spec { const wchar_t* family; float size; DWRITE_FONT_WEIGHT weight; };
    const Spec specs[6] = {
        { L"Segoe UI",       11.5f, DWRITE_FONT_WEIGHT_NORMAL    },  // Small
        { L"Segoe UI",       13.0f, DWRITE_FONT_WEIGHT_NORMAL    },  // Body
        { L"Segoe UI",       13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },  // BodyBold
        { L"Segoe UI",       16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },  // Title
        { L"Consolas",       11.5f, DWRITE_FONT_WEIGHT_NORMAL    },  // Mono
        { L"Segoe UI",       22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },  // Big
    };
    for (int i = 0; i < 6; ++i) {
        if (FAILED(g_dwrite->CreateTextFormat(specs[i].family, nullptr, specs[i].weight,
                                              DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, specs[i].size,
                                              L"", g_fonts[i].put())))
            return false;
        g_fonts[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return true;
}

} // namespace

// ---- theme -------------------------------------------------------------
D2D1_COLOR_F rgb(unsigned hex, float a) {
    return D2D1::ColorF(static_cast<UINT32>(hex & 0xFFFFFF), a);
}

const Theme& theme() {
    static Theme t = [] {
        Theme v{};
        v.bg        = rgb(0x14161A);
        v.panel     = rgb(0x1C1F25);
        v.panel_hi  = rgb(0x262A32);
        v.panel_sel = rgb(0x2E333D);
        v.border    = rgb(0x303540);
        v.text      = rgb(0xE8EAED);
        v.text_dim  = rgb(0x9AA2AF);
        v.accent    = rgb(0x3B82F6);
        v.accent_hi = rgb(0x60A5FA);
        v.on_accent = rgb(0xFFFFFF);
        v.danger    = rgb(0xEF4444);
        v.ok        = rgb(0x22C55E);
        v.warn      = rgb(0xF59E0B);
        v.tally_pgm = rgb(0xEF4444);
        v.tally_pvw = rgb(0x22C55E);
        return v;
    }();
    return t;
}

// ---- Device ------------------------------------------------------------
bool Device::init() {
    if (g_ready) return true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef OMTMINI_D3D_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   g_d3d.put(), &got, g_ctx.put());
    if (FAILED(hr)) {
        // A machine with no usable GPU (or an RDP session) still gets a window.
        util::logf("gfx: hardware device failed hr=0x%08lx, trying WARP",
                   static_cast<unsigned long>(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               g_d3d.put(), &got, g_ctx.put());
        g_warp = true;
    }
    if (FAILED(hr)) {
        g_error = L"Direct3D 11 device creation failed.";
        util::logf("gfx: D3D11CreateDevice failed hr=0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }

    // Video arrives on receiver threads which upload straight into their own
    // textures, so the immediate context must be thread safe.
    {
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D11Multithread), mt.put_void())))
            mt->SetMultithreadProtected(TRUE);
        else
            util::logf("gfx: ID3D11Multithread unavailable");
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(g_d3d->QueryInterface(__uuidof(IDXGIDevice), dxgi_device.put_void()))) {
        g_error = L"Could not obtain the DXGI device.";
        return false;
    }
    // One frame of queued work is plenty and keeps latency down.
    ComPtr<IDXGIDevice1> dxgi_device1;
    if (SUCCEEDED(dxgi_device->QueryInterface(__uuidof(IDXGIDevice1), dxgi_device1.put_void())))
        dxgi_device1->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(dxgi_device->GetAdapter(adapter.put()))) {
        adapter->GetParent(__uuidof(IDXGIFactory2), g_dxgi_factory.put_void());
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(adapter->GetDesc(&ad)))
            util::logf("gfx: adapter %s", util::narrow(ad.Description).c_str());
    }
    if (!g_dxgi_factory) {
        g_error = L"Could not obtain the DXGI factory.";
        return false;
    }

    D2D1_FACTORY_OPTIONS opts{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                           &opts, g_d2d_factory.put_void());
    if (FAILED(hr)) {
        g_error = L"Direct2D factory creation failed.";
        return false;
    }
    if (FAILED(g_d2d_factory->CreateDevice(dxgi_device.get(), g_d2d_device.put()))) {
        g_error = L"Direct2D device creation failed.";
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(g_dwrite.put()));
    if (FAILED(hr)) {
        g_error = L"DirectWrite factory creation failed.";
        return false;
    }

    if (!compile_shaders())       return false;
    if (!create_pipeline_state()) { g_error = L"Pipeline state creation failed."; return false; }
    if (!create_fonts())          { g_error = L"Font creation failed."; return false; }

    g_ready = true;
    util::logf("gfx: ready (feature level 0x%04x%s)", static_cast<unsigned>(got),
               g_warp ? ", WARP" : "");
    return true;
}

void Device::teardown() {
    for (auto& f : g_fonts) f.reset();
    g_raster.reset();
    g_blend_opaque.reset();
    g_video_cb.reset();
    g_smp_linear.reset();
    g_smp_point.reset();
    g_ps_bgra.reset();
    g_ps_uyvy.reset();
    g_vs.reset();
    g_dwrite.reset();
    g_d2d_device.reset();
    g_d2d_factory.reset();
    g_dxgi_factory.reset();
    if (g_ctx) g_ctx->ClearState();
    g_ctx.reset();
    g_d3d.reset();
    g_ready = false;
}

bool Device::ready() { return g_ready; }
ID3D11Device*        Device::d3d()            { return g_d3d.get(); }
ID3D11DeviceContext* Device::ctx()            { return g_ctx.get(); }
ID2D1Device*         Device::d2d()            { return g_d2d_device.get(); }
ID2D1Factory1*       Device::d2d_factory()    { return g_d2d_factory.get(); }
IDWriteFactory*      Device::dwrite()         { return g_dwrite.get(); }
IDXGIFactory2*       Device::dxgi_factory()   { return g_dxgi_factory.get(); }
ID3D11VertexShader*  Device::vs_fullscreen()  { return g_vs.get(); }
ID3D11PixelShader*   Device::ps_uyvy()        { return g_ps_uyvy.get(); }
ID3D11PixelShader*   Device::ps_bgra()        { return g_ps_bgra.get(); }
ID3D11SamplerState*  Device::sampler_point()  { return g_smp_point.get(); }
ID3D11SamplerState*  Device::sampler_linear() { return g_smp_linear.get(); }
ID3D11Buffer*        Device::video_cb()       { return g_video_cb.get(); }
ID3D11BlendState*    Device::blend_opaque()   { return g_blend_opaque.get(); }
ID3D11RasterizerState* Device::raster()       { return g_raster.get(); }
bool                 Device::is_warp()        { return g_warp; }
const std::wstring&  Device::error()          { return g_error; }

IDWriteTextFormat* text_format(Font f) {
    const int i = static_cast<int>(f);
    return (i >= 0 && i < 6) ? g_fonts[i].get() : nullptr;
}

// ---- Surface -----------------------------------------------------------
bool Surface::create(HWND hwnd) {
    if (!g_ready) return false;
    hwnd_ = hwnd;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    width_  = std::max<UINT>(1, rc.right - rc.left);
    height_ = std::max<UINT>(1, rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = width_;
    desc.Height      = height_;
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = g_dxgi_factory->CreateSwapChainForHwnd(g_d3d.get(), hwnd, &desc,
                                                        nullptr, nullptr, swap_chain_.put());
    if (FAILED(hr)) {
        // FLIP_DISCARD needs Windows 10. Fall back for older systems.
        desc.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
        desc.BufferCount = 1;
        hr = g_dxgi_factory->CreateSwapChainForHwnd(g_d3d.get(), hwnd, &desc,
                                                    nullptr, nullptr, swap_chain_.put());
    }
    if (FAILED(hr)) {
        util::logf("gfx: CreateSwapChainForHwnd failed hr=0x%08lx",
                   static_cast<unsigned long>(hr));
        return false;
    }

    // The app draws its own title bar, so suppress DXGI's Alt+Enter handling.
    g_dxgi_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (FAILED(g_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc_.put()))) {
        util::logf("gfx: CreateDeviceContext failed");
        return false;
    }
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

void Surface::destroy() {
    if (d2d_open_ && dc_) { dc_->EndDraw(); d2d_open_ = false; }
    if (dc_) dc_->SetTarget(nullptr);
    cache_.clear();
    current_rtv_ = nullptr;
    dc_.reset();
    swap_chain_.reset();
    hwnd_ = nullptr;
}

void Surface::resize(UINT width, UINT height) {
    if (!swap_chain_) return;
    width  = std::max<UINT>(1, width);
    height = std::max<UINT>(1, height);
    if (width == width_ && height == height_) return;

    // Every reference to the old buffers must go before ResizeBuffers.
    if (dc_) dc_->SetTarget(nullptr);
    cache_.clear();
    current_rtv_ = nullptr;
    if (g_ctx) g_ctx->OMSetRenderTargets(0, nullptr, nullptr);

    HRESULT hr = swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        util::logf("gfx: ResizeBuffers failed hr=0x%08lx", static_cast<unsigned long>(hr));
        return;
    }
    width_  = width;
    height_ = height;
}

Surface::BufferViews* Surface::views_for(ID3D11Texture2D* tex) {
    for (auto& v : cache_)
        if (v.texture == tex) return &v;

    BufferViews v;
    v.texture = tex;
    if (FAILED(g_d3d->CreateRenderTargetView(tex, nullptr, v.rtv.put()))) return nullptr;

    ComPtr<IDXGISurface> dxgi_surface;
    if (FAILED(tex->QueryInterface(__uuidof(IDXGISurface), dxgi_surface.put_void())))
        return nullptr;

    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    if (FAILED(dc_->CreateBitmapFromDxgiSurface(dxgi_surface.get(), &props, v.bitmap.put())))
        return nullptr;

    cache_.push_back(std::move(v));
    return &cache_.back();
}

bool Surface::begin_frame() {
    if (!swap_chain_ || !dc_) return false;

    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void())))
        return false;

    BufferViews* v = views_for(back.get());
    if (!v) return false;

    current_rtv_ = v->rtv.get();
    dc_->SetTarget(v->bitmap.get());
    return true;
}

void Surface::d3d_target() {
    if (!current_rtv_) return;
    ID3D11RenderTargetView* rtv = current_rtv_;
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    d3d_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
}

void Surface::d3d_viewport(float x, float y, float w, float h) {
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width    = std::max(1.0f, w);
    vp.Height   = std::max(1.0f, h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g_ctx->RSSetViewports(1, &vp);
}

void Surface::clear(const D2D1_COLOR_F& c) {
    if (!current_rtv_) return;
    const float rgba[4] = { c.r, c.g, c.b, c.a };
    g_ctx->ClearRenderTargetView(current_rtv_, rgba);
}

ID2D1DeviceContext* Surface::d2d_begin() {
    if (!dc_) return nullptr;
    if (!d2d_open_) {
        dc_->BeginDraw();
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        d2d_open_ = true;
    }
    return dc_.get();
}

void Surface::d2d_end() {
    if (d2d_open_ && dc_) {
        HRESULT hr = dc_->EndDraw();
        if (FAILED(hr))
            util::logf("gfx: D2D EndDraw hr=0x%08lx", static_cast<unsigned long>(hr));
        d2d_open_ = false;
    }
}

void Surface::present(bool vsync) {
    if (!swap_chain_) return;
    d2d_end();

    // DXGI_PRESENT_TEST is cheap and avoids burning GPU time while the window
    // is fully covered, which matters for a viewer left open all day.
    if (occluded_) {
        HRESULT test = swap_chain_->Present(0, DXGI_PRESENT_TEST);
        if (test == DXGI_STATUS_OCCLUDED) return;
        occluded_ = false;
    }

    HRESULT hr = swap_chain_->Present(vsync ? 1 : 0, 0);
    if (hr == DXGI_STATUS_OCCLUDED) {
        occluded_ = true;
    } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        util::logf("gfx: device lost on present hr=0x%08lx", static_cast<unsigned long>(hr));
    }
    if (dc_) dc_->SetTarget(nullptr);
    current_rtv_ = nullptr;
}

// ---- VideoTexture ------------------------------------------------------
bool VideoTexture::ensure(int tex_width, int height, DXGI_FORMAT format,
                          bool alpha_plane, int alpha_w) {
    const bool need_new = !luma_ || tex_width_ != tex_width || height_ != height ||
                          format_ != format;
    if (need_new) {
        luma_.reset();
        luma_srv_.reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width          = static_cast<UINT>(tex_width);
        td.Height         = static_cast<UINT>(height);
        td.MipLevels      = 1;
        td.ArraySize      = 1;
        td.Format         = format;
        td.SampleDesc.Count = 1;
        td.Usage          = D3D11_USAGE_DYNAMIC;
        td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(g_d3d->CreateTexture2D(&td, nullptr, luma_.put()))) return false;
        if (FAILED(g_d3d->CreateShaderResourceView(luma_.get(), nullptr, luma_srv_.put())))
            return false;

        tex_width_ = tex_width;
        height_    = height;
        format_    = format;
    }

    if (alpha_plane) {
        if (!alpha_ || need_new) {
            alpha_.reset();
            alpha_srv_.reset();

            D3D11_TEXTURE2D_DESC td{};
            td.Width          = static_cast<UINT>(alpha_w);
            td.Height         = static_cast<UINT>(height);
            td.MipLevels      = 1;
            td.ArraySize      = 1;
            td.Format         = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage          = D3D11_USAGE_DYNAMIC;
            td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            if (FAILED(g_d3d->CreateTexture2D(&td, nullptr, alpha_.put()))) return false;
            if (FAILED(g_d3d->CreateShaderResourceView(alpha_.get(), nullptr, alpha_srv_.put())))
                return false;
        }
    } else {
        alpha_.reset();
        alpha_srv_.reset();
    }
    return true;
}

bool VideoTexture::upload(const OMTMediaFrame& frame) {
    if (!g_ready || frame.Type != OMTFrameType_Video) return false;
    if (frame.Width <= 0 || frame.Height <= 0 || !frame.Data) return false;

    const bool alpha_flag = (frame.Flags & OMTVideoFlags_Alpha) != 0;
    codec_ = frame.Codec;

    // Colour space: libomt leaves Undefined meaning "infer from height".
    colorspace_ = (frame.ColorSpace == OMTColorSpace_BT601) ? 601
                : (frame.ColorSpace == OMTColorSpace_BT709) ? 709
                : (frame.Height < 720 ? 601 : 709);

    int   tex_w = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool  want_alpha_plane = false;

    switch (frame.Codec) {
        case OMTCodec_UYVY:
            tex_w = frame.Width / 2;
            fmt   = DXGI_FORMAT_R8G8B8A8_UNORM;
            is_yuv_ = true;
            has_alpha_ = false;
            break;
        case OMTCodec_UYVA:
            tex_w = frame.Width / 2;
            fmt   = DXGI_FORMAT_R8G8B8A8_UNORM;
            is_yuv_ = true;
            has_alpha_ = alpha_flag;
            want_alpha_plane = alpha_flag;
            break;
        case OMTCodec_BGRA:
            tex_w = frame.Width;
            fmt   = DXGI_FORMAT_B8G8R8A8_UNORM;
            is_yuv_ = false;
            has_alpha_ = alpha_flag;
            break;
        default:
            // Anything else means the receiver was configured for a format this
            // build does not render; the caller reports it to the user.
            return false;
    }

    if (tex_w <= 0) return false;
    if (!ensure(tex_w, frame.Height, fmt, want_alpha_plane, frame.Width)) return false;

    width_ = frame.Width;

    const int src_stride = frame.Stride > 0
                         ? frame.Stride
                         : (is_yuv_ ? frame.Width * 2 : frame.Width * 4);
    const int row_bytes  = is_yuv_ ? frame.Width * 2 : frame.Width * 4;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_ctx->Map(luma_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;

    const auto* src = static_cast<const uint8_t*>(frame.Data);
    auto*       dst = static_cast<uint8_t*>(mapped.pData);
    const int   copy = std::min<int>(row_bytes, static_cast<int>(mapped.RowPitch));
    for (int y = 0; y < frame.Height; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
                    src + static_cast<size_t>(y) * src_stride, static_cast<size_t>(copy));
    g_ctx->Unmap(luma_.get(), 0);

    if (want_alpha_plane && alpha_) {
        // UYVA is a UYVY image immediately followed by a full resolution alpha
        // plane of one byte per pixel.
        const uint8_t* aplane = src + static_cast<size_t>(src_stride) * frame.Height;
        D3D11_MAPPED_SUBRESOURCE am{};
        if (SUCCEEDED(g_ctx->Map(alpha_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &am))) {
            auto* adst = static_cast<uint8_t*>(am.pData);
            const int acopy = std::min<int>(frame.Width, static_cast<int>(am.RowPitch));
            for (int y = 0; y < frame.Height; ++y)
                std::memcpy(adst + static_cast<size_t>(y) * am.RowPitch,
                            aplane + static_cast<size_t>(y) * frame.Width,
                            static_cast<size_t>(acopy));
            g_ctx->Unmap(alpha_.get(), 0);
        }
    }
    return true;
}

void VideoTexture::release() {
    alpha_srv_.reset();
    alpha_.reset();
    luma_srv_.reset();
    luma_.reset();
    width_ = height_ = tex_width_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

void VideoTexture::draw(Surface& surface, const D2D1_RECT_F& dest) {
    if (!luma_srv_ || !g_ready) return;

    const float w = dest.right - dest.left;
    const float h = dest.bottom - dest.top;
    if (w < 1.0f || h < 1.0f) return;

    surface.d3d_target();
    surface.d3d_viewport(dest.left, dest.top, w, h);

    VideoCB cb{};
    cb.texSize[0] = static_cast<float>(tex_width_);
    cb.texSize[1] = static_cast<float>(height_);
    cb.colorSpace = colorspace_;
    cb.useAlpha   = (has_alpha_ && (is_yuv_ ? alpha_srv_.get() != nullptr : true)) ? 1 : 0;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_ctx->Map(g_video_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &cb, sizeof(cb));
        g_ctx->Unmap(g_video_cb.get(), 0);
    }

    ID3D11ShaderResourceView* srvs[2] = { luma_srv_.get(), alpha_srv_.get() };
    ID3D11SamplerState*       smp     = g_smp_point.get();
    ID3D11Buffer*             cbuf    = g_video_cb.get();

    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.get(), nullptr, 0);
    g_ctx->PSSetShader(is_yuv_ ? g_ps_uyvy.get() : g_ps_bgra.get(), nullptr, 0);
    g_ctx->PSSetShaderResources(0, 2, srvs);
    g_ctx->PSSetSamplers(0, 1, &smp);
    g_ctx->PSSetConstantBuffers(0, 1, &cbuf);
    g_ctx->RSSetState(g_raster.get());
    const float blend_factor[4] = { 0, 0, 0, 0 };
    g_ctx->OMSetBlendState(g_blend_opaque.get(), blend_factor, 0xFFFFFFFF);
    g_ctx->Draw(3, 0);

    // Unbind so the texture can be mapped again next frame.
    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    g_ctx->PSSetShaderResources(0, 2, none);
}

// ---- helpers -----------------------------------------------------------
D2D1_RECT_F fit_rect(const D2D1_RECT_F& bounds, int src_w, int src_h, float aspect) {
    const float bw = bounds.right - bounds.left;
    const float bh = bounds.bottom - bounds.top;
    if (src_w <= 0 || src_h <= 0 || bw <= 0 || bh <= 0) return bounds;

    float ar = aspect > 0.01f ? aspect
                              : static_cast<float>(src_w) / static_cast<float>(src_h);

    float w = bw;
    float h = w / ar;
    if (h > bh) { h = bh; w = h * ar; }

    const float x = bounds.left + (bw - w) * 0.5f;
    const float y = bounds.top  + (bh - h) * 0.5f;
    return D2D1::RectF(x, y, x + w, y + h);
}

} // namespace gfx
