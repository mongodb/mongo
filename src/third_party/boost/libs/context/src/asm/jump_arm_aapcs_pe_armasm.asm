;/*
;            Copyright Oliver Kowalke 2009.
;   Distributed under the Boost Software License, Version 1.0.
;      (See accompanying file LICENSE_1_0.txt or copy at
;          http://www.boost.org/LICENSE_1_0.txt)
;*/

; *******************************************************
; *                                                     *
; *  -------------------------------------------------  *
; *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
; *  -------------------------------------------------  *
; *  | 0x0 | 0x4 | 0x8 | 0xc | 0x10| 0x14| 0x18| 0x1c|  *
; *  -------------------------------------------------  *
; *  |deall|limit| base|hiddn|  v1 |  v2 |  v3 |  v4 |  *
; *  -------------------------------------------------  *
; *  -------------------------------------------------  *
; *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
; *  -------------------------------------------------  *
; *  | 0x20| 0x24| 0x28| 0x2c| 0x30| 0x34| 0x38| 0x3c|  *
; *  -------------------------------------------------  *
; *  |  v5 |  v6 |  v7 |  v8 |  lr |  pc | FCTX| DATA|  *
; *  -------------------------------------------------  *
; *                                                     *
; *******************************************************

    AREA |.text|, CODE
    ALIGN 4
    EXPORT jump_fcontext

jump_fcontext PROC
    ; save LR as PC
    push {lr}
    ; save hidden,V1-V8,LR
    push {a1,v1-v8,lr}

    ; load TIB to save/restore thread size and limit.
    ; we do not need preserve CPU flag and can use it's arg register
    mrc     p15, #0, v1, c13, c0, #2

    ; save current stack base
    ldr  a5, [v1, #0x04]
    push {a5}
    ; save current stack limit
    ldr  a5, [v1, #0x08]
    push {a5}
    ; save current deallocation stack
    ldr  a5, [v1, #0xe0c]
    push {a5}

    ; store RSP (pointing to context-data) in A1
    mov  a1, sp

    ; restore RSP (pointing to context-data) from A2
    mov  sp, a2

    ; restore deallocation stack
    pop  {a5}
    str  a5, [v1, #0xe0c]
    ; restore stack limit
    pop  {a5}
    str  a5, [v1, #0x08]
    ; restore stack base
    pop  {a5}
    str  a5, [v1, #0x04]

    ; restore hidden,V1-V8,LR
    pop {a4,v1-v8,lr}

    ; return transfer_t from jump
    str  a1, [a4, #0]
    str  a3, [a4, #4]
    ; pass transfer_t as first arg in context function
    ; A1 == FCTX, A2 == DATA
    mov  a2, a3

    ; restore PC
    pop {pc}

    ENDP
    END
