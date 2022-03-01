/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** GC-safe representations of consecutive JS::Value in memory. */

#ifndef js_ValueArray_h
#define js_ValueArray_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_IMPLICIT, MOZ_RAII

#include <stdint.h>  // size_t

#include "js/CallArgs.h"    // JS::CallArgs
#include "js/GCVector.h"    // JS::RootedVector
#include "js/RootingAPI.h"  // JS::AutoGCRooter, JS::{,Mutable}Handle
#include "js/Value.h"       // JS::Value

namespace js {
JS_PUBLIC_API void TraceValueArray(JSTracer* trc, size_t length,
                                   JS::Value* elements);
}  // namespace js

namespace JS {

/* A fixed-size array of values, for use inside Rooted<>. */
template <size_t N>
struct ValueArray {
  Value elements[N];
  void trace(JSTracer* trc) { js::TraceValueArray(trc, N, elements); }
};

/** RootedValueArray roots an internal fixed-size array of Values. */
template <size_t N>
using RootedValueArray = Rooted<ValueArray<N>>;

/**
 * A generic handle to an array of rooted values.
 *
 * The rooted array refernced can take several forms, therfore this is not the
 * same as Handle<js::ValueArray>.
 */
class HandleValueArray {
  const size_t length_;
  const Value* const elements_;

  HandleValueArray(size_t len, const Value* elements)
      : length_(len), elements_(elements) {}

 public:
  explicit HandleValueArray(Handle<Value> value)
      : length_(1), elements_(value.address()) {}

  MOZ_IMPLICIT HandleValueArray(const RootedVector<Value>& values)
      : length_(values.length()), elements_(values.begin()) {}

  template <size_t N>
  MOZ_IMPLICIT HandleValueArray(const RootedValueArray<N>& values)
      : length_(N), elements_(values.begin()) {}

  /** CallArgs must already be rooted somewhere up the stack. */
  MOZ_IMPLICIT HandleValueArray(const JS::CallArgs& args)
      : length_(args.length()), elements_(args.array()) {}

  /** Use with care! Only call this if the data is guaranteed to be marked. */
  static HandleValueArray fromMarkedLocation(size_t len,
                                             const Value* elements) {
    return HandleValueArray(len, elements);
  }

  static HandleValueArray subarray(const HandleValueArray& values,
                                   size_t startIndex, size_t len) {
    MOZ_ASSERT(startIndex + len <= values.length());
    return HandleValueArray(len, values.begin() + startIndex);
  }

  static HandleValueArray empty() { return HandleValueArray(0, nullptr); }

  size_t length() const { return length_; }
  const Value* begin() const { return elements_; }

  Handle<Value> operator[](size_t i) const {
    MOZ_ASSERT(i < length_);
    return Handle<Value>::fromMarkedLocation(&elements_[i]);
  }
};

}  // namespace JS

namespace js {

template <size_t N, typename Container>
class WrappedPtrOperations<JS::ValueArray<N>, Container> {
  const JS::ValueArray<N>& array() const {
    return static_cast<const Container*>(this)->get();
  }

 public:
  size_t length() const { return N; }
  const JS::Value* begin() const { return array().elements; }

  JS::HandleValue operator[](size_t i) const {
    MOZ_ASSERT(i < N);
    return JS::HandleValue::fromMarkedLocation(&array().elements[i]);
  }
};

template <size_t N, typename Container>
class MutableWrappedPtrOperations<JS::ValueArray<N>, Container>
    : public WrappedPtrOperations<JS::ValueArray<N>, Container> {
  using Base = WrappedPtrOperations<JS::ValueArray<N>, Container>;
  JS::ValueArray<N>& array() { return static_cast<Container*>(this)->get(); }

 public:
  using Base::begin;
  JS::Value* begin() { return array().elements; }

  using Base::operator[];
  JS::MutableHandleValue operator[](size_t i) {
    MOZ_ASSERT(i < N);
    return JS::MutableHandleValue::fromMarkedLocation(&array().elements[i]);
  }
};

}  // namespace js

#endif  // js_ValueArray_h
