;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

default rel
[bits 64]

%include "reg_sizes.asm"

extern isal_deflate_body_base
extern isal_deflate_body_01
extern isal_deflate_body_02
extern isal_deflate_body_04
extern isal_deflate_finish_base
extern isal_deflate_finish_01

extern isal_deflate_icf_body_hash_hist_base
extern isal_deflate_icf_body_hash_hist_01
extern isal_deflate_icf_body_hash_hist_02
extern isal_deflate_icf_body_hash_hist_04
extern isal_deflate_icf_finish_hash_hist_base
extern isal_deflate_icf_finish_hash_hist_01

extern isal_deflate_icf_finish_hash_map_base

extern isal_update_histogram_base
extern isal_update_histogram_01
extern isal_update_histogram_04

extern gen_icf_map_h1_base
extern gen_icf_map_lh1_04

extern encode_deflate_icf_base
extern encode_deflate_icf_04

extern set_long_icf_fg_base
extern set_long_icf_fg_04

%ifdef HAVE_AS_KNOWS_AVX512
extern encode_deflate_icf_06
extern set_long_icf_fg_06
extern gen_icf_map_lh1_06
%endif

extern adler32_base
extern adler32_avx2_4
extern adler32_sse

extern isal_deflate_hash_base
extern isal_deflate_hash_crc_01

extern isal_deflate_hash_mad_base

extern icf_body_hash1_fillgreedy_lazy
extern icf_body_lazyhash1_fillgreedy_greedy

section .text

%include "multibinary.asm"

mbin_interface		isal_deflate_body
mbin_dispatch_init5	isal_deflate_body, isal_deflate_body_base, isal_deflate_body_01, isal_deflate_body_02, isal_deflate_body_04
mbin_interface		isal_deflate_finish
mbin_dispatch_init5	isal_deflate_finish, isal_deflate_finish_base, isal_deflate_finish_01, isal_deflate_finish_01, isal_deflate_finish_01

mbin_interface		isal_deflate_icf_body_lvl1
mbin_dispatch_init5	isal_deflate_icf_body_lvl1, isal_deflate_icf_body_hash_hist_base, isal_deflate_icf_body_hash_hist_01, isal_deflate_icf_body_hash_hist_02, isal_deflate_icf_body_hash_hist_04

mbin_interface		isal_deflate_icf_body_lvl2
mbin_dispatch_init5	isal_deflate_icf_body_lvl2, isal_deflate_icf_body_hash_hist_base, isal_deflate_icf_body_hash_hist_01, isal_deflate_icf_body_hash_hist_02, isal_deflate_icf_body_hash_hist_04

mbin_interface		isal_deflate_icf_body_lvl3
mbin_dispatch_init5	isal_deflate_icf_body_lvl3, icf_body_hash1_fillgreedy_lazy, icf_body_hash1_fillgreedy_lazy, icf_body_hash1_fillgreedy_lazy, icf_body_lazyhash1_fillgreedy_greedy

mbin_interface		isal_deflate_icf_finish_lvl1
mbin_dispatch_init5	isal_deflate_icf_finish_lvl1, isal_deflate_icf_finish_hash_hist_base, isal_deflate_icf_finish_hash_hist_01, isal_deflate_icf_finish_hash_hist_01, isal_deflate_icf_finish_hash_hist_01

mbin_interface		isal_deflate_icf_finish_lvl2
mbin_dispatch_init5	isal_deflate_icf_finish_lvl2, isal_deflate_icf_finish_hash_hist_base, isal_deflate_icf_finish_hash_hist_01, isal_deflate_icf_finish_hash_hist_01, isal_deflate_icf_finish_hash_hist_01

mbin_interface		isal_deflate_icf_finish_lvl3
mbin_dispatch_init5	isal_deflate_icf_finish_lvl3, isal_deflate_icf_finish_hash_map_base, isal_deflate_icf_finish_hash_map_base, isal_deflate_icf_finish_hash_map_base, isal_deflate_icf_finish_hash_map_base

mbin_interface		isal_update_histogram
mbin_dispatch_init5	isal_update_histogram, isal_update_histogram_base, isal_update_histogram_01, isal_update_histogram_01, isal_update_histogram_04

mbin_interface		encode_deflate_icf
mbin_dispatch_init6	encode_deflate_icf, encode_deflate_icf_base, encode_deflate_icf_base, encode_deflate_icf_base, encode_deflate_icf_04, encode_deflate_icf_06

mbin_interface		set_long_icf_fg
mbin_dispatch_init6	set_long_icf_fg, set_long_icf_fg_base, set_long_icf_fg_base, set_long_icf_fg_base, set_long_icf_fg_04, set_long_icf_fg_06

mbin_interface		gen_icf_map_lh1
mbin_dispatch_init6	gen_icf_map_lh1, gen_icf_map_h1_base, gen_icf_map_h1_base, gen_icf_map_h1_base, gen_icf_map_lh1_04, gen_icf_map_lh1_06

mbin_interface		isal_adler32
mbin_dispatch_init5	isal_adler32, adler32_base, adler32_sse, adler32_sse, adler32_avx2_4

mbin_interface		isal_deflate_hash_lvl0
mbin_dispatch_init5	isal_deflate_hash_lvl0, isal_deflate_hash_base, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01

mbin_interface		isal_deflate_hash_lvl1
mbin_dispatch_init5	isal_deflate_hash_lvl1, isal_deflate_hash_base, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01

mbin_interface		isal_deflate_hash_lvl2
mbin_dispatch_init5	isal_deflate_hash_lvl2, isal_deflate_hash_base, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01

mbin_interface		isal_deflate_hash_lvl3
mbin_dispatch_init5	isal_deflate_hash_lvl3, isal_deflate_hash_base, isal_deflate_hash_base, isal_deflate_hash_base, isal_deflate_hash_mad_base
