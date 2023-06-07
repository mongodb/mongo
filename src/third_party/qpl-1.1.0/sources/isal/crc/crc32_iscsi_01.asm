;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;; ISCSI CRC 32 Implementation with crc32 and pclmulqdq Instruction

%include "reg_sizes.asm"

default rel
%define CONCAT(a,b,c)   a %+ b %+ c

; Define threshold where buffers are considered "small" and routed to more
; efficient "by-1" code. This "by-1" code only handles up to 255 bytes, so
; SMALL_SIZE can be no larger than 256.
%define SMALL_SIZE 200

%if (SMALL_SIZE > 256)
%error SMALL_ SIZE must be <= 256
% error ; needed because '%error' actually generates only a warning
%endif

;;; unsigned int crc32_iscsi_01(unsigned char * buffer, int len, unsigned int crc_init);
;;;
;;;        *buf = rcx
;;;         len = rdx
;;;    crc_init = r8

mk_global  crc32_iscsi_01, function
crc32_iscsi_01:
	endbranch

%ifidn __OUTPUT_FORMAT__, elf64
%define bufp            rdi
%define bufp_dw         edi
%define bufp_w          di
%define bufp_b          dil
%define bufptmp         rcx
%define block_0         rcx
%define block_1         rdx
%define block_2         r11
%define len             rsi
%define len_dw          esi
%define len_w           si
%define len_b           sil
%define crc_init_arg    rdx
%else
%define bufp            rcx
%define bufp_dw         ecx
%define bufp_w          cx
%define bufp_b          cl
%define bufptmp         rdi
%define block_0         rdi
%define block_1         rsi
%define block_2         r11
%define len             rdx
%define len_dw          edx
%define len_w           dx
%define len_b           dl
%endif

%define tmp             rbx
%define crc_init        r8
%define crc_init_dw     r8d
%define crc1            r9
%define crc2            r10

	push	rbx
	push	rdi
	push	rsi

	;; Move crc_init for Linux to a different reg
%ifidn __OUTPUT_FORMAT__, elf64
	mov	crc_init, crc_init_arg
%endif

	;; If len is less than 8 we need to jump to special code to avoid
	;; reading beyond the end of the buffer
	cmp	len, 8
	jb	less_than_8

	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; 1) ALIGN: ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

	mov	bufptmp, bufp       ;; rdi = *buf
	neg	bufp
	and	bufp, 7             ;; calculate the unalignment amount of
				    ;; the address
	je	proc_block          ;; Skip if aligned

	;;;; Calculate CRC of unaligned bytes of the buffer (if any) ;;;
	mov	tmp, [bufptmp]      ;; load a quadword from the buffer
	add	bufptmp, bufp       ;; align buffer pointer for quadword
				    ;; processing
	sub	len, bufp           ;; update buffer length
align_loop:
	crc32	crc_init_dw, bl     ;; compute crc32 of 1-byte
	shr	tmp, 8              ;; get next byte
	dec	bufp
	jne	align_loop

proc_block:

	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; 2) PROCESS  BLOCKS: ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

	;; compute num of bytes to be processed
	mov	tmp, len        ;; save num bytes in tmp

	cmp	len, 128*24
	jae	full_block

continue_block:
	cmp	len, SMALL_SIZE
	jb	small

	;; len < 128*24
	mov	rax, 2731       ;; 2731 = ceil(2^16 / 24)
	mul	len_dw
	shr	rax, 16

	;; eax contains floor(bytes / 24) = num 24-byte chunks to do

	;; process rax 24-byte chunks (128 >= rax >= 0)

	;; compute end address of each block
	;; rdi -> block 0 (base addr + RAX * 8)
	;; rsi -> block 1 (base addr + RAX * 16)
	;; r11 -> block 2 (base addr + RAX * 24)
	lea	block_0, [bufptmp + rax * 8]
	lea	block_1, [block_0 + rax * 8]
	lea	block_2, [block_1 + rax * 8]

	xor	crc1,crc1
	xor	crc2,crc2

	;; branch into array
	lea	bufp, [jump_table]
	movzx	len, word [bufp + rax * 2] ;; len is offset from crc_array
	lea	bufp, [bufp + len + crc_array - jump_table]
	jmp	bufp

	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; 2a) PROCESS FULL BLOCKS: ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
full_block:
	mov	rax, 128
	lea	block_1, [block_0 + 128*8*2]
	lea	block_2, [block_0 + 128*8*3]
	add	block_0, 128*8*1

	xor	crc1,crc1
	xor	crc2,crc2

;       ;; branch into array
;       jmp     CONCAT(crc_,128,)
	; Fall thruogh into top of crc array (crc_128)

	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; 3) CRC Array: ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

crc_array:
	cmp 	len, 128*24*2
	jbe 	non_prefetch

%assign i 128
%rep 128-1

CONCAT(_crc_,i,:)
	crc32	crc_init,  qword [block_0 - i*8]
	crc32	crc1,      qword [block_1 - i*8]
	crc32	crc2,      qword [block_2 - i*8]

 %if i > 128*8 / 32	; prefetch next 3KB data
	prefetchnta [block_2 + 128*32 - i*32]
 %endif

%assign i (i-1)
%endrep
 	jmp next_

non_prefetch:
%assign i 128
%rep 128-1

CONCAT(crc_,i,:)
	endbranch
	crc32	crc_init,  qword [block_0 - i*8]
	crc32	crc1,      qword [block_1 - i*8]
	crc32	crc2,      qword [block_2 - i*8]
%assign i (i-1)
%endrep

next_:
CONCAT(crc_,i,:)
	crc32	crc_init,  qword [block_0 - i*8]
	crc32	crc1,      qword [block_1 - i*8]
; SKIP  ;crc32  crc2,      [block_2 - i*8] ; Don't do this one yet

	mov	block_0, block_2

	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; 4) Combine three results: ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

	lea	bufp, [K_table - 16]    ; first entry is for idx 1
	shl	rax, 3                          ; rax *= 8
	sub	tmp, rax                        ; tmp -= rax*8
	shl	rax, 1
	sub	tmp, rax        ; tmp -= rax*16 (total tmp -= rax*24)
	add	bufp, rax

	movdqa	xmm0, [bufp]                    ; 2 consts: K1:K2

	movq	xmm1, crc_init                  ; CRC for block 1
	pclmulqdq	xmm1, xmm0, 0x00        ; Multiply by K2

	movq	xmm2, crc1                      ; CRC for block 2
	pclmulqdq	xmm2, xmm0, 0x10        ; Multiply by K1

	pxor	xmm1, xmm2
	movq	rax, xmm1
	xor	rax, [block_2 - i*8]
	mov	crc_init, crc2
	crc32	crc_init, rax

	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; 5) Check for end: ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

CONCAT(crc_,0,:)
	mov	len, tmp
	cmp	tmp, 128*24
	jae	full_block
	cmp	tmp, 24
	jae	continue_block

fewer_than_24:
	;; now fewer than 24 bytes remain
	cmp	tmp, 16
	jae	do_16
	cmp	tmp, 8
	jae	do_8

	;; 0 <= tmp <= 7
	shl	ebx, 29        ; size now in bits 31:29
	jz	do_return
check_4:
	mov	bufp, [bufptmp]
	shl	ebx, 1         ; shift out into carry MSB (orig size & 4)
	jnc	check_2
	crc32	crc_init_dw, bufp_dw
	jz	do_return
	shr	bufp, 32       ; shift data down by 4 bytes
check_2:
	shl	ebx, 1         ; shift out into carry MSB (orig size & 2)
	jnc	check_1
	crc32	crc_init_dw, bufp_w
	jz	do_return
	shr	bufp, 16       ; shift data down by 2 bytes
check_1:
	crc32	crc_init_dw, bufp_b

do_return:
	mov	rax, crc_init
	pop	rsi
	pop	rdi
	pop	rbx
	ret

do_8:
	crc32	crc_init, qword [bufptmp]
	add	bufptmp, 8
	shl	ebx, 29        ; size (0...7) in bits 31:29
	jnz	check_4
	mov	rax, crc_init
	pop	rsi
	pop	rdi
	pop	rbx
	ret

do_16:
	crc32	crc_init, qword [bufptmp]
	crc32	crc_init, qword [bufptmp+8]
	add	bufptmp, 16
	shl	ebx, 29        ; size (0...7) in bits 31:29
	jnz	check_4
	mov	rax, crc_init
	pop	rsi
	pop	rdi
	pop	rbx
	ret




	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; Handle the case of fewer than 8 bytes, unaligned. In this case
	;; we can't read 8 bytes, as this might go beyond the end of the buffer
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

less_than_8:
	test	len,4
	jz	less_than_4
	crc32	crc_init_dw, dword[bufp]
	add	bufp,4
less_than_4:
	test	len,2
	jz	less_than_2
	crc32	crc_init_dw, word[bufp]
	add	bufp,2
less_than_2:
	test	len,1
	jz	do_return
	crc32	crc_init_dw, byte[bufp]
	mov	rax, crc_init
	pop	rsi
	pop	rdi
	pop	rbx
	ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;4) LESS THAN 256-bytes REMAIN AT THIS POINT (8-bits of len are full)

small:
	mov rax, crc_init

bit8:
	shl len_b, 1               ;; shift-out MSB (bit-7)
	jnc bit7                   ;; jump to bit-6 if bit-7 == 0
 %assign i 0
 %rep 16
	crc32 rax, qword [bufptmp+i] ;; compute crc32 of 8-byte data
	%assign i (i+8)
 %endrep
	je do_return2              ;; return if remaining data is zero
	add bufptmp, 128           ;; buf +=64; (next 64 bytes)

bit7:
	shl len_b, 1               ;; shift-out MSB (bit-7)
	jnc bit6                   ;; jump to bit-6 if bit-7 == 0
 %assign i 0
 %rep 8
	crc32 rax, qword [bufptmp+i] ;; compute crc32 of 8-byte data
	%assign i (i+8)
 %endrep
	je do_return2              ;; return if remaining data is zero
	add bufptmp, 64            ;; buf +=64; (next 64 bytes)
bit6:
	shl len_b, 1               ;; shift-out MSB (bit-6)
	jnc bit5                   ;; jump to bit-5 if bit-6 == 0
 %assign i 0
 %rep 4
	crc32 rax, qword [bufptmp+i] ;;    compute crc32 of 8-byte data
	%assign i (i+8)
 %endrep
	je do_return2              ;; return if remaining data is zero
	add bufptmp, 32            ;; buf +=32; (next 32 bytes)
bit5:
	shl len_b, 1               ;; shift-out MSB (bit-5)
	jnc bit4                   ;; jump to bit-4 if bit-5 == 0
 %assign i 0
 %rep 2
	crc32 rax, qword [bufptmp+i] ;;    compute crc32 of 8-byte data
	%assign i (i+8)
 %endrep
	je do_return2              ;; return if remaining data is zero
	add bufptmp, 16            ;; buf +=16; (next 16 bytes)
bit4:
	shl len_b, 1               ;; shift-out MSB (bit-4)
	jnc bit3                   ;; jump to bit-3 if bit-4 == 0
	crc32 rax, qword [bufptmp] ;; compute crc32 of 8-byte data
	je do_return2              ;; return if remaining data is zero
	add bufptmp, 8             ;; buf +=8; (next 8 bytes)
bit3:
	mov rbx, qword [bufptmp]   ;; load a 8-bytes from the buffer:
	shl len_b, 1               ;; shift-out MSB (bit-3)
	jnc bit2                   ;; jump to bit-2 if bit-3 == 0
	crc32 eax, ebx             ;; compute crc32 of 4-byte data
	je do_return2              ;; return if remaining data is zero
	shr rbx, 32                ;; get next 3 bytes
bit2:
	shl len_b, 1               ;; shift-out MSB (bit-2)
	jnc bit1                   ;; jump to bit-1 if bit-2 == 0
	crc32 eax, bx              ;; compute crc32 of 2-byte data
	je do_return2              ;; return if remaining data is zero
	shr rbx, 16                ;; next byte
bit1:
	test len_b,len_b
	je do_return2
	crc32 eax, bl              ;; compute crc32 of 1-byte data
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

do_return2:
	pop	rsi
	pop	rdi
	pop	rbx
	ret



	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; jump table        ;; Table is 129 entries x 2 bytes each
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
align 4
jump_table:
%assign i 0
%rep 129
	dw	CONCAT(crc_,i,) - crc_array
%assign i (i+1)
%endrep



	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	;; PCLMULQDQ tables
	;; Table is 128 entries x 2 quad words each
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
section .data
align 64
K_table:
	dq 0x14cd00bd6, 0x105ec76f0
	dq 0x0ba4fc28e, 0x14cd00bd6
	dq 0x1d82c63da, 0x0f20c0dfe
	dq 0x09e4addf8, 0x0ba4fc28e
	dq 0x039d3b296, 0x1384aa63a
	dq 0x102f9b8a2, 0x1d82c63da
	dq 0x14237f5e6, 0x01c291d04
	dq 0x00d3b6092, 0x09e4addf8
	dq 0x0c96cfdc0, 0x0740eef02
	dq 0x18266e456, 0x039d3b296
	dq 0x0daece73e, 0x0083a6eec
	dq 0x0ab7aff2a, 0x102f9b8a2
	dq 0x1248ea574, 0x1c1733996
	dq 0x083348832, 0x14237f5e6
	dq 0x12c743124, 0x02ad91c30
	dq 0x0b9e02b86, 0x00d3b6092
	dq 0x018b33a4e, 0x06992cea2
	dq 0x1b331e26a, 0x0c96cfdc0
	dq 0x17d35ba46, 0x07e908048
	dq 0x1bf2e8b8a, 0x18266e456
	dq 0x1a3e0968a, 0x11ed1f9d8
	dq 0x0ce7f39f4, 0x0daece73e
	dq 0x061d82e56, 0x0f1d0f55e
	dq 0x0d270f1a2, 0x0ab7aff2a
	dq 0x1c3f5f66c, 0x0a87ab8a8
	dq 0x12ed0daac, 0x1248ea574
	dq 0x065863b64, 0x08462d800
	dq 0x11eef4f8e, 0x083348832
	dq 0x1ee54f54c, 0x071d111a8
	dq 0x0b3e32c28, 0x12c743124
	dq 0x0064f7f26, 0x0ffd852c6
	dq 0x0dd7e3b0c, 0x0b9e02b86
	dq 0x0f285651c, 0x0dcb17aa4
	dq 0x010746f3c, 0x018b33a4e
	dq 0x1c24afea4, 0x0f37c5aee
	dq 0x0271d9844, 0x1b331e26a
	dq 0x08e766a0c, 0x06051d5a2
	dq 0x093a5f730, 0x17d35ba46
	dq 0x06cb08e5c, 0x11d5ca20e
	dq 0x06b749fb2, 0x1bf2e8b8a
	dq 0x1167f94f2, 0x021f3d99c
	dq 0x0cec3662e, 0x1a3e0968a
	dq 0x19329634a, 0x08f158014
	dq 0x0e6fc4e6a, 0x0ce7f39f4
	dq 0x08227bb8a, 0x1a5e82106
	dq 0x0b0cd4768, 0x061d82e56
	dq 0x13c2b89c4, 0x188815ab2
	dq 0x0d7a4825c, 0x0d270f1a2
	dq 0x10f5ff2ba, 0x105405f3e
	dq 0x00167d312, 0x1c3f5f66c
	dq 0x0f6076544, 0x0e9adf796
	dq 0x026f6a60a, 0x12ed0daac
	dq 0x1a2adb74e, 0x096638b34
	dq 0x19d34af3a, 0x065863b64
	dq 0x049c3cc9c, 0x1e50585a0
	dq 0x068bce87a, 0x11eef4f8e
	dq 0x1524fa6c6, 0x19f1c69dc
	dq 0x16cba8aca, 0x1ee54f54c
	dq 0x042d98888, 0x12913343e
	dq 0x1329d9f7e, 0x0b3e32c28
	dq 0x1b1c69528, 0x088f25a3a
	dq 0x02178513a, 0x0064f7f26
	dq 0x0e0ac139e, 0x04e36f0b0
	dq 0x0170076fa, 0x0dd7e3b0c
	dq 0x141a1a2e2, 0x0bd6f81f8
	dq 0x16ad828b4, 0x0f285651c
	dq 0x041d17b64, 0x19425cbba
	dq 0x1fae1cc66, 0x010746f3c
	dq 0x1a75b4b00, 0x18db37e8a
	dq 0x0f872e54c, 0x1c24afea4
	dq 0x01e41e9fc, 0x04c144932
	dq 0x086d8e4d2, 0x0271d9844
	dq 0x160f7af7a, 0x052148f02
	dq 0x05bb8f1bc, 0x08e766a0c
	dq 0x0a90fd27a, 0x0a3c6f37a
	dq 0x0b3af077a, 0x093a5f730
	dq 0x04984d782, 0x1d22c238e
	dq 0x0ca6ef3ac, 0x06cb08e5c
	dq 0x0234e0b26, 0x063ded06a
	dq 0x1d88abd4a, 0x06b749fb2
	dq 0x04597456a, 0x04d56973c
	dq 0x0e9e28eb4, 0x1167f94f2
	dq 0x07b3ff57a, 0x19385bf2e
	dq 0x0c9c8b782, 0x0cec3662e
	dq 0x13a9cba9e, 0x0e417f38a
	dq 0x093e106a4, 0x19329634a
	dq 0x167001a9c, 0x14e727980
	dq 0x1ddffc5d4, 0x0e6fc4e6a
	dq 0x00df04680, 0x0d104b8fc
	dq 0x02342001e, 0x08227bb8a
	dq 0x00a2a8d7e, 0x05b397730
	dq 0x168763fa6, 0x0b0cd4768
	dq 0x1ed5a407a, 0x0e78eb416
	dq 0x0d2c3ed1a, 0x13c2b89c4
	dq 0x0995a5724, 0x1641378f0
	dq 0x19b1afbc4, 0x0d7a4825c
	dq 0x109ffedc0, 0x08d96551c
	dq 0x0f2271e60, 0x10f5ff2ba
	dq 0x00b0bf8ca, 0x00bf80dd2
	dq 0x123888b7a, 0x00167d312
	dq 0x1e888f7dc, 0x18dcddd1c
	dq 0x002ee03b2, 0x0f6076544
	dq 0x183e8d8fe, 0x06a45d2b2
	dq 0x133d7a042, 0x026f6a60a
	dq 0x116b0f50c, 0x1dd3e10e8
	dq 0x05fabe670, 0x1a2adb74e
	dq 0x130004488, 0x0de87806c
	dq 0x000bcf5f6, 0x19d34af3a
	dq 0x18f0c7078, 0x014338754
	dq 0x017f27698, 0x049c3cc9c
	dq 0x058ca5f00, 0x15e3e77ee
	dq 0x1af900c24, 0x068bce87a
	dq 0x0b5cfca28, 0x0dd07448e
	dq 0x0ded288f8, 0x1524fa6c6
	dq 0x059f229bc, 0x1d8048348
	dq 0x06d390dec, 0x16cba8aca
	dq 0x037170390, 0x0a3e3e02c
	dq 0x06353c1cc, 0x042d98888
	dq 0x0c4584f5c, 0x0d73c7bea
	dq 0x1f16a3418, 0x1329d9f7e
	dq 0x0531377e2, 0x185137662
	dq 0x1d8d9ca7c, 0x1b1c69528
	dq 0x0b25b29f2, 0x18a08b5bc
	dq 0x19fb2a8b0, 0x02178513a
	dq 0x1a08fe6ac, 0x1da758ae0
	dq 0x045cddf4e, 0x0e0ac139e
	dq 0x1a91647f2, 0x169cf9eb0
	dq 0x1a0f717c4, 0x0170076fa

;;;       func            core, ver, snum
slversion crc32_iscsi_01, 01,   04,  0015

