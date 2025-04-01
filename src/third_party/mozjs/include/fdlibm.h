/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/*
 * from: @(#)fdlibm.h 5.1 93/09/24
 * $FreeBSD$
 */

#ifndef mozilla_imported_fdlibm_h
#define	mozilla_imported_fdlibm_h

#ifndef M_PI_2
#define	M_PI_2		1.57079632679489661923	/* pi/2 */
#endif

#ifdef __cplusplus
extern "C" {
#endif

double	fdlibm_acos(double);
double	fdlibm_asin(double);
double	fdlibm_atan(double);
double	fdlibm_atan2(double, double);
double	fdlibm_cos(double);
double	fdlibm_sin(double);
double	fdlibm_tan(double);

double	fdlibm_cosh(double);
double	fdlibm_sinh(double);
double	fdlibm_tanh(double);

double	fdlibm_exp(double);
double	fdlibm_log(double);
double	fdlibm_log10(double);

double	fdlibm_pow(double, double);

double	fdlibm_ceil(double);
double	fdlibm_fabs(double);
double	fdlibm_floor(double);

double	fdlibm_acosh(double);
double	fdlibm_asinh(double);
double	fdlibm_atanh(double);
double	fdlibm_cbrt(double);
double	fdlibm_exp2(double);
double	fdlibm_expm1(double);
double	fdlibm_hypot(double, double);
double	fdlibm_log1p(double);
double	fdlibm_log2(double);
double	fdlibm_rint(double);
double	fdlibm_copysign(double, double);
double	fdlibm_nearbyint(double);
double	fdlibm_scalbn(double, int);
double	fdlibm_trunc(double);
float	fdlibm_acosf(float);
float	fdlibm_asinf(float);
float	fdlibm_atanf(float);
float	fdlibm_cosf(float);
float	fdlibm_sinf(float);
float	fdlibm_tanf(float);
float	fdlibm_exp2f(float);
float	fdlibm_expf(float);
float	fdlibm_log10f(float);
float	fdlibm_logf(float);
float	fdlibm_powf(float, float);
float	fdlibm_sqrtf(float);

float	fdlibm_ceilf(float);
float	fdlibm_fabsf(float);
float	fdlibm_floorf(float);
float	fdlibm_hypotf(float, float);
float	fdlibm_nearbyintf(float);
float	fdlibm_rintf(float);
float	fdlibm_scalbnf(float, int);
float	fdlibm_truncf(float);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* !mozilla_imported_fdlibm_h */
