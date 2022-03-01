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

#include "mozilla/BinarySearch.h"
#include "mozilla/EnumeratedRange.h"

#include <algorithm>

#include "jsnum.h"

#include "jit/Disassemble.h"
#include "jit/ExecutableAllocator.h"
#ifdef JS_ION_PERF
#  include "jit/PerfSpewer.h"
#endif
#include "util/Poison.h"
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif
#include "wasm/WasmModule.h"
#include "wasm/WasmProcess.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmStubs.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;
using mozilla::BinarySearch;
using mozilla::MakeEnumeratedRange;
using mozilla::PodAssign;

size_t LinkData::SymbolicLinkArray::serializedSize() const {
  size_t size = 0;
  for (const Uint32Vector& offsets : *this) {
    size += SerializedPodVectorSize(offsets);
  }
  return size;
}

uint8_t* LinkData::SymbolicLinkArray::serialize(uint8_t* cursor) const {
  for (const Uint32Vector& offsets : *this) {
    cursor = SerializePodVector(cursor, offsets);
  }
  return cursor;
}

const uint8_t* LinkData::SymbolicLinkArray::deserialize(const uint8_t* cursor) {
  for (Uint32Vector& offsets : *this) {
    cursor = DeserializePodVector(cursor, &offsets);
    if (!cursor) {
      return nullptr;
    }
  }
  return cursor;
}

size_t LinkData::SymbolicLinkArray::sizeOfExcludingThis(
    MallocSizeOf mallocSizeOf) const {
  size_t size = 0;
  for (const Uint32Vector& offsets : *this) {
    size += offsets.sizeOfExcludingThis(mallocSizeOf);
  }
  return size;
}

size_t LinkData::serializedSize() const {
  return sizeof(pod()) + SerializedPodVectorSize(internalLinks) +
         symbolicLinks.serializedSize();
}

uint8_t* LinkData::serialize(uint8_t* cursor) const {
  MOZ_ASSERT(tier == Tier::Serialized);

  cursor = WriteBytes(cursor, &pod(), sizeof(pod()));
  cursor = SerializePodVector(cursor, internalLinks);
  cursor = symbolicLinks.serialize(cursor);
  return cursor;
}

const uint8_t* LinkData::deserialize(const uint8_t* cursor) {
  MOZ_ASSERT(tier == Tier::Serialized);

  (cursor = ReadBytes(cursor, &pod(), sizeof(pod()))) &&
      (cursor = DeserializePodVector(cursor, &internalLinks)) &&
      (cursor = symbolicLinks.deserialize(cursor));
  return cursor;
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

/* static */
UniqueCodeBytes CodeSegment::AllocateCodeBytes(uint32_t codeLength) {
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

static bool StaticallyLink(const ModuleSegment& ms, const LinkData& linkData) {
  for (LinkData::InternalLink link : linkData.internalLinks) {
    CodeLabel label;
    label.patchAt()->bind(link.patchAtOffset);
    label.target()->bind(link.targetOffset);
#ifdef JS_CODELABEL_LINKMODE
    label.setLinkMode(static_cast<CodeLabel::LinkMode>(link.mode));
#endif
    Assembler::Bind(ms.base(), label);
  }

  if (!EnsureBuiltinThunksInitialized()) {
    return false;
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

static void StaticallyUnlink(uint8_t* base, const LinkData& linkData) {
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

#ifdef JS_ION_PERF
static bool AppendToString(const char* str, UTF8Bytes* bytes) {
  return bytes->append(str, strlen(str)) && bytes->append('\0');
}
#endif

static void SendCodeRangesToProfiler(const ModuleSegment& ms,
                                     const Metadata& metadata,
                                     const CodeRangeVector& codeRanges) {
  bool enabled = false;
#ifdef JS_ION_PERF
  enabled |= PerfFuncEnabled();
#endif
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

#ifdef JS_ION_PERF
    if (PerfFuncEnabled()) {
      const char* file = metadata.filename.get();
      if (codeRange.isFunction()) {
        if (!name.append('\0')) {
          return;
        }
        unsigned line = codeRange.funcLineOrBytecode();
        writePerfSpewerWasmFunctionMap(start, size, file, line, name.begin());
      } else if (codeRange.isInterpEntry()) {
        if (!AppendToString(" slow entry", &name)) {
          return;
        }
        writePerfSpewerWasmMap(start, size, file, name.begin());
      } else if (codeRange.isJitEntry()) {
        if (!AppendToString(" fast entry", &name)) {
          return;
        }
        writePerfSpewerWasmMap(start, size, file, name.begin());
      } else if (codeRange.isImportInterpExit()) {
        if (!AppendToString(" slow exit", &name)) {
          return;
        }
        writePerfSpewerWasmMap(start, size, file, name.begin());
      } else if (codeRange.isImportJitExit()) {
        if (!AppendToString(" fast exit", &name)) {
          return;
        }
        writePerfSpewerWasmMap(start, size, file, name.begin());
      } else {
        MOZ_CRASH("unhandled perf hasFuncIndex type");
      }
    }
#endif
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

  UniqueCodeBytes codeBytes = AllocateCodeBytes(codeLength);
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

  UniqueCodeBytes codeBytes = AllocateCodeBytes(codeLength);
  if (!codeBytes) {
    return nullptr;
  }

  memcpy(codeBytes.get(), unlinkedBytes.begin(), codeLength);

  return js::MakeUnique<ModuleSegment>(tier, std::move(codeBytes), codeLength,
                                       linkData);
}

bool ModuleSegment::initialize(IsTier2 isTier2, const CodeTier& codeTier,
                               const LinkData& linkData,
                               const Metadata& metadata,
                               const MetadataTier& metadataTier) {
  if (!StaticallyLink(*this, linkData)) {
    return false;
  }

  // Optimized compilation finishes on a background thread, so we must make sure
  // to flush the icaches of all the executing threads.
  FlushICacheSpec flushIcacheSpec = isTier2 == IsTier2::Tier2
                                        ? FlushICacheSpec::AllThreads
                                        : FlushICacheSpec::LocalThreadOnly;

  // Reprotect the whole region to avoid having separate RW and RX mappings.
  if (!ExecutableAllocator::makeExecutableAndFlushICache(
          flushIcacheSpec, base(), RoundupCodeLength(length()))) {
    return false;
  }

  SendCodeRangesToProfiler(*this, metadata, metadataTier.codeRanges);

  // See comments in CodeSegment::initialize() for why this must be last.
  return CodeSegment::initialize(codeTier);
}

size_t ModuleSegment::serializedSize() const {
  return sizeof(uint32_t) + length();
}

void ModuleSegment::addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf,
                                  size_t* code, size_t* data) const {
  CodeSegment::addSizeOfMisc(mallocSizeOf, code);
  *data += mallocSizeOf(this);
}

uint8_t* ModuleSegment::serialize(uint8_t* cursor,
                                  const LinkData& linkData) const {
  MOZ_ASSERT(tier() == Tier::Serialized);

  cursor = WriteScalar<uint32_t>(cursor, length());
  uint8_t* serializedBase = cursor;
  cursor = WriteBytes(cursor, base(), length());
  StaticallyUnlink(serializedBase, linkData);
  return cursor;
}

/* static */ const uint8_t* ModuleSegment::deserialize(
    const uint8_t* cursor, const LinkData& linkData,
    UniqueModuleSegment* segment) {
  uint32_t length;
  cursor = ReadScalar<uint32_t>(cursor, &length);
  if (!cursor) {
    return nullptr;
  }

  UniqueCodeBytes bytes = AllocateCodeBytes(length);
  if (!bytes) {
    return nullptr;
  }

  cursor = ReadBytes(cursor, bytes.get(), length);
  if (!cursor) {
    return nullptr;
  }

  *segment = js::MakeUnique<ModuleSegment>(Tier::Serialized, std::move(bytes),
                                           length, linkData);
  if (!*segment) {
    return nullptr;
  }

  return cursor;
}

const CodeRange* ModuleSegment::lookupRange(const void* pc) const {
  return codeTier().lookupRange(pc);
}

size_t FuncExport::serializedSize() const {
  return funcType_.serializedSize() + sizeof(pod);
}

uint8_t* FuncExport::serialize(uint8_t* cursor) const {
  cursor = funcType_.serialize(cursor);
  cursor = WriteBytes(cursor, &pod, sizeof(pod));
  return cursor;
}

const uint8_t* FuncExport::deserialize(const uint8_t* cursor) {
  (cursor = funcType_.deserialize(cursor)) &&
      (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
  return cursor;
}

size_t FuncExport::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return funcType_.sizeOfExcludingThis(mallocSizeOf);
}

size_t FuncImport::serializedSize() const {
  return funcType_.serializedSize() + sizeof(pod);
}

uint8_t* FuncImport::serialize(uint8_t* cursor) const {
  cursor = funcType_.serialize(cursor);
  cursor = WriteBytes(cursor, &pod, sizeof(pod));
  return cursor;
}

const uint8_t* FuncImport::deserialize(const uint8_t* cursor) {
  (cursor = funcType_.deserialize(cursor)) &&
      (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
  return cursor;
}

size_t FuncImport::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return funcType_.sizeOfExcludingThis(mallocSizeOf);
}

static size_t StringLengthWithNullChar(const char* chars) {
  return chars ? strlen(chars) + 1 : 0;
}

size_t CacheableChars::serializedSize() const {
  return sizeof(uint32_t) + StringLengthWithNullChar(get());
}

uint8_t* CacheableChars::serialize(uint8_t* cursor) const {
  uint32_t lengthWithNullChar = StringLengthWithNullChar(get());
  cursor = WriteScalar<uint32_t>(cursor, lengthWithNullChar);
  cursor = WriteBytes(cursor, get(), lengthWithNullChar);
  return cursor;
}

const uint8_t* CacheableChars::deserialize(const uint8_t* cursor) {
  uint32_t lengthWithNullChar;
  cursor = ReadBytes(cursor, &lengthWithNullChar, sizeof(uint32_t));

  if (lengthWithNullChar) {
    reset(js_pod_malloc<char>(lengthWithNullChar));
    if (!get()) {
      return nullptr;
    }

    cursor = ReadBytes(cursor, get(), lengthWithNullChar);
  } else {
    MOZ_ASSERT(!get());
  }

  return cursor;
}

size_t CacheableChars::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(get());
}

size_t MetadataTier::serializedSize() const {
  return SerializedPodVectorSize(funcToCodeRange) +
         SerializedPodVectorSize(codeRanges) +
         SerializedPodVectorSize(callSites) +
#ifdef ENABLE_WASM_EXCEPTIONS
         SerializedPodVectorSize(tryNotes) +
#endif
         trapSites.serializedSize() + SerializedVectorSize(funcImports) +
         SerializedVectorSize(funcExports);
}

size_t MetadataTier::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return funcToCodeRange.sizeOfExcludingThis(mallocSizeOf) +
         codeRanges.sizeOfExcludingThis(mallocSizeOf) +
         callSites.sizeOfExcludingThis(mallocSizeOf) +
#ifdef ENABLE_WASM_EXCEPTIONS
         tryNotes.sizeOfExcludingThis(mallocSizeOf) +
#endif
         trapSites.sizeOfExcludingThis(mallocSizeOf) +
         SizeOfVectorExcludingThis(funcImports, mallocSizeOf) +
         SizeOfVectorExcludingThis(funcExports, mallocSizeOf);
}

uint8_t* MetadataTier::serialize(uint8_t* cursor) const {
  cursor = SerializePodVector(cursor, funcToCodeRange);
  cursor = SerializePodVector(cursor, codeRanges);
  cursor = SerializePodVector(cursor, callSites);
#ifdef ENABLE_WASM_EXCEPTIONS
  cursor = SerializePodVector(cursor, tryNotes);
#endif
  cursor = trapSites.serialize(cursor);
  cursor = SerializeVector(cursor, funcImports);
  cursor = SerializeVector(cursor, funcExports);
  MOZ_ASSERT(debugTrapFarJumpOffsets.empty());
  return cursor;
}

/* static */ const uint8_t* MetadataTier::deserialize(const uint8_t* cursor) {
  (cursor = DeserializePodVector(cursor, &funcToCodeRange)) &&
      (cursor = DeserializePodVector(cursor, &codeRanges)) &&
      (cursor = DeserializePodVector(cursor, &callSites)) &&
#ifdef ENABLE_WASM_EXCEPTIONS
      (cursor = DeserializePodVector(cursor, &tryNotes)) &&
#endif
      (cursor = trapSites.deserialize(cursor)) &&
      (cursor = DeserializeVector(cursor, &funcImports)) &&
      (cursor = DeserializeVector(cursor, &funcExports));
  MOZ_ASSERT(debugTrapFarJumpOffsets.empty());
  return cursor;
}

UniqueLazyStubSegment LazyStubSegment::create(const CodeTier& codeTier,
                                              size_t length) {
  UniqueCodeBytes codeBytes = AllocateCodeBytes(length);
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

bool LazyStubSegment::addStubs(size_t codeLength,
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
    const CodeRange& interpRange = codeRanges[i];
    MOZ_ASSERT(interpRange.isInterpEntry());
    MOZ_ASSERT(interpRange.funcIndex() ==
               funcExports[funcExportIndex].funcIndex());

    codeRanges_.infallibleAppend(interpRange);
    codeRanges_.back().offsetBy(offsetInSegment);
    i++;

    if (!funcExports[funcExportIndex].canHaveJitEntry()) {
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
  return LookupInSorted(codeRanges_,
                        CodeRange::OffsetInCode((uint8_t*)pc - base()));
}

void LazyStubSegment::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code,
                                    size_t* data) const {
  CodeSegment::addSizeOfMisc(mallocSizeOf, code);
  *data += codeRanges_.sizeOfExcludingThis(mallocSizeOf);
  *data += mallocSizeOf(this);
}

struct ProjectLazyFuncIndex {
  const LazyFuncExportVector& funcExports;
  explicit ProjectLazyFuncIndex(const LazyFuncExportVector& funcExports)
      : funcExports(funcExports) {}
  uint32_t operator[](size_t index) const {
    return funcExports[index].funcIndex;
  }
};

static constexpr unsigned LAZY_STUB_LIFO_DEFAULT_CHUNK_SIZE = 8 * 1024;

bool LazyStubTier::createMany(const Uint32Vector& funcExportIndices,
                              const CodeTier& codeTier,
                              bool flushAllThreadsIcaches,
                              size_t* stubSegmentIndex) {
  MOZ_ASSERT(funcExportIndices.length());

  LifoAlloc lifo(LAZY_STUB_LIFO_DEFAULT_CHUNK_SIZE);
  TempAllocator alloc(&lifo);
  JitContext jitContext(&alloc);
  WasmMacroAssembler masm(alloc);

  const MetadataTier& metadata = codeTier.metadata();
  const FuncExportVector& funcExports = metadata.funcExports;
  uint8_t* moduleSegmentBase = codeTier.segment().base();

  CodeRangeVector codeRanges;
  DebugOnly<uint32_t> numExpectedRanges = 0;
  for (uint32_t funcExportIndex : funcExportIndices) {
    const FuncExport& fe = funcExports[funcExportIndex];
    // Exports that don't support a jit entry get only the interp entry.
    numExpectedRanges += (fe.canHaveJitEntry() ? 2 : 1);
    void* calleePtr =
        moduleSegmentBase + metadata.codeRange(fe).funcUncheckedCallEntry();
    Maybe<ImmPtr> callee;
    callee.emplace(calleePtr, ImmPtr::NoCheckToken());
    if (!GenerateEntryStubs(masm, funcExportIndex, fe, callee,
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
#ifdef ENABLE_WASM_EXCEPTIONS
  MOZ_ASSERT(masm.tryNotes().empty());
#endif

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
  if (!segment->addStubs(codeLength, funcExportIndices, funcExports, codeRanges,
                         &codePtr, &interpRangeIndex)) {
    return false;
  }

  masm.executableCopy(codePtr);
  PatchDebugSymbolicAccesses(codePtr, masm);
  memset(codePtr + masm.bytesNeeded(), 0, codeLength - masm.bytesNeeded());

  for (const CodeLabel& label : masm.codeLabels()) {
    Assembler::Bind(codePtr, label);
  }

  // Optimized compilation finishes on a background thread, so we must make sure
  // to flush the icaches of all the executing threads.
  FlushICacheSpec flushIcacheSpec = flushAllThreadsIcaches
                                        ? FlushICacheSpec::AllThreads
                                        : FlushICacheSpec::LocalThreadOnly;
  if (!ExecutableAllocator::makeExecutableAndFlushICache(flushIcacheSpec,
                                                         codePtr, codeLength)) {
    return false;
  }

  // Create lazy function exports for funcIndex -> entry lookup.
  if (!exports_.reserve(exports_.length() + funcExportIndices.length())) {
    return false;
  }

  for (uint32_t funcExportIndex : funcExportIndices) {
    const FuncExport& fe = funcExports[funcExportIndex];

    DebugOnly<CodeRange> cr = segment->codeRanges()[interpRangeIndex];
    MOZ_ASSERT(cr.value.isInterpEntry());
    MOZ_ASSERT(cr.value.funcIndex() == fe.funcIndex());

    LazyFuncExport lazyExport(fe.funcIndex(), *stubSegmentIndex,
                              interpRangeIndex);

    size_t exportIndex;
    MOZ_ALWAYS_FALSE(BinarySearch(ProjectLazyFuncIndex(exports_), 0,
                                  exports_.length(), fe.funcIndex(),
                                  &exportIndex));
    MOZ_ALWAYS_TRUE(
        exports_.insert(exports_.begin() + exportIndex, std::move(lazyExport)));

    // Exports that don't support a jit entry get only the interp entry.
    interpRangeIndex += (fe.canHaveJitEntry() ? 2 : 1);
  }

  return true;
}

bool LazyStubTier::createOne(uint32_t funcExportIndex,
                             const CodeTier& codeTier) {
  Uint32Vector funcExportIndexes;
  if (!funcExportIndexes.append(funcExportIndex)) {
    return false;
  }

  // This happens on the executing thread (when createOne is called from
  // GetInterpEntryAndEnsureStubs), so no need to flush the icaches on all the
  // threads.
  bool flushAllThreadIcaches = false;

  size_t stubSegmentIndex;
  if (!createMany(funcExportIndexes, codeTier, flushAllThreadIcaches,
                  &stubSegmentIndex)) {
    return false;
  }

  const UniqueLazyStubSegment& segment = stubSegments_[stubSegmentIndex];
  const CodeRangeVector& codeRanges = segment->codeRanges();

  // Exports that don't support a jit entry get only the interp entry.
  if (!codeTier.metadata().funcExports[funcExportIndex].canHaveJitEntry()) {
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
                               const CodeTier& codeTier,
                               Maybe<size_t>* outStubSegmentIndex) {
  if (!funcExportIndices.length()) {
    return true;
  }

  // This compilation happens on a background compiler thread, so the icache may
  // need to be flushed on all the threads.
  bool flushAllThreadIcaches = true;

  size_t stubSegmentIndex;
  if (!createMany(funcExportIndices, codeTier, flushAllThreadIcaches,
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

bool LazyStubTier::hasStub(uint32_t funcIndex) const {
  size_t match;
  return BinarySearch(ProjectLazyFuncIndex(exports_), 0, exports_.length(),
                      funcIndex, &match);
}

void* LazyStubTier::lookupInterpEntry(uint32_t funcIndex) const {
  size_t match;
  if (!BinarySearch(ProjectLazyFuncIndex(exports_), 0, exports_.length(),
                    funcIndex, &match)) {
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

bool MetadataTier::clone(const MetadataTier& src) {
  if (!funcToCodeRange.appendAll(src.funcToCodeRange)) {
    return false;
  }
  if (!codeRanges.appendAll(src.codeRanges)) {
    return false;
  }
  if (!callSites.appendAll(src.callSites)) {
    return false;
  }
  if (!debugTrapFarJumpOffsets.appendAll(src.debugTrapFarJumpOffsets)) {
    return false;
  }
#ifdef ENABLE_WASM_EXCEPTIONS
  if (!tryNotes.appendAll(src.tryNotes)) {
    return false;
  }
#endif

  for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
    if (!trapSites[trap].appendAll(src.trapSites[trap])) {
      return false;
    }
  }

  if (!funcImports.resize(src.funcImports.length())) {
    return false;
  }
  for (size_t i = 0; i < src.funcImports.length(); i++) {
    funcImports[i].clone(src.funcImports[i]);
  }

  if (!funcExports.resize(src.funcExports.length())) {
    return false;
  }
  for (size_t i = 0; i < src.funcExports.length(); i++) {
    funcExports[i].clone(src.funcExports[i]);
  }

  return true;
}

size_t Metadata::serializedSize() const {
  return sizeof(pod()) + SerializedVectorSize(types) +
         SerializedVectorSize(globals) + SerializedPodVectorSize(tables) +
#ifdef ENABLE_WASM_EXCEPTIONS
         SerializedPodVectorSize(events) +
#endif
         sizeof(moduleName) + SerializedPodVectorSize(funcNames) +
         filename.serializedSize() + sourceMapURL.serializedSize();
}

uint8_t* Metadata::serialize(uint8_t* cursor) const {
  MOZ_ASSERT(!debugEnabled && debugFuncArgTypes.empty() &&
             debugFuncReturnTypes.empty());
  cursor = WriteBytes(cursor, &pod(), sizeof(pod()));
  cursor = SerializeVector(cursor, types);
  cursor = SerializeVector(cursor, globals);
  cursor = SerializePodVector(cursor, tables);
#ifdef ENABLE_WASM_EXCEPTIONS
  cursor = SerializePodVector(cursor, events);
#endif
  cursor = WriteBytes(cursor, &moduleName, sizeof(moduleName));
  cursor = SerializePodVector(cursor, funcNames);
  cursor = filename.serialize(cursor);
  cursor = sourceMapURL.serialize(cursor);
  return cursor;
}

/* static */ const uint8_t* Metadata::deserialize(const uint8_t* cursor) {
  (cursor = ReadBytes(cursor, &pod(), sizeof(pod()))) &&
      (cursor = DeserializeVector(cursor, &types)) &&
      (cursor = DeserializeVector(cursor, &globals)) &&
      (cursor = DeserializePodVector(cursor, &tables)) &&
#ifdef ENABLE_WASM_EXCEPTIONS
      (cursor = DeserializePodVector(cursor, &events)) &&
#endif
      (cursor = ReadBytes(cursor, &moduleName, sizeof(moduleName))) &&
      (cursor = DeserializePodVector(cursor, &funcNames)) &&
      (cursor = filename.deserialize(cursor)) &&
      (cursor = sourceMapURL.deserialize(cursor));
  debugEnabled = false;
  debugFuncArgTypes.clear();
  debugFuncReturnTypes.clear();
  return cursor;
}

size_t Metadata::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return SizeOfVectorExcludingThis(types, mallocSizeOf) +
         globals.sizeOfExcludingThis(mallocSizeOf) +
         tables.sizeOfExcludingThis(mallocSizeOf) +
#ifdef ENABLE_WASM_EXCEPTIONS
         events.sizeOfExcludingThis(mallocSizeOf) +
#endif
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

  ToCStringBuf cbuf;
  const char* funcIndexStr = NumberToCString(nullptr, &cbuf, funcIndex);
  MOZ_ASSERT(funcIndexStr);

  return bytes->append(beforeFuncIndex, strlen(beforeFuncIndex)) &&
         bytes->append(funcIndexStr, strlen(funcIndexStr)) &&
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

bool CodeTier::initialize(IsTier2 isTier2, const Code& code,
                          const LinkData& linkData, const Metadata& metadata) {
  MOZ_ASSERT(!initialized());
  code_ = &code;

  MOZ_ASSERT(lazyStubs_.lock()->empty());

  // See comments in CodeSegment::initialize() for why this must be last.
  if (!segment_->initialize(isTier2, *this, linkData, metadata, *metadata_)) {
    return false;
  }

  MOZ_ASSERT(initialized());
  return true;
}

size_t CodeTier::serializedSize() const {
  return segment_->serializedSize() + metadata_->serializedSize();
}

uint8_t* CodeTier::serialize(uint8_t* cursor, const LinkData& linkData) const {
  cursor = metadata_->serialize(cursor);
  cursor = segment_->serialize(cursor, linkData);
  return cursor;
}

/* static */ const uint8_t* CodeTier::deserialize(const uint8_t* cursor,
                                                  const LinkData& linkData,
                                                  UniqueCodeTier* codeTier) {
  auto metadata = js::MakeUnique<MetadataTier>(Tier::Serialized);
  if (!metadata) {
    return nullptr;
  }
  cursor = metadata->deserialize(cursor);
  if (!cursor) {
    return nullptr;
  }

  UniqueModuleSegment segment;
  cursor = ModuleSegment::deserialize(cursor, linkData, &segment);
  if (!cursor) {
    return nullptr;
  }

  *codeTier = js::MakeUnique<CodeTier>(std::move(metadata), std::move(segment));
  if (!*codeTier) {
    return nullptr;
  }

  return cursor;
}

void CodeTier::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code,
                             size_t* data) const {
  segment_->addSizeOfMisc(mallocSizeOf, code, data);
  lazyStubs_.lock()->addSizeOfMisc(mallocSizeOf, code, data);
  *data += metadata_->sizeOfExcludingThis(mallocSizeOf);
}

const CodeRange* CodeTier::lookupRange(const void* pc) const {
  CodeRange::OffsetInCode target((uint8_t*)pc - segment_->base());
  return LookupInSorted(metadata_->codeRanges, target);
}

#ifdef ENABLE_WASM_EXCEPTIONS
const wasm::WasmTryNote* CodeTier::lookupWasmTryNote(const void* pc) const {
  size_t target = (uint8_t*)pc - segment_->base();
  const WasmTryNoteVector& tryNotes = metadata_->tryNotes;

  // We find the first hit (there may be multiple) to obtain the innermost
  // handler, which is why we cannot binary search here.
  for (const auto& tryNote : tryNotes) {
    if (target >= tryNote.begin && target < tryNote.end) {
      return &tryNote;
    }
  }

  return nullptr;
}
#endif

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

  if (!tier1_->initialize(IsTier2::NotTier2, *this, linkData, *metadata_)) {
    return false;
  }

  MOZ_ASSERT(initialized());
  return true;
}

bool Code::setTier2(UniqueCodeTier tier2, const LinkData& linkData) const {
  MOZ_RELEASE_ASSERT(!hasTier2());
  MOZ_RELEASE_ASSERT(tier2->tier() == Tier::Optimized &&
                     tier1_->tier() == Tier::Baseline);

  if (!tier2->initialize(IsTier2::Tier2, *this, linkData, *metadata_)) {
    return false;
  }

  tier2_ = std::move(tier2);

  return true;
}

void Code::commitTier2() const {
  MOZ_RELEASE_ASSERT(!hasTier2());
  MOZ_RELEASE_ASSERT(tier2_.get());
  hasTier2_ = true;
  MOZ_ASSERT(hasTier2());
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
      if (tier2_) {
        MOZ_ASSERT(tier2_->initialized());
        return *tier2_;
      }
      MOZ_CRASH("No code segment at this tier");
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

#ifdef ENABLE_WASM_EXCEPTIONS
const wasm::WasmTryNote* Code::lookupWasmTryNote(void* pc, Tier* tier) const {
  for (Tier t : tiers()) {
    const WasmTryNote* result = codeTier(t).lookupWasmTryNote(pc);
    if (result) {
      *tier = t;
      return result;
    }
  }
  return nullptr;
}
#endif

struct TrapSitePCOffset {
  const TrapSiteVector& trapSites;
  explicit TrapSitePCOffset(const TrapSiteVector& trapSites)
      : trapSites(trapSites) {}
  uint32_t operator[](size_t index) const { return trapSites[index].pcOffset; }
};

bool Code::lookupTrap(void* pc, Trap* trapOut, BytecodeOffset* bytecode) const {
  for (Tier t : tiers()) {
    const TrapSiteVectorArray& trapSitesArray = metadata(t).trapSites;
    for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
      const TrapSiteVector& trapSites = trapSitesArray[trap];

      uint32_t target = ((uint8_t*)pc) - segment(t).base();
      size_t lowerBound = 0;
      size_t upperBound = trapSites.length();

      size_t match;
      if (BinarySearch(TrapSitePCOffset(trapSites), lowerBound, upperBound,
                       target, &match)) {
        MOZ_ASSERT(segment(t).containsCodePC(pc));
        *trapOut = trap;
        *bytecode = trapSites[match].bytecode;
        return true;
      }
    }
  }

  return false;
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

    ToCStringBuf cbuf;
    const char* bytecodeStr =
        NumberToCString(nullptr, &cbuf, codeRange.funcLineOrBytecode());
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

    if (!name.append(':') || !name.append(bytecodeStr, strlen(bytecodeStr)) ||
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

size_t Code::serializedSize() const {
  return metadata().serializedSize() +
         codeTier(Tier::Serialized).serializedSize();
}

uint8_t* Code::serialize(uint8_t* cursor, const LinkData& linkData) const {
  MOZ_RELEASE_ASSERT(!metadata().debugEnabled);

  cursor = metadata().serialize(cursor);
  cursor = codeTier(Tier::Serialized).serialize(cursor, linkData);
  return cursor;
}

/* static */ const uint8_t* Code::deserialize(const uint8_t* cursor,
                                              const LinkData& linkData,
                                              Metadata& metadata,
                                              SharedCode* out) {
  cursor = metadata.deserialize(cursor);
  if (!cursor) {
    return nullptr;
  }

  UniqueCodeTier codeTier;
  cursor = CodeTier::deserialize(cursor, linkData, &codeTier);
  if (!cursor) {
    return nullptr;
  }

  JumpTables jumpTables;
  if (!jumpTables.init(CompileMode::Once, codeTier->segment(),
                       codeTier->metadata().codeRanges)) {
    return nullptr;
  }

  MutableCode code =
      js_new<Code>(std::move(codeTier), metadata, std::move(jumpTables));
  if (!code || !code->initialize(linkData)) {
    return nullptr;
  }

  *out = code;
  return cursor;
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
