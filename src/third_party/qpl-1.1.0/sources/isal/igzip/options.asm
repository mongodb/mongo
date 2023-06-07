;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

default rel

%ifndef __OPTIONS_ASM__
%define __OPTIONS_ASM__

; Options:dir
; m - reschedule mem reads
; e b - bitbuff style
; t s x - compare style
; h - limit hash updates
; l - use longer huffman table
; f - fix cache read

%ifndef IGZIP_HIST_SIZE
%define IGZIP_HIST_SIZE (32 * 1024)
%endif

%if (IGZIP_HIST_SIZE > (32 * 1024))
%undef IGZIP_HIST_SIZE
%define IGZIP_HIST_SIZE (32 * 1024)
%endif

%ifdef LONGER_HUFFTABLE
%if (IGZIP_HIST_SIZE > 8 * 1024)
%undef IGZIP_HIST_SIZE
%define IGZIP_HIST_SIZE (8 * 1024)
%endif
%endif

; (h) limit hash update
%define LIMIT_HASH_UPDATE

; (f) fix cache read problem
%define FIX_CACHE_READ

%define ISAL_DEF_MAX_HDR_SIZE 328

%ifidn __OUTPUT_FORMAT__, elf64
%ifndef __NASM_VER__
%define WRT_OPT         wrt ..sym
%else
%define WRT_OPT
%endif
%else
%define WRT_OPT
%endif

%endif ; ifndef __OPTIONS_ASM__
