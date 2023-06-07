;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "options.asm"
%include "stdmac.asm"

; Assumes m_out_buf is a register
; Clobbers RCX
; code is clobbered
; write_bits_always	m_bits, m_bit_count, code, count, m_out_buf
%macro write_bits 5
%define %%m_bits	%1
%define %%m_bit_count	%2
%define %%code		%3
%define %%count		%4
%define %%m_out_buf	%5

	SHLX	%%code, %%code, %%m_bit_count

	or	%%m_bits, %%code
	add	%%m_bit_count, %%count

	mov	[%%m_out_buf], %%m_bits
	mov	rcx, %%m_bit_count
	shr	rcx, 3			; rcx = bytes
	add	%%m_out_buf, rcx
	shl	rcx, 3			; rcx = bits
	and	%%m_bit_count, 0x7

	SHRX	%%m_bits, %%m_bits, rcx
%endm

%macro write_dword 2
%define %%data %1d
%define %%addr %2
	mov	[%%addr], %%data
	add	%%addr, 4
%endm
