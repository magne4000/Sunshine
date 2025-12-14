/**
 * @file src/platform/linux/evdi.cpp
 * @brief Definitions for EVDI virtual display support.
 */
#include "evdi.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "graphics.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/video.h"

extern "C" {
#include <evdi_lib.h>
}

using namespace std::literals;

namespace platf {

  namespace {
    // Global state for virtual display management
    struct evdi_state_t {
      evdi_handle handle = EVDI_INVALID_HANDLE;
      int device_id = -1;
      bool is_active = false;
      int width = 1920;
      int height = 1080;
      int refresh_rate = 60;
      bool hdr_enabled = false;
    };

    evdi_state_t evdi_state;

    // Standard EDID for 1080p display
    // This is a basic EDID that will be customized based on client requirements
    const unsigned char base_edid[] = {
      0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,  // Header
      0x10, 0xAC,  // Manufacturer ID (Dell)
      0x00, 0x00,  // Product code
      0x00, 0x00, 0x00, 0x00,  // Serial number
      0x01,  // Week of manufacture
      0x1E,  // Year of manufacture (2020)
      0x01, 0x04,  // EDID version 1.4
      0xA5,  // Digital input, 8 bits per color
      0x34, 0x20,  // Screen size (52cm x 32cm)
      0x78,  // Display gamma 2.2
      0x3A,  // Features: DPMS, Preferred timing mode, sRGB
      // Chromaticity coordinates
      0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,
      // Established timings
      0x00, 0x00, 0x00,
      // Standard timing information (8 blocks)
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      // Descriptor blocks (4 blocks of 18 bytes each)
      // Block 1: Preferred timing (1920x1080@60Hz)
      0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,
      0x58, 0x2C, 0x45, 0x00, 0x09, 0x25, 0x21, 0x00,
      0x00, 0x1E,
      // Block 2: Display name
      0x00, 0x00, 0x00, 0xFC, 0x00,
      'S', 'u', 'n', 's', 'h', 'i', 'n', 'e', ' ', 'V', 'D', '\n', ' ',
      // Block 3: Display range limits
      0x00, 0x00, 0x00, 0xFD, 0x00,
      0x38, 0x4C, 0x1E, 0x53, 0x11, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
      // Block 4: Dummy
      0x00, 0x00, 0x00, 0x10, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      // Extension flag and checksum
      0x00, 0x00
    };

    /**
     * @brief Generate an EDID based on the requested display mode.
     * @param width Display width in pixels
     * @param height Display height in pixels
     * @param refresh_rate Display refresh rate in Hz
     * @param hdr_enabled Whether HDR is enabled
     * @return Vector containing the generated EDID
     */
    std::vector<unsigned char> generate_edid(int width, int height, int refresh_rate, bool hdr_enabled) {
      std::vector<unsigned char> edid(base_edid, base_edid + sizeof(base_edid));

      // For now, use the base EDID
      // TODO: Customize EDID based on width, height, refresh_rate, and hdr_enabled
      // This would involve updating the descriptor blocks to match the requested mode

      // Calculate and update checksum
      unsigned char checksum = 0;
      for (size_t i = 0; i < edid.size() - 1; i++) {
        checksum += edid[i];
      }
      edid[edid.size() - 1] = (256 - checksum) & 0xFF;

      return edid;
    }

    /**
     * @brief Event handler for mode changes.
     */
    void mode_changed_handler(struct evdi_mode mode, void *user_data) {
      BOOST_LOG(debug) << "EVDI mode changed: "sv << mode.width << "x"sv << mode.height
                       << "@"sv << mode.refresh_rate << " bpp="sv << mode.bits_per_pixel;

      evdi_state.width = mode.width;
      evdi_state.height = mode.height;
      evdi_state.refresh_rate = mode.refresh_rate;
    }

    /**
     * @brief Event handler for DPMS changes.
     */
    void dpms_handler(int dpms_mode, void *user_data) {
      BOOST_LOG(debug) << "EVDI DPMS mode: "sv << dpms_mode;
    }

    /**
     * @brief Event handler for update ready notifications.
     */
    void update_ready_handler(int buffer_to_be_updated, void *user_data) {
      // Buffer is ready to be updated
    }

    /**
     * @brief Event handler for CRTC state changes.
     */
    void crtc_state_handler(int state, void *user_data) {
      BOOST_LOG(debug) << "EVDI CRTC state: "sv << state;
    }

  }  // anonymous namespace

  std::vector<std::string> evdi_display_names() {
    std::vector<std::string> result;

    // Check if we have an active virtual display
    if (evdi_state.is_active) {
      result.push_back("EVDI Virtual Display");
    }
    // Also check for existing EVDI devices
    else {
      for (int i = 0; i < 16; i++) {
        if (evdi_check_device(i) == AVAILABLE) {
          result.push_back("EVDI-" + std::to_string(i));
        }
      }
    }

    return result;
  }

  bool verify_evdi() {
    // Check if evdi kernel module is loaded and we can add devices
    int device_id = evdi_add_device();
    if (device_id < 0) {
      BOOST_LOG(debug) << "EVDI not available: cannot add device"sv;
      return false;
    }

    // Check if we can open the device
    evdi_handle handle = evdi_open(device_id);
    if (handle == EVDI_INVALID_HANDLE) {
      BOOST_LOG(debug) << "EVDI not available: cannot open device"sv;
      return false;
    }

    evdi_close(handle);
    BOOST_LOG(info) << "EVDI virtual display support is available"sv;
    return true;
  }

  bool evdi_create_virtual_display(const video::config_t &config) {
    if (evdi_state.is_active) {
      BOOST_LOG(warning) << "EVDI virtual display already active"sv;
      return true;
    }

    // Add a new EVDI device
    evdi_state.device_id = evdi_add_device();
    if (evdi_state.device_id < 0) {
      BOOST_LOG(error) << "Failed to add EVDI device"sv;
      return false;
    }

    // Open the device
    evdi_state.handle = evdi_open(evdi_state.device_id);
    if (evdi_state.handle == EVDI_INVALID_HANDLE) {
      BOOST_LOG(error) << "Failed to open EVDI device "sv << evdi_state.device_id;
      return false;
    }

    // Configure display parameters from client config
    evdi_state.width = config.width;
    evdi_state.height = config.height;
    evdi_state.refresh_rate = config.framerate;

    // Check if HDR is requested (10-bit color depth)
    evdi_state.hdr_enabled = (config.dynamicRange > 0);

    // Generate EDID for the requested mode
    auto edid = generate_edid(evdi_state.width, evdi_state.height,
                              evdi_state.refresh_rate, evdi_state.hdr_enabled);

    BOOST_LOG(info) << "Creating EVDI virtual display: "sv
                    << evdi_state.width << "x"sv << evdi_state.height
                    << "@"sv << evdi_state.refresh_rate << "Hz"
                    << (evdi_state.hdr_enabled ? " (HDR)"sv : ""sv);

    // Connect the display with the EDID
    evdi_connect(evdi_state.handle, edid.data(), edid.size(), 0);

    // Set up event handlers
    struct evdi_event_context event_context = {};
    event_context.mode_changed_handler = mode_changed_handler;
    event_context.dpms_handler = dpms_handler;
    event_context.update_ready_handler = update_ready_handler;
    event_context.crtc_state_handler = crtc_state_handler;
    event_context.user_data = nullptr;

    // Process initial events
    evdi_handle_events(evdi_state.handle, &event_context);

    evdi_state.is_active = true;

    BOOST_LOG(info) << "EVDI virtual display created successfully"sv;
    return true;
  }

  void evdi_destroy_virtual_display() {
    if (!evdi_state.is_active) {
      return;
    }

    BOOST_LOG(info) << "Destroying EVDI virtual display"sv;

    if (evdi_state.handle != EVDI_INVALID_HANDLE) {
      evdi_disconnect(evdi_state.handle);
      evdi_close(evdi_state.handle);
      evdi_state.handle = EVDI_INVALID_HANDLE;
    }

    evdi_state.device_id = -1;
    evdi_state.is_active = false;

    BOOST_LOG(info) << "EVDI virtual display destroyed"sv;
  }

  std::shared_ptr<display_t> evdi_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    // Create the virtual display if not already active
    if (!evdi_state.is_active) {
      if (!evdi_create_virtual_display(config)) {
        BOOST_LOG(error) << "Failed to create EVDI virtual display"sv;
        return nullptr;
      }

      // Give the system a moment to recognize the new display
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Use KMS capture to grab from the virtual display
    // The virtual display should now appear as a DRM device that can be captured
#ifdef SUNSHINE_BUILD_DRM
    extern std::shared_ptr<display_t> kms_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);
    
    BOOST_LOG(info) << "Using KMS to capture from EVDI virtual display"sv;
    return kms_display(hwdevice_type, display_name, config);
#else
    BOOST_LOG(error) << "EVDI requires KMS/DRM support to be enabled"sv;
    return nullptr;
#endif
  }

}  // namespace platf
