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

/* @(#)w_scalb.c 1.3 95/01/18 */
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
 * wrapper scalb(double x, double fn) is provide for
 * passing various standard test suite. One 
 * should use scalbn() instead.
 */

#include "fdlibm.h"

#include <errno.h>

#ifdef __STDC__
#ifdef _SCALB_INT
	double fd_scalb(double x, int fn)		/* wrapper scalb */
#else
	double fd_scalb(double x, double fn)	/* wrapper scalb */
#endif
#else
	double fd_scalb(x,fn)			/* wrapper scalb */
#ifdef _SCALB_INT
	double x; int fn;
#else
	double x,fn;
#endif
#endif
{
#ifdef _IEEE_LIBM
	return __ieee754_scalb(x,fn);
#else
	double z;
    int err;
	z = __ieee754_scalb(x,fn);
	if(_LIB_VERSION == _IEEE_) return z;
	if(!(fd_finite(z)||fd_isnan(z))&&fd_finite(x)) {
	    return __kernel_standard(x,(double)fn,32,&err); /* scalb overflow */
	}
	if(z==0.0&&z!=x) {
	    return __kernel_standard(x,(double)fn,33,&err); /* scalb underflow */
	} 
#ifndef _SCALB_INT
	if(!fd_finite(fn)) errno = ERANGE;
#endif
	return z;
#endif 
}
