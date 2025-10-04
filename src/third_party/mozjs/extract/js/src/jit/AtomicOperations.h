/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AtomicOperations_h
#define jit_AtomicOperations_h

#include "mozilla/Types.h"

#include <string.h>

#include "jit/AtomicOperationsGenerated.h"
#include "vm/SharedMem.h"

namespace js {
namespace jit {

/*
 * [SMDOC] Atomic Operations
 *
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
 * functions that use inline assembly.  See GenerateAtomicOperations.py.
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
class AtomicOperations {
  // The following functions are defined for T = int8_t, uint8_t,
  // int16_t, uint16_t, int32_t, uint32_t, int64_t, and uint64_t.

  // Atomically read *addr.
  template <typename T>
  static inline T loadSeqCst(T* addr);

  // Atomically store val in *addr.
  template <typename T>
  static inline void storeSeqCst(T* addr, T val);

  // Atomically store val in *addr and return the old value of *addr.
  template <typename T>
  static inline T exchangeSeqCst(T* addr, T val);

  // Atomically check that *addr contains oldval and if so replace it
  // with newval, in any case returning the old contents of *addr.
  template <typename T>
  static inline T compareExchangeSeqCst(T* addr, T oldval, T newval);

  // Atomically add, subtract, bitwise-AND, bitwise-OR, or bitwise-XOR
  // val into *addr and return the old value of *addr.
  template <typename T>
  static inline T fetchAddSeqCst(T* addr, T val);

  template <typename T>
  static inline T fetchSubSeqCst(T* addr, T val);

  template <typename T>
  static inline T fetchAndSeqCst(T* addr, T val);

  template <typename T>
  static inline T fetchOrSeqCst(T* addr, T val);

  template <typename T>
  static inline T fetchXorSeqCst(T* addr, T val);

  // The SafeWhenRacy functions are to be used when C++ code has to access
  // memory without synchronization and can't guarantee that there won't be a
  // race on the access.  But they are access-atomic for integer data so long
  // as any racing writes are of the same size and to the same address.

  // Defined for all the integral types as well as for float32 and float64,
  // but not access-atomic for floats, nor for int64 and uint64 on 32-bit
  // platforms.
  template <typename T>
  static inline T loadSafeWhenRacy(T* addr);

  // Defined for all the integral types as well as for float32 and float64,
  // but not access-atomic for floats, nor for int64 and uint64 on 32-bit
  // platforms.
  template <typename T>
  static inline void storeSafeWhenRacy(T* addr, T val);

  // Replacement for memcpy().  No access-atomicity guarantees.
  static inline void memcpySafeWhenRacy(void* dest, const void* src,
                                        size_t nbytes);

  // Replacement for memmove().  No access-atomicity guarantees.
  static inline void memmoveSafeWhenRacy(void* dest, const void* src,
                                         size_t nbytes);

 public:
  // Test lock-freedom for any int32 value.  This implements the
  // Atomics::isLockFree() operation in the ECMAScript Shared Memory and
  // Atomics specification, as follows:
  //
  // 4-byte accesses are always lock free (in the spec).
  // 1-, 2-, and 8-byte accesses are always lock free (in SpiderMonkey).
  //
  // There is no lock-freedom for JS for any other values on any platform.
  static constexpr inline bool isLockfreeJS(int32_t n);

  // If the return value is true then the templated functions below are
  // supported for int64_t and uint64_t.  If the return value is false then
  // those functions will MOZ_CRASH.  The value of this call does not change
  // during execution.
  static inline bool hasAtomic8();

  // If the return value is true then hasAtomic8() is true and the atomic
  // operations are indeed lock-free.  The value of this call does not change
  // during execution.
  static inline bool isLockfree8();

  // Execute a full memory barrier (LoadLoad+LoadStore+StoreLoad+StoreStore).
  static inline void fenceSeqCst();

  // All clients should use the APIs that take SharedMem pointers.
  // See above for semantics and acceptable types.

  template <typename T>
  static T loadSeqCst(SharedMem<T*> addr) {
    return loadSeqCst(addr.unwrap());
  }

  template <typename T>
  static void storeSeqCst(SharedMem<T*> addr, T val) {
    return storeSeqCst(addr.unwrap(), val);
  }

  template <typename T>
  static T exchangeSeqCst(SharedMem<T*> addr, T val) {
    return exchangeSeqCst(addr.unwrap(), val);
  }

  template <typename T>
  static T compareExchangeSeqCst(SharedMem<T*> addr, T oldval, T newval) {
    return compareExchangeSeqCst(addr.unwrap(), oldval, newval);
  }

  template <typename T>
  static T fetchAddSeqCst(SharedMem<T*> addr, T val) {
    return fetchAddSeqCst(addr.unwrap(), val);
  }

  template <typename T>
  static T fetchSubSeqCst(SharedMem<T*> addr, T val) {
    return fetchSubSeqCst(addr.unwrap(), val);
  }

  template <typename T>
  static T fetchAndSeqCst(SharedMem<T*> addr, T val) {
    return fetchAndSeqCst(addr.unwrap(), val);
  }

  template <typename T>
  static T fetchOrSeqCst(SharedMem<T*> addr, T val) {
    return fetchOrSeqCst(addr.unwrap(), val);
  }

  template <typename T>
  static T fetchXorSeqCst(SharedMem<T*> addr, T val) {
    return fetchXorSeqCst(addr.unwrap(), val);
  }

  template <typename T>
  static T loadSafeWhenRacy(SharedMem<T*> addr) {
    return loadSafeWhenRacy(addr.unwrap());
  }

  template <typename T>
  static void storeSafeWhenRacy(SharedMem<T*> addr, T val) {
    return storeSafeWhenRacy(addr.unwrap(), val);
  }

  template <typename T>
  static void memcpySafeWhenRacy(SharedMem<T*> dest, SharedMem<T*> src,
                                 size_t nbytes) {
    memcpySafeWhenRacy(dest.template cast<void*>().unwrap(),
                       src.template cast<void*>().unwrap(), nbytes);
  }

  template <typename T>
  static void memcpySafeWhenRacy(SharedMem<T*> dest, T* src, size_t nbytes) {
    memcpySafeWhenRacy(dest.template cast<void*>().unwrap(),
                       static_cast<void*>(src), nbytes);
  }

  template <typename T>
  static void memcpySafeWhenRacy(T* dest, SharedMem<T*> src, size_t nbytes) {
    memcpySafeWhenRacy(static_cast<void*>(dest),
                       src.template cast<void*>().unwrap(), nbytes);
  }

  template <typename T>
  static void memmoveSafeWhenRacy(SharedMem<T*> dest, SharedMem<T*> src,
                                  size_t nbytes) {
    memmoveSafeWhenRacy(dest.template cast<void*>().unwrap(),
                        src.template cast<void*>().unwrap(), nbytes);
  }

  static void memsetSafeWhenRacy(SharedMem<uint8_t*> dest, int value,
                                 size_t nbytes) {
    uint8_t buf[1024];
    size_t iterations = nbytes / sizeof(buf);
    size_t tail = nbytes % sizeof(buf);
    size_t offs = 0;
    if (iterations > 0) {
      memset(buf, value, sizeof(buf));
      while (iterations--) {
        memcpySafeWhenRacy(dest + offs, SharedMem<uint8_t*>::unshared(buf),
                           sizeof(buf));
        offs += sizeof(buf);
      }
    } else {
      memset(buf, value, tail);
    }
    memcpySafeWhenRacy(dest + offs, SharedMem<uint8_t*>::unshared(buf), tail);
  }

  template <typename T>
  static void podCopySafeWhenRacy(SharedMem<T*> dest, SharedMem<T*> src,
                                  size_t nelem) {
    memcpySafeWhenRacy(dest, src, nelem * sizeof(T));
  }

  template <typename T>
  static void podMoveSafeWhenRacy(SharedMem<T*> dest, SharedMem<T*> src,
                                  size_t nelem) {
    memmoveSafeWhenRacy(dest, src, nelem * sizeof(T));
  }
};

constexpr inline bool AtomicOperations::isLockfreeJS(int32_t size) {
  // Keep this in sync with atomicIsLockFreeJS() in jit/MacroAssembler.cpp.

  switch (size) {
    case 1:
      return true;
    case 2:
      return true;
    case 4:
      // The spec requires Atomics.isLockFree(4) to return true.
      return true;
    case 8:
      return true;
    default:
      return false;
  }
}

}  // namespace jit
}  // namespace js

// As explained above, our atomic operations are not portable even in principle,
// so we must include platform+compiler specific definitions here.
//
// x86, x64, arm, and arm64 are maintained by Mozilla.  All other platform
// setups are by platform maintainers' request and are not maintained by
// Mozilla.
//
// If you are using a platform+compiler combination that causes an error below
// (and if the problem isn't just that the compiler uses a different name for a
// known architecture), you have basically three options:
//
//  - find an already-supported compiler for the platform and use that instead
//
//  - write your own support code for the platform+compiler and create a new
//    case below
//
//  - include jit/shared/AtomicOperations-feeling-lucky.h in a case for the
//    platform below, if you have a gcc-compatible compiler and truly feel
//    lucky.  You may have to add a little code to that file, too.
//
// Simulators are confusing.  These atomic primitives must be compatible with
// the code that the JIT emits, but of course for an ARM simulator running on
// x86 the primitives here will be for x86, not for ARM, while the JIT emits ARM
// code.  Our ARM simulator solves that the easy way: by using these primitives
// to implement its atomic operations.  For other simulators there may need to
// be special cases below to provide simulator-compatible primitives, for
// example, for our ARM64 simulator the primitives could in principle
// participate in the memory exclusivity monitors implemented by the simulator.
// Such a solution is likely to be difficult.

#ifdef JS_HAVE_GENERATED_ATOMIC_OPS
#  include "jit/shared/AtomicOperations-shared-jit.h"
#elif defined(JS_SIMULATOR_MIPS32) || defined(__mips__)
#  include "jit/mips-shared/AtomicOperations-mips-shared.h"
#else
#  include "jit/shared/AtomicOperations-feeling-lucky.h"
#endif

#endif  // jit_AtomicOperations_h
