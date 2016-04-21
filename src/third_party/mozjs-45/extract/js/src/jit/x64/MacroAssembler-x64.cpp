/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x64/MacroAssembler-x64.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveEmitter.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

void
MacroAssemblerX64::loadConstantDouble(double d, FloatRegister dest)
{
    if (maybeInlineDouble(d, dest))
        return;
    Double* dbl = getDouble(d);
    if (!dbl)
        return;
    // The constants will be stored in a pool appended to the text (see
    // finish()), so they will always be a fixed distance from the
    // instructions which reference them. This allows the instructions to use
    // PC-relative addressing. Use "jump" label support code, because we need
    // the same PC-relative address patching that jumps use.
    JmpSrc j = masm.vmovsd_ripr(dest.encoding());
    propagateOOM(dbl->uses.append(CodeOffset(j.offset())));
}

void
MacroAssemblerX64::loadConstantFloat32(float f, FloatRegister dest)
{
    if (maybeInlineFloat(f, dest))
        return;
    Float* flt = getFloat(f);
    if (!flt)
        return;
    // See comment in loadConstantDouble
    JmpSrc j = masm.vmovss_ripr(dest.encoding());
    propagateOOM(flt->uses.append(CodeOffset(j.offset())));
}

void
MacroAssemblerX64::loadConstantInt32x4(const SimdConstant& v, FloatRegister dest)
{
    MOZ_ASSERT(v.type() == SimdConstant::Int32x4);
    if (maybeInlineInt32x4(v, dest))
        return;
    SimdData* val = getSimdData(v);
    if (!val)
        return;
    MOZ_ASSERT(val->type() == SimdConstant::Int32x4);
    JmpSrc j = masm.vmovdqa_ripr(dest.encoding());
    propagateOOM(val->uses.append(CodeOffset(j.offset())));
}

void
MacroAssemblerX64::loadConstantFloat32x4(const SimdConstant&v, FloatRegister dest)
{
    MOZ_ASSERT(v.type() == SimdConstant::Float32x4);
    if (maybeInlineFloat32x4(v, dest))
        return;
    SimdData* val = getSimdData(v);
    if (!val)
        return;
    MOZ_ASSERT(val->type() == SimdConstant::Float32x4);
    JmpSrc j = masm.vmovaps_ripr(dest.encoding());
    propagateOOM(val->uses.append(CodeOffset(j.offset())));
}

void
MacroAssemblerX64::bindOffsets(const MacroAssemblerX86Shared::UsesVector& uses)
{
    for (CodeOffset use : uses) {
        JmpDst dst(currentOffset());
        JmpSrc src(use.offset());
        // Using linkJump here is safe, as explaind in the comment in
        // loadConstantDouble.
        masm.linkJump(src, dst);
    }
}

void
MacroAssemblerX64::finish()
{
    if (!doubles_.empty())
        masm.haltingAlign(sizeof(double));
    for (const Double& d : doubles_) {
        bindOffsets(d.uses);
        masm.doubleConstant(d.value);
    }

    if (!floats_.empty())
        masm.haltingAlign(sizeof(float));
    for (const Float& f : floats_) {
        bindOffsets(f.uses);
        masm.floatConstant(f.value);
    }

    // SIMD memory values must be suitably aligned.
    if (!simds_.empty())
        masm.haltingAlign(SimdMemoryAlignment);
    for (const SimdData& v : simds_) {
        bindOffsets(v.uses);
        switch(v.type()) {
          case SimdConstant::Int32x4:   masm.int32x4Constant(v.value.asInt32x4());     break;
          case SimdConstant::Float32x4: masm.float32x4Constant(v.value.asFloat32x4()); break;
          default: MOZ_CRASH("unexpected SimdConstant type");
        }
    }

    MacroAssemblerX86Shared::finish();
}

void
MacroAssemblerX64::branchPrivatePtr(Condition cond, Address lhs, Register ptr, Label* label)
{
    ScratchRegisterScope scratch(asMasm());
    if (ptr != scratch)
        movePtr(ptr, scratch);
    asMasm().rshiftPtr(Imm32(1), scratch);
    branchPtr(cond, lhs, scratch, label);
}

void
MacroAssemblerX64::handleFailureWithHandlerTail(void* handler)
{
    // Reserve space for exception information.
    subq(Imm32(sizeof(ResumeFromException)), rsp);
    movq(rsp, rax);

    // Call the handler.
    asMasm().setupUnalignedABICall(rcx);
    asMasm().passABIArg(rax);
    asMasm().callWithABI(handler);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;

    loadPtr(Address(rsp, offsetof(ResumeFromException, kind)), rax);
    branch32(Assembler::Equal, rax, Imm32(ResumeFromException::RESUME_ENTRY_FRAME), &entryFrame);
    branch32(Assembler::Equal, rax, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    branch32(Assembler::Equal, rax, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    branch32(Assembler::Equal, rax, Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
    branch32(Assembler::Equal, rax, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);

    breakpoint(); // Invalid kind.

    // No exception handler. Load the error value, load the new stack pointer
    // and return from the entry frame.
    bind(&entryFrame);
    moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    loadPtr(Address(rsp, offsetof(ResumeFromException, stackPointer)), rsp);
    ret();

    // If we found a catch handler, this must be a baseline frame. Restore state
    // and jump to the catch block.
    bind(&catch_);
    loadPtr(Address(rsp, offsetof(ResumeFromException, target)), rax);
    loadPtr(Address(rsp, offsetof(ResumeFromException, framePointer)), rbp);
    loadPtr(Address(rsp, offsetof(ResumeFromException, stackPointer)), rsp);
    jmp(Operand(rax));

    // If we found a finally block, this must be a baseline frame. Push
    // two values expected by JSOP_RETSUB: BooleanValue(true) and the
    // exception.
    bind(&finally);
    ValueOperand exception = ValueOperand(rcx);
    loadValue(Address(esp, offsetof(ResumeFromException, exception)), exception);

    loadPtr(Address(rsp, offsetof(ResumeFromException, target)), rax);
    loadPtr(Address(rsp, offsetof(ResumeFromException, framePointer)), rbp);
    loadPtr(Address(rsp, offsetof(ResumeFromException, stackPointer)), rsp);

    pushValue(BooleanValue(true));
    pushValue(exception);
    jmp(Operand(rax));

    // Only used in debug mode. Return BaselineFrame->returnValue() to the caller.
    bind(&return_);
    loadPtr(Address(rsp, offsetof(ResumeFromException, framePointer)), rbp);
    loadPtr(Address(rsp, offsetof(ResumeFromException, stackPointer)), rsp);
    loadValue(Address(rbp, BaselineFrame::reverseOffsetOfReturnValue()), JSReturnOperand);
    movq(rbp, rsp);
    pop(rbp);

    // If profiling is enabled, then update the lastProfilingFrame to refer to caller
    // frame before returning.
    {
        Label skipProfilingInstrumentation;
        AbsoluteAddress addressOfEnabled(GetJitContext()->runtime->spsProfiler().addressOfEnabled());
        branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skipProfilingInstrumentation);
        profilerExitFrame();
        bind(&skipProfilingInstrumentation);
    }

    ret();

    // If we are bailing out to baseline to handle an exception, jump to
    // the bailout tail stub.
    bind(&bailout);
    loadPtr(Address(esp, offsetof(ResumeFromException, bailoutInfo)), r9);
    mov(ImmWord(BAILOUT_RETURN_OK), rax);
    jmp(Operand(rsp, offsetof(ResumeFromException, target)));
}

template <typename T>
void
MacroAssemblerX64::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest,
                                     MIRType slotType)
{
    if (valueType == MIRType_Double) {
        storeDouble(value.reg().typedReg().fpu(), dest);
        return;
    }

    // For known integers and booleans, we can just store the unboxed value if
    // the slot has the same type.
    if ((valueType == MIRType_Int32 || valueType == MIRType_Boolean) && slotType == valueType) {
        if (value.constant()) {
            Value val = value.value();
            if (valueType == MIRType_Int32)
                store32(Imm32(val.toInt32()), dest);
            else
                store32(Imm32(val.toBoolean() ? 1 : 0), dest);
        } else {
            store32(value.reg().typedReg().gpr(), dest);
        }
        return;
    }

    if (value.constant())
        storeValue(value.value(), dest);
    else
        storeValue(ValueTypeFromMIRType(valueType), value.reg().typedReg().gpr(), dest);
}

template void
MacroAssemblerX64::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const Address& dest,
                                     MIRType slotType);

template void
MacroAssemblerX64::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const BaseIndex& dest,
                                     MIRType slotType);

void
MacroAssemblerX64::branchPtrInNurseryRange(Condition cond, Register ptr, Register temp, Label* label)
{
    ScratchRegisterScope scratch(asMasm());

    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != temp);
    MOZ_ASSERT(ptr != scratch);

    const Nursery& nursery = GetJitContext()->runtime->gcNursery();
    movePtr(ImmWord(-ptrdiff_t(nursery.start())), scratch);
    addPtr(ptr, scratch);
    branchPtr(cond == Assembler::Equal ? Assembler::Below : Assembler::AboveOrEqual,
              scratch, Imm32(nursery.nurserySize()), label);
}

void
MacroAssemblerX64::branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp,
                                              Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    const Nursery& nursery = GetJitContext()->runtime->gcNursery();

    // Avoid creating a bogus ObjectValue below.
    if (!nursery.exists())
        return;

    // 'Value' representing the start of the nursery tagged as a JSObject
    Value start = ObjectValue(*reinterpret_cast<JSObject*>(nursery.start()));

    ScratchRegisterScope scratch(asMasm());
    movePtr(ImmWord(-ptrdiff_t(start.asRawBits())), scratch);
    addPtr(value.valueReg(), scratch);
    branchPtr(cond == Assembler::Equal ? Assembler::Below : Assembler::AboveOrEqual,
              scratch, Imm32(nursery.nurserySize()), label);
}

void
MacroAssemblerX64::profilerEnterFrame(Register framePtr, Register scratch)
{
    AbsoluteAddress activation(GetJitContext()->runtime->addressOfProfilingActivation());
    loadPtr(activation, scratch);
    storePtr(framePtr, Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
    storePtr(ImmPtr(nullptr), Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void
MacroAssemblerX64::profilerExitFrame()
{
    jmp(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}

MacroAssembler&
MacroAssemblerX64::asMasm()
{
    return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler&
MacroAssemblerX64::asMasm() const
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
            subq(Imm32(4096), StackPointer);
            store32(Imm32(0), Address(StackPointer, 0));
            amountLeft -= 4096;
        }
        subq(Imm32(amountLeft), StackPointer);
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

    movq(rsp, scratch);
    andq(Imm32(~(ABIStackAlignment - 1)), rsp);
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
        static_assert(sizeof(AsmJSFrame) % ABIStackAlignment == 0,
                      "AsmJSFrame should be part of the stack alignment.");
        stackForCall += ComputeByteAlignment(stackForCall + framePushed(),
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
    if (dynamicAlignment_)
        pop(rsp);

#ifdef DEBUG
    MOZ_ASSERT(inCall_);
    inCall_ = false;
#endif
}

static bool
IsIntArgReg(Register reg)
{
    for (uint32_t i = 0; i < NumIntArgRegs; i++) {
        if (IntArgRegs[i] == reg)
            return true;
    }

    return false;
}

void
MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result)
{
    if (IsIntArgReg(fun)) {
        // Callee register may be clobbered for an argument. Move the callee to
        // r10, a volatile, non-argument register.
        moveResolver_.addMove(MoveOperand(fun), MoveOperand(r10), MoveOp::GENERAL);
        fun = r10;
    }

    MOZ_ASSERT(!IsIntArgReg(fun));

    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(fun);
    callWithABIPost(stackAdjust, result);
}

void
MacroAssembler::callWithABINoProfiler(const Address& fun, MoveOp::Type result)
{
    Address safeFun = fun;
    if (IsIntArgReg(safeFun.base)) {
        // Callee register may be clobbered for an argument. Move the callee to
        // r10, a volatile, non-argument register.
        moveResolver_.addMove(MoveOperand(fun.base), MoveOperand(r10), MoveOp::GENERAL);
        safeFun.base = r10;
    }

    MOZ_ASSERT(!IsIntArgReg(safeFun.base));

    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(safeFun);
    callWithABIPost(stackAdjust, result);
}

//}}} check_macroassembler_style
