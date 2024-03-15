/***************************************************************************************************

  Zyan Core Library (Zycore-C)

  Original Author : Florian Bernd

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * Provides helper functions for performant number to string conversion.
 */

#ifndef ZYCORE_FORMAT_H
#define ZYCORE_FORMAT_H

#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/String.h"
#include "zydis/Zycore/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Helpers                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Get the absolute value of a 64 bit int.
 *
 * @param x The value to process.
 * @return  The absolute, unsigned value.
 *
 * This gracefully deals with the special case of `x` being `INT_MAX`.
 */
ZYAN_INLINE ZyanU64 ZyanAbsI64(ZyanI64 x)
{
    // INT_MIN special case. Can't use the value directly because GCC thinks
    // it's too big for an INT64 literal, however is perfectly happy to accept
    // this expression. This is also hit INT64_MIN is defined in `stdint.h`.
    if (x == (-0x7fffffffffffffff - 1))
    {
        return 0x8000000000000000u;
    }

    return (ZyanU64)(x < 0 ? -x : x);
}

/* ---------------------------------------------------------------------------------------------- */
/* Insertion                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Inserts formatted text in the destination string at the given `index`.
 *
 * @param   string  The destination string.
 * @param   index   The insert index.
 * @param   format  The format string.
 * @param   ...     The format arguments.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYAN_PRINTF_ATTR(3, 4)
ZYCORE_EXPORT ZyanStatus ZyanStringInsertFormat(ZyanString* string, ZyanUSize index,
    const char* format, ...);

/* ---------------------------------------------------------------------------------------------- */

/**
 * Formats the given unsigned ordinal `value` to its decimal text-representation and
 * inserts it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   index           The insert index.
 * @param   value           The value.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringInsertDecU(ZyanString* string, ZyanUSize index, ZyanU64 value,
    ZyanU8 padding_length);

/**
 * Formats the given signed ordinal `value` to its decimal text-representation and
 * inserts it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   index           The insert index.
 * @param   value           The value.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 * @param   force_sign      Set `ZYAN_TRUE`, to force printing of the `+` sign for positive numbers.
 * @param   prefix          The string to use as prefix or `ZYAN_NULL`, if not needed.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringInsertDecS(ZyanString* string, ZyanUSize index, ZyanI64 value,
    ZyanU8 padding_length, ZyanBool force_sign, const ZyanString* prefix);

/**
 * Formats the given unsigned ordinal `value` to its hexadecimal text-representation and
 * inserts it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   index           The insert index.
 * @param   value           The value.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 * @param   uppercase       Set `ZYAN_TRUE` to use uppercase letters ('A'-'F') instead of lowercase
 *                          ones ('a'-'f').
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringInsertHexU(ZyanString* string, ZyanUSize index, ZyanU64 value,
    ZyanU8 padding_length, ZyanBool uppercase);

/**
 * Formats the given signed ordinal `value` to its hexadecimal text-representation and
 * inserts it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   index           The insert index.
 * @param   value           The value.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 * @param   uppercase       Set `ZYAN_TRUE` to use uppercase letters ('A'-'F') instead of lowercase
 *                          ones ('a'-'f').
 * @param   force_sign      Set `ZYAN_TRUE`, to force printing of the `+` sign for positive numbers.
 * @param   prefix          The string to use as prefix or `ZYAN_NULL`, if not needed.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringInsertHexS(ZyanString* string, ZyanUSize index, ZyanI64 value,
    ZyanU8 padding_length, ZyanBool uppercase, ZyanBool force_sign, const ZyanString* prefix);

/* ---------------------------------------------------------------------------------------------- */
/* Appending                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

/**
 * Appends formatted text to the destination string.
 *
 * @param   string  The destination string.
 * @param   format  The format string.
 * @param   ...     The format arguments.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYAN_PRINTF_ATTR(2, 3)
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanStringAppendFormat(
    ZyanString* string, const char* format, ...);

#endif // ZYAN_NO_LIBC

/* ---------------------------------------------------------------------------------------------- */

/**
 * Formats the given unsigned ordinal `value` to its decimal text-representation and
 * appends it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   value           The value.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringAppendDecU(ZyanString* string, ZyanU64 value,
    ZyanU8 padding_length);

/**
 * Formats the given signed ordinal `value` to its decimal text-representation and
 * appends it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   value           The value.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 * @param   force_sign      Set `ZYAN_TRUE`, to force printing of the `+` sign for positive numbers.
 * @param   prefix          The string to use as prefix or `ZYAN_NULL`, if not needed.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringAppendDecS(ZyanString* string, ZyanI64 value,
    ZyanU8 padding_length, ZyanBool force_sign, const ZyanStringView* prefix);

/**
 * Formats the given unsigned ordinal `value` to its hexadecimal text-representation and
 * appends it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   value           The value.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 * @param   uppercase       Set `ZYAN_TRUE` to use uppercase letters ('A'-'F') instead of lowercase
 *                          ones ('a'-'f').
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringAppendHexU(ZyanString* string, ZyanU64 value,
    ZyanU8 padding_length, ZyanBool uppercase);

/**
 * Formats the given signed ordinal `value` to its hexadecimal text-representation and
 * appends it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   value           The value.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 * @param   uppercase       Set `ZYAN_TRUE` to use uppercase letters ('A'-'F') instead of lowercase
 *                          ones ('a'-'f').
 * @param   force_sign      Set `ZYAN_TRUE`, to force printing of the `+` sign for positive numbers.
 * @param   prefix          The string to use as prefix or `ZYAN_NULL`, if not needed.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringAppendHexS(ZyanString* string, ZyanI64 value,
    ZyanU8 padding_length, ZyanBool uppercase, ZyanBool force_sign, const ZyanStringView* prefix);

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif // ZYCORE_FORMAT_H
