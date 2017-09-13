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

#if !defined FOUR_OVER_PI_BUILD_FILE_NAME
#define FOUR_OVER_PI_BUILD_FILE_NAME	dpml_four_over_pi.c
#endif
#define NUM_F_DIGITS	2
#define NUM_G_DIGITS	4
#define NUM_W_DIGITS	4
#define NUM_REQ_BITS	250
#define NUM_EXTRA_BITS	6

#define W_HAS_M_BIT_LOSS	(((MSD_OF_W + 0x40000000000000ull) & 0x3f80000000000000ull) == 0)
#define G_DIGITS	g0, g1, g2, g3
#define F_DIGITS	F0, F1
#define TMP_DIGITS	t0, t1, t2, t3

#define MSD_OF_W	g3
#define LSD_OF_W	g1
#define SECOND_MSD_OF_W	g2
#define CARRY_DIGIT	t3

#define GET_F_DIGITS(x); \
	F1 = G_UX_FRACTION_DIGIT(x, 0); \
	F0 = G_UX_FRACTION_DIGIT(x, 1)

#define PUT_W_DIGITS(x); \
	P_UX_FRACTION_DIGIT(x, 0, g3); \
	P_UX_FRACTION_DIGIT(x, 1, g2)

#define NEGATE_W { \
	g3 = ~g3; \
	g2 = ~g2; \
	g1 = ~g1; \
	g1 += 1; CARRY_DIGIT = (g1 == 0); \
	g2 += CARRY_DIGIT; CARRY_DIGIT = (g2 == 0); \
	g3 += CARRY_DIGIT; }

#define GET_G_DIGITS_FROM_TABLE(p, extra_digit) \
	g3 = p[0];  \
	g2 = p[1];  \
	g1 = p[2];  \
	g0 = p[3];  \
	extra_digit = p[4];  \
	p += 5

#define LEFT_SHIFT_G_DIGITS(lshift, rshift, extra_digit) \
	g3 = (g3 << (lshift)) | (g2 >> (rshift)); \
	g2 = (g2 << (lshift)) | (g1 >> (rshift)); \
	g1 = (g1 << (lshift)) | (g0 >> (rshift)); \
	g0 = (g0 << (lshift)) | (extra_digit >> (rshift))

#define MULTIPLY_F_AND_G_DIGITS(c) \
	XMUL(g0,F0,t1,t0); \
	XMUL_ADD(g0,F1,t1,t2,t1); \
	g0 = t0; \
	XMUL_XADDC(g1,F0,t2,t1,c,t2,t1); \
	XMUL_XADD(g1,F1,c,t2,t0,t2); \
	g1 = t1; \
	XMUL_XADD(g2,F0,t0,t2,t0,t2); \
	MUL_ADD(g2,F1,t0,t0); \
	g2 = t2; \
	MUL_ADD(g3,F0,t0,t0); \
	g3 = t0

#define GET_NEXT_PRODUCT(g, w, c) \
	XMUL_XADDC(g,F0,g0,(DIGIT_TYPE)0,c,g0,w); \
	XMUL_XADDC_W_C_IN(g,F1,g1,g0,c,c,g1,g0); \
	if (c)  \
	if (++g2 == 0)  \
	g3++

#define LEFT_SHIFT_W_LOW_DIGITS_BY_ONE(extra_w_digit) \
	g2 = g1; \
	g1 = g0; \
	g0 = extra_w_digit


