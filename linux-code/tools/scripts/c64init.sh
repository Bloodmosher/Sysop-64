#!/bin/bash
# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

cmdbin=/usr/local/bin

# Enable IO memory from DE00-DFFF
#$cmdbin/cmd 7

# Load monitor code into IO space
#$cmdbin/loadbin $cmdbin/mon_io df00

# Enable NMI intercept - when enabled the NMI vector is $df00; used by monitor
# $cmdbin/cmd 9

# Take the F2H SDRAM interface out of reset
# Required for things like the frame buffer to work
memtool 0xffc25080=0x3fff

# Reset the C64
$cmdbin/cmd 0

# Allow some time for reset to complete
sleep 2

# set your own color palette entries here
# e.g., this is a less red color 14, the default c64 border color
$cmdbin/setcolor 14 85 83 255

# Configure the video clock - the C64 must be running for this to work
# since it reads the C64 CPU clock speed to make its calculations
$cmdbin/pll_config

# Not 'the right way' to load this module but for now will work
# This driver enables /dev/sysop-fb, a future proof way of opening
# the framebuffer (actually right now it opens all of the upper 512MB).
insmod $cmdbin/sysop-fb.ko

# Enable the sysop restart watch (press both button 2 and 3 then release)
$cmdbin/sysop_reset_watch > /dev/null 2>&1 < /dev/null &

# Enable the Sysop console and DMA broker background process
$cmdbin/sysop64 > /dev/null 2>&1 < /dev/null &


# Configure the video clock - the C64 must be running for this to work
# since it reads the C64 CPU clock speed to make its calculations.
# Run once here at boot time.
#$cmdbin/pll_config

# Use this if you want constant monitoring of the clock (experimental)
# Most C64 frequencies change a bit after running for a while and warming up
# and this should help ensure no sync resets occur.
$cmdbin/pll_config 0 loop 10 onchange > /dev/null 2>&1 < /dev/null &

sleep 1

# Stuff below here depends on sysop64 
$cmdbin/sysop_menu > /dev/null 2>&1 < /dev/null & 

# To start more than one process, use "process &" and then the last line should be "wait"
#$cmdbin/loadkernal /root/JiffyDOS_C64.bin
#$cmdbin/cmd 18
#$cmdbin/cmd 0

# adjust DMA timing if needed
# PAL default currently 28 47
$cmdbin/dmatiming pal 27 47
# auto can do a calibration and set based on those values
#$cmdbin/dmatiming auto

#sleep 20
#$cmdbin/showmsg "Greetings BLOODMOSHER."

wait
