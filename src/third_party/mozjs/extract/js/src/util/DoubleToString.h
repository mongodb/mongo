/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_DoubleToString_h
#define util_DoubleToString_h

/*
 * Public interface to portable double-precision floating point to string
 * and back conversion package.
 */

#include <stddef.h>

struct DtoaState;

namespace js {

extern DtoaState* NewDtoaState();

extern void DestroyDtoaState(DtoaState* state);

}  // namespace js

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
 */
double js_strtod_harder(DtoaState* state, const char* s00, char** se);

/* Maximum number of characters (including trailing null) that a DTOSTR_STANDARD
 * or DTOSTR_STANDARD_EXPONENTIAL conversion can produce.  This maximum is
 * reached for a number like -0.0000012345678901234567. */
#define DTOSTR_STANDARD_BUFFER_SIZE 26

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
char* js_dtobasestr(DtoaState* state, int base, double d);

#endif /* util_DoubleToString_h */
