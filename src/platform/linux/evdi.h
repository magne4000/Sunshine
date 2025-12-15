/**
 * @file src/platform/linux/evdi.h
 * @brief Declarations for EVDI virtual display support.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "src/platform/common.h"
#include "src/video.h"

namespace platf {
  /**
   * @brief Get the list of available EVDI virtual display names.
   * @return Vector of display name strings.
   */
  std::vector<std::string> evdi_display_names();

  /**
   * @brief Create an EVDI virtual display instance.
   * @param hwdevice_type The hardware device type for encoding.
   * @param display_name The name of the display to use (or empty for default).
   * @param config The video configuration from the client.
   * @return Shared pointer to the display instance, or nullptr on failure.
   */
  std::shared_ptr<display_t> evdi_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  /**
   * @brief Verify that EVDI virtual display support is available.
   * @return true if EVDI is available, false otherwise.
   */
  bool verify_evdi();

  /**
   * @brief Check if EVDI virtual display is currently active.
   * @return true if EVDI display is active, false otherwise.
   */
  bool evdi_is_active();

  /**
   * @brief Prepare and create EVDI virtual display for streaming session.
   * This should be called explicitly when streaming is about to start.
   * @param config The video configuration from the client (resolution, framerate, HDR).
   * @return true if successful, false otherwise.
   */
  bool evdi_prepare_stream(const video::config_t &config);

  /**
   * @brief Destroy the virtual display device when streaming stops.
   */
  void evdi_destroy_virtual_display();

}  // namespace platf
