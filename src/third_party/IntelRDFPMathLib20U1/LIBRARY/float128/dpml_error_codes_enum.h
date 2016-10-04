/******************************************************************************
  Copyright (c) 2007-2011, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#define	M_ACOS	0
#define	M_ACOSD	1
#define	M_ACOSH	2
#define	M_ASIN	3
#define	M_ASIND	4
#define	M_ASINH	5
#define	M_ATAN	6
#define	M_ATAND	7
#define	M_ATANH	8
#define	M_ATAN2	9
#define	M_ATAND2	10
#define	M_CABS	11
#define	M_COS	12
#define	M_COSD	13
#define	M_COSH	14
#define	M_CSQRT	15
#define	M_EXP	16
#define	M_EXPM1	17
#define	M_LOG	18
#define	M_LOG2	19
#define	M_LOG10	20
#define	M_MOD	21
#define	M_POWER	22
#define	M_REM	23
#define	M_SIN	24
#define	M_SIND	25
#define	M_SINH	26
#define	M_SQRT	27
#define	M_TAN	28
#define	M_TAND	29
#define	M_TANH	30
#define	M_SINCOS	31
#define	M_SINCOSD	32
#define	M_COT	33
#define	M_COTD	34
#define	M_TANCOT	35
#define	M_TANCOTD	36
#define	M_LOGB	37
#define	M_LDEXP	38
#define	M_CDIV	39
#define	M_NEXTAFTER	40
#define	M_INTPOWER	41
#define	M_BES_Y0	42
#define	M_BES_Y1	43
#define	M_BES_YN	44
#define	M_LOG1P	45
#define	M_LGAMMA	46
#define	M_SCALB	47
#define	M_INTINTPOWER	48
#define	M_BES_J0	49
#define	M_BES_J1	50
#define	M_BES_JN	51
#define	M_ERF	52
#define	M_ERFC	53
#define	M_TRUNC	54
#define	M_FLOOR	55
#define	M_CEIL	56
#define	M_FABS	57
#define	M_FREXP	58
#define	M_HYPOT	59
#define	M_MODF	60
#define	M_RSQRT	61
#define	M_EXP2	62
#define	M_TGAMMA	63
#define	M_SCALBN	64
#define	M_SCALBLN	65
#define	M_LRINT	66
#define	M_LROUND	67
#define	M_LLRINT	68
#define	M_LLROUND	69
#define	M_REMQUO	70
#define	M_NEXTTOWARD	71
#define	M_FDIM	72
#define	M_FMAX	73
#define	M_FMIN	74
#define	M_FMA	75
#define	M_NANFUNC	76
#define	M_LAST	77


#define	ACOS_ARG_GT_ONE	0
#define	ACOSD_ARG_GT_ONE	1
#define	ACOSH_ARG_LT_ONE	2
#define	ASIN_ARG_GT_ONE	3
#define	ASIND_ARG_GT_ONE	4
#define	ATANH_ABS_ARG_GT_ONE	5
#define	ATANH_OF_ONE	6
#define	ATANH_OF_NEG_ONE	7
#define	ATAN2_BOTH_ZERO	8
#define	ATAN2_BOTH_INF	9
#define	ATAN2_UNDERFLOW	10
#define	ATAND2_BOTH_ZERO	11
#define	ATAND2_BOTH_INF	12
#define	ATAND2_UNDERFLOW	13
#define	CABS_OVERFLOW	14
#define	CDIV_DIV_BY_ZERO	15
#define	CDIV_OVERFLOW	16
#define	COS_OF_INFINITY	17
#define	COSD_OF_INFINITY	18
#define	COSH_OVERFLOW	19
#define	COT_UNDERFLOW	20
#define	COT_POS_OVERFLOW	21
#define	COT_NEG_OVERFLOW	22
#define	COT_OF_INFINITY	23
#define	COT_OF_ZERO	24
#define	COT_OF_NEG_ZERO	25
#define	COTD_UNDERFLOW	26
#define	COTD_POS_OVERFLOW	27
#define	COTD_NEG_OVERFLOW	28
#define	COTD_OF_INFINITY	29
#define	COTD_OF_ZERO	30
#define	COTD_OF_NEG_ZERO	31
#define	COTD_MULTIPLE_OF_180	32
#define	EXP_OVERFLOW	33
#define	EXP_UNDERFLOW	34
#define	EXP_OF_INF	35
#define	EXP_OF_NEG_INF	36
#define	EXPM1_OVERFLOW	37
#define	EXPM1_OF_INF	38
#define	EXPM1_OF_NEG_INF	39
#define	LDEXP_OVERFLOW	40
#define	LDEXP_NEG_OVERFLOW	41
#define	LDEXP_UNDERFLOW	42
#define	SCALB_OVERFLOW	43
#define	SCALB_NEG_OVERFLOW	44
#define	SCALB_UNDERFLOW	45
#define	SCALB_OF_POS_TO_POS_INF	46
#define	SCALB_OF_NEG_TO_POS_INF	47
#define	SCALB_OF_FINITE_TO_NEG_INF	48
#define	SCALB_OF_INF_TO_NEG_INF	49
#define	SCALB_INVALID	50
#define	LOGB_OF_ZERO	51
#define	LOG_OF_NEGATIVE	52
#define	LOG_OF_ZERO	53
#define	LOG2_OF_NEGATIVE	54
#define	LOG2_OF_ZERO	55
#define	LOG10_OF_NEGATIVE	56
#define	LOG10_OF_ZERO	57
#define	LOG1P_LESS_M1	58
#define	LOG1P_M1	59
#define	MOD_UNDERFLOW	60
#define	MOD_BY_ZERO	61
#define	MOD_OF_INF	62
#define	NEXTAFTER_POS_OVERFLOW	63
#define	NEXTAFTER_NEG_OVERFLOW	64
#define	NEXTAFTER_POS_UNDERFLOW	65
#define	NEXTAFTER_NEG_UNDERFLOW	66
#define	POWER_POS_OVERFLOW	67
#define	POWER_NEG_OVERFLOW	68
#define	POWER_UNDERFLOW	69
#define	POWER_NEG_BASE	70
#define	POWER_ZERO_TO_NEG	71
#define	POWER_INF_TO_ZERO	72
#define	POWER_ONE_TO_INF	73
#define	POWER_NEG_ZERO_TO_NEG	74
#define	POWER_ZERO_TO_ZERO	75
#define	POWER_POS_INF_TO_POS	76
#define	POWER_NEG_INF_TO_POS	77
#define	POWER_NEG_INF_TO_POS_ODD	78
#define	POWER_FINITE_TO_INF	79
#define	POWER_INF_TO_NEG	80
#define	POWER_SMALL_TO_INF	81
#define	INTPOWER_POS_OVERFLOW	82
#define	INTPOWER_NEG_OVERFLOW	83
#define	INTPOWER_POS_UNDERFLOW	84
#define	INTPOWER_NEG_UNDERFLOW	85
#define	INTPOWER_ZERO_TO_ZERO	86
#define	INTPOWER_POS_DIV_BY_ZERO	87
#define	INTPOWER_NEG_DIV_BY_ZERO	88
#define	INTINTPOWER_OVERFLOW	89
#define	INTINTPOWER_ZERODIV	90
#define	REM_UNDERFLOW	91
#define	REM_BY_ZERO	92
#define	REM_OF_INF	93
#define	SIN_OF_INFINITY	94
#define	SINCOS_OF_INFINITY	95
#define	SINCOSD_OF_INFINITY	96
#define	SINCOSD_UNDERFLOW	97
#define	SIND_OF_INFINITY	98
#define	SIND_UNDERFLOW	99
#define	SINH_OVERFLOW	100
#define	SINH_NEG_OVERFLOW	101
#define	SINH_UNDERFLOW	102
#define	SQRT_OF_NEGATIVE	103
#define	RSQRT_OF_POS_ZERO	104
#define	RSQRT_OF_NEG_ZERO	105
#define	TAN_OF_INFINITY	106
#define	TAND_UNDERFLOW	107
#define	TAND_OVERFLOW	108
#define	TAND_OF_INFINITY	109
#define	TAND_ODD_MULTIPLE_OF_90	110
#define	TANH_OVERFLOW	111
#define	TANH_UNDERFLOW	112
#define	TANCOT_OF_INFINITY	113
#define	TANCOTD_OF_INFINITY	114
#define	TANCOTD_UNDERFLOW	115
#define	BES_J0_OF_INFINITY	116
#define	BES_J1_OF_INFINITY	117
#define	BES_JN_OF_INFINITY	118
#define	BES_J1_UNDERFLOW	119
#define	BES_J1_NEG_UNDERFLOW	120
#define	BES_JN_UNDERFLOW	121
#define	BES_JN_NEG_UNDERFLOW	122
#define	BES_Y0_OF_INFINITY	123
#define	BES_Y1_OF_INFINITY	124
#define	BES_YN_OF_INFINITY	125
#define	BES_Y0_OF_NEGATIVE	126
#define	BES_Y0_OF_ZERO	127
#define	BES_Y1_OF_NEGATIVE	128
#define	BES_Y1_OF_ZERO	129
#define	BES_Y1_OVERFLOW	130
#define	BES_YN_OF_NEGATIVE	131
#define	BES_YN_OF_ZERO	132
#define	BES_YN_NEG_OVERFLOW	133
#define	BES_YN_POS_OVERFLOW	134
#define	LGAMMA_OVERFLOW	135
#define	LGAMMA_POS_INF	136
#define	LGAMMA_NEG_INF	137
#define	LGAMMA_NON_POS_INT	138
#define	LGAMMA_OF_ZERO	139
#define	ERFC_UNDERFLOW	140
#define	NANFUNC_CANONICAL_NAN	141
#define	EXP2_OVERFLOW	142
#define	EXP2_UNDERFLOW	143
#define	EXP2_OF_INF	144
#define	EXP2_OF_NEG_INF	145
#define	SCALBN_OVERFLOW	146
#define	SCALBN_NEG_OVERFLOW	147
#define	SCALBN_UNDERFLOW	148
#define	SCALBLN_OVERFLOW	149
#define	SCALBLN_NEG_OVERFLOW	150
#define	SCALBLN_UNDERFLOW	151
#define	TGAMMA_OVERFLOW	152
#define	TGAMMA_NEG_OVERFLOW	153
#define	TGAMMA_POS_INF	154
#define	TGAMMA_NEG_INF	155
#define	TGAMMA_EVEN_NEG_INT	156
#define	TGAMMA_ODD_NEG_INT	157
#define	TGAMMA_OF_ZERO	158
#define	LRINT_OVERFLOW	159
#define	LROUND_OVERFLOW	160
#define	LLRINT_OVERFLOW	161
#define	LLROUND_OVERFLOW	162
#define	REMQUO_UNDERFLOW	163
#define	REMQUO_BY_ZERO	164
#define	REMQUO_OF_INF	165
#define	NEXTTOWARD_POS_OVERFLOW	166
#define	NEXTTOWARD_NEG_OVERFLOW	167
#define	NEXTTOWARD_POS_UNDERFLOW	168
#define	NEXTTOWARD_NEG_UNDERFLOW	169
#define	FDIM_POS_OVERFLOW	170
#define	FDIM_POS_UNDERFLOW	171
#define	FMA_POS_UNDERFLOW	172
#define	FMA_NEG_UNDERFLOW	173
#define	FMA_POS_OVERFLOW	174
#define	FMA_NEG_OVERFLOW	175
#define	FMA_INF_AND_ZERO	176
#define	FMA_INF_AND_INF	177
#define	LAST_ERROR_CODE	178
