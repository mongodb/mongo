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

#ifndef wasm_types_h
#define wasm_types_h

#include "mozilla/Alignment.h"
#include "mozilla/Atomics.h"
#include "mozilla/BinarySearch.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Maybe.h"
#include "mozilla/RefPtr.h"

#include <type_traits>

#include "NamespaceImports.h"

#include "ds/LifoAlloc.h"
#include "jit/IonTypes.h"
#include "js/RefCounted.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "vm/MallocProvider.h"
#include "vm/NativeObject.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmInitExpr.h"
#include "wasm/WasmPages.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmShareable.h"
#include "wasm/WasmTlsData.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmUtility.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmValue.h"

namespace js {

namespace jit {
enum class RoundingMode;
template <class VecT, class ABIArgGeneratorT>
class ABIArgIterBase;
}  // namespace jit

namespace wasm {

using mozilla::Atomic;
using mozilla::DebugOnly;
using mozilla::EnumeratedArray;
using mozilla::MallocSizeOf;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::PodCopy;
using mozilla::PodZero;
using mozilla::Some;

class Code;
class DebugState;
class GeneratedSourceMap;
class Memory;
class Module;
class Instance;
class Table;

// Exception tags are used to uniquely identify exceptions. They are stored
// in a vector in Instances and used by both WebAssembly.Exception for import
// and export, and by the representation of thrown exceptions.
//
// Since an exception tag is a (trivial) substructure of AtomicRefCounted, the
// RefPtr SharedExceptionTag can have many instances/modules referencing a
// single constant exception tag.

struct ExceptionTag : AtomicRefCounted<ExceptionTag> {
  ExceptionTag() = default;
};
using SharedExceptionTag = RefPtr<ExceptionTag>;
using SharedExceptionTagVector =
    Vector<SharedExceptionTag, 0, SystemAllocPolicy>;

// WasmJSExceptionObject wraps a JS Value in order to provide a uniform
// method of handling JS thrown exceptions. Exceptions originating in Wasm
// are WebAssemby.RuntimeException objects, whereas exceptions from JS are
// wrapped as WasmJSExceptionObject objects.
class WasmJSExceptionObject : public NativeObject {
  static const unsigned VALUE_SLOT = 0;

 public:
  static const unsigned RESERVED_SLOTS = 1;
  static const JSClass class_;
  const Value& value() const { return getFixedSlot(VALUE_SLOT); }

  static WasmJSExceptionObject* create(JSContext* cx, MutableHandleValue value);
};

// A Module can either be asm.js or wasm.

enum ModuleKind { Wasm, AsmJS };

// ArgTypeVector type.
//
// Functions usually receive one ABI argument per WebAssembly argument.  However
// if a function has multiple results and some of those results go to the stack,
// then it additionally receives a synthetic ABI argument holding a pointer to
// the stack result area.
//
// Given the presence of synthetic arguments, sometimes we need a name for
// non-synthetic arguments.  We call those "natural" arguments.

enum class StackResults { HasStackResults, NoStackResults };

class ArgTypeVector {
  const ValTypeVector& args_;
  bool hasStackResults_;

  // To allow ABIArgIterBase<VecT, ABIArgGeneratorT>, we define a private
  // length() method.  To prevent accidental errors, other users need to be
  // explicit and call lengthWithStackResults() or
  // lengthWithoutStackResults().
  size_t length() const { return args_.length() + size_t(hasStackResults_); }
  template <class VecT, class ABIArgGeneratorT>
  friend class jit::ABIArgIterBase;

 public:
  ArgTypeVector(const ValTypeVector& args, StackResults stackResults)
      : args_(args),
        hasStackResults_(stackResults == StackResults::HasStackResults) {}
  explicit ArgTypeVector(const FuncType& funcType);

  bool hasSyntheticStackResultPointerArg() const { return hasStackResults_; }
  StackResults stackResults() const {
    return hasSyntheticStackResultPointerArg() ? StackResults::HasStackResults
                                               : StackResults::NoStackResults;
  }
  size_t lengthWithoutStackResults() const { return args_.length(); }
  bool isSyntheticStackResultPointerArg(size_t idx) const {
    // The pointer to stack results area, if present, is a synthetic argument
    // tacked on at the end.
    MOZ_ASSERT(idx < lengthWithStackResults());
    return idx == args_.length();
  }
  bool isNaturalArg(size_t idx) const {
    return !isSyntheticStackResultPointerArg(idx);
  }
  size_t naturalIndex(size_t idx) const {
    MOZ_ASSERT(isNaturalArg(idx));
    // Because the synthetic argument, if present, is tacked on the end, an
    // argument index that isn't synthetic is natural.
    return idx;
  }

  size_t lengthWithStackResults() const { return length(); }
  jit::MIRType operator[](size_t i) const {
    MOZ_ASSERT(i < lengthWithStackResults());
    if (isSyntheticStackResultPointerArg(i)) {
      return jit::MIRType::StackResults;
    }
    return ToMIRType(args_[naturalIndex(i)]);
  }
};

template <typename PointerType>
class TaggedValue {
 public:
  enum Kind {
    ImmediateKind1 = 0,
    ImmediateKind2 = 1,
    PointerKind1 = 2,
    PointerKind2 = 3
  };
  using PackedRepr = uintptr_t;

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

static_assert(
    std::is_same<TaggedValue<void*>::PackedRepr, PackedTypeCode::PackedRepr>(),
    "can use pointer tagging with PackedTypeCode");

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

  ResultType(Kind kind, uint32_t imm) : tagged_(Tagged::Kind(kind), imm) {}
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

  static ResultType Empty() { return ResultType(EmptyKind, uint32_t(0)); }
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

  bool operator==(ResultType rhs) const {
    switch (kind()) {
      case EmptyKind:
      case SingleKind:
      case InvalidKind:
        return tagged_.bits() == rhs.tagged_.bits();
      case VectorKind: {
        if (rhs.kind() != VectorKind) {
          return false;
        }
        return EqualContainers(values(), rhs.values());
      }
      default:
        MOZ_CRASH("bad resulttype");
    }
  }
  bool operator!=(ResultType rhs) const { return !(*this == rhs); }
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

  BlockType(Kind kind, uint32_t imm) : tagged_(Tagged::Kind(kind), imm) {}
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
    return BlockType(VoidToVoidKind, uint32_t(0));
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
        return funcType() == rhs.funcType();
      case FuncResultsKind:
        return EqualContainers(funcType().results(), rhs.funcType().results());
      default:
        MOZ_CRASH("unexpected kind");
    }
  }

  bool operator!=(BlockType rhs) const { return !(*this == rhs); }
};

// CacheableChars is used to cacheably store UniqueChars.

struct CacheableChars : UniqueChars {
  CacheableChars() = default;
  explicit CacheableChars(char* ptr) : UniqueChars(ptr) {}
  MOZ_IMPLICIT CacheableChars(UniqueChars&& rhs)
      : UniqueChars(std::move(rhs)) {}
  WASM_DECLARE_SERIALIZABLE(CacheableChars)
};

using CacheableCharsVector = Vector<CacheableChars, 0, SystemAllocPolicy>;

// Import describes a single wasm import. An ImportVector describes all
// of a single module's imports.
//
// ImportVector is built incrementally by ModuleGenerator and then stored
// immutably by Module.

struct Import {
  CacheableChars module;
  CacheableChars field;
  DefinitionKind kind;

  Import() = default;
  Import(UniqueChars&& module, UniqueChars&& field, DefinitionKind kind)
      : module(std::move(module)), field(std::move(field)), kind(kind) {}

  WASM_DECLARE_SERIALIZABLE(Import)
};

using ImportVector = Vector<Import, 0, SystemAllocPolicy>;

// Export describes the export of a definition in a Module to a field in the
// export object. The Export stores the index of the exported item in the
// appropriate type-specific module data structure (function table, global
// table, table table, and - eventually - memory table).
//
// Note a single definition can be exported by multiple Exports in the
// ExportVector.
//
// ExportVector is built incrementally by ModuleGenerator and then stored
// immutably by Module.

class Export {
  CacheableChars fieldName_;
  struct CacheablePod {
    DefinitionKind kind_;
    uint32_t index_;
  } pod;

 public:
  Export() = default;
  explicit Export(UniqueChars fieldName, uint32_t index, DefinitionKind kind);
  explicit Export(UniqueChars fieldName, DefinitionKind kind);

  const char* fieldName() const { return fieldName_.get(); }

  DefinitionKind kind() const { return pod.kind_; }
  uint32_t funcIndex() const;
#ifdef ENABLE_WASM_EXCEPTIONS
  uint32_t eventIndex() const;
#endif
  uint32_t globalIndex() const;
  uint32_t tableIndex() const;

  WASM_DECLARE_SERIALIZABLE(Export)
};

using ExportVector = Vector<Export, 0, SystemAllocPolicy>;

// FuncFlags provides metadata for a function definition.

enum class FuncFlags : uint8_t {
  None = 0x0,
  // The function maybe be accessible by JS and needs thunks generated for it.
  // See `[SMDOC] Exported wasm functions and the jit-entry stubs` in
  // WasmJS.cpp for more information.
  Exported = 0x1,
  // The function should have thunks generated upon instantiation, not upon
  // first call. May only be set if `Exported` is set.
  Eager = 0x2,
  // The function can be the target of a ref.func instruction in the code
  // section. May only be set if `Exported` is set.
  CanRefFunc = 0x4,
};

// A FuncDesc describes a single function definition.

class TypeIdDesc;

struct FuncDesc {
  FuncType* type;
  TypeIdDesc* typeId;
  // Bit pack to keep this struct small on 32-bit systems
  uint32_t typeIndex : 24;
  FuncFlags flags : 8;

  // Assert that the bit packing scheme is viable
  static_assert(MaxTypes <= (1 << 24) - 1);
  static_assert(sizeof(FuncFlags) == sizeof(uint8_t));

  FuncDesc() = default;
  FuncDesc(FuncType* type, TypeIdDesc* typeId, uint32_t typeIndex)
      : type(type),
        typeId(typeId),
        typeIndex(typeIndex),
        flags(FuncFlags::None) {}

  bool isExported() const {
    return uint8_t(flags) & uint8_t(FuncFlags::Exported);
  }
  bool isEager() const { return uint8_t(flags) & uint8_t(FuncFlags::Eager); }
  bool canRefFunc() const {
    return uint8_t(flags) & uint8_t(FuncFlags::CanRefFunc);
  }
};

using FuncDescVector = Vector<FuncDesc, 0, SystemAllocPolicy>;

// A GlobalDesc describes a single global variable.
//
// wasm can import and export mutable and immutable globals.
//
// asm.js can import mutable and immutable globals, but a mutable global has a
// location that is private to the module, and its initial value is copied into
// that cell from the environment.  asm.js cannot export globals.

enum class GlobalKind { Import, Constant, Variable };

class GlobalDesc {
  GlobalKind kind_;
  // Stores the value type of this global for all kinds, and the initializer
  // expression when `constant` or `variable`.
  InitExpr initial_;
  // Metadata for the global when `variable` or `import`.
  unsigned offset_;
  bool isMutable_;
  bool isWasm_;
  bool isExport_;
  // Metadata for the global when `import`.
  uint32_t importIndex_;

  // Private, as they have unusual semantics.

  bool isExport() const { return !isConstant() && isExport_; }
  bool isWasm() const { return !isConstant() && isWasm_; }

 public:
  GlobalDesc() = default;

  explicit GlobalDesc(InitExpr&& initial, bool isMutable,
                      ModuleKind kind = ModuleKind::Wasm)
      : kind_((isMutable || !initial.isLiteral()) ? GlobalKind::Variable
                                                  : GlobalKind::Constant) {
    initial_ = std::move(initial);
    if (isVariable()) {
      isMutable_ = isMutable;
      isWasm_ = kind == Wasm;
      isExport_ = false;
      offset_ = UINT32_MAX;
    }
  }

  explicit GlobalDesc(ValType type, bool isMutable, uint32_t importIndex,
                      ModuleKind kind = ModuleKind::Wasm)
      : kind_(GlobalKind::Import) {
    initial_ = InitExpr(LitVal(type));
    importIndex_ = importIndex;
    isMutable_ = isMutable;
    isWasm_ = kind == Wasm;
    isExport_ = false;
    offset_ = UINT32_MAX;
  }

  void setOffset(unsigned offset) {
    MOZ_ASSERT(!isConstant());
    MOZ_ASSERT(offset_ == UINT32_MAX);
    offset_ = offset;
  }
  unsigned offset() const {
    MOZ_ASSERT(!isConstant());
    MOZ_ASSERT(offset_ != UINT32_MAX);
    return offset_;
  }

  void setIsExport() {
    if (!isConstant()) {
      isExport_ = true;
    }
  }

  GlobalKind kind() const { return kind_; }
  bool isVariable() const { return kind_ == GlobalKind::Variable; }
  bool isConstant() const { return kind_ == GlobalKind::Constant; }
  bool isImport() const { return kind_ == GlobalKind::Import; }

  bool isMutable() const { return !isConstant() && isMutable_; }
  const InitExpr& initExpr() const {
    MOZ_ASSERT(!isImport());
    return initial_;
  }
  uint32_t importIndex() const {
    MOZ_ASSERT(isImport());
    return importIndex_;
  }

  LitVal constantValue() const { return initial_.literal(); }

  // If isIndirect() is true then storage for the value is not in the
  // instance's global area, but in a WasmGlobalObject::Cell hanging off a
  // WasmGlobalObject; the global area contains a pointer to the Cell.
  //
  // We don't want to indirect unless we must, so only mutable, exposed
  // globals are indirected - in all other cases we copy values into and out
  // of their module.
  //
  // Note that isIndirect() isn't equivalent to getting a WasmGlobalObject:
  // an immutable exported global will still get an object, but will not be
  // indirect.
  bool isIndirect() const {
    return isMutable() && isWasm() && (isImport() || isExport());
  }

  ValType type() const { return initial_.type(); }

  WASM_DECLARE_SERIALIZABLE(GlobalDesc)
};

using GlobalDescVector = Vector<GlobalDesc, 0, SystemAllocPolicy>;

// An EventDesc describes a single event for non-local control flow, such as
// for exceptions.

#ifdef ENABLE_WASM_EXCEPTIONS
struct EventDesc {
  EventKind kind;
  ValTypeVector type;
  bool isExport;

  EventDesc(EventKind kind, ValTypeVector&& type, bool isExport = false)
      : kind(kind), type(std::move(type)), isExport(isExport) {}

  ResultType resultType() const { return ResultType::Vector(type); }
};

using EventDescVector = Vector<EventDesc, 0, SystemAllocPolicy>;
#endif

// When a ElemSegment is "passive" it is shared between a wasm::Module and its
// wasm::Instances. To allow each segment to be released as soon as the last
// Instance elem.drops it and the Module is destroyed, each ElemSegment is
// individually atomically ref-counted.

struct ElemSegment : AtomicRefCounted<ElemSegment> {
  enum class Kind {
    Active,
    Passive,
    Declared,
  };

  Kind kind;
  uint32_t tableIndex;
  RefType elemType;
  Maybe<InitExpr> offsetIfActive;
  Uint32Vector elemFuncIndices;  // Element may be NullFuncIndex

  bool active() const { return kind == Kind::Active; }

  const InitExpr& offset() const { return *offsetIfActive; }

  size_t length() const { return elemFuncIndices.length(); }

  WASM_DECLARE_SERIALIZABLE(ElemSegment)
};

// NullFuncIndex represents the case when an element segment (of type funcref)
// contains a null element.
constexpr uint32_t NullFuncIndex = UINT32_MAX;
static_assert(NullFuncIndex > MaxFuncs, "Invariant");

using MutableElemSegment = RefPtr<ElemSegment>;
using SharedElemSegment = SerializableRefPtr<const ElemSegment>;
using ElemSegmentVector = Vector<SharedElemSegment, 0, SystemAllocPolicy>;

// DataSegmentEnv holds the initial results of decoding a data segment from the
// bytecode and is stored in the ModuleEnvironment during compilation. When
// compilation completes, (non-Env) DataSegments are created and stored in
// the wasm::Module which contain copies of the data segment payload. This
// allows non-compilation uses of wasm validation to avoid expensive copies.
//
// When a DataSegment is "passive" it is shared between a wasm::Module and its
// wasm::Instances. To allow each segment to be released as soon as the last
// Instance mem.drops it and the Module is destroyed, each DataSegment is
// individually atomically ref-counted.

struct DataSegmentEnv {
  Maybe<InitExpr> offsetIfActive;
  uint32_t bytecodeOffset;
  uint32_t length;
};

using DataSegmentEnvVector = Vector<DataSegmentEnv, 0, SystemAllocPolicy>;

struct DataSegment : AtomicRefCounted<DataSegment> {
  Maybe<InitExpr> offsetIfActive;
  Bytes bytes;

  DataSegment() = default;

  bool active() const { return !!offsetIfActive; }

  const InitExpr& offset() const { return *offsetIfActive; }

  [[nodiscard]] bool init(const ShareableBytes& bytecode,
                          const DataSegmentEnv& src) {
    if (src.offsetIfActive) {
      offsetIfActive.emplace();
      if (!offsetIfActive->clone(*src.offsetIfActive)) {
        return false;
      }
    }
    return bytes.append(bytecode.begin() + src.bytecodeOffset, src.length);
  }

  WASM_DECLARE_SERIALIZABLE(DataSegment)
};

using MutableDataSegment = RefPtr<DataSegment>;
using SharedDataSegment = SerializableRefPtr<const DataSegment>;
using DataSegmentVector = Vector<SharedDataSegment, 0, SystemAllocPolicy>;

// The CustomSection(Env) structs are like DataSegment(Env): CustomSectionEnv is
// stored in the ModuleEnvironment and CustomSection holds a copy of the payload
// and is stored in the wasm::Module.

struct CustomSectionEnv {
  uint32_t nameOffset;
  uint32_t nameLength;
  uint32_t payloadOffset;
  uint32_t payloadLength;
};

using CustomSectionEnvVector = Vector<CustomSectionEnv, 0, SystemAllocPolicy>;

struct CustomSection {
  Bytes name;
  SharedBytes payload;

  WASM_DECLARE_SERIALIZABLE(CustomSection)
};

using CustomSectionVector = Vector<CustomSection, 0, SystemAllocPolicy>;

// A Name represents a string of utf8 chars embedded within the name custom
// section. The offset of a name is expressed relative to the beginning of the
// name section's payload so that Names can stored in wasm::Code, which only
// holds the name section's bytes, not the whole bytecode.

struct Name {
  // All fields are treated as cacheable POD:
  uint32_t offsetInNamePayload;
  uint32_t length;

  Name() : offsetInNamePayload(UINT32_MAX), length(0) {}
};

using NameVector = Vector<Name, 0, SystemAllocPolicy>;

// TypeIdDesc describes the runtime representation of a TypeDef suitable for
// type equality checks. The kind of representation depends on whether the type
// is a function or a struct. This will likely be simplified in the future once
// mutually recursives types are able to be collected.
//
// For functions, a FuncType is allocated and stored in a process-wide hash
// table, so that pointer equality implies structural equality. As an
// optimization for the 99% case where the FuncType has a small number of
// parameters, the FuncType is bit-packed into a uint32 immediate value so that
// integer equality implies structural equality. Both cases can be handled with
// a single comparison by always setting the LSB for the immediates
// (the LSB is necessarily 0 for allocated FuncType pointers due to alignment).
//
// TODO: Write description for StructTypes once it is well formed.

class TypeIdDesc {
 public:
  static const uintptr_t ImmediateBit = 0x1;

 private:
  TypeIdDescKind kind_;
  size_t bits_;

  TypeIdDesc(TypeIdDescKind kind, size_t bits) : kind_(kind), bits_(bits) {}

 public:
  TypeIdDescKind kind() const { return kind_; }
  static bool isGlobal(const TypeDef& type);

  TypeIdDesc() : kind_(TypeIdDescKind::None), bits_(0) {}
  static TypeIdDesc global(const TypeDef& type, uint32_t globalDataOffset);
  static TypeIdDesc immediate(const TypeDef& type);

  bool isGlobal() const { return kind_ == TypeIdDescKind::Global; }

  size_t immediate() const {
    MOZ_ASSERT(kind_ == TypeIdDescKind::Immediate);
    return bits_;
  }
  uint32_t globalDataOffset() const {
    MOZ_ASSERT(kind_ == TypeIdDescKind::Global);
    return bits_;
  }
};

using TypeIdDescVector = Vector<TypeIdDesc, 0, SystemAllocPolicy>;

// TypeDefWithId pairs a FuncType with TypeIdDesc, describing either how to
// compile code that compares this signature's id or, at instantiation what
// signature ids to allocate in the global hash and where to put them.

struct TypeDefWithId : public TypeDef {
  TypeIdDesc id;

  TypeDefWithId() = default;
  explicit TypeDefWithId(TypeDef&& typeDef)
      : TypeDef(std::move(typeDef)), id() {}
  TypeDefWithId(TypeDef&& typeDef, TypeIdDesc id)
      : TypeDef(std::move(typeDef)), id(id) {}

  WASM_DECLARE_SERIALIZABLE(TypeDefWithId)
};

using TypeDefWithIdVector = Vector<TypeDefWithId, 0, SystemAllocPolicy>;
using TypeDefWithIdPtrVector =
    Vector<const TypeDefWithId*, 0, SystemAllocPolicy>;

// A wrapper around the bytecode offset of a wasm instruction within a whole
// module, used for trap offsets or call offsets. These offsets should refer to
// the first byte of the instruction that triggered the trap / did the call and
// should ultimately derive from OpIter::bytecodeOffset.

class BytecodeOffset {
  static const uint32_t INVALID = -1;
  uint32_t offset_;

 public:
  BytecodeOffset() : offset_(INVALID) {}
  explicit BytecodeOffset(uint32_t offset) : offset_(offset) {}

  bool isValid() const { return offset_ != INVALID; }
  uint32_t offset() const {
    MOZ_ASSERT(isValid());
    return offset_;
  }
};

// A TrapSite (in the TrapSiteVector for a given Trap code) represents a wasm
// instruction at a given bytecode offset that can fault at the given pc offset.
// When such a fault occurs, a signal/exception handler looks up the TrapSite to
// confirm the fault is intended/safe and redirects pc to the trap stub.

struct TrapSite {
  uint32_t pcOffset;
  BytecodeOffset bytecode;

  TrapSite() : pcOffset(-1), bytecode() {}
  TrapSite(uint32_t pcOffset, BytecodeOffset bytecode)
      : pcOffset(pcOffset), bytecode(bytecode) {}

  void offsetBy(uint32_t offset) { pcOffset += offset; }
};

WASM_DECLARE_POD_VECTOR(TrapSite, TrapSiteVector)

struct TrapSiteVectorArray
    : EnumeratedArray<Trap, Trap::Limit, TrapSiteVector> {
  bool empty() const;
  void clear();
  void swap(TrapSiteVectorArray& rhs);
  void shrinkStorageToFit();

  WASM_DECLARE_SERIALIZABLE(TrapSiteVectorArray)
};

// On trap, the bytecode offset to be reported in callstacks is saved.

struct TrapData {
  // The resumePC indicates where, if the trap doesn't throw, the trap stub
  // should jump to after restoring all register state.
  void* resumePC;

  // The unwoundPC is the PC after adjustment by wasm::StartUnwinding(), which
  // basically unwinds partially-construted wasm::Frames when pc is in the
  // prologue/epilogue. Stack traces during a trap should use this PC since
  // it corresponds to the JitActivation::wasmExitFP.
  void* unwoundPC;

  Trap trap;
  uint32_t bytecodeOffset;
};

// The (,Callable,Func)Offsets classes are used to record the offsets of
// different key points in a CodeRange during compilation.

struct Offsets {
  explicit Offsets(uint32_t begin = 0, uint32_t end = 0)
      : begin(begin), end(end) {}

  // These define a [begin, end) contiguous range of instructions compiled
  // into a CodeRange.
  uint32_t begin;
  uint32_t end;
};

struct CallableOffsets : Offsets {
  MOZ_IMPLICIT CallableOffsets(uint32_t ret = 0) : Offsets(), ret(ret) {}

  // The offset of the return instruction precedes 'end' by a variable number
  // of instructions due to out-of-line codegen.
  uint32_t ret;
};

struct JitExitOffsets : CallableOffsets {
  MOZ_IMPLICIT JitExitOffsets()
      : CallableOffsets(), untrustedFPStart(0), untrustedFPEnd(0) {}

  // There are a few instructions in the Jit exit where FP may be trash
  // (because it may have been clobbered by the JS Jit), known as the
  // untrusted FP zone.
  uint32_t untrustedFPStart;
  uint32_t untrustedFPEnd;
};

struct FuncOffsets : CallableOffsets {
  MOZ_IMPLICIT FuncOffsets()
      : CallableOffsets(), uncheckedCallEntry(0), tierEntry(0) {}

  // Function CodeRanges have a checked call entry which takes an extra
  // signature argument which is checked against the callee's signature before
  // falling through to the normal prologue. The checked call entry is thus at
  // the beginning of the CodeRange and the unchecked call entry is at some
  // offset after the checked call entry.
  uint32_t uncheckedCallEntry;

  // The tierEntry is the point within a function to which the patching code
  // within a Tier-1 function jumps.  It could be the instruction following
  // the jump in the Tier-1 function, or the point following the standard
  // prologue within a Tier-2 function.
  uint32_t tierEntry;
};

using FuncOffsetsVector = Vector<FuncOffsets, 0, SystemAllocPolicy>;

// A CodeRange describes a single contiguous range of code within a wasm
// module's code segment. A CodeRange describes what the code does and, for
// function bodies, the name and source coordinates of the function.

class CodeRange {
 public:
  enum Kind {
    Function,          // function definition
    InterpEntry,       // calls into wasm from C++
    JitEntry,          // calls into wasm from jit code
    ImportInterpExit,  // slow-path calling from wasm into C++ interp
    ImportJitExit,     // fast-path calling from wasm into jit code
    BuiltinThunk,      // fast-path calling from wasm into a C++ native
    TrapExit,          // calls C++ to report and jumps to throw stub
    DebugTrap,         // calls C++ to handle debug event
    FarJumpIsland,     // inserted to connect otherwise out-of-range insns
    Throw              // special stack-unwinding stub jumped to by other stubs
  };

 private:
  // All fields are treated as cacheable POD:
  uint32_t begin_;
  uint32_t ret_;
  uint32_t end_;
  union {
    struct {
      uint32_t funcIndex_;
      union {
        struct {
          uint32_t lineOrBytecode_;
          uint8_t beginToUncheckedCallEntry_;
          uint8_t beginToTierEntry_;
        } func;
        struct {
          uint16_t beginToUntrustedFPStart_;
          uint16_t beginToUntrustedFPEnd_;
        } jitExit;
      };
    };
    Trap trap_;
  } u;
  Kind kind_ : 8;

 public:
  CodeRange() = default;
  CodeRange(Kind kind, Offsets offsets);
  CodeRange(Kind kind, uint32_t funcIndex, Offsets offsets);
  CodeRange(Kind kind, CallableOffsets offsets);
  CodeRange(Kind kind, uint32_t funcIndex, CallableOffsets);
  CodeRange(uint32_t funcIndex, JitExitOffsets offsets);
  CodeRange(uint32_t funcIndex, uint32_t lineOrBytecode, FuncOffsets offsets);

  void offsetBy(uint32_t offset) {
    begin_ += offset;
    end_ += offset;
    if (hasReturn()) {
      ret_ += offset;
    }
  }

  // All CodeRanges have a begin and end.

  uint32_t begin() const { return begin_; }
  uint32_t end() const { return end_; }

  // Other fields are only available for certain CodeRange::Kinds.

  Kind kind() const { return kind_; }

  bool isFunction() const { return kind() == Function; }
  bool isImportExit() const {
    return kind() == ImportJitExit || kind() == ImportInterpExit ||
           kind() == BuiltinThunk;
  }
  bool isImportInterpExit() const { return kind() == ImportInterpExit; }
  bool isImportJitExit() const { return kind() == ImportJitExit; }
  bool isTrapExit() const { return kind() == TrapExit; }
  bool isDebugTrap() const { return kind() == DebugTrap; }
  bool isThunk() const { return kind() == FarJumpIsland; }

  // Function, import exits and trap exits have standard callable prologues
  // and epilogues. Asynchronous frame iteration needs to know the offset of
  // the return instruction to calculate the frame pointer.

  bool hasReturn() const {
    return isFunction() || isImportExit() || isDebugTrap();
  }
  uint32_t ret() const {
    MOZ_ASSERT(hasReturn());
    return ret_;
  }

  // Functions, export stubs and import stubs all have an associated function
  // index.

  bool isJitEntry() const { return kind() == JitEntry; }
  bool isInterpEntry() const { return kind() == InterpEntry; }
  bool isEntry() const { return isInterpEntry() || isJitEntry(); }
  bool hasFuncIndex() const {
    return isFunction() || isImportExit() || isEntry();
  }
  uint32_t funcIndex() const {
    MOZ_ASSERT(hasFuncIndex());
    return u.funcIndex_;
  }

  // TrapExit CodeRanges have a Trap field.

  Trap trap() const {
    MOZ_ASSERT(isTrapExit());
    return u.trap_;
  }

  // Function CodeRanges have two entry points: one for normal calls (with a
  // known signature) and one for table calls (which involves dynamic
  // signature checking).

  uint32_t funcCheckedCallEntry() const {
    MOZ_ASSERT(isFunction());
    return begin_;
  }
  uint32_t funcUncheckedCallEntry() const {
    MOZ_ASSERT(isFunction());
    return begin_ + u.func.beginToUncheckedCallEntry_;
  }
  uint32_t funcTierEntry() const {
    MOZ_ASSERT(isFunction());
    return begin_ + u.func.beginToTierEntry_;
  }
  uint32_t funcLineOrBytecode() const {
    MOZ_ASSERT(isFunction());
    return u.func.lineOrBytecode_;
  }

  // ImportJitExit have a particular range where the value of FP can't be
  // trusted for profiling and thus must be ignored.

  uint32_t jitExitUntrustedFPStart() const {
    MOZ_ASSERT(isImportJitExit());
    return begin_ + u.jitExit.beginToUntrustedFPStart_;
  }
  uint32_t jitExitUntrustedFPEnd() const {
    MOZ_ASSERT(isImportJitExit());
    return begin_ + u.jitExit.beginToUntrustedFPEnd_;
  }

  // A sorted array of CodeRanges can be looked up via BinarySearch and
  // OffsetInCode.

  struct OffsetInCode {
    size_t offset;
    explicit OffsetInCode(size_t offset) : offset(offset) {}
    bool operator==(const CodeRange& rhs) const {
      return offset >= rhs.begin() && offset < rhs.end();
    }
    bool operator<(const CodeRange& rhs) const { return offset < rhs.begin(); }
  };
};

WASM_DECLARE_POD_VECTOR(CodeRange, CodeRangeVector)

extern const CodeRange* LookupInSorted(const CodeRangeVector& codeRanges,
                                       CodeRange::OffsetInCode target);

// While the frame-pointer chain allows the stack to be unwound without
// metadata, Error.stack still needs to know the line/column of every call in
// the chain. A CallSiteDesc describes a single callsite to which CallSite adds
// the metadata necessary to walk up to the next frame. Lastly CallSiteAndTarget
// adds the function index of the callee.

class CallSiteDesc {
  static constexpr size_t LINE_OR_BYTECODE_BITS_SIZE = 29;
  uint32_t lineOrBytecode_ : LINE_OR_BYTECODE_BITS_SIZE;
  uint32_t kind_ : 3;

 public:
  static constexpr uint32_t MAX_LINE_OR_BYTECODE_VALUE =
      (1 << LINE_OR_BYTECODE_BITS_SIZE) - 1;

  enum Kind {
    Func,        // pc-relative call to a specific function
    Dynamic,     // dynamic callee called via register
    Symbolic,    // call to a single symbolic callee
    EnterFrame,  // call to a enter frame handler
    LeaveFrame,  // call to a leave frame handler
    Breakpoint   // call to instruction breakpoint
  };
  CallSiteDesc() : lineOrBytecode_(0), kind_(0) {}
  explicit CallSiteDesc(Kind kind) : lineOrBytecode_(0), kind_(kind) {
    MOZ_ASSERT(kind == Kind(kind_));
  }
  CallSiteDesc(uint32_t lineOrBytecode, Kind kind)
      : lineOrBytecode_(lineOrBytecode), kind_(kind) {
    MOZ_ASSERT(kind == Kind(kind_));
    MOZ_ASSERT(lineOrBytecode == lineOrBytecode_);
  }
  uint32_t lineOrBytecode() const { return lineOrBytecode_; }
  Kind kind() const { return Kind(kind_); }
  bool mightBeCrossInstance() const { return kind() == CallSiteDesc::Dynamic; }
};

class CallSite : public CallSiteDesc {
  uint32_t returnAddressOffset_;

 public:
  CallSite() : returnAddressOffset_(0) {}

  CallSite(CallSiteDesc desc, uint32_t returnAddressOffset)
      : CallSiteDesc(desc), returnAddressOffset_(returnAddressOffset) {}

  void offsetBy(int32_t delta) { returnAddressOffset_ += delta; }
  uint32_t returnAddressOffset() const { return returnAddressOffset_; }
};

WASM_DECLARE_POD_VECTOR(CallSite, CallSiteVector)

// A CallSiteTarget describes the callee of a CallSite, either a function or a
// trap exit. Although checked in debug builds, a CallSiteTarget doesn't
// officially know whether it targets a function or trap, relying on the Kind of
// the CallSite to discriminate.

class CallSiteTarget {
  uint32_t packed_;
#ifdef DEBUG
  enum Kind { None, FuncIndex, TrapExit } kind_;
#endif

 public:
  explicit CallSiteTarget()
      : packed_(UINT32_MAX)
#ifdef DEBUG
        ,
        kind_(None)
#endif
  {
  }

  explicit CallSiteTarget(uint32_t funcIndex)
      : packed_(funcIndex)
#ifdef DEBUG
        ,
        kind_(FuncIndex)
#endif
  {
  }

  explicit CallSiteTarget(Trap trap)
      : packed_(uint32_t(trap))
#ifdef DEBUG
        ,
        kind_(TrapExit)
#endif
  {
  }

  uint32_t funcIndex() const {
    MOZ_ASSERT(kind_ == FuncIndex);
    return packed_;
  }

  Trap trap() const {
    MOZ_ASSERT(kind_ == TrapExit);
    MOZ_ASSERT(packed_ < uint32_t(Trap::Limit));
    return Trap(packed_);
  }
};

using CallSiteTargetVector = Vector<CallSiteTarget, 0, SystemAllocPolicy>;

// WasmTryNotes are stored in a vector that acts as an exception table for
// wasm try-catch blocks. These represent the information needed to take
// exception handling actions after a throw is executed.
struct WasmTryNote {
  explicit WasmTryNote(uint32_t begin = 0, uint32_t end = 0,
                       uint32_t framePushed = 0)
      : begin(begin), end(end), framePushed(framePushed) {}

  uint32_t begin;        // Begin code offset of try instructions.
  uint32_t end;          // End code offset of try instructions.
  uint32_t entryPoint;   // The offset of the landing pad.
  uint32_t framePushed;  // Track offset from frame of stack pointer.

  void offsetBy(uint32_t offset) {
    begin += offset;
    end += offset;
    entryPoint += offset;
  }

  bool operator<(const WasmTryNote& other) const {
    if (end == other.end) {
      return begin > other.begin;
    }
    return end < other.end;
  }
};

WASM_DECLARE_POD_VECTOR(WasmTryNote, WasmTryNoteVector)

// Represents the resizable limits of memories and tables.

struct Limits {
  uint64_t initial;
  Maybe<uint64_t> maximum;

  // `shared` is Shareable::False for tables but may be Shareable::True for
  // memories.
  Shareable shared;

  Limits() = default;
  explicit Limits(uint64_t initial, const Maybe<uint64_t>& maximum = Nothing(),
                  Shareable shared = Shareable::False)
      : initial(initial), maximum(maximum), shared(shared) {}
};

// Memories can be 32-bit (indices are 32 bits and the max is 4GB) or 64-bit
// (indices are 64 bits and the max is XXX).

enum class MemoryKind { Memory32, Memory64 };

// MemoryDesc describes a memory.

struct MemoryDesc {
  MemoryKind kind;
  Limits limits;

  bool isShared() const { return limits.shared == Shareable::True; }

  // Whether a backing store for this memory may move when grown.
  bool canMovingGrow() const { return limits.maximum.isNothing(); }

  // Whether the bounds check limit (see the doc comment in
  // ArrayBufferObject.cpp regarding linear memory structure) can ever be
  // larger than 32-bits.
  bool boundsCheckLimitIs32Bits() const {
    return limits.maximum.isSome() &&
           limits.maximum.value() < (0x100000000 / PageSize);
  }

  // The initial length of this memory in pages.
  Pages initialPages() const { return Pages(limits.initial); }

  // The maximum length of this memory in pages.
  Maybe<Pages> maximumPages() const {
    return limits.maximum.map([](uint64_t x) { return Pages(x); });
  }

  // The initial length of this memory in bytes. Only valid for memory32.
  uint64_t initialLength32() const {
    MOZ_ASSERT(kind == MemoryKind::Memory32);
    // See static_assert after MemoryDesc for why this is safe.
    return limits.initial * PageSize;
  }

  // The maximum length of this memory in bytes. Only valid for memory32.
  Maybe<uint64_t> maximumLength32() const {
    MOZ_ASSERT(kind == MemoryKind::Memory32);
    if (limits.maximum) {
      // See static_assert after MemoryDesc for why this is safe.
      return Some(*limits.maximum * PageSize);
    }
    return Nothing();
  }

  MemoryDesc() = default;
  MemoryDesc(MemoryKind kind, Limits limits) : kind(kind), limits(limits) {}
};

// We don't need to worry about overflow with a Memory32 field when
// using a uint64_t.
static_assert(MaxMemory32LimitField <= UINT64_MAX / PageSize);

// TableDesc describes a table as well as the offset of the table's base pointer
// in global memory.
//
// A TableDesc contains the element type and whether the table is for asm.js,
// which determines the table representation.
//  - ExternRef: a wasm anyref word (wasm::AnyRef)
//  - FuncRef: a two-word FunctionTableElem (wasm indirect call ABI)
//  - FuncRef (if `isAsmJS`): a two-word FunctionTableElem (asm.js ABI)
// Eventually there should be a single unified AnyRef representation.

struct TableDesc {
  RefType elemType;
  bool importedOrExported;
  bool isAsmJS;
  uint32_t globalDataOffset;
  uint32_t initialLength;
  Maybe<uint32_t> maximumLength;

  TableDesc() = default;
  TableDesc(RefType elemType, uint32_t initialLength,
            Maybe<uint32_t> maximumLength, bool isAsmJS,
            bool importedOrExported = false)
      : elemType(elemType),
        importedOrExported(importedOrExported),
        isAsmJS(isAsmJS),
        globalDataOffset(UINT32_MAX),
        initialLength(initialLength),
        maximumLength(maximumLength) {}
};

using TableDescVector = Vector<TableDesc, 0, SystemAllocPolicy>;

// CalleeDesc describes how to compile one of the variety of asm.js/wasm calls.
// This is hoisted into WasmTypes.h for sharing between Ion and Baseline.

class CalleeDesc {
 public:
  enum Which {
    // Calls a function defined in the same module by its index.
    Func,

    // Calls the import identified by the offset of its FuncImportTls in
    // thread-local data.
    Import,

    // Calls a WebAssembly table (heterogeneous, index must be bounds
    // checked, callee instance depends on TableDesc).
    WasmTable,

    // Calls an asm.js table (homogeneous, masked index, same-instance).
    AsmJSTable,

    // Call a C++ function identified by SymbolicAddress.
    Builtin,

    // Like Builtin, but automatically passes Instance* as first argument.
    BuiltinInstanceMethod
  };

 private:
  // which_ shall be initialized in the static constructors
  MOZ_INIT_OUTSIDE_CTOR Which which_;
  union U {
    U() : funcIndex_(0) {}
    uint32_t funcIndex_;
    struct {
      uint32_t globalDataOffset_;
    } import;
    struct {
      uint32_t globalDataOffset_;
      uint32_t minLength_;
      TypeIdDesc funcTypeId_;
    } table;
    SymbolicAddress builtin_;
  } u;

 public:
  CalleeDesc() = default;
  static CalleeDesc function(uint32_t funcIndex) {
    CalleeDesc c;
    c.which_ = Func;
    c.u.funcIndex_ = funcIndex;
    return c;
  }
  static CalleeDesc import(uint32_t globalDataOffset) {
    CalleeDesc c;
    c.which_ = Import;
    c.u.import.globalDataOffset_ = globalDataOffset;
    return c;
  }
  static CalleeDesc wasmTable(const TableDesc& desc, TypeIdDesc funcTypeId) {
    CalleeDesc c;
    c.which_ = WasmTable;
    c.u.table.globalDataOffset_ = desc.globalDataOffset;
    c.u.table.minLength_ = desc.initialLength;
    c.u.table.funcTypeId_ = funcTypeId;
    return c;
  }
  static CalleeDesc asmJSTable(const TableDesc& desc) {
    CalleeDesc c;
    c.which_ = AsmJSTable;
    c.u.table.globalDataOffset_ = desc.globalDataOffset;
    return c;
  }
  static CalleeDesc builtin(SymbolicAddress callee) {
    CalleeDesc c;
    c.which_ = Builtin;
    c.u.builtin_ = callee;
    return c;
  }
  static CalleeDesc builtinInstanceMethod(SymbolicAddress callee) {
    CalleeDesc c;
    c.which_ = BuiltinInstanceMethod;
    c.u.builtin_ = callee;
    return c;
  }
  Which which() const { return which_; }
  uint32_t funcIndex() const {
    MOZ_ASSERT(which_ == Func);
    return u.funcIndex_;
  }
  uint32_t importGlobalDataOffset() const {
    MOZ_ASSERT(which_ == Import);
    return u.import.globalDataOffset_;
  }
  bool isTable() const { return which_ == WasmTable || which_ == AsmJSTable; }
  uint32_t tableLengthGlobalDataOffset() const {
    MOZ_ASSERT(isTable());
    return u.table.globalDataOffset_ + offsetof(TableTls, length);
  }
  uint32_t tableFunctionBaseGlobalDataOffset() const {
    MOZ_ASSERT(isTable());
    return u.table.globalDataOffset_ + offsetof(TableTls, functionBase);
  }
  TypeIdDesc wasmTableSigId() const {
    MOZ_ASSERT(which_ == WasmTable);
    return u.table.funcTypeId_;
  }
  uint32_t wasmTableMinLength() const {
    MOZ_ASSERT(which_ == WasmTable);
    return u.table.minLength_;
  }
  SymbolicAddress builtin() const {
    MOZ_ASSERT(which_ == Builtin || which_ == BuiltinInstanceMethod);
    return u.builtin_;
  }
};

// Because ARM has a fixed-width instruction encoding, ARM can only express a
// limited subset of immediates (in a single instruction).

static const uint64_t HighestValidARMImmediate = 0xff000000;

extern bool IsValidARMImmediate(uint32_t i);

extern uint64_t RoundUpToNextValidARMImmediate(uint64_t i);

// Bounds checks always compare the base of the memory access with the bounds
// check limit. If the memory access is unaligned, this means that, even if the
// bounds check succeeds, a few bytes of the access can extend past the end of
// memory. To guard against this, extra space is included in the guard region to
// catch the overflow. MaxMemoryAccessSize is a conservative approximation of
// the maximum guard space needed to catch all unaligned overflows.

static const unsigned MaxMemoryAccessSize = LitVal::sizeofLargestValue();

#ifdef WASM_SUPPORTS_HUGE_MEMORY

// On WASM_SUPPORTS_HUGE_MEMORY platforms, every asm.js or WebAssembly 32-bit
// memory unconditionally allocates a huge region of virtual memory of size
// wasm::HugeMappedSize. This allows all memory resizing to work without
// reallocation and provides enough guard space for all offsets to be folded
// into memory accesses.

static const uint64_t HugeIndexRange = uint64_t(UINT32_MAX) + 1;
static const uint64_t HugeOffsetGuardLimit = uint64_t(INT32_MAX) + 1;
static const uint64_t HugeUnalignedGuardPage = PageSize;
static const uint64_t HugeMappedSize =
    HugeIndexRange + HugeOffsetGuardLimit + HugeUnalignedGuardPage;

static_assert(MaxMemoryAccessSize <= HugeUnalignedGuardPage,
              "rounded up to static page size");
static_assert(HugeOffsetGuardLimit < UINT32_MAX,
              "checking for overflow against OffsetGuardLimit is enough.");

#endif

// On !WASM_SUPPORTS_HUGE_MEMORY platforms:
//  - To avoid OOM in ArrayBuffer::prepareForAsmJS, asm.js continues to use the
//    original ArrayBuffer allocation which has no guard region at all.
//  - For WebAssembly memories, an additional GuardSize is mapped after the
//    accessible region of the memory to catch folded (base+offset) accesses
//    where `offset < OffsetGuardLimit` as well as the overflow from unaligned
//    accesses, as described above for MaxMemoryAccessSize.

static const size_t OffsetGuardLimit = PageSize - MaxMemoryAccessSize;
static const size_t GuardSize = PageSize;

static_assert(MaxMemoryAccessSize < GuardSize,
              "Guard page handles partial out-of-bounds");
static_assert(OffsetGuardLimit < UINT32_MAX,
              "checking for overflow against OffsetGuardLimit is enough.");

static constexpr size_t GetMaxOffsetGuardLimit(bool hugeMemory) {
#ifdef WASM_SUPPORTS_HUGE_MEMORY
  return hugeMemory ? HugeOffsetGuardLimit : OffsetGuardLimit;
#else
  return OffsetGuardLimit;
#endif
}

static const size_t MinOffsetGuardLimit = OffsetGuardLimit;

// Return whether the given immediate satisfies the constraints of the platform
// (viz. that, on ARM, IsValidARMImmediate).

extern bool IsValidBoundsCheckImmediate(uint32_t i);

// For a given WebAssembly/asm.js max pages, return the number of bytes to
// map which will necessarily be a multiple of the system page size and greater
// than maxPages in bytes. For a returned mappedSize:
//   boundsCheckLimit = mappedSize - GuardSize
//   IsValidBoundsCheckImmediate(boundsCheckLimit)

extern size_t ComputeMappedSize(Pages maxPages);

// The following thresholds were derived from a microbenchmark. If we begin to
// ship this optimization for more platforms, we will need to extend this list.

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64)
static const uint32_t MaxInlineMemoryCopyLength = 64;
static const uint32_t MaxInlineMemoryFillLength = 64;
#elif defined(JS_CODEGEN_X86)
static const uint32_t MaxInlineMemoryCopyLength = 32;
static const uint32_t MaxInlineMemoryFillLength = 32;
#else
static const uint32_t MaxInlineMemoryCopyLength = 0;
static const uint32_t MaxInlineMemoryFillLength = 0;
#endif

static_assert(MaxInlineMemoryCopyLength < MinOffsetGuardLimit, "precondition");
static_assert(MaxInlineMemoryFillLength < MinOffsetGuardLimit, "precondition");

// Verbose logging support.

extern void Log(JSContext* cx, const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3);

// Codegen debug support.

enum class DebugChannel {
  Function,
  Import,
};

#ifdef WASM_CODEGEN_DEBUG
bool IsCodegenDebugEnabled(DebugChannel channel);
#endif

void DebugCodegen(DebugChannel channel, const char* fmt, ...)
    MOZ_FORMAT_PRINTF(2, 3);

using PrintCallback = void (*)(const char*);

#ifdef ENABLE_WASM_SIMD_WORMHOLE
bool IsWormholeTrigger(const V128& shuffleMask);
jit::SimdConstant WormholeSignature();
#endif

}  // namespace wasm
}  // namespace js

#endif  // wasm_types_h
