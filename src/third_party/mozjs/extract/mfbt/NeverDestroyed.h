/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_NeverDestroyed_h
#define mozilla_NeverDestroyed_h

#include <new>
#include <type_traits>
#include <utility>
#include "mozilla/Attributes.h"

namespace mozilla {

// Helper type for creating a local static member of type `T` when `T` has a
// non-trivial static destructor. When used for the local static value, this
// type will avoid introducing a static destructor for these types, as they
// will survive until shutdown.
//
// This can be very useful to avoid static destructors, which are heavily
// discouraged. Using this type is unnecessary if `T` already has a trivial
// destructor, and may introduce unnecessary extra overhead.
//
// This type must only be used with static local members within a function,
// which will be enforced by the clang static analysis.
template <typename T>
class MOZ_STATIC_LOCAL_CLASS MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS
    NeverDestroyed {
 public:
  static_assert(
      !std::is_trivially_destructible_v<T>,
      "NeverDestroyed is unnecessary for trivially destructable types");

  // Allow constructing the inner type.
  // This isn't constexpr, as it requires invoking placement-new. See the
  // comment on `mStorage`.
  template <typename... U>
  explicit NeverDestroyed(U&&... aArgs) {
    new (mStorage) T(std::forward<U>(aArgs)...);
  }

  const T& operator*() const { return *get(); }
  T& operator*() { return *get(); }

  const T* operator->() const { return get(); }
  T* operator->() { return get(); }

  const T* get() const { return reinterpret_cast<T*>(mStorage); }
  T* get() { return reinterpret_cast<T*>(mStorage); }

  // Block copy & move constructor, as the type is not safe to copy.
  NeverDestroyed(const NeverDestroyed&) = delete;
  NeverDestroyed& operator=(const NeverDestroyed&) = delete;

 private:
  // Correctly aligned storage for the type. We unfortunately can't use a union
  // for alignment & constexpr initialization as that would require an explicit
  // destructor declaration, making `NeverDestroyed` non-trivially destructable.
  alignas(T) char mStorage[sizeof(T)];
};

};  // namespace mozilla

#endif  // mozilla_NeverDestroyed_h
