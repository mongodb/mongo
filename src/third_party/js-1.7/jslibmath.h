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
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   IBM Corp.
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

/*
 * By default all math calls go to fdlibm.  The defines for each platform
 * remap the math calls to native routines.
 */

#ifndef _LIBMATH_H
#define _LIBMATH_H

#include <math.h>
#include "jsconfig.h"

/*
 * Define on which platforms to use fdlibm. Not used by default under
 * assumption that native math library works unless proved guilty.
 * Plus there can be problems with endian-ness and such in fdlibm itself.
 *
 * fdlibm compatibility notes:
 * - fdlibm broken on OSF1/alpha
 */

#ifndef JS_USE_FDLIBM_MATH
#define JS_USE_FDLIBM_MATH 0
#endif

#if !JS_USE_FDLIBM_MATH

/*
 * Use system provided math routines.
 */

#define fd_acos acos
#define fd_asin asin
#define fd_atan atan
#define fd_atan2 atan2
#define fd_ceil ceil

/* The right copysign function is not always named the same thing. */
#if __GNUC__ >= 4 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 4)
#define fd_copysign __builtin_copysign
#elif defined WINCE
#define fd_copysign _copysign
#elif defined _WIN32
#if _MSC_VER < 1400
/* Try to work around apparent _copysign bustage in VC6 and VC7. */
#define fd_copysign js_copysign
extern double js_copysign(double, double);
#else
#define fd_copysign _copysign
#endif
#else
#define fd_copysign copysign
#endif

#define fd_cos cos
#define fd_exp exp
#define fd_fabs fabs
#define fd_floor floor
#define fd_fmod fmod
#define fd_log log
#define fd_pow pow
#define fd_sin sin
#define fd_sqrt sqrt
#define fd_tan tan

#else

/*
 * Use math routines in fdlibm.
 */

#undef __P
#ifdef __STDC__
#define __P(p)  p
#else
#define __P(p)  ()
#endif

#if (defined _WIN32 && !defined WINCE) || defined SUNOS4

#define fd_acos acos
#define fd_asin asin
#define fd_atan atan
#define fd_cos cos
#define fd_sin sin
#define fd_tan tan
#define fd_exp exp
#define fd_log log
#define fd_sqrt sqrt
#define fd_ceil ceil
#define fd_fabs fabs
#define fd_floor floor
#define fd_fmod fmod

extern double fd_atan2 __P((double, double));
extern double fd_copysign __P((double, double));
extern double fd_pow __P((double, double));

#elif defined IRIX

#define fd_acos acos
#define fd_asin asin
#define fd_atan atan
#define fd_exp exp
#define fd_log log
#define fd_log10 log10
#define fd_sqrt sqrt
#define fd_fabs fabs
#define fd_floor floor
#define fd_fmod fmod

extern double fd_cos __P((double));
extern double fd_sin __P((double));
extern double fd_tan __P((double));
extern double fd_atan2 __P((double, double));
extern double fd_pow __P((double, double));
extern double fd_ceil __P((double));
extern double fd_copysign __P((double, double));

#elif defined SOLARIS

#define fd_atan atan
#define fd_cos cos
#define fd_sin sin
#define fd_tan tan
#define fd_exp exp
#define fd_sqrt sqrt
#define fd_ceil ceil
#define fd_fabs fabs
#define fd_floor floor
#define fd_fmod fmod

extern double fd_acos __P((double));
extern double fd_asin __P((double));
extern double fd_log __P((double));
extern double fd_atan2 __P((double, double));
extern double fd_pow __P((double, double));
extern double fd_copysign __P((double, double));

#elif defined HPUX

#define fd_cos cos
#define fd_sin sin
#define fd_exp exp
#define fd_sqrt sqrt
#define fd_fabs fabs
#define fd_floor floor
#define fd_fmod fmod

extern double fd_ceil __P((double));
extern double fd_acos __P((double));
extern double fd_log __P((double));
extern double fd_atan2 __P((double, double));
extern double fd_tan __P((double));
extern double fd_pow __P((double, double));
extern double fd_asin __P((double));
extern double fd_atan __P((double));
extern double fd_copysign __P((double, double));

#elif defined(OSF1)

#define fd_acos acos
#define fd_asin asin
#define fd_atan atan
#define fd_copysign copysign
#define fd_cos cos
#define fd_exp exp
#define fd_fabs fabs
#define fd_fmod fmod
#define fd_sin sin
#define fd_sqrt sqrt
#define fd_tan tan

extern double fd_atan2 __P((double, double));
extern double fd_ceil __P((double));
extern double fd_floor __P((double));
extern double fd_log __P((double));
extern double fd_pow __P((double, double));

#elif defined(AIX)

#define fd_acos acos
#define fd_asin asin
#define fd_atan2 atan2
#define fd_copysign copysign
#define fd_cos cos
#define fd_exp exp
#define fd_fabs fabs
#define fd_floor floor
#define fd_fmod fmod
#define fd_log log
#define fd_sin sin
#define fd_sqrt sqrt

extern double fd_atan __P((double));
extern double fd_ceil __P((double));
extern double fd_pow __P((double,double));
extern double fd_tan __P((double));

#else /* other platform.. generic paranoid slow fdlibm */

extern double fd_acos __P((double));
extern double fd_asin __P((double));
extern double fd_atan __P((double));
extern double fd_cos __P((double));
extern double fd_sin __P((double));
extern double fd_tan __P((double));

extern double fd_exp __P((double));
extern double fd_log __P((double));
extern double fd_sqrt __P((double));

extern double fd_ceil __P((double));
extern double fd_fabs __P((double));
extern double fd_floor __P((double));
extern double fd_fmod __P((double, double));

extern double fd_atan2 __P((double, double));
extern double fd_pow __P((double, double));
extern double fd_copysign __P((double, double));

#endif

#endif /* JS_USE_FDLIBM_MATH */

#endif /* _LIBMATH_H */

