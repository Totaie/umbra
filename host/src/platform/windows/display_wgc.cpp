/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Windows Game Capture (WGC) IPC display implementation with shared session helper and DXGI fallback.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>
#include <winsock2.h>
#include <dxgi1_2.h>
#include <optional>
#include <wrl/client.h>

// local includes
#include "src/config.h"
#include "ipc/ipc_session.h"
#include "ipc/misc_utils.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/display_vram.h"
#include "src/platform/windows/game_activity.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/virtual_display.h"
#include "src/utility.h"

// platform includes
#include <winrt/base.h>

namespace platf::dxgi {
  namespace {
    struct wgc_dxgi_fallback_state_t {
      bool secure_desktop_active;
      bool recent_desktop_switch;
    };

    std::atomic<uint64_t> g_wgc_snapshot_copies {0};
    std::atomic<uint64_t> g_wgc_slow_snapshot_locks {0};
    std::atomic<uint64_t> g_wgc_slow_snapshot_copies {0};

    class adapter_luid_override_guard {
    public:
      explicit adapter_luid_override_guard(const std::optional<LUID> &luid) {
        previous_ = get_dxgi_adapter_luid_override();
        if (luid.has_value()) {
          set_dxgi_adapter_luid_override(luid);
        }
      }

      ~adapter_luid_override_guard() {
        set_dxgi_adapter_luid_override(previous_);
      }

    private:
      std::optional<LUID> previous_;
    };

    std::optional<wgc_dxgi_fallback_state_t> get_wgc_dxgi_fallback_state() {
      wgc_dxgi_fallback_state_t state {
        .secure_desktop_active = platf::dxgi::is_secure_desktop_active(),
        .recent_desktop_switch = recent_wgc_desktop_switch_grace_active()
      };

      if (!state.secure_desktop_active && !state.recent_desktop_switch) {
        return std::nullopt;
      }

      return state;
    }

    void log_wgc_dxgi_fallback_reason(const char *path_name, const wgc_dxgi_fallback_state_t &state) {
      if (state.secure_desktop_active && state.recent_desktop_switch) {
        BOOST_LOG(debug) << "Secure desktop detected and the desktop-switch grace window is active; "
                         << "using DXGI fallback for WGC capture (" << path_name << ")";
      } else if (state.secure_desktop_active) {
        BOOST_LOG(debug) << "Secure desktop detected, using DXGI fallback for WGC capture (" << path_name << ")";
      } else {
        BOOST_LOG(debug) << "Recent desktop switch grace window active, using DXGI fallback for WGC capture ("
                         << path_name << ")";
      }
    }

    bool is_wgc_constant_mode() {
      return config::video.capture == "wgcc";
    }

    // The pointer shape, for clients drawing the cursor themselves.
    //
    // Desktop Duplication hands the shape over with the frame, so the DXGI backend gets
    // this for free. WGC does not - it burns the pointer into the image and tells us
    // nothing about it - so on Windows 11 23H2 and later, where WGC is the default, a
    // client that hid the host's cursor was left drawing a plain arrow over everything,
    // with no I-beam over text and no resize arrows on a window edge. Ask Windows
    // directly instead. GetCursorInfo is a cheap call and the handle it returns is
    // stable per shape, so the expensive conversion only runs when the pointer changes.
    constexpr int CURSOR_MAX_DIMENSION = 256;

    /**
     * @brief Read a GDI bitmap as top-down 32bpp BGRA.
     *
     * A monochrome bitmap comes back as 0x00000000 for a clear bit and 0x00FFFFFF for
     * a set one, which is what the AND/XOR handling below relies on.
     */
    bool read_bitmap_bgra(HDC dc, HBITMAP bitmap, int width, int height, std::vector<std::uint8_t> &out) {
      BITMAPINFO info {};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = width;
      info.bmiHeader.biHeight = -height;  // Negative means top-down, matching the wire format
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;

      out.assign((std::size_t) width * height * 4, 0);
      return GetDIBits(dc, bitmap, 0, (UINT) height, out.data(), &info, DIB_RGB_COLORS) == height;
    }

    /**
     * @brief Convert an HCURSOR into the BGRA shape the client expects.
     * @return A shape with has_shape set, or an empty one for anything unrepresentable.
     */
    platf::cursor_shape_t cursor_shape_from_handle(HCURSOR handle) {
      constexpr std::uint32_t black = 0xFF000000;
      constexpr std::uint32_t white = 0xFFFFFFFF;
      constexpr std::uint32_t transparent = 0;

      ICONINFO icon_info {};
      if (!GetIconInfo(handle, &icon_info)) {
        return {};
      }

      auto bitmaps = util::fail_guard([&]() {
        if (icon_info.hbmMask) {
          DeleteObject(icon_info.hbmMask);
        }
        if (icon_info.hbmColor) {
          DeleteObject(icon_info.hbmColor);
        }
      });

      if (!icon_info.hbmMask) {
        return {};
      }

      BITMAP mask_bm {};
      if (GetObject(icon_info.hbmMask, sizeof(mask_bm), &mask_bm) == 0) {
        return {};
      }

      // A colour cursor keeps its transparency in the colour bitmap's alpha and the mask
      // is the same height. A monochrome one has no colour bitmap and stacks the AND mask
      // above the XOR mask in a single bitmap of twice the height.
      const bool monochrome = icon_info.hbmColor == nullptr;

      BITMAP color_bm {};
      if (!monochrome && GetObject(icon_info.hbmColor, sizeof(color_bm), &color_bm) == 0) {
        return {};
      }

      const int width = monochrome ? mask_bm.bmWidth : color_bm.bmWidth;
      const int height = monochrome ? mask_bm.bmHeight / 2 : color_bm.bmHeight;

      if (width <= 0 || height <= 0 || width > CURSOR_MAX_DIMENSION || height > CURSOR_MAX_DIMENSION) {
        return {};
      }

      HDC dc = GetDC(nullptr);
      if (dc == nullptr) {
        return {};
      }
      auto release_dc = util::fail_guard([&]() {
        ReleaseDC(nullptr, dc);
      });

      platf::cursor_shape_t cursor;
      cursor.width = (std::uint16_t) width;
      cursor.height = (std::uint16_t) height;
      cursor.hotspot_x = (std::int16_t) icon_info.xHotspot;
      cursor.hotspot_y = (std::int16_t) icon_info.yHotspot;

      std::vector<std::uint8_t> mask;
      if (!read_bitmap_bgra(dc, icon_info.hbmMask, width, monochrome ? height * 2 : height, mask)) {
        return {};
      }
      auto mask_px = (const std::uint32_t *) mask.data();

      if (monochrome) {
        cursor.pixels.assign((std::size_t) width * height * 4, 0);
        auto pixels = (std::uint32_t *) cursor.pixels.data();

        for (int i = 0; i < width * height; i++) {
          const bool and_bit = (mask_px[i] & 0x00FFFFFF) != 0;
          const bool xor_bit = (mask_px[(std::size_t) width * height + i] & 0x00FFFFFF) != 0;

          if (!and_bit) {
            pixels[i] = xor_bit ? white : black;
          } else {
            // "Inverse of what's behind it" has no meaning to a client compositing the
            // pointer over its own copy of the frame, so it becomes black - the same
            // choice the Desktop Duplication path makes.
            pixels[i] = xor_bit ? black : transparent;
          }
        }
      } else {
        if (!read_bitmap_bgra(dc, icon_info.hbmColor, width, height, cursor.pixels)) {
          return {};
        }

        auto pixels = (std::uint32_t *) cursor.pixels.data();

        // Cursors built from a 24bpp image come back with the alpha channel all zero,
        // which would draw nothing at all. Those still carry a real AND mask, so use it.
        const bool has_alpha = std::any_of(pixels, pixels + (std::size_t) width * height, [](std::uint32_t px) {
          return (px & 0xFF000000) != 0;
        });

        if (!has_alpha) {
          for (int i = 0; i < width * height; i++) {
            const bool and_bit = (mask_px[i] & 0x00FFFFFF) != 0;
            pixels[i] = and_bit ? transparent : (pixels[i] | 0xFF000000);
          }
        }
      }

      cursor.has_shape = true;
      return cursor;
    }

    /**
     * @brief Publish the current pointer shape if it changed since the last call.
     *
     * Cheap enough to call once per captured frame. State is shared across displays on
     * purpose: there is one pointer, and a multi-display session should not have each
     * capture thread reporting it separately.
     */
    void poll_win32_cursor_shape() {
      static std::mutex mutex;
      static HCURSOR last_handle = nullptr;
      static bool last_visible = false;
      static bool ever_polled = false;
      static std::uint32_t shape_id = 0;
      static std::uint32_t seen_refresh_epoch = 0;
      static platf::cursor_shape_t last_shape;
      static std::chrono::steady_clock::time_point last_poll {};
      static std::chrono::steady_clock::time_point last_desktop_sync {};

      auto lg = std::lock_guard(mutex);

      const auto now = std::chrono::steady_clock::now();
      const auto epoch = cursor_shape_refresh_epoch.load(std::memory_order_acquire);
      const bool refresh_requested = epoch != seen_refresh_epoch;

      // The pointer changing 30 times a second is already far more than anyone can
      // see, and this runs on the capture thread - at 144Hz the GDI work below was
      // competing with the encoder for it.
      if (!refresh_requested && now - last_poll < std::chrono::milliseconds(32)) {
        return;
      }
      last_poll = now;

      CURSORINFO info {};
      info.cbSize = sizeof(info);
      if (!GetCursorInfo(&info)) {
        // Usually means this thread is looking at a desktop that isn't the input one -
        // a UAC prompt, a lock screen, or a display switch tearing capture down and
        // bringing it back. Follow it, but not on every frame: syncThreadDesktop opens
        // and closes a desktop handle and logs on failure, and doing that at frame rate
        // through a display switch is enough to stall the switch itself.
        if (now - last_desktop_sync >= std::chrono::seconds(1)) {
          last_desktop_sync = now;
          syncThreadDesktop();
        }
        return;
      }

      const bool visible = (info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr;

      if (!refresh_requested && ever_polled &&
          info.hCursor == last_handle && visible == last_visible) {
        return;
      }

      const bool handle_changed = refresh_requested || !ever_polled || info.hCursor != last_handle;
      seen_refresh_epoch = epoch;
      ever_polled = true;
      last_handle = info.hCursor;
      last_visible = visible;

      if (handle_changed && info.hCursor != nullptr) {
        auto shape = cursor_shape_from_handle(info.hCursor);
        if (shape.has_shape) {
          shape.shape_id = ++shape_id;
          shape.visible = visible;
          last_shape = shape;
          mail::man->event<platf::cursor_shape_t>(mail::cursor_shape)->raise(std::move(shape));
          return;
        }
      }

      // Visibility changed, or the new shape was one we can't represent. Either way the
      // client only needs to be told whether to keep drawing what it already has.
      platf::cursor_shape_t visibility = last_shape;
      visibility.visible = visible;
      mail::man->event<platf::cursor_shape_t>(mail::cursor_shape)->raise(std::move(visibility));
    }

    std::shared_ptr<platf::game_activity::refresh_target_t> make_wgc_activity_admission_target(
      ipc_session_t &ipc_session,
      const ::video::config_t &config,
      const std::string &display_name,
      const RECT &capture_rect
    ) {
      if (!::config::frame_limiter.game_aware_virtual_display_refresh_enabled() ||
          ::config::video.dd.refresh_rate_option == ::config::video_t::dd_t::refresh_rate_option_e::manual ||
          config.framerate <= 0 || !VDISPLAY::is_virtual_display_output(display_name)) {
        return {};
      }

      const auto base_rate = static_cast<std::uint64_t>(config.framerate);
      const auto max_admission = static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
      const auto desktop_admission = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        base_rate * 2ull,
        max_admission
      ));
      const auto game_admission = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        base_rate * 4ull,
        max_admission
      ));

      return platf::game_activity::make_refresh_target({
        .display_name = display_name,
        .capture_rect = capture_rect,
        .base_refresh_numerator = desktop_admission,
        .base_refresh_denominator = 1,
        .high_refresh_numerator = game_admission,
        .high_refresh_denominator = 1,
        .initial_high = false,
        .apply_activity_state = [&ipc_session, desktop_admission, game_admission](const bool game_active) {
          const auto admission = game_active ? game_admission : desktop_admission;
          return ipc_session.set_activity_admission_fps(static_cast<int>(admission));
        },
      });
    }

    std::chrono::milliseconds effective_wgc_timeout(std::chrono::milliseconds timeout, int client_framerate) {
      if (timeout.count() != 0) {
        return timeout;
      }

      // WGC's instantaneous publish cadence is vsync-stepped on the source
      // monitor (frame_qpc_delta tracks 240/N for an N integer), so the
      // window between our sleep_target wake and the next helper publish
      // can be up to one full helper interarrival. When DWM is composing
      // at 240/2 = 120 fps the gap is ~8.3 ms, but when it bounces between
      // 240 and 120 the average interarrival drops to ~6 ms and a 4 ms
      // grace times out frequently -- captured rate falls visibly below
      // the helper publish rate even though the helper is producing more
      // than enough frames. 6 ms covers the high-rate regime while still
      // staying under the 8.33 ms 120 Hz cadence with ~2 ms of slack for
      // the snapshot's CPU work.
      auto grace = std::chrono::milliseconds(6);

      if (is_wgc_constant_mode() && client_framerate > 0) {
        // In constant mode a timed-out zero-timeout snapshot forwards the
        // cached frame, so the pacing group must survive the full grace: the
        // snapshot returns at slot + grace, and if that lands past the next
        // slot the capture loop invalidates the pacing group and re-anchors
        // through a 200 ms blocking snapshot. Above ~166 fps the client frame
        // interval is shorter than the 6 ms grace, so an idle screen busts
        // the group on every forwarded frame and capture collapses to the
        // compositor's publish rate (umbra#267). Clamp the grace to leave
        // ~2 ms of the slot for wait/scheduling overhead.
        const auto frame_interval = std::chrono::nanoseconds(std::chrono::seconds(1)) / client_framerate;
        const auto clamped = std::chrono::duration_cast<std::chrono::milliseconds>(frame_interval - std::chrono::milliseconds(2));
        grace = std::clamp(clamped, std::chrono::milliseconds(1), grace);
      }

      return grace;
    }

    // The helper detects UAC/lock transitions via a desktop-switch hook, but it runs
    // with the user's token and its notification can be missed entirely (hook not yet
    // installed, pipe down, event lost). Without it the stream freezes on the last
    // pre-UAC frame forever. As a safety net, once frame delivery stalls, probe the
    // input desktop from this process; when the secure desktop is active, force a
    // reinit so the factory swaps to the DXGI fallback.
    bool wgc_stall_requires_dxgi_fallback(std::chrono::steady_clock::time_point &stall_start, std::chrono::steady_clock::time_point &last_probe) {
      constexpr auto stall_before_first_probe = std::chrono::milliseconds(200);
      constexpr auto probe_interval = std::chrono::milliseconds(250);

      const auto now = std::chrono::steady_clock::now();
      if (stall_start.time_since_epoch().count() == 0) {
        stall_start = now;
        last_probe = {};
      }

      if (now - stall_start < stall_before_first_probe) {
        return false;
      }

      if (last_probe.time_since_epoch().count() != 0 && now - last_probe < probe_interval) {
        return false;
      }

      last_probe = now;
      return is_secure_desktop_active();
    }

    capture_e forward_cached_wgc_frame(std::shared_ptr<platf::img_t> cached_frame, std::shared_ptr<platf::img_t> &img_out) {
      if (!cached_frame) {
        return capture_e::timeout;
      }

      const auto now = std::chrono::steady_clock::now();
      cached_frame->frame_timestamp = now;
      cached_frame->host_processing_timestamp = now;
      cached_frame->capture_pacing_timestamp = now;
      img_out = std::move(cached_frame);
      return capture_e::ok;
    }
  }  // namespace

  display_wgc_ipc_vram_t::display_wgc_ipc_vram_t() = default;

  display_wgc_ipc_vram_t::~display_wgc_ipc_vram_t() {
    game_refresh_target.reset();
    if (_frame_locked && _ipc_session) {
      _ipc_session->release();
      _frame_locked = false;
    }
  }

  int display_wgc_ipc_vram_t::init(
    const ::video::config_t &config,
    const std::string &display_name,
    const std::optional<LUID> &required_adapter_luid
  ) {
    _config = config;
    _display_name = display_name;

    if (display_base_t::init(
          config,
          display_name,
          true /* skip_dd_test: WGC doesn't use Desktop Duplication */,
          required_adapter_luid)) {
      return -1;
    }

    capture_format = DXGI_FORMAT_UNKNOWN;  // Start with unknown format (prevents race condition/crash on first frame)

    const bool advanced_color_capture = is_hdr();

    // Create session
    _ipc_session = std::make_unique<ipc_session_t>();
    if (_ipc_session->init(config, display_name, device.get(), advanced_color_capture)) {
      return -1;
    }
    game_refresh_target = make_wgc_activity_admission_target(
      *_ipc_session,
      config,
      display_name,
      captured_output_desc.DesktopCoordinates
    );

    return 0;
  }

  capture_e display_wgc_ipc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (!_ipc_session) {
      return capture_e::error;
    }

    // We return capture::reinit for most scenarios because the logic in picking which mode to capture is all handled in the factory function.
    if (_ipc_session->should_swap_to_dxgi()) {
      return capture_e::reinit;
    }

    // Generally this only becomes true if the helper process has crashed or is otherwise not responding.
    if (_ipc_session->should_reinit()) {
      return capture_e::reinit;
    }

    _ipc_session->initialize_if_needed();
    if (!_ipc_session->is_initialized()) {
      BOOST_LOG(warning) << "WGC IPC helper failed to initialize; requesting capture reinit.";
      return capture_e::reinit;
    }

    // Desktop Duplication hands the pointer over separately and lets us decide whether
    // to blend it in; WGC has already composited it by the time the frame reaches us.
    // So this is the only place display_cursor can be honoured on this backend - and
    // it not being honoured here is why hiding the host's cursor did nothing on
    // Windows 11 23H2 and later, where WGC is the default.
    _ipc_session->set_cursor_capture(cursor_visible);

    // Nothing else on this backend tells a client what the pointer looks like.
    poll_win32_cursor_shape();

    timeout = effective_wgc_timeout(timeout, _config.framerate);

    auto capture_status = _ipc_session->wait_for_frame(timeout);
    if (capture_status != capture_e::ok) {
      if (capture_status == capture_e::timeout) {
        if (wgc_stall_requires_dxgi_fallback(_wgc_stall_start, _last_secure_desktop_probe)) {
          BOOST_LOG(info) << "WGC frames stalled while the secure desktop is active; falling back to DXGI capture";
          note_wgc_desktop_switch();
          return capture_e::reinit;
        }
        if (is_wgc_constant_mode()) {
          return forward_cached_wgc_frame(_last_cached_frame, img_out);
        }
      }
      return capture_status;
    }
    _wgc_stall_start = {};

    // Peek the shared texture's descriptor without taking the keyed mutex.
    // The descriptor is fixed at session setup; reading it here lets us know
    // the capture format on the very first frame so complete_img() can run
    // before we ever touch the shared IPC mutex.
    D3D11_TEXTURE2D_DESC desc;
    if (!_ipc_session->peek_shared_texture_desc(desc)) {
      return capture_e::reinit;
    }

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format [" << dxgi_format_to_string(capture_format) << ']';
    }

    // Display enumeration can race with mode changes and produce mismatched
    // image pool and desktop texture sizes. Detect that early.
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width_before_rotation << 'x' << height_before_rotation << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    // The capture format can change on the fly; if so, reinit to refresh the
    // image pool and detection state.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Pull a free image from the pool before touching the shared IPC keyed
    // mutex. The encoder image pool can block under pressure; holding the
    // shared mutex during that wait stalls the WGC helper producer.
    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img)) {
      return capture_e::interrupted;
    }

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    if (complete_img(d3d_img.get(), false)) {
      return capture_e::error;
    }

    // Acquire the encoder image's keyed mutex BEFORE the shared IPC mutex.
    // The encoder may still be sampling the previous frame; if we held the
    // shared IPC mutex during this wait, the helper's delivery thread would
    // block, its WGC frame pool would back up, and the compositor would drop
    // frames at the source. Taking the encoder mutex first keeps the shared
    // IPC mutex critical section bounded to the GPU-copy submission only.
    const auto capture_mutex_wait_start = std::chrono::steady_clock::now();
    HRESULT status = d3d_img->capture_mutex->AcquireSync(0, 3000);
    const auto capture_mutex_wait = std::chrono::steady_clock::now() - capture_mutex_wait_start;
    if (status == WAIT_ABANDONED) {
      BOOST_LOG(error) << "Capture texture keyed mutex was abandoned; continuing with lock held";
    } else if (status != S_OK) {
      BOOST_LOG(error) << "Failed to lock capture texture [0x"sv << util::hex(status).to_string_view() << ']';
      return capture_e::error;
    }

    auto release_capture_mutex = util::fail_guard([&]() {
      const HRESULT release_status = d3d_img->capture_mutex->ReleaseSync(0);
      if (FAILED(release_status)) {
        BOOST_LOG(warning) << "Failed to release capture texture mutex [0x"sv << util::hex(release_status).to_string_view() << ']';
      }
    });

    texture2d_t src;
    uint64_t frame_qpc = 0;
    winrt::com_ptr<ID3D11Texture2D> gpu_tex;
    capture_status = _ipc_session->lock_frame(gpu_tex, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }
    gpu_tex.copy_to(&src);
    _frame_locked = true;

    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    auto frame_timestamp = host_processing_timestamp - qpc_time_difference(qpc_counter(), frame_qpc);

    // The IPC texture is a single mutable helper-owned surface. Snapshot it into
    // this pool-owned texture so queued encoder frames remain stable.
    const auto copy_start = std::chrono::steady_clock::now();
    device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
    const auto copy_submit = std::chrono::steady_clock::now() - copy_start;
    d3d_img->blank = false;

    // Release the shared IPC mutex immediately after queueing the copy. The
    // GPU work is fenced through the keyed-mutex / encoder pipeline, so the
    // helper is free to publish the next frame as soon as we drop this mutex.
    _ipc_session->release();
    _frame_locked = false;

    const auto copy_count = g_wgc_snapshot_copies.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto capture_mutex_wait_ms = std::chrono::duration<double, std::milli>(capture_mutex_wait).count();
    const auto copy_submit_ms = std::chrono::duration<double, std::milli>(copy_submit).count();
    const bool slow_lock = capture_mutex_wait_ms > 1.0;
    const bool slow_copy = copy_submit_ms > 1.0;
    if (slow_lock) {
      g_wgc_slow_snapshot_locks.fetch_add(1, std::memory_order_relaxed);
    }
    if (slow_copy) {
      g_wgc_slow_snapshot_copies.fetch_add(1, std::memory_order_relaxed);
    }
    if (copy_count == 1 || copy_count % 600 == 0 || slow_lock || slow_copy) {
      BOOST_LOG(debug) << "WGC snapshot copy timing: frame=" << copy_count
                       << " capture_mutex_wait_ms=" << capture_mutex_wait_ms
                       << " copy_submit_ms=" << copy_submit_ms
                       << " slow_locks=" << g_wgc_slow_snapshot_locks.load(std::memory_order_relaxed)
                       << " slow_copies=" << g_wgc_slow_snapshot_copies.load(std::memory_order_relaxed);
    }

    img->frame_timestamp = frame_timestamp;
    img->host_processing_timestamp = host_processing_timestamp;
    // Keep WGC's QPC-derived timestamp for RTP/client accounting, but do not
    // use compositor timestamp jitter as the capture-loop sleep anchor.
    img->capture_pacing_timestamp = host_processing_timestamp;
    img_out = img;
    _last_cached_frame = img;

    return capture_e::ok;
  }

  capture_e display_wgc_ipc_vram_t::acquire_next_frame(std::chrono::milliseconds timeout, texture2d_t &src, uint64_t &frame_qpc, bool cursor_visible) {
    if (!_ipc_session) {
      return capture_e::error;
    }

    winrt::com_ptr<ID3D11Texture2D> gpu_tex;
    auto status = _ipc_session->acquire(effective_wgc_timeout(timeout, _config.framerate), gpu_tex, frame_qpc);

    if (status != capture_e::ok) {
      return status;
    }

    gpu_tex.copy_to(&src);
    _frame_locked = true;

    return capture_e::ok;
  }

  capture_e display_wgc_ipc_vram_t::release_snapshot() {
    if (_ipc_session && _frame_locked) {
      _ipc_session->release();
      _frame_locked = false;
    }
    return capture_e::ok;
  }

  int display_wgc_ipc_vram_t::dummy_img(platf::img_t *img_base) {
    // Use the base class implementation which creates a blank GPU texture directly,
    // avoiding Desktop Duplication which may fail on headless/disconnected sessions.
    return complete_img(img_base, true);
  }

  std::shared_ptr<display_t> display_wgc_ipc_vram_t::create(
    const ::video::config_t &config,
    const std::string &display_name,
    const std::optional<LUID> &required_adapter_luid
  ) {
    if (auto fallback_state = get_wgc_dxgi_fallback_state()) {
      log_wgc_dxgi_fallback_reason("VRAM", *fallback_state);
      adapter_luid_override_guard guard(get_last_wgc_adapter_luid());
      auto disp = std::make_shared<temp_dxgi_vram_t>();
      if (!disp->init(config, display_name, required_adapter_luid)) {
        return disp;
      }
    } else {
      // Secure desktop not active, use WGC IPC
      BOOST_LOG(debug) << "Using WGC IPC implementation (VRAM)";
      auto disp = std::make_shared<display_wgc_ipc_vram_t>();
      if (!disp->init(config, display_name, required_adapter_luid)) {
        return disp;
      }
    }

    return nullptr;
  }

  display_wgc_ipc_ram_t::display_wgc_ipc_ram_t() = default;

  display_wgc_ipc_ram_t::~display_wgc_ipc_ram_t() {
    game_refresh_target.reset();
  }

  int display_wgc_ipc_ram_t::init(
    const ::video::config_t &config,
    const std::string &display_name,
    const std::optional<LUID> &required_adapter_luid
  ) {
    // Save config for later use
    _config = config;
    _display_name = display_name;

    // Initialize the base display class
    if (display_base_t::init(
          config,
          display_name,
          true /* skip_dd_test: WGC doesn't use Desktop Duplication */,
          required_adapter_luid)) {
      return -1;
    }

    // Initialize capture format to unknown - will be determined from first frame
    capture_format = DXGI_FORMAT_UNKNOWN;

    // Note: WGC captures at monitor native resolution, not the requested config resolution.
    // The display helper handles resolution changes before capture starts if needed.
    // We use the dimensions set by display_base_t::init() which reflect the actual monitor size.

    const bool advanced_color_capture = is_hdr();

    // Create session
    _ipc_session = std::make_unique<ipc_session_t>();
    if (_ipc_session->init(config, display_name, device.get(), advanced_color_capture)) {
      return -1;
    }
    game_refresh_target = make_wgc_activity_admission_target(
      *_ipc_session,
      config,
      display_name,
      captured_output_desc.DesktopCoordinates
    );

    return 0;
  }

  capture_e display_wgc_ipc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (!_ipc_session) {
      return capture_e::error;
    }

    if (_ipc_session->should_swap_to_dxgi()) {
      return capture_e::reinit;
    }

    // If the helper process crashed or was terminated forcefully by the user, we will re-initialize it.
    if (_ipc_session->should_reinit()) {
      return capture_e::reinit;
    }

    _ipc_session->initialize_if_needed();
    if (!_ipc_session->is_initialized()) {
      BOOST_LOG(warning) << "WGC IPC helper failed to initialize; requesting capture reinit.";
      return capture_e::reinit;
    }

    // See the VRAM path above: WGC composites the pointer in the helper, so this is
    // where display_cursor has to be applied.
    _ipc_session->set_cursor_capture(cursor_visible);

    // Nothing else on this backend tells a client what the pointer looks like.
    poll_win32_cursor_shape();

    winrt::com_ptr<ID3D11Texture2D> gpu_tex;
    uint64_t frame_qpc = 0;
    timeout = effective_wgc_timeout(timeout, _config.framerate);
    auto status = _ipc_session->acquire(timeout, gpu_tex, frame_qpc);

    if (status != capture_e::ok) {
      if (status == capture_e::timeout) {
        if (wgc_stall_requires_dxgi_fallback(_wgc_stall_start, _last_secure_desktop_probe)) {
          BOOST_LOG(info) << "WGC frames stalled while the secure desktop is active; falling back to DXGI capture";
          note_wgc_desktop_switch();
          return capture_e::reinit;
        }
        if (is_wgc_constant_mode()) {
          return forward_cached_wgc_frame(_last_cached_frame, img_out);
        }
      }
      // For the default mode just return the capture status on timeouts.
      return status;
    }
    _wgc_stall_start = {};

    // Get description of the captured texture
    D3D11_TEXTURE2D_DESC desc;
    gpu_tex->GetDesc(&desc);

    // If we don't know the capture format yet, grab it from this texture
    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format [" << dxgi_format_to_string(capture_format) << ']';
    }

    // Check for size changes - use width_before_rotation/height_before_rotation since WGC
    // captures textures in unrotated physical pixel dimensions, same as VRAM path
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed [" << width_before_rotation << 'x' << height_before_rotation << " -> " << desc.Width << 'x' << desc.Height << ']';
      _ipc_session->release();
      return capture_e::reinit;
    }

    // Check for format changes
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed [" << dxgi_format_to_string(capture_format) << " -> " << dxgi_format_to_string(desc.Format) << ']';
      _ipc_session->release();
      return capture_e::reinit;
    }

    // Create or recreate staging texture if needed
    // Use unrotated dimensions to match the captured texture size
    if (!texture ||
        width_before_rotation != _last_width ||
        height_before_rotation != _last_height ||
        capture_format != _last_format) {
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.Format = capture_format;
      t.ArraySize = 1;
      t.MipLevels = 1;
      t.SampleDesc = {1, 0};
      t.Usage = D3D11_USAGE_STAGING;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto hr = device->CreateTexture2D(&t, nullptr, &texture);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[display_wgc_ipc_ram_t] Failed to create staging texture: " << hr;
        _ipc_session->release();
        return capture_e::error;
      }

      _last_width = width_before_rotation;
      _last_height = height_before_rotation;
      _last_format = capture_format;

      BOOST_LOG(info) << "[display_wgc_ipc_ram_t] Created staging texture: "
                      << width_before_rotation << "x" << height_before_rotation << ", format: " << capture_format;
    }

    // Copy from shared texture to staging texture (queues GPU work)
    device_ctx->CopyResource(texture.get(), gpu_tex.get());

    // CRITICAL: Release the keyed mutex BEFORE blocking on Map()
    // The helper needs the mutex to write the next frame while we're reading this one
    _ipc_session->release();

    // Get a free image from the pool
    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }

    auto img = img_out.get();

    // If we don't know the final capture format yet, encode a dummy image
    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      if (dummy_img(img)) {
        return capture_e::error;
      }
    } else {
      // Map the staging texture for CPU access (blocks until GPU copy completes)
      auto hr = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[display_wgc_ipc_ram_t] Failed to map staging texture: " << hr;
        return capture_e::error;
      }

      // Now that we know the capture format, we can finish creating the image
      if (complete_img(img, false)) {
        device_ctx->Unmap(texture.get(), 0);
        img_info.pData = nullptr;
        return capture_e::error;
      }

      // Copy data - use height_before_rotation since WGC captures unrotated texture
      std::copy_n((std::uint8_t *) img_info.pData, height_before_rotation * img_info.RowPitch, img->data);

      // Unmap the staging texture to allow GPU access again
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
    }

    // Set frame timestamp
    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    auto frame_timestamp = host_processing_timestamp - qpc_time_difference(qpc_counter(), frame_qpc);
    img->frame_timestamp = frame_timestamp;
    img->host_processing_timestamp = host_processing_timestamp;
    img->capture_pacing_timestamp = host_processing_timestamp;
    _last_cached_frame = img_out;

    return capture_e::ok;
  }

  capture_e display_wgc_ipc_ram_t::release_snapshot() {
    // Not used in RAM path since we handle everything in snapshot()
    return capture_e::ok;
  }

  int display_wgc_ipc_ram_t::dummy_img(platf::img_t *img_base) {
    // Use the base class implementation directly,
    // avoiding Desktop Duplication which may fail on headless/disconnected sessions.
    return display_ram_t::dummy_img(img_base);
  }

  std::shared_ptr<display_t> display_wgc_ipc_ram_t::create(
    const ::video::config_t &config,
    const std::string &display_name,
    const std::optional<LUID> &required_adapter_luid
  ) {
    if (auto fallback_state = get_wgc_dxgi_fallback_state()) {
      log_wgc_dxgi_fallback_reason("RAM", *fallback_state);
      adapter_luid_override_guard guard(get_last_wgc_adapter_luid());
      auto disp = std::make_shared<temp_dxgi_ram_t>();
      if (!disp->init(config, display_name, required_adapter_luid)) {
        return disp;
      }
    } else {
      // Secure desktop not active, use WGC IPC
      BOOST_LOG(debug) << "Using WGC IPC implementation (RAM)";
      auto disp = std::make_shared<display_wgc_ipc_ram_t>();
      if (!disp->init(config, display_name, required_adapter_luid)) {
        return disp;
      }
    }

    return nullptr;
  }

  capture_e temp_dxgi_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    // Check periodically if secure desktop is still active
    if (auto now = std::chrono::steady_clock::now(); now - _last_check_time >= CHECK_INTERVAL) {
      _last_check_time = now;
      const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
      if (!secure_desktop_active && !recent_wgc_desktop_switch_grace_active()) {
        BOOST_LOG(debug) << "DXGI Capture is no longer necessary, swapping back to WGC!";
        return capture_e::reinit;
      }
    }

    // Call parent DXGI duplication implementation
    return display_ddup_vram_t::snapshot(pull_free_image_cb, img_out, timeout, cursor_visible);
  }

  capture_e temp_dxgi_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    // Check periodically if secure desktop is still active
    if (auto now = std::chrono::steady_clock::now(); now - _last_check_time >= CHECK_INTERVAL) {
      _last_check_time = now;
      const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
      if (!secure_desktop_active && !recent_wgc_desktop_switch_grace_active()) {
        BOOST_LOG(debug) << "DXGI Capture is no longer necessary, swapping back to WGC!";
        return capture_e::reinit;
      }
    }

    // Call parent DXGI duplication implementation
    return display_ddup_ram_t::snapshot(pull_free_image_cb, img_out, timeout, cursor_visible);
  }

}  // namespace platf::dxgi
