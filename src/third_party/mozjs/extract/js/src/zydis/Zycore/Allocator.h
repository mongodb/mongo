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
 * @brief
 */

#ifndef ZYCORE_ALLOCATOR_H
#define ZYCORE_ALLOCATOR_H

#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

struct ZyanAllocator_;

/**
 * Defines the `ZyanAllocatorAllocate` function prototype.
 *
 * @param   allocator       A pointer to the `ZyanAllocator` instance.
 * @param   p               Receives a pointer to the first memory block sufficient to hold an
 *                          array of `n` elements with a size of `element_size`.
 * @param   element_size    The size of a single element.
 * @param   n               The number of elements to allocate storage for.
 *
 * @return  A zyan status code.
 *
 * This prototype is used for the `allocate()` and `reallocate()` functions.
 *
 * The result of the `reallocate()` function is undefined, if `p` does not point to a memory block
 * previously obtained by `(re-)allocate()`.
 */
typedef ZyanStatus (*ZyanAllocatorAllocate)(struct ZyanAllocator_* allocator, void** p,
    ZyanUSize element_size, ZyanUSize n);

/**
 * Defines the `ZyanAllocatorDeallocate` function prototype.
 *
 * @param   allocator       A pointer to the `ZyanAllocator` instance.
 * @param   p               The pointer obtained from `(re-)allocate()`.
 * @param   element_size    The size of a single element.
 * @param   n               The number of elements earlier passed to `(re-)allocate()`.
 *
  * @return  A zyan status code.
 */
typedef ZyanStatus (*ZyanAllocatorDeallocate)(struct ZyanAllocator_* allocator, void* p,
    ZyanUSize element_size, ZyanUSize n);

/**
 * Defines the `ZyanAllocator` struct.
 *
 * This is the base class for all custom allocator implementations.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZyanAllocator_
{
    /**
     * The allocate function.
     */
    ZyanAllocatorAllocate allocate;
    /**
     * The reallocate function.
     */
    ZyanAllocatorAllocate reallocate;
    /**
     * The deallocate function.
     */
    ZyanAllocatorDeallocate deallocate;
} ZyanAllocator;

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * Initializes the given `ZyanAllocator` instance.
 *
 * @param   allocator   A pointer to the `ZyanAllocator` instance.
 * @param   allocate    The allocate function.
 * @param   reallocate  The reallocate function.
 * @param   deallocate  The deallocate function.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanAllocatorInit(ZyanAllocator* allocator, ZyanAllocatorAllocate allocate,
    ZyanAllocatorAllocate reallocate, ZyanAllocatorDeallocate deallocate);

#ifndef ZYAN_NO_LIBC

/**
 * Returns the default `ZyanAllocator` instance.
 *
 * @return  A pointer to the default `ZyanAllocator` instance.
 *
 * The default allocator uses the default memory manager to allocate memory on the heap.
 *
 * You should in no case modify the returned allocator instance to avoid unexpected behavior.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanAllocator* ZyanAllocatorDefault(void);

#endif // ZYAN_NO_LIBC

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_ALLOCATOR_H */
