# Sunshine - Agent Development Guide

## Repository Overview

**Sunshine** is a self-hosted game stream host for Moonlight clients. It provides low-latency cloud gaming server capabilities with support for AMD, Intel, and Nvidia GPUs for hardware encoding, plus software encoding fallback. The project includes a web UI for configuration and client pairing.

### Key Facts
- **Type**: C++/JavaScript multi-platform application (FreeBSD, Linux, macOS, Windows)
- **Size**: ~60 C++ source files, comprehensive web UI built with Vue.js and Vite
- **Languages**: C++ (23), JavaScript/TypeScript (Vue.js), Python (scripts), CMake
- **Frameworks**: Boost (filesystem, locale, log, program_options), Vue.js 3, Vite 6
- **Test Framework**: Google Test (gtest)
- **Build System**: CMake 3.25+ with Ninja generator
- **CI/CD**: GitHub Actions with multi-platform builds

## Critical Build Requirements

### Prerequisites
**ALWAYS clone with submodules or initialize them before building:**
```bash
git clone https://github.com/lizardbyte/sunshine.git --recurse-submodules
# OR if already cloned:
git submodule update --init --recursive
```

**Compiler Requirements:**
- GCC 13+ (Linux/FreeBSD)
- Clang 17+ or Apple Clang 15+ (macOS)
- MSYS2 UCRT64 GCC (Windows)

**Runtime Requirements:**
- CMake 3.25+
- Ninja build system
- Node.js/npm (for web UI)

### Build Directory Naming Convention
**IMPORTANT**: Build directories MUST be prefixed with `cmake-build-` to be properly ignored by git. Examples:
- `cmake-build-debug`
- `cmake-build-release`
- `cmake-build-test`

## Build Process (Validated Commands)

### 1. Install Node Dependencies (REQUIRED FIRST)
```bash
npm install
```
**Timing**: ~10-15 seconds  
**Note**: This installs 170+ packages. A moderate security vulnerability warning is expected and can be ignored.

### 2. Build Web UI (REQUIRED BEFORE CMAKE)
```bash
npm run build
```
**Timing**: ~5-6 seconds  
**Output**: Builds to `build/assets/web/`  
**Note**: Codecov plugin errors about pre-signed URLs are expected and harmless.

**Alternative during development:**
```bash
npm run dev  # Watch mode for web UI changes
```

### 3. Configure with CMake
```bash
cmake -B cmake-build-debug -G Ninja -S .
```
**Timing**: 5-8 minutes (first run - downloads Boost ~780MB)  
**Subsequent runs**: ~30 seconds  

**Common CMake Options:**
```bash
# Disable tests to speed up builds
cmake -B cmake-build-debug -G Ninja -S . -DBUILD_TESTS=OFF

# Disable docs
cmake -B cmake-build-debug -G Ninja -S . -DBUILD_DOCS=OFF

# Release build (default is Release if not specified)
cmake -B cmake-build-debug -G Ninja -S . -DCMAKE_BUILD_TYPE=Release
```

See `cmake/prep/options.cmake` for all available options.

### 4. Build with Ninja
```bash
ninja -C cmake-build-debug
```
**Timing**: 10-20 minutes (first full build), 1-5 minutes (incremental)  
**Binary Output**: `cmake-build-debug/sunshine` (Linux/macOS) or `cmake-build-debug/sunshine.exe` (Windows)

### 5. Run Tests
```bash
# From build/tests directory
./cmake-build-debug/tests/test_sunshine

# With test options
./cmake-build-debug/tests/test_sunshine --gtest_color=yes
./cmake-build-debug/tests/test_sunshine --gtest_filter="TestName.*"
./cmake-build-debug/tests/test_sunshine --help  # See all options
```

**Note**: Some tests require X11 display. CI uses Xvfb:
```bash
export DISPLAY=:1
Xvfb ${DISPLAY} -screen 0 1024x768x24 &
sleep 5  # Give Xvfb time to start
./cmake-build-debug/tests/test_sunshine --gtest_color=yes
```

The test executable is always named `test_sunshine` and located in `cmake-build-*/tests/`.

## Code Quality & Linting

### C++ Code Formatting (CRITICAL)
**ALWAYS run clang-format before committing C++ code changes.**

The project uses `.clang-format` (LLVM-based style with custom settings). All C++ code MUST conform.

**Option 1 - Manual:**
```bash
find ./ -iname *.cpp -o -iname *.h -iname *.m -iname *.mm | xargs clang-format -i
```

**Option 2 - Python script (modifies files):**
```bash
python ./scripts/update_clang_format.py
```

### Python Code (Scripts)
```bash
# Linting with flake8 (config in .flake8)
flake8 scripts/
```

### Documentation Linting
```bash
# For .rst files (config in .rstcheck.cfg)
rstcheck docs/
```

## Project Structure

### Source Code Layout
```
src/                          # Main C++ source
├── main.cpp                  # Application entry point
├── platform/                 # Platform-specific code
│   ├── linux/                # Linux implementation
│   ├── windows/              # Windows implementation
│   ├── macos/                # macOS implementation
│   └── common.h              # Platform abstractions
├── audio.cpp/h               # Audio capture
├── video.cpp/h               # Video encoding
├── input.cpp/h               # Input handling
├── network.cpp/h             # Network stack
├── nvenc/                    # NVIDIA encoder
├── config.cpp/h              # Configuration
├── confighttp.cpp/h          # Web config API
└── [other core modules]

src_assets/                   # Platform-specific assets
├── common/assets/web/        # Web UI source (Vue.js)
│   ├── public/assets/locale/ # Translations (Vue I18n)
│   └── *.html                # HTML pages (with EJS templates)
├── linux/                    # Linux assets
├── windows/                  # Windows assets
└── macos/                    # macOS assets

tests/                        # Google Test suite
├── tests_main.cpp            # Test runner
├── unit/                     # Unit tests
│   ├── test_*.cpp            # Individual test files
│   └── platform/             # Platform-specific tests
└── integration/              # Integration tests

cmake/                        # CMake modules
├── prep/                     # Build preparation
│   ├── options.cmake         # Build options
│   ├── build_version.cmake   # Version detection
│   └── constants.cmake       # Project constants
├── dependencies/             # Dependency management
├── compile_definitions/      # Compile flags
├── targets/                  # Build targets
├── packaging/                # Package generation
└── Find*.cmake               # Custom find modules

third-party/                  # Git submodules (MUST be initialized)
├── googletest/               # Test framework
├── moonlight-common-c/       # Moonlight protocol
├── boost/ (fetched)          # Downloaded by CMake if not found
└── [18 other submodules]
```

### Configuration Files
- `.clang-format` - C++ code style (MUST follow)
- `.flake8` - Python linting
- `.prettierrc.json` - JavaScript formatting
- `.rstcheck.cfg` - RST documentation linting
- `package.json` - NPM scripts and dependencies
- `vite.config.js` - Web UI build configuration
- `CMakeLists.txt` - Root CMake configuration

## CI/CD Workflows

### Main CI Pipeline (`.github/workflows/ci.yml`)
Triggered on: PRs to master, pushes to master, manual workflow dispatch

**Build Jobs:**
1. `build-linux` - Ubuntu 22.04 AppImage build
2. `build-windows` - Windows AMD64 (MSYS2 UCRT64)
3. `build-freebsd` - FreeBSD 14.3 (amd64, aarch64)
4. `build-homebrew` - macOS (14, 15, 26) + Ubuntu
5. `build-archlinux` - Arch Linux package
6. `build-flatpak` - Linux Flatpak
7. `build-docker` - Multi-platform Docker images

**Coverage Jobs:** Codecov uploads from FreeBSD, Linux, Archlinux, Homebrew, Windows

### Important Workflows
- `_common-lint.yml` - General linting (runs on PRs)
- `localize.yml` - Translation updates (CrowdIn integration)
- `_codeql.yml` - Security analysis

### Build Script Reference
**Linux**: `scripts/linux_build.sh` - Comprehensive build script with steps:
- `deps` - Install dependencies
- `cmake` - Configure
- `validation` - Validate setup
- `build` - Compile
- `package` - Create DEB/RPM
- `cleanup` - Cleanup alternatives
- `all` - Run all steps (default)

Usage in CI:
```bash
./scripts/linux_build.sh --skip-cleanup --skip-package --ubuntu-test-repo
```

## Common Issues & Workarounds

### Issue: Missing Submodules
**Symptom**: CMake error about missing `third-party/moonlight-common-c/enet`  
**Solution**: Run `git submodule update --init --recursive`

### Issue: Boost Download Timeout
**Symptom**: CMake hangs or times out downloading Boost  
**Solution**: CMake downloads Boost 1.89.0 (~780MB) on first run. Increase timeout to 10+ minutes. Alternatively, install system Boost 1.89+.

### Issue: npm Build Fails
**Symptom**: Vite build errors or missing node_modules  
**Solution**: ALWAYS run `npm install` before `npm run build`

### Issue: Test Binary Not Found
**Symptom**: Cannot find test executable  
**Solution**: Test binary is at `cmake-build-*/tests/test_sunshine` (prefix depends on your build directory name)

### Issue: Tests Fail with Display Errors (Linux)
**Symptom**: Tests fail with X11/display errors  
**Solution**: Start Xvfb: `Xvfb :1 -screen 0 1024x768x24 &` and `export DISPLAY=:1`

### Issue: Build Directory Committed to Git
**Symptom**: Build artifacts appear in git status  
**Solution**: Build directories MUST start with `cmake-build-` (e.g., `cmake-build-debug`). This is in `.gitignore`. Use `git rm -r --cached <dir>` to remove if committed.

## Testing Strategy

### Unit Tests
Located in `tests/unit/`, organized by module (e.g., `test_audio.cpp`, `test_video.cpp`).

### Integration Tests  
Located in `tests/integration/`, test cross-module interactions.

### Platform-Specific Tests
Located in `tests/unit/platform/`, contains OS-specific test implementations.

### Coverage
- **Tool**: gcovr with gcc coverage flags (`-fprofile-arcs -ftest-coverage`)
- **Upload**: Codecov via GitHub Actions
- **Exclusions**: Use `GCOVR_EXCL_*` markers for non-coverable code (e.g., GPU-only code)

### Running Specific Tests
```bash
# Run only tests matching pattern
./cmake-build-debug/tests/test_sunshine --gtest_filter="AudioTest.*"

# List all tests
./cmake-build-debug/tests/test_sunshine --gtest_list_tests

# Verbose output
./cmake-build-debug/tests/test_sunshine --gtest_color=yes --verbose
```

## Localization

### Web UI (Vue I18n)
- **Source**: `src_assets/common/assets/web/public/assets/locale/en.json`
- **ONLY modify `en.json`** - other languages managed by CrowdIn
- **Keys must be sorted alphabetically** (use https://novicelab.org/jsonabc)
- **Integration**: Automatic via `.github/workflows/localize.yml`

### C++ (Boost.Locale)
```cpp
#include <boost/locale.hpp>
std::string msg = boost::locale::translate("Hello world!");
```
- Extraction: `python ./scripts/_locale.py --extract --init --update`
- Compilation: `python ./scripts/_locale.py --compile`
- **DO NOT commit** `.pot` or `.mo` files (managed by CI)

## Key Dependencies

### Required Libraries (Linux example)
- `libdrm-dev` - Display/DRM support
- `libwayland-dev` - Wayland protocol
- `libx11-xcb-dev` - X11 support
- `libxcb-dri3-dev` - XCB extensions
- `libxfixes-dev` - X11 fixes
- `libgl-dev` - OpenGL
- `libva` - VA-API (often needs latest from source)
- `libopus` - Audio codec
- `libssl` - OpenSSL
- `miniupnpc` - UPnP support

### Windows Dependencies (MSYS2 UCRT64)
All packages prefixed with `mingw-w64-ucrt-x86_64-`:
- `boost`, `cmake`, `cppwinrt`, `curl-winssl`, `gcc`, `MinHook`, `miniupnpc`, `nodejs`, `nsis`, `onevpl`, `openssl`, `opus`, `toolchain`

### macOS Dependencies (Homebrew)
- `boost`, `cmake`, `miniupnpc`, `ninja`, `node`, `openssl@3`, `opus`, `pkg-config`

## Platform-Specific Notes

### Windows (MSYS2)
- **Shell**: Always use MSYS2 UCRT64 shell
- **Prefix commands**: When running in CI, commands are prefixed with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`
- **Build directory**: Still use `cmake-build-*` prefix

### Linux
- **CUDA Support**: Optional but recommended for NVENC. Requires CUDA Toolkit ~12.9
- **libva**: CI builds latest from source for best compatibility
- **AppImage**: Use `--appimage-build` flag with `linux_build.sh`

### macOS
- **Homebrew vs MacPorts**: Both supported, Homebrew recommended
- **OpenSSL linking**: May need symlink: `ln -s /opt/homebrew/opt/openssl/include/openssl /opt/homebrew/include/openssl`

### FreeBSD
- **Status**: Experimental support
- **Package manager**: pkg

## Quick Reference

### First Time Setup
```bash
git clone https://github.com/lizardbyte/sunshine.git --recurse-submodules
cd sunshine
npm install
npm run build
cmake -B cmake-build-debug -G Ninja -S .
ninja -C cmake-build-debug
```

### Typical Development Cycle
```bash
# Make code changes
# Format C++ code
python ./scripts/update_clang_format.py

# Rebuild
ninja -C cmake-build-debug

# Run tests
./cmake-build-debug/tests/test_sunshine

# If web UI changed
npm run build
ninja -C cmake-build-debug
```

### Pre-commit Checklist
1. ✅ Run clang-format on C++ changes
2. ✅ Run tests: `./cmake-build-debug/tests/test_sunshine`
3. ✅ Build succeeds without warnings
4. ✅ For web UI: `npm run build` succeeds
5. ✅ For translations: Only modify `en.json`, keep keys sorted

## Trust These Instructions

These instructions have been validated by running actual build commands and reviewing CI workflows. When the information here differs from what you might infer from searching the codebase, **trust these instructions** - they represent the tested and working approach.

Only search for additional information if:
- These instructions are incomplete for your specific task
- You encounter an error not covered in "Common Issues"
- You need to understand implementation details not related to building/testing

The goal is to minimize exploration time and maximize development efficiency.
