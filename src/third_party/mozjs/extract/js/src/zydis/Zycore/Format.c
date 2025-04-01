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

#include "zydis/Zycore/Format.h"
#include "zydis/Zycore/LibC.h"

/* ============================================================================================== */
/* Constants                                                                                      */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Defines                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

#define ZYCORE_MAXCHARS_DEC_32 10
#define ZYCORE_MAXCHARS_DEC_64 20
#define ZYCORE_MAXCHARS_HEX_32  8
#define ZYCORE_MAXCHARS_HEX_64 16

/* ---------------------------------------------------------------------------------------------- */
/* Lookup Tables                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

static const char* const DECIMAL_LOOKUP =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

/* ---------------------------------------------------------------------------------------------- */
/* Static strings                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

static const ZyanStringView STR_ADD = ZYAN_DEFINE_STRING_VIEW("+");
static const ZyanStringView STR_SUB = ZYAN_DEFINE_STRING_VIEW("-");

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Internal macros                                                                                */
/* ============================================================================================== */

/**
 * Writes a terminating '\0' character at the end of the string data.
 */
#define ZYCORE_STRING_NULLTERMINATE(string) \
      *(char*)((ZyanU8*)(string)->vector.data + (string)->vector.size - 1) = '\0';

/* ============================================================================================== */
/* Internal functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Decimal                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

#if defined(ZYAN_X86) || defined(ZYAN_ARM) || defined(ZYAN_EMSCRIPTEN) || defined(ZYAN_WASM) || defined(ZYAN_PPC)
ZyanStatus ZyanStringAppendDecU32(ZyanString* string, ZyanU32 value, ZyanU8 padding_length)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    char buffer[ZYCORE_MAXCHARS_DEC_32];
    char *buffer_end = &buffer[ZYCORE_MAXCHARS_DEC_32];
    char *buffer_write_pointer = buffer_end;
    while (value >= 100)
    {
        const ZyanU32 value_old = value;
        buffer_write_pointer -= 2;
        value /= 100;
        ZYAN_MEMCPY(buffer_write_pointer, &DECIMAL_LOOKUP[(value_old - (value * 100)) * 2], 2);
    }
    buffer_write_pointer -= 2;
    ZYAN_MEMCPY(buffer_write_pointer, &DECIMAL_LOOKUP[value * 2], 2);

    const ZyanUSize offset_odd    = (ZyanUSize)(value < 10);
    const ZyanUSize length_number = buffer_end - buffer_write_pointer - offset_odd;
    const ZyanUSize length_total  = ZYAN_MAX(length_number, padding_length);
    const ZyanUSize length_target = string->vector.size;

    if (string->vector.size + length_total > string->vector.capacity)
    {
        ZYAN_CHECK(ZyanStringResize(string, string->vector.size + length_total - 1));
    }

    ZyanUSize offset_write = 0;
    if (padding_length > length_number)
    {
        offset_write = padding_length - length_number;
        ZYAN_MEMSET((char*)string->vector.data + length_target - 1, '0', offset_write);
    }

    ZYAN_MEMCPY((char*)string->vector.data + length_target + offset_write - 1,
        buffer_write_pointer + offset_odd, length_number);
    string->vector.size = length_target + length_total;
    ZYCORE_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}
#endif

ZyanStatus ZyanStringAppendDecU64(ZyanString* string, ZyanU64 value, ZyanU8 padding_length)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    char buffer[ZYCORE_MAXCHARS_DEC_64];
    char *buffer_end = &buffer[ZYCORE_MAXCHARS_DEC_64];
    char *buffer_write_pointer = buffer_end;
    while (value >= 100)
    {
        const ZyanU64 value_old = value;
        buffer_write_pointer -= 2;
        value /= 100;
        ZYAN_MEMCPY(buffer_write_pointer, &DECIMAL_LOOKUP[(value_old - (value * 100)) * 2], 2);
    }
    buffer_write_pointer -= 2;
    ZYAN_MEMCPY(buffer_write_pointer, &DECIMAL_LOOKUP[value * 2], 2);

    const ZyanUSize offset_odd    = (ZyanUSize)(value < 10);
    const ZyanUSize length_number = buffer_end - buffer_write_pointer - offset_odd;
    const ZyanUSize length_total  = ZYAN_MAX(length_number, padding_length);
    const ZyanUSize length_target = string->vector.size;

    if (string->vector.size + length_total > string->vector.capacity)
    {
        ZYAN_CHECK(ZyanStringResize(string, string->vector.size + length_total - 1));
    }

    ZyanUSize offset_write = 0;
    if (padding_length > length_number)
    {
        offset_write = padding_length - length_number;
        ZYAN_MEMSET((char*)string->vector.data + length_target - 1, '0', offset_write);
    }

    ZYAN_MEMCPY((char*)string->vector.data + length_target + offset_write - 1,
        buffer_write_pointer + offset_odd, length_number);
    string->vector.size = length_target + length_total;
    ZYCORE_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Hexadecimal                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

#if defined(ZYAN_X86) || defined(ZYAN_ARM) || defined(ZYAN_EMSCRIPTEN) || defined(ZYAN_WASM) || defined(ZYAN_PPC)
ZyanStatus ZyanStringAppendHexU32(ZyanString* string, ZyanU32 value, ZyanU8 padding_length,
    ZyanBool uppercase)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = string->vector.size;
    ZyanUSize remaining = string->vector.capacity - string->vector.size;

    if (remaining < (ZyanUSize)padding_length)
    {
        ZYAN_CHECK(ZyanStringResize(string, len + padding_length - 1));
        remaining = padding_length;
    }

    if (!value)
    {
        const ZyanU8 n = (padding_length ? padding_length : 1);

        if (remaining < (ZyanUSize)n)
        {
            ZYAN_CHECK(ZyanStringResize(string, string->vector.size + n - 1));
        }

        ZYAN_MEMSET((char*)string->vector.data + len - 1, '0', n);
        string->vector.size = len + n;
        ZYCORE_STRING_NULLTERMINATE(string);

        return ZYAN_STATUS_SUCCESS;
    }

    ZyanU8 n = 0;
    char* buffer = ZYAN_NULL;
    for (ZyanI8 i = ZYCORE_MAXCHARS_HEX_32 - 1; i >= 0; --i)
    {
        const ZyanU8 v = (value >> i * 4) & 0x0F;
        if (!n)
        {
            if (!v)
            {
                continue;
            }
            if (remaining <= (ZyanU8)i)
            {
                ZYAN_CHECK(ZyanStringResize(string, string->vector.size + i));
            }
            buffer = (char*)string->vector.data + len - 1;
            if (padding_length > i)
            {
                n = padding_length - i - 1;
                ZYAN_MEMSET(buffer, '0', n);
            }
        }
        ZYAN_ASSERT(buffer);
        if (uppercase)
        {
            buffer[n++] = "0123456789ABCDEF"[v];
        } else
        {
            buffer[n++] = "0123456789abcdef"[v];
        }
    }
    string->vector.size = len + n;
    ZYCORE_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}
#endif

ZyanStatus ZyanStringAppendHexU64(ZyanString* string, ZyanU64 value, ZyanU8 padding_length,
    ZyanBool uppercase)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = string->vector.size;
    ZyanUSize remaining = string->vector.capacity - string->vector.size;

    if (remaining < (ZyanUSize)padding_length)
    {
        ZYAN_CHECK(ZyanStringResize(string, len + padding_length - 1));
        remaining = padding_length;
    }

    if (!value)
    {
        const ZyanU8 n = (padding_length ? padding_length : 1);

        if (remaining < (ZyanUSize)n)
        {
            ZYAN_CHECK(ZyanStringResize(string, string->vector.size + n - 1));
        }

        ZYAN_MEMSET((char*)string->vector.data + len - 1, '0', n);
        string->vector.size = len + n;
        ZYCORE_STRING_NULLTERMINATE(string);

        return ZYAN_STATUS_SUCCESS;
    }

    ZyanU8 n = 0;
    char* buffer = ZYAN_NULL;
    for (ZyanI8 i = ((value & 0xFFFFFFFF00000000) ?
        ZYCORE_MAXCHARS_HEX_64 : ZYCORE_MAXCHARS_HEX_32) - 1; i >= 0; --i)
    {
        const ZyanU8 v = (value >> i * 4) & 0x0F;
        if (!n)
        {
            if (!v)
            {
                continue;
            }
            if (remaining <= (ZyanU8)i)
            {
                ZYAN_CHECK(ZyanStringResize(string, string->vector.size + i));
            }
            buffer = (char*)string->vector.data + len - 1;
            if (padding_length > i)
            {
                n = padding_length - i - 1;
                ZYAN_MEMSET(buffer, '0', n);
            }
        }
        ZYAN_ASSERT(buffer);
        if (uppercase)
        {
            buffer[n++] = "0123456789ABCDEF"[v];
        } else
        {
            buffer[n++] = "0123456789abcdef"[v];
        }
    }
    string->vector.size = len + n;
    ZYCORE_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Insertion                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

//ZyanStatus ZyanStringInsertFormat(ZyanString* string, ZyanUSize index, const char* format, ...)
//{
//
//}
//
///* ---------------------------------------------------------------------------------------------- */
//
//ZyanStatus ZyanStringInsertDecU(ZyanString* string, ZyanUSize index, ZyanU64 value,
//    ZyanUSize padding_length)
//{
//
//}
//
//ZyanStatus ZyanStringInsertDecS(ZyanString* string, ZyanUSize index, ZyanI64 value,
//    ZyanUSize padding_length, ZyanBool force_sign, const ZyanString* prefix)
//{
//
//}
//
//ZyanStatus ZyanStringInsertHexU(ZyanString* string, ZyanUSize index, ZyanU64 value,
//    ZyanUSize padding_length, ZyanBool uppercase)
//{
//
//}
//
//ZyanStatus ZyanStringInsertHexS(ZyanString* string, ZyanUSize index, ZyanI64 value,
//    ZyanUSize padding_length, ZyanBool uppercase, ZyanBool force_sign, const ZyanString* prefix)
//{
//
//}

/* ---------------------------------------------------------------------------------------------- */
/* Appending                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

ZyanStatus ZyanStringAppendFormat(ZyanString* string, const char* format, ...)
{
    if (!string || !format)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanVAList arglist;
    ZYAN_VA_START(arglist, format);

    const ZyanUSize len = string->vector.size;

    ZyanI32 w = ZYAN_VSNPRINTF((char*)string->vector.data + len - 1,
        string->vector.capacity - len + 1, format, arglist);
    if (w < 0)
    {
        ZYAN_VA_END(arglist);
        return ZYAN_STATUS_FAILED;
    }
    if (w <= (ZyanI32)(string->vector.capacity - len))
    {
        string->vector.size = len + w;

        ZYAN_VA_END(arglist);
        return ZYAN_STATUS_SUCCESS;
    }

    // The remaining capacity was not sufficent to fit the formatted string. Trying to resize ..
    const ZyanStatus status = ZyanStringResize(string, string->vector.size + w - 1);
    if (!ZYAN_SUCCESS(status))
    {
        ZYAN_VA_END(arglist);
        return status;
    }

    w = ZYAN_VSNPRINTF((char*)string->vector.data + len - 1,
        string->vector.capacity - string->vector.size + 1, format, arglist);
    if (w < 0)
    {
        ZYAN_VA_END(arglist);
        return ZYAN_STATUS_FAILED;
    }
    ZYAN_ASSERT(w <= (ZyanI32)(string->vector.capacity - string->vector.size));

    ZYAN_VA_END(arglist);
    return ZYAN_STATUS_SUCCESS;
}

#endif // ZYAN_NO_LIBC

/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringAppendDecU(ZyanString* string, ZyanU64 value, ZyanU8 padding_length)
{
#if defined(ZYAN_X64) || defined(ZYAN_AARCH64) || defined(ZYAN_PPC64) || defined(ZYAN_RISCV64) || defined(ZYAN_LOONGARCH)
    return ZyanStringAppendDecU64(string, value, padding_length);
#else
    // Working with 64-bit values is slow on non 64-bit systems
    if (value & 0xFFFFFFFF00000000)
    {
        return ZyanStringAppendDecU64(string, value, padding_length);
    }
    return ZyanStringAppendDecU32(string, (ZyanU32)value, padding_length);
#endif
}

ZyanStatus ZyanStringAppendDecS(ZyanString* string, ZyanI64 value, ZyanU8 padding_length,
    ZyanBool force_sign, const ZyanStringView* prefix)
{
    if (value < 0)
    {
        ZYAN_CHECK(ZyanStringAppend(string, &STR_SUB));
        if (prefix)
        {
            ZYAN_CHECK(ZyanStringAppend(string, prefix));
        }
        return ZyanStringAppendDecU(string, ZyanAbsI64(value), padding_length);
    }

    if (force_sign)
    {
        ZYAN_ASSERT(value >= 0);
        ZYAN_CHECK(ZyanStringAppend(string, &STR_ADD));
    }

    if (prefix)
    {
        ZYAN_CHECK(ZyanStringAppend(string, prefix));
    }
    return ZyanStringAppendDecU(string, value, padding_length);
}

ZyanStatus ZyanStringAppendHexU(ZyanString* string, ZyanU64 value, ZyanU8 padding_length,
    ZyanBool uppercase)
{
#if defined(ZYAN_X64) || defined(ZYAN_AARCH64) || defined(ZYAN_PPC64) || defined(ZYAN_RISCV64) || defined(ZYAN_LOONGARCH)
    return ZyanStringAppendHexU64(string, value, padding_length, uppercase);
#else
    // Working with 64-bit values is slow on non 64-bit systems
    if (value & 0xFFFFFFFF00000000)
    {
        return ZyanStringAppendHexU64(string, value, padding_length, uppercase);
    }
    return ZyanStringAppendHexU32(string, (ZyanU32)value, padding_length, uppercase);
#endif
}

ZyanStatus ZyanStringAppendHexS(ZyanString* string, ZyanI64 value, ZyanU8 padding_length,
    ZyanBool uppercase, ZyanBool force_sign, const ZyanStringView* prefix)
{
    if (value < 0)
    {
        ZYAN_CHECK(ZyanStringAppend(string, &STR_SUB));
        if (prefix)
        {
            ZYAN_CHECK(ZyanStringAppend(string, prefix));
        }
        return ZyanStringAppendHexU(string, ZyanAbsI64(value), padding_length, uppercase);
    }

    if (force_sign)
    {
        ZYAN_ASSERT(value >= 0);
        ZYAN_CHECK(ZyanStringAppend(string, &STR_ADD));
    }

    if (prefix)
    {
        ZYAN_CHECK(ZyanStringAppend(string, prefix));
    }
    return ZyanStringAppendHexU(string, value, padding_length, uppercase);
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
