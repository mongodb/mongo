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
#define mozilla_imported_fdlibm_h

namespace fdlibm {

double	acos(double);
double	asin(double);
double	atan(double);
double	atan2(double, double);

double	cosh(double);
double	sinh(double);
double	tanh(double);

double	exp(double);
double	log(double);
double	log10(double);

double	pow(double, double);
double	sqrt(double);
double	fabs(double);

double	floor(double);
double	trunc(double);
double	ceil(double);

double	acosh(double);
double	asinh(double);
double	atanh(double);
double	cbrt(double);
double	expm1(double);
double	hypot(double, double);
double	log1p(double);
double	log2(double);
double	rint(double);
double	copysign(double, double);
double	nearbyint(double);
double	scalbn(double, int);

float	ceilf(float);
float	floorf(float);

float	nearbyintf(float);
float	rintf(float);
float	truncf(float);

} /* namespace fdlibm */

#endif /* mozilla_imported_fdlibm_h */
