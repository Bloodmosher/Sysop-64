; SPDX-License-Identifier: MIT
; Copyright (c) 2026 Sysop-64 Project
;
; Sysop-64 tick replay $0001 helper.
;
; Build with:
;   acme -f cbm -o tickreplay_0001_helper.prg tickreplay_0001_helper.asm
;
; The PRG loads at $0801 and contains a BASIC SYS stub that starts the helper
; at $080d.  The replay receiver can patch the immediate operand at
; helper_value before releasing DMA.  In this small test build helper_value is
; $0827.  The CPU then executes STA $01 itself, which is required because
; $0001 is the 6510 processor port.

* = $0801

; 10 SYS 2061
!byte $0b, $08
!byte $0a, $00
!byte $9e
!text "2061"
!byte $00
!byte $00, $00

helper_start:
    sei

    ; Disable CIA interrupt sources, then read the ICRs to clear pending IRQ/NMI
    ; conditions.  $DD0D is especially important because CIA2 can drive NMI.
    lda #$7f
    sta $dc0d
    sta $dd0d
    lda $dc0d
    lda $dd0d

    ; Disable VIC-II interrupts and acknowledge any pending VIC IRQ flags.
    lda #$00
    sta $d01a
    lda #$ff
    sta $d019

helper_loop:
    lda #$37
helper_value = * - 1
    sta $01
    jmp helper_loop
