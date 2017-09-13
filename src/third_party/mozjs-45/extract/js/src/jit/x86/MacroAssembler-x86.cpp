/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/MacroAssembler-x86.h"

#include "mozilla/Alignment.h"
#include "mozilla/Casting.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveEmitter.h"

#include "jsscriptinlines.h"
#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

// vpunpckldq requires 16-byte boundary for memory operand.
// See convertUInt64ToDouble for the details.
MOZ_ALIGNED_DECL(static const uint64_t, 16) TO_DOUBLE[4] = {
    0x4530000043300000LL,
    0x0LL,
    0x4330000000000000LL,
    0x4530000000000000LL
};

static const double TO_DOUBLE_HIGH_SCALE = 0x100000000;

void
MacroAssemblerX86::convertUInt64ToDouble(Register64 src, Register temp, FloatRegister dest)
{
    // SUBPD needs SSE2, HADDPD needs SSE3.
    if (!HasSSE3()) {
        convertUInt32ToDouble(src.high, dest);
        movePtr(ImmPtr(&TO_DOUBLE_HIGH_SCALE), temp);
        loadDouble(Address(temp, 0), ScratchDoubleReg);
        mulDouble(ScratchDoubleReg, dest);
        convertUInt32ToDouble(src.low, ScratchDoubleReg);
        addDouble(ScratchDoubleReg, dest);
        return;
    }

    // Following operation uses entire 128-bit of dest XMM register.
    // Currently higher 64-bit is free when we have access to lower 64-bit.
    MOZ_ASSERT(dest.size() == 8);
    FloatRegister dest128 = FloatRegister(dest.encoding(), FloatRegisters::Simd128);

    // Assume that src is represented as following:
    //   src      = 0x HHHHHHHH LLLLLLLL

    // Move src to dest (=dest128) and ScratchInt32x4Reg (=scratch):
    //   dest     = 0x 00000000 00000000  00000000 LLLLLLLL
    //   scratch  = 0x 00000000 00000000  00000000 HHHHHHHH
    vmovd(src.low, dest128);
    vmovd(src.high, ScratchSimd128Reg);

    // Unpack and interleave dest and scratch to dest:
    //   dest     = 0x 00000000 00000000  HHHHHHHH LLLLLLLL
    vpunpckldq(ScratchSimd128Reg, dest128, dest128);

    // Unpack and interleave dest and a constant C1 to dest:
    //   C1       = 0x 00000000 00000000  45300000 43300000
    //   dest     = 0x 45300000 HHHHHHHH  43300000 LLLLLLLL
    // here, each 64-bit part of dest represents following double:
    //   HI(dest) = 0x 1.00000HHHHHHHH * 2**84 == 2**84 + 0x HHHHHHHH 00000000
    //   LO(dest) = 0x 1.00000LLLLLLLL * 2**52 == 2**52 + 0x 00000000 LLLLLLLL
    movePtr(ImmPtr(TO_DOUBLE), temp);
    vpunpckldq(Operand(temp, 0), dest128, dest128);

    // Subtract a constant C2 from dest, for each 64-bit part:
    //   C2       = 0x 45300000 00000000  43300000 00000000
    // here, each 64-bit part of C2 represents following double:
    //   HI(C2)   = 0x 1.0000000000000 * 2**84 == 2**84
    //   LO(C2)   = 0x 1.0000000000000 * 2**52 == 2**52
    // after the operation each 64-bit part of dest represents following:
    //   HI(dest) = double(0x HHHHHHHH 00000000)
    //   LO(dest) = double(0x 00000000 LLLLLLLL)
    vsubpd(Operand(temp, sizeof(uint64_t) * 2), dest128, dest128);

    // Add HI(dest) and LO(dest) in double and store it into LO(dest),
    //   LO(dest) = double(0x HHHHHHHH 00000000) + double(0x 00000000 LLLLLLLL)
    //            = double(0x HHHHHHHH LLLLLLLL)
    //            = double(src)
    vhaddpd(dest128, dest128);
}

void
MacroAssemblerX86::loadConstantDouble(double d, FloatRegister dest)
{
    if (maybeInlineDouble(d, dest))
        return;
    Double* dbl = getDouble(d);
    if (!dbl)
        return;
    masm.vmovsd_mr(nullptr, dest.encoding());
    propagateOOM(dbl->uses.append(CodeOffset(masm.size())));
}

void
MacroAssemblerX86::addConstantDouble(double d, FloatRegister dest)
{
    Double* dbl = getDouble(d);
    if (!dbl)
        return;
    masm.vaddsd_mr(nullptr, dest.encoding(), dest.encoding());
    propagateOOM(dbl->uses.append(CodeOffset(masm.size())));
}

void
MacroAssemblerX86::loadConstantFloat32(float f, FloatRegister dest)
{
    if (maybeInlineFloat(f, dest))
        return;
    Float* flt = getFloat(f);
    if (!flt)
        return;
    masm.vmovss_mr(nullptr, dest.encoding());
    propagateOOM(flt->uses.append(CodeOffset(masm.size())));
}

void
MacroAssemblerX86::addConstantFloat32(float f, FloatRegister dest)
{
    Float* flt = getFloat(f);
    if (!flt)
        return;
    masm.vaddss_mr(nullptr, dest.encoding(), dest.encoding());
    propagateOOM(flt->uses.append(CodeOffset(masm.size())));
}

void
MacroAssemblerX86::loadConstantInt32x4(const SimdConstant& v, FloatRegister dest)
{
    MOZ_ASSERT(v.type() == SimdConstant::Int32x4);
    if (maybeInlineInt32x4(v, dest))
        return;
    SimdData* i4 = getSimdData(v);
    if (!i4)
        return;
    MOZ_ASSERT(i4->type() == SimdConstant::Int32x4);
    masm.vmovdqa_mr(nullptr, dest.encoding());
    propagateOOM(i4->uses.append(CodeOffset(masm.size())));
}

void
MacroAssemblerX86::loadConstantFloat32x4(const SimdConstant& v, FloatRegister dest)
{
    MOZ_ASSERT(v.type() == SimdConstant::Float32x4);
    if (maybeInlineFloat32x4(v, dest))
        return;
    SimdData* f4 = getSimdData(v);
    if (!f4)
        return;
    MOZ_ASSERT(f4->type() == SimdConstant::Float32x4);
    masm.vmovaps_mr(nullptr, dest.encoding());
    propagateOOM(f4->uses.append(CodeOffset(masm.size())));
}

void
MacroAssemblerX86::finish()
{
    if (!doubles_.empty())
        masm.haltingAlign(sizeof(double));
    for (const Double& d : doubles_) {
        CodeOffset cst(masm.currentOffset());
        for (CodeOffset use : d.uses)
            addCodeLabel(CodeLabel(use, cst));
        masm.doubleConstant(d.value);
        if (!enoughMemory_)
            return;
    }

    if (!floats_.empty())
        masm.haltingAlign(sizeof(float));
    for (const Float& f : floats_) {
        CodeOffset cst(masm.currentOffset());
        for (CodeOffset use : f.uses)
            addCodeLabel(CodeLabel(use, cst));
        masm.floatConstant(f.value);
        if (!enoughMemory_)
            return;
    }

    // SIMD memory values must be suitably aligned.
    if (!simds_.empty())
        masm.haltingAlign(SimdMemoryAlignment);
    for (const SimdData& v : simds_) {
        CodeOffset cst(masm.currentOffset());
        for (CodeOffset use : v.uses)
            addCodeLabel(CodeLabel(use, cst));
        switch (v.type()) {
          case SimdConstant::Int32x4:   masm.int32x4Constant(v.value.asInt32x4());     break;
          case SimdConstant::Float32x4: masm.float32x4Constant(v.value.asFloat32x4()); break;
          default: MOZ_CRASH("unexpected SimdConstant type");
        }
        if (!enoughMemory_)
            return;
    }
}

void
MacroAssemblerX86::handleFailureWithHandlerTail(void* handler)
{
    // Reserve space for exception information.
    subl(Imm32(sizeof(ResumeFromException)), esp);
    movl(esp, eax);

    // Call the handler.
    asMasm().setupUnalignedABICall(ecx);
    asMasm().passABIArg(eax);
    asMasm().callWithABI(handler);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;

    loadPtr(Address(esp, offsetof(ResumeFromException, kind)), eax);
    branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_ENTRY_FRAME), &entryFrame);
    branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
    branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);

    breakpoint(); // Invalid kind.

    // No exception handler. Load the error value, load the new stack pointer
    // and return from the entry frame.
    bind(&entryFrame);
    moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
    ret();

    // If we found a catch handler, this must be a baseline frame. Restore state
    // and jump to the catch block.
    bind(&catch_);
    loadPtr(Address(esp, offsetof(ResumeFromException, target)), eax);
    loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
    loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
    jmp(Operand(eax));

    // If we found a finally block, this must be a baseline frame. Push
    // two values expected by JSOP_RETSUB: BooleanValue(true) and the
    // exception.
    bind(&finally);
    ValueOperand exception = ValueOperand(ecx, edx);
    loadValue(Address(esp, offsetof(ResumeFromException, exception)), exception);

    loadPtr(Address(esp, offsetof(ResumeFromException, target)), eax);
    loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
    loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);

    pushValue(BooleanValue(true));
    pushValue(exception);
    jmp(Operand(eax));

    // Only used in debug mode. Return BaselineFrame->returnValue() to the caller.
    bind(&return_);
    loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
    loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
    loadValue(Address(ebp, BaselineFrame::reverseOffsetOfReturnValue()), JSReturnOperand);
    movl(ebp, esp);
    pop(ebp);

    // If profiling is enabled, then update the lastProfilingFrame to refer to caller
    // frame before returning.
    {
        Label skipProfilingInstrumentation;
        // Test if profiler enabled.
        AbsoluteAddress addressOfEnabled(GetJitContext()->runtime->spsProfiler().addressOfEnabled());
        branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skipProfilingInstrumentation);
        profilerExitFrame();
        bind(&skipProfilingInstrumentation);
    }

    ret();

    // If we are bailing out to baseline to handle an exception, jump to
    // the bailout tail stub.
    bind(&bailout);
    loadPtr(Address(esp, offsetof(ResumeFromException, bailoutInfo)), ecx);
    movl(Imm32(BAILOUT_RETURN_OK), eax);
    jmp(Operand(esp, offsetof(ResumeFromException, target)));
}

void
MacroAssemblerX86::branchTestValue(Condition cond, const ValueOperand& value, const Value& v, Label* label)
{
    jsval_layout jv = JSVAL_TO_IMPL(v);
    if (v.isMarkable())
        cmpPtr(value.payloadReg(), ImmGCPtr(reinterpret_cast<gc::Cell*>(v.toGCThing())));
    else
        cmpPtr(value.payloadReg(), ImmWord(jv.s.payload.i32));

    if (cond == Equal) {
        Label done;
        j(NotEqual, &done);
        {
            cmp32(value.typeReg(), Imm32(jv.s.tag));
            j(Equal, label);
        }
        bind(&done);
    } else {
        MOZ_ASSERT(cond == NotEqual);
        j(NotEqual, label);

        cmp32(value.typeReg(), Imm32(jv.s.tag));
        j(NotEqual, label);
    }
}

template <typename T>
void
MacroAssemblerX86::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest,
                                     MIRType slotType)
{
    if (valueType == MIRType_Double) {
        storeDouble(value.reg().typedReg().fpu(), dest);
        return;
    }

    // Store the type tag if needed.
    if (valueType != slotType)
        storeTypeTag(ImmType(ValueTypeFromMIRType(valueType)), Operand(dest));

    // Store the payload.
    if (value.constant())
        storePayload(value.value(), Operand(dest));
    else
        storePayload(value.reg().typedReg().gpr(), Operand(dest));
}

template void
MacroAssemblerX86::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const Address& dest,
                                     MIRType slotType);

template void
MacroAssemblerX86::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const BaseIndex& dest,
                                     MIRType slotType);

void
MacroAssemblerX86::branchPtrInNurseryRange(Condition cond, Register ptr, Register temp,
                                           Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != temp);
    MOZ_ASSERT(temp != InvalidReg);  // A temp register is required for x86.

    const Nursery& nursery = GetJitContext()->runtime->gcNursery();
    movePtr(ImmWord(-ptrdiff_t(nursery.start())), temp);
    addPtr(ptr, temp);
    branchPtr(cond == Assembler::Equal ? Assembler::Below : Assembler::AboveOrEqual,
              temp, Imm32(nursery.nurserySize()), label);
}

void
MacroAssemblerX86::branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp,
                                              Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    Label done;

    branchTestObject(Assembler::NotEqual, value, cond == Assembler::Equal ? &done : label);
    branchPtrInNurseryRange(cond, value.payloadReg(), temp, label);

    bind(&done);
}

void
MacroAssemblerX86::profilerEnterFrame(Register framePtr, Register scratch)
{
    AbsoluteAddress activation(GetJitContext()->runtime->addressOfProfilingActivation());
    loadPtr(activation, scratch);
    storePtr(framePtr, Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
    storePtr(ImmPtr(nullptr), Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void
MacroAssemblerX86::profilerExitFrame()
{
    jmp(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}

MacroAssembler&
MacroAssemblerX86::asMasm()
{
    return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler&
MacroAssemblerX86::asMasm() const
{
    return *static_cast<const MacroAssembler*>(this);
}

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

void
MacroAssembler::reserveStack(uint32_t amount)
{
    if (amount) {
        // On windows, we cannot skip very far down the stack without touching the
        // memory pages in-between.  This is a corner-case code for situations where the
        // Ion frame data for a piece of code is very large.  To handle this special case,
        // for frames over 1k in size we allocate memory on the stack incrementally, touching
        // it as we go.
        uint32_t amountLeft = amount;
        while (amountLeft > 4096) {
            subl(Imm32(4096), StackPointer);
            store32(Imm32(0), Address(StackPointer, 0));
            amountLeft -= 4096;
        }
        subl(Imm32(amountLeft), StackPointer);
    }
    framePushed_ += amount;
}

// ===============================================================
// ABI function calls.

void
MacroAssembler::setupUnalignedABICall(Register scratch)
{
    setupABICall();
    dynamicAlignment_ = true;

    movl(esp, scratch);
    andl(Imm32(~(ABIStackAlignment - 1)), esp);
    push(scratch);
}

void
MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromAsmJS)
{
    MOZ_ASSERT(inCall_);
    uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

    if (dynamicAlignment_) {
        // sizeof(intptr_t) accounts for the saved stack pointer pushed by
        // setupUnalignedABICall.
        stackForCall += ComputeByteAlignment(stackForCall + sizeof(intptr_t),
                                             ABIStackAlignment);
    } else {
        uint32_t alignmentAtPrologue = callFromAsmJS ? sizeof(AsmJSFrame) : 0;
        stackForCall += ComputeByteAlignment(stackForCall + framePushed() + alignmentAtPrologue,
                                             ABIStackAlignment);
    }

    *stackAdjust = stackForCall;
    reserveStack(stackForCall);

    // Position all arguments.
    {
        enoughMemory_ &= moveResolver_.resolve();
        if (!enoughMemory_)
            return;

        MoveEmitter emitter(*this);
        emitter.emit(moveResolver_);
        emitter.finish();
    }

    assertStackAlignment(ABIStackAlignment);
}

void
MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result)
{
    freeStack(stackAdjust);
    if (result == MoveOp::DOUBLE) {
        reserveStack(sizeof(double));
        fstp(Operand(esp, 0));
        loadDouble(Operand(esp, 0), ReturnDoubleReg);
        freeStack(sizeof(double));
    } else if (result == MoveOp::FLOAT32) {
        reserveStack(sizeof(float));
        fstp32(Operand(esp, 0));
        loadFloat32(Operand(esp, 0), ReturnFloat32Reg);
        freeStack(sizeof(float));
    }
    if (dynamicAlignment_)
        pop(esp);

#ifdef DEBUG
    MOZ_ASSERT(inCall_);
    inCall_ = false;
#endif
}

void
MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result)
{
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(fun);
    callWithABIPost(stackAdjust, result);
}

void
MacroAssembler::callWithABINoProfiler(const Address& fun, MoveOp::Type result)
{
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(fun);
    callWithABIPost(stackAdjust, result);
}

//}}} check_macroassembler_style
