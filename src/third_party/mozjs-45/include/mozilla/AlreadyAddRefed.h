/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Typed temporary pointers for reference-counted smart pointers. */

#ifndef AlreadyAddRefed_h
#define AlreadyAddRefed_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Move.h"

namespace mozilla {

struct unused_t;

} // namespace mozilla

/**
 * already_AddRefed cooperates with reference counting smart pointers to enable
 * you to assign in a pointer _without_ |AddRef|ing it.  You might want to use
 * this as a return type from a function that returns an already |AddRef|ed
 * pointer.
 *
 * TODO Move already_AddRefed to namespace mozilla.  This has not yet been done
 * because of the sheer number of usages of already_AddRefed.
 */
template<class T>
struct MOZ_MUST_USE MOZ_NON_AUTOABLE already_AddRefed
{
  /*
   * We want to allow returning nullptr from functions returning
   * already_AddRefed<T>, for simplicity.  But we also don't want to allow
   * returning raw T*, instead preferring creation of already_AddRefed<T> from
   * a reference counting smart pointer.
   *
   * We address the latter requirement by making the (T*) constructor explicit.
   * But |return nullptr| won't consider an explicit constructor, so we need
   * another constructor to handle it.  Plain old (decltype(nullptr)) doesn't
   * cut it, because if nullptr is emulated as __null (with type int or long),
   * passing nullptr to an int/long parameter triggers compiler warnings.  We
   * need a type that no one can pass accidentally; a pointer-to-member-function
   * (where no such function exists) does the trick nicely.
   *
   * That handles the return-value case.  What about for locals, argument types,
   * and so on?  |already_AddRefed<T>(nullptr)| considers both overloads (and
   * the (already_AddRefed<T>&&) overload as well!), so there's an ambiguity.
   * We can target true nullptr using decltype(nullptr), but we can't target
   * emulated nullptr the same way, because passing __null to an int/long
   * parameter triggers compiler warnings.  So just give up on this, and provide
   * this behavior through the default constructor.
   *
   * We can revert to simply explicit (T*) and implicit (decltype(nullptr)) when
   * nullptr no longer needs to be emulated to support the ancient b2g compiler.
   * (The () overload could also be removed, if desired, if we changed callers.)
   */
  already_AddRefed() : mRawPtr(nullptr) {}

  // The return and argument types here are arbitrarily selected so no
  // corresponding member function exists.
  typedef void (already_AddRefed::* MatchNullptr)(double, float);
  MOZ_IMPLICIT already_AddRefed(MatchNullptr aRawPtr) : mRawPtr(nullptr) {}

  explicit already_AddRefed(T* aRawPtr) : mRawPtr(aRawPtr) {}

  // Disallow copy constructor and copy assignment operator: move semantics used instead.
  already_AddRefed(const already_AddRefed<T>& aOther) = delete;
  already_AddRefed<T>& operator=(const already_AddRefed<T>& aOther) = delete;

  already_AddRefed(already_AddRefed<T>&& aOther) : mRawPtr(aOther.take()) {}

  already_AddRefed<T>& operator=(already_AddRefed<T>&& aOther)
  {
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
  MOZ_IMPLICIT already_AddRefed(already_AddRefed<U>&& aOther) : mRawPtr(aOther.take()) {}

  ~already_AddRefed() { MOZ_ASSERT(!mRawPtr); }

  // Specialize the unused operator<< for already_AddRefed, to allow
  // nsCOMPtr<nsIFoo> foo;
  // Unused << foo.forget();
  // Note that nsCOMPtr is the XPCOM reference counting smart pointer class.
  friend void operator<<(const mozilla::unused_t& aUnused,
                         const already_AddRefed<T>& aRhs)
  {
    auto mutableAlreadyAddRefed = const_cast<already_AddRefed<T>*>(&aRhs);
    aUnused << mutableAlreadyAddRefed->take();
  }

  MOZ_WARN_UNUSED_RESULT T* take()
  {
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
  template<class U>
  already_AddRefed<U> downcast()
  {
    U* tmp = static_cast<U*>(mRawPtr);
    mRawPtr = nullptr;
    return already_AddRefed<U>(tmp);
  }

private:
  T* MOZ_OWNING_REF mRawPtr;
};

#endif // AlreadyAddRefed_h
