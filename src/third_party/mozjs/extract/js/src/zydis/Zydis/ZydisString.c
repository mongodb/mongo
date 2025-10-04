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

#include "zydis/Zydis/Internal/String.h"

/* ============================================================================================== */
/* Constants                                                                                      */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Defines                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

#define ZYDIS_MAXCHARS_DEC_32 10
#define ZYDIS_MAXCHARS_DEC_64 20
#define ZYDIS_MAXCHARS_HEX_32  8
#define ZYDIS_MAXCHARS_HEX_64 16

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

/* ============================================================================================== */
/* Internal Functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Decimal                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

#if defined(ZYAN_X86) || defined(ZYAN_ARM) || defined(ZYAN_EMSCRIPTEN) || defined(ZYAN_WASM) || defined(ZYAN_PPC)
static ZyanStatus ZydisStringAppendDecU32(ZyanString* string, ZyanU32 value, ZyanU8 padding_length)
{
    ZYAN_ASSERT(string);
    ZYAN_ASSERT(!string->vector.allocator);

    char buffer[ZYDIS_MAXCHARS_DEC_32];
    char *buffer_end = &buffer[ZYDIS_MAXCHARS_DEC_32];
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
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
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
    ZYDIS_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}
#endif

static ZyanStatus ZydisStringAppendDecU64(ZyanString* string, ZyanU64 value, ZyanU8 padding_length)
{
    ZYAN_ASSERT(string);
    ZYAN_ASSERT(!string->vector.allocator);

    char buffer[ZYDIS_MAXCHARS_DEC_64];
    char *buffer_end = &buffer[ZYDIS_MAXCHARS_DEC_64];
    char *buffer_write_pointer = buffer_end;
    while (value >= 100)
    {
        const ZyanU64 value_old = value;
        buffer_write_pointer -= 2;
        ZYAN_DIV64(value, 100);
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
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
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
    ZYDIS_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Hexadecimal                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

#if defined(ZYAN_X86) || defined(ZYAN_ARM) || defined(ZYAN_EMSCRIPTEN) || defined(ZYAN_WASM) || defined(ZYAN_PPC)
static ZyanStatus ZydisStringAppendHexU32(ZyanString* string, ZyanU32 value, ZyanU8 padding_length,
    ZyanBool force_leading_number, ZyanBool uppercase)
{
    ZYAN_ASSERT(string);
    ZYAN_ASSERT(!string->vector.allocator);

    const ZyanUSize len = string->vector.size;
    const ZyanUSize remaining = string->vector.capacity - string->vector.size;

    if (remaining < (ZyanUSize)padding_length)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    if (!value)
    {
        const ZyanU8 n = (padding_length ? padding_length : 1);

        if (remaining < (ZyanUSize)n)
        {
            return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
        }

        ZYAN_MEMSET((char*)string->vector.data + len - 1, '0', n);
        string->vector.size = len + n;
        ZYDIS_STRING_NULLTERMINATE(string);

        return ZYAN_STATUS_SUCCESS;
    }

    ZyanU8 n = 0;
    char* buffer = ZYAN_NULL;
    for (ZyanI8 i = ZYDIS_MAXCHARS_HEX_32 - 1; i >= 0; --i)
    {
        const ZyanU8 v = (value >> i * 4) & 0x0F;
        if (!n)
        {
            if (!v)
            {
                continue;
            }
            const ZyanU8 zero = force_leading_number && (v > 9) && (padding_length <= i) ? 1 : 0;
            if (remaining <= (ZyanUSize)i + zero)
            {
                return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
            }
            buffer = (char*)string->vector.data + len - 1;
            if (zero)
            {
                buffer[n++] = '0';
            }
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
    ZYDIS_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}
#endif

static ZyanStatus ZydisStringAppendHexU64(ZyanString* string, ZyanU64 value, ZyanU8 padding_length,
    ZyanBool force_leading_number, ZyanBool uppercase)
{
    ZYAN_ASSERT(string);
    ZYAN_ASSERT(!string->vector.allocator);

    const ZyanUSize len = string->vector.size;
    const ZyanUSize remaining = string->vector.capacity - string->vector.size;

    if (remaining < (ZyanUSize)padding_length)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    if (!value)
    {
        const ZyanU8 n = (padding_length ? padding_length : 1);

        if (remaining < (ZyanUSize)n)
        {
            return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
        }

        ZYAN_MEMSET((char*)string->vector.data + len - 1, '0', n);
        string->vector.size = len + n;
        ZYDIS_STRING_NULLTERMINATE(string);

        return ZYAN_STATUS_SUCCESS;
    }

    ZyanU8 n = 0;
    char* buffer = ZYAN_NULL;
    for (ZyanI8 i = ((value & 0xFFFFFFFF00000000) ?
        ZYDIS_MAXCHARS_HEX_64 : ZYDIS_MAXCHARS_HEX_32) - 1; i >= 0; --i)
    {
        const ZyanU8 v = (value >> i * 4) & 0x0F;
        if (!n)
        {
            if (!v)
            {
                continue;
            }
            const ZyanU8 zero = force_leading_number && (v > 9) && (padding_length <= i) ? 1 : 0;
            if (remaining <= (ZyanUSize)i + zero)
            {
                return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
            }
            buffer = (char*)string->vector.data + len - 1;
            if (zero)
            {
                buffer[n++] = '0';
            }
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
    ZYDIS_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Public Functions                                                                               */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Formatting                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZydisStringAppendDecU(ZyanString* string, ZyanU64 value, ZyanU8 padding_length,
    const ZyanStringView* prefix, const ZyanStringView* suffix)
{
    if (prefix)
    {
        ZYAN_CHECK(ZydisStringAppend(string, prefix));
    }

#if defined(ZYAN_X64) || defined(ZYAN_AARCH64) || defined(ZYAN_PPC64) || defined(ZYAN_RISCV64) || defined(ZYAN_LOONGARCH)
    ZYAN_CHECK(ZydisStringAppendDecU64(string, value, padding_length));
#else
    if (value & 0xFFFFFFFF00000000)
    {
        ZYAN_CHECK(ZydisStringAppendDecU64(string, value, padding_length));
    }
    ZYAN_CHECK(ZydisStringAppendDecU32(string, (ZyanU32)value, padding_length));
#endif

    if (suffix)
    {
        return ZydisStringAppend(string, suffix);
    }
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisStringAppendHexU(ZyanString* string, ZyanU64 value, ZyanU8 padding_length,
    ZyanBool force_leading_number, ZyanBool uppercase, const ZyanStringView* prefix,
    const ZyanStringView* suffix)
{
    if (prefix)
    {
        ZYAN_CHECK(ZydisStringAppend(string, prefix));
    }

#if defined(ZYAN_X64) || defined(ZYAN_AARCH64) || defined(ZYAN_PPC64) || defined(ZYAN_RISCV64) || defined(ZYAN_LOONGARCH)
    ZYAN_CHECK(ZydisStringAppendHexU64(string, value, padding_length, force_leading_number,
        uppercase));
#else
    if (value & 0xFFFFFFFF00000000)
    {
        ZYAN_CHECK(ZydisStringAppendHexU64(string, value, padding_length, force_leading_number,
            uppercase));
    }
    else
    {
        ZYAN_CHECK(ZydisStringAppendHexU32(string, (ZyanU32)value, padding_length,
            force_leading_number, uppercase));
    }
#endif

    if (suffix)
    {
        return ZydisStringAppend(string, suffix);
    }
    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
