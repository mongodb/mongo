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

#ifndef wasm_expr_type_h
#define wasm_expr_type_h

#include <stdint.h>

#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"

namespace js {
namespace wasm {

template <typename PointerType>
class TaggedValue {
 public:
  enum Kind {
    ImmediateKind1 = 0,
    ImmediateKind2 = 1,
    PointerKind1 = 2,
    PointerKind2 = 3
  };
  using PackedRepr = uint64_t;
  static_assert(std::is_same<PackedTypeCode::PackedRepr, uint64_t>(),
                "can use pointer tagging with PackedTypeCode");

 private:
  PackedRepr bits_;

  static constexpr PackedRepr PayloadShift = 2;
  static constexpr PackedRepr KindMask = 0x3;
  static constexpr PackedRepr PointerKindBit = 0x2;

  constexpr static bool IsPointerKind(Kind kind) {
    return PackedRepr(kind) & PointerKindBit;
  }
  constexpr static bool IsImmediateKind(Kind kind) {
    return !IsPointerKind(kind);
  }

  static_assert(IsImmediateKind(ImmediateKind1), "immediate kind 1");
  static_assert(IsImmediateKind(ImmediateKind2), "immediate kind 2");
  static_assert(IsPointerKind(PointerKind1), "pointer kind 1");
  static_assert(IsPointerKind(PointerKind2), "pointer kind 2");

  static PackedRepr PackImmediate(Kind kind, PackedRepr imm) {
    MOZ_ASSERT(IsImmediateKind(kind));
    MOZ_ASSERT((PackedRepr(kind) & KindMask) == kind);
    MOZ_ASSERT((imm & (PackedRepr(KindMask)
                       << ((sizeof(PackedRepr) * 8) - PayloadShift))) == 0);
    return PackedRepr(kind) | (PackedRepr(imm) << PayloadShift);
  }

  static PackedRepr PackPointer(Kind kind, PointerType* ptr) {
    PackedRepr ptrBits = reinterpret_cast<PackedRepr>(ptr);
    MOZ_ASSERT(IsPointerKind(kind));
    MOZ_ASSERT((PackedRepr(kind) & KindMask) == kind);
    MOZ_ASSERT((ptrBits & KindMask) == 0);
    return PackedRepr(kind) | ptrBits;
  }

 public:
  TaggedValue(Kind kind, PackedRepr imm) : bits_(PackImmediate(kind, imm)) {}
  TaggedValue(Kind kind, PointerType* ptr) : bits_(PackPointer(kind, ptr)) {}

  PackedRepr bits() const { return bits_; }
  Kind kind() const { return Kind(bits() & KindMask); }
  PackedRepr immediate() const {
    MOZ_ASSERT(IsImmediateKind(kind()));
    return mozilla::AssertedCast<PackedRepr>(bits() >> PayloadShift);
  }
  PointerType* pointer() const {
    MOZ_ASSERT(IsPointerKind(kind()));
    return reinterpret_cast<PointerType*>(bits() & ~KindMask);
  }
};

// ResultType represents the WebAssembly spec's `resulttype`. Semantically, a
// result type is just a vec(valtype).  For effiency, though, the ResultType
// value is packed into a word, with separate encodings for these 3 cases:
//  []
//  [valtype]
//  pointer to ValTypeVector
//
// Additionally there is an encoding indicating uninitialized ResultType
// values.
//
// Generally in the latter case the ValTypeVector is the args() or results() of
// a FuncType in the compilation unit, so as long as the lifetime of the
// ResultType value is less than the OpIter, we can just borrow the pointer
// without ownership or copying.
class ResultType {
  using Tagged = TaggedValue<const ValTypeVector>;
  Tagged tagged_;

  enum Kind {
    EmptyKind = Tagged::ImmediateKind1,
    SingleKind = Tagged::ImmediateKind2,
    VectorKind = Tagged::PointerKind1,
    InvalidKind = Tagged::PointerKind2,
  };

  ResultType(Kind kind, Tagged::PackedRepr imm)
      : tagged_(Tagged::Kind(kind), imm) {}
  explicit ResultType(const ValTypeVector* ptr)
      : tagged_(Tagged::Kind(VectorKind), ptr) {}

  Kind kind() const { return Kind(tagged_.kind()); }

  ValType singleValType() const {
    MOZ_ASSERT(kind() == SingleKind);
    return ValType(PackedTypeCode::fromBits(tagged_.immediate()));
  }

  const ValTypeVector& values() const {
    MOZ_ASSERT(kind() == VectorKind);
    return *tagged_.pointer();
  }

 public:
  ResultType() : tagged_(Tagged::Kind(InvalidKind), nullptr) {}

  static ResultType Empty() {
    return ResultType(EmptyKind, Tagged::PackedRepr(0));
  }
  static ResultType Single(ValType vt) {
    return ResultType(SingleKind, vt.bitsUnsafe());
  }
  static ResultType Vector(const ValTypeVector& vals) {
    switch (vals.length()) {
      case 0:
        return Empty();
      case 1:
        return Single(vals[0]);
      default:
        return ResultType(&vals);
    }
  }

  [[nodiscard]] bool cloneToVector(ValTypeVector* out) {
    MOZ_ASSERT(out->empty());
    switch (kind()) {
      case EmptyKind:
        return true;
      case SingleKind:
        return out->append(singleValType());
      case VectorKind:
        return out->appendAll(values());
      default:
        MOZ_CRASH("bad resulttype");
    }
  }

  bool valid() const { return kind() != InvalidKind; }
  bool empty() const { return kind() == EmptyKind; }

  size_t length() const {
    switch (kind()) {
      case EmptyKind:
        return 0;
      case SingleKind:
        return 1;
      case VectorKind:
        return values().length();
      default:
        MOZ_CRASH("bad resulttype");
    }
  }

  // See also wasm::CheckIsSubtypeOf in WasmValidate.cpp.
  static bool isSubTypeOf(ResultType subType, ResultType superType) {
    if (subType.length() != superType.length()) {
      return false;
    }
    for (size_t i = 0; i < subType.length(); i++) {
      if (!ValType::isSubTypeOf(subType[i], superType[i])) {
        return false;
      }
    }
    return true;
  }

  // Polyfill the Span API, which is polyfilling the std library
  size_t size() const { return length(); }

  ValType operator[](size_t i) const {
    switch (kind()) {
      case SingleKind:
        MOZ_ASSERT(i == 0);
        return singleValType();
      case VectorKind:
        return values()[i];
      default:
        MOZ_CRASH("bad resulttype");
    }
  }
};

// BlockType represents the WebAssembly spec's `blocktype`. Semantically, a
// block type is just a (vec(valtype) -> vec(valtype)) with four special
// encodings which are represented explicitly in BlockType:
//  [] -> []
//  [] -> [valtype]
//  [params] -> [results] via pointer to FuncType
//  [] -> [results] via pointer to FuncType (ignoring [params])

class BlockType {
  using Tagged = TaggedValue<const FuncType>;
  Tagged tagged_;

  enum Kind {
    VoidToVoidKind = Tagged::ImmediateKind1,
    VoidToSingleKind = Tagged::ImmediateKind2,
    FuncKind = Tagged::PointerKind1,
    FuncResultsKind = Tagged::PointerKind2
  };

  BlockType(Kind kind, Tagged::PackedRepr imm)
      : tagged_(Tagged::Kind(kind), imm) {}
  BlockType(Kind kind, const FuncType& type)
      : tagged_(Tagged::Kind(kind), &type) {}

  Kind kind() const { return Kind(tagged_.kind()); }
  ValType singleValType() const {
    MOZ_ASSERT(kind() == VoidToSingleKind);
    return ValType(PackedTypeCode::fromBits(tagged_.immediate()));
  }

  const FuncType& funcType() const { return *tagged_.pointer(); }

 public:
  BlockType()
      : tagged_(Tagged::Kind(VoidToVoidKind),
                PackedTypeCode::invalid().bits()) {}

  static BlockType VoidToVoid() {
    return BlockType(VoidToVoidKind, Tagged::PackedRepr(0));
  }
  static BlockType VoidToSingle(ValType vt) {
    return BlockType(VoidToSingleKind, vt.bitsUnsafe());
  }
  static BlockType Func(const FuncType& type) {
    if (type.args().length() == 0) {
      return FuncResults(type);
    }
    return BlockType(FuncKind, type);
  }
  static BlockType FuncResults(const FuncType& type) {
    switch (type.results().length()) {
      case 0:
        return VoidToVoid();
      case 1:
        return VoidToSingle(type.results()[0]);
      default:
        return BlockType(FuncResultsKind, type);
    }
  }

  ResultType params() const {
    switch (kind()) {
      case VoidToVoidKind:
      case VoidToSingleKind:
      case FuncResultsKind:
        return ResultType::Empty();
      case FuncKind:
        return ResultType::Vector(funcType().args());
      default:
        MOZ_CRASH("unexpected kind");
    }
  }

  ResultType results() const {
    switch (kind()) {
      case VoidToVoidKind:
        return ResultType::Empty();
      case VoidToSingleKind:
        return ResultType::Single(singleValType());
      case FuncKind:
      case FuncResultsKind:
        return ResultType::Vector(funcType().results());
      default:
        MOZ_CRASH("unexpected kind");
    }
  }

  bool operator==(BlockType rhs) const {
    if (kind() != rhs.kind()) {
      return false;
    }
    switch (kind()) {
      case VoidToVoidKind:
      case VoidToSingleKind:
        return tagged_.bits() == rhs.tagged_.bits();
      case FuncKind:
        return FuncType::strictlyEquals(funcType(), rhs.funcType());
      case FuncResultsKind:
        return EqualContainers(funcType().results(), rhs.funcType().results());
      default:
        MOZ_CRASH("unexpected kind");
    }
  }

  bool operator!=(BlockType rhs) const { return !(*this == rhs); }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_expr_type_h
