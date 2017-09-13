/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/MacroAssembler-arm64.h"

#include "jit/arm64/MoveEmitter-arm64.h"
#include "jit/arm64/SharedICRegisters-arm64.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/MacroAssembler.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

void
MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output)
{
    ARMRegister dest(output, 32);
    Fcvtns(dest, ARMFPRegister(input, 64));

    {
        vixl::UseScratchRegisterScope temps(this);
        const ARMRegister scratch32 = temps.AcquireW();

        Mov(scratch32, Operand(0xff));
        Cmp(dest, scratch32);
        Csel(dest, dest, scratch32, LessThan);
    }

    Cmp(dest, Operand(0));
    Csel(dest, dest, wzr, GreaterThan);
}

void
MacroAssembler::alignFrameForICArguments(MacroAssembler::AfterICSaveLive& aic)
{
    // Exists for MIPS compatibility.
}

void
MacroAssembler::restoreFrameAlignmentForICArguments(MacroAssembler::AfterICSaveLive& aic)
{
    // Exists for MIPS compatibility.
}

js::jit::MacroAssembler&
MacroAssemblerCompat::asMasm()
{
    return *static_cast<js::jit::MacroAssembler*>(this);
}

const js::jit::MacroAssembler&
MacroAssemblerCompat::asMasm() const
{
    return *static_cast<const js::jit::MacroAssembler*>(this);
}

vixl::MacroAssembler&
MacroAssemblerCompat::asVIXL()
{
    return *static_cast<vixl::MacroAssembler*>(this);
}

const vixl::MacroAssembler&
MacroAssemblerCompat::asVIXL() const
{
    return *static_cast<const vixl::MacroAssembler*>(this);
}

BufferOffset
MacroAssemblerCompat::movePatchablePtr(ImmPtr ptr, Register dest)
{
    const size_t numInst = 1; // Inserting one load instruction.
    const unsigned numPoolEntries = 2; // Every pool entry is 4 bytes.
    uint8_t* literalAddr = (uint8_t*)(&ptr.value); // TODO: Should be const.

    // Scratch space for generating the load instruction.
    //
    // allocEntry() will use InsertIndexIntoTag() to store a temporary
    // index to the corresponding PoolEntry in the instruction itself.
    //
    // That index will be fixed up later when finishPool()
    // walks over all marked loads and calls PatchConstantPoolLoad().
    uint32_t instructionScratch = 0;

    // Emit the instruction mask in the scratch space.
    // The offset doesn't matter: it will be fixed up later.
    vixl::Assembler::ldr((Instruction*)&instructionScratch, ARMRegister(dest, 64), 0);

    // Add the entry to the pool, fix up the LDR imm19 offset,
    // and add the completed instruction to the buffer.
    return allocEntry(numInst, numPoolEntries, (uint8_t*)&instructionScratch,
                      literalAddr);
}

BufferOffset
MacroAssemblerCompat::movePatchablePtr(ImmWord ptr, Register dest)
{
    const size_t numInst = 1; // Inserting one load instruction.
    const unsigned numPoolEntries = 2; // Every pool entry is 4 bytes.
    uint8_t* literalAddr = (uint8_t*)(&ptr.value);

    // Scratch space for generating the load instruction.
    //
    // allocEntry() will use InsertIndexIntoTag() to store a temporary
    // index to the corresponding PoolEntry in the instruction itself.
    //
    // That index will be fixed up later when finishPool()
    // walks over all marked loads and calls PatchConstantPoolLoad().
    uint32_t instructionScratch = 0;

    // Emit the instruction mask in the scratch space.
    // The offset doesn't matter: it will be fixed up later.
    vixl::Assembler::ldr((Instruction*)&instructionScratch, ARMRegister(dest, 64), 0);

    // Add the entry to the pool, fix up the LDR imm19 offset,
    // and add the completed instruction to the buffer.
    return allocEntry(numInst, numPoolEntries, (uint8_t*)&instructionScratch,
                      literalAddr);
}

void
MacroAssemblerCompat::loadPrivate(const Address& src, Register dest)
{
    loadPtr(src, dest);
    asMasm().lshiftPtr(Imm32(1), dest);
}

void
MacroAssemblerCompat::handleFailureWithHandlerTail(void* handler)
{
    // Reserve space for exception information.
    int64_t size = (sizeof(ResumeFromException) + 7) & ~7;
    Sub(GetStackPointer64(), GetStackPointer64(), Operand(size));
    if (!GetStackPointer64().Is(sp))
        Mov(sp, GetStackPointer64());

    Mov(x0, GetStackPointer64());

    // Call the handler.
    asMasm().setupUnalignedABICall(r1);
    asMasm().passABIArg(r0);
    asMasm().callWithABI(handler);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;

    MOZ_ASSERT(GetStackPointer64().Is(x28)); // Lets the code below be a little cleaner.

    loadPtr(Address(r28, offsetof(ResumeFromException, kind)), r0);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_ENTRY_FRAME), &entryFrame);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
    branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);

    breakpoint(); // Invalid kind.

    // No exception handler. Load the error value, load the new stack pointer,
    // and return from the entry frame.
    bind(&entryFrame);
    moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    loadPtr(Address(r28, offsetof(ResumeFromException, stackPointer)), r28);
    retn(Imm32(1 * sizeof(void*))); // Pop from stack and return.

    // If we found a catch handler, this must be a baseline frame. Restore state
    // and jump to the catch block.
    bind(&catch_);
    loadPtr(Address(r28, offsetof(ResumeFromException, target)), r0);
    loadPtr(Address(r28, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(r28, offsetof(ResumeFromException, stackPointer)), r28);
    syncStackPtr();
    Br(x0);

    // If we found a finally block, this must be a baseline frame.
    // Push two values expected by JSOP_RETSUB: BooleanValue(true)
    // and the exception.
    bind(&finally);
    ARMRegister exception = x1;
    Ldr(exception, MemOperand(GetStackPointer64(), offsetof(ResumeFromException, exception)));
    Ldr(x0, MemOperand(GetStackPointer64(), offsetof(ResumeFromException, target)));
    Ldr(ARMRegister(BaselineFrameReg, 64),
        MemOperand(GetStackPointer64(), offsetof(ResumeFromException, framePointer)));
    Ldr(GetStackPointer64(), MemOperand(GetStackPointer64(), offsetof(ResumeFromException, stackPointer)));
    syncStackPtr();
    pushValue(BooleanValue(true));
    push(exception);
    Br(x0);

    // Only used in debug mode. Return BaselineFrame->returnValue() to the caller.
    bind(&return_);
    loadPtr(Address(r28, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(r28, offsetof(ResumeFromException, stackPointer)), r28);
    loadValue(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue()),
              JSReturnOperand);
    movePtr(BaselineFrameReg, r28);
    vixl::MacroAssembler::Pop(ARMRegister(BaselineFrameReg, 64), vixl::lr);
    syncStackPtr();
    vixl::MacroAssembler::Ret(vixl::lr);

    // If we are bailing out to baseline to handle an exception,
    // jump to the bailout tail stub.
    bind(&bailout);
    Ldr(x2, MemOperand(GetStackPointer64(), offsetof(ResumeFromException, bailoutInfo)));
    Ldr(x1, MemOperand(GetStackPointer64(), offsetof(ResumeFromException, target)));
    Mov(x0, BAILOUT_RETURN_OK);
    Br(x1);
}

void
MacroAssemblerCompat::branchPtrInNurseryRange(Condition cond, Register ptr, Register temp,
                                              Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != temp);
    MOZ_ASSERT(ptr != ScratchReg && ptr != ScratchReg2); // Both may be used internally.
    MOZ_ASSERT(temp != ScratchReg && temp != ScratchReg2);

    const Nursery& nursery = GetJitContext()->runtime->gcNursery();
    movePtr(ImmWord(-ptrdiff_t(nursery.start())), temp);
    addPtr(ptr, temp);
    branchPtr(cond == Assembler::Equal ? Assembler::Below : Assembler::AboveOrEqual,
              temp, ImmWord(nursery.nurserySize()), label);
}

void
MacroAssemblerCompat::branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp,
                                                 Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(temp != ScratchReg && temp != ScratchReg2); // Both may be used internally.

    const Nursery& nursery = GetJitContext()->runtime->gcNursery();

    // Avoid creating a bogus ObjectValue below.
    if (!nursery.exists())
        return;

    // 'Value' representing the start of the nursery tagged as a JSObject
    Value start = ObjectValue(*reinterpret_cast<JSObject*>(nursery.start()));

    movePtr(ImmWord(-ptrdiff_t(start.asRawBits())), temp);
    addPtr(value.valueReg(), temp);
    branchPtr(cond == Assembler::Equal ? Assembler::Below : Assembler::AboveOrEqual,
              temp, ImmWord(nursery.nurserySize()), label);
}

void
MacroAssemblerCompat::breakpoint()
{
    static int code = 0xA77;
    Brk((code++) & 0xffff);
}

template<typename T>
void
MacroAssemblerCompat::compareExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem,
                                                     Register oldval, Register newval,
                                                     Register temp, AnyRegister output)
{
    switch (arrayType) {
      case Scalar::Int8:
        compareExchange8SignExtend(mem, oldval, newval, output.gpr());
        break;
      case Scalar::Uint8:
        compareExchange8ZeroExtend(mem, oldval, newval, output.gpr());
        break;
      case Scalar::Int16:
        compareExchange16SignExtend(mem, oldval, newval, output.gpr());
        break;
      case Scalar::Uint16:
        compareExchange16ZeroExtend(mem, oldval, newval, output.gpr());
        break;
      case Scalar::Int32:
        compareExchange32(mem, oldval, newval, output.gpr());
        break;
      case Scalar::Uint32:
        // At the moment, the code in MCallOptimize.cpp requires the output
        // type to be double for uint32 arrays.  See bug 1077305.
        MOZ_ASSERT(output.isFloat());
        compareExchange32(mem, oldval, newval, temp);
        convertUInt32ToDouble(temp, output.fpu());
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

template void
MacroAssemblerCompat::compareExchangeToTypedIntArray(Scalar::Type arrayType, const Address& mem,
                                                     Register oldval, Register newval, Register temp,
                                                     AnyRegister output);
template void
MacroAssemblerCompat::compareExchangeToTypedIntArray(Scalar::Type arrayType, const BaseIndex& mem,
                                                     Register oldval, Register newval, Register temp,
                                                     AnyRegister output);

template<typename T>
void
MacroAssemblerCompat::atomicExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem,
                                                    Register value, Register temp, AnyRegister output)
{
    switch (arrayType) {
      case Scalar::Int8:
        atomicExchange8SignExtend(mem, value, output.gpr());
        break;
      case Scalar::Uint8:
        atomicExchange8ZeroExtend(mem, value, output.gpr());
        break;
      case Scalar::Int16:
        atomicExchange16SignExtend(mem, value, output.gpr());
        break;
      case Scalar::Uint16:
        atomicExchange16ZeroExtend(mem, value, output.gpr());
        break;
      case Scalar::Int32:
        atomicExchange32(mem, value, output.gpr());
        break;
      case Scalar::Uint32:
        // At the moment, the code in MCallOptimize.cpp requires the output
        // type to be double for uint32 arrays.  See bug 1077305.
        MOZ_ASSERT(output.isFloat());
        atomicExchange32(mem, value, temp);
        convertUInt32ToDouble(temp, output.fpu());
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

template void
MacroAssemblerCompat::atomicExchangeToTypedIntArray(Scalar::Type arrayType, const Address& mem,
                                                    Register value, Register temp, AnyRegister output);
template void
MacroAssemblerCompat::atomicExchangeToTypedIntArray(Scalar::Type arrayType, const BaseIndex& mem,
                                                    Register value, Register temp, AnyRegister output);

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

void
MacroAssembler::PushRegsInMask(LiveRegisterSet set)
{
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ) {
        vixl::CPURegister src[4] = { vixl::NoCPUReg, vixl::NoCPUReg, vixl::NoCPUReg, vixl::NoCPUReg };

        for (size_t i = 0; i < 4 && iter.more(); i++) {
            src[i] = ARMRegister(*iter, 64);
            ++iter;
            adjustFrame(8);
        }
        vixl::MacroAssembler::Push(src[0], src[1], src[2], src[3]);
    }

    for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush()); iter.more(); ) {
        vixl::CPURegister src[4] = { vixl::NoCPUReg, vixl::NoCPUReg, vixl::NoCPUReg, vixl::NoCPUReg };

        for (size_t i = 0; i < 4 && iter.more(); i++) {
            src[i] = ARMFPRegister(*iter, 64);
            ++iter;
            adjustFrame(8);
        }
        vixl::MacroAssembler::Push(src[0], src[1], src[2], src[3]);
    }
}

void
MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set, LiveRegisterSet ignore)
{
    // The offset of the data from the stack pointer.
    uint32_t offset = 0;

    for (FloatRegisterIterator iter(set.fpus().reduceSetForPush()); iter.more(); ) {
        vixl::CPURegister dest[2] = { vixl::NoCPUReg, vixl::NoCPUReg };
        uint32_t nextOffset = offset;

        for (size_t i = 0; i < 2 && iter.more(); i++) {
            if (!ignore.has(*iter))
                dest[i] = ARMFPRegister(*iter, 64);
            ++iter;
            nextOffset += sizeof(double);
        }

        if (!dest[0].IsNone() && !dest[1].IsNone())
            Ldp(dest[0], dest[1], MemOperand(GetStackPointer64(), offset));
        else if (!dest[0].IsNone())
            Ldr(dest[0], MemOperand(GetStackPointer64(), offset));
        else if (!dest[1].IsNone())
            Ldr(dest[1], MemOperand(GetStackPointer64(), offset + sizeof(double)));

        offset = nextOffset;
    }

    MOZ_ASSERT(offset == set.fpus().getPushSizeInBytes());

    for (GeneralRegisterIterator iter(set.gprs()); iter.more(); ) {
        vixl::CPURegister dest[2] = { vixl::NoCPUReg, vixl::NoCPUReg };
        uint32_t nextOffset = offset;

        for (size_t i = 0; i < 2 && iter.more(); i++) {
            if (!ignore.has(*iter))
                dest[i] = ARMRegister(*iter, 64);
            ++iter;
            nextOffset += sizeof(uint64_t);
        }

        if (!dest[0].IsNone() && !dest[1].IsNone())
            Ldp(dest[0], dest[1], MemOperand(GetStackPointer64(), offset));
        else if (!dest[0].IsNone())
            Ldr(dest[0], MemOperand(GetStackPointer64(), offset));
        else if (!dest[1].IsNone())
            Ldr(dest[1], MemOperand(GetStackPointer64(), offset + sizeof(uint64_t)));

        offset = nextOffset;
    }

    size_t bytesPushed = set.gprs().size() * sizeof(uint64_t) + set.fpus().getPushSizeInBytes();
    MOZ_ASSERT(offset == bytesPushed);
    freeStack(bytesPushed);
}

void
MacroAssembler::Push(Register reg)
{
    push(reg);
    adjustFrame(sizeof(intptr_t));
}

void
MacroAssembler::Push(Register reg1, Register reg2, Register reg3, Register reg4)
{
    push(reg1, reg2, reg3, reg4);
    adjustFrame(4 * sizeof(intptr_t));
}

void
MacroAssembler::Push(const Imm32 imm)
{
    push(imm);
    adjustFrame(sizeof(intptr_t));
}

void
MacroAssembler::Push(const ImmWord imm)
{
    push(imm);
    adjustFrame(sizeof(intptr_t));
}

void
MacroAssembler::Push(const ImmPtr imm)
{
    push(imm);
    adjustFrame(sizeof(intptr_t));
}

void
MacroAssembler::Push(const ImmGCPtr ptr)
{
    push(ptr);
    adjustFrame(sizeof(intptr_t));
}

void
MacroAssembler::Push(FloatRegister f)
{
    push(f);
    adjustFrame(sizeof(double));
}

void
MacroAssembler::Pop(Register reg)
{
    pop(reg);
    adjustFrame(-1 * int64_t(sizeof(int64_t)));
}

void
MacroAssembler::Pop(const ValueOperand& val)
{
    pop(val);
    adjustFrame(-1 * int64_t(sizeof(int64_t)));
}

void
MacroAssembler::reserveStack(uint32_t amount)
{
    // TODO: This bumps |sp| every time we reserve using a second register.
    // It would save some instructions if we had a fixed frame size.
    vixl::MacroAssembler::Claim(Operand(amount));
    adjustFrame(amount);
}

// ===============================================================
// Simple call functions.

CodeOffset
MacroAssembler::call(Register reg)
{
    syncStackPtr();
    Blr(ARMRegister(reg, 64));
    return CodeOffset(currentOffset());
}

CodeOffset
MacroAssembler::call(Label* label)
{
    syncStackPtr();
    Bl(label);
    return CodeOffset(currentOffset());
}

void
MacroAssembler::call(ImmWord imm)
{
    call(ImmPtr((void*)imm.value));
}

void
MacroAssembler::call(ImmPtr imm)
{
    syncStackPtr();
    movePtr(imm, ip0);
    Blr(vixl::ip0);
}

void
MacroAssembler::call(wasm::SymbolicAddress imm)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    syncStackPtr();
    movePtr(imm, scratch);
    call(scratch);
}

void
MacroAssembler::call(JitCode* c)
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    syncStackPtr();
    BufferOffset off = immPool64(scratch64, uint64_t(c->raw()));
    addPendingJump(off, ImmPtr(c->raw()), Relocation::JITCODE);
    blr(scratch64);
}

CodeOffset
MacroAssembler::callWithPatch()
{
    MOZ_CRASH("NYI");
    return CodeOffset();
}
void
MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset)
{
    MOZ_CRASH("NYI");
}

void
MacroAssembler::pushReturnAddress()
{
    push(lr);
}

// ===============================================================
// ABI function calls.

void
MacroAssembler::setupUnalignedABICall(Register scratch)
{
    setupABICall();
    dynamicAlignment_ = true;

    int64_t alignment = ~(int64_t(ABIStackAlignment) - 1);
    ARMRegister scratch64(scratch, 64);

    // Always save LR -- Baseline ICs assume that LR isn't modified.
    push(lr);

    // Unhandled for sp -- needs slightly different logic.
    MOZ_ASSERT(!GetStackPointer64().Is(sp));

    // Remember the stack address on entry.
    Mov(scratch64, GetStackPointer64());

    // Make alignment, including the effective push of the previous sp.
    Sub(GetStackPointer64(), GetStackPointer64(), Operand(8));
    And(GetStackPointer64(), GetStackPointer64(), Operand(alignment));

    // If the PseudoStackPointer is used, sp must be <= psp before a write is valid.
    syncStackPtr();

    // Store previous sp to the top of the stack, aligned.
    Str(scratch64, MemOperand(GetStackPointer64(), 0));
}

void
MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromAsmJS)
{
    MOZ_ASSERT(inCall_);
    uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

    // ARM64 /really/ wants the stack to always be aligned.  Since we're already tracking it
    // getting it aligned for an abi call is pretty easy.
    MOZ_ASSERT(dynamicAlignment_);
    stackForCall += ComputeByteAlignment(stackForCall, StackAlignment);
    *stackAdjust = stackForCall;
    reserveStack(*stackAdjust);
    {
        moveResolver_.resolve();
        MoveEmitter emitter(*this);
        emitter.emit(moveResolver_);
        emitter.finish();
    }

    // Call boundaries communicate stack via sp.
    syncStackPtr();
}

void
MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result)
{
    // Call boundaries communicate stack via sp.
    if (!GetStackPointer64().Is(sp))
        Mov(GetStackPointer64(), sp);

    freeStack(stackAdjust);

    // Restore the stack pointer from entry.
    if (dynamicAlignment_)
        Ldr(GetStackPointer64(), MemOperand(GetStackPointer64(), 0));

    // Restore LR.
    pop(lr);

    // TODO: This one shouldn't be necessary -- check that callers
    // aren't enforcing the ABI themselves!
    syncStackPtr();

    // If the ABI's return regs are where ION is expecting them, then
    // no other work needs to be done.

#ifdef DEBUG
    MOZ_ASSERT(inCall_);
    inCall_ = false;
#endif
}

void
MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    movePtr(fun, scratch);

    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(scratch);
    callWithABIPost(stackAdjust, result);
}

void
MacroAssembler::callWithABINoProfiler(const Address& fun, MoveOp::Type result)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    loadPtr(fun, scratch);

    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(scratch);
    callWithABIPost(stackAdjust, result);
}

// ===============================================================
// Jit Frames.

uint32_t
MacroAssembler::pushFakeReturnAddress(Register scratch)
{
    enterNoPool(3);
    Label fakeCallsite;

    Adr(ARMRegister(scratch, 64), &fakeCallsite);
    Push(scratch);
    bind(&fakeCallsite);
    uint32_t pseudoReturnOffset = currentOffset();

    leaveNoPool();
    return pseudoReturnOffset;
}

//}}} check_macroassembler_style

} // namespace jit
} // namespace js
