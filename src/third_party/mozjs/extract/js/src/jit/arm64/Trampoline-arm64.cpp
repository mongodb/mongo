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
#include "jit/arm64/SharedICHelpers-arm64.h"
#include "jit/VMFunctions.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

/* This method generates a trampoline on ARM64 for a c++ function with
 * the following signature:
 *   bool blah(void* code, int argc, Value* argv,
 *             JSObject* scopeChain, Value* vp)
 *   ...using standard AArch64 calling convention
 */
void JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm) {
  enterJITOffset_ = startTrampolineCode(masm);

  const Register reg_code = IntArgReg0;      // EnterJitData::jitcode.
  const Register reg_argc = IntArgReg1;      // EnterJitData::maxArgc.
  const Register reg_argv = IntArgReg2;      // EnterJitData::maxArgv.
  const Register reg_osrFrame = IntArgReg3;  // EnterJitData::osrFrame.
  const Register reg_callee = IntArgReg4;    // EnterJitData::calleeToken.
  const Register reg_scope = IntArgReg5;     // EnterJitData::scopeChain.
  const Register reg_osrNStack =
      IntArgReg6;                      // EnterJitData::osrNumStackValues.
  const Register reg_vp = IntArgReg7;  // Address of EnterJitData::result.

  static_assert(OsrFrameReg == IntArgReg3);

  // During the pushes below, use the normal stack pointer.
  masm.SetStackPointer64(sp);

  // Save old frame pointer and return address; set new frame pointer.
  masm.push(r29, r30);
  masm.moveStackPtrTo(r29);

  // Save callee-save integer registers.
  // Also save x7 (reg_vp) and x30 (lr), for use later.
  masm.push(r19, r20, r21, r22);
  masm.push(r23, r24, r25, r26);
  masm.push(r27, r28, r7, r30);

  // Save callee-save floating-point registers.
  // AArch64 ABI specifies that only the lower 64 bits must be saved.
  masm.push(d8, d9, d10, d11);
  masm.push(d12, d13, d14, d15);

#ifdef DEBUG
  // Emit stack canaries.
  masm.movePtr(ImmWord(0xdeadd00d), r23);
  masm.movePtr(ImmWord(0xdeadd11d), r24);
  masm.push(r23, r24);
#endif

  // Common code below attempts to push single registers at a time,
  // which breaks the stack pointer's 16-byte alignment requirement.
  // Note that movePtr() is invalid because StackPointer is treated as xzr.
  //
  // FIXME: After testing, this entire function should be rewritten to not
  // use the PseudoStackPointer: since the amount of data pushed is
  // precalculated, we can just allocate the whole frame header at once and
  // index off sp. This will save a significant number of instructions where
  // Push() updates sp.
  masm.Mov(PseudoStackPointer64, sp);
  masm.SetStackPointer64(PseudoStackPointer64);

  // Save the stack pointer at this point for Baseline OSR.
  masm.moveStackPtrTo(BaselineFrameReg);
  // Remember stack depth without padding and arguments.
  masm.moveStackPtrTo(r19);

  // If constructing, include newTarget in argument vector.
  {
    Label noNewTarget;
    Imm32 constructingToken(CalleeToken_FunctionConstructing);
    masm.branchTest32(Assembler::Zero, reg_callee, constructingToken,
                      &noNewTarget);
    masm.add32(Imm32(1), reg_argc);
    masm.bind(&noNewTarget);
  }

  // JitFrameLayout is as follows (higher is higher in memory):
  //  N*8  - [ JS argument vector ] (base 16-byte aligned)
  //  8    - numActualArgs
  //  8    - calleeToken (16-byte aligned)
  //  8    - frameDescriptor
  //  8    - returnAddress (16-byte aligned, pushed by callee)

  // Touch frame incrementally (a requirement for Windows).
  //
  // Use already saved callee-save registers r20 and r21 as temps.
  //
  // This has to be done outside the ScratchRegisterScope, as the temps are
  // under demand inside the touchFrameValues call.

  // Give sp 16-byte alignment and sync stack pointers.
  masm.andToStackPtr(Imm32(~0xf));
  // We needn't worry about the Gecko Profiler mark because touchFrameValues
  // touches in large increments.
  masm.touchFrameValues(reg_argc, r20, r21);
  // Restore stack pointer, preserved above.
  masm.moveToStackPtr(r19);

  // Push the argument vector onto the stack.
  // WARNING: destructively modifies reg_argv
  {
    vixl::UseScratchRegisterScope temps(&masm.asVIXL());

    const ARMRegister tmp_argc = temps.AcquireX();
    const ARMRegister tmp_sp = temps.AcquireX();

    Label noArguments;
    Label loopHead;

    masm.movePtr(reg_argc, tmp_argc.asUnsized());

    // sp -= 8
    // Since we're using PostIndex Str below, this is necessary to avoid
    // overwriting the Gecko Profiler mark pushed above.
    masm.subFromStackPtr(Imm32(8));

    // sp -= 8 * argc
    masm.Sub(PseudoStackPointer64, PseudoStackPointer64,
             Operand(tmp_argc, vixl::SXTX, 3));

    // Give sp 16-byte alignment and sync stack pointers.
    masm.andToStackPtr(Imm32(~0xf));
    masm.moveStackPtrTo(tmp_sp.asUnsized());

    masm.branchTestPtr(Assembler::Zero, reg_argc, reg_argc, &noArguments);

    // Begin argument-pushing loop.
    // This could be optimized using Ldp and Stp.
    {
      masm.bind(&loopHead);

      // Load an argument from argv, then increment argv by 8.
      masm.Ldr(x24, MemOperand(ARMRegister(reg_argv, 64), Operand(8),
                               vixl::PostIndex));

      // Store the argument to tmp_sp, then increment tmp_sp by 8.
      masm.Str(x24, MemOperand(tmp_sp, Operand(8), vixl::PostIndex));

      // Decrement tmp_argc and set the condition codes for the new value.
      masm.Subs(tmp_argc, tmp_argc, Operand(1));

      // Branch if arguments remain.
      masm.B(&loopHead, vixl::Condition::NonZero);
    }

    masm.bind(&noArguments);
  }
  masm.checkStackAlignment();

  // Calculate the number of bytes pushed so far.
  masm.subStackPtrFrom(r19);

  // Create the frame descriptor.
  masm.makeFrameDescriptor(r19, FrameType::CppToJSJit, JitFrameLayout::Size());

  // Push the number of actual arguments and the calleeToken.
  // The result address is used to store the actual number of arguments
  // without adding an argument to EnterJIT.
  {
    vixl::UseScratchRegisterScope temps(&masm.asVIXL());
    MOZ_ASSERT(temps.IsAvailable(ScratchReg64));  // ip0
    temps.Exclude(ScratchReg64);
    masm.unboxInt32(Address(reg_vp, 0x0), ScratchReg64.asUnsized());
    masm.push(ScratchReg64.asUnsized(), reg_callee);
  }
  masm.checkStackAlignment();

  // Push the descriptor.
  masm.Push(r19);

  Label osrReturnPoint;
  {
    // Check for Interpreter -> Baseline OSR.
    Label notOsr;
    masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

    // Push return address and previous frame pointer.
    {
      vixl::UseScratchRegisterScope temps(&masm.asVIXL());
      MOZ_ASSERT(temps.IsAvailable(ScratchReg2_64));  // ip1
      temps.Exclude(ScratchReg2_64);

      masm.Adr(ScratchReg2_64, &osrReturnPoint);
      masm.push(ScratchReg2, BaselineFrameReg);

      // Reserve frame.
      masm.subFromStackPtr(Imm32(BaselineFrame::Size()));

      masm.touchFrameValues(reg_osrNStack, ScratchReg2, BaselineFrameReg);
    }
    masm.moveStackPtrTo(BaselineFrameReg);

    // Reserve space for locals and stack values.
    masm.Lsl(w19, ARMRegister(reg_osrNStack, 32),
             3);  // w19 = num_stack_values * sizeof(Value).
    masm.subFromStackPtr(r19);

    // Enter exit frame.
    masm.addPtr(
        Imm32(BaselineFrame::Size() + BaselineFrame::FramePointerOffset), r19);
    masm.makeFrameDescriptor(r19, FrameType::BaselineJS,
                             ExitFrameLayout::Size());
    masm.asVIXL().Push(x19, xzr);  // Push xzr for a fake return address.
    // No GC things to mark: push a bare token.
    masm.loadJSContext(r19);
    masm.enterFakeExitFrame(r19, r19, ExitFrameType::Bare);

    masm.push(BaselineFrameReg, reg_code);

    // Initialize the frame, including filling in the slots.
    using Fn = bool (*)(BaselineFrame * frame, InterpreterFrame * interpFrame,
                        uint32_t numStackValues);
    masm.setupUnalignedABICall(r19);
    masm.passABIArg(BaselineFrameReg);  // BaselineFrame.
    masm.passABIArg(reg_osrFrame);      // InterpreterFrame.
    masm.passABIArg(reg_osrNStack);
    masm.callWithABI<Fn, jit::InitBaselineFrameForOsr>(
        MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    masm.pop(r19, BaselineFrameReg);
    MOZ_ASSERT(r19 != ReturnReg);

    masm.addToStackPtr(Imm32(ExitFrameLayout::SizeWithFooter()));
    masm.addPtr(Imm32(BaselineFrame::Size()), BaselineFrameReg);

    Label error;
    masm.branchIfFalseBool(ReturnReg, &error);

    masm.jump(r19);

    // OOM: load error value, discard return address and previous frame
    // pointer, and return.
    masm.bind(&error);
    masm.Add(masm.GetStackPointer64(), BaselineFrameReg64,
             Operand(2 * sizeof(uintptr_t)));
    masm.syncStackPtr();
    masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    masm.B(&osrReturnPoint);

    masm.bind(&notOsr);
    masm.movePtr(reg_scope, R1_);
  }

  // Call function.
  // Since AArch64 doesn't have the pc register available, the callee must push
  // lr.
  masm.callJitNoProfiler(reg_code);

  // Interpreter -> Baseline OSR will return here.
  masm.bind(&osrReturnPoint);

  masm.Pop(r19);       // Pop frame descriptor.
  masm.pop(r24, r23);  // Discard calleeToken, numActualArgs.

  // Discard arguments and the stack alignment padding.
  masm.Add(masm.GetStackPointer64(), masm.GetStackPointer64(),
           Operand(x19, vixl::LSR, FRAMESIZE_SHIFT));

  masm.syncStackPtr();
  masm.SetStackPointer64(sp);

#ifdef DEBUG
  // Check that canaries placed on function entry are still present.
  masm.pop(r24, r23);
  Label x23OK, x24OK;

  masm.branchPtr(Assembler::Equal, r23, ImmWord(0xdeadd00d), &x23OK);
  masm.breakpoint();
  masm.bind(&x23OK);

  masm.branchPtr(Assembler::Equal, r24, ImmWord(0xdeadd11d), &x24OK);
  masm.breakpoint();
  masm.bind(&x24OK);
#endif

  // Restore callee-save floating-point registers.
  masm.pop(d15, d14, d13, d12);
  masm.pop(d11, d10, d9, d8);

  // Restore callee-save integer registers.
  // Also restore x7 (reg_vp) and x30 (lr).
  masm.pop(r30, r7, r28, r27);
  masm.pop(r26, r25, r24, r23);
  masm.pop(r22, r21, r20, r19);

  // Store return value (in JSReturnReg = x2 to just-popped reg_vp).
  masm.storeValue(JSReturnOperand, Address(reg_vp, 0));

  // Restore old frame pointer.
  masm.pop(r30, r29);

  // Return using the value popped into x30.
  masm.abiret();

  // Reset stack pointer.
  masm.SetStackPointer64(PseudoStackPointer64);
}

// static
mozilla::Maybe<::JS::ProfilingFrameIterator::RegisterState>
JitRuntime::getCppEntryRegisters(JitFrameLayout* frameStackAddress) {
  // Not supported, or not implemented yet.
  // TODO: Implement along with the corresponding stack-walker changes, in
  // coordination with the Gecko Profiler, see bug 1635987 and follow-ups.
  return mozilla::Nothing{};
}

static void PushRegisterDump(MacroAssembler& masm) {
  const LiveRegisterSet First28GeneralRegisters = LiveRegisterSet(
      GeneralRegisterSet(Registers::AllMask &
                         ~(1 << 31 | 1 << 30 | 1 << 29 | 1 << 28)),
      FloatRegisterSet(FloatRegisters::NoneMask));

  const LiveRegisterSet AllFloatRegisters =
      LiveRegisterSet(GeneralRegisterSet(Registers::NoneMask),
                      FloatRegisterSet(FloatRegisters::AllMask));

  // Push all general-purpose registers.
  //
  // The ARM64 ABI does not treat SP as a normal register that can
  // be pushed. So pushing happens in two phases.
  //
  // Registers are pushed in reverse order of code.
  //
  // See block comment in MacroAssembler.h for further required invariants.

  // First, push the last four registers, passing zero for sp.
  // Zero is pushed for x28 and x31: the pseudo-SP and SP, respectively.
  masm.asVIXL().Push(xzr, x30, x29, xzr);

  // Second, push the first 28 registers that serve no special purpose.
  masm.PushRegsInMask(First28GeneralRegisters);

  // Finally, push all floating-point registers, completing the RegisterDump.
  masm.PushRegsInMask(AllFloatRegisters);
}

void JitRuntime::generateInvalidator(MacroAssembler& masm, Label* bailoutTail) {
  invalidatorOffset_ = startTrampolineCode(masm);

  // The InvalidationBailoutStack saved in r0 must be:
  // - osiPointReturnAddress_
  // - ionScript_  (pushed by CodeGeneratorARM64::generateInvalidateEpilogue())
  // - regs_  (pushed here)
  // - fpregs_  (pushed here) [=r0]
  PushRegisterDump(masm);
  masm.moveStackPtrTo(r0);

  masm.Sub(x1, masm.GetStackPointer64(), Operand(sizeof(size_t)));
  masm.Sub(x2, masm.GetStackPointer64(),
           Operand(sizeof(size_t) + sizeof(void*)));
  masm.moveToStackPtr(r2);

  using Fn = bool (*)(InvalidationBailoutStack * sp, size_t * frameSizeOut,
                      BaselineBailoutInfo * *info);
  masm.setupUnalignedABICall(r10);
  masm.passABIArg(r0);
  masm.passABIArg(r1);
  masm.passABIArg(r2);

  masm.callWithABI<Fn, InvalidationBailout>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(r2, r1);

  masm.Add(masm.GetStackPointer64(), masm.GetStackPointer64(), x1);
  masm.Add(masm.GetStackPointer64(), masm.GetStackPointer64(),
           Operand(sizeof(InvalidationBailoutStack)));
  masm.syncStackPtr();

  masm.jump(bailoutTail);
}

void JitRuntime::generateArgumentsRectifier(MacroAssembler& masm,
                                            ArgumentsRectifierKind kind) {
  switch (kind) {
    case ArgumentsRectifierKind::Normal:
      argumentsRectifierOffset_ = startTrampolineCode(masm);
      break;
    case ArgumentsRectifierKind::TrialInlining:
      trialInliningArgumentsRectifierOffset_ = startTrampolineCode(masm);
      break;
  }

  // Save the return address for later.
  masm.push(lr);

  // Load the information that the rectifier needs from the stack.
  masm.Ldr(w0, MemOperand(masm.GetStackPointer64(),
                          RectifierFrameLayout::offsetOfNumActualArgs()));
  masm.Ldr(x1, MemOperand(masm.GetStackPointer64(),
                          RectifierFrameLayout::offsetOfCalleeToken()));

  // Extract a JSFunction pointer from the callee token and keep the
  // intermediary to avoid later recalculation.
  masm.And(x5, x1, Operand(CalleeTokenMask));

  // Get the arguments from the function object.
  masm.Ldrh(x6, MemOperand(x5, JSFunction::offsetOfNargs()));

  static_assert(CalleeToken_FunctionConstructing == 0x1,
                "Constructing must be low-order bit");
  masm.And(x4, x1, Operand(CalleeToken_FunctionConstructing));
  masm.Add(x7, x6, x4);

  // Copy the number of actual arguments into r8.
  masm.mov(r0, r8);

  // Calculate the position that our arguments are at before sp gets modified.
  masm.Add(x3, masm.GetStackPointer64(), Operand(x8, vixl::LSL, 3));
  masm.Add(x3, x3, Operand(sizeof(RectifierFrameLayout)));

  // Pad to a multiple of 16 bytes. This neglects the |this| value,
  // which will also be pushed, because the rest of the frame will
  // round off that value. See pushes of |argc|, |callee| and |desc| below.
  Label noPadding;
  masm.Tbnz(x7, 0, &noPadding);
  masm.asVIXL().Push(xzr);
  masm.Add(x7, x7, Operand(1));
  masm.bind(&noPadding);

  {
    Label notConstructing;
    masm.Cbz(x4, &notConstructing);

    // new.target lives at the end of the pushed args
    // NB: The arg vector holder starts at the beginning of the last arg,
    //     add a value to get to argv[argc]
    masm.loadPtr(Address(r3, sizeof(Value)), r4);
    masm.Push(r4);

    masm.bind(&notConstructing);
  }

  // Calculate the number of undefineds that need to be pushed.
  masm.Sub(w2, w6, w8);

  // Put an undefined in a register so it can be pushed.
  masm.moveValue(UndefinedValue(), ValueOperand(r4));

  // Push undefined N times.
  {
    Label undefLoopTop;
    masm.bind(&undefLoopTop);
    masm.Push(r4);
    masm.Subs(w2, w2, Operand(1));
    masm.B(&undefLoopTop, Assembler::NonZero);
  }

  // Arguments copy loop. Copy for x8 >= 0 to include |this|.
  {
    Label copyLoopTop;
    masm.bind(&copyLoopTop);
    masm.Ldr(x4, MemOperand(x3, -sizeof(Value), vixl::PostIndex));
    masm.Push(r4);
    masm.Subs(x8, x8, Operand(1));
    masm.B(&copyLoopTop, Assembler::NotSigned);
  }

  // Fix up the size of the stack frame. +1 accounts for |this|.
  masm.Add(x6, x7, Operand(1));
  masm.Lsl(x6, x6, 3);

  // Make that into a frame descriptor.
  masm.makeFrameDescriptor(r6, FrameType::Rectifier, JitFrameLayout::Size());

  masm.push(r0,   // Number of actual arguments.
            r1,   // Callee token.
            r6);  // Frame descriptor.

  // Call the target function.
  switch (kind) {
    case ArgumentsRectifierKind::Normal:
      masm.loadJitCodeRaw(r5, r3);
      argumentsRectifierReturnOffset_ = masm.callJitNoProfiler(r3);
      break;
    case ArgumentsRectifierKind::TrialInlining:
      Label noBaselineScript, done;
      masm.loadBaselineJitCodeRaw(r5, r3, &noBaselineScript);
      masm.callJitNoProfiler(r3);
      masm.jump(&done);

      // See BaselineCacheIRCompiler::emitCallInlinedFunction.
      masm.bind(&noBaselineScript);
      masm.loadJitCodeRaw(r5, r3);
      masm.callJitNoProfiler(r3);
      masm.bind(&done);
      break;
  }

  // Clean up!
  // Get the size of the stack frame, and clean up the later fixed frame.
  masm.Ldr(x4, MemOperand(masm.GetStackPointer64(), 24, vixl::PostIndex));

  // Now that the size of the stack frame sans the fixed frame has been loaded,
  // add that onto the stack pointer.
  masm.Add(masm.GetStackPointer64(), masm.GetStackPointer64(),
           Operand(x4, vixl::LSR, FRAMESIZE_SHIFT));

  // Pop the return address from earlier and branch.
  masm.ret();
}

static void PushBailoutFrame(MacroAssembler& masm, Register spArg) {
  // This assumes no SIMD registers, as JS does not support SIMD.

  // The stack saved in spArg must be (higher entries have higher memory
  // addresses):
  // - snapshotOffset_
  // - frameSize_
  // - regs_
  // - fpregs_ (spArg + 0)
  PushRegisterDump(masm);
  masm.moveStackPtrTo(spArg);
}

static void GenerateBailoutThunk(MacroAssembler& masm, Label* bailoutTail) {
  PushBailoutFrame(masm, r0);

  // SP % 8 == 4
  // STEP 1c: Call the bailout function, giving a pointer to the
  //          structure we just blitted onto the stack.
  // Make space for the BaselineBailoutInfo* outparam.
  masm.reserveStack(sizeof(void*));
  masm.moveStackPtrTo(r1);

  using Fn = bool (*)(BailoutStack * sp, BaselineBailoutInfo * *info);
  masm.setupUnalignedABICall(r2);
  masm.passABIArg(r0);
  masm.passABIArg(r1);
  masm.callWithABI<Fn, Bailout>(MoveOp::GENERAL,
                                CheckUnsafeCallWithABI::DontCheckOther);

  // Get the bailoutInfo outparam.
  masm.pop(r2);

  // Stack is:
  //     [frame]
  //     snapshotOffset
  //     frameSize
  //     [bailoutFrame]
  //
  // We want to remove both the bailout frame and the topmost Ion frame's stack.

  // Remove the bailoutFrame.
  static const uint32_t BailoutDataSize = sizeof(RegisterDump);
  masm.addToStackPtr(Imm32(BailoutDataSize));

  // Pop the frame, snapshotOffset, and frameSize.
  vixl::UseScratchRegisterScope temps(&masm.asVIXL());
  const ARMRegister scratch64 = temps.AcquireX();
  masm.Ldr(scratch64, MemOperand(masm.GetStackPointer64(), 0x0));
  masm.addPtr(Imm32(2 * sizeof(void*)), scratch64.asUnsized());
  masm.addToStackPtr(scratch64.asUnsized());

  // Jump to shared bailout tail. The BailoutInfo pointer has to be in r2.
  masm.jump(bailoutTail);
}

JitRuntime::BailoutTable JitRuntime::generateBailoutTable(MacroAssembler& masm,
                                                          Label* bailoutTail,
                                                          uint32_t frameClass) {
  MOZ_CRASH("arm64 does not use bailout tables");
}

void JitRuntime::generateBailoutHandler(MacroAssembler& masm,
                                        Label* bailoutTail) {
  bailoutHandlerOffset_ = startTrampolineCode(masm);

  GenerateBailoutThunk(masm, bailoutTail);
}

bool JitRuntime::generateVMWrapper(JSContext* cx, MacroAssembler& masm,
                                   const VMFunctionData& f, DynFn nativeFun,
                                   uint32_t* wrapperOffset) {
  *wrapperOffset = startTrampolineCode(masm);

  // Avoid conflicts with argument registers while discarding the result after
  // the function call.
  AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

  static_assert(
      (Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
      "Wrapper register set must be a superset of the Volatile register set.");

  // Unlike on other platforms, it is the responsibility of the VM *callee* to
  // push the return address, while the caller must ensure that the address
  // is stored in lr on entry. This allows the VM wrapper to work with both
  // direct calls and tail calls.
  masm.push(lr);

  // First argument is the JSContext.
  Register reg_cx = IntArgReg0;
  regs.take(reg_cx);

  // Stack is:
  //    ... frame ...
  //  +12 [args]
  //  +8  descriptor
  //  +0  returnAddress (pushed by this function, caller sets as lr)
  //
  //  We're aligned to an exit frame, so link it up.
  masm.loadJSContext(reg_cx);
  masm.enterExitFrame(reg_cx, regs.getAny(), &f);

  // Save the current stack pointer as the base for copying arguments.
  Register argsBase = InvalidReg;
  if (f.explicitArgs) {
    // argsBase can't be an argument register. Bad things would happen if
    // the MoveResolver didn't throw an assertion failure first.
    argsBase = r8;
    regs.take(argsBase);
    masm.Add(ARMRegister(argsBase, 64), masm.GetStackPointer64(),
             Operand(ExitFrameLayout::SizeWithFooter()));
  }

  // Reserve space for any outparameter.
  Register outReg = InvalidReg;
  switch (f.outParam) {
    case Type_Value:
      outReg = regs.takeAny();
      masm.reserveStack(sizeof(Value));
      masm.moveStackPtrTo(outReg);
      break;

    case Type_Handle:
      outReg = regs.takeAny();
      masm.PushEmptyRooted(f.outParamRootType);
      masm.moveStackPtrTo(outReg);
      break;

    case Type_Int32:
    case Type_Bool:
      outReg = regs.takeAny();
      masm.reserveStack(sizeof(int64_t));
      masm.moveStackPtrTo(outReg);
      break;

    case Type_Double:
      outReg = regs.takeAny();
      masm.reserveStack(sizeof(double));
      masm.moveStackPtrTo(outReg);
      break;

    case Type_Pointer:
      outReg = regs.takeAny();
      masm.reserveStack(sizeof(uintptr_t));
      masm.moveStackPtrTo(outReg);
      break;

    default:
      MOZ_ASSERT(f.outParam == Type_Void);
      break;
  }

  if (!generateTLEnterVM(masm, f)) {
    return false;
  }

  masm.setupUnalignedABICall(regs.getAny());
  masm.passABIArg(reg_cx);

  size_t argDisp = 0;

  // Copy arguments.
  for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
    switch (f.argProperties(explicitArg)) {
      case VMFunctionData::WordByValue:
        masm.passABIArg(MoveOperand(argsBase, argDisp),
                        (f.argPassedInFloatReg(explicitArg) ? MoveOp::DOUBLE
                                                            : MoveOp::GENERAL));
        argDisp += sizeof(void*);
        break;

      case VMFunctionData::WordByRef:
        masm.passABIArg(
            MoveOperand(argsBase, argDisp, MoveOperand::EFFECTIVE_ADDRESS),
            MoveOp::GENERAL);
        argDisp += sizeof(void*);
        break;

      case VMFunctionData::DoubleByValue:
      case VMFunctionData::DoubleByRef:
        MOZ_CRASH("NYI: AArch64 callVM should not be used with 128bit values.");
    }
  }

  // Copy the semi-implicit outparam, if any.
  // It is not a C++-abi outparam, which would get passed in the
  // outparam register, but a real parameter to the function, which
  // was stack-allocated above.
  if (outReg != InvalidReg) {
    masm.passABIArg(outReg);
  }

  masm.callWithABI(nativeFun, MoveOp::GENERAL,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  if (!generateTLExitVM(masm, f)) {
    return false;
  }

  // SP is used to transfer stack across call boundaries.
  masm.initPseudoStackPtr();

  // Test for failure.
  switch (f.failType()) {
    case Type_Object:
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

  // Load the outparam and free any allocated stack.
  switch (f.outParam) {
    case Type_Value:
      masm.Ldr(ARMRegister(JSReturnReg, 64),
               MemOperand(masm.GetStackPointer64()));
      masm.freeStack(sizeof(Value));
      break;

    case Type_Handle:
      masm.popRooted(f.outParamRootType, ReturnReg, JSReturnOperand);
      break;

    case Type_Int32:
      masm.Ldr(ARMRegister(ReturnReg, 32),
               MemOperand(masm.GetStackPointer64()));
      masm.freeStack(sizeof(int64_t));
      break;

    case Type_Bool:
      masm.Ldrb(ARMRegister(ReturnReg, 32),
                MemOperand(masm.GetStackPointer64()));
      masm.freeStack(sizeof(int64_t));
      break;

    case Type_Double:
      MOZ_ASSERT(JitOptions.supportsFloatingPoint);
      masm.Ldr(ARMFPRegister(ReturnDoubleReg, 64),
               MemOperand(masm.GetStackPointer64()));
      masm.freeStack(sizeof(double));
      break;

    case Type_Pointer:
      masm.Ldr(ARMRegister(ReturnReg, 64),
               MemOperand(masm.GetStackPointer64()));
      masm.freeStack(sizeof(uintptr_t));
      break;

    default:
      MOZ_ASSERT(f.outParam == Type_Void);
      break;
  }

  // Until C++ code is instrumented against Spectre, prevent speculative
  // execution from returning any private data.
  if (f.returnsData() && JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }

  masm.leaveExitFrame();
  masm.retn(Imm32(sizeof(ExitFrameLayout) +
                  f.explicitStackSlots() * sizeof(void*) +
                  f.extraValuesToPop * sizeof(Value)));

  return true;
}

uint32_t JitRuntime::generatePreBarrier(JSContext* cx, MacroAssembler& masm,
                                        MIRType type) {
  uint32_t offset = startTrampolineCode(masm);

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

  LiveRegisterSet regs =
      LiveRegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                      FloatRegisterSet(FloatRegisters::VolatileMask));

  // Also preserve the return address.
  regs.add(lr);

  masm.PushRegsInMask(regs);

  masm.movePtr(ImmPtr(cx->runtime()), r3);

  masm.setupUnalignedABICall(r0);
  masm.passABIArg(r3);
  masm.passABIArg(PreBarrierReg);
  masm.callWithABI(JitPreWriteBarrier(type));

  // Pop the volatile regs and restore LR.
  masm.PopRegsInMask(regs);
  masm.abiret();

  masm.bind(&noBarrier);
  masm.pop(temp3);
  masm.pop(temp2);
  masm.pop(temp1);
  masm.abiret();

  return offset;
}

void JitRuntime::generateExceptionTailStub(MacroAssembler& masm,
                                           Label* profilerExitTail) {
  exceptionTailOffset_ = startTrampolineCode(masm);

  masm.bind(masm.failureLabel());
  masm.handleFailureWithHandlerTail(profilerExitTail);
}

void JitRuntime::generateBailoutTailStub(MacroAssembler& masm,
                                         Label* bailoutTail) {
  bailoutTailOffset_ = startTrampolineCode(masm);
  masm.bind(bailoutTail);

  masm.generateBailoutTail(r1, r2);
}

void JitRuntime::generateProfilerExitFrameTailStub(MacroAssembler& masm,
                                                   Label* profilerExitTail) {
  profilerExitFrameTailOffset_ = startTrampolineCode(masm);
  masm.bind(profilerExitTail);

  Register scratch1 = r8;
  Register scratch2 = r9;
  Register scratch3 = r10;
  Register scratch4 = r11;

  //
  // The code generated below expects that the current stack pointer points
  // to an Ion or Baseline frame, at the state it would be immediately
  // before a ret().  Thus, after this stub's business is done, it executes
  // a ret() and returns directly to the caller script, on behalf of the
  // callee script that jumped to this code.
  //
  // Thus the expected stack is:
  //
  //                                   StackPointer ----+
  //                                                    v
  // ..., ActualArgc, CalleeToken, Descriptor, ReturnAddr
  // MEM-HI                                       MEM-LOW
  //
  //
  // The generated jitcode is responsible for overwriting the
  // jitActivation->lastProfilingFrame field with a pointer to the previous
  // Ion or Baseline jit-frame that was pushed before this one. It is also
  // responsible for overwriting jitActivation->lastProfilingCallSite with
  // the return address into that frame.  The frame could either be an
  // immediate "caller" frame, or it could be a frame in a previous
  // JitActivation (if the current frame was entered from C++, and the C++
  // was entered by some caller jit-frame further down the stack).
  //
  // So this jitcode is responsible for "walking up" the jit stack, finding
  // the previous Ion or Baseline JS frame, and storing its address and the
  // return address into the appropriate fields on the current jitActivation.
  //
  // There are a fixed number of different path types that can lead to the
  // current frame, which is either a baseline or ion frame:
  //
  // <Baseline-Or-Ion>
  // ^
  // |
  // ^--- Ion
  // |
  // ^--- Baseline Stub <---- Baseline
  // |
  // ^--- Argument Rectifier
  // |    ^
  // |    |
  // |    ^--- Ion
  // |    |
  // |    ^--- Baseline Stub <---- Baseline
  // |
  // ^--- Entry Frame (From C++)
  //
  Register actReg = scratch4;
  masm.loadJSContext(actReg);
  masm.loadPtr(Address(actReg, offsetof(JSContext, profilingActivation_)),
               actReg);

  Address lastProfilingFrame(actReg,
                             JitActivation::offsetOfLastProfilingFrame());
  Address lastProfilingCallSite(actReg,
                                JitActivation::offsetOfLastProfilingCallSite());

#ifdef DEBUG
  // Ensure that frame we are exiting is current lastProfilingFrame
  {
    masm.loadPtr(lastProfilingFrame, scratch1);
    Label checkOk;
    masm.branchPtr(Assembler::Equal, scratch1, ImmWord(0), &checkOk);
    masm.branchStackPtr(Assembler::Equal, scratch1, &checkOk);
    masm.assumeUnreachable(
        "Mismatch between stored lastProfilingFrame and current stack "
        "pointer.");
    masm.bind(&checkOk);
  }
#endif

  // Load the frame descriptor into |scratch1|, figure out what to do depending
  // on its type.
  masm.loadPtr(
      Address(masm.getStackPointer(), JitFrameLayout::offsetOfDescriptor()),
      scratch1);

  // Going into the conditionals, we will have:
  //      FrameDescriptor.size in scratch1
  //      FrameDescriptor.type in scratch2
  masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch1, scratch2);
  masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch1);

  // Handling of each case is dependent on FrameDescriptor.type
  Label handle_IonJS;
  Label handle_BaselineStub;
  Label handle_Rectifier;
  Label handle_IonICCall;
  Label handle_Entry;
  Label end;

  masm.branch32(Assembler::Equal, scratch2, Imm32(FrameType::IonJS),
                &handle_IonJS);
  masm.branch32(Assembler::Equal, scratch2, Imm32(FrameType::BaselineJS),
                &handle_IonJS);
  masm.branch32(Assembler::Equal, scratch2, Imm32(FrameType::BaselineStub),
                &handle_BaselineStub);
  masm.branch32(Assembler::Equal, scratch2, Imm32(FrameType::Rectifier),
                &handle_Rectifier);
  masm.branch32(Assembler::Equal, scratch2, Imm32(FrameType::IonICCall),
                &handle_IonICCall);
  masm.branch32(Assembler::Equal, scratch2, Imm32(FrameType::CppToJSJit),
                &handle_Entry);

  // The WasmToJSJit is just another kind of entry.
  masm.branch32(Assembler::Equal, scratch2, Imm32(FrameType::WasmToJSJit),
                &handle_Entry);

  masm.assumeUnreachable(
      "Invalid caller frame type when exiting from Ion frame.");

  //
  // FrameType::IonJS
  //
  // Stack layout:
  //                  ...
  //                  Ion-Descriptor
  //     Prev-FP ---> Ion-ReturnAddr
  //                  ... previous frame data ... |- Descriptor.Size
  //                  ... arguments ...           |
  //                  ActualArgc          |
  //                  CalleeToken         |- JitFrameLayout::Size()
  //                  Descriptor          |
  //        FP -----> ReturnAddr          |
  //
  masm.bind(&handle_IonJS);
  {
    // |scratch1| contains Descriptor.size

    // returning directly to an IonJS frame.  Store return addr to frame
    // in lastProfilingCallSite.
    masm.loadPtr(Address(masm.getStackPointer(),
                         JitFrameLayout::offsetOfReturnAddress()),
                 scratch2);
    masm.storePtr(scratch2, lastProfilingCallSite);

    // Store return frame in lastProfilingFrame.
    // scratch2 := masm.getStackPointer() + Descriptor.size*1 +
    //             JitFrameLayout::Size();
    masm.Add(ARMRegister(scratch2, 64), masm.GetStackPointer64(),
             ARMRegister(scratch1, 64));
    masm.syncStackPtr();
    masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2, scratch2);
    masm.storePtr(scratch2, lastProfilingFrame);
    masm.ret();
  }

  //
  // FrameType::BaselineStub
  //
  // Look past the stub and store the frame pointer to
  // the baselineJS frame prior to it.
  //
  // Stack layout:
  //              ...
  //              BL-Descriptor
  // Prev-FP ---> BL-ReturnAddr
  //      +-----> BL-PrevFramePointer
  //      |       ... BL-FrameData ...
  //      |       BLStub-Descriptor
  //      |       BLStub-ReturnAddr
  //      |       BLStub-StubPointer          |
  //      +------ BLStub-SavedFramePointer    |- Descriptor.Size
  //              ... arguments ...           |
  //              ActualArgc          |
  //              CalleeToken         |- JitFrameLayout::Size()
  //              Descriptor          |
  //    FP -----> ReturnAddr          |
  //
  // We take advantage of the fact that the stub frame saves the frame
  // pointer pointing to the baseline frame, so a bunch of calculation can
  // be avoided.
  //
  masm.bind(&handle_BaselineStub);
  {
    masm.Add(ARMRegister(scratch3, 64), masm.GetStackPointer64(),
             ARMRegister(scratch1, 64));
    masm.syncStackPtr();
    Address stubFrameReturnAddr(
        scratch3, JitFrameLayout::Size() +
                      BaselineStubFrameLayout::offsetOfReturnAddress());
    masm.loadPtr(stubFrameReturnAddr, scratch2);
    masm.storePtr(scratch2, lastProfilingCallSite);

    Address stubFrameSavedFramePtr(
        scratch3, JitFrameLayout::Size() - (2 * sizeof(void*)));
    masm.loadPtr(stubFrameSavedFramePtr, scratch2);
    masm.addPtr(Imm32(sizeof(void*)), scratch2);  // Skip past BL-PrevFramePtr.
    masm.storePtr(scratch2, lastProfilingFrame);
    masm.ret();
  }

  //
  // FrameType::Rectifier
  //
  // The rectifier frame can be preceded by either an IonJS, a BaselineStub,
  // or a CppToJSJit/WasmToJSJit frame.
  //
  // Stack layout if caller of rectifier was Ion or CppToJSJit/WasmToJSJit:
  //
  //              Ion-Descriptor
  //              Ion-ReturnAddr
  //              ... ion frame data ... |- Rect-Descriptor.Size
  //              < COMMON LAYOUT >
  //
  // Stack layout if caller of rectifier was Baseline:
  //
  //              BL-Descriptor
  // Prev-FP ---> BL-ReturnAddr
  //      +-----> BL-SavedFramePointer
  //      |       ... baseline frame data ...
  //      |       BLStub-Descriptor
  //      |       BLStub-ReturnAddr
  //      |       BLStub-StubPointer          |
  //      +------ BLStub-SavedFramePointer    |- Rect-Descriptor.Size
  //              ... args to rectifier ...   |
  //              < COMMON LAYOUT >
  //
  // Common stack layout:
  //
  //              ActualArgc          |
  //              CalleeToken         |- IonRectitiferFrameLayout::Size()
  //              Rect-Descriptor     |
  //              Rect-ReturnAddr     |
  //              ... rectifier data & args ... |- Descriptor.Size
  //              ActualArgc      |
  //              CalleeToken     |- JitFrameLayout::Size()
  //              Descriptor      |
  //    FP -----> ReturnAddr      |
  //
  masm.bind(&handle_Rectifier);
  {
    // scratch2 := StackPointer + Descriptor.size*1 + JitFrameLayout::Size();
    masm.Add(ARMRegister(scratch2, 64), masm.GetStackPointer64(),
             ARMRegister(scratch1, 64));
    masm.syncStackPtr();
    masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2);
    masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfDescriptor()),
                 scratch3);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch3, scratch1);
    masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch3);

    // Now |scratch1| contains Rect-Descriptor.Size
    // and |scratch2| points to Rectifier frame
    // and |scratch3| contains Rect-Descriptor.Type

    masm.assertRectifierFrameParentType(scratch3);

    // Check for either Ion or BaselineStub frame.
    Label notIonFrame;
    masm.branch32(Assembler::NotEqual, scratch3, Imm32(FrameType::IonJS),
                  &notIonFrame);

    // Handle Rectifier <- IonJS
    // scratch3 := RectFrame[ReturnAddr]
    masm.loadPtr(
        Address(scratch2, RectifierFrameLayout::offsetOfReturnAddress()),
        scratch3);
    masm.storePtr(scratch3, lastProfilingCallSite);

    // scratch3 := RectFrame + Rect-Descriptor.Size +
    //             RectifierFrameLayout::Size()
    masm.addPtr(scratch2, scratch1, scratch3);
    masm.addPtr(Imm32(RectifierFrameLayout::Size()), scratch3);
    masm.storePtr(scratch3, lastProfilingFrame);
    masm.ret();

    masm.bind(&notIonFrame);

    // Check for either BaselineStub or a CppToJSJit/WasmToJSJit entry
    // frame.
    masm.branch32(Assembler::NotEqual, scratch3, Imm32(FrameType::BaselineStub),
                  &handle_Entry);

    // Handle Rectifier <- BaselineStub <- BaselineJS
    masm.addPtr(scratch2, scratch1, scratch3);
    Address stubFrameReturnAddr(
        scratch3, RectifierFrameLayout::Size() +
                      BaselineStubFrameLayout::offsetOfReturnAddress());
    masm.loadPtr(stubFrameReturnAddr, scratch2);
    masm.storePtr(scratch2, lastProfilingCallSite);

    Address stubFrameSavedFramePtr(
        scratch3, RectifierFrameLayout::Size() - (2 * sizeof(void*)));
    masm.loadPtr(stubFrameSavedFramePtr, scratch2);
    masm.addPtr(Imm32(sizeof(void*)), scratch2);
    masm.storePtr(scratch2, lastProfilingFrame);
    masm.ret();
  }

  // FrameType::IonICCall
  //
  // The caller is always an IonJS frame.
  //
  //              Ion-Descriptor
  //              Ion-ReturnAddr
  //              ... ion frame data ... |- CallFrame-Descriptor.Size
  //              StubCode               |
  //              ICCallFrame-Descriptor |- IonICCallFrameLayout::Size()
  //              ICCallFrame-ReturnAddr |
  //              ... call frame data & args ... |- Descriptor.Size
  //              ActualArgc      |
  //              CalleeToken     |- JitFrameLayout::Size()
  //              Descriptor      |
  //    FP -----> ReturnAddr      |
  masm.bind(&handle_IonICCall);
  {
    // scratch2 := StackPointer + Descriptor.size + JitFrameLayout::Size()
    masm.Add(ARMRegister(scratch2, 64), masm.GetStackPointer64(),
             ARMRegister(scratch1, 64));
    masm.syncStackPtr();
    masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2);

    // scratch3 := ICCallFrame-Descriptor.Size
    masm.loadPtr(Address(scratch2, IonICCallFrameLayout::offsetOfDescriptor()),
                 scratch3);
#ifdef DEBUG
    // Assert previous frame is an IonJS frame.
    masm.movePtr(scratch3, scratch1);
    masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch1);
    {
      Label checkOk;
      masm.branch32(Assembler::Equal, scratch1, Imm32(FrameType::IonJS),
                    &checkOk);
      masm.assumeUnreachable("IonICCall frame must be preceded by IonJS frame");
      masm.bind(&checkOk);
    }
#endif
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch3);

    // lastProfilingCallSite := ICCallFrame-ReturnAddr
    masm.loadPtr(
        Address(scratch2, IonICCallFrameLayout::offsetOfReturnAddress()),
        scratch1);
    masm.storePtr(scratch1, lastProfilingCallSite);

    // lastProfilingFrame := ICCallFrame + ICCallFrame-Descriptor.Size +
    //                       IonICCallFrameLayout::Size()
    masm.addPtr(scratch2, scratch3, scratch1);
    masm.addPtr(Imm32(IonICCallFrameLayout::Size()), scratch1);
    masm.storePtr(scratch1, lastProfilingFrame);
    masm.ret();
  }

  //
  // FrameType::CppToJSJit / FrameType::WasmToJSJit
  //
  // If at an entry frame, store null into both fields.
  // A fast-path wasm->jit transition frame is an entry frame from the point
  // of view of the JIT.
  //
  masm.bind(&handle_Entry);
  {
    masm.movePtr(ImmPtr(nullptr), scratch1);
    masm.storePtr(scratch1, lastProfilingCallSite);
    masm.storePtr(scratch1, lastProfilingFrame);
    masm.ret();
  }
}
