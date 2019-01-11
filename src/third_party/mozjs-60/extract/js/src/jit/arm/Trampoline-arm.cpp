/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/SharedICHelpers-arm.h"
#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "vm/JSCompartment.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/VMFunctions.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"

using namespace js;
using namespace js::jit;

static const FloatRegisterSet NonVolatileFloatRegs =
    FloatRegisterSet((1ULL << FloatRegisters::d8) |
                     (1ULL << FloatRegisters::d9) |
                     (1ULL << FloatRegisters::d10) |
                     (1ULL << FloatRegisters::d11) |
                     (1ULL << FloatRegisters::d12) |
                     (1ULL << FloatRegisters::d13) |
                     (1ULL << FloatRegisters::d14) |
                     (1ULL << FloatRegisters::d15));

static void
GenerateReturn(MacroAssembler& masm, int returnCode)
{
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

struct EnterJITStack
{
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
 *   void enter(void* code, int argc, Value* argv, InterpreterFrame* fp, CalleeToken
 *              calleeToken, JSObject* scopeChain, Value* vp)
 *   ...using standard EABI calling convention
 */
void
JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm)
{
    enterJITOffset_ = startTrampolineCode(masm);

    const Address slot_token(sp, offsetof(EnterJITStack, token));
    const Address slot_vp(sp, offsetof(EnterJITStack, vp));

    MOZ_ASSERT(OsrFrameReg == r3);

    Assembler* aasm = &masm;

    // Save non-volatile registers. These must be saved by the trampoline,
    // rather than the JIT'd code, because they are scanned by the conservative
    // scanner.
    masm.startDataTransferM(IsStore, sp, DB, WriteBack);
    masm.transferReg(r4); // [sp,0]
    masm.transferReg(r5); // [sp,4]
    masm.transferReg(r6); // [sp,8]
    masm.transferReg(r7); // [sp,12]
    masm.transferReg(r8); // [sp,16]
    masm.transferReg(r9); // [sp,20]
    masm.transferReg(r10); // [sp,24]
    masm.transferReg(r11); // [sp,28]
    // The abi does not expect r12 (ip) to be preserved
    masm.transferReg(lr);  // [sp,32]
    // The 5th argument is located at [sp, 36]
    masm.finishDataTransfer();

    // Add padding word.
    masm.subPtr(Imm32(sizeof(void*)), sp);

    // Push the float registers.
    masm.transferMultipleByRuns(NonVolatileFloatRegs, IsStore, sp, DB);

    // Save stack pointer into r8
    masm.movePtr(sp, r8);

    // Load calleeToken into r9.
    masm.loadPtr(slot_token, r9);

    // Save stack pointer.
    masm.movePtr(sp, r11);

    // Load the number of actual arguments into r10.
    masm.loadPtr(slot_vp, r10);
    masm.unboxInt32(Address(r10, 0), r10);

    {
        Label noNewTarget;
        masm.branchTest32(Assembler::Zero, r9, Imm32(CalleeToken_FunctionConstructing),
                          &noNewTarget);

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
    aasm->as_sub(r4, sp, O2RegImmShift(r1, LSL, 3));    // r4 = sp - argc*8
    aasm->as_bic(r4, r4, Imm8(JitStackAlignment - 1));
    // r4 is now the aligned on the bottom of the list of arguments.
    static_assert(sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");
    // sp' = ~(JitStackAlignment - 1) & (sp - argc * sizeof(Value)) - sizeof(JitFrameLayout)
    aasm->as_sub(sp, r4, Imm8(sizeof(JitFrameLayout)));

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
        aasm->as_extdtr(IsLoad,  64, true, PostIndex, r6, EDtrAddr(r2, EDtrOffImm(8)));
        aasm->as_extdtr(IsStore, 64, true, PostIndex, r6, EDtrAddr(r4, EDtrOffImm(8)));
        aasm->as_b(&header, Assembler::NonZero);
        masm.bind(&footer);
    }

    masm.ma_sub(r8, sp, r8);
    masm.makeFrameDescriptor(r8, JitFrame_CppToJSJit, JitFrameLayout::Size());

    masm.startDataTransferM(IsStore, sp, IB, NoWriteBack);
                           // [sp]    = return address (written later)
    masm.transferReg(r8);  // [sp',4] = descriptor, argc*8+20
    masm.transferReg(r9);  // [sp',8]  = callee token
    masm.transferReg(r10); // [sp',12]  = actual arguments
    masm.finishDataTransfer();

    Label returnLabel;
    {
        // Handle Interpreter -> Baseline OSR.
        AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
        regs.take(JSReturnOperand);
        regs.takeUnchecked(OsrFrameReg);
        regs.take(r11);
        regs.take(ReturnReg);

        const Address slot_numStackValues(r11, offsetof(EnterJITStack, numStackValues));

        Label notOsr;
        masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

        Register scratch = regs.takeAny();

        Register numStackValues = regs.takeAny();
        masm.load32(slot_numStackValues, numStackValues);

        // Write return address. On ARM, CodeLabel is only used for tableswitch,
        // so we can't use it here to get the return address. Instead, we use pc
        // + a fixed offset to a jump to returnLabel. The pc register holds pc +
        // 8, so we add the size of 2 instructions to skip the instructions
        // emitted by storePtr and jump(&skipJump).
        {
            AutoForbidPools afp(&masm, 5);
            Label skipJump;
            masm.mov(pc, scratch);
            masm.addPtr(Imm32(2 * sizeof(uint32_t)), scratch);
            masm.storePtr(scratch, Address(sp, 0));
            masm.jump(&skipJump);
            masm.jump(&returnLabel);
            masm.bind(&skipJump);
        }

        // Push previous frame pointer.
        masm.push(r11);

        // Reserve frame.
        Register framePtr = r11;
        masm.subPtr(Imm32(BaselineFrame::Size()), sp);
        masm.mov(sp, framePtr);

#ifdef XP_WIN
        // Can't push large frames blindly on windows. Touch frame memory
        // incrementally.
        masm.ma_lsl(Imm32(3), numStackValues, scratch);
        masm.subPtr(scratch, framePtr);
        {
            masm.ma_sub(sp, Imm32(WINDOWS_BIG_FRAME_TOUCH_INCREMENT), scratch);

            Label touchFrameLoop;
            Label touchFrameLoopEnd;
            masm.bind(&touchFrameLoop);
            masm.branchPtr(Assembler::Below, scratch, framePtr, &touchFrameLoopEnd);
            masm.store32(Imm32(0), Address(scratch, 0));
            masm.subPtr(Imm32(WINDOWS_BIG_FRAME_TOUCH_INCREMENT), scratch);
            masm.jump(&touchFrameLoop);
            masm.bind(&touchFrameLoopEnd);
        }
        masm.mov(sp, framePtr);
#endif

        // Reserve space for locals and stack values.
        masm.ma_lsl(Imm32(3), numStackValues, scratch);
        masm.ma_sub(sp, scratch, sp);

        // Enter exit frame.
        masm.addPtr(Imm32(BaselineFrame::Size() + BaselineFrame::FramePointerOffset), scratch);
        masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS, ExitFrameLayout::Size());
        masm.push(scratch);
        masm.push(Imm32(0)); // Fake return address.
        // No GC things to mark on the stack, push a bare token.
        masm.loadJSContext(scratch);
        masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

        masm.push(framePtr); // BaselineFrame
        masm.push(r0); // jitcode

        masm.setupUnalignedABICall(scratch);
        masm.passABIArg(r11); // BaselineFrame
        masm.passABIArg(OsrFrameReg); // InterpreterFrame
        masm.passABIArg(numStackValues);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, jit::InitBaselineFrameForOsr), MoveOp::GENERAL,
                         CheckUnsafeCallWithABI::DontCheckHasExitFrame);

        Register jitcode = regs.takeAny();
        masm.pop(jitcode);
        masm.pop(framePtr);

        MOZ_ASSERT(jitcode != ReturnReg);

        Label error;
        masm.addPtr(Imm32(ExitFrameLayout::SizeWithFooter()), sp);
        masm.addPtr(Imm32(BaselineFrame::Size()), framePtr);
        masm.branchIfFalseBool(ReturnReg, &error);

        // If OSR-ing, then emit instrumentation for setting lastProfilerFrame
        // if profiler instrumentation is enabled.
        {
            Label skipProfilingInstrumentation;
            Register realFramePtr = numStackValues;
            AbsoluteAddress addressOfEnabled(cx->runtime()->geckoProfiler().addressOfEnabled());
            masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                          &skipProfilingInstrumentation);
            masm.as_add(realFramePtr, framePtr, Imm8(sizeof(void*)));
            masm.profilerEnterFrame(realFramePtr, scratch);
            masm.bind(&skipProfilingInstrumentation);
        }

        masm.jump(jitcode);

        // OOM: Load error value, discard return address and previous frame
        // pointer and return.
        masm.bind(&error);
        masm.mov(framePtr, sp);
        masm.addPtr(Imm32(2 * sizeof(uintptr_t)), sp);
        masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
        masm.jump(&returnLabel);

        masm.bind(&notOsr);
        // Load the scope chain in R1.
        MOZ_ASSERT(R1.scratchReg() != r0);
        masm.loadPtr(Address(r11, offsetof(EnterJITStack, scopeChain)), R1.scratchReg());
    }

    // The Data transfer is pushing 4 words, which already account for the
    // return address space of the Jit frame.  We have to undo what the data
    // transfer did before making the call.
    masm.addPtr(Imm32(sizeof(uintptr_t)), sp);

    // The callee will push the return address on the stack, thus we check that
    // the stack would be aligned once the call is complete.
    masm.assertStackAlignment(JitStackAlignment, sizeof(uintptr_t));

    // Call the function.
    masm.callJitNoProfiler(r0);

    // Interpreter -> Baseline OSR will return here.
    masm.bind(&returnLabel);

    // The top of the stack now points to the address of the field following the
    // return address because the return address is popped for the return, so we
    // need to remove the size of the return address field.
    aasm->as_sub(sp, sp, Imm8(4));

    // Load off of the stack the size of our local stack.
    masm.loadPtr(Address(sp, JitFrameLayout::offsetOfDescriptor()), r5);
    aasm->as_add(sp, sp, lsr(r5, FRAMESIZE_SHIFT));

    // Store the returned value into the slot_vp
    masm.loadPtr(slot_vp, r5);
    masm.storeValue(JSReturnOperand, Address(r5, 0));

    // :TODO: Optimize storeValue with:
    // We're using a load-double here. In order for that to work, the data needs
    // to be stored in two consecutive registers, make sure this is the case
    //   MOZ_ASSERT(JSReturnReg_Type.code() == JSReturnReg_Data.code()+1);
    //   aasm->as_extdtr(IsStore, 64, true, Offset,
    //                   JSReturnReg_Data, EDtrAddr(r5, EDtrOffImm(0)));

    // Restore non-volatile registers and return.
    GenerateReturn(masm, true);
}

void
JitRuntime::generateInvalidator(MacroAssembler& masm, Label* bailoutTail)
{
    // See large comment in x86's JitRuntime::generateInvalidator.

    invalidatorOffset_ = startTrampolineCode(masm);

    // At this point, one of two things has happened:
    // 1) Execution has just returned from C code, which left the stack aligned
    // 2) Execution has just returned from Ion code, which left the stack unaligned.
    // The old return address should not matter, but we still want the stack to
    // be aligned, and there is no good reason to automatically align it with a
    // call to setupUnalignedABICall.
    masm.as_bic(sp, sp, Imm8(7));
    masm.startDataTransferM(IsStore, sp, DB, WriteBack);
    // We don't have to push everything, but this is likely easier.
    // Setting regs_.
    for (uint32_t i = 0; i < Registers::Total; i++)
        masm.transferReg(Register::FromCode(i));
    masm.finishDataTransfer();

    // Since our datastructures for stack inspection are compile-time fixed,
    // if there are only 16 double registers, then we need to reserve
    // space on the stack for the missing 16.
    if (FloatRegisters::ActualTotalPhys() != FloatRegisters::TotalPhys) {
        ScratchRegisterScope scratch(masm);
        int missingRegs = FloatRegisters::TotalPhys - FloatRegisters::ActualTotalPhys();
        masm.ma_sub(Imm32(missingRegs * sizeof(double)), sp, scratch);
    }

    masm.startFloatTransferM(IsStore, sp, DB, WriteBack);
    for (uint32_t i = 0; i < FloatRegisters::ActualTotalPhys(); i++)
        masm.transferFloatReg(FloatRegister(i, FloatRegister::Double));
    masm.finishFloatTransfer();

    masm.ma_mov(sp, r0);
    const int sizeOfRetval = sizeof(size_t)*2;
    masm.reserveStack(sizeOfRetval);
    masm.mov(sp, r1);
    const int sizeOfBailoutInfo = sizeof(void*)*2;
    masm.reserveStack(sizeOfBailoutInfo);
    masm.mov(sp, r2);
    masm.setupAlignedABICall();
    masm.passABIArg(r0);
    masm.passABIArg(r1);
    masm.passABIArg(r2);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, InvalidationBailout), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckOther);

    masm.ma_ldr(DTRAddr(sp, DtrOffImm(0)), r2);
    {
        ScratchRegisterScope scratch(masm);
        masm.ma_ldr(Address(sp, sizeOfBailoutInfo), r1, scratch);
    }
    // Remove the return address, the IonScript, the register state
    // (InvaliationBailoutStack) and the space that was allocated for the return
    // value.
    {
        ScratchRegisterScope scratch(masm);
        masm.ma_add(sp, Imm32(sizeof(InvalidationBailoutStack) + sizeOfRetval + sizeOfBailoutInfo), sp, scratch);
    }
    // Remove the space that this frame was using before the bailout (computed
    // by InvalidationBailout)
    masm.ma_add(sp, r1, sp);

    // Jump to shared bailout tail. The BailoutInfo pointer has to be in r2.
    masm.jump(bailoutTail);
}

void
JitRuntime::generateArgumentsRectifier(MacroAssembler& masm)
{
    argumentsRectifierOffset_ = startTrampolineCode(masm);
    masm.pushReturnAddress();

    // Copy number of actual arguments into r0 and r8.
    masm.ma_ldr(DTRAddr(sp, DtrOffImm(RectifierFrameLayout::offsetOfNumActualArgs())), r0);
    masm.mov(r0, r8);

    // Load the number of |undefined|s to push into r6.
    masm.ma_ldr(DTRAddr(sp, DtrOffImm(RectifierFrameLayout::offsetOfCalleeToken())), r1);
    {
        ScratchRegisterScope scratch(masm);
        masm.ma_and(Imm32(CalleeTokenMask), r1, r6, scratch);
    }
    masm.ma_ldrh(EDtrAddr(r6, EDtrOffImm(JSFunction::offsetOfNargs())), r6);

    masm.ma_sub(r6, r8, r2);

    // Get the topmost argument.
    {
        ScratchRegisterScope scratch(masm);
        masm.ma_alu(sp, lsl(r8, 3), r3, OpAdd); // r3 <- r3 + nargs * 8
        masm.ma_add(r3, Imm32(sizeof(RectifierFrameLayout)), r3, scratch);
    }

    {
        Label notConstructing;

        masm.branchTest32(Assembler::Zero, r1, Imm32(CalleeToken_FunctionConstructing),
                          &notConstructing);

        // Add sizeof(Value) to overcome |this|
        masm.as_extdtr(IsLoad, 64, true, Offset, r4, EDtrAddr(r3, EDtrOffImm(8)));
        masm.as_extdtr(IsStore, 64, true, PreIndex, r4, EDtrAddr(sp, EDtrOffImm(-8)));

        // Include the newly pushed newTarget value in the frame size
        // calculated below.
        masm.add32(Imm32(1), r6);

        masm.bind(&notConstructing);
    }

    // Push undefined.
    masm.moveValue(UndefinedValue(), ValueOperand(r5, r4));
    {
        Label undefLoopTop;
        masm.bind(&undefLoopTop);
        masm.as_extdtr(IsStore, 64, true, PreIndex, r4, EDtrAddr(sp, EDtrOffImm(-8)));
        masm.as_sub(r2, r2, Imm8(1), SetCC);

        masm.ma_b(&undefLoopTop, Assembler::NonZero);
    }

    // Push arguments, |nargs| + 1 times (to include |this|).
    {
        Label copyLoopTop;
        masm.bind(&copyLoopTop);
        masm.as_extdtr(IsLoad, 64, true, PostIndex, r4, EDtrAddr(r3, EDtrOffImm(-8)));
        masm.as_extdtr(IsStore, 64, true, PreIndex, r4, EDtrAddr(sp, EDtrOffImm(-8)));

        masm.as_sub(r8, r8, Imm8(1), SetCC);
        masm.ma_b(&copyLoopTop, Assembler::NotSigned);
    }

    // translate the framesize from values into bytes
    masm.as_add(r6, r6, Imm8(1));
    masm.ma_lsl(Imm32(3), r6, r6);

    // Construct sizeDescriptor.
    masm.makeFrameDescriptor(r6, JitFrame_Rectifier, JitFrameLayout::Size());

    // Construct JitFrameLayout.
    masm.ma_push(r0); // actual arguments.
    masm.ma_push(r1); // callee token
    masm.ma_push(r6); // frame descriptor.

    // Call the target function.
    masm.andPtr(Imm32(CalleeTokenMask), r1);
    masm.loadJitCodeRaw(r1, r3);
    argumentsRectifierReturnOffset_ = masm.callJitNoProfiler(r3);

    // arg1
    //  ...
    // argN
    // num actual args
    // callee token
    // sizeDescriptor     <- sp now
    // return address

    // Remove the rectifier frame.
    {
        ScratchRegisterScope scratch(masm);
        masm.ma_dtr(IsLoad, sp, Imm32(12), r4, scratch, PostIndex);
    }

    // arg1
    //  ...
    // argN               <- sp now; r4 <- frame descriptor
    // num actual args
    // callee token
    // sizeDescriptor
    // return address

    // Discard pushed arguments.
    masm.ma_alu(sp, lsr(r4, FRAMESIZE_SHIFT), sp, OpAdd);

    masm.ret();
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

    masm.startDataTransferM(IsStore, sp, DB, WriteBack);
    // We don't have to push everything, but this is likely easier.
    // Setting regs_.
    for (uint32_t i = 0; i < Registers::Total; i++)
        masm.transferReg(Register::FromCode(i));
    masm.finishDataTransfer();

    ScratchRegisterScope scratch(masm);

    // Since our datastructures for stack inspection are compile-time fixed,
    // if there are only 16 double registers, then we need to reserve
    // space on the stack for the missing 16.
    if (FloatRegisters::ActualTotalPhys() != FloatRegisters::TotalPhys) {
        int missingRegs = FloatRegisters::TotalPhys - FloatRegisters::ActualTotalPhys();
        masm.ma_sub(Imm32(missingRegs * sizeof(double)), sp, scratch);
    }
    masm.startFloatTransferM(IsStore, sp, DB, WriteBack);
    for (uint32_t i = 0; i < FloatRegisters::ActualTotalPhys(); i++)
        masm.transferFloatReg(FloatRegister(i, FloatRegister::Double));
    masm.finishFloatTransfer();

    // STEP 1b: Push both the "return address" of the function call (the address
    //          of the instruction after the call that we used to get here) as
    //          well as the callee token onto the stack. The return address is
    //          currently in r14. We will proceed by loading the callee token
    //          into a sacrificial register <= r14, then pushing both onto the
    //          stack.

    // Now place the frameClass onto the stack, via a register.
    masm.ma_mov(Imm32(frameClass), r4);
    // And onto the stack. Since the stack is full, we need to put this one past
    // the end of the current stack. Sadly, the ABI says that we need to always
    // point to the lowest place that has been written. The OS is free to do
    // whatever it wants below sp.
    masm.startDataTransferM(IsStore, sp, DB, WriteBack);
    // Set frameClassId_.
    masm.transferReg(r4);
    // Set tableOffset_; higher registers are stored at higher locations on the
    // stack.
    masm.transferReg(lr);
    masm.finishDataTransfer();

    masm.ma_mov(sp, spArg);
}

static void
GenerateBailoutThunk(MacroAssembler& masm, uint32_t frameClass, Label* bailoutTail)
{
    PushBailoutFrame(masm, frameClass, r0);

    // SP % 8 == 4
    // STEP 1c: Call the bailout function, giving a pointer to the
    //          structure we just blitted onto the stack.
    const int sizeOfBailoutInfo = sizeof(void*)*2;
    masm.reserveStack(sizeOfBailoutInfo);
    masm.mov(sp, r1);
    masm.setupAlignedABICall();

    // Decrement sp by another 4, so we keep alignment. Not Anymore! Pushing
    // both the snapshotoffset as well as the: masm.as_sub(sp, sp, Imm8(4));

    // Set the old (4-byte aligned) value of the sp as the first argument.
    masm.passABIArg(r0);
    masm.passABIArg(r1);

    // Sp % 8 == 0
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, Bailout), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckOther);
    masm.ma_ldr(DTRAddr(sp, DtrOffImm(0)), r2);
    {
        ScratchRegisterScope scratch(masm);
        masm.ma_add(sp, Imm32(sizeOfBailoutInfo), sp, scratch);
    }

    // Common size of a bailout frame.
    uint32_t bailoutFrameSize = 0
        + sizeof(void*) // frameClass
        + sizeof(RegisterDump);

    if (frameClass == NO_FRAME_SIZE_CLASS_ID) {
        // Make sure the bailout frame size fits into the offset for a load.
        masm.as_dtr(IsLoad, 32, Offset,
                    r4, DTRAddr(sp, DtrOffImm(4)));
        // Used to be: offsetof(BailoutStack, frameSize_)
        // This structure is no longer available to us :(
        // We add 12 to the bailoutFrameSize because:
        // sizeof(uint32_t) for the tableOffset that was pushed onto the stack
        // sizeof(uintptr_t) for the snapshotOffset;
        // alignment to round the uintptr_t up to a multiple of 8 bytes.
        ScratchRegisterScope scratch(masm);
        masm.ma_add(sp, Imm32(bailoutFrameSize+12), sp, scratch);
        masm.as_add(sp, sp, O2Reg(r4));
    } else {
        ScratchRegisterScope scratch(masm);
        uint32_t frameSize = FrameSizeClass::FromClass(frameClass).frameSize();
        masm.ma_add(Imm32(// The frame that was added when we entered the most
                          // recent function.
                          frameSize
                          // The size of the "return address" that was dumped on
                          // the stack.
                          + sizeof(void*)
                          // Everything else that was pushed on the stack.
                          + bailoutFrameSize)
                    , sp, scratch);
    }

    // Jump to shared bailout tail. The BailoutInfo pointer has to be in r2.
    masm.jump(bailoutTail);
}

JitRuntime::BailoutTable
JitRuntime::generateBailoutTable(MacroAssembler& masm, Label* bailoutTail, uint32_t frameClass)
{
    uint32_t offset = startTrampolineCode(masm);

    {
        // Emit the table without any pools being inserted.
        Label bailout;
        AutoForbidPools afp(&masm, BAILOUT_TABLE_SIZE);
        for (size_t i = 0; i < BAILOUT_TABLE_SIZE; i++)
            masm.ma_bl(&bailout);
        masm.bind(&bailout);
    }

    GenerateBailoutThunk(masm, frameClass, bailoutTail);

    return BailoutTable(offset, masm.currentOffset() - offset);
}

void
JitRuntime::generateBailoutHandler(MacroAssembler& masm, Label* bailoutTail)
{
    bailoutHandlerOffset_ = startTrampolineCode(masm);

    GenerateBailoutThunk(masm, NO_FRAME_SIZE_CLASS_ID, bailoutTail);
}

bool
JitRuntime::generateVMWrapper(JSContext* cx, MacroAssembler& masm, const VMFunction& f)
{
    MOZ_ASSERT(functionWrappers_);
    MOZ_ASSERT(functionWrappers_->initialized());

    uint32_t wrapperOffset = startTrampolineCode(masm);

    AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

    static_assert((Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
                  "Wrapper register set must be a superset of Volatile register set.");

    // The context is the first argument; r0 is the first argument register.
    Register cxreg = r0;
    regs.take(cxreg);

    // Stack is:
    //    ... frame ...
    //  +8  [args] + argPadding
    //  +0  ExitFrame
    //
    // We're aligned to an exit frame, so link it up.
    // If it isn't a tail call, then the return address needs to be saved
    if (f.expectTailCall == NonTailCall)
        masm.pushReturnAddress();

    masm.loadJSContext(cxreg);
    masm.enterExitFrame(cxreg, regs.getAny(), &f);

    // Save the base of the argument set stored on the stack.
    Register argsBase = InvalidReg;
    if (f.explicitArgs) {
        argsBase = r5;
        regs.take(argsBase);
        ScratchRegisterScope scratch(masm);
        masm.ma_add(sp, Imm32(ExitFrameLayout::SizeWithFooter()), argsBase, scratch);
    }

    // Reserve space for the outparameter.
    Register outReg = InvalidReg;
    switch (f.outParam) {
      case Type_Value:
        outReg = r4;
        regs.take(outReg);
        masm.reserveStack(sizeof(Value));
        masm.ma_mov(sp, outReg);
        break;

      case Type_Handle:
        outReg = r4;
        regs.take(outReg);
        masm.PushEmptyRooted(f.outParamRootType);
        masm.ma_mov(sp, outReg);
        break;

      case Type_Int32:
      case Type_Pointer:
      case Type_Bool:
        outReg = r4;
        regs.take(outReg);
        masm.reserveStack(sizeof(int32_t));
        masm.ma_mov(sp, outReg);
        break;

      case Type_Double:
        outReg = r4;
        regs.take(outReg);
        masm.reserveStack(sizeof(double));
        masm.ma_mov(sp, outReg);
        break;

      default:
        MOZ_ASSERT(f.outParam == Type_Void);
        break;
    }

    if (!generateTLEnterVM(masm, f))
        return false;

    masm.setupUnalignedABICall(regs.getAny());
    masm.passABIArg(cxreg);

    size_t argDisp = 0;

    // Copy any arguments.
    for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
        MoveOperand from;
        switch (f.argProperties(explicitArg)) {
          case VMFunction::WordByValue:
            masm.passABIArg(MoveOperand(argsBase, argDisp), MoveOp::GENERAL);
            argDisp += sizeof(void*);
            break;
          case VMFunction::DoubleByValue:
            // Values should be passed by reference, not by value, so we assert
            // that the argument is a double-precision float.
            MOZ_ASSERT(f.argPassedInFloatReg(explicitArg));
            masm.passABIArg(MoveOperand(argsBase, argDisp), MoveOp::DOUBLE);
            argDisp += sizeof(double);
            break;
          case VMFunction::WordByRef:
            masm.passABIArg(MoveOperand(argsBase, argDisp, MoveOperand::EFFECTIVE_ADDRESS), MoveOp::GENERAL);
            argDisp += sizeof(void*);
            break;
          case VMFunction::DoubleByRef:
            masm.passABIArg(MoveOperand(argsBase, argDisp, MoveOperand::EFFECTIVE_ADDRESS), MoveOp::GENERAL);
            argDisp += 2 * sizeof(void*);
            break;
        }
    }

    // Copy the implicit outparam, if any.
    if (outReg != InvalidReg)
        masm.passABIArg(outReg);

    masm.callWithABI(f.wrapped, MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    if (!generateTLExitVM(masm, f))
        return false;

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
      case Type_Handle:
        masm.popRooted(f.outParamRootType, ReturnReg, JSReturnOperand);
        break;

      case Type_Value:
        masm.loadValue(Address(sp, 0), JSReturnOperand);
        masm.freeStack(sizeof(Value));
        break;

      case Type_Int32:
      case Type_Pointer:
        masm.load32(Address(sp, 0), ReturnReg);
        masm.freeStack(sizeof(int32_t));
        break;

      case Type_Bool:
        masm.load8ZeroExtend(Address(sp, 0), ReturnReg);
        masm.freeStack(sizeof(int32_t));
        break;

      case Type_Double:
        if (cx->runtime()->jitSupportsFloatingPoint)
            masm.loadDouble(Address(sp, 0), ReturnDoubleReg);
        else
            masm.assumeUnreachable("Unable to load into float reg, with no FP support.");
        masm.freeStack(sizeof(double));
        break;

      default:
        MOZ_ASSERT(f.outParam == Type_Void);
        break;
    }

    // Until C++ code is instrumented against Spectre, prevent speculative
    // execution from returning any private data.
    if (f.returnsData() && JitOptions.spectreJitToCxxCalls)
        masm.speculationBarrier();

    masm.leaveExitFrame();
    masm.retn(Imm32(sizeof(ExitFrameLayout) +
                    f.explicitStackSlots() * sizeof(void*) +
                    f.extraValuesToPop * sizeof(Value)));

    return functionWrappers_->putNew(&f, wrapperOffset);
}

uint32_t
JitRuntime::generatePreBarrier(JSContext* cx, MacroAssembler& masm, MIRType type)
{
    uint32_t offset = startTrampolineCode(masm);

    masm.pushReturnAddress();

    MOZ_ASSERT(PreBarrierReg == r1);
    Register temp1 = r2;
    Register temp2 = r3;
    Register temp3 = r4;
    masm.push(temp1);
    masm.push(temp2);
    masm.push(temp3);

    Label noBarrier;
    masm.emitPreBarrierFastPath(cx->runtime(), type, temp1, temp2, temp3, &noBarrier);

    // Call into C++ to mark this GC thing.
    masm.pop(temp3);
    masm.pop(temp2);
    masm.pop(temp1);

    LiveRegisterSet save;
    if (cx->runtime()->jitSupportsFloatingPoint) {
        save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                                 FloatRegisterSet(FloatRegisters::VolatileDoubleMask));
    } else {
        save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                                 FloatRegisterSet());
    }
    masm.PushRegsInMask(save);

    masm.movePtr(ImmPtr(cx->runtime()), r0);

    masm.setupUnalignedABICall(r2);
    masm.passABIArg(r0);
    masm.passABIArg(r1);
    masm.callWithABI(JitMarkFunction(type));
    masm.PopRegsInMask(save);
    masm.ret();

    masm.bind(&noBarrier);
    masm.pop(temp3);
    masm.pop(temp2);
    masm.pop(temp1);
    masm.ret();

    return offset;
}

typedef bool (*HandleDebugTrapFn)(JSContext*, BaselineFrame*, uint8_t*, bool*);
static const VMFunction HandleDebugTrapInfo =
    FunctionInfo<HandleDebugTrapFn>(HandleDebugTrap, "HandleDebugTrap");

JitCode*
JitRuntime::generateDebugTrapHandler(JSContext* cx)
{
    MacroAssembler masm;

    Register scratch1 = r0;
    Register scratch2 = r1;

    // Load BaselineFrame pointer in scratch1.
    masm.mov(r11, scratch1);
    masm.subPtr(Imm32(BaselineFrame::Size()), scratch1);

    // Enter a stub frame and call the HandleDebugTrap VM function. Ensure the
    // stub frame has a nullptr ICStub pointer, since this pointer is marked
    // during GC.
    masm.movePtr(ImmPtr(nullptr), ICStubReg);
    EmitBaselineEnterStubFrame(masm, scratch2);

    TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(HandleDebugTrapInfo);
    masm.push(lr);
    masm.push(scratch1);
    EmitBaselineCallVM(code, masm);

    EmitBaselineLeaveStubFrame(masm);

    // If the stub returns |true|, we have to perform a forced return (return
    // from the JS frame). If the stub returns |false|, just return from the
    // trap stub so that execution continues at the current pc.
    Label forcedReturn;
    masm.branchTest32(Assembler::NonZero, ReturnReg, ReturnReg, &forcedReturn);
    masm.mov(lr, pc);

    masm.bind(&forcedReturn);
    masm.loadValue(Address(r11, BaselineFrame::reverseOffsetOfReturnValue()),
                   JSReturnOperand);
    masm.mov(r11, sp);
    masm.pop(r11);

    // Before returning, if profiling is turned on, make sure that lastProfilingFrame
    // is set to the correct caller frame.
    {
        Label skipProfilingInstrumentation;
        AbsoluteAddress addressOfEnabled(cx->runtime()->geckoProfiler().addressOfEnabled());
        masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skipProfilingInstrumentation);
        masm.profilerExitFrame();
        masm.bind(&skipProfilingInstrumentation);
    }

    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("DebugTrapHandler");
    JitCode* codeDbg = linker.newCode(cx, CodeKind::Other);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(codeDbg, "DebugTrapHandler");
#endif

    return codeDbg;
}

void
JitRuntime::generateExceptionTailStub(MacroAssembler& masm, void* handler, Label* profilerExitTail)
{
    exceptionTailOffset_ = startTrampolineCode(masm);

    masm.bind(masm.failureLabel());
    masm.handleFailureWithHandlerTail(handler, profilerExitTail);
}

void
JitRuntime::generateBailoutTailStub(MacroAssembler& masm, Label* bailoutTail)
{
    bailoutTailOffset_ = startTrampolineCode(masm);
    masm.bind(bailoutTail);

    masm.generateBailoutTail(r1, r2);
}

void
JitRuntime::generateProfilerExitFrameTailStub(MacroAssembler& masm, Label* profilerExitTail)
{
    profilerExitFrameTailOffset_ = startTrampolineCode(masm);
    masm.bind(profilerExitTail);

    Register scratch1 = r5;
    Register scratch2 = r6;
    Register scratch3 = r7;
    Register scratch4 = r8;

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
    masm.loadPtr(Address(actReg, offsetof(JSContext, profilingActivation_)), actReg);

    Address lastProfilingFrame(actReg, JitActivation::offsetOfLastProfilingFrame());
    Address lastProfilingCallSite(actReg, JitActivation::offsetOfLastProfilingCallSite());

#ifdef DEBUG
    // Ensure that frame we are exiting is current lastProfilingFrame
    {
        masm.loadPtr(lastProfilingFrame, scratch1);
        Label checkOk;
        masm.branchPtr(Assembler::Equal, scratch1, ImmWord(0), &checkOk);
        masm.branchPtr(Assembler::Equal, StackPointer, scratch1, &checkOk);
        masm.assumeUnreachable(
            "Mismatch between stored lastProfilingFrame and current stack pointer.");
        masm.bind(&checkOk);
    }
#endif

    // Load the frame descriptor into |scratch1|, figure out what to do depending on its type.
    masm.loadPtr(Address(StackPointer, JitFrameLayout::offsetOfDescriptor()), scratch1);

    // Going into the conditionals, we will have:
    //      FrameDescriptor.size in scratch1
    //      FrameDescriptor.type in scratch2
    {
        ScratchRegisterScope asmScratch(masm);
        masm.ma_and(Imm32((1 << FRAMETYPE_BITS) - 1), scratch1, scratch2, asmScratch);
    }
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch1);

    // Handling of each case is dependent on FrameDescriptor.type
    Label handle_IonJS;
    Label handle_BaselineStub;
    Label handle_Rectifier;
    Label handle_IonICCall;
    Label handle_Entry;
    Label end;

    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_IonJS), &handle_IonJS);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_BaselineJS), &handle_IonJS);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_BaselineStub), &handle_BaselineStub);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_Rectifier), &handle_Rectifier);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_IonICCall), &handle_IonICCall);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_CppToJSJit), &handle_Entry);

    // The WasmToJSJit is just another kind of entry.
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_WasmToJSJit), &handle_Entry);

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
        masm.loadPtr(Address(StackPointer, JitFrameLayout::offsetOfReturnAddress()), scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        // Store return frame in lastProfilingFrame.
        // scratch2 := StackPointer + Descriptor.size*1 + JitFrameLayout::Size();
        masm.ma_add(StackPointer, scratch1, scratch2);
        masm.as_add(scratch2, scratch2, Imm8(JitFrameLayout::Size()));
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
        masm.ma_add(StackPointer, scratch1, scratch3);
        Address stubFrameReturnAddr(scratch3,
                                    JitFrameLayout::Size() +
                                    BaselineStubFrameLayout::offsetOfReturnAddress());
        masm.loadPtr(stubFrameReturnAddr, scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        Address stubFrameSavedFramePtr(scratch3,
                                       JitFrameLayout::Size() - (2 * sizeof(void*)));
        masm.loadPtr(stubFrameSavedFramePtr, scratch2);
        masm.addPtr(Imm32(sizeof(void*)), scratch2); // Skip past BL-PrevFramePtr
        masm.storePtr(scratch2, lastProfilingFrame);
        masm.ret();
    }


    //
    // JitFrame_Rectifier
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
        masm.ma_add(StackPointer, scratch1, scratch2);
        masm.add32(Imm32(JitFrameLayout::Size()), scratch2);
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfDescriptor()), scratch3);
        masm.ma_lsr(Imm32(FRAMESIZE_SHIFT), scratch3, scratch1);
        masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch3);

        // Now |scratch1| contains Rect-Descriptor.Size
        // and |scratch2| points to Rectifier frame
        // and |scratch3| contains Rect-Descriptor.Type

        masm.assertRectifierFrameParentType(scratch3);

        // Check for either Ion or BaselineStub frame.
        Label notIonFrame;
        masm.branch32(Assembler::NotEqual, scratch3, Imm32(JitFrame_IonJS), &notIonFrame);

        // Handle Rectifier <- IonJS
        // scratch3 := RectFrame[ReturnAddr]
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfReturnAddress()), scratch3);
        masm.storePtr(scratch3, lastProfilingCallSite);

        // scratch3 := RectFrame + Rect-Descriptor.Size + RectifierFrameLayout::Size()
        masm.ma_add(scratch2, scratch1, scratch3);
        masm.add32(Imm32(RectifierFrameLayout::Size()), scratch3);
        masm.storePtr(scratch3, lastProfilingFrame);
        masm.ret();

        masm.bind(&notIonFrame);

        // Check for either BaselineStub or a CppToJSJit/WasmToJSJit entry
        // frame.
        masm.branch32(Assembler::NotEqual, scratch3, Imm32(JitFrame_BaselineStub), &handle_Entry);

        // Handle Rectifier <- BaselineStub <- BaselineJS
        masm.ma_add(scratch2, scratch1, scratch3);
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

    // JitFrame_IonICCall
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
        masm.ma_add(StackPointer, scratch1, scratch2);
        masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2);

        // scratch3 := ICCallFrame-Descriptor.Size
        masm.loadPtr(Address(scratch2, IonICCallFrameLayout::offsetOfDescriptor()), scratch3);
#ifdef DEBUG
        // Assert previous frame is an IonJS frame.
        masm.movePtr(scratch3, scratch1);
        masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch1);
        {
            Label checkOk;
            masm.branch32(Assembler::Equal, scratch1, Imm32(JitFrame_IonJS), &checkOk);
            masm.assumeUnreachable("IonICCall frame must be preceded by IonJS frame");
            masm.bind(&checkOk);
        }
#endif
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch3);

        // lastProfilingCallSite := ICCallFrame-ReturnAddr
        masm.loadPtr(Address(scratch2, IonICCallFrameLayout::offsetOfReturnAddress()), scratch1);
        masm.storePtr(scratch1, lastProfilingCallSite);

        // lastProfilingFrame := ICCallFrame + ICCallFrame-Descriptor.Size +
        //                       IonICCallFrameLayout::Size()
        masm.ma_add(scratch2, scratch3, scratch1);
        masm.addPtr(Imm32(IonICCallFrameLayout::Size()), scratch1);
        masm.storePtr(scratch1, lastProfilingFrame);
        masm.ret();
    }

    //
    // JitFrame_CppToJSJit / JitFrame_WasmToJSJit
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
