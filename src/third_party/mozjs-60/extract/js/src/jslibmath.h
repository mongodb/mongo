/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jslibmath_h
#define jslibmath_h

#include "mozilla/FloatingPoint.h"

#include <math.h>

#include "jsnum.h"

#include "vm/JSContext.h"

/*
 * Use system provided math routines.
 */

/* The right copysign function is not always named the same thing. */
#ifdef __GNUC__
#define js_copysign __builtin_copysign
#elif defined _WIN32
#define js_copysign _copysign
#else
#define js_copysign copysign
#endif

/* Consistency wrapper for platform deviations in fmod() */
static inline double
js_fmod(double d, double d2)
{
#ifdef XP_WIN
    /*
     * Workaround MS fmod bug where 42 % (1/0) => NaN, not 42.
     * Workaround MS fmod bug where -0 % -N => 0, not -0.
     */
    if ((mozilla::IsFinite(d) && mozilla::IsInfinite(d2)) ||
        (d == 0 && mozilla::IsFinite(d2))) {
        return d;
    }
#endif
    return fmod(d, d2);
}

namespace js {

inline double
NumberDiv(double a, double b)
{
    AutoUnsafeCallWithABI unsafe;
    if (b == 0) {
        if (a == 0 || mozilla::IsNaN(a)
#ifdef XP_WIN
            || mozilla::IsNaN(b) /* XXX MSVC miscompiles such that (NaN == 0) */
#endif
        )
            return JS::GenericNaN();

        if (mozilla::IsNegative(a) != mozilla::IsNegative(b))
            return mozilla::NegativeInfinity<double>();
        return mozilla::PositiveInfinity<double>();
    }

    return a / b;
}

inline double
NumberMod(double a, double b)
{
    AutoUnsafeCallWithABI unsafe;
    if (b == 0)
        return JS::GenericNaN();
    return js_fmod(a, b);
}

} // namespace js

#endif /* jslibmath_h */
