*=$DE00
!byte $00, $00, $00, $00
!byte $00, $00, $00, $00 ; CPU A, X, Y, Status as needed by handlers

CMD_REQUESTED=$DE00
CMD_STATUS=$DE01
DEBUG_BYTE=$DE02
IO_STATUS_RETURN=$DE03
CPU_A=$DE04
CPU_X=$DE05
CPU_Y=$DE06
CPU_STATUS=$DE07
; $DE29: up to 4 virtual device numbers (unused slots = $FF)
DEVICE_LIST=$DE29

*=$DE08
jmp open
jmp close
jmp chkin
jmp chkout
jmp clrchn
jmp chrin
jmp chrout
jmp getin
jmp clall
jmp load
jmp save

; up to 4 virtual device numbers; $FF = unused slot
device_list:
    !byte $ff, $ff, $ff, $ff

open
    pha
    lda $BA
    jsr is_our_device_a
    pla
    bcc kernel_open
handle_open
    stx CPU_X
    sty CPU_Y
    sta CPU_A

    lda #$09
    cmp $98
    bcc kernel_open

    sta CMD_STATUS
    lda #$01
    sta CMD_REQUESTED
    jmp wait_for_command_completion
kernel_open
    jmp $f34a

close
    sta CPU_A
    lda $BA
    jsr is_our_device_a
    bcc kernel_close
handle_close
    lda #$02
    sta CMD_STATUS
    sta CMD_REQUESTED
    jmp wait_for_command_completion
kernel_close
    jmp $f291

chkin
    sta CPU_A
    stx CPU_X
    lda #$00
    sta CMD_STATUS
    lda #$03
    sta CMD_REQUESTED
-   lda CMD_STATUS
    beq -
    cmp #$02
    beq use_kernal_chkin

    jmp save_status_and_return
use_kernal_chkin
    ldx CPU_X
    lda CPU_A
    ldy CPU_Y
    jmp $f20e

chkout
    sta CPU_A
    stx CPU_X
    sty CPU_Y
    lda #$00
    sta CMD_STATUS
    lda #$04
    sta CMD_REQUESTED
-
    lda CMD_STATUS
    beq -
    cmp #$02
    beq use_kernal_chkout
    jmp save_status_and_return
use_kernal_chkout
    ldx CPU_X
    lda CPU_A
    ldy CPU_Y
    jmp $f250

; chkin/chkout/chrout return here — does NOT restore A from CPU_A
save_status_and_return
    lda IO_STATUS_RETURN
    sta $90
    clc
    rts

; chrin/getin/clrchn return here — restores data byte from CPU_A into A
save_status_and_return_a
    lda IO_STATUS_RETURN
    sta $90
    lda CPU_A
    clc
    rts

clrchn
    sta CPU_A
    lda $9A
    jsr is_our_device_a
    bcs handle_clrchn
    lda $99
    jsr is_our_device_a
    bcs handle_clrchn
kernel_clrchn
    lda CPU_A
    jmp $F333
handle_clrchn
    lda #$05
    sta CMD_STATUS
    sta CMD_REQUESTED
-   lda CMD_STATUS
    cmp #$00
    bne -
    jmp save_status_and_return_a

*=$DF00
!byte $00, $00, $00, $00
!byte $00, $00, $00, $00
!byte $00, $00, $00

*=$DF0B
chrin
    sta CPU_A
    lda $99
    jsr is_our_device_a
    bcc kernel_chrin
handle_chrin
    lda #$01
    sta CMD_STATUS
    lda #$06
    sta CMD_REQUESTED
-   lda CMD_STATUS
    bne -

    jmp save_status_and_return_a
kernel_chrin
    lda CPU_A
    jmp $f157

chrout
    sta CPU_A
    stx CPU_X
    sty CPU_Y
    lda $9A
    jsr is_our_device_a
    bcc kernal_chrout

    lda #$07
    sta CMD_STATUS
    sta CMD_REQUESTED
-   lda CMD_STATUS
    bne -
    jmp save_status_and_return

kernal_chrout
    lda CPU_A
    ldx CPU_X
    ldy CPU_Y
    jmp $f1ca

getin
    sta CPU_A
    lda $99
    jsr is_our_device_a
    bcc kernel_getin
handle_getin
    lda #$01
    sta CMD_STATUS
    lda #$08
    sta CMD_REQUESTED
-   lda CMD_STATUS
    bne -

    jmp save_status_and_return_a
kernel_getin
    lda CPU_A
    jmp $f13e

clall
    lda #$09
    sta DEBUG_BYTE
    jmp $f32f

load
    sta CPU_A
    stx CPU_X
    sty CPU_Y
    lda $ba
    jsr is_our_device_a
    bcc kernel_load
handle_load
    stx $97
    sty $a4

    jsr $f5af
    jsr $f5d2
    lda #$01
    sta CMD_STATUS
    lda #$0b
    sta CMD_REQUESTED
wait_for_command_completion
    lda CMD_STATUS
    bne wait_for_command_completion

    lda IO_STATUS_RETURN
    sta $90
    clc
    ldx $AE
    ldy $AF
    rts
kernel_load
    lda CPU_A
    jmp $f4a7

save
    sta CPU_A
    lda $ba
    jsr is_our_device_a
    bcc kernel_save
handle_save
    lda #$0c
    sta CMD_STATUS
    sta CMD_REQUESTED
    jmp wait_for_command_completion
kernel_save
    lda CPU_A
    jmp $f5ed

; is_our_device_a: tests A (device number) against device_list.
; Returns carry SET if matched, carry CLEAR if not.
; Does NOT clobber A, X, or Y.
is_our_device_a
    cmp device_list+0
    beq found_device
    cmp device_list+1
    beq found_device
    cmp device_list+2
    beq found_device
    cmp device_list+3
    beq found_device
    clc
    rts
found_device
    sec
    rts

!if * > $E000 {
    !error "diskio.o exceeds 512 bytes"
}

