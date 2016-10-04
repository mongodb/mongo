/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitCompartment_h
#define jit_JitCompartment_h

#include "mozilla/Array.h"
#include "mozilla/MemoryReporting.h"

#include "jsweakcache.h"

#include "builtin/TypedObject.h"
#include "jit/CompileInfo.h"
#include "jit/ICStubSpace.h"
#include "jit/IonCode.h"
#include "jit/JitFrames.h"
#include "jit/shared/Assembler-shared.h"
#include "js/Value.h"
#include "vm/Stack.h"

namespace js {
namespace jit {

class FrameSizeClass;

enum EnterJitType {
    EnterJitBaseline = 0,
    EnterJitOptimized = 1
};

struct EnterJitData
{
    explicit EnterJitData(JSContext* cx)
      : scopeChain(cx),
        result(cx)
    {}

    uint8_t* jitcode;
    InterpreterFrame* osrFrame;

    void* calleeToken;

    Value* maxArgv;
    unsigned maxArgc;
    unsigned numActualArgs;
    unsigned osrNumStackValues;

    RootedObject scopeChain;
    RootedValue result;

    bool constructing;
};

typedef void (*EnterJitCode)(void* code, unsigned argc, Value* argv, InterpreterFrame* fp,
                             CalleeToken calleeToken, JSObject* scopeChain,
                             size_t numStackValues, Value* vp);

class JitcodeGlobalTable;

// Information about a loop backedge in the runtime, which can be set to
// point to either the loop header or to an OOL interrupt checking stub,
// if signal handlers are being used to implement interrupts.
class PatchableBackedge : public InlineListNode<PatchableBackedge>
{
    friend class JitRuntime;

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
    friend class JitCompartment;

    // Executable allocator for all code except asm.js code.
    ExecutableAllocator execAlloc_;

    // Shared exception-handler tail.
    JitCode* exceptionTail_;

    // Shared post-bailout-handler tail.
    JitCode* bailoutTail_;

    // Shared profiler exit frame tail.
    JitCode* profilerExitFrameTail_;

    // Trampoline for entering JIT code. Contains OSR prologue.
    JitCode* enterJIT_;

    // Trampoline for entering baseline JIT code.
    JitCode* enterBaselineJIT_;

    // Vector mapping frame class sizes to bailout tables.
    Vector<JitCode*, 4, SystemAllocPolicy> bailoutTables_;

    // Generic bailout table; used if the bailout table overflows.
    JitCode* bailoutHandler_;

    // Argument-rectifying thunk, in the case of insufficient arguments passed
    // to a function call site.
    JitCode* argumentsRectifier_;
    void* argumentsRectifierReturnAddr_;

    // Thunk that invalides an (Ion compiled) caller on the Ion stack.
    JitCode* invalidator_;

    // Thunk that calls the GC pre barrier.
    JitCode* valuePreBarrier_;
    JitCode* stringPreBarrier_;
    JitCode* objectPreBarrier_;
    JitCode* shapePreBarrier_;
    JitCode* objectGroupPreBarrier_;

    // Thunk to call malloc/free.
    JitCode* mallocStub_;
    JitCode* freeStub_;

    // Thunk called to finish compilation of an IonScript.
    JitCode* lazyLinkStub_;

    // Thunk used by the debugger for breakpoint and step mode.
    JitCode* debugTrapHandler_;

    // Thunk used to fix up on-stack recompile of baseline scripts.
    JitCode* baselineDebugModeOSRHandler_;
    void* baselineDebugModeOSRHandlerNoFrameRegPopAddr_;

    // Map VMFunction addresses to the JitCode of the wrapper.
    typedef WeakCache<const VMFunction*, JitCode*> VMWrapperMap;
    VMWrapperMap* functionWrappers_;

    // Buffer for OSR from baseline to Ion. To avoid holding on to this for
    // too long, it's also freed in JitCompartment::mark and in EnterBaseline
    // (after returning from JIT code).
    uint8_t* osrTempData_;

    // List of all backedges in all Ion code. The backedge edge list is accessed
    // asynchronously when the main thread is paused and mutatingBackedgeList_
    // is false. Thus, the list must only be mutated while mutatingBackedgeList_
    // is true.
    volatile bool mutatingBackedgeList_;
    InlineList<PatchableBackedge> backedgeList_;

    // In certain cases, we want to optimize certain opcodes to typed instructions,
    // to avoid carrying an extra register to feed into an unbox. Unfortunately,
    // that's not always possible. For example, a GetPropertyCacheT could return a
    // typed double, but if it takes its out-of-line path, it could return an
    // object, and trigger invalidation. The invalidation bailout will consider the
    // return value to be a double, and create a garbage Value.
    //
    // To allow the GetPropertyCacheT optimization, we allow the ability for
    // GetPropertyCache to override the return value at the top of the stack - the
    // value that will be temporarily corrupt. This special override value is set
    // only in callVM() targets that are about to return *and* have invalidated
    // their callee.
    js::Value ionReturnOverride_;

    // Global table of jitcode native address => bytecode address mappings.
    JitcodeGlobalTable* jitcodeGlobalTable_;

  private:
    JitCode* generateLazyLinkStub(JSContext* cx);
    JitCode* generateProfilerExitFrameTailStub(JSContext* cx);
    JitCode* generateExceptionTailStub(JSContext* cx, void* handler);
    JitCode* generateBailoutTailStub(JSContext* cx);
    JitCode* generateEnterJIT(JSContext* cx, EnterJitType type);
    JitCode* generateArgumentsRectifier(JSContext* cx, void** returnAddrOut);
    JitCode* generateBailoutTable(JSContext* cx, uint32_t frameClass);
    JitCode* generateBailoutHandler(JSContext* cx);
    JitCode* generateInvalidator(JSContext* cx);
    JitCode* generatePreBarrier(JSContext* cx, MIRType type);
    JitCode* generateMallocStub(JSContext* cx);
    JitCode* generateFreeStub(JSContext* cx);
    JitCode* generateDebugTrapHandler(JSContext* cx);
    JitCode* generateBaselineDebugModeOSRHandler(JSContext* cx, uint32_t* noFrameRegPopOffsetOut);
    JitCode* generateVMWrapper(JSContext* cx, const VMFunction& f);

  public:
    JitRuntime();
    ~JitRuntime();
    bool initialize(JSContext* cx);

    uint8_t* allocateOsrTempData(size_t size);
    void freeOsrTempData();

    static void Mark(JSTracer* trc);
    static void MarkJitcodeGlobalTableUnconditionally(JSTracer* trc);
    static bool MarkJitcodeGlobalTableIteratively(JSTracer* trc);
    static void SweepJitcodeGlobalTable(JSRuntime* rt);

    ExecutableAllocator& execAlloc() {
        return execAlloc_;
    }

    class AutoMutateBackedges
    {
        JitRuntime* jrt_;
      public:
        explicit AutoMutateBackedges(JitRuntime* jrt) : jrt_(jrt) {
            MOZ_ASSERT(!jrt->mutatingBackedgeList_);
            jrt->mutatingBackedgeList_ = true;
        }
        ~AutoMutateBackedges() {
            MOZ_ASSERT(jrt_->mutatingBackedgeList_);
            jrt_->mutatingBackedgeList_ = false;
        }
    };

    bool mutatingBackedgeList() const {
        return mutatingBackedgeList_;
    }
    void addPatchableBackedge(PatchableBackedge* backedge) {
        MOZ_ASSERT(mutatingBackedgeList_);
        backedgeList_.pushFront(backedge);
    }
    void removePatchableBackedge(PatchableBackedge* backedge) {
        MOZ_ASSERT(mutatingBackedgeList_);
        backedgeList_.remove(backedge);
    }

    enum BackedgeTarget {
        BackedgeLoopHeader,
        BackedgeInterruptCheck
    };

    void patchIonBackedges(JSRuntime* rt, BackedgeTarget target);

    JitCode* getVMWrapper(const VMFunction& f) const;
    JitCode* debugTrapHandler(JSContext* cx);
    JitCode* getBaselineDebugModeOSRHandler(JSContext* cx);
    void* getBaselineDebugModeOSRHandlerAddress(JSContext* cx, bool popFrameReg);

    JitCode* getGenericBailoutHandler() const {
        return bailoutHandler_;
    }

    JitCode* getExceptionTail() const {
        return exceptionTail_;
    }

    JitCode* getBailoutTail() const {
        return bailoutTail_;
    }

    JitCode* getProfilerExitFrameTail() const {
        return profilerExitFrameTail_;
    }

    JitCode* getBailoutTable(const FrameSizeClass& frameClass) const;

    JitCode* getArgumentsRectifier() const {
        return argumentsRectifier_;
    }

    void* getArgumentsRectifierReturnAddr() const {
        return argumentsRectifierReturnAddr_;
    }

    JitCode* getInvalidationThunk() const {
        return invalidator_;
    }

    EnterJitCode enterIon() const {
        return enterJIT_->as<EnterJitCode>();
    }

    EnterJitCode enterBaseline() const {
        return enterBaselineJIT_->as<EnterJitCode>();
    }

    JitCode* preBarrier(MIRType type) const {
        switch (type) {
          case MIRType_Value: return valuePreBarrier_;
          case MIRType_String: return stringPreBarrier_;
          case MIRType_Object: return objectPreBarrier_;
          case MIRType_Shape: return shapePreBarrier_;
          case MIRType_ObjectGroup: return objectGroupPreBarrier_;
          default: MOZ_CRASH();
        }
    }

    JitCode* mallocStub() const {
        return mallocStub_;
    }

    JitCode* freeStub() const {
        return freeStub_;
    }

    JitCode* lazyLinkStub() const {
        return lazyLinkStub_;
    }

    bool hasIonReturnOverride() const {
        return !ionReturnOverride_.isMagic(JS_ARG_POISON);
    }
    js::Value takeIonReturnOverride() {
        js::Value v = ionReturnOverride_;
        ionReturnOverride_ = js::MagicValue(JS_ARG_POISON);
        return v;
    }
    void setIonReturnOverride(const js::Value& v) {
        MOZ_ASSERT(!hasIonReturnOverride());
        MOZ_ASSERT(!v.isMagic());
        ionReturnOverride_ = v;
    }

    bool hasJitcodeGlobalTable() const {
        return jitcodeGlobalTable_ != nullptr;
    }

    JitcodeGlobalTable* getJitcodeGlobalTable() {
        MOZ_ASSERT(hasJitcodeGlobalTable());
        return jitcodeGlobalTable_;
    }

    bool isProfilerInstrumentationEnabled(JSRuntime* rt) {
        return rt->spsProfiler.enabled();
    }

    bool isOptimizationTrackingEnabled(JSRuntime* rt) {
        return isProfilerInstrumentationEnabled(rt);
    }
};

class JitZone
{
    // Allocated space for optimized baseline stubs.
    OptimizedICStubSpace optimizedStubSpace_;

  public:
    OptimizedICStubSpace* optimizedStubSpace() {
        return &optimizedStubSpace_;
    }
};

class JitCompartment
{
    friend class JitActivation;

    // Map ICStub keys to ICStub shared code objects.
    typedef WeakValueCache<uint32_t, ReadBarrieredJitCode> ICStubCodeMap;
    ICStubCodeMap* stubCodes_;

    // Keep track of offset into various baseline stubs' code at return
    // point from called script.
    void* baselineCallReturnAddrs_[2];
    void* baselineGetPropReturnAddr_;
    void* baselineSetPropReturnAddr_;

    // Stubs to concatenate two strings inline, or perform RegExp calls inline.
    // These bake in zone and compartment specific pointers and can't be stored
    // in JitRuntime. These are weak pointers, but are not declared as
    // ReadBarriered since they are only read from during Ion compilation,
    // which may occur off thread and whose barriers are captured during
    // CodeGenerator::link.
    JitCode* stringConcatStub_;
    JitCode* regExpExecStub_;
    JitCode* regExpTestStub_;

    mozilla::Array<ReadBarrieredObject, SimdTypeDescr::LAST_TYPE + 1> simdTemplateObjects_;

    JitCode* generateStringConcatStub(JSContext* cx);
    JitCode* generateRegExpExecStub(JSContext* cx);
    JitCode* generateRegExpTestStub(JSContext* cx);

  public:
    JSObject* getSimdTemplateObjectFor(JSContext* cx, Handle<SimdTypeDescr*> descr) {
        ReadBarrieredObject& tpl = simdTemplateObjects_[descr->type()];
        if (!tpl)
            tpl.set(TypedObject::createZeroed(cx, descr, 0, gc::TenuredHeap));
        return tpl.get();
    }

    JSObject* maybeGetSimdTemplateObjectFor(SimdTypeDescr::Type type) const {
        const ReadBarrieredObject& tpl = simdTemplateObjects_[type];

        // This function is used by Eager Simd Unbox phase, so we cannot use the
        // read barrier. For more information, see the comment above
        // CodeGenerator::simdRefreshTemplatesDuringLink_ .
        return tpl.unbarrieredGet();
    }

    // This function is used to call the read barrier, to mark the SIMD template
    // type as used. This function can only be called from the main thread.
    void registerSimdTemplateObjectFor(SimdTypeDescr::Type type) {
        ReadBarrieredObject& tpl = simdTemplateObjects_[type];
        MOZ_ASSERT(tpl.unbarrieredGet());
        tpl.get();
    }

    JitCode* getStubCode(uint32_t key) {
        ICStubCodeMap::AddPtr p = stubCodes_->lookupForAdd(key);
        if (p)
            return p->value();
        return nullptr;
    }
    bool putStubCode(JSContext* cx, uint32_t key, Handle<JitCode*> stubCode) {
        MOZ_ASSERT(stubCode);
        if (!stubCodes_->putNew(key, stubCode.get())) {
            ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }
    void initBaselineCallReturnAddr(void* addr, bool constructing) {
        MOZ_ASSERT(baselineCallReturnAddrs_[constructing] == nullptr);
        baselineCallReturnAddrs_[constructing] = addr;
    }
    void* baselineCallReturnAddr(bool constructing) {
        MOZ_ASSERT(baselineCallReturnAddrs_[constructing] != nullptr);
        return baselineCallReturnAddrs_[constructing];
    }
    void initBaselineGetPropReturnAddr(void* addr) {
        MOZ_ASSERT(baselineGetPropReturnAddr_ == nullptr);
        baselineGetPropReturnAddr_ = addr;
    }
    void* baselineGetPropReturnAddr() {
        MOZ_ASSERT(baselineGetPropReturnAddr_ != nullptr);
        return baselineGetPropReturnAddr_;
    }
    void initBaselineSetPropReturnAddr(void* addr) {
        MOZ_ASSERT(baselineSetPropReturnAddr_ == nullptr);
        baselineSetPropReturnAddr_ = addr;
    }
    void* baselineSetPropReturnAddr() {
        MOZ_ASSERT(baselineSetPropReturnAddr_ != nullptr);
        return baselineSetPropReturnAddr_;
    }

    void toggleBarriers(bool enabled);

  public:
    JitCompartment();
    ~JitCompartment();

    bool initialize(JSContext* cx);

    // Initialize code stubs only used by Ion, not Baseline.
    bool ensureIonStubsExist(JSContext* cx);

    void mark(JSTracer* trc, JSCompartment* compartment);
    void sweep(FreeOp* fop, JSCompartment* compartment);

    JitCode* stringConcatStubNoBarrier() const {
        return stringConcatStub_;
    }

    JitCode* regExpExecStubNoBarrier() const {
        return regExpExecStub_;
    }

    bool ensureRegExpExecStubExists(JSContext* cx) {
        if (regExpExecStub_)
            return true;
        regExpExecStub_ = generateRegExpExecStub(cx);
        return regExpExecStub_ != nullptr;
    }

    JitCode* regExpTestStubNoBarrier() const {
        return regExpTestStub_;
    }

    bool ensureRegExpTestStubExists(JSContext* cx) {
        if (regExpTestStub_)
            return true;
        regExpTestStub_ = generateRegExpTestStub(cx);
        return regExpTestStub_ != nullptr;
    }
};

// Called from JSCompartment::discardJitCode().
void InvalidateAll(FreeOp* fop, JS::Zone* zone);
void FinishInvalidation(FreeOp* fop, JSScript* script);

// On windows systems, really large frames need to be incrementally touched.
// The following constant defines the minimum increment of the touch.
#ifdef XP_WIN
const unsigned WINDOWS_BIG_FRAME_TOUCH_INCREMENT = 4096 - 1;
#endif

// If ExecutableAllocator::nonWritableJitCode is |true|, this class will ensure
// JIT code is writable (has RW permissions) in its scope. If nonWritableJitCode
// is |false|, it's a no-op.
class MOZ_STACK_CLASS AutoWritableJitCode
{
    JSRuntime* rt_;
    void* addr_;
    size_t size_;

  public:
    AutoWritableJitCode(JSRuntime* rt, void* addr, size_t size)
      : rt_(rt), addr_(addr), size_(size)
    {
        rt_->toggleAutoWritableJitCodeActive(true);
        ExecutableAllocator::makeWritable(addr_, size_);
    }
    AutoWritableJitCode(void* addr, size_t size)
      : AutoWritableJitCode(TlsPerThreadData.get()->runtimeFromMainThread(), addr, size)
    {}
    explicit AutoWritableJitCode(JitCode* code)
      : AutoWritableJitCode(code->runtimeFromMainThread(), code->raw(), code->bufferSize())
    {}
    ~AutoWritableJitCode() {
        ExecutableAllocator::makeExecutable(addr_, size_);
        rt_->toggleAutoWritableJitCodeActive(false);
    }
};

enum ReprotectCode { Reprotect = true, DontReprotect = false };

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
