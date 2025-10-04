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
 * Implements the bitset class.
 */

#ifndef ZYCORE_BITSET_H
#define ZYCORE_BITSET_H

#include "zydis/Zycore/Allocator.h"
#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Types.h"
#include "zydis/Zycore/Vector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZyanVector` struct.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZyanBitset_
{
    /**
     * The bitset size.
     */
    ZyanUSize size;
    /**
     * The bitset data.
     */
    ZyanVector bits;
} ZyanBitset;

/**
 * Defines the `ZyanBitsetByteOperation` function prototype.
 *
 * @param   v1  A pointer to the first byte. This value receives the result after performing the
 *              desired operation.
 * @param   v2  A pointer to the second byte.
 *
 * @return  A zyan status code.
 *
 * This function is used to perform byte-wise operations on two `ZyanBitset` instances.
 */
typedef ZyanStatus (*ZyanBitsetByteOperation)(ZyanU8* v1, const ZyanU8* v2);

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constructor and destructor                                                                     */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

/**
 * Initializes the given `ZyanBitset` instance.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   count   The initial amount of bits.
 *
 * @return  A zyan status code.
 *
 * The space for the bitset is dynamically allocated by the default allocator using the default
 * growth factor and the default shrink threshold.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanBitsetInit(ZyanBitset* bitset, ZyanUSize count);

#endif // ZYAN_NO_LIBC

/**
 * Initializes the given `ZyanBitset` instance and sets a custom `allocator` and memory
 * allocation/deallocation parameters.
 *
 * @param   bitset              A pointer to the `ZyanBitset` instance.
 * @param   count               The initial amount of bits.
 * @param   allocator           A pointer to a `ZyanAllocator` instance.
 * @param   growth_factor       The growth factor.
 * @param   shrink_threshold    The shrink threshold.
 *
 * @return  A zyan status code.
 *
 * A growth factor of `1` disables overallocation and a shrink threshold of `0` disables
 * dynamic shrinking.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetInitEx(ZyanBitset* bitset, ZyanUSize count,
    ZyanAllocator* allocator, ZyanU8 growth_factor, ZyanU8 shrink_threshold);

/**
 * Initializes the given `ZyanBitset` instance and configures it to use a custom user
 * defined buffer with a fixed size.
 *
 * @param   bitset      A pointer to the `ZyanBitset` instance.
 * @param   count       The initial amount of bits.
 * @param   buffer      A pointer to the buffer that is used as storage for the bits.
 * @param   capacity    The maximum capacity (number of bytes) of the buffer.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetInitBuffer(ZyanBitset* bitset, ZyanUSize count, void* buffer,
    ZyanUSize capacity);

/**
 * Destroys the given `ZyanBitset` instance.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetDestroy(ZyanBitset* bitset);

/* ---------------------------------------------------------------------------------------------- */
/* Logical operations                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Performs a byte-wise `operation` for every byte in the given `ZyanBitset` instances.
 *
 * @param   destination A pointer to the `ZyanBitset` instance that is used as the first input and
 *                      as the destination.
 * @param   source      A pointer to the `ZyanBitset` instance that is used as the second input.
 * @param   operation   A pointer to the function that performs the desired operation.
 *
 * @return  A zyan status code.
 *
 * The `operation` callback is invoked once for every byte in the smallest of the `ZyanBitset`
 * instances.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetPerformByteOperation(ZyanBitset* destination,
    const ZyanBitset* source, ZyanBitsetByteOperation operation);

/**
 * Performs a logical `AND` operation on the given `ZyanBitset` instances.
 *
 * @param   destination A pointer to the `ZyanBitset` instance that is used as the first input and
 *                      as the destination.
 * @param   source      A pointer to the `ZyanBitset` instance that is used as the second input.
 *
 * @return  A zyan status code.
 *
 * If the destination bitmask contains more bits than the source one, the state of the remaining
 * bits will be undefined.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetAND(ZyanBitset* destination, const ZyanBitset* source);

/**
 * Performs a logical `OR`  operation on the given `ZyanBitset` instances.
 *
 * @param   destination A pointer to the `ZyanBitset` instance that is used as the first input and
 *                      as the destination.
 * @param   source      A pointer to the `ZyanBitset` instance that is used as the second input.
 *
 * @return  A zyan status code.
 *
 * If the destination bitmask contains more bits than the source one, the state of the remaining
 * bits will be undefined.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetOR (ZyanBitset* destination, const ZyanBitset* source);

/**
 * Performs a logical `XOR` operation on the given `ZyanBitset` instances.
 *
 * @param   destination A pointer to the `ZyanBitset` instance that is used as the first input and
 *                      as the destination.
 * @param   source      A pointer to the `ZyanBitset` instance that is used as the second input.
 *
 * @return  A zyan status code.
 *
 * If the destination bitmask contains more bits than the source one, the state of the remaining
 * bits will be undefined.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetXOR(ZyanBitset* destination, const ZyanBitset* source);

/**
 * Flips all bits of the given `ZyanBitset` instance.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetFlip(ZyanBitset* bitset);

/* ---------------------------------------------------------------------------------------------- */
/* Bit access                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Sets the bit at `index` of the given `ZyanBitset` instance to `1`.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   index   The bit index.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetSet(ZyanBitset* bitset, ZyanUSize index);

/**
 * Sets the bit at `index` of the given `ZyanBitset` instance to `0`.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   index   The bit index.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetReset(ZyanBitset* bitset, ZyanUSize index);

/**
 * Sets the bit at `index` of the given `ZyanBitset` instance to the specified `value`.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   index   The bit index.
 * @param   value   The new value.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetAssign(ZyanBitset* bitset, ZyanUSize index, ZyanBool value);

/**
 * Toggles the bit at `index` of the given `ZyanBitset` instance.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   index   The bit index.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetToggle(ZyanBitset* bitset, ZyanUSize index);

/**
 * Returns the value of the bit at `index`.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   index   The bit index.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the bit is set or `ZYAN_STATUS_FALSE`, if not, Another zyan
 *          status code, if an error occurred.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetTest(ZyanBitset* bitset, ZyanUSize index);

/**
 * Returns the value of the most significant bit.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the bit is set or `ZYAN_STATUS_FALSE`, if not. Another zyan
 *          status code, if an error occurred.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetTestMSB(ZyanBitset* bitset);

/**
 * Returns the value of the least significant bit.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the bit is set or `ZYAN_STATUS_FALSE`, if not. Another zyan
 *          status code, if an error occurred.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetTestLSB(ZyanBitset* bitset);

/* ---------------------------------------------------------------------------------------------- */

/**
 * Sets all bits of the given `ZyanBitset` instance to `1`.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetSetAll(ZyanBitset* bitset);

/**
 * Sets all bits of the given `ZyanBitset` instance to `0`.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetResetAll(ZyanBitset* bitset);

/* ---------------------------------------------------------------------------------------------- */
/* Size management                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Adds a new bit at the end of the bitset.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   value   The value of the new bit.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetPush(ZyanBitset* bitset, ZyanBool value);

/**
 * Removes the last bit of the bitset.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetPop(ZyanBitset* bitset);

/**
 * Deletes all bits of the given `ZyanBitset` instance.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetClear(ZyanBitset* bitset);

/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Changes the capacity of the given `ZyanBitset` instance.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   count   The new capacity (number of bits).
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetReserve(ZyanBitset* bitset, ZyanUSize count);

/**
 * Shrinks the capacity of the given bitset to match it's size.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetShrinkToFit(ZyanBitset* bitset);

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the current size of the bitset in bits.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   size    Receives the size of the bitset in bits.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetGetSize(const ZyanBitset* bitset, ZyanUSize* size);

/**
 * Returns the current capacity of the bitset in bits.
 *
 * @param   bitset      A pointer to the `ZyanBitset` instance.
 * @param   capacity    Receives the size of the bitset in bits.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetGetCapacity(const ZyanBitset* bitset, ZyanUSize* capacity);

/**
 * Returns the current size of the bitset in bytes.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   size    Receives the size of the bitset in bytes.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetGetSizeBytes(const ZyanBitset* bitset, ZyanUSize* size);

/**
 * Returns the current capacity of the bitset in bytes.
 *
 * @param   bitset      A pointer to the `ZyanBitset` instance.
 * @param   capacity    Receives the size of the bitset in bytes.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetGetCapacityBytes(const ZyanBitset* bitset, ZyanUSize* capacity);

/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the amount of bits set in the given bitset.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 * @param   count   Receives the amount of bits set in the given bitset.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetCount(const ZyanBitset* bitset, ZyanUSize* count);

/**
 * Checks, if all bits of the given bitset are set.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  `ZYAN_STATUS_TRUE`, if all bits are set, `ZYAN_STATUS_FALSE`, if not. Another zyan
 *          status code, if an error occurred.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetAll(const ZyanBitset* bitset);

/**
 * Checks, if at least one bit of the given bitset is set.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  `ZYAN_STATUS_TRUE`, if at least one bit is set, `ZYAN_STATUS_FALSE`, if not. Another
 *          zyan status code, if an error occurred.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetAny(const ZyanBitset* bitset);

/**
 * Checks, if none bits of the given bitset are set.
 *
 * @param   bitset  A pointer to the `ZyanBitset` instance.
 *
 * @return  `ZYAN_STATUS_TRUE`, if none bits are set, `ZYAN_STATUS_FALSE`, if not. Another zyan
 *          status code, if an error occurred.
 */
ZYCORE_EXPORT ZyanStatus ZyanBitsetNone(const ZyanBitset* bitset);

///* ---------------------------------------------------------------------------------------------- */
//
///**
// * Returns a 32-bit unsigned integer representation of the data.
// *
// * @param   bitset  A pointer to the `ZyanBitset` instance.
// * @param   value   Receives the 32-bit unsigned integer representation of the data.
// *
// * @return  A zyan status code.
// */
//ZYCORE_EXPORT ZyanStatus ZyanBitsetToU32(const ZyanBitset* bitset, ZyanU32* value);
//
///**
// * Returns a 64-bit unsigned integer representation of the data.
// *
// * @param   bitset  A pointer to the `ZyanBitset` instance.
// * @param   value   Receives the 64-bit unsigned integer representation of the data.
// *
// * @return  A zyan status code.
// */
//ZYCORE_EXPORT ZyanStatus ZyanBitsetToU64(const ZyanBitset* bitset, ZyanU64* value);

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_BITSET_H */
