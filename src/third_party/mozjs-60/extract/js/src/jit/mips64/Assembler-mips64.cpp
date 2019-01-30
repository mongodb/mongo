/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/Assembler-mips64.h"

#include "mozilla/DebugOnly.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

ABIArgGenerator::ABIArgGenerator()
  : usedArgSlots_(0),
    firstArgFloat(false),
    current_()
{}

ABIArg
ABIArgGenerator::next(MIRType type)
{
    switch (type) {
      case MIRType::Int32:
      case MIRType::Int64:
      case MIRType::Pointer: {
        Register destReg;
        if (GetIntArgReg(usedArgSlots_, &destReg))
            current_ = ABIArg(destReg);
        else
            current_ = ABIArg(GetArgStackDisp(usedArgSlots_));
        usedArgSlots_++;
        break;
      }
      case MIRType::Float32:
      case MIRType::Double: {
        FloatRegister destFReg;
        FloatRegister::ContentType contentType;
        if (!usedArgSlots_)
            firstArgFloat = true;
        contentType = (type == MIRType::Double) ?
            FloatRegisters::Double : FloatRegisters::Single;
        if (GetFloatArgReg(usedArgSlots_, &destFReg))
            current_ = ABIArg(FloatRegister(destFReg.id(), contentType));
        else
            current_ = ABIArg(GetArgStackDisp(usedArgSlots_));
        usedArgSlots_++;
        break;
      }
      default:
        MOZ_CRASH("Unexpected argument type");
    }
    return current_;
}

uint32_t
js::jit::RT(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::TotalPhys);
    return r.id() << RTShift;
}

uint32_t
js::jit::RD(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::TotalPhys);
    return r.id() << RDShift;
}

uint32_t
js::jit::RZ(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::TotalPhys);
    return r.id() << RZShift;
}

uint32_t
js::jit::SA(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::TotalPhys);
    return r.id() << SAShift;
}

// Used to patch jumps created by MacroAssemblerMIPS64Compat::jumpWithPatch.
void
jit::PatchJump(CodeLocationJump& jump_, CodeLocationLabel label, ReprotectCode reprotect)
{
    Instruction* inst = (Instruction*)jump_.raw();

    // Six instructions used in load 64-bit imm.
    MaybeAutoWritableJitCode awjc(inst, 6 * sizeof(uint32_t), reprotect);
    Assembler::UpdateLoad64Value(inst, (uint64_t)label.raw());

    AutoFlushICache::flush(uintptr_t(inst), 6 * sizeof(uint32_t));
}

// For more infromation about backedges look at comment in
// MacroAssemblerMIPS64Compat::backedgeJump()
void
jit::PatchBackedge(CodeLocationJump& jump, CodeLocationLabel label,
                   JitZoneGroup::BackedgeTarget target)
{
    uintptr_t sourceAddr = (uintptr_t)jump.raw();
    uintptr_t targetAddr = (uintptr_t)label.raw();
    InstImm* branch = (InstImm*)jump.raw();

    MOZ_ASSERT(branch->extractOpcode() == (uint32_t(op_beq) >> OpcodeShift));

    if (BOffImm16::IsInRange(targetAddr - sourceAddr)) {
        branch->setBOffImm16(BOffImm16(targetAddr - sourceAddr));
    } else {
        if (target == JitZoneGroup::BackedgeLoopHeader) {
            Instruction* inst = &branch[1];
            Assembler::UpdateLoad64Value(inst, targetAddr);
            // Jump to first ori. The lui will be executed in delay slot.
            branch->setBOffImm16(BOffImm16(2 * sizeof(uint32_t)));
        } else {
            Instruction* inst = &branch[6];
            Assembler::UpdateLoad64Value(inst, targetAddr);
            // Jump to first ori of interrupt loop.
            branch->setBOffImm16(BOffImm16(6 * sizeof(uint32_t)));
        }
    }
}

void
Assembler::executableCopy(uint8_t* buffer, bool flushICache)
{
    MOZ_ASSERT(isFinished);
    m_buffer.executableCopy(buffer);

    if (flushICache)
        AutoFlushICache::setRange(uintptr_t(buffer), m_buffer.size());
}

uintptr_t
Assembler::GetPointer(uint8_t* instPtr)
{
    Instruction* inst = (Instruction*)instPtr;
    return Assembler::ExtractLoad64Value(inst);
}

static JitCode *
CodeFromJump(Instruction* jump)
{
    uint8_t* target = (uint8_t*)Assembler::ExtractLoad64Value(jump);
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
    void* ptr = (void*)Assembler::ExtractLoad64Value(inst);
    void* prior = ptr;

    // All pointers on MIPS64 will have the top bits cleared. If those bits
    // are not cleared, this must be a Value.
    uintptr_t word = reinterpret_cast<uintptr_t>(ptr);
    if (word >> JSVAL_TAG_SHIFT) {
        Value v = Value::fromRawBits(word);
        TraceManuallyBarrieredEdge(trc, &v, "ion-masm-value");
        ptr = (void*)v.bitsAsPunboxPointer();
    } else {
        // No barrier needed since these are constants.
        TraceManuallyBarrieredGenericPointerEdge(trc, reinterpret_cast<gc::Cell**>(&ptr),
                                                     "ion-masm-ptr");
    }

    if (ptr != prior) {
        Assembler::UpdateLoad64Value(inst, uint64_t(ptr));
        AutoFlushICache::flush(uintptr_t(inst), 6 * sizeof(uint32_t));
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
Assembler::Bind(uint8_t* rawCode, const CodeLabel& label)
{
    if (label.patchAt().bound()) {

        auto mode = label.linkMode();
        intptr_t offset = label.patchAt().offset();
        intptr_t target = label.target().offset();

        if (mode == CodeLabel::RawPointer) {
            *reinterpret_cast<const void**>(rawCode + offset) = rawCode + target;
        } else {
            MOZ_ASSERT(mode == CodeLabel::MoveImmediate || mode == CodeLabel::JumpImmediate);
            Instruction* inst = (Instruction*) (rawCode + offset);
            Assembler::UpdateLoad64Value(inst, (uint64_t)(rawCode + target));
        }
    }
}

void
Assembler::bind(InstImm* inst, uintptr_t branch, uintptr_t target)
{
    int64_t offset = target - branch;
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
        addLongJump(BufferOffset(branch), BufferOffset(target));
        Assembler::WriteLoad64Instructions(inst, ScratchRegister, LabelBase::INVALID_OFFSET);
        inst[4] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr).encode();
        // There is 1 nop after this.
        return;
    }

    if (BOffImm16::IsInRange(offset)) {
        // Don't skip trailing nops can improve performance
        // on Loongson3 platform.
        bool skipNops = !isLoongson() && (inst[0].encode() != inst_bgezal.encode() &&
                                          inst[0].encode() != inst_beq.encode());

        inst[0].setBOffImm16(BOffImm16(offset));
        inst[1].makeNop();

        if (skipNops) {
            inst[2] = InstImm(op_regimm, zero, rt_bgez, BOffImm16(5 * sizeof(uint32_t))).encode();
            // There are 4 nops after this
        }
        return;
    }

    if (inst[0].encode() == inst_beq.encode()) {
        // Handle long unconditional jump.
        addLongJump(BufferOffset(branch), BufferOffset(target));
        Assembler::WriteLoad64Instructions(inst, ScratchRegister, LabelBase::INVALID_OFFSET);
        inst[4] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
        // There is 1 nop after this.
    } else {
        // Handle long conditional jump.
        inst[0] = invertBranch(inst[0], BOffImm16(7 * sizeof(uint32_t)));
        // No need for a "nop" here because we can clobber scratch.
        addLongJump(BufferOffset(branch + sizeof(uint32_t)), BufferOffset(target));
        Assembler::WriteLoad64Instructions(&inst[1], ScratchRegister, LabelBase::INVALID_OFFSET);
        inst[5] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
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
        InstImm* inst = (InstImm*)editSrc(b);
        InstImm inst_beq = InstImm(op_beq, zero, zero, BOffImm16(0));
        uint64_t offset = dest.getOffset() - label->offset();

        // If first instruction is lui, then this is a long jump.
        // If second instruction is lui, then this is a loop backedge.
        if (inst[0].extractOpcode() == (uint32_t(op_lui) >> OpcodeShift)) {
            // For unconditional long branches generated by ma_liPatchable,
            // such as under:
            //     jumpWithpatch
            addLongJump(BufferOffset(label->offset()), dest);
        } else if (inst[1].extractOpcode() == (uint32_t(op_lui) >> OpcodeShift) ||
                   BOffImm16::IsInRange(offset))
        {
            // Handle code produced by:
            //     backedgeJump
            //     branchWithCode
            MOZ_ASSERT(BOffImm16::IsInRange(offset));
            MOZ_ASSERT(inst[0].extractOpcode() == (uint32_t(op_beq) >> OpcodeShift) ||
                       inst[0].extractOpcode() == (uint32_t(op_bne) >> OpcodeShift) ||
                       inst[0].extractOpcode() == (uint32_t(op_blez) >> OpcodeShift) ||
                       inst[0].extractOpcode() == (uint32_t(op_bgtz) >> OpcodeShift) ||
                       (inst[0].extractOpcode() == (uint32_t(op_regimm) >> OpcodeShift) &&
                       inst[0].extractRT() == (uint32_t(rt_bltz) >> RTShift)));
            inst[0].setBOffImm16(BOffImm16(offset));
        } else if (inst[0].encode() == inst_beq.encode()) {
            // Handle open long unconditional jumps created by
            // MacroAssemblerMIPSShared::ma_b(..., wasm::Trap, ...).
            // We need to add it to long jumps array here.
            // See MacroAssemblerMIPS64::branchWithCode().
            MOZ_ASSERT(inst[1].encode() == NopInst);
            MOZ_ASSERT(inst[2].encode() == NopInst);
            MOZ_ASSERT(inst[3].encode() == NopInst);
            MOZ_ASSERT(inst[4].encode() == NopInst);
            MOZ_ASSERT(inst[5].encode() == NopInst);
            addLongJump(BufferOffset(label->offset()), dest);
            Assembler::WriteLoad64Instructions(inst, ScratchRegister, LabelBase::INVALID_OFFSET);
            inst[4] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
        } else {
            // Handle open long conditional jumps created by
            // MacroAssemblerMIPSShared::ma_b(..., wasm::Trap, ...).
            inst[0] = invertBranch(inst[0], BOffImm16(7 * sizeof(uint32_t)));
            // No need for a "nop" here because we can clobber scratch.
            // We need to add it to long jumps array here.
            // See MacroAssemblerMIPS64::branchWithCode().
            MOZ_ASSERT(inst[1].encode() == NopInst);
            MOZ_ASSERT(inst[2].encode() == NopInst);
            MOZ_ASSERT(inst[3].encode() == NopInst);
            MOZ_ASSERT(inst[4].encode() == NopInst);
            MOZ_ASSERT(inst[5].encode() == NopInst);
            MOZ_ASSERT(inst[6].encode() == NopInst);
            addLongJump(BufferOffset(label->offset() + sizeof(uint32_t)), dest);
            Assembler::WriteLoad64Instructions(&inst[1], ScratchRegister, LabelBase::INVALID_OFFSET);
            inst[5] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
        }
    }
    label->bind(dest.getOffset());
}

void
Assembler::processCodeLabels(uint8_t* rawCode)
{
    for (const CodeLabel& label : codeLabels_) {
        Bind(rawCode, label);
    }
}

uint32_t
Assembler::PatchWrite_NearCallSize()
{
    // Load an address needs 4 instructions, and a jump with a delay slot.
    return (4 + 2) * sizeof(uint32_t);
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
    Assembler::WriteLoad64Instructions(inst, ScratchRegister, (uint64_t)dest);
    inst[4] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
    inst[5] = InstNOP();

    // Ensure everyone sees the code that was just written into memory.
    AutoFlushICache::flush(uintptr_t(inst), PatchWrite_NearCallSize());
}

uint64_t
Assembler::ExtractLoad64Value(Instruction* inst0)
{
    InstImm* i0 = (InstImm*) inst0;
    InstImm* i1 = (InstImm*) i0->next();
    InstReg* i2 = (InstReg*) i1->next();
    InstImm* i3 = (InstImm*) i2->next();
    InstImm* i5 = (InstImm*) i3->next()->next();

    MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
    MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));
    MOZ_ASSERT(i3->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

    if ((i2->extractOpcode() == ((uint32_t)op_special >> OpcodeShift)) &&
        (i2->extractFunctionField() == ff_dsrl32))
    {
        uint64_t value = (uint64_t(i0->extractImm16Value()) << 32) |
                         (uint64_t(i1->extractImm16Value()) << 16) |
                         uint64_t(i3->extractImm16Value());
        return uint64_t((int64_t(value) <<16) >> 16);
    }

    MOZ_ASSERT(i5->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));
    uint64_t value = (uint64_t(i0->extractImm16Value()) << 48) |
                     (uint64_t(i1->extractImm16Value()) << 32) |
                     (uint64_t(i3->extractImm16Value()) << 16) |
                     uint64_t(i5->extractImm16Value());
    return value;
}

void
Assembler::UpdateLoad64Value(Instruction* inst0, uint64_t value)
{
    InstImm* i0 = (InstImm*) inst0;
    InstImm* i1 = (InstImm*) i0->next();
    InstReg* i2 = (InstReg*) i1->next();
    InstImm* i3 = (InstImm*) i2->next();
    InstImm* i5 = (InstImm*) i3->next()->next();

    MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
    MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));
    MOZ_ASSERT(i3->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

    if ((i2->extractOpcode() == ((uint32_t)op_special >> OpcodeShift)) &&
        (i2->extractFunctionField() == ff_dsrl32))
    {
        i0->setImm16(Imm16::Lower(Imm32(value >> 32)));
        i1->setImm16(Imm16::Upper(Imm32(value)));
        i3->setImm16(Imm16::Lower(Imm32(value)));
        return;
    }

    MOZ_ASSERT(i5->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

    i0->setImm16(Imm16::Upper(Imm32(value >> 32)));
    i1->setImm16(Imm16::Lower(Imm32(value >> 32)));
    i3->setImm16(Imm16::Upper(Imm32(value)));
    i5->setImm16(Imm16::Lower(Imm32(value)));
}

void
Assembler::WriteLoad64Instructions(Instruction* inst0, Register reg, uint64_t value)
{
    Instruction* inst1 = inst0->next();
    Instruction* inst2 = inst1->next();
    Instruction* inst3 = inst2->next();

    *inst0 = InstImm(op_lui, zero, reg, Imm16::Lower(Imm32(value >> 32)));
    *inst1 = InstImm(op_ori, reg, reg, Imm16::Upper(Imm32(value)));
    *inst2 = InstReg(op_special, rs_one, reg, reg, 48 - 32, ff_dsrl32);
    *inst3 = InstImm(op_ori, reg, reg, Imm16::Lower(Imm32(value)));
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
    DebugOnly<uint64_t> value = Assembler::ExtractLoad64Value(inst);
    MOZ_ASSERT(value == uint64_t(expectedValue.value));

    // Replace with new value
    Assembler::UpdateLoad64Value(inst, uint64_t(newValue.value));

    AutoFlushICache::flush(uintptr_t(inst), 6 * sizeof(uint32_t));
}

uint64_t
Assembler::ExtractInstructionImmediate(uint8_t* code)
{
    InstImm* inst = (InstImm*)code;
    return Assembler::ExtractLoad64Value(inst);
}

void
Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled)
{
    Instruction* inst = (Instruction*)inst_.raw();
    InstImm* i0 = (InstImm*) inst;
    InstImm* i1 = (InstImm*) i0->next();
    InstImm* i3 = (InstImm*) i1->next()->next();
    Instruction* i4 = (Instruction*) i3->next();

    MOZ_ASSERT(i0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
    MOZ_ASSERT(i1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));
    MOZ_ASSERT(i3->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

    if (enabled) {
        MOZ_ASSERT(i4->extractOpcode() != ((uint32_t)op_lui >> OpcodeShift));
        InstReg jalr = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
        *i4 = jalr;
    } else {
        InstNOP nop;
        *i4 = nop;
    }

    AutoFlushICache::flush(uintptr_t(i4), sizeof(uint32_t));
}
