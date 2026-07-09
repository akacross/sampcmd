# CMake toolchain file for cross-compiling a 32-bit Windows PE from Linux using
# the MinGW-w64 i686 toolchain.
#
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i686.cmake
#
# 32-bit is mandatory: gta_sa.exe and samp.dll are PE32/i386, and a 64-bit
# launcher cannot CreateRemoteThread + LoadLibraryW into a 32-bit process.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(TRIPLE i686-w64-mingw32)

set(CMAKE_C_COMPILER   ${TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${TRIPLE}-g++)
set(CMAKE_RC_COMPILER  ${TRIPLE}-windres)

# Some distros ship the tools as i686-w64-mingw32-gcc-posix; allow an override.
if(DEFINED ENV{MINGW_TRIPLE})
  set(TRIPLE $ENV{MINGW_TRIPLE})
  set(CMAKE_C_COMPILER   ${TRIPLE}-gcc)
  set(CMAKE_CXX_COMPILER ${TRIPLE}-g++)
  set(CMAKE_RC_COMPILER  ${TRIPLE}-windres)
endif()

set(CMAKE_FIND_ROOT_PATH /usr/${TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
