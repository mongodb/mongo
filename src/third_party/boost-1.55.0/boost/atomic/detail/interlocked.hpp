#ifndef BOOST_ATOMIC_DETAIL_INTERLOCKED_HPP
#define BOOST_ATOMIC_DETAIL_INTERLOCKED_HPP

//  Copyright (c) 2009 Helge Bahmann
//  Copyright (c) 2012, 2013 Andrey Semashev
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/atomic/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if defined(_WIN32_WCE) || (defined(_MSC_VER) && _MSC_VER < 1400)

#include <boost/detail/interlocked.hpp>

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE(dest, exchange, compare) BOOST_INTERLOCKED_COMPARE_EXCHANGE((long*)(dest), exchange, compare)
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE(dest, newval) BOOST_INTERLOCKED_EXCHANGE((long*)(dest), newval)
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD(dest, addend) BOOST_INTERLOCKED_EXCHANGE_ADD((long*)(dest), addend)
#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE_POINTER(dest, exchange, compare) BOOST_INTERLOCKED_COMPARE_EXCHANGE_POINTER(dest, exchange, compare)
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_POINTER(dest, newval) BOOST_INTERLOCKED_EXCHANGE_POINTER(dest, newval)
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD_POINTER(dest, byte_offset) ((void*)BOOST_INTERLOCKED_EXCHANGE_ADD((long*)(dest), byte_offset))

#elif defined(_MSC_VER) && _MSC_VER >= 1400

#include <intrin.h>

#pragma intrinsic(_InterlockedCompareExchange)
#pragma intrinsic(_InterlockedExchangeAdd)
#pragma intrinsic(_InterlockedExchange)
#pragma intrinsic(_InterlockedAnd)
#pragma intrinsic(_InterlockedOr)
#pragma intrinsic(_InterlockedXor)

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE(dest, exchange, compare) _InterlockedCompareExchange((long*)(dest), (long)(exchange), (long)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD(dest, addend) _InterlockedExchangeAdd((long*)(dest), (long)(addend))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE(dest, newval) _InterlockedExchange((long*)(dest), (long)(newval))
#define BOOST_ATOMIC_INTERLOCKED_AND(dest, arg) _InterlockedAnd((long*)(dest), (long)(arg))
#define BOOST_ATOMIC_INTERLOCKED_OR(dest, arg) _InterlockedOr((long*)(dest), (long)(arg))
#define BOOST_ATOMIC_INTERLOCKED_XOR(dest, arg) _InterlockedXor((long*)(dest), (long)(arg))

#if (defined(_M_IX86) && _M_IX86 >= 500) || defined(_M_AMD64) || defined(_M_IA64)
#pragma intrinsic(_InterlockedCompareExchange64)
#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE64(dest, exchange, compare) _InterlockedCompareExchange64((__int64*)(dest), (__int64)(exchange), (__int64)(compare))
#endif

#if _MSC_VER >= 1600

// MSVC 2010 and later provide intrinsics for 8 and 16 bit integers.
// Note that for each bit count these macros must be either all defined or all not defined.
// Otherwise atomic<> operations will be implemented inconsistently.

#pragma intrinsic(_InterlockedCompareExchange8)
#pragma intrinsic(_InterlockedExchangeAdd8)
#pragma intrinsic(_InterlockedExchange8)
#pragma intrinsic(_InterlockedAnd8)
#pragma intrinsic(_InterlockedOr8)
#pragma intrinsic(_InterlockedXor8)

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE8(dest, exchange, compare) _InterlockedCompareExchange8((char*)(dest), (char)(exchange), (char)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD8(dest, addend) _InterlockedExchangeAdd8((char*)(dest), (char)(addend))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE8(dest, newval) _InterlockedExchange8((char*)(dest), (char)(newval))
#define BOOST_ATOMIC_INTERLOCKED_AND8(dest, arg) _InterlockedAnd8((char*)(dest), (char)(arg))
#define BOOST_ATOMIC_INTERLOCKED_OR8(dest, arg) _InterlockedOr8((char*)(dest), (char)(arg))
#define BOOST_ATOMIC_INTERLOCKED_XOR8(dest, arg) _InterlockedXor8((char*)(dest), (char)(arg))

#pragma intrinsic(_InterlockedCompareExchange16)
#pragma intrinsic(_InterlockedExchangeAdd16)
#pragma intrinsic(_InterlockedExchange16)
#pragma intrinsic(_InterlockedAnd16)
#pragma intrinsic(_InterlockedOr16)
#pragma intrinsic(_InterlockedXor16)

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE16(dest, exchange, compare) _InterlockedCompareExchange16((short*)(dest), (short)(exchange), (short)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD16(dest, addend) _InterlockedExchangeAdd16((short*)(dest), (short)(addend))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE16(dest, newval) _InterlockedExchange16((short*)(dest), (short)(newval))
#define BOOST_ATOMIC_INTERLOCKED_AND16(dest, arg) _InterlockedAnd16((short*)(dest), (short)(arg))
#define BOOST_ATOMIC_INTERLOCKED_OR16(dest, arg) _InterlockedOr16((short*)(dest), (short)(arg))
#define BOOST_ATOMIC_INTERLOCKED_XOR16(dest, arg) _InterlockedXor16((short*)(dest), (short)(arg))

#endif // _MSC_VER >= 1600

#if defined(_M_AMD64) || defined(_M_IA64)

#pragma intrinsic(_InterlockedExchangeAdd64)
#pragma intrinsic(_InterlockedExchange64)
#pragma intrinsic(_InterlockedAnd64)
#pragma intrinsic(_InterlockedOr64)
#pragma intrinsic(_InterlockedXor64)

#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD64(dest, addend) _InterlockedExchangeAdd64((__int64*)(dest), (__int64)(addend))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE64(dest, newval) _InterlockedExchange64((__int64*)(dest), (__int64)(newval))
#define BOOST_ATOMIC_INTERLOCKED_AND64(dest, arg) _InterlockedAnd64((__int64*)(dest), (__int64)(arg))
#define BOOST_ATOMIC_INTERLOCKED_OR64(dest, arg) _InterlockedOr64((__int64*)(dest), (__int64)(arg))
#define BOOST_ATOMIC_INTERLOCKED_XOR64(dest, arg) _InterlockedXor64((__int64*)(dest), (__int64)(arg))

#pragma intrinsic(_InterlockedCompareExchangePointer)
#pragma intrinsic(_InterlockedExchangePointer)

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE_POINTER(dest, exchange, compare) _InterlockedCompareExchangePointer((void**)(dest), (void*)(exchange), (void*)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_POINTER(dest, newval) _InterlockedExchangePointer((void**)(dest), (void*)(newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD_POINTER(dest, byte_offset) ((void*)BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD64((long*)(dest), byte_offset))

#else // defined(_M_AMD64) || defined(_M_IA64)

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE_POINTER(dest, exchange, compare) ((void*)_InterlockedCompareExchange((long*)(dest), (long)(exchange), (long)(compare)))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_POINTER(dest, newval) ((void*)_InterlockedExchange((long*)(dest), (long)(newval)))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD_POINTER(dest, byte_offset) ((void*)BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD((long*)(dest), byte_offset))

#endif // defined(_M_AMD64) || defined(_M_IA64)

#else // defined(_MSC_VER) && _MSC_VER >= 1400

#if defined(BOOST_USE_WINDOWS_H)

#include <windows.h>

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE(dest, exchange, compare) InterlockedCompareExchange((long*)(dest), (long)(exchange), (long)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE(dest, newval) InterlockedExchange((long*)(dest), (long)(newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD(dest, addend) InterlockedExchangeAdd((long*)(dest), (long)(addend))

#if defined(_WIN64)

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE64(dest, exchange, compare) InterlockedCompareExchange64((__int64*)(dest), (__int64)(exchange), (__int64)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE64(dest, newval) InterlockedExchange64((__int64*)(dest), (__int64)(newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD64(dest, addend) InterlockedExchangeAdd64((__int64*)(dest), (__int64)(addend))

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE_POINTER(dest, exchange, compare) InterlockedCompareExchangePointer((void**)(dest), (void*)(exchange), (void*)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_POINTER(dest, newval) InterlockedExchangePointer((void**)(dest), (void*)(newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD_POINTER(dest, byte_offset) ((void*)BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD64(dest, byte_offset))

#else // defined(_WIN64)

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE_POINTER(dest, exchange, compare) ((void*)BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE(dest, exchange, compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_POINTER(dest, newval) ((void*)BOOST_ATOMIC_INTERLOCKED_EXCHANGE(dest, newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD_POINTER(dest, byte_offset) ((void*)BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD(dest, byte_offset))

#endif // defined(_WIN64)

#else // defined(BOOST_USE_WINDOWS_H)

#if defined(__MINGW64__)
#define BOOST_ATOMIC_INTERLOCKED_IMPORT
#else
#define BOOST_ATOMIC_INTERLOCKED_IMPORT __declspec(dllimport)
#endif

namespace boost {
namespace atomics {
namespace detail {

extern "C" {

BOOST_ATOMIC_INTERLOCKED_IMPORT long __stdcall InterlockedCompareExchange(long volatile*, long, long);
BOOST_ATOMIC_INTERLOCKED_IMPORT long __stdcall InterlockedExchange(long volatile*, long);
BOOST_ATOMIC_INTERLOCKED_IMPORT long __stdcall InterlockedExchangeAdd(long volatile*, long);

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE(dest, exchange, compare) boost::atomics::detail::InterlockedCompareExchange((long*)(dest), (long)(exchange), (long)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE(dest, newval) boost::atomics::detail::InterlockedExchange((long*)(dest), (long)(newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD(dest, addend) boost::atomics::detail::InterlockedExchangeAdd((long*)(dest), (long)(addend))

#if defined(_WIN64)

BOOST_ATOMIC_INTERLOCKED_IMPORT __int64 __stdcall InterlockedCompareExchange64(__int64 volatile*, __int64, __int64);
BOOST_ATOMIC_INTERLOCKED_IMPORT __int64 __stdcall InterlockedExchange64(__int64 volatile*, __int64);
BOOST_ATOMIC_INTERLOCKED_IMPORT __int64 __stdcall InterlockedExchangeAdd64(__int64 volatile*, __int64);

BOOST_ATOMIC_INTERLOCKED_IMPORT void* __stdcall InterlockedCompareExchangePointer(void* volatile *, void*, void*);
BOOST_ATOMIC_INTERLOCKED_IMPORT void* __stdcall InterlockedExchangePointer(void* volatile *, void*);

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE64(dest, exchange, compare) boost::atomics::detail::InterlockedCompareExchange64((__int64*)(dest), (__int64)(exchange), (__int64)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE64(dest, newval) boost::atomics::detail::InterlockedExchange64((__int64*)(dest), (__int64)(newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD64(dest, addend) boost::atomics::detail::InterlockedExchangeAdd64((__int64*)(dest), (__int64)(addend))

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE_POINTER(dest, exchange, compare) boost::atomics::detail::InterlockedCompareExchangePointer((void**)(dest), (void*)(exchange), (void*)(compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_POINTER(dest, newval) boost::atomics::detail::InterlockedExchangePointer((void**)(dest), (void*)(newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD_POINTER(dest, byte_offset) ((void*)BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD64(dest, byte_offset))

#else // defined(_WIN64)

#define BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE_POINTER(dest, exchange, compare) ((void*)BOOST_ATOMIC_INTERLOCKED_COMPARE_EXCHANGE(dest, exchange, compare))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_POINTER(dest, newval) ((void*)BOOST_ATOMIC_INTERLOCKED_EXCHANGE(dest, newval))
#define BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD_POINTER(dest, byte_offset) ((void*)BOOST_ATOMIC_INTERLOCKED_EXCHANGE_ADD(dest, byte_offset))

#endif // defined(_WIN64)

} // extern "C"

} // namespace detail
} // namespace atomics
} // namespace boost

#undef BOOST_ATOMIC_INTERLOCKED_IMPORT

#endif // defined(BOOST_USE_WINDOWS_H)

#endif // defined(_MSC_VER)

#endif
