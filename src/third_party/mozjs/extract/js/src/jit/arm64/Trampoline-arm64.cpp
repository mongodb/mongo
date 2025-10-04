/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/SharedICHelpers-arm64.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/PerfSpewer.h"
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
  AutoCreatedBy acb(masm, "JitRuntime::generateEnterJIT");

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

  // Save return address and old frame pointer; set new frame pointer.
  masm.push(r30, r29);
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
  //  8    - calleeToken
  //  8    - frameDescriptor (16-byte aligned)
  //  8    - returnAddress
  //  8    - frame pointer (16-byte aligned, pushed by callee)

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

  // Push the calleeToken and the frame descriptor.
  // The result address is used to store the actual number of arguments
  // without adding an argument to EnterJIT.
  {
    vixl::UseScratchRegisterScope temps(&masm.asVIXL());
    MOZ_ASSERT(temps.IsAvailable(ScratchReg64));  // ip0
    temps.Exclude(ScratchReg64);
    Register scratch = ScratchReg64.asUnsized();
    masm.push(reg_callee);

    // Push the descriptor.
    masm.unboxInt32(Address(reg_vp, 0x0), scratch);
    masm.PushFrameDescriptorForJitCall(FrameType::CppToJSJit, scratch, scratch);
  }
  masm.checkStackAlignment();

  Label osrReturnPoint;
  {
    // Check for Interpreter -> Baseline OSR.

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT(!regs.has(FramePointer));
    regs.take(OsrFrameReg);
    regs.take(reg_code);
    regs.take(reg_osrNStack);
    MOZ_ASSERT(!regs.has(ReturnReg), "ReturnReg matches reg_code");

    Label notOsr;
    masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

    Register scratch = regs.takeAny();

    // Frame prologue.
    masm.Adr(ARMRegister(scratch, 64), &osrReturnPoint);
    masm.push(scratch, FramePointer);
    masm.moveStackPtrTo(FramePointer);

    // Reserve frame.
    masm.subFromStackPtr(Imm32(BaselineFrame::Size()));

    Register framePtrScratch = regs.takeAny();
    masm.touchFrameValues(reg_osrNStack, scratch, framePtrScratch);
    masm.moveStackPtrTo(framePtrScratch);

    // Reserve space for locals and stack values.
    // scratch = num_stack_values * sizeof(Value).
    masm.Lsl(ARMRegister(scratch, 32), ARMRegister(reg_osrNStack, 32), 3);
    masm.subFromStackPtr(scratch);

    // Enter exit frame.
    masm.pushFrameDescriptor(FrameType::BaselineJS);
    masm.push(xzr);  // Push xzr for a fake return address.
    masm.push(FramePointer);
    // No GC things to mark: push a bare token.
    masm.loadJSContext(scratch);
    masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    masm.push(reg_code);

    // Initialize the frame, including filling in the slots.
    using Fn = bool (*)(BaselineFrame* frame, InterpreterFrame* interpFrame,
                        uint32_t numStackValues);
    masm.setupUnalignedABICall(r19);
    masm.passABIArg(framePtrScratch);  // BaselineFrame.
    masm.passABIArg(reg_osrFrame);     // InterpreterFrame.
    masm.passABIArg(reg_osrNStack);
    masm.callWithABI<Fn, jit::InitBaselineFrameForOsr>(
        ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    masm.pop(scratch);
    MOZ_ASSERT(scratch != ReturnReg);

    masm.addToStackPtr(Imm32(ExitFrameLayout::SizeWithFooter()));

    Label error;
    masm.branchIfFalseBool(ReturnReg, &error);

    // If OSR-ing, then emit instrumentation for setting lastProfilerFrame
    // if profiler instrumentation is enabled.
    {
      Label skipProfilingInstrumentation;
      AbsoluteAddress addressOfEnabled(
          cx->runtime()->geckoProfiler().addressOfEnabled());
      masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                    &skipProfilingInstrumentation);
      masm.profilerEnterFrame(FramePointer, regs.getAny());
      masm.bind(&skipProfilingInstrumentation);
    }

    masm.jump(scratch);

    // OOM: frame epilogue, load error value, discard return address and return.
    masm.bind(&error);
    masm.moveToStackPtr(FramePointer);
    masm.pop(FramePointer);
    masm.addToStackPtr(Imm32(sizeof(uintptr_t)));  // Return address.
    masm.syncStackPtr();
    masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    masm.B(&osrReturnPoint);

    masm.bind(&notOsr);
    masm.movePtr(reg_scope, R1_);
  }

  // The callee will push the return address and frame pointer on the stack,
  // thus we check that the stack would be aligned once the call is complete.
  masm.assertStackAlignment(JitStackAlignment, 2 * sizeof(uintptr_t));

  // Call function.
  // Since AArch64 doesn't have the pc register available, the callee must push
  // lr.
  masm.callJitNoProfiler(reg_code);

  // Interpreter -> Baseline OSR will return here.
  masm.bind(&osrReturnPoint);

  // Discard arguments and padding. Set sp to the address of the saved
  // registers. In debug builds we have to include the two stack canaries
  // checked below.
#ifdef DEBUG
  static constexpr size_t SavedRegSize = 22 * sizeof(void*);
#else
  static constexpr size_t SavedRegSize = 20 * sizeof(void*);
#endif
  masm.computeEffectiveAddress(Address(FramePointer, -int32_t(SavedRegSize)),
                               masm.getStackPointer());

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
  masm.pop(r29, r30);

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
  AutoCreatedBy acb(masm, "JitRuntime::generateInvalidator");

  invalidatorOffset_ = startTrampolineCode(masm);

  // The InvalidationBailoutStack saved in r0 must be:
  // - osiPointReturnAddress_
  // - ionScript_  (pushed by CodeGeneratorARM64::generateInvalidateEpilogue())
  // - regs_  (pushed here)
  // - fpregs_  (pushed here) [=r0]
  PushRegisterDump(masm);
  masm.moveStackPtrTo(r0);

  // Reserve space for InvalidationBailout's bailoutInfo outparam.
  masm.Sub(x1, masm.GetStackPointer64(), Operand(sizeof(void*)));
  masm.moveToStackPtr(r1);

  using Fn = bool (*)(InvalidationBailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(r10);
  masm.passABIArg(r0);
  masm.passABIArg(r1);

  masm.callWithABI<Fn, InvalidationBailout>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(r2);  // Get the bailoutInfo outparam.

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

  // Save the return address for later.
  masm.push(lr);

  // Frame prologue.
  //
  // NOTE: if this changes, fix the Baseline bailout code too!
  // See BaselineStackBuilder::calculatePrevFramePtr and
  // BaselineStackBuilder::buildRectifierFrame (in BaselineBailouts.cpp).
  masm.push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  // Load the information that the rectifier needs from the stack.
  masm.loadNumActualArgs(FramePointer, r0);
  masm.loadPtr(
      Address(FramePointer, RectifierFrameLayout::offsetOfCalleeToken()), r1);

  // Extract a JSFunction pointer from the callee token and keep the
  // intermediary to avoid later recalculation.
  masm.And(x5, x1, Operand(CalleeTokenMask));

  // Get the arguments from the function object.
  masm.loadFunctionArgCount(x5.asUnsized(), x6.asUnsized());

  static_assert(CalleeToken_FunctionConstructing == 0x1,
                "Constructing must be low-order bit");
  masm.And(x4, x1, Operand(CalleeToken_FunctionConstructing));
  masm.Add(x7, x6, x4);

  // Copy the number of actual arguments into r8.
  masm.mov(r0, r8);

  // Calculate the position that our arguments are at before sp gets modified.
  masm.Add(x3, masm.GetStackPointer64(), Operand(x8, vixl::LSL, 3));
  masm.Add(x3, x3, Operand(sizeof(RectifierFrameLayout)));

  // If the number of Values without |this| is even, push 8 padding bytes to
  // ensure the stack is 16-byte aligned.
  Label noPadding;
  masm.Tbnz(x7, 0, &noPadding);
  masm.asVIXL().Push(xzr);
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

  masm.push(r1);  // Callee token.
  masm.pushFrameDescriptorForJitCall(FrameType::Rectifier, r0, r0);

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

  masm.moveToStackPtr(FramePointer);
  masm.pop(FramePointer);
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

  using Fn = bool (*)(BailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(r2);
  masm.passABIArg(r0);
  masm.passABIArg(r1);
  masm.callWithABI<Fn, Bailout>(ABIType::General,
                                CheckUnsafeCallWithABI::DontCheckOther);

  // Get the bailoutInfo outparam.
  masm.pop(r2);

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

  // Avoid conflicts with argument registers while discarding the result after
  // the function call.
  AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

  static_assert(
      (Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
      "Wrapper register set must be a superset of the Volatile register set.");

  // The first argument is the JSContext.
  Register reg_cx = IntArgReg0;
  regs.take(reg_cx);
  Register temp = regs.getAny();

  // On entry, the stack is:
  //   ... frame ...
  //  [args]
  //  descriptor
  //
  // Before we pass arguments (potentially pushing some of them on the stack),
  // we want:
  //  ... frame ...
  //  [args]
  //  descriptor           \
  //  return address       | <- exit frame
  //  saved frame pointer  /
  //  VM id                  <- exit frame footer
  //  [space for out-param, if necessary]]
  //  [alignment padding, if necessary]
  //
  // To minimize PSP overhead, we compute the final stack size and update the
  // stack pointer all in one go. Then we use the PSP to "push" the required
  // values into the pre-allocated stack space.
  size_t stackAdjustment = 0;

  // The descriptor was already pushed.
  stackAdjustment += ExitFrameLayout::SizeWithFooter() - sizeof(uintptr_t);
  stackAdjustment += f.sizeOfOutParamStackSlot();

  masm.SetStackPointer64(sp);

  // First, update the actual stack pointer to its final aligned value.
  masm.Sub(ARMRegister(temp, 64), masm.GetStackPointer64(),
           Operand(stackAdjustment));
  masm.And(sp, ARMRegister(temp, 64), ~(uint64_t(JitStackAlignment) - 1));

  // On link-register platforms, it is the responsibility of the VM *callee* to
  // push the return address, while the caller must ensure that the address
  // is stored in lr on entry. This allows the VM wrapper to work with both
  // direct calls and tail calls.
  masm.str(ARMRegister(lr, 64),
           MemOperand(PseudoStackPointer64, -8, vixl::PreIndex));

  // Push the frame pointer using the PSP.
  masm.str(ARMRegister(FramePointer, 64),
           MemOperand(PseudoStackPointer64, -8, vixl::PreIndex));

  // Because we've been moving the PSP as we fill in the frame, we can set the
  // frame pointer for this frame directly from the PSP.
  masm.movePtr(PseudoStackPointer, FramePointer);

  masm.loadJSContext(reg_cx);

  // Finish the exit frame. See MacroAssembler::enterExitFrame.

  // linkExitFrame
  masm.loadPtr(Address(reg_cx, JSContext::offsetOfActivation()), temp);
  masm.storePtr(FramePointer,
                Address(temp, JitActivation::offsetOfPackedExitFP()));

  // Push `ExitFrameType::VMFunction + VMFunctionId`
  uint32_t type = uint32_t(ExitFrameType::VMFunction) + uint32_t(id);
  masm.move32(Imm32(type), temp);
  masm.str(ARMRegister(temp, 64),
           MemOperand(PseudoStackPointer64, -8, vixl::PreIndex));

  // If the out parameter is a handle, initialize it to empty.
  // See MacroAssembler::reserveVMFunctionOutParamSpace and PushEmptyRooted.
  if (f.outParam == Type_Handle) {
    switch (f.outParamRootType) {
      case VMFunctionData::RootNone:
        MOZ_CRASH("Handle must have root type");
      case VMFunctionData::RootObject:
      case VMFunctionData::RootString:
      case VMFunctionData::RootCell:
      case VMFunctionData::RootBigInt:
        masm.str(xzr, MemOperand(PseudoStackPointer64, -8, vixl::PreIndex));
        break;
      case VMFunctionData::RootValue:
        masm.movePtr(ImmWord(UndefinedValue().asRawBits()), temp);
        masm.str(ARMRegister(temp, 64),
                 MemOperand(PseudoStackPointer64, -8, vixl::PreIndex));
        break;
      case VMFunctionData::RootId:
        masm.movePtr(ImmWord(JS::PropertyKey::Void().asRawBits()), temp);
        masm.str(ARMRegister(temp, 64),
                 MemOperand(PseudoStackPointer64, -8, vixl::PreIndex));
    }
  }

  // Now that we've filled in the stack frame, synchronize the PSP with the
  // real stack pointer and return to PSP-mode while we pass arguments.
  masm.moveStackPtrTo(PseudoStackPointer);
  masm.SetStackPointer64(PseudoStackPointer64);

  MOZ_ASSERT(masm.framePushed() == 0);
  masm.setupAlignedABICall();
  masm.passABIArg(reg_cx);

  size_t argDisp = ExitFrameLayout::Size();

  // Copy arguments.
  for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
    switch (f.argProperties(explicitArg)) {
      case VMFunctionData::WordByValue:
        masm.passABIArg(
            MoveOperand(FramePointer, argDisp),
            (f.argPassedInFloatReg(explicitArg) ? ABIType::Float64
                                                : ABIType::General));
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
        MOZ_CRASH("NYI: AArch64 callVM should not be used with 128bit values.");
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

  // Pop frame and restore frame pointer. We call Mov here directly instead
  // of `moveToStackPtr` to avoid a syncStackPtr. The stack pointer will be
  // synchronized as part of retn, after adjusting the PSP.
  masm.Mov(masm.GetStackPointer64(), ARMRegister(FramePointer, 64));
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

void JitRuntime::generateBailoutTailStub(MacroAssembler& masm,
                                         Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutTailStub");

  masm.bind(bailoutTail);
  masm.generateBailoutTail(r1, r2);
}
