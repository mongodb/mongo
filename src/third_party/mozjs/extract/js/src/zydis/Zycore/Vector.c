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

#include "zydis/Zycore/LibC.h"
#include "zydis/Zycore/Vector.h"

/* ============================================================================================== */
/* Internal macros                                                                                */
/* ============================================================================================== */

/**
 * Checks, if the passed vector should grow.
 *
 * @param   size        The desired size of the vector.
 * @param   capacity    The current capacity of the vector.
 *
 * @return  `ZYAN_TRUE`, if the vector should grow or `ZYAN_FALSE`, if not.
 */
#define ZYCORE_VECTOR_SHOULD_GROW(size, capacity) \
    ((size) > (capacity))

/**
 * Checks, if the passed vector should shrink.
 *
 * @param   size        The desired size of the vector.
 * @param   capacity    The current capacity of the vector.
 * @param   threshold   The shrink threshold.
 *
 * @return  `ZYAN_TRUE`, if the vector should shrink or `ZYAN_FALSE`, if not.
 */
#define ZYCORE_VECTOR_SHOULD_SHRINK(size, capacity, threshold) \
    (((threshold) != 0) && ((size) * (threshold) < (capacity)))

/**
 * Returns the offset of the element at the given `index`.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The element index.
 *
 * @return  The offset of the element at the given `index`.
 */
#define ZYCORE_VECTOR_OFFSET(vector, index) \
    ((void*)((ZyanU8*)(vector)->data + ((index) * (vector)->element_size)))

/* ============================================================================================== */
/* Internal functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Helper functions                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Reallocates the internal buffer of the vector.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   capacity    The new capacity.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZyanVectorReallocate(ZyanVector* vector, ZyanUSize capacity)
{
    ZYAN_ASSERT(vector);
    ZYAN_ASSERT(vector->capacity >= ZYAN_VECTOR_MIN_CAPACITY);
    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    if (!vector->allocator)
    {
        if (vector->capacity < capacity)
        {
            return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
        }
        return ZYAN_STATUS_SUCCESS;
    }

    ZYAN_ASSERT(vector->allocator);
    ZYAN_ASSERT(vector->allocator->reallocate);

    if (capacity < ZYAN_VECTOR_MIN_CAPACITY)
    {
        if (vector->capacity > ZYAN_VECTOR_MIN_CAPACITY)
        {
            capacity = ZYAN_VECTOR_MIN_CAPACITY;
        } else
        {
            return ZYAN_STATUS_SUCCESS;
        }
    }

    vector->capacity = capacity;
    ZYAN_CHECK(vector->allocator->reallocate(vector->allocator, &vector->data,
        vector->element_size, vector->capacity));

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Shifts all elements starting at the specified `index` by the amount of `count` to the left.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The start index.
 * @param   count   The amount of shift operations.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZyanVectorShiftLeft(ZyanVector* vector, ZyanUSize index, ZyanUSize count)
{
    ZYAN_ASSERT(vector);
    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);
    ZYAN_ASSERT(count > 0);
    //ZYAN_ASSERT((ZyanISize)count - (ZyanISize)index + 1 >= 0);

    const void* const source = ZYCORE_VECTOR_OFFSET(vector, index + count);
    void* const dest         = ZYCORE_VECTOR_OFFSET(vector, index);
    const ZyanUSize size     = (vector->size - index - count) * vector->element_size;
    ZYAN_MEMMOVE(dest, source, size);

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Shifts all elements starting at the specified `index` by the amount of `count` to the right.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The start index.
 * @param   count   The amount of shift operations.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZyanVectorShiftRight(ZyanVector* vector, ZyanUSize index, ZyanUSize count)
{
    ZYAN_ASSERT(vector);
    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);
    ZYAN_ASSERT(count > 0);
    ZYAN_ASSERT(vector->size + count <= vector->capacity);

    const void* const source = ZYCORE_VECTOR_OFFSET(vector, index);
    void* const dest         = ZYCORE_VECTOR_OFFSET(vector, index + count);
    const ZyanUSize size     = (vector->size - index) * vector->element_size;
    ZYAN_MEMMOVE(dest, source, size);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constructor and destructor                                                                     */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

ZyanStatus ZyanVectorInit(ZyanVector* vector, ZyanUSize element_size, ZyanUSize capacity,
    ZyanMemberProcedure destructor)
{
    return ZyanVectorInitEx(vector, element_size, capacity, destructor, ZyanAllocatorDefault(),
        ZYAN_VECTOR_DEFAULT_GROWTH_FACTOR, ZYAN_VECTOR_DEFAULT_SHRINK_THRESHOLD);
}

#endif // ZYAN_NO_LIBC

ZyanStatus ZyanVectorInitEx(ZyanVector* vector, ZyanUSize element_size, ZyanUSize capacity,
    ZyanMemberProcedure destructor, ZyanAllocator* allocator, ZyanU8 growth_factor,
    ZyanU8 shrink_threshold)
{
    if (!vector || !element_size || !allocator || (growth_factor < 1))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_ASSERT(allocator->allocate);

    vector->allocator        = allocator;
    vector->growth_factor    = growth_factor;
    vector->shrink_threshold = shrink_threshold;
    vector->size             = 0;
    vector->capacity         = ZYAN_MAX(ZYAN_VECTOR_MIN_CAPACITY, capacity);
    vector->element_size     = element_size;
    vector->destructor       = destructor;
    vector->data             = ZYAN_NULL;

    return allocator->allocate(vector->allocator, &vector->data, vector->element_size,
        vector->capacity);
}

ZyanStatus ZyanVectorInitCustomBuffer(ZyanVector* vector, ZyanUSize element_size,
    void* buffer, ZyanUSize capacity, ZyanMemberProcedure destructor)
{
    if (!vector || !element_size || !buffer || !capacity)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    vector->allocator        = ZYAN_NULL;
    vector->growth_factor    = 1;
    vector->shrink_threshold = 0;
    vector->size             = 0;
    vector->capacity         = capacity;
    vector->element_size     = element_size;
    vector->destructor       = destructor;
    vector->data             = buffer;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorDestroy(ZyanVector* vector)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    if (vector->destructor)
    {
        for (ZyanUSize i = 0; i < vector->size; ++i)
        {
            vector->destructor(ZYCORE_VECTOR_OFFSET(vector, i));
        }
    }

    if (vector->allocator && vector->capacity)
    {
        ZYAN_ASSERT(vector->allocator->deallocate);
        ZYAN_CHECK(vector->allocator->deallocate(vector->allocator, vector->data,
            vector->element_size, vector->capacity));
    }

    vector->data = ZYAN_NULL;
    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Duplication                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

ZyanStatus ZyanVectorDuplicate(ZyanVector* destination, const ZyanVector* source,
    ZyanUSize capacity)
{
    return ZyanVectorDuplicateEx(destination, source, capacity, ZyanAllocatorDefault(),
        ZYAN_VECTOR_DEFAULT_GROWTH_FACTOR, ZYAN_VECTOR_DEFAULT_SHRINK_THRESHOLD);
}

#endif // ZYAN_NO_LIBC

ZyanStatus ZyanVectorDuplicateEx(ZyanVector* destination, const ZyanVector* source,
    ZyanUSize capacity, ZyanAllocator* allocator, ZyanU8 growth_factor, ZyanU8 shrink_threshold)
{
    if (!source)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = source->size;

    capacity = ZYAN_MAX(capacity, len);
    ZYAN_CHECK(ZyanVectorInitEx(destination, source->element_size, capacity, source->destructor,
        allocator, growth_factor, shrink_threshold));
    ZYAN_ASSERT(destination->capacity >= len);

    ZYAN_MEMCPY(destination->data, source->data, len * source->element_size);
    destination->size = len;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorDuplicateCustomBuffer(ZyanVector* destination, const ZyanVector* source,
    void* buffer, ZyanUSize capacity)
{
    if (!source)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanUSize len = source->size;

    if (capacity < len)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    ZYAN_CHECK(ZyanVectorInitCustomBuffer(destination, source->element_size, buffer, capacity,
        source->destructor));
    ZYAN_ASSERT(destination->capacity >= len);

    ZYAN_MEMCPY(destination->data, source->data, len * source->element_size);
    destination->size = len;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Element access                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

const void* ZyanVectorGet(const ZyanVector* vector, ZyanUSize index)
{
    if (!vector || (index >= vector->size))
    {
        return ZYAN_NULL;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    return ZYCORE_VECTOR_OFFSET(vector, index);
}

void* ZyanVectorGetMutable(const ZyanVector* vector, ZyanUSize index)
{
    if (!vector || (index >= vector->size))
    {
        return ZYAN_NULL;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    return ZYCORE_VECTOR_OFFSET(vector, index);
}

ZyanStatus ZyanVectorGetPointer(const ZyanVector* vector, ZyanUSize index, const void** value)
{
    if (!vector || !value)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index >= vector->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    *value = (const void*)ZYCORE_VECTOR_OFFSET(vector, index);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorGetPointerMutable(const ZyanVector* vector, ZyanUSize index, void** value)
{
    if (!vector || !value)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index >= vector->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    *value = ZYCORE_VECTOR_OFFSET(vector, index);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorSet(ZyanVector* vector, ZyanUSize index, const void* value)
{
    if (!vector || !value)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index >= vector->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    void* const offset = ZYCORE_VECTOR_OFFSET(vector, index);
    if (vector->destructor)
    {
        vector->destructor(offset);
    }
    ZYAN_MEMCPY(offset, value, vector->element_size);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Insertion                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanVectorPushBack(ZyanVector* vector, const void* element)
{
    if (!vector || !element)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    if (ZYCORE_VECTOR_SHOULD_GROW(vector->size + 1, vector->capacity))
    {
        ZYAN_CHECK(ZyanVectorReallocate(vector,
            ZYAN_MAX(1, (ZyanUSize)((vector->size + 1) * vector->growth_factor))));
    }

    void* const offset = ZYCORE_VECTOR_OFFSET(vector, vector->size);
    ZYAN_MEMCPY(offset, element, vector->element_size);

    ++vector->size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorInsert(ZyanVector* vector, ZyanUSize index, const void* element)
{
    return ZyanVectorInsertRange(vector, index, element, 1);
}

ZyanStatus ZyanVectorInsertRange(ZyanVector* vector, ZyanUSize index, const void* elements,
    ZyanUSize count)
{
    if (!vector || !elements || !count)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index > vector->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    if (ZYCORE_VECTOR_SHOULD_GROW(vector->size + count, vector->capacity))
    {
        ZYAN_CHECK(ZyanVectorReallocate(vector,
            ZYAN_MAX(1, (ZyanUSize)((vector->size + count) * vector->growth_factor))));
    }

    if (index < vector->size)
    {
        ZYAN_CHECK(ZyanVectorShiftRight(vector, index, count));
    }

    void* const offset = ZYCORE_VECTOR_OFFSET(vector, index);
    ZYAN_MEMCPY(offset, elements, count * vector->element_size);
    vector->size += count;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorEmplace(ZyanVector* vector, void** element, ZyanMemberFunction constructor)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorEmplaceEx(vector, vector->size, element, constructor);
}

ZyanStatus ZyanVectorEmplaceEx(ZyanVector* vector, ZyanUSize index, void** element,
    ZyanMemberFunction constructor)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index > vector->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    if (ZYCORE_VECTOR_SHOULD_GROW(vector->size + 1, vector->capacity))
    {
        ZYAN_CHECK(ZyanVectorReallocate(vector,
            ZYAN_MAX(1, (ZyanUSize)((vector->size + 1) * vector->growth_factor))));
    }

    if (index < vector->size)
    {
        ZYAN_CHECK(ZyanVectorShiftRight(vector, index, 1));
    }

    *element = ZYCORE_VECTOR_OFFSET(vector, index);
    if (constructor)
    {
        ZYAN_CHECK(constructor(*element));
    }

    ++vector->size;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Utils                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanVectorSwapElements(ZyanVector* vector, ZyanUSize index_first, ZyanUSize index_second)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if ((index_first >= vector->size) || (index_second >= vector->size))
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if (vector->size == vector->capacity)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    ZyanU64* const t = ZYCORE_VECTOR_OFFSET(vector, vector->size);
    ZyanU64* const a = ZYCORE_VECTOR_OFFSET(vector, index_first);
    ZyanU64* const b = ZYCORE_VECTOR_OFFSET(vector, index_second);
    ZYAN_MEMCPY(t, a, vector->element_size);
    ZYAN_MEMCPY(a, b, vector->element_size);
    ZYAN_MEMCPY(b, t, vector->element_size);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Deletion                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanVectorDelete(ZyanVector* vector, ZyanUSize index)
{
    return ZyanVectorDeleteRange(vector, index, 1);
}

ZyanStatus ZyanVectorDeleteRange(ZyanVector* vector, ZyanUSize index, ZyanUSize count)
{
    if (!vector || !count)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index + count > vector->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if (vector->destructor)
    {
        for (ZyanUSize i = index; i < index + count; ++i)
        {
            vector->destructor(ZYCORE_VECTOR_OFFSET(vector, i));
        }
    }

    if (index + count < vector->size)
    {
        ZYAN_CHECK(ZyanVectorShiftLeft(vector, index, count));
    }

    vector->size -= count;
    if (ZYCORE_VECTOR_SHOULD_SHRINK(vector->size, vector->capacity, vector->shrink_threshold))
    {
        return ZyanVectorReallocate(vector,
            ZYAN_MAX(1, (ZyanUSize)(vector->size * vector->growth_factor)));
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorPopBack(ZyanVector* vector)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (vector->size == 0)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if (vector->destructor)
    {
         vector->destructor(ZYCORE_VECTOR_OFFSET(vector, vector->size - 1));
    }

    --vector->size;
    if (ZYCORE_VECTOR_SHOULD_SHRINK(vector->size, vector->capacity, vector->shrink_threshold))
    {
        return ZyanVectorReallocate(vector,
            ZYAN_MAX(1, (ZyanUSize)(vector->size * vector->growth_factor)));
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorClear(ZyanVector* vector)
{
    return ZyanVectorResizeEx(vector, 0, ZYAN_NULL);
}

/* ---------------------------------------------------------------------------------------------- */
/* Searching                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanVectorFind(const ZyanVector* vector, const void* element, ZyanISize* found_index,
    ZyanEqualityComparison comparison)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorFindEx(vector, element, found_index, comparison, 0, vector->size);
}

ZyanStatus ZyanVectorFindEx(const ZyanVector* vector, const void* element, ZyanISize* found_index,
    ZyanEqualityComparison comparison, ZyanUSize index, ZyanUSize count)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if ((index + count > vector->size) || (index == vector->size))
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if (!count)
    {
        *found_index = -1;
        return ZYAN_STATUS_FALSE;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    for (ZyanUSize i = index; i < index + count; ++i)
    {
        if (comparison(ZYCORE_VECTOR_OFFSET(vector, i), element))
        {
            *found_index = i;
            return ZYAN_STATUS_TRUE;
        }
    }

    *found_index = -1;
    return ZYAN_STATUS_FALSE;
}

ZyanStatus ZyanVectorBinarySearch(const ZyanVector* vector, const void* element,
    ZyanUSize* found_index, ZyanComparison comparison)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorBinarySearchEx(vector, element, found_index, comparison, 0, vector->size);
}

ZyanStatus ZyanVectorBinarySearchEx(const ZyanVector* vector, const void* element,
    ZyanUSize* found_index, ZyanComparison comparison, ZyanUSize index, ZyanUSize count)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (((index >= vector->size) && (count > 0)) || (index + count > vector->size))
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    if (!count)
    {
        *found_index = index;
        return ZYAN_STATUS_FALSE;
    }

    ZYAN_ASSERT(vector->element_size);
    ZYAN_ASSERT(vector->data);

    ZyanStatus status = ZYAN_STATUS_FALSE;
    ZyanISize l = index;
    ZyanISize h = index + count - 1;
    while (l <= h)
    {
        const ZyanUSize mid = l + ((h - l) >> 1);
        const ZyanI32 cmp = comparison(ZYCORE_VECTOR_OFFSET(vector, mid), element);
        if (cmp < 0)
        {
            l = mid + 1;
        } else
        {
            h = mid - 1;
            if (cmp == 0)
            {
                status = ZYAN_STATUS_TRUE;
            }
        }
    }

    *found_index = l;
    return status;
}

/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanVectorResize(ZyanVector* vector, ZyanUSize size)
{
    return ZyanVectorResizeEx(vector, size, ZYAN_NULL);
}

ZyanStatus ZyanVectorResizeEx(ZyanVector* vector, ZyanUSize size, const void* initializer)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (size == vector->size)
    {
        return ZYAN_STATUS_SUCCESS;
    }

    if (vector->destructor && (size < vector->size))
    {
        for (ZyanUSize i = size; i < vector->size; ++i)
        {
            vector->destructor(ZYCORE_VECTOR_OFFSET(vector, i));
        }
    }

    if (ZYCORE_VECTOR_SHOULD_GROW(size, vector->capacity) ||
        ZYCORE_VECTOR_SHOULD_SHRINK(size, vector->capacity, vector->shrink_threshold))
    {
        ZYAN_ASSERT(vector->growth_factor >= 1);
        ZYAN_CHECK(ZyanVectorReallocate(vector, (ZyanUSize)(size * vector->growth_factor)));
    }

    if (initializer && (size > vector->size))
    {
        for (ZyanUSize i = vector->size; i < size; ++i)
        {
            ZYAN_MEMCPY(ZYCORE_VECTOR_OFFSET(vector, i), initializer, vector->element_size);
        }
    }

    vector->size = size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorReserve(ZyanVector* vector, ZyanUSize capacity)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (capacity > vector->capacity)
    {
        ZYAN_CHECK(ZyanVectorReallocate(vector, capacity));
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorShrinkToFit(ZyanVector* vector)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorReallocate(vector, vector->size);
}

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanVectorGetCapacity(const ZyanVector* vector, ZyanUSize* capacity)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *capacity = vector->capacity;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanVectorGetSize(const ZyanVector* vector, ZyanUSize* size)
{
    if (!vector)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *size = vector->size;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
