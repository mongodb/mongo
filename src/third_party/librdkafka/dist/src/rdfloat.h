/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2018, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <math.h>

/**
 * rd_dbl_eq0(a,b,prec)
 * Check two doubles for equality with the specified precision.
 * Use this instead of != and == for all floats/doubles.
 * More info:
 *  http://docs.sun.com/source/806-3568/ncg_goldberg.html
 */
static RD_INLINE RD_UNUSED int rd_dbl_eq0(double a, double b, double prec) {
        return fabs(a - b) < prec;
}

/* A default 'good' double-equality precision value.
 * This rather timid epsilon value is useful for tenths, hundreths,
 * and thousands parts, but not anything more precis than that.
 * If a higher precision is needed, use dbl_eq0 and dbl_eq0 directly
 * and specify your own precision. */
#define RD_DBL_EPSILON 0.00001

/**
 * rd_dbl_eq(a,b)
 * Same as rd_dbl_eq0() above but with a predefined 'good' precision.
 */
#define rd_dbl_eq(a, b) rd_dbl_eq0(a, b, RD_DBL_EPSILON)

/**
 * rd_dbl_ne(a,b)
 * Same as rd_dbl_eq() above but with reversed logic: not-equal.
 */
#define rd_dbl_ne(a, b) (!rd_dbl_eq0(a, b, RD_DBL_EPSILON))

/**
 * rd_dbl_zero(a)
 * Checks if the double `a' is zero (or close enough).
 */
#define rd_dbl_zero(a) rd_dbl_eq0(a, 0.0, RD_DBL_EPSILON)
