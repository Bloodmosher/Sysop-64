#!/usr/bin/env python3
# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

"""
MCP Server for Commodore 64 control via HTTP
Supports Model Context Protocol with tools for memory manipulation
"""

import json
import socket
import threading
import struct
import subprocess
import os
import base64
from pathlib import Path
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
import sys
import traceback

# Try to import sysop64 C library bindings (if available). Prefer paths relative
# to this script so build/tools/mcp_server.py can find build/lib/libsysop64.so,
# and an installed /usr/local/bin/mcp_server.py can find /usr/local/lib/libsysop64.so.
try:
    import ctypes
    script_dir = Path(__file__).resolve().parent
    lib_candidates = [
        script_dir / '../lib/libsysop64.so',
        script_dir / 'libsysop64.so',
        Path('../lib/libsysop64.so'),
    ]
    last_load_error = None
    for lib_path in lib_candidates:
        try:
            sysop64 = ctypes.CDLL(str(lib_path))
            break
        except OSError as e:
            last_load_error = e
    else:
        raise last_load_error
    # Define signatures for the libsysop64 functions used by this server.
    sysop64.sysop_init.restype = ctypes.c_int
    sysop64.sysop_server_connect.restype = ctypes.c_int
    sysop64.sysop_server_dma_lock.restype = None
    sysop64.sysop_server_dma_unlock.restype = None
    sysop64.sysop_c64_reset.restype = None

    sysop64.sysop_poke.argtypes = [ctypes.c_uint16, ctypes.c_uint8]
    sysop64.sysop_poke.restype = None
    sysop64.sysop_peek.argtypes = [ctypes.c_uint16]
    sysop64.sysop_peek.restype = ctypes.c_uint8

    sysop64.sysop_get_vic_info.restype = ctypes.c_uint8
    sysop64.sysop_get_palette_entry.argtypes = [
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.POINTER(ctypes.c_uint8),
    ]
    sysop64.sysop_get_palette_entry.restype = None
    sysop64.sysop_set_palette_entry.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
    sysop64.sysop_set_palette_entry.restype = None    
    SYSOP_AVAILABLE = True
except (OSError, AttributeError) as e:  # CDLL load failures or missing expected symbols
    SYSOP_AVAILABLE = False
    print("Warning: failed to load sysop64 library:", file=sys.stderr)
    print(f"  {e}", file=sys.stderr)
    # optional: full traceback for debugging
    traceback.print_exc()

MEMORY_SIZE = 65536
memory = bytearray(MEMORY_SIZE)


def built_tool_path(tool_name: str) -> str:
    """Return the path to a helper tool built next to this MCP server."""
    return str(Path(__file__).resolve().parent / tool_name)

# Tool implementations
def tool_sysop_reset():
    """Reset the C64"""
    global memory
    memory = bytearray(MEMORY_SIZE)
    if SYSOP_AVAILABLE:
        try:
            sysop64.sysop_c64_reset()
        except:
            pass
    print("System reset", file=sys.stderr)

def tool_sysop_poke(address: int, value: int):
    """Write an 8-bit value to a 16-bit memory address"""
    if 0 <= address < MEMORY_SIZE and 0 <= value <= 255:
        memory[address] = value
        print(f"Poked 0x{value:02X} at address 0x{address:04X}", file=sys.stderr)
        
        if SYSOP_AVAILABLE:
            try:
                sysop64.sysop_server_dma_lock()
                sysop64.sysop_poke(address, value)
                sysop64.sysop_server_dma_unlock()
            except:
                pass

def tool_sysop_write_memory(start_address: int, bytes_data: list) -> dict:
    """Write an array of bytes to memory starting at a given address"""
    if start_address < 0 or start_address >= MEMORY_SIZE:
        return {
            "success": False,
            "error": f"Invalid start address: {start_address} (must be 0-{MEMORY_SIZE-1})"
        }
    
    if not bytes_data or len(bytes_data) == 0:
        return {
            "success": False,
            "error": "No bytes provided to write"
        }
    
    # Check if write would go past end of memory
    if start_address + len(bytes_data) > MEMORY_SIZE:
        return {
            "success": False,
            "error": f"Write would exceed memory bounds (start: {start_address}, length: {len(bytes_data)}, max: {MEMORY_SIZE})"
        }
    
    # Validate all bytes are in valid range
    for i, byte_val in enumerate(bytes_data):
        if not isinstance(byte_val, int) or byte_val < 0 or byte_val > 255:
            return {
                "success": False,
                "error": f"Invalid byte value at index {i}: {byte_val} (must be 0-255)"
            }
    
    try:
        # Write to local memory array
        for i, byte_val in enumerate(bytes_data):
            memory[start_address + i] = byte_val
        
        # Write to actual C64 hardware if available
        if SYSOP_AVAILABLE:
            try:
                sysop64.sysop_server_dma_lock()
                for i, byte_val in enumerate(bytes_data):
                    sysop64.sysop_poke(start_address + i, byte_val)
                sysop64.sysop_server_dma_unlock()
            except Exception as e:
                return {
                    "success": False,
                    "error": f"Hardware write failed: {str(e)}"
                }
        
        print(f"Wrote {len(bytes_data)} bytes starting at 0x{start_address:04X}", file=sys.stderr)
        
        return {
            "success": True,
            "start_address": start_address,
            "bytes_written": len(bytes_data),
            "end_address": start_address + len(bytes_data) - 1
        }
    
    except Exception as e:
        return {
            "success": False,
            "error": f"Write failed: {str(e)}"
        }

def tool_sysop_peek(start: int, end: int) -> list:
    """Read a range of memory addresses and return byte values"""
    if start > end or end >= MEMORY_SIZE:
        return []
    
    result = []
    if SYSOP_AVAILABLE:
        try:
            sysop64.sysop_server_dma_lock()
            for addr in range(start, end + 1):
                result.append(sysop64.sysop_peek(addr))
            sysop64.sysop_server_dma_unlock()
        except:
            result = list(memory[start:end + 1])
    else:
        result = list(memory[start:end + 1])
    
    return result

def tool_sysop_read_screen(screen_address: int = 1024) -> str:
    """Read the C64 screen content and convert to text"""
    # C64 screen memory is typically at $0400 (1024), but can be configured via $D018
    # Screen is always 1024 bytes (40x25 chars)
    SCREEN_START = screen_address
    SCREEN_END = screen_address + 1023
    
    # PETSCII screen codes to ASCII mapping
    def petscii_screen_to_ascii(code):
        # Screen codes 1-26: A-Z
        if 1 <= code <= 26:
            return chr(ord('A') + code - 1)
        elif code == 0:
            return '@'
        elif code == 27:
            return '['
        elif code == 28:
            return '\\'
        elif code == 29:
            return ']'
        elif code == 30:
            return '^'
        elif code == 31:
            return '_'
        # Screen codes 32-63: space and symbols
        elif 32 <= code <= 63:
            return chr(code)
        # Screen codes 64-89: lowercase a-z (show as lowercase)
        elif 65 <= code <= 90:
            return chr(ord('a') + (code - 65))
        elif code == 64:
            return '@'
        elif code == 91:
            return '['
        elif code == 92:
            return '\\'
        elif code == 93:
            return ']'
        elif code == 94:
            return '^'
        elif code == 95:
            return '_'
        # Screen codes 96-127: graphics characters
        elif code == 96:
            return ' '
        else:
            return ' '  # Unknown/graphic char
    
    result = []
    if SYSOP_AVAILABLE:
        try:
            sysop64.sysop_server_dma_lock()
            for addr in range(SCREEN_START, SCREEN_END + 1):
                result.append(sysop64.sysop_peek(addr))
            sysop64.sysop_server_dma_unlock()
        except:
            result = list(memory[SCREEN_START:SCREEN_END + 1])
    else:
        result = list(memory[SCREEN_START:SCREEN_END + 1])
    
    # Convert to text with line breaks (40 chars per line, 25 lines)
    lines = []
    for row in range(25):
        start_idx = row * 40
        end_idx = start_idx + 40
        line = ''.join(petscii_screen_to_ascii(result[i]) for i in range(start_idx, end_idx))
        lines.append(line.rstrip())  # Remove trailing spaces
    
    return '\n'.join(lines)

def tool_sysop_get_vic_info() -> dict:
    """Get VIC-II chip model information"""
    
    # VIC chip model constants (bits 0-2)
    VIC_CHIP_6567R56A = 0      # NTSC OLD
    VIC_CHIP_6567R8 = 1        # NTSC NEW
    VIC_CHIP_6569 = 2          # PAL
    VIC_CHIP_6572RO_DREAN = 3  # DREAN
    
    VIC_CHIP_NAMES = {
        VIC_CHIP_6567R56A: "6567R56A (NTSC OLD)",
        VIC_CHIP_6567R8: "6567R8 (NTSC NEW)",
        VIC_CHIP_6569: "6569 (PAL)",
        VIC_CHIP_6572RO_DREAN: "6572RO (DREAN)"
    }
    
    VIC_CHIP_TYPES = {
        VIC_CHIP_6567R56A: "NTSC",
        VIC_CHIP_6567R8: "NTSC",
        VIC_CHIP_6569: "PAL",
        VIC_CHIP_6572RO_DREAN: "PAL-N (DREAN)"
    }
    
    if not SYSOP_AVAILABLE:
        return {
            "success": False,
            "error": "sysop64 library not available"
        }
    
    try:
        # Call sysop_get_vic_info to get the chip info byte
        vic_info = sysop64.sysop_get_vic_info()
        
        # Check if bit 7 (0x80) is set - indicates FPGA has determined VIC model
        if not (vic_info & 0x80):
            return {
                "success": False,
                "error": "FPGA has not yet determined VIC model",
                "determined": False,
                "raw_value": vic_info
            }
        
        # Extract chip model from bits 0-2 (mask with 0x7)
        chip_model = vic_info & 0x7
        
        chip_name = VIC_CHIP_NAMES.get(chip_model, f"Unknown ({chip_model})")
        chip_type = VIC_CHIP_TYPES.get(chip_model, "Unknown")
        
        return {
            "success": True,
            "determined": True,
            "chip_model": chip_model,
            "chip_name": chip_name,
            "video_standard": chip_type,
            "is_pal": chip_model == VIC_CHIP_6569,
            "is_ntsc": chip_model in [VIC_CHIP_6567R56A, VIC_CHIP_6567R8],
            "is_drean": chip_model == VIC_CHIP_6572RO_DREAN,
            "raw_value": vic_info
        }
    
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to get VIC info: {str(e)}"
        }

def tool_sysop_video_standard() -> dict:
    """Determine if the C64 is NTSC or PAL"""
    
    # VIC chip model constants
    VIC_CHIP_6567R56A = 0      # NTSC OLD
    VIC_CHIP_6567R8 = 1        # NTSC NEW
    VIC_CHIP_6569 = 2          # PAL
    VIC_CHIP_6572RO_DREAN = 3  # DREAN
    
    if not SYSOP_AVAILABLE:
        return {
            "success": False,
            "error": "sysop64 library not available"
        }
    
    try:
        # Call sysop_get_vic_info to determine video standard
        vic_info = sysop64.sysop_get_vic_info()
        
        # Check if bit 7 (0x80) is set - indicates FPGA has determined VIC model
        if not (vic_info & 0x80):
            return {
                "success": False,
                "error": "FPGA has not yet determined VIC model",
                "determined": False
            }
        
        # Extract chip model from bits 0-2 (mask with 0x7)
        chip_model = vic_info & 0x7
        
        # Determine PAL or NTSC based on chip model
        if chip_model == VIC_CHIP_6569:
            video_standard = "PAL"
            is_pal = True
            is_ntsc = False
        elif chip_model == VIC_CHIP_6572RO_DREAN:
            video_standard = "PAL-N (DREAN)"
            is_pal = True
            is_ntsc = False
        elif chip_model in [VIC_CHIP_6567R56A, VIC_CHIP_6567R8]:
            if chip_model == VIC_CHIP_6567R56A:
                video_standard = "NTSC (OLD)"
            else:
                video_standard = "NTSC (NEW)"
            is_pal = False
            is_ntsc = True
        else:
            video_standard = "Unknown"
            is_pal = False
            is_ntsc = False
        
        return {
            "success": True,
            "determined": True,
            "video_standard": video_standard,
            "is_pal": is_pal,
            "is_ntsc": is_ntsc,
            "chip_model": chip_model
        }
    
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to determine video standard: {str(e)}"
        }

def tool_sysop_get_palette_entry(index: int) -> dict:
    """Get the RGB values for a C64 palette entry (0-15)"""
    
    if not SYSOP_AVAILABLE:
        return {
            "success": False,
            "error": "sysop64 library not available"
        }
    
    # Validate index
    if index < 0 or index > 15:
        return {
            "success": False,
            "error": f"Invalid palette index: {index} (must be 0-15)"
        }
    
    try:
        # Create ctypes variables to receive the RGB values
        r = ctypes.c_uint8()
        g = ctypes.c_uint8()
        b = ctypes.c_uint8()
        
        # Call the C function
        sysop64.sysop_get_palette_entry(index, ctypes.byref(r), ctypes.byref(g), ctypes.byref(b))
        
        return {
            "success": True,
            "index": index,
            "r": r.value,
            "g": g.value,
            "b": b.value,
            "hex": f"#{r.value:02X}{g.value:02X}{b.value:02X}"
        }
    
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to get palette entry: {str(e)}"
        }

def tool_sysop_set_palette_entry(index: int, r: int, g: int, b: int) -> dict:
    """Set the RGB values for a C64 palette entry (0-15)"""
    
    if not SYSOP_AVAILABLE:
        return {
            "success": False,
            "error": "sysop64 library not available"
        }
    
    # Validate index
    if index < 0 or index > 15:
        return {
            "success": False,
            "error": f"Invalid palette index: {index} (must be 0-15)"
        }
    
    # Validate RGB values
    if r < 0 or r > 255:
        return {
            "success": False,
            "error": f"Invalid red value: {r} (must be 0-255)"
        }
    if g < 0 or g > 255:
        return {
            "success": False,
            "error": f"Invalid green value: {g} (must be 0-255)"
        }
    if b < 0 or b > 255:
        return {
            "success": False,
            "error": f"Invalid blue value: {b} (must be 0-255)"
        }
    
    try:
        # Call the C function
        sysop64.sysop_set_palette_entry(index, r, g, b)
        
        return {
            "success": True,
            "index": index,
            "r": r,
            "g": g,
            "b": b,
            "hex": f"#{r:02X}{g:02X}{b:02X}",
            "message": f"Set palette entry {index} to RGB({r}, {g}, {b})"
        }
    
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to set palette entry: {str(e)}"
        }

def tool_sysop_get_color_palette() -> dict:
    """Get all 16 RGB palette entries at once"""
    
    if not SYSOP_AVAILABLE:
        return {
            "success": False,
            "error": "sysop64 library not available",
            "palette": []
        }
    
    try:
        palette = []
        
        # Get all 16 palette entries
        for index in range(16):
            # Create ctypes variables to receive the RGB values
            r = ctypes.c_uint8()
            g = ctypes.c_uint8()
            b = ctypes.c_uint8()
            
            # Call the C function
            sysop64.sysop_get_palette_entry(index, ctypes.byref(r), ctypes.byref(g), ctypes.byref(b))
            
            palette.append({
                "index": index,
                "r": r.value,
                "g": g.value,
                "b": b.value,
                "hex": f"#{r.value:02X}{g.value:02X}{b.value:02X}"
            })
        
        return {
            "success": True,
            "palette": palette
        }
    
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to get color palette: {str(e)}",
            "palette": []
        }

# Screenshot counter for unique filenames
screenshot_counter = 0
screenshot_counter_lock = threading.Lock()

# File registry for tracking searched files
file_registry = {}
file_registry_counter = 0
file_registry_lock = threading.Lock()

def tool_sysop_screenshot(start_line: int = 1, width: int = 896, height: int = None) -> dict:
    """Capture a C64 screenshot using the screenshot utility"""
    global screenshot_counter
    
    # Validate parameters
    if start_line < 1:
        start_line = 1
    
    if width < 1:
        width = 896
    
    # If height is not provided, calculate it automatically (aspect ratio preserved)
    if height is None:
        # C64 typical aspect ratio calculation
        # Standard C64 resolution is 320x200, so aspect ratio is 1.6
        # If width is 896, height should be 560, but we'll use 659 as default
        height = 659
    
    if height < 1:
        height = 659
    
    # Create output directory
    output_dir = Path("outputs/screenshots")
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Generate unique filename
    with screenshot_counter_lock:
        screenshot_counter += 1
        filename = f"screenshot{screenshot_counter}.png"
    
    filepath = output_dir / filename
    
    try:
        # Call the screenshot utility with width and height
        cmd = ["/usr/local/bin/screenshot", str(filepath), str(start_line), str(width), str(height)]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Screenshot command failed: {result.stderr}",
                "filename": None,
                "image_data": None
            }
        
        # Read the PNG file and encode as base64
        if filepath.exists():
            with open(filepath, 'rb') as f:
                image_data = base64.b64encode(f.read()).decode('utf-8')
            
            return {
                "success": True,
                "filename": str(filepath),
                "image_data": image_data,
                "start_line": start_line,
                "width": width,
                "height": height
            }
        else:
            return {
                "success": False,
                "error": "Screenshot file was not created",
                "filename": None,
                "image_data": None
            }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Screenshot command timed out",
            "filename": None,
            "image_data": None
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Screenshot failed: {str(e)}",
            "filename": None,
            "image_data": None
        }

def tool_sysop_load(file_path: str) -> dict:
    """Load a .prg file into the C64 using the load utility"""
    
    # Check if file exists
    filepath = Path(file_path)
    if not filepath.exists():
        return {
            "success": False,
            "error": f"File not found: {file_path}"
        }
    
    # Check if file is a .prg file
    if filepath.suffix.lower() != '.prg':
        return {
            "success": False,
            "error": f"File must be a .prg file, got: {filepath.suffix}"
        }
    
    try:
        # Call the load utility
        cmd = ["/usr/local/bin/load", str(filepath)]
        print(f"Executing command: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Load command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Successfully loaded {filepath.name}",
            "file_path": str(filepath),
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Load command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Load failed: {str(e)}"
        }

def tool_sysop_load_d64(file_path: str) -> dict:
    """Load a program from a .d64 disk image using the read_d64 utility"""
    
    # Check if file exists
    filepath = Path(file_path)
    if not filepath.exists():
        return {
            "success": False,
            "error": f"File not found: {file_path}"
        }
    
    # Check if file is a .d64 file
    if filepath.suffix.lower() != '.d64':
        return {
            "success": False,
            "error": f"File must be a .d64 file, got: {filepath.suffix}"
        }
    
    try:
        # Call the read_d64 utility with load "*" command
        cmd = ["/usr/local/bin/read_d64", str(filepath), "load", "*"]
        print(f"Executing command: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Load D64 command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Successfully loaded program from {filepath.name}",
            "file_path": str(filepath),
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Load D64 command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Load D64 failed: {str(e)}"
        }

def tool_sysop_run() -> dict:
    """Run a loaded program using the run utility"""
    
    try:
        # Call the run utility
        cmd = ["/usr/local/bin/run"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Run command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": "Program execution started",
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Run command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Run failed: {str(e)}"
        }

def tool_sysop_display_message(message: str) -> dict:
    """Display a message on the C64 screen using the showmsg utility"""
    
    # Validate message length (255 bytes max)
    if len(message.encode('utf-8')) > 255:
        return {
            "success": False,
            "error": f"Message too long: {len(message.encode('utf-8'))} bytes (max 255 bytes)"
        }
    
    if not message or len(message.strip()) == 0:
        return {
            "success": False,
            "error": "Message cannot be empty"
        }
    
    try:
        # Call the showmsg utility with the message as an argument
        cmd = ["/usr/local/bin/showmsg", message]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Display message command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": "Message displayed on C64",
            "text": message,
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Display message command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Display message failed: {str(e)}"
        }

def tool_sidwizard_set_pattern_data(index: int, hex_bytes: str) -> dict:
    """Set SID Wizard pattern data using the sidwiz utility"""
    
    # Validate index (1-100)
    if index < 1 or index > 100:
        return {
            "success": False,
            "error": f"Invalid pattern index: {index} (must be 1-100)"
        }
    
    # Validate hex_bytes is not empty
    if not hex_bytes or len(hex_bytes.strip()) == 0:
        return {
            "success": False,
            "error": "Hex bytes string cannot be empty"
        }
    
    try:
        # Call the sidwiz utility with set-pattern-data command
        cmd = [built_tool_path("sidwiz"), "--set-pattern-data", str(index), hex_bytes]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"SID Wizard set pattern data command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Successfully set pattern data for index {index}",
            "index": index,
            "hex_bytes": hex_bytes,
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "SID Wizard set pattern data command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"SID Wizard set pattern data failed: {str(e)}"
        }

def _parse_hex_byte_string(hex_bytes: str) -> list[int]:
    """Parse comma- or whitespace-separated hex bytes."""
    cleaned = hex_bytes.replace('\\\r\n', '').replace('\\\n', '')
    parts = cleaned.replace(',', ' ').split()
    values = []
    for index, part in enumerate(parts):
        token = part.strip()
        if token.lower().startswith('0x'):
            token = token[2:]
        try:
            value = int(token, 16)
        except ValueError as exc:
            raise ValueError(f"Invalid hex byte at index {index}: {part}") from exc
        if value < 0 or value > 0xFF:
            raise ValueError(f"Hex byte out of range at index {index}: {part}")
        values.append(value)
    return values


def _try_expand_sidwizard_packed_instrument(data: list[int]) -> list[int] | None:
    """Expand SID-Wizard's packed .SWI instrument body to the 128-byte slot layout."""
    if len(data) < 10 or len(data) > 127:
        return None

    size_offset = len(data) - 9
    effective_size = data[size_offset]

    # packinst replaces the final $ff table delimiter with the effective size
    # byte, then appends the 8-byte name. That size byte therefore appears at
    # its own offset in the packed stream.
    if effective_size != size_offset:
        return None
    if effective_size < 0x0F or effective_size >= 128 - 8:
        return None

    expanded = [0] * 128
    expanded[:effective_size] = data[:effective_size]
    expanded[effective_size] = 0xFF
    expanded[120:128] = data[effective_size + 1:effective_size + 9]

    # SID-Wizard's depacker preserves old SW1 compatibility by forcing the
    # first waveform byte to sawtooth if it was zero.
    if expanded[0x0F] == 0:
        expanded[0x0F] = 0x09

    return expanded


def _normalize_sidwizard_instrument_bytes(values: list[int]) -> tuple[list[int], str]:
    """Accept raw 128-byte instruments or packed SID-Wizard .SWI/.PRG exports."""
    if len(values) == 128:
        return values, 'raw128'

    if len(values) == 130:
        return values[2:], 'raw128_prg'

    expanded = _try_expand_sidwizard_packed_instrument(values)
    if expanded is not None:
        return expanded, 'packed_swi'

    if len(values) > 2:
        expanded = _try_expand_sidwizard_packed_instrument(values[2:])
        if expanded is not None:
            return expanded, 'packed_swi_prg'

    raise ValueError(
        f"Expected a raw 128-byte instrument, raw 128-byte PRG, or packed SID-Wizard .SWI/.PRG export; got {len(values)} bytes"
    )


def tool_sidwizard_load_inst(instrument_number: int, hex_bytes: str) -> dict:
    """Load SID Wizard instrument data using the sidwiz utility"""
    
    # Validate instrument number (1-36)
    if instrument_number < 1 or instrument_number > 36:
        return {
            "success": False,
            "error": f"Invalid instrument number: {instrument_number} (must be 1-36)"
        }
    
    # Validate hex_bytes is not empty
    if not hex_bytes or len(hex_bytes.strip()) == 0:
        return {
            "success": False,
            "error": "Hex bytes string cannot be empty"
        }

    try:
        input_values = _parse_hex_byte_string(hex_bytes)
        normalized_values, input_format = _normalize_sidwizard_instrument_bytes(input_values)
    except ValueError as e:
        return {
            "success": False,
            "error": str(e)
        }

    normalized_hex = ','.join(f'{value:02X}' for value in normalized_values)
    
    try:
        # Call the sidwiz utility with expanded 128-byte instrument data.
        cmd = [
            built_tool_path('sidwiz'),
            '--load-inst',
            str(instrument_number),
            'hex',
            normalized_hex,
        ]
        print(f"Executing command: {' '.join(cmd[:4])} <128 bytes from {input_format}>", file=sys.stderr)
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"SID Wizard load instrument command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Successfully loaded instrument {instrument_number}",
            "instrument_number": instrument_number,
            "input_format": input_format,
            "input_bytes": len(input_values),
            "hex_bytes": normalized_hex,
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "SID Wizard load instrument command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"SID Wizard load instrument failed: {str(e)}"
        }

def tool_sidwizard_get_inst(instrument_number: int, format: str = "hex") -> dict:
    """Get SID Wizard instrument data using the sidwiz utility"""
    
    # Validate instrument number (1-36)
    if instrument_number < 1 or instrument_number > 36:
        return {
            "success": False,
            "error": f"Invalid instrument number: {instrument_number} (must be 1-36)"
        }
    
    # Validate format - only hex is supported
    if format != "hex":
        return {
            "success": False,
            "error": f"Invalid format: {format} (only 'hex' is supported)"
        }
    
    try:
        # Call the sidwiz utility with get-inst command
        cmd = [built_tool_path("sidwiz"), "--get-inst", str(instrument_number), format]
        print(f"Executing command: {' '.join(cmd)}", file=sys.stderr)
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"SID Wizard get instrument command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Successfully retrieved instrument {instrument_number} in {format} format",
            "instrument_number": instrument_number,
            "format": format,
            "data": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "SID Wizard get instrument command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"SID Wizard get instrument failed: {str(e)}"
        }

def tool_sidwizard_get_order_list(voice: int) -> dict:
    """Get SID Wizard order list for a specific voice using the sidwiz utility"""
    
    # Validate voice number (1-3)
    if voice < 1 or voice > 3:
        return {
            "success": False,
            "error": f"Invalid voice number: {voice} (must be 1-3)"
        }
    
    try:
        # Call the sidwiz utility with get-order-list command
        cmd = [built_tool_path("sidwiz"), "--get-order-list", str(voice)]
        print(f"Executing command: {' '.join(cmd)}", file=sys.stderr)
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"SID Wizard get order list command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Successfully retrieved order list for voice {voice}",
            "voice": voice,
            "data": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "SID Wizard get order list command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"SID Wizard get order list failed: {str(e)}"
        }

def tool_sidwizard_set_order_list(voice: int, hex_bytes: str) -> dict:
    """Set SID Wizard order list for a specific voice using the sidwiz utility"""
    
    # Validate voice number (1-3)
    if voice < 1 or voice > 3:
        return {
            "success": False,
            "error": f"Invalid voice number: {voice} (must be 1-3)"
        }
    
    # Validate hex_bytes is not empty
    if not hex_bytes or len(hex_bytes.strip()) == 0:
        return {
            "success": False,
            "error": "Hex bytes string cannot be empty"
        }
    
    try:
        order_values = _parse_hex_byte_string(hex_bytes)
    except ValueError as e:
        return {
            "success": False,
            "error": str(e)
        }

    # SID-Wizard order lists terminate with FE (end song) or FF followed
    # by a loop-position byte. Pattern numbers are one-based in the list.
    ends_with_end = len(order_values) >= 1 and order_values[-1] == 0xFE
    ends_with_loop = len(order_values) >= 2 and order_values[-2] == 0xFF
    if not (ends_with_end or ends_with_loop):
        return {
            "success": False,
            "error": "Order list must end with FE, or with FF followed by a loop position byte"
        }

    if ends_with_loop and order_values[-1] > 0x7F:
        return {
            "success": False,
            "error": "Order list loop position after FF must be 00-7F"
        }
    
    normalized_hex = " ".join(f"{value:02X}" for value in order_values)
    
    try:
        # Call the sidwiz utility with set-order-list command.
        cmd = [built_tool_path("sidwiz"), "--set-order-list", str(voice), normalized_hex]
        print(f"Executing command: {' '.join(cmd)}", file=sys.stderr)
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"SID Wizard set order list command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Successfully set order list for voice {voice}",
            "voice": voice,
            "hex_bytes": normalized_hex,
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "SID Wizard set order list command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"SID Wizard set order list failed: {str(e)}"
        }

def tool_sidwizard_clear_song() -> dict:
    """Clear note data in all SID Wizard patterns. Leaves instruments, chords, and tempo untouched."""
    
    try:
        # Call the sidwiz utility with clear-song command
        cmd = [built_tool_path("sidwiz"), "--clear-song"]
        print(f"Executing command: {' '.join(cmd)}", file=sys.stderr)
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"SID Wizard clear song command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": "Successfully cleared SID Wizard song",
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "SID Wizard clear song command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"SID Wizard clear song failed: {str(e)}"
        }

def tool_sidwizard_playback(action: str) -> dict:
    """Start, stop, or inspect SID-Wizard playback using the sidwiz utility."""

    action_map = {
        "play-song": "--play-song",
        "stop": "--stop",
        "status": "--play-status",
    }

    if action not in action_map:
        return {
            "success": False,
            "error": f"Invalid playback action: {action} (must be play-song, stop, or status)"
        }

    try:
        cmd = [built_tool_path("sidwiz"), action_map[action]]
        print(f"Executing command: {' '.join(cmd)}", file=sys.stderr)
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)

        if result.returncode != 0:
            return {
                "success": False,
                "error": f"SID Wizard playback command failed: {result.stderr}",
                "stdout": result.stdout
            }

        return {
            "success": True,
            "message": f"SID Wizard playback action '{action}' completed",
            "action": action,
            "stdout": result.stdout
        }

    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "SID Wizard playback command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"SID Wizard playback command failed: {str(e)}"
        }
def tool_sysop_screen_info() -> dict:
    """Get C64 screen and display register information using the monitor utility"""
    
    try:
        # Call the monitor utility with screen-info command
        cmd = ["/usr/local/bin/mon", "--screen-info"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Screen info command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "screen_info": result.stdout,
            "message": "Screen information retrieved successfully. This shows VIC register states including screen memory location, VIC bank, sprite pointers, and display mode."
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Screen info command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Screen info failed: {str(e)}"
        }

def tool_sysop_disassemble(start_addr: str, end_addr: str) -> dict:
    """Disassemble C64 memory between two addresses using the monitor utility"""
    
    # Validate hex addresses
    try:
        # Remove any 0x prefix and validate hex format
        start_clean = start_addr.replace('0x', '').replace('0X', '')
        end_clean = end_addr.replace('0x', '').replace('0X', '')
        
        # Validate they are valid hex
        start_val = int(start_clean, 16)
        end_val = int(end_clean, 16)
        
        # Validate range
        if start_val < 0 or start_val > 0xFFFF:
            return {
                "success": False,
                "error": f"Invalid start address: {start_addr} (must be 0x0000-0xFFFF)"
            }
        
        if end_val < 0 or end_val > 0xFFFF:
            return {
                "success": False,
                "error": f"Invalid end address: {end_addr} (must be 0x0000-0xFFFF)"
            }
        
        if start_val > end_val:
            return {
                "success": False,
                "error": f"Start address ({start_addr}) must be less than or equal to end address ({end_addr})"
            }
        
    except ValueError:
        return {
            "success": False,
            "error": f"Invalid hexadecimal address format: start={start_addr}, end={end_addr}"
        }
    
    try:
        # Call the monitor utility with disassemble command
        cmd = ["/usr/local/bin/mon", "--disassemble", start_clean, end_clean]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Disassemble command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Disassembled memory from ${start_clean} to ${end_clean}",
            "start_addr": start_clean,
            "end_addr": end_clean,
            "disassembly": result.stdout,
            "instructions": "To annotate this disassembly, copy the output and in your next prompt, add single-line comments after each instruction line using tabs and '; ' followed by a brief explanation of what that instruction does. Make sure all the comments are aligned to the same indentation. This will help document and understand the code.\n\nTo continue disassembling beyond this range, calculate the next start address by looking at the last line of output. Take the address at the start of that line and add the number of opcode and data bytes that follow it. For example, if the last line is '$083C: 8D 21 D0', the bytes are '8D 21 D0' (3 bytes), so the next address would be $083C + 3 = $083F."
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Disassemble command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Disassemble failed: {str(e)}"
        }

def tool_sysop_bypass_keypress(pattern: str = "all") -> dict:
    """Bypass keyboard input handling by patching running code"""
    
    # Validate pattern
    valid_patterns = ["all", "any", "space", "space2", "runstop_or_space", 
                      "highscore_or_trainer", "load_or_reset", "y", "n"]
    
    if pattern not in valid_patterns:
        return {
            "success": False,
            "error": f"Invalid pattern: {pattern}. Must be one of: {', '.join(valid_patterns)}"
        }
    
    try:
        # Call the bypass_keypress utility
        cmd = ["/usr/local/bin/bypass_keypress", pattern]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Bypass keypress command failed: {result.stderr}",
                "stdout": result.stdout
            }
        
        return {
            "success": True,
            "message": f"Successfully bypassed keypress with pattern: {pattern}",
            "pattern": pattern,
            "stdout": result.stdout
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Bypass keypress command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Bypass keypress failed: {str(e)}"
        }

def tool_sysop_list_d64(file_path: str) -> dict:
    """List the directory contents of a .d64 disk image"""
    
    # Check if file exists
    filepath = Path(file_path)
    if not filepath.exists():
        return {
            "success": False,
            "error": f"File not found: {file_path}",
            "files": []
        }
    
    # Check if file is a .d64 file
    if filepath.suffix.lower() != '.d64':
        return {
            "success": False,
            "error": f"File must be a .d64 file, got: {filepath.suffix}",
            "files": []
        }
    
    def petscii_to_ascii(petscii_byte):
        """Convert PETSCII byte to ASCII character"""
        # PETSCII codes 65-90 (0x41-0x5A) are uppercase A-Z
        if 65 <= petscii_byte <= 90:
            return chr(petscii_byte)
        # PETSCII codes 97-122 (0x61-0x7A) are lowercase (but show as uppercase)
        elif 97 <= petscii_byte <= 122:
            return chr(petscii_byte - 32)  # Convert to uppercase
        # PETSCII codes 193-218 (0xC1-0xDA) are lowercase a-z
        elif 193 <= petscii_byte <= 218:
            return chr(petscii_byte - 96)  # a-z
        # Space and common symbols
        elif petscii_byte == 32:
            return ' '
        elif petscii_byte == 33:
            return '!'
        elif petscii_byte == 34:
            return '"'
        elif petscii_byte == 35:
            return '#'
        elif petscii_byte == 36:
            return '$'
        elif petscii_byte == 37:
            return '%'
        elif petscii_byte == 38:
            return '&'
        elif petscii_byte == 39:
            return "'"
        elif petscii_byte == 40:
            return '('
        elif petscii_byte == 41:
            return ')'
        elif petscii_byte == 42:
            return '*'
        elif petscii_byte == 43:
            return '+'
        elif petscii_byte == 44:
            return ','
        elif petscii_byte == 45:
            return '-'
        elif petscii_byte == 46:
            return '.'
        elif petscii_byte == 47:
            return '/'
        elif 48 <= petscii_byte <= 57:  # 0-9
            return chr(petscii_byte)
        elif petscii_byte == 58:
            return ':'
        elif petscii_byte == 59:
            return ';'
        elif petscii_byte == 60:
            return '<'
        elif petscii_byte == 61:
            return '='
        elif petscii_byte == 62:
            return '>'
        elif petscii_byte == 63:
            return '?'
        elif petscii_byte == 64:
            return '@'
        elif petscii_byte == 160:  # shifted space
            return ' '
        else:
            return '?'  # Unknown character
    
    try:
        # Call the read_d64 utility with dir command
        cmd = ["/usr/local/bin/read_d64", str(filepath), "dir"]
        result = subprocess.run(cmd, capture_output=True, timeout=10)
        
        if result.returncode != 0:
            stderr_text = result.stderr.decode('utf-8', errors='replace')
            return {
                "success": False,
                "error": f"List D64 command failed: {stderr_text}",
                "files": []
            }
        
        # Decode output handling potential encoding issues
        stdout_text = result.stdout.decode('utf-8', errors='replace')
        
        # Parse the output to extract file information
        files = []
        lines = stdout_text.split('\n')
        
        for line in lines:
            # Look for lines with "Filename in hex:"
            if "Filename in hex:" in line:
                hex_bytes_str = line.split("Filename in hex:")[1].strip()
                hex_bytes = hex_bytes_str.split()
                
                # Convert hex bytes to PETSCII then to ASCII
                filename = ""
                for hex_byte in hex_bytes:
                    try:
                        byte_val = int(hex_byte, 16)
                        filename += petscii_to_ascii(byte_val)
                    except ValueError:
                        continue
                
                # Look for the next line with "Filename (2):" to verify
                # and subsequent lines for file size and type
                files.append({
                    "filename": filename.strip(),
                    "hex": hex_bytes_str
                })
        
        # Also try to extract file sizes and types from the output
        file_entries = []
        current_file = None
        
        for line in lines:
            if "Filename in hex:" in line:
                if current_file:
                    file_entries.append(current_file)
                
                hex_bytes_str = line.split("Filename in hex:")[1].strip()
                hex_bytes = hex_bytes_str.split()
                filename = ""
                for hex_byte in hex_bytes:
                    try:
                        byte_val = int(hex_byte, 16)
                        filename += petscii_to_ascii(byte_val)
                    except ValueError:
                        continue
                
                current_file = {
                    "filename": filename.strip(),
                    "size": 0,
                    "blocks": 0,
                    "type": "Unknown"
                }
            
            elif current_file and "File size" in line:
                # Parse "File size C100 (193 blocks)"
                parts = line.split()
                for i, part in enumerate(parts):
                    if part == "size" and i + 1 < len(parts):
                        size_hex = parts[i + 1]
                        try:
                            current_file["size"] = int(size_hex, 16)
                        except ValueError:
                            pass
                    if part == "blocks)" and i > 0:
                        try:
                            current_file["blocks"] = int(parts[i - 1].strip('('))
                        except ValueError:
                            pass
            
            elif current_file and "File Type:" in line:
                # Parse "File Type: 2"
                parts = line.split(":")
                if len(parts) > 1:
                    type_val = parts[1].strip()
                    if type_val == "0":
                        current_file["type"] = "DEL"
                    elif type_val == "1":
                        current_file["type"] = "SEQ"
                    elif type_val == "2":
                        current_file["type"] = "PRG"
                    elif type_val == "3":
                        current_file["type"] = "USR"
                    elif type_val == "4":
                        current_file["type"] = "REL"
                    else:
                        current_file["type"] = f"Type {type_val}"
        
        if current_file:
            file_entries.append(current_file)
        
        # Filter out separator lines and empty entries
        file_entries = [f for f in file_entries if f["filename"] and not all(c in '-?' for c in f["filename"])]
        
        return {
            "success": True,
            "file_path": str(filepath),
            "disk_name": filepath.name,
            "file_count": len(file_entries),
            "files": file_entries,
            "raw_output": stdout_text
        }
    
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "List D64 command timed out",
            "files": []
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"List D64 failed: {str(e)}",
            "files": []
        }

def tool_sysop_find_files(pattern: str) -> dict:
    """Search for files matching a pattern in /mnt/data/c64_files"""
    global file_registry, file_registry_counter
    
    base_path = Path("/mnt/data/c64_files")
    
    if not base_path.exists():
        return {
            "success": False,
            "error": f"Base path does not exist: {base_path}",
            "files": []
        }
    
    try:
        # Search for files matching the pattern
        matching_files = []
        
        # Use glob pattern if it contains wildcards, otherwise search by substring
        if '*' in pattern or '?' in pattern:
            # Glob search - search recursively through all subdirectories
            # For patterns like "karate*", we need to check filenames in all subdirs
            for filepath in base_path.rglob('*'):
                if filepath.is_file():
                    # Use fnmatch to match the pattern against just the filename
                    import fnmatch
                    if fnmatch.fnmatch(filepath.name.lower(), pattern.lower()):
                        matching_files.append(filepath)
        else:
            # Substring search in filename
            for filepath in base_path.rglob('*'):
                if filepath.is_file() and pattern.lower() in filepath.name.lower():
                    matching_files.append(filepath)
        
        # Sort files by name
        matching_files.sort(key=lambda p: p.name)
        
        # Assign reference numbers to files
        file_list = []
        with file_registry_lock:
            for filepath in matching_files:
                file_registry_counter += 1
                file_id = file_registry_counter
                file_registry[file_id] = str(filepath)
                
                file_list.append({
                    "id": file_id,
                    "path": str(filepath),
                    "name": filepath.name,
                    "size": filepath.stat().st_size
                })
        
        return {
            "success": True,
            "pattern": pattern,
            "count": len(file_list),
            "files": file_list
        }
    
    except Exception as e:
        return {
            "success": False,
            "error": f"File search failed: {str(e)}",
            "files": []
        }

# MCP protocol handlers
def handle_initialize():
    """Handle MCP initialize request"""
    return {
        "protocolVersion": "2024-11-05",
        "capabilities": {
            "tools": {
                "listChanged": False
            }
        },
        "serverInfo": {
            "name": "sysop64-mcp",
            "version": "1.0.0"
        }
    }

def handle_list_tools():
    """Handle MCP tools/list request"""
    tools = [
        {
            "name": "sysop_reset",
            "description": "Reset the Commodore 64 system",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "sysop_poke",
            "description": "Write an 8-bit value to a 16-bit memory address",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "address": {
                        "type": "integer",
                        "description": "16-bit memory address (0-65535)"
                    },
                    "value": {
                        "type": "integer",
                        "description": "8-bit value to write (0-255)"
                    }
                },
                "required": ["address", "value"]
            }
        },
        {
            "name": "sysop_write_memory",
            "description": "Write multiple bytes to consecutive memory addresses starting at a given address. More efficient than calling sysop_poke multiple times.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "start_address": {
                        "type": "integer",
                        "description": "Starting 16-bit memory address (0-65535)"
                    },
                    "bytes_data": {
                        "type": "array",
                        "description": "Array of byte values to write (each 0-255)",
                        "items": {
                            "type": "integer",
                            "minimum": 0,
                            "maximum": 255
                        }
                    }
                },
                "required": ["start_address", "bytes_data"]
            }
        },
        {
            "name": "sysop_peek",
            "description": "Read a range of memory addresses and return byte values",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "start": {
                        "type": "integer",
                        "description": "Start address (0-65535)"
                    },
                    "end": {
                        "type": "integer",
                        "description": "End address (0-65535)"
                    }
                },
                "required": ["start", "end"]
            }
        },
        {
            "name": "sysop_read_screen",
            "description": "Read the C64 screen content as text (40x25 characters). Best practice: Call sysop_screen_info first to determine the current screen memory location from the D018 register, then pass that address to this tool. Default is 1024 ($0400).",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "screen_address": {
                        "type": "integer",
                        "description": "Starting address of screen memory (default: 1024 / $0400). Use sysop_screen_info to determine the current location.",
                        "default": 1024,
                        "minimum": 0,
                        "maximum": 65535
                    }
                },
                "required": []
            }
        },
        {
            "name": "sysop_screenshot",
            "description": "Capture a C64 screenshot and return as PNG image. The result includes base64-encoded image data that should be decoded and saved to a local file for viewing. The base64 data in the response allows the client to save a local copy or add it as an attachment to the current context.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "start_line": {
                        "type": "integer",
                        "description": "Starting line for capture (default: 1)",
                        "default": 1
                    },
                    "width": {
                        "type": "integer",
                        "description": "Width of the output image in pixels (default: 896)",
                        "default": 896,
                        "minimum": 1
                    },
                    "height": {
                        "type": "integer",
                        "description": "Height of the output image in pixels (default: 659, calculated automatically if omitted)",
                        "default": 659,
                        "minimum": 1
                    }
                },
                "required": []
            }
        },
        {
            "name": "sysop_find_files",
            "description": "Search for files in /mnt/data/c64_files matching a pattern. Returns file paths with reference IDs.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "pattern": {
                        "type": "string",
                        "description": "Search pattern (filename substring or glob pattern with * and ?)"
                    }
                },
                "required": ["pattern"]
            }
        },
        {
            "name": "sysop_load",
            "description": "Load a .prg file into the C64 memory",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "file_path": {
                        "type": "string",
                        "description": "Absolute path to the .prg file to load"
                    }
                },
                "required": ["file_path"]
            }
        },
        {
            "name": "sysop_load_d64",
            "description": "Load a program from a .d64 disk image",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "file_path": {
                        "type": "string",
                        "description": "Absolute path to the .d64 disk image file"
                    }
                },
                "required": ["file_path"]
            }
        },
        {
            "name": "sysop_list_d64",
            "description": "List the directory contents of a .d64 disk image",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "file_path": {
                        "type": "string",
                        "description": "Absolute path to the .d64 disk image file"
                    }
                },
                "required": ["file_path"]
            }
        },
        {
            "name": "sysop_run",
            "description": "Run a loaded program (use after sysop_load or sysop_load_d64)",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "sysop_display_message",
            "description": "Display a message on the C64 screen. Supports up to 3 lines of text (255 bytes max) and can include emoji characters. Use this to show messages, notifications, or information to the user on the C64.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "message": {
                        "type": "string",
                        "description": "Message text to display (max 255 bytes, can include emoji and newlines for multiple lines)"
                    }
                },
                "required": ["message"]
            }
        },
        {
            "name": "sidwizard_set_pattern_data",
            "description": "Set SID Wizard pattern data. Allows you to configure pattern data for SID music patterns by index (1-100) with space-separated hexadecimal bytes.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "index": {
                        "type": "integer",
                        "description": "Pattern index (1-100)",
                        "minimum": 1,
                        "maximum": 100
                    },
                    "hex_bytes": {
                        "type": "string",
                        "description": "Space-separated hexadecimal bytes (e.g., '00 01 02 FF')"
                    }
                },
                "required": ["index", "hex_bytes"]
            }
        },
        {
            "name": "sidwizard_load_inst",
            "description": "Load SID Wizard instrument data for a specific instrument number (1-36). Accepts raw 128-byte SID-Wizard slot data or packed .SWI/.PRG instrument exports; packed exports are expanded before loading.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "instrument_number": {
                        "type": "integer",
                        "description": "Instrument number (1-36)",
                        "minimum": 1,
                        "maximum": 36
                    },
                    "hex_bytes": {
                        "type": "string",
                        "description": "Comma- or whitespace-separated hexadecimal bytes. Accepts raw 128-byte slot data, raw 128-byte PRG data with load address, or packed SID-Wizard .SWI/.PRG instrument export bytes."
                    }
                },
                "required": ["instrument_number", "hex_bytes"]
            }
        },
        {
            "name": "sidwizard_get_inst",
            "description": "Get SID Wizard instrument data. Retrieves instrument data for a specific instrument number (1-36) as comma-delimited hex bytes (128 bytes).",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "instrument_number": {
                        "type": "integer",
                        "description": "Instrument number (1-36)",
                        "minimum": 1,
                        "maximum": 36
                    }
                },
                "required": ["instrument_number"]
            }
        },
        {
            "name": "sidwizard_get_order_list",
            "description": "Get SID Wizard order list for a specific voice. Returns comma-delimited hex bytes. FE ends the song; FF followed by a loop-position byte loops the order list.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "voice": {
                        "type": "integer",
                        "description": "Voice number (1-3)",
                        "minimum": 1,
                        "maximum": 3
                    }
                },
                "required": ["voice"]
            }
        },
        {
            "name": "sidwizard_set_order_list",
            "description": "Set SID Wizard order list for a specific voice. Use FE to end the song, or FF followed by a loop-position byte to loop.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "voice": {
                        "type": "integer",
                        "description": "Voice number (1-3)",
                        "minimum": 1,
                        "maximum": 3
                    },
                    "hex_bytes": {
                        "type": "string",
                        "description": "Space-separated hexadecimal bytes. End with FE for one-shot playback (e.g., '01 FE') or FF plus loop position for looping (e.g., '01 FF 00')."
                    }
                },
                "required": ["voice", "hex_bytes"]
            }
        },
        {
            "name": "sidwizard_playback",
            "description": "Control SID-Wizard playback through sidwiz. action=play-song requests normal song playback from the beginning, stop halts playback, and status returns the current play mode/init request state.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "action": {
                        "type": "string",
                        "description": "Playback action to perform",
                        "enum": ["play-song", "stop", "status"]
                    }
                },
                "required": ["action"]
            }
        },        {
            "name": "sidwizard_clear_song",
            "description": "Clear note data in all SID Wizard patterns. This clears the note data in all patterns but leaves instruments, chords, and tempo settings untouched.",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "sysop_screen_info",
            "description": "Get C64 screen and display register information. Returns the state of key VIC-II registers including screen memory location ($D018), VIC bank ($DD00), display mode ($D011, $D016), and sprite pointer locations. Use this to determine where screen content and sprite data are located in memory before reading them.",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "sysop_disassemble",
            "description": "Disassemble C64 memory between two hexadecimal addresses. Returns 6502 assembly instructions with addresses. The output shows one instruction per line with its memory address. To make the disassembly more useful, you should annotate it by adding '; ' followed by single-line comments explaining what each instruction does. Simply copy the disassembly output and add your comments in the next prompt.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "start_addr": {
                        "type": "string",
                        "description": "Starting hexadecimal address (e.g., '0800', '1000', 'C000'). Can include or omit '0x' prefix."
                    },
                    "end_addr": {
                        "type": "string",
                        "description": "Ending hexadecimal address (e.g., '0900', '1100', 'CFFF'). Can include or omit '0x' prefix."
                    }
                },
                "required": ["start_addr", "end_addr"]
            }
        },
        {
            "name": "sysop_bypass_keypress",
            "description": "Bypass keyboard input in running C64 programs by patching the code to skip waiting for key presses. This is useful when a game or program is stuck waiting for keyboard input (like pressing SPACE to start, Y/N prompts, or 'Press any key' screens). IMPORTANT: Use pattern='all' (the default) in most cases - it will automatically try all patch patterns and use the first one that works. Only use specific patterns (y, n, space, etc.) if the user explicitly requests bypassing a specific key. Common use cases: stuck at title screen waiting for start, trainer/cheat menus asking Y/N, high score entry prompts, or any screen waiting for keyboard input.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "pattern": {
                        "type": "string",
                        "description": "Pattern to match: 'all' (DEFAULT - tries all patterns automatically), 'any', 'space', 'space2', 'runstop_or_space', 'highscore_or_trainer', 'load_or_reset', 'y' (for Yes prompts), 'n' (for No prompts). Use 'all' unless user specifically mentions a key.",
                        "default": "all",
                        "enum": ["all", "any", "space", "space2", "runstop_or_space", "highscore_or_trainer", "load_or_reset", "y", "n"]
                    }
                },
                "required": []
            }
        },
        {
            "name": "sysop_get_vic_info",
            "description": "Get VIC-II chip model information (6567R56A, 6567R8, 6569, 6572RO_DREAN) and determine video standard",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "sysop_video_standard",
            "description": "Determine if the C64 is NTSC or PAL",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "sysop_get_palette_entry",
            "description": "Get the RGB color values for a C64 palette entry. The C64 has 16 palette entries (0-15) that define the colors used by the VIC-II chip.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "index": {
                        "type": "integer",
                        "description": "Palette index (0-15)",
                        "minimum": 0,
                        "maximum": 15
                    }
                },
                "required": ["index"]
            }
        },
        {
            "name": "sysop_get_color_palette",
            "description": "Get all 16 RGB color palette entries at once. More efficient than calling sysop_get_palette_entry 16 times. Returns an array of all palette colors.",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "sysop_set_palette_entry",
            "description": "Set the RGB color values for a C64 palette entry. This allows you to customize the C64's color palette. The C64 has 16 palette entries (0-15).",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "index": {
                        "type": "integer",
                        "description": "Palette index (0-15)",
                        "minimum": 0,
                        "maximum": 15
                    },
                    "r": {
                        "type": "integer",
                        "description": "Red value (0-255)",
                        "minimum": 0,
                        "maximum": 255
                    },
                    "g": {
                        "type": "integer",
                        "description": "Green value (0-255)",
                        "minimum": 0,
                        "maximum": 255
                    },
                    "b": {
                        "type": "integer",
                        "description": "Blue value (0-255)",
                        "minimum": 0,
                        "maximum": 255
                    }
                },
                "required": ["index", "r", "g", "b"]
            }
        }
    ]
    
    return {"tools": tools}

def handle_call_tool(tool_name: str, arguments: dict):
    """Handle MCP tools/call request"""
    content = []
    
    if tool_name == "sysop_reset":
        tool_sysop_reset()
        content.append({
            "type": "text",
            "text": "System reset successfully"
        })
    
    elif tool_name == "sysop_poke":
        address = arguments.get("address")
        value = arguments.get("value")
        if address is not None and value is not None:
            tool_sysop_poke(int(address), int(value))
            content.append({
                "type": "text",
                "text": f"Wrote 0x{value:02X} to address 0x{address:04X}"
            })
    
    elif tool_name == "sysop_write_memory":
        start_address = arguments.get("start_address")
        bytes_data = arguments.get("bytes_data")
        if start_address is not None and bytes_data is not None:
            result = tool_sysop_write_memory(int(start_address), bytes_data)
            if result["success"]:
                content.append({
                    "type": "text",
                    "text": f"Wrote {result['bytes_written']} bytes to 0x{result['start_address']:04X}-0x{result['end_address']:04X}"
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"Write memory failed: {result['error']}"
                })
    
    elif tool_name == "sysop_peek":
        start = arguments.get("start")
        end = arguments.get("end")
        if start is not None and end is not None:
            bytes_data = tool_sysop_peek(int(start), int(end))
            content.append({
                "type": "text",
                "text": json.dumps(bytes_data)
            })
    
    elif tool_name == "sysop_read_screen":
        screen_address = arguments.get("screen_address", 1024)
        screen_text = tool_sysop_read_screen(int(screen_address))
        content.append({
            "type": "text",
            "text": screen_text
        })
    
    elif tool_name == "sysop_screenshot":
        start_line = arguments.get("start_line", 1)
        width = arguments.get("width", 896)
        height = arguments.get("height", None)
        result = tool_sysop_screenshot(int(start_line), int(width), int(height) if height is not None else None)
        
        if result["success"]:
            # Return both text confirmation and image data
            content.append({
                "type": "text",
                "text": f"Screenshot captured successfully (saved to {result['filename']}, {result['width']}x{result['height']})"
            })
            content.append({
                "type": "image",
                "data": result["image_data"],
                "mimeType": "image/png"
            })
        else:
            content.append({
                "type": "text",
                "text": f"Screenshot failed: {result['error']}"
            })
    
    elif tool_name == "sysop_find_files":
        pattern = arguments.get("pattern", "")
        result = tool_sysop_find_files(pattern)
        
        if result["success"]:
            if result["count"] > 0:
                file_list_text = f"Found {result['count']} file(s) matching '{result['pattern']}':\n\n"
                file_list_text += "File Reference # | Full Path for Loading | Tool to Use\n"
                file_list_text += "-" * 70 + "\n"
                
                for file_info in result["files"]:
                    file_ref = f"#{file_info['id']}"
                    full_path = file_info['path']
                    file_ext = file_info['name'].split('.')[-1].lower()
                    
                    # Determine which tool to use based on extension
                    if file_ext == 'prg':
                        tool_to_use = "sysop_load"
                        tool_hint = "(use sysop_load)"
                    elif file_ext == 'd64':
                        tool_to_use = "sysop_load_d64"
                        tool_hint = "(use sysop_load_d64)"
                    else:
                        tool_to_use = "unknown"
                        tool_hint = "(unknown type)"
                    
                    file_list_text += f"{file_ref:16s} | {full_path}\n"
                    file_list_text += f"                 | Name: {file_info['name']}, Size: {file_info['size']} bytes, {tool_hint}\n\n"
                
                file_list_text += "\nTo load a file, use the file reference number with the appropriate tool:\n"
                file_list_text += "- For .prg files: Call sysop_load with file_path={full_path}\n"
                file_list_text += "- For .d64 files: Call sysop_load_d64 with file_path={full_path}\n"
                file_list_text += "Example: sysop_load(file_path=\"/path/to/file.prg\") or reference by #{file_ref}\n"
                
                content.append({
                    "type": "text",
                    "text": file_list_text.rstrip()
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"No files found matching pattern: {result['pattern']}"
                })
        else:
            content.append({
                "type": "text",
                "text": f"File search failed: {result['error']}"
            })
    
    elif tool_name == "sysop_load":
        file_path = arguments.get("file_path", "")
        result = tool_sysop_load(file_path)
        
        if result["success"]:
            content.append({
                "type": "text",
                "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
            })
        else:
            content.append({
                "type": "text",
                "text": f"Load failed: {result['error']}"
            })
    
    elif tool_name == "sysop_load_d64":
        file_path = arguments.get("file_path", "")
        result = tool_sysop_load_d64(file_path)
        
        if result["success"]:
            content.append({
                "type": "text",
                "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
            })
        else:
            content.append({
                "type": "text",
                "text": f"Load D64 failed: {result['error']}"
            })
    
    elif tool_name == "sysop_list_d64":
        file_path = arguments.get("file_path", "")
        result = tool_sysop_list_d64(file_path)
        
        if result["success"]:
            if result["file_count"] > 0:
                dir_text = f"Directory of {result['disk_name']} ({result['file_count']} files):\n\n"
                for file_info in result["files"]:
                    dir_text += f"{file_info['blocks']:3d} blocks  "
                    dir_text += f"\"{file_info['filename']:<16s}\" "
                    dir_text += f"{file_info['type']}\n"
                content.append({
                    "type": "text",
                    "text": dir_text.rstrip()
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"No files found in {result['disk_name']}"
                })
        else:
            content.append({
                "type": "text",
                "text": f"List D64 failed: {result['error']}"
            })
    
    elif tool_name == "sysop_run":
        result = tool_sysop_run()
        
        if result["success"]:
            content.append({
                "type": "text",
                "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
            })
        else:
            content.append({
                "type": "text",
                "text": f"Run failed: {result['error']}"
            })
    
    elif tool_name == "sysop_display_message":
        message = arguments.get("message", "")
        result = tool_sysop_display_message(message)
        
        if result["success"]:
            content.append({
                "type": "text",
                "text": f"{result['message']}: \"{result['text']}\""
            })
        else:
            content.append({
                "type": "text",
                "text": f"Display message failed: {result['error']}"
            })
    
    elif tool_name == "sidwizard_set_pattern_data":
        index = arguments.get("index")
        hex_bytes = arguments.get("hex_bytes", "")
        
        if index is not None:
            result = tool_sidwizard_set_pattern_data(int(index), hex_bytes)
            
            if result["success"]:
                content.append({
                    "type": "text",
                    "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"SID Wizard set pattern data failed: {result['error']}"
                })
    
    elif tool_name == "sidwizard_load_inst":
        instrument_number = arguments.get("instrument_number")
        hex_bytes = arguments.get("hex_bytes", "")
        
        if instrument_number is not None:
            result = tool_sidwizard_load_inst(int(instrument_number), hex_bytes)
            
            if result["success"]:
                content.append({
                    "type": "text",
                    "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"SID Wizard load instrument failed: {result['error']}"
                })
    
    elif tool_name == "sidwizard_get_inst":
        instrument_number = arguments.get("instrument_number")
        
        if instrument_number is not None:
            result = tool_sidwizard_get_inst(int(instrument_number), "hex")
            
            if result["success"]:
                content.append({
                    "type": "text",
                    "text": f"{result['message']}\n\n{result['data']}"
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"SID Wizard get instrument failed: {result['error']}"
                })
    
    elif tool_name == "sidwizard_get_order_list":
        voice = arguments.get("voice")
        
        if voice is not None:
            result = tool_sidwizard_get_order_list(int(voice))
            
            if result["success"]:
                content.append({
                    "type": "text",
                    "text": f"{result['message']}\n\n{result['data']}"
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"SID Wizard get order list failed: {result['error']}"
                })
    
    elif tool_name == "sidwizard_set_order_list":
        voice = arguments.get("voice")
        hex_bytes = arguments.get("hex_bytes", "")
        
        if voice is not None:
            result = tool_sidwizard_set_order_list(int(voice), hex_bytes)
            
            if result["success"]:
                content.append({
                    "type": "text",
                    "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"SID Wizard set order list failed: {result['error']}"
                })
    
    elif tool_name == "sidwizard_playback":
        action = arguments.get("action", "")
        result = tool_sidwizard_playback(action)

        if result["success"]:
            content.append({
                "type": "text",
                "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
            })
        else:
            content.append({
                "type": "text",
                "text": f"SID Wizard playback command failed: {result['error']}"
            })
    elif tool_name == "sidwizard_clear_song":
        result = tool_sidwizard_clear_song()
        
        if result["success"]:
            content.append({
                "type": "text",
                "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
            })
        else:
            content.append({
                "type": "text",
                "text": f"SID Wizard clear song failed: {result['error']}"
            })
    
    elif tool_name == "sysop_screen_info":
        result = tool_sysop_screen_info()
        
        if result["success"]:
            content.append({
                "type": "text",
                "text": f"{result['message']}\n\n{result['screen_info']}"
            })
        else:
            content.append({
                "type": "text",
                "text": f"Screen info failed: {result['error']}"
            })
    
    elif tool_name == "sysop_disassemble":
        start_addr = arguments.get("start_addr", "")
        end_addr = arguments.get("end_addr", "")
        result = tool_sysop_disassemble(start_addr, end_addr)
        
        if result["success"]:
            disasm_text = f"{result['message']}\n\n"
            disasm_text += "Disassembly:\n"
            disasm_text += "=" * 70 + "\n"
            disasm_text += result['disassembly']
            disasm_text += "\n" + "=" * 70 + "\n"
            disasm_text += f"\n{result['instructions']}"
            content.append({
                "type": "text",
                "text": disasm_text
            })
        else:
            content.append({
                "type": "text",
                "text": f"Disassemble failed: {result['error']}"
            })
    
    elif tool_name == "sysop_bypass_keypress":
        pattern = arguments.get("pattern", "all")
        result = tool_sysop_bypass_keypress(pattern)
        
        if result["success"]:
            content.append({
                "type": "text",
                "text": f"{result['message']}\n\nOutput: {result.get('stdout', '')}"
            })
        else:
            content.append({
                "type": "text",
                "text": f"Bypass keypress failed: {result['error']}"
            })
    
    elif tool_name == "sysop_get_vic_info":
        result = tool_sysop_get_vic_info()
        
        if result["success"]:
            info_text = f"VIC-II Chip Information:\n\n"
            info_text += f"Chip Model: {result['chip_name']} (ID: {result['chip_model']})\n"
            info_text += f"Video Standard: {result['video_standard']}\n"
            info_text += f"Is PAL: {result['is_pal']}\n"
            info_text += f"Is NTSC: {result['is_ntsc']}"
            content.append({
                "type": "text",
                "text": info_text
            })
        else:
            content.append({
                "type": "text",
                "text": f"Get VIC info failed: {result['error']}"
            })
    
    elif tool_name == "sysop_video_standard":
        result = tool_sysop_video_standard()
        
        if result["success"]:
            standard_text = f"Video Standard: {result['video_standard']}\n"
            standard_text += f"PAL: {result['is_pal']}\n"
            standard_text += f"NTSC: {result['is_ntsc']}"
            content.append({
                "type": "text",
                "text": standard_text
            })
        else:
            content.append({
                "type": "text",
                "text": f"Get video standard failed: {result['error']}"
            })
    
    elif tool_name == "sysop_get_palette_entry":
        index = arguments.get("index")
        if index is not None:
            result = tool_sysop_get_palette_entry(int(index))
            
            if result["success"]:
                palette_text = f"Palette Entry {result['index']}:\n"
                palette_text += f"RGB: ({result['r']}, {result['g']}, {result['b']})\n"
                palette_text += f"Hex: {result['hex']}"
                content.append({
                    "type": "text",
                    "text": palette_text
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"Get palette entry failed: {result['error']}"
                })
    
    elif tool_name == "sysop_get_color_palette":
        result = tool_sysop_get_color_palette()
        
        if result["success"]:
            content.append({
                "type": "text",
                "text": json.dumps(result, indent=2)
            })
        else:
            content.append({
                "type": "text",
                "text": json.dumps(result, indent=2)
            })
    
    elif tool_name == "sysop_set_palette_entry":
        index = arguments.get("index")
        r = arguments.get("r")
        g = arguments.get("g")
        b = arguments.get("b")
        
        if index is not None and r is not None and g is not None and b is not None:
            result = tool_sysop_set_palette_entry(int(index), int(r), int(g), int(b))
            
            if result["success"]:
                content.append({
                    "type": "text",
                    "text": f"{result['message']}\nHex: {result['hex']}"
                })
            else:
                content.append({
                    "type": "text",
                    "text": f"Set palette entry failed: {result['error']}"
                })
    
    return {"content": content}

def handle_mcp_request(request_data: dict):
    """Handle MCP JSON-RPC request"""
    method = request_data.get("method", "")
    
    # Handle notifications (no response needed)
    if method.startswith("notifications/"):
        print(f"Received notification: {method}", file=sys.stderr)
        return None
    
    response = {"jsonrpc": "2.0"}
    
    if "id" in request_data:
        response["id"] = request_data["id"]
    
    if method == "initialize":
        response["result"] = handle_initialize()
    elif method == "tools/list":
        response["result"] = handle_list_tools()
    elif method == "tools/call":
        params = request_data.get("params", {})
        tool_name = params.get("name", "")
        arguments = params.get("arguments", {})
        response["result"] = handle_call_tool(tool_name, arguments)
    else:
        response["error"] = {
            "code": -32601,
            "message": "Method not found"
        }
    
    return response

class MCPRequestHandler(BaseHTTPRequestHandler):
    """HTTP request handler for MCP server"""
    
    def log_message(self, format, *args):
        """Override to use stderr"""
        print(f"{self.address_string()} - {format % args}", file=sys.stderr)
    
    def send_cors_headers(self):
        """Send CORS headers"""
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'content-type, authorization, x-requested-with, mcp-protocol-version')
        self.send_header('Access-Control-Max-Age', '86400')
    
    def do_OPTIONS(self):
        """Handle CORS preflight"""
        self.send_response(204)
        self.send_cors_headers()
        self.end_headers()
    
    def do_GET(self):
        """Handle GET requests - return server status"""
        response_body = json.dumps({
            "name": "sysop-64 MCP Server",
            "version": "1.0",
            "status": "running"
        })
        
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', len(response_body))
        self.send_cors_headers()
        self.end_headers()
        self.wfile.write(response_body.encode('utf-8'))
    
    def do_POST(self):
        """Handle POST requests with JSON-RPC"""
        # Handle Expect: 100-continue
        expect = self.headers.get('Expect', '')
        if expect.lower() == '100-continue':
            self.send_response(100)
            self.end_headers()
        
        # Read the request body
        content_length = int(self.headers.get('Content-Length', 0))
        if content_length > 0:
            body = self.rfile.read(content_length).decode('utf-8')
        else:
            body = ''
        
        print(f"Received request: {body}", file=sys.stderr)
        
        try:
            request_data = json.loads(body)
            response_data = handle_mcp_request(request_data)
            
            # Handle notifications with 204 No Content
            if response_data is None:
                self.send_response(204)
                self.send_cors_headers()
                self.end_headers()
                return
            
            response_body = json.dumps(response_data)
            
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', len(response_body))
            self.send_cors_headers()
            self.end_headers()
            self.wfile.write(response_body.encode('utf-8'))
            
        except json.JSONDecodeError as e:
            error_body = json.dumps({"error": "Invalid JSON"})
            self.send_response(400)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', len(error_body))
            self.send_cors_headers()
            self.end_headers()
            self.wfile.write(error_body.encode('utf-8'))
            print(f"JSON parse error: {e}", file=sys.stderr)

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    """Multi-threaded HTTP server"""
    daemon_threads = True
    
    def __init__(self, *args, **kwargs):
        # Set TCP_NODELAY
        HTTPServer.__init__(self, *args, **kwargs)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

def main():
    """Main server entry point"""
    # Initialize sysop64 if available
    if SYSOP_AVAILABLE:
        try:
            sysop64.sysop_init()
            result = sysop64.sysop_server_connect()
            if result == -1:
                print("Warning: sysop_connect failed", file=sys.stderr)
            else:
                # Set initial border color
                sysop64.sysop_server_dma_lock()
                # sysop64.sysop_poke(0xd020, 0x2)
                sysop64.sysop_server_dma_unlock()
        except Exception as e:
            print(f"Warning: sysop64 initialization failed: {e}", file=sys.stderr)
    
    # Start HTTP server
    PORT = 8080
    server_address = ('', PORT)
    httpd = ThreadedHTTPServer(server_address, MCPRequestHandler)
    
    print(f"MCP Server listening on port {PORT}")
    
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        httpd.shutdown()

if __name__ == '__main__':
    main()


