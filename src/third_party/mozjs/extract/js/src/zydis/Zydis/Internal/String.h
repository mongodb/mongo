/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Florian Bernd, Joel Hoener

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
 * Provides some internal, more performant, but unsafe helper functions for the `ZyanString`
 * data-type.
 *
 * Most of these functions are very similar to the ones in `Zycore/String.h`, but inlined and
 * without optional overhead like parameter-validation checks, etc ...
 *
 * The `ZyanString` data-type is able to dynamically allocate memory on the heap, but as `Zydis` is
 * designed to be a non-'malloc'ing library, all functions in this file assume that the instances
 * they are operating on are created with a user-defined static-buffer.
 */

#ifndef ZYDIS_INTERNAL_STRING_H
#define ZYDIS_INTERNAL_STRING_H

#include "zydis/Zycore/LibC.h"
#include "zydis/Zycore/String.h"
#include "zydis/Zycore/Types.h"
#include "zydis/Zycore/Format.h"
#include "zydis/Zydis/ShortString.h"
#include "zydis/Zycore/Defines.h"
#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Vector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Letter Case                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisLetterCase` enum.
 */
typedef enum ZydisLetterCase_
{
    /**
     * Uses the given text "as is".
     */
    ZYDIS_LETTER_CASE_DEFAULT,
    /**
     * Converts the given text to lowercase letters.
     */
    ZYDIS_LETTER_CASE_LOWER,
    /**
     * Converts the given text to uppercase letters.
     */
    ZYDIS_LETTER_CASE_UPPER,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_LETTER_CASE_MAX_VALUE = ZYDIS_LETTER_CASE_UPPER,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_LETTER_CASE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_LETTER_CASE_MAX_VALUE)
} ZydisLetterCase;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Internal macros                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Checks for a terminating '\0' character at the end of the string data.
 */
#define ZYDIS_STRING_ASSERT_NULLTERMINATION(string) \
      ZYAN_ASSERT(*(char*)((ZyanU8*)(string)->vector.data + (string)->vector.size - 1) == '\0');

/**
 * Writes a terminating '\0' character at the end of the string data.
 */
#define ZYDIS_STRING_NULLTERMINATE(string) \
      *(char*)((ZyanU8*)(string)->vector.data + (string)->vector.size - 1) = '\0';

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Internal Functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Appending                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Appends the content of the source string to the end of the destination string.
 *
 * @param   destination The destination string.
 * @param   source      The source string.
 *
 * @return  A zyan status code.
 */
ZYAN_INLINE ZyanStatus ZydisStringAppend(ZyanString* destination, const ZyanStringView* source)
{
    ZYAN_ASSERT(destination && source);
    ZYAN_ASSERT(!destination->vector.allocator);
    ZYAN_ASSERT(destination->vector.size && source->string.vector.size);

    if (destination->vector.size + source->string.vector.size - 1 > destination->vector.capacity)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    ZYAN_MEMCPY((char*)destination->vector.data + destination->vector.size - 1,
        source->string.vector.data, source->string.vector.size - 1);

    destination->vector.size += source->string.vector.size - 1;
    ZYDIS_STRING_NULLTERMINATE(destination);

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Appends the content of the source string to the end of the destination
 * string, converting the characters to the specified letter-case.
 *
 * @param   destination The destination string.
 * @param   source      The source string.
 * @param   letter_case The desired letter-case.
 *
 * @return  A zyan status code.
 */
ZYAN_INLINE ZyanStatus ZydisStringAppendCase(ZyanString* destination, const ZyanStringView* source,
    ZydisLetterCase letter_case)
{
    ZYAN_ASSERT(destination && source);
    ZYAN_ASSERT(!destination->vector.allocator);
    ZYAN_ASSERT(destination->vector.size && source->string.vector.size);

    if (destination->vector.size + source->string.vector.size - 1 > destination->vector.capacity)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    ZYAN_MEMCPY((char*)destination->vector.data + destination->vector.size - 1,
        source->string.vector.data, source->string.vector.size - 1);

    switch (letter_case)
    {
    case ZYDIS_LETTER_CASE_DEFAULT:
        break;
    case ZYDIS_LETTER_CASE_LOWER:
    {
        const ZyanUSize index = destination->vector.size - 1;
        const ZyanUSize count = source->string.vector.size - 1;
        char* s = (char*)destination->vector.data + index;
        for (ZyanUSize i = index; i < index + count; ++i)
        {
            const char c = *s;
            if ((c >= 'A') && (c <= 'Z'))
            {
                *s = c | 32;
            }
            ++s;
        }
        break;
    }
    case ZYDIS_LETTER_CASE_UPPER:
    {
        const ZyanUSize index = destination->vector.size - 1;
        const ZyanUSize count = source->string.vector.size - 1;
        char* s = (char*)destination->vector.data + index;
        for (ZyanUSize i = index; i < index + count; ++i)
        {
            const char c = *s;
            if ((c >= 'a') && (c <= 'z'))
            {
                *s = c & ~32;
            }
            ++s;
        }
        break;
    }
    default:
        ZYAN_UNREACHABLE;
    }

    destination->vector.size += source->string.vector.size - 1;
    ZYDIS_STRING_NULLTERMINATE(destination);

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Appends the content of the source short-string to the end of the destination string.
 *
 * @param   destination The destination string.
 * @param   source      The source string.
 *
 * @return  A zyan status code.
 */
ZYAN_INLINE ZyanStatus ZydisStringAppendShort(ZyanString* destination,
    const ZydisShortString* source)
{
    ZYAN_ASSERT(destination && source);
    ZYAN_ASSERT(!destination->vector.allocator);
    ZYAN_ASSERT(destination->vector.size && source->size);

    if (destination->vector.size + source->size > destination->vector.capacity)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    ZYAN_MEMCPY((char*)destination->vector.data + destination->vector.size - 1, source->data,
        (ZyanUSize)source->size + 1);

    destination->vector.size += source->size;
    ZYDIS_STRING_ASSERT_NULLTERMINATION(destination);

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Appends the content of the source short-string to the end of the destination string,
 * converting the characters to the specified letter-case.
 *
 * @param   destination The destination string.
 * @param   source      The source string.
 * @param   letter_case The desired letter-case.
 *
 * @return  A zyan status code.
 */
ZYAN_INLINE ZyanStatus ZydisStringAppendShortCase(ZyanString* destination,
    const ZydisShortString* source, ZydisLetterCase letter_case)
{
    ZYAN_ASSERT(destination && source);
    ZYAN_ASSERT(!destination->vector.allocator);
    ZYAN_ASSERT(destination->vector.size && source->size);

    if (destination->vector.size + source->size > destination->vector.capacity)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    ZYAN_MEMCPY((char*)destination->vector.data + destination->vector.size - 1, source->data,
        (ZyanUSize)source->size + 1);

    switch (letter_case)
    {
    case ZYDIS_LETTER_CASE_DEFAULT:
        break;
    case ZYDIS_LETTER_CASE_LOWER:
    {
        const ZyanUSize index = destination->vector.size - 1;
        const ZyanUSize count = source->size;
        char* s = (char*)destination->vector.data + index;
        for (ZyanUSize i = index; i < index + count; ++i)
        {
            const char c = *s;
            if ((c >= 'A') && (c <= 'Z'))
            {
                *s = c | 32;
            }
            ++s;
        }
        break;
    }
    case ZYDIS_LETTER_CASE_UPPER:
    {
        const ZyanUSize index = destination->vector.size - 1;
        const ZyanUSize count = source->size;
        char* s = (char*)destination->vector.data + index;
        for (ZyanUSize i = index; i < index + count; ++i)
        {
            const char c = *s;
            if ((c >= 'a') && (c <= 'z'))
            {
                *s = c & ~32;
            }
            ++s;
        }
        break;
    }
    default:
        ZYAN_UNREACHABLE;
    }

    destination->vector.size += source->size;
    ZYDIS_STRING_ASSERT_NULLTERMINATION(destination);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Formatting                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Formats the given unsigned ordinal `value` to its decimal text-representation and
 * appends it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   value           The value to append.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 * @param   prefix          The string to use as prefix or `ZYAN_NULL`, if not needed.
 * @param   suffix          The string to use as suffix or `ZYAN_NULL`, if not needed.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZyanStatus ZydisStringAppendDecU(ZyanString* string, ZyanU64 value, ZyanU8 padding_length,
    const ZyanStringView* prefix, const ZyanStringView* suffix);

/**
 * Formats the given signed ordinal `value` to its decimal text-representation and
 * appends it to the `string`.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   value           The value to append.
 * @param   padding_length  Padds the converted value with leading zeros, if the number of chars is
 *                          less than the `padding_length`.
 * @param   force_sign      Enable this option to print the `+` sign for positive numbers.
 * @param   prefix          The string to use as prefix or `ZYAN_NULL`, if not needed.
 * @param   suffix          The string to use as suffix or `ZYAN_NULL`, if not needed.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYAN_INLINE ZyanStatus ZydisStringAppendDecS(ZyanString* string, ZyanI64 value,
    ZyanU8 padding_length, ZyanBool force_sign, const ZyanStringView* prefix,
    const ZyanStringView* suffix)
{
    static const ZydisShortString str_add = ZYDIS_MAKE_SHORTSTRING("+");
    static const ZydisShortString str_sub = ZYDIS_MAKE_SHORTSTRING("-");

    if (value < 0)
    {
        ZYAN_CHECK(ZydisStringAppendShort(string, &str_sub));
        if (prefix)
        {
            ZYAN_CHECK(ZydisStringAppend(string, prefix));
        }
        return ZydisStringAppendDecU(string, ZyanAbsI64(value), padding_length,
            (const ZyanStringView*)ZYAN_NULL, suffix);
    }

    if (force_sign)
    {
        ZYAN_ASSERT(value >= 0);
        ZYAN_CHECK(ZydisStringAppendShort(string, &str_add));
    }
    return ZydisStringAppendDecU(string, value, padding_length, prefix, suffix);
}

/**
 * Formats the given unsigned ordinal `value` to its hexadecimal text-representation and
 * appends it to the `string`.
 *
 * @param   string                  A pointer to the `ZyanString` instance.
 * @param   value                   The value to append.
 * @param   padding_length          Pads the converted value with leading zeros if the number of
 *                                  chars is less than the `padding_length`.
 * @param   force_leading_number    Enable this option to prepend a leading `0` if the first
 *                                  character is non-numeric.
 * @param   uppercase               Enable this option to use uppercase letters ('A'-'F') instead
 *                                  of lowercase ones ('a'-'f').
 * @param   prefix                  The string to use as prefix or `ZYAN_NULL`, if not needed.
 * @param   suffix                  The string to use as suffix or `ZYAN_NULL`, if not needed.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZyanStatus ZydisStringAppendHexU(ZyanString* string, ZyanU64 value, ZyanU8 padding_length,
    ZyanBool force_leading_number, ZyanBool uppercase, const ZyanStringView* prefix,
    const ZyanStringView* suffix);

/**
 * Formats the given signed ordinal `value` to its hexadecimal text-representation and
 * appends it to the `string`.
 *
 * @param   string                  A pointer to the `ZyanString` instance.
 * @param   value                   The value to append.
 * @param   padding_length          Padds the converted value with leading zeros, if the number of
 *                                  chars is less than the `padding_length` (the sign char does not
 *                                  count).
 * @param   force_leading_number    Enable this option to prepend a leading `0`, if the first
 *                                  character is non-numeric.
 * @param   uppercase               Enable this option to use uppercase letters ('A'-'F') instead
 *                                  of lowercase ones ('a'-'f').
 * @param   force_sign              Enable this option to print the `+` sign for positive numbers.
 * @param   prefix                  The string to use as prefix or `ZYAN_NULL`, if not needed.
 * @param   suffix                  The string to use as suffix or `ZYAN_NULL`, if not needed.
 *
 * @return  A zyan status code.
 *
 * This function will fail if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYAN_INLINE ZyanStatus ZydisStringAppendHexS(ZyanString* string, ZyanI64 value,
    ZyanU8 padding_length, ZyanBool force_leading_number, ZyanBool uppercase, ZyanBool force_sign,
    const ZyanStringView* prefix, const ZyanStringView* suffix)
{
    static const ZydisShortString str_add = ZYDIS_MAKE_SHORTSTRING("+");
    static const ZydisShortString str_sub = ZYDIS_MAKE_SHORTSTRING("-");

    if (value < 0)
    {
        ZYAN_CHECK(ZydisStringAppendShort(string, &str_sub));
        if (prefix)
        {
            ZYAN_CHECK(ZydisStringAppend(string, prefix));
        }
        return ZydisStringAppendHexU(string, ZyanAbsI64(value), padding_length,
            force_leading_number, uppercase, (const ZyanStringView*)ZYAN_NULL, suffix);
    }

    if (force_sign)
    {
        ZYAN_ASSERT(value >= 0);
        ZYAN_CHECK(ZydisStringAppendShort(string, &str_add));
    }
    return ZydisStringAppendHexU(string, value, padding_length, force_leading_number, uppercase,
        prefix, suffix);
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif // ZYDIS_INTERNAL_STRING_H
