# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

CC  ?= gcc
CXX ?= g++
RM  ?= rm -f

BUILD_DIR ?= ../build/demos
LIBSYSOP64 = ../build/lib/libsysop64.a

CFLAGS   ?=
CXXFLAGS ?= -std=c++11
CPPFLAGS += -I../libsysop64/include -I.
LIBS ?= -lm -L../build/lib -Wl,-Bstatic -lsysop64 -Wl,-Bdynamic
LDFLAGS ?= -s

DEMOS_SRCS = hdmi_colors.c bmlogo2.c digiplay.c
DEMOS = $(DEMOS_SRCS:.c=)
DEMO_OUTPUTS = $(addprefix $(BUILD_DIR)/, $(DEMOS))
DEMO_PROGRAM_OUTPUTS = $(DEMO_OUTPUTS) $(addprefix $(BUILD_DIR)/, sidplayer sidmidi sid_visualizer anim_demo)

SIDPLAYER_LIBS = $(LIBS) -lstdc++ -lm
SIDMIDI_LIBS   = $(LIBS) -lasound
SID_VIS_CXXFLAGS = $(CXXFLAGS) $(shell pkg-config --cflags cairo)
SID_VIS_LIBS     = $(LIBS) -lcairo -lpthread -lm
ANIM_DEMO_LIBS   = $(LIBS) -lcairo -lm
MOUSE_POINTER_OBJ = $(BUILD_DIR)/mouse_pointer.o
ASSETS_DIR = assets

DEMO_ASSETS = $(ASSETS_DIR)/bmlogo2.bin $(ASSETS_DIR)/blood_mosher.raw
FRAMES_ARCHIVE = $(ASSETS_DIR)/frames.tar.gz
ASSETS_STAMP = $(BUILD_DIR)/.assets.stamp

.PHONY: all clean $(DEMOS) sidplayer sidmidi sid_visualizer anim_demo copyassets

all: $(LIBSYSOP64) $(DEMO_PROGRAM_OUTPUTS) copyassets

$(LIBSYSOP64):
	$(MAKE) -C ../libsysop64

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

copyassets: $(ASSETS_STAMP)

$(ASSETS_STAMP): $(DEMO_ASSETS) $(FRAMES_ARCHIVE) | $(BUILD_DIR)
	for asset in $(DEMO_ASSETS); do \
		cp $$asset $(BUILD_DIR)/; \
	done
	tar xfz $(FRAMES_ARCHIVE) -C $(BUILD_DIR) > /dev/null
	touch $@

$(DEMOS): %: $(BUILD_DIR)/%

$(BUILD_DIR)/%: %.c $(LIBSYSOP64) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

sidplayer: $(BUILD_DIR)/sidplayer

$(BUILD_DIR)/sidplayer: sidplayer.c $(LIBSYSOP64) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(SIDPLAYER_LIBS)

sidmidi: $(BUILD_DIR)/sidmidi

$(BUILD_DIR)/sidmidi: sidmidi.c $(LIBSYSOP64) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(SIDMIDI_LIBS)

$(MOUSE_POINTER_OBJ): $(ASSETS_DIR)/mouse_pointer.png | $(BUILD_DIR)
	cd $(ASSETS_DIR) && ld -r -b binary $(notdir $<) -o $(abspath $@)
	objcopy --add-section .note.GNU-stack=/dev/null $@

sid_visualizer: $(BUILD_DIR)/sid_visualizer

$(BUILD_DIR)/sid_visualizer: sid_visualizer.c sidwiz_ui_mapper.c $(MOUSE_POINTER_OBJ) $(LIBSYSOP64) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(SID_VIS_CXXFLAGS) -fpermissive sid_visualizer.c sidwiz_ui_mapper.c $(MOUSE_POINTER_OBJ) -o $@ $(LDFLAGS) $(SID_VIS_LIBS)

anim_demo: $(BUILD_DIR)/anim_demo

$(BUILD_DIR)/anim_demo: anim_demo.c $(LIBSYSOP64) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(SID_VIS_CXXFLAGS) -fpermissive $< -o $@ $(LDFLAGS) $(ANIM_DEMO_LIBS)

clean:
	$(RM) $(DEMO_PROGRAM_OUTPUTS) $(MOUSE_POINTER_OBJ) $(ASSETS_STAMP)
