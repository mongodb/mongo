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
#include "jit/PerfSpewer.h"
#include "jit/VMFunctions.h"
#include "jit/x64/SharedICRegisters-x64.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::IsPowerOfTwo;

// This struct reflects the contents of the stack entry.
// Given a `CommonFrameLayout* frame`:
// - `frame->prevType()` should be `FrameType::CppToJSJit`.
// - Then EnterJITStackEntry starts at:
//     frame->callerFramePtr() + EnterJITStackEntry::offsetFromFP()
//     (the offset is negative, so this subtracts from the frame pointer)
struct EnterJITStackEntry {
  // Offset from frame pointer to EnterJITStackEntry*.
  static constexpr int32_t offsetFromFP() {
    return -int32_t(offsetof(EnterJITStackEntry, rbp));
  }

  void* result;

#if defined(_WIN64)
  struct XMM {
    using XMM128 = char[16];
    XMM128 xmm6;
    XMM128 xmm7;
    XMM128 xmm8;
    XMM128 xmm9;
    XMM128 xmm10;
    XMM128 xmm11;
    XMM128 xmm12;
    XMM128 xmm13;
    XMM128 xmm14;
    XMM128 xmm15;
  } xmm;

  // 16-byte aligment for xmm registers above.
  uint64_t xmmPadding;

  void* rsi;
  void* rdi;
#endif

  void* r15;
  void* r14;
  void* r13;
  void* r12;
  void* rbx;
  void* rbp;

  // Pushed by CALL.
  void* rip;
};

// All registers to save and restore. This includes the stack pointer, since we
// use the ability to reference register values on the stack by index.
static const LiveRegisterSet AllRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllMask),
                    FloatRegisterSet(FloatRegisters::AllMask));

// Generates a trampoline for calling Jit compiled code from a C++ function.
// The trampoline use the EnterJitCode signature, with the standard x64 fastcall
// calling convention.
void JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateEnterJIT");

  enterJITOffset_ = startTrampolineCode(masm);

  masm.assertStackAlignment(ABIStackAlignment,
                            -int32_t(sizeof(uintptr_t)) /* return address */);

  const Register reg_code = IntArgReg0;
  const Register reg_argc = IntArgReg1;
  const Register reg_argv = IntArgReg2;
  static_assert(OsrFrameReg == IntArgReg3);

#if defined(_WIN64)
  const Address token = Address(rbp, 16 + ShadowStackSpace);
  const Operand scopeChain = Operand(rbp, 24 + ShadowStackSpace);
  const Operand numStackValuesAddr = Operand(rbp, 32 + ShadowStackSpace);
  const Operand result = Operand(rbp, 40 + ShadowStackSpace);
#else
  const Register token = IntArgReg4;
  const Register scopeChain = IntArgReg5;
  const Operand numStackValuesAddr = Operand(rbp, 16 + ShadowStackSpace);
  const Operand result = Operand(rbp, 24 + ShadowStackSpace);
#endif

  // Note: the stack pushes below must match the fields in EnterJITStackEntry.

  // Save old stack frame pointer, set new stack frame pointer.
  masm.push(rbp);
  masm.mov(rsp, rbp);

  // Save non-volatile registers. These must be saved by the trampoline, rather
  // than by the JIT'd code, because they are scanned by the conservative
  // scanner.
  masm.push(rbx);
  masm.push(r12);
  masm.push(r13);
  masm.push(r14);
  masm.push(r15);
#if defined(_WIN64)
  masm.push(rdi);
  masm.push(rsi);

  // 16-byte aligment for vmovdqa
  masm.subq(Imm32(sizeof(EnterJITStackEntry::XMM) + 8), rsp);

  masm.vmovdqa(xmm6, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm6)));
  masm.vmovdqa(xmm7, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm7)));
  masm.vmovdqa(xmm8, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm8)));
  masm.vmovdqa(xmm9, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm9)));
  masm.vmovdqa(xmm10, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm10)));
  masm.vmovdqa(xmm11, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm11)));
  masm.vmovdqa(xmm12, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm12)));
  masm.vmovdqa(xmm13, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm13)));
  masm.vmovdqa(xmm14, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm14)));
  masm.vmovdqa(xmm15, Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm15)));
#endif

  // Save arguments passed in registers needed after function call.
  masm.push(result);

  // End of pushes reflected in EnterJITStackEntry, i.e. EnterJITStackEntry
  // starts at this rsp.

  // Remember number of bytes occupied by argument vector
  masm.mov(reg_argc, r13);

  // if we are constructing, that also needs to include newTarget
  {
    Label noNewTarget;
    masm.branchTest32(Assembler::Zero, token,
                      Imm32(CalleeToken_FunctionConstructing), &noNewTarget);

    masm.addq(Imm32(1), r13);

    masm.bind(&noNewTarget);
  }

  masm.shll(Imm32(3), r13);  // r13 = argc * sizeof(Value)
  static_assert(sizeof(Value) == 1 << 3, "Constant is baked in assembly code");

  // Guarantee stack alignment of Jit frames.
  //
  // This code compensates for the offset created by the copy of the vector of
  // arguments, such that the jit frame will be aligned once the return
  // address is pushed on the stack.
  //
  // In the computation of the offset, we omit the size of the JitFrameLayout
  // which is pushed on the stack, as the JitFrameLayout size is a multiple of
  // the JitStackAlignment.
  masm.mov(rsp, r12);
  masm.subq(r13, r12);
  static_assert(
      sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");
  masm.andl(Imm32(JitStackAlignment - 1), r12);
  masm.subq(r12, rsp);

  /***************************************************************
  Loop over argv vector, push arguments onto stack in reverse order
  ***************************************************************/

  // r13 still stores the number of bytes in the argument vector.
  masm.addq(reg_argv, r13);  // r13 points above last argument or newTarget

  // while r13 > rdx, push arguments.
  {
    Label header, footer;
    masm.bind(&header);

    masm.cmpPtr(r13, reg_argv);
    masm.j(AssemblerX86Shared::BelowOrEqual, &footer);

    masm.subq(Imm32(8), r13);
    masm.push(Operand(r13, 0));
    masm.jmp(&header);

    masm.bind(&footer);
  }

  // Load the number of actual arguments.  |result| is used to store the
  // actual number of arguments without adding an extra argument to the enter
  // JIT.
  masm.movq(result, reg_argc);
  masm.unboxInt32(Operand(reg_argc, 0), reg_argc);

  // Push the callee token.
  masm.push(token);

  // Push the descriptor.
  masm.pushFrameDescriptorForJitCall(FrameType::CppToJSJit, reg_argc, reg_argc);

  CodeLabel returnLabel;
  Label oomReturnLabel;
  {
    // Handle Interpreter -> Baseline OSR.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT(!regs.has(rbp));
    regs.take(OsrFrameReg);
    regs.take(reg_code);

    Register scratch = regs.takeAny();

    Label notOsr;
    masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

    Register numStackValues = regs.takeAny();
    masm.movq(numStackValuesAddr, numStackValues);

    // Push return address
    masm.mov(&returnLabel, scratch);
    masm.push(scratch);

    // Frame prologue.
    masm.push(rbp);
    masm.mov(rsp, rbp);

    // Reserve frame.
    masm.subPtr(Imm32(BaselineFrame::Size()), rsp);

    Register framePtrScratch = regs.takeAny();
    masm.touchFrameValues(numStackValues, scratch, framePtrScratch);
    masm.mov(rsp, framePtrScratch);

    // Reserve space for locals and stack values.
    Register valuesSize = regs.takeAny();
    masm.mov(numStackValues, valuesSize);
    masm.shll(Imm32(3), valuesSize);
    masm.subPtr(valuesSize, rsp);

    // Enter exit frame.
    masm.pushFrameDescriptor(FrameType::BaselineJS);
    masm.push(Imm32(0));  // Fake return address.
    masm.push(FramePointer);
    // No GC things to mark, push a bare token.
    masm.loadJSContext(scratch);
    masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    regs.add(valuesSize);

    masm.push(reg_code);

    using Fn = bool (*)(BaselineFrame* frame, InterpreterFrame* interpFrame,
                        uint32_t numStackValues);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(framePtrScratch);  // BaselineFrame
    masm.passABIArg(OsrFrameReg);      // InterpreterFrame
    masm.passABIArg(numStackValues);
    masm.callWithABI<Fn, jit::InitBaselineFrameForOsr>(
        ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    masm.pop(reg_code);

    MOZ_ASSERT(reg_code != ReturnReg);

    Label error;
    masm.addPtr(Imm32(ExitFrameLayout::SizeWithFooter()), rsp);
    masm.branchIfFalseBool(ReturnReg, &error);

    // If OSR-ing, then emit instrumentation for setting lastProfilerFrame
    // if profiler instrumentation is enabled.
    {
      Label skipProfilingInstrumentation;
      AbsoluteAddress addressOfEnabled(
          cx->runtime()->geckoProfiler().addressOfEnabled());
      masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                    &skipProfilingInstrumentation);
      masm.profilerEnterFrame(rbp, scratch);
      masm.bind(&skipProfilingInstrumentation);
    }

    masm.jump(reg_code);

    // OOM: frame epilogue, load error value, discard return address and return.
    masm.bind(&error);
    masm.mov(rbp, rsp);
    masm.pop(rbp);
    masm.addPtr(Imm32(sizeof(uintptr_t)), rsp);  // Return address.
    masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    masm.jump(&oomReturnLabel);

    masm.bind(&notOsr);
    masm.movq(scopeChain, R1.scratchReg());
  }

  // The call will push the return address and frame pointer on the stack, thus
  // we check that the stack would be aligned once the call is complete.
  masm.assertStackAlignment(JitStackAlignment, 2 * sizeof(uintptr_t));

  // Call function.
  masm.callJitNoProfiler(reg_code);

  {
    // Interpreter -> Baseline OSR will return here.
    masm.bind(&returnLabel);
    masm.addCodeLabel(returnLabel);
    masm.bind(&oomReturnLabel);
  }

  // Discard arguments and padding. Set rsp to the address of the
  // EnterJITStackEntry on the stack.
  masm.lea(Operand(rbp, EnterJITStackEntry::offsetFromFP()), rsp);

  /*****************************************************************
  Place return value where it belongs, pop all saved registers
  *****************************************************************/
  masm.pop(r12);  // vp
  masm.storeValue(JSReturnOperand, Operand(r12, 0));

  // Restore non-volatile registers.
#if defined(_WIN64)
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm6)), xmm6);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm7)), xmm7);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm8)), xmm8);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm9)), xmm9);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm10)), xmm10);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm11)), xmm11);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm12)), xmm12);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm13)), xmm13);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm14)), xmm14);
  masm.vmovdqa(Operand(rsp, offsetof(EnterJITStackEntry::XMM, xmm15)), xmm15);

  masm.addq(Imm32(sizeof(EnterJITStackEntry::XMM) + 8), rsp);

  masm.pop(rsi);
  masm.pop(rdi);
#endif
  masm.pop(r15);
  masm.pop(r14);
  masm.pop(r13);
  masm.pop(r12);
  masm.pop(rbx);

  // Restore frame pointer and return.
  masm.pop(rbp);
  masm.ret();
}

// static
mozilla::Maybe<::JS::ProfilingFrameIterator::RegisterState>
JitRuntime::getCppEntryRegisters(JitFrameLayout* frameStackAddress) {
  if (frameStackAddress->prevType() != FrameType::CppToJSJit) {
    // This is not a CppToJSJit frame, there are no C++ registers here.
    return mozilla::Nothing{};
  }

  // Compute pointer to start of EnterJITStackEntry on the stack.
  uint8_t* fp = frameStackAddress->callerFramePtr();
  auto* enterJITStackEntry = reinterpret_cast<EnterJITStackEntry*>(
      fp + EnterJITStackEntry::offsetFromFP());

  // Extract native function call registers.
  ::JS::ProfilingFrameIterator::RegisterState registerState;
  registerState.fp = enterJITStackEntry->rbp;
  registerState.pc = enterJITStackEntry->rip;
  // sp should be inside the caller's frame, so set sp to the value of the stack
  // pointer before the call to the EnterJit trampoline.
  registerState.sp = &enterJITStackEntry->rip + 1;
  // No lr in this world.
  registerState.lr = nullptr;
  return mozilla::Some(registerState);
}

// Push AllRegs in a way that is compatible with RegisterDump, regardless of
// what PushRegsInMask might do to reduce the set size.
static void DumpAllRegs(MacroAssembler& masm) {
#ifdef ENABLE_WASM_SIMD
  masm.PushRegsInMask(AllRegs);
#else
  // When SIMD isn't supported, PushRegsInMask reduces the set of float
  // registers to be double-sized, while the RegisterDump expects each of
  // the float registers to have the maximal possible size
  // (Simd128DataSize). To work around this, we just spill the double
  // registers by hand here, using the register dump offset directly.
  for (GeneralRegisterBackwardIterator iter(AllRegs.gprs()); iter.more();
       ++iter) {
    masm.Push(*iter);
  }

  masm.reserveStack(sizeof(RegisterDump::FPUArray));
  for (FloatRegisterBackwardIterator iter(AllRegs.fpus()); iter.more();
       ++iter) {
    FloatRegister reg = *iter;
    Address spillAddress(StackPointer, reg.getRegisterDumpOffsetInBytes());
    masm.storeDouble(reg, spillAddress);
  }
#endif
}

void JitRuntime::generateInvalidator(MacroAssembler& masm, Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateInvalidator");

  // See explanatory comment in x86's JitRuntime::generateInvalidator.

  invalidatorOffset_ = startTrampolineCode(masm);

  // Push registers such that we can access them from [base + code].
  DumpAllRegs(masm);

  masm.movq(rsp, rax);  // Argument to jit::InvalidationBailout.

  // Make space for InvalidationBailout's bailoutInfo outparam.
  masm.reserveStack(sizeof(void*));
  masm.movq(rsp, rbx);

  using Fn = bool (*)(InvalidationBailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(rdx);
  masm.passABIArg(rax);
  masm.passABIArg(rbx);
  masm.callWithABI<Fn, InvalidationBailout>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(r9);  // Get the bailoutInfo outparam.

  // Pop the machine state and the dead frame.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in r9.
  masm.jmp(bailoutTail);
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

  // Caller:
  // [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]] <- rsp

  // Frame prologue.
  //
  // NOTE: if this changes, fix the Baseline bailout code too!
  // See BaselineStackBuilder::calculatePrevFramePtr and
  // BaselineStackBuilder::buildRectifierFrame (in BaselineBailouts.cpp).
  masm.push(FramePointer);
  masm.movq(rsp, FramePointer);

  // Load argc.
  masm.loadNumActualArgs(FramePointer, r8);

  // Load |nformals| into %rcx.
  masm.loadPtr(Address(rbp, RectifierFrameLayout::offsetOfCalleeToken()), rax);
  masm.mov(rax, rcx);
  masm.andq(Imm32(uint32_t(CalleeTokenMask)), rcx);
  masm.loadFunctionArgCount(rcx, rcx);

  // Stash another copy in r11, since we are going to do destructive operations
  // on rcx
  masm.mov(rcx, r11);

  static_assert(
      CalleeToken_FunctionConstructing == 1,
      "Ensure that we can use the constructing bit to count the value");
  masm.mov(rax, rdx);
  masm.andq(Imm32(uint32_t(CalleeToken_FunctionConstructing)), rdx);

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
  static_assert(IsPowerOfTwo(JitStackValueAlignment),
                "must have power of two for masm.andl to do its job");

  masm.addl(
      Imm32(JitStackValueAlignment - 1 /* for padding */ + 1 /* for |this| */),
      rcx);
  masm.addl(rdx, rcx);
  masm.andl(Imm32(~(JitStackValueAlignment - 1)), rcx);

  // Load the number of |undefined|s to push into %rcx. Subtract 1 for |this|.
  masm.subl(r8, rcx);
  masm.subl(Imm32(1), rcx);

  // Caller:
  // [arg2] [arg1] [this] [ [argc] [callee] [descr] [raddr] ] <- rsp
  // '--- #r8 ---'
  //
  // Rectifier frame:
  // [rbp'] [undef] [undef] [undef] [arg2] [arg1] [this] [ [argc] [callee]
  //                                                          [descr] [raddr] ]
  //        '------- #rcx --------' '--- #r8 ---'

  // Copy the number of actual arguments into rdx.
  masm.mov(r8, rdx);

  masm.moveValue(UndefinedValue(), ValueOperand(r10));

  // Push undefined. (including the padding)
  {
    Label undefLoopTop;
    masm.bind(&undefLoopTop);

    masm.push(r10);
    masm.subl(Imm32(1), rcx);
    masm.j(Assembler::NonZero, &undefLoopTop);
  }

  // Get the topmost argument.
  static_assert(sizeof(Value) == 8, "TimesEight is used to skip arguments");

  // Get the topmost argument.
  BaseIndex b(FramePointer, r8, TimesEight, sizeof(RectifierFrameLayout));
  masm.lea(Operand(b), rcx);

  // Push arguments, |nargs| + 1 times (to include |this|).
  masm.addl(Imm32(1), r8);
  {
    Label copyLoopTop;

    masm.bind(&copyLoopTop);
    masm.push(Operand(rcx, 0x0));
    masm.subq(Imm32(sizeof(Value)), rcx);
    masm.subl(Imm32(1), r8);
    masm.j(Assembler::NonZero, &copyLoopTop);
  }

  // if constructing, copy newTarget
  {
    Label notConstructing;

    masm.branchTest32(Assembler::Zero, rax,
                      Imm32(CalleeToken_FunctionConstructing),
                      &notConstructing);

    // thisFrame[numFormals] = prevFrame[argc]
    ValueOperand newTarget(r10);

    // Load vp[argc]. Add sizeof(Value) for |this|.
    BaseIndex newTargetSrc(FramePointer, rdx, TimesEight,
                           sizeof(RectifierFrameLayout) + sizeof(Value));
    masm.loadValue(newTargetSrc, newTarget);

    // Again, 1 for |this|
    BaseIndex newTargetDest(rsp, r11, TimesEight, sizeof(Value));
    masm.storeValue(newTarget, newTargetDest);

    masm.bind(&notConstructing);
  }

  // Caller:
  // [arg2] [arg1] [this] [ [argc] [callee] [descr] [raddr] ]
  //
  //
  // Rectifier frame:
  // [rbp'] <- rbp [undef] [undef] [undef] [arg2] [arg1] [this] <- rsp [ [argc]
  //                                                 [callee] [descr] [raddr] ]
  //

  // Construct JitFrameLayout.
  masm.push(rax);  // callee token
  masm.pushFrameDescriptorForJitCall(FrameType::Rectifier, rdx, rdx);

  // Call the target function.
  masm.andq(Imm32(uint32_t(CalleeTokenMask)), rax);
  switch (kind) {
    case ArgumentsRectifierKind::Normal:
      masm.loadJitCodeRaw(rax, rax);
      argumentsRectifierReturnOffset_ = masm.callJitNoProfiler(rax);
      break;
    case ArgumentsRectifierKind::TrialInlining:
      Label noBaselineScript, done;
      masm.loadBaselineJitCodeRaw(rax, rbx, &noBaselineScript);
      masm.callJitNoProfiler(rbx);
      masm.jump(&done);

      // See BaselineCacheIRCompiler::emitCallInlinedFunction.
      masm.bind(&noBaselineScript);
      masm.loadJitCodeRaw(rax, rax);
      masm.callJitNoProfiler(rax);
      masm.bind(&done);
      break;
  }

  masm.mov(FramePointer, StackPointer);
  masm.pop(FramePointer);
  masm.ret();
}

static void PushBailoutFrame(MacroAssembler& masm, Register spArg) {
  // Push registers such that we can access them from [base + code].
  DumpAllRegs(masm);

  // Get the stack pointer into a register, pre-alignment.
  masm.movq(rsp, spArg);
}

static void GenerateBailoutThunk(MacroAssembler& masm, Label* bailoutTail) {
  PushBailoutFrame(masm, r8);

  // Make space for Bailout's bailoutInfo outparam.
  masm.reserveStack(sizeof(void*));
  masm.movq(rsp, r9);

  // Call the bailout function.
  using Fn = bool (*)(BailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(rax);
  masm.passABIArg(r8);
  masm.passABIArg(r9);
  masm.callWithABI<Fn, Bailout>(ABIType::General,
                                CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(r9);  // Get the bailoutInfo outparam.

  // Remove both the bailout frame and the topmost Ion frame's stack.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in r9.
  masm.jmp(bailoutTail);
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
      "Wrapper register set must be a superset of Volatile register set");

  // The context is the first argument.
  Register cxreg = IntArgReg0;
  regs.take(cxreg);

  // Stack is:
  //    ... frame ...
  //  +12 [args]
  //  +8  descriptor
  //  +0  returnAddress
  //
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

  // Copy arguments.
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
        MOZ_CRASH("NYI: x64 callVM should not be used with 128bits values.");
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
      masm.branchTestPtr(Assembler::Zero, rax, rax, masm.failureLabel());
      break;
    case Type_Bool:
      masm.testb(rax, rax);
      masm.j(Assembler::Zero, masm.failureLabel());
      break;
    case Type_Void:
      break;
    default:
      MOZ_CRASH("unknown failure kind");
  }

  // Load the outparam.
  masm.loadVMFunctionOutParam(f, Address(FramePointer, outParamOffset));

  // Until C++ code is instrumented against Spectre, prevent speculative
  // execution from returning any private data.
  if (f.returnsData() && JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }

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

  static_assert(PreBarrierReg == rdx);
  Register temp1 = rax;
  Register temp2 = rbx;
  Register temp3 = rcx;
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

  LiveRegisterSet regs =
      LiveRegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                      FloatRegisterSet(FloatRegisters::VolatileMask));
  masm.PushRegsInMask(regs);

  masm.mov(ImmPtr(cx->runtime()), rcx);

  masm.setupUnalignedABICall(rax);
  masm.passABIArg(rcx);
  masm.passABIArg(rdx);
  masm.callWithABI(JitPreWriteBarrier(type));

  masm.PopRegsInMask(regs);
  masm.ret();

  masm.bind(&noBarrier);
  masm.pop(temp3);
  masm.pop(temp2);
  masm.pop(temp1);
  masm.ret();

  return offset;
}

void JitRuntime::generateBailoutTailStub(MacroAssembler& masm,
                                         Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutTailStub");

  masm.bind(bailoutTail);
  masm.generateBailoutTail(rdx, r9);
}
