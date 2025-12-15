/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

#ifndef wasm_code_h
#define wasm_code_h

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/RefPtr.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Span.h"
#include "mozilla/UniquePtr.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <utility>

#include "jstypes.h"

#include "gc/Memory.h"
#include "jit/ProcessExecutableMemory.h"
#include "js/AllocPolicy.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "threading/ExclusiveData.h"
#include "util/Memory.h"
#include "vm/MutexIDs.h"
#include "wasm/AsmJS.h"  // CodeMetadataForAsmJS::SeenSet
#include "wasm/WasmBuiltinModule.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCodegenConstants.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmExprType.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmLog.h"
#include "wasm/WasmMetadata.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmShareable.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"

struct JS_PUBLIC_API JSContext;
class JSFunction;

namespace js {

namespace jit {
class MacroAssembler;
};

namespace wasm {

// LinkData contains all the metadata necessary to patch all the locations
// that depend on the absolute address of a CodeSegment. This happens in a
// "linking" step after compilation and after the module's code is serialized.
// The LinkData is serialized along with the Module but does not (normally, see
// Module::debugLinkData_ comment) persist after (de)serialization, which
// distinguishes it from Metadata, which is stored in the Code object.

struct LinkDataCacheablePod {
  uint32_t trapOffset = 0;

  WASM_CHECK_CACHEABLE_POD(trapOffset);

  LinkDataCacheablePod() = default;
};

WASM_DECLARE_CACHEABLE_POD(LinkDataCacheablePod);

WASM_CHECK_CACHEABLE_POD_PADDING(LinkDataCacheablePod)

struct LinkData : LinkDataCacheablePod {
  LinkData() = default;

  LinkDataCacheablePod& pod() { return *this; }
  const LinkDataCacheablePod& pod() const { return *this; }

  struct InternalLink {
    uint32_t patchAtOffset;
    uint32_t targetOffset;
#ifdef JS_CODELABEL_LINKMODE
    uint32_t mode;
#endif

    WASM_CHECK_CACHEABLE_POD(patchAtOffset, targetOffset);
#ifdef JS_CODELABEL_LINKMODE
    WASM_CHECK_CACHEABLE_POD(mode)
#endif
  };
  using InternalLinkVector = Vector<InternalLink, 0, SystemAllocPolicy>;

  struct SymbolicLinkArray
      : mozilla::EnumeratedArray<SymbolicAddress, Uint32Vector,
                                 size_t(SymbolicAddress::Limit)> {
    bool isEmpty() const {
      for (const Uint32Vector& symbolicLinks : *this) {
        if (symbolicLinks.length() != 0) {
          return false;
        }
      }
      return true;
    }
    void clear() {
      for (SymbolicAddress symbolicAddress :
           mozilla::MakeEnumeratedRange(SymbolicAddress::Limit)) {
        (*this)[symbolicAddress].clear();
      }
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  };

  InternalLinkVector internalLinks;
  CallFarJumpVector callFarJumps;
  SymbolicLinkArray symbolicLinks;

  bool isEmpty() const {
    return internalLinks.length() == 0 && callFarJumps.length() == 0 &&
           symbolicLinks.isEmpty();
  }
  void clear() {
    internalLinks.clear();
    callFarJumps.clear();
    symbolicLinks.clear();
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

WASM_DECLARE_CACHEABLE_POD(LinkData::InternalLink);

using UniqueLinkData = UniquePtr<LinkData>;
using UniqueLinkDataVector = Vector<UniqueLinkData, 0, SystemAllocPolicy>;

// Executable code must be deallocated specially.

struct FreeCode {
  uint32_t codeLength;
  FreeCode() : codeLength(0) {}
  explicit FreeCode(uint32_t codeLength) : codeLength(codeLength) {}
  void operator()(uint8_t* codeBytes);
};

using UniqueCodeBytes = UniquePtr<uint8_t, FreeCode>;

class Code;
class CodeBlock;

using UniqueCodeBlock = UniquePtr<CodeBlock>;
using UniqueConstCodeBlock = UniquePtr<const CodeBlock>;
using UniqueConstCodeBlockVector =
    Vector<UniqueConstCodeBlock, 0, SystemAllocPolicy>;
using RawCodeBlockVector = Vector<const CodeBlock*, 0, SystemAllocPolicy>;

enum class CodeBlockKind {
  SharedStubs,
  BaselineTier,
  OptimizedTier,
  LazyStubs
};

// A source of machine code for creating an executable code segment.
class CodeSource {
  // The macro assembler to use as the source. If this is set then there is
  // no `bytes_` pointer.
  jit::MacroAssembler* masm_ = nullptr;
  // A raw pointer to the unlinked machine code bytes. If this is set then
  // there is no `masm_` pointer.
  const uint8_t* bytes_ = nullptr;

  // The length in bytes for either case. This is always valid and set to
  // masm.bytesNeeded() if masm_ is present.
  uint32_t length_ = 0;

  // The link data to use, if any. This is always present if we are linking
  // from raw bytes. Otherwise it may or may not be present when we are linking
  // masm. If it is not present for masm we will fall back to basic linking of
  // code labels and debug symbolic accesses.
  const LinkData* linkData_;

  // The code object to use, if any, for linking. This is optionally present
  // in either case. This will not be present if we are doing basic linking
  // without a link data.
  const Code* code_;

 public:
  // Get the machine code from a macro assembler, optional link data, and
  // optional code object.
  CodeSource(jit::MacroAssembler& masm, const LinkData* linkData,
             const Code* code);

  // Get the machine code from a raw bytes range, link data, and optional code
  // object.
  CodeSource(const uint8_t* bytes, uint32_t length, const LinkData& linkData,
             const Code* code);

  // The length of machine code in bytes.
  uint32_t lengthBytes() const { return length_; }

  // Copy and link the machine code into `codeStart`.
  bool copyAndLink(jit::AutoMarkJitCodeWritableForThread& writable,
                   uint8_t* codeStart) const;
};

// CodeSegment is a fixed-size chunk of executable memory that we can
// bump-allocate smaller allocations from.
class CodeSegment : public ShareableBase<CodeSegment> {
 private:
  const UniqueCodeBytes bytes_;
  uint32_t lengthBytes_;
  const uint32_t capacityBytes_;
  const Code* code_;

  // Create a new, empty code segment with a given capacity. The capacity must
  // have granularity of ExecutableCodePageSize (64KB).
  static RefPtr<CodeSegment> create(
      mozilla::Maybe<jit::AutoMarkJitCodeWritableForThread>& writable,
      size_t capacityBytes, bool allowLastDitchGC = true);

  // Returns the alignment that all allocations within a code segment must be.
  //
  // If we are write-protecting code, then we must start every new allocation
  // on a new system page, otherwise we can re-use system pages for new
  // allocations.
  static size_t AllocationAlignment();
  // Align `bytes` to be at least the allocation alignment. See above.
  static size_t AlignAllocationBytes(uintptr_t bytes);
  // Returns whether `bytes` is aligned to the allocation alignment.
  static bool IsAligned(uintptr_t bytes);

  // Checks if this code segment has enough room for an allocation of bytes.
  // The bytes must be aligned to allocation alignment.
  bool hasSpace(size_t bytes) const;

  // Claims space in this code segment for an allocation of bytes. The bytes
  // must be aligned to allocation alignment.
  void claimSpace(size_t bytes, uint8_t** claimedBase);

 public:
  CodeSegment(UniqueCodeBytes bytes, uint32_t lengthBytes,
              uint32_t capacityBytes)
      : bytes_(std::move(bytes)),
        lengthBytes_(lengthBytes),
        capacityBytes_(capacityBytes),
        code_(nullptr) {}

  // Copies, links, and makes the machine code executable from the given code
  // source. Returns the code segment the code was allocated into. An optional
  // pool of code segments may be provided to allocate from.
  //
  // There are two important ranges created, an 'allocation' range and a 'code
  // range'.
  //
  // The allocation range is a superset of the code range. The allocation start
  // offset will be aligned to `AllocationAlignment` which is either the system
  // page size or just executable code alignment.
  //
  // The code range will be within the allocation range and may have some
  // padding inserted before the start of the allocation. The code start offset
  // will always be aligned to the executable code alignment.
  //
  // Random padding is added before the code range when we are aligning to the
  // system page size, the start addressess of all the code memories will not
  // conflict in associative icaches.
  //
  // Here's a picture that illustrates the resulting structure of allocations:
  //
  // This is an example for a machine with a 4KB page size, for a codeLength
  // which requires more than one page but less than two, in a segment where
  // the first page is already allocated.
  //
  // Note: if !JitOptions.writeProtectCode, then allocationStart and
  //   allocationLength will be a multiple of jit::CodeAlignment, not the
  //   system page size.
  //
  // segment->base() (aligned at 4K = hardware page size)
  // :
  // :                      +4k                     +8k                    +12k
  // :                       :                       :                       :
  // +-----------------------+          +---------------------------------+   :
  // |        IN USE         |          |   CODE              CODE        |   :
  // +-----------------------+----------+---------------------------------+---+
  // .                       :          :                                 :
  // :                       :          :     allocationLength            :
  // :                       :<------------------------------------------>:
  // .                       :          :                                 :
  // :                       :  padding :           codeLength            :
  // :<--------------------->:<-------->:<------------------------------->:
  // :                       :          :
  // :                       :          :
  // :<-------------------------------->:
  //                         :          :
  //                         :          codeStart
  //                         :
  //                         allocationStart
  static RefPtr<CodeSegment> allocate(
      const CodeSource& codeSource,
      Vector<RefPtr<CodeSegment>, 0, SystemAllocPolicy>* segmentPool,
      bool allowLastDitchGC, uint8_t** codeStartOut,
      uint32_t* allocationLengthOut);

  void setCode(const Code& code) { code_ = &code; }

  uint8_t* base() const { return bytes_.get(); }
  uint32_t lengthBytes() const {
    MOZ_ASSERT(lengthBytes_ != UINT32_MAX);
    return lengthBytes_;
  }
  uint32_t capacityBytes() const {
    MOZ_ASSERT(capacityBytes_ != UINT32_MAX);
    return capacityBytes_;
  }

  const Code& code() const { return *code_; }

  void addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf, size_t* code,
                     size_t* data) const;
  WASM_DECLARE_FRIEND_SERIALIZE(CodeSegment);
};

using SharedCodeSegment = RefPtr<CodeSegment>;
using SharedCodeSegmentVector = Vector<SharedCodeSegment, 0, SystemAllocPolicy>;

extern UniqueCodeBytes AllocateCodeBytes(
    mozilla::Maybe<jit::AutoMarkJitCodeWritableForThread>& writable,
    uint32_t codeLength, bool allowLastDitchGC);
extern bool StaticallyLink(jit::AutoMarkJitCodeWritableForThread& writable,
                           uint8_t* base, const LinkData& linkData,
                           const Code* maybeCode);
extern void StaticallyUnlink(uint8_t* base, const LinkData& linkData);

enum class TierUpState : uint32_t {
  NotRequested,
  Requested,
  Finished,
};

struct FuncState {
  mozilla::Atomic<const CodeBlock*> bestTier;
  mozilla::Atomic<TierUpState> tierUpState;
};
using FuncStatesPointer = mozilla::UniquePtr<FuncState[], JS::FreePolicy>;

// LazyFuncExport helps to efficiently lookup a CodeRange from a given function
// index. It is inserted in a vector sorted by function index, to perform
// binary search on it later.

struct LazyFuncExport {
  size_t funcIndex;
  size_t lazyStubBlockIndex;
  size_t funcCodeRangeIndex;
  // Used to make sure we only upgrade a lazy stub from baseline to ion.
  mozilla::DebugOnly<CodeBlockKind> funcKind;

  LazyFuncExport(size_t funcIndex, size_t lazyStubBlockIndex,
                 size_t funcCodeRangeIndex, CodeBlockKind funcKind)
      : funcIndex(funcIndex),
        lazyStubBlockIndex(lazyStubBlockIndex),
        funcCodeRangeIndex(funcCodeRangeIndex),
        funcKind(funcKind) {}
};

using LazyFuncExportVector = Vector<LazyFuncExport, 0, SystemAllocPolicy>;

// A FuncExport represents a single function definition inside a wasm Module
// that has been exported one or more times. A FuncExport represents an
// internal entry point that can be called via function definition index by
// Instance::callExport(). To allow O(log(n)) lookup of a FuncExport by
// function definition index, the FuncExportVector is stored sorted by
// function definition index.

class FuncExport {
  uint32_t funcIndex_;
  uint32_t eagerInterpEntryOffset_;  // Machine code offset

  WASM_CHECK_CACHEABLE_POD(funcIndex_, eagerInterpEntryOffset_);

  // Sentinel value that this FuncExport will get eager stubs
  static constexpr uint32_t PENDING_EAGER_STUBS = UINT32_MAX - 1;

  // Sentinel value that this FuncExport will not eager stubs
  static constexpr uint32_t NO_EAGER_STUBS = UINT32_MAX;

 public:
  FuncExport() = default;
  explicit FuncExport(uint32_t funcIndex, bool hasEagerStubs) {
    funcIndex_ = funcIndex;
    eagerInterpEntryOffset_ =
        hasEagerStubs ? PENDING_EAGER_STUBS : NO_EAGER_STUBS;
  }
  void initEagerInterpEntryOffset(uint32_t entryOffset) {
    MOZ_ASSERT(eagerInterpEntryOffset_ == PENDING_EAGER_STUBS);
    MOZ_ASSERT(entryOffset != PENDING_EAGER_STUBS &&
               entryOffset != NO_EAGER_STUBS);
    MOZ_ASSERT(hasEagerStubs());
    eagerInterpEntryOffset_ = entryOffset;
  }

  bool hasEagerStubs() const {
    return eagerInterpEntryOffset_ != NO_EAGER_STUBS;
  }
  uint32_t funcIndex() const { return funcIndex_; }
  uint32_t eagerInterpEntryOffset() const {
    MOZ_ASSERT(eagerInterpEntryOffset_ != PENDING_EAGER_STUBS);
    MOZ_ASSERT(hasEagerStubs());
    return eagerInterpEntryOffset_;
  }
  void offsetBy(uint32_t delta) {
    if (hasEagerStubs()) {
      eagerInterpEntryOffset_ += delta;
    }
  }
};

WASM_DECLARE_CACHEABLE_POD(FuncExport);

using FuncExportVector = Vector<FuncExport, 0, SystemAllocPolicy>;

// A FuncImport contains the runtime metadata needed to implement a call to an
// imported function. Each function import has two call stubs: an optimized path
// into JIT code and a slow path into the generic C++ js::Invoke and these
// offsets of these stubs are stored so that function-import callsites can be
// dynamically patched at runtime.

class FuncImport {
 private:
  uint32_t interpExitCodeOffset_;  // Machine code offset
  uint32_t jitExitCodeOffset_;     // Machine code offset

  WASM_CHECK_CACHEABLE_POD(interpExitCodeOffset_, jitExitCodeOffset_);

 public:
  FuncImport() : interpExitCodeOffset_(0), jitExitCodeOffset_(0) {}

  void initInterpExitOffset(uint32_t off) {
    MOZ_ASSERT(!interpExitCodeOffset_);
    interpExitCodeOffset_ = off;
  }
  void initJitExitOffset(uint32_t off) {
    MOZ_ASSERT(!jitExitCodeOffset_);
    jitExitCodeOffset_ = off;
  }

  uint32_t interpExitCodeOffset() const { return interpExitCodeOffset_; }
  uint32_t jitExitCodeOffset() const { return jitExitCodeOffset_; }
};

WASM_DECLARE_CACHEABLE_POD(FuncImport)

using FuncImportVector = Vector<FuncImport, 0, SystemAllocPolicy>;

static const uint32_t BAD_CODE_RANGE = UINT32_MAX;

class FuncToCodeRangeMap {
  uint32_t startFuncIndex_ = 0;
  Uint32Vector funcToCodeRange_;

  bool denseHasFuncIndex(uint32_t funcIndex) const {
    return funcIndex >= startFuncIndex_ &&
           funcIndex - startFuncIndex_ < funcToCodeRange_.length();
  }

  FuncToCodeRangeMap(uint32_t startFuncIndex, Uint32Vector&& funcToCodeRange)
      : startFuncIndex_(startFuncIndex),
        funcToCodeRange_(std::move(funcToCodeRange)) {}

 public:
  [[nodiscard]] static bool createDense(uint32_t startFuncIndex,
                                        uint32_t numFuncs,
                                        FuncToCodeRangeMap* result) {
    Uint32Vector funcToCodeRange;
    if (!funcToCodeRange.appendN(BAD_CODE_RANGE, numFuncs)) {
      return false;
    }
    *result = FuncToCodeRangeMap(startFuncIndex, std::move(funcToCodeRange));
    return true;
  }

  FuncToCodeRangeMap() = default;
  FuncToCodeRangeMap(FuncToCodeRangeMap&& rhs) = default;
  FuncToCodeRangeMap& operator=(FuncToCodeRangeMap&& rhs) = default;
  FuncToCodeRangeMap(const FuncToCodeRangeMap& rhs) = delete;
  FuncToCodeRangeMap& operator=(const FuncToCodeRangeMap& rhs) = delete;

  uint32_t lookup(uint32_t funcIndex) const {
    if (!denseHasFuncIndex(funcIndex)) {
      return BAD_CODE_RANGE;
    }
    return funcToCodeRange_[funcIndex - startFuncIndex_];
  }

  uint32_t operator[](uint32_t funcIndex) const { return lookup(funcIndex); }

  [[nodiscard]] bool insert(uint32_t funcIndex, uint32_t codeRangeIndex) {
    if (!denseHasFuncIndex(funcIndex)) {
      return false;
    }
    funcToCodeRange_[funcIndex - startFuncIndex_] = codeRangeIndex;
    return true;
  }
  void insertInfallible(uint32_t funcIndex, uint32_t codeRangeIndex) {
    bool result = insert(funcIndex, codeRangeIndex);
    MOZ_RELEASE_ASSERT(result);
  }

  void shrinkStorageToFit() { funcToCodeRange_.shrinkStorageToFit(); }

  void assertAllInitialized() {
#ifdef DEBUG
    for (uint32_t codeRangeIndex : funcToCodeRange_) {
      MOZ_ASSERT(codeRangeIndex != BAD_CODE_RANGE);
    }
#endif
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return funcToCodeRange_.sizeOfExcludingThis(mallocSizeOf);
  }

  size_t numEntries() const { return funcToCodeRange_.length(); }

  WASM_DECLARE_FRIEND_SERIALIZE(FuncToCodeRangeMap);
};

// CodeBlock contains all the data related to a given compilation tier. It is
// built during module generation and then immutably stored in a Code.
//
// Code contains a map from PC to containing code block. The map is thread-safe
// to support lookups from multiple threads (see ThreadSafeCodeBlockMap). This
// is safe because code blocks are immutable after creation, so there won't
// be any concurrent modification during a metadata lookup.

class CodeBlock {
 public:
  // Weak reference to the code that owns us, not serialized.
  const Code* code;
  // The index we are held inside our containing Code::data::blocks_ vector.
  size_t codeBlockIndex;

  // The following information is all serialized
  // Which kind of code is being stored in this block. Most consumers don't
  // care about this.
  const CodeBlockKind kind;

  // The code segment our JIT code is within.
  SharedCodeSegment segment;

  // Pointer to the beginning of the CodeBlock.
  uint8_t* codeBase;
  size_t codeLength;

  // Metadata about the code we have contributed to the segment.
  //
  // All offsets are relative to `codeBase` not the segment base.
  FuncToCodeRangeMap funcToCodeRange;
  CodeRangeVector codeRanges;
  InliningContext inliningContext;
  CallSites callSites;
  TrapSites trapSites;
  FuncExportVector funcExports;
  StackMaps stackMaps;
  TryNoteVector tryNotes;
  CodeRangeUnwindInfoVector codeRangeUnwindInfos;

  // Track whether we are registered in the process map of code blocks.
  bool unregisterOnDestroy_;

  static constexpr CodeBlockKind kindFromTier(Tier tier) {
    if (tier == Tier::Optimized) {
      return CodeBlockKind::OptimizedTier;
    }
    MOZ_ASSERT(tier == Tier::Baseline);
    return CodeBlockKind::BaselineTier;
  }

  explicit CodeBlock(CodeBlockKind kind)
      : code(nullptr),
        codeBlockIndex((size_t)-1),
        kind(kind),
        codeBase(nullptr),
        codeLength(0),
        unregisterOnDestroy_(false) {}
  ~CodeBlock();

  bool initialized() const {
    if (code) {
      // Initialize should have given us an index too.
      MOZ_ASSERT(codeBlockIndex != (size_t)-1);
      return true;
    }
    return false;
  }

  bool initialize(const Code& code, size_t codeBlockIndex);
  void sendToProfiler(const CodeMetadata& codeMeta,
                      const CodeTailMetadata& codeTailMeta,
                      const CodeMetadataForAsmJS* codeMetaForAsmJS,
                      FuncIonPerfSpewerSpan ionSpewers,
                      FuncBaselinePerfSpewerSpan baselineSpewers) const;

  // Gets the tier for this code block. Only valid for non-lazy stub code.
  Tier tier() const {
    switch (kind) {
      case CodeBlockKind::BaselineTier:
        return Tier::Baseline;
      case CodeBlockKind::OptimizedTier:
        return Tier::Optimized;
      default:
        MOZ_CRASH();
    }
  }

  // Returns whether this code block should be considered for serialization.
  bool isSerializable() const {
    return kind == CodeBlockKind::SharedStubs ||
           kind == CodeBlockKind::OptimizedTier;
  }

  uint8_t* base() const { return codeBase; }
  uint32_t length() const { return codeLength; }
  bool containsCodePC(const void* pc) const {
    return pc >= base() && pc < (base() + length());
  }

  const CodeRange& codeRange(uint32_t funcIndex) const {
    return codeRanges[funcToCodeRange[funcIndex]];
  }
  const CodeRange& codeRange(const FuncExport& funcExport) const {
    return codeRanges[funcToCodeRange[funcExport.funcIndex()]];
  }

  const CodeRange* lookupRange(const void* pc) const;
  bool lookupCallSite(void* pc, CallSite* callSite) const;
  const StackMap* lookupStackMap(uint8_t* pc) const;
  const TryNote* lookupTryNote(const void* pc) const;
  bool lookupTrap(void* pc, Trap* kindOut, TrapSite* trapOut) const;
  const CodeRangeUnwindInfo* lookupUnwindInfo(void* pc) const;
  FuncExport& lookupFuncExport(uint32_t funcIndex,
                               size_t* funcExportIndex = nullptr);
  const FuncExport& lookupFuncExport(uint32_t funcIndex,
                                     size_t* funcExportIndex = nullptr) const;

  void disassemble(JSContext* cx, int kindSelection,
                   PrintCallback printString) const;

  void addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf, size_t* code,
                     size_t* data) const;

  WASM_DECLARE_FRIEND_SERIALIZE_ARGS(CodeBlock, const wasm::LinkData& data);
};

// Because of profiling, the thread running wasm might need to know to which
// CodeBlock the current PC belongs, during a call to lookup(). A lookup
// is a read-only operation, and we don't want to take a lock then
// (otherwise, we could have a deadlock situation if an async lookup
// happened on a given thread that was holding mutatorsMutex_ while getting
// sampled). Since the writer could be modifying the data that is getting
// looked up, the writer functions use spin-locks to know if there are any
// observers (i.e. calls to lookup()) of the atomic data.

class ThreadSafeCodeBlockMap {
  // Since writes (insertions or removals) can happen on any background
  // thread at the same time, we need a lock here.

  Mutex mutatorsMutex_ MOZ_UNANNOTATED;

  RawCodeBlockVector segments1_;
  RawCodeBlockVector segments2_;

  // Except during swapAndWait(), there are no lookup() observers of the
  // vector pointed to by mutableCodeBlocks_

  RawCodeBlockVector* mutableCodeBlocks_;
  mozilla::Atomic<const RawCodeBlockVector*> readonlyCodeBlocks_;
  mozilla::Atomic<size_t> numActiveLookups_;

  struct CodeBlockPC {
    const void* pc;
    explicit CodeBlockPC(const void* pc) : pc(pc) {}
    int operator()(const CodeBlock* cb) const {
      if (cb->containsCodePC(pc)) {
        return 0;
      }
      if (pc < cb->base()) {
        return -1;
      }
      return 1;
    }
  };

  void swapAndWait() {
    // Both vectors are consistent for lookup at this point although their
    // contents are different: there is no way for the looked up PC to be
    // in the code segment that is getting registered, because the code
    // segment is not even fully created yet.

    // If a lookup happens before this instruction, then the
    // soon-to-become-former read-only pointer is used during the lookup,
    // which is valid.

    mutableCodeBlocks_ = const_cast<RawCodeBlockVector*>(
        readonlyCodeBlocks_.exchange(mutableCodeBlocks_));

    // If a lookup happens after this instruction, then the updated vector
    // is used, which is valid:
    // - in case of insertion, it means the new vector contains more data,
    // but it's fine since the code segment is getting registered and thus
    // isn't even fully created yet, so the code can't be running.
    // - in case of removal, it means the new vector contains one less
    // entry, but it's fine since unregistering means the code segment
    // isn't used by any live instance anymore, thus PC can't be in the
    // to-be-removed code segment's range.

    // A lookup could have happened on any of the two vectors. Wait for
    // observers to be done using any vector before mutating.

    while (numActiveLookups_ > 0) {
    }
  }

 public:
  ThreadSafeCodeBlockMap()
      : mutatorsMutex_(mutexid::WasmCodeBlockMap),
        mutableCodeBlocks_(&segments1_),
        readonlyCodeBlocks_(&segments2_),
        numActiveLookups_(0) {}

  ~ThreadSafeCodeBlockMap() {
    MOZ_RELEASE_ASSERT(numActiveLookups_ == 0);
    segments1_.clearAndFree();
    segments2_.clearAndFree();
  }

  size_t numActiveLookups() const { return numActiveLookups_; }

  bool insert(const CodeBlock* cs) {
    LockGuard<Mutex> lock(mutatorsMutex_);

    size_t index;
    MOZ_ALWAYS_FALSE(BinarySearchIf(*mutableCodeBlocks_, 0,
                                    mutableCodeBlocks_->length(),
                                    CodeBlockPC(cs->base()), &index));

    if (!mutableCodeBlocks_->insert(mutableCodeBlocks_->begin() + index, cs)) {
      return false;
    }

    swapAndWait();

#ifdef DEBUG
    size_t otherIndex;
    MOZ_ALWAYS_FALSE(BinarySearchIf(*mutableCodeBlocks_, 0,
                                    mutableCodeBlocks_->length(),
                                    CodeBlockPC(cs->base()), &otherIndex));
    MOZ_ASSERT(index == otherIndex);
#endif

    // Although we could simply revert the insertion in the read-only
    // vector, it is simpler to just crash and given that each CodeBlock
    // consumes multiple pages, it is unlikely this insert() would OOM in
    // practice
    AutoEnterOOMUnsafeRegion oom;
    if (!mutableCodeBlocks_->insert(mutableCodeBlocks_->begin() + index, cs)) {
      oom.crash("when inserting a CodeBlock in the process-wide map");
    }

    return true;
  }

  size_t remove(const CodeBlock* cs) {
    LockGuard<Mutex> lock(mutatorsMutex_);

    size_t index;
    MOZ_ALWAYS_TRUE(BinarySearchIf(*mutableCodeBlocks_, 0,
                                   mutableCodeBlocks_->length(),
                                   CodeBlockPC(cs->base()), &index));

    mutableCodeBlocks_->erase(mutableCodeBlocks_->begin() + index);
    size_t newCodeBlockCount = mutableCodeBlocks_->length();

    swapAndWait();

#ifdef DEBUG
    size_t otherIndex;
    MOZ_ALWAYS_TRUE(BinarySearchIf(*mutableCodeBlocks_, 0,
                                   mutableCodeBlocks_->length(),
                                   CodeBlockPC(cs->base()), &otherIndex));
    MOZ_ASSERT(index == otherIndex);
#endif

    mutableCodeBlocks_->erase(mutableCodeBlocks_->begin() + index);
    return newCodeBlockCount;
  }

  const CodeBlock* lookup(const void* pc,
                          const CodeRange** codeRange = nullptr) {
    auto decObserver = mozilla::MakeScopeExit([&] {
      MOZ_ASSERT(numActiveLookups_ > 0);
      numActiveLookups_--;
    });
    numActiveLookups_++;

    const RawCodeBlockVector* readonly = readonlyCodeBlocks_;

    size_t index;
    if (!BinarySearchIf(*readonly, 0, readonly->length(), CodeBlockPC(pc),
                        &index)) {
      if (codeRange) {
        *codeRange = nullptr;
      }
      return nullptr;
    }

    // It is fine returning a raw CodeBlock*, because we assume we are
    // looking up a live PC in code which is on the stack, keeping the
    // CodeBlock alive.

    const CodeBlock* result = (*readonly)[index];
    if (codeRange) {
      *codeRange = result->lookupRange(pc);
    }
    return result;
  }
};

// Jump tables that implement function tiering and fast js-to-wasm calls.
//
// There is one JumpTable object per Code object, holding two jump tables: the
// tiering jump table and the jit-entry jump table.  The JumpTable is not
// serialized with its Code, but is a run-time entity only.  At run-time it is
// shared across threads with its owning Code (and the Module that owns the
// Code).  Values in the JumpTable /must/ /always/ be JSContext-agnostic and
// Instance-agnostic, because of this sharing.
//
// Both jump tables have a number of entries equal to the number of functions in
// their Module, including imports.  In the tiering table, the elements
// corresponding to the Module's imported functions are unused; in the jit-entry
// table, the elements corresponding to the Module's non-exported functions are
// unused.  (Functions can be exported explicitly via the exports section or
// implicitly via a mention of their indices outside function bodies.)  See
// comments at JumpTables::init() and WasmInstanceObject::getExportedFunction().
// The entries are void*.  Unused entries are null.
//
// The tiering jump table.
//
// This table holds code pointers that are used by baseline functions to enter
// optimized code.  See the large comment block in WasmCompile.cpp for
// information about how tiering works.
//
// The jit-entry jump table.
//
// The jit-entry jump table entry for a function holds a stub that allows Jitted
// JS code to call wasm using the JS JIT ABI.  See large comment block at
// WasmInstanceObject::getExportedFunction() for more about exported functions
// and stubs and the lifecycle of the entries in the jit-entry table - there are
// complex invariants.

class JumpTables {
  using TablePointer = mozilla::UniquePtr<void*[], JS::FreePolicy>;

  CompileMode mode_;
  TablePointer tiering_;
  TablePointer jit_;
  size_t numFuncs_;

  static_assert(
      JumpTableJitEntryOffset == 0,
      "Each jit entry in table must have compatible layout with BaseScript and"
      "SelfHostedLazyScript");

 public:
  bool initialize(CompileMode mode, const CodeMetadata& codeMeta,
                  const CodeBlock& sharedStubs, const CodeBlock& tier1);

  void setJitEntry(size_t i, void* target) const;
  void setJitEntryIfNull(size_t i, void* target) const;
  void** getAddressOfJitEntry(size_t i) const {
    MOZ_ASSERT(i < numFuncs_);
    MOZ_ASSERT(jit_.get()[i]);
    return &jit_.get()[i];
  }
  uint32_t funcIndexFromJitEntry(void** target) const {
    MOZ_ASSERT(target >= &jit_.get()[0]);
    MOZ_ASSERT(target <= &(jit_.get()[numFuncs_ - 1]));
    size_t index = (intptr_t*)target - (intptr_t*)&jit_.get()[0];
    MOZ_ASSERT(index < wasm::MaxFuncs);
    return (uint32_t)index;
  }

  void setTieringEntry(size_t i, void* target) const {
    MOZ_ASSERT(i < numFuncs_);
    // See comment in wasm::Module::finishTier2.
    if (mode_ != CompileMode::Once) {
      tiering_.get()[i] = target;
    }
  }
  void** tiering() const { return tiering_.get(); }

  size_t sizeOfMiscExcludingThis() const {
    // 2 words per function for the jit entry table, plus maybe 1 per
    // function if we're tiering.
    return sizeof(void*) * (2 + (tiering_ ? 1 : 0)) * numFuncs_;
  }
};

// Code objects own executable code and the metadata that describe it. A single
// Code object is normally shared between a module and all its instances.
//
// profilingLabels_ is lazily initialized, but behind a lock.

using SharedCode = RefPtr<const Code>;
using MutableCode = RefPtr<Code>;
using MetadataAnalysisHashMap =
    HashMap<const char*, uint32_t, mozilla::CStringHasher, SystemAllocPolicy>;

class Code : public ShareableBase<Code> {
  struct ProtectedData {
    // A vector of all of the code blocks owned by this code. Each code block
    // is immutable once added to the vector, but this vector may grow.
    UniqueConstCodeBlockVector blocks;
    // A vector of link data paired 1:1 with `blocks`. Entries may be null if
    // the code block is not serializable. This is separate from CodeBlock so
    // that we may clear it out after serialization has happened.
    UniqueLinkDataVector blocksLinkData;

    // A vector of code segments that we can allocate lazy segments into
    SharedCodeSegmentVector lazyStubSegments;
    // A sorted vector of LazyFuncExport
    LazyFuncExportVector lazyExports;

    // A vector of code segments that we can lazily allocate functions into
    SharedCodeSegmentVector lazyFuncSegments;

    // Statistics for tiers of code.
    CompileAndLinkStats tier1Stats;
    CompileAndLinkStats tier2Stats;
  };
  using ReadGuard = RWExclusiveData<ProtectedData>::ReadGuard;
  using WriteGuard = RWExclusiveData<ProtectedData>::WriteGuard;

  // The compile mode this code is used with.
  const CompileMode mode_;

  // Core data that is not thread-safe and must acquire a lock in order to
  // access.
  RWExclusiveData<ProtectedData> data_;

  // Thread-safe mutable map from code pointer to code block that contains it.
  mutable ThreadSafeCodeBlockMap blockMap_;

  // Metadata for this module that is needed for the lifetime of Code. This is
  // always non-null.
  SharedCodeMetadata codeMeta_;
  // Metadata for this module that is needed for the lifetime of Code, and is
  // only available after the whole module has been decoded. This is always
  // non-null.
  SharedCodeTailMetadata codeTailMeta_;
  // This is null for a wasm module, non-null for asm.js
  SharedCodeMetadataForAsmJS codeMetaForAsmJS_;

  const CodeBlock* sharedStubs_;
  const CodeBlock* completeTier1_;

  // [SMDOC] Tier-2 data
  //
  // hasCompleteTier2_ and completeTier2_ implement a three-state protocol for
  // broadcasting tier-2 data; this also amounts to a single-writer/
  // multiple-reader setup.
  //
  // Initially hasCompleteTier2_ is false and completeTier2_ is null.
  //
  // While hasCompleteTier2_ is false, *no* thread may read completeTier2_, but
  // one thread may make completeTier2_ non-null (this will be the tier-2
  // compiler thread).  That same thread must then later set hasCompleteTier2_
  // to true to broadcast the completeTier2_ value and its availability.  Note
  // that the writing thread may not itself read completeTier2_ before setting
  // hasCompleteTier2_, in order to simplify reasoning about global invariants.
  //
  // Once hasCompleteTier2_ is true, *no* thread may write completeTier2_ and
  // *no* thread may read completeTier2_ without having observed
  // hasCompleteTier2_ as true first.  Once hasCompleteTier2_ is true, it stays
  // true.
  mutable const CodeBlock* completeTier2_;
  mutable mozilla::Atomic<bool> hasCompleteTier2_;

  // State for every defined function (not imported) in this module. This is
  // only needed if we're doing partial tiering.
  mutable FuncStatesPointer funcStates_;

  FuncImportVector funcImports_;
  ExclusiveData<CacheableCharsVector> profilingLabels_;
  JumpTables jumpTables_;

  // Where to redirect PC to for handling traps from the signal handler.
  uint8_t* trapCode_;

  // Offset of the debug stub in the `sharedStubs_` CodeBlock.  Not serialized.
  uint32_t debugStubOffset_;

  // Offset of the request-tier-up stub in the `sharedStubs_` CodeBlock.
  uint32_t requestTierUpStubOffset_;

  // Offset of the update-call-ref-metrics stub in the `sharedStubs_`
  // CodeBlock.
  uint32_t updateCallRefMetricsStubOffset_;

  // Methods for getting complete tiers, private while we're moving to partial
  // tiering.
  Tiers completeTiers() const;

  [[nodiscard]] bool addCodeBlock(const WriteGuard& guard,
                                  UniqueCodeBlock block,
                                  UniqueLinkData maybeLinkData) const;

  [[nodiscard]] const LazyFuncExport* lookupLazyFuncExport(
      const WriteGuard& guard, uint32_t funcIndex) const;

  // Returns a pointer to the raw interpreter entry of a given function for
  // which stubs have been lazily generated.
  [[nodiscard]] void* lookupLazyInterpEntry(const WriteGuard& guard,
                                            uint32_t funcIndex) const;

  [[nodiscard]] bool createOneLazyEntryStub(const WriteGuard& guard,
                                            uint32_t funcExportIndex,
                                            const CodeBlock& tierCodeBlock,
                                            void** interpEntry) const;
  [[nodiscard]] bool createManyLazyEntryStubs(
      const WriteGuard& guard, const Uint32Vector& funcExportIndices,
      const CodeBlock& tierCodeBlock, size_t* stubBlockIndex) const;
  // Create one lazy stub for all the functions in funcExportIndices, putting
  // them in a single stub. Jit entries won't be used until
  // setJitEntries() is actually called, after the Code owner has committed
  // tier2.
  [[nodiscard]] bool createTier2LazyEntryStubs(
      const WriteGuard& guard, const CodeBlock& tier2Code,
      mozilla::Maybe<size_t>* outStubBlockIndex) const;
  [[nodiscard]] bool appendProfilingLabels(
      const ExclusiveData<CacheableCharsVector>::Guard& labels,
      const CodeBlock& codeBlock) const;

  void printStats() const;

 public:
  Code(CompileMode mode, const CodeMetadata& codeMeta,
       const CodeTailMetadata& codeTailMeta,
       const CodeMetadataForAsmJS* codeMetaForAsmJS);
  ~Code();

  [[nodiscard]] bool initialize(FuncImportVector&& funcImports,
                                UniqueCodeBlock sharedStubs,
                                UniqueLinkData sharedStubsLinkData,
                                UniqueCodeBlock tier1CodeBlock,
                                UniqueLinkData tier1LinkData,
                                const CompileAndLinkStats& tier1Stats);
  [[nodiscard]] bool finishTier2(UniqueCodeBlock tier2CodeBlock,
                                 UniqueLinkData tier2LinkData,
                                 const CompileAndLinkStats& tier2Stats) const;

  [[nodiscard]] bool getOrCreateInterpEntry(uint32_t funcIndex,
                                            const FuncExport** funcExport,
                                            void** interpEntry) const;

  SharedCodeSegment createFuncCodeSegmentFromPool(
      jit::MacroAssembler& masm, const LinkData& linkData,
      bool allowLastDitchGC, uint8_t** codeStartOut,
      uint32_t* codeLengthOut) const;

  bool requestTierUp(uint32_t funcIndex) const;

  CompileMode mode() const { return mode_; }

  void** tieringJumpTable() const { return jumpTables_.tiering(); }

  void setJitEntryIfNull(size_t i, void* target) const {
    jumpTables_.setJitEntryIfNull(i, target);
  }
  void** getAddressOfJitEntry(size_t i) const {
    return jumpTables_.getAddressOfJitEntry(i);
  }
  uint32_t funcIndexFromJitEntry(void** jitEntry) const {
    return jumpTables_.funcIndexFromJitEntry(jitEntry);
  }

  uint8_t* trapCode() const { return trapCode_; }

  uint32_t debugStubOffset() const { return debugStubOffset_; }
  void setDebugStubOffset(uint32_t offs) { debugStubOffset_ = offs; }

  uint32_t requestTierUpStubOffset() const { return requestTierUpStubOffset_; }
  void setRequestTierUpStubOffset(uint32_t offs) {
    requestTierUpStubOffset_ = offs;
  }

  uint32_t updateCallRefMetricsStubOffset() const {
    return updateCallRefMetricsStubOffset_;
  }
  void setUpdateCallRefMetricsStubOffset(uint32_t offs) {
    updateCallRefMetricsStubOffset_ = offs;
  }

  const FuncImport& funcImport(uint32_t funcIndex) const {
    return funcImports_[funcIndex];
  }
  const FuncImportVector& funcImports() const { return funcImports_; }

  bool hasCompleteTier(Tier tier) const;
  // The 'stable' complete tier of code. This is stable during a run/
  Tier stableCompleteTier() const;
  // The 'best' complete tier of code. This may transition from baseline to ion
  // at any time.
  Tier bestCompleteTier() const;
  bool hasSerializableCode() const { return hasCompleteTier(Tier::Serialized); }

  const CodeMetadata& codeMeta() const { return *codeMeta_; }
  const CodeMetadataForAsmJS* codeMetaForAsmJS() const {
    return codeMetaForAsmJS_;
  }
  const CodeTailMetadata& codeTailMeta() const { return *codeTailMeta_; }
  bool debugEnabled() const { return codeTailMeta_->debugEnabled; }

  const CodeBlock& sharedStubs() const { return *sharedStubs_; }
  const CodeBlock& debugCodeBlock() const {
    MOZ_ASSERT(debugEnabled());
    MOZ_ASSERT(completeTier1_->tier() == Tier::Debug);
    return *completeTier1_;
  }
  const CodeBlock& completeTierCodeBlock(Tier tier) const;
  const CodeBlock& funcCodeBlock(uint32_t funcIndex) const {
    if (funcIndex < funcImports_.length()) {
      return *sharedStubs_;
    }
    if (mode_ == CompileMode::LazyTiering) {
      return *funcStates_.get()[funcIndex - codeMeta_->numFuncImports].bestTier;
    }
    return completeTierCodeBlock(bestCompleteTier());
  }
  bool funcHasTier(uint32_t funcIndex, Tier tier) const {
    if (funcIndex < funcImports_.length()) {
      return false;
    }
    return funcCodeBlock(funcIndex).tier() == tier;
  }
  Tier funcTier(uint32_t funcIndex) const {
    MOZ_ASSERT(funcIndex >= funcImports_.length());
    return funcCodeBlock(funcIndex).tier();
  }
  void funcCodeRange(uint32_t funcIndex, const wasm::CodeRange** range,
                     uint8_t** codeBase) const {
    const CodeBlock& codeBlock = funcCodeBlock(funcIndex);
    *range = &codeBlock.codeRanges[codeBlock.funcToCodeRange[funcIndex]];
    *codeBase = codeBlock.base();
  }

  const LinkData* codeBlockLinkData(const CodeBlock& block) const;
  void clearLinkData() const;

  // Code metadata lookup:
  bool lookupCallSite(void* pc, CallSite* callSite) const {
    const CodeBlock* block = blockMap_.lookup(pc);
    if (!block) {
      return false;
    }
    return block->lookupCallSite(pc, callSite);
  }
  const CodeRange* lookupFuncRange(void* pc) const {
    const CodeBlock* block = blockMap_.lookup(pc);
    if (!block) {
      return nullptr;
    }
    const CodeRange* result = block->lookupRange(pc);
    if (result && result->isFunction()) {
      return result;
    }
    return nullptr;
  }
  const StackMap* lookupStackMap(uint8_t* pc) const {
    const CodeBlock* block = blockMap_.lookup(pc);
    if (!block) {
      return nullptr;
    }
    return block->lookupStackMap(pc);
  }
  const wasm::TryNote* lookupTryNote(void* pc, const CodeBlock** block) const {
    *block = blockMap_.lookup(pc);
    if (!*block) {
      return nullptr;
    }
    return (*block)->lookupTryNote(pc);
  }
  bool lookupTrap(void* pc, Trap* kindOut, TrapSite* trapOut) const {
    const CodeBlock* block = blockMap_.lookup(pc);
    if (!block) {
      return false;
    }
    return block->lookupTrap(pc, kindOut, trapOut);
  }
  const CodeRangeUnwindInfo* lookupUnwindInfo(void* pc) const {
    const CodeBlock* block = blockMap_.lookup(pc);
    if (!block) {
      return nullptr;
    }
    return block->lookupUnwindInfo(pc);
  }

  // To save memory, profilingLabels_ are generated lazily when profiling mode
  // is enabled.

  void ensureProfilingLabels(bool profilingEnabled) const;
  const char* profilingLabel(uint32_t funcIndex) const;

  // Wasm disassembly support

  void disassemble(JSContext* cx, Tier tier, int kindSelection,
                   PrintCallback printString) const;

  // Wasm metadata size analysis
  MetadataAnalysisHashMap metadataAnalysis(JSContext* cx) const;

  // about:memory reporting:

  void addSizeOfMiscIfNotSeen(
      mozilla::MallocSizeOf mallocSizeOf, CodeMetadata::SeenSet* seenCodeMeta,
      CodeMetadataForAsmJS::SeenSet* seenCodeMetaForAsmJS,
      Code::SeenSet* seenCode, size_t* code, size_t* data) const;

  size_t tier1CodeMemoryUsed() const {
    return completeTier1_->segment->capacityBytes();
  }

  WASM_DECLARE_FRIEND_SERIALIZE_ARGS(SharedCode,
                                     const wasm::LinkData& sharedStubsLinkData,
                                     const wasm::LinkData& optimizedLinkData);
};

void PatchDebugSymbolicAccesses(uint8_t* codeBase, jit::MacroAssembler& masm);

}  // namespace wasm
}  // namespace js

#endif  // wasm_code_h
