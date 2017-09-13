/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/Assembler-mips32.h"

#include "mozilla/DebugOnly.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

ABIArgGenerator::ABIArgGenerator()
  : usedArgSlots_(0),
    firstArgFloatSize_(0),
    useGPRForFloats_(false),
    current_()
{}

ABIArg
ABIArgGenerator::next(MIRType type)
{
    Register destReg;
    switch (type) {
      case MIRType_Int32:
      case MIRType_Pointer:
        if (GetIntArgReg(usedArgSlots_, &destReg))
            current_ = ABIArg(destReg);
        else
            current_ = ABIArg(usedArgSlots_ * sizeof(intptr_t));
        usedArgSlots_++;
        break;
      case MIRType_Float32:
        if (!usedArgSlots_) {
            current_ = ABIArg(f12.asSingle());
            firstArgFloatSize_ = 1;
        } else if (usedArgSlots_ == firstArgFloatSize_) {
            current_ = ABIArg(f14.asSingle());
        } else if (useGPRForFloats_ && GetIntArgReg(usedArgSlots_, &destReg)) {
            current_ = ABIArg(destReg);
        } else {
            if (usedArgSlots_ < NumIntArgRegs)
                usedArgSlots_ = NumIntArgRegs;
            current_ = ABIArg(usedArgSlots_ * sizeof(intptr_t));
        }
        usedArgSlots_++;
        break;
      case MIRType_Double:
        if (!usedArgSlots_) {
            current_ = ABIArg(f12);
            usedArgSlots_ = 2;
            firstArgFloatSize_ = 2;
        } else if (usedArgSlots_ == firstArgFloatSize_) {
            current_ = ABIArg(f14);
            usedArgSlots_ = 4;
        } else if (useGPRForFloats_ && usedArgSlots_ <= 2) {
            current_ = ABIArg(a2, a3);
            usedArgSlots_ = 4;
        } else {
            if (usedArgSlots_ < NumIntArgRegs)
                usedArgSlots_ = NumIntArgRegs;
            usedArgSlots_ += usedArgSlots_ % 2;
            current_ = ABIArg(usedArgSlots_ * sizeof(intptr_t));
            usedArgSlots_ += 2;
        }
        break;
      default:
        MOZ_CRASH("Unexpected argument type");
    }
    return current_;
}

const Register ABIArgGenerator::NonArgReturnReg0 = t0;
const Register ABIArgGenerator::NonArgReturnReg1 = t1;
const Register ABIArgGenerator::NonArg_VolatileReg = v0;
const Register ABIArgGenerator::NonReturn_VolatileReg0 = a0;
const Register ABIArgGenerator::NonReturn_VolatileReg1 = a1;

uint32_t
js::jit::RT(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::RegisterIdLimit);
    return r.id() << RTShift;
}

uint32_t
js::jit::RD(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::RegisterIdLimit);
    return r.id() << RDShift;
}

uint32_t
js::jit::SA(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::RegisterIdLimit);
    return r.id() << SAShift;
}

// Used to patch jumps created by MacroAssemblerMIPSCompat::jumpWithPatch.
void
jit::PatchJump(CodeLocationJump& jump_, CodeLocationLabel label, ReprotectCode reprotect)
{
    Instruction* inst1 = (Instruction*)jump_.raw();
    Instruction* inst2 = inst1->next();

    MaybeAutoWritableJitCode awjc(inst1, 8, reprotect);
    Assembler::UpdateLuiOriValue(inst1, inst2, (uint32_t)label.raw());

    AutoFlushICache::flush(uintptr_t(inst1), 8);
}

// For more infromation about backedges look at comment in
// MacroAssemblerMIPSCompat::backedgeJump()
void
jit::PatchBackedge(CodeLocationJump& jump, CodeLocationLabel label,
                   JitRuntime::BackedgeTarget target)
{
    uint32_t sourceAddr = (uint32_t)jump.raw();
    uint32_t targetAddr = (uint32_t)label.raw();
    InstImm* branch = (InstImm*)jump.raw();

    MOZ_ASSERT(branch->extractOpcode() == (uint32_t(op_beq) >> OpcodeShift));

    if (BOffImm16::IsInRange(targetAddr - sourceAddr)) {
        branch->setBOffImm16(BOffImm16(targetAddr - sourceAddr));
    } else {
        if (target == JitRuntime::BackedgeLoopHeader) {
            Instruction* lui = &branch[1];
            Assembler::UpdateLuiOriValue(lui, lui->next(), targetAddr);
            // Jump to ori. The lui will be executed in delay slot.
            branch->setBOffImm16(BOffImm16(2 * sizeof(uint32_t)));
        } else {
            Instruction* lui = &branch[4];
            Assembler::UpdateLuiOriValue(lui, lui->next(), targetAddr);
            branch->setBOffImm16(BOffImm16(4 * sizeof(uint32_t)));
        }
    }
}

void
Assembler::executableCopy(uint8_t* buffer)
{
    MOZ_ASSERT(isFinished);
    m_buffer.executableCopy(buffer);

    // Patch all long jumps during code copy.
    for (size_t i = 0; i < longJumps_.length(); i++) {
        Instruction* inst1 = (Instruction*) ((uint32_t)buffer + longJumps_[i]);

        uint32_t value = Assembler::ExtractLuiOriValue(inst1, inst1->next());
        Assembler::UpdateLuiOriValue(inst1, inst1->next(), (uint32_t)buffer + value);
    }

    AutoFlushICache::setRange(uintptr_t(buffer), m_buffer.size());
}

uintptr_t
Assembler::GetPointer(uint8_t* instPtr)
{
    Instruction* inst = (Instruction*)instPtr;
    return Assembler::ExtractLuiOriValue(inst, inst->next());
}

static JitCode*
CodeFromJump(Instruction* jump)
{
    uint8_t* target = (uint8_t*)Assembler::ExtractLuiOriValue(jump, jump->next());
    return JitCode::FromExecutable(target);
}

void
Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    while (reader.more()) {
        JitCode* child = CodeFromJump((Instruction*)(code->raw() + reader.readUnsigned()));
        TraceManuallyBarrieredEdge(trc, &child, "rel32");
    }
}

static void
TraceOneDataRelocation(JSTracer* trc, Instruction* inst)
{
    void* ptr = (void*)Assembler::ExtractLuiOriValue(inst, inst->next());
    void* prior = ptr;

    // No barrier needed since these are constants.
    TraceManuallyBarrieredGenericPointerEdge(trc, reinterpret_cast<gc::Cell**>(&ptr),
                                                 "ion-masm-ptr");
    if (ptr != prior) {
        Assembler::UpdateLuiOriValue(inst, inst->next(), uint32_t(ptr));
        AutoFlushICache::flush(uintptr_t(inst), 8);
    }
}

static void
TraceDataRelocations(JSTracer* trc, uint8_t* buffer, CompactBufferReader& reader)
{
    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        Instruction* inst = (Instruction*)(buffer + offset);
        TraceOneDataRelocation(trc, inst);
    }
}

static void
TraceDataRelocations(JSTracer* trc, MIPSBuffer* buffer, CompactBufferReader& reader)
{
    while (reader.more()) {
        BufferOffset bo (reader.readUnsigned());
        MIPSBuffer::AssemblerBufferInstIterator iter(bo, buffer);
        TraceOneDataRelocation(trc, iter.cur());
    }
}

void
Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    ::TraceDataRelocations(trc, code->raw(), reader);
}

void
Assembler::trace(JSTracer* trc)
{
    for (size_t i = 0; i < jumps_.length(); i++) {
        RelativePatch& rp = jumps_[i];
        if (rp.kind == Relocation::JITCODE) {
            JitCode* code = JitCode::FromExecutable((uint8_t*)rp.target);
            TraceManuallyBarrieredEdge(trc, &code, "masmrel32");
            MOZ_ASSERT(code == JitCode::FromExecutable((uint8_t*)rp.target));
        }
    }
    if (dataRelocations_.length()) {
        CompactBufferReader reader(dataRelocations_);
        ::TraceDataRelocations(trc, &m_buffer, reader);
    }
}

void
Assembler::Bind(uint8_t* rawCode, CodeOffset* label, const void* address)
{
    if (label->bound()) {
        intptr_t offset = label->offset();
        Instruction* inst = (Instruction*) (rawCode + offset);
        Assembler::UpdateLuiOriValue(inst, inst->next(), (uint32_t)address);
    }
}

void
Assembler::bind(InstImm* inst, uintptr_t branch, uintptr_t target)
{
    int32_t offset = target - branch;
    InstImm inst_bgezal = InstImm(op_regimm, zero, rt_bgezal, BOffImm16(0));
    InstImm inst_beq = InstImm(op_beq, zero, zero, BOffImm16(0));

    // If encoded offset is 4, then the jump must be short
    if (BOffImm16(inst[0]).decode() == 4) {
        MOZ_ASSERT(BOffImm16::IsInRange(offset));
        inst[0].setBOffImm16(BOffImm16(offset));
        inst[1].makeNop();
        return;
    }

    // Generate the long jump for calls because return address has to be the
    // address after the reserved block.
    if (inst[0].encode() == inst_bgezal.encode()) {
        addLongJump(BufferOffset(branch));
        Assembler::WriteLuiOriInstructions(inst, &inst[1], ScratchRegister, target);
        inst[2] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr).encode();
        // There is 1 nop after this.
        return;
    }

    if (BOffImm16::IsInRange(offset)) {
        bool conditional = (inst[0].encode() != inst_bgezal.encode() &&
                            inst[0].encode() != inst_beq.encode());

        inst[0].setBOffImm16(BOffImm16(offset));
        inst[1].makeNop();

        // Skip the trailing nops in conditional branches.
        if (conditional) {
            inst[2] = InstImm(op_regimm, zero, rt_bgez, BOffImm16(3 * sizeof(void*))).encode();
            // There are 2 nops after this
        }
        return;
    }

    if (inst[0].encode() == inst_beq.encode()) {
        // Handle long unconditional jump.
        addLongJump(BufferOffset(branch));
        Assembler::WriteLuiOriInstructions(inst, &inst[1], ScratchRegister, target);
        inst[2] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
        // There is 1 nop after this.
    } else {
        // Handle long conditional jump.
        inst[0] = invertBranch(inst[0], BOffImm16(5 * sizeof(void*)));
        // No need for a "nop" here because we can clobber scratch.
        addLongJump(BufferOffset(branch + sizeof(void*)));
        Assembler::WriteLuiOriInstructions(&inst[1], &inst[2], ScratchRegister, target);
        inst[3] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
        // There is 1 nop after this.
    }
}

void
Assembler::bind(RepatchLabel* label)
{
    BufferOffset dest = nextOffset();
    if (label->used() && !oom()) {
        // If the label has a use, then change this use to refer to
        // the bound label;
        BufferOffset b(label->offset());
        InstImm* inst1 = (InstImm*)editSrc(b);

        // If first instruction is branch, then this is a loop backedge.
        if (inst1->extractOpcode() == ((uint32_t)op_beq >> OpcodeShift)) {
            // Backedges are short jumps when bound, but can become long
            // when patched.
            uint32_t offset = dest.getOffset() - label->offset();
            MOZ_ASSERT(BOffImm16::IsInRange(offset));
            inst1->setBOffImm16(BOffImm16(offset));
        } else {
            Assembler::UpdateLuiOriValue(inst1, inst1->next(), dest.getOffset());
        }

    }
    label->bind(dest.getOffset());
}

uint32_t
Assembler::PatchWrite_NearCallSize()
{
    return 4 * sizeof(uint32_t);
}

void
Assembler::PatchWrite_NearCall(CodeLocationLabel start, CodeLocationLabel toCall)
{
    Instruction* inst = (Instruction*) start.raw();
    uint8_t* dest = toCall.raw();

    // Overwrite whatever instruction used to be here with a call.
    // Always use long jump for two reasons:
    // - Jump has to be the same size because of PatchWrite_NearCallSize.
    // - Return address has to be at the end of replaced block.
    // Short jump wouldn't be more efficient.
    Assembler::WriteLuiOriInstructions(inst, &inst[1], ScratchRegister, (uint32_t)dest);
    inst[2] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
    inst[3] = InstNOP();

    // Ensure everyone sees the code that was just written into memory.
    AutoFlushICache::flush(uintptr_t(inst), PatchWrite_NearCallSize());
}

uint32_t
Assembler::ExtractLuiOriValue(Instruction* inst0, Instruction* inst1)
{
    InstImm* i0 = (InstImm*) inst0;
    InstImm* i1 = (InstImm*) inst1;
    MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
    MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

    uint32_t value = i0->extractImm16Value() << 16;
    value = value | i1->extractImm16Value();
    return value;
}

void
Assembler::UpdateLuiOriValue(Instruction* inst0, Instruction* inst1, uint32_t value)
{
    MOZ_ASSERT(inst0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
    MOZ_ASSERT(inst1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

    ((InstImm*) inst0)->setImm16(Imm16::Upper(Imm32(value)));
    ((InstImm*) inst1)->setImm16(Imm16::Lower(Imm32(value)));
}

void
Assembler::WriteLuiOriInstructions(Instruction* inst0, Instruction* inst1,
                                   Register reg, uint32_t value)
{
    *inst0 = InstImm(op_lui, zero, reg, Imm16::Upper(Imm32(value)));
    *inst1 = InstImm(op_ori, reg, reg, Imm16::Lower(Imm32(value)));
}

void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue,
                                   ImmPtr expectedValue)
{
    PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value),
                            PatchedImmPtr(expectedValue.value));
}

void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, PatchedImmPtr newValue,
                                   PatchedImmPtr expectedValue)
{
    Instruction* inst = (Instruction*) label.raw();

    // Extract old Value
    DebugOnly<uint32_t> value = Assembler::ExtractLuiOriValue(&inst[0], &inst[1]);
    MOZ_ASSERT(value == uint32_t(expectedValue.value));

    // Replace with new value
    Assembler::UpdateLuiOriValue(inst, inst->next(), uint32_t(newValue.value));

    AutoFlushICache::flush(uintptr_t(inst), 8);
}

void
Assembler::PatchInstructionImmediate(uint8_t* code, PatchedImmPtr imm)
{
    InstImm* inst = (InstImm*)code;
    Assembler::UpdateLuiOriValue(inst, inst->next(), (uint32_t)imm.value);
}

uint32_t
Assembler::ExtractInstructionImmediate(uint8_t* code)
{
    InstImm* inst = (InstImm*)code;
    return Assembler::ExtractLuiOriValue(inst, inst->next());
}

void
Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled)
{
    Instruction* inst = (Instruction*)inst_.raw();
    InstImm* i0 = (InstImm*) inst;
    InstImm* i1 = (InstImm*) i0->next();
    Instruction* i2 = (Instruction*) i1->next();

    MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
    MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

    if (enabled) {
        InstReg jalr = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
        *i2 = jalr;
    } else {
        InstNOP nop;
        *i2 = nop;
    }

    AutoFlushICache::flush(uintptr_t(i2), 4);
}

void
Assembler::UpdateBoundsCheck(uint32_t heapSize, Instruction* inst)
{
    InstImm* i0 = (InstImm*) inst;
    InstImm* i1 = (InstImm*) i0->next();

    // Replace with new value
    Assembler::UpdateLuiOriValue(i0, i1, heapSize);
}
