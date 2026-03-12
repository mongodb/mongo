/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* For documentation, see jit/AtomicOperations.h, both the comment block at the
 * beginning and the #ifdef nest near the end.
 *
 * This is a common file for tier-3 platforms (including simulators for our
 * tier-1 platforms) that are not providing hardware-specific implementations of
 * the atomic operations.  Please keep it reasonably platform-independent by
 * adding #ifdefs at the beginning as much as possible, not throughout the file.
 *
 *
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * !!!!                              NOTE                                 !!!!
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *
 * The implementations in this file are NOT SAFE and cannot be safe even in
 * principle because they rely on C++ undefined behavior.  However, they are
 * frequently good enough for tier-3 platforms.
 */

#ifndef jit_shared_AtomicOperations_feeling_lucky_gcc_h
#define jit_shared_AtomicOperations_feeling_lucky_gcc_h

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

// Explicitly exclude tier-1 platforms.

#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
     defined(_M_IX86) || (defined(__arm__) && __ARM_ARCH >= 7) ||   \
     defined(__aarch64__))
#  error "Do not use on a tier-1 platform where inline assembly is available"
#endif

#if !(defined(__clang__) || defined(__GNUC__))
#  error "This file only for gcc/Clang"
#endif

// 64-bit atomics are not required by the JS spec, and you can compile
// SpiderMonkey without them. 64-bit atomics are required for BigInt
// support.
//
// 64-bit lock-free atomics are required for WebAssembly, but gating in the
// WebAssembly subsystem ensures that no WebAssembly-supporting platforms need
// code in this file.

#if defined(JS_SIMULATOR_ARM64) || defined(JS_SIMULATOR_ARM) || \
    defined(JS_SIMULATOR_MIPS64) || defined(JS_SIMULATOR_LOONG64)
// On some x86 (32-bit) systems this will not work because the compiler does not
// open-code 64-bit atomics.  If so, try linking with -latomic.  If that doesn't
// work, you're mostly on your own.
#  define HAS_64BIT_ATOMICS
#  define HAS_64BIT_LOCKFREE
#endif

#if defined(__arm__)
#  define HAS_64BIT_ATOMICS
#endif

#if defined(__ppc64__) || defined(__PPC64__) || defined(__ppc64le__) || \
    defined(__PPC64LE__)
#  define HAS_64BIT_ATOMICS
#  define HAS_64BIT_LOCKFREE
#endif

#if defined(__riscv) && __riscv_xlen == 64
#  define HAS_64BIT_ATOMICS
#  define HAS_64BIT_LOCKFREE
#endif

#if defined(__loongarch64)
#  define HAS_64BIT_ATOMICS
#  define HAS_64BIT_LOCKFREE
#endif

#ifdef __sparc__
#  ifdef __LP64__
#    define HAS_64BIT_ATOMICS
#    define HAS_64BIT_LOCKFREE
#  endif
#endif

#ifdef JS_CODEGEN_NONE
#  ifdef JS_64BIT
#    define HAS_64BIT_ATOMICS
#    define HAS_64BIT_LOCKFREE
#  endif
#endif

// The default implementation tactic for gcc/clang is to use the newer __atomic
// intrinsics added for use in C++11 <atomic>.  Where that isn't available, we
// use GCC's older __sync functions instead.
//
// ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS is kept as a backward compatible
// option for older compilers: enable this to use GCC's old __sync functions
// instead of the newer __atomic functions.  This will be required for GCC 4.6.x
// and earlier, and probably for Clang 3.1, should we need to use those
// versions.  Firefox no longer supports compilers that old.

// #define ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS

// Sanity check.

#if defined(HAS_64BIT_LOCKFREE) && !defined(HAS_64BIT_ATOMICS)
#  error "This combination of features is senseless, please fix"
#endif

// Try to avoid platform #ifdefs below this point.

// When compiling with Clang on 32-bit linux it will be necessary to link with
// -latomic to get the proper 64-bit intrinsics.

inline bool js::jit::AtomicOperations::hasAtomic8() {
#if defined(HAS_64BIT_ATOMICS)
  return true;
#else
  return false;
#endif
}

inline bool js::jit::AtomicOperations::isLockfree8() {
#if defined(HAS_64BIT_LOCKFREE)
  return true;
#else
  return false;
#endif
}

inline void js::jit::AtomicOperations::fenceSeqCst() {
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  __sync_synchronize();
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

inline void js::jit::AtomicOperations::pause() {
  // No default implementation.
}

template <typename T>
inline T js::jit::AtomicOperations::loadSeqCst(T* addr) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  __sync_synchronize();
  T v = *addr;
  __sync_synchronize();
#else
  T v;
  __atomic_load(addr, &v, __ATOMIC_SEQ_CST);
#endif
  return v;
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline int64_t AtomicOperations::loadSeqCst(int64_t* addr) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline uint64_t AtomicOperations::loadSeqCst(uint64_t* addr) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

template <typename T>
inline void js::jit::AtomicOperations::storeSeqCst(T* addr, T val) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  __sync_synchronize();
  *addr = val;
  __sync_synchronize();
#else
  __atomic_store(addr, &val, __ATOMIC_SEQ_CST);
#endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline void AtomicOperations::storeSeqCst(int64_t* addr, int64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline void AtomicOperations::storeSeqCst(uint64_t* addr, uint64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

template <typename T>
inline T js::jit::AtomicOperations::exchangeSeqCst(T* addr, T val) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  T v;
  __sync_synchronize();
  do {
    v = *addr;
  } while (__sync_val_compare_and_swap(addr, v, val) != v);
  return v;
#else
  T v;
  __atomic_exchange(addr, &val, &v, __ATOMIC_SEQ_CST);
  return v;
#endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline int64_t AtomicOperations::exchangeSeqCst(int64_t* addr, int64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline uint64_t AtomicOperations::exchangeSeqCst(uint64_t* addr, uint64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

template <typename T>
inline T js::jit::AtomicOperations::compareExchangeSeqCst(T* addr, T oldval,
                                                          T newval) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  return __sync_val_compare_and_swap(addr, oldval, newval);
#else
  __atomic_compare_exchange(addr, &oldval, &newval, false, __ATOMIC_SEQ_CST,
                            __ATOMIC_SEQ_CST);
  return oldval;
#endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline int64_t AtomicOperations::compareExchangeSeqCst(int64_t* addr,
                                                       int64_t oldval,
                                                       int64_t newval) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline uint64_t AtomicOperations::compareExchangeSeqCst(uint64_t* addr,
                                                        uint64_t oldval,
                                                        uint64_t newval) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

template <typename T>
inline T js::jit::AtomicOperations::fetchAddSeqCst(T* addr, T val) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  return __sync_fetch_and_add(addr, val);
#else
  return __atomic_fetch_add(addr, val, __ATOMIC_SEQ_CST);
#endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline int64_t AtomicOperations::fetchAddSeqCst(int64_t* addr, int64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline uint64_t AtomicOperations::fetchAddSeqCst(uint64_t* addr, uint64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

template <typename T>
inline T js::jit::AtomicOperations::fetchSubSeqCst(T* addr, T val) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  return __sync_fetch_and_sub(addr, val);
#else
  return __atomic_fetch_sub(addr, val, __ATOMIC_SEQ_CST);
#endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline int64_t AtomicOperations::fetchSubSeqCst(int64_t* addr, int64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline uint64_t AtomicOperations::fetchSubSeqCst(uint64_t* addr, uint64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

template <typename T>
inline T js::jit::AtomicOperations::fetchAndSeqCst(T* addr, T val) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  return __sync_fetch_and_and(addr, val);
#else
  return __atomic_fetch_and(addr, val, __ATOMIC_SEQ_CST);
#endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline int64_t AtomicOperations::fetchAndSeqCst(int64_t* addr, int64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline uint64_t AtomicOperations::fetchAndSeqCst(uint64_t* addr, uint64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

template <typename T>
inline T js::jit::AtomicOperations::fetchOrSeqCst(T* addr, T val) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  return __sync_fetch_and_or(addr, val);
#else
  return __atomic_fetch_or(addr, val, __ATOMIC_SEQ_CST);
#endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline int64_t AtomicOperations::fetchOrSeqCst(int64_t* addr, int64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline uint64_t AtomicOperations::fetchOrSeqCst(uint64_t* addr, uint64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

template <typename T>
inline T js::jit::AtomicOperations::fetchXorSeqCst(T* addr, T val) {
  static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
#ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
  return __sync_fetch_and_xor(addr, val);
#else
  return __atomic_fetch_xor(addr, val, __ATOMIC_SEQ_CST);
#endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js {
namespace jit {

template <>
inline int64_t AtomicOperations::fetchXorSeqCst(int64_t* addr, int64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

template <>
inline uint64_t AtomicOperations::fetchXorSeqCst(uint64_t* addr, uint64_t val) {
  MOZ_CRASH("No 64-bit atomics");
}

}  // namespace jit
}  // namespace js
#endif

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

#undef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
#undef HAS_64BIT_ATOMICS
#undef HAS_64BIT_LOCKFREE

#endif  // jit_shared_AtomicOperations_feeling_lucky_gcc_h
