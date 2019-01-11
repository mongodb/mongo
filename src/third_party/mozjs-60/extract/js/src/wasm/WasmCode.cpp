/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

#include "jit/ExecutableAllocator.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "vtune/VTuneWrapper.h"
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

bool
CodeSegment::registerInProcessMap()
{
    if (!RegisterCodeSegment(this))
        return false;
    registered_ = true;
    return true;
}

CodeSegment::~CodeSegment()
{
    if (registered_)
        UnregisterCodeSegment(this);
}

static uint32_t
RoundupCodeLength(uint32_t codeLength)
{
    // codeLength is a multiple of the system's page size, but not necessarily
    // a multiple of ExecutableCodePageSize.
    MOZ_ASSERT(codeLength % gc::SystemPageSize() == 0);
    return JS_ROUNDUP(codeLength, ExecutableCodePageSize);
}

/* static */ UniqueCodeBytes
CodeSegment::AllocateCodeBytes(uint32_t codeLength)
{
    codeLength = RoundupCodeLength(codeLength);

    void* p = AllocateExecutableMemory(codeLength, ProtectionSetting::Writable);

    // If the allocation failed and the embedding gives us a last-ditch attempt
    // to purge all memory (which, in gecko, does a purging GC/CC/GC), do that
    // then retry the allocation.
    if (!p) {
        if (OnLargeAllocationFailure) {
            OnLargeAllocationFailure();
            p = AllocateExecutableMemory(codeLength, ProtectionSetting::Writable);
        }
    }

    if (!p)
        return nullptr;

    // We account for the bytes allocated in WasmModuleObject::create, where we
    // have the necessary JSContext.

    return UniqueCodeBytes((uint8_t*)p, FreeCode(codeLength));
}

const Code&
CodeSegment::code() const
{
    MOZ_ASSERT(codeTier_);
    return codeTier_->code();
}

void
CodeSegment::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code) const
{
    *code += RoundupCodeLength(length_);
}

void
FreeCode::operator()(uint8_t* bytes)
{
    MOZ_ASSERT(codeLength);
    MOZ_ASSERT(codeLength == RoundupCodeLength(codeLength));

#ifdef MOZ_VTUNE
    vtune::UnmarkBytes(bytes, codeLength);
#endif
    DeallocateExecutableMemory(bytes, codeLength);
}

static bool
StaticallyLink(const ModuleSegment& ms, const LinkDataTier& linkData)
{
    for (LinkDataTier::InternalLink link : linkData.internalLinks) {
        CodeLabel label;
        label.patchAt()->bind(link.patchAtOffset);
        label.target()->bind(link.targetOffset);
#ifdef JS_CODELABEL_LINKMODE
        label.setLinkMode(static_cast<CodeLabel::LinkMode>(link.mode));
#endif
        Assembler::Bind(ms.base(), label);
    }

    if (!EnsureBuiltinThunksInitialized())
        return false;

    for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
        const Uint32Vector& offsets = linkData.symbolicLinks[imm];
        if (offsets.empty())
            continue;

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

static void
StaticallyUnlink(uint8_t* base, const LinkDataTier& linkData)
{
    for (LinkDataTier::InternalLink link : linkData.internalLinks) {
        CodeLabel label;
        label.patchAt()->bind(link.patchAtOffset);
        label.target()->bind(-size_t(base)); // to reset immediate to null
#ifdef JS_CODELABEL_LINKMODE
        label.setLinkMode(static_cast<CodeLabel::LinkMode>(link.mode));
#endif
        Assembler::Bind(base, label);
    }

    for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
        const Uint32Vector& offsets = linkData.symbolicLinks[imm];
        if (offsets.empty())
            continue;

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
static bool
AppendToString(const char* str, UTF8Bytes* bytes)
{
    return bytes->append(str, strlen(str)) && bytes->append('\0');
}
#endif

static void
SendCodeRangesToProfiler(const ModuleSegment& ms, const Bytes& bytecode, const Metadata& metadata,
                         const CodeRangeVector& codeRanges)
{
    bool enabled = false;
#ifdef JS_ION_PERF
    enabled |= PerfFuncEnabled();
#endif
#ifdef MOZ_VTUNE
    enabled |= vtune::IsProfilingActive();
#endif
    if (!enabled)
        return;

    for (const CodeRange& codeRange : codeRanges) {
        if (!codeRange.hasFuncIndex())
            continue;

        uintptr_t start = uintptr_t(ms.base() + codeRange.begin());
        uintptr_t size = codeRange.end() - codeRange.begin();

        UTF8Bytes name;
        if (!metadata.getFuncName(&bytecode, codeRange.funcIndex(), &name))
            return;

        // Avoid "unused" warnings
        (void)start;
        (void)size;

#ifdef JS_ION_PERF
        if (PerfFuncEnabled()) {
            const char* file = metadata.filename.get();
            if (codeRange.isFunction()) {
                if (!name.append('\0'))
                    return;
                unsigned line = codeRange.funcLineOrBytecode();
                writePerfSpewerWasmFunctionMap(start, size, file, line, name.begin());
            } else if (codeRange.isInterpEntry()) {
                if (!AppendToString(" slow entry", &name))
                    return;
                writePerfSpewerWasmMap(start, size, file, name.begin());
            } else if (codeRange.isJitEntry()) {
                if (!AppendToString(" fast entry", &name))
                    return;
                writePerfSpewerWasmMap(start, size, file, name.begin());
            } else if (codeRange.isImportInterpExit()) {
                if (!AppendToString(" slow exit", &name))
                    return;
                writePerfSpewerWasmMap(start, size, file, name.begin());
            } else if (codeRange.isImportJitExit()) {
                if (!AppendToString(" fast exit", &name))
                    return;
                writePerfSpewerWasmMap(start, size, file, name.begin());
            } else {
                MOZ_CRASH("unhandled perf hasFuncIndex type");
            }
        }
#endif
#ifdef MOZ_VTUNE
        if (!vtune::IsProfilingActive())
            continue;
        if (!codeRange.isFunction())
            continue;
        if (!name.append('\0'))
            return;
        vtune::MarkWasm(vtune::GenerateUniqueMethodID(), name.begin(), (void*)start, size);
#endif
    }
}

/* static */ UniqueModuleSegment
ModuleSegment::create(Tier tier,
                      MacroAssembler& masm,
                      const ShareableBytes& bytecode,
                      const LinkDataTier& linkData,
                      const Metadata& metadata,
                      const CodeRangeVector& codeRanges)
{
    // Round up the code size to page size since this is eventually required by
    // the executable-code allocator and for setting memory protection.
    uint32_t bytesNeeded = masm.bytesNeeded();
    uint32_t padding = ComputeByteAlignment(bytesNeeded, gc::SystemPageSize());
    uint32_t codeLength = bytesNeeded + padding;

    UniqueCodeBytes codeBytes = AllocateCodeBytes(codeLength);
    if (!codeBytes)
        return nullptr;

    // We'll flush the icache after static linking, in initialize().
    masm.executableCopy(codeBytes.get(), /* flushICache = */ false);

    // Zero the padding.
    memset(codeBytes.get() + bytesNeeded, 0, padding);

    return create(tier, Move(codeBytes), codeLength, bytecode, linkData, metadata, codeRanges);
}

/* static */ UniqueModuleSegment
ModuleSegment::create(Tier tier,
                      const Bytes& unlinkedBytes,
                      const ShareableBytes& bytecode,
                      const LinkDataTier& linkData,
                      const Metadata& metadata,
                      const CodeRangeVector& codeRanges)
{
    // The unlinked bytes are a snapshot of the MacroAssembler's contents so
    // round up just like in the MacroAssembler overload above.
    uint32_t padding = ComputeByteAlignment(unlinkedBytes.length(), gc::SystemPageSize());
    uint32_t codeLength = unlinkedBytes.length() + padding;

    UniqueCodeBytes codeBytes = AllocateCodeBytes(codeLength);
    if (!codeBytes)
        return nullptr;

    memcpy(codeBytes.get(), unlinkedBytes.begin(), unlinkedBytes.length());
    memset(codeBytes.get() + unlinkedBytes.length(), 0, padding);

    return create(tier, Move(codeBytes), codeLength, bytecode, linkData, metadata, codeRanges);
}

/* static */ UniqueModuleSegment
ModuleSegment::create(Tier tier,
                      UniqueCodeBytes codeBytes,
                      uint32_t codeLength,
                      const ShareableBytes& bytecode,
                      const LinkDataTier& linkData,
                      const Metadata& metadata,
                      const CodeRangeVector& codeRanges)
{
    // These should always exist and should never be first in the code segment.

    auto ms = js::MakeUnique<ModuleSegment>();
    if (!ms)
        return nullptr;

    if (!ms->initialize(tier, Move(codeBytes), codeLength, bytecode, linkData, metadata, codeRanges))
        return nullptr;

    return UniqueModuleSegment(ms.release());
}

bool
ModuleSegment::initialize(Tier tier,
                          UniqueCodeBytes codeBytes,
                          uint32_t codeLength,
                          const ShareableBytes& bytecode,
                          const LinkDataTier& linkData,
                          const Metadata& metadata,
                          const CodeRangeVector& codeRanges)
{
    MOZ_ASSERT(bytes_ == nullptr);
    MOZ_ASSERT(linkData.interruptOffset);
    MOZ_ASSERT(linkData.outOfBoundsOffset);
    MOZ_ASSERT(linkData.unalignedAccessOffset);
    MOZ_ASSERT(linkData.trapOffset);

    tier_ = tier;
    bytes_ = Move(codeBytes);
    length_ = codeLength;
    interruptCode_ = bytes_.get() + linkData.interruptOffset;
    outOfBoundsCode_ = bytes_.get() + linkData.outOfBoundsOffset;
    unalignedAccessCode_ = bytes_.get() + linkData.unalignedAccessOffset;
    trapCode_ = bytes_.get() + linkData.trapOffset;

    if (!StaticallyLink(*this, linkData))
        return false;

    ExecutableAllocator::cacheFlush(bytes_.get(), RoundupCodeLength(codeLength));

    // Reprotect the whole region to avoid having separate RW and RX mappings.
    if (!ExecutableAllocator::makeExecutable(bytes_.get(), RoundupCodeLength(codeLength)))
        return false;

    if (!registerInProcessMap())
        return false;

    SendCodeRangesToProfiler(*this, bytecode.bytes, metadata, codeRanges);

    return true;
}

size_t
ModuleSegment::serializedSize() const
{
    return sizeof(uint32_t) + length_;
}

void
ModuleSegment::addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf, size_t* code, size_t* data) const
{
    CodeSegment::addSizeOfMisc(mallocSizeOf, code);
    *data += mallocSizeOf(this);
}

uint8_t*
ModuleSegment::serialize(uint8_t* cursor, const LinkDataTier& linkData) const
{
    MOZ_ASSERT(tier() == Tier::Serialized);

    cursor = WriteScalar<uint32_t>(cursor, length_);
    uint8_t* base = cursor;
    cursor = WriteBytes(cursor, bytes_.get(), length_);
    StaticallyUnlink(base, linkData);
    return cursor;
}

const uint8_t*
ModuleSegment::deserialize(const uint8_t* cursor, const ShareableBytes& bytecode,
                           const LinkDataTier& linkData, const Metadata& metadata,
                           const CodeRangeVector& codeRanges)
{
    uint32_t length;
    cursor = ReadScalar<uint32_t>(cursor, &length);
    if (!cursor)
        return nullptr;

    MOZ_ASSERT(length % gc::SystemPageSize() == 0);
    UniqueCodeBytes bytes = AllocateCodeBytes(length);
    if (!bytes)
        return nullptr;

    cursor = ReadBytes(cursor, bytes.get(), length);
    if (!cursor)
        return nullptr;

    if (!initialize(Tier::Serialized, Move(bytes), length, bytecode, linkData, metadata, codeRanges))
        return nullptr;

    return cursor;
}

const CodeRange*
ModuleSegment::lookupRange(const void* pc) const
{
    return codeTier().lookupRange(pc);
}

size_t
FuncExport::serializedSize() const
{
    return sig_.serializedSize() +
           sizeof(pod);
}

uint8_t*
FuncExport::serialize(uint8_t* cursor) const
{
    cursor = sig_.serialize(cursor);
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    return cursor;
}

const uint8_t*
FuncExport::deserialize(const uint8_t* cursor)
{
    (cursor = sig_.deserialize(cursor)) &&
    (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
    return cursor;
}

size_t
FuncExport::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return sig_.sizeOfExcludingThis(mallocSizeOf);
}

size_t
FuncImport::serializedSize() const
{
    return sig_.serializedSize() +
           sizeof(pod);
}

uint8_t*
FuncImport::serialize(uint8_t* cursor) const
{
    cursor = sig_.serialize(cursor);
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    return cursor;
}

const uint8_t*
FuncImport::deserialize(const uint8_t* cursor)
{
    (cursor = sig_.deserialize(cursor)) &&
    (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
    return cursor;
}

size_t
FuncImport::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return sig_.sizeOfExcludingThis(mallocSizeOf);
}

static size_t
StringLengthWithNullChar(const char* chars)
{
    return chars ? strlen(chars) + 1 : 0;
}

size_t
CacheableChars::serializedSize() const
{
    return sizeof(uint32_t) + StringLengthWithNullChar(get());
}

uint8_t*
CacheableChars::serialize(uint8_t* cursor) const
{
    uint32_t lengthWithNullChar = StringLengthWithNullChar(get());
    cursor = WriteScalar<uint32_t>(cursor, lengthWithNullChar);
    cursor = WriteBytes(cursor, get(), lengthWithNullChar);
    return cursor;
}

const uint8_t*
CacheableChars::deserialize(const uint8_t* cursor)
{
    uint32_t lengthWithNullChar;
    cursor = ReadBytes(cursor, &lengthWithNullChar, sizeof(uint32_t));

    if (lengthWithNullChar) {
        reset(js_pod_malloc<char>(lengthWithNullChar));
        if (!get())
            return nullptr;

        cursor = ReadBytes(cursor, get(), lengthWithNullChar);
    } else {
        MOZ_ASSERT(!get());
    }

    return cursor;
}

size_t
CacheableChars::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return mallocSizeOf(get());
}

size_t
MetadataTier::serializedSize() const
{
    return SerializedPodVectorSize(memoryAccesses) +
           SerializedPodVectorSize(codeRanges) +
           SerializedPodVectorSize(callSites) +
           trapSites.serializedSize() +
           SerializedVectorSize(funcImports) +
           SerializedVectorSize(funcExports);
}

size_t
MetadataTier::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return memoryAccesses.sizeOfExcludingThis(mallocSizeOf) +
           codeRanges.sizeOfExcludingThis(mallocSizeOf) +
           callSites.sizeOfExcludingThis(mallocSizeOf) +
           trapSites.sizeOfExcludingThis(mallocSizeOf) +
           SizeOfVectorExcludingThis(funcImports, mallocSizeOf) +
           SizeOfVectorExcludingThis(funcExports, mallocSizeOf);
}

uint8_t*
MetadataTier::serialize(uint8_t* cursor) const
{
    MOZ_ASSERT(debugTrapFarJumpOffsets.empty() && debugFuncToCodeRange.empty());
    cursor = SerializePodVector(cursor, memoryAccesses);
    cursor = SerializePodVector(cursor, codeRanges);
    cursor = SerializePodVector(cursor, callSites);
    cursor = trapSites.serialize(cursor);
    cursor = SerializeVector(cursor, funcImports);
    cursor = SerializeVector(cursor, funcExports);
    return cursor;
}

/* static */ const uint8_t*
MetadataTier::deserialize(const uint8_t* cursor)
{
    (cursor = DeserializePodVector(cursor, &memoryAccesses)) &&
    (cursor = DeserializePodVector(cursor, &codeRanges)) &&
    (cursor = DeserializePodVector(cursor, &callSites)) &&
    (cursor = trapSites.deserialize(cursor)) &&
    (cursor = DeserializeVector(cursor, &funcImports)) &&
    (cursor = DeserializeVector(cursor, &funcExports));
    debugTrapFarJumpOffsets.clear();
    debugFuncToCodeRange.clear();
    return cursor;
}

bool
LazyStubSegment::initialize(UniqueCodeBytes codeBytes, size_t length)
{
    MOZ_ASSERT(bytes_ == nullptr);

    bytes_ = Move(codeBytes);
    length_ = length;

    return registerInProcessMap();
}

UniqueLazyStubSegment
LazyStubSegment::create(const CodeTier& codeTier, size_t length)
{
    UniqueCodeBytes codeBytes = AllocateCodeBytes(length);
    if (!codeBytes)
        return nullptr;

    auto segment = js::MakeUnique<LazyStubSegment>(codeTier);
    if (!segment || !segment->initialize(Move(codeBytes), length))
        return nullptr;
    return segment;
}

bool
LazyStubSegment::hasSpace(size_t bytes) const
{
    MOZ_ASSERT(bytes % MPROTECT_PAGE_SIZE == 0);
    return bytes <= length_ &&
           usedBytes_ <= length_ - bytes;
}

bool
LazyStubSegment::addStubs(size_t codeLength, const Uint32Vector& funcExportIndices,
                          const FuncExportVector& funcExports, const CodeRangeVector& codeRanges,
                          uint8_t** codePtr, size_t* indexFirstInsertedCodeRange)
{
    MOZ_ASSERT(hasSpace(codeLength));

    size_t offsetInSegment = usedBytes_;
    *codePtr = base() + usedBytes_;
    usedBytes_ += codeLength;

    *indexFirstInsertedCodeRange = codeRanges_.length();

    if (!codeRanges_.reserve(codeRanges_.length() + 2 * codeRanges.length()))
        return false;

    size_t i = 0;
    for (DebugOnly<uint32_t> funcExportIndex : funcExportIndices) {
        const CodeRange& interpRange = codeRanges[i];
        MOZ_ASSERT(interpRange.isInterpEntry());
        MOZ_ASSERT(interpRange.funcIndex() == funcExports[funcExportIndex].funcIndex());

        codeRanges_.infallibleAppend(interpRange);
        codeRanges_.back().offsetBy(offsetInSegment);

        const CodeRange& jitRange = codeRanges[i + 1];
        MOZ_ASSERT(jitRange.isJitEntry());
        MOZ_ASSERT(jitRange.funcIndex() == interpRange.funcIndex());

        codeRanges_.infallibleAppend(jitRange);
        codeRanges_.back().offsetBy(offsetInSegment);

        i += 2;
    }

    return true;
}

const CodeRange*
LazyStubSegment::lookupRange(const void* pc) const
{
    return LookupInSorted(codeRanges_, CodeRange::OffsetInCode((uint8_t*)pc - base()));
}

void
LazyStubSegment::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code, size_t* data) const
{
    CodeSegment::addSizeOfMisc(mallocSizeOf, code);
    *data += codeRanges_.sizeOfExcludingThis(mallocSizeOf);
    *data += mallocSizeOf(this);
}

struct ProjectLazyFuncIndex
{
    const LazyFuncExportVector& funcExports;
    explicit ProjectLazyFuncIndex(const LazyFuncExportVector& funcExports)
      : funcExports(funcExports)
    {}
    uint32_t operator[](size_t index) const {
        return funcExports[index].funcIndex;
    }
};

static constexpr unsigned LAZY_STUB_LIFO_DEFAULT_CHUNK_SIZE = 8 * 1024;

bool
LazyStubTier::createMany(const Uint32Vector& funcExportIndices, const CodeTier& codeTier,
                         size_t* stubSegmentIndex)
{
    MOZ_ASSERT(funcExportIndices.length());

    LifoAlloc lifo(LAZY_STUB_LIFO_DEFAULT_CHUNK_SIZE);
    TempAllocator alloc(&lifo);
    JitContext jitContext(&alloc);
    MacroAssembler masm(MacroAssembler::WasmToken(), alloc);

    const CodeRangeVector& moduleRanges = codeTier.metadata().codeRanges;
    const FuncExportVector& funcExports = codeTier.metadata().funcExports;
    uint8_t* moduleSegmentBase = codeTier.segment().base();

    CodeRangeVector codeRanges;
    for (uint32_t funcExportIndex : funcExportIndices) {
        const FuncExport& fe = funcExports[funcExportIndex];
        void* calleePtr = moduleSegmentBase +
                          moduleRanges[fe.interpCodeRangeIndex()].funcNormalEntry();
        Maybe<ImmPtr> callee;
        callee.emplace(calleePtr, ImmPtr::NoCheckToken());
        if (!GenerateEntryStubs(masm, funcExportIndex, fe, callee, /* asmjs*/ false, &codeRanges))
            return false;
    }
    MOZ_ASSERT(codeRanges.length() == 2 * funcExportIndices.length(), "two entries per function");

    masm.finish();

    MOZ_ASSERT(!masm.numCodeLabels());
    MOZ_ASSERT(masm.callSites().empty());
    MOZ_ASSERT(masm.callSiteTargets().empty());
    MOZ_ASSERT(masm.callFarJumps().empty());
    MOZ_ASSERT(masm.trapSites().empty());
    MOZ_ASSERT(masm.oldTrapSites().empty());
    MOZ_ASSERT(masm.oldTrapFarJumps().empty());
    MOZ_ASSERT(masm.callFarJumps().empty());
    MOZ_ASSERT(masm.memoryAccesses().empty());
    MOZ_ASSERT(masm.symbolicAccesses().empty());

    if (masm.oom())
        return false;

    size_t codeLength = LazyStubSegment::AlignBytesNeeded(masm.bytesNeeded());

    if (!stubSegments_.length() || !stubSegments_[lastStubSegmentIndex_]->hasSpace(codeLength)) {
        size_t newSegmentSize = Max(codeLength, ExecutableCodePageSize);
        UniqueLazyStubSegment newSegment = LazyStubSegment::create(codeTier, newSegmentSize);
        if (!newSegment)
            return false;
        lastStubSegmentIndex_ = stubSegments_.length();
        if (!stubSegments_.emplaceBack(Move(newSegment)))
            return false;
    }

    LazyStubSegment* segment = stubSegments_[lastStubSegmentIndex_].get();
    *stubSegmentIndex = lastStubSegmentIndex_;

    size_t interpRangeIndex;
    uint8_t* codePtr = nullptr;
    if (!segment->addStubs(codeLength, funcExportIndices, funcExports, codeRanges, &codePtr,
                           &interpRangeIndex))
        return false;

    masm.executableCopy(codePtr, /* flushICache = */ false);
    memset(codePtr + masm.bytesNeeded(), 0, codeLength - masm.bytesNeeded());

    ExecutableAllocator::cacheFlush(codePtr, codeLength);
    if (!ExecutableAllocator::makeExecutable(codePtr, codeLength))
        return false;

    // Create lazy function exports for funcIndex -> entry lookup.
    if (!exports_.reserve(exports_.length() + funcExportIndices.length()))
        return false;

    for (uint32_t funcExportIndex : funcExportIndices) {
        const FuncExport& fe = funcExports[funcExportIndex];

        DebugOnly<CodeRange> cr = segment->codeRanges()[interpRangeIndex];
        MOZ_ASSERT(cr.value.isInterpEntry());
        MOZ_ASSERT(cr.value.funcIndex() == fe.funcIndex());

        LazyFuncExport lazyExport(fe.funcIndex(), *stubSegmentIndex, interpRangeIndex);

        size_t exportIndex;
        MOZ_ALWAYS_FALSE(BinarySearch(ProjectLazyFuncIndex(exports_), 0, exports_.length(),
                                      fe.funcIndex(), &exportIndex));
        MOZ_ALWAYS_TRUE(exports_.insert(exports_.begin() + exportIndex, Move(lazyExport)));

        interpRangeIndex += 2;
    }

    return true;
}

bool
LazyStubTier::createOne(uint32_t funcExportIndex, const CodeTier& codeTier)
{
    Uint32Vector funcExportIndexes;
    if (!funcExportIndexes.append(funcExportIndex))
        return false;

    size_t stubSegmentIndex;
    if (!createMany(funcExportIndexes, codeTier, &stubSegmentIndex))
        return false;

    const UniqueLazyStubSegment& segment = stubSegments_[stubSegmentIndex];
    const CodeRangeVector& codeRanges = segment->codeRanges();

    MOZ_ASSERT(codeRanges.length() >= 2);
    MOZ_ASSERT(codeRanges[codeRanges.length() - 2].isInterpEntry());

    const CodeRange& cr = codeRanges[codeRanges.length() - 1];
    MOZ_ASSERT(cr.isJitEntry());

    codeTier.code().setJitEntry(cr.funcIndex(), segment->base() + cr.begin());
    return true;
}

bool
LazyStubTier::createTier2(const Uint32Vector& funcExportIndices, const CodeTier& codeTier,
                          Maybe<size_t>* outStubSegmentIndex)
{
    if (!funcExportIndices.length())
        return true;

    size_t stubSegmentIndex;
    if (!createMany(funcExportIndices, codeTier, &stubSegmentIndex))
        return false;

    outStubSegmentIndex->emplace(stubSegmentIndex);
    return true;
}

void
LazyStubTier::setJitEntries(const Maybe<size_t>& stubSegmentIndex, const Code& code)
{
    if (!stubSegmentIndex)
        return;
    const UniqueLazyStubSegment& segment = stubSegments_[*stubSegmentIndex];
    for (const CodeRange& cr : segment->codeRanges()) {
        if (!cr.isJitEntry())
            continue;
        code.setJitEntry(cr.funcIndex(), segment->base() + cr.begin());
    }
}

bool
LazyStubTier::hasStub(uint32_t funcIndex) const
{
    size_t match;
    return BinarySearch(ProjectLazyFuncIndex(exports_), 0, exports_.length(), funcIndex, &match);
}

void*
LazyStubTier::lookupInterpEntry(uint32_t funcIndex) const
{
    size_t match;
    MOZ_ALWAYS_TRUE(BinarySearch(ProjectLazyFuncIndex(exports_), 0, exports_.length(), funcIndex,
                    &match));
    const LazyFuncExport& fe = exports_[match];
    const LazyStubSegment& stub = *stubSegments_[fe.lazyStubSegmentIndex];
    return stub.base() + stub.codeRanges()[fe.interpCodeRangeIndex].begin();
}

void
LazyStubTier::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code, size_t* data) const
{
    *data += sizeof(this);
    *data += exports_.sizeOfExcludingThis(mallocSizeOf);
    for (const UniqueLazyStubSegment& stub : stubSegments_)
        stub->addSizeOfMisc(mallocSizeOf, code, data);
}

bool
MetadataTier::clone(const MetadataTier& src)
{
    if (!memoryAccesses.appendAll(src.memoryAccesses))
        return false;
    if (!codeRanges.appendAll(src.codeRanges))
        return false;
    if (!callSites.appendAll(src.callSites))
        return false;
    if (!debugTrapFarJumpOffsets.appendAll(src.debugTrapFarJumpOffsets))
        return false;
    if (!debugFuncToCodeRange.appendAll(src.debugFuncToCodeRange))
        return false;

    for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
        if (!trapSites[trap].appendAll(src.trapSites[trap]))
            return false;
    }

    if (!funcImports.resize(src.funcImports.length()))
        return false;
    for (size_t i = 0; i < src.funcImports.length(); i++)
        funcImports[i].clone(src.funcImports[i]);

    if (!funcExports.resize(src.funcExports.length()))
        return false;
    for (size_t i = 0; i < src.funcExports.length(); i++)
        funcExports[i].clone(src.funcExports[i]);

    return true;
}

size_t
Metadata::serializedSize() const
{
    return sizeof(pod()) +
           SerializedVectorSize(sigIds) +
           SerializedPodVectorSize(globals) +
           SerializedPodVectorSize(tables) +
           SerializedPodVectorSize(funcNames) +
           SerializedPodVectorSize(customSections) +
           filename.serializedSize() +
           baseURL.serializedSize() +
           sourceMapURL.serializedSize();
}

size_t
Metadata::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return SizeOfVectorExcludingThis(sigIds, mallocSizeOf) +
           globals.sizeOfExcludingThis(mallocSizeOf) +
           tables.sizeOfExcludingThis(mallocSizeOf) +
           funcNames.sizeOfExcludingThis(mallocSizeOf) +
           customSections.sizeOfExcludingThis(mallocSizeOf) +
           filename.sizeOfExcludingThis(mallocSizeOf) +
           baseURL.sizeOfExcludingThis(mallocSizeOf) +
           sourceMapURL.sizeOfExcludingThis(mallocSizeOf);
}

uint8_t*
Metadata::serialize(uint8_t* cursor) const
{
    MOZ_ASSERT(!debugEnabled && debugFuncArgTypes.empty() && debugFuncReturnTypes.empty());
    cursor = WriteBytes(cursor, &pod(), sizeof(pod()));
    cursor = SerializeVector(cursor, sigIds);
    cursor = SerializePodVector(cursor, globals);
    cursor = SerializePodVector(cursor, tables);
    cursor = SerializePodVector(cursor, funcNames);
    cursor = SerializePodVector(cursor, customSections);
    cursor = filename.serialize(cursor);
    cursor = baseURL.serialize(cursor);
    cursor = sourceMapURL.serialize(cursor);
    return cursor;
}

/* static */ const uint8_t*
Metadata::deserialize(const uint8_t* cursor)
{
    (cursor = ReadBytes(cursor, &pod(), sizeof(pod()))) &&
    (cursor = DeserializeVector(cursor, &sigIds)) &&
    (cursor = DeserializePodVector(cursor, &globals)) &&
    (cursor = DeserializePodVector(cursor, &tables)) &&
    (cursor = DeserializePodVector(cursor, &funcNames)) &&
    (cursor = DeserializePodVector(cursor, &customSections)) &&
    (cursor = filename.deserialize(cursor));
    (cursor = baseURL.deserialize(cursor));
    (cursor = sourceMapURL.deserialize(cursor));
    debugEnabled = false;
    debugFuncArgTypes.clear();
    debugFuncReturnTypes.clear();
    return cursor;
}

struct ProjectFuncIndex
{
    const FuncExportVector& funcExports;
    explicit ProjectFuncIndex(const FuncExportVector& funcExports)
      : funcExports(funcExports)
    {}
    uint32_t operator[](size_t index) const {
        return funcExports[index].funcIndex();
    }
};

FuncExport&
MetadataTier::lookupFuncExport(uint32_t funcIndex, size_t* funcExportIndex /* = nullptr */)
{
    size_t match;
    if (!BinarySearch(ProjectFuncIndex(funcExports), 0, funcExports.length(), funcIndex, &match))
        MOZ_CRASH("missing function export");
    if (funcExportIndex)
        *funcExportIndex = match;
    return funcExports[match];
}

const FuncExport&
MetadataTier::lookupFuncExport(uint32_t funcIndex, size_t* funcExportIndex) const
{
    return const_cast<MetadataTier*>(this)->lookupFuncExport(funcIndex, funcExportIndex);
}

bool
Metadata::getFuncName(const Bytes* maybeBytecode, uint32_t funcIndex, UTF8Bytes* name) const
{
    if (funcIndex < funcNames.length()) {
        MOZ_ASSERT(maybeBytecode, "NameInBytecode requires preserved bytecode");

        const NameInBytecode& n = funcNames[funcIndex];
        if (n.length != 0) {
            MOZ_ASSERT(n.offset + n.length <= maybeBytecode->length());
            return name->append((const char*)maybeBytecode->begin() + n.offset, n.length);
        }
    }

    // For names that are out of range or invalid, synthesize a name.

    const char beforeFuncIndex[] = "wasm-function[";
    const char afterFuncIndex[] = "]";

    ToCStringBuf cbuf;
    const char* funcIndexStr = NumberToCString(nullptr, &cbuf, funcIndex);
    MOZ_ASSERT(funcIndexStr);

    return name->append(beforeFuncIndex, strlen(beforeFuncIndex)) &&
           name->append(funcIndexStr, strlen(funcIndexStr)) &&
           name->append(afterFuncIndex, strlen(afterFuncIndex));
}

size_t
CodeTier::serializedSize() const
{
    return segment_->serializedSize() +
           metadata_->serializedSize();
}

uint8_t*
CodeTier::serialize(uint8_t* cursor, const LinkDataTier& linkData) const
{
    cursor = metadata_->serialize(cursor);
    cursor = segment_->serialize(cursor, linkData);
    return cursor;
}

const uint8_t*
CodeTier::deserialize(const uint8_t* cursor, const SharedBytes& bytecode, Metadata& metadata,
                      const LinkDataTier& linkData)
{
    metadata_ = js::MakeUnique<MetadataTier>(Tier::Serialized);
    if (!metadata_)
        return nullptr;
    cursor = metadata_->deserialize(cursor);
    if (!cursor)
        return nullptr;

    auto segment = Move(js::MakeUnique<ModuleSegment>());
    if (!segment)
        return nullptr;
    cursor = segment->deserialize(cursor, *bytecode, linkData, metadata, metadata_->codeRanges);
    if (!cursor)
        return nullptr;
    segment_ = takeOwnership(Move(segment));

    return cursor;
}

void
CodeTier::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code, size_t* data) const
{
    segment_->addSizeOfMisc(mallocSizeOf, code, data);
    lazyStubs_.lock()->addSizeOfMisc(mallocSizeOf, code, data);
    *data += metadata_->sizeOfExcludingThis(mallocSizeOf);
}

const CodeRange*
CodeTier::lookupRange(const void* pc) const
{
    CodeRange::OffsetInCode target((uint8_t*)pc - segment_->base());
    return LookupInSorted(metadata_->codeRanges, target);
}

bool
JumpTables::init(CompileMode mode, const ModuleSegment& ms, const CodeRangeVector& codeRanges)
{
    // Note a fast jit entry has two addresses, to be compatible with
    // ion/baseline functions which have the raw vs checked args entries,
    // both used all over the place in jit calls. This allows the fast entries
    // to be compatible with jit code pointer loading routines.
    // We can use the same entry for both kinds of jit entries since a wasm
    // entry knows how to convert any kind of arguments and doesn't assume
    // any input types.

    static_assert(JSScript::offsetOfJitCodeRaw() == 0,
                  "wasm fast jit entry is at (void*) jit[2*funcIndex]");
    static_assert(JSScript::offsetOfJitCodeSkipArgCheck() == sizeof(void*),
                  "wasm fast jit entry is also at (void*) jit[2*funcIndex+1]");

    mode_ = mode;

    size_t numFuncs = 0;
    for (const CodeRange& cr : codeRanges) {
        if (cr.isFunction())
            numFuncs++;
    }

    numFuncs_ = numFuncs;

    if (mode_ == CompileMode::Tier1) {
        tiering_ = TablePointer(js_pod_calloc<void*>(numFuncs));
        if (!tiering_)
            return false;
    }

    // The number of jit entries is overestimated, but it is simpler when
    // filling/looking up the jit entries and safe (worst case we'll crash
    // because of a null deref when trying to call the jit entry of an
    // unexported function).
    jit_ = TablePointer(js_pod_calloc<void*>(2 * numFuncs));
    if (!jit_)
        return false;

    uint8_t* codeBase = ms.base();
    for (const CodeRange& cr : codeRanges) {
        if (cr.isFunction())
            setTieringEntry(cr.funcIndex(), codeBase + cr.funcTierEntry());
        else if (cr.isJitEntry())
            setJitEntry(cr.funcIndex(), codeBase + cr.begin());
    }
    return true;
}

Code::Code(UniqueCodeTier codeTier, const Metadata& metadata, JumpTables&& maybeJumpTables)
  : tier1_(takeOwnership(Move(codeTier))),
    metadata_(&metadata),
    profilingLabels_(mutexid::WasmCodeProfilingLabels, CacheableCharsVector()),
    jumpTables_(Move(maybeJumpTables))
{
}

Code::Code()
  : profilingLabels_(mutexid::WasmCodeProfilingLabels, CacheableCharsVector())
{
}

void
Code::setTier2(UniqueCodeTier tier2) const
{
    MOZ_RELEASE_ASSERT(!hasTier2());
    MOZ_RELEASE_ASSERT(tier2->tier() == Tier::Ion && tier1_->tier() == Tier::Baseline);
    tier2_ = takeOwnership(Move(tier2));
}

void
Code::commitTier2() const
{
    MOZ_RELEASE_ASSERT(!hasTier2());
    MOZ_RELEASE_ASSERT(tier2_.get());
    hasTier2_ = true;
}

uint32_t
Code::getFuncIndex(JSFunction* fun) const
{
    if (fun->isAsmJSNative())
        return fun->asmJSFuncIndex();
    return jumpTables_.funcIndexFromJitEntry(fun->wasmJitEntry());
}

Tiers
Code::tiers() const
{
    if (hasTier2())
        return Tiers(tier1_->tier(), tier2_->tier());
    return Tiers(tier1_->tier());
}

bool
Code::hasTier(Tier t) const
{
    if (hasTier2() && tier2_->tier() == t)
        return true;
    return tier1_->tier() == t;
}

Tier
Code::stableTier() const
{
    return tier1_->tier();
}

Tier
Code::bestTier() const
{
    if (hasTier2())
        return tier2_->tier();
    return tier1_->tier();
}

const CodeTier&
Code::codeTier(Tier tier) const
{
    switch (tier) {
      case Tier::Baseline:
        if (tier1_->tier() == Tier::Baseline)
            return *tier1_;
        MOZ_CRASH("No code segment at this tier");
      case Tier::Ion:
        if (tier1_->tier() == Tier::Ion)
            return *tier1_;
        if (hasTier2())
            return *tier2_;
        MOZ_CRASH("No code segment at this tier");
      default:
        MOZ_CRASH();
    }
}

bool
Code::containsCodePC(const void* pc) const
{
    for (Tier t : tiers()) {
        const ModuleSegment& ms = segment(t);
        if (ms.containsCodePC(pc))
            return true;
    }
    return false;
}

struct CallSiteRetAddrOffset
{
    const CallSiteVector& callSites;
    explicit CallSiteRetAddrOffset(const CallSiteVector& callSites) : callSites(callSites) {}
    uint32_t operator[](size_t index) const {
        return callSites[index].returnAddressOffset();
    }
};

const CallSite*
Code::lookupCallSite(void* returnAddress) const
{
    for (Tier t : tiers()) {
        uint32_t target = ((uint8_t*)returnAddress) - segment(t).base();
        size_t lowerBound = 0;
        size_t upperBound = metadata(t).callSites.length();

        size_t match;
        if (BinarySearch(CallSiteRetAddrOffset(metadata(t).callSites), lowerBound, upperBound,
                         target, &match))
            return &metadata(t).callSites[match];
    }

    return nullptr;
}

const CodeRange*
Code::lookupFuncRange(void* pc) const
{
    for (Tier t : tiers()) {
        const CodeRange* result = codeTier(t).lookupRange(pc);
        if (result && result->isFunction())
            return result;
    }
    return nullptr;
}

struct MemoryAccessOffset
{
    const MemoryAccessVector& accesses;
    explicit MemoryAccessOffset(const MemoryAccessVector& accesses) : accesses(accesses) {}
    uintptr_t operator[](size_t index) const {
        return accesses[index].insnOffset();
    }
};

const MemoryAccess*
Code::lookupMemoryAccess(void* pc) const
{
    for (Tier t : tiers()) {
        const MemoryAccessVector& memoryAccesses = metadata(t).memoryAccesses;

        uint32_t target = ((uint8_t*)pc) - segment(t).base();
        size_t lowerBound = 0;
        size_t upperBound = memoryAccesses.length();

        size_t match;
        if (BinarySearch(MemoryAccessOffset(memoryAccesses), lowerBound, upperBound, target,
                         &match))
        {
            MOZ_ASSERT(segment(t).containsCodePC(pc));
            return &memoryAccesses[match];
        }
    }
    return nullptr;
}

struct TrapSitePCOffset
{
    const TrapSiteVector& trapSites;
    explicit TrapSitePCOffset(const TrapSiteVector& trapSites) : trapSites(trapSites) {}
    uint32_t operator[](size_t index) const {
        return trapSites[index].pcOffset;
    }
};

bool
Code::lookupTrap(void* pc, Trap* trapOut, BytecodeOffset* bytecode) const
{
    for (Tier t : tiers()) {
        const TrapSiteVectorArray& trapSitesArray = metadata(t).trapSites;
        for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
            const TrapSiteVector& trapSites = trapSitesArray[trap];

            uint32_t target = ((uint8_t*)pc) - segment(t).base();
            size_t lowerBound = 0;
            size_t upperBound = trapSites.length();

            size_t match;
            if (BinarySearch(TrapSitePCOffset(trapSites), lowerBound, upperBound, target, &match)) {
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
void
Code::ensureProfilingLabels(const Bytes* maybeBytecode, bool profilingEnabled) const
{
    auto labels = profilingLabels_.lock();

    if (!profilingEnabled) {
        labels->clear();
        return;
    }

    if (!labels->empty())
        return;

    // Any tier will do, we only need tier-invariant data that are incidentally
    // stored with the code ranges.

    for (const CodeRange& codeRange : metadata(stableTier()).codeRanges) {
        if (!codeRange.isFunction())
            continue;

        ToCStringBuf cbuf;
        const char* bytecodeStr = NumberToCString(nullptr, &cbuf, codeRange.funcLineOrBytecode());
        MOZ_ASSERT(bytecodeStr);

        UTF8Bytes name;
        if (!metadata().getFuncName(maybeBytecode, codeRange.funcIndex(), &name))
            return;
        if (!name.append(" (", 2))
            return;

        if (const char* filename = metadata().filename.get()) {
            if (!name.append(filename, strlen(filename)))
                return;
        } else {
            if (!name.append('?'))
                return;
        }

        if (!name.append(':') ||
            !name.append(bytecodeStr, strlen(bytecodeStr)) ||
            !name.append(")\0", 2))
        {
            return;
        }

        UniqueChars label(name.extractOrCopyRawBuffer());
        if (!label)
            return;

        if (codeRange.funcIndex() >= labels->length()) {
            if (!labels->resize(codeRange.funcIndex() + 1))
                return;
        }

        ((CacheableCharsVector&)labels)[codeRange.funcIndex()] = Move(label);
    }
}

const char*
Code::profilingLabel(uint32_t funcIndex) const
{
    auto labels = profilingLabels_.lock();

    if (funcIndex >= labels->length() || !((CacheableCharsVector&)labels)[funcIndex])
        return "?";
    return ((CacheableCharsVector&)labels)[funcIndex].get();
}

void
Code::addSizeOfMiscIfNotSeen(MallocSizeOf mallocSizeOf,
                             Metadata::SeenSet* seenMetadata,
                             Code::SeenSet* seenCode,
                             size_t* code,
                             size_t* data) const
{
    auto p = seenCode->lookupForAdd(this);
    if (p)
        return;
    bool ok = seenCode->add(p, this);
    (void)ok;  // oh well

    *data += mallocSizeOf(this) +
             metadata().sizeOfIncludingThisIfNotSeen(mallocSizeOf, seenMetadata) +
             profilingLabels_.lock()->sizeOfExcludingThis(mallocSizeOf) +
             jumpTables_.sizeOfMiscIncludingThis(mallocSizeOf);

    for (auto t : tiers())
        codeTier(t).addSizeOfMisc(mallocSizeOf, code, data);
}

size_t
Code::serializedSize() const
{
    return metadata().serializedSize() +
           codeTier(Tier::Serialized).serializedSize();
}

uint8_t*
Code::serialize(uint8_t* cursor, const LinkDataTier& linkDataTier) const
{
    MOZ_RELEASE_ASSERT(!metadata().debugEnabled);

    cursor = metadata().serialize(cursor);
    cursor = codeTier(Tier::Serialized).serialize(cursor, linkDataTier);
    return cursor;
}

const uint8_t*
Code::deserialize(const uint8_t* cursor, const SharedBytes& bytecode,
                  const LinkDataTier& linkDataTier, Metadata& metadata)
{
    cursor = metadata.deserialize(cursor);
    if (!cursor)
        return nullptr;

    auto codeTier = js::MakeUnique<CodeTier>(Tier::Serialized);
    if (!codeTier)
        return nullptr;
    cursor = codeTier->deserialize(cursor, bytecode, metadata, linkDataTier);
    if (!cursor)
        return nullptr;

    tier1_ = takeOwnership(Move(codeTier));
    metadata_ = &metadata;

    if (!jumpTables_.init(CompileMode::Once, tier1_->segment(), tier1_->metadata().codeRanges))
        return nullptr;

    return cursor;
}
