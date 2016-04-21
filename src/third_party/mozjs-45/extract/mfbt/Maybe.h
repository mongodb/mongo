/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A class for optional values and in-place lazy construction. */

#ifndef mozilla_Maybe_h
#define mozilla_Maybe_h

#include "mozilla/Alignment.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Move.h"
#include "mozilla/TypeTraits.h"

#include <new>  // for placement new

namespace mozilla {

struct Nothing { };

/*
 * Maybe is a container class which contains either zero or one elements. It
 * serves two roles. It can represent values which are *semantically* optional,
 * augmenting a type with an explicit 'Nothing' value. In this role, it provides
 * methods that make it easy to work with values that may be missing, along with
 * equality and comparison operators so that Maybe values can be stored in
 * containers. Maybe values can be constructed conveniently in expressions using
 * type inference, as follows:
 *
 *   void doSomething(Maybe<Foo> aFoo) {
 *     if (aFoo)                  // Make sure that aFoo contains a value...
 *       aFoo->takeAction();      // and then use |aFoo->| to access it.
 *   }                            // |*aFoo| also works!
 *
 *   doSomething(Nothing());      // Passes a Maybe<Foo> containing no value.
 *   doSomething(Some(Foo(100))); // Passes a Maybe<Foo> containing |Foo(100)|.
 *
 * You'll note that it's important to check whether a Maybe contains a value
 * before using it, using conversion to bool, |isSome()|, or |isNothing()|. You
 * can avoid these checks, and sometimes write more readable code, using
 * |valueOr()|, |ptrOr()|, and |refOr()|, which allow you to retrieve the value
 * in the Maybe and provide a default for the 'Nothing' case.  You can also use
 * |apply()| to call a function only if the Maybe holds a value, and |map()| to
 * transform the value in the Maybe, returning another Maybe with a possibly
 * different type.
 *
 * Maybe's other role is to support lazily constructing objects without using
 * dynamic storage. A Maybe directly contains storage for a value, but it's
 * empty by default. |emplace()|, as mentioned above, can be used to construct a
 * value in Maybe's storage.  The value a Maybe contains can be destroyed by
 * calling |reset()|; this will happen automatically if a Maybe is destroyed
 * while holding a value.
 *
 * It's a common idiom in C++ to use a pointer as a 'Maybe' type, with a null
 * value meaning 'Nothing' and any other value meaning 'Some'. You can convert
 * from such a pointer to a Maybe value using 'ToMaybe()'.
 *
 * Maybe is inspired by similar types in the standard library of many other
 * languages (e.g. Haskell's Maybe and Rust's Option). In the C++ world it's
 * very similar to std::optional, which was proposed for C++14 and originated in
 * Boost. The most important differences between Maybe and std::optional are:
 *
 *   - std::optional<T> may be compared with T. We deliberately forbid that.
 *   - std::optional allows in-place construction without a separate call to
 *     |emplace()| by using a dummy |in_place_t| value to tag the appropriate
 *     constructor.
 *   - std::optional has |valueOr()|, equivalent to Maybe's |valueOr()|, but
 *     lacks corresponding methods for |refOr()| and |ptrOr()|.
 *   - std::optional lacks |map()| and |apply()|, making it less suitable for
 *     functional-style code.
 *   - std::optional lacks many convenience functions that Maybe has. Most
 *     unfortunately, it lacks equivalents of the type-inferred constructor
 *     functions |Some()| and |Nothing()|.
 *
 * N.B. GCC has missed optimizations with Maybe in the past and may generate
 * extra branches/loads/stores. Use with caution on hot paths; it's not known
 * whether or not this is still a problem.
 */
template<class T>
class Maybe
{
  bool mIsSome;
  AlignedStorage2<T> mStorage;

public:
  typedef T ValueType;

  Maybe() : mIsSome(false) { }
  ~Maybe() { reset(); }

  MOZ_IMPLICIT Maybe(Nothing) : mIsSome(false) { }

  Maybe(const Maybe& aOther)
    : mIsSome(false)
  {
    if (aOther.mIsSome) {
      emplace(*aOther);
    }
  }

  Maybe(Maybe&& aOther)
    : mIsSome(false)
  {
    if (aOther.mIsSome) {
      emplace(Move(*aOther));
      aOther.reset();
    }
  }

  Maybe& operator=(const Maybe& aOther)
  {
    if (&aOther != this) {
      if (aOther.mIsSome) {
        if (mIsSome) {
          // XXX(seth): The correct code for this branch, below, can't be used
          // due to a bug in Visual Studio 2010. See bug 1052940.
          /*
          ref() = aOther.ref();
          */
          reset();
          emplace(*aOther);
        } else {
          emplace(*aOther);
        }
      } else {
        reset();
      }
    }
    return *this;
  }

  Maybe& operator=(Maybe&& aOther)
  {
    MOZ_ASSERT(this != &aOther, "Self-moves are prohibited");

    if (aOther.mIsSome) {
      if (mIsSome) {
        ref() = Move(aOther.ref());
      } else {
        emplace(Move(*aOther));
      }
      aOther.reset();
    } else {
      reset();
    }

    return *this;
  }

  /* Methods that check whether this Maybe contains a value */
  explicit operator bool() const { return isSome(); }
  bool isSome() const { return mIsSome; }
  bool isNothing() const { return !mIsSome; }

  /* Returns the contents of this Maybe<T> by value. Unsafe unless |isSome()|. */
  T value() const
  {
    MOZ_ASSERT(mIsSome);
    return ref();
  }

  /*
   * Returns the contents of this Maybe<T> by value. If |isNothing()|, returns
   * the default value provided.
   */
  template<typename V>
  T valueOr(V&& aDefault) const
  {
    if (isSome()) {
      return ref();
    }
    return Forward<V>(aDefault);
  }

  /*
   * Returns the contents of this Maybe<T> by value. If |isNothing()|, returns
   * the value returned from the function or functor provided.
   */
  template<typename F>
  T valueOrFrom(F&& aFunc) const
  {
    if (isSome()) {
      return ref();
    }
    return aFunc();
  }

  /* Returns the contents of this Maybe<T> by pointer. Unsafe unless |isSome()|. */
  T* ptr()
  {
    MOZ_ASSERT(mIsSome);
    return &ref();
  }

  const T* ptr() const
  {
    MOZ_ASSERT(mIsSome);
    return &ref();
  }

  /*
   * Returns the contents of this Maybe<T> by pointer. If |isNothing()|,
   * returns the default value provided.
   */
  T* ptrOr(T* aDefault)
  {
    if (isSome()) {
      return ptr();
    }
    return aDefault;
  }

  const T* ptrOr(const T* aDefault) const
  {
    if (isSome()) {
      return ptr();
    }
    return aDefault;
  }

  /*
   * Returns the contents of this Maybe<T> by pointer. If |isNothing()|,
   * returns the value returned from the function or functor provided.
   */
  template<typename F>
  T* ptrOrFrom(F&& aFunc)
  {
    if (isSome()) {
      return ptr();
    }
    return aFunc();
  }

  template<typename F>
  const T* ptrOrFrom(F&& aFunc) const
  {
    if (isSome()) {
      return ptr();
    }
    return aFunc();
  }

  T* operator->()
  {
    MOZ_ASSERT(mIsSome);
    return ptr();
  }

  const T* operator->() const
  {
    MOZ_ASSERT(mIsSome);
    return ptr();
  }

  /* Returns the contents of this Maybe<T> by ref. Unsafe unless |isSome()|. */
  T& ref()
  {
    MOZ_ASSERT(mIsSome);
    return *mStorage.addr();
  }

  const T& ref() const
  {
    MOZ_ASSERT(mIsSome);
    return *mStorage.addr();
  }

  /*
   * Returns the contents of this Maybe<T> by ref. If |isNothing()|, returns
   * the default value provided.
   */
  T& refOr(T& aDefault)
  {
    if (isSome()) {
      return ref();
    }
    return aDefault;
  }

  const T& refOr(const T& aDefault) const
  {
    if (isSome()) {
      return ref();
    }
    return aDefault;
  }

  /*
   * Returns the contents of this Maybe<T> by ref. If |isNothing()|, returns the
   * value returned from the function or functor provided.
   */
  template<typename F>
  T& refOrFrom(F&& aFunc)
  {
    if (isSome()) {
      return ref();
    }
    return aFunc();
  }

  template<typename F>
  const T& refOrFrom(F&& aFunc) const
  {
    if (isSome()) {
      return ref();
    }
    return aFunc();
  }

  T& operator*()
  {
    MOZ_ASSERT(mIsSome);
    return ref();
  }

  const T& operator*() const
  {
    MOZ_ASSERT(mIsSome);
    return ref();
  }

  /* If |isSome()|, runs the provided function or functor on the contents of
   * this Maybe. */
  template<typename F, typename... Args>
  void apply(F&& aFunc, Args&&... aArgs)
  {
    if (isSome()) {
      aFunc(ref(), Forward<Args>(aArgs)...);
    }
  }

  template<typename F, typename... Args>
  void apply(F&& aFunc, Args&&... aArgs) const
  {
    if (isSome()) {
      aFunc(ref(), Forward<Args>(aArgs)...);
    }
  }

  /*
   * If |isSome()|, runs the provided function and returns the result wrapped
   * in a Maybe. If |isNothing()|, returns an empty Maybe value.
   */
  template<typename R, typename... FArgs, typename... Args>
  Maybe<R> map(R (*aFunc)(T&, FArgs...), Args&&... aArgs)
  {
    if (isSome()) {
      Maybe<R> val;
      val.emplace(aFunc(ref(), Forward<Args>(aArgs)...));
      return val;
    }
    return Maybe<R>();
  }

  template<typename R, typename... FArgs, typename... Args>
  Maybe<R> map(R (*aFunc)(const T&, FArgs...), Args&&... aArgs) const
  {
    if (isSome()) {
      Maybe<R> val;
      val.emplace(aFunc(ref(), Forward<Args>(aArgs)...));
      return val;
    }
    return Maybe<R>();
  }

  /* If |isSome()|, empties this Maybe and destroys its contents. */
  void reset()
  {
    if (isSome()) {
      ref().~T();
      mIsSome = false;
    }
  }

  /*
   * Constructs a T value in-place in this empty Maybe<T>'s storage. The
   * arguments to |emplace()| are the parameters to T's constructor.
   */
  template<typename... Args>
  void emplace(Args&&... aArgs)
  {
    MOZ_ASSERT(!mIsSome);
    ::new (mStorage.addr()) T(Forward<Args>(aArgs)...);
    mIsSome = true;
  }
};

/*
 * Some() creates a Maybe<T> value containing the provided T value. If T has a
 * move constructor, it's used to make this as efficient as possible.
 *
 * Some() selects the type of Maybe it returns by removing any const, volatile,
 * or reference qualifiers from the type of the value you pass to it. This gives
 * it more intuitive behavior when used in expressions, but it also means that
 * if you need to construct a Maybe value that holds a const, volatile, or
 * reference value, you need to use emplace() instead.
 */
template<typename T>
Maybe<typename RemoveCV<typename RemoveReference<T>::Type>::Type>
Some(T&& aValue)
{
  typedef typename RemoveCV<typename RemoveReference<T>::Type>::Type U;
  Maybe<U> value;
  value.emplace(Forward<T>(aValue));
  return value;
}

template<typename T>
Maybe<typename RemoveCV<typename RemoveReference<T>::Type>::Type>
ToMaybe(T* aPtr)
{
  if (aPtr) {
    return Some(*aPtr);
  }
  return Nothing();
}

/*
 * Two Maybe<T> values are equal if
 * - both are Nothing, or
 * - both are Some, and the values they contain are equal.
 */
template<typename T> bool
operator==(const Maybe<T>& aLHS, const Maybe<T>& aRHS)
{
  if (aLHS.isNothing() != aRHS.isNothing()) {
    return false;
  }
  return aLHS.isNothing() || *aLHS == *aRHS;
}

template<typename T> bool
operator!=(const Maybe<T>& aLHS, const Maybe<T>& aRHS)
{
  return !(aLHS == aRHS);
}

/*
 * We support comparison to Nothing to allow reasonable expressions like:
 *   if (maybeValue == Nothing()) { ... }
 */
template<typename T> bool
operator==(const Maybe<T>& aLHS, const Nothing& aRHS)
{
  return aLHS.isNothing();
}

template<typename T> bool
operator!=(const Maybe<T>& aLHS, const Nothing& aRHS)
{
  return !(aLHS == aRHS);
}

template<typename T> bool
operator==(const Nothing& aLHS, const Maybe<T>& aRHS)
{
  return aRHS.isNothing();
}

template<typename T> bool
operator!=(const Nothing& aLHS, const Maybe<T>& aRHS)
{
  return !(aLHS == aRHS);
}

/*
 * Maybe<T> values are ordered in the same way T values are ordered, except that
 * Nothing comes before anything else.
 */
template<typename T> bool
operator<(const Maybe<T>& aLHS, const Maybe<T>& aRHS)
{
  if (aLHS.isNothing()) {
    return aRHS.isSome();
  }
  if (aRHS.isNothing()) {
    return false;
  }
  return *aLHS < *aRHS;
}

template<typename T> bool
operator>(const Maybe<T>& aLHS, const Maybe<T>& aRHS)
{
  return !(aLHS < aRHS || aLHS == aRHS);
}

template<typename T> bool
operator<=(const Maybe<T>& aLHS, const Maybe<T>& aRHS)
{
  return aLHS < aRHS || aLHS == aRHS;
}

template<typename T> bool
operator>=(const Maybe<T>& aLHS, const Maybe<T>& aRHS)
{
  return !(aLHS < aRHS);
}

} // namespace mozilla

#endif /* mozilla_Maybe_h */
