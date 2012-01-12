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

/* @(#)s_rint.c 1.3 95/01/18 */
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

/*
 * rint(x)
 * Return x rounded to integral value according to the prevailing
 * rounding mode.
 * Method:
 *	Using floating addition.
 * Exception:
 *	Inexact flag raised if x not equal to rint(x).
 */

#include "fdlibm.h"

#ifdef __STDC__
static const double
#else
static double 
#endif
TWO52[2]={
  4.50359962737049600000e+15, /* 0x43300000, 0x00000000 */
 -4.50359962737049600000e+15, /* 0xC3300000, 0x00000000 */
};

#ifdef __STDC__
	double fd_rint(double x)
#else
	double fd_rint(x)
	double x;
#endif
{
	int i0,j0,sx;
	unsigned i,i1;
	double w,t;
        fd_twoints u;

        u.d = x;
	i0 =  __HI(u);
	sx = (i0>>31)&1;
	i1 =  __LO(u);
	j0 = ((i0>>20)&0x7ff)-0x3ff;
	if(j0<20) {
	    if(j0<0) { 	
		if(((i0&0x7fffffff)|i1)==0) return x;
		i1 |= (i0&0x0fffff);
		i0 &= 0xfffe0000;
		i0 |= ((i1|-(int)i1)>>12)&0x80000;
                u.d = x;
		__HI(u)=i0;
                x = u.d;
	        w = TWO52[sx]+x;
	        t =  w-TWO52[sx];
                u.d = t;
	        i0 = __HI(u);
	        __HI(u) = (i0&0x7fffffff)|(sx<<31);
                t = u.d;
	        return t;
	    } else {
		i = (0x000fffff)>>j0;
		if(((i0&i)|i1)==0) return x; /* x is integral */
		i>>=1;
		if(((i0&i)|i1)!=0) {
		    if(j0==19) i1 = 0x40000000; else
		    i0 = (i0&(~i))|((0x20000)>>j0);
		}
	    }
	} else if (j0>51) {
	    if(j0==0x400) return x+x;	/* inf or NaN */
	    else return x;		/* x is integral */
	} else {
	    i = ((unsigned)(0xffffffff))>>(j0-20);
	    if((i1&i)==0) return x;	/* x is integral */
	    i>>=1;
	    if((i1&i)!=0) i1 = (i1&(~i))|((0x40000000)>>(j0-20));
	}
        u.d = x;
	__HI(u) = i0;
	__LO(u) = i1;
        x = u.d;
	w = TWO52[sx]+x;
	return w-TWO52[sx];
}
