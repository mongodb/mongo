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

#include <stddef.h>

#include <type_traits>
#include <utility>

#include "mozilla/Assertions.h"

namespace mozilla {

namespace detail {
template <typename T>
struct CopyablePtr {
  T mPtr;

  template <typename U>
  explicit CopyablePtr(U&& aPtr) : mPtr{std::forward<U>(aPtr)} {}

  template <typename U>
  explicit CopyablePtr(CopyablePtr<U> aPtr) : mPtr{std::move(aPtr.mPtr)} {}
};
}  // namespace detail

template <typename T>
class MovingNotNull;

// NotNull can be used to wrap a "base" pointer (raw or smart) to indicate it
// is not null. Some examples:
//
// - NotNull<char*>
// - NotNull<RefPtr<Event>>
// - NotNull<nsCOMPtr<Event>>
// - NotNull<UniquePtr<Pointee>>
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
// - It is not movable, but copyable if the base pointer type is copyable. It
//   may be used together with MovingNotNull to avoid unnecessary copies or when
//   the base pointer type is not copyable (such as UniquePtr<T>).
//
template <typename T>
class NotNull {
  template <typename U>
  friend constexpr NotNull<U> WrapNotNull(U aBasePtr);
  template <typename U>
  friend constexpr NotNull<U> WrapNotNullUnchecked(U aBasePtr);
  template <typename U, typename... Args>
  friend constexpr NotNull<U> MakeNotNull(Args&&... aArgs);
  template <typename U>
  friend class NotNull;

  detail::CopyablePtr<T> mBasePtr;

  // This constructor is only used by WrapNotNull() and MakeNotNull<U>().
  template <typename U>
  constexpr explicit NotNull(U aBasePtr) : mBasePtr(T{std::move(aBasePtr)}) {
    static_assert(sizeof(T) == sizeof(NotNull<T>),
                  "NotNull must have zero space overhead.");
    static_assert(offsetof(NotNull<T>, mBasePtr) == 0,
                  "mBasePtr must have zero offset.");
  }

 public:
  // Disallow default construction.
  NotNull() = delete;

  // Construct/assign from another NotNull with a compatible base pointer type.
  template <typename U,
            typename = std::enable_if_t<std::is_convertible_v<const U&, T>>>
  constexpr MOZ_IMPLICIT NotNull(const NotNull<U>& aOther)
      : mBasePtr(aOther.mBasePtr) {}

  template <typename U,
            typename = std::enable_if_t<std::is_convertible_v<U&&, T>>>
  constexpr MOZ_IMPLICIT NotNull(MovingNotNull<U>&& aOther)
      : mBasePtr(std::move(aOther).unwrapBasePtr()) {}

  // Disallow null checks, which are unnecessary for this type.
  explicit operator bool() const = delete;

  // Explicit conversion to a base pointer. Use only to resolve ambiguity or to
  // get a castable pointer.
  constexpr const T& get() const { return mBasePtr.mPtr; }

  // Implicit conversion to a base pointer. Preferable to get().
  constexpr operator const T&() const { return get(); }

  // Implicit conversion to a raw pointer from const lvalue-reference if
  // supported by the base pointer (for RefPtr<T> -> T* compatibility).
  template <typename U,
            std::enable_if_t<!std::is_pointer_v<T> &&
                                 std::is_convertible_v<const T&, U*>,
                             int> = 0>
  constexpr operator U*() const& {
    return get();
  }

  // Don't allow implicit conversions to raw pointers from rvalue-references.
  template <typename U,
            std::enable_if_t<!std::is_pointer_v<T> &&
                                 std::is_convertible_v<const T&, U*> &&
                                 !std::is_convertible_v<const T&&, U*>,
                             int> = 0>
  constexpr operator U*() const&& = delete;

  // Dereference operators.
  constexpr auto* operator->() const MOZ_NONNULL_RETURN {
    return mBasePtr.mPtr.operator->();
  }
  constexpr decltype(*mBasePtr.mPtr) operator*() const {
    return *mBasePtr.mPtr;
  }

  // NotNull can be copied, but not moved. Moving a NotNull with a smart base
  // pointer would leave a nullptr NotNull behind. The move operations must not
  // be explicitly deleted though, since that would cause overload resolution to
  // fail in situations where a copy is possible.
  NotNull(const NotNull&) = default;
  NotNull& operator=(const NotNull&) = default;
};

// Specialization for T* to allow adding MOZ_NONNULL_RETURN attributes.
template <typename T>
class NotNull<T*> {
  template <typename U>
  friend constexpr NotNull<U> WrapNotNull(U aBasePtr);
  template <typename U>
  friend constexpr NotNull<U*> WrapNotNullUnchecked(U* aBasePtr);
  template <typename U, typename... Args>
  friend constexpr NotNull<U> MakeNotNull(Args&&... aArgs);
  template <typename U>
  friend class NotNull;

  T* mBasePtr;

  // This constructor is only used by WrapNotNull() and MakeNotNull<U>().
  template <typename U>
  constexpr explicit NotNull(U* aBasePtr) : mBasePtr(aBasePtr) {}

 public:
  // Disallow default construction.
  NotNull() = delete;

  // Construct/assign from another NotNull with a compatible base pointer type.
  template <typename U,
            typename = std::enable_if_t<std::is_convertible_v<const U&, T*>>>
  constexpr MOZ_IMPLICIT NotNull(const NotNull<U>& aOther)
      : mBasePtr(aOther.get()) {
    static_assert(sizeof(T*) == sizeof(NotNull<T*>),
                  "NotNull must have zero space overhead.");
    static_assert(offsetof(NotNull<T*>, mBasePtr) == 0,
                  "mBasePtr must have zero offset.");
  }

  template <typename U,
            typename = std::enable_if_t<std::is_convertible_v<U&&, T*>>>
  constexpr MOZ_IMPLICIT NotNull(MovingNotNull<U>&& aOther)
      : mBasePtr(NotNull{std::move(aOther)}) {}

  // Disallow null checks, which are unnecessary for this type.
  explicit operator bool() const = delete;

  // Explicit conversion to a base pointer. Use only to resolve ambiguity or to
  // get a castable pointer.
  constexpr T* get() const MOZ_NONNULL_RETURN { return mBasePtr; }

  // Implicit conversion to a base pointer. Preferable to get().
  constexpr operator T*() const MOZ_NONNULL_RETURN { return get(); }

  // Dereference operators.
  constexpr T* operator->() const MOZ_NONNULL_RETURN { return get(); }
  constexpr T& operator*() const { return *mBasePtr; }
};

template <typename T>
constexpr NotNull<T> WrapNotNull(T aBasePtr) {
  MOZ_RELEASE_ASSERT(aBasePtr);
  return NotNull<T>{std::move(aBasePtr)};
}

// WrapNotNullUnchecked should only be used in situations, where it is
// statically known that aBasePtr is non-null, and redundant release assertions
// should be avoided. It is only defined for raw base pointers, since it is only
// needed for those right now. There is no fundamental reason not to allow
// arbitrary base pointers here.
template <typename T>
constexpr NotNull<T> WrapNotNullUnchecked(T aBasePtr) {
  return NotNull<T>{std::move(aBasePtr)};
}

template <typename T>
MOZ_NONNULL(1)
constexpr NotNull<T*> WrapNotNullUnchecked(T* const aBasePtr) {
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wpointer-bool-conversion"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wnonnull-compare"
#endif
  MOZ_ASSERT(aBasePtr);
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
  return NotNull<T*>{aBasePtr};
}

// A variant of NotNull that can be used as a return value or parameter type and
// moved into both NotNull and non-NotNull targets. This is not possible with
// NotNull, as it is not movable. MovingNotNull can therefore not guarantee it
// is always non-nullptr, but it can't be dereferenced, and there are debug
// assertions that ensure it is only moved once.
template <typename T>
class MOZ_NON_AUTOABLE MovingNotNull {
  template <typename U>
  friend constexpr MovingNotNull<U> WrapMovingNotNullUnchecked(U aBasePtr);

  T mBasePtr;
#ifdef DEBUG
  bool mConsumed = false;
#endif

  // This constructor is only used by WrapNotNull() and MakeNotNull<U>().
  template <typename U>
  constexpr explicit MovingNotNull(U aBasePtr) : mBasePtr{std::move(aBasePtr)} {
#ifndef DEBUG
    static_assert(sizeof(T) == sizeof(MovingNotNull<T>),
                  "NotNull must have zero space overhead.");
#endif
    static_assert(offsetof(MovingNotNull<T>, mBasePtr) == 0,
                  "mBasePtr must have zero offset.");
  }

 public:
  MovingNotNull() = delete;

  MOZ_IMPLICIT MovingNotNull(const NotNull<T>& aSrc) : mBasePtr(aSrc.get()) {}

  template <typename U,
            typename = std::enable_if_t<std::is_convertible_v<U, T>>>
  MOZ_IMPLICIT MovingNotNull(const NotNull<U>& aSrc) : mBasePtr(aSrc.get()) {}

  template <typename U,
            typename = std::enable_if_t<std::is_convertible_v<U, T>>>
  MOZ_IMPLICIT MovingNotNull(MovingNotNull<U>&& aSrc)
      : mBasePtr(std::move(aSrc).unwrapBasePtr()) {}

  MOZ_IMPLICIT operator T() && { return std::move(*this).unwrapBasePtr(); }

  MOZ_IMPLICIT operator NotNull<T>() && { return std::move(*this).unwrap(); }

  NotNull<T> unwrap() && {
    return WrapNotNullUnchecked(std::move(*this).unwrapBasePtr());
  }

  T unwrapBasePtr() && {
#ifdef DEBUG
    MOZ_ASSERT(!mConsumed);
    mConsumed = true;
#endif
    return std::move(mBasePtr);
  }

  MovingNotNull(MovingNotNull&&) = default;
  MovingNotNull& operator=(MovingNotNull&&) = default;
};

template <typename T>
constexpr MovingNotNull<T> WrapMovingNotNullUnchecked(T aBasePtr) {
  return MovingNotNull<T>{std::move(aBasePtr)};
}

template <typename T>
constexpr MovingNotNull<T> WrapMovingNotNull(T aBasePtr) {
  MOZ_RELEASE_ASSERT(aBasePtr);
  return WrapMovingNotNullUnchecked(std::move(aBasePtr));
}

namespace detail {

// Extract the pointed-to type from a pointer type (be it raw or smart).
// The default implementation uses the dereferencing operator of the pointer
// type to find what it's pointing to.
template <typename Pointer>
struct PointedTo {
  // Remove the reference that dereferencing operators may return.
  using Type = std::remove_reference_t<decltype(*std::declval<Pointer>())>;
  using NonConstType = std::remove_const_t<Type>;
};

// Specializations for raw pointers.
// This is especially required because VS 2017 15.6 (March 2018) started
// rejecting the above `decltype(*std::declval<Pointer>())` trick for raw
// pointers.
// See bug 1443367.
template <typename T>
struct PointedTo<T*> {
  using Type = T;
  using NonConstType = T;
};

template <typename T>
struct PointedTo<const T*> {
  using Type = const T;
  using NonConstType = T;
};

}  // namespace detail

// Allocate an object with infallible new, and wrap its pointer in NotNull.
// |MakeNotNull<Ptr<Ob>>(args...)| will run |new Ob(args...)|
// and return NotNull<Ptr<Ob>>.
template <typename T, typename... Args>
constexpr NotNull<T> MakeNotNull(Args&&... aArgs) {
  using Pointee = typename detail::PointedTo<T>::NonConstType;
  static_assert(!std::is_array_v<Pointee>,
                "MakeNotNull cannot construct an array");
  return NotNull<T>(new Pointee(std::forward<Args>(aArgs)...));
}

// Compare two NotNulls.
template <typename T, typename U>
constexpr bool operator==(const NotNull<T>& aLhs, const NotNull<U>& aRhs) {
  return aLhs.get() == aRhs.get();
}
template <typename T, typename U>
constexpr bool operator!=(const NotNull<T>& aLhs, const NotNull<U>& aRhs) {
  return aLhs.get() != aRhs.get();
}

// Compare a NotNull to a base pointer.
template <typename T, typename U>
constexpr bool operator==(const NotNull<T>& aLhs, const U& aRhs) {
  return aLhs.get() == aRhs;
}
template <typename T, typename U>
constexpr bool operator!=(const NotNull<T>& aLhs, const U& aRhs) {
  return aLhs.get() != aRhs;
}

// Compare a base pointer to a NotNull.
template <typename T, typename U>
constexpr bool operator==(const T& aLhs, const NotNull<U>& aRhs) {
  return aLhs == aRhs.get();
}
template <typename T, typename U>
constexpr bool operator!=(const T& aLhs, const NotNull<U>& aRhs) {
  return aLhs != aRhs.get();
}

// Disallow comparing a NotNull to a nullptr.
template <typename T>
bool operator==(const NotNull<T>&, decltype(nullptr)) = delete;
template <typename T>
bool operator!=(const NotNull<T>&, decltype(nullptr)) = delete;

// Disallow comparing a nullptr to a NotNull.
template <typename T>
bool operator==(decltype(nullptr), const NotNull<T>&) = delete;
template <typename T>
bool operator!=(decltype(nullptr), const NotNull<T>&) = delete;

}  // namespace mozilla

#endif /* mozilla_NotNull_h */
