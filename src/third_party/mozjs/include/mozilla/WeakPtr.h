/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Weak pointer functionality, implemented as a mixin for use with any class. */

/**
 * SupportsWeakPtr lets you have a pointer to an object 'Foo' without affecting
 * its lifetime. It works by creating a single shared reference counted object
 * (WeakReference) that each WeakPtr will access 'Foo' through. This lets 'Foo'
 * clear the pointer in the WeakReference without having to know about all of
 * the WeakPtrs to it and allows the WeakReference to live beyond the lifetime
 * of 'Foo'.
 *
 * PLEASE NOTE: This weak pointer implementation is not thread-safe.
 *
 * The overhead of WeakPtr is that accesses to 'Foo' becomes an additional
 * dereference, and an additional heap allocated pointer sized object shared
 * between all of the WeakPtrs.
 *
 * Example of usage:
 *
 *   // To have a class C support weak pointers, inherit from
 *   // SupportsWeakPtr
 *   class C : public SupportsWeakPtr
 *   {
 *   public:
 *     int mNum;
 *     void act();
 *   };
 *
 *   C* ptr = new C();
 *
 *   // Get weak pointers to ptr. The first time a weak pointer
 *   // is obtained, a reference counted WeakReference object is created that
 *   // can live beyond the lifetime of 'ptr'. The WeakReference
 *   // object will be notified of 'ptr's destruction.
 *   WeakPtr<C> weak = ptr;
 *   WeakPtr<C> other = ptr;
 *
 *   // Test a weak pointer for validity before using it.
 *   if (weak) {
 *     weak->mNum = 17;
 *     weak->act();
 *   }
 *
 *   // Destroying the underlying object clears weak pointers to it.
 *   delete ptr;
 *
 *   MOZ_ASSERT(!weak, "Deleting |ptr| clears weak pointers to it.");
 *   MOZ_ASSERT(!other, "Deleting |ptr| clears all weak pointers to it.");
 *
 * WeakPtr is typesafe and may be used with any class. It is not required that
 * the class be reference-counted or allocated in any particular way.
 *
 * The API was loosely inspired by Chromium's weak_ptr.h:
 * http://src.chromium.org/svn/trunk/src/base/memory/weak_ptr.h
 *
 * Note that multiple base classes inheriting from SupportsWeakPtr is not
 * currently supported. We could support it if needed though.
 *
 * For Gecko-internal usage there is also MainThreadWeakPtr<T>, a version of
 * WeakPtr that can be destroyed on any thread, but whose release gets proxied
 * to the main thread. This is a similar API to nsMainThreadPtrHandle, but
 * without keeping a strong reference to the main-thread object. Said WeakPtr
 * can't be accessed from any other thread that isn't the main thread.
 */

#ifndef mozilla_WeakPtr_h
#define mozilla_WeakPtr_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/RefCounted.h"
#include "mozilla/RefPtr.h"

#include <string.h>
#include <type_traits>

#if defined(MOZILLA_INTERNAL_API)
// For thread safety checking.
#  include "nsISupportsImpl.h"
// For main thread destructor behavior.
#  include "nsProxyRelease.h"
#endif

#if defined(MOZILLA_INTERNAL_API) && \
    defined(MOZ_THREAD_SAFETY_OWNERSHIP_CHECKS_SUPPORTED)

// Weak referencing is not implemented as thread safe.  When a WeakPtr
// is created or dereferenced on thread A but the real object is just
// being Released() on thread B, there is a possibility of a race
// when the proxy object (detail::WeakReference) is notified about
// the real object destruction just between when thread A is storing
// the object pointer locally and is about to add a reference to it.
//
// Hence, a non-null weak proxy object is considered to have a single
// "owning thread".  It means that each query for a weak reference,
// its dereference, and destruction of the real object must all happen
// on a single thread.  The following macros implement assertions for
// checking these conditions.
//
// We re-use XPCOM's nsAutoOwningEventTarget checks when they are available.
// This has the advantage that it works with cooperative thread pools.

#  define MOZ_WEAKPTR_DECLARE_THREAD_SAFETY_CHECK \
    /* Will be none if mPtr = nullptr. */         \
    Maybe<nsAutoOwningEventTarget> _owningThread;
#  define MOZ_WEAKPTR_INIT_THREAD_SAFETY_CHECK() \
    do {                                         \
      if (p) {                                   \
        _owningThread.emplace();                 \
      }                                          \
    } while (false)
#  define MOZ_WEAKPTR_ASSERT_THREAD_SAFETY()                  \
    do {                                                      \
      MOZ_DIAGNOSTIC_ASSERT(                                  \
          !_owningThread || _owningThread->IsCurrentThread(), \
          "WeakPtr accessed from multiple threads");          \
    } while (false)
#  define MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED(that) \
    (that)->AssertThreadSafety();
#  define MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED_IF(that) \
    do {                                                      \
      if (that) {                                             \
        (that)->AssertThreadSafety();                         \
      }                                                       \
    } while (false)

#  define MOZ_WEAKPTR_THREAD_SAFETY_CHECKING 1

#else

#  define MOZ_WEAKPTR_DECLARE_THREAD_SAFETY_CHECK
#  define MOZ_WEAKPTR_INIT_THREAD_SAFETY_CHECK() \
    do {                                         \
    } while (false)
#  define MOZ_WEAKPTR_ASSERT_THREAD_SAFETY() \
    do {                                     \
    } while (false)
#  define MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED(that) \
    do {                                                   \
    } while (false)
#  define MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED_IF(that) \
    do {                                                      \
    } while (false)

#endif

namespace mozilla {

namespace detail {

enum class WeakPtrDestructorBehavior {
  Normal,
#ifdef MOZILLA_INTERNAL_API
  ProxyToMainThread,
#endif
};

}  // namespace detail

template <typename T, detail::WeakPtrDestructorBehavior =
                          detail::WeakPtrDestructorBehavior::Normal>
class WeakPtr;
class SupportsWeakPtr;

namespace detail {

// This can live beyond the lifetime of the class derived from
// SupportsWeakPtr.
class WeakReference : public ::mozilla::RefCounted<WeakReference> {
 public:
  explicit WeakReference(const SupportsWeakPtr* p)
      : mPtr(const_cast<SupportsWeakPtr*>(p)) {
    MOZ_WEAKPTR_INIT_THREAD_SAFETY_CHECK();
  }

  SupportsWeakPtr* get() const {
    MOZ_WEAKPTR_ASSERT_THREAD_SAFETY();
    return mPtr;
  }

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
  const char* typeName() const { return "WeakReference"; }
  size_t typeSize() const { return sizeof(*this); }
#endif

#ifdef MOZ_WEAKPTR_THREAD_SAFETY_CHECKING
  void AssertThreadSafety() { MOZ_WEAKPTR_ASSERT_THREAD_SAFETY(); }
#endif

 private:
  friend class mozilla::SupportsWeakPtr;

  void detach() {
    MOZ_WEAKPTR_ASSERT_THREAD_SAFETY();
    mPtr = nullptr;
  }

  SupportsWeakPtr* MOZ_NON_OWNING_REF mPtr;
  MOZ_WEAKPTR_DECLARE_THREAD_SAFETY_CHECK
};

}  // namespace detail

class SupportsWeakPtr {
  using WeakReference = detail::WeakReference;

 protected:
  ~SupportsWeakPtr() { DetachWeakPtr(); }

 protected:
  void DetachWeakPtr() {
    if (mSelfReferencingWeakReference) {
      mSelfReferencingWeakReference->detach();
    }
  }

 private:
  WeakReference* SelfReferencingWeakReference() const {
    if (!mSelfReferencingWeakReference) {
      mSelfReferencingWeakReference = new WeakReference(this);
    } else {
      MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED(mSelfReferencingWeakReference);
    }
    return mSelfReferencingWeakReference.get();
  }

  template <typename U, detail::WeakPtrDestructorBehavior>
  friend class WeakPtr;

  mutable RefPtr<WeakReference> mSelfReferencingWeakReference;
};

template <typename T, detail::WeakPtrDestructorBehavior Destruct>
class WeakPtr {
  using WeakReference = detail::WeakReference;

 public:
  WeakPtr& operator=(const WeakPtr& aOther) {
    // We must make sure the reference we have now is safe to be dereferenced
    // before we throw it away... (this can be called from a ctor)
    MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED_IF(mRef);
    // ...and make sure the new reference is used on a single thread as well.
    MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED(aOther.mRef);

    mRef = aOther.mRef;
    return *this;
  }

  WeakPtr(const WeakPtr& aOther) {
    // The thread safety check is performed inside of the operator= method.
    *this = aOther;
  }

  WeakPtr& operator=(decltype(nullptr)) {
    // We must make sure the reference we have now is safe to be dereferenced
    // before we throw it away.
    MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED_IF(mRef);
    if (!mRef || mRef->get()) {
      // Ensure that mRef is dereferenceable in the uninitialized state.
      mRef = new WeakReference(nullptr);
    }
    return *this;
  }

  WeakPtr& operator=(const T* aOther) {
    // We must make sure the reference we have now is safe to be dereferenced
    // before we throw it away.
    MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED_IF(mRef);
    if (aOther) {
      mRef = aOther->SelfReferencingWeakReference();
    } else if (!mRef || mRef->get()) {
      // Ensure that mRef is dereferenceable in the uninitialized state.
      mRef = new WeakReference(nullptr);
    }
    // The thread safety check happens inside SelfReferencingWeakPtr
    // or is initialized in the WeakReference constructor.
    return *this;
  }

  MOZ_IMPLICIT WeakPtr(T* aOther) {
    *this = aOther;
#ifdef MOZILLA_INTERNAL_API
    if (Destruct == detail::WeakPtrDestructorBehavior::ProxyToMainThread) {
      MOZ_ASSERT(NS_IsMainThread(),
                 "MainThreadWeakPtr makes no sense on non-main threads");
    }
#endif
  }

  explicit WeakPtr(const RefPtr<T>& aOther) : WeakPtr(aOther.get()) {}

  // Ensure that mRef is dereferenceable in the uninitialized state.
  WeakPtr() : mRef(new WeakReference(nullptr)) {}

  explicit operator bool() const { return mRef->get(); }
  T* get() const { return static_cast<T*>(mRef->get()); }
  operator T*() const { return get(); }
  T& operator*() const { return *get(); }
  T* operator->() const MOZ_NO_ADDREF_RELEASE_ON_RETURN { return get(); }

#ifdef MOZILLA_INTERNAL_API
  ~WeakPtr() {
    if (Destruct == detail::WeakPtrDestructorBehavior::ProxyToMainThread) {
      NS_ReleaseOnMainThread("WeakPtr::mRef", mRef.forget());
    } else {
      MOZ_WEAKPTR_ASSERT_THREAD_SAFETY_DELEGATED(mRef);
    }
  }
#endif

 private:
  friend class SupportsWeakPtr;

  explicit WeakPtr(const RefPtr<WeakReference>& aOther) : mRef(aOther) {}

  RefPtr<WeakReference> mRef;
};

#ifdef MOZILLA_INTERNAL_API

template <typename T>
using MainThreadWeakPtr =
    WeakPtr<T, detail::WeakPtrDestructorBehavior::ProxyToMainThread>;

#endif

#define NS_IMPL_CYCLE_COLLECTION_UNLINK_WEAK_PTR tmp->DetachWeakPtr();

#define NS_IMPL_CYCLE_COLLECTION_WEAK_PTR(class_, ...) \
  NS_IMPL_CYCLE_COLLECTION_CLASS(class_)               \
  NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(class_)        \
    NS_IMPL_CYCLE_COLLECTION_UNLINK(__VA_ARGS__)       \
    NS_IMPL_CYCLE_COLLECTION_UNLINK_WEAK_PTR           \
  NS_IMPL_CYCLE_COLLECTION_UNLINK_END                  \
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(class_)      \
    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(__VA_ARGS__)     \
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

#define NS_IMPL_CYCLE_COLLECTION_WEAK_PTR_INHERITED(class_, super_, ...) \
  NS_IMPL_CYCLE_COLLECTION_CLASS(class_)                                 \
  NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(class_, super_)        \
    NS_IMPL_CYCLE_COLLECTION_UNLINK(__VA_ARGS__)                         \
    NS_IMPL_CYCLE_COLLECTION_UNLINK_WEAK_PTR                             \
  NS_IMPL_CYCLE_COLLECTION_UNLINK_END                                    \
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(class_, super_)      \
    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(__VA_ARGS__)                       \
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

}  // namespace mozilla

#endif /* mozilla_WeakPtr_h */
