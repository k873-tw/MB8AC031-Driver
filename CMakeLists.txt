# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(mb8cfw VERSION 0.1.0 LANGUAGES C)

option(MB8_BUILD_TOOLS "mb8cfw-rec / mb8cfw-ctl (Linux, macOS, Windows)" ON)
option(MB8_BUILD_IFD "PC/SC IFD handler (needs pcsc-lite headers on Linux)" ON)
option(MB8_BUILD_BONDRIVER "BonDriver DLL (Windows only)" ${WIN32})
option(MB8_BUILD_TESTS "unit tests (no device needed)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Static CRT everywhere, so the DLL and the .exe files need no VC++ runtime
# and nothing mixes /MT and /MD.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()
if(NOT WIN32)
  add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()

# --- libusb-1.0 ----------------------------------------------------------
# Windows: point LIBUSB_ROOT at the unpacked libusb release (include/ and
# the VS20xx/MinGW64 static library), see docs/windows.md.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT WIN32)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  set(MB8_LIBUSB PkgConfig::LIBUSB)
else()
  find_path(LIBUSB_INCLUDE_DIR libusb.h
            HINTS ${LIBUSB_ROOT}/include ${LIBUSB_ROOT}/include/libusb-1.0
            PATH_SUFFIXES libusb-1.0)
  find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0
               HINTS ${LIBUSB_ROOT}/MinGW64/static ${LIBUSB_ROOT}/VS2022/MS64/static
                     ${LIBUSB_ROOT}/MinGW32/static ${LIBUSB_ROOT}/VS2022/MS32/static
                     ${LIBUSB_ROOT}/lib)
  if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
    message(FATAL_ERROR "libusb-1.0 not found: set LIBUSB_ROOT")
  endif()
  add_library(mb8_libusb INTERFACE)
  target_include_directories(mb8_libusb INTERFACE ${LIBUSB_INCLUDE_DIR})
  target_link_libraries(mb8_libusb INTERFACE ${LIBUSB_LIBRARY})
  set(MB8_LIBUSB mb8_libusb)
endif()

find_package(Threads REQUIRED)

# --- the library ---------------------------------------------------------
add_library(mb8cfw STATIC
  src/lib/plat.c
  src/lib/usbio.c
  src/lib/dev.c
  src/lib/fe_m2tv.c
  src/lib/fe_dt295.c
  src/lib/stream.c
  src/lib/install.c
  src/lib/card.c)
target_include_directories(mb8cfw PUBLIC include PRIVATE src/lib)
target_link_libraries(mb8cfw PUBLIC ${MB8_LIBUSB} Threads::Threads)
set_target_properties(mb8cfw PROPERTIES C_VISIBILITY_PRESET hidden)

# --- command-line tools (all platforms) ---------------------------------
if(MB8_BUILD_TOOLS)
  foreach(t mb8cfw-rec mb8cfw-ctl)
    add_executable(${t} src/tools/${t}.c)
    target_link_libraries(${t} mb8cfw)
    if(MINGW)
      target_link_options(${t} PRIVATE -static -static-libgcc)
    endif()
  endforeach()
  install(TARGETS mb8cfw-rec mb8cfw-ctl RUNTIME DESTINATION bin)
endif()
if(NOT WIN32)
  install(FILES src/linux/udev/60-mb8cfw.rules
          DESTINATION lib/udev/rules.d)
endif()

# --- PC/SC IFD handler ---------------------------------------------------
if(MB8_BUILD_IFD AND NOT WIN32)
  include(cmake/ifd.cmake)
endif()

# --- BonDriver -----------------------------------------------------------
if(MB8_BUILD_BONDRIVER AND WIN32)
  enable_language(CXX)
  add_library(BonDriver_MB8CFW SHARED
    src/windows/bondriver/BonDriver_MB8CFW.cpp
    src/windows/bondriver/BonDriver_MB8CFW.def)
  target_link_libraries(BonDriver_MB8CFW mb8cfw)
  target_compile_definitions(BonDriver_MB8CFW PRIVATE UNICODE _UNICODE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # "const BOOL" returns are how the interface is spelled; harmless.
    target_compile_options(BonDriver_MB8CFW PRIVATE -Wno-ignored-qualifiers)
  endif()
  set_target_properties(BonDriver_MB8CFW PROPERTIES PREFIX "")
  if(MINGW)
    target_link_options(BonDriver_MB8CFW PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endif()

# --- tests ---------------------------------------------------------------
if(MB8_BUILD_TESTS)
  enable_testing()
  foreach(t test_channel test_fe_m2tv test_fe_dt295 test_stream test_frame test_presence)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} mb8cfw)
    target_include_directories(${t} PRIVATE src/lib)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
  add_executable(test_t1 tests/test_t1.c)
  target_include_directories(test_t1 PRIVATE src/lib)
  add_test(NAME test_t1 COMMAND test_t1)
endif()
