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

SIDPLAYER_LIBS = $(LIBS) -lstdc++ -lm
SIDMIDI_LIBS   = $(LIBS) -lasound
SID_VIS_CXXFLAGS = $(CXXFLAGS) $(shell pkg-config --cflags cairo)
SID_VIS_LIBS     = $(LIBS) -lcairo -lpthread -lm
ANIM_DEMO_LIBS   = $(LIBS) -lcairo -lm
MOUSE_POINTER_OBJ = $(BUILD_DIR)/mouse_pointer.o
ASSETS_DIR = assets

DEMO_ASSETS = $(ASSETS_DIR)/bmlogo2.bin $(ASSETS_DIR)/blood_mosher.raw
FRAMES_ARCHIVE = $(ASSETS_DIR)/frames.tar.gz

.PHONY: all clean $(DEMOS) sidplayer sidmidi sid_visualizer anim_demo

all: $(LIBSYSOP64) $(DEMOS) sidplayer sidmidi sid_visualizer anim_demo copyassets

$(LIBSYSOP64):
	$(MAKE) -C ../libsysop64

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

copyassets: | $(BUILD_DIR)
	for asset in $(DEMO_ASSETS); do \
		cp $$asset $(BUILD_DIR)/; \
	done
	tar xfz $(FRAMES_ARCHIVE) -C $(BUILD_DIR) > /dev/null

$(DEMOS): %: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $(BUILD_DIR)/$@ $(LDFLAGS) $(LIBS)

sidplayer: sidplayer.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $(BUILD_DIR)/$@ $(LDFLAGS) $(SIDPLAYER_LIBS)

sidmidi: sidmidi.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $(BUILD_DIR)/$@ $(LDFLAGS) $(SIDMIDI_LIBS)

$(MOUSE_POINTER_OBJ): $(ASSETS_DIR)/mouse_pointer.png | $(BUILD_DIR)
	cd $(ASSETS_DIR) && ld -r -b binary $(notdir $<) -o $(abspath $@)
	objcopy --add-section .note.GNU-stack=/dev/null $@

sid_visualizer: sid_visualizer.c sidwiz_ui_mapper.c $(MOUSE_POINTER_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(SID_VIS_CXXFLAGS) -fpermissive $^ -o $(BUILD_DIR)/$@ $(LDFLAGS) $(SID_VIS_LIBS)

anim_demo: anim_demo.c | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(SID_VIS_CXXFLAGS) -fpermissive $< -o $(BUILD_DIR)/$@ $(LDFLAGS) $(ANIM_DEMO_LIBS)

clean:
	$(RM) $(addprefix $(BUILD_DIR)/, $(DEMOS) sidplayer sidmidi sid_visualizer anim_demo) $(MOUSE_POINTER_OBJ)