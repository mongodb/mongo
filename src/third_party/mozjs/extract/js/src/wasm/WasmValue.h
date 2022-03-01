/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
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

#ifndef wasm_val_h
#define wasm_val_h

#include "js/Class.h"  // JSClassOps, ClassSpec
#include "vm/JSObject.h"
#include "vm/NativeObject.h"  // NativeObject
#include "wasm/WasmValType.h"

namespace js {
namespace wasm {

// A V128 value.

struct V128 {
  uint8_t bytes[16];  // Little-endian

  V128() { memset(bytes, 0, sizeof(bytes)); }

  template <typename T>
  T extractLane(unsigned lane) const {
    T result;
    MOZ_ASSERT(lane < 16 / sizeof(T));
    memcpy(&result, bytes + sizeof(T) * lane, sizeof(T));
    return result;
  }

  template <typename T>
  void insertLane(unsigned lane, T value) {
    MOZ_ASSERT(lane < 16 / sizeof(T));
    memcpy(bytes + sizeof(T) * lane, &value, sizeof(T));
  }

  bool operator==(const V128& rhs) const {
    for (size_t i = 0; i < sizeof(bytes); i++) {
      if (bytes[i] != rhs.bytes[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const V128& rhs) const { return !(*this == rhs); }
};

static_assert(sizeof(V128) == 16, "Invariant");

// An AnyRef is a boxed value that can represent any wasm reference type and any
// host type that the host system allows to flow into and out of wasm
// transparently.  It is a pointer-sized datum that has the same representation
// as all its subtypes (funcref, externref, eqref, (ref T), et al) due to the
// non-coercive subtyping of the wasm type system.  Its current representation
// is a plain JSObject*, and the private JSObject subtype WasmValueBox is used
// to box non-object non-null JS values.
//
// The C++/wasm boundary always uses a 'void*' type to express AnyRef values, to
// emphasize the pointer-ness of the value.  The C++ code must transform the
// void* into an AnyRef by calling AnyRef::fromCompiledCode(), and transform an
// AnyRef into a void* by calling AnyRef::toCompiledCode().  Once in C++, we use
// AnyRef everywhere.  A JS Value is transformed into an AnyRef by calling
// AnyRef::box(), and the AnyRef is transformed into a JS Value by calling
// AnyRef::unbox().
//
// NOTE that AnyRef values may point to GC'd storage and as such need to be
// rooted if they are kept live in boxed form across code that may cause GC!
// Use RootedAnyRef / HandleAnyRef / MutableHandleAnyRef where necessary.
//
// The lowest bits of the pointer value are used for tagging, to allow for some
// representation optimizations and to distinguish various types.

// For version 0, we simply equate AnyRef and JSObject* (this means that there
// are technically no tags at all yet).  We use a simple boxing scheme that
// wraps a JS value that is not already JSObject in a distinguishable JSObject
// that holds the value, see WasmTypes.cpp for details.  Knowledge of this
// mapping is embedded in CodeGenerator.cpp (in WasmBoxValue and
// WasmAnyRefFromJSObject) and in WasmStubs.cpp (in functions Box* and Unbox*).

class AnyRef {
  // mutable so that tracing may access a JSObject* from a `const Val` or
  // `const AnyRef`.
  mutable JSObject* value_;

  explicit AnyRef() : value_((JSObject*)-1) {}
  explicit AnyRef(JSObject* p) : value_(p) {
    MOZ_ASSERT(((uintptr_t)p & 0x03) == 0);
  }

 public:
  // An invalid AnyRef cannot arise naturally from wasm and so can be used as
  // a sentinel value to indicate failure from an AnyRef-returning function.
  static AnyRef invalid() { return AnyRef(); }

  // Given a void* that comes from compiled wasm code, turn it into AnyRef.
  static AnyRef fromCompiledCode(void* p) { return AnyRef((JSObject*)p); }

  // Given a JSObject* that comes from JS, turn it into AnyRef.
  static AnyRef fromJSObject(JSObject* p) { return AnyRef(p); }

  // Generate an AnyRef null pointer.
  static AnyRef null() { return AnyRef(nullptr); }

  bool isNull() const { return value_ == nullptr; }

  bool operator==(const AnyRef& rhs) const {
    return this->value_ == rhs.value_;
  }

  bool operator!=(const AnyRef& rhs) const { return !(*this == rhs); }

  void* forCompiledCode() const { return value_; }

  JSObject* asJSObject() const { return value_; }

  JSObject** asJSObjectAddress() const { return &value_; }

  void trace(JSTracer* trc);

  // Tags (to be developed further)
  static constexpr uintptr_t AnyRefTagMask = 1;
  static constexpr uintptr_t AnyRefObjTag = 0;
};

using RootedAnyRef = Rooted<AnyRef>;
using HandleAnyRef = Handle<AnyRef>;
using MutableHandleAnyRef = MutableHandle<AnyRef>;

// TODO/AnyRef-boxing: With boxed immediates and strings, these will be defined
// as MOZ_CRASH or similar so that we can find all locations that need to be
// fixed.

#define ASSERT_ANYREF_IS_JSOBJECT (void)(0)
#define STATIC_ASSERT_ANYREF_IS_JSOBJECT static_assert(1, "AnyRef is JSObject")

// Given any JS value, box it as an AnyRef and store it in *result.  Returns
// false on OOM.

bool BoxAnyRef(JSContext* cx, HandleValue val, MutableHandleAnyRef result);

// Given a JS value that requires an object box, box it as an AnyRef and return
// it, returning nullptr on OOM.
//
// Currently the values requiring a box are those other than JSObject* or
// nullptr, but in the future more values will be represented without an
// allocation.
JSObject* BoxBoxableValue(JSContext* cx, HandleValue val);

// Given any AnyRef, unbox it as a JS Value.  If it is a reference to a wasm
// object it will be reflected as a JSObject* representing some TypedObject
// instance.

Value UnboxAnyRef(AnyRef val);

class WasmValueBox : public NativeObject {
  static const unsigned VALUE_SLOT = 0;

 public:
  static const unsigned RESERVED_SLOTS = 1;
  static const JSClass class_;

  static WasmValueBox* create(JSContext* cx, HandleValue val);
  Value value() const { return getFixedSlot(VALUE_SLOT); }
  static size_t offsetOfValue() {
    return NativeObject::getFixedSlotOffset(VALUE_SLOT);
  }
};

// A FuncRef is a JSFunction* and is hence also an AnyRef, and the remarks above
// about AnyRef apply also to FuncRef.  When 'funcref' is used as a value type
// in wasm code, the value that is held is "the canonical function value", which
// is a function for which IsWasmExportedFunction() is true, and which has the
// correct identity wrt reference equality of functions.  Notably, if a function
// is imported then its ref.func value compares === in JS to the function that
// was passed as an import when the instance was created.
//
// These rules ensure that casts from funcref to anyref are non-converting
// (generate no code), and that no wrapping or unwrapping needs to happen when a
// funcref or anyref flows across the JS/wasm boundary, and that functions have
// the necessary identity when observed from JS, and in the future, from wasm.
//
// Functions stored in tables, whether wasm tables or internal tables, can be
// stored in a form that optimizes for eg call speed, however.
//
// Reading a funcref from a funcref table, writing a funcref to a funcref table,
// and generating the value for a ref.func instruction are therefore nontrivial
// operations that require mapping between the canonical JSFunction and the
// optimized table representation.  Once we get an instruction to call a
// ref.func directly it too will require such a mapping.

// In many cases, a FuncRef is exactly the same as AnyRef and we can use AnyRef
// functionality on funcref values.  The FuncRef class exists mostly to add more
// checks and to make it clear, when we need to, that we're manipulating funcref
// values.  FuncRef does not currently subclass AnyRef because there's been no
// need to, but it probably could.

class FuncRef {
  JSFunction* value_;

  explicit FuncRef() : value_((JSFunction*)-1) {}
  explicit FuncRef(JSFunction* p) : value_(p) {
    MOZ_ASSERT(((uintptr_t)p & 0x03) == 0);
  }

 public:
  // Given a void* that comes from compiled wasm code, turn it into FuncRef.
  static FuncRef fromCompiledCode(void* p) { return FuncRef((JSFunction*)p); }

  // Given a JSFunction* that comes from JS, turn it into FuncRef.
  static FuncRef fromJSFunction(JSFunction* p) { return FuncRef(p); }

  // Given an AnyRef that represents a possibly-null funcref, turn it into a
  // FuncRef.
  static FuncRef fromAnyRefUnchecked(AnyRef p);

  AnyRef asAnyRef() { return AnyRef::fromJSObject((JSObject*)value_); }

  void* forCompiledCode() const { return value_; }

  JSFunction* asJSFunction() { return value_; }

  bool isNull() { return value_ == nullptr; }
};

using RootedFuncRef = Rooted<FuncRef>;
using HandleFuncRef = Handle<FuncRef>;
using MutableHandleFuncRef = MutableHandle<FuncRef>;

// Given any FuncRef, unbox it as a JS Value -- always a JSFunction*.

Value UnboxFuncRef(FuncRef val);

// The LitVal class represents a single WebAssembly value of a given value
// type, mostly for the purpose of numeric literals and initializers. A LitVal
// does not directly map to a JS value since there is not (currently) a precise
// representation of i64 values. A LitVal may contain non-canonical NaNs since,
// within WebAssembly, floats are not canonicalized. Canonicalization must
// happen at the JS boundary.

class LitVal {
 public:
  union Cell {
    int32_t i32_;
    int64_t i64_;
    float f32_;
    double f64_;
    wasm::V128 v128_;
    wasm::AnyRef ref_;
    Cell() : v128_() {}
    ~Cell() = default;
  };

 protected:
  ValType type_;
  Cell cell_;

 public:
  LitVal() : type_(ValType()), cell_{} {}

  explicit LitVal(ValType type) : type_(type) {
    MOZ_ASSERT(type.isDefaultable());
    switch (type.kind()) {
      case ValType::Kind::I32: {
        cell_.i32_ = 0;
        break;
      }
      case ValType::Kind::I64: {
        cell_.i64_ = 0;
        break;
      }
      case ValType::Kind::F32: {
        cell_.f32_ = 0;
        break;
      }
      case ValType::Kind::F64: {
        cell_.f64_ = 0;
        break;
      }
      case ValType::Kind::V128: {
        new (&cell_.v128_) V128();
        break;
      }
      case ValType::Kind::Ref: {
        cell_.ref_ = AnyRef::null();
        break;
      }
      case ValType::Kind::Rtt: {
        MOZ_CRASH("not defaultable");
      }
    }
  }

  explicit LitVal(uint32_t i32) : type_(ValType::I32) { cell_.i32_ = i32; }
  explicit LitVal(uint64_t i64) : type_(ValType::I64) { cell_.i64_ = i64; }

  explicit LitVal(float f32) : type_(ValType::F32) { cell_.f32_ = f32; }
  explicit LitVal(double f64) : type_(ValType::F64) { cell_.f64_ = f64; }

  explicit LitVal(V128 v128) : type_(ValType::V128) { cell_.v128_ = v128; }

  explicit LitVal(ValType type, AnyRef any) : type_(type) {
    MOZ_ASSERT(type.isReference());
    MOZ_ASSERT(any.isNull(),
               "use Val for non-nullptr ref types to get tracing");
    cell_.ref_ = any;
  }

  ValType type() const { return type_; }
  static constexpr size_t sizeofLargestValue() { return sizeof(cell_); }

  Cell& cell() { return cell_; }
  const Cell& cell() const { return cell_; }

  uint32_t i32() const {
    MOZ_ASSERT(type_ == ValType::I32);
    return cell_.i32_;
  }
  uint64_t i64() const {
    MOZ_ASSERT(type_ == ValType::I64);
    return cell_.i64_;
  }
  const float& f32() const {
    MOZ_ASSERT(type_ == ValType::F32);
    return cell_.f32_;
  }
  const double& f64() const {
    MOZ_ASSERT(type_ == ValType::F64);
    return cell_.f64_;
  }
  AnyRef ref() const {
    MOZ_ASSERT(type_.isReference());
    return cell_.ref_;
  }
  const V128& v128() const {
    MOZ_ASSERT(type_ == ValType::V128);
    return cell_.v128_;
  }
};

// A Val is a LitVal that can contain (non-null) pointers to GC things. All Vals
// must be used with the rooting APIs as they may contain JS objects.

class MOZ_NON_PARAM Val : public LitVal {
 public:
  Val() : LitVal() {}
  explicit Val(ValType type) : LitVal(type) {}
  explicit Val(const LitVal& val);
  explicit Val(uint32_t i32) : LitVal(i32) {}
  explicit Val(uint64_t i64) : LitVal(i64) {}
  explicit Val(float f32) : LitVal(f32) {}
  explicit Val(double f64) : LitVal(f64) {}
  explicit Val(V128 v128) : LitVal(v128) {}
  explicit Val(ValType type, AnyRef val) : LitVal(type, AnyRef::null()) {
    MOZ_ASSERT(type.isReference());
    cell_.ref_ = val;
  }
  explicit Val(ValType type, FuncRef val) : LitVal(type, AnyRef::null()) {
    MOZ_ASSERT(type.isFuncRef());
    cell_.ref_ = val.asAnyRef();
  }

  Val(const Val&) = default;
  Val& operator=(const Val&) = default;

  bool operator==(const Val& rhs) const {
    if (type_ != rhs.type_) {
      return false;
    }
    switch (type_.kind()) {
      case ValType::I32:
        return cell_.i32_ == rhs.cell_.i32_;
      case ValType::I64:
        return cell_.i64_ == rhs.cell_.i64_;
      case ValType::F32:
        return cell_.f32_ == rhs.cell_.f32_;
      case ValType::F64:
        return cell_.f64_ == rhs.cell_.f64_;
      case ValType::V128:
        return cell_.v128_ == rhs.cell_.v128_;
      case ValType::Rtt:
      case ValType::Ref:
        return cell_.ref_ == rhs.cell_.ref_;
    }
    MOZ_ASSERT_UNREACHABLE();
    return false;
  }
  bool operator!=(const Val& rhs) const { return !(*this == rhs); }

  bool isJSObject() const {
    return type_.isValid() && type_.isReference() && !cell_.ref_.isNull();
  }

  JSObject* asJSObject() const {
    MOZ_ASSERT(isJSObject());
    return cell_.ref_.asJSObject();
  }

  JSObject** asJSObjectAddress() const {
    return cell_.ref_.asJSObjectAddress();
  }

  void readFromRootedLocation(const void* loc);
  void writeToRootedLocation(void* loc, bool mustWrite64) const;

  // See the comment for `ToWebAssemblyValue` below.
  static bool fromJSValue(JSContext* cx, ValType targetType, HandleValue val,
                          MutableHandle<Val> rval);
  // See the comment for `ToJSValue` below.
  bool toJSValue(JSContext* cx, MutableHandleValue rval) const;

  void trace(JSTracer* trc) const;
};

using GCPtrVal = GCPtr<Val>;
using RootedVal = Rooted<Val>;
using HandleVal = Handle<Val>;
using MutableHandleVal = MutableHandle<Val>;

using ValVector = GCVector<Val, 0, SystemAllocPolicy>;
using RootedValVector = Rooted<ValVector>;
using HandleValVector = Handle<ValVector>;
using MutableHandleValVector = MutableHandle<ValVector>;

// Check a value against the given reference type.  If the targetType
// is RefType::Extern then the test always passes, but the value may be boxed.
// If the test passes then the value is stored either in fnval (for
// RefType::Func) or in refval (for other types); this split is not strictly
// necessary but is convenient for the users of this function.
//
// This can return false if the type check fails, or if a boxing into AnyRef
// throws an OOM.
[[nodiscard]] extern bool CheckRefType(JSContext* cx, RefType targetType,
                                       HandleValue v,
                                       MutableHandleFunction fnval,
                                       MutableHandleAnyRef refval);

// The same as above for when the target type is 'funcref'.
[[nodiscard]] extern bool CheckFuncRefValue(JSContext* cx, HandleValue v,
                                            MutableHandleFunction fun);

// The same as above for when the target type is 'eqref'.
[[nodiscard]] extern bool CheckEqRefValue(JSContext* cx, HandleValue v,
                                          MutableHandleAnyRef vp);
class NoDebug;
class DebugCodegenVal;

// The level of coercion to apply in `ToWebAssemblyValue` and `ToJSValue`.
enum class CoercionLevel {
  // The default coercions given by the JS-API specification.
  Spec,
  // Allow for the coercions given by `Spec` but also use WebAssembly.Global
  // as a container for lossless conversions. This is only available through
  // the wasmLosslessInvoke testing function and is used in tests.
  Lossless,
};

// Coercion function from a JS value to a WebAssembly value [1].
//
// This function may fail for any of the following reasons:
//  * The input value has an incorrect type for the targetType
//  * The targetType is not exposable
//  * An OOM ocurred
// An error will be set upon failure.
//
// [1] https://webassembly.github.io/spec/js-api/index.html#towebassemblyvalue
template <typename Debug = NoDebug>
extern bool ToWebAssemblyValue(JSContext* cx, HandleValue val, FieldType type,
                               void* loc, bool mustWrite64,
                               CoercionLevel level = CoercionLevel::Spec);
template <typename Debug = NoDebug>
extern bool ToWebAssemblyValue(JSContext* cx, HandleValue val, ValType type,
                               void* loc, bool mustWrite64,
                               CoercionLevel level = CoercionLevel::Spec);

// Coercion function from a WebAssembly value to a JS value [1].
//
// This function will only fail if an OOM ocurred. If the type of WebAssembly
// value being coerced is not exposable to JS, then it will be coerced to
// 'undefined'. Callers are responsible for guarding against this if this is
// not desirable.
//
// [1] https://webassembly.github.io/spec/js-api/index.html#tojsvalue
template <typename Debug = NoDebug>
extern bool ToJSValue(JSContext* cx, const void* src, FieldType type,
                      MutableHandleValue dst,
                      CoercionLevel level = CoercionLevel::Spec);
template <typename Debug = NoDebug>
extern bool ToJSValue(JSContext* cx, const void* src, ValType type,
                      MutableHandleValue dst,
                      CoercionLevel level = CoercionLevel::Spec);

}  // namespace wasm

template <>
struct InternalBarrierMethods<wasm::Val> {
  STATIC_ASSERT_ANYREF_IS_JSOBJECT;

  static bool isMarkable(const wasm::Val& v) { return v.isJSObject(); }

  static void preBarrier(const wasm::Val& v) {
    if (v.isJSObject()) {
      gc::PreWriteBarrier(v.asJSObject());
    }
  }

  static MOZ_ALWAYS_INLINE void postBarrier(wasm::Val* vp,
                                            const wasm::Val& prev,
                                            const wasm::Val& next) {
    MOZ_RELEASE_ASSERT(!prev.type().isValid() || prev.type() == next.type());
    JSObject* prevObj = prev.isJSObject() ? prev.asJSObject() : nullptr;
    JSObject* nextObj = next.isJSObject() ? next.asJSObject() : nullptr;
    if (nextObj) {
      JSObject::postWriteBarrier(vp->asJSObjectAddress(), prevObj, nextObj);
    }
  }

  static void readBarrier(const wasm::Val& v) {
    if (v.isJSObject()) {
      gc::ReadBarrier(v.asJSObject());
    }
  }

#ifdef DEBUG
  static void assertThingIsNotGray(const wasm::Val& v) {
    if (v.isJSObject()) {
      JS::AssertObjectIsNotGray(v.asJSObject());
    }
  }
#endif
};

}  // namespace js

#endif  // wasm_val_h
