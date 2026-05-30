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
    protocol/RDPPeer.c
    display/VirtualDisplay.m
    display/ScreenCapture.m
    display/FrameEncoder.m
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
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AppKit"
    "-framework ApplicationServices"
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

target_compile_definitions(macos-rdp-daemon PRIVATE
    MACOS_RDP_VERSION="0.1.5"
)

# ── Install ───────────────────────────────────────────────────────────────
install(TARGETS macos-rdp-daemon DESTINATION /usr/local/sbin)
install(FILES launchd/com.macosrdp.daemon.plist
        DESTINATION /Library/LaunchDaemons)
