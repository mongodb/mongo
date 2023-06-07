;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;       Function API:
;       UINT32 crc32_ieee_by4(
;               UINT32 init_crc, //initial CRC value, 32 bits
;               const unsigned char *buf, //buffer pointer to calculate CRC on
;               UINT64 len //buffer length in bytes (64-bit data)
;       );
;
;       Authors:
;               Erdinc Ozturk
;               Vinodh Gopal
;               James Guilford
;
;       Reference paper titled "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction"
;       URL: http://download.intel.com/design/intarch/papers/323102.pdf
;

%include "reg_sizes.asm"

%define	fetch_dist	1024

[bits 64]
default rel

section .text

%ifidn __OUTPUT_FORMAT__, win64
	%xdefine        arg1 rcx
	%xdefine        arg2 rdx
	%xdefine        arg3 r8

	%xdefine        arg1_low32 ecx
%else
	%xdefine        arg1 rdi
	%xdefine        arg2 rsi
	%xdefine        arg3 rdx

	%xdefine        arg1_low32 edi
%endif

%ifidn __OUTPUT_FORMAT__, win64
	%define XMM_SAVE 16*2
	%define VARIABLE_OFFSET 16*4+8
%else
	%define VARIABLE_OFFSET 16*2+8
%endif

align 16
mk_global 	crc32_ieee_by4, function
crc32_ieee_by4:
	endbranch

	not arg1_low32

	sub rsp,VARIABLE_OFFSET

%ifidn __OUTPUT_FORMAT__, win64
	; push the xmm registers into the stack to maintain
	movdqa [rsp + XMM_SAVE + 16*0],xmm6
	movdqa [rsp + XMM_SAVE + 16*1],xmm7
%endif

	; check if smaller than 128B
	cmp arg3, 128
	jl _less_than_128



	; load the initial crc value
	movd xmm6, arg1_low32					; initial crc
	; crc value does not need to be byte-reflected, but it needs to be
	; moved to the high part of the register.
	; because data will be byte-reflected and will align with initial
	; crc at correct place.
	pslldq xmm6, 12



	movdqa xmm7, [SHUF_MASK]
	; receive the initial 64B data, xor the initial crc value
	movdqu xmm0, [arg2]
	movdqu xmm1, [arg2+16]
	movdqu xmm2, [arg2+32]
	movdqu xmm3, [arg2+48]



	pshufb xmm0, xmm7
	; XOR the initial_crc value
	pxor xmm0, xmm6
	pshufb xmm1, xmm7
	pshufb xmm2, xmm7
	pshufb xmm3, xmm7

	movdqa xmm6, [rk3]	; k3=2^480 mod POLY << 32
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;we subtract 128 instead of 64 to save one instruction from the loop
	sub	arg3, 128

	; at this section of the code, there is 64*x+y (0<=y<64) bytes of
	; buffer. The _fold_64_B_loop loop will fold 64B at a time until we
	;  have 64+y Bytes of buffer


	; fold 64B at a time. This section of the code folds 4 xmm registers in parallel
_fold_64_B_loop:

	;update the buffer pointer
	add arg2, 64

	prefetchnta [arg2+fetch_dist+0]
	movdqa xmm4, xmm0
	movdqa xmm5, xmm1

	pclmulqdq xmm0, xmm6 , 0x11
	pclmulqdq xmm1, xmm6 , 0x11

	pclmulqdq xmm4, xmm6, 0x0
	pclmulqdq xmm5, xmm6, 0x0

	pxor xmm0, xmm4
   	pxor xmm1, xmm5

	prefetchnta [arg2+fetch_dist+32]
	movdqa xmm4, xmm2
	movdqa xmm5, xmm3

	pclmulqdq xmm2, xmm6, 0x11
	pclmulqdq xmm3, xmm6, 0x11

	pclmulqdq xmm4, xmm6, 0x0
	pclmulqdq xmm5, xmm6, 0x0

	pxor xmm2, xmm4
	pxor xmm3, xmm5

	movdqu xmm4, [arg2]
	movdqu xmm5, [arg2+16]
	pshufb xmm4, xmm7
	pshufb xmm5, xmm7
	pxor xmm0, xmm4
	pxor xmm1, xmm5

	movdqu xmm4, [arg2+32]
	movdqu xmm5, [arg2+48]
	pshufb xmm4, xmm7
	pshufb xmm5, xmm7

	pxor xmm2, xmm4
	pxor xmm3, xmm5

	sub	arg3, 64

	; check if there is another 64B in the buffer to be able to fold
	jge	_fold_64_B_loop
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;


	add arg2, 64
	;at this point, the arg2 is pointing at the last y Bytes of the buffer
	; the 64B of data is in 4 of the xmm registers: xmm0, xmm1, xmm2, xmm3


	movdqa xmm6, [rk1]		;k1

	; fold the 4 xmm registers to 1 xmm register with different constants
	movdqa xmm4, xmm0
	pclmulqdq xmm0, xmm6, 0x11
	pclmulqdq xmm4, xmm6, 0x0
	pxor xmm1, xmm4
	xorps xmm1, xmm0

	movdqa xmm4, xmm1
	pclmulqdq xmm1, xmm6, 0x11
	pclmulqdq xmm4, xmm6, 0x0
	pxor xmm2, xmm4
	xorps xmm2, xmm1

	movdqa xmm4, xmm2
	pclmulqdq xmm2, xmm6, 0x11
	pclmulqdq xmm4, xmm6, 0x0
	pxor xmm3, xmm4
	pxor xmm3, xmm2


	;instead of 64, we add 48 to the loop counter to save 1 instruction from the loop
	; instead of a cmp instruction, we use the negative flag with the jl instruction
	add arg3, 64-16
	jl _final_reduction_for_128

; now we have 16+y bytes left to reduce. 16 Bytes is in register xmm3 and the rest is in memory
; we can fold 16 bytes at a time if y>=16
; continue folding 16B at a time

_16B_reduction_loop:
	movdqa xmm4, xmm3
	pclmulqdq xmm3, xmm6, 0x11
	pclmulqdq xmm4, xmm6, 0x0
	pxor xmm3, xmm4
	movdqu xmm0, [arg2]
	pshufb xmm0, xmm7
	pxor xmm3, xmm0
	add arg2, 16
	sub arg3, 16
	; instead of a cmp instruction, we utilize the flags with the jge instruction
	; equivalent of: cmp arg3, 16-16
	; check if there is any more 16B in the buffer to be able to fold
	jge _16B_reduction_loop

	;now we have 16+z bytes left to reduce, where 0<= z < 16.
	;first, we reduce the data in the xmm3 register



_final_reduction_for_128:
	; check if any more data to fold. If not, compute the CRC of the final 128 bits
	add arg3, 16
	je _128_done

	; here we are getting data that is less than 16 bytes.
	; since we know that there was data before the pointer, we can offset
	; the input pointer before the actual point, to receive exactly 16 bytes.
	; after that the registers need to be adjusted.
_get_last_two_xmms:
	movdqa xmm2, xmm3

	movdqu xmm1, [arg2 - 16 + arg3]
	pshufb xmm1, xmm7

	shl arg3, 4
	lea rax, [pshufb_shf_table + 15*16]
	sub rax, arg3
	movdqu xmm0, [rax]

	pshufb	xmm2, xmm0

	pxor xmm0, [mask3]

	pshufb	xmm3, xmm0

	pblendvb xmm1, xmm2	;xmm0 is implicit

	movdqa xmm2, xmm1

	movdqa xmm4, xmm3
	pclmulqdq xmm3, xmm6, 0x11

	pclmulqdq xmm4, xmm6, 0x0
	pxor xmm3, xmm4
	pxor xmm3, xmm2

_128_done:

	movdqa xmm6, [rk5]
	movdqa xmm0, xmm3

	;64b fold
	pclmulqdq xmm3, xmm6, 0x1
	pslldq xmm0, 8
	pxor xmm3, xmm0

	;32b fold
	movdqa xmm0, xmm3

	pand xmm0, [mask4]

	psrldq xmm3, 12
	pclmulqdq xmm3, xmm6, 0x10
	pxor xmm3, xmm0

	;barrett reduction
_barrett:
	movdqa xmm6, [rk7]
	movdqa xmm0, xmm3
	pclmulqdq xmm3, xmm6, 0x01
	pslldq xmm3, 4
	pclmulqdq xmm3, xmm6, 0x11

	pslldq xmm3, 4
	pxor xmm3, xmm0
	pextrd eax, xmm3,1

_cleanup:
	not eax
%ifidn __OUTPUT_FORMAT__, win64
	movdqa xmm6, [rsp + XMM_SAVE + 16*0]
	movdqa xmm7, [rsp + XMM_SAVE + 16*1]
%endif
	add rsp,VARIABLE_OFFSET


	ret







;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

align 16
_less_than_128:

	;check if there is enough buffer to be able to fold 16B at a time
	cmp arg3, 32
	jl _less_than_32
	movdqa xmm7, [SHUF_MASK]

	;if there is, load the constants
	movdqa xmm6, [rk1]		;k1

	movd xmm0, arg1_low32
	pslldq xmm0, 12
	movdqu xmm3, [arg2]
	pshufb xmm3, xmm7
	pxor xmm3, xmm0


	;update the buffer pointer
	add arg2, 16

	;update the counter. subtract 32 instead of 16 to save one instruction from the loop
	sub arg3, 32

	jmp _16B_reduction_loop


align 16
_less_than_32:
	mov eax, arg1_low32
	test arg3, arg3
	je _cleanup

	movdqa xmm7, [SHUF_MASK]

	movd xmm0, arg1_low32
	pslldq xmm0, 12

	cmp arg3, 16
	je _exact_16_left
	jl _less_than_16_left
	movd xmm0, arg1_low32
	pslldq xmm0, 12
	movdqu xmm3, [arg2]
	pshufb xmm3, xmm7
	pxor xmm3, xmm0
	add arg2, 16
	sub arg3, 16
	movdqa xmm6, [rk1]		;k1
	jmp _get_last_two_xmms


align 16
_less_than_16_left:
	; use stack space to load data less than 16 bytes, zero-out the 16B in memory first.

	pxor xmm1, xmm1
	mov r11, rsp
	movdqa [r11], xmm1



	cmp arg3, 4
	jl _only_less_than_4

	mov r9, arg3


	cmp arg3, 8
	jl _less_than_8_left
	mov rax, [arg2]
	mov [r11], rax
	add r11, 8
	sub arg3, 8
	add arg2, 8
_less_than_8_left:

	cmp arg3, 4
	jl _less_than_4_left
	mov eax, [arg2]
	mov [r11], eax
	add r11, 4
	sub arg3, 4
	add arg2, 4
_less_than_4_left:

	cmp arg3, 2
	jl _less_than_2_left
	mov ax, [arg2]
	mov [r11], ax
	add r11, 2
	sub arg3, 2
	add arg2, 2
_less_than_2_left:
	cmp arg3, 1
	jl _zero_left

	mov al, [arg2]
	mov [r11], al

_zero_left:
	movdqa xmm3, [rsp]
	pshufb xmm3, xmm7
	pxor xmm3, xmm0

	shl r9, 4
	lea rax, [pshufb_shf_table + 15*16]
	sub rax, r9
	movdqu xmm0, [rax]
	pxor xmm0, [mask3]

	pshufb xmm3, xmm0
	jmp _128_done

align 16
_exact_16_left:
	movdqu xmm3, [arg2]
	pshufb xmm3, xmm7
	pxor xmm3, xmm0

	jmp _128_done

_only_less_than_4:
	cmp arg3, 3
	jl _only_less_than_3
	mov al, [arg2]
	mov [r11], al

	mov al, [arg2+1]
	mov [r11+1], al

	mov al, [arg2+2]
	mov [r11+2], al

	movdqa xmm3, [rsp]
	pshufb xmm3, xmm7
	pxor xmm3, xmm0

	psrldq xmm3, 5

	jmp _barrett
_only_less_than_3:
	cmp arg3, 2
	jl _only_less_than_2
	mov al, [arg2]
	mov [r11], al

	mov al, [arg2+1]
	mov [r11+1], al

	movdqa xmm3, [rsp]
	pshufb xmm3, xmm7
	pxor xmm3, xmm0

	psrldq xmm3, 6

	jmp _barrett
_only_less_than_2:
	mov al, [arg2]
	mov [r11], al

	movdqa xmm3, [rsp]
	pshufb xmm3, xmm7
	pxor xmm3, xmm0

	psrldq xmm3, 7

	jmp _barrett
; precomputed constants
section .data

align 16
rk1:
DQ 0xf200aa6600000000
rk2:
DQ 0x17d3315d00000000
rk3:
DQ 0xd3504ec700000000
rk4:
DQ 0x57a8445500000000
rk5:
DQ 0xf200aa6600000000
rk6:
DQ 0x490d678d00000000
rk7:
DQ 0x0000000104d101df
rk8:
DQ 0x0000000104c11db7
mask:
dq 0xFFFFFFFFFFFFFFFF, 0x0000000000000000
mask2:
dq 0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFF
mask3:
dq 0x8080808080808080, 0x8080808080808080
mask4:
dq 0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF
	align 32
pshufb_shf_table:

	dq 0x8887868584838281, 0x008f8e8d8c8b8a89 ; shl 15 (16-1) / shr1

	dq 0x8988878685848382, 0x01008f8e8d8c8b8a ; shl 14 (16-3) / shr2

	dq 0x8a89888786858483, 0x0201008f8e8d8c8b ; shl 13 (16-4) / shr3

	dq 0x8b8a898887868584, 0x030201008f8e8d8c ; shl 12 (16-4) / shr4

	dq 0x8c8b8a8988878685, 0x04030201008f8e8d ; shl 11 (16-5) / shr5

	dq 0x8d8c8b8a89888786, 0x0504030201008f8e ; shl 10 (16-6) / shr6

	dq 0x8e8d8c8b8a898887, 0x060504030201008f ; shl 9  (16-7) / shr7

	dq 0x8f8e8d8c8b8a8988, 0x0706050403020100 ; shl 8  (16-8) / shr8

	dq 0x008f8e8d8c8b8a89, 0x0807060504030201 ; shl 7  (16-9) / shr9

	dq 0x01008f8e8d8c8b8a, 0x0908070605040302 ; shl 6  (16-10) / shr10

	dq 0x0201008f8e8d8c8b, 0x0a09080706050403 ; shl 5  (16-11) / shr11

	dq 0x030201008f8e8d8c, 0x0b0a090807060504 ; shl 4  (16-12) / shr12

	dq 0x04030201008f8e8d, 0x0c0b0a0908070605 ; shl 3  (16-13) / shr13

	dq 0x0504030201008f8e, 0x0d0c0b0a09080706 ; shl 2  (16-14) / shr14

	dq 0x060504030201008f, 0x0e0d0c0b0a090807 ; shl 1  (16-15) / shr15


SHUF_MASK	dq 0x08090A0B0C0D0E0F, 0x0001020304050607

;;;       func             core, ver, snum
slversion crc32_ieee_by4, 05,   02,  0017
