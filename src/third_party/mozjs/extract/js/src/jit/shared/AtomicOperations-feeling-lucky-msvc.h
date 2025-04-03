/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* For documentation, see jit/AtomicOperations.h, both the comment block at the
 * beginning and the #ifdef nest near the end.
 *
 * This adaptation of AtomicOperations-feeling-lucky-gcc.h uses MSVC intrinsics
 * and compiles with MSVC targeting x86-64. Due care was taken to match the
 * intended semantics, but these functions ARE NOT SUFFICIENTLY TESTED.
 *
 * MongoDB users: Use of atomic operations from JavaScript functions may work
 * but is not supported and will not be useful.
 *
 * Others: Any other project that imports this header (within the license terms)
 * should extensively test these functions to ensure they are fit for the
 * desired purpose.
 */

#ifndef jit_shared_AtomicOperations_feeling_lucky_msvc_h
#define jit_shared_AtomicOperations_feeling_lucky_msvc_h

#include <windef.h>

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

#ifndef _MSC_VER
#  error "Attempt to compile MSVC-specific header from another compiler"
#endif

#ifndef JS_64BIT
#  error "Attempt to compile a 64-bit header for a non-64-bit target"
#endif

#if defined(HAS_64BIT_LOCKFREE) && !defined(HAS_64BIT_ATOMICS)
#  error "Attempt to compile atomics with an invalid combination of features"
#endif

inline bool js::jit::AtomicOperations::hasAtomic8() { return true; }

inline bool js::jit::AtomicOperations::isLockfree8() { return true; }

inline void js::jit::AtomicOperations::fenceSeqCst() {
  // Interlocked functions serve as a memory barrier and generate a fence
  // instruction on x86-64.
  static volatile LONG barrier = 0;
  (void*)InterlockedExchange(&barrier, 0);
}

template <typename T>
inline T js::jit::AtomicOperations::loadSeqCst(T* addr) {
  // Aligned reads of up to 8 bytes are guaranteed atomic on x86-64.
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
  MOZ_ASSERT(reinterpret_cast<uintptr_t>(addr) % sizeof(*addr) == 0);

  _ReadWriteBarrier();
  T v = *addr;
  _ReadWriteBarrier();

  return v;
}

template <typename T>
inline void js::jit::AtomicOperations::storeSeqCst(T* addr, T val) {
  // Aligned writes of up to 8 bytes are guaranteed atomic on x86-64.
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
  MOZ_ASSERT(reinterpret_cast<uintptr_t>(addr) % sizeof(*addr) == 0);

  _ReadWriteBarrier();
  *addr = val;
  _ReadWriteBarrier();
}

namespace detail {
template <class T>
struct InterlockedIntrinsics;

template <class T>
requires(sizeof(T) == 1) struct InterlockedIntrinsics<T> {
  using ApiType = char;

  static ApiType exchange(volatile ApiType* addr, ApiType val) {
    return InterlockedExchange8(addr, val);
  }

  static ApiType compareExchange(volatile ApiType* addr, ApiType newval,
                                 ApiType oldval) {
    return _InterlockedCompareExchange8(addr, newval, oldval);
  }

  static ApiType exchangeAdd(volatile ApiType* addr, ApiType val) {
    return _InterlockedExchangeAdd8(addr, val);
  }

  static ApiType exchangeAnd(volatile ApiType* addr, ApiType val) {
    // Note: InterlockedExchangeAnd8 would be a more appropriate name for this
    // intrinsic, because like InterlockedExchangeAdd8, it returns the
    // _original_ value stored at 'addr' rather than the result of the
    // operation. Returning the original value is the desired behavior here.
    return InterlockedAnd8(addr, val);
  }

  static ApiType exchangeOr(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedOr8(addr, val);
  }

  static ApiType exchangeXor(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedXor8(addr, val);
  }
};

template <class T>
requires(sizeof(T) == 2) struct InterlockedIntrinsics<T> {
  using ApiType = SHORT;

  static ApiType exchange(volatile ApiType* addr, ApiType val) {
    return InterlockedExchange16(addr, val);
  }

  static ApiType compareExchange(volatile ApiType* addr, ApiType newval,
                                 ApiType oldval) {
    return InterlockedCompareExchange16(addr, newval, oldval);
  }

  static ApiType exchangeAdd(volatile ApiType* addr, ApiType val) {
    return _InterlockedExchangeAdd16(addr, val);
  }

  static ApiType exchangeAnd(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedAnd16(addr, val);
  }

  static ApiType exchangeOr(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedOr16(addr, val);
  }

  static ApiType exchangeXor(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedXor16(addr, val);
  }
};

template <class T>
requires(sizeof(T) == 4) struct InterlockedIntrinsics<T> {
  using ApiType = LONG;

  static ApiType exchange(volatile ApiType* addr, ApiType val) {
    return InterlockedExchange(addr, val);
  }

  static ApiType compareExchange(volatile ApiType* addr, ApiType newval,
                                 ApiType oldval) {
    return InterlockedCompareExchange(addr, newval, oldval);
  }

  static ApiType exchangeAdd(volatile ApiType* addr, ApiType val) {
    return InterlockedExchangeAdd(addr, val);
  }

  static ApiType exchangeAnd(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedAnd(addr, val);
  }

  static ApiType exchangeOr(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedOr(addr, val);
  }

  static ApiType exchangeXor(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedXor(addr, val);
  }
};

template <class T>
requires(sizeof(T) == 8) struct InterlockedIntrinsics<T> {
  using ApiType = LONG64;

  static ApiType exchange(volatile ApiType* addr, ApiType val) {
    return InterlockedExchange64(addr, val);
  }

  static ApiType compareExchange(volatile ApiType* addr, ApiType newval,
                                 ApiType oldval) {
    return InterlockedCompareExchange64(addr, newval, oldval);
  }

  static ApiType exchangeAdd(volatile ApiType* addr, ApiType val) {
    return InterlockedExchangeAdd64(addr, val);
  }

  static ApiType exchangeAnd(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedAnd64(addr, val);
  }

  static ApiType exchangeOr(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedOr64(addr, val);
  }

  static ApiType exchangeXor(volatile ApiType* addr, ApiType val) {
    // See the note above about InterlockedAnd8.
    return InterlockedXor64(addr, val);
  }
};
}  // namespace detail

template <typename T>
inline T js::jit::AtomicOperations::exchangeSeqCst(T* addr, T val) {
  using Intrinsics = typename ::detail::InterlockedIntrinsics<T>;
  using ApiType = typename Intrinsics::ApiType;
  ApiType v = ::detail::InterlockedIntrinsics<T>::exchange(
      reinterpret_cast<ApiType*>(addr), *reinterpret_cast<ApiType*>(&val));

  return *reinterpret_cast<ApiType*>(&v);
};

template <typename T>
inline T js::jit::AtomicOperations::compareExchangeSeqCst(T* addr, T oldval,
                                                          T newval) {
  using Intrinsics = typename ::detail::InterlockedIntrinsics<T>;
  using ApiType = typename Intrinsics::ApiType;
  ApiType v = ::detail::InterlockedIntrinsics<T>::compareExchange(
      reinterpret_cast<ApiType*>(addr), *reinterpret_cast<ApiType*>(&newval),
      *reinterpret_cast<ApiType*>(&oldval));
  return *reinterpret_cast<T*>(&v);
}

template <typename T>
inline T js::jit::AtomicOperations::fetchAddSeqCst(T* addr, T val) {
  // NB: This function is _not correct_ for unsigned T if the operation
  // overflows.
  static_assert(std::numeric_limits<T>::is_integer);

  using Intrinsics = typename ::detail::InterlockedIntrinsics<T>;
  using ApiType = typename Intrinsics::ApiType;
  return Intrinsics::exchangeAdd(reinterpret_cast<ApiType*>(addr), val);
}

template <typename T>
inline T js::jit::AtomicOperations::fetchSubSeqCst(T* addr, T val) {
  // NB: This function is _not correct_ for unsigned T if the operation
  // overflows.
  static_assert(std::numeric_limits<T>::is_integer);

  using Intrinsics = typename ::detail::InterlockedIntrinsics<T>;
  using ApiType = typename Intrinsics::ApiType;
  return Intrinsics::exchangeAdd(reinterpret_cast<ApiType*>(addr), -val);
}

template <typename T>
inline T js::jit::AtomicOperations::fetchAndSeqCst(T* addr, T val) {
  using Intrinsics = typename ::detail::InterlockedIntrinsics<T>;
  using ApiType = typename Intrinsics::ApiType;
  ApiType v = Intrinsics::exchangeAnd(reinterpret_cast<ApiType*>(addr),
                                      *reinterpret_cast<ApiType*>(val));

  return *reinterpret_cast<T*>(&v);
}

template <typename T>
inline T js::jit::AtomicOperations::fetchOrSeqCst(T* addr, T val) {
  using Intrinsics = typename ::detail::InterlockedIntrinsics<T>;
  using ApiType = typename Intrinsics::ApiType;
  ApiType v = Intrinsics::exchangeOr(reinterpret_cast<ApiType*>(addr),
                                     *reinterpret_cast<ApiType*>(val));

  return *reinterpret_cast<T*>(&v);
}

template <typename T>
inline T js::jit::AtomicOperations::fetchXorSeqCst(T* addr, T val) {
  using Intrinsics = typename ::detail::InterlockedIntrinsics<T>;
  using ApiType = typename Intrinsics::ApiType;
  ApiType v = Intrinsics::exchangeXor(reinterpret_cast<ApiType*>(addr),
                                      *reinterpret_cast<ApiType*>(val));

  return *reinterpret_cast<T*>(&v);
}

template <typename T>
inline T js::jit::AtomicOperations::loadSafeWhenRacy(T* addr) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
  // This is actually roughly right even on 32-bit platforms since in that
  // case, double, int64, and uint64 loads need not be access-atomic.
  //
  // We could use __atomic_load, but it would be needlessly expensive on
  // 32-bit platforms that could support it and just plain wrong on others.
  return *addr;
}

template <typename T>
inline void js::jit::AtomicOperations::storeSafeWhenRacy(T* addr, T val) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
  // This is actually roughly right even on 32-bit platforms since in that
  // case, double, int64, and uint64 loads need not be access-atomic.
  //
  // We could use __atomic_store, but it would be needlessly expensive on
  // 32-bit platforms that could support it and just plain wrong on others.
  *addr = val;
}

inline void js::jit::AtomicOperations::memcpySafeWhenRacy(void* dest,
                                                          const void* src,
                                                          size_t nbytes) {
  MOZ_ASSERT(!((char*)dest <= (char*)src && (char*)src < (char*)dest + nbytes));
  MOZ_ASSERT(!((char*)src <= (char*)dest && (char*)dest < (char*)src + nbytes));
  ::memcpy(dest, src, nbytes);
}

inline void js::jit::AtomicOperations::memmoveSafeWhenRacy(void* dest,
                                                           const void* src,
                                                           size_t nbytes) {
  ::memmove(dest, src, nbytes);
}

#endif  // jit_shared_AtomicOperations_feeling_lucky_msvc_h
