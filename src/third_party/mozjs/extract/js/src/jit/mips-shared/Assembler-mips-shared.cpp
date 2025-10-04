/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips-shared/Assembler-mips-shared.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "gc/Marking.h"
#include "jit/ExecutableAllocator.h"
#include "vm/Realm.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

// Encode a standard register when it is being used as rd, the rs, and
// an extra register(rt). These should never be called with an InvalidReg.
uint32_t js::jit::RS(Register r) {
  MOZ_ASSERT((r.code() & ~RegMask) == 0);
  return r.code() << RSShift;
}

uint32_t js::jit::RT(Register r) {
  MOZ_ASSERT((r.code() & ~RegMask) == 0);
  return r.code() << RTShift;
}

uint32_t js::jit::RD(Register r) {
  MOZ_ASSERT((r.code() & ~RegMask) == 0);
  return r.code() << RDShift;
}

uint32_t js::jit::RZ(Register r) {
  MOZ_ASSERT((r.code() & ~RegMask) == 0);
  return r.code() << RZShift;
}

uint32_t js::jit::SA(uint32_t value) {
  MOZ_ASSERT(value < 32);
  return value << SAShift;
}

uint32_t js::jit::FS(uint32_t value) {
  MOZ_ASSERT(value < 32);
  return value << FSShift;
}

Register js::jit::toRS(Instruction& i) {
  return Register::FromCode((i.encode() & RSMask) >> RSShift);
}

Register js::jit::toRT(Instruction& i) {
  return Register::FromCode((i.encode() & RTMask) >> RTShift);
}

Register js::jit::toRD(Instruction& i) {
  return Register::FromCode((i.encode() & RDMask) >> RDShift);
}

Register js::jit::toR(Instruction& i) {
  return Register::FromCode(i.encode() & RegMask);
}

void InstImm::extractImm16(BOffImm16* dest) { *dest = BOffImm16(*this); }

void AssemblerMIPSShared::finish() {
  MOZ_ASSERT(!isFinished);
  isFinished = true;
}

bool AssemblerMIPSShared::appendRawCode(const uint8_t* code, size_t numBytes) {
  return m_buffer.appendRawCode(code, numBytes);
}

bool AssemblerMIPSShared::reserve(size_t size) {
  // This buffer uses fixed-size chunks so there's no point in reserving
  // now vs. on-demand.
  return !oom();
}

bool AssemblerMIPSShared::swapBuffer(wasm::Bytes& bytes) {
  // For now, specialize to the one use case. As long as wasm::Bytes is a
  // Vector, not a linked-list of chunks, there's not much we can do other
  // than copy.
  MOZ_ASSERT(bytes.empty());
  if (!bytes.resize(bytesNeeded())) {
    return false;
  }
  m_buffer.executableCopy(bytes.begin());
  return true;
}

void AssemblerMIPSShared::copyJumpRelocationTable(uint8_t* dest) {
  if (jumpRelocations_.length()) {
    memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
  }
}

void AssemblerMIPSShared::copyDataRelocationTable(uint8_t* dest) {
  if (dataRelocations_.length()) {
    memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
  }
}

AssemblerMIPSShared::Condition AssemblerMIPSShared::InvertCondition(
    Condition cond) {
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

AssemblerMIPSShared::DoubleCondition AssemblerMIPSShared::InvertCondition(
    DoubleCondition cond) {
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

BOffImm16::BOffImm16(InstImm inst) : data(inst.encode() & Imm16Mask) {}

Instruction* BOffImm16::getDest(Instruction* src) const {
  return &src[(((int32_t)data << 16) >> 16) + 1];
}

bool AssemblerMIPSShared::oom() const {
  return AssemblerShared::oom() || m_buffer.oom() || jumpRelocations_.oom() ||
         dataRelocations_.oom();
}

// Size of the instruction stream, in bytes.
size_t AssemblerMIPSShared::size() const { return m_buffer.size(); }

// Size of the relocation table, in bytes.
size_t AssemblerMIPSShared::jumpRelocationTableBytes() const {
  return jumpRelocations_.length();
}

size_t AssemblerMIPSShared::dataRelocationTableBytes() const {
  return dataRelocations_.length();
}

// Size of the data table, in bytes.
size_t AssemblerMIPSShared::bytesNeeded() const {
  return size() + jumpRelocationTableBytes() + dataRelocationTableBytes();
}

// write a blob of binary into the instruction stream
BufferOffset AssemblerMIPSShared::writeInst(uint32_t x, uint32_t* dest) {
  MOZ_ASSERT(hasCreator());
  if (dest == nullptr) {
    return m_buffer.putInt(x);
  }

  WriteInstStatic(x, dest);
  return BufferOffset();
}

void AssemblerMIPSShared::WriteInstStatic(uint32_t x, uint32_t* dest) {
  MOZ_ASSERT(dest != nullptr);
  *dest = x;
}

BufferOffset AssemblerMIPSShared::haltingAlign(int alignment) {
  // TODO: Implement a proper halting align.
  return nopAlign(alignment);
}

BufferOffset AssemblerMIPSShared::nopAlign(int alignment) {
  BufferOffset ret;
  MOZ_ASSERT(m_buffer.isAligned(4));
  if (alignment == 8) {
    if (!m_buffer.isAligned(alignment)) {
      BufferOffset tmp = as_nop();
      if (!ret.assigned()) {
        ret = tmp;
      }
    }
  } else {
    MOZ_ASSERT((alignment & (alignment - 1)) == 0);
    while (size() & (alignment - 1)) {
      BufferOffset tmp = as_nop();
      if (!ret.assigned()) {
        ret = tmp;
      }
    }
  }
  return ret;
}

BufferOffset AssemblerMIPSShared::as_nop() {
  spew("nop");
  return writeInst(op_special | ff_sll);
}

// Logical operations.
BufferOffset AssemblerMIPSShared::as_and(Register rd, Register rs,
                                         Register rt) {
  spew("and    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_and).encode());
}

BufferOffset AssemblerMIPSShared::as_or(Register rd, Register rs, Register rt) {
  spew("or     %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_or).encode());
}

BufferOffset AssemblerMIPSShared::as_xor(Register rd, Register rs,
                                         Register rt) {
  spew("xor    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_xor).encode());
}

BufferOffset AssemblerMIPSShared::as_nor(Register rd, Register rs,
                                         Register rt) {
  spew("nor    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_nor).encode());
}

BufferOffset AssemblerMIPSShared::as_andi(Register rd, Register rs, int32_t j) {
  MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
  spew("andi   %3s,%3s,0x%x", rd.name(), rs.name(), j);
  return writeInst(InstImm(op_andi, rs, rd, Imm16(j)).encode());
}

BufferOffset AssemblerMIPSShared::as_ori(Register rd, Register rs, int32_t j) {
  MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
  spew("ori    %3s,%3s,0x%x", rd.name(), rs.name(), j);
  return writeInst(InstImm(op_ori, rs, rd, Imm16(j)).encode());
}

BufferOffset AssemblerMIPSShared::as_xori(Register rd, Register rs, int32_t j) {
  MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
  spew("xori   %3s,%3s,0x%x", rd.name(), rs.name(), j);
  return writeInst(InstImm(op_xori, rs, rd, Imm16(j)).encode());
}

BufferOffset AssemblerMIPSShared::as_lui(Register rd, int32_t j) {
  MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
  spew("lui    %3s,0x%x", rd.name(), j);
  return writeInst(InstImm(op_lui, zero, rd, Imm16(j)).encode());
}

// Branch and jump instructions
BufferOffset AssemblerMIPSShared::as_bal(BOffImm16 off) {
  spew("bal    %d", off.decode());
  BufferOffset bo =
      writeInst(InstImm(op_regimm, zero, rt_bgezal, off).encode());
  return bo;
}

BufferOffset AssemblerMIPSShared::as_b(BOffImm16 off) {
  spew("b      %d", off.decode());
  BufferOffset bo = writeInst(InstImm(op_beq, zero, zero, off).encode());
  return bo;
}

InstImm AssemblerMIPSShared::getBranchCode(JumpOrCall jumpOrCall) {
  if (jumpOrCall == BranchIsCall) {
    return InstImm(op_regimm, zero, rt_bgezal, BOffImm16(0));
  }

  return InstImm(op_beq, zero, zero, BOffImm16(0));
}

InstImm AssemblerMIPSShared::getBranchCode(Register s, Register t,
                                           Condition c) {
  MOZ_ASSERT(c == AssemblerMIPSShared::Equal ||
             c == AssemblerMIPSShared::NotEqual);
  return InstImm(c == AssemblerMIPSShared::Equal ? op_beq : op_bne, s, t,
                 BOffImm16(0));
}

InstImm AssemblerMIPSShared::getBranchCode(Register s, Condition c) {
  switch (c) {
    case AssemblerMIPSShared::Equal:
    case AssemblerMIPSShared::Zero:
    case AssemblerMIPSShared::BelowOrEqual:
      return InstImm(op_beq, s, zero, BOffImm16(0));
    case AssemblerMIPSShared::NotEqual:
    case AssemblerMIPSShared::NonZero:
    case AssemblerMIPSShared::Above:
      return InstImm(op_bne, s, zero, BOffImm16(0));
    case AssemblerMIPSShared::GreaterThan:
      return InstImm(op_bgtz, s, zero, BOffImm16(0));
    case AssemblerMIPSShared::GreaterThanOrEqual:
    case AssemblerMIPSShared::NotSigned:
      return InstImm(op_regimm, s, rt_bgez, BOffImm16(0));
    case AssemblerMIPSShared::LessThan:
    case AssemblerMIPSShared::Signed:
      return InstImm(op_regimm, s, rt_bltz, BOffImm16(0));
    case AssemblerMIPSShared::LessThanOrEqual:
      return InstImm(op_blez, s, zero, BOffImm16(0));
    default:
      MOZ_CRASH("Condition not supported.");
  }
}

InstImm AssemblerMIPSShared::getBranchCode(FloatTestKind testKind,
                                           FPConditionBit fcc) {
  MOZ_ASSERT(!(fcc && FccMask));
#ifdef MIPSR6
  RSField rsField = ((testKind == TestForTrue ? rs_t : rs_f));

  return InstImm(op_cop1, rsField, FloatRegisters::f24 << 16, BOffImm16(0));
#else
  uint32_t rtField = ((testKind == TestForTrue ? 1 : 0) | (fcc << FccShift))
                     << RTShift;

  return InstImm(op_cop1, rs_bc1, rtField, BOffImm16(0));
#endif
}

BufferOffset AssemblerMIPSShared::as_j(JOffImm26 off) {
  spew("j      0x%x", off.decode());
  BufferOffset bo = writeInst(InstJump(op_j, off).encode());
  return bo;
}
BufferOffset AssemblerMIPSShared::as_jal(JOffImm26 off) {
  spew("jal    0x%x", off.decode());
  BufferOffset bo = writeInst(InstJump(op_jal, off).encode());
  return bo;
}

BufferOffset AssemblerMIPSShared::as_jr(Register rs) {
  spew("jr     %3s", rs.name());
#ifdef MIPSR6
  BufferOffset bo =
      writeInst(InstReg(op_special, rs, zero, zero, ff_jalr).encode());
#else
  BufferOffset bo =
      writeInst(InstReg(op_special, rs, zero, zero, ff_jr).encode());
#endif
  return bo;
}
BufferOffset AssemblerMIPSShared::as_jalr(Register rs) {
  spew("jalr   %3s", rs.name());
  BufferOffset bo =
      writeInst(InstReg(op_special, rs, zero, ra, ff_jalr).encode());
  return bo;
}

// Arithmetic instructions
BufferOffset AssemblerMIPSShared::as_addu(Register rd, Register rs,
                                          Register rt) {
  spew("addu   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_addu).encode());
}

BufferOffset AssemblerMIPSShared::as_addiu(Register rd, Register rs,
                                           int32_t j) {
  MOZ_ASSERT(Imm16::IsInSignedRange(j));
  spew("addiu  %3s,%3s,0x%x", rd.name(), rs.name(), j);
  return writeInst(InstImm(op_addiu, rs, rd, Imm16(j)).encode());
}

BufferOffset AssemblerMIPSShared::as_daddu(Register rd, Register rs,
                                           Register rt) {
  spew("daddu  %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_daddu).encode());
}

BufferOffset AssemblerMIPSShared::as_daddiu(Register rd, Register rs,
                                            int32_t j) {
  MOZ_ASSERT(Imm16::IsInSignedRange(j));
  spew("daddiu %3s,%3s,0x%x", rd.name(), rs.name(), j);
  return writeInst(InstImm(op_daddiu, rs, rd, Imm16(j)).encode());
}

BufferOffset AssemblerMIPSShared::as_subu(Register rd, Register rs,
                                          Register rt) {
  spew("subu   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_subu).encode());
}

BufferOffset AssemblerMIPSShared::as_dsubu(Register rd, Register rs,
                                           Register rt) {
  spew("dsubu  %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_dsubu).encode());
}

BufferOffset AssemblerMIPSShared::as_mult(Register rs, Register rt) {
  spew("mult   %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, ff_mult).encode());
}

BufferOffset AssemblerMIPSShared::as_multu(Register rs, Register rt) {
  spew("multu  %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, ff_multu).encode());
}

BufferOffset AssemblerMIPSShared::as_dmult(Register rs, Register rt) {
  spew("dmult  %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, ff_dmult).encode());
}

BufferOffset AssemblerMIPSShared::as_dmultu(Register rs, Register rt) {
  spew("dmultu %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, ff_dmultu).encode());
}

BufferOffset AssemblerMIPSShared::as_div(Register rs, Register rt) {
  spew("div    %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, ff_div).encode());
}

BufferOffset AssemblerMIPSShared::as_div(Register rd, Register rs,
                                         Register rt) {
  spew("div    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x2, ff_div).encode());
}

BufferOffset AssemblerMIPSShared::as_divu(Register rs, Register rt) {
  spew("divu   %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, ff_divu).encode());
}

BufferOffset AssemblerMIPSShared::as_divu(Register rd, Register rs,
                                          Register rt) {
  spew("divu    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x2, ff_divu).encode());
}

BufferOffset AssemblerMIPSShared::as_mod(Register rd, Register rs,
                                         Register rt) {
  spew("mod    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x3, ff_mod).encode());
}

BufferOffset AssemblerMIPSShared::as_modu(Register rd, Register rs,
                                          Register rt) {
  spew("modu   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x3, ff_modu).encode());
}

BufferOffset AssemblerMIPSShared::as_ddiv(Register rs, Register rt) {
  spew("ddiv   %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, ff_ddiv).encode());
}

BufferOffset AssemblerMIPSShared::as_ddiv(Register rd, Register rs,
                                          Register rt) {
  spew("ddiv   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x2, ff_ddiv).encode());
}

BufferOffset AssemblerMIPSShared::as_ddivu(Register rs, Register rt) {
  spew("ddivu  %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, ff_ddivu).encode());
}

BufferOffset AssemblerMIPSShared::as_ddivu(Register rd, Register rs,
                                           Register rt) {
  spew("ddivu  %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x2, ff_ddivu).encode());
}

BufferOffset AssemblerMIPSShared::as_mul(Register rd, Register rs,
                                         Register rt) {
  spew("mul    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
#ifdef MIPSR6
  return writeInst(InstReg(op_special, rs, rt, rd, 0x2, ff_mul).encode());
#else
  return writeInst(InstReg(op_special2, rs, rt, rd, ff_mul).encode());
#endif
}

BufferOffset AssemblerMIPSShared::as_muh(Register rd, Register rs,
                                         Register rt) {
  spew("muh  %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x3, ff_muh).encode());
}

BufferOffset AssemblerMIPSShared::as_mulu(Register rd, Register rs,
                                          Register rt) {
  spew("mulu %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x2, ff_mulu).encode());
}

BufferOffset AssemblerMIPSShared::as_muhu(Register rd, Register rs,
                                          Register rt) {
  spew("muhu %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x3, ff_muhu).encode());
}

BufferOffset AssemblerMIPSShared::as_dmul(Register rd, Register rs,
                                          Register rt) {
  spew("dmul   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x2, ff_dmul).encode());
}

BufferOffset AssemblerMIPSShared::as_dmuh(Register rd, Register rs,
                                          Register rt) {
  spew("dmuh   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x3, ff_dmuh).encode());
}

BufferOffset AssemblerMIPSShared::as_dmulu(Register rd, Register rt,
                                           Register rs) {
  spew("dmulu   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x2, ff_dmulu).encode());
}

BufferOffset AssemblerMIPSShared::as_dmuhu(Register rd, Register rt,
                                           Register rs) {
  spew("dmuhu   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x3, ff_dmuhu).encode());
}

BufferOffset AssemblerMIPSShared::as_dmod(Register rd, Register rs,
                                          Register rt) {
  spew("dmod    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x3, ff_dmod).encode());
}

BufferOffset AssemblerMIPSShared::as_dmodu(Register rd, Register rs,
                                           Register rt) {
  spew("dmodu    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x3, ff_dmodu).encode());
}

BufferOffset AssemblerMIPSShared::as_madd(Register rs, Register rt) {
  spew("madd %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special2, rs, rt, ff_madd).encode());
}

BufferOffset AssemblerMIPSShared::as_maddu(Register rs, Register rt) {
  spew("maddu %3s,%3s", rs.name(), rt.name());
  return writeInst(InstReg(op_special2, rs, rt, ff_maddu).encode());
}

// Shift instructions
BufferOffset AssemblerMIPSShared::as_sll(Register rd, Register rt,
                                         uint16_t sa) {
  MOZ_ASSERT(sa < 32);
  spew("sll    %3s,%3s, 0x%x", rd.name(), rt.name(), sa);
  return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_sll).encode());
}

BufferOffset AssemblerMIPSShared::as_dsll(Register rd, Register rt,
                                          uint16_t sa) {
  MOZ_ASSERT(sa < 32);
  spew("dsll   %3s,%3s, 0x%x", rd.name(), rt.name(), sa);
  return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_dsll).encode());
}

BufferOffset AssemblerMIPSShared::as_dsll32(Register rd, Register rt,
                                            uint16_t sa) {
  MOZ_ASSERT(31 < sa && sa < 64);
  spew("dsll32 %3s,%3s, 0x%x", rd.name(), rt.name(), sa - 32);
  return writeInst(
      InstReg(op_special, rs_zero, rt, rd, sa - 32, ff_dsll32).encode());
}

BufferOffset AssemblerMIPSShared::as_sllv(Register rd, Register rt,
                                          Register rs) {
  spew("sllv   %3s,%3s,%3s", rd.name(), rt.name(), rs.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_sllv).encode());
}

BufferOffset AssemblerMIPSShared::as_dsllv(Register rd, Register rt,
                                           Register rs) {
  spew("dsllv  %3s,%3s,%3s", rd.name(), rt.name(), rs.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_dsllv).encode());
}

BufferOffset AssemblerMIPSShared::as_srl(Register rd, Register rt,
                                         uint16_t sa) {
  MOZ_ASSERT(sa < 32);
  spew("srl    %3s,%3s, 0x%x", rd.name(), rt.name(), sa);
  return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_srl).encode());
}

BufferOffset AssemblerMIPSShared::as_dsrl(Register rd, Register rt,
                                          uint16_t sa) {
  MOZ_ASSERT(sa < 32);
  spew("dsrl   %3s,%3s, 0x%x", rd.name(), rt.name(), sa);
  return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_dsrl).encode());
}

BufferOffset AssemblerMIPSShared::as_dsrl32(Register rd, Register rt,
                                            uint16_t sa) {
  MOZ_ASSERT(31 < sa && sa < 64);
  spew("dsrl32 %3s,%3s, 0x%x", rd.name(), rt.name(), sa - 32);
  return writeInst(
      InstReg(op_special, rs_zero, rt, rd, sa - 32, ff_dsrl32).encode());
}

BufferOffset AssemblerMIPSShared::as_srlv(Register rd, Register rt,
                                          Register rs) {
  spew("srlv   %3s,%3s,%3s", rd.name(), rt.name(), rs.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_srlv).encode());
}

BufferOffset AssemblerMIPSShared::as_dsrlv(Register rd, Register rt,
                                           Register rs) {
  spew("dsrlv  %3s,%3s,%3s", rd.name(), rt.name(), rs.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_dsrlv).encode());
}

BufferOffset AssemblerMIPSShared::as_sra(Register rd, Register rt,
                                         uint16_t sa) {
  MOZ_ASSERT(sa < 32);
  spew("sra    %3s,%3s, 0x%x", rd.name(), rt.name(), sa);
  return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_sra).encode());
}

BufferOffset AssemblerMIPSShared::as_dsra(Register rd, Register rt,
                                          uint16_t sa) {
  MOZ_ASSERT(sa < 32);
  spew("dsra   %3s,%3s, 0x%x", rd.name(), rt.name(), sa);
  return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_dsra).encode());
}

BufferOffset AssemblerMIPSShared::as_dsra32(Register rd, Register rt,
                                            uint16_t sa) {
  MOZ_ASSERT(31 < sa && sa < 64);
  spew("dsra32 %3s,%3s, 0x%x", rd.name(), rt.name(), sa - 32);
  return writeInst(
      InstReg(op_special, rs_zero, rt, rd, sa - 32, ff_dsra32).encode());
}

BufferOffset AssemblerMIPSShared::as_srav(Register rd, Register rt,
                                          Register rs) {
  spew("srav   %3s,%3s,%3s", rd.name(), rt.name(), rs.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_srav).encode());
}

BufferOffset AssemblerMIPSShared::as_dsrav(Register rd, Register rt,
                                           Register rs) {
  spew("dsrav  %3s,%3s,%3s", rd.name(), rt.name(), rs.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_dsrav).encode());
}

BufferOffset AssemblerMIPSShared::as_rotr(Register rd, Register rt,
                                          uint16_t sa) {
  MOZ_ASSERT(sa < 32);
  spew("rotr   %3s,%3s, 0x%x", rd.name(), rt.name(), sa);
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special, rs_one, rt, rd, sa, ff_srl).encode());
}

BufferOffset AssemblerMIPSShared::as_drotr(Register rd, Register rt,
                                           uint16_t sa) {
  MOZ_ASSERT(sa < 32);
  spew("drotr  %3s,%3s, 0x%x", rd.name(), rt.name(), sa);
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special, rs_one, rt, rd, sa, ff_dsrl).encode());
}

BufferOffset AssemblerMIPSShared::as_drotr32(Register rd, Register rt,
                                             uint16_t sa) {
  MOZ_ASSERT(31 < sa && sa < 64);
  spew("drotr32%3s,%3s, 0x%x", rd.name(), rt.name(), sa - 32);
  MOZ_ASSERT(hasR2());
  return writeInst(
      InstReg(op_special, rs_one, rt, rd, sa - 32, ff_dsrl32).encode());
}

BufferOffset AssemblerMIPSShared::as_rotrv(Register rd, Register rt,
                                           Register rs) {
  spew("rotrv  %3s,%3s,%3s", rd.name(), rt.name(), rs.name());
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special, rs, rt, rd, 1, ff_srlv).encode());
}

BufferOffset AssemblerMIPSShared::as_drotrv(Register rd, Register rt,
                                            Register rs) {
  spew("drotrv %3s,%3s,%3s", rd.name(), rt.name(), rs.name());
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special, rs, rt, rd, 1, ff_dsrlv).encode());
}

// Load and store instructions
BufferOffset AssemblerMIPSShared::as_lb(Register rd, Register rs, int16_t off) {
  spew("lb     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_lb, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_lbu(Register rd, Register rs,
                                         int16_t off) {
  spew("lbu    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_lbu, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_lh(Register rd, Register rs, int16_t off) {
  spew("lh     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_lh, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_lhu(Register rd, Register rs,
                                         int16_t off) {
  spew("lhu    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_lhu, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_lw(Register rd, Register rs, int16_t off) {
  spew("lw     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_lw, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_lwu(Register rd, Register rs,
                                         int16_t off) {
  spew("lwu    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_lwu, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_lwl(Register rd, Register rs,
                                         int16_t off) {
  spew("lwl    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_lwl, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_lwr(Register rd, Register rs,
                                         int16_t off) {
  spew("lwr    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_lwr, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_ll(Register rd, Register rs, int16_t off) {
  spew("ll     %3s, (0x%x)%2s", rd.name(), off, rs.name());
#ifdef MIPSR6
  return writeInst(InstReg(op_special3, rs, rd, ff_ll).encode());
#else
  return writeInst(InstImm(op_ll, rs, rd, Imm16(off)).encode());
#endif
}

BufferOffset AssemblerMIPSShared::as_lld(Register rd, Register rs,
                                         int16_t off) {
  spew("lld     %3s, (0x%x)%2s", rd.name(), off, rs.name());
#ifdef MIPSR6
  return writeInst(InstReg(op_special3, rs, rd, ff_lld).encode());
#else
  return writeInst(InstImm(op_lld, rs, rd, Imm16(off)).encode());
#endif
}

BufferOffset AssemblerMIPSShared::as_ld(Register rd, Register rs, int16_t off) {
  spew("ld     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_ld, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_ldl(Register rd, Register rs,
                                         int16_t off) {
  spew("ldl    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_ldl, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_ldr(Register rd, Register rs,
                                         int16_t off) {
  spew("ldr    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_ldr, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_sb(Register rd, Register rs, int16_t off) {
  spew("sb     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_sb, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_sh(Register rd, Register rs, int16_t off) {
  spew("sh     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_sh, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_sw(Register rd, Register rs, int16_t off) {
  spew("sw     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_sw, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_swl(Register rd, Register rs,
                                         int16_t off) {
  spew("swl    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_swl, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_swr(Register rd, Register rs,
                                         int16_t off) {
  spew("swr    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_swr, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_sc(Register rd, Register rs, int16_t off) {
  spew("sc     %3s, (0x%x)%2s", rd.name(), off, rs.name());
#ifdef MIPSR6
  return writeInst(InstReg(op_special3, rs, rd, ff_sc).encode());
#else
  return writeInst(InstImm(op_sc, rs, rd, Imm16(off)).encode());
#endif
}

BufferOffset AssemblerMIPSShared::as_scd(Register rd, Register rs,
                                         int16_t off) {
#ifdef MIPSR6
  return writeInst(InstReg(op_special3, rs, rd, ff_scd).encode());
#else
  spew("scd     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_scd, rs, rd, Imm16(off)).encode());
#endif
}

BufferOffset AssemblerMIPSShared::as_sd(Register rd, Register rs, int16_t off) {
  spew("sd     %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_sd, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_sdl(Register rd, Register rs,
                                         int16_t off) {
  spew("sdl    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_sdl, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_sdr(Register rd, Register rs,
                                         int16_t off) {
  spew("sdr    %3s, (0x%x)%2s", rd.name(), off, rs.name());
  return writeInst(InstImm(op_sdr, rs, rd, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_seleqz(Register rd, Register rs,
                                            Register rt) {
  spew("seleqz    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x0, ff_seleqz).encode());
}

BufferOffset AssemblerMIPSShared::as_selnez(Register rd, Register rs,
                                            Register rt) {
  spew("selnez    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, 0x0, ff_selnez).encode());
}

BufferOffset AssemblerMIPSShared::as_gslbx(Register rd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gslbx  %3s,%3s, (0x%x)%2s", rd.name(), rs.name(), off, ri.name());
  return writeInst(InstGS(op_ldc2, rs, rd, ri, Imm8(off), ff_gsxbx).encode());
}

BufferOffset AssemblerMIPSShared::as_gssbx(Register rd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gssbx  %3s,%3s, (0x%x)%2s", rd.name(), rs.name(), off, ri.name());
  return writeInst(InstGS(op_sdc2, rs, rd, ri, Imm8(off), ff_gsxbx).encode());
}

BufferOffset AssemblerMIPSShared::as_gslhx(Register rd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gslhx  %3s,%3s, (0x%x)%2s", rd.name(), rs.name(), off, ri.name());
  return writeInst(InstGS(op_ldc2, rs, rd, ri, Imm8(off), ff_gsxhx).encode());
}

BufferOffset AssemblerMIPSShared::as_gsshx(Register rd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsshx  %3s,%3s, (0x%x)%2s", rd.name(), rs.name(), off, ri.name());
  return writeInst(InstGS(op_sdc2, rs, rd, ri, Imm8(off), ff_gsxhx).encode());
}

BufferOffset AssemblerMIPSShared::as_gslwx(Register rd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gslwx  %3s,%3s, (0x%x)%2s", rd.name(), rs.name(), off, ri.name());
  return writeInst(InstGS(op_ldc2, rs, rd, ri, Imm8(off), ff_gsxwx).encode());
}

BufferOffset AssemblerMIPSShared::as_gsswx(Register rd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsswx  %3s,%3s, (0x%x)%2s", rd.name(), rs.name(), off, ri.name());
  return writeInst(InstGS(op_sdc2, rs, rd, ri, Imm8(off), ff_gsxwx).encode());
}

BufferOffset AssemblerMIPSShared::as_gsldx(Register rd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsldx  %3s,%3s, (0x%x)%2s", rd.name(), rs.name(), off, ri.name());
  return writeInst(InstGS(op_ldc2, rs, rd, ri, Imm8(off), ff_gsxdx).encode());
}

BufferOffset AssemblerMIPSShared::as_gssdx(Register rd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gssdx  %3s,%3s, (0x%x)%2s", rd.name(), rs.name(), off, ri.name());
  return writeInst(InstGS(op_sdc2, rs, rd, ri, Imm8(off), ff_gsxdx).encode());
}

BufferOffset AssemblerMIPSShared::as_gslq(Register rh, Register rl, Register rs,
                                          int16_t off) {
  MOZ_ASSERT(GSImm13::IsInRange(off));
  spew("gslq   %3s,%3s, (0x%x)%2s", rh.name(), rl.name(), off, rs.name());
  return writeInst(InstGS(op_lwc2, rs, rl, rh, GSImm13(off), ff_gsxq).encode());
}

BufferOffset AssemblerMIPSShared::as_gssq(Register rh, Register rl, Register rs,
                                          int16_t off) {
  MOZ_ASSERT(GSImm13::IsInRange(off));
  spew("gssq   %3s,%3s, (0x%x)%2s", rh.name(), rl.name(), off, rs.name());
  return writeInst(InstGS(op_swc2, rs, rl, rh, GSImm13(off), ff_gsxq).encode());
}

// Move from HI/LO register.
BufferOffset AssemblerMIPSShared::as_mfhi(Register rd) {
  spew("mfhi   %3s", rd.name());
  return writeInst(InstReg(op_special, rd, ff_mfhi).encode());
}

BufferOffset AssemblerMIPSShared::as_mflo(Register rd) {
  spew("mflo   %3s", rd.name());
  return writeInst(InstReg(op_special, rd, ff_mflo).encode());
}

// Set on less than.
BufferOffset AssemblerMIPSShared::as_slt(Register rd, Register rs,
                                         Register rt) {
  spew("slt    %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_slt).encode());
}

BufferOffset AssemblerMIPSShared::as_sltu(Register rd, Register rs,
                                          Register rt) {
  spew("sltu   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_sltu).encode());
}

BufferOffset AssemblerMIPSShared::as_slti(Register rd, Register rs, int32_t j) {
  MOZ_ASSERT(Imm16::IsInSignedRange(j));
  spew("slti   %3s,%3s, 0x%x", rd.name(), rs.name(), j);
  return writeInst(InstImm(op_slti, rs, rd, Imm16(j)).encode());
}

BufferOffset AssemblerMIPSShared::as_sltiu(Register rd, Register rs,
                                           uint32_t j) {
  MOZ_ASSERT(Imm16::IsInSignedRange(int32_t(j)));
  spew("sltiu  %3s,%3s, 0x%x", rd.name(), rs.name(), j);
  return writeInst(InstImm(op_sltiu, rs, rd, Imm16(j)).encode());
}

// Conditional move.
BufferOffset AssemblerMIPSShared::as_movz(Register rd, Register rs,
                                          Register rt) {
  spew("movz   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_movz).encode());
}

BufferOffset AssemblerMIPSShared::as_movn(Register rd, Register rs,
                                          Register rt) {
  spew("movn   %3s,%3s,%3s", rd.name(), rs.name(), rt.name());
  return writeInst(InstReg(op_special, rs, rt, rd, ff_movn).encode());
}

BufferOffset AssemblerMIPSShared::as_movt(Register rd, Register rs,
                                          uint16_t cc) {
  Register rt;
  rt = Register::FromCode((cc & 0x7) << 2 | 1);
  spew("movt   %3s,%3s, FCC%d", rd.name(), rs.name(), cc);
  return writeInst(InstReg(op_special, rs, rt, rd, ff_movci).encode());
}

BufferOffset AssemblerMIPSShared::as_movf(Register rd, Register rs,
                                          uint16_t cc) {
  Register rt;
  rt = Register::FromCode((cc & 0x7) << 2 | 0);
  spew("movf   %3s,%3s, FCC%d", rd.name(), rs.name(), cc);
  return writeInst(InstReg(op_special, rs, rt, rd, ff_movci).encode());
}

// Bit twiddling.
BufferOffset AssemblerMIPSShared::as_clz(Register rd, Register rs) {
  spew("clz    %3s,%3s", rd.name(), rs.name());
#ifdef MIPSR6
  return writeInst(InstReg(op_special, rs, 0x0, rd, 0x1, ff_clz).encode());
#else
  return writeInst(InstReg(op_special2, rs, rd, rd, ff_clz).encode());
#endif
}

BufferOffset AssemblerMIPSShared::as_dclz(Register rd, Register rs) {
  spew("dclz   %3s,%3s", rd.name(), rs.name());
#ifdef MIPSR6
  return writeInst(InstReg(op_special, rs, 0x0, rd, 0x1, ff_dclz).encode());
#else
  return writeInst(InstReg(op_special2, rs, rd, rd, ff_dclz).encode());
#endif
}

BufferOffset AssemblerMIPSShared::as_wsbh(Register rd, Register rt) {
  spew("wsbh   %3s,%3s", rd.name(), rt.name());
  return writeInst(InstReg(op_special3, zero, rt, rd, 0x2, ff_bshfl).encode());
}

BufferOffset AssemblerMIPSShared::as_dsbh(Register rd, Register rt) {
  spew("dsbh   %3s,%3s", rd.name(), rt.name());
  return writeInst(InstReg(op_special3, zero, rt, rd, 0x2, ff_dbshfl).encode());
}

BufferOffset AssemblerMIPSShared::as_dshd(Register rd, Register rt) {
  spew("dshd   %3s,%3s", rd.name(), rt.name());
  return writeInst(InstReg(op_special3, zero, rt, rd, 0x5, ff_dbshfl).encode());
}

BufferOffset AssemblerMIPSShared::as_ins(Register rt, Register rs, uint16_t pos,
                                         uint16_t size) {
  MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 &&
             pos + size <= 32);
  Register rd;
  rd = Register::FromCode(pos + size - 1);
  spew("ins    %3s,%3s, %d, %d", rt.name(), rs.name(), pos, size);
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_ins).encode());
}

BufferOffset AssemblerMIPSShared::as_dins(Register rt, Register rs,
                                          uint16_t pos, uint16_t size) {
  MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 &&
             pos + size <= 32);
  Register rd;
  rd = Register::FromCode(pos + size - 1);
  spew("dins   %3s,%3s, %d, %d", rt.name(), rs.name(), pos, size);
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_dins).encode());
}

BufferOffset AssemblerMIPSShared::as_dinsm(Register rt, Register rs,
                                           uint16_t pos, uint16_t size) {
  MOZ_ASSERT(pos < 32 && size >= 2 && size <= 64 && pos + size > 32 &&
             pos + size <= 64);
  Register rd;
  rd = Register::FromCode(pos + size - 1 - 32);
  spew("dinsm  %3s,%3s, %d, %d", rt.name(), rs.name(), pos, size);
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_dinsm).encode());
}

BufferOffset AssemblerMIPSShared::as_dinsu(Register rt, Register rs,
                                           uint16_t pos, uint16_t size) {
  MOZ_ASSERT(pos >= 32 && pos < 64 && size >= 1 && size <= 32 &&
             pos + size > 32 && pos + size <= 64);
  Register rd;
  rd = Register::FromCode(pos + size - 1 - 32);
  spew("dinsu  %3s,%3s, %d, %d", rt.name(), rs.name(), pos, size);
  MOZ_ASSERT(hasR2());
  return writeInst(
      InstReg(op_special3, rs, rt, rd, pos - 32, ff_dinsu).encode());
}

BufferOffset AssemblerMIPSShared::as_ext(Register rt, Register rs, uint16_t pos,
                                         uint16_t size) {
  MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 &&
             pos + size <= 32);
  Register rd;
  rd = Register::FromCode(size - 1);
  spew("ext    %3s,%3s, %d, %d", rt.name(), rs.name(), pos, size);
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_ext).encode());
}

// Sign extend
BufferOffset AssemblerMIPSShared::as_seb(Register rd, Register rt) {
  spew("seb    %3s,%3s", rd.name(), rt.name());
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special3, zero, rt, rd, 16, ff_bshfl).encode());
}

BufferOffset AssemblerMIPSShared::as_seh(Register rd, Register rt) {
  spew("seh    %3s,%3s", rd.name(), rt.name());
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special3, zero, rt, rd, 24, ff_bshfl).encode());
}

BufferOffset AssemblerMIPSShared::as_dext(Register rt, Register rs,
                                          uint16_t pos, uint16_t size) {
  MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 &&
             pos + size <= 63);
  Register rd;
  rd = Register::FromCode(size - 1);
  spew("dext   %3s,%3s, %d, %d", rt.name(), rs.name(), pos, size);
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_dext).encode());
}

BufferOffset AssemblerMIPSShared::as_dextm(Register rt, Register rs,
                                           uint16_t pos, uint16_t size) {
  MOZ_ASSERT(pos < 32 && size > 32 && size <= 64 && pos + size > 32 &&
             pos + size <= 64);
  Register rd;
  rd = Register::FromCode(size - 1 - 32);
  spew("dextm  %3s,%3s, %d, %d", rt.name(), rs.name(), pos, size);
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_dextm).encode());
}

BufferOffset AssemblerMIPSShared::as_dextu(Register rt, Register rs,
                                           uint16_t pos, uint16_t size) {
  MOZ_ASSERT(pos >= 32 && pos < 64 && size != 0 && size <= 32 &&
             pos + size > 32 && pos + size <= 64);
  Register rd;
  rd = Register::FromCode(size - 1);
  spew("dextu  %3s,%3s, %d, %d", rt.name(), rs.name(), pos, size);
  MOZ_ASSERT(hasR2());
  return writeInst(
      InstReg(op_special3, rs, rt, rd, pos - 32, ff_dextu).encode());
}

// FP instructions
BufferOffset AssemblerMIPSShared::as_ldc1(FloatRegister ft, Register base,
                                          int32_t off) {
  MOZ_ASSERT(Imm16::IsInSignedRange(off));
  spew("ldc1   %3s, (0x%x)%2s", ft.name(), off, base.name());
  return writeInst(InstImm(op_ldc1, base, ft, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_sdc1(FloatRegister ft, Register base,
                                          int32_t off) {
  MOZ_ASSERT(Imm16::IsInSignedRange(off));
  spew("sdc1   %3s, (0x%x)%2s", ft.name(), off, base.name());
  return writeInst(InstImm(op_sdc1, base, ft, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_lwc1(FloatRegister ft, Register base,
                                          int32_t off) {
  MOZ_ASSERT(Imm16::IsInSignedRange(off));
  spew("lwc1   %3s, (0x%x)%2s", ft.name(), off, base.name());
  return writeInst(InstImm(op_lwc1, base, ft, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_swc1(FloatRegister ft, Register base,
                                          int32_t off) {
  MOZ_ASSERT(Imm16::IsInSignedRange(off));
  spew("swc1   %3s, (0x%x)%2s", ft.name(), off, base.name());
  return writeInst(InstImm(op_swc1, base, ft, Imm16(off)).encode());
}

BufferOffset AssemblerMIPSShared::as_gsldl(FloatRegister fd, Register base,
                                           int32_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsldl  %3s, (0x%x)%2s", fd.name(), off, base.name());
  return writeInst(InstGS(op_lwc2, base, fd, Imm8(off), ff_gsxdlc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gsldr(FloatRegister fd, Register base,
                                           int32_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsldr  %3s, (0x%x)%2s", fd.name(), off, base.name());
  return writeInst(InstGS(op_lwc2, base, fd, Imm8(off), ff_gsxdrc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gssdl(FloatRegister fd, Register base,
                                           int32_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gssdl  %3s, (0x%x)%2s", fd.name(), off, base.name());
  return writeInst(InstGS(op_swc2, base, fd, Imm8(off), ff_gsxdlc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gssdr(FloatRegister fd, Register base,
                                           int32_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gssdr  %3s, (0x%x)%2s", fd.name(), off, base.name());
  return writeInst(InstGS(op_swc2, base, fd, Imm8(off), ff_gsxdrc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gslsl(FloatRegister fd, Register base,
                                           int32_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gslsl  %3s, (0x%x)%2s", fd.name(), off, base.name());
  return writeInst(InstGS(op_lwc2, base, fd, Imm8(off), ff_gsxwlc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gslsr(FloatRegister fd, Register base,
                                           int32_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gslsr  %3s, (0x%x)%2s", fd.name(), off, base.name());
  return writeInst(InstGS(op_lwc2, base, fd, Imm8(off), ff_gsxwrc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gsssl(FloatRegister fd, Register base,
                                           int32_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsssl  %3s, (0x%x)%2s", fd.name(), off, base.name());
  return writeInst(InstGS(op_swc2, base, fd, Imm8(off), ff_gsxwlc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gsssr(FloatRegister fd, Register base,
                                           int32_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsssr  %3s, (0x%x)%2s", fd.name(), off, base.name());
  return writeInst(InstGS(op_swc2, base, fd, Imm8(off), ff_gsxwrc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gslsx(FloatRegister fd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gslsx  %3s, (%3s,%3s, 0x%x)", fd.name(), rs.name(), ri.name(), off);
  return writeInst(InstGS(op_ldc2, rs, fd, ri, Imm8(off), ff_gsxwxc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gsssx(FloatRegister fd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsssx  %3s, (%3s,%3s, 0x%x)", fd.name(), rs.name(), ri.name(), off);
  return writeInst(InstGS(op_sdc2, rs, fd, ri, Imm8(off), ff_gsxwxc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gsldx(FloatRegister fd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gsldx  %3s, (%3s,%3s, 0x%x)", fd.name(), rs.name(), ri.name(), off);
  return writeInst(InstGS(op_ldc2, rs, fd, ri, Imm8(off), ff_gsxdxc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gssdx(FloatRegister fd, Register rs,
                                           Register ri, int16_t off) {
  MOZ_ASSERT(Imm8::IsInSignedRange(off));
  spew("gssdx  %3s, (%3s,%3s, 0x%x)", fd.name(), rs.name(), ri.name(), off);
  return writeInst(InstGS(op_sdc2, rs, fd, ri, Imm8(off), ff_gsxdxc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gslq(FloatRegister rh, FloatRegister rl,
                                          Register rs, int16_t off) {
  MOZ_ASSERT(GSImm13::IsInRange(off));
  spew("gslq   %3s,%3s, (0x%x)%2s", rh.name(), rl.name(), off, rs.name());
  return writeInst(
      InstGS(op_lwc2, rs, rl, rh, GSImm13(off), ff_gsxqc1).encode());
}

BufferOffset AssemblerMIPSShared::as_gssq(FloatRegister rh, FloatRegister rl,
                                          Register rs, int16_t off) {
  MOZ_ASSERT(GSImm13::IsInRange(off));
  spew("gssq   %3s,%3s, (0x%x)%2s", rh.name(), rl.name(), off, rs.name());
  return writeInst(
      InstGS(op_swc2, rs, rl, rh, GSImm13(off), ff_gsxqc1).encode());
}

BufferOffset AssemblerMIPSShared::as_movs(FloatRegister fd, FloatRegister fs) {
  spew("mov.s  %3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_mov_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_movd(FloatRegister fd, FloatRegister fs) {
  spew("mov.d  %3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_mov_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_ctc1(Register rt, FPControl fc) {
  spew("ctc1   %3s,%d", rt.name(), fc);
  return writeInst(InstReg(op_cop1, rs_ctc1, rt, (uint32_t)fc).encode());
}

BufferOffset AssemblerMIPSShared::as_cfc1(Register rt, FPControl fc) {
  spew("cfc1   %3s,%d", rt.name(), fc);
  return writeInst(InstReg(op_cop1, rs_cfc1, rt, (uint32_t)fc).encode());
}

BufferOffset AssemblerMIPSShared::as_mtc1(Register rt, FloatRegister fs) {
  spew("mtc1   %3s,%3s", rt.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_mtc1, rt, fs).encode());
}

BufferOffset AssemblerMIPSShared::as_mfc1(Register rt, FloatRegister fs) {
  spew("mfc1   %3s,%3s", rt.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_mfc1, rt, fs).encode());
}

BufferOffset AssemblerMIPSShared::as_mthc1(Register rt, FloatRegister fs) {
  spew("mthc1  %3s,%3s", rt.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_mthc1, rt, fs).encode());
}

BufferOffset AssemblerMIPSShared::as_mfhc1(Register rt, FloatRegister fs) {
  spew("mfhc1  %3s,%3s", rt.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_mfhc1, rt, fs).encode());
}

BufferOffset AssemblerMIPSShared::as_dmtc1(Register rt, FloatRegister fs) {
  spew("dmtc1  %3s,%3s", rt.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_dmtc1, rt, fs).encode());
}

BufferOffset AssemblerMIPSShared::as_dmfc1(Register rt, FloatRegister fs) {
  spew("dmfc1  %3s,%3s", rt.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_dmfc1, rt, fs).encode());
}

// FP convert instructions
BufferOffset AssemblerMIPSShared::as_ceilws(FloatRegister fd,
                                            FloatRegister fs) {
  spew("ceil.w.s%3s,%3s", fd.name(), fs.name());
  return writeInst(
      InstReg(op_cop1, rs_s, zero, fs, fd, ff_ceil_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_floorws(FloatRegister fd,
                                             FloatRegister fs) {
  spew("floor.w.s%3s,%3s", fd.name(), fs.name());
  return writeInst(
      InstReg(op_cop1, rs_s, zero, fs, fd, ff_floor_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_roundws(FloatRegister fd,
                                             FloatRegister fs) {
  spew("round.w.s%3s,%3s", fd.name(), fs.name());
  return writeInst(
      InstReg(op_cop1, rs_s, zero, fs, fd, ff_round_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_truncws(FloatRegister fd,
                                             FloatRegister fs) {
  spew("trunc.w.s%3s,%3s", fd.name(), fs.name());
  return writeInst(
      InstReg(op_cop1, rs_s, zero, fs, fd, ff_trunc_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_truncls(FloatRegister fd,
                                             FloatRegister fs) {
  spew("trunc.l.s%3s,%3s", fd.name(), fs.name());
  MOZ_ASSERT(hasR2());
  return writeInst(
      InstReg(op_cop1, rs_s, zero, fs, fd, ff_trunc_l_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_ceilwd(FloatRegister fd,
                                            FloatRegister fs) {
  spew("ceil.w.d%3s,%3s", fd.name(), fs.name());
  return writeInst(
      InstReg(op_cop1, rs_d, zero, fs, fd, ff_ceil_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_floorwd(FloatRegister fd,
                                             FloatRegister fs) {
  spew("floor.w.d%3s,%3s", fd.name(), fs.name());
  return writeInst(
      InstReg(op_cop1, rs_d, zero, fs, fd, ff_floor_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_roundwd(FloatRegister fd,
                                             FloatRegister fs) {
  spew("round.w.d%3s,%3s", fd.name(), fs.name());
  return writeInst(
      InstReg(op_cop1, rs_d, zero, fs, fd, ff_round_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_truncwd(FloatRegister fd,
                                             FloatRegister fs) {
  spew("trunc.w.d%3s,%3s", fd.name(), fs.name());
  return writeInst(
      InstReg(op_cop1, rs_d, zero, fs, fd, ff_trunc_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_truncld(FloatRegister fd,
                                             FloatRegister fs) {
  spew("trunc.l.d%3s,%3s", fd.name(), fs.name());
  MOZ_ASSERT(hasR2());
  return writeInst(
      InstReg(op_cop1, rs_d, zero, fs, fd, ff_trunc_l_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_cvtdl(FloatRegister fd, FloatRegister fs) {
  spew("cvt.d.l%3s,%3s", fd.name(), fs.name());
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_cop1, rs_l, zero, fs, fd, ff_cvt_d_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_cvtds(FloatRegister fd, FloatRegister fs) {
  spew("cvt.d.s%3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_cvt_d_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_cvtdw(FloatRegister fd, FloatRegister fs) {
  spew("cvt.d.w%3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_w, zero, fs, fd, ff_cvt_d_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_cvtsd(FloatRegister fd, FloatRegister fs) {
  spew("cvt.s.d%3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_cvt_s_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_cvtsl(FloatRegister fd, FloatRegister fs) {
  spew("cvt.s.l%3s,%3s", fd.name(), fs.name());
  MOZ_ASSERT(hasR2());
  return writeInst(InstReg(op_cop1, rs_l, zero, fs, fd, ff_cvt_s_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_cvtsw(FloatRegister fd, FloatRegister fs) {
  spew("cvt.s.w%3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_w, zero, fs, fd, ff_cvt_s_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_cvtwd(FloatRegister fd, FloatRegister fs) {
  spew("cvt.w.d%3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_cvt_w_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_cvtws(FloatRegister fd, FloatRegister fs) {
  spew("cvt.w.s%3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_cvt_w_fmt).encode());
}

// FP arithmetic instructions
BufferOffset AssemblerMIPSShared::as_adds(FloatRegister fd, FloatRegister fs,
                                          FloatRegister ft) {
  spew("add.s  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
  return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_add_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_addd(FloatRegister fd, FloatRegister fs,
                                          FloatRegister ft) {
  spew("add.d  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
  return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_add_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_subs(FloatRegister fd, FloatRegister fs,
                                          FloatRegister ft) {
  spew("sub.s  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
  return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_sub_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_subd(FloatRegister fd, FloatRegister fs,
                                          FloatRegister ft) {
  spew("sub.d  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
  return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_sub_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_abss(FloatRegister fd, FloatRegister fs) {
  spew("abs.s  %3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_abs_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_absd(FloatRegister fd, FloatRegister fs) {
  spew("abs.d  %3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_abs_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_negs(FloatRegister fd, FloatRegister fs) {
  spew("neg.s  %3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_neg_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_negd(FloatRegister fd, FloatRegister fs) {
  spew("neg.d  %3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_neg_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_muls(FloatRegister fd, FloatRegister fs,
                                          FloatRegister ft) {
  spew("mul.s  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
  return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_mul_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_muld(FloatRegister fd, FloatRegister fs,
                                          FloatRegister ft) {
  spew("mul.d  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
  return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_mul_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_divs(FloatRegister fd, FloatRegister fs,
                                          FloatRegister ft) {
  spew("div.s  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
  return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_div_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_divd(FloatRegister fd, FloatRegister fs,
                                          FloatRegister ft) {
  spew("divd.d  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
  return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_div_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_sqrts(FloatRegister fd, FloatRegister fs) {
  spew("sqrts  %3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_sqrt_fmt).encode());
}

BufferOffset AssemblerMIPSShared::as_sqrtd(FloatRegister fd, FloatRegister fs) {
  spew("sqrtd  %3s,%3s", fd.name(), fs.name());
  return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_sqrt_fmt).encode());
}

// FP compare instructions
BufferOffset AssemblerMIPSShared::as_cf(FloatFormat fmt, FloatRegister fs,
                                        FloatRegister ft, FPConditionBit fcc) {
  if (fmt == DoubleFloat) {
    spew("c.f.d  FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_d_r6, ft, fs, FloatRegisters::f24, ff_c_f_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_d, ft, fs, fcc << FccShift, ff_c_f_fmt).encode());
#endif
  } else {
    spew("c.f.s  FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_s_r6, ft, fs, FloatRegisters::f24, ff_c_f_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_s, ft, fs, fcc << FccShift, ff_c_f_fmt).encode());
#endif
  }
}

BufferOffset AssemblerMIPSShared::as_cun(FloatFormat fmt, FloatRegister fs,
                                         FloatRegister ft, FPConditionBit fcc) {
  if (fmt == DoubleFloat) {
    spew("c.un.d FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_d_r6, ft, fs, FloatRegisters::f24, ff_c_un_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_d, ft, fs, fcc << FccShift, ff_c_un_fmt).encode());
#endif
  } else {
    spew("c.un.s FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_s_r6, ft, fs, FloatRegisters::f24, ff_c_un_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_s, ft, fs, fcc << FccShift, ff_c_un_fmt).encode());
#endif
  }
}

BufferOffset AssemblerMIPSShared::as_ceq(FloatFormat fmt, FloatRegister fs,
                                         FloatRegister ft, FPConditionBit fcc) {
  if (fmt == DoubleFloat) {
    spew("c.eq.d FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_d_r6, ft, fs, FloatRegisters::f24, ff_c_eq_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_d, ft, fs, fcc << FccShift, ff_c_eq_fmt).encode());
#endif
  } else {
    spew("c.eq.s FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_s_r6, ft, fs, FloatRegisters::f24, ff_c_eq_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_s, ft, fs, fcc << FccShift, ff_c_eq_fmt).encode());
#endif
  }
}

BufferOffset AssemblerMIPSShared::as_cueq(FloatFormat fmt, FloatRegister fs,
                                          FloatRegister ft,
                                          FPConditionBit fcc) {
  if (fmt == DoubleFloat) {
    spew("c.ueq.d FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_d_r6, ft, fs, FloatRegisters::f24, ff_c_ueq_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_d, ft, fs, fcc << FccShift, ff_c_ueq_fmt).encode());
#endif
  } else {
    spew("c.ueq.s FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_s_r6, ft, fs, FloatRegisters::f24, ff_c_ueq_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_s, ft, fs, fcc << FccShift, ff_c_ueq_fmt).encode());
#endif
  }
}

BufferOffset AssemblerMIPSShared::as_colt(FloatFormat fmt, FloatRegister fs,
                                          FloatRegister ft,
                                          FPConditionBit fcc) {
  if (fmt == DoubleFloat) {
    spew("c.olt.d FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_d_r6, ft, fs, FloatRegisters::f24, ff_c_olt_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_d, ft, fs, fcc << FccShift, ff_c_olt_fmt).encode());
#endif
  } else {
    spew("c.olt.s FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_s_r6, ft, fs, FloatRegisters::f24, ff_c_olt_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_s, ft, fs, fcc << FccShift, ff_c_olt_fmt).encode());
#endif
  }
}

BufferOffset AssemblerMIPSShared::as_cult(FloatFormat fmt, FloatRegister fs,
                                          FloatRegister ft,
                                          FPConditionBit fcc) {
  if (fmt == DoubleFloat) {
    spew("c.ult.d FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_d_r6, ft, fs, FloatRegisters::f24, ff_c_ult_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_d, ft, fs, fcc << FccShift, ff_c_ult_fmt).encode());
#endif
  } else {
    spew("c.ult.s FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_s_r6, ft, fs, FloatRegisters::f24, ff_c_ult_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_s, ft, fs, fcc << FccShift, ff_c_ult_fmt).encode());
#endif
  }
}

BufferOffset AssemblerMIPSShared::as_cole(FloatFormat fmt, FloatRegister fs,
                                          FloatRegister ft,
                                          FPConditionBit fcc) {
  if (fmt == DoubleFloat) {
    spew("c.ole.d FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_d_r6, ft, fs, FloatRegisters::f24, ff_c_ole_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_d, ft, fs, fcc << FccShift, ff_c_ole_fmt).encode());
#endif
  } else {
    spew("c.ole.s FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_s_r6, ft, fs, FloatRegisters::f24, ff_c_ole_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_s, ft, fs, fcc << FccShift, ff_c_ole_fmt).encode());
#endif
  }
}

BufferOffset AssemblerMIPSShared::as_cule(FloatFormat fmt, FloatRegister fs,
                                          FloatRegister ft,
                                          FPConditionBit fcc) {
  if (fmt == DoubleFloat) {
    spew("c.ule.d FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_d_r6, ft, fs, FloatRegisters::f24, ff_c_ule_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_d, ft, fs, fcc << FccShift, ff_c_ule_fmt).encode());
#endif
  } else {
    spew("c.ule.s FCC%d,%3s,%3s", fcc, fs.name(), ft.name());
#ifdef MIPSR6
    return writeInst(
        InstReg(op_cop1, rs_s_r6, ft, fs, FloatRegisters::f24, ff_c_ule_fmt)
            .encode());
#else
    return writeInst(
        InstReg(op_cop1, rs_s, ft, fs, fcc << FccShift, ff_c_ule_fmt).encode());
#endif
  }
}

// FP conditional move.
BufferOffset AssemblerMIPSShared::as_movt(FloatFormat fmt, FloatRegister fd,
                                          FloatRegister fs,
                                          FPConditionBit fcc) {
  Register rt = Register::FromCode(fcc << 2 | 1);
  if (fmt == DoubleFloat) {
    spew("movt.d FCC%d,%3s,%3s", fcc, fd.name(), fs.name());
    return writeInst(InstReg(op_cop1, rs_d, rt, fs, fd, ff_movf_fmt).encode());
  } else {
    spew("movt.s FCC%d,%3s,%3s", fcc, fd.name(), fs.name());
    return writeInst(InstReg(op_cop1, rs_s, rt, fs, fd, ff_movf_fmt).encode());
  }
}

BufferOffset AssemblerMIPSShared::as_movf(FloatFormat fmt, FloatRegister fd,
                                          FloatRegister fs,
                                          FPConditionBit fcc) {
  Register rt = Register::FromCode(fcc << 2 | 0);
  if (fmt == DoubleFloat) {
    spew("movf.d FCC%d,%3s,%3s", fcc, fd.name(), fs.name());
    return writeInst(InstReg(op_cop1, rs_d, rt, fs, fd, ff_movf_fmt).encode());
  } else {
    spew("movf.s FCC%d,%3s,%3s", fcc, fd.name(), fs.name());
    return writeInst(InstReg(op_cop1, rs_s, rt, fs, fd, ff_movf_fmt).encode());
  }
}

BufferOffset AssemblerMIPSShared::as_movz(FloatFormat fmt, FloatRegister fd,
                                          FloatRegister fs, Register rt) {
  if (fmt == DoubleFloat) {
    spew("movz.d %3s,%3s,%3s", fd.name(), fs.name(), rt.name());
    return writeInst(InstReg(op_cop1, rs_d, rt, fs, fd, ff_movz_fmt).encode());
  } else {
    spew("movz.s %3s,%3s,%3s", fd.name(), fs.name(), rt.name());
    return writeInst(InstReg(op_cop1, rs_s, rt, fs, fd, ff_movz_fmt).encode());
  }
}

BufferOffset AssemblerMIPSShared::as_movn(FloatFormat fmt, FloatRegister fd,
                                          FloatRegister fs, Register rt) {
  if (fmt == DoubleFloat) {
    spew("movn.d %3s,%3s,%3s", fd.name(), fs.name(), rt.name());
    return writeInst(InstReg(op_cop1, rs_d, rt, fs, fd, ff_movn_fmt).encode());
  } else {
    spew("movn.s %3s,%3s,%3s", fd.name(), fs.name(), rt.name());
    return writeInst(InstReg(op_cop1, rs_s, rt, fs, fd, ff_movn_fmt).encode());
  }
}

BufferOffset AssemblerMIPSShared::as_max(FloatFormat fmt, FloatRegister fd,
                                         FloatRegister fs, FloatRegister ft) {
  if (fmt == DoubleFloat) {
    spew("max  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_max).encode());
  } else {
    spew("max  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_max).encode());
  }
}

BufferOffset AssemblerMIPSShared::as_min(FloatFormat fmt, FloatRegister fd,
                                         FloatRegister fs, FloatRegister ft) {
  if (fmt == DoubleFloat) {
    spew("min  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_min).encode());
  } else {
    spew("min  %3s,%3s,%3s", fd.name(), fs.name(), ft.name());
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_min).encode());
  }
}

BufferOffset AssemblerMIPSShared::as_tge(Register rs, Register rt,
                                         uint32_t code) {
  MOZ_ASSERT(code <= MAX_BREAK_CODE);
  spew("tge %3s,%3s,%d", rs.name(), rt.name(), code);
  return writeInst(InstReg(op_special, rs, rt, zero, code, ff_tge).encode());
}

BufferOffset AssemblerMIPSShared::as_tgeu(Register rs, Register rt,
                                          uint32_t code) {
  MOZ_ASSERT(code <= MAX_BREAK_CODE);
  spew("tgeu %3s,%3s,%d", rs.name(), rt.name(), code);
  return writeInst(InstReg(op_special, rs, rt, zero, code, ff_tgeu).encode());
}

BufferOffset AssemblerMIPSShared::as_tlt(Register rs, Register rt,
                                         uint32_t code) {
  MOZ_ASSERT(code <= MAX_BREAK_CODE);
  spew("tlt %3s,%3s,%d", rs.name(), rt.name(), code);
  return writeInst(InstReg(op_special, rs, rt, zero, code, ff_tlt).encode());
}

BufferOffset AssemblerMIPSShared::as_tltu(Register rs, Register rt,
                                          uint32_t code) {
  MOZ_ASSERT(code <= MAX_BREAK_CODE);
  spew("tltu %3s,%3s,%d", rs.name(), rt.name(), code);
  return writeInst(InstReg(op_special, rs, rt, zero, code, ff_tltu).encode());
}

BufferOffset AssemblerMIPSShared::as_teq(Register rs, Register rt,
                                         uint32_t code) {
  MOZ_ASSERT(code <= MAX_BREAK_CODE);
  spew("teq %3s,%3s,%d", rs.name(), rt.name(), code);
  return writeInst(InstReg(op_special, rs, rt, zero, code, ff_teq).encode());
}

BufferOffset AssemblerMIPSShared::as_tne(Register rs, Register rt,
                                         uint32_t code) {
  MOZ_ASSERT(code <= MAX_BREAK_CODE);
  spew("tne %3s,%3s,%d", rs.name(), rt.name(), code);
  return writeInst(InstReg(op_special, rs, rt, zero, code, ff_tne).encode());
}

void AssemblerMIPSShared::bind(Label* label, BufferOffset boff) {
  spew(".set Llabel %p", label);
  // If our caller didn't give us an explicit target to bind to
  // then we want to bind to the location of the next instruction
  BufferOffset dest = boff.assigned() ? boff : nextOffset();
  if (label->used()) {
    int32_t next;

    // A used label holds a link to branch that uses it.
    BufferOffset b(label);
    do {
      // Even a 0 offset may be invalid if we're out of memory.
      if (oom()) {
        return;
      }

      Instruction* inst = editSrc(b);

      // Second word holds a pointer to the next branch in label's chain.
      next = inst[1].encode();
      bind(reinterpret_cast<InstImm*>(inst), b.getOffset(), dest.getOffset());

      b = BufferOffset(next);
    } while (next != LabelBase::INVALID_OFFSET);
  }
  label->bind(dest.getOffset());
}

void AssemblerMIPSShared::retarget(Label* label, Label* target) {
  spew("retarget %p -> %p", label, target);
  if (label->used() && !oom()) {
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
      int32_t prev = target->offset();
      target->use(label->offset());
      inst[1].setData(prev);
    } else {
      // The target is unbound and unused.  We can just take the head of
      // the list hanging off of label, and dump that into target.
      target->use(label->offset());
    }
  }
  label->reset();
}

void dbg_break() {}
void AssemblerMIPSShared::as_break(uint32_t code) {
  MOZ_ASSERT(code <= MAX_BREAK_CODE);
  spew("break %d", code);
  writeInst(op_special | code << FunctionBits | ff_break);
}

void AssemblerMIPSShared::as_sync(uint32_t stype) {
  MOZ_ASSERT(stype <= 31);
  spew("sync %d", stype);
  writeInst(InstReg(op_special, zero, zero, zero, stype, ff_sync).encode());
}

// This just stomps over memory with 32 bits of raw data. Its purpose is to
// overwrite the call of JITed code with 32 bits worth of an offset. This will
// is only meant to function on code that has been invalidated, so it should
// be totally safe. Since that instruction will never be executed again, a
// ICache flush should not be necessary
void AssemblerMIPSShared::PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm) {
  // Raw is going to be the return address.
  uint32_t* raw = (uint32_t*)label.raw();
  // Overwrite the 4 bytes before the return address, which will
  // end up being the call instruction.
  *(raw - 1) = imm.value;
}

uint8_t* AssemblerMIPSShared::NextInstruction(uint8_t* inst_, uint32_t* count) {
  Instruction* inst = reinterpret_cast<Instruction*>(inst_);
  if (count != nullptr) {
    *count += sizeof(Instruction);
  }
  return reinterpret_cast<uint8_t*>(inst->next());
}

// Since there are no pools in MIPS implementation, this should be simple.
Instruction* Instruction::next() { return this + 1; }

InstImm AssemblerMIPSShared::invertBranch(InstImm branch,
                                          BOffImm16 skipOffset) {
  uint32_t rt = 0;
  OpcodeField op = (OpcodeField)(branch.extractOpcode() << OpcodeShift);
  switch (op) {
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
      if (rt & 0x1) {
        branch.setRT((RTField)((rt & ~0x1) << RTShift));
      } else {
        branch.setRT((RTField)((rt | 0x1) << RTShift));
      }
      return branch;
    default:
      MOZ_CRASH("Error creating long branch.");
  }
}

void AssemblerMIPSShared::ToggleToJmp(CodeLocationLabel inst_) {
  InstImm* inst = (InstImm*)inst_.raw();

  MOZ_ASSERT(inst->extractOpcode() == ((uint32_t)op_andi >> OpcodeShift));
  // We converted beq to andi, so now we restore it.
  inst->setOpcode(op_beq);
}

void AssemblerMIPSShared::ToggleToCmp(CodeLocationLabel inst_) {
  InstImm* inst = (InstImm*)inst_.raw();

  // toggledJump is allways used for short jumps.
  MOZ_ASSERT(inst->extractOpcode() == ((uint32_t)op_beq >> OpcodeShift));
  // Replace "beq $zero, $zero, offset" with "andi $zero, $zero, offset"
  inst->setOpcode(op_andi);
}

void AssemblerMIPSShared::UpdateLuiOriValue(Instruction* inst0,
                                            Instruction* inst1,
                                            uint32_t value) {
  MOZ_ASSERT(inst0->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
  MOZ_ASSERT(inst1->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  ((InstImm*)inst0)->setImm16(Imm16::Upper(Imm32(value)));
  ((InstImm*)inst1)->setImm16(Imm16::Lower(Imm32(value)));
}

#ifdef JS_JITSPEW
void AssemblerMIPSShared::decodeBranchInstAndSpew(InstImm branch) {
  OpcodeField op = (OpcodeField)(branch.extractOpcode() << OpcodeShift);
  uint32_t rt_id;
  uint32_t rs_id;
  uint32_t immi = branch.extractImm16Value();
  uint32_t fcc;
  switch (op) {
    case op_beq:
      rt_id = branch.extractRT();
      rs_id = branch.extractRS();
      spew("beq    %3s,%3s,0x%x", Registers::GetName(rs_id),
           Registers::GetName(rt_id), (int32_t(immi << 18) >> 16) + 4);
      break;
    case op_bne:
      rt_id = branch.extractRT();
      rs_id = branch.extractRS();
      spew("bne    %3s,%3s,0x%x", Registers::GetName(rs_id),
           Registers::GetName(rt_id), (int32_t(immi << 18) >> 16) + 4);
      break;
    case op_bgtz:
      rs_id = branch.extractRS();
      spew("bgt    %3s,  0,0x%x", Registers::GetName(rs_id),
           (int32_t(immi << 18) >> 16) + 4);
      break;
    case op_blez:
      rs_id = branch.extractRS();
      spew("ble    %3s,  0,0x%x", Registers::GetName(rs_id),
           (int32_t(immi << 18) >> 16) + 4);
      break;
    case op_regimm:
      rt_id = branch.extractRT();
      if (rt_id == (rt_bltz >> RTShift)) {
        rs_id = branch.extractRS();
        spew("blt   %3s,  0,0x%x", Registers::GetName(rs_id),
             (int32_t(immi << 18) >> 16) + 4);
      } else if (rt_id == (rt_bgez >> RTShift)) {
        rs_id = branch.extractRS();
        spew("bge   %3s,  0,0x%x", Registers::GetName(rs_id),
             (int32_t(immi << 18) >> 16) + 4);
      } else {
        MOZ_CRASH("Error disassemble branch.");
      }
      break;
    case op_cop1:
      MOZ_ASSERT(branch.extractRS() == rs_bc1 >> RSShift);
      rt_id = branch.extractRT();
      fcc = branch.extractBitField(FCccShift + FCccBits - 1, FCccShift);
      if (rt_id & 0x1) {
        spew("bc1t  FCC%d, 0x%x", fcc, (int32_t(immi << 18) >> 16) + 4);
      } else {
        spew("bc1f  FCC%d, 0x%x", fcc, (int32_t(immi << 18) >> 16) + 4);
      }
      break;
    default:
      MOZ_CRASH("Error disassemble branch.");
  }
}
#endif
