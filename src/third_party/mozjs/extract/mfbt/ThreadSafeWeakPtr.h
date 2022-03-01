/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A thread-safe weak pointer */

/**
 * Derive from SupportsThreadSafeWeakPtr to allow thread-safe weak pointers to
 * an atomically refcounted derived class. These thread-safe weak pointers may
 * be safely accessed and converted to strong pointers on multiple threads.
 *
 * Note that SupportsThreadSafeWeakPtr defines the same member functions as
 * AtomicRefCounted, so you should not separately inherit from it.
 *
 * ThreadSafeWeakPtr and its implementation is distinct from the normal WeakPtr
 * which is not thread-safe. The interface discipline and implementation details
 * are different enough that these two implementations are separated for now for
 * efficiency reasons. If you don't actually need to use weak pointers on
 * multiple threads, you can just use WeakPtr instead.
 *
 * When deriving from SupportsThreadSafeWeakPtr, you should add
 * MOZ_DECLARE_THREADSAFEWEAKREFERENCE_TYPENAME(ClassName) and
 * MOZ_DECLARE_REFCOUNTED_TYPENAME(ClassName) to the public section of your
 * class, where ClassName is the name of your class.
 *
 * Example usage:
 *
 *   class C : public SupportsThreadSafeWeakPtr<C>
 *   {
 *   public:
 *     MOZ_DECLARE_THREADSAFEWEAKREFERENCE_TYPENAME(C)
 *     MOZ_DECLARE_REFCOUNTED_TYPENAME(C)
 *     void doStuff();
 *   };
 *
 *   ThreadSafeWeakPtr<C> weak;
 *   {
 *     RefPtr<C> strong = new C;
 *     if (strong) {
 *       strong->doStuff();
 *     }
 *     // Make a new weak reference to the object from the strong reference.
 *     weak = strong;
 *   }
 *   MOZ_ASSERT(!bool(weak), "Weak pointers are cleared after all "
 *                           "strong references are released.");
 *
 *   // Convert the weak reference to a strong reference for usage.
 *   RefPtr<C> other(weak);
 *   if (other) {
 *     other->doStuff();
 *   }
 */

#ifndef mozilla_ThreadSafeWeakPtr_h
#define mozilla_ThreadSafeWeakPtr_h

#include "mozilla/Assertions.h"
#include "mozilla/RefCounted.h"
#include "mozilla/RefPtr.h"

namespace mozilla {

template <typename T>
class ThreadSafeWeakPtr;
template <typename T>
class SupportsThreadSafeWeakPtr;

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
#  define MOZ_DECLARE_THREADSAFEWEAKREFERENCE_TYPENAME(T)  \
    static const char* threadSafeWeakReferenceTypeName() { \
      return "ThreadSafeWeakReference<" #T ">";            \
    }
#else
#  define MOZ_DECLARE_THREADSAFEWEAKREFERENCE_TYPENAME(T)
#endif

namespace detail {

// A shared weak reference that is used to track a SupportsThreadSafeWeakPtr
// object. This object owns the reference count for the tracked object, and can
// perform atomic refcount upgrades.
template <typename T>
class ThreadSafeWeakReference
    : public external::AtomicRefCounted<ThreadSafeWeakReference<T>> {
 public:
  typedef T ElementType;

  explicit ThreadSafeWeakReference(T* aPtr) : mPtr(aPtr) {}

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
  const char* typeName() const {
    // The first time this is called mPtr is null, so don't
    // invoke any methods on mPtr.
    return T::threadSafeWeakReferenceTypeName();
  }
  size_t typeSize() const { return sizeof(*this); }
#endif

 private:
  friend class mozilla::SupportsThreadSafeWeakPtr<T>;
  template <typename U>
  friend class mozilla::ThreadSafeWeakPtr;

  // Creates a new RefPtr to the tracked object.
  already_AddRefed<T> getRefPtr() {
    // Increment our strong reference count only if it is nonzero, meaning that
    // the object is still alive.
    MozRefCountType cnt = mStrongCnt.IncrementIfNonzero();
    if (cnt == 0) {
      return nullptr;
    }

    RefPtr<T> result{already_AddRefed(mPtr)};
    detail::RefCountLogger::logAddRef(result.get(), cnt);
    return result.forget();
  }

  // Number of strong references to the underlying data structure.
  //
  // Other than the initial strong `AddRef` call incrementing this value to 1,
  // which must occur before any weak references are taken, once this value
  // reaches `0` again it cannot be changed.
  RC<MozRefCountType, AtomicRefCount> mStrongCnt{0};

  // Raw pointer to the tracked object. It is never valid to read this value
  // outside of `getRefPtr()`.
  T* MOZ_NON_OWNING_REF mPtr;
};

}  // namespace detail

// For usage documentation for SupportsThreadSafeWeakPtr, see the header-level
// documentation.
//
// To understand the layout of SupportsThreadSafeWeakPtr, consider the following
// simplified declaration:
//
// class MyType: SupportsThreadSafeWeakPtr { uint32_t mMyData; ... }
//
// Which will result in the following layout:
//
//   +--------------------+
//   | MyType             | <========================================+
//   +--------------------+                                          I
//   | RefPtr mWeakRef  o===========> +-------------------------+    I
//   | uint32_t mMyData   |           | ThreadSafeWeakReference |    I
//   +--------------------+           +-------------------------+    I
//                                    | RC mRefCount            |    I
//                                    | RC mStrongCount         |    I
//                                    | MyType* mPtr          o======+
//                                    +-------------------------+
//
// The mRefCount inherited from AtomicRefCounted<ThreadSafeWeakReference<T>> is
// the weak count. This means MyType implicitly holds a weak reference, so if
// the weak count ever hits 0, we know all strong *and* weak references are
// gone, and it's safe to free the ThreadSafeWeakReference. MyType's AddRef and
// Release implementations otherwise only manipulate mStrongCount.
//
// It's necessary to keep the counts in a separate allocation because we need
// to be able to delete MyType while weak references still exist. This ensures
// that weak references can still access all the state necessary to check if
// they can be upgraded (mStrongCount).
template <typename T>
class SupportsThreadSafeWeakPtr {
 protected:
  typedef detail::ThreadSafeWeakReference<T> ThreadSafeWeakReference;

  // The `this` pointer will not have subclasses initialized yet, but it will
  // also not be read until a weak pointer is upgraded, which should be after
  // this point.
  SupportsThreadSafeWeakPtr()
      : mWeakRef(new ThreadSafeWeakReference(static_cast<T*>(this))) {
    static_assert(std::is_base_of_v<SupportsThreadSafeWeakPtr<T>, T>,
                  "T must derive from SupportsThreadSafeWeakPtr<T>");
  }

 public:
  // Compatibility with RefPtr
  void AddRef() const {
    auto& refCnt = mWeakRef->mStrongCnt;
    MOZ_ASSERT(int32_t(refCnt) >= 0);
    MozRefCountType cnt = ++refCnt;
    detail::RefCountLogger::logAddRef(static_cast<const T*>(this), cnt);
  }

  void Release() const {
    auto& refCnt = mWeakRef->mStrongCnt;
    MOZ_ASSERT(int32_t(refCnt) > 0);
    detail::RefCountLogger::ReleaseLogger logger(static_cast<const T*>(this));
    MozRefCountType cnt = --refCnt;
    logger.logRelease(cnt);
    if (0 == cnt) {
      // Because we have atomically decremented the refcount above, only one
      // thread can get a 0 count here. Thus, it is safe to access and destroy
      // |this| here.
      // No other thread can acquire a strong reference to |this| anymore
      // through our weak pointer, as upgrading a weak pointer always uses
      // |IncrementIfNonzero|, meaning the refcount can't leave a zero reference
      // state.
      // NOTE: We can't update our refcount to the marker `DEAD` value here, as
      // it may still be read by mWeakRef.
      delete static_cast<const T*>(this);
    }
  }

  // Compatibility with wtf::RefPtr
  void ref() { AddRef(); }
  void deref() { Release(); }
  MozRefCountType refCount() const { return mWeakRef->mStrongCnt; }

 private:
  template <typename U>
  friend class ThreadSafeWeakPtr;

  ThreadSafeWeakReference* getThreadSafeWeakReference() const {
    return mWeakRef;
  }

  const RefPtr<ThreadSafeWeakReference> mWeakRef;
};

// A thread-safe variant of a weak pointer
template <typename T>
class ThreadSafeWeakPtr {
  // Be careful to use the weak reference type T in the
  // SupportsThreadSafeWeakPtr<T> definition.
  typedef typename T::ThreadSafeWeakReference ThreadSafeWeakReference;

 public:
  ThreadSafeWeakPtr() = default;

  ThreadSafeWeakPtr& operator=(const ThreadSafeWeakPtr& aOther) = default;
  ThreadSafeWeakPtr(const ThreadSafeWeakPtr& aOther) = default;

  ThreadSafeWeakPtr& operator=(ThreadSafeWeakPtr&& aOther) = default;
  ThreadSafeWeakPtr(ThreadSafeWeakPtr&& aOther) = default;

  ThreadSafeWeakPtr& operator=(const RefPtr<T>& aOther) {
    if (aOther) {
      // Get the underlying shared weak reference to the object.
      mRef = aOther->getThreadSafeWeakReference();
    } else {
      mRef = nullptr;
    }
    return *this;
  }

  explicit ThreadSafeWeakPtr(const RefPtr<T>& aOther) { *this = aOther; }

  ThreadSafeWeakPtr& operator=(decltype(nullptr)) {
    mRef = nullptr;
    return *this;
  }

  explicit ThreadSafeWeakPtr(decltype(nullptr)) {}

  // Use the explicit `IsNull()` or `IsDead()` methods instead.
  explicit operator bool() const = delete;

  // Check if the ThreadSafeWeakPtr was created wrapping a null pointer.
  bool IsNull() const { return !mRef; }

  // Check if the managed object is nullptr or has already been destroyed. Once
  // IsDead returns true, this ThreadSafeWeakPtr can never be upgraded again
  // (until it has been re-assigned), but a false return value does NOT imply
  // that any future upgrade will be successful.
  bool IsDead() const { return IsNull() || size_t(mRef->mStrongCnt) == 0; }

  bool operator==(const ThreadSafeWeakPtr& aOther) const {
    return mRef == aOther.mRef;
  }

  bool operator==(const RefPtr<T>& aOther) const {
    return *this == aOther.get();
  }

  bool operator==(const T* aOther) const {
    if (!mRef) {
      return !aOther;
    }
    return aOther && aOther->getThreadSafeWeakReference() == mRef;
  }

  template <typename U>
  bool operator!=(const U& aOther) const {
    return !(*this == aOther);
  }

  // Convert the weak pointer to a strong RefPtr.
  explicit operator RefPtr<T>() const { return getRefPtr(); }

 private:
  // Gets a new strong reference of the proper type T to the tracked object.
  already_AddRefed<T> getRefPtr() const {
    static_assert(std::is_base_of<typename ThreadSafeWeakReference::ElementType,
                                  T>::value,
                  "T must derive from ThreadSafeWeakReference::ElementType");
    return mRef ? mRef->getRefPtr().template downcast<T>() : nullptr;
  }

  // A shared weak reference to an object. Note that this may be null so as to
  // save memory (at the slight cost of an extra null check) if no object is
  // being tracked.
  RefPtr<ThreadSafeWeakReference> mRef;
};

}  // namespace mozilla

template <typename T>
inline already_AddRefed<T> do_AddRef(
    const mozilla::ThreadSafeWeakPtr<T>& aObj) {
  RefPtr<T> ref(aObj);
  return ref.forget();
}

#endif /* mozilla_ThreadSafeWeakPtr_h */
