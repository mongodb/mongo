/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_NonDereferenceable_h
#define mozilla_NonDereferenceable_h

/* A pointer wrapper indicating that the pointer should not be dereferenced. */

#include "mozilla/Attributes.h"

#include <cstdint>

// Macro indicating that a function manipulates a pointer that will not be
// dereferenced, and therefore there is no need to check the object.
#if defined(__clang__)
#  define NO_POINTEE_CHECKS __attribute__((no_sanitize("vptr")))
#else
#  define NO_POINTEE_CHECKS /* nothing */
#endif

namespace mozilla {

// NonDereferenceable<T> wraps a raw pointer value of type T*, but prevents
// dereferencing.
//
// The main use case is for pointers that referencing memory that may not
// contain a valid object, either because the object has already been freed, or
// is under active construction or destruction (and hence parts of it may be
// uninitialized or destructed.)
// Such a pointer may still be useful, e.g., for its numeric value for
// logging/debugging purposes, which may be accessed with `value()`.
// Using NonDereferenceable with such pointers will make this intent clearer,
// and prevent misuses.
//
// Note that NonDereferenceable is only a wrapper and is NOT an owning pointer,
// i.e., it will not release/free the object.
//
// NonDereferenceable allows conversions between compatible pointer types, e.g.,
// to navigate a class hierarchy and identify parent/sub-objects. Note that the
// converted pointers stay safely NonDereferenceable.
//
// Use of NonDereferenceable is required to avoid errors from sanitization tools
// like `clang++ -fsanitize=vptr`, and should prevent false positives while
// pointers are manipulated within NonDereferenceable objects.
//
template <typename T>
class NonDereferenceable {
 public:
  // Default construction with a null value.
  NonDereferenceable() : mPtr(nullptr) {}

  // Default copy construction and assignment.
  NO_POINTEE_CHECKS
  NonDereferenceable(const NonDereferenceable&) = default;
  NO_POINTEE_CHECKS
  NonDereferenceable<T>& operator=(const NonDereferenceable&) = default;
  // No move operations, as we're only carrying a non-owning pointer, so
  // copying is most efficient.

  // Construct/assign from a T* raw pointer.
  // A raw pointer should usually point at a valid object, however we want to
  // leave the ability to the user to create a NonDereferenceable from any
  // pointer. Also, strictly speaking, in a constructor or destructor, `this`
  // points at an object still being constructed or already partially
  // destructed, which some very sensitive sanitizers could complain about.
  NO_POINTEE_CHECKS
  explicit NonDereferenceable(T* aPtr) : mPtr(aPtr) {}
  NO_POINTEE_CHECKS
  NonDereferenceable& operator=(T* aPtr) {
    mPtr = aPtr;
    return *this;
  }

  // Construct/assign from a compatible pointer type.
  template <typename U>
  NO_POINTEE_CHECKS explicit NonDereferenceable(U* aOther)
      : mPtr(static_cast<T*>(aOther)) {}
  template <typename U>
  NO_POINTEE_CHECKS NonDereferenceable& operator=(U* aOther) {
    mPtr = static_cast<T*>(aOther);
    return *this;
  }

  // Construct/assign from a NonDereferenceable with a compatible pointer type.
  template <typename U>
  NO_POINTEE_CHECKS MOZ_IMPLICIT
  NonDereferenceable(const NonDereferenceable<U>& aOther)
      : mPtr(static_cast<T*>(aOther.mPtr)) {}
  template <typename U>
  NO_POINTEE_CHECKS NonDereferenceable& operator=(
      const NonDereferenceable<U>& aOther) {
    mPtr = static_cast<T*>(aOther.mPtr);
    return *this;
  }

  // Explicitly disallow dereference operators, so that compiler errors point
  // at these lines:
  T& operator*() = delete;   // Cannot dereference NonDereferenceable!
  T* operator->() = delete;  // Cannot dereference NonDereferenceable!

  // Null check.
  NO_POINTEE_CHECKS
  explicit operator bool() const { return !!mPtr; }

  // Extract the pointer value, untyped.
  NO_POINTEE_CHECKS
  uintptr_t value() const { return reinterpret_cast<uintptr_t>(mPtr); }

 private:
  // Let other NonDereferenceable templates access mPtr, to permit construction/
  // assignment from compatible pointer types.
  template <typename>
  friend class NonDereferenceable;

  T* MOZ_NON_OWNING_REF mPtr;
};

}  // namespace mozilla

#undef NO_POINTEE_CHECKS

#endif /* mozilla_NonDereferenceable_h */
