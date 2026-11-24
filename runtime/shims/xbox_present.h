#pragma once

// The Xbox presentation core (docs/d3d12_vulkan.md section 4.5): the frame events and PresentX of a GDKX title, which
// presents without a DXGI swapchain. FramePacer (src/frame_pacer.h) does the timing and the token bookkeeping;
// WindowPresenter (d3d12_com.h, over leonida::VulkanPresenter) puts the pixels in a window. This class is the
// glue, and the surface the Xbox device and queue proxy will forward to once the vtable slots are known.
//
// WHAT IS KNOWN: the public GDKX samples give the call order, the argument counts and the field names (section 4.5).
// WHAT IS OURS: every struct layout, constant value and HRESULT below is a provisional definition of this layer; the real
// slots, layouts, enum values and the token width are unmeasured. They are named like the console's so the proxy
// can swap them, but nothing here may be read as the console's ABI.
//
// TODO, the slots to measure before the proxy can forward (index in the vtable of the Xbox device / queue):
//   ID3D12Device (Xbox):        SetFrameIntervalX   (output, interval, latency, flags)           slot ?
//                               ScheduleFrameEventX (event, offset, reserved pointer, flags)     slot ?
//                               WaitFrameEventX     (event, timeout ms, reserved pointer, flags, &token)  slot ?
//                               GetFrameStatisticsX (token, type mask, &count, array)            slot ?
//   ID3D12CommandQueue (Xbox):  PresentX (plane count, planes, params)                           slot ?
//                               SuspendX (flags)                                                 slot ?
//                               ResumeX ()                                                       slot ?
// The queue-side calls do not look at the queue: the Vulkan device has one VkQueue behind every D3D12 queue, and
// the present is submitted after the work already executed on it.

#include "d3d12_com.h"
#include "../src/frame_pacer.h"
#include "../src/vulkan_present.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace rt::d3d12 {

// Provisional (ours): D3D12XBOX_FRAME_INTERVAL_* as periods in microseconds.
inline constexpr std::uint32_t xbox_frame_interval_30hz_us = 33333;
inline constexpr std::uint32_t xbox_frame_interval_60hz_us = 16667;
inline constexpr std::uint32_t xbox_frame_interval_120hz_us = 8333;

// Provisional (ours): D3D12XBOX_PRESENT_PLANE_PARAMETERS. The names are those the samples use; the layout, the
// token width (64 bits here) and the fields the console has beyond these are unmeasured.
//
// The fields after pSrcViewRects exist in the console's struct (names and order seen in a public binding, see the struct)
// but their types, enum values and flag bits are unmeasured: the values of ours below are placeholders that make the
// composition (section 4.5) testable. Zero everything is the default and means "as before": the whole output,
// bilinear when scaling, SDR, plane 1 premultiplied and not composed.
struct XboxViewRect {  // D3D12XBOX_VIEW_RECT: four floats; as fractions 0..1 of the output (hypothesis, section 4.5)
    float left, top, right, bottom;
};
// DXGI_COLOR_SPACE_TYPE values (documented DXGI numbers). Only these two are composed; any other is treated as the first.
inline constexpr int xbox_color_space_g22_p709 = 0;       // DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: SDR
inline constexpr int xbox_color_space_g2084_p2020 = 12;   // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: HDR10
// Provisional (ours): ScaleFilter and Flags.
inline constexpr int xbox_scale_filter_default = 0;  // bilinear (point when not scaled makes no difference)
inline constexpr int xbox_scale_filter_point = 1;
inline constexpr int xbox_scale_filter_linear = 2;
// Plane 1 is composed over plane 0 as an overlay only with this flag (hypothesis: whether the console blends a second plane
// at all is unmeasured; the public HDR sample's plane 1 is the GameDVR copy of the frame, not shown, so without the flag
// plane 1 is not composed, as before).
inline constexpr UINT xbox_plane_flag_overlay = 1;
inline constexpr UINT xbox_plane_flag_straight_alpha = 2;  // plane 1's colour is not premultiplied by its alpha

struct XboxPresentPlaneParameters {
    // Field ORDER and names: the public Go binding of the GDK header in hajimehoshi/ebiten (Apache-2.0,
    // internal/graphicsdriver/directx/d3d12_windows.go, _D3D12XBOX_PRESENT_PLANE_PARAMETERS); the HDR10 sample confirms
    // the first four and ColorSpace. Types, padding and the console's real sizes are unmeasured (ours: 64-bit token, int
    // enums), so nothing here is the console's ABI.
    std::uint64_t Token;                  // from WaitFrameEventX
    UINT ResourceCount;                   // 1 in the samples
    ID3D12Resource* const* ppResources;   // ResourceCount committed resources with ALLOW_DISPLAY, in PRESENT
    const D3D12_RECT* pSrcViewRects;      // null, or one rectangle per resource (the HDR sample sets it)
    const XboxViewRect* pDestPlacementBase;  // null: the whole output
    int ColorSpace;                       // a DXGI_COLOR_SPACE_TYPE (see above)
    int ScaleFilter;                      // xbox_scale_filter_*; anything unknown is the default
    UINT ExtendedDescCount;               // not used (logged once when non-zero)
    const void* pExtendedDescs;
    UINT Flags;                           // xbox_plane_flag_*; unknown bits are ignored
};

// Provisional (ours): D3D12XBOX_PRESENT_PARAMETERS. The samples show at least this field.
struct XboxPresentParameters {
    // 0 (the default): a frame that missed its vblank waits for the next one. Above 0: a frame that missed it is shown
    // at once, mid-scan, when the scan is not further than this percentage down the screen (100: wherever it is).
    // Needs VK_KHR/EXT_swapchain_maintenance1 and an IMMEDIATE or FIFO_RELAXED mode the swapchain can switch to;
    // without them it is ignored (logged once). What late means: see FramePacer::present_timing.
    float ImmediateThresholdPercent;
};

class XboxPresentation {
public:
    // `clock` null: real time (a steady high-resolution timer). Tests pass a leonida::ManualFrameClock, which must
    // outlive this object. `presenter` is what create_window_presenter returned.
    explicit XboxPresentation(std::unique_ptr<WindowPresenter> presenter, leonida::FrameClock* clock = nullptr);
    ~XboxPresentation();
    XboxPresentation(const XboxPresentation&) = delete;
    XboxPresentation& operator=(const XboxPresentation&) = delete;

    // Device side. `output` (an IDXGIOutput*, which the samples fetch from the device) is not used: there is one
    // window. `interval_period_us` is the period of a frame interval, e.g. xbox_frame_interval_60hz_us; `latency`
    // is the frames allowed in flight (the samples pass the buffer count minus 1, or 1).
    HRESULT SetFrameIntervalX(IUnknown* output, UINT interval_period_us, UINT latency, UINT flags);
    // Only the origin event (xbox_frame_event_origin) exists. `offset_us` delays it inside the frame. The pointer
    // argument the samples pass as null is not modelled.
    HRESULT ScheduleFrameEventX(UINT event, UINT offset_us, UINT flags);
    // Blocks for the next origin and while the latency is used up. HRESULT_FROM_WIN32(WAIT_TIMEOUT) after `timeout_ms`
    // (INFINITE waits for ever); `*token` is untouched then. E_INVALIDARG for a null token pointer or another event;
    // HRESULT_FROM_WIN32(ERROR_INVALID_STATE) when nothing is registered (not yet, or since SuspendX).
    HRESULT WaitFrameEventX(UINT event, DWORD timeout_ms, UINT flags, std::uint64_t* token);
    // Frame tokens up to `token` were handed out before this presentation existed (the device proxy's).
    void adopt_frame_tokens(std::uint64_t token) { pacer_.adopt_issued(token); }
    // See FramePacer::get_frame_statistics: S_FALSE and count 0 when nothing is available.
    HRESULT GetFrameStatisticsX(std::uint64_t token, UINT type_mask, UINT* count, leonida::FrameStatistic* out);

    // Queue side.
    //   0 planes: a black frame (what a title does before it releases its resources); no token is looked at.
    //   1 plane:  plane 0's first resource, which must be a single-sample 2D colour texture in the PRESENT state,
    //             is blitted to the window (format conversion by the blit; an sRGB format gets an sRGB swapchain),
    //             limited to pSrcViewRects[0] when that is set. This is the whole behaviour while the plane asks for
    //             nothing else (SDR colour space, whole output, default filter).
    //   Composed: a plane with pDestPlacementBase, a point or linear ScaleFilter, or the HDR10 colour space (and any
    //             second plane that is an overlay) is composed by a compute pass (VulkanPresenter::present_planes): plane 0
    //             from its source rectangle to its destination placement, plane 1 alpha-blended over it, colour spaces
    //             converted to the swapchain's (HDR10 when the surface has it, else HDR10 is tone mapped to SDR). Needs sampled,
    //             single-sample, non-integer sources; others give DXGI_ERROR_UNSUPPORTED.
    //   2 planes: the two share one token. Plane 1 is composed over plane 0 only when xbox_plane_flag_overlay is set on it
    //             (hypothesis); otherwise it is not shown (the GameDVR copy of the public HDR sample), logged once.
    //             Placement, ScaleFilter and Flags values that are not plausible are ignored and logged once, not refused.
    //   More than 2: E_INVALIDARG (the UMD limit, section 4.5).
    //   ResourceCount above 1 on any plane: DXGI_ERROR_UNSUPPORTED (logged once), nothing presented; only resource 0 is read.
    // Token: the one from WaitFrameEventX. Real behaviour for a null, repeated or out-of-order token is unmeasured and a
    // title must not freeze on it, so those are presented anyway and logged once per kind; only a token that was never
    // handed out gives E_INVALIDARG (nothing presented). Skipping a token is fine. Sources the GPU cannot blit
    // (multisampled, integer, block-compressed) give DXGI_ERROR_UNSUPPORTED and present nothing; resolving a
    // multisampled plane into a temporary is not done. While suspended: HRESULT_FROM_WIN32(ERROR_INVALID_STATE), also
    // for a present that was waiting for a swapchain image when SuspendX came.
    // `params` may be null (threshold 0). ImmediateThresholdPercent: see XboxPresentParameters; a frame that was shown
    // at once has its display statistic's percent_scanned set and is counted in immediate_presents().
    HRESULT PresentX(UINT plane_count, const XboxPresentPlaneParameters* planes, const XboxPresentParameters* params);
    // SuspendX drops the interval, the origin event and the frames in flight, and refuses presents. ResumeX allows
    // presents again; SetFrameIntervalX and ScheduleFrameEventX must be called again before the next wait.
    HRESULT SuspendX(UINT flags);
    HRESULT ResumeX();

    // What SDR white is in nits when SDR and HDR10 planes meet (docs section 4.5). `nits` > 0 overrides everything; 0 goes
    // back to automatic: while the swapchain is HDR10, the SDR content brightness Windows has for the output the window
    // is on (a background thread refreshes it once a second; PresentX only reads the cached value), else 203 (ITU-R
    // BT.2408's reference white). Applies to the next PresentX. The setters may be called from any thread.
    void set_sdr_white_nits(float nits) { sdr_white_override_ = nits > 0.0f ? nits : 0.0f; }
    // How SDR planes are decoded and an SDR output is encoded: piecewise sRGB (default) or gamma 2.2. The environment
    // variable LEONIDA_SDR_EOTF=gamma22 (or 2.2; any case; "srgb" is the default; other values are logged once and
    // ignored) chooses when the object is made.
    void set_sdr_eotf(leonida::PresentPlanes::SdrEotf eotf) { sdr_eotf_ = eotf; }
    // Peak of HDR10 content in nits, the white point of the tone map onto an SDR output (default 1000; no metadata is read).
    void set_hdr_peak_nits(float nits) { hdr_peak_nits_ = nits > 0.0f ? nits : 1000.0f; }

    leonida::FramePacer& pacer() { return pacer_; }
    WindowPresenter& presenter() { return *presenter_; }
    std::uint64_t presents() const { return presents_; }
    // Frames shown mid-scan because of ImmediateThresholdPercent.
    std::uint64_t immediate_presents() const { return pacer_.immediate_presents(); }

private:
    float effective_sdr_white_nits();  // override, else the cached Windows setting while HDR10, else 203
    void display_white_worker();       // refreshes display_white_ off the present path
    std::unique_ptr<WindowPresenter> presenter_;
    std::unique_ptr<leonida::FrameClock> own_clock_;
    leonida::FramePacer pacer_;
    std::mutex present_mutex_;  // PresentX, SuspendX and ResumeX one at a time
    std::atomic<std::uint64_t> presents_{0};
    std::uint64_t present_calls_ = 0;  // under present_mutex_
    HRESULT last_failure_ = S_OK;      // under present_mutex_
    bool logged_plane_words_ = false;  // under present_mutex_
    // PresentX's body, called under present_mutex_.
    HRESULT present_x(UINT plane_count, const XboxPresentPlaneParameters* planes, const XboxPresentParameters* params);
    bool logged_planes_ = false, logged_threshold_ = false, logged_display_ = false, logged_color_space_ = false,
         logged_extras_ = false, logged_resource_count_ = false;
    std::atomic<float> sdr_white_override_{0.0f};  // > 0: set_sdr_white_nits
    std::atomic<leonida::PresentPlanes::SdrEotf> sdr_eotf_{leonida::PresentPlanes::SdrEotf::srgb};
    std::atomic<float> hdr_peak_nits_{1000.0f};
    // The last good Windows SDR white level (nits, 0: none yet), refreshed by display_white_thread_ about once a second while
    // an HDR10 swapchain is in use (white_wanted_); a failed query keeps the previous value.
    std::atomic<float> display_white_{0.0f};
    std::atomic<bool> white_wanted_{false};
    std::mutex worker_mutex_;
    std::condition_variable worker_wake_;
    bool worker_stop_ = false;
    std::thread display_white_thread_;
    bool logged_null_token_ = false, logged_stale_token_ = false, logged_repeated_token_ = false;
};

inline constexpr UINT xbox_frame_event_origin = leonida::frame_event_origin;

}  // namespace rt::d3d12
