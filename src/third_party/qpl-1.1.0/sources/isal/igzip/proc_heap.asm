;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; returns modified node_ptr
; uint32_t proc_heap(uint64_t *heap, uint32_t  heap_size);

%include "reg_sizes.asm"
%include "heap_macros.asm"

%ifidn __OUTPUT_FORMAT__, win64
%define heap		rcx	; pointer, 64-bit
%define heap_size	rdx
%define	arg3		r8
%define child		rsi
%define tmp32		rdi
%else
%define heap		rdi
%define heap_size	rsi
%define	arg3		rdx
%define child		rcx
%define tmp32		rdx
%endif

%define node_ptr	rax
%define h1		r8
%define h2		r9
%define h3		r10
%define i		r11
%define tmp2		r12

[bits 64]
default rel
section .text

	global build_huff_tree
build_huff_tree:
	endbranch
%ifidn __OUTPUT_FORMAT__, win64
	push	rsi
	push	rdi
%endif
	push	r12

	mov	node_ptr, arg3
.main_loop:
	; REMOVE_MIN64(heap, heap_size, h1);
	mov	h2, [heap + heap_size*8]
	mov	h1, [heap + 1*8]
	mov	qword [heap + heap_size*8], -1
	dec	heap_size
	mov	[heap + 1*8], h2

	mov	i, 1
	heapify	heap, heap_size, i, child, h2, h3, tmp32, tmp2

	mov	h2, [heap + 1*8]
	lea	h3, [h1 + h2]
	mov	[heap + node_ptr*8], h1 %+ w
	mov	[heap + node_ptr*8 - 8], h2 %+ w

	and 	h3, ~0xffff
	or	h3, node_ptr
	sub	node_ptr, 2

	; replace_min64(heap, heap_size, h3)
	mov	[heap + 1*8], h3
	mov	i, 1
	heapify	heap, heap_size, i, child, h2, h3, tmp32, tmp2

	cmp	heap_size, 1
	ja	.main_loop

	mov	h1, [heap + 1*8]
	mov	[heap + node_ptr*8], h1 %+ w

	pop	r12
%ifidn __OUTPUT_FORMAT__, win64
	pop	rdi
	pop	rsi
%endif
	ret

align 32
	global	build_heap
build_heap:
	endbranch
%ifidn __OUTPUT_FORMAT__, win64
	push	rsi
	push	rdi
%endif
	push	r12
	mov	qword [heap + heap_size*8 + 8], -1
	mov	i, heap_size
	shr	i, 1
.loop:
	mov	h1, i
	heapify	heap, heap_size, h1, child, h2, h3, tmp32, tmp2
	dec	i
	jnz	.loop

	pop	r12
%ifidn __OUTPUT_FORMAT__, win64
	pop	rdi
	pop	rsi
%endif
	ret
