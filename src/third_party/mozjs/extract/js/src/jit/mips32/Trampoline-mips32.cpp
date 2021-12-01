/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/mips-shared/SharedICHelpers-mips-shared.h"
#include "jit/mips32/Bailouts-mips32.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/VMFunctions.h"
#include "vm/JSCompartment.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"

using namespace js;
using namespace js::jit;

static_assert(sizeof(uintptr_t) == sizeof(uint32_t), "Not 64-bit clean.");

struct EnterJITRegs
{
    double f30;
    double f28;
    double f26;
    double f24;
    double f22;
    double f20;

    // non-volatile registers.
    uintptr_t ra;
    uintptr_t fp;
    uintptr_t s7;
    uintptr_t s6;
    uintptr_t s5;
    uintptr_t s4;
    uintptr_t s3;
    uintptr_t s2;
    uintptr_t s1;
    uintptr_t s0;
};

struct EnterJITArgs
{
    // First 4 argumet placeholders
    void* jitcode; // <- sp points here when function is entered.
    int maxArgc;
    Value* maxArgv;
    InterpreterFrame* fp;

    // Arguments on stack
    CalleeToken calleeToken;
    JSObject* scopeChain;
    size_t numStackValues;
    Value* vp;
};

static void
GenerateReturn(MacroAssembler& masm, int returnCode)
{
    MOZ_ASSERT(masm.framePushed() == sizeof(EnterJITRegs));

    // Restore non-volatile registers
    masm.as_lw(s0, StackPointer, offsetof(EnterJITRegs, s0));
    masm.as_lw(s1, StackPointer, offsetof(EnterJITRegs, s1));
    masm.as_lw(s2, StackPointer, offsetof(EnterJITRegs, s2));
    masm.as_lw(s3, StackPointer, offsetof(EnterJITRegs, s3));
    masm.as_lw(s4, StackPointer, offsetof(EnterJITRegs, s4));
    masm.as_lw(s5, StackPointer, offsetof(EnterJITRegs, s5));
    masm.as_lw(s6, StackPointer, offsetof(EnterJITRegs, s6));
    masm.as_lw(s7, StackPointer, offsetof(EnterJITRegs, s7));
    masm.as_lw(fp, StackPointer, offsetof(EnterJITRegs, fp));
    masm.as_lw(ra, StackPointer, offsetof(EnterJITRegs, ra));

    // Restore non-volatile floating point registers
    masm.as_ldc1(f20, StackPointer, offsetof(EnterJITRegs, f20));
    masm.as_ldc1(f22, StackPointer, offsetof(EnterJITRegs, f22));
    masm.as_ldc1(f24, StackPointer, offsetof(EnterJITRegs, f24));
    masm.as_ldc1(f26, StackPointer, offsetof(EnterJITRegs, f26));
    masm.as_ldc1(f28, StackPointer, offsetof(EnterJITRegs, f28));
    masm.as_ldc1(f30, StackPointer, offsetof(EnterJITRegs, f30));

    masm.freeStack(sizeof(EnterJITRegs));

    masm.branch(ra);
}

static void
GeneratePrologue(MacroAssembler& masm)
{
    // Save non-volatile registers. These must be saved by the trampoline,
    // rather than the JIT'd code, because they are scanned by the conservative
    // scanner.
    masm.reserveStack(sizeof(EnterJITRegs));
    masm.as_sw(s0, StackPointer, offsetof(EnterJITRegs, s0));
    masm.as_sw(s1, StackPointer, offsetof(EnterJITRegs, s1));
    masm.as_sw(s2, StackPointer, offsetof(EnterJITRegs, s2));
    masm.as_sw(s3, StackPointer, offsetof(EnterJITRegs, s3));
    masm.as_sw(s4, StackPointer, offsetof(EnterJITRegs, s4));
    masm.as_sw(s5, StackPointer, offsetof(EnterJITRegs, s5));
    masm.as_sw(s6, StackPointer, offsetof(EnterJITRegs, s6));
    masm.as_sw(s7, StackPointer, offsetof(EnterJITRegs, s7));
    masm.as_sw(fp, StackPointer, offsetof(EnterJITRegs, fp));
    masm.as_sw(ra, StackPointer, offsetof(EnterJITRegs, ra));

    masm.as_sdc1(f20, StackPointer, offsetof(EnterJITRegs, f20));
    masm.as_sdc1(f22, StackPointer, offsetof(EnterJITRegs, f22));
    masm.as_sdc1(f24, StackPointer, offsetof(EnterJITRegs, f24));
    masm.as_sdc1(f26, StackPointer, offsetof(EnterJITRegs, f26));
    masm.as_sdc1(f28, StackPointer, offsetof(EnterJITRegs, f28));
    masm.as_sdc1(f30, StackPointer, offsetof(EnterJITRegs, f30));
}


/*
 * This method generates a trampoline for a c++ function with the following
 * signature:
 *   void enter(void* code, int argc, Value* argv, InterpreterFrame* fp,
 *              CalleeToken calleeToken, JSObject* scopeChain, Value* vp)
 *   ...using standard EABI calling convention
 */
void
JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm)
{
    enterJITOffset_ = startTrampolineCode(masm);

    const Register reg_code = a0;
    const Register reg_argc = a1;
    const Register reg_argv = a2;
    const mozilla::DebugOnly<Register> reg_frame = a3;

    MOZ_ASSERT(OsrFrameReg == reg_frame);

    GeneratePrologue(masm);

    const Address slotToken(sp, sizeof(EnterJITRegs) + offsetof(EnterJITArgs, calleeToken));
    const Address slotVp(sp, sizeof(EnterJITRegs) + offsetof(EnterJITArgs, vp));

    // Save stack pointer into s4
    masm.movePtr(StackPointer, s4);

    // Load calleeToken into s2.
    masm.loadPtr(slotToken, s2);

    // Save stack pointer as baseline frame.
    masm.movePtr(StackPointer, BaselineFrameReg);

    // Load the number of actual arguments into s3.
    masm.loadPtr(slotVp, s3);
    masm.unboxInt32(Address(s3, 0), s3);

    /***************************************************************
    Loop over argv vector, push arguments onto stack in reverse order
    ***************************************************************/

    // if we are constructing, that also needs to include newTarget
    {
        Label noNewTarget;
        masm.branchTest32(Assembler::Zero, s2, Imm32(CalleeToken_FunctionConstructing),
                          &noNewTarget);

        masm.add32(Imm32(1), reg_argc);

        masm.bind(&noNewTarget);
    }

    masm.as_sll(s0, reg_argc, 3); // s0 = argc * 8
    masm.addPtr(reg_argv, s0); // s0 = argv + argc * 8

    // Loop over arguments, copying them from an unknown buffer onto the Ion
    // stack so they can be accessed from JIT'ed code.
    Label header, footer;
    // If there aren't any arguments, don't do anything
    masm.ma_b(s0, reg_argv, &footer, Assembler::BelowOrEqual, ShortJump);
    {
        masm.bind(&header);

        masm.subPtr(Imm32(2 * sizeof(uintptr_t)), s0);
        masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);

        ValueOperand value = ValueOperand(s6, s7);
        masm.loadValue(Address(s0, 0), value);
        masm.storeValue(value, Address(StackPointer, 0));

        masm.ma_b(s0, reg_argv, &header, Assembler::Above, ShortJump);
    }
    masm.bind(&footer);

    masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    masm.storePtr(s3, Address(StackPointer, sizeof(uintptr_t))); // actual arguments
    masm.storePtr(s2, Address(StackPointer, 0)); // callee token

    masm.subPtr(StackPointer, s4);
    masm.makeFrameDescriptor(s4, JitFrame_CppToJSJit, JitFrameLayout::Size());
    masm.push(s4); // descriptor

    CodeLabel returnLabel;
    CodeLabel oomReturnLabel;
    {
        // Handle Interpreter -> Baseline OSR.
        AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
        regs.take(OsrFrameReg);
        regs.take(BaselineFrameReg);
        regs.take(reg_code);
        regs.take(ReturnReg);

        const Address slotNumStackValues(BaselineFrameReg, sizeof(EnterJITRegs) +
                                         offsetof(EnterJITArgs, numStackValues));
        const Address slotScopeChain(BaselineFrameReg, sizeof(EnterJITRegs) +
                                     offsetof(EnterJITArgs, scopeChain));

        Label notOsr;
        masm.ma_b(OsrFrameReg, OsrFrameReg, &notOsr, Assembler::Zero, ShortJump);

        Register scratch = regs.takeAny();

        Register numStackValues = regs.takeAny();
        masm.load32(slotNumStackValues, numStackValues);

        // Push return address.
        masm.subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
        masm.ma_li(scratch, &returnLabel);
        masm.storePtr(scratch, Address(StackPointer, 0));

        // Push previous frame pointer.
        masm.subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
        masm.storePtr(BaselineFrameReg, Address(StackPointer, 0));

        // Reserve frame.
        Register framePtr = BaselineFrameReg;
        masm.subPtr(Imm32(BaselineFrame::Size()), StackPointer);
        masm.movePtr(StackPointer, framePtr);

        // Reserve space for locals and stack values.
        masm.ma_sll(scratch, numStackValues, Imm32(3));
        masm.subPtr(scratch, StackPointer);

        // Enter exit frame.
        masm.addPtr(Imm32(BaselineFrame::Size() + BaselineFrame::FramePointerOffset), scratch);
        masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS, ExitFrameLayout::Size());

        // Push frame descriptor and fake return address.
        masm.reserveStack(2 * sizeof(uintptr_t));
        masm.storePtr(scratch, Address(StackPointer, sizeof(uintptr_t))); // Frame descriptor
        masm.storePtr(zero, Address(StackPointer, 0)); // fake return address

        // No GC things to mark, push a bare token.
        masm.loadJSContext(scratch);
        masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

        masm.reserveStack(2 * sizeof(uintptr_t));
        masm.storePtr(framePtr, Address(StackPointer, sizeof(uintptr_t))); // BaselineFrame
        masm.storePtr(reg_code, Address(StackPointer, 0)); // jitcode

        masm.setupUnalignedABICall(scratch);
        masm.passABIArg(BaselineFrameReg); // BaselineFrame
        masm.passABIArg(OsrFrameReg); // InterpreterFrame
        masm.passABIArg(numStackValues);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, jit::InitBaselineFrameForOsr), MoveOp::GENERAL,
                         CheckUnsafeCallWithABI::DontCheckHasExitFrame);

        regs.add(OsrFrameReg);
        regs.take(JSReturnOperand);
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
            AbsoluteAddress addressOfEnabled(cx->runtime()->geckoProfiler().addressOfEnabled());
            masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                          &skipProfilingInstrumentation);
            masm.ma_addu(realFramePtr, framePtr, Imm32(sizeof(void*)));
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
        masm.ma_li(scratch, &oomReturnLabel);
        masm.jump(scratch);

        masm.bind(&notOsr);
        // Load the scope chain in R1.
        MOZ_ASSERT(R1.scratchReg() != reg_code);
        masm.loadPtr(slotScopeChain, R1.scratchReg());
    }

    // The call will push the return address on the stack, thus we check that
    // the stack would be aligned once the call is complete.
    masm.assertStackAlignment(JitStackAlignment, sizeof(uintptr_t));

    // Call the function with pushing return address to stack.
    masm.callJitNoProfiler(reg_code);

    {
        // Interpreter -> Baseline OSR will return here.
        masm.bind(&returnLabel);
        masm.addCodeLabel(returnLabel);
        masm.bind(&oomReturnLabel);
        masm.addCodeLabel(oomReturnLabel);
    }

    // Pop arguments off the stack.
    // s0 <- 8*argc (size of all arguments we pushed on the stack)
    masm.pop(s0);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), s0);
    masm.addPtr(s0, StackPointer);

    // Store the returned value into the slotVp
    masm.loadPtr(slotVp, s1);
    masm.storeValue(JSReturnOperand, Address(s1, 0));

    // Restore non-volatile registers and return.
    GenerateReturn(masm, ShortJump);
}

void
JitRuntime::generateInvalidator(MacroAssembler& masm, Label* bailoutTail)
{
    invalidatorOffset_ = startTrampolineCode(masm);

    // NOTE: Members ionScript_ and osiPointReturnAddress_ of
    // InvalidationBailoutStack are already on the stack.
    static const uint32_t STACK_DATA_SIZE = sizeof(InvalidationBailoutStack) -
                                            2 * sizeof(uintptr_t);

    // Stack has to be alligned here. If not, we will have to fix it.
    masm.checkStackAlignment();

    // Make room for data on stack.
    masm.subPtr(Imm32(STACK_DATA_SIZE), StackPointer);

    // Save general purpose registers
    for (uint32_t i = 0; i < Registers::Total; i++) {
        Address address = Address(StackPointer, InvalidationBailoutStack::offsetOfRegs() +
                                                i * sizeof(uintptr_t));
        masm.storePtr(Register::FromCode(i), address);
    }

    // Save floating point registers
    // We can use as_sd because stack is alligned.
    for (uint32_t i = 0; i < FloatRegisters::TotalDouble; i ++) {
        masm.as_sdc1(FloatRegister::FromIndex(i, FloatRegister::Double), StackPointer,
                     InvalidationBailoutStack::offsetOfFpRegs() + i * sizeof(double));
    }

    // Pass pointer to InvalidationBailoutStack structure.
    masm.movePtr(StackPointer, a0);

    // Reserve place for return value and BailoutInfo pointer
    masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    // Pass pointer to return value.
    masm.ma_addu(a1, StackPointer, Imm32(sizeof(uintptr_t)));
    // Pass pointer to BailoutInfo
    masm.movePtr(StackPointer, a2);

    masm.setupAlignedABICall();
    masm.passABIArg(a0);
    masm.passABIArg(a1);
    masm.passABIArg(a2);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, InvalidationBailout), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckOther);

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
    masm.jump(bailoutTail);
}

void
JitRuntime::generateArgumentsRectifier(MacroAssembler& masm)
{
    argumentsRectifierOffset_ = startTrampolineCode(masm);
    masm.pushReturnAddress();

    Register numActArgsReg = t6;
    Register calleeTokenReg = t7;
    Register numArgsReg = t5;

    // Load the number of actual arguments into numActArgsReg
    masm.loadPtr(Address(StackPointer, RectifierFrameLayout::offsetOfNumActualArgs()),
                 numActArgsReg);

    // Load the number of |undefined|s to push into t1.
    masm.loadPtr(Address(StackPointer, RectifierFrameLayout::offsetOfCalleeToken()),
                 calleeTokenReg);

    // Copy the number of actual arguments into s3.
    masm.mov(numActArgsReg, s3);

    masm.mov(calleeTokenReg, numArgsReg);
    masm.andPtr(Imm32(CalleeTokenMask), numArgsReg);
    masm.load16ZeroExtend(Address(numArgsReg, JSFunction::offsetOfNargs()), numArgsReg);

    masm.as_subu(t1, numArgsReg, s3);

    // Get the topmost argument.
    masm.ma_sll(t0, s3, Imm32(3)); // t0 <- nargs * 8
    masm.as_addu(t2, sp, t0); // t2 <- sp + nargs * 8
    masm.addPtr(Imm32(sizeof(RectifierFrameLayout)), t2);

    {
        Label notConstructing;

        masm.branchTest32(Assembler::Zero, calleeTokenReg, Imm32(CalleeToken_FunctionConstructing),
                          &notConstructing);

        // Add sizeof(Value) to overcome |this|
        masm.subPtr(Imm32(sizeof(Value)), StackPointer);
        masm.load32(Address(t2, NUNBOX32_TYPE_OFFSET + sizeof(Value)), t0);
        masm.store32(t0, Address(StackPointer, NUNBOX32_TYPE_OFFSET));
        masm.load32(Address(t2, NUNBOX32_PAYLOAD_OFFSET + sizeof(Value)), t0);
        masm.store32(t0, Address(StackPointer, NUNBOX32_PAYLOAD_OFFSET));

        // Include the newly pushed newTarget value in the frame size
        // calculated below.
        masm.add32(Imm32(1), numArgsReg);

        masm.bind(&notConstructing);
    }

    // Push undefined.
    masm.moveValue(UndefinedValue(), ValueOperand(t3, t4));
    {
        Label undefLoopTop;
        masm.bind(&undefLoopTop);

        masm.subPtr(Imm32(sizeof(Value)), StackPointer);
        masm.storeValue(ValueOperand(t3, t4), Address(StackPointer, 0));
        masm.sub32(Imm32(1), t1);

        masm.ma_b(t1, t1, &undefLoopTop, Assembler::NonZero, ShortJump);
    }

    // Push arguments, |nargs| + 1 times (to include |this|).
    {
        Label copyLoopTop, initialSkip;

        masm.ma_b(&initialSkip, ShortJump);

        masm.bind(&copyLoopTop);
        masm.subPtr(Imm32(sizeof(Value)), t2);
        masm.sub32(Imm32(1), s3);

        masm.bind(&initialSkip);

        MOZ_ASSERT(sizeof(Value) == 2 * sizeof(uint32_t));
        // Read argument and push to stack.
        masm.subPtr(Imm32(sizeof(Value)), StackPointer);
        masm.load32(Address(t2, NUNBOX32_TYPE_OFFSET), t0);
        masm.store32(t0, Address(StackPointer, NUNBOX32_TYPE_OFFSET));
        masm.load32(Address(t2, NUNBOX32_PAYLOAD_OFFSET), t0);
        masm.store32(t0, Address(StackPointer, NUNBOX32_PAYLOAD_OFFSET));

        masm.ma_b(s3, s3, &copyLoopTop, Assembler::NonZero, ShortJump);
    }

    // translate the framesize from values into bytes
    masm.ma_addu(t0, numArgsReg, Imm32(1));
    masm.lshiftPtr(Imm32(3), t0);

    // Construct sizeDescriptor.
    masm.makeFrameDescriptor(t0, JitFrame_Rectifier, JitFrameLayout::Size());

    // Construct JitFrameLayout.
    masm.subPtr(Imm32(3 * sizeof(uintptr_t)), StackPointer);
    // Push actual arguments.
    masm.storePtr(numActArgsReg, Address(StackPointer, 2 * sizeof(uintptr_t)));
    // Push callee token.
    masm.storePtr(calleeTokenReg, Address(StackPointer, sizeof(uintptr_t)));
    // Push frame descriptor.
    masm.storePtr(t0, Address(StackPointer, 0));

    // Call the target function.
    masm.andPtr(Imm32(CalleeTokenMask), calleeTokenReg);
    masm.loadJitCodeRaw(calleeTokenReg, t1);
    argumentsRectifierReturnOffset_ = masm.callJitNoProfiler(t1);

    // arg1
    //  ...
    // argN
    // num actual args
    // callee token
    // sizeDescriptor     <- sp now
    // return address

    // Remove the rectifier frame.
    // t0 <- descriptor with FrameType.
    masm.loadPtr(Address(StackPointer, 0), t0);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), t0); // t0 <- descriptor.

    // Discard descriptor, calleeToken and number of actual arguments.
    masm.addPtr(Imm32(3 * sizeof(uintptr_t)), StackPointer);

    // arg1
    //  ...
    // argN               <- sp now; t0 <- frame descriptor
    // num actual args
    // callee token
    // sizeDescriptor
    // return address

    // Discard pushed arguments.
    masm.addPtr(t0, StackPointer);

    masm.ret();
}

// NOTE: Members snapshotOffset_ and padding_ of BailoutStack
// are not stored in PushBailoutFrame().
static const uint32_t bailoutDataSize = sizeof(BailoutStack) - 2 * sizeof(uintptr_t);
static const uint32_t bailoutInfoOutParamSize = 2 * sizeof(uintptr_t);

/* There are two different stack layouts when doing bailout. They are
 * represented via class BailoutStack.
 *
 * - First case is when bailout is done trough bailout table. In this case
 * table offset is stored in $ra (look at JitRuntime::generateBailoutTable())
 * and thunk code should save it on stack. In this case frameClassId_ cannot
 * be NO_FRAME_SIZE_CLASS_ID. Members snapshotOffset_ and padding_ are not on
 * the stack.
 *
 * - Other case is when bailout is done via out of line code (lazy bailout).
 * In this case frame size is stored in $ra (look at
 * CodeGeneratorMIPS::generateOutOfLineCode()) and thunk code should save it
 * on stack. Other difference is that members snapshotOffset_ and padding_ are
 * pushed to the stack by CodeGeneratorMIPS::visitOutOfLineBailout(). Field
 * frameClassId_ is forced to be NO_FRAME_SIZE_CLASS_ID
 * (See: JitRuntime::generateBailoutHandler).
 */
static void
PushBailoutFrame(MacroAssembler& masm, uint32_t frameClass, Register spArg)
{
    // Make sure that alignment is proper.
    masm.checkStackAlignment();

    // Make room for data.
    masm.subPtr(Imm32(bailoutDataSize), StackPointer);

    // Save general purpose registers.
    for (uint32_t i = 0; i < Registers::Total; i++) {
        uint32_t off = BailoutStack::offsetOfRegs() + i * sizeof(uintptr_t);
        masm.storePtr(Register::FromCode(i), Address(StackPointer, off));
    }

    // Save floating point registers
    // We can use as_sdc1 because stack is alligned.
    for (uint32_t i = 0; i < FloatRegisters::TotalDouble; i++) {
        masm.as_sdc1(FloatRegister::FromIndex(i, FloatRegister::Double), StackPointer,
                     BailoutStack::offsetOfFpRegs() + i * sizeof(double));
    }

    // Store the frameSize_ or tableOffset_ stored in ra
    // See: JitRuntime::generateBailoutTable()
    // See: CodeGeneratorMIPS::generateOutOfLineCode()
    masm.storePtr(ra, Address(StackPointer, BailoutStack::offsetOfFrameSize()));

    // Put frame class to stack
    masm.storePtr(ImmWord(frameClass), Address(StackPointer, BailoutStack::offsetOfFrameClass()));

    // Put pointer to BailoutStack as first argument to the Bailout()
    masm.movePtr(StackPointer, spArg);
}

static void
GenerateBailoutThunk(MacroAssembler& masm, uint32_t frameClass, Label* bailoutTail)
{
    PushBailoutFrame(masm, frameClass, a0);

    // Put pointer to BailoutInfo
    masm.subPtr(Imm32(bailoutInfoOutParamSize), StackPointer);
    masm.storePtr(ImmPtr(nullptr), Address(StackPointer, 0));
    masm.movePtr(StackPointer, a1);

    masm.setupAlignedABICall();
    masm.passABIArg(a0);
    masm.passABIArg(a1);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, Bailout), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckOther);

    // Get BailoutInfo pointer
    masm.loadPtr(Address(StackPointer, 0), a2);

    // Remove both the bailout frame and the topmost Ion frame's stack.
    if (frameClass == NO_FRAME_SIZE_CLASS_ID) {
        // Load frameSize from stack
        masm.loadPtr(Address(StackPointer,
                             bailoutInfoOutParamSize + BailoutStack::offsetOfFrameSize()), a1);

        // Remove complete BailoutStack class and data after it
        masm.addPtr(Imm32(sizeof(BailoutStack) + bailoutInfoOutParamSize), StackPointer);
        // Remove frame size srom stack
        masm.addPtr(a1, StackPointer);
    } else {
        uint32_t frameSize = FrameSizeClass::FromClass(frameClass).frameSize();
        // Remove the data this fuction added and frame size.
        masm.addPtr(Imm32(bailoutDataSize + bailoutInfoOutParamSize + frameSize), StackPointer);
    }

    // Jump to shared bailout tail. The BailoutInfo pointer has to be in a2.
    masm.jump(bailoutTail);
}

JitRuntime::BailoutTable
JitRuntime::generateBailoutTable(MacroAssembler& masm, Label* bailoutTail, uint32_t frameClass)
{
    uint32_t offset = startTrampolineCode(masm);

    Label bailout;
    for (size_t i = 0; i < BAILOUT_TABLE_SIZE; i++) {
        // Calculate offset to the end of table
        int32_t offset = (BAILOUT_TABLE_SIZE - i) * BAILOUT_TABLE_ENTRY_SIZE;

        // We use the 'ra' as table offset later in GenerateBailoutThunk
        masm.as_bal(BOffImm16(offset));
        masm.nop();
    }
    masm.bind(&bailout);

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
                  "Wrapper register set should be a superset of Volatile register set.");

    // The context is the first argument; a0 is the first argument register.
    Register cxreg = a0;
    regs.take(cxreg);

    // If it isn't a tail call, then the return address needs to be saved
    if (f.expectTailCall == NonTailCall)
        masm.pushReturnAddress();

    // We're aligned to an exit frame, so link it up.
    masm.loadJSContext(cxreg);
    masm.enterExitFrame(cxreg, regs.getAny(), &f);

    // Save the base of the argument set stored on the stack.
    Register argsBase = InvalidReg;
    if (f.explicitArgs) {
        argsBase = t1; // Use temporary register.
        regs.take(argsBase);
        masm.ma_addu(argsBase, StackPointer, Imm32(ExitFrameLayout::SizeWithFooter()));
    }
    uint32_t framePushedBeforeAlignStack = masm.framePushed();
    masm.alignStackPointer();
    masm.setFramePushed(0);

    // Reserve space for the outparameter. Reserve sizeof(Value) for every
    // case so that stack stays aligned.
    uint32_t outParamSize = 0;
    switch (f.outParam) {
      case Type_Value:
        outParamSize = sizeof(Value);
        masm.reserveStack(outParamSize);
        break;

      case Type_Handle:
        {
            uint32_t pushed = masm.framePushed();
            masm.PushEmptyRooted(f.outParamRootType);
            outParamSize = masm.framePushed() - pushed;
        }
        break;

      case Type_Bool:
      case Type_Int32:
        MOZ_ASSERT(sizeof(uintptr_t) == sizeof(uint32_t));
        MOZ_FALLTHROUGH;
      case Type_Pointer:
        outParamSize = sizeof(uintptr_t);
        masm.reserveStack(outParamSize);
        break;

      case Type_Double:
        outParamSize = sizeof(double);
        masm.reserveStack(outParamSize);
        break;
      default:
        MOZ_ASSERT(f.outParam == Type_Void);
        break;
    }

    uint32_t outParamOffset = 0;
    if (f.outParam != Type_Void) {
        // Make sure that stack is double aligned after outParam.
        MOZ_ASSERT(outParamSize <= sizeof(double));
        outParamOffset += sizeof(double) - outParamSize;
    }
    // Reserve stack for double sized args that are copied to be aligned.
    outParamOffset += f.doubleByRefArgs() * sizeof(double);

    Register doubleArgs = t0;
    masm.reserveStack(outParamOffset);
    masm.movePtr(StackPointer, doubleArgs);

    if (!generateTLEnterVM(masm, f))
        return false;

    masm.setupAlignedABICall();
    masm.passABIArg(cxreg);

    size_t argDisp = 0;
    size_t doubleArgDisp = 0;

    // Copy any arguments.
    for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
        MoveOperand from;
        switch (f.argProperties(explicitArg)) {
          case VMFunction::WordByValue:
            masm.passABIArg(MoveOperand(argsBase, argDisp), MoveOp::GENERAL);
            argDisp += sizeof(uint32_t);
            break;
          case VMFunction::DoubleByValue:
            // Values should be passed by reference, not by value, so we
            // assert that the argument is a double-precision float.
            MOZ_ASSERT(f.argPassedInFloatReg(explicitArg));
            masm.passABIArg(MoveOperand(argsBase, argDisp), MoveOp::DOUBLE);
            argDisp += sizeof(double);
            break;
          case VMFunction::WordByRef:
            masm.passABIArg(MoveOperand(argsBase, argDisp, MoveOperand::EFFECTIVE_ADDRESS),
                            MoveOp::GENERAL);
            argDisp += sizeof(uint32_t);
            break;
          case VMFunction::DoubleByRef:
            // Copy double sized argument to aligned place.
            masm.ma_ldc1WordAligned(ScratchDoubleReg, argsBase, argDisp);
            masm.as_sdc1(ScratchDoubleReg, doubleArgs, doubleArgDisp);
            masm.passABIArg(MoveOperand(doubleArgs, doubleArgDisp, MoveOperand::EFFECTIVE_ADDRESS),
                            MoveOp::GENERAL);
            doubleArgDisp += sizeof(double);
            argDisp += sizeof(double);
            break;
        }
    }

    MOZ_ASSERT_IF(f.outParam != Type_Void,
                  doubleArgDisp + sizeof(double) == outParamOffset + outParamSize);

    // Copy the implicit outparam, if any.
    if (f.outParam != Type_Void) {
        masm.passABIArg(MoveOperand(doubleArgs, outParamOffset, MoveOperand::EFFECTIVE_ADDRESS),
                            MoveOp::GENERAL);
    }

    masm.callWithABI(f.wrapped, MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    if (!generateTLExitVM(masm, f))
        return false;

    // Test for failure.
    switch (f.failType()) {
      case Type_Object:
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

    masm.freeStack(outParamOffset);

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
        MOZ_ASSERT(sizeof(uintptr_t) == sizeof(uint32_t));
        MOZ_FALLTHROUGH;
      case Type_Pointer:
        masm.load32(Address(StackPointer, 0), ReturnReg);
        masm.freeStack(sizeof(uintptr_t));
        break;

      case Type_Bool:
        masm.load8ZeroExtend(Address(StackPointer, 0), ReturnReg);
        masm.freeStack(sizeof(uintptr_t));
        break;

      case Type_Double:
        if (cx->runtime()->jitSupportsFloatingPoint) {
            masm.as_ldc1(ReturnDoubleReg, StackPointer, 0);
        } else {
            masm.assumeUnreachable("Unable to load into float reg, with no FP support.");
        }
        masm.freeStack(sizeof(double));
        break;

      default:
        MOZ_ASSERT(f.outParam == Type_Void);
        break;
    }

    masm.restoreStackPointer();
    masm.setFramePushed(framePushedBeforeAlignStack);

    masm.leaveExitFrame();
    masm.retn(Imm32(sizeof(ExitFrameLayout) +
                    f.explicitStackSlots() * sizeof(uintptr_t) +
                    f.extraValuesToPop * sizeof(Value)));

    return functionWrappers_->putNew(&f, wrapperOffset);
}

uint32_t
JitRuntime::generatePreBarrier(JSContext* cx, MacroAssembler& masm, MIRType type)
{
    uint32_t offset = startTrampolineCode(masm);

    MOZ_ASSERT(PreBarrierReg == a1);
    Register temp1 = a0;
    Register temp2 = a2;
    Register temp3 = a3;
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
                           FloatRegisterSet(FloatRegisters::VolatileMask));
    } else {
        save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                           FloatRegisterSet());
    }
    save.add(ra);
    masm.PushRegsInMask(save);

    masm.movePtr(ImmPtr(cx->runtime()), a0);

    masm.setupUnalignedABICall(a2);
    masm.passABIArg(a0);
    masm.passABIArg(a1);
    masm.callWithABI(JitMarkFunction(type));

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

typedef bool (*HandleDebugTrapFn)(JSContext*, BaselineFrame*, uint8_t*, bool*);
static const VMFunction HandleDebugTrapInfo =
    FunctionInfo<HandleDebugTrapFn>(HandleDebugTrap, "HandleDebugTrap");

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

    TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(HandleDebugTrapInfo);

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

    masm.generateBailoutTail(a1, a2);
}

void
JitRuntime::generateProfilerExitFrameTailStub(MacroAssembler& masm, Label* profilerExitTail)
{
    profilerExitFrameTailOffset_ = startTrampolineCode(masm);
    masm.bind(profilerExitTail);

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
    masm.ma_and(scratch2, scratch1, Imm32((1 << FRAMETYPE_BITS) - 1));
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
        masm.as_addu(scratch2, StackPointer, scratch1);
        masm.ma_addu(scratch2, scratch2, Imm32(JitFrameLayout::Size()));
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
        masm.as_addu(scratch3, StackPointer, scratch1);
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
        masm.as_addu(scratch2, StackPointer, scratch1);
        masm.add32(Imm32(JitFrameLayout::Size()), scratch2);
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfDescriptor()), scratch3);
        masm.ma_srl(scratch1, scratch3, Imm32(FRAMESIZE_SHIFT));
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
        masm.as_addu(scratch3, scratch2, scratch1);
        masm.add32(Imm32(RectifierFrameLayout::Size()), scratch3);
        masm.storePtr(scratch3, lastProfilingFrame);
        masm.ret();

        masm.bind(&notIonFrame);

        // Check for either BaselineStub or a CppToJSJit/WasmToJSJit entry
        // frame.
        masm.branch32(Assembler::NotEqual, scratch3, Imm32(JitFrame_BaselineStub), &handle_Entry);

        // Handle Rectifier <- BaselineStub <- BaselineJS
        masm.as_addu(scratch3, scratch2, scratch1);
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
        masm.as_addu(scratch2, StackPointer, scratch1);
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
        masm.as_addu(scratch1, scratch2, scratch3);
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
