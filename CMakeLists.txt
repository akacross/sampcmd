cmake_minimum_required(VERSION 3.20)
project(sampcmd LANGUAGES CXX RC VERSION 2.2.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Enforce a 32-bit Windows target -----------------------------------------
# Injecting into 32-bit gta_sa.exe requires a 32-bit launcher.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
  message(FATAL_ERROR
    "sampcmd targets Windows. Configure with the MinGW toolchain file:\n"
    "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i686.cmake")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "sampcmd must be built 32-bit (pointer size is 8 => 64-bit toolchain).\n"
    "Use the i686 MinGW toolchain (cmake/toolchain-mingw-i686.cmake).")
endif()

add_executable(sampcmd
  src/main.cpp
  src/app.cpp
  src/cli.cpp
  src/logger.cpp
  src/strconv.cpp
  src/win32_helpers.cpp
  src/wine_detection.cpp
  src/diagnostics.cpp
  src/error_formatter.cpp
  src/process_launcher.cpp
  src/injector.cpp
  src/monitor.cpp
  res/sampcmd.rc
)

# GUI subsystem (no console window on a normal launch), Unicode entry point.
set_target_properties(sampcmd PROPERTIES
  WIN32_EXECUTABLE ON
  OUTPUT_NAME "sampcmd"
)

target_compile_definitions(sampcmd PRIVATE UNICODE _UNICODE)
target_compile_options(sampcmd PRIVATE
  -municode
  -Wall -Wextra
  $<$<CONFIG:Release>:-O2>
)

# -municode gives us wWinMain; static-link the runtimes so the .exe has no
# external CRT dependency (important under a bare Wine/Proton prefix).
target_link_options(sampcmd PRIVATE
  -municode
  -static -static-libgcc -static-libstdc++
  -Wl,--subsystem,windows
  $<$<CONFIG:Release>:-s>          # strip debug info from the shipping binary
)

target_link_libraries(sampcmd PRIVATE
  shell32   # CommandLineToArgvW
  user32    # MessageBoxW
  advapi32
  psapi     # GetModuleFileNameEx / GetModuleInformation (monitor)
)

# Ensure windres can find res/sampcmd.manifest (referenced by a relative path).
set_source_files_properties(res/sampcmd.rc PROPERTIES
  COMPILE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/res")
