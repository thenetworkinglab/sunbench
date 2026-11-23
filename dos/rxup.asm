; Copyright (C) 2026 thenetworkinglab
; SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE)
; rxup.asm -- the packet driver's receive upcall for SBDOS (net command).
;
; The packet driver calls this twice for every frame (PC/TCP packet
; driver spec 1.09, section 6.4), at interrupt time and on the driver's
; own stack:
;   AX = 0, CX = length: return ES:DI -> a buffer for the frame, or 0:0
;                        to have the driver drop it;
;   AX = 1:              the copy is done.
; Frames go into a ring of RX_SLOTS fixed-size slots that the C code
; (dos/pktnet.c) polls; this routine touches nothing else, so it needs no
; C runtime, no DGROUP and almost no stack.  The driver saves what it
; needs; all registers but DS and BX (restored here) may change.
;
; Layout of rxq_ (dos/pktnet.c's struct rxq_t must match):
;   word head            slot the driver fills next (written here only)
;   word tail            slot the program reads next (written by C only)
;   word drops           frames refused: ring full, too long, or no ring
;   word frames          frames accepted (wraps)
;   word bseg            segment of the slots (32 KB from DOS, offset 0;
;                        set by pn_open, 0 = none: everything refused)
;   word len[RX_SLOTS]   frame lengths
;
        .8086
        name    rxup

RX_SLOTS equ    16                      ; power of two
RX_SHIFT equ    11
RX_SLOT  equ    2048                    ; 1 shl RX_SHIFT: a whole Ethernet
                                        ; frame, or a 2 KB token ring one

RXDATA  segment para public 'FAR_DATA'
        public  rxq_
rxq_    label   byte
rx_head dw      0
rx_tail dw      0
rx_drop dw      0
rx_cnt  dw      0
rx_bseg dw      0
rx_len  dw      RX_SLOTS dup (0)
RXDATA  ends

RXUP_TEXT segment byte public 'CODE'
        assume  cs:RXUP_TEXT, ds:RXDATA

        public  rx_upcall_
rx_upcall_ proc far
        push    ds
        push    bx
        mov     bx, seg rxq_
        mov     ds, bx
        or      ax, ax
        jnz     copied

        ; --- AX = 0: hand out the head slot, if it is free and fits.
        cmp     rx_bseg, 0
        je      refuse
        cmp     cx, RX_SLOT
        ja      refuse
        mov     ax, rx_head
        inc     ax
        and     ax, RX_SLOTS - 1
        cmp     ax, rx_tail
        je      refuse                  ; full: keep one slot empty
        mov     bx, rx_head
        mov     di, bx
        shl     bx, 1
        mov     rx_len[bx], cx          ; length now; CX may not survive
        mov     cl, RX_SHIFT
        shl     di, cl                  ; slot * RX_SLOT, offset 0 based
        mov     es, rx_bseg
        pop     bx
        pop     ds
        ret

refuse:
        inc     rx_drop
        xor     di, di
        mov     es, di
        pop     bx
        pop     ds
        ret

        ; --- AX = 1: the frame is in the head slot: publish it.
copied:
        mov     ax, rx_head
        inc     ax
        and     ax, RX_SLOTS - 1
        mov     rx_head, ax
        inc     rx_cnt
        pop     bx
        pop     ds
        ret
rx_upcall_ endp

RXUP_TEXT ends
        end
