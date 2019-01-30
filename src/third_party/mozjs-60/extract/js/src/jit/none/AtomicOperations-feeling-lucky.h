/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* For documentation, see jit/AtomicOperations.h, both the comment block at the
 * beginning and the #ifdef nest near the end.
 *
 * This is a common file for tier-3 platforms that are not providing
 * hardware-specific implementations of the atomic operations.  Please keep it
 * reasonably platform-independent by adding #ifdefs at the beginning as much as
 * possible, not throughout the file.
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

#ifndef jit_none_AtomicOperations_feeling_lucky_h
#define jit_none_AtomicOperations_feeling_lucky_h

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

// 64-bit atomics are not required by the JS spec, and you can compile
// SpiderMonkey without them.
//
// 64-bit lock-free atomics are however required for WebAssembly, and
// WebAssembly will be disabled if you do not define both HAS_64BIT_ATOMICS and
// HAS_64BIT_LOCKFREE.
//
// If you are only able to provide 64-bit non-lock-free atomics and you really
// want WebAssembly support you can always just lie about the lock-freedom.
// After all, you're already feeling lucky.

#if defined(__ppc__) || defined(__PPC__)
#  define GNUC_COMPATIBLE
#endif

#if defined(__ppc64__) ||  defined (__PPC64__) || defined(__ppc64le__) || defined (__PPC64LE__)
#  define HAS_64BIT_ATOMICS
#  define HAS_64BIT_LOCKFREE
#  define GNUC_COMPATIBLE
#endif

#ifdef __sparc__
#  define GNUC_COMPATIBLE
#  ifdef  __LP64__
#    define HAS_64BIT_ATOMICS
#    define HAS_64BIT_LOCKFREE
#  endif
#endif

#ifdef __alpha__
#  define GNUC_COMPATIBLE
#endif

#ifdef __hppa__
#  define GNUC_COMPATIBLE
#endif

#ifdef __sh__
#  define GNUC_COMPATIBLE
#endif

#ifdef __s390__
#  define GNUC_COMPATIBLE
#endif

#ifdef __s390x__
#  define HAS_64BIT_ATOMICS
#  define HAS_64BIT_LOCKFREE
#  define GNUC_COMPATIBLE
#endif

// The default implementation tactic for gcc/clang is to use the newer
// __atomic intrinsics added for use in C++11 <atomic>.  Where that
// isn't available, we use GCC's older __sync functions instead.
//
// ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS is kept as a backward
// compatible option for older compilers: enable this to use GCC's old
// __sync functions instead of the newer __atomic functions.  This
// will be required for GCC 4.6.x and earlier, and probably for Clang
// 3.1, should we need to use those versions.

//#define ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS

// Sanity check.

#if defined(HAS_64BIT_LOCKFREE) && !defined(HAS_64BIT_ATOMICS)
#  error "This combination of features is senseless, please fix"
#endif

// Try to avoid platform #ifdefs below this point.

#ifdef GNUC_COMPATIBLE

inline bool
js::jit::AtomicOperations::hasAtomic8()
{
#if defined(HAS_64BIT_ATOMICS)
    return true;
#else
    return false;
#endif
}

inline bool
js::jit::AtomicOperations::isLockfree8()
{
#if defined(HAS_64BIT_LOCKFREE)
    return true;
#else
    return false;
#endif
}

inline void
js::jit::AtomicOperations::fenceSeqCst()
{
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    __sync_synchronize();
# else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
# endif
}

template<typename T>
inline T
js::jit::AtomicOperations::loadSeqCst(T* addr)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    __sync_synchronize();
    T v = *addr;
    __sync_synchronize();
# else
    T v;
    __atomic_load(addr, &v, __ATOMIC_SEQ_CST);
# endif
    return v;
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline int64_t
AtomicOperations::loadSeqCst(int64_t* addr) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline uint64_t
AtomicOperations::loadSeqCst(uint64_t* addr) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

template<typename T>
inline void
js::jit::AtomicOperations::storeSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    __sync_synchronize();
    *addr = val;
    __sync_synchronize();
# else
    __atomic_store(addr, &val, __ATOMIC_SEQ_CST);
# endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline void
AtomicOperations::storeSeqCst(int64_t* addr, int64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline void
AtomicOperations::storeSeqCst(uint64_t* addr, uint64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

template<typename T>
inline T
js::jit::AtomicOperations::compareExchangeSeqCst(T* addr, T oldval, T newval)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    return __sync_val_compare_and_swap(addr, oldval, newval);
# else
    __atomic_compare_exchange(addr, &oldval, &newval, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return oldval;
# endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline int64_t
AtomicOperations::compareExchangeSeqCst(int64_t* addr, int64_t oldval, int64_t newval) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline uint64_t
AtomicOperations::compareExchangeSeqCst(uint64_t* addr, uint64_t oldval, uint64_t newval) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

template<typename T>
inline T
js::jit::AtomicOperations::fetchAddSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    return __sync_fetch_and_add(addr, val);
# else
    return __atomic_fetch_add(addr, val, __ATOMIC_SEQ_CST);
# endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline int64_t
AtomicOperations::fetchAddSeqCst(int64_t* addr, int64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline uint64_t
AtomicOperations::fetchAddSeqCst(uint64_t* addr, uint64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

template<typename T>
inline T
js::jit::AtomicOperations::fetchSubSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    return __sync_fetch_and_sub(addr, val);
# else
    return __atomic_fetch_sub(addr, val, __ATOMIC_SEQ_CST);
# endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline int64_t
AtomicOperations::fetchSubSeqCst(int64_t* addr, int64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline uint64_t
AtomicOperations::fetchSubSeqCst(uint64_t* addr, uint64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

template<typename T>
inline T
js::jit::AtomicOperations::fetchAndSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    return __sync_fetch_and_and(addr, val);
# else
    return __atomic_fetch_and(addr, val, __ATOMIC_SEQ_CST);
# endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline int64_t
AtomicOperations::fetchAndSeqCst(int64_t* addr, int64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline uint64_t
AtomicOperations::fetchAndSeqCst(uint64_t* addr, uint64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

template<typename T>
inline T
js::jit::AtomicOperations::fetchOrSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    return __sync_fetch_and_or(addr, val);
# else
    return __atomic_fetch_or(addr, val, __ATOMIC_SEQ_CST);
# endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline int64_t
AtomicOperations::fetchOrSeqCst(int64_t* addr, int64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline uint64_t
AtomicOperations::fetchOrSeqCst(uint64_t* addr, uint64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

template<typename T>
inline T
js::jit::AtomicOperations::fetchXorSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    return __sync_fetch_and_xor(addr, val);
# else
    return __atomic_fetch_xor(addr, val, __ATOMIC_SEQ_CST);
# endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline int64_t
AtomicOperations::fetchXorSeqCst(int64_t* addr, int64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline uint64_t
AtomicOperations::fetchXorSeqCst(uint64_t* addr, uint64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

template<typename T>
inline T
js::jit::AtomicOperations::loadSafeWhenRacy(T* addr)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
    // This is actually roughly right even on 32-bit platforms since in that
    // case, double, int64, and uint64 loads need not be access-atomic.
    return *addr;
}

template<typename T>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(T* addr, T val)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
    // This is actually roughly right even on 32-bit platforms since in that
    // case, double, int64, and uint64 loads need not be access-atomic.
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

template<typename T>
inline T
js::jit::AtomicOperations::exchangeSeqCst(T* addr, T val)
{
    static_assert(sizeof(T) <= 8, "atomics supported up to 8 bytes only");
# ifdef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
    T v;
    __sync_synchronize();
    do {
	v = *addr;
    } while (__sync_val_compare_and_swap(addr, v, val) != v);
    return v;
# else
    T v;
    __atomic_exchange(addr, &val, &v, __ATOMIC_SEQ_CST);
    return v;
# endif
}

#ifndef HAS_64BIT_ATOMICS
namespace js { namespace jit {

template<>
inline int64_t
AtomicOperations::exchangeSeqCst(int64_t* addr, int64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

template<>
inline uint64_t
AtomicOperations::exchangeSeqCst(uint64_t* addr, uint64_t val) {
    MOZ_CRASH("No 64-bit atomics");
}

} }
#endif

#elif defined(ENABLE_SHARED_ARRAY_BUFFER)

# error "Either disable JS shared memory, use GCC or Clang, or add code here"

#endif

#undef ATOMICS_IMPLEMENTED_WITH_SYNC_INTRINSICS
#undef GNUC_COMPATIBLE
#undef HAS_64BIT_ATOMICS
#undef HAS_64BIT_LOCKFREE

#endif // jit_none_AtomicOperations_feeling_lucky_h
