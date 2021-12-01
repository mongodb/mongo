/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_AtomicOperations_x86_shared_msvc_h
#define jit_shared_AtomicOperations_x86_shared_msvc_h

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

#if !defined(_MSC_VER)
# error "This file only for Microsoft Visual C++"
#endif

// For overall documentation, see jit/AtomicOperations.h/
//
// For general comments on lock-freedom, access-atomicity, and related matters
// on x86 and x64, notably for justification of the implementations of the
// 64-bit primitives on 32-bit systems, see the comment block in
// AtomicOperations-x86-shared-gcc.h.

// Below, _ReadWriteBarrier is a compiler directive, preventing reordering of
// instructions and reuse of memory values across it in the compiler, but having
// no impact on what the CPU does.

// Note, here we use MSVC intrinsics directly.  But MSVC supports a slightly
// higher level of function which uses the intrinsic when possible (8, 16, and
// 32-bit operations, and 64-bit operations on 64-bit systems) and otherwise
// falls back on CMPXCHG8B for 64-bit operations on 32-bit systems.  We could be
// using those functions in many cases here (though not all).  I have not done
// so because (a) I don't yet know how far back those functions are supported
// and (b) I expect we'll end up dropping into assembler here eventually so as
// to guarantee that the C++ compiler won't optimize the code.

// Note, _InterlockedCompareExchange takes the *new* value as the second argument
// and the *comparand* (expected old value) as the third argument.

inline bool
js::jit::AtomicOperations::hasAtomic8()
{
    return true;
}

inline bool
js::jit::AtomicOperations::isLockfree8()
{
    // The MSDN docs suggest very strongly that if code is compiled for Pentium
    // or better the 64-bit primitives will be lock-free, see eg the "Remarks"
    // secion of the page for _InterlockedCompareExchange64, currently here:
    // https://msdn.microsoft.com/en-us/library/ttk2z1ws%28v=vs.85%29.aspx
    //
    // But I've found no way to assert that at compile time or run time, there
    // appears to be no WinAPI is_lock_free() test.

    return true;
}

inline void
js::jit::AtomicOperations::fenceSeqCst()
{
    _ReadWriteBarrier();
    _mm_mfence();
}

template<typename T>
inline T
js::jit::AtomicOperations::loadSeqCst(T* addr)
{
    MOZ_ASSERT(tier1Constraints(addr));
    _ReadWriteBarrier();
    T v = *addr;
    _ReadWriteBarrier();
    return v;
}

#ifdef _M_IX86
namespace js { namespace jit {

# define MSC_LOADOP(T)                      \
    template<>                              \
    inline T                                \
    AtomicOperations::loadSeqCst(T* addr) { \
        MOZ_ASSERT(tier1Constraints(addr)); \
        _ReadWriteBarrier();                \
        return (T)_InterlockedCompareExchange64((__int64 volatile*)addr, 0, 0); \
    }

MSC_LOADOP(int64_t)
MSC_LOADOP(uint64_t)

# undef MSC_LOADOP

} }
#endif // _M_IX86

template<typename T>
inline void
js::jit::AtomicOperations::storeSeqCst(T* addr, T val)
{
    MOZ_ASSERT(tier1Constraints(addr));
    _ReadWriteBarrier();
    *addr = val;
    fenceSeqCst();
}

#ifdef _M_IX86
namespace js { namespace jit {

# define MSC_STOREOP(T)                              \
    template<>                                      \
    inline void                                     \
    AtomicOperations::storeSeqCst(T* addr, T val) { \
        MOZ_ASSERT(tier1Constraints(addr));         \
        _ReadWriteBarrier();                        \
        T oldval = *addr;                           \
        for (;;) {                                  \
            T nextval = (T)_InterlockedCompareExchange64((__int64 volatile*)addr, \
                                                         (__int64)val,            \
                                                         (__int64)oldval);        \
            if (nextval == oldval)                  \
                break;                              \
            oldval = nextval;                       \
        }                                           \
        _ReadWriteBarrier();                        \
    }

MSC_STOREOP(int64_t)
MSC_STOREOP(uint64_t)

# undef MSC_STOREOP

} }
#endif // _M_IX86

#define MSC_EXCHANGEOP(T, U, xchgop)                            \
    template<> inline T                                         \
    AtomicOperations::exchangeSeqCst(T* addr, T val) {          \
        MOZ_ASSERT(tier1Constraints(addr));                     \
        return (T)xchgop((U volatile*)addr, (U)val);            \
    }

#ifdef _M_IX86
# define MSC_EXCHANGEOP_CAS(T)                                       \
    template<> inline T                                              \
    AtomicOperations::exchangeSeqCst(T* addr, T val) {               \
        MOZ_ASSERT(tier1Constraints(addr));                          \
        _ReadWriteBarrier();                                         \
        T oldval = *addr;                                            \
        for (;;) {                                                   \
            T nextval = (T)_InterlockedCompareExchange64((__int64 volatile*)addr, \
                                                         (__int64)val,            \
                                                         (__int64)oldval);        \
            if (nextval == oldval)                                   \
                break;                                               \
            oldval = nextval;                                        \
        }                                                            \
        _ReadWriteBarrier();                                         \
        return oldval;                                               \
    }
#endif // _M_IX86

namespace js { namespace jit {

MSC_EXCHANGEOP(int8_t, char, _InterlockedExchange8)
MSC_EXCHANGEOP(uint8_t, char, _InterlockedExchange8)
MSC_EXCHANGEOP(int16_t, short, _InterlockedExchange16)
MSC_EXCHANGEOP(uint16_t, short, _InterlockedExchange16)
MSC_EXCHANGEOP(int32_t, long, _InterlockedExchange)
MSC_EXCHANGEOP(uint32_t, long, _InterlockedExchange)

#ifdef _M_IX86
MSC_EXCHANGEOP_CAS(int64_t)
MSC_EXCHANGEOP_CAS(uint64_t)
#else
MSC_EXCHANGEOP(int64_t, __int64, _InterlockedExchange64)
MSC_EXCHANGEOP(uint64_t, __int64, _InterlockedExchange64)
#endif

} }

#undef MSC_EXCHANGEOP
#undef MSC_EXCHANGEOP_CAS

#define MSC_CAS(T, U, cmpxchg)                                          \
    template<> inline T                                                 \
    AtomicOperations::compareExchangeSeqCst(T* addr, T oldval, T newval) { \
        MOZ_ASSERT(tier1Constraints(addr));                             \
        return (T)cmpxchg((U volatile*)addr, (U)newval, (U)oldval);     \
    }

namespace js { namespace jit {

MSC_CAS(int8_t, char, _InterlockedCompareExchange8)
MSC_CAS(uint8_t, char, _InterlockedCompareExchange8)
MSC_CAS(int16_t, short, _InterlockedCompareExchange16)
MSC_CAS(uint16_t, short, _InterlockedCompareExchange16)
MSC_CAS(int32_t, long, _InterlockedCompareExchange)
MSC_CAS(uint32_t, long, _InterlockedCompareExchange)
MSC_CAS(int64_t, __int64, _InterlockedCompareExchange64)
MSC_CAS(uint64_t, __int64, _InterlockedCompareExchange64)

} }

#undef MSC_CAS

#define MSC_FETCHADDOP(T, U, xadd)                                   \
    template<> inline T                                              \
    AtomicOperations::fetchAddSeqCst(T* addr, T val) {               \
        MOZ_ASSERT(tier1Constraints(addr));                          \
        return (T)xadd((U volatile*)addr, (U)val);                   \
    }                                                                \

#define MSC_FETCHSUBOP(T)                                            \
    template<> inline T                                              \
    AtomicOperations::fetchSubSeqCst(T* addr, T val) {               \
        return fetchAddSeqCst(addr, (T)(0-val));                     \
    }

#ifdef _M_IX86
# define MSC_FETCHADDOP_CAS(T)                                       \
    template<> inline T                                              \
    AtomicOperations::fetchAddSeqCst(T* addr, T val) {               \
        MOZ_ASSERT(tier1Constraints(addr));                          \
        _ReadWriteBarrier();                                         \
        T oldval = *addr;                                            \
        for (;;) {                                                   \
            T nextval = (T)_InterlockedCompareExchange64((__int64 volatile*)addr, \
                                                         (__int64)(oldval + val), \
                                                         (__int64)oldval);        \
            if (nextval == oldval)                                   \
                break;                                               \
            oldval = nextval;                                        \
        }                                                            \
        _ReadWriteBarrier();                                         \
        return oldval;                                               \
    }
#endif // _M_IX86

namespace js { namespace jit {

MSC_FETCHADDOP(int8_t, char, _InterlockedExchangeAdd8)
MSC_FETCHADDOP(uint8_t, char, _InterlockedExchangeAdd8)
MSC_FETCHADDOP(int16_t, short, _InterlockedExchangeAdd16)
MSC_FETCHADDOP(uint16_t, short, _InterlockedExchangeAdd16)
MSC_FETCHADDOP(int32_t, long, _InterlockedExchangeAdd)
MSC_FETCHADDOP(uint32_t, long, _InterlockedExchangeAdd)

#ifdef _M_IX86
MSC_FETCHADDOP_CAS(int64_t)
MSC_FETCHADDOP_CAS(uint64_t)
#else
MSC_FETCHADDOP(int64_t, __int64, _InterlockedExchangeAdd64)
MSC_FETCHADDOP(uint64_t, __int64, _InterlockedExchangeAdd64)
#endif

MSC_FETCHSUBOP(int8_t)
MSC_FETCHSUBOP(uint8_t)
MSC_FETCHSUBOP(int16_t)
MSC_FETCHSUBOP(uint16_t)
MSC_FETCHSUBOP(int32_t)
MSC_FETCHSUBOP(uint32_t)
MSC_FETCHSUBOP(int64_t)
MSC_FETCHSUBOP(uint64_t)

} }

#undef MSC_FETCHADDOP
#undef MSC_FETCHADDOP_CAS
#undef MSC_FETCHSUBOP

#define MSC_FETCHBITOPX(T, U, name, op)                                 \
    template<> inline T                                                 \
    AtomicOperations::name(T* addr, T val) {                            \
        MOZ_ASSERT(tier1Constraints(addr));                             \
        return (T)op((U volatile*)addr, (U)val);                        \
    }

#define MSC_FETCHBITOP(T, U, andop, orop, xorop)                        \
    MSC_FETCHBITOPX(T, U, fetchAndSeqCst, andop)                        \
    MSC_FETCHBITOPX(T, U, fetchOrSeqCst, orop)                          \
    MSC_FETCHBITOPX(T, U, fetchXorSeqCst, xorop)

#ifdef _M_IX86
# define AND_OP &
# define OR_OP |
# define XOR_OP ^
# define MSC_FETCHBITOPX_CAS(T, name, OP)                            \
    template<> inline T                                              \
    AtomicOperations::name(T* addr, T val) {                         \
        MOZ_ASSERT(tier1Constraints(addr));                          \
        _ReadWriteBarrier();                                         \
        T oldval = *addr;                                            \
        for (;;) {                                                   \
            T nextval = (T)_InterlockedCompareExchange64((__int64 volatile*)addr,  \
                                                         (__int64)(oldval OP val), \
                                                         (__int64)oldval);         \
            if (nextval == oldval)                                   \
                break;                                               \
            oldval = nextval;                                        \
        }                                                            \
        _ReadWriteBarrier();                                         \
        return oldval;                                               \
    }

#define MSC_FETCHBITOP_CAS(T)                                        \
    MSC_FETCHBITOPX_CAS(T, fetchAndSeqCst, AND_OP)                   \
    MSC_FETCHBITOPX_CAS(T, fetchOrSeqCst, OR_OP)                     \
    MSC_FETCHBITOPX_CAS(T, fetchXorSeqCst, XOR_OP)

#endif

namespace js { namespace jit {

MSC_FETCHBITOP(int8_t, char, _InterlockedAnd8, _InterlockedOr8, _InterlockedXor8)
MSC_FETCHBITOP(uint8_t, char, _InterlockedAnd8, _InterlockedOr8, _InterlockedXor8)
MSC_FETCHBITOP(int16_t, short, _InterlockedAnd16, _InterlockedOr16, _InterlockedXor16)
MSC_FETCHBITOP(uint16_t, short, _InterlockedAnd16, _InterlockedOr16, _InterlockedXor16)
MSC_FETCHBITOP(int32_t, long,  _InterlockedAnd, _InterlockedOr, _InterlockedXor)
MSC_FETCHBITOP(uint32_t, long, _InterlockedAnd, _InterlockedOr, _InterlockedXor)

#ifdef _M_IX86
MSC_FETCHBITOP_CAS(int64_t)
MSC_FETCHBITOP_CAS(uint64_t)
#else
MSC_FETCHBITOP(int64_t, __int64,  _InterlockedAnd64, _InterlockedOr64, _InterlockedXor64)
MSC_FETCHBITOP(uint64_t, __int64, _InterlockedAnd64, _InterlockedOr64, _InterlockedXor64)
#endif

} }

#undef MSC_FETCHBITOPX_CAS
#undef MSC_FETCHBITOPX
#undef MSC_FETCHBITOP_CAS
#undef MSC_FETCHBITOP

template<typename T>
inline T
js::jit::AtomicOperations::loadSafeWhenRacy(T* addr)
{
    MOZ_ASSERT(tier1Constraints(addr));
    // This is also appropriate for double, int64, and uint64 on 32-bit
    // platforms since there are no guarantees of access-atomicity.
    return *addr;
}

template<typename T>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(T* addr, T val)
{
    MOZ_ASSERT(tier1Constraints(addr));
    // This is also appropriate for double, int64, and uint64 on 32-bit
    // platforms since there are no guarantees of access-atomicity.
    *addr = val;
}

inline void
js::jit::AtomicOperations::memcpySafeWhenRacy(void* dest, const void* src, size_t nbytes)
{
    MOZ_ASSERT(!((char*)dest <= (char*)src && (char*)src < (char*)dest+nbytes));
    MOZ_ASSERT(!((char*)src <= (char*)dest && (char*)dest < (char*)src+nbytes));
    ::memcpy(dest, src, nbytes);
}

inline void
js::jit::AtomicOperations::memmoveSafeWhenRacy(void* dest, const void* src, size_t nbytes)
{
    ::memmove(dest, src, nbytes);
}

#endif // jit_shared_AtomicOperations_x86_shared_msvc_h
