cmake_minimum_required(VERSION 3.16)
project(imessage_exporter LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

# --- Core library: zero external dependencies, fully unit-testable ----------
add_library(imsg_core STATIC
  src/models.cpp
  src/time_util.cpp
  src/attributed_body.cpp
  src/exporters.cpp
)
target_include_directories(imsg_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# --- Tests (no SQLite needed) ----------------------------------------------
enable_testing()
add_executable(imsg_tests tests/test_core.cpp)
target_link_libraries(imsg_tests PRIVATE imsg_core)
add_test(NAME core COMMAND imsg_tests)

# --- CLI binary: needs SQLite3 (ships with macOS; libsqlite3-dev on Linux) --
find_package(SQLite3)
if(SQLite3_FOUND)
  add_executable(imessage-exporter src/main.cpp src/database.cpp)
  target_link_libraries(imessage-exporter PRIVATE imsg_core SQLite::SQLite3)
  install(TARGETS imessage-exporter RUNTIME DESTINATION bin)
else()
  message(WARNING
    "SQLite3 not found: building the core library and tests only. "
    "Install libsqlite3-dev (Linux) to build the 'imessage-exporter' binary. "
    "On macOS SQLite3 ships with the system and is found automatically.")
endif()
