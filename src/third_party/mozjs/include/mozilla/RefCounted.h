/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* CRTP refcounting templates.  Do not use unless you are an Expert. */

#ifndef mozilla_RefCounted_h
#define mozilla_RefCounted_h

#include <utility>

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/RefCountType.h"

#ifdef __wasi__
#  include "mozilla/WasiAtomic.h"
#else
#  include <atomic>
#endif  // __wasi__

#if defined(MOZILLA_INTERNAL_API)
#  include "nsXPCOM.h"
#endif

#if defined(MOZILLA_INTERNAL_API) && \
    (defined(DEBUG) || defined(FORCE_BUILD_REFCNT_LOGGING))
#  define MOZ_REFCOUNTED_LEAK_CHECKING
#endif

namespace mozilla {

/**
 * RefCounted<T> is a sort of a "mixin" for a class T.  RefCounted
 * manages, well, refcounting for T, and because RefCounted is
 * parameterized on T, RefCounted<T> can call T's destructor directly.
 * This means T doesn't need to have a virtual dtor and so doesn't
 * need a vtable.
 *
 * RefCounted<T> is created with refcount == 0.  Newly-allocated
 * RefCounted<T> must immediately be assigned to a RefPtr to make the
 * refcount > 0.  It's an error to allocate and free a bare
 * RefCounted<T>, i.e. outside of the RefPtr machinery.  Attempts to
 * do so will abort DEBUG builds.
 *
 * Live RefCounted<T> have refcount > 0.  The lifetime (refcounts) of
 * live RefCounted<T> are controlled by RefPtr<T> and
 * RefPtr<super/subclass of T>.  Upon a transition from refcounted==1
 * to 0, the RefCounted<T> "dies" and is destroyed.  The "destroyed"
 * state is represented in DEBUG builds by refcount==0xffffdead.  This
 * state distinguishes use-before-ref (refcount==0) from
 * use-after-destroy (refcount==0xffffdead).
 *
 * Note that when deriving from RefCounted or AtomicRefCounted, you
 * should add MOZ_DECLARE_REFCOUNTED_TYPENAME(ClassName) to the public
 * section of your class, where ClassName is the name of your class.
 *
 * Note: SpiderMonkey should use js::RefCounted instead since that type
 * will use appropriate js_delete and also not break ref-count logging.
 */
namespace detail {
const MozRefCountType DEAD = 0xffffdead;

// When building code that gets compiled into Gecko, try to use the
// trace-refcount leak logging facilities.
class RefCountLogger {
 public:
  // Called by `RefCounted`-like classes to log a successful AddRef call in the
  // Gecko leak-logging system. This call is a no-op outside of Gecko. Should be
  // called afer incrementing the reference count.
  template <class T>
  static void logAddRef(const T* aPointer, MozRefCountType aRefCount) {
#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
    const void* pointer = aPointer;
    const char* typeName = aPointer->typeName();
    uint32_t typeSize = aPointer->typeSize();
    NS_LogAddRef(const_cast<void*>(pointer), aRefCount, typeName, typeSize);
#endif
  }

  // Created by `RefCounted`-like classes to log a successful Release call in
  // the Gecko leak-logging system. The constructor should be invoked before the
  // refcount is decremented to avoid invoking `typeName()` with a zero
  // reference count. This call is a no-op outside of Gecko.
  class MOZ_STACK_CLASS ReleaseLogger final {
   public:
    template <class T>
    explicit ReleaseLogger(const T* aPointer)
#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
        : mPointer(aPointer),
          mTypeName(aPointer->typeName())
#endif
    {
    }

    void logRelease(MozRefCountType aRefCount) {
#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
      MOZ_ASSERT(aRefCount != DEAD);
      NS_LogRelease(const_cast<void*>(mPointer), aRefCount, mTypeName);
#endif
    }

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
    const void* mPointer;
    const char* mTypeName;
#endif
  };
};

// This is used WeakPtr.h as well as this file.
enum RefCountAtomicity { AtomicRefCount, NonAtomicRefCount };

template <typename T, RefCountAtomicity Atomicity>
class RC {
 public:
  explicit RC(T aCount) : mValue(aCount) {}

  RC(const RC&) = delete;
  RC& operator=(const RC&) = delete;
  RC(RC&&) = delete;
  RC& operator=(RC&&) = delete;

  T operator++() { return ++mValue; }
  T operator--() { return --mValue; }

#ifdef DEBUG
  void operator=(const T& aValue) { mValue = aValue; }
#endif

  operator T() const { return mValue; }

 private:
  T mValue;
};

template <typename T>
class RC<T, AtomicRefCount> {
 public:
  explicit RC(T aCount) : mValue(aCount) {}

  RC(const RC&) = delete;
  RC& operator=(const RC&) = delete;
  RC(RC&&) = delete;
  RC& operator=(RC&&) = delete;

  T operator++() {
    // Memory synchronization is not required when incrementing a
    // reference count.  The first increment of a reference count on a
    // thread is not important, since the first use of the object on a
    // thread can happen before it.  What is important is the transfer
    // of the pointer to that thread, which may happen prior to the
    // first increment on that thread.  The necessary memory
    // synchronization is done by the mechanism that transfers the
    // pointer between threads.
    return mValue.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  T operator--() {
    // Since this may be the last release on this thread, we need
    // release semantics so that prior writes on this thread are visible
    // to the thread that destroys the object when it reads mValue with
    // acquire semantics.
    T result = mValue.fetch_sub(1, std::memory_order_release) - 1;
    if (result == 0) {
      // We're going to destroy the object on this thread, so we need
      // acquire semantics to synchronize with the memory released by
      // the last release on other threads, that is, to ensure that
      // writes prior to that release are now visible on this thread.
#if defined(MOZ_TSAN) || defined(__wasi__)
      // TSan doesn't understand std::atomic_thread_fence, so in order
      // to avoid a false positive for every time a refcounted object
      // is deleted, we replace the fence with an atomic operation.
      mValue.load(std::memory_order_acquire);
#else
      std::atomic_thread_fence(std::memory_order_acquire);
#endif
    }
    return result;
  }

#ifdef DEBUG
  // This method is only called in debug builds, so we're not too concerned
  // about its performance.
  void operator=(const T& aValue) {
    mValue.store(aValue, std::memory_order_seq_cst);
  }
#endif

  operator T() const {
    // Use acquire semantics since we're not sure what the caller is
    // doing.
    return mValue.load(std::memory_order_acquire);
  }

  T IncrementIfNonzero() {
    // This can be a relaxed load as any write of 0 that we observe will leave
    // the field in a permanently zero (or `DEAD`) state (so a "stale" read of 0
    // is fine), and any other value is confirmed by the CAS below.
    //
    // This roughly matches rust's Arc::upgrade implementation as of rust 1.49.0
    T prev = mValue.load(std::memory_order_relaxed);
    while (prev != 0) {
      MOZ_ASSERT(prev != detail::DEAD,
                 "Cannot IncrementIfNonzero if marked as dead!");
      // TODO: It may be possible to use relaxed success ordering here?
      if (mValue.compare_exchange_weak(prev, prev + 1,
                                       std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
        return prev + 1;
      }
    }
    return 0;
  }

 private:
  std::atomic<T> mValue;
};

template <typename T, RefCountAtomicity Atomicity>
class RefCounted {
 protected:
  RefCounted() : mRefCnt(0) {}
#ifdef DEBUG
  ~RefCounted() { MOZ_ASSERT(mRefCnt == detail::DEAD); }
#endif

 public:
  // Compatibility with RefPtr.
  void AddRef() const {
    // Note: this method must be thread safe for AtomicRefCounted.
    MOZ_ASSERT(int32_t(mRefCnt) >= 0);
    MozRefCountType cnt = ++mRefCnt;
    detail::RefCountLogger::logAddRef(static_cast<const T*>(this), cnt);
  }

  void Release() const {
    // Note: this method must be thread safe for AtomicRefCounted.
    MOZ_ASSERT(int32_t(mRefCnt) > 0);
    detail::RefCountLogger::ReleaseLogger logger(static_cast<const T*>(this));
    MozRefCountType cnt = --mRefCnt;
    // Note: it's not safe to touch |this| after decrementing the refcount,
    // except for below.
    logger.logRelease(cnt);
    if (0 == cnt) {
      // Because we have atomically decremented the refcount above, only
      // one thread can get a 0 count here, so as long as we can assume that
      // everything else in the system is accessing this object through
      // RefPtrs, it's safe to access |this| here.
#ifdef DEBUG
      mRefCnt = detail::DEAD;
#endif
      delete static_cast<const T*>(this);
    }
  }

  // Compatibility with wtf::RefPtr.
  void ref() { AddRef(); }
  void deref() { Release(); }
  MozRefCountType refCount() const { return mRefCnt; }
  bool hasOneRef() const {
    MOZ_ASSERT(mRefCnt > 0);
    return mRefCnt == 1;
  }

 private:
  mutable RC<MozRefCountType, Atomicity> mRefCnt;
};

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
// Passing override for the optional argument marks the typeName and
// typeSize functions defined by this macro as overrides.
#  define MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(T, ...)           \
    virtual const char* typeName() const __VA_ARGS__ { return #T; } \
    virtual size_t typeSize() const __VA_ARGS__ { return sizeof(*this); }
#else
#  define MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(T, ...)
#endif

// Note that this macro is expanded unconditionally because it declares only
// two small inline functions which will hopefully get eliminated by the linker
// in non-leak-checking builds.
#define MOZ_DECLARE_REFCOUNTED_TYPENAME(T)    \
  const char* typeName() const { return #T; } \
  size_t typeSize() const { return sizeof(*this); }

}  // namespace detail

template <typename T>
class RefCounted : public detail::RefCounted<T, detail::NonAtomicRefCount> {
 public:
  ~RefCounted() {
    static_assert(std::is_base_of<RefCounted, T>::value,
                  "T must derive from RefCounted<T>");
  }
};

namespace external {

/**
 * AtomicRefCounted<T> is like RefCounted<T>, with an atomically updated
 * reference counter.
 *
 * NOTE: Please do not use this class, use NS_INLINE_DECL_THREADSAFE_REFCOUNTING
 * instead.
 */
template <typename T>
class AtomicRefCounted
    : public mozilla::detail::RefCounted<T, mozilla::detail::AtomicRefCount> {
 public:
  ~AtomicRefCounted() {
    static_assert(std::is_base_of<AtomicRefCounted, T>::value,
                  "T must derive from AtomicRefCounted<T>");
  }
};

}  // namespace external

}  // namespace mozilla

#endif  // mozilla_RefCounted_h
