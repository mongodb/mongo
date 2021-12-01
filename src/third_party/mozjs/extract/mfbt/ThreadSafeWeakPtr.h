/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A thread-safe weak pointer */

/**
 * Derive from SupportsThreadSafeWeakPtr to allow thread-safe weak pointers to an
 * atomically refcounted derived class. These thread-safe weak pointers may be safely
 * accessed and converted to strong pointers on multiple threads.
 *
 * Note that SupportsThreadSafeWeakPtr necessarily already inherits from AtomicRefCounted,
 * so you should not separately inherit from AtomicRefCounted.
 *
 * ThreadSafeWeakPtr and its implementation is distinct from the normal WeakPtr which is
 * not thread-safe. The interface discipline and implementation details are different enough
 * that these two implementations are separated for now for efficiency reasons. If you don't
 * actually need to use weak pointers on multiple threads, you can just use WeakPtr instead.
 *
 * When deriving from SupportsThreadSafeWeakPtr, you should add
 * MOZ_DECLARE_THREADSAFEWEAKREFERENCE_TYPENAME(ClassName) and
 * MOZ_DECLARE_REFCOUNTED_TYPENAME(ClassName) to the public section of your class,
 * where ClassName is the name of your class.
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
 *   MOZ_ASSERT(!bool(weak), "Weak pointers are cleared after all strong references are released.");
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
#include "mozilla/Atomics.h"
#include "mozilla/RefCounted.h"
#include "mozilla/RefPtr.h"
#include "mozilla/TypeTraits.h"
#include "mozilla/Unused.h"

#include <limits>

namespace mozilla {

template<typename T> class ThreadSafeWeakPtr;
template<typename T> class SupportsThreadSafeWeakPtr;

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
  #define MOZ_DECLARE_THREADSAFEWEAKREFERENCE_TYPENAME(T) \
    static const char* threadSafeWeakReferenceTypeName() { return "ThreadSafeWeakReference<" #T ">"; }
#else
  #define MOZ_DECLARE_THREADSAFEWEAKREFERENCE_TYPENAME(T)
#endif

namespace detail {

// A multiple reader, single writer spin-lock.
// This lock maintains an atomic counter which is incremented every time the lock is acquired
// reading. So long as the counter remains positive, it may be incremented for reading multiple
// times. When acquiring the lock for writing, we must ensure the counter is 0 (no readers),
// and if so, set it to a negative value to indicate that no new readers may take the lock.
class ReadWriteSpinLock
{
  // Only need a type large enough to represent the number of simultaneously accessing threads.
  typedef int32_t CounterType;

public:
  // Try to increment the counter for reading, so long as it is positive.
  void readLock()
  {
    for (;;)
    {
      CounterType oldCounter = mCounter & std::numeric_limits<CounterType>::max();
      CounterType newCounter = oldCounter + 1;
      if (mCounter.compareExchange(oldCounter, newCounter)) {
        break;
      }
    }
  }

  // Decrement the counter to remove a read lock.
  void readUnlock()
  {
    mCounter--;
  }

  // Try to acquire the write lock, but only if there are no readers.
  // If successful, sets the counter to a negative value.
  bool tryWriteLock()
  {
    return mCounter.compareExchange(0, std::numeric_limits<CounterType>::min());
  }

  // Reset the counter to 0.
  void writeUnlock()
  {
    mCounter = 0;
  }

private:
  Atomic<CounterType> mCounter;
};

// A shared weak reference that is used to track a SupportsThreadSafeWeakPtr object.
// It guards access to that object via a read-write spinlock.
template<typename T>
class ThreadSafeWeakReference : public external::AtomicRefCounted<ThreadSafeWeakReference<T>>
{
public:
  typedef T ElementType;

  explicit ThreadSafeWeakReference(T* aPtr)
  {
    mPtr = aPtr;
  }

#ifdef MOZ_REFCOUNTED_LEAK_CHECKING
  const char* typeName() const
  {
    // The first time this is called mPtr is null, so don't
    // invoke any methods on mPtr.
    return T::threadSafeWeakReferenceTypeName();
  }
  size_t typeSize() const { return sizeof(*this); }
#endif

private:
  friend class mozilla::SupportsThreadSafeWeakPtr<T>;
  template<typename U> friend class mozilla::ThreadSafeWeakPtr;

  // Does an unsafe read of the raw weak pointer.
  T* get() const
  {
    return mPtr;
  }

  // Creates a new RefPtr to the tracked object.
  // We need to acquire the read lock while we do this, as we need to atomically
  // both read the pointer and then increment the refcount on it within the scope
  // of the lock. This guards against the object being destroyed while in the middle
  // of creating the new RefPtr.
  already_AddRefed<T> getRefPtr()
  {
    mLock.readLock();
    RefPtr<T> result(get());
    mLock.readUnlock();
    return result.forget();
  }

  // Try to detach the weak reference from the tracked object.
  // We need to acquire the write lock while we do this, to ensure that no
  // RefPtr is created to this while detaching. Once acquired, it is safe
  // to check the refcount and verify that this is the last reference to
  // the tracked object, so the weak reference can be safely detached.
  void tryDetach(const SupportsThreadSafeWeakPtr<T>* aOwner)
  {
    if (mLock.tryWriteLock()) {
      if (aOwner->hasOneRef()) {
        mPtr = nullptr;
      }
      mLock.writeUnlock();
    }
  }

  ReadWriteSpinLock mLock;
  Atomic<T*> mPtr;
};

} // namespace detail

template<typename T>
class SupportsThreadSafeWeakPtr : public external::AtomicRefCounted<T>
{
protected:
  typedef external::AtomicRefCounted<T> AtomicRefCounted;
  typedef detail::ThreadSafeWeakReference<T> ThreadSafeWeakReference;

public:
  ~SupportsThreadSafeWeakPtr()
  {
    // Clean up the shared weak reference if one exists.
    if (ThreadSafeWeakReference* ptr = mRef) {
      ptr->Release();
    }
  }

  void Release() const
  {
    // If there is only one remaining reference to the object when trying to release,
    // then attempt to detach it from its weak reference. New references could possibly
    // be created to the object while this happens, so take care to do this atomically
    // inside tryDetach.
    if (AtomicRefCounted::hasOneRef()) {
      if (ThreadSafeWeakReference* ptr = mRef) {
        ptr->tryDetach(this);
      }
    }

    // Once possibly detached, it is now safe to continue to decrement the refcount.
    AtomicRefCounted::Release();
  }

private:
  template<typename U> friend class ThreadSafeWeakPtr;

  // Creates a shared weak reference for the object if one does not exist. Note that the
  // object may be of an actual derived type U, but the weak reference is created for the
  // supplied type T of SupportsThreadSafeWeakPtr<T>.
  already_AddRefed<ThreadSafeWeakReference> getThreadSafeWeakReference()
  {
    static_assert(IsBaseOf<SupportsThreadSafeWeakPtr<T>, T>::value,
                  "T must derive from SupportsThreadSafeWeakPtr<T>");

    if (!mRef) {
      RefPtr<ThreadSafeWeakReference> ptr(new ThreadSafeWeakReference(static_cast<T*>(this)));
      // Only set the new weak reference if one does not exist (== nullptr).
      // If there is already a weak reference, just let this superflous weak reference get
      // destroyed when it goes out of scope.
      if (mRef.compareExchange(nullptr, ptr)) {
        // If successful, forget the refcount so that the weak reference stays alive.
        Unused << ptr.forget();
      }
    }

    // Create a new RefPtr to weak reference.
    RefPtr<ThreadSafeWeakReference> ptr(mRef);
    return ptr.forget();
  }

  Atomic<ThreadSafeWeakReference*> mRef;
};

// A thread-safe variant of a weak pointer
template<typename T>
class ThreadSafeWeakPtr
{
  // Be careful to use the weak reference type T in the SupportsThreadSafeWeakPtr<T> definition.
  typedef typename T::ThreadSafeWeakReference ThreadSafeWeakReference;

public:
  ThreadSafeWeakPtr()
  {}

  ThreadSafeWeakPtr& operator=(const ThreadSafeWeakPtr& aOther)
  {
    mRef = aOther.mRef;
    return *this;
  }

  ThreadSafeWeakPtr(const ThreadSafeWeakPtr& aOther)
    : mRef(aOther.mRef)
  {
  }

  ThreadSafeWeakPtr& operator=(ThreadSafeWeakPtr&& aOther)
  {
    mRef = aOther.mRef.forget();
    return *this;
  }

  ThreadSafeWeakPtr(ThreadSafeWeakPtr&& aOther)
    : mRef(aOther.mRef.forget())
  {
  }

  ThreadSafeWeakPtr& operator=(const RefPtr<T>& aOther)
  {
    if (aOther) {
      // Get the underlying shared weak reference to the object, creating one if necessary.
      mRef = aOther->getThreadSafeWeakReference();
    } else {
      mRef = nullptr;
    }
    return *this;
  }

  explicit ThreadSafeWeakPtr(const RefPtr<T>& aOther)
  {
    *this = aOther;
  }

  ThreadSafeWeakPtr& operator=(decltype(nullptr))
  {
    mRef = nullptr;
    return *this;
  }

  explicit ThreadSafeWeakPtr(decltype(nullptr))
  {}

  explicit operator bool() const
  {
    return !!get();
  }

  bool operator==(const ThreadSafeWeakPtr& aOther) const
  {
    return get() == aOther.get();
  }

  bool operator==(const RefPtr<T>& aOther) const
  {
    return get() == aOther.get();
  }

  bool operator==(const T* aOther) const
  {
    return get() == aOther;
  }

  template<typename U>
  bool operator!=(const U& aOther) const
  {
    return !(*this == aOther);
  }

  // Convert the weak pointer to a strong RefPtr.
  explicit operator RefPtr<T>() const
  {
    return getRefPtr();
  }

private:
  // Gets a new strong reference of the proper type T to the tracked object.
  already_AddRefed<T> getRefPtr() const
  {
    static_assert(IsBaseOf<typename ThreadSafeWeakReference::ElementType, T>::value,
                  "T must derive from ThreadSafeWeakReference::ElementType");
    return mRef ? mRef->getRefPtr().template downcast<T>() : nullptr;
  }

  // Get a pointer to the tracked object, downcasting to the proper type T.
  // Note that this operation is unsafe as it may cause races if downwind
  // code depends on the value not to change after reading.
  T* get() const
  {
    static_assert(IsBaseOf<typename ThreadSafeWeakReference::ElementType, T>::value,
                  "T must derive from ThreadSafeWeakReference::ElementType");
    return mRef ? static_cast<T*>(mRef->get()) : nullptr;
  }

  // A shared weak reference to an object. Note that this may be null so as to save memory
  // (at the slight cost of an extra null check) if no object is being tracked.
  RefPtr<ThreadSafeWeakReference> mRef;
};

} // namespace mozilla

template<typename T>
inline already_AddRefed<T>
do_AddRef(const mozilla::ThreadSafeWeakPtr<T>& aObj)
{
  RefPtr<T> ref(aObj);
  return ref.forget();
}

#endif /* mozilla_ThreadSafeWeakPtr_h */

