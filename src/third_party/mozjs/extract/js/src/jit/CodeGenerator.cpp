/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CodeGenerator.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/EnumSet.h"
#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/Latin1.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/SIMD.h"

#include <limits>
#include <type_traits>
#include <utility>

#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"

#include "builtin/MapObject.h"
#include "builtin/RegExp.h"
#include "builtin/String.h"
#include "irregexp/RegExpTypes.h"
#include "jit/ABIArgGenerator.h"
#include "jit/CompileInfo.h"
#include "jit/InlineScriptTree.h"
#include "jit/Invalidation.h"
#include "jit/IonGenericCallStub.h"
#include "jit/IonIC.h"
#include "jit/IonScript.h"
#include "jit/JitcodeMap.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/Linker.h"
#include "jit/MIRGenerator.h"
#include "jit/MoveEmitter.h"
#include "jit/RangeAnalysis.h"
#include "jit/RegExpStubConstants.h"
#include "jit/SafepointIndex.h"
#include "jit/SharedICHelpers.h"
#include "jit/SharedICRegisters.h"
#include "jit/VMFunctions.h"
#include "jit/WarpSnapshot.h"
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin
#include "js/experimental/JitInfo.h"  // JSJit{Getter,Setter}CallArgs, JSJitMethodCallArgsTraits, JSJitInfo
#include "js/friend/DOMProxy.h"  // JS::ExpandoAndGeneration
#include "js/RegExpFlags.h"      // JS::RegExpFlag
#include "js/ScalarType.h"       // js::Scalar::Type
#include "proxy/DOMProxy.h"
#include "proxy/ScriptedProxyHandler.h"
#include "util/CheckedArithmetic.h"
#include "util/DifferentialTesting.h"
#include "util/Unicode.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BuiltinObjectKind.h"
#include "vm/ConstantCompareOperand.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/Interpreter.h"
#include "vm/JSAtomUtils.h"  // AtomizeString
#include "vm/MatchPairs.h"
#include "vm/RegExpObject.h"
#include "vm/RegExpStatics.h"
#include "vm/StaticStrings.h"
#include "vm/StringObject.h"
#include "vm/StringType.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmCodegenConstants.h"
#include "wasm/WasmPI.h"
#include "wasm/WasmValType.h"
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif
#include "wasm/WasmBinary.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmStubs.h"

#include "builtin/Boolean-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"
#include "jit/TemplateObject-inl.h"
#include "jit/VMFunctionList-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/JSScript-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::CheckedUint32;
using mozilla::DebugOnly;
using mozilla::FloatingPoint;
using mozilla::NegativeInfinity;
using mozilla::PositiveInfinity;

using JS::ExpandoAndGeneration;

namespace js {
namespace jit {

#ifdef CHECK_OSIPOINT_REGISTERS
template <class Op>
static void HandleRegisterDump(Op op, MacroAssembler& masm,
                               LiveRegisterSet liveRegs, Register activation,
                               Register scratch) {
  const size_t baseOffset = JitActivation::offsetOfRegs();

  // Handle live GPRs.
  for (GeneralRegisterIterator iter(liveRegs.gprs()); iter.more(); ++iter) {
    Register reg = *iter;
    Address dump(activation, baseOffset + RegisterDump::offsetOfRegister(reg));

    if (reg == activation) {
      // To use the original value of the activation register (that's
      // now on top of the stack), we need the scratch register.
      masm.push(scratch);
      masm.loadPtr(Address(masm.getStackPointer(), sizeof(uintptr_t)), scratch);
      op(scratch, dump);
      masm.pop(scratch);
    } else {
      op(reg, dump);
    }
  }

  // Handle live FPRs.
  for (FloatRegisterIterator iter(liveRegs.fpus()); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    Address dump(activation, baseOffset + RegisterDump::offsetOfRegister(reg));
    op(reg, dump);
  }
}

class StoreOp {
  MacroAssembler& masm;

 public:
  explicit StoreOp(MacroAssembler& masm) : masm(masm) {}

  void operator()(Register reg, Address dump) { masm.storePtr(reg, dump); }
  void operator()(FloatRegister reg, Address dump) {
    if (reg.isDouble()) {
      masm.storeDouble(reg, dump);
    } else if (reg.isSingle()) {
      masm.storeFloat32(reg, dump);
    } else if (reg.isSimd128()) {
      MOZ_CRASH("Unexpected case for SIMD");
    } else {
      MOZ_CRASH("Unexpected register type.");
    }
  }
};

class VerifyOp {
  MacroAssembler& masm;
  Label* failure_;

 public:
  VerifyOp(MacroAssembler& masm, Label* failure)
      : masm(masm), failure_(failure) {}

  void operator()(Register reg, Address dump) {
    masm.branchPtr(Assembler::NotEqual, dump, reg, failure_);
  }
  void operator()(FloatRegister reg, Address dump) {
    if (reg.isDouble()) {
      ScratchDoubleScope scratch(masm);
      masm.loadDouble(dump, scratch);
      masm.branchDouble(Assembler::DoubleNotEqual, scratch, reg, failure_);
    } else if (reg.isSingle()) {
      ScratchFloat32Scope scratch(masm);
      masm.loadFloat32(dump, scratch);
      masm.branchFloat(Assembler::DoubleNotEqual, scratch, reg, failure_);
    } else if (reg.isSimd128()) {
      MOZ_CRASH("Unexpected case for SIMD");
    } else {
      MOZ_CRASH("Unexpected register type.");
    }
  }
};

void CodeGenerator::verifyOsiPointRegs(LSafepoint* safepoint) {
  // Ensure the live registers stored by callVM did not change between
  // the call and this OsiPoint. Try-catch relies on this invariant.

  // Load pointer to the JitActivation in a scratch register.
  AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
  Register scratch = allRegs.takeAny();
  masm.push(scratch);
  masm.loadJitActivation(scratch);

  // If we should not check registers (because the instruction did not call
  // into the VM, or a GC happened), we're done.
  Label failure, done;
  Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
  masm.branch32(Assembler::Equal, checkRegs, Imm32(0), &done);

  // Having more than one VM function call made in one visit function at
  // runtime is a sec-ciritcal error, because if we conservatively assume that
  // one of the function call can re-enter Ion, then the invalidation process
  // will potentially add a call at a random location, by patching the code
  // before the return address.
  masm.branch32(Assembler::NotEqual, checkRegs, Imm32(1), &failure);

  // Set checkRegs to 0, so that we don't try to verify registers after we
  // return from this script to the caller.
  masm.store32(Imm32(0), checkRegs);

  // Ignore clobbered registers. Some instructions (like LValueToInt32) modify
  // temps after calling into the VM. This is fine because no other
  // instructions (including this OsiPoint) will depend on them. Also
  // backtracking can also use the same register for an input and an output.
  // These are marked as clobbered and shouldn't get checked.
  LiveRegisterSet liveRegs;
  liveRegs.set() = RegisterSet::Intersect(
      safepoint->liveRegs().set(),
      RegisterSet::Not(safepoint->clobberedRegs().set()));

  VerifyOp op(masm, &failure);
  HandleRegisterDump<VerifyOp>(op, masm, liveRegs, scratch, allRegs.getAny());

  masm.jump(&done);

  // Do not profile the callWithABI that occurs below.  This is to avoid a
  // rare corner case that occurs when profiling interacts with itself:
  //
  // When slow profiling assertions are turned on, FunctionBoundary ops
  // (which update the profiler pseudo-stack) may emit a callVM, which
  // forces them to have an osi point associated with them.  The
  // FunctionBoundary for inline function entry is added to the caller's
  // graph with a PC from the caller's code, but during codegen it modifies
  // Gecko Profiler instrumentation to add the callee as the current top-most
  // script. When codegen gets to the OSIPoint, and the callWithABI below is
  // emitted, the codegen thinks that the current frame is the callee, but
  // the PC it's using from the OSIPoint refers to the caller.  This causes
  // the profiler instrumentation of the callWithABI below to ASSERT, since
  // the script and pc are mismatched.  To avoid this, we simply omit
  // instrumentation for these callWithABIs.

  // Any live register captured by a safepoint (other than temp registers)
  // must remain unchanged between the call and the OsiPoint instruction.
  masm.bind(&failure);
  masm.assumeUnreachable("Modified registers between VM call and OsiPoint");

  masm.bind(&done);
  masm.pop(scratch);
}

bool CodeGenerator::shouldVerifyOsiPointRegs(LSafepoint* safepoint) {
  if (!checkOsiPointRegisters) {
    return false;
  }

  if (safepoint->liveRegs().emptyGeneral() &&
      safepoint->liveRegs().emptyFloat()) {
    return false;  // No registers to check.
  }

  return true;
}

void CodeGenerator::resetOsiPointRegs(LSafepoint* safepoint) {
  if (!shouldVerifyOsiPointRegs(safepoint)) {
    return;
  }

  // Set checkRegs to 0. If we perform a VM call, the instruction
  // will set it to 1.
  AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
  Register scratch = allRegs.takeAny();
  masm.push(scratch);
  masm.loadJitActivation(scratch);
  Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
  masm.store32(Imm32(0), checkRegs);
  masm.pop(scratch);
}

static void StoreAllLiveRegs(MacroAssembler& masm, LiveRegisterSet liveRegs) {
  // Store a copy of all live registers before performing the call.
  // When we reach the OsiPoint, we can use this to check nothing
  // modified them in the meantime.

  // Load pointer to the JitActivation in a scratch register.
  AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
  Register scratch = allRegs.takeAny();
  masm.push(scratch);
  masm.loadJitActivation(scratch);

  Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
  masm.add32(Imm32(1), checkRegs);

  StoreOp op(masm);
  HandleRegisterDump<StoreOp>(op, masm, liveRegs, scratch, allRegs.getAny());

  masm.pop(scratch);
}
#endif  // CHECK_OSIPOINT_REGISTERS

// Before doing any call to Cpp, you should ensure that volatile
// registers are evicted by the register allocator.
void CodeGenerator::callVMInternal(VMFunctionId id, LInstruction* ins) {
  TrampolinePtr code = gen->jitRuntime()->getVMWrapper(id);
  const VMFunctionData& fun = GetVMFunction(id);

  // Stack is:
  //    ... frame ...
  //    [args]
#ifdef DEBUG
  MOZ_ASSERT(pushedArgs_ == fun.explicitArgs);
  pushedArgs_ = 0;
#endif

#ifdef CHECK_OSIPOINT_REGISTERS
  if (shouldVerifyOsiPointRegs(ins->safepoint())) {
    StoreAllLiveRegs(masm, ins->safepoint()->liveRegs());
  }
#endif

#ifdef DEBUG
  if (ins->mirRaw()) {
    MOZ_ASSERT(ins->mirRaw()->isInstruction());
    MInstruction* mir = ins->mirRaw()->toInstruction();
    MOZ_ASSERT_IF(mir->needsResumePoint(), mir->resumePoint());

    // If this MIR instruction has an overridden AliasSet, set the JitRuntime's
    // disallowArbitraryCode_ flag so we can assert this VMFunction doesn't call
    // RunScript. Whitelist MInterruptCheck and MCheckOverRecursed because
    // interrupt callbacks can call JS (chrome JS or shell testing functions).
    bool isWhitelisted = mir->isInterruptCheck() || mir->isCheckOverRecursed();
    if (!mir->hasDefaultAliasSet() && !isWhitelisted) {
      const void* addr = gen->jitRuntime()->addressOfDisallowArbitraryCode();
      masm.move32(Imm32(1), ReturnReg);
      masm.store32(ReturnReg, AbsoluteAddress(addr));
    }
  }
#endif

  // Push an exit frame descriptor.
  masm.PushFrameDescriptor(FrameType::IonJS);

  // Call the wrapper function.  The wrapper is in charge to unwind the stack
  // when returning from the call.  Failures are handled with exceptions based
  // on the return value of the C functions.  To guard the outcome of the
  // returned value, use another LIR instruction.
  ensureOsiSpace();
  uint32_t callOffset = masm.callJit(code);
  markSafepointAt(callOffset, ins);

#ifdef DEBUG
  // Reset the disallowArbitraryCode flag after the call.
  {
    const void* addr = gen->jitRuntime()->addressOfDisallowArbitraryCode();
    masm.push(ReturnReg);
    masm.move32(Imm32(0), ReturnReg);
    masm.store32(ReturnReg, AbsoluteAddress(addr));
    masm.pop(ReturnReg);
  }
#endif

  // Pop rest of the exit frame and the arguments left on the stack.
  int framePop =
      sizeof(ExitFrameLayout) - ExitFrameLayout::bytesPoppedAfterCall();
  masm.implicitPop(fun.explicitStackSlots() * sizeof(void*) + framePop);

  // Stack is:
  //    ... frame ...
}

template <typename Fn, Fn fn>
void CodeGenerator::callVM(LInstruction* ins) {
  VMFunctionId id = VMFunctionToId<Fn, fn>::id;
  callVMInternal(id, ins);
}

// ArgSeq store arguments for OutOfLineCallVM.
//
// OutOfLineCallVM are created with "oolCallVM" function. The third argument of
// this function is an instance of a class which provides a "generate" in charge
// of pushing the argument, with "pushArg", for a VMFunction.
//
// Such list of arguments can be created by using the "ArgList" function which
// creates one instance of "ArgSeq", where the type of the arguments are
// inferred from the type of the arguments.
//
// The list of arguments must be written in the same order as if you were
// calling the function in C++.
//
// Example:
//   ArgList(ToRegister(lir->lhs()), ToRegister(lir->rhs()))

template <typename... ArgTypes>
class ArgSeq {
  std::tuple<std::remove_reference_t<ArgTypes>...> args_;

  template <std::size_t... ISeq>
  inline void generate(CodeGenerator* codegen,
                       std::index_sequence<ISeq...>) const {
    // Arguments are pushed in reverse order, from last argument to first
    // argument.
    (codegen->pushArg(std::get<sizeof...(ISeq) - 1 - ISeq>(args_)), ...);
  }

 public:
  explicit ArgSeq(ArgTypes&&... args)
      : args_(std::forward<ArgTypes>(args)...) {}

  inline void generate(CodeGenerator* codegen) const {
    generate(codegen, std::index_sequence_for<ArgTypes...>{});
  }

#ifdef DEBUG
  static constexpr size_t numArgs = sizeof...(ArgTypes);
#endif
};

template <typename... ArgTypes>
inline ArgSeq<ArgTypes...> ArgList(ArgTypes&&... args) {
  return ArgSeq<ArgTypes...>(std::forward<ArgTypes>(args)...);
}

// Store wrappers, to generate the right move of data after the VM call.

struct StoreNothing {
  inline void generate(CodeGenerator* codegen) const {}
  inline LiveRegisterSet clobbered() const {
    return LiveRegisterSet();  // No register gets clobbered
  }
};

class StoreRegisterTo {
 private:
  Register out_;

 public:
  explicit StoreRegisterTo(Register out) : out_(out) {}

  inline void generate(CodeGenerator* codegen) const {
    // It's okay to use storePointerResultTo here - the VMFunction wrapper
    // ensures the upper bytes are zero for bool/int32 return values.
    codegen->storePointerResultTo(out_);
  }
  inline LiveRegisterSet clobbered() const {
    LiveRegisterSet set;
    set.add(out_);
    return set;
  }
};

class StoreFloatRegisterTo {
 private:
  FloatRegister out_;

 public:
  explicit StoreFloatRegisterTo(FloatRegister out) : out_(out) {}

  inline void generate(CodeGenerator* codegen) const {
    codegen->storeFloatResultTo(out_);
  }
  inline LiveRegisterSet clobbered() const {
    LiveRegisterSet set;
    set.add(out_);
    return set;
  }
};

template <typename Output>
class StoreValueTo_ {
 private:
  Output out_;

 public:
  explicit StoreValueTo_(const Output& out) : out_(out) {}

  inline void generate(CodeGenerator* codegen) const {
    codegen->storeResultValueTo(out_);
  }
  inline LiveRegisterSet clobbered() const {
    LiveRegisterSet set;
    set.add(out_);
    return set;
  }
};

template <typename Output>
StoreValueTo_<Output> StoreValueTo(const Output& out) {
  return StoreValueTo_<Output>(out);
}

template <typename Fn, Fn fn, class ArgSeq, class StoreOutputTo>
class OutOfLineCallVM : public OutOfLineCodeBase<CodeGenerator> {
 private:
  LInstruction* lir_;
  ArgSeq args_;
  StoreOutputTo out_;

 public:
  OutOfLineCallVM(LInstruction* lir, const ArgSeq& args,
                  const StoreOutputTo& out)
      : lir_(lir), args_(args), out_(out) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineCallVM(this);
  }

  LInstruction* lir() const { return lir_; }
  const ArgSeq& args() const { return args_; }
  const StoreOutputTo& out() const { return out_; }
};

template <typename Fn, Fn fn, class ArgSeq, class StoreOutputTo>
OutOfLineCode* CodeGenerator::oolCallVM(LInstruction* lir, const ArgSeq& args,
                                        const StoreOutputTo& out) {
  MOZ_ASSERT(lir->mirRaw());
  MOZ_ASSERT(lir->mirRaw()->isInstruction());

#ifdef DEBUG
  VMFunctionId id = VMFunctionToId<Fn, fn>::id;
  const VMFunctionData& fun = GetVMFunction(id);
  MOZ_ASSERT(fun.explicitArgs == args.numArgs);
  MOZ_ASSERT(fun.returnsData() !=
             (std::is_same_v<StoreOutputTo, StoreNothing>));
#endif

  OutOfLineCode* ool = new (alloc())
      OutOfLineCallVM<Fn, fn, ArgSeq, StoreOutputTo>(lir, args, out);
  addOutOfLineCode(ool, lir->mirRaw()->toInstruction());
  return ool;
}

template <typename Fn, Fn fn, class ArgSeq, class StoreOutputTo>
void CodeGenerator::visitOutOfLineCallVM(
    OutOfLineCallVM<Fn, fn, ArgSeq, StoreOutputTo>* ool) {
  LInstruction* lir = ool->lir();

#ifdef JS_JITSPEW
  JitSpewStart(JitSpew_Codegen, "                                # LIR=%s",
               lir->opName());
  if (const char* extra = lir->getExtraName()) {
    JitSpewCont(JitSpew_Codegen, ":%s", extra);
  }
  JitSpewFin(JitSpew_Codegen);
#endif
  perfSpewer_.recordInstruction(masm, lir);
  saveLive(lir);
  ool->args().generate(this);
  callVM<Fn, fn>(lir);
  ool->out().generate(this);
  restoreLiveIgnore(lir, ool->out().clobbered());
  masm.jump(ool->rejoin());
}

class OutOfLineICFallback : public OutOfLineCodeBase<CodeGenerator> {
 private:
  LInstruction* lir_;
  size_t cacheIndex_;
  size_t cacheInfoIndex_;

 public:
  OutOfLineICFallback(LInstruction* lir, size_t cacheIndex,
                      size_t cacheInfoIndex)
      : lir_(lir), cacheIndex_(cacheIndex), cacheInfoIndex_(cacheInfoIndex) {}

  void bind(MacroAssembler* masm) override {
    // The binding of the initial jump is done in
    // CodeGenerator::visitOutOfLineICFallback.
  }

  size_t cacheIndex() const { return cacheIndex_; }
  size_t cacheInfoIndex() const { return cacheInfoIndex_; }
  LInstruction* lir() const { return lir_; }

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineICFallback(this);
  }
};

void CodeGeneratorShared::addIC(LInstruction* lir, size_t cacheIndex) {
  if (cacheIndex == SIZE_MAX) {
    masm.setOOM();
    return;
  }

  DataPtr<IonIC> cache(this, cacheIndex);
  MInstruction* mir = lir->mirRaw()->toInstruction();
  cache->setScriptedLocation(mir->block()->info().script(),
                             mir->resumePoint()->pc());

  Register temp = cache->scratchRegisterForEntryJump();
  icInfo_.back().icOffsetForJump = masm.movWithPatch(ImmWord(-1), temp);
  masm.jump(Address(temp, 0));

  MOZ_ASSERT(!icInfo_.empty());

  OutOfLineICFallback* ool =
      new (alloc()) OutOfLineICFallback(lir, cacheIndex, icInfo_.length() - 1);
  addOutOfLineCode(ool, mir);

  masm.bind(ool->rejoin());
  cache->setRejoinOffset(CodeOffset(ool->rejoin()->offset()));
}

void CodeGenerator::visitOutOfLineICFallback(OutOfLineICFallback* ool) {
  LInstruction* lir = ool->lir();
  size_t cacheIndex = ool->cacheIndex();
  size_t cacheInfoIndex = ool->cacheInfoIndex();

  DataPtr<IonIC> ic(this, cacheIndex);

  // Register the location of the OOL path in the IC.
  ic->setFallbackOffset(CodeOffset(masm.currentOffset()));

  switch (ic->kind()) {
    case CacheKind::GetProp:
    case CacheKind::GetElem: {
      IonGetPropertyIC* getPropIC = ic->asGetPropertyIC();

      saveLive(lir);

      pushArg(getPropIC->id());
      pushArg(getPropIC->value());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonGetPropertyIC*,
                          HandleValue, HandleValue, MutableHandleValue);
      callVM<Fn, IonGetPropertyIC::update>(lir);

      StoreValueTo(getPropIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreValueTo(getPropIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::GetPropSuper:
    case CacheKind::GetElemSuper: {
      IonGetPropSuperIC* getPropSuperIC = ic->asGetPropSuperIC();

      saveLive(lir);

      pushArg(getPropSuperIC->id());
      pushArg(getPropSuperIC->receiver());
      pushArg(getPropSuperIC->object());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn =
          bool (*)(JSContext*, HandleScript, IonGetPropSuperIC*, HandleObject,
                   HandleValue, HandleValue, MutableHandleValue);
      callVM<Fn, IonGetPropSuperIC::update>(lir);

      StoreValueTo(getPropSuperIC->output()).generate(this);
      restoreLiveIgnore(lir,
                        StoreValueTo(getPropSuperIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::SetProp:
    case CacheKind::SetElem: {
      IonSetPropertyIC* setPropIC = ic->asSetPropertyIC();

      saveLive(lir);

      pushArg(setPropIC->rhs());
      pushArg(setPropIC->id());
      pushArg(setPropIC->object());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonSetPropertyIC*,
                          HandleObject, HandleValue, HandleValue);
      callVM<Fn, IonSetPropertyIC::update>(lir);

      restoreLive(lir);

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::GetName: {
      IonGetNameIC* getNameIC = ic->asGetNameIC();

      saveLive(lir);

      pushArg(getNameIC->environment());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonGetNameIC*, HandleObject,
                          MutableHandleValue);
      callVM<Fn, IonGetNameIC::update>(lir);

      StoreValueTo(getNameIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreValueTo(getNameIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::BindName: {
      IonBindNameIC* bindNameIC = ic->asBindNameIC();

      saveLive(lir);

      pushArg(bindNameIC->environment());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn =
          JSObject* (*)(JSContext*, HandleScript, IonBindNameIC*, HandleObject);
      callVM<Fn, IonBindNameIC::update>(lir);

      StoreRegisterTo(bindNameIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreRegisterTo(bindNameIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::GetIterator: {
      IonGetIteratorIC* getIteratorIC = ic->asGetIteratorIC();

      saveLive(lir);

      pushArg(getIteratorIC->value());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = JSObject* (*)(JSContext*, HandleScript, IonGetIteratorIC*,
                               HandleValue);
      callVM<Fn, IonGetIteratorIC::update>(lir);

      StoreRegisterTo(getIteratorIC->output()).generate(this);
      restoreLiveIgnore(lir,
                        StoreRegisterTo(getIteratorIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::OptimizeSpreadCall: {
      auto* optimizeSpreadCallIC = ic->asOptimizeSpreadCallIC();

      saveLive(lir);

      pushArg(optimizeSpreadCallIC->value());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonOptimizeSpreadCallIC*,
                          HandleValue, MutableHandleValue);
      callVM<Fn, IonOptimizeSpreadCallIC::update>(lir);

      StoreValueTo(optimizeSpreadCallIC->output()).generate(this);
      restoreLiveIgnore(
          lir, StoreValueTo(optimizeSpreadCallIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::In: {
      IonInIC* inIC = ic->asInIC();

      saveLive(lir);

      pushArg(inIC->object());
      pushArg(inIC->key());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonInIC*, HandleValue,
                          HandleObject, bool*);
      callVM<Fn, IonInIC::update>(lir);

      StoreRegisterTo(inIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreRegisterTo(inIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::HasOwn: {
      IonHasOwnIC* hasOwnIC = ic->asHasOwnIC();

      saveLive(lir);

      pushArg(hasOwnIC->id());
      pushArg(hasOwnIC->value());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonHasOwnIC*, HandleValue,
                          HandleValue, int32_t*);
      callVM<Fn, IonHasOwnIC::update>(lir);

      StoreRegisterTo(hasOwnIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreRegisterTo(hasOwnIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::CheckPrivateField: {
      IonCheckPrivateFieldIC* checkPrivateFieldIC = ic->asCheckPrivateFieldIC();

      saveLive(lir);

      pushArg(checkPrivateFieldIC->id());
      pushArg(checkPrivateFieldIC->value());

      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonCheckPrivateFieldIC*,
                          HandleValue, HandleValue, bool*);
      callVM<Fn, IonCheckPrivateFieldIC::update>(lir);

      StoreRegisterTo(checkPrivateFieldIC->output()).generate(this);
      restoreLiveIgnore(
          lir, StoreRegisterTo(checkPrivateFieldIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::InstanceOf: {
      IonInstanceOfIC* hasInstanceOfIC = ic->asInstanceOfIC();

      saveLive(lir);

      pushArg(hasInstanceOfIC->rhs());
      pushArg(hasInstanceOfIC->lhs());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonInstanceOfIC*,
                          HandleValue lhs, HandleObject rhs, bool* res);
      callVM<Fn, IonInstanceOfIC::update>(lir);

      StoreRegisterTo(hasInstanceOfIC->output()).generate(this);
      restoreLiveIgnore(lir,
                        StoreRegisterTo(hasInstanceOfIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::UnaryArith: {
      IonUnaryArithIC* unaryArithIC = ic->asUnaryArithIC();

      saveLive(lir);

      pushArg(unaryArithIC->input());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext* cx, HandleScript outerScript,
                          IonUnaryArithIC* stub, HandleValue val,
                          MutableHandleValue res);
      callVM<Fn, IonUnaryArithIC::update>(lir);

      StoreValueTo(unaryArithIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreValueTo(unaryArithIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::ToPropertyKey: {
      IonToPropertyKeyIC* toPropertyKeyIC = ic->asToPropertyKeyIC();

      saveLive(lir);

      pushArg(toPropertyKeyIC->input());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext* cx, HandleScript outerScript,
                          IonToPropertyKeyIC* ic, HandleValue val,
                          MutableHandleValue res);
      callVM<Fn, IonToPropertyKeyIC::update>(lir);

      StoreValueTo(toPropertyKeyIC->output()).generate(this);
      restoreLiveIgnore(lir,
                        StoreValueTo(toPropertyKeyIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::BinaryArith: {
      IonBinaryArithIC* binaryArithIC = ic->asBinaryArithIC();

      saveLive(lir);

      pushArg(binaryArithIC->rhs());
      pushArg(binaryArithIC->lhs());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext* cx, HandleScript outerScript,
                          IonBinaryArithIC* stub, HandleValue lhs,
                          HandleValue rhs, MutableHandleValue res);
      callVM<Fn, IonBinaryArithIC::update>(lir);

      StoreValueTo(binaryArithIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreValueTo(binaryArithIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::Compare: {
      IonCompareIC* compareIC = ic->asCompareIC();

      saveLive(lir);

      pushArg(compareIC->rhs());
      pushArg(compareIC->lhs());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn =
          bool (*)(JSContext* cx, HandleScript outerScript, IonCompareIC* stub,
                   HandleValue lhs, HandleValue rhs, bool* res);
      callVM<Fn, IonCompareIC::update>(lir);

      StoreRegisterTo(compareIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreRegisterTo(compareIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::CloseIter: {
      IonCloseIterIC* closeIterIC = ic->asCloseIterIC();

      saveLive(lir);

      pushArg(closeIterIC->iter());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn =
          bool (*)(JSContext*, HandleScript, IonCloseIterIC*, HandleObject);
      callVM<Fn, IonCloseIterIC::update>(lir);

      restoreLive(lir);

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::OptimizeGetIterator: {
      auto* optimizeGetIteratorIC = ic->asOptimizeGetIteratorIC();

      saveLive(lir);

      pushArg(optimizeGetIteratorIC->value());
      icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
      pushArg(ImmGCPtr(gen->outerInfo().script()));

      using Fn = bool (*)(JSContext*, HandleScript, IonOptimizeGetIteratorIC*,
                          HandleValue, bool* res);
      callVM<Fn, IonOptimizeGetIteratorIC::update>(lir);

      StoreRegisterTo(optimizeGetIteratorIC->output()).generate(this);
      restoreLiveIgnore(
          lir, StoreRegisterTo(optimizeGetIteratorIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::Call:
    case CacheKind::TypeOf:
    case CacheKind::TypeOfEq:
    case CacheKind::ToBool:
    case CacheKind::LazyConstant:
    case CacheKind::NewArray:
    case CacheKind::NewObject:
    case CacheKind::Lambda:
    case CacheKind::GetImport:
      MOZ_CRASH("Unsupported IC");
  }
  MOZ_CRASH();
}

StringObject* MNewStringObject::templateObj() const {
  return &templateObj_->as<StringObject>();
}

CodeGenerator::CodeGenerator(MIRGenerator* gen, LIRGraph* graph,
                             MacroAssembler* masm,
                             const wasm::CodeMetadata* wasmCodeMeta)
    : CodeGeneratorSpecific(gen, graph, masm, wasmCodeMeta),
      ionScriptLabels_(gen->alloc()),
      ionNurseryObjectLabels_(gen->alloc()),
      scriptCounts_(nullptr) {}

CodeGenerator::~CodeGenerator() { js_delete(scriptCounts_); }

void CodeGenerator::visitValueToNumberInt32(LValueToNumberInt32* lir) {
  ValueOperand operand = ToValue(lir->input());
  Register output = ToRegister(lir->output());
  FloatRegister temp = ToFloatRegister(lir->temp0());

  Label fails;
  masm.convertValueToInt32(operand, temp, output, &fails,
                           lir->mir()->needsNegativeZeroCheck(),
                           lir->mir()->conversion());

  bailoutFrom(&fails, lir->snapshot());
}

void CodeGenerator::visitValueTruncateToInt32(LValueTruncateToInt32* lir) {
  ValueOperand operand = ToValue(lir->input());
  Register output = ToRegister(lir->output());
  FloatRegister temp = ToFloatRegister(lir->temp0());
  Register stringReg = ToRegister(lir->temp1());

  auto* oolDouble = oolTruncateDouble(temp, output, lir->mir());

  using Fn = bool (*)(JSContext*, JSString*, double*);
  auto* oolString = oolCallVM<Fn, StringToNumber>(lir, ArgList(stringReg),
                                                  StoreFloatRegisterTo(temp));
  Label* stringEntry = oolString->entry();
  Label* stringRejoin = oolString->rejoin();

  Label fails;
  masm.truncateValueToInt32(operand, stringEntry, stringRejoin,
                            oolDouble->entry(), stringReg, temp, output,
                            &fails);
  masm.bind(oolDouble->rejoin());

  bailoutFrom(&fails, lir->snapshot());
}

void CodeGenerator::visitValueToDouble(LValueToDouble* lir) {
  ValueOperand operand = ToValue(lir->input());
  FloatRegister output = ToFloatRegister(lir->output());

  Label fail;
  masm.convertValueToDouble(operand, output, &fail);
  bailoutFrom(&fail, lir->snapshot());
}

void CodeGenerator::visitValueToFloat32(LValueToFloat32* lir) {
  ValueOperand operand = ToValue(lir->input());
  FloatRegister output = ToFloatRegister(lir->output());

  Label fail;
  masm.convertValueToFloat32(operand, output, &fail);
  bailoutFrom(&fail, lir->snapshot());
}

void CodeGenerator::visitValueToFloat16(LValueToFloat16* lir) {
  ValueOperand operand = ToValue(lir->input());
  Register temp = ToTempRegisterOrInvalid(lir->temp0());
  FloatRegister output = ToFloatRegister(lir->output());

  LiveRegisterSet volatileRegs;
  if (!MacroAssembler::SupportsFloat64To16()) {
    volatileRegs = liveVolatileRegs(lir);
  }

  Label fail;
  masm.convertValueToFloat16(operand, output, temp, volatileRegs, &fail);
  bailoutFrom(&fail, lir->snapshot());
}

void CodeGenerator::visitValueToBigInt(LValueToBigInt* lir) {
  ValueOperand operand = ToValue(lir->input());
  Register output = ToRegister(lir->output());

  using Fn = BigInt* (*)(JSContext*, HandleValue);
  auto* ool =
      oolCallVM<Fn, ToBigInt>(lir, ArgList(operand), StoreRegisterTo(output));

  Register tag = masm.extractTag(operand, output);

  Label notBigInt, done;
  masm.branchTestBigInt(Assembler::NotEqual, tag, &notBigInt);
  masm.unboxBigInt(operand, output);
  masm.jump(&done);
  masm.bind(&notBigInt);

  masm.branchTestBoolean(Assembler::Equal, tag, ool->entry());
  masm.branchTestString(Assembler::Equal, tag, ool->entry());

  // ToBigInt(object) can have side-effects; all other types throw a TypeError.
  bailout(lir->snapshot());

  masm.bind(ool->rejoin());
  masm.bind(&done);
}

void CodeGenerator::visitInt32ToDouble(LInt32ToDouble* lir) {
  masm.convertInt32ToDouble(ToRegister(lir->input()),
                            ToFloatRegister(lir->output()));
}

void CodeGenerator::visitFloat32ToDouble(LFloat32ToDouble* lir) {
  masm.convertFloat32ToDouble(ToFloatRegister(lir->input()),
                              ToFloatRegister(lir->output()));
}

void CodeGenerator::visitDoubleToFloat32(LDoubleToFloat32* lir) {
  masm.convertDoubleToFloat32(ToFloatRegister(lir->input()),
                              ToFloatRegister(lir->output()));
}

void CodeGenerator::visitInt32ToFloat32(LInt32ToFloat32* lir) {
  masm.convertInt32ToFloat32(ToRegister(lir->input()),
                             ToFloatRegister(lir->output()));
}

void CodeGenerator::visitDoubleToFloat16(LDoubleToFloat16* lir) {
  LiveRegisterSet volatileRegs;
  if (!MacroAssembler::SupportsFloat64To16()) {
    volatileRegs = liveVolatileRegs(lir);
  }
  masm.convertDoubleToFloat16(
      ToFloatRegister(lir->input()), ToFloatRegister(lir->output()),
      ToTempRegisterOrInvalid(lir->temp0()), volatileRegs);
}

void CodeGenerator::visitDoubleToFloat32ToFloat16(
    LDoubleToFloat32ToFloat16* lir) {
  masm.convertDoubleToFloat16(
      ToFloatRegister(lir->input()), ToFloatRegister(lir->output()),
      ToRegister(lir->temp0()), ToRegister(lir->temp1()));
}

void CodeGenerator::visitFloat32ToFloat16(LFloat32ToFloat16* lir) {
  LiveRegisterSet volatileRegs;
  if (!MacroAssembler::SupportsFloat32To16()) {
    volatileRegs = liveVolatileRegs(lir);
  }
  masm.convertFloat32ToFloat16(
      ToFloatRegister(lir->input()), ToFloatRegister(lir->output()),
      ToTempRegisterOrInvalid(lir->temp0()), volatileRegs);
}

void CodeGenerator::visitInt32ToFloat16(LInt32ToFloat16* lir) {
  LiveRegisterSet volatileRegs;
  if (!MacroAssembler::SupportsFloat32To16()) {
    volatileRegs = liveVolatileRegs(lir);
  }
  masm.convertInt32ToFloat16(
      ToRegister(lir->input()), ToFloatRegister(lir->output()),
      ToTempRegisterOrInvalid(lir->temp0()), volatileRegs);
}

void CodeGenerator::visitDoubleToInt32(LDoubleToInt32* lir) {
  Label fail;
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());
  masm.convertDoubleToInt32(input, output, &fail,
                            lir->mir()->needsNegativeZeroCheck());
  bailoutFrom(&fail, lir->snapshot());
}

void CodeGenerator::visitFloat32ToInt32(LFloat32ToInt32* lir) {
  Label fail;
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());
  masm.convertFloat32ToInt32(input, output, &fail,
                             lir->mir()->needsNegativeZeroCheck());
  bailoutFrom(&fail, lir->snapshot());
}

void CodeGenerator::visitInt32ToIntPtr(LInt32ToIntPtr* lir) {
#ifdef JS_64BIT
  // This LIR instruction is only used if the input can be negative.
  MOZ_ASSERT(lir->mir()->canBeNegative());

  Register output = ToRegister(lir->output());
  const LAllocation* input = lir->input();
  if (input->isGeneralReg()) {
    masm.move32SignExtendToPtr(ToRegister(input), output);
  } else {
    masm.load32SignExtendToPtr(ToAddress(input), output);
  }
#else
  MOZ_CRASH("Not used on 32-bit platforms");
#endif
}

void CodeGenerator::visitNonNegativeIntPtrToInt32(
    LNonNegativeIntPtrToInt32* lir) {
#ifdef JS_64BIT
  Register output = ToRegister(lir->output());
  MOZ_ASSERT(ToRegister(lir->input()) == output);

  Label bail;
  masm.guardNonNegativeIntPtrToInt32(output, &bail);
  bailoutFrom(&bail, lir->snapshot());
#else
  MOZ_CRASH("Not used on 32-bit platforms");
#endif
}

void CodeGenerator::visitIntPtrToDouble(LIntPtrToDouble* lir) {
  Register input = ToRegister(lir->input());
  FloatRegister output = ToFloatRegister(lir->output());
  masm.convertIntPtrToDouble(input, output);
}

void CodeGenerator::visitAdjustDataViewLength(LAdjustDataViewLength* lir) {
  Register output = ToRegister(lir->output());
  MOZ_ASSERT(ToRegister(lir->input()) == output);

  uint32_t byteSize = lir->mir()->byteSize();

#ifdef DEBUG
  Label ok;
  masm.branchTestPtr(Assembler::NotSigned, output, output, &ok);
  masm.assumeUnreachable("Unexpected negative value in LAdjustDataViewLength");
  masm.bind(&ok);
#endif

  Label bail;
  masm.branchSubPtr(Assembler::Signed, Imm32(byteSize - 1), output, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::emitOOLTestObject(Register objreg,
                                      Label* ifEmulatesUndefined,
                                      Label* ifDoesntEmulateUndefined,
                                      Register scratch) {
  saveVolatile(scratch);
#if defined(DEBUG) || defined(FUZZING)
  masm.loadPtr(AbsoluteAddress(
                   gen->runtime->addressOfHasSeenObjectEmulateUndefinedFuse()),
               scratch);
  using Fn = bool (*)(JSObject* obj, size_t fuseValue);
  masm.setupAlignedABICall();
  masm.passABIArg(objreg);
  masm.passABIArg(scratch);
  masm.callWithABI<Fn, js::EmulatesUndefinedCheckFuse>();
#else
  using Fn = bool (*)(JSObject* obj);
  masm.setupAlignedABICall();
  masm.passABIArg(objreg);
  masm.callWithABI<Fn, js::EmulatesUndefined>();
#endif
  masm.storeCallPointerResult(scratch);
  restoreVolatile(scratch);

  masm.branchIfTrueBool(scratch, ifEmulatesUndefined);
  masm.jump(ifDoesntEmulateUndefined);
}

// Base out-of-line code generator for all tests of the truthiness of an
// object, where the object might not be truthy.  (Recall that per spec all
// objects are truthy, but we implement the JSCLASS_EMULATES_UNDEFINED class
// flag to permit objects to look like |undefined| in certain contexts,
// including in object truthiness testing.)  We check truthiness inline except
// when we're testing it on a proxy, in which case out-of-line code will call
// EmulatesUndefined for a conclusive answer.
class OutOfLineTestObject : public OutOfLineCodeBase<CodeGenerator> {
  Register objreg_;
  Register scratch_;

  Label* ifEmulatesUndefined_;
  Label* ifDoesntEmulateUndefined_;

#ifdef DEBUG
  bool initialized() { return ifEmulatesUndefined_ != nullptr; }
#endif

 public:
  OutOfLineTestObject()
      : ifEmulatesUndefined_(nullptr), ifDoesntEmulateUndefined_(nullptr) {}

  void accept(CodeGenerator* codegen) final {
    MOZ_ASSERT(initialized());
    codegen->emitOOLTestObject(objreg_, ifEmulatesUndefined_,
                               ifDoesntEmulateUndefined_, scratch_);
  }

  // Specify the register where the object to be tested is found, labels to
  // jump to if the object is truthy or falsy, and a scratch register for
  // use in the out-of-line path.
  void setInputAndTargets(Register objreg, Label* ifEmulatesUndefined,
                          Label* ifDoesntEmulateUndefined, Register scratch) {
    MOZ_ASSERT(!initialized());
    MOZ_ASSERT(ifEmulatesUndefined);
    objreg_ = objreg;
    scratch_ = scratch;
    ifEmulatesUndefined_ = ifEmulatesUndefined;
    ifDoesntEmulateUndefined_ = ifDoesntEmulateUndefined;
  }
};

// A subclass of OutOfLineTestObject containing two extra labels, for use when
// the ifTruthy/ifFalsy labels are needed in inline code as well as out-of-line
// code.  The user should bind these labels in inline code, and specify them as
// targets via setInputAndTargets, as appropriate.
class OutOfLineTestObjectWithLabels : public OutOfLineTestObject {
  Label label1_;
  Label label2_;

 public:
  OutOfLineTestObjectWithLabels() = default;

  Label* label1() { return &label1_; }
  Label* label2() { return &label2_; }
};

void CodeGenerator::testObjectEmulatesUndefinedKernel(
    Register objreg, Label* ifEmulatesUndefined,
    Label* ifDoesntEmulateUndefined, Register scratch,
    OutOfLineTestObject* ool) {
  ool->setInputAndTargets(objreg, ifEmulatesUndefined, ifDoesntEmulateUndefined,
                          scratch);

  // Perform a fast-path check of the object's class flags if the object's
  // not a proxy.  Let out-of-line code handle the slow cases that require
  // saving registers, making a function call, and restoring registers.
  masm.branchIfObjectEmulatesUndefined(objreg, scratch, ool->entry(),
                                       ifEmulatesUndefined);
}

void CodeGenerator::branchTestObjectEmulatesUndefined(
    Register objreg, Label* ifEmulatesUndefined,
    Label* ifDoesntEmulateUndefined, Register scratch,
    OutOfLineTestObject* ool) {
  MOZ_ASSERT(!ifDoesntEmulateUndefined->bound(),
             "ifDoesntEmulateUndefined will be bound to the fallthrough path");

  testObjectEmulatesUndefinedKernel(objreg, ifEmulatesUndefined,
                                    ifDoesntEmulateUndefined, scratch, ool);
  masm.bind(ifDoesntEmulateUndefined);
}

void CodeGenerator::testObjectEmulatesUndefined(Register objreg,
                                                Label* ifEmulatesUndefined,
                                                Label* ifDoesntEmulateUndefined,
                                                Register scratch,
                                                OutOfLineTestObject* ool) {
  testObjectEmulatesUndefinedKernel(objreg, ifEmulatesUndefined,
                                    ifDoesntEmulateUndefined, scratch, ool);
  masm.jump(ifDoesntEmulateUndefined);
}

void CodeGenerator::testValueTruthyForType(
    JSValueType type, ScratchTagScope& tag, const ValueOperand& value,
    Register tempToUnbox, Register temp, FloatRegister floatTemp,
    Label* ifTruthy, Label* ifFalsy, OutOfLineTestObject* ool,
    bool skipTypeTest) {
#ifdef DEBUG
  if (skipTypeTest) {
    Label expected;
    masm.branchTestType(Assembler::Equal, tag, type, &expected);
    masm.assumeUnreachable("Unexpected Value type in testValueTruthyForType");
    masm.bind(&expected);
  }
#endif

  // Handle irregular types first.
  switch (type) {
    case JSVAL_TYPE_UNDEFINED:
    case JSVAL_TYPE_NULL:
      // Undefined and null are falsy.
      if (!skipTypeTest) {
        masm.branchTestType(Assembler::Equal, tag, type, ifFalsy);
      } else {
        masm.jump(ifFalsy);
      }
      return;
    case JSVAL_TYPE_SYMBOL:
      // Symbols are truthy.
      if (!skipTypeTest) {
        masm.branchTestSymbol(Assembler::Equal, tag, ifTruthy);
      } else {
        masm.jump(ifTruthy);
      }
      return;
    case JSVAL_TYPE_OBJECT: {
      Label notObject;
      if (!skipTypeTest) {
        masm.branchTestObject(Assembler::NotEqual, tag, &notObject);
      }
      ScratchTagScopeRelease _(&tag);
      Register objreg = masm.extractObject(value, tempToUnbox);
      testObjectEmulatesUndefined(objreg, ifFalsy, ifTruthy, temp, ool);
      masm.bind(&notObject);
      return;
    }
    default:
      break;
  }

  // Check the type of the value (unless this is the last possible type).
  Label differentType;
  if (!skipTypeTest) {
    masm.branchTestType(Assembler::NotEqual, tag, type, &differentType);
  }

  // Branch if the value is falsy.
  ScratchTagScopeRelease _(&tag);
  switch (type) {
    case JSVAL_TYPE_BOOLEAN: {
      masm.branchTestBooleanTruthy(false, value, ifFalsy);
      break;
    }
    case JSVAL_TYPE_INT32: {
      masm.branchTestInt32Truthy(false, value, ifFalsy);
      break;
    }
    case JSVAL_TYPE_STRING: {
      masm.branchTestStringTruthy(false, value, ifFalsy);
      break;
    }
    case JSVAL_TYPE_BIGINT: {
      masm.branchTestBigIntTruthy(false, value, ifFalsy);
      break;
    }
    case JSVAL_TYPE_DOUBLE: {
      masm.unboxDouble(value, floatTemp);
      masm.branchTestDoubleTruthy(false, floatTemp, ifFalsy);
      break;
    }
    default:
      MOZ_CRASH("Unexpected value type");
  }

  // If we reach this point, the value is truthy.  We fall through for
  // truthy on the last test; otherwise, branch.
  if (!skipTypeTest) {
    masm.jump(ifTruthy);
  }

  masm.bind(&differentType);
}

void CodeGenerator::testValueTruthy(const ValueOperand& value,
                                    Register tempToUnbox, Register temp,
                                    FloatRegister floatTemp,
                                    const TypeDataList& observedTypes,
                                    Label* ifTruthy, Label* ifFalsy,
                                    OutOfLineTestObject* ool) {
  ScratchTagScope tag(masm, value);
  masm.splitTagForTest(value, tag);

  const std::initializer_list<JSValueType> defaultOrder = {
      JSVAL_TYPE_UNDEFINED, JSVAL_TYPE_NULL,   JSVAL_TYPE_BOOLEAN,
      JSVAL_TYPE_INT32,     JSVAL_TYPE_OBJECT, JSVAL_TYPE_STRING,
      JSVAL_TYPE_DOUBLE,    JSVAL_TYPE_SYMBOL, JSVAL_TYPE_BIGINT};

  mozilla::EnumSet<JSValueType, uint32_t> remaining(defaultOrder);

  // Generate tests for previously observed types first.
  // The TypeDataList is sorted by descending frequency.
  for (auto& observed : observedTypes) {
    JSValueType type = observed.type();
    remaining -= type;

    testValueTruthyForType(type, tag, value, tempToUnbox, temp, floatTemp,
                           ifTruthy, ifFalsy, ool, /*skipTypeTest*/ false);
  }

  // Generate tests for remaining types.
  for (auto type : defaultOrder) {
    if (!remaining.contains(type)) {
      continue;
    }
    remaining -= type;

    // We don't need a type test for the last possible type.
    bool skipTypeTest = remaining.isEmpty();
    testValueTruthyForType(type, tag, value, tempToUnbox, temp, floatTemp,
                           ifTruthy, ifFalsy, ool, skipTypeTest);
  }
  MOZ_ASSERT(remaining.isEmpty());

  // We fall through if the final test is truthy.
}

void CodeGenerator::visitTestIAndBranch(LTestIAndBranch* test) {
  Register input = ToRegister(test->input());
  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    masm.branchTest32(Assembler::NonZero, input, input,
                      getJumpLabelForBranch(ifTrue));
  } else {
    masm.branchTest32(Assembler::Zero, input, input,
                      getJumpLabelForBranch(ifFalse));
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitTestIPtrAndBranch(LTestIPtrAndBranch* test) {
  Register input = ToRegister(test->input());
  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    masm.branchTestPtr(Assembler::NonZero, input, input,
                       getJumpLabelForBranch(ifTrue));
  } else {
    masm.branchTestPtr(Assembler::Zero, input, input,
                       getJumpLabelForBranch(ifFalse));
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitTestI64AndBranch(LTestI64AndBranch* test) {
  Register64 input = ToRegister64(test->input());
  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    masm.branchTest64(Assembler::NonZero, input, input,
                      getJumpLabelForBranch(ifTrue));
  } else if (isNextBlock(ifTrue->lir())) {
    masm.branchTest64(Assembler::Zero, input, input,
                      getJumpLabelForBranch(ifFalse));
  } else {
    masm.branchTest64(Assembler::NonZero, input, input,
                      getJumpLabelForBranch(ifTrue),
                      getJumpLabelForBranch(ifFalse));
  }
}

void CodeGenerator::visitTestBIAndBranch(LTestBIAndBranch* lir) {
  Register input = ToRegister(lir->input());
  MBasicBlock* ifTrue = lir->ifTrue();
  MBasicBlock* ifFalse = lir->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    masm.branchIfBigIntIsNonZero(input, getJumpLabelForBranch(ifTrue));
  } else {
    masm.branchIfBigIntIsZero(input, getJumpLabelForBranch(ifFalse));
    jumpToBlock(ifTrue);
  }
}

static Assembler::Condition ReverseCondition(Assembler::Condition condition) {
  switch (condition) {
    case Assembler::Equal:
    case Assembler::NotEqual:
      return condition;
    case Assembler::Above:
      return Assembler::Below;
    case Assembler::AboveOrEqual:
      return Assembler::BelowOrEqual;
    case Assembler::Below:
      return Assembler::Above;
    case Assembler::BelowOrEqual:
      return Assembler::AboveOrEqual;
    case Assembler::GreaterThan:
      return Assembler::LessThan;
    case Assembler::GreaterThanOrEqual:
      return Assembler::LessThanOrEqual;
    case Assembler::LessThan:
      return Assembler::GreaterThan;
    case Assembler::LessThanOrEqual:
      return Assembler::GreaterThanOrEqual;
    default:
      break;
  }
  MOZ_CRASH("unhandled condition");
}

void CodeGenerator::visitCompare(LCompare* comp) {
  MCompare::CompareType compareType = comp->mir()->compareType();
  Assembler::Condition cond = JSOpToCondition(compareType, comp->jsop());
  Register left = ToRegister(comp->left());
  const LAllocation* right = comp->right();
  Register output = ToRegister(comp->output());

  if (compareType == MCompare::Compare_Object ||
      compareType == MCompare::Compare_Symbol ||
      compareType == MCompare::Compare_IntPtr ||
      compareType == MCompare::Compare_UIntPtr ||
      compareType == MCompare::Compare_WasmAnyRef) {
    if (right->isConstant()) {
      MOZ_ASSERT(compareType == MCompare::Compare_IntPtr ||
                 compareType == MCompare::Compare_UIntPtr);
      masm.cmpPtrSet(cond, left, ImmWord(ToInt32(right)), output);
    } else if (right->isGeneralReg()) {
      masm.cmpPtrSet(cond, left, ToRegister(right), output);
    } else {
      masm.cmpPtrSet(ReverseCondition(cond), ToAddress(right), left, output);
    }
    return;
  }

  MOZ_ASSERT(compareType == MCompare::Compare_Int32 ||
             compareType == MCompare::Compare_UInt32);

  if (right->isConstant()) {
    masm.cmp32Set(cond, left, Imm32(ToInt32(right)), output);
  } else if (right->isGeneralReg()) {
    masm.cmp32Set(cond, left, ToRegister(right), output);
  } else {
    masm.cmp32Set(ReverseCondition(cond), ToAddress(right), left, output);
  }
}

void CodeGenerator::visitStrictConstantCompareInt32(
    LStrictConstantCompareInt32* comp) {
  ValueOperand value = ToValue(comp->value());
  int32_t constantVal = comp->mir()->constant();
  JSOp op = comp->mir()->jsop();
  Register output = ToRegister(comp->output());

  Label fail, pass, done, maybeDouble;
  masm.branchTestInt32(Assembler::NotEqual, value, &maybeDouble);
  masm.branch32(JSOpToCondition(op, true), value.payloadOrValueReg(),
                Imm32(constantVal), &pass,
                MacroAssembler::LhsHighBitsAreClean::No);
  masm.jump(&fail);

  masm.bind(&maybeDouble);
  {
    FloatRegister unboxedValue = ToFloatRegister(comp->temp0());
    FloatRegister floatPayload = ToFloatRegister(comp->temp1());

    masm.branchTestDouble(Assembler::NotEqual, value,
                          op == JSOp::StrictEq ? &fail : &pass);

    masm.unboxDouble(value, unboxedValue);
    masm.loadConstantDouble(double(constantVal), floatPayload);
    masm.branchDouble(JSOpToDoubleCondition(op), unboxedValue, floatPayload,
                      &pass);
  }

  masm.bind(&fail);
  masm.move32(Imm32(0), output);
  masm.jump(&done);

  masm.bind(&pass);
  masm.move32(Imm32(1), output);

  masm.bind(&done);
}

void CodeGenerator::visitStrictConstantCompareBoolean(
    LStrictConstantCompareBoolean* comp) {
  ValueOperand value = ToValue(comp->value());
  bool constantVal = comp->mir()->constant();
  JSOp op = comp->mir()->jsop();
  Register output = ToRegister(comp->output());

  Label fail, pass, done;
  Register boolUnboxed = ToRegister(comp->temp0());
  masm.fallibleUnboxBoolean(value, boolUnboxed,
                            op == JSOp::StrictEq ? &fail : &pass);
  masm.branch32(JSOpToCondition(op, true), boolUnboxed, Imm32(constantVal),
                &pass);

  masm.bind(&fail);
  masm.move32(Imm32(0), output);
  masm.jump(&done);

  masm.bind(&pass);
  masm.move32(Imm32(1), output);

  masm.bind(&done);
}

void CodeGenerator::visitCompareAndBranch(LCompareAndBranch* comp) {
  MCompare::CompareType compareType = comp->cmpMir()->compareType();
  Assembler::Condition cond = JSOpToCondition(compareType, comp->jsop());
  Register left = ToRegister(comp->left());
  const LAllocation* right = comp->right();

  MBasicBlock* ifTrue = comp->ifTrue();
  MBasicBlock* ifFalse = comp->ifFalse();

  // If the next block is the true case, invert the condition to fall through.
  Label* label;
  if (isNextBlock(ifTrue->lir())) {
    cond = Assembler::InvertCondition(cond);
    label = getJumpLabelForBranch(ifFalse);
  } else {
    label = getJumpLabelForBranch(ifTrue);
  }

  if (compareType == MCompare::Compare_Object ||
      compareType == MCompare::Compare_Symbol ||
      compareType == MCompare::Compare_IntPtr ||
      compareType == MCompare::Compare_UIntPtr ||
      compareType == MCompare::Compare_WasmAnyRef) {
    if (right->isConstant()) {
      MOZ_ASSERT(compareType == MCompare::Compare_IntPtr ||
                 compareType == MCompare::Compare_UIntPtr);
      masm.branchPtr(cond, left, ImmWord(ToInt32(right)), label);
    } else if (right->isGeneralReg()) {
      masm.branchPtr(cond, left, ToRegister(right), label);
    } else {
      masm.branchPtr(ReverseCondition(cond), ToAddress(right), left, label);
    }
  } else {
    MOZ_ASSERT(compareType == MCompare::Compare_Int32 ||
               compareType == MCompare::Compare_UInt32);

    if (right->isConstant()) {
      masm.branch32(cond, left, Imm32(ToInt32(right)), label);
    } else if (right->isGeneralReg()) {
      masm.branch32(cond, left, ToRegister(right), label);
    } else {
      masm.branch32(ReverseCondition(cond), ToAddress(right), left, label);
    }
  }

  if (!isNextBlock(ifTrue->lir())) {
    jumpToBlock(ifFalse);
  }
}

void CodeGenerator::visitCompareI64(LCompareI64* lir) {
  MCompare::CompareType compareType = lir->mir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Int64 ||
             compareType == MCompare::Compare_UInt64);
  bool isSigned = compareType == MCompare::Compare_Int64;
  Assembler::Condition cond = JSOpToCondition(lir->jsop(), isSigned);
  Register64 left = ToRegister64(lir->left());
  LInt64Allocation right = lir->right();
  Register output = ToRegister(lir->output());

  if (IsConstant(right)) {
    masm.cmp64Set(cond, left, Imm64(ToInt64(right)), output);
  } else if (IsRegister64(right)) {
    masm.cmp64Set(cond, left, ToRegister64(right), output);
  } else {
    masm.cmp64Set(ReverseCondition(cond), ToAddress(right), left, output);
  }
}

void CodeGenerator::visitCompareI64AndBranch(LCompareI64AndBranch* lir) {
  MCompare::CompareType compareType = lir->cmpMir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Int64 ||
             compareType == MCompare::Compare_UInt64);
  bool isSigned = compareType == MCompare::Compare_Int64;
  Assembler::Condition cond = JSOpToCondition(lir->jsop(), isSigned);
  Register64 left = ToRegister64(lir->left());
  LInt64Allocation right = lir->right();

  MBasicBlock* ifTrue = lir->ifTrue();
  MBasicBlock* ifFalse = lir->ifFalse();

  Label* trueLabel = getJumpLabelForBranch(ifTrue);
  Label* falseLabel = getJumpLabelForBranch(ifFalse);

  // If the next block is the true case, invert the condition to fall through.
  if (isNextBlock(ifTrue->lir())) {
    cond = Assembler::InvertCondition(cond);
    trueLabel = falseLabel;
    falseLabel = nullptr;
  } else if (isNextBlock(ifFalse->lir())) {
    falseLabel = nullptr;
  }

  if (IsConstant(right)) {
    masm.branch64(cond, left, Imm64(ToInt64(right)), trueLabel, falseLabel);
  } else if (IsRegister64(right)) {
    masm.branch64(cond, left, ToRegister64(right), trueLabel, falseLabel);
  } else {
    masm.branch64(ReverseCondition(cond), ToAddress(right), left, trueLabel,
                  falseLabel);
  }
}

void CodeGenerator::visitBitAndAndBranch(LBitAndAndBranch* baab) {
  Assembler::Condition cond = baab->cond();
  MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);

  Register left = ToRegister(baab->left());
  const LAllocation* right = baab->right();

  MBasicBlock* ifTrue = baab->ifTrue();
  MBasicBlock* ifFalse = baab->ifFalse();

  // If the next block is the true case, invert the condition to fall through.
  Label* label;
  if (isNextBlock(ifTrue->lir())) {
    cond = Assembler::InvertCondition(cond);
    label = getJumpLabelForBranch(ifFalse);
  } else {
    label = getJumpLabelForBranch(ifTrue);
  }

  if (right->isConstant()) {
    masm.branchTest32(cond, left, Imm32(ToInt32(right)), label);
  } else {
    masm.branchTest32(cond, left, ToRegister(right), label);
  }

  if (!isNextBlock(ifTrue->lir())) {
    jumpToBlock(ifFalse);
  }
}

void CodeGenerator::visitBitAnd64AndBranch(LBitAnd64AndBranch* baab) {
  Assembler::Condition cond = baab->cond();
  MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);

  Register64 left = ToRegister64(baab->left());
  LInt64Allocation right = baab->right();

  MBasicBlock* ifTrue = baab->ifTrue();
  MBasicBlock* ifFalse = baab->ifFalse();

  Label* trueLabel = getJumpLabelForBranch(ifTrue);
  Label* falseLabel = getJumpLabelForBranch(ifFalse);

  // If the next block is the true case, invert the condition to fall through.
  if (isNextBlock(ifTrue->lir())) {
    cond = Assembler::InvertCondition(cond);
    trueLabel = falseLabel;
    falseLabel = nullptr;
  } else if (isNextBlock(ifFalse->lir())) {
    falseLabel = nullptr;
  }

  if (IsConstant(right)) {
    masm.branchTest64(cond, left, Imm64(ToInt64(right)), trueLabel, falseLabel);
  } else {
    masm.branchTest64(cond, left, ToRegister64(right), trueLabel, falseLabel);
  }
}

void CodeGenerator::assertObjectDoesNotEmulateUndefined(
    Register input, Register temp, const MInstruction* mir) {
#if defined(DEBUG) || defined(FUZZING)
  // Validate that the object indeed doesn't have the emulates undefined flag.
  auto* ool = new (alloc()) OutOfLineTestObjectWithLabels();
  addOutOfLineCode(ool, mir);

  Label* doesNotEmulateUndefined = ool->label1();
  Label* emulatesUndefined = ool->label2();

  testObjectEmulatesUndefined(input, emulatesUndefined, doesNotEmulateUndefined,
                              temp, ool);
  masm.bind(emulatesUndefined);
  masm.assumeUnreachable(
      "Found an object emulating undefined while the fuse is intact");
  masm.bind(doesNotEmulateUndefined);
#endif
}

void CodeGenerator::visitTestOAndBranch(LTestOAndBranch* lir) {
  Label* truthy = getJumpLabelForBranch(lir->ifTruthy());
  Label* falsy = getJumpLabelForBranch(lir->ifFalsy());
  Register input = ToRegister(lir->input());
  Register temp = ToRegister(lir->temp0());

  bool intact = hasSeenObjectEmulateUndefinedFuseIntactAndDependencyNoted();
  if (intact) {
    assertObjectDoesNotEmulateUndefined(input, temp, lir->mir());
    // Bug 1874905: It would be fantastic if this could be optimized out
    masm.jump(truthy);
  } else {
    auto* ool = new (alloc()) OutOfLineTestObject();
    addOutOfLineCode(ool, lir->mir());

    testObjectEmulatesUndefined(input, falsy, truthy, temp, ool);
  }
}

void CodeGenerator::visitTestVAndBranch(LTestVAndBranch* lir) {
  auto* ool = new (alloc()) OutOfLineTestObject();
  addOutOfLineCode(ool, lir->mir());

  Label* truthy = getJumpLabelForBranch(lir->ifTruthy());
  Label* falsy = getJumpLabelForBranch(lir->ifFalsy());

  ValueOperand input = ToValue(lir->input());
  Register tempToUnbox = ToTempUnboxRegister(lir->temp1());
  Register temp = ToRegister(lir->temp2());
  FloatRegister floatTemp = ToFloatRegister(lir->temp0());
  const TypeDataList& observedTypes = lir->mir()->observedTypes();

  testValueTruthy(input, tempToUnbox, temp, floatTemp, observedTypes, truthy,
                  falsy, ool);
  masm.jump(truthy);
}

void CodeGenerator::visitBooleanToString(LBooleanToString* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  const JSAtomState& names = gen->runtime->names();
  Label true_, done;

  masm.branchTest32(Assembler::NonZero, input, input, &true_);
  masm.movePtr(ImmGCPtr(names.false_), output);
  masm.jump(&done);

  masm.bind(&true_);
  masm.movePtr(ImmGCPtr(names.true_), output);

  masm.bind(&done);
}

void CodeGenerator::visitIntToString(LIntToString* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  using Fn = JSLinearString* (*)(JSContext*, int);
  OutOfLineCode* ool = oolCallVM<Fn, Int32ToString<CanGC>>(
      lir, ArgList(input), StoreRegisterTo(output));

  masm.lookupStaticIntString(input, output, gen->runtime->staticStrings(),
                             ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitDoubleToString(LDoubleToString* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register temp = ToRegister(lir->temp0());
  Register output = ToRegister(lir->output());

  using Fn = JSString* (*)(JSContext*, double);
  OutOfLineCode* ool = oolCallVM<Fn, NumberToString<CanGC>>(
      lir, ArgList(input), StoreRegisterTo(output));

  // Try double to integer conversion and run integer to string code.
  masm.convertDoubleToInt32(input, temp, ool->entry(), false);
  masm.lookupStaticIntString(temp, output, gen->runtime->staticStrings(),
                             ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitValueToString(LValueToString* lir) {
  ValueOperand input = ToValue(lir->input());
  Register output = ToRegister(lir->output());

  using Fn = JSString* (*)(JSContext*, HandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, ToStringSlowForVM<CanGC>>(
      lir, ArgList(input), StoreRegisterTo(output));

  Label done;
  Register tag = masm.extractTag(input, output);
  const JSAtomState& names = gen->runtime->names();

  // String
  {
    Label notString;
    masm.branchTestString(Assembler::NotEqual, tag, &notString);
    masm.unboxString(input, output);
    masm.jump(&done);
    masm.bind(&notString);
  }

  // Integer
  {
    Label notInteger;
    masm.branchTestInt32(Assembler::NotEqual, tag, &notInteger);
    Register unboxed = ToTempUnboxRegister(lir->temp0());
    unboxed = masm.extractInt32(input, unboxed);
    masm.lookupStaticIntString(unboxed, output, gen->runtime->staticStrings(),
                               ool->entry());
    masm.jump(&done);
    masm.bind(&notInteger);
  }

  // Double
  {
    // Note: no fastpath. Need two extra registers and can only convert doubles
    // that fit integers and are smaller than StaticStrings::INT_STATIC_LIMIT.
    masm.branchTestDouble(Assembler::Equal, tag, ool->entry());
  }

  // Undefined
  {
    Label notUndefined;
    masm.branchTestUndefined(Assembler::NotEqual, tag, &notUndefined);
    masm.movePtr(ImmGCPtr(names.undefined), output);
    masm.jump(&done);
    masm.bind(&notUndefined);
  }

  // Null
  {
    Label notNull;
    masm.branchTestNull(Assembler::NotEqual, tag, &notNull);
    masm.movePtr(ImmGCPtr(names.null), output);
    masm.jump(&done);
    masm.bind(&notNull);
  }

  // Boolean
  {
    Label notBoolean, true_;
    masm.branchTestBoolean(Assembler::NotEqual, tag, &notBoolean);
    masm.branchTestBooleanTruthy(true, input, &true_);
    masm.movePtr(ImmGCPtr(names.false_), output);
    masm.jump(&done);
    masm.bind(&true_);
    masm.movePtr(ImmGCPtr(names.true_), output);
    masm.jump(&done);
    masm.bind(&notBoolean);
  }

  // Objects/symbols are only possible when |mir->mightHaveSideEffects()|.
  if (lir->mir()->mightHaveSideEffects()) {
    // Object
    if (lir->mir()->supportSideEffects()) {
      masm.branchTestObject(Assembler::Equal, tag, ool->entry());
    } else {
      // Bail.
      MOZ_ASSERT(lir->mir()->needsSnapshot());
      Label bail;
      masm.branchTestObject(Assembler::Equal, tag, &bail);
      bailoutFrom(&bail, lir->snapshot());
    }

    // Symbol
    if (lir->mir()->supportSideEffects()) {
      masm.branchTestSymbol(Assembler::Equal, tag, ool->entry());
    } else {
      // Bail.
      MOZ_ASSERT(lir->mir()->needsSnapshot());
      Label bail;
      masm.branchTestSymbol(Assembler::Equal, tag, &bail);
      bailoutFrom(&bail, lir->snapshot());
    }
  }

  // BigInt
  {
    // No fastpath currently implemented.
    masm.branchTestBigInt(Assembler::Equal, tag, ool->entry());
  }

  masm.assumeUnreachable("Unexpected type for LValueToString.");

  masm.bind(&done);
  masm.bind(ool->rejoin());
}

using StoreBufferMutationFn = void (*)(js::gc::StoreBuffer*, js::gc::Cell**);

static void EmitStoreBufferMutation(MacroAssembler& masm, Register holder,
                                    size_t offset, Register buffer,
                                    LiveGeneralRegisterSet& liveVolatiles,
                                    StoreBufferMutationFn fun) {
  Label callVM;
  Label exit;

  // Call into the VM to barrier the write. The only registers that need to
  // be preserved are those in liveVolatiles, so once they are saved on the
  // stack all volatile registers are available for use.
  masm.bind(&callVM);
  masm.PushRegsInMask(liveVolatiles);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  regs.takeUnchecked(buffer);
  regs.takeUnchecked(holder);
  Register addrReg = regs.takeAny();

  masm.computeEffectiveAddress(Address(holder, offset), addrReg);

  bool needExtraReg = !regs.hasAny<GeneralRegisterSet::DefaultType>();
  if (needExtraReg) {
    masm.push(holder);
    masm.setupUnalignedABICall(holder);
  } else {
    masm.setupUnalignedABICall(regs.takeAny());
  }
  masm.passABIArg(buffer);
  masm.passABIArg(addrReg);
  masm.callWithABI(DynamicFunction<StoreBufferMutationFn>(fun),
                   ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);

  if (needExtraReg) {
    masm.pop(holder);
  }
  masm.PopRegsInMask(liveVolatiles);
  masm.bind(&exit);
}

// Warning: this function modifies prev and next.
static void EmitPostWriteBarrierS(MacroAssembler& masm, Register holder,
                                  size_t offset, Register prev, Register next,
                                  LiveGeneralRegisterSet& liveVolatiles) {
  Label exit;
  Label checkRemove, putCell;

  // if (next && (buffer = next->storeBuffer()))
  // but we never pass in nullptr for next.
  Register storebuffer = next;
  masm.loadStoreBuffer(next, storebuffer);
  masm.branchPtr(Assembler::Equal, storebuffer, ImmWord(0), &checkRemove);

  // if (prev && prev->storeBuffer())
  masm.branchPtr(Assembler::Equal, prev, ImmWord(0), &putCell);
  masm.loadStoreBuffer(prev, prev);
  masm.branchPtr(Assembler::NotEqual, prev, ImmWord(0), &exit);

  // buffer->putCell(cellp)
  masm.bind(&putCell);
  EmitStoreBufferMutation(masm, holder, offset, storebuffer, liveVolatiles,
                          JSString::addCellAddressToStoreBuffer);
  masm.jump(&exit);

  // if (prev && (buffer = prev->storeBuffer()))
  masm.bind(&checkRemove);
  masm.branchPtr(Assembler::Equal, prev, ImmWord(0), &exit);
  masm.loadStoreBuffer(prev, storebuffer);
  masm.branchPtr(Assembler::Equal, storebuffer, ImmWord(0), &exit);
  EmitStoreBufferMutation(masm, holder, offset, storebuffer, liveVolatiles,
                          JSString::removeCellAddressFromStoreBuffer);

  masm.bind(&exit);
}

void CodeGenerator::visitRegExp(LRegExp* lir) {
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());
  JSObject* source = lir->mir()->source();

  using Fn = JSObject* (*)(JSContext*, Handle<RegExpObject*>);
  OutOfLineCode* ool = oolCallVM<Fn, CloneRegExpObject>(
      lir, ArgList(ImmGCPtr(source)), StoreRegisterTo(output));
  if (lir->mir()->hasShared()) {
    TemplateObject templateObject(source);
    masm.createGCObject(output, temp, templateObject, gc::Heap::Default,
                        ool->entry());
  } else {
    masm.jump(ool->entry());
  }
  masm.bind(ool->rejoin());
}

static constexpr int32_t RegExpPairsVectorStartOffset(
    int32_t inputOutputDataStartOffset) {
  return inputOutputDataStartOffset + int32_t(InputOutputDataSize) +
         int32_t(sizeof(MatchPairs));
}

static Address RegExpPairCountAddress(MacroAssembler& masm,
                                      int32_t inputOutputDataStartOffset) {
  return Address(FramePointer, inputOutputDataStartOffset +
                                   int32_t(InputOutputDataSize) +
                                   MatchPairs::offsetOfPairCount());
}

static void UpdateRegExpStatics(MacroAssembler& masm, Register regexp,
                                Register input, Register lastIndex,
                                Register staticsReg, Register temp1,
                                Register temp2, gc::Heap initialStringHeap,
                                LiveGeneralRegisterSet& volatileRegs) {
  Address pendingInputAddress(staticsReg,
                              RegExpStatics::offsetOfPendingInput());
  Address matchesInputAddress(staticsReg,
                              RegExpStatics::offsetOfMatchesInput());
  Address lazySourceAddress(staticsReg, RegExpStatics::offsetOfLazySource());
  Address lazyIndexAddress(staticsReg, RegExpStatics::offsetOfLazyIndex());

  masm.guardedCallPreBarrier(pendingInputAddress, MIRType::String);
  masm.guardedCallPreBarrier(matchesInputAddress, MIRType::String);
  masm.guardedCallPreBarrier(lazySourceAddress, MIRType::String);

  if (initialStringHeap == gc::Heap::Default) {
    // Writing into RegExpStatics tenured memory; must post-barrier.
    if (staticsReg.volatile_()) {
      volatileRegs.add(staticsReg);
    }

    masm.loadPtr(pendingInputAddress, temp1);
    masm.storePtr(input, pendingInputAddress);
    masm.movePtr(input, temp2);
    EmitPostWriteBarrierS(masm, staticsReg,
                          RegExpStatics::offsetOfPendingInput(),
                          temp1 /* prev */, temp2 /* next */, volatileRegs);

    masm.loadPtr(matchesInputAddress, temp1);
    masm.storePtr(input, matchesInputAddress);
    masm.movePtr(input, temp2);
    EmitPostWriteBarrierS(masm, staticsReg,
                          RegExpStatics::offsetOfMatchesInput(),
                          temp1 /* prev */, temp2 /* next */, volatileRegs);
  } else {
    masm.debugAssertGCThingIsTenured(input, temp1);
    masm.storePtr(input, pendingInputAddress);
    masm.storePtr(input, matchesInputAddress);
  }

  masm.storePtr(lastIndex,
                Address(staticsReg, RegExpStatics::offsetOfLazyIndex()));
  masm.store32(
      Imm32(1),
      Address(staticsReg, RegExpStatics::offsetOfPendingLazyEvaluation()));

  masm.unboxNonDouble(Address(regexp, NativeObject::getFixedSlotOffset(
                                          RegExpObject::SHARED_SLOT)),
                      temp1, JSVAL_TYPE_PRIVATE_GCTHING);
  masm.loadPtr(Address(temp1, RegExpShared::offsetOfSource()), temp2);
  masm.storePtr(temp2, lazySourceAddress);
  static_assert(sizeof(JS::RegExpFlags) == 1, "load size must match flag size");
  masm.load8ZeroExtend(Address(temp1, RegExpShared::offsetOfFlags()), temp2);
  masm.store8(temp2, Address(staticsReg, RegExpStatics::offsetOfLazyFlags()));
}

// Prepare an InputOutputData and optional MatchPairs which space has been
// allocated for on the stack, and try to execute a RegExp on a string input.
// If the RegExp was successfully executed and matched the input, fallthrough.
// Otherwise, jump to notFound or failure.
//
// inputOutputDataStartOffset is the offset relative to the frame pointer
// register. This offset is negative for the RegExpExecTest stub.
static bool PrepareAndExecuteRegExp(MacroAssembler& masm, Register regexp,
                                    Register input, Register lastIndex,
                                    Register temp1, Register temp2,
                                    Register temp3,
                                    int32_t inputOutputDataStartOffset,
                                    gc::Heap initialStringHeap, Label* notFound,
                                    Label* failure) {
  JitSpew(JitSpew_Codegen, "# Emitting PrepareAndExecuteRegExp");

  using irregexp::InputOutputData;

  /*
   * [SMDOC] Stack layout for PrepareAndExecuteRegExp
   *
   * Before this function is called, the caller is responsible for
   * allocating enough stack space for the following data:
   *
   * inputOutputDataStartOffset +-----> +---------------+
   *                                    |InputOutputData|
   *          inputStartAddress +---------->  inputStart|
   *            inputEndAddress +---------->    inputEnd|
   *          startIndexAddress +---------->  startIndex|
   *             matchesAddress +---------->     matches|-----+
   *                                    +---------------+     |
   * matchPairs(Address|Offset) +-----> +---------------+  <--+
   *                                    |  MatchPairs   |
   *           pairCountAddress +---------->    count   |
   *        pairsPointerAddress +---------->    pairs   |-----+
   *                                    +---------------+     |
   * pairsArray(Address|Offset) +-----> +---------------+  <--+
   *                                    |   MatchPair   |
   *     firstMatchStartAddress +---------->    start   |  <--+
   *                                    |       limit   |     |
   *                                    +---------------+     |
   *                                           .              |
   *                                           .  Reserved space for
   *                                           .  RegExpObject::MaxPairCount
   *                                           .  MatchPair objects
   *                                           .              |
   *                                    +---------------+     |
   *                                    |   MatchPair   |     |
   *                                    |       start   |     |
   *                                    |       limit   |  <--+
   *                                    +---------------+
   */

  int32_t ioOffset = inputOutputDataStartOffset;
  int32_t matchPairsOffset = ioOffset + int32_t(sizeof(InputOutputData));
  int32_t pairsArrayOffset = matchPairsOffset + int32_t(sizeof(MatchPairs));

  Address inputStartAddress(FramePointer,
                            ioOffset + InputOutputData::offsetOfInputStart());
  Address inputEndAddress(FramePointer,
                          ioOffset + InputOutputData::offsetOfInputEnd());
  Address startIndexAddress(FramePointer,
                            ioOffset + InputOutputData::offsetOfStartIndex());
  Address matchesAddress(FramePointer,
                         ioOffset + InputOutputData::offsetOfMatches());

  Address matchPairsAddress(FramePointer, matchPairsOffset);
  Address pairCountAddress(FramePointer,
                           matchPairsOffset + MatchPairs::offsetOfPairCount());
  Address pairsPointerAddress(FramePointer,
                              matchPairsOffset + MatchPairs::offsetOfPairs());

  Address pairsArrayAddress(FramePointer, pairsArrayOffset);
  Address firstMatchStartAddress(FramePointer,
                                 pairsArrayOffset + MatchPair::offsetOfStart());

  // First, fill in a skeletal MatchPairs instance on the stack. This will be
  // passed to the OOL stub in the caller if we aren't able to execute the
  // RegExp inline, and that stub needs to be able to determine whether the
  // execution finished successfully.

  // Initialize MatchPairs::pairCount to 1. The correct value can only
  // be determined after loading the RegExpShared. If the RegExpShared
  // has Kind::Atom, this is the correct pairCount.
  masm.store32(Imm32(1), pairCountAddress);

  // Initialize MatchPairs::pairs pointer
  masm.computeEffectiveAddress(pairsArrayAddress, temp1);
  masm.storePtr(temp1, pairsPointerAddress);

  // Initialize MatchPairs::pairs[0]::start to MatchPair::NoMatch
  masm.store32(Imm32(MatchPair::NoMatch), firstMatchStartAddress);

  // Determine the set of volatile inputs to save when calling into C++ or
  // regexp code.
  LiveGeneralRegisterSet volatileRegs;
  if (lastIndex.volatile_()) {
    volatileRegs.add(lastIndex);
  }
  if (input.volatile_()) {
    volatileRegs.add(input);
  }
  if (regexp.volatile_()) {
    volatileRegs.add(regexp);
  }

  // Ensure the input string is not a rope.
  Label isLinear;
  masm.branchIfNotRope(input, &isLinear);
  {
    masm.PushRegsInMask(volatileRegs);

    using Fn = JSLinearString* (*)(JSString*);
    masm.setupUnalignedABICall(temp1);
    masm.passABIArg(input);
    masm.callWithABI<Fn, js::jit::LinearizeForCharAccessPure>();

    MOZ_ASSERT(!volatileRegs.has(temp1));
    masm.storeCallPointerResult(temp1);
    masm.PopRegsInMask(volatileRegs);

    masm.branchTestPtr(Assembler::Zero, temp1, temp1, failure);
  }
  masm.bind(&isLinear);

  // Load the RegExpShared.
  Register regexpReg = temp1;
  Address sharedSlot = Address(
      regexp, NativeObject::getFixedSlotOffset(RegExpObject::SHARED_SLOT));
  masm.branchTestUndefined(Assembler::Equal, sharedSlot, failure);
  masm.unboxNonDouble(sharedSlot, regexpReg, JSVAL_TYPE_PRIVATE_GCTHING);

  // Handle Atom matches
  Label notAtom, checkSuccess;
  masm.branchPtr(Assembler::Equal,
                 Address(regexpReg, RegExpShared::offsetOfPatternAtom()),
                 ImmWord(0), &notAtom);
  {
    masm.computeEffectiveAddress(matchPairsAddress, temp3);

    masm.PushRegsInMask(volatileRegs);
    using Fn =
        RegExpRunStatus (*)(RegExpShared* re, const JSLinearString* input,
                            size_t start, MatchPairs* matchPairs);
    masm.setupUnalignedABICall(temp2);
    masm.passABIArg(regexpReg);
    masm.passABIArg(input);
    masm.passABIArg(lastIndex);
    masm.passABIArg(temp3);
    masm.callWithABI<Fn, js::ExecuteRegExpAtomRaw>();

    MOZ_ASSERT(!volatileRegs.has(temp1));
    masm.storeCallInt32Result(temp1);
    masm.PopRegsInMask(volatileRegs);

    masm.jump(&checkSuccess);
  }
  masm.bind(&notAtom);

  // Don't handle regexps with too many capture pairs.
  masm.load32(Address(regexpReg, RegExpShared::offsetOfPairCount()), temp2);
  masm.branch32(Assembler::Above, temp2, Imm32(RegExpObject::MaxPairCount),
                failure);

  // Fill in the pair count in the MatchPairs on the stack.
  masm.store32(temp2, pairCountAddress);

  // Load code pointer and length of input (in bytes).
  // Store the input start in the InputOutputData.
  Register codePointer = temp1;  // Note: temp1 was previously regexpReg.
  Register byteLength = temp3;
  {
    Label isLatin1, done;
    masm.loadStringLength(input, byteLength);

    masm.branchLatin1String(input, &isLatin1);

    // Two-byte input
    masm.loadStringChars(input, temp2, CharEncoding::TwoByte);
    masm.storePtr(temp2, inputStartAddress);
    masm.loadPtr(
        Address(regexpReg, RegExpShared::offsetOfJitCode(/*latin1 =*/false)),
        codePointer);
    masm.lshiftPtr(Imm32(1), byteLength);
    masm.jump(&done);

    // Latin1 input
    masm.bind(&isLatin1);
    masm.loadStringChars(input, temp2, CharEncoding::Latin1);
    masm.storePtr(temp2, inputStartAddress);
    masm.loadPtr(
        Address(regexpReg, RegExpShared::offsetOfJitCode(/*latin1 =*/true)),
        codePointer);

    masm.bind(&done);

    // Store end pointer
    masm.addPtr(byteLength, temp2);
    masm.storePtr(temp2, inputEndAddress);
  }

  // Guard that the RegExpShared has been compiled for this type of input.
  // If it has not been compiled, we fall back to the OOL case, which will
  // do a VM call into the interpreter.
  // TODO: add an interpreter trampoline?
  masm.branchPtr(Assembler::Equal, codePointer, ImmWord(0), failure);
  masm.loadPtr(Address(codePointer, JitCode::offsetOfCode()), codePointer);

  // Finish filling in the InputOutputData instance on the stack
  masm.computeEffectiveAddress(matchPairsAddress, temp2);
  masm.storePtr(temp2, matchesAddress);
  masm.storePtr(lastIndex, startIndexAddress);

  // Execute the RegExp.
  masm.computeEffectiveAddress(
      Address(FramePointer, inputOutputDataStartOffset), temp2);
  masm.PushRegsInMask(volatileRegs);
  masm.setupUnalignedABICall(temp3);
  masm.passABIArg(temp2);
  masm.callWithABI(codePointer);
  masm.storeCallInt32Result(temp1);
  masm.PopRegsInMask(volatileRegs);

  masm.bind(&checkSuccess);
  masm.branch32(Assembler::Equal, temp1,
                Imm32(int32_t(RegExpRunStatus::Success_NotFound)), notFound);
  masm.branch32(Assembler::Equal, temp1, Imm32(int32_t(RegExpRunStatus::Error)),
                failure);

  // Lazily update the RegExpStatics.
  size_t offset = GlobalObjectData::offsetOfRegExpRealm() +
                  RegExpRealm::offsetOfRegExpStatics();
  masm.loadGlobalObjectData(temp1);
  masm.loadPtr(Address(temp1, offset), temp1);
  UpdateRegExpStatics(masm, regexp, input, lastIndex, temp1, temp2, temp3,
                      initialStringHeap, volatileRegs);

  return true;
}

static void EmitInitDependentStringBase(MacroAssembler& masm,
                                        Register dependent, Register base,
                                        Register temp1, Register temp2,
                                        bool needsPostBarrier) {
  // Determine the base string to use and store it in temp2.
  Label notDependent, markedDependedOn;
  masm.load32(Address(base, JSString::offsetOfFlags()), temp1);
  masm.branchTest32(Assembler::Zero, temp1, Imm32(JSString::DEPENDENT_BIT),
                    &notDependent);
  {
    // The base is also a dependent string. Load its base to prevent chains of
    // dependent strings in most cases. This must either be an atom or already
    // have the DEPENDED_ON_BIT set.
    masm.loadDependentStringBase(base, temp2);
    masm.jump(&markedDependedOn);
  }
  masm.bind(&notDependent);
  {
    // The base is not a dependent string. Set the DEPENDED_ON_BIT if it's not
    // an atom.
    masm.movePtr(base, temp2);
    masm.branchTest32(Assembler::NonZero, temp1, Imm32(JSString::ATOM_BIT),
                      &markedDependedOn);
    masm.or32(Imm32(JSString::DEPENDED_ON_BIT), temp1);
    masm.store32(temp1, Address(temp2, JSString::offsetOfFlags()));
  }
  masm.bind(&markedDependedOn);

#ifdef DEBUG
  // Assert the base has the DEPENDED_ON_BIT set or is an atom.
  Label isAppropriatelyMarked;
  masm.branchTest32(Assembler::NonZero,
                    Address(temp2, JSString::offsetOfFlags()),
                    Imm32(JSString::ATOM_BIT | JSString::DEPENDED_ON_BIT),
                    &isAppropriatelyMarked);
  masm.assumeUnreachable("Base string is missing DEPENDED_ON_BIT");
  masm.bind(&isAppropriatelyMarked);
#endif
  masm.storeDependentStringBase(temp2, dependent);

  // Post-barrier the base store. The base is still in temp2.
  if (needsPostBarrier) {
    Label done;
    masm.branchPtrInNurseryChunk(Assembler::Equal, dependent, temp1, &done);
    masm.branchPtrInNurseryChunk(Assembler::NotEqual, temp2, temp1, &done);

    LiveRegisterSet regsToSave(RegisterSet::Volatile());
    regsToSave.takeUnchecked(temp1);
    regsToSave.takeUnchecked(temp2);

    masm.PushRegsInMask(regsToSave);

    masm.mov(ImmPtr(masm.runtime()), temp1);

    using Fn = void (*)(JSRuntime* rt, js::gc::Cell* cell);
    masm.setupUnalignedABICall(temp2);
    masm.passABIArg(temp1);
    masm.passABIArg(dependent);
    masm.callWithABI<Fn, PostWriteBarrier>();

    masm.PopRegsInMask(regsToSave);

    masm.bind(&done);
  } else {
#ifdef DEBUG
    Label done;
    masm.branchPtrInNurseryChunk(Assembler::Equal, dependent, temp1, &done);
    masm.branchPtrInNurseryChunk(Assembler::NotEqual, temp2, temp1, &done);
    masm.assumeUnreachable("Missing post barrier for dependent string base");
    masm.bind(&done);
#endif
  }
}

static void CopyStringChars(MacroAssembler& masm, Register to, Register from,
                            Register len, Register byteOpScratch,
                            CharEncoding encoding,
                            size_t maximumLength = SIZE_MAX);

class CreateDependentString {
  CharEncoding encoding_;
  Register string_;
  Register temp1_;
  Register temp2_;
  Label* failure_;

  enum class FallbackKind : uint8_t {
    InlineString,
    FatInlineString,
    NotInlineString,
    Count
  };
  mozilla::EnumeratedArray<FallbackKind, Label, size_t(FallbackKind::Count)>
      fallbacks_, joins_;

 public:
  CreateDependentString(CharEncoding encoding, Register string, Register temp1,
                        Register temp2, Label* failure)
      : encoding_(encoding),
        string_(string),
        temp1_(temp1),
        temp2_(temp2),
        failure_(failure) {}

  Register string() const { return string_; }
  CharEncoding encoding() const { return encoding_; }

  // Generate code that creates DependentString.
  // Caller should call generateFallback after masm.ret(), to generate
  // fallback path.
  void generate(MacroAssembler& masm, const JSAtomState& names,
                CompileRuntime* runtime, Register base,
                BaseIndex startIndexAddress, BaseIndex limitIndexAddress,
                gc::Heap initialStringHeap);

  // Generate fallback path for creating DependentString.
  void generateFallback(MacroAssembler& masm);
};

void CreateDependentString::generate(MacroAssembler& masm,
                                     const JSAtomState& names,
                                     CompileRuntime* runtime, Register base,
                                     BaseIndex startIndexAddress,
                                     BaseIndex limitIndexAddress,
                                     gc::Heap initialStringHeap) {
  JitSpew(JitSpew_Codegen, "# Emitting CreateDependentString (encoding=%s)",
          (encoding_ == CharEncoding::Latin1 ? "Latin-1" : "Two-Byte"));

  auto newGCString = [&](FallbackKind kind) {
    uint32_t flags = kind == FallbackKind::InlineString
                         ? JSString::INIT_THIN_INLINE_FLAGS
                     : kind == FallbackKind::FatInlineString
                         ? JSString::INIT_FAT_INLINE_FLAGS
                         : JSString::INIT_DEPENDENT_FLAGS;
    if (encoding_ == CharEncoding::Latin1) {
      flags |= JSString::LATIN1_CHARS_BIT;
    }

    if (kind != FallbackKind::FatInlineString) {
      masm.newGCString(string_, temp2_, initialStringHeap, &fallbacks_[kind]);
    } else {
      masm.newGCFatInlineString(string_, temp2_, initialStringHeap,
                                &fallbacks_[kind]);
    }
    masm.bind(&joins_[kind]);
    masm.store32(Imm32(flags), Address(string_, JSString::offsetOfFlags()));
  };

  // Compute the string length.
  masm.load32(startIndexAddress, temp2_);
  masm.load32(limitIndexAddress, temp1_);
  masm.sub32(temp2_, temp1_);

  Label done, nonEmpty;

  // Zero length matches use the empty string.
  masm.branchTest32(Assembler::NonZero, temp1_, temp1_, &nonEmpty);
  masm.movePtr(ImmGCPtr(names.empty_), string_);
  masm.jump(&done);

  masm.bind(&nonEmpty);

  // Complete matches use the base string.
  Label nonBaseStringMatch;
  masm.branchTest32(Assembler::NonZero, temp2_, temp2_, &nonBaseStringMatch);
  masm.branch32(Assembler::NotEqual, Address(base, JSString::offsetOfLength()),
                temp1_, &nonBaseStringMatch);
  masm.movePtr(base, string_);
  masm.jump(&done);

  masm.bind(&nonBaseStringMatch);

  Label notInline;

  int32_t maxInlineLength = encoding_ == CharEncoding::Latin1
                                ? JSFatInlineString::MAX_LENGTH_LATIN1
                                : JSFatInlineString::MAX_LENGTH_TWO_BYTE;
  masm.branch32(Assembler::Above, temp1_, Imm32(maxInlineLength), &notInline);
  {
    // Make a thin or fat inline string.
    Label stringAllocated, fatInline;

    int32_t maxThinInlineLength = encoding_ == CharEncoding::Latin1
                                      ? JSThinInlineString::MAX_LENGTH_LATIN1
                                      : JSThinInlineString::MAX_LENGTH_TWO_BYTE;
    masm.branch32(Assembler::Above, temp1_, Imm32(maxThinInlineLength),
                  &fatInline);
    if (encoding_ == CharEncoding::Latin1) {
      // One character Latin-1 strings can be loaded directly from the
      // static strings table.
      Label thinInline;
      masm.branch32(Assembler::Above, temp1_, Imm32(1), &thinInline);
      {
        static_assert(
            StaticStrings::UNIT_STATIC_LIMIT - 1 == JSString::MAX_LATIN1_CHAR,
            "Latin-1 strings can be loaded from static strings");

        masm.loadStringChars(base, temp1_, encoding_);
        masm.loadChar(temp1_, temp2_, temp1_, encoding_);

        masm.lookupStaticString(temp1_, string_, runtime->staticStrings());

        masm.jump(&done);
      }
      masm.bind(&thinInline);
    }
    {
      newGCString(FallbackKind::InlineString);
      masm.jump(&stringAllocated);
    }
    masm.bind(&fatInline);
    {
      newGCString(FallbackKind::FatInlineString);
    }
    masm.bind(&stringAllocated);

    masm.store32(temp1_, Address(string_, JSString::offsetOfLength()));

    masm.push(string_);
    masm.push(base);

    MOZ_ASSERT(startIndexAddress.base == FramePointer,
               "startIndexAddress is still valid after stack pushes");

    // Load chars pointer for the new string.
    masm.loadInlineStringCharsForStore(string_, string_);

    // Load the source characters pointer.
    masm.loadStringChars(base, temp2_, encoding_);
    masm.load32(startIndexAddress, base);
    masm.addToCharPtr(temp2_, base, encoding_);

    CopyStringChars(masm, string_, temp2_, temp1_, base, encoding_);

    masm.pop(base);
    masm.pop(string_);

    masm.jump(&done);
  }

  masm.bind(&notInline);

  {
    // Make a dependent string.
    // Warning: string may be tenured (if the fallback case is hit), so
    // stores into it must be post barriered.
    newGCString(FallbackKind::NotInlineString);

    masm.store32(temp1_, Address(string_, JSString::offsetOfLength()));

    masm.loadNonInlineStringChars(base, temp1_, encoding_);
    masm.load32(startIndexAddress, temp2_);
    masm.addToCharPtr(temp1_, temp2_, encoding_);
    masm.storeNonInlineStringChars(temp1_, string_);

    EmitInitDependentStringBase(masm, string_, base, temp1_, temp2_,
                                /* needsPostBarrier = */ true);
  }

  masm.bind(&done);
}

void CreateDependentString::generateFallback(MacroAssembler& masm) {
  JitSpew(JitSpew_Codegen,
          "# Emitting CreateDependentString fallback (encoding=%s)",
          (encoding_ == CharEncoding::Latin1 ? "Latin-1" : "Two-Byte"));

  LiveRegisterSet regsToSave(RegisterSet::Volatile());
  regsToSave.takeUnchecked(string_);
  regsToSave.takeUnchecked(temp2_);

  for (FallbackKind kind : mozilla::MakeEnumeratedRange(FallbackKind::Count)) {
    masm.bind(&fallbacks_[kind]);

    masm.PushRegsInMask(regsToSave);

    using Fn = void* (*)(JSContext* cx);
    masm.setupUnalignedABICall(string_);
    masm.loadJSContext(string_);
    masm.passABIArg(string_);
    if (kind == FallbackKind::FatInlineString) {
      masm.callWithABI<Fn, AllocateFatInlineString>();
    } else {
      masm.callWithABI<Fn, AllocateDependentString>();
    }
    masm.storeCallPointerResult(string_);

    masm.PopRegsInMask(regsToSave);

    masm.branchPtr(Assembler::Equal, string_, ImmWord(0), failure_);

    masm.jump(&joins_[kind]);
  }
}

// Generate the RegExpMatcher and RegExpExecMatch stubs. These are very similar,
// but RegExpExecMatch also has to load and update .lastIndex for global/sticky
// regular expressions.
static JitCode* GenerateRegExpMatchStubShared(JSContext* cx,
                                              gc::Heap initialStringHeap,
                                              bool isExecMatch) {
  if (isExecMatch) {
    JitSpew(JitSpew_Codegen, "# Emitting RegExpExecMatch stub");
  } else {
    JitSpew(JitSpew_Codegen, "# Emitting RegExpMatcher stub");
  }

  // |initialStringHeap| could be stale after a GC.
  JS::AutoCheckCannotGC nogc(cx);

  Register regexp = RegExpMatcherRegExpReg;
  Register input = RegExpMatcherStringReg;
  Register lastIndex = RegExpMatcherLastIndexReg;
  ValueOperand result = JSReturnOperand;

  // We are free to clobber all registers, as LRegExpMatcher is a call
  // instruction.
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(input);
  regs.take(regexp);
  regs.take(lastIndex);

  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();
  Register temp3 = regs.takeAny();
  Register maybeTemp4 = InvalidReg;
  if (!regs.empty()) {
    // There are not enough registers on x86.
    maybeTemp4 = regs.takeAny();
  }
  Register maybeTemp5 = InvalidReg;
  if (!regs.empty()) {
    // There are not enough registers on x86.
    maybeTemp5 = regs.takeAny();
  }

  Address flagsSlot(regexp, RegExpObject::offsetOfFlags());
  Address lastIndexSlot(regexp, RegExpObject::offsetOfLastIndex());

  TempAllocator temp(&cx->tempLifoAlloc());
  JitContext jcx(cx);
  StackMacroAssembler masm(cx, temp);
  AutoCreatedBy acb(masm, "GenerateRegExpMatchStubShared");

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif
  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  Label notFoundZeroLastIndex;
  if (isExecMatch) {
    masm.loadRegExpLastIndex(regexp, input, lastIndex, &notFoundZeroLastIndex);
  }

  // The InputOutputData is placed above the frame pointer and return address on
  // the stack.
  int32_t inputOutputDataStartOffset = 2 * sizeof(void*);

  Label notFound, oolEntry;
  if (!PrepareAndExecuteRegExp(masm, regexp, input, lastIndex, temp1, temp2,
                               temp3, inputOutputDataStartOffset,
                               initialStringHeap, &notFound, &oolEntry)) {
    return nullptr;
  }

  // If a regexp has named captures, fall back to the OOL stub, which
  // will end up calling CreateRegExpMatchResults.
  Register shared = temp2;
  masm.unboxNonDouble(Address(regexp, NativeObject::getFixedSlotOffset(
                                          RegExpObject::SHARED_SLOT)),
                      shared, JSVAL_TYPE_PRIVATE_GCTHING);
  masm.branchPtr(Assembler::NotEqual,
                 Address(shared, RegExpShared::offsetOfGroupsTemplate()),
                 ImmWord(0), &oolEntry);

  // Similarly, if the |hasIndices| flag is set, fall back to the OOL stub.
  masm.branchTest32(Assembler::NonZero,
                    Address(shared, RegExpShared::offsetOfFlags()),
                    Imm32(int32_t(JS::RegExpFlag::HasIndices)), &oolEntry);

  Address pairCountAddress =
      RegExpPairCountAddress(masm, inputOutputDataStartOffset);

  // Construct the result.
  Register object = temp1;
  {
    // In most cases, the array will have just 1-2 elements, so we optimize for
    // that by emitting separate code paths for capacity 2/6/14 (= 4/8/16 slots
    // because two slots are used for the elements header).

    // Load the array length in temp2 and the shape in temp3.
    Label allocated;
    masm.load32(pairCountAddress, temp2);
    size_t offset = GlobalObjectData::offsetOfRegExpRealm() +
                    RegExpRealm::offsetOfNormalMatchResultShape();
    masm.loadGlobalObjectData(temp3);
    masm.loadPtr(Address(temp3, offset), temp3);

    auto emitAllocObject = [&](size_t elementCapacity) {
      gc::AllocKind kind = GuessArrayGCKind(elementCapacity);
      MOZ_ASSERT(gc::GetObjectFinalizeKind(&ArrayObject::class_) ==
                 gc::FinalizeKind::None);
      MOZ_ASSERT(!IsFinalizedKind(kind));

#ifdef DEBUG
      // Assert all of the available slots are used for |elementCapacity|
      // elements.
      size_t usedSlots = ObjectElements::VALUES_PER_HEADER + elementCapacity;
      MOZ_ASSERT(usedSlots == GetGCKindSlots(kind));
#endif

      constexpr size_t numUsedDynamicSlots =
          RegExpRealm::MatchResultObjectSlotSpan;
      constexpr size_t numDynamicSlots =
          RegExpRealm::MatchResultObjectNumDynamicSlots;
      constexpr size_t arrayLength = 1;
      masm.createArrayWithFixedElements(object, temp3, temp2, temp3,
                                        arrayLength, elementCapacity,
                                        numUsedDynamicSlots, numDynamicSlots,
                                        kind, gc::Heap::Default, &oolEntry);
    };

    Label moreThan2;
    masm.branch32(Assembler::Above, temp2, Imm32(2), &moreThan2);
    emitAllocObject(2);
    masm.jump(&allocated);

    Label moreThan6;
    masm.bind(&moreThan2);
    masm.branch32(Assembler::Above, temp2, Imm32(6), &moreThan6);
    emitAllocObject(6);
    masm.jump(&allocated);

    masm.bind(&moreThan6);
    static_assert(RegExpObject::MaxPairCount == 14);
    emitAllocObject(RegExpObject::MaxPairCount);

    masm.bind(&allocated);
  }

  // clang-format off
   /*
    * [SMDOC] Stack layout for the RegExpMatcher stub
    *
    *                                    +---------------+
    *               FramePointer +-----> |Caller-FramePtr|
    *                                    +---------------+
    *                                    |Return-Address |
    *                                    +---------------+
    * inputOutputDataStartOffset +-----> +---------------+
    *                                    |InputOutputData|
    *                                    +---------------+
    *                                    +---------------+
    *                                    |  MatchPairs   |
    *           pairsCountAddress +----------->  count   |
    *                                    |       pairs   |
    *                                    |               |
    *                                    +---------------+
    *     pairsVectorStartOffset +-----> +---------------+
    *                                    |   MatchPair   |
    *             matchPairStart +------------>  start   |  <-------+
    *             matchPairLimit +------------>  limit   |          | Reserved space for
    *                                    +---------------+          | `RegExpObject::MaxPairCount`
    *                                           .                   | MatchPair objects.
    *                                           .                   |
    *                                           .                   | `count` objects will be
    *                                    +---------------+          | initialized and can be
    *                                    |   MatchPair   |          | accessed below.
    *                                    |       start   |  <-------+
    *                                    |       limit   |
    *                                    +---------------+
    */
  // clang-format on

  static_assert(sizeof(MatchPair) == 2 * sizeof(int32_t),
                "MatchPair consists of two int32 values representing the start"
                "and the end offset of the match");

  int32_t pairsVectorStartOffset =
      RegExpPairsVectorStartOffset(inputOutputDataStartOffset);

  // Incremented by one below for each match pair.
  Register matchIndex = temp2;
  masm.move32(Imm32(0), matchIndex);

  // The element in which to store the result of the current match.
  size_t elementsOffset = NativeObject::offsetOfFixedElements();
  BaseObjectElementIndex objectMatchElement(object, matchIndex, elementsOffset);

  // The current match pair's "start" and "limit" member.
  BaseIndex matchPairStart(FramePointer, matchIndex, TimesEight,
                           pairsVectorStartOffset + MatchPair::offsetOfStart());
  BaseIndex matchPairLimit(FramePointer, matchIndex, TimesEight,
                           pairsVectorStartOffset + MatchPair::offsetOfLimit());

  Label* depStrFailure = &oolEntry;
  Label restoreRegExpAndLastIndex;

  Register temp4;
  if (maybeTemp4 == InvalidReg) {
    depStrFailure = &restoreRegExpAndLastIndex;

    // We don't have enough registers for a fourth temporary. Reuse |regexp|
    // as a temporary. We restore its value at |restoreRegExpAndLastIndex|.
    masm.push(regexp);
    temp4 = regexp;
  } else {
    temp4 = maybeTemp4;
  }

  Register temp5;
  if (maybeTemp5 == InvalidReg) {
    depStrFailure = &restoreRegExpAndLastIndex;

    // We don't have enough registers for a fifth temporary. Reuse |lastIndex|
    // as a temporary. We restore its value at |restoreRegExpAndLastIndex|.
    masm.push(lastIndex);
    temp5 = lastIndex;
  } else {
    temp5 = maybeTemp5;
  }

  auto maybeRestoreRegExpAndLastIndex = [&]() {
    if (maybeTemp5 == InvalidReg) {
      masm.pop(lastIndex);
    }
    if (maybeTemp4 == InvalidReg) {
      masm.pop(regexp);
    }
  };

  // Loop to construct the match strings. There are two different loops,
  // depending on whether the input is a Two-Byte or a Latin-1 string.
  CreateDependentString depStrs[]{
      {CharEncoding::TwoByte, temp3, temp4, temp5, depStrFailure},
      {CharEncoding::Latin1, temp3, temp4, temp5, depStrFailure},
  };

  {
    Label isLatin1, done;
    masm.branchLatin1String(input, &isLatin1);

    for (auto& depStr : depStrs) {
      if (depStr.encoding() == CharEncoding::Latin1) {
        masm.bind(&isLatin1);
      }

      Label matchLoop;
      masm.bind(&matchLoop);

      static_assert(MatchPair::NoMatch == -1,
                    "MatchPair::start is negative if no match was found");

      Label isUndefined, storeDone;
      masm.branch32(Assembler::LessThan, matchPairStart, Imm32(0),
                    &isUndefined);
      {
        depStr.generate(masm, cx->names(), CompileRuntime::get(cx->runtime()),
                        input, matchPairStart, matchPairLimit,
                        initialStringHeap);

        // Storing into nursery-allocated results object's elements; no post
        // barrier.
        masm.storeValue(JSVAL_TYPE_STRING, depStr.string(), objectMatchElement);
        masm.jump(&storeDone);
      }
      masm.bind(&isUndefined);
      {
        masm.storeValue(UndefinedValue(), objectMatchElement);
      }
      masm.bind(&storeDone);

      masm.add32(Imm32(1), matchIndex);
      masm.branch32(Assembler::LessThanOrEqual, pairCountAddress, matchIndex,
                    &done);
      masm.jump(&matchLoop);
    }

#ifdef DEBUG
    masm.assumeUnreachable("The match string loop doesn't fall through.");
#endif

    masm.bind(&done);
  }

  maybeRestoreRegExpAndLastIndex();

  // Fill in the rest of the output object.
  masm.store32(
      matchIndex,
      Address(object,
              elementsOffset + ObjectElements::offsetOfInitializedLength()));
  masm.store32(
      matchIndex,
      Address(object, elementsOffset + ObjectElements::offsetOfLength()));

  Address firstMatchPairStartAddress(
      FramePointer, pairsVectorStartOffset + MatchPair::offsetOfStart());
  Address firstMatchPairLimitAddress(
      FramePointer, pairsVectorStartOffset + MatchPair::offsetOfLimit());

  static_assert(RegExpRealm::MatchResultObjectIndexSlot == 0,
                "First slot holds the 'index' property");
  static_assert(RegExpRealm::MatchResultObjectInputSlot == 1,
                "Second slot holds the 'input' property");

  masm.loadPtr(Address(object, NativeObject::offsetOfSlots()), temp2);

  masm.load32(firstMatchPairStartAddress, temp3);
  masm.storeValue(JSVAL_TYPE_INT32, temp3, Address(temp2, 0));

  // No post barrier needed (address is within nursery object.)
  masm.storeValue(JSVAL_TYPE_STRING, input, Address(temp2, sizeof(Value)));

  // For the ExecMatch stub, if the regular expression is global or sticky, we
  // have to update its .lastIndex slot.
  if (isExecMatch) {
    MOZ_ASSERT(object != lastIndex);
    Label notGlobalOrSticky;
    masm.branchTest32(Assembler::Zero, flagsSlot,
                      Imm32(JS::RegExpFlag::Global | JS::RegExpFlag::Sticky),
                      &notGlobalOrSticky);
    masm.load32(firstMatchPairLimitAddress, lastIndex);
    masm.storeValue(JSVAL_TYPE_INT32, lastIndex, lastIndexSlot);
    masm.bind(&notGlobalOrSticky);
  }

  // All done!
  masm.tagValue(JSVAL_TYPE_OBJECT, object, result);
  masm.pop(FramePointer);
  masm.ret();

  masm.bind(&notFound);
  if (isExecMatch) {
    Label notGlobalOrSticky;
    masm.branchTest32(Assembler::Zero, flagsSlot,
                      Imm32(JS::RegExpFlag::Global | JS::RegExpFlag::Sticky),
                      &notGlobalOrSticky);
    masm.bind(&notFoundZeroLastIndex);
    masm.storeValue(Int32Value(0), lastIndexSlot);
    masm.bind(&notGlobalOrSticky);
  }
  masm.moveValue(NullValue(), result);
  masm.pop(FramePointer);
  masm.ret();

  // Fallback paths for CreateDependentString.
  for (auto& depStr : depStrs) {
    depStr.generateFallback(masm);
  }

  // Fall-through to the ool entry after restoring the registers.
  masm.bind(&restoreRegExpAndLastIndex);
  maybeRestoreRegExpAndLastIndex();

  // Use an undefined value to signal to the caller that the OOL stub needs to
  // be called.
  masm.bind(&oolEntry);
  masm.moveValue(UndefinedValue(), result);
  masm.pop(FramePointer);
  masm.ret();

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);
  if (!code) {
    return nullptr;
  }

  const char* name = isExecMatch ? "RegExpExecMatchStub" : "RegExpMatcherStub";
  CollectPerfSpewerJitCodeProfile(code, name);
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, name);
#endif

  return code;
}

JitCode* JitZone::generateRegExpMatcherStub(JSContext* cx) {
  return GenerateRegExpMatchStubShared(cx, initialStringHeap,
                                       /* isExecMatch = */ false);
}

JitCode* JitZone::generateRegExpExecMatchStub(JSContext* cx) {
  return GenerateRegExpMatchStubShared(cx, initialStringHeap,
                                       /* isExecMatch = */ true);
}

void CodeGenerator::visitRegExpMatcher(LRegExpMatcher* lir) {
  MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpMatcherRegExpReg);
  MOZ_ASSERT(ToRegister(lir->string()) == RegExpMatcherStringReg);
  MOZ_ASSERT(ToRegister(lir->lastIndex()) == RegExpMatcherLastIndexReg);
  MOZ_ASSERT(ToOutValue(lir) == JSReturnOperand);

#if defined(JS_NUNBOX32)
  static_assert(RegExpMatcherRegExpReg != JSReturnReg_Type);
  static_assert(RegExpMatcherRegExpReg != JSReturnReg_Data);
  static_assert(RegExpMatcherStringReg != JSReturnReg_Type);
  static_assert(RegExpMatcherStringReg != JSReturnReg_Data);
  static_assert(RegExpMatcherLastIndexReg != JSReturnReg_Type);
  static_assert(RegExpMatcherLastIndexReg != JSReturnReg_Data);
#elif defined(JS_PUNBOX64)
  static_assert(RegExpMatcherRegExpReg != JSReturnReg);
  static_assert(RegExpMatcherStringReg != JSReturnReg);
  static_assert(RegExpMatcherLastIndexReg != JSReturnReg);
#endif

  masm.reserveStack(RegExpReservedStack);

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Register lastIndex = ToRegister(lir->lastIndex());
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(lastIndex);
    regs.take(input);
    regs.take(regexp);
    Register temp = regs.takeAny();

    masm.computeEffectiveAddress(
        Address(masm.getStackPointer(), InputOutputDataSize), temp);

    pushArg(temp);
    pushArg(lastIndex);
    pushArg(input);
    pushArg(regexp);

    // We are not using oolCallVM because we are in a Call, and that live
    // registers are already saved by the the register allocator.
    using Fn = bool (*)(JSContext*, HandleObject regexp, HandleString input,
                        int32_t lastIndex, MatchPairs* pairs,
                        MutableHandleValue output);
    callVM<Fn, RegExpMatcherRaw>(lir);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  JitCode* regExpMatcherStub =
      snapshot_->getZoneStub(JitZone::StubKind::RegExpMatcher);
  masm.call(regExpMatcherStub);
  masm.branchTestUndefined(Assembler::Equal, JSReturnOperand, ool->entry());
  masm.bind(ool->rejoin());

  masm.freeStack(RegExpReservedStack);
}

void CodeGenerator::visitRegExpExecMatch(LRegExpExecMatch* lir) {
  MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpMatcherRegExpReg);
  MOZ_ASSERT(ToRegister(lir->string()) == RegExpMatcherStringReg);
  MOZ_ASSERT(ToOutValue(lir) == JSReturnOperand);

#if defined(JS_NUNBOX32)
  static_assert(RegExpMatcherRegExpReg != JSReturnReg_Type);
  static_assert(RegExpMatcherRegExpReg != JSReturnReg_Data);
  static_assert(RegExpMatcherStringReg != JSReturnReg_Type);
  static_assert(RegExpMatcherStringReg != JSReturnReg_Data);
#elif defined(JS_PUNBOX64)
  static_assert(RegExpMatcherRegExpReg != JSReturnReg);
  static_assert(RegExpMatcherStringReg != JSReturnReg);
#endif

  masm.reserveStack(RegExpReservedStack);

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(input);
    regs.take(regexp);
    Register temp = regs.takeAny();

    masm.computeEffectiveAddress(
        Address(masm.getStackPointer(), InputOutputDataSize), temp);

    pushArg(temp);
    pushArg(input);
    pushArg(regexp);

    // We are not using oolCallVM because we are in a Call and live registers
    // have already been saved by the register allocator.
    using Fn =
        bool (*)(JSContext*, Handle<RegExpObject*> regexp, HandleString input,
                 MatchPairs* pairs, MutableHandleValue output);
    callVM<Fn, RegExpBuiltinExecMatchFromJit>(lir);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  JitCode* regExpExecMatchStub =
      snapshot_->getZoneStub(JitZone::StubKind::RegExpExecMatch);
  masm.call(regExpExecMatchStub);
  masm.branchTestUndefined(Assembler::Equal, JSReturnOperand, ool->entry());

  masm.bind(ool->rejoin());
  masm.freeStack(RegExpReservedStack);
}

JitCode* JitZone::generateRegExpSearcherStub(JSContext* cx) {
  JitSpew(JitSpew_Codegen, "# Emitting RegExpSearcher stub");

  Register regexp = RegExpSearcherRegExpReg;
  Register input = RegExpSearcherStringReg;
  Register lastIndex = RegExpSearcherLastIndexReg;
  Register result = ReturnReg;

  // We are free to clobber all registers, as LRegExpSearcher is a call
  // instruction.
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(input);
  regs.take(regexp);
  regs.take(lastIndex);

  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();
  Register temp3 = regs.takeAny();

  TempAllocator temp(&cx->tempLifoAlloc());
  JitContext jcx(cx);
  StackMacroAssembler masm(cx, temp);
  AutoCreatedBy acb(masm, "JitZone::generateRegExpSearcherStub");

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif
  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

#ifdef DEBUG
  // Store sentinel value to cx->regExpSearcherLastLimit.
  // See comment in RegExpSearcherImpl.
  masm.loadJSContext(temp1);
  masm.store32(Imm32(RegExpSearcherLastLimitSentinel),
               Address(temp1, JSContext::offsetOfRegExpSearcherLastLimit()));
#endif

  // The InputOutputData is placed above the frame pointer and return address on
  // the stack.
  int32_t inputOutputDataStartOffset = 2 * sizeof(void*);

  Label notFound, oolEntry;
  if (!PrepareAndExecuteRegExp(masm, regexp, input, lastIndex, temp1, temp2,
                               temp3, inputOutputDataStartOffset,
                               initialStringHeap, &notFound, &oolEntry)) {
    return nullptr;
  }

  // clang-format off
    /*
     * [SMDOC] Stack layout for the RegExpSearcher stub
     *
     *                                    +---------------+
     *               FramePointer +-----> |Caller-FramePtr|
     *                                    +---------------+
     *                                    |Return-Address |
     *                                    +---------------+
     * inputOutputDataStartOffset +-----> +---------------+
     *                                    |InputOutputData|
     *                                    +---------------+
     *                                    +---------------+
     *                                    |  MatchPairs   |
     *                                    |       count   |
     *                                    |       pairs   |
     *                                    |               |
     *                                    +---------------+
     *     pairsVectorStartOffset +-----> +---------------+
     *                                    |   MatchPair   |
     *             matchPairStart +------------>  start   |  <-------+
     *             matchPairLimit +------------>  limit   |          | Reserved space for
     *                                    +---------------+          | `RegExpObject::MaxPairCount`
     *                                           .                   | MatchPair objects.
     *                                           .                   |
     *                                           .                   | Only a single object will
     *                                    +---------------+          | be initialized and can be
     *                                    |   MatchPair   |          | accessed below.
     *                                    |       start   |  <-------+
     *                                    |       limit   |
     *                                    +---------------+
     */
  // clang-format on

  int32_t pairsVectorStartOffset =
      RegExpPairsVectorStartOffset(inputOutputDataStartOffset);
  Address matchPairStart(FramePointer,
                         pairsVectorStartOffset + MatchPair::offsetOfStart());
  Address matchPairLimit(FramePointer,
                         pairsVectorStartOffset + MatchPair::offsetOfLimit());

  // Store match limit to cx->regExpSearcherLastLimit and return the index.
  masm.load32(matchPairLimit, result);
  masm.loadJSContext(input);
  masm.store32(result,
               Address(input, JSContext::offsetOfRegExpSearcherLastLimit()));
  masm.load32(matchPairStart, result);
  masm.pop(FramePointer);
  masm.ret();

  masm.bind(&notFound);
  masm.move32(Imm32(RegExpSearcherResultNotFound), result);
  masm.pop(FramePointer);
  masm.ret();

  masm.bind(&oolEntry);
  masm.move32(Imm32(RegExpSearcherResultFailed), result);
  masm.pop(FramePointer);
  masm.ret();

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);
  if (!code) {
    return nullptr;
  }

  CollectPerfSpewerJitCodeProfile(code, "RegExpSearcherStub");
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, "RegExpSearcherStub");
#endif

  return code;
}

void CodeGenerator::visitRegExpSearcher(LRegExpSearcher* lir) {
  MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpSearcherRegExpReg);
  MOZ_ASSERT(ToRegister(lir->string()) == RegExpSearcherStringReg);
  MOZ_ASSERT(ToRegister(lir->lastIndex()) == RegExpSearcherLastIndexReg);
  MOZ_ASSERT(ToRegister(lir->output()) == ReturnReg);

  static_assert(RegExpSearcherRegExpReg != ReturnReg);
  static_assert(RegExpSearcherStringReg != ReturnReg);
  static_assert(RegExpSearcherLastIndexReg != ReturnReg);

  masm.reserveStack(RegExpReservedStack);

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Register lastIndex = ToRegister(lir->lastIndex());
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(lastIndex);
    regs.take(input);
    regs.take(regexp);
    Register temp = regs.takeAny();

    masm.computeEffectiveAddress(
        Address(masm.getStackPointer(), InputOutputDataSize), temp);

    pushArg(temp);
    pushArg(lastIndex);
    pushArg(input);
    pushArg(regexp);

    // We are not using oolCallVM because we are in a Call, and that live
    // registers are already saved by the the register allocator.
    using Fn = bool (*)(JSContext* cx, HandleObject regexp, HandleString input,
                        int32_t lastIndex, MatchPairs* pairs, int32_t* result);
    callVM<Fn, RegExpSearcherRaw>(lir);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  JitCode* regExpSearcherStub =
      snapshot_->getZoneStub(JitZone::StubKind::RegExpSearcher);
  masm.call(regExpSearcherStub);
  masm.branch32(Assembler::Equal, ReturnReg, Imm32(RegExpSearcherResultFailed),
                ool->entry());
  masm.bind(ool->rejoin());

  masm.freeStack(RegExpReservedStack);
}

void CodeGenerator::visitRegExpSearcherLastLimit(
    LRegExpSearcherLastLimit* lir) {
  Register result = ToRegister(lir->output());
  Register scratch = ToRegister(lir->temp0());

  masm.loadAndClearRegExpSearcherLastLimit(result, scratch);
}

JitCode* JitZone::generateRegExpExecTestStub(JSContext* cx) {
  JitSpew(JitSpew_Codegen, "# Emitting RegExpExecTest stub");

  Register regexp = RegExpExecTestRegExpReg;
  Register input = RegExpExecTestStringReg;
  Register result = ReturnReg;

  TempAllocator temp(&cx->tempLifoAlloc());
  JitContext jcx(cx);
  StackMacroAssembler masm(cx, temp);
  AutoCreatedBy acb(masm, "JitZone::generateRegExpExecTestStub");

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif
  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  // We are free to clobber all registers, as LRegExpExecTest is a call
  // instruction.
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(input);
  regs.take(regexp);

  // Ensure lastIndex != result.
  regs.take(result);
  Register lastIndex = regs.takeAny();
  regs.add(result);
  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();
  Register temp3 = regs.takeAny();

  Address flagsSlot(regexp, RegExpObject::offsetOfFlags());
  Address lastIndexSlot(regexp, RegExpObject::offsetOfLastIndex());

  masm.reserveStack(RegExpReservedStack);

  // Load lastIndex and skip RegExp execution if needed.
  Label notFoundZeroLastIndex;
  masm.loadRegExpLastIndex(regexp, input, lastIndex, &notFoundZeroLastIndex);

  // In visitRegExpMatcher and visitRegExpSearcher, we reserve stack space
  // before calling the stub. For RegExpExecTest we call the stub before
  // reserving stack space, so the offset of the InputOutputData relative to the
  // frame pointer is negative.
  constexpr int32_t inputOutputDataStartOffset = -int32_t(RegExpReservedStack);

  // On ARM64, load/store instructions can encode an immediate offset in the
  // range [-256, 4095]. If we ever fail this assertion, it would be more
  // efficient to store the data above the frame pointer similar to
  // RegExpMatcher and RegExpSearcher.
  static_assert(inputOutputDataStartOffset >= -256);

  Label notFound, oolEntry;
  if (!PrepareAndExecuteRegExp(masm, regexp, input, lastIndex, temp1, temp2,
                               temp3, inputOutputDataStartOffset,
                               initialStringHeap, &notFound, &oolEntry)) {
    return nullptr;
  }

  // Set `result` to true/false to indicate found/not-found, or to
  // RegExpExecTestResultFailed if we have to retry in C++. If the regular
  // expression is global or sticky, we also have to update its .lastIndex slot.

  Label done;
  int32_t pairsVectorStartOffset =
      RegExpPairsVectorStartOffset(inputOutputDataStartOffset);
  Address matchPairLimit(FramePointer,
                         pairsVectorStartOffset + MatchPair::offsetOfLimit());

  masm.move32(Imm32(1), result);
  masm.branchTest32(Assembler::Zero, flagsSlot,
                    Imm32(JS::RegExpFlag::Global | JS::RegExpFlag::Sticky),
                    &done);
  masm.load32(matchPairLimit, lastIndex);
  masm.storeValue(JSVAL_TYPE_INT32, lastIndex, lastIndexSlot);
  masm.jump(&done);

  masm.bind(&notFound);
  masm.move32(Imm32(0), result);
  masm.branchTest32(Assembler::Zero, flagsSlot,
                    Imm32(JS::RegExpFlag::Global | JS::RegExpFlag::Sticky),
                    &done);
  masm.storeValue(Int32Value(0), lastIndexSlot);
  masm.jump(&done);

  masm.bind(&notFoundZeroLastIndex);
  masm.move32(Imm32(0), result);
  masm.storeValue(Int32Value(0), lastIndexSlot);
  masm.jump(&done);

  masm.bind(&oolEntry);
  masm.move32(Imm32(RegExpExecTestResultFailed), result);

  masm.bind(&done);
  masm.freeStack(RegExpReservedStack);
  masm.pop(FramePointer);
  masm.ret();

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);
  if (!code) {
    return nullptr;
  }

  CollectPerfSpewerJitCodeProfile(code, "RegExpExecTestStub");
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, "RegExpExecTestStub");
#endif

  return code;
}

void CodeGenerator::visitRegExpExecTest(LRegExpExecTest* lir) {
  MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpExecTestRegExpReg);
  MOZ_ASSERT(ToRegister(lir->string()) == RegExpExecTestStringReg);
  MOZ_ASSERT(ToRegister(lir->output()) == ReturnReg);

  static_assert(RegExpExecTestRegExpReg != ReturnReg);
  static_assert(RegExpExecTestStringReg != ReturnReg);

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    pushArg(input);
    pushArg(regexp);

    // We are not using oolCallVM because we are in a Call and live registers
    // have already been saved by the register allocator.
    using Fn = bool (*)(JSContext* cx, Handle<RegExpObject*> regexp,
                        HandleString input, bool* result);
    callVM<Fn, RegExpBuiltinExecTestFromJit>(lir);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  JitCode* regExpExecTestStub =
      snapshot_->getZoneStub(JitZone::StubKind::RegExpExecTest);
  masm.call(regExpExecTestStub);

  masm.branch32(Assembler::Equal, ReturnReg, Imm32(RegExpExecTestResultFailed),
                ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitRegExpHasCaptureGroups(LRegExpHasCaptureGroups* ins) {
  Register regexp = ToRegister(ins->regexp());
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  using Fn =
      bool (*)(JSContext*, Handle<RegExpObject*>, Handle<JSString*>, bool*);
  auto* ool = oolCallVM<Fn, js::RegExpHasCaptureGroups>(
      ins, ArgList(regexp, input), StoreRegisterTo(output));

  // Load RegExpShared in |output|.
  Label vmCall;
  masm.loadParsedRegExpShared(regexp, output, ool->entry());

  // Return true iff pairCount > 1.
  Label returnTrue;
  masm.branch32(Assembler::Above,
                Address(output, RegExpShared::offsetOfPairCount()), Imm32(1),
                &returnTrue);
  masm.move32(Imm32(0), output);
  masm.jump(ool->rejoin());

  masm.bind(&returnTrue);
  masm.move32(Imm32(1), output);

  masm.bind(ool->rejoin());
}

static void FindFirstDollarIndex(MacroAssembler& masm, Register str,
                                 Register len, Register temp0, Register temp1,
                                 Register output, CharEncoding encoding) {
#ifdef DEBUG
  Label ok;
  masm.branch32(Assembler::GreaterThan, len, Imm32(0), &ok);
  masm.assumeUnreachable("Length should be greater than 0.");
  masm.bind(&ok);
#endif

  Register chars = temp0;
  masm.loadStringChars(str, chars, encoding);

  masm.move32(Imm32(0), output);

  Label start, done;
  masm.bind(&start);

  Register currentChar = temp1;
  masm.loadChar(chars, output, currentChar, encoding);
  masm.branch32(Assembler::Equal, currentChar, Imm32('$'), &done);

  masm.add32(Imm32(1), output);
  masm.branch32(Assembler::NotEqual, output, len, &start);

  masm.move32(Imm32(-1), output);

  masm.bind(&done);
}

void CodeGenerator::visitGetFirstDollarIndex(LGetFirstDollarIndex* ins) {
  Register str = ToRegister(ins->str());
  Register output = ToRegister(ins->output());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register len = ToRegister(ins->temp2());

  using Fn = bool (*)(JSContext*, JSString*, int32_t*);
  OutOfLineCode* ool = oolCallVM<Fn, GetFirstDollarIndexRaw>(
      ins, ArgList(str), StoreRegisterTo(output));

  masm.branchIfRope(str, ool->entry());
  masm.loadStringLength(str, len);

  Label isLatin1, done;
  masm.branchLatin1String(str, &isLatin1);
  {
    FindFirstDollarIndex(masm, str, len, temp0, temp1, output,
                         CharEncoding::TwoByte);
    masm.jump(&done);
  }
  masm.bind(&isLatin1);
  {
    FindFirstDollarIndex(masm, str, len, temp0, temp1, output,
                         CharEncoding::Latin1);
  }
  masm.bind(&done);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitStringReplace(LStringReplace* lir) {
  if (lir->replacement()->isConstant()) {
    pushArg(ImmGCPtr(lir->replacement()->toConstant()->toString()));
  } else {
    pushArg(ToRegister(lir->replacement()));
  }

  if (lir->pattern()->isConstant()) {
    pushArg(ImmGCPtr(lir->pattern()->toConstant()->toString()));
  } else {
    pushArg(ToRegister(lir->pattern()));
  }

  if (lir->string()->isConstant()) {
    pushArg(ImmGCPtr(lir->string()->toConstant()->toString()));
  } else {
    pushArg(ToRegister(lir->string()));
  }

  using Fn =
      JSString* (*)(JSContext*, HandleString, HandleString, HandleString);
  if (lir->mir()->isFlatReplacement()) {
    callVM<Fn, StringFlatReplaceString>(lir);
  } else {
    callVM<Fn, StringReplace>(lir);
  }
}

void CodeGenerator::visitBinaryValueCache(LBinaryValueCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  TypedOrValueRegister lhs = TypedOrValueRegister(ToValue(lir->lhs()));
  TypedOrValueRegister rhs = TypedOrValueRegister(ToValue(lir->rhs()));
  ValueOperand output = ToOutValue(lir);

  JSOp jsop = JSOp(*lir->mirRaw()->toInstruction()->resumePoint()->pc());

  switch (jsop) {
    case JSOp::Add:
    case JSOp::Sub:
    case JSOp::Mul:
    case JSOp::Div:
    case JSOp::Mod:
    case JSOp::Pow:
    case JSOp::BitAnd:
    case JSOp::BitOr:
    case JSOp::BitXor:
    case JSOp::Lsh:
    case JSOp::Rsh:
    case JSOp::Ursh: {
      IonBinaryArithIC ic(liveRegs, lhs, rhs, output);
      addIC(lir, allocateIC(ic));
      return;
    }
    default:
      MOZ_CRASH("Unsupported jsop in MBinaryValueCache");
  }
}

void CodeGenerator::visitBinaryBoolCache(LBinaryBoolCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  TypedOrValueRegister lhs = TypedOrValueRegister(ToValue(lir->lhs()));
  TypedOrValueRegister rhs = TypedOrValueRegister(ToValue(lir->rhs()));
  Register output = ToRegister(lir->output());

  JSOp jsop = JSOp(*lir->mirRaw()->toInstruction()->resumePoint()->pc());

  switch (jsop) {
    case JSOp::Lt:
    case JSOp::Le:
    case JSOp::Gt:
    case JSOp::Ge:
    case JSOp::Eq:
    case JSOp::Ne:
    case JSOp::StrictEq:
    case JSOp::StrictNe: {
      IonCompareIC ic(liveRegs, lhs, rhs, output);
      addIC(lir, allocateIC(ic));
      return;
    }
    default:
      MOZ_CRASH("Unsupported jsop in MBinaryBoolCache");
  }
}

void CodeGenerator::visitUnaryCache(LUnaryCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  TypedOrValueRegister input = TypedOrValueRegister(ToValue(lir->input()));
  ValueOperand output = ToOutValue(lir);

  IonUnaryArithIC ic(liveRegs, input, output);
  addIC(lir, allocateIC(ic));
}

void CodeGenerator::visitModuleMetadata(LModuleMetadata* lir) {
  pushArg(ImmPtr(lir->mir()->module()));

  using Fn = JSObject* (*)(JSContext*, HandleObject);
  callVM<Fn, js::GetOrCreateModuleMetaObject>(lir);
}

void CodeGenerator::visitDynamicImport(LDynamicImport* lir) {
  pushArg(ToValue(lir->options()));
  pushArg(ToValue(lir->specifier()));
  pushArg(ImmGCPtr(current->mir()->info().script()));

  using Fn = JSObject* (*)(JSContext*, HandleScript, HandleValue, HandleValue);
  callVM<Fn, js::StartDynamicModuleImport>(lir);
}

void CodeGenerator::visitLambda(LLambda* lir) {
  Register envChain = ToRegister(lir->environmentChain());
  Register output = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());
  gc::Heap heap = lir->mir()->initialHeap();

  JSFunction* fun = lir->mir()->templateFunction();
  MOZ_ASSERT(fun->isTenured());

  using Fn = JSObject* (*)(JSContext*, HandleFunction, HandleObject, gc::Heap);
  OutOfLineCode* ool = oolCallVM<Fn, js::LambdaOptimizedFallback>(
      lir, ArgList(ImmGCPtr(fun), envChain, Imm32(uint32_t(heap))),
      StoreRegisterTo(output));

  TemplateObject templateObject(fun);
  masm.createGCObject(output, tempReg, templateObject, heap, ool->entry(),
                      /* initContents = */ true,
                      AllocSiteInput(gc::CatchAllAllocSite::Optimized));

  masm.storeValue(JSVAL_TYPE_OBJECT, envChain,
                  Address(output, JSFunction::offsetOfEnvironment()));

  // If we specified the tenured heap then we need a post barrier. Otherwise no
  // post barrier needed as the output is guaranteed to be allocated in the
  // nursery.
  if (heap == gc::Heap::Tenured) {
    Label skipBarrier;
    masm.branchPtrInNurseryChunk(Assembler::NotEqual, envChain, tempReg,
                                 &skipBarrier);
    saveVolatile(tempReg);
    emitPostWriteBarrier(output);
    restoreVolatile(tempReg);
    masm.bind(&skipBarrier);
  }

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitFunctionWithProto(LFunctionWithProto* lir) {
  Register envChain = ToRegister(lir->envChain());
  Register prototype = ToRegister(lir->prototype());

  pushArg(prototype);
  pushArg(envChain);
  pushArg(ImmGCPtr(lir->mir()->function()));

  using Fn =
      JSObject* (*)(JSContext*, HandleFunction, HandleObject, HandleObject);
  callVM<Fn, js::FunWithProtoOperation>(lir);
}

void CodeGenerator::visitSetFunName(LSetFunName* lir) {
  pushArg(Imm32(lir->mir()->prefixKind()));
  pushArg(ToValue(lir->name()));
  pushArg(ToRegister(lir->fun()));

  using Fn =
      bool (*)(JSContext*, HandleFunction, HandleValue, FunctionPrefixKind);
  callVM<Fn, js::SetFunctionName>(lir);
}

void CodeGenerator::visitOsiPoint(LOsiPoint* lir) {
  // Note: markOsiPoint ensures enough space exists between the last
  // LOsiPoint and this one to patch adjacent call instructions.

  MOZ_ASSERT(masm.framePushed() == frameSize());

  uint32_t osiCallPointOffset = markOsiPoint(lir);

  LSafepoint* safepoint = lir->associatedSafepoint();
  MOZ_ASSERT(!safepoint->osiCallPointOffset());
  safepoint->setOsiCallPointOffset(osiCallPointOffset);

#ifdef DEBUG
  // There should be no movegroups or other instructions between
  // an instruction and its OsiPoint. This is necessary because
  // we use the OsiPoint's snapshot from within VM calls.
  for (LInstructionReverseIterator iter(current->rbegin(lir));
       iter != current->rend(); iter++) {
    if (*iter == lir) {
      continue;
    }
    MOZ_ASSERT(!iter->isMoveGroup());
    MOZ_ASSERT(iter->safepoint() == safepoint);
    break;
  }
#endif

#ifdef CHECK_OSIPOINT_REGISTERS
  if (shouldVerifyOsiPointRegs(safepoint)) {
    verifyOsiPointRegs(safepoint);
  }
#endif
}

void CodeGenerator::visitPhi(LPhi* lir) {
  MOZ_CRASH("Unexpected LPhi in CodeGenerator");
}

void CodeGenerator::visitGoto(LGoto* lir) { jumpToBlock(lir->target()); }

void CodeGenerator::visitTableSwitch(LTableSwitch* ins) {
  MTableSwitch* mir = ins->mir();
  Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

  Register intIndex;
  if (mir->getOperand(0)->type() != MIRType::Int32) {
    intIndex = ToRegister(ins->temp0());

    // The input is a double, so try and convert it to an integer.
    // If it does not fit in an integer, take the default case.
    masm.convertDoubleToInt32(ToFloatRegister(ins->index()), intIndex,
                              defaultcase, false);
  } else {
    intIndex = ToRegister(ins->index());
  }

  emitTableSwitchDispatch(mir, intIndex, ToTempRegisterOrInvalid(ins->temp1()));
}

void CodeGenerator::visitTableSwitchV(LTableSwitchV* ins) {
  MTableSwitch* mir = ins->mir();
  Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

  Register index = ToRegister(ins->temp0());
  ValueOperand value = ToValue(ins->input());
  Register tag = masm.extractTag(value, index);
  masm.branchTestNumber(Assembler::NotEqual, tag, defaultcase);

  Label unboxInt, isInt;
  masm.branchTestInt32(Assembler::Equal, tag, &unboxInt);
  {
    FloatRegister floatIndex = ToFloatRegister(ins->temp1());
    masm.unboxDouble(value, floatIndex);
    masm.convertDoubleToInt32(floatIndex, index, defaultcase, false);
    masm.jump(&isInt);
  }

  masm.bind(&unboxInt);
  masm.unboxInt32(value, index);

  masm.bind(&isInt);

  emitTableSwitchDispatch(mir, index, ToTempRegisterOrInvalid(ins->temp2()));
}

void CodeGenerator::visitParameter(LParameter* lir) {}

void CodeGenerator::visitCallee(LCallee* lir) {
  Register callee = ToRegister(lir->output());
  Address ptr(FramePointer, JitFrameLayout::offsetOfCalleeToken());

  masm.loadFunctionFromCalleeToken(ptr, callee);
}

void CodeGenerator::visitIsConstructing(LIsConstructing* lir) {
  Register output = ToRegister(lir->output());
  Address calleeToken(FramePointer, JitFrameLayout::offsetOfCalleeToken());
  masm.loadPtr(calleeToken, output);

  // We must be inside a function.
  MOZ_ASSERT(current->mir()->info().script()->function());

  // The low bit indicates whether this call is constructing, just clear the
  // other bits.
  static_assert(CalleeToken_Function == 0x0,
                "CalleeTokenTag value should match");
  static_assert(CalleeToken_FunctionConstructing == 0x1,
                "CalleeTokenTag value should match");
  masm.andPtr(Imm32(0x1), output);
}

void CodeGenerator::visitReturn(LReturn* lir) {
#if defined(JS_NUNBOX32)
  DebugOnly<LAllocation*> type = lir->getOperand(TYPE_INDEX);
  DebugOnly<LAllocation*> payload = lir->getOperand(PAYLOAD_INDEX);
  MOZ_ASSERT(ToRegister(type) == JSReturnReg_Type);
  MOZ_ASSERT(ToRegister(payload) == JSReturnReg_Data);
#elif defined(JS_PUNBOX64)
  DebugOnly<LAllocation*> result = lir->getOperand(0);
  MOZ_ASSERT(ToRegister(result) == JSReturnReg);
#endif
  // Don't emit a jump to the return label if this is the last block, as
  // it'll fall through to the epilogue.
  //
  // This is -not- true however for a Generator-return, which may appear in the
  // middle of the last block, so we should always emit the jump there.
  if (current->mir() != *gen->graph().poBegin() || lir->isGenerator()) {
    masm.jump(&returnLabel_);
  }
}

void CodeGenerator::visitOsrEntry(LOsrEntry* lir) {
  Register temp = ToRegister(lir->temp());

  // Remember the OSR entry offset into the code buffer.
  masm.flushBuffer();
  setOsrEntryOffset(masm.size());

  // Allocate the full frame for this function
  // Note we have a new entry here. So we reset MacroAssembler::framePushed()
  // to 0, before reserving the stack.
  MOZ_ASSERT(masm.framePushed() == frameSize());
  masm.setFramePushed(0);

  // The Baseline code ensured both the frame pointer and stack pointer point to
  // the JitFrameLayout on the stack.

  // If profiling, save the current frame pointer to a per-thread global field.
  if (isProfilerInstrumentationEnabled()) {
    masm.profilerEnterFrame(FramePointer, temp);
  }

  masm.reserveStack(frameSize());
  MOZ_ASSERT(masm.framePushed() == frameSize());

  // Ensure that the Ion frames is properly aligned.
  masm.assertStackAlignment(JitStackAlignment, 0);
}

void CodeGenerator::visitOsrEnvironmentChain(LOsrEnvironmentChain* lir) {
  const LAllocation* frame = lir->entry();
  const LDefinition* object = lir->output();

  const ptrdiff_t frameOffset =
      BaselineFrame::reverseOffsetOfEnvironmentChain();

  masm.loadPtr(Address(ToRegister(frame), frameOffset), ToRegister(object));
}

void CodeGenerator::visitOsrArgumentsObject(LOsrArgumentsObject* lir) {
  const LAllocation* frame = lir->entry();
  const LDefinition* object = lir->output();

  const ptrdiff_t frameOffset = BaselineFrame::reverseOffsetOfArgsObj();

  masm.loadPtr(Address(ToRegister(frame), frameOffset), ToRegister(object));
}

void CodeGenerator::visitOsrValue(LOsrValue* value) {
  const LAllocation* frame = value->entry();
  const ValueOperand out = ToOutValue(value);

  const ptrdiff_t frameOffset = value->mir()->frameOffset();

  masm.loadValue(Address(ToRegister(frame), frameOffset), out);
}

void CodeGenerator::visitOsrReturnValue(LOsrReturnValue* lir) {
  const LAllocation* frame = lir->entry();
  const ValueOperand out = ToOutValue(lir);

  Address flags =
      Address(ToRegister(frame), BaselineFrame::reverseOffsetOfFlags());
  Address retval =
      Address(ToRegister(frame), BaselineFrame::reverseOffsetOfReturnValue());

  masm.moveValue(UndefinedValue(), out);

  Label done;
  masm.branchTest32(Assembler::Zero, flags, Imm32(BaselineFrame::HAS_RVAL),
                    &done);
  masm.loadValue(retval, out);
  masm.bind(&done);
}

void CodeGenerator::visitStackArgT(LStackArgT* lir) {
  const LAllocation* arg = lir->arg();
  MIRType argType = lir->type();
  uint32_t argslot = lir->argslot();
  MOZ_ASSERT(argslot - 1u < graph.argumentSlotCount());

  Address dest = AddressOfPassedArg(argslot);

  if (arg->isFloatReg()) {
    masm.boxDouble(ToFloatRegister(arg), dest);
  } else if (arg->isGeneralReg()) {
    masm.storeValue(ValueTypeFromMIRType(argType), ToRegister(arg), dest);
  } else {
    masm.storeValue(arg->toConstant()->toJSValue(), dest);
  }
}

void CodeGenerator::visitStackArgV(LStackArgV* lir) {
  ValueOperand val = ToValue(lir->value());
  uint32_t argslot = lir->argslot();
  MOZ_ASSERT(argslot - 1u < graph.argumentSlotCount());

  masm.storeValue(val, AddressOfPassedArg(argslot));
}

void CodeGenerator::visitMoveGroup(LMoveGroup* group) {
  if (!group->numMoves()) {
    return;
  }

  MoveResolver& resolver = masm.moveResolver();

  for (size_t i = 0; i < group->numMoves(); i++) {
    const LMove& move = group->getMove(i);

    LAllocation from = move.from();
    LAllocation to = move.to();
    LDefinition::Type type = move.type();

    // No bogus moves.
    MOZ_ASSERT(from != to);
    MOZ_ASSERT(!from.isConstant());
    MoveOp::Type moveType;
    switch (type) {
      case LDefinition::OBJECT:
      case LDefinition::SLOTS:
      case LDefinition::WASM_ANYREF:
#ifdef JS_NUNBOX32
      case LDefinition::TYPE:
      case LDefinition::PAYLOAD:
#else
      case LDefinition::BOX:
#endif
      case LDefinition::GENERAL:
      case LDefinition::STACKRESULTS:
        moveType = MoveOp::GENERAL;
        break;
      case LDefinition::INT32:
        moveType = MoveOp::INT32;
        break;
      case LDefinition::FLOAT32:
        moveType = MoveOp::FLOAT32;
        break;
      case LDefinition::DOUBLE:
        moveType = MoveOp::DOUBLE;
        break;
      case LDefinition::SIMD128:
        moveType = MoveOp::SIMD128;
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }

    masm.propagateOOM(
        resolver.addMove(toMoveOperand(from), toMoveOperand(to), moveType));
  }

  masm.propagateOOM(resolver.resolve());
  if (masm.oom()) {
    return;
  }

  MoveEmitter emitter(masm);

#ifdef JS_CODEGEN_X86
  if (group->maybeScratchRegister().isGeneralReg()) {
    emitter.setScratchRegister(
        group->maybeScratchRegister().toGeneralReg()->reg());
  } else {
    resolver.sortMemoryToMemoryMoves();
  }
#endif

  emitter.emit(resolver);
  emitter.finish();
}

void CodeGenerator::visitInteger(LInteger* lir) {
  masm.move32(Imm32(lir->i32()), ToRegister(lir->output()));
}

void CodeGenerator::visitInteger64(LInteger64* lir) {
  masm.move64(Imm64(lir->i64()), ToOutRegister64(lir));
}

void CodeGenerator::visitPointer(LPointer* lir) {
  masm.movePtr(ImmGCPtr(lir->gcptr()), ToRegister(lir->output()));
}

void CodeGenerator::visitDouble(LDouble* ins) {
  masm.loadConstantDouble(ins->value(), ToFloatRegister(ins->output()));
}

void CodeGenerator::visitFloat32(LFloat32* ins) {
  masm.loadConstantFloat32(ins->value(), ToFloatRegister(ins->output()));
}

void CodeGenerator::visitValue(LValue* value) {
  ValueOperand result = ToOutValue(value);
  masm.moveValue(value->value(), result);
}

void CodeGenerator::visitNurseryObject(LNurseryObject* lir) {
  Register output = ToRegister(lir->output());
  uint32_t nurseryIndex = lir->mir()->nurseryIndex();

  // Load a pointer to the entry in IonScript's nursery objects list.
  CodeOffset label = masm.movWithPatch(ImmWord(uintptr_t(-1)), output);
  masm.propagateOOM(ionNurseryObjectLabels_.emplaceBack(label, nurseryIndex));

  // Load the JSObject*.
  masm.loadPtr(Address(output, 0), output);
}

void CodeGenerator::visitKeepAliveObject(LKeepAliveObject* lir) {
  // No-op.
}

void CodeGenerator::visitDebugEnterGCUnsafeRegion(
    LDebugEnterGCUnsafeRegion* lir) {
  Register temp = ToRegister(lir->temp0());

  masm.loadJSContext(temp);

  Address inUnsafeRegion(temp, JSContext::offsetOfInUnsafeRegion());
  masm.add32(Imm32(1), inUnsafeRegion);

  Label ok;
  masm.branch32(Assembler::GreaterThan, inUnsafeRegion, Imm32(0), &ok);
  masm.assumeUnreachable("unbalanced enter/leave GC unsafe region");
  masm.bind(&ok);
}

void CodeGenerator::visitDebugLeaveGCUnsafeRegion(
    LDebugLeaveGCUnsafeRegion* lir) {
  Register temp = ToRegister(lir->temp0());

  masm.loadJSContext(temp);

  Address inUnsafeRegion(temp, JSContext::offsetOfInUnsafeRegion());
  masm.add32(Imm32(-1), inUnsafeRegion);

  Label ok;
  masm.branch32(Assembler::GreaterThanOrEqual, inUnsafeRegion, Imm32(0), &ok);
  masm.assumeUnreachable("unbalanced enter/leave GC unsafe region");
  masm.bind(&ok);
}

void CodeGenerator::visitSlots(LSlots* lir) {
  Address slots(ToRegister(lir->object()), NativeObject::offsetOfSlots());
  masm.loadPtr(slots, ToRegister(lir->output()));
}

void CodeGenerator::visitLoadDynamicSlotV(LLoadDynamicSlotV* lir) {
  ValueOperand dest = ToOutValue(lir);
  Register base = ToRegister(lir->input());
  int32_t offset = lir->mir()->slot() * sizeof(js::Value);

  masm.loadValue(Address(base, offset), dest);
}

static ConstantOrRegister ToConstantOrRegister(const LAllocation* value,
                                               MIRType valueType) {
  if (value->isConstant()) {
    return ConstantOrRegister(value->toConstant()->toJSValue());
  }
  return TypedOrValueRegister(valueType, ToAnyRegister(value));
}

void CodeGenerator::visitStoreDynamicSlotT(LStoreDynamicSlotT* lir) {
  Register base = ToRegister(lir->slots());
  int32_t offset = lir->mir()->slot() * sizeof(js::Value);
  Address dest(base, offset);

  if (lir->mir()->needsBarrier()) {
    emitPreBarrier(dest);
  }

  MIRType valueType = lir->mir()->value()->type();
  ConstantOrRegister value = ToConstantOrRegister(lir->value(), valueType);
  masm.storeUnboxedValue(value, valueType, dest);
}

void CodeGenerator::visitStoreDynamicSlotV(LStoreDynamicSlotV* lir) {
  Register base = ToRegister(lir->slots());
  int32_t offset = lir->mir()->slot() * sizeof(Value);

  ValueOperand value = ToValue(lir->value());

  if (lir->mir()->needsBarrier()) {
    emitPreBarrier(Address(base, offset));
  }

  masm.storeValue(value, Address(base, offset));
}

void CodeGenerator::visitElements(LElements* lir) {
  Address elements(ToRegister(lir->object()), NativeObject::offsetOfElements());
  masm.loadPtr(elements, ToRegister(lir->output()));
}

void CodeGenerator::visitFunctionEnvironment(LFunctionEnvironment* lir) {
  Address environment(ToRegister(lir->function()),
                      JSFunction::offsetOfEnvironment());
  masm.unboxObject(environment, ToRegister(lir->output()));
}

void CodeGenerator::visitHomeObject(LHomeObject* lir) {
  Register func = ToRegister(lir->function());
  Address homeObject(func, FunctionExtended::offsetOfMethodHomeObjectSlot());

  masm.assertFunctionIsExtended(func);
#ifdef DEBUG
  Label isObject;
  masm.branchTestObject(Assembler::Equal, homeObject, &isObject);
  masm.assumeUnreachable("[[HomeObject]] must be Object");
  masm.bind(&isObject);
#endif

  masm.unboxObject(homeObject, ToRegister(lir->output()));
}

void CodeGenerator::visitHomeObjectSuperBase(LHomeObjectSuperBase* lir) {
  Register homeObject = ToRegister(lir->homeObject());
  ValueOperand output = ToOutValue(lir);
  Register temp = output.scratchReg();

  masm.loadObjProto(homeObject, temp);

#ifdef DEBUG
  // We won't encounter a lazy proto, because the prototype is guaranteed to
  // either be a JSFunction or a PlainObject, and only proxy objects can have a
  // lazy proto.
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  Label proxyCheckDone;
  masm.branchPtr(Assembler::NotEqual, temp, ImmWord(1), &proxyCheckDone);
  masm.assumeUnreachable("Unexpected lazy proto in JSOp::SuperBase");
  masm.bind(&proxyCheckDone);
#endif

  Label nullProto, done;
  masm.branchPtr(Assembler::Equal, temp, ImmWord(0), &nullProto);

  // Box prototype and return
  masm.tagValue(JSVAL_TYPE_OBJECT, temp, output);
  masm.jump(&done);

  masm.bind(&nullProto);
  masm.moveValue(NullValue(), output);

  masm.bind(&done);
}

template <class T>
static T* ToConstantObject(MDefinition* def) {
  MOZ_ASSERT(def->isConstant());
  return &def->toConstant()->toObject().as<T>();
}

void CodeGenerator::visitNewLexicalEnvironmentObject(
    LNewLexicalEnvironmentObject* lir) {
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  auto* templateObj = ToConstantObject<BlockLexicalEnvironmentObject>(
      lir->mir()->templateObj());
  auto* scope = &templateObj->scope();
  gc::Heap initialHeap = gc::Heap::Default;

  using Fn =
      BlockLexicalEnvironmentObject* (*)(JSContext*, Handle<LexicalScope*>);
  auto* ool =
      oolCallVM<Fn, BlockLexicalEnvironmentObject::createWithoutEnclosing>(
          lir, ArgList(ImmGCPtr(scope)), StoreRegisterTo(output));

  TemplateObject templateObject(templateObj);
  masm.createGCObject(output, temp, templateObject, initialHeap, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewClassBodyEnvironmentObject(
    LNewClassBodyEnvironmentObject* lir) {
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  auto* templateObj = ToConstantObject<ClassBodyLexicalEnvironmentObject>(
      lir->mir()->templateObj());
  auto* scope = &templateObj->scope();
  gc::Heap initialHeap = gc::Heap::Default;

  using Fn = ClassBodyLexicalEnvironmentObject* (*)(JSContext*,
                                                    Handle<ClassBodyScope*>);
  auto* ool =
      oolCallVM<Fn, ClassBodyLexicalEnvironmentObject::createWithoutEnclosing>(
          lir, ArgList(ImmGCPtr(scope)), StoreRegisterTo(output));

  TemplateObject templateObject(templateObj);
  masm.createGCObject(output, temp, templateObject, initialHeap, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewVarEnvironmentObject(
    LNewVarEnvironmentObject* lir) {
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  auto* templateObj =
      ToConstantObject<VarEnvironmentObject>(lir->mir()->templateObj());
  auto* scope = &templateObj->scope().as<VarScope>();
  gc::Heap initialHeap = gc::Heap::Default;

  using Fn = VarEnvironmentObject* (*)(JSContext*, Handle<VarScope*>);
  auto* ool = oolCallVM<Fn, VarEnvironmentObject::createWithoutEnclosing>(
      lir, ArgList(ImmGCPtr(scope)), StoreRegisterTo(output));

  TemplateObject templateObject(templateObj);
  masm.createGCObject(output, temp, templateObject, initialHeap, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitGuardShape(LGuardShape* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToTempRegisterOrInvalid(guard->temp0());
  Label bail;
  masm.branchTestObjShape(Assembler::NotEqual, obj, guard->mir()->shape(), temp,
                          obj, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardFuse(LGuardFuse* guard) {
  auto fuseIndex = guard->mir()->fuseIndex();

  Register temp = ToRegister(guard->temp0());
  Label bail;

  // Bake specific fuse address for Ion code, because we won't share this code
  // across realms.
  GuardFuse* fuse = mirGen().realm->realmFuses().getFuseByIndex(fuseIndex);
  masm.loadPtr(AbsoluteAddress(fuse->fuseRef()), temp);
  masm.branchPtr(Assembler::NotEqual, temp, ImmPtr(nullptr), &bail);

  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardMultipleShapes(LGuardMultipleShapes* guard) {
  Register obj = ToRegister(guard->object());
  Register shapeList = ToRegister(guard->shapeList());
  Register temp = ToRegister(guard->temp0());
  Register temp2 = ToRegister(guard->temp1());
  Register temp3 = ToRegister(guard->temp2());
  Register spectre = ToTempRegisterOrInvalid(guard->temp3());

  Label bail;
  masm.loadPtr(Address(shapeList, NativeObject::offsetOfElements()), temp);
  masm.branchTestObjShapeList(Assembler::NotEqual, obj, temp, temp2, temp3,
                              spectre, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardProto(LGuardProto* guard) {
  Register obj = ToRegister(guard->object());
  Register expected = ToRegister(guard->expected());
  Register temp = ToRegister(guard->temp0());

  masm.loadObjProto(obj, temp);

  Label bail;
  masm.branchPtr(Assembler::NotEqual, temp, expected, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardNullProto(LGuardNullProto* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToRegister(guard->temp0());

  masm.loadObjProto(obj, temp);

  Label bail;
  masm.branchTestPtr(Assembler::NonZero, temp, temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsNativeObject(LGuardIsNativeObject* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToRegister(guard->temp0());

  Label bail;
  masm.branchIfNonNativeObj(obj, temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardGlobalGeneration(LGuardGlobalGeneration* guard) {
  Register temp = ToRegister(guard->temp0());
  Label bail;

  masm.load32(AbsoluteAddress(guard->mir()->generationAddr()), temp);
  masm.branch32(Assembler::NotEqual, temp, Imm32(guard->mir()->expected()),
                &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsProxy(LGuardIsProxy* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToRegister(guard->temp0());

  Label bail;
  masm.branchTestObjectIsProxy(false, obj, temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsNotProxy(LGuardIsNotProxy* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToRegister(guard->temp0());

  Label bail;
  masm.branchTestObjectIsProxy(true, obj, temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsNotDOMProxy(LGuardIsNotDOMProxy* guard) {
  Register proxy = ToRegister(guard->proxy());
  Register temp = ToRegister(guard->temp0());

  Label bail;
  masm.branchTestProxyHandlerFamily(Assembler::Equal, proxy, temp,
                                    GetDOMProxyHandlerFamily(), &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitProxyGet(LProxyGet* lir) {
  Register proxy = ToRegister(lir->proxy());
  Register temp = ToRegister(lir->temp0());

  pushArg(lir->mir()->id(), temp);
  pushArg(proxy);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId, MutableHandleValue);
  callVM<Fn, ProxyGetProperty>(lir);
}

void CodeGenerator::visitProxyGetByValue(LProxyGetByValue* lir) {
  Register proxy = ToRegister(lir->proxy());
  ValueOperand idVal = ToValue(lir->idVal());

  pushArg(idVal);
  pushArg(proxy);

  using Fn =
      bool (*)(JSContext*, HandleObject, HandleValue, MutableHandleValue);
  callVM<Fn, ProxyGetPropertyByValue>(lir);
}

void CodeGenerator::visitProxyHasProp(LProxyHasProp* lir) {
  Register proxy = ToRegister(lir->proxy());
  ValueOperand idVal = ToValue(lir->id());

  pushArg(idVal);
  pushArg(proxy);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, bool*);
  if (lir->mir()->hasOwn()) {
    callVM<Fn, ProxyHasOwn>(lir);
  } else {
    callVM<Fn, ProxyHas>(lir);
  }
}

void CodeGenerator::visitProxySet(LProxySet* lir) {
  Register proxy = ToRegister(lir->proxy());
  ValueOperand rhs = ToValue(lir->rhs());
  Register temp = ToRegister(lir->temp0());

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(rhs);
  pushArg(lir->mir()->id(), temp);
  pushArg(proxy);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId, HandleValue, bool);
  callVM<Fn, ProxySetProperty>(lir);
}

void CodeGenerator::visitProxySetByValue(LProxySetByValue* lir) {
  Register proxy = ToRegister(lir->proxy());
  ValueOperand idVal = ToValue(lir->idVal());
  ValueOperand rhs = ToValue(lir->rhs());

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(rhs);
  pushArg(idVal);
  pushArg(proxy);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue, bool);
  callVM<Fn, ProxySetPropertyByValue>(lir);
}

void CodeGenerator::visitCallSetArrayLength(LCallSetArrayLength* lir) {
  Register obj = ToRegister(lir->obj());
  ValueOperand rhs = ToValue(lir->rhs());

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(rhs);
  pushArg(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, bool);
  callVM<Fn, jit::SetArrayLength>(lir);
}

void CodeGenerator::visitMegamorphicLoadSlot(LMegamorphicLoadSlot* lir) {
  Register obj = ToRegister(lir->object());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register temp3 = ToRegister(lir->temp3());
  ValueOperand output = ToOutValue(lir);

  Label cacheHit;
  masm.emitMegamorphicCacheLookup(lir->mir()->name(), obj, temp0, temp1, temp2,
                                  output, &cacheHit);

  Label bail;
  masm.branchIfNonNativeObj(obj, temp0, &bail);

  masm.Push(UndefinedValue());
  masm.moveStackPtrTo(temp3);

  using Fn = bool (*)(JSContext* cx, JSObject* obj, PropertyKey id,
                      MegamorphicCache::Entry* cacheEntry, Value* vp);
  masm.setupAlignedABICall();
  masm.loadJSContext(temp0);
  masm.passABIArg(temp0);
  masm.passABIArg(obj);
  masm.movePropertyKey(lir->mir()->name(), temp1);
  masm.passABIArg(temp1);
  masm.passABIArg(temp2);
  masm.passABIArg(temp3);

  masm.callWithABI<Fn, GetNativeDataPropertyPure>();

  MOZ_ASSERT(!output.aliases(ReturnReg));
  masm.Pop(output);

  masm.branchIfFalseBool(ReturnReg, &bail);
  masm.bind(&cacheHit);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitMegamorphicLoadSlotPermissive(
    LMegamorphicLoadSlotPermissive* lir) {
  Register obj = ToRegister(lir->object());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register temp3 = ToRegister(lir->temp3());
  ValueOperand output = ToOutValue(lir);

  masm.movePtr(obj, temp3);

  Label done, getter, nullGetter;
  masm.emitMegamorphicCacheLookup(lir->mir()->name(), obj, temp0, temp1, temp2,
                                  output, &done, &getter);

  masm.movePropertyKey(lir->mir()->name(), temp1);
  pushArg(temp2);
  pushArg(temp1);
  pushArg(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId,
                      MegamorphicCacheEntry*, MutableHandleValue);
  callVM<Fn, GetPropMaybeCached>(lir);

  masm.jump(&done);

  masm.bind(&getter);

  emitCallMegamorphicGetter(lir, output, temp3, temp1, temp2, &nullGetter);
  masm.jump(&done);

  masm.bind(&nullGetter);
  masm.moveValue(UndefinedValue(), output);

  masm.bind(&done);
}

void CodeGenerator::visitMegamorphicLoadSlotByValue(
    LMegamorphicLoadSlotByValue* lir) {
  Register obj = ToRegister(lir->object());
  ValueOperand idVal = ToValue(lir->idVal());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  ValueOperand output = ToOutValue(lir);

  Label cacheHit, bail;
  masm.emitMegamorphicCacheLookupByValue(idVal, obj, temp0, temp1, temp2,
                                         output, &cacheHit);

  masm.branchIfNonNativeObj(obj, temp0, &bail);

  // idVal will be in vp[0], result will be stored in vp[1].
  masm.reserveStack(sizeof(Value));
  masm.Push(idVal);
  masm.moveStackPtrTo(temp0);

  using Fn = bool (*)(JSContext* cx, JSObject* obj,
                      MegamorphicCache::Entry* cacheEntry, Value* vp);
  masm.setupAlignedABICall();
  masm.loadJSContext(temp1);
  masm.passABIArg(temp1);
  masm.passABIArg(obj);
  masm.passABIArg(temp2);
  masm.passABIArg(temp0);
  masm.callWithABI<Fn, GetNativeDataPropertyByValuePure>();

  MOZ_ASSERT(!idVal.aliases(temp0));
  masm.storeCallPointerResult(temp0);
  masm.Pop(idVal);

  uint32_t framePushed = masm.framePushed();
  Label ok;
  masm.branchIfTrueBool(temp0, &ok);
  masm.freeStack(sizeof(Value));  // Discard result Value.
  masm.jump(&bail);

  masm.bind(&ok);
  masm.setFramePushed(framePushed);
  masm.Pop(output);

  masm.bind(&cacheHit);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitMegamorphicLoadSlotByValuePermissive(
    LMegamorphicLoadSlotByValuePermissive* lir) {
  Register obj = ToRegister(lir->object());
  ValueOperand idVal = ToValue(lir->idVal());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  ValueOperand output = ToOutValue(lir);

  // If we have enough registers available, we can call getters directly from
  // jitcode. On x86, we have to call into the VM.
#ifndef JS_CODEGEN_X86
  Label done, getter, nullGetter;
  Register temp3 = ToRegister(lir->temp3());
  masm.movePtr(obj, temp3);

  masm.emitMegamorphicCacheLookupByValue(idVal, obj, temp0, temp1, temp2,
                                         output, &done, &getter);
#else
  Label done;
  masm.emitMegamorphicCacheLookupByValue(idVal, obj, temp0, temp1, temp2,
                                         output, &done);
#endif

  pushArg(temp2);
  pushArg(idVal);
  pushArg(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue,
                      MegamorphicCacheEntry*, MutableHandleValue);
  callVM<Fn, GetElemMaybeCached>(lir);

#ifndef JS_CODEGEN_X86
  masm.jump(&done);
  masm.bind(&getter);

  emitCallMegamorphicGetter(lir, output, temp3, temp1, temp2, &nullGetter);
  masm.jump(&done);

  masm.bind(&nullGetter);
  masm.moveValue(UndefinedValue(), output);
#endif

  masm.bind(&done);
}

void CodeGenerator::visitMegamorphicStoreSlot(LMegamorphicStoreSlot* lir) {
  Register obj = ToRegister(lir->object());
  ValueOperand value = ToValue(lir->rhs());

  Register temp0 = ToRegister(lir->temp0());
#ifndef JS_CODEGEN_X86
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
#endif

  // The instruction is marked as call-instruction so only these registers are
  // live.
  LiveRegisterSet liveRegs;
  liveRegs.addUnchecked(obj);
  liveRegs.addUnchecked(value);
  liveRegs.addUnchecked(temp0);
#ifndef JS_CODEGEN_X86
  liveRegs.addUnchecked(temp1);
  liveRegs.addUnchecked(temp2);
#endif

  Label cacheHit, done;
#ifdef JS_CODEGEN_X86
  masm.emitMegamorphicCachedSetSlot(
      lir->mir()->name(), obj, temp0, value, liveRegs, &cacheHit,
      [](MacroAssembler& masm, const Address& addr, MIRType mirType) {
        EmitPreBarrier(masm, addr, mirType);
      });
#else
  masm.emitMegamorphicCachedSetSlot(
      lir->mir()->name(), obj, temp0, temp1, temp2, value, liveRegs, &cacheHit,
      [](MacroAssembler& masm, const Address& addr, MIRType mirType) {
        EmitPreBarrier(masm, addr, mirType);
      });
#endif

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(value);
  pushArg(lir->mir()->name(), temp0);
  pushArg(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId, HandleValue, bool);
  callVM<Fn, SetPropertyMegamorphic<true>>(lir);

  masm.jump(&done);
  masm.bind(&cacheHit);

  masm.branchPtrInNurseryChunk(Assembler::Equal, obj, temp0, &done);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, value, temp0, &done);

  // Note: because this is a call-instruction, no registers need to be saved.
  MOZ_ASSERT(lir->isCall());
  emitPostWriteBarrier(obj);

  masm.bind(&done);
}

void CodeGenerator::visitMegamorphicHasProp(LMegamorphicHasProp* lir) {
  Register obj = ToRegister(lir->object());
  ValueOperand idVal = ToValue(lir->idVal());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register output = ToRegister(lir->output());

  Label bail, cacheHit;
  masm.emitMegamorphicCacheLookupExists(idVal, obj, temp0, temp1, temp2, output,
                                        &cacheHit, lir->mir()->hasOwn());

  masm.branchIfNonNativeObj(obj, temp0, &bail);

  // idVal will be in vp[0], result will be stored in vp[1].
  masm.reserveStack(sizeof(Value));
  masm.Push(idVal);
  masm.moveStackPtrTo(temp0);

  using Fn = bool (*)(JSContext* cx, JSObject* obj,
                      MegamorphicCache::Entry* cacheEntry, Value* vp);
  masm.setupAlignedABICall();
  masm.loadJSContext(temp1);
  masm.passABIArg(temp1);
  masm.passABIArg(obj);
  masm.passABIArg(temp2);
  masm.passABIArg(temp0);
  if (lir->mir()->hasOwn()) {
    masm.callWithABI<Fn, HasNativeDataPropertyPure<true>>();
  } else {
    masm.callWithABI<Fn, HasNativeDataPropertyPure<false>>();
  }

  MOZ_ASSERT(!idVal.aliases(temp0));
  masm.storeCallPointerResult(temp0);
  masm.Pop(idVal);

  uint32_t framePushed = masm.framePushed();
  Label ok;
  masm.branchIfTrueBool(temp0, &ok);
  masm.freeStack(sizeof(Value));  // Discard result Value.
  masm.jump(&bail);

  masm.bind(&ok);
  masm.setFramePushed(framePushed);
  masm.unboxBoolean(Address(masm.getStackPointer(), 0), output);
  masm.freeStack(sizeof(Value));
  masm.bind(&cacheHit);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitSmallObjectVariableKeyHasProp(
    LSmallObjectVariableKeyHasProp* lir) {
  Register id = ToRegister(lir->idStr());
  Register output = ToRegister(lir->output());

#ifdef DEBUG
  Label isAtom;
  masm.branchTest32(Assembler::NonZero, Address(id, JSString::offsetOfFlags()),
                    Imm32(JSString::ATOM_BIT), &isAtom);
  masm.assumeUnreachable("Expected atom input");
  masm.bind(&isAtom);
#endif

  SharedShape* shape = &lir->mir()->shape()->asShared();

  Label done, success;
  for (SharedShapePropertyIter<NoGC> iter(shape); !iter.done(); iter++) {
    masm.branchPtr(Assembler::Equal, id, ImmGCPtr(iter->key().toAtom()),
                   &success);
  }
  masm.move32(Imm32(0), output);
  masm.jump(&done);
  masm.bind(&success);
  masm.move32(Imm32(1), output);
  masm.bind(&done);
}

void CodeGenerator::visitGuardIsNotArrayBufferMaybeShared(
    LGuardIsNotArrayBufferMaybeShared* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToRegister(guard->temp0());

  Label bail;
  masm.loadObjClassUnsafe(obj, temp);
  masm.branchPtr(Assembler::Equal, temp,
                 ImmPtr(&FixedLengthArrayBufferObject::class_), &bail);
  masm.branchPtr(Assembler::Equal, temp,
                 ImmPtr(&FixedLengthSharedArrayBufferObject::class_), &bail);
  masm.branchPtr(Assembler::Equal, temp,
                 ImmPtr(&ResizableArrayBufferObject::class_), &bail);
  masm.branchPtr(Assembler::Equal, temp,
                 ImmPtr(&GrowableSharedArrayBufferObject::class_), &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsTypedArray(LGuardIsTypedArray* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToRegister(guard->temp0());

  Label bail;
  masm.loadObjClassUnsafe(obj, temp);
  masm.branchIfClassIsNotTypedArray(temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsFixedLengthTypedArray(
    LGuardIsFixedLengthTypedArray* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToRegister(guard->temp0());

  Label bail;
  masm.loadObjClassUnsafe(obj, temp);
  masm.branchIfClassIsNotFixedLengthTypedArray(temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsResizableTypedArray(
    LGuardIsResizableTypedArray* guard) {
  Register obj = ToRegister(guard->object());
  Register temp = ToRegister(guard->temp0());

  Label bail;
  masm.loadObjClassUnsafe(obj, temp);
  masm.branchIfClassIsNotResizableTypedArray(temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardHasProxyHandler(LGuardHasProxyHandler* guard) {
  Register obj = ToRegister(guard->object());

  Label bail;

  Address handlerAddr(obj, ProxyObject::offsetOfHandler());
  masm.branchPtr(Assembler::NotEqual, handlerAddr,
                 ImmPtr(guard->mir()->handler()), &bail);

  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardObjectIdentity(LGuardObjectIdentity* guard) {
  Register input = ToRegister(guard->input());
  Register expected = ToRegister(guard->expected());

  Assembler::Condition cond =
      guard->mir()->bailOnEquality() ? Assembler::Equal : Assembler::NotEqual;
  bailoutCmpPtr(cond, input, expected, guard->snapshot());
}

void CodeGenerator::visitGuardSpecificFunction(LGuardSpecificFunction* guard) {
  Register input = ToRegister(guard->input());
  Register expected = ToRegister(guard->expected());

  bailoutCmpPtr(Assembler::NotEqual, input, expected, guard->snapshot());
}

void CodeGenerator::visitGuardSpecificAtom(LGuardSpecificAtom* guard) {
  Register str = ToRegister(guard->str());
  Register scratch = ToRegister(guard->temp0());

  LiveRegisterSet volatileRegs = liveVolatileRegs(guard);
  volatileRegs.takeUnchecked(scratch);

  Label bail;
  masm.guardSpecificAtom(str, guard->mir()->atom(), scratch, volatileRegs,
                         &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardSpecificSymbol(LGuardSpecificSymbol* guard) {
  Register symbol = ToRegister(guard->symbol());

  bailoutCmpPtr(Assembler::NotEqual, symbol, ImmGCPtr(guard->mir()->expected()),
                guard->snapshot());
}

void CodeGenerator::visitGuardSpecificInt32(LGuardSpecificInt32* guard) {
  Register num = ToRegister(guard->num());

  bailoutCmp32(Assembler::NotEqual, num, Imm32(guard->mir()->expected()),
               guard->snapshot());
}

void CodeGenerator::visitGuardStringToIndex(LGuardStringToIndex* lir) {
  Register str = ToRegister(lir->string());
  Register output = ToRegister(lir->output());

  Label vmCall, done;
  masm.loadStringIndexValue(str, output, &vmCall);
  masm.jump(&done);

  {
    masm.bind(&vmCall);

    LiveRegisterSet volatileRegs = liveVolatileRegs(lir);
    volatileRegs.takeUnchecked(output);
    masm.PushRegsInMask(volatileRegs);

    using Fn = int32_t (*)(JSString* str);
    masm.setupAlignedABICall();
    masm.passABIArg(str);
    masm.callWithABI<Fn, GetIndexFromString>();
    masm.storeCallInt32Result(output);

    masm.PopRegsInMask(volatileRegs);

    // GetIndexFromString returns a negative value on failure.
    bailoutTest32(Assembler::Signed, output, output, lir->snapshot());
  }

  masm.bind(&done);
}

void CodeGenerator::visitGuardStringToInt32(LGuardStringToInt32* lir) {
  Register str = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  LiveRegisterSet volatileRegs = liveVolatileRegs(lir);

  Label bail;
  masm.guardStringToInt32(str, output, temp, volatileRegs, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardStringToDouble(LGuardStringToDouble* lir) {
  Register str = ToRegister(lir->string());
  FloatRegister output = ToFloatRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  Label vmCall, done;
  // Use indexed value as fast path if possible.
  masm.loadStringIndexValue(str, temp0, &vmCall);
  masm.convertInt32ToDouble(temp0, output);
  masm.jump(&done);
  {
    masm.bind(&vmCall);

    // Reserve stack for holding the result value of the call.
    masm.reserveStack(sizeof(double));
    masm.moveStackPtrTo(temp0);

    LiveRegisterSet volatileRegs = liveVolatileRegs(lir);
    volatileRegs.takeUnchecked(temp0);
    volatileRegs.takeUnchecked(temp1);
    masm.PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSContext* cx, JSString* str, double* result);
    masm.setupAlignedABICall();
    masm.loadJSContext(temp1);
    masm.passABIArg(temp1);
    masm.passABIArg(str);
    masm.passABIArg(temp0);
    masm.callWithABI<Fn, StringToNumberPure>();
    masm.storeCallPointerResult(temp0);

    masm.PopRegsInMask(volatileRegs);

    Label ok;
    masm.branchIfTrueBool(temp0, &ok);
    {
      // OOM path, recovered by StringToNumberPure.
      //
      // Use addToStackPtr instead of freeStack as freeStack tracks stack height
      // flow-insensitively, and using it here would confuse the stack height
      // tracking.
      masm.addToStackPtr(Imm32(sizeof(double)));
      bailout(lir->snapshot());
    }
    masm.bind(&ok);
    masm.Pop(output);
  }
  masm.bind(&done);
}

void CodeGenerator::visitGuardNoDenseElements(LGuardNoDenseElements* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToRegister(guard->temp0());

  // Load obj->elements.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), temp);

  // Make sure there are no dense elements.
  Address initLength(temp, ObjectElements::offsetOfInitializedLength());
  bailoutCmp32(Assembler::NotEqual, initLength, Imm32(0), guard->snapshot());
}

void CodeGenerator::visitBooleanToInt64(LBooleanToInt64* lir) {
  Register input = ToRegister(lir->input());
  Register64 output = ToOutRegister64(lir);

  masm.move32To64ZeroExtend(input, output);
}

void CodeGenerator::emitStringToInt64(LInstruction* lir, Register input,
                                      Register64 output) {
  Register temp = output.scratchReg();

  saveLive(lir);

  masm.reserveStack(sizeof(uint64_t));
  masm.moveStackPtrTo(temp);
  pushArg(temp);
  pushArg(input);

  using Fn = bool (*)(JSContext*, HandleString, uint64_t*);
  callVM<Fn, DoStringToInt64>(lir);

  masm.load64(Address(masm.getStackPointer(), 0), output);
  masm.freeStack(sizeof(uint64_t));

  restoreLiveIgnore(lir, StoreValueTo(output).clobbered());
}

void CodeGenerator::visitStringToInt64(LStringToInt64* lir) {
  Register input = ToRegister(lir->input());
  Register64 output = ToOutRegister64(lir);

  emitStringToInt64(lir, input, output);
}

void CodeGenerator::visitValueToInt64(LValueToInt64* lir) {
  ValueOperand input = ToValue(lir->input());
  Register temp = ToRegister(lir->temp0());
  Register64 output = ToOutRegister64(lir);

  int checks = 3;

  Label fail, done;
  // Jump to fail if this is the last check and we fail it,
  // otherwise to the next test.
  auto emitTestAndUnbox = [&](auto testAndUnbox) {
    MOZ_ASSERT(checks > 0);

    checks--;
    Label notType;
    Label* target = checks ? &notType : &fail;

    testAndUnbox(target);

    if (checks) {
      masm.jump(&done);
      masm.bind(&notType);
    }
  };

  Register tag = masm.extractTag(input, temp);

  // BigInt.
  emitTestAndUnbox([&](Label* target) {
    masm.branchTestBigInt(Assembler::NotEqual, tag, target);
    masm.unboxBigInt(input, temp);
    masm.loadBigInt64(temp, output);
  });

  // Boolean
  emitTestAndUnbox([&](Label* target) {
    masm.branchTestBoolean(Assembler::NotEqual, tag, target);
    masm.unboxBoolean(input, temp);
    masm.move32To64ZeroExtend(temp, output);
  });

  // String
  emitTestAndUnbox([&](Label* target) {
    masm.branchTestString(Assembler::NotEqual, tag, target);
    masm.unboxString(input, temp);
    emitStringToInt64(lir, temp, output);
  });

  MOZ_ASSERT(checks == 0);

  bailoutFrom(&fail, lir->snapshot());
  masm.bind(&done);
}

void CodeGenerator::visitTruncateBigIntToInt64(LTruncateBigIntToInt64* lir) {
  Register operand = ToRegister(lir->input());
  Register64 output = ToOutRegister64(lir);

  masm.loadBigInt64(operand, output);
}

OutOfLineCode* CodeGenerator::createBigIntOutOfLine(LInstruction* lir,
                                                    Scalar::Type type,
                                                    Register64 input,
                                                    Register output) {
#if JS_BITS_PER_WORD == 32
  using Fn = BigInt* (*)(JSContext*, uint32_t, uint32_t);
  auto args = ArgList(input.low, input.high);
#else
  using Fn = BigInt* (*)(JSContext*, uint64_t);
  auto args = ArgList(input);
#endif

  if (type == Scalar::BigInt64) {
    return oolCallVM<Fn, jit::CreateBigIntFromInt64>(lir, args,
                                                     StoreRegisterTo(output));
  }
  MOZ_ASSERT(type == Scalar::BigUint64);
  return oolCallVM<Fn, jit::CreateBigIntFromUint64>(lir, args,
                                                    StoreRegisterTo(output));
}

void CodeGenerator::emitCreateBigInt(LInstruction* lir, Scalar::Type type,
                                     Register64 input, Register output,
                                     Register maybeTemp,
                                     Register64 maybeTemp64) {
  OutOfLineCode* ool = createBigIntOutOfLine(lir, type, input, output);

  if (maybeTemp != InvalidReg) {
    masm.newGCBigInt(output, maybeTemp, initialBigIntHeap(), ool->entry());
  } else {
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(input);
    regs.take(output);

    Register temp = regs.takeAny();

    masm.push(temp);

    Label fail, ok;
    masm.newGCBigInt(output, temp, initialBigIntHeap(), &fail);
    masm.pop(temp);
    masm.jump(&ok);
    masm.bind(&fail);
    masm.pop(temp);
    masm.jump(ool->entry());
    masm.bind(&ok);
  }
  masm.initializeBigInt64(type, output, input, maybeTemp64);
  masm.bind(ool->rejoin());
}

void CodeGenerator::emitCallMegamorphicGetter(
    LInstruction* lir, ValueOperand accessorAndOutput, Register obj,
    Register calleeScratch, Register argcScratch, Label* nullGetter) {
  MOZ_ASSERT(calleeScratch == IonGenericCallCalleeReg);
  MOZ_ASSERT(argcScratch == IonGenericCallArgcReg);

  masm.unboxNonDouble(accessorAndOutput, calleeScratch,
                      JSVAL_TYPE_PRIVATE_GCTHING);

  masm.loadPtr(Address(calleeScratch, GetterSetter::offsetOfGetter()),
               calleeScratch);
  masm.branchTestPtr(Assembler::Zero, calleeScratch, calleeScratch, nullGetter);
  masm.loadPtr(Address(calleeScratch, JSFunction::offsetOfJitInfoOrScript()),
               argcScratch);

  if (JitStackValueAlignment > 1) {
    masm.reserveStack(sizeof(Value) * (JitStackValueAlignment - 1));
  }
  masm.pushValue(JSVAL_TYPE_OBJECT, obj);

  masm.checkStackAlignment();

  masm.move32(Imm32(0), argcScratch);
  ensureOsiSpace();

  TrampolinePtr genericCallStub =
      gen->jitRuntime()->getIonGenericCallStub(IonGenericCallKind::Call);
  uint32_t callOffset = masm.callJit(genericCallStub);
  markSafepointAt(callOffset, lir);

  masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);

  masm.moveValue(JSReturnOperand, accessorAndOutput);

  masm.setFramePushed(frameSize());
  emitRestoreStackPointerFromFP();
}

void CodeGenerator::visitInt64ToBigInt(LInt64ToBigInt* lir) {
  Register64 input = ToRegister64(lir->input());
  Register64 temp = ToRegister64(lir->temp0());
  Register output = ToRegister(lir->output());

  emitCreateBigInt(lir, Scalar::BigInt64, input, output, temp.scratchReg(),
                   temp);
}

void CodeGenerator::visitUint64ToBigInt(LUint64ToBigInt* lir) {
  Register64 input = ToRegister64(lir->input());
  Register temp = ToRegister(lir->temp0());
  Register output = ToRegister(lir->output());

  emitCreateBigInt(lir, Scalar::BigUint64, input, output, temp);
}

void CodeGenerator::visitInt64ToIntPtr(LInt64ToIntPtr* lir) {
  Register64 input = ToRegister64(lir->input());
#ifdef JS_64BIT
  MOZ_ASSERT(input.reg == ToRegister(lir->output()));
#else
  Register output = ToRegister(lir->output());
#endif

  Label bail;
  if (lir->mir()->isSigned()) {
    masm.branchInt64NotInPtrRange(input, &bail);
  } else {
    masm.branchUInt64NotInPtrRange(input, &bail);
  }
  bailoutFrom(&bail, lir->snapshot());

#ifndef JS_64BIT
  masm.move64To32(input, output);
#endif
}

void CodeGenerator::visitIntPtrToInt64(LIntPtrToInt64* lir) {
#ifdef JS_64BIT
  MOZ_CRASH("Not used on 64-bit platforms");
#else
  Register input = ToRegister(lir->input());
  Register64 output = ToOutRegister64(lir);

  masm.move32To64SignExtend(input, output);
#endif
}

void CodeGenerator::visitGuardValue(LGuardValue* lir) {
  ValueOperand input = ToValue(lir->input());
  Register nanTemp = ToTempRegisterOrInvalid(lir->temp0());
  Value expected = lir->mir()->expected();
  Label bail;

  if (expected.isNaN()) {
    masm.branchTestNaNValue(Assembler::NotEqual, input, nanTemp, &bail);
  } else {
    MOZ_ASSERT(nanTemp == InvalidReg);
    masm.branchTestValue(Assembler::NotEqual, input, expected, &bail);
  }

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardNullOrUndefined(LGuardNullOrUndefined* lir) {
  ValueOperand input = ToValue(lir->input());

  ScratchTagScope tag(masm, input);
  masm.splitTagForTest(input, tag);

  Label done;
  masm.branchTestNull(Assembler::Equal, tag, &done);

  Label bail;
  masm.branchTestUndefined(Assembler::NotEqual, tag, &bail);
  bailoutFrom(&bail, lir->snapshot());

  masm.bind(&done);
}

void CodeGenerator::visitGuardIsNotObject(LGuardIsNotObject* lir) {
  ValueOperand input = ToValue(lir->input());

  Label bail;
  masm.branchTestObject(Assembler::Equal, input, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardFunctionFlags(LGuardFunctionFlags* lir) {
  Register function = ToRegister(lir->function());

  Label bail;
  if (uint16_t flags = lir->mir()->expectedFlags()) {
    masm.branchTestFunctionFlags(function, flags, Assembler::Zero, &bail);
  }
  if (uint16_t flags = lir->mir()->unexpectedFlags()) {
    masm.branchTestFunctionFlags(function, flags, Assembler::NonZero, &bail);
  }
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardFunctionIsNonBuiltinCtor(
    LGuardFunctionIsNonBuiltinCtor* lir) {
  Register function = ToRegister(lir->function());
  Register temp = ToRegister(lir->temp0());

  Label bail;
  masm.branchIfNotFunctionIsNonBuiltinCtor(function, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardFunctionKind(LGuardFunctionKind* lir) {
  Register function = ToRegister(lir->function());
  Register temp = ToRegister(lir->temp0());

  Assembler::Condition cond =
      lir->mir()->bailOnEquality() ? Assembler::Equal : Assembler::NotEqual;

  Label bail;
  masm.branchFunctionKind(cond, lir->mir()->expected(), function, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardFunctionScript(LGuardFunctionScript* lir) {
  Register function = ToRegister(lir->function());

  Address scriptAddr(function, JSFunction::offsetOfJitInfoOrScript());
  bailoutCmpPtr(Assembler::NotEqual, scriptAddr,
                ImmGCPtr(lir->mir()->expected()), lir->snapshot());
}

// Out-of-line path to update the store buffer.
class OutOfLineCallPostWriteBarrier : public OutOfLineCodeBase<CodeGenerator> {
  LInstruction* lir_;
  const LAllocation* object_;

 public:
  OutOfLineCallPostWriteBarrier(LInstruction* lir, const LAllocation* object)
      : lir_(lir), object_(object) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineCallPostWriteBarrier(this);
  }

  LInstruction* lir() const { return lir_; }
  const LAllocation* object() const { return object_; }
};

static void EmitStoreBufferCheckForConstant(MacroAssembler& masm,
                                            const gc::TenuredCell* cell,
                                            AllocatableGeneralRegisterSet& regs,
                                            Label* exit, Label* callVM) {
  Register temp = regs.takeAny();

  gc::Arena* arena = cell->arena();

  Register cells = temp;
  masm.loadPtr(AbsoluteAddress(&arena->bufferedCells()), cells);

  size_t index = gc::ArenaCellSet::getCellIndex(cell);
  size_t word;
  uint32_t mask;
  gc::ArenaCellSet::getWordIndexAndMask(index, &word, &mask);
  size_t offset = gc::ArenaCellSet::offsetOfBits() + word * sizeof(uint32_t);

  masm.branchTest32(Assembler::NonZero, Address(cells, offset), Imm32(mask),
                    exit);

  // Check whether this is the sentinel set and if so call the VM to allocate
  // one for this arena.
  masm.branchPtr(Assembler::Equal,
                 Address(cells, gc::ArenaCellSet::offsetOfArena()),
                 ImmPtr(nullptr), callVM);

  // Add the cell to the set.
  masm.or32(Imm32(mask), Address(cells, offset));
  masm.jump(exit);

  regs.add(temp);
}

static void EmitPostWriteBarrier(MacroAssembler& masm, CompileRuntime* runtime,
                                 Register objreg, JSObject* maybeConstant,
                                 bool isGlobal,
                                 AllocatableGeneralRegisterSet& regs) {
  MOZ_ASSERT_IF(isGlobal, maybeConstant);

  Label callVM;
  Label exit;

  Register temp = regs.takeAny();

  // We already have a fast path to check whether a global is in the store
  // buffer.
  if (!isGlobal) {
    if (maybeConstant) {
      // Check store buffer bitmap directly for known object.
      EmitStoreBufferCheckForConstant(masm, &maybeConstant->asTenured(), regs,
                                      &exit, &callVM);
    } else {
      // Check one element cache to avoid VM call.
      masm.branchPtr(Assembler::Equal,
                     AbsoluteAddress(runtime->addressOfLastBufferedWholeCell()),
                     objreg, &exit);
    }
  }

  // Call into the VM to barrier the write.
  masm.bind(&callVM);

  Register runtimereg = temp;
  masm.mov(ImmPtr(runtime), runtimereg);

  masm.setupAlignedABICall();
  masm.passABIArg(runtimereg);
  masm.passABIArg(objreg);
  if (isGlobal) {
    using Fn = void (*)(JSRuntime* rt, GlobalObject* obj);
    masm.callWithABI<Fn, PostGlobalWriteBarrier>();
  } else {
    using Fn = void (*)(JSRuntime* rt, js::gc::Cell* obj);
    masm.callWithABI<Fn, PostWriteBarrier>();
  }

  masm.bind(&exit);
}

void CodeGenerator::emitPostWriteBarrier(const LAllocation* obj) {
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());

  Register objreg;
  JSObject* object = nullptr;
  bool isGlobal = false;
  if (obj->isConstant()) {
    object = &obj->toConstant()->toObject();
    isGlobal = isGlobalObject(object);
    objreg = regs.takeAny();
    masm.movePtr(ImmGCPtr(object), objreg);
  } else {
    objreg = ToRegister(obj);
    regs.takeUnchecked(objreg);
  }

  EmitPostWriteBarrier(masm, gen->runtime, objreg, object, isGlobal, regs);
}

// Returns true if `def` might be allocated in the nursery.
static bool ValueNeedsPostBarrier(MDefinition* def) {
  if (def->isBox()) {
    def = def->toBox()->input();
  }
  if (def->type() == MIRType::Value) {
    return true;
  }
  return NeedsPostBarrier(def->type());
}

void CodeGenerator::emitElementPostWriteBarrier(
    MInstruction* mir, const LiveRegisterSet& liveVolatileRegs, Register obj,
    const LAllocation* index, Register scratch, const ConstantOrRegister& val,
    int32_t indexDiff) {
  if (val.constant()) {
    MOZ_ASSERT_IF(val.value().isGCThing(),
                  !IsInsideNursery(val.value().toGCThing()));
    return;
  }

  TypedOrValueRegister reg = val.reg();
  if (reg.hasTyped() && !NeedsPostBarrier(reg.type())) {
    return;
  }

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    masm.PushRegsInMask(liveVolatileRegs);

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
    regs.takeUnchecked(obj);
    regs.takeUnchecked(scratch);

    Register indexReg;
    if (index->isConstant()) {
      indexReg = regs.takeAny();
      masm.move32(Imm32(ToInt32(index) + indexDiff), indexReg);
    } else {
      indexReg = ToRegister(index);
      regs.takeUnchecked(indexReg);
      if (indexDiff != 0) {
        masm.add32(Imm32(indexDiff), indexReg);
      }
    }

    masm.setupUnalignedABICall(scratch);
    masm.movePtr(ImmPtr(gen->runtime), scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(obj);
    masm.passABIArg(indexReg);
    using Fn = void (*)(JSRuntime* rt, JSObject* obj, int32_t index);
    masm.callWithABI<Fn, PostWriteElementBarrier>();

    // We don't need a sub32 here because indexReg must be in liveVolatileRegs
    // if indexDiff is not zero, so it will be restored below.
    MOZ_ASSERT_IF(indexDiff != 0, liveVolatileRegs.has(indexReg));

    masm.PopRegsInMask(liveVolatileRegs);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, mir);

  masm.branchPtrInNurseryChunk(Assembler::Equal, obj, scratch, ool->rejoin());

  if (reg.hasValue()) {
    masm.branchValueIsNurseryCell(Assembler::Equal, reg.valueReg(), scratch,
                                  ool->entry());
  } else {
    masm.branchPtrInNurseryChunk(Assembler::Equal, reg.typedReg().gpr(),
                                 scratch, ool->entry());
  }

  masm.bind(ool->rejoin());
}

void CodeGenerator::emitPostWriteBarrier(Register objreg) {
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  regs.takeUnchecked(objreg);
  EmitPostWriteBarrier(masm, gen->runtime, objreg, nullptr, false, regs);
}

void CodeGenerator::visitOutOfLineCallPostWriteBarrier(
    OutOfLineCallPostWriteBarrier* ool) {
  saveLiveVolatile(ool->lir());
  const LAllocation* obj = ool->object();
  emitPostWriteBarrier(obj);
  restoreLiveVolatile(ool->lir());

  masm.jump(ool->rejoin());
}

void CodeGenerator::maybeEmitGlobalBarrierCheck(const LAllocation* maybeGlobal,
                                                OutOfLineCode* ool) {
  // Check whether an object is a global that we have already barriered before
  // calling into the VM.
  //
  // We only check for the script's global, not other globals within the same
  // compartment, because we bake in a pointer to realm->globalWriteBarriered
  // and doing that would be invalid for other realms because they could be
  // collected before the Ion code is discarded.

  if (!maybeGlobal->isConstant()) {
    return;
  }

  JSObject* obj = &maybeGlobal->toConstant()->toObject();
  if (gen->realm->maybeGlobal() != obj) {
    return;
  }

  const uint32_t* addr = gen->realm->addressOfGlobalWriteBarriered();
  masm.branch32(Assembler::NotEqual, AbsoluteAddress(addr), Imm32(0),
                ool->rejoin());
}

template <class LPostBarrierType, MIRType nurseryType>
void CodeGenerator::visitPostWriteBarrierCommon(LPostBarrierType* lir,
                                                OutOfLineCode* ool) {
  static_assert(NeedsPostBarrier(nurseryType));

  addOutOfLineCode(ool, lir->mir());

  Register temp = ToTempRegisterOrInvalid(lir->temp0());

  if (lir->object()->isConstant()) {
    // Constant nursery objects cannot appear here, see
    // LIRGenerator::visitPostWriteElementBarrier.
    MOZ_ASSERT(!IsInsideNursery(&lir->object()->toConstant()->toObject()));
  } else {
    masm.branchPtrInNurseryChunk(Assembler::Equal, ToRegister(lir->object()),
                                 temp, ool->rejoin());
  }

  maybeEmitGlobalBarrierCheck(lir->object(), ool);

  Register value = ToRegister(lir->value());
  if constexpr (nurseryType == MIRType::Object) {
    MOZ_ASSERT(lir->mir()->value()->type() == MIRType::Object);
  } else if constexpr (nurseryType == MIRType::String) {
    MOZ_ASSERT(lir->mir()->value()->type() == MIRType::String);
  } else {
    static_assert(nurseryType == MIRType::BigInt);
    MOZ_ASSERT(lir->mir()->value()->type() == MIRType::BigInt);
  }
  masm.branchPtrInNurseryChunk(Assembler::Equal, value, temp, ool->entry());

  masm.bind(ool->rejoin());
}

template <class LPostBarrierType>
void CodeGenerator::visitPostWriteBarrierCommonV(LPostBarrierType* lir,
                                                 OutOfLineCode* ool) {
  addOutOfLineCode(ool, lir->mir());

  Register temp = ToTempRegisterOrInvalid(lir->temp0());

  if (lir->object()->isConstant()) {
    // Constant nursery objects cannot appear here, see
    // LIRGenerator::visitPostWriteElementBarrier.
    MOZ_ASSERT(!IsInsideNursery(&lir->object()->toConstant()->toObject()));
  } else {
    masm.branchPtrInNurseryChunk(Assembler::Equal, ToRegister(lir->object()),
                                 temp, ool->rejoin());
  }

  maybeEmitGlobalBarrierCheck(lir->object(), ool);

  ValueOperand value = ToValue(lir->value());
  masm.branchValueIsNurseryCell(Assembler::Equal, value, temp, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitPostWriteBarrierO(LPostWriteBarrierO* lir) {
  auto ool = new (alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
  visitPostWriteBarrierCommon<LPostWriteBarrierO, MIRType::Object>(lir, ool);
}

void CodeGenerator::visitPostWriteBarrierS(LPostWriteBarrierS* lir) {
  auto ool = new (alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
  visitPostWriteBarrierCommon<LPostWriteBarrierS, MIRType::String>(lir, ool);
}

void CodeGenerator::visitPostWriteBarrierBI(LPostWriteBarrierBI* lir) {
  auto ool = new (alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
  visitPostWriteBarrierCommon<LPostWriteBarrierBI, MIRType::BigInt>(lir, ool);
}

void CodeGenerator::visitPostWriteBarrierV(LPostWriteBarrierV* lir) {
  auto ool = new (alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
  visitPostWriteBarrierCommonV(lir, ool);
}

// Out-of-line path to update the store buffer.
class OutOfLineCallPostWriteElementBarrier
    : public OutOfLineCodeBase<CodeGenerator> {
  LInstruction* lir_;
  const LAllocation* object_;
  const LAllocation* index_;

 public:
  OutOfLineCallPostWriteElementBarrier(LInstruction* lir,
                                       const LAllocation* object,
                                       const LAllocation* index)
      : lir_(lir), object_(object), index_(index) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineCallPostWriteElementBarrier(this);
  }

  LInstruction* lir() const { return lir_; }

  const LAllocation* object() const { return object_; }

  const LAllocation* index() const { return index_; }
};

void CodeGenerator::visitOutOfLineCallPostWriteElementBarrier(
    OutOfLineCallPostWriteElementBarrier* ool) {
  saveLiveVolatile(ool->lir());

  const LAllocation* obj = ool->object();
  const LAllocation* index = ool->index();

  Register objreg = obj->isConstant() ? InvalidReg : ToRegister(obj);
  Register indexreg = ToRegister(index);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  regs.takeUnchecked(indexreg);

  if (obj->isConstant()) {
    objreg = regs.takeAny();
    masm.movePtr(ImmGCPtr(&obj->toConstant()->toObject()), objreg);
  } else {
    regs.takeUnchecked(objreg);
  }

  Register runtimereg = regs.takeAny();
  using Fn = void (*)(JSRuntime* rt, JSObject* obj, int32_t index);
  masm.setupAlignedABICall();
  masm.mov(ImmPtr(gen->runtime), runtimereg);
  masm.passABIArg(runtimereg);
  masm.passABIArg(objreg);
  masm.passABIArg(indexreg);
  masm.callWithABI<Fn, PostWriteElementBarrier>();

  restoreLiveVolatile(ool->lir());

  masm.jump(ool->rejoin());
}

void CodeGenerator::visitPostWriteElementBarrierO(
    LPostWriteElementBarrierO* lir) {
  auto ool = new (alloc())
      OutOfLineCallPostWriteElementBarrier(lir, lir->object(), lir->index());
  visitPostWriteBarrierCommon<LPostWriteElementBarrierO, MIRType::Object>(lir,
                                                                          ool);
}

void CodeGenerator::visitPostWriteElementBarrierS(
    LPostWriteElementBarrierS* lir) {
  auto ool = new (alloc())
      OutOfLineCallPostWriteElementBarrier(lir, lir->object(), lir->index());
  visitPostWriteBarrierCommon<LPostWriteElementBarrierS, MIRType::String>(lir,
                                                                          ool);
}

void CodeGenerator::visitPostWriteElementBarrierBI(
    LPostWriteElementBarrierBI* lir) {
  auto ool = new (alloc())
      OutOfLineCallPostWriteElementBarrier(lir, lir->object(), lir->index());
  visitPostWriteBarrierCommon<LPostWriteElementBarrierBI, MIRType::BigInt>(lir,
                                                                           ool);
}

void CodeGenerator::visitPostWriteElementBarrierV(
    LPostWriteElementBarrierV* lir) {
  auto ool = new (alloc())
      OutOfLineCallPostWriteElementBarrier(lir, lir->object(), lir->index());
  visitPostWriteBarrierCommonV(lir, ool);
}

void CodeGenerator::visitAssertCanElidePostWriteBarrier(
    LAssertCanElidePostWriteBarrier* lir) {
  Register object = ToRegister(lir->object());
  ValueOperand value = ToValue(lir->value());
  Register temp = ToRegister(lir->temp0());

  Label ok;
  masm.branchPtrInNurseryChunk(Assembler::Equal, object, temp, &ok);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, value, temp, &ok);

  masm.assumeUnreachable("Unexpected missing post write barrier");

  masm.bind(&ok);
}

template <typename LCallIns>
void CodeGenerator::emitCallNative(LCallIns* call, JSNative native,
                                   Register argContextReg, Register argUintNReg,
                                   Register argVpReg, Register tempReg,
                                   uint32_t unusedStack) {
  masm.checkStackAlignment();

  // Native functions have the signature:
  //  bool (*)(JSContext*, unsigned, Value* vp)
  // Where vp[0] is space for an outparam, vp[1] is |this|, and vp[2] onward
  // are the function arguments.

  // Allocate space for the outparam, moving the StackPointer to what will be
  // &vp[1].
  masm.adjustStack(unusedStack);

  // Push a Value containing the callee object: natives are allowed to access
  // their callee before setting the return value. The StackPointer is moved
  // to &vp[0].
  //
  // Also reserves the space for |NativeExitFrameLayout::{lo,hi}CalleeResult_|.
  if constexpr (std::is_same_v<LCallIns, LCallClassHook>) {
    Register calleeReg = ToRegister(call->getCallee());
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(calleeReg)));

    // Enter the callee realm.
    if (call->mir()->maybeCrossRealm()) {
      masm.switchToObjectRealm(calleeReg, tempReg);
    }
  } else {
    WrappedFunction* target = call->mir()->getSingleTarget();
    masm.Push(ObjectValue(*target->rawNativeJSFunction()));

    // Enter the callee realm.
    if (call->mir()->maybeCrossRealm()) {
      masm.movePtr(ImmGCPtr(target->rawNativeJSFunction()), tempReg);
      masm.switchToObjectRealm(tempReg, tempReg);
    }
  }

  // Preload arguments into registers.
  masm.loadJSContext(argContextReg);
  masm.moveStackPtrTo(argVpReg);

  // Initialize |NativeExitFrameLayout::argc_|.
  masm.Push(argUintNReg);

  // Construct native exit frame.
  //
  // |buildFakeExitFrame| initializes |NativeExitFrameLayout::exit_| and
  // |enterFakeExitFrameForNative| initializes |NativeExitFrameLayout::footer_|.
  //
  // The NativeExitFrameLayout is now fully initialized.
  uint32_t safepointOffset = masm.buildFakeExitFrame(tempReg);
  masm.enterFakeExitFrameForNative(argContextReg, tempReg,
                                   call->mir()->isConstructing());

  markSafepointAt(safepointOffset, call);

  // Construct and execute call.
  masm.setupAlignedABICall();
  masm.passABIArg(argContextReg);
  masm.passABIArg(argUintNReg);
  masm.passABIArg(argVpReg);

  ensureOsiSpace();
  // If we're using a simulator build, `native` will already point to the
  // simulator's call-redirection code for LCallClassHook. Load the address in
  // a register first so that we don't try to redirect it a second time.
  bool emittedCall = false;
#ifdef JS_SIMULATOR
  if constexpr (std::is_same_v<LCallIns, LCallClassHook>) {
    masm.movePtr(ImmPtr(native), tempReg);
    masm.callWithABI(tempReg);
    emittedCall = true;
  }
#endif
  if (!emittedCall) {
    masm.callWithABI(DynamicFunction<JSNative>(native), ABIType::General,
                     CheckUnsafeCallWithABI::DontCheckHasExitFrame);
  }

  // Test for failure.
  masm.branchIfFalseBool(ReturnReg, masm.failureLabel());

  // Exit the callee realm.
  if (call->mir()->maybeCrossRealm()) {
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
  }

  // Load the outparam vp[0] into output register(s).
  masm.loadValue(
      Address(masm.getStackPointer(), NativeExitFrameLayout::offsetOfResult()),
      JSReturnOperand);

  // Until C++ code is instrumented against Spectre, prevent speculative
  // execution from returning any private data.
  if (JitOptions.spectreJitToCxxCalls && !call->mir()->ignoresReturnValue() &&
      call->mir()->hasLiveDefUses()) {
    masm.speculationBarrier();
  }

#ifdef DEBUG
  // Native constructors are guaranteed to return an Object value.
  if (call->mir()->isConstructing()) {
    Label notPrimitive;
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.assumeUnreachable("native constructors don't return primitives");
    masm.bind(&notPrimitive);
  }
#endif
}

template <typename LCallIns>
void CodeGenerator::emitCallNative(LCallIns* call, JSNative native) {
  uint32_t unusedStack =
      UnusedStackBytesForCall(call->mir()->paddedNumStackArgs());

  // Registers used for callWithABI() argument-passing.
  const Register argContextReg = ToRegister(call->getArgContextReg());
  const Register argUintNReg = ToRegister(call->getArgUintNReg());
  const Register argVpReg = ToRegister(call->getArgVpReg());

  // Misc. temporary registers.
  const Register tempReg = ToRegister(call->getTempReg());

  DebugOnly<uint32_t> initialStack = masm.framePushed();

  // Initialize the argc register.
  masm.move32(Imm32(call->mir()->numActualArgs()), argUintNReg);

  // Create the exit frame and call the native.
  emitCallNative(call, native, argContextReg, argUintNReg, argVpReg, tempReg,
                 unusedStack);

  // The next instruction is removing the footer of the exit frame, so there
  // is no need for leaveFakeExitFrame.

  // Move the StackPointer back to its original location, unwinding the native
  // exit frame.
  masm.adjustStack(NativeExitFrameLayout::Size() - unusedStack);
  MOZ_ASSERT(masm.framePushed() == initialStack);
}

void CodeGenerator::visitCallNative(LCallNative* call) {
  WrappedFunction* target = call->getSingleTarget();
  MOZ_ASSERT(target);
  MOZ_ASSERT(target->isNativeWithoutJitEntry());

  JSNative native = target->native();
  if (call->ignoresReturnValue() && target->hasJitInfo()) {
    const JSJitInfo* jitInfo = target->jitInfo();
    if (jitInfo->type() == JSJitInfo::IgnoresReturnValueNative) {
      native = jitInfo->ignoresReturnValueMethod;
    }
  }
  emitCallNative(call, native);
}

void CodeGenerator::visitCallClassHook(LCallClassHook* call) {
  emitCallNative(call, call->mir()->target());
}

static void LoadDOMPrivate(MacroAssembler& masm, Register obj, Register priv,
                           DOMObjectKind kind) {
  // Load the value in DOM_OBJECT_SLOT for a native or proxy DOM object. This
  // will be in the first slot but may be fixed or non-fixed.
  MOZ_ASSERT(obj != priv);

  switch (kind) {
    case DOMObjectKind::Native:
      // If it's a native object, the value must be in a fixed slot.
      // See CanAttachDOMCall in CacheIR.cpp.
      masm.debugAssertObjHasFixedSlots(obj, priv);
      masm.loadPrivate(Address(obj, NativeObject::getFixedSlotOffset(0)), priv);
      break;
    case DOMObjectKind::Proxy: {
#ifdef DEBUG
      // Sanity check: it must be a DOM proxy.
      Label isDOMProxy;
      masm.branchTestProxyHandlerFamily(
          Assembler::Equal, obj, priv, GetDOMProxyHandlerFamily(), &isDOMProxy);
      masm.assumeUnreachable("Expected a DOM proxy");
      masm.bind(&isDOMProxy);
#endif
      masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), priv);
      masm.loadPrivate(
          Address(priv, js::detail::ProxyReservedSlots::offsetOfSlot(0)), priv);
      break;
    }
  }
}

void CodeGenerator::visitCallDOMNative(LCallDOMNative* call) {
  WrappedFunction* target = call->getSingleTarget();
  MOZ_ASSERT(target);
  MOZ_ASSERT(target->isNativeWithoutJitEntry());
  MOZ_ASSERT(target->hasJitInfo());
  MOZ_ASSERT(call->mir()->isCallDOMNative());

  int unusedStack = UnusedStackBytesForCall(call->mir()->paddedNumStackArgs());

  // Registers used for callWithABI() argument-passing.
  const Register argJSContext = ToRegister(call->getArgJSContext());
  const Register argObj = ToRegister(call->getArgObj());
  const Register argPrivate = ToRegister(call->getArgPrivate());
  const Register argArgs = ToRegister(call->getArgArgs());

  DebugOnly<uint32_t> initialStack = masm.framePushed();

  masm.checkStackAlignment();

  // DOM methods have the signature:
  //  bool (*)(JSContext*, HandleObject, void* private, const
  //  JSJitMethodCallArgs& args)
  // Where args is initialized from an argc and a vp, vp[0] is space for an
  // outparam and the callee, vp[1] is |this|, and vp[2] onward are the
  // function arguments.  Note that args stores the argv, not the vp, and
  // argv == vp + 2.

  // Nestle the stack up against the pushed arguments, leaving StackPointer at
  // &vp[1]
  masm.adjustStack(unusedStack);
  // argObj is filled with the extracted object, then returned.
  Register obj = masm.extractObject(Address(masm.getStackPointer(), 0), argObj);
  MOZ_ASSERT(obj == argObj);

  // Push a Value containing the callee object: natives are allowed to access
  // their callee before setting the return value. After this the StackPointer
  // points to &vp[0].
  masm.Push(ObjectValue(*target->rawNativeJSFunction()));

  // Now compute the argv value.  Since StackPointer is pointing to &vp[0] and
  // argv is &vp[2] we just need to add 2*sizeof(Value) to the current
  // StackPointer.
  static_assert(JSJitMethodCallArgsTraits::offsetOfArgv == 0);
  static_assert(JSJitMethodCallArgsTraits::offsetOfArgc ==
                IonDOMMethodExitFrameLayoutTraits::offsetOfArgcFromArgv);
  masm.computeEffectiveAddress(
      Address(masm.getStackPointer(), 2 * sizeof(Value)), argArgs);

  LoadDOMPrivate(masm, obj, argPrivate,
                 static_cast<MCallDOMNative*>(call->mir())->objectKind());

  // Push argc from the call instruction into what will become the IonExitFrame
  masm.Push(Imm32(call->numActualArgs()));

  // Push our argv onto the stack
  masm.Push(argArgs);
  // And store our JSJitMethodCallArgs* in argArgs.
  masm.moveStackPtrTo(argArgs);

  // Push |this| object for passing HandleObject. We push after argc to
  // maintain the same sp-relative location of the object pointer with other
  // DOMExitFrames.
  masm.Push(argObj);
  masm.moveStackPtrTo(argObj);

  if (call->mir()->maybeCrossRealm()) {
    // We use argJSContext as scratch register here.
    masm.movePtr(ImmGCPtr(target->rawNativeJSFunction()), argJSContext);
    masm.switchToObjectRealm(argJSContext, argJSContext);
  }

  bool preTenureWrapperAllocation =
      call->mir()->to<MCallDOMNative>()->initialHeap() == gc::Heap::Tenured;
  if (preTenureWrapperAllocation) {
    auto ptr = ImmPtr(mirGen().realm->zone()->tenuringAllocSite());
    masm.storeLocalAllocSite(ptr, argJSContext);
  }

  // Construct native exit frame.
  uint32_t safepointOffset = masm.buildFakeExitFrame(argJSContext);

  masm.loadJSContext(argJSContext);
  masm.enterFakeExitFrame(argJSContext, argJSContext,
                          ExitFrameType::IonDOMMethod);

  markSafepointAt(safepointOffset, call);

  // Construct and execute call.
  masm.setupAlignedABICall();
  masm.loadJSContext(argJSContext);
  masm.passABIArg(argJSContext);
  masm.passABIArg(argObj);
  masm.passABIArg(argPrivate);
  masm.passABIArg(argArgs);
  ensureOsiSpace();
  masm.callWithABI(DynamicFunction<JSJitMethodOp>(target->jitInfo()->method),
                   ABIType::General,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  if (target->jitInfo()->isInfallible) {
    masm.loadValue(Address(masm.getStackPointer(),
                           IonDOMMethodExitFrameLayout::offsetOfResult()),
                   JSReturnOperand);
  } else {
    // Test for failure.
    masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

    // Load the outparam vp[0] into output register(s).
    masm.loadValue(Address(masm.getStackPointer(),
                           IonDOMMethodExitFrameLayout::offsetOfResult()),
                   JSReturnOperand);
  }

  static_assert(!JSReturnOperand.aliases(ReturnReg),
                "Clobbering ReturnReg should not affect the return value");

  // Switch back to the current realm if needed. Note: if the DOM method threw
  // an exception, the exception handler will do this.
  if (call->mir()->maybeCrossRealm()) {
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
  }

  // Wipe out the preTenuring bit from the local alloc site
  // On exception we handle this in C++
  if (preTenureWrapperAllocation) {
    masm.storeLocalAllocSite(ImmPtr(nullptr), ReturnReg);
  }

  // Until C++ code is instrumented against Spectre, prevent speculative
  // execution from returning any private data.
  if (JitOptions.spectreJitToCxxCalls && call->mir()->hasLiveDefUses()) {
    masm.speculationBarrier();
  }

  // The next instruction is removing the footer of the exit frame, so there
  // is no need for leaveFakeExitFrame.

  // Move the StackPointer back to its original location, unwinding the native
  // exit frame.
  masm.adjustStack(IonDOMMethodExitFrameLayout::Size() - unusedStack);
  MOZ_ASSERT(masm.framePushed() == initialStack);
}

void CodeGenerator::visitCallGetIntrinsicValue(LCallGetIntrinsicValue* lir) {
  pushArg(ImmGCPtr(lir->mir()->name()));

  using Fn = bool (*)(JSContext* cx, Handle<PropertyName*>, MutableHandleValue);
  callVM<Fn, GetIntrinsicValue>(lir);
}

void CodeGenerator::emitCallInvokeFunction(
    LInstruction* call, Register calleereg, bool constructing,
    bool ignoresReturnValue, uint32_t argc, uint32_t unusedStack) {
  // Nestle %esp up to the argument vector.
  // Each path must account for framePushed_ separately, for callVM to be valid.
  masm.freeStack(unusedStack);

  pushArg(masm.getStackPointer());  // argv.
  pushArg(Imm32(argc));             // argc.
  pushArg(Imm32(ignoresReturnValue));
  pushArg(Imm32(constructing));  // constructing.
  pushArg(calleereg);            // JSFunction*.

  using Fn = bool (*)(JSContext*, HandleObject, bool, bool, uint32_t, Value*,
                      MutableHandleValue);
  callVM<Fn, jit::InvokeFunction>(call);

  // Un-nestle %esp from the argument vector. No prefix was pushed.
  masm.reserveStack(unusedStack);
}

void CodeGenerator::visitCallGeneric(LCallGeneric* call) {
  // The callee is passed straight through to the trampoline.
  MOZ_ASSERT(ToRegister(call->getCallee()) == IonGenericCallCalleeReg);

  Register argcReg = ToRegister(call->getArgc());
  uint32_t unusedStack =
      UnusedStackBytesForCall(call->mir()->paddedNumStackArgs());

  // Known-target case is handled by LCallKnown.
  MOZ_ASSERT(!call->hasSingleTarget());

  masm.checkStackAlignment();

  masm.move32(Imm32(call->numActualArgs()), argcReg);

  // Nestle the StackPointer up to the argument vector.
  masm.freeStack(unusedStack);
  ensureOsiSpace();

  auto kind = call->mir()->isConstructing() ? IonGenericCallKind::Construct
                                            : IonGenericCallKind::Call;

  TrampolinePtr genericCallStub =
      gen->jitRuntime()->getIonGenericCallStub(kind);
  uint32_t callOffset = masm.callJit(genericCallStub);
  markSafepointAt(callOffset, call);

  if (call->mir()->maybeCrossRealm()) {
    static_assert(!JSReturnOperand.aliases(ReturnReg),
                  "ReturnReg available as scratch after scripted calls");
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
  }

  // Restore stack pointer.
  masm.setFramePushed(frameSize());
  emitRestoreStackPointerFromFP();

  // If the return value of the constructing function is Primitive,
  // replace the return value with the Object from CreateThis.
  if (call->mir()->isConstructing()) {
    Label notPrimitive;
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.loadValue(Address(masm.getStackPointer(), unusedStack),
                   JSReturnOperand);
#ifdef DEBUG
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.assumeUnreachable("CreateThis creates an object");
#endif
    masm.bind(&notPrimitive);
  }
}

void JitRuntime::generateIonGenericCallArgumentsShift(
    MacroAssembler& masm, Register argc, Register curr, Register end,
    Register scratch, Label* done) {
  static_assert(sizeof(Value) == 8);
  // There are |argc| Values on the stack. Shift them all down by 8 bytes,
  // overwriting the first value.

  // Initialize `curr` to the destination of the first copy, and `end` to the
  // final value of curr.
  masm.moveStackPtrTo(curr);
  masm.computeEffectiveAddress(BaseValueIndex(curr, argc), end);

  Label loop;
  masm.bind(&loop);
  masm.branchPtr(Assembler::Equal, curr, end, done);
  masm.loadPtr(Address(curr, 8), scratch);
  masm.storePtr(scratch, Address(curr, 0));
  masm.addPtr(Imm32(sizeof(uintptr_t)), curr);
  masm.jump(&loop);
}

void JitRuntime::generateIonGenericCallStub(MacroAssembler& masm,
                                            IonGenericCallKind kind) {
  AutoCreatedBy acb(masm, "JitRuntime::generateIonGenericCallStub");
  ionGenericCallStubOffset_[kind] = startTrampolineCode(masm);

  // This code is tightly coupled with visitCallGeneric.
  //
  // Upon entry:
  //   IonGenericCallCalleeReg contains a pointer to the callee object.
  //   IonGenericCallArgcReg contains the number of actual args.
  //   The arguments have been pushed onto the stack:
  //     [newTarget] (iff isConstructing)
  //     [argN]
  //     ...
  //     [arg1]
  //     [arg0]
  //     [this]
  //     <return address> (if not JS_USE_LINK_REGISTER)
  //
  // This trampoline is responsible for entering the callee's realm,
  // massaging the stack into the right shape, and then performing a
  // tail call. We will return directly to the Ion code from the
  // callee.
  //
  // To do a tail call, we keep the return address in a register, even
  // on platforms that don't normally use a link register, and push it
  // just before jumping to the callee, after we are done setting up
  // the stack.
  //
  // The caller is responsible for switching back to the caller's
  // realm and cleaning up the stack.

  Register calleeReg = IonGenericCallCalleeReg;
  Register argcReg = IonGenericCallArgcReg;
  Register scratch = IonGenericCallScratch;
  Register scratch2 = IonGenericCallScratch2;

#ifndef JS_USE_LINK_REGISTER
  Register returnAddrReg = IonGenericCallReturnAddrReg;
  masm.pop(returnAddrReg);
#endif

#ifdef JS_CODEGEN_ARM
  // The default second scratch register on arm is lr, which we need
  // preserved for tail calls.
  AutoNonDefaultSecondScratchRegister andssr(masm, IonGenericSecondScratchReg);
#endif

  bool isConstructing = kind == IonGenericCallKind::Construct;

  Label entry, notFunction, noJitEntry, vmCall;
  masm.bind(&entry);

  // Guard that the callee is actually a function.
  masm.branchTestObjIsFunction(Assembler::NotEqual, calleeReg, scratch,
                               calleeReg, &notFunction);

  // Guard that the callee supports the [[Call]] or [[Construct]] operation.
  // If these tests fail, we will call into the VM to throw an exception.
  if (isConstructing) {
    masm.branchTestFunctionFlags(calleeReg, FunctionFlags::CONSTRUCTOR,
                                 Assembler::Zero, &vmCall);
  } else {
    masm.branchFunctionKind(Assembler::Equal, FunctionFlags::ClassConstructor,
                            calleeReg, scratch, &vmCall);
  }

  if (isConstructing) {
    // Use the slow path if CreateThis was unable to create the |this| object.
    Address thisAddr(masm.getStackPointer(), 0);
    masm.branchTestNull(Assembler::Equal, thisAddr, &vmCall);
  }

  masm.switchToObjectRealm(calleeReg, scratch);

  // Load jitCodeRaw for callee if it exists.
  masm.branchIfFunctionHasNoJitEntry(calleeReg, &noJitEntry);

  // ****************************
  // * Functions with jit entry *
  // ****************************
  masm.loadJitCodeRaw(calleeReg, scratch2);

  // Construct the JitFrameLayout.
  masm.PushCalleeToken(calleeReg, isConstructing);
  masm.PushFrameDescriptorForJitCall(FrameType::IonJS, argcReg, scratch);
#ifndef JS_USE_LINK_REGISTER
  masm.push(returnAddrReg);
#endif

  // Check whether we need a rectifier frame.
  Label noRectifier;
  masm.loadFunctionArgCount(calleeReg, scratch);
  masm.branch32(Assembler::BelowOrEqual, scratch, argcReg, &noRectifier);
  {
    // Tail-call the arguments rectifier.
    // Because all trampolines are created at the same time,
    // we can't create a TrampolinePtr for the arguments rectifier,
    // because it hasn't been linked yet. We can, however, directly
    // encode its offset.
    Label rectifier;
    bindLabelToOffset(&rectifier, argumentsRectifierOffset_);

    masm.jump(&rectifier);
  }

  // Tail call the jit entry.
  masm.bind(&noRectifier);
  masm.jump(scratch2);

  // ********************
  // * Native functions *
  // ********************
  masm.bind(&noJitEntry);
  if (!isConstructing) {
    generateIonGenericCallFunCall(masm, &entry, &vmCall);
  }
  generateIonGenericCallNativeFunction(masm, isConstructing);

  // *******************
  // * Bound functions *
  // *******************
  // TODO: support class hooks?
  masm.bind(&notFunction);
  if (!isConstructing) {
    // TODO: support generic bound constructors?
    generateIonGenericCallBoundFunction(masm, &entry, &vmCall);
  }

  // ********************
  // * Fallback VM call *
  // ********************
  masm.bind(&vmCall);

  masm.push(masm.getStackPointer());  // argv
  masm.push(argcReg);                 // argc
  masm.push(Imm32(false));            // ignores return value
  masm.push(Imm32(isConstructing));   // constructing
  masm.push(calleeReg);               // callee

  using Fn = bool (*)(JSContext*, HandleObject, bool, bool, uint32_t, Value*,
                      MutableHandleValue);
  VMFunctionId id = VMFunctionToId<Fn, jit::InvokeFunction>::id;
  uint32_t invokeFunctionOffset = functionWrapperOffsets_[size_t(id)];
  Label invokeFunctionVMEntry;
  bindLabelToOffset(&invokeFunctionVMEntry, invokeFunctionOffset);

  masm.pushFrameDescriptor(FrameType::IonJS);
#ifndef JS_USE_LINK_REGISTER
  masm.push(returnAddrReg);
#endif
  masm.jump(&invokeFunctionVMEntry);
}

void JitRuntime::generateIonGenericCallNativeFunction(MacroAssembler& masm,
                                                      bool isConstructing) {
  Register calleeReg = IonGenericCallCalleeReg;
  Register argcReg = IonGenericCallArgcReg;
  Register scratch = IonGenericCallScratch;
  Register scratch2 = IonGenericCallScratch2;
  Register contextReg = IonGenericCallScratch3;
#ifndef JS_USE_LINK_REGISTER
  Register returnAddrReg = IonGenericCallReturnAddrReg;
#endif

  // Push a value containing the callee, which will become argv[0].
  masm.pushValue(JSVAL_TYPE_OBJECT, calleeReg);

  // Load the callee address into calleeReg.
#ifdef JS_SIMULATOR
  masm.movePtr(ImmPtr(RedirectedCallAnyNative()), calleeReg);
#else
  masm.loadPrivate(Address(calleeReg, JSFunction::offsetOfNativeOrEnv()),
                   calleeReg);
#endif

  // Load argv into scratch2.
  masm.moveStackPtrTo(scratch2);

  // Push argc.
  masm.push(argcReg);

  masm.loadJSContext(contextReg);

  // Construct native exit frame. Note that unlike other cases in this
  // trampoline, this code does not use a tail call.
  masm.pushFrameDescriptor(FrameType::IonJS);
#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#else
  masm.push(returnAddrReg);
#endif

  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);
  masm.enterFakeExitFrameForNative(contextReg, scratch, isConstructing);

  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(contextReg);  // cx
  masm.passABIArg(argcReg);     // argc
  masm.passABIArg(scratch2);    // argv

  masm.callWithABI(calleeReg);

  // Test for failure.
  masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

  masm.loadValue(
      Address(masm.getStackPointer(), NativeExitFrameLayout::offsetOfResult()),
      JSReturnOperand);

  // Leave the exit frame.
  masm.moveToStackPtr(FramePointer);
  masm.pop(FramePointer);

  // Return.
  masm.ret();
}

void JitRuntime::generateIonGenericCallFunCall(MacroAssembler& masm,
                                               Label* entry, Label* vmCall) {
  Register calleeReg = IonGenericCallCalleeReg;
  Register argcReg = IonGenericCallArgcReg;
  Register scratch = IonGenericCallScratch;
  Register scratch2 = IonGenericCallScratch2;
  Register scratch3 = IonGenericCallScratch3;

  Label notFunCall;
  masm.branchPtr(Assembler::NotEqual,
                 Address(calleeReg, JSFunction::offsetOfNativeOrEnv()),
                 ImmPtr(js::fun_call), &notFunCall);

  // In general, we can implement fun_call by replacing calleeReg with
  // |this|, sliding all the other arguments down, and decrementing argc.
  //
  // *BEFORE*                           *AFTER*
  //  [argN]  argc = N+1                 <padding>
  //  ...                                [argN]  argc = N
  //  [arg1]                             ...
  //  [arg0]                             [arg1] <- now arg0
  //  [this] <- top of stack (aligned)   [arg0] <- now this
  //
  // The only exception is when argc is already 0, in which case instead
  // of shifting arguments down we replace [this] with UndefinedValue():
  //
  // *BEFORE*                           *AFTER*
  // [this] argc = 0                     [undef] argc = 0
  //
  // After making this transformation, we can jump back to the beginning
  // of this trampoline to handle the inner call.

  // Guard that |this| is an object. If it is, replace calleeReg.
  masm.fallibleUnboxObject(Address(masm.getStackPointer(), 0), scratch, vmCall);
  masm.movePtr(scratch, calleeReg);

  Label hasArgs;
  masm.branch32(Assembler::NotEqual, argcReg, Imm32(0), &hasArgs);

  // No arguments. Replace |this| with |undefined| and start from the top.
  masm.storeValue(UndefinedValue(), Address(masm.getStackPointer(), 0));
  masm.jump(entry);

  masm.bind(&hasArgs);

  Label doneSliding;
  generateIonGenericCallArgumentsShift(masm, argcReg, scratch, scratch2,
                                       scratch3, &doneSliding);
  masm.bind(&doneSliding);
  masm.sub32(Imm32(1), argcReg);

  masm.jump(entry);

  masm.bind(&notFunCall);
}

void JitRuntime::generateIonGenericCallBoundFunction(MacroAssembler& masm,
                                                     Label* entry,
                                                     Label* vmCall) {
  Register calleeReg = IonGenericCallCalleeReg;
  Register argcReg = IonGenericCallArgcReg;
  Register scratch = IonGenericCallScratch;
  Register scratch2 = IonGenericCallScratch2;
  Register scratch3 = IonGenericCallScratch3;

  masm.branchTestObjClass(Assembler::NotEqual, calleeReg,
                          &BoundFunctionObject::class_, scratch, calleeReg,
                          vmCall);

  Address targetSlot(calleeReg, BoundFunctionObject::offsetOfTargetSlot());
  Address flagsSlot(calleeReg, BoundFunctionObject::offsetOfFlagsSlot());
  Address thisSlot(calleeReg, BoundFunctionObject::offsetOfBoundThisSlot());
  Address firstInlineArgSlot(
      calleeReg, BoundFunctionObject::offsetOfFirstInlineBoundArg());

  // Check that we won't be pushing too many arguments.
  masm.load32(flagsSlot, scratch);
  masm.rshift32(Imm32(BoundFunctionObject::NumBoundArgsShift), scratch);
  masm.add32(argcReg, scratch);
  masm.branch32(Assembler::Above, scratch, Imm32(JIT_ARGS_LENGTH_MAX), vmCall);

  // The stack is currently correctly aligned for a jit call. We will
  // be updating the `this` value and potentially adding additional
  // arguments. On platforms with 16-byte alignment, if the number of
  // bound arguments is odd, we have to move the arguments that are
  // currently on the stack. For example, with one bound argument:
  //
  // *BEFORE*                           *AFTER*
  //  [argN]                             <padding>
  //  ...                                [argN]   |
  //  [arg1]                             ...      |  These arguments have been
  //  [arg0]                             [arg1]   |  shifted down 8 bytes.
  //  [this] <- top of stack (aligned)   [arg0]   v
  //                                     [bound0]    <- one bound argument (odd)
  //                                     [boundThis] <- top of stack (aligned)
  //
  Label poppedThis;
  if (JitStackValueAlignment > 1) {
    Label alreadyAligned;
    masm.branchTest32(Assembler::Zero, flagsSlot,
                      Imm32(1 << BoundFunctionObject::NumBoundArgsShift),
                      &alreadyAligned);

    // We have an odd number of bound arguments. Shift the existing arguments
    // down by 8 bytes.
    generateIonGenericCallArgumentsShift(masm, argcReg, scratch, scratch2,
                                         scratch3, &poppedThis);
    masm.bind(&alreadyAligned);
  }

  // Pop the current `this`. It will be replaced with the bound `this`.
  masm.freeStack(sizeof(Value));
  masm.bind(&poppedThis);

  // Load the number of bound arguments in scratch
  masm.load32(flagsSlot, scratch);
  masm.rshift32(Imm32(BoundFunctionObject::NumBoundArgsShift), scratch);

  Label donePushingBoundArguments;
  masm.branch32(Assembler::Equal, scratch, Imm32(0),
                &donePushingBoundArguments);

  // Update argc to include bound arguments.
  masm.add32(scratch, argcReg);

  // Load &boundArgs[0] in scratch2.
  Label outOfLineBoundArguments, haveBoundArguments;
  masm.branch32(Assembler::Above, scratch,
                Imm32(BoundFunctionObject::MaxInlineBoundArgs),
                &outOfLineBoundArguments);
  masm.computeEffectiveAddress(firstInlineArgSlot, scratch2);
  masm.jump(&haveBoundArguments);

  masm.bind(&outOfLineBoundArguments);
  masm.unboxObject(firstInlineArgSlot, scratch2);
  masm.loadPtr(Address(scratch2, NativeObject::offsetOfElements()), scratch2);

  masm.bind(&haveBoundArguments);

  // Load &boundArgs[numBoundArgs] in scratch.
  BaseObjectElementIndex lastBoundArg(scratch2, scratch);
  masm.computeEffectiveAddress(lastBoundArg, scratch);

  // Push the bound arguments, starting with the last one.
  // Copying pre-decrements scratch until scratch2 is reached.
  Label boundArgumentsLoop;
  masm.bind(&boundArgumentsLoop);
  masm.subPtr(Imm32(sizeof(Value)), scratch);
  masm.pushValue(Address(scratch, 0));
  masm.branchPtr(Assembler::Above, scratch, scratch2, &boundArgumentsLoop);
  masm.bind(&donePushingBoundArguments);

  // Push the bound `this`.
  masm.pushValue(thisSlot);

  // Load the target in calleeReg.
  masm.unboxObject(targetSlot, calleeReg);

  // At this point, all preconditions for entering the trampoline are met:
  // - calleeReg contains a pointer to the callee object
  // - argcReg contains the number of actual args (now including bound args)
  // - the arguments are on the stack with the correct alignment.
  // Instead of generating more code, we can jump back to the entry point
  // of the trampoline to call the bound target.
  masm.jump(entry);
}

void CodeGenerator::visitCallKnown(LCallKnown* call) {
  Register calleereg = ToRegister(call->getFunction());
  Register objreg = ToRegister(call->getTempObject());
  uint32_t unusedStack =
      UnusedStackBytesForCall(call->mir()->paddedNumStackArgs());
  WrappedFunction* target = call->getSingleTarget();

  // Native single targets (except Wasm and TrampolineNative functions) are
  // handled by LCallNative.
  MOZ_ASSERT(target->hasJitEntry());

  // Missing arguments must have been explicitly appended by WarpBuilder.
  DebugOnly<unsigned> numNonArgsOnStack = 1 + call->isConstructing();
  MOZ_ASSERT(target->nargs() <=
             call->mir()->numStackArgs() - numNonArgsOnStack);

  MOZ_ASSERT_IF(call->isConstructing(), target->isConstructor());

  masm.checkStackAlignment();

  if (target->isClassConstructor() && !call->isConstructing()) {
    emitCallInvokeFunction(call, calleereg, call->isConstructing(),
                           call->ignoresReturnValue(), call->numActualArgs(),
                           unusedStack);
    return;
  }

  MOZ_ASSERT_IF(target->isClassConstructor(), call->isConstructing());

  MOZ_ASSERT(!call->mir()->needsThisCheck());

  if (call->mir()->maybeCrossRealm()) {
    masm.switchToObjectRealm(calleereg, objreg);
  }

  masm.loadJitCodeRaw(calleereg, objreg);

  // Nestle the StackPointer up to the argument vector.
  masm.freeStack(unusedStack);

  // Construct the JitFrameLayout.
  masm.PushCalleeToken(calleereg, call->mir()->isConstructing());
  masm.PushFrameDescriptorForJitCall(FrameType::IonJS, call->numActualArgs());

  // Finally call the function in objreg.
  ensureOsiSpace();
  uint32_t callOffset = masm.callJit(objreg);
  markSafepointAt(callOffset, call);

  if (call->mir()->maybeCrossRealm()) {
    static_assert(!JSReturnOperand.aliases(ReturnReg),
                  "ReturnReg available as scratch after scripted calls");
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
  }

  // Restore stack pointer: pop JitFrameLayout fields still left on the stack
  // and undo the earlier |freeStack(unusedStack)|.
  int prefixGarbage =
      sizeof(JitFrameLayout) - JitFrameLayout::bytesPoppedAfterCall();
  masm.adjustStack(prefixGarbage - unusedStack);

  // If the return value of the constructing function is Primitive,
  // replace the return value with the Object from CreateThis.
  if (call->mir()->isConstructing()) {
    Label notPrimitive;
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.loadValue(Address(masm.getStackPointer(), unusedStack),
                   JSReturnOperand);
#ifdef DEBUG
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.assumeUnreachable("CreateThis creates an object");
#endif
    masm.bind(&notPrimitive);
  }
}

template <typename T>
void CodeGenerator::emitCallInvokeFunction(T* apply) {
  pushArg(masm.getStackPointer());                     // argv.
  pushArg(ToRegister(apply->getArgc()));               // argc.
  pushArg(Imm32(apply->mir()->ignoresReturnValue()));  // ignoresReturnValue.
  pushArg(Imm32(apply->mir()->isConstructing()));      // isConstructing.
  pushArg(ToRegister(apply->getFunction()));           // JSFunction*.

  using Fn = bool (*)(JSContext*, HandleObject, bool, bool, uint32_t, Value*,
                      MutableHandleValue);
  callVM<Fn, jit::InvokeFunction>(apply);
}

// Do not bailout after the execution of this function since the stack no longer
// correspond to what is expected by the snapshots.
void CodeGenerator::emitAllocateSpaceForApply(Register argcreg,
                                              Register scratch) {
  // Use scratch register to calculate stack space (including padding).
  masm.movePtr(argcreg, scratch);

  // Align the JitFrameLayout on the JitStackAlignment.
  if (JitStackValueAlignment > 1) {
    MOZ_ASSERT(frameSize() % JitStackAlignment == 0,
               "Stack padding assumes that the frameSize is correct");
    MOZ_ASSERT(JitStackValueAlignment == 2);
    Label noPaddingNeeded;
    // If the number of arguments is odd, then we do not need any padding.
    //
    // Note: The |JitStackValueAlignment == 2| condition requires that the
    // overall number of values on the stack is even. When we have an odd number
    // of arguments, we don't need any padding, because the |thisValue| is
    // pushed after the arguments, so the overall number of values on the stack
    // is even.
    masm.branchTestPtr(Assembler::NonZero, argcreg, Imm32(1), &noPaddingNeeded);
    masm.addPtr(Imm32(1), scratch);
    masm.bind(&noPaddingNeeded);
  }

  // Reserve space for copying the arguments.
  NativeObject::elementsSizeMustNotOverflow();
  masm.lshiftPtr(Imm32(ValueShift), scratch);
  masm.subFromStackPtr(scratch);

#ifdef DEBUG
  // Put a magic value in the space reserved for padding. Note, this code cannot
  // be merged with the previous test, as not all architectures can write below
  // their stack pointers.
  if (JitStackValueAlignment > 1) {
    MOZ_ASSERT(JitStackValueAlignment == 2);
    Label noPaddingNeeded;
    // If the number of arguments is odd, then we do not need any padding.
    masm.branchTestPtr(Assembler::NonZero, argcreg, Imm32(1), &noPaddingNeeded);
    BaseValueIndex dstPtr(masm.getStackPointer(), argcreg);
    masm.storeValue(MagicValue(JS_ARG_POISON), dstPtr);
    masm.bind(&noPaddingNeeded);
  }
#endif
}

// Do not bailout after the execution of this function since the stack no longer
// correspond to what is expected by the snapshots.
void CodeGenerator::emitAllocateSpaceForConstructAndPushNewTarget(
    Register argcreg, Register newTargetAndScratch) {
  // Align the JitFrameLayout on the JitStackAlignment. Contrary to
  // |emitAllocateSpaceForApply()|, we're always pushing a magic value, because
  // we can't write to |newTargetAndScratch| before |new.target| has been pushed
  // onto the stack.
  if (JitStackValueAlignment > 1) {
    MOZ_ASSERT(frameSize() % JitStackAlignment == 0,
               "Stack padding assumes that the frameSize is correct");
    MOZ_ASSERT(JitStackValueAlignment == 2);

    Label noPaddingNeeded;
    // If the number of arguments is even, then we do not need any padding.
    //
    // Note: The |JitStackValueAlignment == 2| condition requires that the
    // overall number of values on the stack is even. When we have an even
    // number of arguments, we don't need any padding, because |new.target| is
    // is pushed before the arguments and |thisValue| is pushed after all
    // arguments, so the overall number of values on the stack is even.
    masm.branchTestPtr(Assembler::Zero, argcreg, Imm32(1), &noPaddingNeeded);
    masm.pushValue(MagicValue(JS_ARG_POISON));
    masm.bind(&noPaddingNeeded);
  }

  // Push |new.target| after the padding value, but before any arguments.
  masm.pushValue(JSVAL_TYPE_OBJECT, newTargetAndScratch);

  // Use newTargetAndScratch to calculate stack space (including padding).
  masm.movePtr(argcreg, newTargetAndScratch);

  // Reserve space for copying the arguments.
  NativeObject::elementsSizeMustNotOverflow();
  masm.lshiftPtr(Imm32(ValueShift), newTargetAndScratch);
  masm.subFromStackPtr(newTargetAndScratch);
}

// Destroys argvIndex and copyreg.
void CodeGenerator::emitCopyValuesForApply(Register argvSrcBase,
                                           Register argvIndex, Register copyreg,
                                           size_t argvSrcOffset,
                                           size_t argvDstOffset) {
  Label loop;
  masm.bind(&loop);

  // As argvIndex is off by 1, and we use the decBranchPtr instruction to loop
  // back, we have to substract the size of the word which are copied.
  BaseValueIndex srcPtr(argvSrcBase, argvIndex,
                        int32_t(argvSrcOffset) - sizeof(void*));
  BaseValueIndex dstPtr(masm.getStackPointer(), argvIndex,
                        int32_t(argvDstOffset) - sizeof(void*));
  masm.loadPtr(srcPtr, copyreg);
  masm.storePtr(copyreg, dstPtr);

  // Handle 32 bits architectures.
  if (sizeof(Value) == 2 * sizeof(void*)) {
    BaseValueIndex srcPtrLow(argvSrcBase, argvIndex,
                             int32_t(argvSrcOffset) - 2 * sizeof(void*));
    BaseValueIndex dstPtrLow(masm.getStackPointer(), argvIndex,
                             int32_t(argvDstOffset) - 2 * sizeof(void*));
    masm.loadPtr(srcPtrLow, copyreg);
    masm.storePtr(copyreg, dstPtrLow);
  }

  masm.decBranchPtr(Assembler::NonZero, argvIndex, Imm32(1), &loop);
}

void CodeGenerator::emitRestoreStackPointerFromFP() {
  // This is used to restore the stack pointer after a call with a dynamic
  // number of arguments.

  MOZ_ASSERT(masm.framePushed() == frameSize());

  int32_t offset = -int32_t(frameSize());
  masm.computeEffectiveAddress(Address(FramePointer, offset),
                               masm.getStackPointer());
#if JS_CODEGEN_ARM64
  masm.syncStackPtr();
#endif
}

void CodeGenerator::emitPushArguments(Register argcreg, Register scratch,
                                      Register copyreg, uint32_t extraFormals) {
  Label end;

  // Skip the copy of arguments if there are none.
  masm.branchTestPtr(Assembler::Zero, argcreg, argcreg, &end);

  // clang-format off
  //
  // We are making a copy of the arguments which are above the JitFrameLayout
  // of the current Ion frame.
  //
  // [arg1] [arg0] <- src [this] [JitFrameLayout] [.. frameSize ..] [pad] [arg1] [arg0] <- dst
  //
  // clang-format on

  // Compute the source and destination offsets into the stack.
  //
  // The |extraFormals| parameter is used when copying rest-parameters and
  // allows to skip the initial parameters before the actual rest-parameters.
  Register argvSrcBase = FramePointer;
  size_t argvSrcOffset =
      JitFrameLayout::offsetOfActualArgs() + extraFormals * sizeof(JS::Value);
  size_t argvDstOffset = 0;

  Register argvIndex = scratch;
  masm.move32(argcreg, argvIndex);

  // Copy arguments.
  emitCopyValuesForApply(argvSrcBase, argvIndex, copyreg, argvSrcOffset,
                         argvDstOffset);

  // Join with all arguments copied.
  masm.bind(&end);
}

void CodeGenerator::emitPushArguments(LApplyArgsGeneric* apply) {
  // Holds the function nargs.
  Register argcreg = ToRegister(apply->getArgc());
  Register copyreg = ToRegister(apply->getTempObject());
  Register scratch = ToRegister(apply->getTempForArgCopy());
  uint32_t extraFormals = apply->numExtraFormals();

  // Allocate space on the stack for arguments.
  emitAllocateSpaceForApply(argcreg, scratch);

  emitPushArguments(argcreg, scratch, copyreg, extraFormals);

  // Push |this|.
  masm.pushValue(ToValue(apply->thisValue()));
}

void CodeGenerator::emitPushArguments(LApplyArgsObj* apply) {
  Register argsObj = ToRegister(apply->getArgsObj());
  Register tmpArgc = ToRegister(apply->getTempObject());
  Register scratch = ToRegister(apply->getTempForArgCopy());

  // argc and argsObj are mapped to the same calltemp register.
  MOZ_ASSERT(argsObj == ToRegister(apply->getArgc()));

  // Load argc into tmpArgc.
  masm.loadArgumentsObjectLength(argsObj, tmpArgc);

  // Allocate space on the stack for arguments.
  emitAllocateSpaceForApply(tmpArgc, scratch);

  // Load arguments data.
  masm.loadPrivate(Address(argsObj, ArgumentsObject::getDataSlotOffset()),
                   argsObj);
  size_t argsSrcOffset = ArgumentsData::offsetOfArgs();

  // This is the end of the lifetime of argsObj.
  // After this call, the argsObj register holds the argument count instead.
  emitPushArrayAsArguments(tmpArgc, argsObj, scratch, argsSrcOffset);

  // Push |this|.
  masm.pushValue(ToValue(apply->thisValue()));
}

void CodeGenerator::emitPushArrayAsArguments(Register tmpArgc,
                                             Register srcBaseAndArgc,
                                             Register scratch,
                                             size_t argvSrcOffset) {
  // Preconditions:
  // 1. |tmpArgc| * sizeof(Value) bytes have been allocated at the top of
  //    the stack to hold arguments.
  // 2. |srcBaseAndArgc| + |srcOffset| points to an array of |tmpArgc| values.
  //
  // Postconditions:
  // 1. The arguments at |srcBaseAndArgc| + |srcOffset| have been copied into
  //    the allocated space.
  // 2. |srcBaseAndArgc| now contains the original value of |tmpArgc|.
  //
  // |scratch| is used as a temp register within this function and clobbered.

  Label noCopy, epilogue;

  // Skip the copy of arguments if there are none.
  masm.branchTestPtr(Assembler::Zero, tmpArgc, tmpArgc, &noCopy);
  {
    // Copy the values. This code is skipped entirely if there are no values.
    size_t argvDstOffset = 0;

    Register argvSrcBase = srcBaseAndArgc;

    // Stash away |tmpArgc| and adjust argvDstOffset accordingly.
    masm.push(tmpArgc);
    Register argvIndex = tmpArgc;
    argvDstOffset += sizeof(void*);

    // Copy
    emitCopyValuesForApply(argvSrcBase, argvIndex, scratch, argvSrcOffset,
                           argvDstOffset);

    // Restore.
    masm.pop(srcBaseAndArgc);  // srcBaseAndArgc now contains argc.
    masm.jump(&epilogue);
  }
  masm.bind(&noCopy);
  {
    // Clear argc if we skipped the copy step.
    masm.movePtr(ImmWord(0), srcBaseAndArgc);
  }

  // Join with all arguments copied.
  // Note, "srcBase" has become "argc".
  masm.bind(&epilogue);
}

void CodeGenerator::emitPushArguments(LApplyArrayGeneric* apply) {
  Register elements = ToRegister(apply->getElements());
  Register tmpArgc = ToRegister(apply->getTempObject());
  Register scratch = ToRegister(apply->getTempForArgCopy());

  // argc and elements are mapped to the same calltemp register.
  MOZ_ASSERT(elements == ToRegister(apply->getArgc()));

  // Invariants guarded in the caller:
  //  - the array is not too long
  //  - the array length equals its initialized length

  // The array length is our argc for the purposes of allocating space.
  masm.load32(Address(elements, ObjectElements::offsetOfLength()), tmpArgc);

  // Allocate space for the values.
  emitAllocateSpaceForApply(tmpArgc, scratch);

  // After this call "elements" has become "argc".
  size_t elementsOffset = 0;
  emitPushArrayAsArguments(tmpArgc, elements, scratch, elementsOffset);

  // Push |this|.
  masm.pushValue(ToValue(apply->thisValue()));
}

void CodeGenerator::emitPushArguments(LConstructArgsGeneric* construct) {
  // Holds the function nargs.
  Register argcreg = ToRegister(construct->getArgc());
  Register copyreg = ToRegister(construct->getTempObject());
  Register scratch = ToRegister(construct->getTempForArgCopy());
  uint32_t extraFormals = construct->numExtraFormals();

  // newTarget and scratch are mapped to the same calltemp register.
  MOZ_ASSERT(scratch == ToRegister(construct->getNewTarget()));

  // Allocate space for the values.
  // After this call "newTarget" has become "scratch".
  emitAllocateSpaceForConstructAndPushNewTarget(argcreg, scratch);

  emitPushArguments(argcreg, scratch, copyreg, extraFormals);

  // Push |this|.
  masm.pushValue(ToValue(construct->thisValue()));
}

void CodeGenerator::emitPushArguments(LConstructArrayGeneric* construct) {
  Register elements = ToRegister(construct->getElements());
  Register tmpArgc = ToRegister(construct->getTempObject());
  Register scratch = ToRegister(construct->getTempForArgCopy());

  // argc and elements are mapped to the same calltemp register.
  MOZ_ASSERT(elements == ToRegister(construct->getArgc()));

  // newTarget and scratch are mapped to the same calltemp register.
  MOZ_ASSERT(scratch == ToRegister(construct->getNewTarget()));

  // Invariants guarded in the caller:
  //  - the array is not too long
  //  - the array length equals its initialized length

  // The array length is our argc for the purposes of allocating space.
  masm.load32(Address(elements, ObjectElements::offsetOfLength()), tmpArgc);

  // Allocate space for the values.
  // After this call "newTarget" has become "scratch".
  emitAllocateSpaceForConstructAndPushNewTarget(tmpArgc, scratch);

  // After this call "elements" has become "argc".
  size_t elementsOffset = 0;
  emitPushArrayAsArguments(tmpArgc, elements, scratch, elementsOffset);

  // Push |this|.
  masm.pushValue(ToValue(construct->thisValue()));
}

template <typename T>
void CodeGenerator::emitApplyGeneric(T* apply) {
  // Holds the function object.
  Register calleereg = ToRegister(apply->getFunction());

  // Temporary register for modifying the function object.
  Register objreg = ToRegister(apply->getTempObject());
  Register scratch = ToRegister(apply->getTempForArgCopy());

  // Holds the function nargs, computed in the invoker or (for ApplyArray,
  // ConstructArray, or ApplyArgsObj) in the argument pusher.
  Register argcreg = ToRegister(apply->getArgc());

  // Copy the arguments of the current function.
  //
  // In the case of ApplyArray, ConstructArray, or ApplyArgsObj, also compute
  // argc. The argc register and the elements/argsObj register are the same;
  // argc must not be referenced before the call to emitPushArguments() and
  // elements/argsObj must not be referenced after it returns.
  //
  // In the case of ConstructArray or ConstructArgs, also overwrite newTarget;
  // newTarget must not be referenced after this point.
  //
  // objreg is dead across this call.
  emitPushArguments(apply);

  masm.checkStackAlignment();

  bool constructing = apply->mir()->isConstructing();

  // If the function is native, the call is compiled through emitApplyNative.
  MOZ_ASSERT_IF(apply->hasSingleTarget(),
                !apply->getSingleTarget()->isNativeWithoutJitEntry());

  Label end, invoke;

  // Unless already known, guard that calleereg is actually a function object.
  if (!apply->hasSingleTarget()) {
    masm.branchTestObjIsFunction(Assembler::NotEqual, calleereg, objreg,
                                 calleereg, &invoke);
  }

  // Guard that calleereg is an interpreted function with a JSScript.
  masm.branchIfFunctionHasNoJitEntry(calleereg, &invoke);

  // Guard that callee allows the [[Call]] or [[Construct]] operation required.
  if (constructing) {
    masm.branchTestFunctionFlags(calleereg, FunctionFlags::CONSTRUCTOR,
                                 Assembler::Zero, &invoke);
  } else {
    masm.branchFunctionKind(Assembler::Equal, FunctionFlags::ClassConstructor,
                            calleereg, objreg, &invoke);
  }

  // Use the slow path if CreateThis was unable to create the |this| object.
  if (constructing) {
    Address thisAddr(masm.getStackPointer(), 0);
    masm.branchTestNull(Assembler::Equal, thisAddr, &invoke);
  }

  // Call with an Ion frame or a rectifier frame.
  {
    if (apply->mir()->maybeCrossRealm()) {
      masm.switchToObjectRealm(calleereg, objreg);
    }

    // Knowing that calleereg is a non-native function, load jitcode.
    masm.loadJitCodeRaw(calleereg, objreg);

    masm.PushCalleeToken(calleereg, constructing);
    masm.PushFrameDescriptorForJitCall(FrameType::IonJS, argcreg, scratch);

    Label underflow, rejoin;

    // Check whether the provided arguments satisfy target argc.
    if (!apply->hasSingleTarget()) {
      Register nformals = scratch;
      masm.loadFunctionArgCount(calleereg, nformals);
      masm.branch32(Assembler::Below, argcreg, nformals, &underflow);
    } else {
      masm.branch32(Assembler::Below, argcreg,
                    Imm32(apply->getSingleTarget()->nargs()), &underflow);
    }

    // Skip the construction of the rectifier frame because we have no
    // underflow.
    masm.jump(&rejoin);

    // Argument fixup needed. Get ready to call the argumentsRectifier.
    {
      masm.bind(&underflow);

      // Hardcode the address of the argumentsRectifier code.
      TrampolinePtr argumentsRectifier =
          gen->jitRuntime()->getArgumentsRectifier();
      masm.movePtr(argumentsRectifier, objreg);
    }

    masm.bind(&rejoin);

    // Finally call the function in objreg, as assigned by one of the paths
    // above.
    ensureOsiSpace();
    uint32_t callOffset = masm.callJit(objreg);
    markSafepointAt(callOffset, apply);

    if (apply->mir()->maybeCrossRealm()) {
      static_assert(!JSReturnOperand.aliases(ReturnReg),
                    "ReturnReg available as scratch after scripted calls");
      masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
    }

    // Discard JitFrameLayout fields still left on the stack.
    masm.freeStack(sizeof(JitFrameLayout) -
                   JitFrameLayout::bytesPoppedAfterCall());
    masm.jump(&end);
  }

  // Handle uncompiled or native functions.
  {
    masm.bind(&invoke);
    emitCallInvokeFunction(apply);
  }

  masm.bind(&end);

  // If the return value of the constructing function is Primitive, replace the
  // return value with the Object from CreateThis.
  if (constructing) {
    Label notPrimitive;
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.loadValue(Address(masm.getStackPointer(), 0), JSReturnOperand);

#ifdef DEBUG
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.assumeUnreachable("CreateThis creates an object");
#endif

    masm.bind(&notPrimitive);
  }

  // Pop arguments and continue.
  emitRestoreStackPointerFromFP();
}

template <typename T>
void CodeGenerator::emitAlignStackForApplyNative(T* apply, Register argc) {
  static_assert(JitStackAlignment % ABIStackAlignment == 0,
                "aligning on JIT stack subsumes ABI alignment");

  // Align the arguments on the JitStackAlignment.
  if (JitStackValueAlignment > 1) {
    MOZ_ASSERT(JitStackValueAlignment == 2,
               "Stack padding adds exactly one Value");
    MOZ_ASSERT(frameSize() % JitStackValueAlignment == 0,
               "Stack padding assumes that the frameSize is correct");

    Assembler::Condition cond;
    if constexpr (T::isConstructing()) {
      // If the number of arguments is even, then we do not need any padding.
      //
      // Also see emitAllocateSpaceForApply().
      cond = Assembler::Zero;
    } else {
      // If the number of arguments is odd, then we do not need any padding.
      //
      // Also see emitAllocateSpaceForConstructAndPushNewTarget().
      cond = Assembler::NonZero;
    }

    Label noPaddingNeeded;
    masm.branchTestPtr(cond, argc, Imm32(1), &noPaddingNeeded);
    masm.pushValue(MagicValue(JS_ARG_POISON));
    masm.bind(&noPaddingNeeded);
  }
}

template <typename T>
void CodeGenerator::emitPushNativeArguments(T* apply) {
  Register argc = ToRegister(apply->getArgc());
  Register tmpArgc = ToRegister(apply->getTempObject());
  Register scratch = ToRegister(apply->getTempForArgCopy());
  uint32_t extraFormals = apply->numExtraFormals();

  // Align stack.
  emitAlignStackForApplyNative(apply, argc);

  // Push newTarget.
  if constexpr (T::isConstructing()) {
    masm.pushValue(JSVAL_TYPE_OBJECT, ToRegister(apply->getNewTarget()));
  }

  // Push arguments.
  Label noCopy;
  masm.branchTestPtr(Assembler::Zero, argc, argc, &noCopy);
  {
    // Use scratch register to calculate stack space.
    masm.movePtr(argc, scratch);

    // Reserve space for copying the arguments.
    NativeObject::elementsSizeMustNotOverflow();
    masm.lshiftPtr(Imm32(ValueShift), scratch);
    masm.subFromStackPtr(scratch);

    // Compute the source and destination offsets into the stack.
    Register argvSrcBase = FramePointer;
    size_t argvSrcOffset =
        JitFrameLayout::offsetOfActualArgs() + extraFormals * sizeof(JS::Value);
    size_t argvDstOffset = 0;

    Register argvIndex = tmpArgc;
    masm.move32(argc, argvIndex);

    // Copy arguments.
    emitCopyValuesForApply(argvSrcBase, argvIndex, scratch, argvSrcOffset,
                           argvDstOffset);
  }
  masm.bind(&noCopy);

  // Push |this|.
  if constexpr (T::isConstructing()) {
    masm.pushValue(MagicValue(JS_IS_CONSTRUCTING));
  } else {
    masm.pushValue(ToValue(apply->thisValue()));
  }
}

template <typename T>
void CodeGenerator::emitPushArrayAsNativeArguments(T* apply) {
  Register argc = ToRegister(apply->getArgc());
  Register elements = ToRegister(apply->getElements());
  Register tmpArgc = ToRegister(apply->getTempObject());
  Register scratch = ToRegister(apply->getTempForArgCopy());

  // NB: argc and elements are mapped to the same register.
  MOZ_ASSERT(argc == elements);

  // Invariants guarded in the caller:
  //  - the array is not too long
  //  - the array length equals its initialized length

  // The array length is our argc.
  masm.load32(Address(elements, ObjectElements::offsetOfLength()), tmpArgc);

  // Align stack.
  emitAlignStackForApplyNative(apply, tmpArgc);

  // Push newTarget.
  if constexpr (T::isConstructing()) {
    masm.pushValue(JSVAL_TYPE_OBJECT, ToRegister(apply->getNewTarget()));
  }

  // Skip the copy of arguments if there are none.
  Label noCopy;
  masm.branchTestPtr(Assembler::Zero, tmpArgc, tmpArgc, &noCopy);
  {
    // |tmpArgc| is off-by-one, so adjust the offset accordingly.
    BaseObjectElementIndex srcPtr(elements, tmpArgc,
                                  -int32_t(sizeof(JS::Value)));

    Label loop;
    masm.bind(&loop);
    masm.pushValue(srcPtr, scratch);
    masm.decBranchPtr(Assembler::NonZero, tmpArgc, Imm32(1), &loop);
  }
  masm.bind(&noCopy);

  // Set argc in preparation for calling the native function.
  masm.load32(Address(elements, ObjectElements::offsetOfLength()), argc);

  // Push |this|.
  if constexpr (T::isConstructing()) {
    masm.pushValue(MagicValue(JS_IS_CONSTRUCTING));
  } else {
    masm.pushValue(ToValue(apply->thisValue()));
  }
}

void CodeGenerator::emitPushArguments(LApplyArgsNative* apply) {
  emitPushNativeArguments(apply);
}

void CodeGenerator::emitPushArguments(LApplyArrayNative* apply) {
  emitPushArrayAsNativeArguments(apply);
}

void CodeGenerator::emitPushArguments(LConstructArgsNative* construct) {
  emitPushNativeArguments(construct);
}

void CodeGenerator::emitPushArguments(LConstructArrayNative* construct) {
  emitPushArrayAsNativeArguments(construct);
}

void CodeGenerator::emitPushArguments(LApplyArgsObjNative* apply) {
  Register argc = ToRegister(apply->getArgc());
  Register argsObj = ToRegister(apply->getArgsObj());
  Register tmpArgc = ToRegister(apply->getTempObject());
  Register scratch = ToRegister(apply->getTempForArgCopy());
  Register scratch2 = ToRegister(apply->getTempExtra());

  // NB: argc and argsObj are mapped to the same register.
  MOZ_ASSERT(argc == argsObj);

  // Load argc into tmpArgc.
  masm.loadArgumentsObjectLength(argsObj, tmpArgc);

  // Align stack.
  emitAlignStackForApplyNative(apply, tmpArgc);

  // Push arguments.
  Label noCopy, epilogue;
  masm.branchTestPtr(Assembler::Zero, tmpArgc, tmpArgc, &noCopy);
  {
    // Use scratch register to calculate stack space.
    masm.movePtr(tmpArgc, scratch);

    // Reserve space for copying the arguments.
    NativeObject::elementsSizeMustNotOverflow();
    masm.lshiftPtr(Imm32(ValueShift), scratch);
    masm.subFromStackPtr(scratch);

    // Load arguments data.
    Register argvSrcBase = argsObj;
    masm.loadPrivate(Address(argsObj, ArgumentsObject::getDataSlotOffset()),
                     argvSrcBase);
    size_t argvSrcOffset = ArgumentsData::offsetOfArgs();
    size_t argvDstOffset = 0;

    Register argvIndex = scratch2;
    masm.move32(tmpArgc, argvIndex);

    // Copy the values.
    emitCopyValuesForApply(argvSrcBase, argvIndex, scratch, argvSrcOffset,
                           argvDstOffset);
  }
  masm.bind(&noCopy);

  // Set argc in preparation for calling the native function.
  masm.movePtr(tmpArgc, argc);

  // Push |this|.
  masm.pushValue(ToValue(apply->thisValue()));
}

template <typename T>
void CodeGenerator::emitApplyNative(T* apply) {
  MOZ_ASSERT(T::isConstructing() == apply->mir()->isConstructing(),
             "isConstructing condition must be consistent");

  WrappedFunction* target = apply->mir()->getSingleTarget();
  MOZ_ASSERT(target->isNativeWithoutJitEntry());

  JSNative native = target->native();
  if (apply->mir()->ignoresReturnValue() && target->hasJitInfo()) {
    const JSJitInfo* jitInfo = target->jitInfo();
    if (jitInfo->type() == JSJitInfo::IgnoresReturnValueNative) {
      native = jitInfo->ignoresReturnValueMethod;
    }
  }

  // Push arguments, including newTarget and |this|.
  emitPushArguments(apply);

  // Registers used for callWithABI() argument-passing.
  Register argContextReg = ToRegister(apply->getTempObject());
  Register argUintNReg = ToRegister(apply->getArgc());
  Register argVpReg = ToRegister(apply->getTempForArgCopy());
  Register tempReg = ToRegister(apply->getTempExtra());

  // No unused stack for variadic calls.
  uint32_t unusedStack = 0;

  // Pushed arguments don't change the pushed frames amount.
  MOZ_ASSERT(masm.framePushed() == frameSize());

  // Create the exit frame and call the native.
  emitCallNative(apply, native, argContextReg, argUintNReg, argVpReg, tempReg,
                 unusedStack);

  // The exit frame is still on the stack.
  MOZ_ASSERT(masm.framePushed() == frameSize() + NativeExitFrameLayout::Size());

  // The next instruction is removing the exit frame, so there is no need for
  // leaveFakeExitFrame.

  // Pop arguments and continue.
  masm.setFramePushed(frameSize());
  emitRestoreStackPointerFromFP();
}

template <typename T>
void CodeGenerator::emitApplyArgsGuard(T* apply) {
  LSnapshot* snapshot = apply->snapshot();
  Register argcreg = ToRegister(apply->getArgc());

  // Ensure that we have a reasonable number of arguments.
  bailoutCmp32(Assembler::Above, argcreg, Imm32(JIT_ARGS_LENGTH_MAX), snapshot);
}

template <typename T>
void CodeGenerator::emitApplyArgsObjGuard(T* apply) {
  Register argsObj = ToRegister(apply->getArgsObj());
  Register temp = ToRegister(apply->getTempObject());

  Label bail;
  masm.loadArgumentsObjectLength(argsObj, temp, &bail);
  masm.branch32(Assembler::Above, temp, Imm32(JIT_ARGS_LENGTH_MAX), &bail);
  bailoutFrom(&bail, apply->snapshot());
}

template <typename T>
void CodeGenerator::emitApplyArrayGuard(T* apply) {
  LSnapshot* snapshot = apply->snapshot();
  Register elements = ToRegister(apply->getElements());
  Register tmp = ToRegister(apply->getTempObject());

  Address length(elements, ObjectElements::offsetOfLength());
  masm.load32(length, tmp);

  // Ensure that we have a reasonable number of arguments.
  bailoutCmp32(Assembler::Above, tmp, Imm32(JIT_ARGS_LENGTH_MAX), snapshot);

  // Ensure that the array does not contain an uninitialized tail.

  Address initializedLength(elements,
                            ObjectElements::offsetOfInitializedLength());
  masm.sub32(initializedLength, tmp);
  bailoutCmp32(Assembler::NotEqual, tmp, Imm32(0), snapshot);
}

void CodeGenerator::visitApplyArgsGeneric(LApplyArgsGeneric* apply) {
  emitApplyArgsGuard(apply);
  emitApplyGeneric(apply);
}

void CodeGenerator::visitApplyArgsObj(LApplyArgsObj* apply) {
  emitApplyArgsObjGuard(apply);
  emitApplyGeneric(apply);
}

void CodeGenerator::visitApplyArrayGeneric(LApplyArrayGeneric* apply) {
  emitApplyArrayGuard(apply);
  emitApplyGeneric(apply);
}

void CodeGenerator::visitConstructArgsGeneric(LConstructArgsGeneric* lir) {
  emitApplyArgsGuard(lir);
  emitApplyGeneric(lir);
}

void CodeGenerator::visitConstructArrayGeneric(LConstructArrayGeneric* lir) {
  emitApplyArrayGuard(lir);
  emitApplyGeneric(lir);
}

void CodeGenerator::visitApplyArgsNative(LApplyArgsNative* lir) {
  emitApplyArgsGuard(lir);
  emitApplyNative(lir);
}

void CodeGenerator::visitApplyArgsObjNative(LApplyArgsObjNative* lir) {
  emitApplyArgsObjGuard(lir);
  emitApplyNative(lir);
}

void CodeGenerator::visitApplyArrayNative(LApplyArrayNative* lir) {
  emitApplyArrayGuard(lir);
  emitApplyNative(lir);
}

void CodeGenerator::visitConstructArgsNative(LConstructArgsNative* lir) {
  emitApplyArgsGuard(lir);
  emitApplyNative(lir);
}

void CodeGenerator::visitConstructArrayNative(LConstructArrayNative* lir) {
  emitApplyArrayGuard(lir);
  emitApplyNative(lir);
}

void CodeGenerator::visitBail(LBail* lir) { bailout(lir->snapshot()); }

void CodeGenerator::visitUnreachable(LUnreachable* lir) {
  masm.assumeUnreachable("end-of-block assumed unreachable");
}

void CodeGenerator::visitEncodeSnapshot(LEncodeSnapshot* lir) {
  encode(lir->snapshot());
}

void CodeGenerator::visitUnreachableResultV(LUnreachableResultV* lir) {
  masm.assumeUnreachable("must be unreachable");
}

void CodeGenerator::visitUnreachableResultT(LUnreachableResultT* lir) {
  masm.assumeUnreachable("must be unreachable");
}

void CodeGenerator::visitCheckOverRecursed(LCheckOverRecursed* lir) {
  // If we don't push anything on the stack, skip the check.
  if (omitOverRecursedCheck()) {
    return;
  }

  // Ensure that this frame will not cross the stack limit.
  // This is a weak check, justified by Ion using the C stack: we must always
  // be some distance away from the actual limit, since if the limit is
  // crossed, an error must be thrown, which requires more frames.
  //
  // It must always be possible to trespass past the stack limit.
  // Ion may legally place frames very close to the limit. Calling additional
  // C functions may then violate the limit without any checking.
  //
  // Since Ion frames exist on the C stack, the stack limit may be
  // dynamically set by JS_SetThreadStackLimit() and JS_SetNativeStackQuota().

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    // The OOL path is hit if the recursion depth has been exceeded.
    // Throw an InternalError for over-recursion.

    // LFunctionEnvironment can appear before LCheckOverRecursed, so we have
    // to save all live registers to avoid crashes if CheckOverRecursed triggers
    // a GC.
    saveLive(lir);

    using Fn = bool (*)(JSContext*);
    callVM<Fn, CheckOverRecursed>(lir);

    restoreLive(lir);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  // Conditional forward (unlikely) branch to failure.
  const void* limitAddr = gen->runtime->addressOfJitStackLimit();
  masm.branchStackPtrRhs(Assembler::AboveOrEqual, AbsoluteAddress(limitAddr),
                         ool->entry());
  masm.bind(ool->rejoin());
}

IonScriptCounts* CodeGenerator::maybeCreateScriptCounts() {
  // If scripts are being profiled, create a new IonScriptCounts for the
  // profiling data, which will be attached to the associated JSScript or
  // wasm module after code generation finishes.
  if (!gen->hasProfilingScripts()) {
    return nullptr;
  }

  // This test inhibits IonScriptCount creation for wasm code which is
  // currently incompatible with wasm codegen for two reasons: (1) wasm code
  // must be serializable and script count codegen bakes in absolute
  // addresses, (2) wasm code does not have a JSScript with which to associate
  // code coverage data.
  JSScript* script = gen->outerInfo().script();
  if (!script) {
    return nullptr;
  }

  auto counts = MakeUnique<IonScriptCounts>();
  if (!counts || !counts->init(graph.numBlocks())) {
    return nullptr;
  }

  for (size_t i = 0; i < graph.numBlocks(); i++) {
    MBasicBlock* block = graph.getBlock(i)->mir();

    uint32_t offset = 0;
    char* description = nullptr;
    if (MResumePoint* resume = block->entryResumePoint()) {
      // Find a PC offset in the outermost script to use. If this
      // block is from an inlined script, find a location in the
      // outer script to associate information about the inlining
      // with.
      while (resume->caller()) {
        resume = resume->caller();
      }
      offset = script->pcToOffset(resume->pc());

      if (block->entryResumePoint()->caller()) {
        // Get the filename and line number of the inner script.
        JSScript* innerScript = block->info().script();
        description = js_pod_calloc<char>(200);
        if (description) {
          snprintf(description, 200, "%s:%u", innerScript->filename(),
                   innerScript->lineno());
        }
      }
    }

    if (!counts->block(i).init(block->id(), offset, description,
                               block->numSuccessors())) {
      return nullptr;
    }

    for (size_t j = 0; j < block->numSuccessors(); j++) {
      counts->block(i).setSuccessor(
          j, skipTrivialBlocks(block->getSuccessor(j))->id());
    }
  }

  scriptCounts_ = counts.release();
  return scriptCounts_;
}

// Structure for managing the state tracked for a block by script counters.
struct ScriptCountBlockState {
  IonBlockCounts& block;
  MacroAssembler& masm;

  Sprinter printer;

 public:
  ScriptCountBlockState(IonBlockCounts* block, MacroAssembler* masm)
      : block(*block), masm(*masm), printer(GetJitContext()->cx, false) {}

  bool init() {
    if (!printer.init()) {
      return false;
    }

    // Bump the hit count for the block at the start. This code is not
    // included in either the text for the block or the instruction byte
    // counts.
    masm.inc64(AbsoluteAddress(block.addressOfHitCount()));

    // Collect human readable assembly for the code generated in the block.
    masm.setPrinter(&printer);

    return true;
  }

  void visitInstruction(LInstruction* ins) {
#ifdef JS_JITSPEW
    // Prefix stream of assembly instructions with their LIR instruction
    // name and any associated high level info.
    if (const char* extra = ins->getExtraName()) {
      printer.printf("[%s:%s]\n", ins->opName(), extra);
    } else {
      printer.printf("[%s]\n", ins->opName());
    }
#endif
  }

  ~ScriptCountBlockState() {
    masm.setPrinter(nullptr);

    if (JS::UniqueChars str = printer.release()) {
      block.setCode(str.get());
    }
  }
};

void CodeGenerator::branchIfInvalidated(Register temp, Label* invalidated) {
  CodeOffset label = masm.movWithPatch(ImmWord(uintptr_t(-1)), temp);
  masm.propagateOOM(ionScriptLabels_.append(label));

  // If IonScript::invalidationCount_ != 0, the script has been invalidated.
  masm.branch32(Assembler::NotEqual,
                Address(temp, IonScript::offsetOfInvalidationCount()), Imm32(0),
                invalidated);
}

#ifdef DEBUG
void CodeGenerator::emitAssertGCThingResult(Register input,
                                            const MDefinition* mir) {
  MIRType type = mir->type();
  MOZ_ASSERT(type == MIRType::Object || type == MIRType::String ||
             type == MIRType::Symbol || type == MIRType::BigInt);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(input);

  Register temp = regs.takeAny();
  masm.push(temp);

  // Don't check if the script has been invalidated. In that case invalid
  // types are expected (until we reach the OsiPoint and bailout).
  Label done;
  branchIfInvalidated(temp, &done);

#  ifndef JS_SIMULATOR
  // Check that we have a valid GC pointer.
  // Disable for wasm because we don't have a context on wasm compilation
  // threads and this needs a context.
  // Also disable for simulator builds because the C++ call is a lot slower
  // there than on actual hardware.
  if (JitOptions.fullDebugChecks && !IsCompilingWasm()) {
    saveVolatile();
    masm.setupUnalignedABICall(temp);
    masm.loadJSContext(temp);
    masm.passABIArg(temp);
    masm.passABIArg(input);

    switch (type) {
      case MIRType::Object: {
        using Fn = void (*)(JSContext* cx, JSObject* obj);
        masm.callWithABI<Fn, AssertValidObjectPtr>();
        break;
      }
      case MIRType::String: {
        using Fn = void (*)(JSContext* cx, JSString* str);
        masm.callWithABI<Fn, AssertValidStringPtr>();
        break;
      }
      case MIRType::Symbol: {
        using Fn = void (*)(JSContext* cx, JS::Symbol* sym);
        masm.callWithABI<Fn, AssertValidSymbolPtr>();
        break;
      }
      case MIRType::BigInt: {
        using Fn = void (*)(JSContext* cx, JS::BigInt* bi);
        masm.callWithABI<Fn, AssertValidBigIntPtr>();
        break;
      }
      default:
        MOZ_CRASH();
    }

    restoreVolatile();
  }
#  endif

  masm.bind(&done);
  masm.pop(temp);
}

void CodeGenerator::emitAssertResultV(const ValueOperand input,
                                      const MDefinition* mir) {
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(input);

  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();
  masm.push(temp1);
  masm.push(temp2);

  // Don't check if the script has been invalidated. In that case invalid
  // types are expected (until we reach the OsiPoint and bailout).
  Label done;
  branchIfInvalidated(temp1, &done);

  // Check that we have a valid GC pointer.
  if (JitOptions.fullDebugChecks) {
    saveVolatile();

    masm.pushValue(input);
    masm.moveStackPtrTo(temp1);

    using Fn = void (*)(JSContext* cx, Value* v);
    masm.setupUnalignedABICall(temp2);
    masm.loadJSContext(temp2);
    masm.passABIArg(temp2);
    masm.passABIArg(temp1);
    masm.callWithABI<Fn, AssertValidValue>();
    masm.popValue(input);
    restoreVolatile();
  }

  masm.bind(&done);
  masm.pop(temp2);
  masm.pop(temp1);
}

void CodeGenerator::emitGCThingResultChecks(LInstruction* lir,
                                            MDefinition* mir) {
  if (lir->numDefs() == 0) {
    return;
  }

  MOZ_ASSERT(lir->numDefs() == 1);
  if (lir->getDef(0)->isBogusTemp()) {
    return;
  }

  Register output = ToRegister(lir->getDef(0));
  emitAssertGCThingResult(output, mir);
}

void CodeGenerator::emitValueResultChecks(LInstruction* lir, MDefinition* mir) {
  if (lir->numDefs() == 0) {
    return;
  }

  MOZ_ASSERT(lir->numDefs() == BOX_PIECES);
  if (!lir->getDef(0)->output()->isGeneralReg()) {
    return;
  }

  ValueOperand output = ToOutValue(lir);

  emitAssertResultV(output, mir);
}

void CodeGenerator::emitWasmAnyrefResultChecks(LInstruction* lir,
                                               MDefinition* mir) {
  MOZ_ASSERT(mir->type() == MIRType::WasmAnyRef);

  wasm::MaybeRefType destType = mir->wasmRefType();
  if (!destType) {
    return;
  }

  if (!JitOptions.fullDebugChecks) {
    return;
  }

  if (lir->numDefs() == 0) {
    return;
  }

  MOZ_ASSERT(lir->numDefs() == 1);
  if (lir->getDef(0)->isBogusTemp()) {
    return;
  }

  if (lir->getDef(0)->output()->isMemory()) {
    return;
  }
  Register output = ToRegister(lir->getDef(0));

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(output);

  BranchWasmRefIsSubtypeRegisters needs =
      MacroAssembler::regsForBranchWasmRefIsSubtype(destType.value());

  Register temp1;
  Register temp2;
  Register temp3;
  if (needs.needSuperSTV) {
    temp1 = regs.takeAny();
    masm.push(temp1);
  }
  if (needs.needScratch1) {
    temp2 = regs.takeAny();
    masm.push(temp2);
  }
  if (needs.needScratch2) {
    temp3 = regs.takeAny();
    masm.push(temp3);
  }

  if (needs.needSuperSTV) {
    uint32_t typeIndex =
        wasmCodeMeta()->types->indexOf(*destType.value().typeDef());

    // When full debug checks are enabled, we always write the callee instance
    // pointer into its usual slot in the frame in our function prologue, so
    // that we can get it even if the InstanceReg is currently being used for
    // something else.
    masm.loadPtr(
        Address(FramePointer, wasm::FrameWithInstances::calleeInstanceOffset()),
        temp1);
    masm.loadPtr(
        Address(temp1, wasm::Instance::offsetInData(
                           wasmCodeMeta()->offsetOfSuperTypeVector(typeIndex))),
        temp1);
  }

  Label ok;
  masm.branchWasmRefIsSubtype(output, wasm::MaybeRefType(), destType.value(),
                              &ok, true, temp1, temp2, temp3);
  masm.breakpoint();
  masm.bind(&ok);

  if (needs.needScratch2) {
    masm.pop(temp3);
  }
  if (needs.needScratch1) {
    masm.pop(temp2);
  }
  if (needs.needSuperSTV) {
    masm.pop(temp1);
  }

#  ifdef JS_CODEGEN_ARM64
  masm.syncStackPtr();
#  endif
}

void CodeGenerator::emitDebugResultChecks(LInstruction* ins) {
  // In debug builds, check that LIR instructions return valid values.

  MDefinition* mir = ins->mirRaw();
  if (!mir) {
    return;
  }

  switch (mir->type()) {
    case MIRType::Object:
    case MIRType::String:
    case MIRType::Symbol:
    case MIRType::BigInt:
      emitGCThingResultChecks(ins, mir);
      break;
    case MIRType::Value:
      emitValueResultChecks(ins, mir);
      break;
    case MIRType::WasmAnyRef:
      emitWasmAnyrefResultChecks(ins, mir);
      break;
    default:
      break;
  }
}

void CodeGenerator::emitDebugForceBailing(LInstruction* lir) {
  if (MOZ_LIKELY(!gen->options.ionBailAfterEnabled())) {
    return;
  }
  if (!lir->snapshot()) {
    return;
  }
  if (lir->isOsiPoint()) {
    return;
  }

  masm.comment("emitDebugForceBailing");
  const void* bailAfterCounterAddr =
      gen->runtime->addressOfIonBailAfterCounter();

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());

  Label done, notBail;
  masm.branch32(Assembler::Equal, AbsoluteAddress(bailAfterCounterAddr),
                Imm32(0), &done);
  {
    Register temp = regs.takeAny();

    masm.push(temp);
    masm.load32(AbsoluteAddress(bailAfterCounterAddr), temp);
    masm.sub32(Imm32(1), temp);
    masm.store32(temp, AbsoluteAddress(bailAfterCounterAddr));

    masm.branch32(Assembler::NotEqual, temp, Imm32(0), &notBail);
    {
      masm.pop(temp);
      bailout(lir->snapshot());
    }
    masm.bind(&notBail);
    masm.pop(temp);
  }
  masm.bind(&done);
}
#endif  // DEBUG

bool CodeGenerator::generateBody() {
  JitSpewCont(JitSpew_Codegen, "\n");
  AutoCreatedBy acb(masm, "CodeGenerator::generateBody");

  JitSpew(JitSpew_Codegen, "==== BEGIN CodeGenerator::generateBody ====");
  IonScriptCounts* counts = maybeCreateScriptCounts();

  const bool compilingWasm = gen->compilingWasm();

  for (size_t i = 0; i < graph.numBlocks(); i++) {
    current = graph.getBlock(i);

    // Don't emit any code for trivial blocks, containing just a goto. Such
    // blocks are created to split critical edges, and if we didn't end up
    // putting any instructions in them, we can skip them.
    if (current->isTrivial()) {
      continue;
    }

    if (gen->shouldCancel("Generate Code (block loop)")) {
      return false;
    }

#ifdef JS_JITSPEW
    const char* filename = nullptr;
    size_t lineNumber = 0;
    JS::LimitedColumnNumberOneOrigin columnNumber;
    if (current->mir()->info().script()) {
      filename = current->mir()->info().script()->filename();
      if (current->mir()->pc()) {
        lineNumber = PCToLineNumber(current->mir()->info().script(),
                                    current->mir()->pc(), &columnNumber);
      }
    }
    JitSpew(JitSpew_Codegen, "--------------------------------");
    JitSpew(JitSpew_Codegen, "# block%zu %s:%zu:%u%s:", i,
            filename ? filename : "?", lineNumber,
            columnNumber.oneOriginValue(),
            current->mir()->isLoopHeader() ? " (loop header)" : "");
#endif

    if (current->mir()->isLoopHeader() && compilingWasm) {
      masm.nopAlign(CodeAlignment);
    }

    masm.bind(current->label());

    mozilla::Maybe<ScriptCountBlockState> blockCounts;
    if (counts) {
      blockCounts.emplace(&counts->block(i), &masm);
      if (!blockCounts->init()) {
        return false;
      }
    }

    for (LInstructionIterator iter = current->begin(); iter != current->end();
         iter++) {
      if (gen->shouldCancel("Generate Code (instruction loop)")) {
        return false;
      }
      if (!alloc().ensureBallast()) {
        return false;
      }

      perfSpewer_.recordInstruction(masm, *iter);
#ifdef JS_JITSPEW
      JitSpewStart(JitSpew_Codegen, "                                # LIR=%s",
                   iter->opName());
      if (const char* extra = iter->getExtraName()) {
        JitSpewCont(JitSpew_Codegen, ":%s", extra);
      }
      JitSpewFin(JitSpew_Codegen);
#endif

      if (counts) {
        blockCounts->visitInstruction(*iter);
      }

#ifdef CHECK_OSIPOINT_REGISTERS
      if (iter->safepoint() && !compilingWasm) {
        resetOsiPointRegs(iter->safepoint());
      }
#endif

      if (!compilingWasm) {
        if (MDefinition* mir = iter->mirRaw()) {
          if (!addNativeToBytecodeEntry(mir->trackedSite())) {
            return false;
          }
        }
      }

      setElement(*iter);  // needed to encode correct snapshot location.

#ifdef DEBUG
      emitDebugForceBailing(*iter);
#endif

      switch (iter->op()) {
#ifndef JS_CODEGEN_NONE
#  define LIROP(op)              \
    case LNode::Opcode::op:      \
      visit##op(iter->to##op()); \
      break;
        LIR_OPCODE_LIST(LIROP)
#  undef LIROP
#endif
        case LNode::Opcode::Invalid:
        default:
          MOZ_CRASH("Invalid LIR op");
      }

#ifdef DEBUG
      if (!counts) {
        emitDebugResultChecks(*iter);
      }
#endif
    }
    if (masm.oom()) {
      return false;
    }
  }

  JitSpew(JitSpew_Codegen, "==== END CodeGenerator::generateBody ====\n");
  return true;
}

void CodeGenerator::visitNewArrayCallVM(LNewArray* lir) {
  Register objReg = ToRegister(lir->output());

  MOZ_ASSERT(!lir->isCall());
  saveLive(lir);

  JSObject* templateObject = lir->mir()->templateObject();

  if (templateObject) {
    pushArg(ImmGCPtr(templateObject->shape()));
    pushArg(Imm32(lir->mir()->length()));

    using Fn = ArrayObject* (*)(JSContext*, uint32_t, Handle<Shape*>);
    callVM<Fn, NewArrayWithShape>(lir);
  } else {
    pushArg(Imm32(GenericObject));
    pushArg(Imm32(lir->mir()->length()));

    using Fn = ArrayObject* (*)(JSContext*, uint32_t, NewObjectKind);
    callVM<Fn, NewArrayOperation>(lir);
  }

  masm.storeCallPointerResult(objReg);

  MOZ_ASSERT(!lir->safepoint()->liveRegs().has(objReg));
  restoreLive(lir);
}

void CodeGenerator::visitAtan2D(LAtan2D* lir) {
  FloatRegister y = ToFloatRegister(lir->y());
  FloatRegister x = ToFloatRegister(lir->x());

  using Fn = double (*)(double x, double y);
  masm.setupAlignedABICall();
  masm.passABIArg(y, ABIType::Float64);
  masm.passABIArg(x, ABIType::Float64);
  masm.callWithABI<Fn, ecmaAtan2>(ABIType::Float64);

  MOZ_ASSERT(ToFloatRegister(lir->output()) == ReturnDoubleReg);
}

void CodeGenerator::visitHypot(LHypot* lir) {
  uint32_t numArgs = lir->numArgs();
  masm.setupAlignedABICall();

  for (uint32_t i = 0; i < numArgs; ++i) {
    masm.passABIArg(ToFloatRegister(lir->getOperand(i)), ABIType::Float64);
  }

  switch (numArgs) {
    case 2: {
      using Fn = double (*)(double x, double y);
      masm.callWithABI<Fn, ecmaHypot>(ABIType::Float64);
      break;
    }
    case 3: {
      using Fn = double (*)(double x, double y, double z);
      masm.callWithABI<Fn, hypot3>(ABIType::Float64);
      break;
    }
    case 4: {
      using Fn = double (*)(double x, double y, double z, double w);
      masm.callWithABI<Fn, hypot4>(ABIType::Float64);
      break;
    }
    default:
      MOZ_CRASH("Unexpected number of arguments to hypot function.");
  }
  MOZ_ASSERT(ToFloatRegister(lir->output()) == ReturnDoubleReg);
}

void CodeGenerator::visitNewArray(LNewArray* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());
  DebugOnly<uint32_t> length = lir->mir()->length();

  MOZ_ASSERT(length <= NativeObject::MAX_DENSE_ELEMENTS_COUNT);

  if (lir->mir()->isVMCall()) {
    visitNewArrayCallVM(lir);
    return;
  }

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    visitNewArrayCallVM(lir);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());
  TemplateObject templateObject(lir->mir()->templateObject());
#ifdef DEBUG
  size_t numInlineElements = gc::GetGCKindSlots(templateObject.getAllocKind()) -
                             ObjectElements::VALUES_PER_HEADER;
  MOZ_ASSERT(length <= numInlineElements,
             "Inline allocation only supports inline elements");
#endif
  masm.createGCObject(objReg, tempReg, templateObject,
                      lir->mir()->initialHeap(), ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewArrayDynamicLength(LNewArrayDynamicLength* lir) {
  Register lengthReg = ToRegister(lir->length());
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());

  JSObject* templateObject = lir->mir()->templateObject();
  gc::Heap initialHeap = lir->mir()->initialHeap();

  using Fn = ArrayObject* (*)(JSContext*, Handle<ArrayObject*>, int32_t length,
                              gc::AllocSite*);
  OutOfLineCode* ool = oolCallVM<Fn, ArrayConstructorOneArg>(
      lir, ArgList(ImmGCPtr(templateObject), lengthReg, ImmPtr(nullptr)),
      StoreRegisterTo(objReg));

  bool canInline = true;
  size_t inlineLength = 0;
  if (templateObject->as<ArrayObject>().hasFixedElements()) {
    size_t numSlots =
        gc::GetGCKindSlots(templateObject->asTenured().getAllocKind());
    inlineLength = numSlots - ObjectElements::VALUES_PER_HEADER;
  } else {
    canInline = false;
  }

  if (canInline) {
    // Try to do the allocation inline if the template object is big enough
    // for the length in lengthReg. If the length is bigger we could still
    // use the template object and not allocate the elements, but it's more
    // efficient to do a single big allocation than (repeatedly) reallocating
    // the array later on when filling it.
    masm.branch32(Assembler::Above, lengthReg, Imm32(inlineLength),
                  ool->entry());

    TemplateObject templateObj(templateObject);
    masm.createGCObject(objReg, tempReg, templateObj, initialHeap,
                        ool->entry());

    size_t lengthOffset = NativeObject::offsetOfFixedElements() +
                          ObjectElements::offsetOfLength();
    masm.store32(lengthReg, Address(objReg, lengthOffset));
  } else {
    masm.jump(ool->entry());
  }

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewIterator(LNewIterator* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());

  OutOfLineCode* ool;
  switch (lir->mir()->type()) {
    case MNewIterator::ArrayIterator: {
      using Fn = ArrayIteratorObject* (*)(JSContext*);
      ool = oolCallVM<Fn, NewArrayIterator>(lir, ArgList(),
                                            StoreRegisterTo(objReg));
      break;
    }
    case MNewIterator::StringIterator: {
      using Fn = StringIteratorObject* (*)(JSContext*);
      ool = oolCallVM<Fn, NewStringIterator>(lir, ArgList(),
                                             StoreRegisterTo(objReg));
      break;
    }
    case MNewIterator::RegExpStringIterator: {
      using Fn = RegExpStringIteratorObject* (*)(JSContext*);
      ool = oolCallVM<Fn, NewRegExpStringIterator>(lir, ArgList(),
                                                   StoreRegisterTo(objReg));
      break;
    }
    default:
      MOZ_CRASH("unexpected iterator type");
  }

  TemplateObject templateObject(lir->mir()->templateObject());
  masm.createGCObject(objReg, tempReg, templateObject, gc::Heap::Default,
                      ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewTypedArray(LNewTypedArray* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());
  Register lengthReg = ToRegister(lir->temp1());
  LiveRegisterSet liveRegs = liveVolatileRegs(lir);

  JSObject* templateObject = lir->mir()->templateObject();
  gc::Heap initialHeap = lir->mir()->initialHeap();

  auto* ttemplate = &templateObject->as<FixedLengthTypedArrayObject>();

  size_t n = ttemplate->length();
  MOZ_ASSERT(n <= INT32_MAX,
             "Template objects are only created for int32 lengths");

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, int32_t length);
  OutOfLineCode* ool = oolCallVM<Fn, NewTypedArrayWithTemplateAndLength>(
      lir, ArgList(ImmGCPtr(templateObject), Imm32(n)),
      StoreRegisterTo(objReg));

  TemplateObject templateObj(templateObject);
  masm.createGCObject(objReg, tempReg, templateObj, initialHeap, ool->entry());

  masm.initTypedArraySlots(objReg, tempReg, lengthReg, liveRegs, ool->entry(),
                           ttemplate, MacroAssembler::TypedArrayLength::Fixed);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewTypedArrayDynamicLength(
    LNewTypedArrayDynamicLength* lir) {
  Register lengthReg = ToRegister(lir->length());
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());
  LiveRegisterSet liveRegs = liveVolatileRegs(lir);

  JSObject* templateObject = lir->mir()->templateObject();
  gc::Heap initialHeap = lir->mir()->initialHeap();

  auto* ttemplate = &templateObject->as<FixedLengthTypedArrayObject>();

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, int32_t length);
  OutOfLineCode* ool = oolCallVM<Fn, NewTypedArrayWithTemplateAndLength>(
      lir, ArgList(ImmGCPtr(templateObject), lengthReg),
      StoreRegisterTo(objReg));

  // Volatile |lengthReg| is saved across the ABI call in |initTypedArraySlots|.
  MOZ_ASSERT_IF(lengthReg.volatile_(), liveRegs.has(lengthReg));

  TemplateObject templateObj(templateObject);
  masm.createGCObject(objReg, tempReg, templateObj, initialHeap, ool->entry());

  masm.initTypedArraySlots(objReg, tempReg, lengthReg, liveRegs, ool->entry(),
                           ttemplate,
                           MacroAssembler::TypedArrayLength::Dynamic);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewTypedArrayFromArray(LNewTypedArrayFromArray* lir) {
  pushArg(ToRegister(lir->array()));
  pushArg(ImmGCPtr(lir->mir()->templateObject()));

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, HandleObject);
  callVM<Fn, js::NewTypedArrayWithTemplateAndArray>(lir);
}

void CodeGenerator::visitNewTypedArrayFromArrayBuffer(
    LNewTypedArrayFromArrayBuffer* lir) {
  pushArg(ToValue(lir->length()));
  pushArg(ToValue(lir->byteOffset()));
  pushArg(ToRegister(lir->arrayBuffer()));
  pushArg(ImmGCPtr(lir->mir()->templateObject()));

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, HandleObject,
                                   HandleValue, HandleValue);
  callVM<Fn, js::NewTypedArrayWithTemplateAndBuffer>(lir);
}

void CodeGenerator::visitBindFunction(LBindFunction* lir) {
  Register target = ToRegister(lir->target());
  Register temp1 = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp1());

  // Try to allocate a new BoundFunctionObject we can pass to the VM function.
  // If this fails, we set temp1 to nullptr so we do the allocation in C++.
  TemplateObject templateObject(lir->mir()->templateObject());
  Label allocOk, allocFailed;
  masm.createGCObject(temp1, temp2, templateObject, gc::Heap::Default,
                      &allocFailed);
  masm.jump(&allocOk);

  masm.bind(&allocFailed);
  masm.movePtr(ImmWord(0), temp1);

  masm.bind(&allocOk);

  // Set temp2 to the address of the first argument on the stack.
  // Note that the Value slots used for arguments are currently aligned for a
  // JIT call, even though that's not strictly necessary for calling into C++.
  uint32_t argc = lir->mir()->numStackArgs();
  if (JitStackValueAlignment > 1) {
    argc = AlignBytes(argc, JitStackValueAlignment);
  }
  uint32_t unusedStack = UnusedStackBytesForCall(argc);
  masm.computeEffectiveAddress(Address(masm.getStackPointer(), unusedStack),
                               temp2);

  pushArg(temp1);
  pushArg(Imm32(lir->mir()->numStackArgs()));
  pushArg(temp2);
  pushArg(target);

  using Fn = BoundFunctionObject* (*)(JSContext*, Handle<JSObject*>, Value*,
                                      uint32_t, Handle<BoundFunctionObject*>);
  callVM<Fn, js::BoundFunctionObject::functionBindImpl>(lir);
}

void CodeGenerator::visitNewBoundFunction(LNewBoundFunction* lir) {
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  JSObject* templateObj = lir->mir()->templateObj();

  using Fn = BoundFunctionObject* (*)(JSContext*, Handle<BoundFunctionObject*>);
  OutOfLineCode* ool = oolCallVM<Fn, BoundFunctionObject::createWithTemplate>(
      lir, ArgList(ImmGCPtr(templateObj)), StoreRegisterTo(output));

  TemplateObject templateObject(templateObj);
  masm.createGCObject(output, temp, templateObject, gc::Heap::Default,
                      ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewObjectVMCall(LNewObject* lir) {
  Register objReg = ToRegister(lir->output());

  MOZ_ASSERT(!lir->isCall());
  saveLive(lir);

  JSObject* templateObject = lir->mir()->templateObject();

  // If we're making a new object with a class prototype (that is, an object
  // that derives its class from its prototype instead of being
  // PlainObject::class_'d) from self-hosted code, we need a different init
  // function.
  switch (lir->mir()->mode()) {
    case MNewObject::ObjectLiteral: {
      MOZ_ASSERT(!templateObject);
      pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));
      pushArg(ImmGCPtr(lir->mir()->block()->info().script()));

      using Fn = JSObject* (*)(JSContext*, HandleScript, const jsbytecode* pc);
      callVM<Fn, NewObjectOperation>(lir);
      break;
    }
    case MNewObject::ObjectCreate: {
      pushArg(ImmGCPtr(templateObject));

      using Fn = PlainObject* (*)(JSContext*, Handle<PlainObject*>);
      callVM<Fn, ObjectCreateWithTemplate>(lir);
      break;
    }
  }

  masm.storeCallPointerResult(objReg);

  MOZ_ASSERT(!lir->safepoint()->liveRegs().has(objReg));
  restoreLive(lir);
}

static bool ShouldInitFixedSlots(MIRGenerator* gen, LNewPlainObject* lir,
                                 const Shape* shape, uint32_t nfixed) {
  // Look for StoreFixedSlot instructions following an object allocation
  // that write to this object before a GC is triggered or this object is
  // passed to a VM call. If all fixed slots will be initialized, the
  // allocation code doesn't need to set the slots to |undefined|.

  if (nfixed == 0) {
    return false;
  }

#ifdef DEBUG
  // The bailAfter testing function can trigger a bailout between allocating the
  // object and initializing the slots.
  if (gen->options.ionBailAfterEnabled()) {
    return true;
  }
#endif

  // Keep track of the fixed slots that are initialized. initializedSlots is
  // a bit mask with a bit for each slot.
  MOZ_ASSERT(nfixed <= NativeObject::MAX_FIXED_SLOTS);
  static_assert(NativeObject::MAX_FIXED_SLOTS <= 32,
                "Slot bits must fit in 32 bits");
  uint32_t initializedSlots = 0;
  uint32_t numInitialized = 0;

  MInstruction* allocMir = lir->mir();
  MBasicBlock* block = allocMir->block();

  // Skip the allocation instruction.
  MInstructionIterator iter = block->begin(allocMir);
  MOZ_ASSERT(*iter == allocMir);
  iter++;

  // Handle the leading shape guard, if present.
  for (; iter != block->end(); iter++) {
    if (iter->isConstant()) {
      // This instruction won't trigger a GC or read object slots.
      continue;
    }
    if (iter->isGuardShape()) {
      auto* guard = iter->toGuardShape();
      if (guard->object() != allocMir || guard->shape() != shape) {
        return true;
      }
      allocMir = guard;
      iter++;
    }
    break;
  }

  for (; iter != block->end(); iter++) {
    if (iter->isConstant() || iter->isPostWriteBarrier()) {
      // These instructions won't trigger a GC or read object slots.
      continue;
    }

    if (iter->isStoreFixedSlot()) {
      MStoreFixedSlot* store = iter->toStoreFixedSlot();
      if (store->object() != allocMir) {
        return true;
      }

      // We may not initialize this object slot on allocation, so the
      // pre-barrier could read uninitialized memory. Simply disable
      // the barrier for this store: the object was just initialized
      // so the barrier is not necessary.
      store->setNeedsBarrier(false);

      uint32_t slot = store->slot();
      MOZ_ASSERT(slot < nfixed);
      if ((initializedSlots & (1 << slot)) == 0) {
        numInitialized++;
        initializedSlots |= (1 << slot);

        if (numInitialized == nfixed) {
          // All fixed slots will be initialized.
          MOZ_ASSERT(mozilla::CountPopulation32(initializedSlots) == nfixed);
          return false;
        }
      }
      continue;
    }

    // Unhandled instruction, assume it bails or reads object slots.
    return true;
  }

  MOZ_CRASH("Shouldn't get here");
}

void CodeGenerator::visitNewObject(LNewObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());

  if (lir->mir()->isVMCall()) {
    visitNewObjectVMCall(lir);
    return;
  }

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    visitNewObjectVMCall(lir);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  TemplateObject templateObject(lir->mir()->templateObject());

  masm.createGCObject(objReg, tempReg, templateObject,
                      lir->mir()->initialHeap(), ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewPlainObject(LNewPlainObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register temp0Reg = ToRegister(lir->temp0());
  Register temp1Reg = ToRegister(lir->temp1());
  Register shapeReg = ToRegister(lir->temp2());

  auto* mir = lir->mir();
  const Shape* shape = mir->shape();
  gc::Heap initialHeap = mir->initialHeap();
  gc::AllocKind allocKind = mir->allocKind();

  using Fn =
      JSObject* (*)(JSContext*, Handle<SharedShape*>, gc::AllocKind, gc::Heap);
  OutOfLineCode* ool = oolCallVM<Fn, NewPlainObjectOptimizedFallback>(
      lir,
      ArgList(ImmGCPtr(shape), Imm32(int32_t(allocKind)),
              Imm32(int32_t(initialHeap))),
      StoreRegisterTo(objReg));

  bool initContents =
      ShouldInitFixedSlots(gen, lir, shape, mir->numFixedSlots());

  masm.movePtr(ImmGCPtr(shape), shapeReg);
  masm.createPlainGCObject(
      objReg, shapeReg, temp0Reg, temp1Reg, mir->numFixedSlots(),
      mir->numDynamicSlots(), allocKind, initialHeap, ool->entry(),
      AllocSiteInput(gc::CatchAllAllocSite::Optimized), initContents);

#ifdef DEBUG
  // ShouldInitFixedSlots expects that the leading GuardShape will never fail,
  // so ensure the newly created object has the correct shape. Should the guard
  // ever fail, we may end up with uninitialized fixed slots, which can confuse
  // the GC.
  Label ok;
  masm.branchTestObjShape(Assembler::Equal, objReg, shape, temp0Reg, objReg,
                          &ok);
  masm.assumeUnreachable("Newly created object has the correct shape");
  masm.bind(&ok);
#endif

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewArrayObject(LNewArrayObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register temp0Reg = ToRegister(lir->temp0());
  Register shapeReg = ToRegister(lir->temp1());

  auto* mir = lir->mir();
  uint32_t arrayLength = mir->length();

  gc::AllocKind allocKind = GuessArrayGCKind(arrayLength);
  MOZ_ASSERT(gc::GetObjectFinalizeKind(&ArrayObject::class_) ==
             gc::FinalizeKind::None);
  MOZ_ASSERT(!IsFinalizedKind(allocKind));

  uint32_t slotCount = GetGCKindSlots(allocKind);
  MOZ_ASSERT(slotCount >= ObjectElements::VALUES_PER_HEADER);
  uint32_t arrayCapacity = slotCount - ObjectElements::VALUES_PER_HEADER;

  const Shape* shape = mir->shape();

  NewObjectKind objectKind =
      mir->initialHeap() == gc::Heap::Tenured ? TenuredObject : GenericObject;

  using Fn =
      ArrayObject* (*)(JSContext*, uint32_t, gc::AllocKind, NewObjectKind);
  OutOfLineCode* ool = oolCallVM<Fn, NewArrayObjectOptimizedFallback>(
      lir,
      ArgList(Imm32(arrayLength), Imm32(int32_t(allocKind)), Imm32(objectKind)),
      StoreRegisterTo(objReg));

  masm.movePtr(ImmPtr(shape), shapeReg);
  masm.createArrayWithFixedElements(
      objReg, shapeReg, temp0Reg, InvalidReg, arrayLength, arrayCapacity, 0, 0,
      allocKind, mir->initialHeap(), ool->entry(),
      AllocSiteInput(gc::CatchAllAllocSite::Optimized));
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewNamedLambdaObject(LNewNamedLambdaObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());
  const CompileInfo& info = lir->mir()->block()->info();
  gc::Heap heap = lir->mir()->initialHeap();

  using Fn = js::NamedLambdaObject* (*)(JSContext*, HandleFunction, gc::Heap);
  OutOfLineCode* ool = oolCallVM<Fn, NamedLambdaObject::createWithoutEnclosing>(
      lir, ArgList(info.funMaybeLazy(), Imm32(uint32_t(heap))),
      StoreRegisterTo(objReg));

  TemplateObject templateObject(lir->mir()->templateObj());

  masm.createGCObject(objReg, tempReg, templateObject, heap, ool->entry(),
                      /* initContents = */ true,
                      AllocSiteInput(gc::CatchAllAllocSite::Optimized));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewCallObject(LNewCallObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp0());

  CallObject* templateObj = lir->mir()->templateObject();
  gc::Heap heap = lir->mir()->initialHeap();

  // todo: should get a specialized fallback that passes site
  using Fn = CallObject* (*)(JSContext*, Handle<SharedShape*>, gc::Heap);
  OutOfLineCode* ool = oolCallVM<Fn, CallObject::createWithShape>(
      lir, ArgList(ImmGCPtr(templateObj->sharedShape()), Imm32(uint32_t(heap))),
      StoreRegisterTo(objReg));

  // Inline call object creation, using the OOL path only for tricky cases.
  TemplateObject templateObject(templateObj);

  masm.createGCObject(objReg, tempReg, templateObject, heap, ool->entry(),
                      /* initContents = */ true,
                      AllocSiteInput(gc::CatchAllAllocSite::Optimized));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewMapObject(LNewMapObject* lir) {
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  // Note: pass nullptr for |proto| to use |Map.prototype|.
  using Fn = MapObject* (*)(JSContext*, HandleObject);
  auto* ool = oolCallVM<Fn, MapObject::create>(lir, ArgList(ImmPtr(nullptr)),
                                               StoreRegisterTo(output));

  TemplateObject templateObject(lir->mir()->templateObject());
  masm.createGCObject(output, temp, templateObject, gc::Heap::Default,
                      ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewSetObject(LNewSetObject* lir) {
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  // Note: pass nullptr for |proto| to use |Set.prototype|.
  using Fn = SetObject* (*)(JSContext*, HandleObject);
  auto* ool = oolCallVM<Fn, SetObject::create>(lir, ArgList(ImmPtr(nullptr)),
                                               StoreRegisterTo(output));

  TemplateObject templateObject(lir->mir()->templateObject());
  masm.createGCObject(output, temp, templateObject, gc::Heap::Default,
                      ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewMapObjectFromIterable(
    LNewMapObjectFromIterable* lir) {
  ValueOperand iterable = ToValue(lir->iterable());
  Register output = ToRegister(lir->output());
  Register temp1 = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp1());

  // Allocate a new MapObject. If this fails we pass nullptr for
  // allocatedFromJit.
  Label failedAlloc, vmCall, done;
  TemplateObject templateObject(lir->mir()->templateObject());
  masm.createGCObject(temp1, temp2, templateObject, gc::Heap::Default,
                      &failedAlloc);

  // We're done if |iterable| is null or undefined.
  masm.branchIfNotNullOrUndefined(iterable, &vmCall);
  masm.movePtr(temp1, output);
  masm.jump(&done);

  masm.bind(&failedAlloc);
  masm.movePtr(ImmPtr(nullptr), temp1);

  masm.bind(&vmCall);

  pushArg(temp1);  // allocatedFromJit
  pushArg(iterable);
  pushArg(ImmPtr(nullptr));  // proto

  using Fn = MapObject* (*)(JSContext*, Handle<JSObject*>, Handle<Value>,
                            Handle<MapObject*>);
  callVM<Fn, MapObject::createFromIterable>(lir);

  masm.bind(&done);
}

void CodeGenerator::visitNewSetObjectFromIterable(
    LNewSetObjectFromIterable* lir) {
  ValueOperand iterable = ToValue(lir->iterable());
  Register output = ToRegister(lir->output());
  Register temp1 = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp1());

  // Allocate a new SetObject. If this fails we pass nullptr for
  // allocatedFromJit.
  Label failedAlloc, vmCall, done;
  TemplateObject templateObject(lir->mir()->templateObject());
  masm.createGCObject(temp1, temp2, templateObject, gc::Heap::Default,
                      &failedAlloc);

  // We're done if |iterable| is null or undefined.
  masm.branchIfNotNullOrUndefined(iterable, &vmCall);
  masm.movePtr(temp1, output);
  masm.jump(&done);

  masm.bind(&failedAlloc);
  masm.movePtr(ImmPtr(nullptr), temp1);

  masm.bind(&vmCall);

  pushArg(temp1);  // allocatedFromJit
  pushArg(iterable);
  pushArg(ImmPtr(nullptr));  // proto

  using Fn = SetObject* (*)(JSContext*, Handle<JSObject*>, Handle<Value>,
                            Handle<SetObject*>);
  callVM<Fn, SetObject::createFromIterable>(lir);

  masm.bind(&done);
}

void CodeGenerator::visitNewStringObject(LNewStringObject* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  StringObject* templateObj = lir->mir()->templateObj();

  using Fn = JSObject* (*)(JSContext*, HandleString);
  OutOfLineCode* ool = oolCallVM<Fn, NewStringObject>(lir, ArgList(input),
                                                      StoreRegisterTo(output));

  TemplateObject templateObject(templateObj);
  masm.createGCObject(output, temp, templateObject, gc::Heap::Default,
                      ool->entry());

  masm.loadStringLength(input, temp);

  masm.storeValue(JSVAL_TYPE_STRING, input,
                  Address(output, StringObject::offsetOfPrimitiveValue()));
  masm.storeValue(JSVAL_TYPE_INT32, temp,
                  Address(output, StringObject::offsetOfLength()));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitInitElemGetterSetter(LInitElemGetterSetter* lir) {
  Register obj = ToRegister(lir->object());
  Register value = ToRegister(lir->value());

  pushArg(value);
  pushArg(ToValue(lir->id()));
  pushArg(obj);
  pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));

  using Fn = bool (*)(JSContext*, jsbytecode*, HandleObject, HandleValue,
                      HandleObject);
  callVM<Fn, InitElemGetterSetterOperation>(lir);
}

void CodeGenerator::visitMutateProto(LMutateProto* lir) {
  Register objReg = ToRegister(lir->object());

  pushArg(ToValue(lir->value()));
  pushArg(objReg);

  using Fn =
      bool (*)(JSContext* cx, Handle<PlainObject*> obj, HandleValue value);
  callVM<Fn, MutatePrototype>(lir);
}

void CodeGenerator::visitInitPropGetterSetter(LInitPropGetterSetter* lir) {
  Register obj = ToRegister(lir->object());
  Register value = ToRegister(lir->value());

  pushArg(value);
  pushArg(ImmGCPtr(lir->mir()->name()));
  pushArg(obj);
  pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));

  using Fn = bool (*)(JSContext*, jsbytecode*, HandleObject,
                      Handle<PropertyName*>, HandleObject);
  callVM<Fn, InitPropGetterSetterOperation>(lir);
}

void CodeGenerator::visitCreateThis(LCreateThis* lir) {
  const LAllocation* callee = lir->callee();
  const LAllocation* newTarget = lir->newTarget();

  if (newTarget->isConstant()) {
    pushArg(ImmGCPtr(&newTarget->toConstant()->toObject()));
  } else {
    pushArg(ToRegister(newTarget));
  }

  if (callee->isConstant()) {
    pushArg(ImmGCPtr(&callee->toConstant()->toObject()));
  } else {
    pushArg(ToRegister(callee));
  }

  using Fn = bool (*)(JSContext* cx, HandleObject callee,
                      HandleObject newTarget, MutableHandleValue rval);
  callVM<Fn, jit::CreateThisFromIon>(lir);
}

void CodeGenerator::visitCreateArgumentsObject(LCreateArgumentsObject* lir) {
  // This should be getting constructed in the first block only, and not any OSR
  // entry blocks.
  MOZ_ASSERT(lir->mir()->block()->id() == 0);

  Register callObj = ToRegister(lir->callObject());
  Register temp0 = ToRegister(lir->temp0());
  Label done;

  if (ArgumentsObject* templateObj = lir->mir()->templateObject()) {
    Register objTemp = ToRegister(lir->temp1());
    Register cxTemp = ToRegister(lir->temp2());

    masm.Push(callObj);

    // Try to allocate an arguments object. This will leave the reserved
    // slots uninitialized, so it's important we don't GC until we
    // initialize these slots in ArgumentsObject::finishForIonPure.
    Label failure;
    TemplateObject templateObject(templateObj);
    masm.createGCObject(objTemp, temp0, templateObject, gc::Heap::Default,
                        &failure,
                        /* initContents = */ false);

    masm.moveStackPtrTo(temp0);
    masm.addPtr(Imm32(masm.framePushed()), temp0);

    using Fn = ArgumentsObject* (*)(JSContext* cx, jit::JitFrameLayout* frame,
                                    JSObject* scopeChain, ArgumentsObject* obj);
    masm.setupAlignedABICall();
    masm.loadJSContext(cxTemp);
    masm.passABIArg(cxTemp);
    masm.passABIArg(temp0);
    masm.passABIArg(callObj);
    masm.passABIArg(objTemp);

    masm.callWithABI<Fn, ArgumentsObject::finishForIonPure>();
    masm.branchTestPtr(Assembler::Zero, ReturnReg, ReturnReg, &failure);

    // Discard saved callObj on the stack.
    masm.addToStackPtr(Imm32(sizeof(uintptr_t)));
    masm.jump(&done);

    masm.bind(&failure);
    masm.Pop(callObj);
  }

  masm.moveStackPtrTo(temp0);
  masm.addPtr(Imm32(frameSize()), temp0);

  pushArg(callObj);
  pushArg(temp0);

  using Fn = ArgumentsObject* (*)(JSContext*, JitFrameLayout*, HandleObject);
  callVM<Fn, ArgumentsObject::createForIon>(lir);

  masm.bind(&done);
}

void CodeGenerator::visitCreateInlinedArgumentsObject(
    LCreateInlinedArgumentsObject* lir) {
  Register callObj = ToRegister(lir->getCallObject());
  Register callee = ToRegister(lir->getCallee());
  Register argsAddress = ToRegister(lir->temp1());
  Register argsObj = ToRegister(lir->temp2());

  // TODO: Do we have to worry about alignment here?

  // Create a contiguous array of values for ArgumentsObject::create
  // by pushing the arguments onto the stack in reverse order.
  uint32_t argc = lir->mir()->numActuals();
  for (uint32_t i = 0; i < argc; i++) {
    uint32_t argNum = argc - i - 1;
    uint32_t index = LCreateInlinedArgumentsObject::ArgIndex(argNum);
    ConstantOrRegister arg =
        toConstantOrRegister(lir, index, lir->mir()->getArg(argNum)->type());
    masm.Push(arg);
  }
  masm.moveStackPtrTo(argsAddress);

  Label done;
  if (ArgumentsObject* templateObj = lir->mir()->templateObject()) {
    LiveRegisterSet liveRegs;
    liveRegs.add(callObj);
    liveRegs.add(callee);

    masm.PushRegsInMask(liveRegs);

    // We are free to clobber all registers, as LCreateInlinedArgumentsObject is
    // a call instruction.
    AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
    allRegs.take(callObj);
    allRegs.take(callee);
    allRegs.take(argsObj);
    allRegs.take(argsAddress);

    Register temp3 = allRegs.takeAny();
    Register temp4 = allRegs.takeAny();

    // Try to allocate an arguments object. This will leave the reserved slots
    // uninitialized, so it's important we don't GC until we initialize these
    // slots in ArgumentsObject::finishForIonPure.
    Label failure;
    TemplateObject templateObject(templateObj);
    masm.createGCObject(argsObj, temp3, templateObject, gc::Heap::Default,
                        &failure,
                        /* initContents = */ false);

    Register numActuals = temp3;
    masm.move32(Imm32(argc), numActuals);

    using Fn = ArgumentsObject* (*)(JSContext*, JSObject*, JSFunction*, Value*,
                                    uint32_t, ArgumentsObject*);
    masm.setupAlignedABICall();
    masm.loadJSContext(temp4);
    masm.passABIArg(temp4);
    masm.passABIArg(callObj);
    masm.passABIArg(callee);
    masm.passABIArg(argsAddress);
    masm.passABIArg(numActuals);
    masm.passABIArg(argsObj);

    masm.callWithABI<Fn, ArgumentsObject::finishInlineForIonPure>();
    masm.branchTestPtr(Assembler::Zero, ReturnReg, ReturnReg, &failure);

    // Discard saved callObj, callee, and values array on the stack.
    masm.addToStackPtr(
        Imm32(MacroAssembler::PushRegsInMaskSizeInBytes(liveRegs) +
              argc * sizeof(Value)));
    masm.jump(&done);

    masm.bind(&failure);
    masm.PopRegsInMask(liveRegs);

    // Reload argsAddress because it may have been overridden.
    masm.moveStackPtrTo(argsAddress);
  }

  pushArg(Imm32(argc));
  pushArg(callObj);
  pushArg(callee);
  pushArg(argsAddress);

  using Fn = ArgumentsObject* (*)(JSContext*, Value*, HandleFunction,
                                  HandleObject, uint32_t);
  callVM<Fn, ArgumentsObject::createForInlinedIon>(lir);

  // Discard the array of values.
  masm.freeStack(argc * sizeof(Value));

  masm.bind(&done);
}

template <class GetInlinedArgument>
void CodeGenerator::emitGetInlinedArgument(GetInlinedArgument* lir,
                                           Register index,
                                           ValueOperand output) {
  uint32_t numActuals = lir->mir()->numActuals();
  MOZ_ASSERT(numActuals <= ArgumentsObject::MaxInlinedArgs);

  // The index has already been bounds-checked, so the code we
  // generate here should be unreachable. We can end up in this
  // situation in self-hosted code using GetArgument(), or in a
  // monomorphically inlined function if we've inlined some CacheIR
  // that was created for a different caller.
  if (numActuals == 0) {
    masm.assumeUnreachable("LGetInlinedArgument: invalid index");
    return;
  }

  // Check the first n-1 possible indices.
  Label done;
  for (uint32_t i = 0; i < numActuals - 1; i++) {
    Label skip;
    ConstantOrRegister arg = toConstantOrRegister(
        lir, GetInlinedArgument::ArgIndex(i), lir->mir()->getArg(i)->type());
    masm.branch32(Assembler::NotEqual, index, Imm32(i), &skip);
    masm.moveValue(arg, output);

    masm.jump(&done);
    masm.bind(&skip);
  }

#ifdef DEBUG
  Label skip;
  masm.branch32(Assembler::Equal, index, Imm32(numActuals - 1), &skip);
  masm.assumeUnreachable("LGetInlinedArgument: invalid index");
  masm.bind(&skip);
#endif

  // The index has already been bounds-checked, so load the last argument.
  uint32_t lastIdx = numActuals - 1;
  ConstantOrRegister arg =
      toConstantOrRegister(lir, GetInlinedArgument::ArgIndex(lastIdx),
                           lir->mir()->getArg(lastIdx)->type());
  masm.moveValue(arg, output);
  masm.bind(&done);
}

void CodeGenerator::visitGetInlinedArgument(LGetInlinedArgument* lir) {
  Register index = ToRegister(lir->getIndex());
  ValueOperand output = ToOutValue(lir);

  emitGetInlinedArgument(lir, index, output);
}

void CodeGenerator::visitGetInlinedArgumentHole(LGetInlinedArgumentHole* lir) {
  Register index = ToRegister(lir->getIndex());
  ValueOperand output = ToOutValue(lir);

  uint32_t numActuals = lir->mir()->numActuals();

  if (numActuals == 0) {
    bailoutCmp32(Assembler::LessThan, index, Imm32(0), lir->snapshot());
    masm.moveValue(UndefinedValue(), output);
    return;
  }

  Label outOfBounds, done;
  masm.branch32(Assembler::AboveOrEqual, index, Imm32(numActuals),
                &outOfBounds);

  emitGetInlinedArgument(lir, index, output);
  masm.jump(&done);

  masm.bind(&outOfBounds);
  bailoutCmp32(Assembler::LessThan, index, Imm32(0), lir->snapshot());
  masm.moveValue(UndefinedValue(), output);

  masm.bind(&done);
}

void CodeGenerator::visitGetArgumentsObjectArg(LGetArgumentsObjectArg* lir) {
  Register temp = ToRegister(lir->temp0());
  Register argsObj = ToRegister(lir->argsObject());
  ValueOperand out = ToOutValue(lir);

  masm.loadPrivate(Address(argsObj, ArgumentsObject::getDataSlotOffset()),
                   temp);
  Address argAddr(temp, ArgumentsData::offsetOfArgs() +
                            lir->mir()->argno() * sizeof(Value));
  masm.loadValue(argAddr, out);
#ifdef DEBUG
  Label success;
  masm.branchTestMagic(Assembler::NotEqual, out, &success);
  masm.assumeUnreachable(
      "Result from ArgumentObject shouldn't be JSVAL_TYPE_MAGIC.");
  masm.bind(&success);
#endif
}

void CodeGenerator::visitSetArgumentsObjectArg(LSetArgumentsObjectArg* lir) {
  Register temp = ToRegister(lir->temp0());
  Register argsObj = ToRegister(lir->argsObject());
  ValueOperand value = ToValue(lir->value());

  masm.loadPrivate(Address(argsObj, ArgumentsObject::getDataSlotOffset()),
                   temp);
  Address argAddr(temp, ArgumentsData::offsetOfArgs() +
                            lir->mir()->argno() * sizeof(Value));
  emitPreBarrier(argAddr);
#ifdef DEBUG
  Label success;
  masm.branchTestMagic(Assembler::NotEqual, argAddr, &success);
  masm.assumeUnreachable(
      "Result in ArgumentObject shouldn't be JSVAL_TYPE_MAGIC.");
  masm.bind(&success);
#endif
  masm.storeValue(value, argAddr);
}

void CodeGenerator::visitLoadArgumentsObjectArg(LLoadArgumentsObjectArg* lir) {
  Register temp = ToRegister(lir->temp0());
  Register argsObj = ToRegister(lir->argsObject());
  Register index = ToRegister(lir->index());
  ValueOperand out = ToOutValue(lir);

  Label bail;
  masm.loadArgumentsObjectElement(argsObj, index, out, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitLoadArgumentsObjectArgHole(
    LLoadArgumentsObjectArgHole* lir) {
  Register temp = ToRegister(lir->temp0());
  Register argsObj = ToRegister(lir->argsObject());
  Register index = ToRegister(lir->index());
  ValueOperand out = ToOutValue(lir);

  Label bail;
  masm.loadArgumentsObjectElementHole(argsObj, index, out, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitInArgumentsObjectArg(LInArgumentsObjectArg* lir) {
  Register temp = ToRegister(lir->temp0());
  Register argsObj = ToRegister(lir->argsObject());
  Register index = ToRegister(lir->index());
  Register out = ToRegister(lir->output());

  Label bail;
  masm.loadArgumentsObjectElementExists(argsObj, index, out, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitArgumentsObjectLength(LArgumentsObjectLength* lir) {
  Register argsObj = ToRegister(lir->argsObject());
  Register out = ToRegister(lir->output());

  Label bail;
  masm.loadArgumentsObjectLength(argsObj, out, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitArrayFromArgumentsObject(
    LArrayFromArgumentsObject* lir) {
  pushArg(ToRegister(lir->argsObject()));

  using Fn = ArrayObject* (*)(JSContext*, Handle<ArgumentsObject*>);
  callVM<Fn, js::ArrayFromArgumentsObject>(lir);
}

void CodeGenerator::visitGuardArgumentsObjectFlags(
    LGuardArgumentsObjectFlags* lir) {
  Register argsObj = ToRegister(lir->argsObject());
  Register temp = ToRegister(lir->temp0());

  Label bail;
  masm.branchTestArgumentsObjectFlags(argsObj, temp, lir->mir()->flags(),
                                      Assembler::NonZero, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardObjectHasSameRealm(
    LGuardObjectHasSameRealm* lir) {
  Register obj = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp0());

  Label bail;
  masm.guardObjectHasSameRealm(obj, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitBoundFunctionNumArgs(LBoundFunctionNumArgs* lir) {
  Register obj = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  masm.unboxInt32(Address(obj, BoundFunctionObject::offsetOfFlagsSlot()),
                  output);
  masm.rshift32(Imm32(BoundFunctionObject::NumBoundArgsShift), output);
}

void CodeGenerator::visitGuardBoundFunctionIsConstructor(
    LGuardBoundFunctionIsConstructor* lir) {
  Register obj = ToRegister(lir->object());

  Label bail;
  Address flagsSlot(obj, BoundFunctionObject::offsetOfFlagsSlot());
  masm.branchTest32(Assembler::Zero, flagsSlot,
                    Imm32(BoundFunctionObject::IsConstructorFlag), &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitReturnFromCtor(LReturnFromCtor* lir) {
  ValueOperand value = ToValue(lir->value());
  Register obj = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  Label valueIsObject, end;

  masm.branchTestObject(Assembler::Equal, value, &valueIsObject);

  // Value is not an object. Return that other object.
  masm.movePtr(obj, output);
  masm.jump(&end);

  // Value is an object. Return unbox(Value).
  masm.bind(&valueIsObject);
  Register payload = masm.extractObject(value, output);
  if (payload != output) {
    masm.movePtr(payload, output);
  }

  masm.bind(&end);
}

void CodeGenerator::visitBoxNonStrictThis(LBoxNonStrictThis* lir) {
  ValueOperand value = ToValue(lir->value());
  Register output = ToRegister(lir->output());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Label notNullOrUndefined;
    {
      Label isNullOrUndefined;
      ScratchTagScope tag(masm, value);
      masm.splitTagForTest(value, tag);
      masm.branchTestUndefined(Assembler::Equal, tag, &isNullOrUndefined);
      masm.branchTestNull(Assembler::NotEqual, tag, &notNullOrUndefined);
      masm.bind(&isNullOrUndefined);
      masm.movePtr(ImmGCPtr(lir->mir()->globalThis()), output);
      masm.jump(ool.rejoin());
    }

    masm.bind(&notNullOrUndefined);

    saveLive(lir);

    pushArg(value);
    using Fn = JSObject* (*)(JSContext*, HandleValue);
    callVM<Fn, BoxNonStrictThis>(lir);

    StoreRegisterTo(output).generate(this);
    restoreLiveIgnore(lir, StoreRegisterTo(output).clobbered());

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  masm.fallibleUnboxObject(value, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitImplicitThis(LImplicitThis* lir) {
  Register env = ToRegister(lir->env());
  ValueOperand output = ToOutValue(lir);

  using Fn = void (*)(JSContext*, HandleObject, MutableHandleValue);
  auto* ool = oolCallVM<Fn, ImplicitThisOperation>(lir, ArgList(env),
                                                   StoreValueTo(output));

  masm.computeImplicitThis(env, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitArrayLength(LArrayLength* lir) {
  Register elements = ToRegister(lir->elements());
  Register output = ToRegister(lir->output());

  Address length(elements, ObjectElements::offsetOfLength());
  masm.load32(length, output);

  bool intact = hasSeenArrayExceedsInt32LengthFuseIntactAndDependencyNoted();

  if (intact) {
#ifdef DEBUG
    Label done;
    masm.branchTest32(Assembler::NotSigned, output, output, &done);
    masm.assumeUnreachable("Unexpected array with length > INT32_MAX");
    masm.bind(&done);
#endif
  } else {
    // Bail out if the length doesn't fit in int32.
    bailoutTest32(Assembler::Signed, output, output, lir->snapshot());
  }
}

static void SetLengthFromIndex(MacroAssembler& masm, const LAllocation* index,
                               const Address& length) {
  if (index->isConstant()) {
    masm.store32(Imm32(ToInt32(index) + 1), length);
  } else {
    Register newLength = ToRegister(index);
    masm.add32(Imm32(1), newLength);
    masm.store32(newLength, length);
    masm.sub32(Imm32(1), newLength);
  }
}

void CodeGenerator::visitSetArrayLength(LSetArrayLength* lir) {
  Address length(ToRegister(lir->elements()), ObjectElements::offsetOfLength());
  SetLengthFromIndex(masm, lir->index(), length);
}

void CodeGenerator::visitFunctionLength(LFunctionLength* lir) {
  Register function = ToRegister(lir->function());
  Register output = ToRegister(lir->output());

  Label bail;

  // Get the JSFunction flags.
  masm.load32(Address(function, JSFunction::offsetOfFlagsAndArgCount()),
              output);

  // Functions with a SelfHostedLazyScript must be compiled with the slow-path
  // before the function length is known. If the length was previously resolved,
  // the length property may be shadowed.
  masm.branchTest32(
      Assembler::NonZero, output,
      Imm32(FunctionFlags::SELFHOSTLAZY | FunctionFlags::RESOLVED_LENGTH),
      &bail);

  masm.loadFunctionLength(function, output, output, &bail);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitFunctionName(LFunctionName* lir) {
  Register function = ToRegister(lir->function());
  Register output = ToRegister(lir->output());

  Label bail;

  const JSAtomState& names = gen->runtime->names();
  masm.loadFunctionName(function, output, ImmGCPtr(names.empty_), &bail);

  bailoutFrom(&bail, lir->snapshot());
}

template <class TableObject>
static void TableIteratorLoadEntry(MacroAssembler&, Register, Register,
                                   Register);

template <>
void TableIteratorLoadEntry<MapObject>(MacroAssembler& masm, Register iter,
                                       Register i, Register front) {
  masm.unboxObject(Address(iter, MapIteratorObject::offsetOfTarget()), front);
  masm.loadPrivate(Address(front, MapObject::offsetOfData()), front);

  static_assert(MapObject::Table::offsetOfImplDataElement() == 0,
                "offsetof(Data, element) is 0");
  static_assert(MapObject::Table::sizeofImplData() == 24, "sizeof(Data) is 24");
  masm.mulBy3(i, i);
  masm.lshiftPtr(Imm32(3), i);
  masm.addPtr(i, front);
}

template <>
void TableIteratorLoadEntry<SetObject>(MacroAssembler& masm, Register iter,
                                       Register i, Register front) {
  masm.unboxObject(Address(iter, SetIteratorObject::offsetOfTarget()), front);
  masm.loadPrivate(Address(front, SetObject::offsetOfData()), front);

  static_assert(SetObject::Table::offsetOfImplDataElement() == 0,
                "offsetof(Data, element) is 0");
  static_assert(SetObject::Table::sizeofImplData() == 16, "sizeof(Data) is 16");
  masm.lshiftPtr(Imm32(4), i);
  masm.addPtr(i, front);
}

template <class TableObject>
static void TableIteratorAdvance(MacroAssembler& masm, Register iter,
                                 Register front, Register dataLength,
                                 Register temp) {
  Register i = temp;

  // Note: |count| and |index| are stored as PrivateUint32Value. We use add32
  // and store32 to change the payload.
  masm.add32(Imm32(1), Address(iter, TableIteratorObject::offsetOfCount()));

  masm.unboxInt32(Address(iter, TableIteratorObject::offsetOfIndex()), i);

  Label done, seek;
  masm.bind(&seek);
  masm.add32(Imm32(1), i);
  masm.branch32(Assembler::AboveOrEqual, i, dataLength, &done);

  // We can add sizeof(Data) to |front| to select the next element, because
  // |front| and |mapOrSetObject.data[i]| point to the same location.
  static_assert(TableObject::Table::offsetOfImplDataElement() == 0,
                "offsetof(Data, element) is 0");
  masm.addPtr(Imm32(TableObject::Table::sizeofImplData()), front);

  masm.branchTestMagic(Assembler::Equal,
                       Address(front, TableObject::Table::offsetOfEntryKey()),
                       JS_HASH_KEY_EMPTY, &seek);

  masm.bind(&done);
  masm.store32(i, Address(iter, TableIteratorObject::offsetOfIndex()));
}

// Corresponds to TableIteratorObject::finish.
static void TableIteratorFinish(MacroAssembler& masm, Register iter,
                                Register temp0, Register temp1) {
  Register next = temp0;
  Register prevp = temp1;
  masm.loadPrivate(Address(iter, TableIteratorObject::offsetOfNext()), next);
  masm.loadPrivate(Address(iter, TableIteratorObject::offsetOfPrevPtr()),
                   prevp);
  masm.storePtr(next, Address(prevp, 0));

  Label hasNoNext;
  masm.branchTestPtr(Assembler::Zero, next, next, &hasNoNext);
  masm.storePrivateValue(prevp,
                         Address(next, TableIteratorObject::offsetOfPrevPtr()));
  masm.bind(&hasNoNext);

  // Mark iterator inactive.
  Address targetAddr(iter, TableIteratorObject::offsetOfTarget());
  masm.guardedCallPreBarrier(targetAddr, MIRType::Value);
  masm.storeValue(UndefinedValue(), targetAddr);
}

template <>
void CodeGenerator::emitLoadIteratorValues<MapObject>(Register result,
                                                      Register temp,
                                                      Register front) {
  size_t elementsOffset = NativeObject::offsetOfFixedElements();

  Address keyAddress(front, MapObject::Table::Entry::offsetOfKey());
  Address valueAddress(front, MapObject::Table::Entry::offsetOfValue());
  Address keyElemAddress(result, elementsOffset);
  Address valueElemAddress(result, elementsOffset + sizeof(Value));
  masm.guardedCallPreBarrier(keyElemAddress, MIRType::Value);
  masm.guardedCallPreBarrier(valueElemAddress, MIRType::Value);
  masm.storeValue(keyAddress, keyElemAddress, temp);
  masm.storeValue(valueAddress, valueElemAddress, temp);

  Label emitBarrier, skipBarrier;
  masm.branchValueIsNurseryCell(Assembler::Equal, keyAddress, temp,
                                &emitBarrier);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, valueAddress, temp,
                                &skipBarrier);
  {
    masm.bind(&emitBarrier);
    saveVolatile(temp);
    emitPostWriteBarrier(result);
    restoreVolatile(temp);
  }
  masm.bind(&skipBarrier);
}

template <>
void CodeGenerator::emitLoadIteratorValues<SetObject>(Register result,
                                                      Register temp,
                                                      Register front) {
  size_t elementsOffset = NativeObject::offsetOfFixedElements();

  Address keyAddress(front, SetObject::Table::offsetOfEntryKey());
  Address keyElemAddress(result, elementsOffset);
  masm.guardedCallPreBarrier(keyElemAddress, MIRType::Value);
  masm.storeValue(keyAddress, keyElemAddress, temp);

  Label skipBarrier;
  masm.branchValueIsNurseryCell(Assembler::NotEqual, keyAddress, temp,
                                &skipBarrier);
  {
    saveVolatile(temp);
    emitPostWriteBarrier(result);
    restoreVolatile(temp);
  }
  masm.bind(&skipBarrier);
}

template <class IteratorObject, class TableObject>
void CodeGenerator::emitGetNextEntryForIterator(LGetNextEntryForIterator* lir) {
  Register iter = ToRegister(lir->iter());
  Register result = ToRegister(lir->result());
  Register temp = ToRegister(lir->temp0());
  Register dataLength = ToRegister(lir->temp1());
  Register front = ToRegister(lir->temp2());
  Register output = ToRegister(lir->output());

#ifdef DEBUG
  // Self-hosted code is responsible for ensuring GetNextEntryForIterator is
  // only called with the correct iterator class. Assert here all self-
  // hosted callers of GetNextEntryForIterator perform this class check.
  // No Spectre mitigations are needed because this is DEBUG-only code.
  Label success;
  masm.branchTestObjClassNoSpectreMitigations(
      Assembler::Equal, iter, &IteratorObject::class_, temp, &success);
  masm.assumeUnreachable("Iterator object should have the correct class.");
  masm.bind(&success);
#endif

  // If the iterator has no target, it's already done.
  // See TableIteratorObject::isActive.
  Label iterAlreadyDone, iterDone, done;
  masm.branchTestUndefined(Assembler::Equal,
                           Address(iter, IteratorObject::offsetOfTarget()),
                           &iterAlreadyDone);

  // Load |iter->index| in |temp| and |iter->target->dataLength| in
  // |dataLength|. Both values are stored as PrivateUint32Value.
  masm.unboxInt32(Address(iter, IteratorObject::offsetOfIndex()), temp);
  masm.unboxObject(Address(iter, IteratorObject::offsetOfTarget()), dataLength);
  masm.unboxInt32(Address(dataLength, TableObject::offsetOfDataLength()),
                  dataLength);
  masm.branch32(Assembler::AboveOrEqual, temp, dataLength, &iterDone);
  {
    TableIteratorLoadEntry<TableObject>(masm, iter, temp, front);

    emitLoadIteratorValues<TableObject>(result, temp, front);

    TableIteratorAdvance<TableObject>(masm, iter, front, dataLength, temp);

    masm.move32(Imm32(0), output);
    masm.jump(&done);
  }
  {
    masm.bind(&iterDone);
    TableIteratorFinish(masm, iter, temp, dataLength);

    masm.bind(&iterAlreadyDone);
    masm.move32(Imm32(1), output);
  }
  masm.bind(&done);
}

void CodeGenerator::visitGetNextEntryForIterator(
    LGetNextEntryForIterator* lir) {
  if (lir->mir()->mode() == MGetNextEntryForIterator::Map) {
    emitGetNextEntryForIterator<MapIteratorObject, MapObject>(lir);
  } else {
    MOZ_ASSERT(lir->mir()->mode() == MGetNextEntryForIterator::Set);
    emitGetNextEntryForIterator<SetIteratorObject, SetObject>(lir);
  }
}

// The point of these is to inform Ion of where these values already are; they
// don't normally generate (much) code.
void CodeGenerator::visitWasmRegisterPairResult(LWasmRegisterPairResult* lir) {}
void CodeGenerator::visitWasmStackResult(LWasmStackResult* lir) {}
void CodeGenerator::visitWasmStackResult64(LWasmStackResult64* lir) {}

void CodeGenerator::visitWasmStackResultArea(LWasmStackResultArea* lir) {
  LAllocation* output = lir->getDef(0)->output();
  MOZ_ASSERT(output->isStackArea());
  bool tempInit = false;
  for (auto iter = output->toStackArea()->results(); iter; iter.next()) {
    // Zero out ref stack results.
    if (iter.isWasmAnyRef()) {
      Register temp = ToRegister(lir->temp0());
      if (!tempInit) {
        masm.xorPtr(temp, temp);
        tempInit = true;
      }
      masm.storePtr(temp, ToAddress(iter.alloc()));
    }
  }
}

void CodeGenerator::visitWasmRegisterResult(LWasmRegisterResult* lir) {
#ifdef JS_64BIT
  if (MWasmRegisterResult* mir = lir->mir()) {
    if (mir->type() == MIRType::Int32) {
      masm.widenInt32(ToRegister(lir->output()));
    }
  }
#endif
}

void CodeGenerator::visitWasmCall(LWasmCall* lir) {
  const MWasmCallBase* callBase = lir->callBase();
  bool isReturnCall = lir->isReturnCall();

  // If this call is in Wasm try code block, initialise a wasm::TryNote for this
  // call.
  bool inTry = callBase->inTry();
  if (inTry) {
    size_t tryNoteIndex = callBase->tryNoteIndex();
    wasm::TryNoteVector& tryNotes = masm.tryNotes();
    wasm::TryNote& tryNote = tryNotes[tryNoteIndex];
    tryNote.setTryBodyBegin(masm.currentOffset());
  }

  MOZ_ASSERT((sizeof(wasm::Frame) + masm.framePushed()) % WasmStackAlignment ==
             0);
  static_assert(
      WasmStackAlignment >= ABIStackAlignment &&
          WasmStackAlignment % ABIStackAlignment == 0,
      "The wasm stack alignment should subsume the ABI-required alignment");

#ifdef DEBUG
  Label ok;
  masm.branchTestStackPtr(Assembler::Zero, Imm32(WasmStackAlignment - 1), &ok);
  masm.breakpoint();
  masm.bind(&ok);
#endif

  // LWasmCallBase::isCallPreserved() assumes that all MWasmCalls preserve the
  // instance and pinned regs. The only case where where we don't have to
  // reload the instance and pinned regs is when the callee preserves them.
  bool reloadRegs = true;
  bool switchRealm = true;

  const wasm::CallSiteDesc& desc = callBase->desc();
  const wasm::CalleeDesc& callee = callBase->callee();
  CodeOffset retOffset;
  CodeOffset secondRetOffset;
  switch (callee.which()) {
    case wasm::CalleeDesc::Func:
      if (isReturnCall) {
        ReturnCallAdjustmentInfo retCallInfo(
            callBase->stackArgAreaSizeUnaligned(), inboundStackArgBytes_);
        masm.wasmReturnCall(desc, callee.funcIndex(), retCallInfo);
        // The rest of the method is unnecessary for a return call.
        return;
      }
      MOZ_ASSERT(!isReturnCall);
      retOffset = masm.call(desc, callee.funcIndex());
      reloadRegs = false;
      switchRealm = false;
      break;
    case wasm::CalleeDesc::Import:
      if (isReturnCall) {
        ReturnCallAdjustmentInfo retCallInfo(
            callBase->stackArgAreaSizeUnaligned(), inboundStackArgBytes_);
        masm.wasmReturnCallImport(desc, callee, retCallInfo);
        // The rest of the method is unnecessary for a return call.
        return;
      }
      MOZ_ASSERT(!isReturnCall);
      retOffset = masm.wasmCallImport(desc, callee);
      break;
    case wasm::CalleeDesc::AsmJSTable:
      retOffset = masm.asmCallIndirect(desc, callee);
      break;
    case wasm::CalleeDesc::WasmTable: {
      Label* boundsCheckFailed = nullptr;
      if (lir->needsBoundsCheck()) {
        auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
          masm.wasmTrap(wasm::Trap::OutOfBounds, desc.toTrapSiteDesc());
        });
        if (lir->isCatchable()) {
          addOutOfLineCode(ool, lir->mirCatchable());
        } else if (isReturnCall) {
          addOutOfLineCode(ool, lir->mirReturnCall());
        } else {
          addOutOfLineCode(ool, lir->mirUncatchable());
        }
        boundsCheckFailed = ool->entry();
      }
      Label* nullCheckFailed = nullptr;
#ifndef WASM_HAS_HEAPREG
      {
        auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
          masm.wasmTrap(wasm::Trap::IndirectCallToNull, desc.toTrapSiteDesc());
        });
        if (lir->isCatchable()) {
          addOutOfLineCode(ool, lir->mirCatchable());
        } else if (isReturnCall) {
          addOutOfLineCode(ool, lir->mirReturnCall());
        } else {
          addOutOfLineCode(ool, lir->mirUncatchable());
        }
        nullCheckFailed = ool->entry();
      }
#endif
      if (isReturnCall) {
        ReturnCallAdjustmentInfo retCallInfo(
            callBase->stackArgAreaSizeUnaligned(), inboundStackArgBytes_);
        masm.wasmReturnCallIndirect(desc, callee, boundsCheckFailed,
                                    nullCheckFailed, mozilla::Nothing(),
                                    retCallInfo);
        // The rest of the method is unnecessary for a return call.
        return;
      }
      MOZ_ASSERT(!isReturnCall);
      masm.wasmCallIndirect(desc, callee, boundsCheckFailed, nullCheckFailed,
                            lir->tableSize(), &retOffset, &secondRetOffset);
      // Register reloading and realm switching are handled dynamically inside
      // wasmCallIndirect.  There are two return offsets, one for each call
      // instruction (fast path and slow path).
      reloadRegs = false;
      switchRealm = false;
      break;
    }
    case wasm::CalleeDesc::Builtin:
      retOffset = masm.call(desc, callee.builtin());
      reloadRegs = false;
      switchRealm = false;
      break;
    case wasm::CalleeDesc::BuiltinInstanceMethod:
      retOffset = masm.wasmCallBuiltinInstanceMethod(
          desc, callBase->instanceArg(), callee.builtin(),
          callBase->builtinMethodFailureMode());
      switchRealm = false;
      break;
    case wasm::CalleeDesc::FuncRef:
      if (isReturnCall) {
        ReturnCallAdjustmentInfo retCallInfo(
            callBase->stackArgAreaSizeUnaligned(), inboundStackArgBytes_);
        masm.wasmReturnCallRef(desc, callee, retCallInfo);
        // The rest of the method is unnecessary for a return call.
        return;
      }
      MOZ_ASSERT(!isReturnCall);
      // Register reloading and realm switching are handled dynamically inside
      // wasmCallRef.  There are two return offsets, one for each call
      // instruction (fast path and slow path).
      masm.wasmCallRef(desc, callee, &retOffset, &secondRetOffset);
      reloadRegs = false;
      switchRealm = false;
      break;
  }

  // Note the assembler offset for the associated LSafePoint.
  MOZ_ASSERT(!isReturnCall);
  markSafepointAt(retOffset.offset(), lir);

  // Now that all the outbound in-memory args are on the stack, note the
  // required lower boundary point of the associated StackMap.
  uint32_t framePushedAtStackMapBase =
      masm.framePushed() -
      wasm::AlignStackArgAreaSize(callBase->stackArgAreaSizeUnaligned());
  lir->safepoint()->setFramePushedAtStackMapBase(framePushedAtStackMapBase);
  MOZ_ASSERT(lir->safepoint()->wasmSafepointKind() ==
             WasmSafepointKind::LirCall);

  // Note the assembler offset and framePushed for use by the adjunct
  // LSafePoint, see visitor for LWasmCallIndirectAdjunctSafepoint below.
  if (callee.which() == wasm::CalleeDesc::WasmTable ||
      callee.which() == wasm::CalleeDesc::FuncRef) {
    lir->adjunctSafepoint()->recordSafepointInfo(secondRetOffset,
                                                 framePushedAtStackMapBase);
  }

  if (reloadRegs) {
    masm.loadPtr(
        Address(masm.getStackPointer(), WasmCallerInstanceOffsetBeforeCall),
        InstanceReg);
    masm.loadWasmPinnedRegsFromInstance(mozilla::Nothing());
    if (switchRealm) {
      masm.switchToWasmInstanceRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
    }
  } else {
    MOZ_ASSERT(!switchRealm);
  }

  switch (callee.which()) {
    case wasm::CalleeDesc::Func:
    case wasm::CalleeDesc::Import:
    case wasm::CalleeDesc::WasmTable:
    case wasm::CalleeDesc::FuncRef:
      // Stack allocation could change during Wasm (return) calls,
      // recover pre-call state.
      masm.freeStackTo(masm.framePushed());
      break;
    default:
      break;
  }

  if (inTry) {
    // Set the end of the try note range
    size_t tryNoteIndex = callBase->tryNoteIndex();
    wasm::TryNoteVector& tryNotes = masm.tryNotes();
    wasm::TryNote& tryNote = tryNotes[tryNoteIndex];

    // Don't set the end of the try note if we've OOM'ed, as the above
    // instructions may not have been emitted, which will trigger an assert
    // about zero-length try-notes. This is okay as this compilation will be
    // thrown away.
    if (!masm.oom()) {
      tryNote.setTryBodyEnd(masm.currentOffset());
    }

    // This instruction or the adjunct safepoint must be the last instruction
    // in the block. No other instructions may be inserted.
    LBlock* block = lir->block();
    MOZ_RELEASE_ASSERT(*block->rbegin() == lir ||
                       (block->rbegin()->isWasmCallIndirectAdjunctSafepoint() &&
                        *(++block->rbegin()) == lir));

    // Jump to the fallthrough block
    jumpToBlock(lir->mirCatchable()->getSuccessor(
        MWasmCallCatchable::FallthroughBranchIndex));
  }
}

#ifdef ENABLE_WASM_JSPI
void CodeGenerator::callWasmUpdateSuspenderState(
    wasm::UpdateSuspenderStateAction kind, Register suspender, Register temp) {
  masm.Push(InstanceReg);
  int32_t framePushedAfterInstance = masm.framePushed();

  masm.move32(Imm32(uint32_t(kind)), temp);

  masm.setupWasmABICall();
  masm.passABIArg(InstanceReg);
  masm.passABIArg(suspender);
  masm.passABIArg(temp);
  int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
  masm.callWithABI(wasm::BytecodeOffset(0),
                   wasm::SymbolicAddress::UpdateSuspenderState,
                   mozilla::Some(instanceOffset));

  masm.Pop(InstanceReg);
}

void CodeGenerator::prepareWasmStackSwitchTrampolineCall(Register suspender,
                                                         Register data) {
  // Reserve stack space for the wasm call.
  unsigned argDecrement;
  {
    WasmABIArgGenerator abi;
    ABIArg arg;
    arg = abi.next(MIRType::Pointer);
    arg = abi.next(MIRType::Pointer);
    argDecrement = StackDecrementForCall(WasmStackAlignment, 0,
                                         abi.stackBytesConsumedSoFar());
  }
  masm.reserveStack(argDecrement);

  // Pass the suspender and data params through the wasm function ABI registers.
  WasmABIArgGenerator abi;
  ABIArg arg;
  arg = abi.next(MIRType::Pointer);
  if (arg.kind() == ABIArg::GPR) {
    masm.movePtr(suspender, arg.gpr());
  } else {
    MOZ_ASSERT(arg.kind() == ABIArg::Stack);
    masm.storePtr(suspender,
                  Address(masm.getStackPointer(), arg.offsetFromArgBase()));
  }
  arg = abi.next(MIRType::Pointer);
  if (arg.kind() == ABIArg::GPR) {
    masm.movePtr(data, arg.gpr());
  } else {
    MOZ_ASSERT(arg.kind() == ABIArg::Stack);
    masm.storePtr(data,
                  Address(masm.getStackPointer(), arg.offsetFromArgBase()));
  }

  masm.storePtr(InstanceReg, Address(masm.getStackPointer(),
                                     WasmCallerInstanceOffsetBeforeCall));
}
#endif  // ENABLE_WASM_JSPI

void CodeGenerator::visitWasmStackSwitchToSuspendable(
    LWasmStackSwitchToSuspendable* lir) {
#ifdef ENABLE_WASM_JSPI
  const Register SuspenderReg = lir->suspender()->toGeneralReg()->reg();
  const Register FnReg = lir->fn()->toGeneralReg()->reg();
  const Register DataReg = lir->data()->toGeneralReg()->reg();
  const Register SuspenderDataReg = ABINonArgReg3;

#  ifdef JS_CODEGEN_ARM64
  vixl::UseScratchRegisterScope temps(&masm);
  const Register ScratchReg1 = temps.AcquireX().asUnsized();
#  elif defined(JS_CODEGEN_X86)
  const Register ScratchReg1 = ABINonArgReg3;
#  elif defined(JS_CODEGEN_X64)
  const Register ScratchReg1 = ScratchReg;
#  elif defined(JS_CODEGEN_ARM)
  const Register ScratchReg1 = ABINonArgReturnVolatileReg;
#  elif defined(JS_CODEGEN_LOONG64)
  SecondScratchRegisterScope scratch2(masm);
  const Register ScratchReg1 = scratch2;
#  else
#    error "NYI: scratch register"
#  endif

  masm.Push(SuspenderReg);
  masm.Push(FnReg);
  masm.Push(DataReg);

  callWasmUpdateSuspenderState(wasm::UpdateSuspenderStateAction::Enter,
                               SuspenderReg, ScratchReg1);
  masm.Pop(DataReg);
  masm.Pop(FnReg);
  masm.Pop(SuspenderReg);

  masm.Push(SuspenderReg);
  int32_t framePushedAtSuspender = masm.framePushed();
  masm.Push(InstanceReg);

  wasm::CallSiteDesc desc(wasm::CallSiteKind::StackSwitch);
  CodeLabel returnCallsite;

  // Aligning stack before trampoline call.
  uint32_t reserve = ComputeByteAlignment(
      masm.framePushed() - sizeof(wasm::Frame), WasmStackAlignment);
  masm.reserveStack(reserve);

  masm.loadPrivate(Address(SuspenderReg, NativeObject::getFixedSlotOffset(
                                             wasm::SuspenderObjectDataSlot)),
                   SuspenderDataReg);

  // Switch stacks to suspendable, keep original FP to maintain
  // frames chain between main and suspendable stack segments.
  masm.storeStackPtr(
      Address(SuspenderDataReg, wasm::SuspenderObjectData::offsetOfMainSP()));
  masm.storePtr(
      FramePointer,
      Address(SuspenderDataReg, wasm::SuspenderObjectData::offsetOfMainFP()));

  masm.loadStackPtr(Address(
      SuspenderDataReg, wasm::SuspenderObjectData::offsetOfSuspendableSP()));

  masm.assertStackAlignment(WasmStackAlignment);

  // The FramePointer is not changed for SwitchToSuspendable.
  uint32_t framePushed = masm.framePushed();

  // On different stack, reset framePushed. FramePointer is not valid here.
  masm.setFramePushed(0);

  prepareWasmStackSwitchTrampolineCall(SuspenderReg, DataReg);

  // Get wasm instance pointer for callee.
  size_t instanceSlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_INSTANCE_SLOT);
  masm.loadPtr(Address(FnReg, instanceSlotOffset), InstanceReg);

  masm.storePtr(InstanceReg, Address(masm.getStackPointer(),
                                     WasmCalleeInstanceOffsetBeforeCall));
  masm.loadWasmPinnedRegsFromInstance(mozilla::Nothing());

  masm.assertStackAlignment(WasmStackAlignment);

  const Register ReturnAddressReg = ScratchReg1;

  // DataReg is not needed anymore, using it as a scratch register.
  const Register ScratchReg2 = DataReg;

  // Save future of suspendable stack exit frame pointer.
  masm.computeEffectiveAddress(
      Address(masm.getStackPointer(), -int32_t(sizeof(wasm::Frame))),
      ScratchReg2);
  masm.storePtr(
      ScratchReg2,
      Address(SuspenderDataReg,
              wasm::SuspenderObjectData::offsetOfSuspendableExitFP()));

  masm.mov(&returnCallsite, ReturnAddressReg);

  // Call wasm function fast.
#  ifdef JS_USE_LINK_REGISTER
#    if defined(JS_CODEGEN_LOONG64)
  masm.mov(ReturnAddressReg, ra);
#    else
  masm.mov(ReturnAddressReg, lr);
#    endif
#  else
  masm.Push(ReturnAddressReg);
#  endif
  // Get funcUncheckedCallEntry() from the function's
  // WASM_FUNC_UNCHECKED_ENTRY_SLOT extended slot.
  size_t uncheckedEntrySlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_FUNC_UNCHECKED_ENTRY_SLOT);
  masm.loadPtr(Address(FnReg, uncheckedEntrySlotOffset), ScratchReg2);
  masm.jump(ScratchReg2);

  // About to use valid FramePointer -- restore framePushed.
  masm.setFramePushed(framePushed);

  // For IsPlausibleStackMapKey check for the following callsite.
  masm.wasmTrapInstruction();

  // Callsite for return from main stack.
  masm.bind(&returnCallsite);
  masm.append(desc, *returnCallsite.target());
  masm.addCodeLabel(returnCallsite);

  masm.assertStackAlignment(WasmStackAlignment);

  markSafepointAt(returnCallsite.target()->offset(), lir);
  lir->safepoint()->setFramePushedAtStackMapBase(framePushed);
  lir->safepoint()->setWasmSafepointKind(WasmSafepointKind::StackSwitch);
  // Rooting SuspenderReg.
  masm.propagateOOM(
      lir->safepoint()->addWasmAnyRefSlot(true, framePushedAtSuspender));

  masm.freeStackTo(framePushed);

  masm.freeStack(reserve);
  masm.Pop(InstanceReg);
  masm.Pop(SuspenderReg);

  masm.switchToWasmInstanceRealm(ScratchReg1, ScratchReg2);

  callWasmUpdateSuspenderState(wasm::UpdateSuspenderStateAction::Leave,
                               SuspenderReg, ScratchReg1);
#else
  MOZ_CRASH("NYI");
#endif  // ENABLE_WASM_JSPI
}

void CodeGenerator::visitWasmStackSwitchToMain(LWasmStackSwitchToMain* lir) {
#ifdef ENABLE_WASM_JSPI
  const Register SuspenderReg = lir->suspender()->toGeneralReg()->reg();
  const Register FnReg = lir->fn()->toGeneralReg()->reg();
  const Register DataReg = lir->data()->toGeneralReg()->reg();
  const Register SuspenderDataReg = ABINonArgReg3;

#  ifdef JS_CODEGEN_ARM64
  vixl::UseScratchRegisterScope temps(&masm);
  const Register ScratchReg1 = temps.AcquireX().asUnsized();
#  elif defined(JS_CODEGEN_X86)
  const Register ScratchReg1 = ABINonArgReg3;
#  elif defined(JS_CODEGEN_X64)
  const Register ScratchReg1 = ScratchReg;
#  elif defined(JS_CODEGEN_ARM)
  const Register ScratchReg1 = ABINonArgReturnVolatileReg;
#  elif defined(JS_CODEGEN_LOONG64)
  SecondScratchRegisterScope scratch2(masm);
  const Register ScratchReg1 = scratch2;
#  else
#    error "NYI: scratch register"
#  endif

  masm.Push(SuspenderReg);
  masm.Push(FnReg);
  masm.Push(DataReg);

  callWasmUpdateSuspenderState(wasm::UpdateSuspenderStateAction::Suspend,
                               SuspenderReg, ScratchReg1);

  masm.Pop(DataReg);
  masm.Pop(FnReg);
  masm.Pop(SuspenderReg);

  masm.Push(SuspenderReg);
  int32_t framePushedAtSuspender = masm.framePushed();
  masm.Push(InstanceReg);

  wasm::CallSiteDesc desc(wasm::CallSiteKind::StackSwitch);
  CodeLabel returnCallsite;

  // Aligning stack before trampoline call.
  uint32_t reserve = ComputeByteAlignment(
      masm.framePushed() - sizeof(wasm::Frame), WasmStackAlignment);
  masm.reserveStack(reserve);

  masm.loadPrivate(Address(SuspenderReg, NativeObject::getFixedSlotOffset(
                                             wasm::SuspenderObjectDataSlot)),
                   SuspenderDataReg);

  // Switch stacks to main.
  masm.storeStackPtr(Address(
      SuspenderDataReg, wasm::SuspenderObjectData::offsetOfSuspendableSP()));
  masm.storePtr(FramePointer,
                Address(SuspenderDataReg,
                        wasm::SuspenderObjectData::offsetOfSuspendableFP()));

  masm.loadStackPtr(
      Address(SuspenderDataReg, wasm::SuspenderObjectData::offsetOfMainSP()));
  masm.loadPtr(
      Address(SuspenderDataReg, wasm::SuspenderObjectData::offsetOfMainFP()),
      FramePointer);

  // Set main_ra field to returnCallsite.
#  ifdef JS_CODEGEN_X86
  // SuspenderDataReg is also ScratchReg1, use DataReg as a scratch register.
  MOZ_ASSERT(ScratchReg1 == SuspenderDataReg);
  masm.push(DataReg);
  masm.mov(&returnCallsite, DataReg);
  masm.storePtr(
      DataReg,
      Address(SuspenderDataReg,
              wasm::SuspenderObjectData::offsetOfSuspendedReturnAddress()));
  masm.pop(DataReg);
#  else
  MOZ_ASSERT(ScratchReg1 != SuspenderDataReg);
  masm.mov(&returnCallsite, ScratchReg1);
  masm.storePtr(
      ScratchReg1,
      Address(SuspenderDataReg,
              wasm::SuspenderObjectData::offsetOfSuspendedReturnAddress()));
#  endif

  masm.assertStackAlignment(WasmStackAlignment);

  // The FramePointer is pointing to the same
  // place as before switch happened.
  uint32_t framePushed = masm.framePushed();

  // On different stack, reset framePushed. FramePointer is not valid here.
  masm.setFramePushed(0);

  prepareWasmStackSwitchTrampolineCall(SuspenderReg, DataReg);

  // Get wasm instance pointer for callee.
  size_t instanceSlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_INSTANCE_SLOT);
  masm.loadPtr(Address(FnReg, instanceSlotOffset), InstanceReg);

  masm.storePtr(InstanceReg, Address(masm.getStackPointer(),
                                     WasmCalleeInstanceOffsetBeforeCall));
  masm.loadWasmPinnedRegsFromInstance(mozilla::Nothing());

  masm.assertStackAlignment(WasmStackAlignment);

  const Register ReturnAddressReg = ScratchReg1;
  // DataReg is not needed anymore, using it as a scratch register.
  const Register ScratchReg2 = DataReg;

  // Save future of main stack exit frame pointer.
  masm.computeEffectiveAddress(
      Address(masm.getStackPointer(), -int32_t(sizeof(wasm::Frame))),
      ScratchReg2);
  masm.storePtr(ScratchReg2,
                Address(SuspenderDataReg,
                        wasm::SuspenderObjectData::offsetOfMainExitFP()));

  // Load InstanceReg from suspendable stack exit frame.
  masm.loadPtr(Address(SuspenderDataReg,
                       wasm::SuspenderObjectData::offsetOfSuspendableExitFP()),
               ScratchReg2);
  masm.loadPtr(
      Address(ScratchReg2, wasm::FrameWithInstances::callerInstanceOffset()),
      ScratchReg2);
  masm.storePtr(ScratchReg2, Address(masm.getStackPointer(),
                                     WasmCallerInstanceOffsetBeforeCall));

  // Load RA from suspendable stack exit frame.
  masm.loadPtr(Address(SuspenderDataReg,
                       wasm::SuspenderObjectData::offsetOfSuspendableExitFP()),
               ScratchReg1);
  masm.loadPtr(Address(ScratchReg1, wasm::Frame::returnAddressOffset()),
               ReturnAddressReg);

  // Call wasm function fast.
#  ifdef JS_USE_LINK_REGISTER
#    if defined(JS_CODEGEN_LOONG64)
  masm.mov(ReturnAddressReg, ra);
#    else
  masm.mov(ReturnAddressReg, lr);
#    endif
#  else
  masm.Push(ReturnAddressReg);
#  endif
  // Get funcUncheckedCallEntry() from the function's
  // WASM_FUNC_UNCHECKED_ENTRY_SLOT extended slot.
  size_t uncheckedEntrySlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_FUNC_UNCHECKED_ENTRY_SLOT);
  masm.loadPtr(Address(FnReg, uncheckedEntrySlotOffset), ScratchReg2);
  masm.jump(ScratchReg2);

  // About to use valid FramePointer -- restore framePushed.
  masm.setFramePushed(framePushed);

  // For IsPlausibleStackMapKey check for the following callsite.
  masm.wasmTrapInstruction();

  // Callsite for return from suspendable stack.
  masm.bind(&returnCallsite);
  masm.append(desc, *returnCallsite.target());
  masm.addCodeLabel(returnCallsite);

  masm.assertStackAlignment(WasmStackAlignment);

  markSafepointAt(returnCallsite.target()->offset(), lir);
  lir->safepoint()->setFramePushedAtStackMapBase(framePushed);
  lir->safepoint()->setWasmSafepointKind(WasmSafepointKind::StackSwitch);
  // Rooting SuspenderReg.
  masm.propagateOOM(
      lir->safepoint()->addWasmAnyRefSlot(true, framePushedAtSuspender));

  masm.freeStackTo(framePushed);

  // Push ReturnReg that is passed from ContinueOnSuspended on the stack after,
  // the SuspenderReg has been restored (see ScratchReg1 push below).
  // (On some platforms SuspenderReg == ReturnReg)
  masm.mov(ReturnReg, ScratchReg1);

  masm.freeStack(reserve);
  masm.Pop(InstanceReg);
  masm.Pop(SuspenderReg);

  masm.Push(ScratchReg1);

  masm.switchToWasmInstanceRealm(ScratchReg1, ScratchReg2);

  callWasmUpdateSuspenderState(wasm::UpdateSuspenderStateAction::Resume,
                               SuspenderReg, ScratchReg1);

  masm.Pop(ToRegister(lir->output()));

#else
  MOZ_CRASH("NYI");
#endif  // ENABLE_WASM_JSPI
}

void CodeGenerator::visitWasmStackContinueOnSuspendable(
    LWasmStackContinueOnSuspendable* lir) {
#ifdef ENABLE_WASM_JSPI
  const Register SuspenderReg = lir->suspender()->toGeneralReg()->reg();
  const Register ResultReg = lir->result()->toGeneralReg()->reg();
  const Register SuspenderDataReg = ABINonArgReg3;

#  ifdef JS_CODEGEN_ARM64
  vixl::UseScratchRegisterScope temps(&masm);
  const Register ScratchReg1 = temps.AcquireX().asUnsized();
#  elif defined(JS_CODEGEN_X86)
  const Register ScratchReg1 = ABINonArgReturnReg1;
#  elif defined(JS_CODEGEN_X64)
  const Register ScratchReg1 = ScratchReg;
#  elif defined(JS_CODEGEN_ARM)
  const Register ScratchReg1 = ABINonArgReturnVolatileReg;
#  elif defined(JS_CODEGEN_LOONG64)
  SecondScratchRegisterScope scratch2(masm);
  const Register ScratchReg1 = scratch2;
#  else
#    error "NYI: scratch register"
#  endif
  const Register ScratchReg2 = ABINonArgReg1;

  masm.Push(SuspenderReg);
  int32_t framePushedAtSuspender = masm.framePushed();
  masm.Push(InstanceReg);

  wasm::CallSiteDesc desc(wasm::CallSiteKind::StackSwitch);
  CodeLabel returnCallsite;

  // Aligning stack before trampoline call.
  uint32_t reserve = ComputeByteAlignment(
      masm.framePushed() - sizeof(wasm::Frame), WasmStackAlignment);
  masm.reserveStack(reserve);

  masm.loadPrivate(Address(SuspenderReg, NativeObject::getFixedSlotOffset(
                                             wasm::SuspenderObjectDataSlot)),
                   SuspenderDataReg);
  masm.storeStackPtr(
      Address(SuspenderDataReg, wasm::SuspenderObjectData::offsetOfMainSP()));
  masm.storePtr(
      FramePointer,
      Address(SuspenderDataReg, wasm::SuspenderObjectData::offsetOfMainFP()));

  // Adjust exit frame FP.
  masm.loadPtr(Address(SuspenderDataReg,
                       wasm::SuspenderObjectData::offsetOfSuspendableExitFP()),
               ScratchReg1);
  masm.storePtr(FramePointer,
                Address(ScratchReg1, wasm::Frame::callerFPOffset()));

  // Adjust exit frame RA.
  masm.mov(&returnCallsite, ScratchReg2);

  masm.storePtr(ScratchReg2,
                Address(ScratchReg1, wasm::Frame::returnAddressOffset()));
  // Adjust exit frame caller instance slot.
  masm.storePtr(
      InstanceReg,
      Address(ScratchReg1, wasm::FrameWithInstances::callerInstanceOffset()));

  // Switch stacks to suspendable.
  masm.loadStackPtr(Address(
      SuspenderDataReg, wasm::SuspenderObjectData::offsetOfSuspendableSP()));
  masm.loadPtr(Address(SuspenderDataReg,
                       wasm::SuspenderObjectData::offsetOfSuspendableFP()),
               FramePointer);

  masm.assertStackAlignment(WasmStackAlignment);

  // The FramePointer is pointing to the same
  // place as before switch happened.
  uint32_t framePushed = masm.framePushed();

  // On different stack, reset framePushed. FramePointer is not valid here.
  masm.setFramePushed(0);

  // Restore shadow stack area and instance slots.
  WasmABIArgGenerator abi;
  unsigned reserveBeforeCall = abi.stackBytesConsumedSoFar();
  MOZ_ASSERT(masm.framePushed() == 0);
  unsigned argDecrement =
      StackDecrementForCall(WasmStackAlignment, 0, reserveBeforeCall);
  masm.reserveStack(argDecrement);

  masm.storePtr(InstanceReg, Address(masm.getStackPointer(),
                                     WasmCallerInstanceOffsetBeforeCall));
  masm.storePtr(InstanceReg, Address(masm.getStackPointer(),
                                     WasmCalleeInstanceOffsetBeforeCall));

  masm.assertStackAlignment(WasmStackAlignment);

  // Transfer results to ReturnReg so it will appear at SwitchToMain return.
  masm.mov(ResultReg, ReturnReg);

  const Register ReturnAddressReg = ScratchReg1;

  // Pretend we just returned from the function.
  masm.loadPtr(
      Address(SuspenderDataReg,
              wasm::SuspenderObjectData::offsetOfSuspendedReturnAddress()),
      ReturnAddressReg);
  masm.jump(ReturnAddressReg);

  // About to use valid FramePointer -- restore framePushed.
  masm.setFramePushed(framePushed);

  // For IsPlausibleStackMapKey check for the following callsite.
  masm.wasmTrapInstruction();

  // Callsite for return from suspendable stack.
  masm.bind(&returnCallsite);
  masm.append(desc, *returnCallsite.target());
  masm.addCodeLabel(returnCallsite);

  masm.assertStackAlignment(WasmStackAlignment);

  markSafepointAt(returnCallsite.target()->offset(), lir);
  lir->safepoint()->setFramePushedAtStackMapBase(framePushed);
  lir->safepoint()->setWasmSafepointKind(WasmSafepointKind::StackSwitch);
  // Rooting SuspenderReg.
  masm.propagateOOM(
      lir->safepoint()->addWasmAnyRefSlot(true, framePushedAtSuspender));

  masm.freeStackTo(framePushed);

  masm.freeStack(reserve);
  masm.Pop(InstanceReg);
  masm.Pop(SuspenderReg);

  // Using SuspenderDataReg and ABINonArgReg2 as temps.
  masm.switchToWasmInstanceRealm(SuspenderDataReg, ABINonArgReg2);

  callWasmUpdateSuspenderState(wasm::UpdateSuspenderStateAction::Leave,
                               SuspenderReg, ScratchReg1);
#else
  MOZ_CRASH("NYI");
#endif  // ENABLE_WASM_JSPI
}

void CodeGenerator::visitWasmCallLandingPrePad(LWasmCallLandingPrePad* lir) {
  LBlock* block = lir->block();
  MWasmCallLandingPrePad* mir = lir->mir();
  MBasicBlock* mirBlock = mir->block();
  MBasicBlock* callMirBlock = mir->callBlock();

  // This block must be the pre-pad successor of the call block. No blocks may
  // be inserted between us, such as for critical edge splitting.
  MOZ_RELEASE_ASSERT(mirBlock == callMirBlock->getSuccessor(
                                     MWasmCallCatchable::PrePadBranchIndex));

  // This instruction or a move group must be the first instruction in the
  // block. No other instructions may be inserted.
  MOZ_RELEASE_ASSERT(*block->begin() == lir || (block->begin()->isMoveGroup() &&
                                                *(++block->begin()) == lir));

  wasm::TryNoteVector& tryNotes = masm.tryNotes();
  wasm::TryNote& tryNote = tryNotes[mir->tryNoteIndex()];
  // Set the entry point for the call try note to be the beginning of this
  // block. The above assertions (and assertions in visitWasmCall) guarantee
  // that we are not skipping over instructions that should be executed.
  tryNote.setLandingPad(block->label()->offset(), masm.framePushed());
}

void CodeGenerator::visitWasmCallIndirectAdjunctSafepoint(
    LWasmCallIndirectAdjunctSafepoint* lir) {
  markSafepointAt(lir->safepointLocation().offset(), lir);
  lir->safepoint()->setFramePushedAtStackMapBase(
      lir->framePushedAtStackMapBase());
}

template <typename InstructionWithMaybeTrapSite>
void EmitSignalNullCheckTrapSite(MacroAssembler& masm,
                                 InstructionWithMaybeTrapSite* ins,
                                 FaultingCodeOffset fco,
                                 wasm::TrapMachineInsn tmi) {
  if (!ins->maybeTrap()) {
    return;
  }
  masm.append(wasm::Trap::NullPointerDereference, tmi, fco.get(),
              *ins->maybeTrap());
}

template <typename InstructionWithMaybeTrapSite, class AddressOrBaseIndex>
void CodeGenerator::emitWasmValueLoad(InstructionWithMaybeTrapSite* ins,
                                      MIRType type, MWideningOp wideningOp,
                                      AddressOrBaseIndex addr,
                                      AnyRegister dst) {
  FaultingCodeOffset fco;
  switch (type) {
    case MIRType::Int32:
      switch (wideningOp) {
        case MWideningOp::None:
          fco = masm.load32(addr, dst.gpr());
          EmitSignalNullCheckTrapSite(masm, ins, fco,
                                      wasm::TrapMachineInsn::Load32);
          break;
        case MWideningOp::FromU16:
          fco = masm.load16ZeroExtend(addr, dst.gpr());
          EmitSignalNullCheckTrapSite(masm, ins, fco,
                                      wasm::TrapMachineInsn::Load16);
          break;
        case MWideningOp::FromS16:
          fco = masm.load16SignExtend(addr, dst.gpr());
          EmitSignalNullCheckTrapSite(masm, ins, fco,
                                      wasm::TrapMachineInsn::Load16);
          break;
        case MWideningOp::FromU8:
          fco = masm.load8ZeroExtend(addr, dst.gpr());
          EmitSignalNullCheckTrapSite(masm, ins, fco,
                                      wasm::TrapMachineInsn::Load8);
          break;
        case MWideningOp::FromS8:
          fco = masm.load8SignExtend(addr, dst.gpr());
          EmitSignalNullCheckTrapSite(masm, ins, fco,
                                      wasm::TrapMachineInsn::Load8);
          break;
        default:
          MOZ_CRASH("unexpected widening op in ::visitWasmLoadElement");
      }
      break;
    case MIRType::Float32:
      MOZ_ASSERT(wideningOp == MWideningOp::None);
      fco = masm.loadFloat32(addr, dst.fpu());
      EmitSignalNullCheckTrapSite(masm, ins, fco,
                                  wasm::TrapMachineInsn::Load32);
      break;
    case MIRType::Double:
      MOZ_ASSERT(wideningOp == MWideningOp::None);
      fco = masm.loadDouble(addr, dst.fpu());
      EmitSignalNullCheckTrapSite(masm, ins, fco,
                                  wasm::TrapMachineInsn::Load64);
      break;
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::WasmArrayData:
      MOZ_ASSERT(wideningOp == MWideningOp::None);
      fco = masm.loadPtr(addr, dst.gpr());
      EmitSignalNullCheckTrapSite(masm, ins, fco,
                                  wasm::TrapMachineInsnForLoadWord());
      break;
    default:
      MOZ_CRASH("unexpected type in ::emitWasmValueLoad");
  }
}

template <typename InstructionWithMaybeTrapSite, class AddressOrBaseIndex>
void CodeGenerator::emitWasmValueStore(InstructionWithMaybeTrapSite* ins,
                                       MIRType type, MNarrowingOp narrowingOp,
                                       AnyRegister src,
                                       AddressOrBaseIndex addr) {
  FaultingCodeOffset fco;
  switch (type) {
    case MIRType::Int32:
      switch (narrowingOp) {
        case MNarrowingOp::None:
          fco = masm.store32(src.gpr(), addr);
          EmitSignalNullCheckTrapSite(masm, ins, fco,
                                      wasm::TrapMachineInsn::Store32);
          break;
        case MNarrowingOp::To16:
          fco = masm.store16(src.gpr(), addr);
          EmitSignalNullCheckTrapSite(masm, ins, fco,
                                      wasm::TrapMachineInsn::Store16);
          break;
        case MNarrowingOp::To8:
          fco = masm.store8(src.gpr(), addr);
          EmitSignalNullCheckTrapSite(masm, ins, fco,
                                      wasm::TrapMachineInsn::Store8);
          break;
        default:
          MOZ_CRASH();
      }
      break;
    case MIRType::Float32:
      fco = masm.storeFloat32(src.fpu(), addr);
      EmitSignalNullCheckTrapSite(masm, ins, fco,
                                  wasm::TrapMachineInsn::Store32);
      break;
    case MIRType::Double:
      fco = masm.storeDouble(src.fpu(), addr);
      EmitSignalNullCheckTrapSite(masm, ins, fco,
                                  wasm::TrapMachineInsn::Store64);
      break;
    case MIRType::Pointer:
      // This could be correct, but it would be a new usage, so check carefully.
      MOZ_CRASH("Unexpected type in ::emitWasmValueStore.");
    case MIRType::WasmAnyRef:
      MOZ_CRASH("Bad type in ::emitWasmValueStore. Use LWasmStoreElementRef.");
    default:
      MOZ_CRASH("unexpected type in ::emitWasmValueStore");
  }
}

void CodeGenerator::visitWasmLoadSlot(LWasmLoadSlot* ins) {
  MIRType type = ins->type();
  MWideningOp wideningOp = ins->wideningOp();
  Register container = ToRegister(ins->containerRef());
  Address addr(container, ins->offset());
  AnyRegister dst = ToAnyRegister(ins->output());

#ifdef ENABLE_WASM_SIMD
  if (type == MIRType::Simd128) {
    MOZ_ASSERT(wideningOp == MWideningOp::None);
    FaultingCodeOffset fco = masm.loadUnalignedSimd128(addr, dst.fpu());
    EmitSignalNullCheckTrapSite(masm, ins, fco, wasm::TrapMachineInsn::Load128);
    return;
  }
#endif
  emitWasmValueLoad(ins, type, wideningOp, addr, dst);
}

void CodeGenerator::visitWasmLoadElement(LWasmLoadElement* ins) {
  MIRType type = ins->type();
  MWideningOp wideningOp = ins->wideningOp();
  Scale scale = ins->scale();
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  AnyRegister dst = ToAnyRegister(ins->output());

#ifdef ENABLE_WASM_SIMD
  if (type == MIRType::Simd128) {
    MOZ_ASSERT(wideningOp == MWideningOp::None);
    FaultingCodeOffset fco;
    Register temp = ToRegister(ins->temp0());
    masm.lshiftPtr(Imm32(4), index, temp);
    fco = masm.loadUnalignedSimd128(BaseIndex(base, temp, Scale::TimesOne),
                                    dst.fpu());
    EmitSignalNullCheckTrapSite(masm, ins, fco, wasm::TrapMachineInsn::Load128);
    return;
  }
#endif
  emitWasmValueLoad(ins, type, wideningOp, BaseIndex(base, index, scale), dst);
}

void CodeGenerator::visitWasmStoreSlot(LWasmStoreSlot* ins) {
  MIRType type = ins->type();
  MNarrowingOp narrowingOp = ins->narrowingOp();
  Register container = ToRegister(ins->containerRef());
  Address addr(container, ins->offset());
  AnyRegister src = ToAnyRegister(ins->value());
  if (type != MIRType::Int32) {
    MOZ_RELEASE_ASSERT(narrowingOp == MNarrowingOp::None);
  }

#ifdef ENABLE_WASM_SIMD
  if (type == MIRType::Simd128) {
    FaultingCodeOffset fco = masm.storeUnalignedSimd128(src.fpu(), addr);
    EmitSignalNullCheckTrapSite(masm, ins, fco,
                                wasm::TrapMachineInsn::Store128);
    return;
  }
#endif
  emitWasmValueStore(ins, type, narrowingOp, src, addr);
}

void CodeGenerator::visitWasmStoreStackResult(LWasmStoreStackResult* ins) {
  const LAllocation* value = ins->value();
  Address addr(ToRegister(ins->stackResultsArea()), ins->offset());

  switch (ins->type()) {
    case MIRType::Int32:
      masm.storePtr(ToRegister(value), addr);
      break;
    case MIRType::Float32:
      masm.storeFloat32(ToFloatRegister(value), addr);
      break;
    case MIRType::Double:
      masm.storeDouble(ToFloatRegister(value), addr);
      break;
#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128:
      masm.storeUnalignedSimd128(ToFloatRegister(value), addr);
      break;
#endif
    case MIRType::WasmAnyRef:
      masm.storePtr(ToRegister(value), addr);
      break;
    default:
      MOZ_CRASH("unexpected type in ::visitWasmStoreStackResult");
  }
}

void CodeGenerator::visitWasmStoreStackResultI64(LWasmStoreStackResultI64* ins) {
  masm.store64(ToRegister64(ins->value()), Address(ToRegister(ins->stackResultsArea()), ins->offset()));
}

void CodeGenerator::visitWasmStoreElement(LWasmStoreElement* ins) {
  MIRType type = ins->type();
  MNarrowingOp narrowingOp = ins->narrowingOp();
  Scale scale = ins->scale();
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  AnyRegister src = ToAnyRegister(ins->value());
  if (type != MIRType::Int32) {
    MOZ_RELEASE_ASSERT(narrowingOp == MNarrowingOp::None);
  }

#ifdef ENABLE_WASM_SIMD
  if (type == MIRType::Simd128) {
    Register temp = ToRegister(ins->temp0());
    masm.lshiftPtr(Imm32(4), index, temp);
    FaultingCodeOffset fco = masm.storeUnalignedSimd128(
        src.fpu(), BaseIndex(base, temp, Scale::TimesOne));
    EmitSignalNullCheckTrapSite(masm, ins, fco,
                                wasm::TrapMachineInsn::Store128);
    return;
  }
#endif
  emitWasmValueStore(ins, type, narrowingOp, src,
                     BaseIndex(base, index, scale));
}

void CodeGenerator::visitWasmLoadTableElement(LWasmLoadTableElement* ins) {
  Register elements = ToRegister(ins->elements());
  Register index = ToRegister(ins->index());
  Register output = ToRegister(ins->output());
  masm.loadPtr(BaseIndex(elements, index, ScalePointer), output);
}

void CodeGenerator::visitWasmDerivedPointer(LWasmDerivedPointer* ins) {
  masm.movePtr(ToRegister(ins->base()), ToRegister(ins->output()));
  masm.addPtr(Imm32(int32_t(ins->mir()->offset())), ToRegister(ins->output()));
}

void CodeGenerator::visitWasmDerivedIndexPointer(
    LWasmDerivedIndexPointer* ins) {
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  Register output = ToRegister(ins->output());
  masm.computeEffectiveAddress(BaseIndex(base, index, ins->mir()->scale()),
                               output);
}

void CodeGenerator::visitWasmStoreRef(LWasmStoreRef* ins) {
  Register instance = ToRegister(ins->instance());
  Register valueBase = ToRegister(ins->valueBase());
  size_t offset = ins->offset();
  Register value = ToRegister(ins->value());
  Register temp = ToRegister(ins->temp0());

  if (ins->preBarrierKind() == WasmPreBarrierKind::Normal) {
    Label skipPreBarrier;
    wasm::EmitWasmPreBarrierGuard(masm, instance, temp,
                                  Address(valueBase, offset), &skipPreBarrier,
                                  ins->maybeTrap());
    wasm::EmitWasmPreBarrierCallImmediate(masm, instance, temp, valueBase,
                                          offset);
    masm.bind(&skipPreBarrier);
  }

  FaultingCodeOffset fco = masm.storePtr(value, Address(valueBase, offset));
  EmitSignalNullCheckTrapSite(masm, ins, fco,
                              wasm::TrapMachineInsnForStoreWord());
  // The postbarrier is handled separately.
}

void CodeGenerator::visitWasmStoreElementRef(LWasmStoreElementRef* ins) {
  Register instance = ToRegister(ins->instance());
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  Register value = ToRegister(ins->value());
  Register temp0 = ToTempRegisterOrInvalid(ins->temp0());
  Register temp1 = ToTempRegisterOrInvalid(ins->temp1());

  BaseIndex addr(base, index, ScalePointer);

  if (ins->preBarrierKind() == WasmPreBarrierKind::Normal) {
    Label skipPreBarrier;
    wasm::EmitWasmPreBarrierGuard(masm, instance, temp0, addr, &skipPreBarrier,
                                  ins->maybeTrap());
    wasm::EmitWasmPreBarrierCallIndex(masm, instance, temp0, temp1, addr);
    masm.bind(&skipPreBarrier);
  }

  FaultingCodeOffset fco = masm.storePtr(value, addr);
  EmitSignalNullCheckTrapSite(masm, ins, fco,
                              wasm::TrapMachineInsnForStoreWord());
  // The postbarrier is handled separately.
}

void CodeGenerator::visitWasmPostWriteBarrierWholeCell(
    LWasmPostWriteBarrierWholeCell* lir) {
  Register object = ToRegister(lir->object());
  Register value = ToRegister(lir->value());
  Register temp = ToRegister(lir->temp0());
  MOZ_ASSERT(ToRegister(lir->instance()) == InstanceReg);
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    // Skip the barrier if this object was previously added to the store buffer.
    // We perform this check out of line because in practice the prior guards
    // eliminate most calls to the barrier.
    wasm::CheckWholeCellLastElementCache(masm, InstanceReg, object, temp,
                                         ool.rejoin());

    saveLiveVolatile(lir);
    masm.Push(InstanceReg);
    int32_t framePushedAfterInstance = masm.framePushed();

    // Call Instance::postBarrierWholeCell
    masm.setupWasmABICall();
    masm.passABIArg(InstanceReg);
    masm.passABIArg(object);
    int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
    masm.callWithABI(wasm::BytecodeOffset(0),
                     wasm::SymbolicAddress::PostBarrierWholeCell,
                     mozilla::Some(instanceOffset), ABIType::General);

    masm.Pop(InstanceReg);
    restoreLiveVolatile(lir);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  wasm::EmitWasmPostBarrierGuard(masm, mozilla::Some(object), temp, value,
                                 ool->rejoin());
  masm.jump(ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitWasmPostWriteBarrierEdgeAtIndex(
    LWasmPostWriteBarrierEdgeAtIndex* lir) {
  Register object = ToRegister(lir->object());
  Register value = ToRegister(lir->value());
  Register valueBase = ToRegister(lir->valueBase());
  Register index = ToRegister(lir->index());
  Register temp = ToRegister(lir->temp0());
  MOZ_ASSERT(ToRegister(lir->instance()) == InstanceReg);
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    saveLiveVolatile(lir);
    masm.Push(InstanceReg);
    int32_t framePushedAfterInstance = masm.framePushed();

    // Fold the value offset into the value base
    if (lir->elemSize() == 16) {
      masm.lshiftPtr(Imm32(4), index, temp);
      masm.addPtr(valueBase, temp);
    } else {
      masm.computeEffectiveAddress(
          BaseIndex(valueBase, index, ScaleFromElemWidth(lir->elemSize())),
          temp);
    }

    // Call Instance::postBarrier
    masm.setupWasmABICall();
    masm.passABIArg(InstanceReg);
    masm.passABIArg(temp);
    int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
    masm.callWithABI(wasm::BytecodeOffset(0),
                     wasm::SymbolicAddress::PostBarrierEdge,
                     mozilla::Some(instanceOffset), ABIType::General);

    masm.Pop(InstanceReg);
    restoreLiveVolatile(lir);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  wasm::EmitWasmPostBarrierGuard(masm, mozilla::Some(object), temp, value,
                                 ool->rejoin());
  masm.jump(ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitWasmLoadSlotI64(LWasmLoadSlotI64* ins) {
  Register container = ToRegister(ins->containerRef());
  Address addr(container, ins->offset());
  Register64 output = ToOutRegister64(ins);
  // Either 1 or 2 words.  On a 32-bit target, it is hard to argue that one
  // transaction will always trap before the other, so it seems safest to
  // register both of them as potentially trapping.
#ifdef JS_64BIT
  FaultingCodeOffset fco = masm.load64(addr, output);
  EmitSignalNullCheckTrapSite(masm, ins, fco, wasm::TrapMachineInsn::Load64);
#else
  FaultingCodeOffsetPair fcop = masm.load64(addr, output);
  EmitSignalNullCheckTrapSite(masm, ins, fcop.first,
                              wasm::TrapMachineInsn::Load32);
  EmitSignalNullCheckTrapSite(masm, ins, fcop.second,
                              wasm::TrapMachineInsn::Load32);
#endif
}

void CodeGenerator::visitWasmLoadElementI64(LWasmLoadElementI64* ins) {
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  BaseIndex addr(base, index, Scale::TimesEight);
  Register64 output = ToOutRegister64(ins);
  // Either 1 or 2 words.  On a 32-bit target, it is hard to argue that one
  // transaction will always trap before the other, so it seems safest to
  // register both of them as potentially trapping.
#ifdef JS_64BIT
  FaultingCodeOffset fco = masm.load64(addr, output);
  EmitSignalNullCheckTrapSite(masm, ins, fco, wasm::TrapMachineInsn::Load64);
#else
  FaultingCodeOffsetPair fcop = masm.load64(addr, output);
  EmitSignalNullCheckTrapSite(masm, ins, fcop.first,
                              wasm::TrapMachineInsn::Load32);
  EmitSignalNullCheckTrapSite(masm, ins, fcop.second,
                              wasm::TrapMachineInsn::Load32);
#endif
}

void CodeGenerator::visitWasmStoreSlotI64(LWasmStoreSlotI64* ins) {
  Register container = ToRegister(ins->containerRef());
  Address addr(container, ins->offset());
  Register64 value = ToRegister64(ins->value());
  // Either 1 or 2 words.  As above we register both transactions in the
  // 2-word case.
#ifdef JS_64BIT
  FaultingCodeOffset fco = masm.store64(value, addr);
  EmitSignalNullCheckTrapSite(masm, ins, fco, wasm::TrapMachineInsn::Store64);
#else
  FaultingCodeOffsetPair fcop = masm.store64(value, addr);
  EmitSignalNullCheckTrapSite(masm, ins, fcop.first,
                              wasm::TrapMachineInsn::Store32);
  EmitSignalNullCheckTrapSite(masm, ins, fcop.second,
                              wasm::TrapMachineInsn::Store32);
#endif
}

void CodeGenerator::visitWasmStoreElementI64(LWasmStoreElementI64* ins) {
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  BaseIndex addr(base, index, Scale::TimesEight);
  Register64 value = ToRegister64(ins->value());
  // Either 1 or 2 words.  As above we register both transactions in the
  // 2-word case.
#ifdef JS_64BIT
  FaultingCodeOffset fco = masm.store64(value, addr);
  EmitSignalNullCheckTrapSite(masm, ins, fco, wasm::TrapMachineInsn::Store64);
#else
  FaultingCodeOffsetPair fcop = masm.store64(value, addr);
  EmitSignalNullCheckTrapSite(masm, ins, fcop.first,
                              wasm::TrapMachineInsn::Store32);
  EmitSignalNullCheckTrapSite(masm, ins, fcop.second,
                              wasm::TrapMachineInsn::Store32);
#endif
}

void CodeGenerator::visitWasmClampTable64Address(
    LWasmClampTable64Address* lir) {
#ifdef ENABLE_WASM_MEMORY64
  Register64 address = ToRegister64(lir->address());
  Register out = ToRegister(lir->output());
  masm.wasmClampTable64Address(address, out);
#else
  MOZ_CRASH("table64 addresses should not be valid without memory64");
#endif
}

void CodeGenerator::visitArrayBufferByteLength(LArrayBufferByteLength* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());
  masm.loadArrayBufferByteLengthIntPtr(obj, out);
}

void CodeGenerator::visitArrayBufferViewLength(LArrayBufferViewLength* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());
  masm.loadArrayBufferViewLengthIntPtr(obj, out);
}

void CodeGenerator::visitArrayBufferViewByteOffset(
    LArrayBufferViewByteOffset* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());
  masm.loadArrayBufferViewByteOffsetIntPtr(obj, out);
}

void CodeGenerator::visitArrayBufferViewElements(
    LArrayBufferViewElements* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());
  masm.loadPtr(Address(obj, ArrayBufferViewObject::dataOffset()), out);
}

void CodeGenerator::visitTypedArrayElementSize(LTypedArrayElementSize* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());

  masm.typedArrayElementSize(obj, out);
}

void CodeGenerator::visitResizableTypedArrayByteOffsetMaybeOutOfBounds(
    LResizableTypedArrayByteOffsetMaybeOutOfBounds* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  masm.loadResizableTypedArrayByteOffsetMaybeOutOfBoundsIntPtr(obj, out, temp);
}

void CodeGenerator::visitResizableTypedArrayLength(
    LResizableTypedArrayLength* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  masm.loadResizableTypedArrayLengthIntPtr(lir->synchronization(), obj, out,
                                           temp);
}

void CodeGenerator::visitResizableDataViewByteLength(
    LResizableDataViewByteLength* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  masm.loadResizableDataViewByteLengthIntPtr(lir->synchronization(), obj, out,
                                             temp);
}

void CodeGenerator::visitGrowableSharedArrayBufferByteLength(
    LGrowableSharedArrayBufferByteLength* lir) {
  Register obj = ToRegister(lir->object());
  Register out = ToRegister(lir->output());

  // Explicit |byteLength| accesses are seq-consistent atomic loads.
  auto sync = Synchronization::Load();

  masm.loadGrowableSharedArrayBufferByteLengthIntPtr(sync, obj, out);
}

void CodeGenerator::visitGuardResizableArrayBufferViewInBounds(
    LGuardResizableArrayBufferViewInBounds* lir) {
  Register obj = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp0());

  Label bail;
  masm.branchIfResizableArrayBufferViewOutOfBounds(obj, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardResizableArrayBufferViewInBoundsOrDetached(
    LGuardResizableArrayBufferViewInBoundsOrDetached* lir) {
  Register obj = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp0());

  Label done, bail;
  masm.branchIfResizableArrayBufferViewInBounds(obj, temp, &done);
  masm.branchIfHasAttachedArrayBuffer(obj, temp, &bail);
  masm.bind(&done);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardHasAttachedArrayBuffer(
    LGuardHasAttachedArrayBuffer* lir) {
  Register obj = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp0());

  Label bail;
  masm.branchIfHasDetachedArrayBuffer(obj, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardNumberToIntPtrIndex(
    LGuardNumberToIntPtrIndex* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  if (!lir->mir()->supportOOB()) {
    Label bail;
    masm.convertDoubleToPtr(input, output, &bail, false);
    bailoutFrom(&bail, lir->snapshot());
    return;
  }

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    // Substitute the invalid index with an arbitrary out-of-bounds index.
    masm.movePtr(ImmWord(-1), output);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  masm.convertDoubleToPtr(input, output, ool->entry(), false);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitStringLength(LStringLength* lir) {
  Register input = ToRegister(lir->string());
  Register output = ToRegister(lir->output());

  masm.loadStringLength(input, output);
}

void CodeGenerator::visitMinMaxI(LMinMaxI* ins) {
  Register first = ToRegister(ins->first());
  Register output = ToRegister(ins->output());

  MOZ_ASSERT(first == output);

  Assembler::Condition cond =
      ins->mir()->isMax() ? Assembler::GreaterThan : Assembler::LessThan;

  if (ins->second()->isConstant()) {
    Label done;
    masm.branch32(cond, first, Imm32(ToInt32(ins->second())), &done);
    masm.move32(Imm32(ToInt32(ins->second())), output);
    masm.bind(&done);
  } else {
    Register second = ToRegister(ins->second());
    masm.cmp32Move32(cond, second, first, second, output);
  }
}

void CodeGenerator::visitMinMaxArrayI(LMinMaxArrayI* ins) {
  Register array = ToRegister(ins->array());
  Register output = ToRegister(ins->output());
  Register temp1 = ToRegister(ins->temp0());
  Register temp2 = ToRegister(ins->temp1());
  Register temp3 = ToRegister(ins->temp2());
  bool isMax = ins->mir()->isMax();

  Label bail;
  masm.minMaxArrayInt32(array, output, temp1, temp2, temp3, isMax, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitMinMaxArrayD(LMinMaxArrayD* ins) {
  Register array = ToRegister(ins->array());
  FloatRegister output = ToFloatRegister(ins->output());
  FloatRegister floatTemp = ToFloatRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  bool isMax = ins->mir()->isMax();

  Label bail;
  masm.minMaxArrayNumber(array, output, floatTemp, temp1, temp2, isMax, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

// For Abs*, lowering will have tied input to output on platforms where that is
// sensible, and otherwise left them untied.

void CodeGenerator::visitAbsI(LAbsI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  if (ins->mir()->fallible()) {
    Label positive;
    if (input != output) {
      masm.move32(input, output);
    }
    masm.branchTest32(Assembler::NotSigned, output, output, &positive);
    Label bail;
    masm.branchNeg32(Assembler::Overflow, output, &bail);
    bailoutFrom(&bail, ins->snapshot());
    masm.bind(&positive);
  } else {
    masm.abs32(input, output);
  }
}

void CodeGenerator::visitAbsD(LAbsD* ins) {
  masm.absDouble(ToFloatRegister(ins->input()), ToFloatRegister(ins->output()));
}

void CodeGenerator::visitAbsF(LAbsF* ins) {
  masm.absFloat32(ToFloatRegister(ins->input()),
                  ToFloatRegister(ins->output()));
}

void CodeGenerator::visitPowII(LPowII* ins) {
  Register value = ToRegister(ins->value());
  Register power = ToRegister(ins->power());
  Register output = ToRegister(ins->output());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());

  Label bailout;
  masm.pow32(value, power, output, temp0, temp1, &bailout);
  bailoutFrom(&bailout, ins->snapshot());
}

void CodeGenerator::visitPowI(LPowI* ins) {
  FloatRegister value = ToFloatRegister(ins->value());
  Register power = ToRegister(ins->power());

  using Fn = double (*)(double x, int32_t y);
  masm.setupAlignedABICall();
  masm.passABIArg(value, ABIType::Float64);
  masm.passABIArg(power);

  masm.callWithABI<Fn, js::powi>(ABIType::Float64);
  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
}

void CodeGenerator::visitPowD(LPowD* ins) {
  FloatRegister value = ToFloatRegister(ins->value());
  FloatRegister power = ToFloatRegister(ins->power());

  using Fn = double (*)(double x, double y);
  masm.setupAlignedABICall();
  masm.passABIArg(value, ABIType::Float64);
  masm.passABIArg(power, ABIType::Float64);
  masm.callWithABI<Fn, ecmaPow>(ABIType::Float64);

  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
}

void CodeGenerator::visitPowOfTwoI(LPowOfTwoI* ins) {
  Register power = ToRegister(ins->power());
  Register output = ToRegister(ins->output());

  uint32_t base = ins->base();
  MOZ_ASSERT(mozilla::IsPowerOfTwo(base));

  uint32_t n = mozilla::FloorLog2(base);
  MOZ_ASSERT(n != 0);

  // Hacker's Delight, 2nd edition, theorem D2.
  auto ceilingDiv = [](uint32_t x, uint32_t y) { return (x + y - 1) / y; };

  // Take bailout if |power| is greater-or-equals |log_y(2^31)| or is negative.
  // |2^(n*y) < 2^31| must hold, hence |n*y < 31| resp. |y < 31/n|.
  //
  // Note: it's important for this condition to match the code in CacheIR.cpp
  // (CanAttachInt32Pow) to prevent failure loops.
  bailoutCmp32(Assembler::AboveOrEqual, power, Imm32(ceilingDiv(31, n)),
               ins->snapshot());

  // Compute (2^n)^y as 2^(n*y) using repeated shifts. We could directly scale
  // |power| and perform a single shift, but due to the lack of necessary
  // MacroAssembler functionality, like multiplying a register with an
  // immediate, we restrict the number of generated shift instructions when
  // lowering this operation.
  masm.move32(Imm32(1), output);
  do {
    masm.lshift32(power, output);
    n--;
  } while (n > 0);
}

void CodeGenerator::visitSqrtD(LSqrtD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());
  masm.sqrtDouble(input, output);
}

void CodeGenerator::visitSqrtF(LSqrtF* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());
  masm.sqrtFloat32(input, output);
}

void CodeGenerator::visitSignI(LSignI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());
  masm.signInt32(input, output);
}

void CodeGenerator::visitSignD(LSignD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());
  masm.signDouble(input, output);
}

void CodeGenerator::visitSignDI(LSignDI* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister temp = ToFloatRegister(ins->temp0());
  Register output = ToRegister(ins->output());

  Label bail;
  masm.signDoubleToInt32(input, output, temp, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitSignID(LSignID* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp0());
  FloatRegister output = ToFloatRegister(ins->output());

  masm.signInt32(input, temp);
  masm.convertInt32ToDouble(temp, output);
}

void CodeGenerator::visitMathFunctionD(LMathFunctionD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);

  UnaryMathFunction fun = ins->mir()->function();
  UnaryMathFunctionType funPtr = GetUnaryMathFunctionPtr(fun);

  masm.setupAlignedABICall();

  masm.passABIArg(input, ABIType::Float64);
  masm.callWithABI(DynamicFunction<UnaryMathFunctionType>(funPtr),
                   ABIType::Float64);
}

void CodeGenerator::visitMathFunctionF(LMathFunctionF* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnFloat32Reg);

  masm.setupAlignedABICall();
  masm.passABIArg(input, ABIType::Float32);

  using Fn = float (*)(float x);
  Fn funptr = nullptr;
  CheckUnsafeCallWithABI check = CheckUnsafeCallWithABI::Check;
  switch (ins->mir()->function()) {
    case UnaryMathFunction::Floor:
      funptr = floorf;
      check = CheckUnsafeCallWithABI::DontCheckOther;
      break;
    case UnaryMathFunction::Round:
      funptr = math_roundf_impl;
      break;
    case UnaryMathFunction::Trunc:
      funptr = math_truncf_impl;
      break;
    case UnaryMathFunction::Ceil:
      funptr = ceilf;
      check = CheckUnsafeCallWithABI::DontCheckOther;
      break;
    default:
      MOZ_CRASH("Unknown or unsupported float32 math function");
  }

  masm.callWithABI(DynamicFunction<Fn>(funptr), ABIType::Float32, check);
}

void CodeGenerator::visitModD(LModD* ins) {
  MOZ_ASSERT(!gen->compilingWasm());

  FloatRegister lhs = ToFloatRegister(ins->lhs());
  FloatRegister rhs = ToFloatRegister(ins->rhs());

  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);

  using Fn = double (*)(double a, double b);
  masm.setupAlignedABICall();
  masm.passABIArg(lhs, ABIType::Float64);
  masm.passABIArg(rhs, ABIType::Float64);
  masm.callWithABI<Fn, NumberMod>(ABIType::Float64);
}

void CodeGenerator::visitModPowTwoD(LModPowTwoD* ins) {
  FloatRegister lhs = ToFloatRegister(ins->lhs());
  uint32_t divisor = ins->divisor();
  MOZ_ASSERT(mozilla::IsPowerOfTwo(divisor));

  FloatRegister output = ToFloatRegister(ins->output());

  // Compute |n % d| using |copysign(n - (d * trunc(n / d)), n)|.
  //
  // This doesn't work if |d| isn't a power of two, because we may lose too much
  // precision. For example |Number.MAX_VALUE % 3 == 2|, but
  // |3 * trunc(Number.MAX_VALUE / 3) == Infinity|.

  Label done;
  {
    ScratchDoubleScope scratch(masm);

    // Subnormals can lead to performance degradation, which can make calling
    // |fmod| faster than this inline implementation. Work around this issue by
    // directly returning the input for any value in the interval ]-1, +1[.
    Label notSubnormal;
    masm.loadConstantDouble(1.0, scratch);
    masm.loadConstantDouble(-1.0, output);
    masm.branchDouble(Assembler::DoubleGreaterThanOrEqual, lhs, scratch,
                      &notSubnormal);
    masm.branchDouble(Assembler::DoubleLessThanOrEqual, lhs, output,
                      &notSubnormal);

    masm.moveDouble(lhs, output);
    masm.jump(&done);

    masm.bind(&notSubnormal);

    if (divisor == 1) {
      // The pattern |n % 1 == 0| is used to detect integer numbers. We can skip
      // the multiplication by one in this case.
      masm.moveDouble(lhs, output);
      masm.nearbyIntDouble(RoundingMode::TowardsZero, output, scratch);
      masm.subDouble(scratch, output);
    } else {
      masm.loadConstantDouble(1.0 / double(divisor), scratch);
      masm.loadConstantDouble(double(divisor), output);

      masm.mulDouble(lhs, scratch);
      masm.nearbyIntDouble(RoundingMode::TowardsZero, scratch, scratch);
      masm.mulDouble(output, scratch);

      masm.moveDouble(lhs, output);
      masm.subDouble(scratch, output);
    }
  }

  masm.copySignDouble(output, lhs, output);
  masm.bind(&done);
}

void CodeGenerator::visitWasmBuiltinModD(LWasmBuiltinModD* ins) {
  masm.Push(InstanceReg);
  int32_t framePushedAfterInstance = masm.framePushed();

  FloatRegister lhs = ToFloatRegister(ins->lhs());
  FloatRegister rhs = ToFloatRegister(ins->rhs());

  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);

  masm.setupWasmABICall();
  masm.passABIArg(lhs, ABIType::Float64);
  masm.passABIArg(rhs, ABIType::Float64);

  int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
  masm.callWithABI(ins->mir()->bytecodeOffset(), wasm::SymbolicAddress::ModD,
                   mozilla::Some(instanceOffset), ABIType::Float64);

  masm.Pop(InstanceReg);
}

void CodeGenerator::visitClzI(LClzI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());
  bool knownNotZero = ins->mir()->operandIsNeverZero();

  masm.clz32(input, output, knownNotZero);
}

void CodeGenerator::visitCtzI(LCtzI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());
  bool knownNotZero = ins->mir()->operandIsNeverZero();

  masm.ctz32(input, output, knownNotZero);
}

void CodeGenerator::visitPopcntI(LPopcntI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp0());

  masm.popcnt32(input, output, temp);
}

void CodeGenerator::visitClzI64(LClzI64* ins) {
  Register64 input = ToRegister64(ins->input());
  Register64 output = ToOutRegister64(ins);

  masm.clz64(input, output);
}

void CodeGenerator::visitCtzI64(LCtzI64* ins) {
  Register64 input = ToRegister64(ins->input());
  Register64 output = ToOutRegister64(ins);

  masm.ctz64(input, output);
}

void CodeGenerator::visitPopcntI64(LPopcntI64* ins) {
  Register64 input = ToRegister64(ins->input());
  Register64 output = ToOutRegister64(ins);
  Register temp = ToRegister(ins->temp0());

  masm.popcnt64(input, output, temp);
}

void CodeGenerator::visitBigIntAdd(LBigIntAdd* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::add>(ins);
}

void CodeGenerator::visitBigIntSub(LBigIntSub* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::sub>(ins);
}

void CodeGenerator::visitBigIntMul(LBigIntMul* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::mul>(ins);
}

void CodeGenerator::visitBigIntDiv(LBigIntDiv* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::div>(ins);
}

void CodeGenerator::visitBigIntMod(LBigIntMod* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::mod>(ins);
}

void CodeGenerator::visitBigIntPow(LBigIntPow* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::pow>(ins);
}

void CodeGenerator::visitBigIntBitAnd(LBigIntBitAnd* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::bitAnd>(ins);
}

void CodeGenerator::visitBigIntBitOr(LBigIntBitOr* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::bitOr>(ins);
}

void CodeGenerator::visitBigIntBitXor(LBigIntBitXor* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::bitXor>(ins);
}

void CodeGenerator::visitBigIntLsh(LBigIntLsh* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::lsh>(ins);
}

void CodeGenerator::visitBigIntRsh(LBigIntRsh* ins) {
  pushArg(ToRegister(ins->rhs()));
  pushArg(ToRegister(ins->lhs()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  callVM<Fn, BigInt::rsh>(ins);
}

void CodeGenerator::visitBigIntIncrement(LBigIntIncrement* ins) {
  pushArg(ToRegister(ins->input()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  callVM<Fn, BigInt::inc>(ins);
}

void CodeGenerator::visitBigIntDecrement(LBigIntDecrement* ins) {
  pushArg(ToRegister(ins->input()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  callVM<Fn, BigInt::dec>(ins);
}

void CodeGenerator::visitBigIntNegate(LBigIntNegate* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp0());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  auto* ool =
      oolCallVM<Fn, BigInt::neg>(ins, ArgList(input), StoreRegisterTo(output));

  // -0n == 0n
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(input, &lhsNonZero);
  masm.movePtr(input, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // Call into the VM when the input uses heap digits.
  masm.copyBigIntWithInlineDigits(input, output, temp, initialBigIntHeap(),
                                  ool->entry());

  // Flip the sign bit.
  masm.xor32(Imm32(BigInt::signBitMask()),
             Address(output, BigInt::offsetOfFlags()));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntBitNot(LBigIntBitNot* ins) {
  pushArg(ToRegister(ins->input()));

  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  callVM<Fn, BigInt::bitNot>(ins);
}

void CodeGenerator::visitBigIntToIntPtr(LBigIntToIntPtr* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  Label bail;
  masm.loadBigIntPtr(input, output, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitIntPtrToBigInt(LIntPtrToBigInt* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp0());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, intptr_t);
  auto* ool = oolCallVM<Fn, JS::BigInt::createFromIntPtr>(
      ins, ArgList(input), StoreRegisterTo(output));

  masm.newGCBigInt(output, temp, initialBigIntHeap(), ool->entry());
  masm.movePtr(input, temp);
  masm.initializeBigIntPtr(output, temp);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntPtrAdd(LBigIntPtrAdd* ins) {
  Register lhs = ToRegister(ins->lhs());
  const LAllocation* rhs = ins->rhs();
  Register output = ToRegister(ins->output());

  if (rhs->isConstant()) {
    masm.movePtr(ImmWord(ToIntPtr(rhs)), output);
  } else {
    masm.movePtr(ToRegister(rhs), output);
  }

  Label bail;
  masm.branchAddPtr(Assembler::Overflow, lhs, output, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitBigIntPtrSub(LBigIntPtrSub* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());

  Label bail;
  masm.movePtr(lhs, output);
  masm.branchSubPtr(Assembler::Overflow, rhs, output, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitBigIntPtrMul(LBigIntPtrMul* ins) {
  Register lhs = ToRegister(ins->lhs());
  const LAllocation* rhs = ins->rhs();
  Register output = ToRegister(ins->output());

  if (rhs->isConstant()) {
    masm.movePtr(ImmWord(ToIntPtr(rhs)), output);
  } else {
    masm.movePtr(ToRegister(rhs), output);
  }

  Label bail;
  masm.branchMulPtr(Assembler::Overflow, lhs, output, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitBigIntPtrDiv(LBigIntPtrDiv* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());

  // x / 0 throws an error.
  Label bail;
  if (ins->mir()->canBeDivideByZero()) {
    masm.branchPtr(Assembler::Equal, rhs, Imm32(0), &bail);
  }

  static constexpr auto DigitMin = std::numeric_limits<
      mozilla::SignedStdintTypeForSize<sizeof(BigInt::Digit)>::Type>::min();

  // Handle an integer overflow from INT{32,64}_MIN / -1.
  Label notOverflow;
  masm.branchPtr(Assembler::NotEqual, lhs, ImmWord(DigitMin), &notOverflow);
  masm.branchPtr(Assembler::Equal, rhs, Imm32(-1), &bail);
  masm.bind(&notOverflow);

  emitBigIntPtrDiv(ins, lhs, rhs, output);

  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitBigIntPtrDivPowTwo(LBigIntPtrDivPowTwo* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register output = ToRegister(ins->output());
  int32_t shift = ins->shift();
  bool negativeDivisor = ins->negativeDivisor();

  masm.movePtr(lhs, output);

  if (shift) {
    // Adjust the value so that shifting produces a correctly rounded result
    // when the numerator is negative.
    // See 10-1 "Signed Division by a Known Power of 2" in Henry S. Warren,
    // Jr.'s Hacker's Delight.

    constexpr size_t bits = BigInt::DigitBits;

    if (shift > 1) {
      // Copy the sign bit of the numerator. (= (2^bits - 1) or 0)
      masm.rshiftPtrArithmetic(Imm32(bits - 1), output);
    }

    // Divide by 2^(bits - shift)
    // i.e. (= (2^bits - 1) / 2^(bits - shift) or 0)
    // i.e. (= (2^shift - 1) or 0)
    masm.rshiftPtr(Imm32(bits - shift), output);

    // If signed, make any 1 bit below the shifted bits to bubble up, such that
    // once shifted the value would be rounded towards 0.
    masm.addPtr(lhs, output);

    masm.rshiftPtrArithmetic(Imm32(shift), output);

    if (negativeDivisor) {
      masm.negPtr(output);
    }
  } else if (negativeDivisor) {
    Label bail;
    masm.branchNegPtr(Assembler::Overflow, output, &bail);
    bailoutFrom(&bail, ins->snapshot());
  }
}

void CodeGenerator::visitBigIntPtrMod(LBigIntPtrMod* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp0());

  // x % 0 throws an error.
  if (ins->mir()->canBeDivideByZero()) {
    bailoutCmpPtr(Assembler::Equal, rhs, Imm32(0), ins->snapshot());
  }

  static constexpr auto DigitMin = std::numeric_limits<
      mozilla::SignedStdintTypeForSize<sizeof(BigInt::Digit)>::Type>::min();

  masm.movePtr(lhs, temp);

  // Handle an integer overflow from INT{32,64}_MIN / -1.
  Label notOverflow;
  masm.branchPtr(Assembler::NotEqual, lhs, ImmWord(DigitMin), &notOverflow);
  masm.branchPtr(Assembler::NotEqual, rhs, Imm32(-1), &notOverflow);
  masm.movePtr(ImmWord(0), temp);
  masm.bind(&notOverflow);

  emitBigIntPtrMod(ins, temp, rhs, output);
}

void CodeGenerator::visitBigIntPtrModPowTwo(LBigIntPtrModPowTwo* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp0());
  int32_t shift = ins->shift();

  masm.movePtr(lhs, output);
  masm.movePtr(ImmWord((uintptr_t(1) << shift) - uintptr_t(1)), temp);

  // Switch based on sign of the lhs.

  // Positive numbers are just a bitmask.
  Label negative;
  masm.branchTestPtr(Assembler::Signed, lhs, lhs, &negative);

  masm.andPtr(temp, output);

  Label done;
  masm.jump(&done);

  // Negative numbers need a negate, bitmask, negate
  masm.bind(&negative);

  masm.negPtr(output);
  masm.andPtr(temp, output);
  masm.negPtr(output);

  masm.bind(&done);
}

void CodeGenerator::visitBigIntPtrPow(LBigIntPtrPow* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());

  Label bail;
  masm.powPtr(lhs, rhs, output, temp0, temp1, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitBigIntPtrBitAnd(LBigIntPtrBitAnd* ins) {
  Register lhs = ToRegister(ins->lhs());
  const LAllocation* rhs = ins->rhs();
  Register output = ToRegister(ins->output());

  if (rhs->isConstant()) {
    masm.movePtr(ImmWord(ToIntPtr(rhs)), output);
  } else {
    masm.movePtr(ToRegister(rhs), output);
  }
  masm.andPtr(lhs, output);
}

void CodeGenerator::visitBigIntPtrBitOr(LBigIntPtrBitOr* ins) {
  Register lhs = ToRegister(ins->lhs());
  const LAllocation* rhs = ins->rhs();
  Register output = ToRegister(ins->output());

  if (rhs->isConstant()) {
    masm.movePtr(ImmWord(ToIntPtr(rhs)), output);
  } else {
    masm.movePtr(ToRegister(rhs), output);
  }
  masm.orPtr(lhs, output);
}

void CodeGenerator::visitBigIntPtrBitXor(LBigIntPtrBitXor* ins) {
  Register lhs = ToRegister(ins->lhs());
  const LAllocation* rhs = ins->rhs();
  Register output = ToRegister(ins->output());

  if (rhs->isConstant()) {
    masm.movePtr(ImmWord(ToIntPtr(rhs)), output);
  } else {
    masm.movePtr(ToRegister(rhs), output);
  }
  masm.xorPtr(lhs, output);
}

void CodeGenerator::visitBigIntPtrLsh(LBigIntPtrLsh* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register output = ToRegister(ins->output());
  Register temp = ToTempRegisterOrInvalid(ins->temp0());
  Register tempShift = ToTempRegisterOrInvalid(ins->temp1());

  if (ins->rhs()->isConstant()) {
    intptr_t rhs = ToIntPtr(ins->rhs());

    if (rhs >= intptr_t(BigInt::DigitBits)) {
      MOZ_ASSERT(ins->mir()->fallible());

      // x << DigitBits with x != 0n always exceeds pointer-sized storage.
      masm.movePtr(ImmWord(0), output);
      bailoutCmpPtr(Assembler::NotEqual, lhs, Imm32(0), ins->snapshot());
    } else if (rhs <= -intptr_t(BigInt::DigitBits)) {
      MOZ_ASSERT(!ins->mir()->fallible());

      // x << -DigitBits == x >> DigitBits, which is either 0n or -1n.
      masm.rshiftPtrArithmetic(Imm32(BigInt::DigitBits - 1), lhs, output);
    } else if (rhs <= 0) {
      MOZ_ASSERT(!ins->mir()->fallible());

      // |x << -y| is computed as |x >> y|.
      masm.rshiftPtrArithmetic(Imm32(-rhs), lhs, output);
    } else {
      MOZ_ASSERT(ins->mir()->fallible());

      masm.lshiftPtr(Imm32(rhs), lhs, output);

      // Check for overflow: ((lhs << rhs) >> rhs) == lhs.
      masm.rshiftPtrArithmetic(Imm32(rhs), output, temp);
      bailoutCmpPtr(Assembler::NotEqual, temp, lhs, ins->snapshot());
    }
  } else {
    Register rhs = ToRegister(ins->rhs());

    Label done, bail;
    MOZ_ASSERT(ins->mir()->fallible());

    masm.movePtr(lhs, output);

    // 0n << x == 0n
    masm.branchPtr(Assembler::Equal, lhs, Imm32(0), &done);

    // x << DigitBits with x != 0n always exceeds pointer-sized storage.
    masm.branchPtr(Assembler::GreaterThanOrEqual, rhs, Imm32(BigInt::DigitBits),
                   &bail);

    // x << -DigitBits == x >> DigitBits, which is either 0n or -1n.
    Label shift;
    masm.branchPtr(Assembler::GreaterThan, rhs,
                   Imm32(-int32_t(BigInt::DigitBits)), &shift);
    {
      masm.rshiftPtrArithmetic(Imm32(BigInt::DigitBits - 1), output);
      masm.jump(&done);
    }
    masm.bind(&shift);

    // Move |rhs| into the designated shift register.
    masm.movePtr(rhs, tempShift);

    // |x << -y| is computed as |x >> y|.
    Label leftShift;
    masm.branchPtr(Assembler::GreaterThanOrEqual, rhs, Imm32(0), &leftShift);
    {
      masm.negPtr(tempShift);
      masm.rshiftPtrArithmetic(tempShift, output);
      masm.jump(&done);
    }
    masm.bind(&leftShift);

    masm.lshiftPtr(tempShift, output);

    // Check for overflow: ((lhs << rhs) >> rhs) == lhs.
    masm.movePtr(output, temp);
    masm.rshiftPtrArithmetic(tempShift, temp);
    masm.branchPtr(Assembler::NotEqual, temp, lhs, &bail);

    masm.bind(&done);
    bailoutFrom(&bail, ins->snapshot());
  }
}

void CodeGenerator::visitBigIntPtrRsh(LBigIntPtrRsh* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register output = ToRegister(ins->output());
  Register temp = ToTempRegisterOrInvalid(ins->temp0());
  Register tempShift = ToTempRegisterOrInvalid(ins->temp1());

  if (ins->rhs()->isConstant()) {
    intptr_t rhs = ToIntPtr(ins->rhs());

    if (rhs <= -intptr_t(BigInt::DigitBits)) {
      MOZ_ASSERT(ins->mir()->fallible());

      // x >> -DigitBits == x << DigitBits, which exceeds pointer-sized storage.
      masm.movePtr(ImmWord(0), output);
      bailoutCmpPtr(Assembler::NotEqual, lhs, Imm32(0), ins->snapshot());
    } else if (rhs >= intptr_t(BigInt::DigitBits)) {
      MOZ_ASSERT(!ins->mir()->fallible());

      // x >> DigitBits is either 0n or -1n.
      masm.rshiftPtrArithmetic(Imm32(BigInt::DigitBits - 1), lhs, output);
    } else if (rhs < 0) {
      MOZ_ASSERT(ins->mir()->fallible());

      // |x >> -y| is computed as |x << y|.
      masm.lshiftPtr(Imm32(-rhs), lhs, output);

      // Check for overflow: ((lhs << rhs) >> rhs) == lhs.
      masm.rshiftPtrArithmetic(Imm32(-rhs), output, temp);
      bailoutCmpPtr(Assembler::NotEqual, temp, lhs, ins->snapshot());
    } else {
      MOZ_ASSERT(!ins->mir()->fallible());

      masm.rshiftPtrArithmetic(Imm32(rhs), lhs, output);
    }
  } else {
    Register rhs = ToRegister(ins->rhs());

    Label done, bail;
    MOZ_ASSERT(ins->mir()->fallible());

    masm.movePtr(lhs, output);

    // 0n >> x == 0n
    masm.branchPtr(Assembler::Equal, lhs, Imm32(0), &done);

    // x >> -DigitBits == x << DigitBits, which exceeds pointer-sized storage.
    masm.branchPtr(Assembler::LessThanOrEqual, rhs,
                   Imm32(-int32_t(BigInt::DigitBits)), &bail);

    // x >> DigitBits is either 0n or -1n.
    Label shift;
    masm.branchPtr(Assembler::LessThan, rhs, Imm32(BigInt::DigitBits), &shift);
    {
      masm.rshiftPtrArithmetic(Imm32(BigInt::DigitBits - 1), output);
      masm.jump(&done);
    }
    masm.bind(&shift);

    // Move |rhs| into the designated shift register.
    masm.movePtr(rhs, tempShift);

    // |x >> -y| is computed as |x << y|.
    Label rightShift;
    masm.branchPtr(Assembler::GreaterThanOrEqual, rhs, Imm32(0), &rightShift);
    {
      masm.negPtr(tempShift);
      masm.lshiftPtr(tempShift, output);

      // Check for overflow: ((lhs << rhs) >> rhs) == lhs.
      masm.movePtr(output, temp);
      masm.rshiftPtrArithmetic(tempShift, temp);
      masm.branchPtr(Assembler::NotEqual, temp, lhs, &bail);

      masm.jump(&done);
    }
    masm.bind(&rightShift);

    masm.rshiftPtrArithmetic(tempShift, output);

    masm.bind(&done);
    bailoutFrom(&bail, ins->snapshot());
  }
}

void CodeGenerator::visitBigIntPtrBitNot(LBigIntPtrBitNot* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.movePtr(input, output);
  masm.notPtr(output);
}

void CodeGenerator::visitInt32ToStringWithBase(LInt32ToStringWithBase* lir) {
  Register input = ToRegister(lir->input());
  RegisterOrInt32 base = ToRegisterOrInt32(lir->base());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  bool lowerCase = lir->mir()->lowerCase();

  using Fn = JSLinearString* (*)(JSContext*, int32_t, int32_t, bool);
  if (base.is<Register>()) {
    auto* ool = oolCallVM<Fn, js::Int32ToStringWithBase<CanGC>>(
        lir, ArgList(input, base.as<Register>(), Imm32(lowerCase)),
        StoreRegisterTo(output));

    LiveRegisterSet liveRegs = liveVolatileRegs(lir);
    masm.loadInt32ToStringWithBase(input, base.as<Register>(), output, temp0,
                                   temp1, gen->runtime->staticStrings(),
                                   liveRegs, lowerCase, ool->entry());
    masm.bind(ool->rejoin());
  } else {
    auto* ool = oolCallVM<Fn, js::Int32ToStringWithBase<CanGC>>(
        lir, ArgList(input, Imm32(base.as<int32_t>()), Imm32(lowerCase)),
        StoreRegisterTo(output));

    masm.loadInt32ToStringWithBase(input, base.as<int32_t>(), output, temp0,
                                   temp1, gen->runtime->staticStrings(),
                                   lowerCase, ool->entry());
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitNumberParseInt(LNumberParseInt* lir) {
  Register string = ToRegister(lir->string());
  Register radix = ToRegister(lir->radix());
  ValueOperand output = ToOutValue(lir);
  Register temp = ToRegister(lir->temp0());

#ifdef DEBUG
  Label ok;
  masm.branch32(Assembler::Equal, radix, Imm32(0), &ok);
  masm.branch32(Assembler::Equal, radix, Imm32(10), &ok);
  masm.assumeUnreachable("radix must be 0 or 10 for indexed value fast path");
  masm.bind(&ok);
#endif

  // Use indexed value as fast path if possible.
  Label vmCall, done;
  masm.loadStringIndexValue(string, temp, &vmCall);
  masm.tagValue(JSVAL_TYPE_INT32, temp, output);
  masm.jump(&done);
  {
    masm.bind(&vmCall);

    pushArg(radix);
    pushArg(string);

    using Fn = bool (*)(JSContext*, HandleString, int32_t, MutableHandleValue);
    callVM<Fn, js::NumberParseInt>(lir);
  }
  masm.bind(&done);
}

void CodeGenerator::visitDoubleParseInt(LDoubleParseInt* lir) {
  FloatRegister number = ToFloatRegister(lir->number());
  Register output = ToRegister(lir->output());
  FloatRegister temp = ToFloatRegister(lir->temp0());

  Label bail;
  masm.branchDouble(Assembler::DoubleUnordered, number, number, &bail);
  masm.branchTruncateDoubleToInt32(number, output, &bail);

  Label ok;
  masm.branch32(Assembler::NotEqual, output, Imm32(0), &ok);
  {
    // Accept both +0 and -0 and return 0.
    masm.loadConstantDouble(0.0, temp);
    masm.branchDouble(Assembler::DoubleEqual, number, temp, &ok);

    // Fail if a non-zero input is in the exclusive range (-1, 1.0e-6).
    masm.loadConstantDouble(DOUBLE_DECIMAL_IN_SHORTEST_LOW, temp);
    masm.branchDouble(Assembler::DoubleLessThan, number, temp, &bail);
  }
  masm.bind(&ok);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitFloor(LFloor* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.floorDoubleToInt32(input, output, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitFloorF(LFloorF* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.floorFloat32ToInt32(input, output, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitCeil(LCeil* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.ceilDoubleToInt32(input, output, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitCeilF(LCeilF* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.ceilFloat32ToInt32(input, output, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitRound(LRound* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  FloatRegister temp = ToFloatRegister(lir->temp0());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.roundDoubleToInt32(input, output, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitRoundF(LRoundF* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  FloatRegister temp = ToFloatRegister(lir->temp0());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.roundFloat32ToInt32(input, output, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitTrunc(LTrunc* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.truncDoubleToInt32(input, output, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitTruncF(LTruncF* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.truncFloat32ToInt32(input, output, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitCompareS(LCompareS* lir) {
  JSOp op = lir->mir()->jsop();
  Register left = ToRegister(lir->left());
  Register right = ToRegister(lir->right());
  Register output = ToRegister(lir->output());

  OutOfLineCode* ool = nullptr;

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  if (op == JSOp::Eq || op == JSOp::StrictEq) {
    ool = oolCallVM<Fn, jit::StringsEqual<EqualityKind::Equal>>(
        lir, ArgList(left, right), StoreRegisterTo(output));
  } else if (op == JSOp::Ne || op == JSOp::StrictNe) {
    ool = oolCallVM<Fn, jit::StringsEqual<EqualityKind::NotEqual>>(
        lir, ArgList(left, right), StoreRegisterTo(output));
  } else if (op == JSOp::Lt) {
    ool = oolCallVM<Fn, jit::StringsCompare<ComparisonKind::LessThan>>(
        lir, ArgList(left, right), StoreRegisterTo(output));
  } else if (op == JSOp::Le) {
    // Push the operands in reverse order for JSOp::Le:
    // - |left <= right| is implemented as |right >= left|.
    ool =
        oolCallVM<Fn, jit::StringsCompare<ComparisonKind::GreaterThanOrEqual>>(
            lir, ArgList(right, left), StoreRegisterTo(output));
  } else if (op == JSOp::Gt) {
    // Push the operands in reverse order for JSOp::Gt:
    // - |left > right| is implemented as |right < left|.
    ool = oolCallVM<Fn, jit::StringsCompare<ComparisonKind::LessThan>>(
        lir, ArgList(right, left), StoreRegisterTo(output));
  } else {
    MOZ_ASSERT(op == JSOp::Ge);
    ool =
        oolCallVM<Fn, jit::StringsCompare<ComparisonKind::GreaterThanOrEqual>>(
            lir, ArgList(left, right), StoreRegisterTo(output));
  }

  masm.compareStrings(op, left, right, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCompareSInline(LCompareSInline* lir) {
  JSOp op = lir->mir()->jsop();
  MOZ_ASSERT(IsEqualityOp(op));

  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  const JSLinearString* str = lir->constant();
  MOZ_ASSERT(str->length() > 0);

  OutOfLineCode* ool = nullptr;

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  if (op == JSOp::Eq || op == JSOp::StrictEq) {
    ool = oolCallVM<Fn, jit::StringsEqual<EqualityKind::Equal>>(
        lir, ArgList(ImmGCPtr(str), input), StoreRegisterTo(output));
  } else {
    MOZ_ASSERT(op == JSOp::Ne || op == JSOp::StrictNe);
    ool = oolCallVM<Fn, jit::StringsEqual<EqualityKind::NotEqual>>(
        lir, ArgList(ImmGCPtr(str), input), StoreRegisterTo(output));
  }

  Label compareChars;
  {
    Label notPointerEqual;

    // If operands point to the same instance, the strings are trivially equal.
    masm.branchPtr(Assembler::NotEqual, input, ImmGCPtr(str), &notPointerEqual);
    masm.move32(Imm32(op == JSOp::Eq || op == JSOp::StrictEq), output);
    masm.jump(ool->rejoin());

    masm.bind(&notPointerEqual);

    Label setNotEqualResult;

    if (str->isAtom()) {
      // Atoms cannot be equal to each other if they point to different strings.
      Imm32 atomBit(JSString::ATOM_BIT);
      masm.branchTest32(Assembler::NonZero,
                        Address(input, JSString::offsetOfFlags()), atomBit,
                        &setNotEqualResult);
    }

    if (str->hasTwoByteChars()) {
      // Pure two-byte strings can't be equal to Latin-1 strings.
      JS::AutoCheckCannotGC nogc;
      if (!mozilla::IsUtf16Latin1(str->twoByteRange(nogc))) {
        masm.branchLatin1String(input, &setNotEqualResult);
      }
    }

    // Strings of different length can never be equal.
    masm.branch32(Assembler::NotEqual,
                  Address(input, JSString::offsetOfLength()),
                  Imm32(str->length()), &setNotEqualResult);

    if (str->isAtom()) {
      Label forwardedPtrEqual;
      masm.tryFastAtomize(input, output, output, &compareChars);

      // We now have two atoms. Just check pointer equality.
      masm.branchPtr(Assembler::Equal, output, ImmGCPtr(str),
                     &forwardedPtrEqual);

      masm.move32(Imm32(op == JSOp::Ne || op == JSOp::StrictNe), output);
      masm.jump(ool->rejoin());

      masm.bind(&forwardedPtrEqual);
      masm.move32(Imm32(op == JSOp::Eq || op == JSOp::StrictEq), output);
      masm.jump(ool->rejoin());
    } else {
      masm.jump(&compareChars);
    }

    masm.bind(&setNotEqualResult);
    masm.move32(Imm32(op == JSOp::Ne || op == JSOp::StrictNe), output);
    masm.jump(ool->rejoin());
  }

  masm.bind(&compareChars);

  // Load the input string's characters.
  Register stringChars = output;
  masm.loadStringCharsForCompare(input, str, stringChars, ool->entry());

  // Start comparing character by character.
  masm.compareStringChars(op, stringChars, str, output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCompareSSingle(LCompareSSingle* lir) {
  JSOp op = lir->jsop();
  MOZ_ASSERT(IsRelationalOp(op));

  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  const JSLinearString* str = lir->constant();
  MOZ_ASSERT(str->length() == 1);

  char16_t ch = str->latin1OrTwoByteChar(0);

  masm.movePtr(input, temp);

  // Check if the string is empty.
  Label compareLength;
  masm.branch32(Assembler::Equal, Address(temp, JSString::offsetOfLength()),
                Imm32(0), &compareLength);

  // The first character is in the left-most rope child.
  Label notRope;
  masm.branchIfNotRope(temp, &notRope);
  {
    // Unwind ropes at the start if possible.
    Label unwindRope;
    masm.bind(&unwindRope);
    masm.loadRopeLeftChild(temp, output);
    masm.movePtr(output, temp);

#ifdef DEBUG
    Label notEmpty;
    masm.branch32(Assembler::NotEqual,
                  Address(temp, JSString::offsetOfLength()), Imm32(0),
                  &notEmpty);
    masm.assumeUnreachable("rope children are non-empty");
    masm.bind(&notEmpty);
#endif

    // Otherwise keep unwinding ropes.
    masm.branchIfRope(temp, &unwindRope);
  }
  masm.bind(&notRope);

  // Load the first character into |output|.
  auto loadFirstChar = [&](auto encoding) {
    masm.loadStringChars(temp, output, encoding);
    masm.loadChar(Address(output, 0), output, encoding);
  };

  Label done;
  if (ch <= JSString::MAX_LATIN1_CHAR) {
    // Handle both encodings when the search character is Latin-1.
    Label twoByte, compare;
    masm.branchTwoByteString(temp, &twoByte);

    loadFirstChar(CharEncoding::Latin1);
    masm.jump(&compare);

    masm.bind(&twoByte);
    loadFirstChar(CharEncoding::TwoByte);

    masm.bind(&compare);
  } else {
    // The search character is a two-byte character, so it can't be equal to any
    // character of a Latin-1 string.
    masm.move32(Imm32(int32_t(op == JSOp::Lt || op == JSOp::Le)), output);
    masm.branchLatin1String(temp, &done);

    loadFirstChar(CharEncoding::TwoByte);
  }

  // Compare the string length when the search character is equal to the
  // input's first character.
  masm.branch32(Assembler::Equal, output, Imm32(ch), &compareLength);

  // Otherwise compute the result and jump to the end.
  masm.cmp32Set(JSOpToCondition(op, /* isSigned = */ false), output, Imm32(ch),
                output);
  masm.jump(&done);

  // Compare the string length to compute the overall result.
  masm.bind(&compareLength);
  masm.cmp32Set(JSOpToCondition(op, /* isSigned = */ false),
                Address(input, JSString::offsetOfLength()), Imm32(1), output);

  masm.bind(&done);
}

void CodeGenerator::visitCompareBigInt(LCompareBigInt* lir) {
  JSOp op = lir->mir()->jsop();
  Register left = ToRegister(lir->left());
  Register right = ToRegister(lir->right());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register output = ToRegister(lir->output());

  Label notSame;
  Label compareSign;
  Label compareLength;
  Label compareDigit;

  Label* notSameSign;
  Label* notSameLength;
  Label* notSameDigit;
  if (IsEqualityOp(op)) {
    notSameSign = &notSame;
    notSameLength = &notSame;
    notSameDigit = &notSame;
  } else {
    notSameSign = &compareSign;
    notSameLength = &compareLength;
    notSameDigit = &compareDigit;
  }

  masm.equalBigInts(left, right, temp0, temp1, temp2, output, notSameSign,
                    notSameLength, notSameDigit);

  Label done;
  masm.move32(Imm32(op == JSOp::Eq || op == JSOp::StrictEq || op == JSOp::Le ||
                    op == JSOp::Ge),
              output);
  masm.jump(&done);

  if (IsEqualityOp(op)) {
    masm.bind(&notSame);
    masm.move32(Imm32(op == JSOp::Ne || op == JSOp::StrictNe), output);
  } else {
    Label invertWhenNegative;

    // There are two cases when sign(left) != sign(right):
    // 1. sign(left) = positive and sign(right) = negative,
    // 2. or the dual case with reversed signs.
    //
    // For case 1, |left| <cmp> |right| is true for cmp=Gt or cmp=Ge and false
    // for cmp=Lt or cmp=Le. Initialize the result for case 1 and handle case 2
    // with |invertWhenNegative|.
    masm.bind(&compareSign);
    masm.move32(Imm32(op == JSOp::Gt || op == JSOp::Ge), output);
    masm.jump(&invertWhenNegative);

    // For sign(left) = sign(right) and len(digits(left)) != len(digits(right)),
    // we have to consider the two cases:
    // 1. len(digits(left)) < len(digits(right))
    // 2. len(digits(left)) > len(digits(right))
    //
    // For |left| <cmp> |right| with cmp=Lt:
    // Assume both BigInts are positive, then |left < right| is true for case 1
    // and false for case 2. When both are negative, the result is reversed.
    //
    // The other comparison operators can be handled similarly.
    //
    // |temp0| holds the digits length of the right-hand side operand.
    masm.bind(&compareLength);
    masm.cmp32Set(JSOpToCondition(op, /* isSigned = */ false),
                  Address(left, BigInt::offsetOfLength()), temp0, output);
    masm.jump(&invertWhenNegative);

    // Similar to the case above, compare the current digit to determine the
    // overall comparison result.
    //
    // |temp1| points to the current digit of the left-hand side operand.
    // |output| holds the current digit of the right-hand side operand.
    masm.bind(&compareDigit);
    masm.cmpPtrSet(JSOpToCondition(op, /* isSigned = */ false),
                   Address(temp1, 0), output, output);

    Label nonNegative;
    masm.bind(&invertWhenNegative);
    masm.branchIfBigIntIsNonNegative(left, &nonNegative);
    masm.xor32(Imm32(1), output);
    masm.bind(&nonNegative);
  }

  masm.bind(&done);
}

void CodeGenerator::visitCompareBigIntInt32(LCompareBigIntInt32* lir) {
  JSOp op = lir->mir()->jsop();
  Register left = ToRegister(lir->left());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToTempRegisterOrInvalid(lir->temp1());
  Register output = ToRegister(lir->output());

  Label ifTrue, ifFalse;
  if (lir->right()->isConstant()) {
    MOZ_ASSERT(temp1 == InvalidReg);

    Imm32 right = Imm32(ToInt32(lir->right()));
    masm.compareBigIntAndInt32(op, left, right, temp0, &ifTrue, &ifFalse);
  } else {
    MOZ_ASSERT(temp1 != InvalidReg);

    Register right = ToRegister(lir->right());
    masm.compareBigIntAndInt32(op, left, right, temp0, temp1, &ifTrue,
                               &ifFalse);
  }

  Label done;
  masm.bind(&ifFalse);
  masm.move32(Imm32(0), output);
  masm.jump(&done);
  masm.bind(&ifTrue);
  masm.move32(Imm32(1), output);
  masm.bind(&done);
}

void CodeGenerator::visitCompareBigIntInt32AndBranch(
    LCompareBigIntInt32AndBranch* lir) {
  JSOp op = lir->cmpMir()->jsop();
  Register left = ToRegister(lir->left());
  Register temp1 = ToRegister(lir->temp0());
  Register temp2 = ToTempRegisterOrInvalid(lir->temp1());

  Label* ifTrue = getJumpLabelForBranch(lir->ifTrue());
  Label* ifFalse = getJumpLabelForBranch(lir->ifFalse());

  // compareBigIntAndInt32 falls through to the false case. If the next block
  // is the true case, negate the comparison so we can fall through.
  if (isNextBlock(lir->ifTrue()->lir())) {
    op = NegateCompareOp(op);
    std::swap(ifTrue, ifFalse);
  }

  if (lir->right()->isConstant()) {
    MOZ_ASSERT(temp2 == InvalidReg);

    Imm32 right = Imm32(ToInt32(lir->right()));
    masm.compareBigIntAndInt32(op, left, right, temp1, ifTrue, ifFalse);
  } else {
    MOZ_ASSERT(temp2 != InvalidReg);

    Register right = ToRegister(lir->right());
    masm.compareBigIntAndInt32(op, left, right, temp1, temp2, ifTrue, ifFalse);
  }

  if (!isNextBlock(lir->ifTrue()->lir())) {
    jumpToBlock(lir->ifFalse());
  }
}

void CodeGenerator::visitCompareBigIntDouble(LCompareBigIntDouble* lir) {
  JSOp op = lir->mir()->jsop();
  Register left = ToRegister(lir->left());
  FloatRegister right = ToFloatRegister(lir->right());
  Register output = ToRegister(lir->output());

  masm.setupAlignedABICall();

  // Push the operands in reverse order for JSOp::Le and JSOp::Gt:
  // - |left <= right| is implemented as |right >= left|.
  // - |left > right| is implemented as |right < left|.
  if (op == JSOp::Le || op == JSOp::Gt) {
    masm.passABIArg(right, ABIType::Float64);
    masm.passABIArg(left);
  } else {
    masm.passABIArg(left);
    masm.passABIArg(right, ABIType::Float64);
  }

  using FnBigIntNumber = bool (*)(BigInt*, double);
  using FnNumberBigInt = bool (*)(double, BigInt*);
  switch (op) {
    case JSOp::Eq: {
      masm.callWithABI<FnBigIntNumber,
                       jit::BigIntNumberEqual<EqualityKind::Equal>>();
      break;
    }
    case JSOp::Ne: {
      masm.callWithABI<FnBigIntNumber,
                       jit::BigIntNumberEqual<EqualityKind::NotEqual>>();
      break;
    }
    case JSOp::Lt: {
      masm.callWithABI<FnBigIntNumber,
                       jit::BigIntNumberCompare<ComparisonKind::LessThan>>();
      break;
    }
    case JSOp::Gt: {
      masm.callWithABI<FnNumberBigInt,
                       jit::NumberBigIntCompare<ComparisonKind::LessThan>>();
      break;
    }
    case JSOp::Le: {
      masm.callWithABI<
          FnNumberBigInt,
          jit::NumberBigIntCompare<ComparisonKind::GreaterThanOrEqual>>();
      break;
    }
    case JSOp::Ge: {
      masm.callWithABI<
          FnBigIntNumber,
          jit::BigIntNumberCompare<ComparisonKind::GreaterThanOrEqual>>();
      break;
    }
    default:
      MOZ_CRASH("unhandled op");
  }

  masm.storeCallBoolResult(output);
}

void CodeGenerator::visitCompareBigIntString(LCompareBigIntString* lir) {
  JSOp op = lir->mir()->jsop();
  Register left = ToRegister(lir->left());
  Register right = ToRegister(lir->right());

  // Push the operands in reverse order for JSOp::Le and JSOp::Gt:
  // - |left <= right| is implemented as |right >= left|.
  // - |left > right| is implemented as |right < left|.
  if (op == JSOp::Le || op == JSOp::Gt) {
    pushArg(left);
    pushArg(right);
  } else {
    pushArg(right);
    pushArg(left);
  }

  using FnBigIntString =
      bool (*)(JSContext*, HandleBigInt, HandleString, bool*);
  using FnStringBigInt =
      bool (*)(JSContext*, HandleString, HandleBigInt, bool*);

  switch (op) {
    case JSOp::Eq: {
      callVM<FnBigIntString, InstantiatedBigIntStringEqual>(lir);
      break;
    }
    case JSOp::Ne: {
      callVM<FnBigIntString, InstantiatedBigIntStringNotEqual>(lir);
      break;
    }
    case JSOp::Lt: {
      constexpr auto LessThan = ComparisonKind::LessThan;
      callVM<FnBigIntString, InstantiatedBigIntStringLessThan>(lir);
      break;
    }
    case JSOp::Gt: {
      constexpr auto LessThan = ComparisonKind::LessThan;
      callVM<FnStringBigInt, InstantiatedStringBigIntLessThan>(lir);
      break;
    }
    case JSOp::Le: {
      constexpr auto GreaterThanOrEqual = ComparisonKind::GreaterThanOrEqual;
      callVM<FnStringBigInt, InstantiatedStringBigIntGreaterThanOrEqual>(lir);
      break;
    }
    case JSOp::Ge: {
      constexpr auto GreaterThanOrEqual = ComparisonKind::GreaterThanOrEqual;
      callVM<FnBigIntString, InstantiatedBigIntStringGreaterThanOrEqual>(lir);
      break;
    }
    default:
      MOZ_CRASH("Unexpected compare op");
  }
}

void CodeGenerator::visitIsNullOrLikeUndefinedV(LIsNullOrLikeUndefinedV* lir) {
  MOZ_ASSERT(lir->mir()->compareType() == MCompare::Compare_Undefined ||
             lir->mir()->compareType() == MCompare::Compare_Null);

  JSOp op = lir->mir()->jsop();
  MOZ_ASSERT(IsLooseEqualityOp(op));

  ValueOperand value = ToValue(lir->value());
  Register output = ToRegister(lir->output());

  bool intact = hasSeenObjectEmulateUndefinedFuseIntactAndDependencyNoted();
  if (!intact) {
    auto* ool = new (alloc()) OutOfLineTestObjectWithLabels();
    addOutOfLineCode(ool, lir->mir());

    Label* nullOrLikeUndefined = ool->label1();
    Label* notNullOrLikeUndefined = ool->label2();

    {
      ScratchTagScope tag(masm, value);
      masm.splitTagForTest(value, tag);

      masm.branchTestNull(Assembler::Equal, tag, nullOrLikeUndefined);
      masm.branchTestUndefined(Assembler::Equal, tag, nullOrLikeUndefined);

      // Check whether it's a truthy object or a falsy object that emulates
      // undefined.
      masm.branchTestObject(Assembler::NotEqual, tag, notNullOrLikeUndefined);
    }

    Register objreg =
        masm.extractObject(value, ToTempUnboxRegister(lir->temp0()));
    branchTestObjectEmulatesUndefined(objreg, nullOrLikeUndefined,
                                      notNullOrLikeUndefined, output, ool);
    // fall through

    Label done;

    // It's not null or undefined, and if it's an object it doesn't
    // emulate undefined, so it's not like undefined.
    masm.move32(Imm32(op == JSOp::Ne), output);
    masm.jump(&done);

    masm.bind(nullOrLikeUndefined);
    masm.move32(Imm32(op == JSOp::Eq), output);

    // Both branches meet here.
    masm.bind(&done);
  } else {
    Label nullOrUndefined, notNullOrLikeUndefined;
#if defined(DEBUG) || defined(FUZZING)
    Register objreg = Register::Invalid();
#endif
    {
      ScratchTagScope tag(masm, value);
      masm.splitTagForTest(value, tag);

      masm.branchTestNull(Assembler::Equal, tag, &nullOrUndefined);
      masm.branchTestUndefined(Assembler::Equal, tag, &nullOrUndefined);

#if defined(DEBUG) || defined(FUZZING)
      // Check whether it's a truthy object or a falsy object that emulates
      // undefined.
      masm.branchTestObject(Assembler::NotEqual, tag, &notNullOrLikeUndefined);
      objreg = masm.extractObject(value, ToTempUnboxRegister(lir->temp0()));
#endif
    }

#if defined(DEBUG) || defined(FUZZING)
    assertObjectDoesNotEmulateUndefined(objreg, output, lir->mir());
    masm.bind(&notNullOrLikeUndefined);
#endif

    Label done;

    // It's not null or undefined, and if it's an object it doesn't
    // emulate undefined.
    masm.move32(Imm32(op == JSOp::Ne), output);
    masm.jump(&done);

    masm.bind(&nullOrUndefined);
    masm.move32(Imm32(op == JSOp::Eq), output);

    // Both branches meet here.
    masm.bind(&done);
  }
}

void CodeGenerator::visitIsNullOrLikeUndefinedAndBranchV(
    LIsNullOrLikeUndefinedAndBranchV* lir) {
  MOZ_ASSERT(lir->cmpMir()->compareType() == MCompare::Compare_Undefined ||
             lir->cmpMir()->compareType() == MCompare::Compare_Null);

  JSOp op = lir->cmpMir()->jsop();
  MOZ_ASSERT(IsLooseEqualityOp(op));

  ValueOperand value = ToValue(lir->value());

  MBasicBlock* ifTrue = lir->ifTrue();
  MBasicBlock* ifFalse = lir->ifFalse();

  if (op == JSOp::Ne) {
    // Swap branches.
    std::swap(ifTrue, ifFalse);
  }

  bool intact = hasSeenObjectEmulateUndefinedFuseIntactAndDependencyNoted();

  Label* ifTrueLabel = getJumpLabelForBranch(ifTrue);
  Label* ifFalseLabel = getJumpLabelForBranch(ifFalse);

  {
    ScratchTagScope tag(masm, value);
    masm.splitTagForTest(value, tag);

    masm.branchTestNull(Assembler::Equal, tag, ifTrueLabel);
    masm.branchTestUndefined(Assembler::Equal, tag, ifTrueLabel);

    masm.branchTestObject(Assembler::NotEqual, tag, ifFalseLabel);
  }

  bool extractObject = !intact;
#if defined(DEBUG) || defined(FUZZING)
  // always extract objreg if we're in debug and
  // assertObjectDoesNotEmulateUndefined;
  extractObject = true;
#endif

  Register objreg = Register::Invalid();
  Register scratch = ToRegister(lir->temp0());
  if (extractObject) {
    objreg = masm.extractObject(value, ToTempUnboxRegister(lir->temp1()));
  }
  if (!intact) {
    // Objects that emulate undefined are loosely equal to null/undefined.
    OutOfLineTestObject* ool = new (alloc()) OutOfLineTestObject();
    addOutOfLineCode(ool, lir->cmpMir());
    testObjectEmulatesUndefined(objreg, ifTrueLabel, ifFalseLabel, scratch,
                                ool);
  } else {
    assertObjectDoesNotEmulateUndefined(objreg, scratch, lir->cmpMir());
    // Bug 1874905. This would be nice to optimize out at the MIR level.
    masm.jump(ifFalseLabel);
  }
}

void CodeGenerator::visitIsNullOrLikeUndefinedT(LIsNullOrLikeUndefinedT* lir) {
  MOZ_ASSERT(lir->mir()->compareType() == MCompare::Compare_Undefined ||
             lir->mir()->compareType() == MCompare::Compare_Null);
  MOZ_ASSERT(lir->mir()->lhs()->type() == MIRType::Object);

  bool intact = hasSeenObjectEmulateUndefinedFuseIntactAndDependencyNoted();
  JSOp op = lir->mir()->jsop();
  Register output = ToRegister(lir->output());
  Register objreg = ToRegister(lir->input());
  if (!intact) {
    MOZ_ASSERT(IsLooseEqualityOp(op),
               "Strict equality should have been folded");

    auto* ool = new (alloc()) OutOfLineTestObjectWithLabels();
    addOutOfLineCode(ool, lir->mir());

    Label* emulatesUndefined = ool->label1();
    Label* doesntEmulateUndefined = ool->label2();

    branchTestObjectEmulatesUndefined(objreg, emulatesUndefined,
                                      doesntEmulateUndefined, output, ool);

    Label done;

    masm.move32(Imm32(op == JSOp::Ne), output);
    masm.jump(&done);

    masm.bind(emulatesUndefined);
    masm.move32(Imm32(op == JSOp::Eq), output);
    masm.bind(&done);
  } else {
    assertObjectDoesNotEmulateUndefined(objreg, output, lir->mir());
    masm.move32(Imm32(op == JSOp::Ne), output);
  }
}

void CodeGenerator::visitIsNullOrLikeUndefinedAndBranchT(
    LIsNullOrLikeUndefinedAndBranchT* lir) {
  MOZ_ASSERT(lir->cmpMir()->compareType() == MCompare::Compare_Undefined ||
             lir->cmpMir()->compareType() == MCompare::Compare_Null);
  MOZ_ASSERT(lir->cmpMir()->lhs()->type() == MIRType::Object);

  bool intact = hasSeenObjectEmulateUndefinedFuseIntactAndDependencyNoted();

  JSOp op = lir->cmpMir()->jsop();
  MOZ_ASSERT(IsLooseEqualityOp(op), "Strict equality should have been folded");

  MBasicBlock* ifTrue = lir->ifTrue();
  MBasicBlock* ifFalse = lir->ifFalse();

  if (op == JSOp::Ne) {
    // Swap branches.
    std::swap(ifTrue, ifFalse);
  }

  Register input = ToRegister(lir->value());
  Register scratch = ToRegister(lir->temp0());
  Label* ifTrueLabel = getJumpLabelForBranch(ifTrue);
  Label* ifFalseLabel = getJumpLabelForBranch(ifFalse);

  if (intact) {
    // Bug 1874905. Ideally branches like this would be optimized out.
    assertObjectDoesNotEmulateUndefined(input, scratch, lir->mir());
    masm.jump(ifFalseLabel);
  } else {
    auto* ool = new (alloc()) OutOfLineTestObject();
    addOutOfLineCode(ool, lir->cmpMir());

    // Objects that emulate undefined are loosely equal to null/undefined.
    testObjectEmulatesUndefined(input, ifTrueLabel, ifFalseLabel, scratch, ool);
  }
}

void CodeGenerator::visitIsNull(LIsNull* lir) {
  MCompare::CompareType compareType = lir->mir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Null);

  JSOp op = lir->mir()->jsop();
  MOZ_ASSERT(IsStrictEqualityOp(op));

  ValueOperand value = ToValue(lir->value());
  Register output = ToRegister(lir->output());

  Assembler::Condition cond = JSOpToCondition(compareType, op);
  masm.testNullSet(cond, value, output);
}

void CodeGenerator::visitIsUndefined(LIsUndefined* lir) {
  MCompare::CompareType compareType = lir->mir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Undefined);

  JSOp op = lir->mir()->jsop();
  MOZ_ASSERT(IsStrictEqualityOp(op));

  ValueOperand value = ToValue(lir->value());
  Register output = ToRegister(lir->output());

  Assembler::Condition cond = JSOpToCondition(compareType, op);
  masm.testUndefinedSet(cond, value, output);
}

void CodeGenerator::visitIsNullAndBranch(LIsNullAndBranch* lir) {
  MCompare::CompareType compareType = lir->cmpMir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Null);

  JSOp op = lir->cmpMir()->jsop();
  MOZ_ASSERT(IsStrictEqualityOp(op));

  ValueOperand value = ToValue(lir->value());

  Assembler::Condition cond = JSOpToCondition(compareType, op);

  MBasicBlock* ifTrue = lir->ifTrue();
  MBasicBlock* ifFalse = lir->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    masm.branchTestNull(cond, value, getJumpLabelForBranch(ifTrue));
  } else {
    masm.branchTestNull(Assembler::InvertCondition(cond), value,
                        getJumpLabelForBranch(ifFalse));
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitIsUndefinedAndBranch(LIsUndefinedAndBranch* lir) {
  MCompare::CompareType compareType = lir->cmpMir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Undefined);

  JSOp op = lir->cmpMir()->jsop();
  MOZ_ASSERT(IsStrictEqualityOp(op));

  ValueOperand value = ToValue(lir->value());

  Assembler::Condition cond = JSOpToCondition(compareType, op);

  MBasicBlock* ifTrue = lir->ifTrue();
  MBasicBlock* ifFalse = lir->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    masm.branchTestUndefined(cond, value, getJumpLabelForBranch(ifTrue));
  } else {
    masm.branchTestUndefined(Assembler::InvertCondition(cond), value,
                             getJumpLabelForBranch(ifFalse));
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitSameValueDouble(LSameValueDouble* lir) {
  FloatRegister left = ToFloatRegister(lir->left());
  FloatRegister right = ToFloatRegister(lir->right());
  FloatRegister temp = ToFloatRegister(lir->temp0());
  Register output = ToRegister(lir->output());

  masm.sameValueDouble(left, right, temp, output);
}

void CodeGenerator::visitSameValue(LSameValue* lir) {
  ValueOperand lhs = ToValue(lir->left());
  ValueOperand rhs = ToValue(lir->right());
  Register output = ToRegister(lir->output());

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue, bool*);
  OutOfLineCode* ool =
      oolCallVM<Fn, SameValue>(lir, ArgList(lhs, rhs), StoreRegisterTo(output));

  // First check to see if the values have identical bits.
  // This is correct for SameValue because SameValue(NaN,NaN) is true,
  // and SameValue(0,-0) is false.
  masm.branch64(Assembler::NotEqual, lhs.toRegister64(), rhs.toRegister64(),
                ool->entry());
  masm.move32(Imm32(1), output);

  // If this fails, call SameValue.
  masm.bind(ool->rejoin());
}

void CodeGenerator::emitConcat(LInstruction* lir, Register lhs, Register rhs,
                               Register output) {
  using Fn =
      JSString* (*)(JSContext*, HandleString, HandleString, js::gc::Heap);
  OutOfLineCode* ool = oolCallVM<Fn, ConcatStrings<CanGC>>(
      lir, ArgList(lhs, rhs, static_cast<Imm32>(int32_t(gc::Heap::Default))),
      StoreRegisterTo(output));

  JitCode* stringConcatStub =
      snapshot_->getZoneStub(JitZone::StubKind::StringConcat);
  masm.call(stringConcatStub);
  masm.branchTestPtr(Assembler::Zero, output, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitConcat(LConcat* lir) {
  Register lhs = ToRegister(lir->lhs());
  Register rhs = ToRegister(lir->rhs());

  Register output = ToRegister(lir->output());

  MOZ_ASSERT(lhs == CallTempReg0);
  MOZ_ASSERT(rhs == CallTempReg1);
  MOZ_ASSERT(ToRegister(lir->temp0()) == CallTempReg0);
  MOZ_ASSERT(ToRegister(lir->temp1()) == CallTempReg1);
  MOZ_ASSERT(ToRegister(lir->temp2()) == CallTempReg2);
  MOZ_ASSERT(ToRegister(lir->temp3()) == CallTempReg3);
  MOZ_ASSERT(ToRegister(lir->temp4()) == CallTempReg4);
  MOZ_ASSERT(output == CallTempReg5);

  emitConcat(lir, lhs, rhs, output);
}

static void CopyStringChars(MacroAssembler& masm, Register to, Register from,
                            Register len, Register byteOpScratch,
                            CharEncoding fromEncoding, CharEncoding toEncoding,
                            size_t maximumLength = SIZE_MAX) {
  // Copy |len| char16_t code units from |from| to |to|. Assumes len > 0
  // (checked below in debug builds), and when done |to| must point to the
  // next available char.

#ifdef DEBUG
  Label ok;
  masm.branch32(Assembler::GreaterThan, len, Imm32(0), &ok);
  masm.assumeUnreachable("Length should be greater than 0.");
  masm.bind(&ok);

  if (maximumLength != SIZE_MAX) {
    MOZ_ASSERT(maximumLength <= INT32_MAX, "maximum length fits into int32");

    Label ok;
    masm.branchPtr(Assembler::BelowOrEqual, len, Imm32(maximumLength), &ok);
    masm.assumeUnreachable("Length should not exceed maximum length.");
    masm.bind(&ok);
  }
#endif

  MOZ_ASSERT_IF(toEncoding == CharEncoding::Latin1,
                fromEncoding == CharEncoding::Latin1);

  size_t fromWidth =
      fromEncoding == CharEncoding::Latin1 ? sizeof(char) : sizeof(char16_t);
  size_t toWidth =
      toEncoding == CharEncoding::Latin1 ? sizeof(char) : sizeof(char16_t);

  // Try to copy multiple characters at once when both encoding are equal.
  if (fromEncoding == toEncoding) {
    constexpr size_t ptrWidth = sizeof(uintptr_t);

    // Copy |width| bytes and then adjust |from| and |to|.
    auto copyCharacters = [&](size_t width) {
      static_assert(ptrWidth <= 8, "switch handles only up to eight bytes");

      switch (width) {
        case 1:
          masm.load8ZeroExtend(Address(from, 0), byteOpScratch);
          masm.store8(byteOpScratch, Address(to, 0));
          break;
        case 2:
          masm.load16ZeroExtend(Address(from, 0), byteOpScratch);
          masm.store16(byteOpScratch, Address(to, 0));
          break;
        case 4:
          masm.load32(Address(from, 0), byteOpScratch);
          masm.store32(byteOpScratch, Address(to, 0));
          break;
        case 8:
          MOZ_ASSERT(width == ptrWidth);
          masm.loadPtr(Address(from, 0), byteOpScratch);
          masm.storePtr(byteOpScratch, Address(to, 0));
          break;
      }

      masm.addPtr(Imm32(width), from);
      masm.addPtr(Imm32(width), to);
    };

    // First align |len| to pointer width.
    Label done;
    for (size_t width = fromWidth; width < ptrWidth; width *= 2) {
      // Number of characters which fit into |width| bytes.
      size_t charsPerWidth = width / fromWidth;

      if (charsPerWidth < maximumLength) {
        Label next;
        masm.branchTest32(Assembler::Zero, len, Imm32(charsPerWidth), &next);

        copyCharacters(width);

        masm.branchSub32(Assembler::Zero, Imm32(charsPerWidth), len, &done);
        masm.bind(&next);
      } else if (charsPerWidth == maximumLength) {
        copyCharacters(width);
        masm.sub32(Imm32(charsPerWidth), len);
      }
    }

    size_t maxInlineLength;
    if (fromEncoding == CharEncoding::Latin1) {
      maxInlineLength = JSFatInlineString::MAX_LENGTH_LATIN1;
    } else {
      maxInlineLength = JSFatInlineString::MAX_LENGTH_TWO_BYTE;
    }

    // Number of characters which fit into a single register.
    size_t charsPerPtr = ptrWidth / fromWidth;

    // Unroll small loops.
    constexpr size_t unrollLoopLimit = 3;
    size_t loopCount = std::min(maxInlineLength, maximumLength) / charsPerPtr;

#ifdef JS_64BIT
    static constexpr size_t latin1MaxInlineByteLength =
        JSFatInlineString::MAX_LENGTH_LATIN1 * sizeof(char);
    static constexpr size_t twoByteMaxInlineByteLength =
        JSFatInlineString::MAX_LENGTH_TWO_BYTE * sizeof(char16_t);

    // |unrollLoopLimit| should be large enough to allow loop unrolling on
    // 64-bit targets.
    static_assert(latin1MaxInlineByteLength / ptrWidth == unrollLoopLimit,
                  "Latin-1 loops are unrolled on 64-bit");
    static_assert(twoByteMaxInlineByteLength / ptrWidth == unrollLoopLimit,
                  "Two-byte loops are unrolled on 64-bit");
#endif

    if (loopCount <= unrollLoopLimit) {
      Label labels[unrollLoopLimit];

      // Check up front how many characters can be copied.
      for (size_t i = 1; i < loopCount; i++) {
        masm.branch32(Assembler::Below, len, Imm32((i + 1) * charsPerPtr),
                      &labels[i]);
      }

      // Generate the unrolled loop body.
      for (size_t i = loopCount; i > 0; i--) {
        copyCharacters(ptrWidth);
        masm.sub32(Imm32(charsPerPtr), len);

        // Jump target for the previous length check.
        if (i != 1) {
          masm.bind(&labels[i - 1]);
        }
      }
    } else {
      Label start;
      masm.bind(&start);
      copyCharacters(ptrWidth);
      masm.branchSub32(Assembler::NonZero, Imm32(charsPerPtr), len, &start);
    }

    masm.bind(&done);
  } else {
    Label start;
    masm.bind(&start);
    masm.loadChar(Address(from, 0), byteOpScratch, fromEncoding);
    masm.storeChar(byteOpScratch, Address(to, 0), toEncoding);
    masm.addPtr(Imm32(fromWidth), from);
    masm.addPtr(Imm32(toWidth), to);
    masm.branchSub32(Assembler::NonZero, Imm32(1), len, &start);
  }
}

static void CopyStringChars(MacroAssembler& masm, Register to, Register from,
                            Register len, Register byteOpScratch,
                            CharEncoding encoding, size_t maximumLength) {
  CopyStringChars(masm, to, from, len, byteOpScratch, encoding, encoding,
                  maximumLength);
}

static void CopyStringCharsMaybeInflate(MacroAssembler& masm, Register input,
                                        Register destChars, Register temp1,
                                        Register temp2) {
  // destChars is TwoByte and input is a Latin1 or TwoByte string, so we may
  // have to inflate.

  Label isLatin1, done;
  masm.loadStringLength(input, temp1);
  masm.branchLatin1String(input, &isLatin1);
  {
    masm.loadStringChars(input, temp2, CharEncoding::TwoByte);
    masm.movePtr(temp2, input);
    CopyStringChars(masm, destChars, input, temp1, temp2,
                    CharEncoding::TwoByte);
    masm.jump(&done);
  }
  masm.bind(&isLatin1);
  {
    masm.loadStringChars(input, temp2, CharEncoding::Latin1);
    masm.movePtr(temp2, input);
    CopyStringChars(masm, destChars, input, temp1, temp2, CharEncoding::Latin1,
                    CharEncoding::TwoByte);
  }
  masm.bind(&done);
}

static void AllocateThinOrFatInlineString(MacroAssembler& masm, Register output,
                                          Register length, Register temp,
                                          gc::Heap initialStringHeap,
                                          Label* failure,
                                          CharEncoding encoding) {
#ifdef DEBUG
  size_t maxInlineLength;
  if (encoding == CharEncoding::Latin1) {
    maxInlineLength = JSFatInlineString::MAX_LENGTH_LATIN1;
  } else {
    maxInlineLength = JSFatInlineString::MAX_LENGTH_TWO_BYTE;
  }

  Label ok;
  masm.branch32(Assembler::BelowOrEqual, length, Imm32(maxInlineLength), &ok);
  masm.assumeUnreachable("string length too large to be allocated as inline");
  masm.bind(&ok);
#endif

  size_t maxThinInlineLength;
  if (encoding == CharEncoding::Latin1) {
    maxThinInlineLength = JSThinInlineString::MAX_LENGTH_LATIN1;
  } else {
    maxThinInlineLength = JSThinInlineString::MAX_LENGTH_TWO_BYTE;
  }

  Label isFat, allocDone;
  masm.branch32(Assembler::Above, length, Imm32(maxThinInlineLength), &isFat);
  {
    uint32_t flags = JSString::INIT_THIN_INLINE_FLAGS;
    if (encoding == CharEncoding::Latin1) {
      flags |= JSString::LATIN1_CHARS_BIT;
    }
    masm.newGCString(output, temp, initialStringHeap, failure);
    masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
    masm.jump(&allocDone);
  }
  masm.bind(&isFat);
  {
    uint32_t flags = JSString::INIT_FAT_INLINE_FLAGS;
    if (encoding == CharEncoding::Latin1) {
      flags |= JSString::LATIN1_CHARS_BIT;
    }
    masm.newGCFatInlineString(output, temp, initialStringHeap, failure);
    masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
  }
  masm.bind(&allocDone);

  // Store length.
  masm.store32(length, Address(output, JSString::offsetOfLength()));
}

static void ConcatInlineString(MacroAssembler& masm, Register lhs, Register rhs,
                               Register output, Register temp1, Register temp2,
                               Register temp3, gc::Heap initialStringHeap,
                               Label* failure, CharEncoding encoding) {
  JitSpew(JitSpew_Codegen, "# Emitting ConcatInlineString (encoding=%s)",
          (encoding == CharEncoding::Latin1 ? "Latin-1" : "Two-Byte"));

  // State: result length in temp2.

  // Ensure both strings are linear.
  masm.branchIfRope(lhs, failure);
  masm.branchIfRope(rhs, failure);

  // Allocate a JSThinInlineString or JSFatInlineString.
  AllocateThinOrFatInlineString(masm, output, temp2, temp1, initialStringHeap,
                                failure, encoding);

  // Load chars pointer in temp2.
  masm.loadInlineStringCharsForStore(output, temp2);

  auto copyChars = [&](Register src) {
    if (encoding == CharEncoding::TwoByte) {
      CopyStringCharsMaybeInflate(masm, src, temp2, temp1, temp3);
    } else {
      masm.loadStringLength(src, temp3);
      masm.loadStringChars(src, temp1, CharEncoding::Latin1);
      masm.movePtr(temp1, src);
      CopyStringChars(masm, temp2, src, temp3, temp1, CharEncoding::Latin1);
    }
  };

  // Copy lhs chars. Note that this advances temp2 to point to the next
  // char. This also clobbers the lhs register.
  copyChars(lhs);

  // Copy rhs chars. Clobbers the rhs register.
  copyChars(rhs);
}

void CodeGenerator::visitSubstr(LSubstr* lir) {
  Register string = ToRegister(lir->string());
  Register begin = ToRegister(lir->begin());
  Register length = ToRegister(lir->length());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp2());

  // On x86 there are not enough registers. In that case reuse the string
  // register as temporary.
  Register temp1 =
      lir->temp1()->isBogusTemp() ? string : ToRegister(lir->temp1());

  size_t maximumLength = SIZE_MAX;

  Range* range = lir->mir()->length()->range();
  if (range && range->hasInt32UpperBound()) {
    MOZ_ASSERT(range->upper() >= 0);
    maximumLength = size_t(range->upper());
  }

  static_assert(JSThinInlineString::MAX_LENGTH_TWO_BYTE <=
                JSThinInlineString::MAX_LENGTH_LATIN1);

  static_assert(JSFatInlineString::MAX_LENGTH_TWO_BYTE <=
                JSFatInlineString::MAX_LENGTH_LATIN1);

  bool tryFatInlineOrDependent =
      maximumLength > JSThinInlineString::MAX_LENGTH_TWO_BYTE;
  bool tryDependent = maximumLength > JSFatInlineString::MAX_LENGTH_TWO_BYTE;

#ifdef DEBUG
  if (maximumLength != SIZE_MAX) {
    Label ok;
    masm.branch32(Assembler::BelowOrEqual, length, Imm32(maximumLength), &ok);
    masm.assumeUnreachable("length should not exceed maximum length");
    masm.bind(&ok);
  }
#endif

  Label nonZero, nonInput;

  // For every edge case use the C++ variant.
  // Note: we also use this upon allocation failure in newGCString and
  // newGCFatInlineString. To squeeze out even more performance those failures
  // can be handled by allocate in ool code and returning to jit code to fill
  // in all data.
  using Fn = JSString* (*)(JSContext* cx, HandleString str, int32_t begin,
                           int32_t len);
  OutOfLineCode* ool = oolCallVM<Fn, SubstringKernel>(
      lir, ArgList(string, begin, length), StoreRegisterTo(output));
  Label* slowPath = ool->entry();
  Label* done = ool->rejoin();

  // Zero length, return emptystring.
  masm.branchTest32(Assembler::NonZero, length, length, &nonZero);
  const JSAtomState& names = gen->runtime->names();
  masm.movePtr(ImmGCPtr(names.empty_), output);
  masm.jump(done);

  // Substring from 0..|str.length|, return str.
  masm.bind(&nonZero);
  masm.branch32(Assembler::NotEqual,
                Address(string, JSString::offsetOfLength()), length, &nonInput);
#ifdef DEBUG
  {
    Label ok;
    masm.branchTest32(Assembler::Zero, begin, begin, &ok);
    masm.assumeUnreachable("length == str.length implies begin == 0");
    masm.bind(&ok);
  }
#endif
  masm.movePtr(string, output);
  masm.jump(done);

  // Use slow path for ropes.
  masm.bind(&nonInput);
  masm.branchIfRope(string, slowPath);

  // Optimize one and two character strings.
  Label nonStatic;
  masm.branch32(Assembler::Above, length, Imm32(2), &nonStatic);
  {
    Label loadLengthOne, loadLengthTwo;

    auto loadChars = [&](CharEncoding encoding, bool fallthru) {
      size_t size = encoding == CharEncoding::Latin1 ? sizeof(JS::Latin1Char)
                                                     : sizeof(char16_t);

      masm.loadStringChars(string, temp0, encoding);
      masm.loadChar(temp0, begin, temp2, encoding);
      masm.branch32(Assembler::Equal, length, Imm32(1), &loadLengthOne);
      masm.loadChar(temp0, begin, temp0, encoding, int32_t(size));
      if (!fallthru) {
        masm.jump(&loadLengthTwo);
      }
    };

    Label isLatin1;
    masm.branchLatin1String(string, &isLatin1);
    loadChars(CharEncoding::TwoByte, /* fallthru = */ false);

    masm.bind(&isLatin1);
    loadChars(CharEncoding::Latin1, /* fallthru = */ true);

    // Try to load a length-two static string.
    masm.bind(&loadLengthTwo);
    masm.lookupStaticString(temp2, temp0, output, gen->runtime->staticStrings(),
                            &nonStatic);
    masm.jump(done);

    // Try to load a length-one static string.
    masm.bind(&loadLengthOne);
    masm.lookupStaticString(temp2, output, gen->runtime->staticStrings(),
                            &nonStatic);
    masm.jump(done);
  }
  masm.bind(&nonStatic);

  // Allocate either a JSThinInlineString or JSFatInlineString, or jump to
  // notInline if we need a dependent string.
  Label notInline;
  {
    static_assert(JSThinInlineString::MAX_LENGTH_LATIN1 <
                  JSFatInlineString::MAX_LENGTH_LATIN1);
    static_assert(JSThinInlineString::MAX_LENGTH_TWO_BYTE <
                  JSFatInlineString::MAX_LENGTH_TWO_BYTE);

    // Use temp2 to store the JS(Thin|Fat)InlineString flags. This avoids having
    // duplicate newGCString/newGCFatInlineString codegen for Latin1 vs TwoByte
    // strings.

    Label allocFat, allocDone;
    if (tryFatInlineOrDependent) {
      Label isLatin1, allocThin;
      masm.branchLatin1String(string, &isLatin1);
      {
        if (tryDependent) {
          masm.branch32(Assembler::Above, length,
                        Imm32(JSFatInlineString::MAX_LENGTH_TWO_BYTE),
                        &notInline);
        }
        masm.move32(Imm32(0), temp2);
        masm.branch32(Assembler::Above, length,
                      Imm32(JSThinInlineString::MAX_LENGTH_TWO_BYTE),
                      &allocFat);
        masm.jump(&allocThin);
      }

      masm.bind(&isLatin1);
      {
        if (tryDependent) {
          masm.branch32(Assembler::Above, length,
                        Imm32(JSFatInlineString::MAX_LENGTH_LATIN1),
                        &notInline);
        }
        masm.move32(Imm32(JSString::LATIN1_CHARS_BIT), temp2);
        masm.branch32(Assembler::Above, length,
                      Imm32(JSThinInlineString::MAX_LENGTH_LATIN1), &allocFat);
      }

      masm.bind(&allocThin);
    } else {
      masm.load32(Address(string, JSString::offsetOfFlags()), temp2);
      masm.and32(Imm32(JSString::LATIN1_CHARS_BIT), temp2);
    }

    {
      masm.newGCString(output, temp0, initialStringHeap(), slowPath);
      masm.or32(Imm32(JSString::INIT_THIN_INLINE_FLAGS), temp2);
    }

    if (tryFatInlineOrDependent) {
      masm.jump(&allocDone);

      masm.bind(&allocFat);
      {
        masm.newGCFatInlineString(output, temp0, initialStringHeap(), slowPath);
        masm.or32(Imm32(JSString::INIT_FAT_INLINE_FLAGS), temp2);
      }

      masm.bind(&allocDone);
    }

    masm.store32(temp2, Address(output, JSString::offsetOfFlags()));
    masm.store32(length, Address(output, JSString::offsetOfLength()));

    auto initializeInlineString = [&](CharEncoding encoding) {
      masm.loadStringChars(string, temp0, encoding);
      masm.addToCharPtr(temp0, begin, encoding);
      if (temp1 == string) {
        masm.push(string);
      }
      masm.loadInlineStringCharsForStore(output, temp1);
      CopyStringChars(masm, temp1, temp0, length, temp2, encoding,
                      maximumLength);
      masm.loadStringLength(output, length);
      if (temp1 == string) {
        masm.pop(string);
      }
    };

    Label isInlineLatin1;
    masm.branchTest32(Assembler::NonZero, temp2,
                      Imm32(JSString::LATIN1_CHARS_BIT), &isInlineLatin1);
    initializeInlineString(CharEncoding::TwoByte);
    masm.jump(done);

    masm.bind(&isInlineLatin1);
    initializeInlineString(CharEncoding::Latin1);
  }

  // Handle other cases with a DependentString.
  if (tryDependent) {
    masm.jump(done);

    masm.bind(&notInline);
    masm.newGCString(output, temp0, gen->initialStringHeap(), slowPath);
    masm.store32(length, Address(output, JSString::offsetOfLength()));

    // Note: no post barrier is needed because the dependent string is either
    // allocated in the nursery or both strings are tenured (if nursery strings
    // are disabled for this zone).
    EmitInitDependentStringBase(masm, output, string, temp0, temp2,
                                /* needsPostBarrier = */ false);

    auto initializeDependentString = [&](CharEncoding encoding) {
      uint32_t flags = JSString::INIT_DEPENDENT_FLAGS;
      if (encoding == CharEncoding::Latin1) {
        flags |= JSString::LATIN1_CHARS_BIT;
      }
      masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
      masm.loadNonInlineStringChars(string, temp0, encoding);
      masm.addToCharPtr(temp0, begin, encoding);
      masm.storeNonInlineStringChars(temp0, output);
    };

    Label isLatin1;
    masm.branchLatin1String(string, &isLatin1);
    initializeDependentString(CharEncoding::TwoByte);
    masm.jump(done);

    masm.bind(&isLatin1);
    initializeDependentString(CharEncoding::Latin1);
  }

  masm.bind(done);
}

JitCode* JitZone::generateStringConcatStub(JSContext* cx) {
  JitSpew(JitSpew_Codegen, "# Emitting StringConcat stub");

  TempAllocator temp(&cx->tempLifoAlloc());
  JitContext jcx(cx);
  StackMacroAssembler masm(cx, temp);
  AutoCreatedBy acb(masm, "JitZone::generateStringConcatStub");

  Register lhs = CallTempReg0;
  Register rhs = CallTempReg1;
  Register temp1 = CallTempReg2;
  Register temp2 = CallTempReg3;
  Register temp3 = CallTempReg4;
  Register output = CallTempReg5;

  Label failure;
#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif
  masm.Push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  // If lhs is empty, return rhs.
  Label leftEmpty;
  masm.loadStringLength(lhs, temp1);
  masm.branchTest32(Assembler::Zero, temp1, temp1, &leftEmpty);

  // If rhs is empty, return lhs.
  Label rightEmpty;
  masm.loadStringLength(rhs, temp2);
  masm.branchTest32(Assembler::Zero, temp2, temp2, &rightEmpty);

  masm.add32(temp1, temp2);

  // Check if we can use a JSInlineString. The result is a Latin1 string if
  // lhs and rhs are both Latin1, so we AND the flags.
  Label isInlineTwoByte, isInlineLatin1;
  masm.load32(Address(lhs, JSString::offsetOfFlags()), temp1);
  masm.and32(Address(rhs, JSString::offsetOfFlags()), temp1);

  Label isLatin1, notInline;
  masm.branchTest32(Assembler::NonZero, temp1,
                    Imm32(JSString::LATIN1_CHARS_BIT), &isLatin1);
  {
    masm.branch32(Assembler::BelowOrEqual, temp2,
                  Imm32(JSFatInlineString::MAX_LENGTH_TWO_BYTE),
                  &isInlineTwoByte);
    masm.jump(&notInline);
  }
  masm.bind(&isLatin1);
  {
    masm.branch32(Assembler::BelowOrEqual, temp2,
                  Imm32(JSFatInlineString::MAX_LENGTH_LATIN1), &isInlineLatin1);
  }
  masm.bind(&notInline);

  // Keep AND'ed flags in temp1.

  // Ensure result length <= JSString::MAX_LENGTH.
  masm.branch32(Assembler::Above, temp2, Imm32(JSString::MAX_LENGTH), &failure);

  // Allocate a new rope, guaranteed to be in the nursery if initialStringHeap
  // == gc::Heap::Default. (As a result, no post barriers are needed below.)
  masm.newGCString(output, temp3, initialStringHeap, &failure);

  // Store rope length and flags. temp1 still holds the result of AND'ing the
  // lhs and rhs flags, so we just have to clear the other flags to get our rope
  // flags (Latin1 if both lhs and rhs are Latin1).
  static_assert(JSString::INIT_ROPE_FLAGS == 0,
                "Rope type flags must have no bits set");
  masm.and32(Imm32(JSString::LATIN1_CHARS_BIT), temp1);
  masm.store32(temp1, Address(output, JSString::offsetOfFlags()));
  masm.store32(temp2, Address(output, JSString::offsetOfLength()));

  // Store left and right nodes.
  masm.storeRopeChildren(lhs, rhs, output);
  masm.pop(FramePointer);
  masm.ret();

  masm.bind(&leftEmpty);
  masm.mov(rhs, output);
  masm.pop(FramePointer);
  masm.ret();

  masm.bind(&rightEmpty);
  masm.mov(lhs, output);
  masm.pop(FramePointer);
  masm.ret();

  masm.bind(&isInlineTwoByte);
  ConcatInlineString(masm, lhs, rhs, output, temp1, temp2, temp3,
                     initialStringHeap, &failure, CharEncoding::TwoByte);
  masm.pop(FramePointer);
  masm.ret();

  masm.bind(&isInlineLatin1);
  ConcatInlineString(masm, lhs, rhs, output, temp1, temp2, temp3,
                     initialStringHeap, &failure, CharEncoding::Latin1);
  masm.pop(FramePointer);
  masm.ret();

  masm.bind(&failure);
  masm.movePtr(ImmPtr(nullptr), output);
  masm.pop(FramePointer);
  masm.ret();

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);

  CollectPerfSpewerJitCodeProfile(code, "StringConcatStub");
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, "StringConcatStub");
#endif

  return code;
}

void JitRuntime::generateLazyLinkStub(MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateLazyLinkStub");

  lazyLinkStubOffset_ = startTrampolineCode(masm);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif
  masm.Push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  Register temp0 = regs.takeAny();
  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();

  masm.loadJSContext(temp0);
  masm.enterFakeExitFrame(temp0, temp2, ExitFrameType::LazyLink);
  masm.moveStackPtrTo(temp1);

  using Fn = uint8_t* (*)(JSContext* cx, LazyLinkExitFrameLayout* frame);
  masm.setupUnalignedABICall(temp2);
  masm.passABIArg(temp0);
  masm.passABIArg(temp1);
  masm.callWithABI<Fn, LazyLinkTopActivation>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  // Discard exit frame and restore frame pointer.
  masm.leaveExitFrame(0);
  masm.pop(FramePointer);

#ifdef JS_USE_LINK_REGISTER
  // Restore the return address such that the emitPrologue function of the
  // CodeGenerator can push it back on the stack with pushReturnAddress.
  masm.popReturnAddress();
#endif
  masm.jump(ReturnReg);
}

void JitRuntime::generateInterpreterStub(MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateInterpreterStub");

  interpreterStubOffset_ = startTrampolineCode(masm);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif
  masm.Push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  Register temp0 = regs.takeAny();
  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();

  masm.loadJSContext(temp0);
  masm.enterFakeExitFrame(temp0, temp2, ExitFrameType::InterpreterStub);
  masm.moveStackPtrTo(temp1);

  using Fn = bool (*)(JSContext* cx, InterpreterStubExitFrameLayout* frame);
  masm.setupUnalignedABICall(temp2);
  masm.passABIArg(temp0);
  masm.passABIArg(temp1);
  masm.callWithABI<Fn, InvokeFromInterpreterStub>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  masm.branchIfFalseBool(ReturnReg, masm.failureLabel());

  // Discard exit frame and restore frame pointer.
  masm.leaveExitFrame(0);
  masm.pop(FramePointer);

  // InvokeFromInterpreterStub stores the return value in argv[0], where the
  // caller stored |this|. Subtract |sizeof(void*)| for the frame pointer we
  // just popped.
  masm.loadValue(Address(masm.getStackPointer(),
                         JitFrameLayout::offsetOfThis() - sizeof(void*)),
                 JSReturnOperand);
  masm.ret();
}

void JitRuntime::generateDoubleToInt32ValueStub(MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateDoubleToInt32ValueStub");
  doubleToInt32ValueStubOffset_ = startTrampolineCode(masm);

  Label done;
  masm.branchTestDouble(Assembler::NotEqual, R0, &done);

  masm.unboxDouble(R0, FloatReg0);
  masm.convertDoubleToInt32(FloatReg0, R1.scratchReg(), &done,
                            /* negativeZeroCheck = */ false);
  masm.tagValue(JSVAL_TYPE_INT32, R1.scratchReg(), R0);

  masm.bind(&done);
  masm.abiret();
}

void CodeGenerator::visitLinearizeString(LLinearizeString* lir) {
  Register str = ToRegister(lir->string());
  Register output = ToRegister(lir->output());

  using Fn = JSLinearString* (*)(JSContext*, JSString*);
  auto* ool = oolCallVM<Fn, jit::LinearizeForCharAccess>(
      lir, ArgList(str), StoreRegisterTo(output));

  masm.branchIfRope(str, ool->entry());

  masm.movePtr(str, output);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitLinearizeForCharAccess(LLinearizeForCharAccess* lir) {
  Register str = ToRegister(lir->string());
  Register index = ToRegister(lir->index());
  Register output = ToRegister(lir->output());

  using Fn = JSLinearString* (*)(JSContext*, JSString*);
  auto* ool = oolCallVM<Fn, jit::LinearizeForCharAccess>(
      lir, ArgList(str), StoreRegisterTo(output));

  masm.branchIfNotCanLoadStringChar(str, index, output, ool->entry());

  masm.movePtr(str, output);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitLinearizeForCodePointAccess(
    LLinearizeForCodePointAccess* lir) {
  Register str = ToRegister(lir->string());
  Register index = ToRegister(lir->index());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  using Fn = JSLinearString* (*)(JSContext*, JSString*);
  auto* ool = oolCallVM<Fn, jit::LinearizeForCharAccess>(
      lir, ArgList(str), StoreRegisterTo(output));

  masm.branchIfNotCanLoadStringCodePoint(str, index, output, temp,
                                         ool->entry());

  masm.movePtr(str, output);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitToRelativeStringIndex(LToRelativeStringIndex* lir) {
  Register index = ToRegister(lir->index());
  Register length = ToRegister(lir->length());
  Register output = ToRegister(lir->output());

  masm.move32(Imm32(0), output);
  masm.cmp32Move32(Assembler::LessThan, index, Imm32(0), length, output);
  masm.add32(index, output);
}

void CodeGenerator::visitCharCodeAt(LCharCodeAt* lir) {
  Register str = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  using Fn = bool (*)(JSContext*, HandleString, int32_t, uint32_t*);

  if (lir->index()->isBogus()) {
    auto* ool = oolCallVM<Fn, jit::CharCodeAt>(lir, ArgList(str, Imm32(0)),
                                               StoreRegisterTo(output));
    masm.loadStringChar(str, 0, output, temp0, temp1, ool->entry());
    masm.bind(ool->rejoin());
  } else {
    Register index = ToRegister(lir->index());

    auto* ool = oolCallVM<Fn, jit::CharCodeAt>(lir, ArgList(str, index),
                                               StoreRegisterTo(output));
    masm.loadStringChar(str, index, output, temp0, temp1, ool->entry());
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitCharCodeAtOrNegative(LCharCodeAtOrNegative* lir) {
  Register str = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  using Fn = bool (*)(JSContext*, HandleString, int32_t, uint32_t*);

  // Return -1 for out-of-bounds access.
  masm.move32(Imm32(-1), output);

  if (lir->index()->isBogus()) {
    auto* ool = oolCallVM<Fn, jit::CharCodeAt>(lir, ArgList(str, Imm32(0)),
                                               StoreRegisterTo(output));

    masm.branch32(Assembler::Equal, Address(str, JSString::offsetOfLength()),
                  Imm32(0), ool->rejoin());
    masm.loadStringChar(str, 0, output, temp0, temp1, ool->entry());
    masm.bind(ool->rejoin());
  } else {
    Register index = ToRegister(lir->index());

    auto* ool = oolCallVM<Fn, jit::CharCodeAt>(lir, ArgList(str, index),
                                               StoreRegisterTo(output));

    masm.spectreBoundsCheck32(index, Address(str, JSString::offsetOfLength()),
                              temp0, ool->rejoin());
    masm.loadStringChar(str, index, output, temp0, temp1, ool->entry());
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitCodePointAt(LCodePointAt* lir) {
  Register str = ToRegister(lir->string());
  Register index = ToRegister(lir->index());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  using Fn = bool (*)(JSContext*, HandleString, int32_t, uint32_t*);
  auto* ool = oolCallVM<Fn, jit::CodePointAt>(lir, ArgList(str, index),
                                              StoreRegisterTo(output));

  masm.loadStringCodePoint(str, index, output, temp0, temp1, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCodePointAtOrNegative(LCodePointAtOrNegative* lir) {
  Register str = ToRegister(lir->string());
  Register index = ToRegister(lir->index());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  using Fn = bool (*)(JSContext*, HandleString, int32_t, uint32_t*);
  auto* ool = oolCallVM<Fn, jit::CodePointAt>(lir, ArgList(str, index),
                                              StoreRegisterTo(output));

  // Return -1 for out-of-bounds access.
  masm.move32(Imm32(-1), output);

  masm.spectreBoundsCheck32(index, Address(str, JSString::offsetOfLength()),
                            temp0, ool->rejoin());
  masm.loadStringCodePoint(str, index, output, temp0, temp1, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNegativeToNaN(LNegativeToNaN* lir) {
  Register input = ToRegister(lir->input());
  ValueOperand output = ToOutValue(lir);

  masm.tagValue(JSVAL_TYPE_INT32, input, output);

  Label done;
  masm.branchTest32(Assembler::NotSigned, input, input, &done);
  masm.moveValue(JS::NaNValue(), output);
  masm.bind(&done);
}

void CodeGenerator::visitNegativeToUndefined(LNegativeToUndefined* lir) {
  Register input = ToRegister(lir->input());
  ValueOperand output = ToOutValue(lir);

  masm.tagValue(JSVAL_TYPE_INT32, input, output);

  Label done;
  masm.branchTest32(Assembler::NotSigned, input, input, &done);
  masm.moveValue(JS::UndefinedValue(), output);
  masm.bind(&done);
}

void CodeGenerator::visitFromCharCode(LFromCharCode* lir) {
  Register code = ToRegister(lir->code());
  Register output = ToRegister(lir->output());

  using Fn = JSLinearString* (*)(JSContext*, int32_t);
  auto* ool = oolCallVM<Fn, js::StringFromCharCode>(lir, ArgList(code),
                                                    StoreRegisterTo(output));

  // OOL path if code >= UNIT_STATIC_LIMIT.
  masm.lookupStaticString(code, output, gen->runtime->staticStrings(),
                          ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitFromCharCodeEmptyIfNegative(
    LFromCharCodeEmptyIfNegative* lir) {
  Register code = ToRegister(lir->code());
  Register output = ToRegister(lir->output());

  using Fn = JSLinearString* (*)(JSContext*, int32_t);
  auto* ool = oolCallVM<Fn, js::StringFromCharCode>(lir, ArgList(code),
                                                    StoreRegisterTo(output));

  // Return the empty string for negative inputs.
  const JSAtomState& names = gen->runtime->names();
  masm.movePtr(ImmGCPtr(names.empty_), output);
  masm.branchTest32(Assembler::Signed, code, code, ool->rejoin());

  // OOL path if code >= UNIT_STATIC_LIMIT.
  masm.lookupStaticString(code, output, gen->runtime->staticStrings(),
                          ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitFromCharCodeUndefinedIfNegative(
    LFromCharCodeUndefinedIfNegative* lir) {
  Register code = ToRegister(lir->code());
  ValueOperand output = ToOutValue(lir);
  Register temp = output.scratchReg();

  using Fn = JSLinearString* (*)(JSContext*, int32_t);
  auto* ool = oolCallVM<Fn, js::StringFromCharCode>(lir, ArgList(code),
                                                    StoreRegisterTo(temp));

  // Return |undefined| for negative inputs.
  Label done;
  masm.moveValue(UndefinedValue(), output);
  masm.branchTest32(Assembler::Signed, code, code, &done);

  // OOL path if code >= UNIT_STATIC_LIMIT.
  masm.lookupStaticString(code, temp, gen->runtime->staticStrings(),
                          ool->entry());

  masm.bind(ool->rejoin());
  masm.tagValue(JSVAL_TYPE_STRING, temp, output);

  masm.bind(&done);
}

void CodeGenerator::visitFromCodePoint(LFromCodePoint* lir) {
  Register codePoint = ToRegister(lir->codePoint());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  LSnapshot* snapshot = lir->snapshot();

  // The OOL path is only taken when we can't allocate the inline string.
  using Fn = JSLinearString* (*)(JSContext*, char32_t);
  auto* ool = oolCallVM<Fn, js::StringFromCodePoint>(lir, ArgList(codePoint),
                                                     StoreRegisterTo(output));

  Label isTwoByte;
  Label* done = ool->rejoin();

  static_assert(
      StaticStrings::UNIT_STATIC_LIMIT - 1 == JSString::MAX_LATIN1_CHAR,
      "Latin-1 strings can be loaded from static strings");

  {
    masm.lookupStaticString(codePoint, output, gen->runtime->staticStrings(),
                            &isTwoByte);
    masm.jump(done);
  }
  masm.bind(&isTwoByte);
  {
    // Use a bailout if the input is not a valid code point, because
    // MFromCodePoint is movable and it'd be observable when a moved
    // fromCodePoint throws an exception before its actual call site.
    bailoutCmp32(Assembler::Above, codePoint, Imm32(unicode::NonBMPMax),
                 snapshot);

    // Allocate a JSThinInlineString.
    {
      static_assert(JSThinInlineString::MAX_LENGTH_TWO_BYTE >= 2,
                    "JSThinInlineString can hold a supplementary code point");

      uint32_t flags = JSString::INIT_THIN_INLINE_FLAGS;
      masm.newGCString(output, temp0, gen->initialStringHeap(), ool->entry());
      masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
    }

    Label isSupplementary;
    masm.branch32(Assembler::AboveOrEqual, codePoint, Imm32(unicode::NonBMPMin),
                  &isSupplementary);
    {
      // Store length.
      masm.store32(Imm32(1), Address(output, JSString::offsetOfLength()));

      // Load chars pointer in temp0.
      masm.loadInlineStringCharsForStore(output, temp0);

      masm.store16(codePoint, Address(temp0, 0));

      masm.jump(done);
    }
    masm.bind(&isSupplementary);
    {
      // Store length.
      masm.store32(Imm32(2), Address(output, JSString::offsetOfLength()));

      // Load chars pointer in temp0.
      masm.loadInlineStringCharsForStore(output, temp0);

      // Inlined unicode::LeadSurrogate(uint32_t).
      masm.rshift32(Imm32(10), codePoint, temp1);
      masm.add32(Imm32(unicode::LeadSurrogateMin - (unicode::NonBMPMin >> 10)),
                 temp1);

      masm.store16(temp1, Address(temp0, 0));

      // Inlined unicode::TrailSurrogate(uint32_t).
      masm.and32(Imm32(0x3FF), codePoint, temp1);
      masm.or32(Imm32(unicode::TrailSurrogateMin), temp1);

      masm.store16(temp1, Address(temp0, sizeof(char16_t)));
    }
  }

  masm.bind(done);
}

void CodeGenerator::visitStringIncludes(LStringIncludes* lir) {
  pushArg(ToRegister(lir->searchString()));
  pushArg(ToRegister(lir->string()));

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  callVM<Fn, js::StringIncludes>(lir);
}

template <typename LIns>
static void CallStringMatch(MacroAssembler& masm, LIns* lir, OutOfLineCode* ool,
                            LiveRegisterSet volatileRegs) {
  Register string = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  Register tempLength = ToRegister(lir->temp0());
  Register tempChars = ToRegister(lir->temp1());
  Register maybeTempPat = ToTempRegisterOrInvalid(lir->temp2());

  const JSLinearString* searchString = lir->searchString();
  size_t length = searchString->length();
  MOZ_ASSERT(length == 1 || length == 2);

  // The additional temp register is only needed when searching for two
  // pattern characters.
  MOZ_ASSERT_IF(length == 2, maybeTempPat != InvalidReg);

  if constexpr (std::is_same_v<LIns, LStringIncludesSIMD>) {
    masm.move32(Imm32(0), output);
  } else {
    masm.move32(Imm32(-1), output);
  }

  masm.loadStringLength(string, tempLength);

  // Can't be a substring when the string is smaller than the search string.
  Label done;
  masm.branch32(Assembler::Below, tempLength, Imm32(length), ool->rejoin());

  bool searchStringIsPureTwoByte = false;
  if (searchString->hasTwoByteChars()) {
    JS::AutoCheckCannotGC nogc;
    searchStringIsPureTwoByte =
        !mozilla::IsUtf16Latin1(searchString->twoByteRange(nogc));
  }

  // Pure two-byte strings can't occur in a Latin-1 string.
  if (searchStringIsPureTwoByte) {
    masm.branchLatin1String(string, ool->rejoin());
  }

  // Slow path when we need to linearize the string.
  masm.branchIfRope(string, ool->entry());

  Label restoreVolatile;

  auto callMatcher = [&](CharEncoding encoding) {
    masm.loadStringChars(string, tempChars, encoding);

    LiveGeneralRegisterSet liveRegs;
    if constexpr (std::is_same_v<LIns, LStringIndexOfSIMD>) {
      // Save |tempChars| to compute the result index.
      liveRegs.add(tempChars);

#ifdef DEBUG
      // Save |tempLength| in debug-mode for assertions.
      liveRegs.add(tempLength);
#endif

      // Exclude non-volatile registers.
      liveRegs.set() = GeneralRegisterSet::Intersect(
          liveRegs.set(), GeneralRegisterSet::Volatile());

      masm.PushRegsInMask(liveRegs);
    }

    if (length == 1) {
      char16_t pat = searchString->latin1OrTwoByteChar(0);
      MOZ_ASSERT_IF(encoding == CharEncoding::Latin1,
                    pat <= JSString::MAX_LATIN1_CHAR);

      masm.move32(Imm32(pat), output);

      masm.setupAlignedABICall();
      masm.passABIArg(tempChars);
      masm.passABIArg(output);
      masm.passABIArg(tempLength);
      if (encoding == CharEncoding::Latin1) {
        using Fn = const char* (*)(const char*, char, size_t);
        masm.callWithABI<Fn, mozilla::SIMD::memchr8>(
            ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);
      } else {
        using Fn = const char16_t* (*)(const char16_t*, char16_t, size_t);
        masm.callWithABI<Fn, mozilla::SIMD::memchr16>(
            ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);
      }
    } else {
      char16_t pat0 = searchString->latin1OrTwoByteChar(0);
      MOZ_ASSERT_IF(encoding == CharEncoding::Latin1,
                    pat0 <= JSString::MAX_LATIN1_CHAR);

      char16_t pat1 = searchString->latin1OrTwoByteChar(1);
      MOZ_ASSERT_IF(encoding == CharEncoding::Latin1,
                    pat1 <= JSString::MAX_LATIN1_CHAR);

      masm.move32(Imm32(pat0), output);
      masm.move32(Imm32(pat1), maybeTempPat);

      masm.setupAlignedABICall();
      masm.passABIArg(tempChars);
      masm.passABIArg(output);
      masm.passABIArg(maybeTempPat);
      masm.passABIArg(tempLength);
      if (encoding == CharEncoding::Latin1) {
        using Fn = const char* (*)(const char*, char, char, size_t);
        masm.callWithABI<Fn, mozilla::SIMD::memchr2x8>(
            ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);
      } else {
        using Fn =
            const char16_t* (*)(const char16_t*, char16_t, char16_t, size_t);
        masm.callWithABI<Fn, mozilla::SIMD::memchr2x16>(
            ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);
      }
    }

    masm.storeCallPointerResult(output);

    // Convert to string index for `indexOf`.
    if constexpr (std::is_same_v<LIns, LStringIndexOfSIMD>) {
      // Restore |tempChars|. (And in debug mode |tempLength|.)
      masm.PopRegsInMask(liveRegs);

      Label found;
      masm.branchPtr(Assembler::NotEqual, output, ImmPtr(nullptr), &found);
      {
        masm.move32(Imm32(-1), output);
        masm.jump(&restoreVolatile);
      }
      masm.bind(&found);

#ifdef DEBUG
      // Check lower bound.
      Label lower;
      masm.branchPtr(Assembler::AboveOrEqual, output, tempChars, &lower);
      masm.assumeUnreachable("result pointer below string chars");
      masm.bind(&lower);

      // Compute the end position of the characters.
      auto scale = encoding == CharEncoding::Latin1 ? TimesOne : TimesTwo;
      masm.computeEffectiveAddress(BaseIndex(tempChars, tempLength, scale),
                                   tempLength);

      // Check upper bound.
      Label upper;
      masm.branchPtr(Assembler::Below, output, tempLength, &upper);
      masm.assumeUnreachable("result pointer above string chars");
      masm.bind(&upper);
#endif

      masm.subPtr(tempChars, output);

      if (encoding == CharEncoding::TwoByte) {
        masm.rshiftPtr(Imm32(1), output);
      }
    }
  };

  volatileRegs.takeUnchecked(output);
  volatileRegs.takeUnchecked(tempLength);
  volatileRegs.takeUnchecked(tempChars);
  if (maybeTempPat != InvalidReg) {
    volatileRegs.takeUnchecked(maybeTempPat);
  }
  masm.PushRegsInMask(volatileRegs);

  // Handle the case when the input is a Latin-1 string.
  if (!searchStringIsPureTwoByte) {
    Label twoByte;
    masm.branchTwoByteString(string, &twoByte);
    {
      callMatcher(CharEncoding::Latin1);
      masm.jump(&restoreVolatile);
    }
    masm.bind(&twoByte);
  }

  // Handle the case when the input is a two-byte string.
  callMatcher(CharEncoding::TwoByte);

  masm.bind(&restoreVolatile);
  masm.PopRegsInMask(volatileRegs);

  // Convert to bool for `includes`.
  if constexpr (std::is_same_v<LIns, LStringIncludesSIMD>) {
    masm.cmpPtrSet(Assembler::NotEqual, output, ImmPtr(nullptr), output);
  }

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitStringIncludesSIMD(LStringIncludesSIMD* lir) {
  Register string = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  const JSLinearString* searchString = lir->searchString();

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  auto* ool = oolCallVM<Fn, js::StringIncludes>(
      lir, ArgList(string, ImmGCPtr(searchString)), StoreRegisterTo(output));

  CallStringMatch(masm, lir, ool, liveVolatileRegs(lir));
}

void CodeGenerator::visitStringIndexOf(LStringIndexOf* lir) {
  pushArg(ToRegister(lir->searchString()));
  pushArg(ToRegister(lir->string()));

  using Fn = bool (*)(JSContext*, HandleString, HandleString, int32_t*);
  callVM<Fn, js::StringIndexOf>(lir);
}

void CodeGenerator::visitStringIndexOfSIMD(LStringIndexOfSIMD* lir) {
  Register string = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  const JSLinearString* searchString = lir->searchString();

  using Fn = bool (*)(JSContext*, HandleString, HandleString, int32_t*);
  auto* ool = oolCallVM<Fn, js::StringIndexOf>(
      lir, ArgList(string, ImmGCPtr(searchString)), StoreRegisterTo(output));

  CallStringMatch(masm, lir, ool, liveVolatileRegs(lir));
}

void CodeGenerator::visitStringLastIndexOf(LStringLastIndexOf* lir) {
  pushArg(ToRegister(lir->searchString()));
  pushArg(ToRegister(lir->string()));

  using Fn = bool (*)(JSContext*, HandleString, HandleString, int32_t*);
  callVM<Fn, js::StringLastIndexOf>(lir);
}

void CodeGenerator::visitStringStartsWith(LStringStartsWith* lir) {
  pushArg(ToRegister(lir->searchString()));
  pushArg(ToRegister(lir->string()));

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  callVM<Fn, js::StringStartsWith>(lir);
}

void CodeGenerator::visitStringStartsWithInline(LStringStartsWithInline* lir) {
  Register string = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  const JSLinearString* searchString = lir->searchString();

  size_t length = searchString->length();
  MOZ_ASSERT(length > 0);

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  auto* ool = oolCallVM<Fn, js::StringStartsWith>(
      lir, ArgList(string, ImmGCPtr(searchString)), StoreRegisterTo(output));

  masm.move32(Imm32(0), output);

  // Can't be a prefix when the string is smaller than the search string.
  masm.branch32(Assembler::Below, Address(string, JSString::offsetOfLength()),
                Imm32(length), ool->rejoin());

  // Unwind ropes at the start if possible.
  Label compare;
  masm.movePtr(string, temp);
  masm.branchIfNotRope(temp, &compare);

  Label unwindRope;
  masm.bind(&unwindRope);
  masm.loadRopeLeftChild(temp, output);
  masm.movePtr(output, temp);

  // If the left child is smaller than the search string, jump into the VM to
  // linearize the string.
  masm.branch32(Assembler::Below, Address(temp, JSString::offsetOfLength()),
                Imm32(length), ool->entry());

  // Otherwise keep unwinding ropes.
  masm.branchIfRope(temp, &unwindRope);

  masm.bind(&compare);

  // If operands point to the same instance, it's trivially a prefix.
  Label notPointerEqual;
  masm.branchPtr(Assembler::NotEqual, temp, ImmGCPtr(searchString),
                 &notPointerEqual);
  masm.move32(Imm32(1), output);
  masm.jump(ool->rejoin());
  masm.bind(&notPointerEqual);

  if (searchString->hasTwoByteChars()) {
    // Pure two-byte strings can't be a prefix of Latin-1 strings.
    JS::AutoCheckCannotGC nogc;
    if (!mozilla::IsUtf16Latin1(searchString->twoByteRange(nogc))) {
      Label compareChars;
      masm.branchTwoByteString(temp, &compareChars);
      masm.move32(Imm32(0), output);
      masm.jump(ool->rejoin());
      masm.bind(&compareChars);
    }
  }

  // Load the input string's characters.
  Register stringChars = output;
  masm.loadStringCharsForCompare(temp, searchString, stringChars, ool->entry());

  // Start comparing character by character.
  masm.compareStringChars(JSOp::Eq, stringChars, searchString, output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitStringEndsWith(LStringEndsWith* lir) {
  pushArg(ToRegister(lir->searchString()));
  pushArg(ToRegister(lir->string()));

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  callVM<Fn, js::StringEndsWith>(lir);
}

void CodeGenerator::visitStringEndsWithInline(LStringEndsWithInline* lir) {
  Register string = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  const JSLinearString* searchString = lir->searchString();

  size_t length = searchString->length();
  MOZ_ASSERT(length > 0);

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  auto* ool = oolCallVM<Fn, js::StringEndsWith>(
      lir, ArgList(string, ImmGCPtr(searchString)), StoreRegisterTo(output));

  masm.move32(Imm32(0), output);

  // Can't be a suffix when the string is smaller than the search string.
  masm.branch32(Assembler::Below, Address(string, JSString::offsetOfLength()),
                Imm32(length), ool->rejoin());

  // Unwind ropes at the end if possible.
  Label compare;
  masm.movePtr(string, temp);
  masm.branchIfNotRope(temp, &compare);

  Label unwindRope;
  masm.bind(&unwindRope);
  masm.loadRopeRightChild(temp, output);
  masm.movePtr(output, temp);

  // If the right child is smaller than the search string, jump into the VM to
  // linearize the string.
  masm.branch32(Assembler::Below, Address(temp, JSString::offsetOfLength()),
                Imm32(length), ool->entry());

  // Otherwise keep unwinding ropes.
  masm.branchIfRope(temp, &unwindRope);

  masm.bind(&compare);

  // If operands point to the same instance, it's trivially a suffix.
  Label notPointerEqual;
  masm.branchPtr(Assembler::NotEqual, temp, ImmGCPtr(searchString),
                 &notPointerEqual);
  masm.move32(Imm32(1), output);
  masm.jump(ool->rejoin());
  masm.bind(&notPointerEqual);

  CharEncoding encoding = searchString->hasLatin1Chars()
                              ? CharEncoding::Latin1
                              : CharEncoding::TwoByte;
  if (encoding == CharEncoding::TwoByte) {
    // Pure two-byte strings can't be a suffix of Latin-1 strings.
    JS::AutoCheckCannotGC nogc;
    if (!mozilla::IsUtf16Latin1(searchString->twoByteRange(nogc))) {
      Label compareChars;
      masm.branchTwoByteString(temp, &compareChars);
      masm.move32(Imm32(0), output);
      masm.jump(ool->rejoin());
      masm.bind(&compareChars);
    }
  }

  // Load the input string's characters.
  Register stringChars = output;
  masm.loadStringCharsForCompare(temp, searchString, stringChars, ool->entry());

  // Move string-char pointer to the suffix string.
  masm.loadStringLength(temp, temp);
  masm.sub32(Imm32(length), temp);
  masm.addToCharPtr(stringChars, temp, encoding);

  // Start comparing character by character.
  masm.compareStringChars(JSOp::Eq, stringChars, searchString, output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitStringToLowerCase(LStringToLowerCase* lir) {
  Register string = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());

  // On x86 there are not enough registers. In that case reuse the string
  // register as a temporary.
  Register temp3 =
      lir->temp3()->isBogusTemp() ? string : ToRegister(lir->temp3());
  Register temp4 = ToRegister(lir->temp4());

  using Fn = JSLinearString* (*)(JSContext*, JSString*);
  OutOfLineCode* ool = oolCallVM<Fn, js::StringToLowerCase>(
      lir, ArgList(string), StoreRegisterTo(output));

  // Take the slow path if the string isn't a linear Latin-1 string.
  Imm32 linearLatin1Bits(JSString::LINEAR_BIT | JSString::LATIN1_CHARS_BIT);
  Register flags = temp0;
  masm.load32(Address(string, JSString::offsetOfFlags()), flags);
  masm.and32(linearLatin1Bits, flags);
  masm.branch32(Assembler::NotEqual, flags, linearLatin1Bits, ool->entry());

  Register length = temp0;
  masm.loadStringLength(string, length);

  // Return the input if it's the empty string.
  Label notEmptyString;
  masm.branch32(Assembler::NotEqual, length, Imm32(0), &notEmptyString);
  {
    masm.movePtr(string, output);
    masm.jump(ool->rejoin());
  }
  masm.bind(&notEmptyString);

  Register inputChars = temp1;
  masm.loadStringChars(string, inputChars, CharEncoding::Latin1);

  Register toLowerCaseTable = temp2;
  masm.movePtr(ImmPtr(unicode::latin1ToLowerCaseTable), toLowerCaseTable);

  // Single element strings can be directly retrieved from static strings cache.
  Label notSingleElementString;
  masm.branch32(Assembler::NotEqual, length, Imm32(1), &notSingleElementString);
  {
    Register current = temp4;

    masm.loadChar(Address(inputChars, 0), current, CharEncoding::Latin1);
    masm.load8ZeroExtend(BaseIndex(toLowerCaseTable, current, TimesOne),
                         current);
    masm.lookupStaticString(current, output, gen->runtime->staticStrings());

    masm.jump(ool->rejoin());
  }
  masm.bind(&notSingleElementString);

  // Use the OOL-path when the string is too long. This prevents scanning long
  // strings which have upper case characters only near the end a second time in
  // the VM.
  constexpr int32_t MaxInlineLength = 64;
  masm.branch32(Assembler::Above, length, Imm32(MaxInlineLength), ool->entry());

  {
    // Check if there are any characters which need to be converted.
    //
    // This extra loop gives a small performance improvement for strings which
    // are already lower cased and lets us avoid calling into the runtime for
    // non-inline, all lower case strings. But more importantly it avoids
    // repeated inline allocation failures:
    // |AllocateThinOrFatInlineString| below takes the OOL-path and calls the
    // |js::StringToLowerCase| runtime function when the result string can't be
    // allocated inline. And |js::StringToLowerCase| directly returns the input
    // string when no characters need to be converted. That means it won't
    // trigger GC to clear up the free nursery space, so the next toLowerCase()
    // call will again fail to inline allocate the result string.
    Label hasUpper;
    {
      Register checkInputChars = output;
      masm.movePtr(inputChars, checkInputChars);

      Register current = temp4;

      Label start;
      masm.bind(&start);
      masm.loadChar(Address(checkInputChars, 0), current, CharEncoding::Latin1);
      masm.branch8(Assembler::NotEqual,
                   BaseIndex(toLowerCaseTable, current, TimesOne), current,
                   &hasUpper);
      masm.addPtr(Imm32(sizeof(Latin1Char)), checkInputChars);
      masm.branchSub32(Assembler::NonZero, Imm32(1), length, &start);

      // Input is already in lower case.
      masm.movePtr(string, output);
      masm.jump(ool->rejoin());
    }
    masm.bind(&hasUpper);

    // |length| was clobbered above, reload.
    masm.loadStringLength(string, length);

    // Call into the runtime when we can't create an inline string.
    masm.branch32(Assembler::Above, length,
                  Imm32(JSFatInlineString::MAX_LENGTH_LATIN1), ool->entry());

    AllocateThinOrFatInlineString(masm, output, length, temp4,
                                  initialStringHeap(), ool->entry(),
                                  CharEncoding::Latin1);

    if (temp3 == string) {
      masm.push(string);
    }

    Register outputChars = temp3;
    masm.loadInlineStringCharsForStore(output, outputChars);

    {
      Register current = temp4;

      Label start;
      masm.bind(&start);
      masm.loadChar(Address(inputChars, 0), current, CharEncoding::Latin1);
      masm.load8ZeroExtend(BaseIndex(toLowerCaseTable, current, TimesOne),
                           current);
      masm.storeChar(current, Address(outputChars, 0), CharEncoding::Latin1);
      masm.addPtr(Imm32(sizeof(Latin1Char)), inputChars);
      masm.addPtr(Imm32(sizeof(Latin1Char)), outputChars);
      masm.branchSub32(Assembler::NonZero, Imm32(1), length, &start);
    }

    if (temp3 == string) {
      masm.pop(string);
    }
  }

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitStringToUpperCase(LStringToUpperCase* lir) {
  pushArg(ToRegister(lir->string()));

  using Fn = JSLinearString* (*)(JSContext*, JSString*);
  callVM<Fn, js::StringToUpperCase>(lir);
}

void CodeGenerator::visitCharCodeToLowerCase(LCharCodeToLowerCase* lir) {
  Register code = ToRegister(lir->code());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  using Fn = JSString* (*)(JSContext*, int32_t);
  auto* ool = oolCallVM<Fn, jit::CharCodeToLowerCase>(lir, ArgList(code),
                                                      StoreRegisterTo(output));

  constexpr char16_t NonLatin1Min = char16_t(JSString::MAX_LATIN1_CHAR) + 1;

  // OOL path if code >= NonLatin1Min.
  masm.boundsCheck32PowerOfTwo(code, NonLatin1Min, ool->entry());

  // Convert to lower case.
  masm.movePtr(ImmPtr(unicode::latin1ToLowerCaseTable), temp);
  masm.load8ZeroExtend(BaseIndex(temp, code, TimesOne), temp);

  // Load static string for lower case character.
  masm.lookupStaticString(temp, output, gen->runtime->staticStrings());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCharCodeToUpperCase(LCharCodeToUpperCase* lir) {
  Register code = ToRegister(lir->code());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  using Fn = JSString* (*)(JSContext*, int32_t);
  auto* ool = oolCallVM<Fn, jit::CharCodeToUpperCase>(lir, ArgList(code),
                                                      StoreRegisterTo(output));

  constexpr char16_t NonLatin1Min = char16_t(JSString::MAX_LATIN1_CHAR) + 1;

  // OOL path if code >= NonLatin1Min.
  masm.boundsCheck32PowerOfTwo(code, NonLatin1Min, ool->entry());

  // Most one element Latin-1 strings can be directly retrieved from the
  // static strings cache, except the following three characters:
  //
  // 1. ToUpper(U+00B5) = 0+039C
  // 2. ToUpper(U+00FF) = 0+0178
  // 3. ToUpper(U+00DF) = 0+0053 0+0053
  masm.branch32(Assembler::Equal, code, Imm32(unicode::MICRO_SIGN),
                ool->entry());
  masm.branch32(Assembler::Equal, code,
                Imm32(unicode::LATIN_SMALL_LETTER_Y_WITH_DIAERESIS),
                ool->entry());
  masm.branch32(Assembler::Equal, code,
                Imm32(unicode::LATIN_SMALL_LETTER_SHARP_S), ool->entry());

  // Inline unicode::ToUpperCase (without the special case for ASCII characters)

  constexpr size_t shift = unicode::CharInfoShift;

  // code >> shift
  masm.rshift32(Imm32(shift), code, temp);

  // index = index1[code >> shift];
  masm.movePtr(ImmPtr(unicode::index1), output);
  masm.load8ZeroExtend(BaseIndex(output, temp, TimesOne), temp);

  // (code & ((1 << shift) - 1)
  masm.and32(Imm32((1 << shift) - 1), code, output);

  // (index << shift) + (code & ((1 << shift) - 1))
  masm.lshift32(Imm32(shift), temp);
  masm.add32(output, temp);

  // index = index2[(index << shift) + (code & ((1 << shift) - 1))]
  masm.movePtr(ImmPtr(unicode::index2), output);
  masm.load8ZeroExtend(BaseIndex(output, temp, TimesOne), temp);

  // Compute |index * 6| through |(index * 3) * TimesTwo|.
  static_assert(sizeof(unicode::CharacterInfo) == 6);
  masm.mulBy3(temp, temp);

  // upperCase = js_charinfo[index].upperCase
  masm.movePtr(ImmPtr(unicode::js_charinfo), output);
  masm.load16ZeroExtend(BaseIndex(output, temp, TimesTwo,
                                  offsetof(unicode::CharacterInfo, upperCase)),
                        temp);

  // uint16_t(ch) + upperCase
  masm.add32(code, temp);

  // Clear any high bits added when performing the unsigned 16-bit addition
  // through a signed 32-bit addition.
  masm.move8ZeroExtend(temp, temp);

  // Load static string for upper case character.
  masm.lookupStaticString(temp, output, gen->runtime->staticStrings());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitStringTrimStartIndex(LStringTrimStartIndex* lir) {
  Register string = ToRegister(lir->string());
  Register output = ToRegister(lir->output());

  auto volatileRegs = liveVolatileRegs(lir);
  volatileRegs.takeUnchecked(output);

  masm.PushRegsInMask(volatileRegs);

  using Fn = int32_t (*)(const JSString*);
  masm.setupAlignedABICall();
  masm.passABIArg(string);
  masm.callWithABI<Fn, jit::StringTrimStartIndex>();
  masm.storeCallInt32Result(output);

  masm.PopRegsInMask(volatileRegs);
}

void CodeGenerator::visitStringTrimEndIndex(LStringTrimEndIndex* lir) {
  Register string = ToRegister(lir->string());
  Register start = ToRegister(lir->start());
  Register output = ToRegister(lir->output());

  auto volatileRegs = liveVolatileRegs(lir);
  volatileRegs.takeUnchecked(output);

  masm.PushRegsInMask(volatileRegs);

  using Fn = int32_t (*)(const JSString*, int32_t);
  masm.setupAlignedABICall();
  masm.passABIArg(string);
  masm.passABIArg(start);
  masm.callWithABI<Fn, jit::StringTrimEndIndex>();
  masm.storeCallInt32Result(output);

  masm.PopRegsInMask(volatileRegs);
}

void CodeGenerator::visitStringSplit(LStringSplit* lir) {
  pushArg(Imm32(INT32_MAX));
  pushArg(ToRegister(lir->separator()));
  pushArg(ToRegister(lir->string()));

  using Fn = ArrayObject* (*)(JSContext*, HandleString, HandleString, uint32_t);
  callVM<Fn, js::StringSplitString>(lir);
}

void CodeGenerator::visitInitializedLength(LInitializedLength* lir) {
  Address initLength(ToRegister(lir->elements()),
                     ObjectElements::offsetOfInitializedLength());
  masm.load32(initLength, ToRegister(lir->output()));
}

void CodeGenerator::visitSetInitializedLength(LSetInitializedLength* lir) {
  Address initLength(ToRegister(lir->elements()),
                     ObjectElements::offsetOfInitializedLength());
  SetLengthFromIndex(masm, lir->index(), initLength);
}

void CodeGenerator::visitNotI(LNotI* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  masm.cmp32Set(Assembler::Equal, input, Imm32(0), output);
}

void CodeGenerator::visitNotIPtr(LNotIPtr* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  masm.cmpPtrSet(Assembler::Equal, input, ImmWord(0), output);
}

void CodeGenerator::visitNotI64(LNotI64* lir) {
  Register64 input = ToRegister64(lir->inputI64());
  Register output = ToRegister(lir->output());

  masm.cmp64Set(Assembler::Equal, input, Imm64(0), output);
}

void CodeGenerator::visitNotBI(LNotBI* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  masm.cmp32Set(Assembler::Equal, Address(input, BigInt::offsetOfLength()),
                Imm32(0), output);
}

void CodeGenerator::visitNotO(LNotO* lir) {
  Register objreg = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  bool intact = hasSeenObjectEmulateUndefinedFuseIntactAndDependencyNoted();
  if (intact) {
    // Bug 1874905: It would be fantastic if this could be optimized out.
    assertObjectDoesNotEmulateUndefined(objreg, output, lir->mir());
    masm.move32(Imm32(0), output);
  } else {
    auto* ool = new (alloc()) OutOfLineTestObjectWithLabels();
    addOutOfLineCode(ool, lir->mir());

    Label* ifEmulatesUndefined = ool->label1();
    Label* ifDoesntEmulateUndefined = ool->label2();

    branchTestObjectEmulatesUndefined(objreg, ifEmulatesUndefined,
                                      ifDoesntEmulateUndefined, output, ool);
    // fall through

    Label join;

    masm.move32(Imm32(0), output);
    masm.jump(&join);

    masm.bind(ifEmulatesUndefined);
    masm.move32(Imm32(1), output);

    masm.bind(&join);
  }
}

void CodeGenerator::visitNotV(LNotV* lir) {
  auto* ool = new (alloc()) OutOfLineTestObjectWithLabels();
  addOutOfLineCode(ool, lir->mir());

  Label* ifTruthy = ool->label1();
  Label* ifFalsy = ool->label2();

  ValueOperand input = ToValue(lir->input());
  Register tempToUnbox = ToTempUnboxRegister(lir->temp1());
  FloatRegister floatTemp = ToFloatRegister(lir->temp0());
  Register output = ToRegister(lir->output());
  const TypeDataList& observedTypes = lir->mir()->observedTypes();

  testValueTruthy(input, tempToUnbox, output, floatTemp, observedTypes,
                  ifTruthy, ifFalsy, ool);

  Label join;

  // Note that the testValueTruthy call above may choose to fall through
  // to ifTruthy instead of branching there.
  masm.bind(ifTruthy);
  masm.move32(Imm32(0), output);
  masm.jump(&join);

  masm.bind(ifFalsy);
  masm.move32(Imm32(1), output);

  // both branches meet here.
  masm.bind(&join);
}

void CodeGenerator::visitBoundsCheck(LBoundsCheck* lir) {
  const LAllocation* index = lir->index();
  const LAllocation* length = lir->length();
  LSnapshot* snapshot = lir->snapshot();

  MIRType type = lir->mir()->type();

  auto bailoutCmp = [&](Assembler::Condition cond, auto lhs, auto rhs) {
    if (type == MIRType::Int32) {
      bailoutCmp32(cond, lhs, rhs, snapshot);
    } else {
      MOZ_ASSERT(type == MIRType::IntPtr);
      bailoutCmpPtr(cond, lhs, rhs, snapshot);
    }
  };

  auto bailoutCmpConstant = [&](Assembler::Condition cond, auto lhs,
                                int32_t rhs) {
    if (type == MIRType::Int32) {
      bailoutCmp32(cond, lhs, Imm32(rhs), snapshot);
    } else {
      MOZ_ASSERT(type == MIRType::IntPtr);
      bailoutCmpPtr(cond, lhs, ImmWord(rhs), snapshot);
    }
  };

  if (index->isConstant()) {
    // Use uint32 so that the comparison is unsigned.
    uint32_t idx = ToInt32(index);
    if (length->isConstant()) {
      uint32_t len = ToInt32(lir->length());
      if (idx < len) {
        return;
      }
      bailout(snapshot);
      return;
    }

    if (length->isGeneralReg()) {
      bailoutCmpConstant(Assembler::BelowOrEqual, ToRegister(length), idx);
    } else {
      bailoutCmpConstant(Assembler::BelowOrEqual, ToAddress(length), idx);
    }
    return;
  }

  Register indexReg = ToRegister(index);
  if (length->isConstant()) {
    bailoutCmpConstant(Assembler::AboveOrEqual, indexReg, ToInt32(length));
  } else if (length->isGeneralReg()) {
    bailoutCmp(Assembler::BelowOrEqual, ToRegister(length), indexReg);
  } else {
    bailoutCmp(Assembler::BelowOrEqual, ToAddress(length), indexReg);
  }
}

void CodeGenerator::visitBoundsCheckRange(LBoundsCheckRange* lir) {
  int32_t min = lir->mir()->minimum();
  int32_t max = lir->mir()->maximum();
  MOZ_ASSERT(max >= min);

  LSnapshot* snapshot = lir->snapshot();
  MIRType type = lir->mir()->type();

  const LAllocation* length = lir->length();
  Register temp = ToRegister(lir->temp0());

  auto bailoutCmp = [&](Assembler::Condition cond, auto lhs, auto rhs) {
    if (type == MIRType::Int32) {
      bailoutCmp32(cond, lhs, rhs, snapshot);
    } else {
      MOZ_ASSERT(type == MIRType::IntPtr);
      bailoutCmpPtr(cond, lhs, rhs, snapshot);
    }
  };

  auto bailoutCmpConstant = [&](Assembler::Condition cond, auto lhs,
                                int32_t rhs) {
    if (type == MIRType::Int32) {
      bailoutCmp32(cond, lhs, Imm32(rhs), snapshot);
    } else {
      MOZ_ASSERT(type == MIRType::IntPtr);
      bailoutCmpPtr(cond, lhs, ImmWord(rhs), snapshot);
    }
  };

  if (lir->index()->isConstant()) {
    int32_t nmin, nmax;
    int32_t index = ToInt32(lir->index());
    if (SafeAdd(index, min, &nmin) && SafeAdd(index, max, &nmax) && nmin >= 0) {
      if (length->isGeneralReg()) {
        bailoutCmpConstant(Assembler::BelowOrEqual, ToRegister(length), nmax);
      } else {
        bailoutCmpConstant(Assembler::BelowOrEqual, ToAddress(length), nmax);
      }
      return;
    }
    masm.mov(ImmWord(index), temp);
  } else {
    masm.mov(ToRegister(lir->index()), temp);
  }

  // If the minimum and maximum differ then do an underflow check first.
  // If the two are the same then doing an unsigned comparison on the
  // length will also catch a negative index.
  if (min != max) {
    if (min != 0) {
      Label bail;
      if (type == MIRType::Int32) {
        masm.branchAdd32(Assembler::Overflow, Imm32(min), temp, &bail);
      } else {
        masm.branchAddPtr(Assembler::Overflow, Imm32(min), temp, &bail);
      }
      bailoutFrom(&bail, snapshot);
    }

    bailoutCmpConstant(Assembler::LessThan, temp, 0);

    if (min != 0) {
      int32_t diff;
      if (SafeSub(max, min, &diff)) {
        max = diff;
      } else {
        if (type == MIRType::Int32) {
          masm.sub32(Imm32(min), temp);
        } else {
          masm.subPtr(Imm32(min), temp);
        }
      }
    }
  }

  // Compute the maximum possible index. No overflow check is needed when
  // max > 0. We can only wraparound to a negative number, which will test as
  // larger than all nonnegative numbers in the unsigned comparison, and the
  // length is required to be nonnegative (else testing a negative length
  // would succeed on any nonnegative index).
  if (max != 0) {
    if (max < 0) {
      Label bail;
      if (type == MIRType::Int32) {
        masm.branchAdd32(Assembler::Overflow, Imm32(max), temp, &bail);
      } else {
        masm.branchAddPtr(Assembler::Overflow, Imm32(max), temp, &bail);
      }
      bailoutFrom(&bail, snapshot);
    } else {
      if (type == MIRType::Int32) {
        masm.add32(Imm32(max), temp);
      } else {
        masm.addPtr(Imm32(max), temp);
      }
    }
  }

  if (length->isGeneralReg()) {
    bailoutCmp(Assembler::BelowOrEqual, ToRegister(length), temp);
  } else {
    bailoutCmp(Assembler::BelowOrEqual, ToAddress(length), temp);
  }
}

void CodeGenerator::visitBoundsCheckLower(LBoundsCheckLower* lir) {
  int32_t min = lir->mir()->minimum();
  bailoutCmp32(Assembler::LessThan, ToRegister(lir->index()), Imm32(min),
               lir->snapshot());
}

void CodeGenerator::visitSpectreMaskIndex(LSpectreMaskIndex* lir) {
  MOZ_ASSERT(JitOptions.spectreIndexMasking);

  const LAllocation* length = lir->length();
  Register index = ToRegister(lir->index());
  Register output = ToRegister(lir->output());

  if (lir->mir()->type() == MIRType::Int32) {
    if (length->isGeneralReg()) {
      masm.spectreMaskIndex32(index, ToRegister(length), output);
    } else {
      masm.spectreMaskIndex32(index, ToAddress(length), output);
    }
  } else {
    MOZ_ASSERT(lir->mir()->type() == MIRType::IntPtr);
    if (length->isGeneralReg()) {
      masm.spectreMaskIndexPtr(index, ToRegister(length), output);
    } else {
      masm.spectreMaskIndexPtr(index, ToAddress(length), output);
    }
  }
}

void CodeGenerator::emitStoreHoleCheck(Register elements,
                                       const LAllocation* index,
                                       LSnapshot* snapshot) {
  Label bail;
  if (index->isConstant()) {
    Address dest(elements, ToInt32(index) * sizeof(js::Value));
    masm.branchTestMagic(Assembler::Equal, dest, &bail);
  } else {
    BaseObjectElementIndex dest(elements, ToRegister(index));
    masm.branchTestMagic(Assembler::Equal, dest, &bail);
  }
  bailoutFrom(&bail, snapshot);
}

void CodeGenerator::emitStoreElementTyped(const LAllocation* value,
                                          MIRType valueType, Register elements,
                                          const LAllocation* index) {
  MOZ_ASSERT(valueType != MIRType::MagicHole);
  ConstantOrRegister v = ToConstantOrRegister(value, valueType);
  if (index->isConstant()) {
    Address dest(elements, ToInt32(index) * sizeof(js::Value));
    masm.storeUnboxedValue(v, valueType, dest);
  } else {
    BaseObjectElementIndex dest(elements, ToRegister(index));
    masm.storeUnboxedValue(v, valueType, dest);
  }
}

void CodeGenerator::visitStoreElementT(LStoreElementT* store) {
  Register elements = ToRegister(store->elements());
  const LAllocation* index = store->index();

  if (store->mir()->needsBarrier()) {
    emitPreBarrier(elements, index);
  }

  if (store->mir()->needsHoleCheck()) {
    emitStoreHoleCheck(elements, index, store->snapshot());
  }

  emitStoreElementTyped(store->value(), store->mir()->value()->type(), elements,
                        index);
}

void CodeGenerator::visitStoreElementV(LStoreElementV* lir) {
  ValueOperand value = ToValue(lir->value());
  Register elements = ToRegister(lir->elements());
  const LAllocation* index = lir->index();

  if (lir->mir()->needsBarrier()) {
    emitPreBarrier(elements, index);
  }

  if (lir->mir()->needsHoleCheck()) {
    emitStoreHoleCheck(elements, index, lir->snapshot());
  }

  if (lir->index()->isConstant()) {
    Address dest(elements, ToInt32(lir->index()) * sizeof(js::Value));
    masm.storeValue(value, dest);
  } else {
    BaseObjectElementIndex dest(elements, ToRegister(lir->index()));
    masm.storeValue(value, dest);
  }
}

void CodeGenerator::visitStoreHoleValueElement(LStoreHoleValueElement* lir) {
  Register elements = ToRegister(lir->elements());
  Register index = ToRegister(lir->index());

  Address elementsFlags(elements, ObjectElements::offsetOfFlags());
  masm.or32(Imm32(ObjectElements::NON_PACKED), elementsFlags);

  BaseObjectElementIndex element(elements, index);
  masm.storeValue(MagicValue(JS_ELEMENTS_HOLE), element);
}

void CodeGenerator::visitStoreElementHoleT(LStoreElementHoleT* lir) {
  Register obj = ToRegister(lir->object());
  Register elements = ToRegister(lir->elements());
  Register index = ToRegister(lir->index());
  Register temp = ToRegister(lir->temp0());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Label bail;
    masm.prepareOOBStoreElement(obj, index, elements, temp, &bail,
                                liveVolatileRegs(lir));
    bailoutFrom(&bail, lir->snapshot());

    // Jump to the inline path where we will store the value.
    // We rejoin after the prebarrier, because the memory is uninitialized.
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  Address initLength(elements, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, initLength, temp, ool->entry());

  emitPreBarrier(elements, lir->index());

  masm.bind(ool->rejoin());
  emitStoreElementTyped(lir->value(), lir->mir()->value()->type(), elements,
                        lir->index());

  if (ValueNeedsPostBarrier(lir->mir()->value())) {
    LiveRegisterSet regs = liveVolatileRegs(lir);
    ConstantOrRegister val =
        ToConstantOrRegister(lir->value(), lir->mir()->value()->type());
    emitElementPostWriteBarrier(lir->mir(), regs, obj, lir->index(), temp, val);
  }
}

void CodeGenerator::visitStoreElementHoleV(LStoreElementHoleV* lir) {
  Register obj = ToRegister(lir->object());
  Register elements = ToRegister(lir->elements());
  Register index = ToRegister(lir->index());
  ValueOperand value = ToValue(lir->value());
  Register temp = ToRegister(lir->temp0());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Label bail;
    masm.prepareOOBStoreElement(obj, index, elements, temp, &bail,
                                liveVolatileRegs(lir));
    bailoutFrom(&bail, lir->snapshot());

    // Jump to the inline path where we will store the value.
    // We rejoin after the prebarrier, because the memory is uninitialized.
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  Address initLength(elements, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, initLength, temp, ool->entry());

  emitPreBarrier(elements, lir->index());

  masm.bind(ool->rejoin());
  masm.storeValue(value, BaseObjectElementIndex(elements, index));

  if (ValueNeedsPostBarrier(lir->mir()->value())) {
    LiveRegisterSet regs = liveVolatileRegs(lir);
    emitElementPostWriteBarrier(lir->mir(), regs, obj, lir->index(), temp,
                                ConstantOrRegister(value));
  }
}

void CodeGenerator::visitArrayPopShift(LArrayPopShift* lir) {
  Register obj = ToRegister(lir->object());
  Register temp1 = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp1());
  ValueOperand out = ToOutValue(lir);

  Label bail;
  if (lir->mir()->mode() == MArrayPopShift::Pop) {
    masm.packedArrayPop(obj, out, temp1, temp2, &bail);
  } else {
    MOZ_ASSERT(lir->mir()->mode() == MArrayPopShift::Shift);
    LiveRegisterSet volatileRegs = liveVolatileRegs(lir);
    masm.packedArrayShift(obj, out, temp1, temp2, volatileRegs, &bail);
  }
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitArrayPush(LArrayPush* lir) {
  Register obj = ToRegister(lir->object());
  Register elementsTemp = ToRegister(lir->temp0());
  Register length = ToRegister(lir->output());
  ValueOperand value = ToValue(lir->value());
  Register spectreTemp = ToTempRegisterOrInvalid(lir->temp1());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Register temp = ToRegister(lir->temp0());

    LiveRegisterSet liveRegs = liveVolatileRegs(lir);
    liveRegs.takeUnchecked(temp);
    liveRegs.addUnchecked(ToRegister(lir->output()));
    liveRegs.addUnchecked(ToValue(lir->value()));

    masm.PushRegsInMask(liveRegs);

    masm.setupAlignedABICall();
    masm.loadJSContext(temp);
    masm.passABIArg(temp);
    masm.passABIArg(obj);

    using Fn = bool (*)(JSContext*, NativeObject* obj);
    masm.callWithABI<Fn, NativeObject::addDenseElementPure>();
    masm.storeCallPointerResult(temp);

    masm.PopRegsInMask(liveRegs);
    bailoutIfFalseBool(temp, lir->snapshot());

    // Load the reallocated elements pointer.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), temp);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  // Load obj->elements in elementsTemp.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), elementsTemp);

  Address initLengthAddr(elementsTemp,
                         ObjectElements::offsetOfInitializedLength());
  Address lengthAddr(elementsTemp, ObjectElements::offsetOfLength());
  Address capacityAddr(elementsTemp, ObjectElements::offsetOfCapacity());

  // Bail out if length != initLength.
  masm.load32(lengthAddr, length);
  bailoutCmp32(Assembler::NotEqual, initLengthAddr, length, lir->snapshot());

  // If length < capacity, we can add a dense element inline. If not, we
  // need to allocate more elements.
  masm.spectreBoundsCheck32(length, capacityAddr, spectreTemp, ool->entry());
  masm.bind(ool->rejoin());

  // Store the value.
  masm.storeValue(value, BaseObjectElementIndex(elementsTemp, length));

  // Update length and initialized length.
  masm.add32(Imm32(1), length);
  masm.store32(length, Address(elementsTemp, ObjectElements::offsetOfLength()));
  masm.store32(length, Address(elementsTemp,
                               ObjectElements::offsetOfInitializedLength()));

  if (ValueNeedsPostBarrier(lir->mir()->value())) {
    LiveRegisterSet regs = liveVolatileRegs(lir);
    regs.addUnchecked(length);
    emitElementPostWriteBarrier(lir->mir(), regs, obj, lir->output()->output(),
                                elementsTemp, ConstantOrRegister(value),
                                /* indexDiff = */ -1);
  }
}

void CodeGenerator::visitArraySlice(LArraySlice* lir) {
  Register object = ToRegister(lir->object());
  Register begin = ToRegister(lir->begin());
  Register end = ToRegister(lir->end());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  Label call, fail;

  Label bail;
  masm.branchArrayIsNotPacked(object, temp0, temp1, &bail);
  bailoutFrom(&bail, lir->snapshot());

  // Try to allocate an object.
  TemplateObject templateObject(lir->mir()->templateObj());
  masm.createGCObject(temp0, temp1, templateObject, lir->mir()->initialHeap(),
                      &fail);

  masm.jump(&call);
  {
    masm.bind(&fail);
    masm.movePtr(ImmPtr(nullptr), temp0);
  }
  masm.bind(&call);

  pushArg(temp0);
  pushArg(end);
  pushArg(begin);
  pushArg(object);

  using Fn =
      JSObject* (*)(JSContext*, HandleObject, int32_t, int32_t, HandleObject);
  callVM<Fn, ArraySliceDense>(lir);
}

void CodeGenerator::visitArgumentsSlice(LArgumentsSlice* lir) {
  Register object = ToRegister(lir->object());
  Register begin = ToRegister(lir->begin());
  Register end = ToRegister(lir->end());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  Label call, fail;

  // Try to allocate an object.
  TemplateObject templateObject(lir->mir()->templateObj());
  masm.createGCObject(temp0, temp1, templateObject, lir->mir()->initialHeap(),
                      &fail);

  masm.jump(&call);
  {
    masm.bind(&fail);
    masm.movePtr(ImmPtr(nullptr), temp0);
  }
  masm.bind(&call);

  pushArg(temp0);
  pushArg(end);
  pushArg(begin);
  pushArg(object);

  using Fn =
      JSObject* (*)(JSContext*, HandleObject, int32_t, int32_t, HandleObject);
  callVM<Fn, ArgumentsSliceDense>(lir);
}

#ifdef DEBUG
void CodeGenerator::emitAssertArgumentsSliceBounds(const RegisterOrInt32& begin,
                                                   const RegisterOrInt32& count,
                                                   Register numActualArgs) {
  // |begin| must be positive or zero.
  if (begin.is<Register>()) {
    Label beginOk;
    masm.branch32(Assembler::GreaterThanOrEqual, begin.as<Register>(), Imm32(0),
                  &beginOk);
    masm.assumeUnreachable("begin < 0");
    masm.bind(&beginOk);
  } else {
    MOZ_ASSERT(begin.as<int32_t>() >= 0);
  }

  // |count| must be positive or zero.
  if (count.is<Register>()) {
    Label countOk;
    masm.branch32(Assembler::GreaterThanOrEqual, count.as<Register>(), Imm32(0),
                  &countOk);
    masm.assumeUnreachable("count < 0");
    masm.bind(&countOk);
  } else {
    MOZ_ASSERT(count.as<int32_t>() >= 0);
  }

  // |begin| must be less-or-equal to |numActualArgs|.
  Label argsBeginOk;
  if (begin.is<Register>()) {
    masm.branchPtr(Assembler::AboveOrEqual, numActualArgs, begin.as<Register>(),
                   &argsBeginOk);
  } else {
    masm.branchPtr(Assembler::AboveOrEqual, numActualArgs,
                   Imm32(begin.as<int32_t>()), &argsBeginOk);
  }
  masm.assumeUnreachable("begin <= numActualArgs");
  masm.bind(&argsBeginOk);

  // |count| must be less-or-equal to |numActualArgs|.
  Label argsCountOk;
  if (count.is<Register>()) {
    masm.branchPtr(Assembler::AboveOrEqual, numActualArgs, count.as<Register>(),
                   &argsCountOk);
  } else {
    masm.branchPtr(Assembler::AboveOrEqual, numActualArgs,
                   Imm32(count.as<int32_t>()), &argsCountOk);
  }
  masm.assumeUnreachable("count <= numActualArgs");
  masm.bind(&argsCountOk);

  // |begin| and |count| must be preserved, but |numActualArgs| can be changed.
  //
  // Pre-condition: |count| <= |numActualArgs|
  // Condition to test: |begin + count| <= |numActualArgs|
  // Transform to: |begin| <= |numActualArgs - count|
  if (count.is<Register>()) {
    masm.subPtr(count.as<Register>(), numActualArgs);
  } else {
    masm.subPtr(Imm32(count.as<int32_t>()), numActualArgs);
  }

  // |begin + count| must be less-or-equal to |numActualArgs|.
  Label argsBeginCountOk;
  if (begin.is<Register>()) {
    masm.branchPtr(Assembler::AboveOrEqual, numActualArgs, begin.as<Register>(),
                   &argsBeginCountOk);
  } else {
    masm.branchPtr(Assembler::AboveOrEqual, numActualArgs,
                   Imm32(begin.as<int32_t>()), &argsBeginCountOk);
  }
  masm.assumeUnreachable("begin + count <= numActualArgs");
  masm.bind(&argsBeginCountOk);
}
#endif

template <class ArgumentsSlice>
void CodeGenerator::emitNewArray(ArgumentsSlice* lir,
                                 const RegisterOrInt32& count, Register output,
                                 Register temp) {
  using Fn = ArrayObject* (*)(JSContext*, int32_t);
  auto* ool = count.match(
      [&](Register count) {
        return oolCallVM<Fn, NewArrayObjectEnsureDenseInitLength>(
            lir, ArgList(count), StoreRegisterTo(output));
      },
      [&](int32_t count) {
        return oolCallVM<Fn, NewArrayObjectEnsureDenseInitLength>(
            lir, ArgList(Imm32(count)), StoreRegisterTo(output));
      });

  TemplateObject templateObject(lir->mir()->templateObj());
  MOZ_ASSERT(templateObject.isArrayObject());

  auto templateNativeObj = templateObject.asTemplateNativeObject();
  MOZ_ASSERT(templateNativeObj.getArrayLength() == 0);
  MOZ_ASSERT(templateNativeObj.getDenseInitializedLength() == 0);
  MOZ_ASSERT(!templateNativeObj.hasDynamicElements());

  // Check array capacity. Call into the VM if the template object's capacity
  // is too small.
  bool tryAllocate = count.match(
      [&](Register count) {
        masm.branch32(Assembler::Above, count,
                      Imm32(templateNativeObj.getDenseCapacity()),
                      ool->entry());
        return true;
      },
      [&](int32_t count) {
        MOZ_ASSERT(count >= 0);
        if (uint32_t(count) > templateNativeObj.getDenseCapacity()) {
          masm.jump(ool->entry());
          return false;
        }
        return true;
      });

  if (tryAllocate) {
    // Try to allocate an object.
    masm.createGCObject(output, temp, templateObject, lir->mir()->initialHeap(),
                        ool->entry());

    auto setInitializedLengthAndLength = [&](auto count) {
      const int elementsOffset = NativeObject::offsetOfFixedElements();

      // Update initialized length.
      Address initLength(
          output, elementsOffset + ObjectElements::offsetOfInitializedLength());
      masm.store32(count, initLength);

      // Update length.
      Address length(output, elementsOffset + ObjectElements::offsetOfLength());
      masm.store32(count, length);
    };

    // The array object was successfully created. Set the length and initialized
    // length and then proceed to fill the elements.
    count.match([&](Register count) { setInitializedLengthAndLength(count); },
                [&](int32_t count) {
                  if (count > 0) {
                    setInitializedLengthAndLength(Imm32(count));
                  }
                });
  }

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitFrameArgumentsSlice(LFrameArgumentsSlice* lir) {
  Register begin = ToRegister(lir->begin());
  Register count = ToRegister(lir->count());
  Register temp = ToRegister(lir->temp0());
  Register output = ToRegister(lir->output());

#ifdef DEBUG
  masm.loadNumActualArgs(FramePointer, temp);
  emitAssertArgumentsSliceBounds(RegisterOrInt32(begin), RegisterOrInt32(count),
                                 temp);
#endif

  emitNewArray(lir, RegisterOrInt32(count), output, temp);

  Label done;
  masm.branch32(Assembler::Equal, count, Imm32(0), &done);
  {
    AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
    allRegs.take(begin);
    allRegs.take(count);
    allRegs.take(temp);
    allRegs.take(output);

    ValueOperand value = allRegs.takeAnyValue();

    LiveRegisterSet liveRegs;
    liveRegs.add(output);
    liveRegs.add(begin);
    liveRegs.add(value);

    masm.PushRegsInMask(liveRegs);

    // Initialize all elements.

    Register elements = output;
    masm.loadPtr(Address(output, NativeObject::offsetOfElements()), elements);

    Register argIndex = begin;

    Register index = temp;
    masm.move32(Imm32(0), index);

    size_t argvOffset = JitFrameLayout::offsetOfActualArgs();
    BaseValueIndex argPtr(FramePointer, argIndex, argvOffset);

    Label loop;
    masm.bind(&loop);

    masm.loadValue(argPtr, value);

    // We don't need a pre-barrier, because the element at |index| is guaranteed
    // to be a non-GC thing (either uninitialized memory or the magic hole
    // value).
    masm.storeValue(value, BaseObjectElementIndex(elements, index));

    masm.add32(Imm32(1), index);
    masm.add32(Imm32(1), argIndex);

    masm.branch32(Assembler::LessThan, index, count, &loop);

    masm.PopRegsInMask(liveRegs);

    // Emit a post-write barrier if |output| is tenured.
    //
    // We expect that |output| is nursery allocated, so it isn't worth the
    // trouble to check if no frame argument is a nursery thing, which would
    // allow to omit the post-write barrier.
    masm.branchPtrInNurseryChunk(Assembler::Equal, output, temp, &done);

    LiveRegisterSet volatileRegs = liveVolatileRegs(lir);
    volatileRegs.takeUnchecked(temp);
    if (output.volatile_()) {
      volatileRegs.addUnchecked(output);
    }

    masm.PushRegsInMask(volatileRegs);
    emitPostWriteBarrier(output);
    masm.PopRegsInMask(volatileRegs);
  }
  masm.bind(&done);
}

CodeGenerator::RegisterOrInt32 CodeGenerator::ToRegisterOrInt32(
    const LAllocation* allocation) {
  if (allocation->isConstant()) {
    return RegisterOrInt32(allocation->toConstant()->toInt32());
  }
  return RegisterOrInt32(ToRegister(allocation));
}

void CodeGenerator::visitInlineArgumentsSlice(LInlineArgumentsSlice* lir) {
  RegisterOrInt32 begin = ToRegisterOrInt32(lir->begin());
  RegisterOrInt32 count = ToRegisterOrInt32(lir->count());
  Register temp = ToRegister(lir->temp());
  Register output = ToRegister(lir->output());

  uint32_t numActuals = lir->mir()->numActuals();

#ifdef DEBUG
  masm.move32(Imm32(numActuals), temp);

  emitAssertArgumentsSliceBounds(begin, count, temp);
#endif

  emitNewArray(lir, count, output, temp);

  // We're done if there are no actual arguments.
  if (numActuals == 0) {
    return;
  }

  // Check if any arguments have to be copied.
  Label done;
  if (count.is<Register>()) {
    masm.branch32(Assembler::Equal, count.as<Register>(), Imm32(0), &done);
  } else if (count.as<int32_t>() == 0) {
    return;
  }

  auto getArg = [&](uint32_t i) {
    return toConstantOrRegister(lir, LInlineArgumentsSlice::ArgIndex(i),
                                lir->mir()->getArg(i)->type());
  };

  auto storeArg = [&](uint32_t i, auto dest) {
    // We don't need a pre-barrier because the element at |index| is guaranteed
    // to be a non-GC thing (either uninitialized memory or the magic hole
    // value).
    masm.storeConstantOrRegister(getArg(i), dest);
  };

  // Initialize all elements.
  if (numActuals == 1) {
    // There's exactly one argument. We've checked that |count| is non-zero,
    // which implies that |begin| must be zero.
    MOZ_ASSERT_IF(begin.is<int32_t>(), begin.as<int32_t>() == 0);

    Register elements = temp;
    masm.loadPtr(Address(output, NativeObject::offsetOfElements()), elements);

    storeArg(0, Address(elements, 0));
  } else if (begin.is<Register>()) {
    // There is more than one argument and |begin| isn't a compile-time
    // constant. Iterate through 0..numActuals to search for |begin| and then
    // start copying |count| arguments from that index.

    LiveGeneralRegisterSet liveRegs;
    liveRegs.add(output);
    liveRegs.add(begin.as<Register>());

    masm.PushRegsInMask(liveRegs);

    Register elements = output;
    masm.loadPtr(Address(output, NativeObject::offsetOfElements()), elements);

    Register argIndex = begin.as<Register>();

    Register index = temp;
    masm.move32(Imm32(0), index);

    Label doneLoop;
    for (uint32_t i = 0; i < numActuals; ++i) {
      Label next;
      masm.branch32(Assembler::NotEqual, argIndex, Imm32(i), &next);

      storeArg(i, BaseObjectElementIndex(elements, index));

      masm.add32(Imm32(1), index);
      masm.add32(Imm32(1), argIndex);

      if (count.is<Register>()) {
        masm.branch32(Assembler::GreaterThanOrEqual, index,
                      count.as<Register>(), &doneLoop);
      } else {
        masm.branch32(Assembler::GreaterThanOrEqual, index,
                      Imm32(count.as<int32_t>()), &doneLoop);
      }

      masm.bind(&next);
    }
    masm.bind(&doneLoop);

    masm.PopRegsInMask(liveRegs);
  } else {
    // There is more than one argument and |begin| is a compile-time constant.

    Register elements = temp;
    masm.loadPtr(Address(output, NativeObject::offsetOfElements()), elements);

    int32_t argIndex = begin.as<int32_t>();

    int32_t index = 0;

    Label doneLoop;
    for (uint32_t i = argIndex; i < numActuals; ++i) {
      storeArg(i, Address(elements, index * sizeof(Value)));

      index += 1;

      if (count.is<Register>()) {
        masm.branch32(Assembler::LessThanOrEqual, count.as<Register>(),
                      Imm32(index), &doneLoop);
      } else {
        if (index >= count.as<int32_t>()) {
          break;
        }
      }
    }
    masm.bind(&doneLoop);
  }

  // Determine if we have to emit post-write barrier.
  //
  // If either |begin| or |count| is a constant, use their value directly.
  // Otherwise assume we copy all inline arguments from 0..numActuals.
  bool postWriteBarrier = false;
  uint32_t actualBegin = begin.match([](Register) { return 0; },
                                     [](int32_t value) { return value; });
  uint32_t actualCount =
      count.match([=](Register) { return numActuals; },
                  [](int32_t value) -> uint32_t { return value; });
  for (uint32_t i = 0; i < actualCount; ++i) {
    ConstantOrRegister arg = getArg(actualBegin + i);
    if (arg.constant()) {
      Value v = arg.value();
      if (v.isGCThing() && IsInsideNursery(v.toGCThing())) {
        postWriteBarrier = true;
      }
    } else {
      MIRType type = arg.reg().type();
      if (type == MIRType::Value || NeedsPostBarrier(type)) {
        postWriteBarrier = true;
      }
    }
  }

  // Emit a post-write barrier if |output| is tenured and we couldn't
  // determine at compile-time that no barrier is needed.
  if (postWriteBarrier) {
    masm.branchPtrInNurseryChunk(Assembler::Equal, output, temp, &done);

    LiveRegisterSet volatileRegs = liveVolatileRegs(lir);
    volatileRegs.takeUnchecked(temp);
    if (output.volatile_()) {
      volatileRegs.addUnchecked(output);
    }

    masm.PushRegsInMask(volatileRegs);
    emitPostWriteBarrier(output);
    masm.PopRegsInMask(volatileRegs);
  }

  masm.bind(&done);
}

void CodeGenerator::visitNormalizeSliceTerm(LNormalizeSliceTerm* lir) {
  Register value = ToRegister(lir->value());
  Register length = ToRegister(lir->length());
  Register output = ToRegister(lir->output());

  masm.move32(value, output);

  Label positive;
  masm.branch32(Assembler::GreaterThanOrEqual, value, Imm32(0), &positive);

  Label done;
  masm.add32(length, output);
  masm.branch32(Assembler::GreaterThanOrEqual, output, Imm32(0), &done);
  masm.move32(Imm32(0), output);
  masm.jump(&done);

  masm.bind(&positive);
  masm.cmp32Move32(Assembler::LessThan, length, value, length, output);

  masm.bind(&done);
}

void CodeGenerator::visitArrayJoin(LArrayJoin* lir) {
  Label skipCall;

  Register output = ToRegister(lir->output());
  Register sep = ToRegister(lir->separator());
  Register array = ToRegister(lir->array());
  Register temp = ToRegister(lir->temp0());

  // Fast path for simple length <= 1 cases.
  {
    masm.loadPtr(Address(array, NativeObject::offsetOfElements()), temp);
    Address length(temp, ObjectElements::offsetOfLength());
    Address initLength(temp, ObjectElements::offsetOfInitializedLength());

    // Check for length == 0
    Label notEmpty;
    masm.branch32(Assembler::NotEqual, length, Imm32(0), &notEmpty);
    const JSAtomState& names = gen->runtime->names();
    masm.movePtr(ImmGCPtr(names.empty_), output);
    masm.jump(&skipCall);

    masm.bind(&notEmpty);
    Label notSingleString;
    // Check for length == 1, initializedLength >= 1, arr[0].isString()
    masm.branch32(Assembler::NotEqual, length, Imm32(1), &notSingleString);
    masm.branch32(Assembler::LessThan, initLength, Imm32(1), &notSingleString);

    Address elem0(temp, 0);
    masm.branchTestString(Assembler::NotEqual, elem0, &notSingleString);

    // At this point, 'output' can be used as a scratch register, since we're
    // guaranteed to succeed.
    masm.unboxString(elem0, output);
    masm.jump(&skipCall);
    masm.bind(&notSingleString);
  }

  pushArg(sep);
  pushArg(array);

  using Fn = JSString* (*)(JSContext*, HandleObject, HandleString);
  callVM<Fn, jit::ArrayJoin>(lir);
  masm.bind(&skipCall);
}

void CodeGenerator::visitObjectKeys(LObjectKeys* lir) {
  Register object = ToRegister(lir->object());

  pushArg(object);

  using Fn = JSObject* (*)(JSContext*, HandleObject);
  callVM<Fn, jit::ObjectKeys>(lir);
}

void CodeGenerator::visitObjectKeysLength(LObjectKeysLength* lir) {
  Register object = ToRegister(lir->object());

  pushArg(object);

  using Fn = bool (*)(JSContext*, HandleObject, int32_t*);
  callVM<Fn, jit::ObjectKeysLength>(lir);
}

void CodeGenerator::visitGetIteratorCache(LGetIteratorCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  TypedOrValueRegister val =
      toConstantOrRegister(lir, LGetIteratorCache::ValueIndex,
                           lir->mir()->value()->type())
          .reg();
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  IonGetIteratorIC ic(liveRegs, val, output, temp0, temp1);
  addIC(lir, allocateIC(ic));
}

void CodeGenerator::visitOptimizeSpreadCallCache(
    LOptimizeSpreadCallCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  ValueOperand val = ToValue(lir->value());
  ValueOperand output = ToOutValue(lir);
  Register temp = ToRegister(lir->temp0());

  IonOptimizeSpreadCallIC ic(liveRegs, val, output, temp);
  addIC(lir, allocateIC(ic));
}

void CodeGenerator::visitCloseIterCache(LCloseIterCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  Register iter = ToRegister(lir->iter());
  Register temp = ToRegister(lir->temp0());
  CompletionKind kind = CompletionKind(lir->mir()->completionKind());

  IonCloseIterIC ic(liveRegs, iter, temp, kind);
  addIC(lir, allocateIC(ic));
}

void CodeGenerator::visitOptimizeGetIteratorCache(
    LOptimizeGetIteratorCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  ValueOperand val = ToValue(lir->value());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  IonOptimizeGetIteratorIC ic(liveRegs, val, output, temp);
  addIC(lir, allocateIC(ic));
}

void CodeGenerator::visitIteratorMore(LIteratorMore* lir) {
  const Register obj = ToRegister(lir->iterator());
  const ValueOperand output = ToOutValue(lir);
  const Register temp = ToRegister(lir->temp0());

  masm.iteratorMore(obj, output, temp);
}

void CodeGenerator::visitIsNoIterAndBranch(LIsNoIterAndBranch* lir) {
  ValueOperand input = ToValue(lir->input());
  Label* ifTrue = getJumpLabelForBranch(lir->ifTrue());
  Label* ifFalse = getJumpLabelForBranch(lir->ifFalse());

  masm.branchTestMagic(Assembler::Equal, input, ifTrue);

  if (!isNextBlock(lir->ifFalse()->lir())) {
    masm.jump(ifFalse);
  }
}

void CodeGenerator::visitIteratorEnd(LIteratorEnd* lir) {
  const Register obj = ToRegister(lir->iterator());
  const Register temp0 = ToRegister(lir->temp0());
  const Register temp1 = ToRegister(lir->temp1());
  const Register temp2 = ToRegister(lir->temp2());

  masm.iteratorClose(obj, temp0, temp1, temp2);
}

void CodeGenerator::visitArgumentsLength(LArgumentsLength* lir) {
  // read number of actual arguments from the JS frame.
  Register argc = ToRegister(lir->output());
  masm.loadNumActualArgs(FramePointer, argc);
}

void CodeGenerator::visitGetFrameArgument(LGetFrameArgument* lir) {
  ValueOperand result = ToOutValue(lir);
  const LAllocation* index = lir->index();
  size_t argvOffset = JitFrameLayout::offsetOfActualArgs();

  // This instruction is used to access actual arguments and formal arguments.
  // The number of Values on the stack is |max(numFormals, numActuals)|, so we
  // assert |index < numFormals || index < numActuals| in debug builds.
  DebugOnly<size_t> numFormals = gen->outerInfo().script()->function()->nargs();

  if (index->isConstant()) {
    int32_t i = index->toConstant()->toInt32();
#ifdef DEBUG
    if (uint32_t(i) >= numFormals) {
      Label ok;
      Register argc = result.scratchReg();
      masm.loadNumActualArgs(FramePointer, argc);
      masm.branch32(Assembler::Above, argc, Imm32(i), &ok);
      masm.assumeUnreachable("Invalid argument index");
      masm.bind(&ok);
    }
#endif
    Address argPtr(FramePointer, sizeof(Value) * i + argvOffset);
    masm.loadValue(argPtr, result);
  } else {
    Register i = ToRegister(index);
#ifdef DEBUG
    Label ok;
    Register argc = result.scratchReg();
    masm.branch32(Assembler::Below, i, Imm32(numFormals), &ok);
    masm.loadNumActualArgs(FramePointer, argc);
    masm.branch32(Assembler::Above, argc, i, &ok);
    masm.assumeUnreachable("Invalid argument index");
    masm.bind(&ok);
#endif
    BaseValueIndex argPtr(FramePointer, i, argvOffset);
    masm.loadValue(argPtr, result);
  }
}

void CodeGenerator::visitGetFrameArgumentHole(LGetFrameArgumentHole* lir) {
  ValueOperand result = ToOutValue(lir);
  Register index = ToRegister(lir->index());
  Register length = ToRegister(lir->length());
  Register spectreTemp = ToTempRegisterOrInvalid(lir->temp0());
  size_t argvOffset = JitFrameLayout::offsetOfActualArgs();

  Label outOfBounds, done;
  masm.spectreBoundsCheck32(index, length, spectreTemp, &outOfBounds);

  BaseValueIndex argPtr(FramePointer, index, argvOffset);
  masm.loadValue(argPtr, result);
  masm.jump(&done);

  masm.bind(&outOfBounds);
  bailoutCmp32(Assembler::LessThan, index, Imm32(0), lir->snapshot());
  masm.moveValue(UndefinedValue(), result);

  masm.bind(&done);
}

void CodeGenerator::visitRest(LRest* lir) {
  Register numActuals = ToRegister(lir->numActuals());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register temp3 = ToRegister(lir->temp3());
  unsigned numFormals = lir->mir()->numFormals();

  // In baseline, DoRestFallback calls into NewArray to allocate the rest array.
  // If the length is 0, NewArray guesses a good capacity for it. We don't want
  // a smaller capacity in Ion, because that can lead to bailout loops.
  constexpr uint32_t arrayCapacity = 6;
  static_assert(GuessArrayGCKind(0) == GuessArrayGCKind(arrayCapacity));

  if (Shape* shape = lir->mir()->shape()) {
    uint32_t arrayLength = 0;
    gc::AllocKind allocKind = GuessArrayGCKind(arrayCapacity);
    MOZ_ASSERT(gc::GetObjectFinalizeKind(&ArrayObject::class_) ==
               gc::FinalizeKind::None);
    MOZ_ASSERT(!IsFinalizedKind(allocKind));
    MOZ_ASSERT(GetGCKindSlots(allocKind) ==
               arrayCapacity + ObjectElements::VALUES_PER_HEADER);

    Label joinAlloc, failAlloc;
    masm.movePtr(ImmGCPtr(shape), temp0);
    masm.createArrayWithFixedElements(temp2, temp0, temp1, InvalidReg,
                                      arrayLength, arrayCapacity, 0, 0,
                                      allocKind, gc::Heap::Default, &failAlloc);
    masm.jump(&joinAlloc);
    {
      masm.bind(&failAlloc);
      masm.movePtr(ImmPtr(nullptr), temp2);
    }
    masm.bind(&joinAlloc);
  } else {
    masm.movePtr(ImmPtr(nullptr), temp2);
  }

  // Set temp1 to the address of the first actual argument.
  size_t actualsOffset = JitFrameLayout::offsetOfActualArgs();
  masm.computeEffectiveAddress(Address(FramePointer, actualsOffset), temp1);

  // Compute array length: max(numActuals - numFormals, 0).
  Register lengthReg;
  if (numFormals) {
    lengthReg = temp0;
    Label emptyLength, joinLength;
    masm.branch32(Assembler::LessThanOrEqual, numActuals, Imm32(numFormals),
                  &emptyLength);
    {
      masm.move32(numActuals, lengthReg);
      masm.sub32(Imm32(numFormals), lengthReg);

      // Skip formal arguments.
      masm.addPtr(Imm32(sizeof(Value) * numFormals), temp1);

      masm.jump(&joinLength);
    }
    masm.bind(&emptyLength);
    {
      masm.move32(Imm32(0), lengthReg);

      // Leave temp1 pointed to the start of actuals() when the rest-array
      // length is zero. We don't use |actuals() + numFormals| because
      // |numFormals| can be any non-negative int32 value when this MRest was
      // created from scalar replacement optimizations. And it seems
      // questionable to compute a Value* pointer which points to who knows
      // where.
    }
    masm.bind(&joinLength);
  } else {
    // Use numActuals directly when there are no formals.
    lengthReg = numActuals;
  }

  // Try to initialize the array elements.
  Label vmCall, done;
  if (lir->mir()->shape()) {
    // Call into C++ if we failed to allocate an array or there are more than
    // |arrayCapacity| elements.
    masm.branchTestPtr(Assembler::Zero, temp2, temp2, &vmCall);
    masm.branch32(Assembler::Above, lengthReg, Imm32(arrayCapacity), &vmCall);

    // The array must be nursery allocated so no post barrier is needed.
#ifdef DEBUG
    Label ok;
    masm.branchPtrInNurseryChunk(Assembler::Equal, temp2, temp3, &ok);
    masm.assumeUnreachable("Unexpected tenured object for LRest");
    masm.bind(&ok);
#endif

    Label nonZeroLength;
    masm.branch32(Assembler::NotEqual, lengthReg, Imm32(0), &nonZeroLength);
    masm.movePtr(temp2, ReturnReg);
    masm.jump(&done);
    masm.bind(&nonZeroLength);

    // Store length and initializedLength.
    Register elements = temp3;
    masm.loadPtr(Address(temp2, NativeObject::offsetOfElements()), elements);
    Address lengthAddr(elements, ObjectElements::offsetOfLength());
    Address initLengthAddr(elements,
                           ObjectElements::offsetOfInitializedLength());
    masm.store32(lengthReg, lengthAddr);
    masm.store32(lengthReg, initLengthAddr);

    masm.push(temp2);  // Spill result to free up register.

    Register end = temp0;
    Register args = temp1;
    Register scratch = temp2;
    masm.computeEffectiveAddress(BaseValueIndex(elements, lengthReg), end);

    Label loop;
    masm.bind(&loop);
    masm.storeValue(Address(args, 0), Address(elements, 0), scratch);
    masm.addPtr(Imm32(sizeof(Value)), args);
    masm.addPtr(Imm32(sizeof(Value)), elements);
    masm.branchPtr(Assembler::Below, elements, end, &loop);

    // Pop result
    masm.pop(ReturnReg);
    masm.jump(&done);
  }

  masm.bind(&vmCall);

  pushArg(temp2);
  pushArg(temp1);
  pushArg(lengthReg);

  using Fn =
      ArrayObject* (*)(JSContext*, uint32_t, Value*, Handle<ArrayObject*>);
  callVM<Fn, InitRestParameter>(lir);

  masm.bind(&done);
}

// Create a stackmap from the given safepoint, with the structure:
//
//   <reg dump, if any>
//   |       ++ <body (general spill)>
//   |       |       ++ <space for Frame>
//   |       |               ++ <inbound args>
//   |       |                               |
//   Lowest Addr                             Highest Addr
//           |
//           framePushedAtStackMapBase
//
// The caller owns the resulting stackmap.  This assumes a grow-down stack.
//
// For non-debug builds, if the stackmap would contain no pointers, no
// stackmap is created, and nullptr is returned.  For a debug build, a
// stackmap is always created and returned.
//
// Depending on the type of safepoint, the stackmap may need to account for
// spilled registers. WasmSafepointKind::LirCall corresponds to LIR nodes where
// isCall() == true, for which the register allocator will spill/restore all
// live registers at the LIR level - in this case, the LSafepoint sees only live
// values on the stack, never in registers. WasmSafepointKind::CodegenCall, on
// the other hand, is for LIR nodes which may manually spill/restore live
// registers in codegen, in which case the stackmap must account for this. Traps
// also require tracking of live registers, but spilling is handled by the trap
// mechanism.
static bool CreateStackMapFromLSafepoint(LSafepoint& safepoint,
                                         const RegisterOffsets& trapExitLayout,
                                         size_t trapExitLayoutNumWords,
                                         size_t nInboundStackArgBytes,
                                         wasm::StackMap** result) {
  // Ensure this is defined on all return paths.
  *result = nullptr;

  // The size of the wasm::Frame itself.
  const size_t nFrameBytes = sizeof(wasm::Frame);

  // This is the number of bytes spilled for live registers, outside of a trap.
  // For traps, trapExitLayout and trapExitLayoutNumWords will be used.
  const size_t nRegisterDumpBytes =
      MacroAssembler::PushRegsInMaskSizeInBytes(safepoint.liveRegs());

  // As mentioned above, for WasmSafepointKind::LirCall, register spills and
  // restores are handled at the LIR level and there should therefore be no live
  // registers to handle here.
  MOZ_ASSERT_IF(safepoint.wasmSafepointKind() == WasmSafepointKind::LirCall,
                nRegisterDumpBytes == 0);
  MOZ_ASSERT(nRegisterDumpBytes % sizeof(void*) == 0);

  // This is the number of bytes in the general spill area, below the Frame.
  const size_t nBodyBytes = safepoint.framePushedAtStackMapBase();

  // The stack map owns any alignment padding around inbound stack args.
  const size_t nInboundStackArgBytesAligned =
      wasm::AlignStackArgAreaSize(nInboundStackArgBytes);

  // This is the number of bytes in the general spill area, the Frame, and the
  // incoming args, but not including any register dump area.
  const size_t nNonRegisterBytes =
      nBodyBytes + nFrameBytes + nInboundStackArgBytesAligned;
  MOZ_ASSERT(nNonRegisterBytes % sizeof(void*) == 0);

  // This is the number of bytes in the register dump area, if any, below the
  // general spill area.
  const size_t nRegisterBytes =
      (safepoint.wasmSafepointKind() == WasmSafepointKind::Trap)
          ? (trapExitLayoutNumWords * sizeof(void*))
          : nRegisterDumpBytes;

  // This is the total number of bytes covered by the map.
  const size_t nTotalBytes = nNonRegisterBytes + nRegisterBytes;

#ifndef DEBUG
  bool needStackMap = !(safepoint.wasmAnyRefRegs().empty() &&
                        safepoint.wasmAnyRefSlots().empty() &&
                        safepoint.slotsOrElementsSlots().empty());

  // There are no references, and this is a non-debug build, so don't bother
  // building the stackmap.
  if (!needStackMap) {
    return true;
  }
#endif

  wasm::StackMap* stackMap =
      wasm::StackMap::create(nTotalBytes / sizeof(void*));
  if (!stackMap) {
    return false;
  }
  if (safepoint.wasmSafepointKind() == WasmSafepointKind::Trap) {
    stackMap->setExitStubWords(trapExitLayoutNumWords);
  }

  // REG DUMP AREA, if any.
  size_t regDumpWords = 0;
  const LiveGeneralRegisterSet wasmAnyRefRegs = safepoint.wasmAnyRefRegs();
  const LiveGeneralRegisterSet slotsOrElementsRegs =
      safepoint.slotsOrElementsRegs();
  const LiveGeneralRegisterSet refRegs(GeneralRegisterSet::Union(
      wasmAnyRefRegs.set(), slotsOrElementsRegs.set()));
  GeneralRegisterForwardIterator refRegsIter(refRegs);
  switch (safepoint.wasmSafepointKind()) {
    case WasmSafepointKind::LirCall:
    case WasmSafepointKind::StackSwitch:
    case WasmSafepointKind::CodegenCall: {
      size_t spilledNumWords = nRegisterDumpBytes / sizeof(void*);
      regDumpWords += spilledNumWords;

      for (; refRegsIter.more(); ++refRegsIter) {
        Register reg = *refRegsIter;
        size_t offsetFromSpillBase =
            safepoint.liveRegs().gprs().offsetOfPushedRegister(reg) /
            sizeof(void*);
        MOZ_ASSERT(0 < offsetFromSpillBase &&
                   offsetFromSpillBase <= spilledNumWords);
        size_t index = spilledNumWords - offsetFromSpillBase;

        if (wasmAnyRefRegs.has(reg)) {
          stackMap->set(index, wasm::StackMap::AnyRef);
        } else {
          MOZ_ASSERT(slotsOrElementsRegs.has(reg));
          stackMap->set(index, wasm::StackMap::ArrayDataPointer);
        }
      }
      // Float and vector registers do not have to be handled; they cannot
      // contain wasm anyrefs, and they are spilled after general-purpose
      // registers. Gprs are therefore closest to the spill base and thus their
      // offset calculation does not need to account for other spills.
    } break;
    case WasmSafepointKind::Trap: {
      regDumpWords += trapExitLayoutNumWords;

      for (; refRegsIter.more(); ++refRegsIter) {
        Register reg = *refRegsIter;
        size_t offsetFromTop = trapExitLayout.getOffset(reg);

        // If this doesn't hold, the associated register wasn't saved by
        // the trap exit stub.  Better to crash now than much later, in
        // some obscure place, and possibly with security consequences.
        MOZ_RELEASE_ASSERT(offsetFromTop < trapExitLayoutNumWords);

        // offsetFromTop is an offset in words down from the highest
        // address in the exit stub save area.  Switch it around to be an
        // offset up from the bottom of the (integer register) save area.
        size_t offsetFromBottom = trapExitLayoutNumWords - 1 - offsetFromTop;

        if (wasmAnyRefRegs.has(reg)) {
          stackMap->set(offsetFromBottom, wasm::StackMap::AnyRef);
        } else {
          MOZ_ASSERT(slotsOrElementsRegs.has(reg));
          stackMap->set(offsetFromBottom, wasm::StackMap::ArrayDataPointer);
        }
      }
    } break;
    default:
      MOZ_CRASH("unreachable");
  }

  // Ensure other reg/slot collections on LSafepoint are empty.
  MOZ_ASSERT(safepoint.gcRegs().empty() && safepoint.gcSlots().empty());
#ifdef JS_NUNBOX32
  MOZ_ASSERT(safepoint.nunboxParts().empty());
#elif JS_PUNBOX64
  MOZ_ASSERT(safepoint.valueRegs().empty() && safepoint.valueSlots().empty());
#endif

  // BODY (GENERAL SPILL) AREA and FRAME and INCOMING ARGS
  // Deal with roots on the stack.
  const LSafepoint::SlotList& wasmAnyRefSlots = safepoint.wasmAnyRefSlots();
  for (SafepointSlotEntry wasmAnyRefSlot : wasmAnyRefSlots) {
    // The following needs to correspond with JitFrameLayout::slotRef
    // wasmAnyRefSlot.stack == 0 means the slot is in the args area
    if (wasmAnyRefSlot.stack) {
      // It's a slot in the body allocation, so .slot is interpreted
      // as an index downwards from the Frame*
      MOZ_ASSERT(wasmAnyRefSlot.slot <= nBodyBytes);
      uint32_t offsetInBytes = nBodyBytes - wasmAnyRefSlot.slot;
      MOZ_ASSERT(offsetInBytes % sizeof(void*) == 0);
      stackMap->set(regDumpWords + offsetInBytes / sizeof(void*),
                    wasm::StackMap::AnyRef);
    } else {
      // It's an argument slot
      MOZ_ASSERT(wasmAnyRefSlot.slot < nInboundStackArgBytes);
      uint32_t offsetInBytes = nBodyBytes + nFrameBytes + wasmAnyRefSlot.slot;
      MOZ_ASSERT(offsetInBytes % sizeof(void*) == 0);
      stackMap->set(regDumpWords + offsetInBytes / sizeof(void*),
                    wasm::StackMap::AnyRef);
    }
  }

  // Track array data pointers on the stack
  const LSafepoint::SlotList& slots = safepoint.slotsOrElementsSlots();
  for (SafepointSlotEntry slot : slots) {
    MOZ_ASSERT(slot.stack);

    // It's a slot in the body allocation, so .slot is interpreted
    // as an index downwards from the Frame*
    MOZ_ASSERT(slot.slot <= nBodyBytes);
    uint32_t offsetInBytes = nBodyBytes - slot.slot;
    MOZ_ASSERT(offsetInBytes % sizeof(void*) == 0);
    stackMap->set(regDumpWords + offsetInBytes / sizeof(void*),
                  wasm::StackMap::Kind::ArrayDataPointer);
  }

  // Record in the map, how far down from the highest address the Frame* is.
  // Take the opportunity to check that we haven't marked any part of the
  // Frame itself as a pointer.
  stackMap->setFrameOffsetFromTop((nInboundStackArgBytesAligned + nFrameBytes) /
                                  sizeof(void*));
#ifdef DEBUG
  for (uint32_t i = 0; i < nFrameBytes / sizeof(void*); i++) {
    MOZ_ASSERT(stackMap->get(stackMap->header.numMappedWords -
                             stackMap->header.frameOffsetFromTop + i) ==
               wasm::StackMap::Kind::POD);
  }
#endif

  *result = stackMap;
  return true;
}

bool CodeGenerator::generateWasm(wasm::CallIndirectId callIndirectId,
                                 const wasm::TrapSiteDesc& entryTrapSiteDesc,
                                 const wasm::ArgTypeVector& argTypes,
                                 const RegisterOffsets& trapExitLayout,
                                 size_t trapExitLayoutNumWords,
                                 wasm::FuncOffsets* offsets,
                                 wasm::StackMaps* stackMaps,
                                 wasm::Decoder* decoder,
                                 jit::IonPerfSpewer* spewer) {
  AutoCreatedBy acb(masm, "CodeGenerator::generateWasm");

  JitSpew(JitSpew_Codegen, "# Emitting wasm code");

  size_t nInboundStackArgBytes = StackArgAreaSizeUnaligned(argTypes);
  inboundStackArgBytes_ = nInboundStackArgBytes;

  perfSpewer_.markStartOffset(masm.currentOffset());
  perfSpewer_.recordOffset(masm, "Prologue");
  wasm::GenerateFunctionPrologue(masm, callIndirectId, mozilla::Nothing(),
                                 offsets);

#ifdef DEBUG
  // If we are doing full debug checks, always load the instance pointer into
  // the usual spot in the frame so that it can be loaded later regardless of
  // what is in InstanceReg. See CodeGenerator::emitDebugResultChecks.
  if (JitOptions.fullDebugChecks) {
    masm.storePtr(InstanceReg,
                  Address(FramePointer,
                          wasm::FrameWithInstances::calleeInstanceOffset()));
  }
#endif

  MOZ_ASSERT(masm.framePushed() == 0);

  // Very large frames are implausible, probably an attack.
  if (frameSize() > wasm::MaxFrameSize) {
    return decoder->fail(decoder->beginOffset(), "stack frame is too large");
  }

  if (omitOverRecursedCheck()) {
    masm.reserveStack(frameSize());
  } else {
    std::pair<CodeOffset, uint32_t> pair =
        masm.wasmReserveStackChecked(frameSize(), entryTrapSiteDesc);
    CodeOffset trapInsnOffset = pair.first;
    size_t nBytesReservedBeforeTrap = pair.second;

    wasm::StackMap* functionEntryStackMap = nullptr;
    if (!CreateStackMapForFunctionEntryTrap(
            argTypes, trapExitLayout, trapExitLayoutNumWords,
            nBytesReservedBeforeTrap, nInboundStackArgBytes,
            &functionEntryStackMap)) {
      return false;
    }

    // In debug builds, we'll always have a stack map, even if there are no
    // refs to track.
    MOZ_ASSERT(functionEntryStackMap);

    if (functionEntryStackMap &&
        !stackMaps->add(trapInsnOffset.offset(), functionEntryStackMap)) {
      functionEntryStackMap->destroy();
      return false;
    }
  }

  MOZ_ASSERT(masm.framePushed() == frameSize());

  if (!generateBody()) {
    return false;
  }

  perfSpewer_.recordOffset(masm, "Epilogue");
  masm.bind(&returnLabel_);
  wasm::GenerateFunctionEpilogue(masm, frameSize(), offsets);

  perfSpewer_.recordOffset(masm, "OOLCode");
  if (!generateOutOfLineCode()) {
    return false;
  }

  masm.flush();
  if (masm.oom()) {
    return false;
  }

  offsets->end = masm.currentOffset();

  MOZ_ASSERT(!masm.failureLabel()->used());
  MOZ_ASSERT(snapshots_.listSize() == 0);
  MOZ_ASSERT(snapshots_.RVATableSize() == 0);
  MOZ_ASSERT(recovers_.size() == 0);
  MOZ_ASSERT(graph.numConstants() == 0);
  MOZ_ASSERT(osiIndices_.empty());
  MOZ_ASSERT(icList_.empty());
  MOZ_ASSERT(safepoints_.size() == 0);
  MOZ_ASSERT(!scriptCounts_);

  // Convert the safepoints to stackmaps and add them to our running
  // collection thereof.
  for (CodegenSafepointIndex& index : safepointIndices_) {
    wasm::StackMap* stackMap = nullptr;
    if (!CreateStackMapFromLSafepoint(*index.safepoint(), trapExitLayout,
                                      trapExitLayoutNumWords,
                                      nInboundStackArgBytes, &stackMap)) {
      return false;
    }

    // In debug builds, we'll always have a stack map.
    MOZ_ASSERT(stackMap);
    if (!stackMap) {
      continue;
    }

    if (!stackMaps->add(index.displacement(), stackMap)) {
      stackMap->destroy();
      return false;
    }
  }

  *spewer = std::move(perfSpewer_);
  return true;
}

bool CodeGenerator::generate(const WarpSnapshot* snapshot) {
  AutoCreatedBy acb(masm, "CodeGenerator::generate");

  MOZ_ASSERT(snapshot);
  snapshot_ = snapshot;

  JitSpew(JitSpew_Codegen, "# Emitting code for script %s:%u:%u",
          gen->outerInfo().script()->filename(),
          gen->outerInfo().script()->lineno(),
          gen->outerInfo().script()->column().oneOriginValue());

  // Initialize native code table with an entry to the start of
  // top-level script.
  InlineScriptTree* tree = gen->outerInfo().inlineScriptTree();
  jsbytecode* startPC = tree->script()->code();
  BytecodeSite* startSite = new (gen->alloc()) BytecodeSite(tree, startPC);
  if (!addNativeToBytecodeEntry(startSite)) {
    return false;
  }

  if (!safepoints_.init(gen->alloc())) {
    return false;
  }

  size_t maxSafepointIndices =
      graph.numSafepoints() + graph.extraSafepointUses();
  if (!safepointIndices_.reserve(maxSafepointIndices)) {
    return false;
  }
  if (!osiIndices_.reserve(graph.numSafepoints())) {
    return false;
  }

  perfSpewer_.recordOffset(masm, "Prologue");
  if (!generatePrologue()) {
    return false;
  }

  // Reset native => bytecode map table with top-level script and startPc.
  if (!addNativeToBytecodeEntry(startSite)) {
    return false;
  }

  if (!generateBody()) {
    return false;
  }

  // Reset native => bytecode map table with top-level script and startPc.
  if (!addNativeToBytecodeEntry(startSite)) {
    return false;
  }

  perfSpewer_.recordOffset(masm, "Epilogue");
  if (!generateEpilogue()) {
    return false;
  }

  // Reset native => bytecode map table with top-level script and startPc.
  if (!addNativeToBytecodeEntry(startSite)) {
    return false;
  }

  perfSpewer_.recordOffset(masm, "InvalidateEpilogue");
  generateInvalidateEpilogue();

  // native => bytecode entries for OOL code will be added
  // by CodeGeneratorShared::generateOutOfLineCode
  perfSpewer_.recordOffset(masm, "OOLCode");
  if (!generateOutOfLineCode()) {
    return false;
  }

  // Add terminal entry.
  if (!addNativeToBytecodeEntry(startSite)) {
    return false;
  }

  // Dump Native to bytecode entries to spew.
  dumpNativeToBytecodeEntries();

  // We encode safepoints after the OSI-point offsets have been determined.
  if (!encodeSafepoints()) {
    return false;
  }

  // If this assertion trips, then you have multiple things to do:
  //
  // This assertion will report if a safepoint is used multiple times for the
  // same instruction. To fix this assertion make sure to call
  // `lirGraph_.addExtraSafepointUses(..);` in the Lowering phase.
  //
  // However, this non-worrying issue might hide a more dramatic security issue,
  // which is that having multiple encoding of a safepoint in a single LIR
  // instruction is not safe, unless:
  //
  //   - The multiple uses of the safepoints are in different code path. i-e
  //     there should be not single execution trace making use of multiple
  //     calls within a single instruction.
  //
  //   - There is enough space to encode data in-place of the call instruction.
  //     Such that a patched-call site does not corrupt the code path on another
  //     execution trace.
  //
  // This issue is caused by the way invalidation works, to keep the code alive
  // when invalidated code is only referenced by the stack. This works by
  // storing data in-place of the calling code, which thus becomes unsafe to
  // execute.
  MOZ_ASSERT(safepointIndices_.length() <= maxSafepointIndices);

  // For each instruction with a safepoint, we have an OSI point inserted after
  // which handles bailouts in case of invalidation of the code.
  MOZ_ASSERT(osiIndices_.length() == graph.numSafepoints());

  return !masm.oom();
}

static bool AddInlinedCompilations(JSContext* cx, HandleScript script,
                                   IonCompilationId compilationId,
                                   const WarpSnapshot* snapshot,
                                   bool* isValid) {
  MOZ_ASSERT(!*isValid);
  RecompileInfo recompileInfo(script, compilationId);

  JitZone* jitZone = cx->zone()->jitZone();

  for (const auto* scriptSnapshot : snapshot->scripts()) {
    JSScript* inlinedScript = scriptSnapshot->script();
    if (inlinedScript == script) {
      continue;
    }

    // TODO(post-Warp): This matches FinishCompilation and is necessary to
    // ensure in-progress compilations are canceled when an inlined functon
    // becomes a debuggee. See the breakpoint-14.js jit-test.
    // When TI is gone, try to clean this up by moving AddInlinedCompilations to
    // WarpOracle so that we can handle this as part of addPendingRecompile
    // instead of requiring this separate check.
    if (inlinedScript->isDebuggee()) {
      *isValid = false;
      return true;
    }

    if (!jitZone->addInlinedCompilation(recompileInfo, inlinedScript)) {
      return false;
    }
  }

  *isValid = true;
  return true;
}

struct EmulatesUndefinedDependency final : public CompilationDependency {
  explicit EmulatesUndefinedDependency()
      : CompilationDependency(CompilationDependency::Type::EmulatesUndefined) {
        };

  virtual bool operator==(const CompilationDependency& dep) const override {
    // Since the emulates undefined fuse is runtime wide, they are all equal
    return dep.type == type;
  }

  virtual bool checkDependency(JSContext* cx) override {
    return cx->runtime()->hasSeenObjectEmulateUndefinedFuse.ref().intact();
  }

  virtual bool registerDependency(JSContext* cx, HandleScript script) override {
    MOZ_ASSERT(checkDependency(cx));
    return cx->runtime()
        ->hasSeenObjectEmulateUndefinedFuse.ref()
        .addFuseDependency(cx, script);
  }

  virtual UniquePtr<CompilationDependency> clone() const override {
    return MakeUnique<EmulatesUndefinedDependency>();
  }
};

struct ArrayExceedsInt32LengthDependency final : public CompilationDependency {
  explicit ArrayExceedsInt32LengthDependency()
      : CompilationDependency(
            CompilationDependency::Type::ArrayExceedsInt32Length) {};

  virtual bool operator==(const CompilationDependency& dep) const override {
    return dep.type == type;
  }

  virtual bool checkDependency(JSContext* cx) override {
    return cx->runtime()->hasSeenArrayExceedsInt32LengthFuse.ref().intact();
  }

  virtual bool registerDependency(JSContext* cx, HandleScript script) override {
    MOZ_ASSERT(checkDependency(cx));
    return cx->runtime()
        ->hasSeenArrayExceedsInt32LengthFuse.ref()
        .addFuseDependency(cx, script);
  }

  virtual UniquePtr<CompilationDependency> clone() const override {
    return MakeUnique<ArrayExceedsInt32LengthDependency>();
  }
};

bool CodeGenerator::addHasSeenObjectEmulateUndefinedFuseDependency() {
  EmulatesUndefinedDependency dep;
  return mirGen().tracker.addDependency(dep);
}

bool CodeGenerator::addHasSeenArrayExceedsInt32LengthFuseDependency() {
  ArrayExceedsInt32LengthDependency dep;
  return mirGen().tracker.addDependency(dep);
}

bool CodeGenerator::link(JSContext* cx) {
  AutoCreatedBy acb(masm, "CodeGenerator::link");

  // We cancel off-thread Ion compilations in a few places during GC, but if
  // this compilation was performed off-thread it will already have been
  // removed from the relevant lists by this point. Don't allow GC here.
  JS::AutoAssertNoGC nogc(cx);

  RootedScript script(cx, gen->outerInfo().script());
  MOZ_ASSERT(!script->hasIonScript());

  if (scriptCounts_ && !script->hasScriptCounts() &&
      !script->initScriptCounts(cx)) {
    return false;
  }

  JitZone* jitZone = cx->zone()->jitZone();

  IonCompilationId compilationId =
      cx->runtime()->jitRuntime()->nextCompilationId();
  jitZone->currentCompilationIdRef().emplace(compilationId);
  auto resetCurrentId = mozilla::MakeScopeExit(
      [jitZone] { jitZone->currentCompilationIdRef().reset(); });

  // Record constraints. If an error occured, returns false and potentially
  // prevent future compilations. Otherwise, if an invalidation occured, then
  // skip the current compilation.
  bool isValid = false;

  // If an inlined script is invalidated (for example, by attaching
  // a debugger), we must also invalidate the parent IonScript.
  if (!AddInlinedCompilations(cx, script, compilationId, snapshot_, &isValid)) {
    return false;
  }

  // This compilation is no longer valid; don't proceed, but return true as this
  // isn't an error case either.
  if (!isValid) {
    return true;
  }

  CompilationDependencyTracker& tracker = mirGen().tracker;
  // Make sure we're using the same realm as this context.
  MOZ_ASSERT(mirGen().realm->realmPtr() == cx->realm());
  if (!tracker.checkDependencies(cx)) {
    return true;
  }

  for (auto& dep : tracker.dependencies) {
    if (!dep->registerDependency(cx, script)) {
      return false;  // Should we make sure we only return false on OOM and then
                     // eat the OOM here?
    }
  }

  uint32_t argumentSlots = (gen->outerInfo().nargs() + 1) * sizeof(Value);

  size_t numNurseryObjects = snapshot_->nurseryObjects().length();

  IonScript* ionScript = IonScript::New(
      cx, compilationId, graph.localSlotsSize(), argumentSlots, frameDepth_,
      snapshots_.listSize(), snapshots_.RVATableSize(), recovers_.size(),
      graph.numConstants(), numNurseryObjects, safepointIndices_.length(),
      osiIndices_.length(), icList_.length(), runtimeData_.length(),
      safepoints_.size());
  if (!ionScript) {
    return false;
  }
#ifdef DEBUG
  ionScript->setICHash(snapshot_->icHash());
#endif

  auto freeIonScript = mozilla::MakeScopeExit([&ionScript] {
    // Use js_free instead of IonScript::Destroy: the cache list is still
    // uninitialized.
    js_free(ionScript);
  });

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Ion);
  if (!code) {
    return false;
  }

  // Encode native to bytecode map if profiling is enabled.
  if (isProfilerInstrumentationEnabled()) {
    // Generate native-to-bytecode main table.
    IonEntry::ScriptList scriptList;
    if (!generateCompactNativeToBytecodeMap(cx, code, scriptList)) {
      return false;
    }

    uint8_t* ionTableAddr =
        ((uint8_t*)nativeToBytecodeMap_.get()) + nativeToBytecodeTableOffset_;
    JitcodeIonTable* ionTable = (JitcodeIonTable*)ionTableAddr;

    // Construct the IonEntry that will go into the global table.
    auto entry = MakeJitcodeGlobalEntry<IonEntry>(
        cx, code, code->raw(), code->rawEnd(), std::move(scriptList), ionTable);
    if (!entry) {
      return false;
    }
    (void)nativeToBytecodeMap_.release();  // Table is now owned by |entry|.

    // Add entry to the global table.
    JitcodeGlobalTable* globalTable =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(std::move(entry))) {
      return false;
    }

    // Mark the jitcode as having a bytecode map.
    code->setHasBytecodeMap();
  } else {
    // Add a dumy jitcodeGlobalTable entry.
    auto entry = MakeJitcodeGlobalEntry<DummyEntry>(cx, code, code->raw(),
                                                    code->rawEnd());
    if (!entry) {
      return false;
    }

    // Add entry to the global table.
    JitcodeGlobalTable* globalTable =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(std::move(entry))) {
      return false;
    }

    // Mark the jitcode as having a bytecode map.
    code->setHasBytecodeMap();
  }

  ionScript->setMethod(code);

  // If the Gecko Profiler is enabled, mark IonScript as having been
  // instrumented accordingly.
  if (isProfilerInstrumentationEnabled()) {
    ionScript->setHasProfilingInstrumentation();
  }

  Assembler::PatchDataWithValueCheck(
      CodeLocationLabel(code, invalidateEpilogueData_), ImmPtr(ionScript),
      ImmPtr((void*)-1));

  for (CodeOffset offset : ionScriptLabels_) {
    Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, offset),
                                       ImmPtr(ionScript), ImmPtr((void*)-1));
  }

  for (NurseryObjectLabel label : ionNurseryObjectLabels_) {
    void* entry = ionScript->addressOfNurseryObject(label.nurseryIndex);
    Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, label.offset),
                                       ImmPtr(entry), ImmPtr((void*)-1));
  }

  // for generating inline caches during the execution.
  if (runtimeData_.length()) {
    ionScript->copyRuntimeData(&runtimeData_[0]);
  }
  if (icList_.length()) {
    ionScript->copyICEntries(&icList_[0]);
  }

  for (size_t i = 0; i < icInfo_.length(); i++) {
    IonIC& ic = ionScript->getICFromIndex(i);
    Assembler::PatchDataWithValueCheck(
        CodeLocationLabel(code, icInfo_[i].icOffsetForJump),
        ImmPtr(ic.codeRawPtr()), ImmPtr((void*)-1));
    Assembler::PatchDataWithValueCheck(
        CodeLocationLabel(code, icInfo_[i].icOffsetForPush), ImmPtr(&ic),
        ImmPtr((void*)-1));
  }

  JitSpew(JitSpew_Codegen, "Created IonScript %p (raw %p)", (void*)ionScript,
          (void*)code->raw());

  ionScript->setInvalidationEpilogueDataOffset(
      invalidateEpilogueData_.offset());
  if (jsbytecode* osrPc = gen->outerInfo().osrPc()) {
    ionScript->setOsrPc(osrPc);
    ionScript->setOsrEntryOffset(getOsrEntryOffset());
  }
  ionScript->setInvalidationEpilogueOffset(invalidate_.offset());

  perfSpewer_.saveJSProfile(cx, script, code);

#ifdef MOZ_VTUNE
  vtune::MarkScript(code, script, "ion");
#endif

  // Set a Ion counter hint for this script.
  if (cx->runtime()->jitRuntime()->hasJitHintsMap()) {
    JitHintsMap* jitHints = cx->runtime()->jitRuntime()->getJitHintsMap();
    jitHints->recordIonCompilation(script);
  }

  // for marking during GC.
  if (safepointIndices_.length()) {
    ionScript->copySafepointIndices(&safepointIndices_[0]);
  }
  if (safepoints_.size()) {
    ionScript->copySafepoints(&safepoints_);
  }

  // for recovering from an Ion Frame.
  if (osiIndices_.length()) {
    ionScript->copyOsiIndices(&osiIndices_[0]);
  }
  if (snapshots_.listSize()) {
    ionScript->copySnapshots(&snapshots_);
  }
  MOZ_ASSERT_IF(snapshots_.listSize(), recovers_.size());
  if (recovers_.size()) {
    ionScript->copyRecovers(&recovers_);
  }
  if (graph.numConstants()) {
    const Value* vp = graph.constantPool();
    ionScript->copyConstants(vp);
    for (size_t i = 0; i < graph.numConstants(); i++) {
      const Value& v = vp[i];
      if (v.isGCThing()) {
        if (gc::StoreBuffer* sb = v.toGCThing()->storeBuffer()) {
          sb->putWholeCell(script);
          break;
        }
      }
    }
  }

  // Attach any generated script counts to the script.
  if (IonScriptCounts* counts = extractScriptCounts()) {
    script->addIonCounts(counts);
  }
  // WARNING: Code after this point must be infallible!

  // Copy the list of nursery objects. Note that the store buffer can add
  // HeapPtr edges that must be cleared in IonScript::Destroy. See the
  // infallibility warning above.
  const auto& nurseryObjects = snapshot_->nurseryObjects();
  for (size_t i = 0; i < nurseryObjects.length(); i++) {
    ionScript->nurseryObjects()[i].init(nurseryObjects[i]);
  }

  // Transfer ownership of the IonScript to the JitScript. At this point enough
  // of the IonScript must be initialized for IonScript::Destroy to work.
  freeIonScript.release();
  script->jitScript()->setIonScript(script, ionScript);

  return true;
}

void CodeGenerator::visitUnboxFloatingPoint(LUnboxFloatingPoint* lir) {
  ValueOperand box = ToValue(lir->input());
  const LDefinition* result = lir->output();

  // Out-of-line path to convert int32 to double or bailout
  // if this instruction is fallible.
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    ValueOperand value = ToValue(lir->input());

    if (lir->mir()->fallible()) {
      Label bail;
      masm.branchTestInt32(Assembler::NotEqual, value, &bail);
      bailoutFrom(&bail, lir->snapshot());
    }
    masm.convertInt32ToDouble(value.payloadOrValueReg(),
                              ToFloatRegister(lir->output()));
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  FloatRegister resultReg = ToFloatRegister(result);
  masm.branchTestDouble(Assembler::NotEqual, box, ool->entry());
  masm.unboxDouble(box, resultReg);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCallBindVar(LCallBindVar* lir) {
  pushArg(ToRegister(lir->environmentChain()));

  using Fn = JSObject* (*)(JSContext*, JSObject*);
  callVM<Fn, BindVarOperation>(lir);
}

void CodeGenerator::visitMegamorphicSetElement(LMegamorphicSetElement* lir) {
  Register obj = ToRegister(lir->object());
  ValueOperand idVal = ToValue(lir->index());
  ValueOperand value = ToValue(lir->value());

  Register temp0 = ToRegister(lir->temp0());
  // See comment in LIROps.yaml (x86 is short on registers)
#ifndef JS_CODEGEN_X86
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
#endif

  // The instruction is marked as call-instruction so only these registers are
  // live.
  LiveRegisterSet liveRegs;
  liveRegs.addUnchecked(obj);
  liveRegs.addUnchecked(idVal);
  liveRegs.addUnchecked(value);
  liveRegs.addUnchecked(temp0);
#ifndef JS_CODEGEN_X86
  liveRegs.addUnchecked(temp1);
  liveRegs.addUnchecked(temp2);
#endif

  Label cacheHit, done;
#ifdef JS_CODEGEN_X86
  masm.emitMegamorphicCachedSetSlot(
      idVal, obj, temp0, value, liveRegs, &cacheHit,
      [](MacroAssembler& masm, const Address& addr, MIRType mirType) {
        EmitPreBarrier(masm, addr, mirType);
      });
#else
  masm.emitMegamorphicCachedSetSlot(
      idVal, obj, temp0, temp1, temp2, value, liveRegs, &cacheHit,
      [](MacroAssembler& masm, const Address& addr, MIRType mirType) {
        EmitPreBarrier(masm, addr, mirType);
      });
#endif

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(ToValue(lir->value()));
  pushArg(ToValue(lir->index()));
  pushArg(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue, bool);
  callVM<Fn, js::jit::SetElementMegamorphic<true>>(lir);

  masm.jump(&done);
  masm.bind(&cacheHit);

  masm.branchPtrInNurseryChunk(Assembler::Equal, obj, temp0, &done);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, value, temp0, &done);

  // Note: because this is a call-instruction, no registers need to be saved.
  MOZ_ASSERT(lir->isCall());
  emitPostWriteBarrier(obj);

  masm.bind(&done);
}

void CodeGenerator::visitLoadScriptedProxyHandler(
    LLoadScriptedProxyHandler* ins) {
  Register obj = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), output);

  Label bail;
  Address handlerAddr(output, js::detail::ProxyReservedSlots::offsetOfSlot(
                                  ScriptedProxyHandler::HANDLER_EXTRA));
  masm.fallibleUnboxObject(handlerAddr, output, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

#ifdef JS_PUNBOX64
void CodeGenerator::visitCheckScriptedProxyGetResult(
    LCheckScriptedProxyGetResult* ins) {
  ValueOperand target = ToValue(ins->target());
  ValueOperand value = ToValue(ins->value());
  ValueOperand id = ToValue(ins->id());
  Register scratch = ToRegister(ins->temp0());
  Register scratch2 = ToRegister(ins->temp1());

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue,
                      MutableHandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, CheckProxyGetByValueResult>(
      ins, ArgList(scratch, id, value), StoreValueTo(value));

  masm.unboxObject(target, scratch);
  masm.branchTestObjectNeedsProxyResultValidation(Assembler::NonZero, scratch,
                                                  scratch2, ool->entry());
  masm.bind(ool->rejoin());
}
#endif

void CodeGenerator::visitIdToStringOrSymbol(LIdToStringOrSymbol* ins) {
  ValueOperand id = ToValue(ins->idVal());
  ValueOperand output = ToOutValue(ins);
  Register scratch = ToRegister(ins->temp0());

  masm.moveValue(id, output);

  Label done, callVM;
  Label bail;
  {
    ScratchTagScope tag(masm, output);
    masm.splitTagForTest(output, tag);
    masm.branchTestString(Assembler::Equal, tag, &done);
    masm.branchTestSymbol(Assembler::Equal, tag, &done);
    masm.branchTestInt32(Assembler::NotEqual, tag, &bail);
  }

  masm.unboxInt32(output, scratch);

  using Fn = JSLinearString* (*)(JSContext*, int);
  OutOfLineCode* ool = oolCallVM<Fn, Int32ToString<CanGC>>(
      ins, ArgList(scratch), StoreRegisterTo(output.scratchReg()));

  masm.lookupStaticIntString(scratch, output.scratchReg(),
                             gen->runtime->staticStrings(), ool->entry());

  masm.bind(ool->rejoin());
  masm.tagValue(JSVAL_TYPE_STRING, output.scratchReg(), output);
  masm.bind(&done);

  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitLoadFixedSlotV(LLoadFixedSlotV* ins) {
  const Register obj = ToRegister(ins->object());
  size_t slot = ins->mir()->slot();
  ValueOperand result = ToOutValue(ins);

  masm.loadValue(Address(obj, NativeObject::getFixedSlotOffset(slot)), result);
}

void CodeGenerator::visitLoadFixedSlotT(LLoadFixedSlotT* ins) {
  const Register obj = ToRegister(ins->object());
  size_t slot = ins->mir()->slot();
  AnyRegister result = ToAnyRegister(ins->output());
  MIRType type = ins->mir()->type();

  masm.loadUnboxedValue(Address(obj, NativeObject::getFixedSlotOffset(slot)),
                        type, result);
}

template <typename T>
static void EmitLoadAndUnbox(MacroAssembler& masm, const T& src, MIRType type,
                             bool fallible, AnyRegister dest, Label* fail) {
  if (type == MIRType::Double) {
    MOZ_ASSERT(dest.isFloat());
    masm.ensureDouble(src, dest.fpu(), fail);
    return;
  }
  if (fallible) {
    switch (type) {
      case MIRType::Int32:
        masm.fallibleUnboxInt32(src, dest.gpr(), fail);
        break;
      case MIRType::Boolean:
        masm.fallibleUnboxBoolean(src, dest.gpr(), fail);
        break;
      case MIRType::Object:
        masm.fallibleUnboxObject(src, dest.gpr(), fail);
        break;
      case MIRType::String:
        masm.fallibleUnboxString(src, dest.gpr(), fail);
        break;
      case MIRType::Symbol:
        masm.fallibleUnboxSymbol(src, dest.gpr(), fail);
        break;
      case MIRType::BigInt:
        masm.fallibleUnboxBigInt(src, dest.gpr(), fail);
        break;
      default:
        MOZ_CRASH("Unexpected MIRType");
    }
    return;
  }
  masm.loadUnboxedValue(src, type, dest);
}

void CodeGenerator::visitLoadFixedSlotAndUnbox(LLoadFixedSlotAndUnbox* ins) {
  const MLoadFixedSlotAndUnbox* mir = ins->mir();
  MIRType type = mir->type();
  Register input = ToRegister(ins->object());
  AnyRegister result = ToAnyRegister(ins->output());
  size_t slot = mir->slot();

  Address address(input, NativeObject::getFixedSlotOffset(slot));

  Label bail;
  EmitLoadAndUnbox(masm, address, type, mir->fallible(), result, &bail);
  if (mir->fallible()) {
    bailoutFrom(&bail, ins->snapshot());
  }
}

void CodeGenerator::visitLoadDynamicSlotAndUnbox(
    LLoadDynamicSlotAndUnbox* ins) {
  const MLoadDynamicSlotAndUnbox* mir = ins->mir();
  MIRType type = mir->type();
  Register input = ToRegister(ins->slots());
  AnyRegister result = ToAnyRegister(ins->output());
  size_t slot = mir->slot();

  Address address(input, slot * sizeof(JS::Value));

  Label bail;
  EmitLoadAndUnbox(masm, address, type, mir->fallible(), result, &bail);
  if (mir->fallible()) {
    bailoutFrom(&bail, ins->snapshot());
  }
}

void CodeGenerator::visitLoadElementAndUnbox(LLoadElementAndUnbox* ins) {
  const MLoadElementAndUnbox* mir = ins->mir();
  MIRType type = mir->type();
  Register elements = ToRegister(ins->elements());
  AnyRegister result = ToAnyRegister(ins->output());

  Label bail;
  if (ins->index()->isConstant()) {
    NativeObject::elementsSizeMustNotOverflow();
    int32_t offset = ToInt32(ins->index()) * sizeof(Value);
    Address address(elements, offset);
    EmitLoadAndUnbox(masm, address, type, mir->fallible(), result, &bail);
  } else {
    BaseObjectElementIndex address(elements, ToRegister(ins->index()));
    EmitLoadAndUnbox(masm, address, type, mir->fallible(), result, &bail);
  }

  if (mir->fallible()) {
    bailoutFrom(&bail, ins->snapshot());
  }
}

void CodeGenerator::emitMaybeAtomizeSlot(LInstruction* ins, Register stringReg,
                                         Address slotAddr,
                                         TypedOrValueRegister dest) {
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    // This code is called with a non-atomic string in |stringReg|.
    // When it returns, |stringReg| contains an unboxed pointer to an
    // atomized version of that string, and |slotAddr| contains a
    // StringValue pointing to that atom. If |dest| is a ValueOperand,
    // it contains the same StringValue; otherwise we assert that |dest|
    // is |stringReg|.

    saveLive(ins);
    pushArg(stringReg);

    using Fn = JSAtom* (*)(JSContext*, JSString*);
    callVM<Fn, js::AtomizeString>(ins);
    StoreRegisterTo(stringReg).generate(this);
    restoreLiveIgnore(ins, StoreRegisterTo(stringReg).clobbered());

    if (dest.hasValue()) {
      masm.moveValue(
          TypedOrValueRegister(MIRType::String, AnyRegister(stringReg)),
          dest.valueReg());
    } else {
      MOZ_ASSERT(dest.typedReg().gpr() == stringReg);
    }

    emitPreBarrier(slotAddr);
    masm.storeTypedOrValue(dest, slotAddr);

    // We don't need a post-barrier because atoms aren't nursery-allocated.
#ifdef DEBUG
    // We need a temp register for the nursery check. Spill something.
    AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
    allRegs.take(stringReg);
    Register temp = allRegs.takeAny();
    masm.push(temp);

    Label tenured;
    masm.branchPtrInNurseryChunk(Assembler::NotEqual, stringReg, temp,
                                 &tenured);
    masm.assumeUnreachable("AtomizeString returned a nursery pointer");
    masm.bind(&tenured);

    masm.pop(temp);
#endif

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, ins->mirRaw()->toInstruction());
  masm.branchTest32(Assembler::NonZero,
                    Address(stringReg, JSString::offsetOfFlags()),
                    Imm32(JSString::ATOM_BIT), ool->rejoin());

  masm.branchTest32(Assembler::Zero,
                    Address(stringReg, JSString::offsetOfFlags()),
                    Imm32(JSString::ATOM_REF_BIT), ool->entry());
  masm.loadPtr(Address(stringReg, JSAtomRefString::offsetOfAtom()), stringReg);

  if (dest.hasValue()) {
    masm.moveValue(
        TypedOrValueRegister(MIRType::String, AnyRegister(stringReg)),
        dest.valueReg());
  } else {
    MOZ_ASSERT(dest.typedReg().gpr() == stringReg);
  }

  emitPreBarrier(slotAddr);
  masm.storeTypedOrValue(dest, slotAddr);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitLoadFixedSlotAndAtomize(
    LLoadFixedSlotAndAtomize* ins) {
  Register obj = ToRegister(ins->object());
  Register temp = ToRegister(ins->temp0());
  size_t slot = ins->mir()->slot();
  ValueOperand result = ToOutValue(ins);

  Address slotAddr(obj, NativeObject::getFixedSlotOffset(slot));
  masm.loadValue(slotAddr, result);

  Label notString;
  masm.branchTestString(Assembler::NotEqual, result, &notString);
  masm.unboxString(result, temp);
  emitMaybeAtomizeSlot(ins, temp, slotAddr, result);
  masm.bind(&notString);
}

void CodeGenerator::visitLoadDynamicSlotAndAtomize(
    LLoadDynamicSlotAndAtomize* ins) {
  ValueOperand result = ToOutValue(ins);
  Register temp = ToRegister(ins->temp0());
  Register base = ToRegister(ins->input());
  int32_t offset = ins->mir()->slot() * sizeof(js::Value);

  Address slotAddr(base, offset);
  masm.loadValue(slotAddr, result);

  Label notString;
  masm.branchTestString(Assembler::NotEqual, result, &notString);
  masm.unboxString(result, temp);
  emitMaybeAtomizeSlot(ins, temp, slotAddr, result);
  masm.bind(&notString);
}

void CodeGenerator::visitLoadFixedSlotUnboxAndAtomize(
    LLoadFixedSlotUnboxAndAtomize* ins) {
  const MLoadFixedSlotAndUnbox* mir = ins->mir();
  MOZ_ASSERT(mir->type() == MIRType::String);
  Register input = ToRegister(ins->object());
  AnyRegister result = ToAnyRegister(ins->output());
  size_t slot = mir->slot();

  Address slotAddr(input, NativeObject::getFixedSlotOffset(slot));

  Label bail;
  EmitLoadAndUnbox(masm, slotAddr, MIRType::String, mir->fallible(), result,
                   &bail);
  emitMaybeAtomizeSlot(ins, result.gpr(), slotAddr,
                       TypedOrValueRegister(MIRType::String, result));

  if (mir->fallible()) {
    bailoutFrom(&bail, ins->snapshot());
  }
}

void CodeGenerator::visitLoadDynamicSlotUnboxAndAtomize(
    LLoadDynamicSlotUnboxAndAtomize* ins) {
  const MLoadDynamicSlotAndUnbox* mir = ins->mir();
  MOZ_ASSERT(mir->type() == MIRType::String);
  Register input = ToRegister(ins->slots());
  AnyRegister result = ToAnyRegister(ins->output());
  size_t slot = mir->slot();

  Address slotAddr(input, slot * sizeof(JS::Value));

  Label bail;
  EmitLoadAndUnbox(masm, slotAddr, MIRType::String, mir->fallible(), result,
                   &bail);
  emitMaybeAtomizeSlot(ins, result.gpr(), slotAddr,
                       TypedOrValueRegister(MIRType::String, result));

  if (mir->fallible()) {
    bailoutFrom(&bail, ins->snapshot());
  }
}

void CodeGenerator::visitAddAndStoreSlot(LAddAndStoreSlot* ins) {
  Register obj = ToRegister(ins->object());
  ValueOperand value = ToValue(ins->value());
  Register maybeTemp = ToTempRegisterOrInvalid(ins->temp0());

  Shape* shape = ins->mir()->shape();
  masm.storeObjShape(shape, obj, [](MacroAssembler& masm, const Address& addr) {
    EmitPreBarrier(masm, addr, MIRType::Shape);
  });

  // Perform the store. No pre-barrier required since this is a new
  // initialization.

  uint32_t offset = ins->mir()->slotOffset();
  if (ins->mir()->kind() == MAddAndStoreSlot::Kind::FixedSlot) {
    Address slot(obj, offset);
    masm.storeValue(value, slot);
  } else {
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), maybeTemp);
    Address slot(maybeTemp, offset);
    masm.storeValue(value, slot);
  }
}

void CodeGenerator::visitAllocateAndStoreSlot(LAllocateAndStoreSlot* ins) {
  Register obj = ToRegister(ins->object());
  ValueOperand value = ToValue(ins->value());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());

  masm.Push(obj);
  masm.Push(value);

  using Fn = bool (*)(JSContext* cx, NativeObject* obj, uint32_t newCount);
  masm.setupAlignedABICall();
  masm.loadJSContext(temp0);
  masm.passABIArg(temp0);
  masm.passABIArg(obj);
  masm.move32(Imm32(ins->mir()->numNewSlots()), temp1);
  masm.passABIArg(temp1);
  masm.callWithABI<Fn, NativeObject::growSlotsPure>();
  masm.storeCallPointerResult(temp0);

  masm.Pop(value);
  masm.Pop(obj);

  bailoutIfFalseBool(temp0, ins->snapshot());

  masm.storeObjShape(ins->mir()->shape(), obj,
                     [](MacroAssembler& masm, const Address& addr) {
                       EmitPreBarrier(masm, addr, MIRType::Shape);
                     });

  // Perform the store. No pre-barrier required since this is a new
  // initialization.
  masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), temp0);
  Address slot(temp0, ins->mir()->slotOffset());
  masm.storeValue(value, slot);
}

void CodeGenerator::visitAddSlotAndCallAddPropHook(
    LAddSlotAndCallAddPropHook* ins) {
  Register obj = ToRegister(ins->object());
  ValueOperand value = ToValue(ins->value());

  pushArg(ImmGCPtr(ins->mir()->shape()));
  pushArg(value);
  pushArg(obj);

  using Fn =
      bool (*)(JSContext*, Handle<NativeObject*>, HandleValue, Handle<Shape*>);
  callVM<Fn, AddSlotAndCallAddPropHook>(ins);
}

void CodeGenerator::visitStoreFixedSlotV(LStoreFixedSlotV* ins) {
  Register obj = ToRegister(ins->obj());
  size_t slot = ins->mir()->slot();

  ValueOperand value = ToValue(ins->value());

  Address address(obj, NativeObject::getFixedSlotOffset(slot));
  if (ins->mir()->needsBarrier()) {
    emitPreBarrier(address);
  }

  masm.storeValue(value, address);
}

void CodeGenerator::visitStoreFixedSlotT(LStoreFixedSlotT* ins) {
  const Register obj = ToRegister(ins->obj());
  size_t slot = ins->mir()->slot();

  const LAllocation* value = ins->value();
  MIRType valueType = ins->mir()->value()->type();

  Address address(obj, NativeObject::getFixedSlotOffset(slot));
  if (ins->mir()->needsBarrier()) {
    emitPreBarrier(address);
  }

  ConstantOrRegister nvalue =
      value->isConstant()
          ? ConstantOrRegister(value->toConstant()->toJSValue())
          : TypedOrValueRegister(valueType, ToAnyRegister(value));
  masm.storeConstantOrRegister(nvalue, address);
}

void CodeGenerator::visitGetNameCache(LGetNameCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  Register envChain = ToRegister(ins->envObj());
  ValueOperand output = ToOutValue(ins);
  Register temp = ToRegister(ins->temp0());

  IonGetNameIC ic(liveRegs, envChain, output, temp);
  addIC(ins, allocateIC(ic));
}

void CodeGenerator::addGetPropertyCache(LInstruction* ins,
                                        LiveRegisterSet liveRegs,
                                        TypedOrValueRegister value,
                                        const ConstantOrRegister& id,
                                        ValueOperand output) {
  CacheKind kind = CacheKind::GetElem;
  if (id.constant() && id.value().isString()) {
    JSString* idString = id.value().toString();
    if (idString->isAtom() && !idString->asAtom().isIndex()) {
      kind = CacheKind::GetProp;
    }
  }
  IonGetPropertyIC cache(kind, liveRegs, value, id, output);
  addIC(ins, allocateIC(cache));
}

void CodeGenerator::addSetPropertyCache(LInstruction* ins,
                                        LiveRegisterSet liveRegs,
                                        Register objReg, Register temp,
                                        const ConstantOrRegister& id,
                                        const ConstantOrRegister& value,
                                        bool strict) {
  CacheKind kind = CacheKind::SetElem;
  if (id.constant() && id.value().isString()) {
    JSString* idString = id.value().toString();
    if (idString->isAtom() && !idString->asAtom().isIndex()) {
      kind = CacheKind::SetProp;
    }
  }
  IonSetPropertyIC cache(kind, liveRegs, objReg, temp, id, value, strict);
  addIC(ins, allocateIC(cache));
}

ConstantOrRegister CodeGenerator::toConstantOrRegister(LInstruction* lir,
                                                       size_t n, MIRType type) {
  if (type == MIRType::Value) {
    return TypedOrValueRegister(ToValue(lir->getBoxOperand(n)));
  }

  const LAllocation* value = lir->getOperand(n);
  if (value->isConstant()) {
    return ConstantOrRegister(value->toConstant()->toJSValue());
  }

  return TypedOrValueRegister(type, ToAnyRegister(value));
}

void CodeGenerator::visitGetPropertyCache(LGetPropertyCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  TypedOrValueRegister value =
      toConstantOrRegister(ins, LGetPropertyCache::ValueIndex,
                           ins->mir()->value()->type())
          .reg();
  ConstantOrRegister id = toConstantOrRegister(ins, LGetPropertyCache::IdIndex,
                                               ins->mir()->idval()->type());
  ValueOperand output = ToOutValue(ins);
  addGetPropertyCache(ins, liveRegs, value, id, output);
}

void CodeGenerator::visitGetPropSuperCache(LGetPropSuperCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  Register obj = ToRegister(ins->obj());
  TypedOrValueRegister receiver =
      toConstantOrRegister(ins, LGetPropSuperCache::ReceiverIndex,
                           ins->mir()->receiver()->type())
          .reg();
  ConstantOrRegister id = toConstantOrRegister(ins, LGetPropSuperCache::IdIndex,
                                               ins->mir()->idval()->type());
  ValueOperand output = ToOutValue(ins);

  CacheKind kind = CacheKind::GetElemSuper;
  if (id.constant() && id.value().isString()) {
    JSString* idString = id.value().toString();
    if (idString->isAtom() && !idString->asAtom().isIndex()) {
      kind = CacheKind::GetPropSuper;
    }
  }

  IonGetPropSuperIC cache(kind, liveRegs, obj, receiver, id, output);
  addIC(ins, allocateIC(cache));
}

void CodeGenerator::visitBindNameCache(LBindNameCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  Register envChain = ToRegister(ins->environmentChain());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp0());

  IonBindNameIC ic(liveRegs, envChain, output, temp);
  addIC(ins, allocateIC(ic));
}

void CodeGenerator::visitHasOwnCache(LHasOwnCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  TypedOrValueRegister value =
      toConstantOrRegister(ins, LHasOwnCache::ValueIndex,
                           ins->mir()->value()->type())
          .reg();
  TypedOrValueRegister id = toConstantOrRegister(ins, LHasOwnCache::IdIndex,
                                                 ins->mir()->idval()->type())
                                .reg();
  Register output = ToRegister(ins->output());

  IonHasOwnIC cache(liveRegs, value, id, output);
  addIC(ins, allocateIC(cache));
}

void CodeGenerator::visitCheckPrivateFieldCache(LCheckPrivateFieldCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  TypedOrValueRegister value =
      toConstantOrRegister(ins, LCheckPrivateFieldCache::ValueIndex,
                           ins->mir()->value()->type())
          .reg();
  TypedOrValueRegister id =
      toConstantOrRegister(ins, LCheckPrivateFieldCache::IdIndex,
                           ins->mir()->idval()->type())
          .reg();
  Register output = ToRegister(ins->output());

  IonCheckPrivateFieldIC cache(liveRegs, value, id, output);
  addIC(ins, allocateIC(cache));
}

void CodeGenerator::visitNewPrivateName(LNewPrivateName* ins) {
  pushArg(ImmGCPtr(ins->mir()->name()));

  using Fn = JS::Symbol* (*)(JSContext*, Handle<JSAtom*>);
  callVM<Fn, NewPrivateName>(ins);
}

void CodeGenerator::visitCallDeleteProperty(LCallDeleteProperty* lir) {
  pushArg(ImmGCPtr(lir->mir()->name()));
  pushArg(ToValue(lir->value()));

  using Fn = bool (*)(JSContext*, HandleValue, Handle<PropertyName*>, bool*);
  if (lir->mir()->strict()) {
    callVM<Fn, DelPropOperation<true>>(lir);
  } else {
    callVM<Fn, DelPropOperation<false>>(lir);
  }
}

void CodeGenerator::visitCallDeleteElement(LCallDeleteElement* lir) {
  pushArg(ToValue(lir->index()));
  pushArg(ToValue(lir->value()));

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue, bool*);
  if (lir->mir()->strict()) {
    callVM<Fn, DelElemOperation<true>>(lir);
  } else {
    callVM<Fn, DelElemOperation<false>>(lir);
  }
}

void CodeGenerator::visitObjectToIterator(LObjectToIterator* lir) {
  Register obj = ToRegister(lir->object());
  Register iterObj = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp1());
  Register temp3 = ToRegister(lir->temp2());

  using Fn = PropertyIteratorObject* (*)(JSContext*, HandleObject);
  OutOfLineCode* ool = (lir->mir()->wantsIndices())
                           ? oolCallVM<Fn, GetIteratorWithIndices>(
                                 lir, ArgList(obj), StoreRegisterTo(iterObj))
                           : oolCallVM<Fn, GetIterator>(
                                 lir, ArgList(obj), StoreRegisterTo(iterObj));

  masm.maybeLoadIteratorFromShape(obj, iterObj, temp, temp2, temp3,
                                  ool->entry());

  Register nativeIter = temp;
  masm.loadPrivate(
      Address(iterObj, PropertyIteratorObject::offsetOfIteratorSlot()),
      nativeIter);

  if (lir->mir()->wantsIndices()) {
    // At least one consumer of the output of this iterator has been optimized
    // to use iterator indices. If the cached iterator doesn't include indices,
    // but it was marked to indicate that we can create them if needed, then we
    // do a VM call to replace the cached iterator with a fresh iterator
    // including indices.
    masm.branchNativeIteratorIndices(Assembler::Equal, nativeIter, temp2,
                                     NativeIteratorIndices::AvailableOnRequest,
                                     ool->entry());
  }

  Address iterFlagsAddr(nativeIter, NativeIterator::offsetOfFlagsAndCount());
  masm.storePtr(
      obj, Address(nativeIter, NativeIterator::offsetOfObjectBeingIterated()));
  masm.or32(Imm32(NativeIterator::Flags::Active), iterFlagsAddr);

  Register enumeratorsAddr = temp2;
  masm.movePtr(ImmPtr(lir->mir()->enumeratorsAddr()), enumeratorsAddr);
  masm.registerIterator(enumeratorsAddr, nativeIter, temp3);

  // Generate post-write barrier for storing to |iterObj->objectBeingIterated_|.
  // We already know that |iterObj| is tenured, so we only have to check |obj|.
  Label skipBarrier;
  masm.branchPtrInNurseryChunk(Assembler::NotEqual, obj, temp2, &skipBarrier);
  {
    LiveRegisterSet save = liveVolatileRegs(lir);
    save.takeUnchecked(temp);
    save.takeUnchecked(temp2);
    save.takeUnchecked(temp3);
    if (iterObj.volatile_()) {
      save.addUnchecked(iterObj);
    }

    masm.PushRegsInMask(save);
    emitPostWriteBarrier(iterObj);
    masm.PopRegsInMask(save);
  }
  masm.bind(&skipBarrier);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitValueToIterator(LValueToIterator* lir) {
  pushArg(ToValue(lir->value()));

  using Fn = PropertyIteratorObject* (*)(JSContext*, HandleValue);
  callVM<Fn, ValueToIterator>(lir);
}

void CodeGenerator::visitIteratorHasIndicesAndBranch(
    LIteratorHasIndicesAndBranch* lir) {
  Register iterator = ToRegister(lir->iterator());
  Register object = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp1());
  Label* ifTrue = getJumpLabelForBranch(lir->ifTrue());
  Label* ifFalse = getJumpLabelForBranch(lir->ifFalse());

  // Check that the iterator has indices available.
  Address nativeIterAddr(iterator,
                         PropertyIteratorObject::offsetOfIteratorSlot());
  masm.loadPrivate(nativeIterAddr, temp);
  masm.branchNativeIteratorIndices(Assembler::NotEqual, temp, temp2,
                                   NativeIteratorIndices::Valid, ifFalse);

  // Guard that the first shape stored in the iterator matches the current
  // shape of the iterated object.
  Address firstShapeAddr(temp, NativeIterator::offsetOfFirstShape());
  masm.loadPtr(firstShapeAddr, temp);
  masm.branchTestObjShape(Assembler::NotEqual, object, temp, temp2, object,
                          ifFalse);

  if (!isNextBlock(lir->ifTrue()->lir())) {
    masm.jump(ifTrue);
  }
}

void CodeGenerator::visitLoadSlotByIteratorIndex(
    LLoadSlotByIteratorIndex* lir) {
  Register object = ToRegister(lir->object());
  Register iterator = ToRegister(lir->iterator());
  Register temp = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp1());
  ValueOperand result = ToOutValue(lir);

  masm.extractCurrentIndexAndKindFromIterator(iterator, temp, temp2);

  Label notDynamicSlot, notFixedSlot, done;
  masm.branch32(Assembler::NotEqual, temp2,
                Imm32(uint32_t(PropertyIndex::Kind::DynamicSlot)),
                &notDynamicSlot);
  masm.loadPtr(Address(object, NativeObject::offsetOfSlots()), temp2);
  masm.loadValue(BaseValueIndex(temp2, temp), result);
  masm.jump(&done);

  masm.bind(&notDynamicSlot);
  masm.branch32(Assembler::NotEqual, temp2,
                Imm32(uint32_t(PropertyIndex::Kind::FixedSlot)), &notFixedSlot);
  // Fixed slot
  masm.loadValue(BaseValueIndex(object, temp, sizeof(NativeObject)), result);
  masm.jump(&done);
  masm.bind(&notFixedSlot);

#ifdef DEBUG
  Label kindOkay;
  masm.branch32(Assembler::Equal, temp2,
                Imm32(uint32_t(PropertyIndex::Kind::Element)), &kindOkay);
  masm.assumeUnreachable("Invalid PropertyIndex::Kind");
  masm.bind(&kindOkay);
#endif

  // Dense element
  masm.loadPtr(Address(object, NativeObject::offsetOfElements()), temp2);
  Label indexOkay;
  Address initLength(temp2, ObjectElements::offsetOfInitializedLength());
  masm.branch32(Assembler::Above, initLength, temp, &indexOkay);
  masm.assumeUnreachable("Dense element out of bounds");
  masm.bind(&indexOkay);

  masm.loadValue(BaseObjectElementIndex(temp2, temp), result);
  masm.bind(&done);
}

void CodeGenerator::visitStoreSlotByIteratorIndex(
    LStoreSlotByIteratorIndex* lir) {
  Register object = ToRegister(lir->object());
  Register iterator = ToRegister(lir->iterator());
  ValueOperand value = ToValue(lir->value());
  Register temp = ToRegister(lir->temp0());
  Register temp2 = ToRegister(lir->temp1());

  masm.extractCurrentIndexAndKindFromIterator(iterator, temp, temp2);

  Label notDynamicSlot, notFixedSlot, done, doStore;
  masm.branch32(Assembler::NotEqual, temp2,
                Imm32(uint32_t(PropertyIndex::Kind::DynamicSlot)),
                &notDynamicSlot);
  masm.loadPtr(Address(object, NativeObject::offsetOfSlots()), temp2);
  masm.computeEffectiveAddress(BaseValueIndex(temp2, temp), temp);
  masm.jump(&doStore);

  masm.bind(&notDynamicSlot);
  masm.branch32(Assembler::NotEqual, temp2,
                Imm32(uint32_t(PropertyIndex::Kind::FixedSlot)), &notFixedSlot);
  // Fixed slot
  masm.computeEffectiveAddress(
      BaseValueIndex(object, temp, sizeof(NativeObject)), temp);
  masm.jump(&doStore);
  masm.bind(&notFixedSlot);

#ifdef DEBUG
  Label kindOkay;
  masm.branch32(Assembler::Equal, temp2,
                Imm32(uint32_t(PropertyIndex::Kind::Element)), &kindOkay);
  masm.assumeUnreachable("Invalid PropertyIndex::Kind");
  masm.bind(&kindOkay);
#endif

  // Dense element
  masm.loadPtr(Address(object, NativeObject::offsetOfElements()), temp2);
  Label indexOkay;
  Address initLength(temp2, ObjectElements::offsetOfInitializedLength());
  masm.branch32(Assembler::Above, initLength, temp, &indexOkay);
  masm.assumeUnreachable("Dense element out of bounds");
  masm.bind(&indexOkay);

  BaseObjectElementIndex elementAddress(temp2, temp);
  masm.computeEffectiveAddress(elementAddress, temp);

  masm.bind(&doStore);
  Address storeAddress(temp, 0);
  emitPreBarrier(storeAddress);
  masm.storeValue(value, storeAddress);

  masm.branchPtrInNurseryChunk(Assembler::Equal, object, temp2, &done);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, value, temp2, &done);

  saveVolatile(temp2);
  emitPostWriteBarrier(object);
  restoreVolatile(temp2);

  masm.bind(&done);
}

void CodeGenerator::visitSetPropertyCache(LSetPropertyCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  Register objReg = ToRegister(ins->object());
  Register temp = ToRegister(ins->temp0());

  ConstantOrRegister id = toConstantOrRegister(ins, LSetPropertyCache::IdIndex,
                                               ins->mir()->idval()->type());
  ConstantOrRegister value = toConstantOrRegister(
      ins, LSetPropertyCache::ValueIndex, ins->mir()->value()->type());

  addSetPropertyCache(ins, liveRegs, objReg, temp, id, value,
                      ins->mir()->strict());
}

void CodeGenerator::visitThrow(LThrow* lir) {
  pushArg(ToValue(lir->value()));

  using Fn = bool (*)(JSContext*, HandleValue);
  callVM<Fn, js::ThrowOperation>(lir);
}

void CodeGenerator::visitThrowWithStack(LThrowWithStack* lir) {
  pushArg(ToValue(lir->stack()));
  pushArg(ToValue(lir->value()));

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue);
  callVM<Fn, js::ThrowWithStackOperation>(lir);
}

void CodeGenerator::emitTypeOfJSType(JSValueType type, Register output) {
  switch (type) {
    case JSVAL_TYPE_OBJECT:
      masm.move32(Imm32(JSTYPE_OBJECT), output);
      break;
    case JSVAL_TYPE_DOUBLE:
    case JSVAL_TYPE_INT32:
      masm.move32(Imm32(JSTYPE_NUMBER), output);
      break;
    case JSVAL_TYPE_BOOLEAN:
      masm.move32(Imm32(JSTYPE_BOOLEAN), output);
      break;
    case JSVAL_TYPE_UNDEFINED:
      masm.move32(Imm32(JSTYPE_UNDEFINED), output);
      break;
    case JSVAL_TYPE_NULL:
      masm.move32(Imm32(JSTYPE_OBJECT), output);
      break;
    case JSVAL_TYPE_STRING:
      masm.move32(Imm32(JSTYPE_STRING), output);
      break;
    case JSVAL_TYPE_SYMBOL:
      masm.move32(Imm32(JSTYPE_SYMBOL), output);
      break;
    case JSVAL_TYPE_BIGINT:
      masm.move32(Imm32(JSTYPE_BIGINT), output);
      break;
    default:
      MOZ_CRASH("Unsupported JSValueType");
  }
}

void CodeGenerator::emitTypeOfCheck(JSValueType type, Register tag,
                                    Register output, Label* done,
                                    Label* oolObject) {
  Label notMatch;
  switch (type) {
    case JSVAL_TYPE_OBJECT:
      // The input may be a callable object (result is "function") or
      // may emulate undefined (result is "undefined"). Use an OOL path.
      masm.branchTestObject(Assembler::Equal, tag, oolObject);
      return;
    case JSVAL_TYPE_DOUBLE:
    case JSVAL_TYPE_INT32:
      masm.branchTestNumber(Assembler::NotEqual, tag, &notMatch);
      break;
    default:
      masm.branchTestType(Assembler::NotEqual, tag, type, &notMatch);
      break;
  }

  emitTypeOfJSType(type, output);
  masm.jump(done);
  masm.bind(&notMatch);
}

void CodeGenerator::visitTypeOfV(LTypeOfV* lir) {
  ValueOperand value = ToValue(lir->input());
  Register output = ToRegister(lir->output());
  Register tag = masm.extractTag(value, output);

  Label done;

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    ValueOperand input = ToValue(lir->input());
    Register temp = ToTempUnboxRegister(lir->temp0());
    Register output = ToRegister(lir->output());

    Register obj = masm.extractObject(input, temp);
    emitTypeOfObject(obj, output, ool.rejoin());
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  const std::initializer_list<JSValueType> defaultOrder = {
      JSVAL_TYPE_OBJECT, JSVAL_TYPE_DOUBLE,  JSVAL_TYPE_UNDEFINED,
      JSVAL_TYPE_NULL,   JSVAL_TYPE_BOOLEAN, JSVAL_TYPE_STRING,
      JSVAL_TYPE_SYMBOL, JSVAL_TYPE_BIGINT};

  mozilla::EnumSet<JSValueType, uint32_t> remaining(defaultOrder);

  // Generate checks for previously observed types first.
  // The TypeDataList is sorted by descending frequency.
  for (auto& observed : lir->mir()->observedTypes()) {
    JSValueType type = observed.type();

    // Unify number types.
    if (type == JSVAL_TYPE_INT32) {
      type = JSVAL_TYPE_DOUBLE;
    }

    remaining -= type;

    emitTypeOfCheck(type, tag, output, &done, ool->entry());
  }

  // Generate checks for remaining types.
  for (auto type : defaultOrder) {
    if (!remaining.contains(type)) {
      continue;
    }
    remaining -= type;

    if (remaining.isEmpty() && type != JSVAL_TYPE_OBJECT) {
      // We can skip the check for the last remaining type, unless the type is
      // JSVAL_TYPE_OBJECT, which may have to go through the OOL path.
#ifdef DEBUG
      emitTypeOfCheck(type, tag, output, &done, ool->entry());
      masm.assumeUnreachable("Unexpected Value type in visitTypeOfV");
#else
      emitTypeOfJSType(type, output);
#endif
    } else {
      emitTypeOfCheck(type, tag, output, &done, ool->entry());
    }
  }
  MOZ_ASSERT(remaining.isEmpty());

  masm.bind(&done);
  masm.bind(ool->rejoin());
}

void CodeGenerator::emitTypeOfObject(Register obj, Register output,
                                     Label* done) {
  Label slowCheck, isObject, isCallable, isUndefined;
  masm.typeOfObject(obj, output, &slowCheck, &isObject, &isCallable,
                    &isUndefined);

  masm.bind(&isCallable);
  masm.move32(Imm32(JSTYPE_FUNCTION), output);
  masm.jump(done);

  masm.bind(&isUndefined);
  masm.move32(Imm32(JSTYPE_UNDEFINED), output);
  masm.jump(done);

  masm.bind(&isObject);
  masm.move32(Imm32(JSTYPE_OBJECT), output);
  masm.jump(done);

  masm.bind(&slowCheck);

  saveVolatile(output);
  using Fn = JSType (*)(JSObject*);
  masm.setupAlignedABICall();
  masm.passABIArg(obj);
  masm.callWithABI<Fn, js::TypeOfObject>();
  masm.storeCallInt32Result(output);
  restoreVolatile(output);
}

void CodeGenerator::visitTypeOfO(LTypeOfO* lir) {
  Register obj = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  Label done;
  emitTypeOfObject(obj, output, &done);
  masm.bind(&done);
}

void CodeGenerator::visitTypeOfName(LTypeOfName* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

#ifdef DEBUG
  Label ok;
  masm.branch32(Assembler::Below, input, Imm32(JSTYPE_LIMIT), &ok);
  masm.assumeUnreachable("bad JSType");
  masm.bind(&ok);
#endif

  static_assert(JSTYPE_UNDEFINED == 0);

  masm.movePtr(ImmPtr(&gen->runtime->names().undefined), output);
  masm.loadPtr(BaseIndex(output, input, ScalePointer), output);
}

void CodeGenerator::emitTypeOfIsObjectOOL(MTypeOfIs* mir, Register obj,
                                          Register output) {
  saveVolatile(output);
  using Fn = JSType (*)(JSObject*);
  masm.setupAlignedABICall();
  masm.passABIArg(obj);
  masm.callWithABI<Fn, js::TypeOfObject>();
  masm.storeCallInt32Result(output);
  restoreVolatile(output);

  auto cond = JSOpToCondition(mir->jsop(), /* isSigned = */ false);
  masm.cmp32Set(cond, output, Imm32(mir->jstype()), output);
}

void CodeGenerator::emitTypeOfIsObject(MTypeOfIs* mir, Register obj,
                                       Register output, Label* success,
                                       Label* fail, Label* slowCheck) {
  Label* isObject = fail;
  Label* isFunction = fail;
  Label* isUndefined = fail;

  switch (mir->jstype()) {
    case JSTYPE_UNDEFINED:
      isUndefined = success;
      break;

    case JSTYPE_OBJECT:
      isObject = success;
      break;

    case JSTYPE_FUNCTION:
      isFunction = success;
      break;

    case JSTYPE_STRING:
    case JSTYPE_NUMBER:
    case JSTYPE_BOOLEAN:
    case JSTYPE_SYMBOL:
    case JSTYPE_BIGINT:
    case JSTYPE_LIMIT:
      MOZ_CRASH("Primitive type");
  }

  masm.typeOfObject(obj, output, slowCheck, isObject, isFunction, isUndefined);

  auto op = mir->jsop();

  Label done;
  masm.bind(fail);
  masm.move32(Imm32(op == JSOp::Ne || op == JSOp::StrictNe), output);
  masm.jump(&done);
  masm.bind(success);
  masm.move32(Imm32(op == JSOp::Eq || op == JSOp::StrictEq), output);
  masm.bind(&done);
}

void CodeGenerator::visitTypeOfIsNonPrimitiveV(LTypeOfIsNonPrimitiveV* lir) {
  ValueOperand input = ToValue(lir->input());
  Register output = ToRegister(lir->output());
  Register temp = ToTempUnboxRegister(lir->temp0());

  auto* mir = lir->mir();

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    ValueOperand input = ToValue(lir->input());
    Register output = ToRegister(lir->output());
    Register temp = ToTempUnboxRegister(lir->temp0());

    Register obj = masm.extractObject(input, temp);

    emitTypeOfIsObjectOOL(lir->mir(), obj, output);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, mir);

  Label success, fail;

  switch (mir->jstype()) {
    case JSTYPE_UNDEFINED: {
      ScratchTagScope tag(masm, input);
      masm.splitTagForTest(input, tag);

      masm.branchTestUndefined(Assembler::Equal, tag, &success);
      masm.branchTestObject(Assembler::NotEqual, tag, &fail);
      break;
    }

    case JSTYPE_OBJECT: {
      ScratchTagScope tag(masm, input);
      masm.splitTagForTest(input, tag);

      masm.branchTestNull(Assembler::Equal, tag, &success);
      masm.branchTestObject(Assembler::NotEqual, tag, &fail);
      break;
    }

    case JSTYPE_FUNCTION: {
      masm.branchTestObject(Assembler::NotEqual, input, &fail);
      break;
    }

    case JSTYPE_STRING:
    case JSTYPE_NUMBER:
    case JSTYPE_BOOLEAN:
    case JSTYPE_SYMBOL:
    case JSTYPE_BIGINT:
    case JSTYPE_LIMIT:
      MOZ_CRASH("Primitive type");
  }

  Register obj = masm.extractObject(input, temp);

  emitTypeOfIsObject(mir, obj, output, &success, &fail, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitTypeOfIsNonPrimitiveO(LTypeOfIsNonPrimitiveO* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  auto* mir = lir->mir();

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    Register input = ToRegister(lir->input());
    Register output = ToRegister(lir->output());

    emitTypeOfIsObjectOOL(lir->mir(), input, output);

    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, mir);

  Label success, fail;
  emitTypeOfIsObject(mir, input, output, &success, &fail, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitTypeOfIsPrimitive(LTypeOfIsPrimitive* lir) {
  ValueOperand input = ToValue(lir->input());
  Register output = ToRegister(lir->output());

  auto* mir = lir->mir();
  auto cond = JSOpToCondition(mir->jsop(), /* isSigned = */ false);

  switch (mir->jstype()) {
    case JSTYPE_STRING:
      masm.testStringSet(cond, input, output);
      break;
    case JSTYPE_NUMBER:
      masm.testNumberSet(cond, input, output);
      break;
    case JSTYPE_BOOLEAN:
      masm.testBooleanSet(cond, input, output);
      break;
    case JSTYPE_SYMBOL:
      masm.testSymbolSet(cond, input, output);
      break;
    case JSTYPE_BIGINT:
      masm.testBigIntSet(cond, input, output);
      break;

    case JSTYPE_UNDEFINED:
    case JSTYPE_OBJECT:
    case JSTYPE_FUNCTION:
    case JSTYPE_LIMIT:
      MOZ_CRASH("Non-primitive type");
  }
}

void CodeGenerator::visitToAsyncIter(LToAsyncIter* lir) {
  pushArg(ToValue(lir->nextMethod()));
  pushArg(ToRegister(lir->iterator()));

  using Fn = JSObject* (*)(JSContext*, HandleObject, HandleValue);
  callVM<Fn, js::CreateAsyncFromSyncIterator>(lir);
}

void CodeGenerator::visitToPropertyKeyCache(LToPropertyKeyCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  ValueOperand input = ToValue(lir->input());
  ValueOperand output = ToOutValue(lir);

  IonToPropertyKeyIC ic(liveRegs, input, output);
  addIC(lir, allocateIC(ic));
}

void CodeGenerator::visitLoadElementV(LLoadElementV* load) {
  Register elements = ToRegister(load->elements());
  const ValueOperand out = ToOutValue(load);

  if (load->index()->isConstant()) {
    NativeObject::elementsSizeMustNotOverflow();
    int32_t offset = ToInt32(load->index()) * sizeof(Value);
    masm.loadValue(Address(elements, offset), out);
  } else {
    masm.loadValue(BaseObjectElementIndex(elements, ToRegister(load->index())),
                   out);
  }

  Label testMagic;
  masm.branchTestMagic(Assembler::Equal, out, &testMagic);
  bailoutFrom(&testMagic, load->snapshot());
}

void CodeGenerator::visitLoadElementHole(LLoadElementHole* lir) {
  Register elements = ToRegister(lir->elements());
  Register index = ToRegister(lir->index());
  Register initLength = ToRegister(lir->initLength());
  const ValueOperand out = ToOutValue(lir);

  const MLoadElementHole* mir = lir->mir();

  // If the index is out of bounds, load |undefined|. Otherwise, load the
  // value.
  Label outOfBounds, done;
  masm.spectreBoundsCheck32(index, initLength, out.scratchReg(), &outOfBounds);

  masm.loadValue(BaseObjectElementIndex(elements, index), out);

  // If the value wasn't a hole, we're done. Otherwise, we'll load undefined.
  masm.branchTestMagic(Assembler::NotEqual, out, &done);

  if (mir->needsNegativeIntCheck()) {
    Label loadUndefined;
    masm.jump(&loadUndefined);

    masm.bind(&outOfBounds);

    bailoutCmp32(Assembler::LessThan, index, Imm32(0), lir->snapshot());

    masm.bind(&loadUndefined);
  } else {
    masm.bind(&outOfBounds);
  }
  masm.moveValue(UndefinedValue(), out);

  masm.bind(&done);
}

void CodeGenerator::visitLoadUnboxedScalar(LLoadUnboxedScalar* lir) {
  Register elements = ToRegister(lir->elements());
  Register temp0 = ToTempRegisterOrInvalid(lir->temp0());
  Register temp1 = ToTempRegisterOrInvalid(lir->temp1());
  AnyRegister out = ToAnyRegister(lir->output());

  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

  LiveRegisterSet volatileRegs;
  if (MacroAssembler::LoadRequiresCall(storageType)) {
    volatileRegs = liveVolatileRegs(lir);
  }

  Label fail;
  if (lir->index()->isConstant()) {
    Address source =
        ToAddress(elements, lir->index(), storageType, mir->offsetAdjustment());
    masm.loadFromTypedArray(storageType, source, out, temp0, temp1, &fail,
                            volatileRegs);
  } else {
    BaseIndex source(elements, ToRegister(lir->index()),
                     ScaleFromScalarType(storageType), mir->offsetAdjustment());
    masm.loadFromTypedArray(storageType, source, out, temp0, temp1, &fail,
                            volatileRegs);
  }

  if (fail.used()) {
    bailoutFrom(&fail, lir->snapshot());
  }
}

void CodeGenerator::visitLoadUnboxedInt64(LLoadUnboxedInt64* lir) {
  Register elements = ToRegister(lir->elements());
  Register64 out = ToOutRegister64(lir);

  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

  if (lir->index()->isConstant()) {
    Address source =
        ToAddress(elements, lir->index(), storageType, mir->offsetAdjustment());
    masm.load64(source, out);
  } else {
    BaseIndex source(elements, ToRegister(lir->index()),
                     ScaleFromScalarType(storageType), mir->offsetAdjustment());
    masm.load64(source, out);
  }
}

void CodeGenerator::visitLoadDataViewElement(LLoadDataViewElement* lir) {
  Register elements = ToRegister(lir->elements());
  const LAllocation* littleEndian = lir->littleEndian();
  Register temp1 = ToTempRegisterOrInvalid(lir->temp0());
  Register temp2 = ToTempRegisterOrInvalid(lir->temp1());
  Register64 temp64 = ToTempRegister64OrInvalid(lir->temp2());
  AnyRegister out = ToAnyRegister(lir->output());

  const MLoadDataViewElement* mir = lir->mir();
  Scalar::Type storageType = mir->storageType();

  LiveRegisterSet volatileRegs;
  if (MacroAssembler::LoadRequiresCall(storageType)) {
    volatileRegs = liveVolatileRegs(lir);
  }

  BaseIndex source(elements, ToRegister(lir->index()), TimesOne);

  bool noSwap = littleEndian->isConstant() &&
                ToBoolean(littleEndian) == MOZ_LITTLE_ENDIAN();

  // Directly load if no byte swap is needed and the platform supports unaligned
  // accesses for the access.  (Such support is assumed for integer types.)
  if (noSwap && (!Scalar::isFloatingType(storageType) ||
                 MacroAssembler::SupportsFastUnalignedFPAccesses())) {
    Label fail;
    masm.loadFromTypedArray(storageType, source, out, temp1, temp2, &fail,
                            volatileRegs);

    if (fail.used()) {
      bailoutFrom(&fail, lir->snapshot());
    }
    return;
  }

  // Load the value into a gpr register.
  switch (storageType) {
    case Scalar::Int16:
      masm.load16UnalignedSignExtend(source, out.gpr());
      break;
    case Scalar::Uint16:
      masm.load16UnalignedZeroExtend(source, out.gpr());
      break;
    case Scalar::Int32:
      masm.load32Unaligned(source, out.gpr());
      break;
    case Scalar::Uint32:
      masm.load32Unaligned(source, out.isFloat() ? temp1 : out.gpr());
      break;
    case Scalar::Float16:
      masm.load16UnalignedZeroExtend(source, temp1);
      break;
    case Scalar::Float32:
      masm.load32Unaligned(source, temp1);
      break;
    case Scalar::Float64:
      masm.load64Unaligned(source, temp64);
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    default:
      MOZ_CRASH("Invalid typed array type");
  }

  if (!noSwap) {
    // Swap the bytes in the loaded value.
    Label skip;
    if (!littleEndian->isConstant()) {
      masm.branch32(
          MOZ_LITTLE_ENDIAN() ? Assembler::NotEqual : Assembler::Equal,
          ToRegister(littleEndian), Imm32(0), &skip);
    }

    switch (storageType) {
      case Scalar::Int16:
        masm.byteSwap16SignExtend(out.gpr());
        break;
      case Scalar::Uint16:
        masm.byteSwap16ZeroExtend(out.gpr());
        break;
      case Scalar::Int32:
        masm.byteSwap32(out.gpr());
        break;
      case Scalar::Uint32:
        masm.byteSwap32(out.isFloat() ? temp1 : out.gpr());
        break;
      case Scalar::Float16:
        masm.byteSwap16ZeroExtend(temp1);
        break;
      case Scalar::Float32:
        masm.byteSwap32(temp1);
        break;
      case Scalar::Float64:
        masm.byteSwap64(temp64);
        break;
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
      default:
        MOZ_CRASH("Invalid typed array type");
    }

    if (skip.used()) {
      masm.bind(&skip);
    }
  }

  // Move the value into the output register.
  switch (storageType) {
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
      break;
    case Scalar::Uint32:
      if (out.isFloat()) {
        masm.convertUInt32ToDouble(temp1, out.fpu());
      } else {
        // Bail out if the value doesn't fit into a signed int32 value. This
        // is what allows MLoadDataViewElement to have a type() of
        // MIRType::Int32 for UInt32 array loads.
        bailoutTest32(Assembler::Signed, out.gpr(), out.gpr(), lir->snapshot());
      }
      break;
    case Scalar::Float16:
      masm.moveGPRToFloat16(temp1, out.fpu(), temp2, volatileRegs);
      masm.canonicalizeFloat(out.fpu());
      break;
    case Scalar::Float32:
      masm.moveGPRToFloat32(temp1, out.fpu());
      masm.canonicalizeFloat(out.fpu());
      break;
    case Scalar::Float64:
      masm.moveGPR64ToDouble(temp64, out.fpu());
      masm.canonicalizeDouble(out.fpu());
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

void CodeGenerator::visitLoadDataViewElement64(LLoadDataViewElement64* lir) {
  Register elements = ToRegister(lir->elements());
  const LAllocation* littleEndian = lir->littleEndian();
  Register64 out = ToOutRegister64(lir);

  MOZ_ASSERT(Scalar::isBigIntType(lir->mir()->storageType()));

  BaseIndex source(elements, ToRegister(lir->index()), TimesOne);

  bool noSwap = littleEndian->isConstant() &&
                ToBoolean(littleEndian) == MOZ_LITTLE_ENDIAN();

  // Load the value into a register.
  masm.load64Unaligned(source, out);

  if (!noSwap) {
    // Swap the bytes in the loaded value.
    Label skip;
    if (!littleEndian->isConstant()) {
      masm.branch32(
          MOZ_LITTLE_ENDIAN() ? Assembler::NotEqual : Assembler::Equal,
          ToRegister(littleEndian), Imm32(0), &skip);
    }

    masm.byteSwap64(out);

    if (skip.used()) {
      masm.bind(&skip);
    }
  }
}

void CodeGenerator::visitLoadTypedArrayElementHole(
    LLoadTypedArrayElementHole* lir) {
  Register elements = ToRegister(lir->elements());
  Register index = ToRegister(lir->index());
  Register length = ToRegister(lir->length());
  Register temp = ToTempRegisterOrInvalid(lir->temp0());
  const ValueOperand out = ToOutValue(lir);

  Register scratch = out.scratchReg();

  // Load undefined if index >= length.
  Label outOfBounds, done;
  masm.spectreBoundsCheckPtr(index, length, scratch, &outOfBounds);

  Scalar::Type arrayType = lir->mir()->arrayType();

  LiveRegisterSet volatileRegs;
  if (MacroAssembler::LoadRequiresCall(arrayType)) {
    volatileRegs = liveVolatileRegs(lir);
  }

  Label fail;
  BaseIndex source(elements, index, ScaleFromScalarType(arrayType));
  MacroAssembler::Uint32Mode uint32Mode =
      lir->mir()->forceDouble() ? MacroAssembler::Uint32Mode::ForceDouble
                                : MacroAssembler::Uint32Mode::FailOnDouble;
  masm.loadFromTypedArray(arrayType, source, out, uint32Mode, temp, &fail,
                          volatileRegs);
  masm.jump(&done);

  masm.bind(&outOfBounds);
  masm.moveValue(UndefinedValue(), out);

  if (fail.used()) {
    bailoutFrom(&fail, lir->snapshot());
  }

  masm.bind(&done);
}

void CodeGenerator::visitLoadTypedArrayElementHoleBigInt(
    LLoadTypedArrayElementHoleBigInt* lir) {
  Register elements = ToRegister(lir->elements());
  Register index = ToRegister(lir->index());
  Register length = ToRegister(lir->length());
  const ValueOperand out = ToOutValue(lir);

  Register temp = ToRegister(lir->temp0());

  // On x86 there are not enough registers. In that case reuse the output
  // registers as temporaries.
#ifdef JS_CODEGEN_X86
  MOZ_ASSERT(lir->temp1().isBogusTemp());
  Register64 temp64 = out.toRegister64();
#else
  Register64 temp64 = ToRegister64(lir->temp1());
#endif

  // Load undefined if index >= length.
  Label outOfBounds, done;
  masm.spectreBoundsCheckPtr(index, length, temp, &outOfBounds);

  Scalar::Type arrayType = lir->mir()->arrayType();
  BaseIndex source(elements, index, ScaleFromScalarType(arrayType));
  masm.load64(source, temp64);

#ifdef JS_CODEGEN_X86
  Register bigInt = temp;
  Register maybeTemp = InvalidReg;
#else
  Register bigInt = out.scratchReg();
  Register maybeTemp = temp;
#endif
  emitCreateBigInt(lir, arrayType, temp64, bigInt, maybeTemp);

  masm.tagValue(JSVAL_TYPE_BIGINT, bigInt, out);
  masm.jump(&done);

  masm.bind(&outOfBounds);
  masm.moveValue(UndefinedValue(), out);

  masm.bind(&done);
}

template <typename T>
static inline void StoreToTypedArray(MacroAssembler& masm,
                                     Scalar::Type writeType,
                                     const LAllocation* value, const T& dest,
                                     Register temp,
                                     LiveRegisterSet volatileRegs) {
  if (Scalar::isFloatingType(writeType)) {
    masm.storeToTypedFloatArray(writeType, ToFloatRegister(value), dest, temp,
                                volatileRegs);
  } else {
    if (value->isConstant()) {
      masm.storeToTypedIntArray(writeType, Imm32(ToInt32(value)), dest);
    } else {
      masm.storeToTypedIntArray(writeType, ToRegister(value), dest);
    }
  }
}

void CodeGenerator::visitStoreUnboxedScalar(LStoreUnboxedScalar* lir) {
  Register elements = ToRegister(lir->elements());
  Register temp = ToTempRegisterOrInvalid(lir->temp0());
  const LAllocation* value = lir->value();

  const MStoreUnboxedScalar* mir = lir->mir();

  Scalar::Type writeType = mir->writeType();

  LiveRegisterSet volatileRegs;
  if (MacroAssembler::StoreRequiresCall(writeType)) {
    volatileRegs = liveVolatileRegs(lir);
  }

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), writeType);
    StoreToTypedArray(masm, writeType, value, dest, temp, volatileRegs);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(writeType));
    StoreToTypedArray(masm, writeType, value, dest, temp, volatileRegs);
  }
}

template <typename T>
static inline void StoreToTypedBigIntArray(MacroAssembler& masm,
                                           const LInt64Allocation& value,
                                           const T& dest) {
  if (IsConstant(value)) {
    masm.storeToTypedBigIntArray(Imm64(ToInt64(value)), dest);
  } else {
    masm.storeToTypedBigIntArray(ToRegister64(value), dest);
  }
}

void CodeGenerator::visitStoreUnboxedInt64(LStoreUnboxedInt64* lir) {
  Register elements = ToRegister(lir->elements());
  LInt64Allocation value = lir->value();

  Scalar::Type writeType = lir->mir()->writeType();
  MOZ_ASSERT(Scalar::isBigIntType(writeType));

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), writeType);
    StoreToTypedBigIntArray(masm, value, dest);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(writeType));
    StoreToTypedBigIntArray(masm, value, dest);
  }
}

void CodeGenerator::visitStoreDataViewElement(LStoreDataViewElement* lir) {
  Register elements = ToRegister(lir->elements());
  const LAllocation* value = lir->value();
  const LAllocation* littleEndian = lir->littleEndian();
  Register temp = ToTempRegisterOrInvalid(lir->temp0());
  Register64 temp64 = ToTempRegister64OrInvalid(lir->temp1());

  Scalar::Type writeType = lir->mir()->writeType();

  LiveRegisterSet volatileRegs;
  if (MacroAssembler::StoreRequiresCall(writeType)) {
    volatileRegs = liveVolatileRegs(lir);
  }

  BaseIndex dest(elements, ToRegister(lir->index()), TimesOne);

  bool noSwap = littleEndian->isConstant() &&
                ToBoolean(littleEndian) == MOZ_LITTLE_ENDIAN();

  // Directly store if no byte swap is needed and the platform supports
  // unaligned accesses for the access.  (Such support is assumed for integer
  // types.)
  if (noSwap && (!Scalar::isFloatingType(writeType) ||
                 MacroAssembler::SupportsFastUnalignedFPAccesses())) {
    StoreToTypedArray(masm, writeType, value, dest, temp, volatileRegs);
    return;
  }

  // Load the value into a gpr register.
  switch (writeType) {
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
      if (value->isConstant()) {
        masm.move32(Imm32(ToInt32(value)), temp);
      } else {
        masm.move32(ToRegister(value), temp);
      }
      break;
    case Scalar::Float16: {
      FloatRegister fvalue = ToFloatRegister(value);
      masm.moveFloat16ToGPR(fvalue, temp, volatileRegs);
      break;
    }
    case Scalar::Float32: {
      FloatRegister fvalue = ToFloatRegister(value);
      masm.moveFloat32ToGPR(fvalue, temp);
      break;
    }
    case Scalar::Float64: {
      FloatRegister fvalue = ToFloatRegister(value);
      masm.moveDoubleToGPR64(fvalue, temp64);
      break;
    }
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    default:
      MOZ_CRASH("Invalid typed array type");
  }

  if (!noSwap) {
    // Swap the bytes in the loaded value.
    Label skip;
    if (!littleEndian->isConstant()) {
      masm.branch32(
          MOZ_LITTLE_ENDIAN() ? Assembler::NotEqual : Assembler::Equal,
          ToRegister(littleEndian), Imm32(0), &skip);
    }

    switch (writeType) {
      case Scalar::Int16:
        masm.byteSwap16SignExtend(temp);
        break;
      case Scalar::Uint16:
      case Scalar::Float16:
        masm.byteSwap16ZeroExtend(temp);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
      case Scalar::Float32:
        masm.byteSwap32(temp);
        break;
      case Scalar::Float64:
        masm.byteSwap64(temp64);
        break;
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
      default:
        MOZ_CRASH("Invalid typed array type");
    }

    if (skip.used()) {
      masm.bind(&skip);
    }
  }

  // Store the value into the destination.
  switch (writeType) {
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Float16:
      masm.store16Unaligned(temp, dest);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
      masm.store32Unaligned(temp, dest);
      break;
    case Scalar::Float64:
      masm.store64Unaligned(temp64, dest);
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

void CodeGenerator::visitStoreDataViewElement64(LStoreDataViewElement64* lir) {
  Register elements = ToRegister(lir->elements());
  LInt64Allocation value = lir->value();
  const LAllocation* littleEndian = lir->littleEndian();
  Register64 temp = ToTempRegister64OrInvalid(lir->temp0());

  MOZ_ASSERT(Scalar::isBigIntType(lir->mir()->writeType()));

  BaseIndex dest(elements, ToRegister(lir->index()), TimesOne);

  bool noSwap = littleEndian->isConstant() &&
                ToBoolean(littleEndian) == MOZ_LITTLE_ENDIAN();

  // Directly store if no byte swap is needed and the platform supports
  // unaligned accesses for the access.  (Such support is assumed for integer
  // types.)
  if (noSwap) {
    StoreToTypedBigIntArray(masm, value, dest);
    return;
  }

  Register64 valueReg = Register64::Invalid();
  if (IsConstant(value)) {
    MOZ_ASSERT(temp != Register64::Invalid());
    masm.move64(Imm64(ToInt64(value)), temp);
  } else {
    valueReg = ToRegister64(value);

    // Preserve the input value.
    if (temp != Register64::Invalid()) {
      masm.move64(valueReg, temp);
    } else {
      masm.Push(valueReg);
      temp = valueReg;
    }
  }

  // Swap the bytes in the loaded value.
  Label skip;
  if (!littleEndian->isConstant()) {
    masm.branch32(MOZ_LITTLE_ENDIAN() ? Assembler::NotEqual : Assembler::Equal,
                  ToRegister(littleEndian), Imm32(0), &skip);
  }

  masm.byteSwap64(temp);

  if (skip.used()) {
    masm.bind(&skip);
  }

  // Store the value into the destination.
  masm.store64Unaligned(temp, dest);

  // Restore |value| if it was modified.
  if (valueReg == temp) {
    masm.Pop(valueReg);
  }
}

void CodeGenerator::visitStoreTypedArrayElementHole(
    LStoreTypedArrayElementHole* lir) {
  Register elements = ToRegister(lir->elements());
  const LAllocation* value = lir->value();

  Scalar::Type arrayType = lir->mir()->arrayType();

  Register index = ToRegister(lir->index());
  const LAllocation* length = lir->length();
  Register temp = ToTempRegisterOrInvalid(lir->temp0());

  LiveRegisterSet volatileRegs;
  if (MacroAssembler::StoreRequiresCall(arrayType)) {
    volatileRegs = liveVolatileRegs(lir);
  }

  Label skip;
  if (length->isGeneralReg()) {
    masm.spectreBoundsCheckPtr(index, ToRegister(length), temp, &skip);
  } else {
    masm.spectreBoundsCheckPtr(index, ToAddress(length), temp, &skip);
  }

  BaseIndex dest(elements, index, ScaleFromScalarType(arrayType));
  StoreToTypedArray(masm, arrayType, value, dest, temp, volatileRegs);

  masm.bind(&skip);
}

void CodeGenerator::visitStoreTypedArrayElementHoleInt64(
    LStoreTypedArrayElementHoleInt64* lir) {
  Register elements = ToRegister(lir->elements());
  LInt64Allocation value = lir->value();

  Scalar::Type arrayType = lir->mir()->arrayType();
  MOZ_ASSERT(Scalar::isBigIntType(arrayType));

  Register index = ToRegister(lir->index());
  const LAllocation* length = lir->length();
  Register spectreTemp = ToTempRegisterOrInvalid(lir->temp0());

  Label skip;
  if (length->isGeneralReg()) {
    masm.spectreBoundsCheckPtr(index, ToRegister(length), spectreTemp, &skip);
  } else {
    masm.spectreBoundsCheckPtr(index, ToAddress(length), spectreTemp, &skip);
  }

  BaseIndex dest(elements, index, ScaleFromScalarType(arrayType));
  StoreToTypedBigIntArray(masm, value, dest);

  masm.bind(&skip);
}

void CodeGenerator::visitMemoryBarrier(LMemoryBarrier* ins) {
  masm.memoryBarrier(ins->barrier());
}

void CodeGenerator::visitAtomicIsLockFree(LAtomicIsLockFree* lir) {
  Register value = ToRegister(lir->value());
  Register output = ToRegister(lir->output());

  masm.atomicIsLockFreeJS(value, output);
}

void CodeGenerator::visitAtomicPause(LAtomicPause* lir) { masm.atomicPause(); }

void CodeGenerator::visitClampIToUint8(LClampIToUint8* lir) {
  Register output = ToRegister(lir->output());
  MOZ_ASSERT(output == ToRegister(lir->input()));
  masm.clampIntToUint8(output);
}

void CodeGenerator::visitClampDToUint8(LClampDToUint8* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());
  masm.clampDoubleToUint8(input, output);
}

void CodeGenerator::visitClampVToUint8(LClampVToUint8* lir) {
  ValueOperand operand = ToValue(lir->input());
  FloatRegister tempFloat = ToFloatRegister(lir->temp0());
  Register output = ToRegister(lir->output());

  using Fn = bool (*)(JSContext*, JSString*, double*);
  OutOfLineCode* oolString = oolCallVM<Fn, StringToNumber>(
      lir, ArgList(output), StoreFloatRegisterTo(tempFloat));
  Label* stringEntry = oolString->entry();
  Label* stringRejoin = oolString->rejoin();

  Label fails;
  masm.clampValueToUint8(operand, stringEntry, stringRejoin, output, tempFloat,
                         output, &fails);

  bailoutFrom(&fails, lir->snapshot());
}

void CodeGenerator::visitInCache(LInCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();

  ConstantOrRegister key =
      toConstantOrRegister(ins, LInCache::LhsIndex, ins->mir()->key()->type());
  Register object = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp0());

  IonInIC cache(liveRegs, key, object, output, temp);
  addIC(ins, allocateIC(cache));
}

void CodeGenerator::visitInArray(LInArray* lir) {
  const MInArray* mir = lir->mir();
  Register elements = ToRegister(lir->elements());
  Register initLength = ToRegister(lir->initLength());
  Register output = ToRegister(lir->output());

  Label falseBranch, done, trueBranch;

  if (lir->index()->isConstant()) {
    int32_t index = ToInt32(lir->index());

    if (index < 0) {
      MOZ_ASSERT(mir->needsNegativeIntCheck());
      bailout(lir->snapshot());
      return;
    }

    masm.branch32(Assembler::BelowOrEqual, initLength, Imm32(index),
                  &falseBranch);

    NativeObject::elementsSizeMustNotOverflow();
    Address address = Address(elements, index * sizeof(Value));
    masm.branchTestMagic(Assembler::Equal, address, &falseBranch);
  } else {
    Register index = ToRegister(lir->index());

    Label negativeIntCheck;
    Label* failedInitLength = &falseBranch;
    if (mir->needsNegativeIntCheck()) {
      failedInitLength = &negativeIntCheck;
    }

    masm.branch32(Assembler::BelowOrEqual, initLength, index, failedInitLength);

    BaseObjectElementIndex address(elements, index);
    masm.branchTestMagic(Assembler::Equal, address, &falseBranch);

    if (mir->needsNegativeIntCheck()) {
      masm.jump(&trueBranch);
      masm.bind(&negativeIntCheck);

      bailoutCmp32(Assembler::LessThan, index, Imm32(0), lir->snapshot());

      masm.jump(&falseBranch);
    }
  }

  masm.bind(&trueBranch);
  masm.move32(Imm32(1), output);
  masm.jump(&done);

  masm.bind(&falseBranch);
  masm.move32(Imm32(0), output);
  masm.bind(&done);
}

void CodeGenerator::visitGuardElementNotHole(LGuardElementNotHole* lir) {
  Register elements = ToRegister(lir->elements());
  const LAllocation* index = lir->index();

  Label testMagic;
  if (index->isConstant()) {
    Address address(elements, ToInt32(index) * sizeof(js::Value));
    masm.branchTestMagic(Assembler::Equal, address, &testMagic);
  } else {
    BaseObjectElementIndex address(elements, ToRegister(index));
    masm.branchTestMagic(Assembler::Equal, address, &testMagic);
  }
  bailoutFrom(&testMagic, lir->snapshot());
}

void CodeGenerator::visitInstanceOfO(LInstanceOfO* ins) {
  Register protoReg = ToRegister(ins->rhs());
  emitInstanceOf(ins, protoReg);
}

void CodeGenerator::visitInstanceOfV(LInstanceOfV* ins) {
  Register protoReg = ToRegister(ins->rhs());
  emitInstanceOf(ins, protoReg);
}

void CodeGenerator::emitInstanceOf(LInstruction* ins, Register protoReg) {
  // This path implements fun_hasInstance when the function's prototype is
  // known to be the object in protoReg

  Label done;
  Register output = ToRegister(ins->getDef(0));

  // If the lhs is a primitive, the result is false.
  Register objReg;
  if (ins->isInstanceOfV()) {
    Label isObject;
    ValueOperand lhsValue = ToValue(ins->toInstanceOfV()->lhs());
    masm.branchTestObject(Assembler::Equal, lhsValue, &isObject);
    masm.mov(ImmWord(0), output);
    masm.jump(&done);
    masm.bind(&isObject);
    objReg = masm.extractObject(lhsValue, output);
  } else {
    objReg = ToRegister(ins->toInstanceOfO()->lhs());
  }

  // Crawl the lhs's prototype chain in a loop to search for prototypeObject.
  // This follows the main loop of js::IsPrototypeOf, though additionally breaks
  // out of the loop on Proxy::LazyProto.

  // Load the lhs's prototype.
  masm.loadObjProto(objReg, output);

  Label testLazy;
  {
    Label loopPrototypeChain;
    masm.bind(&loopPrototypeChain);

    // Test for the target prototype object.
    Label notPrototypeObject;
    masm.branchPtr(Assembler::NotEqual, output, protoReg, &notPrototypeObject);
    masm.mov(ImmWord(1), output);
    masm.jump(&done);
    masm.bind(&notPrototypeObject);

    MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

    // Test for nullptr or Proxy::LazyProto
    masm.branchPtr(Assembler::BelowOrEqual, output, ImmWord(1), &testLazy);

    // Load the current object's prototype.
    masm.loadObjProto(output, output);

    masm.jump(&loopPrototypeChain);
  }

  // Make a VM call if an object with a lazy proto was found on the prototype
  // chain. This currently occurs only for cross compartment wrappers, which
  // we do not expect to be compared with non-wrapper functions from this
  // compartment. Otherwise, we stopped on a nullptr prototype and the output
  // register is already correct.

  using Fn = bool (*)(JSContext*, HandleObject, JSObject*, bool*);
  auto* ool = oolCallVM<Fn, IsPrototypeOf>(ins, ArgList(protoReg, objReg),
                                           StoreRegisterTo(output));

  // Regenerate the original lhs object for the VM call.
  Label regenerate, *lazyEntry;
  if (objReg != output) {
    lazyEntry = ool->entry();
  } else {
    masm.bind(&regenerate);
    lazyEntry = &regenerate;
    if (ins->isInstanceOfV()) {
      ValueOperand lhsValue = ToValue(ins->toInstanceOfV()->lhs());
      objReg = masm.extractObject(lhsValue, output);
    } else {
      objReg = ToRegister(ins->toInstanceOfO()->lhs());
    }
    MOZ_ASSERT(objReg == output);
    masm.jump(ool->entry());
  }

  masm.bind(&testLazy);
  masm.branchPtr(Assembler::Equal, output, ImmWord(1), lazyEntry);

  masm.bind(&done);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitInstanceOfCache(LInstanceOfCache* ins) {
  // The Lowering ensures that RHS is an object, and that LHS is a value.
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  TypedOrValueRegister lhs = TypedOrValueRegister(ToValue(ins->obj()));
  Register rhs = ToRegister(ins->proto());
  Register output = ToRegister(ins->output());

  IonInstanceOfIC ic(liveRegs, lhs, rhs, output);
  addIC(ins, allocateIC(ic));
}

void CodeGenerator::visitGetDOMProperty(LGetDOMProperty* ins) {
  const Register JSContextReg = ToRegister(ins->temp0());
  const Register ObjectReg = ToRegister(ins->object());
  const Register PrivateReg = ToRegister(ins->temp1());
  const Register ValueReg = ToRegister(ins->temp2());

  Label haveValue;
  if (ins->mir()->valueMayBeInSlot()) {
    size_t slot = ins->mir()->domMemberSlotIndex();
    // It's a bit annoying to redo these slot calculations, which duplcate
    // LSlots and a few other things like that, but I'm not sure there's a
    // way to reuse those here.
    //
    // If this ever gets fixed to work with proxies (by not assuming that
    // reserved slot indices, which is what domMemberSlotIndex() returns,
    // match fixed slot indices), we can reenable MGetDOMProperty for
    // proxies in IonBuilder.
    if (slot < NativeObject::MAX_FIXED_SLOTS) {
      masm.loadValue(Address(ObjectReg, NativeObject::getFixedSlotOffset(slot)),
                     JSReturnOperand);
    } else {
      // It's a dynamic slot.
      slot -= NativeObject::MAX_FIXED_SLOTS;
      // Use PrivateReg as a scratch register for the slots pointer.
      masm.loadPtr(Address(ObjectReg, NativeObject::offsetOfSlots()),
                   PrivateReg);
      masm.loadValue(Address(PrivateReg, slot * sizeof(js::Value)),
                     JSReturnOperand);
    }
    masm.branchTestUndefined(Assembler::NotEqual, JSReturnOperand, &haveValue);
  }

  DebugOnly<uint32_t> initialStack = masm.framePushed();

  masm.checkStackAlignment();

  // Make space for the outparam.  Pre-initialize it to UndefinedValue so we
  // can trace it at GC time.
  masm.Push(UndefinedValue());
  // We pass the pointer to our out param as an instance of
  // JSJitGetterCallArgs, since on the binary level it's the same thing.
  static_assert(sizeof(JSJitGetterCallArgs) == sizeof(Value*));
  masm.moveStackPtrTo(ValueReg);

  masm.Push(ObjectReg);

  LoadDOMPrivate(masm, ObjectReg, PrivateReg, ins->mir()->objectKind());

  // Rooting will happen at GC time.
  masm.moveStackPtrTo(ObjectReg);

  Realm* getterRealm = ins->mir()->getterRealm();
  if (gen->realm->realmPtr() != getterRealm) {
    // We use JSContextReg as scratch register here.
    masm.switchToRealm(getterRealm, JSContextReg);
  }

  uint32_t safepointOffset = masm.buildFakeExitFrame(JSContextReg);
  masm.loadJSContext(JSContextReg);
  masm.enterFakeExitFrame(JSContextReg, JSContextReg,
                          ExitFrameType::IonDOMGetter);

  markSafepointAt(safepointOffset, ins);

  masm.setupAlignedABICall();
  masm.loadJSContext(JSContextReg);
  masm.passABIArg(JSContextReg);
  masm.passABIArg(ObjectReg);
  masm.passABIArg(PrivateReg);
  masm.passABIArg(ValueReg);
  ensureOsiSpace();
  masm.callWithABI(DynamicFunction<JSJitGetterOp>(ins->mir()->fun()),
                   ABIType::General,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  if (ins->mir()->isInfallible()) {
    masm.loadValue(Address(masm.getStackPointer(),
                           IonDOMExitFrameLayout::offsetOfResult()),
                   JSReturnOperand);
  } else {
    masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

    masm.loadValue(Address(masm.getStackPointer(),
                           IonDOMExitFrameLayout::offsetOfResult()),
                   JSReturnOperand);
  }

  // Switch back to the current realm if needed. Note: if the getter threw an
  // exception, the exception handler will do this.
  if (gen->realm->realmPtr() != getterRealm) {
    static_assert(!JSReturnOperand.aliases(ReturnReg),
                  "Clobbering ReturnReg should not affect the return value");
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
  }

  // Until C++ code is instrumented against Spectre, prevent speculative
  // execution from returning any private data.
  if (JitOptions.spectreJitToCxxCalls && ins->mir()->hasLiveDefUses()) {
    masm.speculationBarrier();
  }

  masm.adjustStack(IonDOMExitFrameLayout::Size());

  masm.bind(&haveValue);

  MOZ_ASSERT(masm.framePushed() == initialStack);
}

void CodeGenerator::visitGetDOMMemberV(LGetDOMMemberV* ins) {
  // It's simpler to duplicate visitLoadFixedSlotV here than it is to try to
  // use an LLoadFixedSlotV or some subclass of it for this case: that would
  // require us to have MGetDOMMember inherit from MLoadFixedSlot, and then
  // we'd have to duplicate a bunch of stuff we now get for free from
  // MGetDOMProperty.
  //
  // If this ever gets fixed to work with proxies (by not assuming that
  // reserved slot indices, which is what domMemberSlotIndex() returns,
  // match fixed slot indices), we can reenable MGetDOMMember for
  // proxies in IonBuilder.
  Register object = ToRegister(ins->object());
  size_t slot = ins->mir()->domMemberSlotIndex();
  ValueOperand result = ToOutValue(ins);

  masm.loadValue(Address(object, NativeObject::getFixedSlotOffset(slot)),
                 result);
}

void CodeGenerator::visitGetDOMMemberT(LGetDOMMemberT* ins) {
  // It's simpler to duplicate visitLoadFixedSlotT here than it is to try to
  // use an LLoadFixedSlotT or some subclass of it for this case: that would
  // require us to have MGetDOMMember inherit from MLoadFixedSlot, and then
  // we'd have to duplicate a bunch of stuff we now get for free from
  // MGetDOMProperty.
  //
  // If this ever gets fixed to work with proxies (by not assuming that
  // reserved slot indices, which is what domMemberSlotIndex() returns,
  // match fixed slot indices), we can reenable MGetDOMMember for
  // proxies in IonBuilder.
  Register object = ToRegister(ins->object());
  size_t slot = ins->mir()->domMemberSlotIndex();
  AnyRegister result = ToAnyRegister(ins->output());
  MIRType type = ins->mir()->type();

  masm.loadUnboxedValue(Address(object, NativeObject::getFixedSlotOffset(slot)),
                        type, result);
}

void CodeGenerator::visitSetDOMProperty(LSetDOMProperty* ins) {
  const Register JSContextReg = ToRegister(ins->temp0());
  const Register ObjectReg = ToRegister(ins->object());
  const Register PrivateReg = ToRegister(ins->temp1());
  const Register ValueReg = ToRegister(ins->temp2());

  DebugOnly<uint32_t> initialStack = masm.framePushed();

  masm.checkStackAlignment();

  // Push the argument. Rooting will happen at GC time.
  ValueOperand argVal = ToValue(ins->value());
  masm.Push(argVal);
  // We pass the pointer to our out param as an instance of
  // JSJitGetterCallArgs, since on the binary level it's the same thing.
  static_assert(sizeof(JSJitSetterCallArgs) == sizeof(Value*));
  masm.moveStackPtrTo(ValueReg);

  masm.Push(ObjectReg);

  LoadDOMPrivate(masm, ObjectReg, PrivateReg, ins->mir()->objectKind());

  // Rooting will happen at GC time.
  masm.moveStackPtrTo(ObjectReg);

  Realm* setterRealm = ins->mir()->setterRealm();
  if (gen->realm->realmPtr() != setterRealm) {
    // We use JSContextReg as scratch register here.
    masm.switchToRealm(setterRealm, JSContextReg);
  }

  uint32_t safepointOffset = masm.buildFakeExitFrame(JSContextReg);
  masm.loadJSContext(JSContextReg);
  masm.enterFakeExitFrame(JSContextReg, JSContextReg,
                          ExitFrameType::IonDOMSetter);

  markSafepointAt(safepointOffset, ins);

  masm.setupAlignedABICall();
  masm.loadJSContext(JSContextReg);
  masm.passABIArg(JSContextReg);
  masm.passABIArg(ObjectReg);
  masm.passABIArg(PrivateReg);
  masm.passABIArg(ValueReg);
  ensureOsiSpace();
  masm.callWithABI(DynamicFunction<JSJitSetterOp>(ins->mir()->fun()),
                   ABIType::General,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

  // Switch back to the current realm if needed. Note: if the setter threw an
  // exception, the exception handler will do this.
  if (gen->realm->realmPtr() != setterRealm) {
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
  }

  masm.adjustStack(IonDOMExitFrameLayout::Size());

  MOZ_ASSERT(masm.framePushed() == initialStack);
}

void CodeGenerator::visitLoadDOMExpandoValue(LLoadDOMExpandoValue* ins) {
  Register proxy = ToRegister(ins->proxy());
  ValueOperand out = ToOutValue(ins);

  masm.loadPtr(Address(proxy, ProxyObject::offsetOfReservedSlots()),
               out.scratchReg());
  masm.loadValue(Address(out.scratchReg(),
                         js::detail::ProxyReservedSlots::offsetOfPrivateSlot()),
                 out);
}

void CodeGenerator::visitLoadDOMExpandoValueGuardGeneration(
    LLoadDOMExpandoValueGuardGeneration* ins) {
  Register proxy = ToRegister(ins->proxy());
  ValueOperand out = ToOutValue(ins);

  Label bail;
  masm.loadDOMExpandoValueGuardGeneration(proxy, out,
                                          ins->mir()->expandoAndGeneration(),
                                          ins->mir()->generation(), &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitLoadDOMExpandoValueIgnoreGeneration(
    LLoadDOMExpandoValueIgnoreGeneration* ins) {
  Register proxy = ToRegister(ins->proxy());
  ValueOperand out = ToOutValue(ins);

  masm.loadPtr(Address(proxy, ProxyObject::offsetOfReservedSlots()),
               out.scratchReg());

  // Load the ExpandoAndGeneration* from the PrivateValue.
  masm.loadPrivate(
      Address(out.scratchReg(),
              js::detail::ProxyReservedSlots::offsetOfPrivateSlot()),
      out.scratchReg());

  // Load expandoAndGeneration->expando into the output Value register.
  masm.loadValue(
      Address(out.scratchReg(), ExpandoAndGeneration::offsetOfExpando()), out);
}

void CodeGenerator::visitGuardDOMExpandoMissingOrGuardShape(
    LGuardDOMExpandoMissingOrGuardShape* ins) {
  Register temp = ToRegister(ins->temp0());
  ValueOperand input = ToValue(ins->input());

  Label done;
  masm.branchTestUndefined(Assembler::Equal, input, &done);

  masm.debugAssertIsObject(input);
  masm.unboxObject(input, temp);
  // The expando object is not used in this case, so we don't need Spectre
  // mitigations.
  Label bail;
  masm.branchTestObjShapeNoSpectreMitigations(Assembler::NotEqual, temp,
                                              ins->mir()->shape(), &bail);
  bailoutFrom(&bail, ins->snapshot());

  masm.bind(&done);
}

void CodeGenerator::emitIsCallableOOL(Register object, Register output) {
  saveVolatile(output);
  using Fn = bool (*)(JSObject* obj);
  masm.setupAlignedABICall();
  masm.passABIArg(object);
  masm.callWithABI<Fn, ObjectIsCallable>();
  masm.storeCallBoolResult(output);
  restoreVolatile(output);
}

void CodeGenerator::visitIsCallableO(LIsCallableO* ins) {
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    emitIsCallableOOL(object, output);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, ins->mir());

  masm.isCallable(object, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitIsCallableV(LIsCallableV* ins) {
  ValueOperand val = ToValue(ins->object());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp0());

  Label notObject;
  masm.fallibleUnboxObject(val, temp, &notObject);

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    emitIsCallableOOL(temp, output);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, ins->mir());

  masm.isCallable(temp, output, ool->entry());
  masm.jump(ool->rejoin());

  masm.bind(&notObject);
  masm.move32(Imm32(0), output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitIsConstructor(LIsConstructor* ins) {
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    saveVolatile(output);
    using Fn = bool (*)(JSObject* obj);
    masm.setupAlignedABICall();
    masm.passABIArg(object);
    masm.callWithABI<Fn, ObjectIsConstructor>();
    masm.storeCallBoolResult(output);
    restoreVolatile(output);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, ins->mir());

  masm.isConstructor(object, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitIsCrossRealmArrayConstructor(
    LIsCrossRealmArrayConstructor* ins) {
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  masm.setIsCrossRealmArrayConstructor(object, output);
}

static void EmitObjectIsArray(MacroAssembler& masm, OutOfLineCode* ool,
                              Register obj, Register output,
                              Label* notArray = nullptr) {
  masm.loadObjClassUnsafe(obj, output);

  Label isArray;
  masm.branchPtr(Assembler::Equal, output, ImmPtr(&ArrayObject::class_),
                 &isArray);

  // Branch to OOL path if it's a proxy.
  masm.branchTestClassIsProxy(true, output, ool->entry());

  if (notArray) {
    masm.bind(notArray);
  }
  masm.move32(Imm32(0), output);
  masm.jump(ool->rejoin());

  masm.bind(&isArray);
  masm.move32(Imm32(1), output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitIsArrayO(LIsArrayO* lir) {
  Register object = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  using Fn = bool (*)(JSContext*, HandleObject, bool*);
  OutOfLineCode* ool = oolCallVM<Fn, js::IsArrayFromJit>(
      lir, ArgList(object), StoreRegisterTo(output));
  EmitObjectIsArray(masm, ool, object, output);
}

void CodeGenerator::visitIsArrayV(LIsArrayV* lir) {
  ValueOperand val = ToValue(lir->value());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  Label notArray;
  masm.fallibleUnboxObject(val, temp, &notArray);

  using Fn = bool (*)(JSContext*, HandleObject, bool*);
  OutOfLineCode* ool = oolCallVM<Fn, js::IsArrayFromJit>(
      lir, ArgList(temp), StoreRegisterTo(output));
  EmitObjectIsArray(masm, ool, temp, output, &notArray);
}

void CodeGenerator::visitIsTypedArray(LIsTypedArray* lir) {
  Register object = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  OutOfLineCode* ool = nullptr;
  if (lir->mir()->isPossiblyWrapped()) {
    using Fn = bool (*)(JSContext*, JSObject*, bool*);
    ool = oolCallVM<Fn, jit::IsPossiblyWrappedTypedArray>(
        lir, ArgList(object), StoreRegisterTo(output));
  }

  Label notTypedArray;
  Label done;

  masm.loadObjClassUnsafe(object, output);
  masm.branchIfClassIsNotTypedArray(output, &notTypedArray);

  masm.move32(Imm32(1), output);
  masm.jump(&done);
  masm.bind(&notTypedArray);
  if (ool) {
    Label notProxy;
    masm.branchTestClassIsProxy(false, output, &notProxy);
    masm.branchTestProxyHandlerFamily(Assembler::Equal, object, output,
                                      &Wrapper::family, ool->entry());
    masm.bind(&notProxy);
  }
  masm.move32(Imm32(0), output);
  masm.bind(&done);
  if (ool) {
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitIsObject(LIsObject* ins) {
  Register output = ToRegister(ins->output());
  ValueOperand value = ToValue(ins->object());
  masm.testObjectSet(Assembler::Equal, value, output);
}

void CodeGenerator::visitIsObjectAndBranch(LIsObjectAndBranch* ins) {
  ValueOperand value = ToValue(ins->input());

  MBasicBlock* ifTrue = ins->ifTrue();
  MBasicBlock* ifFalse = ins->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    masm.branchTestObject(Assembler::Equal, value,
                          getJumpLabelForBranch(ifTrue));
  } else {
    masm.branchTestObject(Assembler::NotEqual, value,
                          getJumpLabelForBranch(ifFalse));
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitIsNullOrUndefined(LIsNullOrUndefined* ins) {
  Register output = ToRegister(ins->output());
  ValueOperand value = ToValue(ins->value());

  Label isNotNull, done;
  masm.branchTestNull(Assembler::NotEqual, value, &isNotNull);

  masm.move32(Imm32(1), output);
  masm.jump(&done);

  masm.bind(&isNotNull);
  masm.testUndefinedSet(Assembler::Equal, value, output);

  masm.bind(&done);
}

void CodeGenerator::visitIsNullOrUndefinedAndBranch(
    LIsNullOrUndefinedAndBranch* ins) {
  Label* ifTrue = getJumpLabelForBranch(ins->ifTrue());
  Label* ifFalse = getJumpLabelForBranch(ins->ifFalse());
  ValueOperand value = ToValue(ins->input());

  ScratchTagScope tag(masm, value);
  masm.splitTagForTest(value, tag);

  masm.branchTestNull(Assembler::Equal, tag, ifTrue);
  masm.branchTestUndefined(Assembler::Equal, tag, ifTrue);

  if (!isNextBlock(ins->ifFalse()->lir())) {
    masm.jump(ifFalse);
  }
}

void CodeGenerator::visitHasClass(LHasClass* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register output = ToRegister(ins->output());

  masm.loadObjClassUnsafe(lhs, output);
  masm.cmpPtrSet(Assembler::Equal, output, ImmPtr(ins->mir()->getClass()),
                 output);
}

void CodeGenerator::visitHasShape(LHasShape* ins) {
  Register obj = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  // Note: no Spectre mitigations are needed here because this shape check only
  // affects correctness.
  masm.loadObjShapeUnsafe(obj, output);
  masm.cmpPtrSet(Assembler::Equal, output, ImmGCPtr(ins->mir()->shape()),
                 output);
}

void CodeGenerator::visitGuardToClass(LGuardToClass* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register temp = ToRegister(ins->temp0());

  // branchTestObjClass may zero the object register on speculative paths
  // (we should have a defineReuseInput allocation in this case).
  Register spectreRegToZero = lhs;

  Label notEqual;

  masm.branchTestObjClass(Assembler::NotEqual, lhs, ins->mir()->getClass(),
                          temp, spectreRegToZero, &notEqual);

  // Can't return null-return here, so bail.
  bailoutFrom(&notEqual, ins->snapshot());
}

void CodeGenerator::visitGuardToEitherClass(LGuardToEitherClass* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register temp = ToRegister(ins->temp0());

  // branchTestObjClass may zero the object register on speculative paths
  // (we should have a defineReuseInput allocation in this case).
  Register spectreRegToZero = lhs;

  Label notEqual;

  masm.branchTestObjClass(Assembler::NotEqual, lhs,
                          {ins->mir()->getClass1(), ins->mir()->getClass2()},
                          temp, spectreRegToZero, &notEqual);

  // Can't return null-return here, so bail.
  bailoutFrom(&notEqual, ins->snapshot());
}

void CodeGenerator::visitGuardToFunction(LGuardToFunction* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register temp = ToRegister(ins->temp0());

  // branchTestObjClass may zero the object register on speculative paths
  // (we should have a defineReuseInput allocation in this case).
  Register spectreRegToZero = lhs;

  Label notEqual;

  masm.branchTestObjIsFunction(Assembler::NotEqual, lhs, temp, spectreRegToZero,
                               &notEqual);

  // Can't return null-return here, so bail.
  bailoutFrom(&notEqual, ins->snapshot());
}

void CodeGenerator::visitObjectClassToString(LObjectClassToString* lir) {
  Register obj = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp0());

  using Fn = JSString* (*)(JSContext*, JSObject*);
  masm.setupAlignedABICall();
  masm.loadJSContext(temp);
  masm.passABIArg(temp);
  masm.passABIArg(obj);
  masm.callWithABI<Fn, js::ObjectClassToString>();

  bailoutCmpPtr(Assembler::Equal, ReturnReg, ImmWord(0), lir->snapshot());
}

void CodeGenerator::visitWasmParameter(LWasmParameter* lir) {}

void CodeGenerator::visitWasmParameterI64(LWasmParameterI64* lir) {}

void CodeGenerator::visitWasmReturn(LWasmReturn* lir) {
  // Don't emit a jump to the return label if this is the last block.
  if (current->mir() != *gen->graph().poBegin()) {
    masm.jump(&returnLabel_);
  }
}

void CodeGenerator::visitWasmReturnI64(LWasmReturnI64* lir) {
  // Don't emit a jump to the return label if this is the last block.
  if (current->mir() != *gen->graph().poBegin()) {
    masm.jump(&returnLabel_);
  }
}

void CodeGenerator::visitWasmReturnVoid(LWasmReturnVoid* lir) {
  // Don't emit a jump to the return label if this is the last block.
  if (current->mir() != *gen->graph().poBegin()) {
    masm.jump(&returnLabel_);
  }
}

void CodeGenerator::emitAssertRangeI(MIRType type, const Range* r,
                                     Register input) {
  // Check the lower bound.
  if (r->hasInt32LowerBound() && r->lower() > INT32_MIN) {
    Label success;
    if (type == MIRType::Int32 || type == MIRType::Boolean) {
      masm.branch32(Assembler::GreaterThanOrEqual, input, Imm32(r->lower()),
                    &success);
    } else {
      MOZ_ASSERT(type == MIRType::IntPtr);
      masm.branchPtr(Assembler::GreaterThanOrEqual, input, Imm32(r->lower()),
                     &success);
    }
    masm.assumeUnreachable(
        "Integer input should be equal or higher than Lowerbound.");
    masm.bind(&success);
  }

  // Check the upper bound.
  if (r->hasInt32UpperBound() && r->upper() < INT32_MAX) {
    Label success;
    if (type == MIRType::Int32 || type == MIRType::Boolean) {
      masm.branch32(Assembler::LessThanOrEqual, input, Imm32(r->upper()),
                    &success);
    } else {
      MOZ_ASSERT(type == MIRType::IntPtr);
      masm.branchPtr(Assembler::LessThanOrEqual, input, Imm32(r->upper()),
                     &success);
    }
    masm.assumeUnreachable(
        "Integer input should be lower or equal than Upperbound.");
    masm.bind(&success);
  }

  // For r->canHaveFractionalPart(), r->canBeNegativeZero(), and
  // r->exponent(), there's nothing to check, because if we ended up in the
  // integer range checking code, the value is already in an integer register
  // in the integer range.
}

void CodeGenerator::emitAssertRangeD(const Range* r, FloatRegister input,
                                     FloatRegister temp) {
  // Check the lower bound.
  if (r->hasInt32LowerBound()) {
    Label success;
    masm.loadConstantDouble(r->lower(), temp);
    if (r->canBeNaN()) {
      masm.branchDouble(Assembler::DoubleUnordered, input, input, &success);
    }
    masm.branchDouble(Assembler::DoubleGreaterThanOrEqual, input, temp,
                      &success);
    masm.assumeUnreachable(
        "Double input should be equal or higher than Lowerbound.");
    masm.bind(&success);
  }
  // Check the upper bound.
  if (r->hasInt32UpperBound()) {
    Label success;
    masm.loadConstantDouble(r->upper(), temp);
    if (r->canBeNaN()) {
      masm.branchDouble(Assembler::DoubleUnordered, input, input, &success);
    }
    masm.branchDouble(Assembler::DoubleLessThanOrEqual, input, temp, &success);
    masm.assumeUnreachable(
        "Double input should be lower or equal than Upperbound.");
    masm.bind(&success);
  }

  // This code does not yet check r->canHaveFractionalPart(). This would require
  // new assembler interfaces to make rounding instructions available.

  if (!r->canBeNegativeZero()) {
    Label success;

    // First, test for being equal to 0.0, which also includes -0.0.
    masm.loadConstantDouble(0.0, temp);
    masm.branchDouble(Assembler::DoubleNotEqualOrUnordered, input, temp,
                      &success);

    // The easiest way to distinguish -0.0 from 0.0 is that 1.0/-0.0 is
    // -Infinity instead of Infinity.
    masm.loadConstantDouble(1.0, temp);
    masm.divDouble(input, temp);
    masm.branchDouble(Assembler::DoubleGreaterThan, temp, input, &success);

    masm.assumeUnreachable("Input shouldn't be negative zero.");

    masm.bind(&success);
  }

  if (!r->hasInt32Bounds() && !r->canBeInfiniteOrNaN() &&
      r->exponent() < FloatingPoint<double>::kExponentBias) {
    // Check the bounds implied by the maximum exponent.
    Label exponentLoOk;
    masm.loadConstantDouble(pow(2.0, r->exponent() + 1), temp);
    masm.branchDouble(Assembler::DoubleUnordered, input, input, &exponentLoOk);
    masm.branchDouble(Assembler::DoubleLessThanOrEqual, input, temp,
                      &exponentLoOk);
    masm.assumeUnreachable("Check for exponent failed.");
    masm.bind(&exponentLoOk);

    Label exponentHiOk;
    masm.loadConstantDouble(-pow(2.0, r->exponent() + 1), temp);
    masm.branchDouble(Assembler::DoubleUnordered, input, input, &exponentHiOk);
    masm.branchDouble(Assembler::DoubleGreaterThanOrEqual, input, temp,
                      &exponentHiOk);
    masm.assumeUnreachable("Check for exponent failed.");
    masm.bind(&exponentHiOk);
  } else if (!r->hasInt32Bounds() && !r->canBeNaN()) {
    // If we think the value can't be NaN, check that it isn't.
    Label notnan;
    masm.branchDouble(Assembler::DoubleOrdered, input, input, &notnan);
    masm.assumeUnreachable("Input shouldn't be NaN.");
    masm.bind(&notnan);

    // If we think the value also can't be an infinity, check that it isn't.
    if (!r->canBeInfiniteOrNaN()) {
      Label notposinf;
      masm.loadConstantDouble(PositiveInfinity<double>(), temp);
      masm.branchDouble(Assembler::DoubleLessThan, input, temp, &notposinf);
      masm.assumeUnreachable("Input shouldn't be +Inf.");
      masm.bind(&notposinf);

      Label notneginf;
      masm.loadConstantDouble(NegativeInfinity<double>(), temp);
      masm.branchDouble(Assembler::DoubleGreaterThan, input, temp, &notneginf);
      masm.assumeUnreachable("Input shouldn't be -Inf.");
      masm.bind(&notneginf);
    }
  }
}

void CodeGenerator::visitAssertClass(LAssertClass* ins) {
  Register obj = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp0());

  Label success;
  if (ins->mir()->getClass() == &FunctionClass) {
    // Allow both possible function classes here.
    masm.branchTestObjIsFunctionNoSpectreMitigations(Assembler::Equal, obj,
                                                     temp, &success);
  } else {
    masm.branchTestObjClassNoSpectreMitigations(
        Assembler::Equal, obj, ins->mir()->getClass(), temp, &success);
  }
  masm.assumeUnreachable("Wrong KnownClass during run-time");
  masm.bind(&success);
}

void CodeGenerator::visitAssertShape(LAssertShape* ins) {
  Register obj = ToRegister(ins->input());

  Label success;
  masm.branchTestObjShapeNoSpectreMitigations(Assembler::Equal, obj,
                                              ins->mir()->shape(), &success);
  masm.assumeUnreachable("Wrong Shape during run-time");
  masm.bind(&success);
}

void CodeGenerator::visitAssertRangeI(LAssertRangeI* ins) {
  Register input = ToRegister(ins->input());
  const Range* r = ins->mir()->assertedRange();

  emitAssertRangeI(ins->mir()->input()->type(), r, input);
}

void CodeGenerator::visitAssertRangeD(LAssertRangeD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister temp = ToFloatRegister(ins->temp0());
  const Range* r = ins->mir()->assertedRange();

  emitAssertRangeD(r, input, temp);
}

void CodeGenerator::visitAssertRangeF(LAssertRangeF* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister temp = ToFloatRegister(ins->temp0());
  FloatRegister temp2 = ToFloatRegister(ins->temp1());

  const Range* r = ins->mir()->assertedRange();

  masm.convertFloat32ToDouble(input, temp);
  emitAssertRangeD(r, temp, temp2);
}

void CodeGenerator::visitAssertRangeV(LAssertRangeV* ins) {
  const Range* r = ins->mir()->assertedRange();
  ValueOperand value = ToValue(ins->input());
  Label done;

  {
    ScratchTagScope tag(masm, value);
    masm.splitTagForTest(value, tag);

    {
      Label isNotInt32;
      masm.branchTestInt32(Assembler::NotEqual, tag, &isNotInt32);
      {
        ScratchTagScopeRelease _(&tag);
        Register unboxInt32 = ToTempUnboxRegister(ins->temp0());
        Register input = masm.extractInt32(value, unboxInt32);
        emitAssertRangeI(MIRType::Int32, r, input);
        masm.jump(&done);
      }
      masm.bind(&isNotInt32);
    }

    {
      Label isNotDouble;
      masm.branchTestDouble(Assembler::NotEqual, tag, &isNotDouble);
      {
        ScratchTagScopeRelease _(&tag);
        FloatRegister input = ToFloatRegister(ins->temp1());
        FloatRegister temp = ToFloatRegister(ins->temp2());
        masm.unboxDouble(value, input);
        emitAssertRangeD(r, input, temp);
        masm.jump(&done);
      }
      masm.bind(&isNotDouble);
    }
  }

  masm.assumeUnreachable("Incorrect range for Value.");
  masm.bind(&done);
}

void CodeGenerator::visitInterruptCheck(LInterruptCheck* lir) {
  using Fn = bool (*)(JSContext*);
  OutOfLineCode* ool =
      oolCallVM<Fn, InterruptCheck>(lir, ArgList(), StoreNothing());

  const void* interruptAddr = gen->runtime->addressOfInterruptBits();
  masm.branch32(Assembler::NotEqual, AbsoluteAddress(interruptAddr), Imm32(0),
                ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitWasmInterruptCheck(LWasmInterruptCheck* lir) {
  MOZ_ASSERT(gen->compilingWasm());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    emitResumableWasmTrapOOL(lir, masm.framePushed(),
                             lir->mir()->trapSiteDesc(),
                             wasm::Trap::CheckInterrupt);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());
  masm.branch32(
      Assembler::NotEqual,
      Address(ToRegister(lir->instance()), wasm::Instance::offsetOfInterrupt()),
      Imm32(0), ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitWasmTrap(LWasmTrap* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  const MWasmTrap* mir = lir->mir();

  masm.wasmTrap(mir->trap(), mir->trapSiteDesc());
}

void CodeGenerator::visitWasmRefAsNonNull(LWasmRefAsNonNull* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  const MWasmRefAsNonNull* mir = lir->mir();
  Label nonNull;
  Register ref = ToRegister(lir->ref());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    masm.wasmTrap(wasm::Trap::NullPointerDereference, mir->trapSiteDesc());
  });
  addOutOfLineCode(ool, mir);
  masm.branchWasmAnyRefIsNull(true, ref, ool->entry());
}

void CodeGenerator::visitWasmRefTestAbstract(LWasmRefTestAbstract* ins) {
  MOZ_ASSERT(gen->compilingWasm());

  const MWasmRefTestAbstract* mir = ins->mir();
  MOZ_ASSERT(!mir->destType().isTypeRef());

  Register ref = ToRegister(ins->ref());
  Register superSTV = Register::Invalid();
  Register scratch1 = ToTempRegisterOrInvalid(ins->temp0());
  Register scratch2 = Register::Invalid();
  Register result = ToRegister(ins->output());
  Label onSuccess;
  Label onFail;
  Label join;
  masm.branchWasmRefIsSubtype(ref, mir->ref()->wasmRefType(), mir->destType(),
                              &onSuccess, /*onSuccess=*/true, superSTV,
                              scratch1, scratch2);
  masm.bind(&onFail);
  masm.xor32(result, result);
  masm.jump(&join);
  masm.bind(&onSuccess);
  masm.move32(Imm32(1), result);
  masm.bind(&join);
}

void CodeGenerator::visitWasmRefTestConcrete(LWasmRefTestConcrete* ins) {
  MOZ_ASSERT(gen->compilingWasm());

  const MWasmRefTestConcrete* mir = ins->mir();
  MOZ_ASSERT(mir->destType().isTypeRef());

  Register ref = ToRegister(ins->ref());
  Register superSTV = ToRegister(ins->superSTV());
  Register scratch1 = ToRegister(ins->temp0());
  Register scratch2 = ToTempRegisterOrInvalid(ins->temp1());
  Register result = ToRegister(ins->output());
  Label onSuccess;
  Label join;
  masm.branchWasmRefIsSubtype(ref, mir->ref()->wasmRefType(), mir->destType(),
                              &onSuccess, /*onSuccess=*/true, superSTV,
                              scratch1, scratch2);
  masm.move32(Imm32(0), result);
  masm.jump(&join);
  masm.bind(&onSuccess);
  masm.move32(Imm32(1), result);
  masm.bind(&join);
}

void CodeGenerator::visitWasmRefTestAbstractAndBranch(
    LWasmRefTestAbstractAndBranch* ins) {
  MOZ_ASSERT(gen->compilingWasm());
  Register ref = ToRegister(ins->ref());
  Register scratch1 = ToTempRegisterOrInvalid(ins->temp0());
  Label* onSuccess = getJumpLabelForBranch(ins->ifTrue());
  Label* onFail = getJumpLabelForBranch(ins->ifFalse());
  masm.branchWasmRefIsSubtype(
      ref, ins->sourceType(), ins->destType(), onSuccess, /*onSuccess=*/true,
      Register::Invalid(), scratch1, Register::Invalid());
  masm.jump(onFail);
}

void CodeGenerator::visitWasmRefTestConcreteAndBranch(
    LWasmRefTestConcreteAndBranch* ins) {
  MOZ_ASSERT(gen->compilingWasm());
  Register ref = ToRegister(ins->ref());
  Register superSTV = ToRegister(ins->superSTV());
  Register scratch1 = ToRegister(ins->temp0());
  Register scratch2 = ToTempRegisterOrInvalid(ins->temp1());
  Label* onSuccess = getJumpLabelForBranch(ins->ifTrue());
  Label* onFail = getJumpLabelForBranch(ins->ifFalse());
  masm.branchWasmRefIsSubtype(ref, ins->sourceType(), ins->destType(),
                              onSuccess, /*onSuccess=*/true, superSTV, scratch1,
                              scratch2);
  masm.jump(onFail);
}

void CodeGenerator::visitWasmRefCastAbstract(LWasmRefCastAbstract* ins) {
  MOZ_ASSERT(gen->compilingWasm());

  const MWasmRefCastAbstract* mir = ins->mir();
  MOZ_ASSERT(!mir->destType().isTypeRef());

  Register ref = ToRegister(ins->ref());
  Register superSTV = Register::Invalid();
  Register scratch1 = ToTempRegisterOrInvalid(ins->temp0());
  Register scratch2 = Register::Invalid();
  MOZ_ASSERT(ref == ToRegister(ins->output()));
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    masm.wasmTrap(wasm::Trap::BadCast, mir->trapSiteDesc());
  });
  addOutOfLineCode(ool, ins->mir());
  masm.branchWasmRefIsSubtype(ref, mir->ref()->wasmRefType(), mir->destType(),
                              ool->entry(), /*onSuccess=*/false, superSTV,
                              scratch1, scratch2);
}

void CodeGenerator::visitWasmRefCastConcrete(LWasmRefCastConcrete* ins) {
  MOZ_ASSERT(gen->compilingWasm());

  const MWasmRefCastConcrete* mir = ins->mir();
  MOZ_ASSERT(mir->destType().isTypeRef());

  Register ref = ToRegister(ins->ref());
  Register superSTV = ToRegister(ins->superSTV());
  Register scratch1 = ToRegister(ins->temp0());
  Register scratch2 = ToTempRegisterOrInvalid(ins->temp1());
  MOZ_ASSERT(ref == ToRegister(ins->output()));
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    masm.wasmTrap(wasm::Trap::BadCast, mir->trapSiteDesc());
  });
  addOutOfLineCode(ool, ins->mir());
  masm.branchWasmRefIsSubtype(ref, mir->ref()->wasmRefType(), mir->destType(),
                              ool->entry(), /*onSuccess=*/false, superSTV,
                              scratch1, scratch2);
}

void CodeGenerator::callWasmStructAllocFun(
    LInstruction* lir, wasm::SymbolicAddress fun, Register typeDefIndex,
    Register allocSite, Register output,
    const wasm::TrapSiteDesc& trapSiteDesc) {
  MOZ_ASSERT(fun == wasm::SymbolicAddress::StructNewIL_true ||
             fun == wasm::SymbolicAddress::StructNewIL_false ||
             fun == wasm::SymbolicAddress::StructNewOOL_true ||
             fun == wasm::SymbolicAddress::StructNewOOL_false);
  MOZ_ASSERT(wasm::SASigStructNewIL_true.failureMode ==
             wasm::FailureMode::FailOnNullPtr);
  MOZ_ASSERT(wasm::SASigStructNewIL_false.failureMode ==
             wasm::FailureMode::FailOnNullPtr);
  MOZ_ASSERT(wasm::SASigStructNewOOL_true.failureMode ==
             wasm::FailureMode::FailOnNullPtr);
  MOZ_ASSERT(wasm::SASigStructNewOOL_false.failureMode ==
             wasm::FailureMode::FailOnNullPtr);

  masm.Push(InstanceReg);
  int32_t framePushedAfterInstance = masm.framePushed();
  saveLive(lir);

  masm.setupWasmABICall();
  masm.passABIArg(InstanceReg);
  masm.passABIArg(typeDefIndex);
  masm.passABIArg(allocSite);
  int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
  CodeOffset offset =
      masm.callWithABI(trapSiteDesc.bytecodeOffset, fun,
                       mozilla::Some(instanceOffset), ABIType::General);
  masm.storeCallPointerResult(output);

  markSafepointAt(offset.offset(), lir);
  lir->safepoint()->setFramePushedAtStackMapBase(framePushedAfterInstance);
  lir->safepoint()->setWasmSafepointKind(WasmSafepointKind::CodegenCall);

  restoreLive(lir);
  masm.Pop(InstanceReg);
#if JS_CODEGEN_ARM64
  masm.syncStackPtr();
#endif

  masm.wasmTrapOnFailedInstanceCall(output, wasm::FailureMode::FailOnNullPtr,
                                    trapSiteDesc);
}

void CodeGenerator::visitWasmNewStructObject(LWasmNewStructObject* lir) {
  MOZ_ASSERT(gen->compilingWasm());

  MWasmNewStructObject* mir = lir->mir();
  uint32_t typeDefIndex = wasmCodeMeta()->types->indexOf(mir->typeDef());

  Register allocSite = ToRegister(lir->allocSite());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  if (mir->isOutline()) {
    wasm::SymbolicAddress fun = mir->zeroFields()
                                    ? wasm::SymbolicAddress::StructNewOOL_true
                                    : wasm::SymbolicAddress::StructNewOOL_false;

    masm.move32(Imm32(typeDefIndex), temp);
    callWasmStructAllocFun(lir, fun, temp, allocSite, output,
                           mir->trapSiteDesc());
  } else {
    wasm::SymbolicAddress fun = mir->zeroFields()
                                    ? wasm::SymbolicAddress::StructNewIL_true
                                    : wasm::SymbolicAddress::StructNewIL_false;

    Register instance = ToRegister(lir->instance());
    MOZ_ASSERT(instance == InstanceReg);

    auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
      masm.move32(Imm32(typeDefIndex), temp);
      callWasmStructAllocFun(lir, fun, temp, allocSite, output,
                             mir->trapSiteDesc());
      masm.jump(ool.rejoin());
    });
    addOutOfLineCode(ool, lir->mir());

    size_t offsetOfTypeDefData = wasm::Instance::offsetInData(
        wasmCodeMeta()->offsetOfTypeDefInstanceData(typeDefIndex));
    masm.wasmNewStructObject(instance, output, allocSite, temp,
                             offsetOfTypeDefData, ool->entry(),
                             mir->allocKind(), mir->zeroFields());

    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::callWasmArrayAllocFun(
    LInstruction* lir, wasm::SymbolicAddress fun, Register numElements,
    Register typeDefIndex, Register allocSite, Register output,
    const wasm::TrapSiteDesc& trapSiteDesc) {
  MOZ_ASSERT(fun == wasm::SymbolicAddress::ArrayNew_true ||
             fun == wasm::SymbolicAddress::ArrayNew_false);
  MOZ_ASSERT(wasm::SASigArrayNew_true.failureMode ==
             wasm::FailureMode::FailOnNullPtr);
  MOZ_ASSERT(wasm::SASigArrayNew_false.failureMode ==
             wasm::FailureMode::FailOnNullPtr);

  masm.Push(InstanceReg);
  int32_t framePushedAfterInstance = masm.framePushed();
  saveLive(lir);

  masm.setupWasmABICall();
  masm.passABIArg(InstanceReg);
  masm.passABIArg(numElements);
  masm.passABIArg(typeDefIndex);
  masm.passABIArg(allocSite);
  int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
  CodeOffset offset =
      masm.callWithABI(trapSiteDesc.bytecodeOffset, fun,
                       mozilla::Some(instanceOffset), ABIType::General);
  masm.storeCallPointerResult(output);

  markSafepointAt(offset.offset(), lir);
  lir->safepoint()->setFramePushedAtStackMapBase(framePushedAfterInstance);
  lir->safepoint()->setWasmSafepointKind(WasmSafepointKind::CodegenCall);

  restoreLive(lir);
  masm.Pop(InstanceReg);
#if JS_CODEGEN_ARM64
  masm.syncStackPtr();
#endif

  masm.wasmTrapOnFailedInstanceCall(output, wasm::FailureMode::FailOnNullPtr,
                                    trapSiteDesc);
}

void CodeGenerator::visitWasmNewArrayObject(LWasmNewArrayObject* lir) {
  MOZ_ASSERT(gen->compilingWasm());

  MWasmNewArrayObject* mir = lir->mir();
  uint32_t typeDefIndex = wasmCodeMeta()->types->indexOf(mir->typeDef());

  Register allocSite = ToRegister(lir->allocSite());
  Register output = ToRegister(lir->output());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  wasm::SymbolicAddress fun = mir->zeroFields()
                                  ? wasm::SymbolicAddress::ArrayNew_true
                                  : wasm::SymbolicAddress::ArrayNew_false;

  if (lir->numElements()->isConstant()) {
    // numElements is constant, so we can do optimized code generation.
    uint32_t numElements = lir->numElements()->toConstant()->toInt32();
    CheckedUint32 storageBytes =
        WasmArrayObject::calcStorageBytesChecked(mir->elemSize(), numElements);
    if (!storageBytes.isValid() ||
        storageBytes.value() > WasmArrayObject_MaxInlineBytes) {
      // Too much array data to store inline. Immediately perform an instance
      // call to handle the out-of-line storage (or the trap).
      masm.move32(Imm32(typeDefIndex), temp0);
      masm.move32(Imm32(numElements), temp1);
      callWasmArrayAllocFun(lir, fun, temp1, temp0, allocSite, output,
                            mir->trapSiteDesc());
    } else {
      // storageBytes is small enough to be stored inline in WasmArrayObject.
      // Attempt a nursery allocation and fall back to an instance call if it
      // fails.
      Register instance = ToRegister(lir->instance());
      MOZ_ASSERT(instance == InstanceReg);

      auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
        masm.move32(Imm32(typeDefIndex), temp0);
        masm.move32(Imm32(numElements), temp1);
        callWasmArrayAllocFun(lir, fun, temp1, temp0, allocSite, output,
                              mir->trapSiteDesc());
        masm.jump(ool.rejoin());
      });
      addOutOfLineCode(ool, lir->mir());

      size_t offsetOfTypeDefData = wasm::Instance::offsetInData(
          wasmCodeMeta()->offsetOfTypeDefInstanceData(typeDefIndex));
      masm.wasmNewArrayObjectFixed(
          instance, output, allocSite, temp0, temp1, offsetOfTypeDefData,
          ool->entry(), numElements, storageBytes.value(), mir->zeroFields());

      masm.bind(ool->rejoin());
    }
  } else {
    // numElements is dynamic. Attempt a dynamic inline-storage nursery
    // allocation and fall back to an instance call if it fails.
    Register instance = ToRegister(lir->instance());
    MOZ_ASSERT(instance == InstanceReg);
    Register numElements = ToRegister(lir->numElements());

    auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
      masm.move32(Imm32(typeDefIndex), temp0);
      callWasmArrayAllocFun(lir, fun, numElements, temp0, allocSite, output,
                            mir->trapSiteDesc());
      masm.jump(ool.rejoin());
    });
    addOutOfLineCode(ool, lir->mir());

    size_t offsetOfTypeDefData = wasm::Instance::offsetInData(
        wasmCodeMeta()->offsetOfTypeDefInstanceData(typeDefIndex));
    masm.wasmNewArrayObject(instance, output, numElements, allocSite, temp1,
                            offsetOfTypeDefData, ool->entry(), mir->elemSize(),
                            mir->zeroFields());

    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitWasmHeapReg(LWasmHeapReg* ins) {
#ifdef WASM_HAS_HEAPREG
  masm.movePtr(HeapReg, ToRegister(ins->output()));
#else
  MOZ_CRASH();
#endif
}

void CodeGenerator::emitResumableWasmTrapOOL(
    LInstruction* lir, size_t framePushed,
    const wasm::TrapSiteDesc& trapSiteDesc, wasm::Trap trap) {
  masm.wasmTrap(trap, trapSiteDesc);

  markSafepointAt(masm.currentOffset(), lir);

  // Note that masm.framePushed() doesn't include the register dump area.
  // That will be taken into account when the StackMap is created from the
  // LSafepoint.
  lir->safepoint()->setFramePushedAtStackMapBase(framePushed);
  lir->safepoint()->setWasmSafepointKind(WasmSafepointKind::Trap);
}

void CodeGenerator::visitWasmBoundsCheck(LWasmBoundsCheck* ins) {
  const MWasmBoundsCheck* mir = ins->mir();
  Register ptr = ToRegister(ins->ptr());
  Register boundsCheckLimit = ToRegister(ins->boundsCheckLimit());
  // When there are no spectre mitigations in place, branching out-of-line to
  // the trap is a big performance win, but with mitigations it's trickier.  See
  // bug 1680243.
  if (JitOptions.spectreIndexMasking) {
    Label ok;
    masm.wasmBoundsCheck32(Assembler::Below, ptr, boundsCheckLimit, &ok);
    masm.wasmTrap(wasm::Trap::OutOfBounds, mir->trapSiteDesc());
    masm.bind(&ok);
  } else {
    auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
      masm.wasmTrap(wasm::Trap::OutOfBounds, mir->trapSiteDesc());
    });
    addOutOfLineCode(ool, mir);
    masm.wasmBoundsCheck32(Assembler::AboveOrEqual, ptr, boundsCheckLimit,
                           ool->entry());
  }
}

void CodeGenerator::visitWasmBoundsCheck64(LWasmBoundsCheck64* ins) {
  const MWasmBoundsCheck* mir = ins->mir();
  Register64 ptr = ToRegister64(ins->ptr());
  Register64 boundsCheckLimit = ToRegister64(ins->boundsCheckLimit());
  // See above.
  if (JitOptions.spectreIndexMasking) {
    Label ok;
    masm.wasmBoundsCheck64(Assembler::Below, ptr, boundsCheckLimit, &ok);
    masm.wasmTrap(wasm::Trap::OutOfBounds, mir->trapSiteDesc());
    masm.bind(&ok);
  } else {
    auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
      masm.wasmTrap(wasm::Trap::OutOfBounds, mir->trapSiteDesc());
    });
    addOutOfLineCode(ool, mir);
    masm.wasmBoundsCheck64(Assembler::AboveOrEqual, ptr, boundsCheckLimit,
                           ool->entry());
  }
}

void CodeGenerator::visitWasmBoundsCheckRange32(LWasmBoundsCheckRange32* ins) {
  const MWasmBoundsCheckRange32* mir = ins->mir();
  Register index = ToRegister(ins->index());
  Register length = ToRegister(ins->length());
  Register limit = ToRegister(ins->limit());
  Register tmp = ToRegister(ins->temp0());

  masm.wasmBoundsCheckRange32(index, length, limit, tmp, mir->trapSiteDesc());
}

void CodeGenerator::visitWasmAlignmentCheck(LWasmAlignmentCheck* ins) {
  const MWasmAlignmentCheck* mir = ins->mir();
  Register ptr = ToRegister(ins->ptr());
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    masm.wasmTrap(wasm::Trap::UnalignedAccess, mir->trapSiteDesc());
  });
  addOutOfLineCode(ool, mir);
  masm.branchTest32(Assembler::NonZero, ptr, Imm32(mir->byteSize() - 1),
                    ool->entry());
}

void CodeGenerator::visitWasmAlignmentCheck64(LWasmAlignmentCheck64* ins) {
  const MWasmAlignmentCheck* mir = ins->mir();
  Register64 ptr = ToRegister64(ins->ptr());
#ifdef JS_64BIT
  Register r = ptr.reg;
#else
  Register r = ptr.low;
#endif
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    masm.wasmTrap(wasm::Trap::UnalignedAccess, mir->trapSiteDesc());
  });
  addOutOfLineCode(ool, mir);
  masm.branchTestPtr(Assembler::NonZero, r, Imm32(mir->byteSize() - 1),
                     ool->entry());
}

void CodeGenerator::visitWasmLoadInstance(LWasmLoadInstance* ins) {
  switch (ins->mir()->type()) {
    case MIRType::WasmAnyRef:
    case MIRType::Pointer:
      masm.loadPtr(Address(ToRegister(ins->instance()), ins->mir()->offset()),
                   ToRegister(ins->output()));
      break;
    case MIRType::Int32:
      masm.load32(Address(ToRegister(ins->instance()), ins->mir()->offset()),
                  ToRegister(ins->output()));
      break;
    default:
      MOZ_CRASH("MIRType not supported in WasmLoadInstance");
  }
}

void CodeGenerator::visitWasmLoadInstance64(LWasmLoadInstance64* ins) {
  MOZ_ASSERT(ins->mir()->type() == MIRType::Int64);
  masm.load64(Address(ToRegister(ins->instance()), ins->mir()->offset()),
              ToOutRegister64(ins));
}

void CodeGenerator::incrementWarmUpCounter(AbsoluteAddress warmUpCount,
                                           JSScript* script, Register tmp) {
  // The code depends on the JitScript* not being discarded without also
  // invalidating Ion code. Assert this.
#ifdef DEBUG
  Label ok;
  masm.movePtr(ImmGCPtr(script), tmp);
  masm.loadJitScript(tmp, tmp);
  masm.branchPtr(Assembler::Equal, tmp, ImmPtr(script->jitScript()), &ok);
  masm.assumeUnreachable("Didn't find JitScript?");
  masm.bind(&ok);
#endif

  masm.load32(warmUpCount, tmp);
  masm.add32(Imm32(1), tmp);
  masm.store32(tmp, warmUpCount);
}

void CodeGenerator::visitIncrementWarmUpCounter(LIncrementWarmUpCounter* ins) {
  Register tmp = ToRegister(ins->temp0());

  AbsoluteAddress warmUpCount =
      AbsoluteAddress(ins->mir()->script()->jitScript())
          .offset(JitScript::offsetOfWarmUpCount());
  incrementWarmUpCounter(warmUpCount, ins->mir()->script(), tmp);
}

void CodeGenerator::visitLexicalCheck(LLexicalCheck* ins) {
  ValueOperand inputValue = ToValue(ins->input());
  Label bail;
  masm.branchTestMagicValue(Assembler::Equal, inputValue,
                            JS_UNINITIALIZED_LEXICAL, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitThrowRuntimeLexicalError(
    LThrowRuntimeLexicalError* ins) {
  pushArg(Imm32(ins->mir()->errorNumber()));

  using Fn = bool (*)(JSContext*, unsigned);
  callVM<Fn, jit::ThrowRuntimeLexicalError>(ins);
}

void CodeGenerator::visitThrowMsg(LThrowMsg* ins) {
  pushArg(Imm32(static_cast<int32_t>(ins->mir()->throwMsgKind())));

  using Fn = bool (*)(JSContext*, unsigned);
  callVM<Fn, js::ThrowMsgOperation>(ins);
}

void CodeGenerator::visitGlobalDeclInstantiation(
    LGlobalDeclInstantiation* ins) {
  pushArg(ImmPtr(ins->mir()->resumePoint()->pc()));
  pushArg(ImmGCPtr(ins->mir()->block()->info().script()));

  using Fn = bool (*)(JSContext*, HandleScript, const jsbytecode*);
  callVM<Fn, GlobalDeclInstantiationFromIon>(ins);
}

void CodeGenerator::visitDebugger(LDebugger* ins) {
  Register cx = ToRegister(ins->temp0());

  masm.loadJSContext(cx);
  using Fn = bool (*)(JSContext* cx);
  masm.setupAlignedABICall();
  masm.passABIArg(cx);
  masm.callWithABI<Fn, GlobalHasLiveOnDebuggerStatement>();

  Label bail;
  masm.branchIfTrueBool(ReturnReg, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitNewTarget(LNewTarget* ins) {
  ValueOperand output = ToOutValue(ins);

  // if (isConstructing) output = argv[Max(numActualArgs, numFormalArgs)]
  Label notConstructing, done;
  Address calleeToken(FramePointer, JitFrameLayout::offsetOfCalleeToken());
  masm.branchTestPtr(Assembler::Zero, calleeToken,
                     Imm32(CalleeToken_FunctionConstructing), &notConstructing);

  Register argvLen = output.scratchReg();
  masm.loadNumActualArgs(FramePointer, argvLen);

  Label useNFormals;

  size_t numFormalArgs = ins->mirRaw()->block()->info().nargs();
  masm.branchPtr(Assembler::Below, argvLen, Imm32(numFormalArgs), &useNFormals);

  size_t argsOffset = JitFrameLayout::offsetOfActualArgs();
  {
    BaseValueIndex newTarget(FramePointer, argvLen, argsOffset);
    masm.loadValue(newTarget, output);
    masm.jump(&done);
  }

  masm.bind(&useNFormals);

  {
    Address newTarget(FramePointer,
                      argsOffset + (numFormalArgs * sizeof(Value)));
    masm.loadValue(newTarget, output);
    masm.jump(&done);
  }

  // else output = undefined
  masm.bind(&notConstructing);
  masm.moveValue(UndefinedValue(), output);
  masm.bind(&done);
}

void CodeGenerator::visitCheckReturn(LCheckReturn* ins) {
  ValueOperand returnValue = ToValue(ins->returnValue());
  ValueOperand thisValue = ToValue(ins->thisValue());
  ValueOperand output = ToOutValue(ins);

  using Fn = bool (*)(JSContext*, HandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, ThrowBadDerivedReturnOrUninitializedThis>(
      ins, ArgList(returnValue), StoreNothing());

  Label noChecks;
  masm.branchTestObject(Assembler::Equal, returnValue, &noChecks);
  masm.branchTestUndefined(Assembler::NotEqual, returnValue, ool->entry());
  masm.branchTestMagic(Assembler::Equal, thisValue, ool->entry());
  masm.moveValue(thisValue, output);
  masm.jump(ool->rejoin());
  masm.bind(&noChecks);
  masm.moveValue(returnValue, output);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckIsObj(LCheckIsObj* ins) {
  ValueOperand value = ToValue(ins->value());
  Register output = ToRegister(ins->output());

  using Fn = bool (*)(JSContext*, CheckIsObjectKind);
  OutOfLineCode* ool = oolCallVM<Fn, ThrowCheckIsObject>(
      ins, ArgList(Imm32(ins->mir()->checkKind())), StoreNothing());

  masm.fallibleUnboxObject(value, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckObjCoercible(LCheckObjCoercible* ins) {
  ValueOperand checkValue = ToValue(ins->value());

  using Fn = bool (*)(JSContext*, HandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, ThrowObjectCoercible>(
      ins, ArgList(checkValue), StoreNothing());
  masm.branchTestNull(Assembler::Equal, checkValue, ool->entry());
  masm.branchTestUndefined(Assembler::Equal, checkValue, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckClassHeritage(LCheckClassHeritage* ins) {
  ValueOperand heritage = ToValue(ins->heritage());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());

  using Fn = bool (*)(JSContext*, HandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, CheckClassHeritageOperation>(
      ins, ArgList(heritage), StoreNothing());

  masm.branchTestNull(Assembler::Equal, heritage, ool->rejoin());
  masm.fallibleUnboxObject(heritage, temp0, ool->entry());

  masm.isConstructor(temp0, temp1, ool->entry());
  masm.branchTest32(Assembler::Zero, temp1, temp1, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckThis(LCheckThis* ins) {
  ValueOperand thisValue = ToValue(ins->value());

  using Fn = bool (*)(JSContext*);
  OutOfLineCode* ool =
      oolCallVM<Fn, ThrowUninitializedThis>(ins, ArgList(), StoreNothing());
  masm.branchTestMagic(Assembler::Equal, thisValue, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckThisReinit(LCheckThisReinit* ins) {
  ValueOperand thisValue = ToValue(ins->thisValue());

  using Fn = bool (*)(JSContext*);
  OutOfLineCode* ool =
      oolCallVM<Fn, ThrowInitializedThis>(ins, ArgList(), StoreNothing());
  masm.branchTestMagic(Assembler::NotEqual, thisValue, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitGenerator(LGenerator* lir) {
  Register callee = ToRegister(lir->callee());
  Register environmentChain = ToRegister(lir->environmentChain());
  Register argsObject = ToRegister(lir->argsObject());

  pushArg(argsObject);
  pushArg(environmentChain);
  pushArg(ImmGCPtr(current->mir()->info().script()));
  pushArg(callee);

  using Fn = JSObject* (*)(JSContext* cx, HandleFunction, HandleScript,
                           HandleObject, HandleObject);
  callVM<Fn, CreateGenerator>(lir);
}

void CodeGenerator::visitAsyncResolve(LAsyncResolve* lir) {
  Register generator = ToRegister(lir->generator());
  ValueOperand value = ToValue(lir->value());

  pushArg(value);
  pushArg(generator);

  using Fn = JSObject* (*)(JSContext*, Handle<AsyncFunctionGeneratorObject*>,
                           HandleValue);
  callVM<Fn, js::AsyncFunctionResolve>(lir);
}

void CodeGenerator::visitAsyncReject(LAsyncReject* lir) {
  Register generator = ToRegister(lir->generator());
  ValueOperand reason = ToValue(lir->reason());
  ValueOperand stack = ToValue(lir->stack());

  pushArg(stack);
  pushArg(reason);
  pushArg(generator);

  using Fn = JSObject* (*)(JSContext*, Handle<AsyncFunctionGeneratorObject*>,
                           HandleValue, HandleValue);
  callVM<Fn, js::AsyncFunctionReject>(lir);
}

void CodeGenerator::visitAsyncAwait(LAsyncAwait* lir) {
  ValueOperand value = ToValue(lir->value());
  Register generator = ToRegister(lir->generator());

  pushArg(value);
  pushArg(generator);

  using Fn =
      JSObject* (*)(JSContext* cx, Handle<AsyncFunctionGeneratorObject*> genObj,
                    HandleValue value);
  callVM<Fn, js::AsyncFunctionAwait>(lir);
}

void CodeGenerator::visitCanSkipAwait(LCanSkipAwait* lir) {
  ValueOperand value = ToValue(lir->value());

  pushArg(value);

  using Fn = bool (*)(JSContext*, HandleValue, bool* canSkip);
  callVM<Fn, js::CanSkipAwait>(lir);
}

void CodeGenerator::visitMaybeExtractAwaitValue(LMaybeExtractAwaitValue* lir) {
  ValueOperand value = ToValue(lir->value());
  ValueOperand output = ToOutValue(lir);
  Register canSkip = ToRegister(lir->canSkip());

  Label cantExtract, finished;
  masm.branchIfFalseBool(canSkip, &cantExtract);

  pushArg(value);

  using Fn = bool (*)(JSContext*, HandleValue, MutableHandleValue);
  callVM<Fn, js::ExtractAwaitValue>(lir);
  masm.jump(&finished);
  masm.bind(&cantExtract);

  masm.moveValue(value, output);

  masm.bind(&finished);
}

void CodeGenerator::visitDebugCheckSelfHosted(LDebugCheckSelfHosted* ins) {
  ValueOperand checkValue = ToValue(ins->value());
  pushArg(checkValue);
  using Fn = bool (*)(JSContext*, HandleValue);
  callVM<Fn, js::Debug_CheckSelfHosted>(ins);
}

void CodeGenerator::visitRandom(LRandom* ins) {
  using mozilla::non_crypto::XorShift128PlusRNG;

  FloatRegister output = ToFloatRegister(ins->output());
  Register rngReg = ToRegister(ins->temp0());

  Register64 temp1 = ToRegister64(ins->temp1());
  Register64 temp2 = ToRegister64(ins->temp2());

  const XorShift128PlusRNG* rng = gen->realm->addressOfRandomNumberGenerator();
  masm.movePtr(ImmPtr(rng), rngReg);

  masm.randomDouble(rngReg, output, temp1, temp2);
  if (js::SupportDifferentialTesting()) {
    masm.loadConstantDouble(0.0, output);
  }
}

void CodeGenerator::visitSignExtendInt32(LSignExtendInt32* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  switch (ins->mir()->mode()) {
    case MSignExtendInt32::Byte:
      masm.move8SignExtend(input, output);
      break;
    case MSignExtendInt32::Half:
      masm.move16SignExtend(input, output);
      break;
  }
}

void CodeGenerator::visitSignExtendIntPtr(LSignExtendIntPtr* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  switch (ins->mir()->mode()) {
    case MSignExtendIntPtr::Byte:
      masm.move8SignExtendToPtr(input, output);
      break;
    case MSignExtendIntPtr::Half:
      masm.move16SignExtendToPtr(input, output);
      break;
    case MSignExtendIntPtr::Word:
      masm.move32SignExtendToPtr(input, output);
      break;
  }
}

void CodeGenerator::visitRotate(LRotate* ins) {
  MRotate* mir = ins->mir();
  Register input = ToRegister(ins->input());
  Register dest = ToRegister(ins->output());

  const LAllocation* count = ins->count();
  if (count->isConstant()) {
    int32_t c = ToInt32(count) & 0x1F;
    if (mir->isLeftRotate()) {
      masm.rotateLeft(Imm32(c), input, dest);
    } else {
      masm.rotateRight(Imm32(c), input, dest);
    }
  } else {
    Register creg = ToRegister(count);
    if (mir->isLeftRotate()) {
      masm.rotateLeft(creg, input, dest);
    } else {
      masm.rotateRight(creg, input, dest);
    }
  }
}

void CodeGenerator::visitRotateI64(LRotateI64* lir) {
  MRotate* mir = lir->mir();
  const LAllocation* count = lir->count();

  Register64 input = ToRegister64(lir->input());
  Register64 output = ToOutRegister64(lir);
  Register temp = ToTempRegisterOrInvalid(lir->temp0());

  if (count->isConstant()) {
    int32_t c = int32_t(count->toConstant()->toInt64() & 0x3F);
    if (!c) {
      if (input != output) {
        masm.move64(input, output);
      }
      return;
    }
    if (mir->isLeftRotate()) {
      masm.rotateLeft64(Imm32(c), input, output, temp);
    } else {
      masm.rotateRight64(Imm32(c), input, output, temp);
    }
  } else {
    if (mir->isLeftRotate()) {
      masm.rotateLeft64(ToRegister(count), input, output, temp);
    } else {
      masm.rotateRight64(ToRegister(count), input, output, temp);
    }
  }
}

void CodeGenerator::visitReinterpretCast(LReinterpretCast* lir) {
  MReinterpretCast* ins = lir->mir();

  MIRType to = ins->type();
  mozilla::DebugOnly<MIRType> from = ins->input()->type();

  switch (to) {
    case MIRType::Int32:
      MOZ_ASSERT(from == MIRType::Float32);
      masm.moveFloat32ToGPR(ToFloatRegister(lir->input()),
                            ToRegister(lir->output()));
      break;
    case MIRType::Float32:
      MOZ_ASSERT(from == MIRType::Int32);
      masm.moveGPRToFloat32(ToRegister(lir->input()),
                            ToFloatRegister(lir->output()));
      break;
    case MIRType::Double:
    case MIRType::Int64:
      MOZ_CRASH("not handled by this LIR opcode");
    default:
      MOZ_CRASH("unexpected ReinterpretCast");
  }
}

void CodeGenerator::visitReinterpretCastFromI64(LReinterpretCastFromI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Double);
  MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Int64);
  masm.moveGPR64ToDouble(ToRegister64(lir->input()),
                         ToFloatRegister(lir->output()));
}

void CodeGenerator::visitReinterpretCastToI64(LReinterpretCastToI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);
  MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Double);
  masm.moveDoubleToGPR64(ToFloatRegister(lir->input()), ToOutRegister64(lir));
}

void CodeGenerator::visitNaNToZero(LNaNToZero* lir) {
  FloatRegister input = ToFloatRegister(lir->input());

  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    FloatRegister output = ToFloatRegister(lir->output());
    masm.loadConstantDouble(0.0, output);
    masm.jump(ool.rejoin());
  });
  addOutOfLineCode(ool, lir->mir());

  if (lir->mir()->operandIsNeverNegativeZero()) {
    masm.branchDouble(Assembler::DoubleUnordered, input, input, ool->entry());
  } else {
    FloatRegister scratch = ToFloatRegister(lir->temp0());
    masm.loadConstantDouble(0.0, scratch);
    masm.branchDouble(Assembler::DoubleEqualOrUnordered, input, scratch,
                      ool->entry());
  }
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitIsPackedArray(LIsPackedArray* lir) {
  Register obj = ToRegister(lir->object());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());

  masm.setIsPackedArray(obj, output, temp);
}

void CodeGenerator::visitGuardArrayIsPacked(LGuardArrayIsPacked* lir) {
  Register array = ToRegister(lir->array());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());

  Label bail;
  masm.branchArrayIsNotPacked(array, temp0, temp1, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGetPrototypeOf(LGetPrototypeOf* lir) {
  Register target = ToRegister(lir->target());
  ValueOperand out = ToOutValue(lir);
  Register scratch = out.scratchReg();

  using Fn = bool (*)(JSContext*, HandleObject, MutableHandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, jit::GetPrototypeOf>(lir, ArgList(target),
                                                          StoreValueTo(out));

  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  masm.loadObjProto(target, scratch);

  Label hasProto;
  masm.branchPtr(Assembler::Above, scratch, ImmWord(1), &hasProto);

  // Call into the VM for lazy prototypes.
  masm.branchPtr(Assembler::Equal, scratch, ImmWord(1), ool->entry());

  masm.moveValue(NullValue(), out);
  masm.jump(ool->rejoin());

  masm.bind(&hasProto);
  masm.tagValue(JSVAL_TYPE_OBJECT, scratch, out);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitObjectWithProto(LObjectWithProto* lir) {
  pushArg(ToValue(lir->prototype()));

  using Fn = PlainObject* (*)(JSContext*, HandleValue);
  callVM<Fn, js::ObjectWithProtoOperation>(lir);
}

void CodeGenerator::visitObjectStaticProto(LObjectStaticProto* lir) {
  Register obj = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  masm.loadObjProto(obj, output);

#ifdef DEBUG
  // We shouldn't encounter a null or lazy proto.
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  Label done;
  masm.branchPtr(Assembler::Above, output, ImmWord(1), &done);
  masm.assumeUnreachable("Unexpected null or lazy proto in MObjectStaticProto");
  masm.bind(&done);
#endif
}

void CodeGenerator::visitBuiltinObject(LBuiltinObject* lir) {
  pushArg(Imm32(static_cast<int32_t>(lir->mir()->builtinObjectKind())));

  using Fn = JSObject* (*)(JSContext*, BuiltinObjectKind);
  callVM<Fn, js::BuiltinObjectOperation>(lir);
}

void CodeGenerator::visitSuperFunction(LSuperFunction* lir) {
  Register callee = ToRegister(lir->callee());
  ValueOperand out = ToOutValue(lir);
  Register temp = ToRegister(lir->temp0());

#ifdef DEBUG
  Label classCheckDone;
  masm.branchTestObjIsFunction(Assembler::Equal, callee, temp, callee,
                               &classCheckDone);
  masm.assumeUnreachable("Unexpected non-JSFunction callee in JSOp::SuperFun");
  masm.bind(&classCheckDone);
#endif

  // Load prototype of callee
  masm.loadObjProto(callee, temp);

#ifdef DEBUG
  // We won't encounter a lazy proto, because |callee| is guaranteed to be a
  // JSFunction and only proxy objects can have a lazy proto.
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  Label proxyCheckDone;
  masm.branchPtr(Assembler::NotEqual, temp, ImmWord(1), &proxyCheckDone);
  masm.assumeUnreachable("Unexpected lazy proto in JSOp::SuperFun");
  masm.bind(&proxyCheckDone);
#endif

  Label nullProto, done;
  masm.branchPtr(Assembler::Equal, temp, ImmWord(0), &nullProto);

  // Box prototype and return
  masm.tagValue(JSVAL_TYPE_OBJECT, temp, out);
  masm.jump(&done);

  masm.bind(&nullProto);
  masm.moveValue(NullValue(), out);

  masm.bind(&done);
}

void CodeGenerator::visitInitHomeObject(LInitHomeObject* lir) {
  Register func = ToRegister(lir->function());
  ValueOperand homeObject = ToValue(lir->homeObject());

  masm.assertFunctionIsExtended(func);

  Address addr(func, FunctionExtended::offsetOfMethodHomeObjectSlot());

  emitPreBarrier(addr);
  masm.storeValue(homeObject, addr);
}

void CodeGenerator::visitIsTypedArrayConstructor(
    LIsTypedArrayConstructor* lir) {
  Register object = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  masm.setIsDefinitelyTypedArrayConstructor(object, output);
}

void CodeGenerator::visitLoadValueTag(LLoadValueTag* lir) {
  ValueOperand value = ToValue(lir->value());
  Register output = ToRegister(lir->output());

  Register tag = masm.extractTag(value, output);
  if (tag != output) {
    masm.mov(tag, output);
  }
}

void CodeGenerator::visitGuardTagNotEqual(LGuardTagNotEqual* lir) {
  Register lhs = ToRegister(lir->lhs());
  Register rhs = ToRegister(lir->rhs());

  bailoutCmp32(Assembler::Equal, lhs, rhs, lir->snapshot());

  // If both lhs and rhs are numbers, can't use tag comparison to do inequality
  // comparison
  Label done;
  masm.branchTestNumber(Assembler::NotEqual, lhs, &done);
  masm.branchTestNumber(Assembler::NotEqual, rhs, &done);
  bailout(lir->snapshot());

  masm.bind(&done);
}

void CodeGenerator::visitLoadWrapperTarget(LLoadWrapperTarget* lir) {
  Register object = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  masm.loadPtr(Address(object, ProxyObject::offsetOfReservedSlots()), output);

  // Bail for revoked proxies.
  Label bail;
  Address targetAddr(output,
                     js::detail::ProxyReservedSlots::offsetOfPrivateSlot());
  if (lir->mir()->fallible()) {
    masm.fallibleUnboxObject(targetAddr, output, &bail);
    bailoutFrom(&bail, lir->snapshot());
  } else {
    masm.unboxObject(targetAddr, output);
  }
}

void CodeGenerator::visitGuardHasGetterSetter(LGuardHasGetterSetter* lir) {
  Register object = ToRegister(lir->object());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());

  masm.movePropertyKey(lir->mir()->propId(), temp1);
  masm.movePtr(ImmGCPtr(lir->mir()->getterSetter()), temp2);

  using Fn = bool (*)(JSContext* cx, JSObject* obj, jsid id,
                      GetterSetter* getterSetter);
  masm.setupAlignedABICall();
  masm.loadJSContext(temp0);
  masm.passABIArg(temp0);
  masm.passABIArg(object);
  masm.passABIArg(temp1);
  masm.passABIArg(temp2);
  masm.callWithABI<Fn, ObjectHasGetterSetterPure>();

  bailoutIfFalseBool(ReturnReg, lir->snapshot());
}

void CodeGenerator::visitGuardIsExtensible(LGuardIsExtensible* lir) {
  Register object = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp0());

  Label bail;
  masm.branchIfObjectNotExtensible(object, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardInt32IsNonNegative(
    LGuardInt32IsNonNegative* lir) {
  Register index = ToRegister(lir->index());

  bailoutCmp32(Assembler::LessThan, index, Imm32(0), lir->snapshot());
}

void CodeGenerator::visitGuardInt32Range(LGuardInt32Range* lir) {
  Register input = ToRegister(lir->input());

  bailoutCmp32(Assembler::LessThan, input, Imm32(lir->mir()->minimum()),
               lir->snapshot());
  bailoutCmp32(Assembler::GreaterThan, input, Imm32(lir->mir()->maximum()),
               lir->snapshot());
}

void CodeGenerator::visitGuardIndexIsNotDenseElement(
    LGuardIndexIsNotDenseElement* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());
  Register temp = ToRegister(lir->temp0());
  Register spectreTemp = ToTempRegisterOrInvalid(lir->temp1());

  // Load obj->elements.
  masm.loadPtr(Address(object, NativeObject::offsetOfElements()), temp);

  // Ensure index >= initLength or the element is a hole.
  Label notDense;
  Address capacity(temp, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, capacity, spectreTemp, &notDense);

  BaseValueIndex element(temp, index);
  masm.branchTestMagic(Assembler::Equal, element, &notDense);

  bailout(lir->snapshot());

  masm.bind(&notDense);
}

void CodeGenerator::visitGuardIndexIsValidUpdateOrAdd(
    LGuardIndexIsValidUpdateOrAdd* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());
  Register temp = ToRegister(lir->temp0());
  Register spectreTemp = ToTempRegisterOrInvalid(lir->temp1());

  // Load obj->elements.
  masm.loadPtr(Address(object, NativeObject::offsetOfElements()), temp);

  Label success;

  // If length is writable, branch to &success.  All indices are writable.
  Address flags(temp, ObjectElements::offsetOfFlags());
  masm.branchTest32(Assembler::Zero, flags,
                    Imm32(ObjectElements::Flags::NONWRITABLE_ARRAY_LENGTH),
                    &success);

  // Otherwise, ensure index is in bounds.
  Label bail;
  Address length(temp, ObjectElements::offsetOfLength());
  masm.spectreBoundsCheck32(index, length, spectreTemp, &bail);
  masm.bind(&success);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitCallAddOrUpdateSparseElement(
    LCallAddOrUpdateSparseElement* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());
  ValueOperand value = ToValue(lir->value());

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(value);
  pushArg(index);
  pushArg(object);

  using Fn =
      bool (*)(JSContext*, Handle<NativeObject*>, int32_t, HandleValue, bool);
  callVM<Fn, js::AddOrUpdateSparseElementHelper>(lir);
}

void CodeGenerator::visitCallGetSparseElement(LCallGetSparseElement* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());

  pushArg(index);
  pushArg(object);

  using Fn =
      bool (*)(JSContext*, Handle<NativeObject*>, int32_t, MutableHandleValue);
  callVM<Fn, js::GetSparseElementHelper>(lir);
}

void CodeGenerator::visitCallNativeGetElement(LCallNativeGetElement* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());

  pushArg(index);
  pushArg(TypedOrValueRegister(MIRType::Object, AnyRegister(object)));
  pushArg(object);

  using Fn = bool (*)(JSContext*, Handle<NativeObject*>, HandleValue, int32_t,
                      MutableHandleValue);
  callVM<Fn, js::NativeGetElement>(lir);
}

void CodeGenerator::visitCallNativeGetElementSuper(
    LCallNativeGetElementSuper* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());
  ValueOperand receiver = ToValue(lir->receiver());

  pushArg(index);
  pushArg(receiver);
  pushArg(object);

  using Fn = bool (*)(JSContext*, Handle<NativeObject*>, HandleValue, int32_t,
                      MutableHandleValue);
  callVM<Fn, js::NativeGetElement>(lir);
}

void CodeGenerator::visitCallObjectHasSparseElement(
    LCallObjectHasSparseElement* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());
  Register temp0 = ToRegister(lir->temp0());
  Register temp1 = ToRegister(lir->temp1());
  Register output = ToRegister(lir->output());

  masm.reserveStack(sizeof(Value));
  masm.moveStackPtrTo(temp1);

  using Fn = bool (*)(JSContext*, NativeObject*, int32_t, Value*);
  masm.setupAlignedABICall();
  masm.loadJSContext(temp0);
  masm.passABIArg(temp0);
  masm.passABIArg(object);
  masm.passABIArg(index);
  masm.passABIArg(temp1);
  masm.callWithABI<Fn, HasNativeElementPure>();
  masm.storeCallPointerResult(temp0);

  Label bail, ok;
  uint32_t framePushed = masm.framePushed();
  masm.branchIfTrueBool(temp0, &ok);
  masm.adjustStack(sizeof(Value));
  masm.jump(&bail);

  masm.bind(&ok);
  masm.setFramePushed(framePushed);
  masm.unboxBoolean(Address(masm.getStackPointer(), 0), output);
  masm.adjustStack(sizeof(Value));

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitBigIntAsIntN(LBigIntAsIntN* ins) {
  Register bits = ToRegister(ins->bits());
  Register input = ToRegister(ins->input());

  pushArg(bits);
  pushArg(input);

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, int32_t);
  callVM<Fn, jit::BigIntAsIntN>(ins);
}

void CodeGenerator::visitBigIntAsUintN(LBigIntAsUintN* ins) {
  Register bits = ToRegister(ins->bits());
  Register input = ToRegister(ins->input());

  pushArg(bits);
  pushArg(input);

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, int32_t);
  callVM<Fn, jit::BigIntAsUintN>(ins);
}

void CodeGenerator::visitGuardNonGCThing(LGuardNonGCThing* ins) {
  ValueOperand input = ToValue(ins->input());

  Label bail;
  masm.branchTestGCThing(Assembler::Equal, input, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitToHashableNonGCThing(LToHashableNonGCThing* ins) {
  ValueOperand input = ToValue(ins->input());
  FloatRegister tempFloat = ToFloatRegister(ins->temp0());
  ValueOperand output = ToOutValue(ins);

  masm.toHashableNonGCThing(input, output, tempFloat);
}

void CodeGenerator::visitToHashableString(LToHashableString* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  using Fn = JSAtom* (*)(JSContext*, JSString*);
  auto* ool = oolCallVM<Fn, js::AtomizeString>(ins, ArgList(input),
                                               StoreRegisterTo(output));

  Label isAtom;
  masm.branchTest32(Assembler::NonZero,
                    Address(input, JSString::offsetOfFlags()),
                    Imm32(JSString::ATOM_BIT), &isAtom);

  masm.tryFastAtomize(input, output, output, ool->entry());
  masm.jump(ool->rejoin());
  masm.bind(&isAtom);
  masm.movePtr(input, output);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitToHashableValue(LToHashableValue* ins) {
  ValueOperand input = ToValue(ins->input());
  FloatRegister tempFloat = ToFloatRegister(ins->temp0());
  ValueOperand output = ToOutValue(ins);

  Register str = output.scratchReg();

  using Fn = JSAtom* (*)(JSContext*, JSString*);
  auto* ool =
      oolCallVM<Fn, js::AtomizeString>(ins, ArgList(str), StoreRegisterTo(str));

  masm.toHashableValue(input, output, tempFloat, ool->entry(), ool->rejoin());
}

void CodeGenerator::visitHashNonGCThing(LHashNonGCThing* ins) {
  ValueOperand input = ToValue(ins->input());
  Register temp = ToRegister(ins->temp0());
  Register output = ToRegister(ins->output());

  masm.prepareHashNonGCThing(input, output, temp);
}

void CodeGenerator::visitHashString(LHashString* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp0());
  Register output = ToRegister(ins->output());

  masm.prepareHashString(input, output, temp);
}

void CodeGenerator::visitHashSymbol(LHashSymbol* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.prepareHashSymbol(input, output);
}

void CodeGenerator::visitHashBigInt(LHashBigInt* ins) {
  Register input = ToRegister(ins->input());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  masm.prepareHashBigInt(input, output, temp0, temp1, temp2);
}

void CodeGenerator::visitHashObject(LHashObject* ins) {
  Register setObj = ToRegister(ins->setObject());
  ValueOperand input = ToValue(ins->input());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  Register output = ToRegister(ins->output());

  masm.prepareHashObject(setObj, input, output, temp0, temp1, temp2, temp3);
}

void CodeGenerator::visitHashValue(LHashValue* ins) {
  Register setObj = ToRegister(ins->setObject());
  ValueOperand input = ToValue(ins->input());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  Register output = ToRegister(ins->output());

  masm.prepareHashValue(setObj, input, output, temp0, temp1, temp2, temp3);
}

void CodeGenerator::visitSetObjectHasNonBigInt(LSetObjectHasNonBigInt* ins) {
  Register setObj = ToRegister(ins->setObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register output = ToRegister(ins->output());

  masm.setObjectHasNonBigInt(setObj, input, hash, output, temp0, temp1);
}

void CodeGenerator::visitSetObjectHasBigInt(LSetObjectHasBigInt* ins) {
  Register setObj = ToRegister(ins->setObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  Register output = ToRegister(ins->output());

  masm.setObjectHasBigInt(setObj, input, hash, output, temp0, temp1, temp2,
                          temp3);
}

void CodeGenerator::visitSetObjectHasValue(LSetObjectHasValue* ins) {
  Register setObj = ToRegister(ins->setObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  Register output = ToRegister(ins->output());

  masm.setObjectHasValue(setObj, input, hash, output, temp0, temp1, temp2,
                         temp3);
}

void CodeGenerator::visitSetObjectHasValueVMCall(
    LSetObjectHasValueVMCall* ins) {
  pushArg(ToValue(ins->value()));
  pushArg(ToRegister(ins->setObject()));

  using Fn = bool (*)(JSContext*, Handle<SetObject*>, HandleValue, bool*);
  callVM<Fn, jit::SetObjectHas>(ins);
}

void CodeGenerator::visitSetObjectDelete(LSetObjectDelete* ins) {
  pushArg(ToValue(ins->key()));
  pushArg(ToRegister(ins->setObject()));
  using Fn = bool (*)(JSContext*, Handle<SetObject*>, HandleValue, bool*);
  callVM<Fn, jit::SetObjectDelete>(ins);
}

void CodeGenerator::visitSetObjectAdd(LSetObjectAdd* ins) {
  pushArg(ToValue(ins->key()));
  pushArg(ToRegister(ins->setObject()));
  using Fn = bool (*)(JSContext*, Handle<SetObject*>, HandleValue);
  callVM<Fn, jit::SetObjectAdd>(ins);
}

void CodeGenerator::visitSetObjectSize(LSetObjectSize* ins) {
  Register setObj = ToRegister(ins->setObject());
  Register output = ToRegister(ins->output());

  masm.loadSetObjectSize(setObj, output);
}

void CodeGenerator::visitMapObjectHasNonBigInt(LMapObjectHasNonBigInt* ins) {
  Register mapObj = ToRegister(ins->mapObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register output = ToRegister(ins->output());

  masm.mapObjectHasNonBigInt(mapObj, input, hash, output, temp0, temp1);
}

void CodeGenerator::visitMapObjectHasBigInt(LMapObjectHasBigInt* ins) {
  Register mapObj = ToRegister(ins->mapObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  Register output = ToRegister(ins->output());

  masm.mapObjectHasBigInt(mapObj, input, hash, output, temp0, temp1, temp2,
                          temp3);
}

void CodeGenerator::visitMapObjectHasValue(LMapObjectHasValue* ins) {
  Register mapObj = ToRegister(ins->mapObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  Register output = ToRegister(ins->output());

  masm.mapObjectHasValue(mapObj, input, hash, output, temp0, temp1, temp2,
                         temp3);
}

void CodeGenerator::visitMapObjectHasValueVMCall(
    LMapObjectHasValueVMCall* ins) {
  pushArg(ToValue(ins->value()));
  pushArg(ToRegister(ins->mapObject()));

  using Fn = bool (*)(JSContext*, Handle<MapObject*>, HandleValue, bool*);
  callVM<Fn, jit::MapObjectHas>(ins);
}

void CodeGenerator::visitMapObjectGetNonBigInt(LMapObjectGetNonBigInt* ins) {
  Register mapObj = ToRegister(ins->mapObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  ValueOperand output = ToOutValue(ins);

  masm.mapObjectGetNonBigInt(mapObj, input, hash, output, temp0, temp1,
                             output.scratchReg());
}

void CodeGenerator::visitMapObjectGetBigInt(LMapObjectGetBigInt* ins) {
  Register mapObj = ToRegister(ins->mapObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  ValueOperand output = ToOutValue(ins);

  masm.mapObjectGetBigInt(mapObj, input, hash, output, temp0, temp1, temp2,
                          temp3, output.scratchReg());
}

void CodeGenerator::visitMapObjectGetValue(LMapObjectGetValue* ins) {
  Register mapObj = ToRegister(ins->mapObject());
  ValueOperand input = ToValue(ins->value());
  Register hash = ToRegister(ins->hash());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  ValueOperand output = ToOutValue(ins);

  masm.mapObjectGetValue(mapObj, input, hash, output, temp0, temp1, temp2,
                         temp3, output.scratchReg());
}

void CodeGenerator::visitMapObjectGetValueVMCall(
    LMapObjectGetValueVMCall* ins) {
  pushArg(ToValue(ins->value()));
  pushArg(ToRegister(ins->mapObject()));

  using Fn =
      bool (*)(JSContext*, Handle<MapObject*>, HandleValue, MutableHandleValue);
  callVM<Fn, jit::MapObjectGet>(ins);
}

void CodeGenerator::visitMapObjectDelete(LMapObjectDelete* ins) {
  pushArg(ToValue(ins->key()));
  pushArg(ToRegister(ins->mapObject()));
  using Fn = bool (*)(JSContext*, Handle<MapObject*>, HandleValue, bool*);
  callVM<Fn, jit::MapObjectDelete>(ins);
}

void CodeGenerator::visitMapObjectSet(LMapObjectSet* ins) {
  pushArg(ToValue(ins->value()));
  pushArg(ToValue(ins->key()));
  pushArg(ToRegister(ins->mapObject()));
  using Fn = bool (*)(JSContext*, Handle<MapObject*>, HandleValue, HandleValue);
  callVM<Fn, jit::MapObjectSet>(ins);
}

void CodeGenerator::visitMapObjectSize(LMapObjectSize* ins) {
  Register mapObj = ToRegister(ins->mapObject());
  Register output = ToRegister(ins->output());

  masm.loadMapObjectSize(mapObj, output);
}

void CodeGenerator::visitDateFillLocalTimeSlots(LDateFillLocalTimeSlots* ins) {
  Register date = ToRegister(ins->date());
  Register temp = ToRegister(ins->temp0());

  masm.dateFillLocalTimeSlots(date, temp, liveVolatileRegs(ins));
}

void CodeGenerator::visitDateHoursFromSecondsIntoYear(
    LDateHoursFromSecondsIntoYear* ins) {
  auto secondsIntoYear = ToValue(ins->secondsIntoYear());
  auto output = ToOutValue(ins);
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());

  masm.dateHoursFromSecondsIntoYear(secondsIntoYear, output, temp0, temp1);
}

void CodeGenerator::visitDateMinutesFromSecondsIntoYear(
    LDateMinutesFromSecondsIntoYear* ins) {
  auto secondsIntoYear = ToValue(ins->secondsIntoYear());
  auto output = ToOutValue(ins);
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());

  masm.dateMinutesFromSecondsIntoYear(secondsIntoYear, output, temp0, temp1);
}

void CodeGenerator::visitDateSecondsFromSecondsIntoYear(
    LDateSecondsFromSecondsIntoYear* ins) {
  auto secondsIntoYear = ToValue(ins->secondsIntoYear());
  auto output = ToOutValue(ins);
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());

  masm.dateSecondsFromSecondsIntoYear(secondsIntoYear, output, temp0, temp1);
}

void CodeGenerator::visitCanonicalizeNaND(LCanonicalizeNaND* ins) {
  auto output = ToFloatRegister(ins->output());
  MOZ_ASSERT(output == ToFloatRegister(ins->input()));

  masm.canonicalizeDouble(output);
}

void CodeGenerator::visitCanonicalizeNaNF(LCanonicalizeNaNF* ins) {
  auto output = ToFloatRegister(ins->output());
  MOZ_ASSERT(output == ToFloatRegister(ins->input()));

  masm.canonicalizeFloat(output);
}

template <size_t NumDefs>
void CodeGenerator::emitIonToWasmCallBase(LIonToWasmCallBase<NumDefs>* lir) {
  wasm::JitCallStackArgVector stackArgs;
  masm.propagateOOM(stackArgs.reserve(lir->numOperands()));
  if (masm.oom()) {
    return;
  }

  MIonToWasmCall* mir = lir->mir();
  const wasm::FuncExport& funcExport = mir->funcExport();
  const wasm::FuncType& sig =
      mir->instance()->code().codeMeta().getFuncType(funcExport.funcIndex());

  WasmABIArgGenerator abi;
  for (size_t i = 0; i < lir->numOperands(); i++) {
    MIRType argMir;
    switch (sig.args()[i].kind()) {
      case wasm::ValType::I32:
      case wasm::ValType::I64:
      case wasm::ValType::F32:
      case wasm::ValType::F64:
        argMir = sig.args()[i].toMIRType();
        break;
      case wasm::ValType::V128:
        MOZ_CRASH("unexpected argument type when calling from ion to wasm");
      case wasm::ValType::Ref:
        // temporarilyUnsupportedReftypeForEntry() restricts args to externref
        MOZ_RELEASE_ASSERT(sig.args()[i].refType().isExtern());
        // Argument is boxed on the JS side to an anyref, so passed as a
        // pointer here.
        argMir = sig.args()[i].toMIRType();
        break;
    }

    ABIArg arg = abi.next(argMir);
    switch (arg.kind()) {
      case ABIArg::GPR:
      case ABIArg::FPU: {
        MOZ_ASSERT(ToAnyRegister(lir->getOperand(i)) == arg.reg());
        stackArgs.infallibleEmplaceBack(wasm::JitCallStackArg());
        break;
      }
      case ABIArg::Stack: {
        const LAllocation* larg = lir->getOperand(i);
        if (larg->isConstant()) {
          stackArgs.infallibleEmplaceBack(ToInt32(larg));
        } else if (larg->isGeneralReg()) {
          stackArgs.infallibleEmplaceBack(ToRegister(larg));
        } else if (larg->isFloatReg()) {
          stackArgs.infallibleEmplaceBack(ToFloatRegister(larg));
        } else {
          // Always use the stack pointer here because GenerateDirectCallFromJit
          // depends on this.
          Address addr = ToAddress<BaseRegForAddress::SP>(larg);
          stackArgs.infallibleEmplaceBack(addr);
        }
        break;
      }
#ifdef JS_CODEGEN_REGISTER_PAIR
      case ABIArg::GPR_PAIR: {
        MOZ_CRASH(
            "no way to pass i64, and wasm uses hardfp for function calls");
      }
#endif
      case ABIArg::Uninitialized: {
        MOZ_CRASH("Uninitialized ABIArg kind");
      }
    }
  }

  const wasm::ValTypeVector& results = sig.results();
  if (results.length() == 0) {
    MOZ_ASSERT(lir->mir()->type() == MIRType::Value);
  } else {
    MOZ_ASSERT(results.length() == 1, "multi-value return unimplemented");
    switch (results[0].kind()) {
      case wasm::ValType::I32:
        MOZ_ASSERT(lir->mir()->type() == MIRType::Int32);
        MOZ_ASSERT(ToRegister(lir->output()) == ReturnReg);
        break;
      case wasm::ValType::I64:
        MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);
        MOZ_ASSERT(ToOutRegister64(lir) == ReturnReg64);
        break;
      case wasm::ValType::F32:
        MOZ_ASSERT(lir->mir()->type() == MIRType::Float32);
        MOZ_ASSERT(ToFloatRegister(lir->output()) == ReturnFloat32Reg);
        break;
      case wasm::ValType::F64:
        MOZ_ASSERT(lir->mir()->type() == MIRType::Double);
        MOZ_ASSERT(ToFloatRegister(lir->output()) == ReturnDoubleReg);
        break;
      case wasm::ValType::V128:
        MOZ_CRASH("unexpected return type when calling from ion to wasm");
      case wasm::ValType::Ref:
        // The wasm stubs layer unboxes anything that needs to be unboxed
        // and leaves it in a Value.  A FuncRef/EqRef we could in principle
        // leave it as a raw object pointer but for now it complicates the
        // API to do so.
        MOZ_ASSERT(lir->mir()->type() == MIRType::Value);
        break;
    }
  }

  WasmInstanceObject* instObj = lir->mir()->instanceObject();

  Register scratch = ToRegister(lir->temp());

  uint32_t callOffset;
  ensureOsiSpace();
  GenerateDirectCallFromJit(masm, funcExport, instObj->instance(), stackArgs,
                            scratch, &callOffset);

  // Add the instance object to the constant pool, so it is transferred to
  // the owning IonScript and so that it gets traced as long as the IonScript
  // lives.

  uint32_t unused;
  masm.propagateOOM(graph.addConstantToPool(ObjectValue(*instObj), &unused));

  markSafepointAt(callOffset, lir);
}

void CodeGenerator::visitIonToWasmCall(LIonToWasmCall* lir) {
  emitIonToWasmCallBase(lir);
}
void CodeGenerator::visitIonToWasmCallV(LIonToWasmCallV* lir) {
  emitIonToWasmCallBase(lir);
}
void CodeGenerator::visitIonToWasmCallI64(LIonToWasmCallI64* lir) {
  emitIonToWasmCallBase(lir);
}

void CodeGenerator::visitWasmNullConstant(LWasmNullConstant* lir) {
  masm.xorPtr(ToRegister(lir->output()), ToRegister(lir->output()));
}

void CodeGenerator::visitWasmFence(LWasmFence* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  masm.memoryBarrier(MemoryBarrier::Full());
}

void CodeGenerator::visitWasmAnyRefFromJSValue(LWasmAnyRefFromJSValue* lir) {
  ValueOperand input = ToValue(lir->def());
  Register output = ToRegister(lir->output());
  FloatRegister tempFloat = ToFloatRegister(lir->temp0());

  using Fn = JSObject* (*)(JSContext* cx, HandleValue value);
  OutOfLineCode* oolBoxValue = oolCallVM<Fn, wasm::AnyRef::boxValue>(
      lir, ArgList(input), StoreRegisterTo(output));
  masm.convertValueToWasmAnyRef(input, output, tempFloat, oolBoxValue->entry());
  masm.bind(oolBoxValue->rejoin());
}

void CodeGenerator::visitWasmAnyRefFromJSObject(LWasmAnyRefFromJSObject* lir) {
  Register input = ToRegister(lir->def());
  Register output = ToRegister(lir->output());
  masm.convertObjectToWasmAnyRef(input, output);
}

void CodeGenerator::visitWasmAnyRefFromJSString(LWasmAnyRefFromJSString* lir) {
  Register input = ToRegister(lir->def());
  Register output = ToRegister(lir->output());
  masm.convertStringToWasmAnyRef(input, output);
}

void CodeGenerator::visitWasmAnyRefIsJSString(LWasmAnyRefIsJSString* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());
  Label fallthrough;
  Label isJSString;
  masm.branchWasmAnyRefIsJSString(true, input, temp, &isJSString);
  masm.move32(Imm32(0), output);
  masm.jump(&fallthrough);
  masm.bind(&isJSString);
  masm.move32(Imm32(1), output);
  masm.bind(&fallthrough);
}

void CodeGenerator::visitWasmTrapIfAnyRefIsNotJSString(
    LWasmTrapIfAnyRefIsNotJSString* lir) {
  Register input = ToRegister(lir->input());
  Register temp = ToRegister(lir->temp0());
  Label isJSString;
  masm.branchWasmAnyRefIsJSString(true, input, temp, &isJSString);
  masm.wasmTrap(lir->mir()->trap(), lir->mir()->trapSiteDesc());
  masm.bind(&isJSString);
}

void CodeGenerator::visitWasmAnyRefJSStringLength(
    LWasmAnyRefJSStringLength* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());
  Label isJSString;
  masm.branchWasmAnyRefIsJSString(true, input, temp, &isJSString);
  masm.wasmTrap(lir->mir()->trap(), lir->mir()->trapSiteDesc());
  masm.bind(&isJSString);
  masm.untagWasmAnyRef(input, temp, wasm::AnyRefTag::String);
  masm.loadStringLength(temp, output);
}

void CodeGenerator::visitWasmNewI31Ref(LWasmNewI31Ref* lir) {
  if (lir->value()->isConstant()) {
    // i31ref are often created with constants. If that's the case we will
    // do the operation statically here. This is similar to what is done
    // in masm.truncate32ToWasmI31Ref.
    Register output = ToRegister(lir->output());
    uint32_t value =
        static_cast<uint32_t>(lir->value()->toConstant()->toInt32());
    uintptr_t ptr = wasm::AnyRef::fromUint32Truncate(value).rawValue();
    masm.movePtr(ImmWord(ptr), output);
  } else {
    Register value = ToRegister(lir->value());
    Register output = ToRegister(lir->output());
    masm.truncate32ToWasmI31Ref(value, output);
  }
}

void CodeGenerator::visitWasmI31RefGet(LWasmI31RefGet* lir) {
  Register value = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  if (lir->mir()->wideningOp() == wasm::FieldWideningOp::Signed) {
    masm.convertWasmI31RefTo32Signed(value, output);
  } else {
    masm.convertWasmI31RefTo32Unsigned(value, output);
  }
}

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
void CodeGenerator::visitAddDisposableResource(LAddDisposableResource* lir) {
  Register environment = ToRegister(lir->environment());
  ValueOperand resource = ToValue(lir->resource());
  ValueOperand method = ToValue(lir->method());
  Register needsClosure = ToRegister(lir->needsClosure());
  uint8_t hint = lir->hint();

  pushArg(Imm32(hint));
  pushArg(needsClosure);
  pushArg(method);
  pushArg(resource);
  pushArg(environment);

  using Fn = bool (*)(JSContext*, JS::Handle<JSObject*>, JS::Handle<JS::Value>,
                      JS::Handle<JS::Value>, bool, UsingHint);
  callVM<Fn, js::AddDisposableResourceToCapability>(lir);
}

void CodeGenerator::visitTakeDisposeCapability(LTakeDisposeCapability* lir) {
  Register environment = ToRegister(lir->environment());
  ValueOperand output = ToOutValue(lir);

  Address capabilityAddr(
      environment, DisposableEnvironmentObject::offsetOfDisposeCapability());
  emitPreBarrier(capabilityAddr);
  masm.loadValue(capabilityAddr, output);
  masm.storeValue(JS::UndefinedValue(), capabilityAddr);
}

void CodeGenerator::visitCreateSuppressedError(LCreateSuppressedError* lir) {
  ValueOperand error = ToValue(lir->error());
  ValueOperand suppressed = ToValue(lir->suppressed());

  pushArg(suppressed);
  pushArg(error);

  using Fn = ErrorObject* (*)(JSContext*, JS::Handle<JS::Value>,
                              JS::Handle<JS::Value>);
  callVM<Fn, js::CreateSuppressedError>(lir);
}
#endif

#ifdef FUZZING_JS_FUZZILLI
void CodeGenerator::emitFuzzilliHashObject(LInstruction* lir, Register obj,
                                           Register output) {
  using Fn = void (*)(JSContext* cx, JSObject* obj, uint32_t* out);
  OutOfLineCode* ool = oolCallVM<Fn, FuzzilliHashObjectInl>(
      lir, ArgList(obj), StoreRegisterTo(output));

  masm.jump(ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::emitFuzzilliHashBigInt(LInstruction* lir, Register bigInt,
                                           Register output) {
  LiveRegisterSet volatileRegs = liveVolatileRegs(lir);
  volatileRegs.takeUnchecked(output);

  masm.PushRegsInMask(volatileRegs);

  using Fn = uint32_t (*)(BigInt* bigInt);
  masm.setupUnalignedABICall(output);
  masm.passABIArg(bigInt);
  masm.callWithABI<Fn, js::FuzzilliHashBigInt>();
  masm.storeCallInt32Result(output);

  masm.PopRegsInMask(volatileRegs);
}

void CodeGenerator::visitFuzzilliHashV(LFuzzilliHashV* ins) {
  ValueOperand value = ToValue(ins->value());

  FloatRegister scratchFloat = ToFloatRegister(ins->temp1());
  Register scratch = ToRegister(ins->temp0());
  Register output = ToRegister(ins->output());
  MOZ_ASSERT(scratch != output);

  Label hashDouble, done;

  Label isInt32, isDouble, isNull, isUndefined, isBoolean, isBigInt, isObject;
  {
    ScratchTagScope tag(masm, value);
    masm.splitTagForTest(value, tag);

    masm.branchTestInt32(Assembler::Equal, tag, &isInt32);
    masm.branchTestDouble(Assembler::Equal, tag, &isDouble);
    masm.branchTestNull(Assembler::Equal, tag, &isNull);
    masm.branchTestUndefined(Assembler::Equal, tag, &isUndefined);
    masm.branchTestBoolean(Assembler::Equal, tag, &isBoolean);
    masm.branchTestBigInt(Assembler::Equal, tag, &isBigInt);
    masm.branchTestObject(Assembler::Equal, tag, &isObject);

    // Symbol or String.
    masm.move32(Imm32(0), output);
    masm.jump(&done);
  }

  masm.bind(&isInt32);
  {
    masm.unboxInt32(value, scratch);
    masm.convertInt32ToDouble(scratch, scratchFloat);
    masm.jump(&hashDouble);
  }

  masm.bind(&isDouble);
  {
    masm.unboxDouble(value, scratchFloat);
    masm.jump(&hashDouble);
  }

  masm.bind(&isNull);
  {
    masm.loadConstantDouble(1.0, scratchFloat);
    masm.jump(&hashDouble);
  }

  masm.bind(&isUndefined);
  {
    masm.loadConstantDouble(2.0, scratchFloat);
    masm.jump(&hashDouble);
  }

  masm.bind(&isBoolean);
  {
    masm.unboxBoolean(value, scratch);
    masm.add32(Imm32(3), scratch);
    masm.convertInt32ToDouble(scratch, scratchFloat);
    masm.jump(&hashDouble);
  }

  masm.bind(&isBigInt);
  {
    masm.unboxBigInt(value, scratch);
    emitFuzzilliHashBigInt(ins, scratch, output);
    masm.jump(&done);
  }

  masm.bind(&isObject);
  {
    masm.unboxObject(value, scratch);
    emitFuzzilliHashObject(ins, scratch, output);
    masm.jump(&done);
  }

  masm.bind(&hashDouble);
  masm.fuzzilliHashDouble(scratchFloat, output, scratch);

  masm.bind(&done);
}

void CodeGenerator::visitFuzzilliHashT(LFuzzilliHashT* ins) {
  const LAllocation* value = ins->value();
  MIRType mirType = ins->mir()->getOperand(0)->type();

  Register scratch = ToTempRegisterOrInvalid(ins->temp0());
  FloatRegister scratchFloat = ToTempFloatRegisterOrInvalid(ins->temp1());

  Register output = ToRegister(ins->output());
  MOZ_ASSERT(scratch != output);

  switch (mirType) {
    case MIRType::Undefined: {
      masm.loadConstantDouble(2.0, scratchFloat);
      masm.fuzzilliHashDouble(scratchFloat, output, scratch);
      break;
    }

    case MIRType::Null: {
      masm.loadConstantDouble(1.0, scratchFloat);
      masm.fuzzilliHashDouble(scratchFloat, output, scratch);
      break;
    }

    case MIRType::Int32: {
      masm.move32(ToRegister(value), scratch);
      masm.convertInt32ToDouble(scratch, scratchFloat);
      masm.fuzzilliHashDouble(scratchFloat, output, scratch);
      break;
    }

    case MIRType::Double: {
      masm.moveDouble(ToFloatRegister(value), scratchFloat);
      masm.fuzzilliHashDouble(scratchFloat, output, scratch);
      break;
    }

    case MIRType::Float32: {
      masm.convertFloat32ToDouble(ToFloatRegister(value), scratchFloat);
      masm.fuzzilliHashDouble(scratchFloat, output, scratch);
      break;
    }

    case MIRType::Boolean: {
      masm.add32(Imm32(3), ToRegister(value), scratch);
      masm.convertInt32ToDouble(scratch, scratchFloat);
      masm.fuzzilliHashDouble(scratchFloat, output, scratch);
      break;
    }

    case MIRType::BigInt: {
      emitFuzzilliHashBigInt(ins, ToRegister(value), output);
      break;
    }

    case MIRType::Object: {
      emitFuzzilliHashObject(ins, ToRegister(value), output);
      break;
    }

    default:
      MOZ_CRASH("unexpected type");
  }
}

void CodeGenerator::visitFuzzilliHashStore(LFuzzilliHashStore* ins) {
  Register value = ToRegister(ins->value());
  Register temp0 = ToRegister(ins->temp0());
  Register temp1 = ToRegister(ins->temp1());

  masm.fuzzilliStoreHash(value, temp0, temp1);
}
#endif

static_assert(!std::is_polymorphic_v<CodeGenerator>,
              "CodeGenerator should not have any virtual methods");

}  // namespace jit
}  // namespace js
