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

#ifndef wasm_module_types_h
#define wasm_module_types_h

#include "mozilla/RefPtr.h"
#include "mozilla/Span.h"

#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/RefCounted.h"
#include "js/Utility.h"
#include "js/Vector.h"

#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmExprType.h"
#include "wasm/WasmInitExpr.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmShareable.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmValue.h"

namespace js {
namespace wasm {

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Span;

class FuncType;

// A Module can either be asm.js or wasm.

enum ModuleKind { Wasm, AsmJS };

// CacheableChars is used to cacheably store UniqueChars.

struct CacheableChars : UniqueChars {
  CacheableChars() = default;
  explicit CacheableChars(char* ptr) : UniqueChars(ptr) {}
  MOZ_IMPLICIT CacheableChars(UniqueChars&& rhs)
      : UniqueChars(std::move(rhs)) {}
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

using CacheableCharsVector = Vector<CacheableChars, 0, SystemAllocPolicy>;

// CacheableName is used to cacheably store a UTF-8 string that may contain
// null terminators in sequence.

struct CacheableName {
 private:
  UTF8Bytes bytes_;

  const char* begin() const { return (const char*)bytes_.begin(); }
  size_t length() const { return bytes_.length(); }

 public:
  CacheableName() = default;
  MOZ_IMPLICIT CacheableName(UTF8Bytes&& rhs) : bytes_(std::move(rhs)) {}

  bool isEmpty() const { return bytes_.length() == 0; }

  Span<char> utf8Bytes() { return Span<char>(bytes_); }
  Span<const char> utf8Bytes() const { return Span<const char>(bytes_); }

  static CacheableName fromUTF8Chars(UniqueChars&& utf8Chars);
  [[nodiscard]] static bool fromUTF8Chars(const char* utf8Chars,
                                          CacheableName* name);

  [[nodiscard]] JSAtom* toAtom(JSContext* cx) const;
  [[nodiscard]] bool toPropertyKey(JSContext* cx,
                                   MutableHandleId propertyKey) const;
  [[nodiscard]] UniqueChars toQuotedString(JSContext* cx) const;

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  WASM_DECLARE_FRIEND_SERIALIZE(CacheableName);
};

using CacheableNameVector = Vector<CacheableName, 0, SystemAllocPolicy>;

// A hash policy for names.
struct NameHasher {
  using Key = Span<const char>;
  using Lookup = Span<const char>;

  static HashNumber hash(const Lookup& aLookup) {
    return mozilla::HashString(aLookup.data(), aLookup.Length());
  }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    return aKey == aLookup;
  }
};

// Import describes a single wasm import. An ImportVector describes all
// of a single module's imports.
//
// ImportVector is built incrementally by ModuleGenerator and then stored
// immutably by Module.

struct Import {
  CacheableName module;
  CacheableName field;
  DefinitionKind kind;

  Import() = default;
  Import(CacheableName&& module, CacheableName&& field, DefinitionKind kind)
      : module(std::move(module)), field(std::move(field)), kind(kind) {}

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
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
 public:
  struct CacheablePod {
    DefinitionKind kind_;
    uint32_t index_;

    WASM_CHECK_CACHEABLE_POD(kind_, index_);
  };

 private:
  CacheableName fieldName_;
  CacheablePod pod;

 public:
  Export() = default;
  explicit Export(CacheableName&& fieldName, uint32_t index,
                  DefinitionKind kind);

  const CacheableName& fieldName() const { return fieldName_; }

  DefinitionKind kind() const { return pod.kind_; }
  uint32_t funcIndex() const;
  uint32_t tagIndex() const;
  uint32_t memoryIndex() const;
  uint32_t globalIndex() const;
  uint32_t tableIndex() const;

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  WASM_DECLARE_FRIEND_SERIALIZE(Export);
};

WASM_DECLARE_CACHEABLE_POD(Export::CacheablePod);

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

struct FuncDesc {
  const FuncType* type;
  // Bit pack to keep this struct small on 32-bit systems
  uint32_t typeIndex : 24;
  FuncFlags flags : 8;

  // Assert that the bit packing scheme is viable
  static_assert(MaxTypes <= (1 << 24) - 1);
  static_assert(sizeof(FuncFlags) == sizeof(uint8_t));

  FuncDesc() = default;
  FuncDesc(const FuncType* type, uint32_t typeIndex)
      : type(type), typeIndex(typeIndex), flags(FuncFlags::None) {}

  bool isExported() const {
    return uint8_t(flags) & uint8_t(FuncFlags::Exported);
  }
  bool isEager() const { return uint8_t(flags) & uint8_t(FuncFlags::Eager); }
  bool canRefFunc() const {
    return uint8_t(flags) & uint8_t(FuncFlags::CanRefFunc);
  }
};

using FuncDescVector = Vector<FuncDesc, 0, SystemAllocPolicy>;

enum class BranchHint : uint8_t { Unlikely = 0, Likely = 1, Invalid = 2 };

// Stores pairs of <BranchOffset, BranchHint>
struct BranchHintEntry {
  uint32_t branchOffset;
  BranchHint value;

  BranchHintEntry() = default;
  BranchHintEntry(uint32_t branchOffset, BranchHint value)
      : branchOffset(branchOffset), value(value) {}
};

// Branch hint sorted vector for a function,
// stores tuples of <BranchOffset, BranchHint>
using BranchHintVector = Vector<BranchHintEntry, 0, SystemAllocPolicy>;

struct BranchHintCollection {
 private:
  // Map from function index to their collection of branch hints
  HashMap<uint32_t, BranchHintVector, DefaultHasher<uint32_t>,
          SystemAllocPolicy>
      branchHintsMap;

 public:
  // Used for lookups into the collection if a function
  // doesn't contain any hints.
  static BranchHintVector invalidVector;

  // Add all the branch hints for a function
  [[nodiscard]] bool addHintsForFunc(uint32_t functionIndex,
                                     BranchHintVector&& branchHints) {
    return branchHintsMap.put(functionIndex, std::move(branchHints));
  }

  // Return the vector with branch hints for a funcIndex.
  // If this function doesn't contain any hints, return an empty vector.
  BranchHintVector& getHintVector(uint32_t funcIndex) const {
    if (auto hintsVector = branchHintsMap.readonlyThreadsafeLookup(funcIndex)) {
      return hintsVector->value();
    }

    // If not found, return the empty invalid Vector
    return invalidVector;
  }

  bool isEmpty() const { return branchHintsMap.empty(); }
};

enum class GlobalKind { Import, Constant, Variable };

// A GlobalDesc describes a single global variable.
//
// wasm can import and export mutable and immutable globals.
//
// asm.js can import mutable and immutable globals, but a mutable global has a
// location that is private to the module, and its initial value is copied into
// that cell from the environment.  asm.js cannot export globals.
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
      : kind_((!isMutable && initial.isLiteral()) ? GlobalKind::Constant
                                                  : GlobalKind::Variable) {
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

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  WASM_DECLARE_FRIEND_SERIALIZE(GlobalDesc);
};

using GlobalDescVector = Vector<GlobalDesc, 0, SystemAllocPolicy>;

// A TagDesc represents fresh per-instance tags that are used for the
// exception handling proposal and potentially other future proposals.

// The TagOffsetVector represents the offsets in the layout of the
// data buffer stored in a Wasm exception.
using TagOffsetVector = Vector<uint32_t, 2, SystemAllocPolicy>;

class TagType : public AtomicRefCounted<TagType> {
  ValTypeVector argTypes_;
  TagOffsetVector argOffsets_;
  uint32_t size_;

 public:
  TagType() : size_(0) {}
  ~TagType();

  const ValTypeVector& argTypes() const { return argTypes_; }
  const TagOffsetVector& argOffsets() const { return argOffsets_; }
  ResultType resultType() const { return ResultType::Vector(argTypes_); }

  uint32_t tagSize() const { return size_; }

  [[nodiscard]] bool initialize(ValTypeVector&& argTypes);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

using MutableTagType = RefPtr<TagType>;
using SharedTagType = RefPtr<const TagType>;

struct TagDesc {
  TagKind kind;
  SharedTagType type;
  bool isExport;

  TagDesc() : isExport(false) {}
  TagDesc(TagKind kind, const SharedTagType& type, bool isExport = false)
      : kind(kind), type(type), isExport(isExport) {}

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

using TagDescVector = Vector<TagDesc, 0, SystemAllocPolicy>;
using ElemExprOffsetVector = Vector<size_t, 0, SystemAllocPolicy>;

struct ModuleElemSegment {
  enum class Kind {
    Active,
    Passive,
    Declared,
  };

  // The type of encoding used by this element segment. 0 is an invalid value to
  // make sure we notice if we fail to correctly initialize the element segment
  // - reading from the wrong representation could be a bad time.
  enum class Encoding {
    Indices = 1,
    Expressions,
  };

  struct Expressions {
    size_t count = 0;
    Bytes exprBytes;

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  };

  Kind kind;
  uint32_t tableIndex;
  RefType elemType;
  Maybe<InitExpr> offsetIfActive;

  // We store either an array of indices or the full bytecode of the element
  // expressions, depending on the encoding used for the element segment.
  Encoding encoding;
  Uint32Vector elemIndices;
  Expressions elemExpressions;

  bool active() const { return kind == Kind::Active; }

  const InitExpr& offset() const { return *offsetIfActive; }

  size_t numElements() const {
    switch (encoding) {
      case Encoding::Indices:
        return elemIndices.length();
      case Encoding::Expressions:
        return elemExpressions.count;
      default:
        MOZ_CRASH("unknown element segment encoding");
    }
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

using ModuleElemSegmentVector = Vector<ModuleElemSegment, 0, SystemAllocPolicy>;

using InstanceElemSegment = GCVector<HeapPtr<AnyRef>, 0, SystemAllocPolicy>;
using InstanceElemSegmentVector =
    GCVector<InstanceElemSegment, 0, SystemAllocPolicy>;

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

constexpr uint32_t InvalidMemoryIndex = UINT32_MAX;
static_assert(InvalidMemoryIndex > MaxMemories, "Invariant");

struct DataSegmentEnv {
  uint32_t memoryIndex;
  Maybe<InitExpr> offsetIfActive;
  uint32_t bytecodeOffset;
  uint32_t length;
};

using DataSegmentEnvVector = Vector<DataSegmentEnv, 0, SystemAllocPolicy>;

struct DataSegment : AtomicRefCounted<DataSegment> {
  uint32_t memoryIndex;
  Maybe<InitExpr> offsetIfActive;
  Bytes bytes;

  DataSegment() = default;

  bool active() const { return !!offsetIfActive; }

  const InitExpr& offset() const { return *offsetIfActive; }

  [[nodiscard]] bool init(const ShareableBytes& bytecode,
                          const DataSegmentEnv& src) {
    memoryIndex = src.memoryIndex;
    if (src.offsetIfActive) {
      offsetIfActive.emplace();
      if (!offsetIfActive->clone(*src.offsetIfActive)) {
        return false;
      }
    }
    return bytes.append(bytecode.begin() + src.bytecodeOffset, src.length);
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

using MutableDataSegment = RefPtr<DataSegment>;
using SharedDataSegment = RefPtr<const DataSegment>;
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

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
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

  WASM_CHECK_CACHEABLE_POD(offsetInNamePayload, length);

  Name() : offsetInNamePayload(UINT32_MAX), length(0) {}
};

WASM_DECLARE_CACHEABLE_POD(Name);

using NameVector = Vector<Name, 0, SystemAllocPolicy>;

// The kind of limits to decode or convert from JS.

enum class LimitsKind {
  Memory,
  Table,
};

// Represents the resizable limits of memories and tables.

struct Limits {
  // `indexType` will always be I32 for tables, but may be I64 for memories
  // when memory64 is enabled.
  IndexType indexType;

  // The initial and maximum limit. The unit is pages for memories and elements
  // for tables.
  uint64_t initial;
  Maybe<uint64_t> maximum;

  // `shared` is Shareable::False for tables but may be Shareable::True for
  // memories.
  Shareable shared;

  WASM_CHECK_CACHEABLE_POD(indexType, initial, maximum, shared);

  Limits() = default;
  explicit Limits(uint64_t initial, const Maybe<uint64_t>& maximum = Nothing(),
                  Shareable shared = Shareable::False)
      : indexType(IndexType::I32),
        initial(initial),
        maximum(maximum),
        shared(shared) {}
};

WASM_DECLARE_CACHEABLE_POD(Limits);

// MemoryDesc describes a memory.

struct MemoryDesc {
  Limits limits;

  WASM_CHECK_CACHEABLE_POD(limits);

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

  IndexType indexType() const { return limits.indexType; }

  // The initial length of this memory in pages.
  Pages initialPages() const { return Pages(limits.initial); }

  // The maximum length of this memory in pages.
  Maybe<Pages> maximumPages() const {
    return limits.maximum.map([](uint64_t x) { return Pages(x); });
  }

  // The initial length of this memory in bytes. Only valid for memory32.
  uint64_t initialLength32() const {
    MOZ_ASSERT(indexType() == IndexType::I32);
    // See static_assert after MemoryDesc for why this is safe.
    return limits.initial * PageSize;
  }

  uint64_t initialLength64() const {
    MOZ_ASSERT(indexType() == IndexType::I64);
    return limits.initial * PageSize;
  }

  MemoryDesc() {}
  explicit MemoryDesc(Limits limits) : limits(limits) {}
};

WASM_DECLARE_CACHEABLE_POD(MemoryDesc);

using MemoryDescVector = Vector<MemoryDesc, 1, SystemAllocPolicy>;

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
  bool isImported;
  bool isExported;
  bool isAsmJS;
  uint32_t initialLength;
  Maybe<uint32_t> maximumLength;
  Maybe<InitExpr> initExpr;

  TableDesc() = default;
  TableDesc(RefType elemType, uint32_t initialLength,
            Maybe<uint32_t> maximumLength, Maybe<InitExpr>&& initExpr,
            bool isAsmJS, bool isImported = false, bool isExported = false)
      : elemType(elemType),
        isImported(isImported),
        isExported(isExported),
        isAsmJS(isAsmJS),
        initialLength(initialLength),
        maximumLength(maximumLength),
        initExpr(std::move(initExpr)) {}
};

using TableDescVector = Vector<TableDesc, 0, SystemAllocPolicy>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_module_types_h
