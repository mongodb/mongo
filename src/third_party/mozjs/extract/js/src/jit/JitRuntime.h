/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitRuntime_h
#define jit_JitRuntime_h

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/LinkedList.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "jit/ABIFunctions.h"
#include "jit/BaselineICList.h"
#include "jit/BaselineJIT.h"
#include "jit/CalleeToken.h"
#include "jit/InterpreterEntryTrampoline.h"
#include "jit/IonCompileTask.h"
#include "jit/IonTypes.h"
#include "jit/JitCode.h"
#include "jit/JitHints.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/TrampolineNatives.h"
#include "js/AllocPolicy.h"
#include "js/ProfilingFrameIterator.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "threading/ProtectedData.h"
#include "vm/GeckoProfiler.h"
#include "vm/Runtime.h"

class JS_PUBLIC_API JSTracer;

namespace js {

class AutoLockHelperThreadState;
class GCMarker;
enum class ArraySortKind;

namespace jit {

class FrameSizeClass;
class Label;
class MacroAssembler;
struct VMFunctionData;

enum class VMFunctionId;

enum class BaselineICFallbackKind : uint8_t {
#define DEF_ENUM_KIND(kind) kind,
  IC_BASELINE_FALLBACK_CODE_KIND_LIST(DEF_ENUM_KIND)
#undef DEF_ENUM_KIND
      Count
};

enum class BailoutReturnKind {
  GetProp,
  GetPropSuper,
  SetProp,
  GetElem,
  GetElemSuper,
  Call,
  New,
  Count
};

// Class storing code and offsets for all Baseline IC fallback trampolines. This
// is stored in JitRuntime and generated when creating the JitRuntime.
class BaselineICFallbackCode {
  JitCode* code_ = nullptr;
  using OffsetArray =
      mozilla::EnumeratedArray<BaselineICFallbackKind, uint32_t,
                               size_t(BaselineICFallbackKind::Count)>;
  OffsetArray offsets_ = {};

  // Keep track of offset into various baseline stubs' code at return
  // point from called script.
  using BailoutReturnArray =
      mozilla::EnumeratedArray<BailoutReturnKind, uint32_t,
                               size_t(BailoutReturnKind::Count)>;
  BailoutReturnArray bailoutReturnOffsets_ = {};

 public:
  BaselineICFallbackCode() = default;
  BaselineICFallbackCode(const BaselineICFallbackCode&) = delete;
  void operator=(const BaselineICFallbackCode&) = delete;

  void initOffset(BaselineICFallbackKind kind, uint32_t offset) {
    offsets_[kind] = offset;
  }
  void initCode(JitCode* code) { code_ = code; }
  void initBailoutReturnOffset(BailoutReturnKind kind, uint32_t offset) {
    bailoutReturnOffsets_[kind] = offset;
  }
  TrampolinePtr addr(BaselineICFallbackKind kind) const {
    return TrampolinePtr(code_->raw() + offsets_[kind]);
  }
  uint8_t* bailoutReturnAddr(BailoutReturnKind kind) const {
    return code_->raw() + bailoutReturnOffsets_[kind];
  }
};

enum class ArgumentsRectifierKind { Normal, TrialInlining };

enum class DebugTrapHandlerKind { Interpreter, Compiler, Count };

enum class IonGenericCallKind { Call, Construct, Count };

using EnterJitCode = void (*)(void*, unsigned int, Value*, InterpreterFrame*,
                              CalleeToken, JSObject*, size_t, Value*);

class JitcodeGlobalTable;
class PerfSpewerRangeRecorder;

class JitRuntime {
 private:
  MainThreadData<uint64_t> nextCompilationId_{0};

  // Buffer for OSR from baseline to Ion. To avoid holding on to this for too
  // long it's also freed in EnterBaseline and EnterJit (after returning from
  // JIT code).
  MainThreadData<js::UniquePtr<uint8_t>> ionOsrTempData_{nullptr};
  MainThreadData<uint32_t> ionOsrTempDataSize_{0};

  // List of Ion compile tasks that should be freed. Used to batch multiple
  // tasks into a single IonFreeTask.
  MainThreadData<IonFreeCompileTasks> ionFreeTaskBatch_;

  // Shared exception-handler tail.
  WriteOnceData<uint32_t> exceptionTailOffset_{0};
  WriteOnceData<uint32_t> exceptionTailReturnValueCheckOffset_{0};

  // Shared profiler exit frame tail.
  WriteOnceData<uint32_t> profilerExitFrameTailOffset_{0};

  // Trampoline for entering JIT code.
  WriteOnceData<uint32_t> enterJITOffset_{0};

  // Generic bailout table; used if the bailout table overflows.
  WriteOnceData<uint32_t> bailoutHandlerOffset_{0};

  // Argument-rectifying thunks, in the case of insufficient arguments passed
  // to a function call site. The return offset is used to rebuild stack frames
  // when bailing out.
  WriteOnceData<uint32_t> argumentsRectifierOffset_{0};
  WriteOnceData<uint32_t> trialInliningArgumentsRectifierOffset_{0};
  WriteOnceData<uint32_t> argumentsRectifierReturnOffset_{0};

  // Thunk that invalides an (Ion compiled) caller on the Ion stack.
  WriteOnceData<uint32_t> invalidatorOffset_{0};

  // Thunk that calls the GC pre barrier.
  WriteOnceData<uint32_t> valuePreBarrierOffset_{0};
  WriteOnceData<uint32_t> stringPreBarrierOffset_{0};
  WriteOnceData<uint32_t> objectPreBarrierOffset_{0};
  WriteOnceData<uint32_t> shapePreBarrierOffset_{0};
  WriteOnceData<uint32_t> wasmAnyRefPreBarrierOffset_{0};

  // Thunk called to finish compilation of an IonScript.
  WriteOnceData<uint32_t> lazyLinkStubOffset_{0};

  // Thunk to enter the interpreter from JIT code.
  WriteOnceData<uint32_t> interpreterStubOffset_{0};

  // Thunk to convert the value in R0 to int32 if it's a double.
  // Note: this stub treats -0 as +0 and may clobber R1.scratchReg().
  WriteOnceData<uint32_t> doubleToInt32ValueStubOffset_{0};

  // Thunk to do a generic call from Ion.
  mozilla::EnumeratedArray<IonGenericCallKind, WriteOnceData<uint32_t>,
                           size_t(IonGenericCallKind::Count)>
      ionGenericCallStubOffset_;

  // Thunk used by the debugger for breakpoint and step mode.
  mozilla::EnumeratedArray<DebugTrapHandlerKind, WriteOnceData<JitCode*>,
                           size_t(DebugTrapHandlerKind::Count)>
      debugTrapHandlers_;

  // BaselineInterpreter state.
  BaselineInterpreter baselineInterpreter_;

  // Code for trampolines and VMFunction wrappers.
  WriteOnceData<JitCode*> trampolineCode_{nullptr};

  // Thunk that calls into the C++ interpreter from the interpreter
  // entry trampoline that is generated with --emit-interpreter-entry
  WriteOnceData<uint32_t> vmInterpreterEntryOffset_{0};

  // Maps VMFunctionId to the offset of the wrapper code in trampolineCode_.
  using VMWrapperOffsets = Vector<uint32_t, 0, SystemAllocPolicy>;
  VMWrapperOffsets functionWrapperOffsets_;

  MainThreadData<BaselineICFallbackCode> baselineICFallbackCode_;

  // Global table of jitcode native address => bytecode address mappings.
  UnprotectedData<JitcodeGlobalTable*> jitcodeGlobalTable_{nullptr};

  // Map that stores Jit Hints for each script.
  MainThreadData<JitHintsMap*> jitHintsMap_{nullptr};

  // Map used to collect entry trampolines for the Interpreters which is used
  // for external profiling to identify which functions are being interpreted.
  MainThreadData<EntryTrampolineMap*> interpreterEntryMap_{nullptr};

#ifdef DEBUG
  // The number of possible bailing places encountered before forcefully bailing
  // in that place if the counter reaches zero. Note that zero also means
  // inactive.
  MainThreadData<uint32_t> ionBailAfterCounter_{0};

  // Whether the bailAfter mechanism is enabled. Used to avoid generating the
  // Ion code instrumentation for ionBailAfterCounter_ if the testing function
  // isn't used.
  MainThreadData<bool> ionBailAfterEnabled_{false};
#endif

  // Number of Ion compilations which were finished off thread and are
  // waiting to be lazily linked. This is only set while holding the helper
  // thread state lock, but may be read from at other times.
  using NumFinishedOffThreadTasksType =
      mozilla::Atomic<size_t, mozilla::SequentiallyConsistent>;
  NumFinishedOffThreadTasksType numFinishedOffThreadTasks_{0};

  // List of Ion compilation waiting to get linked.
  using IonCompileTaskList = mozilla::LinkedList<js::jit::IonCompileTask>;
  MainThreadData<IonCompileTaskList> ionLazyLinkList_;
  MainThreadData<size_t> ionLazyLinkListSize_{0};

  // Pointer to trampoline code for each TrampolineNative. The JSFunction has
  // a JitEntry pointer that points to an item in this array.
  using TrampolineNativeJitEntryArray =
      mozilla::EnumeratedArray<TrampolineNative, void*,
                               size_t(TrampolineNative::Count)>;
  TrampolineNativeJitEntryArray trampolineNativeJitEntries_{};

#ifdef DEBUG
  // Flag that can be set from JIT code to indicate it's invalid to call
  // arbitrary JS code in a particular region. This is checked in RunScript.
  MainThreadData<uint32_t> disallowArbitraryCode_{false};
#endif

  bool generateTrampolines(JSContext* cx);
  bool generateBaselineICFallbackCode(JSContext* cx);

  void generateLazyLinkStub(MacroAssembler& masm);
  void generateInterpreterStub(MacroAssembler& masm);
  void generateDoubleToInt32ValueStub(MacroAssembler& masm);
  void generateProfilerExitFrameTailStub(MacroAssembler& masm,
                                         Label* profilerExitTail);
  void generateExceptionTailStub(MacroAssembler& masm, Label* profilerExitTail,
                                 Label* bailoutTail);
  void generateBailoutTailStub(MacroAssembler& masm, Label* bailoutTail);
  void generateEnterJIT(JSContext* cx, MacroAssembler& masm);
  void generateArgumentsRectifier(MacroAssembler& masm,
                                  ArgumentsRectifierKind kind);
  void generateBailoutHandler(MacroAssembler& masm, Label* bailoutTail);
  void generateInvalidator(MacroAssembler& masm, Label* bailoutTail);
  uint32_t generatePreBarrier(JSContext* cx, MacroAssembler& masm,
                              MIRType type);
  void generateIonGenericCallStub(MacroAssembler& masm,
                                  IonGenericCallKind kind);

  // Helper functions for generateIonGenericCallStub
  void generateIonGenericCallBoundFunction(MacroAssembler& masm, Label* entry,
                                           Label* vmCall);
  void generateIonGenericCallNativeFunction(MacroAssembler& masm,
                                            bool isConstructing);
  void generateIonGenericCallFunCall(MacroAssembler& masm, Label* entry,
                                     Label* vmCall);
  void generateIonGenericCallArgumentsShift(MacroAssembler& masm, Register argc,
                                            Register curr, Register end,
                                            Register scratch, Label* done);

  JitCode* generateDebugTrapHandler(JSContext* cx, DebugTrapHandlerKind kind);

  bool generateVMWrapper(JSContext* cx, MacroAssembler& masm, VMFunctionId id,
                         const VMFunctionData& f, DynFn nativeFun,
                         uint32_t* wrapperOffset);

  bool generateVMWrappers(JSContext* cx, MacroAssembler& masm,
                          PerfSpewerRangeRecorder& rangeRecorder);

  uint32_t startTrampolineCode(MacroAssembler& masm);

  TrampolinePtr trampolineCode(uint32_t offset) const {
    MOZ_ASSERT(offset > 0);
    MOZ_ASSERT(offset < trampolineCode_->instructionsSize());
    return TrampolinePtr(trampolineCode_->raw() + offset);
  }

  void generateBaselineInterpreterEntryTrampoline(MacroAssembler& masm);
  void generateInterpreterEntryTrampoline(MacroAssembler& masm);

  using TrampolineNativeJitEntryOffsets =
      mozilla::EnumeratedArray<TrampolineNative, uint32_t,
                               size_t(TrampolineNative::Count)>;
  void generateTrampolineNatives(MacroAssembler& masm,
                                 TrampolineNativeJitEntryOffsets& offsets,
                                 PerfSpewerRangeRecorder& rangeRecorder);
  uint32_t generateArraySortTrampoline(MacroAssembler& masm,
                                       ArraySortKind kind);

  void bindLabelToOffset(Label* label, uint32_t offset) {
    MOZ_ASSERT(!trampolineCode_);
    label->bind(offset);
  }

 public:
  JitCode* generateEntryTrampolineForScript(JSContext* cx, JSScript* script);

  JitRuntime() = default;
  ~JitRuntime();
  [[nodiscard]] bool initialize(JSContext* cx);

  static void TraceAtomZoneRoots(JSTracer* trc);
  [[nodiscard]] static bool MarkJitcodeGlobalTableIteratively(GCMarker* marker);
  static void TraceWeakJitcodeGlobalTable(JSRuntime* rt, JSTracer* trc);

  const BaselineICFallbackCode& baselineICFallbackCode() const {
    return baselineICFallbackCode_.ref();
  }

  IonCompilationId nextCompilationId() {
    return IonCompilationId(nextCompilationId_++);
  }

  [[nodiscard]] bool addIonCompileToFreeTaskBatch(IonCompileTask* task) {
    return ionFreeTaskBatch_.ref().append(task);
  }
  void maybeStartIonFreeTask(bool force);

  UniquePtr<LifoAlloc> tryReuseIonLifoAlloc();

#ifdef DEBUG
  bool disallowArbitraryCode() const { return disallowArbitraryCode_; }
  void clearDisallowArbitraryCode() { disallowArbitraryCode_ = false; }
  const void* addressOfDisallowArbitraryCode() const {
    return &disallowArbitraryCode_.refNoCheck();
  }
#endif

  uint8_t* allocateIonOsrTempData(size_t size);
  void freeIonOsrTempData();

  TrampolinePtr getVMWrapper(VMFunctionId funId) const {
    MOZ_ASSERT(trampolineCode_);
    return trampolineCode(functionWrapperOffsets_[size_t(funId)]);
  }

  bool ensureDebugTrapHandler(JSContext* cx, DebugTrapHandlerKind kind);
  JitCode* debugTrapHandler(DebugTrapHandlerKind kind) const {
    MOZ_ASSERT(debugTrapHandlers_[kind]);
    return debugTrapHandlers_[kind];
  }

  BaselineInterpreter& baselineInterpreter() { return baselineInterpreter_; }
  const BaselineInterpreter& baselineInterpreter() const {
    return baselineInterpreter_;
  }

  TrampolinePtr getGenericBailoutHandler() const {
    return trampolineCode(bailoutHandlerOffset_);
  }

  TrampolinePtr getExceptionTail() const {
    return trampolineCode(exceptionTailOffset_);
  }
  TrampolinePtr getExceptionTailReturnValueCheck() const {
    return trampolineCode(exceptionTailReturnValueCheckOffset_);
  }

  TrampolinePtr getProfilerExitFrameTail() const {
    return trampolineCode(profilerExitFrameTailOffset_);
  }

  TrampolinePtr getArgumentsRectifier(
      ArgumentsRectifierKind kind = ArgumentsRectifierKind::Normal) const {
    if (kind == ArgumentsRectifierKind::TrialInlining) {
      return trampolineCode(trialInliningArgumentsRectifierOffset_);
    }
    return trampolineCode(argumentsRectifierOffset_);
  }

  uint32_t vmInterpreterEntryOffset() { return vmInterpreterEntryOffset_; }

  TrampolinePtr getArgumentsRectifierReturnAddr() const {
    return trampolineCode(argumentsRectifierReturnOffset_);
  }

  TrampolinePtr getInvalidationThunk() const {
    return trampolineCode(invalidatorOffset_);
  }

  EnterJitCode enterJit() const {
    return JS_DATA_TO_FUNC_PTR(EnterJitCode,
                               trampolineCode(enterJITOffset_).value);
  }

  // Return the registers from the native caller frame of the given JIT frame.
  // Nothing{} if frameStackAddress is NOT pointing at a native-to-JIT entry
  // frame, or if the information is not accessible/implemented on this
  // platform.
  static mozilla::Maybe<::JS::ProfilingFrameIterator::RegisterState>
  getCppEntryRegisters(JitFrameLayout* frameStackAddress);

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
      case MIRType::WasmAnyRef:
        return trampolineCode(wasmAnyRefPreBarrierOffset_);
      default:
        MOZ_CRASH();
    }
  }

  TrampolinePtr lazyLinkStub() const {
    return trampolineCode(lazyLinkStubOffset_);
  }
  TrampolinePtr interpreterStub() const {
    return trampolineCode(interpreterStubOffset_);
  }

  TrampolinePtr getDoubleToInt32ValueStub() const {
    return trampolineCode(doubleToInt32ValueStubOffset_);
  }

  TrampolinePtr getIonGenericCallStub(IonGenericCallKind kind) const {
    return trampolineCode(ionGenericCallStubOffset_[kind]);
  }

  void** trampolineNativeJitEntry(TrampolineNative native) {
    void** jitEntry = &trampolineNativeJitEntries_[native];
    MOZ_ASSERT(*jitEntry >= trampolineCode_->raw());
    MOZ_ASSERT(*jitEntry <
               trampolineCode_->raw() + trampolineCode_->instructionsSize());
    return jitEntry;
  }
  TrampolineNative trampolineNativeForJitEntry(void** entry) {
    MOZ_RELEASE_ASSERT(entry >= trampolineNativeJitEntries_.begin());
    size_t index = entry - trampolineNativeJitEntries_.begin();
    MOZ_RELEASE_ASSERT(index < size_t(TrampolineNative::Count));
    return TrampolineNative(index);
  }

  bool hasJitcodeGlobalTable() const { return jitcodeGlobalTable_ != nullptr; }

  JitcodeGlobalTable* getJitcodeGlobalTable() {
    MOZ_ASSERT(hasJitcodeGlobalTable());
    return jitcodeGlobalTable_;
  }

  bool hasJitHintsMap() const { return jitHintsMap_ != nullptr; }

  JitHintsMap* getJitHintsMap() {
    MOZ_ASSERT(hasJitHintsMap());
    return jitHintsMap_;
  }

  bool hasInterpreterEntryMap() const {
    return interpreterEntryMap_ != nullptr;
  }

  EntryTrampolineMap* getInterpreterEntryMap() {
    MOZ_ASSERT(hasInterpreterEntryMap());
    return interpreterEntryMap_;
  }

  bool isProfilerInstrumentationEnabled(JSRuntime* rt) {
    return rt->geckoProfiler().enabled();
  }

  bool isOptimizationTrackingEnabled(JSRuntime* rt) {
    return isProfilerInstrumentationEnabled(rt);
  }

#ifdef DEBUG
  void* addressOfIonBailAfterCounter() { return &ionBailAfterCounter_; }

  // Set after how many bailing places we should forcefully bail.
  // Zero disables this feature.
  void setIonBailAfterCounter(uint32_t after) { ionBailAfterCounter_ = after; }
  bool ionBailAfterEnabled() const { return ionBailAfterEnabled_; }
  void setIonBailAfterEnabled(bool enabled) { ionBailAfterEnabled_ = enabled; }
#endif

  size_t numFinishedOffThreadTasks() const {
    return numFinishedOffThreadTasks_;
  }
  NumFinishedOffThreadTasksType& numFinishedOffThreadTasksRef(
      const AutoLockHelperThreadState& locked) {
    return numFinishedOffThreadTasks_;
  }

  IonCompileTaskList& ionLazyLinkList(JSRuntime* rt);

  size_t ionLazyLinkListSize() const { return ionLazyLinkListSize_; }

  void ionLazyLinkListRemove(JSRuntime* rt, js::jit::IonCompileTask* task);
  void ionLazyLinkListAdd(JSRuntime* rt, js::jit::IonCompileTask* task);
};

}  // namespace jit
}  // namespace js

#endif /* jit_JitRuntime_h */
