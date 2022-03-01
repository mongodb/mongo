/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCodeGen_h
#define jit_BaselineCodeGen_h

#include "jit/BaselineFrameInfo.h"
#include "jit/BaselineIC.h"
#include "jit/BytecodeAnalysis.h"
#include "jit/FixedList.h"
#include "jit/MacroAssembler.h"
#include "vm/GeneratorResumeKind.h"  // GeneratorResumeKind

namespace js {

namespace jit {

enum class ScriptGCThingType { Atom, RegExp, Object, Function, Scope, BigInt };

// Base class for BaselineCompiler and BaselineInterpreterGenerator. The Handler
// template is a class storing fields/methods that are interpreter or compiler
// specific. This can be combined with template specialization of methods in
// this class to specialize behavior.
template <typename Handler>
class BaselineCodeGen {
 protected:
  Handler handler;

  JSContext* cx;
  StackMacroAssembler masm;

  typename Handler::FrameInfoT& frame;

  js::Vector<CodeOffset> traceLoggerToggleOffsets_;

  // Shared epilogue code to return to the caller.
  NonAssertingLabel return_;

  NonAssertingLabel postBarrierSlot_;

  // Prologue code where we resume for Ion prologue bailouts.
  NonAssertingLabel bailoutPrologue_;

  CodeOffset profilerEnterFrameToggleOffset_;
  CodeOffset profilerExitFrameToggleOffset_;

  // Early Ion bailouts will enter at this address. This is after frame
  // construction and before environment chain is initialized.
  CodeOffset bailoutPrologueOffset_;

  // Baseline Interpreter can enter Baseline Compiler code at this address. This
  // is right after the warm-up counter check in the prologue.
  CodeOffset warmUpCheckPrologueOffset_;

  uint32_t pushedBeforeCall_ = 0;
#ifdef DEBUG
  bool inCall_ = false;
#endif

  template <typename... HandlerArgs>
  explicit BaselineCodeGen(JSContext* cx, HandlerArgs&&... args);

  template <typename T>
  void pushArg(const T& t) {
    masm.Push(t);
  }

  // Pushes the current script as argument for a VM function.
  void pushScriptArg();

  // Pushes the bytecode pc as argument for a VM function.
  void pushBytecodePCArg();

  // Pushes a name/object/scope associated with the current bytecode op (and
  // stored in the script) as argument for a VM function.
  void loadScriptGCThing(ScriptGCThingType type, Register dest,
                         Register scratch);
  void pushScriptGCThingArg(ScriptGCThingType type, Register scratch1,
                            Register scratch2);
  void pushScriptNameArg(Register scratch1, Register scratch2);

  // Pushes a bytecode operand as argument for a VM function.
  void pushUint8BytecodeOperandArg(Register scratch);
  void pushUint16BytecodeOperandArg(Register scratch);

  void loadInt32LengthBytecodeOperand(Register dest);
  void loadNumFormalArguments(Register dest);

  // Loads the current JSScript* in dest.
  void loadScript(Register dest);

  void saveInterpreterPCReg();
  void restoreInterpreterPCReg();

  // Subtracts |script->nslots() * sizeof(Value)| from reg.
  void subtractScriptSlotsSize(Register reg, Register scratch);

  // Jump to the script's resume entry indicated by resumeIndex.
  void jumpToResumeEntry(Register resumeIndex, Register scratch1,
                         Register scratch2);

  // Load the global's lexical environment.
  void loadGlobalLexicalEnvironment(Register dest);
  void pushGlobalLexicalEnvironmentValue(ValueOperand scratch);

  // Load the |this|-value from the global's lexical environment.
  void loadGlobalThisValue(ValueOperand dest);

  // Computes the frame size. See BaselineFrame::debugFrameSize_.
  void computeFrameSize(Register dest);

  void prepareVMCall();

  void storeFrameSizeAndPushDescriptor(uint32_t argSize, Register scratch1,
                                       Register scratch2);

  enum class CallVMPhase { BeforePushingLocals, AfterPushingLocals };
  bool callVMInternal(VMFunctionId id, RetAddrEntry::Kind kind,
                      CallVMPhase phase);

  template <typename Fn, Fn fn>
  bool callVM(RetAddrEntry::Kind kind = RetAddrEntry::Kind::CallVM,
              CallVMPhase phase = CallVMPhase::AfterPushingLocals);

  template <typename Fn, Fn fn>
  bool callVMNonOp(CallVMPhase phase = CallVMPhase::AfterPushingLocals) {
    return callVM<Fn, fn>(RetAddrEntry::Kind::NonOpCallVM, phase);
  }

  // ifDebuggee should be a function emitting code for when the script is a
  // debuggee script. ifNotDebuggee (if present) is called to emit code for
  // non-debuggee scripts.
  template <typename F1, typename F2>
  [[nodiscard]] bool emitDebugInstrumentation(
      const F1& ifDebuggee, const mozilla::Maybe<F2>& ifNotDebuggee);
  template <typename F>
  [[nodiscard]] bool emitDebugInstrumentation(const F& ifDebuggee) {
    return emitDebugInstrumentation(ifDebuggee, mozilla::Maybe<F>());
  }

  bool emitSuspend(JSOp op);

  template <typename F>
  [[nodiscard]] bool emitAfterYieldDebugInstrumentation(const F& ifDebuggee,
                                                        Register scratch);

  // ifSet should be a function emitting code for when the script has |flag|
  // set. ifNotSet emits code for when the flag isn't set.
  template <typename F1, typename F2>
  [[nodiscard]] bool emitTestScriptFlag(JSScript::ImmutableFlags flag,
                                        const F1& ifSet, const F2& ifNotSet,
                                        Register scratch);

  // If |script->hasFlag(flag) == value|, execute the code emitted by |emit|.
  template <typename F>
  [[nodiscard]] bool emitTestScriptFlag(JSScript::ImmutableFlags flag,
                                        bool value, const F& emit,
                                        Register scratch);
  template <typename F>
  [[nodiscard]] bool emitTestScriptFlag(JSScript::MutableFlags flag, bool value,
                                        const F& emit, Register scratch);

  [[nodiscard]] bool emitEnterGeneratorCode(Register script,
                                            Register resumeIndex,
                                            Register scratch);

  void emitInterpJumpToResumeEntry(Register script, Register resumeIndex,
                                   Register scratch);
  void emitJumpToInterpretOpLabel();

  [[nodiscard]] bool emitCheckThis(ValueOperand val, bool reinit = false);
  void emitLoadReturnValue(ValueOperand val);
  void emitPushNonArrowFunctionNewTarget();
  void emitGetAliasedVar(ValueOperand dest);
  [[nodiscard]] bool emitGetAliasedDebugVar(ValueOperand dest);

  [[nodiscard]] bool emitNextIC();
  [[nodiscard]] bool emitInterruptCheck();
  [[nodiscard]] bool emitWarmUpCounterIncrement();
  [[nodiscard]] bool emitTraceLoggerResume(Register script,
                                           AllocatableGeneralRegisterSet& regs);

#define EMIT_OP(op, ...) bool emit_##op();
  FOR_EACH_OPCODE(EMIT_OP)
#undef EMIT_OP

  // JSOp::Pos, JSOp::Neg, JSOp::BitNot, JSOp::Inc, JSOp::Dec, JSOp::ToNumeric.
  [[nodiscard]] bool emitUnaryArith();

  // JSOp::BitXor, JSOp::Lsh, JSOp::Add etc.
  [[nodiscard]] bool emitBinaryArith();

  // Handles JSOp::Lt, JSOp::Gt, and friends
  [[nodiscard]] bool emitCompare();

  // Handles JSOp::NewObject and JSOp::NewInit.
  [[nodiscard]] bool emitNewObject();

  // For a JOF_JUMP op, jumps to the op's jump target.
  void emitJump();

  // For a JOF_JUMP op, jumps to the op's jump target depending on the Value
  // in |val|.
  void emitTestBooleanTruthy(bool branchIfTrue, ValueOperand val);

  // Converts |val| to an index in the jump table and stores this in |dest|
  // or branches to the default pc if not int32 or out-of-range.
  void emitGetTableSwitchIndex(ValueOperand val, Register dest,
                               Register scratch1, Register scratch2);

  // Jumps to the target of a table switch based on |key| and the
  // firstResumeIndex stored in JSOp::TableSwitch.
  void emitTableSwitchJump(Register key, Register scratch1, Register scratch2);

  [[nodiscard]] bool emitReturn();

  [[nodiscard]] bool emitTest(bool branchIfTrue);
  [[nodiscard]] bool emitAndOr(bool branchIfTrue);
  [[nodiscard]] bool emitCoalesce();

  [[nodiscard]] bool emitCall(JSOp op);
  [[nodiscard]] bool emitSpreadCall(JSOp op);

  [[nodiscard]] bool emitDelElem(bool strict);
  [[nodiscard]] bool emitDelProp(bool strict);
  [[nodiscard]] bool emitSetElemSuper(bool strict);
  [[nodiscard]] bool emitSetPropSuper(bool strict);

  [[nodiscard]] bool emitBindName(JSOp op);

  // Try to bake in the result of GETGNAME/BINDGNAME instead of using an IC.
  // Return true if we managed to optimize the op.
  bool tryOptimizeGetGlobalName();
  bool tryOptimizeBindGlobalName();

  [[nodiscard]] bool emitInitPropGetterSetter();
  [[nodiscard]] bool emitInitElemGetterSetter();

  [[nodiscard]] bool emitFormalArgAccess(JSOp op);

  [[nodiscard]] bool emitUninitializedLexicalCheck(const ValueOperand& val);

  [[nodiscard]] bool emitIsMagicValue();

  void getEnvironmentCoordinateObject(Register reg);
  Address getEnvironmentCoordinateAddressFromObject(Register objReg,
                                                    Register reg);
  Address getEnvironmentCoordinateAddress(Register reg);

  [[nodiscard]] bool emitPrologue();
  [[nodiscard]] bool emitEpilogue();
  [[nodiscard]] bool emitOutOfLinePostBarrierSlot();
  [[nodiscard]] bool emitStackCheck();
  [[nodiscard]] bool emitDebugPrologue();
  [[nodiscard]] bool emitDebugEpilogue();

  template <typename F>
  [[nodiscard]] bool initEnvironmentChainHelper(const F& initFunctionEnv);
  [[nodiscard]] bool initEnvironmentChain();

  [[nodiscard]] bool emitTraceLoggerEnter();
  [[nodiscard]] bool emitTraceLoggerExit();

  [[nodiscard]] bool emitHandleCodeCoverageAtPrologue();

  void emitInitFrameFields(Register nonFunctionEnv);
  [[nodiscard]] bool emitIsDebuggeeCheck();
  void emitInitializeLocals();

  void emitProfilerEnterFrame();
  void emitProfilerExitFrame();
};

using RetAddrEntryVector = js::Vector<RetAddrEntry, 16, SystemAllocPolicy>;

// Interface used by BaselineCodeGen for BaselineCompiler.
class BaselineCompilerHandler {
  CompilerFrameInfo frame_;
  TempAllocator& alloc_;
  BytecodeAnalysis analysis_;
#ifdef DEBUG
  const MacroAssembler& masm_;
#endif
  FixedList<Label> labels_;
  RetAddrEntryVector retAddrEntries_;

  // Native code offsets for OSR at JSOp::LoopHead ops.
  using OSREntryVector =
      Vector<BaselineScript::OSREntry, 16, SystemAllocPolicy>;
  OSREntryVector osrEntries_;

  JSScript* script_;
  jsbytecode* pc_;

  // Index of the current ICEntry in the script's JitScript.
  uint32_t icEntryIndex_;

  bool compileDebugInstrumentation_;
  bool ionCompileable_;

 public:
  using FrameInfoT = CompilerFrameInfo;

  BaselineCompilerHandler(JSContext* cx, MacroAssembler& masm,
                          TempAllocator& alloc, JSScript* script);

  [[nodiscard]] bool init(JSContext* cx);

  CompilerFrameInfo& frame() { return frame_; }

  jsbytecode* pc() const { return pc_; }
  jsbytecode* maybePC() const { return pc_; }

  void moveToNextPC() { pc_ += GetBytecodeLength(pc_); }
  Label* labelOf(jsbytecode* pc) { return &labels_[script_->pcToOffset(pc)]; }

  bool isDefinitelyLastOp() const { return pc_ == script_->lastPC(); }

  bool shouldEmitDebugEpilogueAtReturnOp() const {
    // The JIT uses the return address -> pc mapping and bakes in the pc
    // argument so the DebugEpilogue call needs to be part of the returning
    // bytecode op for this to work.
    return true;
  }

  JSScript* script() const { return script_; }
  JSScript* maybeScript() const { return script_; }

  JSFunction* function() const { return script_->function(); }
  JSFunction* maybeFunction() const { return function(); }

  ModuleObject* module() const { return script_->module(); }

  void setCompileDebugInstrumentation() { compileDebugInstrumentation_ = true; }
  bool compileDebugInstrumentation() const {
    return compileDebugInstrumentation_;
  }

  bool maybeIonCompileable() const { return ionCompileable_; }

  uint32_t icEntryIndex() const { return icEntryIndex_; }
  void moveToNextICEntry() { icEntryIndex_++; }

  BytecodeAnalysis& analysis() { return analysis_; }

  RetAddrEntryVector& retAddrEntries() { return retAddrEntries_; }
  OSREntryVector& osrEntries() { return osrEntries_; }

  [[nodiscard]] bool recordCallRetAddr(JSContext* cx, RetAddrEntry::Kind kind,
                                       uint32_t retOffset);

  // If a script has more |nslots| than this the stack check must account
  // for these slots explicitly.
  bool mustIncludeSlotsInStackCheck() const {
    static constexpr size_t NumSlotsLimit = 128;
    return script()->nslots() > NumSlotsLimit;
  }

  bool canHaveFixedSlots() const { return script()->nfixed() != 0; }
};

using BaselineCompilerCodeGen = BaselineCodeGen<BaselineCompilerHandler>;

class BaselineCompiler final : private BaselineCompilerCodeGen {
  // Native code offsets for bytecode ops in the script's resume offsets list.
  ResumeOffsetEntryVector resumeOffsetEntries_;

  // Native code offsets for debug traps if the script is compiled with debug
  // instrumentation.
  using DebugTrapEntryVector =
      Vector<BaselineScript::DebugTrapEntry, 0, SystemAllocPolicy>;
  DebugTrapEntryVector debugTrapEntries_;

  CodeOffset profilerPushToggleOffset_;

  CodeOffset traceLoggerScriptTextIdOffset_;

 public:
  BaselineCompiler(JSContext* cx, TempAllocator& alloc, JSScript* script);
  [[nodiscard]] bool init();

  MethodStatus compile();

  bool compileDebugInstrumentation() const {
    return handler.compileDebugInstrumentation();
  }
  void setCompileDebugInstrumentation() {
    handler.setCompileDebugInstrumentation();
  }

 private:
  MethodStatus emitBody();

  [[nodiscard]] bool emitDebugTrap();
};

// Interface used by BaselineCodeGen for BaselineInterpreterGenerator.
class BaselineInterpreterHandler {
  InterpreterFrameInfo frame_;

  // Entry point to start interpreting a bytecode op. No registers are live. PC
  // is loaded from the frame.
  NonAssertingLabel interpretOp_;

  // Like interpretOp_ but at this point the PC is expected to be in
  // InterpreterPCReg.
  NonAssertingLabel interpretOpWithPCReg_;

  // Offsets of toggled jumps for debugger instrumentation.
  using CodeOffsetVector = Vector<uint32_t, 0, SystemAllocPolicy>;
  CodeOffsetVector debugInstrumentationOffsets_;

  // Offsets of toggled jumps for code coverage instrumentation.
  CodeOffsetVector codeCoverageOffsets_;
  NonAssertingLabel codeCoverageAtPrologueLabel_;
  NonAssertingLabel codeCoverageAtPCLabel_;

  // Offsets of IC calls for IsIonInlinableOp ops, for Ion bailouts.
  BaselineInterpreter::ICReturnOffsetVector icReturnOffsets_;

  // Offsets of some callVMs for BaselineDebugModeOSR.
  BaselineInterpreter::CallVMOffsets callVMOffsets_;

  // The current JSOp we are emitting interpreter code for.
  mozilla::Maybe<JSOp> currentOp_;

 public:
  using FrameInfoT = InterpreterFrameInfo;

  explicit BaselineInterpreterHandler(JSContext* cx, MacroAssembler& masm);

  InterpreterFrameInfo& frame() { return frame_; }

  Label* interpretOpLabel() { return &interpretOp_; }
  Label* interpretOpWithPCRegLabel() { return &interpretOpWithPCReg_; }

  Label* codeCoverageAtPrologueLabel() { return &codeCoverageAtPrologueLabel_; }
  Label* codeCoverageAtPCLabel() { return &codeCoverageAtPCLabel_; }

  CodeOffsetVector& debugInstrumentationOffsets() {
    return debugInstrumentationOffsets_;
  }
  CodeOffsetVector& codeCoverageOffsets() { return codeCoverageOffsets_; }

  BaselineInterpreter::ICReturnOffsetVector& icReturnOffsets() {
    return icReturnOffsets_;
  }

  void setCurrentOp(JSOp op) { currentOp_.emplace(op); }
  void resetCurrentOp() { currentOp_.reset(); }
  mozilla::Maybe<JSOp> currentOp() const { return currentOp_; }

  // Interpreter doesn't know the script and pc statically.
  jsbytecode* maybePC() const { return nullptr; }
  bool isDefinitelyLastOp() const { return false; }
  JSScript* maybeScript() const { return nullptr; }
  JSFunction* maybeFunction() const { return nullptr; }

  bool shouldEmitDebugEpilogueAtReturnOp() const {
    // The interpreter doesn't use the return address -> pc mapping and doesn't
    // bake in bytecode PCs so it can emit a shared DebugEpilogue call instead
    // of duplicating it for every return op.
    return false;
  }

  [[nodiscard]] bool addDebugInstrumentationOffset(JSContext* cx,
                                                   CodeOffset offset);

  const BaselineInterpreter::CallVMOffsets& callVMOffsets() const {
    return callVMOffsets_;
  }

  [[nodiscard]] bool recordCallRetAddr(JSContext* cx, RetAddrEntry::Kind kind,
                                       uint32_t retOffset);

  bool maybeIonCompileable() const { return true; }

  // The interpreter doesn't know the number of slots statically so we always
  // include them.
  bool mustIncludeSlotsInStackCheck() const { return true; }

  bool canHaveFixedSlots() const { return true; }
};

using BaselineInterpreterCodeGen = BaselineCodeGen<BaselineInterpreterHandler>;

class BaselineInterpreterGenerator final : private BaselineInterpreterCodeGen {
  // Offsets of patchable call instructions for debugger breakpoints/stepping.
  Vector<uint32_t, 0, SystemAllocPolicy> debugTrapOffsets_;

  // Offsets of move instructions for tableswitch base address.
  Vector<CodeOffset, 0, SystemAllocPolicy> tableLabels_;

  // Offset of the first tableswitch entry.
  uint32_t tableOffset_ = 0;

  // Offset of the code to start interpreting a bytecode op.
  uint32_t interpretOpOffset_ = 0;

  // Like interpretOpOffset_ but skips the debug trap for the current op.
  uint32_t interpretOpNoDebugTrapOffset_ = 0;

  // Offset of the jump (tail call) to the debug trap handler trampoline code.
  // When the debugger is enabled, NOPs are patched to calls to this location.
  uint32_t debugTrapHandlerOffset_ = 0;

 public:
  explicit BaselineInterpreterGenerator(JSContext* cx);

  [[nodiscard]] bool generate(BaselineInterpreter& interpreter);

 private:
  [[nodiscard]] bool emitInterpreterLoop();
  [[nodiscard]] bool emitDebugTrap();

  void emitOutOfLineCodeCoverageInstrumentation();
};

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineCodeGen_h */
