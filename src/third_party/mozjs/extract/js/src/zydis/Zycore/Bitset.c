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

#include "zydis/Zycore/Bitset.h"
#include "zydis/Zycore/LibC.h"

/* ============================================================================================== */
/* Internal constants                                                                             */
/* ============================================================================================== */

#define ZYAN_BITSET_GROWTH_FACTOR    2
#define ZYAN_BITSET_SHRINK_THRESHOLD 2

/* ============================================================================================== */
/* Internal macros                                                                                */
/* ============================================================================================== */

/**
 * Computes the smallest integer value not less than `x`.
 *
 * @param   x   The value.
 *
 * @return  The smallest integer value not less than `x`.
 */
#define ZYAN_BITSET_CEIL(x) \
    (((x) == ((ZyanU32)(x))) ? (ZyanU32)(x) : ((ZyanU32)(x)) + 1)

/**
 * Converts bits to bytes.
 *
 * @param   x   The value in bits.
 *
 * @return  The amount of bytes needed to fit `x` bits.
 */
#define ZYAN_BITSET_BITS_TO_BYTES(x) \
    ZYAN_BITSET_CEIL((x) / 8)

/**
 * Returns the offset of the given bit.
 *
 * @param   index   The bit index.
 *
 * @return  The offset of the given bit.
 */
#define ZYAN_BITSET_BIT_OFFSET(index) \
    (7 - ((index) % 8))

/* ============================================================================================== */
/* Internal functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Helper functions                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Initializes the given `vector` with `count` "zero"-bytes.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   count   The number of bytes.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZyanBitsetInitVectorElements(ZyanVector* vector, ZyanUSize count)
{
    ZYAN_ASSERT(vector);

    static const ZyanU8 zero = 0;
    for (ZyanUSize i = 0; i < count; ++i)
    {
        ZYAN_CHECK(ZyanVectorPushBack(vector, &zero));
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Byte operations                                                                                */
/* ---------------------------------------------------------------------------------------------- */

static ZyanStatus ZyanBitsetOperationAND(ZyanU8* b1, const ZyanU8* b2)
{
    *b1 &= *b2;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZyanBitsetOperationOR (ZyanU8* b1, const ZyanU8* b2)
{
    *b1 |= *b2;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZyanBitsetOperationXOR(ZyanU8* b1, const ZyanU8* b2)
{
    *b1 ^= *b2;
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

ZyanStatus ZyanBitsetInit(ZyanBitset* bitset, ZyanUSize count)
{
    return ZyanBitsetInitEx(bitset, count, ZyanAllocatorDefault(), ZYAN_BITSET_GROWTH_FACTOR,
        ZYAN_BITSET_SHRINK_THRESHOLD);
}

#endif // ZYAN_NO_LIBC

ZyanStatus ZyanBitsetInitEx(ZyanBitset* bitset, ZyanUSize count, ZyanAllocator* allocator,
    ZyanU8 growth_factor, ZyanU8 shrink_threshold)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanU32 bytes = ZYAN_BITSET_BITS_TO_BYTES(count);

    bitset->size = count;
    ZYAN_CHECK(ZyanVectorInitEx(&bitset->bits, sizeof(ZyanU8), bytes, ZYAN_NULL, allocator,
        growth_factor, shrink_threshold));
    ZYAN_CHECK(ZyanBitsetInitVectorElements(&bitset->bits, bytes));

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetInitBuffer(ZyanBitset* bitset, ZyanUSize count, void* buffer,
    ZyanUSize capacity)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    const ZyanU32 bytes = ZYAN_BITSET_BITS_TO_BYTES(count);
    if (capacity < bytes)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    bitset->size = count;
    ZYAN_CHECK(ZyanVectorInitCustomBuffer(&bitset->bits, sizeof(ZyanU8), buffer, capacity,
        ZYAN_NULL));
    ZYAN_CHECK(ZyanBitsetInitVectorElements(&bitset->bits, bytes));

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetDestroy(ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorDestroy(&bitset->bits);
}

/* ---------------------------------------------------------------------------------------------- */
/* Logical operations                                                                             */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanBitsetPerformByteOperation(ZyanBitset* destination, const ZyanBitset* source,
    ZyanBitsetByteOperation operation)
{
    if (!destination || !source || !operation)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanUSize s1;
    ZyanUSize s2;
    ZYAN_CHECK(ZyanVectorGetSize(&destination->bits, &s1));
    ZYAN_CHECK(ZyanVectorGetSize(&source->bits, &s2));

    const ZyanUSize min = ZYAN_MIN(s1, s2);
    for (ZyanUSize i = 0; i < min; ++i)
    {
        ZyanU8* v1;
        const ZyanU8* v2;
        ZYAN_CHECK(ZyanVectorGetPointerMutable(&destination->bits, i, (void**)&v1));
        ZYAN_CHECK(ZyanVectorGetPointer(&source->bits, i, (const void**)&v2));

        ZYAN_ASSERT(v1);
        ZYAN_ASSERT(v2);

        ZYAN_CHECK(operation(v1, v2));
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetAND(ZyanBitset* destination, const ZyanBitset* source)
{
    return ZyanBitsetPerformByteOperation(destination, source, ZyanBitsetOperationAND);
}

ZyanStatus ZyanBitsetOR (ZyanBitset* destination, const ZyanBitset* source)
{
    return ZyanBitsetPerformByteOperation(destination, source, ZyanBitsetOperationOR );
}

ZyanStatus ZyanBitsetXOR(ZyanBitset* destination, const ZyanBitset* source)
{
    return ZyanBitsetPerformByteOperation(destination, source, ZyanBitsetOperationXOR);
}

ZyanStatus ZyanBitsetFlip(ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanUSize size;
    ZYAN_CHECK(ZyanVectorGetSize(&bitset->bits, &size));
    for (ZyanUSize i = 0; i < size; ++i)
    {
        ZyanU8* value;
        ZYAN_CHECK(ZyanVectorGetPointerMutable(&bitset->bits, i, (void**)&value));
        *value = ~(*value);
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Bit access                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanBitsetSet(ZyanBitset* bitset, ZyanUSize index)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index >= bitset->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZyanU8* value;
    ZYAN_CHECK(ZyanVectorGetPointerMutable(&bitset->bits, index / 8, (void**)&value));

    *value |= (1 << ZYAN_BITSET_BIT_OFFSET(index));

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetReset(ZyanBitset* bitset, ZyanUSize index)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index >= bitset->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZyanU8* value;
    ZYAN_CHECK(ZyanVectorGetPointerMutable(&bitset->bits, index / 8, (void**)&value));
    *value &= ~(1 << ZYAN_BITSET_BIT_OFFSET(index));

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetAssign(ZyanBitset* bitset, ZyanUSize index, ZyanBool value)
{
    if (value)
    {
        return ZyanBitsetSet(bitset, index);
    }
    return ZyanBitsetReset(bitset, index);
}

ZyanStatus ZyanBitsetToggle(ZyanBitset* bitset, ZyanUSize index)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index >= bitset->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    ZyanU8* value;
    ZYAN_CHECK(ZyanVectorGetPointerMutable(&bitset->bits, index / 8, (void**)&value));
    *value ^= (1 << ZYAN_BITSET_BIT_OFFSET(index));

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetTest(ZyanBitset* bitset, ZyanUSize index)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (index >= bitset->size)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }

    const ZyanU8* value;
    ZYAN_CHECK(ZyanVectorGetPointer(&bitset->bits, index / 8, (const void**)&value));
    if ((*value & (1 << ZYAN_BITSET_BIT_OFFSET(index))) == 0)
    {
        return ZYAN_STATUS_FALSE;
    }
    return ZYAN_STATUS_TRUE;
}

ZyanStatus ZyanBitsetTestMSB(ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanBitsetTest(bitset, bitset->size - 1);
}

ZyanStatus ZyanBitsetTestLSB(ZyanBitset* bitset)
{
    return ZyanBitsetTest(bitset, 0);
}

/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanBitsetSetAll(ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanUSize size;
    ZYAN_CHECK(ZyanVectorGetSize(&bitset->bits, &size));
    for (ZyanUSize i = 0; i < size; ++i)
    {
        ZyanU8* value;
        ZYAN_CHECK(ZyanVectorGetPointerMutable(&bitset->bits, i, (void**)&value));
        *value = 0xFF;
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetResetAll(ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanUSize size;
    ZYAN_CHECK(ZyanVectorGetSize(&bitset->bits, &size));
    for (ZyanUSize i = 0; i < size; ++i)
    {
        ZyanU8* value;
        ZYAN_CHECK(ZyanVectorGetPointerMutable(&bitset->bits, i, (void**)&value));
        *value = 0x00;
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Size management                                                                                */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanBitsetPush(ZyanBitset* bitset, ZyanBool value)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if ((bitset->size++ % 8) == 0)
    {
        static const ZyanU8 zero = 0;
        ZYAN_CHECK(ZyanVectorPushBack(&bitset->bits, &zero));
    }

    return ZyanBitsetAssign(bitset, bitset->size - 1, value);
}

ZyanStatus ZyanBitsetPop(ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if ((--bitset->size % 8) == 0)
    {
        return ZyanVectorPopBack(&bitset->bits);
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetClear(ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    bitset->size = 0;
    return ZyanVectorClear(&bitset->bits);
}

/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanBitsetReserve(ZyanBitset* bitset, ZyanUSize count)
{
    return ZyanVectorReserve(&bitset->bits, ZYAN_BITSET_BITS_TO_BYTES(count));
}

ZyanStatus ZyanBitsetShrinkToFit(ZyanBitset* bitset)
{
    return ZyanVectorShrinkToFit(&bitset->bits);
}

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanBitsetGetSize(const ZyanBitset* bitset, ZyanUSize* size)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *size = bitset->size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetGetCapacity(const ZyanBitset* bitset, ZyanUSize* capacity)
{
    ZYAN_CHECK(ZyanBitsetGetCapacityBytes(bitset, capacity));
    *capacity *= 8;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetGetSizeBytes(const ZyanBitset* bitset, ZyanUSize* size)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorGetSize(&bitset->bits, size);
}

ZyanStatus ZyanBitsetGetCapacityBytes(const ZyanBitset* bitset, ZyanUSize* capacity)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZyanVectorGetCapacity(&bitset->bits, capacity);
}

/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanBitsetCount(const ZyanBitset* bitset, ZyanUSize* count)
{
    if (!bitset || !count)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *count = 0;

    ZyanUSize size;
    ZYAN_CHECK(ZyanVectorGetSize(&bitset->bits, &size));
    for (ZyanUSize i = 0; i < size; ++i)
    {
        ZyanU8* value;
        ZYAN_CHECK(ZyanVectorGetPointer(&bitset->bits, i, (const void**)&value));

        ZyanU8 popcnt = *value;
        popcnt = (popcnt & 0x55) + ((popcnt >> 1) & 0x55);
        popcnt = (popcnt & 0x33) + ((popcnt >> 2) & 0x33);
        popcnt = (popcnt & 0x0F) + ((popcnt >> 4) & 0x0F);

        *count += popcnt;
    }

    *count = ZYAN_MIN(*count, bitset->size);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanBitsetAll(const ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanUSize size;
    ZYAN_CHECK(ZyanVectorGetSize(&bitset->bits, &size));
    for (ZyanUSize i = 0; i < size; ++i)
    {
        ZyanU8* value;
        ZYAN_CHECK(ZyanVectorGetPointer(&bitset->bits, i, (const void**)&value));
        if (i < (size - 1))
        {
            if (*value != 0xFF)
            {
                return ZYAN_STATUS_FALSE;
            }
        } else
        {
            const ZyanU8 mask = ~(8 - (bitset->size % 8));
            if ((*value & mask) != mask)
            {
                return ZYAN_STATUS_FALSE;
            }
        }
    }

    return ZYAN_STATUS_TRUE;
}

ZyanStatus ZyanBitsetAny(const ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanUSize size;
    ZYAN_CHECK(ZyanVectorGetSize(&bitset->bits, &size));
    for (ZyanUSize i = 0; i < size; ++i)
    {
        ZyanU8* value;
        ZYAN_CHECK(ZyanVectorGetPointer(&bitset->bits, i, (const void**)&value));
        if (i < (size - 1))
        {
            if (*value != 0x00)
            {
                return ZYAN_STATUS_TRUE;
            }
        } else
        {
            const ZyanU8 mask = ~(8 - (bitset->size % 8));
            if ((*value & mask) != 0x00)
            {
                return ZYAN_STATUS_TRUE;
            }
        }
    }

    return ZYAN_STATUS_FALSE;
}

ZyanStatus ZyanBitsetNone(const ZyanBitset* bitset)
{
    if (!bitset)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanUSize size;
    ZYAN_CHECK(ZyanVectorGetSize(&bitset->bits, &size));
    for (ZyanUSize i = 0; i < size; ++i)
    {
        ZyanU8* value;
        ZYAN_CHECK(ZyanVectorGetPointer(&bitset->bits, i, (const void**)&value));
        if (i < (size - 1))
        {
            if (*value != 0x00)
            {
                return ZYAN_STATUS_FALSE;
            }
        } else
        {
            const ZyanU8 mask = ~(8 - (bitset->size % 8));
            if ((*value & mask) != 0x00)
            {
                return ZYAN_STATUS_FALSE;
            }
        }
    }

    return ZYAN_STATUS_TRUE;
}

/* ---------------------------------------------------------------------------------------------- */

//ZyanStatus ZyanBitsetToU32(const ZyanBitset* bitset, ZyanU32* value)
//{
//    if (!bitset)
//    {
//        return ZYAN_STATUS_INVALID_ARGUMENT;
//    }
//    if (bitset->size > 32)
//    {
//        return ZYAN_STATUS_INVALID_OPERATION;
//    }
//
//    // TODO:
//
//    return ZYAN_STATUS_SUCCESS;
//}
//
//ZyanStatus ZyanBitsetToU64(const ZyanBitset* bitset, ZyanU64* value)
//{
//    if (!bitset)
//    {
//        return ZYAN_STATUS_INVALID_ARGUMENT;
//    }
//    if (bitset->size > 64)
//    {
//        return ZYAN_STATUS_INVALID_OPERATION;
//    }
//
//    // TODO:
//
//    return ZYAN_STATUS_SUCCESS;
//}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
