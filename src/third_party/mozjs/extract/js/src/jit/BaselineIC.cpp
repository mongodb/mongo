/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineIC.h"

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TemplateLib.h"

#include "jsfriendapi.h"
#include "jslibmath.h"
#include "jstypes.h"

#include "builtin/Eval.h"
#include "gc/Policy.h"
#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIRHealth.h"
#include "jit/InlinableNatives.h"
#include "jit/JitFrames.h"
#include "jit/JitRealm.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/Linker.h"
#include "jit/Lowering.h"
#ifdef JS_ION_PERF
#  include "jit/PerfSpewer.h"
#endif
#include "jit/SharedICHelpers.h"
#include "jit/SharedICRegisters.h"
#include "jit/VMFunctions.h"
#include "js/Conversions.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/GCVector.h"
#include "vm/BytecodeIterator.h"
#include "vm/BytecodeLocation.h"
#include "vm/BytecodeUtil.h"
#include "vm/EqualityOperations.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/Opcodes.h"
#include "vm/SelfHosting.h"
#include "vm/TypedArrayObject.h"
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif

#include "builtin/Boolean-inl.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "jit/SharedICHelpers-inl.h"
#include "jit/VMFunctionList-inl.h"
#include "vm/BytecodeIterator-inl.h"
#include "vm/BytecodeLocation-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/StringObject-inl.h"

using mozilla::DebugOnly;

namespace js {
namespace jit {

// Class used to emit all Baseline IC fallback code when initializing the
// JitRuntime.
class MOZ_RAII FallbackICCodeCompiler final {
  BaselineICFallbackCode& code;
  MacroAssembler& masm;

  JSContext* cx;
  bool inStubFrame_ = false;

#ifdef DEBUG
  bool entersStubFrame_ = false;
  uint32_t framePushedAtEnterStubFrame_ = 0;
#endif

  [[nodiscard]] bool emitCall(bool isSpread, bool isConstructing);
  [[nodiscard]] bool emitGetElem(bool hasReceiver);
  [[nodiscard]] bool emitGetProp(bool hasReceiver);

 public:
  FallbackICCodeCompiler(JSContext* cx, BaselineICFallbackCode& code,
                         MacroAssembler& masm)
      : code(code), masm(masm), cx(cx) {}

#define DEF_METHOD(kind) [[nodiscard]] bool emit_##kind();
  IC_BASELINE_FALLBACK_CODE_KIND_LIST(DEF_METHOD)
#undef DEF_METHOD

  void pushCallArguments(MacroAssembler& masm,
                         AllocatableGeneralRegisterSet regs, Register argcReg,
                         bool isConstructing);

  // Push a payload specialized per compiler needed to execute stubs.
  void PushStubPayload(MacroAssembler& masm, Register scratch);
  void pushStubPayload(MacroAssembler& masm, Register scratch);

  // Emits a tail call to a VMFunction wrapper.
  [[nodiscard]] bool tailCallVMInternal(MacroAssembler& masm,
                                        TailCallVMFunctionId id);

  template <typename Fn, Fn fn>
  [[nodiscard]] bool tailCallVM(MacroAssembler& masm);

  // Emits a normal (non-tail) call to a VMFunction wrapper.
  [[nodiscard]] bool callVMInternal(MacroAssembler& masm, VMFunctionId id);

  template <typename Fn, Fn fn>
  [[nodiscard]] bool callVM(MacroAssembler& masm);

  // A stub frame is used when a stub wants to call into the VM without
  // performing a tail call. This is required for the return address
  // to pc mapping to work.
  void enterStubFrame(MacroAssembler& masm, Register scratch);
  void assumeStubFrame();
  void leaveStubFrame(MacroAssembler& masm, bool calledIntoIon = false);
};

AllocatableGeneralRegisterSet BaselineICAvailableGeneralRegs(size_t numInputs) {
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
#if defined(JS_CODEGEN_ARM)
  MOZ_ASSERT(!regs.has(BaselineStackReg));
  MOZ_ASSERT(!regs.has(ICTailCallReg));
  regs.take(BaselineSecondScratchReg);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  MOZ_ASSERT(!regs.has(BaselineStackReg));
  MOZ_ASSERT(!regs.has(ICTailCallReg));
  MOZ_ASSERT(!regs.has(BaselineSecondScratchReg));
#elif defined(JS_CODEGEN_ARM64)
  MOZ_ASSERT(!regs.has(PseudoStackPointer));
  MOZ_ASSERT(!regs.has(RealStackPointer));
  MOZ_ASSERT(!regs.has(ICTailCallReg));
#else
  MOZ_ASSERT(!regs.has(BaselineStackReg));
#endif
  regs.take(BaselineFrameReg);
  regs.take(ICStubReg);
#ifdef JS_CODEGEN_X64
  regs.take(ExtractTemp0);
  regs.take(ExtractTemp1);
#endif

  switch (numInputs) {
    case 0:
      break;
    case 1:
      regs.take(R0);
      break;
    case 2:
      regs.take(R0);
      regs.take(R1);
      break;
    default:
      MOZ_CRASH("Invalid numInputs");
  }

  return regs;
}

#ifdef JS_JITSPEW
void FallbackICSpew(JSContext* cx, ICFallbackStub* stub, const char* fmt, ...) {
  if (JitSpewEnabled(JitSpew_BaselineICFallback)) {
    RootedScript script(cx, GetTopJitJSScript(cx));
    jsbytecode* pc = stub->pc(script);

    char fmtbuf[100];
    va_list args;
    va_start(args, fmt);
    (void)VsprintfLiteral(fmtbuf, fmt, args);
    va_end(args);

    JitSpew(
        JitSpew_BaselineICFallback,
        "Fallback hit for (%s:%u:%u) (pc=%zu,line=%u,uses=%u,stubs=%zu): %s",
        script->filename(), script->lineno(), script->column(),
        script->pcToOffset(pc), PCToLineNumber(script, pc),
        script->getWarmUpCount(), stub->numOptimizedStubs(), fmtbuf);
  }
}
#endif  // JS_JITSPEW

void ICEntry::trace(JSTracer* trc) {
  ICStub* stub = firstStub();

  // Trace CacheIR stubs.
  while (!stub->isFallback()) {
    stub->toCacheIRStub()->trace(trc);
    stub = stub->toCacheIRStub()->next();
  }

  // Fallback stubs use runtime-wide trampoline code we don't need to trace.
  MOZ_ASSERT(stub->usesTrampolineCode());
}

// constexpr table mapping JSOp to BaselineICFallbackKind. Each value in the
// table is either a fallback kind or a sentinel value (NoICValue) indicating
// the JSOp is not a JOF_IC op.
class MOZ_STATIC_CLASS OpToFallbackKindTable {
  static_assert(sizeof(BaselineICFallbackKind) == sizeof(uint8_t));
  uint8_t table_[JSOP_LIMIT] = {};

  constexpr void setKind(JSOp op, BaselineICFallbackKind kind) {
    MOZ_ASSERT(uint8_t(kind) != NoICValue);
    table_[size_t(op)] = uint8_t(kind);
  }

 public:
  static constexpr uint8_t NoICValue = uint8_t(BaselineICFallbackKind::Count);

  uint8_t lookup(JSOp op) const { return table_[size_t(op)]; }

  constexpr OpToFallbackKindTable() {
    for (size_t i = 0; i < JSOP_LIMIT; i++) {
      table_[i] = NoICValue;
    }

    setKind(JSOp::Not, BaselineICFallbackKind::ToBool);
    setKind(JSOp::And, BaselineICFallbackKind::ToBool);
    setKind(JSOp::Or, BaselineICFallbackKind::ToBool);
    setKind(JSOp::JumpIfTrue, BaselineICFallbackKind::ToBool);
    setKind(JSOp::JumpIfFalse, BaselineICFallbackKind::ToBool);

    setKind(JSOp::BitNot, BaselineICFallbackKind::UnaryArith);
    setKind(JSOp::Pos, BaselineICFallbackKind::UnaryArith);
    setKind(JSOp::Neg, BaselineICFallbackKind::UnaryArith);
    setKind(JSOp::Inc, BaselineICFallbackKind::UnaryArith);
    setKind(JSOp::Dec, BaselineICFallbackKind::UnaryArith);
    setKind(JSOp::ToNumeric, BaselineICFallbackKind::UnaryArith);

    setKind(JSOp::BitOr, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::BitXor, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::BitAnd, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Lsh, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Rsh, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Ursh, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Add, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Sub, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Mul, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Div, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Mod, BaselineICFallbackKind::BinaryArith);
    setKind(JSOp::Pow, BaselineICFallbackKind::BinaryArith);

    setKind(JSOp::Eq, BaselineICFallbackKind::Compare);
    setKind(JSOp::Ne, BaselineICFallbackKind::Compare);
    setKind(JSOp::Lt, BaselineICFallbackKind::Compare);
    setKind(JSOp::Le, BaselineICFallbackKind::Compare);
    setKind(JSOp::Gt, BaselineICFallbackKind::Compare);
    setKind(JSOp::Ge, BaselineICFallbackKind::Compare);
    setKind(JSOp::StrictEq, BaselineICFallbackKind::Compare);
    setKind(JSOp::StrictNe, BaselineICFallbackKind::Compare);

    setKind(JSOp::NewArray, BaselineICFallbackKind::NewArray);

    setKind(JSOp::NewObject, BaselineICFallbackKind::NewObject);
    setKind(JSOp::NewInit, BaselineICFallbackKind::NewObject);

    setKind(JSOp::InitElem, BaselineICFallbackKind::SetElem);
    setKind(JSOp::InitHiddenElem, BaselineICFallbackKind::SetElem);
    setKind(JSOp::InitLockedElem, BaselineICFallbackKind::SetElem);
    setKind(JSOp::InitElemInc, BaselineICFallbackKind::SetElem);
    setKind(JSOp::SetElem, BaselineICFallbackKind::SetElem);
    setKind(JSOp::StrictSetElem, BaselineICFallbackKind::SetElem);

    setKind(JSOp::InitProp, BaselineICFallbackKind::SetProp);
    setKind(JSOp::InitLockedProp, BaselineICFallbackKind::SetProp);
    setKind(JSOp::InitHiddenProp, BaselineICFallbackKind::SetProp);
    setKind(JSOp::InitGLexical, BaselineICFallbackKind::SetProp);
    setKind(JSOp::SetProp, BaselineICFallbackKind::SetProp);
    setKind(JSOp::StrictSetProp, BaselineICFallbackKind::SetProp);
    setKind(JSOp::SetName, BaselineICFallbackKind::SetProp);
    setKind(JSOp::StrictSetName, BaselineICFallbackKind::SetProp);
    setKind(JSOp::SetGName, BaselineICFallbackKind::SetProp);
    setKind(JSOp::StrictSetGName, BaselineICFallbackKind::SetProp);

    setKind(JSOp::GetProp, BaselineICFallbackKind::GetProp);
    setKind(JSOp::GetBoundName, BaselineICFallbackKind::GetProp);

    setKind(JSOp::GetPropSuper, BaselineICFallbackKind::GetPropSuper);

    setKind(JSOp::GetElem, BaselineICFallbackKind::GetElem);

    setKind(JSOp::GetElemSuper, BaselineICFallbackKind::GetElemSuper);

    setKind(JSOp::In, BaselineICFallbackKind::In);

    setKind(JSOp::HasOwn, BaselineICFallbackKind::HasOwn);

    setKind(JSOp::CheckPrivateField, BaselineICFallbackKind::CheckPrivateField);

    setKind(JSOp::GetName, BaselineICFallbackKind::GetName);
    setKind(JSOp::GetGName, BaselineICFallbackKind::GetName);

    setKind(JSOp::BindName, BaselineICFallbackKind::BindName);
    setKind(JSOp::BindGName, BaselineICFallbackKind::BindName);

    setKind(JSOp::GetIntrinsic, BaselineICFallbackKind::GetIntrinsic);

    setKind(JSOp::Call, BaselineICFallbackKind::Call);
    setKind(JSOp::CallIgnoresRv, BaselineICFallbackKind::Call);
    setKind(JSOp::CallIter, BaselineICFallbackKind::Call);
    setKind(JSOp::FunCall, BaselineICFallbackKind::Call);
    setKind(JSOp::FunApply, BaselineICFallbackKind::Call);
    setKind(JSOp::Eval, BaselineICFallbackKind::Call);
    setKind(JSOp::StrictEval, BaselineICFallbackKind::Call);

    setKind(JSOp::SuperCall, BaselineICFallbackKind::CallConstructing);
    setKind(JSOp::New, BaselineICFallbackKind::CallConstructing);

    setKind(JSOp::SpreadCall, BaselineICFallbackKind::SpreadCall);
    setKind(JSOp::SpreadEval, BaselineICFallbackKind::SpreadCall);
    setKind(JSOp::StrictSpreadEval, BaselineICFallbackKind::SpreadCall);

    setKind(JSOp::SpreadSuperCall,
            BaselineICFallbackKind::SpreadCallConstructing);
    setKind(JSOp::SpreadNew, BaselineICFallbackKind::SpreadCallConstructing);

    setKind(JSOp::Instanceof, BaselineICFallbackKind::InstanceOf);

    setKind(JSOp::Typeof, BaselineICFallbackKind::TypeOf);
    setKind(JSOp::TypeofExpr, BaselineICFallbackKind::TypeOf);

    setKind(JSOp::ToPropertyKey, BaselineICFallbackKind::ToPropertyKey);

    setKind(JSOp::Iter, BaselineICFallbackKind::GetIterator);

    setKind(JSOp::OptimizeSpreadCall,
            BaselineICFallbackKind::OptimizeSpreadCall);

    setKind(JSOp::Rest, BaselineICFallbackKind::Rest);
  }
};

static constexpr OpToFallbackKindTable FallbackKindTable;

void ICScript::initICEntries(JSContext* cx, JSScript* script) {
  MOZ_ASSERT(cx->realm()->jitRealm());
  MOZ_ASSERT(jit::IsBaselineInterpreterEnabled());

  MOZ_ASSERT(numICEntries() == script->numICEntries());

  // Index of the next ICEntry to initialize.
  uint32_t icEntryIndex = 0;

  const BaselineICFallbackCode& fallbackCode =
      cx->runtime()->jitRuntime()->baselineICFallbackCode();

  // For JOF_IC ops: initialize ICEntries and fallback stubs.
  for (BytecodeLocation loc : js::AllBytecodesIterable(script)) {
    JSOp op = loc.getOp();

    // Assert the frontend stored the correct IC index in jump target ops.
    MOZ_ASSERT_IF(BytecodeIsJumpTarget(op), loc.icIndex() == icEntryIndex);

    uint8_t tableValue = FallbackKindTable.lookup(op);

    if (tableValue == OpToFallbackKindTable::NoICValue) {
      MOZ_ASSERT(!BytecodeOpHasIC(op),
                 "Missing entry in OpToFallbackKindTable for JOF_IC op");
      continue;
    }

    MOZ_ASSERT(BytecodeOpHasIC(op),
               "Unexpected fallback kind for non-JOF_IC op");

    BaselineICFallbackKind kind = BaselineICFallbackKind(tableValue);
    TrampolinePtr stubCode = fallbackCode.addr(kind);

    // Initialize the ICEntry and ICFallbackStub.
    uint32_t offset = loc.bytecodeToOffset(script);
    ICEntry& entryRef = this->icEntry(icEntryIndex);
    ICFallbackStub* stub = fallbackStub(icEntryIndex);
    icEntryIndex++;
    new (&entryRef) ICEntry(stub);
    new (stub) ICFallbackStub(offset, stubCode);
  }

  // Assert all ICEntries have been initialized.
  MOZ_ASSERT(icEntryIndex == numICEntries());
}

bool ICSupportsPolymorphicTypeData(JSOp op) {
  MOZ_ASSERT(BytecodeOpHasIC(op));
  BaselineICFallbackKind kind =
      BaselineICFallbackKind(FallbackKindTable.lookup(op));
  switch (kind) {
    case BaselineICFallbackKind::ToBool:
    case BaselineICFallbackKind::TypeOf:
      return true;
    default:
      return false;
  }
}

bool ICCacheIRStub::makesGCCalls() const { return stubInfo()->makesGCCalls(); }

void ICFallbackStub::trackNotAttached() { state().trackNotAttached(); }

// When we enter a baseline fallback stub, if a Warp compilation
// exists that transpiled that IC, we notify that compilation. This
// helps the bailout code tell whether a bailing instruction hoisted
// by LICM would have been executed anyway.
static void MaybeNotifyWarp(JSScript* script, ICFallbackStub* stub) {
  if (stub->state().usedByTranspiler() && script->hasIonScript()) {
    script->ionScript()->noteBaselineFallback();
  }
}

void ICCacheIRStub::trace(JSTracer* trc) {
  JitCode* stubJitCode = jitCode();
  TraceManuallyBarrieredEdge(trc, &stubJitCode, "baseline-ic-stub-code");

  TraceCacheIRStub(trc, this, stubInfo());
}

static void MaybeTransition(JSContext* cx, BaselineFrame* frame,
                            ICFallbackStub* stub) {
  if (stub->state().maybeTransition()) {
    ICEntry* icEntry = frame->icScript()->icEntryForStub(stub);
#ifdef JS_CACHEIR_SPEW
    if (cx->spewer().enabled(cx, frame->script(),
                             SpewChannel::CacheIRHealthReport)) {
      CacheIRHealth cih;
      RootedScript script(cx, frame->script());
      cih.healthReportForIC(cx, icEntry, stub, script, SpewContext::Transition);
    }
#endif
    stub->discardStubs(cx, icEntry);
  }
}

// This helper handles ICState updates/transitions while attaching CacheIR
// stubs.
template <typename IRGenerator, typename... Args>
static void TryAttachStub(const char* name, JSContext* cx, BaselineFrame* frame,
                          ICFallbackStub* stub, Args&&... args) {
  MaybeTransition(cx, frame, stub);

  if (stub->state().canAttachStub()) {
    RootedScript script(cx, frame->script());
    ICScript* icScript = frame->icScript();
    jsbytecode* pc = stub->pc(script);
    bool attached = false;
    IRGenerator gen(cx, script, pc, stub->state(), std::forward<Args>(args)...);
    switch (gen.tryAttachStub()) {
      case AttachDecision::Attach: {
        ICStub* newStub =
            AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                      script, icScript, stub, &attached);
        if (newStub) {
          JitSpew(JitSpew_BaselineIC, "  Attached %s CacheIR stub", name);
        }
      } break;
      case AttachDecision::NoAction:
        break;
      case AttachDecision::TemporarilyUnoptimizable:
      case AttachDecision::Deferred:
        MOZ_ASSERT_UNREACHABLE("Not expected in generic TryAttachStub");
        break;
    }
    if (!attached) {
      stub->trackNotAttached();
    }
  }
}

void ICFallbackStub::unlinkStub(Zone* zone, ICEntry* icEntry,
                                ICCacheIRStub* prev, ICCacheIRStub* stub) {
  if (prev) {
    MOZ_ASSERT(prev->next() == stub);
    prev->setNext(stub->next());
  } else {
    MOZ_ASSERT(icEntry->firstStub() == stub);
    icEntry->setFirstStub(stub->next());
  }

  state_.trackUnlinkedStub();

  // We are removing edges from ICStub to gcthings. Perform a barrier to let the
  // GC know about those edges.
  PreWriteBarrier(zone, stub);

#ifdef DEBUG
  // Poison stub code to ensure we don't call this stub again. However, if
  // this stub can make calls, a pointer to it may be stored in a stub frame
  // on the stack, so we can't touch the stubCode_ or GC will crash when
  // tracing this pointer.
  if (!stub->makesGCCalls()) {
    stub->stubCode_ = (uint8_t*)0xbad;
  }
#endif
}

void ICFallbackStub::discardStubs(JSContext* cx, ICEntry* icEntry) {
  ICStub* stub = icEntry->firstStub();
  while (stub != this) {
    unlinkStub(cx->zone(), icEntry, /* prev = */ nullptr,
               stub->toCacheIRStub());
    stub = stub->toCacheIRStub()->next();
  }
}

static void InitMacroAssemblerForICStub(StackMacroAssembler& masm) {
#ifndef JS_USE_LINK_REGISTER
  // The first value contains the return addres,
  // which we pull into ICTailCallReg for tail calls.
  masm.adjustFrame(sizeof(intptr_t));
#endif
#ifdef JS_CODEGEN_ARM
  masm.setSecondScratchReg(BaselineSecondScratchReg);
#endif
}

bool FallbackICCodeCompiler::tailCallVMInternal(MacroAssembler& masm,
                                                TailCallVMFunctionId id) {
  TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(id);
  const VMFunctionData& fun = GetVMFunction(id);
  MOZ_ASSERT(fun.expectTailCall == TailCall);
  uint32_t argSize = fun.explicitStackSlots() * sizeof(void*);
  EmitBaselineTailCallVM(code, masm, argSize);
  return true;
}

bool FallbackICCodeCompiler::callVMInternal(MacroAssembler& masm,
                                            VMFunctionId id) {
  MOZ_ASSERT(inStubFrame_);

  TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(id);
  MOZ_ASSERT(GetVMFunction(id).expectTailCall == NonTailCall);

  EmitBaselineCallVM(code, masm);
  return true;
}

template <typename Fn, Fn fn>
bool FallbackICCodeCompiler::callVM(MacroAssembler& masm) {
  VMFunctionId id = VMFunctionToId<Fn, fn>::id;
  return callVMInternal(masm, id);
}

template <typename Fn, Fn fn>
bool FallbackICCodeCompiler::tailCallVM(MacroAssembler& masm) {
  TailCallVMFunctionId id = TailCallVMFunctionToId<Fn, fn>::id;
  return tailCallVMInternal(masm, id);
}

void FallbackICCodeCompiler::enterStubFrame(MacroAssembler& masm,
                                            Register scratch) {
  EmitBaselineEnterStubFrame(masm, scratch);
#ifdef DEBUG
  framePushedAtEnterStubFrame_ = masm.framePushed();
#endif

  MOZ_ASSERT(!inStubFrame_);
  inStubFrame_ = true;

#ifdef DEBUG
  entersStubFrame_ = true;
#endif
}

void FallbackICCodeCompiler::assumeStubFrame() {
  MOZ_ASSERT(!inStubFrame_);
  inStubFrame_ = true;

#ifdef DEBUG
  entersStubFrame_ = true;

  // |framePushed| isn't tracked precisely in ICStubs, so simply assume it to
  // be STUB_FRAME_SIZE so that assertions don't fail in leaveStubFrame.
  framePushedAtEnterStubFrame_ = STUB_FRAME_SIZE;
#endif
}

void FallbackICCodeCompiler::leaveStubFrame(MacroAssembler& masm,
                                            bool calledIntoIon) {
  MOZ_ASSERT(entersStubFrame_ && inStubFrame_);
  inStubFrame_ = false;

#ifdef DEBUG
  masm.setFramePushed(framePushedAtEnterStubFrame_);
  if (calledIntoIon) {
    masm.adjustFrame(sizeof(intptr_t));  // Calls into ion have this extra.
  }
#endif
  EmitBaselineLeaveStubFrame(masm, calledIntoIon);
}

void FallbackICCodeCompiler::pushStubPayload(MacroAssembler& masm,
                                             Register scratch) {
  if (inStubFrame_) {
    masm.loadPtr(Address(BaselineFrameReg, 0), scratch);
    masm.pushBaselineFramePtr(scratch, scratch);
  } else {
    masm.pushBaselineFramePtr(BaselineFrameReg, scratch);
  }
}

void FallbackICCodeCompiler::PushStubPayload(MacroAssembler& masm,
                                             Register scratch) {
  pushStubPayload(masm, scratch);
  masm.adjustFrame(sizeof(intptr_t));
}

//
// ToBool_Fallback
//

bool DoToBoolFallback(JSContext* cx, BaselineFrame* frame, ICFallbackStub* stub,
                      HandleValue arg, MutableHandleValue ret) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "ToBool");

  TryAttachStub<ToBoolIRGenerator>("ToBool", cx, frame, stub, arg);

  bool cond = ToBoolean(arg);
  ret.setBoolean(cond);

  return true;
}

bool FallbackICCodeCompiler::emit_ToBool() {
  static_assert(R0 == JSReturnOperand);

  // Restore the tail call register.
  EmitRestoreTailCallReg(masm);

  // Push arguments.
  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      MutableHandleValue);
  return tailCallVM<Fn, DoToBoolFallback>(masm);
}

//
// GetElem_Fallback
//

bool DoGetElemFallback(JSContext* cx, BaselineFrame* frame,
                       ICFallbackStub* stub, HandleValue lhs, HandleValue rhs,
                       MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "GetElem");

#ifdef DEBUG
  jsbytecode* pc = stub->pc(frame->script());
  MOZ_ASSERT(JSOp(*pc) == JSOp::GetElem);
#endif

  TryAttachStub<GetPropIRGenerator>("GetElem", cx, frame, stub,
                                    CacheKind::GetElem, lhs, rhs);

  if (!GetElementOperation(cx, lhs, rhs, res)) {
    return false;
  }

  return true;
}

bool DoGetElemSuperFallback(JSContext* cx, BaselineFrame* frame,
                            ICFallbackStub* stub, HandleValue lhs,
                            HandleValue rhs, HandleValue receiver,
                            MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(frame->script());

  JSOp op = JSOp(*pc);
  FallbackICSpew(cx, stub, "GetElemSuper(%s)", CodeName(op));

  MOZ_ASSERT(op == JSOp::GetElemSuper);

  TryAttachStub<GetPropIRGenerator>("GetElemSuper", cx, frame, stub,
                                    CacheKind::GetElemSuper, lhs, rhs);

  // |lhs| is [[HomeObject]].[[Prototype]] which must be Object
  RootedObject lhsObj(cx, &lhs.toObject());
  return GetObjectElementOperation(cx, op, lhsObj, receiver, rhs, res);
}

bool FallbackICCodeCompiler::emitGetElem(bool hasReceiver) {
  static_assert(R0 == JSReturnOperand);

  // Restore the tail call register.
  EmitRestoreTailCallReg(masm);

  // Super property getters use a |this| that differs from base object
  if (hasReceiver) {
    // State: receiver in R0, index in R1, obj on the stack

    // Ensure stack is fully synced for the expression decompiler.
    // We need: receiver, index, obj
    masm.pushValue(R0);
    masm.pushValue(R1);
    masm.pushValue(Address(masm.getStackPointer(), sizeof(Value) * 2));

    // Push arguments.
    masm.pushValue(R0);  // Receiver
    masm.pushValue(R1);  // Index
    masm.pushValue(Address(masm.getStackPointer(), sizeof(Value) * 5));  // Obj
    masm.push(ICStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    using Fn =
        bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                 HandleValue, HandleValue, MutableHandleValue);
    if (!tailCallVM<Fn, DoGetElemSuperFallback>(masm)) {
      return false;
    }
  } else {
    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*,
                        HandleValue, HandleValue, MutableHandleValue);
    if (!tailCallVM<Fn, DoGetElemFallback>(masm)) {
      return false;
    }
  }

  // This is the resume point used when bailout rewrites call stack to undo
  // Ion inlined frames. The return address pushed onto reconstructed stack
  // will point here.
  assumeStubFrame();
  if (hasReceiver) {
    code.initBailoutReturnOffset(BailoutReturnKind::GetElemSuper,
                                 masm.currentOffset());
  } else {
    code.initBailoutReturnOffset(BailoutReturnKind::GetElem,
                                 masm.currentOffset());
  }

  leaveStubFrame(masm, true);

  EmitReturnFromIC(masm);
  return true;
}

bool FallbackICCodeCompiler::emit_GetElem() {
  return emitGetElem(/* hasReceiver = */ false);
}

bool FallbackICCodeCompiler::emit_GetElemSuper() {
  return emitGetElem(/* hasReceiver = */ true);
}

bool DoSetElemFallback(JSContext* cx, BaselineFrame* frame,
                       ICFallbackStub* stub, Value* stack, HandleValue objv,
                       HandleValue index, HandleValue rhs) {
  using DeferType = SetPropIRGenerator::DeferType;

  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  RootedScript outerScript(cx, script);
  jsbytecode* pc = stub->pc(script);
  JSOp op = JSOp(*pc);
  FallbackICSpew(cx, stub, "SetElem(%s)", CodeName(JSOp(*pc)));

  MOZ_ASSERT(op == JSOp::SetElem || op == JSOp::StrictSetElem ||
             op == JSOp::InitElem || op == JSOp::InitHiddenElem ||
             op == JSOp::InitLockedElem || op == JSOp::InitElemInc);

  int objvIndex = -3;
  RootedObject obj(
      cx, ToObjectFromStackForPropertyAccess(cx, objv, objvIndex, index));
  if (!obj) {
    return false;
  }

  RootedShape oldShape(cx, obj->shape());

  // We cannot attach a stub if the operation executed after the stub
  // is attached may throw.
  bool mayThrow = false;

  DeferType deferType = DeferType::None;
  bool attached = false;

  MaybeTransition(cx, frame, stub);

  if (stub->state().canAttachStub() && !mayThrow) {
    ICScript* icScript = frame->icScript();
    SetPropIRGenerator gen(cx, script, pc, CacheKind::SetElem, stub->state(),
                           objv, index, rhs);
    switch (gen.tryAttachStub()) {
      case AttachDecision::Attach: {
        ICStub* newStub = AttachBaselineCacheIRStub(
            cx, gen.writerRef(), gen.cacheKind(), frame->script(), icScript,
            stub, &attached);
        if (newStub) {
          JitSpew(JitSpew_BaselineIC, "  Attached SetElem CacheIR stub");
        }
      } break;
      case AttachDecision::NoAction:
        break;
      case AttachDecision::TemporarilyUnoptimizable:
        attached = true;
        break;
      case AttachDecision::Deferred:
        deferType = gen.deferType();
        MOZ_ASSERT(deferType != DeferType::None);
        break;
    }
  }

  if (op == JSOp::InitElem || op == JSOp::InitHiddenElem ||
      op == JSOp::InitLockedElem) {
    if (!InitElemOperation(cx, pc, obj, index, rhs)) {
      return false;
    }
  } else if (op == JSOp::InitElemInc) {
    if (!InitElemIncOperation(cx, obj.as<ArrayObject>(), index.toInt32(),
                              rhs)) {
      return false;
    }
  } else {
    if (!SetObjectElementWithReceiver(cx, obj, index, rhs, objv,
                                      JSOp(*pc) == JSOp::StrictSetElem)) {
      return false;
    }
  }

  // Don't try to attach stubs that wish to be hidden. We don't know how to
  // have different enumerability in the stubs for the moment.
  if (op == JSOp::InitHiddenElem) {
    return true;
  }

  // Overwrite the object on the stack (pushed for the decompiler) with the rhs.
  MOZ_ASSERT(stack[2] == objv);
  stack[2] = rhs;

  if (attached) {
    return true;
  }

  // The SetObjectElement call might have entered this IC recursively, so try
  // to transition.
  MaybeTransition(cx, frame, stub);

  bool canAttachStub = stub->state().canAttachStub();

  if (deferType != DeferType::None && canAttachStub) {
    SetPropIRGenerator gen(cx, script, pc, CacheKind::SetElem, stub->state(),
                           objv, index, rhs);

    MOZ_ASSERT(deferType == DeferType::AddSlot);
    AttachDecision decision = gen.tryAttachAddSlotStub(oldShape);

    switch (decision) {
      case AttachDecision::Attach: {
        ICScript* icScript = frame->icScript();
        ICStub* newStub = AttachBaselineCacheIRStub(
            cx, gen.writerRef(), gen.cacheKind(), frame->script(), icScript,
            stub, &attached);
        if (newStub) {
          JitSpew(JitSpew_BaselineIC, "  Attached SetElem CacheIR stub");
        }
      } break;
      case AttachDecision::NoAction:
        gen.trackAttached(IRGenerator::NotAttached);
        break;
      case AttachDecision::TemporarilyUnoptimizable:
      case AttachDecision::Deferred:
        MOZ_ASSERT_UNREACHABLE("Invalid attach result");
        break;
    }
  }
  if (!attached && canAttachStub) {
    stub->trackNotAttached();
  }
  return true;
}

bool FallbackICCodeCompiler::emit_SetElem() {
  static_assert(R0 == JSReturnOperand);

  EmitRestoreTailCallReg(masm);

  // State: R0: object, R1: index, stack: rhs.
  // For the decompiler, the stack has to be: object, index, rhs,
  // so we push the index, then overwrite the rhs Value with R0
  // and push the rhs value.
  masm.pushValue(R1);
  masm.loadValue(Address(masm.getStackPointer(), sizeof(Value)), R1);
  masm.storeValue(R0, Address(masm.getStackPointer(), sizeof(Value)));
  masm.pushValue(R1);

  // Push arguments.
  masm.pushValue(R1);  // RHS

  // Push index. On x86 and ARM two push instructions are emitted so use a
  // separate register to store the old stack pointer.
  masm.moveStackPtrTo(R1.scratchReg());
  masm.pushValue(Address(R1.scratchReg(), 2 * sizeof(Value)));
  masm.pushValue(R0);  // Object.

  // Push pointer to stack values, so that the stub can overwrite the object
  // (pushed for the decompiler) with the rhs.
  masm.computeEffectiveAddress(
      Address(masm.getStackPointer(), 3 * sizeof(Value)), R0.scratchReg());
  masm.push(R0.scratchReg());

  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, Value*,
                      HandleValue, HandleValue, HandleValue);
  return tailCallVM<Fn, DoSetElemFallback>(masm);
}

//
// In_Fallback
//

bool DoInFallback(JSContext* cx, BaselineFrame* frame, ICFallbackStub* stub,
                  HandleValue key, HandleValue objValue,
                  MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "In");

  if (!objValue.isObject()) {
    ReportInNotObjectError(cx, key, objValue);
    return false;
  }

  TryAttachStub<HasPropIRGenerator>("In", cx, frame, stub, CacheKind::In, key,
                                    objValue);

  RootedObject obj(cx, &objValue.toObject());
  bool cond = false;
  if (!OperatorIn(cx, key, obj, &cond)) {
    return false;
  }
  res.setBoolean(cond);

  return true;
}

bool FallbackICCodeCompiler::emit_In() {
  EmitRestoreTailCallReg(masm);

  // Sync for the decompiler.
  masm.pushValue(R0);
  masm.pushValue(R1);

  // Push arguments.
  masm.pushValue(R1);
  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      HandleValue, MutableHandleValue);
  return tailCallVM<Fn, DoInFallback>(masm);
}

//
// HasOwn_Fallback
//

bool DoHasOwnFallback(JSContext* cx, BaselineFrame* frame, ICFallbackStub* stub,
                      HandleValue keyValue, HandleValue objValue,
                      MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "HasOwn");

  TryAttachStub<HasPropIRGenerator>("HasOwn", cx, frame, stub,
                                    CacheKind::HasOwn, keyValue, objValue);

  bool found;
  if (!HasOwnProperty(cx, objValue, keyValue, &found)) {
    return false;
  }

  res.setBoolean(found);
  return true;
}

bool FallbackICCodeCompiler::emit_HasOwn() {
  EmitRestoreTailCallReg(masm);

  // Sync for the decompiler.
  masm.pushValue(R0);
  masm.pushValue(R1);

  // Push arguments.
  masm.pushValue(R1);
  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      HandleValue, MutableHandleValue);
  return tailCallVM<Fn, DoHasOwnFallback>(masm);
}

//
// CheckPrivate_Fallback
//

bool DoCheckPrivateFieldFallback(JSContext* cx, BaselineFrame* frame,
                                 ICFallbackStub* stub, HandleValue objValue,
                                 HandleValue keyValue, MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);

  FallbackICSpew(cx, stub, "CheckPrivateField");

  MOZ_ASSERT(keyValue.isSymbol() && keyValue.toSymbol()->isPrivateName());

  TryAttachStub<CheckPrivateFieldIRGenerator>("CheckPrivate", cx, frame, stub,
                                              CacheKind::CheckPrivateField,
                                              keyValue, objValue);

  bool result;
  if (!CheckPrivateFieldOperation(cx, pc, objValue, keyValue, &result)) {
    return false;
  }

  res.setBoolean(result);
  return true;
}

bool FallbackICCodeCompiler::emit_CheckPrivateField() {
  EmitRestoreTailCallReg(masm);

  // Sync for the decompiler.
  masm.pushValue(R0);
  masm.pushValue(R1);

  // Push arguments.
  masm.pushValue(R1);
  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      HandleValue, MutableHandleValue);
  return tailCallVM<Fn, DoCheckPrivateFieldFallback>(masm);
}

//
// GetName_Fallback
//

bool DoGetNameFallback(JSContext* cx, BaselineFrame* frame,
                       ICFallbackStub* stub, HandleObject envChain,
                       MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  mozilla::DebugOnly<JSOp> op = JSOp(*pc);
  FallbackICSpew(cx, stub, "GetName(%s)", CodeName(JSOp(*pc)));

  MOZ_ASSERT(op == JSOp::GetName || op == JSOp::GetGName);

  RootedPropertyName name(cx, script->getName(pc));

  TryAttachStub<GetNameIRGenerator>("GetName", cx, frame, stub, envChain, name);

  static_assert(JSOpLength_GetGName == JSOpLength_GetName,
                "Otherwise our check for JSOp::Typeof isn't ok");
  if (JSOp(pc[JSOpLength_GetGName]) == JSOp::Typeof) {
    if (!GetEnvironmentName<GetNameMode::TypeOf>(cx, envChain, name, res)) {
      return false;
    }
  } else {
    if (!GetEnvironmentName<GetNameMode::Normal>(cx, envChain, name, res)) {
      return false;
    }
  }

  return true;
}

bool FallbackICCodeCompiler::emit_GetName() {
  static_assert(R0 == JSReturnOperand);

  EmitRestoreTailCallReg(masm);

  masm.push(R0.scratchReg());
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleObject,
                      MutableHandleValue);
  return tailCallVM<Fn, DoGetNameFallback>(masm);
}

//
// BindName_Fallback
//

bool DoBindNameFallback(JSContext* cx, BaselineFrame* frame,
                        ICFallbackStub* stub, HandleObject envChain,
                        MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  jsbytecode* pc = stub->pc(frame->script());
  mozilla::DebugOnly<JSOp> op = JSOp(*pc);
  FallbackICSpew(cx, stub, "BindName(%s)", CodeName(JSOp(*pc)));

  MOZ_ASSERT(op == JSOp::BindName || op == JSOp::BindGName);

  RootedPropertyName name(cx, frame->script()->getName(pc));

  TryAttachStub<BindNameIRGenerator>("BindName", cx, frame, stub, envChain,
                                     name);

  RootedObject scope(cx);
  if (!LookupNameUnqualified(cx, name, envChain, &scope)) {
    return false;
  }

  res.setObject(*scope);
  return true;
}

bool FallbackICCodeCompiler::emit_BindName() {
  static_assert(R0 == JSReturnOperand);

  EmitRestoreTailCallReg(masm);

  masm.push(R0.scratchReg());
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleObject,
                      MutableHandleValue);
  return tailCallVM<Fn, DoBindNameFallback>(masm);
}

//
// GetIntrinsic_Fallback
//

bool DoGetIntrinsicFallback(JSContext* cx, BaselineFrame* frame,
                            ICFallbackStub* stub, MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  mozilla::DebugOnly<JSOp> op = JSOp(*pc);
  FallbackICSpew(cx, stub, "GetIntrinsic(%s)", CodeName(JSOp(*pc)));

  MOZ_ASSERT(op == JSOp::GetIntrinsic);

  if (!GetIntrinsicOperation(cx, script, pc, res)) {
    return false;
  }

  TryAttachStub<GetIntrinsicIRGenerator>("GetIntrinsic", cx, frame, stub, res);

  return true;
}

bool FallbackICCodeCompiler::emit_GetIntrinsic() {
  EmitRestoreTailCallReg(masm);

  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn =
      bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, MutableHandleValue);
  return tailCallVM<Fn, DoGetIntrinsicFallback>(masm);
}

//
// GetProp_Fallback
//

bool DoGetPropFallback(JSContext* cx, BaselineFrame* frame,
                       ICFallbackStub* stub, MutableHandleValue val,
                       MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  JSOp op = JSOp(*pc);
  FallbackICSpew(cx, stub, "GetProp(%s)", CodeName(op));

  MOZ_ASSERT(op == JSOp::GetProp || op == JSOp::GetBoundName);

  RootedPropertyName name(cx, script->getName(pc));
  RootedValue idVal(cx, StringValue(name));

  TryAttachStub<GetPropIRGenerator>("GetProp", cx, frame, stub,
                                    CacheKind::GetProp, val, idVal);

  if (op == JSOp::GetBoundName) {
    RootedObject env(cx, &val.toObject());
    RootedId id(cx, NameToId(name));
    return GetNameBoundInEnvironment(cx, env, id, res);
  }

  MOZ_ASSERT(op == JSOp::GetProp);
  if (!GetProperty(cx, val, name, res)) {
    return false;
  }

  return true;
}

bool DoGetPropSuperFallback(JSContext* cx, BaselineFrame* frame,
                            ICFallbackStub* stub, HandleValue receiver,
                            MutableHandleValue val, MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  FallbackICSpew(cx, stub, "GetPropSuper(%s)", CodeName(JSOp(*pc)));

  MOZ_ASSERT(JSOp(*pc) == JSOp::GetPropSuper);

  RootedPropertyName name(cx, script->getName(pc));
  RootedValue idVal(cx, StringValue(name));

  TryAttachStub<GetPropIRGenerator>("GetPropSuper", cx, frame, stub,
                                    CacheKind::GetPropSuper, val, idVal);

  // |val| is [[HomeObject]].[[Prototype]] which must be Object
  RootedObject valObj(cx, &val.toObject());
  if (!GetProperty(cx, valObj, receiver, name, res)) {
    return false;
  }

  return true;
}

bool FallbackICCodeCompiler::emitGetProp(bool hasReceiver) {
  static_assert(R0 == JSReturnOperand);

  EmitRestoreTailCallReg(masm);

  // Super property getters use a |this| that differs from base object
  if (hasReceiver) {
    // Push arguments.
    masm.pushValue(R0);
    masm.pushValue(R1);
    masm.push(ICStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*,
                        HandleValue, MutableHandleValue, MutableHandleValue);
    if (!tailCallVM<Fn, DoGetPropSuperFallback>(masm)) {
      return false;
    }
  } else {
    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(ICStubReg);
    masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*,
                        MutableHandleValue, MutableHandleValue);
    if (!tailCallVM<Fn, DoGetPropFallback>(masm)) {
      return false;
    }
  }

  // This is the resume point used when bailout rewrites call stack to undo
  // Ion inlined frames. The return address pushed onto reconstructed stack
  // will point here.
  assumeStubFrame();
  if (hasReceiver) {
    code.initBailoutReturnOffset(BailoutReturnKind::GetPropSuper,
                                 masm.currentOffset());
  } else {
    code.initBailoutReturnOffset(BailoutReturnKind::GetProp,
                                 masm.currentOffset());
  }

  leaveStubFrame(masm, true);

  EmitReturnFromIC(masm);
  return true;
}

bool FallbackICCodeCompiler::emit_GetProp() {
  return emitGetProp(/* hasReceiver = */ false);
}

bool FallbackICCodeCompiler::emit_GetPropSuper() {
  return emitGetProp(/* hasReceiver = */ true);
}

//
// SetProp_Fallback
//

bool DoSetPropFallback(JSContext* cx, BaselineFrame* frame,
                       ICFallbackStub* stub, Value* stack, HandleValue lhs,
                       HandleValue rhs) {
  using DeferType = SetPropIRGenerator::DeferType;

  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  JSOp op = JSOp(*pc);
  FallbackICSpew(cx, stub, "SetProp(%s)", CodeName(op));

  MOZ_ASSERT(op == JSOp::SetProp || op == JSOp::StrictSetProp ||
             op == JSOp::SetName || op == JSOp::StrictSetName ||
             op == JSOp::SetGName || op == JSOp::StrictSetGName ||
             op == JSOp::InitProp || op == JSOp::InitLockedProp ||
             op == JSOp::InitHiddenProp || op == JSOp::InitGLexical);

  RootedPropertyName name(cx, script->getName(pc));
  RootedId id(cx, NameToId(name));

  int lhsIndex = -2;
  RootedObject obj(cx,
                   ToObjectFromStackForPropertyAccess(cx, lhs, lhsIndex, id));
  if (!obj) {
    return false;
  }
  RootedShape oldShape(cx, obj->shape());

  DeferType deferType = DeferType::None;
  bool attached = false;
  MaybeTransition(cx, frame, stub);

  if (stub->state().canAttachStub()) {
    RootedValue idVal(cx, StringValue(name));
    SetPropIRGenerator gen(cx, script, pc, CacheKind::SetProp, stub->state(),
                           lhs, idVal, rhs);
    switch (gen.tryAttachStub()) {
      case AttachDecision::Attach: {
        ICScript* icScript = frame->icScript();
        ICStub* newStub = AttachBaselineCacheIRStub(
            cx, gen.writerRef(), gen.cacheKind(), frame->script(), icScript,
            stub, &attached);
        if (newStub) {
          JitSpew(JitSpew_BaselineIC, "  Attached SetProp CacheIR stub");
        }
      } break;
      case AttachDecision::NoAction:
        break;
      case AttachDecision::TemporarilyUnoptimizable:
        attached = true;
        break;
      case AttachDecision::Deferred:
        deferType = gen.deferType();
        MOZ_ASSERT(deferType != DeferType::None);
        break;
    }
  }

  if (op == JSOp::InitProp || op == JSOp::InitLockedProp ||
      op == JSOp::InitHiddenProp) {
    if (!InitPropertyOperation(cx, op, obj, name, rhs)) {
      return false;
    }
  } else if (op == JSOp::SetName || op == JSOp::StrictSetName ||
             op == JSOp::SetGName || op == JSOp::StrictSetGName) {
    if (!SetNameOperation(cx, script, pc, obj, rhs)) {
      return false;
    }
  } else if (op == JSOp::InitGLexical) {
    RootedValue v(cx, rhs);
    ExtensibleLexicalEnvironmentObject* lexicalEnv;
    if (script->hasNonSyntacticScope()) {
      lexicalEnv = &NearestEnclosingExtensibleLexicalEnvironment(
          frame->environmentChain());
    } else {
      lexicalEnv = &cx->global()->lexicalEnvironment();
    }
    InitGlobalLexicalOperation(cx, lexicalEnv, script, pc, v);
  } else {
    MOZ_ASSERT(op == JSOp::SetProp || op == JSOp::StrictSetProp);

    ObjectOpResult result;
    if (!SetProperty(cx, obj, id, rhs, lhs, result) ||
        !result.checkStrictModeError(cx, obj, id, op == JSOp::StrictSetProp)) {
      return false;
    }
  }

  // Overwrite the LHS on the stack (pushed for the decompiler) with the RHS.
  MOZ_ASSERT(stack[1] == lhs);
  stack[1] = rhs;

  if (attached) {
    return true;
  }

  // The SetProperty call might have entered this IC recursively, so try
  // to transition.
  MaybeTransition(cx, frame, stub);

  bool canAttachStub = stub->state().canAttachStub();

  if (deferType != DeferType::None && canAttachStub) {
    RootedValue idVal(cx, StringValue(name));
    SetPropIRGenerator gen(cx, script, pc, CacheKind::SetProp, stub->state(),
                           lhs, idVal, rhs);

    MOZ_ASSERT(deferType == DeferType::AddSlot);
    AttachDecision decision = gen.tryAttachAddSlotStub(oldShape);

    switch (decision) {
      case AttachDecision::Attach: {
        ICScript* icScript = frame->icScript();
        ICStub* newStub = AttachBaselineCacheIRStub(
            cx, gen.writerRef(), gen.cacheKind(), frame->script(), icScript,
            stub, &attached);
        if (newStub) {
          JitSpew(JitSpew_BaselineIC, "  Attached SetElem CacheIR stub");
        }
      } break;
      case AttachDecision::NoAction:
        gen.trackAttached(IRGenerator::NotAttached);
        break;
      case AttachDecision::TemporarilyUnoptimizable:
      case AttachDecision::Deferred:
        MOZ_ASSERT_UNREACHABLE("Invalid attach result");
        break;
    }
  }
  if (!attached && canAttachStub) {
    stub->trackNotAttached();
  }

  return true;
}

bool FallbackICCodeCompiler::emit_SetProp() {
  static_assert(R0 == JSReturnOperand);

  EmitRestoreTailCallReg(masm);

  // Ensure stack is fully synced for the expression decompiler.
  // Overwrite the RHS value on top of the stack with the object, then push
  // the RHS in R1 on top of that.
  masm.storeValue(R0, Address(masm.getStackPointer(), 0));
  masm.pushValue(R1);

  // Push arguments.
  masm.pushValue(R1);
  masm.pushValue(R0);

  // Push pointer to stack values, so that the stub can overwrite the object
  // (pushed for the decompiler) with the RHS.
  masm.computeEffectiveAddress(
      Address(masm.getStackPointer(), 2 * sizeof(Value)), R0.scratchReg());
  masm.push(R0.scratchReg());

  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, Value*,
                      HandleValue, HandleValue);
  if (!tailCallVM<Fn, DoSetPropFallback>(masm)) {
    return false;
  }

  // This is the resume point used when bailout rewrites call stack to undo
  // Ion inlined frames. The return address pushed onto reconstructed stack
  // will point here.
  assumeStubFrame();
  code.initBailoutReturnOffset(BailoutReturnKind::SetProp,
                               masm.currentOffset());

  leaveStubFrame(masm, true);
  EmitReturnFromIC(masm);

  return true;
}

//
// Call_Fallback
//

bool DoCallFallback(JSContext* cx, BaselineFrame* frame, ICFallbackStub* stub,
                    uint32_t argc, Value* vp, MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  JSOp op = JSOp(*pc);
  FallbackICSpew(cx, stub, "Call(%s)", CodeName(op));

  MOZ_ASSERT(argc == GET_ARGC(pc));
  bool constructing = (op == JSOp::New || op == JSOp::SuperCall);
  bool ignoresReturnValue = (op == JSOp::CallIgnoresRv);

  // Ensure vp array is rooted - we may GC in here.
  size_t numValues = argc + 2 + constructing;
  RootedExternalValueArray vpRoot(cx, numValues, vp);

  CallArgs callArgs = CallArgsFromSp(argc + constructing, vp + numValues,
                                     constructing, ignoresReturnValue);
  RootedValue callee(cx, vp[0]);
  RootedValue newTarget(cx, constructing ? callArgs.newTarget() : NullValue());

  // Transition stub state to megamorphic or generic if warranted.
  MaybeTransition(cx, frame, stub);

  bool canAttachStub = stub->state().canAttachStub();
  bool handled = false;

  // Only bother to try optimizing JSOp::Call with CacheIR if the chain is still
  // allowed to attach stubs.
  if (canAttachStub) {
    HandleValueArray args = HandleValueArray::fromMarkedLocation(argc, vp + 2);
    CallIRGenerator gen(cx, script, pc, op, stub->state(), argc, callee,
                        callArgs.thisv(), newTarget, args);
    switch (gen.tryAttachStub()) {
      case AttachDecision::NoAction:
        break;
      case AttachDecision::Attach: {
        ICScript* icScript = frame->icScript();
        ICStub* newStub =
            AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                      script, icScript, stub, &handled);
        if (newStub) {
          JitSpew(JitSpew_BaselineIC, "  Attached Call CacheIR stub");
        }
      } break;
      case AttachDecision::TemporarilyUnoptimizable:
        handled = true;
        break;
      case AttachDecision::Deferred:
        MOZ_CRASH("No deferred Call stubs");
    }
    if (!handled) {
      stub->trackNotAttached();
    }
  }

  if (constructing) {
    if (!ConstructFromStack(cx, callArgs)) {
      return false;
    }
    res.set(callArgs.rval());
  } else if ((op == JSOp::Eval || op == JSOp::StrictEval) &&
             cx->global()->valueIsEval(callee)) {
    if (!DirectEval(cx, callArgs.get(0), res)) {
      return false;
    }
  } else {
    MOZ_ASSERT(op == JSOp::Call || op == JSOp::CallIgnoresRv ||
               op == JSOp::CallIter || op == JSOp::FunCall ||
               op == JSOp::FunApply || op == JSOp::Eval ||
               op == JSOp::StrictEval);
    if (op == JSOp::CallIter && callee.isPrimitive()) {
      MOZ_ASSERT(argc == 0, "thisv must be on top of the stack");
      ReportValueError(cx, JSMSG_NOT_ITERABLE, -1, callArgs.thisv(), nullptr);
      return false;
    }

    if (!CallFromStack(cx, callArgs)) {
      return false;
    }

    res.set(callArgs.rval());
  }

  return true;
}

bool DoSpreadCallFallback(JSContext* cx, BaselineFrame* frame,
                          ICFallbackStub* stub, Value* vp,
                          MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  JSOp op = JSOp(*pc);
  bool constructing = (op == JSOp::SpreadNew || op == JSOp::SpreadSuperCall);
  FallbackICSpew(cx, stub, "SpreadCall(%s)", CodeName(op));

  // Ensure vp array is rooted - we may GC in here.
  RootedExternalValueArray vpRoot(cx, 3 + constructing, vp);

  RootedValue callee(cx, vp[0]);
  RootedValue thisv(cx, vp[1]);
  RootedValue arr(cx, vp[2]);
  RootedValue newTarget(cx, constructing ? vp[3] : NullValue());

  // Transition stub state to megamorphic or generic if warranted.
  MaybeTransition(cx, frame, stub);

  // Try attaching a call stub.
  bool handled = false;
  if (op != JSOp::SpreadEval && op != JSOp::StrictSpreadEval &&
      stub->state().canAttachStub()) {
    // Try CacheIR first:
    RootedArrayObject aobj(cx, &arr.toObject().as<ArrayObject>());
    MOZ_ASSERT(aobj->length() == aobj->getDenseInitializedLength());

    HandleValueArray args = HandleValueArray::fromMarkedLocation(
        aobj->length(), aobj->getDenseElements());
    CallIRGenerator gen(cx, script, pc, op, stub->state(), 1, callee, thisv,
                        newTarget, args);
    switch (gen.tryAttachStub()) {
      case AttachDecision::NoAction:
        break;
      case AttachDecision::Attach: {
        ICScript* icScript = frame->icScript();
        ICStub* newStub =
            AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                      script, icScript, stub, &handled);

        if (newStub) {
          JitSpew(JitSpew_BaselineIC, "  Attached Spread Call CacheIR stub");
        }
      } break;
      case AttachDecision::TemporarilyUnoptimizable:
        handled = true;
        break;
      case AttachDecision::Deferred:
        MOZ_ASSERT_UNREACHABLE("No deferred optimizations for spread calls");
        break;
    }
    if (!handled) {
      stub->trackNotAttached();
    }
  }

  return SpreadCallOperation(cx, script, pc, thisv, callee, arr, newTarget,
                             res);
}

void FallbackICCodeCompiler::pushCallArguments(
    MacroAssembler& masm, AllocatableGeneralRegisterSet regs, Register argcReg,
    bool isConstructing) {
  MOZ_ASSERT(!regs.has(argcReg));

  // argPtr initially points to the last argument.
  Register argPtr = regs.takeAny();
  masm.moveStackPtrTo(argPtr);

  // Skip 4 pointers pushed on top of the arguments: the frame descriptor,
  // return address, old frame pointer and stub reg.
  size_t valueOffset = STUB_FRAME_SIZE;

  // We have to push |this|, callee, new.target (if constructing) and argc
  // arguments. Handle the number of Values we know statically first.

  size_t numNonArgValues = 2 + isConstructing;
  for (size_t i = 0; i < numNonArgValues; i++) {
    masm.pushValue(Address(argPtr, valueOffset));
    valueOffset += sizeof(Value);
  }

  // If there are no arguments we're done.
  Label done;
  masm.branchTest32(Assembler::Zero, argcReg, argcReg, &done);

  // Push argc Values.
  Label loop;
  Register count = regs.takeAny();
  masm.addPtr(Imm32(valueOffset), argPtr);
  masm.move32(argcReg, count);
  masm.bind(&loop);
  {
    masm.pushValue(Address(argPtr, 0));
    masm.addPtr(Imm32(sizeof(Value)), argPtr);

    masm.branchSub32(Assembler::NonZero, Imm32(1), count, &loop);
  }
  masm.bind(&done);
}

bool FallbackICCodeCompiler::emitCall(bool isSpread, bool isConstructing) {
  static_assert(R0 == JSReturnOperand);

  // Values are on the stack left-to-right. Calling convention wants them
  // right-to-left so duplicate them on the stack in reverse order.
  // |this| and callee are pushed last.

  AllocatableGeneralRegisterSet regs = BaselineICAvailableGeneralRegs(0);

  if (MOZ_UNLIKELY(isSpread)) {
    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, R1.scratchReg());

    // Use BaselineFrameReg instead of BaselineStackReg, because
    // BaselineFrameReg and BaselineStackReg hold the same value just after
    // calling enterStubFrame.

    // newTarget
    uint32_t valueOffset = 0;
    if (isConstructing) {
      masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE));
      valueOffset++;
    }

    // array
    masm.pushValue(Address(BaselineFrameReg,
                           valueOffset * sizeof(Value) + STUB_FRAME_SIZE));
    valueOffset++;

    // this
    masm.pushValue(Address(BaselineFrameReg,
                           valueOffset * sizeof(Value) + STUB_FRAME_SIZE));
    valueOffset++;

    // callee
    masm.pushValue(Address(BaselineFrameReg,
                           valueOffset * sizeof(Value) + STUB_FRAME_SIZE));
    valueOffset++;

    masm.push(masm.getStackPointer());
    masm.push(ICStubReg);

    PushStubPayload(masm, R0.scratchReg());

    using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, Value*,
                        MutableHandleValue);
    if (!callVM<Fn, DoSpreadCallFallback>(masm)) {
      return false;
    }

    leaveStubFrame(masm);
    EmitReturnFromIC(masm);

    // SpreadCall is not yet supported in Ion, so do not generate asmcode for
    // bailout.
    return true;
  }

  // Push a stub frame so that we can perform a non-tail call.
  enterStubFrame(masm, R1.scratchReg());

  regs.take(R0.scratchReg());  // argc.

  pushCallArguments(masm, regs, R0.scratchReg(), isConstructing);

  masm.push(masm.getStackPointer());
  masm.push(R0.scratchReg());
  masm.push(ICStubReg);

  PushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, uint32_t,
                      Value*, MutableHandleValue);
  if (!callVM<Fn, DoCallFallback>(masm)) {
    return false;
  }

  leaveStubFrame(masm);
  EmitReturnFromIC(masm);

  // This is the resume point used when bailout rewrites call stack to undo
  // Ion inlined frames. The return address pushed onto reconstructed stack
  // will point here.
  assumeStubFrame();

  MOZ_ASSERT(!isSpread);

  if (isConstructing) {
    code.initBailoutReturnOffset(BailoutReturnKind::New, masm.currentOffset());
  } else {
    code.initBailoutReturnOffset(BailoutReturnKind::Call, masm.currentOffset());
  }

  // Load passed-in ThisV into R1 just in case it's needed.  Need to do this
  // before we leave the stub frame since that info will be lost.
  // Current stack:  [...., ThisV, ActualArgc, CalleeToken, Descriptor ]
  masm.loadValue(Address(masm.getStackPointer(), 3 * sizeof(size_t)), R1);

  leaveStubFrame(masm, true);

  // If this is a |constructing| call, if the callee returns a non-object, we
  // replace it with the |this| object passed in.
  if (isConstructing) {
    static_assert(JSReturnOperand == R0);
    Label skipThisReplace;

    masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
    masm.moveValue(R1, R0);
#ifdef DEBUG
    masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
    masm.assumeUnreachable("Failed to return object in constructing call.");
#endif
    masm.bind(&skipThisReplace);
  }

  EmitReturnFromIC(masm);
  return true;
}

bool FallbackICCodeCompiler::emit_Call() {
  return emitCall(/* isSpread = */ false, /* isConstructing = */ false);
}

bool FallbackICCodeCompiler::emit_CallConstructing() {
  return emitCall(/* isSpread = */ false, /* isConstructing = */ true);
}

bool FallbackICCodeCompiler::emit_SpreadCall() {
  return emitCall(/* isSpread = */ true, /* isConstructing = */ false);
}

bool FallbackICCodeCompiler::emit_SpreadCallConstructing() {
  return emitCall(/* isSpread = */ true, /* isConstructing = */ true);
}

//
// GetIterator_Fallback
//

bool DoGetIteratorFallback(JSContext* cx, BaselineFrame* frame,
                           ICFallbackStub* stub, HandleValue value,
                           MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "GetIterator");

  TryAttachStub<GetIteratorIRGenerator>("GetIterator", cx, frame, stub, value);

  JSObject* iterobj = ValueToIterator(cx, value);
  if (!iterobj) {
    return false;
  }

  res.setObject(*iterobj);
  return true;
}

bool FallbackICCodeCompiler::emit_GetIterator() {
  EmitRestoreTailCallReg(masm);

  // Sync stack for the decompiler.
  masm.pushValue(R0);

  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      MutableHandleValue);
  return tailCallVM<Fn, DoGetIteratorFallback>(masm);
}

//
// OptimizeSpreadCall_Fallback
//

bool DoOptimizeSpreadCallFallback(JSContext* cx, BaselineFrame* frame,
                                  ICFallbackStub* stub, HandleValue value,
                                  MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "OptimizeSpreadCall");

  TryAttachStub<OptimizeSpreadCallIRGenerator>("OptimizeSpreadCall", cx, frame,
                                               stub, value);

  bool optimized;
  if (!OptimizeSpreadCall(cx, value, &optimized)) {
    return false;
  }

  res.setBoolean(optimized);
  return true;
}

bool FallbackICCodeCompiler::emit_OptimizeSpreadCall() {
  EmitRestoreTailCallReg(masm);

  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      MutableHandleValue);
  return tailCallVM<Fn, DoOptimizeSpreadCallFallback>(masm);
}

//
// InstanceOf_Fallback
//

bool DoInstanceOfFallback(JSContext* cx, BaselineFrame* frame,
                          ICFallbackStub* stub, HandleValue lhs,
                          HandleValue rhs, MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "InstanceOf");

  if (!rhs.isObject()) {
    ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS, -1, rhs, nullptr);
    return false;
  }

  RootedObject obj(cx, &rhs.toObject());
  bool cond = false;
  if (!HasInstance(cx, obj, lhs, &cond)) {
    return false;
  }

  res.setBoolean(cond);

  if (!obj->is<JSFunction>()) {
    // ensure we've recorded at least one failure, so we can detect there was a
    // non-optimizable case
    if (!stub->state().hasFailures()) {
      stub->trackNotAttached();
    }
    return true;
  }

  TryAttachStub<InstanceOfIRGenerator>("InstanceOf", cx, frame, stub, lhs, obj);
  return true;
}

bool FallbackICCodeCompiler::emit_InstanceOf() {
  EmitRestoreTailCallReg(masm);

  // Sync stack for the decompiler.
  masm.pushValue(R0);
  masm.pushValue(R1);

  masm.pushValue(R1);
  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      HandleValue, MutableHandleValue);
  return tailCallVM<Fn, DoInstanceOfFallback>(masm);
}

//
// TypeOf_Fallback
//

bool DoTypeOfFallback(JSContext* cx, BaselineFrame* frame, ICFallbackStub* stub,
                      HandleValue val, MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "TypeOf");

  TryAttachStub<TypeOfIRGenerator>("TypeOf", cx, frame, stub, val);

  JSType type = js::TypeOfValue(val);
  RootedString string(cx, TypeName(type, cx->names()));
  res.setString(string);
  return true;
}

bool FallbackICCodeCompiler::emit_TypeOf() {
  EmitRestoreTailCallReg(masm);

  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      MutableHandleValue);
  return tailCallVM<Fn, DoTypeOfFallback>(masm);
}

//
// ToPropertyKey_Fallback
//

bool DoToPropertyKeyFallback(JSContext* cx, BaselineFrame* frame,
                             ICFallbackStub* stub, HandleValue val,
                             MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "ToPropertyKey");

  TryAttachStub<ToPropertyKeyIRGenerator>("ToPropertyKey", cx, frame, stub,
                                          val);

  return ToPropertyKeyOperation(cx, val, res);
}

bool FallbackICCodeCompiler::emit_ToPropertyKey() {
  EmitRestoreTailCallReg(masm);

  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      MutableHandleValue);
  return tailCallVM<Fn, DoToPropertyKeyFallback>(masm);
}

//
// Rest_Fallback
//

bool DoRestFallback(JSContext* cx, BaselineFrame* frame, ICFallbackStub* stub,
                    MutableHandleValue res) {
  unsigned numFormals = frame->numFormalArgs() - 1;
  unsigned numActuals = frame->numActualArgs();
  unsigned numRest = numActuals > numFormals ? numActuals - numFormals : 0;
  Value* rest = frame->argv() + numFormals;

  ArrayObject* obj = NewDenseCopiedArray(cx, numRest, rest);
  if (!obj) {
    return false;
  }
  res.setObject(*obj);
  return true;
}

bool FallbackICCodeCompiler::emit_Rest() {
  EmitRestoreTailCallReg(masm);

  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn =
      bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, MutableHandleValue);
  return tailCallVM<Fn, DoRestFallback>(masm);
}

//
// UnaryArith_Fallback
//

bool DoUnaryArithFallback(JSContext* cx, BaselineFrame* frame,
                          ICFallbackStub* stub, HandleValue val,
                          MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  JSOp op = JSOp(*pc);
  FallbackICSpew(cx, stub, "UnaryArith(%s)", CodeName(op));

  switch (op) {
    case JSOp::BitNot: {
      res.set(val);
      if (!BitNot(cx, res, res)) {
        return false;
      }
      break;
    }
    case JSOp::Pos: {
      res.set(val);
      if (!ToNumber(cx, res)) {
        return false;
      }
      break;
    }
    case JSOp::Neg: {
      res.set(val);
      if (!NegOperation(cx, res, res)) {
        return false;
      }
      break;
    }
    case JSOp::Inc: {
      if (!IncOperation(cx, val, res)) {
        return false;
      }
      break;
    }
    case JSOp::Dec: {
      if (!DecOperation(cx, val, res)) {
        return false;
      }
      break;
    }
    case JSOp::ToNumeric: {
      res.set(val);
      if (!ToNumeric(cx, res)) {
        return false;
      }
      break;
    }
    default:
      MOZ_CRASH("Unexpected op");
  }
  MOZ_ASSERT(res.isNumeric());

  TryAttachStub<UnaryArithIRGenerator>("UnaryArith", cx, frame, stub, op, val,
                                       res);
  return true;
}

bool FallbackICCodeCompiler::emit_UnaryArith() {
  static_assert(R0 == JSReturnOperand);

  // Restore the tail call register.
  EmitRestoreTailCallReg(masm);

  // Ensure stack is fully synced for the expression decompiler.
  masm.pushValue(R0);

  // Push arguments.
  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      MutableHandleValue);
  return tailCallVM<Fn, DoUnaryArithFallback>(masm);
}

//
// BinaryArith_Fallback
//

bool DoBinaryArithFallback(JSContext* cx, BaselineFrame* frame,
                           ICFallbackStub* stub, HandleValue lhs,
                           HandleValue rhs, MutableHandleValue ret) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  JSOp op = JSOp(*pc);
  FallbackICSpew(
      cx, stub, "CacheIRBinaryArith(%s,%d,%d)", CodeName(op),
      int(lhs.isDouble() ? JSVAL_TYPE_DOUBLE : lhs.extractNonDoubleType()),
      int(rhs.isDouble() ? JSVAL_TYPE_DOUBLE : rhs.extractNonDoubleType()));

  // Don't pass lhs/rhs directly, we need the original values when
  // generating stubs.
  RootedValue lhsCopy(cx, lhs);
  RootedValue rhsCopy(cx, rhs);

  // Perform the arith operation.
  switch (op) {
    case JSOp::Add:
      // Do an add.
      if (!AddValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Sub:
      if (!SubValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Mul:
      if (!MulValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Div:
      if (!DivValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Mod:
      if (!ModValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Pow:
      if (!PowValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::BitOr: {
      if (!BitOr(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::BitXor: {
      if (!BitXor(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::BitAnd: {
      if (!BitAnd(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::Lsh: {
      if (!BitLsh(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::Rsh: {
      if (!BitRsh(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::Ursh: {
      if (!UrshValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    default:
      MOZ_CRASH("Unhandled baseline arith op");
  }

  TryAttachStub<BinaryArithIRGenerator>("BinaryArith", cx, frame, stub, op, lhs,
                                        rhs, ret);
  return true;
}

bool FallbackICCodeCompiler::emit_BinaryArith() {
  static_assert(R0 == JSReturnOperand);

  // Restore the tail call register.
  EmitRestoreTailCallReg(masm);

  // Ensure stack is fully synced for the expression decompiler.
  masm.pushValue(R0);
  masm.pushValue(R1);

  // Push arguments.
  masm.pushValue(R1);
  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      HandleValue, MutableHandleValue);
  return tailCallVM<Fn, DoBinaryArithFallback>(masm);
}

//
// Compare_Fallback
//
bool DoCompareFallback(JSContext* cx, BaselineFrame* frame,
                       ICFallbackStub* stub, HandleValue lhs, HandleValue rhs,
                       MutableHandleValue ret) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);
  JSOp op = JSOp(*pc);

  FallbackICSpew(cx, stub, "Compare(%s)", CodeName(op));

  // Don't pass lhs/rhs directly, we need the original values when
  // generating stubs.
  RootedValue lhsCopy(cx, lhs);
  RootedValue rhsCopy(cx, rhs);

  // Perform the compare operation.
  bool out;
  switch (op) {
    case JSOp::Lt:
      if (!LessThan(cx, &lhsCopy, &rhsCopy, &out)) {
        return false;
      }
      break;
    case JSOp::Le:
      if (!LessThanOrEqual(cx, &lhsCopy, &rhsCopy, &out)) {
        return false;
      }
      break;
    case JSOp::Gt:
      if (!GreaterThan(cx, &lhsCopy, &rhsCopy, &out)) {
        return false;
      }
      break;
    case JSOp::Ge:
      if (!GreaterThanOrEqual(cx, &lhsCopy, &rhsCopy, &out)) {
        return false;
      }
      break;
    case JSOp::Eq:
      if (!js::LooselyEqual(cx, lhsCopy, rhsCopy, &out)) {
        return false;
      }
      break;
    case JSOp::Ne:
      if (!js::LooselyEqual(cx, lhsCopy, rhsCopy, &out)) {
        return false;
      }
      out = !out;
      break;
    case JSOp::StrictEq:
      if (!js::StrictlyEqual(cx, lhsCopy, rhsCopy, &out)) {
        return false;
      }
      break;
    case JSOp::StrictNe:
      if (!js::StrictlyEqual(cx, lhsCopy, rhsCopy, &out)) {
        return false;
      }
      out = !out;
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Unhandled baseline compare op");
      return false;
  }

  ret.setBoolean(out);

  TryAttachStub<CompareIRGenerator>("Compare", cx, frame, stub, op, lhs, rhs);
  return true;
}

bool FallbackICCodeCompiler::emit_Compare() {
  static_assert(R0 == JSReturnOperand);

  // Restore the tail call register.
  EmitRestoreTailCallReg(masm);

  // Ensure stack is fully synced for the expression decompiler.
  masm.pushValue(R0);
  masm.pushValue(R1);

  // Push arguments.
  masm.pushValue(R1);
  masm.pushValue(R0);
  masm.push(ICStubReg);
  pushStubPayload(masm, R0.scratchReg());

  using Fn = bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, HandleValue,
                      HandleValue, MutableHandleValue);
  return tailCallVM<Fn, DoCompareFallback>(masm);
}

//
// NewArray_Fallback
//

bool DoNewArrayFallback(JSContext* cx, BaselineFrame* frame,
                        ICFallbackStub* stub, MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "NewArray");

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);

  uint32_t length = GET_UINT32(pc);
  MOZ_ASSERT(length <= INT32_MAX,
             "the bytecode emitter must fail to compile code that would "
             "produce a length exceeding int32_t range");

  RootedArrayObject array(cx, NewArrayOperation(cx, length));
  if (!array) {
    return false;
  }

  TryAttachStub<NewArrayIRGenerator>("NewArray", cx, frame, stub, JSOp(*pc),
                                     array, frame);

  res.setObject(*array);
  return true;
}

bool FallbackICCodeCompiler::emit_NewArray() {
  EmitRestoreTailCallReg(masm);

  masm.push(ICStubReg);  // stub.
  masm.pushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

  using Fn =
      bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, MutableHandleValue);
  return tailCallVM<Fn, DoNewArrayFallback>(masm);
}

//
// NewObject_Fallback
//
bool DoNewObjectFallback(JSContext* cx, BaselineFrame* frame,
                         ICFallbackStub* stub, MutableHandleValue res) {
  stub->incrementEnteredCount();
  MaybeNotifyWarp(frame->outerScript(), stub);
  FallbackICSpew(cx, stub, "NewObject");

  RootedScript script(cx, frame->script());
  jsbytecode* pc = stub->pc(script);

  RootedObject obj(cx, NewObjectOperation(cx, script, pc));
  if (!obj) {
    return false;
  }

  TryAttachStub<NewObjectIRGenerator>("NewObject", cx, frame, stub, JSOp(*pc),
                                      obj, frame);

  res.setObject(*obj);
  return true;
}

bool FallbackICCodeCompiler::emit_NewObject() {
  EmitRestoreTailCallReg(masm);

  masm.push(ICStubReg);  // stub.
  pushStubPayload(masm, R0.scratchReg());

  using Fn =
      bool (*)(JSContext*, BaselineFrame*, ICFallbackStub*, MutableHandleValue);
  return tailCallVM<Fn, DoNewObjectFallback>(masm);
}

bool JitRuntime::generateBaselineICFallbackCode(JSContext* cx) {
  StackMacroAssembler masm;

  BaselineICFallbackCode& fallbackCode = baselineICFallbackCode_.ref();
  FallbackICCodeCompiler compiler(cx, fallbackCode, masm);

  JitSpew(JitSpew_Codegen, "# Emitting Baseline IC fallback code");

#define EMIT_CODE(kind)                                            \
  {                                                                \
    uint32_t offset = startTrampolineCode(masm);                   \
    InitMacroAssemblerForICStub(masm);                             \
    if (!compiler.emit_##kind()) {                                 \
      return false;                                                \
    }                                                              \
    fallbackCode.initOffset(BaselineICFallbackKind::kind, offset); \
  }
  IC_BASELINE_FALLBACK_CODE_KIND_LIST(EMIT_CODE)
#undef EMIT_CODE

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);
  if (!code) {
    return false;
  }

#ifdef JS_ION_PERF
  writePerfSpewerJitCodeProfile(code, "BaselineICFallback");
#endif
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, "BaselineICFallback");
#endif

  fallbackCode.initCode(code);
  return true;
}

}  // namespace jit
}  // namespace js
