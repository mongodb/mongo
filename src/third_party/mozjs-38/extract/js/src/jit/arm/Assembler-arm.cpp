/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/Assembler-arm.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"
#include "jsutil.h"

#include "gc/Marking.h"
#include "jit/arm/MacroAssembler-arm.h"
#include "jit/ExecutableAllocator.h"
#include "jit/JitCompartment.h"

using namespace js;
using namespace js::jit;

using mozilla::CountLeadingZeroes32;

void dbg_break() {}

// Note this is used for inter-AsmJS calls and may pass arguments and results in
// floating point registers even if the system ABI does not.
ABIArgGenerator::ABIArgGenerator() :
    intRegIndex_(0),
    floatRegIndex_(0),
    stackOffset_(0),
    current_()
{}

ABIArg
ABIArgGenerator::next(MIRType type)
{
    switch (type) {
      case MIRType_Int32:
      case MIRType_Pointer:
        if (intRegIndex_ == NumIntArgRegs) {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint32_t);
            break;
        }
        current_ = ABIArg(Register::FromCode(intRegIndex_));
        intRegIndex_++;
        break;
      case MIRType_Float32:
        if (floatRegIndex_ == NumFloatArgRegs) {
            static const int align = sizeof(double) - 1;
            stackOffset_ = (stackOffset_ + align) & ~align;
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint64_t);
            break;
        }
        current_ = ABIArg(VFPRegister(floatRegIndex_, VFPRegister::Single));
        floatRegIndex_++;
        break;
      case MIRType_Double:
        // Bump the number of used registers up to the next multiple of two.
        floatRegIndex_ = (floatRegIndex_ + 1) & ~1;
        if (floatRegIndex_ == NumFloatArgRegs) {
            static const int align = sizeof(double) - 1;
            stackOffset_ = (stackOffset_ + align) & ~align;
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint64_t);
            break;
        }
        current_ = ABIArg(VFPRegister(floatRegIndex_ >> 1, VFPRegister::Double));
        floatRegIndex_+=2;
        break;
      default:
        MOZ_CRASH("Unexpected argument type");
    }

    return current_;
}

const Register ABIArgGenerator::NonArgReturnReg0 = r4;
const Register ABIArgGenerator::NonArgReturnReg1 = r5;
const Register ABIArgGenerator::NonReturn_VolatileReg0 = r2;
const Register ABIArgGenerator::NonReturn_VolatileReg1 = r3;

// Encode a standard register when it is being used as src1, the dest, and an
// extra register. These should never be called with an InvalidReg.
uint32_t
js::jit::RT(Register r)
{
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 12;
}

uint32_t
js::jit::RN(Register r)
{
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 16;
}

uint32_t
js::jit::RD(Register r)
{
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 12;
}

uint32_t
js::jit::RM(Register r)
{
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 8;
}

// Encode a standard register when it is being used as src1, the dest, and an
// extra register. For these, an InvalidReg is used to indicate a optional
// register that has been omitted.
uint32_t
js::jit::maybeRT(Register r)
{
    if (r == InvalidReg)
        return 0;

    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 12;
}

uint32_t
js::jit::maybeRN(Register r)
{
    if (r == InvalidReg)
        return 0;

    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 16;
}

uint32_t
js::jit::maybeRD(Register r)
{
    if (r == InvalidReg)
        return 0;

    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 12;
}

Register
js::jit::toRD(Instruction& i)
{
    return Register::FromCode((i.encode() >> 12) & 0xf);
}
Register
js::jit::toR(Instruction& i)
{
    return Register::FromCode(i.encode() & 0xf);
}

Register
js::jit::toRM(Instruction& i)
{
    return Register::FromCode((i.encode() >> 8) & 0xf);
}

Register
js::jit::toRN(Instruction& i)
{
    return Register::FromCode((i.encode() >> 16) & 0xf);
}

uint32_t
js::jit::VD(VFPRegister vr)
{
    if (vr.isMissing())
        return 0;

    // Bits 15,14,13,12, 22.
    VFPRegister::VFPRegIndexSplit s = vr.encode();
    return s.bit << 22 | s.block << 12;
}
uint32_t
js::jit::VN(VFPRegister vr)
{
    if (vr.isMissing())
        return 0;

    // Bits 19,18,17,16, 7.
    VFPRegister::VFPRegIndexSplit s = vr.encode();
    return s.bit << 7 | s.block << 16;
}
uint32_t
js::jit::VM(VFPRegister vr)
{
    if (vr.isMissing())
        return 0;

    // Bits 5, 3,2,1,0.
    VFPRegister::VFPRegIndexSplit s = vr.encode();
    return s.bit << 5 | s.block;
}

VFPRegister::VFPRegIndexSplit
jit::VFPRegister::encode()
{
    MOZ_ASSERT(!_isInvalid);

    switch (kind) {
      case Double:
        return VFPRegIndexSplit(code_ & 0xf, code_ >> 4);
      case Single:
        return VFPRegIndexSplit(code_ >> 1, code_ & 1);
      default:
        // VFP register treated as an integer, NOT a gpr.
        return VFPRegIndexSplit(code_ >> 1, code_ & 1);
    }
}

bool
InstDTR::IsTHIS(const Instruction& i)
{
    return (i.encode() & IsDTRMask) == (uint32_t)IsDTR;
}

InstDTR*
InstDTR::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstDTR*)&i;
    return nullptr;
}

bool
InstLDR::IsTHIS(const Instruction& i)
{
    return (i.encode() & IsDTRMask) == (uint32_t)IsDTR;
}

InstLDR*
InstLDR::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstLDR*)&i;
    return nullptr;
}

InstNOP*
InstNOP::AsTHIS(Instruction& i)
{
    if (IsTHIS(i))
        return (InstNOP*)&i;
    return nullptr;
}

bool
InstNOP::IsTHIS(const Instruction& i)
{
    return (i.encode() & 0x0fffffff) == NopInst;
}

bool
InstBranchReg::IsTHIS(const Instruction& i)
{
    return InstBXReg::IsTHIS(i) || InstBLXReg::IsTHIS(i);
}

InstBranchReg*
InstBranchReg::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBranchReg*)&i;
    return nullptr;
}
void
InstBranchReg::extractDest(Register* dest)
{
    *dest = toR(*this);
}
bool
InstBranchReg::checkDest(Register dest)
{
    return dest == toR(*this);
}

bool
InstBranchImm::IsTHIS(const Instruction& i)
{
    return InstBImm::IsTHIS(i) || InstBLImm::IsTHIS(i);
}

InstBranchImm*
InstBranchImm::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBranchImm*)&i;
    return nullptr;
}

void
InstBranchImm::extractImm(BOffImm* dest)
{
    *dest = BOffImm(*this);
}

bool
InstBXReg::IsTHIS(const Instruction& i)
{
    return (i.encode() & IsBRegMask) == IsBX;
}

InstBXReg*
InstBXReg::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBXReg*)&i;
    return nullptr;
}

bool
InstBLXReg::IsTHIS(const Instruction& i)
{
    return (i.encode() & IsBRegMask) == IsBLX;

}
InstBLXReg*
InstBLXReg::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBLXReg*)&i;
    return nullptr;
}

bool
InstBImm::IsTHIS(const Instruction& i)
{
    return (i.encode () & IsBImmMask) == IsB;
}
InstBImm*
InstBImm::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBImm*)&i;
    return nullptr;
}

bool
InstBLImm::IsTHIS(const Instruction& i)
{
    return (i.encode () & IsBImmMask) == IsBL;

}
InstBLImm*
InstBLImm::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBLImm*)&i;
    return nullptr;
}

bool
InstMovWT::IsTHIS(Instruction& i)
{
    return  InstMovW::IsTHIS(i) || InstMovT::IsTHIS(i);
}
InstMovWT*
InstMovWT::AsTHIS(Instruction& i)
{
    if (IsTHIS(i))
        return (InstMovWT*)&i;
    return nullptr;
}

void
InstMovWT::extractImm(Imm16* imm)
{
    *imm = Imm16(*this);
}
bool
InstMovWT::checkImm(Imm16 imm)
{
    return imm.decode() == Imm16(*this).decode();
}

void
InstMovWT::extractDest(Register* dest)
{
    *dest = toRD(*this);
}
bool
InstMovWT::checkDest(Register dest)
{
    return dest == toRD(*this);
}

bool
InstMovW::IsTHIS(const Instruction& i)
{
    return (i.encode() & IsWTMask) == IsW;
}

InstMovW*
InstMovW::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstMovW*)&i;
    return nullptr;
}
InstMovT*
InstMovT::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstMovT*)&i;
    return nullptr;
}

bool
InstMovT::IsTHIS(const Instruction& i)
{
    return (i.encode() & IsWTMask) == IsT;
}

InstALU*
InstALU::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstALU*)&i;
    return nullptr;
}
bool
InstALU::IsTHIS(const Instruction& i)
{
    return (i.encode() & ALUMask) == 0;
}
void
InstALU::extractOp(ALUOp* ret)
{
    *ret = ALUOp(encode() & (0xf << 21));
}
bool
InstALU::checkOp(ALUOp op)
{
    ALUOp mine;
    extractOp(&mine);
    return mine == op;
}
void
InstALU::extractDest(Register* ret)
{
    *ret = toRD(*this);
}
bool
InstALU::checkDest(Register rd)
{
    return rd == toRD(*this);
}
void
InstALU::extractOp1(Register* ret)
{
    *ret = toRN(*this);
}
bool
InstALU::checkOp1(Register rn)
{
    return rn == toRN(*this);
}
Operand2
InstALU::extractOp2()
{
    return Operand2(encode());
}

InstCMP*
InstCMP::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstCMP*)&i;
    return nullptr;
}

bool
InstCMP::IsTHIS(const Instruction& i)
{
    return InstALU::IsTHIS(i) && InstALU::AsTHIS(i)->checkDest(r0) && InstALU::AsTHIS(i)->checkOp(OpCmp);
}

InstMOV*
InstMOV::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstMOV*)&i;
    return nullptr;
}

bool
InstMOV::IsTHIS(const Instruction& i)
{
    return InstALU::IsTHIS(i) && InstALU::AsTHIS(i)->checkOp1(r0) && InstALU::AsTHIS(i)->checkOp(OpMov);
}

Op2Reg
Operand2::toOp2Reg() {
    return *(Op2Reg*)this;
}
O2RegImmShift
Op2Reg::toO2RegImmShift() {
    return *(O2RegImmShift*)this;
}
O2RegRegShift
Op2Reg::toO2RegRegShift() {
    return *(O2RegRegShift*)this;
}

Imm16::Imm16(Instruction& inst)
  : lower(inst.encode() & 0xfff),
    upper(inst.encode() >> 16),
    invalid(0xfff)
{ }

Imm16::Imm16(uint32_t imm)
  : lower(imm & 0xfff), pad(0),
    upper((imm >> 12) & 0xf),
    invalid(0)
{
    MOZ_ASSERT(decode() == imm);
}

Imm16::Imm16()
  : invalid(0xfff)
{ }

void
jit::PatchJump(CodeLocationJump& jump_, CodeLocationLabel label)
{
    // We need to determine if this jump can fit into the standard 24+2 bit
    // address or if we need a larger branch (or just need to use our pool
    // entry).
    Instruction* jump = (Instruction*)jump_.raw();
    // jumpWithPatch() returns the offset of the jump and never a pool or nop.
    Assembler::Condition c;
    jump->extractCond(&c);
    MOZ_ASSERT(jump->is<InstBranchImm>() || jump->is<InstLDR>());

    int jumpOffset = label.raw() - jump_.raw();
    if (BOffImm::IsInRange(jumpOffset)) {
        // This instruction started off as a branch, and will remain one.
        Assembler::RetargetNearBranch(jump, jumpOffset, c);
    } else {
        // This instruction started off as a branch, but now needs to be demoted
        // to an ldr.
        uint8_t** slot = reinterpret_cast<uint8_t**>(jump_.jumpTableEntry());
        Assembler::RetargetFarBranch(jump, slot, label.raw(), c);
    }
}

void
Assembler::finish()
{
    flush();
    MOZ_ASSERT(!isFinished);
    isFinished = true;

    for (unsigned int i = 0; i < tmpDataRelocations_.length(); i++) {
        size_t offset = tmpDataRelocations_[i].getOffset();
        size_t real_offset = offset + m_buffer.poolSizeBefore(offset);
        dataRelocations_.writeUnsigned(real_offset);
    }

    for (unsigned int i = 0; i < tmpJumpRelocations_.length(); i++) {
        size_t offset = tmpJumpRelocations_[i].getOffset();
        size_t real_offset = offset + m_buffer.poolSizeBefore(offset);
        jumpRelocations_.writeUnsigned(real_offset);
    }

    for (unsigned int i = 0; i < tmpPreBarriers_.length(); i++) {
        size_t offset = tmpPreBarriers_[i].getOffset();
        size_t real_offset = offset + m_buffer.poolSizeBefore(offset);
        preBarriers_.writeUnsigned(real_offset);
    }
}

void
Assembler::executableCopy(uint8_t* buffer)
{
    MOZ_ASSERT(isFinished);
    m_buffer.executableCopy(buffer);
    AutoFlushICache::setRange(uintptr_t(buffer), m_buffer.size());
}

uint32_t
Assembler::actualOffset(uint32_t off_) const
{
    return off_ + m_buffer.poolSizeBefore(off_);
}

uint32_t
Assembler::actualIndex(uint32_t idx_) const
{
    ARMBuffer::PoolEntry pe(idx_);
    return m_buffer.poolEntryOffset(pe);
}

uint8_t*
Assembler::PatchableJumpAddress(JitCode* code, uint32_t pe_)
{
    return code->raw() + pe_;
}

BufferOffset
Assembler::actualOffset(BufferOffset off_) const
{
    return BufferOffset(off_.getOffset() + m_buffer.poolSizeBefore(off_.getOffset()));
}

class RelocationIterator
{
    CompactBufferReader reader_;
    // Offset in bytes.
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

template<class Iter>
const uint32_t*
Assembler::GetCF32Target(Iter* iter)
{
    Instruction* inst1 = iter->cur();
    Instruction* inst2 = iter->next();
    Instruction* inst3 = iter->next();
    Instruction* inst4 = iter->next();

    if (inst1->is<InstBranchImm>()) {
        // See if we have a simple case, b #offset.
        BOffImm imm;
        InstBranchImm* jumpB = inst1->as<InstBranchImm>();
        jumpB->extractImm(&imm);
        return imm.getDest(inst1)->raw();
    }

    if (inst1->is<InstMovW>() && inst2->is<InstMovT>() &&
        (inst3->is<InstNOP>() || inst3->is<InstBranchReg>() || inst4->is<InstBranchReg>()))
    {
        // See if we have the complex case:
        //  movw r_temp, #imm1
        //  movt r_temp, #imm2
        //  bx r_temp
        // OR
        //  movw r_temp, #imm1
        //  movt r_temp, #imm2
        //  str pc, [sp]
        //  bx r_temp

        Imm16 targ_bot;
        Imm16 targ_top;
        Register temp;

        // Extract both the temp register and the bottom immediate.
        InstMovW* bottom = inst1->as<InstMovW>();
        bottom->extractImm(&targ_bot);
        bottom->extractDest(&temp);

        // Extract the top part of the immediate.
        InstMovT* top = inst2->as<InstMovT>();
        top->extractImm(&targ_top);

        // Make sure they are being loaded into the same register.
        MOZ_ASSERT(top->checkDest(temp));

        // Make sure we're branching to the same register.
#ifdef DEBUG
        // A toggled call sometimes has a NOP instead of a branch for the third
        // instruction. No way to assert that it's valid in that situation.
        if (!inst3->is<InstNOP>()) {
            InstBranchReg* realBranch = inst3->is<InstBranchReg>() ? inst3->as<InstBranchReg>()
                                                                   : inst4->as<InstBranchReg>();
            MOZ_ASSERT(realBranch->checkDest(temp));
        }
#endif

        uint32_t* dest = (uint32_t*) (targ_bot.decode() | (targ_top.decode() << 16));
        return dest;
    }

    if (inst1->is<InstLDR>()) {
        InstLDR* load = inst1->as<InstLDR>();
        uint32_t inst = load->encode();
        // Get the address of the instruction as a raw pointer.
        char* dataInst = reinterpret_cast<char*>(load);
        IsUp_ iu = IsUp_(inst & IsUp);
        int32_t offset = inst & 0xfff;
        if (iu != IsUp) {
            offset = - offset;
        }
        uint32_t** ptr = (uint32_t**)&dataInst[offset + 8];
        return *ptr;

    }

    MOZ_CRASH("unsupported branch relocation");
}

uintptr_t
Assembler::GetPointer(uint8_t* instPtr)
{
    InstructionIterator iter((Instruction*)instPtr);
    uintptr_t ret = (uintptr_t)GetPtr32Target(&iter, nullptr, nullptr);
    return ret;
}

template<class Iter>
const uint32_t*
Assembler::GetPtr32Target(Iter* start, Register* dest, RelocStyle* style)
{
    Instruction* load1 = start->cur();
    Instruction* load2 = start->next();

    if (load1->is<InstMovW>() && load2->is<InstMovT>()) {
        // See if we have the complex case:
        //  movw r_temp, #imm1
        //  movt r_temp, #imm2

        Imm16 targ_bot;
        Imm16 targ_top;
        Register temp;

        // Extract both the temp register and the bottom immediate.
        InstMovW* bottom = load1->as<InstMovW>();
        bottom->extractImm(&targ_bot);
        bottom->extractDest(&temp);

        // Extract the top part of the immediate.
        InstMovT* top = load2->as<InstMovT>();
        top->extractImm(&targ_top);

        // Make sure they are being loaded into the same register.
        MOZ_ASSERT(top->checkDest(temp));

        if (dest)
            *dest = temp;
        if (style)
            *style = L_MOVWT;

        uint32_t* value = (uint32_t*) (targ_bot.decode() | (targ_top.decode() << 16));
        return value;
    }
    if (load1->is<InstLDR>()) {
        InstLDR* load = load1->as<InstLDR>();
        uint32_t inst = load->encode();
        // Get the address of the instruction as a raw pointer.
        char* dataInst = reinterpret_cast<char*>(load);
        IsUp_ iu = IsUp_(inst & IsUp);
        int32_t offset = inst & 0xfff;
        if (iu == IsDown)
            offset = - offset;
        if (dest)
            *dest = toRD(*load);
        if (style)
            *style = L_LDR;
        uint32_t** ptr = (uint32_t**)&dataInst[offset + 8];
        return *ptr;
    }

    MOZ_CRASH("unsupported relocation");
}

static JitCode*
CodeFromJump(InstructionIterator* jump)
{
    uint8_t* target = (uint8_t*)Assembler::GetCF32Target(jump);
    return JitCode::FromExecutable(target);
}

void
Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    RelocationIterator iter(reader);
    while (iter.read()) {
        InstructionIterator institer((Instruction*) (code->raw() + iter.offset()));
        JitCode* child = CodeFromJump(&institer);
        MarkJitCodeUnbarriered(trc, &child, "rel32");
    }
}

template <class Iter>
static void
TraceOneDataRelocation(JSTracer* trc, Iter* iter)
{
    Instruction* ins = iter->cur();
    Register dest;
    Assembler::RelocStyle rs;
    const void* prior = Assembler::GetPtr32Target(iter, &dest, &rs);
    void* ptr = const_cast<void*>(prior);

    // The low bit shouldn't be set. If it is, we probably got a dummy
    // pointer inserted by CodeGenerator::visitNurseryObject, but we
    // shouldn't be able to trigger GC before those are patched to their
    // real values.
    MOZ_ASSERT(!(uintptr_t(ptr) & 0x1));

    // No barrier needed since these are constants.
    gc::MarkGCThingUnbarriered(trc, &ptr, "ion-masm-ptr");

    if (ptr != prior) {
        MacroAssemblerARM::ma_mov_patch(Imm32(int32_t(ptr)), dest, Assembler::Always, rs, ins);

        // L_LDR won't cause any instructions to be updated.
        if (rs != Assembler::L_LDR) {
            AutoFlushICache::flush(uintptr_t(ins), 4);
            AutoFlushICache::flush(uintptr_t(ins->next()), 4);
        }
    }
}

static void
TraceDataRelocations(JSTracer* trc, uint8_t* buffer, CompactBufferReader& reader)
{
    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        InstructionIterator iter((Instruction*)(buffer + offset));
        TraceOneDataRelocation(trc, &iter);
    }
}

static void
TraceDataRelocations(JSTracer* trc, ARMBuffer* buffer,
                     Vector<BufferOffset, 0, SystemAllocPolicy>* locs)
{
    for (unsigned int idx = 0; idx < locs->length(); idx++) {
        BufferOffset bo = (*locs)[idx];
        ARMBuffer::AssemblerBufferInstIterator iter(bo, buffer);
        TraceOneDataRelocation(trc, &iter);
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
        InstructionIterator iter((Instruction*)(buffer + offset));
        Instruction* ins = iter.cur();
        Register dest;
        Assembler::RelocStyle rs;
        const void* prior = Assembler::GetPtr32Target(&iter, &dest, &rs);
        void* ptr = const_cast<void*>(prior);
        uintptr_t word = reinterpret_cast<uintptr_t>(ptr);

        if (!(word & 0x1))
            continue;

        uint32_t index = word >> 1;
        JSObject* obj = nurseryObjects[index];
        MacroAssembler::ma_mov_patch(Imm32(int32_t(obj)), dest, Assembler::Always, rs, ins);

        if (rs != Assembler::L_LDR) {
            // L_LDR won't cause any instructions to be updated.
            AutoFlushICache::flush(uintptr_t(ins), 4);
            AutoFlushICache::flush(uintptr_t(ins->next()), 4);
        }

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

    if (tmpDataRelocations_.length())
        ::TraceDataRelocations(trc, &m_buffer, &tmpDataRelocations_);
}

void
Assembler::processCodeLabels(uint8_t* rawCode)
{
    for (size_t i = 0; i < codeLabels_.length(); i++) {
        CodeLabel label = codeLabels_[i];
        Bind(rawCode, label.dest(), rawCode + actualOffset(label.src()->offset()));
    }
}

void
Assembler::writeCodePointer(AbsoluteLabel* absoluteLabel) {
    MOZ_ASSERT(!absoluteLabel->bound());
    BufferOffset off = writeInst(LabelBase::INVALID_OFFSET);

    // The x86/x64 makes general use of AbsoluteLabel and weaves a linked list
    // of uses of an AbsoluteLabel through the assembly. ARM only uses labels
    // for the case statements of switch jump tables. Thus, for simplicity, we
    // simply treat the AbsoluteLabel as a label and bind it to the offset of
    // the jump table entry that needs to be patched.
    LabelBase* label = absoluteLabel;
    label->bind(off.getOffset());
}

void
Assembler::Bind(uint8_t* rawCode, AbsoluteLabel* label, const void* address)
{
    // See writeCodePointer comment.
    uint32_t off = actualOffset(label->offset());
    *reinterpret_cast<const void**>(rawCode + off) = address;
}

Assembler::Condition
Assembler::InvertCondition(Condition cond)
{
    const uint32_t ConditionInversionBit = 0x10000000;
    return Condition(ConditionInversionBit ^ cond);
}

Imm8::TwoImm8mData
Imm8::EncodeTwoImms(uint32_t imm)
{
    // In the ideal case, we are looking for a number that (in binary) looks
    // like:
    //   0b((00)*)n_1((00)*)n_2((00)*)
    //      left  n1   mid  n2
    //   where both n_1 and n_2 fit into 8 bits.
    // Since this is being done with rotates, we also need to handle the case
    // that one of these numbers is in fact split between the left and right
    // sides, in which case the constant will look like:
    //   0bn_1a((00)*)n_2((00)*)n_1b
    //     n1a  mid  n2   rgh    n1b
    // Also remember, values are rotated by multiples of two, and left, mid or
    // right can have length zero.
    uint32_t imm1, imm2;
    int left = CountLeadingZeroes32(imm) & 0x1E;
    uint32_t no_n1 = imm & ~(0xff << (24 - left));

    // Not technically needed: this case only happens if we can encode as a
    // single imm8m. There is a perfectly reasonable encoding in this case, but
    // we shouldn't encourage people to do things like this.
    if (no_n1 == 0)
        return TwoImm8mData();

    int mid = CountLeadingZeroes32(no_n1) & 0x1E;
    uint32_t no_n2 = no_n1 & ~((0xff << ((24 - mid) & 0x1f)) | 0xff >> ((8 + mid) & 0x1f));

    if (no_n2 == 0) {
        // We hit the easy case, no wraparound.
        // Note: a single constant *may* look like this.
        int imm1shift = left + 8;
        int imm2shift = mid + 8;
        imm1 = (imm >> (32 - imm1shift)) & 0xff;
        if (imm2shift >= 32) {
            imm2shift = 0;
            // This assert does not always hold, in fact, this would lead to
            // some incredibly subtle bugs.
            // assert((imm & 0xff) == no_n1);
            imm2 = no_n1;
        } else {
            imm2 = ((imm >> (32 - imm2shift)) | (imm << imm2shift)) & 0xff;
            MOZ_ASSERT( ((no_n1 >> (32 - imm2shift)) | (no_n1 << imm2shift)) ==
                        imm2);
        }
        MOZ_ASSERT((imm1shift & 0x1) == 0);
        MOZ_ASSERT((imm2shift & 0x1) == 0);
        return TwoImm8mData(datastore::Imm8mData(imm1, imm1shift >> 1),
                            datastore::Imm8mData(imm2, imm2shift >> 1));
    }

    // Either it wraps, or it does not fit. If we initially chopped off more
    // than 8 bits, then it won't fit.
    if (left >= 8)
        return TwoImm8mData();

    int right = 32 - (CountLeadingZeroes32(no_n2) & 30);
    // All remaining set bits *must* fit into the lower 8 bits.
    // The right == 8 case should be handled by the previous case.
    if (right > 8)
        return TwoImm8mData();

    // Make sure the initial bits that we removed for no_n1 fit into the
    // 8-(32-right) leftmost bits.
    if (((imm & (0xff << (24 - left))) << (8 - right)) != 0) {
        // BUT we may have removed more bits than we needed to for no_n1
        // 0x04104001 e.g. we can encode 0x104 with a single op, then 0x04000001
        // with a second, but we try to encode 0x0410000 and find that we need a
        // second op for 0x4000, and 0x1 cannot be included in the encoding of
        // 0x04100000.
        no_n1 = imm & ~((0xff >> (8 - right)) | (0xff << (24 + right)));
        mid = CountLeadingZeroes32(no_n1) & 30;
        no_n2 = no_n1  & ~((0xff << ((24 - mid)&31)) | 0xff >> ((8 + mid)&31));
        if (no_n2 != 0)
            return TwoImm8mData();
    }

    // Now assemble all of this information into a two coherent constants it is
    // a rotate right from the lower 8 bits.
    int imm1shift = 8 - right;
    imm1 = 0xff & ((imm << imm1shift) | (imm >> (32 - imm1shift)));
    MOZ_ASSERT((imm1shift & ~0x1e) == 0);
    // left + 8 + mid is the position of the leftmost bit of n_2.
    // We needed to rotate 0x000000ab right by 8 in order to get 0xab000000,
    // then shift again by the leftmost bit in order to get the constant that we
    // care about.
    int imm2shift =  mid + 8;
    imm2 = ((imm >> (32 - imm2shift)) | (imm << imm2shift)) & 0xff;
    MOZ_ASSERT((imm1shift & 0x1) == 0);
    MOZ_ASSERT((imm2shift & 0x1) == 0);
    return TwoImm8mData(datastore::Imm8mData(imm1, imm1shift >> 1),
                        datastore::Imm8mData(imm2, imm2shift >> 1));
}

ALUOp
jit::ALUNeg(ALUOp op, Register dest, Imm32* imm, Register* negDest)
{
    // Find an alternate ALUOp to get the job done, and use a different imm.
    *negDest = dest;
    switch (op) {
      case OpMov:
        *imm = Imm32(~imm->value);
        return OpMvn;
      case OpMvn:
        *imm = Imm32(~imm->value);
        return OpMov;
      case OpAnd:
        *imm = Imm32(~imm->value);
        return OpBic;
      case OpBic:
        *imm = Imm32(~imm->value);
        return OpAnd;
      case OpAdd:
        *imm = Imm32(-imm->value);
        return OpSub;
      case OpSub:
        *imm = Imm32(-imm->value);
        return OpAdd;
      case OpCmp:
        *imm = Imm32(-imm->value);
        return OpCmn;
      case OpCmn:
        *imm = Imm32(-imm->value);
        return OpCmp;
      case OpTst:
        MOZ_ASSERT(dest == InvalidReg);
        *imm = Imm32(~imm->value);
        *negDest = ScratchRegister;
        return OpBic;
        // orr has orn on thumb2 only.
      default:
        return OpInvalid;
    }
}

bool
jit::can_dbl(ALUOp op)
{
    // Some instructions can't be processed as two separate instructions such as
    // and, and possibly add (when we're setting ccodes). There is also some
    // hilarity with *reading* condition codes. For example, adc dest, src1,
    // 0xfff; (add with carry) can be split up into adc dest, src1, 0xf00; add
    // dest, dest, 0xff, since "reading" the condition code increments the
    // result by one conditionally, that only needs to be done on one of the two
    // instructions.
    switch (op) {
      case OpBic:
      case OpAdd:
      case OpSub:
      case OpEor:
      case OpOrr:
        return true;
      default:
        return false;
    }
}

bool
jit::condsAreSafe(ALUOp op) {
    // Even when we are setting condition codes, sometimes we can get away with
    // splitting an operation into two. For example, if our immediate is
    // 0x00ff00ff, and the operation is eors we can split this in half, since x
    // ^ 0x00ff0000 ^ 0x000000ff should set all of its condition codes exactly
    // the same as x ^ 0x00ff00ff. However, if the operation were adds, we
    // cannot split this in half. If the source on the add is 0xfff00ff0, the
    // result sholud be 0xef10ef, but do we set the overflow bit or not?
    // Depending on which half is performed first (0x00ff0000 or 0x000000ff) the
    // V bit will be set differently, and *not* updating the V bit would be
    // wrong. Theoretically, the following should work:
    //  adds r0, r1, 0x00ff0000;
    //  addsvs r0, r1, 0x000000ff;
    //  addvc r0, r1, 0x000000ff;
    // But this is 3 instructions, and at that point, we might as well use
    // something else.
    switch(op) {
      case OpBic:
      case OpOrr:
      case OpEor:
        return true;
      default:
        return false;
    }
}

ALUOp
jit::getDestVariant(ALUOp op)
{
    // All of the compare operations are dest-less variants of a standard
    // operation. Given the dest-less variant, return the dest-ful variant.
    switch (op) {
      case OpCmp:
        return OpSub;
      case OpCmn:
        return OpAdd;
      case OpTst:
        return OpAnd;
      case OpTeq:
        return OpEor;
      default:
        return op;
    }
}

O2RegImmShift
jit::O2Reg(Register r) {
    return O2RegImmShift(r, LSL, 0);
}

O2RegImmShift
jit::lsl(Register r, int amt)
{
    MOZ_ASSERT(0 <= amt && amt <= 31);
    return O2RegImmShift(r, LSL, amt);
}

O2RegImmShift
jit::lsr(Register r, int amt)
{
    MOZ_ASSERT(1 <= amt && amt <= 32);
    return O2RegImmShift(r, LSR, amt);
}

O2RegImmShift
jit::ror(Register r, int amt)
{
    MOZ_ASSERT(1 <= amt && amt <= 31);
    return O2RegImmShift(r, ROR, amt);
}
O2RegImmShift
jit::rol(Register r, int amt)
{
    MOZ_ASSERT(1 <= amt && amt <= 31);
    return O2RegImmShift(r, ROR, 32 - amt);
}

O2RegImmShift
jit::asr (Register r, int amt)
{
    MOZ_ASSERT(1 <= amt && amt <= 32);
    return O2RegImmShift(r, ASR, amt);
}


O2RegRegShift
jit::lsl(Register r, Register amt)
{
    return O2RegRegShift(r, LSL, amt);
}

O2RegRegShift
jit::lsr(Register r, Register amt)
{
    return O2RegRegShift(r, LSR, amt);
}

O2RegRegShift
jit::ror(Register r, Register amt)
{
    return O2RegRegShift(r, ROR, amt);
}

O2RegRegShift
jit::asr (Register r, Register amt)
{
    return O2RegRegShift(r, ASR, amt);
}

static js::jit::DoubleEncoder doubleEncoder;

/* static */ const js::jit::VFPImm js::jit::VFPImm::One(0x3FF00000);

js::jit::VFPImm::VFPImm(uint32_t top)
{
    data = -1;
    datastore::Imm8VFPImmData tmp;
    if (doubleEncoder.lookup(top, &tmp))
        data = tmp.encode();
}

BOffImm::BOffImm(Instruction& inst)
  : data(inst.encode() & 0x00ffffff)
{
}

Instruction*
BOffImm::getDest(Instruction* src)
{
    // TODO: It is probably worthwhile to verify that src is actually a branch.
    // NOTE: This does not explicitly shift the offset of the destination left by 2,
    // since it is indexing into an array of instruction sized objects.
    return &src[(((int32_t)data << 8) >> 8) + 2];
}

const js::jit::DoubleEncoder::DoubleEntry js::jit::DoubleEncoder::table[256] = {
#include "jit/arm/DoubleEntryTable.tbl"
};

// VFPRegister implementation
VFPRegister
VFPRegister::doubleOverlay(unsigned int which) const
{
    MOZ_ASSERT(!_isInvalid);
    MOZ_ASSERT(which == 0);
    if (kind != Double)
        return VFPRegister(code_ >> 1, Double);
    return *this;
}
VFPRegister
VFPRegister::singleOverlay(unsigned int which) const
{
    MOZ_ASSERT(!_isInvalid);
    if (kind == Double) {
        // There are no corresponding float registers for d16-d31.
        MOZ_ASSERT(code_ < 16);
        MOZ_ASSERT(which < 2);
        return VFPRegister((code_ << 1) + which, Single);
    }
    MOZ_ASSERT(which == 0);
    return VFPRegister(code_, Single);
}

VFPRegister
VFPRegister::sintOverlay(unsigned int which) const
{
    MOZ_ASSERT(!_isInvalid);
    if (kind == Double) {
        // There are no corresponding float registers for d16-d31.
        MOZ_ASSERT(code_ < 16);
        MOZ_ASSERT(which < 2);
        return VFPRegister((code_ << 1) + which, Int);
    }
    MOZ_ASSERT(which == 0);
    return VFPRegister(code_, Int);
}
VFPRegister
VFPRegister::uintOverlay(unsigned int which) const
{
    MOZ_ASSERT(!_isInvalid);
    if (kind == Double) {
        // There are no corresponding float registers for d16-d31.
        MOZ_ASSERT(code_ < 16);
        MOZ_ASSERT(which < 2);
        return VFPRegister((code_ << 1) + which, UInt);
    }
    MOZ_ASSERT(which == 0);
    return VFPRegister(code_, UInt);
}

bool
VFPRegister::isInvalid() const
{
    return _isInvalid;
}

bool
VFPRegister::isMissing() const
{
    MOZ_ASSERT(!_isInvalid);
    return _isMissing;
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

// Size of the instruction stream, in bytes. Including pools. This function
// expects all pools that need to be placed have been placed. If they haven't
// then we need to go an flush the pools :(
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

// Write a blob of binary into the instruction stream.
BufferOffset
Assembler::writeInst(uint32_t x)
{
    return m_buffer.putInt(x);
}

BufferOffset
Assembler::writeBranchInst(uint32_t x)
{
    return m_buffer.putInt(x, /* markAsBranch = */ true);
}
void
Assembler::WriteInstStatic(uint32_t x, uint32_t* dest)
{
    MOZ_ASSERT(dest != nullptr);
    *dest = x;
}

void
Assembler::align(int alignment)
{
    m_buffer.align(alignment);
}

BufferOffset
Assembler::as_nop()
{
    return writeInst(0xe320f000);
}

static uint32_t
EncodeAlu(Register dest, Register src1, Operand2 op2, ALUOp op, SetCond_ sc,
          Assembler::Condition c)
{
    return (int)op | (int)sc | (int) c | op2.encode() |
           ((dest == InvalidReg) ? 0 : RD(dest)) |
           ((src1 == InvalidReg) ? 0 : RN(src1));
}

BufferOffset
Assembler::as_alu(Register dest, Register src1, Operand2 op2,
                  ALUOp op, SetCond_ sc, Condition c)
{
    return writeInst(EncodeAlu(dest, src1, op2, op, sc, c));
}

BufferOffset
Assembler::as_mov(Register dest, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, InvalidReg, op2, OpMov, sc, c);
}

/* static */ void
Assembler::as_alu_patch(Register dest, Register src1, Operand2 op2, ALUOp op, SetCond_ sc,
                        Condition c, uint32_t* pos)
{
    WriteInstStatic(EncodeAlu(dest, src1, op2, op, sc, c), pos);
}

/* static */ void
Assembler::as_mov_patch(Register dest, Operand2 op2, SetCond_ sc, Condition c, uint32_t* pos)
{
    as_alu_patch(dest, InvalidReg, op2, OpMov, sc, c, pos);
}

BufferOffset
Assembler::as_mvn(Register dest, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, InvalidReg, op2, OpMvn, sc, c);
}

// Logical operations.
BufferOffset
Assembler::as_and(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpAnd, sc, c);
}
BufferOffset
Assembler::as_bic(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpBic, sc, c);
}
BufferOffset
Assembler::as_eor(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpEor, sc, c);
}
BufferOffset
Assembler::as_orr(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpOrr, sc, c);
}

// Mathematical operations.
BufferOffset
Assembler::as_adc(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpAdc, sc, c);
}
BufferOffset
Assembler::as_add(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpAdd, sc, c);
}
BufferOffset
Assembler::as_sbc(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpSbc, sc, c);
}
BufferOffset
Assembler::as_sub(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpSub, sc, c);
}
BufferOffset
Assembler::as_rsb(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpRsb, sc, c);
}
BufferOffset
Assembler::as_rsc(Register dest, Register src1, Operand2 op2, SetCond_ sc, Condition c)
{
    return as_alu(dest, src1, op2, OpRsc, sc, c);
}

// Test operations.
BufferOffset
Assembler::as_cmn(Register src1, Operand2 op2, Condition c)
{
    return as_alu(InvalidReg, src1, op2, OpCmn, SetCond, c);
}
BufferOffset
Assembler::as_cmp(Register src1, Operand2 op2, Condition c)
{
    return as_alu(InvalidReg, src1, op2, OpCmp, SetCond, c);
}
BufferOffset
Assembler::as_teq(Register src1, Operand2 op2, Condition c)
{
    return as_alu(InvalidReg, src1, op2, OpTeq, SetCond, c);
}
BufferOffset
Assembler::as_tst(Register src1, Operand2 op2, Condition c)
{
    return as_alu(InvalidReg, src1, op2, OpTst, SetCond, c);
}

static MOZ_CONSTEXPR_VAR Register NoAddend = { Registers::pc };

static const int SignExtend = 0x06000070;

enum SignExtend {
    SxSxtb = 10 << 20,
    SxSxth = 11 << 20,
    SxUxtb = 14 << 20,
    SxUxth = 15 << 20
};

// Sign extension operations.
BufferOffset
Assembler::as_sxtb(Register dest, Register src, int rotate, Condition c)
{
    return writeInst((int)c | SignExtend | SxSxtb | RN(NoAddend) | RD(dest) | ((rotate & 3) << 10) | src.code());
}
BufferOffset
Assembler::as_sxth(Register dest, Register src, int rotate, Condition c)
{
    return writeInst((int)c | SignExtend | SxSxth | RN(NoAddend) | RD(dest) | ((rotate & 3) << 10) | src.code());
}
BufferOffset
Assembler::as_uxtb(Register dest, Register src, int rotate, Condition c)
{
    return writeInst((int)c | SignExtend | SxUxtb | RN(NoAddend) | RD(dest) | ((rotate & 3) << 10) | src.code());
}
BufferOffset
Assembler::as_uxth(Register dest, Register src, int rotate, Condition c)
{
    return writeInst((int)c | SignExtend | SxUxth | RN(NoAddend) | RD(dest) | ((rotate & 3) << 10) | src.code());
}

static uint32_t
EncodeMovW(Register dest, Imm16 imm, Assembler::Condition c)
{
    MOZ_ASSERT(HasMOVWT());
    return 0x03000000 | c | imm.encode() | RD(dest);
}

static uint32_t
EncodeMovT(Register dest, Imm16 imm, Assembler::Condition c)
{
    MOZ_ASSERT(HasMOVWT());
    return 0x03400000 | c | imm.encode() | RD(dest);
}

// Not quite ALU worthy, but these are useful none the less. These also have
// the isue of these being formatted completly differently from the standard ALU
// operations.
BufferOffset
Assembler::as_movw(Register dest, Imm16 imm, Condition c)
{
    return writeInst(EncodeMovW(dest, imm, c));
}

/* static */ void
Assembler::as_movw_patch(Register dest, Imm16 imm, Condition c, Instruction* pos)
{
    WriteInstStatic(EncodeMovW(dest, imm, c), (uint32_t*)pos);
}

BufferOffset
Assembler::as_movt(Register dest, Imm16 imm, Condition c)
{
    return writeInst(EncodeMovT(dest, imm, c));
}

/* static */ void
Assembler::as_movt_patch(Register dest, Imm16 imm, Condition c, Instruction* pos)
{
    WriteInstStatic(EncodeMovT(dest, imm, c), (uint32_t*)pos);
}

static const int mull_tag = 0x90;

BufferOffset
Assembler::as_genmul(Register dhi, Register dlo, Register rm, Register rn,
                     MULOp op, SetCond_ sc, Condition c)
{

    return writeInst(RN(dhi) | maybeRD(dlo) | RM(rm) | rn.code() | op | sc | c | mull_tag);
}
BufferOffset
Assembler::as_mul(Register dest, Register src1, Register src2, SetCond_ sc, Condition c)
{
    return as_genmul(dest, InvalidReg, src1, src2, OpmMul, sc, c);
}
BufferOffset
Assembler::as_mla(Register dest, Register acc, Register src1, Register src2,
                  SetCond_ sc, Condition c)
{
    return as_genmul(dest, acc, src1, src2, OpmMla, sc, c);
}
BufferOffset
Assembler::as_umaal(Register destHI, Register destLO, Register src1, Register src2, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmUmaal, NoSetCond, c);
}
BufferOffset
Assembler::as_mls(Register dest, Register acc, Register src1, Register src2, Condition c)
{
    return as_genmul(dest, acc, src1, src2, OpmMls, NoSetCond, c);
}

BufferOffset
Assembler::as_umull(Register destHI, Register destLO, Register src1, Register src2,
                    SetCond_ sc, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmUmull, sc, c);
}

BufferOffset
Assembler::as_umlal(Register destHI, Register destLO, Register src1, Register src2,
                    SetCond_ sc, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmUmlal, sc, c);
}

BufferOffset
Assembler::as_smull(Register destHI, Register destLO, Register src1, Register src2,
                    SetCond_ sc, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmSmull, sc, c);
}

BufferOffset
Assembler::as_smlal(Register destHI, Register destLO, Register src1, Register src2,
                    SetCond_ sc, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmSmlal, sc, c);
}

BufferOffset
Assembler::as_sdiv(Register rd, Register rn, Register rm, Condition c)
{
    return writeInst(0x0710f010 | c | RN(rd) | RM(rm) | rn.code());
}

BufferOffset
Assembler::as_udiv(Register rd, Register rn, Register rm, Condition c)
{
    return writeInst(0x0730f010 | c | RN(rd) | RM(rm) | rn.code());
}

BufferOffset
Assembler::as_clz(Register dest, Register src, Condition c)
{
    return writeInst(RD(dest) | src.code() | c | 0x016f0f10);
}

// Data transfer instructions: ldr, str, ldrb, strb. Using an int to
// differentiate between 8 bits and 32 bits is overkill, but meh.

static uint32_t
EncodeDtr(LoadStore ls, int size, Index mode, Register rt, DTRAddr addr, Assembler::Condition c)
{
    MOZ_ASSERT(mode == Offset ||  (rt != addr.getBase() && pc != addr.getBase()));
    MOZ_ASSERT(size == 32 || size == 8);
    return 0x04000000 | ls | (size == 8 ? 0x00400000 : 0) | mode | c | RT(rt) | addr.encode();
}

BufferOffset
Assembler::as_dtr(LoadStore ls, int size, Index mode, Register rt, DTRAddr addr, Condition c)
{
    return writeInst(EncodeDtr(ls, size, mode, rt, addr, c));
}

/* static */ void
Assembler::as_dtr_patch(LoadStore ls, int size, Index mode, Register rt, DTRAddr addr, Condition c,
                        uint32_t* dest)
{
    WriteInstStatic(EncodeDtr(ls, size, mode, rt, addr, c), dest);
}

class PoolHintData {
  public:
    enum LoadType {
        // Set 0 to bogus, since that is the value most likely to be
        // accidentally left somewhere.
        PoolBOGUS  = 0,
        PoolDTR    = 1,
        PoolBranch = 2,
        PoolVDTR   = 3
    };

  private:
    uint32_t   index_    : 16;
    uint32_t   cond_     : 4;
    LoadType   loadType_ : 2;
    uint32_t   destReg_  : 5;
    uint32_t   destType_ : 1;
    uint32_t   ONES     : 4;

    static const uint32_t ExpectedOnes = 0xfu;

  public:
    void init(uint32_t index, Assembler::Condition cond, LoadType lt, Register destReg) {
        index_ = index;
        MOZ_ASSERT(index_ == index);
        cond_ = cond >> 28;
        MOZ_ASSERT(cond_ == cond >> 28);
        loadType_ = lt;
        ONES = ExpectedOnes;
        destReg_ = destReg.code();
        destType_ = 0;
    }
    void init(uint32_t index, Assembler::Condition cond, LoadType lt, const VFPRegister& destReg) {
        MOZ_ASSERT(destReg.isFloat());
        index_ = index;
        MOZ_ASSERT(index_ == index);
        cond_ = cond >> 28;
        MOZ_ASSERT(cond_ == cond >> 28);
        loadType_ = lt;
        ONES = ExpectedOnes;
        destReg_ = destReg.id();
        destType_ = destReg.isDouble();
    }
    Assembler::Condition getCond() {
        return Assembler::Condition(cond_ << 28);
    }

    Register getReg() {
        return Register::FromCode(destReg_);
    }
    VFPRegister getVFPReg() {
        VFPRegister r = VFPRegister(destReg_, destType_ ? VFPRegister::Double : VFPRegister::Single);
        return r;
    }

    int32_t getIndex() {
        return index_;
    }
    void setIndex(uint32_t index) {
        MOZ_ASSERT(ONES == ExpectedOnes && loadType_ != PoolBOGUS);
        index_ = index;
        MOZ_ASSERT(index_ == index);
    }

    LoadType getLoadType() {
        // If this *was* a PoolBranch, but the branch has already been bound
        // then this isn't going to look like a real poolhintdata, but we still
        // want to lie about it so everyone knows it *used* to be a branch.
        if (ONES != ExpectedOnes)
            return PoolHintData::PoolBranch;
        return loadType_;
    }

    bool isValidPoolHint() {
        // Most instructions cannot have a condition that is 0xf. Notable
        // exceptions are blx and the entire NEON instruction set. For the
        // purposes of pool loads, and possibly patched branches, the possible
        // instructions are ldr and b, neither of which can have a condition
        // code of 0xf.
        return ONES == ExpectedOnes;
    }
};

union PoolHintPun {
    PoolHintData phd;
    uint32_t raw;
};

// Handles all of the other integral data transferring functions: ldrsb, ldrsh,
// ldrd, etc. The size is given in bits.
BufferOffset
Assembler::as_extdtr(LoadStore ls, int size, bool IsSigned, Index mode,
                     Register rt, EDtrAddr addr, Condition c)
{
    int extra_bits2 = 0;
    int extra_bits1 = 0;
    switch(size) {
      case 8:
        MOZ_ASSERT(IsSigned);
        MOZ_ASSERT(ls != IsStore);
        extra_bits1 = 0x1;
        extra_bits2 = 0x2;
        break;
      case 16:
        // 'case 32' doesn't need to be handled, it is handled by the default
        // ldr/str.
        extra_bits2 = 0x01;
        extra_bits1 = (ls == IsStore) ? 0 : 1;
        if (IsSigned) {
            MOZ_ASSERT(ls != IsStore);
            extra_bits2 |= 0x2;
        }
        break;
      case 64:
        extra_bits2 = (ls == IsStore) ? 0x3 : 0x2;
        extra_bits1 = 0;
        break;
      default:
        MOZ_CRASH("SAY WHAT?");
    }
    return writeInst(extra_bits2 << 5 | extra_bits1 << 20 | 0x90 |
                     addr.encode() | RT(rt) | mode | c);
}

BufferOffset
Assembler::as_dtm(LoadStore ls, Register rn, uint32_t mask,
                DTMMode mode, DTMWriteBack wb, Condition c)
{
    return writeInst(0x08000000 | RN(rn) | ls |
                     mode | mask | c | wb);
}

BufferOffset
Assembler::as_Imm32Pool(Register dest, uint32_t value, Condition c)
{
    PoolHintPun php;
    php.phd.init(0, c, PoolHintData::PoolDTR, dest);
    return m_buffer.allocEntry(1, 1, (uint8_t*)&php.raw, (uint8_t*)&value);
}

/* static */ void
Assembler::WritePoolEntry(Instruction* addr, Condition c, uint32_t data)
{
    MOZ_ASSERT(addr->is<InstLDR>());
    int32_t offset = addr->encode() & 0xfff;
    if ((addr->encode() & IsUp) != IsUp)
        offset = -offset;
    char * rawAddr = reinterpret_cast<char*>(addr);
    uint32_t * dest = reinterpret_cast<uint32_t*>(&rawAddr[offset + 8]);
    *dest = data;
    Condition orig_cond;
    addr->extractCond(&orig_cond);
    MOZ_ASSERT(orig_cond == c);
}

BufferOffset
Assembler::as_BranchPool(uint32_t value, RepatchLabel* label, ARMBuffer::PoolEntry* pe, Condition c)
{
    PoolHintPun php;
    php.phd.init(0, c, PoolHintData::PoolBranch, pc);
    BufferOffset ret = m_buffer.allocEntry(1, 1, (uint8_t*)&php.raw, (uint8_t*)&value, pe,
                                           /* markAsBranch = */ true);
    // If this label is already bound, then immediately replace the stub load
    // with a correct branch.
    if (label->bound()) {
        BufferOffset dest(label);
        as_b(dest.diffB<BOffImm>(ret), c, ret);
    } else {
        label->use(ret.getOffset());
    }
    return ret;
}

BufferOffset
Assembler::as_FImm64Pool(VFPRegister dest, double value, Condition c)
{
    MOZ_ASSERT(dest.isDouble());
    PoolHintPun php;
    php.phd.init(0, c, PoolHintData::PoolVDTR, dest);
    return m_buffer.allocEntry(1, 2, (uint8_t*)&php.raw, (uint8_t*)&value);
}

BufferOffset
Assembler::as_FImm32Pool(VFPRegister dest, float value, Condition c)
{
    // Insert floats into the double pool as they have the same limitations on
    // immediate offset. This wastes 4 bytes padding per float. An alternative
    // would be to have a separate pool for floats.
    MOZ_ASSERT(dest.isSingle());
    PoolHintPun php;
    php.phd.init(0, c, PoolHintData::PoolVDTR, dest);
    return m_buffer.allocEntry(1, 1, (uint8_t*)&php.raw, (uint8_t*)&value);
}

// Pool callbacks stuff:
void
Assembler::InsertIndexIntoTag(uint8_t* load_, uint32_t index)
{
    uint32_t* load = (uint32_t*)load_;
    PoolHintPun php;
    php.raw = *load;
    php.phd.setIndex(index);
    *load = php.raw;
}

// patchConstantPoolLoad takes the address of the instruction that wants to be
// patched, and the address of the start of the constant pool, and figures
// things out from there.
void
Assembler::PatchConstantPoolLoad(void* loadAddr, void* constPoolAddr)
{
    PoolHintData data = *(PoolHintData*)loadAddr;
    uint32_t* instAddr = (uint32_t*) loadAddr;
    int offset = (char*)constPoolAddr - (char*)loadAddr;
    switch(data.getLoadType()) {
      case PoolHintData::PoolBOGUS:
        MOZ_CRASH("bogus load type!");
      case PoolHintData::PoolDTR:
        Assembler::as_dtr_patch(IsLoad, 32, Offset, data.getReg(),
                                DTRAddr(pc, DtrOffImm(offset+4*data.getIndex() - 8)),
                                data.getCond(), instAddr);
        break;
      case PoolHintData::PoolBranch:
        // Either this used to be a poolBranch, and the label was already bound,
        // so it was replaced with a real branch, or this may happen in the
        // future. If this is going to happen in the future, then the actual
        // bits that are written here don't matter (except the condition code,
        // since that is always preserved across patchings) but if it does not
        // get bound later, then we want to make sure this is a load from the
        // pool entry (and the pool entry should be nullptr so it will crash).
        if (data.isValidPoolHint()) {
            Assembler::as_dtr_patch(IsLoad, 32, Offset, pc,
                                    DTRAddr(pc, DtrOffImm(offset+4*data.getIndex() - 8)),
                                    data.getCond(), instAddr);
        }
        break;
      case PoolHintData::PoolVDTR: {
        VFPRegister dest = data.getVFPReg();
        int32_t imm = offset + (data.getIndex() * 4) - 8;
        MOZ_ASSERT(-1024 < imm && imm < 1024);
        Assembler::as_vdtr_patch(IsLoad, dest, VFPAddr(pc, VFPOffImm(imm)), data.getCond(),
                                 instAddr);
        break;
      }
    }
}

// Atomic instruction stuff:

BufferOffset
Assembler::as_ldrex(Register rt, Register rn, Condition c)
{
    return writeInst(0x01900f9f | (int)c | RT(rt) | RN(rn));
}

BufferOffset
Assembler::as_ldrexh(Register rt, Register rn, Condition c)
{
    return writeInst(0x01f00f9f | (int)c | RT(rt) | RN(rn));
}

BufferOffset
Assembler::as_ldrexb(Register rt, Register rn, Condition c)
{
    return writeInst(0x01d00f9f | (int)c | RT(rt) | RN(rn));
}

BufferOffset
Assembler::as_strex(Register rd, Register rt, Register rn, Condition c)
{
    return writeInst(0x01800f90 | (int)c | RD(rd) | RN(rn) | rt.code());
}

BufferOffset
Assembler::as_strexh(Register rd, Register rt, Register rn, Condition c)
{
    return writeInst(0x01e00f90 | (int)c | RD(rd) | RN(rn) | rt.code());
}

BufferOffset
Assembler::as_strexb(Register rd, Register rt, Register rn, Condition c)
{
    return writeInst(0x01c00f90 | (int)c | RD(rd) | RN(rn) | rt.code());
}

// Memory barrier stuff:

BufferOffset
Assembler::as_dmb(BarrierOption option)
{
    return writeInst(0xf57ff050U | (int)option);
}
BufferOffset
Assembler::as_dsb(BarrierOption option)
{
    return writeInst(0xf57ff040U | (int)option);
}
BufferOffset
Assembler::as_isb()
{
    return writeInst(0xf57ff06fU); // option == SY
}
BufferOffset
Assembler::as_dsb_trap()
{
    // DSB is "mcr 15, 0, r0, c7, c10, 4".
    // See eg https://bugs.kde.org/show_bug.cgi?id=228060.
    // ARMv7 manual, "VMSA CP15 c7 register summary".
    // Flagged as "legacy" starting with ARMv8, may be disabled on chip, see
    // ARMv8 manual E2.7.3 and G3.18.16.
    return writeInst(0xee070f9a);
}
BufferOffset
Assembler::as_dmb_trap()
{
    // DMB is "mcr 15, 0, r0, c7, c10, 5".
    // ARMv7 manual, "VMSA CP15 c7 register summary".
    // Flagged as "legacy" starting with ARMv8, may be disabled on chip, see
    // ARMv8 manual E2.7.3 and G3.18.16.
    return writeInst(0xee070fba);
}
BufferOffset
Assembler::as_isb_trap()
{
    // ISB is "mcr 15, 0, r0, c7, c5, 4".
    // ARMv7 manual, "VMSA CP15 c7 register summary".
    // Flagged as "legacy" starting with ARMv8, may be disabled on chip, see
    // ARMv8 manual E2.7.3 and G3.18.16.
    return writeInst(0xee070f94);
}

// Control flow stuff:

// bx can *only* branch to a register, never to an immediate.
BufferOffset
Assembler::as_bx(Register r, Condition c)
{
    BufferOffset ret = writeInst(((int) c) | OpBx | r.code());
    return ret;
}
void
Assembler::WritePoolGuard(BufferOffset branch, Instruction* dest, BufferOffset afterPool)
{
    BOffImm off = afterPool.diffB<BOffImm>(branch);
    *dest = InstBImm(off, Always);
}
// Branch can branch to an immediate *or* to a register.
// Branches to immediates are pc relative, branches to registers are absolute.
BufferOffset
Assembler::as_b(BOffImm off, Condition c)
{
    BufferOffset ret = writeBranchInst(((int)c) | OpB | off.encode());
    return ret;
}

BufferOffset
Assembler::as_b(Label* l, Condition c)
{
    if (m_buffer.oom()) {
        BufferOffset ret;
        return ret;
    }

    if (l->bound()) {
        // Note only one instruction is emitted here, the NOP is overwritten.
        BufferOffset ret = writeBranchInst(Always | InstNOP::NopInst);
        as_b(BufferOffset(l).diffB<BOffImm>(ret), c, ret);
        return ret;
    }

    int32_t old;
    BufferOffset ret;
    if (l->used()) {
        old = l->offset();
        // This will currently throw an assertion if we couldn't actually
        // encode the offset of the branch.
        if (!BOffImm::IsInRange(old)) {
            m_buffer.fail_bail();
            return ret;
        }
        ret = as_b(BOffImm(old), c);
    } else {
        old = LabelBase::INVALID_OFFSET;
        BOffImm inv;
        ret = as_b(inv, c);
    }
    DebugOnly<int32_t> check = l->use(ret.getOffset());
    MOZ_ASSERT(check == old);
    return ret;
}
BufferOffset
Assembler::as_b(BOffImm off, Condition c, BufferOffset inst)
{
    *editSrc(inst) = InstBImm(off, c);
    return inst;
}

// blx can go to either an immediate or a register.
// When blx'ing to a register, we change processor state depending on the low
// bit of the register when blx'ing to an immediate, we *always* change
// processor state.

BufferOffset
Assembler::as_blx(Register r, Condition c)
{
    return writeInst(((int) c) | OpBlx | r.code());
}

// bl can only branch to an pc-relative immediate offset
// It cannot change the processor state.
BufferOffset
Assembler::as_bl(BOffImm off, Condition c)
{
    return writeBranchInst(((int)c) | OpBl | off.encode());
}

BufferOffset
Assembler::as_bl(Label* l, Condition c)
{
    if (m_buffer.oom()) {
        BufferOffset ret;
        return ret;
    }

    if (l->bound()) {
        // Note only one instruction is emitted here, the NOP is overwritten.
        BufferOffset ret = writeBranchInst(Always | InstNOP::NopInst);
        as_bl(BufferOffset(l).diffB<BOffImm>(ret), c, ret);
        return ret;
    }

    int32_t old;
    BufferOffset ret;
    // See if the list was empty :(
    if (l->used()) {
        // This will currently throw an assertion if we couldn't actually encode
        // the offset of the branch.
        old = l->offset();
        if (!BOffImm::IsInRange(old)) {
            m_buffer.fail_bail();
            return ret;
        }
        ret = as_bl(BOffImm(old), c);
    } else {
        old = LabelBase::INVALID_OFFSET;
        BOffImm inv;
        ret = as_bl(inv, c);
    }
    DebugOnly<int32_t> check = l->use(ret.getOffset());
    MOZ_ASSERT(check == old);
    return ret;
}
BufferOffset
Assembler::as_bl(BOffImm off, Condition c, BufferOffset inst)
{
    *editSrc(inst) = InstBLImm(off, c);
    return inst;
}

BufferOffset
Assembler::as_mrs(Register r, Condition c)
{
    return writeInst(0x010f0000 | int(c) | RD(r));
}

BufferOffset
Assembler::as_msr(Register r, Condition c)
{
    // Hardcode the 'mask' field to 0b11 for now. It is bits 18 and 19, which
    // are the two high bits of the 'c' in this constant.
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return writeInst(0x012cf000 | int(c) | r.code());
}

// VFP instructions!
enum vfp_tags {
    VfpTag   = 0x0C000A00,
    VfpArith = 0x02000000
};
BufferOffset
Assembler::writeVFPInst(vfp_size sz, uint32_t blob)
{
    MOZ_ASSERT((sz & blob) == 0);
    MOZ_ASSERT((VfpTag & blob) == 0);
    return writeInst(VfpTag | sz | blob);
}

/* static */ void
Assembler::WriteVFPInstStatic(vfp_size sz, uint32_t blob, uint32_t* dest)
{
    MOZ_ASSERT((sz & blob) == 0);
    MOZ_ASSERT((VfpTag & blob) == 0);
    WriteInstStatic(VfpTag | sz | blob, dest);
}

// Unityped variants: all registers hold the same (ieee754 single/double)
// notably not included are vcvt; vmov vd, #imm; vmov rt, vn.
BufferOffset
Assembler::as_vfp_float(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                  VFPOp op, Condition c)
{
    // Make sure we believe that all of our operands are the same kind.
    MOZ_ASSERT_IF(!vn.isMissing(), vd.equiv(vn));
    MOZ_ASSERT_IF(!vm.isMissing(), vd.equiv(vm));
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;
    return writeVFPInst(sz, VD(vd) | VN(vn) | VM(vm) | op | VfpArith | c);
}

BufferOffset
Assembler::as_vadd(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                 Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvAdd, c);
}

BufferOffset
Assembler::as_vdiv(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                 Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvDiv, c);
}

BufferOffset
Assembler::as_vmul(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                 Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvMul, c);
}

BufferOffset
Assembler::as_vnmul(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                  Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvMul, c);
}

BufferOffset
Assembler::as_vnmla(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                  Condition c)
{
    MOZ_CRASH("Feature NYI");
}

BufferOffset
Assembler::as_vnmls(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                  Condition c)
{
    MOZ_CRASH("Feature NYI");
}

BufferOffset
Assembler::as_vneg(VFPRegister vd, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vm, OpvNeg, c);
}

BufferOffset
Assembler::as_vsqrt(VFPRegister vd, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vm, OpvSqrt, c);
}

BufferOffset
Assembler::as_vabs(VFPRegister vd, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vm, OpvAbs, c);
}

BufferOffset
Assembler::as_vsub(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                 Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvSub, c);
}

BufferOffset
Assembler::as_vcmp(VFPRegister vd, VFPRegister vm,
                 Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vm, OpvCmp, c);
}
BufferOffset
Assembler::as_vcmpz(VFPRegister vd, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, NoVFPRegister, OpvCmpz, c);
}

// Specifically, a move between two same sized-registers.
BufferOffset
Assembler::as_vmov(VFPRegister vd, VFPRegister vsrc, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vsrc, OpvMov, c);
}
// Transfer between Core and VFP.

// Unlike the next function, moving between the core registers and vfp registers
// can't be *that* properly typed. Namely, since I don't want to munge the type
// VFPRegister to also include core registers. Thus, the core and vfp registers
// are passed in based on their type, and src/dest is determined by the
// float2core.

BufferOffset
Assembler::as_vxfer(Register vt1, Register vt2, VFPRegister vm, FloatToCore_ f2c,
                    Condition c, int idx)
{
    vfp_size sz = IsSingle;
    if (vm.isDouble()) {
        // Technically, this can be done with a vmov à la ARM ARM under vmov
        // however, that requires at least an extra bit saying if the operation
        // should be performed on the lower or upper half of the double. Moving
        // a single to/from 2N/2N+1 isn't equivalent, since there are 32 single
        // registers, and 32 double registers so there is no way to encode the
        // last 16 double registers.
        sz = IsDouble;
        MOZ_ASSERT(idx == 0 || idx == 1);
        // If we are transferring a single half of the double then it must be
        // moving a VFP reg to a core reg.
        MOZ_ASSERT_IF(vt2 == InvalidReg, f2c == FloatToCore);
        idx = idx << 21;
    } else {
        MOZ_ASSERT(idx == 0);
    }

    if (vt2 == InvalidReg) {
        return writeVFPInst(sz, WordTransfer | f2c | c |
                            RT(vt1) | maybeRN(vt2) | VN(vm) | idx);
    } else {
        // We are doing a 64 bit transfer.
        return writeVFPInst(sz, DoubleTransfer | f2c | c |
                            RT(vt1) | maybeRN(vt2) | VM(vm) | idx);
    }
}
enum vcvt_destFloatness {
    VcvtToInteger = 1 << 18,
    VcvtToFloat  = 0 << 18
};
enum vcvt_toZero {
    VcvtToZero = 1 << 7, // Use the default rounding mode, which rounds truncates.
    VcvtToFPSCR = 0 << 7 // Use whatever rounding mode the fpscr specifies.
};
enum vcvt_Signedness {
    VcvtToSigned   = 1 << 16,
    VcvtToUnsigned = 0 << 16,
    VcvtFromSigned   = 1 << 7,
    VcvtFromUnsigned = 0 << 7
};

// Our encoding actually allows just the src and the dest (and their types) to
// uniquely specify the encoding that we are going to use.
BufferOffset
Assembler::as_vcvt(VFPRegister vd, VFPRegister vm, bool useFPSCR,
                   Condition c)
{
    // Unlike other cases, the source and dest types cannot be the same.
    MOZ_ASSERT(!vd.equiv(vm));
    vfp_size sz = IsDouble;
    if (vd.isFloat() && vm.isFloat()) {
        // Doing a float -> float conversion.
        if (vm.isSingle())
            sz = IsSingle;
        return writeVFPInst(sz, c | 0x02B700C0 | VM(vm) | VD(vd));
    }

    // At least one of the registers should be a float.
    vcvt_destFloatness destFloat;
    vcvt_Signedness opSign;
    vcvt_toZero doToZero = VcvtToFPSCR;
    MOZ_ASSERT(vd.isFloat() || vm.isFloat());
    if (vd.isSingle() || vm.isSingle()) {
        sz = IsSingle;
    }
    if (vd.isFloat()) {
        destFloat = VcvtToFloat;
        opSign = (vm.isSInt()) ? VcvtFromSigned : VcvtFromUnsigned;
    } else {
        destFloat = VcvtToInteger;
        opSign = (vd.isSInt()) ? VcvtToSigned : VcvtToUnsigned;
        doToZero = useFPSCR ? VcvtToFPSCR : VcvtToZero;
    }
    return writeVFPInst(sz, c | 0x02B80040 | VD(vd) | VM(vm) | destFloat | opSign | doToZero);
}

BufferOffset
Assembler::as_vcvtFixed(VFPRegister vd, bool isSigned, uint32_t fixedPoint, bool toFixed, Condition c)
{
    MOZ_ASSERT(vd.isFloat());
    uint32_t sx = 0x1;
    vfp_size sf = vd.isDouble() ? IsDouble : IsSingle;
    int32_t imm5 = fixedPoint;
    imm5 = (sx ? 32 : 16) - imm5;
    MOZ_ASSERT(imm5 >= 0);
    imm5 = imm5 >> 1 | (imm5 & 1) << 5;
    return writeVFPInst(sf, 0x02BA0040 | VD(vd) | toFixed << 18 | sx << 7 |
                        (!isSigned) << 16 | imm5 | c);
}

// Transfer between VFP and memory.
static uint32_t
EncodeVdtr(LoadStore ls, VFPRegister vd, VFPAddr addr, Assembler::Condition c)
{
    return ls | 0x01000000 | addr.encode() | VD(vd) | c;
}

BufferOffset
Assembler::as_vdtr(LoadStore ls, VFPRegister vd, VFPAddr addr,
                   Condition c /* vfp doesn't have a wb option */)
{
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;
    return writeVFPInst(sz, EncodeVdtr(ls, vd, addr, c));
}

/* static */ void
Assembler::as_vdtr_patch(LoadStore ls, VFPRegister vd, VFPAddr addr, Condition c, uint32_t* dest)
{
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;
    WriteVFPInstStatic(sz, EncodeVdtr(ls, vd, addr, c), dest);
}

// VFP's ldm/stm work differently from the standard arm ones. You can only
// transfer a range.

BufferOffset
Assembler::as_vdtm(LoadStore st, Register rn, VFPRegister vd, int length,
                   /* also has update conditions */ Condition c)
{
    MOZ_ASSERT(length <= 16 && length >= 0);
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;

    if (vd.isDouble())
        length *= 2;

    return writeVFPInst(sz, dtmLoadStore | RN(rn) | VD(vd) | length |
                        dtmMode | dtmUpdate | dtmCond);
}

BufferOffset
Assembler::as_vimm(VFPRegister vd, VFPImm imm, Condition c)
{
    MOZ_ASSERT(imm.isValid());
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;
    return writeVFPInst(sz,  c | imm.encode() | VD(vd) | 0x02B00000);

}
BufferOffset
Assembler::as_vmrs(Register r, Condition c)
{
    return writeInst(c | 0x0ef10a10 | RT(r));
}

BufferOffset
Assembler::as_vmsr(Register r, Condition c)
{
    return writeInst(c | 0x0ee10a10 | RT(r));
}

bool
Assembler::nextLink(BufferOffset b, BufferOffset* next)
{
    Instruction branch = *editSrc(b);
    MOZ_ASSERT(branch.is<InstBranchImm>());

    BOffImm destOff;
    branch.as<InstBranchImm>()->extractImm(&destOff);
    if (destOff.isInvalid())
        return false;

    // Propagate the next link back to the caller, by constructing a new
    // BufferOffset into the space they provided.
    new (next) BufferOffset(destOff.decode());
    return true;
}

void
Assembler::bind(Label* label, BufferOffset boff)
{
    if (label->used()) {
        bool more;
        // If our caller didn't give us an explicit target to bind to then we
        // want to bind to the location of the next instruction.
        BufferOffset dest = boff.assigned() ? boff : nextOffset();
        BufferOffset b(label);
        do {
            BufferOffset next;
            more = nextLink(b, &next);
            Instruction branch = *editSrc(b);
            Condition c;
            branch.extractCond(&c);
            if (branch.is<InstBImm>())
                as_b(dest.diffB<BOffImm>(b), c, b);
            else if (branch.is<InstBLImm>())
                as_bl(dest.diffB<BOffImm>(b), c, b);
            else
                MOZ_CRASH("crazy fixup!");
            b = next;
        } while (more);
    }
    label->bind(nextOffset().getOffset());
}

void
Assembler::bind(RepatchLabel* label)
{
    BufferOffset dest = nextOffset();
    if (label->used()) {
        // If the label has a use, then change this use to refer to the bound
        // label.
        BufferOffset branchOff(label->offset());
        // Since this was created with a RepatchLabel, the value written in the
        // instruction stream is not branch shaped, it is PoolHintData shaped.
        Instruction* branch = editSrc(branchOff);
        PoolHintPun p;
        p.raw = branch->encode();
        Condition cond;
        if (p.phd.isValidPoolHint())
            cond = p.phd.getCond();
        else
            branch->extractCond(&cond);
        as_b(dest.diffB<BOffImm>(branchOff), cond, branchOff);
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
            BufferOffset labelBranchOffset(label);
            BufferOffset next;

            // Find the head of the use chain for label.
            while (nextLink(labelBranchOffset, &next))
                labelBranchOffset = next;

            // Then patch the head of label's use chain to the tail of target's
            // use chain, prepending the entire use chain of target.
            Instruction branch = *editSrc(labelBranchOffset);
            Condition c;
            branch.extractCond(&c);
            int32_t prev = target->use(label->offset());
            if (branch.is<InstBImm>())
                as_b(BOffImm(prev), c, labelBranchOffset);
            else if (branch.is<InstBLImm>())
                as_bl(BOffImm(prev), c, labelBranchOffset);
            else
                MOZ_CRASH("crazy fixup!");
        } else {
            // The target is unbound and unused. We can just take the head of
            // the list hanging off of label, and dump that into target.
            DebugOnly<uint32_t> prev = target->use(label->offset());
            MOZ_ASSERT((int32_t)prev == Label::INVALID_OFFSET);
        }
    }
    label->reset();

}


static int stopBKPT = -1;
void
Assembler::as_bkpt()
{
    // This is a count of how many times a breakpoint instruction has been
    // generated. It is embedded into the instruction for debugging
    // purposes. Gdb will print "bkpt xxx" when you attempt to dissassemble a
    // breakpoint with the number xxx embedded into it. If this breakpoint is
    // being hit, then you can run (in gdb):
    //  >b dbg_break
    //  >b main
    //  >commands
    //  >set stopBKPT = xxx
    //  >c
    //  >end
    // which will set a breakpoint on the function dbg_break above set a
    // scripted breakpoint on main that will set the (otherwise unmodified)
    // value to the number of the breakpoint, so dbg_break will actuall be
    // called and finally, when you run the executable, execution will halt when
    // that breakpoint is generated.
    static int hit = 0;
    if (stopBKPT == hit)
        dbg_break();
    writeInst(0xe1200070 | (hit & 0xf) | ((hit & 0xfff0) << 4));
    hit++;
}

void
Assembler::flushBuffer()
{
    m_buffer.flushPool();
}

void
Assembler::enterNoPool(size_t maxInst)
{
    m_buffer.enterNoPool(maxInst);
}

void
Assembler::leaveNoPool()
{
    m_buffer.leaveNoPool();
}

ptrdiff_t
Assembler::GetBranchOffset(const Instruction* i_)
{
    MOZ_ASSERT(i_->is<InstBranchImm>());
    InstBranchImm* i = i_->as<InstBranchImm>();
    BOffImm dest;
    i->extractImm(&dest);
    return dest.decode();
}
void
Assembler::RetargetNearBranch(Instruction* i, int offset, bool final)
{
    Assembler::Condition c;
    i->extractCond(&c);
    RetargetNearBranch(i, offset, c, final);
}

void
Assembler::RetargetNearBranch(Instruction* i, int offset, Condition cond, bool final)
{
    // Retargeting calls is totally unsupported!
    MOZ_ASSERT_IF(i->is<InstBranchImm>(), i->is<InstBImm>() || i->is<InstBLImm>());
    if (i->is<InstBLImm>())
        new (i) InstBLImm(BOffImm(offset), cond);
    else
        new (i) InstBImm(BOffImm(offset), cond);

    // Flush the cache, since an instruction was overwritten.
    if (final)
        AutoFlushICache::flush(uintptr_t(i), 4);
}

void
Assembler::RetargetFarBranch(Instruction* i, uint8_t** slot, uint8_t* dest, Condition cond)
{
    int32_t offset = reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(i);
    if (!i->is<InstLDR>()) {
        new (i) InstLDR(Offset, pc, DTRAddr(pc, DtrOffImm(offset - 8)), cond);
        AutoFlushICache::flush(uintptr_t(i), 4);
    }
    *slot = dest;

}

struct PoolHeader : Instruction {
    struct Header
    {
        // The size should take into account the pool header.
        // The size is in units of Instruction (4 bytes), not byte.
        uint32_t size : 15;
        bool isNatural : 1;
        uint32_t ONES : 16;

        Header(int size_, bool isNatural_)
          : size(size_),
            isNatural(isNatural_),
            ONES(0xffff)
        { }

        Header(const Instruction* i) {
            JS_STATIC_ASSERT(sizeof(Header) == sizeof(uint32_t));
            memcpy(this, i, sizeof(Header));
            MOZ_ASSERT(ONES == 0xffff);
        }

        uint32_t raw() const {
            JS_STATIC_ASSERT(sizeof(Header) == sizeof(uint32_t));
            uint32_t dest;
            memcpy(&dest, this, sizeof(Header));
            return dest;
        }
    };

    PoolHeader(int size_, bool isNatural_)
      : Instruction(Header(size_, isNatural_).raw(), true)
    { }

    uint32_t size() const {
        Header tmp(this);
        return tmp.size;
    }
    uint32_t isNatural() const {
        Header tmp(this);
        return tmp.isNatural;
    }
    static bool IsTHIS(const Instruction& i) {
        return (*i.raw() & 0xffff0000) == 0xffff0000;
    }
    static const PoolHeader* AsTHIS(const Instruction& i) {
        if (!IsTHIS(i))
            return nullptr;
        return static_cast<const PoolHeader*>(&i);
    }
};


void
Assembler::WritePoolHeader(uint8_t* start, Pool* p, bool isNatural)
{
    STATIC_ASSERT(sizeof(PoolHeader) == 4);
    uint8_t* pool = start + 4;
    // Go through the usual rigmarole to get the size of the pool.
    pool += p->getPoolSize();
    uint32_t size = pool - start;
    MOZ_ASSERT((size & 3) == 0);
    size = size >> 2;
    MOZ_ASSERT(size < (1 << 15));
    PoolHeader header(size, isNatural);
    *(PoolHeader*)start = header;
}


// The size of an arbitrary 32-bit call in the instruction stream. On ARM this
// sequence is |pc = ldr pc - 4; imm32| given that we never reach the imm32.
uint32_t
Assembler::PatchWrite_NearCallSize()
{
    return sizeof(uint32_t);
}
void
Assembler::PatchWrite_NearCall(CodeLocationLabel start, CodeLocationLabel toCall)
{
    Instruction* inst = (Instruction*) start.raw();
    // Overwrite whatever instruction used to be here with a call. Since the
    // destination is in the same function, it will be within range of the
    // 24 << 2 byte bl instruction.
    uint8_t* dest = toCall.raw();
    new (inst) InstBLImm(BOffImm(dest - (uint8_t*)inst) , Always);
    // Ensure everyone sees the code that was just written into memory.
    AutoFlushICache::flush(uintptr_t(inst), 4);

}
void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, PatchedImmPtr newValue,
                                   PatchedImmPtr expectedValue)
{
    Instruction* ptr = (Instruction*) label.raw();
    InstructionIterator iter(ptr);
    Register dest;
    Assembler::RelocStyle rs;
    DebugOnly<const uint32_t*> val = GetPtr32Target(&iter, &dest, &rs);
    MOZ_ASSERT((uint32_t)(const uint32_t*)val == uint32_t(expectedValue.value));

    MacroAssembler::ma_mov_patch(Imm32(int32_t(newValue.value)), dest, Always, rs, ptr);

    // L_LDR won't cause any instructions to be updated.
    if (rs != L_LDR) {
        AutoFlushICache::flush(uintptr_t(ptr), 4);
        AutoFlushICache::flush(uintptr_t(ptr->next()), 4);
    }
}

void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue, ImmPtr expectedValue)
{
    PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value), PatchedImmPtr(expectedValue.value));
}

// This just stomps over memory with 32 bits of raw data. Its purpose is to
// overwrite the call of JITed code with 32 bits worth of an offset. This will
// is only meant to function on code that has been invalidated, so it should be
// totally safe. Since that instruction will never be executed again, a ICache
// flush should not be necessary
void
Assembler::PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm) {
    // Raw is going to be the return address.
    uint32_t* raw = (uint32_t*)label.raw();
    // Overwrite the 4 bytes before the return address, which will end up being
    // the call instruction.
    *(raw - 1) = imm.value;
}


uint8_t*
Assembler::NextInstruction(uint8_t* inst_, uint32_t* count)
{
    Instruction* inst = reinterpret_cast<Instruction*>(inst_);
    if (count != nullptr)
        *count += sizeof(Instruction);
    return reinterpret_cast<uint8_t*>(inst->next());
}

static bool
InstIsGuard(Instruction* inst, const PoolHeader** ph)
{
    Assembler::Condition c;
    inst->extractCond(&c);
    if (c != Assembler::Always)
        return false;
    if (!(inst->is<InstBXReg>() || inst->is<InstBImm>()))
        return false;
    // See if the next instruction is a pool header.
    *ph = (inst + 1)->as<const PoolHeader>();
    return *ph != nullptr;
}

static bool
InstIsBNop(Instruction* inst) {
    // In some special situations, it is necessary to insert a NOP into the
    // instruction stream that nobody knows about, since nobody should know
    // about it, make sure it gets skipped when Instruction::next() is called.
    // this generates a very specific nop, namely a branch to the next
    // instruction.
    Assembler::Condition c;
    inst->extractCond(&c);
    if (c != Assembler::Always)
        return false;
    if (!inst->is<InstBImm>())
        return false;
    InstBImm* b = inst->as<InstBImm>();
    BOffImm offset;
    b->extractImm(&offset);
    return offset.decode() == 4;
}

static bool
InstIsArtificialGuard(Instruction* inst, const PoolHeader** ph)
{
    if (!InstIsGuard(inst, ph))
        return false;
    return !(*ph)->isNatural();
}

// If the instruction points to a artificial pool guard then skip the pool.
Instruction*
Instruction::skipPool()
{
    const PoolHeader* ph;
    // If this is a guard, and the next instruction is a header, always work
    // around the pool. If it isn't a guard, then start looking ahead.
    if (InstIsGuard(this, &ph)) {
        // Don't skip a natural guard.
        if (ph->isNatural())
            return this;
        return (this + 1 + ph->size())->skipPool();
    }
    if (InstIsBNop(this))
        return (this + 1)->skipPool();
    return this;
}

// Cases to be handled:
// 1) no pools or branches in sight => return this+1
// 2) branch to next instruction => return this+2, because a nop needed to be inserted into the stream.
// 3) this+1 is an artificial guard for a pool => return first instruction after the pool
// 4) this+1 is a natural guard => return the branch
// 5) this is a branch, right before a pool => return first instruction after the pool
// in assembly form:
// 1) add r0, r0, r0 <= this
//    add r1, r1, r1 <= returned value
//    add r2, r2, r2
//
// 2) add r0, r0, r0 <= this
//    b foo
//    foo:
//    add r2, r2, r2 <= returned value
//
// 3) add r0, r0, r0 <= this
//    b after_pool;
//    .word 0xffff0002  # bit 15 being 0 indicates that the branch was not requested by the assembler
//    0xdeadbeef        # the 2 indicates that there is 1 pool entry, and the pool header
//    add r4, r4, r4 <= returned value
// 4) add r0, r0, r0 <= this
//    b after_pool  <= returned value
//    .word 0xffff8002  # bit 15 being 1 indicates that the branch was requested by the assembler
//    0xdeadbeef
//    add r4, r4, r4
// 5) b after_pool  <= this
//    .word 0xffff8002  # bit 15 has no bearing on the returned value
//    0xdeadbeef
//    add r4, r4, r4  <= returned value

Instruction*
Instruction::next()
{
    Instruction* ret = this+1;
    const PoolHeader* ph;
    // If this is a guard, and the next instruction is a header, always work
    // around the pool. If it isn't a guard, then start looking ahead.
    if (InstIsGuard(this, &ph))
        return (ret + ph->size())->skipPool();
    if (InstIsArtificialGuard(ret, &ph))
        return (ret + 1 + ph->size())->skipPool();
    return ret->skipPool();
}

void
Assembler::ToggleToJmp(CodeLocationLabel inst_)
{
    uint32_t* ptr = (uint32_t*)inst_.raw();

    DebugOnly<Instruction*> inst = (Instruction*)inst_.raw();
    MOZ_ASSERT(inst->is<InstCMP>());

    // Zero bits 20-27, then set 24-27 to be correct for a branch.
    // 20-23 will be party of the B's immediate, and should be 0.
    *ptr = (*ptr & ~(0xff << 20)) | (0xa0 << 20);
    AutoFlushICache::flush(uintptr_t(ptr), 4);
}

void
Assembler::ToggleToCmp(CodeLocationLabel inst_)
{
    uint32_t* ptr = (uint32_t*)inst_.raw();

    DebugOnly<Instruction*> inst = (Instruction*)inst_.raw();
    MOZ_ASSERT(inst->is<InstBImm>());

    // Ensure that this masking operation doesn't affect the offset of the
    // branch instruction when it gets toggled back.
    MOZ_ASSERT((*ptr & (0xf << 20)) == 0);

    // Also make sure that the CMP is valid. Part of having a valid CMP is that
    // all of the bits describing the destination in most ALU instructions are
    // all unset (looks like it is encoding r0).
    MOZ_ASSERT(toRD(*inst) == r0);

    // Zero out bits 20-27, then set them to be correct for a compare.
    *ptr = (*ptr & ~(0xff << 20)) | (0x35 << 20);

    AutoFlushICache::flush(uintptr_t(ptr), 4);
}

void
Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled)
{
    Instruction* inst = (Instruction*)inst_.raw();
    // Skip a pool with an artificial guard.
    inst = inst->skipPool();
    MOZ_ASSERT(inst->is<InstMovW>() || inst->is<InstLDR>());

    if (inst->is<InstMovW>()) {
        // If it looks like the start of a movw/movt sequence, then make sure we
        // have all of it (and advance the iterator past the full sequence).
        inst = inst->next();
        MOZ_ASSERT(inst->is<InstMovT>());
    }

    inst = inst->next();
    MOZ_ASSERT(inst->is<InstNOP>() || inst->is<InstBLXReg>());

    if (enabled == inst->is<InstBLXReg>()) {
        // Nothing to do.
        return;
    }

    if (enabled)
        *inst = InstBLXReg(ScratchRegister, Always);
    else
        *inst = InstNOP();

    AutoFlushICache::flush(uintptr_t(inst), 4);
}

size_t
Assembler::ToggledCallSize(uint8_t* code)
{
    Instruction* inst = (Instruction*)code;
    // Skip a pool with an artificial guard.
    inst = inst->skipPool();
    MOZ_ASSERT(inst->is<InstMovW>() || inst->is<InstLDR>());

    if (inst->is<InstMovW>()) {
        // If it looks like the start of a movw/movt sequence, then make sure we
        // have all of it (and advance the iterator past the full sequence).
        inst = inst->next();
        MOZ_ASSERT(inst->is<InstMovT>());
    }

    inst = inst->next();
    MOZ_ASSERT(inst->is<InstNOP>() || inst->is<InstBLXReg>());
    return uintptr_t(inst) + 4 - uintptr_t(code);
}

uint8_t*
Assembler::BailoutTableStart(uint8_t* code)
{
    Instruction* inst = (Instruction*)code;
    // Skip a pool with an artificial guard or NOP fill.
    inst = inst->skipPool();
    MOZ_ASSERT(inst->is<InstBLImm>());
    return (uint8_t*) inst;
}

void Assembler::UpdateBoundsCheck(uint32_t heapSize, Instruction* inst)
{
    MOZ_ASSERT(inst->is<InstCMP>());
    InstCMP* cmp = inst->as<InstCMP>();

    Register index;
    cmp->extractOp1(&index);

#ifdef DEBUG
    Operand2 op = cmp->extractOp2();
    MOZ_ASSERT(op.isImm8());
#endif

    Imm8 imm8 = Imm8(heapSize);
    MOZ_ASSERT(!imm8.invalid);

    *inst = InstALU(InvalidReg, index, imm8, OpCmp, SetCond, Always);
    // NOTE: we don't update the Auto Flush Cache!  this function is currently
    // only called from within AsmJSModule::patchHeapAccesses, which does that
    // for us. Don't call this!
}

InstructionIterator::InstructionIterator(Instruction* i_) : i(i_)
{
    // Work around pools with an artificial pool guard and around nop-fill.
    i = i->skipPool();
}

uint32_t Assembler::NopFill = 0;

uint32_t
Assembler::GetNopFill()
{
    static bool isSet = false;
    if (!isSet) {
        char* fillStr = getenv("ARM_ASM_NOP_FILL");
        uint32_t fill;
        if (fillStr && sscanf(fillStr, "%u", &fill) == 1)
            NopFill = fill;
        isSet = true;
    }
    return NopFill;
}

uint32_t Assembler::AsmPoolMaxOffset = 1024;

uint32_t
Assembler::GetPoolMaxOffset()
{
    static bool isSet = false;
    if (!isSet) {
        char* poolMaxOffsetStr = getenv("ASM_POOL_MAX_OFFSET");
        uint32_t poolMaxOffset;
        if (poolMaxOffsetStr && sscanf(poolMaxOffsetStr, "%u", &poolMaxOffset) == 1)
            AsmPoolMaxOffset = poolMaxOffset;
        isSet = true;
    }
    return AsmPoolMaxOffset;
}
