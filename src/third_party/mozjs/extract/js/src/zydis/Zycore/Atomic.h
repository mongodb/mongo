/***************************************************************************************************

  Zyan Core Library (Zyan-C)

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
 * Cross compiler atomic intrinsics.
 */

#ifndef ZYCORE_ATOMIC_H
#define ZYCORE_ATOMIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zydis/Zycore/Defines.h"
#include "zydis/Zycore/Types.h"

/* ============================================================================================== */
/* Enums and Types                                                                                */
/* ============================================================================================== */

/*
 * Wraps a 32-bit value to provide atomic access.
 */
typedef struct ZyanAtomic32_
{
    ZyanU32 volatile value;
} ZyanAtomic32;

/*
 * Wraps a 64-bit value to provide atomic access.
 */
typedef struct ZyanAtomic64_
{
    ZyanU64 volatile value;
} ZyanAtomic64;

/*
 * Wraps a pointer-sized value to provide atomic access.
 */
typedef struct ZyanAtomicPointer_
{
    ZyanVoidPointer volatile value;
} ZyanAtomicPointer;

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Pointer sized                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @copydoc ZyanAtomicCompareExchange
 */
#define ZYAN_ATOMIC_COMPARE_EXCHANGE(destination, comparand, value) \
    ZyanAtomicCompareExchange((ZyanAtomicPointer*)&(destination), (comparand), (value))

/**
 * @copydoc ZyanAtomicIncrement
 */
#define ZYAN_ATOMIC_INCREMENT(destination) \
    ZyanAtomicIncrement((ZyanAtomicPointer*)&(destination));

/**
 * @copydoc ZyanAtomicDecrement
 */
#define ZYAN_ATOMIC_DECREMENT(destination) \
    ZyanAtomicDecrement((ZyanAtomicPointer*)&(destination));

/* ---------------------------------------------------------------------------------------------- */
/* 32-bit                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @copydoc ZyanAtomicCompareExchange
 */
#define ZYAN_ATOMIC_COMPARE_EXCHANGE32(destination, comparand, value) \
    ZyanAtomicCompareExchange32((ZyanAtomic32*)&(destination), (comparand), (value))

/**
 * @copydoc ZyanAtomicIncrement
 */
#define ZYAN_ATOMIC_INCREMENT32(destination) \
    ZyanAtomicIncrement32((ZyanAtomic32*)&(destination));

/**
 * @copydoc ZyanAtomicDecrement
 */
#define ZYAN_ATOMIC_DECREMENT32(destination) \
    ZyanAtomicDecrement32((ZyanAtomic32*)&(destination));

/* ---------------------------------------------------------------------------------------------- */
/* 64-bit                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @copydoc ZyanAtomicCompareExchange
 */
#define ZYAN_ATOMIC_COMPARE_EXCHANGE64(destination, comparand, value) \
    ZyanAtomicCompareExchange64((ZyanAtomic64*)&(destination), (comparand), (value))

/**
 * @copydoc ZyanAtomicIncrement
 */
#define ZYAN_ATOMIC_INCREMENT64(destination) \
    ZyanAtomicIncrement64((ZyanAtomic64*)&(destination));

/**
 * @copydoc ZyanAtomicDecrement
 */
#define ZYAN_ATOMIC_DECREMENT64(destination) \
    ZyanAtomicDecrement64((ZyanAtomic64*)&(destination));

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Functions                                                                                      */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Pointer sized                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Compares two values for equality and, if they are equal, replaces the first value.
 *
 * @param   destination A pointer to the destination value.
 * @param   comparand   The value to compare with.
 * @param   value       The replacement value.
 *
 * @return  The original value.
 */
static ZyanUPointer ZyanAtomicCompareExchange(ZyanAtomicPointer* destination,
    ZyanUPointer comparand, ZyanUPointer value);

/**
 * Increments the given value and stores the result, as an atomic operation.
 *
 * @param   destination A pointer to the destination value.
 *
 * @return  The incremented value.
*/
static ZyanUPointer ZyanAtomicIncrement(ZyanAtomicPointer* destination);

/**
 * Decrements the given value and stores the result, as an atomic operation.
 *
 * @param   destination A pointer to the destination value.
 *
 * @return  The decremented value.
*/
static ZyanUPointer ZyanAtomicDecrement(ZyanAtomicPointer* destination);

/* ---------------------------------------------------------------------------------------------- */
/* 32-bit                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @copydoc ZyanAtomicCompareExchange
 */
static ZyanU32 ZyanAtomicCompareExchange32(ZyanAtomic32* destination,
    ZyanU32 comparand, ZyanU32 value);

/**
 * @copydoc ZyanAtomicIncrement
 */
static ZyanU32 ZyanAtomicIncrement32(ZyanAtomic32* destination);

/**
 * @copydoc ZyanAtomicDecrement
 */
static ZyanU32 ZyanAtomicDecrement32(ZyanAtomic32* destination);

/* ---------------------------------------------------------------------------------------------- */
/* 64-bit                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @copydoc ZyanAtomicCompareExchange
 */
static ZyanU64 ZyanAtomicCompareExchange64(ZyanAtomic64* destination,
    ZyanU64 comparand, ZyanU64 value);

/**
 * @copydoc ZyanAtomicIncrement
 */
static ZyanU64 ZyanAtomicIncrement64(ZyanAtomic64* destination);

/**
 * @copydoc ZyanAtomicDecrement
 */
static ZyanU64 ZyanAtomicDecrement64(ZyanAtomic64* destination);

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#if defined(ZYAN_CLANG) || defined(ZYAN_GCC) || defined(ZYAN_ICC)
#   include "zydis/Zycore/Internal/AtomicGNU.h"
#elif defined(ZYAN_MSVC)
#   include "zydis/Zycore/Internal/AtomicMSVC.h"
#else
#   error "Unsupported compiler detected"
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_ATOMIC_H */
