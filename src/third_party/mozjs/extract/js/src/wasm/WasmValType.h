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

#ifndef wasm_valtype_h
#define wasm_valtype_h

#include "mozilla/Maybe.h"

#include <type_traits>

#include "jit/IonTypes.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace wasm {

using mozilla::Maybe;

// A PackedTypeCode represents any value type in an compact POD format.
union PackedTypeCode {
 public:
  using PackedRepr = uintptr_t;

 private:
#ifdef JS_64BIT
  static constexpr size_t PointerTagBits = 2;
  static constexpr size_t TypeCodeBits = 8;
  static constexpr size_t TypeIndexBits = 21;
  static constexpr size_t NullableBits = 1;
  static constexpr size_t RttDepthBits = 10;
#else
  static constexpr size_t PointerTagBits = 2;
  static constexpr size_t TypeCodeBits = 8;
  static constexpr size_t TypeIndexBits = 14;
  static constexpr size_t NullableBits = 1;
  static constexpr size_t RttDepthBits = 7;
#endif

  static_assert(PointerTagBits + TypeCodeBits + TypeIndexBits + NullableBits +
                        RttDepthBits <=
                    (sizeof(PackedRepr) * 8),
                "enough bits");
  static_assert(MaxTypeIndex < (1 << TypeIndexBits), "enough bits");
  static_assert(MaxRttDepth < (1 << RttDepthBits), "enough bits");

  PackedRepr bits_;
  struct {
    PackedRepr pointerTag_ : PointerTagBits;
    PackedRepr typeCode_ : TypeCodeBits;
    PackedRepr typeIndex_ : TypeIndexBits;
    PackedRepr nullable_ : NullableBits;
    PackedRepr rttDepth_ : RttDepthBits;
  };

 public:
  static constexpr uint32_t NoTypeCode = (1 << TypeCodeBits) - 1;
  static constexpr uint32_t NoTypeIndex = (1 << TypeIndexBits) - 1;

  static PackedTypeCode invalid() {
    PackedTypeCode ptc = {};
    ptc.typeCode_ = NoTypeCode;
    return ptc;
  }

  static constexpr PackedTypeCode fromBits(PackedRepr bits) {
    PackedTypeCode ptc = {};
    ptc.bits_ = bits;
    return ptc;
  }

  static constexpr PackedTypeCode pack(TypeCode tc, uint32_t refTypeIndex,
                                       bool isNullable, uint32_t rttDepth) {
    MOZ_ASSERT(uint32_t(tc) <= ((1 << TypeCodeBits) - 1));
    MOZ_ASSERT_IF(tc != AbstractReferenceTypeIndexCode && tc != TypeCode::Rtt,
                  refTypeIndex == NoTypeIndex);
    MOZ_ASSERT_IF(tc == AbstractReferenceTypeIndexCode || tc == TypeCode::Rtt,
                  refTypeIndex <= MaxTypeIndex);
    MOZ_ASSERT_IF(tc != TypeCode::Rtt, rttDepth == 0);
    MOZ_ASSERT_IF(tc == TypeCode::Rtt, rttDepth <= MaxRttDepth);
    PackedTypeCode ptc = {};
    ptc.typeCode_ = PackedRepr(tc);
    ptc.typeIndex_ = refTypeIndex;
    ptc.nullable_ = isNullable;
    ptc.rttDepth_ = rttDepth;
    return ptc;
  }

  static constexpr PackedTypeCode pack(TypeCode tc, bool nullable) {
    return pack(tc, PackedTypeCode::NoTypeIndex, nullable, 0);
  }

  static constexpr PackedTypeCode pack(TypeCode tc) {
    return pack(tc, PackedTypeCode::NoTypeIndex, false, 0);
  }

  bool isValid() const { return typeCode_ != NoTypeCode; }

  bool isReference() const {
    return typeCodeAbstracted() == AbstractReferenceTypeCode;
  }

  PackedRepr bits() const { return bits_; }

  TypeCode typeCode() const {
    MOZ_ASSERT(isValid());
    return TypeCode(typeCode_);
  }

  // Return the TypeCode, but return AbstractReferenceTypeCode for any reference
  // type.
  //
  // This function is very, very hot, hence what would normally be a switch on
  // the value `c` to map the reference types to AbstractReferenceTypeCode has
  // been distilled into a simple comparison; this is fastest.  Should type
  // codes become too complicated for this to work then a lookup table also has
  // better performance than a switch.
  //
  // An alternative is for the PackedTypeCode to represent something closer to
  // what ValType needs, so that this decoding step is not necessary, but that
  // moves complexity elsewhere, and the perf gain here would be only about 1%
  // for baseline compilation throughput.
  //
  // TODO: with rtt types this is no longer a simple comparison, we should
  // re-evaluate the performance of this function.
  TypeCode typeCodeAbstracted() const {
    MOZ_ASSERT(isValid());
    TypeCode tc = TypeCode(typeCode_);
    return (tc < LowestPrimitiveTypeCode && tc != TypeCode::Rtt)
               ? AbstractReferenceTypeCode
               : tc;
  }

  uint32_t typeIndex() const {
    MOZ_ASSERT(isValid());
    return uint32_t(typeIndex_);
  }

  uint32_t typeIndexUnchecked() const {
    MOZ_ASSERT(isValid());
    return uint32_t(typeIndex_);
  }

  bool isNullable() const {
    MOZ_ASSERT(isValid());
    return bool(nullable_);
  }

  uint32_t rttDepth() const {
    MOZ_ASSERT(isValid());
    return uint32_t(rttDepth_);
  }

  PackedTypeCode asNonNullable() const {
    MOZ_ASSERT(isReference());
    PackedTypeCode mutated = *this;
    mutated.nullable_ = 0;
    return mutated;
  }

  bool operator==(const PackedTypeCode& rhs) const {
    return bits_ == rhs.bits_;
  }
  bool operator!=(const PackedTypeCode& rhs) const {
    return bits_ != rhs.bits_;
  }
};

static_assert(sizeof(PackedTypeCode) == sizeof(uintptr_t), "packed");
static_assert(std::is_pod_v<PackedTypeCode>,
              "must be POD to be simply serialized/deserialized");

// An enum that describes the representation classes for tables; The table
// element type is mapped into this by Table::repr().

enum class TableRepr { Ref, Func };

// The RefType carries more information about types t for which t.isReference()
// is true.

class RefType {
 public:
  enum Kind {
    Func = uint8_t(TypeCode::FuncRef),
    Extern = uint8_t(TypeCode::ExternRef),
    Eq = uint8_t(TypeCode::EqRef),
    TypeIndex = uint8_t(AbstractReferenceTypeIndexCode)
  };

 private:
  PackedTypeCode ptc_;

#ifdef DEBUG
  bool isValid() const {
    switch (ptc_.typeCode()) {
      case TypeCode::FuncRef:
      case TypeCode::ExternRef:
      case TypeCode::EqRef:
        MOZ_ASSERT(ptc_.typeIndex() == PackedTypeCode::NoTypeIndex);
        return true;
      case AbstractReferenceTypeIndexCode:
        MOZ_ASSERT(ptc_.typeIndex() != PackedTypeCode::NoTypeIndex);
        return true;
      default:
        return false;
    }
  }
#endif
  RefType(Kind kind, bool nullable)
      : ptc_(PackedTypeCode::pack(TypeCode(kind), nullable)) {
    MOZ_ASSERT(isValid());
  }

  RefType(uint32_t refTypeIndex, bool nullable)
      : ptc_(PackedTypeCode::pack(AbstractReferenceTypeIndexCode, refTypeIndex,
                                  nullable, 0)) {
    MOZ_ASSERT(isValid());
  }

 public:
  RefType() : ptc_(PackedTypeCode::invalid()) {}
  explicit RefType(PackedTypeCode ptc) : ptc_(ptc) { MOZ_ASSERT(isValid()); }

  static RefType fromTypeCode(TypeCode tc, bool nullable) {
    MOZ_ASSERT(tc != AbstractReferenceTypeIndexCode);
    return RefType(Kind(tc), nullable);
  }

  static RefType fromTypeIndex(uint32_t refTypeIndex, bool nullable) {
    return RefType(refTypeIndex, nullable);
  }

  Kind kind() const { return Kind(ptc_.typeCode()); }

  uint32_t typeIndex() const { return ptc_.typeIndex(); }

  PackedTypeCode packed() const { return ptc_; }

  static RefType func() { return RefType(Func, true); }
  static RefType extern_() { return RefType(Extern, true); }
  static RefType eq() { return RefType(Eq, true); }

  bool isFunc() const { return kind() == RefType::Func; }
  bool isExtern() const { return kind() == RefType::Extern; }
  bool isEq() const { return kind() == RefType::Eq; }
  bool isTypeIndex() const { return kind() == RefType::TypeIndex; }

  bool isNullable() const { return bool(ptc_.isNullable()); }
  RefType asNonNullable() const { return RefType(ptc_.asNonNullable()); }

  TableRepr tableRepr() const {
    switch (kind()) {
      case RefType::Func:
        return TableRepr::Func;
      case RefType::Extern:
      case RefType::Eq:
        return TableRepr::Ref;
      case RefType::TypeIndex:
        MOZ_CRASH("NYI");
    }
    MOZ_CRASH("switch is exhaustive");
  }

  bool operator==(const RefType& that) const { return ptc_ == that.ptc_; }
  bool operator!=(const RefType& that) const { return ptc_ != that.ptc_; }
};

class FieldTypeTraits {
 public:
  enum Kind {
    I8 = uint8_t(TypeCode::I8),
    I16 = uint8_t(TypeCode::I16),
    I32 = uint8_t(TypeCode::I32),
    I64 = uint8_t(TypeCode::I64),
    F32 = uint8_t(TypeCode::F32),
    F64 = uint8_t(TypeCode::F64),
    V128 = uint8_t(TypeCode::V128),
    Rtt = uint8_t(TypeCode::Rtt),
    Ref = uint8_t(AbstractReferenceTypeCode),
  };

  static bool isValidTypeCode(TypeCode tc) {
    switch (tc) {
#ifdef ENABLE_WASM_GC
      case TypeCode::I8:
      case TypeCode::I16:
#endif
      case TypeCode::I32:
      case TypeCode::I64:
      case TypeCode::F32:
      case TypeCode::F64:
#ifdef ENABLE_WASM_SIMD
      case TypeCode::V128:
#endif
      case TypeCode::FuncRef:
      case TypeCode::ExternRef:
#ifdef ENABLE_WASM_GC
      case TypeCode::EqRef:
      case TypeCode::Rtt:
#endif
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
      case AbstractReferenceTypeIndexCode:
#endif
        return true;
      default:
        return false;
    }
  }
};

class ValTypeTraits {
 public:
  enum Kind {
    I32 = uint8_t(TypeCode::I32),
    I64 = uint8_t(TypeCode::I64),
    F32 = uint8_t(TypeCode::F32),
    F64 = uint8_t(TypeCode::F64),
    V128 = uint8_t(TypeCode::V128),
    Rtt = uint8_t(TypeCode::Rtt),
    Ref = uint8_t(AbstractReferenceTypeCode),
  };

  static bool isValidTypeCode(TypeCode tc) {
    switch (tc) {
      case TypeCode::I32:
      case TypeCode::I64:
      case TypeCode::F32:
      case TypeCode::F64:
#ifdef ENABLE_WASM_SIMD
      case TypeCode::V128:
#endif
      case TypeCode::FuncRef:
      case TypeCode::ExternRef:
#ifdef ENABLE_WASM_GC
      case TypeCode::EqRef:
      case TypeCode::Rtt:
#endif
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
      case AbstractReferenceTypeIndexCode:
#endif
        return true;
      default:
        return false;
    }
  }
};

// The PackedType represents the storage type of a WebAssembly location, whether
// parameter, local, field, or global. See specializations below for ValType and
// FieldType.

template <class T>
class PackedType : public T {
 public:
  using Kind = typename T::Kind;

 protected:
  PackedTypeCode tc_;

  explicit PackedType(TypeCode c) : tc_(PackedTypeCode::pack(c)) {
    MOZ_ASSERT(c != AbstractReferenceTypeIndexCode);
    MOZ_ASSERT(isValid());
  }

  TypeCode typeCode() const {
    MOZ_ASSERT(isValid());
    return tc_.typeCode();
  }

 public:
  PackedType() : tc_(PackedTypeCode::invalid()) {}

  MOZ_IMPLICIT PackedType(Kind c) : tc_(PackedTypeCode::pack(TypeCode(c))) {
    MOZ_ASSERT(c != Kind::Ref);
    MOZ_ASSERT(isValid());
  }

  MOZ_IMPLICIT PackedType(RefType rt) : tc_(rt.packed()) {
    MOZ_ASSERT(isValid());
  }

  explicit PackedType(PackedTypeCode ptc) : tc_(ptc) { MOZ_ASSERT(isValid()); }

  explicit PackedType(jit::MIRType mty) {
    switch (mty) {
      case jit::MIRType::Int32:
        tc_ = PackedTypeCode::pack(TypeCode::I32);
        break;
      case jit::MIRType::Int64:
        tc_ = PackedTypeCode::pack(TypeCode::I64);
        break;
      case jit::MIRType::Float32:
        tc_ = PackedTypeCode::pack(TypeCode::F32);
        break;
      case jit::MIRType::Double:
        tc_ = PackedTypeCode::pack(TypeCode::F64);
        break;
      case jit::MIRType::Simd128:
        tc_ = PackedTypeCode::pack(TypeCode::V128);
        break;
      default:
        MOZ_CRASH("PackedType(MIRType): unexpected type");
    }
  }

  static PackedType fromNonRefTypeCode(TypeCode tc) {
#ifdef DEBUG
    switch (tc) {
      case TypeCode::I8:
      case TypeCode::I16:
      case TypeCode::I32:
      case TypeCode::I64:
      case TypeCode::F32:
      case TypeCode::F64:
      case TypeCode::V128:
        break;
      default:
        MOZ_CRASH("Bad type code");
    }
#endif
    return PackedType(tc);
  }

  static PackedType fromRtt(uint32_t typeIndex, uint32_t rttDepth) {
    return PackedType(
        PackedTypeCode::pack(TypeCode::Rtt, typeIndex, false, rttDepth));
  }

  static PackedType fromBitsUnsafe(uint64_t bits) {
    return PackedType(PackedTypeCode::fromBits(bits));
  }

  static constexpr PackedType hostPtr() {
#ifdef JS_64BIT
    return PackedType::I64;
#else
    return PackedType::I32;
#endif
  }

  bool isValid() const {
    if (!tc_.isValid()) {
      return false;
    }
    return T::isValidTypeCode(tc_.typeCode());
  }

  PackedTypeCode packed() const {
    MOZ_ASSERT(isValid());
    return tc_;
  }

  uint64_t bitsUnsafe() const {
    MOZ_ASSERT(isValid());
    return tc_.bits();
  }

  bool isFuncRef() const { return tc_.typeCode() == TypeCode::FuncRef; }

  bool isExternRef() const { return tc_.typeCode() == TypeCode::ExternRef; }

  bool isEqRef() const { return tc_.typeCode() == TypeCode::EqRef; }

  bool isTypeIndex() const {
    MOZ_ASSERT(isValid());
    return tc_.typeCode() == AbstractReferenceTypeIndexCode;
  }

  bool isReference() const {
    MOZ_ASSERT(isValid());
    return tc_.isReference();
  }

  bool isRtt() const { return tc_.typeCode() == TypeCode::Rtt; }

  // Returns whether the type has a default value.
  bool isDefaultable() const {
    MOZ_ASSERT(isValid());
    return !(isRtt() || (isReference() && !isNullable()));
  }

  // Returns whether the type has a representation in JS.
  bool isExposable() const {
    MOZ_ASSERT(isValid());
#if defined(ENABLE_WASM_SIMD) || defined(ENABLE_WASM_GC)
    return !(kind() == Kind::V128 || isRtt() || isTypeIndex());
#else
    return true;
#endif
  }

  bool isNullable() const {
    MOZ_ASSERT(isValid());
    return tc_.isNullable();
  }

  uint32_t typeIndex() const {
    MOZ_ASSERT(isValid());
    return tc_.typeIndex();
  }

  uint32_t rttDepth() const {
    MOZ_ASSERT(isValid());
    return tc_.rttDepth();
  }

  Kind kind() const {
    MOZ_ASSERT(isValid());
    return Kind(tc_.typeCodeAbstracted());
  }

  RefType refType() const {
    MOZ_ASSERT(isReference());
    return RefType(tc_);
  }

  RefType::Kind refTypeKind() const {
    MOZ_ASSERT(isReference());
    return RefType(tc_).kind();
  }

  void renumber(const RenumberMap& map) {
    if (!isTypeIndex()) {
      return;
    }

    if (RenumberMap::Ptr p = map.lookup(refType().typeIndex())) {
      *this = RefType::fromTypeIndex(p->value(), isNullable());
    }
  }

  void offsetTypeIndex(uint32_t offsetBy) {
    if (!isTypeIndex()) {
      return;
    }
    *this =
        RefType::fromTypeIndex(refType().typeIndex() + offsetBy, isNullable());
  }

  // Some types are encoded as JS::Value when they escape from Wasm (when passed
  // as parameters to imports or returned from exports).  For ExternRef the
  // Value encoding is pretty much a requirement.  For other types it's a choice
  // that may (temporarily) simplify some code.
  bool isEncodedAsJSValueOnEscape() const {
    switch (typeCode()) {
      case TypeCode::FuncRef:
      case TypeCode::ExternRef:
      case TypeCode::EqRef:
        return true;
      default:
        return false;
    }
  }

  uint32_t size() const {
    switch (tc_.typeCodeAbstracted()) {
      case TypeCode::I8:
        return 1;
      case TypeCode::I16:
        return 2;
      case TypeCode::I32:
        return 4;
      case TypeCode::I64:
        return 8;
      case TypeCode::F32:
        return 4;
      case TypeCode::F64:
        return 8;
      case TypeCode::V128:
        return 16;
      case TypeCode::Rtt:
      case AbstractReferenceTypeCode:
        return sizeof(void*);
      default:
        MOZ_ASSERT_UNREACHABLE();
        return 0;
    }
  }
  uint32_t alignmentInStruct() const { return size(); }
  uint32_t indexingShift() const {
    switch (size()) {
      case 1:
        return 0;
      case 2:
        return 1;
      case 4:
        return 2;
      case 8:
        return 3;
      case 16:
        return 4;
      default:
        MOZ_ASSERT_UNREACHABLE();
        return 0;
    }
  }

  PackedType<ValTypeTraits> widenToValType() const {
    switch (tc_.typeCodeAbstracted()) {
      case TypeCode::I8:
      case TypeCode::I16:
        return PackedType<ValTypeTraits>::I32;
      default:
        return PackedType<ValTypeTraits>(tc_);
    }
  }

  PackedType<ValTypeTraits> valType() const {
    MOZ_ASSERT(isValType());
    return PackedType<ValTypeTraits>(tc_);
  }

  bool isValType() const {
    switch (tc_.typeCode()) {
      case TypeCode::I8:
      case TypeCode::I16:
        return false;
      default:
        return true;
    }
  }

  bool operator==(const PackedType& that) const {
    MOZ_ASSERT(isValid() && that.isValid());
    return tc_ == that.tc_;
  }

  bool operator!=(const PackedType& that) const {
    MOZ_ASSERT(isValid() && that.isValid());
    return tc_ != that.tc_;
  }

  bool operator==(Kind that) const {
    MOZ_ASSERT(isValid());
    MOZ_ASSERT(that != Kind::Ref);
    return Kind(typeCode()) == that;
  }

  bool operator!=(Kind that) const { return !(*this == that); }
};

using ValType = PackedType<ValTypeTraits>;
using FieldType = PackedType<FieldTypeTraits>;

// The dominant use of this data type is for locals and args, and profiling
// with ZenGarden and Tanks suggests an initial size of 16 minimises heap
// allocation, both in terms of blocks and bytes.
using ValTypeVector = Vector<ValType, 16, SystemAllocPolicy>;

// ValType utilities

static inline unsigned SizeOf(ValType vt) {
  switch (vt.kind()) {
    case ValType::I32:
    case ValType::F32:
      return 4;
    case ValType::I64:
    case ValType::F64:
      return 8;
    case ValType::V128:
      return 16;
    case ValType::Rtt:
    case ValType::Ref:
      return sizeof(intptr_t);
  }
  MOZ_CRASH("Invalid ValType");
}

// Note, ToMIRType is only correct within Wasm, where an AnyRef is represented
// as a pointer.  At the JS/wasm boundary, an AnyRef can be represented as a
// JS::Value, and the type translation may have to be handled specially and on a
// case-by-case basis.

static inline jit::MIRType ToMIRType(ValType vt) {
  switch (vt.kind()) {
    case ValType::I32:
      return jit::MIRType::Int32;
    case ValType::I64:
      return jit::MIRType::Int64;
    case ValType::F32:
      return jit::MIRType::Float32;
    case ValType::F64:
      return jit::MIRType::Double;
    case ValType::V128:
      return jit::MIRType::Simd128;
    case ValType::Rtt:
    case ValType::Ref:
      return jit::MIRType::RefOrNull;
  }
  MOZ_CRASH("bad type");
}

static inline bool IsNumberType(ValType vt) { return !vt.isReference(); }

static inline jit::MIRType ToMIRType(const Maybe<ValType>& t) {
  return t ? ToMIRType(ValType(t.ref())) : jit::MIRType::None;
}

extern bool ToValType(JSContext* cx, HandleValue v, ValType* out);

extern UniqueChars ToString(ValType type);

extern UniqueChars ToString(const Maybe<ValType>& type);

}  // namespace wasm
}  // namespace js

#endif  // wasm_valtype_h
