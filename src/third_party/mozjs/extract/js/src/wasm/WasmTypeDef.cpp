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

#include "mozilla/MathAlgorithms.h"

#include "jit/JitOptions.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/HashTable.h"
#include "js/Printf.h"
#include "js/Value.h"
#include "threading/ExclusiveData.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"
#include "wasm/WasmCodegenConstants.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmJS.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CheckedInt32;
using mozilla::CheckedUint32;
using mozilla::IsPowerOfTwo;
using mozilla::MallocSizeOf;

// [SMDOC] Immediate type signature encoding
//
// call_indirect requires a signature check to ensure the dynamic callee type
// matches the static specified callee type. This involves comparing whether
// the two function types are equal. We canonicalize function types so that
// comparing the pointers of the types will indicate if they're equal. The
// canonicalized function types are loaded from the instance at runtime.
//
// For the common case of simple/small function types, we can avoid the cost
// of loading the function type pointers from the instance by having an
// alternate 'immediate' form that encodes a function type in a constant.
// We encode the function types such that bitwise equality implies the original
// function types were equal. We use a tag bit such that if one of the types
// is a pointer and the other an immediate, they will not compare as equal.
//
// The encoding is optimized for common function types that have at most one
// result and an arbitrary amount of arguments.
//
// [
//   1 bit : tag (always 1),
//   1 bit : numResults,
//   3 bits : numArgs,
//   numResults * 3 bits : results,
//   numArgs * 3 bits : args
// ]
// (lsb -> msb order)
//
// Any function type that cannot be encoded in the above format is falls back
// to the pointer representation.
//

//=========================================================================
// ImmediateType

// ImmediateType is 32-bits to ensure it's easy to materialize the constant
// on all platforms.
using ImmediateType = uint32_t;
static const unsigned sTotalBits = sizeof(ImmediateType) * 8;
static const unsigned sTagBits = 1;
static const unsigned sNumResultsBits = 1;
static const unsigned sNumArgsBits = 3;
static const unsigned sValTypeBits = 3;
static const unsigned sMaxValTypes = 8;

static_assert(((1 << sNumResultsBits) - 1) + ((1 << sNumArgsBits) - 1) ==
                  sMaxValTypes,
              "sNumResultsBits, sNumArgsBits, sMaxValTypes are consistent");

static_assert(sTagBits + sNumResultsBits + sNumArgsBits +
                      sValTypeBits * sMaxValTypes <=
                  sTotalBits,
              "have room");

static bool IsImmediateValType(ValType vt) {
  switch (vt.kind()) {
    case ValType::I32:
    case ValType::I64:
    case ValType::F32:
    case ValType::F64:
    case ValType::V128:
      return true;
    case ValType::Ref:
      // We don't have space to encode nullability, so we optimize for
      // non-nullable types.
      if (!vt.isNullable()) {
        return false;
      }
      switch (vt.refType().kind()) {
        case RefType::Func:
        case RefType::Extern:
        case RefType::Any:
          return true;
        default:
          return false;
      }
    default:
      return false;
  }
}

static unsigned EncodeImmediateValType(ValType vt) {
  // We have run out of bits for each type, anything new must increase the
  // sValTypeBits.
  static_assert(7 < (1 << sValTypeBits), "enough space for ValType kind");

  switch (vt.kind()) {
    case ValType::I32:
      return 0;
    case ValType::I64:
      return 1;
    case ValType::F32:
      return 2;
    case ValType::F64:
      return 3;
    case ValType::V128:
      return 4;
    case ValType::Ref:
      MOZ_ASSERT(vt.isNullable());
      switch (vt.refType().kind()) {
        case RefType::Func:
          return 5;
        case RefType::Extern:
          return 6;
        case RefType::Any:
          return 7;
        default:
          MOZ_CRASH("bad RefType");
      }
    default:
      MOZ_CRASH("bad ValType");
  }
}

static bool IsImmediateFuncType(const FuncType& funcType) {
  const ValTypeVector& results = funcType.results();
  const ValTypeVector& args = funcType.args();

  // Check the number of results and args fits
  if (results.length() > ((1 << sNumResultsBits) - 1) ||
      args.length() > ((1 << sNumArgsBits) - 1)) {
    return false;
  }

  // Ensure every result is compatible
  for (ValType v : results) {
    if (!IsImmediateValType(v)) {
      return false;
    }
  }

  // Ensure every arg is compatible
  for (ValType v : args) {
    if (!IsImmediateValType(v)) {
      return false;
    }
  }

  return true;
}

static ImmediateType EncodeNumResults(uint32_t numResults) {
  MOZ_ASSERT(numResults <= (1 << sNumResultsBits) - 1);
  return numResults;
}

static ImmediateType EncodeNumArgs(uint32_t numArgs) {
  MOZ_ASSERT(numArgs <= (1 << sNumArgsBits) - 1);
  return numArgs;
}

static ImmediateType EncodeImmediateFuncType(const FuncType& funcType) {
  ImmediateType immediate = FuncType::ImmediateBit;
  uint32_t shift = sTagBits;

  // Encode the results
  immediate |= EncodeNumResults(funcType.results().length()) << shift;
  shift += sNumResultsBits;

  for (ValType resultType : funcType.results()) {
    immediate |= EncodeImmediateValType(resultType) << shift;
    shift += sValTypeBits;
  }

  // Encode the args
  immediate |= EncodeNumArgs(funcType.args().length()) << shift;
  shift += sNumArgsBits;

  for (ValType argType : funcType.args()) {
    immediate |= EncodeImmediateValType(argType) << shift;
    shift += sValTypeBits;
  }

  MOZ_ASSERT(shift <= sTotalBits);
  return immediate;
}

//=========================================================================
// FuncType

void FuncType::initImmediateTypeId(bool isFinal, const TypeDef* superTypeDef,
                                   uint32_t recGroupLength) {
  // To improve the performance of the structural type check in
  // the call_indirect function prologue, we attempt to encode the
  // entire function type into an immediate such that bitwise equality
  // implies structural equality. With the GC proposal, we don't
  // want to generalize the immediate form for the new type system, so
  // we don't use it when a type is non-final (i.e. may have sub types), or
  // has super types, or is in a recursion group with other types.
  //
  // If non-final types are allowed, then the type can have subtypes, and we
  // should therefore do a full subtype check on call_indirect, which
  // doesn't work well with immediates. If the type has a super type, the
  // same reason applies. And finally, types in recursion groups of
  // size > 1 may not be considered equivalent even if they are
  // structurally equivalent in every respect.
  if (!isFinal || superTypeDef || recGroupLength != 1) {
    immediateTypeId_ = NO_IMMEDIATE_TYPE_ID;
    return;
  }

  // Otherwise, try to encode this function type into an immediate.
  if (!IsImmediateFuncType(*this)) {
    immediateTypeId_ = NO_IMMEDIATE_TYPE_ID;
    return;
  }
  immediateTypeId_ = EncodeImmediateFuncType(*this);
}

bool FuncType::canHaveJitEntry() const {
  return !hasUnexposableArgOrRet() &&
         !temporarilyUnsupportedReftypeForEntry() &&
         !temporarilyUnsupportedResultCountForJitEntry() &&
         JitOptions.enableWasmJitEntry;
}

bool FuncType::canHaveJitExit() const {
  return !hasUnexposableArgOrRet() && !temporarilyUnsupportedReftypeForExit() &&
         !hasInt64Arg() && !temporarilyUnsupportedResultCountForJitExit() &&
         JitOptions.enableWasmJitExit;
}

size_t FuncType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return args_.sizeOfExcludingThis(mallocSizeOf);
}

//=========================================================================
// StructType and StructLayout

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

CheckedInt32 StructLayout::addField(StorageType type) {
  uint32_t fieldSize = type.size();
  uint32_t fieldAlignment = type.alignmentInStruct();

  // We have to ensure that `offset` is chosen so that no field crosses the
  // inline/outline boundary.  The assertions here ensure that.  See comment
  // on `class StructLayout` for background.
  MOZ_ASSERT(fieldSize >= 1 && fieldSize <= 16);
  MOZ_ASSERT((fieldSize & (fieldSize - 1)) == 0);  // is a power of 2
  MOZ_ASSERT(fieldAlignment == fieldSize);         // is naturally aligned

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

  // The following should hold if the three assertions above hold.
  MOZ_ASSERT(offset / 16 == (offset + fieldSize - 1) / 16);
  return offset;
}

CheckedInt32 StructLayout::close() {
  CheckedInt32 size = RoundUpToAlignment(sizeSoFar, structAlignment);
  // What we are computing into `size` is the size of
  // WasmGcObject::inlineData_, or the size of the outline data area.  Either
  // way, it is helpful if the area size is an integral number of machine
  // words, since this would make any initialisation loop for inline
  // allocation able to operate on machine word sized units, should we decide
  // to do inline allocation.
  if (structAlignment < sizeof(uintptr_t)) {
    size = RoundUpToAlignment(size, sizeof(uintptr_t));
  }
  return size;
}

bool StructType::init() {
  bool isDefaultable = true;

  StructLayout layout;
  for (FieldType& field : fields_) {
    CheckedInt32 offset = layout.addField(field.type);
    if (!offset.isValid()) {
      return false;
    }

    // Add the offset to the list
    if (!fieldOffsets_.append(offset.value())) {
      return false;
    }

    // If any field is not defaultable, this whole struct is not defaultable
    if (!field.type.isDefaultable()) {
      isDefaultable = false;
    }

    // If this field is not a ref, then don't add it to the trace lists
    if (!field.type.isRefRepr()) {
      continue;
    }

    bool isOutline;
    uint32_t adjustedOffset;
    WasmStructObject::fieldOffsetToAreaAndOffset(field.type, offset.value(),
                                                 &isOutline, &adjustedOffset);
    if (isOutline) {
      if (!outlineTraceOffsets_.append(adjustedOffset)) {
        return false;
      }
    } else {
      if (!inlineTraceOffsets_.append(adjustedOffset)) {
        return false;
      }
    }
  }

  CheckedInt32 size = layout.close();
  if (!size.isValid()) {
    return false;
  }

  size_ = size.value();
  isDefaultable_ = isDefaultable;
  return true;
}

/* static */
bool StructType::createImmutable(const ValTypeVector& types,
                                 StructType* struct_) {
  FieldTypeVector fields;
  if (!fields.resize(types.length())) {
    return false;
  }
  for (size_t i = 0; i < types.length(); i++) {
    fields[i].type = StorageType(types[i].packed());
    fields[i].isMutable = false;
  }
  *struct_ = StructType(std::move(fields));
  return struct_->init();
}

size_t StructType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return fields_.sizeOfExcludingThis(mallocSizeOf);
}

size_t ArrayType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return 0;
}

size_t TypeDef::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  switch (kind_) {
    case TypeDefKind::Struct: {
      return structType_.sizeOfExcludingThis(mallocSizeOf);
    }
    case TypeDefKind::Func: {
      return funcType_.sizeOfExcludingThis(mallocSizeOf);
    }
    case TypeDefKind::Array: {
      return arrayType_.sizeOfExcludingThis(mallocSizeOf);
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

//=========================================================================
// SuperTypeVector

/* static */
size_t SuperTypeVector::offsetOfSTVInVector(uint32_t subTypingDepth) {
  return offsetof(SuperTypeVector, types_) + sizeof(void*) * subTypingDepth;
}

/* static */
size_t SuperTypeVector::lengthForTypeDef(const TypeDef& typeDef) {
  return std::max(uint32_t(typeDef.subTypingDepth()) + 1,
                  MinSuperTypeVectorLength);
}

/* static */
size_t SuperTypeVector::byteSizeForTypeDef(const TypeDef& typeDef) {
  static_assert(
      sizeof(SuperTypeVector) + sizeof(void*) * (MaxSubTypingDepth + 1) <=
          UINT16_MAX,
      "cannot overflow");
  return sizeof(SuperTypeVector) + (sizeof(void*) * lengthForTypeDef(typeDef));
}

/* static */
const SuperTypeVector* SuperTypeVector::createMultipleForRecGroup(
    RecGroup* recGroup) {
  // Pre-size the amount of space needed for all the super type vectors in this
  // recursion group.
  CheckedUint32 totalBytes = 0;
  for (uint32_t typeIndex = 0; typeIndex < recGroup->numTypes(); typeIndex++) {
    totalBytes +=
        SuperTypeVector::byteSizeForTypeDef(recGroup->type(typeIndex));
  }
  if (!totalBytes.isValid()) {
    return nullptr;
  }

  // Allocate the batch, and retain reference to the first one.
  SuperTypeVector* firstVector =
      (SuperTypeVector*)js_malloc(totalBytes.value());
  if (!firstVector) {
    return nullptr;
  }

  // Initialize the vectors, one by one
  SuperTypeVector* currentVector = firstVector;
  for (uint32_t typeIndex = 0; typeIndex < recGroup->numTypes(); typeIndex++) {
    TypeDef& typeDef = recGroup->type(typeIndex);

    // Compute the size again to know where the next vector can be found.
    size_t vectorByteSize = SuperTypeVector::byteSizeForTypeDef(typeDef);

    // Make the typedef and the vector point at each other.
    typeDef.setSuperTypeVector(currentVector);
    currentVector->typeDef_ = &typeDef;
    currentVector->subTypingDepth_ = typeDef.subTypingDepth();

    // Every vector stores all ancestor types and itself.
    currentVector->length_ = SuperTypeVector::lengthForTypeDef(typeDef);

    // Initialize the entries in the vector
    const TypeDef* currentTypeDef = &typeDef;
    for (uint32_t index = 0; index < currentVector->length(); index++) {
      uint32_t reverseIndex = currentVector->length() - index - 1;

      // If this entry is required just to hit the minimum size, then
      // initialize it to null.
      if (reverseIndex > typeDef.subTypingDepth()) {
        currentVector->types_[reverseIndex] = nullptr;
        continue;
      }

      // Otherwise we should always be iterating at the same depth as our
      // currentTypeDef.
      MOZ_ASSERT(reverseIndex == currentTypeDef->subTypingDepth());

      currentVector->types_[reverseIndex] = currentTypeDef->superTypeVector();
      currentTypeDef = currentTypeDef->superTypeDef();
    }

    // There should be no more super types left over
    MOZ_ASSERT(currentTypeDef == nullptr);

    // Advance to the next super type vector
    currentVector =
        (SuperTypeVector*)(((const char*)currentVector) + vectorByteSize);
  }

  return firstVector;
}

//=========================================================================
// TypeIdSet and TypeContext

struct RecGroupHashPolicy {
  using Lookup = const SharedRecGroup&;

  static HashNumber hash(Lookup lookup) { return lookup->hash(); }

  static bool match(const SharedRecGroup& lhs, Lookup rhs) {
    return RecGroup::isoEquals(*rhs, *lhs);
  }
};

// A global hash set of recursion groups for use in fast type equality checks.
class TypeIdSet {
  using Set = HashSet<SharedRecGroup, RecGroupHashPolicy, SystemAllocPolicy>;
  Set set_;

 public:
  // Attempt to insert a recursion group into the set, returning an existing
  // recursion group if there was one.
  SharedRecGroup insert(SharedRecGroup recGroup) {
    Set::AddPtr p = set_.lookupForAdd(recGroup);
    if (p) {
      // A canonical recursion group already existed, return it.
      return *p;
    }

    // Insert this recursion group into the set, and return it as the canonical
    // recursion group instance.
    if (!set_.add(p, recGroup)) {
      return nullptr;
    }
    return recGroup;
  }

  void purge() {
    // TODO: this is not guaranteed to remove all types that are not referenced
    // from outside the canonical set, as removing a type may make a previous
    // type we've visited now only have one ref and be eligible to be freed.
    //
    // Solving this either involves iterating to a fixed point, or else a much
    // more invasive change to the lifetime management of recursion groups.
    for (auto iter = set_.modIter(); !iter.done(); iter.next()) {
      if (iter.get()->hasOneRef()) {
        iter.remove();
      }
    }
  }

  // Release the provided recursion group reference and remove it from the
  // canonical set if it was the last reference. This is one unified method
  // because we need to perform the lookup before releasing the reference, but
  // need to release the reference in order to see if it was the last reference
  // outside the canonical set.
  void clearRecGroup(SharedRecGroup* recGroupCell) {
    if (Set::Ptr p = set_.lookup(*recGroupCell)) {
      *recGroupCell = nullptr;
      if ((*p)->hasOneRef()) {
        set_.remove(p);
      }
    } else {
      *recGroupCell = nullptr;
    }
  }
};

MOZ_RUNINIT ExclusiveData<TypeIdSet> typeIdSet(mutexid::WasmTypeIdSet);

void wasm::PurgeCanonicalTypes() {
  ExclusiveData<TypeIdSet>::Guard locked = typeIdSet.lock();
  locked->purge();
}

SharedRecGroup TypeContext::canonicalizeGroup(SharedRecGroup recGroup) {
  ExclusiveData<TypeIdSet>::Guard locked = typeIdSet.lock();
  return locked->insert(recGroup);
}

TypeContext::~TypeContext() {
  ExclusiveData<TypeIdSet>::Guard locked = typeIdSet.lock();

  // Clear out the recursion groups in this module, freeing them from the
  // canonical type set if needed.
  //
  // We iterate backwards here so that we free every previous recursion group
  // that may be referring to the current recursion group we're freeing. This
  // is possible due to recursion groups being ordered.
  for (int32_t groupIndex = recGroups_.length() - 1; groupIndex >= 0;
       groupIndex--) {
    // Try to remove this entry from the canonical set if we have the last
    // strong reference. The entry may not exist if canonicalization failed
    // and this type context was aborted. This will clear the reference in the
    // vector.
    locked->clearRecGroup(&recGroups_[groupIndex]);
  }
}
