;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%ifndef _REG_SIZES_ASM_
%define _REG_SIZES_ASM_

%ifdef QPL_LIB
%define QPL_KBYTE           1024
%define QPL_HISTORY_SIZE    (4*QPL_KBYTE)
%endif

%ifndef AS_FEATURE_LEVEL
%define AS_FEATURE_LEVEL 4
%endif

; Intel® Intelligent Storage Acceleration Library (Intel® ISA-L) component has a
; standalone dispatching logic and the following flags are used for Intel ISA-L only.
; Intel® Query Processing Library (Intel® QPL) has another dispatcher implementation
; in middle-layer.

%define EFLAGS_HAS_CPUID        (1<<21)
%define FLAG_CPUID1_ECX_CLMUL   (1<<1)
%define FLAG_CPUID1_EDX_SSE2    (1<<26)
%define FLAG_CPUID1_ECX_SSE3	(1)
%define FLAG_CPUID1_ECX_SSE4_1  (1<<19)
%define FLAG_CPUID1_ECX_SSE4_2  (1<<20)
%define FLAG_CPUID1_ECX_POPCNT  (1<<23)
%define FLAG_CPUID1_ECX_AESNI   (1<<25)
%define FLAG_CPUID1_ECX_OSXSAVE (1<<27)
%define FLAG_CPUID1_ECX_AVX     (1<<28)
%define FLAG_CPUID1_EBX_AVX2    (1<<5)

%define FLAG_CPUID7_EBX_AVX2           (1<<5)
%define FLAG_CPUID7_EBX_AVX512F        (1<<16)
%define FLAG_CPUID7_EBX_AVX512DQ       (1<<17)
%define FLAG_CPUID7_EBX_AVX512IFMA     (1<<21)
%define FLAG_CPUID7_EBX_AVX512PF       (1<<26)
%define FLAG_CPUID7_EBX_AVX512ER       (1<<27)
%define FLAG_CPUID7_EBX_AVX512CD       (1<<28)
%define FLAG_CPUID7_EBX_AVX512BW       (1<<30)
%define FLAG_CPUID7_EBX_AVX512VL       (1<<31)

%define FLAG_CPUID7_ECX_AVX512VBMI     (1<<1)
%define FLAG_CPUID7_ECX_AVX512VBMI2    (1 << 6)
%define FLAG_CPUID7_ECX_GFNI           (1 << 8)
%define FLAG_CPUID7_ECX_VAES           (1 << 9)
%define FLAG_CPUID7_ECX_VPCLMULQDQ     (1 << 10)
%define FLAG_CPUID7_ECX_VNNI           (1 << 11)
%define FLAG_CPUID7_ECX_BITALG         (1 << 12)
%define FLAG_CPUID7_ECX_VPOPCNTDQ      (1 << 14)

%define FLAGS_CPUID7_EBX_AVX512_G1 (FLAG_CPUID7_EBX_AVX512F | FLAG_CPUID7_EBX_AVX512VL | FLAG_CPUID7_EBX_AVX512BW | FLAG_CPUID7_EBX_AVX512CD | FLAG_CPUID7_EBX_AVX512DQ)
%define FLAGS_CPUID7_ECX_AVX512_G2 (FLAG_CPUID7_ECX_AVX512VBMI2 | FLAG_CPUID7_ECX_GFNI | FLAG_CPUID7_ECX_VAES | FLAG_CPUID7_ECX_VPCLMULQDQ | FLAG_CPUID7_ECX_VNNI | FLAG_CPUID7_ECX_BITALG | FLAG_CPUID7_ECX_VPOPCNTDQ)

%define FLAG_XGETBV_EAX_XMM            (1<<1)
%define FLAG_XGETBV_EAX_YMM            (1<<2)
%define FLAG_XGETBV_EAX_XMM_YMM        0x6
%define FLAG_XGETBV_EAX_ZMM_OPM        0xe0

%define FLAG_CPUID1_EAX_AVOTON     0x000406d0
%define FLAG_CPUID1_EAX_STEP_MASK  0xfffffff0

; define d and w variants for registers

%define	raxd	eax
%define raxw	ax
%define raxb	al

%define	rbxd	ebx
%define rbxw	bx
%define rbxb	bl

%define	rcxd	ecx
%define rcxw	cx
%define rcxb	cl

%define	rdxd	edx
%define rdxw	dx
%define rdxb	dl

%define	rsid	esi
%define rsiw	si
%define rsib	sil

%define	rdid	edi
%define rdiw	di
%define rdib	dil

%define	rbpd	ebp
%define rbpw	bp
%define rbpb	bpl

%define ymm0x xmm0
%define ymm1x xmm1
%define ymm2x xmm2
%define ymm3x xmm3
%define ymm4x xmm4
%define ymm5x xmm5
%define ymm6x xmm6
%define ymm7x xmm7
%define ymm8x xmm8
%define ymm9x xmm9
%define ymm10x xmm10
%define ymm11x xmm11
%define ymm12x xmm12
%define ymm13x xmm13
%define ymm14x xmm14
%define ymm15x xmm15

%define zmm0x xmm0
%define zmm1x xmm1
%define zmm2x xmm2
%define zmm3x xmm3
%define zmm4x xmm4
%define zmm5x xmm5
%define zmm6x xmm6
%define zmm7x xmm7
%define zmm8x xmm8
%define zmm9x xmm9
%define zmm10x xmm10
%define zmm11x xmm11
%define zmm12x xmm12
%define zmm13x xmm13
%define zmm14x xmm14
%define zmm15x xmm15
%define zmm16x xmm16
%define zmm17x xmm17
%define zmm18x xmm18
%define zmm19x xmm19
%define zmm20x xmm20
%define zmm21x xmm21
%define zmm22x xmm22
%define zmm23x xmm23
%define zmm24x xmm24
%define zmm25x xmm25
%define zmm26x xmm26
%define zmm27x xmm27
%define zmm28x xmm28
%define zmm29x xmm29
%define zmm30x xmm30
%define zmm31x xmm31

%define zmm0y ymm0
%define zmm1y ymm1
%define zmm2y ymm2
%define zmm3y ymm3
%define zmm4y ymm4
%define zmm5y ymm5
%define zmm6y ymm6
%define zmm7y ymm7
%define zmm8y ymm8
%define zmm9y ymm9
%define zmm10y ymm10
%define zmm11y ymm11
%define zmm12y ymm12
%define zmm13y ymm13
%define zmm14y ymm14
%define zmm15y ymm15
%define zmm16y ymm16
%define zmm17y ymm17
%define zmm18y ymm18
%define zmm19y ymm19
%define zmm20y ymm20
%define zmm21y ymm21
%define zmm22y ymm22
%define zmm23y ymm23
%define zmm24y ymm24
%define zmm25y ymm25
%define zmm26y ymm26
%define zmm27y ymm27
%define zmm28y ymm28
%define zmm29y ymm29
%define zmm30y ymm30
%define zmm31y ymm31

%define DWORD(reg) reg %+ d
%define WORD(reg)  reg %+ w
%define BYTE(reg)  reg %+ b

%define XWORD(reg) reg %+ x

%ifidn __OUTPUT_FORMAT__,elf32
section .note.GNU-stack noalloc noexec nowrite progbits
section .text
%endif
%ifidn __OUTPUT_FORMAT__,elf64
 %define __x86_64__
section .note.GNU-stack noalloc noexec nowrite progbits
section .text
%endif
%ifidn __OUTPUT_FORMAT__,win64
 %define __x86_64__
%endif
%ifidn __OUTPUT_FORMAT__,macho64
 %define __x86_64__
%endif

%ifdef __x86_64__
 %define endbranch db 0xf3, 0x0f, 0x1e, 0xfa
%else
 %define endbranch db 0xf3, 0x0f, 0x1e, 0xfb
%endif

%ifdef REL_TEXT
 %define WRT_OPT
%elifidn __OUTPUT_FORMAT__, elf64
 %define WRT_OPT        wrt ..plt
%else
 %define WRT_OPT
%endif

%macro mk_global 1-3
  %ifdef __NASM_VER__
    %ifidn __OUTPUT_FORMAT__, macho64
	global %1
    %elifidn __OUTPUT_FORMAT__, win64
	global %1
    %else
	global %1:%2 %3
    %endif
  %else
	global %1:%2 %3
  %endif
%endmacro


; Fixes for nasm lack of MS proc helpers
%ifdef __NASM_VER__
  %ifidn __OUTPUT_FORMAT__, win64
    %macro alloc_stack 1
	sub	rsp, %1
    %endmacro

    %macro proc_frame 1
	%1:
    %endmacro

    %macro save_xmm128 2
	movdqa	[rsp + %2], %1
    %endmacro

    %macro save_reg 2
	mov	[rsp + %2], %1
    %endmacro

    %macro rex_push_reg	1
	push	%1
    %endmacro

    %macro push_reg 1
	push	%1
    %endmacro

    %define end_prolog
  %endif

  %define endproc_frame
%endif

%ifidn __OUTPUT_FORMAT__, macho64
 %define elf64 macho64
 mac_equ equ 1
%endif

%macro slversion 4
	section .text
	global %1_slver_%2%3%4
	global %1_slver
	%1_slver:
	%1_slver_%2%3%4:
		dw 0x%4
		db 0x%3, 0x%2
%endmacro

%endif ; ifndef _REG_SIZES_ASM_
