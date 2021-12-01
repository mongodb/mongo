/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* CRTP refcounting templates.  Do not use unless you are an Expert. */

#ifndef mozilla_RefCounted_h
#define mozilla_RefCounted_h

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/Move.h"
#include "mozilla/RefCountType.h"
#include "mozilla/TypeTraits.h"

#include <atomic>

#if defined(MOZILLA_INTERNAL_API)
#include "nsXPCOM.h"
#endif

#if defined(MOZILLA_INTERNAL_API) && \
    (defined(DEBUG) || defined(FORCE_BUILD_REFCNT_LOGGING))
#define MOZ_REFCOUNTED_LEAK_CHECKING
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
#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
class RefCountLogger
{
public:
  static void logAddRef(const void* aPointer, MozRefCountType aRefCount,
                        const char* aTypeName, uint32_t aInstanceSize)
  {
    MOZ_ASSERT(aRefCount != DEAD);
    NS_LogAddRef(const_cast<void*>(aPointer), aRefCount, aTypeName,
                 aInstanceSize);
  }

  static void logRelease(const void* aPointer, MozRefCountType aRefCount,
                         const char* aTypeName)
  {
    MOZ_ASSERT(aRefCount != DEAD);
    NS_LogRelease(const_cast<void*>(aPointer), aRefCount, aTypeName);
  }
};
#endif

// This is used WeakPtr.h as well as this file.
enum RefCountAtomicity
{
  AtomicRefCount,
  NonAtomicRefCount
};

template<typename T, RefCountAtomicity Atomicity>
class RC
{
public:
  explicit RC(T aCount) : mValue(aCount) {}

  T operator++() { return ++mValue; }
  T operator--() { return --mValue; }

  void operator=(const T& aValue) { mValue = aValue; }

  operator T() const { return mValue; }

private:
  T mValue;
};

template<typename T>
class RC<T, AtomicRefCount>
{
public:
  explicit RC(T aCount) : mValue(aCount) {}

  T operator++()
  {
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

  T operator--()
  {
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
      std::atomic_thread_fence(std::memory_order_acquire);
    }
    return result;
  }

  // This method is only called in debug builds, so we're not too concerned
  // about its performance.
  void operator=(const T& aValue) { mValue.store(aValue, std::memory_order_seq_cst); }

  operator T() const
  {
    // Use acquire semantics since we're not sure what the caller is
    // doing.
    return mValue.load(std::memory_order_acquire);
  }

private:
  std::atomic<T> mValue;
};

template<typename T, RefCountAtomicity Atomicity>
class RefCounted
{
protected:
  RefCounted() : mRefCnt(0) {}
  ~RefCounted() { MOZ_ASSERT(mRefCnt == detail::DEAD); }

public:
  // Compatibility with nsRefPtr.
  void AddRef() const
  {
    // Note: this method must be thread safe for AtomicRefCounted.
    MOZ_ASSERT(int32_t(mRefCnt) >= 0);
#ifndef MOZ_REFCOUNTED_LEAK_CHECKING
    ++mRefCnt;
#else
    const char* type = static_cast<const T*>(this)->typeName();
    uint32_t size = static_cast<const T*>(this)->typeSize();
    const void* ptr = static_cast<const T*>(this);
    MozRefCountType cnt = ++mRefCnt;
    detail::RefCountLogger::logAddRef(ptr, cnt, type, size);
#endif
  }

  void Release() const
  {
    // Note: this method must be thread safe for AtomicRefCounted.
    MOZ_ASSERT(int32_t(mRefCnt) > 0);
#ifndef MOZ_REFCOUNTED_LEAK_CHECKING
    MozRefCountType cnt = --mRefCnt;
#else
    const char* type = static_cast<const T*>(this)->typeName();
    const void* ptr = static_cast<const T*>(this);
    MozRefCountType cnt = --mRefCnt;
    // Note: it's not safe to touch |this| after decrementing the refcount,
    // except for below.
    detail::RefCountLogger::logRelease(ptr, cnt, type);
#endif
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
  bool hasOneRef() const
  {
    MOZ_ASSERT(mRefCnt > 0);
    return mRefCnt == 1;
  }

private:
  mutable RC<MozRefCountType, Atomicity> mRefCnt;
};

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
// Passing override for the optional argument marks the typeName and
// typeSize functions defined by this macro as overrides.
#define MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(T, ...) \
  virtual const char* typeName() const __VA_ARGS__ { return #T; } \
  virtual size_t typeSize() const __VA_ARGS__ { return sizeof(*this); }
#else
#define MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(T, ...)
#endif

// Note that this macro is expanded unconditionally because it declares only
// two small inline functions which will hopefully get eliminated by the linker
// in non-leak-checking builds.
#define MOZ_DECLARE_REFCOUNTED_TYPENAME(T) \
  const char* typeName() const { return #T; } \
  size_t typeSize() const { return sizeof(*this); }

} // namespace detail

template<typename T>
class RefCounted : public detail::RefCounted<T, detail::NonAtomicRefCount>
{
public:
  ~RefCounted()
  {
    static_assert(IsBaseOf<RefCounted, T>::value,
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
template<typename T>
class AtomicRefCounted :
  public mozilla::detail::RefCounted<T, mozilla::detail::AtomicRefCount>
{
public:
  ~AtomicRefCounted()
  {
    static_assert(IsBaseOf<AtomicRefCounted, T>::value,
                  "T must derive from AtomicRefCounted<T>");
  }
};

} // namespace external

} // namespace mozilla

#endif // mozilla_RefCounted_h
