/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Sun Microsystems, Inc.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/* @(#)fdlibm.h 1.5 95/01/18 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice 
 * is preserved.
 * ====================================================
 */

/* Modified defines start here.. */
#undef __LITTLE_ENDIAN

#ifdef _WIN32
#define huge myhuge
#define __LITTLE_ENDIAN
#endif

#ifdef XP_OS2
#define __LITTLE_ENDIAN
#endif

#if defined(linux) && (defined(__i386__) || defined(__x86_64__) || defined(__ia64) || (defined(__mips) && defined(__MIPSEL__)))
#define __LITTLE_ENDIAN
#endif

/* End here. The rest is the standard file. */

#ifdef SOLARIS	/* special setup for Sun test regime */
#if defined(i386) || defined(i486) || \
	defined(intel) || defined(x86) || defined(i86pc)
#define __LITTLE_ENDIAN
#endif
#endif

typedef union {
#ifdef __LITTLE_ENDIAN
    struct { int lo, hi; } ints;
#else
    struct { int hi, lo; } ints;
#endif
    double d;
} fd_twoints;

#define __HI(x) x.ints.hi
#define __LO(x) x.ints.lo

#undef __P
#ifdef __STDC__
#define	__P(p)	p
#else
#define	__P(p)	()
#endif

/*
 * ANSI/POSIX
 */

extern int signgam;

#define	MAXFLOAT	((float)3.40282346638528860e+38)

enum fdversion {fdlibm_ieee = -1, fdlibm_svid, fdlibm_xopen, fdlibm_posix};

#define _LIB_VERSION_TYPE enum fdversion
#define _LIB_VERSION _fdlib_version  

/* if global variable _LIB_VERSION is not desirable, one may 
 * change the following to be a constant by: 
 *	#define _LIB_VERSION_TYPE const enum version
 * In that case, after one initializes the value _LIB_VERSION (see
 * s_lib_version.c) during compile time, it cannot be modified
 * in the middle of a program
 */ 
extern  _LIB_VERSION_TYPE  _LIB_VERSION;

#define _IEEE_  fdlibm_ieee
#define _SVID_  fdlibm_svid
#define _XOPEN_ fdlibm_xopen
#define _POSIX_ fdlibm_posix

struct exception {
	int type;
	char *name;
	double arg1;
	double arg2;
	double retval;
};

#define	HUGE		MAXFLOAT

/* 
 * set X_TLOSS = pi*2**52, which is possibly defined in <values.h>
 * (one may replace the following line by "#include <values.h>")
 */

#define X_TLOSS		1.41484755040568800000e+16 

#define	DOMAIN		1
#define	SING		2
#define	OVERFLOW	3
#define	UNDERFLOW	4
#define	TLOSS		5
#define	PLOSS		6

/*
 * ANSI/POSIX
 */

extern double fd_acos __P((double));
extern double fd_asin __P((double));
extern double fd_atan __P((double));
extern double fd_atan2 __P((double, double));
extern double fd_cos __P((double));
extern double fd_sin __P((double));
extern double fd_tan __P((double));
 
extern double fd_cosh __P((double));
extern double fd_sinh __P((double));
extern double fd_tanh __P((double));

extern double fd_exp __P((double));
extern double fd_frexp __P((double, int *));
extern double fd_ldexp __P((double, int));
extern double fd_log __P((double));
extern double fd_log10 __P((double));
extern double fd_modf __P((double, double *));

extern double fd_pow __P((double, double));
extern double fd_sqrt __P((double));

extern double fd_ceil __P((double));
extern double fd_fabs __P((double));
extern double fd_floor __P((double));
extern double fd_fmod __P((double, double));

extern double fd_erf __P((double));
extern double fd_erfc __P((double));
extern double fd_gamma __P((double));
extern double fd_hypot __P((double, double));
extern int fd_isnan __P((double));
extern int fd_finite __P((double));
extern double fd_j0 __P((double));
extern double fd_j1 __P((double));
extern double fd_jn __P((int, double));
extern double fd_lgamma __P((double));
extern double fd_y0 __P((double));
extern double fd_y1 __P((double));
extern double fd_yn __P((int, double));

extern double fd_acosh __P((double));
extern double fd_asinh __P((double));
extern double fd_atanh __P((double));
extern double fd_cbrt __P((double));
extern double fd_logb __P((double));
extern double fd_nextafter __P((double, double));
extern double fd_remainder __P((double, double));
#ifdef _SCALB_INT
extern double fd_scalb __P((double, int));
#else
extern double fd_scalb __P((double, double));
#endif

extern int fd_matherr __P((struct exception *));

/*
 * IEEE Test Vector
 */
extern double significand __P((double));

/*
 * Functions callable from C, intended to support IEEE arithmetic.
 */
extern double fd_copysign __P((double, double));
extern int fd_ilogb __P((double));
extern double fd_rint __P((double));
extern double fd_scalbn __P((double, int));

/*
 * BSD math library entry points
 */
extern double fd_expm1 __P((double));
extern double fd_log1p __P((double));

/*
 * Reentrant version of gamma & lgamma; passes signgam back by reference
 * as the second argument; user must allocate space for signgam.
 */
#ifdef _REENTRANT
extern double gamma_r __P((double, int *));
extern double lgamma_r __P((double, int *));
#endif	/* _REENTRANT */

/* ieee style elementary functions */
extern double __ieee754_sqrt __P((double));			
extern double __ieee754_acos __P((double));			
extern double __ieee754_acosh __P((double));			
extern double __ieee754_log __P((double));			
extern double __ieee754_atanh __P((double));			
extern double __ieee754_asin __P((double));			
extern double __ieee754_atan2 __P((double,double));			
extern double __ieee754_exp __P((double));
extern double __ieee754_cosh __P((double));
extern double __ieee754_fmod __P((double,double));
extern double __ieee754_pow __P((double,double));
extern double __ieee754_lgamma_r __P((double,int *));
extern double __ieee754_gamma_r __P((double,int *));
extern double __ieee754_lgamma __P((double));
extern double __ieee754_gamma __P((double));
extern double __ieee754_log10 __P((double));
extern double __ieee754_sinh __P((double));
extern double __ieee754_hypot __P((double,double));
extern double __ieee754_j0 __P((double));
extern double __ieee754_j1 __P((double));
extern double __ieee754_y0 __P((double));
extern double __ieee754_y1 __P((double));
extern double __ieee754_jn __P((int,double));
extern double __ieee754_yn __P((int,double));
extern double __ieee754_remainder __P((double,double));
extern int    __ieee754_rem_pio2 __P((double,double*));
#ifdef _SCALB_INT
extern double __ieee754_scalb __P((double,int));
#else
extern double __ieee754_scalb __P((double,double));
#endif

/* fdlibm kernel function */
extern double __kernel_standard __P((double,double,int,int*));
extern double __kernel_sin __P((double,double,int));
extern double __kernel_cos __P((double,double));
extern double __kernel_tan __P((double,double,int));
extern int    __kernel_rem_pio2 __P((double*,double*,int,int,int,const int*));
