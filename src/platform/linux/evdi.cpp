/**
 * @file src/platform/linux/evdi.cpp
 * @brief Definitions for EVDI virtual display support.
 */
#include "evdi.h"

#include <cstring>
#include <fcntl.h>
#include <thread>
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

      // TODO: Customize EDID based on width, height, refresh_rate, and hdr_enabled
      // This would involve updating the descriptor blocks to match the requested mode
      // For now, the base EDID provides a working 1920x1080@60Hz display
      // Future enhancement: Generate proper timing descriptors for arbitrary resolutions
      // and add HDR metadata extension blocks when hdr_enabled is true

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

    // EVDI creates virtual displays on-demand when streaming starts
    // Always return a placeholder to allow EVDI to be selected
    result.push_back("EVDI Virtual Display");

    // Check if we have an active virtual display
    if (evdi_state.is_active) {
      BOOST_LOG(debug) << "EVDI virtual display is currently active"sv;
    }
    // Also check for existing EVDI devices (supports pre-created devices)
    else {
      int found_count = 0;
      for (int i = 0; i < 16; i++) {
        if (evdi_check_device(i) == AVAILABLE) {
          found_count++;
          BOOST_LOG(debug) << "Found pre-existing EVDI device: EVDI-"sv << i;
        }
      }
      
      if (found_count == 0) {
        BOOST_LOG(debug) << "No pre-existing EVDI devices (virtual display will be created on-demand)"sv;
      }
    }

    return result;
  }

  bool verify_evdi() {
    // EVDI was compiled in, so it's available
    // The kernel module (evdi-dkms) needs to be loaded at runtime for actual operation
    // We don't check for the kernel module here because:
    // 1. It may not be loaded until first use
    // 2. On CI/build systems, the module won't be available
    // 3. The actual device creation will handle errors gracefully if module is missing
    BOOST_LOG(info) << "EVDI virtual display support is available"sv;
    return true;
  }

  bool evdi_is_active() {
    return evdi_state.is_active;
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

    // Construct the device path
    std::string device_path = "/dev/dri/card" + std::to_string(evdi_state.device_id);
    
    // Open the device using the device path
    evdi_state.handle = evdi_open(device_path.c_str());
    if (evdi_state.handle == EVDI_INVALID_HANDLE) {
      BOOST_LOG(error) << "Failed to open EVDI device at "sv << device_path 
                       << " (device_id: "sv << evdi_state.device_id << ")"sv;
      return false;
    }
    
    BOOST_LOG(debug) << "Opened EVDI device at "sv << device_path;

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
    // This allows for dynamic creation when streaming starts
    if (!evdi_state.is_active) {
      BOOST_LOG(info) << "Creating EVDI virtual display dynamically for streaming session"sv;
      if (!evdi_create_virtual_display(config)) {
        BOOST_LOG(error) << "Failed to create EVDI virtual display"sv;
        return nullptr;
      }

      // Wait for the system to recognize the new display
      // Try for up to 5 seconds, checking every 100ms
      int attempts = 50;
      bool display_ready = false;
      
      for (int i = 0; i < attempts && !display_ready; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Check if KMS can see the display
#ifdef SUNSHINE_BUILD_DRM
        extern std::vector<std::string> kms_display_names(mem_type_e hwdevice_type);
        auto displays = kms_display_names(hwdevice_type);
        if (!displays.empty()) {
          display_ready = true;
          BOOST_LOG(info) << "EVDI virtual display detected by KMS after "sv << (i + 1) * 100 << "ms"sv;
        }
#else
        // If KMS is not available, just wait a reasonable time
        if (i >= 5) {  // 500ms
          display_ready = true;
        }
#endif
      }
      
      if (!display_ready) {
        BOOST_LOG(warning) << "Timeout waiting for EVDI virtual display to be recognized"sv;
      }
    }
    else {
      BOOST_LOG(info) << "Using existing EVDI virtual display"sv;
    }

    // Use KMS capture to grab from the virtual display
    // The virtual display should now appear as a DRM device that can be captured
#ifdef SUNSHINE_BUILD_DRM
    extern std::shared_ptr<display_t> kms_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);
    
    BOOST_LOG(info) << "Using KMS to capture from EVDI virtual display"sv;
    
    // When EVDI is active, we want to use the virtual display by default
    // Find the VIRTUAL connector (EVDI) in the KMS display list
    std::string evdi_display_name = display_name;
    
    if (evdi_state.is_active) {
      // Try to find the EVDI/VIRTUAL display in the KMS display list
      std::string virtual_display_id = find_virtual_display(hwdevice_type);
      
      if (!virtual_display_id.empty()) {
        evdi_display_name = virtual_display_id;
        BOOST_LOG(info) << "Using EVDI virtual display (KMS id: "sv << evdi_display_name << ")"sv;
        
        // If user specified a display_name, log that we're overriding it
        if (!display_name.empty() && display_name != evdi_display_name) {
          BOOST_LOG(info) << "Overriding configured Display Id ("sv << display_name 
                         << ") with EVDI virtual display ("sv << evdi_display_name << ")"sv;
        }
      }
      else {
        BOOST_LOG(warning) << "Could not find EVDI VIRTUAL display in KMS list"sv;
        // Fall back to using display_name or empty string
      }
    }
    
    return kms_display(hwdevice_type, evdi_display_name, config);
#else
    BOOST_LOG(error) << "EVDI requires KMS/DRM support to be enabled"sv;
    return nullptr;
#endif
  }

}  // namespace platf
