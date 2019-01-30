/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/MacroAssembler-x86.h"

#include "mozilla/Alignment.h"
#include "mozilla/Casting.h"

#include "jit/AtomicOp.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveEmitter.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

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
MacroAssemblerX86::loadConstantSimd128Int(const SimdConstant& v, FloatRegister dest)
{
    if (maybeInlineSimd128Int(v, dest))
        return;
    SimdData* i4 = getSimdData(v);
    if (!i4)
        return;
    masm.vmovdqa_mr(nullptr, dest.encoding());
    propagateOOM(i4->uses.append(CodeOffset(masm.size())));
}

void
MacroAssemblerX86::loadConstantSimd128Float(const SimdConstant& v, FloatRegister dest)
{
    if (maybeInlineSimd128Float(v, dest))
        return;
    SimdData* f4 = getSimdData(v);
    if (!f4)
        return;
    masm.vmovaps_mr(nullptr, dest.encoding());
    propagateOOM(f4->uses.append(CodeOffset(masm.size())));
}

void
MacroAssemblerX86::finish()
{
    // Last instruction may be an indirect jump so eagerly insert an undefined
    // instruction byte to prevent processors from decoding data values into
    // their pipelines. See Intel performance guides.
    masm.ud2();

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
        masm.simd128Constant(v.value.bytes());
        if (!enoughMemory_)
            return;
    }
}

void
MacroAssemblerX86::handleFailureWithHandlerTail(void* handler, Label* profilerExitTail)
{
    // Reserve space for exception information.
    subl(Imm32(sizeof(ResumeFromException)), esp);
    movl(esp, eax);

    // Call the handler.
    asMasm().setupUnalignedABICall(ecx);
    asMasm().passABIArg(eax);
    asMasm().callWithABI(handler, MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;
    Label wasm;

    loadPtr(Address(esp, offsetof(ResumeFromException, kind)), eax);
    asMasm().branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_ENTRY_FRAME),
                      &entryFrame);
    asMasm().branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    asMasm().branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    asMasm().branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_FORCED_RETURN),
                      &return_);
    asMasm().branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);
    asMasm().branch32(Assembler::Equal, eax, Imm32(ResumeFromException::RESUME_WASM), &wasm);

    breakpoint(); // Invalid kind.

    // No exception handler. Load the error value, load the new stack pointer
    // and return from the entry frame.
    bind(&entryFrame);
    asMasm().moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
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
        AbsoluteAddress addressOfEnabled(GetJitContext()->runtime->geckoProfiler().addressOfEnabled());
        asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                          &skipProfilingInstrumentation);
        jump(profilerExitTail);
        bind(&skipProfilingInstrumentation);
    }

    ret();

    // If we are bailing out to baseline to handle an exception, jump to
    // the bailout tail stub.
    bind(&bailout);
    loadPtr(Address(esp, offsetof(ResumeFromException, bailoutInfo)), ecx);
    movl(Imm32(BAILOUT_RETURN_OK), eax);
    jmp(Operand(esp, offsetof(ResumeFromException, target)));

    // If we are throwing and the innermost frame was a wasm frame, reset SP and
    // FP; SP is pointing to the unwound return address to the wasm entry, so
    // we can just ret().
    bind(&wasm);
    loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
    loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
    masm.ret();
}

void
MacroAssemblerX86::profilerEnterFrame(Register framePtr, Register scratch)
{
    asMasm().loadJSContext(scratch);
    loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
    storePtr(framePtr, Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
    storePtr(ImmPtr(nullptr), Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void
MacroAssemblerX86::profilerExitFrame()
{
    jump(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
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

void
MacroAssembler::subFromStackPtr(Imm32 imm32)
{
    if (imm32.value) {
        // On windows, we cannot skip very far down the stack without touching the
        // memory pages in-between.  This is a corner-case code for situations where the
        // Ion frame data for a piece of code is very large.  To handle this special case,
        // for frames over 4k in size we allocate memory on the stack incrementally, touching
        // it as we go.
        //
        // When the amount is quite large, which it can be, we emit an actual loop, in order
        // to keep the function prologue compact.  Compactness is a requirement for eg
        // Wasm's CodeRange data structure, which can encode only 8-bit offsets.
        uint32_t amountLeft = imm32.value;
        uint32_t fullPages = amountLeft / 4096;
        if (fullPages <= 8) {
            while (amountLeft > 4096) {
                subl(Imm32(4096), StackPointer);
                store32(Imm32(0), Address(StackPointer, 0));
                amountLeft -= 4096;
            }
            subl(Imm32(amountLeft), StackPointer);
        } else {
            // Save scratch register.
            push(eax);
            amountLeft -= 4;
            fullPages = amountLeft / 4096;

            Label top;
            move32(Imm32(fullPages), eax);
            bind(&top);
            subl(Imm32(4096), StackPointer);
            store32(Imm32(0), Address(StackPointer, 0));
            subl(Imm32(1), eax);
            j(Assembler::NonZero, &top);
            amountLeft -= fullPages * 4096;
            if (amountLeft)
                subl(Imm32(amountLeft), StackPointer);

            // Restore scratch register.
            movl(Operand(StackPointer, uint32_t(imm32.value) - 4), eax);
        }
    }
}

//{{{ check_macroassembler_style
// ===============================================================
// ABI function calls.

void
MacroAssembler::setupUnalignedABICall(Register scratch)
{
    MOZ_ASSERT(!IsCompilingWasm(), "wasm should only use aligned ABI calls");
    setupABICall();
    dynamicAlignment_ = true;

    movl(esp, scratch);
    andl(Imm32(~(ABIStackAlignment - 1)), esp);
    push(scratch);
}

void
MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm)
{
    MOZ_ASSERT(inCall_);
    uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

    if (dynamicAlignment_) {
        // sizeof(intptr_t) accounts for the saved stack pointer pushed by
        // setupUnalignedABICall.
        stackForCall += ComputeByteAlignment(stackForCall + sizeof(intptr_t),
                                             ABIStackAlignment);
    } else {
        uint32_t alignmentAtPrologue = callFromWasm ? sizeof(wasm::Frame) : 0;
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
MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result, bool callFromWasm)
{
    freeStack(stackAdjust);

    // Calls to native functions in wasm pass through a thunk which already
    // fixes up the return value for us.
    if (!callFromWasm) {
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

// ===============================================================
// Move instructions

void
MacroAssembler::moveValue(const TypedOrValueRegister& src, const ValueOperand& dest)
{
    if (src.hasValue()) {
        moveValue(src.valueReg(), dest);
        return;
    }

    MIRType type = src.type();
    AnyRegister reg = src.typedReg();

    if (!IsFloatingPointType(type)) {
        mov(ImmWord(MIRTypeToTag(type)), dest.typeReg());
        if (reg.gpr() != dest.payloadReg())
            movl(reg.gpr(), dest.payloadReg());
        return;
    }

    ScratchDoubleScope scratch(*this);
    FloatRegister freg = reg.fpu();
    if (type == MIRType::Float32) {
        convertFloat32ToDouble(freg, scratch);
        freg = scratch;
    }
    boxDouble(freg, dest, scratch);
}

void
MacroAssembler::moveValue(const ValueOperand& src, const ValueOperand& dest)
{
    Register s0 = src.typeReg();
    Register s1 = src.payloadReg();
    Register d0 = dest.typeReg();
    Register d1 = dest.payloadReg();

    // Either one or both of the source registers could be the same as a
    // destination register.
    if (s1 == d0) {
        if (s0 == d1) {
            // If both are, this is just a swap of two registers.
            xchgl(d0, d1);
            return;
        }
        // If only one is, copy that source first.
        mozilla::Swap(s0, s1);
        mozilla::Swap(d0, d1);
    }

    if (s0 != d0)
        movl(s0, d0);
    if (s1 != d1)
        movl(s1, d1);
}

void
MacroAssembler::moveValue(const Value& src, const ValueOperand& dest)
{
    movl(Imm32(src.toNunboxTag()), dest.typeReg());
    if (src.isGCThing())
        movl(ImmGCPtr(src.toGCThing()), dest.payloadReg());
    else
        movl(Imm32(src.toNunboxPayload()), dest.payloadReg());
}

// ===============================================================
// Branch functions

void
MacroAssembler::loadStoreBuffer(Register ptr, Register buffer)
{
    if (ptr != buffer)
        movePtr(ptr, buffer);
    orPtr(Imm32(gc::ChunkMask), buffer);
    loadPtr(Address(buffer, gc::ChunkStoreBufferOffsetFromLastByte), buffer);
}

void
MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr, Register temp,
                                        Label* label)
{
    MOZ_ASSERT(temp != InvalidReg);  // A temp register is required for x86.
    MOZ_ASSERT(ptr != temp);
    movePtr(ptr, temp);
    branchPtrInNurseryChunkImpl(cond, temp, label);
}

void
MacroAssembler::branchPtrInNurseryChunk(Condition cond, const Address& address, Register temp,
                                        Label* label)
{
    MOZ_ASSERT(temp != InvalidReg);  // A temp register is required for x86.
    loadPtr(address, temp);
    branchPtrInNurseryChunkImpl(cond, temp, label);
}

void
MacroAssembler::branchPtrInNurseryChunkImpl(Condition cond, Register ptr, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    orPtr(Imm32(gc::ChunkMask), ptr);
    branch32(cond, Address(ptr, gc::ChunkLocationOffsetFromLastByte),
             Imm32(int32_t(gc::ChunkLocation::Nursery)), label);
}

void
MacroAssembler::branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp,
                                           Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    Label done;

    branchTestObject(Assembler::NotEqual, value, cond == Assembler::Equal ? &done : label);
    branchPtrInNurseryChunk(cond, value.payloadReg(), temp, label);

    bind(&done);
}

void
MacroAssembler::branchValueIsNurseryCell(Condition cond, const Address& address, Register temp,
                                         Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Label done, checkAddress;

    Register tag = extractTag(address, temp);
    MOZ_ASSERT(tag == temp);
    branchTestObject(Assembler::Equal, tag, &checkAddress);
    branchTestString(Assembler::NotEqual, tag, cond == Assembler::Equal ? &done : label);

    bind(&checkAddress);
    branchPtrInNurseryChunk(cond, ToPayload(address), temp, label);

    bind(&done);
}

void
MacroAssembler::branchValueIsNurseryCell(Condition cond, ValueOperand value, Register temp,
                                         Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Label done, checkAddress;

    branchTestObject(Assembler::Equal, value, &checkAddress);
    branchTestString(Assembler::NotEqual, value, cond == Assembler::Equal ? &done : label);

    bind(&checkAddress);
    branchPtrInNurseryChunk(cond, value.payloadReg(), temp, label);

    bind(&done);
}

void
MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                const Value& rhs, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    if (rhs.isGCThing())
        cmpPtr(lhs.payloadReg(), ImmGCPtr(rhs.toGCThing()));
    else
        cmpPtr(lhs.payloadReg(), ImmWord(rhs.toNunboxPayload()));

    if (cond == Equal) {
        Label done;
        j(NotEqual, &done);
        {
            cmp32(lhs.typeReg(), Imm32(rhs.toNunboxTag()));
            j(Equal, label);
        }
        bind(&done);
    } else {
        j(NotEqual, label);

        cmp32(lhs.typeReg(), Imm32(rhs.toNunboxTag()));
        j(NotEqual, label);
    }
}

// ========================================================================
// Memory access primitives.
template <typename T>
void
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const T& dest, MIRType slotType)
{
    if (valueType == MIRType::Double) {
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
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const Address& dest, MIRType slotType);
template void
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const BaseIndex& dest, MIRType slotType);

// wasm specific methods, used in both the wasm baseline compiler and ion.

void
MacroAssembler::wasmLoad(const wasm::MemoryAccessDesc& access, Operand srcAddr, AnyRegister out)
{
    MOZ_ASSERT(srcAddr.kind() == Operand::MEM_REG_DISP || srcAddr.kind() == Operand::MEM_SCALE);

    memoryBarrierBefore(access.sync());

    size_t loadOffset = size();
    switch (access.type()) {
      case Scalar::Int8:
        movsbl(srcAddr, out.gpr());
        break;
      case Scalar::Uint8:
        movzbl(srcAddr, out.gpr());
        break;
      case Scalar::Int16:
        movswl(srcAddr, out.gpr());
        break;
      case Scalar::Uint16:
        movzwl(srcAddr, out.gpr());
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        movl(srcAddr, out.gpr());
        break;
      case Scalar::Float32:
        vmovss(srcAddr, out.fpu());
        break;
      case Scalar::Float64:
        vmovsd(srcAddr, out.fpu());
        break;
      case Scalar::Float32x4:
        switch (access.numSimdElems()) {
          // In memory-to-register mode, movss zeroes out the high lanes.
          case 1: vmovss(srcAddr, out.fpu()); break;
          // See comment above, which also applies to movsd.
          case 2: vmovsd(srcAddr, out.fpu()); break;
          case 4: vmovups(srcAddr, out.fpu()); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      case Scalar::Int32x4:
        switch (access.numSimdElems()) {
          // In memory-to-register mode, movd zeroes out the high lanes.
          case 1: vmovd(srcAddr, out.fpu()); break;
          // See comment above, which also applies to movq.
          case 2: vmovq(srcAddr, out.fpu()); break;
          case 4: vmovdqu(srcAddr, out.fpu()); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      case Scalar::Int8x16:
        MOZ_ASSERT(access.numSimdElems() == 16, "unexpected partial load");
        vmovdqu(srcAddr, out.fpu());
        break;
      case Scalar::Int16x8:
        MOZ_ASSERT(access.numSimdElems() == 8, "unexpected partial load");
        vmovdqu(srcAddr, out.fpu());
        break;
      case Scalar::Int64:
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected type");
    }
    append(access, loadOffset, framePushed());

    memoryBarrierAfter(access.sync());
}

void
MacroAssembler::wasmLoadI64(const wasm::MemoryAccessDesc& access, Operand srcAddr, Register64 out)
{
    // Atomic i64 load must use lock_cmpxchg8b.
    MOZ_ASSERT_IF(access.isAtomic(), access.byteSize() <= 4);
    MOZ_ASSERT(!access.isSimd());
    MOZ_ASSERT(srcAddr.kind() == Operand::MEM_REG_DISP || srcAddr.kind() == Operand::MEM_SCALE);

    memoryBarrierBefore(access.sync());

    size_t loadOffset = size();
    switch (access.type()) {
      case Scalar::Int8:
        MOZ_ASSERT(out == Register64(edx, eax));
        movsbl(srcAddr, out.low);
        append(access, loadOffset, framePushed());

        cdq();
        break;
      case Scalar::Uint8:
        movzbl(srcAddr, out.low);
        append(access, loadOffset, framePushed());

        xorl(out.high, out.high);
        break;
      case Scalar::Int16:
        MOZ_ASSERT(out == Register64(edx, eax));
        movswl(srcAddr, out.low);
        append(access, loadOffset, framePushed());

        cdq();
        break;
      case Scalar::Uint16:
        movzwl(srcAddr, out.low);
        append(access, loadOffset, framePushed());

        xorl(out.high, out.high);
        break;
      case Scalar::Int32:
        MOZ_ASSERT(out == Register64(edx, eax));
        movl(srcAddr, out.low);
        append(access, loadOffset, framePushed());

        cdq();
        break;
      case Scalar::Uint32:
        movl(srcAddr, out.low);
        append(access, loadOffset, framePushed());

        xorl(out.high, out.high);
        break;
      case Scalar::Int64: {
        if (srcAddr.kind() == Operand::MEM_SCALE) {
            MOZ_RELEASE_ASSERT(srcAddr.toBaseIndex().base != out.low &&
                               srcAddr.toBaseIndex().index != out.low);
        }
        if (srcAddr.kind() == Operand::MEM_REG_DISP)
            MOZ_RELEASE_ASSERT(srcAddr.toAddress().base != out.low);

        movl(LowWord(srcAddr), out.low);
        append(access, loadOffset, framePushed());

        loadOffset = size();
        movl(HighWord(srcAddr), out.high);
        append(access, loadOffset, framePushed());

        break;
      }
      case Scalar::Float32:
      case Scalar::Float64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Int32x4:
        MOZ_CRASH("non-int64 loads should use load()");
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
    }

    memoryBarrierAfter(access.sync());
}

void
MacroAssembler::wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value, Operand dstAddr)
{
    MOZ_ASSERT(dstAddr.kind() == Operand::MEM_REG_DISP || dstAddr.kind() == Operand::MEM_SCALE);

    memoryBarrierBefore(access.sync());

    size_t storeOffset = size();
    switch (access.type()) {
      case Scalar::Int8:
      case Scalar::Uint8Clamped:
      case Scalar::Uint8:
        movb(value.gpr(), dstAddr);
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        movw(value.gpr(), dstAddr);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        movl(value.gpr(), dstAddr);
        break;
      case Scalar::Float32:
        vmovss(value.fpu(), dstAddr);
        break;
      case Scalar::Float64:
        vmovsd(value.fpu(), dstAddr);
        break;
      case Scalar::Float32x4:
        switch (access.numSimdElems()) {
          // In memory-to-register mode, movss zeroes out the high lanes.
          case 1: vmovss(value.fpu(), dstAddr); break;
          // See comment above, which also applies to movsd.
          case 2: vmovsd(value.fpu(), dstAddr); break;
          case 4: vmovups(value.fpu(), dstAddr); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      case Scalar::Int32x4:
        switch (access.numSimdElems()) {
          // In memory-to-register mode, movd zeroes out the high lanes.
          case 1: vmovd(value.fpu(), dstAddr); break;
          // See comment above, which also applies to movsd.
          case 2: vmovq(value.fpu(), dstAddr); break;
          case 4: vmovdqu(value.fpu(), dstAddr); break;
          default: MOZ_CRASH("unexpected size for partial load");
        }
        break;
      case Scalar::Int8x16:
        MOZ_ASSERT(access.numSimdElems() == 16, "unexpected partial store");
        vmovdqu(value.fpu(), dstAddr);
        break;
      case Scalar::Int16x8:
        MOZ_ASSERT(access.numSimdElems() == 8, "unexpected partial store");
        vmovdqu(value.fpu(), dstAddr);
        break;
      case Scalar::Int64:
        MOZ_CRASH("Should be handled in storeI64.");
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected type");
    }
    append(access, storeOffset, framePushed());

    memoryBarrierAfter(access.sync());
}

void
MacroAssembler::wasmStoreI64(const wasm::MemoryAccessDesc& access, Register64 value, Operand dstAddr)
{
    // Atomic i64 store must use lock_cmpxchg8b.
    MOZ_ASSERT(!access.isAtomic());
    MOZ_ASSERT(!access.isSimd());
    MOZ_ASSERT(dstAddr.kind() == Operand::MEM_REG_DISP || dstAddr.kind() == Operand::MEM_SCALE);

    size_t storeOffset = size();
    movl(value.low, LowWord(dstAddr));
    append(access, storeOffset, framePushed());

    storeOffset = size();
    movl(value.high, HighWord(dstAddr));
    append(access, storeOffset, framePushed());
}

template <typename T>
static void
AtomicLoad64(MacroAssembler& masm, const T& address, Register64 temp, Register64 output)
{
    MOZ_ASSERT(temp.low == ebx);
    MOZ_ASSERT(temp.high == ecx);
    MOZ_ASSERT(output.high == edx);
    MOZ_ASSERT(output.low == eax);

    // In the event edx:eax matches what's in memory, ecx:ebx will be
    // stored.  The two pairs must therefore have the same values.

    masm.movl(edx, ecx);
    masm.movl(eax, ebx);

    masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(address));
}

void
MacroAssembler::atomicLoad64(const Synchronization&, const Address& mem, Register64 temp,
                             Register64 output)
{
    AtomicLoad64(*this, mem, temp, output);
}

void
MacroAssembler::atomicLoad64(const Synchronization&, const BaseIndex& mem, Register64 temp,
                             Register64 output)
{
    AtomicLoad64(*this, mem, temp, output);
}

template <typename T>
static void
CompareExchange64(MacroAssembler& masm, const T& mem, Register64 expected,
                  Register64 replacement, Register64 output)
{
    MOZ_ASSERT(expected == output);
    MOZ_ASSERT(expected.high == edx);
    MOZ_ASSERT(expected.low == eax);
    MOZ_ASSERT(replacement.high == ecx);
    MOZ_ASSERT(replacement.low == ebx);

    masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(mem));
}

void
MacroAssembler::compareExchange64(const Synchronization&, const Address& mem, Register64 expected,
                                  Register64 replacement, Register64 output)
{
    CompareExchange64(*this, mem, expected, replacement, output);
}

void
MacroAssembler::compareExchange64(const Synchronization&, const BaseIndex& mem, Register64 expected,
                                  Register64 replacement, Register64 output)
{
    CompareExchange64(*this, mem, expected, replacement, output);
}

template <typename T>
static void
AtomicExchange64(MacroAssembler& masm, const T& mem, Register64 value, Register64 output)
{
    MOZ_ASSERT(value.low == ebx);
    MOZ_ASSERT(value.high == ecx);
    MOZ_ASSERT(output.high == edx);
    MOZ_ASSERT(output.low == eax);

    // edx:eax has garbage initially, and that is the best we can do unless
    // we can guess with high probability what's in memory.

    Label again;
    masm.bind(&again);
    masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(mem));
    masm.j(MacroAssembler::NonZero, &again);
}

void
MacroAssembler::atomicExchange64(const Synchronization&, const Address& mem, Register64 value,
                                 Register64 output)
{
    AtomicExchange64(*this, mem, value, output);
}

void
MacroAssembler::atomicExchange64(const Synchronization&, const BaseIndex& mem, Register64 value,
                                 Register64 output)
{
    AtomicExchange64(*this, mem, value, output);
}

template<typename T>
static void
AtomicFetchOp64(MacroAssembler& masm, AtomicOp op, const Address& value, const T& mem,
                Register64 temp, Register64 output)
{

// We don't have enough registers for all the operands on x86, so the rhs
// operand is in memory.

#define ATOMIC_OP_BODY(OPERATE)                                    \
    do {                                                           \
        MOZ_ASSERT(output.low == eax);                             \
        MOZ_ASSERT(output.high == edx);                            \
        MOZ_ASSERT(temp.low == ebx);                               \
        MOZ_ASSERT(temp.high == ecx);                              \
        masm.load64(mem, output);                                  \
        Label again;                                               \
        masm.bind(&again);                                         \
        masm.move64(output, temp);                                 \
        masm.OPERATE(Operand(value), temp);                        \
        masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(mem));     \
        masm.j(MacroAssembler::NonZero, &again);                   \
    } while(0)

    switch (op) {
      case AtomicFetchAddOp:
        ATOMIC_OP_BODY(add64FromMemory);
        break;
      case AtomicFetchSubOp:
        ATOMIC_OP_BODY(sub64FromMemory);
        break;
      case AtomicFetchAndOp:
        ATOMIC_OP_BODY(and64FromMemory);
        break;
      case AtomicFetchOrOp:
        ATOMIC_OP_BODY(or64FromMemory);
        break;
      case AtomicFetchXorOp:
        ATOMIC_OP_BODY(xor64FromMemory);
        break;
      default:
        MOZ_CRASH();
    }

#undef ATOMIC_OP_BODY
}

void
MacroAssembler::atomicFetchOp64(const Synchronization&, AtomicOp op, const Address& value,
                                const Address& mem, Register64 temp, Register64 output)
{
    AtomicFetchOp64(*this, op, value, mem, temp, output);
}

void
MacroAssembler::atomicFetchOp64(const Synchronization&, AtomicOp op, const Address& value,
                                const BaseIndex& mem, Register64 temp, Register64 output)
{
    AtomicFetchOp64(*this, op, value, mem, temp, output);
}

void
MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input, Register output, bool isSaturating,
                                           Label* oolEntry)
{
    Label done;
    vcvttsd2si(input, output);
    branch32(Assembler::Condition::NotSigned, output, Imm32(0), &done);

    loadConstantDouble(double(int32_t(0x80000000)), ScratchDoubleReg);
    addDouble(input, ScratchDoubleReg);
    vcvttsd2si(ScratchDoubleReg, output);

    branch32(Assembler::Condition::Signed, output, Imm32(0), oolEntry);
    or32(Imm32(0x80000000), output);

    bind(&done);
}

void
MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input, Register output, bool isSaturating,
                                            Label* oolEntry)
{
    Label done;
    vcvttss2si(input, output);
    branch32(Assembler::Condition::NotSigned, output, Imm32(0), &done);

    loadConstantFloat32(float(int32_t(0x80000000)), ScratchFloat32Reg);
    addFloat32(input, ScratchFloat32Reg);
    vcvttss2si(ScratchFloat32Reg, output);

    branch32(Assembler::Condition::Signed, output, Imm32(0), oolEntry);
    or32(Imm32(0x80000000), output);

    bind(&done);
}

void
MacroAssembler::wasmTruncateDoubleToInt64(FloatRegister input, Register64 output, bool isSaturating,
                                          Label* oolEntry, Label* oolRejoin, FloatRegister tempReg)
{
    Label fail, convert;
    Register temp = output.high;

    // Make sure input fits in (u)int64.
    reserveStack(2 * sizeof(int32_t));
    storeDouble(input, Operand(esp, 0));
    branchDoubleNotInInt64Range(Address(esp, 0), temp, &fail);
    jump(&convert);

    // Handle failure in ool.
    bind(&fail);
    freeStack(2 * sizeof(int32_t));
    jump(oolEntry);
    bind(oolRejoin);
    reserveStack(2 * sizeof(int32_t));
    storeDouble(input, Operand(esp, 0));

    // Convert the double/float to int64.
    bind(&convert);
    truncateDoubleToInt64(Address(esp, 0), Address(esp, 0), temp);

    // Load value into int64 register.
    load64(Address(esp, 0), output);
    freeStack(2 * sizeof(int32_t));
}

void
MacroAssembler::wasmTruncateFloat32ToInt64(FloatRegister input, Register64 output,
                                           bool isSaturating,
                                           Label* oolEntry, Label* oolRejoin, FloatRegister tempReg)
{
    Label fail, convert;
    Register temp = output.high;

    // Make sure input fits in (u)int64.
    reserveStack(2 * sizeof(int32_t));
    storeFloat32(input, Operand(esp, 0));
    branchFloat32NotInInt64Range(Address(esp, 0), temp, &fail);
    jump(&convert);

    // Handle failure in ool.
    bind(&fail);
    freeStack(2 * sizeof(int32_t));
    jump(oolEntry);
    bind(oolRejoin);
    reserveStack(2 * sizeof(int32_t));
    storeFloat32(input, Operand(esp, 0));

    // Convert the double/float to int64.
    bind(&convert);
    truncateFloat32ToInt64(Address(esp, 0), Address(esp, 0), temp);

    // Load value into int64 register.
    load64(Address(esp, 0), output);
    freeStack(2 * sizeof(int32_t));
}

void
MacroAssembler::wasmTruncateDoubleToUInt64(FloatRegister input, Register64 output,
                                           bool isSaturating, Label* oolEntry,
                                           Label* oolRejoin, FloatRegister tempReg)
{
    Label fail, convert;
    Register temp = output.high;

    // Make sure input fits in (u)int64.
    reserveStack(2 * sizeof(int32_t));
    storeDouble(input, Operand(esp, 0));
    branchDoubleNotInUInt64Range(Address(esp, 0), temp, &fail);
    jump(&convert);

    // Handle failure in ool.
    bind(&fail);
    freeStack(2 * sizeof(int32_t));
    jump(oolEntry);
    bind(oolRejoin);
    reserveStack(2 * sizeof(int32_t));
    storeDouble(input, Operand(esp, 0));

    // Convert the double/float to int64.
    bind(&convert);
    truncateDoubleToUInt64(Address(esp, 0), Address(esp, 0), temp, tempReg);

    // Load value into int64 register.
    load64(Address(esp, 0), output);
    freeStack(2 * sizeof(int32_t));
}

void
MacroAssembler::wasmTruncateFloat32ToUInt64(FloatRegister input, Register64 output,
                                            bool isSaturating, Label* oolEntry,
                                            Label* oolRejoin, FloatRegister tempReg)
{
    Label fail, convert;
    Register temp = output.high;

    // Make sure input fits in (u)int64.
    reserveStack(2 * sizeof(int32_t));
    storeFloat32(input, Operand(esp, 0));
    branchFloat32NotInUInt64Range(Address(esp, 0), temp, &fail);
    jump(&convert);

    // Handle failure in ool.
    bind(&fail);
    freeStack(2 * sizeof(int32_t));
    jump(oolEntry);
    bind(oolRejoin);
    reserveStack(2 * sizeof(int32_t));
    storeFloat32(input, Operand(esp, 0));

    // Convert the double/float to int64.
    bind(&convert);
    truncateFloat32ToUInt64(Address(esp, 0), Address(esp, 0), temp, tempReg);

    // Load value into int64 register.
    load64(Address(esp, 0), output);
    freeStack(2 * sizeof(int32_t));
}


// ========================================================================
// Convert floating point.

bool
MacroAssembler::convertUInt64ToDoubleNeedsTemp()
{
    return HasSSE3();
}

void
MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest, Register temp)
{
    // SUBPD needs SSE2, HADDPD needs SSE3.
    if (!HasSSE3()) {
        MOZ_ASSERT(temp == Register::Invalid());

        // Zero the dest register to break dependencies, see convertInt32ToDouble.
        zeroDouble(dest);

        Push(src.high);
        Push(src.low);
        fild(Operand(esp, 0));

        Label notNegative;
        branch32(Assembler::NotSigned, src.high, Imm32(0), &notNegative);
        double add_constant = 18446744073709551616.0; // 2^64
        store64(Imm64(mozilla::BitwiseCast<uint64_t>(add_constant)), Address(esp, 0));
        fld(Operand(esp, 0));
        faddp();
        bind(&notNegative);

        fstp(Operand(esp, 0));
        vmovsd(Address(esp, 0), dest);
        freeStack(2 * sizeof(intptr_t));
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
    // See convertUInt64ToDouble for the details.
    static const int32_t CST1[4] = {
        0x43300000,
        0x45300000,
        0x0,
        0x0,
    };

    loadConstantSimd128Int(SimdConstant::CreateX4(CST1), ScratchSimd128Reg);
    vpunpckldq(ScratchSimd128Reg, dest128, dest128);

    // Subtract a constant C2 from dest, for each 64-bit part:
    //   C2       = 0x 45300000 00000000  43300000 00000000
    // here, each 64-bit part of C2 represents following double:
    //   HI(C2)   = 0x 1.0000000000000 * 2**84 == 2**84
    //   LO(C2)   = 0x 1.0000000000000 * 2**52 == 2**52
    // after the operation each 64-bit part of dest represents following:
    //   HI(dest) = double(0x HHHHHHHH 00000000)
    //   LO(dest) = double(0x 00000000 LLLLLLLL)
    static const int32_t CST2[4] = {
        0x0,
        0x43300000,
        0x0,
        0x45300000,
    };

    loadConstantSimd128Int(SimdConstant::CreateX4(CST2), ScratchSimd128Reg);
    vsubpd(ScratchSimd128Reg, dest128, dest128);

    // Add HI(dest) and LO(dest) in double and store it into LO(dest),
    //   LO(dest) = double(0x HHHHHHHH 00000000) + double(0x 00000000 LLLLLLLL)
    //            = double(0x HHHHHHHH LLLLLLLL)
    //            = double(src)
    vhaddpd(dest128, dest128);
}

void
MacroAssembler::convertInt64ToDouble(Register64 input, FloatRegister output)
{
    // Zero the output register to break dependencies, see convertInt32ToDouble.
    zeroDouble(output);

    Push(input.high);
    Push(input.low);
    fild(Operand(esp, 0));

    fstp(Operand(esp, 0));
    vmovsd(Address(esp, 0), output);
    freeStack(2 * sizeof(intptr_t));
}

void
MacroAssembler::convertUInt64ToFloat32(Register64 input, FloatRegister output, Register temp)
{
    // Zero the dest register to break dependencies, see convertInt32ToDouble.
    zeroDouble(output);

    // Set the FPU precision to 80 bits.
    reserveStack(2 * sizeof(intptr_t));
    fnstcw(Operand(esp, 0));
    load32(Operand(esp, 0), temp);
    orl(Imm32(0x300), temp);
    store32(temp, Operand(esp, sizeof(intptr_t)));
    fldcw(Operand(esp, sizeof(intptr_t)));

    Push(input.high);
    Push(input.low);
    fild(Operand(esp, 0));

    Label notNegative;
    branch32(Assembler::NotSigned, input.high, Imm32(0), &notNegative);
    double add_constant = 18446744073709551616.0; // 2^64
    uint64_t add_constant_u64 = mozilla::BitwiseCast<uint64_t>(add_constant);
    store64(Imm64(add_constant_u64), Address(esp, 0));

    fld(Operand(esp, 0));
    faddp();
    bind(&notNegative);

    fstp32(Operand(esp, 0));
    vmovss(Address(esp, 0), output);
    freeStack(2 * sizeof(intptr_t));

    // Restore FPU precision to the initial value.
    fldcw(Operand(esp, 0));
    freeStack(2 * sizeof(intptr_t));
}

void
MacroAssembler::convertInt64ToFloat32(Register64 input, FloatRegister output)
{
    // Zero the output register to break dependencies, see convertInt32ToDouble.
    zeroDouble(output);

    Push(input.high);
    Push(input.low);
    fild(Operand(esp, 0));

    fstp32(Operand(esp, 0));
    vmovss(Address(esp, 0), output);
    freeStack(2 * sizeof(intptr_t));
}

//}}} check_macroassembler_style

