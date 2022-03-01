/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Template types for use in generic code: to use Rooted/Handle/MutableHandle in
 * cases where GC may occur, or to use mock versions of those types that perform
 * no rooting or root list manipulation when GC cannot occur.
 */

#ifndef gc_MaybeRooted_h
#define gc_MaybeRooted_h

#include "mozilla/Attributes.h"  // MOZ_IMPLICIT, MOZ_RAII

#include <type_traits>  // std::true_type

#include "gc/Allocator.h"            // js::AllowGC, js::CanGC, js::NoGC
#include "js/ComparisonOperators.h"  // JS::detail::DefineComparisonOps
#include "js/RootingAPI.h"  // js::{Rooted,MutableHandle}Base, JS::SafelyInitialized, DECLARE_POINTER_{CONSTREF,ASSIGN}_OPS, DECLARE_NONPOINTER_{,MUTABLE_}ACCESSOR_METHODS, JS::Rooted, JS::{,Mutable}Handle

namespace js {

/**
 * Interface substitute for Rooted<T> which does not root the variable's
 * memory.
 */
template <typename T>
class MOZ_RAII FakeRooted : public RootedBase<T, FakeRooted<T>> {
 public:
  using ElementType = T;

  explicit FakeRooted(JSContext* cx) : ptr(JS::SafelyInitialized<T>()) {}

  FakeRooted(JSContext* cx, T initial) : ptr(initial) {}

  DECLARE_POINTER_CONSTREF_OPS(T);
  DECLARE_POINTER_ASSIGN_OPS(FakeRooted, T);
  DECLARE_NONPOINTER_ACCESSOR_METHODS(ptr);
  DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(ptr);

 private:
  T ptr;

  void set(const T& value) { ptr = value; }

  FakeRooted(const FakeRooted&) = delete;
};

}  // namespace js

namespace JS {

namespace detail {

template <typename T>
struct DefineComparisonOps<js::FakeRooted<T>> : std::true_type {
  static const T& get(const js::FakeRooted<T>& v) { return v.get(); }
};

}  // namespace detail

}  // namespace JS

namespace js {

/**
 * Interface substitute for MutableHandle<T> which is not required to point to
 * rooted memory.
 */
template <typename T>
class FakeMutableHandle
    : public js::MutableHandleBase<T, FakeMutableHandle<T>> {
 public:
  using ElementType = T;

  MOZ_IMPLICIT FakeMutableHandle(T* t) : ptr(t) {}

  MOZ_IMPLICIT FakeMutableHandle(FakeRooted<T>* root) : ptr(root->address()) {}

  void set(const T& v) { *ptr = v; }

  DECLARE_POINTER_CONSTREF_OPS(T);
  DECLARE_NONPOINTER_ACCESSOR_METHODS(*ptr);
  DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(*ptr);

 private:
  FakeMutableHandle() : ptr(nullptr) {}
  DELETE_ASSIGNMENT_OPS(FakeMutableHandle, T);

  T* ptr;
};

}  // namespace js

namespace JS {

namespace detail {

template <typename T>
struct DefineComparisonOps<js::FakeMutableHandle<T>> : std::true_type {
  static const T& get(const js::FakeMutableHandle<T>& v) { return v.get(); }
};

}  // namespace detail

}  // namespace JS

namespace js {

/**
 * Types for a variable that either should or shouldn't be rooted, depending on
 * the template parameter allowGC. Used for implementing functions that can
 * operate on either rooted or unrooted data.
 */

template <typename T, AllowGC allowGC>
class MaybeRooted;

template <typename T>
class MaybeRooted<T, CanGC> {
 public:
  using HandleType = JS::Handle<T>;
  using RootType = JS::Rooted<T>;
  using MutableHandleType = JS::MutableHandle<T>;
};

template <typename T>
class MaybeRooted<T, NoGC> {
 public:
  using HandleType = const T&;
  using RootType = FakeRooted<T>;
  using MutableHandleType = FakeMutableHandle<T>;
};

}  // namespace js

#endif  // gc_MaybeRooted_h
