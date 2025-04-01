/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/SharedICHelpers-arm.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/PerfSpewer.h"
#include "jit/VMFunctions.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"
#include "vm/Realm.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

static const FloatRegisterSet NonVolatileFloatRegs = FloatRegisterSet(
    (1ULL << FloatRegisters::d8) | (1ULL << FloatRegisters::d9) |
    (1ULL << FloatRegisters::d10) | (1ULL << FloatRegisters::d11) |
    (1ULL << FloatRegisters::d12) | (1ULL << FloatRegisters::d13) |
    (1ULL << FloatRegisters::d14) | (1ULL << FloatRegisters::d15));

static void GenerateReturn(MacroAssembler& masm, int returnCode) {
  // Restore non-volatile floating point registers.
  masm.transferMultipleByRuns(NonVolatileFloatRegs, IsLoad, StackPointer, IA);

  // Get rid of padding word.
  masm.addPtr(Imm32(sizeof(void*)), sp);

  // Set up return value
  masm.ma_mov(Imm32(returnCode), r0);

  // Pop and return
  masm.startDataTransferM(IsLoad, sp, IA, WriteBack);
  masm.transferReg(r4);
  masm.transferReg(r5);
  masm.transferReg(r6);
  masm.transferReg(r7);
  masm.transferReg(r8);
  masm.transferReg(r9);
  masm.transferReg(r10);
  masm.transferReg(r11);
  // r12 isn't saved, so it shouldn't be restored.
  masm.transferReg(pc);
  masm.finishDataTransfer();
  masm.flushBuffer();
}

struct EnterJITStack {
  double d8;
  double d9;
  double d10;
  double d11;
  double d12;
  double d13;
  double d14;
  double d15;

  // Padding.
  void* padding;

  // Non-volatile registers.
  void* r4;
  void* r5;
  void* r6;
  void* r7;
  void* r8;
  void* r9;
  void* r10;
  void* r11;
  // The abi does not expect r12 (ip) to be preserved
  void* lr;

  // Arguments.
  // code == r0
  // argc == r1
  // argv == r2
  // frame == r3
  CalleeToken token;
  JSObject* scopeChain;
  size_t numStackValues;
  Value* vp;
};

/*
 * This method generates a trampoline for a c++ function with the following
 * signature:
 *   void enter(void* code, int argc, Value* argv, InterpreterFrame* fp,
 *              CalleeToken calleeToken, JSObject* scopeChain, Value* vp)
 *   ...using standard EABI calling convention
 */
void JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateEnterJIT");

  enterJITOffset_ = startTrampolineCode(masm);

  const Address slot_token(sp, offsetof(EnterJITStack, token));
  const Address slot_vp(sp, offsetof(EnterJITStack, vp));

  static_assert(OsrFrameReg == r3);

  Assembler* aasm = &masm;

  // Save non-volatile registers. These must be saved by the trampoline,
  // rather than the JIT'd code, because they are scanned by the conservative
  // scanner.
  masm.startDataTransferM(IsStore, sp, DB, WriteBack);
  masm.transferReg(r4);   // [sp,0]
  masm.transferReg(r5);   // [sp,4]
  masm.transferReg(r6);   // [sp,8]
  masm.transferReg(r7);   // [sp,12]
  masm.transferReg(r8);   // [sp,16]
  masm.transferReg(r9);   // [sp,20]
  masm.transferReg(r10);  // [sp,24]
  masm.transferReg(r11);  // [sp,28]
  // The abi does not expect r12 (ip) to be preserved
  masm.transferReg(lr);  // [sp,32]
  // The 5th argument is located at [sp, 36]
  masm.finishDataTransfer();

  // Add padding word.
  masm.subPtr(Imm32(sizeof(void*)), sp);

  // Push the float registers.
  masm.transferMultipleByRuns(NonVolatileFloatRegs, IsStore, sp, DB);

  // Load calleeToken into r9.
  masm.loadPtr(slot_token, r9);

  // Save stack pointer.
  masm.movePtr(sp, r11);

  // Load the number of actual arguments into r10.
  masm.loadPtr(slot_vp, r10);
  masm.unboxInt32(Address(r10, 0), r10);

  {
    Label noNewTarget;
    masm.branchTest32(Assembler::Zero, r9,
                      Imm32(CalleeToken_FunctionConstructing), &noNewTarget);

    masm.add32(Imm32(1), r1);

    masm.bind(&noNewTarget);
  }

  // Guarantee stack alignment of Jit frames.
  //
  // This code moves the stack pointer to the location where it should be when
  // we enter the Jit frame.  It moves the stack pointer such that we have
  // enough space reserved for pushing the arguments, and the JitFrameLayout.
  // The stack pointer is also aligned on the alignment expected by the Jit
  // frames.
  //
  // At the end the register r4, is a pointer to the stack where the first
  // argument is expected by the Jit frame.
  //
  aasm->as_sub(r4, sp, O2RegImmShift(r1, LSL, 3));  // r4 = sp - argc*8
  aasm->as_bic(r4, r4, Imm8(JitStackAlignment - 1));
  // r4 is now the aligned on the bottom of the list of arguments.
  static_assert(
      sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");
  // sp' = ~(JitStackAlignment - 1) & (sp - argc * sizeof(Value))
  masm.movePtr(r4, sp);

  // Get a copy of the number of args to use as a decrement counter, also set
  // the zero condition code.
  aasm->as_mov(r5, O2Reg(r1), SetCC);

  // Loop over arguments, copying them from an unknown buffer onto the Ion
  // stack so they can be accessed from JIT'ed code.
  {
    Label header, footer;
    // If there aren't any arguments, don't do anything.
    aasm->as_b(&footer, Assembler::Zero);
    // Get the top of the loop.
    masm.bind(&header);
    aasm->as_sub(r5, r5, Imm8(1), SetCC);
    // We could be more awesome, and unroll this, using a loadm
    // (particularly since the offset is effectively 0) but that seems more
    // error prone, and complex.
    // BIG FAT WARNING: this loads both r6 and r7.
    aasm->as_extdtr(IsLoad, 64, true, PostIndex, r6,
                    EDtrAddr(r2, EDtrOffImm(8)));
    aasm->as_extdtr(IsStore, 64, true, PostIndex, r6,
                    EDtrAddr(r4, EDtrOffImm(8)));
    aasm->as_b(&header, Assembler::NonZero);
    masm.bind(&footer);
  }

  // Push the callee token.
  masm.push(r9);

  // Push the frame descriptor.
  masm.pushFrameDescriptorForJitCall(FrameType::CppToJSJit, r10, r10);

  Label returnLabel;
  {
    // Handle Interpreter -> Baseline OSR.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT(!regs.has(r11));
    regs.take(OsrFrameReg);
    regs.take(r0);  // jitcode
    MOZ_ASSERT(!regs.has(ReturnReg), "ReturnReg matches r0");

    const Address slot_numStackValues(r11,
                                      offsetof(EnterJITStack, numStackValues));

    Label notOsr;
    masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

    Register scratch = regs.takeAny();

    Register numStackValues = regs.takeAny();
    masm.load32(slot_numStackValues, numStackValues);

    // Write return address. On ARM, CodeLabel is only used for tableswitch,
    // so we can't use it here to get the return address. Instead, we use pc
    // + a fixed offset to a jump to returnLabel. The pc register holds pc +
    // 8, so we add the size of 2 instructions to skip the instructions
    // emitted by push and jump(&skipJump).
    {
      AutoForbidPoolsAndNops afp(&masm, 5);
      Label skipJump;
      masm.mov(pc, scratch);
      masm.addPtr(Imm32(2 * sizeof(uint32_t)), scratch);
      masm.push(scratch);
      masm.jump(&skipJump);
      masm.jump(&returnLabel);
      masm.bind(&skipJump);
    }

    // Frame prologue.
    masm.push(FramePointer);
    masm.mov(sp, FramePointer);

    // Reserve frame.
    masm.subPtr(Imm32(BaselineFrame::Size()), sp);

    Register framePtrScratch = regs.takeAny();
    masm.touchFrameValues(numStackValues, scratch, framePtrScratch);
    masm.mov(sp, framePtrScratch);

    // Reserve space for locals and stack values.
    masm.ma_lsl(Imm32(3), numStackValues, scratch);
    masm.ma_sub(sp, scratch, sp);

    // Enter exit frame.
    masm.pushFrameDescriptor(FrameType::BaselineJS);
    masm.push(Imm32(0));  // Fake return address.
    masm.push(FramePointer);
    // No GC things to mark on the stack, push a bare token.
    masm.loadJSContext(scratch);
    masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    masm.push(r0);  // jitcode

    using Fn = bool (*)(BaselineFrame* frame, InterpreterFrame* interpFrame,
                        uint32_t numStackValues);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(framePtrScratch);  // BaselineFrame
    masm.passABIArg(OsrFrameReg);      // InterpreterFrame
    masm.passABIArg(numStackValues);
    masm.callWithABI<Fn, jit::InitBaselineFrameForOsr>(
        ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    Register jitcode = regs.takeAny();
    masm.pop(jitcode);

    MOZ_ASSERT(jitcode != ReturnReg);

    Label error;
    masm.addPtr(Imm32(ExitFrameLayout::SizeWithFooter()), sp);
    masm.branchIfFalseBool(ReturnReg, &error);

    // If OSR-ing, then emit instrumentation for setting lastProfilerFrame
    // if profiler instrumentation is enabled.
    {
      Label skipProfilingInstrumentation;
      AbsoluteAddress addressOfEnabled(
          cx->runtime()->geckoProfiler().addressOfEnabled());
      masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                    &skipProfilingInstrumentation);
      masm.profilerEnterFrame(FramePointer, scratch);
      masm.bind(&skipProfilingInstrumentation);
    }

    masm.jump(jitcode);

    // OOM: frame epilogue, load error value, discard return address and return.
    masm.bind(&error);
    masm.mov(FramePointer, sp);
    masm.pop(FramePointer);
    masm.addPtr(Imm32(sizeof(uintptr_t)), sp);  // Return address.
    masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    masm.jump(&returnLabel);

    masm.bind(&notOsr);
    // Load the scope chain in R1.
    MOZ_ASSERT(R1.scratchReg() != r0);
    masm.loadPtr(Address(r11, offsetof(EnterJITStack, scopeChain)),
                 R1.scratchReg());
  }

  // The callee will push the return address and frame pointer on the stack,
  // thus we check that the stack would be aligned once the call is complete.
  masm.assertStackAlignment(JitStackAlignment, 2 * sizeof(uintptr_t));

  // Call the function.
  masm.callJitNoProfiler(r0);

  // Interpreter -> Baseline OSR will return here.
  masm.bind(&returnLabel);

  // Discard arguments and padding. Set sp to the address of the EnterJITStack
  // on the stack.
  masm.mov(r11, sp);

  // Store the returned value into the slot_vp
  masm.loadPtr(slot_vp, r5);
  masm.storeValue(JSReturnOperand, Address(r5, 0));

  // Restore non-volatile registers and return.
  GenerateReturn(masm, true);
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
  // See large comment in x86's JitRuntime::generateInvalidator.

  AutoCreatedBy acb(masm, "JitRuntime::generateInvalidator");

  invalidatorOffset_ = startTrampolineCode(masm);

  // At this point, one of two things has happened:
  // 1) Execution has just returned from C code, which left the stack aligned
  // 2) Execution has just returned from Ion code, which left the stack
  // unaligned. The old return address should not matter, but we still want the
  // stack to be aligned, and there is no good reason to automatically align it
  // with a call to setupUnalignedABICall.
  masm.as_bic(sp, sp, Imm8(7));
  masm.startDataTransferM(IsStore, sp, DB, WriteBack);
  // We don't have to push everything, but this is likely easier.
  // Setting regs_.
  for (uint32_t i = 0; i < Registers::Total; i++) {
    masm.transferReg(Register::FromCode(i));
  }
  masm.finishDataTransfer();

  // Since our datastructures for stack inspection are compile-time fixed,
  // if there are only 16 double registers, then we need to reserve
  // space on the stack for the missing 16.
  if (FloatRegisters::ActualTotalPhys() != FloatRegisters::TotalPhys) {
    ScratchRegisterScope scratch(masm);
    int missingRegs =
        FloatRegisters::TotalPhys - FloatRegisters::ActualTotalPhys();
    masm.ma_sub(Imm32(missingRegs * sizeof(double)), sp, scratch);
  }

  masm.startFloatTransferM(IsStore, sp, DB, WriteBack);
  for (uint32_t i = 0; i < FloatRegisters::ActualTotalPhys(); i++) {
    masm.transferFloatReg(FloatRegister(i, FloatRegister::Double));
  }
  masm.finishFloatTransfer();

  masm.ma_mov(sp, r0);
  // Reserve 8 bytes for the outparam to ensure alignment for
  // setupAlignedABICall.
  masm.reserveStack(sizeof(void*) * 2);
  masm.mov(sp, r1);
  using Fn = bool (*)(InvalidationBailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupAlignedABICall();
  masm.passABIArg(r0);
  masm.passABIArg(r1);
  masm.callWithABI<Fn, InvalidationBailout>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(r2);  // Get bailoutInfo outparam.

  // Pop the machine state and the dead frame.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in r2.
  masm.jump(bailoutTail);
}

void JitRuntime::generateArgumentsRectifier(MacroAssembler& masm,
                                            ArgumentsRectifierKind kind) {
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

  // Frame prologue.
  //
  // NOTE: if this changes, fix the Baseline bailout code too!
  // See BaselineStackBuilder::calculatePrevFramePtr and
  // BaselineStackBuilder::buildRectifierFrame (in BaselineBailouts.cpp).
  masm.push(FramePointer);
  masm.mov(StackPointer, FramePointer);

  static_assert(JitStackAlignment == sizeof(Value));

  // Copy number of actual arguments into r0 and r8.
  masm.loadNumActualArgs(FramePointer, r0);
  masm.mov(r0, r8);

  // Load the number of |undefined|s to push into r6.
  masm.loadPtr(
      Address(FramePointer, RectifierFrameLayout::offsetOfCalleeToken()), r1);
  {
    ScratchRegisterScope scratch(masm);
    masm.ma_and(Imm32(CalleeTokenMask), r1, r6, scratch);
  }
  masm.loadFunctionArgCount(r6, r6);

  masm.ma_sub(r6, r8, r2);

  // Get the topmost argument.
  {
    ScratchRegisterScope scratch(masm);
    masm.ma_alu(sp, lsl(r8, 3), r3, OpAdd);  // r3 <- sp + nargs * 8
    masm.ma_add(r3, Imm32(sizeof(RectifierFrameLayout)), r3, scratch);
  }

  {
    Label notConstructing;

    masm.branchTest32(Assembler::Zero, r1,
                      Imm32(CalleeToken_FunctionConstructing),
                      &notConstructing);

    // Add sizeof(Value) to overcome |this|
    masm.as_extdtr(IsLoad, 64, true, Offset, r4, EDtrAddr(r3, EDtrOffImm(8)));
    masm.as_extdtr(IsStore, 64, true, PreIndex, r4,
                   EDtrAddr(sp, EDtrOffImm(-8)));

    masm.bind(&notConstructing);
  }

  // Push undefined.
  masm.moveValue(UndefinedValue(), ValueOperand(r5, r4));
  {
    Label undefLoopTop;
    masm.bind(&undefLoopTop);
    masm.as_extdtr(IsStore, 64, true, PreIndex, r4,
                   EDtrAddr(sp, EDtrOffImm(-8)));
    masm.as_sub(r2, r2, Imm8(1), SetCC);

    masm.ma_b(&undefLoopTop, Assembler::NonZero);
  }

  // Push arguments, |nargs| + 1 times (to include |this|).
  {
    Label copyLoopTop;
    masm.bind(&copyLoopTop);
    masm.as_extdtr(IsLoad, 64, true, PostIndex, r4,
                   EDtrAddr(r3, EDtrOffImm(-8)));
    masm.as_extdtr(IsStore, 64, true, PreIndex, r4,
                   EDtrAddr(sp, EDtrOffImm(-8)));

    masm.as_sub(r8, r8, Imm8(1), SetCC);
    masm.ma_b(&copyLoopTop, Assembler::NotSigned);
  }

  // Construct JitFrameLayout.
  masm.ma_push(r1);  // callee token
  masm.pushFrameDescriptorForJitCall(FrameType::Rectifier, r0, r0);

  // Call the target function.
  masm.andPtr(Imm32(CalleeTokenMask), r1);
  switch (kind) {
    case ArgumentsRectifierKind::Normal:
      masm.loadJitCodeRaw(r1, r3);
      argumentsRectifierReturnOffset_ = masm.callJitNoProfiler(r3);
      break;
    case ArgumentsRectifierKind::TrialInlining:
      Label noBaselineScript, done;
      masm.loadBaselineJitCodeRaw(r1, r3, &noBaselineScript);
      masm.callJitNoProfiler(r3);
      masm.jump(&done);

      // See BaselineCacheIRCompiler::emitCallInlinedFunction.
      masm.bind(&noBaselineScript);
      masm.loadJitCodeRaw(r1, r3);
      masm.callJitNoProfiler(r3);
      masm.bind(&done);
      break;
  }

  masm.mov(FramePointer, StackPointer);
  masm.pop(FramePointer);
  masm.ret();
}

static void PushBailoutFrame(MacroAssembler& masm, Register spArg) {
#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  // STEP 1a: Save our register sets to the stack so Bailout() can read
  // everything.
  // sp % 8 == 0

  masm.startDataTransferM(IsStore, sp, DB, WriteBack);
  // We don't have to push everything, but this is likely easier.
  // Setting regs_.
  for (uint32_t i = 0; i < Registers::Total; i++) {
    masm.transferReg(Register::FromCode(i));
  }
  masm.finishDataTransfer();

  ScratchRegisterScope scratch(masm);

  // Since our datastructures for stack inspection are compile-time fixed,
  // if there are only 16 double registers, then we need to reserve
  // space on the stack for the missing 16.
  if (FloatRegisters::ActualTotalPhys() != FloatRegisters::TotalPhys) {
    int missingRegs =
        FloatRegisters::TotalPhys - FloatRegisters::ActualTotalPhys();
    masm.ma_sub(Imm32(missingRegs * sizeof(double)), sp, scratch);
  }
  masm.startFloatTransferM(IsStore, sp, DB, WriteBack);
  for (uint32_t i = 0; i < FloatRegisters::ActualTotalPhys(); i++) {
    masm.transferFloatReg(FloatRegister(i, FloatRegister::Double));
  }
  masm.finishFloatTransfer();

  // The current stack pointer is the first argument to jit::Bailout.
  masm.ma_mov(sp, spArg);
}

static void GenerateBailoutThunk(MacroAssembler& masm, Label* bailoutTail) {
  PushBailoutFrame(masm, r0);

  // Make space for Bailout's bailoutInfo outparam.
  masm.reserveStack(sizeof(void*));
  masm.mov(sp, r1);
  using Fn = bool (*)(BailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupAlignedABICall();

  masm.passABIArg(r0);
  masm.passABIArg(r1);

  masm.callWithABI<Fn, Bailout>(ABIType::General,
                                CheckUnsafeCallWithABI::DontCheckOther);
  masm.pop(r2);  // Get the bailoutInfo outparam.

  // Remove both the bailout frame and the topmost Ion frame's stack.
  masm.moveToStackPtr(FramePointer);

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in r2.
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

  AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

  static_assert(
      (Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
      "Wrapper register set must be a superset of Volatile register set.");

  // The context is the first argument; r0 is the first argument register.
  Register cxreg = r0;
  regs.take(cxreg);

  // On link-register platforms, it is the responsibility of the VM *callee* to
  // push the return address, while the caller must ensure that the address
  // is stored in lr on entry. This allows the VM wrapper to work with both
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
        masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::General);
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::DoubleByValue:
        // Values should be passed by reference, not by value, so we assert
        // that the argument is a double-precision float.
        MOZ_ASSERT(f.argPassedInFloatReg(explicitArg));
        masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::Float64);
        argDisp += sizeof(double);
        break;
      case VMFunctionData::WordByRef:
        masm.passABIArg(MoveOperand(FramePointer, argDisp,
                                    MoveOperand::Kind::EffectiveAddress),
                        ABIType::General);
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::DoubleByRef:
        masm.passABIArg(MoveOperand(FramePointer, argDisp,
                                    MoveOperand::Kind::EffectiveAddress),
                        ABIType::General);
        argDisp += 2 * sizeof(void*);
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
      masm.branchTestPtr(Assembler::Zero, r0, r0, masm.failureLabel());
      break;
    case Type_Bool:
      masm.branchIfFalseBool(r0, masm.failureLabel());
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

  masm.pushReturnAddress();

  static_assert(PreBarrierReg == r1);
  Register temp1 = r2;
  Register temp2 = r3;
  Register temp3 = r4;
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
  save.set() =
      RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                  FloatRegisterSet(FloatRegisters::VolatileDoubleMask));
  masm.PushRegsInMask(save);

  masm.movePtr(ImmPtr(cx->runtime()), r0);

  masm.setupUnalignedABICall(r2);
  masm.passABIArg(r0);
  masm.passABIArg(r1);
  masm.callWithABI(JitPreWriteBarrier(type));
  masm.PopRegsInMask(save);
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
  masm.generateBailoutTail(r1, r2);
}
