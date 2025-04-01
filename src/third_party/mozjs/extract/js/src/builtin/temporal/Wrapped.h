/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Wrapped_h
#define builtin_temporal_Wrapped_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <type_traits>

#include "gc/Tracer.h"
#include "js/RootingAPI.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"

namespace js::temporal {

/**
 * Type to represent possibly wrapped objects from a different compartment.
 *
 * This can be used to represent specific JSObject sub-classes in return types
 * without having to pass unwrapped objects around.
 */
template <class T>
class MOZ_STACK_CLASS Wrapped final {
  static_assert(std::is_pointer_v<T>);
  static_assert(std::is_convertible_v<T, NativeObject*>);

  using U = std::remove_pointer_t<T>;

  JSObject* ptr_ = nullptr;

 public:
  Wrapped() = default;

  MOZ_IMPLICIT Wrapped(decltype(nullptr)) : ptr_(nullptr) {}

  MOZ_IMPLICIT Wrapped(T ptr) : ptr_(ptr) {
    // No assertion needed when the object already has the correct type.
  }

  MOZ_IMPLICIT Wrapped(JSObject* ptr) : ptr_(ptr) {
    // Ensure the caller passed a valid pointer.
    MOZ_ASSERT_IF(ptr_, ptr_->canUnwrapAs<U>());
  }

  template <typename S>
  MOZ_IMPLICIT Wrapped(
      const JS::Rooted<S>& root,
      std::enable_if_t<std::is_convertible_v<S, T>, int> dummy = 0)
      : Wrapped(root.get()) {}

  MOZ_IMPLICIT Wrapped(const JS::Rooted<JSObject*>& root)
      : Wrapped(root.get()) {}

  template <typename S>
  MOZ_IMPLICIT Wrapped(
      const JS::Handle<S>& root,
      std::enable_if_t<std::is_convertible_v<S, T>, int> dummy = 0)
      : Wrapped(root.get()) {}

  MOZ_IMPLICIT Wrapped(const JS::Handle<JSObject*>& root)
      : Wrapped(root.get()) {}

  template <typename S>
  MOZ_IMPLICIT Wrapped(
      const JS::MutableHandle<S>& root,
      std::enable_if_t<std::is_convertible_v<S, T>, int> dummy = 0)
      : Wrapped(root.get()) {}

  MOZ_IMPLICIT Wrapped(const JS::MutableHandle<JSObject*>& root)
      : Wrapped(root.get()) {}

  Wrapped& operator=(decltype(nullptr)) {
    ptr_ = nullptr;
    return *this;
  }

  Wrapped& operator=(T ptr) {
    ptr_ = ptr;
    return *this;
  }

  explicit operator bool() const { return !!ptr_; }

  JSObject* operator->() const { return ptr_; }

  JSObject& operator*() const { return *ptr_; }

  JSObject* get() const { return ptr_; }

  operator JSObject*() const { return get(); }

  auto address() const { return &ptr_; }

  U& unwrap() const {
    MOZ_ASSERT(ptr_);

    // Direct unwrap because the constructor already verified the object can be
    // unwrapped.
    //
    // We use JSObject::unwrapAs() instead of JSObject::maybeUnwrapIf(), because
    // this is an unrooted Wrapped, so hazard analysis will ensure that no
    // wrappers have been invalidated, because wrapper invalidation generally
    // only happens in the same case as GC.
    //
    // Rooted Wrapped are accessed through their WrappedPtrOperations
    // specialization, which uses JSObject::maybeUnwrapIf() to handle the
    // wrapper invalidation case correctly.
    return ptr_->unwrapAs<U>();
  }

  U* unwrapOrNull() const {
    // Direct unwrap because the constructor already verified the object can be
    // unwrapped.
    //
    // See Wrapped::unwrap() for why we don't call maybeUnwrapIf() here.
    return ptr_ ? &ptr_->unwrapAs<U>() : nullptr;
  }

  void trace(JSTracer* trc) { TraceNullableRoot(trc, &ptr_, "Wrapped::ptr_"); }
};

void ReportDeadWrapperOrAccessDenied(JSContext* cx, JSObject* obj);

} /* namespace js::temporal */

namespace js {
template <typename T, typename Container>
class WrappedPtrOperations<temporal::Wrapped<T>, Container> {
  using U = std::remove_pointer_t<T>;

  const auto& wrapped() const {
    return static_cast<const Container*>(this)->get();
  }

 public:
  explicit operator bool() const { return !!wrapped(); }

  JSObject* operator->() const { return wrapped().get(); }

  JSObject& operator*() const { return *wrapped().get(); }

  JS::Handle<JSObject*> object() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(wrapped().address());
  }

  operator JS::Handle<JSObject*>() const { return object(); }

  [[nodiscard]] U* unwrap(JSContext* cx) const {
    JSObject* obj = wrapped().get();

    // Call JSObject::maybeUnwrapIf() instead of JSObject::unwrapAs() in case
    // |obj| is an invalidated wrapper.
    if (auto* unwrapped = obj->maybeUnwrapIf<U>()) {
      return unwrapped;
    }

    temporal::ReportDeadWrapperOrAccessDenied(cx, obj);
    return nullptr;
  }
};
}  // namespace js

#endif /* builtin_temporal_Wrapped_h */
