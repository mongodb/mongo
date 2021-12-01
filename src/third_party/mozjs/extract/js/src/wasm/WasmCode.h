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

#ifndef wasm_code_h
#define wasm_code_h

#include "js/HashTable.h"
#include "threading/ExclusiveData.h"
#include "vm/MutexIDs.h"
#include "wasm/WasmTypes.h"

namespace js {

struct AsmJSMetadata;
class WasmInstanceObject;

namespace wasm {

struct LinkDataTier;
struct MetadataTier;
struct Metadata;

// ShareableBytes is a reference-counted Vector of bytes.

struct ShareableBytes : ShareableBase<ShareableBytes>
{
    // Vector is 'final', so instead make Vector a member and add boilerplate.
    Bytes bytes;
    ShareableBytes() = default;
    explicit ShareableBytes(Bytes&& bytes) : bytes(Move(bytes)) {}
    size_t sizeOfExcludingThis(MallocSizeOf m) const { return bytes.sizeOfExcludingThis(m); }
    const uint8_t* begin() const { return bytes.begin(); }
    const uint8_t* end() const { return bytes.end(); }
    size_t length() const { return bytes.length(); }
    bool append(const uint8_t *p, uint32_t ct) { return bytes.append(p, ct); }
};

typedef RefPtr<ShareableBytes> MutableBytes;
typedef RefPtr<const ShareableBytes> SharedBytes;

// Executable code must be deallocated specially.

struct FreeCode {
    uint32_t codeLength;
    FreeCode() : codeLength(0) {}
    explicit FreeCode(uint32_t codeLength) : codeLength(codeLength) {}
    void operator()(uint8_t* codeBytes);
};

using UniqueCodeBytes = UniquePtr<uint8_t, FreeCode>;

class Code;
class CodeTier;
class ModuleSegment;
class LazyStubSegment;

// CodeSegment contains common helpers for determining the base and length of a
// code segment and if a pc belongs to this segment. It is inherited by:
// - ModuleSegment, i.e. the code segment of a Module, generated
// eagerly when a Module is instanciated.
// - LazyStubSegment, i.e. the code segment of entry stubs that are lazily
// generated.

class CodeSegment
{
  protected:
    static UniqueCodeBytes AllocateCodeBytes(uint32_t codeLength);

    UniqueCodeBytes bytes_;
    uint32_t length_;

    // A back reference to the owning code.
    const CodeTier* codeTier_;

    enum class Kind {
        LazyStubs,
        Module
    } kind_;

    bool registerInProcessMap();

  private:
    bool registered_;

  public:
    explicit CodeSegment(Kind kind = Kind::Module)
      : length_(UINT32_MAX),
        codeTier_(nullptr),
        kind_(kind),
        registered_(false)
    {}

    ~CodeSegment();

    bool isLazyStubs() const { return kind_ == Kind::LazyStubs; }
    bool isModule() const { return kind_ == Kind::Module; }
    const ModuleSegment* asModule() const {
        MOZ_ASSERT(isModule());
        return (ModuleSegment*) this;
    }
    const LazyStubSegment* asLazyStub() const {
        MOZ_ASSERT(isLazyStubs());
        return (LazyStubSegment*) this;
    }

    uint8_t* base() const { return bytes_.get(); }
    uint32_t length() const { MOZ_ASSERT(length_ != UINT32_MAX); return length_; }

    bool containsCodePC(const void* pc) const {
        return pc >= base() && pc < (base() + length_);
    }

    void initCodeTier(const CodeTier* codeTier) {
        MOZ_ASSERT(!codeTier_);
        codeTier_ = codeTier;
    }
    const CodeTier& codeTier() const { return *codeTier_; }
    const Code& code() const;

    void addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code) const;
};

// A wasm ModuleSegment owns the allocated executable code for a wasm module.

typedef UniquePtr<ModuleSegment> UniqueModuleSegment;
typedef UniquePtr<const ModuleSegment> UniqueConstModuleSegment;

class ModuleSegment : public CodeSegment
{
    Tier            tier_;

    // These are pointers into code for stubs used for asynchronous
    // signal-handler control-flow transfer.
    uint8_t*        interruptCode_;
    uint8_t*        outOfBoundsCode_;
    uint8_t*        unalignedAccessCode_;
    uint8_t*        trapCode_;

    bool initialize(Tier tier,
                    UniqueCodeBytes bytes,
                    uint32_t codeLength,
                    const ShareableBytes& bytecode,
                    const LinkDataTier& linkData,
                    const Metadata& metadata,
                    const CodeRangeVector& codeRanges);

    static UniqueModuleSegment create(Tier tier,
                                      UniqueCodeBytes bytes,
                                      uint32_t codeLength,
                                      const ShareableBytes& bytecode,
                                      const LinkDataTier& linkData,
                                      const Metadata& metadata,
                                      const CodeRangeVector& codeRanges);
  public:
    ModuleSegment(const ModuleSegment&) = delete;
    void operator=(const ModuleSegment&) = delete;

    ModuleSegment()
      : CodeSegment(),
        tier_(Tier(-1)),
        interruptCode_(nullptr),
        outOfBoundsCode_(nullptr),
        unalignedAccessCode_(nullptr),
        trapCode_(nullptr)
    {}

    static UniqueModuleSegment create(Tier tier,
                                      jit::MacroAssembler& masm,
                                      const ShareableBytes& bytecode,
                                      const LinkDataTier& linkData,
                                      const Metadata& metadata,
                                      const CodeRangeVector& codeRanges);

    static UniqueModuleSegment create(Tier tier,
                                      const Bytes& unlinkedBytes,
                                      const ShareableBytes& bytecode,
                                      const LinkDataTier& linkData,
                                      const Metadata& metadata,
                                      const CodeRangeVector& codeRanges);

    Tier tier() const { return tier_; }

    uint8_t* interruptCode() const { return interruptCode_; }
    uint8_t* outOfBoundsCode() const { return outOfBoundsCode_; }
    uint8_t* unalignedAccessCode() const { return unalignedAccessCode_; }
    uint8_t* trapCode() const { return trapCode_; }

    // Structured clone support:

    size_t serializedSize() const;
    uint8_t* serialize(uint8_t* cursor, const LinkDataTier& linkDataTier) const;
    const uint8_t* deserialize(const uint8_t* cursor, const ShareableBytes& bytecode,
                               const LinkDataTier& linkDataTier, const Metadata& metadata,
                               const CodeRangeVector& codeRanges);

    const CodeRange* lookupRange(const void* pc) const;

    void addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf, size_t* code, size_t* data) const;
};

// A FuncExport represents a single function definition inside a wasm Module
// that has been exported one or more times. A FuncExport represents an
// internal entry point that can be called via function definition index by
// Instance::callExport(). To allow O(log(n)) lookup of a FuncExport by
// function definition index, the FuncExportVector is stored sorted by
// function definition index.

class FuncExport
{
    Sig sig_;
    MOZ_INIT_OUTSIDE_CTOR struct CacheablePod {
        uint32_t funcIndex_;
        uint32_t interpCodeRangeIndex_;
        uint32_t eagerInterpEntryOffset_; // Machine code offset
        bool     hasEagerStubs_;
    } pod;

  public:
    FuncExport() = default;
    explicit FuncExport(Sig&& sig, uint32_t funcIndex, bool hasEagerStubs)
      : sig_(Move(sig))
    {
        pod.funcIndex_ = funcIndex;
        pod.interpCodeRangeIndex_ = UINT32_MAX;
        pod.eagerInterpEntryOffset_ = UINT32_MAX;
        pod.hasEagerStubs_ = hasEagerStubs;
    }
    void initEagerInterpEntryOffset(uint32_t entryOffset) {
        MOZ_ASSERT(pod.eagerInterpEntryOffset_ == UINT32_MAX);
        MOZ_ASSERT(hasEagerStubs());
        pod.eagerInterpEntryOffset_ = entryOffset;
    }
    void initInterpCodeRangeIndex(uint32_t codeRangeIndex) {
        MOZ_ASSERT(pod.interpCodeRangeIndex_ == UINT32_MAX);
        pod.interpCodeRangeIndex_ = codeRangeIndex;
    }

    bool hasEagerStubs() const {
        return pod.hasEagerStubs_;
    }
    const Sig& sig() const {
        return sig_;
    }
    uint32_t funcIndex() const {
        return pod.funcIndex_;
    }
    uint32_t interpCodeRangeIndex() const {
        MOZ_ASSERT(pod.interpCodeRangeIndex_ != UINT32_MAX);
        return pod.interpCodeRangeIndex_;
    }
    uint32_t eagerInterpEntryOffset() const {
        MOZ_ASSERT(pod.eagerInterpEntryOffset_ != UINT32_MAX);
        MOZ_ASSERT(hasEagerStubs());
        return pod.eagerInterpEntryOffset_;
    }

    bool clone(const FuncExport& src) {
        mozilla::PodAssign(&pod, &src.pod);
        return sig_.clone(src.sig_);
    }

    WASM_DECLARE_SERIALIZABLE(FuncExport)
};

typedef Vector<FuncExport, 0, SystemAllocPolicy> FuncExportVector;

// An FuncImport contains the runtime metadata needed to implement a call to an
// imported function. Each function import has two call stubs: an optimized path
// into JIT code and a slow path into the generic C++ js::Invoke and these
// offsets of these stubs are stored so that function-import callsites can be
// dynamically patched at runtime.

class FuncImport
{
    Sig sig_;
    struct CacheablePod {
        uint32_t tlsDataOffset_;
        uint32_t interpExitCodeOffset_; // Machine code offset
        uint32_t jitExitCodeOffset_;    // Machine code offset
    } pod;

  public:
    FuncImport() {
        memset(&pod, 0, sizeof(CacheablePod));
    }

    FuncImport(Sig&& sig, uint32_t tlsDataOffset)
      : sig_(Move(sig))
    {
        pod.tlsDataOffset_ = tlsDataOffset;
        pod.interpExitCodeOffset_ = 0;
        pod.jitExitCodeOffset_ = 0;
    }

    void initInterpExitOffset(uint32_t off) {
        MOZ_ASSERT(!pod.interpExitCodeOffset_);
        pod.interpExitCodeOffset_ = off;
    }
    void initJitExitOffset(uint32_t off) {
        MOZ_ASSERT(!pod.jitExitCodeOffset_);
        pod.jitExitCodeOffset_ = off;
    }

    const Sig& sig() const {
        return sig_;
    }
    uint32_t tlsDataOffset() const {
        return pod.tlsDataOffset_;
    }
    uint32_t interpExitCodeOffset() const {
        return pod.interpExitCodeOffset_;
    }
    uint32_t jitExitCodeOffset() const {
        return pod.jitExitCodeOffset_;
    }

    bool clone(const FuncImport& src) {
        mozilla::PodAssign(&pod, &src.pod);
        return sig_.clone(src.sig_);
    }

    WASM_DECLARE_SERIALIZABLE(FuncImport)
};

typedef Vector<FuncImport, 0, SystemAllocPolicy> FuncImportVector;

// A wasm module can either use no memory, a unshared memory (ArrayBuffer) or
// shared memory (SharedArrayBuffer).

enum class MemoryUsage
{
    None = false,
    Unshared = 1,
    Shared = 2
};

// NameInBytecode represents a name that is embedded in the wasm bytecode.
// The presence of NameInBytecode implies that bytecode has been kept.

struct NameInBytecode
{
    uint32_t offset;
    uint32_t length;

    NameInBytecode()
      : offset(UINT32_MAX), length(0)
    {}
    NameInBytecode(uint32_t offset, uint32_t length)
      : offset(offset), length(length)
    {}
};

typedef Vector<NameInBytecode, 0, SystemAllocPolicy> NameInBytecodeVector;

// CustomSection represents a custom section in the bytecode which can be
// extracted via Module.customSections. The (offset, length) pair does not
// include the custom section name.

struct CustomSection
{
    NameInBytecode name;
    uint32_t offset;
    uint32_t length;

    CustomSection() = default;
    CustomSection(NameInBytecode name, uint32_t offset, uint32_t length)
      : name(name), offset(offset), length(length)
    {}
};

typedef Vector<CustomSection, 0, SystemAllocPolicy> CustomSectionVector;
typedef Vector<ValTypeVector, 0, SystemAllocPolicy> FuncArgTypesVector;
typedef Vector<ExprType, 0, SystemAllocPolicy> FuncReturnTypesVector;

// Metadata holds all the data that is needed to describe compiled wasm code
// at runtime (as opposed to data that is only used to statically link or
// instantiate a module).
//
// Metadata is built incrementally by ModuleGenerator and then shared immutably
// between modules.
//
// The Metadata structure is split into tier-invariant and tier-variant parts;
// the former points to instances of the latter.  Additionally, the asm.js
// subsystem subclasses the Metadata, adding more tier-invariant data, some of
// which is serialized.  See AsmJS.cpp.

struct MetadataCacheablePod
{
    ModuleKind            kind;
    MemoryUsage           memoryUsage;
    uint32_t              minMemoryLength;
    uint32_t              globalDataLength;
    Maybe<uint32_t>       maxMemoryLength;
    Maybe<uint32_t>       startFuncIndex;

    explicit MetadataCacheablePod(ModuleKind kind)
      : kind(kind),
        memoryUsage(MemoryUsage::None),
        minMemoryLength(0),
        globalDataLength(0)
    {}
};

typedef uint8_t ModuleHash[8];

struct Metadata : public ShareableBase<Metadata>, public MetadataCacheablePod
{
    SigWithIdVector       sigIds;
    GlobalDescVector      globals;
    TableDescVector       tables;
    NameInBytecodeVector  funcNames;
    CustomSectionVector   customSections;
    CacheableChars        filename;
    CacheableChars        baseURL;
    CacheableChars        sourceMapURL;

    // Debug-enabled code is not serialized.
    bool                  debugEnabled;
    FuncArgTypesVector    debugFuncArgTypes;
    FuncReturnTypesVector debugFuncReturnTypes;
    ModuleHash            debugHash;

    explicit Metadata(ModuleKind kind = ModuleKind::Wasm)
      : MetadataCacheablePod(kind),
        debugEnabled(false),
        debugHash()
    {}
    virtual ~Metadata() {}

    MetadataCacheablePod& pod() { return *this; }
    const MetadataCacheablePod& pod() const { return *this; }

    bool usesMemory() const { return memoryUsage != MemoryUsage::None; }
    bool usesSharedMemory() const { return memoryUsage == MemoryUsage::Shared; }

    // AsmJSMetadata derives Metadata iff isAsmJS(). Mostly this distinction is
    // encapsulated within AsmJS.cpp, but the additional virtual functions allow
    // asm.js to override wasm behavior in the handful of cases that can't be
    // easily encapsulated by AsmJS.cpp.

    bool isAsmJS() const {
        return kind == ModuleKind::AsmJS;
    }
    const AsmJSMetadata& asAsmJS() const {
        MOZ_ASSERT(isAsmJS());
        return *(const AsmJSMetadata*)this;
    }
    virtual bool mutedErrors() const {
        return false;
    }
    virtual const char16_t* displayURL() const {
        return nullptr;
    }
    virtual ScriptSource* maybeScriptSource() const {
        return nullptr;
    }
    virtual bool getFuncName(const Bytes* maybeBytecode, uint32_t funcIndex, UTF8Bytes* name) const;

    WASM_DECLARE_SERIALIZABLE_VIRTUAL(Metadata);
};

typedef RefPtr<Metadata> MutableMetadata;
typedef RefPtr<const Metadata> SharedMetadata;

struct MetadataTier
{
    explicit MetadataTier(Tier tier) : tier(tier) {}

    const Tier            tier;

    MemoryAccessVector    memoryAccesses;
    CodeRangeVector       codeRanges;
    CallSiteVector        callSites;
    TrapSiteVectorArray   trapSites;
    FuncImportVector      funcImports;
    FuncExportVector      funcExports;

    // Debug information, not serialized.
    Uint32Vector          debugTrapFarJumpOffsets;
    Uint32Vector          debugFuncToCodeRange;

    FuncExport& lookupFuncExport(uint32_t funcIndex, size_t* funcExportIndex = nullptr);
    const FuncExport& lookupFuncExport(uint32_t funcIndex, size_t* funcExportIndex = nullptr) const;

    bool clone(const MetadataTier& src);

    WASM_DECLARE_SERIALIZABLE(MetadataTier);
};

using UniqueMetadataTier = UniquePtr<MetadataTier>;

// LazyStubSegment is a code segment lazily generated for function entry stubs
// (both interpreter and jit ones).
//
// Because a stub is usually small (a few KiB) and an executable code segment
// isn't (64KiB), a given stub segment can contain entry stubs of many
// functions.

class LazyStubSegment : public CodeSegment
{
    CodeRangeVector codeRanges_;
    size_t usedBytes_;

    static constexpr size_t MPROTECT_PAGE_SIZE = 4 * 1024;

    bool initialize(UniqueCodeBytes codeBytes, size_t length);

  public:
    explicit LazyStubSegment(const CodeTier& codeTier)
      : CodeSegment(CodeSegment::Kind::LazyStubs),
        usedBytes_(0)
    {
        initCodeTier(&codeTier);
    }

    static UniquePtr<LazyStubSegment> create(const CodeTier& codeTier, size_t length);
    static size_t AlignBytesNeeded(size_t bytes) { return AlignBytes(bytes, MPROTECT_PAGE_SIZE); }

    bool hasSpace(size_t bytes) const;
    bool addStubs(size_t codeLength, const Uint32Vector& funcExportIndices,
                  const FuncExportVector& funcExports, const CodeRangeVector& codeRanges,
                  uint8_t** codePtr, size_t* indexFirstInsertedCodeRange);

    const CodeRangeVector& codeRanges() const { return codeRanges_; }
    const CodeRange* lookupRange(const void* pc) const;

    void addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code, size_t* data) const;
};

using UniqueLazyStubSegment = UniquePtr<LazyStubSegment>;
using LazyStubSegmentVector = Vector<UniqueLazyStubSegment, 0, SystemAllocPolicy>;

// LazyFuncExport helps to efficiently lookup a CodeRange from a given function
// index. It is inserted in a vector sorted by function index, to perform
// binary search on it later.

struct LazyFuncExport
{
    size_t funcIndex;
    size_t lazyStubSegmentIndex;
    size_t interpCodeRangeIndex;
    LazyFuncExport(size_t funcIndex, size_t lazyStubSegmentIndex, size_t interpCodeRangeIndex)
      : funcIndex(funcIndex),
        lazyStubSegmentIndex(lazyStubSegmentIndex),
        interpCodeRangeIndex(interpCodeRangeIndex)
    {}
};

using LazyFuncExportVector = Vector<LazyFuncExport, 0, SystemAllocPolicy>;

// LazyStubTier contains all the necessary information for lazy function entry
// stubs that are generated at runtime. None of its data is ever serialized.
//
// It must be protected by a lock, because the main thread can both read and
// write lazy stubs at any time while a background thread can regenerate lazy
// stubs for tier2 at any time.

class LazyStubTier
{
    LazyStubSegmentVector stubSegments_;
    LazyFuncExportVector exports_;
    size_t lastStubSegmentIndex_;

    bool createMany(const Uint32Vector& funcExportIndices, const CodeTier& codeTier,
                    size_t* stubSegmentIndex);

  public:
    LazyStubTier() : lastStubSegmentIndex_(0) {}

    bool empty() const { return stubSegments_.empty(); }
    bool hasStub(uint32_t funcIndex) const;

    // Returns a pointer to the raw interpreter entry of a given function which
    // stubs have been lazily generated.
    void* lookupInterpEntry(uint32_t funcIndex) const;

    // Creates one lazy stub for the exported function, for which the jit entry
    // will be set to the lazily-generated one.
    bool createOne(uint32_t funcExportIndex, const CodeTier& codeTier);

    // Create one lazy stub for all the functions in funcExportIndices, putting
    // them in a single stub. Jit entries won't be used until
    // setJitEntries() is actually called, after the Code owner has committed
    // tier2.
    bool createTier2(const Uint32Vector& funcExportIndices, const CodeTier& codeTier,
                     Maybe<size_t>* stubSegmentIndex);
    void setJitEntries(const Maybe<size_t>& stubSegmentIndex, const Code& code);

    void addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code, size_t* data) const;
};

// CodeTier contains all the data related to a given compilation tier. It is
// built during module generation and then immutably stored in a Code.

class CodeTier
{
    const Tier                  tier_;
    const Code*                 code_;

    // Serialized information.
    UniqueMetadataTier          metadata_;
    UniqueConstModuleSegment    segment_;

    // Lazy stubs, not serialized.
    ExclusiveData<LazyStubTier> lazyStubs_;

    UniqueConstModuleSegment takeOwnership(UniqueModuleSegment segment) const {
        segment->initCodeTier(this);
        return UniqueConstModuleSegment(segment.release());
    }

    static const MutexId& mutexForTier(Tier tier) {
        if (tier == Tier::Baseline)
            return mutexid::WasmLazyStubsTier1;
        MOZ_ASSERT(tier == Tier::Ion);
        return mutexid::WasmLazyStubsTier2;
    }

  public:
    explicit CodeTier(Tier tier)
      : tier_(tier),
        code_(nullptr),
        metadata_(nullptr),
        segment_(nullptr),
        lazyStubs_(mutexForTier(tier))
    {}

    CodeTier(Tier tier, UniqueMetadataTier metadata, UniqueModuleSegment segment)
      : tier_(tier),
        code_(nullptr),
        metadata_(Move(metadata)),
        segment_(takeOwnership(Move(segment))),
        lazyStubs_(mutexForTier(tier))
    {}

    void initCode(const Code* code) {
        MOZ_ASSERT(!code_);
        code_ = code;
    }

    Tier tier() const { return tier_; }
    const ExclusiveData<LazyStubTier>& lazyStubs() const { return lazyStubs_; }
    const MetadataTier& metadata() const { return *metadata_.get(); }
    const ModuleSegment& segment() const { return *segment_.get(); }
    const Code& code() const { return *code_; }

    const CodeRange* lookupRange(const void* pc) const;

    size_t serializedSize() const;
    uint8_t* serialize(uint8_t* cursor, const LinkDataTier& linkData) const;
    const uint8_t* deserialize(const uint8_t* cursor, const SharedBytes& bytecode,
                               Metadata& metadata, const LinkDataTier& linkData);
    void addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* code, size_t* data) const;
};

typedef UniquePtr<CodeTier> UniqueCodeTier;
typedef UniquePtr<const CodeTier> UniqueConstCodeTier;

// Jump tables to take tiering into account, when calling either from wasm to
// wasm (through rabaldr) or from jit to wasm (jit entry).

class JumpTables
{
    using TablePointer = mozilla::UniquePtr<void*[], JS::FreePolicy>;

    CompileMode mode_;
    TablePointer tiering_;
    TablePointer jit_;
    size_t numFuncs_;

  public:
    bool init(CompileMode mode, const ModuleSegment& ms, const CodeRangeVector& codeRanges);

    void setJitEntry(size_t i, void* target) const {
        // See comment in wasm::Module::finishTier2 and JumpTables::init.
        MOZ_ASSERT(i < numFuncs_);
        jit_.get()[2 * i] = target;
        jit_.get()[2 * i + 1] = target;
    }
    void** getAddressOfJitEntry(size_t i) const {
        MOZ_ASSERT(i < numFuncs_);
        MOZ_ASSERT(jit_.get()[2 * i]);
        return &jit_.get()[2 * i];
    }
    size_t funcIndexFromJitEntry(void** target) const {
        MOZ_ASSERT(target >= &jit_.get()[0]);
        MOZ_ASSERT(target <= &(jit_.get()[2 * numFuncs_ - 1]));
        size_t index = (intptr_t*)target - (intptr_t*)&jit_.get()[0];
        MOZ_ASSERT(index % 2 == 0);
        return index / 2;
    }

    void setTieringEntry(size_t i, void* target) const {
        MOZ_ASSERT(i < numFuncs_);
        // See comment in wasm::Module::finishTier2.
        if (mode_ == CompileMode::Tier1)
            tiering_.get()[i] = target;
    }
    void** tiering() const {
        return tiering_.get();
    }

    size_t sizeOfMiscIncludingThis(MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) +
               2 * sizeof(void*) * numFuncs_ +
               (tiering_ ? sizeof(void*) : numFuncs_);
    }
};

// Code objects own executable code and the metadata that describe it. A single
// Code object is normally shared between a module and all its instances.
//
// profilingLabels_ is lazily initialized, but behind a lock.

class Code : public ShareableBase<Code>
{
    UniqueConstCodeTier                 tier1_;
    mutable UniqueConstCodeTier         tier2_; // Access only when hasTier2() is true
    mutable Atomic<bool>                hasTier2_;
    SharedMetadata                      metadata_;
    ExclusiveData<CacheableCharsVector> profilingLabels_;
    JumpTables                          jumpTables_;

    UniqueConstCodeTier takeOwnership(UniqueCodeTier codeTier) const {
        codeTier->initCode(this);
        return UniqueConstCodeTier(codeTier.release());
    }

  public:
    Code();
    Code(UniqueCodeTier tier, const Metadata& metadata, JumpTables&& maybeJumpTables);

    void setTieringEntry(size_t i, void* target) const { jumpTables_.setTieringEntry(i, target); }
    void** tieringJumpTable() const { return jumpTables_.tiering(); }

    void setJitEntry(size_t i, void* target) const { jumpTables_.setJitEntry(i, target); }
    void** getAddressOfJitEntry(size_t i) const { return jumpTables_.getAddressOfJitEntry(i); }
    uint32_t getFuncIndex(JSFunction* fun) const;

    void setTier2(UniqueCodeTier tier2) const;
    void commitTier2() const;

    bool hasTier2() const { return hasTier2_; }
    Tiers tiers() const;
    bool hasTier(Tier t) const;

    Tier stableTier() const;    // This is stable during a run
    Tier bestTier() const;      // This may transition from Baseline -> Ion at any time

    const CodeTier& codeTier(Tier tier) const;
    const Metadata& metadata() const { return *metadata_; }

    const ModuleSegment& segment(Tier iter) const {
        return codeTier(iter).segment();
    }
    const MetadataTier& metadata(Tier iter) const {
        return codeTier(iter).metadata();
    }

    // Metadata lookup functions:

    const CallSite* lookupCallSite(void* returnAddress) const;
    const CodeRange* lookupFuncRange(void* pc) const;
    const MemoryAccess* lookupMemoryAccess(void* pc) const;
    bool containsCodePC(const void* pc) const;
    bool lookupTrap(void* pc, Trap* trap, BytecodeOffset* bytecode) const;

    // To save memory, profilingLabels_ are generated lazily when profiling mode
    // is enabled.

    void ensureProfilingLabels(const Bytes* maybeBytecode, bool profilingEnabled) const;
    const char* profilingLabel(uint32_t funcIndex) const;

    // about:memory reporting:

    void addSizeOfMiscIfNotSeen(MallocSizeOf mallocSizeOf,
                                Metadata::SeenSet* seenMetadata,
                                Code::SeenSet* seenCode,
                                size_t* code,
                                size_t* data) const;

    // A Code object is serialized as the length and bytes of the machine code
    // after statically unlinking it; the Code is then later recreated from the
    // machine code and other parts.

    size_t serializedSize() const;
    uint8_t* serialize(uint8_t* cursor, const LinkDataTier& linkDataTier) const;
    const uint8_t* deserialize(const uint8_t* cursor, const SharedBytes& bytecode,
                               const LinkDataTier& linkDataTier, Metadata& metadata);
};

typedef RefPtr<const Code> SharedCode;
typedef RefPtr<Code> MutableCode;

} // namespace wasm
} // namespace js

#endif // wasm_code_h
