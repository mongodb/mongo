/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/loong64/Assembler-loong64.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include "gc/Marking.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/ExecutableAllocator.h"
#include "vm/Realm.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

// Note this is used for inter-wasm calls and may pass arguments and results
// in floating point registers even if the system ABI does not.

// TODO(loong64): Inconsistent with LoongArch's calling convention.
// LoongArch floating-point parameters calling convention:
//   The first eight floating-point parameters should be passed in f0-f7, and
//   the other floating point parameters will be passed like integer parameters.
// But we just pass the other floating-point parameters on stack here.
ABIArg ABIArgGenerator::next(MIRType type) {
  switch (type) {
    case MIRType::Int32:
    case MIRType::Int64:
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::WasmArrayData:
    case MIRType::StackResults: {
      if (intRegIndex_ == NumIntArgRegs) {
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(uintptr_t);
        break;
      }
      current_ = ABIArg(Register::FromCode(intRegIndex_ + a0.encoding()));
      intRegIndex_++;
      break;
    }
    case MIRType::Float32:
    case MIRType::Double: {
      if (floatRegIndex_ == NumFloatArgRegs) {
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(double);
        break;
      }
      current_ = ABIArg(FloatRegister(
          FloatRegisters::Encoding(floatRegIndex_ + f0.encoding()),
          type == MIRType::Double ? FloatRegisters::Double
                                  : FloatRegisters::Single));
      floatRegIndex_++;
      break;
    }
    case MIRType::Simd128: {
      MOZ_CRASH("LoongArch does not support simd yet.");
      break;
    }
    default:
      MOZ_CRASH("Unexpected argument type");
  }
  return current_;
}

// Encode a standard register when it is being used as rd, the rj, and
// an extra register(rk). These should never be called with an InvalidReg.
uint32_t js::jit::RJ(Register r) {
  MOZ_ASSERT(r != InvalidReg);
  return r.encoding() << RJShift;
}

uint32_t js::jit::RK(Register r) {
  MOZ_ASSERT(r != InvalidReg);
  return r.encoding() << RKShift;
}

uint32_t js::jit::RD(Register r) {
  MOZ_ASSERT(r != InvalidReg);
  return r.encoding() << RDShift;
}

uint32_t js::jit::FJ(FloatRegister r) { return r.encoding() << RJShift; }

uint32_t js::jit::FK(FloatRegister r) { return r.encoding() << RKShift; }

uint32_t js::jit::FD(FloatRegister r) { return r.encoding() << RDShift; }

uint32_t js::jit::FA(FloatRegister r) { return r.encoding() << FAShift; }

uint32_t js::jit::SA2(uint32_t value) {
  MOZ_ASSERT(value < 4);
  return (value & SA2Mask) << SAShift;
}

uint32_t js::jit::SA3(uint32_t value) {
  MOZ_ASSERT(value < 8);
  return (value & SA3Mask) << SAShift;
}

Register js::jit::toRK(Instruction& i) {
  return Register::FromCode(((i.encode() >> RKShift) & RKMask));
}

Register js::jit::toRJ(Instruction& i) {
  return Register::FromCode(((i.encode() >> RJShift) & RJMask));
}

Register js::jit::toRD(Instruction& i) {
  return Register::FromCode(((i.encode() >> RDShift) & RDMask));
}

Register js::jit::toR(Instruction& i) {
  return Register::FromCode(i.encode() & RegMask);
}

void InstImm::extractImm16(BOffImm16* dest) { *dest = BOffImm16(*this); }

void AssemblerLOONG64::finish() {
  MOZ_ASSERT(!isFinished);
  isFinished = true;
}

bool AssemblerLOONG64::appendRawCode(const uint8_t* code, size_t numBytes) {
  return m_buffer.appendRawCode(code, numBytes);
}

bool AssemblerLOONG64::reserve(size_t size) {
  // This buffer uses fixed-size chunks so there's no point in reserving
  // now vs. on-demand.
  return !oom();
}

bool AssemblerLOONG64::swapBuffer(wasm::Bytes& bytes) {
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

void AssemblerLOONG64::copyJumpRelocationTable(uint8_t* dest) {
  if (jumpRelocations_.length()) {
    memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
  }
}

void AssemblerLOONG64::copyDataRelocationTable(uint8_t* dest) {
  if (dataRelocations_.length()) {
    memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
  }
}

AssemblerLOONG64::Condition AssemblerLOONG64::InvertCondition(Condition cond) {
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

AssemblerLOONG64::DoubleCondition AssemblerLOONG64::InvertCondition(
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

AssemblerLOONG64::Condition AssemblerLOONG64::InvertCmpCondition(
    Condition cond) {
  switch (cond) {
    case Equal:
    case NotEqual:
      return cond;
    case LessThan:
      return GreaterThan;
    case LessThanOrEqual:
      return GreaterThanOrEqual;
    case GreaterThan:
      return LessThanOrEqual;
    case GreaterThanOrEqual:
      return LessThan;
    case Above:
      return Below;
    case AboveOrEqual:
      return BelowOrEqual;
    case Below:
      return Above;
    case BelowOrEqual:
      return AboveOrEqual;
    default:
      MOZ_CRASH("no meaningful swapped-operand condition");
  }
}

BOffImm16::BOffImm16(InstImm inst)
    : data((inst.encode() >> Imm16Shift) & Imm16Mask) {}

Instruction* BOffImm16::getDest(Instruction* src) const {
  return &src[(((int32_t)data << 16) >> 16) + 1];
}

bool AssemblerLOONG64::oom() const {
  return AssemblerShared::oom() || m_buffer.oom() || jumpRelocations_.oom() ||
         dataRelocations_.oom();
}

// Size of the instruction stream, in bytes.
size_t AssemblerLOONG64::size() const { return m_buffer.size(); }

// Size of the relocation table, in bytes.
size_t AssemblerLOONG64::jumpRelocationTableBytes() const {
  return jumpRelocations_.length();
}

size_t AssemblerLOONG64::dataRelocationTableBytes() const {
  return dataRelocations_.length();
}

// Size of the data table, in bytes.
size_t AssemblerLOONG64::bytesNeeded() const {
  return size() + jumpRelocationTableBytes() + dataRelocationTableBytes();
}

// write a blob of binary into the instruction stream
BufferOffset AssemblerLOONG64::writeInst(uint32_t x, uint32_t* dest) {
  MOZ_ASSERT(hasCreator());
  if (dest == nullptr) {
    return m_buffer.putInt(x);
  }

  WriteInstStatic(x, dest);
  return BufferOffset();
}

void AssemblerLOONG64::WriteInstStatic(uint32_t x, uint32_t* dest) {
  MOZ_ASSERT(dest != nullptr);
  *dest = x;
}

BufferOffset AssemblerLOONG64::haltingAlign(int alignment) {
  // TODO(loong64): Implement a proper halting align.
  return nopAlign(alignment);
}

BufferOffset AssemblerLOONG64::nopAlign(int alignment) {
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

// Logical operations.
BufferOffset AssemblerLOONG64::as_and(Register rd, Register rj, Register rk) {
  spew("and    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_and, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_or(Register rd, Register rj, Register rk) {
  spew("or     %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_or, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_xor(Register rd, Register rj, Register rk) {
  spew("xor    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_xor, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_nor(Register rd, Register rj, Register rk) {
  spew("nor    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_nor, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_andn(Register rd, Register rj, Register rk) {
  spew("andn    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_andn, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_orn(Register rd, Register rj, Register rk) {
  spew("orn     %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_orn, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_andi(Register rd, Register rj, int32_t ui12) {
  MOZ_ASSERT(is_uintN(ui12, 12));
  spew("andi   %3s,%3s,0x%x", rd.name(), rj.name(), ui12);
  return writeInst(InstImm(op_andi, ui12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_ori(Register rd, Register rj, int32_t ui12) {
  MOZ_ASSERT(is_uintN(ui12, 12));
  spew("ori    %3s,%3s,0x%x", rd.name(), rj.name(), ui12);
  return writeInst(InstImm(op_ori, ui12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_xori(Register rd, Register rj, int32_t ui12) {
  MOZ_ASSERT(is_uintN(ui12, 12));
  spew("xori   %3s,%3s,0x%x", rd.name(), rj.name(), ui12);
  return writeInst(InstImm(op_xori, ui12, rj, rd, 12).encode());
}

// Branch and jump instructions
BufferOffset AssemblerLOONG64::as_b(JOffImm26 off) {
  spew("b    %d", off.decode());
  return writeInst(InstJump(op_b, off).encode());
}

BufferOffset AssemblerLOONG64::as_bl(JOffImm26 off) {
  spew("bl    %d", off.decode());
  return writeInst(InstJump(op_bl, off).encode());
}

BufferOffset AssemblerLOONG64::as_jirl(Register rd, Register rj,
                                       BOffImm16 off) {
  spew("jirl   %3s, %3s, %d", rd.name(), rj.name(), off.decode());
  return writeInst(InstImm(op_jirl, off, rj, rd).encode());
}

InstImm AssemblerLOONG64::getBranchCode(JumpOrCall jumpOrCall) {
  // jirl or beq
  if (jumpOrCall == BranchIsCall) {
    return InstImm(op_jirl, BOffImm16(0), zero, ra);
  }

  return InstImm(op_beq, BOffImm16(0), zero, zero);
}

InstImm AssemblerLOONG64::getBranchCode(Register rj, Register rd, Condition c) {
  // beq, bne
  MOZ_ASSERT(c == AssemblerLOONG64::Equal || c == AssemblerLOONG64::NotEqual);
  return InstImm(c == AssemblerLOONG64::Equal ? op_beq : op_bne, BOffImm16(0),
                 rj, rd);
}

InstImm AssemblerLOONG64::getBranchCode(Register rj, Condition c) {
  // beq, bne, blt, bge
  switch (c) {
    case AssemblerLOONG64::Equal:
    case AssemblerLOONG64::Zero:
    case AssemblerLOONG64::BelowOrEqual:
      return InstImm(op_beq, BOffImm16(0), rj, zero);
    case AssemblerLOONG64::NotEqual:
    case AssemblerLOONG64::NonZero:
    case AssemblerLOONG64::Above:
      return InstImm(op_bne, BOffImm16(0), rj, zero);
    case AssemblerLOONG64::GreaterThan:
      return InstImm(op_blt, BOffImm16(0), zero, rj);
    case AssemblerLOONG64::GreaterThanOrEqual:
    case AssemblerLOONG64::NotSigned:
      return InstImm(op_bge, BOffImm16(0), rj, zero);
    case AssemblerLOONG64::LessThan:
    case AssemblerLOONG64::Signed:
      return InstImm(op_blt, BOffImm16(0), rj, zero);
    case AssemblerLOONG64::LessThanOrEqual:
      return InstImm(op_bge, BOffImm16(0), zero, rj);
    default:
      MOZ_CRASH("Condition not supported.");
  }
}

// Code semantics must conform to compareFloatingpoint
InstImm AssemblerLOONG64::getBranchCode(FPConditionBit cj) {
  return InstImm(op_bcz, 0, cj, true);  // bcnez
}

// Arithmetic instructions
BufferOffset AssemblerLOONG64::as_add_w(Register rd, Register rj, Register rk) {
  spew("add_w   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_add_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_add_d(Register rd, Register rj, Register rk) {
  spew("add_d   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_add_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_sub_w(Register rd, Register rj, Register rk) {
  spew("sub_w   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_sub_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_sub_d(Register rd, Register rj, Register rk) {
  spew("sub_d   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_sub_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_addi_w(Register rd, Register rj,
                                         int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("addi_w   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_addi_w, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_addi_d(Register rd, Register rj,
                                         int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("addi_d   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_addi_d, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_addu16i_d(Register rd, Register rj,
                                            int32_t si16) {
  MOZ_ASSERT(Imm16::IsInSignedRange(si16));
  spew("addu16i_d   %3s,%3s,0x%x", rd.name(), rj.name(), si16);
  return writeInst(InstImm(op_addu16i_d, Imm16(si16), rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_alsl_w(Register rd, Register rj, Register rk,
                                         uint32_t sa2) {
  MOZ_ASSERT(sa2 < 4);
  spew("alsl_w   %3s,%3s,0x%x", rd.name(), rj.name(), sa2);
  return writeInst(InstReg(op_alsl_w, sa2, rk, rj, rd, 2).encode());
}

BufferOffset AssemblerLOONG64::as_alsl_wu(Register rd, Register rj, Register rk,
                                          uint32_t sa2) {
  MOZ_ASSERT(sa2 < 4);
  spew("alsl_wu   %3s,%3s,0x%x", rd.name(), rj.name(), sa2);
  return writeInst(InstReg(op_alsl_wu, sa2, rk, rj, rd, 2).encode());
}

BufferOffset AssemblerLOONG64::as_alsl_d(Register rd, Register rj, Register rk,
                                         uint32_t sa2) {
  MOZ_ASSERT(sa2 < 4);
  spew("alsl_d   %3s,%3s,%3s,0x%x", rd.name(), rj.name(), rk.name(), sa2);
  return writeInst(InstReg(op_alsl_d, sa2, rk, rj, rd, 2).encode());
}

BufferOffset AssemblerLOONG64::as_lu12i_w(Register rd, int32_t si20) {
  spew("lu12i_w   %3s,0x%x", rd.name(), si20);
  return writeInst(InstImm(op_lu12i_w, si20, rd, false).encode());
}

BufferOffset AssemblerLOONG64::as_lu32i_d(Register rd, int32_t si20) {
  spew("lu32i_d   %3s,0x%x", rd.name(), si20);
  return writeInst(InstImm(op_lu32i_d, si20, rd, false).encode());
}

BufferOffset AssemblerLOONG64::as_lu52i_d(Register rd, Register rj,
                                          int32_t si12) {
  MOZ_ASSERT(is_uintN(si12, 12));
  spew("lu52i_d   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_lu52i_d, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_slt(Register rd, Register rj, Register rk) {
  spew("slt   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_slt, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_sltu(Register rd, Register rj, Register rk) {
  spew("sltu   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_sltu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_slti(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("slti   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_slti, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_sltui(Register rd, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("sltui   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_sltui, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_pcaddi(Register rd, int32_t si20) {
  spew("pcaddi   %3s,0x%x", rd.name(), si20);
  return writeInst(InstImm(op_pcaddi, si20, rd, false).encode());
}

BufferOffset AssemblerLOONG64::as_pcaddu12i(Register rd, int32_t si20) {
  spew("pcaddu12i   %3s,0x%x", rd.name(), si20);
  return writeInst(InstImm(op_pcaddu12i, si20, rd, false).encode());
}

BufferOffset AssemblerLOONG64::as_pcaddu18i(Register rd, int32_t si20) {
  spew("pcaddu18i   %3s,0x%x", rd.name(), si20);
  return writeInst(InstImm(op_pcaddu18i, si20, rd, false).encode());
}

BufferOffset AssemblerLOONG64::as_pcalau12i(Register rd, int32_t si20) {
  spew("pcalau12i   %3s,0x%x", rd.name(), si20);
  return writeInst(InstImm(op_pcalau12i, si20, rd, false).encode());
}

BufferOffset AssemblerLOONG64::as_mul_w(Register rd, Register rj, Register rk) {
  spew("mul_w   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mul_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mulh_w(Register rd, Register rj,
                                         Register rk) {
  spew("mulh_w   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mulh_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mulh_wu(Register rd, Register rj,
                                          Register rk) {
  spew("mulh_wu   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mulh_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mul_d(Register rd, Register rj, Register rk) {
  spew("mul_d   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mul_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mulh_d(Register rd, Register rj,
                                         Register rk) {
  spew("mulh_d   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mulh_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mulh_du(Register rd, Register rj,
                                          Register rk) {
  spew("mulh_du   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mulh_du, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mulw_d_w(Register rd, Register rj,
                                           Register rk) {
  spew("mulw_d_w   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mulw_d_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mulw_d_wu(Register rd, Register rj,
                                            Register rk) {
  spew("mulw_d_wu   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mulw_d_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_div_w(Register rd, Register rj, Register rk) {
  spew("div_w   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_div_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mod_w(Register rd, Register rj, Register rk) {
  spew("mod_w   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mod_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_div_wu(Register rd, Register rj,
                                         Register rk) {
  spew("div_wu   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_div_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mod_wu(Register rd, Register rj,
                                         Register rk) {
  spew("mod_wu   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mod_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_div_d(Register rd, Register rj, Register rk) {
  spew("div_d   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_div_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mod_d(Register rd, Register rj, Register rk) {
  spew("mod_d   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mod_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_div_du(Register rd, Register rj,
                                         Register rk) {
  spew("div_du   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_div_du, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_mod_du(Register rd, Register rj,
                                         Register rk) {
  spew("mod_du   %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_mod_du, rk, rj, rd).encode());
}

// Shift instructions
BufferOffset AssemblerLOONG64::as_sll_w(Register rd, Register rj, Register rk) {
  spew("sll_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_sll_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_srl_w(Register rd, Register rj, Register rk) {
  spew("srl_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_srl_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_sra_w(Register rd, Register rj, Register rk) {
  spew("sra_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_sra_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_rotr_w(Register rd, Register rj,
                                         Register rk) {
  spew("rotr_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_rotr_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_slli_w(Register rd, Register rj,
                                         int32_t ui5) {
  MOZ_ASSERT(is_uintN(ui5, 5));
  spew("slli_w   %3s,%3s,0x%x", rd.name(), rj.name(), ui5);
  return writeInst(InstImm(op_slli_w, ui5, rj, rd, 5).encode());
}

BufferOffset AssemblerLOONG64::as_srli_w(Register rd, Register rj,
                                         int32_t ui5) {
  MOZ_ASSERT(is_uintN(ui5, 5));
  spew("srli_w   %3s,%3s,0x%x", rd.name(), rj.name(), ui5);
  return writeInst(InstImm(op_srli_w, ui5, rj, rd, 5).encode());
}

BufferOffset AssemblerLOONG64::as_srai_w(Register rd, Register rj,
                                         int32_t ui5) {
  MOZ_ASSERT(is_uintN(ui5, 5));
  spew("srai_w   %3s,%3s,0x%x", rd.name(), rj.name(), ui5);
  return writeInst(InstImm(op_srai_w, ui5, rj, rd, 5).encode());
}

BufferOffset AssemblerLOONG64::as_rotri_w(Register rd, Register rj,
                                          int32_t ui5) {
  MOZ_ASSERT(is_uintN(ui5, 5));
  spew("rotri_w   %3s,%3s,0x%x", rd.name(), rj.name(), ui5);
  return writeInst(InstImm(op_rotri_w, ui5, rj, rd, 5).encode());
}

BufferOffset AssemblerLOONG64::as_sll_d(Register rd, Register rj, Register rk) {
  spew("sll_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_sll_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_srl_d(Register rd, Register rj, Register rk) {
  spew("srl_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_srl_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_sra_d(Register rd, Register rj, Register rk) {
  spew("sra_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_sra_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_rotr_d(Register rd, Register rj,
                                         Register rk) {
  spew("rotr_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_rotr_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_slli_d(Register rd, Register rj,
                                         int32_t ui6) {
  MOZ_ASSERT(is_uintN(ui6, 6));
  spew("slli_d   %3s,%3s,0x%x", rd.name(), rj.name(), ui6);
  return writeInst(InstImm(op_slli_d, ui6, rj, rd, 6).encode());
}

BufferOffset AssemblerLOONG64::as_srli_d(Register rd, Register rj,
                                         int32_t ui6) {
  MOZ_ASSERT(is_uintN(ui6, 6));
  spew("srli_d   %3s,%3s,0x%x", rd.name(), rj.name(), ui6);
  return writeInst(InstImm(op_srli_d, ui6, rj, rd, 6).encode());
}

BufferOffset AssemblerLOONG64::as_srai_d(Register rd, Register rj,
                                         int32_t ui6) {
  MOZ_ASSERT(is_uintN(ui6, 6));
  spew("srai_d   %3s,%3s,0x%x", rd.name(), rj.name(), ui6);
  return writeInst(InstImm(op_srai_d, ui6, rj, rd, 6).encode());
}

BufferOffset AssemblerLOONG64::as_rotri_d(Register rd, Register rj,
                                          int32_t ui6) {
  MOZ_ASSERT(is_uintN(ui6, 6));
  spew("rotri_d   %3s,%3s,0x%x", rd.name(), rj.name(), ui6);
  return writeInst(InstImm(op_rotri_d, ui6, rj, rd, 6).encode());
}

// Bit operation instrucitons
BufferOffset AssemblerLOONG64::as_ext_w_b(Register rd, Register rj) {
  spew("ext_w_b    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_ext_w_b, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ext_w_h(Register rd, Register rj) {
  spew("ext_w_h    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_ext_w_h, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_clo_w(Register rd, Register rj) {
  spew("clo_w    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_clo_w, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_clz_w(Register rd, Register rj) {
  spew("clz_w    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_clz_w, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_cto_w(Register rd, Register rj) {
  spew("cto_w    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_cto_w, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ctz_w(Register rd, Register rj) {
  spew("ctz_w    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_ctz_w, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_clo_d(Register rd, Register rj) {
  spew("clo_d    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_clo_d, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_clz_d(Register rd, Register rj) {
  spew("clz_d    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_clz_d, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_cto_d(Register rd, Register rj) {
  spew("cto_d    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_cto_d, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ctz_d(Register rd, Register rj) {
  spew("ctz_d    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_ctz_d, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_bytepick_w(Register rd, Register rj,
                                             Register rk, int32_t sa2) {
  MOZ_ASSERT(sa2 < 4);
  spew("bytepick_w    %3s,%3s,%3s, 0x%x", rd.name(), rj.name(), rk.name(), sa2);
  return writeInst(InstReg(op_bytepick_w, sa2, rk, rj, rd, 2).encode());
}

BufferOffset AssemblerLOONG64::as_bytepick_d(Register rd, Register rj,
                                             Register rk, int32_t sa3) {
  MOZ_ASSERT(sa3 < 8);
  spew("bytepick_d    %3s,%3s,%3s, 0x%x", rd.name(), rj.name(), rk.name(), sa3);
  return writeInst(InstReg(op_bytepick_d, sa3, rk, rj, rd, 3).encode());
}

BufferOffset AssemblerLOONG64::as_revb_2h(Register rd, Register rj) {
  spew("revb_2h    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_revb_2h, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_revb_4h(Register rd, Register rj) {
  spew("revb_4h    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_revb_4h, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_revb_2w(Register rd, Register rj) {
  spew("revb_2w    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_revb_2w, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_revb_d(Register rd, Register rj) {
  spew("revb_d    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_revb_d, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_revh_2w(Register rd, Register rj) {
  spew("revh_2w    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_revh_2w, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_revh_d(Register rd, Register rj) {
  spew("revh_d    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_revh_d, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_bitrev_4b(Register rd, Register rj) {
  spew("bitrev_4b    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_bitrev_4b, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_bitrev_8b(Register rd, Register rj) {
  spew("bitrev_8b    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_bitrev_8b, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_bitrev_w(Register rd, Register rj) {
  spew("bitrev_w    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_bitrev_w, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_bitrev_d(Register rd, Register rj) {
  spew("bitrev_d    %3s,%3s", rd.name(), rj.name());
  return writeInst(InstReg(op_bitrev_d, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_bstrins_w(Register rd, Register rj,
                                            int32_t msbw, int32_t lsbw) {
  MOZ_ASSERT(lsbw <= msbw);
  spew("bstrins_w   %3s,%3s,0x%x,0x%x", rd.name(), rj.name(), msbw, lsbw);
  return writeInst(InstImm(op_bstr_w, msbw, lsbw, rj, rd, 5).encode());
}

BufferOffset AssemblerLOONG64::as_bstrins_d(Register rd, Register rj,
                                            int32_t msbd, int32_t lsbd) {
  MOZ_ASSERT(lsbd <= msbd);
  spew("bstrins_d   %3s,%3s,0x%x,0x%x", rd.name(), rj.name(), msbd, lsbd);
  return writeInst(InstImm(op_bstrins_d, msbd, lsbd, rj, rd, 6).encode());
}

BufferOffset AssemblerLOONG64::as_bstrpick_w(Register rd, Register rj,
                                             int32_t msbw, int32_t lsbw) {
  MOZ_ASSERT(lsbw <= msbw);
  spew("bstrpick_w   %3s,%3s,0x%x,0x%x", rd.name(), rj.name(), msbw, lsbw);
  return writeInst(InstImm(op_bstr_w, msbw, lsbw, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_bstrpick_d(Register rd, Register rj,
                                             int32_t msbd, int32_t lsbd) {
  MOZ_ASSERT(lsbd <= msbd);
  spew("bstrpick_d   %3s,%3s,0x%x,0x%x", rd.name(), rj.name(), msbd, lsbd);
  return writeInst(InstImm(op_bstrpick_d, msbd, lsbd, rj, rd, 6).encode());
}

BufferOffset AssemblerLOONG64::as_maskeqz(Register rd, Register rj,
                                          Register rk) {
  spew("maskeqz    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_maskeqz, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_masknez(Register rd, Register rj,
                                          Register rk) {
  spew("masknez    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_masknez, rk, rj, rd).encode());
}

// Load and store instructions
BufferOffset AssemblerLOONG64::as_ld_b(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("ld_b   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_ld_b, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_ld_h(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("ld_h   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_ld_h, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_ld_w(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("ld_w   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_ld_w, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_ld_d(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("ld_d   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_ld_d, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_ld_bu(Register rd, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("ld_bu   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_ld_bu, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_ld_hu(Register rd, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("ld_hu   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_ld_hu, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_ld_wu(Register rd, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("ld_wu   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_ld_wu, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_st_b(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("st_b   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_st_b, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_st_h(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("st_h   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_st_h, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_st_w(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("st_w   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_st_w, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_st_d(Register rd, Register rj, int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("st_d   %3s,%3s,0x%x", rd.name(), rj.name(), si12);
  return writeInst(InstImm(op_st_d, si12, rj, rd, 12).encode());
}

BufferOffset AssemblerLOONG64::as_ldx_b(Register rd, Register rj, Register rk) {
  spew("ldx_b    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ldx_b, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ldx_h(Register rd, Register rj, Register rk) {
  spew("ldx_h    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ldx_h, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ldx_w(Register rd, Register rj, Register rk) {
  spew("ldx_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ldx_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ldx_d(Register rd, Register rj, Register rk) {
  spew("ldx_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ldx_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ldx_bu(Register rd, Register rj,
                                         Register rk) {
  spew("ldx_bu    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ldx_bu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ldx_hu(Register rd, Register rj,
                                         Register rk) {
  spew("ldx_hu    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ldx_hu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ldx_wu(Register rd, Register rj,
                                         Register rk) {
  spew("ldx_wu    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ldx_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_stx_b(Register rd, Register rj, Register rk) {
  spew("stx_b    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_stx_b, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_stx_h(Register rd, Register rj, Register rk) {
  spew("stx_h    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_stx_h, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_stx_w(Register rd, Register rj, Register rk) {
  spew("stx_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_stx_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_stx_d(Register rd, Register rj, Register rk) {
  spew("stx_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_stx_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ldptr_w(Register rd, Register rj,
                                          int32_t si14) {
  MOZ_ASSERT(is_intN(si14, 16) && ((si14 & 0x3) == 0));
  spew("ldptr_w   %3s,%3s,0x%x", rd.name(), rj.name(), si14);
  return writeInst(InstImm(op_ldptr_w, si14 >> 2, rj, rd, 14).encode());
}

BufferOffset AssemblerLOONG64::as_ldptr_d(Register rd, Register rj,
                                          int32_t si14) {
  MOZ_ASSERT(is_intN(si14, 16) && ((si14 & 0x3) == 0));
  spew("ldptr_d   %3s,%3s,0x%x", rd.name(), rj.name(), si14);
  return writeInst(InstImm(op_ldptr_d, si14 >> 2, rj, rd, 14).encode());
}

BufferOffset AssemblerLOONG64::as_stptr_w(Register rd, Register rj,
                                          int32_t si14) {
  MOZ_ASSERT(is_intN(si14, 16) && ((si14 & 0x3) == 0));
  spew("stptr_w   %3s,%3s,0x%x", rd.name(), rj.name(), si14);
  return writeInst(InstImm(op_stptr_w, si14 >> 2, rj, rd, 14).encode());
}

BufferOffset AssemblerLOONG64::as_stptr_d(Register rd, Register rj,
                                          int32_t si14) {
  MOZ_ASSERT(is_intN(si14, 16) && ((si14 & 0x3) == 0));
  spew("stptr_d   %3s,%3s,0x%x", rd.name(), rj.name(), si14);
  return writeInst(InstImm(op_stptr_d, si14 >> 2, rj, rd, 14).encode());
}

BufferOffset AssemblerLOONG64::as_preld(int32_t hint, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("preld   0x%x,%3s,0x%x", hint, rj.name(), si12);
  return writeInst(InstImm(op_preld, si12, rj, hint).encode());
}

// Atomic instructions
BufferOffset AssemblerLOONG64::as_amswap_w(Register rd, Register rj,
                                           Register rk) {
  spew("amswap_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amswap_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amswap_d(Register rd, Register rj,
                                           Register rk) {
  spew("amswap_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amswap_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amadd_w(Register rd, Register rj,
                                          Register rk) {
  spew("amadd_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amadd_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amadd_d(Register rd, Register rj,
                                          Register rk) {
  spew("amadd_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amadd_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amand_w(Register rd, Register rj,
                                          Register rk) {
  spew("amand_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amand_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amand_d(Register rd, Register rj,
                                          Register rk) {
  spew("amand_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amand_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amor_w(Register rd, Register rj,
                                         Register rk) {
  spew("amor_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amor_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amor_d(Register rd, Register rj,
                                         Register rk) {
  spew("amor_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amor_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amxor_w(Register rd, Register rj,
                                          Register rk) {
  spew("amxor_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amxor_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amxor_d(Register rd, Register rj,
                                          Register rk) {
  spew("amxor_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amxor_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammax_w(Register rd, Register rj,
                                          Register rk) {
  spew("ammax_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammax_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammax_d(Register rd, Register rj,
                                          Register rk) {
  spew("ammax_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammax_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammin_w(Register rd, Register rj,
                                          Register rk) {
  spew("ammin_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammin_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammin_d(Register rd, Register rj,
                                          Register rk) {
  spew("ammin_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammin_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammax_wu(Register rd, Register rj,
                                           Register rk) {
  spew("ammax_wu    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammax_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammax_du(Register rd, Register rj,
                                           Register rk) {
  spew("ammax_du    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammax_du, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammin_wu(Register rd, Register rj,
                                           Register rk) {
  spew("ammin_wu    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammin_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammin_du(Register rd, Register rj,
                                           Register rk) {
  spew("ammin_du    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammin_du, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amswap_db_w(Register rd, Register rj,
                                              Register rk) {
  spew("amswap_db_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amswap_db_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amswap_db_d(Register rd, Register rj,
                                              Register rk) {
  spew("amswap_db_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amswap_db_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amadd_db_w(Register rd, Register rj,
                                             Register rk) {
  spew("amadd_db_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amadd_db_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amadd_db_d(Register rd, Register rj,
                                             Register rk) {
  spew("amadd_db_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amadd_db_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amand_db_w(Register rd, Register rj,
                                             Register rk) {
  spew("amand_db_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amand_db_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amand_db_d(Register rd, Register rj,
                                             Register rk) {
  spew("amand_db_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amand_db_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amor_db_w(Register rd, Register rj,
                                            Register rk) {
  spew("amor_db_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amor_db_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amor_db_d(Register rd, Register rj,
                                            Register rk) {
  spew("amor_db_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amor_db_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amxor_db_w(Register rd, Register rj,
                                             Register rk) {
  spew("amxor_db_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amxor_db_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_amxor_db_d(Register rd, Register rj,
                                             Register rk) {
  spew("amxor_db_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_amxor_db_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammax_db_w(Register rd, Register rj,
                                             Register rk) {
  spew("ammax_db_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammax_db_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammax_db_d(Register rd, Register rj,
                                             Register rk) {
  spew("ammax_db_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammax_db_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammin_db_w(Register rd, Register rj,
                                             Register rk) {
  spew("ammin_db_w    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammin_db_w, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammin_db_d(Register rd, Register rj,
                                             Register rk) {
  spew("ammin_db_d    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammin_db_d, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammax_db_wu(Register rd, Register rj,
                                              Register rk) {
  spew("ammax_db_wu    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammax_db_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammax_db_du(Register rd, Register rj,
                                              Register rk) {
  spew("ammax_db_du    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammax_db_du, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammin_db_wu(Register rd, Register rj,
                                              Register rk) {
  spew("ammin_db_wu    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammin_db_wu, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ammin_db_du(Register rd, Register rj,
                                              Register rk) {
  spew("ammin_db_du    %3s,%3s,%3s", rd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_ammin_db_du, rk, rj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_ll_w(Register rd, Register rj, int32_t si14) {
  spew("ll_w   %3s,%3s,0x%x", rd.name(), rj.name(), si14);
  MOZ_ASSERT(is_intN(si14, 16) && ((si14 & 0x3) == 0));
  return writeInst(InstImm(op_ll_w, si14 >> 2, rj, rd, 14).encode());
}

BufferOffset AssemblerLOONG64::as_ll_d(Register rd, Register rj, int32_t si14) {
  spew("ll_d   %3s,%3s,0x%x", rd.name(), rj.name(), si14);
  MOZ_ASSERT(is_intN(si14, 16) && ((si14 & 0x3) == 0));
  return writeInst(InstImm(op_ll_d, si14 >> 2, rj, rd, 14).encode());
}

BufferOffset AssemblerLOONG64::as_sc_w(Register rd, Register rj, int32_t si14) {
  spew("sc_w   %3s,%3s,0x%x", rd.name(), rj.name(), si14);
  MOZ_ASSERT(is_intN(si14, 16) && ((si14 & 0x3) == 0));
  return writeInst(InstImm(op_sc_w, si14 >> 2, rj, rd, 14).encode());
}

BufferOffset AssemblerLOONG64::as_sc_d(Register rd, Register rj, int32_t si14) {
  spew("sc_d   %3s,%3s,0x%x", rd.name(), rj.name(), si14);
  MOZ_ASSERT(is_intN(si14, 16) && ((si14 & 0x3) == 0));
  return writeInst(InstImm(op_sc_d, si14 >> 2, rj, rd, 14).encode());
}

// Barrier instructions
BufferOffset AssemblerLOONG64::as_dbar(int32_t hint) {
  MOZ_ASSERT(is_uintN(hint, 15));
  spew("dbar   0x%x", hint);
  return writeInst(InstImm(op_dbar, hint).encode());
}

BufferOffset AssemblerLOONG64::as_ibar(int32_t hint) {
  MOZ_ASSERT(is_uintN(hint, 15));
  spew("ibar   0x%x", hint);
  return writeInst(InstImm(op_ibar, hint).encode());
}

/* =============================================================== */

// FP Arithmetic instructions
BufferOffset AssemblerLOONG64::as_fadd_s(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fadd_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fadd_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fadd_d(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fadd_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fadd_d, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fsub_s(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fsub_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fsub_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fsub_d(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fsub_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fsub_d, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmul_s(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fmul_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmul_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmul_d(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fmul_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmul_d, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fdiv_s(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fdiv_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fdiv_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fdiv_d(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fdiv_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fdiv_d, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmadd_s(FloatRegister fd, FloatRegister fj,
                                          FloatRegister fk, FloatRegister fa) {
  spew("fmadd_s    %3s,%3s,%3s,%3s", fd.name(), fj.name(), fk.name(),
       fa.name());
  return writeInst(InstReg(op_fmadd_s, fa, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmadd_d(FloatRegister fd, FloatRegister fj,
                                          FloatRegister fk, FloatRegister fa) {
  spew("fmadd_d    %3s,%3s,%3s,%3s", fd.name(), fj.name(), fk.name(),
       fa.name());
  return writeInst(InstReg(op_fmadd_d, fa, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmsub_s(FloatRegister fd, FloatRegister fj,
                                          FloatRegister fk, FloatRegister fa) {
  spew("fmsub_s    %3s,%3s,%3s,%3s", fd.name(), fj.name(), fk.name(),
       fa.name());
  return writeInst(InstReg(op_fmsub_s, fa, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmsub_d(FloatRegister fd, FloatRegister fj,
                                          FloatRegister fk, FloatRegister fa) {
  spew("fmsub_d    %3s,%3s,%3s,%3s", fd.name(), fj.name(), fk.name(),
       fa.name());
  return writeInst(InstReg(op_fmsub_d, fa, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fnmadd_s(FloatRegister fd, FloatRegister fj,
                                           FloatRegister fk, FloatRegister fa) {
  spew("fnmadd_s    %3s,%3s,%3s,%3s", fd.name(), fj.name(), fk.name(),
       fa.name());
  return writeInst(InstReg(op_fnmadd_s, fa, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fnmadd_d(FloatRegister fd, FloatRegister fj,
                                           FloatRegister fk, FloatRegister fa) {
  spew("fnmadd_d    %3s,%3s,%3s,%3s", fd.name(), fj.name(), fk.name(),
       fa.name());
  return writeInst(InstReg(op_fnmadd_d, fa, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fnmsub_s(FloatRegister fd, FloatRegister fj,
                                           FloatRegister fk, FloatRegister fa) {
  spew("fnmsub_s    %3s,%3s,%3s,%3s", fd.name(), fj.name(), fk.name(),
       fa.name());
  return writeInst(InstReg(op_fnmsub_s, fa, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fnmsub_d(FloatRegister fd, FloatRegister fj,
                                           FloatRegister fk, FloatRegister fa) {
  spew("fnmsub_d    %3s,%3s,%3s,%3s", fd.name(), fj.name(), fk.name(),
       fa.name());
  return writeInst(InstReg(op_fnmsub_d, fa, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmax_s(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fmax_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmax_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmax_d(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fmax_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmax_d, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmin_s(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fmin_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmin_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmin_d(FloatRegister fd, FloatRegister fj,
                                         FloatRegister fk) {
  spew("fmin_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmin_d, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmaxa_s(FloatRegister fd, FloatRegister fj,
                                          FloatRegister fk) {
  spew("fmaxa_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmaxa_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmaxa_d(FloatRegister fd, FloatRegister fj,
                                          FloatRegister fk) {
  spew("fmaxa_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmaxa_d, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmina_s(FloatRegister fd, FloatRegister fj,
                                          FloatRegister fk) {
  spew("fmina_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmina_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmina_d(FloatRegister fd, FloatRegister fj,
                                          FloatRegister fk) {
  spew("fmina_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fmina_d, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fabs_s(FloatRegister fd, FloatRegister fj) {
  spew("fabs_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fabs_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fabs_d(FloatRegister fd, FloatRegister fj) {
  spew("fabs_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fabs_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fneg_s(FloatRegister fd, FloatRegister fj) {
  spew("fneg_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fneg_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fneg_d(FloatRegister fd, FloatRegister fj) {
  spew("fneg_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fneg_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fsqrt_s(FloatRegister fd, FloatRegister fj) {
  spew("fsqrt_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fsqrt_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fsqrt_d(FloatRegister fd, FloatRegister fj) {
  spew("fsqrt_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fsqrt_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fcopysign_s(FloatRegister fd,
                                              FloatRegister fj,
                                              FloatRegister fk) {
  spew("fcopysign_s    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fcopysign_s, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fcopysign_d(FloatRegister fd,
                                              FloatRegister fj,
                                              FloatRegister fk) {
  spew("fcopysign_d    %3s,%3s,%3s", fd.name(), fj.name(), fk.name());
  return writeInst(InstReg(op_fcopysign_d, fk, fj, fd).encode());
}

// FP compare instructions
// fcmp.cond.s and fcmp.cond.d instructions
BufferOffset AssemblerLOONG64::as_fcmp_cor(FloatFormat fmt, FloatRegister fj,
                                           FloatRegister fk,
                                           FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_cor_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, COR, fk, fj, cd).encode());
  } else {
    spew("fcmp_cor_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, COR, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_ceq(FloatFormat fmt, FloatRegister fj,
                                           FloatRegister fk,
                                           FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_ceq_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CEQ, fk, fj, cd).encode());
  } else {
    spew("fcmp_ceq_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CEQ, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_cne(FloatFormat fmt, FloatRegister fj,
                                           FloatRegister fk,
                                           FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_cne_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CNE, fk, fj, cd).encode());
  } else {
    spew("fcmp_cne_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CNE, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_cle(FloatFormat fmt, FloatRegister fj,
                                           FloatRegister fk,
                                           FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_cle_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CLE, fk, fj, cd).encode());
  } else {
    spew("fcmp_cle_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CLE, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_clt(FloatFormat fmt, FloatRegister fj,
                                           FloatRegister fk,
                                           FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_clt_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CLT, fk, fj, cd).encode());
  } else {
    spew("fcmp_clt_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CLT, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_cun(FloatFormat fmt, FloatRegister fj,
                                           FloatRegister fk,
                                           FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_cun_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CUN, fk, fj, cd).encode());
  } else {
    spew("fcmp_cun_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CUN, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_cueq(FloatFormat fmt, FloatRegister fj,
                                            FloatRegister fk,
                                            FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_cueq_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CUEQ, fk, fj, cd).encode());
  } else {
    spew("fcmp_cueq_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CUEQ, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_cune(FloatFormat fmt, FloatRegister fj,
                                            FloatRegister fk,
                                            FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_cune_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CUNE, fk, fj, cd).encode());
  } else {
    spew("fcmp_cune_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CUNE, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_cule(FloatFormat fmt, FloatRegister fj,
                                            FloatRegister fk,
                                            FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_cule_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CULE, fk, fj, cd).encode());
  } else {
    spew("fcmp_cule_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CULE, fk, fj, cd).encode());
  }
}

BufferOffset AssemblerLOONG64::as_fcmp_cult(FloatFormat fmt, FloatRegister fj,
                                            FloatRegister fk,
                                            FPConditionBit cd) {
  if (fmt == DoubleFloat) {
    spew("fcmp_cult_d    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_d, CULT, fk, fj, cd).encode());
  } else {
    spew("fcmp_cult_s    FCC%d,%3s,%3s", cd, fj.name(), fk.name());
    return writeInst(InstReg(op_fcmp_cond_s, CULT, fk, fj, cd).encode());
  }
}

// FP conversion instructions
BufferOffset AssemblerLOONG64::as_fcvt_s_d(FloatRegister fd, FloatRegister fj) {
  spew("fcvt_s_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fcvt_s_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fcvt_d_s(FloatRegister fd, FloatRegister fj) {
  spew("fcvt_d_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fcvt_d_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ffint_s_w(FloatRegister fd,
                                            FloatRegister fj) {
  spew("ffint_s_w    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ffint_s_w, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ffint_s_l(FloatRegister fd,
                                            FloatRegister fj) {
  spew("ffint_s_l    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ffint_s_l, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ffint_d_w(FloatRegister fd,
                                            FloatRegister fj) {
  spew("ffint_d_w    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ffint_d_w, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ffint_d_l(FloatRegister fd,
                                            FloatRegister fj) {
  spew("ffint_d_l    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ffint_d_l, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftint_w_s(FloatRegister fd,
                                            FloatRegister fj) {
  spew("ftint_w_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftint_w_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftint_w_d(FloatRegister fd,
                                            FloatRegister fj) {
  spew("ftint_w_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftint_w_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftint_l_s(FloatRegister fd,
                                            FloatRegister fj) {
  spew("ftint_l_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftint_l_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftint_l_d(FloatRegister fd,
                                            FloatRegister fj) {
  spew("ftint_l_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftint_l_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrm_w_s(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrm_w_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrm_w_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrm_w_d(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrm_w_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrm_w_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrm_l_s(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrm_l_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrm_l_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrm_l_d(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrm_l_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrm_l_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrp_w_s(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrp_w_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrp_w_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrp_w_d(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrp_w_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrp_w_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrp_l_s(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrp_l_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrp_l_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrp_l_d(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrp_l_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrp_l_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrz_w_s(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrz_w_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrz_w_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrz_w_d(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrz_w_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrz_w_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrz_l_s(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrz_l_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrz_l_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrz_l_d(FloatRegister fd,
                                              FloatRegister fj) {
  spew("ftintrz_l_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrz_l_d, fj, fd).encode());
}
BufferOffset AssemblerLOONG64::as_ftintrne_w_s(FloatRegister fd,
                                               FloatRegister fj) {
  spew("ftintrne_w_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrne_w_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrne_w_d(FloatRegister fd,
                                               FloatRegister fj) {
  spew("ftintrne_w_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrne_w_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrne_l_s(FloatRegister fd,
                                               FloatRegister fj) {
  spew("ftintrne_l_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrne_l_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_ftintrne_l_d(FloatRegister fd,
                                               FloatRegister fj) {
  spew("ftintrne_l_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_ftintrne_l_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_frint_s(FloatRegister fd, FloatRegister fj) {
  spew("frint_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_frint_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_frint_d(FloatRegister fd, FloatRegister fj) {
  spew("frint_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_frint_d, fj, fd).encode());
}

// FP mov instructions
BufferOffset AssemblerLOONG64::as_fmov_s(FloatRegister fd, FloatRegister fj) {
  spew("fmov_s    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fmov_s, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fmov_d(FloatRegister fd, FloatRegister fj) {
  spew("fmov_d    %3s,%3s", fd.name(), fj.name());
  return writeInst(InstReg(op_fmov_d, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fsel(FloatRegister fd, FloatRegister fj,
                                       FloatRegister fk, FPConditionBit ca) {
  spew("fsel      %3s,%3s,%3s,%d", fd.name(), fj.name(), fk.name(), ca);
  return writeInst(InstReg(op_fsel, ca, fk, fj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_movgr2fr_w(FloatRegister fd, Register rj) {
  spew("movgr2fr_w    %3s,%3s", fd.name(), rj.name());
  return writeInst(InstReg(op_movgr2fr_w, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_movgr2fr_d(FloatRegister fd, Register rj) {
  spew("movgr2fr_d    %3s,%3s", fd.name(), rj.name());
  return writeInst(InstReg(op_movgr2fr_d, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_movgr2frh_w(FloatRegister fd, Register rj) {
  spew("movgr2frh_w    %3s,%3s", fd.name(), rj.name());
  return writeInst(InstReg(op_movgr2frh_w, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_movfr2gr_s(Register rd, FloatRegister fj) {
  spew("movfr2gr_s    %3s,%3s", rd.name(), fj.name());
  return writeInst(InstReg(op_movfr2gr_s, fj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_movfr2gr_d(Register rd, FloatRegister fj) {
  spew("movfr2gr_d    %3s,%3s", rd.name(), fj.name());
  return writeInst(InstReg(op_movfr2gr_d, fj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_movfrh2gr_s(Register rd, FloatRegister fj) {
  spew("movfrh2gr_s    %3s,%3s", rd.name(), fj.name());
  return writeInst(InstReg(op_movfrh2gr_s, fj, rd).encode());
}

BufferOffset AssemblerLOONG64::as_movgr2fcsr(Register rj) {
  spew("movgr2fcsr    %3s", rj.name());
  return writeInst(InstReg(op_movgr2fcsr, rj, FCSR).encode());
}

BufferOffset AssemblerLOONG64::as_movfcsr2gr(Register rd) {
  spew("movfcsr2gr    %3s", rd.name());
  return writeInst(InstReg(op_movfcsr2gr, FCSR, rd).encode());
}

BufferOffset AssemblerLOONG64::as_movfr2cf(FPConditionBit cd,
                                           FloatRegister fj) {
  spew("movfr2cf    %d,%3s", cd, fj.name());
  return writeInst(InstReg(op_movfr2cf, fj, cd).encode());
}

BufferOffset AssemblerLOONG64::as_movcf2fr(FloatRegister fd,
                                           FPConditionBit cj) {
  spew("movcf2fr    %3s,%d", fd.name(), cj);
  return writeInst(InstReg(op_movcf2fr, cj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_movgr2cf(FPConditionBit cd, Register rj) {
  spew("movgr2cf    %d,%3s", cd, rj.name());
  return writeInst(InstReg(op_movgr2cf, rj, cd).encode());
}

BufferOffset AssemblerLOONG64::as_movcf2gr(Register rd, FPConditionBit cj) {
  spew("movcf2gr    %3s,%d", rd.name(), cj);
  return writeInst(InstReg(op_movcf2gr, cj, rd).encode());
}

// FP load/store instructions
BufferOffset AssemblerLOONG64::as_fld_s(FloatRegister fd, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("fld_s   %3s,%3s,0x%x", fd.name(), rj.name(), si12);
  return writeInst(InstImm(op_fld_s, si12, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fld_d(FloatRegister fd, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("fld_d   %3s,%3s,0x%x", fd.name(), rj.name(), si12);
  return writeInst(InstImm(op_fld_d, si12, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fst_s(FloatRegister fd, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("fst_s   %3s,%3s,0x%x", fd.name(), rj.name(), si12);
  return writeInst(InstImm(op_fst_s, si12, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fst_d(FloatRegister fd, Register rj,
                                        int32_t si12) {
  MOZ_ASSERT(is_intN(si12, 12));
  spew("fst_d   %3s,%3s,0x%x", fd.name(), rj.name(), si12);
  return writeInst(InstImm(op_fst_d, si12, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fldx_s(FloatRegister fd, Register rj,
                                         Register rk) {
  spew("fldx_s    %3s,%3s,%3s", fd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_fldx_s, rk, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fldx_d(FloatRegister fd, Register rj,
                                         Register rk) {
  spew("fldx_d    %3s,%3s,%3s", fd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_fldx_d, rk, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fstx_s(FloatRegister fd, Register rj,
                                         Register rk) {
  spew("fstx_s    %3s,%3s,%3s", fd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_fstx_s, rk, rj, fd).encode());
}

BufferOffset AssemblerLOONG64::as_fstx_d(FloatRegister fd, Register rj,
                                         Register rk) {
  spew("fstx_d    %3s,%3s,%3s", fd.name(), rj.name(), rk.name());
  return writeInst(InstReg(op_fstx_d, rk, rj, fd).encode());
}

/* ========================================================================= */

void AssemblerLOONG64::bind(Label* label, BufferOffset boff) {
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

void AssemblerLOONG64::retarget(Label* label, Label* target) {
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

void AssemblerLOONG64::as_break(uint32_t code) {
  MOZ_ASSERT(code <= MAX_BREAK_CODE);
  spew("break %d", code);
  writeInst(InstImm(op_break, code).encode());
}

// This just stomps over memory with 32 bits of raw data. Its purpose is to
// overwrite the call of JITed code with 32 bits worth of an offset. This will
// is only meant to function on code that has been invalidated, so it should
// be totally safe. Since that instruction will never be executed again, a
// ICache flush should not be necessary
void AssemblerLOONG64::PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm) {
  // Raw is going to be the return address.
  uint32_t* raw = (uint32_t*)label.raw();
  // Overwrite the 4 bytes before the return address, which will
  // end up being the call instruction.
  *(raw - 1) = imm.value;
}

uint8_t* AssemblerLOONG64::NextInstruction(uint8_t* inst_, uint32_t* count) {
  Instruction* inst = reinterpret_cast<Instruction*>(inst_);
  if (count != nullptr) {
    *count += sizeof(Instruction);
  }
  return reinterpret_cast<uint8_t*>(inst->next());
}

void AssemblerLOONG64::ToggleToJmp(CodeLocationLabel inst_) {
  InstImm* inst = (InstImm*)inst_.raw();

  MOZ_ASSERT(inst->extractBitField(31, 26) == (uint32_t)op_addu16i_d >> 26);
  // We converted beq to addu16i_d, so now we restore it.
  inst->setOpcode(op_beq, 6);
}

void AssemblerLOONG64::ToggleToCmp(CodeLocationLabel inst_) {
  InstImm* inst = (InstImm*)inst_.raw();

  // toggledJump is allways used for short jumps.
  MOZ_ASSERT(inst->extractBitField(31, 26) == (uint32_t)op_beq >> 26);
  // Replace "beq $zero, $zero, offset" with "addu16i_d $zero, $zero, offset"
  inst->setOpcode(op_addu16i_d, 6);
}

// Since there are no pools in LoongArch64 implementation, this should be
// simple.
Instruction* Instruction::next() { return this + 1; }

InstImm AssemblerLOONG64::invertBranch(InstImm branch, BOffImm16 skipOffset) {
  uint32_t rj = 0;
  OpcodeField opcode = (OpcodeField)((branch.extractBitField(31, 26)) << 26);
  switch (opcode) {
    case op_beq:
      branch.setBOffImm16(skipOffset);
      branch.setOpcode(op_bne, 6);
      return branch;
    case op_bne:
      branch.setBOffImm16(skipOffset);
      branch.setOpcode(op_beq, 6);
      return branch;
    case op_bge:
      branch.setBOffImm16(skipOffset);
      branch.setOpcode(op_blt, 6);
      return branch;
    case op_bgeu:
      branch.setBOffImm16(skipOffset);
      branch.setOpcode(op_bltu, 6);
      return branch;
    case op_blt:
      branch.setBOffImm16(skipOffset);
      branch.setOpcode(op_bge, 6);
      return branch;
    case op_bltu:
      branch.setBOffImm16(skipOffset);
      branch.setOpcode(op_bgeu, 6);
      return branch;
    case op_beqz:
      branch.setBOffImm16(skipOffset);
      branch.setOpcode(op_bnez, 6);
      return branch;
    case op_bnez:
      branch.setBOffImm16(skipOffset);
      branch.setOpcode(op_beqz, 6);
      return branch;
    case op_bcz:
      branch.setBOffImm16(skipOffset);
      rj = branch.extractRJ();
      if (rj & 0x8) {
        branch.setRJ(rj & 0x17);
      } else {
        branch.setRJ(rj | 0x8);
      }
      return branch;
    default:
      MOZ_CRASH("Error creating long branch.");
  }
}

#ifdef JS_JITSPEW
void AssemblerLOONG64::decodeBranchInstAndSpew(InstImm branch) {
  OpcodeField opcode = (OpcodeField)((branch.extractBitField(31, 26)) << 26);
  uint32_t rd_id;
  uint32_t rj_id;
  uint32_t cj_id;
  uint32_t immi = branch.extractImm16Value();
  switch (opcode) {
    case op_beq:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("beq    0x%x,%3s,%3s", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), Registers::GetName(rd_id));
      break;
    case op_bne:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("bne    0x%x,%3s,%3s", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), Registers::GetName(rd_id));
      break;
    case op_bge:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("bge    0x%x,%3s,%3s", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), Registers::GetName(rd_id));
      break;
    case op_bgeu:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("bgeu    0x%x,%3s,%3s", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), Registers::GetName(rd_id));
      break;
    case op_blt:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("blt    0x%x,%3s,%3s", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), Registers::GetName(rd_id));
      break;
    case op_bltu:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("bltu    0x%x,%3s,%3s", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), Registers::GetName(rd_id));
      break;
    case op_beqz:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("beqz    0x%x,%3s,0x%x", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), rd_id);
      break;
    case op_bnez:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("bnez    0x%x,%3s,0x%x", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), rd_id);
      break;
    case op_bcz:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      cj_id = branch.extractBitField(CJShift + CJBits - 1, CJShift);
      if (rj_id & 0x8) {
        spew("bcnez    0x%x,FCC%d,0x%x", (int32_t(immi << 18) >> 16) + 4, cj_id,
             rd_id);
      } else {
        spew("bceqz    0x%x,FCC%d,0x%x", (int32_t(immi << 18) >> 16) + 4, cj_id,
             rd_id);
      }
      break;
    case op_jirl:
      rd_id = branch.extractRD();
      rj_id = branch.extractRJ();
      spew("beqz    0x%x,%3s,%3s", (int32_t(immi << 18) >> 16) + 4,
           Registers::GetName(rj_id), Registers::GetName(rd_id));
      break;
    default:
      MOZ_CRASH("Error disassemble branch.");
  }
}
#endif

void Assembler::executableCopy(uint8_t* buffer) {
  MOZ_ASSERT(isFinished);
  m_buffer.executableCopy(buffer);
}

uintptr_t Assembler::GetPointer(uint8_t* instPtr) {
  Instruction* inst = (Instruction*)instPtr;
  return Assembler::ExtractLoad64Value(inst);
}

static JitCode* CodeFromJump(Instruction* jump) {
  uint8_t* target = (uint8_t*)Assembler::ExtractLoad64Value(jump);
  return JitCode::FromExecutable(target);
}

void Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  while (reader.more()) {
    JitCode* child =
        CodeFromJump((Instruction*)(code->raw() + reader.readUnsigned()));
    TraceManuallyBarrieredEdge(trc, &child, "rel32");
  }
}

static void TraceOneDataRelocation(JSTracer* trc,
                                   mozilla::Maybe<AutoWritableJitCode>& awjc,
                                   JitCode* code, Instruction* inst) {
  void* ptr = (void*)Assembler::ExtractLoad64Value(inst);
  void* prior = ptr;

  // Data relocations can be for Values or for raw pointers. If a Value is
  // zero-tagged, we can trace it as if it were a raw pointer. If a Value
  // is not zero-tagged, we have to interpret it as a Value to ensure that the
  // tag bits are masked off to recover the actual pointer.
  uintptr_t word = reinterpret_cast<uintptr_t>(ptr);
  if (word >> JSVAL_TAG_SHIFT) {
    // This relocation is a Value with a non-zero tag.
    Value v = Value::fromRawBits(word);
    TraceManuallyBarrieredEdge(trc, &v, "jit-masm-value");
    ptr = (void*)v.bitsAsPunboxPointer();
  } else {
    // This relocation is a raw pointer or a Value with a zero tag.
    // No barrier needed since these are constants.
    TraceManuallyBarrieredGenericPointerEdge(
        trc, reinterpret_cast<gc::Cell**>(&ptr), "jit-masm-ptr");
  }

  if (ptr != prior) {
    if (awjc.isNothing()) {
      awjc.emplace(code);
    }
    Assembler::UpdateLoad64Value(inst, uint64_t(ptr));
  }
}

/* static */
void Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  mozilla::Maybe<AutoWritableJitCode> awjc;
  while (reader.more()) {
    size_t offset = reader.readUnsigned();
    Instruction* inst = (Instruction*)(code->raw() + offset);
    TraceOneDataRelocation(trc, awjc, code, inst);
  }
}

void Assembler::Bind(uint8_t* rawCode, const CodeLabel& label) {
  if (label.patchAt().bound()) {
    auto mode = label.linkMode();
    intptr_t offset = label.patchAt().offset();
    intptr_t target = label.target().offset();

    if (mode == CodeLabel::RawPointer) {
      *reinterpret_cast<const void**>(rawCode + offset) = rawCode + target;
    } else {
      MOZ_ASSERT(mode == CodeLabel::MoveImmediate ||
                 mode == CodeLabel::JumpImmediate);
      Instruction* inst = (Instruction*)(rawCode + offset);
      Assembler::UpdateLoad64Value(inst, (uint64_t)(rawCode + target));
    }
  }
}

void Assembler::bind(InstImm* inst, uintptr_t branch, uintptr_t target) {
  int64_t offset = target - branch;
  InstImm inst_jirl = InstImm(op_jirl, BOffImm16(0), zero, ra);
  InstImm inst_beq = InstImm(op_beq, BOffImm16(0), zero, zero);

  // If encoded offset is 4, then the jump must be short
  if (BOffImm16(inst[0]).decode() == 4) {
    MOZ_ASSERT(BOffImm16::IsInRange(offset));
    inst[0].setBOffImm16(BOffImm16(offset));
    inst[1].makeNop();  // because before set INVALID_OFFSET
    return;
  }

  // Generate the long jump for calls because return address has to be the
  // address after the reserved block.
  if (inst[0].encode() == inst_jirl.encode()) {
    addLongJump(BufferOffset(branch), BufferOffset(target));
    Assembler::WriteLoad64Instructions(inst, ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
    inst[3].makeNop();  // There are 1 nop.
    inst[4] = InstImm(op_jirl, BOffImm16(0), ScratchRegister, ra);
    return;
  }

  if (BOffImm16::IsInRange(offset)) {
    // Skip trailing nops .
    bool skipNops = (inst[0].encode() != inst_jirl.encode() &&
                     inst[0].encode() != inst_beq.encode());

    inst[0].setBOffImm16(BOffImm16(offset));
    inst[1].makeNop();

    if (skipNops) {
      inst[2] = InstImm(op_bge, BOffImm16(3 * sizeof(uint32_t)), zero, zero);
      // There are 2 nops after this
    }
    return;
  }

  if (inst[0].encode() == inst_beq.encode()) {
    // Handle long unconditional jump. Only four 4 instruction.
    addLongJump(BufferOffset(branch), BufferOffset(target));
    Assembler::WriteLoad64Instructions(inst, ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
    inst[3] = InstImm(op_jirl, BOffImm16(0), ScratchRegister, zero);
  } else {
    // Handle long conditional jump.
    inst[0] = invertBranch(inst[0], BOffImm16(5 * sizeof(uint32_t)));
    // No need for a "nop" here because we can clobber scratch.
    addLongJump(BufferOffset(branch + sizeof(uint32_t)), BufferOffset(target));
    Assembler::WriteLoad64Instructions(&inst[1], ScratchRegister,
                                       LabelBase::INVALID_OFFSET);
    inst[4] = InstImm(op_jirl, BOffImm16(0), ScratchRegister, zero);
  }
}

void Assembler::processCodeLabels(uint8_t* rawCode) {
  for (const CodeLabel& label : codeLabels_) {
    Bind(rawCode, label);
  }
}

uint32_t Assembler::PatchWrite_NearCallSize() {
  // Load an address needs 3 instructions, and a jump.
  return (3 + 1) * sizeof(uint32_t);
}

void Assembler::PatchWrite_NearCall(CodeLocationLabel start,
                                    CodeLocationLabel toCall) {
  Instruction* inst = (Instruction*)start.raw();
  uint8_t* dest = toCall.raw();

  // Overwrite whatever instruction used to be here with a call.
  // Always use long jump for two reasons:
  // - Jump has to be the same size because of PatchWrite_NearCallSize.
  // - Return address has to be at the end of replaced block.
  // Short jump wouldn't be more efficient.
  Assembler::WriteLoad64Instructions(inst, ScratchRegister, (uint64_t)dest);
  inst[3] = InstImm(op_jirl, BOffImm16(0), ScratchRegister, ra);
}

uint64_t Assembler::ExtractLoad64Value(Instruction* inst0) {
  InstImm* i0 = (InstImm*)inst0;
  InstImm* i1 = (InstImm*)i0->next();
  InstImm* i2 = (InstImm*)i1->next();
  InstImm* i3 = (InstImm*)i2->next();

  MOZ_ASSERT((i0->extractBitField(31, 25)) == ((uint32_t)op_lu12i_w >> 25));
  MOZ_ASSERT((i1->extractBitField(31, 22)) == ((uint32_t)op_ori >> 22));
  MOZ_ASSERT((i2->extractBitField(31, 25)) == ((uint32_t)op_lu32i_d >> 25));

  if ((i3->extractBitField(31, 22)) == ((uint32_t)op_lu52i_d >> 22)) {
    // Li64
    uint64_t value =
        (uint64_t(i0->extractBitField(Imm20Bits + Imm20Shift - 1, Imm20Shift))
         << 12) |
        (uint64_t(
            i1->extractBitField(Imm12Bits + Imm12Shift - 1, Imm12Shift))) |
        (uint64_t(i2->extractBitField(Imm20Bits + Imm20Shift - 1, Imm20Shift))
         << 32) |
        (uint64_t(i3->extractBitField(Imm12Bits + Imm12Shift - 1, Imm12Shift))
         << 52);
    return value;
  } else {
    // Li48
    uint64_t value =
        (uint64_t(i0->extractBitField(Imm20Bits + Imm20Shift - 1, Imm20Shift))
         << 12) |
        (uint64_t(
            i1->extractBitField(Imm12Bits + Imm12Shift - 1, Imm12Shift))) |
        (uint64_t(i2->extractBitField(Imm20Bits + Imm20Shift - 1, Imm20Shift))
         << 32);

    return uint64_t((int64_t(value) << 16) >> 16);
  }
}

void Assembler::UpdateLoad64Value(Instruction* inst0, uint64_t value) {
  // Todo: with ma_liPatchable
  InstImm* i0 = (InstImm*)inst0;
  InstImm* i1 = (InstImm*)i0->next();
  InstImm* i2 = (InstImm*)i1->next();
  InstImm* i3 = (InstImm*)i2->next();

  MOZ_ASSERT((i0->extractBitField(31, 25)) == ((uint32_t)op_lu12i_w >> 25));
  MOZ_ASSERT((i1->extractBitField(31, 22)) == ((uint32_t)op_ori >> 22));
  MOZ_ASSERT((i2->extractBitField(31, 25)) == ((uint32_t)op_lu32i_d >> 25));

  if ((i3->extractBitField(31, 22)) == ((uint32_t)op_lu52i_d >> 22)) {
    // Li64
    *i0 = InstImm(op_lu12i_w, (int32_t)((value >> 12) & 0xfffff),
                  Register::FromCode(i0->extractRD()), false);
    *i1 = InstImm(op_ori, (int32_t)(value & 0xfff),
                  Register::FromCode(i1->extractRJ()),
                  Register::FromCode(i1->extractRD()), 12);
    *i2 = InstImm(op_lu32i_d, (int32_t)((value >> 32) & 0xfffff),
                  Register::FromCode(i2->extractRD()), false);
    *i3 = InstImm(op_lu52i_d, (int32_t)((value >> 52) & 0xfff),
                  Register::FromCode(i3->extractRJ()),
                  Register::FromCode(i3->extractRD()), 12);
  } else {
    // Li48
    *i0 = InstImm(op_lu12i_w, (int32_t)((value >> 12) & 0xfffff),
                  Register::FromCode(i0->extractRD()), false);
    *i1 = InstImm(op_ori, (int32_t)(value & 0xfff),
                  Register::FromCode(i1->extractRJ()),
                  Register::FromCode(i1->extractRD()), 12);
    *i2 = InstImm(op_lu32i_d, (int32_t)((value >> 32) & 0xfffff),
                  Register::FromCode(i2->extractRD()), false);
  }
}

void Assembler::WriteLoad64Instructions(Instruction* inst0, Register reg,
                                        uint64_t value) {
  Instruction* inst1 = inst0->next();
  Instruction* inst2 = inst1->next();
  *inst0 = InstImm(op_lu12i_w, (int32_t)((value >> 12) & 0xfffff), reg, false);
  *inst1 = InstImm(op_ori, (int32_t)(value & 0xfff), reg, reg, 12);
  *inst2 = InstImm(op_lu32i_d, (int32_t)((value >> 32) & 0xfffff), reg, false);
}

void Assembler::PatchDataWithValueCheck(CodeLocationLabel label,
                                        ImmPtr newValue, ImmPtr expectedValue) {
  PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value),
                          PatchedImmPtr(expectedValue.value));
}

void Assembler::PatchDataWithValueCheck(CodeLocationLabel label,
                                        PatchedImmPtr newValue,
                                        PatchedImmPtr expectedValue) {
  Instruction* inst = (Instruction*)label.raw();

  // Extract old Value
  DebugOnly<uint64_t> value = Assembler::ExtractLoad64Value(inst);
  MOZ_ASSERT(value == uint64_t(expectedValue.value));

  // Replace with new value
  Assembler::UpdateLoad64Value(inst, uint64_t(newValue.value));
}

uint64_t Assembler::ExtractInstructionImmediate(uint8_t* code) {
  InstImm* inst = (InstImm*)code;
  return Assembler::ExtractLoad64Value(inst);
}

void Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled) {
  Instruction* inst = (Instruction*)inst_.raw();
  InstImm* i0 = (InstImm*)inst;
  InstImm* i1 = (InstImm*)i0->next();
  InstImm* i2 = (InstImm*)i1->next();
  Instruction* i3 = (Instruction*)i2->next();

  MOZ_ASSERT((i0->extractBitField(31, 25)) == ((uint32_t)op_lu12i_w >> 25));
  MOZ_ASSERT((i1->extractBitField(31, 22)) == ((uint32_t)op_ori >> 22));
  MOZ_ASSERT((i2->extractBitField(31, 25)) == ((uint32_t)op_lu32i_d >> 25));

  if (enabled) {
    MOZ_ASSERT((i3->extractBitField(31, 25)) != ((uint32_t)op_lu12i_w >> 25));
    InstImm jirl = InstImm(op_jirl, BOffImm16(0), ScratchRegister, ra);
    *i3 = jirl;
  } else {
    InstNOP nop;
    *i3 = nop;
  }
}
