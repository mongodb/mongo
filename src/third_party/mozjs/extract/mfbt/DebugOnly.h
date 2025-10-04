/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Provides DebugOnly, a type for variables used only in debug builds (i.e. by
 * assertions).
 */

#ifndef mozilla_DebugOnly_h
#define mozilla_DebugOnly_h

#include "mozilla/Attributes.h"

#include <utility>

namespace mozilla {

/**
 * DebugOnly contains a value of type T, but only in debug builds.  In release
 * builds, it does not contain a value.  This helper is intended to be used with
 * MOZ_ASSERT()-style macros, allowing one to write:
 *
 *   DebugOnly<bool> check = func();
 *   MOZ_ASSERT(check);
 *
 * more concisely than declaring |check| conditional on #ifdef DEBUG.
 *
 * DebugOnly instances can only be coerced to T in debug builds.  In release
 * builds they don't have a value, so type coercion is not well defined.
 *
 * NOTE: DebugOnly instances still take up one byte of space, plus padding, even
 * in optimized, non-DEBUG builds (see bug 1253094 comment 37 for more info).
 * For this reason the class is MOZ_STACK_CLASS to prevent consumers using
 * DebugOnly for struct/class members and unwittingly inflating the size of
 * their objects in release builds.
 */
template <typename T>
class MOZ_STACK_CLASS DebugOnly {
 public:
#ifdef DEBUG
  T value;

  DebugOnly() = default;
  MOZ_IMPLICIT DebugOnly(T&& aOther) : value(std::move(aOther)) {}
  MOZ_IMPLICIT DebugOnly(const T& aOther) : value(aOther) {}
  DebugOnly(const DebugOnly& aOther) : value(aOther.value) {}
  DebugOnly& operator=(const T& aRhs) {
    value = aRhs;
    return *this;
  }
  DebugOnly& operator=(T&& aRhs) {
    value = std::move(aRhs);
    return *this;
  }

  void operator++(int) { value++; }
  void operator--(int) { value--; }

  // Do not define operator+=(), etc. here.  These will coerce via the
  // implicit cast and built-in operators.  Defining explicit methods here
  // will create ambiguity the compiler can't deal with.

  T* operator&() { return &value; }

  operator T&() { return value; }
  operator const T&() const { return value; }

  T& operator->() { return value; }
  const T& operator->() const { return value; }

  const T& inspect() const { return value; }

#else
  DebugOnly() = default;
  MOZ_IMPLICIT DebugOnly(const T&) {}
  DebugOnly(const DebugOnly&) {}
  DebugOnly& operator=(const T&) { return *this; }
  MOZ_IMPLICIT DebugOnly(T&&) {}
  DebugOnly& operator=(T&&) { return *this; }
  void operator++(int) {}
  void operator--(int) {}
  DebugOnly& operator+=(const T&) { return *this; }
  DebugOnly& operator-=(const T&) { return *this; }
  DebugOnly& operator&=(const T&) { return *this; }
  DebugOnly& operator|=(const T&) { return *this; }
  DebugOnly& operator^=(const T&) { return *this; }
#endif

  /*
   * DebugOnly must always have a user-defined destructor or else it will
   * generate "unused variable" warnings, exactly what it's intended
   * to avoid!
   */
  ~DebugOnly() {}
};

}  // namespace mozilla

#endif /* mozilla_DebugOnly_h */
