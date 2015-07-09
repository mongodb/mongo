/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Helpers for defining and using refcounted objects. */

#ifndef mozilla_RefPtr_h
#define mozilla_RefPtr_h

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/RefCountType.h"
#include "mozilla/TypeTraits.h"
#if defined(MOZILLA_INTERNAL_API)
#include "nsXPCOM.h"
#endif

#if defined(MOZILLA_INTERNAL_API) && \
    (defined(DEBUG) || defined(FORCE_BUILD_REFCNT_LOGGING))
#define MOZ_REFCOUNTED_LEAK_CHECKING
#endif

namespace mozilla {

template<typename T> class RefCounted;
template<typename T> class RefPtr;
template<typename T> class TemporaryRef;
template<typename T> class OutParamRef;
template<typename T> OutParamRef<T> byRef(RefPtr<T>&);

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
 */
namespace detail {
#ifdef DEBUG
const MozRefCountType DEAD = 0xffffdead;
#endif

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
class RefCounted
{
  friend class RefPtr<T>;

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
  mutable typename Conditional<Atomicity == AtomicRefCount,
                               Atomic<MozRefCountType>,
                               MozRefCountType>::Type mRefCnt;
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

/**
 * RefPtr points to a refcounted thing that has AddRef and Release
 * methods to increase/decrease the refcount, respectively.  After a
 * RefPtr<T> is assigned a T*, the T* can be used through the RefPtr
 * as if it were a T*.
 *
 * A RefPtr can forget its underlying T*, which results in the T*
 * being wrapped in a temporary object until the T* is either
 * re-adopted from or released by the temporary.
 */
template<typename T>
class RefPtr
{
  // To allow them to use unref()
  friend class TemporaryRef<T>;
  friend class OutParamRef<T>;

  struct DontRef {};

public:
  RefPtr() : mPtr(0) {}
  RefPtr(const RefPtr& aOther) : mPtr(ref(aOther.mPtr)) {}
  MOZ_IMPLICIT RefPtr(const TemporaryRef<T>& aOther) : mPtr(aOther.take()) {}
  MOZ_IMPLICIT RefPtr(already_AddRefed<T>& aOther) : mPtr(aOther.take()) {}
  MOZ_IMPLICIT RefPtr(T* aVal) : mPtr(ref(aVal)) {}

  template<typename U>
  RefPtr(const RefPtr<U>& aOther) : mPtr(ref(aOther.get())) {}

  ~RefPtr() { unref(mPtr); }

  RefPtr& operator=(const RefPtr& aOther)
  {
    assign(ref(aOther.mPtr));
    return *this;
  }
  RefPtr& operator=(const TemporaryRef<T>& aOther)
  {
    assign(aOther.take());
    return *this;
  }
  RefPtr& operator=(already_AddRefed<T>& aOther)
  {
    assign(aOther.take());
    return *this;
  }
  RefPtr& operator=(T* aVal)
  {
    assign(ref(aVal));
    return *this;
  }

  template<typename U>
  RefPtr& operator=(const RefPtr<U>& aOther)
  {
    assign(ref(aOther.get()));
    return *this;
  }

  TemporaryRef<T> forget()
  {
    T* tmp = mPtr;
    mPtr = nullptr;
    return TemporaryRef<T>(tmp, DontRef());
  }

  T* get() const { return mPtr; }
  operator T*() const { return mPtr; }
  T* operator->() const MOZ_NO_ADDREF_RELEASE_ON_RETURN { return mPtr; }
  T& operator*() const { return *mPtr; }
  template<typename U>
  operator TemporaryRef<U>() { return TemporaryRef<U>(mPtr); }

private:
  void assign(T* aVal)
  {
    unref(mPtr);
    mPtr = aVal;
  }

  T* MOZ_OWNING_REF mPtr;

  static MOZ_ALWAYS_INLINE T* ref(T* aVal)
  {
    if (aVal) {
      aVal->AddRef();
    }
    return aVal;
  }

  static MOZ_ALWAYS_INLINE void unref(T* aVal)
  {
    if (aVal) {
      aVal->Release();
    }
  }
};

/**
 * TemporaryRef<T> represents an object that holds a temporary
 * reference to a T.  TemporaryRef objects can't be manually ref'd or
 * unref'd (being temporaries, not lvalues), so can only relinquish
 * references to other objects, or unref on destruction.
 */
template<typename T>
class TemporaryRef
{
  // To allow it to construct TemporaryRef from a bare T*
  friend class RefPtr<T>;

  typedef typename RefPtr<T>::DontRef DontRef;

public:
  MOZ_IMPLICIT TemporaryRef(T* aVal) : mPtr(RefPtr<T>::ref(aVal)) {}
  TemporaryRef(const TemporaryRef& aOther) : mPtr(aOther.take()) {}

  template<typename U>
  TemporaryRef(const TemporaryRef<U>& aOther) : mPtr(aOther.take()) {}

  ~TemporaryRef() { RefPtr<T>::unref(mPtr); }

  MOZ_WARN_UNUSED_RESULT T* take() const
  {
    T* tmp = mPtr;
    mPtr = nullptr;
    return tmp;
  }

private:
  TemporaryRef(T* aVal, const DontRef&) : mPtr(aVal) {}

  mutable T* MOZ_OWNING_REF mPtr;

  TemporaryRef() = delete;
  void operator=(const TemporaryRef&) = delete;
};

/**
 * OutParamRef is a wrapper that tracks a refcounted pointer passed as
 * an outparam argument to a function.  OutParamRef implements COM T**
 * outparam semantics: this requires the callee to AddRef() the T*
 * returned through the T** outparam on behalf of the caller.  This
 * means the caller (through OutParamRef) must Release() the old
 * object contained in the tracked RefPtr.  It's OK if the callee
 * returns the same T* passed to it through the T** outparam, as long
 * as the callee obeys the COM discipline.
 *
 * Prefer returning TemporaryRef<T> from functions over creating T**
 * outparams and passing OutParamRef<T> to T**.  Prefer RefPtr<T>*
 * outparams over T** outparams.
 */
template<typename T>
class OutParamRef
{
  friend OutParamRef byRef<T>(RefPtr<T>&);

public:
  ~OutParamRef()
  {
    RefPtr<T>::unref(mRefPtr.mPtr);
    mRefPtr.mPtr = mTmp;
  }

  operator T**() { return &mTmp; }

private:
  explicit OutParamRef(RefPtr<T>& p) : mRefPtr(p), mTmp(p.get()) {}

  RefPtr<T>& mRefPtr;
  T* mTmp;

  OutParamRef() = delete;
  OutParamRef& operator=(const OutParamRef&) = delete;
};

/**
 * byRef cooperates with OutParamRef to implement COM outparam semantics.
 */
template<typename T>
OutParamRef<T>
byRef(RefPtr<T>& aPtr)
{
  return OutParamRef<T>(aPtr);
}

} // namespace mozilla

#endif /* mozilla_RefPtr_h */
