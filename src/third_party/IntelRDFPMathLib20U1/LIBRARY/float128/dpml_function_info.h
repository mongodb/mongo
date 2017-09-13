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

#define __INTEGER		0
#define	__FLOAT			1
#define __COMPLEX		2
#define __PTR_TO_INTEGER	3
#define __PTR_TO_FLOAT		4
#define __PTR_TO_COMPLEX	5

#define	ONE_ARG(r,a)		(1 | ((r) << 3) | ((a) << 6))
#define	TWO_ARGS(r,a,b)		(2 | ((r) << 3) | ((a) << 6) | ((b) << 9))
#define	THREE_ARGS(r,a,b,c)	(4 | ((r) << 3) | ((a) << 6) | ((b) << 9) \
				   | ((c) << 12))
#define	FOUR_ARGS(r,a,b,c,d)	(4 | ((r) << 3) | ((a) << 6) | ((b) << 9) \
				   | ((c) << 12) | ((d) << 15))

#define	F_F	ONE_ARG(   __FLOAT,   __FLOAT)
#define I_F     ONE_ARG(__INTEGER,  __FLOAT)
#define	C_F	ONE_ARG(   __COMPLEX, __FLOAT)
#define	F_FF	TWO_ARGS(  __FLOAT,   __FLOAT,   __FLOAT)
#define	F_FI	TWO_ARGS(  __FLOAT,   __FLOAT,   __INTEGER)
#define	F_IF	TWO_ARGS(  __FLOAT,   __INTEGER, __FLOAT)
#define	I_II	TWO_ARGS(  __INTEGER, __INTEGER, __INTEGER)
#define	C_FF	TWO_ARGS(  __COMPLEX, __FLOAT,   __FLOAT)
#define	F_FpI	TWO_ARGS(  __FLOAT,   __FLOAT,   __PTR_TO_INTEGER)
#define F_FFpI  THREE_ARGS( __FLOAT, __FLOAT, __FLOAT, __PTR_TO_INTEGER)
#define	C_FFFF	FOUR_ARGS( __COMPLEX, __FLOAT,   __FLOAT, __FLOAT, __FLOAT)

#define	G_NUM_ARGS(n)		((n) & 0x7)
#define	G_RESULT_TYPE(n)	(((n) >> 3) & 0x7)
#define	G_ARG1_TYPE(n)		(((n) >> 6) & 0x7)
#define	G_ARG2_TYPE(n)		(((n) >> 9) & 0x7)
#define	G_ARG3_TYPE(n)		(((n) >> 12) & 0x7)
#define	G_ARG4_TYPE(n)		(((n) >> 15) & 0x7)

               /* Generic    Argument    NT Opcode   */
               /*  Name     Descriptor     Root      */
    GEN_FUNC_INFO( ACOS,	F_F,	Acos )
    GEN_FUNC_INFO( ACOSD,	F_F,	Unspecified )
    GEN_FUNC_INFO( ACOSH,	F_F,	Unspecified )
    GEN_FUNC_INFO( ASIN,	F_F,	Asin )
    GEN_FUNC_INFO( ASIND,	F_F,	Unspecified )
    GEN_FUNC_INFO( ASINH,	F_F,	Unspecified )
    GEN_FUNC_INFO( ATAN,	F_F,	Atan )
    GEN_FUNC_INFO( ATAND,	F_F,	Unspecified )
    GEN_FUNC_INFO( ATANH,	F_F,	Unspecified )
    GEN_FUNC_INFO( ATAN2,	F_FF,	Atan2 )
    GEN_FUNC_INFO( ATAND2,	F_FF,	Unspecified )
    GEN_FUNC_INFO( CABS,	F_FF,	Cabs )
    GEN_FUNC_INFO( COS,		F_F,	Cos )
    GEN_FUNC_INFO( COSD,	F_F,	Unspecified )
    GEN_FUNC_INFO( COSH,	F_F,	Cosh )
    GEN_FUNC_INFO( CSQRT,	C_FF,	Unspecified )
    GEN_FUNC_INFO( EXP,		F_F,	Exp )
    GEN_FUNC_INFO( EXPM1,	F_F,	Unspecified )
    GEN_FUNC_INFO( LOG,		F_F,	Log )
    GEN_FUNC_INFO( LOG2,	F_F,	Unspecified )
    GEN_FUNC_INFO( LOG10,	F_F,	Log10 )
    GEN_FUNC_INFO( MOD,		F_FF,	Fmod )
    GEN_FUNC_INFO( POWER,	F_FF,	Pow )
    GEN_FUNC_INFO( REM,		F_FF,	Remainder )
    GEN_FUNC_INFO( SIN,		F_F,	Sin )
    GEN_FUNC_INFO( SIND,	F_F,	Unspecified )
    GEN_FUNC_INFO( SINH,	F_F,	Sinh )
    GEN_FUNC_INFO( SQRT,	F_F,	SquareRoot )
    GEN_FUNC_INFO( TAN,		F_F,	Tan )
    GEN_FUNC_INFO( TAND,	F_F,	Unspecified )
    GEN_FUNC_INFO( TANH,	F_F,	Tanh )
    GEN_FUNC_INFO( SINCOS,	C_F,	Unspecified )
    GEN_FUNC_INFO( SINCOSD,	C_F,	Unspecified )
    GEN_FUNC_INFO( COT,		F_F,	Unspecified )
    GEN_FUNC_INFO( COTD,	F_F,	Unspecified )
    GEN_FUNC_INFO( TANCOT,	C_F,	Unspecified )
    GEN_FUNC_INFO( TANCOTD,	C_F,	Unspecified )
    GEN_FUNC_INFO( LOGB,	F_F,	Logb )
    GEN_FUNC_INFO( LDEXP,	F_FI,	Ldexp )
    GEN_FUNC_INFO( CDIV,	C_FFFF,	Unspecified )
    GEN_FUNC_INFO( NEXTAFTER,	F_FF,	Nextafter )
    GEN_FUNC_INFO( INTPOWER,	F_FI,	Unspecified )
    GEN_FUNC_INFO( BES_Y0,	F_F,	Y0 )
    GEN_FUNC_INFO( BES_Y1,	F_F,	Y1 )
    GEN_FUNC_INFO( BES_YN,	F_IF,	Yn )
    GEN_FUNC_INFO( LOG1P,	F_F,	Unspecified )
    GEN_FUNC_INFO( LGAMMA,	F_F,	Unspecified )
    GEN_FUNC_INFO( SCALB,	F_FF,	Unspecified )
    GEN_FUNC_INFO( INTINTPOWER,	I_II,	Unspecified )
    GEN_FUNC_INFO( BES_J0,	F_F,	Unspecified )
    GEN_FUNC_INFO( BES_J1,	F_F,	Unspecified )
    GEN_FUNC_INFO( BES_JN,	F_IF,	Unspecified )
    GEN_FUNC_INFO( ERF,		F_F,	Unspecified )
    GEN_FUNC_INFO( ERFC,	F_F,	Unspecified )

    /* New functions */
    GEN_FUNC_INFO( TRUNC,	F_F,	Truncate )
    GEN_FUNC_INFO( FLOOR,	F_F,	Floor )
    GEN_FUNC_INFO( CEIL,	F_F,	Ceil )
    GEN_FUNC_INFO( FABS,	F_F,	Fabs )
    GEN_FUNC_INFO( FREXP,	F_FpI,	Frexp )
    GEN_FUNC_INFO( HYPOT,	F_FF,	Hypot )
    GEN_FUNC_INFO( MODF,	F_FF,	Modf )
    GEN_FUNC_INFO( RSQRT,	F_F,	Unspecified )
    GEN_FUNC_INFO(EXP2,         F_F,    Unspecified)
    GEN_FUNC_INFO(TGAMMA,       F_F,    Unspecified )
    GEN_FUNC_INFO(SCALBN,       F_FI,   Unspecified )
    GEN_FUNC_INFO(SCALBLN,      F_FI,   Unspecified )
    GEN_FUNC_INFO(LRINT,        I_F,    Unspecified )
    GEN_FUNC_INFO(LROUND,       I_F,    Unspecified )
    GEN_FUNC_INFO(LLRINT,       I_F,    Unspecified )
    GEN_FUNC_INFO(LLROUND,      I_F,    Unspecified )
    GEN_FUNC_INFO(REMQUO,       F_FFpI, Unspecified )
    GEN_FUNC_INFO(NEXTTOWARD,   F_FF,    Unspecified )
    GEN_FUNC_INFO(FDIM,         F_FF,    Unspecified )
    GEN_FUNC_INFO(FMAX,         F_FF,    Unspecified )
    GEN_FUNC_INFO(FMIN,         F_FF,    Unspecified )
    GEN_FUNC_INFO(FMA,          F_FF,    Unspecified )
    GEN_FUNC_INFO( NANFUNC,     F_F,     Unspecified )
