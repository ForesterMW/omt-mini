// OMT Mini virtual camera: a DirectShow push source filter that publishes the
// frames OMT Mini writes into shared memory as a normal capture device.
//
// Written against the raw COM interfaces rather than the DirectShow base class
// library, which is an MSVC sample and is not available to a mingw-w64 cross
// build. The filter is deliberately conservative: it is loaded into other
// people's processes, so every path falls back to a black frame rather than
// failing a call.
#include <windows.h>
#include <dshow.h>
#include <olectl.h>
#include <strsafe.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <new>

#include "../src/vcam_shm.h"

// {6B7A9E54-2C1D-4F8B-9E3A-7D5C2F1A8B40}
static const GUID CLSID_OMTMiniVCam =
    { 0x6b7a9e54, 0x2c1d, 0x4f8b, { 0x9e, 0x3a, 0x7d, 0x5c, 0x2f, 0x1a, 0x8b, 0x40 } };
static const GUID CLSID_SystemMemoryAllocator =
    { 0x1e651cc0, 0xb199, 0x11d0, { 0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45 } };

static const wchar_t* kFilterName = L"OMT Mini Virtual Camera";
static const wchar_t* kClsidText  = L"{6B7A9E54-2C1D-4F8B-9E3A-7D5C2F1A8B40}";

static HMODULE g_module = nullptr;
static LONG    g_locks  = 0;

// ---------------------------------------------------------------- formats --
struct FormatSpec { int width; int height; };
static const FormatSpec kFormats[] = {
    { 1920, 1080 }, { 1280, 720 }, { 960, 540 }, { 640, 360 }, { 640, 480 },
};
static const int kFormatCount = static_cast<int>(sizeof(kFormats) / sizeof(kFormats[0]));
static const int kDefaultFormat = 1;   // 1280x720
static const REFERENCE_TIME kDefaultFrameTime = 333333;   // 30 fps

static void free_media_type(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
    mt.pbFormat = nullptr;
    mt.cbFormat = 0;
    mt.pUnk = nullptr;
}

static bool copy_media_type(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
    if (!dst || !src) return false;
    *dst = *src;
    if (src->cbFormat && src->pbFormat) {
        dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src->cbFormat));
        if (!dst->pbFormat) { dst->cbFormat = 0; return false; }
        std::memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    } else {
        dst->pbFormat = nullptr;
        dst->cbFormat = 0;
    }
    if (dst->pUnk) dst->pUnk->AddRef();
    return true;
}

// Builds a VIDEOINFOHEADER media type. RGB32 is bottom-up (positive biHeight),
// YUY2 is top-down and carries a negative height by convention here.
static bool build_media_type(AM_MEDIA_TYPE* mt, int index, bool yuy2,
                             REFERENCE_TIME frame_time) {
    if (index < 0 || index >= kFormatCount) return false;
    const int w = kFormats[index].width;
    const int h = kFormats[index].height;

    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) return false;
    std::memset(vih, 0, sizeof(VIDEOINFOHEADER));

    vih->AvgTimePerFrame = frame_time;
    vih->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth       = w;
    vih->bmiHeader.biHeight      = h;
    vih->bmiHeader.biPlanes      = 1;
    vih->bmiHeader.biBitCount    = yuy2 ? 16 : 32;
    vih->bmiHeader.biCompression = yuy2 ? MAKEFOURCC('Y', 'U', 'Y', '2') : BI_RGB;
    vih->bmiHeader.biSizeImage   = static_cast<DWORD>(w * h * (yuy2 ? 2 : 4));
    vih->dwBitRate = static_cast<DWORD>(
        static_cast<double>(vih->bmiHeader.biSizeImage) * 8.0 * 1e7 /
        static_cast<double>(frame_time ? frame_time : kDefaultFrameTime));

    std::memset(mt, 0, sizeof(AM_MEDIA_TYPE));
    mt->majortype            = MEDIATYPE_Video;
    mt->subtype              = yuy2 ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_RGB32;
    mt->formattype           = FORMAT_VideoInfo;
    mt->bFixedSizeSamples    = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize          = vih->bmiHeader.biSizeImage;
    mt->cbFormat             = sizeof(VIDEOINFOHEADER);
    mt->pbFormat             = reinterpret_cast<BYTE*>(vih);
    return true;
}

// ------------------------------------------------------------ shm reader ---
class ShmReader {
public:
    ~ShmReader() { close(); }

    bool open() {
        if (header_) return true;
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, VCAM_SHM_NAME);
        if (!mapping_) return false;
        base_ = static_cast<const uint8_t*>(
            MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, VCAM_TOTAL_BYTES));
        if (!base_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
        header_ = reinterpret_cast<const VCamHeader*>(base_);
        if (header_->magic != VCAM_MAGIC || header_->version != VCAM_VERSION) {
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (base_)    { UnmapViewOfFile(base_); base_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        header_ = nullptr;
    }

    // Returns a pointer to the newest complete frame, or null when the
    // producer is absent or stale.
    const uint8_t* latest(int* width, int* height) {
        if (!header_ && !open()) return nullptr;
        if (!header_) return nullptr;

        if (!header_->active) return nullptr;
        const uint64_t now = GetTickCount64();
        if (now > header_->heartbeat_ms + VCAM_STALE_MS) return nullptr;

        const uint32_t w = header_->width;
        const uint32_t h = header_->height;
        if (w == 0 || h == 0 || w > VCAM_MAX_WIDTH || h > VCAM_MAX_HEIGHT) return nullptr;

        const uint32_t idx = header_->write_index;
        if (idx >= VCAM_BUFFERS) return nullptr;
        MemoryBarrier();

        *width  = static_cast<int>(w);
        *height = static_cast<int>(h);
        return base_ + VCAM_HEADER_BYTES + static_cast<size_t>(idx) * VCAM_FRAME_BYTES;
    }

private:
    HANDLE            mapping_ = nullptr;
    const uint8_t*    base_ = nullptr;
    const VCamHeader* header_ = nullptr;
};

// ------------------------------------------------------------- conversion --
// Nearest neighbour scale, chosen over bilinear because this runs inside the
// host application's process and predictable cost matters more than a little
// extra quality on a webcam feed.
static void scale_to(const uint8_t* src, int sw, int sh,
                     uint8_t* dst, int dw, int dh) {
    const int src_stride = sw * 4;
    const int dst_stride = dw * 4;
    for (int y = 0; y < dh; ++y) {
        const int sy = (sh > 1 && dh > 1) ? (y * (sh - 1)) / (dh - 1) : 0;
        const uint8_t* srow = src + static_cast<size_t>(sy) * src_stride;
        uint8_t* drow = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < dw; ++x) {
            const int sx = (sw > 1 && dw > 1) ? (x * (sw - 1)) / (dw - 1) : 0;
            std::memcpy(drow + x * 4, srow + sx * 4, 4);
        }
    }
}

static void bgra_to_rgb32_flipped(const uint8_t* src, uint8_t* dst, int w, int h) {
    const int stride = w * 4;
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + static_cast<size_t>(h - 1 - y) * stride,
                    src + static_cast<size_t>(y) * stride, static_cast<size_t>(stride));
}

static void bgra_to_yuy2(const uint8_t* src, uint8_t* dst, int w, int h) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * w * 4;
        uint8_t* d = dst + static_cast<size_t>(y) * w * 2;
        for (int x = 0; x < w; x += 2) {
            const int b0 = s[x * 4 + 0], g0 = s[x * 4 + 1], r0 = s[x * 4 + 2];
            const int x1 = (x + 1 < w) ? x + 1 : x;
            const int b1 = s[x1 * 4 + 0], g1 = s[x1 * 4 + 1], r1 = s[x1 * 4 + 2];

            const int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16;
            const int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;
            const int rb = (r0 + r1) / 2, gb = (g0 + g1) / 2, bb = (b0 + b1) / 2;
            const int u  = ((-38 * rb - 74 * gb + 112 * bb + 128) >> 8) + 128;
            const int v  = ((112 * rb - 94 * gb - 18 * bb + 128) >> 8) + 128;

            d[x * 2 + 0] = static_cast<uint8_t>(std::clamp(y0, 0, 255));
            d[x * 2 + 1] = static_cast<uint8_t>(std::clamp(u, 0, 255));
            d[x * 2 + 2] = static_cast<uint8_t>(std::clamp(y1, 0, 255));
            d[x * 2 + 3] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }
}

class VCamFilter;

// ------------------------------------------------------------------- pin ---
class VCamPin : public IPin, public IAMStreamConfig, public IKsPropertySet, public IQualityControl {
public:
    explicit VCamPin(VCamFilter* filter);
    virtual ~VCamPin();

    // IUnknown, delegated to the owning filter for lifetime.
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet: how DirectShow asks "what category of pin are you".
    STDMETHODIMP Set(REFGUID, DWORD, void*, DWORD, void*, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, void* pInstanceData,
                     DWORD cbInstanceData, void* pPropData, DWORD cbPropData,
                     DWORD* pcbReturned) override;
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    // IQualityControl
    STDMETHODIMP Notify(IBaseFilter*, Quality) override { return S_OK; }
    STDMETHODIMP SetSink(IQualityControl*) override { return S_OK; }

    HRESULT start_streaming();
    HRESULT stop_streaming();
    bool    is_connected() const { return connected_ != nullptr; }

private:
    static DWORD WINAPI thread_proc(void* param);
    void stream_loop();
    void fill_sample(BYTE* out, long out_size);

    VCamFilter*    filter_;
    IPin*          connected_ = nullptr;
    IMemInputPin*  mem_input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;
    AM_MEDIA_TYPE  mt_{};
    bool           has_mt_ = false;

    HANDLE thread_ = nullptr;
    HANDLE stop_event_ = nullptr;

    int  format_index_ = kDefaultFormat;
    bool yuy2_ = false;
    REFERENCE_TIME frame_time_ = kDefaultFrameTime;

    ShmReader shm_;
    std::vector<uint8_t> scratch_;
    REFERENCE_TIME stream_time_ = 0;
    LONGLONG frame_number_ = 0;
};

// ---------------------------------------------------------------- filter ---
class VCamFilter : public IBaseFilter, public IAMFilterMiscFlags {
public:
    VCamFilter();
    virtual ~VCamFilter();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

    FILTER_STATE     state() const { return state_; }
    IReferenceClock* clock() const { return clock_; }
    REFERENCE_TIME   start_time() const { return start_time_; }
    VCamPin*         pin() { return pin_; }
    CRITICAL_SECTION* lock() { return &lock_; }

private:
    LONG              ref_ = 1;
    FILTER_STATE      state_ = State_Stopped;
    VCamPin*          pin_ = nullptr;
    IFilterGraph*     graph_ = nullptr;
    IReferenceClock*  clock_ = nullptr;
    REFERENCE_TIME    start_time_ = 0;
    WCHAR             name_[128] = {};
    CRITICAL_SECTION  lock_{};
};

// ------------------------------------------------------------ enumerators --
class EnumPins : public IEnumPins {
public:
    EnumPins(IPin* pin, ULONG pos) : pin_(pin), pos_(pos) { if (pin_) pin_->AddRef(); }
    virtual ~EnumPins() { if (pin_) pin_->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        ULONG fetched = 0;
        if (cPins > 0 && pos_ == 0 && pin_) {
            pin_->AddRef();
            ppPins[0] = pin_;
            fetched = 1;
            pos_ = 1;
        }
        if (pcFetched) *pcFetched = fetched;
        return fetched == cPins ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cPins) override {
        pos_ += cPins;
        return pos_ > 1 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new (std::nothrow) EnumPins(pin_, pos_);
        return *ppEnum ? S_OK : E_OUTOFMEMORY;
    }

private:
    LONG  ref_ = 1;
    IPin* pin_ = nullptr;
    ULONG pos_ = 0;
};

class EnumMediaTypes : public IEnumMediaTypes {
public:
    explicit EnumMediaTypes(ULONG pos, REFERENCE_TIME frame_time)
        : pos_(pos), frame_time_(frame_time) {}
    virtual ~EnumMediaTypes() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    // Media types are enumerated RGB32 first for every size, then YUY2.
    STDMETHODIMP Next(ULONG cTypes, AM_MEDIA_TYPE** ppTypes, ULONG* pcFetched) override {
        if (!ppTypes) return E_POINTER;
        ULONG fetched = 0;
        const ULONG total = static_cast<ULONG>(kFormatCount) * 2;
        while (fetched < cTypes && pos_ < total) {
            auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (!mt) break;
            const bool yuy2 = pos_ >= static_cast<ULONG>(kFormatCount);
            const int idx = static_cast<int>(yuy2 ? pos_ - kFormatCount : pos_);
            if (!build_media_type(mt, idx, yuy2, frame_time_)) {
                CoTaskMemFree(mt);
                break;
            }
            ppTypes[fetched++] = mt;
            ++pos_;
        }
        if (pcFetched) *pcFetched = fetched;
        return fetched == cTypes ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cTypes) override {
        pos_ += cTypes;
        return pos_ > static_cast<ULONG>(kFormatCount) * 2 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new (std::nothrow) EnumMediaTypes(pos_, frame_time_);
        return *ppEnum ? S_OK : E_OUTOFMEMORY;
    }

private:
    LONG  ref_ = 1;
    ULONG pos_ = 0;
    REFERENCE_TIME frame_time_ = kDefaultFrameTime;
};

// ------------------------------------------------------------ VCamPin impl -
VCamPin::VCamPin(VCamFilter* filter) : filter_(filter) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    build_media_type(&mt_, kDefaultFormat, false, kDefaultFrameTime);
    has_mt_ = true;
}

VCamPin::~VCamPin() {
    stop_streaming();
    Disconnect();
    if (has_mt_) free_media_type(mt_);
    if (stop_event_) CloseHandle(stop_event_);
}

STDMETHODIMP VCamPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin)      *ppv = static_cast<IPin*>(this);
    else if (riid == IID_IAMStreamConfig)              *ppv = static_cast<IAMStreamConfig*>(this);
    else if (riid == IID_IKsPropertySet)               *ppv = static_cast<IKsPropertySet*>(this);
    else if (riid == IID_IQualityControl)              *ppv = static_cast<IQualityControl*>(this);
    else { *ppv = nullptr; return E_NOINTERFACE; }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) VCamPin::AddRef()  { return filter_->AddRef(); }
STDMETHODIMP_(ULONG) VCamPin::Release() { return filter_->Release(); }

STDMETHODIMP VCamPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    EnterCriticalSection(filter_->lock());

    if (connected_) { LeaveCriticalSection(filter_->lock()); return VFW_E_ALREADY_CONNECTED; }
    if (filter_->state() != State_Stopped) {
        LeaveCriticalSection(filter_->lock());
        return VFW_E_NOT_STOPPED;
    }

    AM_MEDIA_TYPE chosen{};
    bool have_chosen = false;

    // An explicit, fully specified type from the graph wins.
    if (pmt && pmt->majortype == MEDIATYPE_Video && pmt->formattype == FORMAT_VideoInfo &&
        pmt->pbFormat && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        if (QueryAccept(pmt) == S_OK && copy_media_type(&chosen, pmt)) {
            if (SUCCEEDED(pReceivePin->ReceiveConnection(this, &chosen))) have_chosen = true;
            else free_media_type(chosen);
        }
    }

    // Otherwise offer our list, preferred first.
    if (!have_chosen) {
        for (int pass = 0; pass < 2 && !have_chosen; ++pass) {
            const bool yuy2 = (pass == 1);
            // Try the currently selected size first, then the rest.
            for (int n = 0; n < kFormatCount && !have_chosen; ++n) {
                const int idx = (n == 0) ? format_index_
                                         : ((n <= format_index_) ? n - 1 : n);
                AM_MEDIA_TYPE candidate{};
                if (!build_media_type(&candidate, idx, yuy2, frame_time_)) continue;
                if (SUCCEEDED(pReceivePin->ReceiveConnection(this, &candidate))) {
                    chosen = candidate;
                    have_chosen = true;
                    format_index_ = idx;
                    yuy2_ = yuy2;
                } else {
                    free_media_type(candidate);
                }
            }
        }
    }

    if (!have_chosen) {
        LeaveCriticalSection(filter_->lock());
        return VFW_E_NO_ACCEPTABLE_TYPES;
    }

    // Negotiate the allocator with the downstream pin.
    IMemInputPin* mem = nullptr;
    if (FAILED(pReceivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&mem))) ||
        !mem) {
        pReceivePin->Disconnect();
        free_media_type(chosen);
        LeaveCriticalSection(filter_->lock());
        return VFW_E_NO_TRANSPORT;
    }

    IMemAllocator* allocator = nullptr;
    if (FAILED(mem->GetAllocator(&allocator)) || !allocator) {
        CoCreateInstance(CLSID_SystemMemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IMemAllocator, reinterpret_cast<void**>(&allocator));
    }
    if (!allocator) {
        mem->Release();
        pReceivePin->Disconnect();
        free_media_type(chosen);
        LeaveCriticalSection(filter_->lock());
        return VFW_E_NO_ALLOCATOR;
    }

    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(chosen.pbFormat);
    ALLOCATOR_PROPERTIES request{}, actual{};
    request.cBuffers  = 4;
    request.cbBuffer  = static_cast<long>(vih->bmiHeader.biSizeImage);
    request.cbAlign   = 1;
    request.cbPrefix  = 0;

    // Honour what the downstream pin asks for, where it asks for more.
    ALLOCATOR_PROPERTIES downstream{};
    if (SUCCEEDED(mem->GetAllocatorRequirements(&downstream))) {
        if (downstream.cBuffers > request.cBuffers) request.cBuffers = downstream.cBuffers;
        if (downstream.cbAlign  > 0)                request.cbAlign  = downstream.cbAlign;
        if (downstream.cbPrefix > 0)                request.cbPrefix = downstream.cbPrefix;
    }

    HRESULT hr = allocator->SetProperties(&request, &actual);
    if (SUCCEEDED(hr) && actual.cbBuffer < request.cbBuffer) hr = VFW_E_BUFFER_UNDERFLOW;
    if (SUCCEEDED(hr)) hr = mem->NotifyAllocator(allocator, FALSE);
    if (FAILED(hr)) {
        allocator->Release();
        mem->Release();
        pReceivePin->Disconnect();
        free_media_type(chosen);
        LeaveCriticalSection(filter_->lock());
        return hr;
    }

    if (has_mt_) free_media_type(mt_);
    mt_ = chosen;
    has_mt_ = true;
    frame_time_ = vih->AvgTimePerFrame ? vih->AvgTimePerFrame : kDefaultFrameTime;
    yuy2_ = (mt_.subtype == MEDIASUBTYPE_YUY2);

    connected_ = pReceivePin;
    connected_->AddRef();
    mem_input_ = mem;
    allocator_ = allocator;

    LeaveCriticalSection(filter_->lock());
    return S_OK;
}

STDMETHODIMP VCamPin::Disconnect() {
    EnterCriticalSection(filter_->lock());
    if (filter_->state() != State_Stopped) {
        LeaveCriticalSection(filter_->lock());
        return VFW_E_NOT_STOPPED;
    }
    if (allocator_) { allocator_->Decommit(); allocator_->Release(); allocator_ = nullptr; }
    if (mem_input_) { mem_input_->Release(); mem_input_ = nullptr; }
    if (connected_) { connected_->Release(); connected_ = nullptr; }
    LeaveCriticalSection(filter_->lock());
    return S_OK;
}

STDMETHODIMP VCamPin::ConnectedTo(IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    EnterCriticalSection(filter_->lock());
    *ppPin = connected_;
    if (connected_) connected_->AddRef();
    LeaveCriticalSection(filter_->lock());
    return connected_ ? S_OK : VFW_E_NOT_CONNECTED;
}

STDMETHODIMP VCamPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    EnterCriticalSection(filter_->lock());
    const bool ok = connected_ && has_mt_ && copy_media_type(pmt, &mt_);
    LeaveCriticalSection(filter_->lock());
    return ok ? S_OK : VFW_E_NOT_CONNECTED;
}

STDMETHODIMP VCamPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = static_cast<IBaseFilter*>(filter_);
    filter_->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    lstrcpynW(pInfo->achName, L"Capture", MAX_PIN_NAME);
    return S_OK;
}

STDMETHODIMP VCamPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP VCamPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    const size_t bytes = sizeof(WCHAR) * 8;   // "Capture" + null
    auto* buf = static_cast<WCHAR*>(CoTaskMemAlloc(bytes));
    if (!buf) return E_OUTOFMEMORY;
    lstrcpynW(buf, L"Capture", 8);
    *Id = buf;
    return S_OK;
}

STDMETHODIMP VCamPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video) return S_FALSE;
    if (pmt->subtype != MEDIASUBTYPE_RGB32 && pmt->subtype != MEDIASUBTYPE_YUY2) return S_FALSE;
    if (pmt->formattype != FORMAT_VideoInfo) return S_FALSE;
    if (!pmt->pbFormat || pmt->cbFormat < sizeof(VIDEOINFOHEADER)) return S_FALSE;

    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
    const long w = vih->bmiHeader.biWidth;
    const long h = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight
                                               : vih->bmiHeader.biHeight;
    for (int i = 0; i < kFormatCount; ++i)
        if (kFormats[i].width == w && kFormats[i].height == h) return S_OK;
    return S_FALSE;
}

STDMETHODIMP VCamPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) ::EnumMediaTypes(0, frame_time_);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

// ---- IAMStreamConfig ----
STDMETHODIMP VCamPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (QueryAccept(pmt) != S_OK) return E_INVALIDARG;

    EnterCriticalSection(filter_->lock());
    if (connected_) {
        LeaveCriticalSection(filter_->lock());
        return VFW_E_ALREADY_CONNECTED;
    }

    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
    const long w = vih->bmiHeader.biWidth;
    const long h = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight
                                               : vih->bmiHeader.biHeight;
    for (int i = 0; i < kFormatCount; ++i) {
        if (kFormats[i].width == w && kFormats[i].height == h) { format_index_ = i; break; }
    }
    yuy2_ = (pmt->subtype == MEDIASUBTYPE_YUY2);
    if (vih->AvgTimePerFrame > 0) frame_time_ = vih->AvgTimePerFrame;

    if (has_mt_) free_media_type(mt_);
    has_mt_ = copy_media_type(&mt_, pmt);

    LeaveCriticalSection(filter_->lock());
    return S_OK;
}

STDMETHODIMP VCamPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return E_OUTOFMEMORY;

    EnterCriticalSection(filter_->lock());
    const bool ok = has_mt_ ? copy_media_type(mt, &mt_)
                            : build_media_type(mt, format_index_, yuy2_, frame_time_);
    LeaveCriticalSection(filter_->lock());

    if (!ok) { CoTaskMemFree(mt); return E_OUTOFMEMORY; }
    *ppmt = mt;
    return S_OK;
}

STDMETHODIMP VCamPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = kFormatCount * 2;
    *piSize  = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP VCamPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex < 0 || iIndex >= kFormatCount * 2) return S_FALSE;

    const bool yuy2 = iIndex >= kFormatCount;
    const int  idx  = yuy2 ? iIndex - kFormatCount : iIndex;

    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return E_OUTOFMEMORY;
    if (!build_media_type(mt, idx, yuy2, frame_time_)) {
        CoTaskMemFree(mt);
        return E_FAIL;
    }
    *ppmt = mt;

    auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(pSCC);
    std::memset(caps, 0, sizeof(*caps));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = AnalogVideo_None;
    caps->InputSize.cx  = kFormats[idx].width;
    caps->InputSize.cy  = kFormats[idx].height;
    caps->MinCroppingSize = caps->InputSize;
    caps->MaxCroppingSize = caps->InputSize;
    caps->CropGranularityX = 1;
    caps->CropGranularityY = 1;
    caps->MinOutputSize = caps->InputSize;
    caps->MaxOutputSize = caps->InputSize;
    caps->OutputGranularityX = 1;
    caps->OutputGranularityY = 1;
    caps->MinFrameInterval = 166667;    // 60 fps
    caps->MaxFrameInterval = 1000000;   // 10 fps
    caps->MinBitsPerSecond = static_cast<LONG>(
        kFormats[idx].width * kFormats[idx].height * (yuy2 ? 16 : 32) * 10);
    caps->MaxBitsPerSecond = static_cast<LONG>(
        kFormats[idx].width * kFormats[idx].height * (yuy2 ? 16 : 32) * 60);
    return S_OK;
}

// ---- IKsPropertySet ----
STDMETHODIMP VCamPin::Get(REFGUID guidPropSet, DWORD dwPropID, void*, DWORD,
                          void* pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pcbReturned) *pcbReturned = sizeof(GUID);
    if (!pPropData) return S_OK;
    if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
    *static_cast<GUID*>(pPropData) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

STDMETHODIMP VCamPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

// ---- streaming ----
HRESULT VCamPin::start_streaming() {
    if (!connected_ || !mem_input_ || !allocator_) return S_OK;
    if (thread_) return S_OK;

    ResetEvent(stop_event_);
    allocator_->Commit();
    stream_time_ = 0;
    frame_number_ = 0;

    thread_ = CreateThread(nullptr, 0, &VCamPin::thread_proc, this, 0, nullptr);
    return thread_ ? S_OK : E_FAIL;
}

HRESULT VCamPin::stop_streaming() {
    if (!thread_) return S_OK;
    SetEvent(stop_event_);
    WaitForSingleObject(thread_, 3000);
    CloseHandle(thread_);
    thread_ = nullptr;
    if (allocator_) allocator_->Decommit();
    return S_OK;
}

DWORD WINAPI VCamPin::thread_proc(void* param) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<VCamPin*>(param)->stream_loop();
    CoUninitialize();
    return 0;
}

void VCamPin::fill_sample(BYTE* out, long out_size) {
    const int w = kFormats[format_index_].width;
    const int h = kFormats[format_index_].height;
    const long needed = static_cast<long>(w) * h * (yuy2_ ? 2 : 4);
    if (out_size < needed) return;

    int sw = 0, sh = 0;
    const uint8_t* src = shm_.latest(&sw, &sh);

    if (!src) {
        // No producer: a clean black frame keeps the device usable.
        std::memset(out, yuy2_ ? 0 : 0, static_cast<size_t>(out_size));
        if (yuy2_) {
            for (long i = 0; i < needed; i += 2) { out[i] = 16; out[i + 1] = 128; }
        }
        return;
    }

    const size_t rgba_bytes = static_cast<size_t>(w) * h * 4;
    if (scratch_.size() < rgba_bytes) scratch_.resize(rgba_bytes);

    const uint8_t* bgra = src;
    if (sw != w || sh != h) {
        scale_to(src, sw, sh, scratch_.data(), w, h);
        bgra = scratch_.data();
    }

    if (yuy2_) bgra_to_yuy2(bgra, out, w, h);
    else       bgra_to_rgb32_flipped(bgra, out, w, h);
}

void VCamPin::stream_loop() {
    const REFERENCE_TIME interval = frame_time_ > 0 ? frame_time_ : kDefaultFrameTime;
    LARGE_INTEGER freq{}, start{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
        IMediaSample* sample = nullptr;
        if (!allocator_ || FAILED(allocator_->GetBuffer(&sample, nullptr, nullptr, 0)) ||
            !sample) {
            if (WaitForSingleObject(stop_event_, 10) == WAIT_OBJECT_0) break;
            continue;
        }

        BYTE* data = nullptr;
        if (SUCCEEDED(sample->GetPointer(&data)) && data) {
            fill_sample(data, sample->GetSize());
            sample->SetActualDataLength(sample->GetSize());
        }

        REFERENCE_TIME begin = stream_time_;
        REFERENCE_TIME end   = begin + interval;
        sample->SetTime(&begin, &end);
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(frame_number_ == 0);
        sample->SetMediaTime(&frame_number_, &frame_number_);

        HRESULT hr = mem_input_ ? mem_input_->Receive(sample) : E_FAIL;
        sample->Release();
        if (FAILED(hr)) break;

        stream_time_ = end;
        ++frame_number_;

        // Pace against the wall clock, measured from the start of streaming,
        // so small per frame overruns cannot accumulate into drift.
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const REFERENCE_TIME elapsed =
            (now.QuadPart - start.QuadPart) * 10000000 / freq.QuadPart;
        const REFERENCE_TIME wait = stream_time_ - elapsed;
        DWORD wait_ms = (wait > 0) ? static_cast<DWORD>(wait / 10000) : 0;
        if (wait_ms > 200) wait_ms = 200;
        if (wait_ms > 0 && WaitForSingleObject(stop_event_, wait_ms) == WAIT_OBJECT_0) break;
        if (wait_ms == 0 && WaitForSingleObject(stop_event_, 1) == WAIT_OBJECT_0) break;
    }
}

// --------------------------------------------------------- VCamFilter impl -
VCamFilter::VCamFilter() {
    InitializeCriticalSection(&lock_);
    lstrcpynW(name_, kFilterName, 128);
    pin_ = new (std::nothrow) VCamPin(this);
    InterlockedIncrement(&g_locks);
}

VCamFilter::~VCamFilter() {
    delete pin_;
    pin_ = nullptr;
    if (clock_) clock_->Release();
    DeleteCriticalSection(&lock_);
    InterlockedDecrement(&g_locks);
}

STDMETHODIMP VCamFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown)                *ppv = static_cast<IUnknown*>(static_cast<IBaseFilter*>(this));
    else if (riid == IID_IPersist)           *ppv = static_cast<IPersist*>(this);
    else if (riid == IID_IMediaFilter)       *ppv = static_cast<IMediaFilter*>(this);
    else if (riid == IID_IBaseFilter)        *ppv = static_cast<IBaseFilter*>(this);
    else if (riid == IID_IAMFilterMiscFlags) *ppv = static_cast<IAMFilterMiscFlags*>(this);
    else { *ppv = nullptr; return E_NOINTERFACE; }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) VCamFilter::AddRef() { return InterlockedIncrement(&ref_); }

STDMETHODIMP_(ULONG) VCamFilter::Release() {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP VCamFilter::GetClassID(CLSID* pClassID) {
    if (!pClassID) return E_POINTER;
    *pClassID = CLSID_OMTMiniVCam;
    return S_OK;
}

STDMETHODIMP VCamFilter::Stop() {
    EnterCriticalSection(&lock_);
    if (state_ != State_Stopped) {
        if (pin_) pin_->stop_streaming();
        state_ = State_Stopped;
    }
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP VCamFilter::Pause() {
    EnterCriticalSection(&lock_);
    if (state_ == State_Running && pin_) pin_->stop_streaming();
    state_ = State_Paused;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP VCamFilter::Run(REFERENCE_TIME tStart) {
    EnterCriticalSection(&lock_);
    start_time_ = tStart;
    if (state_ != State_Running) {
        state_ = State_Running;
        if (pin_) pin_->start_streaming();
    }
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP VCamFilter::GetState(DWORD, FILTER_STATE* State) {
    if (!State) return E_POINTER;
    *State = state_;
    return S_OK;
}

STDMETHODIMP VCamFilter::SetSyncSource(IReferenceClock* pClock) {
    EnterCriticalSection(&lock_);
    if (clock_) clock_->Release();
    clock_ = pClock;
    if (clock_) clock_->AddRef();
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP VCamFilter::GetSyncSource(IReferenceClock** pClock) {
    if (!pClock) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pClock = clock_;
    if (clock_) clock_->AddRef();
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP VCamFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) ::EnumPins(static_cast<IPin*>(pin_), 0);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP VCamFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    if (Id && lstrcmpW(Id, L"Capture") == 0 && pin_) {
        *ppPin = static_cast<IPin*>(pin_);
        (*ppPin)->AddRef();
        return S_OK;
    }
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP VCamFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    lstrcpynW(pInfo->achName, name_, MAX_FILTER_NAME);
    pInfo->pGraph = graph_;
    if (graph_) graph_->AddRef();
    return S_OK;
}

STDMETHODIMP VCamFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    EnterCriticalSection(&lock_);
    // Weak reference by design: the graph owns the filter, not the reverse.
    graph_ = pGraph;
    if (pName) lstrcpynW(name_, pName, 128);
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP VCamFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    *pVendorInfo = nullptr;
    return E_NOTIMPL;
}

// --------------------------------------------------------- class factory ---
class VCamClassFactory : public IClassFactory {
public:
    virtual ~VCamClassFactory() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        auto* filter = new (std::nothrow) VCamFilter();
        if (!filter) return E_OUTOFMEMORY;
        const HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_locks);
        else      InterlockedDecrement(&g_locks);
        return S_OK;
    }

private:
    LONG ref_ = 1;
};

// ---------------------------------------------------------- registration ---
// Everything is written under HKEY_CURRENT_USER\Software\Classes so the filter
// installs for one user without elevation. On 64 bit Windows a 32 bit build of
// this DLL lands in the Wow6432Node view automatically via registry
// redirection, which is exactly what 32 bit host applications look at.
static const wchar_t* kVideoInputCategory = L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}";

static LONG set_string(HKEY root, const wchar_t* subkey, const wchar_t* value,
                       const wchar_t* data) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                              KEY_WRITE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    rc = RegSetValueExW(key, value, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(data),
                        static_cast<DWORD>((lstrlenW(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc;
}

static void delete_tree(HKEY root, const wchar_t* subkey) {
    RegDeleteKeyExW(root, subkey, KEY_WOW64_64KEY, 0);
    RegDeleteKeyW(root, subkey);
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return E_FAIL;

    wchar_t key[512];

    StringCchPrintfW(key, 512, L"Software\\Classes\\CLSID\\%s", kClsidText);
    if (set_string(HKEY_CURRENT_USER, key, nullptr, kFilterName) != ERROR_SUCCESS)
        return SELFREG_E_CLASS;

    StringCchPrintfW(key, 512, L"Software\\Classes\\CLSID\\%s\\InprocServer32", kClsidText);
    if (set_string(HKEY_CURRENT_USER, key, nullptr, path) != ERROR_SUCCESS)
        return SELFREG_E_CLASS;
    set_string(HKEY_CURRENT_USER, key, L"ThreadingModel", L"Both");

    // Listing under the video input device category is what makes the filter
    // appear in the camera list of other applications.
    StringCchPrintfW(key, 512, L"Software\\Classes\\CLSID\\%s\\Instance\\%s",
             kVideoInputCategory, kClsidText);
    if (set_string(HKEY_CURRENT_USER, key, L"FriendlyName", kFilterName) != ERROR_SUCCESS)
        return SELFREG_E_CLASS;
    set_string(HKEY_CURRENT_USER, key, L"CLSID", kClsidText);
    set_string(HKEY_CURRENT_USER, key, nullptr, kFilterName);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    wchar_t key[512];

    StringCchPrintfW(key, 512, L"Software\\Classes\\CLSID\\%s\\Instance\\%s",
             kVideoInputCategory, kClsidText);
    delete_tree(HKEY_CURRENT_USER, key);

    StringCchPrintfW(key, 512, L"Software\\Classes\\CLSID\\%s\\InprocServer32", kClsidText);
    delete_tree(HKEY_CURRENT_USER, key);

    StringCchPrintfW(key, 512, L"Software\\Classes\\CLSID\\%s", kClsidText);
    delete_tree(HKEY_CURRENT_USER, key);

    return S_OK;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_OMTMiniVCam) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) VCamClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return g_locks == 0 ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
