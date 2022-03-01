/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_RefPtr_h
#define mozilla_RefPtr_h

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/DbgMacro.h"

#include <type_traits>

/*****************************************************************************/

// template <class T> class RefPtrGetterAddRefs;

class nsQueryReferent;
class nsCOMPtr_helper;
class nsISupports;

namespace mozilla {
template <class T>
class OwningNonNull;
template <class T>
class StaticLocalRefPtr;
template <class T>
class StaticRefPtr;
#if defined(XP_WIN)
namespace mscom {
class AgileReference;
}  // namespace mscom
#endif  // defined(XP_WIN)

// Traditionally, RefPtr supports automatic refcounting of any pointer type
// with AddRef() and Release() methods that follow the traditional semantics.
//
// This traits class can be specialized to operate on other pointer types. For
// example, we specialize this trait for opaque FFI types that represent
// refcounted objects in Rust.
//
// Given the use of ConstRemovingRefPtrTraits below, U should not be a const-
// qualified type.
template <class U>
struct RefPtrTraits {
  static void AddRef(U* aPtr) { aPtr->AddRef(); }
  static void Release(U* aPtr) { aPtr->Release(); }
};

}  // namespace mozilla

template <class T>
class MOZ_IS_REFPTR RefPtr {
 private:
  void assign_with_AddRef(T* aRawPtr) {
    if (aRawPtr) {
      ConstRemovingRefPtrTraits<T>::AddRef(aRawPtr);
    }
    assign_assuming_AddRef(aRawPtr);
  }

  void assign_assuming_AddRef(T* aNewPtr) {
    T* oldPtr = mRawPtr;
    mRawPtr = aNewPtr;
    if (oldPtr) {
      ConstRemovingRefPtrTraits<T>::Release(oldPtr);
    }
  }

 private:
  T* MOZ_OWNING_REF mRawPtr;

 public:
  typedef T element_type;

  ~RefPtr() {
    if (mRawPtr) {
      ConstRemovingRefPtrTraits<T>::Release(mRawPtr);
    }
  }

  // Constructors

  RefPtr()
      : mRawPtr(nullptr)
  // default constructor
  {}

  RefPtr(const RefPtr<T>& aSmartPtr)
      : mRawPtr(aSmartPtr.mRawPtr)
  // copy-constructor
  {
    if (mRawPtr) {
      ConstRemovingRefPtrTraits<T>::AddRef(mRawPtr);
    }
  }

  RefPtr(RefPtr<T>&& aRefPtr) : mRawPtr(aRefPtr.mRawPtr) {
    aRefPtr.mRawPtr = nullptr;
  }

  // construct from a raw pointer (of the right type)

  MOZ_IMPLICIT RefPtr(T* aRawPtr) : mRawPtr(aRawPtr) {
    if (mRawPtr) {
      ConstRemovingRefPtrTraits<T>::AddRef(mRawPtr);
    }
  }

  MOZ_IMPLICIT RefPtr(decltype(nullptr)) : mRawPtr(nullptr) {}

  template <typename I,
            typename = std::enable_if_t<std::is_convertible_v<I*, T*>>>
  MOZ_IMPLICIT RefPtr(already_AddRefed<I>& aSmartPtr)
      : mRawPtr(aSmartPtr.take())
  // construct from |already_AddRefed|
  {}

  template <typename I,
            typename = std::enable_if_t<std::is_convertible_v<I*, T*>>>
  MOZ_IMPLICIT RefPtr(already_AddRefed<I>&& aSmartPtr)
      : mRawPtr(aSmartPtr.take())
  // construct from |otherRefPtr.forget()|
  {}

  template <typename I,
            typename = std::enable_if_t<std::is_convertible_v<I*, T*>>>
  MOZ_IMPLICIT RefPtr(const RefPtr<I>& aSmartPtr)
      : mRawPtr(aSmartPtr.get())
  // copy-construct from a smart pointer with a related pointer type
  {
    if (mRawPtr) {
      ConstRemovingRefPtrTraits<T>::AddRef(mRawPtr);
    }
  }

  template <typename I,
            typename = std::enable_if_t<std::is_convertible_v<I*, T*>>>
  MOZ_IMPLICIT RefPtr(RefPtr<I>&& aSmartPtr)
      : mRawPtr(aSmartPtr.forget().take())
  // construct from |Move(RefPtr<SomeSubclassOfT>)|.
  {}

  MOZ_IMPLICIT RefPtr(const nsQueryReferent& aHelper);
  MOZ_IMPLICIT RefPtr(const nsCOMPtr_helper& aHelper);
#if defined(XP_WIN)
  MOZ_IMPLICIT RefPtr(const mozilla::mscom::AgileReference& aAgileRef);
#endif  // defined(XP_WIN)

  // Defined in OwningNonNull.h
  template <class U>
  MOZ_IMPLICIT RefPtr(const mozilla::OwningNonNull<U>& aOther);

  // Defined in StaticLocalPtr.h
  template <class U>
  MOZ_IMPLICIT RefPtr(const mozilla::StaticLocalRefPtr<U>& aOther);

  // Defined in StaticPtr.h
  template <class U>
  MOZ_IMPLICIT RefPtr(const mozilla::StaticRefPtr<U>& aOther);

  // Assignment operators

  RefPtr<T>& operator=(decltype(nullptr)) {
    assign_assuming_AddRef(nullptr);
    return *this;
  }

  RefPtr<T>& operator=(const RefPtr<T>& aRhs)
  // copy assignment operator
  {
    assign_with_AddRef(aRhs.mRawPtr);
    return *this;
  }

  template <typename I>
  RefPtr<T>& operator=(const RefPtr<I>& aRhs)
  // assign from an RefPtr of a related pointer type
  {
    assign_with_AddRef(aRhs.get());
    return *this;
  }

  RefPtr<T>& operator=(T* aRhs)
  // assign from a raw pointer (of the right type)
  {
    assign_with_AddRef(aRhs);
    return *this;
  }

  template <typename I>
  RefPtr<T>& operator=(already_AddRefed<I>& aRhs)
  // assign from |already_AddRefed|
  {
    assign_assuming_AddRef(aRhs.take());
    return *this;
  }

  template <typename I>
  RefPtr<T>& operator=(already_AddRefed<I>&& aRhs)
  // assign from |otherRefPtr.forget()|
  {
    assign_assuming_AddRef(aRhs.take());
    return *this;
  }

  RefPtr<T>& operator=(const nsQueryReferent& aQueryReferent);
  RefPtr<T>& operator=(const nsCOMPtr_helper& aHelper);
#if defined(XP_WIN)
  RefPtr<T>& operator=(const mozilla::mscom::AgileReference& aAgileRef);
#endif  // defined(XP_WIN)

  template <typename I,
            typename = std::enable_if_t<std::is_convertible_v<I*, T*>>>
  RefPtr<T>& operator=(RefPtr<I>&& aRefPtr) {
    assign_assuming_AddRef(aRefPtr.forget().take());
    return *this;
  }

  // Defined in OwningNonNull.h
  template <class U>
  RefPtr<T>& operator=(const mozilla::OwningNonNull<U>& aOther);

  // Defined in StaticLocalPtr.h
  template <class U>
  RefPtr<T>& operator=(const mozilla::StaticLocalRefPtr<U>& aOther);

  // Defined in StaticPtr.h
  template <class U>
  RefPtr<T>& operator=(const mozilla::StaticRefPtr<U>& aOther);

  // Other pointer operators

  void swap(RefPtr<T>& aRhs)
  // ...exchange ownership with |aRhs|; can save a pair of refcount operations
  {
    T* temp = aRhs.mRawPtr;
    aRhs.mRawPtr = mRawPtr;
    mRawPtr = temp;
  }

  void swap(T*& aRhs)
  // ...exchange ownership with |aRhs|; can save a pair of refcount operations
  {
    T* temp = aRhs;
    aRhs = mRawPtr;
    mRawPtr = temp;
  }

  already_AddRefed<T> MOZ_MAY_CALL_AFTER_MUST_RETURN forget()
  // return the value of mRawPtr and null out mRawPtr. Useful for
  // already_AddRefed return values.
  {
    T* temp = nullptr;
    swap(temp);
    return already_AddRefed<T>(temp);
  }

  template <typename I>
  void forget(I** aRhs)
  // Set the target of aRhs to the value of mRawPtr and null out mRawPtr.
  // Useful to avoid unnecessary AddRef/Release pairs with "out"
  // parameters where aRhs bay be a T** or an I** where I is a base class
  // of T.
  {
    MOZ_ASSERT(aRhs, "Null pointer passed to forget!");
    *aRhs = mRawPtr;
    mRawPtr = nullptr;
  }

  void forget(nsISupports** aRhs) {
    MOZ_ASSERT(aRhs, "Null pointer passed to forget!");
    *aRhs = ToSupports(mRawPtr);
    mRawPtr = nullptr;
  }

  T* get() const
  /*
    Prefer the implicit conversion provided automatically by |operator T*()
    const|. Use |get()| to resolve ambiguity or to get a castable pointer.
  */
  {
    return const_cast<T*>(mRawPtr);
  }

  operator T*() const&
  /*
    ...makes an |RefPtr| act like its underlying raw pointer type whenever it
    is used in a context where a raw pointer is expected.  It is this operator
    that makes an |RefPtr| substitutable for a raw pointer.

    Prefer the implicit use of this operator to calling |get()|, except where
    necessary to resolve ambiguity.
  */
  {
    return get();
  }

  // Don't allow implicit conversion of temporary RefPtr to raw pointer,
  // because the refcount might be one and the pointer will immediately become
  // invalid.
  operator T*() const&& = delete;

  // These are needed to avoid the deleted operator above.  XXX Why is operator!
  // needed separately?  Shouldn't the compiler prefer using the non-deleted
  // operator bool instead of the deleted operator T*?
  explicit operator bool() const { return !!mRawPtr; }
  bool operator!() const { return !mRawPtr; }

  T* operator->() const MOZ_NO_ADDREF_RELEASE_ON_RETURN {
    MOZ_ASSERT(mRawPtr != nullptr,
               "You can't dereference a NULL RefPtr with operator->().");
    return get();
  }

  template <typename R, typename... Args>
  class Proxy {
    typedef R (T::*member_function)(Args...);
    T* mRawPtr;
    member_function mFunction;

   public:
    Proxy(T* aRawPtr, member_function aFunction)
        : mRawPtr(aRawPtr), mFunction(aFunction) {}
    template <typename... ActualArgs>
    R operator()(ActualArgs&&... aArgs) {
      return ((*mRawPtr).*mFunction)(std::forward<ActualArgs>(aArgs)...);
    }
  };

  template <typename R, typename... Args>
  Proxy<R, Args...> operator->*(R (T::*aFptr)(Args...)) const {
    MOZ_ASSERT(mRawPtr != nullptr,
               "You can't dereference a NULL RefPtr with operator->*().");
    return Proxy<R, Args...>(get(), aFptr);
  }

  RefPtr<T>* get_address()
  // This is not intended to be used by clients.  See |address_of|
  // below.
  {
    return this;
  }

  const RefPtr<T>* get_address() const
  // This is not intended to be used by clients.  See |address_of|
  // below.
  {
    return this;
  }

 public:
  T& operator*() const {
    MOZ_ASSERT(mRawPtr != nullptr,
               "You can't dereference a NULL RefPtr with operator*().");
    return *get();
  }

  T** StartAssignment() {
    assign_assuming_AddRef(nullptr);
    return reinterpret_cast<T**>(&mRawPtr);
  }

 private:
  // This helper class makes |RefPtr<const T>| possible by casting away
  // the constness from the pointer when calling AddRef() and Release().
  //
  // This is necessary because AddRef() and Release() implementations can't
  // generally expected to be const themselves (without heavy use of |mutable|
  // and |const_cast| in their own implementations).
  //
  // This should be sound because while |RefPtr<const T>| provides a
  // const view of an object, the object itself should not be const (it
  // would have to be allocated as |new const T| or similar to be const).
  template <class U>
  struct ConstRemovingRefPtrTraits {
    static void AddRef(U* aPtr) { mozilla::RefPtrTraits<U>::AddRef(aPtr); }
    static void Release(U* aPtr) { mozilla::RefPtrTraits<U>::Release(aPtr); }
  };
  template <class U>
  struct ConstRemovingRefPtrTraits<const U> {
    static void AddRef(const U* aPtr) {
      mozilla::RefPtrTraits<U>::AddRef(const_cast<U*>(aPtr));
    }
    static void Release(const U* aPtr) {
      mozilla::RefPtrTraits<U>::Release(const_cast<U*>(aPtr));
    }
  };
};

class nsCycleCollectionTraversalCallback;
template <typename T>
void CycleCollectionNoteChild(nsCycleCollectionTraversalCallback& aCallback,
                              T* aChild, const char* aName, uint32_t aFlags);

template <typename T>
inline void ImplCycleCollectionUnlink(RefPtr<T>& aField) {
  aField = nullptr;
}

template <typename T>
inline void ImplCycleCollectionTraverse(
    nsCycleCollectionTraversalCallback& aCallback, RefPtr<T>& aField,
    const char* aName, uint32_t aFlags = 0) {
  CycleCollectionNoteChild(aCallback, aField.get(), aName, aFlags);
}

template <class T>
inline RefPtr<T>* address_of(RefPtr<T>& aPtr) {
  return aPtr.get_address();
}

template <class T>
inline const RefPtr<T>* address_of(const RefPtr<T>& aPtr) {
  return aPtr.get_address();
}

template <class T>
class RefPtrGetterAddRefs
/*
  ...

  This class is designed to be used for anonymous temporary objects in the
  argument list of calls that return COM interface pointers, e.g.,

    RefPtr<IFoo> fooP;
    ...->GetAddRefedPointer(getter_AddRefs(fooP))

  DO NOT USE THIS TYPE DIRECTLY IN YOUR CODE.  Use |getter_AddRefs()| instead.

  When initialized with a |RefPtr|, as in the example above, it returns
  a |void**|, a |T**|, or an |nsISupports**| as needed, that the
  outer call (|GetAddRefedPointer| in this case) can fill in.

  This type should be a nested class inside |RefPtr<T>|.
*/
{
 public:
  explicit RefPtrGetterAddRefs(RefPtr<T>& aSmartPtr)
      : mTargetSmartPtr(aSmartPtr) {
    // nothing else to do
  }

  operator void**() {
    return reinterpret_cast<void**>(mTargetSmartPtr.StartAssignment());
  }

  operator T**() { return mTargetSmartPtr.StartAssignment(); }

  T*& operator*() { return *(mTargetSmartPtr.StartAssignment()); }

 private:
  RefPtr<T>& mTargetSmartPtr;
};

template <class T>
inline RefPtrGetterAddRefs<T> getter_AddRefs(RefPtr<T>& aSmartPtr)
/*
  Used around a |RefPtr| when
  ...makes the class |RefPtrGetterAddRefs<T>| invisible.
*/
{
  return RefPtrGetterAddRefs<T>(aSmartPtr);
}

// Comparing two |RefPtr|s

template <class T, class U>
inline bool operator==(const RefPtr<T>& aLhs, const RefPtr<U>& aRhs) {
  return static_cast<const T*>(aLhs.get()) == static_cast<const U*>(aRhs.get());
}

template <class T, class U>
inline bool operator!=(const RefPtr<T>& aLhs, const RefPtr<U>& aRhs) {
  return static_cast<const T*>(aLhs.get()) != static_cast<const U*>(aRhs.get());
}

// Comparing an |RefPtr| to a raw pointer

template <class T, class U>
inline bool operator==(const RefPtr<T>& aLhs, const U* aRhs) {
  return static_cast<const T*>(aLhs.get()) == static_cast<const U*>(aRhs);
}

template <class T, class U>
inline bool operator==(const U* aLhs, const RefPtr<T>& aRhs) {
  return static_cast<const U*>(aLhs) == static_cast<const T*>(aRhs.get());
}

template <class T, class U>
inline bool operator!=(const RefPtr<T>& aLhs, const U* aRhs) {
  return static_cast<const T*>(aLhs.get()) != static_cast<const U*>(aRhs);
}

template <class T, class U>
inline bool operator!=(const U* aLhs, const RefPtr<T>& aRhs) {
  return static_cast<const U*>(aLhs) != static_cast<const T*>(aRhs.get());
}

template <class T, class U>
inline bool operator==(const RefPtr<T>& aLhs, U* aRhs) {
  return static_cast<const T*>(aLhs.get()) == const_cast<const U*>(aRhs);
}

template <class T, class U>
inline bool operator==(U* aLhs, const RefPtr<T>& aRhs) {
  return const_cast<const U*>(aLhs) == static_cast<const T*>(aRhs.get());
}

template <class T, class U>
inline bool operator!=(const RefPtr<T>& aLhs, U* aRhs) {
  return static_cast<const T*>(aLhs.get()) != const_cast<const U*>(aRhs);
}

template <class T, class U>
inline bool operator!=(U* aLhs, const RefPtr<T>& aRhs) {
  return const_cast<const U*>(aLhs) != static_cast<const T*>(aRhs.get());
}

// Comparing an |RefPtr| to |nullptr|

template <class T>
inline bool operator==(const RefPtr<T>& aLhs, decltype(nullptr)) {
  return aLhs.get() == nullptr;
}

template <class T>
inline bool operator==(decltype(nullptr), const RefPtr<T>& aRhs) {
  return nullptr == aRhs.get();
}

template <class T>
inline bool operator!=(const RefPtr<T>& aLhs, decltype(nullptr)) {
  return aLhs.get() != nullptr;
}

template <class T>
inline bool operator!=(decltype(nullptr), const RefPtr<T>& aRhs) {
  return nullptr != aRhs.get();
}

// MOZ_DBG support

template <class T>
std::ostream& operator<<(std::ostream& aOut, const RefPtr<T>& aObj) {
  return mozilla::DebugValue(aOut, aObj.get());
}

/*****************************************************************************/

template <class T>
inline already_AddRefed<T> do_AddRef(T* aObj) {
  RefPtr<T> ref(aObj);
  return ref.forget();
}

template <class T>
inline already_AddRefed<T> do_AddRef(const RefPtr<T>& aObj) {
  RefPtr<T> ref(aObj);
  return ref.forget();
}

namespace mozilla {

template <typename T>
class AlignmentFinder;

// Provide a specialization of AlignmentFinder to allow MOZ_ALIGNOF(RefPtr<T>)
// with an incomplete T.
template <typename T>
class AlignmentFinder<RefPtr<T>> {
 public:
  static const size_t alignment = alignof(T*);
};

/**
 * Helper function to be able to conveniently write things like:
 *
 *   already_AddRefed<T>
 *   f(...)
 *   {
 *     return MakeAndAddRef<T>(...);
 *   }
 */
template <typename T, typename... Args>
already_AddRefed<T> MakeAndAddRef(Args&&... aArgs) {
  RefPtr<T> p(new T(std::forward<Args>(aArgs)...));
  return p.forget();
}

/**
 * Helper function to be able to conveniently write things like:
 *
 *   auto runnable =
 * MakeRefPtr<ErrorCallbackRunnable<nsIDOMGetUserMediaSuccessCallback>>(
 *       mOnSuccess, mOnFailure, *error, mWindowID);
 */
template <typename T, typename... Args>
RefPtr<T> MakeRefPtr(Args&&... aArgs) {
  RefPtr<T> p(new T(std::forward<Args>(aArgs)...));
  return p;
}

}  // namespace mozilla

/**
 * Deduction guide to allow simple `RefPtr` definitions from an
 * already_AddRefed<T> without repeating the type, e.g.:
 *
 *   RefPtr ptr = MakeAndAddRef<SomeType>(...);
 */
template <typename T>
RefPtr(already_AddRefed<T>) -> RefPtr<T>;

#endif /* mozilla_RefPtr_h */
