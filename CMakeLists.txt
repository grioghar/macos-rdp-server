cmake_minimum_required(VERSION 3.22)
project(macos-rdp-daemon LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# When cross-compiling in CI the arch is passed explicitly; otherwise default
# to the host arch so local builds stay fast (no universal overhead).
if(NOT CMAKE_OSX_ARCHITECTURES)
    execute_process(COMMAND uname -m OUTPUT_VARIABLE HOST_ARCH
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_OSX_ARCHITECTURES "${HOST_ARCH}")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")

# ── Build version stamping ────────────────────────────────────────────────
# Stamp the binary with the short git SHA so the auto-updater can compare the
# running version against the latest published release (whose tag == this SHA).
# Falls back to a UTC timestamp, then "dev", when git is unavailable.
# CI may override with -DMACOS_RDP_BUILD_VERSION=<sha> to guarantee the stamp
# matches the release tag exactly.
if(NOT MACOS_RDP_BUILD_VERSION)
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE MACOS_RDP_BUILD_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT MACOS_RDP_BUILD_VERSION)
    string(TIMESTAMP MACOS_RDP_BUILD_VERSION "dev-%Y%m%d%H%M%S" UTC)
endif()
message(STATUS "macos-rdp-daemon build version: ${MACOS_RDP_BUILD_VERSION}")

# ── Static vs dynamic FreeRDP ─────────────────────────────────────────────
option(MACOS_RDP_STATIC
       "Link FreeRDP/WinPR as static libraries (used in CI release builds)"
       OFF)

if(MACOS_RDP_STATIC)
    # CI passes -DFREERDP_STATIC_INSTALL=<path> pointing at the cmake-installed
    # FreeRDP tree built with BUILD_SHARED_LIBS=OFF.
    if(NOT FREERDP_STATIC_INSTALL)
        message(FATAL_ERROR "MACOS_RDP_STATIC=ON requires -DFREERDP_STATIC_INSTALL=<path>")
    endif()

    set(FREERDP_ROOT "${FREERDP_STATIC_INSTALL}")
    set(FREERDP_INCLUDE "${FREERDP_ROOT}/include/freerdp3")
    set(WINPR_INCLUDE   "${FREERDP_ROOT}/include/winpr3")

    # Collect all static libs from the install tree.
    file(GLOB_RECURSE FREERDP_STATIC_LIBS
         "${FREERDP_ROOT}/lib/*.a"
         "${FREERDP_ROOT}/lib64/*.a")

    # OpenSSL static libs must come after FreeRDP on the link line.
    if(OPENSSL_ROOT_DIR)
        file(GLOB OPENSSL_STATIC_LIBS
             "${OPENSSL_ROOT_DIR}/lib/libssl.a"
             "${OPENSSL_ROOT_DIR}/lib/libcrypto.a")
    endif()

    set(FREERDP_LINK_LIBS
        ${FREERDP_STATIC_LIBS}
        ${OPENSSL_STATIC_LIBS}
        z   # zlib — always present on macOS
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FREERDP REQUIRED freerdp3)
    pkg_check_modules(WINPR   REQUIRED winpr3)
    set(FREERDP_INCLUDE "${FREERDP_INCLUDE_DIRS}")
    set(WINPR_INCLUDE   "${WINPR_INCLUDE_DIRS}")
    set(FREERDP_LINK_LIBS ${FREERDP_LIBRARIES} ${WINPR_LIBRARIES})
endif()

# ── Target ────────────────────────────────────────────────────────────────
add_executable(macos-rdp-daemon
    logging/RDPLog.c
    daemon/main.m
    daemon/RDPServer.m
    daemon/RDPSession.m
    daemon/Authenticator.m
    daemon/DisplayControl.m
    daemon/AutoUpdate.m
    protocol/RDPPeer.c
    display/VirtualDisplay.m
    display/ScreenCapture.m
    display/FrameEncoder.m
    display/CursorCapture.m
    input/InputInjector.m
    input/ClipboardSync.m
    audio/AudioCapture.m
    audio/AudioRedirect.c
)

target_include_directories(macos-rdp-daemon PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${FREERDP_INCLUDE}
    ${WINPR_INCLUDE}
    # Static build: FreeRDP installs headers under include/freerdp3 and include/winpr3
    $<$<BOOL:${MACOS_RDP_STATIC}>:${FREERDP_STATIC_INSTALL}/include>
)

target_link_libraries(macos-rdp-daemon PRIVATE
    ${FREERDP_LINK_LIBS}
    "-framework Foundation"
    "-framework CoreGraphics"
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework ScreenCaptureKit"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AppKit"
    "-framework ApplicationServices"
    "-framework OpenDirectory"
    "-framework IOKit"
    "-framework IOSurface"
    "-framework Security"
    pam
)

target_compile_options(macos-rdp-daemon PRIVATE
    -fobjc-arc
    -Os
    -Wall -Wextra -Wno-unused-parameter
)

# Link-time optimisation — dead-strips unused code across translation units.
set_property(TARGET macos-rdp-daemon PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

# MACOS_RDP_VIRTUAL_DISPLAY: create a CGVirtualDisplay sized to the RDP client
# (no pillarbox / 1:1 input). Falls back to the main display if the private API fails.
target_compile_definitions(macos-rdp-daemon PRIVATE
    MACOS_RDP_VERSION="0.1.6"
    MACOS_RDP_BUILD_VERSION="${MACOS_RDP_BUILD_VERSION}"
    MACOS_RDP_VIRTUAL_DISPLAY=1
)

# ── Install ───────────────────────────────────────────────────────────────
install(TARGETS macos-rdp-daemon DESTINATION /usr/local/sbin)
install(FILES launchd/com.macosrdp.daemon.plist
        DESTINATION /Library/LaunchDaemons)
