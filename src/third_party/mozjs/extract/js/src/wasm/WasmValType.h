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

#include "mozilla/HashTable.h"
#include "mozilla/Maybe.h"

#include <type_traits>

#include "jit/IonTypes.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace wasm {

// A PackedTypeCode represents any value type.
union PackedTypeCode {
 public:
  using PackedRepr = uint64_t;

 private:
  static constexpr size_t NullableBits = 1;
  static constexpr size_t TypeCodeBits = 8;
  static constexpr size_t TypeDefBits = 48;
  static constexpr size_t PointerTagBits = 2;
  static constexpr size_t UnusedBits = 5;

  static_assert(NullableBits + TypeCodeBits + TypeDefBits + PointerTagBits +
                        UnusedBits ==
                    (sizeof(PackedRepr) * 8),
                "enough bits");

  PackedRepr bits_;
  struct {
    PackedRepr nullable_ : NullableBits;
    PackedRepr typeCode_ : TypeCodeBits;
    // A pointer to the TypeDef this type references. We use 48-bits for this,
    // and rely on system memory allocators not allocating outside of this
    // range. This is also assumed by JS::Value, and so should be safe here.
    PackedRepr typeDef_ : TypeDefBits;
    // Reserve the bottom two bits for use as a tagging scheme for BlockType
    // and ResultType, which can encode a ValType inside themselves in special
    // cases.
    PackedRepr pointerTag_ : PointerTagBits;
    // The remaining bits are unused, but still need to be explicitly
    // initialized to zero.
    PackedRepr unused_ : UnusedBits;
  };

  explicit constexpr PackedTypeCode(PackedRepr bits) : bits_(bits) {}

  constexpr PackedTypeCode(PackedRepr nullable, PackedRepr typeCode,
                           PackedRepr typeDef)
      : nullable_(nullable),
        typeCode_(typeCode),
        typeDef_(typeDef),
        pointerTag_(0),
        unused_(0) {}

 public:
  PackedTypeCode() = default;

  static constexpr PackedRepr NoTypeCode = ((uint64_t)1 << TypeCodeBits) - 1;

  static constexpr PackedTypeCode invalid() {
    return PackedTypeCode{false, NoTypeCode, 0};
  }

  static constexpr PackedTypeCode fromBits(PackedRepr bits) {
    return PackedTypeCode{bits};
  }

  static PackedTypeCode pack(TypeCode tc, const TypeDef* typeDef,
                             bool isNullable) {
    MOZ_ASSERT(uint32_t(tc) <= ((1 << TypeCodeBits) - 1));
    MOZ_ASSERT_IF(tc != AbstractTypeRefCode, typeDef == nullptr);
    MOZ_ASSERT_IF(tc == AbstractTypeRefCode, typeDef != nullptr);
    auto tydef = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(typeDef));
#if defined(JS_64BIT) && defined(DEBUG)
    // Double check that `typeDef` only has 48 significant bits, with the top
    // 16 being zero.  This is necessary since we will only store the lowest
    // 48 bits of it, as noted above.  There's no equivalent check on 32 bit
    // targets since we can store the whole pointer.
    static_assert(sizeof(int64_t) == sizeof(uintptr_t));
    MOZ_ASSERT((tydef >> TypeDefBits) == 0);
#endif
    return PackedTypeCode{isNullable, PackedRepr(tc), tydef};
  }

  static constexpr PackedTypeCode pack(TypeCode tc, bool nullable) {
    MOZ_ASSERT(uint32_t(tc) <= ((1 << TypeCodeBits) - 1));
    MOZ_ASSERT(tc != AbstractTypeRefCode);
    return PackedTypeCode{nullable, PackedRepr(tc), 0};
  }

  static constexpr PackedTypeCode pack(TypeCode tc) { return pack(tc, false); }

  constexpr bool isValid() const { return typeCode_ != NoTypeCode; }

  PackedRepr bits() const { return bits_; }

  constexpr TypeCode typeCode() const {
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
  constexpr TypeCode typeCodeAbstracted() const {
    TypeCode tc = typeCode();
    return tc < LowestPrimitiveTypeCode ? AbstractReferenceTypeCode : tc;
  }

  // Return whether this type is a reference type.
  bool isRefType() const {
    return typeCodeAbstracted() == AbstractReferenceTypeCode;
  }

  // Return whether this type is represented by a reference at runtime.
  bool isRefRepr() const { return typeCode() < LowestPrimitiveTypeCode; }

  const TypeDef* typeDef() const {
    MOZ_ASSERT(isValid());
    // On a 64-bit target, this reconstitutes the pointer by zero-extending
    // the lowest TypeDefBits bits of `typeDef_`.  On a 32-bit target, the
    // pointer is stored exactly in the lowest 32 bits of `typeDef_`.
    return (const TypeDef*)(uintptr_t)typeDef_;
  }

  bool isNullable() const {
    MOZ_ASSERT(isValid());
    return bool(nullable_);
  }

  PackedTypeCode withIsNullable(bool nullable) const {
    MOZ_ASSERT(isRefType());
    PackedTypeCode mutated = *this;
    mutated.nullable_ = (PackedRepr)nullable;
    return mutated;
  }

  bool operator==(const PackedTypeCode& rhs) const {
    return bits_ == rhs.bits_;
  }
  bool operator!=(const PackedTypeCode& rhs) const {
    return bits_ != rhs.bits_;
  }
};

static_assert(sizeof(PackedTypeCode) == sizeof(uint64_t), "packed");

// A SerializableTypeCode represents any value type in a form that can be
// serialized and deserialized.
union SerializableTypeCode {
  using PackedRepr = uintptr_t;

  static constexpr size_t NullableBits = 1;
  static constexpr size_t TypeCodeBits = 8;
  static constexpr size_t TypeIndexBits = 20;

  PackedRepr bits;
  struct {
    PackedRepr nullable : NullableBits;
    PackedRepr typeCode : TypeCodeBits;
    PackedRepr typeIndex : TypeIndexBits;
  };

  WASM_CHECK_CACHEABLE_POD(bits);

  static constexpr PackedRepr NoTypeIndex = (1 << TypeIndexBits) - 1;

  static_assert(NullableBits + TypeCodeBits + TypeIndexBits <=
                    (sizeof(PackedRepr) * 8),
                "enough bits");
  static_assert(NoTypeIndex < (1 << TypeIndexBits), "enough bits");
  static_assert(MaxTypes < NoTypeIndex, "enough bits");

  // Defined in WasmSerialize.cpp
  static inline SerializableTypeCode serialize(PackedTypeCode ptc,
                                               const TypeContext& types);
  inline PackedTypeCode deserialize(const TypeContext& types);
};

WASM_DECLARE_CACHEABLE_POD(SerializableTypeCode);
static_assert(sizeof(SerializableTypeCode) == sizeof(uintptr_t), "packed");

// [SMDOC] Comparing type definitions
//
// WebAssembly type equality is structural, and we implement canonicalization
// such that equality of pointers to type definitions means that the type
// definitions are structurally equal.
//
// 'IsoEquals' is the algorithm used to determine if two types are equal while
// canonicalizing types.
//
// A iso equals type code encodes a type code for use in equality and hashing
// matching. It normalizes type references that are local to a recursion group
// so that they can be bitwise compared to type references from other recursion
// groups.
//
// This is useful for the following example:
//   (rec (func $a))
//   (rec
//     (func $b)
//     (struct
//       (field (ref $a)))
//       (field (ref $b)))
//   )
//   (rec
//     (func $c)
//     (struct
//       (field (ref $a)))
//       (field (ref $c)))
//   )
//
// The last two recursion groups are identical and should canonicalize to the
// same instance. However, they will be initially represented as two separate
// recursion group instances each with an array type instance with element
// types that point to the function type instance before them. A bitwise
// comparison of the element type pointers would fail.
//
// To solve this, we use `IsoEqualsTypeCode` to convert the example to:
//   (rec (func $a))
//   (rec
//     (func $b)
//     (struct
//       (field (ref nonlocal $a)))
//       (field (ref local 0)))
//   )
//   (rec
//     (func $c)
//     (struct
//       (field (ref nonlocal $a)))
//       (field (ref local 0)))
//   )
//
// Now, comparing the element types will see that these are local type
// references of the same kinds. `IsoEqualsTypeCode` performs the same mechanism
// as `tie` in the MVP presentation of type equality [1].
//
// [1]
// https://github.com/WebAssembly/gc/blob/main/proposals/gc/MVP.md#equivalence
union IsoEqualsTypeCode {
  using PackedRepr = uint64_t;

  static constexpr size_t NullableBits = 1;
  static constexpr size_t TypeCodeBits = 8;
  static constexpr size_t TypeRefBits = 48;

  PackedRepr bits;
  struct {
    PackedRepr nullable : NullableBits;
    PackedRepr typeCode : TypeCodeBits;
    PackedRepr typeRef : TypeRefBits;
  };

  WASM_CHECK_CACHEABLE_POD(bits);

  static_assert(NullableBits + TypeCodeBits + TypeRefBits <=
                    (sizeof(PackedRepr) * 8),
                "enough bits");

  // Defined in WasmTypeDef.h to avoid a cycle while allowing inlining
  static inline IsoEqualsTypeCode forIsoEquals(PackedTypeCode ptc,
                                               const RecGroup* recGroup);

  bool operator==(IsoEqualsTypeCode other) const { return bits == other.bits; }
  bool operator!=(IsoEqualsTypeCode other) const { return bits != other.bits; }
  HashNumber hash() const { return HashNumber(bits); }
};

// An enum that describes the representation classes for tables; The table
// element type is mapped into this by Table::repr().

enum class TableRepr { Ref, Func };

// An enum that describes the different type hierarchies.

enum class RefTypeHierarchy { Func, Extern, Exn, Any };

// The RefType carries more information about types t for which t.isRefType()
// is true.

class RefType {
 public:
  enum Kind {
    Func = uint8_t(TypeCode::FuncRef),
    Extern = uint8_t(TypeCode::ExternRef),
    Exn = uint8_t(TypeCode::ExnRef),
    Any = uint8_t(TypeCode::AnyRef),
    NoFunc = uint8_t(TypeCode::NullFuncRef),
    NoExtern = uint8_t(TypeCode::NullExternRef),
    NoExn = uint8_t(TypeCode::NullExnRef),
    None = uint8_t(TypeCode::NullAnyRef),
    Eq = uint8_t(TypeCode::EqRef),
    I31 = uint8_t(TypeCode::I31Ref),
    Struct = uint8_t(TypeCode::StructRef),
    Array = uint8_t(TypeCode::ArrayRef),
    TypeRef = uint8_t(AbstractTypeRefCode)
  };

 private:
  PackedTypeCode ptc_;

  RefType(Kind kind, bool nullable)
      : ptc_(PackedTypeCode::pack(TypeCode(kind), nullable)) {
    MOZ_ASSERT(isValid());
  }

  RefType(const TypeDef* typeDef, bool nullable)
      : ptc_(PackedTypeCode::pack(AbstractTypeRefCode, typeDef, nullable)) {
    MOZ_ASSERT(isValid());
  }

 public:
  RefType() : ptc_(PackedTypeCode::invalid()) {}
  explicit RefType(PackedTypeCode ptc) : ptc_(ptc) { MOZ_ASSERT(isValid()); }

  static RefType fromKind(Kind kind, bool nullable) {
    MOZ_ASSERT(kind != TypeRef);
    return RefType(kind, nullable);
  }

  static RefType fromTypeCode(TypeCode tc, bool nullable) {
    MOZ_ASSERT(tc != AbstractTypeRefCode);
    return RefType(Kind(tc), nullable);
  }

  static RefType fromTypeDef(const TypeDef* typeDef, bool nullable) {
    return RefType(typeDef, nullable);
  }

  Kind kind() const { return Kind(ptc_.typeCode()); }

  const TypeDef* typeDef() const { return ptc_.typeDef(); }

  PackedTypeCode packed() const { return ptc_; }
  PackedTypeCode* addressOfPacked() { return &ptc_; }
  const PackedTypeCode* addressOfPacked() const { return &ptc_; }

  // Further restricts PackedTypeCode::isValid() to only those TypeCodes which
  // represent a wasm reference type.
  bool isValid() const {
    if (!ptc_.isValid()) {
      return false;
    }

    MOZ_ASSERT((ptc_.typeCode() == AbstractTypeRefCode) ==
               (ptc_.typeDef() != nullptr));
    switch (ptc_.typeCode()) {
      case TypeCode::FuncRef:
      case TypeCode::ExternRef:
      case TypeCode::ExnRef:
      case TypeCode::AnyRef:
      case TypeCode::EqRef:
      case TypeCode::I31Ref:
      case TypeCode::StructRef:
      case TypeCode::ArrayRef:
      case TypeCode::NullFuncRef:
      case TypeCode::NullExternRef:
      case TypeCode::NullExnRef:
      case TypeCode::NullAnyRef:
      case AbstractTypeRefCode:
        return true;
      default:
        return false;
    }
  }

  static RefType func() { return RefType(Func, true); }
  static RefType extern_() { return RefType(Extern, true); }
  static RefType exn() { return RefType(Exn, true); }
  static RefType any() { return RefType(Any, true); }
  static RefType nofunc() { return RefType(NoFunc, true); }
  static RefType noextern() { return RefType(NoExtern, true); }
  static RefType noexn() { return RefType(NoExn, true); }
  static RefType none() { return RefType(None, true); }
  static RefType eq() { return RefType(Eq, true); }
  static RefType i31() { return RefType(I31, true); }
  static RefType struct_() { return RefType(Struct, true); }
  static RefType array() { return RefType(Array, true); }

  bool isFunc() const { return kind() == RefType::Func; }
  bool isExtern() const { return kind() == RefType::Extern; }
  bool isAny() const { return kind() == RefType::Any; }
  bool isNoFunc() const { return kind() == RefType::NoFunc; }
  bool isNoExtern() const { return kind() == RefType::NoExtern; }
  bool isNoExn() const { return kind() == RefType::NoExn; }
  bool isNone() const { return kind() == RefType::None; }
  bool isEq() const { return kind() == RefType::Eq; }
  bool isI31() const { return kind() == RefType::I31; }
  bool isStruct() const { return kind() == RefType::Struct; }
  bool isArray() const { return kind() == RefType::Array; }
  bool isTypeRef() const { return kind() == RefType::TypeRef; }

  bool isNullable() const { return bool(ptc_.isNullable()); }
  RefType asNonNullable() const { return withIsNullable(false); }
  RefType withIsNullable(bool nullable) const {
    return RefType(ptc_.withIsNullable(nullable));
  }

  bool isRefBottom() const {
    return isNone() || isNoFunc() || isNoExtern() || isNoExn();
  }

  // These methods are defined in WasmTypeDef.h to avoid a cycle while allowing
  // inlining.
  inline RefTypeHierarchy hierarchy() const;
  inline TableRepr tableRepr() const;
  inline bool isFuncHierarchy() const;
  inline bool isExternHierarchy() const;
  inline bool isAnyHierarchy() const;
  inline bool isExnHierarchy() const;
  static bool isSubTypeOf(RefType subType, RefType superType);
  static bool castPossible(RefType sourceType, RefType destType);

  // Gets the top of the given type's hierarchy, e.g. Any for structs and
  // arrays, and Func for funcs.
  RefType topType() const;

  // Gets the bottom of the given type's hierarchy, e.g. None for structs and
  // arrays, and NoFunc for funcs.
  RefType bottomType() const;

  static RefType leastUpperBound(RefType a, RefType b);

  // Gets the TypeDefKind associated with this RefType, e.g. TypeDefKind::Struct
  // for RefType::Struct.
  TypeDefKind typeDefKind() const;

  inline void AddRef() const;
  inline void Release() const;

  bool operator==(const RefType& that) const { return ptc_ == that.ptc_; }
  bool operator!=(const RefType& that) const { return ptc_ != that.ptc_; }
};

using RefTypeVector = Vector<RefType, 0, SystemAllocPolicy>;

class StorageTypeTraits {
 public:
  enum Kind {
    I8 = uint8_t(TypeCode::I8),
    I16 = uint8_t(TypeCode::I16),
    I32 = uint8_t(TypeCode::I32),
    I64 = uint8_t(TypeCode::I64),
    F32 = uint8_t(TypeCode::F32),
    F64 = uint8_t(TypeCode::F64),
    V128 = uint8_t(TypeCode::V128),
    Ref = uint8_t(AbstractReferenceTypeCode),
  };

  static bool isValidTypeCode(TypeCode tc) {
    switch (tc) {
      case TypeCode::I8:
      case TypeCode::I16:
      case TypeCode::I32:
      case TypeCode::I64:
      case TypeCode::F32:
      case TypeCode::F64:
#ifdef ENABLE_WASM_SIMD
      case TypeCode::V128:
#endif
      case TypeCode::FuncRef:
      case TypeCode::ExternRef:
      case TypeCode::ExnRef:
      case TypeCode::NullExnRef:
      case TypeCode::AnyRef:
      case TypeCode::EqRef:
      case TypeCode::I31Ref:
      case TypeCode::StructRef:
      case TypeCode::ArrayRef:
      case TypeCode::NullFuncRef:
      case TypeCode::NullExternRef:
      case TypeCode::NullAnyRef:
      case AbstractTypeRefCode:
        return true;
      default:
        return false;
    }
  }

  static bool isNumberTypeCode(TypeCode tc) {
    switch (tc) {
      case TypeCode::I32:
      case TypeCode::I64:
      case TypeCode::F32:
      case TypeCode::F64:
        return true;
      default:
        return false;
    }
  }

  static bool isPackedTypeCode(TypeCode tc) {
    switch (tc) {
      case TypeCode::I8:
      case TypeCode::I16:
        return true;
      default:
        return false;
    }
  }

  static bool isVectorTypeCode(TypeCode tc) {
    switch (tc) {
#ifdef ENABLE_WASM_SIMD
      case TypeCode::V128:
        return true;
#endif
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
    Ref = uint8_t(AbstractReferenceTypeCode),
  };

  static const char* KindEnumName(Kind kind) {
    switch (kind) {
      case Kind::I32:
        return "I32";
      case Kind::I64:
        return "I64";
      case Kind::F32:
        return "F32";
      case Kind::F64:
        return "F64";
      case Kind::V128:
        return "V128";
      case Kind::Ref:
        return "Ref";
    }
    MOZ_CRASH("Unknown kind");
  }

  static constexpr bool isValidTypeCode(TypeCode tc) {
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
      case TypeCode::ExnRef:
      case TypeCode::NullExnRef:
      case TypeCode::AnyRef:
      case TypeCode::EqRef:
      case TypeCode::I31Ref:
      case TypeCode::StructRef:
      case TypeCode::ArrayRef:
      case TypeCode::NullFuncRef:
      case TypeCode::NullExternRef:
      case TypeCode::NullAnyRef:
      case AbstractTypeRefCode:
        return true;
      default:
        return false;
    }
  }

  static bool isNumberTypeCode(TypeCode tc) {
    switch (tc) {
      case TypeCode::I32:
      case TypeCode::I64:
      case TypeCode::F32:
      case TypeCode::F64:
        return true;
      default:
        return false;
    }
  }

  static bool isPackedTypeCode(TypeCode tc) { return false; }

  static bool isVectorTypeCode(TypeCode tc) {
    switch (tc) {
#ifdef ENABLE_WASM_SIMD
      case TypeCode::V128:
        return true;
#endif
      default:
        return false;
    }
  }
};

// The PackedType represents the storage type of a WebAssembly location, whether
// parameter, local, field, or global. See specializations below for ValType and
// StorageType.

template <class T>
class PackedType : public T {
 public:
  using Kind = typename T::Kind;

 protected:
  PackedTypeCode tc_;

  explicit PackedType(TypeCode c) : tc_(PackedTypeCode::pack(c)) {
    MOZ_ASSERT(c != AbstractTypeRefCode);
    MOZ_ASSERT(isValid());
  }

  TypeCode typeCode() const {
    MOZ_ASSERT(isValid());
    return tc_.typeCode();
  }

 public:
  constexpr PackedType() : tc_(PackedTypeCode::invalid()) {}

  MOZ_IMPLICIT constexpr PackedType(Kind c)
      : tc_(PackedTypeCode::pack(TypeCode(c))) {
    MOZ_ASSERT(c != Kind::Ref);
    MOZ_ASSERT(isValid());
  }

  MOZ_IMPLICIT PackedType(RefType rt) : tc_(rt.packed()) {
    MOZ_ASSERT(isValid());
  }

  explicit constexpr PackedType(PackedTypeCode ptc) : tc_(ptc) {
    MOZ_ASSERT(isValid());
  }

  inline void AddRef() const;
  inline void Release() const;

  static constexpr PackedType i32() { return PackedType(PackedType::I32); }
  static constexpr PackedType f32() { return PackedType(PackedType::F32); }
  static constexpr PackedType i64() { return PackedType(PackedType::I64); }
  static constexpr PackedType f64() { return PackedType(PackedType::F64); }

  static PackedType fromMIRType(jit::MIRType mty) {
    switch (mty) {
      case jit::MIRType::Int32:
        return PackedType::I32;
        break;
      case jit::MIRType::Int64:
        return PackedType::I64;
        break;
      case jit::MIRType::Float32:
        return PackedType::F32;
        break;
      case jit::MIRType::Double:
        return PackedType::F64;
        break;
      case jit::MIRType::Simd128:
        return PackedType::V128;
        break;
      case jit::MIRType::WasmAnyRef:
        return PackedType::Ref;
      default:
        MOZ_CRASH("fromMIRType: unexpected type");
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

  static PackedType fromBitsUnsafe(PackedTypeCode::PackedRepr bits) {
    return PackedType(PackedTypeCode::fromBits(bits));
  }

  constexpr bool isValid() const {
    if (!tc_.isValid()) {
      return false;
    }
    return T::isValidTypeCode(tc_.typeCode());
  }

  IsoEqualsTypeCode forIsoEquals(const RecGroup* recGroup) const {
    return IsoEqualsTypeCode::forIsoEquals(tc_, recGroup);
  }

  PackedTypeCode packed() const {
    MOZ_ASSERT(isValid());
    return tc_;
  }
  PackedTypeCode* addressOfPacked() { return &tc_; }
  const PackedTypeCode* addressOfPacked() const { return &tc_; }

  PackedTypeCode::PackedRepr bitsUnsafe() const {
    MOZ_ASSERT(isValid());
    return tc_.bits();
  }

  bool isNumber() const { return T::isNumberTypeCode(tc_.typeCode()); }

  bool isPacked() const { return T::isPackedTypeCode(tc_.typeCode()); }

  bool isVector() const { return T::isVectorTypeCode(tc_.typeCode()); }

  bool isRefType() const { return tc_.isRefType(); }

  bool isFuncRef() const { return tc_.typeCode() == TypeCode::FuncRef; }

  bool isExternRef() const { return tc_.typeCode() == TypeCode::ExternRef; }

  bool isExnRef() const { return tc_.typeCode() == TypeCode::ExnRef; }

  bool isAnyRef() const { return tc_.typeCode() == TypeCode::AnyRef; }

  bool isNoFunc() const { return tc_.typeCode() == TypeCode::NullFuncRef; }

  bool isNoExtern() const { return tc_.typeCode() == TypeCode::NullExternRef; }

  bool isNoExn() const { return tc_.typeCode() == TypeCode::NullExnRef; }

  bool isNone() const { return tc_.typeCode() == TypeCode::NullAnyRef; }

  bool isEqRef() const { return tc_.typeCode() == TypeCode::EqRef; }

  bool isI31Ref() const { return tc_.typeCode() == TypeCode::I31Ref; }

  bool isStructRef() const { return tc_.typeCode() == TypeCode::StructRef; }

  bool isArrayRef() const { return tc_.typeCode() == TypeCode::ArrayRef; }

  bool isTypeRef() const { return tc_.typeCode() == AbstractTypeRefCode; }

  bool isRefRepr() const { return tc_.isRefRepr(); }

  // Returns whether the type has a default value.
  bool isDefaultable() const { return !(isRefType() && !isNullable()); }

  // Returns whether the type has a representation in JS.
  bool isExposable() const {
#if defined(ENABLE_WASM_SIMD)
    return kind() != Kind::V128 && !isExnRef() && !isNoExn();
#else
    return !isExnRef() && !isNoExn();
#endif
  }

  bool isNullable() const { return tc_.isNullable(); }

  const TypeDef* typeDef() const { return tc_.typeDef(); }

  Kind kind() const { return Kind(tc_.typeCodeAbstracted()); }

  RefType refType() const {
    MOZ_ASSERT(isRefType());
    return RefType(tc_);
  }

  RefType::Kind refTypeKind() const {
    MOZ_ASSERT(isRefType());
    return RefType(tc_).kind();
  }

  // Some types are encoded as JS::Value when they escape from Wasm (when passed
  // as parameters to imports or returned from exports).  For ExternRef the
  // Value encoding is pretty much a requirement.  For other types it's a choice
  // that may (temporarily) simplify some code.
  bool isEncodedAsJSValueOnEscape() const { return isRefType(); }

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

  // Note, ToMIRType is only correct within Wasm, where an AnyRef is represented
  // as a pointer.  At the JS/wasm boundary, an AnyRef can be represented as a
  // JS::Value, and the type translation may have to be handled specially and on
  // a case-by-case basis.
  constexpr jit::MIRType toMIRType() const {
    switch (tc_.typeCodeAbstracted()) {
      case TypeCode::I32:
        return jit::MIRType::Int32;
      case TypeCode::I64:
        return jit::MIRType::Int64;
      case TypeCode::F32:
        return jit::MIRType::Float32;
      case TypeCode::F64:
        return jit::MIRType::Double;
      case TypeCode::V128:
        return jit::MIRType::Simd128;
      case AbstractReferenceTypeCode:
        return jit::MIRType::WasmAnyRef;
      default:
        MOZ_CRASH("bad type");
    }
  }

  MaybeRefType toMaybeRefType() const;

  bool isValType() const {
    switch (tc_.typeCode()) {
      case TypeCode::I8:
      case TypeCode::I16:
        return false;
      default:
        return true;
    }
  }

  PackedType<StorageTypeTraits> storageType() const {
    MOZ_ASSERT(isValid());
    return PackedType<StorageTypeTraits>(tc_);
  }

  static bool isSubTypeOf(PackedType subType, PackedType superType) {
    // Anything is a subtype of itself.
    if (subType == superType) {
      return true;
    }

    // A reference may be a subtype of another reference
    if (subType.isRefType() && superType.isRefType()) {
      return RefType::isSubTypeOf(subType.refType(), superType.refType());
    }

    return false;
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

using StorageType = PackedType<StorageTypeTraits>;

// The dominant use of this data type is for locals and args, and profiling
// with ZenGarden and Tanks suggests an initial size of 16 minimises heap
// allocation, both in terms of blocks and bytes.
using ValTypeVector = Vector<ValType, 16, SystemAllocPolicy>;

// A MaybeRefType behaves like a mozilla::Maybe<RefType>, but saves space by
// reusing PackedTypeCode. If the inner RefType is not valid, it will be
// considered Nothing; otherwise it will be considered Some.
class MaybeRefType {
 private:
  RefType inner_;

 public:
  // Creates a MaybeRefType that isNothing().
  MaybeRefType() { MOZ_ASSERT(isNothing()); }

  // Creates a MaybeRefType that isSome().
  explicit MaybeRefType(RefType type) : inner_(type) {
    MOZ_RELEASE_ASSERT(isSome());
  }

  bool isSome() const { return inner_.isValid(); }
  bool isNothing() const { return !isSome(); }

  /* Returns the inner RefType by value. Unsafe unless |isSome()|. */
  RefType& value() & {
    MOZ_RELEASE_ASSERT(isSome());
    return inner_;
  };
  const RefType& value() const& {
    MOZ_RELEASE_ASSERT(isSome());
    return inner_;
  };

  RefType& valueOr(RefType& aDefault) {
    if (isSome()) {
      return value();
    }
    return aDefault;
  }
  const RefType& valueOr(const RefType& aDefault) {
    if (isSome()) {
      return value();
    }
    return aDefault;
  }

  bool operator==(const MaybeRefType& other) { return inner_ == other.inner_; }
  bool operator!=(const MaybeRefType& other) { return inner_ != other.inner_; }

  explicit operator bool() const { return isSome(); }

  mozilla::Maybe<wasm::RefTypeHierarchy> hierarchy() const {
    if (isSome()) {
      return mozilla::Some(value().hierarchy());
    }
    return mozilla::Nothing();
  }

  static MaybeRefType leastUpperBound(MaybeRefType a, MaybeRefType b) {
    if (a.isSome() && b.isSome()) {
      return MaybeRefType(RefType::leastUpperBound(a.value(), b.value()));
    }
    return MaybeRefType();
  }
};

template <class T>
MaybeRefType PackedType<T>::toMaybeRefType() const {
  if (!isRefType()) {
    return MaybeRefType();
  }
  return MaybeRefType(refType());
};

// ValType utilities

extern bool ToValType(JSContext* cx, HandleValue v, ValType* out);
extern bool ToRefType(JSContext* cx, HandleValue v, RefType* out);

extern UniqueChars ToString(RefType type, const TypeContext* types);
extern UniqueChars ToString(ValType type, const TypeContext* types);
extern UniqueChars ToString(StorageType type, const TypeContext* types);
extern UniqueChars ToString(const mozilla::Maybe<ValType>& type,
                            const TypeContext* types);
extern UniqueChars ToString(const MaybeRefType& type, const TypeContext* types);

}  // namespace wasm
}  // namespace js

#endif  // wasm_valtype_h
