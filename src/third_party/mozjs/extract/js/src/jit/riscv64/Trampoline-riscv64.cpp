/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#ifdef JS_ION_PERF
#  include "jit/PerfSpewer.h"
#endif
#include "jit/riscv64/SharedICRegisters-riscv64.h"
#include "jit/VMFunctions.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

// This file includes stubs for generating the JIT trampolines when there is no
// JIT backend, and also includes implementations for assorted random things
// which can't be implemented in headers.

// All registers to save and restore. This includes the stack pointer, since we
// use the ability to reference register values on the stack by index.
static const LiveRegisterSet AllRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllMask),
                    FloatRegisterSet(FloatRegisters::AllMask));

static void PushBailoutFrame(MacroAssembler& masm, Register spArg) {
  // Push the frameSize_ stored in ra
  // See: CodeGeneratorRiscv64::generateOutOfLineCode()
  masm.push(ra);

  // Push registers such that we can access them from [base + code].
  masm.PushRegsInMask(AllRegs);

  // Put pointer to BailoutStack as first argument to the Bailout()
  masm.movePtr(StackPointer, spArg);
}

struct EnterJITRegs {
  double fs11;
  double fs10;
  double fs9;
  double fs8;
  double fs7;
  double fs6;
  double fs5;
  double fs4;
  double fs3;
  double fs2;
  double fs1;
  double fs0;

  //  uintptr_t align;

  // non-volatile registers.
  uint64_t ra;
  uint64_t sp;
  uint64_t fp;
  uint64_t gp;
  uint64_t s11;
  uint64_t s10;
  uint64_t s9;
  uint64_t s8;
  uint64_t s7;
  uint64_t s6;
  uint64_t s5;
  uint64_t s4;
  uint64_t s3;
  uint64_t s2;
  uint64_t s1;
  // Save reg_vp(a7) on stack, use it after call jit code.
  uint64_t a7;
};

static void GenerateReturn(MacroAssembler& masm, int returnCode) {
  MOZ_ASSERT(masm.framePushed() == sizeof(EnterJITRegs));

  // Restore non-volatile registers
  masm.ld(s1, StackPointer, offsetof(EnterJITRegs, s1));
  masm.ld(s2, StackPointer, offsetof(EnterJITRegs, s2));
  masm.ld(s3, StackPointer, offsetof(EnterJITRegs, s3));
  masm.ld(s4, StackPointer, offsetof(EnterJITRegs, s4));
  masm.ld(s5, StackPointer, offsetof(EnterJITRegs, s5));
  masm.ld(s6, StackPointer, offsetof(EnterJITRegs, s6));
  masm.ld(s7, StackPointer, offsetof(EnterJITRegs, s7));
  masm.ld(s8, StackPointer, offsetof(EnterJITRegs, s8));
  masm.ld(s9, StackPointer, offsetof(EnterJITRegs, s9));
  masm.ld(s10, StackPointer, offsetof(EnterJITRegs, s10));
  masm.ld(s11, StackPointer, offsetof(EnterJITRegs, s11));
  masm.ld(gp, StackPointer, offsetof(EnterJITRegs, gp));
  masm.ld(fp, StackPointer, offsetof(EnterJITRegs, fp));
  masm.ld(sp, StackPointer, offsetof(EnterJITRegs, sp));
  masm.ld(ra, StackPointer, offsetof(EnterJITRegs, ra));

  // Restore non-volatile floating point registers
  masm.fld(fs11, StackPointer, offsetof(EnterJITRegs, fs11));
  masm.fld(fs10, StackPointer, offsetof(EnterJITRegs, fs10));
  masm.fld(fs9, StackPointer, offsetof(EnterJITRegs, fs9));
  masm.fld(fs8, StackPointer, offsetof(EnterJITRegs, fs8));
  masm.fld(fs7, StackPointer, offsetof(EnterJITRegs, fs7));
  masm.fld(fs6, StackPointer, offsetof(EnterJITRegs, fs6));
  masm.fld(fs5, StackPointer, offsetof(EnterJITRegs, fs5));
  masm.fld(fs4, StackPointer, offsetof(EnterJITRegs, fs4));
  masm.fld(fs3, StackPointer, offsetof(EnterJITRegs, fs3));
  masm.fld(fs2, StackPointer, offsetof(EnterJITRegs, fs2));
  masm.fld(fs1, StackPointer, offsetof(EnterJITRegs, fs1));
  masm.fld(fs0, StackPointer, offsetof(EnterJITRegs, fs0));

  masm.freeStack(sizeof(EnterJITRegs));

  masm.branch(ra);
}

static void GeneratePrologue(MacroAssembler& masm) {
  masm.reserveStack(sizeof(EnterJITRegs));

  masm.sd(s1, StackPointer, offsetof(EnterJITRegs, s1));
  masm.sd(s2, StackPointer, offsetof(EnterJITRegs, s2));
  masm.sd(s3, StackPointer, offsetof(EnterJITRegs, s3));
  masm.sd(s4, StackPointer, offsetof(EnterJITRegs, s4));
  masm.sd(s5, StackPointer, offsetof(EnterJITRegs, s5));
  masm.sd(s6, StackPointer, offsetof(EnterJITRegs, s6));
  masm.sd(s7, StackPointer, offsetof(EnterJITRegs, s7));
  masm.sd(s8, StackPointer, offsetof(EnterJITRegs, s8));
  masm.sd(s9, StackPointer, offsetof(EnterJITRegs, s9));
  masm.sd(s10, StackPointer, offsetof(EnterJITRegs, s10));
  masm.sd(s11, StackPointer, offsetof(EnterJITRegs, s11));
  masm.sd(gp, StackPointer, offsetof(EnterJITRegs, gp));
  masm.sd(fp, StackPointer, offsetof(EnterJITRegs, fp));
  masm.sd(sp, StackPointer, offsetof(EnterJITRegs, sp));
  masm.sd(ra, StackPointer, offsetof(EnterJITRegs, ra));
  masm.sd(a7, StackPointer, offsetof(EnterJITRegs, a7));

  masm.fsd(fs11, StackPointer, offsetof(EnterJITRegs, fs11));
  masm.fsd(fs10, StackPointer, offsetof(EnterJITRegs, fs10));
  masm.fsd(fs9, StackPointer, offsetof(EnterJITRegs, fs9));
  masm.fsd(fs8, StackPointer, offsetof(EnterJITRegs, fs8));
  masm.fsd(fs7, StackPointer, offsetof(EnterJITRegs, fs7));
  masm.fsd(fs6, StackPointer, offsetof(EnterJITRegs, fs6));
  masm.fsd(fs5, StackPointer, offsetof(EnterJITRegs, fs5));
  masm.fsd(fs4, StackPointer, offsetof(EnterJITRegs, fs4));
  masm.fsd(fs3, StackPointer, offsetof(EnterJITRegs, fs3));
  masm.fsd(fs2, StackPointer, offsetof(EnterJITRegs, fs2));
  masm.fsd(fs1, StackPointer, offsetof(EnterJITRegs, fs1));
  masm.fsd(fs0, StackPointer, offsetof(EnterJITRegs, fs0));
}

static void GenerateBailoutThunk(MacroAssembler& masm, Label* bailoutTail) {
  PushBailoutFrame(masm, a0);

  // Make space for Bailout's bailoutInfo outparam.
  masm.reserveStack(sizeof(void*));
  masm.movePtr(StackPointer, a1);

  // Call the bailout function.
  using Fn = bool (*)(BailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(a2);
  masm.passABIArg(a0);
  masm.passABIArg(a1);
  masm.callWithABI<Fn, Bailout>(ABIType::General,
                                CheckUnsafeCallWithABI::DontCheckOther);

  // Get the bailoutInfo outparam.
  masm.pop(a2);

  // Remove both the bailout frame and the topmost Ion frame's stack.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in a2.
  masm.jump(bailoutTail);
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
  JitSpew(JitSpew_Codegen, "__Line__: %d", __LINE__);
  {
    Label noNewTarget;
    masm.branchTest32(Assembler::Zero, reg_token,
                      Imm32(CalleeToken_FunctionConstructing), &noNewTarget);

    masm.add32(Imm32(1), reg_argc);

    masm.bind(&noNewTarget);
  }
  JitSpew(JitSpew_Codegen, "__Line__: %d", __LINE__);
  // Make stack algined
  masm.ma_and(s2, reg_argc, Imm32(1));
  masm.ma_sub64(s1, zero, Imm32(sizeof(Value)));
  Label no_zero;
  masm.ma_branch(&no_zero, Assembler::Condition::Equal, s2, Operand(0));
  masm.mv(s1, zero);
  masm.bind(&no_zero);
  masm.ma_add64(StackPointer, StackPointer, s1);

  masm.slli(s2, reg_argc, 3);  // Value* argv
  masm.addPtr(reg_argv, s2);   // s2 = &argv[argc]
  JitSpew(JitSpew_Codegen, "__Line__: %d", __LINE__);
  // Loop over arguments, copying them from an unknown buffer onto the Ion
  // stack so they can be accessed from JIT'ed code.
  Label header, footer;
  // If there aren't any arguments, don't do anything
  masm.ma_b(s2, reg_argv, &footer, Assembler::BelowOrEqual, ShortJump);
  {
    masm.bind(&header);

    masm.subPtr(Imm32(sizeof(Value)), s2);
    masm.subPtr(Imm32(sizeof(Value)), StackPointer);

    ValueOperand value = ValueOperand(s6);
    masm.loadValue(Address(s2, 0), value);
    masm.storeValue(value, Address(StackPointer, 0));

    masm.ma_b(s2, reg_argv, &header, Assembler::Above, ShortJump);
  }
  masm.bind(&footer);
  JitSpew(JitSpew_Codegen, "__Line__: %d", __LINE__);
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
    MOZ_ASSERT(!regs.has(ReturnReg), "ReturnReg matches reg_code");

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
    masm.slli(scratch, numStackValues, 3);
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
    masm.ma_or(R1.scratchReg(), reg_chain, zero);
  }
  JitSpew(JitSpew_Codegen, "__Line__: %d", __LINE__);
  // The call will push the return address and frame pointer on the stack, thus
  // we check that the stack would be aligned once the call is complete.
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
  masm.ld(reg_vp, StackPointer, offsetof(EnterJITRegs, a7));
  masm.storeValue(JSReturnOperand, Address(reg_vp, 0));
  JitSpew(JitSpew_Codegen, "__Line__: %d", __LINE__);
  // Restore non-volatile registers and return.
  GenerateReturn(masm, ShortJump);
}

// static
mozilla::Maybe<::JS::ProfilingFrameIterator::RegisterState>
JitRuntime::getCppEntryRegisters(JitFrameLayout* frameStackAddress) {
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
  masm.ma_and(t2, t2, Imm32(uint32_t(CalleeToken_FunctionConstructing)));

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
  masm.ma_sub64(t1, numArgsReg, s3);
  masm.sub32(Imm32(1), t1);

  // Caller:
  // [arg2] [arg1] [this] [ [argc] [callee] [descr] [raddr] ] <- sp
  // '--- s3 ----'
  //
  // Rectifier frame:
  // [fp'] [undef] [undef] [undef] [arg2] [arg1] [this] [ [argc] [callee]
  //                                                    [descr] [raddr] ]
  //       '-------- t1 ---------' '--- s3 ----'

  // Copy number of actual arguments into numActArgsReg.
  masm.mov(s3, numActArgsReg);

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
  masm.slli(t0, s3, 3);                 // t0 <- nargs * 8
  masm.ma_add64(t1, FramePointer, t0);  // t1 <- fp(saved sp) + nargs * 8
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
  //

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

void JitRuntime::generateBailoutHandler(MacroAssembler& masm,
                                        Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutHandler");

  bailoutHandlerOffset_ = startTrampolineCode(masm);

  GenerateBailoutThunk(masm, bailoutTail);
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
  masm.push(ra);
  masm.PushRegsInMask(save);

  masm.movePtr(ImmPtr(cx->runtime()), a0);

  masm.setupUnalignedABICall(a2);
  masm.passABIArg(a0);
  masm.passABIArg(a1);
  masm.callWithABI(JitPreWriteBarrier(type));

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
        MOZ_CRASH("NYI: riscv callVM should not be used with 128bits values.");
        break;
    }
  }

  // Copy the semi-implicit outparam, if any.
  // It is not a C++-abi outparam, which would get passed in the
  // outparam register, but a real parameter to the function, which
  // was stack-allocated above.
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
      masm.branchTestPtr(Assembler::Zero, a0, a0, masm.failureLabel());
      break;
    case Type_Bool:
      // Called functions return bools, which are 0/false and non-zero/true
      masm.branchIfFalseBool(a0, masm.failureLabel());
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
