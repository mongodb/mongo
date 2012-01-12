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
 * JS math package.
 */
#include "jsstddef.h"
#include "jslibmath.h"
#include <stdlib.h>
#include "jstypes.h"
#include "jslong.h"
#include "prmjtime.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jslock.h"
#include "jsmath.h"
#include "jsnum.h"
#include "jsobj.h"

#ifndef M_E
#define M_E             2.7182818284590452354
#endif
#ifndef M_LOG2E
#define M_LOG2E         1.4426950408889634074
#endif
#ifndef M_LOG10E
#define M_LOG10E        0.43429448190325182765
#endif
#ifndef M_LN2
#define M_LN2           0.69314718055994530942
#endif
#ifndef M_LN10
#define M_LN10          2.30258509299404568402
#endif
#ifndef M_PI
#define M_PI            3.14159265358979323846
#endif
#ifndef M_SQRT2
#define M_SQRT2         1.41421356237309504880
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2       0.70710678118654752440
#endif

static JSConstDoubleSpec math_constants[] = {
    {M_E,       "E",            0, {0,0,0}},
    {M_LOG2E,   "LOG2E",        0, {0,0,0}},
    {M_LOG10E,  "LOG10E",       0, {0,0,0}},
    {M_LN2,     "LN2",          0, {0,0,0}},
    {M_LN10,    "LN10",         0, {0,0,0}},
    {M_PI,      "PI",           0, {0,0,0}},
    {M_SQRT2,   "SQRT2",        0, {0,0,0}},
    {M_SQRT1_2, "SQRT1_2",      0, {0,0,0}},
    {0,0,0,{0,0,0}}
};

JSClass js_MathClass = {
    js_Math_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Math),
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,   JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSBool
math_abs(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_fabs(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_acos(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_acos(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_asin(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_asin(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_atan(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_atan(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_atan2(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, y, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    if (!js_ValueToNumber(cx, argv[1], &y))
        return JS_FALSE;
#if !JS_USE_FDLIBM_MATH && defined(_MSC_VER)
    /*
     * MSVC's atan2 does not yield the result demanded by ECMA when both x
     * and y are infinite.
     * - The result is a multiple of pi/4.
     * - The sign of x determines the sign of the result.
     * - The sign of y determines the multiplicator, 1 or 3.
     */
    if (JSDOUBLE_IS_INFINITE(x) && JSDOUBLE_IS_INFINITE(y)) {
        z = fd_copysign(M_PI / 4, x);
        if (y < 0)
            z *= 3;
        return js_NewDoubleValue(cx, z, rval);
    }
#endif
    z = fd_atan2(x, y);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_ceil(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_ceil(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_cos(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_cos(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_exp(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
#ifdef _WIN32
    if (!JSDOUBLE_IS_NaN(x)) {
        if (x == *cx->runtime->jsPositiveInfinity) {
            *rval = DOUBLE_TO_JSVAL(cx->runtime->jsPositiveInfinity);
            return JS_TRUE;
        }
        if (x == *cx->runtime->jsNegativeInfinity) {
            *rval = JSVAL_ZERO;
            return JS_TRUE;
        }
    }
#endif
    z = fd_exp(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_floor(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_floor(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_log(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_log(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_max(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z = *cx->runtime->jsNegativeInfinity;
    uintN i;

    if (argc == 0) {
        *rval = DOUBLE_TO_JSVAL(cx->runtime->jsNegativeInfinity);
        return JS_TRUE;
    }
    for (i = 0; i < argc; i++) {
        if (!js_ValueToNumber(cx, argv[i], &x))
            return JS_FALSE;
        if (JSDOUBLE_IS_NaN(x)) {
            *rval = DOUBLE_TO_JSVAL(cx->runtime->jsNaN);
            return JS_TRUE;
        }
        if (x == 0 && x == z && fd_copysign(1.0, z) == -1)
            z = x;
        else
            z = (x > z) ? x : z;
    }
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_min(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z = *cx->runtime->jsPositiveInfinity;
    uintN i;

    if (argc == 0) {
        *rval = DOUBLE_TO_JSVAL(cx->runtime->jsPositiveInfinity);
        return JS_TRUE;
    }
    for (i = 0; i < argc; i++) {
        if (!js_ValueToNumber(cx, argv[i], &x))
            return JS_FALSE;
        if (JSDOUBLE_IS_NaN(x)) {
            *rval = DOUBLE_TO_JSVAL(cx->runtime->jsNaN);
            return JS_TRUE;
        }
        if (x == 0 && x == z && fd_copysign(1.0,x) == -1)
            z = x;
        else
            z = (x < z) ? x : z;
    }
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_pow(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, y, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    if (!js_ValueToNumber(cx, argv[1], &y))
        return JS_FALSE;
#if !JS_USE_FDLIBM_MATH
    /*
     * Because C99 and ECMA specify different behavior for pow(),
     * we need to wrap the libm call to make it ECMA compliant.
     */
    if (!JSDOUBLE_IS_FINITE(y) && (x == 1.0 || x == -1.0)) {
        *rval = DOUBLE_TO_JSVAL(cx->runtime->jsNaN);
        return JS_TRUE;
    }
    /* pow(x, +-0) is always 1, even for x = NaN. */
    if (y == 0) {
        *rval = JSVAL_ONE;
        return JS_TRUE;
    }
#endif
    z = fd_pow(x, y);
    return js_NewNumberValue(cx, z, rval);
}

/*
 * Math.random() support, lifted from java.util.Random.java.
 */
static void
random_setSeed(JSRuntime *rt, int64 seed)
{
    int64 tmp;

    JSLL_I2L(tmp, 1000);
    JSLL_DIV(seed, seed, tmp);
    JSLL_XOR(tmp, seed, rt->rngMultiplier);
    JSLL_AND(rt->rngSeed, tmp, rt->rngMask);
}

static void
random_init(JSRuntime *rt)
{
    int64 tmp, tmp2;

    /* Do at most once. */
    if (rt->rngInitialized)
        return;
    rt->rngInitialized = JS_TRUE;

    /* rt->rngMultiplier = 0x5DEECE66DL */
    JSLL_ISHL(tmp, 0x5, 32);
    JSLL_UI2L(tmp2, 0xDEECE66DL);
    JSLL_OR(rt->rngMultiplier, tmp, tmp2);

    /* rt->rngAddend = 0xBL */
    JSLL_I2L(rt->rngAddend, 0xBL);

    /* rt->rngMask = (1L << 48) - 1 */
    JSLL_I2L(tmp, 1);
    JSLL_SHL(tmp2, tmp, 48);
    JSLL_SUB(rt->rngMask, tmp2, tmp);

    /* rt->rngDscale = (jsdouble)(1L << 53) */
    JSLL_SHL(tmp2, tmp, 53);
    JSLL_L2D(rt->rngDscale, tmp2);

    /* Finally, set the seed from current time. */
    random_setSeed(rt, PRMJ_Now());
}

static uint32
random_next(JSRuntime *rt, int bits)
{
    int64 nextseed, tmp;
    uint32 retval;

    JSLL_MUL(nextseed, rt->rngSeed, rt->rngMultiplier);
    JSLL_ADD(nextseed, nextseed, rt->rngAddend);
    JSLL_AND(nextseed, nextseed, rt->rngMask);
    rt->rngSeed = nextseed;
    JSLL_USHR(tmp, nextseed, 48 - bits);
    JSLL_L2I(retval, tmp);
    return retval;
}

static jsdouble
random_nextDouble(JSRuntime *rt)
{
    int64 tmp, tmp2;
    jsdouble d;

    JSLL_ISHL(tmp, random_next(rt, 26), 27);
    JSLL_UI2L(tmp2, random_next(rt, 27));
    JSLL_ADD(tmp, tmp, tmp2);
    JSLL_L2D(d, tmp);
    return d / rt->rngDscale;
}

static JSBool
math_random(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSRuntime *rt;
    jsdouble z;

    rt = cx->runtime;
    JS_LOCK_RUNTIME(rt);
    random_init(rt);
    z = random_nextDouble(rt);
    JS_UNLOCK_RUNTIME(rt);
    return js_NewNumberValue(cx, z, rval);
}

#if defined _WIN32 && !defined WINCE && _MSC_VER < 1400
/* Try to work around apparent _copysign bustage in VC6 and VC7. */
double
js_copysign(double x, double y)
{
    jsdpun xu, yu;

    xu.d = x;
    yu.d = y;
    xu.s.hi &= ~JSDOUBLE_HI32_SIGNBIT;
    xu.s.hi |= yu.s.hi & JSDOUBLE_HI32_SIGNBIT;
    return xu.d;
}
#endif

static JSBool
math_round(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_copysign(fd_floor(x + 0.5), x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_sin(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_sin(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_sqrt(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_sqrt(x);
    return js_NewNumberValue(cx, z, rval);
}

static JSBool
math_tan(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    jsdouble x, z;

    if (!js_ValueToNumber(cx, argv[0], &x))
        return JS_FALSE;
    z = fd_tan(x);
    return js_NewNumberValue(cx, z, rval);
}

#if JS_HAS_TOSOURCE
static JSBool
math_toSource(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
    *rval = ATOM_KEY(CLASS_ATOM(cx, Math));
    return JS_TRUE;
}
#endif

static JSFunctionSpec math_static_methods[] = {
#if JS_HAS_TOSOURCE
    {js_toSource_str,   math_toSource,          0, 0, 0},
#endif
    {"abs",             math_abs,               1, 0, 0},
    {"acos",            math_acos,              1, 0, 0},
    {"asin",            math_asin,              1, 0, 0},
    {"atan",            math_atan,              1, 0, 0},
    {"atan2",           math_atan2,             2, 0, 0},
    {"ceil",            math_ceil,              1, 0, 0},
    {"cos",             math_cos,               1, 0, 0},
    {"exp",             math_exp,               1, 0, 0},
    {"floor",           math_floor,             1, 0, 0},
    {"log",             math_log,               1, 0, 0},
    {"max",             math_max,               2, 0, 0},
    {"min",             math_min,               2, 0, 0},
    {"pow",             math_pow,               2, 0, 0},
    {"random",          math_random,            0, 0, 0},
    {"round",           math_round,             1, 0, 0},
    {"sin",             math_sin,               1, 0, 0},
    {"sqrt",            math_sqrt,              1, 0, 0},
    {"tan",             math_tan,               1, 0, 0},
    {0,0,0,0,0}
};

JSObject *
js_InitMathClass(JSContext *cx, JSObject *obj)
{
    JSObject *Math;

    Math = JS_DefineObject(cx, obj, js_Math_str, &js_MathClass, NULL, 0);
    if (!Math)
        return NULL;
    if (!JS_DefineFunctions(cx, Math, math_static_methods))
        return NULL;
    if (!JS_DefineConstDoubles(cx, Math, math_constants))
        return NULL;
    return Math;
}
