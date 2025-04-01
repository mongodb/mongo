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
#include "jit/MacroAssembler.h"
#include "jit/PerfSpewer.h"
#include "util/Poison.h"
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
using mozilla::BinarySearch;
using mozilla::BinarySearchIf;
using mozilla::MakeEnumeratedRange;
using mozilla::PodAssign;

size_t LinkData::SymbolicLinkArray::sizeOfExcludingThis(
    MallocSizeOf mallocSizeOf) const {
  size_t size = 0;
  for (const Uint32Vector& offsets : *this) {
    size += offsets.sizeOfExcludingThis(mallocSizeOf);
  }
  return size;
}

CodeSegment::~CodeSegment() {
  if (unregisterOnDestroy_) {
    UnregisterCodeSegment(this);
  }
}

static uint32_t RoundupCodeLength(uint32_t codeLength) {
  // AllocateExecutableMemory() requires a multiple of ExecutableCodePageSize.
  return RoundUp(codeLength, ExecutableCodePageSize);
}

UniqueCodeBytes wasm::AllocateCodeBytes(
    Maybe<AutoMarkJitCodeWritableForThread>& writable, uint32_t codeLength) {
  if (codeLength > MaxCodeBytesPerProcess) {
    return nullptr;
  }

  static_assert(MaxCodeBytesPerProcess <= INT32_MAX, "rounding won't overflow");
  uint32_t roundedCodeLength = RoundupCodeLength(codeLength);

  void* p =
      AllocateExecutableMemory(roundedCodeLength, ProtectionSetting::Writable,
                               MemCheckKind::MakeUndefined);

  // If the allocation failed and the embedding gives us a last-ditch attempt
  // to purge all memory (which, in gecko, does a purging GC/CC/GC), do that
  // then retry the allocation.
  if (!p) {
    if (OnLargeAllocationFailure) {
      OnLargeAllocationFailure();
      p = AllocateExecutableMemory(roundedCodeLength,
                                   ProtectionSetting::Writable,
                                   MemCheckKind::MakeUndefined);
    }
  }

  if (!p) {
    return nullptr;
  }

  // Construct AutoMarkJitCodeWritableForThread after allocating memory, to
  // ensure it's not nested (OnLargeAllocationFailure can trigger GC).
  writable.emplace();

  // Zero the padding.
  memset(((uint8_t*)p) + codeLength, 0, roundedCodeLength - codeLength);

  // We account for the bytes allocated in WasmModuleObject::create, where we
  // have the necessary JSContext.

  return UniqueCodeBytes((uint8_t*)p, FreeCode(roundedCodeLength));
}

bool CodeSegment::initialize(const CodeTier& codeTier) {
  MOZ_ASSERT(!initialized());
  codeTier_ = &codeTier;
  MOZ_ASSERT(initialized());

  // In the case of tiering, RegisterCodeSegment() immediately makes this code
  // segment live to access from other threads executing the containing
  // module. So only call once the CodeSegment is fully initialized.
  if (!RegisterCodeSegment(this)) {
    return false;
  }

  // This bool is only used by the destructor which cannot be called racily
  // and so it is not a problem to mutate it after RegisterCodeSegment().
  MOZ_ASSERT(!unregisterOnDestroy_);
  unregisterOnDestroy_ = true;
  return true;
}

const Code& CodeSegment::code() const {
  MOZ_ASSERT(codeTier_);
  return codeTier_->code();
}

void CodeSegment::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code) const {
  *code += RoundupCodeLength(length());
}

void FreeCode::operator()(uint8_t* bytes) {
  MOZ_ASSERT(codeLength);
  MOZ_ASSERT(codeLength == RoundupCodeLength(codeLength));

#ifdef MOZ_VTUNE
  vtune::UnmarkBytes(bytes, codeLength);
#endif
  DeallocateExecutableMemory(bytes, codeLength);
}

bool wasm::StaticallyLink(const ModuleSegment& ms, const LinkData& linkData) {
  if (!EnsureBuiltinThunksInitialized()) {
    return false;
  }

  AutoMarkJitCodeWritableForThread writable;

  for (LinkData::InternalLink link : linkData.internalLinks) {
    CodeLabel label;
    label.patchAt()->bind(link.patchAtOffset);
    label.target()->bind(link.targetOffset);
#ifdef JS_CODELABEL_LINKMODE
    label.setLinkMode(static_cast<CodeLabel::LinkMode>(link.mode));
#endif
    Assembler::Bind(ms.base(), label);
  }

  for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
    const Uint32Vector& offsets = linkData.symbolicLinks[imm];
    if (offsets.empty()) {
      continue;
    }

    void* target = SymbolicAddressTarget(imm);
    for (uint32_t offset : offsets) {
      uint8_t* patchAt = ms.base() + offset;
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

static bool AppendToString(const char* str, UTF8Bytes* bytes) {
  return bytes->append(str, strlen(str)) && bytes->append('\0');
}

static void SendCodeRangesToProfiler(const ModuleSegment& ms,
                                     const Metadata& metadata,
                                     const CodeRangeVector& codeRanges) {
  bool enabled = false;
  enabled |= PerfEnabled();
#ifdef MOZ_VTUNE
  enabled |= vtune::IsProfilingActive();
#endif
  if (!enabled) {
    return;
  }

  for (const CodeRange& codeRange : codeRanges) {
    if (!codeRange.hasFuncIndex()) {
      continue;
    }

    uintptr_t start = uintptr_t(ms.base() + codeRange.begin());
    uintptr_t size = codeRange.end() - codeRange.begin();

    UTF8Bytes name;
    if (!metadata.getFuncNameStandalone(codeRange.funcIndex(), &name)) {
      return;
    }

    // Avoid "unused" warnings
    (void)start;
    (void)size;

    if (PerfEnabled()) {
      const char* file = metadata.filename.get();
      if (codeRange.isFunction()) {
        if (!name.append('\0')) {
          return;
        }
        unsigned line = codeRange.funcLineOrBytecode();
        CollectPerfSpewerWasmFunctionMap(start, size, file, line, name.begin());
      } else if (codeRange.isInterpEntry()) {
        if (!AppendToString(" slow entry", &name)) {
          return;
        }
        CollectPerfSpewerWasmMap(start, size, file, name.begin());
      } else if (codeRange.isJitEntry()) {
        if (!AppendToString(" fast entry", &name)) {
          return;
        }
        CollectPerfSpewerWasmMap(start, size, file, name.begin());
      } else if (codeRange.isImportInterpExit()) {
        if (!AppendToString(" slow exit", &name)) {
          return;
        }
        CollectPerfSpewerWasmMap(start, size, file, name.begin());
      } else if (codeRange.isImportJitExit()) {
        if (!AppendToString(" fast exit", &name)) {
          return;
        }
        CollectPerfSpewerWasmMap(start, size, file, name.begin());
      } else {
        MOZ_CRASH("unhandled perf hasFuncIndex type");
      }
    }
#ifdef MOZ_VTUNE
    if (!vtune::IsProfilingActive()) {
      continue;
    }
    if (!codeRange.isFunction()) {
      continue;
    }
    if (!name.append('\0')) {
      return;
    }
    vtune::MarkWasm(vtune::GenerateUniqueMethodID(), name.begin(), (void*)start,
                    size);
#endif
  }
}

ModuleSegment::ModuleSegment(Tier tier, UniqueCodeBytes codeBytes,
                             uint32_t codeLength, const LinkData& linkData)
    : CodeSegment(std::move(codeBytes), codeLength, CodeSegment::Kind::Module),
      tier_(tier),
      trapCode_(base() + linkData.trapOffset) {}

/* static */
UniqueModuleSegment ModuleSegment::create(Tier tier, MacroAssembler& masm,
                                          const LinkData& linkData) {
  uint32_t codeLength = masm.bytesNeeded();

  Maybe<AutoMarkJitCodeWritableForThread> writable;
  UniqueCodeBytes codeBytes = AllocateCodeBytes(writable, codeLength);
  if (!codeBytes) {
    return nullptr;
  }

  masm.executableCopy(codeBytes.get());

  return js::MakeUnique<ModuleSegment>(tier, std::move(codeBytes), codeLength,
                                       linkData);
}

/* static */
UniqueModuleSegment ModuleSegment::create(Tier tier, const Bytes& unlinkedBytes,
                                          const LinkData& linkData) {
  uint32_t codeLength = unlinkedBytes.length();

  Maybe<AutoMarkJitCodeWritableForThread> writable;
  UniqueCodeBytes codeBytes = AllocateCodeBytes(writable, codeLength);
  if (!codeBytes) {
    return nullptr;
  }

  memcpy(codeBytes.get(), unlinkedBytes.begin(), codeLength);

  return js::MakeUnique<ModuleSegment>(tier, std::move(codeBytes), codeLength,
                                       linkData);
}

bool ModuleSegment::initialize(const CodeTier& codeTier,
                               const LinkData& linkData,
                               const Metadata& metadata,
                               const MetadataTier& metadataTier) {
  if (!StaticallyLink(*this, linkData)) {
    return false;
  }

  // Optimized compilation finishes on a background thread, so we must make sure
  // to flush the icaches of all the executing threads.
  // Reprotect the whole region to avoid having separate RW and RX mappings.
  if (!ExecutableAllocator::makeExecutableAndFlushICache(
          base(), RoundupCodeLength(length()))) {
    return false;
  }

  SendCodeRangesToProfiler(*this, metadata, metadataTier.codeRanges);

  // See comments in CodeSegment::initialize() for why this must be last.
  return CodeSegment::initialize(codeTier);
}

void ModuleSegment::addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf,
                                  size_t* code, size_t* data) const {
  CodeSegment::addSizeOfMisc(mallocSizeOf, code);
  *data += mallocSizeOf(this);
}

const CodeRange* ModuleSegment::lookupRange(const void* pc) const {
  return codeTier().lookupRange(pc);
}

size_t CacheableChars::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(get());
}

size_t MetadataTier::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return funcToCodeRange.sizeOfExcludingThis(mallocSizeOf) +
         codeRanges.sizeOfExcludingThis(mallocSizeOf) +
         callSites.sizeOfExcludingThis(mallocSizeOf) +
         tryNotes.sizeOfExcludingThis(mallocSizeOf) +
         codeRangeUnwindInfos.sizeOfExcludingThis(mallocSizeOf) +
         trapSites.sizeOfExcludingThis(mallocSizeOf) +
         stackMaps.sizeOfExcludingThis(mallocSizeOf) +
         funcImports.sizeOfExcludingThis(mallocSizeOf) +
         funcExports.sizeOfExcludingThis(mallocSizeOf);
}

UniqueLazyStubSegment LazyStubSegment::create(const CodeTier& codeTier,
                                              size_t length) {
  Maybe<AutoMarkJitCodeWritableForThread> writable;
  UniqueCodeBytes codeBytes = AllocateCodeBytes(writable, length);
  if (!codeBytes) {
    return nullptr;
  }

  auto segment = js::MakeUnique<LazyStubSegment>(std::move(codeBytes), length);
  if (!segment || !segment->initialize(codeTier)) {
    return nullptr;
  }

  return segment;
}

bool LazyStubSegment::hasSpace(size_t bytes) const {
  MOZ_ASSERT(AlignBytesNeeded(bytes) == bytes);
  return bytes <= length() && usedBytes_ <= length() - bytes;
}

bool LazyStubSegment::addStubs(const Metadata& metadata, size_t codeLength,
                               const Uint32Vector& funcExportIndices,
                               const FuncExportVector& funcExports,
                               const CodeRangeVector& codeRanges,
                               uint8_t** codePtr,
                               size_t* indexFirstInsertedCodeRange) {
  MOZ_ASSERT(hasSpace(codeLength));

  size_t offsetInSegment = usedBytes_;
  *codePtr = base() + usedBytes_;
  usedBytes_ += codeLength;

  *indexFirstInsertedCodeRange = codeRanges_.length();

  if (!codeRanges_.reserve(codeRanges_.length() + 2 * codeRanges.length())) {
    return false;
  }

  size_t i = 0;
  for (uint32_t funcExportIndex : funcExportIndices) {
    const FuncExport& fe = funcExports[funcExportIndex];
    const FuncType& funcType = metadata.getFuncExportType(fe);
    const CodeRange& interpRange = codeRanges[i];
    MOZ_ASSERT(interpRange.isInterpEntry());
    MOZ_ASSERT(interpRange.funcIndex() ==
               funcExports[funcExportIndex].funcIndex());

    codeRanges_.infallibleAppend(interpRange);
    codeRanges_.back().offsetBy(offsetInSegment);
    i++;

    if (!funcType.canHaveJitEntry()) {
      continue;
    }

    const CodeRange& jitRange = codeRanges[i];
    MOZ_ASSERT(jitRange.isJitEntry());
    MOZ_ASSERT(jitRange.funcIndex() == interpRange.funcIndex());

    codeRanges_.infallibleAppend(jitRange);
    codeRanges_.back().offsetBy(offsetInSegment);
    i++;
  }

  return true;
}

const CodeRange* LazyStubSegment::lookupRange(const void* pc) const {
  // Do not search if the search will not find anything.  There can be many
  // segments, each with many entries.
  if (pc < base() || pc >= base() + length()) {
    return nullptr;
  }
  return LookupInSorted(codeRanges_,
                        CodeRange::OffsetInCode((uint8_t*)pc - base()));
}

void LazyStubSegment::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code,
                                    size_t* data) const {
  CodeSegment::addSizeOfMisc(mallocSizeOf, code);
  *data += codeRanges_.sizeOfExcludingThis(mallocSizeOf);
  *data += mallocSizeOf(this);
}

// When allocating a single stub to a page, we should not always place the stub
// at the beginning of the page as the stubs will tend to thrash the icache by
// creating conflicts (everything ends up in the same cache set).  Instead,
// locate stubs at different line offsets up to 3/4 the system page size (the
// code allocation quantum).
//
// This may be called on background threads, hence the atomic.

static void PadCodeForSingleStub(MacroAssembler& masm) {
  // Assume 64B icache line size
  static uint8_t zeroes[64];

  // The counter serves only to spread the code out, it has no other meaning and
  // can wrap around.
  static mozilla::Atomic<uint32_t, mozilla::MemoryOrdering::ReleaseAcquire>
      counter(0);

  uint32_t maxPadLines = ((gc::SystemPageSize() * 3) / 4) / sizeof(zeroes);
  uint32_t padLines = counter++ % maxPadLines;
  for (uint32_t i = 0; i < padLines; i++) {
    masm.appendRawCode(zeroes, sizeof(zeroes));
  }
}

static constexpr unsigned LAZY_STUB_LIFO_DEFAULT_CHUNK_SIZE = 8 * 1024;

bool LazyStubTier::createManyEntryStubs(const Uint32Vector& funcExportIndices,
                                        const Metadata& metadata,
                                        const CodeTier& codeTier,
                                        size_t* stubSegmentIndex) {
  MOZ_ASSERT(funcExportIndices.length());

  LifoAlloc lifo(LAZY_STUB_LIFO_DEFAULT_CHUNK_SIZE);
  TempAllocator alloc(&lifo);
  JitContext jitContext;
  WasmMacroAssembler masm(alloc);

  if (funcExportIndices.length() == 1) {
    PadCodeForSingleStub(masm);
  }

  const MetadataTier& metadataTier = codeTier.metadata();
  const FuncExportVector& funcExports = metadataTier.funcExports;
  uint8_t* moduleSegmentBase = codeTier.segment().base();

  CodeRangeVector codeRanges;
  DebugOnly<uint32_t> numExpectedRanges = 0;
  for (uint32_t funcExportIndex : funcExportIndices) {
    const FuncExport& fe = funcExports[funcExportIndex];
    const FuncType& funcType = metadata.getFuncExportType(fe);
    // Exports that don't support a jit entry get only the interp entry.
    numExpectedRanges += (funcType.canHaveJitEntry() ? 2 : 1);
    void* calleePtr =
        moduleSegmentBase + metadataTier.codeRange(fe).funcUncheckedCallEntry();
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

  MOZ_ASSERT(masm.callSites().empty());
  MOZ_ASSERT(masm.callSiteTargets().empty());
  MOZ_ASSERT(masm.trapSites().empty());
  MOZ_ASSERT(masm.tryNotes().empty());
  MOZ_ASSERT(masm.codeRangeUnwindInfos().empty());

  if (masm.oom()) {
    return false;
  }

  size_t codeLength = LazyStubSegment::AlignBytesNeeded(masm.bytesNeeded());

  if (!stubSegments_.length() ||
      !stubSegments_[lastStubSegmentIndex_]->hasSpace(codeLength)) {
    size_t newSegmentSize = std::max(codeLength, ExecutableCodePageSize);
    UniqueLazyStubSegment newSegment =
        LazyStubSegment::create(codeTier, newSegmentSize);
    if (!newSegment) {
      return false;
    }
    lastStubSegmentIndex_ = stubSegments_.length();
    if (!stubSegments_.emplaceBack(std::move(newSegment))) {
      return false;
    }
  }

  LazyStubSegment* segment = stubSegments_[lastStubSegmentIndex_].get();
  *stubSegmentIndex = lastStubSegmentIndex_;

  size_t interpRangeIndex;
  uint8_t* codePtr = nullptr;
  if (!segment->addStubs(metadata, codeLength, funcExportIndices, funcExports,
                         codeRanges, &codePtr, &interpRangeIndex)) {
    return false;
  }

  {
    AutoMarkJitCodeWritableForThread writable;
    masm.executableCopy(codePtr);
    PatchDebugSymbolicAccesses(codePtr, masm);
    memset(codePtr + masm.bytesNeeded(), 0, codeLength - masm.bytesNeeded());

    for (const CodeLabel& label : masm.codeLabels()) {
      Assembler::Bind(codePtr, label);
    }
  }

  if (!ExecutableAllocator::makeExecutableAndFlushICache(codePtr, codeLength)) {
    return false;
  }

  // Create lazy function exports for funcIndex -> entry lookup.
  if (!exports_.reserve(exports_.length() + funcExportIndices.length())) {
    return false;
  }

  for (uint32_t funcExportIndex : funcExportIndices) {
    const FuncExport& fe = funcExports[funcExportIndex];
    const FuncType& funcType = metadata.getFuncExportType(fe);

    DebugOnly<CodeRange> cr = segment->codeRanges()[interpRangeIndex];
    MOZ_ASSERT(cr.value.isInterpEntry());
    MOZ_ASSERT(cr.value.funcIndex() == fe.funcIndex());

    LazyFuncExport lazyExport(fe.funcIndex(), *stubSegmentIndex,
                              interpRangeIndex);

    size_t exportIndex;
    const uint32_t targetFunctionIndex = fe.funcIndex();
    MOZ_ALWAYS_FALSE(BinarySearchIf(
        exports_, 0, exports_.length(),
        [targetFunctionIndex](const LazyFuncExport& funcExport) {
          return targetFunctionIndex - funcExport.funcIndex;
        },
        &exportIndex));
    MOZ_ALWAYS_TRUE(
        exports_.insert(exports_.begin() + exportIndex, std::move(lazyExport)));

    // Exports that don't support a jit entry get only the interp entry.
    interpRangeIndex += (funcType.canHaveJitEntry() ? 2 : 1);
  }

  return true;
}

bool LazyStubTier::createOneEntryStub(uint32_t funcExportIndex,
                                      const Metadata& metadata,
                                      const CodeTier& codeTier) {
  Uint32Vector funcExportIndexes;
  if (!funcExportIndexes.append(funcExportIndex)) {
    return false;
  }

  size_t stubSegmentIndex;
  if (!createManyEntryStubs(funcExportIndexes, metadata, codeTier,
                            &stubSegmentIndex)) {
    return false;
  }

  const UniqueLazyStubSegment& segment = stubSegments_[stubSegmentIndex];
  const CodeRangeVector& codeRanges = segment->codeRanges();

  const FuncExport& fe = codeTier.metadata().funcExports[funcExportIndex];
  const FuncType& funcType = metadata.getFuncExportType(fe);

  // Exports that don't support a jit entry get only the interp entry.
  if (!funcType.canHaveJitEntry()) {
    MOZ_ASSERT(codeRanges.length() >= 1);
    MOZ_ASSERT(codeRanges.back().isInterpEntry());
    return true;
  }

  MOZ_ASSERT(codeRanges.length() >= 2);
  MOZ_ASSERT(codeRanges[codeRanges.length() - 2].isInterpEntry());

  const CodeRange& cr = codeRanges[codeRanges.length() - 1];
  MOZ_ASSERT(cr.isJitEntry());

  codeTier.code().setJitEntry(cr.funcIndex(), segment->base() + cr.begin());
  return true;
}

bool LazyStubTier::createTier2(const Uint32Vector& funcExportIndices,
                               const Metadata& metadata,
                               const CodeTier& codeTier,
                               Maybe<size_t>* outStubSegmentIndex) {
  if (!funcExportIndices.length()) {
    return true;
  }

  size_t stubSegmentIndex;
  if (!createManyEntryStubs(funcExportIndices, metadata, codeTier,
                            &stubSegmentIndex)) {
    return false;
  }

  outStubSegmentIndex->emplace(stubSegmentIndex);
  return true;
}

void LazyStubTier::setJitEntries(const Maybe<size_t>& stubSegmentIndex,
                                 const Code& code) {
  if (!stubSegmentIndex) {
    return;
  }
  const UniqueLazyStubSegment& segment = stubSegments_[*stubSegmentIndex];
  for (const CodeRange& cr : segment->codeRanges()) {
    if (!cr.isJitEntry()) {
      continue;
    }
    code.setJitEntry(cr.funcIndex(), segment->base() + cr.begin());
  }
}

bool LazyStubTier::hasEntryStub(uint32_t funcIndex) const {
  size_t match;
  return BinarySearchIf(
      exports_, 0, exports_.length(),
      [funcIndex](const LazyFuncExport& funcExport) {
        return funcIndex - funcExport.funcIndex;
      },
      &match);
}

void* LazyStubTier::lookupInterpEntry(uint32_t funcIndex) const {
  size_t match;
  if (!BinarySearchIf(
          exports_, 0, exports_.length(),
          [funcIndex](const LazyFuncExport& funcExport) {
            return funcIndex - funcExport.funcIndex;
          },
          &match)) {
    return nullptr;
  }
  const LazyFuncExport& fe = exports_[match];
  const LazyStubSegment& stub = *stubSegments_[fe.lazyStubSegmentIndex];
  return stub.base() + stub.codeRanges()[fe.funcCodeRangeIndex].begin();
}

void LazyStubTier::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code,
                                 size_t* data) const {
  *data += sizeof(*this);
  *data += exports_.sizeOfExcludingThis(mallocSizeOf);
  for (const UniqueLazyStubSegment& stub : stubSegments_) {
    stub->addSizeOfMisc(mallocSizeOf, code, data);
  }
}

size_t Metadata::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return types->sizeOfExcludingThis(mallocSizeOf) +
         globals.sizeOfExcludingThis(mallocSizeOf) +
         tables.sizeOfExcludingThis(mallocSizeOf) +
         tags.sizeOfExcludingThis(mallocSizeOf) +
         funcNames.sizeOfExcludingThis(mallocSizeOf) +
         filename.sizeOfExcludingThis(mallocSizeOf) +
         sourceMapURL.sizeOfExcludingThis(mallocSizeOf);
}

struct ProjectFuncIndex {
  const FuncExportVector& funcExports;
  explicit ProjectFuncIndex(const FuncExportVector& funcExports)
      : funcExports(funcExports) {}
  uint32_t operator[](size_t index) const {
    return funcExports[index].funcIndex();
  }
};

FuncExport& MetadataTier::lookupFuncExport(
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

const FuncExport& MetadataTier::lookupFuncExport(
    uint32_t funcIndex, size_t* funcExportIndex) const {
  return const_cast<MetadataTier*>(this)->lookupFuncExport(funcIndex,
                                                           funcExportIndex);
}

static bool AppendName(const Bytes& namePayload, const Name& name,
                       UTF8Bytes* bytes) {
  MOZ_RELEASE_ASSERT(name.offsetInNamePayload <= namePayload.length());
  MOZ_RELEASE_ASSERT(name.length <=
                     namePayload.length() - name.offsetInNamePayload);
  return bytes->append(
      (const char*)namePayload.begin() + name.offsetInNamePayload, name.length);
}

static bool AppendFunctionIndexName(uint32_t funcIndex, UTF8Bytes* bytes) {
  const char beforeFuncIndex[] = "wasm-function[";
  const char afterFuncIndex[] = "]";

  Int32ToCStringBuf cbuf;
  size_t funcIndexStrLen;
  const char* funcIndexStr =
      Uint32ToCString(&cbuf, funcIndex, &funcIndexStrLen);
  MOZ_ASSERT(funcIndexStr);

  return bytes->append(beforeFuncIndex, strlen(beforeFuncIndex)) &&
         bytes->append(funcIndexStr, funcIndexStrLen) &&
         bytes->append(afterFuncIndex, strlen(afterFuncIndex));
}

bool Metadata::getFuncName(NameContext ctx, uint32_t funcIndex,
                           UTF8Bytes* name) const {
  if (moduleName && moduleName->length != 0) {
    if (!AppendName(namePayload->bytes, *moduleName, name)) {
      return false;
    }
    if (!name->append('.')) {
      return false;
    }
  }

  if (funcIndex < funcNames.length() && funcNames[funcIndex].length != 0) {
    return AppendName(namePayload->bytes, funcNames[funcIndex], name);
  }

  if (ctx == NameContext::BeforeLocation) {
    return true;
  }

  return AppendFunctionIndexName(funcIndex, name);
}

bool CodeTier::initialize(const Code& code, const LinkData& linkData,
                          const Metadata& metadata) {
  MOZ_ASSERT(!initialized());
  code_ = &code;

  MOZ_ASSERT(lazyStubs_.readLock()->entryStubsEmpty());

  // See comments in CodeSegment::initialize() for why this must be last.
  if (!segment_->initialize(*this, linkData, metadata, *metadata_)) {
    return false;
  }

  MOZ_ASSERT(initialized());
  return true;
}

void CodeTier::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code,
                             size_t* data) const {
  segment_->addSizeOfMisc(mallocSizeOf, code, data);
  lazyStubs_.readLock()->addSizeOfMisc(mallocSizeOf, code, data);
  *data += metadata_->sizeOfExcludingThis(mallocSizeOf);
}

const CodeRange* CodeTier::lookupRange(const void* pc) const {
  CodeRange::OffsetInCode target((uint8_t*)pc - segment_->base());
  return LookupInSorted(metadata_->codeRanges, target);
}

const wasm::TryNote* CodeTier::lookupTryNote(const void* pc) const {
  size_t target = (uint8_t*)pc - segment_->base();
  const TryNoteVector& tryNotes = metadata_->tryNotes;

  // We find the first hit (there may be multiple) to obtain the innermost
  // handler, which is why we cannot binary search here.
  for (const auto& tryNote : tryNotes) {
    if (tryNote.offsetWithinTryBody(target)) {
      return &tryNote;
    }
  }

  return nullptr;
}

bool JumpTables::init(CompileMode mode, const ModuleSegment& ms,
                      const CodeRangeVector& codeRanges) {
  static_assert(JSScript::offsetOfJitCodeRaw() == 0,
                "wasm fast jit entry is at (void*) jit[funcIndex]");

  mode_ = mode;

  size_t numFuncs = 0;
  for (const CodeRange& cr : codeRanges) {
    if (cr.isFunction()) {
      numFuncs++;
    }
  }

  numFuncs_ = numFuncs;

  if (mode_ == CompileMode::Tier1) {
    tiering_ = TablePointer(js_pod_calloc<void*>(numFuncs));
    if (!tiering_) {
      return false;
    }
  }

  // The number of jit entries is overestimated, but it is simpler when
  // filling/looking up the jit entries and safe (worst case we'll crash
  // because of a null deref when trying to call the jit entry of an
  // unexported function).
  jit_ = TablePointer(js_pod_calloc<void*>(numFuncs));
  if (!jit_) {
    return false;
  }

  uint8_t* codeBase = ms.base();
  for (const CodeRange& cr : codeRanges) {
    if (cr.isFunction()) {
      setTieringEntry(cr.funcIndex(), codeBase + cr.funcTierEntry());
    } else if (cr.isJitEntry()) {
      setJitEntry(cr.funcIndex(), codeBase + cr.begin());
    }
  }
  return true;
}

Code::Code(UniqueCodeTier tier1, const Metadata& metadata,
           JumpTables&& maybeJumpTables)
    : tier1_(std::move(tier1)),
      metadata_(&metadata),
      profilingLabels_(mutexid::WasmCodeProfilingLabels,
                       CacheableCharsVector()),
      jumpTables_(std::move(maybeJumpTables)) {}

bool Code::initialize(const LinkData& linkData) {
  MOZ_ASSERT(!initialized());

  if (!tier1_->initialize(*this, linkData, *metadata_)) {
    return false;
  }

  MOZ_ASSERT(initialized());
  return true;
}

bool Code::setAndBorrowTier2(UniqueCodeTier tier2, const LinkData& linkData,
                             const CodeTier** borrowedTier) const {
  MOZ_RELEASE_ASSERT(!hasTier2());
  MOZ_RELEASE_ASSERT(tier2->tier() == Tier::Optimized &&
                     tier1_->tier() == Tier::Baseline);

  if (!tier2->initialize(*this, linkData, *metadata_)) {
    return false;
  }

  tier2_ = std::move(tier2);
  *borrowedTier = &*tier2_;

  return true;
}

void Code::commitTier2() const {
  MOZ_RELEASE_ASSERT(!hasTier2());
  hasTier2_ = true;
  MOZ_ASSERT(hasTier2());

  // To maintain the invariant that tier2_ is never read without the tier having
  // been committed, this checks tier2_ here instead of before setting hasTier2_
  // (as would be natural).  See comment in WasmCode.h.
  MOZ_RELEASE_ASSERT(tier2_.get());
}

uint32_t Code::getFuncIndex(JSFunction* fun) const {
  MOZ_ASSERT(fun->isWasm() || fun->isAsmJSNative());
  if (!fun->isWasmWithJitEntry()) {
    return fun->wasmFuncIndex();
  }
  return jumpTables_.funcIndexFromJitEntry(fun->wasmJitEntry());
}

Tiers Code::tiers() const {
  if (hasTier2()) {
    return Tiers(tier1_->tier(), tier2_->tier());
  }
  return Tiers(tier1_->tier());
}

bool Code::hasTier(Tier t) const {
  if (hasTier2() && tier2_->tier() == t) {
    return true;
  }
  return tier1_->tier() == t;
}

Tier Code::stableTier() const { return tier1_->tier(); }

Tier Code::bestTier() const {
  if (hasTier2()) {
    return tier2_->tier();
  }
  return tier1_->tier();
}

const CodeTier& Code::codeTier(Tier tier) const {
  switch (tier) {
    case Tier::Baseline:
      if (tier1_->tier() == Tier::Baseline) {
        MOZ_ASSERT(tier1_->initialized());
        return *tier1_;
      }
      MOZ_CRASH("No code segment at this tier");
    case Tier::Optimized:
      if (tier1_->tier() == Tier::Optimized) {
        MOZ_ASSERT(tier1_->initialized());
        return *tier1_;
      }
      // It is incorrect to ask for the optimized tier without there being such
      // a tier and the tier having been committed.  The guard here could
      // instead be `if (hasTier2()) ... ` but codeTier(t) should not be called
      // in contexts where that test is necessary.
      MOZ_RELEASE_ASSERT(hasTier2());
      MOZ_ASSERT(tier2_->initialized());
      return *tier2_;
  }
  MOZ_CRASH();
}

bool Code::containsCodePC(const void* pc) const {
  for (Tier t : tiers()) {
    const ModuleSegment& ms = segment(t);
    if (ms.containsCodePC(pc)) {
      return true;
    }
  }
  return false;
}

struct CallSiteRetAddrOffset {
  const CallSiteVector& callSites;
  explicit CallSiteRetAddrOffset(const CallSiteVector& callSites)
      : callSites(callSites) {}
  uint32_t operator[](size_t index) const {
    return callSites[index].returnAddressOffset();
  }
};

const CallSite* Code::lookupCallSite(void* returnAddress) const {
  for (Tier t : tiers()) {
    uint32_t target = ((uint8_t*)returnAddress) - segment(t).base();
    size_t lowerBound = 0;
    size_t upperBound = metadata(t).callSites.length();

    size_t match;
    if (BinarySearch(CallSiteRetAddrOffset(metadata(t).callSites), lowerBound,
                     upperBound, target, &match)) {
      return &metadata(t).callSites[match];
    }
  }

  return nullptr;
}

const CodeRange* Code::lookupFuncRange(void* pc) const {
  for (Tier t : tiers()) {
    const CodeRange* result = codeTier(t).lookupRange(pc);
    if (result && result->isFunction()) {
      return result;
    }
  }
  return nullptr;
}

const StackMap* Code::lookupStackMap(uint8_t* nextPC) const {
  for (Tier t : tiers()) {
    const StackMap* result = metadata(t).stackMaps.findMap(nextPC);
    if (result) {
      return result;
    }
  }
  return nullptr;
}

const wasm::TryNote* Code::lookupTryNote(void* pc, Tier* tier) const {
  for (Tier t : tiers()) {
    const TryNote* result = codeTier(t).lookupTryNote(pc);
    if (result) {
      *tier = t;
      return result;
    }
  }
  return nullptr;
}

struct TrapSitePCOffset {
  const TrapSiteVector& trapSites;
  explicit TrapSitePCOffset(const TrapSiteVector& trapSites)
      : trapSites(trapSites) {}
  uint32_t operator[](size_t index) const { return trapSites[index].pcOffset; }
};

bool Code::lookupTrap(void* pc, Trap* trapOut, BytecodeOffset* bytecode) const {
  for (Tier t : tiers()) {
    uint32_t target = ((uint8_t*)pc) - segment(t).base();
    const TrapSiteVectorArray& trapSitesArray = metadata(t).trapSites;
    for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
      const TrapSiteVector& trapSites = trapSitesArray[trap];

      size_t upperBound = trapSites.length();
      size_t match;
      if (BinarySearch(TrapSitePCOffset(trapSites), 0, upperBound, target,
                       &match)) {
        MOZ_ASSERT(segment(t).containsCodePC(pc));
        *trapOut = trap;
        *bytecode = trapSites[match].bytecode;
        return true;
      }
    }
  }

  return false;
}

bool Code::lookupFunctionTier(const CodeRange* codeRange, Tier* tier) const {
  // This logic only works if the codeRange is a function, and therefore only
  // exists in metadata and not a lazy stub tier. Generalizing to access lazy
  // stubs would require taking a lock, which is undesirable for the profiler.
  MOZ_ASSERT(codeRange->isFunction());
  for (Tier t : tiers()) {
    const CodeTier& code = codeTier(t);
    const MetadataTier& metadata = code.metadata();
    if (codeRange >= metadata.codeRanges.begin() &&
        codeRange < metadata.codeRanges.end()) {
      *tier = t;
      return true;
    }
  }
  return false;
}

struct UnwindInfoPCOffset {
  const CodeRangeUnwindInfoVector& info;
  explicit UnwindInfoPCOffset(const CodeRangeUnwindInfoVector& info)
      : info(info) {}
  uint32_t operator[](size_t index) const { return info[index].offset(); }
};

const CodeRangeUnwindInfo* Code::lookupUnwindInfo(void* pc) const {
  for (Tier t : tiers()) {
    uint32_t target = ((uint8_t*)pc) - segment(t).base();
    const CodeRangeUnwindInfoVector& unwindInfoArray =
        metadata(t).codeRangeUnwindInfos;
    size_t match;
    const CodeRangeUnwindInfo* info = nullptr;
    if (BinarySearch(UnwindInfoPCOffset(unwindInfoArray), 0,
                     unwindInfoArray.length(), target, &match)) {
      info = &unwindInfoArray[match];
    } else {
      // Exact match is not found, using insertion point to get the previous
      // info entry; skip if info is outside of codeRangeUnwindInfos.
      if (match == 0) continue;
      if (match == unwindInfoArray.length()) {
        MOZ_ASSERT(unwindInfoArray[unwindInfoArray.length() - 1].unwindHow() ==
                   CodeRangeUnwindInfo::Normal);
        continue;
      }
      info = &unwindInfoArray[match - 1];
    }
    return info->unwindHow() == CodeRangeUnwindInfo::Normal ? nullptr : info;
  }
  return nullptr;
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

  for (const CodeRange& codeRange : metadata(stableTier()).codeRanges) {
    if (!codeRange.isFunction()) {
      continue;
    }

    Int32ToCStringBuf cbuf;
    size_t bytecodeStrLen;
    const char* bytecodeStr =
        Uint32ToCString(&cbuf, codeRange.funcLineOrBytecode(), &bytecodeStrLen);
    MOZ_ASSERT(bytecodeStr);

    UTF8Bytes name;
    if (!metadata().getFuncNameStandalone(codeRange.funcIndex(), &name)) {
      return;
    }
    if (!name.append(" (", 2)) {
      return;
    }

    if (const char* filename = metadata().filename.get()) {
      if (!name.append(filename, strlen(filename))) {
        return;
      }
    } else {
      if (!name.append('?')) {
        return;
      }
    }

    if (!name.append(':') || !name.append(bytecodeStr, bytecodeStrLen) ||
        !name.append(")\0", 2)) {
      return;
    }

    UniqueChars label(name.extractOrCopyRawBuffer());
    if (!label) {
      return;
    }

    if (codeRange.funcIndex() >= labels->length()) {
      if (!labels->resize(codeRange.funcIndex() + 1)) {
        return;
      }
    }

    ((CacheableCharsVector&)labels)[codeRange.funcIndex()] = std::move(label);
  }
}

const char* Code::profilingLabel(uint32_t funcIndex) const {
  auto labels = profilingLabels_.lock();

  if (funcIndex >= labels->length() ||
      !((CacheableCharsVector&)labels)[funcIndex]) {
    return "?";
  }
  return ((CacheableCharsVector&)labels)[funcIndex].get();
}

void Code::addSizeOfMiscIfNotSeen(MallocSizeOf mallocSizeOf,
                                  Metadata::SeenSet* seenMetadata,
                                  Code::SeenSet* seenCode, size_t* code,
                                  size_t* data) const {
  auto p = seenCode->lookupForAdd(this);
  if (p) {
    return;
  }
  bool ok = seenCode->add(p, this);
  (void)ok;  // oh well

  *data += mallocSizeOf(this) +
           metadata().sizeOfIncludingThisIfNotSeen(mallocSizeOf, seenMetadata) +
           profilingLabels_.lock()->sizeOfExcludingThis(mallocSizeOf) +
           jumpTables_.sizeOfMiscExcludingThis();

  for (auto t : tiers()) {
    codeTier(t).addSizeOfMisc(mallocSizeOf, code, data);
  }
}

void Code::disassemble(JSContext* cx, Tier tier, int kindSelection,
                       PrintCallback printString) const {
  const MetadataTier& metadataTier = metadata(tier);
  const CodeTier& codeTier = this->codeTier(tier);
  const ModuleSegment& segment = codeTier.segment();

  for (const CodeRange& range : metadataTier.codeRanges) {
    if (kindSelection & (1 << range.kind())) {
      MOZ_ASSERT(range.begin() < segment.length());
      MOZ_ASSERT(range.end() < segment.length());

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
        if (metadata().getFuncNameStandalone(range.funcIndex(), &namebuf) &&
            namebuf.append('\0')) {
          funcName = namebuf.begin();
        }
        SprintfLiteral(buf, "%sKind = %s, index = %d, name = %s:\n", separator,
                       kind, range.funcIndex(), funcName);
      } else {
        SprintfLiteral(buf, "%sKind = %s\n", separator, kind);
      }
      printString(buf);

      uint8_t* theCode = segment.base() + range.begin();
      jit::Disassemble(theCode, range.end() - range.begin(), printString);
    }
  }
}

// Return a map with names and associated statistics
MetadataAnalysisHashMap Code::metadataAnalysis(JSContext* cx) const {
  MetadataAnalysisHashMap hashmap;
  if (!hashmap.reserve(15)) {
    return hashmap;
  }

  for (auto t : tiers()) {
    size_t length = metadata(t).funcToCodeRange.length();
    length += metadata(t).codeRanges.length();
    length += metadata(t).callSites.length();
    length += metadata(t).trapSites.sumOfLengths();
    length += metadata(t).funcImports.length();
    length += metadata(t).funcExports.length();
    length += metadata(t).stackMaps.length();
    length += metadata(t).tryNotes.length();

    hashmap.putNewInfallible("metadata length", length);

    // Iterate over the Code Ranges and accumulate all pieces of code.
    size_t code_size = 0;
    for (const CodeRange& codeRange : metadata(stableTier()).codeRanges) {
      if (!codeRange.isFunction()) {
        continue;
      }
      code_size += codeRange.end() - codeRange.begin();
    }

    hashmap.putNewInfallible("stackmaps number",
                             this->metadata(t).stackMaps.length());
    hashmap.putNewInfallible("trapSites number",
                             this->metadata(t).trapSites.sumOfLengths());
    hashmap.putNewInfallible("codeRange size in bytes", code_size);
    hashmap.putNewInfallible("code segment length",
                             this->codeTier(t).segment().length());

    auto mallocSizeOf = cx->runtime()->debuggerMallocSizeOf;

    hashmap.putNewInfallible("metadata total size",
                             metadata(t).sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "funcToCodeRange size",
        metadata(t).funcToCodeRange.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "codeRanges size",
        metadata(t).codeRanges.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "callSites size",
        metadata(t).callSites.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "tryNotes size",
        metadata(t).tryNotes.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "trapSites size",
        metadata(t).trapSites.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "stackMaps size",
        metadata(t).stackMaps.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "funcImports size",
        metadata(t).funcImports.sizeOfExcludingThis(mallocSizeOf));
    hashmap.putNewInfallible(
        "funcExports size",
        metadata(t).funcExports.sizeOfExcludingThis(mallocSizeOf));
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
