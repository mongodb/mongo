/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips/Assembler-mips.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"
#include "jsutil.h"

#include "gc/Marking.h"
#include "jit/ExecutableAllocator.h"
#include "jit/JitCompartment.h"

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
    FloatRegister::RegType regType;
    switch (type) {
      case MIRType_Int32:
      case MIRType_Pointer:
        Register destReg;
        if (GetIntArgReg(usedArgSlots_, &destReg))
            current_ = ABIArg(destReg);
        else
            current_ = ABIArg(usedArgSlots_ * sizeof(intptr_t));
        usedArgSlots_++;
        break;
      case MIRType_Float32:
      case MIRType_Double:
        regType = (type == MIRType_Double ? FloatRegister::Double : FloatRegister::Single);
        if (!usedArgSlots_) {
            current_ = ABIArg(FloatRegister(FloatRegisters::f12, regType));
            usedArgSlots_ += 2;
            firstArgFloat = true;
        } else if (usedArgSlots_ <= 2) {
            // NOTE: We will use f14 always. This is not compatible with
            // system ABI. We will have to introduce some infrastructure
            // changes if we have to use system ABI here.
            current_ = ABIArg(FloatRegister(FloatRegisters::f14, regType));
            usedArgSlots_ = 4;
        } else {
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

// Encode a standard register when it is being used as rd, the rs, and
// an extra register(rt). These should never be called with an InvalidReg.
uint32_t
js::jit::RS(Register r)
{
    MOZ_ASSERT((r.code() & ~RegMask) == 0);
    return r.code() << RSShift;
}

uint32_t
js::jit::RT(Register r)
{
    MOZ_ASSERT((r.code() & ~RegMask) == 0);
    return r.code() << RTShift;
}

uint32_t
js::jit::RT(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::TotalSingle);
    return r.id() << RTShift;
}

uint32_t
js::jit::RD(Register r)
{
    MOZ_ASSERT((r.code() & ~RegMask) == 0);
    return r.code() << RDShift;
}

uint32_t
js::jit::RD(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::TotalSingle);
    return r.id() << RDShift;
}

uint32_t
js::jit::SA(uint32_t value)
{
    MOZ_ASSERT(value < 32);
    return value << SAShift;
}

uint32_t
js::jit::SA(FloatRegister r)
{
    MOZ_ASSERT(r.id() < FloatRegisters::TotalSingle);
    return r.id() << SAShift;
}

Register
js::jit::toRS(Instruction& i)
{
    return Register::FromCode((i.encode() & RSMask ) >> RSShift);
}

Register
js::jit::toRT(Instruction& i)
{
    return Register::FromCode((i.encode() & RTMask ) >> RTShift);
}

Register
js::jit::toRD(Instruction& i)
{
    return Register::FromCode((i.encode() & RDMask ) >> RDShift);
}

Register
js::jit::toR(Instruction& i)
{
    return Register::FromCode(i.encode() & RegMask);
}

void
InstImm::extractImm16(BOffImm16* dest)
{
    *dest = BOffImm16(*this);
}

// Used to patch jumps created by MacroAssemblerMIPSCompat::jumpWithPatch.
void
jit::PatchJump(CodeLocationJump& jump_, CodeLocationLabel label)
{
    Instruction* inst1 = (Instruction*)jump_.raw();
    Instruction* inst2 = inst1->next();

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
Assembler::finish()
{
    MOZ_ASSERT(!isFinished);
    isFinished = true;
}

void
Assembler::executableCopy(uint8_t* buffer)
{
    MOZ_ASSERT(isFinished);
    m_buffer.executableCopy(buffer);

    // Patch all long jumps during code copy.
    for (size_t i = 0; i < longJumps_.length(); i++) {
        Instruction* inst1 = (Instruction*) ((uint32_t)buffer + longJumps_[i]);

        uint32_t value = ExtractLuiOriValue(inst1, inst1->next());
        UpdateLuiOriValue(inst1, inst1->next(), (uint32_t)buffer + value);
    }

    AutoFlushICache::setRange(uintptr_t(buffer), m_buffer.size());
}

uint32_t
Assembler::actualOffset(uint32_t off_) const
{
    return off_;
}

uint32_t
Assembler::actualIndex(uint32_t idx_) const
{
    return idx_;
}

uint8_t*
Assembler::PatchableJumpAddress(JitCode* code, uint32_t pe_)
{
    return code->raw() + pe_;
}

class RelocationIterator
{
    CompactBufferReader reader_;
    // offset in bytes
    uint32_t offset_;

  public:
    RelocationIterator(CompactBufferReader& reader)
      : reader_(reader)
    { }

    bool read() {
        if (!reader_.more())
            return false;
        offset_ = reader_.readUnsigned();
        return true;
    }

    uint32_t offset() const {
        return offset_;
    }
};

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
    RelocationIterator iter(reader);
    while (iter.read()) {
        JitCode* child = CodeFromJump((Instruction*)(code->raw() + iter.offset()));
        MarkJitCodeUnbarriered(trc, &child, "rel32");
    }
}

static void
TraceOneDataRelocation(JSTracer* trc, Instruction* inst)
{
    void* ptr = (void*)Assembler::ExtractLuiOriValue(inst, inst->next());
    void* prior = ptr;

    // The low bit shouldn't be set. If it is, we probably got a dummy
    // pointer inserted by CodeGenerator::visitNurseryObject, but we
    // shouldn't be able to trigger GC before those are patched to their
    // real values.
    MOZ_ASSERT(!(uintptr_t(ptr) & 0x1));

    // No barrier needed since these are constants.
    gc::MarkGCThingUnbarriered(trc, reinterpret_cast<void**>(&ptr), "ion-masm-ptr");
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
Assembler::FixupNurseryObjects(JSContext* cx, JitCode* code, CompactBufferReader& reader,
                               const ObjectVector& nurseryObjects)
{
    MOZ_ASSERT(!nurseryObjects.empty());

    uint8_t* buffer = code->raw();
    bool hasNurseryPointers = false;

    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        Instruction* inst = (Instruction*)(buffer + offset);

        void* ptr = (void*)Assembler::ExtractLuiOriValue(inst, inst->next());
        uintptr_t word = uintptr_t(ptr);

        if (!(word & 0x1))
            continue;

        uint32_t index = word >> 1;
        JSObject* obj = nurseryObjects[index];

        Assembler::UpdateLuiOriValue(inst, inst->next(), uint32_t(obj));
        AutoFlushICache::flush(uintptr_t(inst), 8);

        // Either all objects are still in the nursery, or all objects are
        // tenured.
        MOZ_ASSERT_IF(hasNurseryPointers, IsInsideNursery(obj));

        if (!hasNurseryPointers && IsInsideNursery(obj))
            hasNurseryPointers = true;
    }

    if (hasNurseryPointers)
        cx->runtime()->gc.storeBuffer.putWholeCellFromMainThread(code);
}

void
Assembler::copyJumpRelocationTable(uint8_t* dest)
{
    if (jumpRelocations_.length())
        memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
}

void
Assembler::copyDataRelocationTable(uint8_t* dest)
{
    if (dataRelocations_.length())
        memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
}

void
Assembler::copyPreBarrierTable(uint8_t* dest)
{
    if (preBarriers_.length())
        memcpy(dest, preBarriers_.buffer(), preBarriers_.length());
}

void
Assembler::trace(JSTracer* trc)
{
    for (size_t i = 0; i < jumps_.length(); i++) {
        RelativePatch& rp = jumps_[i];
        if (rp.kind == Relocation::JITCODE) {
            JitCode* code = JitCode::FromExecutable((uint8_t*)rp.target);
            MarkJitCodeUnbarriered(trc, &code, "masmrel32");
            MOZ_ASSERT(code == JitCode::FromExecutable((uint8_t*)rp.target));
        }
    }
    if (dataRelocations_.length()) {
        CompactBufferReader reader(dataRelocations_);
        ::TraceDataRelocations(trc, &m_buffer, reader);
    }
}

void
Assembler::processCodeLabels(uint8_t* rawCode)
{
    for (size_t i = 0; i < codeLabels_.length(); i++) {
        CodeLabel label = codeLabels_[i];
        Bind(rawCode, label.dest(), rawCode + actualOffset(label.src()->offset()));
    }
}

int32_t
Assembler::ExtractCodeLabelOffset(uint8_t* code) {
    InstImm* inst = (InstImm*)code;
    return Assembler::ExtractLuiOriValue(inst, inst->next());
}

void
Assembler::Bind(uint8_t* rawCode, AbsoluteLabel* label, const void* address)
{
    if (label->used()) {
        int32_t src = label->offset();
        do {
            Instruction* inst = (Instruction*) (rawCode + src);
            uint32_t next = Assembler::ExtractLuiOriValue(inst, inst->next());
            Assembler::UpdateLuiOriValue(inst, inst->next(), (uint32_t)address);
            src = next;
        } while (src != AbsoluteLabel::INVALID_OFFSET);
    }
    label->bind();
}

Assembler::Condition
Assembler::InvertCondition(Condition cond)
{
    switch (cond) {
      case Equal:
        return NotEqual;
      case NotEqual:
        return Equal;
      case Zero:
        return NonZero;
      case NonZero:
        return Zero;
      case LessThan:
        return GreaterThanOrEqual;
      case LessThanOrEqual:
        return GreaterThan;
      case GreaterThan:
        return LessThanOrEqual;
      case GreaterThanOrEqual:
        return LessThan;
      case Above:
        return BelowOrEqual;
      case AboveOrEqual:
        return Below;
      case Below:
        return AboveOrEqual;
      case BelowOrEqual:
        return Above;
      case Signed:
        return NotSigned;
      case NotSigned:
        return Signed;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

Assembler::DoubleCondition
Assembler::InvertCondition(DoubleCondition cond)
{
    switch (cond) {
      case DoubleOrdered:
        return DoubleUnordered;
      case DoubleEqual:
        return DoubleNotEqualOrUnordered;
      case DoubleNotEqual:
        return DoubleEqualOrUnordered;
      case DoubleGreaterThan:
        return DoubleLessThanOrEqualOrUnordered;
      case DoubleGreaterThanOrEqual:
        return DoubleLessThanOrUnordered;
      case DoubleLessThan:
        return DoubleGreaterThanOrEqualOrUnordered;
      case DoubleLessThanOrEqual:
        return DoubleGreaterThanOrUnordered;
      case DoubleUnordered:
        return DoubleOrdered;
      case DoubleEqualOrUnordered:
        return DoubleNotEqual;
      case DoubleNotEqualOrUnordered:
        return DoubleEqual;
      case DoubleGreaterThanOrUnordered:
        return DoubleLessThanOrEqual;
      case DoubleGreaterThanOrEqualOrUnordered:
        return DoubleLessThan;
      case DoubleLessThanOrUnordered:
        return DoubleGreaterThanOrEqual;
      case DoubleLessThanOrEqualOrUnordered:
        return DoubleGreaterThan;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

BOffImm16::BOffImm16(InstImm inst)
  : data(inst.encode() & Imm16Mask)
{
}

bool
Assembler::oom() const
{
    return AssemblerShared::oom() ||
           m_buffer.oom() ||
           jumpRelocations_.oom() ||
           dataRelocations_.oom() ||
           preBarriers_.oom();
}

void
Assembler::addCodeLabel(CodeLabel label)
{
    propagateOOM(codeLabels_.append(label));
}

// Size of the instruction stream, in bytes.
size_t
Assembler::size() const
{
    return m_buffer.size();
}

// Size of the relocation table, in bytes.
size_t
Assembler::jumpRelocationTableBytes() const
{
    return jumpRelocations_.length();
}

size_t
Assembler::dataRelocationTableBytes() const
{
    return dataRelocations_.length();
}

size_t
Assembler::preBarrierTableBytes() const
{
    return preBarriers_.length();
}

// Size of the data table, in bytes.
size_t
Assembler::bytesNeeded() const
{
    return size() +
           jumpRelocationTableBytes() +
           dataRelocationTableBytes() +
           preBarrierTableBytes();
}

// write a blob of binary into the instruction stream
BufferOffset
Assembler::writeInst(uint32_t x, uint32_t* dest)
{
    if (dest == nullptr)
        return m_buffer.putInt(x);

    WriteInstStatic(x, dest);
    return BufferOffset();
}

void
Assembler::WriteInstStatic(uint32_t x, uint32_t* dest)
{
    MOZ_ASSERT(dest != nullptr);
    *dest = x;
}

BufferOffset
Assembler::align(int alignment)
{
    BufferOffset ret;
    MOZ_ASSERT(m_buffer.isAligned(4));
    if (alignment == 8) {
        if (!m_buffer.isAligned(alignment)) {
            BufferOffset tmp = as_nop();
            if (!ret.assigned())
                ret = tmp;
        }
    } else {
        MOZ_ASSERT((alignment & (alignment - 1)) == 0);
        while (size() & (alignment - 1)) {
            BufferOffset tmp = as_nop();
            if (!ret.assigned())
                ret = tmp;
        }
    }
    return ret;
}

BufferOffset
Assembler::as_nop()
{
    return writeInst(op_special | ff_sll);
}

// Logical operations.
BufferOffset
Assembler::as_and(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_and).encode());
}

BufferOffset
Assembler::as_or(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_or).encode());
}

BufferOffset
Assembler::as_xor(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_xor).encode());
}

BufferOffset
Assembler::as_nor(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_nor).encode());
}

BufferOffset
Assembler::as_andi(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_andi, rs, rd, Imm16(j)).encode());
}

BufferOffset
Assembler::as_ori(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_ori, rs, rd, Imm16(j)).encode());
}

BufferOffset
Assembler::as_xori(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_xori, rs, rd, Imm16(j)).encode());
}

// Branch and jump instructions
BufferOffset
Assembler::as_bal(BOffImm16 off)
{
    BufferOffset bo = writeInst(InstImm(op_regimm, zero, rt_bgezal, off).encode());
    return bo;
}

BufferOffset
Assembler::as_b(BOffImm16 off)
{
    BufferOffset bo = writeInst(InstImm(op_beq, zero, zero, off).encode());
    return bo;
}

InstImm
Assembler::getBranchCode(JumpOrCall jumpOrCall)
{
    if (jumpOrCall == BranchIsCall)
        return InstImm(op_regimm, zero, rt_bgezal, BOffImm16(0));

    return InstImm(op_beq, zero, zero, BOffImm16(0));
}

InstImm
Assembler::getBranchCode(Register s, Register t, Condition c)
{
    MOZ_ASSERT(c == Assembler::Equal || c == Assembler::NotEqual);
    return InstImm(c == Assembler::Equal ? op_beq : op_bne, s, t, BOffImm16(0));
}

InstImm
Assembler::getBranchCode(Register s, Condition c)
{
    switch (c) {
      case Assembler::Equal:
      case Assembler::Zero:
      case Assembler::BelowOrEqual:
        return InstImm(op_beq, s, zero, BOffImm16(0));
      case Assembler::NotEqual:
      case Assembler::NonZero:
      case Assembler::Above:
        return InstImm(op_bne, s, zero, BOffImm16(0));
      case Assembler::GreaterThan:
        return InstImm(op_bgtz, s, zero, BOffImm16(0));
      case Assembler::GreaterThanOrEqual:
      case Assembler::NotSigned:
        return InstImm(op_regimm, s, rt_bgez, BOffImm16(0));
      case Assembler::LessThan:
      case Assembler::Signed:
        return InstImm(op_regimm, s, rt_bltz, BOffImm16(0));
      case Assembler::LessThanOrEqual:
        return InstImm(op_blez, s, zero, BOffImm16(0));
      default:
        MOZ_CRASH("Condition not supported.");
    }
}

InstImm
Assembler::getBranchCode(FloatTestKind testKind, FPConditionBit fcc)
{
    MOZ_ASSERT(!(fcc && FccMask));
    uint32_t rtField = ((testKind == TestForTrue ? 1 : 0) | (fcc << FccShift)) << RTShift;

    return InstImm(op_cop1, rs_bc1, rtField, BOffImm16(0));
}

BufferOffset
Assembler::as_j(JOffImm26 off)
{
    BufferOffset bo = writeInst(InstJump(op_j, off).encode());
    return bo;
}
BufferOffset
Assembler::as_jal(JOffImm26 off)
{
    BufferOffset bo = writeInst(InstJump(op_jal, off).encode());
    return bo;
}

BufferOffset
Assembler::as_jr(Register rs)
{
    BufferOffset bo = writeInst(InstReg(op_special, rs, zero, zero, ff_jr).encode());
    return bo;
}
BufferOffset
Assembler::as_jalr(Register rs)
{
    BufferOffset bo = writeInst(InstReg(op_special, rs, zero, ra, ff_jalr).encode());
    return bo;
}


// Arithmetic instructions
BufferOffset
Assembler::as_addu(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_addu).encode());
}

BufferOffset
Assembler::as_addiu(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(j));
    return writeInst(InstImm(op_addiu, rs, rd, Imm16(j)).encode());
}

BufferOffset
Assembler::as_subu(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_subu).encode());
}

BufferOffset
Assembler::as_mult(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_mult).encode());
}

BufferOffset
Assembler::as_multu(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_multu).encode());
}

BufferOffset
Assembler::as_div(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_div).encode());
}

BufferOffset
Assembler::as_divu(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_divu).encode());
}

BufferOffset
Assembler::as_mul(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special2, rs, rt, rd, ff_mul).encode());
}

BufferOffset
Assembler::as_lui(Register rd, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_lui, zero, rd, Imm16(j)).encode());
}

// Shift instructions
BufferOffset
Assembler::as_sll(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_sll).encode());
}

BufferOffset
Assembler::as_sllv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_sllv).encode());
}

BufferOffset
Assembler::as_srl(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_srl).encode());
}

BufferOffset
Assembler::as_srlv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_srlv).encode());
}

BufferOffset
Assembler::as_sra(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_sra).encode());
}

BufferOffset
Assembler::as_srav(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_srav).encode());
}

BufferOffset
Assembler::as_rotr(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_one, rt, rd, sa, ff_srl).encode());
}

BufferOffset
Assembler::as_rotrv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, 1, ff_srlv).encode());
}

// Load and store instructions
BufferOffset
Assembler::as_lb(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lb, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_lbu(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lbu, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_lh(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lh, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_lhu(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lhu, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_lw(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lw, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_lwl(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lwl, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_lwr(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lwr, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_sb(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sb, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_sh(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sh, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_sw(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sw, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_swl(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_swl, rs, rd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_swr(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_swr, rs, rd, Imm16(off)).encode());
}

// Move from HI/LO register.
BufferOffset
Assembler::as_mfhi(Register rd)
{
    return writeInst(InstReg(op_special, rd, ff_mfhi).encode());
}

BufferOffset
Assembler::as_mflo(Register rd)
{
    return writeInst(InstReg(op_special, rd, ff_mflo).encode());
}

// Set on less than.
BufferOffset
Assembler::as_slt(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_slt).encode());
}

BufferOffset
Assembler::as_sltu(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_sltu).encode());
}

BufferOffset
Assembler::as_slti(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(j));
    return writeInst(InstImm(op_slti, rs, rd, Imm16(j)).encode());
}

BufferOffset
Assembler::as_sltiu(Register rd, Register rs, uint32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_sltiu, rs, rd, Imm16(j)).encode());
}

// Conditional move.
BufferOffset
Assembler::as_movz(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_movz).encode());
}

BufferOffset
Assembler::as_movn(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_movn).encode());
}

BufferOffset
Assembler::as_movt(Register rd, Register rs, uint16_t cc)
{
    Register rt;
    rt = Register::FromCode((cc & 0x7) << 2 | 1);
    return writeInst(InstReg(op_special, rs, rt, rd, ff_movci).encode());
}

BufferOffset
Assembler::as_movf(Register rd, Register rs, uint16_t cc)
{
    Register rt;
    rt = Register::FromCode((cc & 0x7) << 2 | 0);
    return writeInst(InstReg(op_special, rs, rt, rd, ff_movci).encode());
}

// Bit twiddling.
BufferOffset
Assembler::as_clz(Register rd, Register rs)
{
    return writeInst(InstReg(op_special2, rs, rd, rd, ff_clz).encode());
}

BufferOffset
Assembler::as_ins(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 && pos + size <= 32);
    Register rd;
    rd = Register::FromCode(pos + size - 1);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_ins).encode());
}

BufferOffset
Assembler::as_ext(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 && pos + size <= 32);
    Register rd;
    rd = Register::FromCode(size - 1);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_ext).encode());
}

// FP instructions
BufferOffset
Assembler::as_ld(FloatRegister fd, Register base, int32_t off)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(off));
    return writeInst(InstImm(op_ldc1, base, fd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_sd(FloatRegister fd, Register base, int32_t off)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(off));
    return writeInst(InstImm(op_sdc1, base, fd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_ls(FloatRegister fd, Register base, int32_t off)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(off));
    return writeInst(InstImm(op_lwc1, base, fd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_ss(FloatRegister fd, Register base, int32_t off)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(off));
    return writeInst(InstImm(op_swc1, base, fd, Imm16(off)).encode());
}

BufferOffset
Assembler::as_movs(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_mov_fmt).encode());
}

BufferOffset
Assembler::as_movd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_mov_fmt).encode());
}

BufferOffset
Assembler::as_mtc1(Register rt, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_mtc1, rt, fs).encode());
}

BufferOffset
Assembler::as_mfc1(Register rt, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_mfc1, rt, fs).encode());
}

// FP convert instructions
BufferOffset
Assembler::as_ceilws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_ceil_w_fmt).encode());
}

BufferOffset
Assembler::as_floorws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_floor_w_fmt).encode());
}

BufferOffset
Assembler::as_roundws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_round_w_fmt).encode());
}

BufferOffset
Assembler::as_truncws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_trunc_w_fmt).encode());
}

BufferOffset
Assembler::as_ceilwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_ceil_w_fmt).encode());
}

BufferOffset
Assembler::as_floorwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_floor_w_fmt).encode());
}

BufferOffset
Assembler::as_roundwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_round_w_fmt).encode());
}

BufferOffset
Assembler::as_truncwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_trunc_w_fmt).encode());
}

BufferOffset
Assembler::as_cvtds(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_cvt_d_fmt).encode());
}

BufferOffset
Assembler::as_cvtdw(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_w, zero, fs, fd, ff_cvt_d_fmt).encode());
}

BufferOffset
Assembler::as_cvtsd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_cvt_s_fmt).encode());
}

BufferOffset
Assembler::as_cvtsw(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_w, zero, fs, fd, ff_cvt_s_fmt).encode());
}

BufferOffset
Assembler::as_cvtwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_cvt_w_fmt).encode());
}

BufferOffset
Assembler::as_cvtws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_cvt_w_fmt).encode());
}

// FP arithmetic instructions
BufferOffset
Assembler::as_adds(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_add_fmt).encode());
}

BufferOffset
Assembler::as_addd(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_add_fmt).encode());
}

BufferOffset
Assembler::as_subs(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_sub_fmt).encode());
}

BufferOffset
Assembler::as_subd(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_sub_fmt).encode());
}

BufferOffset
Assembler::as_abss(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_abs_fmt).encode());
}

BufferOffset
Assembler::as_absd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_abs_fmt).encode());
}

BufferOffset
Assembler::as_negs(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_neg_fmt).encode());
}

BufferOffset
Assembler::as_negd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_neg_fmt).encode());
}

BufferOffset
Assembler::as_muls(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_mul_fmt).encode());
}

BufferOffset
Assembler::as_muld(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_mul_fmt).encode());
}

BufferOffset
Assembler::as_divs(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_div_fmt).encode());
}

BufferOffset
Assembler::as_divd(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_div_fmt).encode());
}

BufferOffset
Assembler::as_sqrts(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_sqrt_fmt).encode());
}

BufferOffset
Assembler::as_sqrtd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_sqrt_fmt).encode());
}

// FP compare instructions
BufferOffset
Assembler::as_cf(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_f_fmt).encode());
}

BufferOffset
Assembler::as_cun(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_un_fmt).encode());
}

BufferOffset
Assembler::as_ceq(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_eq_fmt).encode());
}

BufferOffset
Assembler::as_cueq(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_ueq_fmt).encode());
}

BufferOffset
Assembler::as_colt(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_olt_fmt).encode());
}

BufferOffset
Assembler::as_cult(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_ult_fmt).encode());
}

BufferOffset
Assembler::as_cole(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_ole_fmt).encode());
}

BufferOffset
Assembler::as_cule(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_ule_fmt).encode());
}


void
Assembler::bind(Label* label, BufferOffset boff)
{
    // If our caller didn't give us an explicit target to bind to
    // then we want to bind to the location of the next instruction
    BufferOffset dest = boff.assigned() ? boff : nextOffset();
    if (label->used()) {
        int32_t next;

        // A used label holds a link to branch that uses it.
        BufferOffset b(label);
        do {
            Instruction* inst = editSrc(b);

            // Second word holds a pointer to the next branch in label's chain.
            next = inst[1].encode();
            bind(reinterpret_cast<InstImm*>(inst), b.getOffset(), dest.getOffset());

            b = BufferOffset(next);
        } while (next != LabelBase::INVALID_OFFSET);
    }
    label->bind(dest.getOffset());
}

void
Assembler::bind(InstImm* inst, uint32_t branch, uint32_t target)
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
        WriteLuiOriInstructions(inst, &inst[1], ScratchRegister, target);
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
        WriteLuiOriInstructions(inst, &inst[1], ScratchRegister, target);
        inst[2] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
        // There is 1 nop after this.
    } else {
        // Handle long conditional jump.
        inst[0] = invertBranch(inst[0], BOffImm16(5 * sizeof(void*)));
        // No need for a "nop" here because we can clobber scratch.
        addLongJump(BufferOffset(branch + sizeof(void*)));
        WriteLuiOriInstructions(&inst[1], &inst[2], ScratchRegister, target);
        inst[3] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr).encode();
        // There is 1 nop after this.
    }
}

void
Assembler::bind(RepatchLabel* label)
{
    BufferOffset dest = nextOffset();
    if (label->used()) {
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
            UpdateLuiOriValue(inst1, inst1->next(), dest.getOffset());
        }

    }
    label->bind(dest.getOffset());
}

void
Assembler::retarget(Label* label, Label* target)
{
    if (label->used()) {
        if (target->bound()) {
            bind(label, BufferOffset(target));
        } else if (target->used()) {
            // The target is not bound but used. Prepend label's branch list
            // onto target's.
            int32_t next;
            BufferOffset labelBranchOffset(label);

            // Find the head of the use chain for label.
            do {
                Instruction* inst = editSrc(labelBranchOffset);

                // Second word holds a pointer to the next branch in chain.
                next = inst[1].encode();
                labelBranchOffset = BufferOffset(next);
            } while (next != LabelBase::INVALID_OFFSET);

            // Then patch the head of label's use chain to the tail of
            // target's use chain, prepending the entire use chain of target.
            Instruction* inst = editSrc(labelBranchOffset);
            int32_t prev = target->use(label->offset());
            inst[1].setData(prev);
        } else {
            // The target is unbound and unused.  We can just take the head of
            // the list hanging off of label, and dump that into target.
            DebugOnly<uint32_t> prev = target->use(label->offset());
            MOZ_ASSERT((int32_t)prev == Label::INVALID_OFFSET);
        }
    }
    label->reset();
}

void dbg_break() {}
void
Assembler::as_break(uint32_t code)
{
    MOZ_ASSERT(code <= MAX_BREAK_CODE);
    writeInst(op_special | code << RTShift | ff_break);
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
    WriteLuiOriInstructions(inst, &inst[1], ScratchRegister, (uint32_t)dest);
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
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue, ImmPtr expectedValue)
{
    PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value),
                            PatchedImmPtr(expectedValue.value));
}

// This just stomps over memory with 32 bits of raw data. Its purpose is to
// overwrite the call of JITed code with 32 bits worth of an offset. This will
// is only meant to function on code that has been invalidated, so it should
// be totally safe. Since that instruction will never be executed again, a
// ICache flush should not be necessary
void
Assembler::PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm)
{
    // Raw is going to be the return address.
    uint32_t* raw = (uint32_t*)label.raw();
    // Overwrite the 4 bytes before the return address, which will
    // end up being the call instruction.
    *(raw - 1) = imm.value;
}

void
Assembler::PatchInstructionImmediate(uint8_t* code, PatchedImmPtr imm)
{
    InstImm* inst = (InstImm*)code;
    Assembler::UpdateLuiOriValue(inst, inst->next(), (uint32_t)imm.value);
}

uint8_t*
Assembler::NextInstruction(uint8_t* inst_, uint32_t* count)
{
    Instruction* inst = reinterpret_cast<Instruction*>(inst_);
    if (count != nullptr)
        *count += sizeof(Instruction);
    return reinterpret_cast<uint8_t*>(inst->next());
}

// Since there are no pools in MIPS implementation, this should be simple.
Instruction*
Instruction::next()
{
    return this + 1;
}

InstImm Assembler::invertBranch(InstImm branch, BOffImm16 skipOffset)
{
    uint32_t rt = 0;
    Opcode op = (Opcode) (branch.extractOpcode() << OpcodeShift);
    switch(op) {
      case op_beq:
        branch.setBOffImm16(skipOffset);
        branch.setOpcode(op_bne);
        return branch;
      case op_bne:
        branch.setBOffImm16(skipOffset);
        branch.setOpcode(op_beq);
        return branch;
      case op_bgtz:
        branch.setBOffImm16(skipOffset);
        branch.setOpcode(op_blez);
        return branch;
      case op_blez:
        branch.setBOffImm16(skipOffset);
        branch.setOpcode(op_bgtz);
        return branch;
      case op_regimm:
        branch.setBOffImm16(skipOffset);
        rt = branch.extractRT();
        if (rt == (rt_bltz >> RTShift)) {
            branch.setRT(rt_bgez);
            return branch;
        }
        if (rt == (rt_bgez >> RTShift)) {
            branch.setRT(rt_bltz);
            return branch;
        }

        MOZ_CRASH("Error creating long branch.");

      case op_cop1:
        MOZ_ASSERT(branch.extractRS() == rs_bc1 >> RSShift);

        branch.setBOffImm16(skipOffset);
        rt = branch.extractRT();
        if (rt & 0x1)
            branch.setRT((RTField) ((rt & ~0x1) << RTShift));
        else
            branch.setRT((RTField) ((rt | 0x1) << RTShift));
        return branch;
      default:
        MOZ_CRASH("Error creating long branch.");
    }
}

void
Assembler::ToggleToJmp(CodeLocationLabel inst_)
{
    InstImm * inst = (InstImm*)inst_.raw();

    MOZ_ASSERT(inst->extractOpcode() == ((uint32_t)op_andi >> OpcodeShift));
    // We converted beq to andi, so now we restore it.
    inst->setOpcode(op_beq);

    AutoFlushICache::flush(uintptr_t(inst), 4);
}

void
Assembler::ToggleToCmp(CodeLocationLabel inst_)
{
    InstImm * inst = (InstImm*)inst_.raw();

    // toggledJump is allways used for short jumps.
    MOZ_ASSERT(inst->extractOpcode() == ((uint32_t)op_beq >> OpcodeShift));
    // Replace "beq $zero, $zero, offset" with "andi $zero, $zero, offset"
    inst->setOpcode(op_andi);

    AutoFlushICache::flush(uintptr_t(inst), 4);
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

void Assembler::UpdateBoundsCheck(uint32_t heapSize, Instruction* inst)
{
    InstImm* i0 = (InstImm*) inst;
    InstImm* i1 = (InstImm*) i0->next();

    // Replace with new value
    Assembler::UpdateLuiOriValue(i0, i1, heapSize);
}
