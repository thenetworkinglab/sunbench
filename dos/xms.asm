; Copyright (C) 2026 thenetworkinglab
; SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE)
; xms.asm -- ask an XMS driver (HIMEM.SYS) how much extended memory is free.
;
; Once HIMEM.SYS is loaded it owns all extended memory and makes the
; BIOS's INT 15h AH=88h report 0 KB, so SBDOS asks it directly:
; INT 2Fh AX=4300h (installed? AL=80h), AX=4310h (entry point in ES:BX),
; then a far call with AH=08h: AX = largest free block, DX = total free,
; both in KB.  Watcom calling convention: result in AX, everything else
; preserved.
;
;   unsigned xms_free_kb(void);   0FFFFh = no XMS driver
;
        .8086
        name    xms

XMS_TEXT segment byte public 'CODE'
        assume  cs:XMS_TEXT

xent    dd      0                       ; the driver's entry point

        public  xms_free_kb_
xms_free_kb_ proc far
        push    bx
        push    dx
        push    es
        mov     ax, 4300h
        int     2Fh
        cmp     al, 80h
        jne     none
        mov     ax, 4310h
        int     2Fh
        mov     word ptr cs:xent, bx
        mov     word ptr cs:xent + 2, es
        mov     ah, 08h
        call    dword ptr cs:xent
        mov     ax, dx                  ; total free KB
        jmp     short done
none:
        mov     ax, 0FFFFh
done:
        pop     es
        pop     dx
        pop     bx
        ret
xms_free_kb_ endp

XMS_TEXT ends
        end
