/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/mips-shared/SharedICHelpers-mips-shared.h"
#include "jit/PerfSpewer.h"
#include "jit/VMFunctions.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"
#include "vm/Realm.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

// All registers to save and restore. This includes the stack pointer, since we
// use the ability to reference register values on the stack by index.
static const LiveRegisterSet AllRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllMask),
                    FloatRegisterSet(FloatRegisters::AllMask));

static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "Not 32-bit clean.");

struct EnterJITRegs {
  double f31;
  double f30;
  double f29;
  double f28;
  double f27;
  double f26;
  double f25;
  double f24;

  uintptr_t align;

  // non-volatile registers.
  uint64_t ra;
  uint64_t fp;
  uint64_t s7;
  uint64_t s6;
  uint64_t s5;
  uint64_t s4;
  uint64_t s3;
  uint64_t s2;
  uint64_t s1;
  uint64_t s0;
  // Save reg_vp(a7) on stack, use it after call jit code.
  uint64_t a7;
};

static void GenerateReturn(MacroAssembler& masm, int returnCode) {
  MOZ_ASSERT(masm.framePushed() == sizeof(EnterJITRegs));

  if (isLoongson()) {
    // Restore non-volatile registers
    masm.as_ld(s0, StackPointer, offsetof(EnterJITRegs, s0));
    masm.as_gslq(s1, s2, StackPointer, offsetof(EnterJITRegs, s2));
    masm.as_gslq(s3, s4, StackPointer, offsetof(EnterJITRegs, s4));
    masm.as_gslq(s5, s6, StackPointer, offsetof(EnterJITRegs, s6));
    masm.as_gslq(s7, fp, StackPointer, offsetof(EnterJITRegs, fp));
    masm.as_ld(ra, StackPointer, offsetof(EnterJITRegs, ra));

    // Restore non-volatile floating point registers
    masm.as_gslq(f24, f25, StackPointer, offsetof(EnterJITRegs, f25));
    masm.as_gslq(f26, f27, StackPointer, offsetof(EnterJITRegs, f27));
    masm.as_gslq(f28, f29, StackPointer, offsetof(EnterJITRegs, f29));
    masm.as_gslq(f30, f31, StackPointer, offsetof(EnterJITRegs, f31));
  } else {
    // Restore non-volatile registers
    masm.as_ld(s0, StackPointer, offsetof(EnterJITRegs, s0));
    masm.as_ld(s1, StackPointer, offsetof(EnterJITRegs, s1));
    masm.as_ld(s2, StackPointer, offsetof(EnterJITRegs, s2));
    masm.as_ld(s3, StackPointer, offsetof(EnterJITRegs, s3));
    masm.as_ld(s4, StackPointer, offsetof(EnterJITRegs, s4));
    masm.as_ld(s5, StackPointer, offsetof(EnterJITRegs, s5));
    masm.as_ld(s6, StackPointer, offsetof(EnterJITRegs, s6));
    masm.as_ld(s7, StackPointer, offsetof(EnterJITRegs, s7));
    masm.as_ld(fp, StackPointer, offsetof(EnterJITRegs, fp));
    masm.as_ld(ra, StackPointer, offsetof(EnterJITRegs, ra));

    // Restore non-volatile floating point registers
    masm.as_ldc1(f24, StackPointer, offsetof(EnterJITRegs, f24));
    masm.as_ldc1(f25, StackPointer, offsetof(EnterJITRegs, f25));
    masm.as_ldc1(f26, StackPointer, offsetof(EnterJITRegs, f26));
    masm.as_ldc1(f27, StackPointer, offsetof(EnterJITRegs, f27));
    masm.as_ldc1(f28, StackPointer, offsetof(EnterJITRegs, f28));
    masm.as_ldc1(f29, StackPointer, offsetof(EnterJITRegs, f29));
    masm.as_ldc1(f30, StackPointer, offsetof(EnterJITRegs, f30));
    masm.as_ldc1(f31, StackPointer, offsetof(EnterJITRegs, f31));
  }

  masm.freeStack(sizeof(EnterJITRegs));

  masm.branch(ra);
}

static void GeneratePrologue(MacroAssembler& masm) {
  masm.reserveStack(sizeof(EnterJITRegs));

  if (isLoongson()) {
    masm.as_gssq(a7, s0, StackPointer, offsetof(EnterJITRegs, s0));
    masm.as_gssq(s1, s2, StackPointer, offsetof(EnterJITRegs, s2));
    masm.as_gssq(s3, s4, StackPointer, offsetof(EnterJITRegs, s4));
    masm.as_gssq(s5, s6, StackPointer, offsetof(EnterJITRegs, s6));
    masm.as_gssq(s7, fp, StackPointer, offsetof(EnterJITRegs, fp));
    masm.as_sd(ra, StackPointer, offsetof(EnterJITRegs, ra));

    masm.as_gssq(f24, f25, StackPointer, offsetof(EnterJITRegs, f25));
    masm.as_gssq(f26, f27, StackPointer, offsetof(EnterJITRegs, f27));
    masm.as_gssq(f28, f29, StackPointer, offsetof(EnterJITRegs, f29));
    masm.as_gssq(f30, f31, StackPointer, offsetof(EnterJITRegs, f31));
    return;
  }

  masm.as_sd(s0, StackPointer, offsetof(EnterJITRegs, s0));
  masm.as_sd(s1, StackPointer, offsetof(EnterJITRegs, s1));
  masm.as_sd(s2, StackPointer, offsetof(EnterJITRegs, s2));
  masm.as_sd(s3, StackPointer, offsetof(EnterJITRegs, s3));
  masm.as_sd(s4, StackPointer, offsetof(EnterJITRegs, s4));
  masm.as_sd(s5, StackPointer, offsetof(EnterJITRegs, s5));
  masm.as_sd(s6, StackPointer, offsetof(EnterJITRegs, s6));
  masm.as_sd(s7, StackPointer, offsetof(EnterJITRegs, s7));
  masm.as_sd(fp, StackPointer, offsetof(EnterJITRegs, fp));
  masm.as_sd(ra, StackPointer, offsetof(EnterJITRegs, ra));
  masm.as_sd(a7, StackPointer, offsetof(EnterJITRegs, a7));

  masm.as_sdc1(f24, StackPointer, offsetof(EnterJITRegs, f24));
  masm.as_sdc1(f25, StackPointer, offsetof(EnterJITRegs, f25));
  masm.as_sdc1(f26, StackPointer, offsetof(EnterJITRegs, f26));
  masm.as_sdc1(f27, StackPointer, offsetof(EnterJITRegs, f27));
  masm.as_sdc1(f28, StackPointer, offsetof(EnterJITRegs, f28));
  masm.as_sdc1(f29, StackPointer, offsetof(EnterJITRegs, f29));
  masm.as_sdc1(f30, StackPointer, offsetof(EnterJITRegs, f30));
  masm.as_sdc1(f31, StackPointer, offsetof(EnterJITRegs, f31));
}

// Generates a trampoline for calling Jit compiled code from a C++ function.
// The trampoline use the EnterJitCode signature, with the standard x64 fastcall
// calling convention.
void JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateEnterJIT");

  enterJITOffset_ = startTrampolineCode(masm);

  const Register reg_code = IntArgReg0;
  const Register reg_argc = IntArgReg1;
  const Register reg_argv = IntArgReg2;
  const mozilla::DebugOnly<Register> reg_frame = IntArgReg3;
  const Register reg_token = IntArgReg4;
  const Register reg_chain = IntArgReg5;
  const Register reg_values = IntArgReg6;
  const Register reg_vp = IntArgReg7;

  MOZ_ASSERT(OsrFrameReg == reg_frame);

  GeneratePrologue(masm);

  // Save stack pointer as baseline frame.
  masm.movePtr(StackPointer, FramePointer);

  // Load the number of actual arguments into s3.
  masm.unboxInt32(Address(reg_vp, 0), s3);

  /***************************************************************
  Loop over argv vector, push arguments onto stack in reverse order
  ***************************************************************/

  // if we are constructing, that also needs to include newTarget
  {
    Label noNewTarget;
    masm.branchTest32(Assembler::Zero, reg_token,
                      Imm32(CalleeToken_FunctionConstructing), &noNewTarget);

    masm.add32(Imm32(1), reg_argc);

    masm.bind(&noNewTarget);
  }

  // Make stack algined
  masm.ma_and(s0, reg_argc, Imm32(1));
  masm.ma_dsubu(s1, StackPointer, Imm32(sizeof(Value)));
#ifdef MIPSR6
  masm.as_selnez(s1, s1, s0);
  masm.as_seleqz(StackPointer, StackPointer, s0);
  masm.as_or(StackPointer, StackPointer, s1);
#else
  masm.as_movn(StackPointer, s1, s0);
#endif

  masm.as_dsll(s0, reg_argc, 3);  // Value* argv
  masm.addPtr(reg_argv, s0);      // s0 = &argv[argc]

  // Loop over arguments, copying them from an unknown buffer onto the Ion
  // stack so they can be accessed from JIT'ed code.
  Label header, footer;
  // If there aren't any arguments, don't do anything
  masm.ma_b(s0, reg_argv, &footer, Assembler::BelowOrEqual, ShortJump);
  {
    masm.bind(&header);

    masm.subPtr(Imm32(sizeof(Value)), s0);
    masm.subPtr(Imm32(sizeof(Value)), StackPointer);

    ValueOperand value = ValueOperand(s6);
    masm.loadValue(Address(s0, 0), value);
    masm.storeValue(value, Address(StackPointer, 0));

    masm.ma_b(s0, reg_argv, &header, Assembler::Above, ShortJump);
  }
  masm.bind(&footer);

  masm.push(reg_token);
  masm.pushFrameDescriptorForJitCall(FrameType::CppToJSJit, s3, s3);

  CodeLabel returnLabel;
  Label oomReturnLabel;
  {
    // Handle Interpreter -> Baseline OSR.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT(!regs.has(FramePointer));
    regs.take(OsrFrameReg);
    regs.take(reg_code);

    Label notOsr;
    masm.ma_b(OsrFrameReg, OsrFrameReg, &notOsr, Assembler::Zero, ShortJump);

    Register numStackValues = reg_values;
    regs.take(numStackValues);
    Register scratch = regs.takeAny();

    // Push return address.
    masm.subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
    masm.ma_li(scratch, &returnLabel);
    masm.storePtr(scratch, Address(StackPointer, 0));

    // Push previous frame pointer.
    masm.subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
    masm.storePtr(FramePointer, Address(StackPointer, 0));

    // Reserve frame.
    Register framePtr = FramePointer;
    masm.movePtr(StackPointer, framePtr);
    masm.subPtr(Imm32(BaselineFrame::Size()), StackPointer);

    Register framePtrScratch = regs.takeAny();
    masm.movePtr(sp, framePtrScratch);

    // Reserve space for locals and stack values.
    masm.ma_dsll(scratch, numStackValues, Imm32(3));
    masm.subPtr(scratch, StackPointer);

    // Enter exit frame.
    masm.reserveStack(3 * sizeof(uintptr_t));
    masm.storePtr(
        ImmWord(MakeFrameDescriptor(FrameType::BaselineJS)),
        Address(StackPointer, 2 * sizeof(uintptr_t)));  // Frame descriptor
    masm.storePtr(
        zero, Address(StackPointer, sizeof(uintptr_t)));  // fake return address
    masm.storePtr(FramePointer, Address(StackPointer, 0));

    // No GC things to mark, push a bare token.
    masm.loadJSContext(scratch);
    masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    masm.reserveStack(2 * sizeof(uintptr_t));
    masm.storePtr(framePtr,
                  Address(StackPointer, sizeof(uintptr_t)));  // BaselineFrame
    masm.storePtr(reg_code, Address(StackPointer, 0));        // jitcode

    using Fn = bool (*)(BaselineFrame* frame, InterpreterFrame* interpFrame,
                        uint32_t numStackValues);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(framePtrScratch);  // BaselineFrame
    masm.passABIArg(OsrFrameReg);      // InterpreterFrame
    masm.passABIArg(numStackValues);
    masm.callWithABI<Fn, jit::InitBaselineFrameForOsr>(
        ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    regs.add(OsrFrameReg);
    Register jitcode = regs.takeAny();
    masm.loadPtr(Address(StackPointer, 0), jitcode);
    masm.loadPtr(Address(StackPointer, sizeof(uintptr_t)), framePtr);
    masm.freeStack(2 * sizeof(uintptr_t));

    Label error;
    masm.freeStack(ExitFrameLayout::SizeWithFooter());
    masm.branchIfFalseBool(ReturnReg, &error);

    // If OSR-ing, then emit instrumentation for setting lastProfilerFrame
    // if profiler instrumentation is enabled.
    {
      Label skipProfilingInstrumentation;
      AbsoluteAddress addressOfEnabled(
          cx->runtime()->geckoProfiler().addressOfEnabled());
      masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                    &skipProfilingInstrumentation);
      masm.profilerEnterFrame(framePtr, scratch);
      masm.bind(&skipProfilingInstrumentation);
    }

    masm.jump(jitcode);

    // OOM: load error value, discard return address and previous frame
    // pointer and return.
    masm.bind(&error);
    masm.movePtr(framePtr, StackPointer);
    masm.addPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    masm.jump(&oomReturnLabel);

    masm.bind(&notOsr);
    // Load the scope chain in R1.
    MOZ_ASSERT(R1.scratchReg() != reg_code);
    masm.ma_move(R1.scratchReg(), reg_chain);
  }

  // The call will push the return address on the stack, thus we check that
  // the stack would be aligned once the call is complete.
  masm.assertStackAlignment(JitStackAlignment, 2 * sizeof(uintptr_t));

  // Call the function with pushing return address to stack.
  masm.callJitNoProfiler(reg_code);

  {
    // Interpreter -> Baseline OSR will return here.
    masm.bind(&returnLabel);
    masm.addCodeLabel(returnLabel);
    masm.bind(&oomReturnLabel);
  }

  // Discard arguments and padding. Set sp to the address of the EnterJITRegs
  // on the stack.
  masm.mov(FramePointer, StackPointer);

  // Store the returned value into the vp
  masm.as_ld(reg_vp, StackPointer, offsetof(EnterJITRegs, a7));
  masm.storeValue(JSReturnOperand, Address(reg_vp, 0));

  // Restore non-volatile registers and return.
  GenerateReturn(masm, ShortJump);
}

// static
mozilla::Maybe<::JS::ProfilingFrameIterator::RegisterState>
JitRuntime::getCppEntryRegisters(JitFrameLayout* frameStackAddress) {
  // Not supported, or not implemented yet.
  // TODO: Implement along with the corresponding stack-walker changes, in
  // coordination with the Gecko Profiler, see bug 1635987 and follow-ups.
  return mozilla::Nothing{};
}

void JitRuntime::generateInvalidator(MacroAssembler& masm, Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateInvalidator");

  invalidatorOffset_ = startTrampolineCode(masm);

  // Stack has to be alligned here. If not, we will have to fix it.
  masm.checkStackAlignment();

  // Push registers such that we can access them from [base + code].
  masm.PushRegsInMask(AllRegs);

  // Pass pointer to InvalidationBailoutStack structure.
  masm.movePtr(StackPointer, a0);

  // Reserve place for BailoutInfo pointer. Two words to ensure alignment for
  // setupAlignedABICall.
  masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
  // Pass pointer to BailoutInfo
  masm.movePtr(StackPointer, a1);

  using Fn = bool (*)(InvalidationBailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupAlignedABICall();
  masm.passABIArg(a0);
  masm.passABIArg(a1);
  masm.callWithABI<Fn, InvalidationBailout>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(a2);

  // Pop the machine state and the dead frame.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in r2.
  masm.jump(bailoutTail);
}

void JitRuntime::generateArgumentsRectifier(MacroAssembler& masm,
                                            ArgumentsRectifierKind kind) {
  // Do not erase the frame pointer in this function.

  AutoCreatedBy acb(masm, "JitRuntime::generateArgumentsRectifier");

  switch (kind) {
    case ArgumentsRectifierKind::Normal:
      argumentsRectifierOffset_ = startTrampolineCode(masm);
      break;
    case ArgumentsRectifierKind::TrialInlining:
      trialInliningArgumentsRectifierOffset_ = startTrampolineCode(masm);
      break;
  }
  masm.pushReturnAddress();

  // Caller:
  // [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]] <- sp

  // Frame prologue.
  //
  // NOTE: if this changes, fix the Baseline bailout code too!
  // See BaselineStackBuilder::calculatePrevFramePtr and
  // BaselineStackBuilder::buildRectifierFrame (in BaselineBailouts.cpp).
  masm.push(FramePointer);
  masm.mov(StackPointer, FramePointer);

  // Load argc.
  masm.loadNumActualArgs(FramePointer, s3);

  Register numActArgsReg = a6;
  Register calleeTokenReg = a7;
  Register numArgsReg = a5;

  // Load |nformals| into numArgsReg.
  masm.loadPtr(
      Address(FramePointer, RectifierFrameLayout::offsetOfCalleeToken()),
      calleeTokenReg);
  masm.mov(calleeTokenReg, numArgsReg);
  masm.andPtr(Imm32(uint32_t(CalleeTokenMask)), numArgsReg);
  masm.loadFunctionArgCount(numArgsReg, numArgsReg);

  // Stash another copy in t3, since we are going to do destructive operations
  // on numArgsReg
  masm.mov(numArgsReg, t3);

  static_assert(
      CalleeToken_FunctionConstructing == 1,
      "Ensure that we can use the constructing bit to count the value");
  masm.mov(calleeTokenReg, t2);
  masm.ma_and(t2, Imm32(uint32_t(CalleeToken_FunctionConstructing)));

  // Including |this|, and |new.target|, there are (|nformals| + 1 +
  // isConstructing) arguments to push to the stack.  Then we push a
  // JitFrameLayout.  We compute the padding expressed in the number of extra
  // |undefined| values to push on the stack.
  static_assert(
      sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");
  static_assert(
      JitStackAlignment % sizeof(Value) == 0,
      "Ensure that we can pad the stack by pushing extra UndefinedValue");

  MOZ_ASSERT(mozilla::IsPowerOfTwo(JitStackValueAlignment));
  masm.add32(
      Imm32(JitStackValueAlignment - 1 /* for padding */ + 1 /* for |this| */),
      numArgsReg);
  masm.add32(t2, numArgsReg);
  masm.and32(Imm32(~(JitStackValueAlignment - 1)), numArgsReg);

  // Load the number of |undefined|s to push into t1. Subtract 1 for |this|.
  masm.as_dsubu(t1, numArgsReg, s3);
  masm.sub32(Imm32(1), t1);

  // Caller:
  // [arg2] [arg1] [this] [ [argc] [callee] [descr] [raddr] ] <- sp
  // '--- s3 ----'
  //
  // Rectifier frame:
  // [fp'][undef] [undef] [undef] [arg2] [arg1] [this] [ [argc] [callee]
  //                                                   [descr] [raddr] ]
  //      '-------- t1 ---------' '--- s3 ----'

  // Copy number of actual arguments into numActArgsReg
  masm.mov(s3, numActArgsReg);  // Save %sp.

  masm.moveValue(UndefinedValue(), ValueOperand(t0));

  // Push undefined. (including the padding)
  {
    Label undefLoopTop;

    masm.bind(&undefLoopTop);
    masm.sub32(Imm32(1), t1);
    masm.subPtr(Imm32(sizeof(Value)), StackPointer);
    masm.storeValue(ValueOperand(t0), Address(StackPointer, 0));

    masm.ma_b(t1, t1, &undefLoopTop, Assembler::NonZero, ShortJump);
  }

  // Get the topmost argument.
  static_assert(sizeof(Value) == 8, "TimesEight is used to skip arguments");

  // Get the topmost argument.
  masm.ma_dsll(t0, s3, Imm32(3));       // t0 <- nargs * 8
  masm.as_daddu(t1, FramePointer, t0);  // t1 <- fp(saved sp) + nargs * 8
  masm.addPtr(Imm32(sizeof(RectifierFrameLayout)), t1);

  // Push arguments, |nargs| + 1 times (to include |this|).

  masm.addPtr(Imm32(1), s3);
  {
    Label copyLoopTop;

    masm.bind(&copyLoopTop);
    masm.sub32(Imm32(1), s3);
    masm.subPtr(Imm32(sizeof(Value)), StackPointer);
    masm.loadValue(Address(t1, 0), ValueOperand(t0));
    masm.storeValue(ValueOperand(t0), Address(StackPointer, 0));
    masm.subPtr(Imm32(sizeof(Value)), t1);

    masm.ma_b(s3, s3, &copyLoopTop, Assembler::NonZero, ShortJump);
  }

  // if constructing, copy newTarget
  {
    Label notConstructing;

    masm.branchTest32(Assembler::Zero, calleeTokenReg,
                      Imm32(CalleeToken_FunctionConstructing),
                      &notConstructing);

    // thisFrame[numFormals] = prevFrame[argc]
    ValueOperand newTarget(t0);

    // Load vp[argc]. Add sizeof(Value) for |this|.
    BaseIndex newTargetSrc(FramePointer, numActArgsReg, TimesEight,
                           sizeof(RectifierFrameLayout) + sizeof(Value));
    masm.loadValue(newTargetSrc, newTarget);

    // Again, 1 for |this|
    BaseIndex newTargetDest(StackPointer, t3, TimesEight, sizeof(Value));
    masm.storeValue(newTarget, newTargetDest);

    masm.bind(&notConstructing);
  }

  // Caller:
  // [arg2] [arg1] [this] [ [argc] [callee] [descr] [raddr] ]
  //
  //
  // Rectifier frame:
  // [fp'] <- fp [undef] [undef] [undef] [arg2] [arg1] [this] <- sp [ [argc]
  //                                              [callee] [descr] [raddr] ]

  // Construct JitFrameLayout.
  masm.push(calleeTokenReg);
  masm.pushFrameDescriptorForJitCall(FrameType::Rectifier, numActArgsReg,
                                     numActArgsReg);

  // Call the target function.
  masm.andPtr(Imm32(uint32_t(CalleeTokenMask)), calleeTokenReg);
  switch (kind) {
    case ArgumentsRectifierKind::Normal:
      masm.loadJitCodeRaw(calleeTokenReg, t1);
      argumentsRectifierReturnOffset_ = masm.callJitNoProfiler(t1);
      break;
    case ArgumentsRectifierKind::TrialInlining:
      Label noBaselineScript, done;
      masm.loadBaselineJitCodeRaw(calleeTokenReg, t1, &noBaselineScript);
      masm.callJitNoProfiler(t1);
      masm.jump(&done);

      // See BaselineCacheIRCompiler::emitCallInlinedFunction.
      masm.bind(&noBaselineScript);
      masm.loadJitCodeRaw(calleeTokenReg, t1);
      masm.callJitNoProfiler(t1);
      masm.bind(&done);
      break;
  }

  masm.mov(FramePointer, StackPointer);
  masm.pop(FramePointer);
  masm.ret();
}

/* - When bailout is done via out of line code (lazy bailout).
 * Frame size is stored in $ra (look at
 * CodeGeneratorMIPS64::generateOutOfLineCode()) and thunk code should save it
 * on stack. Other difference is that members snapshotOffset_ and padding_ are
 * pushed to the stack by CodeGeneratorMIPS64::visitOutOfLineBailout().
 */
static void PushBailoutFrame(MacroAssembler& masm, Register spArg) {
  // Push the frameSize_ stored in ra
  // See: CodeGeneratorMIPS64::generateOutOfLineCode()
  masm.push(ra);

  // Push registers such that we can access them from [base + code].
  masm.PushRegsInMask(AllRegs);

  // Put pointer to BailoutStack as first argument to the Bailout()
  masm.movePtr(StackPointer, spArg);
}

static void GenerateBailoutThunk(MacroAssembler& masm, Label* bailoutTail) {
  PushBailoutFrame(masm, a0);

  // Put pointer to BailoutInfo
  static const uint32_t sizeOfBailoutInfo = sizeof(uintptr_t) * 2;
  masm.subPtr(Imm32(sizeOfBailoutInfo), StackPointer);
  masm.movePtr(StackPointer, a1);

  using Fn = bool (*)(BailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupAlignedABICall();
  masm.passABIArg(a0);
  masm.passABIArg(a1);
  masm.callWithABI<Fn, Bailout>(ABIType::General,
                                CheckUnsafeCallWithABI::DontCheckOther);

  // Get BailoutInfo pointer
  masm.loadPtr(Address(StackPointer, 0), a2);

  // Remove both the bailout frame and the topmost Ion frame's stack.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in a2.
  masm.jump(bailoutTail);
}

void JitRuntime::generateBailoutHandler(MacroAssembler& masm,
                                        Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutHandler");

  bailoutHandlerOffset_ = startTrampolineCode(masm);

  GenerateBailoutThunk(masm, bailoutTail);
}

bool JitRuntime::generateVMWrapper(JSContext* cx, MacroAssembler& masm,
                                   VMFunctionId id, const VMFunctionData& f,
                                   DynFn nativeFun, uint32_t* wrapperOffset) {
  AutoCreatedBy acb(masm, "JitRuntime::generateVMWrapper");

  *wrapperOffset = startTrampolineCode(masm);

  // Avoid conflicts with argument registers while discarding the result after
  // the function call.
  AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

  static_assert(
      (Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
      "Wrapper register set should be a superset of Volatile register set.");

  // The context is the first argument; a0 is the first argument register.
  Register cxreg = a0;
  regs.take(cxreg);

  // On link-register platforms, it is the responsibility of the VM *callee* to
  // push the return address, while the caller must ensure that the address
  // is stored in ra on entry. This allows the VM wrapper to work with both
  // direct calls and tail calls.
  masm.pushReturnAddress();

  // Push the frame pointer to finish the exit frame, then link it up.
  masm.Push(FramePointer);
  masm.moveStackPtrTo(FramePointer);
  masm.loadJSContext(cxreg);
  masm.enterExitFrame(cxreg, regs.getAny(), id);

  // Reserve space for the outparameter.
  masm.reserveVMFunctionOutParamSpace(f);
  masm.setupUnalignedABICallDontSaveRestoreSP();
  masm.passABIArg(cxreg);

  size_t argDisp = ExitFrameLayout::Size();

  // Copy any arguments.
  for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
    switch (f.argProperties(explicitArg)) {
      case VMFunctionData::WordByValue:
        if (f.argPassedInFloatReg(explicitArg)) {
          masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::Float64);
        } else {
          masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::General);
        }
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::WordByRef:
        masm.passABIArg(MoveOperand(FramePointer, argDisp,
                                    MoveOperand::Kind::EffectiveAddress),
                        ABIType::General);
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::DoubleByValue:
      case VMFunctionData::DoubleByRef:
        MOZ_CRASH("NYI: MIPS64 callVM should not be used with 128bits values.");
        break;
    }
  }

  // Copy the implicit outparam, if any.
  const int32_t outParamOffset =
      -int32_t(ExitFooterFrame::Size()) - f.sizeOfOutParamStackSlot();
  if (f.outParam != Type_Void) {
    masm.passABIArg(MoveOperand(FramePointer, outParamOffset,
                                MoveOperand::Kind::EffectiveAddress),
                    ABIType::General);
  }

  masm.callWithABI(nativeFun, ABIType::General,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  // Test for failure.
  switch (f.failType()) {
    case Type_Cell:
      masm.branchTestPtr(Assembler::Zero, v0, v0, masm.failureLabel());
      break;
    case Type_Bool:
      // Called functions return bools, which are 0/false and non-zero/true
      masm.branchIfFalseBool(v0, masm.failureLabel());
      break;
    case Type_Void:
      break;
    default:
      MOZ_CRASH("unknown failure kind");
  }

  // Load the outparam.
  masm.loadVMFunctionOutParam(f, Address(FramePointer, outParamOffset));

  // Pop frame and restore frame pointer.
  masm.moveToStackPtr(FramePointer);
  masm.pop(FramePointer);

  // Return. Subtract sizeof(void*) for the frame pointer.
  masm.retn(Imm32(sizeof(ExitFrameLayout) - sizeof(void*) +
                  f.explicitStackSlots() * sizeof(void*) +
                  f.extraValuesToPop * sizeof(Value)));

  return true;
}

uint32_t JitRuntime::generatePreBarrier(JSContext* cx, MacroAssembler& masm,
                                        MIRType type) {
  AutoCreatedBy acb(masm, "JitRuntime::generatePreBarrier");

  uint32_t offset = startTrampolineCode(masm);

  MOZ_ASSERT(PreBarrierReg == a1);
  Register temp1 = a0;
  Register temp2 = a2;
  Register temp3 = a3;
  masm.push(temp1);
  masm.push(temp2);
  masm.push(temp3);

  Label noBarrier;
  masm.emitPreBarrierFastPath(cx->runtime(), type, temp1, temp2, temp3,
                              &noBarrier);

  // Call into C++ to mark this GC thing.
  masm.pop(temp3);
  masm.pop(temp2);
  masm.pop(temp1);

  LiveRegisterSet save;
  save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                           FloatRegisterSet(FloatRegisters::VolatileMask));
  save.add(ra);
  masm.PushRegsInMask(save);

  masm.movePtr(ImmPtr(cx->runtime()), a0);

  masm.setupUnalignedABICall(a2);
  masm.passABIArg(a0);
  masm.passABIArg(a1);
  masm.callWithABI(JitPreWriteBarrier(type));

  save.take(AnyRegister(ra));
  masm.PopRegsInMask(save);
  masm.ret();

  masm.bind(&noBarrier);
  masm.pop(temp3);
  masm.pop(temp2);
  masm.pop(temp1);
  masm.abiret();

  return offset;
}

void JitRuntime::generateBailoutTailStub(MacroAssembler& masm,
                                         Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutTailStub");

  masm.bind(bailoutTail);
  masm.generateBailoutTail(a1, a2);
}
