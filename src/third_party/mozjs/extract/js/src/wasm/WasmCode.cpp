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

#include "wasm/WasmCode.h"

#include "mozilla/Atomics.h"
#include "mozilla/BinarySearch.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/Sprintf.h"

#include <algorithm>

#include "jsnum.h"

#include "jit/Disassemble.h"
#include "jit/ExecutableAllocator.h"
#include "jit/FlushICache.h"  // for FlushExecutionContextForAllThreads
#include "jit/MacroAssembler.h"
#include "jit/PerfSpewer.h"
#include "util/Poison.h"
#include "vm/HelperThreadState.h"  // PartialTier2CompileTask
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif
#include "wasm/WasmModule.h"
#include "wasm/WasmProcess.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmUtility.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;
using mozilla::Atomic;
using mozilla::BinarySearch;
using mozilla::BinarySearchIf;
using mozilla::DebugOnly;
using mozilla::MakeEnumeratedRange;
using mozilla::MallocSizeOf;
using mozilla::Maybe;

size_t LinkData::SymbolicLinkArray::sizeOfExcludingThis(
    MallocSizeOf mallocSizeOf) const {
  size_t size = 0;
  for (const Uint32Vector& offsets : *this) {
    size += offsets.sizeOfExcludingThis(mallocSizeOf);
  }
  return size;
}

static uint32_t RoundupExecutableCodePageSize(uint32_t codeLength) {
  static_assert(MaxCodeBytesPerProcess <= INT32_MAX, "rounding won't overflow");
  // AllocateExecutableMemory() requires a multiple of ExecutableCodePageSize.
  return RoundUp(codeLength, ExecutableCodePageSize);
}

UniqueCodeBytes wasm::AllocateCodeBytes(
    Maybe<AutoMarkJitCodeWritableForThread>& writable, uint32_t codeLength,
    bool allowLastDitchGC) {
  if (codeLength > MaxCodeBytesPerProcess) {
    return nullptr;
  }

  MOZ_RELEASE_ASSERT(codeLength == RoundupExecutableCodePageSize(codeLength));
  void* p = AllocateExecutableMemory(codeLength, ProtectionSetting::Writable,
                                     MemCheckKind::MakeUndefined);

  // If the allocation failed and the embedding gives us a last-ditch attempt
  // to purge all memory (which, in gecko, does a purging GC/CC/GC), do that
  // then retry the allocation.
  if (!p && allowLastDitchGC) {
    if (OnLargeAllocationFailure) {
      OnLargeAllocationFailure();
      p = AllocateExecutableMemory(codeLength, ProtectionSetting::Writable,
                                   MemCheckKind::MakeUndefined);
    }
  }

  if (!p) {
    return nullptr;
  }

  // Construct AutoMarkJitCodeWritableForThread after allocating memory, to
  // ensure it's not nested (OnLargeAllocationFailure can trigger GC).
  writable.emplace();

  // We account for the bytes allocated in WasmModuleObject::create, where we
  // have the necessary JSContext.
  return UniqueCodeBytes((uint8_t*)p, FreeCode(codeLength));
}

void FreeCode::operator()(uint8_t* bytes) {
  MOZ_ASSERT(codeLength);
  MOZ_ASSERT(codeLength == RoundupExecutableCodePageSize(codeLength));

#ifdef MOZ_VTUNE
  vtune::UnmarkBytes(bytes, codeLength);
#endif
  DeallocateExecutableMemory(bytes, codeLength);
}

bool wasm::StaticallyLink(jit::AutoMarkJitCodeWritableForThread& writable,
                          uint8_t* base, const LinkData& linkData,
                          const Code* maybeCode) {
  if (!EnsureBuiltinThunksInitialized(writable)) {
    return false;
  }

  for (LinkData::InternalLink link : linkData.internalLinks) {
    CodeLabel label;
    label.patchAt()->bind(link.patchAtOffset);
    label.target()->bind(link.targetOffset);
#ifdef JS_CODELABEL_LINKMODE
    label.setLinkMode(static_cast<CodeLabel::LinkMode>(link.mode));
#endif
    Assembler::Bind(base, label);
  }

  // MONGODB MODIFICATION: Variable name "far" is a reserved keyword on windows
  // and triggers a cryptic compilation error in MSVC
  for (CallFarJump cfj : linkData.callFarJumps) {
    MOZ_ASSERT(maybeCode && maybeCode->mode() == CompileMode::LazyTiering);
    const CodeBlock& bestBlock = maybeCode->funcCodeBlock(cfj.targetFuncIndex);
    uint32_t stubRangeIndex = bestBlock.funcToCodeRange[cfj.targetFuncIndex];
    const CodeRange& stubRange = bestBlock.codeRanges[stubRangeIndex];
    uint8_t* stubBase = bestBlock.base();
    MacroAssembler::patchFarJump(base + cfj.jumpOffset,
                                 stubBase + stubRange.funcUncheckedCallEntry());
  }

  for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
    const Uint32Vector& offsets = linkData.symbolicLinks[imm];
    if (offsets.empty()) {
      continue;
    }

    void* target = SymbolicAddressTarget(imm);
    for (uint32_t offset : offsets) {
      uint8_t* patchAt = base + offset;
      Assembler::PatchDataWithValueCheck(CodeLocationLabel(patchAt),
                                         PatchedImmPtr(target),
                                         PatchedImmPtr((void*)-1));
    }
  }

  return true;
}

void wasm::StaticallyUnlink(uint8_t* base, const LinkData& linkData) {
  for (LinkData::InternalLink link : linkData.internalLinks) {
    CodeLabel label;
    label.patchAt()->bind(link.patchAtOffset);
    label.target()->bind(-size_t(base));  // to reset immediate to null
#ifdef JS_CODELABEL_LINKMODE
    label.setLinkMode(static_cast<CodeLabel::LinkMode>(link.mode));
#endif
    Assembler::Bind(base, label);
  }

  for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
    const Uint32Vector& offsets = linkData.symbolicLinks[imm];
    if (offsets.empty()) {
      continue;
    }

    void* target = SymbolicAddressTarget(imm);
    for (uint32_t offset : offsets) {
      uint8_t* patchAt = base + offset;
      Assembler::PatchDataWithValueCheck(CodeLocationLabel(patchAt),
                                         PatchedImmPtr((void*)-1),
                                         PatchedImmPtr(target));
    }
  }
}

CodeSource::CodeSource(jit::MacroAssembler& masm, const LinkData* linkData,
                       const Code* code)
    : masm_(&masm),
      bytes_(nullptr),
      length_(masm.bytesNeeded()),
      linkData_(linkData),
      code_(code) {}

CodeSource::CodeSource(const uint8_t* bytes, uint32_t length,
                       const LinkData& linkData, const Code* code)
    : masm_(nullptr),
      bytes_(bytes),
      length_(length),
      linkData_(&linkData),
      code_(code) {}

bool CodeSource::copyAndLink(jit::AutoMarkJitCodeWritableForThread& writable,
                             uint8_t* codeStart) const {
  // Copy the machine code over
  if (masm_) {
    masm_->executableCopy(codeStart);
  } else {
    memcpy(codeStart, bytes_, length_);
  }

  // Use link data if we have it, or else fall back to basic linking using the
  // MacroAssembler.
  if (linkData_) {
    return StaticallyLink(writable, codeStart, *linkData_, code_);
  }

  // We must always have link data if we're coming from raw bytes.
  MOZ_ASSERT(masm_);
  // If we didn't provide link data, then we shouldn't have provided the code
  // object.
  MOZ_ASSERT(!code_);
  PatchDebugSymbolicAccesses(codeStart, *masm_);
  for (const CodeLabel& label : masm_->codeLabels()) {
    Assembler::Bind(codeStart, label);
  }
  return true;
}

size_t CodeSegment::AllocationAlignment() {
  // If we are write-protecting code, all new code allocations must be rounded
  // to the system page size.
  if (JitOptions.writeProtectCode) {
    return gc::SystemPageSize();
  }

  // Otherwise we can just use the standard JIT code alignment.
  return jit::CodeAlignment;
}

size_t CodeSegment::AlignAllocationBytes(uintptr_t bytes) {
  return AlignBytes(bytes, AllocationAlignment());
}

bool CodeSegment::IsAligned(uintptr_t bytes) {
  return bytes == AlignAllocationBytes(bytes);
}

bool CodeSegment::hasSpace(size_t bytes) const {
  MOZ_ASSERT(CodeSegment::IsAligned(bytes));
  return bytes <= capacityBytes() && lengthBytes_ <= capacityBytes() - bytes;
}

void CodeSegment::claimSpace(size_t bytes, uint8_t** claimedBase) {
  MOZ_RELEASE_ASSERT(hasSpace(bytes));
  *claimedBase = base() + lengthBytes_;
  lengthBytes_ += bytes;
}

/* static */
SharedCodeSegment CodeSegment::create(
    mozilla::Maybe<jit::AutoMarkJitCodeWritableForThread>& writable,
    size_t capacityBytes, bool allowLastDitchGC) {
  MOZ_RELEASE_ASSERT(capacityBytes ==
                     RoundupExecutableCodePageSize(capacityBytes));

  UniqueCodeBytes codeBytes;
  if (capacityBytes != 0) {
    codeBytes = AllocateCodeBytes(writable, capacityBytes, allowLastDitchGC);
    if (!codeBytes) {
      return nullptr;
    }
  }

  return js_new<CodeSegment>(std::move(codeBytes), /*lengthBytes=*/0,
                             capacityBytes);
}

// When allocating a single stub to a page, we should not always place the stub
// at the beginning of the page as the stubs will tend to thrash the icache by
// creating conflicts (everything ends up in the same cache set).  Instead,
// locate stubs at different line offsets up to 3/4 the system page size (the
// code allocation quantum).
//
// This may be called on background threads, hence the atomic.
static uint32_t RandomPaddingForCodeLength(uint32_t codeLength) {
  // The counter serves only to spread the code out, it has no other meaning and
  // can wrap around.
  static mozilla::Atomic<uint32_t, mozilla::MemoryOrdering::ReleaseAcquire>
      counter(0);
  // We assume that the icache line size is 64 bytes, which is close to
  // universally true.
  const size_t cacheLineSize = 64;
  const size_t systemPageSize = gc::SystemPageSize();

  // If we're not write-protecting code, then we do not need to add any padding
  if (!JitOptions.writeProtectCode) {
    return 0;
  }

  // Don't add more than 3/4 of a page of padding
  size_t maxPadBytes = ((systemPageSize * 3) / 4);
  size_t maxPadLines = maxPadBytes / cacheLineSize;

  // If code length is close to a page boundary, avoid pushing it to a new page
  size_t remainingBytesInPage =
      AlignBytes(codeLength, systemPageSize) - codeLength;
  size_t remainingLinesInPage = remainingBytesInPage / cacheLineSize;

  // Limit padding to the smallest of the above
  size_t padLinesAvailable = std::min(maxPadLines, remainingLinesInPage);

  // Don't add any padding if none is available
  if (padLinesAvailable == 0) {
    return 0;
  }

  uint32_t random = counter++;
  uint32_t padding = (random % padLinesAvailable) * cacheLineSize;
  // "adding on the padding area doesn't change the total number of pages
  //  required"
  MOZ_ASSERT(AlignBytes(codeLength + padding, systemPageSize) ==
             AlignBytes(codeLength, systemPageSize));
  return padding;
}

/* static */
SharedCodeSegment CodeSegment::allocate(const CodeSource& codeSource,
                                        SharedCodeSegmentVector* segmentPool,
                                        bool allowLastDitchGC,
                                        uint8_t** codeStart,
                                        uint32_t* allocationLength) {
  mozilla::Maybe<AutoMarkJitCodeWritableForThread> writable;
  uint32_t codeLength = codeSource.lengthBytes();
  uint32_t paddingLength = RandomPaddingForCodeLength(codeLength);
  *allocationLength =
      CodeSegment::AlignAllocationBytes(paddingLength + codeLength);

  // If we have a pool of segments, try to find one that has enough space. We
  // just check the last segment in the pool for simplicity.
  SharedCodeSegment segment;
  if (segmentPool && !segmentPool->empty() &&
      segmentPool->back()->hasSpace(*allocationLength)) {
    segment = segmentPool->back();
  } else {
    uint32_t newSegmentCapacity =
        RoundupExecutableCodePageSize(*allocationLength);
    segment =
        CodeSegment::create(writable, newSegmentCapacity, allowLastDitchGC);
    if (!segment) {
      return nullptr;
    }
    if (segmentPool && !segmentPool->append(segment)) {
      return nullptr;
    }
  }

  // Claim space in the segment we found or created
  uint8_t* allocationStart = nullptr;
  segment->claimSpace(*allocationLength, &allocationStart);
  *codeStart = allocationStart + paddingLength;

  // Check our constraints
  MOZ_ASSERT(CodeSegment::IsAligned(uintptr_t(segment->base())));
  MOZ_ASSERT(CodeSegment::IsAligned(allocationStart - segment->base()));
  MOZ_ASSERT(CodeSegment::IsAligned(uintptr_t(allocationStart)));
  MOZ_ASSERT(*codeStart >= allocationStart);
  MOZ_ASSERT(codeLength <= *allocationLength);
  MOZ_ASSERT_IF(JitOptions.writeProtectCode,
                uintptr_t(allocationStart) % gc::SystemPageSize() == 0 &&
                    *allocationLength % gc::SystemPageSize() == 0);
  MOZ_ASSERT(uintptr_t(*codeStart) % jit::CodeAlignment == 0);

  if (!writable) {
    writable.emplace();
  }
  if (!codeSource.copyAndLink(*writable, *codeStart)) {
    return nullptr;
  }

  // Clear the padding between the end of the code and the end of the
  // allocation.
  uint8_t* allocationEnd = allocationStart + *allocationLength;
  uint8_t* codeEnd = *codeStart + codeLength;
  MOZ_ASSERT(codeEnd <= allocationEnd);
  size_t paddingAfterCode = allocationEnd - codeEnd;
  // The swept code pattern is guaranteed to crash if it is ever executed.
  memset(codeEnd, JS_SWEPT_CODE_PATTERN, paddingAfterCode);

  // Optimized compilation finishes on a background thread, so we must make sure
  // to flush the icaches of all the executing threads.
  // Reprotect the whole region to avoid having separate RW and RX mappings.
  if (*allocationLength != 0 &&
      !ExecutableAllocator::makeExecutableAndFlushICache(allocationStart,
                                                         *allocationLength)) {
    return nullptr;
  }

  return segment;
}

void CodeSegment::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code,
                                size_t* data) const {
  *code += capacityBytes();
  *data += mallocSizeOf(this);
}

size_t CacheableChars::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(get());
}

static constexpr unsigned LAZY_STUB_LIFO_DEFAULT_CHUNK_SIZE = 8 * 1024;

bool Code::createManyLazyEntryStubs(const WriteGuard& guard,
                                    const Uint32Vector& funcExportIndices,
                                    const CodeBlock& tierCodeBlock,
                                    size_t* stubBlockIndex) const {
  MOZ_ASSERT(funcExportIndices.length());

  LifoAlloc lifo(LAZY_STUB_LIFO_DEFAULT_CHUNK_SIZE, js::MallocArena);
  TempAllocator alloc(&lifo);
  JitContext jitContext;
  WasmMacroAssembler masm(alloc);

  const FuncExportVector& funcExports = tierCodeBlock.funcExports;
  uint8_t* codeBase = tierCodeBlock.base();

  CodeRangeVector codeRanges;
  DebugOnly<uint32_t> numExpectedRanges = 0;
  for (uint32_t funcExportIndex : funcExportIndices) {
    const FuncExport& fe = funcExports[funcExportIndex];
    const FuncType& funcType = codeMeta_->getFuncType(fe.funcIndex());
    // Exports that don't support a jit entry get only the interp entry.
    numExpectedRanges += (funcType.canHaveJitEntry() ? 2 : 1);
    void* calleePtr =
        codeBase + tierCodeBlock.codeRange(fe).funcUncheckedCallEntry();
    Maybe<ImmPtr> callee;
    callee.emplace(calleePtr, ImmPtr::NoCheckToken());
    if (!GenerateEntryStubs(masm, funcExportIndex, fe, funcType, callee,
                            /* asmjs */ false, &codeRanges)) {
      return false;
    }
  }
  MOZ_ASSERT(codeRanges.length() == numExpectedRanges,
             "incorrect number of entries per function");

  masm.finish();

  MOZ_ASSERT(masm.inliningContext().empty());
  MOZ_ASSERT(masm.callSites().empty());
  MOZ_ASSERT(masm.callSiteTargets().empty());
  MOZ_ASSERT(masm.trapSites().empty());
  MOZ_ASSERT(masm.tryNotes().empty());
  MOZ_ASSERT(masm.codeRangeUnwindInfos().empty());

  if (masm.oom()) {
    return false;
  }

  UniqueCodeBlock stubCodeBlock =
      MakeUnique<CodeBlock>(CodeBlockKind::LazyStubs);
  if (!stubCodeBlock) {
    return false;
  }

  // Allocate space in a code segment we can use
  uint32_t codeLength = masm.bytesNeeded();
  uint8_t* codeStart;
  uint32_t allocationLength;
  CodeSource codeSource(masm, nullptr, nullptr);
  stubCodeBlock->segment = CodeSegment::allocate(
      codeSource, &guard->lazyStubSegments,
      /* allowLastDitchGC = */ true, &codeStart, &allocationLength);
  if (!stubCodeBlock->segment) {
    return false;
  }

  stubCodeBlock->codeBase = codeStart;
  stubCodeBlock->codeLength = codeLength;
  stubCodeBlock->codeRanges = std::move(codeRanges);

  *stubBlockIndex = guard->blocks.length();

  uint32_t codeRangeIndex = 0;
  for (uint32_t funcExportIndex : funcExportIndices) {
    const FuncExport& fe = funcExports[funcExportIndex];
    const FuncType& funcType = codeMeta_->getFuncType(fe.funcIndex());

    LazyFuncExport lazyExport(fe.funcIndex(), *stubBlockIndex, codeRangeIndex,
                              tierCodeBlock.kind);

    codeRangeIndex += 1;

    if (funcType.canHaveJitEntry()) {
      codeRangeIndex += 1;
    }

    size_t exportIndex;
    const uint32_t targetFunctionIndex = fe.funcIndex();

    if (BinarySearchIf(
            guard->lazyExports, 0, guard->lazyExports.length(),
            [targetFunctionIndex](const LazyFuncExport& funcExport) {
              return targetFunctionIndex - funcExport.funcIndex;
            },
            &exportIndex)) {
      DebugOnly<CodeBlockKind> oldKind =
          guard->lazyExports[exportIndex].funcKind;
      MOZ_ASSERT(oldKind == CodeBlockKind::SharedStubs ||
                 oldKind == CodeBlockKind::BaselineTier);
      guard->lazyExports[exportIndex] = std::move(lazyExport);
    } else if (!guard->lazyExports.insert(
                   guard->lazyExports.begin() + exportIndex,
                   std::move(lazyExport))) {
      return false;
    }
  }

  stubCodeBlock->sendToProfiler(*codeMeta_, *codeTailMeta_, codeMetaForAsmJS_,
                                FuncIonPerfSpewerSpan(),
                                FuncBaselinePerfSpewerSpan());
  return addCodeBlock(guard, std::move(stubCodeBlock), nullptr);
}

bool Code::createOneLazyEntryStub(const WriteGuard& guard,
                                  uint32_t funcExportIndex,
                                  const CodeBlock& tierCodeBlock,
                                  void** interpEntry) const {
  Uint32Vector funcExportIndexes;
  if (!funcExportIndexes.append(funcExportIndex)) {
    return false;
  }

  size_t stubBlockIndex;
  if (!createManyLazyEntryStubs(guard, funcExportIndexes, tierCodeBlock,
                                &stubBlockIndex)) {
    return false;
  }

  const CodeBlock& block = *guard->blocks[stubBlockIndex];
  const CodeRangeVector& codeRanges = block.codeRanges;

  const FuncExport& fe = tierCodeBlock.funcExports[funcExportIndex];
  const FuncType& funcType = codeMeta_->getFuncType(fe.funcIndex());

  // We created one or two stubs, depending on the function type.
  uint32_t funcEntryRanges = funcType.canHaveJitEntry() ? 2 : 1;
  MOZ_ASSERT(codeRanges.length() >= funcEntryRanges);

  // The first created range is the interp entry
  const CodeRange& interpRange =
      codeRanges[codeRanges.length() - funcEntryRanges];
  MOZ_ASSERT(interpRange.isInterpEntry());
  *interpEntry = block.base() + interpRange.begin();

  // The second created range is the jit entry
  if (funcType.canHaveJitEntry()) {
    const CodeRange& jitRange =
        codeRanges[codeRanges.length() - funcEntryRanges + 1];
    MOZ_ASSERT(jitRange.isJitEntry());
    jumpTables_.setJitEntry(jitRange.funcIndex(),
                            block.base() + jitRange.begin());
  }
  return true;
}

bool Code::getOrCreateInterpEntry(uint32_t funcIndex,
                                  const FuncExport** funcExport,
                                  void** interpEntry) const {
  size_t funcExportIndex;
  const CodeBlock& codeBlock = funcCodeBlock(funcIndex);
  *funcExport = &codeBlock.lookupFuncExport(funcIndex, &funcExportIndex);

  const FuncExport& fe = **funcExport;
  if (fe.hasEagerStubs()) {
    *interpEntry = codeBlock.base() + fe.eagerInterpEntryOffset();
    return true;
  }

  MOZ_ASSERT(!codeMetaForAsmJS_, "only wasm can lazily export functions");

  auto guard = data_.writeLock();
  *interpEntry = lookupLazyInterpEntry(guard, funcIndex);
  if (*interpEntry) {
    return true;
  }

  return createOneLazyEntryStub(guard, funcExportIndex, codeBlock, interpEntry);
}

bool Code::createTier2LazyEntryStubs(const WriteGuard& guard,
                                     const CodeBlock& tier2Code,
                                     Maybe<size_t>* outStubBlockIndex) const {
  if (!guard->lazyExports.length()) {
    return true;
  }

  Uint32Vector funcExportIndices;
  if (!funcExportIndices.reserve(guard->lazyExports.length())) {
    return false;
  }

  for (size_t i = 0; i < tier2Code.funcExports.length(); i++) {
    const FuncExport& fe = tier2Code.funcExports[i];
    const LazyFuncExport* lfe = lookupLazyFuncExport(guard, fe.funcIndex());
    if (lfe) {
      MOZ_ASSERT(lfe->funcKind == CodeBlockKind::BaselineTier);
      funcExportIndices.infallibleAppend(i);
    }
  }

  if (funcExportIndices.length() == 0) {
    return true;
  }

  size_t stubBlockIndex;
  if (!createManyLazyEntryStubs(guard, funcExportIndices, tier2Code,
                                &stubBlockIndex)) {
    return false;
  }

  outStubBlockIndex->emplace(stubBlockIndex);
  return true;
}

class Module::PartialTier2CompileTaskImpl : public PartialTier2CompileTask {
  const SharedCode code_;
  uint32_t funcIndex_;
  Atomic<bool> cancelled_;

 public:
  PartialTier2CompileTaskImpl(const Code& code, uint32_t funcIndex)
      : code_(&code), funcIndex_(funcIndex), cancelled_(false) {}

  void cancel() override { cancelled_ = true; }

  void runHelperThreadTask(AutoLockHelperThreadState& locked) override {
    if (!cancelled_) {
      AutoUnlockHelperThreadState unlock(locked);

      // In the case `!success && !cancelled_`, compilation has failed
      // and this function will be stuck in state TierUpState::Requested
      // forever.
      UniqueChars error;
      UniqueCharsVector warnings;
      bool success = CompilePartialTier2(*code_, funcIndex_, &error, &warnings,
                                         &cancelled_);
      ReportTier2ResultsOffThread(
          cancelled_, success, mozilla::Some(funcIndex_),
          code_->codeMeta().scriptedCaller(), error, warnings);
    }

    // The task is finished, release it.
    js_delete(this);
  }

  ThreadType threadType() override {
    return ThreadType::THREAD_TYPE_WASM_COMPILE_PARTIAL_TIER2;
  }
};

bool Code::requestTierUp(uint32_t funcIndex) const {
  // Note: this runs on the requesting (wasm-running) thread, not on a
  // compilation-helper thread.
  MOZ_ASSERT(mode_ == CompileMode::LazyTiering);
  FuncState& state = funcStates_[funcIndex - codeMeta_->numFuncImports];
  if (!state.tierUpState.compareExchange(TierUpState::NotRequested,
                                         TierUpState::Requested)) {
    return true;
  }

  auto task =
      js::MakeUnique<Module::PartialTier2CompileTaskImpl>(*this, funcIndex);
  if (!task) {
    // Effect is (I think), if we OOM here, the request is ignored.
    // See bug 1911060.
    return false;
  }

  StartOffThreadWasmPartialTier2Compile(std::move(task));
  return true;
}

bool Code::finishTier2(UniqueCodeBlock tier2CodeBlock,
                       UniqueLinkData tier2LinkData,
                       const CompileAndLinkStats& tier2Stats) const {
  MOZ_RELEASE_ASSERT(mode_ == CompileMode::EagerTiering ||
                     mode_ == CompileMode::LazyTiering);
  MOZ_RELEASE_ASSERT(hasCompleteTier2_ == false &&
                     tier2CodeBlock->tier() == Tier::Optimized);
  // Acquire the write guard before we start mutating anything. We hold this
  // for the minimum amount of time necessary.
  CodeBlock* tier2CodePointer;
  {
    auto guard = data_.writeLock();

    // Record the tier2 stats.
    guard->tier2Stats.merge(tier2Stats);

    // Borrow the tier2 pointer before moving it into the block vector. This
    // ensures we maintain the invariant that completeTier2_ is never read if
    // hasCompleteTier2_ is false.
    tier2CodePointer = tier2CodeBlock.get();

    // Publish this code to the process wide map.
    if (!addCodeBlock(guard, std::move(tier2CodeBlock),
                      std::move(tier2LinkData))) {
      return false;
    }

    // Before we can make tier-2 live, we need to compile tier2 versions of any
    // extant tier1 lazy stubs (otherwise, tiering would break the assumption
    // that any extant exported wasm function has had a lazy entry stub already
    // compiled for it).
    //
    // Also see doc block for stubs in WasmJS.cpp.
    Maybe<size_t> stub2Index;
    if (!createTier2LazyEntryStubs(guard, *tier2CodePointer, &stub2Index)) {
      return false;
    }

    // Initializing the code above will have flushed the icache for all cores.
    // However, there could still be stale data in the execution pipeline of
    // other cores on some platforms. Force an execution context flush on all
    // threads to fix this before we commit the code.
    //
    // This is safe due to the check in `PlatformCanTier` in WasmCompile.cpp
    jit::FlushExecutionContextForAllThreads();

    // Now that we can't fail or otherwise abort tier2, make it live.
    if (mode_ == CompileMode::EagerTiering) {
      completeTier2_ = tier2CodePointer;
      hasCompleteTier2_ = true;

      // We don't need to update funcStates, because we're doing eager tiering
      MOZ_ASSERT(!funcStates_.get());
    } else {
      for (const CodeRange& cr : tier2CodePointer->codeRanges) {
        if (!cr.isFunction()) {
          continue;
        }
        FuncState& state =
            funcStates_.get()[cr.funcIndex() - codeMeta_->numFuncImports];
        state.bestTier = tier2CodePointer;
        state.tierUpState = TierUpState::Finished;
      }
    }

    // Update jump vectors with pointers to tier-2 lazy entry stubs, if any.
    if (stub2Index) {
      const CodeBlock& block = *guard->blocks[*stub2Index];
      for (const CodeRange& cr : block.codeRanges) {
        if (!cr.isJitEntry()) {
          continue;
        }
        jumpTables_.setJitEntry(cr.funcIndex(), block.base() + cr.begin());
      }
    }
  }

  // And we update the jump vectors with pointers to tier-2 functions and eager
  // stubs.  Callers will continue to invoke tier-1 code until, suddenly, they
  // will invoke tier-2 code.  This is benign.
  uint8_t* base = tier2CodePointer->base();
  for (const CodeRange& cr : tier2CodePointer->codeRanges) {
    // These are racy writes that we just want to be visible, atomically,
    // eventually.  All hardware we care about will do this right.  But
    // we depend on the compiler not splitting the stores hidden inside the
    // set*Entry functions.
    if (cr.isFunction()) {
      jumpTables_.setTieringEntry(cr.funcIndex(), base + cr.funcTierEntry());
    } else if (cr.isJitEntry()) {
      jumpTables_.setJitEntry(cr.funcIndex(), base + cr.begin());
    }
  }
  return true;
}

bool Code::addCodeBlock(const WriteGuard& guard, UniqueCodeBlock block,
                        UniqueLinkData maybeLinkData) const {
  // Don't bother saving the link data if the block won't be serialized
  if (maybeLinkData && !block->isSerializable()) {
    maybeLinkData = nullptr;
  }

  CodeBlock* blockPtr = block.get();
  size_t codeBlockIndex = guard->blocks.length();
  return guard->blocks.append(std::move(block)) &&
         guard->blocksLinkData.append(std::move(maybeLinkData)) &&
         blockMap_.insert(blockPtr) &&
         blockPtr->initialize(*this, codeBlockIndex);
}

SharedCodeSegment Code::createFuncCodeSegmentFromPool(
    jit::MacroAssembler& masm, const LinkData& linkData, bool allowLastDitchGC,
    uint8_t** codeStartOut, uint32_t* codeLengthOut) const {
  uint32_t codeLength = masm.bytesNeeded();

  // Allocate the code segment
  uint8_t* codeStart;
  uint32_t allocationLength;
  SharedCodeSegment segment;
  {
    auto guard = data_.writeLock();
    CodeSource codeSource(masm, &linkData, this);
    segment =
        CodeSegment::allocate(codeSource, &guard->lazyFuncSegments,
                              allowLastDitchGC, &codeStart, &allocationLength);
    if (!segment) {
      return nullptr;
    }

    // This function is always used with tier-2
    guard->tier2Stats.codeBytesMapped += allocationLength;
    guard->tier2Stats.codeBytesUsed += codeLength;
  }

  *codeStartOut = codeStart;
  *codeLengthOut = codeLength;
  return segment;
}

const LazyFuncExport* Code::lookupLazyFuncExport(const WriteGuard& guard,
                                                 uint32_t funcIndex) const {
  size_t match;
  if (!BinarySearchIf(
          guard->lazyExports, 0, guard->lazyExports.length(),
          [funcIndex](const LazyFuncExport& funcExport) {
            return funcIndex - funcExport.funcIndex;
          },
          &match)) {
    return nullptr;
  }
  return &guard->lazyExports[match];
}

void* Code::lookupLazyInterpEntry(const WriteGuard& guard,
                                  uint32_t funcIndex) const {
  const LazyFuncExport* fe = lookupLazyFuncExport(guard, funcIndex);
  if (!fe) {
    return nullptr;
  }
  const CodeBlock& block = *guard->blocks[fe->lazyStubBlockIndex];
  return block.base() + block.codeRanges[fe->funcCodeRangeIndex].begin();
}

CodeBlock::~CodeBlock() {
  if (unregisterOnDestroy_) {
    UnregisterCodeBlock(this);
  }
}

bool CodeBlock::initialize(const Code& code, size_t codeBlockIndex) {
  MOZ_ASSERT(!initialized());
  this->code = &code;
  this->codeBlockIndex = codeBlockIndex;
  segment->setCode(code);

  // In the case of tiering, RegisterCodeBlock() immediately makes this code
  // block live to access from other threads executing the containing
  // module. So only call once the CodeBlock is fully initialized.
  if (!RegisterCodeBlock(this)) {
    return false;
  }

  // This bool is only used by the destructor which cannot be called racily
  // and so it is not a problem to mutate it after RegisterCodeBlock().
  MOZ_ASSERT(!unregisterOnDestroy_);
  unregisterOnDestroy_ = true;

  MOZ_ASSERT(initialized());
  return true;
}

static JS::UniqueChars DescribeCodeRangeForProfiler(
    const wasm::CodeMetadata& codeMeta,
    const wasm::CodeTailMetadata& codeTailMeta,
    const CodeMetadataForAsmJS* codeMetaForAsmJS, const CodeRange& codeRange,
    CodeBlockKind codeBlockKind) {
  uint32_t funcIndex = codeRange.funcIndex();
  UTF8Bytes name;
  bool ok;
  if (codeMetaForAsmJS) {
    ok = codeMetaForAsmJS->getFuncNameForAsmJS(funcIndex, &name);
  } else {
    ok = codeMeta.getFuncNameForWasm(NameContext::Standalone, funcIndex,
                                     codeTailMeta.nameSectionPayload.get(),
                                     &name);
  }
  if (!ok) {
    return nullptr;
  }
  if (!name.append('\0')) {
    return nullptr;
  }

  const char* category = "";
  const char* filename = codeMeta.scriptedCaller().filename.get();
  const char* suffix = "";
  if (codeRange.isFunction()) {
    category = "Wasm";
    if (codeBlockKind == CodeBlockKind::BaselineTier) {
      suffix = " [baseline]";
    } else if (codeBlockKind == CodeBlockKind::OptimizedTier) {
      suffix = " [optimized]";
    }
  } else if (codeRange.isInterpEntry()) {
    category = "WasmTrampoline";
    suffix = " slow entry";
  } else if (codeRange.isJitEntry()) {
    category = "WasmTrampoline";
    suffix = " fast entry";
  } else if (codeRange.isImportInterpExit()) {
    category = "WasmTrampoline";
    suffix = " slow exit";
  } else if (codeRange.isImportJitExit()) {
    category = "WasmTrampoline";
    suffix = " fast exit";
  }

  return JS_smprintf("%s: %s: Function %s%s (WASM:%u)", category, filename,
                     name.begin(), suffix, funcIndex);
}

void CodeBlock::sendToProfiler(
    const CodeMetadata& codeMeta, const CodeTailMetadata& codeTailMeta,
    const CodeMetadataForAsmJS* codeMetaForAsmJS,
    FuncIonPerfSpewerSpan ionSpewers,
    FuncBaselinePerfSpewerSpan baselineSpewers) const {
  bool enabled = false;
  enabled |= PerfEnabled();
#ifdef MOZ_VTUNE
  enabled |= vtune::IsProfilingActive();
#endif
  if (!enabled) {
    return;
  }

  // We only ever have ion or baseline spewers, and they correspond with our
  // code block kind.
  MOZ_ASSERT(ionSpewers.empty() || baselineSpewers.empty());
  MOZ_ASSERT_IF(kind == CodeBlockKind::BaselineTier, ionSpewers.empty());
  MOZ_ASSERT_IF(kind == CodeBlockKind::OptimizedTier, baselineSpewers.empty());
  bool hasSpewers = !ionSpewers.empty() || !baselineSpewers.empty();

  // Save the collected Ion perf spewers with their IR/source information.
  for (FuncIonPerfSpewer& funcIonSpewer : ionSpewers) {
    const CodeRange& codeRange = this->codeRange(funcIonSpewer.funcIndex);
    UniqueChars desc = DescribeCodeRangeForProfiler(
        codeMeta, codeTailMeta, codeMetaForAsmJS, codeRange, kind);
    if (!desc) {
      return;
    }
    uintptr_t start = uintptr_t(base() + codeRange.begin());
    uintptr_t size = codeRange.end() - codeRange.begin();
    funcIonSpewer.spewer.saveWasmProfile(start, size, desc);
  }

  // Save the collected baseline perf spewers with their IR/source information.
  for (FuncBaselinePerfSpewer& funcBaselineSpewer : baselineSpewers) {
    const CodeRange& codeRange = this->codeRange(funcBaselineSpewer.funcIndex);
    UniqueChars desc = DescribeCodeRangeForProfiler(
        codeMeta, codeTailMeta, codeMetaForAsmJS, codeRange, kind);
    if (!desc) {
      return;
    }
    uintptr_t start = uintptr_t(base() + codeRange.begin());
    uintptr_t size = codeRange.end() - codeRange.begin();
    funcBaselineSpewer.spewer.saveProfile(start, size, desc);
  }

  // Save the rest of the code ranges.
  for (const CodeRange& codeRange : codeRanges) {
    if (!codeRange.hasFuncIndex()) {
      continue;
    }

    // Skip functions when they have corresponding spewers, as they will have
    // already handled the function.
    if (codeRange.isFunction() && hasSpewers) {
      continue;
    }

    UniqueChars desc = DescribeCodeRangeForProfiler(
        codeMeta, codeTailMeta, codeMetaForAsmJS, codeRange, kind);
    if (!desc) {
      return;
    }

    uintptr_t start = uintptr_t(base() + codeRange.begin());
    uintptr_t size = codeRange.end() - codeRange.begin();

#ifdef MOZ_VTUNE
    if (vtune::IsProfilingActive()) {
      vtune::MarkWasm(vtune::GenerateUniqueMethodID(), desc.get(), (void*)start,
                      size);
    }
#endif

    if (PerfEnabled()) {
      CollectPerfSpewerWasmMap(start, size, std::move(desc));
    }
  }
}

void CodeBlock::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code,
                              size_t* data) const {
  segment->addSizeOfMisc(mallocSizeOf, code, data);
  *data += funcToCodeRange.sizeOfExcludingThis(mallocSizeOf) +
           codeRanges.sizeOfExcludingThis(mallocSizeOf) +
           inliningContext.sizeOfExcludingThis(mallocSizeOf) +
           callSites.sizeOfExcludingThis(mallocSizeOf) +
           tryNotes.sizeOfExcludingThis(mallocSizeOf) +
           codeRangeUnwindInfos.sizeOfExcludingThis(mallocSizeOf) +
           trapSites.sizeOfExcludingThis(mallocSizeOf) +
           stackMaps.sizeOfExcludingThis(mallocSizeOf) +
           funcExports.sizeOfExcludingThis(mallocSizeOf);
  ;
}

const CodeRange* CodeBlock::lookupRange(const void* pc) const {
  CodeRange::OffsetInCode target((uint8_t*)pc - base());
  return LookupInSorted(codeRanges, target);
}

bool CodeBlock::lookupCallSite(void* pc, CallSite* callSite) const {
  uint32_t target = ((uint8_t*)pc) - base();
  return callSites.lookup(target, inliningContext, callSite);
}

const StackMap* CodeBlock::lookupStackMap(uint8_t* pc) const {
  // We need to subtract the offset from the beginning of the codeblock.
  uint32_t offsetInCodeBlock = pc - base();
  return stackMaps.lookup(offsetInCodeBlock);
}

const wasm::TryNote* CodeBlock::lookupTryNote(const void* pc) const {
  size_t target = (uint8_t*)pc - base();

  // We find the first hit (there may be multiple) to obtain the innermost
  // handler, which is why we cannot binary search here.
  for (const auto& tryNote : tryNotes) {
    if (tryNote.offsetWithinTryBody(target)) {
      return &tryNote;
    }
  }

  return nullptr;
}

bool CodeBlock::lookupTrap(void* pc, Trap* kindOut, TrapSite* trapOut) const {
  MOZ_ASSERT(containsCodePC(pc));
  uint32_t target = ((uint8_t*)pc) - base();
  return trapSites.lookup(target, inliningContext, kindOut, trapOut);
}

struct UnwindInfoPCOffset {
  const CodeRangeUnwindInfoVector& info;
  explicit UnwindInfoPCOffset(const CodeRangeUnwindInfoVector& info)
      : info(info) {}
  uint32_t operator[](size_t index) const { return info[index].offset(); }
};

const CodeRangeUnwindInfo* CodeBlock::lookupUnwindInfo(void* pc) const {
  uint32_t target = ((uint8_t*)pc) - base();
  size_t match;
  const CodeRangeUnwindInfo* info = nullptr;
  if (BinarySearch(UnwindInfoPCOffset(codeRangeUnwindInfos), 0,
                   codeRangeUnwindInfos.length(), target, &match)) {
    info = &codeRangeUnwindInfos[match];
  } else {
    // Exact match is not found, using insertion point to get the previous
    // info entry; skip if info is outside of codeRangeUnwindInfos.
    if (match == 0) return nullptr;
    if (match == codeRangeUnwindInfos.length()) {
      MOZ_ASSERT(
          codeRangeUnwindInfos[codeRangeUnwindInfos.length() - 1].unwindHow() ==
          CodeRangeUnwindInfo::Normal);
      return nullptr;
    }
    info = &codeRangeUnwindInfos[match - 1];
  }
  return info->unwindHow() == CodeRangeUnwindInfo::Normal ? nullptr : info;
}

struct ProjectFuncIndex {
  const FuncExportVector& funcExports;
  explicit ProjectFuncIndex(const FuncExportVector& funcExports)
      : funcExports(funcExports) {}
  uint32_t operator[](size_t index) const {
    return funcExports[index].funcIndex();
  }
};

FuncExport& CodeBlock::lookupFuncExport(
    uint32_t funcIndex, size_t* funcExportIndex /* = nullptr */) {
  size_t match;
  if (!BinarySearch(ProjectFuncIndex(funcExports), 0, funcExports.length(),
                    funcIndex, &match)) {
    MOZ_CRASH("missing function export");
  }
  if (funcExportIndex) {
    *funcExportIndex = match;
  }
  return funcExports[match];
}

const FuncExport& CodeBlock::lookupFuncExport(uint32_t funcIndex,
                                              size_t* funcExportIndex) const {
  return const_cast<CodeBlock*>(this)->lookupFuncExport(funcIndex,
                                                        funcExportIndex);
}

bool JumpTables::initialize(CompileMode mode, const CodeMetadata& codeMeta,
                            const CodeBlock& sharedStubs,
                            const CodeBlock& tier1) {
  static_assert(JSScript::offsetOfJitCodeRaw() == 0,
                "wasm fast jit entry is at (void*) jit[funcIndex]");

  mode_ = mode;
  numFuncs_ = codeMeta.numFuncs();

  if (mode_ != CompileMode::Once) {
    tiering_ = TablePointer(js_pod_calloc<void*>(numFuncs_));
    if (!tiering_) {
      return false;
    }
  }

  // The number of jit entries is overestimated, but it is simpler when
  // filling/looking up the jit entries and safe (worst case we'll crash
  // because of a null deref when trying to call the jit entry of an
  // unexported function).
  jit_ = TablePointer(js_pod_calloc<void*>(numFuncs_));
  if (!jit_) {
    return false;
  }

  uint8_t* codeBase = sharedStubs.base();
  for (const CodeRange& cr : sharedStubs.codeRanges) {
    if (cr.isFunction()) {
      setTieringEntry(cr.funcIndex(), codeBase + cr.funcTierEntry());
    } else if (cr.isJitEntry()) {
      setJitEntry(cr.funcIndex(), codeBase + cr.begin());
    }
  }

  codeBase = tier1.base();
  for (const CodeRange& cr : tier1.codeRanges) {
    if (cr.isFunction()) {
      setTieringEntry(cr.funcIndex(), codeBase + cr.funcTierEntry());
    } else if (cr.isJitEntry()) {
      setJitEntry(cr.funcIndex(), codeBase + cr.begin());
    }
  }
  return true;
}

Code::Code(CompileMode mode, const CodeMetadata& codeMeta,
           const CodeTailMetadata& codeTailMeta,
           const CodeMetadataForAsmJS* codeMetaForAsmJS)
    : mode_(mode),
      data_(mutexid::WasmCodeProtected),
      codeMeta_(&codeMeta),
      codeTailMeta_(&codeTailMeta),
      codeMetaForAsmJS_(codeMetaForAsmJS),
      completeTier1_(nullptr),
      completeTier2_(nullptr),
      profilingLabels_(mutexid::WasmCodeProfilingLabels,
                       CacheableCharsVector()),
      trapCode_(nullptr),
      debugStubOffset_(0),
      requestTierUpStubOffset_(0),
      updateCallRefMetricsStubOffset_(0) {}

Code::~Code() { printStats(); }

void Code::printStats() const {
#ifdef JS_JITSPEW
  auto guard = data_.readLock();

  JS_LOG(wasmPerf, Info, "CM=..%06lx  Code::~Code <<<<",
         0xFFFFFF & (unsigned long)uintptr_t(codeMeta_.get()));

  // Module information
  JS_LOG(wasmPerf, Info, "    %7zu functions in module", codeMeta_->numFuncs());
  JS_LOG(wasmPerf, Info, "    %7zu bytecode bytes in module",
         codeMeta_->codeSectionSize());
  uint32_t numCallRefs = codeTailMeta_->numCallRefMetrics == UINT32_MAX
                             ? 0
                             : codeTailMeta_->numCallRefMetrics;
  JS_LOG(wasmPerf, Info, "    %7u call_refs in module", numCallRefs);

  // Tier information
  JS_LOG(wasmPerf, Info, "            ------ Tier 1 ------");
  guard->tier1Stats.print();
  if (mode() != CompileMode::Once) {
    JS_LOG(wasmPerf, Info, "            ------ Tier 2 ------");
    guard->tier2Stats.print();
  }

  JS_LOG(wasmPerf, Info, ">>>>");
#endif
}

bool Code::initialize(FuncImportVector&& funcImports,
                      UniqueCodeBlock sharedStubs,
                      UniqueLinkData sharedStubsLinkData,
                      UniqueCodeBlock tier1CodeBlock,
                      UniqueLinkData tier1LinkData,
                      const CompileAndLinkStats& tier1Stats) {
  funcImports_ = std::move(funcImports);

  auto guard = data_.writeLock();

  MOZ_ASSERT(guard->tier1Stats.empty());
  guard->tier1Stats = tier1Stats;

  sharedStubs_ = sharedStubs.get();
  completeTier1_ = tier1CodeBlock.get();
  trapCode_ = sharedStubs_->base() + sharedStubsLinkData->trapOffset;
  if (!jumpTables_.initialize(mode_, *codeMeta_, *sharedStubs_,
                              *completeTier1_) ||
      !addCodeBlock(guard, std::move(sharedStubs),
                    std::move(sharedStubsLinkData)) ||
      !addCodeBlock(guard, std::move(tier1CodeBlock),
                    std::move(tier1LinkData))) {
    return false;
  }

  if (mode_ == CompileMode::LazyTiering) {
    uint32_t numFuncDefs = codeMeta_->numFuncs() - codeMeta_->numFuncImports;
    funcStates_ = FuncStatesPointer(js_pod_calloc<FuncState>(numFuncDefs));
    if (!funcStates_) {
      return false;
    }
    for (uint32_t funcDefIndex = 0; funcDefIndex < numFuncDefs;
         funcDefIndex++) {
      funcStates_.get()[funcDefIndex].bestTier = completeTier1_;
      funcStates_.get()[funcDefIndex].tierUpState = TierUpState::NotRequested;
    }
  }

  return true;
}

Tiers Code::completeTiers() const {
  if (hasCompleteTier2_) {
    return Tiers(completeTier1_->tier(), completeTier2_->tier());
  }
  return Tiers(completeTier1_->tier());
}

bool Code::hasCompleteTier(Tier t) const {
  if (hasCompleteTier2_ && completeTier2_->tier() == t) {
    return true;
  }
  return completeTier1_->tier() == t;
}

Tier Code::stableCompleteTier() const { return completeTier1_->tier(); }

Tier Code::bestCompleteTier() const {
  if (hasCompleteTier2_) {
    return completeTier2_->tier();
  }
  return completeTier1_->tier();
}

const CodeBlock& Code::completeTierCodeBlock(Tier tier) const {
  switch (tier) {
    case Tier::Baseline:
      if (completeTier1_->tier() == Tier::Baseline) {
        MOZ_ASSERT(completeTier1_->initialized());
        return *completeTier1_;
      }
      MOZ_CRASH("No code segment at this tier");
    case Tier::Optimized:
      if (completeTier1_->tier() == Tier::Optimized) {
        MOZ_ASSERT(completeTier1_->initialized());
        return *completeTier1_;
      }
      // It is incorrect to ask for the optimized tier without there being such
      // a tier and the tier having been committed.  The guard here could
      // instead be `if (hasCompleteTier2_) ... ` but codeBlock(t) should not be
      // called in contexts where that test is necessary.
      MOZ_RELEASE_ASSERT(hasCompleteTier2_);
      MOZ_ASSERT(completeTier2_->initialized());
      return *completeTier2_;
  }
  MOZ_CRASH();
}

const LinkData* Code::codeBlockLinkData(const CodeBlock& block) const {
  auto guard = data_.readLock();
  MOZ_ASSERT(block.initialized() && block.code == this);
  return guard->blocksLinkData[block.codeBlockIndex].get();
}

void Code::clearLinkData() const {
  auto guard = data_.writeLock();
  for (UniqueLinkData& linkData : guard->blocksLinkData) {
    linkData = nullptr;
  }
}

// When enabled, generate profiling labels for every name in funcNames_ that is
// the name of some Function CodeRange. This involves malloc() so do it now
// since, once we start sampling, we'll be in a signal-handing context where we
// cannot malloc.
void Code::ensureProfilingLabels(bool profilingEnabled) const {
  auto labels = profilingLabels_.lock();

  if (!profilingEnabled) {
    labels->clear();
    return;
  }

  if (!labels->empty()) {
    return;
  }

  // Any tier will do, we only need tier-invariant data that are incidentally
  // stored with the code ranges.
  const CodeBlock& sharedStubsCodeBlock = sharedStubs();
  const CodeBlock& tier1CodeBlock = completeTierCodeBlock(stableCompleteTier());

  // Ignore any OOM failures, nothing we can do about it
  (void)appendProfilingLabels(labels, sharedStubsCodeBlock);
  (void)appendProfilingLabels(labels, tier1CodeBlock);
}

bool Code::appendProfilingLabels(
    const ExclusiveData<CacheableCharsVector>::Guard& labels,
    const CodeBlock& codeBlock) const {
  for (const CodeRange& codeRange : codeBlock.codeRanges) {
    if (!codeRange.isFunction()) {
      continue;
    }

    Int32ToCStringBuf cbuf;
    size_t bytecodeStrLen;
    const char* bytecodeStr = Uint32ToCString(
        &cbuf, codeTailMeta().funcBytecodeOffset(codeRange.funcIndex()),
        &bytecodeStrLen);
    MOZ_ASSERT(bytecodeStr);

    UTF8Bytes name;
    bool ok;
    if (codeMetaForAsmJS()) {
      ok =
          codeMetaForAsmJS()->getFuncNameForAsmJS(codeRange.funcIndex(), &name);
    } else {
      ok = codeMeta().getFuncNameForWasm(
          NameContext::Standalone, codeRange.funcIndex(),
          codeTailMeta().nameSectionPayload.get(), &name);
    }
    if (!ok || !name.append(" (", 2)) {
      return false;
    }

    if (const char* filename = codeMeta().scriptedCaller().filename.get()) {
      if (!name.append(filename, strlen(filename))) {
        return false;
      }
    } else {
      if (!name.append('?')) {
        return false;
      }
    }

    if (!name.append(':') || !name.append(bytecodeStr, bytecodeStrLen) ||
        !name.append(")\0", 2)) {
      return false;
    }

    UniqueChars label(name.extractOrCopyRawBuffer());
    if (!label) {
      return false;
    }

    if (codeRange.funcIndex() >= labels->length()) {
      if (!labels->resize(codeRange.funcIndex() + 1)) {
        return false;
      }
    }

    ((CacheableCharsVector&)labels)[codeRange.funcIndex()] = std::move(label);
  }
  return true;
}

const char* Code::profilingLabel(uint32_t funcIndex) const {
  auto labels = profilingLabels_.lock();

  if (funcIndex >= labels->length() ||
      !((CacheableCharsVector&)labels)[funcIndex]) {
    return "?";
  }
  return ((CacheableCharsVector&)labels)[funcIndex].get();
}

void Code::addSizeOfMiscIfNotSeen(
    MallocSizeOf mallocSizeOf, CodeMetadata::SeenSet* seenCodeMeta,
    CodeMetadataForAsmJS::SeenSet* seenCodeMetaForAsmJS,
    Code::SeenSet* seenCode, size_t* code, size_t* data) const {
  auto p = seenCode->lookupForAdd(this);
  if (p) {
    return;
  }
  bool ok = seenCode->add(p, this);
  (void)ok;  // oh well

  auto guard = data_.readLock();
  *data +=
      mallocSizeOf(this) + guard->blocks.sizeOfExcludingThis(mallocSizeOf) +
      guard->blocksLinkData.sizeOfExcludingThis(mallocSizeOf) +
      guard->lazyExports.sizeOfExcludingThis(mallocSizeOf) +
      (codeMetaForAsmJS() ? codeMetaForAsmJS()->sizeOfIncludingThisIfNotSeen(
                                mallocSizeOf, seenCodeMetaForAsmJS)
                          : 0) +
      funcImports_.sizeOfExcludingThis(mallocSizeOf) +
      profilingLabels_.lock()->sizeOfExcludingThis(mallocSizeOf) +
      jumpTables_.sizeOfMiscExcludingThis();
  for (const SharedCodeSegment& stub : guard->lazyStubSegments) {
    stub->addSizeOfMisc(mallocSizeOf, code, data);
  }

  sharedStubs().addSizeOfMisc(mallocSizeOf, code, data);
  for (auto t : completeTiers()) {
    completeTierCodeBlock(t).addSizeOfMisc(mallocSizeOf, code, data);
  }
}

void CodeBlock::disassemble(JSContext* cx, int kindSelection,
                            PrintCallback printString) const {
  for (const CodeRange& range : codeRanges) {
    if (kindSelection & (1 << range.kind())) {
      MOZ_ASSERT(range.begin() < segment->lengthBytes());
      MOZ_ASSERT(range.end() < segment->lengthBytes());

      const char* kind;
      char kindbuf[128];
      switch (range.kind()) {
        case CodeRange::Function:
          kind = "Function";
          break;
        case CodeRange::InterpEntry:
          kind = "InterpEntry";
          break;
        case CodeRange::JitEntry:
          kind = "JitEntry";
          break;
        case CodeRange::ImportInterpExit:
          kind = "ImportInterpExit";
          break;
        case CodeRange::ImportJitExit:
          kind = "ImportJitExit";
          break;
        default:
          SprintfLiteral(kindbuf, "CodeRange::Kind(%d)", range.kind());
          kind = kindbuf;
          break;
      }
      const char* separator =
          "\n--------------------------------------------------\n";
      // The buffer is quite large in order to accomodate mangled C++ names;
      // lengths over 3500 have been observed in the wild.
      char buf[4096];
      if (range.hasFuncIndex()) {
        const char* funcName = "(unknown)";
        UTF8Bytes namebuf;
        bool ok;
        if (code->codeMetaForAsmJS()) {
          ok = code->codeMetaForAsmJS()->getFuncNameForAsmJS(range.funcIndex(),
                                                             &namebuf);
        } else {
          ok = code->codeMeta().getFuncNameForWasm(
              NameContext::Standalone, range.funcIndex(),
              code->codeTailMeta().nameSectionPayload.get(), &namebuf);
        }
        if (ok && namebuf.append('\0')) {
          funcName = namebuf.begin();
        }
        SprintfLiteral(buf, "%sKind = %s, index = %d, name = %s:\n", separator,
                       kind, range.funcIndex(), funcName);
      } else {
        SprintfLiteral(buf, "%sKind = %s\n", separator, kind);
      }
      printString(buf);

      uint8_t* theCode = base() + range.begin();
      jit::Disassemble(theCode, range.end() - range.begin(), printString);
    }
  }
}

void Code::disassemble(JSContext* cx, Tier tier, int kindSelection,
                       PrintCallback printString) const {
  this->sharedStubs().disassemble(cx, kindSelection, printString);
  this->completeTierCodeBlock(tier).disassemble(cx, kindSelection, printString);
}

// Return a map with names and associated statistics
MetadataAnalysisHashMap Code::metadataAnalysis(JSContext* cx) const {
  MetadataAnalysisHashMap hashmap;
  if (!hashmap.reserve(14)) {
    return hashmap;
  }

  for (auto t : completeTiers()) {
    const CodeBlock& codeBlock = completeTierCodeBlock(t);
    size_t length = codeBlock.funcToCodeRange.numEntries();
    length += codeBlock.codeRanges.length();
    length += codeBlock.callSites.length();
    length += codeBlock.trapSites.sumOfLengths();
    length += codeBlock.funcExports.length();
    length += codeBlock.stackMaps.length();
    length += codeBlock.tryNotes.length();

    hashmap.putNewInfallible("metadata length", length);

    // Iterate over the Code Ranges and accumulate all pieces of code.
    size_t code_size = 0;
    for (const CodeRange& codeRange : codeBlock.codeRanges) {
      if (!codeRange.isFunction()) {
        continue;
      }
      code_size += codeRange.end() - codeRange.begin();
    }

    hashmap.putNewInfallible("stackmaps number", codeBlock.stackMaps.length());
    hashmap.putNewInfallible("trapSites number",
                             codeBlock.trapSites.sumOfLengths());
    hashmap.putNewInfallible("codeRange size in bytes", code_size);
    hashmap.putNewInfallible("code segment capacity",
                             codeBlock.segment->capacityBytes());

    auto mallocSizeOf = cx->runtime()->debuggerMallocSizeOf;

    hashmap.putNewInfallible(
        "funcToCodeRange size",
        codeBlock.funcToCodeRange.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "codeRanges size",
        codeBlock.codeRanges.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "callSites size",
        codeBlock.callSites.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "tryNotes size", codeBlock.tryNotes.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "trapSites size",
        codeBlock.trapSites.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "stackMaps size",
        codeBlock.stackMaps.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "funcExports size",
        codeBlock.funcExports.sizeOfExcludingThis(mallocSizeOf));
  }

  return hashmap;
}

void wasm::PatchDebugSymbolicAccesses(uint8_t* codeBase, MacroAssembler& masm) {
#ifdef WASM_CODEGEN_DEBUG
  for (auto& access : masm.symbolicAccesses()) {
    switch (access.target) {
      case SymbolicAddress::PrintI32:
      case SymbolicAddress::PrintPtr:
      case SymbolicAddress::PrintF32:
      case SymbolicAddress::PrintF64:
      case SymbolicAddress::PrintText:
        break;
      default:
        MOZ_CRASH("unexpected symbol in PatchDebugSymbolicAccesses");
    }
    ABIFunctionType abiType;
    void* target = AddressOf(access.target, &abiType);
    uint8_t* patchAt = codeBase + access.patchAt.offset();
    Assembler::PatchDataWithValueCheck(CodeLocationLabel(patchAt),
                                       PatchedImmPtr(target),
                                       PatchedImmPtr((void*)-1));
  }
#else
  MOZ_ASSERT(masm.symbolicAccesses().empty());
#endif
}
