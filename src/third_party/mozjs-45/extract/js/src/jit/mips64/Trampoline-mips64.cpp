/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "jscompartment.h"

#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/mips64/Bailouts-mips64.h"
#include "jit/mips64/SharedICHelpers-mips64.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/VMFunctions.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

// All registers to save and restore. This includes the stack pointer, since we
// use the ability to reference register values on the stack by index.
static const LiveRegisterSet AllRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllMask),
                         FloatRegisterSet(FloatRegisters::AllMask));

static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "Not 32-bit clean.");

struct EnterJITRegs
{
    double f31;
    double f30;
    double f29;
    double f28;
    double f27;
    double f26;
    double f25;
    double f24;

    // non-volatile registers.
    uint64_t ra;
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

static void
GenerateReturn(MacroAssembler& masm, int returnCode)
{
    MOZ_ASSERT(masm.framePushed() == sizeof(EnterJITRegs));

    // Restore non-volatile registers
    masm.as_ld(s0, StackPointer, offsetof(EnterJITRegs, s0));
    masm.as_ld(s1, StackPointer, offsetof(EnterJITRegs, s1));
    masm.as_ld(s2, StackPointer, offsetof(EnterJITRegs, s2));
    masm.as_ld(s3, StackPointer, offsetof(EnterJITRegs, s3));
    masm.as_ld(s4, StackPointer, offsetof(EnterJITRegs, s4));
    masm.as_ld(s5, StackPointer, offsetof(EnterJITRegs, s5));
    masm.as_ld(s6, StackPointer, offsetof(EnterJITRegs, s6));
    masm.as_ld(s7, StackPointer, offsetof(EnterJITRegs, s7));
    masm.as_ld(ra, StackPointer, offsetof(EnterJITRegs, ra));

    // Restore non-volatile floating point registers
    masm.as_ld(f24, StackPointer, offsetof(EnterJITRegs, f24));
    masm.as_ld(f25, StackPointer, offsetof(EnterJITRegs, f25));
    masm.as_ld(f26, StackPointer, offsetof(EnterJITRegs, f26));
    masm.as_ld(f27, StackPointer, offsetof(EnterJITRegs, f27));
    masm.as_ld(f28, StackPointer, offsetof(EnterJITRegs, f28));
    masm.as_ld(f29, StackPointer, offsetof(EnterJITRegs, f29));
    masm.as_ld(f30, StackPointer, offsetof(EnterJITRegs, f30));
    masm.as_ld(f31, StackPointer, offsetof(EnterJITRegs, f31));

    masm.freeStack(sizeof(EnterJITRegs));

    masm.branch(ra);
}

static void
GeneratePrologue(MacroAssembler& masm)
{
    masm.reserveStack(sizeof(EnterJITRegs));

    masm.as_sd(s0, StackPointer, offsetof(EnterJITRegs, s0));
    masm.as_sd(s1, StackPointer, offsetof(EnterJITRegs, s1));
    masm.as_sd(s2, StackPointer, offsetof(EnterJITRegs, s2));
    masm.as_sd(s3, StackPointer, offsetof(EnterJITRegs, s3));
    masm.as_sd(s4, StackPointer, offsetof(EnterJITRegs, s4));
    masm.as_sd(s5, StackPointer, offsetof(EnterJITRegs, s5));
    masm.as_sd(s6, StackPointer, offsetof(EnterJITRegs, s6));
    masm.as_sd(s7, StackPointer, offsetof(EnterJITRegs, s7));
    masm.as_sd(ra, StackPointer, offsetof(EnterJITRegs, ra));
    masm.as_sd(a7, StackPointer, offsetof(EnterJITRegs, a7));

    masm.as_sd(f24, StackPointer, offsetof(EnterJITRegs, f24));
    masm.as_sd(f25, StackPointer, offsetof(EnterJITRegs, f25));
    masm.as_sd(f26, StackPointer, offsetof(EnterJITRegs, f26));
    masm.as_sd(f27, StackPointer, offsetof(EnterJITRegs, f27));
    masm.as_sd(f28, StackPointer, offsetof(EnterJITRegs, f28));
    masm.as_sd(f29, StackPointer, offsetof(EnterJITRegs, f29));
    masm.as_sd(f30, StackPointer, offsetof(EnterJITRegs, f30));
    masm.as_sd(f31, StackPointer, offsetof(EnterJITRegs, f31));
}


// Generates a trampoline for calling Jit compiled code from a C++ function.
// The trampoline use the EnterJitCode signature, with the standard x64 fastcall
// calling convention.
JitCode *
JitRuntime::generateEnterJIT(JSContext* cx, EnterJitType type)
{
    const Register reg_code = IntArgReg0;
    const Register reg_argc = IntArgReg1;
    const Register reg_argv = IntArgReg2;
    const mozilla::DebugOnly<Register> reg_frame = IntArgReg3;
    const Register reg_token = IntArgReg4;
    const Register reg_chain = IntArgReg5;
    const Register reg_values = IntArgReg6;
    const Register reg_vp = IntArgReg7;
    MacroAssembler masm(cx);

    MOZ_ASSERT(OsrFrameReg == reg_frame);

    GeneratePrologue(masm);

    // Save stack pointer into s4
    masm.movePtr(StackPointer, s4);

    // Save stack pointer as baseline frame.
    if (type == EnterJitBaseline)
        masm.movePtr(StackPointer, BaselineFrameReg);

    // Load the number of actual arguments into s3.
    masm.unboxInt32(Address(reg_vp, 0), s3);

    /***************************************************************
    Loop over argv vector, push arguments onto stack in reverse order
    ***************************************************************/

    // if we are constructing, that also needs to include newTarget
    {
        Label noNewTarget;
        masm.branchTest32(Assembler::Zero, reg_token, Imm32(CalleeToken_FunctionConstructing),
                          &noNewTarget);

        masm.add32(Imm32(1), reg_argc);

        masm.bind(&noNewTarget);
    }

    // Make stack algined
    masm.ma_and(s0, reg_argc, Imm32(1));
    masm.ma_dsubu(s1, StackPointer, Imm32(sizeof(Value)));
    masm.as_movn(StackPointer, s1, s0);

    masm.as_dsll(s0, reg_argc, 3); // Value* argv
    masm.addPtr(reg_argv, s0); // s0 = &argv[argc]

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

    masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    masm.storePtr(s3, Address(StackPointer, sizeof(uintptr_t))); // actual arguments
    masm.storePtr(reg_token, Address(StackPointer, 0)); // callee token

    masm.subPtr(StackPointer, s4);
    masm.makeFrameDescriptor(s4, JitFrame_Entry);
    masm.push(s4); // descriptor

    CodeLabel returnLabel;
    CodeLabel oomReturnLabel;
    if (type == EnterJitBaseline) {
        // Handle OSR.
        AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
        regs.take(OsrFrameReg);
        regs.take(BaselineFrameReg);
        regs.take(reg_code);
        regs.take(ReturnReg);
        regs.take(JSReturnOperand);

        Label notOsr;
        masm.ma_b(OsrFrameReg, OsrFrameReg, &notOsr, Assembler::Zero, ShortJump);

        Register numStackValues = reg_values;
        regs.take(numStackValues);
        Register scratch = regs.takeAny();

        // Push return address.
        masm.subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
        masm.ma_li(scratch, returnLabel.patchAt());
        masm.storePtr(scratch, Address(StackPointer, 0));

        // Push previous frame pointer.
        masm.subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
        masm.storePtr(BaselineFrameReg, Address(StackPointer, 0));

        // Reserve frame.
        Register framePtr = BaselineFrameReg;
        masm.subPtr(Imm32(BaselineFrame::Size()), StackPointer);
        masm.movePtr(StackPointer, framePtr);

        // Reserve space for locals and stack values.
        masm.ma_dsll(scratch, numStackValues, Imm32(3));
        masm.subPtr(scratch, StackPointer);

        // Enter exit frame.
        masm.addPtr(Imm32(BaselineFrame::Size() + BaselineFrame::FramePointerOffset), scratch);
        masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS);

        // Push frame descriptor and fake return address.
        masm.reserveStack(2 * sizeof(uintptr_t));
        masm.storePtr(scratch, Address(StackPointer, sizeof(uintptr_t))); // Frame descriptor
        masm.storePtr(zero, Address(StackPointer, 0)); // fake return address

        // No GC things to mark, push a bare token.
        masm.enterFakeExitFrame(ExitFrameLayoutBareToken);

        masm.reserveStack(2 * sizeof(uintptr_t));
        masm.storePtr(framePtr, Address(StackPointer, sizeof(uintptr_t))); // BaselineFrame
        masm.storePtr(reg_code, Address(StackPointer, 0)); // jitcode

        masm.setupUnalignedABICall(scratch);
        masm.passABIArg(BaselineFrameReg); // BaselineFrame
        masm.passABIArg(OsrFrameReg); // InterpreterFrame
        masm.passABIArg(numStackValues);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, jit::InitBaselineFrameForOsr));

        regs.add(OsrFrameReg);
        Register jitcode = regs.takeAny();
        masm.loadPtr(Address(StackPointer, 0), jitcode);
        masm.loadPtr(Address(StackPointer, sizeof(uintptr_t)), framePtr);
        masm.freeStack(2 * sizeof(uintptr_t));

        Label error;
        masm.freeStack(ExitFrameLayout::SizeWithFooter());
        masm.addPtr(Imm32(BaselineFrame::Size()), framePtr);
        masm.branchIfFalseBool(ReturnReg, &error);

        // If OSR-ing, then emit instrumentation for setting lastProfilerFrame
        // if profiler instrumentation is enabled.
        {
            Label skipProfilingInstrumentation;
            Register realFramePtr = numStackValues;
            AbsoluteAddress addressOfEnabled(cx->runtime()->spsProfiler.addressOfEnabled());
            masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                          &skipProfilingInstrumentation);
            masm.ma_daddu(realFramePtr, framePtr, Imm32(sizeof(void*)));
            masm.profilerEnterFrame(realFramePtr, scratch);
            masm.bind(&skipProfilingInstrumentation);
        }

        masm.jump(jitcode);

        // OOM: load error value, discard return address and previous frame
        // pointer and return.
        masm.bind(&error);
        masm.movePtr(framePtr, StackPointer);
        masm.addPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
        masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
        masm.ma_li(scratch, oomReturnLabel.patchAt());
        masm.jump(scratch);

        masm.bind(&notOsr);
        // Load the scope chain in R1.
        MOZ_ASSERT(R1.scratchReg() != reg_code);
        masm.ma_move(R1.scratchReg(), reg_chain);
    }

    // The call will push the return address on the stack, thus we check that
    // the stack would be aligned once the call is complete.
    masm.assertStackAlignment(JitStackAlignment, sizeof(uintptr_t));

    // Call the function with pushing return address to stack.
    masm.callJitNoProfiler(reg_code);

    if (type == EnterJitBaseline) {
        // Baseline OSR will return here.
        masm.bind(returnLabel.target());
        masm.addCodeLabel(returnLabel);
        masm.bind(oomReturnLabel.target());
        masm.addCodeLabel(oomReturnLabel);
    }

    // Pop arguments off the stack.
    // s0 <- 8*argc (size of all arguments we pushed on the stack)
    masm.pop(s0);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), s0);
    masm.addPtr(s0, StackPointer);

    // Store the returned value into the vp
    masm.as_ld(reg_vp, StackPointer, offsetof(EnterJITRegs, a7));
    masm.storeValue(JSReturnOperand, Address(reg_vp, 0));

    // Restore non-volatile registers and return.
    GenerateReturn(masm, ShortJump);

    Linker linker(masm);
    AutoFlushICache afc("GenerateEnterJIT");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "EnterJIT");
#endif

    return code;
}

JitCode*
JitRuntime::generateInvalidator(JSContext* cx)
{
    MacroAssembler masm(cx);

    // Stack has to be alligned here. If not, we will have to fix it.
    masm.checkStackAlignment();

    // Push registers such that we can access them from [base + code].
    masm.PushRegsInMask(AllRegs);

    // Pass pointer to InvalidationBailoutStack structure.
    masm.movePtr(StackPointer, a0);

    // Reserve place for return value and BailoutInfo pointer
    masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    // Pass pointer to return value.
    masm.ma_daddu(a1, StackPointer, Imm32(sizeof(uintptr_t)));
    // Pass pointer to BailoutInfo
    masm.movePtr(StackPointer, a2);

    masm.setupAlignedABICall();
    masm.passABIArg(a0);
    masm.passABIArg(a1);
    masm.passABIArg(a2);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, InvalidationBailout));

    masm.loadPtr(Address(StackPointer, 0), a2);
    masm.loadPtr(Address(StackPointer, sizeof(uintptr_t)), a1);
    // Remove the return address, the IonScript, the register state
    // (InvaliationBailoutStack) and the space that was allocated for the
    // return value.
    masm.addPtr(Imm32(sizeof(InvalidationBailoutStack) + 2 * sizeof(uintptr_t)), StackPointer);
    // remove the space that this frame was using before the bailout
    // (computed by InvalidationBailout)
    masm.addPtr(a1, StackPointer);

    // Jump to shared bailout tail. The BailoutInfo pointer has to be in r2.
    JitCode* bailoutTail = cx->runtime()->jitRuntime()->getBailoutTail();
    masm.branch(bailoutTail);

    Linker linker(masm);
    AutoFlushICache afc("Invalidator");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    JitSpew(JitSpew_IonInvalidate, "   invalidation thunk created at %p", (void*) code->raw());

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "Invalidator");
#endif

    return code;
}

JitCode*
JitRuntime::generateArgumentsRectifier(JSContext* cx, void** returnAddrOut)
{
    // Do not erase the frame pointer in this function.

    MacroAssembler masm(cx);
    // Caller:
    // [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]] <- sp
    // '--- s3 ---'

    // ArgumentsRectifierReg contains the |nargs| pushed onto the current
    // frame. Including |this|, there are (|nargs| + 1) arguments to copy.
    MOZ_ASSERT(ArgumentsRectifierReg == s3);

    // Add |this|, in the counter of known arguments.
    masm.addPtr(Imm32(1), ArgumentsRectifierReg);

    Register numActArgsReg = a6;
    Register calleeTokenReg = a7;
    Register numArgsReg = a5;

    // Load |nformals| into numArgsReg.
    masm.loadPtr(Address(StackPointer, RectifierFrameLayout::offsetOfCalleeToken()),
                 calleeTokenReg);
    masm.mov(calleeTokenReg, numArgsReg);
    masm.andPtr(Imm32(uint32_t(CalleeTokenMask)), numArgsReg);
    masm.load16ZeroExtend(Address(numArgsReg, JSFunction::offsetOfNargs()), numArgsReg);

    // Stash another copy in t3, since we are going to do destructive operations
    // on numArgsReg
    masm.mov(numArgsReg, t3);

    static_assert(CalleeToken_FunctionConstructing == 1,
      "Ensure that we can use the constructing bit to count the value");
    masm.mov(calleeTokenReg, t2);
    masm.ma_and(t2, Imm32(uint32_t(CalleeToken_FunctionConstructing)));

    // Including |this|, and |new.target|, there are (|nformals| + 1 + isConstructing)
    // arguments to push to the stack.  Then we push a JitFrameLayout.  We
    // compute the padding expressed in the number of extra |undefined| values
    // to push on the stack.
    static_assert(sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");
    static_assert(JitStackAlignment % sizeof(Value) == 0,
      "Ensure that we can pad the stack by pushing extra UndefinedValue");

    MOZ_ASSERT(IsPowerOfTwo(JitStackValueAlignment));
    masm.add32(Imm32(JitStackValueAlignment - 1 /* for padding */ + 1 /* for |this| */), numArgsReg);
    masm.add32(t2, numArgsReg);
    masm.and32(Imm32(~(JitStackValueAlignment - 1)), numArgsReg);

    // Load the number of |undefined|s to push into t1.
    masm.as_dsubu(t1, numArgsReg, s3);

    // Caller:
    // [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]] <- sp <- t2
    // '------ s3 -------'
    //
    // Rectifier frame:
    // [undef] [undef] [undef] [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]]
    // '-------- t1 ---------' '------- s3 -------'

    // Copy number of actual arguments into numActArgsReg
    masm.loadPtr(Address(StackPointer, RectifierFrameLayout::offsetOfNumActualArgs()),
                 numActArgsReg);


    masm.moveValue(UndefinedValue(), ValueOperand(t0));

    masm.movePtr(StackPointer, t2); // Save %sp.

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

    // | - sizeof(Value)| is used to put rcx such that we can read the last
    // argument, and not the value which is after.
    masm.ma_dsll(t0, s3, Imm32(3)); // t0 <- nargs * 8
    masm.as_daddu(t1, t2, t0); // t1 <- t2(saved sp) + nargs * 8
    masm.addPtr(Imm32(sizeof(RectifierFrameLayout) - sizeof(Value)), t1);

    // Copy & Push arguments, |nargs| + 1 times (to include |this|).
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

        masm.branchTest32(Assembler::Zero, calleeTokenReg, Imm32(CalleeToken_FunctionConstructing),
                          &notConstructing);

        // thisFrame[numFormals] = prevFrame[argc]
        ValueOperand newTarget(t0);

        // +1 for |this|. We want vp[argc], so don't subtract 1
        BaseIndex newTargetSrc(t2, numActArgsReg, TimesEight, sizeof(RectifierFrameLayout) + sizeof(Value));
        masm.loadValue(newTargetSrc, newTarget);

        // Again, 1 for |this|
        BaseIndex newTargetDest(StackPointer, t3, TimesEight, sizeof(Value));
        masm.storeValue(newTarget, newTargetDest);

        masm.bind(&notConstructing);
    }

    // Caller:
    // [arg2] [arg1] [this] [[argc] [callee] [descr] [raddr]] <- t2
    //
    //
    // Rectifier frame:
    // [undef] [undef] [undef] [arg2] [arg1] [this] <- sp [[argc] [callee] [descr] [raddr]]

    // Construct sizeDescriptor.
    masm.subPtr(StackPointer, t2);
    masm.makeFrameDescriptor(t2, JitFrame_Rectifier);

    // Construct JitFrameLayout.
    masm.subPtr(Imm32(3 * sizeof(uintptr_t)), StackPointer);
    // Push actual arguments.
    masm.storePtr(numActArgsReg, Address(StackPointer, 2 * sizeof(uintptr_t)));
    // Push callee token.
    masm.storePtr(calleeTokenReg, Address(StackPointer, sizeof(uintptr_t)));
    // Push frame descriptor.
    masm.storePtr(t2, Address(StackPointer, 0));

    // Call the target function.
    // Note that this code assumes the function is JITted.
    masm.andPtr(Imm32(uint32_t(CalleeTokenMask)), calleeTokenReg);
    masm.loadPtr(Address(calleeTokenReg, JSFunction::offsetOfNativeOrScript()), t1);
    masm.loadBaselineOrIonRaw(t1, t1, nullptr);
    uint32_t returnOffset = masm.callJitNoProfiler(t1);

    // Remove the rectifier frame.
    // t2 <- descriptor with FrameType.
    masm.loadPtr(Address(StackPointer, 0), t2);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), t2); // t2 <- descriptor.

    // Discard descriptor, calleeToken and number of actual arguments.
    masm.addPtr(Imm32(3 * sizeof(uintptr_t)), StackPointer);

    // Discard pushed arguments.
    masm.addPtr(t2, StackPointer);

    masm.ret();
    Linker linker(masm);
    AutoFlushICache afc("ArgumentsRectifier");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

    if (returnAddrOut)
        *returnAddrOut = (void*) (code->raw() + returnOffset);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "ArgumentsRectifier");
#endif

    return code;
}

/* - When bailout is done via out of line code (lazy bailout).
 * Frame size is stored in $ra (look at
 * CodeGeneratorMIPS64::generateOutOfLineCode()) and thunk code should save it
 * on stack. Other difference is that members snapshotOffset_ and padding_ are
 * pushed to the stack by CodeGeneratorMIPS64::visitOutOfLineBailout(). Field
 * frameClassId_ is forced to be NO_FRAME_SIZE_CLASS_ID
 * (See: JitRuntime::generateBailoutHandler).
 */
static void
PushBailoutFrame(MacroAssembler& masm, Register spArg)
{
    // Push the frameSize_ stored in ra
    // See: CodeGeneratorMIPS64::generateOutOfLineCode()
    masm.push(ra);

    // Push registers such that we can access them from [base + code].
    masm.PushRegsInMask(AllRegs);

    // Put pointer to BailoutStack as first argument to the Bailout()
    masm.movePtr(StackPointer, spArg);
}

static void
GenerateBailoutThunk(JSContext* cx, MacroAssembler& masm, uint32_t frameClass)
{
    PushBailoutFrame(masm, a0);

    // Put pointer to BailoutInfo
    static const uint32_t sizeOfBailoutInfo = sizeof(uintptr_t) * 2;
    masm.subPtr(Imm32(sizeOfBailoutInfo), StackPointer);
    masm.movePtr(StackPointer, a1);

    masm.setupAlignedABICall();
    masm.passABIArg(a0);
    masm.passABIArg(a1);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, Bailout));

    // Get BailoutInfo pointer
    masm.loadPtr(Address(StackPointer, 0), a2);

    // Stack is:
    //     [frame]
    //     snapshotOffset
    //     frameSize
    //     [bailoutFrame]
    //     [bailoutInfo]
    //
    // Remove both the bailout frame and the topmost Ion frame's stack.
    // Load frameSize from stack
    masm.loadPtr(Address(StackPointer,
                         sizeOfBailoutInfo + BailoutStack::offsetOfFrameSize()), a1);
    // Remove complete BailoutStack class and data after it
    masm.addPtr(Imm32(sizeof(BailoutStack) + sizeOfBailoutInfo), StackPointer);
    // Remove frame size srom stack
    masm.addPtr(a1, StackPointer);

    // Jump to shared bailout tail. The BailoutInfo pointer has to be in a2.
    JitCode* bailoutTail = cx->runtime()->jitRuntime()->getBailoutTail();
    masm.branch(bailoutTail);
}

JitCode*
JitRuntime::generateBailoutTable(JSContext* cx, uint32_t frameClass)
{
    MOZ_CRASH("MIPS64 does not use bailout tables");
}

JitCode*
JitRuntime::generateBailoutHandler(JSContext* cx)
{
    MacroAssembler masm(cx);
    GenerateBailoutThunk(cx, masm, NO_FRAME_SIZE_CLASS_ID);

    Linker linker(masm);
    AutoFlushICache afc("BailoutHandler");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "BailoutHandler");
#endif

    return code;
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

    AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

    static_assert((Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
                  "Wrapper register set should be a superset of Volatile register set.");

    // The context is the first argument; a0 is the first argument register.
    Register cxreg = a0;
    regs.take(cxreg);

    // We're aligned to an exit frame, so link it up.
    masm.enterExitFrame(&f);
    masm.loadJSContext(cxreg);

    // Save the base of the argument set stored on the stack.
    Register argsBase = InvalidReg;
    if (f.explicitArgs) {
        argsBase = t1; // Use temporary register.
        regs.take(argsBase);
        masm.ma_daddu(argsBase, StackPointer, Imm32(ExitFrameLayout::SizeWithFooter()));
    }

    // Reserve space for the outparameter.
    Register outReg = InvalidReg;
    switch (f.outParam) {
      case Type_Value:
        outReg = regs.takeAny();
        masm.reserveStack(sizeof(Value));
        masm.movePtr(StackPointer, outReg);
        break;

      case Type_Handle:
        outReg = regs.takeAny();
        masm.PushEmptyRooted(f.outParamRootType);
        masm.movePtr(StackPointer, outReg);
        break;

      case Type_Bool:
      case Type_Int32:
        outReg = regs.takeAny();
        // Reserve 4-byte space to make stack aligned to 8-byte.
        masm.reserveStack(2 * sizeof(int32_t));
        masm.movePtr(StackPointer, outReg);
        break;

      case Type_Pointer:
        outReg = regs.takeAny();
        masm.reserveStack(sizeof(uintptr_t));
        masm.movePtr(StackPointer, outReg);
        break;

      case Type_Double:
        outReg = regs.takeAny();
        masm.reserveStack(sizeof(double));
        masm.movePtr(StackPointer, outReg);
        break;

      default:
        MOZ_ASSERT(f.outParam == Type_Void);
        break;
    }

    masm.setupUnalignedABICall(regs.getAny());
    masm.passABIArg(cxreg);

    size_t argDisp = 0;

    // Copy any arguments.
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
            MOZ_CRASH("NYI: MIPS64 callVM should not be used with 128bits values.");
            break;
        }
    }

    // Copy the implicit outparam, if any.
    if (InvalidReg != outReg)
        masm.passABIArg(outReg);

    masm.callWithABI(f.wrapped);

    // Test for failure.
    switch (f.failType()) {
      case Type_Object:
        masm.branchTestPtr(Assembler::Zero, v0, v0, masm.failureLabel());
        break;
      case Type_Bool:
        // Called functions return bools, which are 0/false and non-zero/true
        masm.branchIfFalseBool(v0, masm.failureLabel());
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
        masm.loadValue(Address(StackPointer, 0), JSReturnOperand);
        masm.freeStack(sizeof(Value));
        break;

      case Type_Int32:
        masm.load32(Address(StackPointer, 0), ReturnReg);
        masm.freeStack(2 * sizeof(int32_t));
        break;

      case Type_Pointer:
        masm.loadPtr(Address(StackPointer, 0), ReturnReg);
        masm.freeStack(sizeof(uintptr_t));
        break;

      case Type_Bool:
        masm.load8ZeroExtend(Address(StackPointer, 0), ReturnReg);
        masm.freeStack(2 * sizeof(int32_t));
        break;

      case Type_Double:
        if (cx->runtime()->jitSupportsFloatingPoint) {
            masm.as_ld(ReturnDoubleReg, StackPointer, 0);
        } else {
            masm.assumeUnreachable("Unable to load into float reg, with no FP support.");
        }
        masm.freeStack(sizeof(double));
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
    AutoFlushICache afc("VMWrapper");
    JitCode* wrapper = linker.newCode<NoGC>(cx, OTHER_CODE);
    if (!wrapper)
        return nullptr;

    // linker.newCode may trigger a GC and sweep functionWrappers_ so we have
    // to use relookupOrAdd instead of add.
    if (!functionWrappers_->relookupOrAdd(p, &f, wrapper))
        return nullptr;

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(wrapper, "VMWrapper");
#endif

    return wrapper;
}

JitCode*
JitRuntime::generatePreBarrier(JSContext* cx, MIRType type)
{
    MacroAssembler masm(cx);

    LiveRegisterSet save;
    if (cx->runtime()->jitSupportsFloatingPoint) {
        save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                           FloatRegisterSet(FloatRegisters::VolatileMask));
    } else {
        save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                           FloatRegisterSet());
    }
    masm.PushRegsInMask(save);

    MOZ_ASSERT(PreBarrierReg == a1);
    masm.movePtr(ImmPtr(cx->runtime()), a0);

    masm.setupUnalignedABICall(a2);
    masm.passABIArg(a0);
    masm.passABIArg(a1);
    masm.callWithABI(IonMarkFunction(type));

    masm.PopRegsInMask(save);
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("PreBarrier");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "PreBarrier");
#endif

    return code;
}

typedef bool (*HandleDebugTrapFn)(JSContext*, BaselineFrame*, uint8_t*, bool*);
static const VMFunction HandleDebugTrapInfo = FunctionInfo<HandleDebugTrapFn>(HandleDebugTrap);

JitCode*
JitRuntime::generateDebugTrapHandler(JSContext* cx)
{
    MacroAssembler masm(cx);

    Register scratch1 = t0;
    Register scratch2 = t1;

    // Load BaselineFrame pointer in scratch1.
    masm.movePtr(s5, scratch1);
    masm.subPtr(Imm32(BaselineFrame::Size()), scratch1);

    // Enter a stub frame and call the HandleDebugTrap VM function. Ensure
    // the stub frame has a nullptr ICStub pointer, since this pointer is
    // marked during GC.
    masm.movePtr(ImmPtr(nullptr), ICStubReg);
    EmitBaselineEnterStubFrame(masm, scratch2);

    JitCode* code = cx->runtime()->jitRuntime()->getVMWrapper(HandleDebugTrapInfo);
    if (!code)
        return nullptr;

    masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    masm.storePtr(ra, Address(StackPointer, sizeof(uintptr_t)));
    masm.storePtr(scratch1, Address(StackPointer, 0));

    EmitBaselineCallVM(code, masm);

    EmitBaselineLeaveStubFrame(masm);

    // If the stub returns |true|, we have to perform a forced return
    // (return from the JS frame). If the stub returns |false|, just return
    // from the trap stub so that execution continues at the current pc.
    Label forcedReturn;
    masm.branchTest32(Assembler::NonZero, ReturnReg, ReturnReg, &forcedReturn);

    // ra was restored by EmitLeaveStubFrame
    masm.branch(ra);

    masm.bind(&forcedReturn);
    masm.loadValue(Address(s5, BaselineFrame::reverseOffsetOfReturnValue()),
                   JSReturnOperand);
    masm.movePtr(s5, StackPointer);
    masm.pop(s5);

    // Before returning, if profiling is turned on, make sure that lastProfilingFrame
    // is set to the correct caller frame.
    {
        Label skipProfilingInstrumentation;
        AbsoluteAddress addressOfEnabled(cx->runtime()->spsProfiler.addressOfEnabled());
        masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skipProfilingInstrumentation);
        masm.profilerExitFrame();
        masm.bind(&skipProfilingInstrumentation);
    }

    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("DebugTrapHandler");
    JitCode* codeDbg = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(codeDbg, "DebugTrapHandler");
#endif

    return codeDbg;
}


JitCode*
JitRuntime::generateExceptionTailStub(JSContext* cx, void* handler)
{
    MacroAssembler masm;

    masm.handleFailureWithHandlerTail(handler);

    Linker linker(masm);
    AutoFlushICache afc("ExceptionTailStub");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "ExceptionTailStub");
#endif

    return code;
}

JitCode*
JitRuntime::generateBailoutTailStub(JSContext* cx)
{
    MacroAssembler masm;

    masm.generateBailoutTail(a1, a2);

    Linker linker(masm);
    AutoFlushICache afc("BailoutTailStub");
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

    Register scratch1 = t0;
    Register scratch2 = t1;
    Register scratch3 = t2;
    Register scratch4 = t3;

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
    masm.ma_and(scratch2, scratch1, Imm32((1 << FRAMETYPE_BITS) - 1));
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
        masm.loadPtr(Address(StackPointer, JitFrameLayout::offsetOfReturnAddress()), scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        // Store return frame in lastProfilingFrame.
        // scratch2 := StackPointer + Descriptor.size*1 + JitFrameLayout::Size();
        masm.as_daddu(scratch2, StackPointer, scratch1);
        masm.ma_daddu(scratch2, scratch2, Imm32(JitFrameLayout::Size()));
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
        masm.as_daddu(scratch3, StackPointer, scratch1);
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
        masm.as_daddu(scratch2, StackPointer, scratch1);
        masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2);
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfDescriptor()), scratch3);
        masm.ma_dsrl(scratch1, scratch3, Imm32(FRAMESIZE_SHIFT));
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
        masm.as_daddu(scratch3, scratch2, scratch1);
        masm.addPtr(Imm32(RectifierFrameLayout::Size()), scratch3);
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
        masm.as_daddu(scratch3, scratch2, scratch1);
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
        masm.as_daddu(scratch2, StackPointer, scratch1);
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
        masm.as_daddu(scratch1, scratch2, scratch3);
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
