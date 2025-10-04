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

#include "zydis/Zycore/Allocator.h"
#include "zydis/Zycore/LibC.h"

/* ============================================================================================== */
/* Internal functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Default allocator                                                                              */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

static ZyanStatus ZyanAllocatorDefaultAllocate(ZyanAllocator* allocator, void** p,
    ZyanUSize element_size, ZyanUSize n)
{
    ZYAN_ASSERT(allocator);
    ZYAN_ASSERT(p);
    ZYAN_ASSERT(element_size);
    ZYAN_ASSERT(n);

    ZYAN_UNUSED(allocator);

    *p = ZYAN_MALLOC(element_size * n);
    if (!*p)
    {
        return ZYAN_STATUS_NOT_ENOUGH_MEMORY;
    }

    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZyanAllocatorDefaultReallocate(ZyanAllocator* allocator, void** p,
    ZyanUSize element_size, ZyanUSize n)
{
    ZYAN_ASSERT(allocator);
    ZYAN_ASSERT(p);
    ZYAN_ASSERT(element_size);
    ZYAN_ASSERT(n);

    ZYAN_UNUSED(allocator);

    void* const x = ZYAN_REALLOC(*p, element_size * n);
    if (!x)
    {
        return ZYAN_STATUS_NOT_ENOUGH_MEMORY;
    }
    *p = x;

    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZyanAllocatorDefaultDeallocate(ZyanAllocator* allocator, void* p,
    ZyanUSize element_size, ZyanUSize n)
{
    ZYAN_ASSERT(allocator);
    ZYAN_ASSERT(p);
    ZYAN_ASSERT(element_size);
    ZYAN_ASSERT(n);

    ZYAN_UNUSED(allocator);
    ZYAN_UNUSED(element_size);
    ZYAN_UNUSED(n);

    ZYAN_FREE(p);

    return ZYAN_STATUS_SUCCESS;
}

#endif // ZYAN_NO_LIBC

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

ZyanStatus ZyanAllocatorInit(ZyanAllocator* allocator, ZyanAllocatorAllocate allocate,
    ZyanAllocatorAllocate reallocate, ZyanAllocatorDeallocate deallocate)
{
    if (!allocator || !allocate || !reallocate || !deallocate)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    allocator->allocate   = allocate;
    allocator->reallocate = reallocate;
    allocator->deallocate = deallocate;

    return ZYAN_STATUS_SUCCESS;
}

#ifndef ZYAN_NO_LIBC

ZyanAllocator* ZyanAllocatorDefault(void)
{
    static ZyanAllocator allocator =
    {
        &ZyanAllocatorDefaultAllocate,
        &ZyanAllocatorDefaultReallocate,
        &ZyanAllocatorDefaultDeallocate
    };
    return &allocator;
}

#endif

/* ============================================================================================== */
