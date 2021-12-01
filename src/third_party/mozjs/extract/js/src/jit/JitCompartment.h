/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitCompartment_h
#define jit_JitCompartment_h

#include "mozilla/Array.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"

#include "builtin/TypedObject.h"
#include "jit/CompileInfo.h"
#include "jit/ICStubSpace.h"
#include "jit/IonCode.h"
#include "jit/IonControlFlow.h"
#include "jit/JitFrames.h"
#include "jit/shared/Assembler-shared.h"
#include "js/GCHashTable.h"
#include "js/Value.h"
#include "vm/Stack.h"

namespace js {
namespace jit {

class FrameSizeClass;

struct EnterJitData
{
    explicit EnterJitData(JSContext* cx)
      : envChain(cx),
        result(cx)
    {}

    uint8_t* jitcode;
    InterpreterFrame* osrFrame;

    void* calleeToken;

    Value* maxArgv;
    unsigned maxArgc;
    unsigned numActualArgs;
    unsigned osrNumStackValues;

    RootedObject envChain;
    RootedValue result;

    bool constructing;
};

typedef void (*EnterJitCode)(void* code, unsigned argc, Value* argv, InterpreterFrame* fp,
                             CalleeToken calleeToken, JSObject* envChain,
                             size_t numStackValues, Value* vp);

class JitcodeGlobalTable;

// Information about a loop backedge in the runtime, which can be set to
// point to either the loop header or to an OOL interrupt checking stub,
// if signal handlers are being used to implement interrupts.
class PatchableBackedge : public InlineListNode<PatchableBackedge>
{
    friend class JitZoneGroup;

    CodeLocationJump backedge;
    CodeLocationLabel loopHeader;
    CodeLocationLabel interruptCheck;

  public:
    PatchableBackedge(CodeLocationJump backedge,
                      CodeLocationLabel loopHeader,
                      CodeLocationLabel interruptCheck)
      : backedge(backedge), loopHeader(loopHeader), interruptCheck(interruptCheck)
    {}
};

class JitRuntime
{
  private:
    friend class JitCompartment;

    // Executable allocator for all code except wasm code and Ion code with
    // patchable backedges (see below).
    ActiveThreadData<ExecutableAllocator> execAlloc_;

    // Executable allocator for Ion scripts with patchable backedges.
    ActiveThreadData<ExecutableAllocator> backedgeExecAlloc_;

    // Shared exception-handler tail.
    ExclusiveAccessLockWriteOnceData<uint32_t> exceptionTailOffset_;

    // Shared post-bailout-handler tail.
    ExclusiveAccessLockWriteOnceData<uint32_t> bailoutTailOffset_;

    // Shared profiler exit frame tail.
    ExclusiveAccessLockWriteOnceData<uint32_t> profilerExitFrameTailOffset_;

    // Trampoline for entering JIT code.
    ExclusiveAccessLockWriteOnceData<uint32_t> enterJITOffset_;

    // Vector mapping frame class sizes to bailout tables.
    struct BailoutTable {
        uint32_t startOffset;
        uint32_t size;
        BailoutTable(uint32_t startOffset, uint32_t size)
          : startOffset(startOffset), size(size)
        {}
    };
    typedef Vector<BailoutTable, 4, SystemAllocPolicy> BailoutTableVector;
    ExclusiveAccessLockWriteOnceData<BailoutTableVector> bailoutTables_;

    // Generic bailout table; used if the bailout table overflows.
    ExclusiveAccessLockWriteOnceData<uint32_t> bailoutHandlerOffset_;

    // Argument-rectifying thunk, in the case of insufficient arguments passed
    // to a function call site.
    ExclusiveAccessLockWriteOnceData<uint32_t> argumentsRectifierOffset_;
    ExclusiveAccessLockWriteOnceData<uint32_t> argumentsRectifierReturnOffset_;

    // Thunk that invalides an (Ion compiled) caller on the Ion stack.
    ExclusiveAccessLockWriteOnceData<uint32_t> invalidatorOffset_;

    // Thunk that calls the GC pre barrier.
    ExclusiveAccessLockWriteOnceData<uint32_t> valuePreBarrierOffset_;
    ExclusiveAccessLockWriteOnceData<uint32_t> stringPreBarrierOffset_;
    ExclusiveAccessLockWriteOnceData<uint32_t> objectPreBarrierOffset_;
    ExclusiveAccessLockWriteOnceData<uint32_t> shapePreBarrierOffset_;
    ExclusiveAccessLockWriteOnceData<uint32_t> objectGroupPreBarrierOffset_;

    // Thunk to call malloc/free.
    ExclusiveAccessLockWriteOnceData<uint32_t> mallocStubOffset_;
    ExclusiveAccessLockWriteOnceData<uint32_t> freeStubOffset_;

    // Thunk called to finish compilation of an IonScript.
    ExclusiveAccessLockWriteOnceData<uint32_t> lazyLinkStubOffset_;

    // Thunk to enter the interpreter from JIT code.
    ExclusiveAccessLockWriteOnceData<uint32_t> interpreterStubOffset_;

    // Thunk used by the debugger for breakpoint and step mode.
    ExclusiveAccessLockWriteOnceData<JitCode*> debugTrapHandler_;

    // Thunk used to fix up on-stack recompile of baseline scripts.
    ExclusiveAccessLockWriteOnceData<JitCode*> baselineDebugModeOSRHandler_;
    ExclusiveAccessLockWriteOnceData<void*> baselineDebugModeOSRHandlerNoFrameRegPopAddr_;

    // Code for trampolines and VMFunction wrappers.
    ExclusiveAccessLockWriteOnceData<JitCode*> trampolineCode_;

    // Map VMFunction addresses to the offset of the wrapper in
    // trampolineCode_.
    using VMWrapperMap = HashMap<const VMFunction*, uint32_t, VMFunction>;
    ExclusiveAccessLockWriteOnceData<VMWrapperMap*> functionWrappers_;

    // If true, the signal handler to interrupt Ion code should not attempt to
    // patch backedges, as some thread is busy modifying data structures.
    mozilla::Atomic<bool> preventBackedgePatching_;

    // Global table of jitcode native address => bytecode address mappings.
    UnprotectedData<JitcodeGlobalTable*> jitcodeGlobalTable_;

  private:
    void generateLazyLinkStub(MacroAssembler& masm);
    void generateInterpreterStub(MacroAssembler& masm);
    void generateProfilerExitFrameTailStub(MacroAssembler& masm, Label* profilerExitTail);
    void generateExceptionTailStub(MacroAssembler& masm, void* handler, Label* profilerExitTail);
    void generateBailoutTailStub(MacroAssembler& masm, Label* bailoutTail);
    void generateEnterJIT(JSContext* cx, MacroAssembler& masm);
    void generateArgumentsRectifier(MacroAssembler& masm);
    BailoutTable generateBailoutTable(MacroAssembler& masm, Label* bailoutTail, uint32_t frameClass);
    void generateBailoutHandler(MacroAssembler& masm, Label* bailoutTail);
    void generateInvalidator(MacroAssembler& masm, Label* bailoutTail);
    uint32_t generatePreBarrier(JSContext* cx, MacroAssembler& masm, MIRType type);
    void generateMallocStub(MacroAssembler& masm);
    void generateFreeStub(MacroAssembler& masm);
    JitCode* generateDebugTrapHandler(JSContext* cx);
    JitCode* generateBaselineDebugModeOSRHandler(JSContext* cx, uint32_t* noFrameRegPopOffsetOut);
    bool generateVMWrapper(JSContext* cx, MacroAssembler& masm, const VMFunction& f);

    bool generateTLEventVM(MacroAssembler& masm, const VMFunction& f, bool enter);

    inline bool generateTLEnterVM(MacroAssembler& masm, const VMFunction& f) {
        return generateTLEventVM(masm, f, /* enter = */ true);
    }
    inline bool generateTLExitVM(MacroAssembler& masm, const VMFunction& f) {
        return generateTLEventVM(masm, f, /* enter = */ false);
    }

    uint32_t startTrampolineCode(MacroAssembler& masm);

    TrampolinePtr trampolineCode(uint32_t offset) const {
        MOZ_ASSERT(offset > 0);
        MOZ_ASSERT(offset < trampolineCode_->instructionsSize());
        return TrampolinePtr(trampolineCode_->raw() + offset);
    }

  public:
    explicit JitRuntime(JSRuntime* rt);
    ~JitRuntime();
    MOZ_MUST_USE bool initialize(JSContext* cx, js::AutoLockForExclusiveAccess& lock);

    static void Trace(JSTracer* trc, js::AutoLockForExclusiveAccess& lock);
    static void TraceJitcodeGlobalTableForMinorGC(JSTracer* trc);
    static MOZ_MUST_USE bool MarkJitcodeGlobalTableIteratively(GCMarker* marker);
    static void SweepJitcodeGlobalTable(JSRuntime* rt);

    ExecutableAllocator& execAlloc() {
        return execAlloc_.ref();
    }
    ExecutableAllocator& backedgeExecAlloc() {
        return backedgeExecAlloc_.ref();
    }

    class AutoPreventBackedgePatching
    {
        mozilla::DebugOnly<JSRuntime*> rt_;
        JitRuntime* jrt_;
        bool prev_;

      public:
        // This two-arg constructor is provided for JSRuntime::createJitRuntime,
        // where we have a JitRuntime but didn't set rt->jitRuntime_ yet.
        AutoPreventBackedgePatching(JSRuntime* rt, JitRuntime* jrt)
          : rt_(rt),
            jrt_(jrt),
            prev_(false)  // silence GCC warning
        {
            if (jrt_) {
                prev_ = jrt_->preventBackedgePatching_;
                jrt_->preventBackedgePatching_ = true;
            }
        }
        explicit AutoPreventBackedgePatching(JSRuntime* rt)
          : AutoPreventBackedgePatching(rt, rt->jitRuntime())
        {}
        ~AutoPreventBackedgePatching() {
            MOZ_ASSERT(jrt_ == rt_->jitRuntime());
            if (jrt_) {
                MOZ_ASSERT(jrt_->preventBackedgePatching_);
                jrt_->preventBackedgePatching_ = prev_;
            }
        }
    };

    bool preventBackedgePatching() const {
        return preventBackedgePatching_;
    }

    TrampolinePtr getVMWrapper(const VMFunction& f) const;
    JitCode* debugTrapHandler(JSContext* cx);
    JitCode* getBaselineDebugModeOSRHandler(JSContext* cx);
    void* getBaselineDebugModeOSRHandlerAddress(JSContext* cx, bool popFrameReg);

    TrampolinePtr getGenericBailoutHandler() const {
        return trampolineCode(bailoutHandlerOffset_);
    }

    TrampolinePtr getExceptionTail() const {
        return trampolineCode(exceptionTailOffset_);
    }

    TrampolinePtr getBailoutTail() const {
        return trampolineCode(bailoutTailOffset_);
    }

    TrampolinePtr getProfilerExitFrameTail() const {
        return trampolineCode(profilerExitFrameTailOffset_);
    }

    TrampolinePtr getBailoutTable(const FrameSizeClass& frameClass) const;
    uint32_t getBailoutTableSize(const FrameSizeClass& frameClass) const;

    TrampolinePtr getArgumentsRectifier() const {
        return trampolineCode(argumentsRectifierOffset_);
    }

    TrampolinePtr getArgumentsRectifierReturnAddr() const {
        return trampolineCode(argumentsRectifierReturnOffset_);
    }

    TrampolinePtr getInvalidationThunk() const {
        return trampolineCode(invalidatorOffset_);
    }

    EnterJitCode enterJit() const {
        return JS_DATA_TO_FUNC_PTR(EnterJitCode, trampolineCode(enterJITOffset_).value);
    }

    TrampolinePtr preBarrier(MIRType type) const {
        switch (type) {
          case MIRType::Value:
            return trampolineCode(valuePreBarrierOffset_);
          case MIRType::String:
            return trampolineCode(stringPreBarrierOffset_);
          case MIRType::Object:
            return trampolineCode(objectPreBarrierOffset_);
          case MIRType::Shape:
            return trampolineCode(shapePreBarrierOffset_);
          case MIRType::ObjectGroup:
            return trampolineCode(objectGroupPreBarrierOffset_);
          default: MOZ_CRASH();
        }
    }

    TrampolinePtr mallocStub() const {
        return trampolineCode(mallocStubOffset_);
    }

    TrampolinePtr freeStub() const {
        return trampolineCode(freeStubOffset_);
    }

    TrampolinePtr lazyLinkStub() const {
        return trampolineCode(lazyLinkStubOffset_);
    }
    TrampolinePtr interpreterStub() const {
        return trampolineCode(interpreterStubOffset_);
    }

    bool hasJitcodeGlobalTable() const {
        return jitcodeGlobalTable_ != nullptr;
    }

    JitcodeGlobalTable* getJitcodeGlobalTable() {
        MOZ_ASSERT(hasJitcodeGlobalTable());
        return jitcodeGlobalTable_;
    }

    bool isProfilerInstrumentationEnabled(JSRuntime* rt) {
        return rt->geckoProfiler().enabled();
    }

    bool isOptimizationTrackingEnabled(ZoneGroup* group) {
        return isProfilerInstrumentationEnabled(group->runtime);
    }
};

class JitZoneGroup
{
  public:
    enum BackedgeTarget {
        BackedgeLoopHeader,
        BackedgeInterruptCheck
    };

  private:
    // Whether patchable backedges currently jump to the loop header or the
    // interrupt check.
    ZoneGroupData<BackedgeTarget> backedgeTarget_;

    // List of all backedges in all Ion code. The backedge edge list is accessed
    // asynchronously when the active thread is paused and preventBackedgePatching_
    // is false. Thus, the list must only be mutated while preventBackedgePatching_
    // is true.
    ZoneGroupData<InlineList<PatchableBackedge>> backedgeList_;
    InlineList<PatchableBackedge>& backedgeList() { return backedgeList_.ref(); }

  public:
    explicit JitZoneGroup(ZoneGroup* group);

    BackedgeTarget backedgeTarget() const {
        return backedgeTarget_;
    }
    void addPatchableBackedge(JitRuntime* jrt, PatchableBackedge* backedge) {
        MOZ_ASSERT(jrt->preventBackedgePatching());
        backedgeList().pushFront(backedge);
    }
    void removePatchableBackedge(JitRuntime* jrt, PatchableBackedge* backedge) {
        MOZ_ASSERT(jrt->preventBackedgePatching());
        backedgeList().remove(backedge);
    }

    void patchIonBackedges(JSContext* cx, BackedgeTarget target);
};

enum class CacheKind : uint8_t;
class CacheIRStubInfo;

enum class ICStubEngine : uint8_t {
    // Baseline IC, see SharedIC.h and BaselineIC.h.
    Baseline = 0,

    // Ion IC that reuses Baseline IC code, see SharedIC.h.
    IonSharedIC,

    // Ion IC, see IonIC.h.
    IonIC
};

struct CacheIRStubKey : public DefaultHasher<CacheIRStubKey> {
    struct Lookup {
        CacheKind kind;
        ICStubEngine engine;
        const uint8_t* code;
        uint32_t length;

        Lookup(CacheKind kind, ICStubEngine engine, const uint8_t* code, uint32_t length)
          : kind(kind), engine(engine), code(code), length(length)
        {}
    };

    static HashNumber hash(const Lookup& l);
    static bool match(const CacheIRStubKey& entry, const Lookup& l);

    UniquePtr<CacheIRStubInfo, JS::FreePolicy> stubInfo;

    explicit CacheIRStubKey(CacheIRStubInfo* info) : stubInfo(info) {}
    CacheIRStubKey(CacheIRStubKey&& other) : stubInfo(Move(other.stubInfo)) { }

    void operator=(CacheIRStubKey&& other) {
        stubInfo = Move(other.stubInfo);
    }
};

template<typename Key>
struct IcStubCodeMapGCPolicy
{
    static bool needsSweep(Key*, ReadBarrieredJitCode* value) {
        return IsAboutToBeFinalized(value);
    }
};

class JitZone
{
    // Allocated space for optimized baseline stubs.
    OptimizedICStubSpace optimizedStubSpace_;
    // Allocated space for cached cfg.
    CFGSpace cfgSpace_;

    // Set of CacheIRStubInfo instances used by Ion stubs in this Zone.
    using IonCacheIRStubInfoSet = HashSet<CacheIRStubKey, CacheIRStubKey, SystemAllocPolicy>;
    IonCacheIRStubInfoSet ionCacheIRStubInfoSet_;

    // Map CacheIRStubKey to shared JitCode objects.
    using BaselineCacheIRStubCodeMap = GCHashMap<CacheIRStubKey,
                                                 ReadBarrieredJitCode,
                                                 CacheIRStubKey,
                                                 SystemAllocPolicy,
                                                 IcStubCodeMapGCPolicy<CacheIRStubKey>>;
    BaselineCacheIRStubCodeMap baselineCacheIRStubCodes_;

  public:
    MOZ_MUST_USE bool init(JSContext* cx);
    void sweep();

    void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                size_t* jitZone,
                                size_t* baselineStubsOptimized,
                                size_t* cachedCFG) const;

    OptimizedICStubSpace* optimizedStubSpace() {
        return &optimizedStubSpace_;
    }
    CFGSpace* cfgSpace() {
        return &cfgSpace_;
    }

    JitCode* getBaselineCacheIRStubCode(const CacheIRStubKey::Lookup& key,
                                        CacheIRStubInfo** stubInfo) {
        auto p = baselineCacheIRStubCodes_.lookup(key);
        if (p) {
            *stubInfo = p->key().stubInfo.get();
            return p->value();
        }
        *stubInfo = nullptr;
        return nullptr;
    }
    MOZ_MUST_USE bool putBaselineCacheIRStubCode(const CacheIRStubKey::Lookup& lookup,
                                                 CacheIRStubKey& key,
                                                 JitCode* stubCode)
    {
        auto p = baselineCacheIRStubCodes_.lookupForAdd(lookup);
        MOZ_ASSERT(!p);
        return baselineCacheIRStubCodes_.add(p, Move(key), stubCode);
    }

    CacheIRStubInfo* getIonCacheIRStubInfo(const CacheIRStubKey::Lookup& key) {
        if (!ionCacheIRStubInfoSet_.initialized())
            return nullptr;
        IonCacheIRStubInfoSet::Ptr p = ionCacheIRStubInfoSet_.lookup(key);
        return p ? p->stubInfo.get() : nullptr;
    }
    MOZ_MUST_USE bool putIonCacheIRStubInfo(const CacheIRStubKey::Lookup& lookup,
                                            CacheIRStubKey& key)
    {
        if (!ionCacheIRStubInfoSet_.initialized() && !ionCacheIRStubInfoSet_.init())
            return false;
        IonCacheIRStubInfoSet::AddPtr p = ionCacheIRStubInfoSet_.lookupForAdd(lookup);
        MOZ_ASSERT(!p);
        return ionCacheIRStubInfoSet_.add(p, Move(key));
    }
    void purgeIonCacheIRStubInfo() {
        ionCacheIRStubInfoSet_.finish();
    }
};

enum class BailoutReturnStub {
    GetProp,
    GetPropSuper,
    SetProp,
    Call,
    New,
    Count
};

class JitCompartment
{
    friend class JitActivation;

    // Map ICStub keys to ICStub shared code objects.
    using ICStubCodeMap = GCHashMap<uint32_t,
                                    ReadBarrieredJitCode,
                                    DefaultHasher<uint32_t>,
                                    ZoneAllocPolicy,
                                    IcStubCodeMapGCPolicy<uint32_t>>;
    ICStubCodeMap* stubCodes_;

    // Keep track of offset into various baseline stubs' code at return
    // point from called script.
    struct BailoutReturnStubInfo
    {
        void* addr;
        uint32_t key;

        BailoutReturnStubInfo() : addr(nullptr), key(0) { }
        BailoutReturnStubInfo(void* addr_, uint32_t key_) : addr(addr_), key(key_) { }
    };
    mozilla::EnumeratedArray<BailoutReturnStub,
                             BailoutReturnStub::Count,
                             BailoutReturnStubInfo> bailoutReturnStubInfo_;

    // The JitCompartment stores stubs to concatenate strings inline and perform
    // RegExp calls inline.  These bake in zone and compartment specific
    // pointers and can't be stored in JitRuntime.
    //
    // These are weak pointers, but they can by accessed during off-thread Ion
    // compilation and therefore can't use the usual read barrier. Instead, we
    // record which stubs have been read and perform the appropriate barriers in
    // CodeGenerator::link().

    enum StubIndex : uint32_t
    {
        StringConcat = 0,
        RegExpMatcher,
        RegExpSearcher,
        RegExpTester,
        Count
    };

    mozilla::EnumeratedArray<StubIndex, StubIndex::Count, ReadBarrieredJitCode> stubs_;

    // The same approach is taken for SIMD template objects.

    mozilla::EnumeratedArray<SimdType, SimdType::Count, ReadBarrieredObject> simdTemplateObjects_;

    JitCode* generateStringConcatStub(JSContext* cx);
    JitCode* generateRegExpMatcherStub(JSContext* cx);
    JitCode* generateRegExpSearcherStub(JSContext* cx);
    JitCode* generateRegExpTesterStub(JSContext* cx);

    JitCode* getStubNoBarrier(StubIndex stub, uint32_t* requiredBarriersOut) const {
        MOZ_ASSERT(CurrentThreadIsIonCompiling());
        *requiredBarriersOut |= 1 << uint32_t(stub);
        return stubs_[stub].unbarrieredGet();
    }

  public:
    JSObject* getSimdTemplateObjectFor(JSContext* cx, Handle<SimdTypeDescr*> descr) {
        ReadBarrieredObject& tpl = simdTemplateObjects_[descr->type()];
        if (!tpl)
            tpl.set(TypedObject::createZeroed(cx, descr, 0, gc::TenuredHeap));
        return tpl.get();
    }

    JSObject* maybeGetSimdTemplateObjectFor(SimdType type) const {
        // This function is used by Eager Simd Unbox phase which can run
        // off-thread, so we cannot use the usual read barrier. For more
        // information, see the comment above
        // CodeGenerator::simdRefreshTemplatesDuringLink_.

        MOZ_ASSERT(CurrentThreadIsIonCompiling());
        return simdTemplateObjects_[type].unbarrieredGet();
    }

    JitCode* getStubCode(uint32_t key) {
        ICStubCodeMap::Ptr p = stubCodes_->lookup(key);
        if (p)
            return p->value();
        return nullptr;
    }
    MOZ_MUST_USE bool putStubCode(JSContext* cx, uint32_t key, Handle<JitCode*> stubCode) {
        MOZ_ASSERT(stubCode);
        if (!stubCodes_->putNew(key, stubCode.get())) {
            ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }
    void initBailoutReturnAddr(void* addr, uint32_t key, BailoutReturnStub kind) {
        MOZ_ASSERT(bailoutReturnStubInfo_[kind].addr == nullptr);
        bailoutReturnStubInfo_[kind] = BailoutReturnStubInfo { addr, key };
    }
    void* bailoutReturnAddr(BailoutReturnStub kind) {
        MOZ_ASSERT(bailoutReturnStubInfo_[kind].addr);
        return bailoutReturnStubInfo_[kind].addr;
    }

    JitCompartment();
    ~JitCompartment();

    MOZ_MUST_USE bool initialize(JSContext* cx);

    // Initialize code stubs only used by Ion, not Baseline.
    MOZ_MUST_USE bool ensureIonStubsExist(JSContext* cx) {
        if (stubs_[StringConcat])
            return true;
        stubs_[StringConcat] = generateStringConcatStub(cx);
        return stubs_[StringConcat];
    }

    void sweep(JSCompartment* compartment);

    void discardStubs() {
        for (ReadBarrieredJitCode& stubRef : stubs_)
            stubRef = nullptr;
    }

    JitCode* stringConcatStubNoBarrier(uint32_t* requiredBarriersOut) const {
        return getStubNoBarrier(StringConcat, requiredBarriersOut);
    }

    JitCode* regExpMatcherStubNoBarrier(uint32_t* requiredBarriersOut) const {
        return getStubNoBarrier(RegExpMatcher, requiredBarriersOut);
    }

    MOZ_MUST_USE bool ensureRegExpMatcherStubExists(JSContext* cx) {
        if (stubs_[RegExpMatcher])
            return true;
        stubs_[RegExpMatcher] = generateRegExpMatcherStub(cx);
        return stubs_[RegExpMatcher];
    }

    JitCode* regExpSearcherStubNoBarrier(uint32_t* requiredBarriersOut) const {
        return getStubNoBarrier(RegExpSearcher, requiredBarriersOut);
    }

    MOZ_MUST_USE bool ensureRegExpSearcherStubExists(JSContext* cx) {
        if (stubs_[RegExpSearcher])
            return true;
        stubs_[RegExpSearcher] = generateRegExpSearcherStub(cx);
        return stubs_[RegExpSearcher];
    }

    JitCode* regExpTesterStubNoBarrier(uint32_t* requiredBarriersOut) const {
        return getStubNoBarrier(RegExpTester, requiredBarriersOut);
    }

    MOZ_MUST_USE bool ensureRegExpTesterStubExists(JSContext* cx) {
        if (stubs_[RegExpTester])
            return true;
        stubs_[RegExpTester] = generateRegExpTesterStub(cx);
        return stubs_[RegExpTester];
    }

    // Perform the necessary read barriers on stubs and SIMD template object
    // described by the bitmasks passed in. This function can only be called
    // from the active thread.
    //
    // The stub and template object pointers must still be valid by the time
    // these methods are called. This is arranged by cancelling off-thread Ion
    // compilation at the start of GC and at the start of sweeping.
    void performStubReadBarriers(uint32_t stubsToBarrier) const;
    void performSIMDTemplateReadBarriers(uint32_t simdTemplatesToBarrier) const;

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

    bool stringsCanBeInNursery;
};

// Called from JSCompartment::discardJitCode().
void InvalidateAll(FreeOp* fop, JS::Zone* zone);
void FinishInvalidation(FreeOp* fop, JSScript* script);

// On windows systems, really large frames need to be incrementally touched.
// The following constant defines the minimum increment of the touch.
#ifdef XP_WIN
const unsigned WINDOWS_BIG_FRAME_TOUCH_INCREMENT = 4096 - 1;
#endif

// If NON_WRITABLE_JIT_CODE is enabled, this class will ensure
// JIT code is writable (has RW permissions) in its scope.
// Otherwise it's a no-op.
class MOZ_STACK_CLASS AutoWritableJitCode
{
    // Backedge patching from the signal handler will change memory protection
    // flags, so don't allow it in a AutoWritableJitCode scope.
    JitRuntime::AutoPreventBackedgePatching preventPatching_;
    JSRuntime* rt_;
    void* addr_;
    size_t size_;

  public:
    AutoWritableJitCode(JSRuntime* rt, void* addr, size_t size)
      : preventPatching_(rt), rt_(rt), addr_(addr), size_(size)
    {
        rt_->toggleAutoWritableJitCodeActive(true);
        if (!ExecutableAllocator::makeWritable(addr_, size_))
            MOZ_CRASH();
    }
    AutoWritableJitCode(void* addr, size_t size)
      : AutoWritableJitCode(TlsContext.get()->runtime(), addr, size)
    {}
    explicit AutoWritableJitCode(JitCode* code)
      : AutoWritableJitCode(code->runtimeFromActiveCooperatingThread(), code->raw(), code->bufferSize())
    {}
    ~AutoWritableJitCode() {
        if (!ExecutableAllocator::makeExecutable(addr_, size_))
            MOZ_CRASH();
        rt_->toggleAutoWritableJitCodeActive(false);
    }
};

class MOZ_STACK_CLASS MaybeAutoWritableJitCode
{
    mozilla::Maybe<AutoWritableJitCode> awjc_;

  public:
    MaybeAutoWritableJitCode(void* addr, size_t size, ReprotectCode reprotect) {
        if (reprotect)
            awjc_.emplace(addr, size);
    }
    MaybeAutoWritableJitCode(JitCode* code, ReprotectCode reprotect) {
        if (reprotect)
            awjc_.emplace(code);
    }
};

} // namespace jit
} // namespace js

#endif /* jit_JitCompartment_h */
