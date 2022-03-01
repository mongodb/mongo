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
#include "jit/IonCompileTask.h"
#include "jit/IonTypes.h"
#include "jit/JitCode.h"
#include "jit/shared/Assembler-shared.h"
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

class AutoAccessAtomsZone;
class AutoLockHelperThreadState;
class GCMarker;

namespace jit {

class FrameSizeClass;
class JitRealm;
class Label;
class MacroAssembler;
struct VMFunctionData;

enum class TailCallVMFunctionId;
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
      mozilla::EnumeratedArray<BaselineICFallbackKind,
                               BaselineICFallbackKind::Count, uint32_t>;
  OffsetArray offsets_ = {};

  // Keep track of offset into various baseline stubs' code at return
  // point from called script.
  using BailoutReturnArray =
      mozilla::EnumeratedArray<BailoutReturnKind, BailoutReturnKind::Count,
                               uint32_t>;
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

using EnterJitCode = void (*)(void*, unsigned int, Value*, InterpreterFrame*,
                              CalleeToken, JSObject*, size_t, Value*);

class JitcodeGlobalTable;

class JitRuntime {
 private:
  friend class JitRealm;

  MainThreadData<uint64_t> nextCompilationId_{0};

  // Buffer for OSR from baseline to Ion. To avoid holding on to this for too
  // long it's also freed in EnterBaseline and EnterJit (after returning from
  // JIT code).
  MainThreadData<js::UniquePtr<uint8_t>> ionOsrTempData_{nullptr};

  // Shared exception-handler tail.
  WriteOnceData<uint32_t> exceptionTailOffset_{0};

  // Shared post-bailout-handler tail.
  WriteOnceData<uint32_t> bailoutTailOffset_{0};

  // Shared profiler exit frame tail.
  WriteOnceData<uint32_t> profilerExitFrameTailOffset_{0};

  // Trampoline for entering JIT code.
  WriteOnceData<uint32_t> enterJITOffset_{0};

  // Vector mapping frame class sizes to bailout tables.
  struct BailoutTable {
    uint32_t startOffset;
    uint32_t size;
    BailoutTable(uint32_t startOffset, uint32_t size)
        : startOffset(startOffset), size(size) {}
  };
  typedef Vector<BailoutTable, 4, SystemAllocPolicy> BailoutTableVector;
  WriteOnceData<BailoutTableVector> bailoutTables_;

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

  // Thunk to call malloc/free.
  WriteOnceData<uint32_t> freeStubOffset_{0};

  // Thunk called to finish compilation of an IonScript.
  WriteOnceData<uint32_t> lazyLinkStubOffset_{0};

  // Thunk to enter the interpreter from JIT code.
  WriteOnceData<uint32_t> interpreterStubOffset_{0};

  // Thunk to convert the value in R0 to int32 if it's a double.
  // Note: this stub treats -0 as +0 and may clobber R1.scratchReg().
  WriteOnceData<uint32_t> doubleToInt32ValueStubOffset_{0};

  // Thunk used by the debugger for breakpoint and step mode.
  mozilla::EnumeratedArray<DebugTrapHandlerKind, DebugTrapHandlerKind::Count,
                           WriteOnceData<JitCode*>>
      debugTrapHandlers_;

  // BaselineInterpreter state.
  BaselineInterpreter baselineInterpreter_;

  // Code for trampolines and VMFunction wrappers.
  WriteOnceData<JitCode*> trampolineCode_{nullptr};

  // Maps VMFunctionId to the offset of the wrapper code in trampolineCode_.
  using VMWrapperOffsets = Vector<uint32_t, 0, SystemAllocPolicy>;
  VMWrapperOffsets functionWrapperOffsets_;

  // Maps TailCallVMFunctionId to the offset of the wrapper code in
  // trampolineCode_.
  VMWrapperOffsets tailCallFunctionWrapperOffsets_;

  MainThreadData<BaselineICFallbackCode> baselineICFallbackCode_;

  // Global table of jitcode native address => bytecode address mappings.
  UnprotectedData<JitcodeGlobalTable*> jitcodeGlobalTable_{nullptr};

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
  typedef mozilla::Atomic<size_t, mozilla::SequentiallyConsistent>
      NumFinishedOffThreadTasksType;
  NumFinishedOffThreadTasksType numFinishedOffThreadTasks_{0};

  // List of Ion compilation waiting to get linked.
  using IonCompileTaskList = mozilla::LinkedList<js::jit::IonCompileTask>;
  MainThreadData<IonCompileTaskList> ionLazyLinkList_;
  MainThreadData<size_t> ionLazyLinkListSize_{0};

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
  void generateExceptionTailStub(MacroAssembler& masm, Label* profilerExitTail);
  void generateBailoutTailStub(MacroAssembler& masm, Label* bailoutTail);
  void generateEnterJIT(JSContext* cx, MacroAssembler& masm);
  void generateArgumentsRectifier(MacroAssembler& masm,
                                  ArgumentsRectifierKind kind);
  BailoutTable generateBailoutTable(MacroAssembler& masm, Label* bailoutTail,
                                    uint32_t frameClass);
  void generateBailoutHandler(MacroAssembler& masm, Label* bailoutTail);
  void generateInvalidator(MacroAssembler& masm, Label* bailoutTail);
  uint32_t generatePreBarrier(JSContext* cx, MacroAssembler& masm,
                              MIRType type);
  void generateFreeStub(MacroAssembler& masm);
  JitCode* generateDebugTrapHandler(JSContext* cx, DebugTrapHandlerKind kind);

  bool generateVMWrapper(JSContext* cx, MacroAssembler& masm,
                         const VMFunctionData& f, DynFn nativeFun,
                         uint32_t* wrapperOffset);

  template <typename IdT>
  bool generateVMWrappers(JSContext* cx, MacroAssembler& masm,
                          VMWrapperOffsets& offsets);
  bool generateVMWrappers(JSContext* cx, MacroAssembler& masm);

  bool generateTLEventVM(MacroAssembler& masm, const VMFunctionData& f,
                         bool enter);

  inline bool generateTLEnterVM(MacroAssembler& masm, const VMFunctionData& f) {
    return generateTLEventVM(masm, f, /* enter = */ true);
  }
  inline bool generateTLExitVM(MacroAssembler& masm, const VMFunctionData& f) {
    return generateTLEventVM(masm, f, /* enter = */ false);
  }

  uint32_t startTrampolineCode(MacroAssembler& masm);

  TrampolinePtr trampolineCode(uint32_t offset) const {
    MOZ_ASSERT(offset > 0);
    MOZ_ASSERT(offset < trampolineCode_->instructionsSize());
    return TrampolinePtr(trampolineCode_->raw() + offset);
  }

 public:
  JitRuntime() = default;
  ~JitRuntime();
  [[nodiscard]] bool initialize(JSContext* cx);

  static void TraceAtomZoneRoots(JSTracer* trc,
                                 const js::AutoAccessAtomsZone& access);
  [[nodiscard]] static bool MarkJitcodeGlobalTableIteratively(GCMarker* marker);
  static void TraceWeakJitcodeGlobalTable(JSRuntime* rt, JSTracer* trc);

  const BaselineICFallbackCode& baselineICFallbackCode() const {
    return baselineICFallbackCode_.ref();
  }

  IonCompilationId nextCompilationId() {
    return IonCompilationId(nextCompilationId_++);
  }

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
  TrampolinePtr getVMWrapper(TailCallVMFunctionId funId) const {
    MOZ_ASSERT(trampolineCode_);
    return trampolineCode(tailCallFunctionWrapperOffsets_[size_t(funId)]);
  }

  JitCode* debugTrapHandler(JSContext* cx, DebugTrapHandlerKind kind);

  BaselineInterpreter& baselineInterpreter() { return baselineInterpreter_; }

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

  TrampolinePtr getArgumentsRectifier(
      ArgumentsRectifierKind kind = ArgumentsRectifierKind::Normal) const {
    if (kind == ArgumentsRectifierKind::TrialInlining) {
      return trampolineCode(trialInliningArgumentsRectifierOffset_);
    }
    return trampolineCode(argumentsRectifierOffset_);
  }

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
      default:
        MOZ_CRASH();
    }
  }

  TrampolinePtr freeStub() const { return trampolineCode(freeStubOffset_); }

  TrampolinePtr lazyLinkStub() const {
    return trampolineCode(lazyLinkStubOffset_);
  }
  TrampolinePtr interpreterStub() const {
    return trampolineCode(interpreterStubOffset_);
  }

  TrampolinePtr getDoubleToInt32ValueStub() const {
    return trampolineCode(doubleToInt32ValueStubOffset_);
  }

  bool hasJitcodeGlobalTable() const { return jitcodeGlobalTable_ != nullptr; }

  JitcodeGlobalTable* getJitcodeGlobalTable() {
    MOZ_ASSERT(hasJitcodeGlobalTable());
    return jitcodeGlobalTable_;
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
