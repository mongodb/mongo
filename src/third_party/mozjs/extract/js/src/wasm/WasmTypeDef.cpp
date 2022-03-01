/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
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

#include "wasm/WasmTypeDef.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/JitOptions.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Printf.h"
#include "js/Value.h"
#include "vm/StringType.h"
#include "wasm/WasmJS.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CheckedInt32;
using mozilla::IsPowerOfTwo;

bool FuncType::canHaveJitEntry() const {
  return !hasUnexposableArgOrRet() &&
         !temporarilyUnsupportedReftypeForEntry() &&
         !temporarilyUnsupportedResultCountForJitEntry() &&
         JitOptions.enableWasmJitEntry;
}

bool FuncType::canHaveJitExit() const {
  return !hasUnexposableArgOrRet() && !temporarilyUnsupportedReftypeForExit() &&
         !temporarilyUnsupportedResultCountForJitExit() &&
         JitOptions.enableWasmJitExit;
}

size_t FuncType::serializedSize() const {
  return SerializedPodVectorSize(results_) + SerializedPodVectorSize(args_);
}

uint8_t* FuncType::serialize(uint8_t* cursor) const {
  cursor = SerializePodVector(cursor, results_);
  cursor = SerializePodVector(cursor, args_);
  return cursor;
}

const uint8_t* FuncType::deserialize(const uint8_t* cursor) {
  cursor = DeserializePodVector(cursor, &results_);
  if (!cursor) {
    return nullptr;
  }
  return DeserializePodVector(cursor, &args_);
}

size_t FuncType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return args_.sizeOfExcludingThis(mallocSizeOf);
}

static inline CheckedInt32 RoundUpToAlignment(CheckedInt32 address,
                                              uint32_t align) {
  MOZ_ASSERT(IsPowerOfTwo(align));

  // Note: Be careful to order operators such that we first make the
  // value smaller and then larger, so that we don't get false
  // overflow errors due to (e.g.) adding `align` and then
  // subtracting `1` afterwards when merely adding `align-1` would
  // not have overflowed. Note that due to the nature of two's
  // complement representation, if `address` is already aligned,
  // then adding `align-1` cannot itself cause an overflow.

  return ((address + (align - 1)) / align) * align;
}

class StructLayout {
  CheckedInt32 sizeSoFar = 0;
  uint32_t structAlignment = 1;

 public:
  // The field adders return the offset of the the field.
  CheckedInt32 addField(FieldType type) {
    uint32_t fieldSize = type.size();
    uint32_t fieldAlignment = type.alignmentInStruct();

    // Alignment of the struct is the max of the alignment of its fields.
    structAlignment = std::max(structAlignment, fieldAlignment);

    // Align the pointer.
    CheckedInt32 offset = RoundUpToAlignment(sizeSoFar, fieldAlignment);
    if (!offset.isValid()) {
      return offset;
    }

    // Allocate space.
    sizeSoFar = offset + fieldSize;
    if (!sizeSoFar.isValid()) {
      return sizeSoFar;
    }

    return offset;
  }

  // The close method rounds up the structure size to the appropriate
  // alignment and returns that size.
  CheckedInt32 close() {
    return RoundUpToAlignment(sizeSoFar, structAlignment);
  }
};

bool StructType::computeLayout() {
  StructLayout layout;
  for (StructField& field : fields_) {
    CheckedInt32 offset = layout.addField(field.type);
    if (!offset.isValid()) {
      return false;
    }
    field.offset = offset.value();
  }

  CheckedInt32 size = layout.close();
  if (!size.isValid()) {
    return false;
  }
  size_ = size.value();

  return true;
}

size_t StructType::serializedSize() const {
  return SerializedPodVectorSize(fields_) + sizeof(size_);
}

uint8_t* StructType::serialize(uint8_t* cursor) const {
  cursor = SerializePodVector(cursor, fields_);
  cursor = WriteBytes(cursor, &size_, sizeof(size_));
  return cursor;
}

const uint8_t* StructType::deserialize(const uint8_t* cursor) {
  (cursor = DeserializePodVector(cursor, &fields_)) &&
      (cursor = ReadBytes(cursor, &size_, sizeof(size_)));
  return cursor;
}

size_t StructType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return fields_.sizeOfExcludingThis(mallocSizeOf);
}

size_t TypeDef::serializedSize() const {
  size_t size = sizeof(kind_);
  switch (kind_) {
    case TypeDefKind::Struct: {
      size += structType_.serializedSize();
      break;
    }
    case TypeDefKind::Func: {
      size += funcType_.serializedSize();
      break;
    }
    case TypeDefKind::None: {
      break;
    }
    default:
      MOZ_ASSERT_UNREACHABLE();
  }
  return size;
}

uint8_t* TypeDef::serialize(uint8_t* cursor) const {
  cursor = WriteBytes(cursor, &kind_, sizeof(kind_));
  switch (kind_) {
    case TypeDefKind::Struct: {
      cursor = structType_.serialize(cursor);
      break;
    }
    case TypeDefKind::Func: {
      cursor = funcType_.serialize(cursor);
      break;
    }
    case TypeDefKind::None: {
      break;
    }
    default:
      MOZ_ASSERT_UNREACHABLE();
  }
  return cursor;
}

const uint8_t* TypeDef::deserialize(const uint8_t* cursor) {
  cursor = ReadBytes(cursor, &kind_, sizeof(kind_));
  // kind_ was replaced -- call in-place constructors for union members.
  switch (kind_) {
    case TypeDefKind::Struct: {
      StructType* structType = new (&structType_) StructType();
      cursor = structType->deserialize(cursor);
      break;
    }
    case TypeDefKind::Func: {
      FuncType* funcType = new (&funcType_) FuncType();
      cursor = funcType->deserialize(cursor);
      break;
    }
    case TypeDefKind::None: {
      break;
    }
    default:
      MOZ_ASSERT_UNREACHABLE();
  }
  return cursor;
}

size_t TypeDef::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  switch (kind_) {
    case TypeDefKind::Struct: {
      return structType_.sizeOfExcludingThis(mallocSizeOf);
    }
    case TypeDefKind::Func: {
      return funcType_.sizeOfExcludingThis(mallocSizeOf);
    }
    case TypeDefKind::None: {
      return 0;
    }
    default:
      break;
  }
  MOZ_ASSERT_UNREACHABLE();
  return 0;
}

TypeResult TypeContext::isRefEquivalent(RefType one, RefType two,
                                        TypeCache* cache) const {
  // Anything's equal to itself.
  if (one == two) {
    return TypeResult::True;
  }

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
  if (features_.functionReferences) {
    // Two references must have the same nullability to be equal
    if (one.isNullable() != two.isNullable()) {
      return TypeResult::False;
    }

    // Non type-index references are equal if they have the same kind
    if (!one.isTypeIndex() && !two.isTypeIndex() && one.kind() == two.kind()) {
      return TypeResult::True;
    }

    // Type-index references can be equal
    if (one.isTypeIndex() && two.isTypeIndex()) {
      return isTypeIndexEquivalent(one.typeIndex(), two.typeIndex(), cache);
    }
  }
#endif
  return TypeResult::False;
}

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
TypeResult TypeContext::isTypeIndexEquivalent(uint32_t one, uint32_t two,
                                              TypeCache* cache) const {
  MOZ_ASSERT(features_.functionReferences);

  // Anything's equal to itself.
  if (one == two) {
    return TypeResult::True;
  }

#  ifdef ENABLE_WASM_GC
  if (features_.gc) {
    // A struct may be equal to a struct
    if (isStructType(one) && isStructType(two)) {
      return isStructEquivalent(one, two, cache);
    }

    // An array may be equal to an array
    if (isArrayType(one) && isArrayType(two)) {
      return isArrayEquivalent(one, two, cache);
    }
  }
#  endif

  return TypeResult::False;
}
#endif

#ifdef ENABLE_WASM_GC
TypeResult TypeContext::isStructEquivalent(uint32_t oneIndex, uint32_t twoIndex,
                                           TypeCache* cache) const {
  if (cache->isEquivalent(oneIndex, twoIndex)) {
    return TypeResult::True;
  }

  const StructType& one = structType(oneIndex);
  const StructType& two = structType(twoIndex);

  // Structs must have the same number of fields to be equal
  if (one.fields_.length() != two.fields_.length()) {
    return TypeResult::False;
  }

  // Assume these structs are equal while checking fields. If any field is
  // not equal then we remove the assumption.
  if (!cache->markEquivalent(oneIndex, twoIndex)) {
    return TypeResult::OOM;
  }

  for (uint32_t i = 0; i < two.fields_.length(); i++) {
    TypeResult result =
        isStructFieldEquivalent(one.fields_[i], two.fields_[i], cache);
    if (result != TypeResult::True) {
      cache->unmarkEquivalent(oneIndex, twoIndex);
      return result;
    }
  }
  return TypeResult::True;
}

TypeResult TypeContext::isStructFieldEquivalent(const StructField one,
                                                const StructField two,
                                                TypeCache* cache) const {
  // Struct fields must share the same mutability to equal
  if (one.isMutable != two.isMutable) {
    return TypeResult::False;
  }
  // Struct field types must be equal
  return isEquivalent(one.type, two.type, cache);
}

TypeResult TypeContext::isArrayEquivalent(uint32_t oneIndex, uint32_t twoIndex,
                                          TypeCache* cache) const {
  if (cache->isEquivalent(oneIndex, twoIndex)) {
    return TypeResult::True;
  }

  const ArrayType& one = arrayType(oneIndex);
  const ArrayType& two = arrayType(twoIndex);

  // Assume these arrays are equal while checking fields. If the array
  // element is not equal then we remove the assumption.
  if (!cache->markEquivalent(oneIndex, twoIndex)) {
    return TypeResult::OOM;
  }

  TypeResult result = isArrayElementEquivalent(one, two, cache);
  if (result != TypeResult::True) {
    cache->unmarkEquivalent(oneIndex, twoIndex);
  }
  return result;
}

TypeResult TypeContext::isArrayElementEquivalent(const ArrayType& one,
                                                 const ArrayType& two,
                                                 TypeCache* cache) const {
  // Array elements must share the same mutability to be equal
  if (one.isMutable_ != two.isMutable_) {
    return TypeResult::False;
  }
  // Array elements must be equal
  return isEquivalent(one.elementType_, two.elementType_, cache);
}
#endif

TypeResult TypeContext::isRefSubtypeOf(RefType one, RefType two,
                                       TypeCache* cache) const {
  // Anything's a subtype of itself.
  if (one == two) {
    return TypeResult::True;
  }

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
  if (features_.functionReferences) {
    // A subtype must have the same nullability as the supertype or the
    // supertype must be nullable.
    if (!(one.isNullable() == two.isNullable() || two.isNullable())) {
      return TypeResult::False;
    }

    // Non type-index references are subtypes if they have the same kind
    if (!one.isTypeIndex() && !two.isTypeIndex() && one.kind() == two.kind()) {
      return TypeResult::True;
    }

    // Structs are subtypes of eqref
    if (isStructType(one) && two.isEq()) {
      return TypeResult::True;
    }

    // Arrays are subtypes of eqref
    if (isArrayType(one) && two.isEq()) {
      return TypeResult::True;
    }

    // Type-index references can be subtypes
    if (one.isTypeIndex() && two.isTypeIndex()) {
      return isTypeIndexSubtypeOf(one.typeIndex(), two.typeIndex(), cache);
    }
  }
#endif
  return TypeResult::False;
}

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
TypeResult TypeContext::isTypeIndexSubtypeOf(uint32_t one, uint32_t two,
                                             TypeCache* cache) const {
  MOZ_ASSERT(features_.functionReferences);

  // Anything's a subtype of itself.
  if (one == two) {
    return TypeResult::True;
  }

#  ifdef ENABLE_WASM_GC
  if (features_.gc) {
    // Structs may be subtypes of structs
    if (isStructType(one) && isStructType(two)) {
      return isStructSubtypeOf(one, two, cache);
    }

    // Arrays may be subtypes of arrays
    if (isArrayType(one) && isArrayType(two)) {
      return isArraySubtypeOf(one, two, cache);
    }
  }
#  endif
  return TypeResult::False;
}
#endif

#ifdef ENABLE_WASM_GC
TypeResult TypeContext::isStructSubtypeOf(uint32_t oneIndex, uint32_t twoIndex,
                                          TypeCache* cache) const {
  if (cache->isSubtypeOf(oneIndex, twoIndex)) {
    return TypeResult::True;
  }

  const StructType& one = structType(oneIndex);
  const StructType& two = structType(twoIndex);

  // A subtype must have at least as many fields as its supertype
  if (one.fields_.length() < two.fields_.length()) {
    return TypeResult::False;
  }

  // Assume these structs are subtypes while checking fields. If any field
  // fails a check then we remove the assumption.
  if (!cache->markSubtypeOf(oneIndex, twoIndex)) {
    return TypeResult::OOM;
  }

  for (uint32_t i = 0; i < two.fields_.length(); i++) {
    TypeResult result =
        isStructFieldSubtypeOf(one.fields_[i], two.fields_[i], cache);
    if (result != TypeResult::True) {
      cache->unmarkSubtypeOf(oneIndex, twoIndex);
      return result;
    }
  }
  return TypeResult::True;
}

TypeResult TypeContext::isStructFieldSubtypeOf(const StructField one,
                                               const StructField two,
                                               TypeCache* cache) const {
  // Mutable fields are invariant w.r.t. field types
  if (one.isMutable && two.isMutable) {
    return isEquivalent(one.type, two.type, cache);
  }
  // Immutable fields are covariant w.r.t. field types
  if (!one.isMutable && !two.isMutable) {
    return isSubtypeOf(one.type, two.type, cache);
  }
  return TypeResult::False;
}

TypeResult TypeContext::isArraySubtypeOf(uint32_t oneIndex, uint32_t twoIndex,
                                         TypeCache* cache) const {
  if (cache->isSubtypeOf(oneIndex, twoIndex)) {
    return TypeResult::True;
  }

  const ArrayType& one = arrayType(oneIndex);
  const ArrayType& two = arrayType(twoIndex);

  // Assume these arrays are subtypes while checking elements. If the elements
  // fail the check then we remove the assumption.
  if (!cache->markSubtypeOf(oneIndex, twoIndex)) {
    return TypeResult::OOM;
  }

  TypeResult result = isArrayElementSubtypeOf(one, two, cache);
  if (result != TypeResult::True) {
    cache->unmarkSubtypeOf(oneIndex, twoIndex);
  }
  return result;
}

TypeResult TypeContext::isArrayElementSubtypeOf(const ArrayType& one,
                                                const ArrayType& two,
                                                TypeCache* cache) const {
  // Mutable elements are invariant w.r.t. field types
  if (one.isMutable_ && two.isMutable_) {
    return isEquivalent(one.elementType_, two.elementType_, cache);
  }
  // Immutable elements are covariant w.r.t. field types
  if (!one.isMutable_ && !two.isMutable_) {
    return isSubtypeOf(one.elementType_, two.elementType_, cache);
  }
  return TypeResult::False;
}
#endif
