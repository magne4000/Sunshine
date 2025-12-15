/**
 * @file src/platform/linux/evdi.cpp
 * @brief Definitions for EVDI virtual display support.
 */
#include "evdi.h"

#include <chrono>
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
     * @brief Generate a DTD (Detailed Timing Descriptor) for the given resolution.
     * Uses CVT (Coordinated Video Timings) reduced blanking formulas.
     */
    void generate_dtd(unsigned char *dtd, int width, int height, int refresh_rate) {
      // CVT reduced blanking timing calculations
      // These are simplified values - proper CVT would calculate exact timings
      
      // For common resolutions, use standard timings
      // Otherwise, use approximate values based on CVT reduced blanking
      
      int h_blank = width / 5;  // Approximate horizontal blanking
      int v_blank = 30;         // Vertical blanking lines
      int h_sync = 32;          // H-sync pulse width
      int v_sync = 4;           // V-sync pulse width
      
      int pixel_clock_khz = ((width + h_blank) * (height + v_blank) * refresh_rate) / 1000;
      
      // DTD structure (18 bytes):
      // Bytes 0-1: Pixel clock in 10 kHz units (little endian)
      dtd[0] = (pixel_clock_khz / 10) & 0xFF;
      dtd[1] = ((pixel_clock_khz / 10) >> 8) & 0xFF;
      
      // Bytes 2-3: Horizontal addressable pixels and blanking
      dtd[2] = width & 0xFF;
      dtd[3] = h_blank & 0xFF;
      dtd[4] = ((width >> 8) & 0x0F) | (((h_blank >> 8) & 0x0F) << 4);
      
      // Bytes 5-6: Vertical addressable lines and blanking
      dtd[5] = height & 0xFF;
      dtd[6] = v_blank & 0xFF;
      dtd[7] = ((height >> 8) & 0x0F) | (((v_blank >> 8) & 0x0F) << 4);
      
      // Bytes 8-10: Sync pulse parameters
      int h_sync_offset = (h_blank - h_sync) / 2;
      int v_sync_offset = 3;
      
      dtd[8] = h_sync_offset & 0xFF;
      dtd[9] = h_sync & 0xFF;
      dtd[10] = ((v_sync_offset & 0x0F) << 4) | (v_sync & 0x0F);
      dtd[11] = ((h_sync_offset >> 8) & 0x03) | 
                (((h_sync >> 8) & 0x03) << 2) |
                (((v_sync_offset >> 4) & 0x03) << 4) |
                (((v_sync >> 4) & 0x03) << 6);
      
      // Bytes 12-13: Image size (52cm x 32cm - approximate 24" 16:9 display)
      dtd[12] = 0x20;
      dtd[13] = 0x34;
      dtd[14] = 0x00;
      
      // Bytes 15-16: Border and flags
      dtd[15] = 0x00;  // No border
      dtd[16] = 0x00;  // No border
      
      // Byte 17: Flags (digital separate sync, positive polarity)
      dtd[17] = 0x1E;
    }

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

      // Generate custom DTD (Detailed Timing Descriptor) for the requested resolution
      // DTD is located at bytes 54-71 (first descriptor block)
      generate_dtd(&edid[54], width, height, refresh_rate);
      
      BOOST_LOG(debug) << "EVDI: Generated custom EDID with DTD for "sv << width << "x"sv << height 
                       << "@"sv << refresh_rate << "Hz"sv;

      // TODO: Add HDR metadata extension blocks when hdr_enabled is true
      // This would require adding a CTA-861 extension block with HDR static metadata
      if (hdr_enabled) {
        BOOST_LOG(debug) << "EVDI: HDR requested but HDR EDID extension not yet implemented"sv;
      }

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

  bool evdi_prepare_stream(const video::config_t &config) {
    if (evdi_state.is_active) {
      BOOST_LOG(warning) << "EVDI virtual display already active"sv;
      return true;
    }

    BOOST_LOG(info) << "Preparing EVDI virtual display for streaming session"sv;
    BOOST_LOG(debug) << "EVDI: Requested display config: "sv << config.width << "x"sv << config.height 
                     << "@"sv << config.framerate << "Hz, dynamicRange="sv << config.dynamicRange;

    // Check if the EVDI kernel module is properly loaded by checking for sysfs interface
    BOOST_LOG(debug) << "EVDI: Checking if kernel module is properly loaded..."sv;
    if (access("/sys/devices/evdi", F_OK) != 0) {
      BOOST_LOG(error) << "EVDI: /sys/devices/evdi does not exist"sv;
      BOOST_LOG(error) << "EVDI: The evdi kernel module is either not loaded or failed to initialize"sv;
      BOOST_LOG(error) << "EVDI: Install evdi-dkms package (v1.14.11) and run: sudo modprobe evdi"sv;
      BOOST_LOG(debug) << "EVDI: After loading, verify with: ls -la /sys/devices/evdi/"sv;
      BOOST_LOG(debug) << "EVDI: Check kernel logs with: dmesg | grep evdi"sv;
      return false;
    }
    
    BOOST_LOG(debug) << "EVDI: Kernel module loaded, searching for available EVDI device nodes..."sv;

    // Iterate through device nodes to find an EVDI device using evdi_check_device()
    // As per EVDI documentation: "In order to distinguish non-EVDI nodes from a node 
    // that's created by EVDI kernel module, evdi_check_device function should be used."
    // We scan /dev/dri/card* devices to find EVDI virtual displays
    int found_device_index = -1;
    for (int i = 0; i < 16; i++) {  // Check card0 through card15
      evdi_device_status status = evdi_check_device(i);
      
      if (status == AVAILABLE) {
        BOOST_LOG(debug) << "EVDI: Found available EVDI device at index "sv << i;
        found_device_index = i;
        break;
      }
      else if (status == UNRECOGNIZED) {
        // Not an EVDI device, continue searching
        continue;
      }
      else if (status == NOT_PRESENT) {
        // Device node doesn't exist
        continue;
      }
    }
    
    if (found_device_index < 0) {
      BOOST_LOG(error) << "EVDI: No available EVDI device found"sv;
      BOOST_LOG(error) << "EVDI: The EVDI kernel module may not have created any device nodes"sv;
      BOOST_LOG(error) << "EVDI: Ensure evdi-dkms is properly installed and the kernel module is loaded"sv;
      BOOST_LOG(info) << "EVDI: Try: sudo modprobe evdi"sv;
      BOOST_LOG(debug) << "EVDI: Check device nodes: ls -la /dev/dri/card*"sv;
      BOOST_LOG(debug) << "EVDI: Check kernel logs: dmesg | grep evdi"sv;
      return false;
    }
    
    BOOST_LOG(info) << "EVDI: Using EVDI device at index "sv << found_device_index;
    
    // Open the EVDI device
    evdi_handle handle = EVDI_INVALID_HANDLE;
    try {
      handle = evdi_open(found_device_index);
      BOOST_LOG(debug) << "EVDI: evdi_open("sv << found_device_index << ") returned handle="sv << (void*)handle;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "EVDI: Exception in evdi_open(): "sv << e.what();
      BOOST_LOG(error) << "EVDI: This indicates a problem with the EVDI library or kernel module"sv;
      return false;
    }
    catch (...) {
      BOOST_LOG(error) << "EVDI: Unknown exception in evdi_open()"sv;
      BOOST_LOG(error) << "EVDI: This indicates a serious problem with the EVDI library or kernel module"sv;
      return false;
    }
    
    evdi_state.handle = handle;
    
    if (evdi_state.handle == EVDI_INVALID_HANDLE) {
      BOOST_LOG(error) << "EVDI: Failed to open EVDI device at index "sv << found_device_index;
      BOOST_LOG(error) << "EVDI: evdi_open() returned EVDI_INVALID_HANDLE"sv;
      BOOST_LOG(debug) << "EVDI: Check device permissions: ls -la /dev/dri/card"sv << found_device_index;
      BOOST_LOG(debug) << "EVDI: Check kernel logs: dmesg | grep evdi"sv;
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
    try {
      evdi_connect(evdi_state.handle, edid.data(), edid.size(), 0);
      BOOST_LOG(debug) << "EVDI: evdi_connect() completed successfully"sv;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "EVDI: Exception in evdi_connect(): "sv << e.what();
      evdi_close(evdi_state.handle);
      evdi_state.handle = EVDI_INVALID_HANDLE;
      return false;
    }
    catch (...) {
      BOOST_LOG(error) << "EVDI: Unknown exception in evdi_connect()"sv;
      evdi_close(evdi_state.handle);
      evdi_state.handle = EVDI_INVALID_HANDLE;
      return false;
    }

    // Mark as active before waiting for KMS detection
    evdi_state.is_active = true;

    BOOST_LOG(info) << "EVDI: Virtual display configured successfully"sv;
    BOOST_LOG(debug) << "EVDI: Display state - width="sv << evdi_state.width 
                     << ", height="sv << evdi_state.height 
                     << ", refresh_rate="sv << evdi_state.refresh_rate;
    
    // Wait for KMS to detect the newly configured display
    // The kernel DRM subsystem needs time to enumerate the new EVDI connector
    constexpr auto KMS_DETECTION_WAIT_MS = 500;
    BOOST_LOG(debug) << "EVDI: Waiting "sv << KMS_DETECTION_WAIT_MS << "ms for KMS to detect display..."sv;
    std::this_thread::sleep_for(std::chrono::milliseconds(KMS_DETECTION_WAIT_MS));
    BOOST_LOG(debug) << "EVDI: KMS detection wait complete"sv;
    
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
      try {
        evdi_disconnect(evdi_state.handle);
        evdi_close(evdi_state.handle);
        BOOST_LOG(debug) << "EVDI: Device disconnected and closed successfully"sv;
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "EVDI: Exception during cleanup: "sv << e.what();
      }
      catch (...) {
        BOOST_LOG(warning) << "EVDI: Unknown exception during cleanup"sv;
      }
      evdi_state.handle = EVDI_INVALID_HANDLE;
    }

    evdi_state.is_active = false;

    BOOST_LOG(info) << "EVDI: Virtual display destroyed"sv;
  }

  std::shared_ptr<display_t> evdi_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    BOOST_LOG(debug) << "EVDI: evdi_display() called - hwdevice_type="sv << (int)hwdevice_type 
                     << ", display_name='"sv << display_name << "', is_active="sv << evdi_state.is_active;
    
#ifndef SUNSHINE_BUILD_DRM
    BOOST_LOG(error) << "EVDI: EVDI requires KMS/DRM support to be enabled"sv;
    return nullptr;
#else
    // EVDI virtual display must be explicitly created via evdi_prepare_stream() before calling this
    // During encoder validation at startup, we don't have a display yet - return nullptr gracefully
    
    if (!evdi_state.is_active) {
      // This is expected during encoder validation - encoder will use default capabilities
      BOOST_LOG(debug) << "EVDI: Virtual display not yet created - call evdi_prepare_stream() before streaming"sv;
      return nullptr;
    }
    
    BOOST_LOG(debug) << "EVDI: Using active virtual display"sv;

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
