/*
 * decklink_shim — cross-platform DeckLink SDI output shared library.
 *
 * Exposes a small pure-C API (consumed from Python via ctypes) over the
 * Blackmagic DeckLink SDK C++ interfaces:
 *
 *   - device / display-mode enumeration
 *   - scheduled video output in UYVY, v210, ARGB, or 10-bit RGB
 *   - timestamped embedded audio output (48 kHz, 32-bit signed PCM)
 *
 * The design (scheduled playback, in-flight buffer FIFO, v210 row packer,
 * display-mode auto-selection ladder) is ported from the DeckLink output
 * written for mpv (vf_decklink_output.mm in the author's mpv tree).
 *
 * Platform differences are confined to this file:
 *   macOS   — CFStringRef names, CreateDeckLinkIteratorInstance from
 *             DeckLinkAPIDispatch.cpp (dlopens DeckLinkAPI.framework)
 *   Linux   — const char* names, same dispatch mechanism
 *   Windows — BSTR names, CoCreateInstance, MIDL-generated header
 */

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#if defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
    #include "DeckLinkAPI.h"
#elif defined(_WIN32)
    #include <objbase.h>
    #include "DeckLinkAPI_h.h"
#else
    #include "DeckLinkAPI.h"
#endif

// SDK-version compatibility (ported from the author's nomacs-decklink
// plugin): SDK 16.0 changed the IIDs for IDeckLinkOutput and
// IDeckLinkVideoBuffer, and the IDeckLinkOutput vtable itself changed
// between the 14.x and 15.x SDK eras (CreateVideoFrameWithBuffer and
// RowBytesForPixelFormat were added; SetVideoOutputFrameMemoryAllocator was
// removed). A user with an older Desktop Video install would otherwise get
// "device has no output interface" outright. On macOS/Linux the SDK ships
// these older interfaces as separate versioned headers; on Windows,
// DeckLinkAPI.idl already #includes the compat IDLs, so MIDL inlines every
// compat interface and GUID into the generated header — nothing extra
// needed there.
#if !defined(_WIN32)
    #include "DeckLinkAPIVideoOutput_v15_3_1.h"  // IDeckLinkOutput_v15_3_1 + IID (same vtable as current)
    #include "DeckLinkAPIVideoOutput_v14_2_1.h"  // IDeckLinkOutput_v14_2_1, IDeckLinkVideoOutputCallback_v14_2_1,
                                                  // IDeckLinkMutableVideoFrame_v14_2_1 + IIDs
                                                  // (pulls in DeckLinkAPIVideoFrame_v14_2_1.h for IDeckLinkVideoFrame_v14_2_1)
    #include "DeckLinkAPI_v15_3_1.h"             // IDeckLinkVideoBuffer_v15_3_1 + IID
    // IDeckLinkConfiguration + IID — pulled in transitively via the IDL on
    // Windows (DeckLinkAPI.idl #includes DeckLinkAPIConfiguration.idl), but
    // a separate header on Mac/Linux. Its vtable hasn't changed since before
    // 14.2.1, so no versioned variant is needed.
    #include "DeckLinkAPIConfiguration.h"
#endif

#ifndef STDMETHODCALLTYPE
    #define STDMETHODCALLTYPE
#endif

#if defined(_WIN32)
    #define DLK_EXPORT extern "C" __declspec(dllexport)
#else
    #define DLK_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// DoesSupportVideoMode's out "supported" param is BOOL (int) in the
// MIDL-generated Windows headers but bool in the Mac/Linux SDK headers.
#if defined(_WIN32)
    using DlkBool = BOOL;
#else
    using DlkBool = bool;
#endif

// Public constants and the C API declarations.
#include "decklink_shim.h"

// ---------------------------------------------------------------------------
// Logging — one global callback, registered by the helper at startup.
// ---------------------------------------------------------------------------
static dlk_log_fn g_log_fn = nullptr;

DLK_EXPORT void dlk_set_log_callback(dlk_log_fn fn) { g_log_fn = fn; }

static void dlk_logf(int level, const char *fmt, ...)
{
    if (!g_log_fn)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log_fn(level, buf);
}

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

// The same IUnknown GUID on every platform; the SDK only defines it on Windows.
static const unsigned char kIID_IUnknown_bytes[16] =
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
     0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46};

static bool iid_is_iunknown(REFIID riid)
{
    return memcmp(&riid, kIID_IUnknown_bytes, 16) == 0;
}

#if defined(_WIN32)
// Every thread that touches COM must be initialised.  Python may call in from
// several threads; COINIT_MULTITHREADED makes them all share one apartment.
static void com_thread_init(void)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}
#else
static void com_thread_init(void) {}
#endif

static IDeckLinkIterator *make_iterator(void)
{
    com_thread_init();
#if defined(_WIN32)
    IDeckLinkIterator *iter = nullptr;
    if (CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL,
                         IID_IDeckLinkIterator, (void **)&iter) != S_OK)
        return nullptr;
    return iter;
#else
    // Provided by DeckLinkAPIDispatch.cpp, which dlopens the Desktop Video
    // library at runtime — no link-time dependency on Blackmagic binaries.
    return CreateDeckLinkIteratorInstance();
#endif
}

// Return the device display name as a malloc'd UTF-8 string.
static char *get_display_name_utf8(IDeckLink *device)
{
#if defined(__APPLE__)
    CFStringRef cf_name = nullptr;
    if (device->GetDisplayName(&cf_name) != S_OK || !cf_name)
        return strdup("(unknown)");
    char buf[256];
    bool ok = CFStringGetCString(cf_name, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cf_name);
    return strdup(ok ? buf : "(unknown)");
#elif defined(_WIN32)
    BSTR bstr = nullptr;
    if (device->GetDisplayName(&bstr) != S_OK || !bstr)
        return _strdup("(unknown)");
    int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
    char *out = (char *)malloc((size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, bstr, -1, out, len, nullptr, nullptr);
    SysFreeString(bstr);
    return out;
#else
    const char *name = nullptr;
    if (device->GetDisplayName(&name) != S_OK || !name)
        return strdup("(unknown)");
    char *out = strdup(name);
    free((void *)name);
    return out;
#endif
}

// Find a device by display name (nullptr/empty → first device found).
// Caller owns the returned reference.
static IDeckLink *find_device(const char *want_name)
{
    IDeckLinkIterator *iter = make_iterator();
    if (!iter) {
        dlk_logf(0, "DeckLink: cannot create iterator — is Desktop Video installed?");
        return nullptr;
    }

    IDeckLink *device = nullptr;
    IDeckLink *found  = nullptr;
    while (iter->Next(&device) == S_OK) {
        char *name = get_display_name_utf8(device);
        if (!found && (!want_name || !want_name[0] || strcmp(name, want_name) == 0)) {
            found = device;
            device->AddRef();
        }
        free(name);
        device->Release();
    }
    iter->Release();

    if (!found)
        dlk_logf(0, "DeckLink: device '%s' not found",
                 (want_name && want_name[0]) ? want_name : "(any)");
    return found;
}

// ---------------------------------------------------------------------------
// IDeckLinkOutput version-compatibility dispatcher (ported from the author's
// nomacs-decklink plugin — see that project's DkDeckLinkCompat.h for the
// original, more thoroughly annotated version).
//
// Holds whichever versioned pointer QueryInterface actually returned and
// routes each call to the correct vtable. The 15.3.1 and current/16.0
// IDeckLinkOutput interfaces share an identical vtable (only the IID changed),
// so both are stored in v16 and every method here is dispatched identically;
// 14.x has a different IDeckLinkOutput vtable (older Desktop Video installs)
// and is dispatched separately.
//
// IMPORTANT: the IDeckLinkOutput vtables matching does NOT mean the zero-copy
// buffer ABI matches. IDeckLinkOutput::CreateVideoFrameWithBuffer takes an
// IDeckLinkVideoBuffer on 16.0 but an IDeckLinkVideoBuffer_v15_3_1 on 15.x,
// and those two interfaces have DIFFERENT vtables (16.0 inserted GetSize at
// slot 2). Handing a 16.0-vtable MallocVideoBuffer to a 15.x driver therefore
// makes the driver call the wrong slot (StartAccess dispatches into GetSize)
// and crash. So `zero_copy` gates the CreateVideoFrameWithBuffer path to the
// genuine current interface only; 15.x falls back to the copy-based pool
// alongside 14.x. See find_output_iface().
// ---------------------------------------------------------------------------
struct DlkOutputIface {
    IDeckLinkOutput         *v16 = nullptr;  // current SDK OR 15.3.1 (identical IDeckLinkOutput vtable)
    IDeckLinkOutput_v14_2_1 *v14 = nullptr;  // pre-15.x SDK — no CreateVideoFrameWithBuffer
    bool                     zero_copy = false;  // true ONLY for the genuine 16.0 IDeckLinkVideoBuffer ABI

    bool valid() const { return v16 || v14; }

    void release()
    {
        if (v16) { v16->Release(); v16 = nullptr; }
        if (v14) { v14->Release(); v14 = nullptr; }
    }

    HRESULT GetDisplayModeIterator(IDeckLinkDisplayModeIterator **it)
    {
        return v16 ? v16->GetDisplayModeIterator(it) : v14->GetDisplayModeIterator(it);
    }
    HRESULT EnableVideoOutput(BMDDisplayMode mode, BMDVideoOutputFlags flags)
    {
        return v16 ? v16->EnableVideoOutput(mode, flags) : v14->EnableVideoOutput(mode, flags);
    }
    HRESULT DisableVideoOutput()
    {
        return v16 ? v16->DisableVideoOutput() : v14->DisableVideoOutput();
    }
    HRESULT SetScheduledFrameCompletionCallback(IDeckLinkVideoOutputCallback *cb)
    {
        if (v16) return v16->SetScheduledFrameCompletionCallback(cb);
        return v14->SetScheduledFrameCompletionCallback(
            reinterpret_cast<IDeckLinkVideoOutputCallback_v14_2_1 *>(cb));
    }
    HRESULT StartScheduledPlayback(BMDTimeValue t, BMDTimeScale scale, double speed)
    {
        return v16 ? v16->StartScheduledPlayback(t, scale, speed)
                   : v14->StartScheduledPlayback(t, scale, speed);
    }
    HRESULT StopScheduledPlayback(BMDTimeValue t, BMDTimeValue *actual, BMDTimeScale scale)
    {
        return v16 ? v16->StopScheduledPlayback(t, actual, scale)
                   : v14->StopScheduledPlayback(t, actual, scale);
    }
    HRESULT GetBufferedVideoFrameCount(uint32_t *n)
    {
        return v16 ? v16->GetBufferedVideoFrameCount(n) : v14->GetBufferedVideoFrameCount(n);
    }
    // Identical signature on both tiers.
    HRESULT EnableAudioOutput(BMDAudioSampleRate rate, BMDAudioSampleType type,
                              uint32_t channels, BMDAudioOutputStreamType streamType)
    {
        return v16 ? v16->EnableAudioOutput(rate, type, channels, streamType)
                   : v14->EnableAudioOutput(rate, type, channels, streamType);
    }
    HRESULT DisableAudioOutput()
    {
        return v16 ? v16->DisableAudioOutput() : v14->DisableAudioOutput();
    }
    HRESULT ScheduleAudioSamples(void *buf, uint32_t sampleFrameCount,
                                 BMDTimeValue streamTime, BMDTimeScale timeScale,
                                 uint32_t *written)
    {
        return v16 ? v16->ScheduleAudioSamples(buf, sampleFrameCount, streamTime, timeScale, written)
                   : v14->ScheduleAudioSamples(buf, sampleFrameCount, streamTime, timeScale, written);
    }
    HRESULT GetBufferedAudioSampleFrameCount(uint32_t *n)
    {
        return v16 ? v16->GetBufferedAudioSampleFrameCount(n)
                   : v14->GetBufferedAudioSampleFrameCount(n);
    }
    HRESULT GetScheduledStreamTime(BMDTimeScale desiredScale, BMDTimeValue *streamTime,
                                   double *speed)
    {
        return v16 ? v16->GetScheduledStreamTime(desiredScale, streamTime, speed)
                   : v14->GetScheduledStreamTime(desiredScale, streamTime, speed);
    }

    // v14 CreateVideoFrame returns IDeckLinkMutableVideoFrame_v14_2_1**; only
    // GetWidth/Height/RowBytes/QueryInterface (identical slots in both
    // vtables) are ever called on the result, so storing it as the current
    // IDeckLinkMutableVideoFrame* (same pointer value) is safe.
    HRESULT CreateVideoFrame(int32_t w, int32_t h, int32_t rowBytes, BMDPixelFormat fmt,
                             BMDFrameFlags flags, IDeckLinkMutableVideoFrame **outFrame)
    {
        if (v16)
            return v16->CreateVideoFrame(w, h, rowBytes, fmt, flags, outFrame);
        IDeckLinkMutableVideoFrame_v14_2_1 *f14 = nullptr;
        HRESULT hr = v14->CreateVideoFrame(w, h, rowBytes, fmt, flags, &f14);
        *outFrame = reinterpret_cast<IDeckLinkMutableVideoFrame *>(f14);
        return hr;
    }
    // v14 expects IDeckLinkVideoFrame_v14_2_1*; frames created by the v14
    // CreateVideoFrame path above are stored as IDeckLinkMutableVideoFrame*,
    // so cast back to the original pointer type for that path only.
    HRESULT ScheduleVideoFrame(IDeckLinkMutableVideoFrame *frame, BMDTimeValue t,
                               BMDTimeValue dur, BMDTimeScale scale)
    {
        return v16 ? v16->ScheduleVideoFrame(frame, t, dur, scale)
                   : v14->ScheduleVideoFrame(
                         reinterpret_cast<IDeckLinkVideoFrame_v14_2_1 *>(frame), t, dur, scale);
    }
    // Identical signature on both tiers — used to check, before committing to
    // a mode via EnableVideoOutput, whether the current connector/profile can
    // actually carry a given display mode + pixel format combination (e.g.
    // 4:4:4 at high frame rates commonly exceeds single-link SDI bandwidth
    // that the same mode in 4:2:2 fits within).
    HRESULT DoesSupportVideoMode(BMDVideoConnection connection, BMDDisplayMode requestedMode,
                                 BMDPixelFormat requestedPixelFormat,
                                 BMDVideoOutputConversionMode conversionMode,
                                 BMDSupportedVideoModeFlags flags,
                                 BMDDisplayMode *actualMode, DlkBool *supported)
    {
        return v16 ? v16->DoesSupportVideoMode(connection, requestedMode, requestedPixelFormat,
                                               conversionMode, flags, actualMode, supported)
                   : v14->DoesSupportVideoMode(connection, requestedMode, requestedPixelFormat,
                                               conversionMode, flags, actualMode, supported);
    }
};

static DlkOutputIface find_output_iface(IDeckLink *device)
{
    DlkOutputIface iface;
    IDeckLinkOutput *out16 = nullptr;
    if (device->QueryInterface(IID_IDeckLinkOutput, (void **)&out16) == S_OK) {
        iface.v16 = out16;
        iface.zero_copy = true;   // genuine current IDeckLinkVideoBuffer ABI
        return iface;
    }
    void *p = nullptr;
    if (device->QueryInterface(IID_IDeckLinkOutput_v15_3_1, &p) == S_OK) {
        // 15.x driver. Its IDeckLinkOutput vtable is identical to 16.0 for
        // every method we dispatch, so store it in v16 — but its
        // CreateVideoFrameWithBuffer expects an IDeckLinkVideoBuffer_v15_3_1
        // (a different vtable from 16.0's IDeckLinkVideoBuffer), so the
        // zero-copy MallocVideoBuffer path would crash it. Leave zero_copy
        // false so the copy-based pool is used instead.
        iface.v16 = reinterpret_cast<IDeckLinkOutput *>(p);
        return iface;
    }
    IDeckLinkOutput_v14_2_1 *out14 = nullptr;
    if (device->QueryInterface(IID_IDeckLinkOutput_v14_2_1, (void **)&out14) == S_OK)
        iface.v14 = out14;
    return iface;
}

// Sets single-, dual- or quad-link SDI output (ported from the author's
// nomacs-decklink plugin's equivalent setting). Multi-link splits the signal
// across two or four SDI connectors/cables to raise available bandwidth —
// needed for some high-bandwidth combinations (e.g. 4:4:4 at high frame
// rates/resolutions) that don't fit a single link. Must be set before
// GetDisplayModeIterator()/EnableVideoOutput(): it can change which display
// modes the device even offers. No-ops (logging at debug level) on hardware
// that doesn't expose IDeckLinkConfiguration or reports it unsupported —
// most single-connector devices, which is the common case.
static void set_sdi_link_configuration(IDeckLink *device, int link_mode)
{
    IDeckLinkConfiguration *cfg = nullptr;
    if (device->QueryInterface(IID_IDeckLinkConfiguration, (void **)&cfg) != S_OK) {
        dlk_logf(2, "DeckLink: IDeckLinkConfiguration unavailable — cannot set SDI link mode");
        return;
    }
    BMDLinkConfiguration link;
    switch (link_mode) {
    case DLK_LINK_DUAL: link = bmdLinkConfigurationDualLink; break;
    case DLK_LINK_QUAD: link = bmdLinkConfigurationQuadLink; break;
    default:            link = bmdLinkConfigurationSingleLink; break;
    }
    if (cfg->SetInt(bmdDeckLinkConfigSDIOutputLinkConfiguration, link) != S_OK)
        dlk_logf(1, "DeckLink: device doesn't support setting SDI link configuration");
    cfg->Release();
}

// RAII lock on a mutable video frame's write buffer, spanning every SDK era:
// current IDeckLinkVideoBuffer, the 15.3.1-only IDeckLinkVideoBuffer_v15_3_1,
// or (pre-15.x, no separate buffer interface at all) GetBytes directly on
// IDeckLinkVideoFrame_v14_2_1. Used by the copy-based fallback frame pool
// (both the 15.x and 14.x tiers) — the genuine 16.0 zero-copy
// CreateVideoFrameWithBuffer path never calls this, since MallocVideoBuffer
// already implements IDeckLinkVideoBuffer. The per-era QueryInterface probe
// below is what makes this correct across driver versions: on a 15.x frame it
// binds vbuf153 and calls the v15_3_1 buffer vtable, never the 16.0 one.
struct FrameWriteBuffer {
    void                          *buf     = nullptr;
    IDeckLinkVideoBuffer          *vbuf    = nullptr;
    IDeckLinkVideoBuffer_v15_3_1  *vbuf153 = nullptr;
    IDeckLinkVideoFrame_v14_2_1   *old     = nullptr;

    explicit FrameWriteBuffer(IDeckLinkMutableVideoFrame *frame)
    {
        if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, (void **)&vbuf) == S_OK) {
            vbuf->StartAccess(bmdBufferAccessWrite);
            vbuf->GetBytes(&buf);
        } else if (frame->QueryInterface(IID_IDeckLinkVideoBuffer_v15_3_1,
                                         (void **)&vbuf153) == S_OK) {
            vbuf153->StartAccess(bmdBufferAccessWrite);
            vbuf153->GetBytes(&buf);
        } else if (frame->QueryInterface(IID_IDeckLinkVideoFrame_v14_2_1,
                                         (void **)&old) == S_OK) {
            old->GetBytes(&buf);
        }
    }

    ~FrameWriteBuffer()
    {
        if (vbuf)    { vbuf->EndAccess(bmdBufferAccessWrite);    vbuf->Release(); }
        if (vbuf153) { vbuf153->EndAccess(bmdBufferAccessWrite); vbuf153->Release(); }
        if (old)     { old->Release(); }
    }

    FrameWriteBuffer(const FrameWriteBuffer &) = delete;
    FrameWriteBuffer &operator=(const FrameWriteBuffer &) = delete;
};

// ---------------------------------------------------------------------------
// v210 row packer.
// Input: YUV422P10LE planes — 10-bit values in the low 10 bits of 16-bit LE
// words.  v210 stores 6 pixels in 4 × 32-bit words.  Width is padded to a
// multiple of 48 pixels by the row_bytes computation, so partial groups never
// occur at canvas sizes DeckLink supports.
// ---------------------------------------------------------------------------
static void pack_v210_row(const uint16_t *y, const uint16_t *u,
                          const uint16_t *v, uint8_t *dst, int width)
{
    int x = 0;
    uint32_t *out = (uint32_t *)dst;
    while (x <= width - 6) {
        uint32_t u0 = u[x/2]     & 0x3FF;
        uint32_t y0 = y[x]       & 0x3FF;
        uint32_t v0 = v[x/2]     & 0x3FF;
        uint32_t y1 = y[x+1]     & 0x3FF;
        uint32_t u1 = u[x/2 + 1] & 0x3FF;
        uint32_t y2 = y[x+2]     & 0x3FF;
        uint32_t v1 = v[x/2 + 1] & 0x3FF;
        uint32_t y3 = y[x+3]     & 0x3FF;
        uint32_t u2 = u[x/2 + 2] & 0x3FF;
        uint32_t y4 = y[x+4]     & 0x3FF;
        uint32_t v2 = v[x/2 + 2] & 0x3FF;
        uint32_t y5 = y[x+5]     & 0x3FF;
        out[0] = (v0 << 20) | (y0 << 10) | u0;
        out[1] = (y2 << 20) | (u1 << 10) | y1;
        out[2] = (u2 << 20) | (y3 << 10) | v1;
        out[3] = (y5 << 20) | (v2 << 10) | y4;
        out += 4;
        x   += 6;
    }
}

// ---------------------------------------------------------------------------
// Pixel buffer — malloc'd memory implementing IDeckLinkVideoBuffer so it can
// be handed to CreateVideoFrameWithBuffer.  Atomically ref-counted: the
// DeckLink completion thread and the feeder thread release from different
// threads.
// ---------------------------------------------------------------------------
class MallocVideoBuffer : public IDeckLinkVideoBuffer {
    unsigned char        *data_;
    size_t                size_;
    std::atomic<int32_t>  refs_;

public:
    MallocVideoBuffer(int row_bytes, int height)
        : size_((size_t)row_bytes * height), refs_(1)
    {
        data_ = (unsigned char *)malloc(size_);
    }

    unsigned char *data() const { return data_; }
    bool           ok()   const { return data_ != nullptr; }

    HRESULT STDMETHODCALLTYPE GetBytes(void **buf) override
        { *buf = data_; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSize(uint64_t *s) override
        { *s = (uint64_t)size_; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags) override
        { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags) override
        { return S_OK; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID *ppv) override
        { *ppv = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = (ULONG)(--refs_);
        if (r == 0) { free(data_); delete this; }
        return r;
    }
};

// ---------------------------------------------------------------------------
// Completion callback.  DeckLink calls ScheduledFrameCompleted on its own
// thread each time a scheduled frame is retired.  Owns a FIFO of in-flight
// pixel buffers so each buffer is freed only after its frame completes.
//
// Embedded inside dlk_output, so dlk_output MUST be allocated with new (never
// calloc) so this object's vtable pointer is initialised before DeckLink's
// thread can call into it.
// ---------------------------------------------------------------------------
class OutputCallback : public IDeckLinkVideoOutputCallback {
public:
    std::mutex        *lock = nullptr;
    int               *frames_in_flight = nullptr;

    static const int FIFO_SIZE = 64;
    MallocVideoBuffer *fifo[FIFO_SIZE] = {};
    int head = 0, tail = 0;

    // Called under *lock from the feeder thread right after ScheduleVideoFrame.
    void push_inflight(MallocVideoBuffer *buf)
    {
        fifo[tail % FIFO_SIZE] = buf;
        tail++;
    }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(
        IDeckLinkVideoFrame *, BMDOutputFrameCompletionResult) override
    {
        std::lock_guard<std::mutex> guard(*lock);
        (*frames_in_flight)--;
        // Frames complete in schedule order; retire the oldest buffer.
        if (head < tail) {
            MallocVideoBuffer *buf = fifo[head % FIFO_SIZE];
            fifo[head % FIFO_SIZE] = nullptr;
            head++;
            buf->Release();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID *ppv) override
    {
        if (iid_is_iunknown(riid)) {
            *ppv = static_cast<IUnknown *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Lifetime is owned by the enclosing dlk_output; refcounting is a no-op.
    ULONG STDMETHODCALLTYPE AddRef()  override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

// ---------------------------------------------------------------------------
// Output state
// ---------------------------------------------------------------------------
struct dlk_output {
    IDeckLink       *device = nullptr;
    DlkOutputIface   output;                 // dispatches to whichever SDK-era interface was found
    OutputCallback   callback;               // vtable set by ctor — see class docs

    BMDDisplayMode   bmd_mode       = bmdModeUnknown;
    int              width          = 0;
    int              height         = 0;
    BMDTimeValue     frame_duration = 0;
    BMDTimeScale     time_scale     = 0;
    double           mode_fps       = 0.0;
    BMDPixelFormat   pixel_format   = bmdFormat8BitYUV;
    int              pixfmt         = 0;    // DLK_PIXFMT_*
    int              row_bytes      = 0;
    int64_t          frame_count    = 0;    // next frame index on the schedule
    int              preroll        = 3;
    int              inflight_limit = 6;

    std::mutex       lock;
    int              frames_in_flight = 0;

    bool             audio_enabled = false;
    int              audio_channels = 2;
    int64_t          audio_sample_pos = 0;  // next timestamp, in samples @48kHz

    // Full-range → SMPTE legal-range mapping for RGB output formats.  YUV
    // range handling happens upstream in swscale; RGB legal range does not
    // exist in swscale, so it is applied here during packing via LUT.
    bool             rgb_legal = false;
    uint8_t          lut8[256];
    uint16_t         lut10[1024];

    // Copy-based fallback for every pre-16.0 tier (both 15.x and 14.x): the
    // zero-copy CreateVideoFrameWithBuffer path is only safe on the genuine
    // 16.0 IDeckLinkVideoBuffer ABI (see DlkOutputIface::zero_copy), so on the
    // older tiers a pool of SDK-owned frames (created once via the
    // universally-available CreateVideoFrame) is reused round-robin, each
    // refilled with a memcpy before every reschedule — unavoidable here since
    // there's no safe caller-supplied-
    // buffer API. Sized to inflight_limit: since at most inflight_limit
    // frames are ever scheduled-and-not-yet-completed at once, and
    // completions happen in schedule order, a slot is always safe to refill
    // again once its previous scheduling has had time to complete. Empty
    // (unused) when output.zero_copy is set (the genuine 16.0 tier).
    std::vector<IDeckLinkMutableVideoFrame *> compat_pool;
    int              compat_pool_next = 0;
};

static void init_rgb_luts(dlk_output *d)
{
    for (int i = 0; i < 256; i++)
        d->lut8[i] = (uint8_t)(16 + (i * 219 + 127) / 255);
    for (int i = 0; i < 1024; i++)
        d->lut10[i] = (uint16_t)(64 + (i * 876 + 511) / 1023);
}

// ---------------------------------------------------------------------------
// Display-mode selection.
//
// Auto-selection ladder (stops at the first pass that matches), ported from
// the mpv implementation so the output frame rate follows the source:
//   0. Explicit 4-char format code — no fallback.
//   1. Exact size + source fps.
//   2. UHD (3840x2160) then HD (1920x1080) at source fps.
//   3. UHD then HD at the smallest clean integer multiple of source fps
//      (23.976 → 47.952 rather than 71.928).
//   4. Last resort: UHD then HD at ~30 fps.
// ---------------------------------------------------------------------------
struct ModeEntry {
    BMDDisplayMode bmd;
    int            width, height;
    BMDTimeValue   dur;
    BMDTimeScale   scale;
    double         fps;
    bool           progressive;
};

static std::vector<ModeEntry> collect_modes(DlkOutputIface &output)
{
    std::vector<ModeEntry> entries;
    IDeckLinkDisplayModeIterator *mi = nullptr;
    if (output.GetDisplayModeIterator(&mi) != S_OK)
        return entries;
    IDeckLinkDisplayMode *mode = nullptr;
    while (mi->Next(&mode) == S_OK) {
        ModeEntry e;
        e.bmd    = mode->GetDisplayMode();
        e.width  = (int)mode->GetWidth();
        e.height = (int)mode->GetHeight();
        mode->GetFrameRate(&e.dur, &e.scale);
        e.fps    = (double)e.scale / (double)e.dur;
        BMDFieldDominance fd = mode->GetFieldDominance();
        e.progressive = (fd == bmdProgressiveFrame ||
                         fd == bmdProgressiveSegmentedFrame);
        entries.push_back(e);
        mode->Release();
    }
    mi->Release();
    return entries;
}

static void mode_code_str(BMDDisplayMode bmd, char code[5])
{
    code[0] = (char)((bmd >> 24) & 0xff);
    code[1] = (char)((bmd >> 16) & 0xff);
    code[2] = (char)((bmd >>  8) & 0xff);
    code[3] = (char)( bmd        & 0xff);
    code[4] = '\0';
}

// Mode selection, in priority order:
//   0. Explicit 4-char format code — no fallback; caller knows exactly what
//      they want.
//   1. Locked resolution (from the user's DeckLink settings): the output
//      resolution and chroma/pixel format stay exactly what the user chose
//      (e.g. matching a fixed downstream monitor) regardless of what the
//      source video is; only the frame rate is auto-negotiated to best fit
//      the source, so a mismatched fps degrades to a clean multiple/divisor
//      or, at worst, judder — see resolve_mode_fps() — rather than silently
//      changing the picture's resolution out from under the user.
//   2. Fully automatic (no locked resolution set): matches the source's own
//      resolution first, then falls back through UHD and HD, at the best
//      available fps for each — this is the original mpv-derived behavior,
//      used when the user leaves DeckLink resolution on "Auto".
//
// Within a resolution (fixed tiers 1 and each candidate in tier 2), fps is
// chosen by: exact match, then smallest clean integer multiple, then the
// nearest mode to ~30fps, then (last resort) any progressive mode at all —
// ensuring some usable mode is always found if the resolution exists.
static bool find_mode(dlk_output *d, int width, int height, double fps,
                      const char *format_code, int fixed_width, int fixed_height)
{
    BMDDisplayMode target = bmdModeUnknown;
    if (format_code && strlen(format_code) >= 4) {
        target = (BMDDisplayMode)(((uint32_t)(uint8_t)format_code[0] << 24) |
                                  ((uint32_t)(uint8_t)format_code[1] << 16) |
                                  ((uint32_t)(uint8_t)format_code[2] <<  8) |
                                   (uint32_t)(uint8_t)format_code[3]);
    }

    std::vector<ModeEntry> entries = collect_modes(d->output);
    int n = (int)entries.size();
    if (n == 0)
        return false;

    // Absolute tolerance of 0.02 fps distinguishes 24.0 from 23.976 cleanly.
    auto fps_close = [](double a, double b) { return fabs(a - b) < 0.02; };

    // Best progressive mode at exactly (rw, rh) for want_fps, or -1 if the
    // device has no progressive mode at that resolution at all.
    auto best_for_resolution = [&](int rw, int rh, double want_fps) -> int {
        for (int i = 0; i < n; i++) {
            if (entries[i].width == rw && entries[i].height == rh &&
                entries[i].progressive &&
                (want_fps <= 0.0 || fps_close(entries[i].fps, want_fps))) {
                return i;
            }
        }
        if (want_fps > 0.0) {
            int best_i = -1, best_ratio = 0;
            for (int i = 0; i < n; i++) {
                if (entries[i].width != rw || entries[i].height != rh ||
                    !entries[i].progressive)
                    continue;
                double ratio  = entries[i].fps / want_fps;
                int    iratio = (int)lround(ratio);
                if (iratio >= 1 && iratio <= 4 && fabs(ratio - iratio) < 0.01 &&
                    (best_i < 0 || iratio < best_ratio)) {
                    best_i     = i;
                    best_ratio = iratio;
                }
            }
            if (best_i >= 0)
                return best_i;
        }
        // No exact rate and no whole-number multiple.  Rather than taking
        // whichever mode the device happens to list first, pick the one whose
        // ratio to the source lands closest to a clean pulldown cadence —
        // counting halves, because 2.5 is 3:2 and the feed loop produces it
        // exactly.
        //
        // This matters precisely in the workflow this ladder exists for: lock
        // the output to the monitor's resolution and let the rate negotiate.
        // A device with only 720p50/59.94/60 has no 23.976, and the old
        // first-listed rule handed 23.976 film to 720p50 — a ratio of 2.086,
        // which is visible judder — when 720p59.94 was sitting there at 2.5.
        if (want_fps > 0.0) {
            int    best_i     = -1;
            double best_score = 0.0;
            double best_fps   = 0.0;
            for (int i = 0; i < n; i++) {
                if (entries[i].width != rw || entries[i].height != rh ||
                    !entries[i].progressive)
                    continue;
                double ratio = entries[i].fps / want_fps;
                double score = fabs(ratio - round(ratio * 2.0) / 2.0);
                // Ties go to the lower rate: same cadence, less bandwidth.
                if (best_i < 0 || score < best_score - 1e-9 ||
                    (fabs(score - best_score) <= 1e-9 && entries[i].fps < best_fps)) {
                    best_i     = i;
                    best_score = score;
                    best_fps   = entries[i].fps;
                }
            }
            if (best_i >= 0)
                return best_i;
        }
        // Nothing to score against (unknown source rate): any progressive mode
        // at this resolution beats failing outright.
        for (int i = 0; i < n; i++) {
            if (entries[i].width == rw && entries[i].height == rh && entries[i].progressive)
                return i;
        }
        return -1;
    };

    int best = -1;

    if (target != bmdModeUnknown) {
        // Pass 0: explicit format code — no fallback.
        for (int i = 0; i < n; i++)
            if (entries[i].bmd == target) { best = i; break; }
    } else {
        if (fixed_width > 0 && fixed_height > 0) {
            // Pass 1: locked resolution, auto fps.
            best = best_for_resolution(fixed_width, fixed_height, fps);
            if (best < 0)
                dlk_logf(1, "DeckLink: no mode at locked resolution %dx%d — "
                            "falling back to automatic resolution selection",
                         fixed_width, fixed_height);
        }
        if (best < 0) {
            // Pass 2: fully automatic — source resolution, then UHD, then HD.
            const int pw[3] = {width, 3840, 1920};
            const int ph[3] = {height, 2160, 1080};
            for (int p = 0; p < 3 && best < 0; p++) {
                if (p > 0 && pw[p] == pw[0] && ph[p] == ph[0])
                    continue;  // skip a duplicate of the exact-source pass
                best = best_for_resolution(pw[p], ph[p], fps);
            }
        }
    }

    if (best < 0) {
        dlk_logf(0, "DeckLink: no mode found for %dx%d @ %.3f fps%s%s",
                 width, height, fps,
                 (format_code && format_code[0]) ? " code=" : "",
                 (format_code && format_code[0]) ? format_code : "");
        return false;
    }

    d->bmd_mode       = entries[best].bmd;
    d->frame_duration = entries[best].dur;
    d->time_scale     = entries[best].scale;
    d->width          = entries[best].width;
    d->height         = entries[best].height;
    d->mode_fps       = entries[best].fps;

    char code[5];
    mode_code_str(d->bmd_mode, code);
    dlk_logf(1, "DeckLink: using mode '%s' (%dx%d @ %.3f fps) for source %dx%d @ %.3f fps",
             code, d->width, d->height, d->mode_fps, width, height, fps);
    return true;
}

// ---------------------------------------------------------------------------
// Enumeration API
// ---------------------------------------------------------------------------
DLK_EXPORT void dlk_free_string(char *s) { free(s); }

// Newline-separated device display names; NULL if the driver is missing.
DLK_EXPORT char *dlk_enumerate_devices(void)
{
    IDeckLinkIterator *iter = make_iterator();
    if (!iter)
        return nullptr;

    std::string result;
    IDeckLink *device = nullptr;
    while (iter->Next(&device) == S_OK) {
        char *name = get_display_name_utf8(device);
        device->Release();
        result += name;
        result += '\n';
        free(name);
    }
    iter->Release();
    return strdup(result.c_str());
}

// One mode per line: "code\twidth\theight\tfps\tscan".  NULL on failure.
DLK_EXPORT char *dlk_enumerate_modes(const char *device_name)
{
    IDeckLink *device = find_device(device_name);
    if (!device)
        return nullptr;

    DlkOutputIface output = find_output_iface(device);
    if (!output.valid()) {
        device->Release();
        return nullptr;
    }

    std::string result;
    std::vector<ModeEntry> entries = collect_modes(output);
    for (const ModeEntry &e : entries) {
        char code[5];
        mode_code_str(e.bmd, code);
        char line[128];
        snprintf(line, sizeof(line), "%s\t%d\t%d\t%.6f\t%s\n",
                 code, e.width, e.height, e.fps, e.progressive ? "p" : "i");
        result += line;
    }
    output.release();
    device->Release();
    return strdup(result.c_str());
}

// Pre-creates the copy-based fallback frame pool used by every pre-16.0 tier
// (see the dlk_output::compat_pool field comment). Called once, after
// EnableVideoOutput succeeds and before scheduling begins;
// d->width/height/row_bytes/pixel_format/inflight_limit must already be
// finalized.
static bool create_compat_pool(dlk_output *d)
{
    d->compat_pool.assign((size_t)d->inflight_limit, nullptr);
    for (int i = 0; i < d->inflight_limit; i++) {
        IDeckLinkMutableVideoFrame *frame = nullptr;
        if (d->output.CreateVideoFrame(d->width, d->height, d->row_bytes,
                                       d->pixel_format, bmdFrameFlagDefault,
                                       &frame) != S_OK || !frame) {
            for (int j = 0; j < i; j++)
                d->compat_pool[j]->Release();
            d->compat_pool.clear();
            return false;
        }
        // Zero the SDK-allocated buffer so a slot that hasn't been refilled
        // yet (start-of-playback preroll) never scans out uninitialized
        // memory. Not true broadcast black for the YUV formats (needs
        // Y=16/64, Cb=Cr=128/512, not all-zero) but it's overwritten by a
        // real frame within milliseconds, well before the first frame the
        // Python side ever forces onto the wire while paused.
        FrameWriteBuffer wb(frame);
        if (wb.buf)
            memset(wb.buf, 0, (size_t)d->row_bytes * (size_t)d->height);
        d->compat_pool[i] = frame;
    }
    return true;
}

static BMDPixelFormat bmd_pixel_format(int pixfmt)
{
    switch (pixfmt) {
    case DLK_PIXFMT_UYVY:  return bmdFormat8BitYUV;
    case DLK_PIXFMT_V210:  return bmdFormat10BitYUV;
    case DLK_PIXFMT_ARGB:  return bmdFormat8BitARGB;
    case DLK_PIXFMT_RGB10: return bmdFormat10BitRGB;
    default:               return (BMDPixelFormat)0;
    }
}

// If the requested pixel format doesn't fit the connector's available
// bandwidth for this display mode, fall back from 4:4:4 RGB to the
// same-bit-depth 4:2:2 YUV format — YouTube sources are 4:2:0 chroma
// subsampled already, so this costs no real fidelity, and it lets high frame
// rates (where 4:4:4 commonly exceeds single-link SDI bandwidth that 4:2:2
// fits within) keep outputting instead of failing to display at all.
static int resolve_pixfmt(DlkOutputIface &out, BMDDisplayMode mode, int pixfmt)
{
    int fallback = (pixfmt == DLK_PIXFMT_RGB10) ? DLK_PIXFMT_V210
                  : (pixfmt == DLK_PIXFMT_ARGB)  ? DLK_PIXFMT_UYVY
                  : 0;
    if (!fallback)
        return pixfmt;  // already 4:2:2 (or unknown) — nothing lower to fall back to

    BMDDisplayMode actual = bmdModeUnknown;
    DlkBool supported = false;
    HRESULT hr = out.DoesSupportVideoMode(
        bmdVideoConnectionUnspecified, mode, bmd_pixel_format(pixfmt),
        bmdNoVideoOutputConversion, bmdSupportedVideoModeDefault, &actual, &supported);
    if (hr == S_OK && supported)
        return pixfmt;

    dlk_logf(1, "DeckLink: requested pixel format unsupported at this mode "
                "(bandwidth) — falling back from 4:4:4 to 4:2:2");
    return fallback;
}

// ---------------------------------------------------------------------------
// Output lifecycle
// ---------------------------------------------------------------------------
DLK_EXPORT dlk_output *dlk_output_create(
    const char *device_name,
    const char *format_code,     // NULL/"" → auto-select from source/fixed_w/h
    int src_width, int src_height, double src_fps,
    int pixfmt,
    int preroll,
    int enable_audio, int audio_channels,
    int rgb_legal,               // 1 → scale RGB output to SMPTE legal levels
    int fixed_width, int fixed_height,  // >0,>0 → lock resolution, auto fps
    int link_mode)                // DLK_LINK_* — more links, more bandwidth
{
    com_thread_init();

    // new (never calloc) — the embedded OutputCallback needs its constructor
    // run so its vtable pointer is set before DeckLink threads call into it.
    dlk_output *d = new (std::nothrow) dlk_output{};
    if (!d)
        return nullptr;

    d->preroll        = preroll > 0 ? (preroll > 16 ? 16 : preroll) : 3;
    d->inflight_limit = d->preroll * 2;
    d->pixfmt         = pixfmt;
    d->rgb_legal      = rgb_legal != 0;
    init_rgb_luts(d);

    d->device = find_device(device_name);
    if (!d->device)
        goto fail;

    d->output = find_output_iface(d->device);
    if (!d->output.valid()) {
        dlk_logf(0, "DeckLink: device has no output interface");
        goto fail;
    }
    if (!d->output.zero_copy)
        dlk_logf(1, "DeckLink: connected via the %s SDK compatibility path "
                    "(older Desktop Video install) — a copy-based frame pool "
                    "will be used instead of zero-copy buffers",
                 d->output.v14 ? "pre-15.x" : "15.x");

    // Before mode enumeration: link configuration can change which display
    // modes the device offers (some high-bandwidth modes only appear once
    // dual link is set).
    set_sdi_link_configuration(d->device, link_mode);

    if (!find_mode(d, src_width, src_height, src_fps, format_code,
                   fixed_width, fixed_height))
        goto fail;

    d->pixfmt = resolve_pixfmt(d->output, d->bmd_mode, pixfmt);

    switch (d->pixfmt) {
    case DLK_PIXFMT_UYVY:
        d->pixel_format = bmdFormat8BitYUV;
        d->row_bytes    = d->width * 2;
        break;
    case DLK_PIXFMT_V210:
        d->pixel_format = bmdFormat10BitYUV;
        d->row_bytes    = ((d->width + 47) / 48) * 128;
        break;
    case DLK_PIXFMT_ARGB:
        d->pixel_format = bmdFormat8BitARGB;
        d->row_bytes    = d->width * 4;
        break;
    case DLK_PIXFMT_RGB10:
        d->pixel_format = bmdFormat10BitRGB;
        d->row_bytes    = d->width * 4;
        break;
    default:
        dlk_logf(0, "DeckLink: unknown pixel format %d", pixfmt);
        goto fail;
    }

    d->callback.lock             = &d->lock;
    d->callback.frames_in_flight = &d->frames_in_flight;
    d->output.SetScheduledFrameCompletionCallback(&d->callback);

    if (d->output.EnableVideoOutput(d->bmd_mode, bmdVideoOutputFlagDefault) != S_OK) {
        dlk_logf(0, "DeckLink: EnableVideoOutput failed (device in use?)");
        goto fail;
    }

    if (!d->output.zero_copy && !create_compat_pool(d)) {
        dlk_logf(0, "DeckLink: failed to allocate the compatibility frame pool");
        d->output.DisableVideoOutput();
        goto fail;
    }

    if (enable_audio) {
        d->audio_channels = (audio_channels == 8 || audio_channels == 16)
                            ? audio_channels : 2;
        if (d->output.EnableAudioOutput(
                bmdAudioSampleRate48kHz,
                bmdAudioSampleType32bitInteger,
                (uint32_t)d->audio_channels,
                bmdAudioOutputStreamTimestamped) == S_OK) {
            d->audio_enabled = true;
        } else {
            dlk_logf(1, "DeckLink: audio output unavailable — video only");
        }
    }

    if (d->output.StartScheduledPlayback(0, d->time_scale, 1.0) != S_OK) {
        dlk_logf(0, "DeckLink: StartScheduledPlayback failed");
        d->output.DisableVideoOutput();
        if (d->audio_enabled)
            d->output.DisableAudioOutput();
        goto fail;
    }

    return d;

fail:
    for (IDeckLinkMutableVideoFrame *f : d->compat_pool)
        if (f) f->Release();
    d->output.release();
    if (d->device) { d->device->Release(); d->device = nullptr; }
    delete d;
    return nullptr;
}

DLK_EXPORT void dlk_output_destroy(dlk_output *d)
{
    if (!d)
        return;
    if (d->output.valid()) {
        BMDTimeValue stop_time = 0;
        d->output.StopScheduledPlayback(0, &stop_time, d->time_scale);
        d->output.SetScheduledFrameCompletionCallback(nullptr);
        d->output.DisableVideoOutput();
        if (d->audio_enabled)
            d->output.DisableAudioOutput();
        d->output.release();
    }
    if (d->device)
        d->device->Release();
    // Release the v14.x fallback pool, if one was created.
    for (IDeckLinkMutableVideoFrame *f : d->compat_pool)
        if (f) f->Release();
    // Free any buffers that were never retired by the completion callback.
    while (d->callback.head < d->callback.tail) {
        MallocVideoBuffer *buf =
            d->callback.fifo[d->callback.head % OutputCallback::FIFO_SIZE];
        if (buf)
            buf->Release();
        d->callback.head++;
    }
    delete d;
}

DLK_EXPORT void dlk_output_get_info(dlk_output *d, int *width, int *height,
                                    double *fps, char *code /* >= 5 bytes */,
                                    int *audio_on, int *pixfmt)
{
    if (width)  *width  = d->width;
    if (height) *height = d->height;
    if (fps)    *fps    = d->mode_fps;
    if (code)   mode_code_str(d->bmd_mode, code);
    if (audio_on) *audio_on = d->audio_enabled ? 1 : 0;
    // The actually-negotiated pixel format — may differ from what was
    // requested if resolve_pixfmt() fell back to 4:2:2 for bandwidth.
    if (pixfmt) *pixfmt = d->pixfmt;
}

// ---------------------------------------------------------------------------
// Video feeding
// ---------------------------------------------------------------------------
DLK_EXPORT int dlk_output_can_send(dlk_output *d)
{
    std::lock_guard<std::mutex> guard(d->lock);
    return d->frames_in_flight < d->inflight_limit ? 1 : 0;
}

DLK_EXPORT int dlk_output_buffered_video_frames(dlk_output *d)
{
    uint32_t n = 0;
    d->output.GetBufferedVideoFrameCount(&n);
    return (int)n;
}

// Packs one packed-format frame (UYVY / ARGB / RGB10) into dst — already
// sized to d->row_bytes * d->height. For RGB10 the input must be x2rgb10le;
// rows are byte-swapped into the big-endian bmdFormat10BitRGB wire format on
// the fly. Shared by both the v16 zero-copy and v14 fallback-copy paths.
static void pack_packed_frame(dlk_output *d, const uint8_t *data, int stride, uint8_t *dst)
{
    const uint8_t *src = data;
    for (int y = 0; y < d->height; y++) {
        if (d->pixfmt == DLK_PIXFMT_RGB10) {
            // x2rgb10le → big-endian bmdFormat10BitRGB, with optional
            // full→legal scaling of each 10-bit component.
            const uint32_t *s   = (const uint32_t *)src;
            uint32_t       *d32 = (uint32_t *)dst;
            for (int x = 0; x < d->width; x++) {
                uint32_t w = s[x];
                if (d->rgb_legal) {
                    uint32_t r = d->lut10[(w >> 20) & 0x3FF];
                    uint32_t g = d->lut10[(w >> 10) & 0x3FF];
                    uint32_t b = d->lut10[ w        & 0x3FF];
                    w = (r << 20) | (g << 10) | b;
                }
#if defined(_MSC_VER)
                d32[x] = _byteswap_ulong(w);
#else
                d32[x] = __builtin_bswap32(w);
#endif
            }
        } else if (d->pixfmt == DLK_PIXFMT_ARGB && d->rgb_legal) {
            // Byte layout A,R,G,B — scale RGB, leave alpha untouched.
            for (int x = 0; x < d->width; x++) {
                dst[x*4 + 0] = src[x*4 + 0];
                dst[x*4 + 1] = d->lut8[src[x*4 + 1]];
                dst[x*4 + 2] = d->lut8[src[x*4 + 2]];
                dst[x*4 + 3] = d->lut8[src[x*4 + 3]];
            }
        } else {
            memcpy(dst, src, (size_t)d->row_bytes);
        }
        src += stride;
        dst += d->row_bytes;
    }
}

// Packs one planar YUV422P10LE frame into v210 in dst. Shared by both paths.
static void pack_planar_frame(dlk_output *d,
                              const uint8_t *y_plane, int y_stride,
                              const uint8_t *u_plane, int u_stride,
                              const uint8_t *v_plane, int v_stride, uint8_t *dst)
{
    for (int y = 0; y < d->height; y++) {
        const uint16_t *y_row = (const uint16_t *)(y_plane + (size_t)y * y_stride);
        const uint16_t *u_row = (const uint16_t *)(u_plane + (size_t)y * u_stride);
        const uint16_t *v_row = (const uint16_t *)(v_plane + (size_t)y * v_stride);
        pack_v210_row(y_row, u_row, v_row, dst + (size_t)y * d->row_bytes, d->width);
    }
}

// --- genuine 16.0 zero-copy path: MallocVideoBuffer + CreateVideoFrameWithBuffer ---

// Schedule mbuf as the next frame, displaying for `repeat` mode-frame periods
// (repeat > 1 when the mode fps is an integer multiple of the source fps).
static int schedule_frame(dlk_output *d, MallocVideoBuffer *mbuf, int repeat)
{
    if (repeat < 1)
        repeat = 1;

    IDeckLinkMutableVideoFrame *dl_frame = nullptr;
    if (d->output.v16->CreateVideoFrameWithBuffer(
            d->width, d->height, d->row_bytes, d->pixel_format,
            bmdFrameFlagDefault, mbuf, &dl_frame) != S_OK || !dl_frame) {
        dlk_logf(0, "DeckLink: CreateVideoFrameWithBuffer failed");
        mbuf->Release();
        return 0;
    }

    BMDTimeValue display_time = d->frame_count * d->frame_duration;
    HRESULT hr = d->output.ScheduleVideoFrame(
        dl_frame, display_time, d->frame_duration * repeat, d->time_scale);
    dl_frame->Release();
    if (hr != S_OK) {
        dlk_logf(0, "DeckLink: ScheduleVideoFrame failed (0x%08x)", (unsigned)hr);
        mbuf->Release();
        return 0;
    }

    // The buffer is held by the FIFO (not released here) until the completion
    // callback retires it.
    std::lock_guard<std::mutex> guard(d->lock);
    d->callback.push_inflight(mbuf);
    d->frames_in_flight++;
    d->frame_count += repeat;
    return 1;
}

// Allocate a pixel buffer, or return nullptr (with backpressure applied).
static MallocVideoBuffer *prepare_buffer(dlk_output *d)
{
    {
        std::lock_guard<std::mutex> guard(d->lock);
        if (d->frames_in_flight >= d->inflight_limit)
            return nullptr;
    }
    MallocVideoBuffer *mbuf = new (std::nothrow) MallocVideoBuffer(
        d->row_bytes, d->height);
    if (!mbuf || !mbuf->ok()) {
        if (mbuf)
            mbuf->Release();
        dlk_logf(0, "DeckLink: pixel buffer allocation failed");
        return nullptr;
    }
    return mbuf;
}

// --- pre-16.0 fallback path (15.x and 14.x): reused pool frames, refilled
// with a memcpy --- (the zero-copy CreateVideoFrameWithBuffer + 16.0
// IDeckLinkVideoBuffer ABI isn't safe on these tiers — see the
// dlk_output::compat_pool field comment.)

// Claims the next pool slot (or returns nullptr under backpressure — the
// same frames_in_flight gate the zero-copy path uses), for the caller to lock
// via FrameWriteBuffer, fill, and pass to schedule_compat_frame().
static IDeckLinkMutableVideoFrame *claim_compat_slot(dlk_output *d)
{
    std::lock_guard<std::mutex> guard(d->lock);
    if (d->frames_in_flight >= d->inflight_limit)
        return nullptr;
    IDeckLinkMutableVideoFrame *frame = d->compat_pool[(size_t)d->compat_pool_next];
    d->compat_pool_next = (d->compat_pool_next + 1) % (int)d->compat_pool.size();
    return frame;
}

static int schedule_compat_frame(dlk_output *d, IDeckLinkMutableVideoFrame *frame, int repeat)
{
    if (repeat < 1)
        repeat = 1;
    BMDTimeValue display_time = d->frame_count * d->frame_duration;
    HRESULT hr = d->output.ScheduleVideoFrame(
        frame, display_time, d->frame_duration * repeat, d->time_scale);
    if (hr != S_OK) {
        dlk_logf(0, "DeckLink: ScheduleVideoFrame failed (0x%08x)", (unsigned)hr);
        return 0;
    }
    // Unlike the v16 FIFO, pool slots are never released — they're reused in
    // place — so only the shared frames_in_flight/frame_count bookkeeping is
    // needed; OutputCallback::ScheduledFrameCompleted decrements the former
    // unconditionally regardless of which path scheduled the frame.
    std::lock_guard<std::mutex> guard(d->lock);
    d->frames_in_flight++;
    d->frame_count += repeat;
    return 1;
}

// Send a packed frame sized exactly to the output mode (UYVY / ARGB / RGB10).
DLK_EXPORT int dlk_output_send_packed(dlk_output *d, const uint8_t *data,
                                      int stride, int repeat)
{
    if (d->output.zero_copy) {
        MallocVideoBuffer *mbuf = prepare_buffer(d);
        if (!mbuf)
            return 0;
        pack_packed_frame(d, data, stride, mbuf->data());
        return schedule_frame(d, mbuf, repeat);
    }
    // Every pre-16.0 tier (15.x and 14.x): copy-based pool. See DlkOutputIface.

    IDeckLinkMutableVideoFrame *frame = claim_compat_slot(d);
    if (!frame)
        return 0;
    {
        FrameWriteBuffer wb(frame);
        if (!wb.buf) {
            dlk_logf(0, "DeckLink: could not lock compatibility frame buffer");
            return 0;
        }
        pack_packed_frame(d, data, stride, (uint8_t *)wb.buf);
    }
    return schedule_compat_frame(d, frame, repeat);
}

// Send a planar YUV422P10LE frame sized exactly to the output mode (v210).
DLK_EXPORT int dlk_output_send_planes(dlk_output *d,
                                      const uint8_t *y_plane, int y_stride,
                                      const uint8_t *u_plane, int u_stride,
                                      const uint8_t *v_plane, int v_stride,
                                      int repeat)
{
    if (d->pixfmt != DLK_PIXFMT_V210) {
        dlk_logf(0, "DeckLink: send_planes only valid for v210 output");
        return 0;
    }

    if (d->output.zero_copy) {
        MallocVideoBuffer *mbuf = prepare_buffer(d);
        if (!mbuf)
            return 0;
        pack_planar_frame(d, y_plane, y_stride, u_plane, u_stride, v_plane, v_stride,
                          mbuf->data());
        return schedule_frame(d, mbuf, repeat);
    }

    IDeckLinkMutableVideoFrame *frame = claim_compat_slot(d);
    if (!frame)
        return 0;
    {
        FrameWriteBuffer wb(frame);
        if (!wb.buf) {
            dlk_logf(0, "DeckLink: could not lock compatibility frame buffer");
            return 0;
        }
        pack_planar_frame(d, y_plane, y_stride, u_plane, u_stride, v_plane, v_stride,
                          (uint8_t *)wb.buf);
    }
    return schedule_compat_frame(d, frame, repeat);
}

// ---------------------------------------------------------------------------
// Audio feeding — timestamped against the same stream-time axis as video, so
// the hardware keeps them aligned.
// ---------------------------------------------------------------------------
DLK_EXPORT int dlk_output_send_audio(dlk_output *d, const int32_t *interleaved,
                                     int nframes)
{
    if (!d->audio_enabled || nframes <= 0)
        return 0;

    int64_t ts;
    {
        std::lock_guard<std::mutex> guard(d->lock);
        ts = d->audio_sample_pos;
    }

    uint32_t written = 0;
    HRESULT hr = d->output.ScheduleAudioSamples(
        (void *)interleaved, (uint32_t)nframes,
        ts, DLK_AUDIO_RATE, &written);
    if (hr != S_OK)
        return 0;

    int accepted = written ? (int)written : nframes;
    std::lock_guard<std::mutex> guard(d->lock);
    d->audio_sample_pos += accepted;
    return accepted;
}

DLK_EXPORT int dlk_output_buffered_audio_frames(dlk_output *d)
{
    if (!d->audio_enabled)
        return 0;
    uint32_t n = 0;
    d->output.GetBufferedAudioSampleFrameCount(&n);
    return (int)n;
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

// Hardware playback position in seconds since StartScheduledPlayback.
DLK_EXPORT double dlk_output_stream_time(dlk_output *d)
{
    BMDTimeValue t = 0;
    double speed = 1.0;
    if (d->output.GetScheduledStreamTime(d->time_scale, &t, &speed) != S_OK)
        return -1.0;
    return (double)t / (double)d->time_scale;
}

// Re-anchor the schedule after a pause, seek, or underrun.  While starved,
// the hardware clock keeps advancing past our frame counter; frames scheduled
// in the past would flush out in a burst.  Snap the frame counter (and the
// audio timestamp axis) to just ahead of the hardware clock.
DLK_EXPORT void dlk_output_resync(dlk_output *d)
{
    BMDTimeValue t = 0;
    double speed = 1.0;
    if (d->output.GetScheduledStreamTime(d->time_scale, &t, &speed) != S_OK)
        return;

    std::lock_guard<std::mutex> guard(d->lock);
    int64_t hw_frame = t / d->frame_duration + 2;   // 2-frame safety margin
    if (hw_frame > d->frame_count)
        d->frame_count = hw_frame;
    d->audio_sample_pos =
        d->frame_count * d->frame_duration * DLK_AUDIO_RATE / d->time_scale;
}
