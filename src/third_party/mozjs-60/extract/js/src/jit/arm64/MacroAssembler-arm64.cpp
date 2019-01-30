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

void
MacroAssemblerCompat::B(wasm::OldTrapDesc target, Condition cond)
{
    Label l;
    if (cond == Always)
        B(&l);
    else
        B(&l, cond);
    bindLater(&l, target);
}

BufferOffset
MacroAssemblerCompat::movePatchablePtr(ImmPtr ptr, Register dest)
{
    const size_t numInst = 1; // Inserting one load instruction.
    const unsigned numPoolEntries = 2; // Every pool entry is 4 bytes.
    uint8_t* literalAddr = (uint8_t*)(&ptr.value); // TODO: Should be const.

    // Scratch space for generating the load instruction.
    //
    // allocLiteralLoadEntry() will use InsertIndexIntoTag() to store a temporary
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
    return allocLiteralLoadEntry(numInst, numPoolEntries, (uint8_t*)&instructionScratch,
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
    // allocLiteralLoadEntry() will use InsertIndexIntoTag() to store a temporary
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
    return allocLiteralLoadEntry(numInst, numPoolEntries, (uint8_t*)&instructionScratch,
                                 literalAddr);
}

void
MacroAssemblerCompat::loadPrivate(const Address& src, Register dest)
{
    loadPtr(src, dest);
    asMasm().lshiftPtr(Imm32(1), dest);
}

void
MacroAssemblerCompat::handleFailureWithHandlerTail(void* handler, Label* profilerExitTail)
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
    asMasm().callWithABI(handler, MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;
    Label wasm;

    MOZ_ASSERT(GetStackPointer64().Is(x28)); // Lets the code below be a little cleaner.

    loadPtr(Address(r28, offsetof(ResumeFromException, kind)), r0);
    asMasm().branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_ENTRY_FRAME),
                      &entryFrame);
    asMasm().branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    asMasm().branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    asMasm().branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_FORCED_RETURN),
                      &return_);
    asMasm().branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);
    asMasm().branch32(Assembler::Equal, r0, Imm32(ResumeFromException::RESUME_WASM), &wasm);

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

    // If we are throwing and the innermost frame was a wasm frame, reset SP and
    // FP; SP is pointing to the unwound return address to the wasm entry, so
    // we can just ret().
    bind(&wasm);
    Ldr(x29, MemOperand(GetStackPointer64(), offsetof(ResumeFromException, framePointer)));
    Ldr(x28, MemOperand(GetStackPointer64(), offsetof(ResumeFromException, stackPointer)));
    syncStackPtr();
    ret();
}

void
MacroAssemblerCompat::profilerEnterFrame(Register framePtr, Register scratch)
{
    profilerEnterFrame(RegisterOrSP(framePtr), scratch);
}

void
MacroAssemblerCompat::profilerEnterFrame(RegisterOrSP framePtr, Register scratch)
{
    asMasm().loadJSContext(scratch);
    loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
    if (IsHiddenSP(framePtr))
        storeStackPtr(Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
    else
        storePtr(AsRegister(framePtr), Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
    storePtr(ImmPtr(nullptr), Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void
MacroAssemblerCompat::breakpoint()
{
    static int code = 0xA77;
    Brk((code++) & 0xffff);
}

// Either `any` is valid or `sixtyfour` is valid.  Return a 32-bit ARMRegister
// in the first case and an ARMRegister of the desired size in the latter case.

static inline ARMRegister
SelectGPReg(AnyRegister any, Register64 sixtyfour, unsigned size = 64)
{
    MOZ_ASSERT(any.isValid() != (sixtyfour != Register64::Invalid()));

    if (sixtyfour == Register64::Invalid())
        return ARMRegister(any.gpr(), 32);

    return ARMRegister(sixtyfour.reg, size);
}

// Assert that `sixtyfour` is invalid and then return an FP register from `any`
// of the desired size.

static inline ARMFPRegister
SelectFPReg(AnyRegister any, Register64 sixtyfour, unsigned size)
{
    MOZ_ASSERT(sixtyfour == Register64::Invalid());
    return ARMFPRegister(any.fpu(), size);
}

void
MacroAssemblerCompat::wasmLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase_,
                                   Register ptr_, Register ptrScratch_, AnyRegister outany,
                                   Register64 out64)
{
    uint32_t offset = access.offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);

    MOZ_ASSERT(ptr_ == ptrScratch_);

    ARMRegister memoryBase(memoryBase_, 64);
    ARMRegister ptr(ptr_, 64);
    if (offset)
        Add(ptr, ptr, Operand(offset));

    asMasm().memoryBarrierBefore(access.sync());

    MemOperand srcAddr(memoryBase, ptr);
    size_t loadOffset = asMasm().currentOffset();
    switch (access.type()) {
      case Scalar::Int8:
        Ldrsb(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Uint8:
        Ldrb(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Int16:
        Ldrsh(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Uint16:
        Ldrh(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Int32:
        if (out64 != Register64::Invalid())
            Ldrsw(SelectGPReg(outany, out64), srcAddr);
        else
            Ldr(SelectGPReg(outany, out64, 32), srcAddr);
        break;
      case Scalar::Uint32:
        Ldr(SelectGPReg(outany, out64, 32), srcAddr);
        break;
      case Scalar::Int64:
        Ldr(SelectGPReg(outany, out64), srcAddr);
        break;
      case Scalar::Float32:
        Ldr(SelectFPReg(outany, out64, 32), srcAddr);
        break;
      case Scalar::Float64:
        Ldr(SelectFPReg(outany, out64, 64), srcAddr);
        break;
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
      case Scalar::Float32x4:
      case Scalar::Int32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
        MOZ_CRASH("unexpected array type");
    }
    append(access, loadOffset, framePushed());

    asMasm().memoryBarrierAfter(access.sync());
}

void
MacroAssemblerCompat::wasmStoreImpl(const wasm::MemoryAccessDesc& access, AnyRegister valany,
                                    Register64 val64, Register memoryBase_, Register ptr_,
                                    Register ptrScratch_)
{
    uint32_t offset = access.offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);

    MOZ_ASSERT(ptr_ == ptrScratch_);

    ARMRegister memoryBase(memoryBase_, 64);
    ARMRegister ptr(ptr_, 64);
    if (offset)
        Add(ptr, ptr, Operand(offset));

    asMasm().memoryBarrierBefore(access.sync());

    MemOperand dstAddr(memoryBase, ptr);
    size_t storeOffset = asMasm().currentOffset();
    switch (access.type()) {
      case Scalar::Int8:
      case Scalar::Uint8:
        Strb(SelectGPReg(valany, val64), dstAddr);
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        Strh(SelectGPReg(valany, val64), dstAddr);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        Str(SelectGPReg(valany, val64), dstAddr);
        break;
      case Scalar::Int64:
        Str(SelectGPReg(valany, val64), dstAddr);
        break;
      case Scalar::Float32:
        Str(SelectFPReg(valany, val64, 32), dstAddr);
        break;
      case Scalar::Float64:
        Str(SelectFPReg(valany, val64, 64), dstAddr);
        break;
      case Scalar::Float32x4:
      case Scalar::Int32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Uint8Clamped:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
    }
    append(access, storeOffset, framePushed());

    asMasm().memoryBarrierAfter(access.sync());
}

void
MacroAssembler::reserveStack(uint32_t amount)
{
    // TODO: This bumps |sp| every time we reserve using a second register.
    // It would save some instructions if we had a fixed frame size.
    vixl::MacroAssembler::Claim(Operand(amount));
    adjustFrame(amount);
}

void
MacroAssembler::Push(RegisterOrSP reg)
{
    if (IsHiddenSP(reg))
        push(sp);
    else
        push(AsRegister(reg));
    adjustFrame(sizeof(intptr_t));
}

//{{{ check_macroassembler_style
// ===============================================================
// MacroAssembler high-level usage.

void
MacroAssembler::flush()
{
    Assembler::flush();
}

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
MacroAssembler::storeRegsInMask(LiveRegisterSet set, Address dest, Register scratch)
{
    MOZ_CRASH("NYI: storeRegsInMask");
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
MacroAssembler::Pop(FloatRegister f)
{
    MOZ_CRASH("NYI: Pop(FloatRegister)");
}

void
MacroAssembler::Pop(const ValueOperand& val)
{
    pop(val);
    adjustFrame(-1 * int64_t(sizeof(int64_t)));
}

void
MacroAssembler::PopStackPtr()
{
    MOZ_CRASH("NYI");
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
MacroAssembler::call(const Address& addr)
{
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    syncStackPtr();
    loadPtr(addr, scratch);
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
    bl(0, LabelDoc());
    return CodeOffset(currentOffset());
}
void
MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset)
{
    Instruction* inst = getInstructionAt(BufferOffset(callerOffset - 4));
    MOZ_ASSERT(inst->IsBL());
    bl(inst, ((int)calleeOffset - ((int)callerOffset - 4)) >> 2);
}

CodeOffset
MacroAssembler::farJumpWithPatch()
{
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch = temps.AcquireX();
    const ARMRegister scratch2 = temps.AcquireX();

    AutoForbidPools afp(this, /* max number of instructions in scope = */ 7);

    mozilla::DebugOnly<uint32_t> before = currentOffset();

    align(8);                   // At most one nop

    Label branch;
    adr(scratch2, &branch);
    ldr(scratch, vixl::MemOperand(scratch2, 4));
    add(scratch2, scratch2, scratch);
    CodeOffset offs(currentOffset());
    bind(&branch);
    br(scratch2);
    Emit(UINT32_MAX);
    Emit(UINT32_MAX);

    mozilla::DebugOnly<uint32_t> after = currentOffset();

    MOZ_ASSERT(after - before == 24 || after - before == 28);

    return offs;
}

void
MacroAssembler::patchFarJump(CodeOffset farJump, uint32_t targetOffset)
{
    Instruction* inst1 = getInstructionAt(BufferOffset(farJump.offset() + 4));
    Instruction* inst2 = getInstructionAt(BufferOffset(farJump.offset() + 8));

    int64_t distance = (int64_t)targetOffset - (int64_t)farJump.offset();

    MOZ_ASSERT(inst1->InstructionBits() == UINT32_MAX);
    MOZ_ASSERT(inst2->InstructionBits() == UINT32_MAX);

    inst1->SetInstructionBits((uint32_t)distance);
    inst2->SetInstructionBits((uint32_t)(distance >> 32));
}

void
MacroAssembler::repatchFarJump(uint8_t* code, uint32_t farJumpOffset, uint32_t targetOffset)
{
    MOZ_CRASH("Unimplemented - never used");
}

CodeOffset
MacroAssembler::nopPatchableToNearJump()
{
    MOZ_CRASH("Unimplemented - never used");
}

void
MacroAssembler::patchNopToNearJump(uint8_t* jump, uint8_t* target)
{
    MOZ_CRASH("Unimplemented - never used");
}

void
MacroAssembler::patchNearJumpToNop(uint8_t* jump)
{
    MOZ_CRASH("Unimplemented - never used");
}

CodeOffset
MacroAssembler::nopPatchableToCall(const wasm::CallSiteDesc& desc)
{
    CodeOffset offset(currentOffset());
    Nop();
    append(desc, CodeOffset(currentOffset()));
    return offset;
}

void
MacroAssembler::patchNopToCall(uint8_t* call, uint8_t* target)
{
    uint8_t* inst = call - 4;
    Instruction* instr = reinterpret_cast<Instruction*>(inst);
    MOZ_ASSERT(instr->IsBL() || instr->IsNOP());
    bl(instr, (target - inst) >> 2);
    AutoFlushICache::flush(uintptr_t(inst), 4);
}

void
MacroAssembler::patchCallToNop(uint8_t* call)
{
    uint8_t* inst = call - 4;
    Instruction* instr = reinterpret_cast<Instruction*>(inst);
    MOZ_ASSERT(instr->IsBL() || instr->IsNOP());
    nop(instr);
    AutoFlushICache::flush(uintptr_t(inst), 4);
}

void
MacroAssembler::pushReturnAddress()
{
    MOZ_RELEASE_ASSERT(!sp.Is(GetStackPointer64()), "Not valid");
    push(lr);
}

void
MacroAssembler::popReturnAddress()
{
    MOZ_RELEASE_ASSERT(!sp.Is(GetStackPointer64()), "Not valid");
    pop(lr);
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
MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm)
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
        enoughMemory_ &= moveResolver_.resolve();
        if (!enoughMemory_)
            return;
        MoveEmitter emitter(*this);
        emitter.emit(moveResolver_);
        emitter.finish();
    }

    // Call boundaries communicate stack via sp.
    syncStackPtr();
}

void
MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result, bool callFromWasm)
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
        boxNonDouble(ValueTypeFromMIRType(type), reg.gpr(), dest);
        return;
    }

    FloatRegister scratch = ScratchDoubleReg;
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
    if (src == dest)
        return;
    movePtr(src.valueReg(), dest.valueReg());
}

void
MacroAssembler::moveValue(const Value& src, const ValueOperand& dest)
{
    if (!src.isGCThing()) {
        movePtr(ImmWord(src.asRawBits()), dest.valueReg());
        return;
    }

    BufferOffset load = movePatchablePtr(ImmPtr(src.bitsAsPunboxPointer()), dest.valueReg());
    writeDataRelocation(src, load);
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
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != temp);
    MOZ_ASSERT(ptr != ScratchReg && ptr != ScratchReg2); // Both may be used internally.
    MOZ_ASSERT(temp != ScratchReg && temp != ScratchReg2);

    movePtr(ptr, temp);
    orPtr(Imm32(gc::ChunkMask), temp);
    branch32(cond, Address(temp, gc::ChunkLocationOffsetFromLastByte),
             Imm32(int32_t(gc::ChunkLocation::Nursery)), label);
}

void
MacroAssembler::branchValueIsNurseryCell(Condition cond, const Address& address, Register temp,
                                         Label* label)
{
    branchValueIsNurseryCellImpl(cond, address, temp, label);
}

void
MacroAssembler::branchValueIsNurseryCell(Condition cond, ValueOperand value, Register temp,
                                         Label* label)
{
    branchValueIsNurseryCellImpl(cond, value, temp, label);
}

template <typename T>
void
MacroAssembler::branchValueIsNurseryCellImpl(Condition cond, const T& value, Register temp,
                                             Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(temp != ScratchReg && temp != ScratchReg2); // Both may be used internally.

    Label done, checkAddress, checkObjectAddress;
    bool testNursery = (cond == Assembler::Equal);
    branchTestObject(Assembler::Equal, value, &checkObjectAddress);
    branchTestString(Assembler::NotEqual, value, testNursery ? &done : label);

    unboxString(value, temp);
    jump(&checkAddress);

    bind(&checkObjectAddress);
    unboxObject(value, temp);

    bind(&checkAddress);
    orPtr(Imm32(gc::ChunkMask), temp);
    branch32(cond, Address(temp, gc::ChunkLocationOffsetFromLastByte),
             Imm32(int32_t(gc::ChunkLocation::Nursery)), label);

    bind(&done);
}

void
MacroAssembler::branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp,
                                           Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(temp != ScratchReg && temp != ScratchReg2); // Both may be used internally.

    Label done;
    branchTestObject(Assembler::NotEqual, value, cond == Assembler::Equal ? &done : label);

    extractObject(value, temp);
    orPtr(Imm32(gc::ChunkMask), temp);
    branch32(cond, Address(temp, gc::ChunkLocationOffsetFromLastByte),
             Imm32(int32_t(gc::ChunkLocation::Nursery)), label);

    bind(&done);
}

void
MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                const Value& rhs, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != lhs.valueReg());
    moveValue(rhs, ValueOperand(scratch64.asUnsized()));
    Cmp(ARMRegister(lhs.valueReg(), 64), scratch64);
    B(label, cond);
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

    // For known integers and booleans, we can just store the unboxed value if
    // the slot has the same type.
    if ((valueType == MIRType::Int32 || valueType == MIRType::Boolean) && slotType == valueType) {
        if (value.constant()) {
            Value val = value.value();
            if (valueType == MIRType::Int32)
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
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const Address& dest, MIRType slotType);
template void
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const BaseIndex& dest, MIRType slotType);

void
MacroAssembler::comment(const char* msg)
{
    Assembler::comment(msg);
}

// ========================================================================
// wasm support

CodeOffset
MacroAssembler::wasmTrapInstruction()
{
    CodeOffset offs(currentOffset());
    Unreachable();
    return offs;
}

// FCVTZU behaves as follows:
//
// on NaN it produces zero
// on too large it produces UINT_MAX (for appropriate type)
// on too small it produces zero
//
// FCVTZS behaves as follows:
//
// on NaN it produces zero
// on too large it produces INT_MAX (for appropriate type)
// on too small it produces INT_MIN (ditto)

void
MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input_, Register output_,
                                           bool isSaturating, Label* oolEntry)
{
    ARMRegister output(output_, 32);
    ARMFPRegister input(input_, 64);
    Fcvtzu(output, input);
    if (!isSaturating) {
        Cmp(output, 0);
        Ccmp(output, -1, vixl::ZFlag, Assembler::NotEqual);
        B(oolEntry, Assembler::Equal);
    }
}

void
MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input_, Register output_,
                                            bool isSaturating, Label* oolEntry)
{
    ARMRegister output(output_, 32);
    ARMFPRegister input(input_, 32);
    Fcvtzu(output, input);
    if (!isSaturating) {
        Cmp(output, 0);
        Ccmp(output, -1, vixl::ZFlag, Assembler::NotEqual);
        B(oolEntry, Assembler::Equal);
    }
}

void
MacroAssembler::wasmTruncateDoubleToInt32(FloatRegister input_, Register output_,
                                          bool isSaturating, Label* oolEntry)
{
    ARMRegister output(output_, 32);
    ARMFPRegister input(input_, 64);
    Fcvtzs(output, input);
    if (!isSaturating) {
        Cmp(output, 0);
        Ccmp(output, INT32_MAX, vixl::ZFlag, Assembler::NotEqual);
        Ccmp(output, INT32_MIN, vixl::ZFlag, Assembler::NotEqual);
        B(oolEntry, Assembler::Equal);
    }
}

void
MacroAssembler::wasmTruncateFloat32ToInt32(FloatRegister input_, Register output_,
                                           bool isSaturating, Label* oolEntry)
{
    ARMRegister output(output_, 32);
    ARMFPRegister input(input_, 32);
    Fcvtzs(output, input);
    if (!isSaturating) {
        Cmp(output, 0);
        Ccmp(output, INT32_MAX, vixl::ZFlag, Assembler::NotEqual);
        Ccmp(output, INT32_MIN, vixl::ZFlag, Assembler::NotEqual);
        B(oolEntry, Assembler::Equal);
    }
}

void
MacroAssembler::wasmTruncateDoubleToUInt64(FloatRegister input_, Register64 output_,
                                           bool isSaturating, Label* oolEntry,
                                           Label* oolRejoin, FloatRegister tempDouble)
{
    MOZ_ASSERT(tempDouble == InvalidFloatReg);

    ARMRegister output(output_.reg, 64);
    ARMFPRegister input(input_, 64);
    Fcvtzu(output, input);
    if (!isSaturating) {
        Cmp(output, 0);
        Ccmp(output, -1, vixl::ZFlag, Assembler::NotEqual);
        B(oolEntry, Assembler::Equal);
        bind(oolRejoin);
    }
}

void
MacroAssembler::wasmTruncateFloat32ToUInt64(FloatRegister input_, Register64 output_,
                                            bool isSaturating, Label* oolEntry,
                                            Label* oolRejoin, FloatRegister tempDouble)
{
    MOZ_ASSERT(tempDouble == InvalidFloatReg);

    ARMRegister output(output_.reg, 64);
    ARMFPRegister input(input_, 32);
    Fcvtzu(output, input);
    if (!isSaturating) {
        Cmp(output, 0);
        Ccmp(output, -1, vixl::ZFlag, Assembler::NotEqual);
        B(oolEntry, Assembler::Equal);
        bind(oolRejoin);
    }
}

void
MacroAssembler::wasmTruncateDoubleToInt64(FloatRegister input_, Register64 output_,
                                          bool isSaturating, Label* oolEntry,
                                          Label* oolRejoin, FloatRegister tempDouble)
{
    MOZ_ASSERT(tempDouble == InvalidFloatReg);

    ARMRegister output(output_.reg, 64);
    ARMFPRegister input(input_, 64);
    Fcvtzs(output, input);
    if (!isSaturating) {
        Cmp(output, 0);
        Ccmp(output, INT64_MAX, vixl::ZFlag, Assembler::NotEqual);
        Ccmp(output, INT64_MIN, vixl::ZFlag, Assembler::NotEqual);
        B(oolEntry, Assembler::Equal);
        bind(oolRejoin);
    }
}

void
MacroAssembler::wasmTruncateFloat32ToInt64(FloatRegister input_, Register64 output_,
                                           bool isSaturating, Label* oolEntry,
                                           Label* oolRejoin, FloatRegister tempDouble)
{
    ARMRegister output(output_.reg, 64);
    ARMFPRegister input(input_, 32);
    Fcvtzs(output, input);
    if (!isSaturating) {
        Cmp(output, 0);
        Ccmp(output, INT64_MAX, vixl::ZFlag, Assembler::NotEqual);
        Ccmp(output, INT64_MIN, vixl::ZFlag, Assembler::NotEqual);
        B(oolEntry, Assembler::Equal);
        bind(oolRejoin);
    }
}

void
MacroAssembler::oolWasmTruncateCheckF32ToI32(FloatRegister input, Register output, TruncFlags flags,
                                             wasm::BytecodeOffset off, Label* rejoin)
{
    Label notNaN;
    branchFloat(Assembler::DoubleOrdered, input, input, &notNaN);
    wasmTrap(wasm::Trap::InvalidConversionToInteger, off);
    bind(&notNaN);

    Label isOverflow;
    const float two_31 = -float(INT32_MIN);
    if (flags & TRUNC_UNSIGNED) {
        loadConstantFloat32(two_31 * 2, ScratchFloat32Reg);
        branchFloat(Assembler::DoubleGreaterThanOrEqual, input, ScratchFloat32Reg, &isOverflow);
        loadConstantFloat32(-1.0f, ScratchFloat32Reg);
        branchFloat(Assembler::DoubleGreaterThan, input, ScratchFloat32Reg, rejoin);
    } else {
        loadConstantFloat32(two_31, ScratchFloat32Reg);
        branchFloat(Assembler::DoubleGreaterThanOrEqual, input, ScratchFloat32Reg, &isOverflow);
        loadConstantFloat32(-two_31, ScratchFloat32Reg);
        branchFloat(Assembler::DoubleGreaterThanOrEqual, input, ScratchFloat32Reg, rejoin);
    }
    bind(&isOverflow);
    wasmTrap(wasm::Trap::IntegerOverflow, off);
}

void
MacroAssembler::oolWasmTruncateCheckF64ToI32(FloatRegister input, Register output, TruncFlags flags,
                                             wasm::BytecodeOffset off, Label* rejoin)
{
    Label notNaN;
    branchDouble(Assembler::DoubleOrdered, input, input, &notNaN);
    wasmTrap(wasm::Trap::InvalidConversionToInteger, off);
    bind(&notNaN);

    Label isOverflow;
    const double two_31 = -double(INT32_MIN);
    if (flags & TRUNC_UNSIGNED) {
        loadConstantDouble(two_31 * 2, ScratchDoubleReg);
        branchDouble(Assembler::DoubleGreaterThanOrEqual, input, ScratchDoubleReg, &isOverflow);
        loadConstantDouble(-1.0, ScratchDoubleReg);
        branchDouble(Assembler::DoubleGreaterThan, input, ScratchDoubleReg, rejoin);
    } else {
        loadConstantDouble(two_31, ScratchDoubleReg);
        branchDouble(Assembler::DoubleGreaterThanOrEqual, input, ScratchDoubleReg, &isOverflow);
        loadConstantDouble(-two_31 - 1, ScratchDoubleReg);
        branchDouble(Assembler::DoubleGreaterThan, input, ScratchDoubleReg, rejoin);
    }
    bind(&isOverflow);
    wasmTrap(wasm::Trap::IntegerOverflow, off);
}

void
MacroAssembler::oolWasmTruncateCheckF32ToI64(FloatRegister input, Register64 output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    Label notNaN;
    branchFloat(Assembler::DoubleOrdered, input, input, &notNaN);
    wasmTrap(wasm::Trap::InvalidConversionToInteger, off);
    bind(&notNaN);

    Label isOverflow;
    const float two_63 = -float(INT64_MIN);
    if (flags & TRUNC_UNSIGNED) {
        loadConstantFloat32(two_63 * 2, ScratchFloat32Reg);
        branchFloat(Assembler::DoubleGreaterThanOrEqual, input, ScratchFloat32Reg, &isOverflow);
        loadConstantFloat32(-1.0f, ScratchFloat32Reg);
        branchFloat(Assembler::DoubleGreaterThan, input, ScratchFloat32Reg, rejoin);
    } else {
        loadConstantFloat32(two_63, ScratchFloat32Reg);
        branchFloat(Assembler::DoubleGreaterThanOrEqual, input, ScratchFloat32Reg, &isOverflow);
        loadConstantFloat32(-two_63, ScratchFloat32Reg);
        branchFloat(Assembler::DoubleGreaterThanOrEqual, input, ScratchFloat32Reg, rejoin);
    }
    bind(&isOverflow);
    wasmTrap(wasm::Trap::IntegerOverflow, off);
}

void
MacroAssembler::oolWasmTruncateCheckF64ToI64(FloatRegister input, Register64 output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    Label notNaN;
    branchDouble(Assembler::DoubleOrdered, input, input, &notNaN);
    wasmTrap(wasm::Trap::InvalidConversionToInteger, off);
    bind(&notNaN);

    Label isOverflow;
    const double two_63 = -double(INT64_MIN);
    if (flags & TRUNC_UNSIGNED) {
        loadConstantDouble(two_63 * 2, ScratchDoubleReg);
        branchDouble(Assembler::DoubleGreaterThanOrEqual, input, ScratchDoubleReg, &isOverflow);
        loadConstantDouble(-1.0, ScratchDoubleReg);
        branchDouble(Assembler::DoubleGreaterThan, input, ScratchDoubleReg, rejoin);
    } else {
        loadConstantDouble(two_63, ScratchDoubleReg);
        branchDouble(Assembler::DoubleGreaterThanOrEqual, input, ScratchDoubleReg, &isOverflow);
        loadConstantDouble(-two_63, ScratchDoubleReg);
        branchDouble(Assembler::DoubleGreaterThanOrEqual, input, ScratchDoubleReg, rejoin);
    }
    bind(&isOverflow);
    wasmTrap(wasm::Trap::IntegerOverflow, off);
}

void
MacroAssembler::wasmLoad(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                         Register ptrScratch, AnyRegister output)
{
    wasmLoadImpl(access, memoryBase, ptr, ptrScratch, output, Register64::Invalid());
}

void
MacroAssembler::wasmLoadI64(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                            Register ptrScratch, Register64 output)
{
    wasmLoadImpl(access, memoryBase, ptr, ptrScratch, AnyRegister(), output);
}

void
MacroAssembler::wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value,
                          Register memoryBase, Register ptr, Register ptrScratch)
{
    wasmStoreImpl(access, value, Register64::Invalid(), memoryBase, ptr, ptrScratch);
}

void
MacroAssembler::wasmStoreI64(const wasm::MemoryAccessDesc& access, Register64 value,
                             Register memoryBase, Register ptr, Register ptrScratch)
{
    wasmStoreImpl(access, AnyRegister(), value, memoryBase, ptr, ptrScratch);
}

// ========================================================================
// Convert floating point.

bool
MacroAssembler::convertUInt64ToDoubleNeedsTemp()
{
    return false;
}

void
MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest, Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());
    Ucvtf(ARMFPRegister(dest, 64), ARMRegister(src.reg, 64));
}

void
MacroAssembler::convertInt64ToDouble(Register64 src, FloatRegister dest)
{
    Scvtf(ARMFPRegister(dest, 64), ARMRegister(src.reg, 64));
}

void
MacroAssembler::convertUInt64ToFloat32(Register64 src, FloatRegister dest, Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());
    Ucvtf(ARMFPRegister(dest, 32), ARMRegister(src.reg, 64));
}

void
MacroAssembler::convertInt64ToFloat32(Register64 src, FloatRegister dest)
{
    Scvtf(ARMFPRegister(dest, 32), ARMRegister(src.reg, 64));
}

// ========================================================================
// Primitive atomic operations.

// The computed MemOperand must be Reg+0 because the load/store exclusive
// instructions only take a single pointer register.

enum class Width {
    _32 = 32,
    _64 = 64
};

static inline ARMRegister
X(Register r) {
    return ARMRegister(r, 64);
}

static inline ARMRegister
X(MacroAssembler& masm, RegisterOrSP r) {
    return masm.toARMRegister(r, 64);
}

static inline ARMRegister
W(Register r) {
    return ARMRegister(r, 32);
}

static inline ARMRegister
R(Register r, Width w) {
    return ARMRegister(r, unsigned(w));
}

static MemOperand
ComputePointerForAtomic(MacroAssembler& masm, const Address& address, Register scratch)
{
    if (address.offset == 0)
        return MemOperand(X(masm, address.base), 0);

    masm.Add(X(scratch), X(masm, address.base), address.offset);
    return MemOperand(X(scratch), 0);
}

static MemOperand
ComputePointerForAtomic(MacroAssembler& masm, const BaseIndex& address, Register scratch)
{
    masm.Add(X(scratch), X(masm, address.base), Operand(X(address.index), vixl::LSL, address.scale));
    if (address.offset)
        masm.Add(X(scratch), X(scratch), address.offset);
    return MemOperand(X(scratch), 0);
}

// This sign extends to targetWidth and leaves any higher bits zero.

static void
SignOrZeroExtend(MacroAssembler& masm, Scalar::Type srcType, Width targetWidth, Register src,
                 Register dest)
{
    bool signExtend = Scalar::isSignedIntType(srcType);

    switch (Scalar::byteSize(srcType)) {
      case 1:
        if (signExtend)
            masm.Sbfm(R(dest, targetWidth), R(src, targetWidth), 0, 7);
        else
            masm.Ubfm(R(dest, targetWidth), R(src, targetWidth), 0, 7);
        break;
      case 2:
        if (signExtend)
            masm.Sbfm(R(dest, targetWidth), R(src, targetWidth), 0, 15);
        else
            masm.Ubfm(R(dest, targetWidth), R(src, targetWidth), 0, 15);
        break;
      case 4:
        if (targetWidth == Width::_64) {
            if (signExtend)
                masm.Sbfm(X(dest), X(src), 0, 31);
            else
                masm.Ubfm(X(dest), X(src), 0, 31);
        } else if (src != dest) {
            masm.Mov(R(dest, targetWidth), R(src, targetWidth));
        }
        break;
      case 8:
        if (src != dest)
            masm.Mov(R(dest, targetWidth), R(src, targetWidth));
        break;
      default:
        MOZ_CRASH();
    }
}

// Exclusive-loads zero-extend their values to the full width of the X register.
//
// Note, we've promised to leave the high bits of the 64-bit register clear if
// the targetWidth is 32.

static void
LoadExclusive(MacroAssembler& masm, Scalar::Type srcType, Width targetWidth, MemOperand ptr,
              Register dest)
{
    bool signExtend = Scalar::isSignedIntType(srcType);

    switch (Scalar::byteSize(srcType)) {
      case 1:
        masm.Ldxrb(W(dest), ptr);
        if (signExtend)
            masm.Sbfm(R(dest, targetWidth), R(dest, targetWidth), 0, 7);
        break;
      case 2:
        masm.Ldxrh(W(dest), ptr);
        if (signExtend)
            masm.Sbfm(R(dest, targetWidth), R(dest, targetWidth), 0, 15);
        break;
      case 4:
        masm.Ldxr(W(dest), ptr);
        if (targetWidth == Width::_64 && signExtend)
            masm.Sbfm(X(dest), X(dest), 0, 31);
        break;
      case 8:
        masm.Ldxr(X(dest), ptr);
        break;
      default:
        MOZ_CRASH();
    }
}

static void
StoreExclusive(MacroAssembler& masm, Scalar::Type type, Register status, Register src,
               MemOperand ptr)
{
    switch (Scalar::byteSize(type)) {
      case 1:
        masm.Stxrb(W(status), W(src), ptr);
        break;
      case 2:
        masm.Stxrh(W(status), W(src), ptr);
        break;
      case 4:
        masm.Stxr(W(status), W(src), ptr);
        break;
      case 8:
        masm.Stxr(W(status), X(src), ptr);
        break;
    }
}

template<typename T>
static void
CompareExchange(MacroAssembler& masm, Scalar::Type type, Width targetWidth,
                const Synchronization& sync, const T& mem, Register oldval,
                Register newval, Register output)
{
    Label again;
    Label done;

    vixl::UseScratchRegisterScope temps(&masm);

    Register scratch2 = temps.AcquireX().asUnsized();
    MemOperand ptr = ComputePointerForAtomic(masm, mem, scratch2);

    masm.memoryBarrierBefore(sync);

    Register scratch = temps.AcquireX().asUnsized();

    masm.bind(&again);
    SignOrZeroExtend(masm, type, targetWidth, oldval, scratch);
    LoadExclusive(masm, type, targetWidth, ptr, output);
    masm.Cmp(R(output, targetWidth), R(scratch, targetWidth));
    masm.B(&done, MacroAssembler::NotEqual);
    StoreExclusive(masm, type, scratch, newval, ptr);
    masm.Cbnz(W(scratch), &again);
    masm.bind(&done);

    masm.memoryBarrierAfter(sync);
}

template<typename T>
static void
AtomicExchange(MacroAssembler& masm, Scalar::Type type, Width targetWidth,
               const Synchronization& sync, const T& mem, Register value,
               Register output)
{
    Label again;

    vixl::UseScratchRegisterScope temps(&masm);

    Register scratch2 = temps.AcquireX().asUnsized();
    MemOperand ptr = ComputePointerForAtomic(masm, mem, scratch2);

    masm.memoryBarrierBefore(sync);

    Register scratch = temps.AcquireX().asUnsized();

    masm.bind(&again);
    LoadExclusive(masm, type, targetWidth, ptr, output);
    StoreExclusive(masm, type, scratch, value, ptr);
    masm.Cbnz(W(scratch), &again);

    masm.memoryBarrierAfter(sync);
}

template<bool wantResult, typename T>
static void
AtomicFetchOp(MacroAssembler& masm, Scalar::Type type, Width targetWidth,
              const Synchronization& sync, AtomicOp op, const T& mem,
              Register value, Register temp, Register output)
{
    Label again;

    vixl::UseScratchRegisterScope temps(&masm);

    Register scratch2 = temps.AcquireX().asUnsized();
    MemOperand ptr = ComputePointerForAtomic(masm, mem, scratch2);

    masm.memoryBarrierBefore(sync);

    Register scratch = temps.AcquireX().asUnsized();

    masm.bind(&again);
    LoadExclusive(masm, type, targetWidth, ptr, output);
    switch (op) {
      case AtomicFetchAddOp: masm.Add(X(temp), X(output), X(value)); break;
      case AtomicFetchSubOp: masm.Sub(X(temp), X(output), X(value)); break;
      case AtomicFetchAndOp: masm.And(X(temp), X(output), X(value)); break;
      case AtomicFetchOrOp:  masm.Orr(X(temp), X(output), X(value)); break;
      case AtomicFetchXorOp: masm.Eor(X(temp), X(output), X(value)); break;
    }
    StoreExclusive(masm, type, scratch, temp, ptr);
    masm.Cbnz(W(scratch), &again);
    if (wantResult)
        SignOrZeroExtend(masm, type, targetWidth, output, output);

    masm.memoryBarrierAfter(sync);
}

void
MacroAssembler::compareExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                                Register oldval, Register newval, Register output)
{
    CompareExchange(*this, type, Width::_32, sync, mem, oldval, newval, output);
}

void
MacroAssembler::compareExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                                Register oldval, Register newval, Register output)
{
    CompareExchange(*this, type, Width::_32, sync, mem, oldval, newval, output);
}

void
MacroAssembler::atomicExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                               Register value, Register output)
{
    AtomicExchange(*this, type, Width::_32, sync, mem, value, output);
}

void
MacroAssembler::atomicExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                               Register value, Register output)
{
    AtomicExchange(*this, type, Width::_32, sync, mem, value, output);
}

void
MacroAssembler::atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                              Register value, const Address& mem, Register temp, Register output)
{
    AtomicFetchOp<true>(*this, type, Width::_32, sync, op, mem, value, temp, output);
}

void
MacroAssembler::atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                              Register value, const BaseIndex& mem, Register temp, Register output)
{
    AtomicFetchOp<true>(*this, type, Width::_32, sync, op, mem, value, temp, output);
}

void
MacroAssembler::atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                               Register value, const Address& mem, Register temp)
{
    AtomicFetchOp<false>(*this, type, Width::_32, sync, op, mem, value, temp, temp);
}

void
MacroAssembler::atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                               Register value, const BaseIndex& mem, Register temp)
{
    AtomicFetchOp<false>(*this, type, Width::_32, sync, op, mem, value, temp, temp);
}

void
MacroAssembler::compareExchange64(const Synchronization& sync, const Address& mem, Register64 expect,
                                  Register64 replace, Register64 output)
{
    CompareExchange(*this, Scalar::Int64, Width::_64, sync, mem, expect.reg, replace.reg, output.reg);
}

void
MacroAssembler::compareExchange64(const Synchronization& sync, const BaseIndex& mem, Register64 expect,
                                  Register64 replace, Register64 output)
{
    CompareExchange(*this, Scalar::Int64, Width::_64, sync, mem, expect.reg, replace.reg, output.reg);
}

void
MacroAssembler::atomicExchange64(const Synchronization& sync, const Address& mem, Register64 value,
                                 Register64 output)
{
    AtomicExchange(*this, Scalar::Int64, Width::_64, sync, mem, value.reg, output.reg);
}

void
MacroAssembler::atomicExchange64(const Synchronization& sync, const BaseIndex& mem, Register64 value,
                                 Register64 output)
{
    AtomicExchange(*this, Scalar::Int64, Width::_64, sync, mem, value.reg, output.reg);
}

void
MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op, Register64 value,
                                const Address& mem, Register64 temp, Register64 output)
{
    AtomicFetchOp<true>(*this, Scalar::Int64, Width::_64, sync, op, mem, value.reg, temp.reg,
                        output.reg);
}

void
MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op, Register64 value,
                                const BaseIndex& mem, Register64 temp, Register64 output)
{
    AtomicFetchOp<true>(*this, Scalar::Int64, Width::_64, sync, op, mem, value.reg, temp.reg,
                        output.reg);
}

// ========================================================================
// JS atomic operations.

template<typename T>
static void
CompareExchangeJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                  const T& mem, Register oldval, Register newval, Register temp, AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.compareExchange(arrayType, sync, mem, oldval, newval, temp);
        masm.convertUInt32ToDouble(temp, output.fpu());
    } else {
        masm.compareExchange(arrayType, sync, mem, oldval, newval, output.gpr());
    }
}

void
MacroAssembler::compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                  const Address& mem, Register oldval, Register newval,
                                  Register temp, AnyRegister output)
{
    CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

void
MacroAssembler::compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                  const BaseIndex& mem, Register oldval, Register newval,
                                  Register temp, AnyRegister output)
{
    CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

template<typename T>
static void
AtomicExchangeJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                 const T& mem, Register value, Register temp, AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.atomicExchange(arrayType, sync, mem, value, temp);
        masm.convertUInt32ToDouble(temp, output.fpu());
    } else {
        masm.atomicExchange(arrayType, sync, mem, value, output.gpr());
    }
}

void
MacroAssembler::atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                 const Address& mem, Register value, Register temp,
                                 AnyRegister output)
{
    AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

void
MacroAssembler::atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                 const BaseIndex& mem, Register value, Register temp,
                                 AnyRegister output)
{
    AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

template<typename T>
static void
AtomicFetchOpJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                AtomicOp op, Register value, const T& mem, Register temp1, Register temp2,
                AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.atomicFetchOp(arrayType, sync, op, value, mem, temp2, temp1);
        masm.convertUInt32ToDouble(temp1, output.fpu());
    } else {
        masm.atomicFetchOp(arrayType, sync, op, value, mem, temp1, output.gpr());
    }
}

void
MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                                Register value, const Address& mem, Register temp1, Register temp2,
                                AnyRegister output)
{
    AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void
MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                                Register value, const BaseIndex& mem, Register temp1, Register temp2,
                                AnyRegister output)
{
    AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void
MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                           Register value, const BaseIndex& mem, Register temp)
{
    atomicEffectOp(arrayType, sync, op, value, mem, temp);
}

void
MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                           Register value, const Address& mem, Register temp)
{
    atomicEffectOp(arrayType, sync, op, value, mem, temp);
}

// ========================================================================
// Spectre Mitigations.

void
MacroAssembler::speculationBarrier()
{
    // Conditional speculation barrier.
    csdb();
}

//}}} check_macroassembler_style

} // namespace jit
} // namespace js
