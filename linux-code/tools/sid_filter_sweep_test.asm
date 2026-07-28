; sid_filter_sweep_test.asm
;
; ACME-compatible C64 PRG for measuring/listening to SID filter cutoff behavior.
; It plays a steady sawtooth on voice 1, routes voice 1 through the SID filter,
; does a few obvious low/high cutoff A/B checks, and then repeatedly sweeps
; the 11-bit cutoff register from 0..2047.
;
; Build:
;   acme -f cbm -o sid_filter_sweep_test.prg sid_filter_sweep_test.asm
;
; Measurement idea:
;   Record the C64 audio output while this runs.  The border color changes with
;   the high cutoff byte, giving a coarse visual cue if you also capture video.
;
; Keys:
;   RUN/STOP is not handled here; reset or stop from your loader/monitor.

* = $0801

; BASIC: 10 SYS2061
!byte $0c,$08,$0a,$00,$9e
!text "2061"
!byte $00,$00,$00

* = $080d

SID       = $d400
BORDER    = $d020
BACKGROUND= $d021

V1_FREQ_LO = SID + $00
V1_FREQ_HI = SID + $01
V1_PW_LO   = SID + $02
V1_PW_HI   = SID + $03
V1_CTRL    = SID + $04
V1_AD      = SID + $05
V1_SR      = SID + $06

FC_LO      = SID + $15
FC_HI      = SID + $16
RES_FILT   = SID + $17
MODE_VOL   = SID + $18

cutlo      = $fb
cuthi      = $fc
abcount    = $fd

start:
    sei
    lda #$00
    sta BORDER
    sta BACKGROUND

    ; Clear all SID registers first so the test starts from a known state.
    ldx #$18
.clear_sid:
    sta SID,x
    dex
    bpl .clear_sid

    ; Voice 1: steady low-ish sawtooth tone, gate on.
    ; Lower notes have more useful harmonics for hearing low-pass changes.
    lda #$80
    sta V1_FREQ_LO
    lda #$08
    sta V1_FREQ_HI

    ; Pulse width is irrelevant for sawtooth, but initialized anyway.
    lda #$00
    sta V1_PW_LO
    lda #$08
    sta V1_PW_HI

    ; Fast attack, no decay. Sustain full, moderate release.
    lda #$00
    sta V1_AD
    lda #$f8
    sta V1_SR

    ; Route voice 1 through filter, resonance maxed for an obvious test.
    ; RES_FILT high nibble = resonance, bit 0 = filter voice 1.
    lda #%11110001
    sta RES_FILT

    ; Low-pass filter on, max volume.
    lda #%00011111
    sta MODE_VOL

    ; Sawtooth + gate.
    lda #%00100001
    sta V1_CTRL

main_loop:
    ; First do a few simple low/high cutoff comparisons.  This should make it
    ; obvious that the filter is engaged before the slower sweep begins.
    lda #$04
    sta abcount
.ab_loop:
    jsr set_cutoff_low
    lda #$02
    sta BORDER
    jsr wait_ab

    jsr set_cutoff_high
    lda #$01
    sta BORDER
    jsr wait_ab

    dec abcount
    bne .ab_loop

    lda #$00
    sta cutlo
    sta cuthi

sweep_loop:
    ; Cutoff is 11 bits.  Low register uses bits 2..0, high register uses bits 10..3.
    lda cutlo
    and #$07
    sta FC_LO
    lda cutlo
    lsr
    lsr
    lsr
    ora cuthi
    sta FC_HI

    ; Visual marker: border follows the upper cutoff nibble.
    lda cuthi
    and #$0f
    sta BORDER

    jsr wait_step

    inc cutlo
    bne sweep_loop
    inc cuthi
    lda cuthi
    cmp #$08
    bne sweep_loop

    jmp main_loop

set_cutoff_low:
    lda #$00
    sta FC_LO
    sta FC_HI
    rts

set_cutoff_high:
    lda #$07
    sta FC_LO
    lda #$ff
    sta FC_HI
    rts

wait_ab:
    ; Longer hold for the audible low/high comparison.
    ldx #$40
.ab_outer:
    ldy #$00
.ab_inner:
    dey
    bne .ab_inner
    dex
    bne .ab_outer
    rts

wait_step:
    ; Rough dwell per cutoff value.  Increase the X value or add another nested
    ; loop if you want a slower sweep for easier FFT/binning.
    ldx #$10
.outer:
    ldy #$00
.inner:
    dey
    bne .inner
    dex
    bne .outer
    rts
