# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

CC  ?= gcc
CXX ?= g++
AR  ?= ar
RM  ?= rm -f

CFLAGS   ?=
CXXFLAGS ?= -std=c++17
CPPFLAGS += -Iinclude -Isrc

BUILD_DIR ?= ../build/lib
LIB = $(BUILD_DIR)/libsysop64.a
SHLIB = $(BUILD_DIR)/libsysop64.so
PIC_BUILD_DIR = $(BUILD_DIR)/pic
CSRCS = \
	src/sysop_state.c \
	src/sysop_locks.c \
	src/sysop_bridge.c \
	src/sysop_dma.c \
	src/sysop_memory.c \
	src/sysop_bus_monitor.c \
	src/sysop_sidcontrol.c \
	src/sysop_video.c \
	src/sysop_midi.c \
	src/sysop_text.c \
	src/sysop_sampler.c \
	src/sysop_tick_sampler.c \
	src/sysop_loader.c \
	src/sysop_server.c \
	src/sysop_input.c \
	src/sysop_audio.c \
	src/sysop_d64.c
CPPSRCS = \
	src/sid_player.cpp \
	src/sysop_sid.cpp
OBJS    = $(CSRCS:.c=.o) $(CPPSRCS:.cpp=.o)
PIC_OBJS = $(CSRCS:src/%.c=$(PIC_BUILD_DIR)/%.o) $(CPPSRCS:src/%.cpp=$(PIC_BUILD_DIR)/%.opp)
HEADERS = include/sysop64.h include/sysop_library.h include/sysop_defines.h \
          include/c64keys.h include/sid_player.h include/sysop_sid.h \
          include/sysop_d64.h \
          src/sysop_internal.h

.PHONY: all tools install install-tools clean clean-tools static shared

all: static shared

static: $(LIB)

shared: $(SHLIB)

tools: all
	$(MAKE) -C ../tools

install: install-tools

install-tools: tools
	$(MAKE) -C ../tools install

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(PIC_BUILD_DIR): | $(BUILD_DIR)
	mkdir -p $(PIC_BUILD_DIR)

$(LIB): $(OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(SHLIB): $(PIC_OBJS) | $(BUILD_DIR)
	$(CXX) -shared -o $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PIC_BUILD_DIR)/%.o: src/%.c $(HEADERS) | $(PIC_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(PIC_BUILD_DIR)/%.opp: src/%.cpp $(HEADERS) | $(PIC_BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -c $< -o $@

clean:
	$(RM) $(OBJS) $(PIC_OBJS) $(LIB) $(SHLIB)

clean-tools:
	$(MAKE) -C ../tools clean
