/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/Linker.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/arm64/SharedICHelpers-arm64.h"
#include "jit/VMFunctions.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

// All registers to save and restore. This includes the stack pointer, since we
// use the ability to reference register values on the stack by index.
static const LiveRegisterSet AllRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllMask & ~(1 << 31 | 1 << 30 | 1 << 29| 1 << 28)),
                FloatRegisterSet(FloatRegisters::AllMask));

/* This method generates a trampoline on ARM64 for a c++ function with
 * the following signature:
 *   bool blah(void* code, int argc, Value* argv, JSObject* scopeChain, Value* vp)
 *   ...using standard AArch64 calling convention
 */
JitCode*
JitRuntime::generateEnterJIT(JSContext* cx, EnterJitType type)
{
    MacroAssembler masm(cx);

    const Register reg_code      = IntArgReg0; // EnterJitData::jitcode.
    const Register reg_argc      = IntArgReg1; // EnterJitData::maxArgc.
    const Register reg_argv      = IntArgReg2; // EnterJitData::maxArgv.
    const Register reg_osrFrame  = IntArgReg3; // EnterJitData::osrFrame.
    const Register reg_callee    = IntArgReg4; // EnterJitData::calleeToken.
    const Register reg_scope     = IntArgReg5; // EnterJitData::scopeChain.
    const Register reg_osrNStack = IntArgReg6; // EnterJitData::osrNumStackValues.
    const Register reg_vp        = IntArgReg7; // Address of EnterJitData::result.

    MOZ_ASSERT(OsrFrameReg == IntArgReg3);

    // During the pushes below, use the normal stack pointer.
    masm.SetStackPointer64(sp);

    // Save old frame pointer and return address; set new frame pointer.
    masm.push(r29, r30);
    masm.moveStackPtrTo(r29);

    // Save callee-save integer registers.
    // Also save x7 (reg_vp) and x30 (lr), for use later.
    masm.push(r19, r20, r21, r22);
    masm.push(r23, r24, r25, r26);
    masm.push(r27, r28, r7,  r30);

    // Save callee-save floating-point registers.
    // AArch64 ABI specifies that only the lower 64 bits must be saved.
    masm.push(d8,  d9,  d10, d11);
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
    // use the PseudoStackPointer: since the amount of data pushed is precalculated,
    // we can just allocate the whole frame header at once and index off sp.
    // This will save a significant number of instructions where Push() updates sp.
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
        masm.branchTest32(Assembler::Zero, reg_callee, constructingToken, &noNewTarget);
        masm.add32(Imm32(1), reg_argc);
        masm.bind(&noNewTarget);
    }

    // JitFrameLayout is as follows (higher is higher in memory):
    //  N*8  - [ JS argument vector ] (base 16-byte aligned)
    //  8    - numActualArgs
    //  8    - calleeToken (16-byte aligned)
    //  8    - frameDescriptor
    //  8    - returnAddress (16-byte aligned, pushed by callee)

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
        // Since we're using PostIndex Str below, this is necessary to avoid overwriting
        // the SPS mark pushed above.
        masm.subFromStackPtr(Imm32(8));

        // sp -= 8 * argc
        masm.Sub(PseudoStackPointer64, PseudoStackPointer64, Operand(tmp_argc, vixl::SXTX, 3));

        // Give sp 16-byte alignment and sync stack pointers.
        masm.andToStackPtr(Imm32(~0xff));
        masm.moveStackPtrTo(tmp_sp.asUnsized());

        masm.branchTestPtr(Assembler::Zero, reg_argc, reg_argc, &noArguments);

        // Begin argument-pushing loop.
        // This could be optimized using Ldp and Stp.
        {
            masm.bind(&loopHead);

            // Load an argument from argv, then increment argv by 8.
            masm.Ldr(x24, MemOperand(ARMRegister(reg_argv, 64), Operand(8), vixl::PostIndex));

            // Store the argument to tmp_sp, then increment tmp_sp by 8.
            masm.Str(x24, MemOperand(tmp_sp, Operand(8), vixl::PostIndex));

            // Set the condition codes for |cmp tmp_argc, 2| (using the old value).
            masm.Subs(tmp_argc, tmp_argc, Operand(1));

            // Branch if arguments remain.
            masm.B(&loopHead, vixl::Condition::ge);
        }

        masm.bind(&noArguments);
    }
    masm.checkStackAlignment();

    // Push the number of actual arguments and the calleeToken.
    // The result address is used to store the actual number of arguments
    // without adding an argument to EnterJIT.
    masm.unboxInt32(Address(reg_vp, 0x0), ip0);
    masm.push(ip0, reg_callee);
    masm.checkStackAlignment();

    // Calculate the number of bytes pushed so far.
    masm.subStackPtrFrom(r19);

    // Push the frameDescriptor.
    masm.makeFrameDescriptor(r19, JitFrame_Entry);
    masm.Push(r19);

    Label osrReturnPoint;
    if (type == EnterJitBaseline) {
        // Check for OSR.
        Label notOsr;
        masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

        // Push return address and previous frame pointer.
        masm.Adr(ScratchReg2_64, &osrReturnPoint);
        masm.push(ScratchReg2, BaselineFrameReg);

        // Reserve frame.
        masm.subFromStackPtr(Imm32(BaselineFrame::Size()));
        masm.moveStackPtrTo(BaselineFrameReg);

        // Reserve space for locals and stack values.
        masm.Lsl(w19, ARMRegister(reg_osrNStack, 32), 3); // w19 = num_stack_values * sizeof(Value).
        masm.subFromStackPtr(r19);

        // Enter exit frame.
        masm.addPtr(Imm32(BaselineFrame::Size() + BaselineFrame::FramePointerOffset), r19);
        masm.makeFrameDescriptor(r19, JitFrame_BaselineJS);
        masm.asVIXL().Push(x19, xzr); // Push xzr for a fake return address.
        // No GC things to mark: push a bare token.
        masm.enterFakeExitFrame(ExitFrameLayoutBareToken);

        masm.push(BaselineFrameReg, reg_code);

        // Initialize the frame, including filling in the slots.
        masm.setupUnalignedABICall(r19);
        masm.passABIArg(BaselineFrameReg); // BaselineFrame.
        masm.passABIArg(reg_osrFrame); // InterpreterFrame.
        masm.passABIArg(reg_osrNStack);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, jit::InitBaselineFrameForOsr));

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
        masm.Add(masm.GetStackPointer64(), BaselineFrameReg64, Operand(2 * sizeof(uintptr_t)));
        masm.syncStackPtr();
        masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
        masm.B(&osrReturnPoint);

        masm.bind(&notOsr);
        masm.movePtr(reg_scope, R1_);
    }

    // Call function.
    // Since AArch64 doesn't have the pc register available, the callee must push lr.
    masm.callJitNoProfiler(reg_code);

    // Baseline OSR will return here.
    if (type == EnterJitBaseline)
        masm.bind(&osrReturnPoint);

    // Return back to SP.
    masm.Pop(r19);
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
    masm.pop(d11, d10,  d9,  d8);

    // Restore callee-save integer registers.
    // Also restore x7 (reg_vp) and x30 (lr).
    masm.pop(r30, r7,  r28, r27);
    masm.pop(r26, r25, r24, r23);
    masm.pop(r22, r21, r20, r19);

    // Store return value (in JSReturnReg = x2 to just-popped reg_vp).
    masm.storeValue(JSReturnOperand, Address(reg_vp, 0));

    // Restore old frame pointer.
    masm.pop(r30, r29);

    // Return using the value popped into x30.
    masm.abiret();

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "EnterJIT");
#endif

    return code;
}

JitCode*
JitRuntime::generateInvalidator(JSContext* cx)
{
    MacroAssembler masm;

    masm.push(r0, r1, r2, r3);

    masm.PushRegsInMask(AllRegs);
    masm.moveStackPtrTo(r0);

    masm.Sub(x1, masm.GetStackPointer64(), Operand(sizeof(size_t)));
    masm.Sub(x2, masm.GetStackPointer64(), Operand(sizeof(size_t) + sizeof(void*)));
    masm.moveToStackPtr(r2);

    masm.setupUnalignedABICall(r10);
    masm.passABIArg(r0);
    masm.passABIArg(r1);
    masm.passABIArg(r2);

    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, InvalidationBailout));

    masm.pop(r2, r1);

    masm.Add(masm.GetStackPointer64(), masm.GetStackPointer64(), x1);
    masm.Add(masm.GetStackPointer64(), masm.GetStackPointer64(),
             Operand(sizeof(InvalidationBailoutStack)));
    masm.syncStackPtr();

    JitCode* bailoutTail = cx->runtime()->jitRuntime()->getBailoutTail();
    masm.branch(bailoutTail);

    Linker linker(masm);
    return linker.newCode<NoGC>(cx, OTHER_CODE);
}

JitCode*
JitRuntime::generateArgumentsRectifier(JSContext* cx, void** returnAddrOut)
{
    MacroAssembler masm;

    // Save the return address for later.
    masm.push(lr);

    // Load the information that the rectifier needs from the stack.
    masm.Ldr(w0, MemOperand(masm.GetStackPointer64(), RectifierFrameLayout::offsetOfNumActualArgs()));
    masm.Ldr(x1, MemOperand(masm.GetStackPointer64(), RectifierFrameLayout::offsetOfCalleeToken()));

    // Extract a JSFunction pointer from the callee token and keep the
    // intermediary to avoid later recalculation.
    masm.And(x5, x1, Operand(CalleeTokenMask));

    // Get the arguments from the function object.
    masm.Ldrh(x6, MemOperand(x5, JSFunction::offsetOfNargs()));

    static_assert(CalleeToken_FunctionConstructing == 0x1, "Constructing must be low-order bit");
    masm.And(x4, x1, Operand(CalleeToken_FunctionConstructing));
    masm.Add(x7, x6, x4);

    // Calculate the position that our arguments are at before sp gets modified.
    MOZ_ASSERT(ArgumentsRectifierReg == r8, "x8 used for argc in Arguments Rectifier");
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
    masm.moveValue(UndefinedValue(), r4);

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
    masm.makeFrameDescriptor(r6, JitFrame_Rectifier);

    masm.push(r0,  // Number of actual arguments.
              r1,  // Callee token.
              r6); // Frame descriptor.

    // Load the address of the code that is getting called.
    masm.Ldr(x3, MemOperand(x5, JSFunction::offsetOfNativeOrScript()));
    masm.loadBaselineOrIonRaw(r3, r3, nullptr);
    uint32_t returnOffset = masm.callJitNoProfiler(r3);

    // Clean up!
    // Get the size of the stack frame, and clean up the later fixed frame.
    masm.Ldr(x4, MemOperand(masm.GetStackPointer64(), 24, vixl::PostIndex));

    // Now that the size of the stack frame sans the fixed frame has been loaded,
    // add that onto the stack pointer.
    masm.Add(masm.GetStackPointer64(), masm.GetStackPointer64(),
             Operand(x4, vixl::LSR, FRAMESIZE_SHIFT));

    // Pop the return address from earlier and branch.
    masm.ret();

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

    if (returnAddrOut)
        *returnAddrOut = (void*) (code->raw() + returnOffset);

    return code;
}

static void
PushBailoutFrame(MacroAssembler& masm, uint32_t frameClass, Register spArg)
{
    // the stack should look like:
    // [IonFrame]
    // bailoutFrame.registersnapshot
    // bailoutFrame.fpsnapshot
    // bailoutFrame.snapshotOffset
    // bailoutFrame.frameSize

    // STEP 1a: Save our register sets to the stack so Bailout() can read
    // everything.
    // sp % 8 == 0

    // We don't have to push everything, but this is likely easier.
    // Setting regs_.
    masm.subFromStackPtr(Imm32(Registers::TotalPhys * sizeof(void*)));
    for (uint32_t i = 0; i < Registers::TotalPhys; i += 2) {
        masm.Stp(ARMRegister::XRegFromCode(i),
                 ARMRegister::XRegFromCode(i + 1),
                 MemOperand(masm.GetStackPointer64(), i * sizeof(void*)));
    }

    // Since our datastructures for stack inspection are compile-time fixed,
    // if there are only 16 double registers, then we need to reserve
    // space on the stack for the missing 16.
    masm.subFromStackPtr(Imm32(FloatRegisters::TotalPhys * sizeof(double)));
    for (uint32_t i = 0; i < FloatRegisters::TotalPhys; i += 2) {
        masm.Stp(ARMFPRegister::DRegFromCode(i),
                 ARMFPRegister::DRegFromCode(i + 1),
                 MemOperand(masm.GetStackPointer64(), i * sizeof(void*)));
    }

    // STEP 1b: Push both the "return address" of the function call (the address
    //          of the instruction after the call that we used to get here) as
    //          well as the callee token onto the stack. The return address is
    //          currently in r14. We will proceed by loading the callee token
    //          into a sacrificial register <= r14, then pushing both onto the
    //          stack.

    // Now place the frameClass onto the stack, via a register.
    masm.Mov(x9, frameClass);

    // And onto the stack. Since the stack is full, we need to put this one past
    // the end of the current stack. Sadly, the ABI says that we need to always
    // point to the lowest place that has been written. The OS is free to do
    // whatever it wants below sp.
    masm.push(r30, r9);
    masm.moveStackPtrTo(spArg);
}

static void
GenerateBailoutThunk(JSContext* cx, MacroAssembler& masm, uint32_t frameClass)
{
    PushBailoutFrame(masm, frameClass, r0);

    // SP % 8 == 4
    // STEP 1c: Call the bailout function, giving a pointer to the
    //          structure we just blitted onto the stack.
    // Make space for the BaselineBailoutInfo* outparam.
    const int sizeOfBailoutInfo = sizeof(void*) * 2;
    masm.reserveStack(sizeOfBailoutInfo);
    masm.moveStackPtrTo(r1);

    masm.setupUnalignedABICall(r2);
    masm.passABIArg(r0);
    masm.passABIArg(r1);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, Bailout));

    masm.Ldr(x2, MemOperand(masm.GetStackPointer64(), 0));
    masm.addToStackPtr(Imm32(sizeOfBailoutInfo));

    static const uint32_t BailoutDataSize = sizeof(void*) * Registers::Total +
                                            sizeof(double) * FloatRegisters::TotalPhys;

    if (frameClass == NO_FRAME_SIZE_CLASS_ID) {
        vixl::UseScratchRegisterScope temps(&masm.asVIXL());
        const ARMRegister scratch64 = temps.AcquireX();

        masm.Ldr(scratch64, MemOperand(masm.GetStackPointer64(), sizeof(uintptr_t)));
        masm.addToStackPtr(Imm32(BailoutDataSize + 32));
        masm.addToStackPtr(scratch64.asUnsized());
    } else {
        uint32_t frameSize = FrameSizeClass::FromClass(frameClass).frameSize();
        masm.addToStackPtr(Imm32(frameSize + BailoutDataSize + sizeof(void*)));
    }

    // Jump to shared bailout tail. The BailoutInfo pointer has to be in r9.
    JitCode* bailoutTail = cx->runtime()->jitRuntime()->getBailoutTail();
    masm.branch(bailoutTail);
}

JitCode*
JitRuntime::generateBailoutTable(JSContext* cx, uint32_t frameClass)
{
    // FIXME: Implement.
    MacroAssembler masm;
    masm.breakpoint();
    Linker linker(masm);
    return linker.newCode<NoGC>(cx, OTHER_CODE);
}

JitCode*
JitRuntime::generateBailoutHandler(JSContext* cx)
{
    MacroAssembler masm(cx);
    GenerateBailoutThunk(cx, masm, NO_FRAME_SIZE_CLASS_ID);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "BailoutHandler");
#endif

    Linker linker(masm);
    return linker.newCode<NoGC>(cx, OTHER_CODE);
}

JitCode*
JitRuntime::generateVMWrapper(JSContext* cx, const VMFunction& f)
{
    MOZ_ASSERT(functionWrappers_);
    MOZ_ASSERT(functionWrappers_->initialized());
    VMWrapperMap::AddPtr p = functionWrappers_->lookupForAdd(&f);
    if (p)
        return p->value();

    MacroAssembler masm(cx);

    // Avoid conflicts with argument registers while discarding the result after
    // the function call.
    AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

    // Wrapper register set is a superset of the Volatile register set.
    JS_STATIC_ASSERT((Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0);

    // Unlike on other platforms, it is the responsibility of the VM *callee* to
    // push the return address, while the caller must ensure that the address
    // is stored in lr on entry. This allows the VM wrapper to work with both direct
    // calls and tail calls.
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
    masm.enterExitFrame(&f);
    masm.loadJSContext(reg_cx);

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

    masm.setupUnalignedABICall(regs.getAny());
    masm.passABIArg(reg_cx);

    size_t argDisp = 0;

    // Copy arguments.
    for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
        MoveOperand from;
        switch (f.argProperties(explicitArg)) {
          case VMFunction::WordByValue:
            masm.passABIArg(MoveOperand(argsBase, argDisp),
                            (f.argPassedInFloatReg(explicitArg) ? MoveOp::DOUBLE : MoveOp::GENERAL));
            argDisp += sizeof(void*);
            break;

          case VMFunction::WordByRef:
            masm.passABIArg(MoveOperand(argsBase, argDisp, MoveOperand::EFFECTIVE_ADDRESS),
                            MoveOp::GENERAL);
            argDisp += sizeof(void*);
            break;

          case VMFunction::DoubleByValue:
          case VMFunction::DoubleByRef:
            MOZ_CRASH("NYI: AArch64 callVM should not be used with 128bit values.");
        }
    }

    // Copy the semi-implicit outparam, if any.
    // It is not a C++-abi outparam, which would get passed in the
    // outparam register, but a real parameter to the function, which
    // was stack-allocated above.
    if (outReg != InvalidReg)
        masm.passABIArg(outReg);

    masm.callWithABI(f.wrapped);

    // SP is used to transfer stack across call boundaries.
    if (!masm.GetStackPointer64().Is(vixl::sp))
        masm.Mov(masm.GetStackPointer64(), vixl::sp);

    // Test for failure.
    switch (f.failType()) {
      case Type_Object:
        masm.branchTestPtr(Assembler::Zero, r0, r0, masm.failureLabel());
        break;
      case Type_Bool:
        masm.branchIfFalseBool(r0, masm.failureLabel());
        break;
      default:
        MOZ_CRASH("unknown failure kind");
    }

    // Load the outparam and free any allocated stack.
    switch (f.outParam) {
      case Type_Value:
        masm.Ldr(ARMRegister(JSReturnReg, 64), MemOperand(masm.GetStackPointer64()));
        masm.freeStack(sizeof(Value));
        break;

      case Type_Handle:
        masm.popRooted(f.outParamRootType, ReturnReg, JSReturnOperand);
        break;

      case Type_Int32:
        masm.Ldr(ARMRegister(ReturnReg, 32), MemOperand(masm.GetStackPointer64()));
        masm.freeStack(sizeof(int64_t));
        break;

      case Type_Bool:
        masm.Ldrb(ARMRegister(ReturnReg, 32), MemOperand(masm.GetStackPointer64()));
        masm.freeStack(sizeof(int64_t));
        break;

      case Type_Double:
        MOZ_ASSERT(cx->runtime()->jitSupportsFloatingPoint);
        masm.Ldr(ARMFPRegister(ReturnDoubleReg, 64), MemOperand(masm.GetStackPointer64()));
        masm.freeStack(sizeof(double));
        break;

      case Type_Pointer:
        masm.Ldr(ARMRegister(ReturnReg, 64), MemOperand(masm.GetStackPointer64()));
        masm.freeStack(sizeof(uintptr_t));
        break;

      default:
        MOZ_ASSERT(f.outParam == Type_Void);
        break;
    }

    masm.leaveExitFrame();
    masm.retn(Imm32(sizeof(ExitFrameLayout) +
              f.explicitStackSlots() * sizeof(void*) +
              f.extraValuesToPop * sizeof(Value)));

    Linker linker(masm);
    JitCode* wrapper = linker.newCode<NoGC>(cx, OTHER_CODE);
    if (!wrapper)
        return nullptr;

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(wrapper, "VMWrapper");
#endif

    // linker.newCode may trigger a GC and sweep functionWrappers_ so we have to
    // use relookupOrAdd instead of add.
    if (!functionWrappers_->relookupOrAdd(p, &f, wrapper))
        return nullptr;

    return wrapper;
}

JitCode*
JitRuntime::generatePreBarrier(JSContext* cx, MIRType type)
{
    MacroAssembler masm(cx);

    LiveRegisterSet regs = LiveRegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                                           FloatRegisterSet(FloatRegisters::VolatileMask));

    // Also preserve the return address.
    regs.add(lr);

    masm.PushRegsInMask(regs);

    MOZ_ASSERT(PreBarrierReg == r1);
    masm.movePtr(ImmPtr(cx->runtime()), r3);

    masm.setupUnalignedABICall(r0);
    masm.passABIArg(r3);
    masm.passABIArg(PreBarrierReg);
    masm.callWithABI(IonMarkFunction(type));

    // Pop the volatile regs and restore LR.
    masm.PopRegsInMask(regs);

    masm.abiret();

    Linker linker(masm);
    return linker.newCode<NoGC>(cx, OTHER_CODE);
}

typedef bool (*HandleDebugTrapFn)(JSContext*, BaselineFrame*, uint8_t*, bool*);
static const VMFunction HandleDebugTrapInfo = FunctionInfo<HandleDebugTrapFn>(HandleDebugTrap);

JitCode*
JitRuntime::generateDebugTrapHandler(JSContext* cx)
{
    MacroAssembler masm(cx);
#ifndef JS_USE_LINK_REGISTER
    // The first value contains the return addres,
    // which we pull into ICTailCallReg for tail calls.
    masm.setFramePushed(sizeof(intptr_t));
#endif

    Register scratch1 = r0;
    Register scratch2 = r1;

    // Load BaselineFrame pointer into scratch1.
    masm.Sub(ARMRegister(scratch1, 64), BaselineFrameReg64, Operand(BaselineFrame::Size()));

    // Enter a stub frame and call the HandleDebugTrap VM function. Ensure the
    // stub frame has a nullptr ICStub pointer, since this pointer is marked
    // during GC.
    masm.movePtr(ImmPtr(nullptr), ICStubReg);
    EmitBaselineEnterStubFrame(masm, scratch2);

    JitCode* code = cx->runtime()->jitRuntime()->getVMWrapper(HandleDebugTrapInfo);
    if (!code)
        return nullptr;

    masm.asVIXL().Push(vixl::lr, ARMRegister(scratch1, 64));
    EmitBaselineCallVM(code, masm);

    EmitBaselineLeaveStubFrame(masm);

    // If the stub returns |true|, we have to perform a forced return (return
    // from the JS frame). If the stub returns |false|, just return from the
    // trap stub so that execution continues at the current pc.
    Label forcedReturn;
    masm.branchTest32(Assembler::NonZero, ReturnReg, ReturnReg, &forcedReturn);
    masm.abiret();

    masm.bind(&forcedReturn);
    masm.loadValue(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue()),
                   JSReturnOperand);
    masm.Mov(masm.GetStackPointer64(), BaselineFrameReg64);

    masm.pop(BaselineFrameReg, lr);
    masm.syncStackPtr();
    masm.abiret();

    Linker linker(masm);
    JitCode* codeDbg = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(codeDbg, "DebugTrapHandler");
#endif

    return codeDbg;
}

JitCode*
JitRuntime::generateExceptionTailStub(JSContext* cx, void* handler)
{
    MacroAssembler masm(cx);

    masm.handleFailureWithHandlerTail(handler);

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "ExceptionTailStub");
#endif

    return code;
}

JitCode*
JitRuntime::generateBailoutTailStub(JSContext* cx)
{
    MacroAssembler masm(cx);

    masm.generateBailoutTail(r1, r2);

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "BailoutTailStub");
#endif

    return code;
}
JitCode*
JitRuntime::generateProfilerExitFrameTailStub(JSContext* cx)
{
    MacroAssembler masm;

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
    AbsoluteAddress activationAddr(GetJitContext()->runtime->addressOfProfilingActivation());
    masm.loadPtr(activationAddr, actReg);

    Address lastProfilingFrame(actReg, JitActivation::offsetOfLastProfilingFrame());
    Address lastProfilingCallSite(actReg, JitActivation::offsetOfLastProfilingCallSite());

#ifdef DEBUG
    // Ensure that frame we are exiting is current lastProfilingFrame
    {
        masm.loadPtr(lastProfilingFrame, scratch1);
        Label checkOk;
        masm.branchPtr(Assembler::Equal, scratch1, ImmWord(0), &checkOk);
        masm.branchStackPtr(Assembler::Equal, scratch1, &checkOk);
        masm.assumeUnreachable("Mismatch between stored lastProfilingFrame and current stack pointer.");
        masm.bind(&checkOk);
    }
#endif

    // Load the frame descriptor into |scratch1|, figure out what to do depending on its type.
    masm.loadPtr(Address(masm.getStackPointer(), JitFrameLayout::offsetOfDescriptor()), scratch1);

    // Going into the conditionals, we will have:
    //      FrameDescriptor.size in scratch1
    //      FrameDescriptor.type in scratch2
    masm.and32(Imm32((1 << FRAMESIZE_SHIFT) - 1), scratch1, scratch2);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch1);

    // Handling of each case is dependent on FrameDescriptor.type
    Label handle_IonJS;
    Label handle_BaselineStub;
    Label handle_Rectifier;
    Label handle_IonAccessorIC;
    Label handle_Entry;
    Label end;

    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_IonJS), &handle_IonJS);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_BaselineJS), &handle_IonJS);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_BaselineStub), &handle_BaselineStub);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_Rectifier), &handle_Rectifier);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_IonAccessorIC), &handle_IonAccessorIC);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_Entry), &handle_Entry);

    masm.assumeUnreachable("Invalid caller frame type when exiting from Ion frame.");

    //
    // JitFrame_IonJS
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
        masm.loadPtr(Address(masm.getStackPointer(), JitFrameLayout::offsetOfReturnAddress()),
                     scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        // Store return frame in lastProfilingFrame.
        // scratch2 := masm.getStackPointer() + Descriptor.size*1 + JitFrameLayout::Size();
        masm.addPtr(masm.getStackPointer(), scratch1, scratch2);
        masm.syncStackPtr();
        masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2, scratch2);
        masm.storePtr(scratch2, lastProfilingFrame);
        masm.ret();
    }

    //
    // JitFrame_BaselineStub
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
        masm.addPtr(masm.getStackPointer(), scratch1, scratch3);
        masm.syncStackPtr();
        Address stubFrameReturnAddr(scratch3,
                                    JitFrameLayout::Size() +
                                    BaselineStubFrameLayout::offsetOfReturnAddress());
        masm.loadPtr(stubFrameReturnAddr, scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        Address stubFrameSavedFramePtr(scratch3,
                                       JitFrameLayout::Size() - (2 * sizeof(void*)));
        masm.loadPtr(stubFrameSavedFramePtr, scratch2);
        masm.addPtr(Imm32(sizeof(void*)), scratch2); // Skip past BL-PrevFramePtr.
        masm.storePtr(scratch2, lastProfilingFrame);
        masm.ret();
    }


    //
    // JitFrame_Rectifier
    //
    // The rectifier frame can be preceded by either an IonJS or a
    // BaselineStub frame.
    //
    // Stack layout if caller of rectifier was Ion:
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
        masm.addPtr(masm.getStackPointer(), scratch1, scratch2);
        masm.syncStackPtr();
        masm.add32(Imm32(JitFrameLayout::Size()), scratch2);
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfDescriptor()), scratch3);
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch3, scratch1);
        masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch3);

        // Now |scratch1| contains Rect-Descriptor.Size
        // and |scratch2| points to Rectifier frame
        // and |scratch3| contains Rect-Descriptor.Type

        // Check for either Ion or BaselineStub frame.
        Label handle_Rectifier_BaselineStub;
        masm.branch32(Assembler::NotEqual, scratch3, Imm32(JitFrame_IonJS),
                      &handle_Rectifier_BaselineStub);

        // Handle Rectifier <- IonJS
        // scratch3 := RectFrame[ReturnAddr]
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfReturnAddress()), scratch3);
        masm.storePtr(scratch3, lastProfilingCallSite);

        // scratch3 := RectFrame + Rect-Descriptor.Size + RectifierFrameLayout::Size()
        masm.addPtr(scratch2, scratch1, scratch3);
        masm.add32(Imm32(RectifierFrameLayout::Size()), scratch3);
        masm.storePtr(scratch3, lastProfilingFrame);
        masm.ret();

        // Handle Rectifier <- BaselineStub <- BaselineJS
        masm.bind(&handle_Rectifier_BaselineStub);
#ifdef DEBUG
        {
            Label checkOk;
            masm.branch32(Assembler::Equal, scratch3, Imm32(JitFrame_BaselineStub), &checkOk);
            masm.assumeUnreachable("Unrecognized frame preceding baselineStub.");
            masm.bind(&checkOk);
        }
#endif
        masm.addPtr(scratch2, scratch1, scratch3);
        Address stubFrameReturnAddr(scratch3, RectifierFrameLayout::Size() +
                                              BaselineStubFrameLayout::offsetOfReturnAddress());
        masm.loadPtr(stubFrameReturnAddr, scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        Address stubFrameSavedFramePtr(scratch3,
                                       RectifierFrameLayout::Size() - (2 * sizeof(void*)));
        masm.loadPtr(stubFrameSavedFramePtr, scratch2);
        masm.addPtr(Imm32(sizeof(void*)), scratch2);
        masm.storePtr(scratch2, lastProfilingFrame);
        masm.ret();
    }

    // JitFrame_IonAccessorIC
    //
    // The caller is always an IonJS frame.
    //
    //              Ion-Descriptor
    //              Ion-ReturnAddr
    //              ... ion frame data ... |- AccFrame-Descriptor.Size
    //              StubCode             |
    //              AccFrame-Descriptor  |- IonAccessorICFrameLayout::Size()
    //              AccFrame-ReturnAddr  |
    //              ... accessor frame data & args ... |- Descriptor.Size
    //              ActualArgc      |
    //              CalleeToken     |- JitFrameLayout::Size()
    //              Descriptor      |
    //    FP -----> ReturnAddr      |
    masm.bind(&handle_IonAccessorIC);
    {
        // scratch2 := StackPointer + Descriptor.size + JitFrameLayout::Size()
        masm.addPtr(masm.getStackPointer(), scratch1, scratch2);
        masm.syncStackPtr();
        masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2);

        // scratch3 := AccFrame-Descriptor.Size
        masm.loadPtr(Address(scratch2, IonAccessorICFrameLayout::offsetOfDescriptor()), scratch3);
#ifdef DEBUG
        // Assert previous frame is an IonJS frame.
        masm.movePtr(scratch3, scratch1);
        masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch1);
        {
            Label checkOk;
            masm.branch32(Assembler::Equal, scratch1, Imm32(JitFrame_IonJS), &checkOk);
            masm.assumeUnreachable("IonAccessorIC frame must be preceded by IonJS frame");
            masm.bind(&checkOk);
        }
#endif
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch3);

        // lastProfilingCallSite := AccFrame-ReturnAddr
        masm.loadPtr(Address(scratch2, IonAccessorICFrameLayout::offsetOfReturnAddress()), scratch1);
        masm.storePtr(scratch1, lastProfilingCallSite);

        // lastProfilingFrame := AccessorFrame + AccFrame-Descriptor.Size +
        //                       IonAccessorICFrameLayout::Size()
        masm.addPtr(scratch2, scratch3, scratch1);
        masm.addPtr(Imm32(IonAccessorICFrameLayout::Size()), scratch1);
        masm.storePtr(scratch1, lastProfilingFrame);
        masm.ret();
    }

    //
    // JitFrame_Entry
    //
    // If at an entry frame, store null into both fields.
    //
    masm.bind(&handle_Entry);
    {
        masm.movePtr(ImmPtr(nullptr), scratch1);
        masm.storePtr(scratch1, lastProfilingCallSite);
        masm.storePtr(scratch1, lastProfilingFrame);
        masm.ret();
    }

    Linker linker(masm);
    AutoFlushICache afc("ProfilerExitFrameTailStub");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "ProfilerExitFrameStub");
#endif

    return code;
}
