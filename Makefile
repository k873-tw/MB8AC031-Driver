# SPDX-License-Identifier: MIT
#
# GNU make build, an alternative to CMake.  See README.md.
#
#   make                  library, mb8cfw-rec, mb8cfw-ctl, IFD handler (if pcsc-lite is found)
#   make check            unit tests (no device needed)
#   sudo make install     PREFIX=/usr/local by default, DESTDIR is honoured
#   make clean
#
# Cross-build for Windows with MinGW-w64 (mb8cfw-rec.exe, mb8cfw-ctl.exe,
# BonDriver_MB8CFW.dll):
#   make CROSS=x86_64-w64-mingw32- LIBUSB_ROOT=/path/to/libusb-1.0.xx

VERSION   := 0.1.0
BUILD     ?= build-make
PREFIX    ?= /usr/local
CROSS     ?=
CC        := $(CROSS)gcc
CXX       := $(CROSS)g++
AR        := $(CROSS)ar
PKG_CONFIG ?= pkg-config

CFLAGS    ?= -O2 -g
CXXFLAGS  ?= -O2 -g
WARN      := -Wall -Wextra -Wno-unused-parameter
CPPFLAGS  += -Iinclude -Isrc/lib

LIB_SRC := plat.c usbio.c dev.c fe_m2tv.c fe_dt295.c stream.c install.c card.c
TESTS   := test_channel test_fe_m2tv test_fe_dt295 test_stream test_frame \
           test_presence test_t1

ifeq ($(CROSS),)
  # --- native (Linux, macOS) -------------------------------------------
  OBJ      := $(BUILD)/obj
  CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
  USB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
  USB_LIBS   := $(shell $(PKG_CONFIG) --libs libusb-1.0)
  ifeq ($(USB_LIBS),)
    $(error libusb-1.0 not found by pkg-config)
  endif
  LDLIBS   := $(USB_LIBS) -lpthread
  PCSC_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpcsclite 2>/dev/null)
  HAVE_PCSC   := $(shell $(PKG_CONFIG) --exists libpcsclite && echo 1)
  USBDROPDIR  := $(shell $(PKG_CONFIG) --variable=usbdropdir libpcsclite 2>/dev/null)
  ifeq ($(USBDROPDIR),)
    USBDROPDIR := $(PREFIX)/lib/pcsc/drivers
  endif
  EXE :=
else
  # --- MinGW-w64 cross build -------------------------------------------
  OBJ      := $(BUILD)/obj-win
  ifeq ($(LIBUSB_ROOT),)
    $(error set LIBUSB_ROOT to the unpacked libusb Windows release)
  endif
  USB_CFLAGS := -I$(LIBUSB_ROOT)/include -I$(LIBUSB_ROOT)/include/libusb-1.0
  USB_LIBS   := $(LIBUSB_ROOT)/MinGW64/static/libusb-1.0.a
  LDLIBS   := $(USB_LIBS)
  TOOL_LDFLAGS := -static -static-libgcc
  EXE := .exe
endif

LIB      := $(OBJ)/libmb8cfw.a
LIB_OBJ  := $(LIB_SRC:%.c=$(OBJ)/lib/%.o)
IFD_DIR  := $(BUILD)/ifd-mb8cfw.bundle
IFD_SO   := $(IFD_DIR)/Contents/Linux/libifd_mb8cfw.so
IFD_PLIST := $(IFD_DIR)/Contents/Info.plist

TOOLS := $(BUILD)/mb8cfw-rec$(EXE) $(BUILD)/mb8cfw-ctl$(EXE)
BON   := $(BUILD)/BonDriver_MB8CFW.dll

ALL := $(LIB) $(TOOLS)
ifeq ($(CROSS),)
  ifeq ($(HAVE_PCSC),1)
    ALL += $(IFD_SO) $(IFD_PLIST)
  endif
else
  ALL += $(BON) $(BUILD)/BonDriver_MB8CFW.ini
endif

.PHONY: all check test install uninstall clean bondriver
.SECONDARY:
all: $(ALL)

# --- library ----------------------------------------------------------
$(OBJ)/lib/%.o: src/lib/%.c
	@mkdir -p $(@D)
	$(CC) -std=c11 $(WARN) $(CFLAGS) -fPIC -fvisibility=hidden $(CPPFLAGS) \
	    $(USB_CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

# --- command-line tools (all platforms) ------------------------------
$(OBJ)/tools/%.o: src/tools/%.c
	@mkdir -p $(@D)
	$(CC) -std=c11 $(WARN) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/mb8cfw-%$(EXE): $(OBJ)/tools/mb8cfw-%.o $(LIB)
	$(CC) $(TOOL_LDFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# --- PC/SC IFD handler -----------------------------------------------
$(OBJ)/ifd/ifd_mb8cfw.o: src/linux/ifd/ifd_mb8cfw.c
	@mkdir -p $(@D)
	$(CC) -std=c11 $(WARN) $(CFLAGS) -fPIC -fvisibility=hidden -D_GNU_SOURCE \
	    $(CPPFLAGS) $(PCSC_CFLAGS) -MMD -MP -c $< -o $@

$(IFD_SO): $(OBJ)/ifd/ifd_mb8cfw.o $(LIB)
	@mkdir -p $(@D)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(LDLIBS) -ldl

$(IFD_PLIST): src/linux/ifd/Info.plist.in
	@mkdir -p $(@D)
	sed 's/@PROJECT_VERSION@/$(VERSION)/g' $< > $@

# --- BonDriver (MinGW) ------------------------------------------------
bondriver: $(BON) $(BUILD)/BonDriver_MB8CFW.ini

$(BUILD)/BonDriver_MB8CFW.ini: src/windows/bondriver/BonDriver_MB8CFW.ini
	cp $< $@

$(OBJ)/bon/BonDriver_MB8CFW.o: src/windows/bondriver/BonDriver_MB8CFW.cpp
	@mkdir -p $(@D)
	$(CXX) $(WARN) -Wno-ignored-qualifiers $(CXXFLAGS) -DUNICODE -D_UNICODE \
	    $(CPPFLAGS) -MMD -MP -c $< -o $@

$(BON): $(OBJ)/bon/BonDriver_MB8CFW.o src/windows/bondriver/BonDriver_MB8CFW.def $(LIB)
	$(CXX) -shared -static -static-libgcc -static-libstdc++ $(LDFLAGS) -o $@ \
	    $(OBJ)/bon/BonDriver_MB8CFW.o src/windows/bondriver/BonDriver_MB8CFW.def \
	    $(LIB) $(LDLIBS)

# --- tests ------------------------------------------------------------
TEST_BIN := $(TESTS:%=$(BUILD)/tests/%$(EXE))

$(OBJ)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) -std=c11 $(WARN) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/tests/test_t1$(EXE): $(OBJ)/tests/test_t1.o
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/tests/%$(EXE): $(OBJ)/tests/%.o $(LIB)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "  $$t"; $$t; done; \
	    echo "$(words $(TEST_BIN)) tests passed"

# --- install ----------------------------------------------------------
install: all
ifneq ($(CROSS),)
	$(error install is for native builds; copy the files from $(BUILD)/ instead)
endif
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/udev/rules.d
	install -m 755 $(BUILD)/mb8cfw-rec $(BUILD)/mb8cfw-ctl $(DESTDIR)$(PREFIX)/bin/
	install -m 644 src/linux/udev/60-mb8cfw.rules $(DESTDIR)$(PREFIX)/lib/udev/rules.d/
ifeq ($(HAVE_PCSC),1)
	install -d $(DESTDIR)$(USBDROPDIR)/ifd-mb8cfw.bundle/Contents/Linux
	install -m 644 $(IFD_PLIST) $(DESTDIR)$(USBDROPDIR)/ifd-mb8cfw.bundle/Contents/
	install -m 755 $(IFD_SO) $(DESTDIR)$(USBDROPDIR)/ifd-mb8cfw.bundle/Contents/Linux/
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mb8cfw-rec $(DESTDIR)$(PREFIX)/bin/mb8cfw-ctl
	rm -f $(DESTDIR)$(PREFIX)/lib/udev/rules.d/60-mb8cfw.rules
	rm -rf $(DESTDIR)$(USBDROPDIR)/ifd-mb8cfw.bundle

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
