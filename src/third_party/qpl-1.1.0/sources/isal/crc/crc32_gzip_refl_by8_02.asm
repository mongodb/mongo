;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;       Function API:
;       UINT32 crc32_gzip_refl_by8_02(
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
;
;       sample yasm command line:
;       yasm -f x64 -f elf64 -X gnu -g dwarf2 crc32_gzip_refl_by8
;
;       As explained here:
;       http://docs.oracle.com/javase/7/docs/api/java/util/zip/package-summary.html
;       CRC-32 checksum is described in RFC 1952
;       Implementing RFC 1952 CRC:
;       http://www.ietf.org/rfc/rfc1952.txt

%include "reg_sizes.asm"

%define	fetch_dist	1024

[bits 64]
default rel

section .text


%ifidn __OUTPUT_FORMAT__, win64
	%xdefine	arg1 rcx
	%xdefine	arg2 rdx
	%xdefine	arg3 r8

	%xdefine	arg1_low32 ecx
%else
	%xdefine	arg1 rdi
	%xdefine	arg2 rsi
	%xdefine	arg3 rdx

	%xdefine	arg1_low32 edi
%endif

%define TMP 16*0
%ifidn __OUTPUT_FORMAT__, win64
	%define XMM_SAVE 16*2
	%define VARIABLE_OFFSET 16*10+8
%else
	%define VARIABLE_OFFSET 16*2+8
%endif

align 16
mk_global  crc32_gzip_refl_by8_02, function
crc32_gzip_refl_by8_02:
	endbranch
	not		arg1_low32
	sub		rsp, VARIABLE_OFFSET

%ifidn __OUTPUT_FORMAT__, win64
	; push the xmm registers into the stack to maintain
	vmovdqa		[rsp + XMM_SAVE + 16*0], xmm6
	vmovdqa		[rsp + XMM_SAVE + 16*1], xmm7
	vmovdqa		[rsp + XMM_SAVE + 16*2], xmm8
	vmovdqa		[rsp + XMM_SAVE + 16*3], xmm9
	vmovdqa		[rsp + XMM_SAVE + 16*4], xmm10
	vmovdqa		[rsp + XMM_SAVE + 16*5], xmm11
	vmovdqa		[rsp + XMM_SAVE + 16*6], xmm12
	vmovdqa		[rsp + XMM_SAVE + 16*7], xmm13
%endif

	; check if smaller than 256B
	cmp		arg3, 256
	jl		.less_than_256

	; load the initial crc value
	vmovd		xmm10, arg1_low32      ; initial crc

	; receive the initial 64B data, xor the initial crc value
	vmovdqu		xmm0, [arg2+16*0]
	vmovdqu		xmm1, [arg2+16*1]
	vmovdqu		xmm2, [arg2+16*2]
	vmovdqu		xmm3, [arg2+16*3]
	vmovdqu		xmm4, [arg2+16*4]
	vmovdqu		xmm5, [arg2+16*5]
	vmovdqu		xmm6, [arg2+16*6]
	vmovdqu		xmm7, [arg2+16*7]

	; XOR the initial_crc value
	vpxor		xmm0, xmm10
	vmovdqa		xmm10, [rk3]	;xmm10 has rk3 and rk4
					;imm value of pclmulqdq instruction will determine which constant to use
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	; we subtract 256 instead of 128 to save one instruction from the loop
	sub		arg3, 256

	; at this section of the code, there is 128*x+y (0<=y<128) bytes of buffer. The fold_128_B_loop
	; loop will fold 128B at a time until we have 128+y Bytes of buffer

	; fold 128B at a time. This section of the code folds 8 xmm registers in parallel
.fold_128_B_loop:
	add		arg2, 128
	prefetchnta	[arg2+fetch_dist+0]
	vmovdqu		xmm9, [arg2+16*0]
	vmovdqu		xmm12, [arg2+16*1]
	vpclmulqdq	xmm8, xmm0, xmm10, 0x10
	vpclmulqdq	xmm0, xmm0, xmm10 , 0x1
	vpclmulqdq	xmm13, xmm1, xmm10, 0x10
	vpclmulqdq	xmm1, xmm1, xmm10 , 0x1
	vpxor		xmm0, xmm9
	vxorps		xmm0, xmm8
	vpxor		xmm1, xmm12
	vxorps		xmm1, xmm13

	prefetchnta	[arg2+fetch_dist+32]
	vmovdqu		xmm9, [arg2+16*2]
	vmovdqu		xmm12, [arg2+16*3]
	vpclmulqdq	xmm8, xmm2, xmm10, 0x10
	vpclmulqdq	xmm2, xmm2, xmm10 , 0x1
	vpclmulqdq	xmm13, xmm3, xmm10, 0x10
	vpclmulqdq	xmm3, xmm3, xmm10 , 0x1
	vpxor		xmm2, xmm9
	vxorps		xmm2, xmm8
	vpxor		xmm3, xmm12
	vxorps		xmm3, xmm13

	prefetchnta	[arg2+fetch_dist+64]
	vmovdqu		xmm9, [arg2+16*4]
	vmovdqu		xmm12, [arg2+16*5]
	vpclmulqdq	xmm8, xmm4, xmm10, 0x10
	vpclmulqdq	xmm4, xmm4, xmm10 , 0x1
	vpclmulqdq	xmm13, xmm5, xmm10, 0x10
	vpclmulqdq	xmm5, xmm5, xmm10 , 0x1
	vpxor		xmm4, xmm9
	vxorps		xmm4, xmm8
	vpxor		xmm5, xmm12
	vxorps		xmm5, xmm13

	prefetchnta	[arg2+fetch_dist+96]
	vmovdqu		xmm9, [arg2+16*6]
	vmovdqu		xmm12, [arg2+16*7]
	vpclmulqdq	xmm8, xmm6, xmm10, 0x10
	vpclmulqdq	xmm6, xmm6, xmm10 , 0x1
	vpclmulqdq	xmm13, xmm7, xmm10, 0x10
	vpclmulqdq	xmm7, xmm7, xmm10 , 0x1
	vpxor		xmm6, xmm9
	vxorps		xmm6, xmm8
	vpxor		xmm7, xmm12
	vxorps		xmm7, xmm13

	sub		arg3, 128
	jge		.fold_128_B_loop
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

	add		arg2, 128
	; at this point, the buffer pointer is pointing at the last y Bytes of the buffer, where 0 <= y < 128
	; the 128B of folded data is in 8 of the xmm registers: xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7

	; fold the 8 xmm registers to 1 xmm register with different constants
	vmovdqa		xmm10, [rk9]
	vpclmulqdq	xmm8, xmm0, xmm10, 0x1
	vpclmulqdq	xmm0, xmm0, xmm10, 0x10
	vpxor		xmm7, xmm8
	vxorps		xmm7, xmm0

	vmovdqa		xmm10, [rk11]
	vpclmulqdq	xmm8, xmm1, xmm10, 0x1
	vpclmulqdq	xmm1, xmm1, xmm10, 0x10
	vpxor		xmm7, xmm8
	vxorps		xmm7, xmm1

	vmovdqa		xmm10, [rk13]
	vpclmulqdq	xmm8, xmm2, xmm10, 0x1
	vpclmulqdq	xmm2, xmm2, xmm10, 0x10
	vpxor		xmm7, xmm8
	vpxor		xmm7, xmm2

	vmovdqa		xmm10, [rk15]
	vpclmulqdq	xmm8, xmm3, xmm10, 0x1
	vpclmulqdq	xmm3, xmm3, xmm10, 0x10
	vpxor		xmm7, xmm8
	vxorps		xmm7, xmm3

	vmovdqa		xmm10, [rk17]
	vpclmulqdq	xmm8, xmm4, xmm10, 0x1
	vpclmulqdq	xmm4, xmm4, xmm10, 0x10
	vpxor		xmm7, xmm8
	vpxor		xmm7, xmm4

	vmovdqa		xmm10, [rk19]
	vpclmulqdq	xmm8, xmm5, xmm10, 0x1
	vpclmulqdq	xmm5, xmm5, xmm10, 0x10
	vpxor		xmm7, xmm8
	vxorps		xmm7, xmm5

	vmovdqa		xmm10, [rk1]
	vpclmulqdq	xmm8, xmm6, xmm10, 0x1
	vpclmulqdq	xmm6, xmm6, xmm10, 0x10
	vpxor		xmm7, xmm8
	vpxor		xmm7, xmm6


	; instead of 128, we add 128-16 to the loop counter to save 1 instruction from the loop
	; instead of a cmp instruction, we use the negative flag with the jl instruction
	add		arg3, 128-16
	jl		.final_reduction_for_128

	; now we have 16+y bytes left to reduce. 16 Bytes is in register xmm7 and the rest is in memory
	; we can fold 16 bytes at a time if y>=16
	; continue folding 16B at a time

.16B_reduction_loop:
	vpclmulqdq	xmm8, xmm7, xmm10, 0x1
	vpclmulqdq	xmm7, xmm7, xmm10, 0x10
	vpxor		xmm7, xmm8
	vmovdqu		xmm0, [arg2]
	vpxor		xmm7, xmm0
	add		arg2, 16
	sub		arg3, 16
	; instead of a cmp instruction, we utilize the flags with the jge instruction
	; equivalent of: cmp arg3, 16-16
	; check if there is any more 16B in the buffer to be able to fold
	jge		.16B_reduction_loop

	;now we have 16+z bytes left to reduce, where 0<= z < 16.
	;first, we reduce the data in the xmm7 register


.final_reduction_for_128:
	add		arg3, 16
	je		.128_done

	; here we are getting data that is less than 16 bytes.
	; since we know that there was data before the pointer, we can offset
	; the input pointer before the actual point, to receive exactly 16 bytes.
	; after that the registers need to be adjusted.
.get_last_two_xmms:

	vmovdqa		xmm2, xmm7
	vmovdqu		xmm1, [arg2 - 16 + arg3]

	; get rid of the extra data that was loaded before
	; load the shift constant
	lea		rax, [pshufb_shf_table]
	add		rax, arg3
	vmovdqu		xmm0, [rax]

	vpshufb		xmm7, xmm0
	vpxor		xmm0, [mask3]
	vpshufb		xmm2, xmm0

	vpblendvb	xmm2, xmm2, xmm1, xmm0
	;;;;;;;;;;
	vpclmulqdq	xmm8, xmm7, xmm10, 0x1
	vpclmulqdq	xmm7, xmm7, xmm10, 0x10
	vpxor		xmm7, xmm8
	vpxor		xmm7, xmm2

.128_done:
	; compute crc of a 128-bit value
	vmovdqa		xmm10, [rk5]
	vmovdqa		xmm0, xmm7

	;64b fold
	vpclmulqdq	xmm7, xmm10, 0
	vpsrldq		xmm0, 8
	vpxor		xmm7, xmm0

	;32b fold
	vmovdqa		xmm0, xmm7
	vpslldq		xmm7, 4
	vpclmulqdq	xmm7, xmm10, 0x10
	vpxor		xmm7, xmm0


	;barrett reduction
.barrett:
	vpand		xmm7, [mask2]
	vmovdqa		xmm1, xmm7
	vmovdqa		xmm2, xmm7
	vmovdqa		xmm10, [rk7]

	vpclmulqdq	xmm7, xmm10, 0
	vpxor		xmm7, xmm2
	vpand		xmm7, [mask]
	vmovdqa		xmm2, xmm7
	vpclmulqdq	xmm7, xmm10, 0x10
	vpxor		xmm7, xmm2
	vpxor		xmm7, xmm1
	vpextrd		eax, xmm7, 2

.cleanup:
	not		eax


%ifidn __OUTPUT_FORMAT__, win64
	vmovdqa		xmm6, [rsp + XMM_SAVE + 16*0]
	vmovdqa		xmm7, [rsp + XMM_SAVE + 16*1]
	vmovdqa		xmm8, [rsp + XMM_SAVE + 16*2]
	vmovdqa		xmm9, [rsp + XMM_SAVE + 16*3]
	vmovdqa		xmm10, [rsp + XMM_SAVE + 16*4]
	vmovdqa		xmm11, [rsp + XMM_SAVE + 16*5]
	vmovdqa		xmm12, [rsp + XMM_SAVE + 16*6]
	vmovdqa		xmm13, [rsp + XMM_SAVE + 16*7]
%endif
	add		rsp, VARIABLE_OFFSET
	ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

align 16
.less_than_256:

	; check if there is enough buffer to be able to fold 16B at a time
	cmp	arg3, 32
	jl	.less_than_32

	; if there is, load the constants
	vmovdqa	xmm10, [rk1]    ; rk1 and rk2 in xmm10

	vmovd	xmm0, arg1_low32	; get the initial crc value
	vmovdqu	xmm7, [arg2]		; load the plaintext
	vpxor	xmm7, xmm0

	; update the buffer pointer
	add	arg2, 16

	; update the counter. subtract 32 instead of 16 to save one instruction from the loop
	sub	arg3, 32

	jmp	.16B_reduction_loop


align 16
.less_than_32:
	; mov initial crc to the return value. this is necessary for zero-length buffers.
	mov	eax, arg1_low32
	test	arg3, arg3
	je	.cleanup

	vmovd	xmm0, arg1_low32	; get the initial crc value

	cmp	arg3, 16
	je	.exact_16_left
	jl	.less_than_16_left

	vmovdqu	xmm7, [arg2]		; load the plaintext
	vpxor	xmm7, xmm0		; xor the initial crc value
	add	arg2, 16
	sub	arg3, 16
	vmovdqa	xmm10, [rk1]		; rk1 and rk2 in xmm10
	jmp	.get_last_two_xmms

align 16
.less_than_16_left:
	; use stack space to load data less than 16 bytes, zero-out the 16B in memory first.

	vpxor	xmm1, xmm1
	mov	r11, rsp
	vmovdqa	[r11], xmm1

	cmp	arg3, 4
	jl	.only_less_than_4

	; backup the counter value
	mov	r9, arg3
	cmp	arg3, 8
	jl	.less_than_8_left

	; load 8 Bytes
	mov	rax, [arg2]
	mov	[r11], rax
	add	r11, 8
	sub	arg3, 8
	add	arg2, 8
.less_than_8_left:

	cmp	arg3, 4
	jl	.less_than_4_left

	; load 4 Bytes
	mov	eax, [arg2]
	mov	[r11], eax
	add	r11, 4
	sub	arg3, 4
	add	arg2, 4
.less_than_4_left:

	cmp	arg3, 2
	jl	.less_than_2_left

	; load 2 Bytes
	mov	ax, [arg2]
	mov	[r11], ax
	add	r11, 2
	sub	arg3, 2
	add	arg2, 2
.less_than_2_left:
	cmp	arg3, 1
	jl	.zero_left

	; load 1 Byte
	mov	al, [arg2]
	mov	[r11], al

.zero_left:
	vmovdqa	xmm7, [rsp]
	vpxor	xmm7, xmm0	; xor the initial crc value

	lea	rax,[pshufb_shf_table]
	vmovdqu	xmm0, [rax + r9]
	vpshufb	xmm7,xmm0
	jmp	.128_done

align 16
.exact_16_left:
	vmovdqu	xmm7, [arg2]
	vpxor	xmm7, xmm0      ; xor the initial crc value
	jmp	.128_done

.only_less_than_4:
	cmp	arg3, 3
	jl	.only_less_than_3

	; load 3 Bytes
	mov	al, [arg2]
	mov	[r11], al

	mov	al, [arg2+1]
	mov	[r11+1], al

	mov	al, [arg2+2]
	mov	[r11+2], al

	vmovdqa	xmm7, [rsp]
	vpxor	xmm7, xmm0	; xor the initial crc value

	vpslldq	xmm7, 5
	jmp	.barrett

.only_less_than_3:
	cmp	arg3, 2
	jl	.only_less_than_2

	; load 2 Bytes
	mov	al, [arg2]
	mov	[r11], al

	mov	al, [arg2+1]
	mov	[r11+1], al

	vmovdqa	xmm7, [rsp]
	vpxor	xmm7, xmm0	; xor the initial crc value

	vpslldq	xmm7, 6
	jmp	.barrett

.only_less_than_2:
	; load 1 Byte
	mov	al, [arg2]
	mov	[r11], al

	vmovdqa	xmm7, [rsp]
	vpxor	xmm7, xmm0      ; xor the initial crc value

	vpslldq	xmm7, 7
	jmp	.barrett

section .data

; precomputed constants
align 16
rk1:  dq 0x00000000ccaa009e
rk2:  dq 0x00000001751997d0
rk3:  dq 0x000000014a7fe880
rk4:  dq 0x00000001e88ef372
rk5:  dq 0x00000000ccaa009e
rk6:  dq 0x0000000163cd6124
rk7:  dq 0x00000001f7011640
rk8:  dq 0x00000001db710640
rk9:  dq 0x00000001d7cfc6ac
rk10: dq 0x00000001ea89367e
rk11: dq 0x000000018cb44e58
rk12: dq 0x00000000df068dc2
rk13: dq 0x00000000ae0b5394
rk14: dq 0x00000001c7569e54
rk15: dq 0x00000001c6e41596
rk16: dq 0x0000000154442bd4
rk17: dq 0x0000000174359406
rk18: dq 0x000000003db1ecdc
rk19: dq 0x000000015a546366
rk20: dq 0x00000000f1da05aa

mask:  dq     0xFFFFFFFFFFFFFFFF, 0x0000000000000000
mask2: dq     0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFF
mask3: dq     0x8080808080808080, 0x8080808080808080

pshufb_shf_table:
; use these values for shift constants for the pshufb instruction
; different alignments result in values as shown:
;       dq 0x8887868584838281, 0x008f8e8d8c8b8a89 ; shl 15 (16-1) / shr1
;       dq 0x8988878685848382, 0x01008f8e8d8c8b8a ; shl 14 (16-3) / shr2
;       dq 0x8a89888786858483, 0x0201008f8e8d8c8b ; shl 13 (16-4) / shr3
;       dq 0x8b8a898887868584, 0x030201008f8e8d8c ; shl 12 (16-4) / shr4
;       dq 0x8c8b8a8988878685, 0x04030201008f8e8d ; shl 11 (16-5) / shr5
;       dq 0x8d8c8b8a89888786, 0x0504030201008f8e ; shl 10 (16-6) / shr6
;       dq 0x8e8d8c8b8a898887, 0x060504030201008f ; shl 9  (16-7) / shr7
;       dq 0x8f8e8d8c8b8a8988, 0x0706050403020100 ; shl 8  (16-8) / shr8
;       dq 0x008f8e8d8c8b8a89, 0x0807060504030201 ; shl 7  (16-9) / shr9
;       dq 0x01008f8e8d8c8b8a, 0x0908070605040302 ; shl 6  (16-10) / shr10
;       dq 0x0201008f8e8d8c8b, 0x0a09080706050403 ; shl 5  (16-11) / shr11
;       dq 0x030201008f8e8d8c, 0x0b0a090807060504 ; shl 4  (16-12) / shr12
;       dq 0x04030201008f8e8d, 0x0c0b0a0908070605 ; shl 3  (16-13) / shr13
;       dq 0x0504030201008f8e, 0x0d0c0b0a09080706 ; shl 2  (16-14) / shr14
;       dq 0x060504030201008f, 0x0e0d0c0b0a090807 ; shl 1  (16-15) / shr15
dq 0x8786858483828100, 0x8f8e8d8c8b8a8988
dq 0x0706050403020100, 0x000e0d0c0b0a0908
