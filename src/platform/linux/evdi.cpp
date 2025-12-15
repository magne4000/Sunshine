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
      bool is_active = false;
      int width = 1920;
      int height = 1080;
      int refresh_rate = 60;
      bool hdr_enabled = false;
      bool allow_device_creation = false;  // Set to true when ready for actual streaming
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
    BOOST_LOG(debug) << "EVDI: evdi_display_names() called, is_active="sv << evdi_state.is_active;
    
    std::vector<std::string> result;

    // EVDI creates virtual displays on-demand when streaming starts
    // Always return a placeholder to allow EVDI to be selected
    result.push_back("EVDI Virtual Display");

    // Check if we have an active virtual display
    if (evdi_state.is_active) {
      BOOST_LOG(debug) << "EVDI: Virtual display is currently active"sv;
    }
    else {
      BOOST_LOG(debug) << "EVDI: Virtual display will be created on-demand when needed"sv;
    }

    BOOST_LOG(debug) << "EVDI: Returning "sv << result.size() << " display name(s)"sv;
    return result;
  }

  bool verify_evdi() {
    // EVDI was compiled in, so it's available for use
    // We don't try to create devices or check device status here
    // The library will be used when streaming actually starts
    BOOST_LOG(debug) << "EVDI: verify_evdi() called - EVDI support compiled in"sv;
    BOOST_LOG(info) << "EVDI: Virtual display support is available"sv;
    BOOST_LOG(debug) << "EVDI: Runtime requires evdi-dkms kernel module (v1.14.11 or compatible)"sv;
    BOOST_LOG(debug) << "EVDI: Virtual display will be created on-demand when streaming starts"sv;
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

    BOOST_LOG(info) << "Creating EVDI virtual display for streaming session"sv;
    BOOST_LOG(debug) << "EVDI: Requested display config: "sv << config.width << "x"sv << config.height 
                     << "@"sv << config.framerate << "Hz, dynamicRange="sv << config.dynamicRange;

    // Check if the EVDI kernel module is properly loaded by checking for sysfs interface
    // The evdi module creates /sys/devices/evdi/add when properly loaded
    BOOST_LOG(debug) << "EVDI: Checking if kernel module is properly loaded..."sv;
    if (access("/sys/devices/evdi", F_OK) != 0) {
      BOOST_LOG(error) << "EVDI: /sys/devices/evdi does not exist"sv;
      BOOST_LOG(error) << "EVDI: The evdi kernel module is either not loaded or failed to initialize"sv;
      BOOST_LOG(error) << "EVDI: Install evdi-dkms package (v1.14.11) and run: sudo modprobe evdi"sv;
      BOOST_LOG(debug) << "EVDI: After loading, verify with: ls -la /sys/devices/evdi/"sv;
      BOOST_LOG(debug) << "EVDI: Check kernel logs with: dmesg | grep evdi"sv;
      return false;
    }
    
    if (access("/sys/devices/evdi/add", F_OK) != 0) {
      BOOST_LOG(error) << "EVDI: /sys/devices/evdi/add does not exist"sv;
      BOOST_LOG(error) << "EVDI: The evdi kernel module loaded but sysfs interface is incomplete"sv;
      BOOST_LOG(error) << "EVDI: Try unloading and reloading: sudo modprobe -r evdi && sudo modprobe evdi"sv;
      BOOST_LOG(debug) << "EVDI: Check kernel logs for errors: dmesg | tail -50 | grep evdi"sv;
      return false;
    }
    
    BOOST_LOG(debug) << "EVDI: Kernel module sysfs interface found - proceeding with device creation"sv;

    // Use evdi_open_attached_to(NULL) which will:
    // 1. Find an unused EVDI device, or
    // 2. Add a new device by writing to /sys/devices/evdi/add and then open it
    // This is the proper way to create/open an EVDI device
    BOOST_LOG(debug) << "EVDI: Calling evdi_open_attached_to(NULL) to create/open device"sv;
    
    evdi_handle handle = EVDI_INVALID_HANDLE;
    try {
      handle = evdi_open_attached_to(NULL);
      BOOST_LOG(debug) << "EVDI: evdi_open_attached_to() returned handle="sv << (void*)handle;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "EVDI: Exception in evdi_open_attached_to(): "sv << e.what();
      BOOST_LOG(error) << "EVDI: This indicates a problem with the EVDI library or kernel module"sv;
      return false;
    }
    catch (...) {
      BOOST_LOG(error) << "EVDI: Unknown exception in evdi_open_attached_to()"sv;
      BOOST_LOG(error) << "EVDI: This indicates a serious problem with the EVDI library or kernel module"sv;
      return false;
    }
    
    evdi_state.handle = handle;
    
    if (evdi_state.handle == EVDI_INVALID_HANDLE) {
      BOOST_LOG(error) << "EVDI: Failed to open/create EVDI device"sv;
      BOOST_LOG(error) << "EVDI: evdi_open_attached_to() returned EVDI_INVALID_HANDLE"sv;
      BOOST_LOG(debug) << "EVDI: Check 'lsmod | grep evdi' to verify kernel module is loaded"sv;
      BOOST_LOG(debug) << "EVDI: Check 'dmesg | grep evdi' for kernel error messages"sv;
      return false;
    }
    
    // Successfully opened EVDI device
    BOOST_LOG(info) << "EVDI: Opened EVDI virtual display device"sv;
    BOOST_LOG(debug) << "EVDI: Device handle: "sv << (void*)evdi_state.handle;

    // Configure display parameters from client config
    evdi_state.width = config.width;
    evdi_state.height = config.height;
    evdi_state.refresh_rate = config.framerate;

    // Check if HDR is requested (10-bit color depth)
    evdi_state.hdr_enabled = (config.dynamicRange > 0);

    // Generate EDID for the requested mode
    BOOST_LOG(debug) << "EVDI: Generating EDID for "sv << evdi_state.width << "x"sv << evdi_state.height
                     << "@"sv << evdi_state.refresh_rate << "Hz"sv;
    auto edid = generate_edid(evdi_state.width, evdi_state.height,
                              evdi_state.refresh_rate, evdi_state.hdr_enabled);

    BOOST_LOG(info) << "EVDI: Connecting virtual display: "sv
                    << evdi_state.width << "x"sv << evdi_state.height
                    << "@"sv << evdi_state.refresh_rate << "Hz"
                    << (evdi_state.hdr_enabled ? " (HDR)"sv : ""sv);

    // Connect the display with the EDID
    BOOST_LOG(debug) << "EVDI: Calling evdi_connect() with "sv << edid.size() << " byte EDID"sv;
    evdi_connect(evdi_state.handle, edid.data(), edid.size(), 0);

    // Set up event handlers
    BOOST_LOG(debug) << "EVDI: Setting up event handlers"sv;
    struct evdi_event_context event_context = {};
    event_context.mode_changed_handler = mode_changed_handler;
    event_context.dpms_handler = dpms_handler;
    event_context.update_ready_handler = update_ready_handler;
    event_context.crtc_state_handler = crtc_state_handler;
    event_context.user_data = nullptr;

    // Process initial events
    BOOST_LOG(debug) << "EVDI: Processing initial events"sv;
    evdi_handle_events(evdi_state.handle, &event_context);

    evdi_state.is_active = true;

    BOOST_LOG(info) << "EVDI: Virtual display created successfully"sv;
    BOOST_LOG(debug) << "EVDI: Display state - width="sv << evdi_state.width 
                     << ", height="sv << evdi_state.height 
                     << ", refresh_rate="sv << evdi_state.refresh_rate;
    return true;
  }

  void evdi_destroy_virtual_display() {
    if (!evdi_state.is_active) {
      BOOST_LOG(debug) << "EVDI: destroy_virtual_display called but display not active"sv;
      return;
    }

    BOOST_LOG(info) << "EVDI: Destroying virtual display"sv;

    if (evdi_state.handle != EVDI_INVALID_HANDLE) {
      BOOST_LOG(debug) << "EVDI: Disconnecting and closing device handle"sv;
      evdi_disconnect(evdi_state.handle);
      evdi_close(evdi_state.handle);
      evdi_state.handle = EVDI_INVALID_HANDLE;
    }

    evdi_state.is_active = false;
    evdi_state.allow_device_creation = false;  // Reset flag when destroying

    BOOST_LOG(info) << "EVDI: Virtual display destroyed"sv;
  }

  void evdi_enable_device_creation() {
    BOOST_LOG(debug) << "EVDI: Device creation enabled - ready for streaming"sv;
    evdi_state.allow_device_creation = true;
  }

  std::shared_ptr<display_t> evdi_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    BOOST_LOG(debug) << "EVDI: evdi_display() called - hwdevice_type="sv << (int)hwdevice_type 
                     << ", display_name='"sv << display_name << "', is_active="sv << evdi_state.is_active 
                     << ", allow_device_creation="sv << evdi_state.allow_device_creation;
    
#ifndef SUNSHINE_BUILD_DRM
    BOOST_LOG(error) << "EVDI: EVDI requires KMS/DRM support to be enabled"sv;
    return nullptr;
#else
    // EVDI virtual displays are created on-demand when streaming starts
    // The allow_device_creation flag ensures we only create devices during actual streaming,
    // not during encoder validation at startup
    
    if (!evdi_state.allow_device_creation && !evdi_state.is_active) {
      // During encoder validation, we don't have a display yet
      // This is expected and normal - encoder validation now handles this gracefully
      BOOST_LOG(debug) << "EVDI: Device creation not enabled yet - encoder validation will use defaults"sv;
      return nullptr;
    }
    
    if (!evdi_state.is_active) {
      BOOST_LOG(info) << "EVDI: Creating virtual display for streaming session"sv;
      if (!evdi_create_virtual_display(config)) {
        BOOST_LOG(error) << "EVDI: Failed to create virtual display"sv;
        BOOST_LOG(error) << "EVDI: Make sure the evdi kernel module (evdi-dkms package v1.14.11) is installed and loaded"sv;
        BOOST_LOG(error) << "EVDI: Check 'lsmod | grep evdi' and 'ls -la /sys/devices/evdi/'"sv;
        return nullptr;
      }

      // Wait for the system to recognize the new display
      // The DRM/KMS subsystem needs time to detect the new card
      BOOST_LOG(debug) << "EVDI: Waiting 500ms for KMS to detect new display"sv;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      BOOST_LOG(debug) << "EVDI: Wait complete, proceeding to use KMS for capture"sv;
    }
    else {
      BOOST_LOG(debug) << "EVDI: Reusing existing virtual display"sv;
    }

    // Use KMS capture to grab from the virtual display
    // The virtual display should now appear as a DRM device that can be captured
    extern std::shared_ptr<display_t> kms_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);
    
    BOOST_LOG(debug) << "EVDI: Using KMS to capture from EVDI virtual display"sv;
    
    // When EVDI is active, we want to use the virtual display by default
    // Find the VIRTUAL connector (EVDI) in the KMS display list
    std::string evdi_display_name = display_name;
    
    if (evdi_state.is_active && evdi_state.handle != EVDI_INVALID_HANDLE) {
      BOOST_LOG(debug) << "EVDI: Searching for VIRTUAL connector in KMS display list"sv;
      
      // Try to find the EVDI/VIRTUAL display in the KMS display list
      // Protect against exceptions in case KMS isn't properly initialized
      try {
        extern std::string find_virtual_display(mem_type_e hwdevice_type);
        std::string virtual_display_id = find_virtual_display(hwdevice_type);
        
        if (!virtual_display_id.empty()) {
          evdi_display_name = virtual_display_id;
          BOOST_LOG(info) << "EVDI: Found virtual display with KMS id: "sv << evdi_display_name;
          
          // If user specified a display_name, log that we're overriding it
          if (!display_name.empty() && display_name != evdi_display_name) {
            BOOST_LOG(info) << "EVDI: Overriding configured Display Id ("sv << display_name 
                           << ") with EVDI virtual display ("sv << evdi_display_name << ")"sv;
          }
        }
        else {
          BOOST_LOG(warning) << "EVDI: Could not find VIRTUAL connector in KMS list"sv;
          BOOST_LOG(debug) << "EVDI: This may indicate the display hasn't been detected yet by KMS"sv;
          // Fall back to using display_name or empty string
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "EVDI: Exception while finding virtual display: "sv << e.what();
        BOOST_LOG(debug) << "EVDI: This may occur if KMS is not fully initialized - falling back to default"sv;
        // Fall back to using display_name or empty string
      }
      catch (...) {
        BOOST_LOG(warning) << "EVDI: Unknown exception while finding virtual display"sv;
        // Fall back to using display_name or empty string
      }
    }
    
    BOOST_LOG(debug) << "EVDI: Calling kms_display() with display_name='"sv << evdi_display_name << "'"sv;
    
    std::shared_ptr<display_t> result;
    try {
      result = kms_display(hwdevice_type, evdi_display_name, config);
      if (result) {
        BOOST_LOG(debug) << "EVDI: kms_display() succeeded, returning display handle"sv;
      }
      else {
        BOOST_LOG(error) << "EVDI: kms_display() returned nullptr"sv;
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "EVDI: Exception in kms_display(): "sv << e.what();
      return nullptr;
    }
    catch (...) {
      BOOST_LOG(error) << "EVDI: Unknown exception in kms_display()"sv;
      return nullptr;
    }
    
    return result;
#endif
  }

}  // namespace platf
