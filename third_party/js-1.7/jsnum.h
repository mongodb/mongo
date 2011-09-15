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

#ifndef jsnum_h___
#define jsnum_h___
/*
 * JS number (IEEE double) interface.
 *
 * JS numbers are optimistically stored in the top 31 bits of 32-bit integers,
 * but floating point literals, results that overflow 31 bits, and division and
 * modulus operands and results require a 64-bit IEEE double.  These are GC'ed
 * and pointed to by 32-bit jsvals on the stack and in object properties.
 *
 * When a JS number is treated as an object (followed by . or []), the runtime
 * wraps it with a JSObject whose valueOf method returns the unwrapped number.
 */

JS_BEGIN_EXTERN_C

/*
 * Stefan Hanske <sh990154@mail.uni-greifswald.de> reports:
 *  ARM is a little endian architecture but 64 bit double words are stored
 * differently: the 32 bit words are in little endian byte order, the two words
 * are stored in big endian`s way.
 */

#if defined(__arm) || defined(__arm32__) || defined(__arm26__) || defined(__arm__)
#define CPU_IS_ARM
#endif

typedef union jsdpun {
    struct {
#if defined(IS_LITTLE_ENDIAN) && !defined(CPU_IS_ARM)
        uint32 lo, hi;
#else
        uint32 hi, lo;
#endif
    } s;
    jsdouble d;
} jsdpun;

#if (__GNUC__ == 2 && __GNUC_MINOR__ > 95) || __GNUC__ > 2
/*
 * This version of the macros is safe for the alias optimizations that gcc
 * does, but uses gcc-specific extensions.
 */

#define JSDOUBLE_HI32(x) (__extension__ ({ jsdpun u; u.d = (x); u.s.hi; }))
#define JSDOUBLE_LO32(x) (__extension__ ({ jsdpun u; u.d = (x); u.s.lo; }))
#define JSDOUBLE_SET_HI32(x, y) \
    (__extension__ ({ jsdpun u; u.d = (x); u.s.hi = (y); (x) = u.d; }))
#define JSDOUBLE_SET_LO32(x, y) \
    (__extension__ ({ jsdpun u; u.d = (x); u.s.lo = (y); (x) = u.d; }))

#else /* not or old GNUC */

/*
 * We don't know of any non-gcc compilers that perform alias optimization,
 * so this code should work.
 */

#if defined(IS_LITTLE_ENDIAN) && !defined(CPU_IS_ARM)
#define JSDOUBLE_HI32(x)        (((uint32 *)&(x))[1])
#define JSDOUBLE_LO32(x)        (((uint32 *)&(x))[0])
#else
#define JSDOUBLE_HI32(x)        (((uint32 *)&(x))[0])
#define JSDOUBLE_LO32(x)        (((uint32 *)&(x))[1])
#endif

#define JSDOUBLE_SET_HI32(x, y) (JSDOUBLE_HI32(x)=(y))
#define JSDOUBLE_SET_LO32(x, y) (JSDOUBLE_LO32(x)=(y))

#endif /* not or old GNUC */

#define JSDOUBLE_HI32_SIGNBIT   0x80000000
#define JSDOUBLE_HI32_EXPMASK   0x7ff00000
#define JSDOUBLE_HI32_MANTMASK  0x000fffff

#define JSDOUBLE_IS_NaN(x)                                                    \
    ((JSDOUBLE_HI32(x) & JSDOUBLE_HI32_EXPMASK) == JSDOUBLE_HI32_EXPMASK &&   \
     (JSDOUBLE_LO32(x) || (JSDOUBLE_HI32(x) & JSDOUBLE_HI32_MANTMASK)))

#define JSDOUBLE_IS_INFINITE(x)                                               \
    ((JSDOUBLE_HI32(x) & ~JSDOUBLE_HI32_SIGNBIT) == JSDOUBLE_HI32_EXPMASK &&  \
     !JSDOUBLE_LO32(x))

#define JSDOUBLE_IS_FINITE(x)                                                 \
    ((JSDOUBLE_HI32(x) & JSDOUBLE_HI32_EXPMASK) != JSDOUBLE_HI32_EXPMASK)

#define JSDOUBLE_IS_NEGZERO(d)  (JSDOUBLE_HI32(d) == JSDOUBLE_HI32_SIGNBIT && \
                                 JSDOUBLE_LO32(d) == 0)

/*
 * JSDOUBLE_IS_INT first checks that d is neither NaN nor infinite, to avoid
 * raising SIGFPE on platforms such as Alpha Linux, then (only if the cast is
 * safe) leaves i as (jsint)d.  This also avoid anomalous NaN floating point
 * comparisons under MSVC.
 */
#define JSDOUBLE_IS_INT(d, i) (JSDOUBLE_IS_FINITE(d)                          \
                               && !JSDOUBLE_IS_NEGZERO(d)                     \
                               && ((d) == (i = (jsint)(d))))

#if defined(XP_WIN)
#define JSDOUBLE_COMPARE(LVAL, OP, RVAL, IFNAN)                               \
    ((JSDOUBLE_IS_NaN(LVAL) || JSDOUBLE_IS_NaN(RVAL))                         \
     ? (IFNAN)                                                                \
     : (LVAL) OP (RVAL))
#else
#define JSDOUBLE_COMPARE(LVAL, OP, RVAL, IFNAN) ((LVAL) OP (RVAL))
#endif

/* Initialize number constants and runtime state for the first context. */
extern JSBool
js_InitRuntimeNumberState(JSContext *cx);

extern void
js_FinishRuntimeNumberState(JSContext *cx);

/* Initialize the Number class, returning its prototype object. */
extern JSClass js_NumberClass;

extern JSObject *
js_InitNumberClass(JSContext *cx, JSObject *obj);

/*
 * String constants for global function names, used in jsapi.c and jsnum.c.
 */
extern const char js_Infinity_str[];
extern const char js_NaN_str[];
extern const char js_isNaN_str[];
extern const char js_isFinite_str[];
extern const char js_parseFloat_str[];
extern const char js_parseInt_str[];

/* GC-allocate a new JS number. */
extern jsdouble *
js_NewDouble(JSContext *cx, jsdouble d, uintN gcflag);

extern void
js_FinalizeDouble(JSContext *cx, jsdouble *dp);

extern JSBool
js_NewDoubleValue(JSContext *cx, jsdouble d, jsval *rval);

extern JSBool
js_NewNumberValue(JSContext *cx, jsdouble d, jsval *rval);

/* Construct a Number instance that wraps around d. */
extern JSObject *
js_NumberToObject(JSContext *cx, jsdouble d);

/* Convert a number to a GC'ed string. */
extern JSString *
js_NumberToString(JSContext *cx, jsdouble d);

/*
 * Convert a value to a number, returning false after reporting any error,
 * otherwise returning true with *dp set.
 */
extern JSBool
js_ValueToNumber(JSContext *cx, jsval v, jsdouble *dp);

/*
 * Convert a value or a double to an int32, according to the ECMA rules
 * for ToInt32.
 */
extern JSBool
js_ValueToECMAInt32(JSContext *cx, jsval v, int32 *ip);

extern JSBool
js_DoubleToECMAInt32(JSContext *cx, jsdouble d, int32 *ip);

/*
 * Convert a value or a double to a uint32, according to the ECMA rules
 * for ToUint32.
 */
extern JSBool
js_ValueToECMAUint32(JSContext *cx, jsval v, uint32 *ip);

extern JSBool
js_DoubleToECMAUint32(JSContext *cx, jsdouble d, uint32 *ip);

/*
 * Convert a value to a number, then to an int32 if it fits by rounding to
 * nearest; but failing with an error report if the double is out of range
 * or unordered.
 */
extern JSBool
js_ValueToInt32(JSContext *cx, jsval v, int32 *ip);

/*
 * Convert a value to a number, then to a uint16 according to the ECMA rules
 * for ToUint16.
 */
extern JSBool
js_ValueToUint16(JSContext *cx, jsval v, uint16 *ip);

/*
 * Convert a jsdouble to an integral number, stored in a jsdouble.
 * If d is NaN, return 0.  If d is an infinity, return it without conversion.
 */
extern jsdouble
js_DoubleToInteger(jsdouble d);

/*
 * Similar to strtod except that it replaces overflows with infinities of the
 * correct sign, and underflows with zeros of the correct sign.  Guaranteed to
 * return the closest double number to the given input in dp.
 *
 * Also allows inputs of the form [+|-]Infinity, which produce an infinity of
 * the appropriate sign.  The case of the "Infinity" string must match exactly.
 * If the string does not contain a number, set *ep to s and return 0.0 in dp.
 * Return false if out of memory.
 */
extern JSBool
js_strtod(JSContext *cx, const jschar *s, const jschar **ep, jsdouble *dp);

/*
 * Similar to strtol except that it handles integers of arbitrary size.
 * Guaranteed to return the closest double number to the given input when radix
 * is 10 or a power of 2.  Callers may see round-off errors for very large
 * numbers of a different radix than 10 or a power of 2.
 *
 * If the string does not contain a number, set *ep to s and return 0.0 in dp.
 * Return false if out of memory.
 */
extern JSBool
js_strtointeger(JSContext *cx, const jschar *s, const jschar **ep, jsint radix, jsdouble *dp);

JS_END_EXTERN_C

#endif /* jsnum_h___ */
