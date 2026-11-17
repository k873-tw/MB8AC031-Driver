# SPDX-License-Identifier: MIT
# The pcsc-lite IFD handler: ifd-mb8cfw.bundle/Contents/{Info.plist,Linux/libifd_mb8cfw.so}
pkg_check_modules(PCSCLITE REQUIRED libpcsclite)
pkg_get_variable(PCSC_USBDROPDIR libpcsclite usbdropdir)
if(NOT PCSC_USBDROPDIR)
  set(PCSC_USBDROPDIR "${CMAKE_INSTALL_PREFIX}/lib/pcsc/drivers")
endif()

add_library(ifd_mb8cfw MODULE src/linux/ifd/ifd_mb8cfw.c)
target_include_directories(ifd_mb8cfw PRIVATE ${PCSCLITE_INCLUDE_DIRS})
target_link_libraries(ifd_mb8cfw PRIVATE mb8cfw ${CMAKE_DL_LIBS})
target_compile_definitions(ifd_mb8cfw PRIVATE _GNU_SOURCE)
set(IFD_BUNDLE ${CMAKE_CURRENT_BINARY_DIR}/ifd-mb8cfw.bundle)
set_target_properties(ifd_mb8cfw PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${IFD_BUNDLE}/Contents/Linux
  C_VISIBILITY_PRESET hidden)
configure_file(src/linux/ifd/Info.plist.in ${IFD_BUNDLE}/Contents/Info.plist @ONLY)

install(DIRECTORY ${IFD_BUNDLE} DESTINATION ${PCSC_USBDROPDIR}
        USE_SOURCE_PERMISSIONS)
