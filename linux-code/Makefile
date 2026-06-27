# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

.PHONY: all lib library tools demos install clean clean-lib clean-tools clean-demos

all: lib tools demos

lib library:
	$(MAKE) -C libsysop64

tools: lib
	$(MAKE) -C tools

demos: lib
	$(MAKE) -C demos


install: tools
	$(MAKE) -C tools install

clean: clean-tools clean-demos clean-lib

clean-lib:
	$(MAKE) -C libsysop64 clean

clean-tools:
	$(MAKE) -C tools clean

clean-demos:
	$(MAKE) -C demos clean
