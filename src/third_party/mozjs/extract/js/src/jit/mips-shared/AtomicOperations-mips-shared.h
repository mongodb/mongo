/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* For documentation, see jit/AtomicOperations.h */

// NOTE, MIPS32 unlike MIPS64 doesn't provide hardware support for lock-free
// 64-bit atomics. We lie down below about 8-byte atomics being always lock-
// free in order to support wasm jit. The 64-bit atomic for MIPS32 do not use
// __atomic intrinsic and therefore do not relay on -latomic.
// Access to a aspecific 64-bit variable in memory is protected by an AddressLock
// whose instance is shared between jit and AtomicOperations.

#ifndef jit_mips_shared_AtomicOperations_mips_shared_h
#define jit_mips_shared_AtomicOperations_mips_shared_h

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

#include "builtin/AtomicsObject.h"
#include "vm/ArrayBufferObject.h"

#if !defined(__clang__) && !defined(__GNUC__)
# error "This file only for gcc-compatible compilers"
#endif

#if defined(JS_SIMULATOR_MIPS32) && !defined(__i386__)
# error "The MIPS32 simulator atomics assume x86"
#endif

namespace js { namespace jit {

#if !defined(JS_64BIT)

struct AddressLock
{
  public:
    void acquire();
    void release();
  private:
    uint32_t spinlock;
};

static_assert(sizeof(AddressLock) == sizeof(uint32_t),
              "AddressLock must be 4 bytes for it to be consumed by jit");

// For now use a single global AddressLock.
static AddressLock gAtomic64Lock;

struct MOZ_RAII AddressGuard
{
  explicit AddressGuard(void* addr)
  {
    gAtomic64Lock.acquire();
  }

  ~AddressGuard() {
    gAtomic64Lock.release();
  }
};

#endif

} }

inline bool
js::jit::AtomicOperations::hasAtomic8()
{
    return true;
}

inline bool
js::jit::AtomicOperations::isLockfree8()
{
    MOZ_ASSERT(__atomic_always_lock_free(sizeof(int8_t), 0));
    MOZ_ASSERT(__atomic_always_lock_free(sizeof(int16_t), 0));
    MOZ_ASSERT(__atomic_always_lock_free(sizeof(int32_t), 0));
# if defined(JS_64BIT)
    MOZ_ASSERT(__atomic_always_lock_free(sizeof(int64_t), 0));
# endif
    return true;
}

inline void
js::jit::AtomicOperations::fenceSeqCst()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

template<typename T>
inline T
js::jit::AtomicOperations::loadSeqCst(T* addr)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    T v;
    __atomic_load(addr, &v, __ATOMIC_SEQ_CST);
    return v;
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::loadSeqCst(int64_t* addr)
{
    AddressGuard guard(addr);
    return *addr;
}

template<>
inline uint64_t
js::jit::AtomicOperations::loadSeqCst(uint64_t* addr)
{
    AddressGuard guard(addr);
    return *addr;
}

#endif

} }

template<typename T>
inline void
js::jit::AtomicOperations::storeSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    __atomic_store(addr, &val, __ATOMIC_SEQ_CST);
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline void
js::jit::AtomicOperations::storeSeqCst(int64_t* addr, int64_t val)
{
    AddressGuard guard(addr);
    *addr = val;
}

template<>
inline void
js::jit::AtomicOperations::storeSeqCst(uint64_t* addr, uint64_t val)
{
    AddressGuard guard(addr);
    *addr = val;
}

#endif

} }

template<typename T>
inline T
js::jit::AtomicOperations::compareExchangeSeqCst(T* addr, T oldval, T newval)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    __atomic_compare_exchange(addr, &oldval, &newval, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return oldval;
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::compareExchangeSeqCst(int64_t* addr, int64_t oldval, int64_t newval)
{
    AddressGuard guard(addr);
    int64_t val = *addr;
    if (val == oldval)
        *addr = newval;
    return val;
}

template<>
inline uint64_t
js::jit::AtomicOperations::compareExchangeSeqCst(uint64_t* addr, uint64_t oldval, uint64_t newval)
{
    AddressGuard guard(addr);
    uint64_t val = *addr;
    if (val == oldval)
        *addr = newval;
    return val;
}

#endif

} }

template<typename T>
inline T
js::jit::AtomicOperations::fetchAddSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    return __atomic_fetch_add(addr, val, __ATOMIC_SEQ_CST);
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::fetchAddSeqCst(int64_t* addr, int64_t val)
{
    AddressGuard guard(addr);
    int64_t old = *addr;
    *addr = old + val;
    return old;
}

template<>
inline uint64_t
js::jit::AtomicOperations::fetchAddSeqCst(uint64_t* addr, uint64_t val)
{
    AddressGuard guard(addr);
    uint64_t old = *addr;
    *addr = old + val;
    return old;
}

#endif

} }

template<typename T>
inline T
js::jit::AtomicOperations::fetchSubSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    return __atomic_fetch_sub(addr, val, __ATOMIC_SEQ_CST);
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::fetchSubSeqCst(int64_t* addr, int64_t val)
{
    AddressGuard guard(addr);
    int64_t old = *addr;
    *addr = old - val;
    return old;
}

template<>
inline uint64_t
js::jit::AtomicOperations::fetchSubSeqCst(uint64_t* addr, uint64_t val)
{
    AddressGuard guard(addr);
    uint64_t old = *addr;
    *addr = old - val;
    return old;
}

#endif

} }

template<typename T>
inline T
js::jit::AtomicOperations::fetchAndSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    return __atomic_fetch_and(addr, val, __ATOMIC_SEQ_CST);
}


namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::fetchAndSeqCst(int64_t* addr, int64_t val)
{
    AddressGuard guard(addr);
    int64_t old = *addr;
    *addr = old & val;
    return old;
}

template<>
inline uint64_t
js::jit::AtomicOperations::fetchAndSeqCst(uint64_t* addr, uint64_t val)
{
    AddressGuard guard(addr);
    uint64_t old = *addr;
    *addr = old & val;
    return old;
}

#endif

} }

template<typename T>
inline T
js::jit::AtomicOperations::fetchOrSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    return __atomic_fetch_or(addr, val, __ATOMIC_SEQ_CST);
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::fetchOrSeqCst(int64_t* addr, int64_t val)
{
    AddressGuard guard(addr);
    int64_t old = *addr;
    *addr = old | val;
    return old;
}

template<>
inline uint64_t
js::jit::AtomicOperations::fetchOrSeqCst(uint64_t* addr, uint64_t val)
{
    AddressGuard guard(addr);
    uint64_t old = *addr;
    *addr = old | val;
    return old;
}

#endif

} }

template<typename T>
inline T
js::jit::AtomicOperations::fetchXorSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    return __atomic_fetch_xor(addr, val, __ATOMIC_SEQ_CST);

}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::fetchXorSeqCst(int64_t* addr, int64_t val)
{
    AddressGuard guard(addr);
    int64_t old = *addr;
    *addr = old ^ val;
    return old;
}

template<>
inline uint64_t
js::jit::AtomicOperations::fetchXorSeqCst(uint64_t* addr, uint64_t val)
{
    AddressGuard guard(addr);
    uint64_t old = *addr;
    *addr = old ^ val;
    return old;
}

#endif

} }

template<typename T>
inline T
js::jit::AtomicOperations::loadSafeWhenRacy(T* addr)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    T v;
    __atomic_load(addr, &v, __ATOMIC_RELAXED);
    return v;
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::loadSafeWhenRacy(int64_t* addr)
{
    return *addr;
}

template<>
inline uint64_t
js::jit::AtomicOperations::loadSafeWhenRacy(uint64_t* addr)
{
    return *addr;
}

#endif

template<>
inline uint8_clamped
js::jit::AtomicOperations::loadSafeWhenRacy(uint8_clamped* addr)
{
    uint8_t v;
    __atomic_load(&addr->val, &v, __ATOMIC_RELAXED);
    return uint8_clamped(v);
}

template<>
inline float
js::jit::AtomicOperations::loadSafeWhenRacy(float* addr)
{
    return *addr;
}

template<>
inline double
js::jit::AtomicOperations::loadSafeWhenRacy(double* addr)
{
    return *addr;
}

} }

template<typename T>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(T* addr, T val)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    __atomic_store(addr, &val, __ATOMIC_RELAXED);
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(int64_t* addr, int64_t val)
{
    *addr = val;
}

template<>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(uint64_t* addr, uint64_t val)
{
    *addr = val;
}

#endif

template<>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(uint8_clamped* addr, uint8_clamped val)
{
    __atomic_store(&addr->val, &val.val, __ATOMIC_RELAXED);
}

template<>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(float* addr, float val)
{
    *addr = val;
}

template<>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(double* addr, double val)
{
    *addr = val;
}

} }

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

template<typename T>
inline T
js::jit::AtomicOperations::exchangeSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= sizeof(void*), "atomics supported up to pointer size only");
    T v;
    __atomic_exchange(addr, &val, &v, __ATOMIC_SEQ_CST);
    return v;
}

namespace js { namespace jit {

#if !defined(JS_64BIT)

template<>
inline int64_t
js::jit::AtomicOperations::exchangeSeqCst(int64_t* addr, int64_t val)
{
    AddressGuard guard(addr);
    int64_t old = *addr;
    *addr = val;
    return old;
}

template<>
inline uint64_t
js::jit::AtomicOperations::exchangeSeqCst(uint64_t* addr, uint64_t val)
{
    AddressGuard guard(addr);
    uint64_t old = *addr;
    *addr = val;
    return old;
}

#endif

} }

#if !defined(JS_64BIT)

inline void
js::jit::AddressLock::acquire()
{
    uint32_t zero = 0;
    uint32_t one = 1;
    while (!__atomic_compare_exchange(&spinlock, &zero, &one, true, __ATOMIC_SEQ_CST,
          __ATOMIC_SEQ_CST))
    {
        zero = 0;
    }
}

inline void
js::jit::AddressLock::release()
{
    uint32_t zero = 0;
    __atomic_store(&spinlock, &zero, __ATOMIC_SEQ_CST);
}

#endif

#endif // jit_mips_shared_AtomicOperations_mips_shared_h
