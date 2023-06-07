;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%ifndef _MULTIBINARY_ASM_
%define _MULTIBINARY_ASM_

%ifidn __OUTPUT_FORMAT__, elf32
 %define mbin_def_ptr	dd
 %define mbin_ptr_sz	dword
 %define mbin_rdi	edi
 %define mbin_rsi	esi
 %define mbin_rax	eax
 %define mbin_rbx	ebx
 %define mbin_rcx	ecx
 %define mbin_rdx	edx
%else
 %define mbin_def_ptr	dq
 %define mbin_ptr_sz	qword
 %define mbin_rdi	rdi
 %define mbin_rsi	rsi
 %define mbin_rax	rax
 %define mbin_rbx	rbx
 %define mbin_rcx	rcx
 %define mbin_rdx	rdx
%endif

%ifndef AS_FEATURE_LEVEL
%define AS_FEATURE_LEVEL 4
%endif

;;;;
; multibinary macro:
;   creates the visable entry point that uses HW optimized call pointer
;   creates the init of the HW optimized call pointer
;;;;
%macro mbin_interface 1
	;;;;
	; *_dispatched is defaulted to *_mbinit and replaced on first call.
	; Therefore, *_dispatch_init is only executed on first call.
	;;;;
	section .data
	%1_dispatched:
		mbin_def_ptr	%1_mbinit

	section .text
	mk_global %1, function
	%1_mbinit:
		endbranch
		;;; only called the first time to setup hardware match
		call	%1_dispatch_init
		;;; falls thru to execute the hw optimized code
	%1:
		endbranch
		jmp	mbin_ptr_sz [%1_dispatched]
%endmacro

;;;;;
; mbin_dispatch_init parameters
; Use this function when SSE/00/01 is a minimum requirement
; 1-> function name
; 2-> SSE/00/01 optimized function used as base
; 3-> AVX or AVX/02 opt func
; 4-> AVX2 or AVX/04 opt func
;;;;;
%macro mbin_dispatch_init 4
	section .text
	%1_dispatch_init:
		push	mbin_rsi
		push	mbin_rax
		push	mbin_rbx
		push	mbin_rcx
		push	mbin_rdx
		lea	mbin_rsi, [%2 WRT_OPT] ; Default to SSE 00/01

		mov	eax, 1
		cpuid
		and	ecx, (FLAG_CPUID1_ECX_AVX | FLAG_CPUID1_ECX_OSXSAVE)
		cmp	ecx, (FLAG_CPUID1_ECX_AVX | FLAG_CPUID1_ECX_OSXSAVE)
		lea	mbin_rbx, [%3 WRT_OPT] ; AVX (gen2) opt func
		jne	_%1_init_done ; AVX is not available so end
		mov	mbin_rsi, mbin_rbx

		;; Try for AVX2
		xor	ecx, ecx
		mov	eax, 7
		cpuid
		test	ebx, FLAG_CPUID7_EBX_AVX2
		lea	mbin_rbx, [%4 WRT_OPT] ; AVX (gen4) opt func
		cmovne	mbin_rsi, mbin_rbx

		;; Does it have xmm and ymm support
		xor	ecx, ecx
		xgetbv
		and	eax, FLAG_XGETBV_EAX_XMM_YMM
		cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
		je	_%1_init_done
		lea	mbin_rsi, [%2 WRT_OPT]

	_%1_init_done:
		pop	mbin_rdx
		pop	mbin_rcx
		pop	mbin_rbx
		pop	mbin_rax
		mov	[%1_dispatched], mbin_rsi
		pop	mbin_rsi
		ret
%endmacro

;;;;;
; mbin_dispatch_init2 parameters
;  Cases where only base functions are available
; 1-> function name
; 2-> base function
;;;;;
%macro mbin_dispatch_init2 2
	section .text
	%1_dispatch_init:
		push	mbin_rsi
		lea	mbin_rsi, [%2 WRT_OPT] ; Default
		mov	[%1_dispatched], mbin_rsi
		pop	mbin_rsi
		ret
%endmacro

;;;;;
; mbin_dispatch_init_clmul 3 parameters
; Use this case for CRC which needs both SSE4_1 and CLMUL
; 1-> function name
; 2-> base function
; 3-> SSE4_1 and CLMUL optimized function
; 4-> AVX/02 opt func
; 5-> AVX512/10 opt func
;;;;;
%macro mbin_dispatch_init_clmul 5
	section .text
	%1_dispatch_init:
		push	mbin_rsi
		push	mbin_rax
		push	mbin_rbx
		push	mbin_rcx
		push	mbin_rdx
		push	mbin_rdi
		lea     mbin_rsi, [%2 WRT_OPT] ; Default - use base function

		mov     eax, 1
		cpuid
		mov	ebx, ecx ; save cpuid1.ecx
		test	ecx, FLAG_CPUID1_ECX_SSE4_1
		jz	_%1_init_done
		test    ecx, FLAG_CPUID1_ECX_CLMUL
		jz	_%1_init_done
		lea	mbin_rsi, [%3 WRT_OPT] ; SSE possible so use 00/01 opt

		;; Test for XMM_YMM support/AVX
		test	ecx, FLAG_CPUID1_ECX_OSXSAVE
		je	_%1_init_done
		xor	ecx, ecx
		xgetbv	; xcr -> edx:eax
		mov	edi, eax	  ; save xgetvb.eax

		and	eax, FLAG_XGETBV_EAX_XMM_YMM
		cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
		jne	_%1_init_done
		test	ebx, FLAG_CPUID1_ECX_AVX
		je	_%1_init_done
		lea	mbin_rsi, [%4 WRT_OPT] ; AVX/02 opt

%if AS_FEATURE_LEVEL >= 10
		;; Test for AVX2
		xor	ecx, ecx
		mov	eax, 7
		cpuid
		test	ebx, FLAG_CPUID7_EBX_AVX2
		je	_%1_init_done		; No AVX2 possible

		;; Test for AVX512
		and	edi, FLAG_XGETBV_EAX_ZMM_OPM
		cmp	edi, FLAG_XGETBV_EAX_ZMM_OPM
		jne	_%1_init_done	  ; No AVX512 possible
		and	ebx, FLAGS_CPUID7_EBX_AVX512_G1
		cmp	ebx, FLAGS_CPUID7_EBX_AVX512_G1
		jne	_%1_init_done

		and	ecx, FLAGS_CPUID7_ECX_AVX512_G2
		cmp	ecx, FLAGS_CPUID7_ECX_AVX512_G2
		lea	mbin_rbx, [%5 WRT_OPT] ; AVX512/10 opt
		cmove	mbin_rsi, mbin_rbx
%endif
	_%1_init_done:
		pop	mbin_rdi
		pop	mbin_rdx
		pop	mbin_rcx
		pop	mbin_rbx
		pop	mbin_rax
		mov	[%1_dispatched], mbin_rsi
		pop	mbin_rsi
		ret
%endmacro

;;;;;
; mbin_dispatch_init5 parameters
; 1-> function name
; 2-> base function
; 3-> SSE4_2 or 00/01 optimized function
; 4-> AVX/02 opt func
; 5-> AVX2/04 opt func
;;;;;
%macro mbin_dispatch_init5 5
	section .text
	%1_dispatch_init:
		push	mbin_rsi
		push	mbin_rax
		push	mbin_rbx
		push	mbin_rcx
		push	mbin_rdx
		lea	mbin_rsi, [%2 WRT_OPT] ; Default - use base function

		mov	eax, 1
		cpuid
		; Test for SSE4.2
		test	ecx, FLAG_CPUID1_ECX_SSE4_2
		lea	mbin_rbx, [%3 WRT_OPT] ; SSE opt func
		cmovne	mbin_rsi, mbin_rbx

		and	ecx, (FLAG_CPUID1_ECX_AVX | FLAG_CPUID1_ECX_OSXSAVE)
		cmp	ecx, (FLAG_CPUID1_ECX_AVX | FLAG_CPUID1_ECX_OSXSAVE)
		lea	mbin_rbx, [%4 WRT_OPT] ; AVX (gen2) opt func
		jne	_%1_init_done ; AVX is not available so end
		mov	mbin_rsi, mbin_rbx

		;; Try for AVX2
		xor	ecx, ecx
		mov	eax, 7
		cpuid
		test	ebx, FLAG_CPUID7_EBX_AVX2
		lea	mbin_rbx, [%5 WRT_OPT] ; AVX (gen4) opt func
		cmovne	mbin_rsi, mbin_rbx

		;; Does it have xmm and ymm support
		xor	ecx, ecx
		xgetbv
		and	eax, FLAG_XGETBV_EAX_XMM_YMM
		cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
		je	_%1_init_done
		lea	mbin_rsi, [%3 WRT_OPT]

	_%1_init_done:
		pop	mbin_rdx
		pop	mbin_rcx
		pop	mbin_rbx
		pop	mbin_rax
		mov	[%1_dispatched], mbin_rsi
		pop	mbin_rsi
		ret
%endmacro

%if AS_FEATURE_LEVEL >= 6
;;;;;
; mbin_dispatch_init6 parameters
; 1-> function name
; 2-> base function
; 3-> SSE4_2 or 00/01 optimized function
; 4-> AVX/02 opt func
; 5-> AVX2/04 opt func
; 6-> AVX512/06 opt func
;;;;;
%macro mbin_dispatch_init6 6
	section .text
	%1_dispatch_init:
		push	mbin_rsi
		push	mbin_rax
		push	mbin_rbx
		push	mbin_rcx
		push	mbin_rdx
		push	mbin_rdi
		lea	mbin_rsi, [%2 WRT_OPT] ; Default - use base function

		mov	eax, 1
		cpuid
		mov	ebx, ecx ; save cpuid1.ecx
		test	ecx, FLAG_CPUID1_ECX_SSE4_2
		je	_%1_init_done	  ; Use base function if no SSE4_2
		lea	mbin_rsi, [%3 WRT_OPT] ; SSE possible so use 00/01 opt

		;; Test for XMM_YMM support/AVX
		test	ecx, FLAG_CPUID1_ECX_OSXSAVE
		je	_%1_init_done
		xor	ecx, ecx
		xgetbv	; xcr -> edx:eax
		mov	edi, eax	  ; save xgetvb.eax

		and	eax, FLAG_XGETBV_EAX_XMM_YMM
		cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
		jne	_%1_init_done
		test	ebx, FLAG_CPUID1_ECX_AVX
		je	_%1_init_done
		lea	mbin_rsi, [%4 WRT_OPT] ; AVX/02 opt

		;; Test for AVX2
		xor	ecx, ecx
		mov	eax, 7
		cpuid
		test	ebx, FLAG_CPUID7_EBX_AVX2
		je	_%1_init_done		; No AVX2 possible
		lea	mbin_rsi, [%5 WRT_OPT] 	; AVX2/04 opt func

		;; Test for AVX512
		and	edi, FLAG_XGETBV_EAX_ZMM_OPM
		cmp	edi, FLAG_XGETBV_EAX_ZMM_OPM
		jne	_%1_init_done	  ; No AVX512 possible
		and	ebx, FLAGS_CPUID7_EBX_AVX512_G1
		cmp	ebx, FLAGS_CPUID7_EBX_AVX512_G1
		lea	mbin_rbx, [%6 WRT_OPT] ; AVX512/06 opt
		cmove	mbin_rsi, mbin_rbx

	_%1_init_done:
		pop	mbin_rdi
		pop	mbin_rdx
		pop	mbin_rcx
		pop	mbin_rbx
		pop	mbin_rax
		mov	[%1_dispatched], mbin_rsi
		pop	mbin_rsi
		ret
%endmacro

%else
%macro mbin_dispatch_init6 6
	mbin_dispatch_init5 %1, %2, %3, %4, %5
%endmacro
%endif

%if AS_FEATURE_LEVEL >= 10
;;;;;
; mbin_dispatch_init7 parameters
; 1-> function name
; 2-> base function
; 3-> SSE4_2 or 00/01 optimized function
; 4-> AVX/02 opt func
; 5-> AVX2/04 opt func
; 6-> AVX512/06 opt func
; 7-> AVX512 Update/10 opt func
;;;;;
%macro mbin_dispatch_init7 7
	section .text
	%1_dispatch_init:
		push	mbin_rsi
		push	mbin_rax
		push	mbin_rbx
		push	mbin_rcx
		push	mbin_rdx
		push	mbin_rdi
		lea	mbin_rsi, [%2 WRT_OPT] ; Default - use base function

		mov	eax, 1
		cpuid
		mov	ebx, ecx ; save cpuid1.ecx
		test	ecx, FLAG_CPUID1_ECX_SSE4_2
		je	_%1_init_done	  ; Use base function if no SSE4_2
		lea	mbin_rsi, [%3 WRT_OPT] ; SSE possible so use 00/01 opt

		;; Test for XMM_YMM support/AVX
		test	ecx, FLAG_CPUID1_ECX_OSXSAVE
		je	_%1_init_done
		xor	ecx, ecx
		xgetbv	; xcr -> edx:eax
		mov	edi, eax	  ; save xgetvb.eax

		and	eax, FLAG_XGETBV_EAX_XMM_YMM
		cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
		jne	_%1_init_done
		test	ebx, FLAG_CPUID1_ECX_AVX
		je	_%1_init_done
		lea	mbin_rsi, [%4 WRT_OPT] ; AVX/02 opt

		;; Test for AVX2
		xor	ecx, ecx
		mov	eax, 7
		cpuid
		test	ebx, FLAG_CPUID7_EBX_AVX2
		je	_%1_init_done		; No AVX2 possible
		lea	mbin_rsi, [%5 WRT_OPT] 	; AVX2/04 opt func

		;; Test for AVX512
		and	edi, FLAG_XGETBV_EAX_ZMM_OPM
		cmp	edi, FLAG_XGETBV_EAX_ZMM_OPM
		jne	_%1_init_done	  ; No AVX512 possible
		and	ebx, FLAGS_CPUID7_EBX_AVX512_G1
		cmp	ebx, FLAGS_CPUID7_EBX_AVX512_G1
		lea	mbin_rbx, [%6 WRT_OPT] ; AVX512/06 opt
		cmove	mbin_rsi, mbin_rbx

		and	ecx, FLAGS_CPUID7_ECX_AVX512_G2
		cmp	ecx, FLAGS_CPUID7_ECX_AVX512_G2
		lea	mbin_rbx, [%7 WRT_OPT] ; AVX512/06 opt
		cmove	mbin_rsi, mbin_rbx

	_%1_init_done:
		pop	mbin_rdi
		pop	mbin_rdx
		pop	mbin_rcx
		pop	mbin_rbx
		pop	mbin_rax
		mov	[%1_dispatched], mbin_rsi
		pop	mbin_rsi
		ret
%endmacro
%else
%macro mbin_dispatch_init7 7
	mbin_dispatch_init6 %1, %2, %3, %4, %5, %6
%endmacro
%endif

%endif ; ifndef _MULTIBINARY_ASM_
