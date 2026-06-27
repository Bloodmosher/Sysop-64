*=$031a
; load these starting at 031a with loadbin

!byte $08, $de ; open
!byte $0b, $de ; close
!byte $0e, $de ; chkin
!byte $11, $de ; chkout
!byte $14, $de ; clrchn
!byte $17, $de ; chrin
!byte $1a, $de ; chrout

!byte $ed, $f6 ; default kernel STOP, check status of stop key

!byte $1d, $de ; getin
!byte $20, $de ; clall

!byte $66, $fe ; unused by c64

!byte $23, $de ; load
!byte $26, $de ; save
