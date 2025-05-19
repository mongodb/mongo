/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Typed temporary pointers for reference-counted smart pointers. */

#ifndef AlreadyAddRefed_h
#define AlreadyAddRefed_h

#include <utility>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

namespace mozilla {

struct unused_t;

}  // namespace mozilla

/**
 * already_AddRefed cooperates with reference counting smart pointers to enable
 * you to assign in a pointer _without_ |AddRef|ing it.  You might want to use
 * this as a return type from a function that returns an already |AddRef|ed
 * pointer.  Or, you might want to use this as a parameter type in a function
 * that wants to force a transfer-of-ownership from a RefPtr in the caller (for
 * example, if the function expects callers to pass in a newly-created object,
 * which the function then takes ownership of).
 *
 * TODO Move already_AddRefed to namespace mozilla.  This has not yet been done
 * because of the sheer number of usages of already_AddRefed.
 *
 * When should you use already_AddRefed<>?
 * * Ensure a consumer takes ownership of a reference
 * * Pass ownership without calling AddRef/Release (sometimes required in
 *   off-main-thread code)
 * * The ref pointer type you're using doesn't support move construction
 *
 * Otherwise, use std::move(RefPtr/nsCOMPtr/etc).
 */
template <class T>
struct
#if !defined(MOZ_CLANG_PLUGIN) && !defined(XGILL_PLUGIN)
    [[nodiscard]]
#endif
    MOZ_NON_AUTOABLE already_AddRefed {
  already_AddRefed() : mRawPtr(nullptr) {}

  // For simplicity, allow returning nullptr from functions returning
  // already_AddRefed<T>. Don't permit returning raw T*, though; it's preferred
  // to create already_AddRefed<T> from a reference-counting smart pointer.
  MOZ_IMPLICIT already_AddRefed(decltype(nullptr)) : mRawPtr(nullptr) {}
  explicit already_AddRefed(T* aRawPtr) : mRawPtr(aRawPtr) {}

  // Disallow copy constructor and copy assignment operator: move semantics used
  // instead.
  already_AddRefed(const already_AddRefed<T>& aOther) = delete;
  already_AddRefed<T>& operator=(const already_AddRefed<T>& aOther) = delete;

  // WARNING: sketchiness ahead.
  //
  // The x86-64 ABI for Unix-like operating systems requires structures to be
  // returned via invisible reference if they are non-trivial for the purposes
  // of calls according to the C++ ABI[1].  For our consideration here, that
  // means that if we have a non-trivial move constructor or destructor,
  // already_AddRefed must be returned by invisible reference.  But
  // already_AddRefed is small enough and so commonly used that it would be
  // beneficial to return it via registers instead.  So we need to figure out
  // a way to make the move constructor and the destructor trivial.
  //
  // Our destructor is normally non-trivial, because it asserts that the
  // stored pointer has been taken by somebody else prior to destruction.
  // However, since the assert in question is compiled only for DEBUG builds,
  // we can make the destructor trivial in non-DEBUG builds by simply defining
  // it with `= default`.
  //
  // We now have to make the move constructor trivial as well.  It is normally
  // non-trivial, because the incoming object has its pointer null-ed during
  // the move. This null-ing is done to satisfy the assert in the destructor.
  // But since that destructor has no assert in non-DEBUG builds, the clearing
  // is unnecessary in such builds; all we really need to perform is a copy of
  // the pointer from the incoming object.  So we can let the compiler define
  // a trivial move constructor for us, and already_AddRefed can now be
  // returned in registers rather than needing to allocate a stack slot for
  // an invisible reference.
  //
  // The above considerations apply to Unix-like operating systems only; the
  // conditions for the same optimization to apply on x86-64 Windows are much
  // more strigent and are basically impossible for already_AddRefed to
  // satisfy[2].  But we do get some benefit from this optimization on Windows
  // because we removed the nulling of the pointer during the move, so that's
  // a codesize win.
  //
  // [1] https://itanium-cxx-abi.github.io/cxx-abi/abi.html#non-trivial
  // [2] https://docs.microsoft.com/en-us/cpp/build/return-values-cpp

  already_AddRefed(already_AddRefed<T>&& aOther)
#ifdef DEBUG
      : mRawPtr(aOther.take()){}
#else
      = default;
#endif

        already_AddRefed<T> &
        operator=(already_AddRefed<T>&& aOther) {
    mRawPtr = aOther.take();
    return *this;
  }

  /**
   * This helper is useful in cases like
   *
   *  already_AddRefed<BaseClass>
   *  Foo()
   *  {
   *    RefPtr<SubClass> x = ...;
   *    return x.forget();
   *  }
   *
   * The autoconversion allows one to omit the idiom
   *
   *    RefPtr<BaseClass> y = x.forget();
   *    return y.forget();
   *
   * Note that nsRefPtr is the XPCOM reference counting smart pointer class.
   */
  template <typename U>
  MOZ_IMPLICIT already_AddRefed(already_AddRefed<U>&& aOther)
      : mRawPtr(aOther.take()) {}

  ~already_AddRefed()
#ifdef DEBUG
  {
    MOZ_ASSERT(!mRawPtr);
  }
#else
      = default;
#endif

  // Specialize the unused operator<< for already_AddRefed, to allow
  // nsCOMPtr<nsIFoo> foo;
  // Unused << foo.forget();
  // Note that nsCOMPtr is the XPCOM reference counting smart pointer class.
  friend void operator<<(const mozilla::unused_t& aUnused,
                         const already_AddRefed<T>& aRhs) {
    auto mutableAlreadyAddRefed = const_cast<already_AddRefed<T>*>(&aRhs);
    aUnused << mutableAlreadyAddRefed->take();
  }

  [[nodiscard]] T* take() {
    T* rawPtr = mRawPtr;
    mRawPtr = nullptr;
    return rawPtr;
  }

  /**
   * This helper provides a static_cast replacement for already_AddRefed, so
   * if you have
   *
   *   already_AddRefed<Parent> F();
   *
   * you can write
   *
   *   already_AddRefed<Child>
   *   G()
   *   {
   *     return F().downcast<Child>();
   *   }
   */
  template <class U>
  already_AddRefed<U> downcast() {
    U* tmp = static_cast<U*>(mRawPtr);
    mRawPtr = nullptr;
    return already_AddRefed<U>(tmp);
  }

 private:
  T* MOZ_OWNING_REF mRawPtr;
};

#endif  // AlreadyAddRefed_h
