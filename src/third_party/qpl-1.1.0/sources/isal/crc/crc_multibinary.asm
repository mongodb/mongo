;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

default rel
[bits 64]

%include "reg_sizes.asm"

extern crc32_iscsi_00
extern crc32_iscsi_01
extern crc32_iscsi_base

extern crc32_ieee_01
extern crc32_ieee_by4  ;; Optimized for SLM
extern crc32_ieee_02
extern crc32_ieee_base

extern crc16_t10dif_01
extern crc16_t10dif_by4  ;; Optimized for SLM
extern crc16_t10dif_02
extern crc16_t10dif_base

extern crc32_gzip_refl_by8
extern crc32_gzip_refl_by8_02
extern crc32_gzip_refl_base

extern crc16_t10dif_copy_by4
extern crc16_t10dif_copy_by4_02
extern crc16_t10dif_copy_base

%if (AS_FEATURE_LEVEL) >= 10
extern crc32_gzip_refl_by16_10
extern crc32_ieee_by16_10
extern crc32_iscsi_by16_10
extern crc16_t10dif_by16_10
%endif

%include "multibinary.asm"

section .data
;;; *_mbinit are initial values for *_dispatched; is updated on first call.
;;; Therefore, *_dispatch_init is only executed on first call.
crc32_iscsi_dispatched:
	dq	crc32_iscsi_mbinit
crc32_ieee_dispatched:
	dq	crc32_ieee_mbinit
%ifndef QPL_LIB
crc16_t10dif_dispatched:
	dq	crc16_t10dif_mbinit
%endif ; %ifndef QPL_LIB
section .text
;;;;
; crc32_iscsi multibinary function
;;;;
mk_global crc32_iscsi, function
crc32_iscsi_mbinit:
	endbranch
	call	crc32_iscsi_dispatch_init
crc32_iscsi:
	endbranch
	jmp	qword [crc32_iscsi_dispatched]

crc32_iscsi_dispatch_init:
	push 	rax
	push	rbx
	push	rcx
	push	rdx
	push	rsi
	push	rdi
	lea     rsi, [crc32_iscsi_base WRT_OPT] ; Default

	mov	eax, 1
	cpuid
	mov	ebx, ecx ; save cpuid1.ecx
	test    ecx, FLAG_CPUID1_ECX_SSE4_2
	jz      .crc_iscsi_init_done ; use iscsi_base
	lea     rsi, [crc32_iscsi_00 WRT_OPT]
	test    ecx, FLAG_CPUID1_ECX_CLMUL
	jz	.crc_iscsi_init_done ; use ieee_base
	lea	rsi, [crc32_iscsi_01 WRT_OPT]

	;; Test for XMM_YMM support/AVX
	test	ecx, FLAG_CPUID1_ECX_OSXSAVE
	je	.crc_iscsi_init_done
	xor	ecx, ecx
	xgetbv	; xcr -> edx:eax
	mov	edi, eax	  ; save xgetvb.eax

	and	eax, FLAG_XGETBV_EAX_XMM_YMM
	cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
	jne	.crc_iscsi_init_done
	test	ebx, FLAG_CPUID1_ECX_AVX
	je	.crc_iscsi_init_done
	;; AVX/02 opt if available

%if AS_FEATURE_LEVEL >= 10
	;; Test for AVX2
	xor	ecx, ecx
	mov	eax, 7
	cpuid
	test	ebx, FLAG_CPUID7_EBX_AVX2
	je	.crc_iscsi_init_done		; No AVX2 possible

	;; Test for AVX512
	and	edi, FLAG_XGETBV_EAX_ZMM_OPM
	cmp	edi, FLAG_XGETBV_EAX_ZMM_OPM
	jne	.crc_iscsi_init_done	  ; No AVX512 possible
	and	ebx, FLAGS_CPUID7_EBX_AVX512_G1
	cmp	ebx, FLAGS_CPUID7_EBX_AVX512_G1
	jne	.crc_iscsi_init_done

	and	ecx, FLAGS_CPUID7_ECX_AVX512_G2
	cmp	ecx, FLAGS_CPUID7_ECX_AVX512_G2
	lea	rbx, [crc32_iscsi_by16_10 WRT_OPT] ; AVX512/10 opt
	cmove	rsi, rbx
%endif

.crc_iscsi_init_done:
	mov	[crc32_iscsi_dispatched], rsi
	pop	rdi
	pop	rsi
	pop	rdx
	pop	rcx
	pop	rbx
	pop	rax
	ret
;;;;
; crc32_ieee multibinary function
;;;;
mk_global crc32_ieee, function
crc32_ieee_mbinit:
	endbranch
	call	crc32_ieee_dispatch_init
crc32_ieee:
	endbranch
	jmp	qword [crc32_ieee_dispatched]

crc32_ieee_dispatch_init:
	push    rax
	push    rbx
	push    rcx
	push    rdx
	push    rsi
	push	rdi
	lea     rsi, [crc32_ieee_base WRT_OPT] ; Default

	mov     eax, 1
	cpuid
	mov	ebx, ecx ; save cpuid1.ecx
	test    ecx, FLAG_CPUID1_ECX_SSE3
	jz      .crc_ieee_init_done ; use ieee_base
	test    ecx, FLAG_CPUID1_ECX_CLMUL
	jz	.crc_ieee_init_done ; use ieee_base
	lea	rsi, [crc32_ieee_01 WRT_OPT]

	;; Extra Avoton test
	lea	rdx, [crc32_ieee_by4 WRT_OPT]
	and     eax, FLAG_CPUID1_EAX_STEP_MASK
	cmp     eax, FLAG_CPUID1_EAX_AVOTON
	cmove   rsi, rdx

	;; Test for XMM_YMM support/AVX
	test	ecx, FLAG_CPUID1_ECX_OSXSAVE
	je	.crc_ieee_init_done
	xor	ecx, ecx
	xgetbv	; xcr -> edx:eax
	mov	edi, eax	  ; save xgetvb.eax

	and	eax, FLAG_XGETBV_EAX_XMM_YMM
	cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
	jne	.crc_ieee_init_done
	test	ebx, FLAG_CPUID1_ECX_AVX
	je	.crc_ieee_init_done
	lea	rsi, [crc32_ieee_02 WRT_OPT] ; AVX/02 opt

%if AS_FEATURE_LEVEL >= 10
	;; Test for AVX2
	xor	ecx, ecx
	mov	eax, 7
	cpuid
	test	ebx, FLAG_CPUID7_EBX_AVX2
	je	.crc_ieee_init_done		; No AVX2 possible

	;; Test for AVX512
	and	edi, FLAG_XGETBV_EAX_ZMM_OPM
	cmp	edi, FLAG_XGETBV_EAX_ZMM_OPM
	jne	.crc_ieee_init_done	  ; No AVX512 possible
	and	ebx, FLAGS_CPUID7_EBX_AVX512_G1
	cmp	ebx, FLAGS_CPUID7_EBX_AVX512_G1
	jne	.crc_ieee_init_done

	and	ecx, FLAGS_CPUID7_ECX_AVX512_G2
	cmp	ecx, FLAGS_CPUID7_ECX_AVX512_G2
	lea	rbx, [crc32_ieee_by16_10 WRT_OPT] ; AVX512/10 opt
	cmove	rsi, rbx
%endif

.crc_ieee_init_done:
	mov     [crc32_ieee_dispatched], rsi
	pop	rdi
	pop     rsi
	pop     rdx
	pop     rcx
	pop     rbx
	pop     rax
	ret

%ifndef QPL_LIB
;;;;
; crc16_t10dif multibinary function
;;;;
mk_global crc16_t10dif, function
crc16_t10dif_mbinit:
	endbranch
	call	crc16_t10dif_dispatch_init
crc16_t10dif:
	endbranch
	jmp	qword [crc16_t10dif_dispatched]

crc16_t10dif_dispatch_init:
	push    rax
	push    rbx
	push    rcx
	push    rdx
	push    rsi
	push    rdi
	lea     rsi, [crc16_t10dif_base WRT_OPT] ; Default

	mov     eax, 1
	cpuid
	mov	ebx, ecx ; save cpuid1.ecx
	test    ecx, FLAG_CPUID1_ECX_SSE3
	jz      .t10dif_init_done ; use t10dif_base
	test    ecx, FLAG_CPUID1_ECX_CLMUL
	jz	.t10dif_init_done ; use t10dif_base
	lea	rsi, [crc16_t10dif_01 WRT_OPT]

	;; Extra Avoton test
	lea	rdx, [crc16_t10dif_by4 WRT_OPT]
	and     eax, FLAG_CPUID1_EAX_STEP_MASK
	cmp     eax, FLAG_CPUID1_EAX_AVOTON
	cmove   rsi, rdx

	;; Test for XMM_YMM support/AVX
	test	ecx, FLAG_CPUID1_ECX_OSXSAVE
	je	.t10dif_init_done
	xor	ecx, ecx
	xgetbv	; xcr -> edx:eax
	mov	edi, eax	  ; save xgetvb.eax

	and	eax, FLAG_XGETBV_EAX_XMM_YMM
	cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
	jne	.t10dif_init_done
	test	ebx, FLAG_CPUID1_ECX_AVX
	je	.t10dif_init_done
	lea	rsi, [crc16_t10dif_02 WRT_OPT] ; AVX/02 opt

%if AS_FEATURE_LEVEL >= 10
	;; Test for AVX2
	xor	ecx, ecx
	mov	eax, 7
	cpuid
	test	ebx, FLAG_CPUID7_EBX_AVX2
	je	.t10dif_init_done		; No AVX2 possible

	;; Test for AVX512
	and	edi, FLAG_XGETBV_EAX_ZMM_OPM
	cmp	edi, FLAG_XGETBV_EAX_ZMM_OPM
	jne	.t10dif_init_done	  ; No AVX512 possible
	and	ebx, FLAGS_CPUID7_EBX_AVX512_G1
	cmp	ebx, FLAGS_CPUID7_EBX_AVX512_G1
	jne	.t10dif_init_done

	and	ecx, FLAGS_CPUID7_ECX_AVX512_G2
	cmp	ecx, FLAGS_CPUID7_ECX_AVX512_G2
	lea	rbx, [crc16_t10dif_by16_10 WRT_OPT] ; AVX512/10 opt
	cmove	rsi, rbx
%endif

.t10dif_init_done:
	mov     [crc16_t10dif_dispatched], rsi
	pop     rdi
	pop     rsi
	pop     rdx
	pop     rcx
	pop     rbx
	pop     rax
	ret
%endif ; %ifndef QPL_LIB
mbin_interface			crc32_gzip_refl
mbin_dispatch_init_clmul	crc32_gzip_refl, crc32_gzip_refl_base, crc32_gzip_refl_by8, crc32_gzip_refl_by8_02, crc32_gzip_refl_by16_10
%ifndef QPL_LIB
mbin_interface			crc16_t10dif_copy
mbin_dispatch_init_clmul	crc16_t10dif_copy, crc16_t10dif_copy_base, crc16_t10dif_copy_by4, crc16_t10dif_copy_by4_02, crc16_t10dif_copy_by4_02
%endif; %ifndef QPL_LIB
;;;       func            	core, ver, snum
slversion crc16_t10dif,		00,   03,  011a
slversion crc32_ieee,		00,   03,  011b
slversion crc32_iscsi,		00,   03,  011c
slversion crc32_gzip_refl,		00,   00,  002a
