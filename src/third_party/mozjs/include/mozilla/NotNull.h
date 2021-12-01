/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_NotNull_h
#define mozilla_NotNull_h

// It's often unclear if a particular pointer, be it raw (T*) or smart
// (RefPtr<T>, nsCOMPtr<T>, etc.) can be null. This leads to missing null
// checks (which can cause crashes) and unnecessary null checks (which clutter
// the code).
//
// C++ has a built-in alternative that avoids these problems: references. This
// module defines another alternative, NotNull, which can be used in cases
// where references are not suitable.
//
// In the comments below we use the word "handle" to cover all varieties of
// pointers and references.
//
// References
// ----------
// References are always non-null. (You can do |T& r = *p;| where |p| is null,
// but that's undefined behaviour. C++ doesn't provide any built-in, ironclad
// guarantee of non-nullness.)
//
// A reference works well when you need a temporary handle to an existing
// single object, e.g. for passing a handle to a function, or as a local handle
// within another object. (In Rust parlance, this is a "borrow".)
//
// A reference is less appropriate in the following cases.
//
// - As a primary handle to an object. E.g. code such as this is possible but
//   strange: |T& t = *new T(); ...; delete &t;|
//
// - As a handle to an array. It's common for |T*| to refer to either a single
//   |T| or an array of |T|, but |T&| cannot refer to an array of |T| because
//   you can't index off a reference (at least, not without first converting it
//   to a pointer).
//
// - When the handle identity is meaningful, e.g. if you have a hashtable of
//   handles, because you have to use |&| on the reference to convert it to a
//   pointer.
//
// - Some people don't like using non-const references as function parameters,
//   because it is not clear at the call site that the argument might be
//   modified.
//
// - When you need "smart" behaviour. E.g. we lack reference equivalents to
//   RefPtr and nsCOMPtr.
//
// - When interfacing with code that uses pointers a lot, sometimes using a
//   reference just feels like an odd fit.
//
// Furthermore, a reference is impossible in the following cases.
//
// - When the handle is rebound to another object. References don't allow this.
//
// - When the handle has type |void|. |void&| is not allowed.
//
// NotNull is an alternative that can be used in any of the above cases except
// for the last one, where the handle type is |void|. See below.

#include "mozilla/Assertions.h"
#include "mozilla/Move.h"
#include <stddef.h>

namespace mozilla {

// NotNull can be used to wrap a "base" pointer (raw or smart) to indicate it
// is not null. Some examples:
//
// - NotNull<char*>
// - NotNull<RefPtr<Event>>
// - NotNull<nsCOMPtr<Event>>
//
// NotNull has the following notable properties.
//
// - It has zero space overhead.
//
// - It must be initialized explicitly. There is no default initialization.
//
// - It auto-converts to the base pointer type.
//
// - It does not auto-convert from a base pointer. Implicit conversion from a
//   less-constrained type (e.g. T*) to a more-constrained type (e.g.
//   NotNull<T*>) is dangerous. Creation and assignment from a base pointer can
//   only be done with WrapNotNull() or MakeNotNull<>(), which makes them
//   impossible to overlook, both when writing and reading code.
//
// - When initialized (or assigned) it is checked, and if it is null we abort.
//   This guarantees that it cannot be null.
//
// - |operator bool()| is deleted. This means you cannot check a NotNull in a
//   boolean context, which eliminates the possibility of unnecessary null
//   checks.
//
// NotNull currently doesn't work with UniquePtr. See
// https://github.com/Microsoft/GSL/issues/89 for some discussion.
//
template <typename T>
class NotNull
{
  template <typename U> friend NotNull<U> WrapNotNull(U aBasePtr);
  template<typename U, typename... Args>
  friend NotNull<U> MakeNotNull(Args&&... aArgs);

  T mBasePtr;

  // This constructor is only used by WrapNotNull() and MakeNotNull<U>().
  template <typename U>
  explicit NotNull(U aBasePtr) : mBasePtr(aBasePtr) {}

public:
  // Disallow default construction.
  NotNull() = delete;

  // Construct/assign from another NotNull with a compatible base pointer type.
  template <typename U>
  MOZ_IMPLICIT NotNull(const NotNull<U>& aOther) : mBasePtr(aOther.get()) {
    static_assert(sizeof(T) == sizeof(NotNull<T>),
                  "NotNull must have zero space overhead.");
    static_assert(offsetof(NotNull<T>, mBasePtr) == 0,
                  "mBasePtr must have zero offset.");
  }

  // Default copy/move construction and assignment.
  NotNull(const NotNull<T>&) = default;
  NotNull<T>& operator=(const NotNull<T>&) = default;
  NotNull(NotNull<T>&&) = default;
  NotNull<T>& operator=(NotNull<T>&&) = default;

  // Disallow null checks, which are unnecessary for this type.
  explicit operator bool() const = delete;

  // Explicit conversion to a base pointer. Use only to resolve ambiguity or to
  // get a castable pointer.
  const T& get() const { return mBasePtr; }

  // Implicit conversion to a base pointer. Preferable to get().
  operator const T&() const { return get(); }

  // Dereference operators.
  const T& operator->() const { return get(); }
  decltype(*mBasePtr) operator*() const { return *mBasePtr; }
};

template <typename T>
NotNull<T>
WrapNotNull(const T aBasePtr)
{
  NotNull<T> notNull(aBasePtr);
  MOZ_RELEASE_ASSERT(aBasePtr);
  return notNull;
}

namespace detail {

// Extract the pointed-to type from a pointer type (be it raw or smart).
// The default implementation uses the dereferencing operator of the pointer
// type to find what it's pointing to.
template<typename Pointer>
struct PointedTo
{
  // Remove the reference that dereferencing operators may return.
  using Type = typename RemoveReference<decltype(*DeclVal<Pointer>())>::Type;
  using NonConstType = typename RemoveConst<Type>::Type;
};

// Specializations for raw pointers.
// This is especially required because VS 2017 15.6 (March 2018) started
// rejecting the above `decltype(*DeclVal<Pointer>())` trick for raw pointers.
// See bug 1443367.
template<typename T>
struct PointedTo<T*>
{
  using Type = T;
  using NonConstType = T;
};

template<typename T>
struct PointedTo<const T*>
{
  using Type = const T;
  using NonConstType = T;
};

} // namespace detail

// Allocate an object with infallible new, and wrap its pointer in NotNull.
// |MakeNotNull<Ptr<Ob>>(args...)| will run |new Ob(args...)|
// and return NotNull<Ptr<Ob>>.
template<typename T, typename... Args>
NotNull<T>
MakeNotNull(Args&&... aArgs)
{
  using Pointee = typename detail::PointedTo<T>::NonConstType;
  static_assert(!IsArray<Pointee>::value,
                "MakeNotNull cannot construct an array");
  return NotNull<T>(new Pointee(Forward<Args>(aArgs)...));
}

// Compare two NotNulls.
template <typename T, typename U>
inline bool
operator==(const NotNull<T>& aLhs, const NotNull<U>& aRhs)
{
  return aLhs.get() == aRhs.get();
}
template <typename T, typename U>
inline bool
operator!=(const NotNull<T>& aLhs, const NotNull<U>& aRhs)
{
  return aLhs.get() != aRhs.get();
}

// Compare a NotNull to a base pointer.
template <typename T, typename U>
inline bool
operator==(const NotNull<T>& aLhs, const U& aRhs)
{
  return aLhs.get() == aRhs;
}
template <typename T, typename U>
inline bool
operator!=(const NotNull<T>& aLhs, const U& aRhs)
{
  return aLhs.get() != aRhs;
}

// Compare a base pointer to a NotNull.
template <typename T, typename U>
inline bool
operator==(const T& aLhs, const NotNull<U>& aRhs)
{
  return aLhs == aRhs.get();
}
template <typename T, typename U>
inline bool
operator!=(const T& aLhs, const NotNull<U>& aRhs)
{
  return aLhs != aRhs.get();
}

// Disallow comparing a NotNull to a nullptr.
template <typename T>
bool
operator==(const NotNull<T>&, decltype(nullptr)) = delete;
template <typename T>
bool
operator!=(const NotNull<T>&, decltype(nullptr)) = delete;

// Disallow comparing a nullptr to a NotNull.
template <typename T>
bool
operator==(decltype(nullptr), const NotNull<T>&) = delete;
template <typename T>
bool
operator!=(decltype(nullptr), const NotNull<T>&) = delete;

} // namespace mozilla

#endif /* mozilla_NotNull_h */
