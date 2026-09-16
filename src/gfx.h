// Shared Direct3D 11 / Direct2D rendering layer.
//
// One D3D11 device is shared by every window. Each window owns a flip-model
// swap chain with a Direct2D device context bound to the same back buffer, so
// video (D3D draw) and UI chrome (D2D draw) composite without an intermediate
// copy. Colour conversion happens in a pixel shader, which is what keeps CPU
// use near zero on a 1080p60 feed.
#pragma once
#include "util.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <vector>

extern "C" {
#include "libomt.h"
}

namespace gfx {

using util::ComPtr;

// ---- theme -------------------------------------------------------------
struct Theme {
    D2D1_COLOR_F bg;          // window background
    D2D1_COLOR_F panel;       // raised surface
    D2D1_COLOR_F panel_hi;    // hovered surface
    D2D1_COLOR_F panel_sel;   // selected surface
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F text_dim;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accent_hi;
    D2D1_COLOR_F on_accent;
    D2D1_COLOR_F danger;
    D2D1_COLOR_F ok;
    D2D1_COLOR_F warn;
    D2D1_COLOR_F tally_pgm;
    D2D1_COLOR_F tally_pvw;
    D2D1_COLOR_F badge_direct;
};
const Theme& theme();

// ---- shared device -----------------------------------------------------
class Device {
public:
    static bool init();
    static void teardown();
    static bool ready();

    static ID3D11Device*        d3d();
    static ID3D11DeviceContext* ctx();
    static ID2D1Device*         d2d();
    static ID2D1Factory1*       d2d_factory();
    static IDWriteFactory*      dwrite();
    static IDXGIFactory2*       dxgi_factory();

    // Video pipeline objects, shared across every viewer window.
    static ID3D11VertexShader*  vs_fullscreen();
    static ID3D11PixelShader*   ps_uyvy();
    static ID3D11PixelShader*   ps_bgra();
    static ID3D11SamplerState*  sampler_point();
    static ID3D11SamplerState*  sampler_linear();
    static ID3D11Buffer*        video_cb();
    static ID3D11BlendState*    blend_opaque();
    static ID3D11RasterizerState* raster();

    // Set if the device was created without hardware support.
    static bool is_warp();
    static const std::wstring& error();

    // Multithread protection makes individual calls safe, but not a sequence
    // of them: two threads each binding a render target and then drawing would
    // interleave and draw into the wrong one. Anything that binds state and
    // then draws must hold this for the whole sequence.
    static void lock();
    static void unlock();
};

// RAII for Device::lock.
class DeviceLock {
public:
    DeviceLock() { Device::lock(); }
    ~DeviceLock() { Device::unlock(); }
    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;
};

// ---- text --------------------------------------------------------------
enum class Font { Small, Body, BodyBold, Title, Mono, Big };
IDWriteTextFormat* text_format(Font f);

// ---- per window surface ------------------------------------------------
class Surface {
public:
    Surface() = default;
    ~Surface() { destroy(); }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    bool create(HWND hwnd);
    void destroy();
    // Call on WM_SIZE. Safe to call with unchanged dimensions.
    void resize(UINT width, UINT height);

    // Acquires the current back buffer. Returns false if the device was lost,
    // in which case the caller should skip the frame.
    bool begin_frame();
    // Binds the back buffer for D3D drawing over the whole window.
    void d3d_target();
    // Restricts subsequent D3D draws to a sub rectangle, in pixels.
    void d3d_viewport(float x, float y, float w, float h);
    void clear(const D2D1_COLOR_F& c);
    // Direct2D pass. Must not be open while issuing D3D draws.
    ID2D1DeviceContext* d2d_begin();
    void d2d_end();
    void present(bool vsync);

    UINT width()  const { return width_; }
    UINT height() const { return height_; }
    HWND hwnd()   const { return hwnd_; }
    bool valid()  const { return swap_chain_.get() != nullptr; }

private:
    // Flip-model back buffers rotate on Present, so views are cached per
    // texture rather than recreated every frame.
    struct BufferViews {
        ID3D11Texture2D*        texture = nullptr;   // weak key, not owned
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID2D1Bitmap1>           bitmap;
    };
    BufferViews* views_for(ID3D11Texture2D* tex);

    HWND                       hwnd_ = nullptr;
    ComPtr<IDXGISwapChain1>    swap_chain_;
    ComPtr<ID2D1DeviceContext> dc_;
    std::vector<BufferViews>   cache_;
    ID3D11RenderTargetView*    current_rtv_ = nullptr;
    UINT width_ = 0, height_ = 0;
    bool d2d_open_ = false;
    bool occluded_ = false;
};

// ---- offscreen target --------------------------------------------------
// A render target that is not a window, for composing a frame to send rather
// than to show. Carries its own Direct2D context so overlays can be drawn onto
// it, and a staging texture for getting the result back to main memory.
class OffscreenTarget {
public:
    ~OffscreenTarget() { destroy(); }

    bool create(int width, int height);
    void destroy();

    void clear(const D2D1_COLOR_F& colour);
    ID3D11RenderTargetView* rtv() const { return rtv_.get(); }

    // Direct2D pass over the same surface. Must not be open while issuing D3D
    // draws, exactly as with a window.
    ID2D1DeviceContext* d2d_begin();
    void d2d_end();

    // Copies the composed frame into main memory as tightly packed BGRA.
    bool read_back(std::vector<uint8_t>* pixels);

    int  width()  const { return width_; }
    int  height() const { return height_; }
    bool valid()  const { return texture_.get() != nullptr; }

private:
    ComPtr<ID3D11Texture2D>        texture_;
    ComPtr<ID3D11Texture2D>        staging_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID2D1DeviceContext>     dc_;
    ComPtr<ID2D1Bitmap1>           bitmap_;
    int  width_ = 0, height_ = 0;
    bool d2d_open_ = false;
};

// ---- video texture -----------------------------------------------------
// Owns the GPU texture(s) for one decoded OMT video frame and draws them with
// the appropriate conversion shader.
class VideoTexture {
public:
    ~VideoTexture() { release(); }

    // Uploads a frame received from libomt. Handles UYVY, UYVA, BGRA and BGRX.
    // Returns false for a format this build cannot render.
    bool upload(const OMTMediaFrame& frame);
    void release();

    // Draws into dest (pixels, already letterboxed by the caller).
    void draw(Surface& surface, const D2D1_RECT_F& dest);
    // Same, onto any render target rather than a window.
    void draw_to(ID3D11RenderTargetView* target, const D2D1_RECT_F& dest);

    int  width()  const { return width_; }
    int  height() const { return height_; }
    bool has_alpha() const { return has_alpha_; }
    bool valid() const { return luma_.get() != nullptr; }
    OMTCodec codec() const { return codec_; }

private:
    bool ensure(int tex_width, int height, DXGI_FORMAT format, bool alpha_plane, int alpha_w);
    // Shader and state setup, once the target and viewport are bound.
    void draw_common();

    ComPtr<ID3D11Texture2D>          luma_;
    ComPtr<ID3D11ShaderResourceView> luma_srv_;
    ComPtr<ID3D11Texture2D>          alpha_;
    ComPtr<ID3D11ShaderResourceView> alpha_srv_;
    int        width_ = 0, height_ = 0, tex_width_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool       has_alpha_ = false;
    bool       is_yuv_ = false;
    int        colorspace_ = 709;
    OMTCodec   codec_ = OMTCodec_UYVY;
};

// ---- small helpers -----------------------------------------------------
D2D1_COLOR_F rgb(unsigned hex, float a = 1.0f);
// Letterboxes src_w x src_h inside bounds, honouring a display aspect ratio.
D2D1_RECT_F fit_rect(const D2D1_RECT_F& bounds, int src_w, int src_h, float aspect);

} // namespace gfx
