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

#ifndef POLY_MACROS_H
#define POLY_MACROS_H



/* Define a macro that selects the appropriate polynomial form by adding a
suffix (an '_' should already be at the end of SELECT_POLY's parameter). */

#if MULTIPLE_ISSUE
#	define  SELECT_POLY(x) PASTE_2(x,M)
#else
#	define  SELECT_POLY(x) PASTE_2(x,C)
#endif



/* Basic polynomial macros (eventually these should be used to build
the other forms in this file */

#define	POLY0(p, x)	(*(p))
#define	POLY1(p, x)	(*(p) + x*POLY0(p + 1, x))
#define	POLY2(p, x)	(*(p) + x*POLY1(p + 1, x))
#define	POLY3(p, x)	(*(p) + x*POLY2(p + 1, x))
#define	POLY4(p, x)	(*(p) + x*POLY3(p + 1, x))
#define	POLY5(p, x)	(*(p) + x*POLY4(p + 1, x))
#define	POLY6(p, x)	(*(p) + x*POLY5(p + 1, x))
#define	POLY7(p, x)	(*(p) + x*POLY6(p + 1, x))
#define	POLY8(p, x)	(*(p) + x*POLY7(p + 1, x))
#define	POLY9(p, x)	(*(p) + x*POLY8(p + 1, x))
#define	POLY10(p, x)	(*(p) + x*POLY9(p + 1, x))
#define	POLY11(p, x)	(*(p) + x*POLY10(p + 1, x))
#define POLY12(p, x)	(*(p) + x*POLY11(p + 1, x))
#define POLY13(p, x)	(*(p) + x*POLY12(p + 1, x))
#define POLY14(p, x)	(*(p) + x*POLY13(p + 1, x))
#define POLY15(p, x)	(*(p) + x*POLY14(p + 1, x))
#define POLY16(p, x)	(*(p) + x*POLY15(p + 1, x))
#define POLY17(p, x)	(*(p) + x*POLY16(p + 1, x))
#define POLY18(p, x)	(*(p) + x*POLY17(p + 1, x))
#define POLY19(p, x)	(*(p) + x*POLY18(p + 1, x))
#define POLY20(p, x)	(*(p) + x*POLY19(p + 1, x))
#define POLY21(p, x)	(*(p) + x*POLY20(p + 1, x))
#define POLY22(p, x)	(*(p) + x*POLY21(p + 1, x))
#define POLY23(p, x)	(*(p) + x*POLY22(p + 1, x))
#define POLY24(p, x)	(*(p) + x*POLY23(p + 1, x))
#define POLY25(p, x)	(*(p) + x*POLY24(p + 1, x))




/* Ordinary poly macros (with a constant term) */


/* y = a */

#define POLY_0_C(x,p,y) { \
	(y)  = (p)[0]; \
}

/* y = a(x) + b */

#define POLY_1_C(x,p,y) { \
	(y)  = (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

/* y = a(x)^2 + b(x) + c */

#define POLY_2_C(x,p,y) { \
	(y)  = (p)[2]; (y) *= (x); \
	(y) += (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

/* y = a(x)^3 + b(x)^2 + c(x) + d */

#define POLY_3_C(x,p,y) { \
	(y)  = (p)[3]; (y) *= (x); \
	(y) += (p)[2]; (y) *= (x); \
	(y) += (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

#define POLY_4_C(x,p,y) { \
	(y)  = (p)[4]; (y) *= (x); \
	(y) += (p)[3]; (y) *= (x); \
	(y) += (p)[2]; (y) *= (x); \
	(y) += (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

#define POLY_5_C(x,p,y) { \
	(y)  = (p)[5]; (y) *= (x); \
	(y) += (p)[4]; (y) *= (x); \
	(y) += (p)[3]; (y) *= (x); \
	(y) += (p)[2]; (y) *= (x); \
	(y) += (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

#define POLY_6_C(x,p,y) { \
	(y)  = (p)[6]; (y) *= (x); \
	(y) += (p)[5]; (y) *= (x); \
	(y) += (p)[4]; (y) *= (x); \
	(y) += (p)[3]; (y) *= (x); \
	(y) += (p)[2]; (y) *= (x); \
	(y) += (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

#define POLY_7_C(x,p,y) { \
	(y)  = (p)[7]; (y) *= (x); \
	(y) += (p)[6]; (y) *= (x); \
	(y) += (p)[5]; (y) *= (x); \
	(y) += (p)[4]; (y) *= (x); \
	(y) += (p)[3]; (y) *= (x); \
	(y) += (p)[2]; (y) *= (x); \
	(y) += (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

#define POLY_8_C(x,p,y) { \
	(y)  = (p)[8]; (y) *= (x); \
	(y) += (p)[7]; (y) *= (x); \
	(y) += (p)[6]; (y) *= (x); \
	(y) += (p)[5]; (y) *= (x); \
	(y) += (p)[4]; (y) *= (x); \
	(y) += (p)[3]; (y) *= (x); \
	(y) += (p)[2]; (y) *= (x); \
	(y) += (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

#define POLY_9_C(x,p,y) { \
	(y)  = (p)[9]; (y) *= (x); \
	(y) += (p)[8]; (y) *= (x); \
	(y) += (p)[7]; (y) *= (x); \
	(y) += (p)[6]; (y) *= (x); \
	(y) += (p)[5]; (y) *= (x); \
	(y) += (p)[4]; (y) *= (x); \
	(y) += (p)[3]; (y) *= (x); \
	(y) += (p)[2]; (y) *= (x); \
	(y) += (p)[1]; (y) *= (x); \
	(y) += (p)[0]; \
}

#define POLY_10_C(x,p,y) { \
        (y)  = (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}


#define POLY_11_C(x,p,y) { \
        (y)  = (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}

#define POLY_12_C(x,p,y) { \
        (y)  = (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}

#define POLY_13_C(x,p,y) { \
        (y)  = (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}

#define POLY_14_C(x,p,y) { \
        (y)  = (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}

#define POLY_15_C(x,p,y) { \
        (y)  = (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_16_C(x,p,y) { \
        (y)  = (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_17_C(x,p,y) { \
        (y)  = (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_18_C(x,p,y) { \
        (y)  = (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_19_C(x,p,y) { \
        (y)  = (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_20_C(x,p,y) { \
        (y)  = (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_21_C(x,p,y) { \
        (y)  = (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_22_C(x,p,y) { \
        (y)  = (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_23_C(x,p,y) { \
        (y)  = (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_24_C(x,p,y) { \
        (y)  = (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_25_C(x,p,y) { \
        (y)  = (p)[25]; (y) *= (x); \
        (y) += (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_26_C(x,p,y) { \
        (y)  = (p)[26]; (y) *= (x); \
        (y) += (p)[25]; (y) *= (x); \
        (y) += (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_27_C(x,p,y) { \
        (y)  = (p)[27]; (y) *= (x); \
        (y) += (p)[26]; (y) *= (x); \
        (y) += (p)[25]; (y) *= (x); \
        (y) += (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_28_C(x,p,y) { \
        (y)  = (p)[28]; (y) *= (x); \
        (y) += (p)[27]; (y) *= (x); \
        (y) += (p)[26]; (y) *= (x); \
        (y) += (p)[25]; (y) *= (x); \
        (y) += (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_29_C(x,p,y) { \
        (y)  = (p)[29]; (y) *= (x); \
        (y) += (p)[28]; (y) *= (x); \
        (y) += (p)[27]; (y) *= (x); \
        (y) += (p)[26]; (y) *= (x); \
        (y) += (p)[25]; (y) *= (x); \
        (y) += (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_30_C(x,p,y) { \
        (y)  = (p)[30]; (y) *= (x); \
        (y) += (p)[29]; (y) *= (x); \
        (y) += (p)[28]; (y) *= (x); \
        (y) += (p)[27]; (y) *= (x); \
        (y) += (p)[26]; (y) *= (x); \
        (y) += (p)[25]; (y) *= (x); \
        (y) += (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}
#define POLY_31_C(x,p,y) { \
        (y)  = (p)[31]; (y) *= (x); \
        (y) += (p)[30]; (y) *= (x); \
        (y) += (p)[29]; (y) *= (x); \
        (y) += (p)[28]; (y) *= (x); \
        (y) += (p)[27]; (y) *= (x); \
        (y) += (p)[26]; (y) *= (x); \
        (y) += (p)[25]; (y) *= (x); \
        (y) += (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x); \
        (y) += (p)[0]; \
}

/* Ordinary poly macros (without a constant term) */


/* y = a(x) */

#define POLY_1(x,p,y) { \
	POLY_0_C((x),(p),(y)); \
	(y) *= (x); \
}

/* y = a(x)^2 + b(x) */

#define POLY_2(x,p,y) { \
	POLY_1_C((x),(p),(y)); \
	(y) *= (x); \
}

/* y = a(x)^3 + b(x)^2 + c(x) */

#define POLY_3(x,p,y) { \
	POLY_2_C((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_4(x,p,y) { \
	POLY_3_C((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_5(x,p,y) { \
	POLY_4_C((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_6(x,p,y) { \
	POLY_5_C((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_7(x,p,y) { \
	POLY_6_C((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_8(x,p,y) { \
	POLY_7_C((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_9(x,p,y) { \
	POLY_8_C((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_10(x,p,y) { \
        POLY_9_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_11(x,p,y) { \
        POLY_10_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_12(x,p,y) { \
        POLY_11_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_13(x,p,y) { \
        POLY_12_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_14(x,p,y) { \
        POLY_13_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_15(x,p,y) { \
        POLY_14_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_16(x,p,y) { \
        POLY_15_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_17(x,p,y) { \
        POLY_16_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_18(x,p,y) { \
        POLY_17_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_19(x,p,y) { \
        POLY_18_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_20(x,p,y) { \
        POLY_19_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_21(x,p,y) { \
        POLY_20_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_22(x,p,y) { \
        POLY_21_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_23(x,p,y) { \
        POLY_22_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_24(x,p,y) { \
        POLY_23_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_25(x,p,y) { \
        POLY_24_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_26(x,p,y) { \
        POLY_25_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_27(x,p,y) { \
        POLY_26_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_28(x,p,y) { \
        POLY_27_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_29(x,p,y) { \
        POLY_28_C((x),(p),(y)); \
        (y) *= (x); \
}

#define POLY_30(x,p,y) { \
        POLY_29_C((x),(p),(y)); \
        (y) *= (x); \
}



/* Ordinary poly macros without linear or constant terms */


#if MULTIPLE_ISSUE
#	define	POLY_2_X2(x,p,y) POLY_2_X2_M(x,p,y)
#	define	POLY_3_X2(x,p,y) POLY_3_X2_M(x,p,y)
#	define	POLY_4_X2(x,p,y) POLY_4_X2_M(x,p,y)
#	define	POLY_5_X2(x,p,y) POLY_5_X2_M(x,p,y)
#	define	POLY_6_X2(x,p,y) POLY_6_X2_M(x,p,y)
#	define	POLY_7_X2(x,p,y) POLY_7_X2_M(x,p,y)
#	define	POLY_8_X2(x,p,y) POLY_8_X2_M(x,p,y)
#	define	POLY_9_X2(x,p,y) POLY_9_X2_M(x,p,y)
	/* the polys below need multiple issue polynomials put together for them */
#	define	POLY_10_X2(x,p,y) POLY_10_WO_L_OR_C(x,p,y)
#	define	POLY_11_X2(x,p,y) POLY_11_WO_L_OR_C(x,p,y)
#	define	POLY_12_X2(x,p,y) POLY_12_WO_L_OR_C(x,p,y)
#	define	POLY_13_X2(x,p,y) POLY_13_WO_L_OR_C(x,p,y)
#	define	POLY_14_X2(x,p,y) POLY_14_WO_L_OR_C(x,p,y)
#	define	POLY_15_X2(x,p,y) POLY_15_WO_L_OR_C(x,p,y)
#	define	POLY_16_X2(x,p,y) POLY_16_WO_L_OR_C(x,p,y)
#	define	POLY_17_X2(x,p,y) POLY_17_WO_L_OR_C(x,p,y)
#else
#	define	POLY_2_X2(x,p,y) POLY_2_WO_L_OR_C(x,p,y)
#	define	POLY_3_X2(x,p,y) POLY_3_WO_L_OR_C(x,p,y)
#	define	POLY_4_X2(x,p,y) POLY_4_WO_L_OR_C(x,p,y)
#	define	POLY_5_X2(x,p,y) POLY_5_WO_L_OR_C(x,p,y)
#	define	POLY_6_X2(x,p,y) POLY_6_WO_L_OR_C(x,p,y)
#	define	POLY_7_X2(x,p,y) POLY_7_WO_L_OR_C(x,p,y)
#	define	POLY_8_X2(x,p,y) POLY_8_WO_L_OR_C(x,p,y)
#	define	POLY_9_X2(x,p,y) POLY_9_WO_L_OR_C(x,p,y)
#	define	POLY_10_X2(x,p,y) POLY_10_WO_L_OR_C(x,p,y)
#	define	POLY_11_X2(x,p,y) POLY_11_WO_L_OR_C(x,p,y)
#	define	POLY_12_X2(x,p,y) POLY_12_WO_L_OR_C(x,p,y)
#	define	POLY_13_X2(x,p,y) POLY_13_WO_L_OR_C(x,p,y)
#	define	POLY_14_X2(x,p,y) POLY_14_WO_L_OR_C(x,p,y)
#	define	POLY_15_X2(x,p,y) POLY_15_WO_L_OR_C(x,p,y)
#	define	POLY_16_X2(x,p,y) POLY_16_WO_L_OR_C(x,p,y)
#	define	POLY_17_X2(x,p,y) POLY_17_WO_L_OR_C(x,p,y)
#endif

/* Poly macros with no linear, constant or square term */


#if MULTIPLE_ISSUE

#       define POLY_3_X3(x,p,y)   POLY_3_X3_M(x,p,y)
#       define POLY_4_X3(x,p,y)   POLY_4_X3_M(x,p,y)
#       define POLY_5_X3(x,p,y)   POLY_5_X3_M(x,p,y)
#       define POLY_6_X3(x,p,y)   POLY_6_X3_M(x,p,y)
#       define POLY_7_X3(x,p,y)   POLY_7_X3_M(x,p,y)
#       define POLY_8_X3(x,p,y)   POLY_8_X3_M(x,p,y)
#       define POLY_9_X3(x,p,y)   POLY_9_X3_M(x,p,y)
#       define POLY_10_X3(x,p,y)   POLY_10_X3_M(x,p,y)
#       define POLY_11_X3(x,p,y)   POLY_11_X3_M(x,p,y)
#       define POLY_12_X3(x,p,y)   POLY_12_X3_M(x,p,y)
#       define POLY_13_X3(x,p,y)   POLY_13_X3_M(x,p,y)
#       define POLY_14_X3(x,p,y)   POLY_14_X3_M(x,p,y)

#       define POLY_15_X3(x,p,y)   POLY_15_WO_L_C_OR_SQ(x,p,y)
#       define POLY_16_X3(x,p,y)   POLY_16_WO_L_C_OR_SQ(x,p,y)
#       define POLY_17_X3(x,p,y)   POLY_17_WO_L_C_OR_SQ(x,p,y)
#       define POLY_18_X3(x,p,y)   POLY_18_WO_L_C_OR_SQ(x,p,y)
#       define POLY_19_X3(x,p,y)   POLY_19_WO_L_C_OR_SQ(x,p,y)
#       define POLY_20_X3(x,p,y)   POLY_20_WO_L_C_OR_SQ(x,p,y)
#       define POLY_21_X3(x,p,y)   POLY_21_WO_L_C_OR_SQ(x,p,y)
#       define POLY_22_X3(x,p,y)   POLY_22_WO_L_C_OR_SQ(x,p,y)
#       define POLY_23_X3(x,p,y)   POLY_23_WO_L_C_OR_SQ(x,p,y)
#       define POLY_24_X3(x,p,y)   POLY_24_WO_L_C_OR_SQ(x,p,y)

#else

#       define POLY_3_X3(x,p,y)   POLY_3_WO_L_C_OR_SQ(x,p,y)
#       define POLY_4_X3(x,p,y)   POLY_4_WO_L_C_OR_SQ(x,p,y)
#       define POLY_5_X3(x,p,y)   POLY_5_WO_L_C_OR_SQ(x,p,y)
#       define POLY_6_X3(x,p,y)   POLY_6_WO_L_C_OR_SQ(x,p,y)
#       define POLY_7_X3(x,p,y)   POLY_7_WO_L_C_OR_SQ(x,p,y)
#       define POLY_8_X3(x,p,y)   POLY_8_WO_L_C_OR_SQ(x,p,y)
#       define POLY_9_X3(x,p,y)   POLY_9_WO_L_C_OR_SQ(x,p,y)
#       define POLY_10_X3(x,p,y)   POLY_10_WO_L_C_OR_SQ(x,p,y)
#       define POLY_11_X3(x,p,y)   POLY_11_WO_L_C_OR_SQ(x,p,y)
#       define POLY_12_X3(x,p,y)   POLY_12_WO_L_C_OR_SQ(x,p,y)
#       define POLY_13_X3(x,p,y)   POLY_13_WO_L_C_OR_SQ(x,p,y)
#       define POLY_14_X3(x,p,y)   POLY_14_WO_L_C_OR_SQ(x,p,y)

#       define POLY_15_X3(x,p,y)   POLY_15_WO_L_C_OR_SQ(x,p,y)
#       define POLY_16_X3(x,p,y)   POLY_16_WO_L_C_OR_SQ(x,p,y)
#       define POLY_17_X3(x,p,y)   POLY_17_WO_L_C_OR_SQ(x,p,y)
#       define POLY_18_X3(x,p,y)   POLY_18_WO_L_C_OR_SQ(x,p,y)
#       define POLY_19_X3(x,p,y)   POLY_19_WO_L_C_OR_SQ(x,p,y)
#       define POLY_20_X3(x,p,y)   POLY_20_WO_L_C_OR_SQ(x,p,y)
#       define POLY_21_X3(x,p,y)   POLY_21_WO_L_C_OR_SQ(x,p,y)
#       define POLY_22_X3(x,p,y)   POLY_22_WO_L_C_OR_SQ(x,p,y)
#       define POLY_23_X3(x,p,y)   POLY_23_WO_L_C_OR_SQ(x,p,y)
#       define POLY_24_X3(x,p,y)   POLY_24_WO_L_C_OR_SQ(x,p,y)
#       define POLY_25_X3(x,p,y)   POLY_25_WO_L_C_OR_SQ(x,p,y)
#       define POLY_26_X3(x,p,y)   POLY_26_WO_L_C_OR_SQ(x,p,y)

#endif

/* labeling for these macros starts at p[1] instead of p[0] */

#define POLY_3_X3_M(x,p,y) \
    y = (((p)[1] *(x)) * ((x)*(x)))

#define POLY_4_X3_M(x,p,y) \
    y = (((p)[2] * (x)) + (p)[1]) * (((x)*(x))*(x))

#define POLY_5_X3_M(x,p,y) \
    y = ((((p)[3] * (x)) + (p)[2])*(((x)*(x))*((x)*(x)))) + \
          (((p)[1] * (x)) * ((x)*(x)))

#define POLY_6_X3_M(x,p,y) \
    y = (((p)[4] * (x)) + (p)[3]) * ((((x)*(x))*(x)) * ((x)*(x))) +\
          (((p)[2] * (x)) + (p)[1]) * (((x)*(x))*(x))

#define POLY_7_X3_M(x,p,y) \
    y = ((((p)[5] * (x)) + (p)[4]) * (((x)*(x))*((x)*(x))) * ((x)*(x))) +\
         ((((p)[3] * (x)) + (p)[2]) * (((x)*(x))*((x)*(x)))) +\
          (((p)[1] * (x)) * ((x)*(x)))

#define POLY_8_X3_M(x,p,y) \
  y = (((p)[6] * (x)) + (p)[5]) * ((((x)*(x))*(x)) * (((x)*(x))*((x)*(x)))) +\
(((p)[4] * (x)) + (p)[3]) * ((((x)*(x))*(x)) * ((x)*(x))) +\
          (((p)[2] * (x)) + (p)[1]) * (((x)*(x))*(x))



#define POLY_9_X3_M(x,p,y) \
    y = ((((p)[7] * (x)) + (p)[6]) * (((x)*(x))*((x)*(x))) * (((x)*(x))*((x)*(x)))) +\
        ((((p)[5] * (x)) + (p)[4]) * (((x)*(x))*((x)*(x))) * ((x)*(x))) +\
         ((((p)[3] * (x)) + (p)[2]) * (((x)*(x))*((x)*(x)))) +\
          (((p)[1] * (x)) * ((x)*(x)))

#define POLY_10_X3_M(x,p,y) \
    y = (((p)[8] * (x)) + (p)[7]) * ((((x)*(x))*(x)) * (((x)*(x))*((x)*(x)))* ((x)*(x))) +\
(((p)[6] * (x)) + (p)[5]) * ((((x)*(x))*(x)) * (((x)*(x))*((x)*(x)))) +\
       (((p)[4] * (x)) + (p)[3]) * ((((x)*(x))*(x)) * ((x)*(x))) +\
          (((p)[2] * (x)) + (p)[1]) * (((x)*(x))*(x))



#define POLY_11_X3_M(x,p,y) \
 y =   (((x)*(x))* ((x) * (p)[1])) + \
  (((x)*(x))*((x)*(x))* (((x) * (p)[3]) + (p)[2])) + \
  (((x)*(x))*((x)*(x))*((x)*(x))*  (((x) * (p)[5]) + (p)[4])) + \
  (((x)*(x))*((x)*(x))*((x)*(x))*((x)*(x))*(((x) * (p)[7]) + (p)[6])) +\
  (((x)*(x))*((x)*(x))*((x)*(x))*((x)*(x))*((x)*(x))*(((x) * (p)[9]) + (p)[8])) 

#define POLY_12_X3_M(x,p,y) \
  y =   ((((x)*(x)) * (x))* (((x) * (p)[2]) + (p)[1])) + \
  ((((x)*(x))*(((x)*(x)) * (x)))* (((x) * (p)[4]) + (p)[3])) + \
  (((((x)*(x)) * (x))* (((x)*(x))*((x)*(x))))*  (((x) * (p)[6]) + (p)[5])) +\
  (((((x)*(x))*((x)*(x)) * (x))* (((x)*(x))*((x)*(x))))*  (((x) * (p)[8]) + (p)[7])) +\
  (((((x)*(x))*((x)*(x))*((x)*(x)) * (x))* (((x)*(x))*((x)*(x))))*  (((x) * (p)[10]) + (p)[9])) 



/* Poly macros with linear and constant terms, maximum parallelization */

#define  POLY_0_ALL(x,p,y) POLY_0_C(x,p,y)

#if MULTIPLE_ISSUE
#       define  POLY_1_ALL(x,p,y) POLY_1_ALL_M(x,p,y)
#       define  POLY_2_ALL(x,p,y) POLY_2_ALL_M(x,p,y)
#       define  POLY_3_ALL(x,p,y) POLY_3_ALL_M(x,p,y)
#       define  POLY_4_ALL(x,p,y) POLY_4_ALL_M(x,p,y)
#       define  POLY_5_ALL(x,p,y) POLY_5_ALL_M(x,p,y)
#       define  POLY_6_ALL(x,p,y) POLY_6_ALL_M(x,p,y)
#       define  POLY_7_ALL(x,p,y) POLY_7_ALL_M(x,p,y)
#       define  POLY_8_ALL(x,p,y) POLY_8_ALL_M(x,p,y)
#       define  POLY_9_ALL(x,p,y) POLY_9_ALL_M(x,p,y)
#       define  POLY_10_ALL(x,p,y) POLY_10_ALL_M(x,p,y)
#       define  POLY_11_ALL(x,p,y) POLY_11_ALL_M(x,p,y)
#       define  POLY_12_ALL(x,p,y) POLY_12_ALL_M(x,p,y)
#       define  POLY_13_ALL(x,p,y) POLY_13_ALL_M(x,p,y)
#else
#       define  POLY_1_ALL(x,p,y) POLY_1_C(x,p,y)
#       define  POLY_2_ALL(x,p,y) POLY_2_C(x,p,y)
#       define  POLY_3_ALL(x,p,y) POLY_3_C(x,p,y)
#       define  POLY_4_ALL(x,p,y) POLY_4_C(x,p,y)
#       define  POLY_5_ALL(x,p,y) POLY_5_C(x,p,y)
#       define  POLY_6_ALL(x,p,y) POLY_6_C(x,p,y)
#define  POLY_7_ALL(x,p,y)  POLY_7_C(x,p,y)
#define  POLY_8_ALL(x,p,y)  POLY_8_C(x,p,y)
#define  POLY_9_ALL(x,p,y)  POLY_9_C(x,p,y)
#define  POLY_10_ALL(x,p,y) POLY_10_C(x,p,y)
#define  POLY_11_ALL(x,p,y) POLY_11_C(x,p,y)
#define  POLY_12_ALL(x,p,y) POLY_12_C(x,p,y)
#define  POLY_13_ALL(x,p,y) POLY_13_C(x,p,y)
#endif


#define  POLY_14_ALL(x,p,y) POLY_14_C(x,p,y)
#define  POLY_15_ALL(x,p,y) POLY_15_C(x,p,y)
#define  POLY_16_ALL(x,p,y) POLY_16_C(x,p,y)
#define  POLY_17_ALL(x,p,y) POLY_17_C(x,p,y)
#define  POLY_18_ALL(x,p,y) POLY_18_C(x,p,y)
#define  POLY_19_ALL(x,p,y) POLY_19_C(x,p,y)
#define  POLY_20_ALL(x,p,y) POLY_20_C(x,p,y)
#define  POLY_21_ALL(x,p,y) POLY_21_C(x,p,y)
#define  POLY_22_ALL(x,p,y) POLY_22_C(x,p,y)
#define  POLY_23_ALL(x,p,y) POLY_23_C(x,p,y)
#define  POLY_24_ALL(x,p,y) POLY_24_C(x,p,y)
#define  POLY_25_ALL(x,p,y) POLY_25_C(x,p,y)
#define  POLY_26_ALL(x,p,y) POLY_26_C(x,p,y)
#define  POLY_27_ALL(x,p,y) POLY_27_C(x,p,y)
#define  POLY_28_ALL(x,p,y) POLY_28_C(x,p,y)


/* Maximum parallelization, not forcing an alignment shift */

           /* y = ax + b */

#define POLY_1_ALL_M(x,p,y)  POLY_1_C(x,p,y)

           /* y = a*(x^2) + (bx + c)  */

#define POLY_2_ALL_M(x,p,y) \
         (y) = (p)[2] * ((x)*(x)) + ((p)[1]*(x) + (p)[0])

           /* y = x^2(ax + b) + (cx + d)    */

#define POLY_3_ALL_M(x,p,y) \
         (y) = ((p)[3]*(x) + (p)[2])*((x)*(x)) +  ((p)[1]*(x) + (p)[0])

          /* y = x^3(ax + b) + (x^2*c + (dx + e))   */

#define POLY_4_ALL_M(x,p,y) \
         (y) =  (((x)*(p)[1] + (p)[0]) + ((p)[2]*((x)*(x)))) + \
             (((x)*(x))*(x))*((p)[4]*(x) + (p)[3])

         /* y = ((x^4)(ax + b) + (x^2)(cx + d)) + (ex + f) */

#define POLY_5_ALL_M(x,p,y) \
         (y) = ((x)*(p)[1] + (p)[0]) +  \
        ((((x)*(p)[3] + (p)[2])* ((x)*(x))) + \
        (((x)*(p)[5] + (p)[4]) * ((x)*(x)) * ((x)*(x))))
    
        /* y = (x^4)(a(x^2) + bx) + (x^2(c(x^2) + dx) + (e(x^2) + fx + g)) */

#define POLY_6_ALL_M(x,p,y) \
         (y) = ((p)[5]*(x) + (p)[6]*((x)*(x)))*(((x)*(x))*((x)*(x))) + \
            ( (((p)[1]*(x) + (p)[0]) + (p)[2]*((x)*(x))) + \
            (((p)[3]*(x) + (p)[4]*((x)*(x)))*((x)*(x))) )


#define POLY_7_ALL_M(x,p,y) \
      (y) = ((p)[1]*(x) + (p)[0]) + \
      (((((p)[3]*(x) + (p)[2])*((x)*(x))) +\
       (((p)[5]*(x) + (p)[4])*(((x)*(x))*((x)*(x))))) +\
       (((p)[7]*(x) + (p)[6])*(((x)*(x))*((x)*(x)))*((x)*(x))))


#define POLY_8_ALL_M(x,p,y) \
      (y) = \
     (((p)[1]*(x) + (p)[0]) + ((((p)[3]*(x) + (p)[2])*((x)*(x))) +\
      (((p)[5]*(x) + (p)[4])*(((x)*(x))*((x)*(x)))))) + \
     ((((p)[7]*(x) + (p)[6])*((((x)*(x))*((x)*(x)))*((x)*(x)))) +\
      (p)[8]*((((x)*(x))*((x)*(x)))*(((x)*(x))*((x)*(x)))))


#define POLY_9_ALL_M(x,p,y) \
      (y) = \
         (((p)[1]*(x) + (p)[0]) + ((((p)[3]*(x) + (p)[2])*((x)*(x))) +\
          (((p)[5]*(x) + (p)[4])*(((x)*(x))*((x)*(x)))))) + \
         ((((p)[7]*(x) + (p)[6])*((((x)*(x))*((x)*(x)))*((x)*(x)))) +\
          (((p)[9]*(x) + (p)[8])*((((x)*(x))*((x)*(x)))*(((x)*(x))*((x)*(x))))))

#define POLY_10_ALL_M(x,p,y) \
      (y) = \
     ((p)[10]*((((x)*(x))*((x)*(x)))*(((x)*(x))*((x)*(x)))*((x)*(x)))) + \
     (((p)[1]*(x) + (p)[0]) + ((((p)[3]*(x) + (p)[2])*((x)*(x))) +\
     (((p)[5]*(x) + (p)[4])*(((x)*(x))*((x)*(x)))))) +\
     (((p)[7]*(x) + (p)[6]) * ((((x)*(x))*((x)*(x)))*((x)*(x))) +\
      (((p)[9]*(x) + (p)[8]) * ((((x)*(x))*((x)*(x)))*(((x)*(x))*((x)*(x))))))


#define POLY_11_ALL_M(x,p,y) \
       (y) = \
      (((p)[1]*(x) + (p)[0]) +\
       ((((p)[3]*(x) + (p)[2]) * ((x)*(x))) +\
      (((p)[5]*(x) + (p)[4])*(((x)*(x))*((x)*(x)))) )) + \
       (((p)[7]*(x) + (p)[6])*((((x)*(x))*((x)*(x)))*((x)*(x)))) + \
       ((((p)[9]*(x) + (p)[8]) + \
     (((p)[11]*(x) + (p)[10])*((x)*(x))))*((((x)*(x))*((x)*(x)))*(((x)*(x))*((x)*(x)))))

#define POLY_12_ALL_M(x,p,y) \
    (y) = \
    (((p)[1]*(x) + (p)[0]) + (p)[2]*((x)*(x))) + \
    ((p)[3] + (p)[4]*(x))*((x)*((x)*(x))) + \
    (((p)[6]*(x) + (p)[5]) + (p)[7]*((x)*(x)))*(((x)*(x))*((x)*(x))*(x)) + \
    ((((p)[9]*(x) + (p)[8]) + (p)[10]*((x)*(x))) + \
    ((p)[11] + (p)[12]*(x))*((x)*((x)*(x))))*((((x)*(x))*(x))*(((x)*(x))*((x)*(x))*(x)))


 /* note: this version of POLY_13_ALL is not the most parallelized, but
   it is necessary to reduce roundoff error in double precision asin/acos.
   The terms are added in pairs, smallest first, e.g.
      (ax + b) + ((((cx + d)x^2 + (ex + f)x^4) + (gx + h)x^6) + ..
 */

#define POLY_13_ALL_M(x,p,y) \
        (y) = \
(( (p)[1]*(x)) + (p)[0]) + \
(((((( (p)[3]*(x)) + (p)[2]) * ((x)*(x))) + ((( (p)[5]*(x)) + (p)[4]) * ( ((x)*(x))* ((x)*(x)))))) + \
((((( (p)[7]*(x)) + (p)[6]) * ( ((x)*(x))*( ((x)*(x))* ((x)*(x))))) + ((( (p)[9]*(x)) + (p)[8]) * (( ((x)*(x))* ((x)*(x))) * ( ((x)*(x))* ((x)*(x)))))) + \
(((( (p)[11]*(x)) + (p)[10]) * ( ((x)*(x))*( ((x)*(x))* ((x)*(x))) * ( ((x)*(x))* ((x)*(x))))) + \
((( (p)[13]*(x)) + (p)[12]) * (( ((x)*(x))*( ((x)*(x))* ((x)*(x)))) * ( ((x)*(x))*( ((x)*(x))* ((x)*(x)))))))))


/*
#define POLY_13_ALL_M(x,p,y) \
        (y) = \
((( (p)[1]*(x)) + (p)[0]) + \
(((( (p)[3]*(x)) + (p)[2]) * ((x)*(x))) + ((( (p)[5]*(x)) + (p)[4]) * ( ((x)*(x))* ((x)*(x)))))) + \
((((( (p)[7]*(x)) + (p)[6]) * ( ((x)*(x))*( ((x)*(x))* ((x)*(x))))) + ((( (p)[9]*(x)) + (p)[8]) * (( ((x)*(x))* ((x)*(x))) * ( ((x)*(x))* ((x)*(x)))))) + \
(((( (p)[11]*(x)) + (p)[10]) * ( ((x)*(x))*( ((x)*(x))* ((x)*(x))) * ( ((x)*(x))* ((x)*(x))))) + \
((( (p)[13]*(x)) + (p)[12]) * (( ((x)*(x))*( ((x)*(x))* ((x)*(x)))) * ( ((x)*(x))*( ((x)*(x))* ((x)*(x))))))))
*/







/* The macros below should allow maximum parallelization */


	/* y  =  a(x)^2  */

#define POLY_2_X2_M(x,p,y) \
		(y) = (x*x) * (p)[0]


	/* y  =  a(x)^2 + b(x)^3  =  (x)^2 * (a + b(x))  */

#define POLY_3_X2_M(x,p,y) \
		(y) = (x*x) * ((p)[0] + (p)[1]*(x))


	/* y  =  a(x)^2 + b(x)^3 + c(x)^4
          =  [ (x)^2 * (a + b(x)) ]  + c(x)^4  */

#define POLY_4_X2_M(x,p,y) \
		(y) = ((x*x) * ((p)[0] + (p)[1]*(x))) + \
			  (((x*x)*(x*x)) * (p)[2])


	/* y  =  a(x)^2 + b(x)^3 + c(x)^4 + d(x)^5
          =  [ (x)^2 * (a + b(x)) ]  +  [ (x)^4 * (c + d(x))  ]  */

#define POLY_5_X2_M(x,p,y) \
		(y) = ((x*x) * ((p)[0] + (p)[1]*(x))) + \
			  (((x*x)*(x*x)) * ((p)[2] + (p)[3]*(x)))


	/* y  =  a(x)^2 + b(x)^3 + c(x)^4 + d(x)^5 + e(x)^6
          =  a(x)^2 + ( [ (x)^2 * (b(x) + c(x)^2) ]
		            +   [ (x)^4 * (d(x) + e(x)^2) ] )  */
#define POLY_6_X2_M(x,p,y) \
		(y) = (((x*x)*(p)[0]) + \
			  (((x*x) *x)* ((p)[1] + (p)[2]*(x)))) + \
			  (((x*x)*(x*x)* x) * ((p)[3] + (p)[4]*(x))) 


	/* y  =  a(x)^2 + b(x)^3 + c(x)^4 + d(x)^5 + e(x)^6 + f(x)^7
		  =  (x)^2 * (a + b(x)) +
			 [ ((x)^2 * (x)^2 * (c + d(x))) +
			   ((x)^2 * (x)^2 * (x)^2 * (e + f(x))) ]
    */ 
#define POLY_7_X2_M(x,p,y) \
		(y) = ((x*x) * ((p)[0] + (p)[1]*(x))) + \
		      (((x*x)*(x*x) * ((p)[2] + (p)[3]*(x))) + \
		      ((x*x)*(x*x)*(x*x) * ((p)[4] + (p)[5]*(x))))


#define POLY_8_X2_M(x,p,y) \
		(y) = (((x*x) * ((p)[0] + (p)[1]*(x))) + \
		      ((x*x)*(x*x) * ((p)[2] + (p)[3]*(x)))) + \
		      ((x*x)*(x*x)*(x*x) * ((p)[4] + (p)[5]*(x) + (p)[6]*(x*x)))


#define POLY_9_X2_M(x,p,y) \
		(y) = (((x*x) * ((p)[0] + (p)[1]*(x))) + \
		      ((x*x)*(x*x) * ((p)[2] + (p)[3]*(x)))) + \
		      (((x*x)*(x*x)*(x*x) * ((p)[4] + (p)[5]*(x))) + \
		      ((x*x)*(x*x)*(x*x)*(x*x) * ((p)[6] + (p)[7]*(x))))



/* The macros below should enforce Horner's scheme */


/* y = a(x)^2 */

#define POLY_2_WO_L_OR_C(x,p,y) { \
	POLY_1((x),(p),(y)); \
	(y) *= (x); \
}

/* y = a(x)^3 + b(x)^2 */

#define POLY_3_WO_L_OR_C(x,p,y) { \
	POLY_2((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_4_WO_L_OR_C(x,p,y) { \
	POLY_3((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_5_WO_L_OR_C(x,p,y) { \
	POLY_4((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_6_WO_L_OR_C(x,p,y) { \
	POLY_5((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_7_WO_L_OR_C(x,p,y) { \
	POLY_6((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_8_WO_L_OR_C(x,p,y) { \
	POLY_7((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_9_WO_L_OR_C(x,p,y) { \
	POLY_8((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_10_WO_L_OR_C(x,p,y) { \
	POLY_9((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_11_WO_L_OR_C(x,p,y) { \
	POLY_10((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_12_WO_L_OR_C(x,p,y) { \
	POLY_11((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_13_WO_L_OR_C(x,p,y) { \
	POLY_12((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_14_WO_L_OR_C(x,p,y) { \
	POLY_13((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_15_WO_L_OR_C(x,p,y) { \
	POLY_14((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_16_WO_L_OR_C(x,p,y) { \
	POLY_15((x),(p),(y)); \
	(y) *= (x); \
}

#define POLY_17_WO_L_OR_C(x,p,y) { \
	POLY_16((x),(p),(y)); \
	(y) *= (x); \
}


/* Poly macros with no constant, linear or square term */
/* labeling for these macros starts at p[1] instead of p[0] */


#define POLY_3_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[1]; (y) *= (x); \
        (y) *= (x); (y) *= (x);

#define POLY_4_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[2]; (y) *= (x); \
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_5_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[3]; (y) *= (x); \
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_6_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[4]; (y) *= (x); \
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_7_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[5]; (y) *= (x); \
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_8_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[6]; (y) *= (x); \
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_9_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[7]; (y) *= (x); \
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_10_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[8]; (y) *= (x); \
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_11_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[9]; (y) *= (x); \
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_12_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[10]; (y) *= (x); \
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_13_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[11]; (y) *= (x); \
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_14_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[12]; (y) *= (x); \
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_15_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[13]; (y) *= (x); \
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_16_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[14]; (y) *= (x); \
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_17_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[15]; (y) *= (x); \
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_18_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[16]; (y) *= (x); \
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_19_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[17]; (y) *= (x); \
        (y) += (p)[16]; (y) *= (x);\
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_20_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[18]; (y) *= (x); \
        (y) += (p)[17]; (y) *= (x);\
        (y) += (p)[16]; (y) *= (x);\
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_21_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[19]; (y) *= (x); \
        (y) += (p)[18]; (y) *= (x);\
        (y) += (p)[17]; (y) *= (x);\
        (y) += (p)[16]; (y) *= (x);\
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_22_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[20]; (y) *= (x); \
        (y) += (p)[19]; (y) *= (x);\
        (y) += (p)[18]; (y) *= (x);\
        (y) += (p)[17]; (y) *= (x);\
        (y) += (p)[16]; (y) *= (x);\
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_23_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[21]; (y) *= (x); \
        (y) += (p)[20]; (y) *= (x);\
        (y) += (p)[19]; (y) *= (x);\
        (y) += (p)[18]; (y) *= (x);\
        (y) += (p)[17]; (y) *= (x);\
        (y) += (p)[16]; (y) *= (x);\
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_24_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[22]; (y) *= (x); \
        (y) += (p)[21]; (y) *= (x);\
        (y) += (p)[20]; (y) *= (x);\
        (y) += (p)[19]; (y) *= (x);\
        (y) += (p)[18]; (y) *= (x);\
        (y) += (p)[17]; (y) *= (x);\
        (y) += (p)[16]; (y) *= (x);\
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);

#define POLY_25_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[23]; (y) *= (x); \
        (y) += (p)[22]; (y) *= (x);\
        (y) += (p)[21]; (y) *= (x);\
        (y) += (p)[20]; (y) *= (x);\
        (y) += (p)[19]; (y) *= (x);\
        (y) += (p)[18]; (y) *= (x);\
        (y) += (p)[17]; (y) *= (x);\
        (y) += (p)[16]; (y) *= (x);\
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);


#define POLY_26_WO_L_C_OR_SQ(x,p,y) \
        (y) = (p)[24]; (y) *= (x); \
        (y) += (p)[23]; (y) *= (x);\
        (y) += (p)[22]; (y) *= (x);\
        (y) += (p)[21]; (y) *= (x);\
        (y) += (p)[20]; (y) *= (x);\
        (y) += (p)[19]; (y) *= (x);\
        (y) += (p)[18]; (y) *= (x);\
        (y) += (p)[17]; (y) *= (x);\
        (y) += (p)[16]; (y) *= (x);\
        (y) += (p)[15]; (y) *= (x);\
        (y) += (p)[14]; (y) *= (x);\
        (y) += (p)[13]; (y) *= (x);\
        (y) += (p)[12]; (y) *= (x);\
        (y) += (p)[11]; (y) *= (x);\
        (y) += (p)[10]; (y) *= (x);\
        (y) += (p)[9]; (y) *= (x);\
        (y) += (p)[8]; (y) *= (x);\
        (y) += (p)[7]; (y) *= (x);\
        (y) += (p)[6]; (y) *= (x);\
        (y) += (p)[5]; (y) *= (x);\
        (y) += (p)[4]; (y) *= (x);\
        (y) += (p)[3]; (y) *= (x);\
        (y) += (p)[2]; (y) *= (x);\
        (y) += (p)[1]; (y) *= (x);\
        (y) *= (x); (y) *= (x);




/* Ordinary poly macros with no constant term and unity low-order coef */


/* y = 1(x) */

#define POLY_1_U(x,p,y) { \
	(y) = (x); \
}

/* y = a(x)^2 + 1(x) */

#define POLY_2_U(x,p,y) { \
	POLY_1((x),(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

/* y = a(x)^3 + b(x)^2 + 1(x) */

#define POLY_3_U(x,p,y) { \
	POLY_2((x),(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define POLY_4_U(x,p,y) { \
	POLY_3((x),(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define POLY_5_U(x,p,y) { \
	POLY_4((x),(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define POLY_6_U(x,p,y) { \
	POLY_5((x),(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define POLY_7_U(x,p,y) { \
	POLY_6((x),(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define POLY_8_U(x,p,y) { \
	POLY_7((x),(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define POLY_9_U(x,p,y) { \
	POLY_8((x),(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}




/* Odd poly macros with no constant term */


/* y = a(x) */

#define ODD_POLY_1(x,p,y) { \
	POLY_0_C((x),(p),(y)); \
	(y) *= (x); \
}

/* y = a(x)^3 + b(x) */

#define ODD_POLY_3(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_1_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

/* y = a(x)^5 + b(x)^3 + c(x) */

#define ODD_POLY_5(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_2_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

/* y = a(x)^7 + b(x)^5 + c(x)^3 + d(x) */

#define ODD_POLY_7(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_3_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_9(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_4_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_11(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_5_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_13(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_6_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_15(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_7_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_17(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_8_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_19(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_9_C(x_sqrd,(p),(y)); \
	(y) *= (x); \
}



/* Odd poly macros without linear or constant terms */


/* y = a(x)^3 */

#define ODD_POLY_3_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_1(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

/* y = a(x)^5 + b(x)^3  */

#define ODD_POLY_5_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_2(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

/* y = a(x)^7 + b(x)^5 + c(x)^3 */

#define ODD_POLY_7_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_3(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_9_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_4(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_11_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_5(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_13_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_6(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_15_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_7(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_17_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_8(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_19_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_9(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_21_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_10(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_23_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_11(x_sqrd,(p),(y)); \
	(y) *= (x); \
}

#define ODD_POLY_25_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_12(x_sqrd,(p),(y)); \
	(y) *= (x); \
}
#define ODD_POLY_27_WO_L_OR_C(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_13(x_sqrd,(p),(y)); \
	(y) *= (x); \
}




/* Odd poly macros with no constant term and unity low-order coef */


/* y = 1(x) */

#define ODD_POLY_1_U(x,p,y) { \
	(y) = (x); \
}

/* y = a(x)^3 + 1(x) */

#define ODD_POLY_3_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_1(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

/* y = a(x)^5 + b(x)^3 + 1(x) */

#define ODD_POLY_5_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_2(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

/* y = a(x)^7 + b(x)^5 + c(x)^3 + 1(x) */

#define ODD_POLY_7_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_3(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define ODD_POLY_9_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_4(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define ODD_POLY_11_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_5(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define ODD_POLY_13_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_6(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define ODD_POLY_15_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_7(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define ODD_POLY_17_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_8(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define ODD_POLY_19_U(x,p,y) { \
	F_TYPE x_sqrd = (x) * (x); \
	POLY_9(x_sqrd,(p),(y)); \
	(y) *= (x); \
	(y) += (x); \
}

#define ODD_POLY_21_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_10(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_23_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_11(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_25_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_12(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_27_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_13(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_29_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_14(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_31_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_15(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_33_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_16(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_35_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_17(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_37_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_18(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_39_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_19(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_41_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_20(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_43_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_21(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_45_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_22(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_47_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_23(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_49_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_24(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_51_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_25(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_53_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_26(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}

#define ODD_POLY_55_U(x,p,y) { \
        F_TYPE x_sqrd = (x) * (x); \
        POLY_27(x_sqrd,(p),(y)); \
        (y) *= (x); \
        (y) += (x); \
}


#endif  /* POLY_MACROS_H */

