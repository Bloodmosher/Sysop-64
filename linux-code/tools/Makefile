# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

CC ?= gcc
CXX ?= g++
ASM ?= acme
INSTALL ?= install
RM ?= rm -f

BUILD_DIR ?= ../build/tools
INSTALL_DIR ?= /usr/local/bin
LIBSYSOP64 = ../build/lib/libsysop64.a

CFLAGS ?=
CXXFLAGS ?= -std=c++11
CPPFLAGS += -I../libsysop64/include -I.
LIBS ?= -lm -L../build/lib -Wl,-Bstatic -lsysop64 -Wl,-Bdynamic
# Strip debug symbols by default to keep binaries small.
# Override to include debug info: make LDFLAGS="-g" or make LDFLAGS=""
LDFLAGS ?= -s

SIMPLE_TOOLS_SRCS = bypass_keypress.c cmd.c dmatest.c dmatiming.c ipeek.c iotest.c load.c loadbin.c loadcart.c loadkernal.c peek.c poke.c pokekeybuf.c playsound.c reu.c run.c setcolor.c showmsg.c sys.c sysop_reset_watch.c video_info.c
SIMPLE_TOOLS = $(SIMPLE_TOOLS_SRCS:.c=)
READ_D64_SRCS = read_d64/read_d64.c read_d64/read_d64_image.c read_d64/read_d64_directory.c read_d64/read_d64_dma.c read_d64/read_d64_commands.c read_d64/read_d64_io.c
READ_D64_HDRS = read_d64/read_d64_private.h
READ_D64_ASSET_SRCS = read_d64/diskio.asm read_d64/vectors.asm
READ_D64_ASSET_BINS = diskio.o vectors.o
ASSETS = assets/sysop-msg-1.png assets/sysop-logo-480x196.png assets/sysop-logo-1920x1080.png
SCRIPTS = scripts/sysop_restart scripts/c64init.sh
SYSOP64_SRCS = sysop64/sysop64_state.cpp sysop64/sysop64_c64_keyboard.cpp sysop64/sysop64_input_mapping.cpp sysop64/sysop64_display.cpp sysop64/sysop64_keyboard_monitor.cpp sysop64/sysop64_pty.cpp sysop64/sysop64_terminal_parser.cpp sysop64/sysop64_terminal_render.cpp sysop64/sysop64_framebuffer.cpp sysop64/sysop64_server.cpp sysop64/sysop64_main.cpp
SYSOP64_HDRS = sysop64/sysop64_internal.h
INSTALL_BINARIES = bypass_keypress c64busview cmd dmatest dmatiming easyflash ipeek iotest load loadbin loadcart loadkernal sysop_menu mon mon_io peek pll_config poke pokekeybuf playsound read_d64 reu run screenshot setcolor showmsg showpic sys sysop64 sysop_reset_watch video_info

.PHONY: all install clean mon mon_io easyflash read_d64 screenshot sysop_menu sysop64 $(SIMPLE_TOOLS)

all: $(LIBSYSOP64) $(INSTALL_BINARIES) $(READ_D64_ASSET_BINS)

$(LIBSYSOP64):
	$(MAKE) -C ../libsysop64

install: all
	$(INSTALL) -d $(INSTALL_DIR)
	for bin in $(INSTALL_BINARIES); do \
		$(INSTALL) -m 0755 $(BUILD_DIR)/$$bin $(INSTALL_DIR)/; \
	done
	for asset in $(READ_D64_ASSET_BINS); do \
		$(INSTALL) -m 0644 $(BUILD_DIR)/$$asset $(INSTALL_DIR)/; \
	done
	for asset in $(ASSETS); do \
		$(INSTALL) -m 0644 $$asset $(INSTALL_DIR)/; \
	done
	for script in $(SCRIPTS); do \
		$(INSTALL) -m 0755 $$script $(INSTALL_DIR)/; \
	done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SIMPLE_TOOLS): %: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $(BUILD_DIR)/$@ $(LDFLAGS) $(LIBS)

c64busview: c64busview.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $(BUILD_DIR)/$@ $(LDFLAGS) $(LIBS) -lncurses

read_d64: $(READ_D64_SRCS) $(READ_D64_HDRS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(READ_D64_SRCS) -o $(BUILD_DIR)/$@ $(LDFLAGS) $(LIBS)

diskio.o: read_d64/diskio.asm | $(BUILD_DIR)
	$(ASM) -o $(BUILD_DIR)/$@ $<

vectors.o: read_d64/vectors.asm | $(BUILD_DIR)
	$(ASM) -o $(BUILD_DIR)/$@ $<

easyflash: easyflash.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $(BUILD_DIR)/$@ $(LDFLAGS) $(LIBS)

pll_config: pll_config.c pll_calc.c pll_calc.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) pll_config.c pll_calc.c -o $(BUILD_DIR)/$@ $(LDFLAGS) $(LIBS)

screenshot: screenshot.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $(BUILD_DIR)/$@ $(LDFLAGS) $(LIBS) -lpng

mon_io: mon/mon_io.asm | $(BUILD_DIR)
	$(ASM) -o $(BUILD_DIR)/$@ $^

MON_SRCS = mon/mon.cpp mon/mon_input.cpp mon/mon_breakpoints.cpp mon/mon_cpu.cpp mon/mon_dma.cpp mon/instructions.cpp mon/asm.cpp
MON_HDRS = mon/mon_private.h mon/mon_input.h mon/mon_breakpoints.h mon/mon_cpu.h mon/mon_dma.h mon/instructions.h mon/asm.h

mon: $(MON_SRCS) $(MON_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fpermissive -o $(BUILD_DIR)/$@ $(MON_SRCS) $(LDFLAGS) $(LIBS) -lncurses

SYSOP_MENU_SRCS = sysop_menu/main.cpp sysop_menu/display.cpp sysop_menu/file_browser.cpp \
                  sysop_menu/disk.cpp sysop_menu/keyboard.c sysop_menu/stars.c
SYSOP_MENU_HDRS = sysop_menu/display.h sysop_menu/file_browser.h sysop_menu/stars.h

sysop_menu: $(SYSOP_MENU_SRCS) $(SYSOP_MENU_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -Isysop_menu $(CXXFLAGS) -fpermissive $(SYSOP_MENU_SRCS) \
		-lcairo -lpangocairo-1.0 -lpango-1.0 -lgobject-2.0 -lglib-2.0 -levdev -lm \
		$(LDFLAGS) $(LIBS) -o $(BUILD_DIR)/$@ \
		-I /usr/include/cairo -I /usr/include/libevdev-1.0 \
		-I /usr/include/glib-2.0/include -I /usr/include/glib-2.0 \
		-I /usr/include/pango-1.0 -I /usr/lib/glib-2.0/include \
		-I /usr/lib/arm-linux-gnueabihf/glib-2.0/include -I /usr/include/harfbuzz

showpic: showpic.c | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fpermissive $^ $(shell pkg-config --libs cairo libevdev) -lm $(LDFLAGS) $(LIBS) -o $(BUILD_DIR)/$@ -I/usr/include/cairo -I/usr/include/libevdev-1.0

sysop64: $(SYSOP64_SRCS) $(SYSOP64_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fsanitize=address -g -fpermissive $(SYSOP64_SRCS) -lcairo -lpangocairo-1.0 -lpango-1.0 -lgobject-2.0 -lglib-2.0 -levdev -lm -ludev $(LIBS) -o $(BUILD_DIR)/$@ -I /usr/include/cairo -I /usr/include/libevdev-1.0 -I /usr/include/glib-2.0/include -I /usr/include/glib-2.0 -I /usr/include/pango-1.0 -I /usr/lib/glib-2.0/include -I /usr/lib/arm-linux-gnueabihf/glib-2.0/include -I /usr/include/harfbuzz

clean:
	$(RM) $(addprefix $(BUILD_DIR)/, $(INSTALL_BINARIES))

