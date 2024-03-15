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

#include "zydis/Zycore/String.h"
#include "zydis/Zycore/LibC.h"

/* ============================================================================================== */
/* Internal macros                                                                                */
/* ============================================================================================== */

/**
 * Writes a terminating '\0' character at the end of the string data.
 */
#define ZYCORE_STRING_NULLTERMINATE(string) \
      *(char*)((ZyanU8*)(string)->vector.data + (string)->vector.size - 1) = '\0';

/**
 * Checks for a terminating '\0' character at the end of the string data.
 */
#define ZYCORE_STRING_ASSERT_NULLTERMINATION(string) \
      ZYAN_ASSERT(*(char*)((ZyanU8*)(string)->vector.data + (string)->vector.size - 1) == '\0');

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constructor and destructor                                                                     */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

ZyanStatus ZyanStringInit(ZyanString* string, ZyanUSize capacity)
{
    return ZyanStringInitEx(string, capacity, ZyanAllocatorDefault(),
        ZYAN_STRING_DEFAULT_GROWTH_FACTOR, ZYAN_STRING_DEFAULT_SHRINK_THRESHOLD);
}

#endif // ZYAN_NO_LIBC

ZyanStatus ZyanStringInitEx(ZyanString* string, ZyanUSize capacity, ZyanAllocator* allocator,
    ZyanU8 growth_factor, ZyanU8 shrink_threshold)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    string->flags = 0;
    capacity = ZYAN_MAX(ZYAN_STRING_MIN_CAPACITY, capacity) + 1;
    ZYAN_CHECK(ZyanVectorInitEx(&string->vector, sizeof(char), capacity, ZYAN_NULL, allocator,
        growth_factor, shrink_threshold));
    ZYAN_ASSERT(string->vector.capacity >= capacity);
    // Some of the string code relies on `sizeof(char) == 1`
    ZYAN_ASSERT(string->vector.element_size == 1);

    *(char*)string->vector.data = '\0';
    ++string->vector.size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringInitCustomBuffer(ZyanString* string, char* buffer, ZyanUSize capacity)
{
    if (!string || !capacity)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    string->flags = ZYAN_STRING_HAS_FIXED_CAPACITY;
    ZYAN_CHECK(ZyanVectorInitCustomBuffer(&string->vector, sizeof(char), (void*)buffer, capacity,
        ZYAN_NULL));
    ZYAN_ASSERT(string->vector.capacity == capacity);
    // Some of the string code relies on `sizeof(char) == 1`
    ZYAN_ASSERT(string->vector.element_size == 1);

    *(char*)string->vector.data = '\0';
    ++string->vector.size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringDestroy(ZyanString* string)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (string->flags & ZYAN_STRING_HAS_FIXED_CAPACITY)
    {
        return ZYAN_STATUS_SUCCESS;
    }

    return ZyanVectorDestroy(&string->vector);
}

/* ---------------------------------------------------------------------------------------------- */
/* Duplication                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

ZyanStatus ZyanStringDuplicate(ZyanString* destination, const ZyanStringView* source,
    ZyanUSize capacity)
{
    return ZyanStringDuplicateEx(destination, source, capacity, ZyanAllocatorDefault(),
        ZYAN_STRING_DEFAULT_GROWTH_FACTOR, ZYAN_STRING_DEFAULT_SHRINK_THRESHOLD);
}

#endif // ZYAN_NO_LIBC

ZyanStatus ZyanStringDuplicateEx(ZyanString* destination, const ZyanStringView* source,
    ZyanUSize capacity, ZyanAllocator* allocator, ZyanU8 growth_factor, ZyanU8 shrink_threshold)
{
    if (!source || !source->string.vector.size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = source->string.vector.size;
    capacity = ZYAN_MAX(capacity, len - 1);
    ZYAN_CHECK(ZyanStringInitEx(destination, capacity, allocator, growth_factor, shrink_threshold));
    ZYAN_ASSERT(destination->vector.capacity >= len);

    ZYAN_MEMCPY(destination->vector.data, source->string.vector.data,
        source->string.vector.size - 1);
    destination->vector.size = len;
    ZYCORE_STRING_NULLTERMINATE(destination);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringDuplicateCustomBuffer(ZyanString* destination, const ZyanStringView* source,
    char* buffer, ZyanUSize capacity)
{
    if (!source || !source->string.vector.size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = source->string.vector.size;
    if (capacity < len)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    ZYAN_CHECK(ZyanStringInitCustomBuffer(destination, buffer, capacity));
    ZYAN_ASSERT(destination->vector.capacity >= len);

    ZYAN_MEMCPY(destination->vector.data, source->string.vector.data,
        source->string.vector.size - 1);
    destination->vector.size = len;
    ZYCORE_STRING_NULLTERMINATE(destination);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Concatenation                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

ZyanStatus ZyanStringConcat(ZyanString* destination, const ZyanStringView* s1,
    const ZyanStringView* s2, ZyanUSize capacity)
{
    return ZyanStringConcatEx(destination, s1, s2, capacity, ZyanAllocatorDefault(),
        ZYAN_STRING_DEFAULT_GROWTH_FACTOR, ZYAN_STRING_DEFAULT_SHRINK_THRESHOLD);
}

#endif // ZYAN_NO_LIBC

ZyanStatus ZyanStringConcatEx(ZyanString* destination, const ZyanStringView* s1,
    const ZyanStringView* s2, ZyanUSize capacity, ZyanAllocator* allocator, ZyanU8 growth_factor,
    ZyanU8 shrink_threshold)
{
    if (!s1 || !s2 || !s1->string.vector.size || !s2->string.vector.size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = s1->string.vector.size + s2->string.vector.size - 1;
    capacity = ZYAN_MAX(capacity, len - 1);
    ZYAN_CHECK(ZyanStringInitEx(destination, capacity, allocator, growth_factor, shrink_threshold));
    ZYAN_ASSERT(destination->vector.capacity >= len);

    ZYAN_MEMCPY(destination->vector.data, s1->string.vector.data, s1->string.vector.size - 1);
    ZYAN_MEMCPY((char*)destination->vector.data + s1->string.vector.size - 1,
        s2->string.vector.data, s2->string.vector.size - 1);
    destination->vector.size = len;
    ZYCORE_STRING_NULLTERMINATE(destination);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringConcatCustomBuffer(ZyanString* destination, const ZyanStringView* s1,
    const ZyanStringView* s2, char* buffer, ZyanUSize capacity)
{
    if (!s1 || !s2 || !s1->string.vector.size || !s2->string.vector.size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = s1->string.vector.size + s2->string.vector.size - 1;
    if (capacity < len)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    ZYAN_CHECK(ZyanStringInitCustomBuffer(destination, buffer, capacity));
    ZYAN_ASSERT(destination->vector.capacity >= len);

    ZYAN_MEMCPY(destination->vector.data, s1->string.vector.data, s1->string.vector.size - 1);
    ZYAN_MEMCPY((char*)destination->vector.data + s1->string.vector.size - 1,
        s2->string.vector.data, s2->string.vector.size - 1);
    destination->vector.size = len;
    ZYCORE_STRING_NULLTERMINATE(destination);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Views                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringViewInsideView(ZyanStringView* view, const ZyanStringView* source)
{
    if (!view || !source)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    view->string.vector.data = source->string.vector.data;
    view->string.vector.size = source->string.vector.size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringViewInsideViewEx(ZyanStringView* view, const ZyanStringView* source,
    ZyanUSize index, ZyanUSize count)
{
    if (!view || !source)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (index + count >= source->string.vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    view->string.vector.data = (void*)((char*)source->string.vector.data + index);
    view->string.vector.size = count;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringViewInsideBuffer(ZyanStringView* view, const char* string)
{
    if (!view || !string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    view->string.vector.data = (void*)string;
    view->string.vector.size = ZYAN_STRLEN(string) + 1;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringViewInsideBufferEx(ZyanStringView* view, const char* buffer, ZyanUSize length)
{
    if (!view || !buffer || !length)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    view->string.vector.data = (void*)buffer;
    view->string.vector.size = length + 1;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringViewGetSize(const ZyanStringView* view, ZyanUSize* size)
{
    if (!view || !size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_ASSERT(view->string.vector.size >= 1);
    *size = view->string.vector.size - 1;

    return ZYAN_STATUS_SUCCESS;
}

ZYCORE_EXPORT ZyanStatus ZyanStringViewGetData(const ZyanStringView* view, const char** buffer)
{
    if (!view || !buffer)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *buffer = view->string.vector.data;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Character access                                                                               */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringGetChar(const ZyanStringView* string, ZyanUSize index, char* value)
{
    if (!string || !value)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow direct access to the terminating '\0' character
    if (index + 1 >= string->string.vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    const char* chr;
    ZYAN_CHECK(ZyanVectorGetPointer(&string->string.vector, index, (const void**)&chr));
    *value = *chr;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringGetCharMutable(ZyanString* string, ZyanUSize index, char** value)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow direct access to the terminating '\0' character
    if (index + 1 >= string->vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    return ZyanVectorGetPointerMutable(&string->vector, index, (void**)value);
}

ZyanStatus ZyanStringSetChar(ZyanString* string, ZyanUSize index, char value)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow direct access to the terminating '\0' character
    if (index + 1 >= string->vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    return ZyanVectorSet(&string->vector, index, (void*)&value);
}

/* ---------------------------------------------------------------------------------------------- */
/* Insertion                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringInsert(ZyanString* destination, ZyanUSize index, const ZyanStringView* source)
{
    if (!destination || !source || !source->string.vector.size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (index == destination->vector.size)
    {
        return ZyanStringAppend(destination, source);
    }

    // Don't allow insertion after the terminating '\0' character
    if (index >= destination->vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_CHECK(ZyanVectorInsertRange(&destination->vector, index, source->string.vector.data,
        source->string.vector.size - 1));
    ZYCORE_STRING_ASSERT_NULLTERMINATION(destination);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringInsertEx(ZyanString* destination, ZyanUSize destination_index,
    const ZyanStringView* source, ZyanUSize source_index, ZyanUSize count)
{
    if (!destination || !source || !source->string.vector.size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (destination_index == destination->vector.size)
    {
        return ZyanStringAppendEx(destination, source, source_index, count);
    }

    // Don't allow insertion after the terminating '\0' character
    if (destination_index >= destination->vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    // Don't allow access to the terminating '\0' character
    if (source_index + count >= source->string.vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_CHECK(ZyanVectorInsertRange(&destination->vector, destination_index,
        (char*)source->string.vector.data + source_index, count));
    ZYCORE_STRING_ASSERT_NULLTERMINATION(destination);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Appending                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringAppend(ZyanString* destination, const ZyanStringView* source)
{
    if (!destination || !source || !source->string.vector.size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = destination->vector.size;
    ZYAN_CHECK(ZyanVectorResize(&destination->vector, len + source->string.vector.size - 1));
    ZYAN_MEMCPY((char*)destination->vector.data + len - 1, source->string.vector.data,
        source->string.vector.size - 1);
    ZYCORE_STRING_NULLTERMINATE(destination);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringAppendEx(ZyanString* destination, const ZyanStringView* source,
    ZyanUSize source_index, ZyanUSize count)
{
    if (!destination || !source || !source->string.vector.size)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow access to the terminating '\0' character
    if (source_index + count >= source->string.vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    const ZyanUSize len = destination->vector.size;
    ZYAN_CHECK(ZyanVectorResize(&destination->vector, len + count));
    ZYAN_MEMCPY((char*)destination->vector.data + len - 1,
        (const char*)source->string.vector.data + source_index, count);
    ZYCORE_STRING_NULLTERMINATE(destination);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Deletion                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringDelete(ZyanString* string, ZyanUSize index, ZyanUSize count)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow removal of the terminating '\0' character
    if (index + count >= string->vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_CHECK(ZyanVectorDeleteRange(&string->vector, index, count));
    ZYCORE_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringTruncate(ZyanString* string, ZyanUSize index)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow removal of the terminating '\0' character
    if (index >= string->vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_CHECK(ZyanVectorDeleteRange(&string->vector, index, string->vector.size - index - 1));
    ZYCORE_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringClear(ZyanString* string)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_CHECK(ZyanVectorClear(&string->vector));
    // `ZyanVector` guarantees a minimum capacity of 1 element/character
    ZYAN_ASSERT(string->vector.capacity >= 1);

    *(char*)string->vector.data = '\0';
    string->vector.size++;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Searching                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringLPos(const ZyanStringView* haystack, const ZyanStringView* needle,
    ZyanISize* found_index)
{
    if (!haystack)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanStringLPosEx(haystack, needle, found_index, 0, haystack->string.vector.size - 1);
}

ZyanStatus ZyanStringLPosEx(const ZyanStringView* haystack, const ZyanStringView* needle,
    ZyanISize* found_index, ZyanUSize index, ZyanUSize count)
{
    if (!haystack || !needle || !found_index)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow access to the terminating '\0' character
    if (index + count >= haystack->string.vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if ((haystack->string.vector.size == 1) || (needle->string.vector.size == 1) ||
        (haystack->string.vector.size < needle->string.vector.size))
    {
        *found_index = -1;
        return ZYAN_STATUS_FALSE;
    }

    const char* s = (const char*)haystack->string.vector.data + index;
    const char* b = (const char*)needle->string.vector.data;
    for (; s + 1 < (const char*)haystack->string.vector.data + haystack->string.vector.size; ++s)
    {
        if (*s != *b)
        {
            continue;
        }
        const char* a = s;
        for (;;)
        {
            if ((ZyanUSize)(a - (const char*)haystack->string.vector.data) > index + count)
            {
                *found_index = -1;
                return ZYAN_STATUS_FALSE;
            }
            if (*b == 0)
            {
                *found_index = (ZyanISize)(s - (const char*)haystack->string.vector.data);
                return ZYAN_STATUS_TRUE;
            }
            if (*a++ != *b++)
            {
                break;
            }
        }
        b = (char*)needle->string.vector.data;
    }

    *found_index = -1;
    return ZYAN_STATUS_FALSE;
}

ZyanStatus ZyanStringLPosI(const ZyanStringView* haystack, const ZyanStringView* needle,
    ZyanISize* found_index)
{
    if (!haystack)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanStringLPosIEx(haystack, needle, found_index, 0, haystack->string.vector.size - 1);
}

ZyanStatus ZyanStringLPosIEx(const ZyanStringView* haystack, const ZyanStringView* needle,
    ZyanISize* found_index, ZyanUSize index, ZyanUSize count)
{
    // This solution assumes that characters are represented using ASCII representation, i.e.,
    // codes for 'a', 'b', 'c', .. 'z' are 97, 98, 99, .. 122 respectively. And codes for 'A',
    // 'B', 'C', .. 'Z' are 65, 66, .. 95 respectively.

    if (!haystack || !needle || !found_index)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow access to the terminating '\0' character
    if (index + count >= haystack->string.vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if ((haystack->string.vector.size == 1) || (needle->string.vector.size == 1) ||
        (haystack->string.vector.size < needle->string.vector.size))
    {
        *found_index = -1;
        return ZYAN_STATUS_FALSE;
    }

    const char* s = (const char*)haystack->string.vector.data + index;
    const char* b = (const char*)needle->string.vector.data;
    for (; s + 1 < (const char*)haystack->string.vector.data + haystack->string.vector.size; ++s)
    {
        if ((*s != *b) && ((*s ^ 32) != *b))
        {
            continue;
        }
        const char* a = s;
        for (;;)
        {
            if ((ZyanUSize)(a - (const char*)haystack->string.vector.data) > index + count)
            {
                *found_index = -1;
                return ZYAN_STATUS_FALSE;
            }
            if (*b == 0)
            {
                *found_index = (ZyanISize)(s - (const char*)haystack->string.vector.data);
                return ZYAN_STATUS_TRUE;
            }
            const char c1 = *a++;
            const char c2 = *b++;
            if ((c1 != c2) && ((c1 ^ 32) != c2))
            {
                break;
            }
        }
        b = (char*)needle->string.vector.data;
    }

    *found_index = -1;
    return ZYAN_STATUS_FALSE;
}

ZyanStatus ZyanStringRPos(const ZyanStringView* haystack, const ZyanStringView* needle,
    ZyanISize* found_index)
{
    if (!haystack)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanStringRPosEx(haystack, needle, found_index, haystack->string.vector.size - 1,
        haystack->string.vector.size - 1);
}

ZyanStatus ZyanStringRPosEx(const ZyanStringView* haystack, const ZyanStringView* needle,
    ZyanISize* found_index, ZyanUSize index, ZyanUSize count)
{
    if (!haystack || !needle || !found_index)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow access to the terminating '\0' character
    if ((index >= haystack->string.vector.size) || (count > index))
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if (!index || !count ||
        (haystack->string.vector.size == 1) || (needle->string.vector.size == 1) ||
        (haystack->string.vector.size < needle->string.vector.size))
    {
        *found_index = -1;
        return ZYAN_STATUS_FALSE;
    }

    const char* s = (const char*)haystack->string.vector.data + index - 1;
    const char* b = (const char*)needle->string.vector.data + needle->string.vector.size - 2;
    for (; s >= (const char*)haystack->string.vector.data; --s)
    {
        if (*s != *b)
        {
            continue;
        }
        const char* a = s;
        for (;;)
        {
            if (b < (const char*)needle->string.vector.data)
            {
                *found_index = (ZyanISize)(a - (const char*)haystack->string.vector.data + 1);
                return ZYAN_STATUS_TRUE;
            }
            if (a < (const char*)haystack->string.vector.data + index - count)
            {
                *found_index = -1;
                return ZYAN_STATUS_FALSE;
            }
            if (*a-- != *b--)
            {
                break;
            }
        }
        b = (char*)needle->string.vector.data + needle->string.vector.size - 2;
    }

    *found_index = -1;
    return ZYAN_STATUS_FALSE;
}

ZyanStatus ZyanStringRPosI(const ZyanStringView* haystack, const ZyanStringView* needle,
    ZyanISize* found_index)
{
    if (!haystack)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanStringRPosIEx(haystack, needle, found_index, haystack->string.vector.size - 1,
        haystack->string.vector.size - 1);
}

ZyanStatus ZyanStringRPosIEx(const ZyanStringView* haystack, const ZyanStringView* needle,
    ZyanISize* found_index, ZyanUSize index, ZyanUSize count)
{
    // This solution assumes that characters are represented using ASCII representation, i.e.,
    // codes for 'a', 'b', 'c', .. 'z' are 97, 98, 99, .. 122 respectively. And codes for 'A',
    // 'B', 'C', .. 'Z' are 65, 66, .. 95 respectively.

    if (!haystack || !needle || !found_index)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow access to the terminating '\0' character
    if ((index >= haystack->string.vector.size) || (count > index))
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if (!index || !count ||
        (haystack->string.vector.size == 1) || (needle->string.vector.size == 1) ||
        (haystack->string.vector.size < needle->string.vector.size))
    {
        *found_index = -1;
        return ZYAN_STATUS_FALSE;
    }

    const char* s = (const char*)haystack->string.vector.data + index - 1;
    const char* b = (const char*)needle->string.vector.data + needle->string.vector.size - 2;
    for (; s >= (const char*)haystack->string.vector.data; --s)
    {
        if ((*s != *b) && ((*s ^ 32) != *b))
        {
            continue;
        }
        const char* a = s;
        for (;;)
        {
            if (b < (const char*)needle->string.vector.data)
            {
                *found_index = (ZyanISize)(a - (const char*)haystack->string.vector.data + 1);
                return ZYAN_STATUS_TRUE;
            }
            if (a < (const char*)haystack->string.vector.data + index - count)
            {
                *found_index = -1;
                return ZYAN_STATUS_FALSE;
            }
            const char c1 = *a--;
            const char c2 = *b--;
            if ((c1 != c2) && ((c1 ^ 32) != c2))
            {
                break;
            }
        }
        b = (char*)needle->string.vector.data + needle->string.vector.size - 2;
    }

    *found_index = -1;
    return ZYAN_STATUS_FALSE;
}

/* ---------------------------------------------------------------------------------------------- */
/* Comparing                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringCompare(const ZyanStringView* s1, const ZyanStringView* s2, ZyanI32* result)
{
    if (!s1 || !s2)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (s1->string.vector.size < s2->string.vector.size)
    {
        *result = -1;
        return ZYAN_STATUS_FALSE;
    }
    if (s1->string.vector.size > s2->string.vector.size)
    {
        *result =  1;
        return ZYAN_STATUS_FALSE;
    }

    const char* const a = (char*)s1->string.vector.data;
    const char* const b = (char*)s2->string.vector.data;
    ZyanUSize i;
    for (i = 0; (i + 1 < s1->string.vector.size) && (i + 1 < s2->string.vector.size); ++i)
    {
        if (a[i] == b[i])
        {
            continue;
        }
        break;
    }

    if (a[i] == b[i])
    {
        *result = 0;
        return ZYAN_STATUS_TRUE;
    }

    if ((a[i] | 32) < (b[i] | 32))
    {
        *result = -1;
        return ZYAN_STATUS_FALSE;
    }

    *result = 1;
    return ZYAN_STATUS_FALSE;
}

ZyanStatus ZyanStringCompareI(const ZyanStringView* s1, const ZyanStringView* s2, ZyanI32* result)
{
    // This solution assumes that characters are represented using ASCII representation, i.e.,
    // codes for 'a', 'b', 'c', .. 'z' are 97, 98, 99, .. 122 respectively. And codes for 'A',
    // 'B', 'C', .. 'Z' are 65, 66, .. 95 respectively.

    if (!s1 || !s2)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (s1->string.vector.size < s2->string.vector.size)
    {
        *result = -1;
        return ZYAN_STATUS_FALSE;
    }
    if (s1->string.vector.size > s2->string.vector.size)
    {
        *result =  1;
        return ZYAN_STATUS_FALSE;
    }

    const char* const a = (char*)s1->string.vector.data;
    const char* const b = (char*)s2->string.vector.data;
    ZyanUSize i;
    for (i = 0; (i + 1 < s1->string.vector.size) && (i + 1 < s2->string.vector.size); ++i)
    {
        if ((a[i] == b[i]) || ((a[i] ^ 32) == b[i]))
        {
            continue;
        }
        break;
    }

    if (a[i] == b[i])
    {
        *result = 0;
        return ZYAN_STATUS_TRUE;
    }

    if ((a[i] | 32) < (b[i] | 32))
    {
        *result = -1;
        return ZYAN_STATUS_FALSE;
    }

    *result = 1;
    return ZYAN_STATUS_FALSE;
}

/* ---------------------------------------------------------------------------------------------- */
/* Case conversion                                                                                */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringToLowerCase(ZyanString* string)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanStringToLowerCaseEx(string, 0, string->vector.size - 1);
}

ZyanStatus ZyanStringToLowerCaseEx(ZyanString* string, ZyanUSize index, ZyanUSize count)
{
    // This solution assumes that characters are represented using ASCII representation, i.e.,
    // codes for 'a', 'b', 'c', .. 'z' are 97, 98, 99, .. 122 respectively. And codes for 'A',
    // 'B', 'C', .. 'Z' are 65, 66, .. 95 respectively.

    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow access to the terminating '\0' character
    if (index + count >= string->vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    char* s = (char*)string->vector.data + index;
    for (ZyanUSize i = index; i < index + count; ++i)
    {
        const char c = *s;
        if ((c >= 'A') && (c <= 'Z'))
        {
            *s = c | 32;
        }
        ++s;
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringToUpperCase(ZyanString* string)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanStringToUpperCaseEx(string, 0, string->vector.size - 1);
}

ZyanStatus ZyanStringToUpperCaseEx(ZyanString* string, ZyanUSize index, ZyanUSize count)
{
    // This solution assumes that characters are represented using ASCII representation, i.e.,
    // codes for 'a', 'b', 'c', .. 'z' are 97, 98, 99, .. 122 respectively. And codes for 'A',
    // 'B', 'C', .. 'Z' are 65, 66, .. 95 respectively.

    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Don't allow access to the terminating '\0' character
    if (index + count >= string->vector.size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    char* s = (char*)string->vector.data + index;
    for (ZyanUSize i = index; i < index + count; ++i)
    {
        const char c = *s;
        if ((c >= 'a') && (c <= 'z'))
        {
            *s = c & ~32;
        }
        ++s;
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringResize(ZyanString* string, ZyanUSize size)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_CHECK(ZyanVectorResize(&string->vector, size + 1));
    ZYCORE_STRING_NULLTERMINATE(string);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringReserve(ZyanString* string, ZyanUSize capacity)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorReserve(&string->vector, capacity);
}

ZyanStatus ZyanStringShrinkToFit(ZyanString* string)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorShrinkToFit(&string->vector);
}

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanStringGetCapacity(const ZyanString* string, ZyanUSize* capacity)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_ASSERT(string->vector.capacity >= 1);
    *capacity = string->vector.capacity - 1;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringGetSize(const ZyanString* string, ZyanUSize* size)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_ASSERT(string->vector.size >= 1);
    *size = string->vector.size - 1;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanStringGetData(const ZyanString* string, const char** value)
{
    if (!string)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *value = string->vector.data;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
