/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsdtoa_h
#define jsdtoa_h

/*
 * Public interface to portable double-precision floating point to string
 * and back conversion package.
 */

#include <stddef.h>

struct DtoaState;

namespace js {

extern DtoaState*
NewDtoaState();

extern void
DestroyDtoaState(DtoaState* state);

} // namespace js

/*
 * js_strtod_harder() returns as a double-precision floating-point number the
 * value represented by the character string pointed to by s00. The string is
 * scanned up to the first unrecognized character.
 *
 * If se is not nullptr, *se receives a pointer to the character terminating
 * the scan. If no number can be formed, *se receives a pointer to the first
 * unparseable character in s00, and zero is returned.
 *
 * On overflow, this function returns infinity and does not indicate an error.
 *
 * *err is set to zero on success; it's set to JS_DTOA_ENOMEM on memory failure.
 */
#define JS_DTOA_ENOMEM 2
double
js_strtod_harder(DtoaState* state, const char* s00, char** se, int* err);

/*
 * Modes for converting floating-point numbers to strings.
 *
 * Some of the modes can round-trip; this means that if the number is converted to
 * a string using one of these mode and then converted back to a number, the result
 * will be identical to the original number (except that, due to ECMA, -0 will get converted
 * to +0).  These round-trip modes return the minimum number of significand digits that
 * permit the round trip.
 *
 * Some of the modes take an integer parameter <precision>.
 */
/* NB: Keep this in sync with number_constants[]. */
typedef enum JSDToStrMode {
    DTOSTR_STANDARD,              /* Either fixed or exponential format; round-trip */
    DTOSTR_STANDARD_EXPONENTIAL,  /* Always exponential format; round-trip */
    DTOSTR_FIXED,                 /* Round to <precision> digits after the decimal point; exponential if number is large */
    DTOSTR_EXPONENTIAL,           /* Always exponential format; <precision> significant digits */
    DTOSTR_PRECISION              /* Either fixed or exponential format; <precision> significant digits */
} JSDToStrMode;


/* Maximum number of characters (including trailing null) that a DTOSTR_STANDARD or DTOSTR_STANDARD_EXPONENTIAL
 * conversion can produce.  This maximum is reached for a number like -0.0000012345678901234567. */
#define DTOSTR_STANDARD_BUFFER_SIZE 26

/* Maximum number of characters (including trailing null) that one of the other conversions
 * can produce.  This maximum is reached for TO_FIXED, which can generate up to 21 digits before the decimal point. */
#define DTOSTR_VARIABLE_BUFFER_SIZE(precision) ((precision)+24 > DTOSTR_STANDARD_BUFFER_SIZE ? (precision)+24 : DTOSTR_STANDARD_BUFFER_SIZE)

/*
 * DO NOT USE THIS FUNCTION IF YOU CAN AVOID IT.  js::NumberToCString() is a
 * better function to use.
 *
 * Convert dval according to the given mode and return a pointer to the
 * resulting ASCII string.  If mode == DTOSTR_STANDARD and precision == 0 it's
 * equivalent to ToString() as specified by ECMA-262-5 section 9.8.1, but it
 * doesn't handle integers specially so should be avoided in that case (that's
 * why js::NumberToCString() is better).
 *
 * The result is held somewhere in buffer, but not necessarily at the
 * beginning.  The size of buffer is given in bufferSize, and must be at least
 * as large as given by the above macros.
 *
 * Return nullptr if out of memory.
 */
char*
js_dtostr(DtoaState* state, char* buffer, size_t bufferSize, JSDToStrMode mode, int precision,
          double dval);

/*
 * DO NOT USE THIS FUNCTION IF YOU CAN AVOID IT.  js::NumberToCString() is a
 * better function to use.
 *
 * Convert d to a string in the given base.  The integral part of d will be
 * printed exactly in that base, regardless of how large it is, because there
 * is no exponential notation for non-base-ten numbers.  The fractional part
 * will be rounded to as few digits as possible while still preserving the
 * round-trip property (analogous to that of printing decimal numbers).  In
 * other words, if one were to read the resulting string in via a hypothetical
 * base-number-reading routine that rounds to the nearest IEEE double (and to
 * an even significand if there are two equally near doubles), then the result
 * would equal d (except for -0.0, which converts to "0", and NaN, which is
 * not equal to itself).
 *
 * Return nullptr if out of memory.  If the result is not nullptr, it must be
 * released via js_free().
 */
char*
js_dtobasestr(DtoaState* state, int base, double d);

#endif /* jsdtoa_h */
