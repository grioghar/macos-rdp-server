cmake_minimum_required(VERSION 3.22)
project(macos-rdp-daemon LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREERDP REQUIRED freerdp3)
pkg_check_modules(WINPR   REQUIRED winpr3)

add_executable(macos-rdp-daemon
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
    ${FREERDP_INCLUDE_DIRS}
    ${WINPR_INCLUDE_DIRS}
)

target_link_libraries(macos-rdp-daemon PRIVATE
    ${FREERDP_LIBRARIES}
    ${WINPR_LIBRARIES}
    "-framework Foundation"
    "-framework CoreGraphics"
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AppKit"
    "-framework IOKit"
    "-framework IOSurface"
    "-framework Security"
    pam
)

target_compile_options(macos-rdp-daemon PRIVATE
    -fobjc-arc
    -Wall -Wextra -Wno-unused-parameter
)

target_compile_definitions(macos-rdp-daemon PRIVATE
    MACOS_RDP_VERSION="1.0.0"
)

install(TARGETS macos-rdp-daemon DESTINATION /usr/local/sbin)
install(FILES launchd/com.macosrdp.daemon.plist
        DESTINATION /Library/LaunchDaemons)
