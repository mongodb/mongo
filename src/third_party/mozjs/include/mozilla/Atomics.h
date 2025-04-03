/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implements (almost always) lock-free atomic operations. The operations here
 * are a subset of that which can be found in C++11's <atomic> header, with a
 * different API to enforce consistent memory ordering constraints.
 *
 * Anyone caught using |volatile| for inter-thread memory safety needs to be
 * sent a copy of this header and the C++11 standard.
 */

#ifndef mozilla_Atomics_h
#define mozilla_Atomics_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Compiler.h"

#ifdef __wasi__
#  include "mozilla/WasiAtomic.h"
#else
#  include <atomic>
#endif  // __wasi__

#include <stdint.h>
#include <type_traits>

namespace mozilla {

/**
 * An enum of memory ordering possibilities for atomics.
 *
 * Memory ordering is the observable state of distinct values in memory.
 * (It's a separate concept from atomicity, which concerns whether an
 * operation can ever be observed in an intermediate state.  Don't
 * conflate the two!)  Given a sequence of operations in source code on
 * memory, it is *not* always the case that, at all times and on all
 * cores, those operations will appear to have occurred in that exact
 * sequence.  First, the compiler might reorder that sequence, if it
 * thinks another ordering will be more efficient.  Second, the CPU may
 * not expose so consistent a view of memory.  CPUs will often perform
 * their own instruction reordering, above and beyond that performed by
 * the compiler.  And each core has its own memory caches, and accesses
 * (reads and writes both) to "memory" may only resolve to out-of-date
 * cache entries -- not to the "most recently" performed operation in
 * some global sense.  Any access to a value that may be used by
 * multiple threads, potentially across multiple cores, must therefore
 * have a memory ordering imposed on it, for all code on all
 * threads/cores to have a sufficiently coherent worldview.
 *
 * http://gcc.gnu.org/wiki/Atomic/GCCMM/AtomicSync and
 * http://en.cppreference.com/w/cpp/atomic/memory_order go into more
 * detail on all this, including examples of how each mode works.
 *
 * Note that for simplicity and practicality, not all of the modes in
 * C++11 are supported.  The missing C++11 modes are either subsumed by
 * the modes we provide below, or not relevant for the CPUs we support
 * in Gecko.  These three modes are confusing enough as it is!
 */
enum MemoryOrdering {
  /*
   * Relaxed ordering is the simplest memory ordering: none at all.
   * When the result of a write is observed, nothing may be inferred
   * about other memory.  Writes ostensibly performed "before" on the
   * writing thread may not yet be visible.  Writes performed "after" on
   * the writing thread may already be visible, if the compiler or CPU
   * reordered them.  (The latter can happen if reads and/or writes get
   * held up in per-processor caches.)  Relaxed ordering means
   * operations can always use cached values (as long as the actual
   * updates to atomic values actually occur, correctly, eventually), so
   * it's usually the fastest sort of atomic access.  For this reason,
   * *it's also the most dangerous kind of access*.
   *
   * Relaxed ordering is good for things like process-wide statistics
   * counters that don't need to be consistent with anything else, so
   * long as updates themselves are atomic.  (And so long as any
   * observations of that value can tolerate being out-of-date -- if you
   * need some sort of up-to-date value, you need some sort of other
   * synchronizing operation.)  It's *not* good for locks, mutexes,
   * reference counts, etc. that mediate access to other memory, or must
   * be observably consistent with other memory.
   *
   * x86 architectures don't take advantage of the optimization
   * opportunities that relaxed ordering permits.  Thus it's possible
   * that using relaxed ordering will "work" on x86 but fail elsewhere
   * (ARM, say, which *does* implement non-sequentially-consistent
   * relaxed ordering semantics).  Be extra-careful using relaxed
   * ordering if you can't easily test non-x86 architectures!
   */
  Relaxed,

  /*
   * When an atomic value is updated with ReleaseAcquire ordering, and
   * that new value is observed with ReleaseAcquire ordering, prior
   * writes (atomic or not) are also observable.  What ReleaseAcquire
   * *doesn't* give you is any observable ordering guarantees for
   * ReleaseAcquire-ordered operations on different objects.  For
   * example, if there are two cores that each perform ReleaseAcquire
   * operations on separate objects, each core may or may not observe
   * the operations made by the other core.  The only way the cores can
   * be synchronized with ReleaseAcquire is if they both
   * ReleaseAcquire-access the same object.  This implies that you can't
   * necessarily describe some global total ordering of ReleaseAcquire
   * operations.
   *
   * ReleaseAcquire ordering is good for (as the name implies) atomic
   * operations on values controlling ownership of things: reference
   * counts, mutexes, and the like.  However, if you are thinking about
   * using these to implement your own locks or mutexes, you should take
   * a good, hard look at actual lock or mutex primitives first.
   */
  ReleaseAcquire,

  /*
   * When an atomic value is updated with SequentiallyConsistent
   * ordering, all writes observable when the update is observed, just
   * as with ReleaseAcquire ordering.  But, furthermore, a global total
   * ordering of SequentiallyConsistent operations *can* be described.
   * For example, if two cores perform SequentiallyConsistent operations
   * on separate objects, one core will observably perform its update
   * (and all previous operations will have completed), then the other
   * core will observably perform its update (and all previous
   * operations will have completed).  (Although those previous
   * operations aren't themselves ordered -- they could be intermixed,
   * or ordered if they occur on atomic values with ordering
   * requirements.)  SequentiallyConsistent is the *simplest and safest*
   * ordering of atomic operations -- it's always as if one operation
   * happens, then another, then another, in some order -- and every
   * core observes updates to happen in that single order.  Because it
   * has the most synchronization requirements, operations ordered this
   * way also tend to be slowest.
   *
   * SequentiallyConsistent ordering can be desirable when multiple
   * threads observe objects, and they all have to agree on the
   * observable order of changes to them.  People expect
   * SequentiallyConsistent ordering, even if they shouldn't, when
   * writing code, atomic or otherwise.  SequentiallyConsistent is also
   * the ordering of choice when designing lockless data structures.  If
   * you don't know what order to use, use this one.
   */
  SequentiallyConsistent,
};

namespace detail {

/*
 * We provide CompareExchangeFailureOrder to work around a bug in some
 * versions of GCC's <atomic> header.  See bug 898491.
 */
template <MemoryOrdering Order>
struct AtomicOrderConstraints;

template <>
struct AtomicOrderConstraints<Relaxed> {
  static const std::memory_order AtomicRMWOrder = std::memory_order_relaxed;
  static const std::memory_order LoadOrder = std::memory_order_relaxed;
  static const std::memory_order StoreOrder = std::memory_order_relaxed;
  static const std::memory_order CompareExchangeFailureOrder =
      std::memory_order_relaxed;
};

template <>
struct AtomicOrderConstraints<ReleaseAcquire> {
  static const std::memory_order AtomicRMWOrder = std::memory_order_acq_rel;
  static const std::memory_order LoadOrder = std::memory_order_acquire;
  static const std::memory_order StoreOrder = std::memory_order_release;
  static const std::memory_order CompareExchangeFailureOrder =
      std::memory_order_acquire;
};

template <>
struct AtomicOrderConstraints<SequentiallyConsistent> {
  static const std::memory_order AtomicRMWOrder = std::memory_order_seq_cst;
  static const std::memory_order LoadOrder = std::memory_order_seq_cst;
  static const std::memory_order StoreOrder = std::memory_order_seq_cst;
  static const std::memory_order CompareExchangeFailureOrder =
      std::memory_order_seq_cst;
};

template <typename T, MemoryOrdering Order>
struct IntrinsicBase {
  typedef std::atomic<T> ValueType;
  typedef AtomicOrderConstraints<Order> OrderedOp;
};

template <typename T, MemoryOrdering Order>
struct IntrinsicMemoryOps : public IntrinsicBase<T, Order> {
  typedef IntrinsicBase<T, Order> Base;

  static T load(const typename Base::ValueType& aPtr) {
    return aPtr.load(Base::OrderedOp::LoadOrder);
  }

  static void store(typename Base::ValueType& aPtr, T aVal) {
    aPtr.store(aVal, Base::OrderedOp::StoreOrder);
  }

  static T exchange(typename Base::ValueType& aPtr, T aVal) {
    return aPtr.exchange(aVal, Base::OrderedOp::AtomicRMWOrder);
  }

  static bool compareExchange(typename Base::ValueType& aPtr, T aOldVal,
                              T aNewVal) {
    return aPtr.compare_exchange_strong(
        aOldVal, aNewVal, Base::OrderedOp::AtomicRMWOrder,
        Base::OrderedOp::CompareExchangeFailureOrder);
  }
};

template <typename T, MemoryOrdering Order>
struct IntrinsicAddSub : public IntrinsicBase<T, Order> {
  typedef IntrinsicBase<T, Order> Base;

  static T add(typename Base::ValueType& aPtr, T aVal) {
    return aPtr.fetch_add(aVal, Base::OrderedOp::AtomicRMWOrder);
  }

  static T sub(typename Base::ValueType& aPtr, T aVal) {
    return aPtr.fetch_sub(aVal, Base::OrderedOp::AtomicRMWOrder);
  }
};

template <typename T, MemoryOrdering Order>
struct IntrinsicAddSub<T*, Order> : public IntrinsicBase<T*, Order> {
  typedef IntrinsicBase<T*, Order> Base;

  static T* add(typename Base::ValueType& aPtr, ptrdiff_t aVal) {
    return aPtr.fetch_add(aVal, Base::OrderedOp::AtomicRMWOrder);
  }

  static T* sub(typename Base::ValueType& aPtr, ptrdiff_t aVal) {
    return aPtr.fetch_sub(aVal, Base::OrderedOp::AtomicRMWOrder);
  }
};

template <typename T, MemoryOrdering Order>
struct IntrinsicIncDec : public IntrinsicAddSub<T, Order> {
  typedef IntrinsicBase<T, Order> Base;

  static T inc(typename Base::ValueType& aPtr) {
    return IntrinsicAddSub<T, Order>::add(aPtr, 1);
  }

  static T dec(typename Base::ValueType& aPtr) {
    return IntrinsicAddSub<T, Order>::sub(aPtr, 1);
  }
};

template <typename T, MemoryOrdering Order>
struct AtomicIntrinsics : public IntrinsicMemoryOps<T, Order>,
                          public IntrinsicIncDec<T, Order> {
  typedef IntrinsicBase<T, Order> Base;

  static T or_(typename Base::ValueType& aPtr, T aVal) {
    return aPtr.fetch_or(aVal, Base::OrderedOp::AtomicRMWOrder);
  }

  static T xor_(typename Base::ValueType& aPtr, T aVal) {
    return aPtr.fetch_xor(aVal, Base::OrderedOp::AtomicRMWOrder);
  }

  static T and_(typename Base::ValueType& aPtr, T aVal) {
    return aPtr.fetch_and(aVal, Base::OrderedOp::AtomicRMWOrder);
  }
};

template <typename T, MemoryOrdering Order>
struct AtomicIntrinsics<T*, Order> : public IntrinsicMemoryOps<T*, Order>,
                                     public IntrinsicIncDec<T*, Order> {};

template <typename T>
struct ToStorageTypeArgument {
  static constexpr T convert(T aT) { return aT; }
};

template <typename T, MemoryOrdering Order>
class AtomicBase {
  static_assert(sizeof(T) == 4 || sizeof(T) == 8,
                "mozilla/Atomics.h only supports 32-bit and 64-bit types");

 protected:
  typedef typename detail::AtomicIntrinsics<T, Order> Intrinsics;
  typedef typename Intrinsics::ValueType ValueType;
  ValueType mValue;

 public:
  constexpr AtomicBase() : mValue() {}
  explicit constexpr AtomicBase(T aInit)
      : mValue(ToStorageTypeArgument<T>::convert(aInit)) {}

  // Note: we can't provide operator T() here because Atomic<bool> inherits
  // from AtomcBase with T=uint32_t and not T=bool. If we implemented
  // operator T() here, it would cause errors when comparing Atomic<bool> with
  // a regular bool.

  T operator=(T aVal) {
    Intrinsics::store(mValue, aVal);
    return aVal;
  }

  /**
   * Performs an atomic swap operation.  aVal is stored and the previous
   * value of this variable is returned.
   */
  T exchange(T aVal) { return Intrinsics::exchange(mValue, aVal); }

  /**
   * Performs an atomic compare-and-swap operation and returns true if it
   * succeeded. This is equivalent to atomically doing
   *
   *   if (mValue == aOldValue) {
   *     mValue = aNewValue;
   *     return true;
   *   } else {
   *     return false;
   *   }
   */
  bool compareExchange(T aOldValue, T aNewValue) {
    return Intrinsics::compareExchange(mValue, aOldValue, aNewValue);
  }

 private:
  AtomicBase(const AtomicBase& aCopy) = delete;
};

template <typename T, MemoryOrdering Order>
class AtomicBaseIncDec : public AtomicBase<T, Order> {
  typedef typename detail::AtomicBase<T, Order> Base;

 public:
  constexpr AtomicBaseIncDec() : Base() {}
  explicit constexpr AtomicBaseIncDec(T aInit) : Base(aInit) {}

  using Base::operator=;

  operator T() const { return Base::Intrinsics::load(Base::mValue); }
  T operator++(int) { return Base::Intrinsics::inc(Base::mValue); }
  T operator--(int) { return Base::Intrinsics::dec(Base::mValue); }
  T operator++() { return Base::Intrinsics::inc(Base::mValue) + 1; }
  T operator--() { return Base::Intrinsics::dec(Base::mValue) - 1; }

 private:
  AtomicBaseIncDec(const AtomicBaseIncDec& aCopy) = delete;
};

}  // namespace detail

/**
 * A wrapper for a type that enforces that all memory accesses are atomic.
 *
 * In general, where a variable |T foo| exists, |Atomic<T> foo| can be used in
 * its place.  Implementations for integral and pointer types are provided
 * below.
 *
 * Atomic accesses are sequentially consistent by default.  You should
 * use the default unless you are tall enough to ride the
 * memory-ordering roller coaster (if you're not sure, you aren't) and
 * you have a compelling reason to do otherwise.
 *
 * There is one exception to the case of atomic memory accesses: providing an
 * initial value of the atomic value is not guaranteed to be atomic.  This is a
 * deliberate design choice that enables static atomic variables to be declared
 * without introducing extra static constructors.
 */
template <typename T, MemoryOrdering Order = SequentiallyConsistent,
          typename Enable = void>
class Atomic;

/**
 * Atomic<T> implementation for integral types.
 *
 * In addition to atomic store and load operations, compound assignment and
 * increment/decrement operators are implemented which perform the
 * corresponding read-modify-write operation atomically.  Finally, an atomic
 * swap method is provided.
 */
template <typename T, MemoryOrdering Order>
class Atomic<
    T, Order,
    std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
    : public detail::AtomicBaseIncDec<T, Order> {
  typedef typename detail::AtomicBaseIncDec<T, Order> Base;

 public:
  constexpr Atomic() : Base() {}
  explicit constexpr Atomic(T aInit) : Base(aInit) {}

  using Base::operator=;

  T operator+=(T aDelta) {
    return Base::Intrinsics::add(Base::mValue, aDelta) + aDelta;
  }

  T operator-=(T aDelta) {
    return Base::Intrinsics::sub(Base::mValue, aDelta) - aDelta;
  }

  T operator|=(T aVal) {
    return Base::Intrinsics::or_(Base::mValue, aVal) | aVal;
  }

  T operator^=(T aVal) {
    return Base::Intrinsics::xor_(Base::mValue, aVal) ^ aVal;
  }

  T operator&=(T aVal) {
    return Base::Intrinsics::and_(Base::mValue, aVal) & aVal;
  }

 private:
  Atomic(Atomic& aOther) = delete;
};

/**
 * Atomic<T> implementation for pointer types.
 *
 * An atomic compare-and-swap primitive for pointer variables is provided, as
 * are atomic increment and decement operators.  Also provided are the compound
 * assignment operators for addition and subtraction. Atomic swap (via
 * exchange()) is included as well.
 */
template <typename T, MemoryOrdering Order>
class Atomic<T*, Order> : public detail::AtomicBaseIncDec<T*, Order> {
  typedef typename detail::AtomicBaseIncDec<T*, Order> Base;

 public:
  constexpr Atomic() : Base() {}
  explicit constexpr Atomic(T* aInit) : Base(aInit) {}

  using Base::operator=;

  T* operator+=(ptrdiff_t aDelta) {
    return Base::Intrinsics::add(Base::mValue, aDelta) + aDelta;
  }

  T* operator-=(ptrdiff_t aDelta) {
    return Base::Intrinsics::sub(Base::mValue, aDelta) - aDelta;
  }

 private:
  Atomic(Atomic& aOther) = delete;
};

/**
 * Atomic<T> implementation for enum types.
 *
 * The atomic store and load operations and the atomic swap method is provided.
 */
template <typename T, MemoryOrdering Order>
class Atomic<T, Order, std::enable_if_t<std::is_enum_v<T>>>
    : public detail::AtomicBase<T, Order> {
  typedef typename detail::AtomicBase<T, Order> Base;

 public:
  constexpr Atomic() : Base() {}
  explicit constexpr Atomic(T aInit) : Base(aInit) {}

  operator T() const { return T(Base::Intrinsics::load(Base::mValue)); }

  using Base::operator=;

 private:
  Atomic(Atomic& aOther) = delete;
};

/**
 * Atomic<T> implementation for boolean types.
 *
 * The atomic store and load operations and the atomic swap method is provided.
 *
 * Note:
 *
 * - sizeof(Atomic<bool>) != sizeof(bool) for some implementations of
 *   bool and/or some implementations of std::atomic. This is allowed in
 *   [atomic.types.generic]p9.
 *
 * - It's not obvious whether the 8-bit atomic functions on Windows are always
 *   inlined or not. If they are not inlined, the corresponding functions in the
 *   runtime library are not available on Windows XP. This is why we implement
 *   Atomic<bool> with an underlying type of uint32_t.
 */
template <MemoryOrdering Order>
class Atomic<bool, Order> : protected detail::AtomicBase<uint32_t, Order> {
  typedef typename detail::AtomicBase<uint32_t, Order> Base;

 public:
  constexpr Atomic() : Base() {}
  explicit constexpr Atomic(bool aInit) : Base(aInit) {}

  // We provide boolean wrappers for the underlying AtomicBase methods.
  MOZ_IMPLICIT operator bool() const {
    return Base::Intrinsics::load(Base::mValue);
  }

  bool operator=(bool aVal) { return Base::operator=(aVal); }

  bool exchange(bool aVal) { return Base::exchange(aVal); }

  bool compareExchange(bool aOldValue, bool aNewValue) {
    return Base::compareExchange(aOldValue, aNewValue);
  }

 private:
  Atomic(Atomic& aOther) = delete;
};

}  // namespace mozilla

namespace std {

// If you want to atomically swap two atomic values, use exchange().
template <typename T, mozilla::MemoryOrdering Order>
void swap(mozilla::Atomic<T, Order>&, mozilla::Atomic<T, Order>&) = delete;

}  // namespace std

#endif /* mozilla_Atomics_h */
