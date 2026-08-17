/**
 * @file globals.h
 * @brief Declarations for globally accessible variables and functions.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>

// local includes
#include "entry_handler.h"
#include "thread_pool.h"

/**
 * @brief A thread pool for processing tasks.
 */
extern thread_pool_util::ThreadPool task_pool;

/**
 * @brief A boolean flag to indicate whether the cursor should be displayed.
 */
extern bool display_cursor;

/**
 * @brief Bumped whenever a client starts drawing the pointer itself.
 *
 * Capture backends that only publish a shape when the pointer *changes* have
 * nothing to say to a client that has just connected - the pointer has been the
 * same arrow for minutes. This tells them to publish the current one anyway.
 */
extern std::atomic<std::uint32_t> cursor_shape_refresh_epoch;

#ifdef _WIN32
  // Declare global singleton used for NVIDIA control panel modifications
  #include "platform/windows/nvprefs/nvprefs_interface.h"

/**
 * @brief A global singleton used for NVIDIA control panel modifications.
 */
extern nvprefs::nvprefs_interface nvprefs_instance;
#endif

/**
 * @brief Handles process-wide communication.
 */
namespace mail {
#define MAIL(x) \
  constexpr auto x = std::string_view { \
    #x \
  }

  /**
   * @brief A process-wide communication mechanism.
   */
  extern safe::mail_t man;

  // Global mail
  MAIL(shutdown);
  MAIL(broadcast_shutdown);
  MAIL(video_packets);
  MAIL(audio_packets);
  MAIL(switch_display);

  // Local mail
  MAIL(touch_port);
  MAIL(idr);
  MAIL(invalidate_ref_frames);
  MAIL(gamepad_feedback);
  MAIL(hdr);
  MAIL(dynamic_bitrate);  // Runtime encoder bitrate change (kbps), posted from the HTTP /bitrate handler
  MAIL(cursor_shape);  // platf::cursor_shape_t, raised by capture for clients drawing the pointer themselves
#undef MAIL

}  // namespace mail
