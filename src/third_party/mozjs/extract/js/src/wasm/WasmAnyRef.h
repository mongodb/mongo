/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2023 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_anyref_h
#define wasm_anyref_h

#include "mozilla/FloatingPoint.h"

#include <utility>

#include "js/HeapAPI.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"

// #include "NamespaceImports.h"

class JSObject;
class JSString;

namespace js {
namespace gc {
struct Cell;
};  // namespace gc

namespace wasm {

// [SMDOC] AnyRef
//
// An AnyRef is a boxed value that can represent any wasm reference type and any
// host type that the host system allows to flow into and out of wasm
// transparently.  It is a pointer-sized datum that has the same representation
// as all its subtypes (funcref, externref, eqref, (ref T), et al) due to the
// non-coercive subtyping of the wasm type system.
//
// The C++/wasm boundary always uses a 'void*' type to express AnyRef values, to
// emphasize the pointer-ness of the value.  The C++ code must transform the
// void* into an AnyRef by calling AnyRef::fromCompiledCode(), and transform an
// AnyRef into a void* by calling AnyRef::toCompiledCode().  Once in C++, we use
// AnyRef everywhere.  A JS Value is transformed into an AnyRef by calling
// AnyRef::fromJSValue(), and the AnyRef is transformed into a JS Value by
// calling AnyRef::toJSValue().
//
// NOTE that AnyRef values may point to GC'd storage and as such need to be
// rooted if they are kept live in boxed form across code that may cause GC!
// Use RootedAnyRef / HandleAnyRef / MutableHandleAnyRef where necessary.
//
// The lowest bits of the pointer value are used for tagging, to allow for some
// representation optimizations and to distinguish various types.
//
// The current tagging scheme is:
//   if (pointer == 0) then 'null'
//   if (pointer & 0x1) then 'i31'
//   if (pointer & 0x2) then 'string'
//   else 'object'
//
// NOTE: there is sequencing required when checking tags. If bit 0x1 is set,
// then bit 0x2 is part of the i31 value and does not imply string.
//
// An i31ref value has no sign interpretation within wasm, where instructions
// specify the signedness. When converting to/from a JS value, an i31ref value
// is treated as a signed 31-bit value.

// The kind of value stored in an AnyRef. This is not 1:1 with the pointer tag
// of AnyRef as this separates the 'Null' and 'Object' cases which are
// collapsed in the pointer tag.
enum class AnyRefKind : uint8_t {
  Null,
  Object,
  String,
  I31,
};

// The pointer tag of an AnyRef.
enum class AnyRefTag : uint8_t {
  // This value is either a JSObject& or a null pointer.
  ObjectOrNull = 0x0,
  // This value is a 31-bit integer.
  I31 = 0x1,
  // This value is a JSString*.
  String = 0x2,
};

// A reference to any wasm reference type or host (JS) value. AnyRef is
// optimized for efficient access to objects, strings, and 31-bit integers.
//
// See the above documentation comment for more details.
class AnyRef {
  uintptr_t value_;

  // Get the pointer tag stored in value_.
  AnyRefTag pointerTag() const { return GetUintptrTag(value_); }

  explicit AnyRef(uintptr_t value) : value_(value) {}

  static constexpr uintptr_t TagUintptr(uintptr_t value, AnyRefTag tag) {
    MOZ_ASSERT(!(value & TagMask));
    return value | uintptr_t(tag);
  }
  static constexpr uintptr_t UntagUintptr(uintptr_t value) {
    return value & ~TagMask;
  }
  static constexpr AnyRefTag GetUintptrTag(uintptr_t value) {
    // Mask off all but the lowest two-bits (the tag)
    uintptr_t rawTag = value & TagMask;
    // If the lowest bit is set, we want to normalize and only return
    // AnyRefTag::I31. Mask off the high-bit iff the low-bit was set.
    uintptr_t normalizedI31 = rawTag & ~(value << 1);
    return AnyRefTag(normalizedI31);
  }

  // Given a 32-bit signed integer within 31-bit signed bounds, turn it into
  // an AnyRef.
  static AnyRef fromInt32(int32_t value) {
    MOZ_ASSERT(!int32NeedsBoxing(value));
    return AnyRef::fromUint32Truncate(uint32_t(value));
  }

 public:
  static constexpr uintptr_t TagMask = 0x3;
  static constexpr uintptr_t TagShift = 2;
  static_assert(TagShift <= gc::CellAlignShift, "not enough free bits");
  // A mask for getting the GC thing an AnyRef represents.
  static constexpr uintptr_t GCThingMask = ~TagMask;
  // A combined mask for getting the gc::Chunk for an AnyRef that is a GC
  // thing.
  static constexpr uintptr_t GCThingChunkMask =
      GCThingMask & ~js::gc::ChunkMask;

  // The representation of a null reference value throughout the compiler for
  // when we need an integer constant. This is asserted to be equivalent to
  // nullptr in wasm::Init.
  static constexpr uintptr_t NullRefValue = 0;
  static constexpr uintptr_t InvalidRefValue = UINTPTR_MAX << TagShift;

  // The inclusive maximum 31-bit signed integer, 2^30 - 1.
  static constexpr int32_t MaxI31Value = (2 << 29) - 1;
  // The inclusive minimum 31-bit signed integer, -2^30.
  static constexpr int32_t MinI31Value = -(2 << 29);

  explicit AnyRef() : value_(NullRefValue) {}
  MOZ_IMPLICIT AnyRef(std::nullptr_t) : value_(NullRefValue) {}

  // The null AnyRef value.
  static AnyRef null() { return AnyRef(NullRefValue); }

  // An invalid AnyRef cannot arise naturally from wasm and so can be used as
  // a sentinel value to indicate failure from an AnyRef-returning function.
  static AnyRef invalid() { return AnyRef(InvalidRefValue); }

  // Given a JSObject* that comes from JS, turn it into AnyRef.
  static AnyRef fromJSObjectOrNull(JSObject* objectOrNull) {
    MOZ_ASSERT(GetUintptrTag((uintptr_t)objectOrNull) ==
               AnyRefTag::ObjectOrNull);
    return AnyRef((uintptr_t)objectOrNull);
  }

  // Given a JSObject& that comes from JS, turn it into AnyRef.
  static AnyRef fromJSObject(JSObject& object) {
    MOZ_ASSERT(GetUintptrTag((uintptr_t)&object) == AnyRefTag::ObjectOrNull);
    return AnyRef((uintptr_t)&object);
  }

  // Given a JSString* that comes from JS, turn it into AnyRef.
  static AnyRef fromJSString(JSString* string) {
    return AnyRef(TagUintptr((uintptr_t)string, AnyRefTag::String));
  }

  // Given a void* that comes from compiled wasm code, turn it into AnyRef.
  static AnyRef fromCompiledCode(void* pointer) {
    return AnyRef((uintptr_t)pointer);
  }

  // Given a JS value, turn it into AnyRef. This returns false if boxing the
  // value failed due to an OOM.
  static bool fromJSValue(JSContext* cx, JS::HandleValue value,
                          JS::MutableHandle<AnyRef> result);

  // fromUint32Truncate will produce an i31 from an int32 by truncating the
  // highest bit. For values in the 31-bit range, this losslessly preserves the
  // value. For values outside the 31-bit range, this performs 31-bit
  // wraparound.
  //
  // There are four cases here based on the two high bits:
  //   00 - [0, MaxI31Value]
  //   01 - (MaxI31Value, INT32_MAX]
  //   10 - [INT32_MIN, MinI31Value)
  //   11 - [MinI31Value, -1]
  //
  // The middle two cases can be ruled out if the value is guaranteed to be
  // within the i31 range. Therefore if we truncate the high bit upon converting
  // to i31 and perform a signed widening upon converting back to i32, we can
  // losslessly represent all i31 values.
  static AnyRef fromUint32Truncate(uint32_t value) {
    // See 64-bit GPRs carrying 32-bit values invariants in MacroAssember.h
#if defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_X64) || \
    defined(JS_CODEGEN_ARM64)
    // Truncate the value to the 31-bit value size.
    uintptr_t wideValue = uintptr_t(value & 0x7FFFFFFF);
#elif defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_RISCV64)
    // Sign extend the value to the native pointer size.
    uintptr_t wideValue = uintptr_t(int64_t((uint64_t(value) << 33)) >> 33);
#elif !defined(JS_64BIT)
    // Transfer 32-bit value as is.
    uintptr_t wideValue = (uintptr_t)value;
#else
#  error "unknown architecture"
#endif

    // Left shift the value by 1, truncating the high bit.
    uintptr_t shiftedValue = wideValue << 1;
    uintptr_t taggedValue = shiftedValue | (uintptr_t)AnyRefTag::I31;
#ifdef JS_64BIT
    debugAssertCanonicalInt32(taggedValue);
#endif
    return AnyRef(taggedValue);
  }

#ifdef JS_64BIT
  // Ensure the value fits into a 32-bits integer on 64-bits platforms.
  static void debugAssertCanonicalInt32(uintptr_t value) {
#  ifdef DEBUG
#    if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64)
    MOZ_ASSERT(value <= UINT32_MAX);
#    endif
#  endif
  }
#endif

  static bool int32NeedsBoxing(int32_t value) {
    // We can represent every signed 31-bit number without boxing
    return value < MinI31Value || value > MaxI31Value;
  }

  static bool doubleNeedsBoxing(double value) {
    int32_t intValue;
    if (!mozilla::NumberIsInt32(value, &intValue)) {
      return true;
    }
    return int32NeedsBoxing(value);
  }

  // Returns whether a JS value will need to be boxed.
  static bool valueNeedsBoxing(JS::HandleValue value) {
    if (value.isObjectOrNull() || value.isString()) {
      return false;
    }
    if (value.isInt32()) {
      return int32NeedsBoxing(value.toInt32());
    }
    if (value.isDouble()) {
      return doubleNeedsBoxing(value.toDouble());
    }
    return true;
  }

  // Box a JS Value that needs boxing.
  static JSObject* boxValue(JSContext* cx, JS::HandleValue value);

  bool operator==(const AnyRef& rhs) const {
    return this->value_ == rhs.value_;
  }
  bool operator!=(const AnyRef& rhs) const { return !(*this == rhs); }

  // Check if this AnyRef is the invalid value.
  bool isInvalid() const { return *this == AnyRef::invalid(); }

  AnyRefKind kind() const {
    if (value_ == NullRefValue) {
      return AnyRefKind::Null;
    }
    switch (pointerTag()) {
      case AnyRefTag::ObjectOrNull: {
        // The invalid pattern uses the ObjectOrNull tag, check for it here.
        MOZ_ASSERT(!isInvalid());
        // We ruled out the null case above
        return AnyRefKind::Object;
      }
      case AnyRefTag::String: {
        return AnyRefKind::String;
      }
      case AnyRefTag::I31: {
        return AnyRefKind::I31;
      }
      default: {
        MOZ_CRASH("unknown AnyRef tag");
      }
    }
  }

  bool isNull() const { return value_ == NullRefValue; }
  bool isGCThing() const { return !isNull() && !isI31(); }
  bool isJSObject() const { return kind() == AnyRefKind::Object; }
  bool isJSString() const { return kind() == AnyRefKind::String; }
  bool isI31() const { return kind() == AnyRefKind::I31; }

  gc::Cell* toGCThing() const {
    MOZ_ASSERT(isGCThing());
    return (gc::Cell*)UntagUintptr(value_);
  }
  JSObject& toJSObject() const {
    MOZ_ASSERT(isJSObject());
    return *(JSObject*)value_;
  }
  JSObject* toJSObjectOrNull() const {
    MOZ_ASSERT(!isInvalid());
    MOZ_ASSERT(pointerTag() == AnyRefTag::ObjectOrNull);
    return (JSObject*)value_;
  }
  JSString* toJSString() const {
    MOZ_ASSERT(isJSString());
    return (JSString*)UntagUintptr(value_);
  }
  // Unpack an i31, interpreting the integer as signed.
  int32_t toI31() const {
    MOZ_ASSERT(isI31());
#ifdef JS_64BIT
    debugAssertCanonicalInt32(value_);
#endif
    // On 64-bit targets, we only care about the low 4-bytes.
    uint32_t truncatedValue;
    memcpy(&truncatedValue, &value_, sizeof(uint32_t));
    // Perform a right arithmetic shift (see AnyRef::fromI31 for more details),
    // avoiding undefined behavior by using an unsigned type.
    uint32_t shiftedValue = value_ >> 1;
    if ((truncatedValue & (1 << 31)) != 0) {
      shiftedValue |= (1 << 31);
    }
    // Perform a bitwise cast to see the result as a signed value.
    return mozilla::BitwiseCast<int32_t>(shiftedValue);
  }

  // Convert from AnyRef to a JS Value. This currently does not require any
  // allocation. If this changes in the future, this function will become
  // more complicated.
  JS::Value toJSValue() const;

  // Get the raw value for returning to wasm code.
  void* forCompiledCode() const { return (void*)value_; }

  // Get the raw value for diagnostics.
  uintptr_t rawValue() const { return value_; }

  // Internal details of the boxing format used by WasmStubs.cpp
  static const JSClass* valueBoxClass();
  static size_t valueBoxOffsetOfValue();
};

using RootedAnyRef = JS::Rooted<AnyRef>;
using HandleAnyRef = JS::Handle<AnyRef>;
using MutableHandleAnyRef = JS::MutableHandle<AnyRef>;

}  // namespace wasm

template <class Wrapper>
class WrappedPtrOperations<wasm::AnyRef, Wrapper> {
  const wasm::AnyRef& value() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  bool isNull() const { return value().isNull(); }
  bool isI31() const { return value().isI31(); }
  bool isJSObject() const { return value().isJSObject(); }
  bool isJSString() const { return value().isJSString(); }
  JSObject& toJSObject() const { return value().toJSObject(); }
  JSString* toJSString() const { return value().toJSString(); }
};

// If the Value is a GC pointer type, call |f| with the pointer cast to that
// type and return the result wrapped in a Maybe, otherwise return None().
template <typename F>
auto MapGCThingTyped(const wasm::AnyRef& val, F&& f) {
  switch (val.kind()) {
    case wasm::AnyRefKind::Object:
      return mozilla::Some(f(&val.toJSObject()));
    case wasm::AnyRefKind::String:
      return mozilla::Some(f(val.toJSString()));
    case wasm::AnyRefKind::I31:
    case wasm::AnyRefKind::Null: {
      using ReturnType = decltype(f(static_cast<JSObject*>(nullptr)));
      return mozilla::Maybe<ReturnType>();
    }
  }
  MOZ_CRASH();
}

template <typename F>
bool ApplyGCThingTyped(const wasm::AnyRef& val, F&& f) {
  return MapGCThingTyped(val,
                         [&f](auto t) {
                           f(t);
                           return true;
                         })
      .isSome();
}

}  // namespace js

namespace JS {

template <>
struct GCPolicy<js::wasm::AnyRef> {
  static void trace(JSTracer* trc, js::wasm::AnyRef* v, const char* name) {
    // This should only be called as part of root marking since that's the only
    // time we should trace unbarriered GC thing pointers. This will assert if
    // called at other times.
    TraceRoot(trc, v, name);
  }
  static bool isValid(const js::wasm::AnyRef& v) {
    return !v.isGCThing() || js::gc::IsCellPointerValid(v.toGCThing());
  }
};

}  // namespace JS

#endif  // wasm_anyref_h
