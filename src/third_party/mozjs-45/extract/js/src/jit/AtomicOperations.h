/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AtomicOperations_h
#define jit_AtomicOperations_h

#include "mozilla/Types.h"

#include "vm/SharedMem.h"

namespace js {
namespace jit {

class RegionLock;

/*
 * The atomic operations layer defines types and functions for
 * JIT-compatible atomic operation.
 *
 * The fundamental constraints on the functions are:
 *
 * - That their realization here MUST be compatible with code the JIT
 *   generates for its Atomics operations, so that an atomic access
 *   from the interpreter or runtime - from any C++ code - really is
 *   atomic relative to a concurrent, compatible atomic access from
 *   jitted code.  That is, these primitives expose JIT-compatible
 *   atomicity functionality to C++.
 *
 * - That accesses may race without creating C++ undefined behavior:
 *   atomic accesses (marked "SeqCst") may race with non-atomic
 *   accesses (marked "SafeWhenRacy"); overlapping but non-matching,
 *   and hence incompatible, atomic accesses may race; and non-atomic
 *   accesses may race.  The effects of races need not be predictable,
 *   so garbage can be produced by a read or written by a write, but
 *   the effects must be benign: the program must continue to run, and
 *   only the memory in the union of addresses named in the racing
 *   accesses may be affected.
 *
 * The compatibility constraint means that if the JIT makes dynamic
 * decisions about how to implement atomic operations then
 * corresponding dynamic decisions MUST be made in the implementations
 * of the functions below.
 *
 * The safe-for-races constraint means that by and large, it is hard
 * to implement these primitives in C++.  See "Implementation notes"
 * below.
 *
 * The "SeqCst" suffix on operations means "sequentially consistent"
 * and means such a function's operation must have "sequentially
 * consistent" memory ordering.  See mfbt/Atomics.h for an explanation
 * of this memory ordering.
 *
 * Note that a "SafeWhenRacy" access does not provide the atomicity of
 * a "relaxed atomic" access: it can read or write garbage if there's
 * a race.
 *
 *
 * Implementation notes.
 *
 * It's not a requirement that these functions be inlined; performance
 * is not a great concern.  On some platforms these functions may call
 * out to code that's generated at run time.
 *
 * In principle these functions will not be written in C++, thus
 * making races defined behavior if all racy accesses from C++ go via
 * these functions.  (Jitted code will always be safe for races and
 * provides the same guarantees as these functions.)
 *
 * The appropriate implementations will be platform-specific and
 * there are some obvious implementation strategies to choose
 * from, sometimes a combination is appropriate:
 *
 *  - generating the code at run-time with the JIT;
 *  - hand-written assembler (maybe inline); or
 *  - using special compiler intrinsics or directives.
 *
 * Trusting the compiler not to generate code that blows up on a
 * race definitely won't work in the presence of TSan, or even of
 * optimizing compilers in seemingly-"innocuous" conditions.  (See
 * https://www.usenix.org/legacy/event/hotpar11/tech/final_files/Boehm.pdf
 * for details.)
 */
class AtomicOperations
{
    friend class RegionLock;

  private:
    // The following functions are defined for T = int8_t, uint8_t,
    // int16_t, uint16_t, int32_t, uint32_t, int64_t, and uint64_t.

    // Atomically read *addr.
    template<typename T>
    static inline T loadSeqCst(T* addr);

    // Atomically store val in *addr.
    template<typename T>
    static inline void storeSeqCst(T* addr, T val);

    // Atomically store val in *addr and return the old value of *addr.
    template<typename T>
    static inline T exchangeSeqCst(T* addr, T val);

    // Atomically check that *addr contains oldval and if so replace it
    // with newval, in any case returning the old contents of *addr.
    template<typename T>
    static inline T compareExchangeSeqCst(T* addr, T oldval, T newval);

    // The following functions are defined for T = int8_t, uint8_t,
    // int16_t, uint16_t, int32_t, uint32_t only.

    // Atomically add, subtract, bitwise-AND, bitwise-OR, or bitwise-XOR
    // val into *addr and return the old value of *addr.
    template<typename T>
    static inline T fetchAddSeqCst(T* addr, T val);

    template<typename T>
    static inline T fetchSubSeqCst(T* addr, T val);

    template<typename T>
    static inline T fetchAndSeqCst(T* addr, T val);

    template<typename T>
    static inline T fetchOrSeqCst(T* addr, T val);

    template<typename T>
    static inline T fetchXorSeqCst(T* addr, T val);

    // The SafeWhenRacy functions are to be used when C++ code has to access
    // memory without synchronization and can't guarantee that there
    // won't be a race on the access.

    // Defined for all the integral types as well as for float32 and float64.
    template<typename T>
    static inline T loadSafeWhenRacy(T* addr);

    // Defined for all the integral types as well as for float32 and float64.
    template<typename T>
    static inline void storeSafeWhenRacy(T* addr, T val);

    // Replacement for memcpy().
    static inline void memcpySafeWhenRacy(void* dest, const void* src, size_t nbytes);

    // Replacement for memmove().
    static inline void memmoveSafeWhenRacy(void* dest, const void* src, size_t nbytes);

  public:
    // Test lock-freedom for any integer value.
    //
    // This implements a platform-independent pattern, as follows:
    //
    // 1, 2, and 4 bytes are always lock free, lock-freedom for 8
    // bytes is determined by the platform's isLockfree8(), and there
    // is no lock-freedom for any other values on any platform.
    static inline bool isLockfree(int32_t n);

    // If the return value is true then a call to the 64-bit (8-byte)
    // routines below will work, otherwise those functions will assert in
    // debug builds and may crash in release build.  (See the code in
    // ../arm for an example.)  The value of this call does not change
    // during execution.
    static inline bool isLockfree8();

    // Execute a full memory barrier (LoadLoad+LoadStore+StoreLoad+StoreStore).
    static inline void fenceSeqCst();

    // All clients should use the APIs that take SharedMem pointers.
    // See above for semantics and acceptable types.

    template<typename T>
    static T loadSeqCst(SharedMem<T*> addr) {
        return loadSeqCst(addr.unwrap());
    }

    template<typename T>
    static void storeSeqCst(SharedMem<T*> addr, T val) {
        return storeSeqCst(addr.unwrap(), val);
    }

    template<typename T>
    static T exchangeSeqCst(SharedMem<T*> addr, T val) {
        return exchangeSeqCst(addr.unwrap(), val);
    }

    template<typename T>
    static T compareExchangeSeqCst(SharedMem<T*> addr, T oldval, T newval) {
        return compareExchangeSeqCst(addr.unwrap(), oldval, newval);
    }

    template<typename T>
    static T fetchAddSeqCst(SharedMem<T*> addr, T val) {
        return fetchAddSeqCst(addr.unwrap(), val);
    }

    template<typename T>
    static T fetchSubSeqCst(SharedMem<T*> addr, T val) {
        return fetchSubSeqCst(addr.unwrap(), val);
    }

    template<typename T>
    static T fetchAndSeqCst(SharedMem<T*> addr, T val) {
        return fetchAndSeqCst(addr.unwrap(), val);
    }

    template<typename T>
    static T fetchOrSeqCst(SharedMem<T*> addr, T val) {
        return fetchOrSeqCst(addr.unwrap(), val);
    }

    template<typename T>
    static T fetchXorSeqCst(SharedMem<T*> addr, T val) {
        return fetchXorSeqCst(addr.unwrap(), val);
    }

    template<typename T>
    static T loadSafeWhenRacy(SharedMem<T*> addr) {
        return loadSafeWhenRacy(addr.unwrap());
    }

    template<typename T>
    static void storeSafeWhenRacy(SharedMem<T*> addr, T val) {
        return storeSafeWhenRacy(addr.unwrap(), val);
    }

    template<typename T>
    static void memcpySafeWhenRacy(SharedMem<T> dest, SharedMem<T> src, size_t nbytes) {
        memcpySafeWhenRacy(static_cast<void*>(dest.unwrap()), static_cast<void*>(src.unwrap()), nbytes);
    }

    template<typename T>
    static void memcpySafeWhenRacy(SharedMem<T> dest, T src, size_t nbytes) {
        memcpySafeWhenRacy(static_cast<void*>(dest.unwrap()), static_cast<void*>(src), nbytes);
    }

    template<typename T>
    static void memcpySafeWhenRacy(T dest, SharedMem<T> src, size_t nbytes) {
        memcpySafeWhenRacy(static_cast<void*>(dest), static_cast<void*>(src.unwrap()), nbytes);
    }

    template<typename T>
    static void memmoveSafeWhenRacy(SharedMem<T> dest, SharedMem<T> src, size_t nbytes) {
        memmoveSafeWhenRacy(static_cast<void*>(dest.unwrap()), static_cast<void*>(src.unwrap()), nbytes);
    }
};

/* A data type representing a lock on some region of a
 * SharedArrayRawBuffer's memory, to be used only when the hardware
 * does not provide necessary atomicity (eg, float64 access on ARMv6
 * and some ARMv7 systems).
 */
class RegionLock
{
  public:
    RegionLock() : spinlock(0) {}

    /* Addr is the address to be locked, nbytes the number of bytes we
     * need to lock.  The lock that is taken may cover a larger range
     * of bytes.
     */
    template<size_t nbytes>
    void acquire(void* addr);

    /* Addr is the address to be unlocked, nbytes the number of bytes
     * we need to unlock.  The lock must be held by the calling thread,
     * at the given address and for the number of bytes.
     */
    template<size_t nbytes>
    void release(void* addr);

  private:
    /* For now, a simple spinlock that covers the entire buffer. */
    uint32_t spinlock;
};

inline bool
AtomicOperations::isLockfree(int32_t size)
{
    // Keep this in sync with visitAtomicIsLockFree() in jit/CodeGenerator.cpp.

    switch (size) {
      case 1:
      case 2:
      case 4:
        return true;
      case 8:
        return AtomicOperations::isLockfree8();
      default:
        return false;
    }
}

} // namespace jit
} // namespace js

#if defined(JS_CODEGEN_ARM)
# include "jit/arm/AtomicOperations-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/AtomicOperations-arm64.h"
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
# include "jit/mips-shared/AtomicOperations-mips-shared.h"
#elif defined(__ppc64__) || defined(__PPC64_)       \
    || defined(__ppc64le__) || defined(__PPC64LE__) \
    || defined(__ppc__) || defined(__PPC__)
# include "jit/none/AtomicOperations-ppc.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/AtomicOperations-none.h"
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
# include "jit/x86-shared/AtomicOperations-x86-shared.h"
#else
# error "Atomic operations must be defined for this platform"
#endif

#endif // jit_AtomicOperations_h
