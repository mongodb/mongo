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

#include <string.h>

#include "js/Class.h"  // JSClassOps, ClassSpec
#include "vm/JSObject.h"
#include "vm/NativeObject.h"  // NativeObject
#include "wasm/WasmAnyRef.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmTypeDef.h"

namespace js {
namespace wasm {

// A V128 value.

struct V128 {
  uint8_t bytes[16] = {};  // Little-endian

  WASM_CHECK_CACHEABLE_POD(bytes);

  V128() = default;

  explicit V128(uint8_t splatValue) {
    memset(bytes, int(splatValue), sizeof(bytes));
  }

  template <typename T>
  void extractLane(unsigned lane, T* result) const {
    MOZ_ASSERT(lane < 16 / sizeof(T));
    memcpy(result, bytes + sizeof(T) * lane, sizeof(T));
  }

  template <typename T>
  void insertLane(unsigned lane, T value) {
    MOZ_ASSERT(lane < 16 / sizeof(T));
    memcpy(bytes + sizeof(T) * lane, &value, sizeof(T));
  }

  bool operator==(const V128& rhs) const {
    return memcmp(bytes, rhs.bytes, sizeof(bytes)) == 0;
  }

  bool operator!=(const V128& rhs) const { return !(*this == rhs); }
};

WASM_DECLARE_CACHEABLE_POD(V128);

static_assert(sizeof(V128) == 16, "Invariant");

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
  // mutable so that tracing may access a JSFunction* from a `const FuncRef`
  mutable JSFunction* value_;

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

  static FuncRef null() { return FuncRef(nullptr); }

  AnyRef toAnyRef() { return AnyRef::fromJSObjectOrNull((JSObject*)value_); }

  void* forCompiledCode() const { return value_; }

  JSFunction* asJSFunction() { return value_; }

  bool isNull() const { return value_ == nullptr; }

  void trace(JSTracer* trc) const;
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
    uint32_t i32_;
    uint64_t i64_;
    float f32_;
    double f64_;
    wasm::V128 v128_;
    // Mutable so that it can be traced
    mutable wasm::AnyRef ref_;

    Cell() : v128_() {}
    ~Cell() = default;

    WASM_CHECK_CACHEABLE_POD(i32_, i64_, f32_, f64_, v128_);
    WASM_ALLOW_NON_CACHEABLE_POD_FIELD(
        ref_,
        "The pointer value in ref_ is guaranteed to always be null in a "
        "LitVal.");
  };

 protected:
  ValType type_;
  Cell cell_;

 public:
  LitVal() = default;

  explicit LitVal(ValType type) : type_(type) {
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
        cell_.ref_ = nullptr;
        break;
      }
    }
  }

  explicit LitVal(uint32_t i32) : type_(ValType::I32) { cell_.i32_ = i32; }
  explicit LitVal(uint64_t i64) : type_(ValType::I64) { cell_.i64_ = i64; }

  explicit LitVal(float f32) : type_(ValType::F32) { cell_.f32_ = f32; }
  explicit LitVal(double f64) : type_(ValType::F64) { cell_.f64_ = f64; }

  explicit LitVal(V128 v128) : type_(ValType::V128) { cell_.v128_ = v128; }

  explicit LitVal(ValType type, AnyRef any) : type_(type) {
    MOZ_ASSERT(type.isRefRepr());
    MOZ_ASSERT(any.isNull(),
               "use Val for non-nullptr ref types to get tracing");
    cell_.ref_ = any;
  }

  ValType type() const { return type_; }
  static constexpr size_t sizeofLargestValue() { return sizeof(cell_); }

  Cell& cell() { return cell_; }
  const Cell& cell() const { return cell_; }

  // Updates the type of the LitVal. Does not check that the type is valid for
  // the actual value, so make sure the type is definitely correct via
  // validation or something.
  void unsafeSetType(ValType type) { type_ = type; }

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
    MOZ_ASSERT(type_.isRefRepr());
    return cell_.ref_;
  }
  const V128& v128() const {
    MOZ_ASSERT(type_ == ValType::V128);
    return cell_.v128_;
  }

  WASM_DECLARE_FRIEND_SERIALIZE(LitVal);
};

WASM_DECLARE_CACHEABLE_POD(LitVal::Cell);

// A Val is a LitVal that can contain (non-null) pointers to GC things. All Vals
// must be used with the rooting APIs as they may contain JS objects.

class MOZ_NON_PARAM Val : public LitVal {
 public:
  Val() = default;
  explicit Val(ValType type) : LitVal(type) {}
  explicit Val(const LitVal& val);
  explicit Val(uint32_t i32) : LitVal(i32) {}
  explicit Val(uint64_t i64) : LitVal(i64) {}
  explicit Val(float f32) : LitVal(f32) {}
  explicit Val(double f64) : LitVal(f64) {}
  explicit Val(V128 v128) : LitVal(v128) {}
  explicit Val(ValType type, AnyRef val) : LitVal(type, AnyRef::null()) {
    MOZ_ASSERT(type.isRefRepr());
    cell_.ref_ = val;
  }
  explicit Val(ValType type, FuncRef val) : LitVal(type, AnyRef::null()) {
    MOZ_ASSERT(type.refType().isFuncHierarchy());
    cell_.ref_ = val.toAnyRef();
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
      case ValType::Ref:
        return cell_.ref_ == rhs.cell_.ref_;
    }
    MOZ_ASSERT_UNREACHABLE();
    return false;
  }
  bool operator!=(const Val& rhs) const { return !(*this == rhs); }

  bool isInvalid() const { return !type_.isValid(); }
  bool isAnyRef() const { return type_.isValid() && type_.isRefRepr(); }
  AnyRef& toAnyRef() const {
    MOZ_ASSERT(isAnyRef());
    return cell_.ref_;
  }

  // Initialize from `loc` which is a rooted location and needs no barriers.
  void initFromRootedLocation(ValType type, const void* loc);
  void initFromHeapLocation(ValType type, const void* loc);

  // Write to `loc` which is a rooted location and needs no barriers.
  void writeToRootedLocation(void* loc, bool mustWrite64) const;

  // Read from `loc` which is in the heap.
  void readFromHeapLocation(const void* loc);
  // Write to `loc` which is in the heap and must be barriered.
  void writeToHeapLocation(void* loc) const;

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

template <int N>
using ValVectorN = GCVector<Val, N, SystemAllocPolicy>;
template <int N>
using RootedValVectorN = Rooted<ValVectorN<N>>;

// Check if a JS value matches against a given reference type.
// Returns true and gives the corresponding wasm::AnyRef value for the JS value
// if the type check succeeds. Returns false and sets an error if the type
// check fails, or boxing the wasm::AnyRef failed due to an OOM.
[[nodiscard]] extern bool CheckRefType(JSContext* cx, RefType targetType,
                                       HandleValue v, MutableHandleAnyRef vp);
// The same as above, but discards the resulting wasm::AnyRef. This may still
// fail due to an OOM.
[[nodiscard]] extern bool CheckRefType(JSContext* cx, RefType targetType,
                                       HandleValue v);

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
extern bool ToJSValue(JSContext* cx, const void* src, StorageType type,
                      MutableHandleValue dst,
                      CoercionLevel level = CoercionLevel::Spec);
template <typename Debug = NoDebug>
extern bool ToJSValueMayGC(StorageType type);
template <typename Debug = NoDebug>
extern bool ToJSValue(JSContext* cx, const void* src, ValType type,
                      MutableHandleValue dst,
                      CoercionLevel level = CoercionLevel::Spec);
template <typename Debug = NoDebug>
extern bool ToJSValueMayGC(ValType type);
}  // namespace wasm

template <>
struct InternalBarrierMethods<wasm::AnyRef> {
  static bool isMarkable(const wasm::AnyRef v) { return v.isGCThing(); }

  static void preBarrier(const wasm::AnyRef v) {
    if (v.isGCThing()) {
      gc::PreWriteBarrierImpl(v.toGCThing());
    }
  }

  static MOZ_ALWAYS_INLINE void postBarrier(wasm::AnyRef* vp,
                                            const wasm::AnyRef prev,
                                            const wasm::AnyRef next) {
    // If the target needs an entry, add it.
    gc::StoreBuffer* sb;
    if (next.isGCThing() && (sb = next.toGCThing()->storeBuffer())) {
      // If we know that the prev has already inserted an entry, we can
      // skip doing the lookup to add the new entry. Note that we cannot
      // safely assert the presence of the entry because it may have been
      // added via a different store buffer.
      if (prev.isGCThing() && prev.toGCThing()->storeBuffer()) {
        return;
      }
      sb->putWasmAnyRef(vp);
      return;
    }
    // Remove the prev entry if the new value does not need it.
    if (prev.isGCThing() && (sb = prev.toGCThing()->storeBuffer())) {
      sb->unputWasmAnyRef(vp);
    }
  }

  static void readBarrier(const wasm::AnyRef v) {
    if (v.isGCThing()) {
      gc::ReadBarrierImpl(v.toGCThing());
    }
  }

#ifdef DEBUG
  static void assertThingIsNotGray(const wasm::AnyRef v) {
    if (v.isGCThing()) {
      JS::AssertCellIsNotGray(v.toGCThing());
    }
  }
#endif
};

template <>
struct InternalBarrierMethods<wasm::Val> {
  static bool isMarkable(const wasm::Val& v) { return v.isAnyRef(); }

  static void preBarrier(const wasm::Val& v) {
    if (v.isAnyRef()) {
      InternalBarrierMethods<wasm::AnyRef>::preBarrier(v.toAnyRef());
    }
  }

  static MOZ_ALWAYS_INLINE void postBarrier(wasm::Val* vp,
                                            const wasm::Val& prev,
                                            const wasm::Val& next) {
    // A wasm::Val can transition from being uninitialized to holding an anyref
    // but cannot change kind after that.
    MOZ_ASSERT_IF(next.isAnyRef(), prev.isAnyRef() || prev.isInvalid());
    MOZ_ASSERT_IF(prev.isAnyRef(), next.isAnyRef());

    if (next.isAnyRef()) {
      InternalBarrierMethods<wasm::AnyRef>::postBarrier(
          &vp->toAnyRef(),
          prev.isAnyRef() ? prev.toAnyRef() : wasm::AnyRef::null(),
          next.toAnyRef());
      return;
    }
  }

  static void readBarrier(const wasm::Val& v) {
    if (v.isAnyRef()) {
      InternalBarrierMethods<wasm::AnyRef>::readBarrier(v.toAnyRef());
    }
  }

#ifdef DEBUG
  static void assertThingIsNotGray(const wasm::Val& v) {
    if (v.isAnyRef()) {
      InternalBarrierMethods<wasm::AnyRef>::assertThingIsNotGray(v.toAnyRef());
    }
  }
#endif
};

}  // namespace js

template <>
struct JS::SafelyInitialized<js::wasm::AnyRef> {
  static js::wasm::AnyRef create() { return js::wasm::AnyRef::null(); }
};

#endif  // wasm_val_h
