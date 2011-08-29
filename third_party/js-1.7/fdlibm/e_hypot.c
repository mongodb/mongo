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

/* @(#)e_hypot.c 1.3 95/01/18 */
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

/* __ieee754_hypot(x,y)
 *
 * Method :                  
 *	If (assume round-to-nearest) z=x*x+y*y 
 *	has error less than sqrt(2)/2 ulp, than 
 *	sqrt(z) has error less than 1 ulp (exercise).
 *
 *	So, compute sqrt(x*x+y*y) with some care as 
 *	follows to get the error below 1 ulp:
 *
 *	Assume x>y>0;
 *	(if possible, set rounding to round-to-nearest)
 *	1. if x > 2y  use
 *		x1*x1+(y*y+(x2*(x+x1))) for x*x+y*y
 *	where x1 = x with lower 32 bits cleared, x2 = x-x1; else
 *	2. if x <= 2y use
 *		t1*y1+((x-y)*(x-y)+(t1*y2+t2*y))
 *	where t1 = 2x with lower 32 bits cleared, t2 = 2x-t1, 
 *	y1= y with lower 32 bits chopped, y2 = y-y1.
 *		
 *	NOTE: scaling may be necessary if some argument is too 
 *	      large or too tiny
 *
 * Special cases:
 *	hypot(x,y) is INF if x or y is +INF or -INF; else
 *	hypot(x,y) is NAN if x or y is NAN.
 *
 * Accuracy:
 * 	hypot(x,y) returns sqrt(x^2+y^2) with error less 
 * 	than 1 ulps (units in the last place) 
 */

#include "fdlibm.h"

#ifdef __STDC__
	double __ieee754_hypot(double x, double y)
#else
	double __ieee754_hypot(x,y)
	double x, y;
#endif
{
        fd_twoints ux, uy;
	double a=x,b=y,t1,t2,y1,y2,w;
	int j,k,ha,hb;
        
        ux.d = x; uy.d = y;
	ha = __HI(ux)&0x7fffffff;	/* high word of  x */
	hb = __HI(uy)&0x7fffffff;	/* high word of  y */
	if(hb > ha) {a=y;b=x;j=ha; ha=hb;hb=j;} else {a=x;b=y;}
        ux.d = a; uy.d = b;
	__HI(ux) = ha;	/* a <- |a| */
	__HI(uy) = hb;	/* b <- |b| */
        a = ux.d; b = uy.d;
	if((ha-hb)>0x3c00000) {return a+b;} /* x/y > 2**60 */
	k=0;
	if(ha > 0x5f300000) {	/* a>2**500 */
	   if(ha >= 0x7ff00000) {	/* Inf or NaN */
	       w = a+b;			/* for sNaN */
               ux.d = a; uy.d = b;
	       if(((ha&0xfffff)|__LO(ux))==0) w = a;
	       if(((hb^0x7ff00000)|__LO(uy))==0) w = b;
	       return w;
	   }
	   /* scale a and b by 2**-600 */
	   ha -= 0x25800000; hb -= 0x25800000;	k += 600;
           ux.d = a; uy.d = b;
	   __HI(ux) = ha;
	   __HI(uy) = hb;
           a = ux.d; b = uy.d;
	}
	if(hb < 0x20b00000) {	/* b < 2**-500 */
	    if(hb <= 0x000fffff) {	/* subnormal b or 0 */	
                uy.d = b;
		if((hb|(__LO(uy)))==0) return a;
		t1=0;
                ux.d = t1;
		__HI(ux) = 0x7fd00000;	/* t1=2^1022 */
                t1 = ux.d;
		b *= t1;
		a *= t1;
		k -= 1022;
	    } else {		/* scale a and b by 2^600 */
	        ha += 0x25800000; 	/* a *= 2^600 */
		hb += 0x25800000;	/* b *= 2^600 */
		k -= 600;
                ux.d = a; uy.d = b;
	   	__HI(ux) = ha;
	   	__HI(uy) = hb;
                a = ux.d; b = uy.d;
	    }
	}
    /* medium size a and b */
	w = a-b;
	if (w>b) {
	    t1 = 0;
            ux.d = t1;
	    __HI(ux) = ha;
            t1 = ux.d;
	    t2 = a-t1;
	    w  = fd_sqrt(t1*t1-(b*(-b)-t2*(a+t1)));
	} else {
	    a  = a+a;
	    y1 = 0;
            ux.d = y1;
	    __HI(ux) = hb;
            y1 = ux.d;
	    y2 = b - y1;
	    t1 = 0;
            ux.d = t1;
	    __HI(ux) = ha+0x00100000;
            t1 = ux.d;
	    t2 = a - t1;
	    w  = fd_sqrt(t1*y1-(w*(-w)-(t1*y2+t2*b)));
	}
	if(k!=0) {
	    t1 = 1.0;
            ux.d = t1;
	    __HI(ux) += (k<<20);
            t1 = ux.d;
	    return t1*w;
	} else return w;
}
