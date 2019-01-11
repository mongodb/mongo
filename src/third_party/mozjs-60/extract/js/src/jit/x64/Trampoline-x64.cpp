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
#include "jit/VMFunctions.h"
#include "jit/x64/SharedICHelpers-x64.h"
#include "vtune/VTuneWrapper.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::IsPowerOfTwo;

// All registers to save and restore. This includes the stack pointer, since we
// use the ability to reference register values on the stack by index.
static const LiveRegisterSet AllRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllMask),
                         FloatRegisterSet(FloatRegisters::AllMask));

// Generates a trampoline for calling Jit compiled code from a C++ function.
// The trampoline use the EnterJitCode signature, with the standard x64 fastcall
// calling convention.
void
JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm)
{
    enterJITOffset_ = startTrampolineCode(masm);

    masm.assertStackAlignment(ABIStackAlignment, -int32_t(sizeof(uintptr_t)) /* return address */);

    const Register reg_code  = IntArgReg0;
    const Register reg_argc  = IntArgReg1;
    const Register reg_argv  = IntArgReg2;
    MOZ_ASSERT(OsrFrameReg == IntArgReg3);

#if defined(_WIN64)
    const Address token  = Address(rbp, 16 + ShadowStackSpace);
    const Operand scopeChain = Operand(rbp, 24 + ShadowStackSpace);
    const Operand numStackValuesAddr = Operand(rbp, 32 + ShadowStackSpace);
    const Operand result = Operand(rbp, 40 + ShadowStackSpace);
#else
    const Register token = IntArgReg4;
    const Register scopeChain = IntArgReg5;
    const Operand numStackValuesAddr = Operand(rbp, 16 + ShadowStackSpace);
    const Operand result = Operand(rbp, 24 + ShadowStackSpace);
#endif

    // Save old stack frame pointer, set new stack frame pointer.
    masm.push(rbp);
    masm.mov(rsp, rbp);

    // Save non-volatile registers. These must be saved by the trampoline, rather
    // than by the JIT'd code, because they are scanned by the conservative scanner.
    masm.push(rbx);
    masm.push(r12);
    masm.push(r13);
    masm.push(r14);
    masm.push(r15);
#if defined(_WIN64)
    masm.push(rdi);
    masm.push(rsi);

    // 16-byte aligment for vmovdqa
    masm.subq(Imm32(16 * 10 + 8), rsp);

    masm.vmovdqa(xmm6, Operand(rsp, 16 * 0));
    masm.vmovdqa(xmm7, Operand(rsp, 16 * 1));
    masm.vmovdqa(xmm8, Operand(rsp, 16 * 2));
    masm.vmovdqa(xmm9, Operand(rsp, 16 * 3));
    masm.vmovdqa(xmm10, Operand(rsp, 16 * 4));
    masm.vmovdqa(xmm11, Operand(rsp, 16 * 5));
    masm.vmovdqa(xmm12, Operand(rsp, 16 * 6));
    masm.vmovdqa(xmm13, Operand(rsp, 16 * 7));
    masm.vmovdqa(xmm14, Operand(rsp, 16 * 8));
    masm.vmovdqa(xmm15, Operand(rsp, 16 * 9));
#endif

    // Save arguments passed in registers needed after function call.
    masm.push(result);

    // Remember stack depth without padding and arguments.
    masm.mov(rsp, r14);

    // Remember number of bytes occupied by argument vector
    masm.mov(reg_argc, r13);

    // if we are constructing, that also needs to include newTarget
    {
        Label noNewTarget;
        masm.branchTest32(Assembler::Zero, token, Imm32(CalleeToken_FunctionConstructing),
                          &noNewTarget);

        masm.addq(Imm32(1), r13);

        masm.bind(&noNewTarget);
    }

    masm.shll(Imm32(3), r13);   // r13 = argc * sizeof(Value)
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
    static_assert(sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");
    masm.andl(Imm32(JitStackAlignment - 1), r12);
    masm.subq(r12, rsp);

    /***************************************************************
    Loop over argv vector, push arguments onto stack in reverse order
    ***************************************************************/

    // r13 still stores the number of bytes in the argument vector.
    masm.addq(reg_argv, r13); // r13 points above last argument or newTarget

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

    // Push the number of actual arguments.  |result| is used to store the
    // actual number of arguments without adding an extra argument to the enter
    // JIT.
    masm.movq(result, reg_argc);
    masm.unboxInt32(Operand(reg_argc, 0), reg_argc);
    masm.push(reg_argc);

    // Push the callee token.
    masm.push(token);

    /*****************************************************************
    Push the number of bytes we've pushed so far on the stack and call
    *****************************************************************/
    masm.subq(rsp, r14);

    // Create a frame descriptor.
    masm.makeFrameDescriptor(r14, JitFrame_CppToJSJit, JitFrameLayout::Size());
    masm.push(r14);

    CodeLabel returnLabel;
    CodeLabel oomReturnLabel;
    {
        // Handle Interpreter -> Baseline OSR.
        AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
        regs.takeUnchecked(OsrFrameReg);
        regs.take(rbp);
        regs.take(reg_code);

        // Ensure that |scratch| does not end up being JSReturnOperand.
        // Do takeUnchecked because on Win64/x64, reg_code (IntArgReg0) and JSReturnOperand are
        // the same (rcx).  See bug 849398.
        regs.takeUnchecked(JSReturnOperand);
        Register scratch = regs.takeAny();

        Label notOsr;
        masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

        Register numStackValues = regs.takeAny();
        masm.movq(numStackValuesAddr, numStackValues);

        // Push return address
        masm.mov(&returnLabel, scratch);
        masm.push(scratch);

        // Push previous frame pointer.
        masm.push(rbp);

        // Reserve frame.
        Register framePtr = rbp;
        masm.subPtr(Imm32(BaselineFrame::Size()), rsp);
        masm.mov(rsp, framePtr);

#ifdef XP_WIN
        // Can't push large frames blindly on windows.  Touch frame memory incrementally.
        masm.mov(numStackValues, scratch);
        masm.lshiftPtr(Imm32(3), scratch);
        masm.subPtr(scratch, framePtr);
        {
            masm.movePtr(rsp, scratch);
            masm.subPtr(Imm32(WINDOWS_BIG_FRAME_TOUCH_INCREMENT), scratch);

            Label touchFrameLoop;
            Label touchFrameLoopEnd;
            masm.bind(&touchFrameLoop);
            masm.branchPtr(Assembler::Below, scratch, framePtr, &touchFrameLoopEnd);
            masm.store32(Imm32(0), Address(scratch, 0));
            masm.subPtr(Imm32(WINDOWS_BIG_FRAME_TOUCH_INCREMENT), scratch);
            masm.jump(&touchFrameLoop);
            masm.bind(&touchFrameLoopEnd);
        }
        masm.mov(rsp, framePtr);
#endif

        // Reserve space for locals and stack values.
        Register valuesSize = regs.takeAny();
        masm.mov(numStackValues, valuesSize);
        masm.shll(Imm32(3), valuesSize);
        masm.subPtr(valuesSize, rsp);

        // Enter exit frame.
        masm.addPtr(Imm32(BaselineFrame::Size() + BaselineFrame::FramePointerOffset), valuesSize);
        masm.makeFrameDescriptor(valuesSize, JitFrame_BaselineJS, ExitFrameLayout::Size());
        masm.push(valuesSize);
        masm.push(Imm32(0)); // Fake return address.
        // No GC things to mark, push a bare token.
        masm.loadJSContext(scratch);
        masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

        regs.add(valuesSize);

        masm.push(framePtr);
        masm.push(reg_code);

        masm.setupUnalignedABICall(scratch);
        masm.passABIArg(framePtr); // BaselineFrame
        masm.passABIArg(OsrFrameReg); // InterpreterFrame
        masm.passABIArg(numStackValues);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, jit::InitBaselineFrameForOsr), MoveOp::GENERAL,
                         CheckUnsafeCallWithABI::DontCheckHasExitFrame);

        masm.pop(reg_code);
        masm.pop(framePtr);

        MOZ_ASSERT(reg_code != ReturnReg);

        Label error;
        masm.addPtr(Imm32(ExitFrameLayout::SizeWithFooter()), rsp);
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
            masm.lea(Operand(framePtr, sizeof(void*)), realFramePtr);
            masm.profilerEnterFrame(realFramePtr, scratch);
            masm.bind(&skipProfilingInstrumentation);
        }

        masm.jump(reg_code);

        // OOM: load error value, discard return address and previous frame
        // pointer and return.
        masm.bind(&error);
        masm.mov(framePtr, rsp);
        masm.addPtr(Imm32(2 * sizeof(uintptr_t)), rsp);
        masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
        masm.mov(&oomReturnLabel, scratch);
        masm.jump(scratch);

        masm.bind(&notOsr);
        masm.movq(scopeChain, R1.scratchReg());
    }

    // The call will push the return address on the stack, thus we check that
    // the stack would be aligned once the call is complete.
    masm.assertStackAlignment(JitStackAlignment, sizeof(uintptr_t));

    // Call function.
    masm.callJitNoProfiler(reg_code);

    {
        // Interpreter -> Baseline OSR will return here.
        masm.bind(&returnLabel);
        masm.addCodeLabel(returnLabel);
        masm.bind(&oomReturnLabel);
        masm.addCodeLabel(oomReturnLabel);
    }

    // Pop arguments and padding from stack.
    masm.pop(r14);              // Pop and decode descriptor.
    masm.shrq(Imm32(FRAMESIZE_SHIFT), r14);
    masm.addq(r14, rsp);        // Remove arguments.

    /*****************************************************************
    Place return value where it belongs, pop all saved registers
    *****************************************************************/
    masm.pop(r12); // vp
    masm.storeValue(JSReturnOperand, Operand(r12, 0));

    // Restore non-volatile registers.
#if defined(_WIN64)
    masm.vmovdqa(Operand(rsp, 16 * 0), xmm6);
    masm.vmovdqa(Operand(rsp, 16 * 1), xmm7);
    masm.vmovdqa(Operand(rsp, 16 * 2), xmm8);
    masm.vmovdqa(Operand(rsp, 16 * 3), xmm9);
    masm.vmovdqa(Operand(rsp, 16 * 4), xmm10);
    masm.vmovdqa(Operand(rsp, 16 * 5), xmm11);
    masm.vmovdqa(Operand(rsp, 16 * 6), xmm12);
    masm.vmovdqa(Operand(rsp, 16 * 7), xmm13);
    masm.vmovdqa(Operand(rsp, 16 * 8), xmm14);
    masm.vmovdqa(Operand(rsp, 16 * 9), xmm15);

    masm.addq(Imm32(16 * 10 + 8), rsp);

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

void
JitRuntime::generateInvalidator(MacroAssembler& masm, Label* bailoutTail)
{
    // See explanatory comment in x86's JitRuntime::generateInvalidator.

    invalidatorOffset_ = startTrampolineCode(masm);

    masm.addq(Imm32(sizeof(uintptr_t)), rsp);

    // Push registers such that we can access them from [base + code].
    masm.PushRegsInMask(AllRegs);

    masm.movq(rsp, rax); // Argument to jit::InvalidationBailout.

    // Make space for InvalidationBailout's frameSize outparam.
    masm.reserveStack(sizeof(size_t));
    masm.movq(rsp, rbx);

    // Make space for InvalidationBailout's bailoutInfo outparam.
    masm.reserveStack(sizeof(void*));
    masm.movq(rsp, r9);

    masm.setupUnalignedABICall(rdx);
    masm.passABIArg(rax);
    masm.passABIArg(rbx);
    masm.passABIArg(r9);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, InvalidationBailout), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckOther);

    masm.pop(r9); // Get the bailoutInfo outparam.
    masm.pop(rbx); // Get the frameSize outparam.

    // Pop the machine state and the dead frame.
    masm.lea(Operand(rsp, rbx, TimesOne, sizeof(InvalidationBailoutStack)), rsp);

    // Jump to shared bailout tail. The BailoutInfo pointer has to be in r9.
    masm.jmp(bailoutTail);
}

void
JitRuntime::generateArgumentsRectifier(MacroAssembler& masm)
{
    // Do not erase the frame pointer in this function.

    argumentsRectifierOffset_ = startTrampolineCode(masm);

    // Caller:
    // [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]] <- rsp

    // Add |this|, in the counter of known arguments.
    masm.loadPtr(Address(rsp, RectifierFrameLayout::offsetOfNumActualArgs()), r8);
    masm.addl(Imm32(1), r8);

    // Load |nformals| into %rcx.
    masm.loadPtr(Address(rsp, RectifierFrameLayout::offsetOfCalleeToken()), rax);
    masm.mov(rax, rcx);
    masm.andq(Imm32(uint32_t(CalleeTokenMask)), rcx);
    masm.movzwl(Operand(rcx, JSFunction::offsetOfNargs()), rcx);

    // Stash another copy in r11, since we are going to do destructive operations
    // on rcx
    masm.mov(rcx, r11);

    static_assert(CalleeToken_FunctionConstructing == 1,
      "Ensure that we can use the constructing bit to count the value");
    masm.mov(rax, rdx);
    masm.andq(Imm32(uint32_t(CalleeToken_FunctionConstructing)), rdx);

    // Including |this|, and |new.target|, there are (|nformals| + 1 + isConstructing)
    // arguments to push to the stack.  Then we push a JitFrameLayout.  We
    // compute the padding expressed in the number of extra |undefined| values
    // to push on the stack.
    static_assert(sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");
    static_assert(JitStackAlignment % sizeof(Value) == 0,
      "Ensure that we can pad the stack by pushing extra UndefinedValue");
    static_assert(IsPowerOfTwo(JitStackValueAlignment),
                  "must have power of two for masm.andl to do its job");

    masm.addl(Imm32(JitStackValueAlignment - 1 /* for padding */ + 1 /* for |this| */), rcx);
    masm.addl(rdx, rcx);
    masm.andl(Imm32(~(JitStackValueAlignment - 1)), rcx);

    // Load the number of |undefined|s to push into %rcx.
    masm.subq(r8, rcx);

    // Caller:
    // [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]] <- rsp <- r9
    // '------ #r8 -------'
    //
    // Rectifier frame:
    // [undef] [undef] [undef] [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]]
    // '------- #rcx --------' '------ #r8 -------'

    // Copy the number of actual arguments into rdx. Use lea to subtract 1 for
    // |this|.
    masm.lea(Operand(r8, -1), rdx);

    masm.moveValue(UndefinedValue(), ValueOperand(r10));

    masm.movq(rsp, r9); // Save %rsp.

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

    // | - sizeof(Value)| is used to put rcx such that we can read the last
    // argument, and not the value which is after.
    BaseIndex b = BaseIndex(r9, r8, TimesEight, sizeof(RectifierFrameLayout) - sizeof(Value));
    masm.lea(Operand(b), rcx);

    // Copy & Push arguments, |nargs| + 1 times (to include |this|).
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

        masm.branchTest32(Assembler::Zero, rax, Imm32(CalleeToken_FunctionConstructing),
                          &notConstructing);

        // thisFrame[numFormals] = prevFrame[argc]
        ValueOperand newTarget(r10);

        // +1 for |this|. We want vp[argc], so don't subtract 1
        BaseIndex newTargetSrc(r9, rdx, TimesEight, sizeof(RectifierFrameLayout) + sizeof(Value));
        masm.loadValue(newTargetSrc, newTarget);

        // Again, 1 for |this|
        BaseIndex newTargetDest(rsp, r11, TimesEight, sizeof(Value));
        masm.storeValue(newTarget, newTargetDest);

        masm.bind(&notConstructing);
    }


    // Caller:
    // [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]] <- r9
    //
    //
    // Rectifier frame:
    // [undef] [undef] [undef] [arg2] [arg1] [this] <- rsp [[argc] [callee] [descr] [raddr]]
    //

    // Construct descriptor.
    masm.subq(rsp, r9);
    masm.makeFrameDescriptor(r9, JitFrame_Rectifier, JitFrameLayout::Size());

    // Construct JitFrameLayout.
    masm.push(rdx); // numActualArgs
    masm.push(rax); // callee token
    masm.push(r9); // descriptor

    // Call the target function.
    masm.andq(Imm32(uint32_t(CalleeTokenMask)), rax);
    masm.loadJitCodeRaw(rax, rax);
    argumentsRectifierReturnOffset_ = masm.callJitNoProfiler(rax);

    // Remove the rectifier frame.
    masm.pop(r9);             // r9 <- descriptor with FrameType.
    masm.shrq(Imm32(FRAMESIZE_SHIFT), r9);
    masm.pop(r11);            // Discard calleeToken.
    masm.pop(r11);            // Discard numActualArgs.
    masm.addq(r9, rsp);       // Discard pushed arguments.

    masm.ret();
}

static void
PushBailoutFrame(MacroAssembler& masm, Register spArg)
{
    // Push registers such that we can access them from [base + code].
    if (JitSupportsSimd()) {
        masm.PushRegsInMask(AllRegs);
    } else {
        // When SIMD isn't supported, PushRegsInMask reduces the set of float
        // registers to be double-sized, while the RegisterDump expects each of
        // the float registers to have the maximal possible size
        // (Simd128DataSize). To work around this, we just spill the double
        // registers by hand here, using the register dump offset directly.
        for (GeneralRegisterBackwardIterator iter(AllRegs.gprs()); iter.more(); ++iter)
            masm.Push(*iter);

        masm.reserveStack(sizeof(RegisterDump::FPUArray));
        for (FloatRegisterBackwardIterator iter(AllRegs.fpus()); iter.more(); ++iter) {
            FloatRegister reg = *iter;
            Address spillAddress(StackPointer, reg.getRegisterDumpOffsetInBytes());
            masm.storeDouble(reg, spillAddress);
        }
    }

    // Get the stack pointer into a register, pre-alignment.
    masm.movq(rsp, spArg);
}

static void
GenerateBailoutThunk(MacroAssembler& masm, uint32_t frameClass, Label* bailoutTail)
{
    PushBailoutFrame(masm, r8);

    // Make space for Bailout's bailoutInfo outparam.
    masm.reserveStack(sizeof(void*));
    masm.movq(rsp, r9);

    // Call the bailout function.
    masm.setupUnalignedABICall(rax);
    masm.passABIArg(r8);
    masm.passABIArg(r9);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, Bailout), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckOther);

    masm.pop(r9); // Get the bailoutInfo outparam.

    // Stack is:
    //     [frame]
    //     snapshotOffset
    //     frameSize
    //     [bailoutFrame]
    //
    // Remove both the bailout frame and the topmost Ion frame's stack.
    static const uint32_t BailoutDataSize = sizeof(RegisterDump);
    masm.addq(Imm32(BailoutDataSize), rsp);
    masm.pop(rcx);
    masm.lea(Operand(rsp, rcx, TimesOne, sizeof(void*)), rsp);

    // Jump to shared bailout tail. The BailoutInfo pointer has to be in r9.
    masm.jmp(bailoutTail);
}

JitRuntime::BailoutTable
JitRuntime::generateBailoutTable(MacroAssembler& masm, Label* bailoutTail, uint32_t frameClass)
{
    MOZ_CRASH("x64 does not use bailout tables");
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

    // Avoid conflicts with argument registers while discarding the result after
    // the function call.
    AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

    static_assert((Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
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
    // We're aligned to an exit frame, so link it up.
    masm.loadJSContext(cxreg);
    masm.enterExitFrame(cxreg, regs.getAny(), &f);

    // Save the current stack pointer as the base for copying arguments.
    Register argsBase = InvalidReg;
    if (f.explicitArgs) {
        argsBase = r10;
        regs.take(argsBase);
        masm.lea(Operand(rsp, ExitFrameLayout::SizeWithFooter()), argsBase);
    }

    // Reserve space for the outparameter.
    Register outReg = InvalidReg;
    switch (f.outParam) {
      case Type_Value:
        outReg = regs.takeAny();
        masm.reserveStack(sizeof(Value));
        masm.movq(esp, outReg);
        break;

      case Type_Handle:
        outReg = regs.takeAny();
        masm.PushEmptyRooted(f.outParamRootType);
        masm.movq(esp, outReg);
        break;

      case Type_Int32:
      case Type_Bool:
        outReg = regs.takeAny();
        masm.reserveStack(sizeof(int32_t));
        masm.movq(esp, outReg);
        break;

      case Type_Double:
        outReg = regs.takeAny();
        masm.reserveStack(sizeof(double));
        masm.movq(esp, outReg);
        break;

      case Type_Pointer:
        outReg = regs.takeAny();
        masm.reserveStack(sizeof(uintptr_t));
        masm.movq(esp, outReg);
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

    // Copy arguments.
    for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
        MoveOperand from;
        switch (f.argProperties(explicitArg)) {
          case VMFunction::WordByValue:
            if (f.argPassedInFloatReg(explicitArg))
                masm.passABIArg(MoveOperand(argsBase, argDisp), MoveOp::DOUBLE);
            else
                masm.passABIArg(MoveOperand(argsBase, argDisp), MoveOp::GENERAL);
            argDisp += sizeof(void*);
            break;
          case VMFunction::WordByRef:
            masm.passABIArg(MoveOperand(argsBase, argDisp, MoveOperand::EFFECTIVE_ADDRESS),
                            MoveOp::GENERAL);
            argDisp += sizeof(void*);
            break;
          case VMFunction::DoubleByValue:
          case VMFunction::DoubleByRef:
            MOZ_CRASH("NYI: x64 callVM should not be used with 128bits values.");
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

    // Load the outparam and free any allocated stack.
    switch (f.outParam) {
      case Type_Handle:
        masm.popRooted(f.outParamRootType, ReturnReg, JSReturnOperand);
        break;

      case Type_Value:
        masm.loadValue(Address(esp, 0), JSReturnOperand);
        masm.freeStack(sizeof(Value));
        break;

      case Type_Int32:
        masm.load32(Address(esp, 0), ReturnReg);
        masm.freeStack(sizeof(int32_t));
        break;

      case Type_Bool:
        masm.load8ZeroExtend(Address(esp, 0), ReturnReg);
        masm.freeStack(sizeof(int32_t));
        break;

      case Type_Double:
        MOZ_ASSERT(cx->runtime()->jitSupportsFloatingPoint);
        masm.loadDouble(Address(esp, 0), ReturnDoubleReg);
        masm.freeStack(sizeof(double));
        break;

      case Type_Pointer:
        masm.loadPtr(Address(esp, 0), ReturnReg);
        masm.freeStack(sizeof(uintptr_t));
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

    MOZ_ASSERT(PreBarrierReg == rdx);
    Register temp1 = rax;
    Register temp2 = rbx;
    Register temp3 = rcx;
    masm.push(temp1);
    masm.push(temp2);
    masm.push(temp3);

    Label noBarrier;
    masm.emitPreBarrierFastPath(cx->runtime(), type, temp1, temp2, temp3, &noBarrier);

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
    masm.callWithABI(JitMarkFunction(type));

    masm.PopRegsInMask(regs);
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
#ifndef JS_USE_LINK_REGISTER
    // The first value contains the return addres,
    // which we pull into ICTailCallReg for tail calls.
    masm.setFramePushed(sizeof(intptr_t));
#endif

    Register scratch1 = rax;
    Register scratch2 = rcx;
    Register scratch3 = rdx;

    // Load the return address in scratch1.
    masm.loadPtr(Address(rsp, 0), scratch1);

    // Load BaselineFrame pointer in scratch2.
    masm.mov(rbp, scratch2);
    masm.subPtr(Imm32(BaselineFrame::Size()), scratch2);

    // Enter a stub frame and call the HandleDebugTrap VM function. Ensure
    // the stub frame has a nullptr ICStub pointer, since this pointer is marked
    // during GC.
    masm.movePtr(ImmPtr(nullptr), ICStubReg);
    EmitBaselineEnterStubFrame(masm, scratch3);

    TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(HandleDebugTrapInfo);
    masm.push(scratch1);
    masm.push(scratch2);
    EmitBaselineCallVM(code, masm);

    EmitBaselineLeaveStubFrame(masm);

    // If the stub returns |true|, we have to perform a forced return
    // (return from the JS frame). If the stub returns |false|, just return
    // from the trap stub so that execution continues at the current pc.
    Label forcedReturn;
    masm.branchTest32(Assembler::NonZero, ReturnReg, ReturnReg, &forcedReturn);
    masm.ret();

    masm.bind(&forcedReturn);
    masm.loadValue(Address(ebp, BaselineFrame::reverseOffsetOfReturnValue()),
                   JSReturnOperand);
    masm.mov(rbp, rsp);
    masm.pop(rbp);

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
    JitCode* codeDbg = linker.newCode(cx, CodeKind::Other);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(codeDbg, "DebugTrapHandler");
#endif
#ifdef MOZ_VTUNE
    vtune::MarkStub(codeDbg, "DebugTrapHandler");
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

    masm.generateBailoutTail(rdx, r9);
}

void
JitRuntime::generateProfilerExitFrameTailStub(MacroAssembler& masm, Label* profilerExitTail)
{
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
    masm.movePtr(scratch1, scratch2);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch1);
    masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch2);

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
        // returning directly to an IonJS frame.  Store return addr to frame
        // in lastProfilingCallSite.
        masm.loadPtr(Address(StackPointer, JitFrameLayout::offsetOfReturnAddress()), scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        // Store return frame in lastProfilingFrame.
        // scratch2 := StackPointer + Descriptor.size*1 + JitFrameLayout::Size();
        masm.lea(Operand(StackPointer, scratch1, TimesOne, JitFrameLayout::Size()), scratch2);
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
        BaseIndex stubFrameReturnAddr(StackPointer, scratch1, TimesOne,
                                      JitFrameLayout::Size() +
                                      BaselineStubFrameLayout::offsetOfReturnAddress());
        masm.loadPtr(stubFrameReturnAddr, scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        BaseIndex stubFrameSavedFramePtr(StackPointer, scratch1, TimesOne,
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
        // scratch2 := StackPointer + Descriptor.size + JitFrameLayout::Size()
        masm.lea(Operand(StackPointer, scratch1, TimesOne, JitFrameLayout::Size()), scratch2);
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfDescriptor()), scratch3);
        masm.movePtr(scratch3, scratch1);
        masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch3);
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch1);

        // Now |scratch1| contains Rect-Descriptor.Size
        // and |scratch2| points to Rectifier frame
        // and |scratch3| contains Rect-Descriptor.Type

        masm.assertRectifierFrameParentType(scratch3);

        // Check for either Ion or something else frame.
        Label notIonFrame;
        masm.branch32(Assembler::NotEqual, scratch3, Imm32(JitFrame_IonJS), &notIonFrame);

        // Handle Rectifier <- IonJS
        // scratch3 := RectFrame[ReturnAddr]
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfReturnAddress()), scratch3);
        masm.storePtr(scratch3, lastProfilingCallSite);

        // scratch3 := RectFrame + Rect-Descriptor.Size + RectifierFrameLayout::Size()
        masm.lea(Operand(scratch2, scratch1, TimesOne, RectifierFrameLayout::Size()), scratch3);
        masm.storePtr(scratch3, lastProfilingFrame);
        masm.ret();

        masm.bind(&notIonFrame);

        // Check for either BaselineStub or a CppToJSJit/WasmToJSJit entry
        // frame.
        masm.branch32(Assembler::NotEqual, scratch3, Imm32(JitFrame_BaselineStub), &handle_Entry);

        // Handle Rectifier <- BaselineStub <- BaselineJS
        BaseIndex stubFrameReturnAddr(scratch2, scratch1, TimesOne,
                                         RectifierFrameLayout::Size() +
                                         BaselineStubFrameLayout::offsetOfReturnAddress());
        masm.loadPtr(stubFrameReturnAddr, scratch3);
        masm.storePtr(scratch3, lastProfilingCallSite);

        BaseIndex stubFrameSavedFramePtr(scratch2, scratch1, TimesOne,
                                         RectifierFrameLayout::Size() - (2 * sizeof(void*)));
        masm.loadPtr(stubFrameSavedFramePtr, scratch3);
        masm.addPtr(Imm32(sizeof(void*)), scratch3);
        masm.storePtr(scratch3, lastProfilingFrame);
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
        masm.lea(Operand(StackPointer, scratch1, TimesOne, JitFrameLayout::Size()), scratch2);

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
        masm.lea(Operand(scratch2, scratch3, TimesOne, IonICCallFrameLayout::Size()), scratch1);
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
