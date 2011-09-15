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

/* @(#)e_cosh.c 1.3 95/01/18 */
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

/* __ieee754_cosh(x)
 * Method : 
 * mathematically cosh(x) if defined to be (exp(x)+exp(-x))/2
 *	1. Replace x by |x| (cosh(x) = cosh(-x)). 
 *	2. 
 *		                                        [ exp(x) - 1 ]^2 
 *	    0        <= x <= ln2/2  :  cosh(x) := 1 + -------------------
 *			       			           2*exp(x)
 *
 *		                                  exp(x) +  1/exp(x)
 *	    ln2/2    <= x <= 22     :  cosh(x) := -------------------
 *			       			          2
 *	    22       <= x <= lnovft :  cosh(x) := exp(x)/2 
 *	    lnovft   <= x <= ln2ovft:  cosh(x) := exp(x/2)/2 * exp(x/2)
 *	    ln2ovft  <  x	    :  cosh(x) := huge*huge (overflow)
 *
 * Special cases:
 *	cosh(x) is |x| if x is +INF, -INF, or NaN.
 *	only cosh(0)=1 is exact for finite x.
 */

#include "fdlibm.h"

#ifdef _WIN32
#define huge myhuge
#endif

#ifdef __STDC__
static const double one = 1.0, half=0.5, really_big = 1.0e300;
#else
static double one = 1.0, half=0.5, really_big = 1.0e300;
#endif

#ifdef __STDC__
	double __ieee754_cosh(double x)
#else
	double __ieee754_cosh(x)
	double x;
#endif
{	
        fd_twoints u;
	double t,w;
	int ix;
	unsigned lx;
        
    /* High word of |x|. */
        u.d = x;
	ix = __HI(u);
	ix &= 0x7fffffff;

    /* x is INF or NaN */
	if(ix>=0x7ff00000) return x*x;	

    /* |x| in [0,0.5*ln2], return 1+expm1(|x|)^2/(2*exp(|x|)) */
	if(ix<0x3fd62e43) {
	    t = fd_expm1(fd_fabs(x));
	    w = one+t;
	    if (ix<0x3c800000) return w;	/* cosh(tiny) = 1 */
	    return one+(t*t)/(w+w);
	}

    /* |x| in [0.5*ln2,22], return (exp(|x|)+1/exp(|x|)/2; */
	if (ix < 0x40360000) {
		t = __ieee754_exp(fd_fabs(x));
		return half*t+half/t;
	}

    /* |x| in [22, log(maxdouble)] return half*exp(|x|) */
	if (ix < 0x40862E42)  return half*__ieee754_exp(fd_fabs(x));

    /* |x| in [log(maxdouble), overflowthresold] */
	lx = *( (((*(unsigned*)&one)>>29)) + (unsigned*)&x);
	if (ix<0x408633CE || 
	      (ix==0x408633ce)&&(lx<=(unsigned)0x8fb9f87d)) {
	    w = __ieee754_exp(half*fd_fabs(x));
	    t = half*w;
	    return t*w;
	}

    /* |x| > overflowthresold, cosh(x) overflow */
	return really_big*really_big;
}
