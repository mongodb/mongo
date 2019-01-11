/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineJIT_h
#define jit_BaselineJIT_h

#include "mozilla/MemoryReporting.h"

#include "ds/LifoAlloc.h"
#include "jit/Bailouts.h"
#include "jit/IonCode.h"
#include "jit/MacroAssembler.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "vm/TraceLogging.h"

namespace js {
namespace jit {

class StackValue;
class BaselineICEntry;
class ICStub;
class ControlFlowGraph;

class PCMappingSlotInfo
{
    uint8_t slotInfo_;

  public:
    // SlotInfo encoding:
    //  Bits 0 & 1: number of slots at top of stack which are unsynced.
    //  Bits 2 & 3: SlotLocation of top slot value (only relevant if numUnsynced > 0).
    //  Bits 3 & 4: SlotLocation of next slot value (only relevant if numUnsynced > 1).
    enum SlotLocation { SlotInR0 = 0, SlotInR1 = 1, SlotIgnore = 3 };

    PCMappingSlotInfo()
      : slotInfo_(0)
    { }

    explicit PCMappingSlotInfo(uint8_t slotInfo)
      : slotInfo_(slotInfo)
    { }

    static inline bool ValidSlotLocation(SlotLocation loc) {
        return (loc == SlotInR0) || (loc == SlotInR1) || (loc == SlotIgnore);
    }

    static SlotLocation ToSlotLocation(const StackValue* stackVal);

    inline static PCMappingSlotInfo MakeSlotInfo() { return PCMappingSlotInfo(0); }

    inline static PCMappingSlotInfo MakeSlotInfo(SlotLocation topSlotLoc) {
        MOZ_ASSERT(ValidSlotLocation(topSlotLoc));
        return PCMappingSlotInfo(1 | (topSlotLoc << 2));
    }

    inline static PCMappingSlotInfo MakeSlotInfo(SlotLocation topSlotLoc, SlotLocation nextSlotLoc) {
        MOZ_ASSERT(ValidSlotLocation(topSlotLoc));
        MOZ_ASSERT(ValidSlotLocation(nextSlotLoc));
        return PCMappingSlotInfo(2 | (topSlotLoc << 2) | (nextSlotLoc) << 4);
    }

    inline unsigned numUnsynced() const {
        return slotInfo_ & 0x3;
    }
    inline SlotLocation topSlotLocation() const {
        return static_cast<SlotLocation>((slotInfo_ >> 2) & 0x3);
    }
    inline SlotLocation nextSlotLocation() const {
        return static_cast<SlotLocation>((slotInfo_ >> 4) & 0x3);
    }
    inline uint8_t toByte() const {
        return slotInfo_;
    }
};

// A CompactBuffer is used to store native code offsets (relative to the
// previous pc) and PCMappingSlotInfo bytes. To allow binary search into this
// table, we maintain a second table of "index" entries. Every X ops, the
// compiler will add an index entry, so that from the index entry to the
// actual native code offset, we have to iterate at most X times.
struct PCMappingIndexEntry
{
    // jsbytecode offset.
    uint32_t pcOffset;

    // Native code offset.
    uint32_t nativeOffset;

    // Offset in the CompactBuffer where data for pcOffset starts.
    uint32_t bufferOffset;
};

// Describes a single wasm::ImportExit which jumps (via an import with
// the given index) directly to a BaselineScript or IonScript.
struct DependentWasmImport
{
    wasm::Instance* instance;
    size_t importIndex;

    DependentWasmImport(wasm::Instance& instance, size_t importIndex)
      : instance(&instance),
        importIndex(importIndex)
    { }
};

struct BaselineScript
{
  public:
    // Largest script that the baseline compiler will attempt to compile.
#if defined(JS_CODEGEN_ARM)
    // ARM branches can only reach 32MB, and the macroassembler doesn't mitigate
    // that limitation. Use a stricter limit on the acceptable script size to
    // avoid crashing when branches go out of range.
    static const uint32_t MAX_JSSCRIPT_LENGTH = 1000000u;
#else
    static const uint32_t MAX_JSSCRIPT_LENGTH = 0x0fffffffu;
#endif

    // Limit the locals on a given script so that stack check on baseline frames
    // doesn't overflow a uint32_t value.
    // (MAX_JSSCRIPT_SLOTS * sizeof(Value)) must fit within a uint32_t.
    static const uint32_t MAX_JSSCRIPT_SLOTS = 0xffffu;

  private:
    // Code pointer containing the actual method.
    HeapPtr<JitCode*> method_;

    // For functions with a call object, template objects to use for the call
    // object and decl env object (linked via the call object's enclosing
    // scope).
    HeapPtr<EnvironmentObject*> templateEnv_;

    // Allocated space for fallback stubs.
    FallbackICStubSpace fallbackStubSpace_;

    // If non-null, the list of wasm::Modules that contain an optimized call
    // directly to this script.
    Vector<DependentWasmImport>* dependentWasmImports_;

    // Native code offset right before the scope chain is initialized.
    uint32_t prologueOffset_;

    // Native code offset right before the frame is popped and the method
    // returned from.
    uint32_t epilogueOffset_;

    // The offsets for the toggledJump instructions for profiler instrumentation.
    uint32_t profilerEnterToggleOffset_;
    uint32_t profilerExitToggleOffset_;

    // The offsets and event used for Tracelogger toggling.
#ifdef JS_TRACE_LOGGING
# ifdef DEBUG
    bool traceLoggerScriptsEnabled_;
    bool traceLoggerEngineEnabled_;
# endif
    TraceLoggerEvent traceLoggerScriptEvent_;
#endif

    // Native code offsets right after the debug prologue VM call returns, or
    // would have returned. This offset is recorded even when debug mode is
    // off to aid on-stack debug mode recompilation.
    //
    // We don't need one for the debug epilogue because that always happens
    // right before the epilogue, so we just use the epilogue offset.
    uint32_t postDebugPrologueOffset_;

  public:
    enum Flag {
        // Flag set by JSScript::argumentsOptimizationFailed. Similar to
        // JSScript::needsArgsObj_, but can be read from JIT code.
        NEEDS_ARGS_OBJ = 1 << 0,

        // Flag set when discarding JIT code, to indicate this script is
        // on the stack and should not be discarded.
        ACTIVE = 1 << 1,

        // Flag set when the script contains any writes to its on-stack
        // (rather than call object stored) arguments.
        MODIFIES_ARGUMENTS = 1 << 2,

        // Flag set when compiled for use with Debugger. Handles various
        // Debugger hooks and compiles toggled calls for traps.
        HAS_DEBUG_INSTRUMENTATION = 1 << 3,

        // Flag set if this script has ever been Ion compiled, either directly
        // or inlined into another script. This is cleared when the script's
        // type information or caches are cleared.
        ION_COMPILED_OR_INLINED = 1 << 4,

        // Flag is set if this script has profiling instrumentation turned on.
        PROFILER_INSTRUMENTATION_ON = 1 << 5,

        // Whether this script uses its environment chain. This is currently
        // determined by the BytecodeAnalysis and cached on the BaselineScript
        // for IonBuilder.
        USES_ENVIRONMENT_CHAIN = 1 << 6,
    };

  private:
    uint32_t flags_;

  private:
    void trace(JSTracer* trc);

    uint32_t icEntriesOffset_;
    uint32_t icEntries_;

    uint32_t pcMappingIndexOffset_;
    uint32_t pcMappingIndexEntries_;

    uint32_t pcMappingOffset_;
    uint32_t pcMappingSize_;

    // List mapping indexes of bytecode type sets to the offset of the opcode
    // they correspond to, for use by TypeScript::BytecodeTypes.
    uint32_t bytecodeTypeMapOffset_;

    // For generator scripts, we store the native code address for each yield
    // instruction.
    uint32_t yieldEntriesOffset_;

    // By default tracelogger is disabled. Therefore we disable the logging code
    // by default. We store the offsets we must patch to enable the logging.
    uint32_t traceLoggerToggleOffsetsOffset_;
    uint32_t numTraceLoggerToggleOffsets_;

    // The total bytecode length of all scripts we inlined when we Ion-compiled
    // this script. 0 if Ion did not compile this script or if we didn't inline
    // anything.
    uint16_t inlinedBytecodeLength_;

    // The max inlining depth where we can still inline all functions we inlined
    // when we Ion-compiled this script. This starts as UINT8_MAX, since we have
    // no data yet, and won't affect inlining heuristics in that case. The value
    // is updated when we Ion-compile this script. See makeInliningDecision for
    // more info.
    uint8_t maxInliningDepth_;

    // An ion compilation that is ready, but isn't linked yet.
    IonBuilder *pendingBuilder_;

    ControlFlowGraph* controlFlowGraph_;

  public:
    // Do not call directly, use BaselineScript::New. This is public for cx->new_.
    BaselineScript(uint32_t prologueOffset, uint32_t epilogueOffset,
                   uint32_t profilerEnterToggleOffset,
                   uint32_t profilerExitToggleOffset,
                   uint32_t postDebugPrologueOffset);

    ~BaselineScript() {
        // The contents of the fallback stub space are removed and freed
        // separately after the next minor GC. See BaselineScript::Destroy.
        MOZ_ASSERT(fallbackStubSpace_.isEmpty());
    }

    static BaselineScript* New(JSScript* jsscript,
                               uint32_t prologueOffset, uint32_t epilogueOffset,
                               uint32_t profilerEnterToggleOffset,
                               uint32_t profilerExitToggleOffset,
                               uint32_t postDebugPrologueOffset,
                               size_t icEntries,
                               size_t pcMappingIndexEntries, size_t pcMappingSize,
                               size_t bytecodeTypeMapEntries,
                               size_t yieldEntries,
                               size_t traceLoggerToggleOffsetEntries);

    static void Trace(JSTracer* trc, BaselineScript* script);
    static void Destroy(FreeOp* fop, BaselineScript* script);

    void purgeOptimizedStubs(Zone* zone);

    static inline size_t offsetOfMethod() {
        return offsetof(BaselineScript, method_);
    }

    void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf, size_t* data,
                                size_t* fallbackStubs) const {
        *data += mallocSizeOf(this);

        // |data| already includes the ICStubSpace itself, so use
        // sizeOfExcludingThis.
        *fallbackStubs += fallbackStubSpace_.sizeOfExcludingThis(mallocSizeOf);
    }

    bool active() const {
        return flags_ & ACTIVE;
    }
    void setActive() {
        flags_ |= ACTIVE;
    }
    void resetActive() {
        flags_ &= ~ACTIVE;
    }

    void setNeedsArgsObj() {
        flags_ |= NEEDS_ARGS_OBJ;
    }

    void setModifiesArguments() {
        flags_ |= MODIFIES_ARGUMENTS;
    }
    bool modifiesArguments() {
        return flags_ & MODIFIES_ARGUMENTS;
    }

    void setHasDebugInstrumentation() {
        flags_ |= HAS_DEBUG_INSTRUMENTATION;
    }
    bool hasDebugInstrumentation() const {
        return flags_ & HAS_DEBUG_INSTRUMENTATION;
    }

    void setIonCompiledOrInlined() {
        flags_ |= ION_COMPILED_OR_INLINED;
    }
    void clearIonCompiledOrInlined() {
        flags_ &= ~ION_COMPILED_OR_INLINED;
    }
    bool ionCompiledOrInlined() const {
        return flags_ & ION_COMPILED_OR_INLINED;
    }

    void setUsesEnvironmentChain() {
        flags_ |= USES_ENVIRONMENT_CHAIN;
    }
    bool usesEnvironmentChain() const {
        return flags_ & USES_ENVIRONMENT_CHAIN;
    }

    uint32_t prologueOffset() const {
        return prologueOffset_;
    }
    uint8_t* prologueEntryAddr() const {
        return method_->raw() + prologueOffset_;
    }

    uint32_t epilogueOffset() const {
        return epilogueOffset_;
    }
    uint8_t* epilogueEntryAddr() const {
        return method_->raw() + epilogueOffset_;
    }

    uint32_t postDebugPrologueOffset() const {
        return postDebugPrologueOffset_;
    }
    uint8_t* postDebugPrologueAddr() const {
        return method_->raw() + postDebugPrologueOffset_;
    }

    BaselineICEntry* icEntryList() {
        return (BaselineICEntry*)(reinterpret_cast<uint8_t*>(this) + icEntriesOffset_);
    }
    uint8_t** yieldEntryList() {
        return (uint8_t**)(reinterpret_cast<uint8_t*>(this) + yieldEntriesOffset_);
    }
    PCMappingIndexEntry* pcMappingIndexEntryList() {
        return (PCMappingIndexEntry*)(reinterpret_cast<uint8_t*>(this) + pcMappingIndexOffset_);
    }
    uint8_t* pcMappingData() {
        return reinterpret_cast<uint8_t*>(this) + pcMappingOffset_;
    }
    FallbackICStubSpace* fallbackStubSpace() {
        return &fallbackStubSpace_;
    }

    JitCode* method() const {
        return method_;
    }
    void setMethod(JitCode* code) {
        MOZ_ASSERT(!method_);
        method_ = code;
    }

    EnvironmentObject* templateEnvironment() const {
        return templateEnv_;
    }
    void setTemplateEnvironment(EnvironmentObject* templateEnv) {
        MOZ_ASSERT(!templateEnv_);
        templateEnv_ = templateEnv;
    }

    bool containsCodeAddress(uint8_t* addr) const {
        return method()->raw() <= addr && addr <= method()->raw() + method()->instructionsSize();
    }

    BaselineICEntry* maybeICEntryFromPCOffset(uint32_t pcOffset);
    BaselineICEntry* maybeICEntryFromPCOffset(uint32_t pcOffset,
                                              BaselineICEntry* prevLookedUpEntry);

    BaselineICEntry& icEntry(size_t index);
    BaselineICEntry& icEntryFromReturnOffset(CodeOffset returnOffset);
    BaselineICEntry& icEntryFromPCOffset(uint32_t pcOffset);
    BaselineICEntry& icEntryFromPCOffset(uint32_t pcOffset, BaselineICEntry* prevLookedUpEntry);
    BaselineICEntry& callVMEntryFromPCOffset(uint32_t pcOffset);
    BaselineICEntry& stackCheckICEntry(bool earlyCheck);
    BaselineICEntry& warmupCountICEntry();
    BaselineICEntry& icEntryFromReturnAddress(uint8_t* returnAddr);
    uint8_t* returnAddressForIC(const BaselineICEntry& ent);

    size_t numICEntries() const {
        return icEntries_;
    }

    void copyICEntries(JSScript* script, const BaselineICEntry* entries);
    void adoptFallbackStubs(FallbackICStubSpace* stubSpace);

    void copyYieldAndAwaitEntries(JSScript* script, Vector<uint32_t>& yieldAndAwaitOffsets);

    PCMappingIndexEntry& pcMappingIndexEntry(size_t index);
    CompactBufferReader pcMappingReader(size_t indexEntry);

    size_t numPCMappingIndexEntries() const {
        return pcMappingIndexEntries_;
    }

    void copyPCMappingIndexEntries(const PCMappingIndexEntry* entries);
    void copyPCMappingEntries(const CompactBufferWriter& entries);

    uint8_t* nativeCodeForPC(JSScript* script, jsbytecode* pc,
                             PCMappingSlotInfo* slotInfo = nullptr);

    // Return the bytecode offset for a given native code address. Be careful
    // when using this method: we don't emit code for some bytecode ops, so
    // the result may not be accurate.
    jsbytecode* approximatePcForNativeAddress(JSScript* script, uint8_t* nativeAddress);

    MOZ_MUST_USE bool addDependentWasmImport(JSContext* cx, wasm::Instance& instance, uint32_t idx);
    void removeDependentWasmImport(wasm::Instance& instance, uint32_t idx);
    void unlinkDependentWasmImports(FreeOp* fop);
    void clearDependentWasmImports();

    // Toggle debug traps (used for breakpoints and step mode) in the script.
    // If |pc| is nullptr, toggle traps for all ops in the script. Else, only
    // toggle traps at |pc|.
    void toggleDebugTraps(JSScript* script, jsbytecode* pc);

    void toggleProfilerInstrumentation(bool enable);
    bool isProfilerInstrumentationOn() const {
        return flags_ & PROFILER_INSTRUMENTATION_ON;
    }

#ifdef JS_TRACE_LOGGING
    void initTraceLogger(JSScript* script, const Vector<CodeOffset>& offsets);
    void toggleTraceLoggerScripts(JSScript* script, bool enable);
    void toggleTraceLoggerEngine(bool enable);

    static size_t offsetOfTraceLoggerScriptEvent() {
        return offsetof(BaselineScript, traceLoggerScriptEvent_);
    }

    uint32_t* traceLoggerToggleOffsets() {
        MOZ_ASSERT(traceLoggerToggleOffsetsOffset_);
        return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) +
                                           traceLoggerToggleOffsetsOffset_);
    }
#endif

    void noteAccessedGetter(uint32_t pcOffset);
    void noteHasDenseAdd(uint32_t pcOffset);

    static size_t offsetOfFlags() {
        return offsetof(BaselineScript, flags_);
    }
    static size_t offsetOfYieldEntriesOffset() {
        return offsetof(BaselineScript, yieldEntriesOffset_);
    }

    static void writeBarrierPre(Zone* zone, BaselineScript* script);

    uint32_t* bytecodeTypeMap() {
        MOZ_ASSERT(bytecodeTypeMapOffset_);
        return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + bytecodeTypeMapOffset_);
    }

    uint8_t maxInliningDepth() const {
        return maxInliningDepth_;
    }
    void setMaxInliningDepth(uint32_t depth) {
        MOZ_ASSERT(depth <= UINT8_MAX);
        maxInliningDepth_ = depth;
    }
    void resetMaxInliningDepth() {
        maxInliningDepth_ = UINT8_MAX;
    }

    uint16_t inlinedBytecodeLength() const {
        return inlinedBytecodeLength_;
    }
    void setInlinedBytecodeLength(uint32_t len) {
        if (len > UINT16_MAX)
            len = UINT16_MAX;
        inlinedBytecodeLength_ = len;
    }

    bool hasPendingIonBuilder() const {
        return !!pendingBuilder_;
    }

    js::jit::IonBuilder* pendingIonBuilder() {
        MOZ_ASSERT(hasPendingIonBuilder());
        return pendingBuilder_;
    }
    void setPendingIonBuilder(JSRuntime* rt, JSScript* script, js::jit::IonBuilder* builder) {
        MOZ_ASSERT(script->baselineScript() == this);
        MOZ_ASSERT(!builder || !hasPendingIonBuilder());

        if (script->isIonCompilingOffThread())
            script->setIonScript(rt, ION_PENDING_SCRIPT);

        pendingBuilder_ = builder;

        // lazy linking cannot happen during asmjs to ion.
        clearDependentWasmImports();

        script->updateJitCodeRaw(rt);
    }
    void removePendingIonBuilder(JSRuntime* rt, JSScript* script) {
        setPendingIonBuilder(rt, script, nullptr);
        if (script->maybeIonScript() == ION_PENDING_SCRIPT)
            script->setIonScript(rt, nullptr);
    }

    const ControlFlowGraph* controlFlowGraph() const {
        return controlFlowGraph_;
    }

    void setControlFlowGraph(ControlFlowGraph* controlFlowGraph) {
        controlFlowGraph_ = controlFlowGraph;
    }

};
static_assert(sizeof(BaselineScript) % sizeof(uintptr_t) == 0,
              "The data attached to the script must be aligned for fast JIT access.");

inline bool
IsBaselineEnabled(JSContext* cx)
{
#ifdef JS_CODEGEN_NONE
    return false;
#else
    return cx->options().baseline() &&
           cx->runtime()->jitSupportsFloatingPoint;
#endif
}

MethodStatus
CanEnterBaselineMethod(JSContext* cx, RunState& state);

MethodStatus
CanEnterBaselineAtBranch(JSContext* cx, InterpreterFrame* fp);

JitExecStatus
EnterBaselineAtBranch(JSContext* cx, InterpreterFrame* fp, jsbytecode* pc);

void
FinishDiscardBaselineScript(FreeOp* fop, JSScript* script);

void
AddSizeOfBaselineData(JSScript* script, mozilla::MallocSizeOf mallocSizeOf, size_t* data,
                      size_t* fallbackStubs);

void
ToggleBaselineProfiling(JSRuntime* runtime, bool enable);

void
ToggleBaselineTraceLoggerScripts(JSRuntime* runtime, bool enable);
void
ToggleBaselineTraceLoggerEngine(JSRuntime* runtime, bool enable);

struct BaselineBailoutInfo
{
    // Pointer into the current C stack, where overwriting will start.
    uint8_t* incomingStack;

    // The top and bottom heapspace addresses of the reconstructed stack
    // which will be copied to the bottom.
    uint8_t* copyStackTop;
    uint8_t* copyStackBottom;

    // Fields to store the top-of-stack baseline values that are held
    // in registers.  The setR0 and setR1 fields are flags indicating
    // whether each one is initialized.
    uint32_t setR0;
    Value valueR0;
    uint32_t setR1;
    Value valueR1;

    // The value of the frame pointer register on resume.
    void* resumeFramePtr;

    // The native code address to resume into.
    void* resumeAddr;

    // The bytecode pc where we will resume.
    jsbytecode* resumePC;

    // The bytecode pc of try block and fault block.
    jsbytecode* tryPC;
    jsbytecode* faultPC;

    // If resuming into a TypeMonitor IC chain, this field holds the
    // address of the first stub in that chain.  If this field is
    // set, then the actual jitcode resumed into is the jitcode for
    // the first stub, not the resumeAddr above.  The resumeAddr
    // above, in this case, is pushed onto the stack so that the
    // TypeMonitor chain can tail-return into the main jitcode when done.
    ICStub* monitorStub;

    // Number of baseline frames to push on the stack.
    uint32_t numFrames;

    // If Ion bailed out on a global script before it could perform the global
    // declaration conflicts check. In such cases the baseline script is
    // resumed at the first pc instead of the prologue, so an extra flag is
    // needed to perform the check.
    bool checkGlobalDeclarationConflicts;

    // The bailout kind.
    BailoutKind bailoutKind;
};

uint32_t
BailoutIonToBaseline(JSContext* cx, JitActivation* activation, const JSJitFrameIter& iter,
                     bool invalidate, BaselineBailoutInfo** bailoutInfo,
                     const ExceptionBailoutInfo* exceptionInfo);

// Mark baseline scripts on the stack as active, so that they are not discarded
// during GC.
void
MarkActiveBaselineScripts(Zone* zone);

MethodStatus
BaselineCompile(JSContext* cx, JSScript* script, bool forceDebugInstrumentation = false);

static const unsigned BASELINE_MAX_ARGS_LENGTH = 20000;

} // namespace jit
} // namespace js

namespace JS {

template <>
struct DeletePolicy<js::jit::BaselineScript>
{
    explicit DeletePolicy(JSRuntime* rt) : rt_(rt) {}
    void operator()(const js::jit::BaselineScript* script);

  private:
    JSRuntime* rt_;
};

} // namespace JS

#endif /* jit_BaselineJIT_h */
