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

#include <float.h>


#if (BITS_PER_WORD != 64)
#   define EXT_MULH_32(i,j,hi) EXT_MULH((i),(j),(hi))
#else
#   define EXT_MULH_32(i,j,hi) (hi) =  (INT_32) ((((WORD) (i)) * ((WORD) (j))) >> 32)
#endif

#define ARITH_SHIFT_WORD_RIGHT(i,j) (i) = (WORD)(i) >> (j);



#if (COMPILER == msc_cc)
#  if BITS_PER_WORD == 32

#    define EXT_UMUL(i,j,lo,hi) { \
	int tmp_i = (i); \
	int tmp_j = (j); \
	int tmp_lo, tmp_hi; \
	{ \
		__asm mov eax, tmp_i \
		__asm mul tmp_j \
		__asm mov tmp_lo, eax \
		__asm mov tmp_hi, edx \
	} \
	(lo) = tmp_lo; \
	(hi) = tmp_hi; \
}


#    define EXT_UMULH(i,j,hi) { \
	int tmp_i = (i); \
	int tmp_j = (j); \
	int tmp_hi; \
	{ \
		__asm mov eax, tmp_i \
		__asm mul tmp_j \
		__asm mov tmp_hi, edx \
	} \
	(hi) = tmp_hi; \
}


#    define EXT_MULH(i,j,hi) { \
	int tmp_i = (i); \
	int tmp_j = (j); \
	int tmp_hi; \
	{ \
		__asm mov eax, tmp_i \
		__asm imul tmp_j \
		__asm mov tmp_hi, edx \
	} \
	(hi) = tmp_hi; \
}


#    define F_RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT (BITS_PER_WORD - 1)
#    define F_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) { \
	{ \
		__asm fld x \
		__asm frndint \
		__asm fstp flt_int_x \
	} \
	int_x = (WORD)flt_int_x; \
}

#    define B_RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT (BITS_PER_WORD - 1)
#    define B_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) \
	F_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) 

#endif

#define FPU_STATUS_WORD_TYPE unsigned short


#if (USE_CONTROL87)

#define INIT_FPU_STATE_AND_ROUND_TO_NEAREST(status_word) { \
	status_word = _control87(0,0); \
	_control87(MCW_RC, _RC_NEAR); \
}

#define INIT_FPU_STATE_AND_ROUND_TO_ZERO(status_word) { \
	status_word = _control87(0,0); \
	_control87(RC_CHOP, MCW_RC); \
}

#define RESTORE_FPU_STATE(status_word) { \
	_control87((INT_16)status_word, 0xffff); \
}

#else

#define INIT_FPU_STATE_AND_ROUND_TO_NEAREST(status_word) { \
	FPU_STATUS_WORD_TYPE tmp = 0x037f; \
	{ \
		__asm fstcw status_word \
		__asm fldcw tmp \
	} \
}

#define INIT_FPU_STATE_AND_ROUND_TO_ZERO(status_word) { \
	FPU_STATUS_WORD_TYPE tmp = 0x0f7f; \
	{ \
		__asm fstcw status_word \
		__asm fldcw tmp \
	} \
}

#define RESTORE_FPU_STATE(status_word) { \
	FPU_STATUS_WORD_TYPE tmp = (FPU_STATUS_WORD_TYPE)(status_word); \
	{ \
		__asm fldcw tmp \
	} \
}

#endif



/* The following several macros are intended to be used as a set.  It is
the combination of F_SAVE_SIGN_AND_GET_ABS and F_RESTORE_SIGN (or
F_NEGATE_IF_SIGN_NEG) that should be efficient (i.e. if slowing one of them
down will make the combination faster, go ahead and do it.  */

#ifndef F_SIGN_TYPE

#	define F_SIGN_TYPE U_WORD

#	define F_SAVE_SIGN_AND_GET_ABS(x, sign, abs_x) { \
		F_UNION u; \
		u.f = (x); \
		F_ABS((x), (abs_x)); \
		(sign) = u.F_HI_WORD; \
	}

#	define F_CHANGE_SIGN(sign) \
		(sign) = ~(sign)

#	define F_RESTORE_SIGN(sign, x) \
		ASSERT((x) >= 0.0); \
		if ((WORD)sign < 0) F_NEGATE(x);

#	define F_NEGATE_IF_SIGN_NEG(sign, x) \
		if ((WORD)sign < 0) F_NEGATE(x);

#endif



#elif (COMPILER == gnu_cc)



#define __ABS(x,abs_x) { \
	abs_x = x; \
	__asm__ __volatile__ ("fabs" :"=t" (abs_x) : "0" (abs_x)); \
}

#define __CLEAR_NEG_BIT(x) { \
	__asm__ __volatile__ ("fabs" :"=t" (x) : "0" (x)); \
}

#define __NEGATE(x) { \
	__asm__ __volatile__ ("fchs" :"=t" (x) : "0" (x)); \
}

#define __SET_NEG_BIT(x) { \
	__asm__ __volatile__ ("fabs" :"=t" (x) : "0" (x)); \
	__asm__ __volatile__ ("fchs" :"=t" (x) : "0" (x)); \
}

#define __RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT (BITS_PER_WORD - 1)
#define __RINT_TO_FLOATING_AND_WORD(x, f_int_x, int_x) { \
	f_int_x = x; \
	__asm__ __volatile__ ("frndint" :"=t" (f_int_x) : "0" (f_int_x)); \
	int_x = (WORD) f_int_x;  \
}


#define F_ABS(x,abs_x) __ABS(x,abs_x)
#define F_CLEAR_NEG_BIT(x) __CLEAR_NEG_BIT(x)
#define F_NEGATE(x) __NEGATE(x)
#define F_SET_NEG_BIT(x) __SET_NEG_BIT(x)

#define F_RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT \
	__RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT
#define F_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) \
	__RINT_TO_FLOATING_AND_WORD((x), (flt_int_x), (int_x))

#if (F_FORMAT == s_floating)

#define B_ABS(x,abs_x) __ABS(x,abs_x)
#define B_CLEAR_NEG_BIT(x) __CLEAR_NEG_BIT(x)
#define B_NEGATE(x) __NEGATE(x)
#define B_SET_NEG_BIT(x) __SET_NEG_BIT(x)

#define B_RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT \
	__RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT
#define B_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) \
	__RINT_TO_FLOATING_AND_WORD((x), (flt_int_x), (int_x))

#endif  /* F_FORMAT */


#define INIT_FPU_STATE_AND_ROUND_TO_NEAREST(status_word) { \
	volatile unsigned short tmp; \
	__asm__ volatile ("fstcw %0" : "=m" (tmp) : ); \
	status_word = tmp; \
	tmp &= 0xf2ff; \
	__asm__ volatile ("fldcw %0" : : "m" (tmp)); \
}

#define INIT_FPU_STATE_AND_ROUND_TO_ZERO(status_word) { \
	volatile unsigned short tmp; \
	__asm__ volatile ("fstcw %0" : "=m" (tmp) : ); \
	status_word = tmp; \
	tmp &= 0xf2ff; \
	tmp |= 0x0c00; \
	__asm__ volatile ("fldcw %0" : : "m" (tmp)); \
}

#define RESTORE_FPU_STATE(status_word) { \
	volatile unsigned short tmp = status_word; \
	__asm__ volatile ("fldcw %0" : : "m" (tmp)); \
}


/* The following several macros are intended to be used as a set.  It is
the combination of F_SAVE_SIGN_AND_GET_ABS and F_RESTORE_SIGN (or
F_NEGATE_IF_SIGN_NEG) that should be efficient (i.e. if slowing one of them
down will make the combination faster, go ahead and do it.  */

#ifndef F_SIGN_TYPE

#    define	F_SIGN_TYPE F_TYPE

#    define	F_SAVE_SIGN_AND_GET_ABS(x, sign, abs_x) \
		(sign) = x; \
		F_ABS((x), (abs_x))

#    define	F_CHANGE_SIGN(sign) \
		F_NEGATE(sign)

#    define	F_RESTORE_SIGN(sign, x) \
		ASSERT((x) >= 0.0); \
		if ((sign) < 0.0) F_NEGATE(x)

#    define	F_NEGATE_IF_SIGN_NEG(sign, x) \
		if ((sign) < 0.0) F_NEGATE(x)

#endif



#endif  /* COMPILER */




#define F_ADD_ROUNDED(x,y,z) \
{   volatile F_TYPE vv; \
    vv = (F_TYPE) (x) + (y); \
    z = vv; }

#define B_ADD_ROUNDED(x,y,z) \
{   volatile B_TYPE vv; \
    vv = (B_TYPE) (x) + (y); \
    z = vv; }

#define F_ADD_CHOPPED(x,y,z) \
{   volatile F_TYPE vv; \
    vv = (F_TYPE) (x) + (y); \
    z = vv; }

#define B_ADD_CHOPPED(x,y,z) \
{   volatile B_TYPE vv; \
    vv = (B_TYPE) (x) + (y); \
    z = vv; }

#define F_MUL_CHOPPED(x,y,z) \
{   volatile F_TYPE vv; \
    vv = (F_TYPE) (x) * (y); \
    z = vv; }

#define ADD_SUB_BIG(z, big) \
{ volatile F_TYPE vv; \
    vv = z + big; \
    z = vv - big; }

#define ADD_SUB_BIG_CHOPPED(z, big) \
{ volatile F_TYPE vv; \
    ADD(z, big, vv); \
    z = vv - big; }

#define SHORTEN_VIA_CASTS(in, out) \
{ volatile S_TYPE vv; \
    vv = (S_TYPE) in; \
    out = (D_TYPE) vv; }


#define ASSIGN_WITH_F_TYPE_PRECISION(y, truncated_y) \
{ volatile F_TYPE vv; \
    vv = (F_TYPE) y; \
    truncated_y = vv; }

#define CHOP_TO_S_TYPE(y, truncated_y) \
{ volatile F_TYPE vv; \
    vv = (F_TYPE) y; \
    truncated_y = vv; }





/*  The macro X_SQR_TO_HI_LO is used to produce high and low parts of x^2;  */
/*  see the comments in DPML_ERF.C for details.  The macros are defined	    */
/*  here specifically for the Intel platform to avoid a problem with the    */
/*  CL386 compiler.  Given the sequence					    */
/*									    */
/*		a = ( B_TYPE ) ( ( F_TYPE ) b )				    */
/*									    */
/*  where a and b are of type B_TYPE, the compiler simple moves b into a    */
/*  rather than first shortening it and then lengthening it.  These macros  */
/*  have been hand-crafted to work around this problem.			    */

#if PRECISION_BACKUP_AVAILABLE
#   define X_SQR_TO_HI_LO(x, t, hi, lo) { \
	volatile B_TYPE tv ; \
	volatile F_TYPE hiv, lov ; \
	tv = ( B_TYPE ) x ; \
	tv = tv * tv ; \
	hiv = ( F_TYPE ) tv ; \
	lov = ( F_TYPE ) ( tv - ( B_TYPE ) hiv ) ; \
	t = tv ; \
	hi = hiv ; \
	lo = lov ; \
	}
#else
#   define X_SQR_TO_HI_LO(x, t, hi, lo) { \
	volatile F_TYPE hiv, lov ; \
	volatile R_TYPE xv ; \
	xv = ( R_TYPE ) x ; \
	hiv = ( F_TYPE ) xv ; \
	lov = x - hiv ; \
	lov = lov * ( hiv + x ) ; \
	hiv = hiv * hiv ; \
	hi = hiv ; \
	lo = lov ; \
	}
#endif





/*  The following macros support extended precision multiplication of a	    */
/*  sequence of unsigned WORDs.  The basic operation is an extended integer */
/*  multiply and add with four inputs and three results.  The inputs are an */
/*  addend, in high and low parts (w_hi, w_lo), the carry in from a	    */
/*  previous operation, c_in, and the multiplier and multiplicand F and g.  */
/*  The three outputs are the carry out, c_out, and the high and low digits */
/*  of the sum, z_hi and z_lo.  The basic operation is			    */
/*									    */
/*	F * g + w_lo + w_hi * BITS_PER_WORD + c_in ->			    */
/*		    z_lo + z_hi * BITS_PER_WORD + c_out * BITS_PER_WORD^2   */
/*									    */
/*  There are six different macros:  one for the basic operation and five   */
/*  special cases (e.g. to ignore the carry out or when the carry in is	    */
/*  zero).								    */
/*									    */

#define BITS_PER_DIGIT       64

#define __LO(x)	((x) & MAKE_MASK( BITS_PER_DIGIT / 2, 0 ))
#define __HI(x)	(((x)) >> ( BITS_PER_DIGIT / 2 ))


#define UMULH(i, j, k) {						\
	U_INT_64 _ii, _iLo, _iHi, _jj, _jLo, _jHi, _p0, _p1, _p2; 	\
        _ii = i; _iLo = __LO(_ii); _iHi = __HI(_ii);			\
        _jj = j; _jLo = __LO(_jj); _jHi = __HI(_jj);			\
	_p0  = _iLo * _jLo;						\
	_p1  = (_iLo * _jHi);						\
	_p2  = (_iHi * _jLo) + __HI(_p0) + __LO(_p1);			\
        k   = (_iHi * _jHi) + __HI(_p1) + __HI(_p2);			\
	}


/*  a * b + add_low -> low						    */
#define MUL_ADD( a, b, add_low, low ) { \
    ( low ) = ( a ) * ( b ) + ( add_low ) ; \
    }

/*  a * b -> low + high * 2^BITS_PER_WORD				    */
#define XMUL( a, b, high, low ) { \
    U_WORD p0, p1, p2, p3, s ; \
    p0 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p1 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    p2 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p3 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    s = ( p0 >> ( BITS_PER_DIGIT / 2 ) ) + ( p1 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
	( p2 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    ( low ) = ( p0 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( s << ( BITS_PER_DIGIT / 2 ) ) ; \
    ( high ) = ( p1 >> ( BITS_PER_DIGIT / 2 ) ) + ( p2 >> ( BITS_PER_DIGIT / 2 ) ) + p3 + \
	( s >> ( BITS_PER_DIGIT / 2 ) ) ; \
    }

/*  a * b + add_low + add_high * 2^BITS_PER_WORD -> low +		    */
/*						    high * 2^BITS_PER_WORD  */
#define XMUL_ADD( a, b, add_low, high, low ) { \
    U_WORD p0, p1, p2, p3, s, t ; \
    p0 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p1 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    p2 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p3 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    s = ( p0 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    t = ( p0 >> ( BITS_PER_DIGIT / 2 ) ) + ( p1 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
        ( p2 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) >> ( BITS_PER_DIGIT / 2 ) ) + \
	( s >> ( BITS_PER_DIGIT / 2 ) ) ; \
    ( low ) = ( s & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( t << ( BITS_PER_DIGIT / 2 ) ) ; \
    ( high ) = ( p1 >> ( BITS_PER_DIGIT / 2 ) ) + ( p2 >> ( BITS_PER_DIGIT / 2 ) ) + p3 + \
	( t >> ( BITS_PER_DIGIT / 2 ) ) ; \
    }
    
/*  a * b + add_low + add_high * 2^BITS_PER_WORD -> low +		    */
/*						    high * 2^BITS_PER_WORD  */
#define XMUL_XADD( a, b, add_high, add_low, high, low ) { \
    U_WORD p0, p1, p2, p3, s, t ; \
    p0 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p1 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    p2 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p3 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    s = ( p0 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    t = ( p0 >> ( BITS_PER_DIGIT / 2 ) ) + ( p1 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
        ( p2 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) >> ( BITS_PER_DIGIT / 2 ) ) + \
	( s >> ( BITS_PER_DIGIT / 2 ) ) ; \
    ( low ) = ( s & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( t << ( BITS_PER_DIGIT / 2 ) ) ; \
    ( high ) = ( p1 >> ( BITS_PER_DIGIT / 2 ) ) + ( p2 >> ( BITS_PER_DIGIT / 2 ) ) + p3 + ( add_high ) + \
	( t >> ( BITS_PER_DIGIT / 2 ) ) ; \
    }

/*  a * b + add_low + add_high * 2^BITS_PER_WORD ->			    */
/*					low +				    */
/*					high * 2^BITS_PER_WORD +	    */
/*					carry_out * 2^(2 * BITS_PER_WORD)   */
#define XMUL_XADDC( a, b, add_high, add_low, carry_out, high, low ) { \
    U_WORD p0, p1, p2, p3, s, t, u, v ; \
    p0 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p1 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    p2 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p3 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    s = ( p0 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    t = ( p0 >> ( BITS_PER_DIGIT / 2 ) ) + ( p1 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
        ( p2 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) >> ( BITS_PER_DIGIT / 2 ) ) + \
	( s >> ( BITS_PER_DIGIT / 2 ) ) ; \
    u = ( p1 >> ( BITS_PER_DIGIT / 2 ) ) + ( p2 >> ( BITS_PER_DIGIT / 2 ) ) + \
	( p3 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_high ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
	( t >> ( BITS_PER_DIGIT / 2 ) ) ; \
    v = ( p3 >> ( BITS_PER_DIGIT / 2 ) ) + ( ( add_high ) >> ( BITS_PER_DIGIT / 2 ) ) + \
	( u >> ( BITS_PER_DIGIT / 2 ) ) ; \
    ( low ) = ( s & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( t << ( BITS_PER_DIGIT / 2 ) ) ; \
    ( high ) = ( u & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( v << ( BITS_PER_DIGIT / 2 ) ) ; \
    ( carry_out ) = v >> ( BITS_PER_DIGIT / 2 ) ; \
    }

/*  a * b + add_low + add_high * 2^BITS_PER_WORD + carry_in ->		    */
/*						    low +		    */
/*						    high * 2^BITS_PER_WORD  */
#define XMUL_XADD_W_C_IN( a, b, add_high, add_low, carry_in, high, low ) { \
    U_WORD p0, p1, p2, p3, s, t ; \
    p0 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p1 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    p2 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p3 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    s = ( p0 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
	( ( carry_in ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    t = ( p0 >> ( BITS_PER_DIGIT / 2 ) ) + ( p1 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
        ( p2 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) >> ( BITS_PER_DIGIT / 2 ) ) + \
	( ( carry_in ) >> ( BITS_PER_DIGIT / 2 ) ) + ( s >> ( BITS_PER_DIGIT / 2 ) ) ; \
    ( low ) = ( s & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( t << ( BITS_PER_DIGIT / 2 ) ) ; \
    ( high ) = ( p1 >> ( BITS_PER_DIGIT / 2 ) ) + ( p2 >> ( BITS_PER_DIGIT / 2 ) ) + p3 + ( add_high ) + \
	( t >> ( BITS_PER_DIGIT / 2 ) ) ; \
    }

/*  a * b + add_low + add_high * 2^BITS_PER_WORD + carry_in ->		    */
/*					low +				    */
/*					high * 2^BITS_PER_WORD +	    */
/*					carry_out * 2^(2 * BITS_PER_WORD)   */
#define XMUL_XADDC_W_C_IN( a, b, add_high, add_low, carry_in, carry_out, high, low ) { \
    U_WORD p0, p1, p2, p3, s, t, u, v ; \
    p0 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p1 = ( ( a ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    p2 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    p3 = ( ( a ) >> ( BITS_PER_DIGIT / 2 ) ) * ( ( b ) >> ( BITS_PER_DIGIT / 2 ) ) ; \
    s = ( p0 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
	( ( carry_in ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) ; \
    t = ( p0 >> ( BITS_PER_DIGIT / 2 ) ) + ( p1 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
        ( p2 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_low ) >> ( BITS_PER_DIGIT / 2 ) ) + \
	( ( carry_in ) >> ( BITS_PER_DIGIT / 2 ) ) + ( s >> ( BITS_PER_DIGIT / 2 ) ) ; \
    u = ( p1 >> ( BITS_PER_DIGIT / 2 ) ) + ( p2 >> ( BITS_PER_DIGIT / 2 ) ) + \
	( p3 & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( ( add_high ) & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + \
	( t >> ( BITS_PER_DIGIT / 2 ) ) ; \
    v = ( p3 >> ( BITS_PER_DIGIT / 2 ) ) + ( ( add_high ) >> ( BITS_PER_DIGIT / 2 ) ) + \
	( u >> ( BITS_PER_DIGIT / 2 ) ) ; \
    ( low ) = ( s & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( t << ( BITS_PER_DIGIT / 2 ) ) ; \
    ( high ) = ( u & MAKE_MASK ( BITS_PER_DIGIT / 2, 0 ) ) + ( v << ( BITS_PER_DIGIT / 2 ) ) ; \
    ( carry_out )= v >> ( BITS_PER_DIGIT / 2 ) ; \
    }


