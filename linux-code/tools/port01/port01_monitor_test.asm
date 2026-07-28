; SPDX-License-Identifier: MIT
; Copyright (c) 2026 Sysop-64 Project
;
; ACME-compatible C64 PRG for exercising SYSOP64 inferred $0001 tracking.
;
; Build:
;   acme -f cbm -o ../build/tools/port01_monitor_test.prg port01/port01_monitor_test.asm
;
; Run on a C64, then start tickstream with --exit-on-01-mismatch to catch
; cases where c64_bus_monitor's inferred $0001 value diverges from CHL.
;
; The test repeats forever.  Each case writes a marker to $D020, performs an
; interesting $0001 operation, then records the expected value on screen.

!cpu 6510

ptr_zp = $fb

* = $0801

!word basic_end
!word 10
!byte $9e
!text "2064"
!byte 0
basic_end:
!word 0

* = $0810

start:
        sei
        lda #$7f
        sta $dc0d
        sta $dd0d
        lda $dc0d
        lda $dd0d

        lda #$1b
        sta $d011
        lda #$08
        sta $d016
        lda #$00
        sta $d020
        sta $d021

        ldx #$00
clear_screen:
        lda #$20
        sta $0400,x
        sta $0500,x
        sta $0600,x
        sta $0700,x
        lda #$01
        sta $d800,x
        sta $d900,x
        sta $da00,x
        sta $db00,x
        inx
        bne clear_screen

main_loop:
        jsr reset_line

        lda #$01
        ldx #<test_direct_sta
        ldy #>test_direct_sta
        jsr run_case

        lda #$02
        ldx #<test_stx_sty
        ldy #>test_stx_sty
        jsr run_case

        lda #$03
        ldx #<test_zp_x_store
        ldy #>test_zp_x_store
        jsr run_case

        lda #$04
        ldx #<test_rmw_direct
        ldy #>test_rmw_direct
        jsr run_case

        lda #$05
        ldx #<test_rmw_zp_x
        ldy #>test_rmw_zp_x
        jsr run_case

        lda #$06
        ldx #<test_stack_to_store
        ldy #>test_stack_to_store
        jsr run_case

        lda #$07
        ldx #<test_load_ora_store
        ldy #>test_load_ora_store
        jsr run_case

        lda #$08
        ldx #<test_abs_store
        ldy #>test_abs_store
        jsr run_case

        lda #$09
        ldx #<test_abs_x_store
        ldy #>test_abs_x_store
        jsr run_case

        lda #$0a
        ldx #<test_indirect_y_store
        ldy #>test_indirect_y_store
        jsr run_case

        lda #$0b
        ldx #<test_irq_sensitive
        ldy #>test_irq_sensitive
        jsr run_case

        lda #$0c
        ldx #<test_rts_resume_store
        ldy #>test_rts_resume_store
        jsr run_case

        lda #$0d
        ldx #<test_rmw_more_indexed
        ldy #>test_rmw_more_indexed
        jsr run_case

        lda #$0e
        ldx #<test_irq_no_port01
        ldy #>test_irq_no_port01
        jsr run_case

        lda #$0f
        ldx #<test_irq_writes_port01
        ldy #>test_irq_writes_port01
        jsr run_case

        lda #$10
        ldx #<test_bit_then_sta
        ldy #>test_bit_then_sta
        jsr run_case

        lda #$11
        ldx #<test_cmp_bne_plus_one
        ldy #>test_cmp_bne_plus_one
        jsr run_case

        lda #$12
        ldx #<test_ddr_inputs_zp_x_store
        ldy #>test_ddr_inputs_zp_x_store
        jsr run_case
        lda #$37
        sta $01
        jmp main_loop

; ---------------------------------------------------------------------------
; A = border marker, X/Y = address of case routine.
; ---------------------------------------------------------------------------
run_case:
        sta marker
        stx case_ptr
        sty case_ptr + 1

        lda marker
        sta $d020
        jsr delay

        jsr call_case

        lda expected_01
        jsr record_result
        jsr delay
        rts

call_case:
        jmp (case_ptr)

record_result:
        ldx screen_index
        sta $0400,x
        lda marker
        sta $d800,x
        inc screen_index
        rts

reset_line:
        lda #$00
        sta screen_index
        rts

delay:
        ldy #$20
delay_outer:
        ldx #$00
delay_inner:
        dex
        bne delay_inner
        dey
        bne delay_outer
        rts

set_expected:
        sta expected_01
        rts

; ---------------------------------------------------------------------------
; Test cases.
; ---------------------------------------------------------------------------

test_direct_sta:
        lda #$34
        sta $01
        jsr set_expected
        rts

test_stx_sty:
        ldx #$35
        stx $01
        txa
        jsr set_expected

        ldy #$37
        sty $01
        tya
        jsr set_expected
        rts

test_zp_x_store:
        lda #$34
        ldx #$01
        sta $00,x        ; actual write target is $0001
        jsr set_expected
        rts

test_rmw_direct:
        lda #$34
        sta $01
        inc $01
        lda #$35
        jsr set_expected
        dec $01
        lda #$34
        jsr set_expected
        rts

test_rmw_zp_x:
        lda #$34
        sta $01
        ldx #$01
        inc $00,x        ; actual write target is $0001
        lda #$35
        jsr set_expected
        dec $00,x        ; actual write target is $0001
        lda #$34
        jsr set_expected
        rts

test_stack_to_store:
        lda #$37
        pha
        lda #$34
        pla
        sta $01
        jsr set_expected
        rts

test_load_ora_store:
        lda #$34
        sta scratch
        lda #$30
        ora scratch
        sta $01
        jsr set_expected
        rts

test_abs_store:
        lda #$35
        sta $0001
        jsr set_expected
        rts

test_abs_x_store:
        lda #$37
        ldx #$01
        sta $0000,x      ; actual write target is $0001
        jsr set_expected
        rts

test_indirect_y_store:
        lda #$00
        sta ptr_zp
        lda #$00
        sta ptr_zp + 1
        lda #$34
        ldy #$01
        sta (ptr_zp),y   ; actual write target is $0001
        jsr set_expected
        rts

test_irq_sensitive:
        cli
        lda #$35
        sta $01
        sei
        jsr set_expected
        rts

test_rts_resume_store:
        lda #$34
        sta $01
        jsr rts_resume_helper
        ldy #$37
        sty $01
        tya
        jsr set_expected
        rts

rts_resume_helper:
        tax
        pla
        pha
        clc
        cli
        sei
        rts

test_rmw_more_indexed:
        lda #$34
        sta $01
        ldx #$01
        asl $00,x        ; actual write target is $0001, value becomes $68
        lda #$68
        jsr set_expected
        lsr $00,x        ; actual write target is $0001, value becomes $34
        lda #$34
        jsr set_expected

        ldx #$01
        inc $0000,x      ; actual write target is $0001, value becomes $35
        lda #$35
        jsr set_expected
        dec $0000,x      ; actual write target is $0001, value becomes $34
        lda #$34
        jsr set_expected
        rts

test_irq_no_port01:
        lda #$37
        sta $01
        jsr set_expected
        ldx #<irq_no_port01_handler
        ldy #>irq_no_port01_handler
        jsr setup_cia_irq
        jsr wait_irq_and_restore
        lda #$37
        jsr set_expected
        rts

test_irq_writes_port01:
        lda #$37
        sta $01
        ldx #<irq_writes_port01_handler
        ldy #>irq_writes_port01_handler
        jsr setup_cia_irq
        jsr wait_irq_and_restore
        lda #$36
        jsr set_expected
        rts

test_bit_then_sta:
        lda #$34
        sta $01
        sta scratch
        bit scratch       ; BIT zp should not shift instruction tracking
        bit bit_abs_data  ; BIT abs should not shift instruction tracking
        lda #$37
        sta $01
        jsr set_expected
        rts

test_cmp_bne_plus_one:
        lda #$38
        sta $01
        lda #$ff
        cmp #$00          ; Z=0, so BNE is taken
        bne cmp_bne_plus_one_taken
        rts               ; skipped; target is exactly this address + 1
cmp_bne_plus_one_taken:
        tax
        lda #$37
        sta $01
        jsr set_expected
        rts

; Reproduces zero-page indexed clearing loops such as:
;   LDA #$00 / LDX #$00 / STA $00,X / INX / STA $00,X
; The first store writes the 6510 port DDR at $0000.  With DDR bits 0-2 set
; as inputs, a following write to the $0001 latch should not drive CHL low.
test_ddr_inputs_zp_x_store:
        lda #$37
        sta $01
        lda #$2f
        sta $00
        lda #$37
        jsr set_expected

        lda #$00
        ldx #$00
        sta $00,x        ; writes $0000 DDR: low port bits become inputs
        lda #$37         ; CHL should still read high while DDR bits are inputs
        jsr set_expected

        lda #$00
        inx
        sta $00,x        ; writes $0001 latch, but CHL should remain high
        lda #$37
        jsr set_expected

        lda #$2f
        sta $00          ; restore normal C64 port DDR
        lda #$37
        sta $01
        jsr set_expected
        rts
setup_cia_irq:
        sei
        stx irq_handler_ptr
        sty irq_handler_ptr + 1
        lda $0314
        sta old_irq_vector
        lda $0315
        sta old_irq_vector + 1
        lda irq_handler_ptr
        sta $0314
        lda irq_handler_ptr + 1
        sta $0315

        lda #$00
        sta irq_seen
        lda #$7f
        sta $dc0d
        lda $dc0d
        lda #$20
        sta $dc04
        lda #$00
        sta $dc05
        lda #$81
        sta $dc0d        ; enable CIA 1 timer A interrupt
        lda #$19
        sta $dc0e        ; force-load, one-shot, start timer A
        cli
        rts

wait_irq_and_restore:
        lda irq_seen
        beq wait_irq_and_restore
        sei
        lda #$7f
        sta $dc0d
        lda #$00
        sta $dc0e
        lda $dc0d
        lda old_irq_vector
        sta $0314
        lda old_irq_vector + 1
        sta $0315
        rts

irq_no_port01_handler:
        lda $dc0d        ; acknowledge CIA 1 interrupt
        inc irq_seen
        jmp $ea31        ; return through KERNAL IRQ epilogue

irq_writes_port01_handler:
        lda $dc0d        ; acknowledge CIA 1 interrupt
        lda #$36         ; keep KERNAL visible before jumping to $EA31
        sta $01
        sta expected_01
        inc irq_seen
        jmp $ea31        ; return through KERNAL IRQ epilogue
; ---------------------------------------------------------------------------
; Data.
; ---------------------------------------------------------------------------

case_ptr:       !word 0
irq_handler_ptr: !word 0
old_irq_vector: !word 0
scratch:        !byte 0
bit_abs_data:   !byte $ff
marker:         !byte 0
expected_01:    !byte 0
screen_index:   !byte 0
irq_seen:       !byte 0
