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

#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmUtility.h"
#include "wasm/WasmValType.h"

namespace js {
namespace wasm {

using mozilla::MallocSizeOf;

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
      if (arg.isReference() && (!arg.isExternRef() || !arg.isNullable())) {
        return true;
      }
    }
    for (ValType result : results()) {
      if (result.isTypeIndex()) {
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
      if (result.isReference() &&
          (!result.isExternRef() || !result.isNullable())) {
        return true;
      }
    }
    return false;
  }

 public:
  FuncType() : args_(), results_() {}
  FuncType(ValTypeVector&& args, ValTypeVector&& results)
      : args_(std::move(args)), results_(std::move(results)) {}

  [[nodiscard]] bool clone(const FuncType& src) {
    MOZ_ASSERT(args_.empty());
    MOZ_ASSERT(results_.empty());
    return args_.appendAll(src.args_) && results_.appendAll(src.results_);
  }

  void renumber(const RenumberMap& map) {
    for (auto& arg : args_) {
      arg.renumber(map);
    }
    for (auto& result : results_) {
      result.renumber(map);
    }
  }
  void offsetTypeIndex(uint32_t offsetBy) {
    for (auto& arg : args_) {
      arg.offsetTypeIndex(offsetBy);
    }
    for (auto& result : results_) {
      result.offsetTypeIndex(offsetBy);
    }
  }

  ValType arg(unsigned i) const { return args_[i]; }
  const ValTypeVector& args() const { return args_; }
  ValType result(unsigned i) const { return results_[i]; }
  const ValTypeVector& results() const { return results_; }

  HashNumber hash() const {
    HashNumber hn = 0;
    for (const ValType& vt : args_) {
      hn = mozilla::AddToHash(hn, HashNumber(vt.packed().bits()));
    }
    for (const ValType& vt : results_) {
      hn = mozilla::AddToHash(hn, HashNumber(vt.packed().bits()));
    }
    return hn;
  }
  bool operator==(const FuncType& rhs) const {
    return EqualContainers(args(), rhs.args()) &&
           EqualContainers(results(), rhs.results());
  }
  bool operator!=(const FuncType& rhs) const { return !(*this == rhs); }

  bool canHaveJitEntry() const;
  bool canHaveJitExit() const;

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

#ifdef WASM_PRIVATE_REFTYPES
  bool exposesTypeIndex() const {
    for (const ValType& arg : args()) {
      if (arg.isTypeIndex()) {
        return true;
      }
    }
    for (const ValType& result : results()) {
      if (result.isTypeIndex()) {
        return true;
      }
    }
    return false;
  }
#endif

  WASM_DECLARE_SERIALIZABLE(FuncType)
};

struct FuncTypeHashPolicy {
  using Lookup = const FuncType&;
  static HashNumber hash(Lookup ft) { return ft.hash(); }
  static bool match(const FuncType* lhs, Lookup rhs) { return *lhs == rhs; }
};

// Structure type.
//
// The Module owns a dense array of StructType values that represent the
// structure types that the module knows about.  It is created from the sparse
// array of types in the ModuleEnvironment when the Module is created.

struct StructField {
  FieldType type;
  uint32_t offset;
  bool isMutable;
};

using StructFieldVector = Vector<StructField, 0, SystemAllocPolicy>;

class StructType {
 public:
  StructFieldVector fields_;  // Field type, offset, and mutability
  uint32_t size_;             // The size of the type in bytes.

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

  void renumber(const RenumberMap& map) {
    for (auto& field : fields_) {
      field.type.renumber(map);
    }
  }
  void offsetTypeIndex(uint32_t offsetBy) {
    for (auto& field : fields_) {
      field.type.offsetTypeIndex(offsetBy);
    }
  }

  bool isDefaultable() const {
    for (auto& field : fields_) {
      if (!field.type.isDefaultable()) {
        return false;
      }
    }
    return true;
  }
  [[nodiscard]] bool computeLayout();

  WASM_DECLARE_SERIALIZABLE(StructType)
};

using StructTypeVector = Vector<StructType, 0, SystemAllocPolicy>;
using StructTypePtrVector = Vector<const StructType*, 0, SystemAllocPolicy>;

// Array type

class ArrayType {
 public:
  FieldType elementType_;  // field type
  bool isMutable_;         // mutability

 public:
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

  void renumber(const RenumberMap& map) { elementType_.renumber(map); }
  void offsetTypeIndex(uint32_t offsetBy) {
    elementType_.offsetTypeIndex(offsetBy);
  }

  bool isDefaultable() const { return elementType_.isDefaultable(); }

  WASM_DECLARE_SERIALIZABLE(ArrayType)
};

using ArrayTypeVector = Vector<ArrayType, 0, SystemAllocPolicy>;
using ArrayTypePtrVector = Vector<const ArrayType*, 0, SystemAllocPolicy>;

// A tagged container for the various types that can be present in a wasm
// module's type section.

enum class TypeDefKind : uint8_t {
  None = 0,
  Func,
  Struct,
  Array,
};

class TypeDef {
  TypeDefKind kind_;
  union {
    FuncType funcType_;
    StructType structType_;
    ArrayType arrayType_;
  };

 public:
  TypeDef() : kind_(TypeDefKind::None) {}

  explicit TypeDef(FuncType&& funcType)
      : kind_(TypeDefKind::Func), funcType_(std::move(funcType)) {}

  explicit TypeDef(StructType&& structType)
      : kind_(TypeDefKind::Struct), structType_(std::move(structType)) {}

  explicit TypeDef(ArrayType&& arrayType)
      : kind_(TypeDefKind::Array), arrayType_(std::move(arrayType)) {}

  TypeDef(TypeDef&& td) noexcept : kind_(td.kind_) {
    switch (kind_) {
      case TypeDefKind::Func:
        new (&funcType_) FuncType(std::move(td.funcType_));
        break;
      case TypeDefKind::Struct:
        new (&structType_) StructType(std::move(td.structType_));
        break;
      case TypeDefKind::Array:
        new (&arrayType_) ArrayType(std::move(td.arrayType_));
        break;
      case TypeDefKind::None:
        break;
    }
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

  TypeDef& operator=(TypeDef&& that) noexcept {
    MOZ_ASSERT(isNone());
    switch (that.kind_) {
      case TypeDefKind::Func:
        new (&funcType_) FuncType(std::move(that.funcType_));
        break;
      case TypeDefKind::Struct:
        new (&structType_) StructType(std::move(that.structType_));
        break;
      case TypeDefKind::Array:
        new (&arrayType_) ArrayType(std::move(that.arrayType_));
        break;
      case TypeDefKind::None:
        break;
    }
    kind_ = that.kind_;
    return *this;
  }

  [[nodiscard]] bool clone(const TypeDef& src) {
    MOZ_ASSERT(isNone());
    kind_ = src.kind_;
    switch (src.kind_) {
      case TypeDefKind::Func:
        new (&funcType_) FuncType();
        return funcType_.clone(src.funcType());
      case TypeDefKind::Struct:
        new (&structType_) StructType();
        return structType_.clone(src.structType());
      case TypeDefKind::Array:
        new (&arrayType_) ArrayType(src.arrayType());
        return true;
      case TypeDefKind::None:
        break;
    }
    MOZ_ASSERT_UNREACHABLE();
    return false;
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

  void renumber(const RenumberMap& map) {
    switch (kind_) {
      case TypeDefKind::Func:
        funcType_.renumber(map);
        break;
      case TypeDefKind::Struct:
        structType_.renumber(map);
        break;
      case TypeDefKind::Array:
        arrayType_.renumber(map);
        break;
      case TypeDefKind::None:
        break;
    }
  }
  void offsetTypeIndex(uint32_t offsetBy) {
    switch (kind_) {
      case TypeDefKind::Func:
        funcType_.offsetTypeIndex(offsetBy);
        break;
      case TypeDefKind::Struct:
        structType_.offsetTypeIndex(offsetBy);
        break;
      case TypeDefKind::Array:
        arrayType_.offsetTypeIndex(offsetBy);
        break;
      case TypeDefKind::None:
        break;
    }
  }

  WASM_DECLARE_SERIALIZABLE(TypeDef)
};

using TypeDefVector = Vector<TypeDef, 0, SystemAllocPolicy>;

template <typename T>
using DerivedTypeDefVector = Vector<T, 0, SystemAllocPolicy>;

// A type cache maintains a cache of equivalence and subtype relations between
// wasm types. This is required for the computation of equivalence and subtyping
// on recursive types.
//
// This class is not thread-safe and so must exist separately from TypeContext,
// which may be shared between multiple threads.

class TypeCache {
  using TypeIndex = uint32_t;
  using TypePair = uint64_t;
  using TypeSet = HashSet<TypePair, DefaultHasher<TypePair>, SystemAllocPolicy>;

  // Generates a hash key for the ordered pair (a, b).
  static constexpr TypePair makeOrderedPair(TypeIndex a, TypeIndex b) {
    return (TypePair(a) << 32) | TypePair(b);
  }

  // Generates a hash key for the unordered pair (a, b).
  static constexpr TypePair makeUnorderedPair(TypeIndex a, TypeIndex b) {
    if (a < b) {
      return (TypePair(a) << 32) | TypePair(b);
    }
    return (TypePair(b) << 32) | TypePair(a);
  }

  TypeSet equivalence_;
  TypeSet subtype_;

 public:
  TypeCache() = default;

  // Mark `a` as equivalent to `b` in the equivalence cache.
  [[nodiscard]] bool markEquivalent(TypeIndex a, TypeIndex b) {
    return equivalence_.put(makeUnorderedPair(a, b));
  }
  // Unmark `a` as equivalent to `b` in the equivalence cache
  void unmarkEquivalent(TypeIndex a, TypeIndex b) {
    equivalence_.remove(makeUnorderedPair(a, b));
  }

  // Check if `a` is equivalent to `b` in the equivalence cache
  bool isEquivalent(TypeIndex a, TypeIndex b) {
    return equivalence_.has(makeUnorderedPair(a, b));
  }

  // Mark `a` as a subtype of `b` in the subtype cache
  [[nodiscard]] bool markSubtypeOf(TypeIndex a, TypeIndex b) {
    return subtype_.put(makeOrderedPair(a, b));
  }
  // Unmark `a` as a subtype of `b` in the subtype cache
  void unmarkSubtypeOf(TypeIndex a, TypeIndex b) {
    subtype_.remove(makeOrderedPair(a, b));
  }
  // Check if `a` is a subtype of `b` in the subtype cache
  bool isSubtypeOf(TypeIndex a, TypeIndex b) {
    return subtype_.has(makeOrderedPair(a, b));
  }
};

// The result of an equivalence or subtyping check between types.
enum class TypeResult {
  True,
  False,
  OOM,
};

// A type context maintains an index space for TypeDef's that can be used to
// give ValType's meaning. It is used during compilation for modules, and
// during runtime for all instances.

class TypeContext {
  FeatureArgs features_;
  TypeDefVector types_;

 public:
  TypeContext(const FeatureArgs& features, TypeDefVector&& types)
      : features_(features), types_(std::move(types)) {}

  size_t sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
    return types_.sizeOfExcludingThis(mallocSizeOf);
  }

  // Disallow copy, allow move initialization
  TypeContext(const TypeContext&) = delete;
  TypeContext& operator=(const TypeContext&) = delete;
  TypeContext(TypeContext&&) = default;
  TypeContext& operator=(TypeContext&&) = default;

  TypeDef& type(uint32_t index) { return types_[index]; }
  const TypeDef& type(uint32_t index) const { return types_[index]; }

  TypeDef& operator[](uint32_t index) { return types_[index]; }
  const TypeDef& operator[](uint32_t index) const { return types_[index]; }

  uint32_t length() const { return types_.length(); }

  template <typename U>
  [[nodiscard]] bool append(U&& typeDef) {
    return types_.append(std::forward<U>(typeDef));
  }
  [[nodiscard]] bool resize(uint32_t length) { return types_.resize(length); }

  template <typename T>
  [[nodiscard]] bool transferTypes(const DerivedTypeDefVector<T>& types,
                                   uint32_t* baseIndex) {
    *baseIndex = length();
    if (!resize(*baseIndex + types.length())) {
      return false;
    }
    for (uint32_t i = 0; i < types.length(); i++) {
      if (!types_[*baseIndex + i].clone(types[i])) {
        return false;
      }
      types_[*baseIndex + i].offsetTypeIndex(*baseIndex);
    }
    return true;
  }

  // FuncType accessors

  bool isFuncType(uint32_t index) const { return types_[index].isFuncType(); }
  bool isFuncType(RefType t) const {
    return t.isTypeIndex() && isFuncType(t.typeIndex());
  }

  FuncType& funcType(uint32_t index) { return types_[index].funcType(); }
  const FuncType& funcType(uint32_t index) const {
    return types_[index].funcType();
  }
  FuncType& funcType(RefType t) { return funcType(t.typeIndex()); }
  const FuncType& funcType(RefType t) const { return funcType(t.typeIndex()); }

  // StructType accessors

  bool isStructType(uint32_t index) const {
    return types_[index].isStructType();
  }
  bool isStructType(RefType t) const {
    return t.isTypeIndex() && isStructType(t.typeIndex());
  }

  StructType& structType(uint32_t index) { return types_[index].structType(); }
  const StructType& structType(uint32_t index) const {
    return types_[index].structType();
  }
  StructType& structType(RefType t) { return structType(t.typeIndex()); }
  const StructType& structType(RefType t) const {
    return structType(t.typeIndex());
  }

  // StructType accessors

  bool isArrayType(uint32_t index) const { return types_[index].isArrayType(); }
  bool isArrayType(RefType t) const {
    return t.isTypeIndex() && isArrayType(t.typeIndex());
  }

  ArrayType& arrayType(uint32_t index) { return types_[index].arrayType(); }
  const ArrayType& arrayType(uint32_t index) const {
    return types_[index].arrayType();
  }
  ArrayType& arrayType(RefType t) { return arrayType(t.typeIndex()); }
  const ArrayType& arrayType(RefType t) const {
    return arrayType(t.typeIndex());
  }

  // Type equivalence

  template <class T>
  TypeResult isEquivalent(T one, T two, TypeCache* cache) const {
    // Anything's equal to itself.
    if (one == two) {
      return TypeResult::True;
    }

    // A reference may be equal to another reference
    if (one.isReference() && two.isReference()) {
      return isRefEquivalent(one.refType(), two.refType(), cache);
    }

#ifdef ENABLE_WASM_GC
    // An rtt may be a equal to another rtt
    if (one.isRtt() && two.isRtt()) {
      return isTypeIndexEquivalent(one.typeIndex(), two.typeIndex(), cache);
    }
#endif

    return TypeResult::False;
  }

  TypeResult isRefEquivalent(RefType one, RefType two, TypeCache* cache) const;
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
  TypeResult isTypeIndexEquivalent(uint32_t one, uint32_t two,
                                   TypeCache* cache) const;
#endif
#ifdef ENABLE_WASM_GC
  TypeResult isStructEquivalent(uint32_t oneIndex, uint32_t twoIndex,
                                TypeCache* cache) const;
  TypeResult isStructFieldEquivalent(const StructField one,
                                     const StructField two,
                                     TypeCache* cache) const;
  TypeResult isArrayEquivalent(uint32_t oneIndex, uint32_t twoIndex,
                               TypeCache* cache) const;
  TypeResult isArrayElementEquivalent(const ArrayType& one,
                                      const ArrayType& two,
                                      TypeCache* cache) const;
#endif

  // Subtyping

  template <class T>
  TypeResult isSubtypeOf(T one, T two, TypeCache* cache) const {
    // Anything's a subtype of itself.
    if (one == two) {
      return TypeResult::True;
    }

    // A reference may be a subtype of another reference
    if (one.isReference() && two.isReference()) {
      return isRefSubtypeOf(one.refType(), two.refType(), cache);
    }

    // An rtt may be a subtype of another rtt
#ifdef ENABLE_WASM_GC
    if (one.isRtt() && two.isRtt()) {
      return isTypeIndexEquivalent(one.typeIndex(), two.typeIndex(), cache);
    }
#endif

    return TypeResult::False;
  }

  TypeResult isRefSubtypeOf(RefType one, RefType two, TypeCache* cache) const;
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
  TypeResult isTypeIndexSubtypeOf(uint32_t one, uint32_t two,
                                  TypeCache* cache) const;
#endif

#ifdef ENABLE_WASM_GC
  TypeResult isStructSubtypeOf(uint32_t oneIndex, uint32_t twoIndex,
                               TypeCache* cache) const;
  TypeResult isStructFieldSubtypeOf(const StructField one,
                                    const StructField two,
                                    TypeCache* cache) const;
  TypeResult isArraySubtypeOf(uint32_t oneIndex, uint32_t twoIndex,
                              TypeCache* cache) const;
  TypeResult isArrayElementSubtypeOf(const ArrayType& one, const ArrayType& two,
                                     TypeCache* cache) const;
#endif
};

class TypeHandle {
 private:
  uint32_t index_;

 public:
  explicit TypeHandle(uint32_t index) : index_(index) {}

  TypeHandle(const TypeHandle&) = default;
  TypeHandle& operator=(const TypeHandle&) = default;

  TypeDef& get(TypeContext* tycx) const { return tycx->type(index_); }
  const TypeDef& get(const TypeContext* tycx) const {
    return tycx->type(index_);
  }

  uint32_t index() const { return index_; }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_type_def_h
