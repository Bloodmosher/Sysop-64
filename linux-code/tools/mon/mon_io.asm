; Sysop-64
; https://github.com/Bloodmosher/Sysop-64
;
; SPDX-License-Identifier: MIT
; Copyright (c) 2026 Sysop-64 Project

*=$DF00
    SEI
    PHA
    TXA
    PHA
    TYA
    PHA
    TSX
    STX $DFF0
    LDA #$00
    STA $DFF1
    STA $DFF3
    STA $DFF4
    LDA $00
    STA $DFF3
    LDA $01
    STA $DFF4

    ;lda #$06  ; debugging
    ;sta $d020 
next_command:

    LDA $DFF1
    CMP #$01 ; write whatever is in dff2 to $01
    BEQ write_01
    CMP #$02 ; write whatever is in dff2 to stack using TXS
    BEQ set_stack
    CMP #$03 ; read $01, store in dff4
    BEQ read_01
    CMP #$FF
    BEQ exit
    JMP next_command
exit:
    PLA
    TAY
    PLA
    TAX
    PLA
    RTI
write_01:
    lda $dff2
    sta $01
    sta $dff4
    lda #$00
    sta $dff1
    jmp next_command
read_01:
    lda $01
    sta $dff4
    lda #$00
    sta $dff1
    jmp next_command
set_stack
    ldx $dff2
    txs
    STX $DFF0
    lda #$00
    sta $dff1
    jmp next_command 

*=$DFE0
num_breakpoints:
    !byte $00
; up to 5 breakpoints can follow, of the form { addr_lo, addr_hi, opcode }
; use the first entry for single step mode
breakpoints:
    !byte $00, $00, $00
    !byte $00, $00, $00
    !byte $00, $00, $00
    !byte $00, $00, $00
    !byte $00, $00, $00
