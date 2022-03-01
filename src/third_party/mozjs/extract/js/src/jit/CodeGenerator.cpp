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
#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Tuple.h"

#include <limits>
#include <type_traits>
#include <utility>

#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"

#include "builtin/Eval.h"
#include "builtin/MapObject.h"
#include "builtin/RegExp.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/String.h"
#include "gc/Nursery.h"
#include "irregexp/RegExpTypes.h"
#include "jit/BaselineCodeGen.h"
#include "jit/CompileInfo.h"
#include "jit/InlineScriptTree.h"
#include "jit/Invalidation.h"
#include "jit/IonIC.h"
#include "jit/IonScript.h"
#include "jit/JitcodeMap.h"
#include "jit/JitFrames.h"
#include "jit/JitRealm.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/Linker.h"
#include "jit/Lowering.h"
#include "jit/MIRGenerator.h"
#include "jit/MoveEmitter.h"
#include "jit/RangeAnalysis.h"
#include "jit/SafepointIndex.h"
#include "jit/SharedICHelpers.h"
#include "jit/StackSlotAllocator.h"
#include "jit/VMFunctions.h"
#include "jit/WarpSnapshot.h"
#include "js/experimental/JitInfo.h"  // JSJit{Getter,Setter}CallArgs, JSJitMethodCallArgsTraits, JSJitInfo
#include "js/friend/DOMProxy.h"  // JS::ExpandoAndGeneration
#include "js/RegExpFlags.h"      // JS::RegExpFlag
#include "js/ScalarType.h"       // js::Scalar::Type
#include "proxy/DOMProxy.h"
#include "util/CheckedArithmetic.h"
#include "util/Unicode.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BuiltinObjectKind.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/MatchPairs.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/RegExpObject.h"
#include "vm/RegExpStatics.h"
#include "vm/StringObject.h"
#include "vm/StringType.h"
#include "vm/TraceLogging.h"
#include "vm/TypedArrayObject.h"
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif
#include "wasm/WasmGC.h"
#include "wasm/WasmStubs.h"

#include "builtin/Boolean-inl.h"
#include "jit/ABIFunctionList-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "jit/TemplateObject-inl.h"
#include "jit/VMFunctionList-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using JS::GenericNaN;
using mozilla::AssertedCast;
using mozilla::DebugOnly;
using mozilla::FloatingPoint;
using mozilla::Maybe;
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
void CodeGenerator::callVMInternal(VMFunctionId id, LInstruction* ins,
                                   const Register* dynStack) {
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

  // Push an exit frame descriptor. If |dynStack| is a valid pointer to a
  // register, then its value is added to the value of the |framePushed()| to
  // fill the frame descriptor.
  if (dynStack) {
    masm.addPtr(Imm32(masm.framePushed()), *dynStack);
    masm.makeFrameDescriptor(*dynStack, FrameType::IonJS,
                             ExitFrameLayout::Size());
    masm.Push(*dynStack);  // descriptor
  } else {
    masm.pushStaticFrameDescriptor(FrameType::IonJS, ExitFrameLayout::Size());
  }

  // Call the wrapper function.  The wrapper is in charge to unwind the stack
  // when returning from the call.  Failures are handled with exceptions based
  // on the return value of the C functions.  To guard the outcome of the
  // returned value, use another LIR instruction.
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

  // Remove rest of the frame left on the stack. We remove the return address
  // which is implicitly poped when returning.
  int framePop = sizeof(ExitFrameLayout) - sizeof(void*);

  // Pop arguments from framePushed.
  masm.implicitPop(fun.explicitStackSlots() * sizeof(void*) + framePop);
  // Stack is:
  //    ... frame ...
}

template <typename Fn, Fn fn>
void CodeGenerator::callVM(LInstruction* ins, const Register* dynStack) {
  VMFunctionId id = VMFunctionToId<Fn, fn>::id;
  callVMInternal(id, ins, dynStack);
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
  mozilla::Tuple<std::remove_reference_t<ArgTypes>...> args_;

  template <std::size_t... ISeq>
  inline void generate(CodeGenerator* codegen,
                       std::index_sequence<ISeq...>) const {
    // Arguments are pushed in reverse order, from last argument to first
    // argument.
    (codegen->pushArg(mozilla::Get<sizeof...(ISeq) - 1 - ISeq>(args_)), ...);
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
                          HandleValue, bool*);
      callVM<Fn, IonOptimizeSpreadCallIC::update>(lir);

      StoreRegisterTo(optimizeSpreadCallIC->output()).generate(this);
      restoreLiveIgnore(
          lir, StoreRegisterTo(optimizeSpreadCallIC->output()).clobbered());

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

      using Fn = bool (*)(JSContext * cx, HandleScript outerScript,
                          IonUnaryArithIC * stub, HandleValue val,
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

      using Fn = bool (*)(JSContext * cx, HandleScript outerScript,
                          IonToPropertyKeyIC * ic, HandleValue val,
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

      using Fn = bool (*)(JSContext * cx, HandleScript outerScript,
                          IonBinaryArithIC * stub, HandleValue lhs,
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

      using Fn = bool (*)(JSContext * cx, HandleScript outerScript,
                          IonCompareIC * stub, HandleValue lhs, HandleValue rhs,
                          bool* res);
      callVM<Fn, IonCompareIC::update>(lir);

      StoreRegisterTo(compareIC->output()).generate(this);
      restoreLiveIgnore(lir, StoreRegisterTo(compareIC->output()).clobbered());

      masm.jump(ool->rejoin());
      return;
    }
    case CacheKind::Call:
    case CacheKind::TypeOf:
    case CacheKind::ToBool:
    case CacheKind::GetIntrinsic:
    case CacheKind::NewArray:
    case CacheKind::NewObject:
      MOZ_CRASH("Unsupported IC");
  }
  MOZ_CRASH();
}

StringObject* MNewStringObject::templateObj() const {
  return &templateObj_->as<StringObject>();
}

CodeGenerator::CodeGenerator(MIRGenerator* gen, LIRGraph* graph,
                             MacroAssembler* masm)
    : CodeGeneratorSpecific(gen, graph, masm),
      ionScriptLabels_(gen->alloc()),
      ionNurseryObjectLabels_(gen->alloc()),
      scriptCounts_(nullptr),
      realmStubsToReadBarrier_(0) {}

CodeGenerator::~CodeGenerator() { js_delete(scriptCounts_); }

class OutOfLineZeroIfNaN : public OutOfLineCodeBase<CodeGenerator> {
  LInstruction* lir_;
  FloatRegister input_;
  Register output_;

 public:
  OutOfLineZeroIfNaN(LInstruction* lir, FloatRegister input, Register output)
      : lir_(lir), input_(input), output_(output) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineZeroIfNaN(this);
  }
  LInstruction* lir() const { return lir_; }
  FloatRegister input() const { return input_; }
  Register output() const { return output_; }
};

void CodeGenerator::visitValueToInt32(LValueToInt32* lir) {
  ValueOperand operand = ToValue(lir, LValueToInt32::Input);
  Register output = ToRegister(lir->output());
  FloatRegister temp = ToFloatRegister(lir->tempFloat());

  Label fails;
  if (lir->mode() == LValueToInt32::TRUNCATE) {
    OutOfLineCode* oolDouble = oolTruncateDouble(temp, output, lir->mir());

    // We can only handle strings in truncation contexts, like bitwise
    // operations.
    Register stringReg = ToRegister(lir->temp());
    using Fn = bool (*)(JSContext*, JSString*, double*);
    auto* oolString = oolCallVM<Fn, StringToNumber>(lir, ArgList(stringReg),
                                                    StoreFloatRegisterTo(temp));
    Label* stringEntry = oolString->entry();
    Label* stringRejoin = oolString->rejoin();

    masm.truncateValueToInt32(operand, stringEntry, stringRejoin,
                              oolDouble->entry(), stringReg, temp, output,
                              &fails);
    masm.bind(oolDouble->rejoin());
  } else if (lir->mode() == LValueToInt32::TRUNCATE_NOWRAP) {
    auto* ool = new (alloc()) OutOfLineZeroIfNaN(lir, temp, output);
    addOutOfLineCode(ool, lir->mir());
    masm.truncateNoWrapValueToInt32(operand, temp, output, ool->entry(),
                                    &fails);
    masm.bind(ool->rejoin());
  } else {
    masm.convertValueToInt32(operand, temp, output, &fails,
                             lir->mirNormal()->needsNegativeZeroCheck(),
                             lir->mirNormal()->conversion());
  }

  bailoutFrom(&fails, lir->snapshot());
}

void CodeGenerator::visitOutOfLineZeroIfNaN(OutOfLineZeroIfNaN* ool) {
  FloatRegister input = ool->input();
  Register output = ool->output();

  // NaN triggers the failure path for branchTruncateDoubleToInt32() on x86,
  // x64, and ARM64, so handle it here. In all other cases bail out.

  Label fails;
  if (input.isSingle()) {
    masm.branchFloat(Assembler::DoubleOrdered, input, input, &fails);
  } else {
    masm.branchDouble(Assembler::DoubleOrdered, input, input, &fails);
  }

  // ToInteger(NaN) is 0.
  masm.move32(Imm32(0), output);
  masm.jump(ool->rejoin());

  bailoutFrom(&fails, ool->lir()->snapshot());
}

void CodeGenerator::visitValueToDouble(LValueToDouble* lir) {
  ValueOperand operand = ToValue(lir, LValueToDouble::Input);
  FloatRegister output = ToFloatRegister(lir->output());

  // Set if we can handle other primitives beside strings, as long as they're
  // guaranteed to never throw. This rules out symbols and BigInts, but allows
  // booleans, undefined, and null.
  bool hasNonStringPrimitives =
      lir->mir()->conversion() == MToFPInstruction::NonStringPrimitives;

  Label isDouble, isInt32, isBool, isNull, isUndefined, done;

  {
    ScratchTagScope tag(masm, operand);
    masm.splitTagForTest(operand, tag);

    masm.branchTestDouble(Assembler::Equal, tag, &isDouble);
    masm.branchTestInt32(Assembler::Equal, tag, &isInt32);

    if (hasNonStringPrimitives) {
      masm.branchTestBoolean(Assembler::Equal, tag, &isBool);
      masm.branchTestUndefined(Assembler::Equal, tag, &isUndefined);
      masm.branchTestNull(Assembler::Equal, tag, &isNull);
    }
  }

  bailout(lir->snapshot());

  if (hasNonStringPrimitives) {
    masm.bind(&isNull);
    masm.loadConstantDouble(0.0, output);
    masm.jump(&done);
  }

  if (hasNonStringPrimitives) {
    masm.bind(&isUndefined);
    masm.loadConstantDouble(GenericNaN(), output);
    masm.jump(&done);
  }

  if (hasNonStringPrimitives) {
    masm.bind(&isBool);
    masm.boolValueToDouble(operand, output);
    masm.jump(&done);
  }

  masm.bind(&isInt32);
  masm.int32ValueToDouble(operand, output);
  masm.jump(&done);

  masm.bind(&isDouble);
  masm.unboxDouble(operand, output);
  masm.bind(&done);
}

void CodeGenerator::visitValueToFloat32(LValueToFloat32* lir) {
  ValueOperand operand = ToValue(lir, LValueToFloat32::Input);
  FloatRegister output = ToFloatRegister(lir->output());

  // Set if we can handle other primitives beside strings, as long as they're
  // guaranteed to never throw. This rules out symbols and BigInts, but allows
  // booleans, undefined, and null.
  bool hasNonStringPrimitives =
      lir->mir()->conversion() == MToFPInstruction::NonStringPrimitives;

  Label isDouble, isInt32, isBool, isNull, isUndefined, done;

  {
    ScratchTagScope tag(masm, operand);
    masm.splitTagForTest(operand, tag);

    masm.branchTestDouble(Assembler::Equal, tag, &isDouble);
    masm.branchTestInt32(Assembler::Equal, tag, &isInt32);

    if (hasNonStringPrimitives) {
      masm.branchTestBoolean(Assembler::Equal, tag, &isBool);
      masm.branchTestUndefined(Assembler::Equal, tag, &isUndefined);
      masm.branchTestNull(Assembler::Equal, tag, &isNull);
    }
  }

  bailout(lir->snapshot());

  if (hasNonStringPrimitives) {
    masm.bind(&isNull);
    masm.loadConstantFloat32(0.0f, output);
    masm.jump(&done);
  }

  if (hasNonStringPrimitives) {
    masm.bind(&isUndefined);
    masm.loadConstantFloat32(float(GenericNaN()), output);
    masm.jump(&done);
  }

  if (hasNonStringPrimitives) {
    masm.bind(&isBool);
    masm.boolValueToFloat32(operand, output);
    masm.jump(&done);
  }

  masm.bind(&isInt32);
  masm.int32ValueToFloat32(operand, output);
  masm.jump(&done);

  masm.bind(&isDouble);
  // ARM and MIPS may not have a double register available if we've
  // allocated output as a float32.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32)
  ScratchDoubleScope fpscratch(masm);
  masm.unboxDouble(operand, fpscratch);
  masm.convertDoubleToFloat32(fpscratch, output);
#else
  masm.unboxDouble(operand, output);
  masm.convertDoubleToFloat32(output, output);
#endif
  masm.bind(&done);
}

void CodeGenerator::visitValueToBigInt(LValueToBigInt* lir) {
  ValueOperand operand = ToValue(lir, LValueToBigInt::Input);
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

void CodeGenerator::visitDoubleToIntegerInt32(LDoubleToIntegerInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  auto* ool = new (alloc()) OutOfLineZeroIfNaN(lir, input, output);
  addOutOfLineCode(ool, lir->mir());

  masm.branchTruncateDoubleToInt32(input, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitFloat32ToIntegerInt32(LFloat32ToIntegerInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  auto* ool = new (alloc()) OutOfLineZeroIfNaN(lir, input, output);
  addOutOfLineCode(ool, lir->mir());

  masm.branchTruncateFloat32ToInt32(input, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitInt32ToIntPtr(LInt32ToIntPtr* lir) {
#ifdef JS_64BIT
  Register output = ToRegister(lir->output());

  // This is a no-op if the input can't be negative. In debug builds assert
  // (1) the upper 32 bits are zero and (2) the value <= INT32_MAX so that sign
  // extension isn't needed.
  if (!lir->mir()->canBeNegative()) {
    MOZ_ASSERT(ToRegister(lir->input()) == output);
#  ifdef DEBUG
    Label ok;
    masm.branchPtr(Assembler::BelowOrEqual, output, ImmWord(INT32_MAX), &ok);
    masm.assumeUnreachable("LInt32ToIntPtr: unexpected range for value");
    masm.bind(&ok);
#  endif
    return;
  }

  const LAllocation* input = lir->input();
  if (input->isRegister()) {
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
  using Fn = bool (*)(JSObject * obj);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(objreg);
  masm.callWithABI<Fn, js::EmulatesUndefined>();
  masm.storeCallBoolResult(scratch);
  restoreVolatile(scratch);

  masm.branchIfTrueBool(scratch, ifEmulatesUndefined);
  masm.jump(ifDoesntEmulateUndefined);
}

// Base out-of-line code generator for all tests of the truthiness of an
// object, where the object might not be truthy.  (Recall that per spec all
// objects are truthy, but we implement the JSCLASS_EMULATES_UNDEFINED class
// flag to permit objects to look like |undefined| in certain contexts,
// including in object truthiness testing.)  We check truthiness inline except
// when we're testing it on a proxy (or if TI guarantees us that the specified
// object will never emulate |undefined|), in which case out-of-line code will
// call EmulatesUndefined for a conclusive answer.
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
    Register scratch1, Register scratch2, FloatRegister fr, Label* ifTruthy,
    Label* ifFalsy, OutOfLineTestObject* ool, bool skipTypeTest) {
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
      Register objreg = masm.extractObject(value, scratch1);
      testObjectEmulatesUndefined(objreg, ifFalsy, ifTruthy, scratch2, ool);
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
      masm.unboxDouble(value, fr);
      masm.branchTestDoubleTruthy(false, fr, ifFalsy);
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

void CodeGenerator::testValueTruthyKernel(
    const ValueOperand& value, const LDefinition* scratch1,
    const LDefinition* scratch2, FloatRegister fr, TypeDataList observedTypes,
    Label* ifTruthy, Label* ifFalsy, OutOfLineTestObject* ool) {
  Register scratch1Reg = ToRegister(scratch1);
  Register scratch2Reg = ToRegister(scratch2);
  ScratchTagScope tag(masm, value);
  masm.splitTagForTest(value, tag);

  const uint32_t NumTypes = 9;
  const auto& defaultOrder = {
      JSVAL_TYPE_UNDEFINED, JSVAL_TYPE_NULL,   JSVAL_TYPE_BOOLEAN,
      JSVAL_TYPE_INT32,     JSVAL_TYPE_OBJECT, JSVAL_TYPE_STRING,
      JSVAL_TYPE_DOUBLE,    JSVAL_TYPE_SYMBOL, JSVAL_TYPE_BIGINT};
  MOZ_ASSERT(defaultOrder.size() == NumTypes);

  Vector<JSValueType, NumTypes, SystemAllocPolicy> remaining;
  MOZ_ALWAYS_TRUE(remaining.reserve(defaultOrder.size()));
  remaining.infallibleAppend(defaultOrder.begin(), defaultOrder.end());

  uint32_t numRemaining = remaining.length();

  // Generate tests for previously observed types first.
  // The TypeDataList is sorted by descending frequency.
  for (auto& observed : observedTypes) {
    JSValueType type = observed.type();

    testValueTruthyForType(type, tag, value, scratch1Reg, scratch2Reg, fr,
                           ifTruthy, ifFalsy, ool, /*skipTypeTest*/ false);
    MOZ_ASSERT(std::count(remaining.begin(), remaining.end(), type) == 1);
    remaining.eraseIfEqual(type);
    numRemaining--;
  }

  // Generate tests for remaining types.
  for (auto type : remaining) {
    // We don't need a type test for the last possible type.
    bool skipTypeTest = numRemaining == 1;
    testValueTruthyForType(type, tag, value, scratch1Reg, scratch2Reg, fr,
                           ifTruthy, ifFalsy, ool, skipTypeTest);
    numRemaining--;
  }
  MOZ_ASSERT(numRemaining == 0);

  // We fall through if the final test is truthy.
}

void CodeGenerator::testValueTruthy(const ValueOperand& value,
                                    const LDefinition* scratch1,
                                    const LDefinition* scratch2,
                                    FloatRegister fr,
                                    TypeDataList observedTypes, Label* ifTruthy,
                                    Label* ifFalsy, OutOfLineTestObject* ool) {
  testValueTruthyKernel(value, scratch1, scratch2, fr, observedTypes, ifTruthy,
                        ifFalsy, ool);
  masm.jump(ifTruthy);
}

void CodeGenerator::visitTestBIAndBranch(LTestBIAndBranch* lir) {
  Label* ifTrueLabel = getJumpLabelForBranch(lir->ifTrue());
  Label* ifFalseLabel = getJumpLabelForBranch(lir->ifFalse());
  Register input = ToRegister(lir->input());

  if (isNextBlock(lir->ifFalse()->lir())) {
    masm.branchIfBigIntIsNonZero(input, ifTrueLabel);
  } else if (isNextBlock(lir->ifTrue()->lir())) {
    masm.branchIfBigIntIsZero(input, ifFalseLabel);
  } else {
    masm.branchIfBigIntIsZero(input, ifFalseLabel);
    jumpToBlock(lir->ifTrue());
  }
}

void CodeGenerator::visitTestOAndBranch(LTestOAndBranch* lir) {
  Label* truthy = getJumpLabelForBranch(lir->ifTruthy());
  Label* falsy = getJumpLabelForBranch(lir->ifFalsy());
  Register input = ToRegister(lir->input());

  auto* ool = new (alloc()) OutOfLineTestObject();
  addOutOfLineCode(ool, lir->mir());

  testObjectEmulatesUndefined(input, falsy, truthy, ToRegister(lir->temp()),
                              ool);
}

void CodeGenerator::visitTestVAndBranch(LTestVAndBranch* lir) {
  auto* ool = new (alloc()) OutOfLineTestObject();
  addOutOfLineCode(ool, lir->mir());

  Label* truthy = getJumpLabelForBranch(lir->ifTruthy());
  Label* falsy = getJumpLabelForBranch(lir->ifFalsy());

  testValueTruthy(ToValue(lir, LTestVAndBranch::Input), lir->temp1(),
                  lir->temp2(), ToFloatRegister(lir->tempFloat()),
                  lir->mir()->observedTypes(), truthy, falsy, ool);
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

void CodeGenerator::emitIntToString(Register input, Register output,
                                    Label* ool) {
  masm.boundsCheck32PowerOfTwo(input, StaticStrings::INT_STATIC_LIMIT, ool);

  // Fast path for small integers.
  masm.movePtr(ImmPtr(&gen->runtime->staticStrings().intStaticTable), output);
  masm.loadPtr(BaseIndex(output, input, ScalePointer), output);
}

void CodeGenerator::visitIntToString(LIntToString* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  using Fn = JSLinearString* (*)(JSContext*, int);
  OutOfLineCode* ool = oolCallVM<Fn, Int32ToString<CanGC>>(
      lir, ArgList(input), StoreRegisterTo(output));

  emitIntToString(input, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitDoubleToString(LDoubleToString* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register temp = ToRegister(lir->tempInt());
  Register output = ToRegister(lir->output());

  using Fn = JSString* (*)(JSContext*, double);
  OutOfLineCode* ool = oolCallVM<Fn, NumberToString<CanGC>>(
      lir, ArgList(input), StoreRegisterTo(output));

  // Try double to integer conversion and run integer to string code.
  masm.convertDoubleToInt32(input, temp, ool->entry(), false);
  emitIntToString(temp, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitValueToString(LValueToString* lir) {
  ValueOperand input = ToValue(lir, LValueToString::Input);
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
    Register unboxed = ToTempUnboxRegister(lir->tempToUnbox());
    unboxed = masm.extractInt32(input, unboxed);
    emitIntToString(unboxed, output, ool->entry());
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
  masm.callWithABI(DynamicFunction<StoreBufferMutationFn>(fun), MoveOp::GENERAL,
                   CheckUnsafeCallWithABI::DontCheckOther);

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
  Register temp = ToRegister(lir->temp());
  JSObject* source = lir->mir()->source();

  using Fn = JSObject* (*)(JSContext*, Handle<RegExpObject*>);
  OutOfLineCode* ool = oolCallVM<Fn, CloneRegExpObject>(
      lir, ArgList(ImmGCPtr(source)), StoreRegisterTo(output));
  if (lir->mir()->hasShared()) {
    TemplateObject templateObject(source);
    masm.createGCObject(output, temp, templateObject, gc::DefaultHeap,
                        ool->entry());
  } else {
    masm.jump(ool->entry());
  }
  masm.bind(ool->rejoin());
}

static const size_t InputOutputDataSize = sizeof(irregexp::InputOutputData);

// Amount of space to reserve on the stack when executing RegExps inline.
static const size_t RegExpReservedStack =
    InputOutputDataSize + sizeof(MatchPairs) +
    RegExpObject::MaxPairCount * sizeof(MatchPair);

static size_t RegExpPairsVectorStartOffset(size_t inputOutputDataStartOffset) {
  return inputOutputDataStartOffset + InputOutputDataSize + sizeof(MatchPairs);
}

static Address RegExpPairCountAddress(MacroAssembler& masm,
                                      size_t inputOutputDataStartOffset) {
  return Address(masm.getStackPointer(), inputOutputDataStartOffset +
                                             InputOutputDataSize +
                                             MatchPairs::offsetOfPairCount());
}

// When the unicode flag is set, if lastIndex points to a trail
// surrogate, we should step back to the corresponding lead surrogate.
// See ExecuteRegExp in builtin/RegExp.cpp for more detail.
static void StepBackToLeadSurrogate(MacroAssembler& masm, Register regexpShared,
                                    Register input, Register lastIndex,
                                    Register temp1, Register temp2) {
  Label done;

  // If the unicode flag is not set, there is nothing to do.
  masm.branchTest32(Assembler::Zero,
                    Address(regexpShared, RegExpShared::offsetOfFlags()),
                    Imm32(int32_t(JS::RegExpFlag::Unicode)), &done);

  // If the input is latin1, there can't be any surrogates.
  masm.branchLatin1String(input, &done);

  // Check if |lastIndex > 0 && lastIndex < input->length()|.
  // lastIndex should already have no sign here.
  masm.branchTest32(Assembler::Zero, lastIndex, lastIndex, &done);
  masm.loadStringLength(input, temp1);
  masm.branch32(Assembler::AboveOrEqual, lastIndex, temp1, &done);

  // For TrailSurrogateMin ≤ x ≤ TrailSurrogateMax and
  // LeadSurrogateMin ≤ x ≤ LeadSurrogateMax, the following
  // equations hold.
  //
  //    SurrogateMin ≤ x ≤ SurrogateMax
  // <> SurrogateMin ≤ x ≤ SurrogateMin + 2^10 - 1
  // <> ((x - SurrogateMin) >>> 10) = 0    where >>> is an unsigned-shift
  // See Hacker's Delight, section 4-1 for details.
  //
  //    ((x - SurrogateMin) >>> 10) = 0
  // <> floor((x - SurrogateMin) / 1024) = 0
  // <> floor((x / 1024) - (SurrogateMin / 1024)) = 0
  // <> floor(x / 1024) = SurrogateMin / 1024
  // <> floor(x / 1024) * 1024 = SurrogateMin
  // <> (x >>> 10) << 10 = SurrogateMin
  // <> x & ~(2^10 - 1) = SurrogateMin

  constexpr char16_t SurrogateMask = 0xFC00;

  Register charsReg = temp1;
  masm.loadStringChars(input, charsReg, CharEncoding::TwoByte);

  // Check if input[lastIndex] is trail surrogate.
  masm.loadChar(charsReg, lastIndex, temp2, CharEncoding::TwoByte);
  masm.and32(Imm32(SurrogateMask), temp2);
  masm.branch32(Assembler::NotEqual, temp2, Imm32(unicode::TrailSurrogateMin),
                &done);

  // Check if input[lastIndex-1] is lead surrogate.
  masm.loadChar(charsReg, lastIndex, temp2, CharEncoding::TwoByte,
                -int32_t(sizeof(char16_t)));
  masm.and32(Imm32(SurrogateMask), temp2);
  masm.branch32(Assembler::NotEqual, temp2, Imm32(unicode::LeadSurrogateMin),
                &done);

  // Move lastIndex back to lead surrogate.
  masm.sub32(Imm32(1), lastIndex);

  masm.bind(&done);
}

static void UpdateRegExpStatics(MacroAssembler& masm, Register regexp,
                                Register input, Register lastIndex,
                                Register staticsReg, Register temp1,
                                Register temp2, bool stringsCanBeInNursery,
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

  if (stringsCanBeInNursery) {
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
  masm.load32(Address(temp1, RegExpShared::offsetOfFlags()), temp2);
  masm.store32(temp2, Address(staticsReg, RegExpStatics::offsetOfLazyFlags()));
}

// Prepare an InputOutputData and optional MatchPairs which space has been
// allocated for on the stack, and try to execute a RegExp on a string input.
// If the RegExp was successfully executed and matched the input, fallthrough.
// Otherwise, jump to notFound or failure.
static bool PrepareAndExecuteRegExp(JSContext* cx, MacroAssembler& masm,
                                    Register regexp, Register input,
                                    Register lastIndex, Register temp1,
                                    Register temp2, Register temp3,
                                    size_t inputOutputDataStartOffset,
                                    bool stringsCanBeInNursery, Label* notFound,
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

  size_t ioOffset = inputOutputDataStartOffset;
  size_t matchPairsOffset = ioOffset + sizeof(InputOutputData);
  size_t pairsArrayOffset = matchPairsOffset + sizeof(MatchPairs);

  Address inputStartAddress(masm.getStackPointer(),
                            ioOffset + offsetof(InputOutputData, inputStart));
  Address inputEndAddress(masm.getStackPointer(),
                          ioOffset + offsetof(InputOutputData, inputEnd));
  Address startIndexAddress(masm.getStackPointer(),
                            ioOffset + offsetof(InputOutputData, startIndex));
  Address matchesAddress(masm.getStackPointer(),
                         ioOffset + offsetof(InputOutputData, matches));

  Address matchPairsAddress(masm.getStackPointer(), matchPairsOffset);
  Address pairCountAddress(masm.getStackPointer(),
                           matchPairsOffset + MatchPairs::offsetOfPairCount());
  Address pairsPointerAddress(masm.getStackPointer(),
                              matchPairsOffset + MatchPairs::offsetOfPairs());

  Address pairsArrayAddress(masm.getStackPointer(), pairsArrayOffset);
  Address firstMatchStartAddress(masm.getStackPointer(),
                                 pairsArrayOffset + offsetof(MatchPair, start));

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

  // Check for a linear input string.
  masm.branchIfRope(input, failure);

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
    LiveGeneralRegisterSet regsToSave(GeneralRegisterSet::Volatile());
    regsToSave.takeUnchecked(temp1);
    regsToSave.takeUnchecked(temp2);
    regsToSave.takeUnchecked(temp3);

    masm.computeEffectiveAddress(matchPairsAddress, temp3);

    masm.PushRegsInMask(regsToSave);
    using Fn = RegExpRunStatus (*)(RegExpShared * re, JSLinearString * input,
                                   size_t start, MatchPairs * matchPairs);
    masm.setupUnalignedABICall(temp2);
    masm.passABIArg(regexpReg);
    masm.passABIArg(input);
    masm.passABIArg(lastIndex);
    masm.passABIArg(temp3);
    masm.callWithABI<Fn, js::ExecuteRegExpAtomRaw>();

    masm.storeCallInt32Result(temp1);
    masm.PopRegsInMask(regsToSave);

    masm.jump(&checkSuccess);
  }
  masm.bind(&notAtom);

  // Don't handle regexps with too many capture pairs.
  masm.load32(Address(regexpReg, RegExpShared::offsetOfPairCount()), temp2);
  masm.branch32(Assembler::Above, temp2, Imm32(RegExpObject::MaxPairCount),
                failure);

  // Fill in the pair count in the MatchPairs on the stack.
  masm.store32(temp2, pairCountAddress);

  // Update lastIndex if necessary.
  StepBackToLeadSurrogate(masm, regexpReg, input, lastIndex, temp2, temp3);

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

  // Save any volatile inputs.
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

#ifdef JS_TRACE_LOGGING
  if (TraceLogTextIdEnabled(TraceLogger_IrregexpExecute)) {
    masm.loadTraceLogger(temp2);
    masm.tracelogStartId(temp2, TraceLogger_IrregexpExecute);
  }
#endif

  // Execute the RegExp.
  masm.computeEffectiveAddress(
      Address(masm.getStackPointer(), inputOutputDataStartOffset), temp2);
  masm.PushRegsInMask(volatileRegs);
  masm.setupUnalignedABICall(temp3);
  masm.passABIArg(temp2);
  masm.callWithABI(codePointer);
  masm.storeCallInt32Result(temp1);
  masm.PopRegsInMask(volatileRegs);

#ifdef JS_TRACE_LOGGING
  if (TraceLogTextIdEnabled(TraceLogger_IrregexpExecute)) {
    masm.loadTraceLogger(temp2);
    masm.tracelogStopId(temp2, TraceLogger_IrregexpExecute);
  }
#endif

  Label success;
  masm.bind(&checkSuccess);
  masm.branch32(Assembler::Equal, temp1,
                Imm32(RegExpRunStatus_Success_NotFound), notFound);
  masm.branch32(Assembler::Equal, temp1, Imm32(RegExpRunStatus_Error), failure);

  // Lazily update the RegExpStatics.
  RegExpStatics* res = GlobalObject::getRegExpStatics(cx, cx->global());
  if (!res) {
    return false;
  }
  masm.movePtr(ImmPtr(res), temp1);
  UpdateRegExpStatics(masm, regexp, input, lastIndex, temp1, temp2, temp3,
                      stringsCanBeInNursery, volatileRegs);

  return true;
}

static void CopyStringChars(MacroAssembler& masm, Register to, Register from,
                            Register len, Register byteOpScratch,
                            CharEncoding encoding);

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
  mozilla::EnumeratedArray<FallbackKind, FallbackKind::Count, Label> fallbacks_,
      joins_;

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
                bool stringsCanBeInNursery);

  // Generate fallback path for creating DependentString.
  void generateFallback(MacroAssembler& masm);
};

void CreateDependentString::generate(MacroAssembler& masm,
                                     const JSAtomState& names,
                                     CompileRuntime* runtime, Register base,
                                     BaseIndex startIndexAddress,
                                     BaseIndex limitIndexAddress,
                                     bool stringsCanBeInNursery) {
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
      masm.newGCString(string_, temp2_, &fallbacks_[kind],
                       stringsCanBeInNursery);
    } else {
      masm.newGCFatInlineString(string_, temp2_, &fallbacks_[kind],
                                stringsCanBeInNursery);
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
  masm.movePtr(ImmGCPtr(names.empty), string_);
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

        masm.movePtr(ImmPtr(&runtime->staticStrings().unitStaticTable),
                     string_);
        masm.loadPtr(BaseIndex(string_, temp1_, ScalePointer), string_);

        masm.jump(&done);
      }
      masm.bind(&thinInline);
    }
    {
      newGCString(FallbackKind::InlineString);
      masm.jump(&stringAllocated);
    }
    masm.bind(&fatInline);
    { newGCString(FallbackKind::FatInlineString); }
    masm.bind(&stringAllocated);

    masm.store32(temp1_, Address(string_, JSString::offsetOfLength()));

    masm.push(string_);
    masm.push(base);

    // Adjust the start index address for the above pushes.
    MOZ_ASSERT(startIndexAddress.base == masm.getStackPointer());
    BaseIndex newStartIndexAddress = startIndexAddress;
    newStartIndexAddress.offset += 2 * sizeof(void*);

    // Load chars pointer for the new string.
    masm.loadInlineStringCharsForStore(string_, string_);

    // Load the source characters pointer.
    masm.loadStringChars(base, temp2_, encoding_);
    masm.load32(newStartIndexAddress, base);
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
    masm.storeDependentStringBase(base, string_);
    masm.movePtr(base, temp1_);

    // Follow any base pointer if the input is itself a dependent string.
    // Watch for undepended strings, which have a base pointer but don't
    // actually share their characters with it.
    Label noBase;
    masm.load32(Address(base, JSString::offsetOfFlags()), temp2_);
    masm.and32(Imm32(JSString::TYPE_FLAGS_MASK), temp2_);
    masm.branchTest32(Assembler::Zero, temp2_, Imm32(JSString::DEPENDENT_BIT),
                      &noBase);
    masm.loadDependentStringBase(base, temp1_);
    masm.storeDependentStringBase(temp1_, string_);
    masm.bind(&noBase);

    // Post-barrier the base store, whether it was the direct or indirect
    // base (both will end up in temp1 here).
    masm.branchPtrInNurseryChunk(Assembler::Equal, string_, temp2_, &done);
    masm.branchPtrInNurseryChunk(Assembler::NotEqual, temp1_, temp2_, &done);

    LiveRegisterSet regsToSave(RegisterSet::Volatile());
    regsToSave.takeUnchecked(temp1_);
    regsToSave.takeUnchecked(temp2_);

    masm.PushRegsInMask(regsToSave);

    masm.mov(ImmPtr(runtime), temp1_);

    using Fn = void (*)(JSRuntime * rt, js::gc::Cell * cell);
    masm.setupUnalignedABICall(temp2_);
    masm.passABIArg(temp1_);
    masm.passABIArg(string_);
    masm.callWithABI<Fn, PostWriteBarrier>();

    masm.PopRegsInMask(regsToSave);
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

    using Fn = void* (*)(JSContext * cx);
    masm.setupUnalignedABICall(string_);
    masm.loadJSContext(string_);
    masm.passABIArg(string_);
    if (kind == FallbackKind::FatInlineString) {
      masm.callWithABI<Fn, AllocateFatInlineString>();
    } else {
      masm.callWithABI<Fn, AllocateString>();
    }
    masm.storeCallPointerResult(string_);

    masm.PopRegsInMask(regsToSave);

    masm.branchPtr(Assembler::Equal, string_, ImmWord(0), failure_);

    masm.jump(&joins_[kind]);
  }
}

static void CreateMatchResultFallback(MacroAssembler& masm, Register object,
                                      Register temp1, Register temp2,
                                      const TemplateObject& templateObject,
                                      Label* fail) {
  JitSpew(JitSpew_Codegen, "# Emitting CreateMatchResult fallback");

  MOZ_ASSERT(templateObject.isArrayObject());

  LiveRegisterSet regsToSave(RegisterSet::Volatile());
  regsToSave.takeUnchecked(object);
  regsToSave.takeUnchecked(temp1);
  regsToSave.takeUnchecked(temp2);

  masm.PushRegsInMask(regsToSave);

  using Fn =
      void* (*)(JSContext * cx, gc::AllocKind kind, size_t nDynamicSlots);
  masm.setupUnalignedABICall(object);

  masm.loadJSContext(object);
  masm.passABIArg(object);
  masm.move32(Imm32(int32_t(templateObject.getAllocKind())), temp1);
  masm.passABIArg(temp1);
  masm.move32(
      Imm32(int32_t(templateObject.asTemplateNativeObject().numDynamicSlots())),
      temp2);
  masm.passABIArg(temp2);
  masm.callWithABI<Fn, CreateMatchResultFallbackFunc>();
  masm.storeCallPointerResult(object);

  masm.PopRegsInMask(regsToSave);

  masm.branchPtr(Assembler::Equal, object, ImmWord(0), fail);

  masm.initGCThing(object, temp1, templateObject, true);
}

JitCode* JitRealm::generateRegExpMatcherStub(JSContext* cx) {
  JitSpew(JitSpew_Codegen, "# Emitting RegExpMatcher stub");

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
  Register temp4 = regs.takeAny();
  Register maybeTemp5 = InvalidReg;
  if (!regs.empty()) {
    // There are not enough registers on x86.
    maybeTemp5 = regs.takeAny();
  }

  ArrayObject* templateObject =
      cx->realm()->regExps.getOrCreateMatchResultTemplateObject(cx);
  if (!templateObject) {
    return nullptr;
  }
  TemplateObject templateObj(templateObject);
  const TemplateNativeObject& nativeTemplateObj =
      templateObj.asTemplateNativeObject();

  // The template object should have enough space for the maximum number of
  // pairs this stub can handle.
  MOZ_ASSERT(ObjectElements::VALUES_PER_HEADER + RegExpObject::MaxPairCount ==
             gc::GetGCKindSlots(templateObj.getAllocKind()));

  StackMacroAssembler masm(cx);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  // The InputOutputData is placed above the return address on the stack.
  size_t inputOutputDataStartOffset = sizeof(void*);

  Label notFound, oolEntry;
  if (!PrepareAndExecuteRegExp(cx, masm, regexp, input, lastIndex, temp1, temp2,
                               temp3, inputOutputDataStartOffset,
                               stringsCanBeInNursery, &notFound, &oolEntry)) {
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

  // Construct the result.
  Register object = temp1;
  Label matchResultFallback, matchResultJoin;
  masm.createGCObject(object, temp2, templateObj, gc::DefaultHeap,
                      &matchResultFallback);
  masm.bind(&matchResultJoin);

  MOZ_ASSERT(nativeTemplateObj.numFixedSlots() == 0);
  // Dynamic slot count is always one less than a power of 2.
  MOZ_ASSERT(nativeTemplateObj.numDynamicSlots() == 3);
  static_assert(RegExpRealm::MatchResultObjectIndexSlot == 0,
                "First slot holds the 'index' property");
  static_assert(RegExpRealm::MatchResultObjectInputSlot == 1,
                "Second slot holds the 'input' property");
  static_assert(RegExpRealm::MatchResultObjectGroupsSlot == 2,
                "Third slot holds the 'groups' property");

  // Initialize the slots of the result object with the dummy values
  // defined in createMatchResultTemplateObject.
  masm.loadPtr(Address(object, NativeObject::offsetOfSlots()), temp2);
  masm.storeValue(
      nativeTemplateObj.getSlot(RegExpRealm::MatchResultObjectIndexSlot),
      Address(temp2, RegExpRealm::offsetOfMatchResultObjectIndexSlot()));
  masm.storeValue(
      nativeTemplateObj.getSlot(RegExpRealm::MatchResultObjectInputSlot),
      Address(temp2, RegExpRealm::offsetOfMatchResultObjectInputSlot()));
  masm.storeValue(
      nativeTemplateObj.getSlot(RegExpRealm::MatchResultObjectGroupsSlot),
      Address(temp2, RegExpRealm::offsetOfMatchResultObjectGroupsSlot()));

  // clang-format off
   /*
    * [SMDOC] Stack layout for the RegExpMatcher stub
    *
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

  Address pairCountAddress =
      RegExpPairCountAddress(masm, inputOutputDataStartOffset);

  size_t pairsVectorStartOffset =
      RegExpPairsVectorStartOffset(inputOutputDataStartOffset);
  Address firstMatchPairStartAddress(
      masm.getStackPointer(),
      pairsVectorStartOffset + offsetof(MatchPair, start));

  // Incremented by one below for each match pair.
  Register matchIndex = temp2;
  masm.move32(Imm32(0), matchIndex);

  // The element in which to store the result of the current match.
  size_t elementsOffset = NativeObject::offsetOfFixedElements();
  BaseObjectElementIndex objectMatchElement(object, matchIndex, elementsOffset);

  // The current match pair's "start" and "limit" member.
  BaseIndex matchPairStart(masm.getStackPointer(), matchIndex, TimesEight,
                           pairsVectorStartOffset + offsetof(MatchPair, start));
  BaseIndex matchPairLimit(masm.getStackPointer(), matchIndex, TimesEight,
                           pairsVectorStartOffset + offsetof(MatchPair, limit));

  Register temp5;
  if (maybeTemp5 == InvalidReg) {
    // We don't have enough registers for a fifth temporary. Reuse
    // |lastIndex| as a temporary. We don't need to restore its value,
    // because |lastIndex| is no longer used after a successful match.
    // (Neither here nor in the OOL path, cf. js::RegExpMatcherRaw.)
    temp5 = lastIndex;
  } else {
    temp5 = maybeTemp5;
  }

  // Loop to construct the match strings. There are two different loops,
  // depending on whether the input is a Two-Byte or a Latin-1 string.
  CreateDependentString depStrs[]{
      {CharEncoding::TwoByte, temp3, temp4, temp5, &oolEntry},
      {CharEncoding::Latin1, temp3, temp4, temp5, &oolEntry},
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
                        stringsCanBeInNursery);

        // Storing into nursery-allocated results object's elements; no post
        // barrier.
        masm.storeValue(JSVAL_TYPE_STRING, depStr.string(), objectMatchElement);
        masm.jump(&storeDone);
      }
      masm.bind(&isUndefined);
      { masm.storeValue(UndefinedValue(), objectMatchElement); }
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

  // Fill in the rest of the output object.
  masm.store32(
      matchIndex,
      Address(object,
              elementsOffset + ObjectElements::offsetOfInitializedLength()));
  masm.store32(
      matchIndex,
      Address(object, elementsOffset + ObjectElements::offsetOfLength()));

  masm.loadPtr(Address(object, NativeObject::offsetOfSlots()), temp2);

  masm.load32(firstMatchPairStartAddress, temp3);
  masm.storeValue(JSVAL_TYPE_INT32, temp3, Address(temp2, 0));

  // No post barrier needed (address is within nursery object.)
  masm.storeValue(JSVAL_TYPE_STRING, input, Address(temp2, sizeof(Value)));

  // All done!
  masm.tagValue(JSVAL_TYPE_OBJECT, object, result);
  masm.ret();

  masm.bind(&notFound);
  masm.moveValue(NullValue(), result);
  masm.ret();

  // Fallback paths for CreateDependentString.
  for (auto& depStr : depStrs) {
    depStr.generateFallback(masm);
  }

  // Fallback path for createGCObject.
  masm.bind(&matchResultFallback);
  CreateMatchResultFallback(masm, object, temp2, temp3, templateObj, &oolEntry);
  masm.jump(&matchResultJoin);

  // Use an undefined value to signal to the caller that the OOL stub needs to
  // be called.
  masm.bind(&oolEntry);
  masm.moveValue(UndefinedValue(), result);
  masm.ret();

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);
  if (!code) {
    return nullptr;
  }

#ifdef JS_ION_PERF
  writePerfSpewerJitCodeProfile(code, "RegExpMatcherStub");
#endif
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, "RegExpMatcherStub");
#endif

  return code;
}

class OutOfLineRegExpMatcher : public OutOfLineCodeBase<CodeGenerator> {
  LRegExpMatcher* lir_;

 public:
  explicit OutOfLineRegExpMatcher(LRegExpMatcher* lir) : lir_(lir) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineRegExpMatcher(this);
  }

  LRegExpMatcher* lir() const { return lir_; }
};

void CodeGenerator::visitOutOfLineRegExpMatcher(OutOfLineRegExpMatcher* ool) {
  LRegExpMatcher* lir = ool->lir();
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
                      int32_t lastIndex, MatchPairs * pairs,
                      MutableHandleValue output);
  callVM<Fn, RegExpMatcherRaw>(lir);

  masm.jump(ool->rejoin());
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

  OutOfLineRegExpMatcher* ool = new (alloc()) OutOfLineRegExpMatcher(lir);
  addOutOfLineCode(ool, lir->mir());

  const JitRealm* jitRealm = gen->realm->jitRealm();
  JitCode* regExpMatcherStub =
      jitRealm->regExpMatcherStubNoBarrier(&realmStubsToReadBarrier_);
  masm.call(regExpMatcherStub);
  masm.branchTestUndefined(Assembler::Equal, JSReturnOperand, ool->entry());
  masm.bind(ool->rejoin());

  masm.freeStack(RegExpReservedStack);
}

static const int32_t RegExpSearcherResultNotFound = -1;
static const int32_t RegExpSearcherResultFailed = -2;

JitCode* JitRealm::generateRegExpSearcherStub(JSContext* cx) {
  JitSpew(JitSpew_Codegen, "# Emitting RegExpSearcher stub");

  Register regexp = RegExpTesterRegExpReg;
  Register input = RegExpTesterStringReg;
  Register lastIndex = RegExpTesterLastIndexReg;
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

  StackMacroAssembler masm(cx);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  // The InputOutputData is placed above the return address on the stack.
  size_t inputOutputDataStartOffset = sizeof(void*);

  Label notFound, oolEntry;
  if (!PrepareAndExecuteRegExp(cx, masm, regexp, input, lastIndex, temp1, temp2,
                               temp3, inputOutputDataStartOffset,
                               stringsCanBeInNursery, &notFound, &oolEntry)) {
    return nullptr;
  }

  // clang-format off
    /*
     * [SMDOC] Stack layout for the RegExpSearcher stub
     *
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

  size_t pairsVectorStartOffset =
      RegExpPairsVectorStartOffset(inputOutputDataStartOffset);
  Address matchPairStart(masm.getStackPointer(),
                         pairsVectorStartOffset + offsetof(MatchPair, start));
  Address matchPairLimit(masm.getStackPointer(),
                         pairsVectorStartOffset + offsetof(MatchPair, limit));

  masm.load32(matchPairStart, result);
  masm.load32(matchPairLimit, input);
  masm.lshiftPtr(Imm32(15), input);
  masm.or32(input, result);
  masm.ret();

  masm.bind(&notFound);
  masm.move32(Imm32(RegExpSearcherResultNotFound), result);
  masm.ret();

  masm.bind(&oolEntry);
  masm.move32(Imm32(RegExpSearcherResultFailed), result);
  masm.ret();

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);
  if (!code) {
    return nullptr;
  }

#ifdef JS_ION_PERF
  writePerfSpewerJitCodeProfile(code, "RegExpSearcherStub");
#endif
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, "RegExpSearcherStub");
#endif

  return code;
}

class OutOfLineRegExpSearcher : public OutOfLineCodeBase<CodeGenerator> {
  LRegExpSearcher* lir_;

 public:
  explicit OutOfLineRegExpSearcher(LRegExpSearcher* lir) : lir_(lir) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineRegExpSearcher(this);
  }

  LRegExpSearcher* lir() const { return lir_; }
};

void CodeGenerator::visitOutOfLineRegExpSearcher(OutOfLineRegExpSearcher* ool) {
  LRegExpSearcher* lir = ool->lir();
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
  using Fn = bool (*)(JSContext * cx, HandleObject regexp, HandleString input,
                      int32_t lastIndex, MatchPairs * pairs, int32_t * result);
  callVM<Fn, RegExpSearcherRaw>(lir);

  masm.jump(ool->rejoin());
}

void CodeGenerator::visitRegExpSearcher(LRegExpSearcher* lir) {
  MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpTesterRegExpReg);
  MOZ_ASSERT(ToRegister(lir->string()) == RegExpTesterStringReg);
  MOZ_ASSERT(ToRegister(lir->lastIndex()) == RegExpTesterLastIndexReg);
  MOZ_ASSERT(ToRegister(lir->output()) == ReturnReg);

  static_assert(RegExpTesterRegExpReg != ReturnReg);
  static_assert(RegExpTesterStringReg != ReturnReg);
  static_assert(RegExpTesterLastIndexReg != ReturnReg);

  masm.reserveStack(RegExpReservedStack);

  OutOfLineRegExpSearcher* ool = new (alloc()) OutOfLineRegExpSearcher(lir);
  addOutOfLineCode(ool, lir->mir());

  const JitRealm* jitRealm = gen->realm->jitRealm();
  JitCode* regExpSearcherStub =
      jitRealm->regExpSearcherStubNoBarrier(&realmStubsToReadBarrier_);
  masm.call(regExpSearcherStub);
  masm.branch32(Assembler::Equal, ReturnReg, Imm32(RegExpSearcherResultFailed),
                ool->entry());
  masm.bind(ool->rejoin());

  masm.freeStack(RegExpReservedStack);
}

static const int32_t RegExpTesterResultNotFound = -1;
static const int32_t RegExpTesterResultFailed = -2;

JitCode* JitRealm::generateRegExpTesterStub(JSContext* cx) {
  JitSpew(JitSpew_Codegen, "# Emitting RegExpTester stub");

  Register regexp = RegExpTesterRegExpReg;
  Register input = RegExpTesterStringReg;
  Register lastIndex = RegExpTesterLastIndexReg;
  Register result = ReturnReg;

  StackMacroAssembler masm(cx);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  // We are free to clobber all registers, as LRegExpTester is a call
  // instruction.
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(input);
  regs.take(regexp);
  regs.take(lastIndex);

  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();
  Register temp3 = regs.takeAny();

  masm.reserveStack(RegExpReservedStack);

  Label notFound, oolEntry;
  if (!PrepareAndExecuteRegExp(cx, masm, regexp, input, lastIndex, temp1, temp2,
                               temp3, 0, stringsCanBeInNursery, &notFound,
                               &oolEntry)) {
    return nullptr;
  }

  Label done;

  // In visitRegExpMatcher and visitRegExpSearcher, we reserve stack space
  // before calling the stub. For RegExpTester we call the stub before reserving
  // stack space, so the offset of the InputOutputData is 0.
  size_t inputOutputDataStartOffset = 0;

  size_t pairsVectorStartOffset =
      RegExpPairsVectorStartOffset(inputOutputDataStartOffset);
  Address matchPairLimit(masm.getStackPointer(),
                         pairsVectorStartOffset + offsetof(MatchPair, limit));

  // RegExpTester returns the end index of the match to update lastIndex.
  masm.load32(matchPairLimit, result);
  masm.jump(&done);

  masm.bind(&notFound);
  masm.move32(Imm32(RegExpTesterResultNotFound), result);
  masm.jump(&done);

  masm.bind(&oolEntry);
  masm.move32(Imm32(RegExpTesterResultFailed), result);

  masm.bind(&done);
  masm.freeStack(RegExpReservedStack);
  masm.ret();

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);
  if (!code) {
    return nullptr;
  }

#ifdef JS_ION_PERF
  writePerfSpewerJitCodeProfile(code, "RegExpTesterStub");
#endif
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, "RegExpTesterStub");
#endif

  return code;
}

class OutOfLineRegExpTester : public OutOfLineCodeBase<CodeGenerator> {
  LRegExpTester* lir_;

 public:
  explicit OutOfLineRegExpTester(LRegExpTester* lir) : lir_(lir) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineRegExpTester(this);
  }

  LRegExpTester* lir() const { return lir_; }
};

void CodeGenerator::visitOutOfLineRegExpTester(OutOfLineRegExpTester* ool) {
  LRegExpTester* lir = ool->lir();
  Register lastIndex = ToRegister(lir->lastIndex());
  Register input = ToRegister(lir->string());
  Register regexp = ToRegister(lir->regexp());

  pushArg(lastIndex);
  pushArg(input);
  pushArg(regexp);

  // We are not using oolCallVM because we are in a Call, and that live
  // registers are already saved by the the register allocator.
  using Fn = bool (*)(JSContext * cx, HandleObject regexp, HandleString input,
                      int32_t lastIndex, int32_t * result);
  callVM<Fn, RegExpTesterRaw>(lir);

  masm.jump(ool->rejoin());
}

void CodeGenerator::visitRegExpTester(LRegExpTester* lir) {
  MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpTesterRegExpReg);
  MOZ_ASSERT(ToRegister(lir->string()) == RegExpTesterStringReg);
  MOZ_ASSERT(ToRegister(lir->lastIndex()) == RegExpTesterLastIndexReg);
  MOZ_ASSERT(ToRegister(lir->output()) == ReturnReg);

  static_assert(RegExpTesterRegExpReg != ReturnReg);
  static_assert(RegExpTesterStringReg != ReturnReg);
  static_assert(RegExpTesterLastIndexReg != ReturnReg);

  OutOfLineRegExpTester* ool = new (alloc()) OutOfLineRegExpTester(lir);
  addOutOfLineCode(ool, lir->mir());

  const JitRealm* jitRealm = gen->realm->jitRealm();
  JitCode* regExpTesterStub =
      jitRealm->regExpTesterStubNoBarrier(&realmStubsToReadBarrier_);
  masm.call(regExpTesterStub);

  masm.branch32(Assembler::Equal, ReturnReg, Imm32(RegExpTesterResultFailed),
                ool->entry());
  masm.bind(ool->rejoin());
}

class OutOfLineRegExpPrototypeOptimizable
    : public OutOfLineCodeBase<CodeGenerator> {
  LRegExpPrototypeOptimizable* ins_;

 public:
  explicit OutOfLineRegExpPrototypeOptimizable(LRegExpPrototypeOptimizable* ins)
      : ins_(ins) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineRegExpPrototypeOptimizable(this);
  }
  LRegExpPrototypeOptimizable* ins() const { return ins_; }
};

void CodeGenerator::visitRegExpPrototypeOptimizable(
    LRegExpPrototypeOptimizable* ins) {
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp());

  OutOfLineRegExpPrototypeOptimizable* ool =
      new (alloc()) OutOfLineRegExpPrototypeOptimizable(ins);
  addOutOfLineCode(ool, ins->mir());

  masm.branchIfNotRegExpPrototypeOptimizable(object, temp, ool->entry());
  masm.move32(Imm32(0x1), output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineRegExpPrototypeOptimizable(
    OutOfLineRegExpPrototypeOptimizable* ool) {
  LRegExpPrototypeOptimizable* ins = ool->ins();
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  saveVolatile(output);

  using Fn = bool (*)(JSContext * cx, JSObject * proto);
  masm.setupUnalignedABICall(output);
  masm.loadJSContext(output);
  masm.passABIArg(output);
  masm.passABIArg(object);
  masm.callWithABI<Fn, RegExpPrototypeOptimizableRaw>();
  masm.storeCallBoolResult(output);

  restoreVolatile(output);

  masm.jump(ool->rejoin());
}

class OutOfLineRegExpInstanceOptimizable
    : public OutOfLineCodeBase<CodeGenerator> {
  LRegExpInstanceOptimizable* ins_;

 public:
  explicit OutOfLineRegExpInstanceOptimizable(LRegExpInstanceOptimizable* ins)
      : ins_(ins) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineRegExpInstanceOptimizable(this);
  }
  LRegExpInstanceOptimizable* ins() const { return ins_; }
};

void CodeGenerator::visitRegExpInstanceOptimizable(
    LRegExpInstanceOptimizable* ins) {
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp());

  OutOfLineRegExpInstanceOptimizable* ool =
      new (alloc()) OutOfLineRegExpInstanceOptimizable(ins);
  addOutOfLineCode(ool, ins->mir());

  masm.branchIfNotRegExpInstanceOptimizable(object, temp, ool->entry());
  masm.move32(Imm32(0x1), output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineRegExpInstanceOptimizable(
    OutOfLineRegExpInstanceOptimizable* ool) {
  LRegExpInstanceOptimizable* ins = ool->ins();
  Register object = ToRegister(ins->object());
  Register proto = ToRegister(ins->proto());
  Register output = ToRegister(ins->output());

  saveVolatile(output);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, JSObject * proto);
  masm.setupUnalignedABICall(output);
  masm.loadJSContext(output);
  masm.passABIArg(output);
  masm.passABIArg(object);
  masm.passABIArg(proto);
  masm.callWithABI<Fn, RegExpInstanceOptimizableRaw>();
  masm.storeCallBoolResult(output);

  restoreVolatile(output);

  masm.jump(ool->rejoin());
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
  TypedOrValueRegister lhs =
      TypedOrValueRegister(ToValue(lir, LBinaryValueCache::LhsInput));
  TypedOrValueRegister rhs =
      TypedOrValueRegister(ToValue(lir, LBinaryValueCache::RhsInput));
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
  TypedOrValueRegister lhs =
      TypedOrValueRegister(ToValue(lir, LBinaryBoolCache::LhsInput));
  TypedOrValueRegister rhs =
      TypedOrValueRegister(ToValue(lir, LBinaryBoolCache::RhsInput));
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
  TypedOrValueRegister input =
      TypedOrValueRegister(ToValue(lir, LUnaryCache::Input));
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
  pushArg(ToValue(lir, LDynamicImport::SpecifierIndex));
  pushArg(ImmGCPtr(current->mir()->info().script()));

  using Fn = JSObject* (*)(JSContext*, HandleScript, HandleValue);
  callVM<Fn, js::StartDynamicModuleImport>(lir);
}

void CodeGenerator::visitLambda(LLambda* lir) {
  Register envChain = ToRegister(lir->environmentChain());
  Register output = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp());
  const LambdaFunctionInfo& info = lir->mir()->info();

  using Fn = JSObject* (*)(JSContext*, HandleFunction, HandleObject);
  OutOfLineCode* ool = oolCallVM<Fn, js::Lambda>(
      lir, ArgList(ImmGCPtr(info.funUnsafe()), envChain),
      StoreRegisterTo(output));

  TemplateObject templateObject(info.funUnsafe());
  masm.createGCObject(output, tempReg, templateObject, gc::DefaultHeap,
                      ool->entry());

  emitLambdaInit(output, envChain, info);

  if (info.flags.isExtended()) {
    MOZ_ASSERT(info.flags.allowSuperProperty() ||
               info.flags.isSelfHostedBuiltin());
    static_assert(FunctionExtended::NUM_EXTENDED_SLOTS == 2,
                  "All slots must be initialized");
    masm.storeValue(UndefinedValue(),
                    Address(output, FunctionExtended::offsetOfExtendedSlot(0)));
    masm.storeValue(UndefinedValue(),
                    Address(output, FunctionExtended::offsetOfExtendedSlot(1)));
  }

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitLambdaArrow(LLambdaArrow* lir) {
  Register envChain = ToRegister(lir->environmentChain());
  ValueOperand newTarget = ToValue(lir, LLambdaArrow::NewTargetValue);
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp());
  const LambdaFunctionInfo& info = lir->mir()->info();

  using Fn =
      JSObject* (*)(JSContext*, HandleFunction, HandleObject, HandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, LambdaArrow>(
      lir, ArgList(ImmGCPtr(info.funUnsafe()), envChain, newTarget),
      StoreRegisterTo(output));

  TemplateObject templateObject(info.funUnsafe());
  masm.createGCObject(output, temp, templateObject, gc::DefaultHeap,
                      ool->entry());

  emitLambdaInit(output, envChain, info);

  // Initialize extended slots. Lexical |this| is stored in the first one.
  MOZ_ASSERT(info.flags.isExtended());
  static_assert(FunctionExtended::NUM_EXTENDED_SLOTS == 2,
                "All slots must be initialized");
  static_assert(FunctionExtended::ARROW_NEWTARGET_SLOT == 0,
                "|new.target| must be stored in first slot");
  masm.storeValue(newTarget,
                  Address(output, FunctionExtended::offsetOfExtendedSlot(0)));
  masm.storeValue(UndefinedValue(),
                  Address(output, FunctionExtended::offsetOfExtendedSlot(1)));

  masm.bind(ool->rejoin());
}

void CodeGenerator::emitLambdaInit(Register output, Register envChain,
                                   const LambdaFunctionInfo& info) {
  // Initialize nargs and flags. We do this with a single uint32 to avoid
  // 16-bit writes.
  union {
    struct S {
      uint16_t nargs;
      uint16_t flags;
    } s;
    uint32_t word;
  } u;
  u.s.nargs = info.nargs;
  u.s.flags = info.flags.toRaw();

  static_assert(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2,
                "the code below needs to be adapted");
  masm.store32(Imm32(u.word), Address(output, JSFunction::offsetOfNargs()));
  masm.storePtr(ImmGCPtr(info.baseScript),
                Address(output, JSFunction::offsetOfBaseScript()));
  masm.storePtr(envChain, Address(output, JSFunction::offsetOfEnvironment()));
  // No post barrier needed because output is guaranteed to be allocated in
  // the nursery.
  masm.storePtr(ImmGCPtr(info.funUnsafe()->displayAtom()),
                Address(output, JSFunction::offsetOfAtom()));
}

void CodeGenerator::visitFunctionWithProto(LFunctionWithProto* lir) {
  Register envChain = ToRegister(lir->environmentChain());
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
  pushArg(ToValue(lir, LSetFunName::NameValue));
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
  const LAllocation* temp;

  if (mir->getOperand(0)->type() != MIRType::Int32) {
    temp = ins->tempInt()->output();

    // The input is a double, so try and convert it to an integer.
    // If it does not fit in an integer, take the default case.
    masm.convertDoubleToInt32(ToFloatRegister(ins->index()), ToRegister(temp),
                              defaultcase, false);
  } else {
    temp = ins->index();
  }

  emitTableSwitchDispatch(mir, ToRegister(temp),
                          ToRegisterOrInvalid(ins->tempPointer()));
}

void CodeGenerator::visitTableSwitchV(LTableSwitchV* ins) {
  MTableSwitch* mir = ins->mir();
  Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

  Register index = ToRegister(ins->tempInt());
  ValueOperand value = ToValue(ins, LTableSwitchV::InputValue);
  Register tag = masm.extractTag(value, index);
  masm.branchTestNumber(Assembler::NotEqual, tag, defaultcase);

  Label unboxInt, isInt;
  masm.branchTestInt32(Assembler::Equal, tag, &unboxInt);
  {
    FloatRegister floatIndex = ToFloatRegister(ins->tempFloat());
    masm.unboxDouble(value, floatIndex);
    masm.convertDoubleToInt32(floatIndex, index, defaultcase, false);
    masm.jump(&isInt);
  }

  masm.bind(&unboxInt);
  masm.unboxInt32(value, index);

  masm.bind(&isInt);

  emitTableSwitchDispatch(mir, index, ToRegisterOrInvalid(ins->tempPointer()));
}

void CodeGenerator::visitParameter(LParameter* lir) {}

void CodeGenerator::visitCallee(LCallee* lir) {
  Register callee = ToRegister(lir->output());
  Address ptr(masm.getStackPointer(),
              frameSize() + JitFrameLayout::offsetOfCalleeToken());

  masm.loadFunctionFromCalleeToken(ptr, callee);
}

void CodeGenerator::visitIsConstructing(LIsConstructing* lir) {
  Register output = ToRegister(lir->output());
  Address calleeToken(masm.getStackPointer(),
                      frameSize() + JitFrameLayout::offsetOfCalleeToken());
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

#ifdef JS_TRACE_LOGGING
  if (JS::TraceLoggerSupported()) {
    emitTracelogStopEvent(TraceLogger_Baseline);
    emitTracelogStartEvent(TraceLogger_IonMonkey);
  }
#endif

  // If profiling, save the current frame pointer to a per-thread global field.
  if (isProfilerInstrumentationEnabled()) {
    masm.profilerEnterFrame(masm.getStackPointer(), temp);
  }

  // Allocate the full frame for this function
  // Note we have a new entry here. So we reset MacroAssembler::framePushed()
  // to 0, before reserving the stack.
  MOZ_ASSERT(masm.framePushed() == frameSize());
  masm.setFramePushed(0);

  // Ensure that the Ion frames is properly aligned.
  masm.assertStackAlignment(JitStackAlignment, 0);

  masm.reserveStack(frameSize());
}

void CodeGenerator::visitOsrEnvironmentChain(LOsrEnvironmentChain* lir) {
  const LAllocation* frame = lir->getOperand(0);
  const LDefinition* object = lir->getDef(0);

  const ptrdiff_t frameOffset =
      BaselineFrame::reverseOffsetOfEnvironmentChain();

  masm.loadPtr(Address(ToRegister(frame), frameOffset), ToRegister(object));
}

void CodeGenerator::visitOsrArgumentsObject(LOsrArgumentsObject* lir) {
  const LAllocation* frame = lir->getOperand(0);
  const LDefinition* object = lir->getDef(0);

  const ptrdiff_t frameOffset = BaselineFrame::reverseOffsetOfArgsObj();

  masm.loadPtr(Address(ToRegister(frame), frameOffset), ToRegister(object));
}

void CodeGenerator::visitOsrValue(LOsrValue* value) {
  const LAllocation* frame = value->getOperand(0);
  const ValueOperand out = ToOutValue(value);

  const ptrdiff_t frameOffset = value->mir()->frameOffset();

  masm.loadValue(Address(ToRegister(frame), frameOffset), out);
}

void CodeGenerator::visitOsrReturnValue(LOsrReturnValue* lir) {
  const LAllocation* frame = lir->getOperand(0);
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
  const LAllocation* arg = lir->getArgument();
  MIRType argType = lir->type();
  uint32_t argslot = lir->argslot();
  MOZ_ASSERT(argslot - 1u < graph.argumentSlotCount());

  int32_t stack_offset = StackOffsetOfPassedArg(argslot);
  Address dest(masm.getStackPointer(), stack_offset);

  if (arg->isFloatReg()) {
    masm.boxDouble(ToFloatRegister(arg), dest);
  } else if (arg->isRegister()) {
    masm.storeValue(ValueTypeFromMIRType(argType), ToRegister(arg), dest);
  } else {
    masm.storeValue(arg->toConstant()->toJSValue(), dest);
  }
}

void CodeGenerator::visitStackArgV(LStackArgV* lir) {
  ValueOperand val = ToValue(lir, 0);
  uint32_t argslot = lir->argslot();
  MOZ_ASSERT(argslot - 1u < graph.argumentSlotCount());

  int32_t stack_offset = StackOffsetOfPassedArg(argslot);

  masm.storeValue(val, Address(masm.getStackPointer(), stack_offset));
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
  masm.move32(Imm32(lir->getValue()), ToRegister(lir->output()));
}

void CodeGenerator::visitInteger64(LInteger64* lir) {
  masm.move64(Imm64(lir->getValue()), ToOutRegister64(lir));
}

void CodeGenerator::visitPointer(LPointer* lir) {
  masm.movePtr(ImmGCPtr(lir->gcptr()), ToRegister(lir->output()));
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
  masm.storeUnboxedValue(value, valueType, dest, lir->mir()->slotType());
}

void CodeGenerator::visitStoreDynamicSlotV(LStoreDynamicSlotV* lir) {
  Register base = ToRegister(lir->slots());
  int32_t offset = lir->mir()->slot() * sizeof(Value);

  const ValueOperand value = ToValue(lir, LStoreDynamicSlotV::Value);

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
  masm.loadPtr(environment, ToRegister(lir->output()));
}

void CodeGenerator::visitHomeObject(LHomeObject* lir) {
  Address homeObject(ToRegister(lir->function()),
                     FunctionExtended::offsetOfMethodHomeObjectSlot());
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
  Register output = ToRegister(lir->output());

  using Fn = bool (*)(JSContext*);
  OutOfLineCode* ool =
      oolCallVM<Fn, ThrowHomeObjectNotObject>(lir, ArgList(), StoreNothing());

  masm.loadObjProto(homeObject, output);

#ifdef DEBUG
  // We won't encounter a lazy proto, because the prototype is guaranteed to
  // either be a JSFunction or a PlainObject, and only proxy objects can have a
  // lazy proto.
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  Label proxyCheckDone;
  masm.branchPtr(Assembler::NotEqual, output, ImmWord(1), &proxyCheckDone);
  masm.assumeUnreachable("Unexpected lazy proto in JSOp::SuperBase");
  masm.bind(&proxyCheckDone);
#endif

  masm.branchPtr(Assembler::Equal, output, ImmWord(0), ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewLexicalEnvironmentObject(
    LNewLexicalEnvironmentObject* lir) {
  pushArg(Imm32(gc::DefaultHeap));
  pushArg(ToRegister(lir->enclosing()));
  pushArg(ImmGCPtr(lir->mir()->scope()));

  using Fn =
      BlockLexicalEnvironmentObject* (*)(JSContext*, Handle<LexicalScope*>,
                                         HandleObject, gc::InitialHeap);
  callVM<Fn, BlockLexicalEnvironmentObject::create>(lir);
}

void CodeGenerator::visitNewClassBodyEnvironmentObject(
    LNewClassBodyEnvironmentObject* lir) {
  pushArg(Imm32(gc::DefaultHeap));
  pushArg(ToRegister(lir->enclosing()));
  pushArg(ImmGCPtr(lir->mir()->scope()));

  using Fn =
      ClassBodyLexicalEnvironmentObject* (*)(JSContext*,
                                             Handle<ClassBodyScope*>,
                                             HandleObject, gc::InitialHeap);
  callVM<Fn, ClassBodyLexicalEnvironmentObject::create>(lir);
}

void CodeGenerator::visitCopyLexicalEnvironmentObject(
    LCopyLexicalEnvironmentObject* lir) {
  pushArg(Imm32(lir->mir()->copySlots()));
  pushArg(ToRegister(lir->env()));

  using Fn = JSObject* (*)(JSContext*, HandleObject, bool);
  callVM<Fn, jit::CopyLexicalEnvironmentObject>(lir);
}

void CodeGenerator::visitGuardShape(LGuardShape* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToTempRegisterOrInvalid(guard->temp());
  Label bail;
  masm.branchTestObjShape(Assembler::NotEqual, obj, guard->mir()->shape(), temp,
                          obj, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardProto(LGuardProto* guard) {
  Register obj = ToRegister(guard->object());
  Register expected = ToRegister(guard->expected());
  Register temp = ToRegister(guard->temp());

  masm.loadObjProto(obj, temp);

  Label bail;
  masm.branchPtr(Assembler::NotEqual, temp, expected, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardNullProto(LGuardNullProto* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToRegister(guard->temp());

  masm.loadObjProto(obj, temp);

  Label bail;
  masm.branchTestPtr(Assembler::NonZero, temp, temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsNativeObject(LGuardIsNativeObject* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToRegister(guard->temp());

  Label bail;
  masm.branchIfNonNativeObj(obj, temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsProxy(LGuardIsProxy* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToRegister(guard->temp());

  Label bail;
  masm.branchTestObjectIsProxy(false, obj, temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsNotProxy(LGuardIsNotProxy* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToRegister(guard->temp());

  Label bail;
  masm.branchTestObjectIsProxy(true, obj, temp, &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsNotDOMProxy(LGuardIsNotDOMProxy* guard) {
  Register proxy = ToRegister(guard->proxy());
  Register temp = ToRegister(guard->temp());

  Label bail;
  masm.branchTestProxyHandlerFamily(Assembler::Equal, proxy, temp,
                                    GetDOMProxyHandlerFamily(), &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitProxyGet(LProxyGet* lir) {
  Register proxy = ToRegister(lir->proxy());
  Register temp = ToRegister(lir->temp());

  pushArg(lir->mir()->id(), temp);
  pushArg(proxy);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId, MutableHandleValue);
  callVM<Fn, ProxyGetProperty>(lir);
}

void CodeGenerator::visitProxyGetByValue(LProxyGetByValue* lir) {
  Register proxy = ToRegister(lir->proxy());
  ValueOperand idVal = ToValue(lir, LProxyGetByValue::IdIndex);

  pushArg(idVal);
  pushArg(proxy);

  using Fn =
      bool (*)(JSContext*, HandleObject, HandleValue, MutableHandleValue);
  callVM<Fn, ProxyGetPropertyByValue>(lir);
}

void CodeGenerator::visitProxyHasProp(LProxyHasProp* lir) {
  Register proxy = ToRegister(lir->proxy());
  ValueOperand idVal = ToValue(lir, LProxyHasProp::IdIndex);

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
  ValueOperand rhs = ToValue(lir, LProxySet::RhsIndex);
  Register temp = ToRegister(lir->temp());

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(rhs);
  pushArg(lir->mir()->id(), temp);
  pushArg(proxy);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId, HandleValue, bool);
  callVM<Fn, ProxySetProperty>(lir);
}

void CodeGenerator::visitProxySetByValue(LProxySetByValue* lir) {
  Register proxy = ToRegister(lir->proxy());
  ValueOperand idVal = ToValue(lir, LProxySetByValue::IdIndex);
  ValueOperand rhs = ToValue(lir, LProxySetByValue::RhsIndex);

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(rhs);
  pushArg(idVal);
  pushArg(proxy);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue, bool);
  callVM<Fn, ProxySetPropertyByValue>(lir);
}

void CodeGenerator::visitCallSetArrayLength(LCallSetArrayLength* lir) {
  Register obj = ToRegister(lir->obj());
  ValueOperand rhs = ToValue(lir, LCallSetArrayLength::RhsIndex);

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(rhs);
  pushArg(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, bool);
  callVM<Fn, jit::SetArrayLength>(lir);
}

void CodeGenerator::visitMegamorphicLoadSlot(LMegamorphicLoadSlot* lir) {
  Register obj = ToRegister(lir->object());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register temp3 = ToRegister(lir->temp3());
  ValueOperand output = ToOutValue(lir);

  Label bail;
  masm.branchIfNonNativeObj(obj, temp1, &bail);

  masm.pushValue(UndefinedValue());
  masm.moveStackPtrTo(temp3);

  using Fn =
      bool (*)(JSContext * cx, JSObject * obj, PropertyName * name, Value * vp);
  masm.setupUnalignedABICall(temp1);
  masm.loadJSContext(temp1);
  masm.passABIArg(temp1);
  masm.passABIArg(obj);
  masm.movePtr(ImmGCPtr(lir->mir()->name()), temp2);
  masm.passABIArg(temp2);
  masm.passABIArg(temp3);

  masm.callWithABI<Fn, GetNativeDataPropertyPure>();

  MOZ_ASSERT(!output.aliases(ReturnReg));
  masm.popValue(output);

  masm.branchIfFalseBool(ReturnReg, &bail);

  if (JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitMegamorphicLoadSlotByValue(
    LMegamorphicLoadSlotByValue* lir) {
  Register obj = ToRegister(lir->object());
  ValueOperand idVal = ToValue(lir, LMegamorphicLoadSlotByValue::IdIndex);
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  ValueOperand output = ToOutValue(lir);

  Label bail;
  masm.branchIfNonNativeObj(obj, temp1, &bail);

  // idVal will be in vp[0], result will be stored in vp[1].
  masm.subFromStackPtr(Imm32(sizeof(Value)));
  masm.pushValue(idVal);
  masm.moveStackPtrTo(temp1);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, Value * vp);
  masm.setupUnalignedABICall(temp2);
  masm.loadJSContext(temp2);
  masm.passABIArg(temp2);
  masm.passABIArg(obj);
  masm.passABIArg(temp1);
  masm.callWithABI<Fn, GetNativeDataPropertyByValuePure>();

  MOZ_ASSERT(!idVal.aliases(temp1));
  masm.mov(ReturnReg, temp1);
  masm.popValue(idVal);

  Label ok;
  masm.branchIfTrueBool(temp1, &ok);
  masm.addToStackPtr(Imm32(sizeof(Value)));  // Discard result Value.
  masm.jump(&bail);

  masm.bind(&ok);
  if (JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }
  masm.popValue(output);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitMegamorphicStoreSlot(LMegamorphicStoreSlot* lir) {
  Register obj = ToRegister(lir->object());
  ValueOperand rhs = ToValue(lir, LMegamorphicStoreSlot::RhsIndex);
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register temp3 = ToRegister(lir->temp3());

  masm.pushValue(rhs);
  masm.moveStackPtrTo(temp1);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, PropertyName * name,
                      Value * val);
  masm.setupUnalignedABICall(temp2);
  masm.loadJSContext(temp2);
  masm.passABIArg(temp2);
  masm.passABIArg(obj);
  masm.movePtr(ImmGCPtr(lir->mir()->name()), temp3);
  masm.passABIArg(temp3);
  masm.passABIArg(temp1);
  masm.callWithABI<Fn, SetNativeDataPropertyPure>();

  MOZ_ASSERT(!rhs.aliases(temp1));
  masm.mov(ReturnReg, temp1);
  masm.popValue(rhs);

  Label bail;
  masm.branchIfFalseBool(temp1, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitMegamorphicHasProp(LMegamorphicHasProp* lir) {
  Register obj = ToRegister(lir->object());
  ValueOperand idVal = ToValue(lir, LMegamorphicHasProp::IdIndex);
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register output = ToRegister(lir->output());

  // idVal will be in vp[0], result will be stored in vp[1].
  masm.subFromStackPtr(Imm32(sizeof(Value)));
  masm.pushValue(idVal);
  masm.moveStackPtrTo(temp1);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, Value * vp);
  masm.setupUnalignedABICall(temp2);
  masm.loadJSContext(temp2);
  masm.passABIArg(temp2);
  masm.passABIArg(obj);
  masm.passABIArg(temp1);
  if (lir->mir()->hasOwn()) {
    masm.callWithABI<Fn, HasNativeDataPropertyPure<true>>();
  } else {
    masm.callWithABI<Fn, HasNativeDataPropertyPure<false>>();
  }

  MOZ_ASSERT(!idVal.aliases(temp1));
  masm.mov(ReturnReg, temp1);
  masm.popValue(idVal);

  Label bail, ok;
  masm.branchIfTrueBool(temp1, &ok);
  masm.addToStackPtr(Imm32(sizeof(Value)));  // Discard result Value.
  masm.jump(&bail);

  masm.bind(&ok);
  masm.unboxBoolean(Address(masm.getStackPointer(), 0), output);
  masm.addToStackPtr(Imm32(sizeof(Value)));

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardIsNotArrayBufferMaybeShared(
    LGuardIsNotArrayBufferMaybeShared* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToRegister(guard->temp());

  Label bail;
  masm.loadObjClassUnsafe(obj, temp);
  masm.branchPtr(Assembler::Equal, temp, ImmPtr(&ArrayBufferObject::class_),
                 &bail);
  masm.branchPtr(Assembler::Equal, temp,
                 ImmPtr(&SharedArrayBufferObject::class_), &bail);
  bailoutFrom(&bail, guard->snapshot());
}

void CodeGenerator::visitGuardIsTypedArray(LGuardIsTypedArray* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToRegister(guard->temp());

  Label bail;
  masm.loadObjClassUnsafe(obj, temp);
  masm.branchIfClassIsNotTypedArray(temp, &bail);
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
  Register scratch = ToRegister(guard->temp());

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

void CodeGenerator::visitGuardStringToIndex(LGuardStringToIndex* lir) {
  Register str = ToRegister(lir->string());
  Register output = ToRegister(lir->output());

  Label bail, vmCall, done;
  masm.loadStringIndexValue(str, output, &vmCall);
  masm.jump(&done);

  {
    masm.bind(&vmCall);

    LiveRegisterSet volatileRegs = liveVolatileRegs(lir);
    volatileRegs.takeUnchecked(output);
    masm.PushRegsInMask(volatileRegs);

    using Fn = int32_t (*)(JSString * str);
    masm.setupUnalignedABICall(output);
    masm.passABIArg(str);
    masm.callWithABI<Fn, GetIndexFromString>();
    masm.storeCallInt32Result(output);

    masm.PopRegsInMask(volatileRegs);

    // GetIndexFromString returns a negative value on failure.
    masm.branchTest32(Assembler::Signed, output, output, &bail);
  }

  masm.bind(&done);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardStringToInt32(LGuardStringToInt32* lir) {
  Register str = ToRegister(lir->string());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp());

  LiveRegisterSet volatileRegs = liveVolatileRegs(lir);

  Label bail;
  masm.guardStringToInt32(str, output, temp, volatileRegs, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardStringToDouble(LGuardStringToDouble* lir) {
  Register str = ToRegister(lir->string());
  FloatRegister output = ToFloatRegister(lir->output());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());

  Label bail, vmCall, done;
  // Use indexed value as fast path if possible.
  masm.loadStringIndexValue(str, temp1, &vmCall);
  masm.convertInt32ToDouble(temp1, output);
  masm.jump(&done);
  {
    masm.bind(&vmCall);

    // Reserve stack for holding the result value of the call.
    masm.reserveStack(sizeof(double));
    masm.moveStackPtrTo(temp1);

    LiveRegisterSet volatileRegs = liveVolatileRegs(lir);
    volatileRegs.takeUnchecked(temp1);
    volatileRegs.takeUnchecked(temp2);
    masm.PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSContext * cx, JSString * str, double* result);
    masm.setupUnalignedABICall(temp2);
    masm.loadJSContext(temp2);
    masm.passABIArg(temp2);
    masm.passABIArg(str);
    masm.passABIArg(temp1);
    masm.callWithABI<Fn, StringToNumberPure>();
    masm.mov(ReturnReg, temp1);

    masm.PopRegsInMask(volatileRegs);

    Label ok;
    masm.branchIfTrueBool(temp1, &ok);
    {
      // OOM path, recovered by StringToNumberPure.
      //
      // Use addToStackPtr instead of freeStack as freeStack tracks stack height
      // flow-insensitively, and using it here would confuse the stack height
      // tracking.
      masm.addToStackPtr(Imm32(sizeof(double)));
      masm.jump(&bail);
    }
    masm.bind(&ok);
    masm.Pop(output);
  }
  masm.bind(&done);

  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardNoDenseElements(LGuardNoDenseElements* guard) {
  Register obj = ToRegister(guard->input());
  Register temp = ToRegister(guard->temp());

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
  ValueOperand input = ToValue(lir, LValueToInt64::Input);
  Register temp = ToRegister(lir->temp());
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
                                     Register maybeTemp) {
  OutOfLineCode* ool = createBigIntOutOfLine(lir, type, input, output);

  if (maybeTemp != InvalidReg) {
    masm.newGCBigInt(output, maybeTemp, ool->entry(), bigIntsCanBeInNursery());
  } else {
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(input);
    regs.take(output);

    Register temp = regs.takeAny();

    masm.push(temp);

    Label fail, ok;
    masm.newGCBigInt(output, temp, &fail, bigIntsCanBeInNursery());
    masm.pop(temp);
    masm.jump(&ok);
    masm.bind(&fail);
    masm.pop(temp);
    masm.jump(ool->entry());
    masm.bind(&ok);
  }
  masm.initializeBigInt64(type, output, input);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitInt64ToBigInt(LInt64ToBigInt* lir) {
  Register64 input = ToRegister64(lir->input());
  Register temp = ToRegister(lir->temp());
  Register output = ToRegister(lir->output());

  emitCreateBigInt(lir, Scalar::BigInt64, input, output, temp);
}

void CodeGenerator::visitGuardValue(LGuardValue* lir) {
  ValueOperand input = ToValue(lir, LGuardValue::Input);
  Value expected = lir->mir()->expected();
  Label bail;
  masm.branchTestValue(Assembler::NotEqual, input, expected, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardNullOrUndefined(LGuardNullOrUndefined* lir) {
  ValueOperand input = ToValue(lir, LGuardNullOrUndefined::Input);

  ScratchTagScope tag(masm, input);
  masm.splitTagForTest(input, tag);

  Label done;
  masm.branchTestNull(Assembler::Equal, tag, &done);

  Label bail;
  masm.branchTestUndefined(Assembler::NotEqual, tag, &bail);
  bailoutFrom(&bail, lir->snapshot());

  masm.bind(&done);
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
  Register temp = ToRegister(lir->temp());

  Label bail;
  masm.branchIfNotFunctionIsNonBuiltinCtor(function, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardFunctionKind(LGuardFunctionKind* lir) {
  Register function = ToRegister(lir->function());
  Register temp = ToRegister(lir->temp());

  Assembler::Condition cond =
      lir->mir()->bailOnEquality() ? Assembler::Equal : Assembler::NotEqual;

  Label bail;
  masm.branchFunctionKind(cond, lir->mir()->expected(), function, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardFunctionScript(LGuardFunctionScript* lir) {
  Register function = ToRegister(lir->function());

  Label bail;
  Address scriptAddr(function, JSFunction::offsetOfBaseScript());
  masm.branchPtr(Assembler::NotEqual, scriptAddr,
                 ImmGCPtr(lir->mir()->expected()), &bail);
  bailoutFrom(&bail, lir->snapshot());
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

  // We already have a fast path to check whether a global is in the store
  // buffer.
  if (!isGlobal && maybeConstant) {
    EmitStoreBufferCheckForConstant(masm, &maybeConstant->asTenured(), regs,
                                    &exit, &callVM);
  }

  // Call into the VM to barrier the write.
  masm.bind(&callVM);

  Register runtimereg = regs.takeAny();
  masm.mov(ImmPtr(runtime), runtimereg);

  masm.setupUnalignedABICall(regs.takeAny());
  masm.passABIArg(runtimereg);
  masm.passABIArg(objreg);
  if (isGlobal) {
    using Fn = void (*)(JSRuntime * rt, GlobalObject * obj);
    masm.callWithABI<Fn, PostGlobalWriteBarrier>();
  } else {
    using Fn = void (*)(JSRuntime * rt, js::gc::Cell * obj);
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

  Register temp = ToTempRegisterOrInvalid(lir->temp());

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

  Register temp = ToTempRegisterOrInvalid(lir->temp());

  if (lir->object()->isConstant()) {
    // Constant nursery objects cannot appear here, see
    // LIRGenerator::visitPostWriteElementBarrier.
    MOZ_ASSERT(!IsInsideNursery(&lir->object()->toConstant()->toObject()));
  } else {
    masm.branchPtrInNurseryChunk(Assembler::Equal, ToRegister(lir->object()),
                                 temp, ool->rejoin());
  }

  maybeEmitGlobalBarrierCheck(lir->object(), ool);

  ValueOperand value = ToValue(lir, LPostBarrierType::Input);
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
  using Fn = void (*)(JSRuntime * rt, JSObject * obj, int32_t index);
  masm.setupUnalignedABICall(runtimereg);
  masm.mov(ImmPtr(gen->runtime), runtimereg);
  masm.passABIArg(runtimereg);
  masm.passABIArg(objreg);
  masm.passABIArg(indexreg);
  masm.callWithABI<Fn, PostWriteElementBarrier<IndexInBounds::Maybe>>();

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

void CodeGenerator::visitCallNative(LCallNative* call) {
  WrappedFunction* target = call->getSingleTarget();
  MOZ_ASSERT(target);
  MOZ_ASSERT(target->isNativeWithoutJitEntry());

  int callargslot = call->argslot();
  int unusedStack = StackOffsetOfPassedArg(callargslot);

  // Registers used for callWithABI() argument-passing.
  const Register argContextReg = ToRegister(call->getArgContextReg());
  const Register argUintNReg = ToRegister(call->getArgUintNReg());
  const Register argVpReg = ToRegister(call->getArgVpReg());

  // Misc. temporary registers.
  const Register tempReg = ToRegister(call->getTempReg());

  DebugOnly<uint32_t> initialStack = masm.framePushed();

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
  masm.Push(ObjectValue(*target->rawNativeJSFunction()));

  // Preload arguments into registers.
  masm.loadJSContext(argContextReg);
  masm.move32(Imm32(call->numActualArgs()), argUintNReg);
  masm.moveStackPtrTo(argVpReg);

  masm.Push(argUintNReg);

  if (call->mir()->maybeCrossRealm()) {
    masm.movePtr(ImmGCPtr(target->rawNativeJSFunction()), tempReg);
    masm.switchToObjectRealm(tempReg, tempReg);
  }

  // Construct native exit frame.
  uint32_t safepointOffset = masm.buildFakeExitFrame(tempReg);
  masm.enterFakeExitFrameForNative(argContextReg, tempReg,
                                   call->mir()->isConstructing());

  markSafepointAt(safepointOffset, call);

  if (JS::TraceLoggerSupported()) {
    emitTracelogStartEvent(TraceLogger_Call);
  }

  // Construct and execute call.
  masm.setupUnalignedABICall(tempReg);
  masm.passABIArg(argContextReg);
  masm.passABIArg(argUintNReg);
  masm.passABIArg(argVpReg);
  JSNative native = target->native();
  if (call->ignoresReturnValue() && target->hasJitInfo()) {
    const JSJitInfo* jitInfo = target->jitInfo();
    if (jitInfo->type() == JSJitInfo::IgnoresReturnValueNative) {
      native = jitInfo->ignoresReturnValueMethod;
    }
  }
  masm.callWithABI(DynamicFunction<JSNative>(native), MoveOp::GENERAL,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  if (JS::TraceLoggerSupported()) {
    emitTracelogStopEvent(TraceLogger_Call);
  }

  // Test for failure.
  masm.branchIfFalseBool(ReturnReg, masm.failureLabel());

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

  // The next instruction is removing the footer of the exit frame, so there
  // is no need for leaveFakeExitFrame.

  // Move the StackPointer back to its original location, unwinding the native
  // exit frame.
  masm.adjustStack(NativeExitFrameLayout::Size() - unusedStack);
  MOZ_ASSERT(masm.framePushed() == initialStack);
}

static void LoadDOMPrivate(MacroAssembler& masm, Register obj, Register priv,
                           DOMObjectKind kind) {
  // Load the value in DOM_OBJECT_SLOT for a native or proxy DOM object. This
  // will be in the first slot but may be fixed or non-fixed.
  MOZ_ASSERT(obj != priv);

  // Check if it's a proxy.
  Label isProxy, done;
  if (kind == DOMObjectKind::Unknown) {
    masm.branchTestObjectIsProxy(true, obj, priv, &isProxy);
  }

  if (kind != DOMObjectKind::Proxy) {
    // If it's a native object, the value must be in a fixed slot.
    masm.debugAssertObjHasFixedSlots(obj, priv);
    masm.loadPrivate(Address(obj, NativeObject::getFixedSlotOffset(0)), priv);
    if (kind == DOMObjectKind::Unknown) {
      masm.jump(&done);
    }
  }

  if (kind != DOMObjectKind::Native) {
    masm.bind(&isProxy);
#ifdef DEBUG
    // Sanity check: it must be a DOM proxy.
    Label isDOMProxy;
    masm.branchTestProxyHandlerFamily(Assembler::Equal, obj, priv,
                                      GetDOMProxyHandlerFamily(), &isDOMProxy);
    masm.assumeUnreachable("Expected a DOM proxy");
    masm.bind(&isDOMProxy);
#endif
    masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), priv);
    masm.loadPrivate(
        Address(priv, js::detail::ProxyReservedSlots::offsetOfSlot(0)), priv);
  }

  masm.bind(&done);
}

void CodeGenerator::visitCallDOMNative(LCallDOMNative* call) {
  WrappedFunction* target = call->getSingleTarget();
  MOZ_ASSERT(target);
  MOZ_ASSERT(target->isNativeWithoutJitEntry());
  MOZ_ASSERT(target->hasJitInfo());
  MOZ_ASSERT(call->mir()->isCallDOMNative());

  int callargslot = call->argslot();
  int unusedStack = StackOffsetOfPassedArg(callargslot);

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

  // Construct native exit frame.
  uint32_t safepointOffset = masm.buildFakeExitFrame(argJSContext);
  masm.loadJSContext(argJSContext);
  masm.enterFakeExitFrame(argJSContext, argJSContext,
                          ExitFrameType::IonDOMMethod);

  markSafepointAt(safepointOffset, call);

  // Construct and execute call.
  masm.setupUnalignedABICall(argJSContext);
  masm.loadJSContext(argJSContext);
  masm.passABIArg(argJSContext);
  masm.passABIArg(argObj);
  masm.passABIArg(argPrivate);
  masm.passABIArg(argArgs);
  masm.callWithABI(DynamicFunction<JSJitMethodOp>(target->jitInfo()->method),
                   MoveOp::GENERAL,
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

  // Switch back to the current realm if needed. Note: if the DOM method threw
  // an exception, the exception handler will do this.
  if (call->mir()->maybeCrossRealm()) {
    static_assert(!JSReturnOperand.aliases(ReturnReg),
                  "Clobbering ReturnReg should not affect the return value");
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
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

  using Fn = bool (*)(JSContext * cx, HandlePropertyName, MutableHandleValue);
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
  Register calleereg = ToRegister(call->getFunction());
  Register objreg = ToRegister(call->getTempObject());
  Register nargsreg = ToRegister(call->getNargsReg());
  uint32_t unusedStack = StackOffsetOfPassedArg(call->argslot());
  Label invoke, thunk, makeCall, end;

  // Known-target case is handled by LCallKnown.
  MOZ_ASSERT(!call->hasSingleTarget());

  masm.checkStackAlignment();

  // Guard that calleereg is actually a function object.
  if (call->mir()->needsClassCheck()) {
    masm.branchTestObjClass(Assembler::NotEqual, calleereg, &JSFunction::class_,
                            nargsreg, calleereg, &invoke);
  }

  // Guard that callee allows the [[Call]] or [[Construct]] operation required.
  if (call->mir()->isConstructing()) {
    masm.branchTestFunctionFlags(calleereg, FunctionFlags::CONSTRUCTOR,
                                 Assembler::Zero, &invoke);
  } else {
    masm.branchFunctionKind(Assembler::Equal, FunctionFlags::ClassConstructor,
                            calleereg, objreg, &invoke);
  }

  // Use the slow path if CreateThis was unable to create the |this| object.
  if (call->mir()->needsThisCheck()) {
    MOZ_ASSERT(call->mir()->isConstructing());
    Address thisAddr(masm.getStackPointer(), unusedStack);
    masm.branchTestNull(Assembler::Equal, thisAddr, &invoke);
  } else {
#ifdef DEBUG
    if (call->mir()->isConstructing()) {
      Address thisAddr(masm.getStackPointer(), unusedStack);
      Label ok;
      masm.branchTestNull(Assembler::NotEqual, thisAddr, &ok);
      masm.assumeUnreachable("Unexpected null this-value");
      masm.bind(&ok);
    }
#endif
  }

  // Load jitCodeRaw for callee if it exists.
  masm.branchIfFunctionHasNoJitEntry(calleereg, call->mir()->isConstructing(),
                                     &invoke);
  masm.loadJitCodeRaw(calleereg, objreg);

  // Target may be a different realm even if same compartment.
  if (call->mir()->maybeCrossRealm()) {
    masm.switchToObjectRealm(calleereg, nargsreg);
  }

  // Nestle the StackPointer up to the argument vector.
  masm.freeStack(unusedStack);

  // Construct the IonFramePrefix.
  uint32_t descriptor = MakeFrameDescriptor(
      masm.framePushed(), FrameType::IonJS, JitFrameLayout::Size());
  masm.Push(Imm32(call->numActualArgs()));
  masm.PushCalleeToken(calleereg, call->mir()->isConstructing());
  masm.Push(Imm32(descriptor));

  // Check whether the provided arguments satisfy target argc.
  // We cannot have lowered to LCallGeneric with a known target. Assert that we
  // didn't add any undefineds in WarpBuilder. NB: MCall::numStackArgs includes
  // |this|.
  DebugOnly<unsigned> numNonArgsOnStack = 1 + call->isConstructing();
  MOZ_ASSERT(call->numActualArgs() ==
             call->mir()->numStackArgs() - numNonArgsOnStack);
  masm.load16ZeroExtend(Address(calleereg, JSFunction::offsetOfNargs()),
                        nargsreg);
  masm.branch32(Assembler::Above, nargsreg, Imm32(call->numActualArgs()),
                &thunk);
  masm.jump(&makeCall);

  // Argument fixup needed. Load the ArgumentsRectifier.
  masm.bind(&thunk);
  {
    TrampolinePtr argumentsRectifier =
        gen->jitRuntime()->getArgumentsRectifier();
    masm.movePtr(argumentsRectifier, objreg);
  }

  // Finally call the function in objreg.
  masm.bind(&makeCall);
  uint32_t callOffset = masm.callJit(objreg);
  markSafepointAt(callOffset, call);

  if (call->mir()->maybeCrossRealm()) {
    static_assert(!JSReturnOperand.aliases(ReturnReg),
                  "ReturnReg available as scratch after scripted calls");
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
  }

  // Increment to remove IonFramePrefix; decrement to fill FrameSizeClass.
  // The return address has already been removed from the Ion frame.
  int prefixGarbage = sizeof(JitFrameLayout) - sizeof(void*);
  masm.adjustStack(prefixGarbage - unusedStack);
  masm.jump(&end);

  // Handle uncompiled or native functions.
  masm.bind(&invoke);
  emitCallInvokeFunction(call, calleereg, call->isConstructing(),
                         call->ignoresReturnValue(), call->numActualArgs(),
                         unusedStack);

  masm.bind(&end);

  // If the return value of the constructing function is Primitive,
  // replace the return value with the Object from CreateThis.
  if (call->mir()->isConstructing()) {
    Label notPrimitive;
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.loadValue(Address(masm.getStackPointer(), unusedStack),
                   JSReturnOperand);
    masm.bind(&notPrimitive);
  }
}

void CodeGenerator::visitCallKnown(LCallKnown* call) {
  Register calleereg = ToRegister(call->getFunction());
  Register objreg = ToRegister(call->getTempObject());
  uint32_t unusedStack = StackOffsetOfPassedArg(call->argslot());
  WrappedFunction* target = call->getSingleTarget();

  // Native single targets (except wasm) are handled by LCallNative.
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

  // Construct the IonFramePrefix.
  uint32_t descriptor = MakeFrameDescriptor(
      masm.framePushed(), FrameType::IonJS, JitFrameLayout::Size());
  masm.Push(Imm32(call->numActualArgs()));
  masm.PushCalleeToken(calleereg, call->mir()->isConstructing());
  masm.Push(Imm32(descriptor));

  // Finally call the function in objreg.
  uint32_t callOffset = masm.callJit(objreg);
  markSafepointAt(callOffset, call);

  if (call->mir()->maybeCrossRealm()) {
    static_assert(!JSReturnOperand.aliases(ReturnReg),
                  "ReturnReg available as scratch after scripted calls");
    masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
  }

  // Increment to remove IonFramePrefix; decrement to fill FrameSizeClass.
  // The return address has already been removed from the Ion frame.
  int prefixGarbage = sizeof(JitFrameLayout) - sizeof(void*);
  masm.adjustStack(prefixGarbage - unusedStack);

  // If the return value of the constructing function is Primitive,
  // replace the return value with the Object from CreateThis.
  if (call->mir()->isConstructing()) {
    Label notPrimitive;
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.loadValue(Address(masm.getStackPointer(), unusedStack),
                   JSReturnOperand);
    masm.bind(&notPrimitive);
  }
}

template <typename T>
void CodeGenerator::emitCallInvokeFunction(T* apply, Register extraStackSize) {
  Register objreg = ToRegister(apply->getTempObject());
  MOZ_ASSERT(objreg != extraStackSize);

  // Push the space used by the arguments.
  masm.moveStackPtrTo(objreg);
  masm.Push(extraStackSize);

  pushArg(objreg);                                     // argv.
  pushArg(ToRegister(apply->getArgc()));               // argc.
  pushArg(Imm32(apply->mir()->ignoresReturnValue()));  // ignoresReturnValue.
  pushArg(Imm32(apply->mir()->isConstructing()));      // isConstructing.
  pushArg(ToRegister(apply->getFunction()));           // JSFunction*.

  // This specialization of callVM restores the extraStackSize after the call.
  using Fn = bool (*)(JSContext*, HandleObject, bool, bool, uint32_t, Value*,
                      MutableHandleValue);
  callVM<Fn, jit::InvokeFunction>(apply, &extraStackSize);

  masm.Pop(extraStackSize);
}

// Do not bailout after the execution of this function since the stack no longer
// correspond to what is expected by the snapshots.
void CodeGenerator::emitAllocateSpaceForApply(Register argcreg,
                                              Register extraStackSpace) {
  // Initialize the loop counter AND Compute the stack usage (if == 0)
  masm.movePtr(argcreg, extraStackSpace);

  // Align the JitFrameLayout on the JitStackAlignment.
  if (JitStackValueAlignment > 1) {
    MOZ_ASSERT(frameSize() % JitStackAlignment == 0,
               "Stack padding assumes that the frameSize is correct");
    MOZ_ASSERT(JitStackValueAlignment == 2);
    Label noPaddingNeeded;
    // if the number of arguments is odd, then we do not need any padding.
    masm.branchTestPtr(Assembler::NonZero, argcreg, Imm32(1), &noPaddingNeeded);
    masm.addPtr(Imm32(1), extraStackSpace);
    masm.bind(&noPaddingNeeded);
  }

  // Reserve space for copying the arguments.
  NativeObject::elementsSizeMustNotOverflow();
  masm.lshiftPtr(Imm32(ValueShift), extraStackSpace);
  masm.subFromStackPtr(extraStackSpace);

#ifdef DEBUG
  // Put a magic value in the space reserved for padding. Note, this code
  // cannot be merged with the previous test, as not all architectures can
  // write below their stack pointers.
  if (JitStackValueAlignment > 1) {
    MOZ_ASSERT(JitStackValueAlignment == 2);
    Label noPaddingNeeded;
    // if the number of arguments is odd, then we do not need any padding.
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
    Register argcreg, Register newTargetAndExtraStackSpace) {
  // Align the JitFrameLayout on the JitStackAlignment. Contrary to
  // |emitAllocateSpaceForApply()|, we're always pushing a magic value, because
  // we can't write to |newTargetAndExtraStackSpace| before |new.target| has
  // been pushed onto the stack.
  if (JitStackValueAlignment > 1) {
    MOZ_ASSERT(frameSize() % JitStackAlignment == 0,
               "Stack padding assumes that the frameSize is correct");
    MOZ_ASSERT(JitStackValueAlignment == 2);

    Label noPaddingNeeded;
    // If the number of arguments is even, then we do not need any padding.
    masm.branchTestPtr(Assembler::Zero, argcreg, Imm32(1), &noPaddingNeeded);
    masm.pushValue(MagicValue(JS_ARG_POISON));
    masm.bind(&noPaddingNeeded);
  }

  // Push |new.target| after the padding value, but before any arguments.
  masm.pushValue(JSVAL_TYPE_OBJECT, newTargetAndExtraStackSpace);

  // Initialize the loop counter AND compute the stack usage.
  masm.movePtr(argcreg, newTargetAndExtraStackSpace);

  // Reserve space for copying the arguments.
  NativeObject::elementsSizeMustNotOverflow();
  masm.lshiftPtr(Imm32(ValueShift), newTargetAndExtraStackSpace);
  masm.subFromStackPtr(newTargetAndExtraStackSpace);

  // Account for |new.target| which has already been pushed onto the stack.
  masm.addPtr(Imm32(sizeof(Value)), newTargetAndExtraStackSpace);

  // And account for the padding.
  if (JitStackValueAlignment > 1) {
    MOZ_ASSERT(frameSize() % JitStackAlignment == 0,
               "Stack padding assumes that the frameSize is correct");
    MOZ_ASSERT(JitStackValueAlignment == 2);

    Label noPaddingNeeded;
    // If the number of arguments is even, then we do not need any padding.
    masm.branchTestPtr(Assembler::Zero, argcreg, Imm32(1), &noPaddingNeeded);
    masm.addPtr(Imm32(sizeof(Value)), newTargetAndExtraStackSpace);
    masm.bind(&noPaddingNeeded);
  }
}

// Destroys argvIndex and copyreg.
void CodeGenerator::emitCopyValuesForApply(Register argvSrcBase,
                                           Register argvIndex, Register copyreg,
                                           size_t argvSrcOffset,
                                           size_t argvDstOffset) {
  Label loop;
  masm.bind(&loop);

  // As argvIndex is off by 1, and we use the decBranchPtr instruction
  // to loop back, we have to substract the size of the word which are
  // copied.
  BaseValueIndex srcPtr(argvSrcBase, argvIndex, argvSrcOffset - sizeof(void*));
  BaseValueIndex dstPtr(masm.getStackPointer(), argvIndex,
                        argvDstOffset - sizeof(void*));
  masm.loadPtr(srcPtr, copyreg);
  masm.storePtr(copyreg, dstPtr);

  // Handle 32 bits architectures.
  if (sizeof(Value) == 2 * sizeof(void*)) {
    BaseValueIndex srcPtrLow(argvSrcBase, argvIndex,
                             argvSrcOffset - 2 * sizeof(void*));
    BaseValueIndex dstPtrLow(masm.getStackPointer(), argvIndex,
                             argvDstOffset - 2 * sizeof(void*));
    masm.loadPtr(srcPtrLow, copyreg);
    masm.storePtr(copyreg, dstPtrLow);
  }

  masm.decBranchPtr(Assembler::NonZero, argvIndex, Imm32(1), &loop);
}

void CodeGenerator::emitPopArguments(Register extraStackSpace) {
  // Pop |this| and Arguments.
  masm.freeStack(extraStackSpace);
}

void CodeGenerator::emitPushArguments(LApplyArgsGeneric* apply,
                                      Register extraStackSpace) {
  // Holds the function nargs. Initially the number of args to the caller.
  Register argcreg = ToRegister(apply->getArgc());
  Register copyreg = ToRegister(apply->getTempObject());

  Label end;
  emitAllocateSpaceForApply(argcreg, extraStackSpace);

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
  size_t argvSrcOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();
  size_t argvDstOffset = 0;

  // Save the extra stack space, and re-use the register as a base.
  masm.push(extraStackSpace);
  Register argvSrcBase = extraStackSpace;
  argvSrcOffset += sizeof(void*);
  argvDstOffset += sizeof(void*);

  // Save the actual number of register, and re-use the register as an index
  // register.
  masm.push(argcreg);
  Register argvIndex = argcreg;
  argvSrcOffset += sizeof(void*);
  argvDstOffset += sizeof(void*);

  // srcPtr = (StackPointer + extraStackSpace) + argvSrcOffset
  // dstPtr = (StackPointer                  ) + argvDstOffset
  masm.addStackPtrTo(argvSrcBase);

  // Copy arguments.
  emitCopyValuesForApply(argvSrcBase, argvIndex, copyreg, argvSrcOffset,
                         argvDstOffset);

  // Restore argcreg and the extra stack space counter.
  masm.pop(argcreg);
  masm.pop(extraStackSpace);

  // Join with all arguments copied and the extra stack usage computed.
  masm.bind(&end);

  // Push |this|.
  masm.addPtr(Imm32(sizeof(Value)), extraStackSpace);
  masm.pushValue(ToValue(apply, LApplyArgsGeneric::ThisIndex));
}

void CodeGenerator::emitPushArguments(LApplyArgsObj* apply,
                                      Register extraStackSpace) {
  // argc and argsObj are mapped to the same calltemp register.
  MOZ_ASSERT(apply->getArgsObj() == apply->getArgc());

  Register tmpArgc = ToRegister(apply->getTempObject());
  Register argsObj = ToRegister(apply->getArgsObj());

  // Load argc into tmpArgc.
  Address lengthAddr(argsObj, ArgumentsObject::getInitialLengthSlotOffset());
  masm.unboxInt32(lengthAddr, tmpArgc);
  masm.rshift32(Imm32(ArgumentsObject::PACKED_BITS_COUNT), tmpArgc);

  // Allocate space on the stack for arguments. This modifies extraStackSpace.
  emitAllocateSpaceForApply(tmpArgc, extraStackSpace);

  // Load arguments data
  masm.loadPrivate(Address(argsObj, ArgumentsObject::getDataSlotOffset()),
                   argsObj);
  size_t argsSrcOffset = ArgumentsData::offsetOfArgs();

  // This is the end of the lifetime of argsObj.
  // After this call, the argsObj register holds the argument count instead.
  emitPushArrayAsArguments(tmpArgc, argsObj, extraStackSpace, argsSrcOffset);

  masm.addPtr(Imm32(sizeof(Value)), extraStackSpace);
  masm.pushValue(ToValue(apply, LApplyArgsObj::ThisIndex));
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
  // |scratch| is used as a temp register within this function. It is
  // restored before returning.

  Label noCopy, epilogue;

  // Skip the copy of arguments if there are none.
  masm.branchTestPtr(Assembler::Zero, tmpArgc, tmpArgc, &noCopy);

  // Copy the values.  This code is skipped entirely if there are
  // no values.
  size_t argvDstOffset = 0;

  Register argvSrcBase = srcBaseAndArgc;

  masm.push(scratch);
  Register copyreg = scratch;
  argvDstOffset += sizeof(void*);

  masm.push(tmpArgc);
  Register argvIndex = tmpArgc;
  argvDstOffset += sizeof(void*);

  // Copy
  emitCopyValuesForApply(argvSrcBase, argvIndex, copyreg, argvSrcOffset,
                         argvDstOffset);

  // Restore.
  masm.pop(srcBaseAndArgc);  // srcBaseAndArgc now contains argc.
  masm.pop(scratch);
  masm.jump(&epilogue);

  // Clear argc if we skipped the copy step.
  masm.bind(&noCopy);
  masm.movePtr(ImmWord(0), srcBaseAndArgc);

  // Join with all arguments copied and the extra stack usage computed.
  // Note, "srcBase" has become "argc".
  masm.bind(&epilogue);
}

void CodeGenerator::emitPushArguments(LApplyArrayGeneric* apply,
                                      Register extraStackSpace) {
  Register tmpArgc = ToRegister(apply->getTempObject());
  Register elementsAndArgc = ToRegister(apply->getElements());

  // Invariants guarded in the caller:
  //  - the array is not too long
  //  - the array length equals its initialized length

  // The array length is our argc for the purposes of allocating space.
  Address length(ToRegister(apply->getElements()),
                 ObjectElements::offsetOfLength());
  masm.load32(length, tmpArgc);

  // Allocate space for the values.
  emitAllocateSpaceForApply(tmpArgc, extraStackSpace);

  // After this call "elements" has become "argc".
  size_t elementsOffset = 0;
  emitPushArrayAsArguments(tmpArgc, elementsAndArgc, extraStackSpace,
                           elementsOffset);

  // Push |this|.
  masm.addPtr(Imm32(sizeof(Value)), extraStackSpace);
  masm.pushValue(ToValue(apply, LApplyArrayGeneric::ThisIndex));
}

void CodeGenerator::emitPushArguments(LConstructArrayGeneric* construct,
                                      Register extraStackSpace) {
  MOZ_ASSERT(extraStackSpace == ToRegister(construct->getNewTarget()));

  Register tmpArgc = ToRegister(construct->getTempObject());
  Register elementsAndArgc = ToRegister(construct->getElements());

  // Invariants guarded in the caller:
  //  - the array is not too long
  //  - the array length equals its initialized length

  // The array length is our argc for the purposes of allocating space.
  Address length(ToRegister(construct->getElements()),
                 ObjectElements::offsetOfLength());
  masm.load32(length, tmpArgc);

  // Allocate space for the values.
  emitAllocateSpaceForConstructAndPushNewTarget(tmpArgc, extraStackSpace);

  // After this call "elements" has become "argc" and "newTarget" has become
  // "extraStackSpace".
  size_t elementsOffset = 0;
  emitPushArrayAsArguments(tmpArgc, elementsAndArgc, extraStackSpace,
                           elementsOffset);

  // Push |this|.
  masm.addPtr(Imm32(sizeof(Value)), extraStackSpace);
  masm.pushValue(ToValue(construct, LConstructArrayGeneric::ThisIndex));
}

template <typename T>
void CodeGenerator::emitApplyGeneric(T* apply) {
  // Holds the function object.
  Register calleereg = ToRegister(apply->getFunction());

  // Temporary register for modifying the function object.
  Register objreg = ToRegister(apply->getTempObject());
  Register extraStackSpace = ToRegister(apply->getTempStackCounter());

  // Holds the function nargs, computed in the invoker or (for ApplyArray,
  // ConstructArray, or ApplyArgsObj) in the argument pusher.
  Register argcreg = ToRegister(apply->getArgc());

  // Copy the arguments of the current function.
  //
  // In the case of ApplyArray, ConstructArray, or ApplyArgsObj, also
  // compute argc. The argc register and the elements/argsObj register
  // are the same; argc must not be referenced before the call to
  // emitPushArguments() and elements/argsObj must not be referenced
  // after it returns.
  //
  // In the case of ConstructArray, also overwrite newTarget with
  // extraStackSpace; newTarget must not be referenced after this point.
  //
  // objreg is dead across this call.
  //
  // extraStackSpace is garbage on entry (for ApplyArray and ApplyArgs) and
  // defined on exit.
  emitPushArguments(apply, extraStackSpace);

  masm.checkStackAlignment();

  bool constructing = apply->mir()->isConstructing();

  // If the function is native, only emit the call to InvokeFunction.
  if (apply->hasSingleTarget() &&
      apply->getSingleTarget()->isNativeWithoutJitEntry()) {
    emitCallInvokeFunction(apply, extraStackSpace);

#ifdef DEBUG
    // Native constructors are guaranteed to return an Object value, so we never
    // have to replace a primitive result with the previously allocated Object
    // from CreateThis.
    if (constructing) {
      Label notPrimitive;
      masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                               &notPrimitive);
      masm.assumeUnreachable("native constructors don't return primitives");
      masm.bind(&notPrimitive);
    }
#endif

    emitPopArguments(extraStackSpace);
    return;
  }

  Label end, invoke;

  // Unless already known, guard that calleereg is actually a function object.
  if (!apply->hasSingleTarget()) {
    masm.branchTestObjClass(Assembler::NotEqual, calleereg, &JSFunction::class_,
                            objreg, calleereg, &invoke);
  }

  // Guard that calleereg is an interpreted function with a JSScript.
  masm.branchIfFunctionHasNoJitEntry(calleereg, constructing, &invoke);

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

    // Create the frame descriptor.
    unsigned pushed = masm.framePushed();
    Register stackSpace = extraStackSpace;
    masm.addPtr(Imm32(pushed), stackSpace);
    masm.makeFrameDescriptor(stackSpace, FrameType::IonJS,
                             JitFrameLayout::Size());

    masm.Push(argcreg);
    masm.PushCalleeToken(calleereg, constructing);
    masm.Push(stackSpace);  // descriptor

    Label underflow, rejoin;

    // Check whether the provided arguments satisfy target argc.
    if (!apply->hasSingleTarget()) {
      Register nformals = extraStackSpace;
      masm.load16ZeroExtend(Address(calleereg, JSFunction::offsetOfNargs()),
                            nformals);
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
    uint32_t callOffset = masm.callJit(objreg);
    markSafepointAt(callOffset, apply);

    if (apply->mir()->maybeCrossRealm()) {
      static_assert(!JSReturnOperand.aliases(ReturnReg),
                    "ReturnReg available as scratch after scripted calls");
      masm.switchToRealm(gen->realm->realmPtr(), ReturnReg);
    }

    // Recover the number of arguments from the frame descriptor.
    masm.loadPtr(Address(masm.getStackPointer(), 0), stackSpace);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), stackSpace);
    masm.subPtr(Imm32(pushed), stackSpace);

    // Increment to remove IonFramePrefix; decrement to fill FrameSizeClass.
    // The return address has already been removed from the Ion frame.
    int prefixGarbage = sizeof(JitFrameLayout) - sizeof(void*);
    masm.adjustStack(prefixGarbage);
    masm.jump(&end);
  }

  // Handle uncompiled or native functions.
  {
    masm.bind(&invoke);
    emitCallInvokeFunction(apply, extraStackSpace);
  }

  masm.bind(&end);

  // If the return value of the constructing function is Primitive,
  // replace the return value with the Object from CreateThis.
  if (constructing) {
    Label notPrimitive;
    masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand,
                             &notPrimitive);
    masm.loadValue(Address(masm.getStackPointer(), 0), JSReturnOperand);
    masm.bind(&notPrimitive);
  }

  // Pop arguments and continue.
  emitPopArguments(extraStackSpace);
}

void CodeGenerator::visitApplyArgsGeneric(LApplyArgsGeneric* apply) {
  LSnapshot* snapshot = apply->snapshot();
  Register argcreg = ToRegister(apply->getArgc());

  // Ensure that we have a reasonable number of arguments.
  bailoutCmp32(Assembler::Above, argcreg, Imm32(JIT_ARGS_LENGTH_MAX), snapshot);

  emitApplyGeneric(apply);
}

void CodeGenerator::visitApplyArgsObj(LApplyArgsObj* apply) {
  Register argsObj = ToRegister(apply->getArgsObj());
  Register temp = ToRegister(apply->getTempObject());

  Label bail;
  masm.loadArgumentsObjectLength(argsObj, temp, &bail);
  masm.branch32(Assembler::Above, temp, Imm32(JIT_ARGS_LENGTH_MAX), &bail);
  bailoutFrom(&bail, apply->snapshot());

  emitApplyGeneric(apply);
}

void CodeGenerator::visitApplyArrayGeneric(LApplyArrayGeneric* apply) {
  LSnapshot* snapshot = apply->snapshot();
  Register tmp = ToRegister(apply->getTempObject());

  Address length(ToRegister(apply->getElements()),
                 ObjectElements::offsetOfLength());
  masm.load32(length, tmp);

  // Ensure that we have a reasonable number of arguments.
  bailoutCmp32(Assembler::Above, tmp, Imm32(JIT_ARGS_LENGTH_MAX), snapshot);

  // Ensure that the array does not contain an uninitialized tail.

  Address initializedLength(ToRegister(apply->getElements()),
                            ObjectElements::offsetOfInitializedLength());
  masm.sub32(initializedLength, tmp);
  bailoutCmp32(Assembler::NotEqual, tmp, Imm32(0), snapshot);

  emitApplyGeneric(apply);
}

void CodeGenerator::visitConstructArrayGeneric(LConstructArrayGeneric* lir) {
  LSnapshot* snapshot = lir->snapshot();
  Register tmp = ToRegister(lir->getTempObject());

  Address length(ToRegister(lir->getElements()),
                 ObjectElements::offsetOfLength());
  masm.load32(length, tmp);

  // Ensure that we have a reasonable number of arguments.
  bailoutCmp32(Assembler::Above, tmp, Imm32(JIT_ARGS_LENGTH_MAX), snapshot);

  // Ensure that the array does not contain an uninitialized tail.

  Address initializedLength(ToRegister(lir->getElements()),
                            ObjectElements::offsetOfInitializedLength());
  masm.sub32(initializedLength, tmp);
  bailoutCmp32(Assembler::NotEqual, tmp, Imm32(0), snapshot);

  emitApplyGeneric(lir);
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

// Out-of-line path to report over-recursed error and fail.
class CheckOverRecursedFailure : public OutOfLineCodeBase<CodeGenerator> {
  LInstruction* lir_;

 public:
  explicit CheckOverRecursedFailure(LInstruction* lir) : lir_(lir) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitCheckOverRecursedFailure(this);
  }

  LInstruction* lir() const { return lir_; }
};

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

  CheckOverRecursedFailure* ool = new (alloc()) CheckOverRecursedFailure(lir);
  addOutOfLineCode(ool, lir->mir());

  // Conditional forward (unlikely) branch to failure.
  const void* limitAddr = gen->runtime->addressOfJitStackLimit();
  masm.branchStackPtrRhs(Assembler::AboveOrEqual, AbsoluteAddress(limitAddr),
                         ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckOverRecursedFailure(
    CheckOverRecursedFailure* ool) {
  // The OOL path is hit if the recursion depth has been exceeded.
  // Throw an InternalError for over-recursion.

  // LFunctionEnvironment can appear before LCheckOverRecursed, so we have
  // to save all live registers to avoid crashes if CheckOverRecursed triggers
  // a GC.
  saveLive(ool->lir());

  using Fn = bool (*)(JSContext*);
  callVM<Fn, CheckOverRecursed>(ool->lir());

  restoreLive(ool->lir());
  masm.jump(ool->rejoin());
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

    if (!printer.hadOutOfMemory()) {
      block.setCode(printer.string());
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
        using Fn = void (*)(JSContext * cx, JSObject * obj);
        masm.callWithABI<Fn, AssertValidObjectPtr>();
        break;
      }
      case MIRType::String: {
        using Fn = void (*)(JSContext * cx, JSString * str);
        masm.callWithABI<Fn, AssertValidStringPtr>();
        break;
      }
      case MIRType::Symbol: {
        using Fn = void (*)(JSContext * cx, JS::Symbol * sym);
        masm.callWithABI<Fn, AssertValidSymbolPtr>();
        break;
      }
      case MIRType::BigInt: {
        using Fn = void (*)(JSContext * cx, JS::BigInt * bi);
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

    using Fn = void (*)(JSContext * cx, Value * v);
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
  if (!lir->getDef(0)->output()->isRegister()) {
    return;
  }

  ValueOperand output = ToOutValue(lir);

  emitAssertResultV(output, mir);
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

  Label done, notBail, bail;
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
      masm.jump(&bail);
      bailoutFrom(&bail, lir->snapshot());
    }
    masm.bind(&notBail);
    masm.pop(temp);
  }
  masm.bind(&done);
}
#endif

bool CodeGenerator::generateBody() {
  JitSpewCont(JitSpew_Codegen, "\n");
  JitSpew(JitSpew_Codegen, "==== BEGIN CodeGenerator::generateBody ====");
  IonScriptCounts* counts = maybeCreateScriptCounts();

  const bool compilingWasm = gen->compilingWasm();

#if defined(JS_ION_PERF)
  PerfSpewer* perfSpewer = &perfSpewer_;
  if (compilingWasm) {
    perfSpewer = &gen->perfSpewer();
  }
#endif

  for (size_t i = 0; i < graph.numBlocks(); i++) {
    current = graph.getBlock(i);

    // Don't emit any code for trivial blocks, containing just a goto. Such
    // blocks are created to split critical edges, and if we didn't end up
    // putting any instructions in them, we can skip them.
    if (current->isTrivial()) {
      continue;
    }

#ifdef JS_JITSPEW
    const char* filename = nullptr;
    size_t lineNumber = 0;
    unsigned columnNumber = 0;
    if (current->mir()->info().script()) {
      filename = current->mir()->info().script()->filename();
      if (current->mir()->pc()) {
        lineNumber = PCToLineNumber(current->mir()->info().script(),
                                    current->mir()->pc(), &columnNumber);
      }
    } else {
#  ifdef DEBUG
      lineNumber = current->mir()->lineno();
      columnNumber = current->mir()->columnIndex();
#  endif
    }
    JitSpew(JitSpew_Codegen, "--------------------------------");
    JitSpew(JitSpew_Codegen, "# block%zu %s:%zu:%u%s:", i,
            filename ? filename : "?", lineNumber, columnNumber,
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

#if defined(JS_ION_PERF)
    if (!perfSpewer->startBasicBlock(current->mir(), masm)) {
      return false;
    }
#endif

    for (LInstructionIterator iter = current->begin(); iter != current->end();
         iter++) {
      if (!alloc().ensureBallast()) {
        return false;
      }

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

#if defined(JS_ION_PERF)
    perfSpewer->endBasicBlock(masm);
#endif
  }

  JitSpew(JitSpew_Codegen, "==== END CodeGenerator::generateBody ====\n");
  return true;
}

// Out-of-line object allocation for LNewArray.
class OutOfLineNewArray : public OutOfLineCodeBase<CodeGenerator> {
  LNewArray* lir_;

 public:
  explicit OutOfLineNewArray(LNewArray* lir) : lir_(lir) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineNewArray(this);
  }

  LNewArray* lir() const { return lir_; }
};

void CodeGenerator::visitNewArrayCallVM(LNewArray* lir) {
  Register objReg = ToRegister(lir->output());

  MOZ_ASSERT(!lir->isCall());
  saveLive(lir);

  JSObject* templateObject = lir->mir()->templateObject();

  if (templateObject) {
    pushArg(ImmGCPtr(templateObject->shape()));
    pushArg(Imm32(lir->mir()->length()));

    using Fn = ArrayObject* (*)(JSContext*, uint32_t, HandleShape);
    callVM<Fn, NewArrayWithShape>(lir);
  } else {
    pushArg(Imm32(GenericObject));
    pushArg(Imm32(lir->mir()->length()));

    using Fn = ArrayObject* (*)(JSContext*, uint32_t, NewObjectKind);
    callVM<Fn, NewArrayOperation>(lir);
  }

  if (ReturnReg != objReg) {
    masm.movePtr(ReturnReg, objReg);
  }

  restoreLive(lir);
}

void CodeGenerator::visitAtan2D(LAtan2D* lir) {
  Register temp = ToRegister(lir->temp());
  FloatRegister y = ToFloatRegister(lir->y());
  FloatRegister x = ToFloatRegister(lir->x());

  using Fn = double (*)(double x, double y);
  masm.setupUnalignedABICall(temp);
  masm.passABIArg(y, MoveOp::DOUBLE);
  masm.passABIArg(x, MoveOp::DOUBLE);
  masm.callWithABI<Fn, ecmaAtan2>(MoveOp::DOUBLE);

  MOZ_ASSERT(ToFloatRegister(lir->output()) == ReturnDoubleReg);
}

void CodeGenerator::visitHypot(LHypot* lir) {
  Register temp = ToRegister(lir->temp());
  uint32_t numArgs = lir->numArgs();
  masm.setupUnalignedABICall(temp);

  for (uint32_t i = 0; i < numArgs; ++i) {
    masm.passABIArg(ToFloatRegister(lir->getOperand(i)), MoveOp::DOUBLE);
  }

  switch (numArgs) {
    case 2: {
      using Fn = double (*)(double x, double y);
      masm.callWithABI<Fn, ecmaHypot>(MoveOp::DOUBLE);
      break;
    }
    case 3: {
      using Fn = double (*)(double x, double y, double z);
      masm.callWithABI<Fn, hypot3>(MoveOp::DOUBLE);
      break;
    }
    case 4: {
      using Fn = double (*)(double x, double y, double z, double w);
      masm.callWithABI<Fn, hypot4>(MoveOp::DOUBLE);
      break;
    }
    default:
      MOZ_CRASH("Unexpected number of arguments to hypot function.");
  }
  MOZ_ASSERT(ToFloatRegister(lir->output()) == ReturnDoubleReg);
}

void CodeGenerator::visitNewArray(LNewArray* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp());
  DebugOnly<uint32_t> length = lir->mir()->length();

  MOZ_ASSERT(length <= NativeObject::MAX_DENSE_ELEMENTS_COUNT);

  if (lir->mir()->isVMCall()) {
    visitNewArrayCallVM(lir);
    return;
  }

  OutOfLineNewArray* ool = new (alloc()) OutOfLineNewArray(lir);
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

void CodeGenerator::visitOutOfLineNewArray(OutOfLineNewArray* ool) {
  visitNewArrayCallVM(ool->lir());
  masm.jump(ool->rejoin());
}

void CodeGenerator::visitNewArrayDynamicLength(LNewArrayDynamicLength* lir) {
  Register lengthReg = ToRegister(lir->length());
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp());

  JSObject* templateObject = lir->mir()->templateObject();
  gc::InitialHeap initialHeap = lir->mir()->initialHeap();

  using Fn = ArrayObject* (*)(JSContext*, HandleArrayObject, int32_t length);
  OutOfLineCode* ool = oolCallVM<Fn, ArrayConstructorOneArg>(
      lir, ArgList(ImmGCPtr(templateObject), lengthReg),
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
  Register tempReg = ToRegister(lir->temp());

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
  masm.createGCObject(objReg, tempReg, templateObject, gc::DefaultHeap,
                      ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewTypedArray(LNewTypedArray* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp1());
  Register lengthReg = ToRegister(lir->temp2());
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();

  JSObject* templateObject = lir->mir()->templateObject();
  gc::InitialHeap initialHeap = lir->mir()->initialHeap();

  TypedArrayObject* ttemplate = &templateObject->as<TypedArrayObject>();

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
  Register tempReg = ToRegister(lir->temp());
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();

  JSObject* templateObject = lir->mir()->templateObject();
  gc::InitialHeap initialHeap = lir->mir()->initialHeap();

  TypedArrayObject* ttemplate = &templateObject->as<TypedArrayObject>();

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, int32_t length);
  OutOfLineCode* ool = oolCallVM<Fn, NewTypedArrayWithTemplateAndLength>(
      lir, ArgList(ImmGCPtr(templateObject), lengthReg),
      StoreRegisterTo(objReg));

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
  pushArg(ToValue(lir, LNewTypedArrayFromArrayBuffer::LengthIndex));
  pushArg(ToValue(lir, LNewTypedArrayFromArrayBuffer::ByteOffsetIndex));
  pushArg(ToRegister(lir->arrayBuffer()));
  pushArg(ImmGCPtr(lir->mir()->templateObject()));

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, HandleObject,
                                   HandleValue, HandleValue);
  callVM<Fn, js::NewTypedArrayWithTemplateAndBuffer>(lir);
}

// Out-of-line object allocation for JSOp::NewObject.
class OutOfLineNewObject : public OutOfLineCodeBase<CodeGenerator> {
  LNewObject* lir_;

 public:
  explicit OutOfLineNewObject(LNewObject* lir) : lir_(lir) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineNewObject(this);
  }

  LNewObject* lir() const { return lir_; }
};

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
    case MNewObject::ObjectLiteral:
      if (templateObject) {
        pushArg(ImmGCPtr(templateObject));

        using Fn = JSObject* (*)(JSContext*, HandleObject);
        callVM<Fn, NewObjectOperationWithTemplate>(lir);
      } else {
        pushArg(Imm32(GenericObject));
        pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));
        pushArg(ImmGCPtr(lir->mir()->block()->info().script()));

        using Fn = JSObject* (*)(JSContext*, HandleScript, jsbytecode * pc,
                                 NewObjectKind);
        callVM<Fn, NewObjectOperation>(lir);
      }
      break;
    case MNewObject::ObjectCreate:
      pushArg(ImmGCPtr(templateObject));

      using Fn = PlainObject* (*)(JSContext*, HandlePlainObject);
      callVM<Fn, ObjectCreateWithTemplate>(lir);
      break;
  }

  if (ReturnReg != objReg) {
    masm.movePtr(ReturnReg, objReg);
  }

  restoreLive(lir);
}

static bool ShouldInitFixedSlots(LInstruction* lir, const TemplateObject& obj) {
  if (!obj.isNativeObject()) {
    return true;
  }
  const TemplateNativeObject& templateObj = obj.asTemplateNativeObject();

  // Look for StoreFixedSlot instructions following an object allocation
  // that write to this object before a GC is triggered or this object is
  // passed to a VM call. If all fixed slots will be initialized, the
  // allocation code doesn't need to set the slots to |undefined|.

  uint32_t nfixed = templateObj.numUsedFixedSlots();
  if (nfixed == 0) {
    return false;
  }

  // Only optimize if all fixed slots are initially |undefined|, so that we
  // can assume incremental pre-barriers are not necessary. See also the
  // comment below.
  for (uint32_t slot = 0; slot < nfixed; slot++) {
    if (!templateObj.getSlot(slot).isUndefined()) {
      return true;
    }
  }

  // Keep track of the fixed slots that are initialized. initializedSlots is
  // a bit mask with a bit for each slot.
  MOZ_ASSERT(nfixed <= NativeObject::MAX_FIXED_SLOTS);
  static_assert(NativeObject::MAX_FIXED_SLOTS <= 32,
                "Slot bits must fit in 32 bits");
  uint32_t initializedSlots = 0;
  uint32_t numInitialized = 0;

  MInstruction* allocMir = lir->mirRaw()->toInstruction();
  MBasicBlock* block = allocMir->block();

  // Skip the allocation instruction.
  MInstructionIterator iter = block->begin(allocMir);
  MOZ_ASSERT(*iter == allocMir);
  iter++;

  while (true) {
    for (; iter != block->end(); iter++) {
      if (iter->isNop() || iter->isConstant() || iter->isPostWriteBarrier()) {
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

      if (iter->isGoto()) {
        block = iter->toGoto()->target();
        if (block->numPredecessors() != 1) {
          return true;
        }
        break;
      }

      // Unhandled instruction, assume it bails or reads object slots.
      return true;
    }
    iter = block->begin();
  }

  MOZ_CRASH("Shouldn't get here");
}

void CodeGenerator::visitNewObject(LNewObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp());

  if (lir->mir()->isVMCall()) {
    visitNewObjectVMCall(lir);
    return;
  }

  OutOfLineNewObject* ool = new (alloc()) OutOfLineNewObject(lir);
  addOutOfLineCode(ool, lir->mir());

  TemplateObject templateObject(lir->mir()->templateObject());

  bool initContents = ShouldInitFixedSlots(lir, templateObject);
  masm.createGCObject(objReg, tempReg, templateObject,
                      lir->mir()->initialHeap(), ool->entry(), initContents);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineNewObject(OutOfLineNewObject* ool) {
  visitNewObjectVMCall(ool->lir());
  masm.jump(ool->rejoin());
}

void CodeGenerator::visitNewPlainObject(LNewPlainObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register temp0Reg = ToRegister(lir->temp0());
  Register temp1Reg = ToRegister(lir->temp1());
  Register shapeReg = ToRegister(lir->temp2());

  auto* mir = lir->mir();
  const Shape* shape = mir->shape();
  gc::InitialHeap initialHeap = mir->initialHeap();
  gc::AllocKind allocKind = mir->allocKind();

  using Fn =
      JSObject* (*)(JSContext*, HandleShape, gc::AllocKind, gc::InitialHeap);
  OutOfLineCode* ool = oolCallVM<Fn, NewPlainObjectOptimizedFallback>(
      lir,
      ArgList(ImmGCPtr(shape), Imm32(int32_t(allocKind)), Imm32(initialHeap)),
      StoreRegisterTo(objReg));

  masm.movePtr(ImmGCPtr(shape), shapeReg);
  masm.createPlainGCObject(objReg, shapeReg, temp0Reg, temp1Reg,
                           mir->numFixedSlots(), mir->numDynamicSlots(),
                           allocKind, initialHeap, ool->entry(),
                           AllocSiteInput(gc::CatchAllAllocSite::Optimized));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewArrayObject(LNewArrayObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register temp0Reg = ToRegister(lir->temp0());
  Register shapeReg = ToRegister(lir->temp1());

  auto* mir = lir->mir();
  uint32_t arrayLength = mir->length();

  gc::AllocKind allocKind = GuessArrayGCKind(arrayLength);
  MOZ_ASSERT(CanChangeToBackgroundAllocKind(allocKind, &ArrayObject::class_));
  allocKind = ForegroundToBackgroundAllocKind(allocKind);

  uint32_t slotCount = GetGCKindSlots(allocKind);
  MOZ_ASSERT(slotCount >= ObjectElements::VALUES_PER_HEADER);
  uint32_t arrayCapacity = slotCount - ObjectElements::VALUES_PER_HEADER;

  const Shape* shape = mir->shape();

  NewObjectKind objectKind =
      mir->initialHeap() == gc::TenuredHeap ? TenuredObject : GenericObject;

  using Fn =
      ArrayObject* (*)(JSContext*, uint32_t, gc::AllocKind, NewObjectKind);
  OutOfLineCode* ool = oolCallVM<Fn, NewArrayObjectOptimizedFallback>(
      lir,
      ArgList(Imm32(arrayLength), Imm32(int32_t(allocKind)), Imm32(objectKind)),
      StoreRegisterTo(objReg));

  masm.movePtr(ImmPtr(shape), shapeReg);
  masm.createArrayWithFixedElements(
      objReg, shapeReg, temp0Reg, arrayLength, arrayCapacity, allocKind,
      mir->initialHeap(), ool->entry(),
      AllocSiteInput(gc::CatchAllAllocSite::Optimized));
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewNamedLambdaObject(LNewNamedLambdaObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp());
  const CompileInfo& info = lir->mir()->block()->info();

  // If we have a template object, we can inline call object creation.
  using Fn =
      js::NamedLambdaObject* (*)(JSContext*, HandleFunction, gc::InitialHeap);
  OutOfLineCode* ool = oolCallVM<Fn, NamedLambdaObject::createTemplateObject>(
      lir, ArgList(ImmGCPtr(info.funMaybeLazy()), Imm32(gc::DefaultHeap)),
      StoreRegisterTo(objReg));

  TemplateObject templateObject(lir->mir()->templateObj());

  bool initContents = ShouldInitFixedSlots(lir, templateObject);
  masm.createGCObject(objReg, tempReg, templateObject, gc::DefaultHeap,
                      ool->entry(), initContents);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewCallObject(LNewCallObject* lir) {
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp());

  CallObject* templateObj = lir->mir()->templateObject();

  using Fn = JSObject* (*)(JSContext*, HandleShape);
  OutOfLineCode* ool = oolCallVM<Fn, NewCallObject>(
      lir, ArgList(ImmGCPtr(templateObj->shape())), StoreRegisterTo(objReg));

  // Inline call object creation, using the OOL path only for tricky cases.
  TemplateObject templateObject(templateObj);
  bool initContents = ShouldInitFixedSlots(lir, templateObject);
  masm.createGCObject(objReg, tempReg, templateObject, gc::DefaultHeap,
                      ool->entry(), initContents);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitNewStringObject(LNewStringObject* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp());

  StringObject* templateObj = lir->mir()->templateObj();

  using Fn = JSObject* (*)(JSContext*, HandleString);
  OutOfLineCode* ool = oolCallVM<Fn, NewStringObject>(lir, ArgList(input),
                                                      StoreRegisterTo(output));

  TemplateObject templateObject(templateObj);
  masm.createGCObject(output, temp, templateObject, gc::DefaultHeap,
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
  pushArg(ToValue(lir, LInitElemGetterSetter::IdIndex));
  pushArg(obj);
  pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));

  using Fn = bool (*)(JSContext*, jsbytecode*, HandleObject, HandleValue,
                      HandleObject);
  callVM<Fn, InitElemGetterSetterOperation>(lir);
}

void CodeGenerator::visitMutateProto(LMutateProto* lir) {
  Register objReg = ToRegister(lir->getObject());

  pushArg(ToValue(lir, LMutateProto::ValueIndex));
  pushArg(objReg);

  using Fn = bool (*)(JSContext * cx, HandlePlainObject obj, HandleValue value);
  callVM<Fn, MutatePrototype>(lir);
}

void CodeGenerator::visitInitPropGetterSetter(LInitPropGetterSetter* lir) {
  Register obj = ToRegister(lir->object());
  Register value = ToRegister(lir->value());

  pushArg(value);
  pushArg(ImmGCPtr(lir->mir()->name()));
  pushArg(obj);
  pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));

  using Fn = bool (*)(JSContext*, jsbytecode*, HandleObject, HandlePropertyName,
                      HandleObject);
  callVM<Fn, InitPropGetterSetterOperation>(lir);
}

void CodeGenerator::visitCreateThis(LCreateThis* lir) {
  const LAllocation* callee = lir->getCallee();
  const LAllocation* newTarget = lir->getNewTarget();

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

  using Fn = bool (*)(JSContext * cx, HandleObject callee,
                      HandleObject newTarget, MutableHandleValue rval);
  callVM<Fn, jit::CreateThisFromIon>(lir);
}

void CodeGenerator::visitCreateThisWithTemplate(LCreateThisWithTemplate* lir) {
  JSObject* templateObject = lir->mir()->templateObject();
  Register objReg = ToRegister(lir->output());
  Register tempReg = ToRegister(lir->temp());

  using Fn = JSObject* (*)(JSContext*, HandleObject);
  OutOfLineCode* ool = oolCallVM<Fn, CreateThisWithTemplate>(
      lir, ArgList(ImmGCPtr(templateObject)), StoreRegisterTo(objReg));

  // Allocate. If the FreeList is empty, call to VM, which may GC.
  TemplateObject templateObj(templateObject);
  bool initContents =
      !templateObj.isPlainObject() || ShouldInitFixedSlots(lir, templateObj);
  masm.createGCObject(objReg, tempReg, templateObj, lir->mir()->initialHeap(),
                      ool->entry(), initContents);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCreateArgumentsObject(LCreateArgumentsObject* lir) {
  // This should be getting constructed in the first block only, and not any OSR
  // entry blocks.
  MOZ_ASSERT(lir->mir()->block()->id() == 0);

  Register callObj = ToRegister(lir->getCallObject());
  Register temp = ToRegister(lir->temp0());
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
    masm.createGCObject(objTemp, temp, templateObject, gc::DefaultHeap,
                        &failure,
                        /* initContents = */ false);

    masm.moveStackPtrTo(temp);
    masm.addPtr(Imm32(masm.framePushed()), temp);

    using Fn =
        ArgumentsObject* (*)(JSContext * cx, jit::JitFrameLayout * frame,
                             JSObject * scopeChain, ArgumentsObject * obj);
    masm.setupUnalignedABICall(cxTemp);
    masm.loadJSContext(cxTemp);
    masm.passABIArg(cxTemp);
    masm.passABIArg(temp);
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

  masm.moveStackPtrTo(temp);
  masm.addPtr(Imm32(frameSize()), temp);

  pushArg(callObj);
  pushArg(temp);

  using Fn = ArgumentsObject* (*)(JSContext*, JitFrameLayout*, HandleObject);
  callVM<Fn, ArgumentsObject::createForIon>(lir);

  masm.bind(&done);
}

void CodeGenerator::visitCreateInlinedArgumentsObject(
    LCreateInlinedArgumentsObject* lir) {
  Register callObj = ToRegister(lir->getCallObject());
  Register callee = ToRegister(lir->getCallee());
  Register argsAddress = ToRegister(lir->temp());

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

  pushArg(Imm32(argc));
  pushArg(callObj);
  pushArg(callee);
  pushArg(argsAddress);

  using Fn = ArgumentsObject* (*)(JSContext*, Value*, HandleFunction,
                                  HandleObject, uint32_t);
  callVM<Fn, ArgumentsObject::createForInlinedIon>(lir);

  // Discard the array of values.
  masm.freeStack(argc * sizeof(Value));
}

void CodeGenerator::visitGetInlinedArgument(LGetInlinedArgument* lir) {
  Register index = ToRegister(lir->getIndex());
  ValueOperand output = ToOutValue(lir);

  uint32_t numActuals = lir->mir()->numActuals();
  MOZ_ASSERT(numActuals > 0 && numActuals <= ArgumentsObject::MaxInlinedArgs);

  // Check the first n-1 possible indices.
  Label done;
  for (uint32_t i = 0; i < numActuals - 1; i++) {
    Label skip;
    ConstantOrRegister arg = toConstantOrRegister(
        lir, LGetInlinedArgument::ArgIndex(i), lir->mir()->getArg(i)->type());
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
      toConstantOrRegister(lir, LGetInlinedArgument::ArgIndex(lastIdx),
                           lir->mir()->getArg(lastIdx)->type());
  masm.moveValue(arg, output);
  masm.bind(&done);
}

void CodeGenerator::visitGetArgumentsObjectArg(LGetArgumentsObjectArg* lir) {
  Register temp = ToRegister(lir->getTemp(0));
  Register argsObj = ToRegister(lir->getArgsObject());
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
  Register temp = ToRegister(lir->getTemp(0));
  Register argsObj = ToRegister(lir->getArgsObject());
  ValueOperand value = ToValue(lir, LSetArgumentsObjectArg::ValueIndex);

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
  Register temp = ToRegister(lir->temp());
  Register argsObj = ToRegister(lir->getArgsObject());
  Register index = ToRegister(lir->index());
  ValueOperand out = ToOutValue(lir);

  Label bail;
  masm.loadArgumentsObjectElement(argsObj, index, out, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitArgumentsObjectLength(LArgumentsObjectLength* lir) {
  Register argsObj = ToRegister(lir->getArgsObject());
  Register out = ToRegister(lir->output());

  Label bail;
  masm.loadArgumentsObjectLength(argsObj, out, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardArgumentsObjectFlags(
    LGuardArgumentsObjectFlags* lir) {
  Register argsObj = ToRegister(lir->getArgsObject());
  Register temp = ToRegister(lir->temp());

  Label bail;
  masm.branchTestArgumentsObjectFlags(argsObj, temp, lir->mir()->flags(),
                                      Assembler::NonZero, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitReturnFromCtor(LReturnFromCtor* lir) {
  ValueOperand value = ToValue(lir, LReturnFromCtor::ValueIndex);
  Register obj = ToRegister(lir->getObject());
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

class OutOfLineBoxNonStrictThis : public OutOfLineCodeBase<CodeGenerator> {
  LBoxNonStrictThis* ins_;

 public:
  explicit OutOfLineBoxNonStrictThis(LBoxNonStrictThis* ins) : ins_(ins) {}
  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineBoxNonStrictThis(this);
  }
  LBoxNonStrictThis* ins() const { return ins_; }
};

void CodeGenerator::visitBoxNonStrictThis(LBoxNonStrictThis* lir) {
  ValueOperand value = ToValue(lir, LBoxNonStrictThis::ValueIndex);
  Register output = ToRegister(lir->output());

  auto* ool = new (alloc()) OutOfLineBoxNonStrictThis(lir);
  addOutOfLineCode(ool, lir->mir());

  masm.fallibleUnboxObject(value, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineBoxNonStrictThis(
    OutOfLineBoxNonStrictThis* ool) {
  LBoxNonStrictThis* lir = ool->ins();

  ValueOperand value = ToValue(lir, LBoxNonStrictThis::ValueIndex);
  Register output = ToRegister(lir->output());

  Label notNullOrUndefined;
  {
    Label isNullOrUndefined;
    ScratchTagScope tag(masm, value);
    masm.splitTagForTest(value, tag);
    masm.branchTestUndefined(Assembler::Equal, tag, &isNullOrUndefined);
    masm.branchTestNull(Assembler::NotEqual, tag, &notNullOrUndefined);
    masm.bind(&isNullOrUndefined);
    masm.movePtr(ImmGCPtr(lir->mir()->globalThis()), output);
    masm.jump(ool->rejoin());
  }

  masm.bind(&notNullOrUndefined);

  saveLive(lir);

  pushArg(value);
  using Fn = JSObject* (*)(JSContext*, HandleValue);
  callVM<Fn, BoxNonStrictThis>(lir);

  StoreRegisterTo(output).generate(this);
  restoreLiveIgnore(lir, StoreRegisterTo(output).clobbered());

  masm.jump(ool->rejoin());
}

void CodeGenerator::visitImplicitThis(LImplicitThis* lir) {
  pushArg(ImmGCPtr(lir->mir()->name()));
  pushArg(ToRegister(lir->env()));

  using Fn = bool (*)(JSContext*, HandleObject, HandlePropertyName,
                      MutableHandleValue);
  callVM<Fn, ImplicitThisOperation>(lir);
}

void CodeGenerator::visitArrowNewTarget(LArrowNewTarget* lir) {
  Register callee = ToRegister(lir->callee());
  ValueOperand output = ToOutValue(lir);
  masm.loadValue(
      Address(callee, FunctionExtended::offsetOfArrowNewTargetSlot()), output);
}

void CodeGenerator::visitArrayLength(LArrayLength* lir) {
  Register elements = ToRegister(lir->elements());
  Register output = ToRegister(lir->output());

  Address length(elements, ObjectElements::offsetOfLength());
  masm.load32(length, output);

  // Bail out if the length doesn't fit in int32.
  Label bail;
  masm.branchTest32(Assembler::Signed, output, output, &bail);
  bailoutFrom(&bail, lir->snapshot());
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
  masm.load16ZeroExtend(Address(function, JSFunction::offsetOfFlags()), output);

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
  masm.loadFunctionName(function, output, ImmGCPtr(names.empty), &bail);

  bailoutFrom(&bail, lir->snapshot());
}

template <class OrderedHashTable>
static void RangeFront(MacroAssembler&, Register, Register, Register);

template <>
void RangeFront<ValueMap>(MacroAssembler& masm, Register range, Register i,
                          Register front) {
  masm.loadPtr(Address(range, ValueMap::Range::offsetOfHashTable()), front);
  masm.loadPtr(Address(front, ValueMap::offsetOfImplData()), front);

  MOZ_ASSERT(ValueMap::offsetOfImplDataElement() == 0,
             "offsetof(Data, element) is 0");
  static_assert(ValueMap::sizeofImplData() == 24, "sizeof(Data) is 24");
  masm.mulBy3(i, i);
  masm.lshiftPtr(Imm32(3), i);
  masm.addPtr(i, front);
}

template <>
void RangeFront<ValueSet>(MacroAssembler& masm, Register range, Register i,
                          Register front) {
  masm.loadPtr(Address(range, ValueSet::Range::offsetOfHashTable()), front);
  masm.loadPtr(Address(front, ValueSet::offsetOfImplData()), front);

  MOZ_ASSERT(ValueSet::offsetOfImplDataElement() == 0,
             "offsetof(Data, element) is 0");
  static_assert(ValueSet::sizeofImplData() == 16, "sizeof(Data) is 16");
  masm.lshiftPtr(Imm32(4), i);
  masm.addPtr(i, front);
}

template <class OrderedHashTable>
static void RangePopFront(MacroAssembler& masm, Register range, Register front,
                          Register dataLength, Register temp) {
  Register i = temp;

  masm.add32(Imm32(1),
             Address(range, OrderedHashTable::Range::offsetOfCount()));

  masm.load32(Address(range, OrderedHashTable::Range::offsetOfI()), i);

  Label done, seek;
  masm.bind(&seek);
  masm.add32(Imm32(1), i);
  masm.branch32(Assembler::AboveOrEqual, i, dataLength, &done);

  // We can add sizeof(Data) to |front| to select the next element, because
  // |front| and |range.ht.data[i]| point to the same location.
  MOZ_ASSERT(OrderedHashTable::offsetOfImplDataElement() == 0,
             "offsetof(Data, element) is 0");
  masm.addPtr(Imm32(OrderedHashTable::sizeofImplData()), front);

  masm.branchTestMagic(Assembler::Equal,
                       Address(front, OrderedHashTable::offsetOfEntryKey()),
                       JS_HASH_KEY_EMPTY, &seek);

  masm.bind(&done);
  masm.store32(i, Address(range, OrderedHashTable::Range::offsetOfI()));
}

template <class OrderedHashTable>
static inline void RangeDestruct(MacroAssembler& masm, Register iter,
                                 Register range, Register temp0,
                                 Register temp1) {
  Register next = temp0;
  Register prevp = temp1;

  masm.loadPtr(Address(range, OrderedHashTable::Range::offsetOfNext()), next);
  masm.loadPtr(Address(range, OrderedHashTable::Range::offsetOfPrevP()), prevp);
  masm.storePtr(next, Address(prevp, 0));

  Label hasNoNext;
  masm.branchTestPtr(Assembler::Zero, next, next, &hasNoNext);

  masm.storePtr(prevp, Address(next, OrderedHashTable::Range::offsetOfPrevP()));

  masm.bind(&hasNoNext);

  Label nurseryAllocated;
  masm.branchPtrInNurseryChunk(Assembler::Equal, iter, temp0,
                               &nurseryAllocated);

  masm.callFreeStub(range);

  masm.bind(&nurseryAllocated);
}

template <>
void CodeGenerator::emitLoadIteratorValues<ValueMap>(Register result,
                                                     Register temp,
                                                     Register front) {
  size_t elementsOffset = NativeObject::offsetOfFixedElements();

  Address keyAddress(front, ValueMap::Entry::offsetOfKey());
  Address valueAddress(front, ValueMap::Entry::offsetOfValue());
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
void CodeGenerator::emitLoadIteratorValues<ValueSet>(Register result,
                                                     Register temp,
                                                     Register front) {
  size_t elementsOffset = NativeObject::offsetOfFixedElements();

  Address keyAddress(front, ValueSet::offsetOfEntryKey());
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

template <class IteratorObject, class OrderedHashTable>
void CodeGenerator::emitGetNextEntryForIterator(LGetNextEntryForIterator* lir) {
  Register iter = ToRegister(lir->iter());
  Register result = ToRegister(lir->result());
  Register temp = ToRegister(lir->temp0());
  Register dataLength = ToRegister(lir->temp1());
  Register range = ToRegister(lir->temp2());
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

  masm.loadPrivate(Address(iter, NativeObject::getFixedSlotOffset(
                                     IteratorObject::RangeSlot)),
                   range);

  Label iterAlreadyDone, iterDone, done;
  masm.branchTestPtr(Assembler::Zero, range, range, &iterAlreadyDone);

  masm.load32(Address(range, OrderedHashTable::Range::offsetOfI()), temp);
  masm.loadPtr(Address(range, OrderedHashTable::Range::offsetOfHashTable()),
               dataLength);
  masm.load32(Address(dataLength, OrderedHashTable::offsetOfImplDataLength()),
              dataLength);
  masm.branch32(Assembler::AboveOrEqual, temp, dataLength, &iterDone);
  {
    masm.push(iter);

    Register front = iter;
    RangeFront<OrderedHashTable>(masm, range, temp, front);

    emitLoadIteratorValues<OrderedHashTable>(result, temp, front);

    RangePopFront<OrderedHashTable>(masm, range, front, dataLength, temp);

    masm.pop(iter);
    masm.move32(Imm32(0), output);
  }
  masm.jump(&done);
  {
    masm.bind(&iterDone);

    RangeDestruct<OrderedHashTable>(masm, iter, range, temp, dataLength);

    masm.storeValue(PrivateValue(nullptr),
                    Address(iter, NativeObject::getFixedSlotOffset(
                                      IteratorObject::RangeSlot)));

    masm.bind(&iterAlreadyDone);

    masm.move32(Imm32(1), output);
  }
  masm.bind(&done);
}

void CodeGenerator::visitGetNextEntryForIterator(
    LGetNextEntryForIterator* lir) {
  if (lir->mir()->mode() == MGetNextEntryForIterator::Map) {
    emitGetNextEntryForIterator<MapIteratorObject, ValueMap>(lir);
  } else {
    MOZ_ASSERT(lir->mir()->mode() == MGetNextEntryForIterator::Set);
    emitGetNextEntryForIterator<SetIteratorObject, ValueSet>(lir);
  }
}

// The point of these is to inform Ion of where these values already are; they
// don't normally generate code.  Still, visitWasmRegisterResult is
// per-platform.
void CodeGenerator::visitWasmRegisterPairResult(LWasmRegisterPairResult* lir) {}
void CodeGenerator::visitWasmStackResult(LWasmStackResult* lir) {}
void CodeGenerator::visitWasmStackResult64(LWasmStackResult64* lir) {}

void CodeGenerator::visitWasmStackResultArea(LWasmStackResultArea* lir) {
  LAllocation* output = lir->getDef(0)->output();
  MOZ_ASSERT(output->isStackArea());
  bool tempInit = false;
  for (auto iter = output->toStackArea()->results(); iter; iter.next()) {
    // Zero out ref stack results.
    if (iter.isGcPointer()) {
      Register temp = ToRegister(lir->temp());
      if (!tempInit) {
        masm.xorPtr(temp, temp);
        tempInit = true;
      }
      masm.storePtr(temp, ToAddress(iter.alloc()));
    }
  }
}

void CodeGenerator::visitWasmCall(LWasmCall* lir) {
  MWasmCall* mir = lir->mir();
  bool needsBoundsCheck = lir->needsBoundsCheck();

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
  // TLS and pinned regs. The only case where where we don't have to reload
  // the TLS and pinned regs is when the callee preserves them.
  bool reloadRegs = true;
  bool switchRealm = true;

  const wasm::CallSiteDesc& desc = mir->desc();
  const wasm::CalleeDesc& callee = mir->callee();
  switch (callee.which()) {
    case wasm::CalleeDesc::Func:
      masm.call(desc, callee.funcIndex());
      reloadRegs = false;
      switchRealm = false;
      break;
    case wasm::CalleeDesc::Import:
      masm.wasmCallImport(desc, callee);
      break;
    case wasm::CalleeDesc::AsmJSTable:
    case wasm::CalleeDesc::WasmTable:
      masm.wasmCallIndirect(desc, callee, needsBoundsCheck);
      reloadRegs = switchRealm = callee.which() == wasm::CalleeDesc::WasmTable;
      break;
    case wasm::CalleeDesc::Builtin:
      masm.call(desc, callee.builtin());
      reloadRegs = false;
      switchRealm = false;
      break;
    case wasm::CalleeDesc::BuiltinInstanceMethod:
      masm.wasmCallBuiltinInstanceMethod(desc, mir->instanceArg(),
                                         callee.builtin(),
                                         mir->builtinMethodFailureMode());
      switchRealm = false;
      break;
  }

  // Note the assembler offset for the associated LSafePoint.
  markSafepointAt(masm.currentOffset(), lir);

  // Now that all the outbound in-memory args are on the stack, note the
  // required lower boundary point of the associated StackMap.
  lir->safepoint()->setFramePushedAtStackMapBase(
      masm.framePushed() - mir->stackArgAreaSizeUnaligned());
  MOZ_ASSERT(!lir->safepoint()->isWasmTrap());

  if (reloadRegs) {
    masm.loadPtr(Address(masm.getStackPointer(), WasmCallerTLSOffsetBeforeCall),
                 WasmTlsReg);
    masm.loadWasmPinnedRegsFromTls();
    if (switchRealm) {
      masm.switchToWasmTlsRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
    }
  } else {
    MOZ_ASSERT(!switchRealm);
  }
}

void CodeGenerator::visitWasmLoadSlot(LWasmLoadSlot* ins) {
  MIRType type = ins->type();
  Register container = ToRegister(ins->containerRef());
  Address addr(container, ins->offset());
  AnyRegister dst = ToAnyRegister(ins->output());

  switch (type) {
    case MIRType::Int32:
      masm.load32(addr, dst.gpr());
      break;
    case MIRType::Float32:
      masm.loadFloat32(addr, dst.fpu());
      break;
    case MIRType::Double:
      masm.loadDouble(addr, dst.fpu());
      break;
    case MIRType::Pointer:
    case MIRType::RefOrNull:
      masm.loadPtr(addr, dst.gpr());
      break;
#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128:
      masm.loadUnalignedSimd128(addr, dst.fpu());
      break;
#endif
    default:
      MOZ_CRASH("unexpected type in LoadPrimitiveValue");
  }
}

void CodeGenerator::visitWasmStoreSlot(LWasmStoreSlot* ins) {
  MIRType type = ins->type();
  Register container = ToRegister(ins->containerRef());
  Address addr(container, ins->offset());
  AnyRegister src = ToAnyRegister(ins->value());

  switch (type) {
    case MIRType::Int32:
      masm.store32(src.gpr(), addr);
      break;
    case MIRType::Float32:
      masm.storeFloat32(src.fpu(), addr);
      break;
    case MIRType::Double:
      masm.storeDouble(src.fpu(), addr);
      break;
    case MIRType::Pointer:
      // This could be correct, but it would be a new usage, so check carefully.
      MOZ_CRASH("Unexpected type in visitWasmStoreSlot.");
    case MIRType::RefOrNull:
      MOZ_CRASH("Bad type in visitWasmStoreSlot. Use LWasmStoreRef.");
#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128:
      masm.storeUnalignedSimd128(src.fpu(), addr);
      break;
#endif
    default:
      MOZ_CRASH("unexpected type in StorePrimitiveValue");
  }
}

void CodeGenerator::visitWasmDerivedPointer(LWasmDerivedPointer* ins) {
  masm.movePtr(ToRegister(ins->base()), ToRegister(ins->output()));
  masm.addPtr(Imm32(int32_t(ins->offset())), ToRegister(ins->output()));
}

void CodeGenerator::visitWasmStoreRef(LWasmStoreRef* ins) {
  Register tls = ToRegister(ins->tls());
  Register valueAddr = ToRegister(ins->valueAddr());
  Register value = ToRegister(ins->value());
  Register temp = ToRegister(ins->temp());

  Label skipPreBarrier;
  wasm::EmitWasmPreBarrierGuard(masm, tls, temp, valueAddr, &skipPreBarrier);
  wasm::EmitWasmPreBarrierCall(masm, tls, temp, valueAddr);
  masm.bind(&skipPreBarrier);

  masm.storePtr(value, Address(valueAddr, 0));
  // The postbarrier is handled separately.
}

void CodeGenerator::visitWasmLoadSlotI64(LWasmLoadSlotI64* ins) {
  Register container = ToRegister(ins->containerRef());
  Address addr(container, ins->offset());
  Register64 output = ToOutRegister64(ins);
  masm.load64(addr, output);
}

void CodeGenerator::visitWasmStoreSlotI64(LWasmStoreSlotI64* ins) {
  Register container = ToRegister(ins->containerRef());
  Address addr(container, ins->offset());
  Register64 value = ToRegister64(ins->value());
  masm.store64(value, addr);
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

void CodeGenerator::visitGuardHasAttachedArrayBuffer(
    LGuardHasAttachedArrayBuffer* lir) {
  Register obj = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp());

  Label bail;
  masm.branchIfHasDetachedArrayBuffer(obj, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

class OutOfLineGuardNumberToIntPtrIndex
    : public OutOfLineCodeBase<CodeGenerator> {
  LGuardNumberToIntPtrIndex* lir_;

 public:
  explicit OutOfLineGuardNumberToIntPtrIndex(LGuardNumberToIntPtrIndex* lir)
      : lir_(lir) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineGuardNumberToIntPtrIndex(this);
  }
  LGuardNumberToIntPtrIndex* lir() const { return lir_; }
};

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

  auto* ool = new (alloc()) OutOfLineGuardNumberToIntPtrIndex(lir);
  addOutOfLineCode(ool, lir->mir());

  masm.convertDoubleToPtr(input, output, ool->entry(), false);
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineGuardNumberToIntPtrIndex(
    OutOfLineGuardNumberToIntPtrIndex* ool) {
  // Substitute the invalid index with an arbitrary out-of-bounds index.
  masm.movePtr(ImmWord(-1), ToRegister(ool->lir()->output()));
  masm.jump(ool->rejoin());
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
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  bool isMax = ins->isMax();

  Label bail;
  masm.minMaxArrayInt32(array, output, temp1, temp2, temp3, isMax, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitMinMaxArrayD(LMinMaxArrayD* ins) {
  Register array = ToRegister(ins->array());
  FloatRegister output = ToFloatRegister(ins->output());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  FloatRegister floatTemp = ToFloatRegister(ins->floatTemp());
  bool isMax = ins->isMax();

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
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());

  Label bailout;
  masm.pow32(value, power, output, temp1, temp2, &bailout);
  bailoutFrom(&bailout, ins->snapshot());
}

void CodeGenerator::visitPowI(LPowI* ins) {
  FloatRegister value = ToFloatRegister(ins->value());
  Register power = ToRegister(ins->power());
  Register temp = ToRegister(ins->temp());

  MOZ_ASSERT(power != temp);

  using Fn = double (*)(double x, int32_t y);
  masm.setupUnalignedABICall(temp);
  masm.passABIArg(value, MoveOp::DOUBLE);
  masm.passABIArg(power);

  masm.callWithABI<Fn, js::powi>(MoveOp::DOUBLE);
  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
}

void CodeGenerator::visitPowD(LPowD* ins) {
  FloatRegister value = ToFloatRegister(ins->value());
  FloatRegister power = ToFloatRegister(ins->power());
  Register temp = ToRegister(ins->temp());

  using Fn = double (*)(double x, double y);
  masm.setupUnalignedABICall(temp);
  masm.passABIArg(value, MoveOp::DOUBLE);
  masm.passABIArg(power, MoveOp::DOUBLE);
  masm.callWithABI<Fn, ecmaPow>(MoveOp::DOUBLE);

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
  FloatRegister temp = ToFloatRegister(ins->temp());
  Register output = ToRegister(ins->output());

  Label bail;
  masm.signDoubleToInt32(input, output, temp, &bail);
  bailoutFrom(&bail, ins->snapshot());
}

void CodeGenerator::visitMathFunctionD(LMathFunctionD* ins) {
  Register temp = ToRegister(ins->temp());
  FloatRegister input = ToFloatRegister(ins->input());
  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);

  UnaryMathFunction fun = ins->mir()->function();
  UnaryMathFunctionType funPtr = GetUnaryMathFunctionPtr(fun);

  masm.setupUnalignedABICall(temp);

  masm.passABIArg(input, MoveOp::DOUBLE);
  masm.callWithABI(DynamicFunction<UnaryMathFunctionType>(funPtr),
                   MoveOp::DOUBLE);
}

void CodeGenerator::visitMathFunctionF(LMathFunctionF* ins) {
  Register temp = ToRegister(ins->temp());
  FloatRegister input = ToFloatRegister(ins->input());
  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnFloat32Reg);

  masm.setupUnalignedABICall(temp);
  masm.passABIArg(input, MoveOp::FLOAT32);

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

  masm.callWithABI(DynamicFunction<Fn>(funptr), MoveOp::FLOAT32, check);
}

void CodeGenerator::visitModD(LModD* ins) {
  MOZ_ASSERT(!gen->compilingWasm());

  FloatRegister lhs = ToFloatRegister(ins->lhs());
  FloatRegister rhs = ToFloatRegister(ins->rhs());

  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
  MOZ_ASSERT(!ins->temp()->isBogusTemp());

  using Fn = double (*)(double a, double b);
  masm.setupUnalignedABICall(ToRegister(ins->temp()));
  masm.passABIArg(lhs, MoveOp::DOUBLE);
  masm.passABIArg(rhs, MoveOp::DOUBLE);
  masm.callWithABI<Fn, NumberMod>(MoveOp::DOUBLE);
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
  masm.Push(WasmTlsReg);
  int32_t framePushedAfterTls = masm.framePushed();

  FloatRegister lhs = ToFloatRegister(ins->lhs());
  FloatRegister rhs = ToFloatRegister(ins->rhs());

  MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);

  masm.setupWasmABICall();
  masm.passABIArg(lhs, MoveOp::DOUBLE);
  masm.passABIArg(rhs, MoveOp::DOUBLE);

  int32_t tlsOffset = masm.framePushed() - framePushedAfterTls;
  masm.callWithABI(ins->mir()->bytecodeOffset(), wasm::SymbolicAddress::ModD,
                   mozilla::Some(tlsOffset), MoveOp::DOUBLE);

  masm.Pop(WasmTlsReg);
}

void CodeGenerator::visitBigIntAdd(LBigIntAdd* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::add>(ins, ArgList(lhs, rhs),
                                         StoreRegisterTo(output));

  // 0n + x == x
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(rhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // x + 0n == x
  Label rhsNonZero;
  masm.branchIfBigIntIsNonZero(rhs, &rhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&rhsNonZero);

  // Call into the VM when either operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntNonZero(lhs, temp1, ool->entry());
  masm.loadBigIntNonZero(rhs, temp2, ool->entry());

  masm.branchAddPtr(Assembler::Overflow, temp2, temp1, ool->entry());

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntSub(LBigIntSub* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::sub>(ins, ArgList(lhs, rhs),
                                         StoreRegisterTo(output));

  // x - 0n == x
  Label rhsNonZero;
  masm.branchIfBigIntIsNonZero(rhs, &rhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&rhsNonZero);

  // Call into the VM when either operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigInt(lhs, temp1, ool->entry());
  masm.loadBigIntNonZero(rhs, temp2, ool->entry());

  masm.branchSubPtr(Assembler::Overflow, temp2, temp1, ool->entry());

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntMul(LBigIntMul* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::mul>(ins, ArgList(lhs, rhs),
                                         StoreRegisterTo(output));

  // 0n * x == 0n
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // x * 0n == 0n
  Label rhsNonZero;
  masm.branchIfBigIntIsNonZero(rhs, &rhsNonZero);
  masm.movePtr(rhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&rhsNonZero);

  // Call into the VM when either operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntNonZero(lhs, temp1, ool->entry());
  masm.loadBigIntNonZero(rhs, temp2, ool->entry());

  masm.branchMulPtr(Assembler::Overflow, temp2, temp1, ool->entry());

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntDiv(LBigIntDiv* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::div>(ins, ArgList(lhs, rhs),
                                         StoreRegisterTo(output));

  // x / 0 throws an error.
  if (ins->mir()->canBeDivideByZero()) {
    masm.branchIfBigIntIsZero(rhs, ool->entry());
  }

  // 0n / x == 0n
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // Call into the VM when either operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntNonZero(lhs, temp1, ool->entry());
  masm.loadBigIntNonZero(rhs, temp2, ool->entry());

  // |BigInt::div()| returns |lhs| for |lhs / 1n|, which means there's no
  // allocation which might trigger a minor GC to free up nursery space. This
  // requires us to apply the same optimization here, otherwise we'd end up with
  // always entering the OOL call, because the nursery is never evicted.
  Label notOne;
  masm.branchPtr(Assembler::NotEqual, temp2, ImmWord(1), &notOne);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&notOne);

  static constexpr auto DigitMin = std::numeric_limits<
      mozilla::SignedStdintTypeForSize<sizeof(BigInt::Digit)>::Type>::min();

  // Handle an integer overflow from INT{32,64}_MIN / -1.
  Label notOverflow;
  masm.branchPtr(Assembler::NotEqual, temp1, ImmWord(DigitMin), &notOverflow);
  masm.branchPtr(Assembler::Equal, temp2, ImmWord(-1), ool->entry());
  masm.bind(&notOverflow);

  emitBigIntDiv(ins, temp1, temp2, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntMod(LBigIntMod* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::mod>(ins, ArgList(lhs, rhs),
                                         StoreRegisterTo(output));

  // x % 0 throws an error.
  if (ins->mir()->canBeDivideByZero()) {
    masm.branchIfBigIntIsZero(rhs, ool->entry());
  }

  // 0n % x == 0n
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // Call into the VM when either operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntAbsolute(lhs, temp1, ool->entry());
  masm.loadBigIntAbsolute(rhs, temp2, ool->entry());

  // Similar to the case for BigInt division, we must apply the same allocation
  // optimizations as performed in |BigInt::mod()|.
  Label notBelow;
  masm.branchPtr(Assembler::AboveOrEqual, temp1, temp2, &notBelow);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&notBelow);

  // Convert both digits to signed pointer-sized values.
  masm.bigIntDigitToSignedPtr(lhs, temp1, ool->entry());
  masm.bigIntDigitToSignedPtr(rhs, temp2, ool->entry());

  static constexpr auto DigitMin = std::numeric_limits<
      mozilla::SignedStdintTypeForSize<sizeof(BigInt::Digit)>::Type>::min();

  // Handle an integer overflow from INT{32,64}_MIN / -1.
  Label notOverflow;
  masm.branchPtr(Assembler::NotEqual, temp1, ImmWord(DigitMin), &notOverflow);
  masm.branchPtr(Assembler::NotEqual, temp2, ImmWord(-1), &notOverflow);
  masm.movePtr(ImmWord(0), temp1);
  masm.bind(&notOverflow);

  emitBigIntMod(ins, temp1, temp2, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntPow(LBigIntPow* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::pow>(ins, ArgList(lhs, rhs),
                                         StoreRegisterTo(output));

  // x ** -y throws an error.
  if (ins->mir()->canBeNegativeExponent()) {
    masm.branchIfBigIntIsNegative(rhs, ool->entry());
  }

  Register dest = temp1;
  Register base = temp2;
  Register exponent = output;

  Label done;
  masm.movePtr(ImmWord(1), dest);  // p = 1

  // 1n ** y == 1n
  // -1n ** y == 1n when y is even
  // -1n ** y == -1n when y is odd
  Label lhsNotOne;
  masm.branch32(Assembler::Above, Address(lhs, BigInt::offsetOfLength()),
                Imm32(1), &lhsNotOne);
  masm.loadFirstBigIntDigitOrZero(lhs, base);
  masm.branchPtr(Assembler::NotEqual, base, Imm32(1), &lhsNotOne);
  {
    masm.loadFirstBigIntDigitOrZero(rhs, exponent);

    Label lhsNonNegative;
    masm.branchIfBigIntIsNonNegative(lhs, &lhsNonNegative);
    masm.branchTestPtr(Assembler::Zero, exponent, Imm32(1), &done);
    masm.bind(&lhsNonNegative);
    masm.movePtr(lhs, output);
    masm.jump(ool->rejoin());
  }
  masm.bind(&lhsNotOne);

  // x ** 0n == 1n
  masm.branchIfBigIntIsZero(rhs, &done);

  // 0n ** y == 0n with y != 0n
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  {
    masm.movePtr(lhs, output);
    masm.jump(ool->rejoin());
  }
  masm.bind(&lhsNonZero);

  // Call into the VM when the exponent can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntAbsolute(rhs, exponent, ool->entry());

  // x ** y with x > 1 and y >= DigitBits can't be pointer-sized.
  masm.branchPtr(Assembler::AboveOrEqual, exponent, Imm32(BigInt::DigitBits),
                 ool->entry());

  // x ** 1n == x
  Label rhsNotOne;
  masm.branch32(Assembler::NotEqual, exponent, Imm32(1), &rhsNotOne);
  {
    masm.movePtr(lhs, output);
    masm.jump(ool->rejoin());
  }
  masm.bind(&rhsNotOne);

  // Call into the VM when the base operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntNonZero(lhs, base, ool->entry());

  // MacroAssembler::pow32() adjusted to work on pointer-sized registers.
  {
    // m = base
    // n = exponent

    Label start, loop;
    masm.jump(&start);
    masm.bind(&loop);

    // m *= m
    masm.branchMulPtr(Assembler::Overflow, base, base, ool->entry());

    masm.bind(&start);

    // if ((n & 1) != 0) p *= m
    Label even;
    masm.branchTest32(Assembler::Zero, exponent, Imm32(1), &even);
    masm.branchMulPtr(Assembler::Overflow, base, dest, ool->entry());
    masm.bind(&even);

    // n >>= 1
    // if (n == 0) return p
    masm.branchRshift32(Assembler::NonZero, Imm32(1), exponent, &loop);
  }

  MOZ_ASSERT(temp1 == dest);

  // Create and return the result.
  masm.bind(&done);
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntBitAnd(LBigIntBitAnd* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::bitAnd>(ins, ArgList(lhs, rhs),
                                            StoreRegisterTo(output));

  // 0n & x == 0n
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // x & 0n == 0n
  Label rhsNonZero;
  masm.branchIfBigIntIsNonZero(rhs, &rhsNonZero);
  masm.movePtr(rhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&rhsNonZero);

  // Call into the VM when either operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntNonZero(lhs, temp1, ool->entry());
  masm.loadBigIntNonZero(rhs, temp2, ool->entry());

  masm.andPtr(temp2, temp1);

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntBitOr(LBigIntBitOr* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::bitOr>(ins, ArgList(lhs, rhs),
                                           StoreRegisterTo(output));

  // 0n | x == x
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(rhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // x | 0n == x
  Label rhsNonZero;
  masm.branchIfBigIntIsNonZero(rhs, &rhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&rhsNonZero);

  // Call into the VM when either operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntNonZero(lhs, temp1, ool->entry());
  masm.loadBigIntNonZero(rhs, temp2, ool->entry());

  masm.orPtr(temp2, temp1);

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntBitXor(LBigIntBitXor* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::bitXor>(ins, ArgList(lhs, rhs),
                                            StoreRegisterTo(output));

  // 0n ^ x == x
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(rhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // x ^ 0n == x
  Label rhsNonZero;
  masm.branchIfBigIntIsNonZero(rhs, &rhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&rhsNonZero);

  // Call into the VM when either operand can't be loaded into a pointer-sized
  // register.
  masm.loadBigIntNonZero(lhs, temp1, ool->entry());
  masm.loadBigIntNonZero(rhs, temp2, ool->entry());

  masm.xorPtr(temp2, temp1);

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntLsh(LBigIntLsh* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::lsh>(ins, ArgList(lhs, rhs),
                                         StoreRegisterTo(output));

  // 0n << x == 0n
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // x << 0n == x
  Label rhsNonZero;
  masm.branchIfBigIntIsNonZero(rhs, &rhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&rhsNonZero);

  // Inline |BigInt::lsh| for the case when |lhs| contains a single digit.

  Label rhsTooLarge;
  masm.loadBigIntAbsolute(rhs, temp2, &rhsTooLarge);

  // Call into the VM when the left-hand side operand can't be loaded into a
  // pointer-sized register.
  masm.loadBigIntAbsolute(lhs, temp1, ool->entry());

  // Handle shifts exceeding |BigInt::DigitBits| first.
  Label shift, create;
  masm.branchPtr(Assembler::Below, temp2, Imm32(BigInt::DigitBits), &shift);
  {
    masm.bind(&rhsTooLarge);

    // x << DigitBits with x != 0n always exceeds pointer-sized storage.
    masm.branchIfBigIntIsNonNegative(rhs, ool->entry());

    // x << -DigitBits == x >> DigitBits, which is either 0n or -1n.
    masm.move32(Imm32(0), temp1);
    masm.branchIfBigIntIsNonNegative(lhs, &create);
    masm.move32(Imm32(1), temp1);
    masm.jump(&create);
  }
  masm.bind(&shift);

  Label nonNegative;
  masm.branchIfBigIntIsNonNegative(rhs, &nonNegative);
  {
    masm.movePtr(temp1, temp3);

    // |x << -y| is computed as |x >> y|.
    masm.rshiftPtr(temp2, temp1);

    // For negative numbers, round down if any bit was shifted out.
    masm.branchIfBigIntIsNonNegative(lhs, &create);

    // Compute |mask = (static_cast<Digit>(1) << shift) - 1|.
    masm.movePtr(ImmWord(-1), output);
    masm.lshiftPtr(temp2, output);
    masm.notPtr(output);

    // Add plus one when |(lhs.digit(0) & mask) != 0|.
    masm.branchTestPtr(Assembler::Zero, output, temp3, &create);
    masm.addPtr(ImmWord(1), temp1);
    masm.jump(&create);
  }
  masm.bind(&nonNegative);
  {
    masm.movePtr(temp2, temp3);

    // Compute |grow = lhs.digit(0) >> (DigitBits - shift)|.
    masm.negPtr(temp2);
    masm.addPtr(Imm32(BigInt::DigitBits), temp2);
    masm.movePtr(temp1, output);
    masm.rshiftPtr(temp2, output);

    // Call into the VM when any bit will be shifted out.
    masm.branchTestPtr(Assembler::NonZero, output, output, ool->entry());

    masm.movePtr(temp3, temp2);
    masm.lshiftPtr(temp2, temp1);
  }
  masm.bind(&create);

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigIntAbsolute(output, temp1);

  // Set the sign bit when the left-hand side is negative.
  masm.branchIfBigIntIsNonNegative(lhs, ool->rejoin());
  masm.or32(Imm32(BigInt::signBitMask()),
            Address(output, BigInt::offsetOfFlags()));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntRsh(LBigIntRsh* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register temp3 = ToRegister(ins->temp3());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::rsh>(ins, ArgList(lhs, rhs),
                                         StoreRegisterTo(output));

  // 0n >> x == 0n
  Label lhsNonZero;
  masm.branchIfBigIntIsNonZero(lhs, &lhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&lhsNonZero);

  // x >> 0n == x
  Label rhsNonZero;
  masm.branchIfBigIntIsNonZero(rhs, &rhsNonZero);
  masm.movePtr(lhs, output);
  masm.jump(ool->rejoin());
  masm.bind(&rhsNonZero);

  // Inline |BigInt::rsh| for the case when |lhs| contains a single digit.

  Label rhsTooLarge;
  masm.loadBigIntAbsolute(rhs, temp2, &rhsTooLarge);

  // Call into the VM when the left-hand side operand can't be loaded into a
  // pointer-sized register.
  masm.loadBigIntAbsolute(lhs, temp1, ool->entry());

  // Handle shifts exceeding |BigInt::DigitBits| first.
  Label shift, create;
  masm.branchPtr(Assembler::Below, temp2, Imm32(BigInt::DigitBits), &shift);
  {
    masm.bind(&rhsTooLarge);

    // x >> -DigitBits == x << DigitBits, which exceeds pointer-sized storage.
    masm.branchIfBigIntIsNegative(rhs, ool->entry());

    // x >> DigitBits is either 0n or -1n.
    masm.move32(Imm32(0), temp1);
    masm.branchIfBigIntIsNonNegative(lhs, &create);
    masm.move32(Imm32(1), temp1);
    masm.jump(&create);
  }
  masm.bind(&shift);

  Label nonNegative;
  masm.branchIfBigIntIsNonNegative(rhs, &nonNegative);
  {
    masm.movePtr(temp2, temp3);

    // Compute |grow = lhs.digit(0) >> (DigitBits - shift)|.
    masm.negPtr(temp2);
    masm.addPtr(Imm32(BigInt::DigitBits), temp2);
    masm.movePtr(temp1, output);
    masm.rshiftPtr(temp2, output);

    // Call into the VM when any bit will be shifted out.
    masm.branchTestPtr(Assembler::NonZero, output, output, ool->entry());

    // |x >> -y| is computed as |x << y|.
    masm.movePtr(temp3, temp2);
    masm.lshiftPtr(temp2, temp1);
    masm.jump(&create);
  }
  masm.bind(&nonNegative);
  {
    masm.movePtr(temp1, temp3);

    masm.rshiftPtr(temp2, temp1);

    // For negative numbers, round down if any bit was shifted out.
    masm.branchIfBigIntIsNonNegative(lhs, &create);

    // Compute |mask = (static_cast<Digit>(1) << shift) - 1|.
    masm.movePtr(ImmWord(-1), output);
    masm.lshiftPtr(temp2, output);
    masm.notPtr(output);

    // Add plus one when |(lhs.digit(0) & mask) != 0|.
    masm.branchTestPtr(Assembler::Zero, output, temp3, &create);
    masm.addPtr(ImmWord(1), temp1);
  }
  masm.bind(&create);

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigIntAbsolute(output, temp1);

  // Set the sign bit when the left-hand side is negative.
  masm.branchIfBigIntIsNonNegative(lhs, ool->rejoin());
  masm.or32(Imm32(BigInt::signBitMask()),
            Address(output, BigInt::offsetOfFlags()));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntIncrement(LBigIntIncrement* ins) {
  Register input = ToRegister(ins->input());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  auto* ool =
      oolCallVM<Fn, BigInt::inc>(ins, ArgList(input), StoreRegisterTo(output));

  // Call into the VM when the input can't be loaded into a pointer-sized
  // register.
  masm.loadBigInt(input, temp1, ool->entry());
  masm.movePtr(ImmWord(1), temp2);

  masm.branchAddPtr(Assembler::Overflow, temp2, temp1, ool->entry());

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntDecrement(LBigIntDecrement* ins) {
  Register input = ToRegister(ins->input());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  auto* ool =
      oolCallVM<Fn, BigInt::dec>(ins, ArgList(input), StoreRegisterTo(output));

  // Call into the VM when the input can't be loaded into a pointer-sized
  // register.
  masm.loadBigInt(input, temp1, ool->entry());
  masm.movePtr(ImmWord(1), temp2);

  masm.branchSubPtr(Assembler::Overflow, temp2, temp1, ool->entry());

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigInt(output, temp1);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntNegate(LBigIntNegate* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp());
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
  masm.copyBigIntWithInlineDigits(input, output, temp, ool->entry(),
                                  bigIntsCanBeInNursery());

  // Flip the sign bit.
  masm.xor32(Imm32(BigInt::signBitMask()),
             Address(output, BigInt::offsetOfFlags()));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitBigIntBitNot(LBigIntBitNot* ins) {
  Register input = ToRegister(ins->input());
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());
  Register output = ToRegister(ins->output());

  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  auto* ool = oolCallVM<Fn, BigInt::bitNot>(ins, ArgList(input),
                                            StoreRegisterTo(output));

  masm.loadBigIntAbsolute(input, temp1, ool->entry());

  // This follows the C++ implementation because it let's us support the full
  // range [-2^64, 2^64 - 1] on 64-bit resp. [-2^32, 2^32 - 1] on 32-bit.
  Label nonNegative, done;
  masm.branchIfBigIntIsNonNegative(input, &nonNegative);
  {
    // ~(-x) == ~(~(x-1)) == x-1
    masm.subPtr(Imm32(1), temp1);
    masm.jump(&done);
  }
  masm.bind(&nonNegative);
  {
    // ~x == -x-1 == -(x+1)
    masm.movePtr(ImmWord(1), temp2);
    masm.branchAddPtr(Assembler::CarrySet, temp2, temp1, ool->entry());
  }
  masm.bind(&done);

  // Create and return the result.
  masm.newGCBigInt(output, temp2, ool->entry(), bigIntsCanBeInNursery());
  masm.initializeBigIntAbsolute(output, temp1);

  // Set the sign bit when the input is positive.
  masm.branchIfBigIntIsNegative(input, ool->rejoin());
  masm.or32(Imm32(BigInt::signBitMask()),
            Address(output, BigInt::offsetOfFlags()));

  masm.bind(ool->rejoin());
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
  FloatRegister temp = ToFloatRegister(lir->temp());
  Register output = ToRegister(lir->output());

  Label bail;
  masm.roundDoubleToInt32(input, output, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitRoundF(LRoundF* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  FloatRegister temp = ToFloatRegister(lir->temp());
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

void CodeGenerator::visitCompareBigInt(LCompareBigInt* lir) {
  JSOp op = lir->mir()->jsop();
  Register left = ToRegister(lir->left());
  Register right = ToRegister(lir->right());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register temp3 = ToRegister(lir->temp3());
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

  // Jump to |notSameSign| when the sign aren't the same.
  masm.load32(Address(left, BigInt::offsetOfFlags()), temp1);
  masm.xor32(Address(right, BigInt::offsetOfFlags()), temp1);
  masm.branchTest32(Assembler::NonZero, temp1, Imm32(BigInt::signBitMask()),
                    notSameSign);

  // Jump to |notSameLength| when the digits length is different.
  masm.load32(Address(right, BigInt::offsetOfLength()), temp1);
  masm.branch32(Assembler::NotEqual, Address(left, BigInt::offsetOfLength()),
                temp1, notSameLength);

  // Both BigInts have the same sign and the same number of digits. Loop over
  // each digit, starting with the left-most one, and break from the loop when
  // the first non-matching digit was found.

  masm.loadBigIntDigits(left, temp2);
  masm.loadBigIntDigits(right, temp3);

  static_assert(sizeof(BigInt::Digit) == sizeof(void*),
                "BigInt::Digit is pointer sized");

  masm.computeEffectiveAddress(BaseIndex(temp2, temp1, ScalePointer), temp2);
  masm.computeEffectiveAddress(BaseIndex(temp3, temp1, ScalePointer), temp3);

  Label start, loop;
  masm.jump(&start);
  masm.bind(&loop);

  masm.subPtr(Imm32(sizeof(BigInt::Digit)), temp2);
  masm.subPtr(Imm32(sizeof(BigInt::Digit)), temp3);

  masm.loadPtr(Address(temp3, 0), output);
  masm.branchPtr(Assembler::NotEqual, Address(temp2, 0), output, notSameDigit);

  masm.bind(&start);
  masm.branchSub32(Assembler::NotSigned, Imm32(1), temp1, &loop);

  // No different digits were found, both BigInts are equal to each other.

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
    // |temp1| holds the digits length of the right-hand side operand.
    masm.bind(&compareLength);
    masm.cmp32Set(JSOpToCondition(op, /* isSigned = */ false),
                  Address(left, BigInt::offsetOfLength()), temp1, output);
    masm.jump(&invertWhenNegative);

    // Similar to the case above, compare the current digit to determine the
    // overall comparison result.
    //
    // |temp2| points to the current digit of the left-hand side operand.
    // |output| holds the current digit of the right-hand side operand.
    masm.bind(&compareDigit);
    masm.cmpPtrSet(JSOpToCondition(op, /* isSigned = */ false),
                   Address(temp2, 0), output, output);

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
  Register right = ToRegister(lir->right());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register output = ToRegister(lir->output());

  Label ifTrue, ifFalse;
  masm.compareBigIntAndInt32(op, left, right, temp1, temp2, &ifTrue, &ifFalse);

  Label done;
  masm.bind(&ifFalse);
  masm.move32(Imm32(0), output);
  masm.jump(&done);
  masm.bind(&ifTrue);
  masm.move32(Imm32(1), output);
  masm.bind(&done);
}

void CodeGenerator::visitCompareBigIntDouble(LCompareBigIntDouble* lir) {
  JSOp op = lir->mir()->jsop();
  Register left = ToRegister(lir->left());
  FloatRegister right = ToFloatRegister(lir->right());
  Register temp = ToRegister(lir->temp());
  Register output = ToRegister(lir->output());

  masm.setupUnalignedABICall(temp);

  // Push the operands in reverse order for JSOp::Le and JSOp::Gt:
  // - |left <= right| is implemented as |right >= left|.
  // - |left > right| is implemented as |right < left|.
  if (op == JSOp::Le || op == JSOp::Gt) {
    masm.passABIArg(right, MoveOp::DOUBLE);
    masm.passABIArg(left);
  } else {
    masm.passABIArg(left);
    masm.passABIArg(right, MoveOp::DOUBLE);
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
      constexpr auto Equal = EqualityKind::Equal;
      callVM<FnBigIntString, BigIntStringEqual<Equal>>(lir);
      break;
    }
    case JSOp::Ne: {
      constexpr auto NotEqual = EqualityKind::NotEqual;
      callVM<FnBigIntString, BigIntStringEqual<NotEqual>>(lir);
      break;
    }
    case JSOp::Lt: {
      constexpr auto LessThan = ComparisonKind::LessThan;
      callVM<FnBigIntString, BigIntStringCompare<LessThan>>(lir);
      break;
    }
    case JSOp::Gt: {
      constexpr auto LessThan = ComparisonKind::LessThan;
      callVM<FnStringBigInt, StringBigIntCompare<LessThan>>(lir);
      break;
    }
    case JSOp::Le: {
      constexpr auto GreaterThanOrEqual = ComparisonKind::GreaterThanOrEqual;
      callVM<FnStringBigInt, StringBigIntCompare<GreaterThanOrEqual>>(lir);
      break;
    }
    case JSOp::Ge: {
      constexpr auto GreaterThanOrEqual = ComparisonKind::GreaterThanOrEqual;
      callVM<FnBigIntString, BigIntStringCompare<GreaterThanOrEqual>>(lir);
      break;
    }
    default:
      MOZ_CRASH("Unexpected compare op");
  }
}

void CodeGenerator::visitIsNullOrLikeUndefinedV(LIsNullOrLikeUndefinedV* lir) {
  JSOp op = lir->mir()->jsop();
  MCompare::CompareType compareType = lir->mir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Undefined ||
             compareType == MCompare::Compare_Null);

  const ValueOperand value = ToValue(lir, LIsNullOrLikeUndefinedV::Value);
  Register output = ToRegister(lir->output());

  if (op == JSOp::Eq || op == JSOp::Ne) {
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
        masm.extractObject(value, ToTempUnboxRegister(lir->tempToUnbox()));
    branchTestObjectEmulatesUndefined(objreg, nullOrLikeUndefined,
                                      notNullOrLikeUndefined,
                                      ToRegister(lir->temp()), ool);
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
    return;
  }

  MOZ_ASSERT(op == JSOp::StrictEq || op == JSOp::StrictNe);

  Assembler::Condition cond = JSOpToCondition(compareType, op);
  if (compareType == MCompare::Compare_Null) {
    masm.testNullSet(cond, value, output);
  } else {
    masm.testUndefinedSet(cond, value, output);
  }
}

void CodeGenerator::visitIsNullOrLikeUndefinedAndBranchV(
    LIsNullOrLikeUndefinedAndBranchV* lir) {
  JSOp op = lir->cmpMir()->jsop();
  MCompare::CompareType compareType = lir->cmpMir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Undefined ||
             compareType == MCompare::Compare_Null);

  const ValueOperand value =
      ToValue(lir, LIsNullOrLikeUndefinedAndBranchV::Value);

  if (op == JSOp::Eq || op == JSOp::Ne) {
    MBasicBlock* ifTrue;
    MBasicBlock* ifFalse;

    if (op == JSOp::Eq) {
      ifTrue = lir->ifTrue();
      ifFalse = lir->ifFalse();
    } else {
      // Swap branches.
      ifTrue = lir->ifFalse();
      ifFalse = lir->ifTrue();
      op = JSOp::Eq;
    }

    auto* ool = new (alloc()) OutOfLineTestObject();
    addOutOfLineCode(ool, lir->cmpMir());

    Label* ifTrueLabel = getJumpLabelForBranch(ifTrue);
    Label* ifFalseLabel = getJumpLabelForBranch(ifFalse);

    {
      ScratchTagScope tag(masm, value);
      masm.splitTagForTest(value, tag);

      masm.branchTestNull(Assembler::Equal, tag, ifTrueLabel);
      masm.branchTestUndefined(Assembler::Equal, tag, ifTrueLabel);

      masm.branchTestObject(Assembler::NotEqual, tag, ifFalseLabel);
    }

    // Objects that emulate undefined are loosely equal to null/undefined.
    Register objreg =
        masm.extractObject(value, ToTempUnboxRegister(lir->tempToUnbox()));
    Register scratch = ToRegister(lir->temp());
    testObjectEmulatesUndefined(objreg, ifTrueLabel, ifFalseLabel, scratch,
                                ool);
    return;
  }

  MOZ_ASSERT(op == JSOp::StrictEq || op == JSOp::StrictNe);

  Assembler::Condition cond = JSOpToCondition(compareType, op);
  if (compareType == MCompare::Compare_Null) {
    testNullEmitBranch(cond, value, lir->ifTrue(), lir->ifFalse());
  } else {
    testUndefinedEmitBranch(cond, value, lir->ifTrue(), lir->ifFalse());
  }
}

void CodeGenerator::visitIsNullOrLikeUndefinedT(LIsNullOrLikeUndefinedT* lir) {
  MOZ_ASSERT(lir->mir()->compareType() == MCompare::Compare_Undefined ||
             lir->mir()->compareType() == MCompare::Compare_Null);
  MOZ_ASSERT(lir->mir()->lhs()->type() == MIRType::Object);

  JSOp op = lir->mir()->jsop();
  MOZ_ASSERT(op == JSOp::Eq || op == JSOp::Ne,
             "Strict equality should have been folded");

  Register objreg = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

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
}

void CodeGenerator::visitIsNullOrLikeUndefinedAndBranchT(
    LIsNullOrLikeUndefinedAndBranchT* lir) {
  DebugOnly<MCompare::CompareType> compareType = lir->cmpMir()->compareType();
  MOZ_ASSERT(compareType == MCompare::Compare_Undefined ||
             compareType == MCompare::Compare_Null);
  MOZ_ASSERT(lir->cmpMir()->lhs()->type() == MIRType::Object);

  JSOp op = lir->cmpMir()->jsop();
  MOZ_ASSERT(op == JSOp::Eq || op == JSOp::Ne,
             "Strict equality should have been folded");

  MBasicBlock* ifTrue;
  MBasicBlock* ifFalse;

  if (op == JSOp::Eq || op == JSOp::StrictEq) {
    ifTrue = lir->ifTrue();
    ifFalse = lir->ifFalse();
  } else {
    // Swap branches.
    ifTrue = lir->ifFalse();
    ifFalse = lir->ifTrue();
  }

  Register input = ToRegister(lir->getOperand(0));

  auto* ool = new (alloc()) OutOfLineTestObject();
  addOutOfLineCode(ool, lir->cmpMir());

  Label* ifTrueLabel = getJumpLabelForBranch(ifTrue);
  Label* ifFalseLabel = getJumpLabelForBranch(ifFalse);

  // Objects that emulate undefined are loosely equal to null/undefined.
  Register scratch = ToRegister(lir->temp());
  testObjectEmulatesUndefined(input, ifTrueLabel, ifFalseLabel, scratch, ool);
}

void CodeGenerator::visitSameValueDouble(LSameValueDouble* lir) {
  FloatRegister left = ToFloatRegister(lir->left());
  FloatRegister right = ToFloatRegister(lir->right());
  FloatRegister temp = ToFloatRegister(lir->tempFloat());
  Register output = ToRegister(lir->output());

  masm.sameValueDouble(left, right, temp, output);
}

void CodeGenerator::visitSameValue(LSameValue* lir) {
  ValueOperand lhs = ToValue(lir, LSameValue::LhsIndex);
  ValueOperand rhs = ToValue(lir, LSameValue::RhsIndex);
  Register output = ToRegister(lir->output());

  Label call, done;

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
  using Fn = JSString* (*)(JSContext*, HandleString, HandleString,
                           js::gc::InitialHeap);
  OutOfLineCode* ool = oolCallVM<Fn, ConcatStrings<CanGC>>(
      lir, ArgList(lhs, rhs, static_cast<Imm32>(gc::DefaultHeap)),
      StoreRegisterTo(output));

  const JitRealm* jitRealm = gen->realm->jitRealm();
  JitCode* stringConcatStub =
      jitRealm->stringConcatStubNoBarrier(&realmStubsToReadBarrier_);
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
  MOZ_ASSERT(ToRegister(lir->temp1()) == CallTempReg0);
  MOZ_ASSERT(ToRegister(lir->temp2()) == CallTempReg1);
  MOZ_ASSERT(ToRegister(lir->temp3()) == CallTempReg2);
  MOZ_ASSERT(ToRegister(lir->temp4()) == CallTempReg3);
  MOZ_ASSERT(ToRegister(lir->temp5()) == CallTempReg4);
  MOZ_ASSERT(output == CallTempReg5);

  emitConcat(lir, lhs, rhs, output);
}

static void CopyStringChars(MacroAssembler& masm, Register to, Register from,
                            Register len, Register byteOpScratch,
                            CharEncoding fromEncoding,
                            CharEncoding toEncoding) {
  // Copy |len| char16_t code units from |from| to |to|. Assumes len > 0
  // (checked below in debug builds), and when done |to| must point to the
  // next available char.

#ifdef DEBUG
  Label ok;
  masm.branch32(Assembler::GreaterThan, len, Imm32(0), &ok);
  masm.assumeUnreachable("Length should be greater than 0.");
  masm.bind(&ok);
#endif

  MOZ_ASSERT_IF(toEncoding == CharEncoding::Latin1,
                fromEncoding == CharEncoding::Latin1);

  size_t fromWidth =
      fromEncoding == CharEncoding::Latin1 ? sizeof(char) : sizeof(char16_t);
  size_t toWidth =
      toEncoding == CharEncoding::Latin1 ? sizeof(char) : sizeof(char16_t);

  Label start;
  masm.bind(&start);
  masm.loadChar(Address(from, 0), byteOpScratch, fromEncoding);
  masm.storeChar(byteOpScratch, Address(to, 0), toEncoding);
  masm.addPtr(Imm32(fromWidth), from);
  masm.addPtr(Imm32(toWidth), to);
  masm.branchSub32(Assembler::NonZero, Imm32(1), len, &start);
}

static void CopyStringChars(MacroAssembler& masm, Register to, Register from,
                            Register len, Register byteOpScratch,
                            CharEncoding encoding) {
  CopyStringChars(masm, to, from, len, byteOpScratch, encoding, encoding);
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

static void ConcatInlineString(MacroAssembler& masm, Register lhs, Register rhs,
                               Register output, Register temp1, Register temp2,
                               Register temp3, bool stringsCanBeInNursery,
                               Label* failure, CharEncoding encoding) {
  JitSpew(JitSpew_Codegen, "# Emitting ConcatInlineString (encoding=%s)",
          (encoding == CharEncoding::Latin1 ? "Latin-1" : "Two-Byte"));

  // State: result length in temp2.

  // Ensure both strings are linear.
  masm.branchIfRope(lhs, failure);
  masm.branchIfRope(rhs, failure);

  // Allocate a JSThinInlineString or JSFatInlineString.
  size_t maxThinInlineLength;
  if (encoding == CharEncoding::Latin1) {
    maxThinInlineLength = JSThinInlineString::MAX_LENGTH_LATIN1;
  } else {
    maxThinInlineLength = JSThinInlineString::MAX_LENGTH_TWO_BYTE;
  }

  Label isFat, allocDone;
  masm.branch32(Assembler::Above, temp2, Imm32(maxThinInlineLength), &isFat);
  {
    uint32_t flags = JSString::INIT_THIN_INLINE_FLAGS;
    if (encoding == CharEncoding::Latin1) {
      flags |= JSString::LATIN1_CHARS_BIT;
    }
    masm.newGCString(output, temp1, failure, stringsCanBeInNursery);
    masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
    masm.jump(&allocDone);
  }
  masm.bind(&isFat);
  {
    uint32_t flags = JSString::INIT_FAT_INLINE_FLAGS;
    if (encoding == CharEncoding::Latin1) {
      flags |= JSString::LATIN1_CHARS_BIT;
    }
    masm.newGCFatInlineString(output, temp1, failure, stringsCanBeInNursery);
    masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
  }
  masm.bind(&allocDone);

  // Store length.
  masm.store32(temp2, Address(output, JSString::offsetOfLength()));

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

  masm.ret();
}

void CodeGenerator::visitSubstr(LSubstr* lir) {
  Register string = ToRegister(lir->string());
  Register begin = ToRegister(lir->begin());
  Register length = ToRegister(lir->length());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp());
  Register temp3 = ToRegister(lir->temp3());

  // On x86 there are not enough registers. In that case reuse the string
  // register as temporary.
  Register temp2 =
      lir->temp2()->isBogusTemp() ? string : ToRegister(lir->temp2());

  Address stringFlags(string, JSString::offsetOfFlags());

  Label isLatin1, notInline, nonZero, isInlinedLatin1;

  // For every edge case use the C++ variant.
  // Note: we also use this upon allocation failure in newGCString and
  // newGCFatInlineString. To squeeze out even more performance those failures
  // can be handled by allocate in ool code and returning to jit code to fill
  // in all data.
  using Fn = JSString* (*)(JSContext * cx, HandleString str, int32_t begin,
                           int32_t len);
  OutOfLineCode* ool = oolCallVM<Fn, SubstringKernel>(
      lir, ArgList(string, begin, length), StoreRegisterTo(output));
  Label* slowPath = ool->entry();
  Label* done = ool->rejoin();

  // Zero length, return emptystring.
  masm.branchTest32(Assembler::NonZero, length, length, &nonZero);
  const JSAtomState& names = gen->runtime->names();
  masm.movePtr(ImmGCPtr(names.empty), output);
  masm.jump(done);

  // Use slow path for ropes.
  masm.bind(&nonZero);
  masm.branchIfRope(string, slowPath);

  // Handle inlined strings by creating a FatInlineString.
  masm.branchTest32(Assembler::Zero, stringFlags,
                    Imm32(JSString::INLINE_CHARS_BIT), &notInline);
  masm.newGCFatInlineString(output, temp, slowPath, stringsCanBeInNursery());
  masm.store32(length, Address(output, JSString::offsetOfLength()));

  auto initializeFatInlineString = [&](CharEncoding encoding) {
    uint32_t flags = JSString::INIT_FAT_INLINE_FLAGS;
    if (encoding == CharEncoding::Latin1) {
      flags |= JSString::LATIN1_CHARS_BIT;
    }

    masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
    masm.loadInlineStringChars(string, temp, encoding);
    masm.addToCharPtr(temp, begin, encoding);
    if (temp2 == string) {
      masm.push(string);
    }
    masm.loadInlineStringCharsForStore(output, temp2);
    CopyStringChars(masm, temp2, temp, length, temp3, encoding);
    masm.loadStringLength(output, length);
    if (temp2 == string) {
      masm.pop(string);
    }
    masm.jump(done);
  };

  masm.branchLatin1String(string, &isInlinedLatin1);
  { initializeFatInlineString(CharEncoding::TwoByte); }
  masm.bind(&isInlinedLatin1);
  { initializeFatInlineString(CharEncoding::Latin1); }

  // Handle other cases with a DependentString.
  masm.bind(&notInline);
  masm.newGCString(output, temp, slowPath, gen->stringsCanBeInNursery());
  masm.store32(length, Address(output, JSString::offsetOfLength()));
  masm.storeDependentStringBase(string, output);

  auto initializeDependentString = [&](CharEncoding encoding) {
    uint32_t flags = JSString::INIT_DEPENDENT_FLAGS;
    if (encoding == CharEncoding::Latin1) {
      flags |= JSString::LATIN1_CHARS_BIT;
    }

    masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
    masm.loadNonInlineStringChars(string, temp, encoding);
    masm.addToCharPtr(temp, begin, encoding);
    masm.storeNonInlineStringChars(temp, output);
    masm.jump(done);
  };

  masm.branchLatin1String(string, &isLatin1);
  { initializeDependentString(CharEncoding::TwoByte); }
  masm.bind(&isLatin1);
  { initializeDependentString(CharEncoding::Latin1); }

  masm.bind(done);
}

JitCode* JitRealm::generateStringConcatStub(JSContext* cx) {
  JitSpew(JitSpew_Codegen, "# Emitting StringConcat stub");

  StackMacroAssembler masm(cx);

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
  // If lhs is empty, return rhs.
  Label leftEmpty;
  masm.loadStringLength(lhs, temp1);
  masm.branchTest32(Assembler::Zero, temp1, temp1, &leftEmpty);

  // If rhs is empty, return lhs.
  Label rightEmpty;
  masm.loadStringLength(rhs, temp2);
  masm.branchTest32(Assembler::Zero, temp2, temp2, &rightEmpty);

  masm.add32(temp1, temp2);

  // Check if we can use a JSFatInlineString. The result is a Latin1 string if
  // lhs and rhs are both Latin1, so we AND the flags.
  Label isFatInlineTwoByte, isFatInlineLatin1;
  masm.load32(Address(lhs, JSString::offsetOfFlags()), temp1);
  masm.and32(Address(rhs, JSString::offsetOfFlags()), temp1);

  Label isLatin1, notInline;
  masm.branchTest32(Assembler::NonZero, temp1,
                    Imm32(JSString::LATIN1_CHARS_BIT), &isLatin1);
  {
    masm.branch32(Assembler::BelowOrEqual, temp2,
                  Imm32(JSFatInlineString::MAX_LENGTH_TWO_BYTE),
                  &isFatInlineTwoByte);
    masm.jump(&notInline);
  }
  masm.bind(&isLatin1);
  {
    masm.branch32(Assembler::BelowOrEqual, temp2,
                  Imm32(JSFatInlineString::MAX_LENGTH_LATIN1),
                  &isFatInlineLatin1);
  }
  masm.bind(&notInline);

  // Keep AND'ed flags in temp1.

  // Ensure result length <= JSString::MAX_LENGTH.
  masm.branch32(Assembler::Above, temp2, Imm32(JSString::MAX_LENGTH), &failure);

  // Allocate a new rope, guaranteed to be in the nursery if
  // stringsCanBeInNursery. (As a result, no post barriers are needed below.)
  masm.newGCString(output, temp3, &failure, stringsCanBeInNursery);

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
  masm.ret();

  masm.bind(&leftEmpty);
  masm.mov(rhs, output);
  masm.ret();

  masm.bind(&rightEmpty);
  masm.mov(lhs, output);
  masm.ret();

  masm.bind(&isFatInlineTwoByte);
  ConcatInlineString(masm, lhs, rhs, output, temp1, temp2, temp3,
                     stringsCanBeInNursery, &failure, CharEncoding::TwoByte);

  masm.bind(&isFatInlineLatin1);
  ConcatInlineString(masm, lhs, rhs, output, temp1, temp2, temp3,
                     stringsCanBeInNursery, &failure, CharEncoding::Latin1);

  masm.pop(temp2);
  masm.pop(temp1);

  masm.bind(&failure);
  masm.movePtr(ImmPtr(nullptr), output);
  masm.ret();

  Linker linker(masm);
  JitCode* code = linker.newCode(cx, CodeKind::Other);

#ifdef JS_ION_PERF
  writePerfSpewerJitCodeProfile(code, "StringConcatStub");
#endif
#ifdef MOZ_VTUNE
  vtune::MarkStub(code, "StringConcatStub");
#endif

  return code;
}

void JitRuntime::generateFreeStub(MacroAssembler& masm) {
  const Register regSlots = CallTempReg0;

  freeStubOffset_ = startTrampolineCode(masm);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  regs.takeUnchecked(regSlots);
  LiveRegisterSet save(regs.asLiveSet());
  masm.PushRegsInMask(save);

  const Register regTemp = regs.takeAnyGeneral();
  MOZ_ASSERT(regTemp != regSlots);

  using Fn = void (*)(void* p);
  masm.setupUnalignedABICall(regTemp);
  masm.passABIArg(regSlots);
  masm.callWithABI<Fn, js_free>(MoveOp::GENERAL,
                                CheckUnsafeCallWithABI::DontCheckOther);

  masm.PopRegsInMask(save);

  masm.ret();
}

void JitRuntime::generateLazyLinkStub(MacroAssembler& masm) {
  lazyLinkStubOffset_ = startTrampolineCode(masm);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  Register temp0 = regs.takeAny();
  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();

  masm.loadJSContext(temp0);
  masm.enterFakeExitFrame(temp0, temp2, ExitFrameType::LazyLink);
  masm.moveStackPtrTo(temp1);

  using Fn = uint8_t* (*)(JSContext * cx, LazyLinkExitFrameLayout * frame);
  masm.setupUnalignedABICall(temp2);
  masm.passABIArg(temp0);
  masm.passABIArg(temp1);
  masm.callWithABI<Fn, LazyLinkTopActivation>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  masm.leaveExitFrame();

#ifdef JS_USE_LINK_REGISTER
  // Restore the return address such that the emitPrologue function of the
  // CodeGenerator can push it back on the stack with pushReturnAddress.
  masm.popReturnAddress();
#endif
  masm.jump(ReturnReg);
}

void JitRuntime::generateInterpreterStub(MacroAssembler& masm) {
  interpreterStubOffset_ = startTrampolineCode(masm);

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  Register temp0 = regs.takeAny();
  Register temp1 = regs.takeAny();
  Register temp2 = regs.takeAny();

  masm.loadJSContext(temp0);
  masm.enterFakeExitFrame(temp0, temp2, ExitFrameType::InterpreterStub);
  masm.moveStackPtrTo(temp1);

  using Fn = bool (*)(JSContext * cx, InterpreterStubExitFrameLayout * frame);
  masm.setupUnalignedABICall(temp2);
  masm.passABIArg(temp0);
  masm.passABIArg(temp1);
  masm.callWithABI<Fn, InvokeFromInterpreterStub>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  masm.branchIfFalseBool(ReturnReg, masm.failureLabel());
  masm.leaveExitFrame();

  // InvokeFromInterpreterStub stores the return value in argv[0], where the
  // caller stored |this|.
  masm.loadValue(
      Address(masm.getStackPointer(), JitFrameLayout::offsetOfThis()),
      JSReturnOperand);
  masm.ret();
}

void JitRuntime::generateDoubleToInt32ValueStub(MacroAssembler& masm) {
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

bool JitRuntime::generateTLEventVM(MacroAssembler& masm,
                                   const VMFunctionData& f, bool enter) {
#ifdef JS_TRACE_LOGGING
  bool vmEventEnabled = TraceLogTextIdEnabled(TraceLogger_VM);
  bool vmSpecificEventEnabled = TraceLogTextIdEnabled(TraceLogger_VMSpecific);

  if (vmEventEnabled || vmSpecificEventEnabled) {
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    Register loggerReg = regs.takeAnyGeneral();
    masm.Push(loggerReg);
    masm.loadTraceLogger(loggerReg);

    if (vmEventEnabled) {
      if (enter) {
        masm.tracelogStartId(loggerReg, TraceLogger_VM, /* force = */ true);
      } else {
        masm.tracelogStopId(loggerReg, TraceLogger_VM, /* force = */ true);
      }
    }
    if (vmSpecificEventEnabled) {
      TraceLoggerEvent event(f.name());
      if (!event.hasTextId()) {
        return false;
      }

      if (enter) {
        masm.tracelogStartId(loggerReg, event.textId(), /* force = */ true);
      } else {
        masm.tracelogStopId(loggerReg, event.textId(), /* force = */ true);
      }
    }

    masm.Pop(loggerReg);
  }
#endif

  return true;
}

void CodeGenerator::visitCharCodeAt(LCharCodeAt* lir) {
  Register str = ToRegister(lir->str());
  Register index = ToRegister(lir->index());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp());

  using Fn = bool (*)(JSContext*, HandleString, int32_t, uint32_t*);
  OutOfLineCode* ool = oolCallVM<Fn, jit::CharCodeAt>(lir, ArgList(str, index),
                                                      StoreRegisterTo(output));
  masm.loadStringChar(str, index, output, temp, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitFromCharCode(LFromCharCode* lir) {
  Register code = ToRegister(lir->code());
  Register output = ToRegister(lir->output());

  using Fn = JSLinearString* (*)(JSContext*, int32_t);
  OutOfLineCode* ool = oolCallVM<Fn, jit::StringFromCharCode>(
      lir, ArgList(code), StoreRegisterTo(output));

  // OOL path if code >= UNIT_STATIC_LIMIT.
  masm.boundsCheck32PowerOfTwo(code, StaticStrings::UNIT_STATIC_LIMIT,
                               ool->entry());

  masm.movePtr(ImmPtr(&gen->runtime->staticStrings().unitStaticTable), output);
  masm.loadPtr(BaseIndex(output, code, ScalePointer), output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitFromCodePoint(LFromCodePoint* lir) {
  Register codePoint = ToRegister(lir->codePoint());
  Register output = ToRegister(lir->output());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  LSnapshot* snapshot = lir->snapshot();

  // The OOL path is only taken when we can't allocate the inline string.
  using Fn = JSString* (*)(JSContext*, int32_t);
  OutOfLineCode* ool = oolCallVM<Fn, jit::StringFromCodePoint>(
      lir, ArgList(codePoint), StoreRegisterTo(output));

  Label isTwoByte;
  Label* done = ool->rejoin();

  static_assert(
      StaticStrings::UNIT_STATIC_LIMIT - 1 == JSString::MAX_LATIN1_CHAR,
      "Latin-1 strings can be loaded from static strings");
  masm.boundsCheck32PowerOfTwo(codePoint, StaticStrings::UNIT_STATIC_LIMIT,
                               &isTwoByte);
  {
    masm.movePtr(ImmPtr(&gen->runtime->staticStrings().unitStaticTable),
                 output);
    masm.loadPtr(BaseIndex(output, codePoint, ScalePointer), output);
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
      masm.newGCString(output, temp1, ool->entry(),
                       gen->stringsCanBeInNursery());
      masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
    }

    Label isSupplementary;
    masm.branch32(Assembler::AboveOrEqual, codePoint, Imm32(unicode::NonBMPMin),
                  &isSupplementary);
    {
      // Store length.
      masm.store32(Imm32(1), Address(output, JSString::offsetOfLength()));

      // Load chars pointer in temp1.
      masm.loadInlineStringCharsForStore(output, temp1);

      masm.store16(codePoint, Address(temp1, 0));

      masm.jump(done);
    }
    masm.bind(&isSupplementary);
    {
      // Store length.
      masm.store32(Imm32(2), Address(output, JSString::offsetOfLength()));

      // Load chars pointer in temp1.
      masm.loadInlineStringCharsForStore(output, temp1);

      // Inlined unicode::LeadSurrogate(uint32_t).
      masm.move32(codePoint, temp2);
      masm.rshift32(Imm32(10), temp2);
      masm.add32(Imm32(unicode::LeadSurrogateMin - (unicode::NonBMPMin >> 10)),
                 temp2);

      masm.store16(temp2, Address(temp1, 0));

      // Inlined unicode::TrailSurrogate(uint32_t).
      masm.move32(codePoint, temp2);
      masm.and32(Imm32(0x3FF), temp2);
      masm.or32(Imm32(unicode::TrailSurrogateMin), temp2);

      masm.store16(temp2, Address(temp1, sizeof(char16_t)));
    }
  }

  masm.bind(done);
}

void CodeGenerator::visitStringConvertCase(LStringConvertCase* lir) {
  pushArg(ToRegister(lir->string()));

  using Fn = JSString* (*)(JSContext*, HandleString);
  if (lir->mir()->mode() == MStringConvertCase::LowerCase) {
    callVM<Fn, js::StringToLowerCase>(lir);
  } else {
    callVM<Fn, js::StringToUpperCase>(lir);
  }
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

void CodeGenerator::visitNotBI(LNotBI* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());

  masm.cmp32Set(Assembler::Equal, Address(input, BigInt::offsetOfLength()),
                Imm32(0), output);
}

void CodeGenerator::visitNotO(LNotO* lir) {
  auto* ool = new (alloc()) OutOfLineTestObjectWithLabels();
  addOutOfLineCode(ool, lir->mir());

  Label* ifEmulatesUndefined = ool->label1();
  Label* ifDoesntEmulateUndefined = ool->label2();

  Register objreg = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
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

void CodeGenerator::visitNotV(LNotV* lir) {
  auto* ool = new (alloc()) OutOfLineTestObjectWithLabels();
  addOutOfLineCode(ool, lir->mir());

  Label* ifTruthy = ool->label1();
  Label* ifFalsy = ool->label2();

  TypeDataList observed = lir->mir()->observedTypes();
  testValueTruthyKernel(ToValue(lir, LNotV::Input), lir->temp1(), lir->temp2(),
                        ToFloatRegister(lir->tempFloat()), observed, ifTruthy,
                        ifFalsy, ool);

  Label join;
  Register output = ToRegister(lir->output());

  // Note that the testValueTruthyKernel call above may choose to fall through
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

    if (length->isRegister()) {
      bailoutCmpConstant(Assembler::BelowOrEqual, ToRegister(length), idx);
    } else {
      bailoutCmpConstant(Assembler::BelowOrEqual, ToAddress(length), idx);
    }
    return;
  }

  Register indexReg = ToRegister(index);
  if (length->isConstant()) {
    bailoutCmpConstant(Assembler::AboveOrEqual, indexReg, ToInt32(length));
  } else if (length->isRegister()) {
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
  Register temp = ToRegister(lir->getTemp(0));

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
      if (length->isRegister()) {
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

  if (length->isRegister()) {
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
    if (length->isRegister()) {
      masm.spectreMaskIndex32(index, ToRegister(length), output);
    } else {
      masm.spectreMaskIndex32(index, ToAddress(length), output);
    }
  } else {
    MOZ_ASSERT(lir->mir()->type() == MIRType::IntPtr);
    if (length->isRegister()) {
      masm.spectreMaskIndexPtr(index, ToRegister(length), output);
    } else {
      masm.spectreMaskIndexPtr(index, ToAddress(length), output);
    }
  }
}

class OutOfLineStoreElementHole : public OutOfLineCodeBase<CodeGenerator> {
  LInstruction* ins_;
  Label rejoinStore_;
  Label callStub_;
  bool strict_;

 public:
  explicit OutOfLineStoreElementHole(LInstruction* ins, bool strict)
      : ins_(ins), strict_(strict) {
    MOZ_ASSERT(ins->isStoreElementHoleV() || ins->isStoreElementHoleT());
  }

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineStoreElementHole(this);
  }
  LInstruction* ins() const { return ins_; }
  Label* rejoinStore() { return &rejoinStore_; }
  Label* callStub() { return &callStub_; }
  bool strict() const { return strict_; }
};

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
                                          MIRType valueType,
                                          MIRType elementType,
                                          Register elements,
                                          const LAllocation* index) {
  MOZ_ASSERT(valueType != MIRType::MagicHole);
  ConstantOrRegister v = ToConstantOrRegister(value, valueType);
  if (index->isConstant()) {
    Address dest(elements, ToInt32(index) * sizeof(js::Value));
    masm.storeUnboxedValue(v, valueType, dest, elementType);
  } else {
    BaseObjectElementIndex dest(elements, ToRegister(index));
    masm.storeUnboxedValue(v, valueType, dest, elementType);
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

  emitStoreElementTyped(store->value(), store->mir()->value()->type(),
                        store->mir()->elementType(), elements, index);
}

void CodeGenerator::visitStoreElementV(LStoreElementV* lir) {
  const ValueOperand value = ToValue(lir, LStoreElementV::Value);
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
  OutOfLineStoreElementHole* ool =
      new (alloc()) OutOfLineStoreElementHole(lir, current->mir()->strict());
  addOutOfLineCode(ool, lir->mir());

  Register elements = ToRegister(lir->elements());
  Register index = ToRegister(lir->index());
  Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

  Address initLength(elements, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, initLength, spectreTemp, ool->entry());

  if (lir->mir()->needsBarrier()) {
    emitPreBarrier(elements, lir->index());
  }

  masm.bind(ool->rejoinStore());
  emitStoreElementTyped(lir->value(), lir->mir()->value()->type(),
                        lir->mir()->elementType(), elements, lir->index());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitStoreElementHoleV(LStoreElementHoleV* lir) {
  OutOfLineStoreElementHole* ool =
      new (alloc()) OutOfLineStoreElementHole(lir, current->mir()->strict());
  addOutOfLineCode(ool, lir->mir());

  Register elements = ToRegister(lir->elements());
  Register index = ToRegister(lir->index());
  const ValueOperand value = ToValue(lir, LStoreElementHoleV::Value);
  Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

  Address initLength(elements, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, initLength, spectreTemp, ool->entry());

  if (lir->mir()->needsBarrier()) {
    emitPreBarrier(elements, lir->index());
  }

  masm.bind(ool->rejoinStore());
  masm.storeValue(value, BaseObjectElementIndex(elements, index));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineStoreElementHole(
    OutOfLineStoreElementHole* ool) {
  Register object, elements;
  LInstruction* ins = ool->ins();
  const LAllocation* index;
  MIRType valueType;
  mozilla::Maybe<ConstantOrRegister> value;
  Register spectreTemp;

  if (ins->isStoreElementHoleV()) {
    LStoreElementHoleV* store = ins->toStoreElementHoleV();
    object = ToRegister(store->object());
    elements = ToRegister(store->elements());
    index = store->index();
    valueType = store->mir()->value()->type();
    value.emplace(
        TypedOrValueRegister(ToValue(store, LStoreElementHoleV::Value)));
    spectreTemp = ToTempRegisterOrInvalid(store->spectreTemp());
  } else {
    LStoreElementHoleT* store = ins->toStoreElementHoleT();
    object = ToRegister(store->object());
    elements = ToRegister(store->elements());
    index = store->index();
    valueType = store->mir()->value()->type();
    if (store->value()->isConstant()) {
      value.emplace(
          ConstantOrRegister(store->value()->toConstant()->toJSValue()));
    } else {
      value.emplace(
          TypedOrValueRegister(valueType, ToAnyRegister(store->value())));
    }
    spectreTemp = ToTempRegisterOrInvalid(store->spectreTemp());
  }

  Register indexReg = ToRegister(index);

  // If index == initializedLength, try to bump the initialized length inline.
  // If index > initializedLength, call a stub. Note that this relies on the
  // condition flags sticking from the incoming branch.
  // Also note: this branch does not need Spectre mitigations, doing that for
  // the capacity check below is sufficient.
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  // Had to reimplement for MIPS because there are no flags.
  Address initLength(elements, ObjectElements::offsetOfInitializedLength());
  masm.branch32(Assembler::NotEqual, initLength, indexReg, ool->callStub());
#else
  masm.j(Assembler::NotEqual, ool->callStub());
#endif

  // Check array capacity.
  masm.spectreBoundsCheck32(
      indexReg, Address(elements, ObjectElements::offsetOfCapacity()),
      spectreTemp, ool->callStub());

  // Update initialized length. The capacity guard above ensures this won't
  // overflow, due to MAX_DENSE_ELEMENTS_COUNT.
  masm.add32(Imm32(1), indexReg);
  masm.store32(indexReg,
               Address(elements, ObjectElements::offsetOfInitializedLength()));

  // Update length if length < initializedLength.
  Label dontUpdate;
  masm.branch32(Assembler::AboveOrEqual,
                Address(elements, ObjectElements::offsetOfLength()), indexReg,
                &dontUpdate);
  masm.store32(indexReg, Address(elements, ObjectElements::offsetOfLength()));
  masm.bind(&dontUpdate);

  masm.sub32(Imm32(1), indexReg);

  if (ins->isStoreElementHoleT() && valueType != MIRType::Double) {
    // The inline path for StoreElementHoleT does not always store the type tag,
    // so we do the store on the OOL path. We use MIRType::None for the element
    // type so that emitStoreElementTyped will always store the type tag.
    emitStoreElementTyped(ins->toStoreElementHoleT()->value(), valueType,
                          MIRType::None, elements, index);
    masm.jump(ool->rejoin());
  } else {
    // Jump to the inline path where we will store the value.
    masm.jump(ool->rejoinStore());
  }

  masm.bind(ool->callStub());
  saveLive(ins);

  pushArg(Imm32(ool->strict()));
  pushArg(value.ref());
  if (index->isConstant()) {
    pushArg(Imm32(ToInt32(index)));
  } else {
    pushArg(ToRegister(index));
  }
  pushArg(object);

  using Fn = bool (*)(JSContext*, HandleNativeObject, int32_t, HandleValue,
                      bool strict);
  callVM<Fn, jit::SetDenseElement>(ins);

  restoreLive(ins);
  masm.jump(ool->rejoin());
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
  Register elementsTemp = ToRegister(lir->temp());
  Register length = ToRegister(lir->output());
  ValueOperand value = ToValue(lir, LArrayPush::Value);
  Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

  using Fn = bool (*)(JSContext*, HandleArrayObject, HandleValue, uint32_t*);
  OutOfLineCode* ool = oolCallVM<Fn, jit::ArrayPushDense>(
      lir, ArgList(obj, value), StoreRegisterTo(length));

  // Load elements and length.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), elementsTemp);
  masm.load32(Address(elementsTemp, ObjectElements::offsetOfLength()), length);

  // TODO(post-Warp): reuse/share the CacheIR implementation when IonBuilder and
  // TI are gone (bug 1654180).

  // Bailout if the incremented length does not fit in int32.
  bailoutCmp32(Assembler::AboveOrEqual, length, Imm32(INT32_MAX),
               lir->snapshot());

  // Guard length == initializedLength.
  Address initLength(elementsTemp, ObjectElements::offsetOfInitializedLength());
  masm.branch32(Assembler::NotEqual, initLength, length, ool->entry());

  // Guard length < capacity.
  Address capacity(elementsTemp, ObjectElements::offsetOfCapacity());
  masm.spectreBoundsCheck32(length, capacity, spectreTemp, ool->entry());

  // Do the store.
  masm.storeValue(value, BaseObjectElementIndex(elementsTemp, length));

  masm.add32(Imm32(1), length);

  // Update length and initialized length.
  masm.store32(length, Address(elementsTemp, ObjectElements::offsetOfLength()));
  masm.store32(length, Address(elementsTemp,
                               ObjectElements::offsetOfInitializedLength()));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitArraySlice(LArraySlice* lir) {
  Register object = ToRegister(lir->object());
  Register begin = ToRegister(lir->begin());
  Register end = ToRegister(lir->end());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());

  Label call, fail;

  Label bail;
  masm.branchArrayIsNotPacked(object, temp1, temp2, &bail);
  bailoutFrom(&bail, lir->snapshot());

  // Try to allocate an object.
  TemplateObject templateObject(lir->mir()->templateObj());
  masm.createGCObject(temp1, temp2, templateObject, lir->mir()->initialHeap(),
                      &fail);

  masm.jump(&call);
  {
    masm.bind(&fail);
    masm.movePtr(ImmPtr(nullptr), temp1);
  }
  masm.bind(&call);

  pushArg(temp1);
  pushArg(end);
  pushArg(begin);
  pushArg(object);

  using Fn =
      JSObject* (*)(JSContext*, HandleObject, int32_t, int32_t, HandleObject);
  callVM<Fn, ArraySliceDense>(lir);
}

void CodeGenerator::visitArrayJoin(LArrayJoin* lir) {
  Label skipCall;

  Register output = ToRegister(lir->output());
  Register sep = ToRegister(lir->separator());
  Register array = ToRegister(lir->array());
  Register temp = ToRegister(lir->temp());

  // Fast path for simple length <= 1 cases.
  {
    masm.loadPtr(Address(array, NativeObject::offsetOfElements()), temp);
    Address length(temp, ObjectElements::offsetOfLength());
    Address initLength(temp, ObjectElements::offsetOfInitializedLength());

    // Check for length == 0
    Label notEmpty;
    masm.branch32(Assembler::NotEqual, length, Imm32(0), &notEmpty);
    const JSAtomState& names = GetJitContext()->runtime->names();
    masm.movePtr(ImmGCPtr(names.empty), output);
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

void CodeGenerator::visitGetIteratorCache(LGetIteratorCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  TypedOrValueRegister val = toConstantOrRegister(lir, LGetIteratorCache::Value,
                                                  lir->mir()->value()->type())
                                 .reg();
  Register output = ToRegister(lir->output());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());

  IonGetIteratorIC ic(liveRegs, val, output, temp1, temp2);
  addIC(lir, allocateIC(ic));
}

void CodeGenerator::visitOptimizeSpreadCallCache(
    LOptimizeSpreadCallCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  ValueOperand val = ToValue(lir, LOptimizeSpreadCallCache::Value);
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp());

  IonOptimizeSpreadCallIC ic(liveRegs, val, output, temp);
  addIC(lir, allocateIC(ic));
}

void CodeGenerator::visitIteratorMore(LIteratorMore* lir) {
  const Register obj = ToRegister(lir->object());
  const ValueOperand output = ToOutValue(lir);
  const Register temp = ToRegister(lir->temp());

  masm.iteratorMore(obj, output, temp);
}

void CodeGenerator::visitIsNoIterAndBranch(LIsNoIterAndBranch* lir) {
  ValueOperand input = ToValue(lir, LIsNoIterAndBranch::Input);
  Label* ifTrue = getJumpLabelForBranch(lir->ifTrue());
  Label* ifFalse = getJumpLabelForBranch(lir->ifFalse());

  masm.branchTestMagic(Assembler::Equal, input, ifTrue);

  if (!isNextBlock(lir->ifFalse()->lir())) {
    masm.jump(ifFalse);
  }
}

void CodeGenerator::visitIteratorEnd(LIteratorEnd* lir) {
  const Register obj = ToRegister(lir->object());
  const Register temp1 = ToRegister(lir->temp1());
  const Register temp2 = ToRegister(lir->temp2());
  const Register temp3 = ToRegister(lir->temp3());

  masm.iteratorClose(obj, temp1, temp2, temp3);
}

void CodeGenerator::visitArgumentsLength(LArgumentsLength* lir) {
  // read number of actual arguments from the JS frame.
  Register argc = ToRegister(lir->output());
  Address ptr(masm.getStackPointer(),
              frameSize() + JitFrameLayout::offsetOfNumActualArgs());

  masm.loadPtr(ptr, argc);
}

void CodeGenerator::visitGetFrameArgument(LGetFrameArgument* lir) {
  ValueOperand result = ToOutValue(lir);
  const LAllocation* index = lir->index();
  size_t argvOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();

  if (index->isConstant()) {
    int32_t i = index->toConstant()->toInt32();
    Address argPtr(masm.getStackPointer(), sizeof(Value) * i + argvOffset);
    masm.loadValue(argPtr, result);
  } else {
    Register i = ToRegister(index);
    BaseValueIndex argPtr(masm.getStackPointer(), i, argvOffset);
    masm.loadValue(argPtr, result);
  }
}

void CodeGenerator::emitRest(LInstruction* lir, Register array,
                             Register numActuals, Register temp0,
                             Register temp1, unsigned numFormals,
                             Register resultreg) {
  // Compute actuals() + numFormals.
  size_t actualsOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();
  masm.moveStackPtrTo(temp1);
  masm.addPtr(Imm32(sizeof(Value) * numFormals + actualsOffset), temp1);

  // Compute numActuals - numFormals.
  Label emptyLength, joinLength;
  masm.movePtr(numActuals, temp0);
  masm.branch32(Assembler::LessThanOrEqual, temp0, Imm32(numFormals),
                &emptyLength);
  masm.sub32(Imm32(numFormals), temp0);
  masm.jump(&joinLength);
  {
    masm.bind(&emptyLength);
    masm.move32(Imm32(0), temp0);
  }
  masm.bind(&joinLength);

  pushArg(array);
  pushArg(temp1);
  pushArg(temp0);

  using Fn = JSObject* (*)(JSContext*, uint32_t, Value*, HandleObject);
  callVM<Fn, InitRestParameter>(lir);
}

void CodeGenerator::visitRest(LRest* lir) {
  Register numActuals = ToRegister(lir->numActuals());
  Register temp0 = ToRegister(lir->getTemp(0));
  Register temp1 = ToRegister(lir->getTemp(1));
  Register temp2 = ToRegister(lir->getTemp(2));
  unsigned numFormals = lir->mir()->numFormals();

  if (Shape* shape = lir->mir()->shape()) {
    uint32_t arrayLength = 0;
    uint32_t arrayCapacity = 2;
    gc::AllocKind allocKind = GuessArrayGCKind(arrayCapacity);
    MOZ_ASSERT(CanChangeToBackgroundAllocKind(allocKind, &ArrayObject::class_));
    allocKind = ForegroundToBackgroundAllocKind(allocKind);
    MOZ_ASSERT(GetGCKindSlots(allocKind) ==
               arrayCapacity + ObjectElements::VALUES_PER_HEADER);

    Label joinAlloc, failAlloc;
    masm.movePtr(ImmGCPtr(shape), temp0);
    masm.createArrayWithFixedElements(temp2, temp0, temp1, arrayLength,
                                      arrayCapacity, allocKind, gc::DefaultHeap,
                                      &failAlloc);
    masm.jump(&joinAlloc);
    {
      masm.bind(&failAlloc);
      masm.movePtr(ImmPtr(nullptr), temp2);
    }
    masm.bind(&joinAlloc);
  } else {
    masm.movePtr(ImmPtr(nullptr), temp2);
  }

  emitRest(lir, temp2, numActuals, temp0, temp1, numFormals,
           ToRegister(lir->output()));
}

// Create a stackmap from the given safepoint, with the structure:
//
//   <reg dump area, if trap>
//   |       ++ <body (general spill)>
//   |               ++ <space for Frame>
//   |                       ++ <inbound args>
//   |                                       |
//   Lowest Addr                             Highest Addr
//
// The caller owns the resulting stackmap.  This assumes a grow-down stack.
//
// For non-debug builds, if the stackmap would contain no pointers, no
// stackmap is created, and nullptr is returned.  For a debug build, a
// stackmap is always created and returned.
static bool CreateStackMapFromLSafepoint(LSafepoint& safepoint,
                                         const MachineState& trapExitLayout,
                                         size_t trapExitLayoutNumWords,
                                         size_t nInboundStackArgBytes,
                                         wasm::StackMap** result) {
  // Ensure this is defined on all return paths.
  *result = nullptr;

  // The size of the wasm::Frame itself.
  const size_t nFrameBytes = sizeof(wasm::Frame);

  // This is the number of bytes in the general spill area, below the Frame.
  const size_t nBodyBytes = safepoint.framePushedAtStackMapBase();

  // This is the number of bytes in the general spill area, the Frame, and the
  // incoming args, but not including any trap (register dump) area.
  const size_t nNonTrapBytes = nBodyBytes + nFrameBytes + nInboundStackArgBytes;
  MOZ_ASSERT(nNonTrapBytes % sizeof(void*) == 0);

  // This is the total number of bytes covered by the map.
  const DebugOnly<size_t> nTotalBytes =
      nNonTrapBytes +
      (safepoint.isWasmTrap() ? (trapExitLayoutNumWords * sizeof(void*)) : 0);

  // Create the stackmap initially in this vector.  Since most frames will
  // contain 128 or fewer words, heap allocation is avoided in the majority of
  // cases.  vec[0] is for the lowest address in the map, vec[N-1] is for the
  // highest address in the map.
  wasm::StackMapBoolVector vec;

  // Keep track of whether we've actually seen any refs.
  bool hasRefs = false;

  // REG DUMP AREA, if any.
  const LiveGeneralRegisterSet gcRegs = safepoint.gcRegs();
  GeneralRegisterForwardIterator gcRegsIter(gcRegs);
  if (safepoint.isWasmTrap()) {
    // Deal with roots in registers.  This can only happen for safepoints
    // associated with a trap.  For safepoints associated with a call, we
    // don't expect to have any live values in registers, hence no roots in
    // registers.
    if (!vec.appendN(false, trapExitLayoutNumWords)) {
      return false;
    }
    for (; gcRegsIter.more(); ++gcRegsIter) {
      Register reg = *gcRegsIter;
      size_t offsetFromTop =
          reinterpret_cast<size_t>(trapExitLayout.address(reg));

      // If this doesn't hold, the associated register wasn't saved by
      // the trap exit stub.  Better to crash now than much later, in
      // some obscure place, and possibly with security consequences.
      MOZ_RELEASE_ASSERT(offsetFromTop < trapExitLayoutNumWords);

      // offsetFromTop is an offset in words down from the highest
      // address in the exit stub save area.  Switch it around to be an
      // offset up from the bottom of the (integer register) save area.
      size_t offsetFromBottom = trapExitLayoutNumWords - 1 - offsetFromTop;

      vec[offsetFromBottom] = true;
      hasRefs = true;
    }
  } else {
    // This map is associated with a call instruction.  We expect there to be
    // no live ref-carrying registers, and if there are we're in deep trouble.
    MOZ_RELEASE_ASSERT(!gcRegsIter.more());
  }

  // BODY (GENERAL SPILL) AREA and FRAME and INCOMING ARGS
  // Deal with roots on the stack.
  size_t wordsSoFar = vec.length();
  if (!vec.appendN(false, nNonTrapBytes / sizeof(void*))) {
    return false;
  }
  const LSafepoint::SlotList& gcSlots = safepoint.gcSlots();
  for (SafepointSlotEntry gcSlot : gcSlots) {
    // The following needs to correspond with JitFrameLayout::slotRef
    // gcSlot.stack == 0 means the slot is in the args area
    if (gcSlot.stack) {
      // It's a slot in the body allocation, so .slot is interpreted
      // as an index downwards from the Frame*
      MOZ_ASSERT(gcSlot.slot <= nBodyBytes);
      uint32_t offsetInBytes = nBodyBytes - gcSlot.slot;
      MOZ_ASSERT(offsetInBytes % sizeof(void*) == 0);
      vec[wordsSoFar + offsetInBytes / sizeof(void*)] = true;
    } else {
      // It's an argument slot
      MOZ_ASSERT(gcSlot.slot < nInboundStackArgBytes);
      uint32_t offsetInBytes = nBodyBytes + nFrameBytes + gcSlot.slot;
      MOZ_ASSERT(offsetInBytes % sizeof(void*) == 0);
      vec[wordsSoFar + offsetInBytes / sizeof(void*)] = true;
    }
    hasRefs = true;
  }

#ifndef DEBUG
  // We saw no references, and this is a non-debug build, so don't bother
  // building the stackmap.
  if (!hasRefs) {
    return true;
  }
#endif

  // Convert vec into a wasm::StackMap.
  MOZ_ASSERT(vec.length() * sizeof(void*) == nTotalBytes);
  wasm::StackMap* stackMap =
      wasm::ConvertStackMapBoolVectorToStackMap(vec, hasRefs);
  if (!stackMap) {
    return false;
  }
  if (safepoint.isWasmTrap()) {
    stackMap->setExitStubWords(trapExitLayoutNumWords);
  }

  // Record in the map, how far down from the highest address the Frame* is.
  // Take the opportunity to check that we haven't marked any part of the
  // Frame itself as a pointer.
  stackMap->setFrameOffsetFromTop((nInboundStackArgBytes + nFrameBytes) /
                                  sizeof(void*));
#ifdef DEBUG
  for (uint32_t i = 0; i < nFrameBytes / sizeof(void*); i++) {
    MOZ_ASSERT(stackMap->getBit(stackMap->numMappedWords -
                                stackMap->frameOffsetFromTop + i) == 0);
  }
#endif

  *result = stackMap;
  return true;
}

bool CodeGenerator::generateWasm(wasm::TypeIdDesc funcTypeId,
                                 wasm::BytecodeOffset trapOffset,
                                 const wasm::ArgTypeVector& argTypes,
                                 const MachineState& trapExitLayout,
                                 size_t trapExitLayoutNumWords,
                                 wasm::FuncOffsets* offsets,
                                 wasm::StackMaps* stackMaps) {
  JitSpew(JitSpew_Codegen, "# Emitting wasm code");
  setUseWasmStackArgumentAbi();

  size_t nInboundStackArgBytes = StackArgAreaSizeUnaligned(argTypes);

  wasm::GenerateFunctionPrologue(masm, funcTypeId, mozilla::Nothing(), offsets);

  MOZ_ASSERT(masm.framePushed() == 0);

  if (omitOverRecursedCheck()) {
    masm.reserveStack(frameSize());
  } else {
    std::pair<CodeOffset, uint32_t> pair =
        masm.wasmReserveStackChecked(frameSize(), trapOffset);
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
        !stackMaps->add((uint8_t*)(uintptr_t)trapInsnOffset.offset(),
                        functionEntryStackMap)) {
      functionEntryStackMap->destroy();
      return false;
    }
  }

  MOZ_ASSERT(masm.framePushed() == frameSize());

  if (!generateBody()) {
    return false;
  }

  masm.bind(&returnLabel_);
  wasm::GenerateFunctionEpilogue(masm, frameSize(), offsets);

#if defined(JS_ION_PERF)
  // Note the end of the inline code and start of the OOL code.
  gen->perfSpewer().noteEndInlineCode(masm);
#endif

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
  MOZ_ASSERT(bailouts_.empty());
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

    if (!stackMaps->add((uint8_t*)(uintptr_t)index.displacement(), stackMap)) {
      stackMap->destroy();
      return false;
    }
  }

  return true;
}

bool CodeGenerator::generate() {
  JitSpew(JitSpew_Codegen, "# Emitting code for script %s:%u:%u",
          gen->outerInfo().script()->filename(),
          gen->outerInfo().script()->lineno(),
          gen->outerInfo().script()->column());

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

  if (!generatePrologue()) {
    return false;
  }

  if (frameClass_ != FrameSizeClass::None()) {
    deoptTable_.emplace(gen->jitRuntime()->getBailoutTable(frameClass_));
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

  if (!generateEpilogue()) {
    return false;
  }

  // Reset native => bytecode map table with top-level script and startPc.
  if (!addNativeToBytecodeEntry(startSite)) {
    return false;
  }

  generateInvalidateEpilogue();
#if defined(JS_ION_PERF)
  // Note the end of the inline code and start of the OOL code.
  perfSpewer_.noteEndInlineCode(masm);
#endif

  // native => bytecode entries for OOL code will be added
  // by CodeGeneratorShared::generateOutOfLineCode
  if (!generateOutOfLineCode()) {
    return false;
  }

  // Add terminal entry.
  if (!addNativeToBytecodeEntry(startSite)) {
    return false;
  }

  // Dump Native to bytecode entries to spew.
  dumpNativeToBytecodeEntries();

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

bool CodeGenerator::link(JSContext* cx, const WarpSnapshot* snapshot) {
  // We cancel off-thread Ion compilations in a few places during GC, but if
  // this compilation was performed off-thread it will already have been
  // removed from the relevant lists by this point. Don't allow GC here.
  JS::AutoAssertNoGC nogc(cx);

  RootedScript script(cx, gen->outerInfo().script());
  MOZ_ASSERT(!script->hasIonScript());

  // Perform any read barriers which were skipped while compiling the
  // script, which may have happened off-thread.
  const JitRealm* jr = gen->realm->jitRealm();
  jr->performStubReadBarriers(realmStubsToReadBarrier_);

  if (scriptCounts_ && !script->hasScriptCounts() &&
      !script->initScriptCounts(cx)) {
    return false;
  }

  // Check to make sure we didn't have a mid-build invalidation. If so, we
  // will trickle to jit::Compile() and return Method_Skipped.
  uint32_t warmUpCount = script->getWarmUpCount();

  IonCompilationId compilationId =
      cx->runtime()->jitRuntime()->nextCompilationId();
  JitZone* jitZone = cx->zone()->jitZone();
  jitZone->currentCompilationIdRef().emplace(compilationId);
  auto resetCurrentId = mozilla::MakeScopeExit(
      [jitZone] { jitZone->currentCompilationIdRef().reset(); });

  // Record constraints. If an error occured, returns false and potentially
  // prevent future compilations. Otherwise, if an invalidation occured, then
  // skip the current compilation.
  bool isValid = false;

  // If an inlined script is invalidated (for example, by attaching
  // a debugger), we must also invalidate the parent IonScript.
  if (!AddInlinedCompilations(cx, script, compilationId, snapshot, &isValid)) {
    return false;
  }
  if (!isValid) {
    return true;
  }

  // IonMonkey could have inferred better type information during
  // compilation. Since adding the new information to the actual type
  // information can reset the usecount, increase it back to what it was
  // before.
  if (warmUpCount > script->getWarmUpCount()) {
    script->incWarmUpCounter(warmUpCount - script->getWarmUpCount());
  }

  uint32_t argumentSlots = (gen->outerInfo().nargs() + 1) * sizeof(Value);
  uint32_t scriptFrameSize =
      frameClass_ == FrameSizeClass::None()
          ? frameDepth_
          : FrameSizeClass::FromDepth(frameDepth_).frameSize();

  // We encode safepoints after the OSI-point offsets have been determined.
  if (!encodeSafepoints()) {
    return false;
  }

  size_t numNurseryObjects = snapshot->nurseryObjects().length();

  IonScript* ionScript = IonScript::New(
      cx, compilationId, graph.totalSlotCount(), argumentSlots, scriptFrameSize,
      snapshots_.listSize(), snapshots_.RVATableSize(), recovers_.size(),
      bailouts_.length(), graph.numConstants(), numNurseryObjects,
      safepointIndices_.length(), osiIndices_.length(), icList_.length(),
      runtimeData_.length(), safepoints_.size());
  if (!ionScript) {
    return false;
  }
#ifdef DEBUG
  ionScript->setICHash(snapshot->icHash());
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
    if (!generateCompactNativeToBytecodeMap(cx, code)) {
      return false;
    }

    uint8_t* ionTableAddr =
        ((uint8_t*)nativeToBytecodeMap_) + nativeToBytecodeTableOffset_;
    JitcodeIonTable* ionTable = (JitcodeIonTable*)ionTableAddr;

    // Construct the IonEntry that will go into the global table.
    JitcodeGlobalEntry::IonEntry entry;
    if (!ionTable->makeIonEntry(cx, code, nativeToBytecodeScriptListLength_,
                                nativeToBytecodeScriptList_, entry)) {
      js_free(nativeToBytecodeScriptList_);
      js_free(nativeToBytecodeMap_);
      return false;
    }

    // nativeToBytecodeScriptList_ is no longer needed.
    js_free(nativeToBytecodeScriptList_);

    // Add entry to the global table.
    JitcodeGlobalTable* globalTable =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(entry)) {
      // Memory may have been allocated for the entry.
      entry.destroy();
      return false;
    }

    // Mark the jitcode as having a bytecode map.
    code->setHasBytecodeMap();
  } else {
    // Add a dumy jitcodeGlobalTable entry.
    JitcodeGlobalEntry::DummyEntry entry;
    entry.init(code, code->raw(), code->rawEnd());

    // Add entry to the global table.
    JitcodeGlobalTable* globalTable =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(entry)) {
      // Memory may have been allocated for the entry.
      entry.destroy();
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

#ifdef JS_TRACE_LOGGING
  bool TLFailed = false;

  MOZ_ASSERT_IF(!JS::TraceLoggerSupported(), patchableTLEvents_.length() == 0);
  for (uint32_t i = 0; i < patchableTLEvents_.length(); i++) {
    TraceLoggerEvent event(patchableTLEvents_[i].event);
    if (!event.hasTextId() || !ionScript->addTraceLoggerEvent(event)) {
      TLFailed = true;
      break;
    }
    Assembler::PatchDataWithValueCheck(
        CodeLocationLabel(code, patchableTLEvents_[i].offset),
        ImmPtr((void*)uintptr_t(event.textId())), ImmPtr((void*)0));
  }

  if (!TLFailed && patchableTLScripts_.length() > 0) {
    MOZ_ASSERT(TraceLogTextIdEnabled(TraceLogger_Scripts));
    TraceLoggerEvent event(TraceLogger_Scripts, script);
    if (!event.hasTextId() || !ionScript->addTraceLoggerEvent(event)) {
      TLFailed = true;
    }
    if (!TLFailed) {
      uint32_t textId = event.textId();
      for (uint32_t i = 0; i < patchableTLScripts_.length(); i++) {
        Assembler::PatchDataWithValueCheck(
            CodeLocationLabel(code, patchableTLScripts_[i]),
            ImmPtr((void*)uintptr_t(textId)), ImmPtr((void*)0));
      }
    }
  }
#endif

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

#if defined(JS_ION_PERF)
  if (PerfEnabled()) {
    perfSpewer_.writeProfile(script, code, masm);
  }
#endif

#ifdef MOZ_VTUNE
  vtune::MarkScript(code, script, "ion");
#endif

  // for marking during GC.
  if (safepointIndices_.length()) {
    ionScript->copySafepointIndices(&safepointIndices_[0]);
  }
  if (safepoints_.size()) {
    ionScript->copySafepoints(&safepoints_);
  }

  // for reconvering from an Ion Frame.
  if (bailouts_.length()) {
    ionScript->copyBailoutTable(&bailouts_[0]);
  }
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
  const auto& nurseryObjects = snapshot->nurseryObjects();
  for (size_t i = 0; i < nurseryObjects.length(); i++) {
    ionScript->nurseryObjects()[i].init(nurseryObjects[i]);
  }

  // Transfer ownership of the IonScript to the JitScript. At this point enough
  // of the IonScript must be initialized for IonScript::Destroy to work.
  freeIonScript.release();
  script->jitScript()->setIonScript(script, ionScript);

  return true;
}

// An out-of-line path to convert a boxed int32 to either a float or double.
class OutOfLineUnboxFloatingPoint : public OutOfLineCodeBase<CodeGenerator> {
  LUnboxFloatingPoint* unboxFloatingPoint_;

 public:
  explicit OutOfLineUnboxFloatingPoint(LUnboxFloatingPoint* unboxFloatingPoint)
      : unboxFloatingPoint_(unboxFloatingPoint) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineUnboxFloatingPoint(this);
  }

  LUnboxFloatingPoint* unboxFloatingPoint() const {
    return unboxFloatingPoint_;
  }
};

void CodeGenerator::visitUnboxFloatingPoint(LUnboxFloatingPoint* lir) {
  const ValueOperand box = ToValue(lir, LUnboxFloatingPoint::Input);
  const LDefinition* result = lir->output();

  // Out-of-line path to convert int32 to double or bailout
  // if this instruction is fallible.
  OutOfLineUnboxFloatingPoint* ool =
      new (alloc()) OutOfLineUnboxFloatingPoint(lir);
  addOutOfLineCode(ool, lir->mir());

  FloatRegister resultReg = ToFloatRegister(result);
  masm.branchTestDouble(Assembler::NotEqual, box, ool->entry());
  masm.unboxDouble(box, resultReg);
  if (lir->type() == MIRType::Float32) {
    masm.convertDoubleToFloat32(resultReg, resultReg);
  }
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineUnboxFloatingPoint(
    OutOfLineUnboxFloatingPoint* ool) {
  LUnboxFloatingPoint* ins = ool->unboxFloatingPoint();
  const ValueOperand value = ToValue(ins, LUnboxFloatingPoint::Input);

  if (ins->mir()->fallible()) {
    Label bail;
    masm.branchTestInt32(Assembler::NotEqual, value, &bail);
    bailoutFrom(&bail, ins->snapshot());
  }
  masm.int32ValueToFloatingPoint(value, ToFloatRegister(ins->output()),
                                 ins->type());
  masm.jump(ool->rejoin());
}

void CodeGenerator::visitCallBindVar(LCallBindVar* lir) {
  pushArg(ToRegister(lir->environmentChain()));

  using Fn = JSObject* (*)(JSContext*, JSObject*);
  callVM<Fn, BindVarOperation>(lir);
}

void CodeGenerator::visitCallSetElement(LCallSetElement* lir) {
  Register obj = ToRegister(lir->getOperand(0));
  pushArg(Imm32(lir->mir()->strict()));
  pushArg(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));
  pushArg(ToValue(lir, LCallSetElement::Value));
  pushArg(ToValue(lir, LCallSetElement::Index));
  pushArg(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue,
                      HandleValue, bool);
  callVM<Fn, js::SetObjectElementWithReceiver>(lir);
}

void CodeGenerator::visitLoadFixedSlotV(LLoadFixedSlotV* ins) {
  const Register obj = ToRegister(ins->getOperand(0));
  size_t slot = ins->mir()->slot();
  ValueOperand result = ToOutValue(ins);

  masm.loadValue(Address(obj, NativeObject::getFixedSlotOffset(slot)), result);
}

void CodeGenerator::visitLoadFixedSlotT(LLoadFixedSlotT* ins) {
  const Register obj = ToRegister(ins->getOperand(0));
  size_t slot = ins->mir()->slot();
  AnyRegister result = ToAnyRegister(ins->getDef(0));
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

void CodeGenerator::visitAddAndStoreSlot(LAddAndStoreSlot* ins) {
  const Register obj = ToRegister(ins->getOperand(0));
  const ValueOperand value = ToValue(ins, LAddAndStoreSlot::Value);
  const Register maybeTemp = ToTempRegisterOrInvalid(ins->getTemp(0));

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
  const Register obj = ToRegister(ins->getOperand(0));
  const ValueOperand value = ToValue(ins, LAllocateAndStoreSlot::Value);
  const Register temp1 = ToRegister(ins->getTemp(0));
  const Register temp2 = ToRegister(ins->getTemp(1));

  masm.push(obj);
  masm.pushValue(value);

  using Fn = bool (*)(JSContext * cx, NativeObject * obj, uint32_t newCount);
  masm.setupUnalignedABICall(temp1);
  masm.loadJSContext(temp1);
  masm.passABIArg(temp1);
  masm.passABIArg(obj);
  masm.move32(Imm32(ins->mir()->numNewSlots()), temp2);
  masm.passABIArg(temp2);
  masm.callWithABI<Fn, NativeObject::growSlotsPure>();
  masm.storeCallBoolResult(temp1);

  masm.popValue(value);
  masm.pop(obj);

  Label bail;
  masm.branchIfFalseBool(temp1, &bail);
  bailoutFrom(&bail, ins->snapshot());

  masm.storeObjShape(ins->mir()->shape(), obj,
                     [](MacroAssembler& masm, const Address& addr) {
                       EmitPreBarrier(masm, addr, MIRType::Shape);
                     });

  // Perform the store. No pre-barrier required since this is a new
  // initialization.
  masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), temp1);
  Address slot(temp1, ins->mir()->slotOffset());
  masm.storeValue(value, slot);
}

void CodeGenerator::visitStoreFixedSlotV(LStoreFixedSlotV* ins) {
  const Register obj = ToRegister(ins->getOperand(0));
  size_t slot = ins->mir()->slot();

  const ValueOperand value = ToValue(ins, LStoreFixedSlotV::Value);

  Address address(obj, NativeObject::getFixedSlotOffset(slot));
  if (ins->mir()->needsBarrier()) {
    emitPreBarrier(address);
  }

  masm.storeValue(value, address);
}

void CodeGenerator::visitStoreFixedSlotT(LStoreFixedSlotT* ins) {
  const Register obj = ToRegister(ins->getOperand(0));
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
  Register temp = ToRegister(ins->temp());

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
    return TypedOrValueRegister(ToValue(lir, n));
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
      toConstantOrRegister(ins, LGetPropertyCache::Value,
                           ins->mir()->value()->type())
          .reg();
  ConstantOrRegister id = toConstantOrRegister(ins, LGetPropertyCache::Id,
                                               ins->mir()->idval()->type());
  ValueOperand output = ToOutValue(ins);
  addGetPropertyCache(ins, liveRegs, value, id, output);
}

void CodeGenerator::visitGetPropSuperCache(LGetPropSuperCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  Register obj = ToRegister(ins->obj());
  TypedOrValueRegister receiver =
      toConstantOrRegister(ins, LGetPropSuperCache::Receiver,
                           ins->mir()->receiver()->type())
          .reg();
  ConstantOrRegister id = toConstantOrRegister(ins, LGetPropSuperCache::Id,
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
  Register temp = ToRegister(ins->temp());

  IonBindNameIC ic(liveRegs, envChain, output, temp);
  addIC(ins, allocateIC(ic));
}

void CodeGenerator::visitHasOwnCache(LHasOwnCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  TypedOrValueRegister value = toConstantOrRegister(ins, LHasOwnCache::Value,
                                                    ins->mir()->value()->type())
                                   .reg();
  TypedOrValueRegister id =
      toConstantOrRegister(ins, LHasOwnCache::Id, ins->mir()->idval()->type())
          .reg();
  Register output = ToRegister(ins->output());

  IonHasOwnIC cache(liveRegs, value, id, output);
  addIC(ins, allocateIC(cache));
}

void CodeGenerator::visitCheckPrivateFieldCache(LCheckPrivateFieldCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  TypedOrValueRegister value =
      toConstantOrRegister(ins, LCheckPrivateFieldCache::Value,
                           ins->mir()->value()->type())
          .reg();
  TypedOrValueRegister id =
      toConstantOrRegister(ins, LCheckPrivateFieldCache::Id,
                           ins->mir()->idval()->type())
          .reg();
  Register output = ToRegister(ins->output());

  IonCheckPrivateFieldIC cache(liveRegs, value, id, output);
  addIC(ins, allocateIC(cache));
}

void CodeGenerator::visitCallDeleteProperty(LCallDeleteProperty* lir) {
  pushArg(ImmGCPtr(lir->mir()->name()));
  pushArg(ToValue(lir, LCallDeleteProperty::Value));

  using Fn = bool (*)(JSContext*, HandleValue, HandlePropertyName, bool*);
  if (lir->mir()->strict()) {
    callVM<Fn, DelPropOperation<true>>(lir);
  } else {
    callVM<Fn, DelPropOperation<false>>(lir);
  }
}

void CodeGenerator::visitCallDeleteElement(LCallDeleteElement* lir) {
  pushArg(ToValue(lir, LCallDeleteElement::Index));
  pushArg(ToValue(lir, LCallDeleteElement::Value));

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue, bool*);
  if (lir->mir()->strict()) {
    callVM<Fn, DelElemOperation<true>>(lir);
  } else {
    callVM<Fn, DelElemOperation<false>>(lir);
  }
}

void CodeGenerator::visitSetPropertyCache(LSetPropertyCache* ins) {
  LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
  Register objReg = ToRegister(ins->getOperand(0));
  Register temp = ToRegister(ins->temp());

  ConstantOrRegister id = toConstantOrRegister(ins, LSetPropertyCache::Id,
                                               ins->mir()->idval()->type());
  ConstantOrRegister value = toConstantOrRegister(ins, LSetPropertyCache::Value,
                                                  ins->mir()->value()->type());

  addSetPropertyCache(ins, liveRegs, objReg, temp, id, value,
                      ins->mir()->strict());
}

void CodeGenerator::visitThrow(LThrow* lir) {
  pushArg(ToValue(lir, LThrow::Value));

  using Fn = bool (*)(JSContext*, HandleValue);
  callVM<Fn, js::ThrowOperation>(lir);
}

class OutOfLineTypeOfV : public OutOfLineCodeBase<CodeGenerator> {
  LTypeOfV* ins_;

 public:
  explicit OutOfLineTypeOfV(LTypeOfV* ins) : ins_(ins) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineTypeOfV(this);
  }
  LTypeOfV* ins() const { return ins_; }
};

void CodeGenerator::emitTypeOfName(JSValueType type, Register output) {
  const JSAtomState& names = gen->runtime->names();

  switch (type) {
    case JSVAL_TYPE_OBJECT:
      masm.movePtr(ImmGCPtr(names.object), output);
      break;
    case JSVAL_TYPE_DOUBLE:
    case JSVAL_TYPE_INT32:
      masm.movePtr(ImmGCPtr(names.number), output);
      break;
    case JSVAL_TYPE_BOOLEAN:
      masm.movePtr(ImmGCPtr(names.boolean), output);
      break;
    case JSVAL_TYPE_UNDEFINED:
      masm.movePtr(ImmGCPtr(names.undefined), output);
      break;
    case JSVAL_TYPE_NULL:
      masm.movePtr(ImmGCPtr(names.object), output);
      break;
    case JSVAL_TYPE_STRING:
      masm.movePtr(ImmGCPtr(names.string), output);
      break;
    case JSVAL_TYPE_SYMBOL:
      masm.movePtr(ImmGCPtr(names.symbol), output);
      break;
    case JSVAL_TYPE_BIGINT:
      masm.movePtr(ImmGCPtr(names.bigint), output);
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

  emitTypeOfName(type, output);
  masm.jump(done);
  masm.bind(&notMatch);
}

void CodeGenerator::visitTypeOfV(LTypeOfV* lir) {
  const ValueOperand value = ToValue(lir, LTypeOfV::Input);
  Register output = ToRegister(lir->output());
  Register tag = masm.extractTag(value, output);

  Label done;

  auto* ool = new (alloc()) OutOfLineTypeOfV(lir);
  addOutOfLineCode(ool, lir->mir());

  const uint32_t NumTypes = 8;
  const auto& defaultOrder = {JSVAL_TYPE_OBJECT,    JSVAL_TYPE_DOUBLE,
                              JSVAL_TYPE_UNDEFINED, JSVAL_TYPE_NULL,
                              JSVAL_TYPE_BOOLEAN,   JSVAL_TYPE_STRING,
                              JSVAL_TYPE_SYMBOL,    JSVAL_TYPE_BIGINT};
  MOZ_ASSERT(defaultOrder.size() == NumTypes);

  Vector<JSValueType, NumTypes, SystemAllocPolicy> remaining;
  MOZ_ALWAYS_TRUE(remaining.reserve(defaultOrder.size()));
  remaining.infallibleAppend(defaultOrder.begin(), defaultOrder.end());

  uint32_t numRemaining = remaining.length();

  // Generate checks for previously observed types first.
  // The TypeDataList is sorted by descending frequency.
  for (auto& observed : lir->mir()->observedTypes()) {
    JSValueType type = observed.type();

    // Unify number types.
    if (type == JSVAL_TYPE_INT32) {
      type = JSVAL_TYPE_DOUBLE;
    }

    emitTypeOfCheck(type, tag, output, &done, ool->entry());
    MOZ_ASSERT(std::count(remaining.begin(), remaining.end(), type) == 1);
    remaining.eraseIfEqual(type);
    numRemaining--;
  }

  // Generate checks for remaining types.
  for (auto type : remaining) {
    if (numRemaining == 1) {
      // We can skip the check for the last remaining type.
#ifdef DEBUG
      emitTypeOfCheck(type, tag, output, &done, ool->entry());
      masm.assumeUnreachable("Unexpected Value type in visitTypeOfV");
#else
      emitTypeOfName(type, output);
#endif
    } else {
      emitTypeOfCheck(type, tag, output, &done, ool->entry());
    }
    numRemaining--;
  }
  MOZ_ASSERT(numRemaining == 0);

  masm.bind(&done);
  masm.bind(ool->rejoin());
}

void CodeGenerator::emitTypeOfObject(Register obj, Register output,
                                     Label* done) {
  const JSAtomState& names = gen->runtime->names();

  Label slowCheck, isObject, isCallable, isUndefined;
  masm.typeOfObject(obj, output, &slowCheck, &isObject, &isCallable,
                    &isUndefined);

  masm.bind(&isCallable);
  masm.movePtr(ImmGCPtr(names.function), output);
  masm.jump(done);

  masm.bind(&isUndefined);
  masm.movePtr(ImmGCPtr(names.undefined), output);
  masm.jump(done);

  masm.bind(&isObject);
  masm.movePtr(ImmGCPtr(names.object), output);
  masm.jump(done);

  masm.bind(&slowCheck);

  saveVolatile(output);
  using Fn = JSString* (*)(JSObject * obj, JSRuntime * rt);
  masm.setupUnalignedABICall(output);
  masm.passABIArg(obj);
  masm.movePtr(ImmPtr(gen->runtime), output);
  masm.passABIArg(output);
  masm.callWithABI<Fn, TypeOfObject>();
  masm.storeCallPointerResult(output);
  restoreVolatile(output);
}

void CodeGenerator::visitOutOfLineTypeOfV(OutOfLineTypeOfV* ool) {
  LTypeOfV* ins = ool->ins();

  ValueOperand input = ToValue(ins, LTypeOfV::Input);
  Register temp = ToTempUnboxRegister(ins->tempToUnbox());
  Register output = ToRegister(ins->output());

  Register obj = masm.extractObject(input, temp);
  emitTypeOfObject(obj, output, ool->rejoin());
  masm.jump(ool->rejoin());
}

void CodeGenerator::visitTypeOfO(LTypeOfO* lir) {
  Register obj = ToRegister(lir->object());
  Register output = ToRegister(lir->output());

  Label done;
  emitTypeOfObject(obj, output, &done);
  masm.bind(&done);
}

void CodeGenerator::visitToAsyncIter(LToAsyncIter* lir) {
  pushArg(ToValue(lir, LToAsyncIter::NextMethodIndex));
  pushArg(ToRegister(lir->iterator()));

  using Fn = JSObject* (*)(JSContext*, HandleObject, HandleValue);
  callVM<Fn, js::CreateAsyncFromSyncIterator>(lir);
}

void CodeGenerator::visitToPropertyKeyCache(LToPropertyKeyCache* lir) {
  LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
  ValueOperand input = ToValue(lir, LToPropertyKeyCache::Input);
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

  if (load->mir()->needsHoleCheck()) {
    Label testMagic;
    masm.branchTestMagic(Assembler::Equal, out, &testMagic);
    bailoutFrom(&testMagic, load->snapshot());
  }
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

  // If a hole check is needed, and the value wasn't a hole, we're done.
  // Otherwise, we'll load undefined.
  if (lir->mir()->needsHoleCheck()) {
    masm.branchTestMagic(Assembler::NotEqual, out, &done);
    masm.moveValue(UndefinedValue(), out);
  }
  masm.jump(&done);

  masm.bind(&outOfBounds);
  if (mir->needsNegativeIntCheck()) {
    Label negative;
    masm.branch32(Assembler::LessThan, index, Imm32(0), &negative);
    bailoutFrom(&negative, lir->snapshot());
  }
  masm.moveValue(UndefinedValue(), out);

  masm.bind(&done);
}

void CodeGenerator::visitLoadUnboxedScalar(LLoadUnboxedScalar* lir) {
  Register elements = ToRegister(lir->elements());
  Register temp = ToTempRegisterOrInvalid(lir->temp());
  AnyRegister out = ToAnyRegister(lir->output());

  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

  Label fail;
  if (lir->index()->isConstant()) {
    Address source =
        ToAddress(elements, lir->index(), storageType, mir->offsetAdjustment());
    masm.loadFromTypedArray(storageType, source, out, temp, &fail);
  } else {
    BaseIndex source(elements, ToRegister(lir->index()),
                     ScaleFromScalarType(storageType), mir->offsetAdjustment());
    masm.loadFromTypedArray(storageType, source, out, temp, &fail);
  }

  if (fail.used()) {
    bailoutFrom(&fail, lir->snapshot());
  }
}

void CodeGenerator::visitLoadUnboxedBigInt(LLoadUnboxedBigInt* lir) {
  Register elements = ToRegister(lir->elements());
  Register temp = ToRegister(lir->temp());
  Register64 temp64 = ToRegister64(lir->temp64());
  Register out = ToRegister(lir->output());

  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

  if (lir->index()->isConstant()) {
    Address source =
        ToAddress(elements, lir->index(), storageType, mir->offsetAdjustment());
    masm.load64(source, temp64);
  } else {
    BaseIndex source(elements, ToRegister(lir->index()),
                     ScaleFromScalarType(storageType), mir->offsetAdjustment());
    masm.load64(source, temp64);
  }

  emitCreateBigInt(lir, storageType, temp64, out, temp);
}

void CodeGenerator::visitLoadDataViewElement(LLoadDataViewElement* lir) {
  Register elements = ToRegister(lir->elements());
  const LAllocation* littleEndian = lir->littleEndian();
  Register temp = ToTempRegisterOrInvalid(lir->temp());
  Register64 temp64 = ToTempRegister64OrInvalid(lir->temp64());
  AnyRegister out = ToAnyRegister(lir->output());

  const MLoadDataViewElement* mir = lir->mir();
  Scalar::Type storageType = mir->storageType();

  BaseIndex source(elements, ToRegister(lir->index()), TimesOne);

  bool noSwap = littleEndian->isConstant() &&
                ToBoolean(littleEndian) == MOZ_LITTLE_ENDIAN();

  // Directly load if no byte swap is needed and the platform supports unaligned
  // accesses for floating point registers.
  if (noSwap && MacroAssembler::SupportsFastUnalignedAccesses()) {
    if (!Scalar::isBigIntType(storageType)) {
      Label fail;
      masm.loadFromTypedArray(storageType, source, out, temp, &fail);

      if (fail.used()) {
        bailoutFrom(&fail, lir->snapshot());
      }
    } else {
      masm.load64(source, temp64);

      emitCreateBigInt(lir, storageType, temp64, out.gpr(), temp);
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
      masm.load32Unaligned(source, out.isFloat() ? temp : out.gpr());
      break;
    case Scalar::Float32:
      masm.load32Unaligned(source, temp);
      break;
    case Scalar::Float64:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      masm.load64Unaligned(source, temp64);
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
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
        masm.byteSwap32(out.isFloat() ? temp : out.gpr());
        break;
      case Scalar::Float32:
        masm.byteSwap32(temp);
        break;
      case Scalar::Float64:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
        masm.byteSwap64(temp64);
        break;
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
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
        masm.convertUInt32ToDouble(temp, out.fpu());
      } else {
        // Bail out if the value doesn't fit into a signed int32 value. This
        // is what allows MLoadDataViewElement to have a type() of
        // MIRType::Int32 for UInt32 array loads.
        bailoutTest32(Assembler::Signed, out.gpr(), out.gpr(), lir->snapshot());
      }
      break;
    case Scalar::Float32:
      masm.moveGPRToFloat32(temp, out.fpu());
      masm.canonicalizeFloat(out.fpu());
      break;
    case Scalar::Float64:
      masm.moveGPR64ToDouble(temp64, out.fpu());
      masm.canonicalizeDouble(out.fpu());
      break;
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      emitCreateBigInt(lir, storageType, temp64, out.gpr(), temp);
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

void CodeGenerator::visitLoadTypedArrayElementHole(
    LLoadTypedArrayElementHole* lir) {
  Register object = ToRegister(lir->object());
  const ValueOperand out = ToOutValue(lir);

  // Load the length.
  Register scratch = out.scratchReg();
  Register scratch2 = ToRegister(lir->temp());
  Register index = ToRegister(lir->index());
  masm.loadArrayBufferViewLengthIntPtr(object, scratch);

  // Load undefined if index >= length.
  Label outOfBounds, done;
  masm.spectreBoundsCheckPtr(index, scratch, scratch2, &outOfBounds);

  // Load the elements vector.
  masm.loadPtr(Address(object, ArrayBufferViewObject::dataOffset()), scratch);

  Scalar::Type arrayType = lir->mir()->arrayType();
  Label fail;
  BaseIndex source(scratch, index, ScaleFromScalarType(arrayType));
  MacroAssembler::Uint32Mode uint32Mode =
      lir->mir()->forceDouble() ? MacroAssembler::Uint32Mode::ForceDouble
                                : MacroAssembler::Uint32Mode::FailOnDouble;
  masm.loadFromTypedArray(arrayType, source, out, uint32Mode, out.scratchReg(),
                          &fail);
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
  Register object = ToRegister(lir->object());
  const ValueOperand out = ToOutValue(lir);

  // On x86 there are not enough registers. In that case reuse the output's
  // type register as temporary.
#ifdef JS_CODEGEN_X86
  MOZ_ASSERT(lir->temp()->isBogusTemp());
  Register temp = out.typeReg();
#else
  Register temp = ToRegister(lir->temp());
#endif
  Register64 temp64 = ToRegister64(lir->temp64());

  // Load the length.
  Register scratch = out.scratchReg();
  Register index = ToRegister(lir->index());
  masm.loadArrayBufferViewLengthIntPtr(object, scratch);

  // Load undefined if index >= length.
  Label outOfBounds, done;
  masm.spectreBoundsCheckPtr(index, scratch, temp, &outOfBounds);

  // Load the elements vector.
  masm.loadPtr(Address(object, ArrayBufferViewObject::dataOffset()), scratch);

  Scalar::Type arrayType = lir->mir()->arrayType();
  BaseIndex source(scratch, index, ScaleFromScalarType(arrayType));
  masm.load64(source, temp64);

  Register bigInt = out.scratchReg();
  emitCreateBigInt(lir, arrayType, temp64, bigInt, temp);

  masm.tagValue(JSVAL_TYPE_BIGINT, bigInt, out);
  masm.jump(&done);

  masm.bind(&outOfBounds);
  masm.moveValue(UndefinedValue(), out);

  masm.bind(&done);
}

template <SwitchTableType tableType>
class OutOfLineSwitch : public OutOfLineCodeBase<CodeGenerator> {
  using LabelsVector = Vector<Label, 0, JitAllocPolicy>;
  using CodeLabelsVector = Vector<CodeLabel, 0, JitAllocPolicy>;
  LabelsVector labels_;
  CodeLabelsVector codeLabels_;
  CodeLabel start_;
  bool isOutOfLine_;

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineSwitch(this);
  }

 public:
  explicit OutOfLineSwitch(TempAllocator& alloc)
      : labels_(alloc), codeLabels_(alloc), isOutOfLine_(false) {}

  CodeLabel* start() { return &start_; }

  CodeLabelsVector& codeLabels() { return codeLabels_; }
  LabelsVector& labels() { return labels_; }

  void jumpToCodeEntries(MacroAssembler& masm, Register index, Register temp) {
    Register base;
    if (tableType == SwitchTableType::Inline) {
#if defined(JS_CODEGEN_ARM)
      base = ::js::jit::pc;
#else
      MOZ_CRASH("NYI: SwitchTableType::Inline");
#endif
    } else {
#if defined(JS_CODEGEN_ARM)
      MOZ_CRASH("NYI: SwitchTableType::OutOfLine");
#else
      masm.mov(start(), temp);
      base = temp;
#endif
    }
    BaseIndex jumpTarget(base, index, ScalePointer);
    masm.branchToComputedAddress(jumpTarget);
  }

  // Register an entry in the switch table.
  void addTableEntry(MacroAssembler& masm) {
    if ((!isOutOfLine_ && tableType == SwitchTableType::Inline) ||
        (isOutOfLine_ && tableType == SwitchTableType::OutOfLine)) {
      CodeLabel cl;
      masm.writeCodePointer(&cl);
      masm.propagateOOM(codeLabels_.append(std::move(cl)));
    }
  }
  // Register the code, to which the table will jump to.
  void addCodeEntry(MacroAssembler& masm) {
    Label entry;
    masm.bind(&entry);
    masm.propagateOOM(labels_.append(std::move(entry)));
  }

  void setOutOfLine() { isOutOfLine_ = true; }
};

template <SwitchTableType tableType>
void CodeGenerator::visitOutOfLineSwitch(
    OutOfLineSwitch<tableType>* jumpTable) {
  jumpTable->setOutOfLine();
  auto& labels = jumpTable->labels();

  if (tableType == SwitchTableType::OutOfLine) {
#if defined(JS_CODEGEN_ARM)
    MOZ_CRASH("NYI: SwitchTableType::OutOfLine");
#elif defined(JS_CODEGEN_NONE)
    MOZ_CRASH();
#else

#  if defined(JS_CODEGEN_ARM64)
    AutoForbidPoolsAndNops afp(
        &masm,
        (labels.length() + 1) * (sizeof(void*) / vixl::kInstructionSize));
#  endif

    masm.haltingAlign(sizeof(void*));

    // Bind the address of the jump table and reserve the space for code
    // pointers to jump in the newly generated code.
    masm.bind(jumpTable->start());
    masm.addCodeLabel(*jumpTable->start());
    for (size_t i = 0, e = labels.length(); i < e; i++) {
      jumpTable->addTableEntry(masm);
    }
#endif
  }

  // Register all reserved pointers of the jump table to target labels. The
  // entries of the jump table need to be absolute addresses and thus must be
  // patched after codegen is finished.
  auto& codeLabels = jumpTable->codeLabels();
  for (size_t i = 0, e = codeLabels.length(); i < e; i++) {
    auto& cl = codeLabels[i];
    cl.target()->bind(labels[i].offset());
    masm.addCodeLabel(cl);
  }
}

template void CodeGenerator::visitOutOfLineSwitch(
    OutOfLineSwitch<SwitchTableType::Inline>* jumpTable);
template void CodeGenerator::visitOutOfLineSwitch(
    OutOfLineSwitch<SwitchTableType::OutOfLine>* jumpTable);

template <typename T>
static inline void StoreToTypedArray(MacroAssembler& masm,
                                     Scalar::Type writeType,
                                     const LAllocation* value, const T& dest) {
  if (writeType == Scalar::Float32 || writeType == Scalar::Float64) {
    masm.storeToTypedFloatArray(writeType, ToFloatRegister(value), dest);
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
  const LAllocation* value = lir->value();

  const MStoreUnboxedScalar* mir = lir->mir();

  Scalar::Type writeType = mir->writeType();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), writeType);
    StoreToTypedArray(masm, writeType, value, dest);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(writeType));
    StoreToTypedArray(masm, writeType, value, dest);
  }
}

void CodeGenerator::visitStoreUnboxedBigInt(LStoreUnboxedBigInt* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp = ToRegister64(lir->temp());

  Scalar::Type writeType = lir->mir()->writeType();

  masm.loadBigInt64(value, temp);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), writeType);
    masm.storeToTypedBigIntArray(writeType, temp, dest);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(writeType));
    masm.storeToTypedBigIntArray(writeType, temp, dest);
  }
}

void CodeGenerator::visitStoreDataViewElement(LStoreDataViewElement* lir) {
  Register elements = ToRegister(lir->elements());
  const LAllocation* value = lir->value();
  const LAllocation* littleEndian = lir->littleEndian();
  Register temp = ToTempRegisterOrInvalid(lir->temp());
  Register64 temp64 = ToTempRegister64OrInvalid(lir->temp64());

  const MStoreDataViewElement* mir = lir->mir();
  Scalar::Type writeType = mir->writeType();

  BaseIndex dest(elements, ToRegister(lir->index()), TimesOne);

  bool noSwap = littleEndian->isConstant() &&
                ToBoolean(littleEndian) == MOZ_LITTLE_ENDIAN();

  // Directly store if no byte swap is needed and the platform supports
  // unaligned accesses for floating point registers.
  if (noSwap && MacroAssembler::SupportsFastUnalignedAccesses()) {
    if (!Scalar::isBigIntType(writeType)) {
      StoreToTypedArray(masm, writeType, value, dest);
    } else {
      masm.loadBigInt64(ToRegister(value), temp64);
      masm.storeToTypedBigIntArray(writeType, temp64, dest);
    }
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
    case Scalar::Float32: {
      FloatRegister fvalue = ToFloatRegister(value);
      masm.canonicalizeFloatIfDeterministic(fvalue);
      masm.moveFloat32ToGPR(fvalue, temp);
      break;
    }
    case Scalar::Float64: {
      FloatRegister fvalue = ToFloatRegister(value);
      masm.canonicalizeDoubleIfDeterministic(fvalue);
      masm.moveDoubleToGPR64(fvalue, temp64);
      break;
    }
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      masm.loadBigInt64(ToRegister(value), temp64);
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
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
        masm.byteSwap16ZeroExtend(temp);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
      case Scalar::Float32:
        masm.byteSwap32(temp);
        break;
      case Scalar::Float64:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
        masm.byteSwap64(temp64);
        break;
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
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
      masm.store16Unaligned(temp, dest);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
      masm.store32Unaligned(temp, dest);
      break;
    case Scalar::Float64:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      masm.store64Unaligned(temp64, dest);
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

void CodeGenerator::visitStoreTypedArrayElementHole(
    LStoreTypedArrayElementHole* lir) {
  Register elements = ToRegister(lir->elements());
  const LAllocation* value = lir->value();

  Scalar::Type arrayType = lir->mir()->arrayType();

  Register index = ToRegister(lir->index());
  const LAllocation* length = lir->length();
  Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

  Label skip;
  if (length->isRegister()) {
    masm.spectreBoundsCheckPtr(index, ToRegister(length), spectreTemp, &skip);
  } else {
    masm.spectreBoundsCheckPtr(index, ToAddress(length), spectreTemp, &skip);
  }

  BaseIndex dest(elements, index, ScaleFromScalarType(arrayType));
  StoreToTypedArray(masm, arrayType, value, dest);

  masm.bind(&skip);
}

void CodeGenerator::visitStoreTypedArrayElementHoleBigInt(
    LStoreTypedArrayElementHoleBigInt* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp = ToRegister64(lir->temp());

  Scalar::Type arrayType = lir->mir()->arrayType();

  Register index = ToRegister(lir->index());
  const LAllocation* length = lir->length();
  Register spectreTemp = temp.scratchReg();

  Label skip;
  if (length->isRegister()) {
    masm.spectreBoundsCheckPtr(index, ToRegister(length), spectreTemp, &skip);
  } else {
    masm.spectreBoundsCheckPtr(index, ToAddress(length), spectreTemp, &skip);
  }

  masm.loadBigInt64(value, temp);

  BaseIndex dest(elements, index, ScaleFromScalarType(arrayType));
  masm.storeToTypedBigIntArray(arrayType, temp, dest);

  masm.bind(&skip);
}

void CodeGenerator::visitAtomicIsLockFree(LAtomicIsLockFree* lir) {
  Register value = ToRegister(lir->value());
  Register output = ToRegister(lir->output());

  masm.atomicIsLockFreeJS(value, output);
}

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
  ValueOperand operand = ToValue(lir, LClampVToUint8::Input);
  FloatRegister tempFloat = ToFloatRegister(lir->tempFloat());
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
      toConstantOrRegister(ins, LInCache::LHS, ins->mir()->key()->type());
  Register object = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp());

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
  emitInstanceOf(ins, ins->rhs());
}

void CodeGenerator::visitInstanceOfV(LInstanceOfV* ins) {
  emitInstanceOf(ins, ins->rhs());
}

void CodeGenerator::emitInstanceOf(LInstruction* ins,
                                   const LAllocation* prototypeObject) {
  // This path implements fun_hasInstance when the function's prototype is
  // known to be prototypeObject.

  Label done;
  Register output = ToRegister(ins->getDef(0));

  // If the lhs is a primitive, the result is false.
  Register objReg;
  if (ins->isInstanceOfV()) {
    Label isObject;
    ValueOperand lhsValue = ToValue(ins, LInstanceOfV::LHS);
    masm.branchTestObject(Assembler::Equal, lhsValue, &isObject);
    masm.mov(ImmWord(0), output);
    masm.jump(&done);
    masm.bind(&isObject);
    objReg = masm.extractObject(lhsValue, output);
  } else {
    objReg = ToRegister(ins->toInstanceOfO()->lhs());
  }

  mozilla::Maybe<ImmGCPtr> protoPtr;
  mozilla::Maybe<Register> protoReg;
  if (prototypeObject->isConstant()) {
    protoPtr.emplace(&prototypeObject->toConstant()->toObject());
  } else {
    protoReg.emplace(ToRegister(prototypeObject));
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
    if (protoPtr) {
      masm.branchPtr(Assembler::NotEqual, output, *protoPtr,
                     &notPrototypeObject);
    } else {
      masm.branchPtr(Assembler::NotEqual, output, *protoReg,
                     &notPrototypeObject);
    }
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
  OutOfLineCode* ool;
  if (protoPtr) {
    ool = oolCallVM<Fn, IsPrototypeOf>(ins, ArgList(*protoPtr, objReg),
                                       StoreRegisterTo(output));
  } else {
    ool = oolCallVM<Fn, IsPrototypeOf>(ins, ArgList(*protoReg, objReg),
                                       StoreRegisterTo(output));
  }

  // Regenerate the original lhs object for the VM call.
  Label regenerate, *lazyEntry;
  if (objReg != output) {
    lazyEntry = ool->entry();
  } else {
    masm.bind(&regenerate);
    lazyEntry = &regenerate;
    if (ins->isInstanceOfV()) {
      ValueOperand lhsValue = ToValue(ins, LInstanceOfV::LHS);
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
  TypedOrValueRegister lhs =
      TypedOrValueRegister(ToValue(ins, LInstanceOfCache::LHS));
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());

  IonInstanceOfIC ic(liveRegs, lhs, rhs, output);
  addIC(ins, allocateIC(ic));
}

void CodeGenerator::visitGetDOMProperty(LGetDOMProperty* ins) {
  const Register JSContextReg = ToRegister(ins->getJSContextReg());
  const Register ObjectReg = ToRegister(ins->getObjectReg());
  const Register PrivateReg = ToRegister(ins->getPrivReg());
  const Register ValueReg = ToRegister(ins->getValueReg());

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

  masm.setupUnalignedABICall(JSContextReg);
  masm.loadJSContext(JSContextReg);
  masm.passABIArg(JSContextReg);
  masm.passABIArg(ObjectReg);
  masm.passABIArg(PrivateReg);
  masm.passABIArg(ValueReg);
  masm.callWithABI(DynamicFunction<JSJitGetterOp>(ins->mir()->fun()),
                   MoveOp::GENERAL,
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
  AnyRegister result = ToAnyRegister(ins->getDef(0));
  MIRType type = ins->mir()->type();

  masm.loadUnboxedValue(Address(object, NativeObject::getFixedSlotOffset(slot)),
                        type, result);
}

void CodeGenerator::visitSetDOMProperty(LSetDOMProperty* ins) {
  const Register JSContextReg = ToRegister(ins->getJSContextReg());
  const Register ObjectReg = ToRegister(ins->getObjectReg());
  const Register PrivateReg = ToRegister(ins->getPrivReg());
  const Register ValueReg = ToRegister(ins->getValueReg());

  DebugOnly<uint32_t> initialStack = masm.framePushed();

  masm.checkStackAlignment();

  // Push the argument. Rooting will happen at GC time.
  ValueOperand argVal = ToValue(ins, LSetDOMProperty::Value);
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

  masm.setupUnalignedABICall(JSContextReg);
  masm.loadJSContext(JSContextReg);
  masm.passABIArg(JSContextReg);
  masm.passABIArg(ObjectReg);
  masm.passABIArg(PrivateReg);
  masm.passABIArg(ValueReg);
  masm.callWithABI(DynamicFunction<JSJitSetterOp>(ins->mir()->fun()),
                   MoveOp::GENERAL,
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
  Register temp = ToRegister(ins->temp());
  ValueOperand input = ToValue(ins, LGuardDOMExpandoMissingOrGuardShape::Input);

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

class OutOfLineIsCallable : public OutOfLineCodeBase<CodeGenerator> {
  Register object_;
  Register output_;

 public:
  OutOfLineIsCallable(Register object, Register output)
      : object_(object), output_(output) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineIsCallable(this);
  }
  Register object() const { return object_; }
  Register output() const { return output_; }
};

void CodeGenerator::visitIsCallableO(LIsCallableO* ins) {
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  OutOfLineIsCallable* ool = new (alloc()) OutOfLineIsCallable(object, output);
  addOutOfLineCode(ool, ins->mir());

  masm.isCallable(object, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitIsCallableV(LIsCallableV* ins) {
  ValueOperand val = ToValue(ins, LIsCallableV::Value);
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp());

  Label notObject;
  masm.fallibleUnboxObject(val, temp, &notObject);

  OutOfLineIsCallable* ool = new (alloc()) OutOfLineIsCallable(temp, output);
  addOutOfLineCode(ool, ins->mir());

  masm.isCallable(temp, output, ool->entry());
  masm.jump(ool->rejoin());

  masm.bind(&notObject);
  masm.move32(Imm32(0), output);

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineIsCallable(OutOfLineIsCallable* ool) {
  Register object = ool->object();
  Register output = ool->output();

  saveVolatile(output);
  using Fn = bool (*)(JSObject * obj);
  masm.setupUnalignedABICall(output);
  masm.passABIArg(object);
  masm.callWithABI<Fn, ObjectIsCallable>();
  masm.storeCallBoolResult(output);
  restoreVolatile(output);
  masm.jump(ool->rejoin());
}

class OutOfLineIsConstructor : public OutOfLineCodeBase<CodeGenerator> {
  LIsConstructor* ins_;

 public:
  explicit OutOfLineIsConstructor(LIsConstructor* ins) : ins_(ins) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineIsConstructor(this);
  }
  LIsConstructor* ins() const { return ins_; }
};

void CodeGenerator::visitIsConstructor(LIsConstructor* ins) {
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  OutOfLineIsConstructor* ool = new (alloc()) OutOfLineIsConstructor(ins);
  addOutOfLineCode(ool, ins->mir());

  masm.isConstructor(object, output, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitOutOfLineIsConstructor(OutOfLineIsConstructor* ool) {
  LIsConstructor* ins = ool->ins();
  Register object = ToRegister(ins->object());
  Register output = ToRegister(ins->output());

  saveVolatile(output);
  using Fn = bool (*)(JSObject * obj);
  masm.setupUnalignedABICall(output);
  masm.passABIArg(object);
  masm.callWithABI<Fn, ObjectIsConstructor>();
  masm.storeCallBoolResult(output);
  restoreVolatile(output);
  masm.jump(ool->rejoin());
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
  ValueOperand val = ToValue(lir, LIsArrayV::Value);
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp());

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
    masm.branchTestClassIsProxy(true, output, ool->entry());
  }
  masm.move32(Imm32(0), output);
  masm.bind(&done);
  if (ool) {
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitIsObject(LIsObject* ins) {
  Register output = ToRegister(ins->output());
  ValueOperand value = ToValue(ins, LIsObject::Input);
  masm.testObjectSet(Assembler::Equal, value, output);
}

void CodeGenerator::visitIsObjectAndBranch(LIsObjectAndBranch* ins) {
  ValueOperand value = ToValue(ins, LIsObjectAndBranch::Input);
  testObjectEmitBranch(Assembler::Equal, value, ins->ifTrue(), ins->ifFalse());
}

void CodeGenerator::visitIsNullOrUndefined(LIsNullOrUndefined* ins) {
  Register output = ToRegister(ins->output());
  ValueOperand value = ToValue(ins, LIsNullOrUndefined::Input);

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
  ValueOperand value = ToValue(ins, LIsNullOrUndefinedAndBranch::Input);

  ScratchTagScope tag(masm, value);
  masm.splitTagForTest(value, tag);

  masm.branchTestNull(Assembler::Equal, tag, ifTrue);
  masm.branchTestUndefined(Assembler::Equal, tag, ifTrue);

  if (!isNextBlock(ins->ifFalse()->lir())) {
    masm.jump(ifFalse);
  }
}

void CodeGenerator::loadOutermostJSScript(Register reg) {
  // The "outermost" JSScript means the script that we are compiling
  // basically; this is not always the script associated with the
  // current basic block, which might be an inlined script.

  MIRGraph& graph = current->mir()->graph();
  MBasicBlock* entryBlock = graph.entryBlock();
  masm.movePtr(ImmGCPtr(entryBlock->info().script()), reg);
}

void CodeGenerator::loadJSScriptForBlock(MBasicBlock* block, Register reg) {
  // The current JSScript means the script for the current
  // basic block. This may be an inlined script.

  JSScript* script = block->info().script();
  masm.movePtr(ImmGCPtr(script), reg);
}

void CodeGenerator::visitHasClass(LHasClass* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register output = ToRegister(ins->output());

  masm.loadObjClassUnsafe(lhs, output);
  masm.cmpPtrSet(Assembler::Equal, output, ImmPtr(ins->mir()->getClass()),
                 output);
}

void CodeGenerator::visitGuardToClass(LGuardToClass* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register temp = ToRegister(ins->temp());

  // branchTestObjClass may zero the object register on speculative paths
  // (we should have a defineReuseInput allocation in this case).
  Register spectreRegToZero = lhs;

  Label notEqual;

  masm.branchTestObjClass(Assembler::NotEqual, lhs, ins->mir()->getClass(),
                          temp, spectreRegToZero, &notEqual);

  // Can't return null-return here, so bail.
  bailoutFrom(&notEqual, ins->snapshot());
}

void CodeGenerator::visitObjectClassToString(LObjectClassToString* lir) {
  Register obj = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp());

  using Fn = JSString* (*)(JSContext*, JSObject*);
  masm.setupUnalignedABICall(temp);
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
  Register temp = ToRegister(ins->getTemp(0));

  Label success;
  masm.branchTestObjClassNoSpectreMitigations(
      Assembler::Equal, obj, ins->mir()->getClass(), temp, &success);
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

void CodeGenerator::visitAssertResultV(LAssertResultV* ins) {
#ifdef DEBUG
  const ValueOperand value = ToValue(ins, LAssertResultV::Input);
  emitAssertResultV(value, ins->mirRaw());
#else
  MOZ_CRASH("LAssertResultV is debug only");
#endif
}

void CodeGenerator::visitAssertResultT(LAssertResultT* ins) {
#ifdef DEBUG
  Register input = ToRegister(ins->input());
  emitAssertGCThingResult(input, ins->mirRaw());
#else
  MOZ_CRASH("LAssertResultT is debug only");
#endif
}

void CodeGenerator::visitAssertRangeI(LAssertRangeI* ins) {
  Register input = ToRegister(ins->input());
  const Range* r = ins->range();

  emitAssertRangeI(ins->mir()->input()->type(), r, input);
}

void CodeGenerator::visitAssertRangeD(LAssertRangeD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister temp = ToFloatRegister(ins->temp());
  const Range* r = ins->range();

  emitAssertRangeD(r, input, temp);
}

void CodeGenerator::visitAssertRangeF(LAssertRangeF* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister temp = ToFloatRegister(ins->temp());
  FloatRegister temp2 = ToFloatRegister(ins->temp2());

  const Range* r = ins->range();

  masm.convertFloat32ToDouble(input, temp);
  emitAssertRangeD(r, temp, temp2);
}

void CodeGenerator::visitAssertRangeV(LAssertRangeV* ins) {
  const Range* r = ins->range();
  const ValueOperand value = ToValue(ins, LAssertRangeV::Input);
  Label done;

  {
    ScratchTagScope tag(masm, value);
    masm.splitTagForTest(value, tag);

    {
      Label isNotInt32;
      masm.branchTestInt32(Assembler::NotEqual, tag, &isNotInt32);
      {
        ScratchTagScopeRelease _(&tag);
        Register unboxInt32 = ToTempUnboxRegister(ins->temp());
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
        FloatRegister input = ToFloatRegister(ins->floatTemp1());
        FloatRegister temp = ToFloatRegister(ins->floatTemp2());
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

  masm.wasmInterruptCheck(ToRegister(lir->tlsPtr()),
                          lir->mir()->bytecodeOffset());

  markSafepointAt(masm.currentOffset(), lir);

  // Note that masm.framePushed() doesn't include the register dump area.
  // That will be taken into account when the StackMap is created from the
  // LSafepoint.
  lir->safepoint()->setFramePushedAtStackMapBase(masm.framePushed());
  lir->safepoint()->setIsWasmTrap();
}

void CodeGenerator::visitWasmTrap(LWasmTrap* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  const MWasmTrap* mir = lir->mir();

  masm.wasmTrap(mir->trap(), mir->bytecodeOffset());
}

void CodeGenerator::visitWasmBoundsCheck(LWasmBoundsCheck* ins) {
  const MWasmBoundsCheck* mir = ins->mir();
  Label ok;
  Register ptr = ToRegister(ins->ptr());
  Register boundsCheckLimit = ToRegister(ins->boundsCheckLimit());
  masm.wasmBoundsCheck32(Assembler::Below, ptr, boundsCheckLimit, &ok);
  masm.wasmTrap(wasm::Trap::OutOfBounds, mir->bytecodeOffset());
  masm.bind(&ok);
}

void CodeGenerator::visitWasmBoundsCheck64(LWasmBoundsCheck64* ins) {
  const MWasmBoundsCheck* mir = ins->mir();
  Label ok;
#ifdef JS_64BIT
  Register64 ptr = ToRegister64(ins->ptr());
  Register64 boundsCheckLimit = ToRegister64(ins->boundsCheckLimit());
  masm.wasmBoundsCheck64(Assembler::Below, ptr, boundsCheckLimit, &ok);
#else
  // 64-bit bounds checks are used only on 64-bit systems.
  MOZ_CRASH("Should not happen");
#endif
  masm.wasmTrap(wasm::Trap::OutOfBounds, mir->bytecodeOffset());
  masm.bind(&ok);
}

void CodeGenerator::visitWasmAlignmentCheck(LWasmAlignmentCheck* ins) {
  const MWasmAlignmentCheck* mir = ins->mir();
  Register ptr = ToRegister(ins->ptr());
  Label ok;
  masm.branchTest32(Assembler::Zero, ptr, Imm32(mir->byteSize() - 1), &ok);
  masm.wasmTrap(wasm::Trap::UnalignedAccess, mir->bytecodeOffset());
  masm.bind(&ok);
}

void CodeGenerator::visitWasmLoadTls(LWasmLoadTls* ins) {
  switch (ins->mir()->type()) {
    case MIRType::Pointer:
      masm.loadPtr(Address(ToRegister(ins->tlsPtr()), ins->mir()->offset()),
                   ToRegister(ins->output()));
      break;
    case MIRType::Int32:
      masm.load32(Address(ToRegister(ins->tlsPtr()), ins->mir()->offset()),
                  ToRegister(ins->output()));
      break;
#ifdef JS_64BIT
    case MIRType::Int64:
      masm.load64(Address(ToRegister(ins->tlsPtr()), ins->mir()->offset()),
                  ToOutRegister64(ins));
      break;
#endif
    default:
      MOZ_CRASH("MIRType not supported in WasmLoadTls");
  }
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
  Register tmp = ToRegister(ins->scratch());

  AbsoluteAddress warmUpCount =
      AbsoluteAddress(ins->mir()->script()->jitScript())
          .offset(JitScript::offsetOfWarmUpCount());
  incrementWarmUpCounter(warmUpCount, ins->mir()->script(), tmp);
}

void CodeGenerator::visitLexicalCheck(LLexicalCheck* ins) {
  ValueOperand inputValue = ToValue(ins, LLexicalCheck::Input);
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

  using Fn = bool (*)(JSContext*, HandleScript, jsbytecode*);
  callVM<Fn, GlobalDeclInstantiationFromIon>(ins);
}

void CodeGenerator::visitDebugger(LDebugger* ins) {
  Register cx = ToRegister(ins->getTemp(0));
  Register temp = ToRegister(ins->getTemp(1));

  masm.loadJSContext(cx);
  using Fn = bool (*)(JSContext * cx);
  masm.setupUnalignedABICall(temp);
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
  Address calleeToken(masm.getStackPointer(),
                      frameSize() + JitFrameLayout::offsetOfCalleeToken());
  masm.branchTestPtr(Assembler::Zero, calleeToken,
                     Imm32(CalleeToken_FunctionConstructing), &notConstructing);

  Register argvLen = output.scratchReg();

  Address actualArgsPtr(masm.getStackPointer(),
                        frameSize() + JitFrameLayout::offsetOfNumActualArgs());
  masm.loadPtr(actualArgsPtr, argvLen);

  Label useNFormals;

  size_t numFormalArgs = ins->mirRaw()->block()->info().nargs();
  masm.branchPtr(Assembler::Below, argvLen, Imm32(numFormalArgs), &useNFormals);

  size_t argsOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();
  {
    BaseValueIndex newTarget(masm.getStackPointer(), argvLen, argsOffset);
    masm.loadValue(newTarget, output);
    masm.jump(&done);
  }

  masm.bind(&useNFormals);

  {
    Address newTarget(masm.getStackPointer(),
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
  ValueOperand returnValue = ToValue(ins, LCheckReturn::ReturnValue);
  ValueOperand thisValue = ToValue(ins, LCheckReturn::ThisValue);
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
  ValueOperand value = ToValue(ins, LCheckIsObj::ValueIndex);
  Register output = ToRegister(ins->output());

  using Fn = bool (*)(JSContext*, CheckIsObjectKind);
  OutOfLineCode* ool = oolCallVM<Fn, ThrowCheckIsObject>(
      ins, ArgList(Imm32(ins->mir()->checkKind())), StoreNothing());

  masm.fallibleUnboxObject(value, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckObjCoercible(LCheckObjCoercible* ins) {
  ValueOperand checkValue = ToValue(ins, LCheckObjCoercible::CheckValue);

  using Fn = bool (*)(JSContext*, HandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, ThrowObjectCoercible>(
      ins, ArgList(checkValue), StoreNothing());
  masm.branchTestNull(Assembler::Equal, checkValue, ool->entry());
  masm.branchTestUndefined(Assembler::Equal, checkValue, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckClassHeritage(LCheckClassHeritage* ins) {
  ValueOperand heritage = ToValue(ins, LCheckClassHeritage::Heritage);
  Register temp1 = ToRegister(ins->temp1());
  Register temp2 = ToRegister(ins->temp2());

  using Fn = bool (*)(JSContext*, HandleValue);
  OutOfLineCode* ool = oolCallVM<Fn, CheckClassHeritageOperation>(
      ins, ArgList(heritage), StoreNothing());

  masm.branchTestNull(Assembler::Equal, heritage, ool->rejoin());
  masm.fallibleUnboxObject(heritage, temp1, ool->entry());

  masm.isConstructor(temp1, temp2, ool->entry());
  masm.branchTest32(Assembler::Zero, temp2, temp2, ool->entry());

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckThis(LCheckThis* ins) {
  ValueOperand thisValue = ToValue(ins, LCheckThis::ThisValue);

  using Fn = bool (*)(JSContext*);
  OutOfLineCode* ool =
      oolCallVM<Fn, ThrowUninitializedThis>(ins, ArgList(), StoreNothing());
  masm.branchTestMagic(Assembler::Equal, thisValue, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCheckThisReinit(LCheckThisReinit* ins) {
  ValueOperand thisValue = ToValue(ins, LCheckThisReinit::ThisValue);

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

  using Fn = JSObject* (*)(JSContext * cx, HandleFunction, HandleScript,
                           HandleObject, HandleObject);
  callVM<Fn, CreateGenerator>(lir);
}

void CodeGenerator::visitAsyncResolve(LAsyncResolve* lir) {
  Register generator = ToRegister(lir->generator());
  ValueOperand valueOrReason = ToValue(lir, LAsyncResolve::ValueOrReasonInput);
  AsyncFunctionResolveKind resolveKind = lir->mir()->resolveKind();

  pushArg(Imm32(static_cast<int32_t>(resolveKind)));
  pushArg(valueOrReason);
  pushArg(generator);

  using Fn = JSObject* (*)(JSContext*, Handle<AsyncFunctionGeneratorObject*>,
                           HandleValue, AsyncFunctionResolveKind);
  callVM<Fn, js::AsyncFunctionResolve>(lir);
}

void CodeGenerator::visitAsyncAwait(LAsyncAwait* lir) {
  ValueOperand value = ToValue(lir, LAsyncAwait::ValueInput);
  Register generator = ToRegister(lir->generator());

  pushArg(value);
  pushArg(generator);

  using Fn = JSObject* (*)(JSContext * cx,
                           Handle<AsyncFunctionGeneratorObject*> genObj,
                           HandleValue value);
  callVM<Fn, js::AsyncFunctionAwait>(lir);
}

void CodeGenerator::visitCanSkipAwait(LCanSkipAwait* lir) {
  ValueOperand value = ToValue(lir, LCanSkipAwait::ValueInput);

  pushArg(value);

  using Fn = bool (*)(JSContext*, HandleValue, bool* canSkip);
  callVM<Fn, js::CanSkipAwait>(lir);
}

void CodeGenerator::visitMaybeExtractAwaitValue(LMaybeExtractAwaitValue* lir) {
  ValueOperand value = ToValue(lir, LMaybeExtractAwaitValue::ValueInput);
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
  ValueOperand checkValue = ToValue(ins, LDebugCheckSelfHosted::CheckValue);
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
}

void CodeGenerator::visitSignExtendInt32(LSignExtendInt32* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  switch (ins->mode()) {
    case MSignExtendInt32::Byte:
      masm.move8SignExtend(input, output);
      break;
    case MSignExtendInt32::Half:
      masm.move16SignExtend(input, output);
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

class OutOfLineNaNToZero : public OutOfLineCodeBase<CodeGenerator> {
  LNaNToZero* lir_;

 public:
  explicit OutOfLineNaNToZero(LNaNToZero* lir) : lir_(lir) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineNaNToZero(this);
  }
  LNaNToZero* lir() const { return lir_; }
};

void CodeGenerator::visitOutOfLineNaNToZero(OutOfLineNaNToZero* ool) {
  FloatRegister output = ToFloatRegister(ool->lir()->output());
  masm.loadConstantDouble(0.0, output);
  masm.jump(ool->rejoin());
}

void CodeGenerator::visitNaNToZero(LNaNToZero* lir) {
  FloatRegister input = ToFloatRegister(lir->input());

  OutOfLineNaNToZero* ool = new (alloc()) OutOfLineNaNToZero(lir);
  addOutOfLineCode(ool, lir->mir());

  if (lir->mir()->operandIsNeverNegativeZero()) {
    masm.branchDouble(Assembler::DoubleUnordered, input, input, ool->entry());
  } else {
    FloatRegister scratch = ToFloatRegister(lir->tempDouble());
    masm.loadConstantDouble(0.0, scratch);
    masm.branchDouble(Assembler::DoubleEqualOrUnordered, input, scratch,
                      ool->entry());
  }
  masm.bind(ool->rejoin());
}

static void BoundFunctionLength(MacroAssembler& masm, Register target,
                                Register targetFlags, Register argCount,
                                Register output, Label* slowPath) {
  masm.loadFunctionLength(target, targetFlags, output, slowPath);

  // Compute the bound function length: Max(0, target.length - argCount).
  Label nonNegative;
  masm.sub32(argCount, output);
  masm.branch32(Assembler::GreaterThanOrEqual, output, Imm32(0), &nonNegative);
  masm.move32(Imm32(0), output);
  masm.bind(&nonNegative);
}

static void BoundFunctionName(MacroAssembler& masm, Register target,
                              Register targetFlags, Register output,
                              const JSAtomState& names, Label* slowPath) {
  Label notBoundTarget, loadName;
  masm.branchTest32(Assembler::Zero, targetFlags,
                    Imm32(FunctionFlags::BOUND_FUN), &notBoundTarget);
  {
    // Call into the VM if the target's name atom contains the bound
    // function prefix.
    masm.branchTest32(Assembler::NonZero, targetFlags,
                      Imm32(FunctionFlags::HAS_BOUND_FUNCTION_NAME_PREFIX),
                      slowPath);

    // Bound functions reuse HAS_GUESSED_ATOM for
    // HAS_BOUND_FUNCTION_NAME_PREFIX, so skip the guessed atom check below.
    static_assert(
        FunctionFlags::HAS_BOUND_FUNCTION_NAME_PREFIX ==
            FunctionFlags::HAS_GUESSED_ATOM,
        "HAS_BOUND_FUNCTION_NAME_PREFIX is shared with HAS_GUESSED_ATOM");
    masm.jump(&loadName);
  }
  masm.bind(&notBoundTarget);

  Label guessed, hasName;
  masm.branchTest32(Assembler::NonZero, targetFlags,
                    Imm32(FunctionFlags::HAS_GUESSED_ATOM), &guessed);
  masm.bind(&loadName);
  masm.loadPtr(Address(target, JSFunction::offsetOfAtom()), output);
  masm.branchTestPtr(Assembler::NonZero, output, output, &hasName);
  {
    masm.bind(&guessed);

    // An absent name property defaults to the empty string.
    masm.movePtr(ImmGCPtr(names.empty), output);
  }
  masm.bind(&hasName);
}

static void BoundFunctionFlags(MacroAssembler& masm, Register targetFlags,
                               Register bound, Register output) {
  // Set the BOUND_FN flag and, if the target is a constructor, the
  // CONSTRUCTOR flag.
  Label isConstructor, boundFlagsComputed;
  masm.load16ZeroExtend(Address(bound, JSFunction::offsetOfFlags()), output);
  masm.branchTest32(Assembler::NonZero, targetFlags,
                    Imm32(FunctionFlags::CONSTRUCTOR), &isConstructor);
  {
    masm.or32(Imm32(FunctionFlags::BOUND_FUN), output);
    masm.jump(&boundFlagsComputed);
  }
  masm.bind(&isConstructor);
  {
    masm.or32(Imm32(FunctionFlags::BOUND_FUN | FunctionFlags::CONSTRUCTOR),
              output);
  }
  masm.bind(&boundFlagsComputed);
}

void CodeGenerator::visitFinishBoundFunctionInit(
    LFinishBoundFunctionInit* lir) {
  Register bound = ToRegister(lir->bound());
  Register target = ToRegister(lir->target());
  Register argCount = ToRegister(lir->argCount());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());

  using Fn = bool (*)(JSContext * cx, HandleFunction bound, HandleObject target,
                      int32_t argCount);
  OutOfLineCode* ool = oolCallVM<Fn, JSFunction::finishBoundFunctionInit>(
      lir, ArgList(bound, target, argCount), StoreNothing());
  Label* slowPath = ool->entry();

  const size_t boundLengthOffset =
      FunctionExtended::offsetOfBoundFunctionLengthSlot();

  // Take the slow path if the target is not a JSFunction.
  masm.branchTestObjClass(Assembler::NotEqual, target, &JSFunction::class_,
                          temp1, target, slowPath);

  // Take the slow path if we'd need to adjust the [[Prototype]].
  masm.loadObjProto(bound, temp1);
  masm.loadObjProto(target, temp2);
  masm.branchPtr(Assembler::NotEqual, temp1, temp2, slowPath);

  // Get the function flags.
  masm.load16ZeroExtend(Address(target, JSFunction::offsetOfFlags()), temp1);

  // Functions with a SelfHostedLazyScript must be compiled with the slow-path
  // before the function length is known. If the length or name property is
  // resolved, it might be shadowed.
  masm.branchTest32(
      Assembler::NonZero, temp1,
      Imm32(FunctionFlags::SELFHOSTLAZY | FunctionFlags::RESOLVED_NAME |
            FunctionFlags::RESOLVED_LENGTH),
      slowPath);

  // Store the bound function's length into the extended slot.
  BoundFunctionLength(masm, target, temp1, argCount, temp2, slowPath);
  masm.storeValue(JSVAL_TYPE_INT32, temp2, Address(bound, boundLengthOffset));

  // Store the target's name atom in the bound function as is.
  BoundFunctionName(masm, target, temp1, temp2, gen->runtime->names(),
                    slowPath);
  masm.storePtr(temp2, Address(bound, JSFunction::offsetOfAtom()));

  // Update the bound function's flags.
  BoundFunctionFlags(masm, temp1, bound, temp2);
  masm.store16(temp2, Address(bound, JSFunction::offsetOfFlags()));

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitIsPackedArray(LIsPackedArray* lir) {
  Register obj = ToRegister(lir->object());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp());

  masm.setIsPackedArray(obj, output, temp);
}

void CodeGenerator::visitGuardArrayIsPacked(LGuardArrayIsPacked* lir) {
  Register array = ToRegister(lir->array());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());

  Label bail;
  masm.branchArrayIsNotPacked(array, temp1, temp2, &bail);
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
  pushArg(ToValue(lir, LObjectWithProto::PrototypeValue));

  using Fn = PlainObject* (*)(JSContext*, HandleValue);
  callVM<Fn, js::ObjectWithProtoOperation>(lir);
}

void CodeGenerator::visitObjectStaticProto(LObjectStaticProto* lir) {
  Register obj = ToRegister(lir->input());
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
  Register temp = ToRegister(lir->temp());

#ifdef DEBUG
  Label classCheckDone;
  masm.branchTestObjClass(Assembler::Equal, callee, &JSFunction::class_, temp,
                          callee, &classCheckDone);
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
  ValueOperand homeObject = ToValue(lir, LInitHomeObject::HomeObjectValue);

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
  ValueOperand value = ToValue(lir, LLoadValueTag::Value);
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
  masm.unboxObject(
      Address(output, js::detail::ProxyReservedSlots::offsetOfPrivateSlot()),
      output);
}

void CodeGenerator::visitGuardHasGetterSetter(LGuardHasGetterSetter* lir) {
  Register object = ToRegister(lir->object());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register temp3 = ToRegister(lir->temp3());

  masm.movePropertyKey(lir->mir()->propId(), temp2);
  masm.movePtr(ImmGCPtr(lir->mir()->getterSetter()), temp3);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, jsid id,
                      GetterSetter * getterSetter);
  masm.setupUnalignedABICall(temp1);
  masm.loadJSContext(temp1);
  masm.passABIArg(temp1);
  masm.passABIArg(object);
  masm.passABIArg(temp2);
  masm.passABIArg(temp3);
  masm.callWithABI<Fn, ObjectHasGetterSetterPure>();

  bailoutIfFalseBool(ReturnReg, lir->snapshot());
}

void CodeGenerator::visitGuardIsExtensible(LGuardIsExtensible* lir) {
  Register object = ToRegister(lir->object());
  Register temp = ToRegister(lir->temp());

  Label bail;
  masm.branchIfObjectNotExtensible(object, temp, &bail);
  bailoutFrom(&bail, lir->snapshot());
}

void CodeGenerator::visitGuardInt32IsNonNegative(
    LGuardInt32IsNonNegative* lir) {
  Register index = ToRegister(lir->index());

  bailoutCmp32(Assembler::LessThan, index, Imm32(0), lir->snapshot());
}

void CodeGenerator::visitGuardIndexGreaterThanDenseInitLength(
    LGuardIndexGreaterThanDenseInitLength* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());
  Register temp = ToRegister(lir->temp());
  Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

  // Load obj->elements.
  masm.loadPtr(Address(object, NativeObject::offsetOfElements()), temp);

  // Ensure index >= initLength.
  Label outOfBounds;
  Address capacity(temp, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, capacity, spectreTemp, &outOfBounds);
  bailout(lir->snapshot());
  masm.bind(&outOfBounds);
}

void CodeGenerator::visitGuardIndexIsValidUpdateOrAdd(
    LGuardIndexIsValidUpdateOrAdd* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());
  Register temp = ToRegister(lir->temp());
  Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

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
  ValueOperand value = ToValue(lir, LCallAddOrUpdateSparseElement::ValueIndex);

  pushArg(Imm32(lir->mir()->strict()));
  pushArg(value);
  pushArg(index);
  pushArg(object);

  using Fn =
      bool (*)(JSContext*, HandleArrayObject, int32_t, HandleValue, bool);
  callVM<Fn, js::AddOrUpdateSparseElementHelper>(lir);
}

void CodeGenerator::visitCallGetSparseElement(LCallGetSparseElement* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());

  pushArg(index);
  pushArg(object);

  using Fn =
      bool (*)(JSContext*, HandleArrayObject, int32_t, MutableHandleValue);
  callVM<Fn, js::GetSparseElementHelper>(lir);
}

void CodeGenerator::visitCallNativeGetElement(LCallNativeGetElement* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());

  pushArg(index);
  pushArg(TypedOrValueRegister(MIRType::Object, AnyRegister(object)));
  pushArg(object);

  using Fn = bool (*)(JSContext*, HandleNativeObject, HandleValue, int32_t,
                      MutableHandleValue);
  callVM<Fn, js::NativeGetElement>(lir);
}

void CodeGenerator::visitCallObjectHasSparseElement(
    LCallObjectHasSparseElement* lir) {
  Register object = ToRegister(lir->object());
  Register index = ToRegister(lir->index());
  Register temp1 = ToRegister(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register output = ToRegister(lir->output());

  masm.reserveStack(sizeof(Value));
  masm.moveStackPtrTo(temp2);

  using Fn = bool (*)(JSContext*, NativeObject*, int32_t, Value*);
  masm.setupUnalignedABICall(temp1);
  masm.loadJSContext(temp1);
  masm.passABIArg(temp1);
  masm.passABIArg(object);
  masm.passABIArg(index);
  masm.passABIArg(temp2);
  masm.callWithABI<Fn, HasNativeElementPure>();
  masm.mov(ReturnReg, temp1);

  Label bail, ok;
  uint32_t framePushed = masm.framePushed();
  masm.branchIfTrueBool(temp1, &ok);
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

void CodeGenerator::visitBigIntAsIntN64(LBigIntAsIntN64* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp());
  Register64 temp64 = ToRegister64(ins->temp64());
  Register output = ToRegister(ins->output());

  Label done, create;

  masm.movePtr(input, output);

  // Load the BigInt value as an int64.
  masm.loadBigInt64(input, temp64);

  // Create a new BigInt when the input exceeds the int64 range.
  masm.branch32(Assembler::Above, Address(input, BigInt::offsetOfLength()),
                Imm32(64 / BigInt::DigitBits), &create);

  // And create a new BigInt when the value and the BigInt have different signs.
  Label nonNegative;
  masm.branchIfBigIntIsNonNegative(input, &nonNegative);
  masm.branchTest64(Assembler::NotSigned, temp64, temp64, temp, &create);
  masm.jump(&done);

  masm.bind(&nonNegative);
  masm.branchTest64(Assembler::NotSigned, temp64, temp64, temp, &done);

  masm.bind(&create);
  emitCreateBigInt(ins, Scalar::BigInt64, temp64, output, temp);

  masm.bind(&done);
}

void CodeGenerator::visitBigIntAsIntN32(LBigIntAsIntN32* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp());
  Register64 temp64 = ToRegister64(ins->temp64());
  Register output = ToRegister(ins->output());

  Label done, create;

  masm.movePtr(input, output);

  // Load the absolute value of the first digit.
  masm.loadFirstBigIntDigitOrZero(input, temp);

  // If the absolute value exceeds the int32 range, create a new BigInt.
  masm.branchPtr(Assembler::Above, temp, Imm32(INT32_MAX), &create);

  // Also create a new BigInt if we have more than one digit.
  masm.branch32(Assembler::BelowOrEqual,
                Address(input, BigInt::offsetOfLength()), Imm32(1), &done);

  masm.bind(&create);

  // |temp| stores the absolute value, negate it when the sign flag is set.
  Label nonNegative;
  masm.branchIfBigIntIsNonNegative(input, &nonNegative);
  masm.negPtr(temp);
  masm.bind(&nonNegative);

  masm.move32To64SignExtend(temp, temp64);
  emitCreateBigInt(ins, Scalar::BigInt64, temp64, output, temp);

  masm.bind(&done);
}

void CodeGenerator::visitBigIntAsUintN(LBigIntAsUintN* ins) {
  Register bits = ToRegister(ins->bits());
  Register input = ToRegister(ins->input());

  pushArg(bits);
  pushArg(input);

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, int32_t);
  callVM<Fn, jit::BigIntAsUintN>(ins);
}

void CodeGenerator::visitBigIntAsUintN64(LBigIntAsUintN64* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp());
  Register64 temp64 = ToRegister64(ins->temp64());
  Register output = ToRegister(ins->output());

  Label done, create;

  masm.movePtr(input, output);

  // Load the BigInt value as an uint64.
  masm.loadBigInt64(input, temp64);

  // Create a new BigInt when the input exceeds the uint64 range.
  masm.branch32(Assembler::Above, Address(input, BigInt::offsetOfLength()),
                Imm32(64 / BigInt::DigitBits), &create);

  // And create a new BigInt when the input has the sign flag set.
  masm.branchIfBigIntIsNonNegative(input, &done);

  masm.bind(&create);
  emitCreateBigInt(ins, Scalar::BigUint64, temp64, output, temp);

  masm.bind(&done);
}

void CodeGenerator::visitBigIntAsUintN32(LBigIntAsUintN32* ins) {
  Register input = ToRegister(ins->input());
  Register temp = ToRegister(ins->temp());
  Register64 temp64 = ToRegister64(ins->temp64());
  Register output = ToRegister(ins->output());

  Label done, create;

  masm.movePtr(input, output);

  // Load the absolute value of the first digit.
  masm.loadFirstBigIntDigitOrZero(input, temp);

  // If the absolute value exceeds the uint32 range, create a new BigInt.
#if JS_PUNBOX64
  masm.branchPtr(Assembler::Above, temp, ImmWord(UINT32_MAX), &create);
#endif

  // Also create a new BigInt if we have more than one digit.
  masm.branch32(Assembler::Above, Address(input, BigInt::offsetOfLength()),
                Imm32(1), &create);

  // And create a new BigInt when the input has the sign flag set.
  masm.branchIfBigIntIsNonNegative(input, &done);

  masm.bind(&create);

  // |temp| stores the absolute value, negate it when the sign flag is set.
  Label nonNegative;
  masm.branchIfBigIntIsNonNegative(input, &nonNegative);
  masm.negPtr(temp);
  masm.bind(&nonNegative);

  masm.move32To64ZeroExtend(temp, temp64);
  emitCreateBigInt(ins, Scalar::BigUint64, temp64, output, temp);

  masm.bind(&done);
}

template <size_t NumDefs>
void CodeGenerator::emitIonToWasmCallBase(LIonToWasmCallBase<NumDefs>* lir) {
  wasm::JitCallStackArgVector stackArgs;
  masm.propagateOOM(stackArgs.reserve(lir->numOperands()));
  if (masm.oom()) {
    return;
  }

  const wasm::FuncExport& funcExport = lir->mir()->funcExport();
  const wasm::FuncType& sig = funcExport.funcType();

  WasmABIArgGenerator abi;
  for (size_t i = 0; i < lir->numOperands(); i++) {
    MIRType argMir;
    switch (sig.args()[i].kind()) {
      case wasm::ValType::I32:
      case wasm::ValType::I64:
      case wasm::ValType::F32:
      case wasm::ValType::F64:
        argMir = ToMIRType(sig.args()[i]);
        break;
      case wasm::ValType::Rtt:
      case wasm::ValType::V128:
        MOZ_CRASH("unexpected argument type when calling from ion to wasm");
      case wasm::ValType::Ref:
        switch (sig.args()[i].refTypeKind()) {
          case wasm::RefType::Extern:
            // AnyRef is boxed on the JS side, so passed as a pointer here.
            argMir = ToMIRType(sig.args()[i]);
            break;
          case wasm::RefType::Func:
          case wasm::RefType::Eq:
          case wasm::RefType::TypeIndex:
            MOZ_CRASH("unexpected argument type when calling from ion to wasm");
        }
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
          stackArgs.infallibleEmplaceBack(ToAddress(larg));
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
      case wasm::ValType::Rtt:
      case wasm::ValType::V128:
        MOZ_CRASH("unexpected return type when calling from ion to wasm");
      case wasm::ValType::Ref:
        switch (results[0].refTypeKind()) {
          case wasm::RefType::Func:
          case wasm::RefType::Extern:
          case wasm::RefType::Eq:
            // The wasm stubs layer unboxes anything that needs to be unboxed
            // and leaves it in a Value.  A FuncRef/EqRef we could in principle
            // leave it as a raw object pointer but for now it complicates the
            // API to do so.
            MOZ_ASSERT(lir->mir()->type() == MIRType::Value);
            break;
          case wasm::RefType::TypeIndex:
            MOZ_CRASH("unexpected return type when calling from ion to wasm");
        }
        break;
    }
  }

  bool profilingEnabled = isProfilerInstrumentationEnabled();
  WasmInstanceObject* instObj = lir->mir()->instanceObject();

  Register scratch = ToRegister(lir->temp());

  uint32_t callOffset;
  GenerateDirectCallFromJit(masm, funcExport, instObj->instance(), stackArgs,
                            profilingEnabled, scratch, &callOffset);

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

// Simple codegen for platforms where some values may be stack allocated,
// compare types and instruction result are limited to i32, and the "true" input
// is reused for the output.  See ditto code in Lowering-shared.cpp.
void CodeGenerator::emitWasmCompareAndSelect(LWasmCompareAndSelect* ins) {
  bool cmpIs32bit = ins->compareType() == MCompare::Compare_Int32 ||
                    ins->compareType() == MCompare::Compare_UInt32;
  bool selIs32bit = ins->mir()->type() == MIRType::Int32;

  if (cmpIs32bit && selIs32bit) {
    Register out = ToRegister(ins->output());
    MOZ_ASSERT(ToRegister(ins->ifTrueExpr()) == out,
               "true expr input is reused for output");

    Assembler::Condition cond = Assembler::InvertCondition(
        JSOpToCondition(ins->compareType(), ins->jsop()));
    const LAllocation* rhs = ins->rightExpr();
    const LAllocation* falseExpr = ins->ifFalseExpr();
    Register lhs = ToRegister(ins->leftExpr());

    if (rhs->isRegister()) {
      if (falseExpr->isRegister()) {
        // On arm32, this is the only one of the four cases that can actually
        // happen, since |rhs| and |falseExpr| are marked useAny() by
        // LIRGenerator::visitWasmSelect, and useAny() means "register only"
        // on arm32.
        masm.cmp32Move32(cond, lhs, ToRegister(rhs), ToRegister(falseExpr),
                         out);
      } else {
        masm.cmp32Load32(cond, lhs, ToRegister(rhs), ToAddress(falseExpr), out);
      }
    } else {
      if (falseExpr->isRegister()) {
        masm.cmp32Move32(cond, lhs, ToAddress(rhs), ToRegister(falseExpr), out);
      } else {
        masm.cmp32Load32(cond, lhs, ToAddress(rhs), ToAddress(falseExpr), out);
      }
    }
    return;
  }

  MOZ_CRASH("in CodeGenerator::visitWasmCompareAndSelect: unexpected types");
}

void CodeGenerator::visitWasmFence(LWasmFence* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  masm.memoryBarrier(MembarFull);
}

void CodeGenerator::visitWasmBoxValue(LWasmBoxValue* lir) {
  ValueOperand input = ToValue(lir, LWasmBoxValue::Input);
  Register output = ToRegister(lir->output());

  Label nullValue, objectValue, done;
  {
    ScratchTagScope tag(masm, input);
    masm.splitTagForTest(input, tag);
    masm.branchTestObject(Assembler::Equal, tag, &objectValue);
    masm.branchTestNull(Assembler::Equal, tag, &nullValue);
  }

  using Fn = JSObject* (*)(JSContext*, HandleValue);
  OutOfLineCode* oolBoxValue = oolCallVM<Fn, wasm::BoxBoxableValue>(
      lir, ArgList(input), StoreRegisterTo(output));

  masm.jump(oolBoxValue->entry());

  masm.bind(&nullValue);
  // See the definition of AnyRef for a discussion of pointer representation.
  masm.xorPtr(output, output);
  masm.jump(&done);

  masm.bind(&objectValue);
  // See the definition of AnyRef for a discussion of pointer representation.
  masm.unboxObject(input, output);

  masm.bind(&done);
  masm.bind(oolBoxValue->rejoin());
}

void CodeGenerator::visitWasmAnyRefFromJSObject(LWasmAnyRefFromJSObject* lir) {
  Register input = ToRegister(lir->getOperand(LWasmAnyRefFromJSObject::Input));
  Register output = ToRegister(lir->output());
  // See the definition of AnyRef for a discussion of pointer representation.
  if (input != output) {
    masm.movePtr(input, output);
  }
}

static_assert(!std::is_polymorphic_v<CodeGenerator>,
              "CodeGenerator should not have any virtual methods");

}  // namespace jit
}  // namespace js
