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

#ifndef ZYCORE_API_MEMORY_H
#define ZYCORE_API_MEMORY_H

#include "zydis/Zycore/Defines.h"
#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Types.h"

#ifndef ZYAN_NO_LIBC

#if   defined(ZYAN_WINDOWS)
#   include <windows.h>
#elif defined(ZYAN_POSIX)
#   include <sys/mman.h>
#else
#   error "Unsupported platform detected"
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZyanMemoryPageProtection` enum.
 */
typedef enum ZyanMemoryPageProtection_
{
#if   defined(ZYAN_WINDOWS)

    ZYAN_PAGE_READONLY          = PAGE_READONLY,
    ZYAN_PAGE_READWRITE         = PAGE_READWRITE,
    ZYAN_PAGE_EXECUTE           = PAGE_EXECUTE,
    ZYAN_PAGE_EXECUTE_READ      = PAGE_EXECUTE_READ,
    ZYAN_PAGE_EXECUTE_READWRITE = PAGE_EXECUTE_READWRITE

#elif defined(ZYAN_POSIX)

    ZYAN_PAGE_READONLY          = PROT_READ,
    ZYAN_PAGE_READWRITE         = PROT_READ | PROT_WRITE,
    ZYAN_PAGE_EXECUTE           = PROT_EXEC,
    ZYAN_PAGE_EXECUTE_READ      = PROT_EXEC | PROT_READ,
    ZYAN_PAGE_EXECUTE_READWRITE = PROT_EXEC | PROT_READ | PROT_WRITE

#endif
} ZyanMemoryPageProtection;

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* General                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the system page size.
 *
 * @return  The system page size.
 */
ZYCORE_EXPORT ZyanU32 ZyanMemoryGetSystemPageSize();

/**
 * Returns the system allocation granularity.
 *
 * The system allocation granularity specifies the minimum amount of bytes which can be allocated
 * at a specific address by a single call of `ZyanMemoryVirtualAlloc`.
 *
 * This value is typically 64KiB on Windows systems and equal to the page size on most POSIX
 * platforms.
 *
 * @return  The system allocation granularity.
 */
ZYCORE_EXPORT ZyanU32 ZyanMemoryGetSystemAllocationGranularity();

/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Changes the memory protection value of one or more pages.
 *
 * @param   address     The start address aligned to a page boundary.
 * @param   size        The size.
 * @param   protection  The new page protection value.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanMemoryVirtualProtect(void* address, ZyanUSize size,
    ZyanMemoryPageProtection protection);

/**
 * Releases one or more memory pages starting at the given address.
 *
 * @param   address The start address aligned to a page boundary.
 * @param   size    The size.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanMemoryVirtualFree(void* address, ZyanUSize size);

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#endif /* ZYAN_NO_LIBC */

#endif /* ZYCORE_API_MEMORY_H */
