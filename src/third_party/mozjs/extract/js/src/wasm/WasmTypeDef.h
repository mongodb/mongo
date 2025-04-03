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

#ifndef wasm_type_def_h
#define wasm_type_def_h

#include "mozilla/CheckedInt.h"
#include "mozilla/HashTable.h"

#include "js/RefCounted.h"

#include "wasm/WasmCodegenConstants.h"
#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmUtility.h"
#include "wasm/WasmValType.h"

namespace js {
namespace wasm {

using mozilla::CheckedInt32;
using mozilla::MallocSizeOf;

class RecGroup;

//=========================================================================
// Function types

// The FuncType class represents a WebAssembly function signature which takes a
// list of value types and returns an expression type. The engine uses two
// in-memory representations of the argument Vector's memory (when elements do
// not fit inline): normal malloc allocation (via SystemAllocPolicy) and
// allocation in a LifoAlloc (via LifoAllocPolicy). The former FuncType objects
// can have any lifetime since they own the memory. The latter FuncType objects
// must not outlive the associated LifoAlloc mark/release interval (which is
// currently the duration of module validation+compilation). Thus, long-lived
// objects like WasmModule must use malloced allocation.

class FuncType {
  ValTypeVector args_;
  ValTypeVector results_;
  // A packed structural type identifier for use in the call_indirect type
  // check in the prologue of functions. If this function type cannot fit in
  // this immediate, it will be NO_IMMEDIATE_TYPE_ID.
  uint32_t immediateTypeId_;

  // This function type cannot be packed into an immediate for call_indirect
  // signature checks.
  static const uint32_t NO_IMMEDIATE_TYPE_ID = UINT32_MAX;

  // Entry from JS to wasm via the JIT is currently unimplemented for
  // functions that return multiple values.
  bool temporarilyUnsupportedResultCountForJitEntry() const {
    return results().length() > MaxResultsForJitEntry;
  }
  // Calls out from wasm to JS that return multiple values is currently
  // unsupported.
  bool temporarilyUnsupportedResultCountForJitExit() const {
    return results().length() > MaxResultsForJitExit;
  }
  // For JS->wasm jit entries, temporarily disallow certain types until the
  // stubs generator is improved.
  //   * ref params may be nullable externrefs
  //   * ref results may not be type indices
  // V128 types are excluded per spec but are guarded against separately.
  bool temporarilyUnsupportedReftypeForEntry() const {
    for (ValType arg : args()) {
      if (arg.isRefType() && (!arg.isExternRef() || !arg.isNullable())) {
        return true;
      }
    }
    for (ValType result : results()) {
      if (result.isTypeRef()) {
        return true;
      }
    }
    return false;
  }
  // For wasm->JS jit exits, temporarily disallow certain types until
  // the stubs generator is improved.
  //   * ref results may be nullable externrefs
  // Unexposable types must be guarded against separately.
  bool temporarilyUnsupportedReftypeForExit() const {
    for (ValType result : results()) {
      if (result.isRefType() &&
          (!result.isExternRef() || !result.isNullable())) {
        return true;
      }
    }
    return false;
  }

  void initImmediateTypeId();

 public:
  FuncType() : args_(), results_() { initImmediateTypeId(); }

  FuncType(ValTypeVector&& args, ValTypeVector&& results)
      : args_(std::move(args)), results_(std::move(results)) {
    initImmediateTypeId();
  }

  FuncType(FuncType&&) = default;
  FuncType& operator=(FuncType&&) = default;

  [[nodiscard]] bool clone(const FuncType& src) {
    MOZ_ASSERT(args_.empty());
    MOZ_ASSERT(results_.empty());
    immediateTypeId_ = src.immediateTypeId_;
    return args_.appendAll(src.args_) && results_.appendAll(src.results_);
  }

  ValType arg(unsigned i) const { return args_[i]; }
  const ValTypeVector& args() const { return args_; }
  ValType result(unsigned i) const { return results_[i]; }
  const ValTypeVector& results() const { return results_; }

  bool hasImmediateTypeId() const {
    return immediateTypeId_ != NO_IMMEDIATE_TYPE_ID;
  }
  uint32_t immediateTypeId() const {
    MOZ_ASSERT(hasImmediateTypeId());
    return immediateTypeId_;
  }

  // The lsb for every immediate type id is set to distinguish an immediate type
  // id from a type id represented by a pointer to the global hash type set.
  static const uint32_t ImmediateBit = 0x1;

  HashNumber hash(const RecGroup* recGroup) const {
    HashNumber hn = 0;
    for (const ValType& vt : args_) {
      hn = mozilla::AddToHash(hn, vt.forMatch(recGroup).hash());
    }
    for (const ValType& vt : results_) {
      hn = mozilla::AddToHash(hn, vt.forMatch(recGroup).hash());
    }
    return hn;
  }

  // Matches two function types for isorecursive equality. See
  // "Matching type definitions" in WasmValType.h for more background.
  static bool matches(const RecGroup* lhsRecGroup, const FuncType& lhs,
                      const RecGroup* rhsRecGroup, const FuncType& rhs) {
    if (lhs.args_.length() != rhs.args_.length() ||
        lhs.results_.length() != rhs.results_.length()) {
      return false;
    }
    for (uint32_t i = 0; i < lhs.args_.length(); i++) {
      if (lhs.args_[i].forMatch(lhsRecGroup) !=
          rhs.args_[i].forMatch(rhsRecGroup)) {
        return false;
      }
    }
    for (uint32_t i = 0; i < lhs.results_.length(); i++) {
      if (lhs.results_[i].forMatch(lhsRecGroup) !=
          rhs.results_[i].forMatch(rhsRecGroup)) {
        return false;
      }
    }
    return true;
  }

  // Checks if every arg and result of the specified function types are bitwise
  // equal. Type references must therefore point to exactly the same type
  // definition instance.
  static bool strictlyEquals(const FuncType& lhs, const FuncType& rhs) {
    return EqualContainers(lhs.args(), rhs.args()) &&
           EqualContainers(lhs.results(), rhs.results());
  }

  // Checks if two function types are compatible in a given subtyping
  // relationship.
  static bool canBeSubTypeOf(const FuncType& subType,
                             const FuncType& superType) {
    // A subtype must have exactly as many arguments as its supertype
    if (subType.args().length() != superType.args().length()) {
      return false;
    }

    // A subtype must have exactly as many returns as its supertype
    if (subType.results().length() != superType.results().length()) {
      return false;
    }

    // Function result types are covariant
    for (uint32_t i = 0; i < superType.results().length(); i++) {
      if (!ValType::isSubTypeOf(subType.results()[i], superType.results()[i])) {
        return false;
      }
    }

    // Function argument types are contravariant
    for (uint32_t i = 0; i < superType.args().length(); i++) {
      if (!ValType::isSubTypeOf(superType.args()[i], subType.args()[i])) {
        return false;
      }
    }

    return true;
  }

  bool canHaveJitEntry() const;
  bool canHaveJitExit() const;

  bool hasInt64Arg() const {
    for (ValType arg : args()) {
      if (arg.kind() == ValType::Kind::I64) {
        return true;
      }
    }
    return false;
  }

  bool hasUnexposableArgOrRet() const {
    for (ValType arg : args()) {
      if (!arg.isExposable()) {
        return true;
      }
    }
    for (ValType result : results()) {
      if (!result.isExposable()) {
        return true;
      }
    }
    return false;
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  WASM_DECLARE_FRIEND_SERIALIZE(FuncType);
};

//=========================================================================
// Structure types

// The Module owns a dense array of StructType values that represent the
// structure types that the module knows about.  It is created from the sparse
// array of types in the ModuleEnvironment when the Module is created.

struct StructField {
  FieldType type;
  uint32_t offset;
  bool isMutable;

  HashNumber hash(const RecGroup* recGroup) const {
    HashNumber hn = 0;
    hn = mozilla::AddToHash(hn, type.forMatch(recGroup).hash());
    hn = mozilla::AddToHash(hn, HashNumber(isMutable));
    return hn;
  }

  // Checks if two struct fields are compatible in a given subtyping
  // relationship.
  static bool canBeSubTypeOf(const StructField& subType,
                             const StructField& superType) {
    // Mutable fields are invariant w.r.t. field types
    if (subType.isMutable && superType.isMutable) {
      return subType.type == superType.type;
    }

    // Immutable fields are covariant w.r.t. field types
    if (!subType.isMutable && !superType.isMutable) {
      return FieldType::isSubTypeOf(subType.type, superType.type);
    }

    return false;
  }
};

using StructFieldVector = Vector<StructField, 0, SystemAllocPolicy>;

using InlineTraceOffsetVector = Vector<uint32_t, 2, SystemAllocPolicy>;
using OutlineTraceOffsetVector = Vector<uint32_t, 0, SystemAllocPolicy>;

class StructType {
 public:
  StructFieldVector fields_;  // Field type, offset, and mutability
  uint32_t size_;             // The size of the type in bytes.
  InlineTraceOffsetVector inlineTraceOffsets_;
  OutlineTraceOffsetVector outlineTraceOffsets_;

 public:
  StructType() : fields_(), size_(0) {}

  explicit StructType(StructFieldVector&& fields)
      : fields_(std::move(fields)), size_(0) {}

  StructType(StructType&&) = default;
  StructType& operator=(StructType&&) = default;

  [[nodiscard]] bool clone(const StructType& src) {
    if (!fields_.appendAll(src.fields_)) {
      return false;
    }
    size_ = src.size_;
    return true;
  }

  [[nodiscard]] bool init();

  bool isDefaultable() const {
    for (auto& field : fields_) {
      if (!field.type.isDefaultable()) {
        return false;
      }
    }
    return true;
  }

  HashNumber hash(const RecGroup* recGroup) const {
    HashNumber hn = 0;
    for (const StructField& field : fields_) {
      hn = mozilla::AddToHash(hn, field.hash(recGroup));
    }
    return hn;
  }

  // Matches two struct types for isorecursive equality. See
  // "Matching type definitions" in WasmValType.h for more background.
  static bool matches(const RecGroup* lhsRecGroup, const StructType& lhs,
                      const RecGroup* rhsRecGroup, const StructType& rhs) {
    if (lhs.fields_.length() != rhs.fields_.length()) {
      return false;
    }
    for (uint32_t i = 0; i < lhs.fields_.length(); i++) {
      const StructField& lhsField = lhs.fields_[i];
      const StructField& rhsField = rhs.fields_[i];
      if (lhsField.isMutable != rhsField.isMutable ||
          lhsField.type.forMatch(lhsRecGroup) !=
              rhsField.type.forMatch(rhsRecGroup)) {
        return false;
      }
    }
    return true;
  }

  // Checks if two struct types are compatible in a given subtyping
  // relationship.
  static bool canBeSubTypeOf(const StructType& subType,
                             const StructType& superType) {
    // A subtype must have at least as many fields as its supertype
    if (subType.fields_.length() < superType.fields_.length()) {
      return false;
    }

    // Every field that is in both superType and subType must be compatible
    for (uint32_t i = 0; i < superType.fields_.length(); i++) {
      if (!StructField::canBeSubTypeOf(subType.fields_[i],
                                       superType.fields_[i])) {
        return false;
      }
    }
    return true;
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  WASM_DECLARE_FRIEND_SERIALIZE(StructType);
};

using StructTypeVector = Vector<StructType, 0, SystemAllocPolicy>;

// Utility for computing field offset and alignments, and total size for
// structs and tags.  This is complicated by fact that a WasmStructObject has
// an inline area, which is used first, and if that fills up an optional
// C++-heap-allocated outline area is used.  We need to be careful not to
// split any data item across the boundary.  This is ensured as follows:
//
// (1) the possible field sizes are 1, 2, 4, 8 and 16 only.
// (2) each field is "naturally aligned" -- aligned to its size.
// (3) MaxInlineBytes (the size of the inline area) % 16 == 0.
//
// From (1) and (2), it follows that all fields are placed so that their first
// and last bytes fall within the same 16-byte chunk.  That is,
// offset_of_first_byte_of_field / 16 == offset_of_last_byte_of_field / 16.
//
// Given that, it follows from (3) that all fields fall completely within
// either the inline or outline areas; no field crosses the boundary.
class StructLayout {
  CheckedInt32 sizeSoFar = 0;
  uint32_t structAlignment = 1;

 public:
  // The field adders return the offset of the the field.
  CheckedInt32 addField(FieldType type);

  // The close method rounds up the structure size to the appropriate
  // alignment and returns that size.
  CheckedInt32 close();
};

//=========================================================================
// Array types

class ArrayType {
 public:
  FieldType elementType_;  // field type
  bool isMutable_;         // mutability

 public:
  ArrayType() : isMutable_(false) {}
  ArrayType(FieldType elementType, bool isMutable)
      : elementType_(elementType), isMutable_(isMutable) {}

  ArrayType(const ArrayType&) = default;
  ArrayType& operator=(const ArrayType&) = default;

  ArrayType(ArrayType&&) = default;
  ArrayType& operator=(ArrayType&&) = default;

  [[nodiscard]] bool clone(const ArrayType& src) {
    elementType_ = src.elementType_;
    isMutable_ = src.isMutable_;
    return true;
  }

  bool isDefaultable() const { return elementType_.isDefaultable(); }

  HashNumber hash(const RecGroup* recGroup) const {
    HashNumber hn = 0;
    hn = mozilla::AddToHash(hn, elementType_.forMatch(recGroup).hash());
    hn = mozilla::AddToHash(hn, HashNumber(isMutable_));
    return hn;
  }

  // Matches two array types for isorecursive equality. See
  // "Matching type definitions" in WasmValType.h for more background.
  static bool matches(const RecGroup* lhsRecGroup, const ArrayType& lhs,
                      const RecGroup* rhsRecGroup, const ArrayType& rhs) {
    if (lhs.isMutable_ != rhs.isMutable_ ||
        lhs.elementType_.forMatch(lhsRecGroup) !=
            rhs.elementType_.forMatch(rhsRecGroup)) {
      return false;
    }
    return true;
  }

  // Checks if two arrays are compatible in a given subtyping relationship.
  static bool canBeSubTypeOf(const ArrayType& subType,
                             const ArrayType& superType) {
    // Mutable fields are invariant w.r.t. field types
    if (subType.isMutable_ && superType.isMutable_) {
      return subType.elementType_ == superType.elementType_;
    }

    // Immutable fields are covariant w.r.t. field types
    if (!subType.isMutable_ && !superType.isMutable_) {
      return FieldType::isSubTypeOf(subType.elementType_,
                                    superType.elementType_);
    }

    return true;
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

WASM_DECLARE_CACHEABLE_POD(ArrayType);

using ArrayTypeVector = Vector<ArrayType, 0, SystemAllocPolicy>;

//=========================================================================
// SuperTypeVector

// [SMDOC] Super type vector
//
// A super type vector is a vector representation of the linked list of super
// types that a type definition has. Every element is a raw pointer to a type
// definition. It's possible to form a vector here because type definitions
// are trees, not DAGs, with every type having at most one super type.
//
// The first element in the vector is the 'root' type definition without a
// super type. The last element is to the type definition itself.
//
// ## Subtype checking
//
// The only purpose of a super type vector is to support constant time
// subtyping checks. This is not free, it comes at the cost of worst case N^2
// metadata size growth. We limit the max subtyping depth to counter this.
//
// To perform a subtype check we rely on the following:
//  (1) a type A is a subtype (<:) of type B iff:
//        type A == type B OR
//        type B is reachable by following declared super types of type A
//  (2) we order super type vectors from least to most derived types
//  (3) the 'subtyping depth' of all type definitions is statically known
//
// With the above, we know that if type B is a super type of type A, that it
// must be in A's super type vector at type B's subtyping depth. We can
// therefore just do an index and comparison to determine if that's the case.
//
// ## Example
//
// For the following type section:
//   ..
//   12: (type (struct))
//   ..
//   34: (type (sub 12 (struct)))
//   ..
//   56: (type (sub 34 (struct)))
//   ..
//   78: (type (sub 56 (struct)))
//   ..
//
// (type 12) would have the following super type vector:
//   [(type 12)]
//
// (type 78) would have the following super type vector:
//   [(type 12), (type 34), (type 56), (type 78)]
//
// Checking that (type 78) <: (type 12) can use the fact that (type 12) will
// always be present at depth 0 of any super type vector it is in, and
// therefore check the vector at that index.
//
// ## Minimum sizing
//
// As a further optimization to avoid bounds checking, we guarantee that all
// super type vectors are at least `MinSuperTypeVectorLength`. All checks
// against indices that we know statically are at/below that can skip bounds
// checking. Extra entries added to reach the minimum size are initialized to
// null.
class SuperTypeVector {
  SuperTypeVector() : typeDef_(nullptr), length_(0) {}

  // The TypeDef for which this is the supertype vector.  That TypeDef should
  // point back to this SuperTypeVector.
  const TypeDef* typeDef_;

  // The length of types stored inline below.
  uint32_t length_;

 public:
  // Raw pointers to the super types of this type definition. Ordered from
  // least-derived to most-derived.  Do not add any fields after this point.
  const SuperTypeVector* types_[0];

  // Batch allocate super type vectors for all the types in a recursion group.
  // Returns a pointer to the first super type vector, which can be used to
  // free all vectors.
  [[nodiscard]] static const SuperTypeVector* createMultipleForRecGroup(
      RecGroup* recGroup);

  const TypeDef* typeDef() const { return typeDef_; }
  void setTypeDef(const TypeDef* typeDef) { typeDef_ = typeDef; }

  uint32_t length() const { return length_; }
  void setLength(uint32_t length) { length_ = length; }

  const SuperTypeVector* type(size_t index) const {
    MOZ_ASSERT(index < length_);
    return types_[index];
  }
  void setType(size_t index, const SuperTypeVector* type) {
    MOZ_ASSERT(index < length_);
    types_[index] = type;
  }

  // The length of a super type vector for a specific type def.
  static size_t lengthForTypeDef(const TypeDef& typeDef);
  // The byte size of a super type vector for a specific type def.
  static size_t byteSizeForTypeDef(const TypeDef& typeDef);

  static size_t offsetOfLength() { return offsetof(SuperTypeVector, length_); }
  static size_t offsetOfSelfTypeDef() {
    return offsetof(SuperTypeVector, typeDef_);
  };
  static size_t offsetOfTypeDefInVector(uint32_t typeDefDepth);
};

// Ensure it is safe to use `sizeof(SuperTypeVector)` to find the offset of
// `types_[0]`.
static_assert(offsetof(SuperTypeVector, types_) == sizeof(SuperTypeVector));

//=========================================================================
// TypeDef and supporting types

// A tagged container for the various types that can be present in a wasm
// module's type section.

enum class TypeDefKind : uint8_t {
  None = 0,
  Func,
  Struct,
  Array,
};

class TypeDef {
  uint32_t offsetToRecGroup_;

  // The supertype vector for this TypeDef.  That SuperTypeVector should point
  // back to this TypeDef.
  const SuperTypeVector* superTypeVector_;

  const TypeDef* superTypeDef_;
  uint16_t subTypingDepth_;
  TypeDefKind kind_;
  union {
    FuncType funcType_;
    StructType structType_;
    ArrayType arrayType_;
  };

  void setRecGroup(RecGroup* recGroup) {
    uintptr_t recGroupAddr = (uintptr_t)recGroup;
    uintptr_t typeDefAddr = (uintptr_t)this;
    MOZ_ASSERT(typeDefAddr > recGroupAddr);
    MOZ_ASSERT(typeDefAddr - recGroupAddr <= UINT32_MAX);
    offsetToRecGroup_ = typeDefAddr - recGroupAddr;
  }

 public:
  explicit TypeDef(RecGroup* recGroup)
      : offsetToRecGroup_(0),
        superTypeVector_(nullptr),
        superTypeDef_(nullptr),
        subTypingDepth_(0),
        kind_(TypeDefKind::None) {
    setRecGroup(recGroup);
  }

  ~TypeDef() {
    switch (kind_) {
      case TypeDefKind::Func:
        funcType_.~FuncType();
        break;
      case TypeDefKind::Struct:
        structType_.~StructType();
        break;
      case TypeDefKind::Array:
        arrayType_.~ArrayType();
        break;
      case TypeDefKind::None:
        break;
    }
  }

  TypeDef& operator=(FuncType&& that) noexcept {
    MOZ_ASSERT(isNone());
    kind_ = TypeDefKind::Func;
    new (&funcType_) FuncType(std::move(that));
    return *this;
  }

  TypeDef& operator=(StructType&& that) noexcept {
    MOZ_ASSERT(isNone());
    kind_ = TypeDefKind::Struct;
    new (&structType_) StructType(std::move(that));
    return *this;
  }

  TypeDef& operator=(ArrayType&& that) noexcept {
    MOZ_ASSERT(isNone());
    kind_ = TypeDefKind::Array;
    new (&arrayType_) ArrayType(std::move(that));
    return *this;
  }

  const SuperTypeVector* superTypeVector() const { return superTypeVector_; }

  void setSuperTypeVector(const SuperTypeVector* superTypeVector) {
    superTypeVector_ = superTypeVector;
  }

  static size_t offsetOfKind() { return offsetof(TypeDef, kind_); }

  static size_t offsetOfSuperTypeVector() {
    return offsetof(TypeDef, superTypeVector_);
  }

  const TypeDef* superTypeDef() const { return superTypeDef_; }

  uint16_t subTypingDepth() const { return subTypingDepth_; }

  const RecGroup& recGroup() const {
    uintptr_t typeDefAddr = (uintptr_t)this;
    uintptr_t recGroupAddr = typeDefAddr - offsetToRecGroup_;
    return *(const RecGroup*)recGroupAddr;
  }

  TypeDefKind kind() const { return kind_; }

  bool isNone() const { return kind_ == TypeDefKind::None; }

  bool isFuncType() const { return kind_ == TypeDefKind::Func; }

  bool isStructType() const { return kind_ == TypeDefKind::Struct; }

  bool isArrayType() const { return kind_ == TypeDefKind::Array; }

  const FuncType& funcType() const {
    MOZ_ASSERT(isFuncType());
    return funcType_;
  }

  FuncType& funcType() {
    MOZ_ASSERT(isFuncType());
    return funcType_;
  }

  const StructType& structType() const {
    MOZ_ASSERT(isStructType());
    return structType_;
  }

  StructType& structType() {
    MOZ_ASSERT(isStructType());
    return structType_;
  }

  const ArrayType& arrayType() const {
    MOZ_ASSERT(isArrayType());
    return arrayType_;
  }

  ArrayType& arrayType() {
    MOZ_ASSERT(isArrayType());
    return arrayType_;
  }

  // Get a value that can be used for matching type definitions across
  // different recursion groups.
  static inline uintptr_t forMatch(const TypeDef* typeDef,
                                   const RecGroup* recGroup);

  HashNumber hash() const {
    HashNumber hn = HashNumber(kind_);
    hn = mozilla::AddToHash(hn, TypeDef::forMatch(superTypeDef_, &recGroup()));
    switch (kind_) {
      case TypeDefKind::Func:
        hn = mozilla::AddToHash(hn, funcType_.hash(&recGroup()));
        break;
      case TypeDefKind::Struct:
        hn = mozilla::AddToHash(hn, structType_.hash(&recGroup()));
        break;
      case TypeDefKind::Array:
        hn = mozilla::AddToHash(hn, arrayType_.hash(&recGroup()));
        break;
      case TypeDefKind::None:
        break;
    }
    return hn;
  }

  // Matches two type definitions for isorecursive equality. See
  // "Matching type definitions" in WasmValType.h for more background.
  static bool matches(const TypeDef& lhs, const TypeDef& rhs) {
    if (lhs.kind_ != rhs.kind_) {
      return false;
    }
    if (TypeDef::forMatch(lhs.superTypeDef_, &lhs.recGroup()) !=
        TypeDef::forMatch(rhs.superTypeDef_, &rhs.recGroup())) {
      return false;
    }
    switch (lhs.kind_) {
      case TypeDefKind::Func:
        return FuncType::matches(&lhs.recGroup(), lhs.funcType_,
                                 &rhs.recGroup(), rhs.funcType_);
      case TypeDefKind::Struct:
        return StructType::matches(&lhs.recGroup(), lhs.structType_,
                                   &rhs.recGroup(), rhs.structType_);
      case TypeDefKind::Array:
        return ArrayType::matches(&lhs.recGroup(), lhs.arrayType_,
                                  &rhs.recGroup(), rhs.arrayType_);
      case TypeDefKind::None:
        return true;
    }
    return false;
  }

  // Checks if two type definitions are compatible in a given subtyping
  // relationship.
  static bool canBeSubTypeOf(const TypeDef* subType, const TypeDef* superType) {
    if (subType->kind() != superType->kind()) {
      return false;
    }

    switch (subType->kind_) {
      case TypeDefKind::Func:
        return FuncType::canBeSubTypeOf(subType->funcType_,
                                        superType->funcType_);
      case TypeDefKind::Struct:
        return StructType::canBeSubTypeOf(subType->structType_,
                                          superType->structType_);
      case TypeDefKind::Array:
        return ArrayType::canBeSubTypeOf(subType->arrayType_,
                                         superType->arrayType_);
      case TypeDefKind::None:
        MOZ_CRASH();
    }
    return false;
  }

  void setSuperTypeDef(const TypeDef* superTypeDef) {
    superTypeDef_ = superTypeDef;
    subTypingDepth_ = superTypeDef_->subTypingDepth_ + 1;
  }

  // Checks if `subTypeDef` is a declared sub type of `superTypeDef`.
  static bool isSubTypeOf(const TypeDef* subTypeDef,
                          const TypeDef* superTypeDef) {
    // Fast path for when the types are equal
    if (MOZ_LIKELY(subTypeDef == superTypeDef)) {
      return true;
    }
    const SuperTypeVector* subSuperTypeVector = subTypeDef->superTypeVector();

    // During construction of a recursion group, the super type vector may not
    // have been computed yet, in which case we need to fall back to a linear
    // search.
    if (!subSuperTypeVector) {
      while (subTypeDef) {
        if (subTypeDef == superTypeDef) {
          return true;
        }
        subTypeDef = subTypeDef->superTypeDef();
      }
      return false;
    }

    // The supertype vector does exist.  So check it points back here.
    MOZ_ASSERT(subSuperTypeVector->typeDef() == subTypeDef);

    // We need to check if `superTypeDef` is one of `subTypeDef`s super types
    // by checking in `subTypeDef`s super type vector. We can use the static
    // information of the depth of `superTypeDef` to index directly into the
    // vector.
    uint32_t subTypingDepth = superTypeDef->subTypingDepth();
    if (subTypingDepth >= subSuperTypeVector->length()) {
      return false;
    }

    const SuperTypeVector* superSuperTypeVector =
        superTypeDef->superTypeVector();
    MOZ_ASSERT(superSuperTypeVector);
    MOZ_ASSERT(superSuperTypeVector->typeDef() == superTypeDef);

    return subSuperTypeVector->type(subTypingDepth) == superSuperTypeVector;
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  WASM_DECLARE_FRIEND_SERIALIZE(TypeDef);
};

using SharedTypeDef = RefPtr<const TypeDef>;
using MutableTypeDef = RefPtr<TypeDef>;

using TypeDefVector = Vector<TypeDef, 0, SystemAllocPolicy>;
using TypeDefPtrVector = Vector<const TypeDef*, 0, SystemAllocPolicy>;

using TypeDefPtrToIndexMap =
    HashMap<const TypeDef*, uint32_t, PointerHasher<const TypeDef*>,
            SystemAllocPolicy>;

//=========================================================================
// RecGroup

// A recursion group is a set of type definitions that may refer to each other
// or to type definitions in another recursion group. There is an ordering
// restriction on type references such that references across recursion groups
// must be acyclic.
//
// Type definitions are stored inline in their containing recursion group, and
// have an offset to their containing recursion group. Recursion groups are
// atomically refcounted and hold strong references to other recursion groups
// they depend on.
//
// Type equality is structural in WebAssembly, and we canonicalize recursion
// groups while building them so that pointer equality of types implies
// equality of types. There is a global hash set of weak pointers to recursion
// groups that holds the current canonical instance of a recursion group.
class RecGroup : public AtomicRefCounted<RecGroup> {
  // Whether this recursion group has been finished and acquired strong
  // references to external recursion groups.
  bool finalizedTypes_;
  // The number of types stored in this recursion group.
  uint32_t numTypes_;
  // The batch allocated super type vectors for all type definitions in this
  // recursion group.
  const SuperTypeVector* vectors_;
  // The first type definition stored inline in this recursion group.
  TypeDef types_[0];

  friend class TypeContext;

  explicit RecGroup(uint32_t numTypes)
      : finalizedTypes_(false),
        numTypes_(numTypes),
        vectors_(nullptr),
        types_{} {}

  // Compute the size in bytes of a recursion group with the specified amount
  // of types.
  static constexpr size_t sizeOfRecGroup(uint32_t numTypes) {
    static_assert(MaxTypes <= SIZE_MAX / sizeof(TypeDef));
    return sizeof(RecGroup) + sizeof(TypeDef) * numTypes;
  }

  // Allocate a recursion group with the specified amount of types. The type
  // definitions will be ready to be filled in. Users must call `finish` once
  // type definitions are initialized so that strong references to external
  // recursion groups are taken.
  static RefPtr<RecGroup> allocate(uint32_t numTypes) {
    // Allocate the recursion group with the correct size
    RecGroup* recGroup = (RecGroup*)js_malloc(sizeOfRecGroup(numTypes));
    if (!recGroup) {
      return nullptr;
    }

    // Construct the recursion group and types that are stored inline
    new (recGroup) RecGroup(numTypes);
    for (uint32_t i = 0; i < numTypes; i++) {
      new (recGroup->types_ + i) TypeDef(recGroup);
    }
    return recGroup;
  }

  // Finish initialization by acquiring strong references to groups referenced
  // by type definitions.
  [[nodiscard]] bool finalizeDefinitions() {
    MOZ_ASSERT(!finalizedTypes_);
    // Super type vectors are only needed for GC and have a size/time impact
    // that we don't want to encur until we're ready for it. Only use them when
    // GC is built into the binary.
#ifdef ENABLE_WASM_GC
    vectors_ = SuperTypeVector::createMultipleForRecGroup(this);
    if (!vectors_) {
      return false;
    }
#endif
    visitReferencedGroups([](const RecGroup* recGroup) { recGroup->AddRef(); });
    finalizedTypes_ = true;
    return true;
  }

  // Visit every external recursion group that is referenced by the types in
  // this recursion group.
  template <typename Visitor>
  void visitReferencedGroups(Visitor visitor) const {
    auto visitValType = [this, visitor](ValType type) {
      if (type.isTypeRef() && &type.typeDef()->recGroup() != this) {
        visitor(&type.typeDef()->recGroup());
      }
    };
    auto visitFieldType = [this, visitor](FieldType type) {
      if (type.isTypeRef() && &type.typeDef()->recGroup() != this) {
        visitor(&type.typeDef()->recGroup());
      }
    };

    for (uint32_t i = 0; i < numTypes_; i++) {
      const TypeDef& typeDef = types_[i];

      if (typeDef.superTypeDef() &&
          &typeDef.superTypeDef()->recGroup() != this) {
        visitor(&typeDef.superTypeDef()->recGroup());
      }

      switch (typeDef.kind()) {
        case TypeDefKind::Func: {
          const FuncType& funcType = typeDef.funcType();
          for (auto type : funcType.args()) {
            visitValType(type);
          }
          for (auto type : funcType.results()) {
            visitValType(type);
          }
          break;
        }
        case TypeDefKind::Struct: {
          const StructType& structType = typeDef.structType();
          for (const auto& field : structType.fields_) {
            visitFieldType(field.type);
          }
          break;
        }
        case TypeDefKind::Array: {
          const ArrayType& arrayType = typeDef.arrayType();
          visitFieldType(arrayType.elementType_);
          break;
        }
        case TypeDefKind::None: {
          MOZ_CRASH();
        }
      }
    }
  }

 public:
  ~RecGroup() {
    // Release the referenced recursion groups if we acquired references to
    // them. Do this before the type definitions are destroyed below.
    if (finalizedTypes_) {
      finalizedTypes_ = false;
      visitReferencedGroups(
          [](const RecGroup* recGroup) { recGroup->Release(); });
    }

    if (vectors_) {
      js_free((void*)vectors_);
      vectors_ = nullptr;
    }

    // Call destructors on all the type definitions.
    for (uint32_t i = 0; i < numTypes_; i++) {
      type(i).~TypeDef();
    }
  }

  // Recursion groups cannot be copied or moved
  RecGroup& operator=(const RecGroup&) = delete;
  RecGroup& operator=(RecGroup&&) = delete;

  // Get the type definition at the group type index (not module type index).
  TypeDef& type(uint32_t groupTypeIndex) {
    // We cannot mutate type definitions after we've finalized them
    MOZ_ASSERT(!finalizedTypes_);
    return types_[groupTypeIndex];
  }
  const TypeDef& type(uint32_t groupTypeIndex) const {
    return types_[groupTypeIndex];
  }

  // The number of types stored in this recursion group.
  uint32_t numTypes() const { return numTypes_; }

  // Get the index of a type definition that's in this recursion group.
  uint32_t indexOf(const TypeDef* typeDef) const {
    MOZ_ASSERT(typeDef >= types_);
    size_t groupTypeIndex = (size_t)(typeDef - types_);
    MOZ_ASSERT(groupTypeIndex < numTypes());
    return (uint32_t)groupTypeIndex;
  }

  HashNumber hash() const {
    HashNumber hn = 0;
    for (uint32_t i = 0; i < numTypes(); i++) {
      hn = mozilla::AddToHash(hn, types_[i].hash());
    }
    return hn;
  }

  // Matches two recursion groups for isorecursive equality. See
  // "Matching type definitions" in WasmValType.h for more background.
  static bool matches(const RecGroup& lhs, const RecGroup& rhs) {
    if (lhs.numTypes() != rhs.numTypes()) {
      return false;
    }
    for (uint32_t i = 0; i < lhs.numTypes(); i++) {
      if (!TypeDef::matches(lhs.type(i), rhs.type(i))) {
        return false;
      }
    }
    return true;
  }
};

// Remove all types from the canonical type set that are not referenced from
// outside the type set.
extern void PurgeCanonicalTypes();

using SharedRecGroup = RefPtr<const RecGroup>;
using MutableRecGroup = RefPtr<RecGroup>;
using SharedRecGroupVector = Vector<SharedRecGroup, 0, SystemAllocPolicy>;

//=========================================================================
// TypeContext

// A type context holds the recursion groups and corresponding type definitions
// defined in a module.
class TypeContext : public AtomicRefCounted<TypeContext> {
  FeatureArgs features_;
  // The pending recursion group that is currently being constructed
  MutableRecGroup pendingRecGroup_;
  // An in-order list of all the recursion groups defined in this module
  SharedRecGroupVector recGroups_;
  // An in-order list of the type definitions in the module. Each type is
  // stored in a recursion group.
  TypeDefPtrVector types_;
  // A map from type definition to the original module index.
  TypeDefPtrToIndexMap moduleIndices_;

  static SharedRecGroup canonicalizeGroup(SharedRecGroup recGroup);

 public:
  TypeContext() = default;
  explicit TypeContext(const FeatureArgs& features) : features_(features) {}
  ~TypeContext();

  size_t sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
    return types_.sizeOfExcludingThis(mallocSizeOf) +
           moduleIndices_.shallowSizeOfExcludingThis(mallocSizeOf);
  }

  // Disallow copy, allow move initialization
  TypeContext(const TypeContext&) = delete;
  TypeContext& operator=(const TypeContext&) = delete;
  TypeContext(TypeContext&&) = delete;
  TypeContext& operator=(TypeContext&&) = delete;

  // Begin creating a recursion group with the specified number of types.
  // Returns a recursion group to be filled in with type definitions. This must
  // be paired with `endGroup`.
  [[nodiscard]] MutableRecGroup startRecGroup(uint32_t numTypes) {
    // We must not have a pending group
    MOZ_ASSERT(!pendingRecGroup_);

    // Create the group and add it to the list of groups
    pendingRecGroup_ = RecGroup::allocate(numTypes);
    if (!pendingRecGroup_ || !recGroups_.append(pendingRecGroup_)) {
      return nullptr;
    }

    // Store the types of the group into our index space maps. These may get
    // overwritten when we finish this group and canonicalize it. We need to do
    // this before finishing, because these entries will be used by decoding
    // and error printing.
    for (uint32_t groupTypeIndex = 0; groupTypeIndex < numTypes;
         groupTypeIndex++) {
      const TypeDef* typeDef = &pendingRecGroup_->type(groupTypeIndex);
      uint32_t typeIndex = types_.length();
      if (!types_.append(typeDef) || !moduleIndices_.put(typeDef, typeIndex)) {
        return nullptr;
      }
    }
    return pendingRecGroup_;
  }

  // Finish creation of a recursion group after type definitions have been
  // initialized. This must be paired with `startGroup`.
  [[nodiscard]] bool endRecGroup() {
    // We must have started a recursion group
    MOZ_ASSERT(pendingRecGroup_);
    MutableRecGroup recGroup = pendingRecGroup_;
    pendingRecGroup_ = nullptr;

    // Finalize the type definitions in the recursion group
    if (!recGroup->finalizeDefinitions()) {
      return false;
    }

    // Canonicalize the recursion group
    SharedRecGroup canonicalRecGroup = canonicalizeGroup(recGroup);
    if (!canonicalRecGroup) {
      return false;
    }

    // Nothing left to do if this group became the canonical group
    if (canonicalRecGroup == recGroup) {
      return true;
    }

    // Store the canonical group into the list
    recGroups_.back() = canonicalRecGroup;

    // Overwrite all the entries we stored into the index space maps when we
    // started this group.
    MOZ_ASSERT(recGroup->numTypes() == canonicalRecGroup->numTypes());
    for (uint32_t groupTypeIndex = 0; groupTypeIndex < recGroup->numTypes();
         groupTypeIndex++) {
      uint32_t typeIndex = length() - recGroup->numTypes() + groupTypeIndex;
      const TypeDef* oldTypeDef = types_[typeIndex];
      const TypeDef* newTypeDef = &canonicalRecGroup->type(groupTypeIndex);
      types_[typeIndex] = newTypeDef;
      moduleIndices_.remove(oldTypeDef);
      if (!moduleIndices_.put(newTypeDef, typeIndex)) {
        return false;
      }
    }

    return true;
  }

  template <typename T>
  [[nodiscard]] bool addType(T&& type) {
    MutableRecGroup recGroup = startRecGroup(1);
    if (!recGroup) {
      return false;
    }
    recGroup->type(0) = std::move(type);
    return endRecGroup();
  }

  const TypeDef& type(uint32_t index) const { return *types_[index]; }
  const TypeDef& operator[](uint32_t index) const { return *types_[index]; }

  bool empty() const { return types_.empty(); }
  uint32_t length() const { return types_.length(); }

  const SharedRecGroupVector& groups() const { return recGroups_; }

  // Map from type definition to index

  uint32_t indexOf(const TypeDef& typeDef) const {
    auto moduleIndex = moduleIndices_.readonlyThreadsafeLookup(&typeDef);
    MOZ_RELEASE_ASSERT(moduleIndex.found());
    return moduleIndex->value();
  }
};

using SharedTypeContext = RefPtr<const TypeContext>;
using MutableTypeContext = RefPtr<TypeContext>;

//=========================================================================
// TypeHandle

// An unambiguous strong reference to a type definition in a specific type
// context.
class TypeHandle {
 private:
  SharedTypeContext context_;
  uint32_t index_;

 public:
  TypeHandle(SharedTypeContext context, uint32_t index)
      : context_(context), index_(index) {
    MOZ_ASSERT(index_ < context_->length());
  }
  TypeHandle(SharedTypeContext context, const TypeDef& def)
      : context_(context), index_(context->indexOf(def)) {}

  TypeHandle(const TypeHandle&) = default;
  TypeHandle& operator=(const TypeHandle&) = default;

  const SharedTypeContext& context() const { return context_; }
  uint32_t index() const { return index_; }
  const TypeDef& def() const { return context_->type(index_); }
};

//=========================================================================
// misc

/* static */
inline uintptr_t TypeDef::forMatch(const TypeDef* typeDef,
                                   const RecGroup* recGroup) {
  // TypeDef is aligned sufficiently to allow a tag to distinguish a local type
  // reference (index) from a non-local type reference (pointer).
  static_assert(alignof(TypeDef) > 1);
  MOZ_ASSERT((uintptr_t(typeDef) & 0x1) == 0);

  // Return a tagged index for local type references
  if (typeDef && &typeDef->recGroup() == recGroup) {
    return uintptr_t(recGroup->indexOf(typeDef)) | 0x1;
  }

  // Return an untagged pointer for non-local type references
  return uintptr_t(typeDef);
}

/* static */
inline MatchTypeCode MatchTypeCode::forMatch(PackedTypeCode ptc,
                                             const RecGroup* recGroup) {
  MatchTypeCode mtc = {};
  mtc.typeCode = PackedRepr(ptc.typeCode());
  mtc.typeRef = TypeDef::forMatch(ptc.typeDef(), recGroup);
  mtc.nullable = ptc.isNullable();
  return mtc;
}

inline RefTypeHierarchy RefType::hierarchy() const {
  switch (kind()) {
    case RefType::Func:
    case RefType::NoFunc:
      return RefTypeHierarchy::Func;
    case RefType::Extern:
    case RefType::NoExtern:
      return RefTypeHierarchy::Extern;
    case RefType::Any:
    case RefType::None:
    case RefType::Eq:
    case RefType::Struct:
    case RefType::Array:
      return RefTypeHierarchy::Any;
    case RefType::TypeRef:
      switch (typeDef()->kind()) {
        case TypeDefKind::Struct:
        case TypeDefKind::Array:
          return RefTypeHierarchy::Any;
        case TypeDefKind::Func:
          return RefTypeHierarchy::Func;
        case TypeDefKind::None:
          MOZ_CRASH();
      }
  }
  MOZ_CRASH("switch is exhaustive");
}

inline TableRepr RefType::tableRepr() const {
  switch (hierarchy()) {
    case RefTypeHierarchy::Any:
    case RefTypeHierarchy::Extern:
      return TableRepr::Ref;
    case RefTypeHierarchy::Func:
      return TableRepr::Func;
  }
  MOZ_CRASH("switch is exhaustive");
}

inline bool RefType::isFuncHierarchy() const {
  return hierarchy() == RefTypeHierarchy::Func;
}
inline bool RefType::isExternHierarchy() const {
  return hierarchy() == RefTypeHierarchy::Extern;
}
inline bool RefType::isAnyHierarchy() const {
  return hierarchy() == RefTypeHierarchy::Any;
}

/* static */
inline bool RefType::isSubTypeOf(RefType subType, RefType superType) {
  // Anything is a subtype of itself.
  if (subType == superType) {
    return true;
  }

  // A subtype must have the same nullability as the supertype or the
  // supertype must be nullable.
  if (subType.isNullable() && !superType.isNullable()) {
    return false;
  }

  // Non type-index references are subtypes if they have the same kind
  if (!subType.isTypeRef() && !superType.isTypeRef() &&
      subType.kind() == superType.kind()) {
    return true;
  }

  // eqref is a subtype of anyref
  if (subType.isEq() && superType.isAny()) {
    return true;
  }

  // structref/arrayref are subtypes of eqref and anyref
  if ((subType.isStruct() || subType.isArray()) &&
      (superType.isAny() || superType.isEq())) {
    return true;
  }

  // Structs are subtypes of structref, eqref and anyref
  if (subType.isTypeRef() && subType.typeDef()->isStructType() &&
      (superType.isAny() || superType.isEq() || superType.isStruct())) {
    return true;
  }

  // Arrays are subtypes of arrayref, eqref and anyref
  if (subType.isTypeRef() && subType.typeDef()->isArrayType() &&
      (superType.isAny() || superType.isEq() || superType.isArray())) {
    return true;
  }

  // Funcs are subtypes of funcref
  if (subType.isTypeRef() && subType.typeDef()->isFuncType() &&
      superType.isFunc()) {
    return true;
  }

  // Type references can be subtypes
  if (subType.isTypeRef() && superType.isTypeRef()) {
    return TypeDef::isSubTypeOf(subType.typeDef(), superType.typeDef());
  }

  // No func is the bottom type of the func hierarchy
  if (subType.isNoFunc() && superType.hierarchy() == RefTypeHierarchy::Func) {
    return true;
  }

  // No extern is the bottom type of the extern hierarchy
  if (subType.isNoExtern() &&
      superType.hierarchy() == RefTypeHierarchy::Extern) {
    return true;
  }

  // None is the bottom type of the any hierarchy
  if (subType.isNone() && superType.hierarchy() == RefTypeHierarchy::Any) {
    return true;
  }

  return false;
}

/* static */
inline bool RefType::castPossible(RefType sourceType, RefType destType) {
  // Nullable types always have null in common.
  if (sourceType.isNullable() && destType.isNullable()) {
    return true;
  }

  // At least one of the types is non-nullable, so the only common values can be
  // non-null. Therefore, if either type is a bottom type, common values are
  // impossible.
  if (sourceType.isRefBottom() || destType.isRefBottom()) {
    return false;
  }

  // After excluding bottom types, our type hierarchy is a tree, and after
  // excluding nulls, subtype relationships are sufficient to tell if the types
  // share any values. If neither type is a subtype of the other, then they are
  // on different branches of the tree and completely disjoint.
  RefType sourceNonNull = sourceType.withIsNullable(false);
  RefType destNonNull = destType.withIsNullable(false);
  return RefType::isSubTypeOf(sourceNonNull, destNonNull) ||
         RefType::isSubTypeOf(destNonNull, sourceNonNull);
}

//=========================================================================
// [SMDOC] Signatures and runtime types
//
// TypeIdDesc describes the runtime representation of a TypeDef suitable for
// type equality checks. The kind of representation depends on whether the type
// is a function or a GC type. This design is in flux and will evolve.
//
// # Function types
//
// For functions in the general case, a FuncType is allocated and stored in a
// process-wide hash table, so that pointer equality implies structural
// equality. This process does not correctly handle type references (which would
// require hash-consing of infinite-trees), but that's okay while
// function-references and gc-types are experimental.
//
// A pointer to the hash table entry is stored in the global data
// area for each instance, and TypeIdDesc gives the offset to this entry.
//
// ## Immediate function types
//
// As an optimization for the 99% case where the FuncType has a small number of
// parameters, the FuncType is bit-packed into a uint32 immediate value so that
// integer equality implies structural equality. Both cases can be handled with
// a single comparison by always setting the LSB for the immediates
// (the LSB is necessarily 0 for allocated FuncType pointers due to alignment).
//
// # GC types
//
// For GC types, an entry is always created in the global data area and a
// unique RttValue (see wasm/TypedObject.h) is stored there. This RttValue
// is the value given by 'rtt.canon $t' for each type definition. As each entry
// is given a unique value and no canonicalization is done (which would require
// hash-consing of infinite-trees), this is not yet spec compliant.
//
// # wasm::Instance and the global type context
//
// As GC objects (aka TypedObject) may outlive the module they are created in,
// types are additionally transferred to a wasm::Context (which is part of
// JSContext) upon instantiation. This wasm::Context contains the
// 'global type context' that RTTValues refer to by type index. Types are never
// freed from the global type context as that would shift the index space. In
// the future, this will be fixed.

}  // namespace wasm
}  // namespace js

#endif  // wasm_type_def_h
