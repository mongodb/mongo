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

/* @(#)k_standard.c 1.3 95/01/18 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice 
 * is preserved.
 * ====================================================
 *
 */

#include "fdlibm.h"

/* XXX ugly hack to get msvc to link without error. */
#if _LIB_VERSION == _IEEE_ && !(defined(DARWIN) || defined(XP_MACOSX))
   int errno;
#  define EDOM 0
#  define ERANGE 0
#else
#  include <errno.h>
#endif


#ifndef _USE_WRITE
#include <stdio.h>			/* fputs(), stderr */
#define	WRITE2(u,v)	fputs(u, stderr)
#else	/* !defined(_USE_WRITE) */
#include <unistd.h>			/* write */
#define	WRITE2(u,v)	write(2, u, v)
#undef fflush
#endif	/* !defined(_USE_WRITE) */

static double zero = 0.0;	/* used as const */

/* 
 * Standard conformance (non-IEEE) on exception cases.
 * Mapping:
 *	1 -- acos(|x|>1)
 *	2 -- asin(|x|>1)
 *	3 -- atan2(+-0,+-0)
 *	4 -- hypot overflow
 *	5 -- cosh overflow
 *	6 -- exp overflow
 *	7 -- exp underflow
 *	8 -- y0(0)
 *	9 -- y0(-ve)
 *	10-- y1(0)
 *	11-- y1(-ve)
 *	12-- yn(0)
 *	13-- yn(-ve)
 *	14-- lgamma(finite) overflow
 *	15-- lgamma(-integer)
 *	16-- log(0)
 *	17-- log(x<0)
 *	18-- log10(0)
 *	19-- log10(x<0)
 *	20-- pow(0.0,0.0)
 *	21-- pow(x,y) overflow
 *	22-- pow(x,y) underflow
 *	23-- pow(0,negative) 
 *	24-- pow(neg,non-integral)
 *	25-- sinh(finite) overflow
 *	26-- sqrt(negative)
 *      27-- fmod(x,0)
 *      28-- remainder(x,0)
 *	29-- acosh(x<1)
 *	30-- atanh(|x|>1)
 *	31-- atanh(|x|=1)
 *	32-- scalb overflow
 *	33-- scalb underflow
 *	34-- j0(|x|>X_TLOSS)
 *	35-- y0(x>X_TLOSS)
 *	36-- j1(|x|>X_TLOSS)
 *	37-- y1(x>X_TLOSS)
 *	38-- jn(|x|>X_TLOSS, n)
 *	39-- yn(x>X_TLOSS, n)
 *	40-- gamma(finite) overflow
 *	41-- gamma(-integer)
 *	42-- pow(NaN,0.0)
 */


#ifdef __STDC__
	double __kernel_standard(double x, double y, int type, int *err)
#else
	double __kernel_standard(x,y,type, err)
     double x,y; int type;int *err;
#endif
{
	struct exception exc;
#ifndef HUGE_VAL	/* this is the only routine that uses HUGE_VAL */ 
#define HUGE_VAL inf
	double inf = 0.0;
        fd_twoints u;

        u.d = inf;
	__HI(u) = 0x7ff00000;	/* set inf to infinite */
        inf = u.d;
#endif

    *err = 0;

#ifdef _USE_WRITE
	(void) fflush(stdout);
#endif
	exc.arg1 = x;
	exc.arg2 = y;
	switch(type) {
	    case 1:
		/* acos(|x|>1) */
		exc.type = DOMAIN;
		exc.name = "acos";
		exc.retval = zero;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if(_LIB_VERSION == _SVID_) {
		    (void) WRITE2("acos: DOMAIN error\n", 19);
		  }
		  *err = EDOM;
		}
		break;
	    case 2:
		/* asin(|x|>1) */
		exc.type = DOMAIN;
		exc.name = "asin";
		exc.retval = zero;
		if(_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if(_LIB_VERSION == _SVID_) {
		    	(void) WRITE2("asin: DOMAIN error\n", 19);
		  }
		  *err = EDOM;
		}
		break;
	    case 3:
		/* atan2(+-0,+-0) */
		exc.arg1 = y;
		exc.arg2 = x;
		exc.type = DOMAIN;
		exc.name = "atan2";
		exc.retval = zero;
		if(_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if(_LIB_VERSION == _SVID_) {
			(void) WRITE2("atan2: DOMAIN error\n", 20);
		      }
		  *err = EDOM;
		}
		break;
	    case 4:
		/* hypot(finite,finite) overflow */
		exc.type = OVERFLOW;
		exc.name = "hypot";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = HUGE;
		else
		  exc.retval = HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 5:
		/* cosh(finite) overflow */
		exc.type = OVERFLOW;
		exc.name = "cosh";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = HUGE;
		else
		  exc.retval = HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 6:
		/* exp(finite) overflow */
		exc.type = OVERFLOW;
		exc.name = "exp";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = HUGE;
		else
		  exc.retval = HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 7:
		/* exp(finite) underflow */
		exc.type = UNDERFLOW;
		exc.name = "exp";
		exc.retval = zero;
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 8:
		/* y0(0) = -inf */
		exc.type = DOMAIN;	/* should be SING for IEEE */
		exc.name = "y0";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("y0: DOMAIN error\n", 17);
		      }
		  *err = EDOM;
		}
		break;
	    case 9:
		/* y0(x<0) = NaN */
		exc.type = DOMAIN;
		exc.name = "y0";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("y0: DOMAIN error\n", 17);
		      }
		  *err = EDOM;
		}
		break;
	    case 10:
		/* y1(0) = -inf */
		exc.type = DOMAIN;	/* should be SING for IEEE */
		exc.name = "y1";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("y1: DOMAIN error\n", 17);
		      }
		  *err = EDOM;
		}
		break;
	    case 11:
		/* y1(x<0) = NaN */
		exc.type = DOMAIN;
		exc.name = "y1";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("y1: DOMAIN error\n", 17);
		      }
		  *err = EDOM;
		}
		break;
	    case 12:
		/* yn(n,0) = -inf */
		exc.type = DOMAIN;	/* should be SING for IEEE */
		exc.name = "yn";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("yn: DOMAIN error\n", 17);
		      }
		  *err = EDOM;
		}
		break;
	    case 13:
		/* yn(x<0) = NaN */
		exc.type = DOMAIN;
		exc.name = "yn";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("yn: DOMAIN error\n", 17);
		      }
		  *err = EDOM;
		}
		break;
	    case 14:
		/* lgamma(finite) overflow */
		exc.type = OVERFLOW;
		exc.name = "lgamma";
                if (_LIB_VERSION == _SVID_)
                  exc.retval = HUGE;
                else
                  exc.retval = HUGE_VAL;
                if (_LIB_VERSION == _POSIX_)
			*err = ERANGE;
                else if (!fd_matherr(&exc)) {
                        *err = ERANGE;
		}
		break;
	    case 15:
		/* lgamma(-integer) or lgamma(0) */
		exc.type = SING;
		exc.name = "lgamma";
                if (_LIB_VERSION == _SVID_)
                  exc.retval = HUGE;
                else
                  exc.retval = HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("lgamma: SING error\n", 19);
		      }
		  *err = EDOM;
		}
		break;
	    case 16:
		/* log(0) */
		exc.type = SING;
		exc.name = "log";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("log: SING error\n", 16);
		      }
		  *err = EDOM;
		}
		break;
	    case 17:
		/* log(x<0) */
		exc.type = DOMAIN;
		exc.name = "log";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("log: DOMAIN error\n", 18);
		      }
		  *err = EDOM;
		}
		break;
	    case 18:
		/* log10(0) */
		exc.type = SING;
		exc.name = "log10";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("log10: SING error\n", 18);
		      }
		  *err = EDOM;
		}
		break;
	    case 19:
		/* log10(x<0) */
		exc.type = DOMAIN;
		exc.name = "log10";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = -HUGE;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("log10: DOMAIN error\n", 20);
		      }
		  *err = EDOM;
		}
		break;
	    case 20:
		/* pow(0.0,0.0) */
		/* error only if _LIB_VERSION == _SVID_ */
		exc.type = DOMAIN;
		exc.name = "pow";
		exc.retval = zero;
		if (_LIB_VERSION != _SVID_) exc.retval = 1.0;
		else if (!fd_matherr(&exc)) {
			(void) WRITE2("pow(0,0): DOMAIN error\n", 23);
			*err = EDOM;
		}
		break;
	    case 21:
		/* pow(x,y) overflow */
		exc.type = OVERFLOW;
		exc.name = "pow";
		if (_LIB_VERSION == _SVID_) {
		  exc.retval = HUGE;
		  y *= 0.5;
		  if(x<zero&&fd_rint(y)!=y) exc.retval = -HUGE;
		} else {
		  exc.retval = HUGE_VAL;
		  y *= 0.5;
		  if(x<zero&&fd_rint(y)!=y) exc.retval = -HUGE_VAL;
		}
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 22:
		/* pow(x,y) underflow */
		exc.type = UNDERFLOW;
		exc.name = "pow";
		exc.retval =  zero;
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 23:
		/* 0**neg */
		exc.type = DOMAIN;
		exc.name = "pow";
		if (_LIB_VERSION == _SVID_) 
		  exc.retval = zero;
		else
		  exc.retval = -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("pow(0,neg): DOMAIN error\n", 25);
		      }
		  *err = EDOM;
		}
		break;
	    case 24:
		/* neg**non-integral */
		exc.type = DOMAIN;
		exc.name = "pow";
		if (_LIB_VERSION == _SVID_) 
		    exc.retval = zero;
		else 
		    exc.retval = zero/zero;	/* X/Open allow NaN */
		if (_LIB_VERSION == _POSIX_) 
		   *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("neg**non-integral: DOMAIN error\n", 32);
		      }
		  *err = EDOM;
		}
		break;
	    case 25:
		/* sinh(finite) overflow */
		exc.type = OVERFLOW;
		exc.name = "sinh";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = ( (x>zero) ? HUGE : -HUGE);
		else
		  exc.retval = ( (x>zero) ? HUGE_VAL : -HUGE_VAL);
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 26:
		/* sqrt(x<0) */
		exc.type = DOMAIN;
		exc.name = "sqrt";
		if (_LIB_VERSION == _SVID_)
		  exc.retval = zero;
		else
		  exc.retval = zero/zero;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("sqrt: DOMAIN error\n", 19);
		      }
		  *err = EDOM;
		}
		break;
            case 27:
                /* fmod(x,0) */
                exc.type = DOMAIN;
                exc.name = "fmod";
                if (_LIB_VERSION == _SVID_)
                    exc.retval = x;
		else
		    exc.retval = zero/zero;
                if (_LIB_VERSION == _POSIX_)
                  *err = EDOM;
                else if (!fd_matherr(&exc)) {
                  if (_LIB_VERSION == _SVID_) {
                    (void) WRITE2("fmod:  DOMAIN error\n", 20);
                  }
                  *err = EDOM;
                }
                break;
            case 28:
                /* remainder(x,0) */
                exc.type = DOMAIN;
                exc.name = "remainder";
                exc.retval = zero/zero;
                if (_LIB_VERSION == _POSIX_)
                  *err = EDOM;
                else if (!fd_matherr(&exc)) {
                  if (_LIB_VERSION == _SVID_) {
                    (void) WRITE2("remainder: DOMAIN error\n", 24);
                  }
                  *err = EDOM;
                }
                break;
            case 29:
                /* acosh(x<1) */
                exc.type = DOMAIN;
                exc.name = "acosh";
                exc.retval = zero/zero;
                if (_LIB_VERSION == _POSIX_)
                  *err = EDOM;
                else if (!fd_matherr(&exc)) {
                  if (_LIB_VERSION == _SVID_) {
                    (void) WRITE2("acosh: DOMAIN error\n", 20);
                  }
                  *err = EDOM;
                }
                break;
            case 30:
                /* atanh(|x|>1) */
                exc.type = DOMAIN;
                exc.name = "atanh";
                exc.retval = zero/zero;
                if (_LIB_VERSION == _POSIX_)
                  *err = EDOM;
                else if (!fd_matherr(&exc)) {
                  if (_LIB_VERSION == _SVID_) {
                    (void) WRITE2("atanh: DOMAIN error\n", 20);
                  }
                  *err = EDOM;
                }
                break;
            case 31:
                /* atanh(|x|=1) */
                exc.type = SING;
                exc.name = "atanh";
		exc.retval = x/zero;	/* sign(x)*inf */
                if (_LIB_VERSION == _POSIX_)
                  *err = EDOM;
                else if (!fd_matherr(&exc)) {
                  if (_LIB_VERSION == _SVID_) {
                    (void) WRITE2("atanh: SING error\n", 18);
                  }
                  *err = EDOM;
                }
                break;
	    case 32:
		/* scalb overflow; SVID also returns +-HUGE_VAL */
		exc.type = OVERFLOW;
		exc.name = "scalb";
		exc.retval = x > zero ? HUGE_VAL : -HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 33:
		/* scalb underflow */
		exc.type = UNDERFLOW;
		exc.name = "scalb";
		exc.retval = fd_copysign(zero,x);
		if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
		else if (!fd_matherr(&exc)) {
			*err = ERANGE;
		}
		break;
	    case 34:
		/* j0(|x|>X_TLOSS) */
                exc.type = TLOSS;
                exc.name = "j0";
                exc.retval = zero;
                if (_LIB_VERSION == _POSIX_)
                        *err = ERANGE;
                else if (!fd_matherr(&exc)) {
                        if (_LIB_VERSION == _SVID_) {
                                (void) WRITE2(exc.name, 2);
                                (void) WRITE2(": TLOSS error\n", 14);
                        }
                        *err = ERANGE;
                }        
		break;
	    case 35:
		/* y0(x>X_TLOSS) */
                exc.type = TLOSS;
                exc.name = "y0";
                exc.retval = zero;
                if (_LIB_VERSION == _POSIX_)
                        *err = ERANGE;
                else if (!fd_matherr(&exc)) {
                        if (_LIB_VERSION == _SVID_) {
                                (void) WRITE2(exc.name, 2);
                                (void) WRITE2(": TLOSS error\n", 14);
                        }
                        *err = ERANGE;
                }        
		break;
	    case 36:
		/* j1(|x|>X_TLOSS) */
                exc.type = TLOSS;
                exc.name = "j1";
                exc.retval = zero;
                if (_LIB_VERSION == _POSIX_)
                        *err = ERANGE;
                else if (!fd_matherr(&exc)) {
                        if (_LIB_VERSION == _SVID_) {
                                (void) WRITE2(exc.name, 2);
                                (void) WRITE2(": TLOSS error\n", 14);
                        }
                        *err = ERANGE;
                }        
		break;
	    case 37:
		/* y1(x>X_TLOSS) */
                exc.type = TLOSS;
                exc.name = "y1";
                exc.retval = zero;
                if (_LIB_VERSION == _POSIX_)
                        *err = ERANGE;
                else if (!fd_matherr(&exc)) {
                        if (_LIB_VERSION == _SVID_) {
                                (void) WRITE2(exc.name, 2);
                                (void) WRITE2(": TLOSS error\n", 14);
                        }
                        *err = ERANGE;
                }        
		break;
	    case 38:
		/* jn(|x|>X_TLOSS) */
                exc.type = TLOSS;
                exc.name = "jn";
                exc.retval = zero;
                if (_LIB_VERSION == _POSIX_)
                        *err = ERANGE;
                else if (!fd_matherr(&exc)) {
                        if (_LIB_VERSION == _SVID_) {
                                (void) WRITE2(exc.name, 2);
                                (void) WRITE2(": TLOSS error\n", 14);
                        }
                        *err = ERANGE;
                }        
		break;
	    case 39:
		/* yn(x>X_TLOSS) */
                exc.type = TLOSS;
                exc.name = "yn";
                exc.retval = zero;
                if (_LIB_VERSION == _POSIX_)
                        *err = ERANGE;
                else if (!fd_matherr(&exc)) {
                        if (_LIB_VERSION == _SVID_) {
                                (void) WRITE2(exc.name, 2);
                                (void) WRITE2(": TLOSS error\n", 14);
                        }
                        *err = ERANGE;
                }        
		break;
	    case 40:
		/* gamma(finite) overflow */
		exc.type = OVERFLOW;
		exc.name = "gamma";
                if (_LIB_VERSION == _SVID_)
                  exc.retval = HUGE;
                else
                  exc.retval = HUGE_VAL;
                if (_LIB_VERSION == _POSIX_)
		  *err = ERANGE;
                else if (!fd_matherr(&exc)) {
                  *err = ERANGE;
                }
		break;
	    case 41:
		/* gamma(-integer) or gamma(0) */
		exc.type = SING;
		exc.name = "gamma";
                if (_LIB_VERSION == _SVID_)
                  exc.retval = HUGE;
                else
                  exc.retval = HUGE_VAL;
		if (_LIB_VERSION == _POSIX_)
		  *err = EDOM;
		else if (!fd_matherr(&exc)) {
		  if (_LIB_VERSION == _SVID_) {
			(void) WRITE2("gamma: SING error\n", 18);
		      }
		  *err = EDOM;
		}
		break;
	    case 42:
		/* pow(NaN,0.0) */
		/* error only if _LIB_VERSION == _SVID_ & _XOPEN_ */
		exc.type = DOMAIN;
		exc.name = "pow";
		exc.retval = x;
		if (_LIB_VERSION == _IEEE_ ||
		    _LIB_VERSION == _POSIX_) exc.retval = 1.0;
		else if (!fd_matherr(&exc)) {
			*err = EDOM;
		}
		break;
	}
	return exc.retval; 
}
