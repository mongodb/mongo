// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "jit/arm64/vixl/Assembler-vixl.h"

#include <cmath>

#include "jit/arm64/vixl/MacroAssembler-vixl.h"

namespace vixl {

// CPURegList utilities.
CPURegister CPURegList::PopLowestIndex() {
  if (IsEmpty()) {
    return NoCPUReg;
  }
  int index = CountTrailingZeros(list_);
  VIXL_ASSERT((1ULL << index) & list_);
  Remove(index);
  return CPURegister(index, size_, type_);
}


CPURegister CPURegList::PopHighestIndex() {
  VIXL_ASSERT(IsValid());
  if (IsEmpty()) {
    return NoCPUReg;
  }
  int index = CountLeadingZeros(list_);
  index = kRegListSizeInBits - 1 - index;
  VIXL_ASSERT((1ULL << index) & list_);
  Remove(index);
  return CPURegister(index, size_, type_);
}


bool CPURegList::IsValid() const {
  if ((type_ == CPURegister::kRegister) ||
      (type_ == CPURegister::kVRegister)) {
    bool is_valid = true;
    // Try to create a CPURegister for each element in the list.
    for (int i = 0; i < kRegListSizeInBits; i++) {
      if (((list_ >> i) & 1) != 0) {
        is_valid &= CPURegister(i, size_, type_).IsValid();
      }
    }
    return is_valid;
  } else if (type_ == CPURegister::kNoRegister) {
    // We can't use IsEmpty here because that asserts IsValid().
    return list_ == 0;
  } else {
    return false;
  }
}


void CPURegList::RemoveCalleeSaved() {
  if (type() == CPURegister::kRegister) {
    Remove(GetCalleeSaved(RegisterSizeInBits()));
  } else if (type() == CPURegister::kVRegister) {
    Remove(GetCalleeSavedV(RegisterSizeInBits()));
  } else {
    VIXL_ASSERT(type() == CPURegister::kNoRegister);
    VIXL_ASSERT(IsEmpty());
    // The list must already be empty, so do nothing.
  }
}


CPURegList CPURegList::Union(const CPURegList& list_1,
                             const CPURegList& list_2,
                             const CPURegList& list_3) {
  return Union(list_1, Union(list_2, list_3));
}


CPURegList CPURegList::Union(const CPURegList& list_1,
                             const CPURegList& list_2,
                             const CPURegList& list_3,
                             const CPURegList& list_4) {
  return Union(Union(list_1, list_2), Union(list_3, list_4));
}


CPURegList CPURegList::Intersection(const CPURegList& list_1,
                                    const CPURegList& list_2,
                                    const CPURegList& list_3) {
  return Intersection(list_1, Intersection(list_2, list_3));
}


CPURegList CPURegList::Intersection(const CPURegList& list_1,
                                    const CPURegList& list_2,
                                    const CPURegList& list_3,
                                    const CPURegList& list_4) {
  return Intersection(Intersection(list_1, list_2),
                      Intersection(list_3, list_4));
}


CPURegList CPURegList::GetCalleeSaved(unsigned size) {
  return CPURegList(CPURegister::kRegister, size, 19, 29);
}


CPURegList CPURegList::GetCalleeSavedV(unsigned size) {
  return CPURegList(CPURegister::kVRegister, size, 8, 15);
}


CPURegList CPURegList::GetCallerSaved(unsigned size) {
  // Registers x0-x18 and lr (x30) are caller-saved.
  CPURegList list = CPURegList(CPURegister::kRegister, size, 0, 18);
  // Do not use lr directly to avoid initialisation order fiasco bugs for users.
  list.Combine(Register(30, kXRegSize));
  return list;
}


CPURegList CPURegList::GetCallerSavedV(unsigned size) {
  // Registers d0-d7 and d16-d31 are caller-saved.
  CPURegList list = CPURegList(CPURegister::kVRegister, size, 0, 7);
  list.Combine(CPURegList(CPURegister::kVRegister, size, 16, 31));
  return list;
}


const CPURegList kCalleeSaved = CPURegList::GetCalleeSaved();
const CPURegList kCalleeSavedV = CPURegList::GetCalleeSavedV();
const CPURegList kCallerSaved = CPURegList::GetCallerSaved();
const CPURegList kCallerSavedV = CPURegList::GetCallerSavedV();


// Registers.
#define WREG(n) w##n,
const Register Register::wregisters[] = {
REGISTER_CODE_LIST(WREG)
};
#undef WREG

#define XREG(n) x##n,
const Register Register::xregisters[] = {
REGISTER_CODE_LIST(XREG)
};
#undef XREG

#define BREG(n) b##n,
const VRegister VRegister::bregisters[] = {
REGISTER_CODE_LIST(BREG)
};
#undef BREG

#define HREG(n) h##n,
const VRegister VRegister::hregisters[] = {
REGISTER_CODE_LIST(HREG)
};
#undef HREG

#define SREG(n) s##n,
const VRegister VRegister::sregisters[] = {
REGISTER_CODE_LIST(SREG)
};
#undef SREG

#define DREG(n) d##n,
const VRegister VRegister::dregisters[] = {
REGISTER_CODE_LIST(DREG)
};
#undef DREG

#define QREG(n) q##n,
const VRegister VRegister::qregisters[] = {
REGISTER_CODE_LIST(QREG)
};
#undef QREG

#define VREG(n) v##n,
const VRegister VRegister::vregisters[] = {
REGISTER_CODE_LIST(VREG)
};
#undef VREG


const Register& Register::WRegFromCode(unsigned code) {
  if (code == kSPRegInternalCode) {
    return wsp;
  } else {
    VIXL_ASSERT(code < kNumberOfRegisters);
    return wregisters[code];
  }
}


const Register& Register::XRegFromCode(unsigned code) {
  if (code == kSPRegInternalCode) {
    return sp;
  } else {
    VIXL_ASSERT(code < kNumberOfRegisters);
    return xregisters[code];
  }
}


const VRegister& VRegister::BRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return bregisters[code];
}


const VRegister& VRegister::HRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return hregisters[code];
}


const VRegister& VRegister::SRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return sregisters[code];
}


const VRegister& VRegister::DRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return dregisters[code];
}


const VRegister& VRegister::QRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return qregisters[code];
}


const VRegister& VRegister::VRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return vregisters[code];
}


const Register& CPURegister::W() const {
  VIXL_ASSERT(IsValidRegister());
  return Register::WRegFromCode(code_);
}


const Register& CPURegister::X() const {
  VIXL_ASSERT(IsValidRegister());
  return Register::XRegFromCode(code_);
}


const VRegister& CPURegister::B() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::BRegFromCode(code_);
}


const VRegister& CPURegister::H() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::HRegFromCode(code_);
}


const VRegister& CPURegister::S() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::SRegFromCode(code_);
}


const VRegister& CPURegister::D() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::DRegFromCode(code_);
}


const VRegister& CPURegister::Q() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::QRegFromCode(code_);
}


const VRegister& CPURegister::V() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::VRegFromCode(code_);
}


// Operand.
Operand::Operand(int64_t immediate)
    : immediate_(immediate),
      reg_(NoReg),
      shift_(NO_SHIFT),
      extend_(NO_EXTEND),
      shift_amount_(0) {}


Operand::Operand(Register reg, Shift shift, unsigned shift_amount)
    : reg_(reg),
      shift_(shift),
      extend_(NO_EXTEND),
      shift_amount_(shift_amount) {
  VIXL_ASSERT(shift != MSL);
  VIXL_ASSERT(reg.Is64Bits() || (shift_amount < kWRegSize));
  VIXL_ASSERT(reg.Is32Bits() || (shift_amount < kXRegSize));
  VIXL_ASSERT(!reg.IsSP());
}


Operand::Operand(Register reg, Extend extend, unsigned shift_amount)
    : reg_(reg),
      shift_(NO_SHIFT),
      extend_(extend),
      shift_amount_(shift_amount) {
  VIXL_ASSERT(reg.IsValid());
  VIXL_ASSERT(shift_amount <= 4);
  VIXL_ASSERT(!reg.IsSP());

  // Extend modes SXTX and UXTX require a 64-bit register.
  VIXL_ASSERT(reg.Is64Bits() || ((extend != SXTX) && (extend != UXTX)));
}


bool Operand::IsImmediate() const {
  return reg_.Is(NoReg);
}


bool Operand::IsShiftedRegister() const {
  return reg_.IsValid() && (shift_ != NO_SHIFT);
}


bool Operand::IsExtendedRegister() const {
  return reg_.IsValid() && (extend_ != NO_EXTEND);
}


bool Operand::IsZero() const {
  if (IsImmediate()) {
    return immediate() == 0;
  } else {
    return reg().IsZero();
  }
}


Operand Operand::ToExtendedRegister() const {
  VIXL_ASSERT(IsShiftedRegister());
  VIXL_ASSERT((shift_ == LSL) && (shift_amount_ <= 4));
  return Operand(reg_, reg_.Is64Bits() ? UXTX : UXTW, shift_amount_);
}


// MemOperand
MemOperand::MemOperand(Register base, int64_t offset, AddrMode addrmode)
  : base_(base), regoffset_(NoReg), offset_(offset), addrmode_(addrmode) {
  VIXL_ASSERT(base.Is64Bits() && !base.IsZero());
}


MemOperand::MemOperand(Register base,
                       Register regoffset,
                       Extend extend,
                       unsigned shift_amount)
  : base_(base), regoffset_(regoffset), offset_(0), addrmode_(Offset),
    shift_(NO_SHIFT), extend_(extend), shift_amount_(shift_amount) {
  VIXL_ASSERT(base.Is64Bits() && !base.IsZero());
  VIXL_ASSERT(!regoffset.IsSP());
  VIXL_ASSERT((extend == UXTW) || (extend == SXTW) || (extend == SXTX));

  // SXTX extend mode requires a 64-bit offset register.
  VIXL_ASSERT(regoffset.Is64Bits() || (extend != SXTX));
}


MemOperand::MemOperand(Register base,
                       Register regoffset,
                       Shift shift,
                       unsigned shift_amount)
  : base_(base), regoffset_(regoffset), offset_(0), addrmode_(Offset),
    shift_(shift), extend_(NO_EXTEND), shift_amount_(shift_amount) {
  VIXL_ASSERT(base.Is64Bits() && !base.IsZero());
  VIXL_ASSERT(regoffset.Is64Bits() && !regoffset.IsSP());
  VIXL_ASSERT(shift == LSL);
}


MemOperand::MemOperand(Register base, const Operand& offset, AddrMode addrmode)
  : base_(base), regoffset_(NoReg), addrmode_(addrmode) {
  VIXL_ASSERT(base.Is64Bits() && !base.IsZero());

  if (offset.IsImmediate()) {
    offset_ = offset.immediate();
  } else if (offset.IsShiftedRegister()) {
    VIXL_ASSERT((addrmode == Offset) || (addrmode == PostIndex));

    regoffset_ = offset.reg();
    shift_ = offset.shift();
    shift_amount_ = offset.shift_amount();

    extend_ = NO_EXTEND;
    offset_ = 0;

    // These assertions match those in the shifted-register constructor.
    VIXL_ASSERT(regoffset_.Is64Bits() && !regoffset_.IsSP());
    VIXL_ASSERT(shift_ == LSL);
  } else {
    VIXL_ASSERT(offset.IsExtendedRegister());
    VIXL_ASSERT(addrmode == Offset);

    regoffset_ = offset.reg();
    extend_ = offset.extend();
    shift_amount_ = offset.shift_amount();

    shift_ = NO_SHIFT;
    offset_ = 0;

    // These assertions match those in the extended-register constructor.
    VIXL_ASSERT(!regoffset_.IsSP());
    VIXL_ASSERT((extend_ == UXTW) || (extend_ == SXTW) || (extend_ == SXTX));
    VIXL_ASSERT((regoffset_.Is64Bits() || (extend_ != SXTX)));
  }
}


bool MemOperand::IsImmediateOffset() const {
  return (addrmode_ == Offset) && regoffset_.Is(NoReg);
}


bool MemOperand::IsRegisterOffset() const {
  return (addrmode_ == Offset) && !regoffset_.Is(NoReg);
}


bool MemOperand::IsPreIndex() const {
  return addrmode_ == PreIndex;
}


bool MemOperand::IsPostIndex() const {
  return addrmode_ == PostIndex;
}


void MemOperand::AddOffset(int64_t offset) {
  VIXL_ASSERT(IsImmediateOffset());
  offset_ += offset;
}

static CPUFeatures InitCachedCPUFeatures() {
  CPUFeatures cpu_features = CPUFeatures::AArch64LegacyBaseline();

  // Mozilla change: always use maximally-present features.
  cpu_features.Combine(CPUFeatures::InferFromOS());

  // Mozilla change: Compile time hard-coded value from js-config.mozbuild.
#ifndef MOZ_AARCH64_JSCVT
#  error "MOZ_AARCH64_JSCVT must be defined."
#elif MOZ_AARCH64_JSCVT >= 1
  // Note, vixl backend implements the JSCVT flag as a boolean despite having 3
  // extra bits reserved for forward compatibility in the ARMv8 documentation.
  cpu_features.Combine(CPUFeatures::kJSCVT);
#endif

  return cpu_features;
}

// Assembler
Assembler::Assembler(PositionIndependentCodeOption pic)
    : pic_(pic)
{
  // Mozilla change: query cpu features once and cache result.
  static CPUFeatures cached_cpu_features = InitCachedCPUFeatures();
  cpu_features_ = cached_cpu_features;
}


// Code generation.
void Assembler::br(const Register& xn) {
  VIXL_ASSERT(xn.Is64Bits());
  Emit(BR | Rn(xn));
}


void Assembler::blr(const Register& xn) {
  VIXL_ASSERT(xn.Is64Bits());
  Emit(BLR | Rn(xn));
}


void Assembler::ret(const Register& xn) {
  VIXL_ASSERT(xn.Is64Bits());
  Emit(RET | Rn(xn));
}


void Assembler::NEONTable(const VRegister& vd,
                          const VRegister& vn,
                          const VRegister& vm,
                          NEONTableOp op) {
  VIXL_ASSERT(vd.Is16B() || vd.Is8B());
  VIXL_ASSERT(vn.Is16B());
  VIXL_ASSERT(AreSameFormat(vd, vm));
  Emit(op | (vd.IsQ() ? NEON_Q : 0) | Rm(vm) | Rn(vn) | Rd(vd));
}


void Assembler::tbl(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vm) {
  NEONTable(vd, vn, vm, NEON_TBL_1v);
}


void Assembler::tbl(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vn2,
                    const VRegister& vm) {
  USE(vn2);
  VIXL_ASSERT(AreSameFormat(vn, vn2));
  VIXL_ASSERT(vn2.code() == ((vn.code() + 1) % kNumberOfVRegisters));

  NEONTable(vd, vn, vm, NEON_TBL_2v);
}


void Assembler::tbl(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vn2,
                    const VRegister& vn3,
                    const VRegister& vm) {
  USE(vn2, vn3);
  VIXL_ASSERT(AreSameFormat(vn, vn2, vn3));
  VIXL_ASSERT(vn2.code() == ((vn.code() + 1) % kNumberOfVRegisters));
  VIXL_ASSERT(vn3.code() == ((vn.code() + 2) % kNumberOfVRegisters));

  NEONTable(vd, vn, vm, NEON_TBL_3v);
}


void Assembler::tbl(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vn2,
                    const VRegister& vn3,
                    const VRegister& vn4,
                    const VRegister& vm) {
  USE(vn2, vn3, vn4);
  VIXL_ASSERT(AreSameFormat(vn, vn2, vn3, vn4));
  VIXL_ASSERT(vn2.code() == ((vn.code() + 1) % kNumberOfVRegisters));
  VIXL_ASSERT(vn3.code() == ((vn.code() + 2) % kNumberOfVRegisters));
  VIXL_ASSERT(vn4.code() == ((vn.code() + 3) % kNumberOfVRegisters));

  NEONTable(vd, vn, vm, NEON_TBL_4v);
}


void Assembler::tbx(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vm) {
  NEONTable(vd, vn, vm, NEON_TBX_1v);
}


void Assembler::tbx(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vn2,
                    const VRegister& vm) {
  USE(vn2);
  VIXL_ASSERT(AreSameFormat(vn, vn2));
  VIXL_ASSERT(vn2.code() == ((vn.code() + 1) % kNumberOfVRegisters));

  NEONTable(vd, vn, vm, NEON_TBX_2v);
}


void Assembler::tbx(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vn2,
                    const VRegister& vn3,
                    const VRegister& vm) {
  USE(vn2, vn3);
  VIXL_ASSERT(AreSameFormat(vn, vn2, vn3));
  VIXL_ASSERT(vn2.code() == ((vn.code() + 1) % kNumberOfVRegisters));
  VIXL_ASSERT(vn3.code() == ((vn.code() + 2) % kNumberOfVRegisters));

  NEONTable(vd, vn, vm, NEON_TBX_3v);
}


void Assembler::tbx(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vn2,
                    const VRegister& vn3,
                    const VRegister& vn4,
                    const VRegister& vm) {
  USE(vn2, vn3, vn4);
  VIXL_ASSERT(AreSameFormat(vn, vn2, vn3, vn4));
  VIXL_ASSERT(vn2.code() == ((vn.code() + 1) % kNumberOfVRegisters));
  VIXL_ASSERT(vn3.code() == ((vn.code() + 2) % kNumberOfVRegisters));
  VIXL_ASSERT(vn4.code() == ((vn.code() + 3) % kNumberOfVRegisters));

  NEONTable(vd, vn, vm, NEON_TBX_4v);
}


void Assembler::add(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  AddSub(rd, rn, operand, LeaveFlags, ADD);
}


void Assembler::adds(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  AddSub(rd, rn, operand, SetFlags, ADD);
}


void Assembler::cmn(const Register& rn,
                    const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rn);
  adds(zr, rn, operand);
}


void Assembler::sub(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  AddSub(rd, rn, operand, LeaveFlags, SUB);
}


void Assembler::subs(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  AddSub(rd, rn, operand, SetFlags, SUB);
}


void Assembler::cmp(const Register& rn, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rn);
  subs(zr, rn, operand);
}


void Assembler::neg(const Register& rd, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rd);
  sub(rd, zr, operand);
}


void Assembler::negs(const Register& rd, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rd);
  subs(rd, zr, operand);
}


void Assembler::adc(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  AddSubWithCarry(rd, rn, operand, LeaveFlags, ADC);
}


void Assembler::adcs(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  AddSubWithCarry(rd, rn, operand, SetFlags, ADC);
}


void Assembler::sbc(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  AddSubWithCarry(rd, rn, operand, LeaveFlags, SBC);
}


void Assembler::sbcs(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  AddSubWithCarry(rd, rn, operand, SetFlags, SBC);
}


void Assembler::ngc(const Register& rd, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rd);
  sbc(rd, zr, operand);
}


void Assembler::ngcs(const Register& rd, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rd);
  sbcs(rd, zr, operand);
}


// Logical instructions.
void Assembler::and_(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  Logical(rd, rn, operand, AND);
}


void Assembler::bic(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, BIC);
}


void Assembler::bics(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  Logical(rd, rn, operand, BICS);
}


void Assembler::orr(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, ORR);
}


void Assembler::orn(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, ORN);
}


void Assembler::eor(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, EOR);
}


void Assembler::eon(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, EON);
}


void Assembler::lslv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == rm.size());
  Emit(SF(rd) | LSLV | Rm(rm) | Rn(rn) | Rd(rd));
}


void Assembler::lsrv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == rm.size());
  Emit(SF(rd) | LSRV | Rm(rm) | Rn(rn) | Rd(rd));
}


void Assembler::asrv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == rm.size());
  Emit(SF(rd) | ASRV | Rm(rm) | Rn(rn) | Rd(rd));
}


void Assembler::rorv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == rm.size());
  Emit(SF(rd) | RORV | Rm(rm) | Rn(rn) | Rd(rd));
}


// Bitfield operations.
void Assembler::bfm(const Register& rd,
                    const Register& rn,
                    unsigned immr,
                    unsigned imms) {
  VIXL_ASSERT(rd.size() == rn.size());
  Instr N = SF(rd) >> (kSFOffset - kBitfieldNOffset);
  Emit(SF(rd) | BFM | N |
       ImmR(immr, rd.size()) | ImmS(imms, rn.size()) | Rn(rn) | Rd(rd));
}


void Assembler::sbfm(const Register& rd,
                     const Register& rn,
                     unsigned immr,
                     unsigned imms) {
  VIXL_ASSERT(rd.Is64Bits() || rn.Is32Bits());
  Instr N = SF(rd) >> (kSFOffset - kBitfieldNOffset);
  Emit(SF(rd) | SBFM | N |
       ImmR(immr, rd.size()) | ImmS(imms, rn.size()) | Rn(rn) | Rd(rd));
}


void Assembler::ubfm(const Register& rd,
                     const Register& rn,
                     unsigned immr,
                     unsigned imms) {
  VIXL_ASSERT(rd.size() == rn.size());
  Instr N = SF(rd) >> (kSFOffset - kBitfieldNOffset);
  Emit(SF(rd) | UBFM | N |
       ImmR(immr, rd.size()) | ImmS(imms, rn.size()) | Rn(rn) | Rd(rd));
}


void Assembler::extr(const Register& rd,
                     const Register& rn,
                     const Register& rm,
                     unsigned lsb) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == rm.size());
  Instr N = SF(rd) >> (kSFOffset - kBitfieldNOffset);
  Emit(SF(rd) | EXTR | N | Rm(rm) | ImmS(lsb, rn.size()) | Rn(rn) | Rd(rd));
}


void Assembler::csel(const Register& rd,
                     const Register& rn,
                     const Register& rm,
                     Condition cond) {
  ConditionalSelect(rd, rn, rm, cond, CSEL);
}


void Assembler::csinc(const Register& rd,
                      const Register& rn,
                      const Register& rm,
                      Condition cond) {
  ConditionalSelect(rd, rn, rm, cond, CSINC);
}


void Assembler::csinv(const Register& rd,
                      const Register& rn,
                      const Register& rm,
                      Condition cond) {
  ConditionalSelect(rd, rn, rm, cond, CSINV);
}


void Assembler::csneg(const Register& rd,
                      const Register& rn,
                      const Register& rm,
                      Condition cond) {
  ConditionalSelect(rd, rn, rm, cond, CSNEG);
}


void Assembler::cset(const Register &rd, Condition cond) {
  VIXL_ASSERT((cond != al) && (cond != nv));
  Register zr = AppropriateZeroRegFor(rd);
  csinc(rd, zr, zr, InvertCondition(cond));
}


void Assembler::csetm(const Register &rd, Condition cond) {
  VIXL_ASSERT((cond != al) && (cond != nv));
  Register zr = AppropriateZeroRegFor(rd);
  csinv(rd, zr, zr, InvertCondition(cond));
}


void Assembler::cinc(const Register &rd, const Register &rn, Condition cond) {
  VIXL_ASSERT((cond != al) && (cond != nv));
  csinc(rd, rn, rn, InvertCondition(cond));
}


void Assembler::cinv(const Register &rd, const Register &rn, Condition cond) {
  VIXL_ASSERT((cond != al) && (cond != nv));
  csinv(rd, rn, rn, InvertCondition(cond));
}


void Assembler::cneg(const Register &rd, const Register &rn, Condition cond) {
  VIXL_ASSERT((cond != al) && (cond != nv));
  csneg(rd, rn, rn, InvertCondition(cond));
}


void Assembler::ConditionalSelect(const Register& rd,
                                  const Register& rn,
                                  const Register& rm,
                                  Condition cond,
                                  ConditionalSelectOp op) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == rm.size());
  Emit(SF(rd) | op | Rm(rm) | Cond(cond) | Rn(rn) | Rd(rd));
}


void Assembler::ccmn(const Register& rn,
                     const Operand& operand,
                     StatusFlags nzcv,
                     Condition cond) {
  ConditionalCompare(rn, operand, nzcv, cond, CCMN);
}


void Assembler::ccmp(const Register& rn,
                     const Operand& operand,
                     StatusFlags nzcv,
                     Condition cond) {
  ConditionalCompare(rn, operand, nzcv, cond, CCMP);
}


void Assembler::DataProcessing3Source(const Register& rd,
                     const Register& rn,
                     const Register& rm,
                     const Register& ra,
                     DataProcessing3SourceOp op) {
  Emit(SF(rd) | op | Rm(rm) | Ra(ra) | Rn(rn) | Rd(rd));
}


void Assembler::crc32b(const Register& rd,
                       const Register& rn,
                       const Register& rm) {
  VIXL_ASSERT(rd.Is32Bits() && rn.Is32Bits() && rm.Is32Bits());
  Emit(SF(rm) | Rm(rm) | CRC32B | Rn(rn) | Rd(rd));
}


void Assembler::crc32h(const Register& rd,
                       const Register& rn,
                       const Register& rm) {
  VIXL_ASSERT(rd.Is32Bits() && rn.Is32Bits() && rm.Is32Bits());
  Emit(SF(rm) | Rm(rm) | CRC32H | Rn(rn) | Rd(rd));
}


void Assembler::crc32w(const Register& rd,
                       const Register& rn,
                       const Register& rm) {
  VIXL_ASSERT(rd.Is32Bits() && rn.Is32Bits() && rm.Is32Bits());
  Emit(SF(rm) | Rm(rm) | CRC32W | Rn(rn) | Rd(rd));
}


void Assembler::crc32x(const Register& rd,
                       const Register& rn,
                       const Register& rm) {
  VIXL_ASSERT(rd.Is32Bits() && rn.Is32Bits() && rm.Is64Bits());
  Emit(SF(rm) | Rm(rm) | CRC32X | Rn(rn) | Rd(rd));
}


void Assembler::crc32cb(const Register& rd,
                        const Register& rn,
                        const Register& rm) {
  VIXL_ASSERT(rd.Is32Bits() && rn.Is32Bits() && rm.Is32Bits());
  Emit(SF(rm) | Rm(rm) | CRC32CB | Rn(rn) | Rd(rd));
}


void Assembler::crc32ch(const Register& rd,
                        const Register& rn,
                        const Register& rm) {
  VIXL_ASSERT(rd.Is32Bits() && rn.Is32Bits() && rm.Is32Bits());
  Emit(SF(rm) | Rm(rm) | CRC32CH | Rn(rn) | Rd(rd));
}


void Assembler::crc32cw(const Register& rd,
                        const Register& rn,
                        const Register& rm) {
  VIXL_ASSERT(rd.Is32Bits() && rn.Is32Bits() && rm.Is32Bits());
  Emit(SF(rm) | Rm(rm) | CRC32CW | Rn(rn) | Rd(rd));
}


void Assembler::crc32cx(const Register& rd,
                        const Register& rn,
                        const Register& rm) {
  VIXL_ASSERT(rd.Is32Bits() && rn.Is32Bits() && rm.Is64Bits());
  Emit(SF(rm) | Rm(rm) | CRC32CX | Rn(rn) | Rd(rd));
}


void Assembler::mul(const Register& rd,
                    const Register& rn,
                    const Register& rm) {
  VIXL_ASSERT(AreSameSizeAndType(rd, rn, rm));
  DataProcessing3Source(rd, rn, rm, AppropriateZeroRegFor(rd), MADD);
}


void Assembler::madd(const Register& rd,
                     const Register& rn,
                     const Register& rm,
                     const Register& ra) {
  DataProcessing3Source(rd, rn, rm, ra, MADD);
}


void Assembler::mneg(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(AreSameSizeAndType(rd, rn, rm));
  DataProcessing3Source(rd, rn, rm, AppropriateZeroRegFor(rd), MSUB);
}


void Assembler::msub(const Register& rd,
                     const Register& rn,
                     const Register& rm,
                     const Register& ra) {
  DataProcessing3Source(rd, rn, rm, ra, MSUB);
}


void Assembler::umaddl(const Register& rd,
                       const Register& rn,
                       const Register& rm,
                       const Register& ra) {
  VIXL_ASSERT(rd.Is64Bits() && ra.Is64Bits());
  VIXL_ASSERT(rn.Is32Bits() && rm.Is32Bits());
  DataProcessing3Source(rd, rn, rm, ra, UMADDL_x);
}


void Assembler::smaddl(const Register& rd,
                       const Register& rn,
                       const Register& rm,
                       const Register& ra) {
  VIXL_ASSERT(rd.Is64Bits() && ra.Is64Bits());
  VIXL_ASSERT(rn.Is32Bits() && rm.Is32Bits());
  DataProcessing3Source(rd, rn, rm, ra, SMADDL_x);
}


void Assembler::umsubl(const Register& rd,
                       const Register& rn,
                       const Register& rm,
                       const Register& ra) {
  VIXL_ASSERT(rd.Is64Bits() && ra.Is64Bits());
  VIXL_ASSERT(rn.Is32Bits() && rm.Is32Bits());
  DataProcessing3Source(rd, rn, rm, ra, UMSUBL_x);
}


void Assembler::smsubl(const Register& rd,
                       const Register& rn,
                       const Register& rm,
                       const Register& ra) {
  VIXL_ASSERT(rd.Is64Bits() && ra.Is64Bits());
  VIXL_ASSERT(rn.Is32Bits() && rm.Is32Bits());
  DataProcessing3Source(rd, rn, rm, ra, SMSUBL_x);
}


void Assembler::smull(const Register& rd,
                      const Register& rn,
                      const Register& rm) {
  VIXL_ASSERT(rd.Is64Bits());
  VIXL_ASSERT(rn.Is32Bits() && rm.Is32Bits());
  DataProcessing3Source(rd, rn, rm, xzr, SMADDL_x);
}


void Assembler::sdiv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == rm.size());
  Emit(SF(rd) | SDIV | Rm(rm) | Rn(rn) | Rd(rd));
}


void Assembler::smulh(const Register& xd,
                      const Register& xn,
                      const Register& xm) {
  VIXL_ASSERT(xd.Is64Bits() && xn.Is64Bits() && xm.Is64Bits());
  DataProcessing3Source(xd, xn, xm, xzr, SMULH_x);
}


void Assembler::umulh(const Register& xd,
                      const Register& xn,
                      const Register& xm) {
  VIXL_ASSERT(xd.Is64Bits() && xn.Is64Bits() && xm.Is64Bits());
  DataProcessing3Source(xd, xn, xm, xzr, UMULH_x);
}


void Assembler::udiv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == rm.size());
  Emit(SF(rd) | UDIV | Rm(rm) | Rn(rn) | Rd(rd));
}


void Assembler::rbit(const Register& rd,
                     const Register& rn) {
  DataProcessing1Source(rd, rn, RBIT);
}


void Assembler::rev16(const Register& rd,
                      const Register& rn) {
  DataProcessing1Source(rd, rn, REV16);
}


void Assembler::rev32(const Register& rd,
                      const Register& rn) {
  VIXL_ASSERT(rd.Is64Bits());
  DataProcessing1Source(rd, rn, REV);
}


void Assembler::rev(const Register& rd,
                    const Register& rn) {
  DataProcessing1Source(rd, rn, rd.Is64Bits() ? REV_x : REV_w);
}


void Assembler::clz(const Register& rd,
                    const Register& rn) {
  DataProcessing1Source(rd, rn, CLZ);
}


void Assembler::cls(const Register& rd,
                    const Register& rn) {
  DataProcessing1Source(rd, rn, CLS);
}


void Assembler::ldp(const CPURegister& rt,
                    const CPURegister& rt2,
                    const MemOperand& src) {
  LoadStorePair(rt, rt2, src, LoadPairOpFor(rt, rt2));
}


void Assembler::stp(const CPURegister& rt,
                    const CPURegister& rt2,
                    const MemOperand& dst) {
  LoadStorePair(rt, rt2, dst, StorePairOpFor(rt, rt2));
}


void Assembler::ldpsw(const Register& rt,
                      const Register& rt2,
                      const MemOperand& src) {
  VIXL_ASSERT(rt.Is64Bits());
  LoadStorePair(rt, rt2, src, LDPSW_x);
}


void Assembler::LoadStorePair(const CPURegister& rt,
                              const CPURegister& rt2,
                              const MemOperand& addr,
                              LoadStorePairOp op) {
  // 'rt' and 'rt2' can only be aliased for stores.
  VIXL_ASSERT(((op & LoadStorePairLBit) == 0) || !rt.Is(rt2));
  VIXL_ASSERT(AreSameSizeAndType(rt, rt2));
  VIXL_ASSERT(IsImmLSPair(addr.offset(), CalcLSPairDataSize(op)));

  int offset = static_cast<int>(addr.offset());
  Instr memop = op | Rt(rt) | Rt2(rt2) | RnSP(addr.base()) |
                ImmLSPair(offset, CalcLSPairDataSize(op));

  Instr addrmodeop;
  if (addr.IsImmediateOffset()) {
    addrmodeop = LoadStorePairOffsetFixed;
  } else {
    VIXL_ASSERT(addr.offset() != 0);
    if (addr.IsPreIndex()) {
      addrmodeop = LoadStorePairPreIndexFixed;
    } else {
      VIXL_ASSERT(addr.IsPostIndex());
      addrmodeop = LoadStorePairPostIndexFixed;
    }
  }
  Emit(addrmodeop | memop);
}


void Assembler::ldnp(const CPURegister& rt,
                     const CPURegister& rt2,
                     const MemOperand& src) {
  LoadStorePairNonTemporal(rt, rt2, src,
                           LoadPairNonTemporalOpFor(rt, rt2));
}


void Assembler::stnp(const CPURegister& rt,
                     const CPURegister& rt2,
                     const MemOperand& dst) {
  LoadStorePairNonTemporal(rt, rt2, dst,
                           StorePairNonTemporalOpFor(rt, rt2));
}


void Assembler::LoadStorePairNonTemporal(const CPURegister& rt,
                                         const CPURegister& rt2,
                                         const MemOperand& addr,
                                         LoadStorePairNonTemporalOp op) {
  VIXL_ASSERT(!rt.Is(rt2));
  VIXL_ASSERT(AreSameSizeAndType(rt, rt2));
  VIXL_ASSERT(addr.IsImmediateOffset());

  unsigned size = CalcLSPairDataSize(
    static_cast<LoadStorePairOp>(op & LoadStorePairMask));
  VIXL_ASSERT(IsImmLSPair(addr.offset(), size));
  int offset = static_cast<int>(addr.offset());
  Emit(op | Rt(rt) | Rt2(rt2) | RnSP(addr.base()) | ImmLSPair(offset, size));
}


// Memory instructions.
void Assembler::ldrb(const Register& rt, const MemOperand& src,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, LDRB_w, option);
}


void Assembler::strb(const Register& rt, const MemOperand& dst,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, dst, STRB_w, option);
}


void Assembler::ldrsb(const Register& rt, const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, rt.Is64Bits() ? LDRSB_x : LDRSB_w, option);
}


void Assembler::ldrh(const Register& rt, const MemOperand& src,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, LDRH_w, option);
}


void Assembler::strh(const Register& rt, const MemOperand& dst,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, dst, STRH_w, option);
}


void Assembler::ldrsh(const Register& rt, const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, rt.Is64Bits() ? LDRSH_x : LDRSH_w, option);
}


void Assembler::ldr(const CPURegister& rt, const MemOperand& src,
                    LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, LoadOpFor(rt), option);
}


void Assembler::str(const CPURegister& rt, const MemOperand& dst,
                    LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, dst, StoreOpFor(rt), option);
}


void Assembler::ldrsw(const Register& rt, const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(rt.Is64Bits());
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, LDRSW_x, option);
}


void Assembler::ldurb(const Register& rt, const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, LDRB_w, option);
}


void Assembler::sturb(const Register& rt, const MemOperand& dst,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, dst, STRB_w, option);
}


void Assembler::ldursb(const Register& rt, const MemOperand& src,
                       LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, rt.Is64Bits() ? LDRSB_x : LDRSB_w, option);
}


void Assembler::ldurh(const Register& rt, const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, LDRH_w, option);
}


void Assembler::sturh(const Register& rt, const MemOperand& dst,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, dst, STRH_w, option);
}


void Assembler::ldursh(const Register& rt, const MemOperand& src,
                       LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, rt.Is64Bits() ? LDRSH_x : LDRSH_w, option);
}


void Assembler::ldur(const CPURegister& rt, const MemOperand& src,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, LoadOpFor(rt), option);
}


void Assembler::stur(const CPURegister& rt, const MemOperand& dst,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, dst, StoreOpFor(rt), option);
}


void Assembler::ldursw(const Register& rt, const MemOperand& src,
                       LoadStoreScalingOption option) {
  VIXL_ASSERT(rt.Is64Bits());
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, LDRSW_x, option);
}


void Assembler::ldrsw(const Register& rt, int imm19) {
  Emit(LDRSW_x_lit | ImmLLiteral(imm19) | Rt(rt));
}


void Assembler::ldr(const CPURegister& rt, int imm19) {
  LoadLiteralOp op = LoadLiteralOpFor(rt);
  Emit(op | ImmLLiteral(imm19) | Rt(rt));
}

// clang-format off
#define COMPARE_AND_SWAP_W_X_LIST(V) \
  V(cas,   CAS)                      \
  V(casa,  CASA)                     \
  V(casl,  CASL)                     \
  V(casal, CASAL)
// clang-format on

#define DEFINE_ASM_FUNC(FN, OP)                                  \
  void Assembler::FN(const Register& rs, const Register& rt,     \
                     const MemOperand& src) {                    \
    VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0)); \
    LoadStoreExclusive op = rt.Is64Bits() ? OP##_x : OP##_w;     \
    Emit(op | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(src.base()));    \
  }
COMPARE_AND_SWAP_W_X_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC

// clang-format off
#define COMPARE_AND_SWAP_W_LIST(V) \
  V(casb,   CASB)                  \
  V(casab,  CASAB)                 \
  V(caslb,  CASLB)                 \
  V(casalb, CASALB)                \
  V(cash,   CASH)                  \
  V(casah,  CASAH)                 \
  V(caslh,  CASLH)                 \
  V(casalh, CASALH)
// clang-format on

#define DEFINE_ASM_FUNC(FN, OP)                                  \
  void Assembler::FN(const Register& rs, const Register& rt,     \
                     const MemOperand& src) {                    \
    VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0)); \
    Emit(OP | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(src.base()));    \
  }
COMPARE_AND_SWAP_W_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC

// clang-format off
#define COMPARE_AND_SWAP_PAIR_LIST(V) \
  V(casp,   CASP)                     \
  V(caspa,  CASPA)                    \
  V(caspl,  CASPL)                    \
  V(caspal, CASPAL)
// clang-format on

#define DEFINE_ASM_FUNC(FN, OP)                                  \
  void Assembler::FN(const Register& rs, const Register& rs1,    \
                     const Register& rt, const Register& rt1,    \
                     const MemOperand& src) {                    \
    USE(rs1, rt1);                                               \
    VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0)); \
    VIXL_ASSERT(AreEven(rs, rt));                                \
    VIXL_ASSERT(AreConsecutive(rs, rs1));                        \
    VIXL_ASSERT(AreConsecutive(rt, rt1));                        \
    LoadStoreExclusive op = rt.Is64Bits() ? OP##_x : OP##_w;     \
    Emit(op | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(src.base()));    \
  }
COMPARE_AND_SWAP_PAIR_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC

void Assembler::prfm(PrefetchOperation op, int imm19) {
  Emit(PRFM_lit | ImmPrefetchOperation(op) | ImmLLiteral(imm19));
}


// Exclusive-access instructions.
void Assembler::stxrb(const Register& rs,
                      const Register& rt,
                      const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  Emit(STXRB_w | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::stxrh(const Register& rs,
                      const Register& rt,
                      const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  Emit(STXRH_w | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::stxr(const Register& rs,
                     const Register& rt,
                     const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? STXR_x : STXR_w;
  Emit(op | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::ldxrb(const Register& rt,
                      const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  Emit(LDXRB_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}


void Assembler::ldxrh(const Register& rt,
                      const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  Emit(LDXRH_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}


void Assembler::ldxr(const Register& rt,
                     const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? LDXR_x : LDXR_w;
  Emit(op | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}


void Assembler::stxp(const Register& rs,
                     const Register& rt,
                     const Register& rt2,
                     const MemOperand& dst) {
  VIXL_ASSERT(rt.size() == rt2.size());
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? STXP_x : STXP_w;
  Emit(op | Rs(rs) | Rt(rt) | Rt2(rt2) | RnSP(dst.base()));
}


void Assembler::ldxp(const Register& rt,
                     const Register& rt2,
                     const MemOperand& src) {
  VIXL_ASSERT(rt.size() == rt2.size());
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? LDXP_x : LDXP_w;
  Emit(op | Rs_mask | Rt(rt) | Rt2(rt2) | RnSP(src.base()));
}


void Assembler::stlxrb(const Register& rs,
                       const Register& rt,
                       const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  Emit(STLXRB_w | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::stlxrh(const Register& rs,
                       const Register& rt,
                       const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  Emit(STLXRH_w | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::stlxr(const Register& rs,
                      const Register& rt,
                      const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? STLXR_x : STLXR_w;
  Emit(op | Rs(rs) | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::ldaxrb(const Register& rt,
                       const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  Emit(LDAXRB_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}


void Assembler::ldaxrh(const Register& rt,
                       const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  Emit(LDAXRH_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}


void Assembler::ldaxr(const Register& rt,
                      const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? LDAXR_x : LDAXR_w;
  Emit(op | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}


void Assembler::stlxp(const Register& rs,
                      const Register& rt,
                      const Register& rt2,
                      const MemOperand& dst) {
  VIXL_ASSERT(rt.size() == rt2.size());
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? STLXP_x : STLXP_w;
  Emit(op | Rs(rs) | Rt(rt) | Rt2(rt2) | RnSP(dst.base()));
}


void Assembler::ldaxp(const Register& rt,
                      const Register& rt2,
                      const MemOperand& src) {
  VIXL_ASSERT(rt.size() == rt2.size());
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? LDAXP_x : LDAXP_w;
  Emit(op | Rs_mask | Rt(rt) | Rt2(rt2) | RnSP(src.base()));
}


void Assembler::stlrb(const Register& rt,
                      const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  Emit(STLRB_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::stlrh(const Register& rt,
                      const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  Emit(STLRH_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::stlr(const Register& rt,
                     const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? STLR_x : STLR_w;
  Emit(op | Rs_mask | Rt(rt) | Rt2_mask | RnSP(dst.base()));
}


void Assembler::ldarb(const Register& rt,
                      const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  Emit(LDARB_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}


void Assembler::ldarh(const Register& rt,
                      const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  Emit(LDARH_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}


void Assembler::ldar(const Register& rt,
                     const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? LDAR_x : LDAR_w;
  Emit(op | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.base()));
}

// These macros generate all the variations of the atomic memory operations,
// e.g. ldadd, ldadda, ldaddb, staddl, etc.
// For a full list of the methods with comments, see the assembler header file.

// clang-format off
#define ATOMIC_MEMORY_SIMPLE_OPERATION_LIST(V, DEF) \
  V(DEF, add,  LDADD)                               \
  V(DEF, clr,  LDCLR)                               \
  V(DEF, eor,  LDEOR)                               \
  V(DEF, set,  LDSET)                               \
  V(DEF, smax, LDSMAX)                              \
  V(DEF, smin, LDSMIN)                              \
  V(DEF, umax, LDUMAX)                              \
  V(DEF, umin, LDUMIN)

#define ATOMIC_MEMORY_STORE_MODES(V, NAME, OP) \
  V(NAME,     OP##_x,   OP##_w)                \
  V(NAME##l,  OP##L_x,  OP##L_w)               \
  V(NAME##b,  OP##B,    OP##B)                 \
  V(NAME##lb, OP##LB,   OP##LB)                \
  V(NAME##h,  OP##H,    OP##H)                 \
  V(NAME##lh, OP##LH,   OP##LH)

#define ATOMIC_MEMORY_LOAD_MODES(V, NAME, OP) \
  ATOMIC_MEMORY_STORE_MODES(V, NAME, OP)      \
  V(NAME##a,   OP##A_x,  OP##A_w)             \
  V(NAME##al,  OP##AL_x, OP##AL_w)            \
  V(NAME##ab,  OP##AB,   OP##AB)              \
  V(NAME##alb, OP##ALB,  OP##ALB)             \
  V(NAME##ah,  OP##AH,   OP##AH)              \
  V(NAME##alh, OP##ALH,  OP##ALH)
// clang-format on

#define DEFINE_ASM_LOAD_FUNC(FN, OP_X, OP_W)                     \
  void Assembler::ld##FN(const Register& rs, const Register& rt, \
                         const MemOperand& src) {                \
    VIXL_ASSERT(CPUHas(CPUFeatures::kAtomics));                  \
    VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0)); \
    AtomicMemoryOp op = rt.Is64Bits() ? OP_X : OP_W;             \
    Emit(op | Rs(rs) | Rt(rt) | RnSP(src.base()));               \
  }
#define DEFINE_ASM_STORE_FUNC(FN, OP_X, OP_W)                         \
  void Assembler::st##FN(const Register& rs, const MemOperand& src) { \
    VIXL_ASSERT(CPUHas(CPUFeatures::kAtomics));                       \
    ld##FN(rs, AppropriateZeroRegFor(rs), src);                       \
  }

ATOMIC_MEMORY_SIMPLE_OPERATION_LIST(ATOMIC_MEMORY_LOAD_MODES,
                                    DEFINE_ASM_LOAD_FUNC)
ATOMIC_MEMORY_SIMPLE_OPERATION_LIST(ATOMIC_MEMORY_STORE_MODES,
                                    DEFINE_ASM_STORE_FUNC)

#define DEFINE_ASM_SWP_FUNC(FN, OP_X, OP_W)                      \
  void Assembler::FN(const Register& rs, const Register& rt,     \
                     const MemOperand& src) {                    \
    VIXL_ASSERT(CPUHas(CPUFeatures::kAtomics));                  \
    VIXL_ASSERT(src.IsImmediateOffset() && (src.offset() == 0)); \
    AtomicMemoryOp op = rt.Is64Bits() ? OP_X : OP_W;             \
    Emit(op | Rs(rs) | Rt(rt) | RnSP(src.base()));               \
  }

ATOMIC_MEMORY_LOAD_MODES(DEFINE_ASM_SWP_FUNC, swp, SWP)

#undef DEFINE_ASM_LOAD_FUNC
#undef DEFINE_ASM_STORE_FUNC
#undef DEFINE_ASM_SWP_FUNC

void Assembler::prfm(PrefetchOperation op, const MemOperand& address,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  Prefetch(op, address, option);
}


void Assembler::prfum(PrefetchOperation op, const MemOperand& address,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  Prefetch(op, address, option);
}


void Assembler::sys(int op1, int crn, int crm, int op2, const Register& rt) {
  Emit(SYS | ImmSysOp1(op1) | CRn(crn) | CRm(crm) | ImmSysOp2(op2) | Rt(rt));
}


void Assembler::sys(int op, const Register& rt) {
  Emit(SYS | SysOp(op) | Rt(rt));
}


void Assembler::dc(DataCacheOp op, const Register& rt) {
  VIXL_ASSERT((op == CVAC) || (op == CVAU) || (op == CIVAC) || (op == ZVA));
  sys(op, rt);
}


void Assembler::ic(InstructionCacheOp op, const Register& rt) {
  VIXL_ASSERT(op == IVAU);
  sys(op, rt);
}


// NEON structure loads and stores.
Instr Assembler::LoadStoreStructAddrModeField(const MemOperand& addr) {
  Instr addr_field = RnSP(addr.base());

  if (addr.IsPostIndex()) {
    VIXL_STATIC_ASSERT(NEONLoadStoreMultiStructPostIndex ==
        static_cast<NEONLoadStoreMultiStructPostIndexOp>(
            NEONLoadStoreSingleStructPostIndex));

    addr_field |= NEONLoadStoreMultiStructPostIndex;
    if (addr.offset() == 0) {
      addr_field |= RmNot31(addr.regoffset());
    } else {
      // The immediate post index addressing mode is indicated by rm = 31.
      // The immediate is implied by the number of vector registers used.
      addr_field |= (0x1f << Rm_offset);
    }
  } else {
    VIXL_ASSERT(addr.IsImmediateOffset() && (addr.offset() == 0));
  }
  return addr_field;
}

void Assembler::LoadStoreStructVerify(const VRegister& vt,
                                      const MemOperand& addr,
                                      Instr op) {
#ifdef DEBUG
  // Assert that addressing mode is either offset (with immediate 0), post
  // index by immediate of the size of the register list, or post index by a
  // value in a core register.
  if (addr.IsImmediateOffset()) {
    VIXL_ASSERT(addr.offset() == 0);
  } else {
    int offset = vt.SizeInBytes();
    switch (op) {
      case NEON_LD1_1v:
      case NEON_ST1_1v:
        offset *= 1; break;
      case NEONLoadStoreSingleStructLoad1:
      case NEONLoadStoreSingleStructStore1:
      case NEON_LD1R:
        offset = (offset / vt.lanes()) * 1; break;

      case NEON_LD1_2v:
      case NEON_ST1_2v:
      case NEON_LD2:
      case NEON_ST2:
        offset *= 2;
        break;
      case NEONLoadStoreSingleStructLoad2:
      case NEONLoadStoreSingleStructStore2:
      case NEON_LD2R:
        offset = (offset / vt.lanes()) * 2; break;

      case NEON_LD1_3v:
      case NEON_ST1_3v:
      case NEON_LD3:
      case NEON_ST3:
        offset *= 3; break;
      case NEONLoadStoreSingleStructLoad3:
      case NEONLoadStoreSingleStructStore3:
      case NEON_LD3R:
        offset = (offset / vt.lanes()) * 3; break;

      case NEON_LD1_4v:
      case NEON_ST1_4v:
      case NEON_LD4:
      case NEON_ST4:
        offset *= 4; break;
      case NEONLoadStoreSingleStructLoad4:
      case NEONLoadStoreSingleStructStore4:
      case NEON_LD4R:
        offset = (offset / vt.lanes()) * 4; break;
      default:
        VIXL_UNREACHABLE();
    }
    VIXL_ASSERT(!addr.regoffset().Is(NoReg) ||
                addr.offset() == offset);
  }
#else
  USE(vt, addr, op);
#endif
}

void Assembler::LoadStoreStruct(const VRegister& vt,
                                const MemOperand& addr,
                                NEONLoadStoreMultiStructOp op) {
  LoadStoreStructVerify(vt, addr, op);
  VIXL_ASSERT(vt.IsVector() || vt.Is1D());
  Emit(op | LoadStoreStructAddrModeField(addr) | LSVFormat(vt) | Rt(vt));
}


void Assembler::LoadStoreStructSingleAllLanes(const VRegister& vt,
					      const MemOperand& addr,
					      NEONLoadStoreSingleStructOp op) {
  LoadStoreStructVerify(vt, addr, op);
  Emit(op | LoadStoreStructAddrModeField(addr) | LSVFormat(vt) | Rt(vt));
}


void Assembler::ld1(const VRegister& vt,
                    const MemOperand& src) {
  LoadStoreStruct(vt, src, NEON_LD1_1v);
}


void Assembler::ld1(const VRegister& vt,
                    const VRegister& vt2,
                    const MemOperand& src) {
  USE(vt2);
  VIXL_ASSERT(AreSameFormat(vt, vt2));
  VIXL_ASSERT(AreConsecutive(vt, vt2));
  LoadStoreStruct(vt, src, NEON_LD1_2v);
}


void Assembler::ld1(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const MemOperand& src) {
  USE(vt2, vt3);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3));
  LoadStoreStruct(vt, src, NEON_LD1_3v);
}


void Assembler::ld1(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const VRegister& vt4,
                    const MemOperand& src) {
  USE(vt2, vt3, vt4);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3, vt4));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3, vt4));
  LoadStoreStruct(vt, src, NEON_LD1_4v);
}


void Assembler::ld2(const VRegister& vt,
                    const VRegister& vt2,
                    const MemOperand& src) {
  USE(vt2);
  VIXL_ASSERT(AreSameFormat(vt, vt2));
  VIXL_ASSERT(AreConsecutive(vt, vt2));
  LoadStoreStruct(vt, src, NEON_LD2);
}


void Assembler::ld2(const VRegister& vt,
                    const VRegister& vt2,
                    int lane,
                    const MemOperand& src) {
  USE(vt2);
  VIXL_ASSERT(AreSameFormat(vt, vt2));
  VIXL_ASSERT(AreConsecutive(vt, vt2));
  LoadStoreStructSingle(vt, lane, src, NEONLoadStoreSingleStructLoad2);
}


void Assembler::ld2r(const VRegister& vt,
                     const VRegister& vt2,
                     const MemOperand& src) {
  USE(vt2);
  VIXL_ASSERT(AreSameFormat(vt, vt2));
  VIXL_ASSERT(AreConsecutive(vt, vt2));
  LoadStoreStructSingleAllLanes(vt, src, NEON_LD2R);
}


void Assembler::ld3(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const MemOperand& src) {
  USE(vt2, vt3);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3));
  LoadStoreStruct(vt, src, NEON_LD3);
}


void Assembler::ld3(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    int lane,
                    const MemOperand& src) {
  USE(vt2, vt3);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3));
  LoadStoreStructSingle(vt, lane, src, NEONLoadStoreSingleStructLoad3);
}


void Assembler::ld3r(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const MemOperand& src) {
  USE(vt2, vt3);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3));
  LoadStoreStructSingleAllLanes(vt, src, NEON_LD3R);
}


void Assembler::ld4(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const VRegister& vt4,
                    const MemOperand& src) {
  USE(vt2, vt3, vt4);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3, vt4));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3, vt4));
  LoadStoreStruct(vt, src, NEON_LD4);
}


void Assembler::ld4(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const VRegister& vt4,
                    int lane,
                    const MemOperand& src) {
  USE(vt2, vt3, vt4);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3, vt4));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3, vt4));
  LoadStoreStructSingle(vt, lane, src, NEONLoadStoreSingleStructLoad4);
}


void Assembler::ld4r(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const VRegister& vt4,
                    const MemOperand& src) {
  USE(vt2, vt3, vt4);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3, vt4));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3, vt4));
  LoadStoreStructSingleAllLanes(vt, src, NEON_LD4R);
}


void Assembler::st1(const VRegister& vt,
                    const MemOperand& src) {
  LoadStoreStruct(vt, src, NEON_ST1_1v);
}


void Assembler::st1(const VRegister& vt,
                    const VRegister& vt2,
                    const MemOperand& src) {
  USE(vt2);
  VIXL_ASSERT(AreSameFormat(vt, vt2));
  VIXL_ASSERT(AreConsecutive(vt, vt2));
  LoadStoreStruct(vt, src, NEON_ST1_2v);
}


void Assembler::st1(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const MemOperand& src) {
  USE(vt2, vt3);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3));
  LoadStoreStruct(vt, src, NEON_ST1_3v);
}


void Assembler::st1(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const VRegister& vt4,
                    const MemOperand& src) {
  USE(vt2, vt3, vt4);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3, vt4));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3, vt4));
  LoadStoreStruct(vt, src, NEON_ST1_4v);
}


void Assembler::st2(const VRegister& vt,
                    const VRegister& vt2,
                    const MemOperand& dst) {
  USE(vt2);
  VIXL_ASSERT(AreSameFormat(vt, vt2));
  VIXL_ASSERT(AreConsecutive(vt, vt2));
  LoadStoreStruct(vt, dst, NEON_ST2);
}


void Assembler::st2(const VRegister& vt,
                    const VRegister& vt2,
                    int lane,
                    const MemOperand& dst) {
  USE(vt2);
  VIXL_ASSERT(AreSameFormat(vt, vt2));
  VIXL_ASSERT(AreConsecutive(vt, vt2));
  LoadStoreStructSingle(vt, lane, dst, NEONLoadStoreSingleStructStore2);
}


void Assembler::st3(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const MemOperand& dst) {
  USE(vt2, vt3);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3));
  LoadStoreStruct(vt, dst, NEON_ST3);
}


void Assembler::st3(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    int lane,
                    const MemOperand& dst) {
  USE(vt2, vt3);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3));
  LoadStoreStructSingle(vt, lane, dst, NEONLoadStoreSingleStructStore3);
}


void Assembler::st4(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const VRegister& vt4,
                    const MemOperand& dst) {
  USE(vt2, vt3, vt4);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3, vt4));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3, vt4));
  LoadStoreStruct(vt, dst, NEON_ST4);
}


void Assembler::st4(const VRegister& vt,
                    const VRegister& vt2,
                    const VRegister& vt3,
                    const VRegister& vt4,
                    int lane,
                    const MemOperand& dst) {
  USE(vt2, vt3, vt4);
  VIXL_ASSERT(AreSameFormat(vt, vt2, vt3, vt4));
  VIXL_ASSERT(AreConsecutive(vt, vt2, vt3, vt4));
  LoadStoreStructSingle(vt, lane, dst, NEONLoadStoreSingleStructStore4);
}


void Assembler::LoadStoreStructSingle(const VRegister& vt,
                                      uint32_t lane,
                                      const MemOperand& addr,
                                      NEONLoadStoreSingleStructOp op) {
  LoadStoreStructVerify(vt, addr, op);

  // We support vt arguments of the form vt.VxT() or vt.T(), where x is the
  // number of lanes, and T is b, h, s or d.
  unsigned lane_size = vt.LaneSizeInBytes();
  VIXL_ASSERT(lane < (kQRegSizeInBytes / lane_size));

  // Lane size is encoded in the opcode field. Lane index is encoded in the Q,
  // S and size fields.
  lane *= lane_size;
  if (lane_size == 8) lane++;

  Instr size = (lane << NEONLSSize_offset) & NEONLSSize_mask;
  Instr s = (lane << (NEONS_offset - 2)) & NEONS_mask;
  Instr q = (lane << (NEONQ_offset - 3)) & NEONQ_mask;

  Instr instr = op;
  switch (lane_size) {
    case 1: instr |= NEONLoadStoreSingle_b; break;
    case 2: instr |= NEONLoadStoreSingle_h; break;
    case 4: instr |= NEONLoadStoreSingle_s; break;
    default:
      VIXL_ASSERT(lane_size == 8);
      instr |= NEONLoadStoreSingle_d;
  }

  Emit(instr | LoadStoreStructAddrModeField(addr) | q | size | s | Rt(vt));
}


void Assembler::ld1(const VRegister& vt,
                    int lane,
                    const MemOperand& src) {
  LoadStoreStructSingle(vt, lane, src, NEONLoadStoreSingleStructLoad1);
}


void Assembler::ld1r(const VRegister& vt,
                     const MemOperand& src) {
  LoadStoreStructSingleAllLanes(vt, src, NEON_LD1R);
}


void Assembler::st1(const VRegister& vt,
                    int lane,
                    const MemOperand& dst) {
  LoadStoreStructSingle(vt, lane, dst, NEONLoadStoreSingleStructStore1);
}


void Assembler::NEON3DifferentL(const VRegister& vd,
                                const VRegister& vn,
                                const VRegister& vm,
                                NEON3DifferentOp vop) {
  VIXL_ASSERT(AreSameFormat(vn, vm));
  VIXL_ASSERT((vn.Is1H() && vd.Is1S()) ||
              (vn.Is1S() && vd.Is1D()) ||
              (vn.Is8B() && vd.Is8H()) ||
              (vn.Is4H() && vd.Is4S()) ||
              (vn.Is2S() && vd.Is2D()) ||
              (vn.Is16B() && vd.Is8H())||
              (vn.Is8H() && vd.Is4S()) ||
              (vn.Is4S() && vd.Is2D()));
  Instr format, op = vop;
  if (vd.IsScalar()) {
    op |= NEON_Q | NEONScalar;
    format = SFormat(vn);
  } else {
    format = VFormat(vn);
  }
  Emit(format | op | Rm(vm) | Rn(vn) | Rd(vd));
}


void Assembler::NEON3DifferentW(const VRegister& vd,
                                const VRegister& vn,
                                const VRegister& vm,
                                NEON3DifferentOp vop) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT((vm.Is8B() && vd.Is8H()) ||
              (vm.Is4H() && vd.Is4S()) ||
              (vm.Is2S() && vd.Is2D()) ||
              (vm.Is16B() && vd.Is8H())||
              (vm.Is8H() && vd.Is4S()) ||
              (vm.Is4S() && vd.Is2D()));
  Emit(VFormat(vm) | vop | Rm(vm) | Rn(vn) | Rd(vd));
}


void Assembler::NEON3DifferentHN(const VRegister& vd,
                                 const VRegister& vn,
                                 const VRegister& vm,
                                 NEON3DifferentOp vop) {
  VIXL_ASSERT(AreSameFormat(vm, vn));
  VIXL_ASSERT((vd.Is8B() && vn.Is8H()) ||
              (vd.Is4H() && vn.Is4S()) ||
              (vd.Is2S() && vn.Is2D()) ||
              (vd.Is16B() && vn.Is8H())||
              (vd.Is8H() && vn.Is4S()) ||
              (vd.Is4S() && vn.Is2D()));
  Emit(VFormat(vd) | vop | Rm(vm) | Rn(vn) | Rd(vd));
}


#define NEON_3DIFF_LONG_LIST(V) \
  V(pmull,  NEON_PMULL,  vn.IsVector() && vn.Is8B())                           \
  V(pmull2, NEON_PMULL2, vn.IsVector() && vn.Is16B())                          \
  V(saddl,  NEON_SADDL,  vn.IsVector() && vn.IsD())                            \
  V(saddl2, NEON_SADDL2, vn.IsVector() && vn.IsQ())                            \
  V(sabal,  NEON_SABAL,  vn.IsVector() && vn.IsD())                            \
  V(sabal2, NEON_SABAL2, vn.IsVector() && vn.IsQ())                            \
  V(uabal,  NEON_UABAL,  vn.IsVector() && vn.IsD())                            \
  V(uabal2, NEON_UABAL2, vn.IsVector() && vn.IsQ())                            \
  V(sabdl,  NEON_SABDL,  vn.IsVector() && vn.IsD())                            \
  V(sabdl2, NEON_SABDL2, vn.IsVector() && vn.IsQ())                            \
  V(uabdl,  NEON_UABDL,  vn.IsVector() && vn.IsD())                            \
  V(uabdl2, NEON_UABDL2, vn.IsVector() && vn.IsQ())                            \
  V(smlal,  NEON_SMLAL,  vn.IsVector() && vn.IsD())                            \
  V(smlal2, NEON_SMLAL2, vn.IsVector() && vn.IsQ())                            \
  V(umlal,  NEON_UMLAL,  vn.IsVector() && vn.IsD())                            \
  V(umlal2, NEON_UMLAL2, vn.IsVector() && vn.IsQ())                            \
  V(smlsl,  NEON_SMLSL,  vn.IsVector() && vn.IsD())                            \
  V(smlsl2, NEON_SMLSL2, vn.IsVector() && vn.IsQ())                            \
  V(umlsl,  NEON_UMLSL,  vn.IsVector() && vn.IsD())                            \
  V(umlsl2, NEON_UMLSL2, vn.IsVector() && vn.IsQ())                            \
  V(smull,  NEON_SMULL,  vn.IsVector() && vn.IsD())                            \
  V(smull2, NEON_SMULL2, vn.IsVector() && vn.IsQ())                            \
  V(umull,  NEON_UMULL,  vn.IsVector() && vn.IsD())                            \
  V(umull2, NEON_UMULL2, vn.IsVector() && vn.IsQ())                            \
  V(ssubl,  NEON_SSUBL,  vn.IsVector() && vn.IsD())                            \
  V(ssubl2, NEON_SSUBL2, vn.IsVector() && vn.IsQ())                            \
  V(uaddl,  NEON_UADDL,  vn.IsVector() && vn.IsD())                            \
  V(uaddl2, NEON_UADDL2, vn.IsVector() && vn.IsQ())                            \
  V(usubl,  NEON_USUBL,  vn.IsVector() && vn.IsD())                            \
  V(usubl2, NEON_USUBL2, vn.IsVector() && vn.IsQ())                            \
  V(sqdmlal,  NEON_SQDMLAL,  vn.Is1H() || vn.Is1S() || vn.Is4H() || vn.Is2S()) \
  V(sqdmlal2, NEON_SQDMLAL2, vn.Is1H() || vn.Is1S() || vn.Is8H() || vn.Is4S()) \
  V(sqdmlsl,  NEON_SQDMLSL,  vn.Is1H() || vn.Is1S() || vn.Is4H() || vn.Is2S()) \
  V(sqdmlsl2, NEON_SQDMLSL2, vn.Is1H() || vn.Is1S() || vn.Is8H() || vn.Is4S()) \
  V(sqdmull,  NEON_SQDMULL,  vn.Is1H() || vn.Is1S() || vn.Is4H() || vn.Is2S()) \
  V(sqdmull2, NEON_SQDMULL2, vn.Is1H() || vn.Is1S() || vn.Is8H() || vn.Is4S()) \


#define DEFINE_ASM_FUNC(FN, OP, AS)        \
void Assembler::FN(const VRegister& vd,    \
                   const VRegister& vn,    \
                   const VRegister& vm) {  \
  VIXL_ASSERT(AS);                         \
  NEON3DifferentL(vd, vn, vm, OP);         \
}
NEON_3DIFF_LONG_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC

#define NEON_3DIFF_HN_LIST(V)         \
  V(addhn,   NEON_ADDHN,   vd.IsD())  \
  V(addhn2,  NEON_ADDHN2,  vd.IsQ())  \
  V(raddhn,  NEON_RADDHN,  vd.IsD())  \
  V(raddhn2, NEON_RADDHN2, vd.IsQ())  \
  V(subhn,   NEON_SUBHN,   vd.IsD())  \
  V(subhn2,  NEON_SUBHN2,  vd.IsQ())  \
  V(rsubhn,  NEON_RSUBHN,  vd.IsD())  \
  V(rsubhn2, NEON_RSUBHN2, vd.IsQ())

#define DEFINE_ASM_FUNC(FN, OP, AS)        \
void Assembler::FN(const VRegister& vd,    \
                   const VRegister& vn,    \
                   const VRegister& vm) {  \
  VIXL_ASSERT(AS);                         \
  NEON3DifferentHN(vd, vn, vm, OP);        \
}
NEON_3DIFF_HN_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC

void Assembler::uaddw(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm) {
  VIXL_ASSERT(vm.IsD());
  NEON3DifferentW(vd, vn, vm, NEON_UADDW);
}


void Assembler::uaddw2(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm) {
  VIXL_ASSERT(vm.IsQ());
  NEON3DifferentW(vd, vn, vm, NEON_UADDW2);
}


void Assembler::saddw(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm) {
  VIXL_ASSERT(vm.IsD());
  NEON3DifferentW(vd, vn, vm, NEON_SADDW);
}


void Assembler::saddw2(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm) {
  VIXL_ASSERT(vm.IsQ());
  NEON3DifferentW(vd, vn, vm, NEON_SADDW2);
}


void Assembler::usubw(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm) {
  VIXL_ASSERT(vm.IsD());
  NEON3DifferentW(vd, vn, vm, NEON_USUBW);
}


void Assembler::usubw2(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm) {
  VIXL_ASSERT(vm.IsQ());
  NEON3DifferentW(vd, vn, vm, NEON_USUBW2);
}


void Assembler::ssubw(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm) {
  VIXL_ASSERT(vm.IsD());
  NEON3DifferentW(vd, vn, vm, NEON_SSUBW);
}


void Assembler::ssubw2(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm) {
  VIXL_ASSERT(vm.IsQ());
  NEON3DifferentW(vd, vn, vm, NEON_SSUBW2);
}


void Assembler::mov(const Register& rd, const Register& rm) {
  // Moves involving the stack pointer are encoded as add immediate with
  // second operand of zero. Otherwise, orr with first operand zr is
  // used.
  if (rd.IsSP() || rm.IsSP()) {
    add(rd, rm, 0);
  } else {
    orr(rd, AppropriateZeroRegFor(rd), rm);
  }
}


void Assembler::mvn(const Register& rd, const Operand& operand) {
  orn(rd, AppropriateZeroRegFor(rd), operand);
}


void Assembler::mrs(const Register& rt, SystemRegister sysreg) {
  VIXL_ASSERT(rt.Is64Bits());
  Emit(MRS | ImmSystemRegister(sysreg) | Rt(rt));
}


void Assembler::msr(SystemRegister sysreg, const Register& rt) {
  VIXL_ASSERT(rt.Is64Bits());
  Emit(MSR | Rt(rt) | ImmSystemRegister(sysreg));
}


void Assembler::clrex(int imm4) {
  Emit(CLREX | CRm(imm4));
}


void Assembler::dmb(BarrierDomain domain, BarrierType type) {
  Emit(DMB | ImmBarrierDomain(domain) | ImmBarrierType(type));
}


void Assembler::dsb(BarrierDomain domain, BarrierType type) {
  Emit(DSB | ImmBarrierDomain(domain) | ImmBarrierType(type));
}


void Assembler::isb() {
  Emit(ISB | ImmBarrierDomain(FullSystem) | ImmBarrierType(BarrierAll));
}


void Assembler::fmov(const VRegister& vd, double imm) {
  if (vd.IsScalar()) {
    VIXL_ASSERT(vd.Is1D());
    Emit(FMOV_d_imm | Rd(vd) | ImmFP64(imm));
  } else {
    VIXL_ASSERT(vd.Is2D());
    Instr op = NEONModifiedImmediate_MOVI | NEONModifiedImmediateOpBit;
    Instr q = NEON_Q;
    uint32_t encoded_imm = FP64ToImm8(imm);
    Emit(q | op | ImmNEONabcdefgh(encoded_imm) | NEONCmode(0xf) | Rd(vd));
  }
}


void Assembler::fmov(const VRegister& vd, float imm) {
  if (vd.IsScalar()) {
    VIXL_ASSERT(vd.Is1S());
    Emit(FMOV_s_imm | Rd(vd) | ImmFP32(imm));
  } else {
    VIXL_ASSERT(vd.Is2S() || vd.Is4S());
    Instr op = NEONModifiedImmediate_MOVI;
    Instr q = vd.Is4S() ?  NEON_Q : 0;
    uint32_t encoded_imm = FP32ToImm8(imm);
    Emit(q | op | ImmNEONabcdefgh(encoded_imm) | NEONCmode(0xf) | Rd(vd));
  }
}


void Assembler::fmov(const Register& rd, const VRegister& vn) {
  VIXL_ASSERT(vn.Is1S() || vn.Is1D());
  VIXL_ASSERT(rd.size() == vn.size());
  FPIntegerConvertOp op = rd.Is32Bits() ? FMOV_ws : FMOV_xd;
  Emit(op | Rd(rd) | Rn(vn));
}


void Assembler::fmov(const VRegister& vd, const Register& rn) {
  VIXL_ASSERT(vd.Is1S() || vd.Is1D());
  VIXL_ASSERT(vd.size() == rn.size());
  FPIntegerConvertOp op = vd.Is32Bits() ? FMOV_sw : FMOV_dx;
  Emit(op | Rd(vd) | Rn(rn));
}


void Assembler::fmov(const VRegister& vd, const VRegister& vn) {
  VIXL_ASSERT(vd.Is1S() || vd.Is1D());
  VIXL_ASSERT(vd.IsSameFormat(vn));
  Emit(FPType(vd) | FMOV | Rd(vd) | Rn(vn));
}


void Assembler::fmov(const VRegister& vd, int index, const Register& rn) {
  VIXL_ASSERT((index == 1) && vd.Is1D() && rn.IsX());
  USE(index);
  Emit(FMOV_d1_x | Rd(vd) | Rn(rn));
}


void Assembler::fmov(const Register& rd, const VRegister& vn, int index) {
  VIXL_ASSERT((index == 1) && vn.Is1D() && rd.IsX());
  USE(index);
  Emit(FMOV_x_d1 | Rd(rd) | Rn(vn));
}


void Assembler::fmadd(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm,
                      const VRegister& va) {
  FPDataProcessing3Source(vd, vn, vm, va, vd.Is1S() ? FMADD_s : FMADD_d);
}


void Assembler::fmsub(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm,
                      const VRegister& va) {
  FPDataProcessing3Source(vd, vn, vm, va, vd.Is1S() ? FMSUB_s : FMSUB_d);
}


void Assembler::fnmadd(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm,
                       const VRegister& va) {
  FPDataProcessing3Source(vd, vn, vm, va, vd.Is1S() ? FNMADD_s : FNMADD_d);
}


void Assembler::fnmsub(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm,
                       const VRegister& va) {
  FPDataProcessing3Source(vd, vn, vm, va, vd.Is1S() ? FNMSUB_s : FNMSUB_d);
}


void Assembler::fnmul(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm) {
  VIXL_ASSERT(AreSameSizeAndType(vd, vn, vm));
  Instr op = vd.Is1S() ? FNMUL_s : FNMUL_d;
  Emit(FPType(vd) | op | Rm(vm) | Rn(vn) | Rd(vd));
}


void Assembler::FPCompareMacro(const VRegister& vn,
                               double value,
                               FPTrapFlags trap) {
  USE(value);
  // Although the fcmp{e} instructions can strictly only take an immediate
  // value of +0.0, we don't need to check for -0.0 because the sign of 0.0
  // doesn't affect the result of the comparison.
  VIXL_ASSERT(value == 0.0);
  VIXL_ASSERT(vn.Is1S() || vn.Is1D());
  Instr op = (trap == EnableTrap) ? FCMPE_zero : FCMP_zero;
  Emit(FPType(vn) | op | Rn(vn));
}


void Assembler::FPCompareMacro(const VRegister& vn,
                               const VRegister& vm,
                               FPTrapFlags trap) {
  VIXL_ASSERT(vn.Is1S() || vn.Is1D());
  VIXL_ASSERT(vn.IsSameSizeAndType(vm));
  Instr op = (trap == EnableTrap) ? FCMPE : FCMP;
  Emit(FPType(vn) | op | Rm(vm) | Rn(vn));
}


void Assembler::fcmp(const VRegister& vn,
                     const VRegister& vm) {
  FPCompareMacro(vn, vm, DisableTrap);
}


void Assembler::fcmpe(const VRegister& vn,
                      const VRegister& vm) {
  FPCompareMacro(vn, vm, EnableTrap);
}


void Assembler::fcmp(const VRegister& vn,
                     double value) {
  FPCompareMacro(vn, value, DisableTrap);
}


void Assembler::fcmpe(const VRegister& vn,
                      double value) {
  FPCompareMacro(vn, value, EnableTrap);
}


void Assembler::FPCCompareMacro(const VRegister& vn,
                                const VRegister& vm,
                                StatusFlags nzcv,
                                Condition cond,
                                FPTrapFlags trap) {
  VIXL_ASSERT(vn.Is1S() || vn.Is1D());
  VIXL_ASSERT(vn.IsSameSizeAndType(vm));
  Instr op = (trap == EnableTrap) ? FCCMPE : FCCMP;
  Emit(FPType(vn) | op | Rm(vm) | Cond(cond) | Rn(vn) | Nzcv(nzcv));
}

void Assembler::fccmp(const VRegister& vn,
                      const VRegister& vm,
                      StatusFlags nzcv,
                      Condition cond) {
  FPCCompareMacro(vn, vm, nzcv, cond, DisableTrap);
}


void Assembler::fccmpe(const VRegister& vn,
                       const VRegister& vm,
                       StatusFlags nzcv,
                       Condition cond) {
  FPCCompareMacro(vn, vm, nzcv, cond, EnableTrap);
}


void Assembler::fcsel(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm,
                      Condition cond) {
  VIXL_ASSERT(vd.Is1S() || vd.Is1D());
  VIXL_ASSERT(AreSameFormat(vd, vn, vm));
  Emit(FPType(vd) | FCSEL | Rm(vm) | Cond(cond) | Rn(vn) | Rd(vd));
}

void Assembler::fjcvtzs(const Register& rd, const VRegister& vn) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kFP, CPUFeatures::kJSCVT));
  VIXL_ASSERT(rd.IsW() && vn.Is1D());
  Emit(FJCVTZS | Rn(vn) | Rd(rd));
}


void Assembler::NEONFPConvertToInt(const Register& rd,
                                   const VRegister& vn,
                                   Instr op) {
  Emit(SF(rd) | FPType(vn) | op | Rn(vn) | Rd(rd));
}


void Assembler::NEONFPConvertToInt(const VRegister& vd,
                                   const VRegister& vn,
                                   Instr op) {
  if (vn.IsScalar()) {
    VIXL_ASSERT((vd.Is1S() && vn.Is1S()) || (vd.Is1D() && vn.Is1D()));
    op |= NEON_Q | NEONScalar;
  }
  Emit(FPFormat(vn) | op | Rn(vn) | Rd(vd));
}


void Assembler::fcvt(const VRegister& vd,
                     const VRegister& vn) {
  FPDataProcessing1SourceOp op;
  if (vd.Is1D()) {
    VIXL_ASSERT(vn.Is1S() || vn.Is1H());
    op = vn.Is1S() ? FCVT_ds : FCVT_dh;
  } else if (vd.Is1S()) {
    VIXL_ASSERT(vn.Is1D() || vn.Is1H());
    op = vn.Is1D() ? FCVT_sd : FCVT_sh;
  } else {
    VIXL_ASSERT(vd.Is1H());
    VIXL_ASSERT(vn.Is1D() || vn.Is1S());
    op = vn.Is1D() ? FCVT_hd : FCVT_hs;
  }
  FPDataProcessing1Source(vd, vn, op);
}


void Assembler::fcvtl(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT((vd.Is4S() && vn.Is4H()) ||
              (vd.Is2D() && vn.Is2S()));
  Instr format = vd.Is2D() ? (1 << NEONSize_offset) : 0;
  Emit(format | NEON_FCVTL | Rn(vn) | Rd(vd));
}


void Assembler::fcvtl2(const VRegister& vd,
                       const VRegister& vn) {
  VIXL_ASSERT((vd.Is4S() && vn.Is8H()) ||
              (vd.Is2D() && vn.Is4S()));
  Instr format = vd.Is2D() ? (1 << NEONSize_offset) : 0;
  Emit(NEON_Q | format | NEON_FCVTL | Rn(vn) | Rd(vd));
}


void Assembler::fcvtn(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT((vn.Is4S() && vd.Is4H()) ||
              (vn.Is2D() && vd.Is2S()));
  Instr format = vn.Is2D() ? (1 << NEONSize_offset) : 0;
  Emit(format | NEON_FCVTN | Rn(vn) | Rd(vd));
}


void Assembler::fcvtn2(const VRegister& vd,
                       const VRegister& vn) {
  VIXL_ASSERT((vn.Is4S() && vd.Is8H()) ||
              (vn.Is2D() && vd.Is4S()));
  Instr format = vn.Is2D() ? (1 << NEONSize_offset) : 0;
  Emit(NEON_Q | format | NEON_FCVTN | Rn(vn) | Rd(vd));
}


void Assembler::fcvtxn(const VRegister& vd,
                       const VRegister& vn) {
  Instr format = 1 << NEONSize_offset;
  if (vd.IsScalar()) {
    VIXL_ASSERT(vd.Is1S() && vn.Is1D());
    Emit(format | NEON_FCVTXN_scalar | Rn(vn) | Rd(vd));
  } else {
    VIXL_ASSERT(vd.Is2S() && vn.Is2D());
    Emit(format | NEON_FCVTXN | Rn(vn) | Rd(vd));
  }
}


void Assembler::fcvtxn2(const VRegister& vd,
                        const VRegister& vn) {
  VIXL_ASSERT(vd.Is4S() && vn.Is2D());
  Instr format = 1 << NEONSize_offset;
  Emit(NEON_Q | format | NEON_FCVTXN | Rn(vn) | Rd(vd));
}


#define NEON_FP2REGMISC_FCVT_LIST(V)  \
  V(fcvtnu, NEON_FCVTNU, FCVTNU)      \
  V(fcvtns, NEON_FCVTNS, FCVTNS)      \
  V(fcvtpu, NEON_FCVTPU, FCVTPU)      \
  V(fcvtps, NEON_FCVTPS, FCVTPS)      \
  V(fcvtmu, NEON_FCVTMU, FCVTMU)      \
  V(fcvtms, NEON_FCVTMS, FCVTMS)      \
  V(fcvtau, NEON_FCVTAU, FCVTAU)      \
  V(fcvtas, NEON_FCVTAS, FCVTAS)

#define DEFINE_ASM_FUNCS(FN, VEC_OP, SCA_OP)  \
void Assembler::FN(const Register& rd,        \
                   const VRegister& vn) {     \
  NEONFPConvertToInt(rd, vn, SCA_OP);         \
}                                             \
void Assembler::FN(const VRegister& vd,       \
                   const VRegister& vn) {     \
  NEONFPConvertToInt(vd, vn, VEC_OP);         \
}
NEON_FP2REGMISC_FCVT_LIST(DEFINE_ASM_FUNCS)
#undef DEFINE_ASM_FUNCS


void Assembler::fcvtzs(const Register& rd,
                       const VRegister& vn,
                       int fbits) {
  VIXL_ASSERT(vn.Is1S() || vn.Is1D());
  VIXL_ASSERT((fbits >= 0) && (fbits <= rd.SizeInBits()));
  if (fbits == 0) {
    Emit(SF(rd) | FPType(vn) | FCVTZS | Rn(vn) | Rd(rd));
  } else {
    Emit(SF(rd) | FPType(vn) | FCVTZS_fixed | FPScale(64 - fbits) | Rn(vn) |
         Rd(rd));
  }
}


void Assembler::fcvtzs(const VRegister& vd,
                       const VRegister& vn,
                       int fbits) {
  VIXL_ASSERT(fbits >= 0);
  if (fbits == 0) {
    NEONFP2RegMisc(vd, vn, NEON_FCVTZS);
  } else {
    VIXL_ASSERT(vd.Is1D() || vd.Is1S() || vd.Is2D() || vd.Is2S() || vd.Is4S());
    NEONShiftRightImmediate(vd, vn, fbits, NEON_FCVTZS_imm);
  }
}


void Assembler::fcvtzu(const Register& rd,
                       const VRegister& vn,
                       int fbits) {
  VIXL_ASSERT(vn.Is1S() || vn.Is1D());
  VIXL_ASSERT((fbits >= 0) && (fbits <= rd.SizeInBits()));
  if (fbits == 0) {
    Emit(SF(rd) | FPType(vn) | FCVTZU | Rn(vn) | Rd(rd));
  } else {
    Emit(SF(rd) | FPType(vn) | FCVTZU_fixed | FPScale(64 - fbits) | Rn(vn) |
         Rd(rd));
  }
}


void Assembler::fcvtzu(const VRegister& vd,
                       const VRegister& vn,
                       int fbits) {
  VIXL_ASSERT(fbits >= 0);
  if (fbits == 0) {
    NEONFP2RegMisc(vd, vn, NEON_FCVTZU);
  } else {
    VIXL_ASSERT(vd.Is1D() || vd.Is1S() || vd.Is2D() || vd.Is2S() || vd.Is4S());
    NEONShiftRightImmediate(vd, vn, fbits, NEON_FCVTZU_imm);
  }
}

void Assembler::ucvtf(const VRegister& vd,
                      const VRegister& vn,
                      int fbits) {
  VIXL_ASSERT(fbits >= 0);
  if (fbits == 0) {
    NEONFP2RegMisc(vd, vn, NEON_UCVTF);
  } else {
    VIXL_ASSERT(vd.Is1D() || vd.Is1S() || vd.Is2D() || vd.Is2S() || vd.Is4S());
    NEONShiftRightImmediate(vd, vn, fbits, NEON_UCVTF_imm);
  }
}

void Assembler::scvtf(const VRegister& vd,
                      const VRegister& vn,
                      int fbits) {
  VIXL_ASSERT(fbits >= 0);
  if (fbits == 0) {
    NEONFP2RegMisc(vd, vn, NEON_SCVTF);
  } else {
    VIXL_ASSERT(vd.Is1D() || vd.Is1S() || vd.Is2D() || vd.Is2S() || vd.Is4S());
    NEONShiftRightImmediate(vd, vn, fbits, NEON_SCVTF_imm);
  }
}


void Assembler::scvtf(const VRegister& vd,
                      const Register& rn,
                      int fbits) {
  VIXL_ASSERT(vd.Is1S() || vd.Is1D());
  VIXL_ASSERT(fbits >= 0);
  if (fbits == 0) {
    Emit(SF(rn) | FPType(vd) | SCVTF | Rn(rn) | Rd(vd));
  } else {
    Emit(SF(rn) | FPType(vd) | SCVTF_fixed | FPScale(64 - fbits) | Rn(rn) |
         Rd(vd));
  }
}


void Assembler::ucvtf(const VRegister& vd,
                      const Register& rn,
                      int fbits) {
  VIXL_ASSERT(vd.Is1S() || vd.Is1D());
  VIXL_ASSERT(fbits >= 0);
  if (fbits == 0) {
    Emit(SF(rn) | FPType(vd) | UCVTF | Rn(rn) | Rd(vd));
  } else {
    Emit(SF(rn) | FPType(vd) | UCVTF_fixed | FPScale(64 - fbits) | Rn(rn) |
         Rd(vd));
  }
}


void Assembler::NEON3Same(const VRegister& vd,
                          const VRegister& vn,
                          const VRegister& vm,
                          NEON3SameOp vop) {
  VIXL_ASSERT(AreSameFormat(vd, vn, vm));
  VIXL_ASSERT(vd.IsVector() || !vd.IsQ());

  Instr format, op = vop;
  if (vd.IsScalar()) {
    op |= NEON_Q | NEONScalar;
    format = SFormat(vd);
  } else {
    format = VFormat(vd);
  }

  Emit(format | op | Rm(vm) | Rn(vn) | Rd(vd));
}


void Assembler::NEONFP3Same(const VRegister& vd,
                            const VRegister& vn,
                            const VRegister& vm,
                            Instr op) {
  VIXL_ASSERT(AreSameFormat(vd, vn, vm));
  Emit(FPFormat(vd) | op | Rm(vm) | Rn(vn) | Rd(vd));
}


#define NEON_FP2REGMISC_LIST(V)                 \
  V(fabs,    NEON_FABS,    FABS)                \
  V(fneg,    NEON_FNEG,    FNEG)                \
  V(fsqrt,   NEON_FSQRT,   FSQRT)               \
  V(frintn,  NEON_FRINTN,  FRINTN)              \
  V(frinta,  NEON_FRINTA,  FRINTA)              \
  V(frintp,  NEON_FRINTP,  FRINTP)              \
  V(frintm,  NEON_FRINTM,  FRINTM)              \
  V(frintx,  NEON_FRINTX,  FRINTX)              \
  V(frintz,  NEON_FRINTZ,  FRINTZ)              \
  V(frinti,  NEON_FRINTI,  FRINTI)              \
  V(frsqrte, NEON_FRSQRTE, NEON_FRSQRTE_scalar) \
  V(frecpe,  NEON_FRECPE,  NEON_FRECPE_scalar )


#define DEFINE_ASM_FUNC(FN, VEC_OP, SCA_OP)            \
void Assembler::FN(const VRegister& vd,                \
                   const VRegister& vn) {              \
  Instr op;                                            \
  if (vd.IsScalar()) {                                 \
    VIXL_ASSERT(vd.Is1S() || vd.Is1D());               \
    op = SCA_OP;                                       \
  } else {                                             \
    VIXL_ASSERT(vd.Is2S() || vd.Is2D() || vd.Is4S());  \
    op = VEC_OP;                                       \
  }                                                    \
  NEONFP2RegMisc(vd, vn, op);                          \
}
NEON_FP2REGMISC_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC


void Assembler::NEONFP2RegMisc(const VRegister& vd,
                               const VRegister& vn,
                               Instr op) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  Emit(FPFormat(vd) | op | Rn(vn) | Rd(vd));
}


void Assembler::NEON2RegMisc(const VRegister& vd,
                             const VRegister& vn,
                             NEON2RegMiscOp vop,
                             int value) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(value == 0);
  USE(value);

  Instr format, op = vop;
  if (vd.IsScalar()) {
    op |= NEON_Q | NEONScalar;
    format = SFormat(vd);
  } else {
    format = VFormat(vd);
  }

  Emit(format | op | Rn(vn) | Rd(vd));
}


void Assembler::cmeq(const VRegister& vd,
                     const VRegister& vn,
                     int value) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEON2RegMisc(vd, vn, NEON_CMEQ_zero, value);
}


void Assembler::cmge(const VRegister& vd,
                     const VRegister& vn,
                     int value) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEON2RegMisc(vd, vn, NEON_CMGE_zero, value);
}


void Assembler::cmgt(const VRegister& vd,
                     const VRegister& vn,
                     int value) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEON2RegMisc(vd, vn, NEON_CMGT_zero, value);
}


void Assembler::cmle(const VRegister& vd,
                     const VRegister& vn,
                     int value) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEON2RegMisc(vd, vn, NEON_CMLE_zero, value);
}


void Assembler::cmlt(const VRegister& vd,
                     const VRegister& vn,
                     int value) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEON2RegMisc(vd, vn, NEON_CMLT_zero, value);
}


void Assembler::shll(const VRegister& vd,
                     const VRegister& vn,
                     int shift) {
  VIXL_ASSERT((vd.Is8H() && vn.Is8B() && shift == 8) ||
              (vd.Is4S() && vn.Is4H() && shift == 16) ||
              (vd.Is2D() && vn.Is2S() && shift == 32));
  USE(shift);
  Emit(VFormat(vn) | NEON_SHLL | Rn(vn) | Rd(vd));
}


void Assembler::shll2(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  USE(shift);
  VIXL_ASSERT((vd.Is8H() && vn.Is16B() && shift == 8) ||
              (vd.Is4S() && vn.Is8H() && shift == 16) ||
              (vd.Is2D() && vn.Is4S() && shift == 32));
  Emit(VFormat(vn) | NEON_SHLL | Rn(vn) | Rd(vd));
}


void Assembler::NEONFP2RegMisc(const VRegister& vd,
                               const VRegister& vn,
                               NEON2RegMiscOp vop,
                               double value) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(value == 0.0);
  USE(value);

  Instr op = vop;
  if (vd.IsScalar()) {
    VIXL_ASSERT(vd.Is1S() || vd.Is1D());
    op |= NEON_Q | NEONScalar;
  } else {
    VIXL_ASSERT(vd.Is2S() || vd.Is2D() || vd.Is4S());
  }

  Emit(FPFormat(vd) | op | Rn(vn) | Rd(vd));
}


void Assembler::fcmeq(const VRegister& vd,
                      const VRegister& vn,
                      double value) {
  NEONFP2RegMisc(vd, vn, NEON_FCMEQ_zero, value);
}


void Assembler::fcmge(const VRegister& vd,
                      const VRegister& vn,
                      double value) {
  NEONFP2RegMisc(vd, vn, NEON_FCMGE_zero, value);
}


void Assembler::fcmgt(const VRegister& vd,
                      const VRegister& vn,
                      double value) {
  NEONFP2RegMisc(vd, vn, NEON_FCMGT_zero, value);
}


void Assembler::fcmle(const VRegister& vd,
                      const VRegister& vn,
                      double value) {
  NEONFP2RegMisc(vd, vn, NEON_FCMLE_zero, value);
}


void Assembler::fcmlt(const VRegister& vd,
                      const VRegister& vn,
                      double value) {
  NEONFP2RegMisc(vd, vn, NEON_FCMLT_zero, value);
}


void Assembler::frecpx(const VRegister& vd,
                       const VRegister& vn) {
  VIXL_ASSERT(vd.IsScalar());
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(vd.Is1S() || vd.Is1D());
  Emit(FPFormat(vd) | NEON_FRECPX_scalar | Rn(vn) | Rd(vd));
}


#define NEON_3SAME_LIST(V) \
  V(add,      NEON_ADD,      vd.IsVector() || vd.Is1D())            \
  V(addp,     NEON_ADDP,     vd.IsVector() || vd.Is1D())            \
  V(sub,      NEON_SUB,      vd.IsVector() || vd.Is1D())            \
  V(cmeq,     NEON_CMEQ,     vd.IsVector() || vd.Is1D())            \
  V(cmge,     NEON_CMGE,     vd.IsVector() || vd.Is1D())            \
  V(cmgt,     NEON_CMGT,     vd.IsVector() || vd.Is1D())            \
  V(cmhi,     NEON_CMHI,     vd.IsVector() || vd.Is1D())            \
  V(cmhs,     NEON_CMHS,     vd.IsVector() || vd.Is1D())            \
  V(cmtst,    NEON_CMTST,    vd.IsVector() || vd.Is1D())            \
  V(sshl,     NEON_SSHL,     vd.IsVector() || vd.Is1D())            \
  V(ushl,     NEON_USHL,     vd.IsVector() || vd.Is1D())            \
  V(srshl,    NEON_SRSHL,    vd.IsVector() || vd.Is1D())            \
  V(urshl,    NEON_URSHL,    vd.IsVector() || vd.Is1D())            \
  V(sqdmulh,  NEON_SQDMULH,  vd.IsLaneSizeH() || vd.IsLaneSizeS())  \
  V(sqrdmulh, NEON_SQRDMULH, vd.IsLaneSizeH() || vd.IsLaneSizeS())  \
  V(shadd,    NEON_SHADD,    vd.IsVector() && !vd.IsLaneSizeD())    \
  V(uhadd,    NEON_UHADD,    vd.IsVector() && !vd.IsLaneSizeD())    \
  V(srhadd,   NEON_SRHADD,   vd.IsVector() && !vd.IsLaneSizeD())    \
  V(urhadd,   NEON_URHADD,   vd.IsVector() && !vd.IsLaneSizeD())    \
  V(shsub,    NEON_SHSUB,    vd.IsVector() && !vd.IsLaneSizeD())    \
  V(uhsub,    NEON_UHSUB,    vd.IsVector() && !vd.IsLaneSizeD())    \
  V(smax,     NEON_SMAX,     vd.IsVector() && !vd.IsLaneSizeD())    \
  V(smaxp,    NEON_SMAXP,    vd.IsVector() && !vd.IsLaneSizeD())    \
  V(smin,     NEON_SMIN,     vd.IsVector() && !vd.IsLaneSizeD())    \
  V(sminp,    NEON_SMINP,    vd.IsVector() && !vd.IsLaneSizeD())    \
  V(umax,     NEON_UMAX,     vd.IsVector() && !vd.IsLaneSizeD())    \
  V(umaxp,    NEON_UMAXP,    vd.IsVector() && !vd.IsLaneSizeD())    \
  V(umin,     NEON_UMIN,     vd.IsVector() && !vd.IsLaneSizeD())    \
  V(uminp,    NEON_UMINP,    vd.IsVector() && !vd.IsLaneSizeD())    \
  V(saba,     NEON_SABA,     vd.IsVector() && !vd.IsLaneSizeD())    \
  V(sabd,     NEON_SABD,     vd.IsVector() && !vd.IsLaneSizeD())    \
  V(uaba,     NEON_UABA,     vd.IsVector() && !vd.IsLaneSizeD())    \
  V(uabd,     NEON_UABD,     vd.IsVector() && !vd.IsLaneSizeD())    \
  V(mla,      NEON_MLA,      vd.IsVector() && !vd.IsLaneSizeD())    \
  V(mls,      NEON_MLS,      vd.IsVector() && !vd.IsLaneSizeD())    \
  V(mul,      NEON_MUL,      vd.IsVector() && !vd.IsLaneSizeD())    \
  V(and_,     NEON_AND,      vd.Is8B() || vd.Is16B())               \
  V(orr,      NEON_ORR,      vd.Is8B() || vd.Is16B())               \
  V(orn,      NEON_ORN,      vd.Is8B() || vd.Is16B())               \
  V(eor,      NEON_EOR,      vd.Is8B() || vd.Is16B())               \
  V(bic,      NEON_BIC,      vd.Is8B() || vd.Is16B())               \
  V(bit,      NEON_BIT,      vd.Is8B() || vd.Is16B())               \
  V(bif,      NEON_BIF,      vd.Is8B() || vd.Is16B())               \
  V(bsl,      NEON_BSL,      vd.Is8B() || vd.Is16B())               \
  V(pmul,     NEON_PMUL,     vd.Is8B() || vd.Is16B())               \
  V(uqadd,    NEON_UQADD,    true)                                  \
  V(sqadd,    NEON_SQADD,    true)                                  \
  V(uqsub,    NEON_UQSUB,    true)                                  \
  V(sqsub,    NEON_SQSUB,    true)                                  \
  V(sqshl,    NEON_SQSHL,    true)                                  \
  V(uqshl,    NEON_UQSHL,    true)                                  \
  V(sqrshl,   NEON_SQRSHL,   true)                                  \
  V(uqrshl,   NEON_UQRSHL,   true)

#define DEFINE_ASM_FUNC(FN, OP, AS)        \
void Assembler::FN(const VRegister& vd,    \
                   const VRegister& vn,    \
                   const VRegister& vm) {  \
  VIXL_ASSERT(AS);                         \
  NEON3Same(vd, vn, vm, OP);               \
}
NEON_3SAME_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC


#define NEON_FP3SAME_OP_LIST(V)                  \
  V(fadd,    NEON_FADD,    FADD)                 \
  V(fsub,    NEON_FSUB,    FSUB)                 \
  V(fmul,    NEON_FMUL,    FMUL)                 \
  V(fdiv,    NEON_FDIV,    FDIV)                 \
  V(fmax,    NEON_FMAX,    FMAX)                 \
  V(fmaxnm,  NEON_FMAXNM,  FMAXNM)               \
  V(fmin,    NEON_FMIN,    FMIN)                 \
  V(fminnm,  NEON_FMINNM,  FMINNM)               \
  V(fmulx,   NEON_FMULX,   NEON_FMULX_scalar)    \
  V(frecps,  NEON_FRECPS,  NEON_FRECPS_scalar)   \
  V(frsqrts, NEON_FRSQRTS, NEON_FRSQRTS_scalar)  \
  V(fabd,    NEON_FABD,    NEON_FABD_scalar)     \
  V(fmla,    NEON_FMLA,    0)                    \
  V(fmls,    NEON_FMLS,    0)                    \
  V(facge,   NEON_FACGE,   NEON_FACGE_scalar)    \
  V(facgt,   NEON_FACGT,   NEON_FACGT_scalar)    \
  V(fcmeq,   NEON_FCMEQ,   NEON_FCMEQ_scalar)    \
  V(fcmge,   NEON_FCMGE,   NEON_FCMGE_scalar)    \
  V(fcmgt,   NEON_FCMGT,   NEON_FCMGT_scalar)    \
  V(faddp,   NEON_FADDP,   0)                    \
  V(fmaxp,   NEON_FMAXP,   0)                    \
  V(fminp,   NEON_FMINP,   0)                    \
  V(fmaxnmp, NEON_FMAXNMP, 0)                    \
  V(fminnmp, NEON_FMINNMP, 0)

#define DEFINE_ASM_FUNC(FN, VEC_OP, SCA_OP)            \
void Assembler::FN(const VRegister& vd,                \
                   const VRegister& vn,                \
                   const VRegister& vm) {              \
  Instr op;                                            \
  if ((SCA_OP != 0) && vd.IsScalar()) {                \
    VIXL_ASSERT(vd.Is1S() || vd.Is1D());               \
    op = SCA_OP;                                       \
  } else {                                             \
    VIXL_ASSERT(vd.IsVector());                        \
    VIXL_ASSERT(vd.Is2S() || vd.Is2D() || vd.Is4S());  \
    op = VEC_OP;                                       \
  }                                                    \
  NEONFP3Same(vd, vn, vm, op);                         \
}
NEON_FP3SAME_OP_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC


void Assembler::addp(const VRegister& vd,
                     const VRegister& vn) {
  VIXL_ASSERT((vd.Is1D() && vn.Is2D()));
  Emit(SFormat(vd) | NEON_ADDP_scalar | Rn(vn) | Rd(vd));
}


void Assembler::faddp(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT((vd.Is1S() && vn.Is2S()) ||
              (vd.Is1D() && vn.Is2D()));
  Emit(FPFormat(vd) | NEON_FADDP_scalar | Rn(vn) | Rd(vd));
}


void Assembler::fmaxp(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT((vd.Is1S() && vn.Is2S()) ||
              (vd.Is1D() && vn.Is2D()));
  Emit(FPFormat(vd) | NEON_FMAXP_scalar | Rn(vn) | Rd(vd));
}


void Assembler::fminp(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT((vd.Is1S() && vn.Is2S()) ||
              (vd.Is1D() && vn.Is2D()));
  Emit(FPFormat(vd) | NEON_FMINP_scalar | Rn(vn) | Rd(vd));
}


void Assembler::fmaxnmp(const VRegister& vd,
                        const VRegister& vn) {
  VIXL_ASSERT((vd.Is1S() && vn.Is2S()) ||
              (vd.Is1D() && vn.Is2D()));
  Emit(FPFormat(vd) | NEON_FMAXNMP_scalar | Rn(vn) | Rd(vd));
}


void Assembler::fminnmp(const VRegister& vd,
                        const VRegister& vn) {
  VIXL_ASSERT((vd.Is1S() && vn.Is2S()) ||
              (vd.Is1D() && vn.Is2D()));
  Emit(FPFormat(vd) | NEON_FMINNMP_scalar | Rn(vn) | Rd(vd));
}


void Assembler::orr(const VRegister& vd,
                    const int imm8,
                    const int left_shift) {
  NEONModifiedImmShiftLsl(vd, imm8, left_shift,
                          NEONModifiedImmediate_ORR);
}


void Assembler::mov(const VRegister& vd,
                    const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  if (vd.IsD()) {
    orr(vd.V8B(), vn.V8B(), vn.V8B());
  } else {
    VIXL_ASSERT(vd.IsQ());
    orr(vd.V16B(), vn.V16B(), vn.V16B());
  }
}


void Assembler::bic(const VRegister& vd,
                    const int imm8,
                    const int left_shift) {
  NEONModifiedImmShiftLsl(vd, imm8, left_shift,
                          NEONModifiedImmediate_BIC);
}


void Assembler::movi(const VRegister& vd,
                     const uint64_t imm,
                     Shift shift,
                     const int shift_amount) {
  VIXL_ASSERT((shift == LSL) || (shift == MSL));
  if (vd.Is2D() || vd.Is1D()) {
    VIXL_ASSERT(shift_amount == 0);
    int imm8 = 0;
    for (int i = 0; i < 8; ++i) {
      int byte = (imm >> (i * 8)) & 0xff;
      VIXL_ASSERT((byte == 0) || (byte == 0xff));
      if (byte == 0xff) {
        imm8 |= (1 << i);
      }
    }
    int q = vd.Is2D() ? NEON_Q : 0;
    Emit(q | NEONModImmOp(1) | NEONModifiedImmediate_MOVI |
         ImmNEONabcdefgh(imm8) | NEONCmode(0xe) | Rd(vd));
  } else if (shift == LSL) {
    VIXL_ASSERT(IsUint8(imm));
    NEONModifiedImmShiftLsl(vd, static_cast<int>(imm), shift_amount,
                            NEONModifiedImmediate_MOVI);
  } else {
    VIXL_ASSERT(IsUint8(imm));
    NEONModifiedImmShiftMsl(vd, static_cast<int>(imm), shift_amount,
                            NEONModifiedImmediate_MOVI);
  }
}


void Assembler::mvn(const VRegister& vd,
                    const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  if (vd.IsD()) {
    not_(vd.V8B(), vn.V8B());
  } else {
    VIXL_ASSERT(vd.IsQ());
    not_(vd.V16B(), vn.V16B());
  }
}


void Assembler::mvni(const VRegister& vd,
                     const int imm8,
                     Shift shift,
                     const int shift_amount) {
  VIXL_ASSERT((shift == LSL) || (shift == MSL));
  if (shift == LSL) {
    NEONModifiedImmShiftLsl(vd, imm8, shift_amount,
                            NEONModifiedImmediate_MVNI);
  } else {
    NEONModifiedImmShiftMsl(vd, imm8, shift_amount,
                            NEONModifiedImmediate_MVNI);
  }
}


void Assembler::NEONFPByElement(const VRegister& vd,
                                const VRegister& vn,
                                const VRegister& vm,
                                int vm_index,
                                NEONByIndexedElementOp vop) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT((vd.Is2S() && vm.Is1S()) ||
              (vd.Is4S() && vm.Is1S()) ||
              (vd.Is1S() && vm.Is1S()) ||
              (vd.Is2D() && vm.Is1D()) ||
              (vd.Is1D() && vm.Is1D()));
  VIXL_ASSERT((vm.Is1S() && (vm_index < 4)) ||
              (vm.Is1D() && (vm_index < 2)));

  Instr op = vop;
  int index_num_bits = vm.Is1S() ? 2 : 1;
  if (vd.IsScalar()) {
    op |= NEON_Q | NEONScalar;
  }

  Emit(FPFormat(vd) | op | ImmNEONHLM(vm_index, index_num_bits) |
       Rm(vm) | Rn(vn) | Rd(vd));
}


void Assembler::NEONByElement(const VRegister& vd,
                              const VRegister& vn,
                              const VRegister& vm,
                              int vm_index,
                              NEONByIndexedElementOp vop) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT((vd.Is4H() && vm.Is1H()) ||
              (vd.Is8H() && vm.Is1H()) ||
              (vd.Is1H() && vm.Is1H()) ||
              (vd.Is2S() && vm.Is1S()) ||
              (vd.Is4S() && vm.Is1S()) ||
              (vd.Is1S() && vm.Is1S()));
  VIXL_ASSERT((vm.Is1H() && (vm.code() < 16) && (vm_index < 8)) ||
              (vm.Is1S() && (vm_index < 4)));

  Instr format, op = vop;
  int index_num_bits = vm.Is1H() ? 3 : 2;
  if (vd.IsScalar()) {
    op |= NEONScalar | NEON_Q;
    format = SFormat(vn);
  } else {
    format = VFormat(vn);
  }
  Emit(format | op | ImmNEONHLM(vm_index, index_num_bits) |
       Rm(vm) | Rn(vn) | Rd(vd));
}


void Assembler::NEONByElementL(const VRegister& vd,
                               const VRegister& vn,
                               const VRegister& vm,
                               int vm_index,
                               NEONByIndexedElementOp vop) {
  VIXL_ASSERT((vd.Is4S() && vn.Is4H() && vm.Is1H()) ||
              (vd.Is4S() && vn.Is8H() && vm.Is1H()) ||
              (vd.Is1S() && vn.Is1H() && vm.Is1H()) ||
              (vd.Is2D() && vn.Is2S() && vm.Is1S()) ||
              (vd.Is2D() && vn.Is4S() && vm.Is1S()) ||
              (vd.Is1D() && vn.Is1S() && vm.Is1S()));

  VIXL_ASSERT((vm.Is1H() && (vm.code() < 16) && (vm_index < 8)) ||
              (vm.Is1S() && (vm_index < 4)));

  Instr format, op = vop;
  int index_num_bits = vm.Is1H() ? 3 : 2;
  if (vd.IsScalar()) {
    op |= NEONScalar | NEON_Q;
    format = SFormat(vn);
  } else {
    format = VFormat(vn);
  }
  Emit(format | op | ImmNEONHLM(vm_index, index_num_bits) |
       Rm(vm) | Rn(vn) | Rd(vd));
}


#define NEON_BYELEMENT_LIST(V)                         \
  V(mul,      NEON_MUL_byelement,      vn.IsVector())  \
  V(mla,      NEON_MLA_byelement,      vn.IsVector())  \
  V(mls,      NEON_MLS_byelement,      vn.IsVector())  \
  V(sqdmulh,  NEON_SQDMULH_byelement,  true)           \
  V(sqrdmulh, NEON_SQRDMULH_byelement, true)


#define DEFINE_ASM_FUNC(FN, OP, AS)        \
void Assembler::FN(const VRegister& vd,    \
                   const VRegister& vn,    \
                   const VRegister& vm,    \
                   int vm_index) {         \
  VIXL_ASSERT(AS);                         \
  NEONByElement(vd, vn, vm, vm_index, OP); \
}
NEON_BYELEMENT_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC


#define NEON_FPBYELEMENT_LIST(V) \
  V(fmul,  NEON_FMUL_byelement)  \
  V(fmla,  NEON_FMLA_byelement)  \
  V(fmls,  NEON_FMLS_byelement)  \
  V(fmulx, NEON_FMULX_byelement)


#define DEFINE_ASM_FUNC(FN, OP)              \
void Assembler::FN(const VRegister& vd,      \
                   const VRegister& vn,      \
                   const VRegister& vm,      \
                   int vm_index) {           \
  NEONFPByElement(vd, vn, vm, vm_index, OP); \
}
NEON_FPBYELEMENT_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC


#define NEON_BYELEMENT_LONG_LIST(V)                               \
  V(sqdmull,  NEON_SQDMULL_byelement, vn.IsScalar() || vn.IsD())  \
  V(sqdmull2, NEON_SQDMULL_byelement, vn.IsVector() && vn.IsQ())  \
  V(sqdmlal,  NEON_SQDMLAL_byelement, vn.IsScalar() || vn.IsD())  \
  V(sqdmlal2, NEON_SQDMLAL_byelement, vn.IsVector() && vn.IsQ())  \
  V(sqdmlsl,  NEON_SQDMLSL_byelement, vn.IsScalar() || vn.IsD())  \
  V(sqdmlsl2, NEON_SQDMLSL_byelement, vn.IsVector() && vn.IsQ())  \
  V(smull,    NEON_SMULL_byelement,   vn.IsVector() && vn.IsD())  \
  V(smull2,   NEON_SMULL_byelement,   vn.IsVector() && vn.IsQ())  \
  V(umull,    NEON_UMULL_byelement,   vn.IsVector() && vn.IsD())  \
  V(umull2,   NEON_UMULL_byelement,   vn.IsVector() && vn.IsQ())  \
  V(smlal,    NEON_SMLAL_byelement,   vn.IsVector() && vn.IsD())  \
  V(smlal2,   NEON_SMLAL_byelement,   vn.IsVector() && vn.IsQ())  \
  V(umlal,    NEON_UMLAL_byelement,   vn.IsVector() && vn.IsD())  \
  V(umlal2,   NEON_UMLAL_byelement,   vn.IsVector() && vn.IsQ())  \
  V(smlsl,    NEON_SMLSL_byelement,   vn.IsVector() && vn.IsD())  \
  V(smlsl2,   NEON_SMLSL_byelement,   vn.IsVector() && vn.IsQ())  \
  V(umlsl,    NEON_UMLSL_byelement,   vn.IsVector() && vn.IsD())  \
  V(umlsl2,   NEON_UMLSL_byelement,   vn.IsVector() && vn.IsQ())


#define DEFINE_ASM_FUNC(FN, OP, AS)         \
void Assembler::FN(const VRegister& vd,     \
                   const VRegister& vn,     \
                   const VRegister& vm,     \
                   int vm_index) {          \
  VIXL_ASSERT(AS);                          \
  NEONByElementL(vd, vn, vm, vm_index, OP); \
}
NEON_BYELEMENT_LONG_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC


void Assembler::suqadd(const VRegister& vd,
                       const VRegister& vn) {
  NEON2RegMisc(vd, vn, NEON_SUQADD);
}


void Assembler::usqadd(const VRegister& vd,
                       const VRegister& vn) {
  NEON2RegMisc(vd, vn, NEON_USQADD);
}


void Assembler::abs(const VRegister& vd,
                    const VRegister& vn) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEON2RegMisc(vd, vn, NEON_ABS);
}


void Assembler::sqabs(const VRegister& vd,
                      const VRegister& vn) {
  NEON2RegMisc(vd, vn, NEON_SQABS);
}


void Assembler::neg(const VRegister& vd,
                    const VRegister& vn) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEON2RegMisc(vd, vn, NEON_NEG);
}


void Assembler::sqneg(const VRegister& vd,
                      const VRegister& vn) {
  NEON2RegMisc(vd, vn, NEON_SQNEG);
}


void Assembler::NEONXtn(const VRegister& vd,
                        const VRegister& vn,
                        NEON2RegMiscOp vop) {
  Instr format, op = vop;
  if (vd.IsScalar()) {
    VIXL_ASSERT((vd.Is1B() && vn.Is1H()) ||
                (vd.Is1H() && vn.Is1S()) ||
                (vd.Is1S() && vn.Is1D()));
    op |= NEON_Q | NEONScalar;
    format = SFormat(vd);
  } else {
    VIXL_ASSERT((vd.Is8B() && vn.Is8H())  ||
                (vd.Is4H() && vn.Is4S())  ||
                (vd.Is2S() && vn.Is2D())  ||
                (vd.Is16B() && vn.Is8H()) ||
                (vd.Is8H() && vn.Is4S())  ||
                (vd.Is4S() && vn.Is2D()));
    format = VFormat(vd);
  }
  Emit(format | op | Rn(vn) | Rd(vd));
}


void Assembler::xtn(const VRegister& vd,
                    const VRegister& vn) {
  VIXL_ASSERT(vd.IsVector() && vd.IsD());
  NEONXtn(vd, vn, NEON_XTN);
}


void Assembler::xtn2(const VRegister& vd,
                     const VRegister& vn) {
  VIXL_ASSERT(vd.IsVector() && vd.IsQ());
  NEONXtn(vd, vn, NEON_XTN);
}


void Assembler::sqxtn(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT(vd.IsScalar() || vd.IsD());
  NEONXtn(vd, vn, NEON_SQXTN);
}


void Assembler::sqxtn2(const VRegister& vd,
                       const VRegister& vn) {
  VIXL_ASSERT(vd.IsVector() && vd.IsQ());
  NEONXtn(vd, vn, NEON_SQXTN);
}


void Assembler::sqxtun(const VRegister& vd,
                       const VRegister& vn) {
  VIXL_ASSERT(vd.IsScalar() || vd.IsD());
  NEONXtn(vd, vn, NEON_SQXTUN);
}


void Assembler::sqxtun2(const VRegister& vd,
                        const VRegister& vn) {
  VIXL_ASSERT(vd.IsVector() && vd.IsQ());
  NEONXtn(vd, vn, NEON_SQXTUN);
}


void Assembler::uqxtn(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT(vd.IsScalar() || vd.IsD());
  NEONXtn(vd, vn, NEON_UQXTN);
}


void Assembler::uqxtn2(const VRegister& vd,
                       const VRegister& vn) {
  VIXL_ASSERT(vd.IsVector() && vd.IsQ());
  NEONXtn(vd, vn, NEON_UQXTN);
}


// NEON NOT and RBIT are distinguised by bit 22, the bottom bit of "size".
void Assembler::not_(const VRegister& vd,
                     const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(vd.Is8B() || vd.Is16B());
  Emit(VFormat(vd) | NEON_RBIT_NOT | Rn(vn) | Rd(vd));
}


void Assembler::rbit(const VRegister& vd,
                     const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(vd.Is8B() || vd.Is16B());
  Emit(VFormat(vn) | (1 << NEONSize_offset) | NEON_RBIT_NOT | Rn(vn) | Rd(vd));
}


void Assembler::ext(const VRegister& vd,
                    const VRegister& vn,
                    const VRegister& vm,
                    int index) {
  VIXL_ASSERT(AreSameFormat(vd, vn, vm));
  VIXL_ASSERT(vd.Is8B() || vd.Is16B());
  VIXL_ASSERT((0 <= index) && (index < vd.lanes()));
  Emit(VFormat(vd) | NEON_EXT | Rm(vm) | ImmNEONExt(index) | Rn(vn) | Rd(vd));
}


void Assembler::dup(const VRegister& vd,
                    const VRegister& vn,
                    int vn_index) {
  Instr q, scalar;

  // We support vn arguments of the form vn.VxT() or vn.T(), where x is the
  // number of lanes, and T is b, h, s or d.
  int lane_size = vn.LaneSizeInBytes();
  NEONFormatField format;
  switch (lane_size) {
    case 1: format = NEON_16B; break;
    case 2: format = NEON_8H;  break;
    case 4: format = NEON_4S;  break;
    default:
      VIXL_ASSERT(lane_size == 8);
      format = NEON_2D;
      break;
  }

  if (vd.IsScalar()) {
    q = NEON_Q;
    scalar = NEONScalar;
  } else {
    VIXL_ASSERT(!vd.Is1D());
    q = vd.IsD() ? 0 : NEON_Q;
    scalar = 0;
  }
  Emit(q | scalar | NEON_DUP_ELEMENT |
       ImmNEON5(format, vn_index) | Rn(vn) | Rd(vd));
}


void Assembler::mov(const VRegister& vd,
                    const VRegister& vn,
                    int vn_index) {
  VIXL_ASSERT(vn.IsScalar());
  dup(vd, vn, vn_index);
}


void Assembler::dup(const VRegister& vd, const Register& rn) {
  VIXL_ASSERT(!vd.Is1D());
  VIXL_ASSERT(vd.Is2D() == rn.IsX());
  int q = vd.IsD() ? 0 : NEON_Q;
  Emit(q | NEON_DUP_GENERAL | ImmNEON5(VFormat(vd), 0) | Rn(rn) | Rd(vd));
}


void Assembler::ins(const VRegister& vd,
                    int vd_index,
                    const VRegister& vn,
                    int vn_index) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  // We support vd arguments of the form vd.VxT() or vd.T(), where x is the
  // number of lanes, and T is b, h, s or d.
  int lane_size = vd.LaneSizeInBytes();
  NEONFormatField format;
  switch (lane_size) {
    case 1: format = NEON_16B; break;
    case 2: format = NEON_8H;  break;
    case 4: format = NEON_4S;  break;
    default:
      VIXL_ASSERT(lane_size == 8);
      format = NEON_2D;
      break;
  }

  VIXL_ASSERT((0 <= vd_index) &&
          (vd_index < LaneCountFromFormat(static_cast<VectorFormat>(format))));
  VIXL_ASSERT((0 <= vn_index) &&
          (vn_index < LaneCountFromFormat(static_cast<VectorFormat>(format))));
  Emit(NEON_INS_ELEMENT | ImmNEON5(format, vd_index) |
       ImmNEON4(format, vn_index) | Rn(vn) | Rd(vd));
}


void Assembler::mov(const VRegister& vd,
                    int vd_index,
                    const VRegister& vn,
                    int vn_index) {
  ins(vd, vd_index, vn, vn_index);
}


void Assembler::ins(const VRegister& vd,
                    int vd_index,
                    const Register& rn) {
  // We support vd arguments of the form vd.VxT() or vd.T(), where x is the
  // number of lanes, and T is b, h, s or d.
  int lane_size = vd.LaneSizeInBytes();
  NEONFormatField format;
  switch (lane_size) {
    case 1: format = NEON_16B; VIXL_ASSERT(rn.IsW()); break;
    case 2: format = NEON_8H;  VIXL_ASSERT(rn.IsW()); break;
    case 4: format = NEON_4S;  VIXL_ASSERT(rn.IsW()); break;
    default:
      VIXL_ASSERT(lane_size == 8);
      VIXL_ASSERT(rn.IsX());
      format = NEON_2D;
      break;
  }

  VIXL_ASSERT((0 <= vd_index) &&
          (vd_index < LaneCountFromFormat(static_cast<VectorFormat>(format))));
  Emit(NEON_INS_GENERAL | ImmNEON5(format, vd_index) | Rn(rn) | Rd(vd));
}


void Assembler::mov(const VRegister& vd,
                    int vd_index,
                    const Register& rn) {
  ins(vd, vd_index, rn);
}


void Assembler::umov(const Register& rd,
                     const VRegister& vn,
                     int vn_index) {
  // We support vd arguments of the form vd.VxT() or vd.T(), where x is the
  // number of lanes, and T is b, h, s or d.
  int lane_size = vn.LaneSizeInBytes();
  NEONFormatField format;
  Instr q = 0;
  switch (lane_size) {
    case 1: format = NEON_16B; VIXL_ASSERT(rd.IsW()); break;
    case 2: format = NEON_8H;  VIXL_ASSERT(rd.IsW()); break;
    case 4: format = NEON_4S;  VIXL_ASSERT(rd.IsW()); break;
    default:
      VIXL_ASSERT(lane_size == 8);
      VIXL_ASSERT(rd.IsX());
      format = NEON_2D;
      q = NEON_Q;
      break;
  }

  VIXL_ASSERT((0 <= vn_index) &&
          (vn_index < LaneCountFromFormat(static_cast<VectorFormat>(format))));
  Emit(q | NEON_UMOV | ImmNEON5(format, vn_index) | Rn(vn) | Rd(rd));
}


void Assembler::mov(const Register& rd,
                    const VRegister& vn,
                    int vn_index) {
  VIXL_ASSERT(vn.SizeInBytes() >= 4);
  umov(rd, vn, vn_index);
}


void Assembler::smov(const Register& rd,
                     const VRegister& vn,
                     int vn_index) {
  // We support vd arguments of the form vd.VxT() or vd.T(), where x is the
  // number of lanes, and T is b, h, s.
  int lane_size = vn.LaneSizeInBytes();
  NEONFormatField format;
  Instr q = 0;
  VIXL_ASSERT(lane_size != 8);
  switch (lane_size) {
    case 1: format = NEON_16B; break;
    case 2: format = NEON_8H;  break;
    default:
      VIXL_ASSERT(lane_size == 4);
      VIXL_ASSERT(rd.IsX());
      format = NEON_4S;
      break;
  }
  q = rd.IsW() ? 0 : NEON_Q;
  VIXL_ASSERT((0 <= vn_index) &&
          (vn_index < LaneCountFromFormat(static_cast<VectorFormat>(format))));
  Emit(q | NEON_SMOV | ImmNEON5(format, vn_index) | Rn(vn) | Rd(rd));
}


void Assembler::cls(const VRegister& vd,
                    const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(!vd.Is1D() && !vd.Is2D());
  Emit(VFormat(vn) | NEON_CLS | Rn(vn) | Rd(vd));
}


void Assembler::clz(const VRegister& vd,
                    const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(!vd.Is1D() && !vd.Is2D());
  Emit(VFormat(vn) | NEON_CLZ | Rn(vn) | Rd(vd));
}


void Assembler::cnt(const VRegister& vd,
                    const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(vd.Is8B() || vd.Is16B());
  Emit(VFormat(vn) | NEON_CNT | Rn(vn) | Rd(vd));
}


void Assembler::rev16(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(vd.Is8B() || vd.Is16B());
  Emit(VFormat(vn) | NEON_REV16 | Rn(vn) | Rd(vd));
}


void Assembler::rev32(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(vd.Is8B() || vd.Is16B() || vd.Is4H() || vd.Is8H());
  Emit(VFormat(vn) | NEON_REV32 | Rn(vn) | Rd(vd));
}


void Assembler::rev64(const VRegister& vd,
                      const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(!vd.Is1D() && !vd.Is2D());
  Emit(VFormat(vn) | NEON_REV64 | Rn(vn) | Rd(vd));
}


void Assembler::ursqrte(const VRegister& vd,
                        const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(vd.Is2S() || vd.Is4S());
  Emit(VFormat(vn) | NEON_URSQRTE | Rn(vn) | Rd(vd));
}


void Assembler::urecpe(const VRegister& vd,
                       const VRegister& vn) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  VIXL_ASSERT(vd.Is2S() || vd.Is4S());
  Emit(VFormat(vn) | NEON_URECPE | Rn(vn) | Rd(vd));
}


void Assembler::NEONAddlp(const VRegister& vd,
                          const VRegister& vn,
                          NEON2RegMiscOp op) {
  VIXL_ASSERT((op == NEON_SADDLP) ||
              (op == NEON_UADDLP) ||
              (op == NEON_SADALP) ||
              (op == NEON_UADALP));

  VIXL_ASSERT((vn.Is8B() && vd.Is4H()) ||
              (vn.Is4H() && vd.Is2S()) ||
              (vn.Is2S() && vd.Is1D()) ||
              (vn.Is16B() && vd.Is8H())||
              (vn.Is8H() && vd.Is4S()) ||
              (vn.Is4S() && vd.Is2D()));
  Emit(VFormat(vn) | op | Rn(vn) | Rd(vd));
}


void Assembler::saddlp(const VRegister& vd,
                       const VRegister& vn) {
  NEONAddlp(vd, vn, NEON_SADDLP);
}


void Assembler::uaddlp(const VRegister& vd,
                       const VRegister& vn) {
  NEONAddlp(vd, vn, NEON_UADDLP);
}


void Assembler::sadalp(const VRegister& vd,
                       const VRegister& vn) {
  NEONAddlp(vd, vn, NEON_SADALP);
}


void Assembler::uadalp(const VRegister& vd,
                       const VRegister& vn) {
  NEONAddlp(vd, vn, NEON_UADALP);
}


void Assembler::NEONAcrossLanesL(const VRegister& vd,
                                 const VRegister& vn,
                                 NEONAcrossLanesOp op) {
  VIXL_ASSERT((vn.Is8B()  && vd.Is1H()) ||
              (vn.Is16B() && vd.Is1H()) ||
              (vn.Is4H()  && vd.Is1S()) ||
              (vn.Is8H()  && vd.Is1S()) ||
              (vn.Is4S()  && vd.Is1D()));
  Emit(VFormat(vn) | op | Rn(vn) | Rd(vd));
}


void Assembler::saddlv(const VRegister& vd,
                       const VRegister& vn) {
  NEONAcrossLanesL(vd, vn, NEON_SADDLV);
}


void Assembler::uaddlv(const VRegister& vd,
                       const VRegister& vn) {
  NEONAcrossLanesL(vd, vn, NEON_UADDLV);
}


void Assembler::NEONAcrossLanes(const VRegister& vd,
                                const VRegister& vn,
                                NEONAcrossLanesOp op) {
  VIXL_ASSERT((vn.Is8B()  && vd.Is1B()) ||
              (vn.Is16B() && vd.Is1B()) ||
              (vn.Is4H()  && vd.Is1H()) ||
              (vn.Is8H()  && vd.Is1H()) ||
              (vn.Is4S()  && vd.Is1S()));
  if ((op & NEONAcrossLanesFPFMask) == NEONAcrossLanesFPFixed) {
    Emit(FPFormat(vn) | op | Rn(vn) | Rd(vd));
  } else {
    Emit(VFormat(vn) | op | Rn(vn) | Rd(vd));
  }
}


#define NEON_ACROSSLANES_LIST(V) \
  V(fmaxv,   NEON_FMAXV,   vd.Is1S()) \
  V(fminv,   NEON_FMINV,   vd.Is1S()) \
  V(fmaxnmv, NEON_FMAXNMV, vd.Is1S()) \
  V(fminnmv, NEON_FMINNMV, vd.Is1S()) \
  V(addv,    NEON_ADDV,    true)      \
  V(smaxv,   NEON_SMAXV,   true)      \
  V(sminv,   NEON_SMINV,   true)      \
  V(umaxv,   NEON_UMAXV,   true)      \
  V(uminv,   NEON_UMINV,   true)


#define DEFINE_ASM_FUNC(FN, OP, AS)        \
void Assembler::FN(const VRegister& vd,    \
                   const VRegister& vn) {  \
  VIXL_ASSERT(AS);                         \
  NEONAcrossLanes(vd, vn, OP);             \
}
NEON_ACROSSLANES_LIST(DEFINE_ASM_FUNC)
#undef DEFINE_ASM_FUNC


void Assembler::NEONPerm(const VRegister& vd,
                         const VRegister& vn,
                         const VRegister& vm,
                         NEONPermOp op) {
  VIXL_ASSERT(AreSameFormat(vd, vn, vm));
  VIXL_ASSERT(!vd.Is1D());
  Emit(VFormat(vd) | op | Rm(vm) | Rn(vn) | Rd(vd));
}


void Assembler::trn1(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm) {
  NEONPerm(vd, vn, vm, NEON_TRN1);
}


void Assembler::trn2(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm) {
  NEONPerm(vd, vn, vm, NEON_TRN2);
}


void Assembler::uzp1(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm) {
  NEONPerm(vd, vn, vm, NEON_UZP1);
}


void Assembler::uzp2(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm) {
  NEONPerm(vd, vn, vm, NEON_UZP2);
}


void Assembler::zip1(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm) {
  NEONPerm(vd, vn, vm, NEON_ZIP1);
}


void Assembler::zip2(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm) {
  NEONPerm(vd, vn, vm, NEON_ZIP2);
}


void Assembler::NEONShiftImmediate(const VRegister& vd,
                                   const VRegister& vn,
                                   NEONShiftImmediateOp op,
                                   int immh_immb) {
  VIXL_ASSERT(AreSameFormat(vd, vn));
  Instr q, scalar;
  if (vn.IsScalar()) {
    q = NEON_Q;
    scalar = NEONScalar;
  } else {
    q = vd.IsD() ? 0 : NEON_Q;
    scalar = 0;
  }
  Emit(q | op | scalar | immh_immb | Rn(vn) | Rd(vd));
}


void Assembler::NEONShiftLeftImmediate(const VRegister& vd,
                                       const VRegister& vn,
                                       int shift,
                                       NEONShiftImmediateOp op) {
  int laneSizeInBits = vn.LaneSizeInBits();
  VIXL_ASSERT((shift >= 0) && (shift < laneSizeInBits));
  NEONShiftImmediate(vd, vn, op, (laneSizeInBits + shift) << 16);
}


void Assembler::NEONShiftRightImmediate(const VRegister& vd,
                                        const VRegister& vn,
                                        int shift,
                                        NEONShiftImmediateOp op) {
  int laneSizeInBits = vn.LaneSizeInBits();
  VIXL_ASSERT((shift >= 1) && (shift <= laneSizeInBits));
  NEONShiftImmediate(vd, vn, op, ((2 * laneSizeInBits) - shift) << 16);
}


void Assembler::NEONShiftImmediateL(const VRegister& vd,
                                    const VRegister& vn,
                                    int shift,
                                    NEONShiftImmediateOp op) {
  int laneSizeInBits = vn.LaneSizeInBits();
  VIXL_ASSERT((shift >= 0) && (shift < laneSizeInBits));
  int immh_immb = (laneSizeInBits + shift) << 16;

  VIXL_ASSERT((vn.Is8B() && vd.Is8H()) ||
              (vn.Is4H() && vd.Is4S()) ||
              (vn.Is2S() && vd.Is2D()) ||
              (vn.Is16B() && vd.Is8H())||
              (vn.Is8H() && vd.Is4S()) ||
              (vn.Is4S() && vd.Is2D()));
  Instr q;
  q = vn.IsD() ? 0 : NEON_Q;
  Emit(q | op | immh_immb | Rn(vn) | Rd(vd));
}


void Assembler::NEONShiftImmediateN(const VRegister& vd,
                                    const VRegister& vn,
                                    int shift,
                                    NEONShiftImmediateOp op) {
  Instr q, scalar;
  int laneSizeInBits = vd.LaneSizeInBits();
  VIXL_ASSERT((shift >= 1) && (shift <= laneSizeInBits));
  int immh_immb = (2 * laneSizeInBits - shift) << 16;

  if (vn.IsScalar()) {
    VIXL_ASSERT((vd.Is1B() && vn.Is1H()) ||
                (vd.Is1H() && vn.Is1S()) ||
                (vd.Is1S() && vn.Is1D()));
    q = NEON_Q;
    scalar = NEONScalar;
  } else {
    VIXL_ASSERT((vd.Is8B() && vn.Is8H()) ||
                (vd.Is4H() && vn.Is4S()) ||
                (vd.Is2S() && vn.Is2D()) ||
                (vd.Is16B() && vn.Is8H())||
                (vd.Is8H() && vn.Is4S()) ||
                (vd.Is4S() && vn.Is2D()));
    scalar = 0;
    q = vd.IsD() ? 0 : NEON_Q;
  }
  Emit(q | op | scalar | immh_immb | Rn(vn) | Rd(vd));
}


void Assembler::shl(const VRegister& vd,
                    const VRegister& vn,
                    int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftLeftImmediate(vd, vn, shift, NEON_SHL);
}


void Assembler::sli(const VRegister& vd,
                    const VRegister& vn,
                    int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftLeftImmediate(vd, vn, shift, NEON_SLI);
}


void Assembler::sqshl(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  NEONShiftLeftImmediate(vd, vn, shift, NEON_SQSHL_imm);
}


void Assembler::sqshlu(const VRegister& vd,
                       const VRegister& vn,
                       int shift) {
  NEONShiftLeftImmediate(vd, vn, shift, NEON_SQSHLU);
}


void Assembler::uqshl(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  NEONShiftLeftImmediate(vd, vn, shift, NEON_UQSHL_imm);
}


void Assembler::sshll(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  VIXL_ASSERT(vn.IsD());
  NEONShiftImmediateL(vd, vn, shift, NEON_SSHLL);
}


void Assembler::sshll2(const VRegister& vd,
                       const VRegister& vn,
                       int shift) {
  VIXL_ASSERT(vn.IsQ());
  NEONShiftImmediateL(vd, vn, shift, NEON_SSHLL);
}


void Assembler::sxtl(const VRegister& vd,
                     const VRegister& vn) {
  sshll(vd, vn, 0);
}


void Assembler::sxtl2(const VRegister& vd,
                      const VRegister& vn) {
  sshll2(vd, vn, 0);
}


void Assembler::ushll(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  VIXL_ASSERT(vn.IsD());
  NEONShiftImmediateL(vd, vn, shift, NEON_USHLL);
}


void Assembler::ushll2(const VRegister& vd,
                       const VRegister& vn,
                       int shift) {
  VIXL_ASSERT(vn.IsQ());
  NEONShiftImmediateL(vd, vn, shift, NEON_USHLL);
}


void Assembler::uxtl(const VRegister& vd,
                     const VRegister& vn) {
  ushll(vd, vn, 0);
}


void Assembler::uxtl2(const VRegister& vd,
                      const VRegister& vn) {
  ushll2(vd, vn, 0);
}


void Assembler::sri(const VRegister& vd,
                    const VRegister& vn,
                    int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_SRI);
}


void Assembler::sshr(const VRegister& vd,
                     const VRegister& vn,
                     int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_SSHR);
}


void Assembler::ushr(const VRegister& vd,
                     const VRegister& vn,
                     int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_USHR);
}


void Assembler::srshr(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_SRSHR);
}


void Assembler::urshr(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_URSHR);
}


void Assembler::ssra(const VRegister& vd,
                     const VRegister& vn,
                     int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_SSRA);
}


void Assembler::usra(const VRegister& vd,
                     const VRegister& vn,
                     int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_USRA);
}


void Assembler::srsra(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_SRSRA);
}


void Assembler::ursra(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  VIXL_ASSERT(vd.IsVector() || vd.Is1D());
  NEONShiftRightImmediate(vd, vn, shift, NEON_URSRA);
}


void Assembler::shrn(const VRegister& vd,
                     const VRegister& vn,
                     int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsD());
  NEONShiftImmediateN(vd, vn, shift, NEON_SHRN);
}


void Assembler::shrn2(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsQ());
  NEONShiftImmediateN(vd, vn, shift, NEON_SHRN);
}


void Assembler::rshrn(const VRegister& vd,
                      const VRegister& vn,
                      int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsD());
  NEONShiftImmediateN(vd, vn, shift, NEON_RSHRN);
}


void Assembler::rshrn2(const VRegister& vd,
                       const VRegister& vn,
                       int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsQ());
  NEONShiftImmediateN(vd, vn, shift, NEON_RSHRN);
}


void Assembler::sqshrn(const VRegister& vd,
                       const VRegister& vn,
                       int shift) {
  VIXL_ASSERT(vd.IsD() || (vn.IsScalar() && vd.IsScalar()));
  NEONShiftImmediateN(vd, vn, shift, NEON_SQSHRN);
}


void Assembler::sqshrn2(const VRegister& vd,
                        const VRegister& vn,
                        int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsQ());
  NEONShiftImmediateN(vd, vn, shift, NEON_SQSHRN);
}


void Assembler::sqrshrn(const VRegister& vd,
                        const VRegister& vn,
                        int shift) {
  VIXL_ASSERT(vd.IsD() || (vn.IsScalar() && vd.IsScalar()));
  NEONShiftImmediateN(vd, vn, shift, NEON_SQRSHRN);
}


void Assembler::sqrshrn2(const VRegister& vd,
                         const VRegister& vn,
                         int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsQ());
  NEONShiftImmediateN(vd, vn, shift, NEON_SQRSHRN);
}


void Assembler::sqshrun(const VRegister& vd,
                        const VRegister& vn,
                        int shift) {
  VIXL_ASSERT(vd.IsD() || (vn.IsScalar() && vd.IsScalar()));
  NEONShiftImmediateN(vd, vn, shift, NEON_SQSHRUN);
}


void Assembler::sqshrun2(const VRegister& vd,
                         const VRegister& vn,
                         int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsQ());
  NEONShiftImmediateN(vd, vn, shift, NEON_SQSHRUN);
}


void Assembler::sqrshrun(const VRegister& vd,
                         const VRegister& vn,
                         int shift) {
  VIXL_ASSERT(vd.IsD() || (vn.IsScalar() && vd.IsScalar()));
  NEONShiftImmediateN(vd, vn, shift, NEON_SQRSHRUN);
}


void Assembler::sqrshrun2(const VRegister& vd,
                          const VRegister& vn,
                          int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsQ());
  NEONShiftImmediateN(vd, vn, shift, NEON_SQRSHRUN);
}


void Assembler::uqshrn(const VRegister& vd,
                       const VRegister& vn,
                       int shift) {
  VIXL_ASSERT(vd.IsD() || (vn.IsScalar() && vd.IsScalar()));
  NEONShiftImmediateN(vd, vn, shift, NEON_UQSHRN);
}


void Assembler::uqshrn2(const VRegister& vd,
                        const VRegister& vn,
                        int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsQ());
  NEONShiftImmediateN(vd, vn, shift, NEON_UQSHRN);
}


void Assembler::uqrshrn(const VRegister& vd,
                        const VRegister& vn,
                        int shift) {
  VIXL_ASSERT(vd.IsD() || (vn.IsScalar() && vd.IsScalar()));
  NEONShiftImmediateN(vd, vn, shift, NEON_UQRSHRN);
}


void Assembler::uqrshrn2(const VRegister& vd,
                         const VRegister& vn,
                         int shift) {
  VIXL_ASSERT(vn.IsVector() && vd.IsQ());
  NEONShiftImmediateN(vd, vn, shift, NEON_UQRSHRN);
}


// Note:
// Below, a difference in case for the same letter indicates a
// negated bit.
// If b is 1, then B is 0.
uint32_t Assembler::FP32ToImm8(float imm) {
  VIXL_ASSERT(IsImmFP32(imm));
  // bits: aBbb.bbbc.defg.h000.0000.0000.0000.0000
  uint32_t bits = FloatToRawbits(imm);
  // bit7: a000.0000
  uint32_t bit7 = ((bits >> 31) & 0x1) << 7;
  // bit6: 0b00.0000
  uint32_t bit6 = ((bits >> 29) & 0x1) << 6;
  // bit5_to_0: 00cd.efgh
  uint32_t bit5_to_0 = (bits >> 19) & 0x3f;

  return bit7 | bit6 | bit5_to_0;
}


Instr Assembler::ImmFP32(float imm) {
  return FP32ToImm8(imm) << ImmFP_offset;
}


uint32_t Assembler::FP64ToImm8(double imm) {
  VIXL_ASSERT(IsImmFP64(imm));
  // bits: aBbb.bbbb.bbcd.efgh.0000.0000.0000.0000
  //       0000.0000.0000.0000.0000.0000.0000.0000
  uint64_t bits = DoubleToRawbits(imm);
  // bit7: a000.0000
  uint64_t bit7 = ((bits >> 63) & 0x1) << 7;
  // bit6: 0b00.0000
  uint64_t bit6 = ((bits >> 61) & 0x1) << 6;
  // bit5_to_0: 00cd.efgh
  uint64_t bit5_to_0 = (bits >> 48) & 0x3f;

  return static_cast<uint32_t>(bit7 | bit6 | bit5_to_0);
}


Instr Assembler::ImmFP64(double imm) {
  return FP64ToImm8(imm) << ImmFP_offset;
}


// Code generation helpers.
void Assembler::MoveWide(const Register& rd,
                         uint64_t imm,
                         int shift,
                         MoveWideImmediateOp mov_op) {
  // Ignore the top 32 bits of an immediate if we're moving to a W register.
  if (rd.Is32Bits()) {
    // Check that the top 32 bits are zero (a positive 32-bit number) or top
    // 33 bits are one (a negative 32-bit number, sign extended to 64 bits).
    VIXL_ASSERT(((imm >> kWRegSize) == 0) ||
                ((imm >> (kWRegSize - 1)) == 0x1ffffffff));
    imm &= kWRegMask;
  }

  if (shift >= 0) {
    // Explicit shift specified.
    VIXL_ASSERT((shift == 0) || (shift == 16) ||
                (shift == 32) || (shift == 48));
    VIXL_ASSERT(rd.Is64Bits() || (shift == 0) || (shift == 16));
    shift /= 16;
  } else {
    // Calculate a new immediate and shift combination to encode the immediate
    // argument.
    shift = 0;
    if ((imm & 0xffffffffffff0000) == 0) {
      // Nothing to do.
    } else if ((imm & 0xffffffff0000ffff) == 0) {
      imm >>= 16;
      shift = 1;
    } else if ((imm & 0xffff0000ffffffff) == 0) {
      VIXL_ASSERT(rd.Is64Bits());
      imm >>= 32;
      shift = 2;
    } else if ((imm & 0x0000ffffffffffff) == 0) {
      VIXL_ASSERT(rd.Is64Bits());
      imm >>= 48;
      shift = 3;
    }
  }

  VIXL_ASSERT(IsUint16(imm));

  Emit(SF(rd) | MoveWideImmediateFixed | mov_op |
       Rd(rd) | ImmMoveWide(imm) | ShiftMoveWide(shift));
}


void Assembler::AddSub(const Register& rd,
                       const Register& rn,
                       const Operand& operand,
                       FlagsUpdate S,
                       AddSubOp op) {
  VIXL_ASSERT(rd.size() == rn.size());
  if (operand.IsImmediate()) {
    int64_t immediate = operand.immediate();
    VIXL_ASSERT(IsImmAddSub(immediate));
    Instr dest_reg = (S == SetFlags) ? Rd(rd) : RdSP(rd);
    Emit(SF(rd) | AddSubImmediateFixed | op | Flags(S) |
         ImmAddSub(static_cast<int>(immediate)) | dest_reg | RnSP(rn));
  } else if (operand.IsShiftedRegister()) {
    VIXL_ASSERT(operand.reg().size() == rd.size());
    VIXL_ASSERT(operand.shift() != ROR);

    // For instructions of the form:
    //   add/sub   wsp, <Wn>, <Wm> [, LSL #0-3 ]
    //   add/sub   <Wd>, wsp, <Wm> [, LSL #0-3 ]
    //   add/sub   wsp, wsp, <Wm> [, LSL #0-3 ]
    //   adds/subs <Wd>, wsp, <Wm> [, LSL #0-3 ]
    // or their 64-bit register equivalents, convert the operand from shifted to
    // extended register mode, and emit an add/sub extended instruction.
    if (rn.IsSP() || rd.IsSP()) {
      VIXL_ASSERT(!(rd.IsSP() && (S == SetFlags)));
      DataProcExtendedRegister(rd, rn, operand.ToExtendedRegister(), S,
                               AddSubExtendedFixed | op);
    } else {
      DataProcShiftedRegister(rd, rn, operand, S, AddSubShiftedFixed | op);
    }
  } else {
    VIXL_ASSERT(operand.IsExtendedRegister());
    DataProcExtendedRegister(rd, rn, operand, S, AddSubExtendedFixed | op);
  }
}


void Assembler::AddSubWithCarry(const Register& rd,
                                const Register& rn,
                                const Operand& operand,
                                FlagsUpdate S,
                                AddSubWithCarryOp op) {
  VIXL_ASSERT(rd.size() == rn.size());
  VIXL_ASSERT(rd.size() == operand.reg().size());
  VIXL_ASSERT(operand.IsShiftedRegister() && (operand.shift_amount() == 0));
  Emit(SF(rd) | op | Flags(S) | Rm(operand.reg()) | Rn(rn) | Rd(rd));
}


void Assembler::hlt(int code) {
  VIXL_ASSERT(IsUint16(code));
  Emit(HLT | ImmException(code));
}


void Assembler::brk(int code) {
  VIXL_ASSERT(IsUint16(code));
  Emit(BRK | ImmException(code));
}


void Assembler::svc(int code) {
  Emit(SVC | ImmException(code));
}


void Assembler::ConditionalCompare(const Register& rn,
                                   const Operand& operand,
                                   StatusFlags nzcv,
                                   Condition cond,
                                   ConditionalCompareOp op) {
  Instr ccmpop;
  if (operand.IsImmediate()) {
    int64_t immediate = operand.immediate();
    VIXL_ASSERT(IsImmConditionalCompare(immediate));
    ccmpop = ConditionalCompareImmediateFixed | op |
        ImmCondCmp(static_cast<unsigned>(immediate));
  } else {
    VIXL_ASSERT(operand.IsShiftedRegister() && (operand.shift_amount() == 0));
    ccmpop = ConditionalCompareRegisterFixed | op | Rm(operand.reg());
  }
  Emit(SF(rn) | ccmpop | Cond(cond) | Rn(rn) | Nzcv(nzcv));
}


void Assembler::DataProcessing1Source(const Register& rd,
                                      const Register& rn,
                                      DataProcessing1SourceOp op) {
  VIXL_ASSERT(rd.size() == rn.size());
  Emit(SF(rn) | op | Rn(rn) | Rd(rd));
}


void Assembler::FPDataProcessing1Source(const VRegister& vd,
                                        const VRegister& vn,
                                        FPDataProcessing1SourceOp op) {
  VIXL_ASSERT(vd.Is1H() || vd.Is1S() || vd.Is1D());
  Emit(FPType(vn) | op | Rn(vn) | Rd(vd));
}


void Assembler::FPDataProcessing3Source(const VRegister& vd,
                                        const VRegister& vn,
                                        const VRegister& vm,
                                        const VRegister& va,
                                        FPDataProcessing3SourceOp op) {
  VIXL_ASSERT(vd.Is1S() || vd.Is1D());
  VIXL_ASSERT(AreSameSizeAndType(vd, vn, vm, va));
  Emit(FPType(vd) | op | Rm(vm) | Rn(vn) | Rd(vd) | Ra(va));
}


void Assembler::NEONModifiedImmShiftLsl(const VRegister& vd,
                                        const int imm8,
                                        const int left_shift,
                                        NEONModifiedImmediateOp op) {
  VIXL_ASSERT(vd.Is8B() || vd.Is16B() || vd.Is4H() || vd.Is8H() ||
              vd.Is2S() || vd.Is4S());
  VIXL_ASSERT((left_shift == 0) || (left_shift == 8) ||
              (left_shift == 16) || (left_shift == 24));
  VIXL_ASSERT(IsUint8(imm8));

  int cmode_1, cmode_2, cmode_3;
  if (vd.Is8B() || vd.Is16B()) {
    VIXL_ASSERT(op == NEONModifiedImmediate_MOVI);
    cmode_1 = 1;
    cmode_2 = 1;
    cmode_3 = 1;
  } else {
    cmode_1 = (left_shift >> 3) & 1;
    cmode_2 = left_shift >> 4;
    cmode_3 = 0;
    if (vd.Is4H() || vd.Is8H()) {
      VIXL_ASSERT((left_shift == 0) || (left_shift == 8));
      cmode_3 = 1;
    }
  }
  int cmode = (cmode_3 << 3) | (cmode_2 << 2) | (cmode_1 << 1);

  int q = vd.IsQ() ? NEON_Q : 0;

  Emit(q | op | ImmNEONabcdefgh(imm8) | NEONCmode(cmode) | Rd(vd));
}


void Assembler::NEONModifiedImmShiftMsl(const VRegister& vd,
                                        const int imm8,
                                        const int shift_amount,
                                        NEONModifiedImmediateOp op) {
  VIXL_ASSERT(vd.Is2S() || vd.Is4S());
  VIXL_ASSERT((shift_amount == 8) || (shift_amount == 16));
  VIXL_ASSERT(IsUint8(imm8));

  int cmode_0 = (shift_amount >> 4) & 1;
  int cmode = 0xc | cmode_0;

  int q = vd.IsQ() ? NEON_Q : 0;

  Emit(q | op | ImmNEONabcdefgh(imm8) | NEONCmode(cmode) | Rd(vd));
}


void Assembler::EmitShift(const Register& rd,
                          const Register& rn,
                          Shift shift,
                          unsigned shift_amount) {
  switch (shift) {
    case LSL:
      lsl(rd, rn, shift_amount);
      break;
    case LSR:
      lsr(rd, rn, shift_amount);
      break;
    case ASR:
      asr(rd, rn, shift_amount);
      break;
    case ROR:
      ror(rd, rn, shift_amount);
      break;
    default:
      VIXL_UNREACHABLE();
  }
}


void Assembler::EmitExtendShift(const Register& rd,
                                const Register& rn,
                                Extend extend,
                                unsigned left_shift) {
  VIXL_ASSERT(rd.size() >= rn.size());
  unsigned reg_size = rd.size();
  // Use the correct size of register.
  Register rn_ = Register(rn.code(), rd.size());
  // Bits extracted are high_bit:0.
  unsigned high_bit = (8 << (extend & 0x3)) - 1;
  // Number of bits left in the result that are not introduced by the shift.
  unsigned non_shift_bits = (reg_size - left_shift) & (reg_size - 1);

  if ((non_shift_bits > high_bit) || (non_shift_bits == 0)) {
    switch (extend) {
      case UXTB:
      case UXTH:
      case UXTW: ubfm(rd, rn_, non_shift_bits, high_bit); break;
      case SXTB:
      case SXTH:
      case SXTW: sbfm(rd, rn_, non_shift_bits, high_bit); break;
      case UXTX:
      case SXTX: {
        VIXL_ASSERT(rn.size() == kXRegSize);
        // Nothing to extend. Just shift.
        lsl(rd, rn_, left_shift);
        break;
      }
      default: VIXL_UNREACHABLE();
    }
  } else {
    // No need to extend as the extended bits would be shifted away.
    lsl(rd, rn_, left_shift);
  }
}


void Assembler::DataProcExtendedRegister(const Register& rd,
                                         const Register& rn,
                                         const Operand& operand,
                                         FlagsUpdate S,
                                         Instr op) {
  Instr dest_reg = (S == SetFlags) ? Rd(rd) : RdSP(rd);
  Emit(SF(rd) | op | Flags(S) | Rm(operand.reg()) |
       ExtendMode(operand.extend()) | ImmExtendShift(operand.shift_amount()) |
       dest_reg | RnSP(rn));
}


Instr Assembler::LoadStoreMemOperand(const MemOperand& addr,
                                     unsigned access_size,
                                     LoadStoreScalingOption option) {
  Instr base = RnSP(addr.base());
  int64_t offset = addr.offset();

  if (addr.IsImmediateOffset()) {
    bool prefer_unscaled = (option == PreferUnscaledOffset) ||
                           (option == RequireUnscaledOffset);
    if (prefer_unscaled && IsImmLSUnscaled(offset)) {
      // Use the unscaled addressing mode.
      return base | LoadStoreUnscaledOffsetFixed |
          ImmLS(static_cast<int>(offset));
    }

    if ((option != RequireUnscaledOffset) &&
        IsImmLSScaled(offset, access_size)) {
      // Use the scaled addressing mode.
      return base | LoadStoreUnsignedOffsetFixed |
          ImmLSUnsigned(static_cast<int>(offset) >> access_size);
    }

    if ((option != RequireScaledOffset) && IsImmLSUnscaled(offset)) {
      // Use the unscaled addressing mode.
      return base | LoadStoreUnscaledOffsetFixed |
          ImmLS(static_cast<int>(offset));
    }
  }

  // All remaining addressing modes are register-offset, pre-indexed or
  // post-indexed modes.
  VIXL_ASSERT((option != RequireUnscaledOffset) &&
              (option != RequireScaledOffset));

  if (addr.IsRegisterOffset()) {
    Extend ext = addr.extend();
    Shift shift = addr.shift();
    unsigned shift_amount = addr.shift_amount();

    // LSL is encoded in the option field as UXTX.
    if (shift == LSL) {
      ext = UXTX;
    }

    // Shifts are encoded in one bit, indicating a left shift by the memory
    // access size.
    VIXL_ASSERT((shift_amount == 0) || (shift_amount == access_size));
    return base | LoadStoreRegisterOffsetFixed | Rm(addr.regoffset()) |
        ExtendMode(ext) | ImmShiftLS((shift_amount > 0) ? 1 : 0);
  }

  if (addr.IsPreIndex() && IsImmLSUnscaled(offset)) {
    return base | LoadStorePreIndexFixed | ImmLS(static_cast<int>(offset));
  }

  if (addr.IsPostIndex() && IsImmLSUnscaled(offset)) {
    return base | LoadStorePostIndexFixed | ImmLS(static_cast<int>(offset));
  }

  // If this point is reached, the MemOperand (addr) cannot be encoded.
  VIXL_UNREACHABLE();
  return 0;
}


void Assembler::LoadStore(const CPURegister& rt,
                          const MemOperand& addr,
                          LoadStoreOp op,
                          LoadStoreScalingOption option) {
  Emit(op | Rt(rt) | LoadStoreMemOperand(addr, CalcLSDataSize(op), option));
}


void Assembler::Prefetch(PrefetchOperation op,
                         const MemOperand& addr,
                         LoadStoreScalingOption option) {
  VIXL_ASSERT(addr.IsRegisterOffset() || addr.IsImmediateOffset());

  Instr prfop = ImmPrefetchOperation(op);
  Emit(PRFM | prfop | LoadStoreMemOperand(addr, kXRegSizeInBytesLog2, option));
}


bool Assembler::IsImmAddSub(int64_t immediate) {
  return IsUint12(immediate) ||
         (IsUint12(immediate >> 12) && ((immediate & 0xfff) == 0));
}


bool Assembler::IsImmConditionalCompare(int64_t immediate) {
  return IsUint5(immediate);
}


bool Assembler::IsImmFP32(float imm) {
  // Valid values will have the form:
  // aBbb.bbbc.defg.h000.0000.0000.0000.0000
  uint32_t bits = FloatToRawbits(imm);
  // bits[19..0] are cleared.
  if ((bits & 0x7ffff) != 0) {
    return false;
  }

  // bits[29..25] are all set or all cleared.
  uint32_t b_pattern = (bits >> 16) & 0x3e00;
  if (b_pattern != 0 && b_pattern != 0x3e00) {
    return false;
  }

  // bit[30] and bit[29] are opposite.
  if (((bits ^ (bits << 1)) & 0x40000000) == 0) {
    return false;
  }

  return true;
}


bool Assembler::IsImmFP64(double imm) {
  // Valid values will have the form:
  // aBbb.bbbb.bbcd.efgh.0000.0000.0000.0000
  // 0000.0000.0000.0000.0000.0000.0000.0000
  uint64_t bits = DoubleToRawbits(imm);
  // bits[47..0] are cleared.
  if ((bits & 0x0000ffffffffffff) != 0) {
    return false;
  }

  // bits[61..54] are all set or all cleared.
  uint32_t b_pattern = (bits >> 48) & 0x3fc0;
  if ((b_pattern != 0) && (b_pattern != 0x3fc0)) {
    return false;
  }

  // bit[62] and bit[61] are opposite.
  if (((bits ^ (bits << 1)) & (UINT64_C(1) << 62)) == 0) {
    return false;
  }

  return true;
}


bool Assembler::IsImmLSPair(int64_t offset, unsigned access_size) {
  VIXL_ASSERT(access_size <= kQRegSizeInBytesLog2);
  bool offset_is_size_multiple =
      (((offset >> access_size) << access_size) == offset);
  return offset_is_size_multiple && IsInt7(offset >> access_size);
}


bool Assembler::IsImmLSScaled(int64_t offset, unsigned access_size) {
  VIXL_ASSERT(access_size <= kQRegSizeInBytesLog2);
  bool offset_is_size_multiple =
      (((offset >> access_size) << access_size) == offset);
  return offset_is_size_multiple && IsUint12(offset >> access_size);
}


bool Assembler::IsImmLSUnscaled(int64_t offset) {
  return IsInt9(offset);
}


// The movn instruction can generate immediates containing an arbitrary 16-bit
// value, with remaining bits set, eg. 0xffff1234, 0xffff1234ffffffff.
bool Assembler::IsImmMovn(uint64_t imm, unsigned reg_size) {
  return IsImmMovz(~imm, reg_size);
}


// The movz instruction can generate immediates containing an arbitrary 16-bit
// value, with remaining bits clear, eg. 0x00001234, 0x0000123400000000.
bool Assembler::IsImmMovz(uint64_t imm, unsigned reg_size) {
  VIXL_ASSERT((reg_size == kXRegSize) || (reg_size == kWRegSize));
  return CountClearHalfWords(imm, reg_size) >= ((reg_size / 16) - 1);
}


// Test if a given value can be encoded in the immediate field of a logical
// instruction.
// If it can be encoded, the function returns true, and values pointed to by n,
// imm_s and imm_r are updated with immediates encoded in the format required
// by the corresponding fields in the logical instruction.
// If it can not be encoded, the function returns false, and the values pointed
// to by n, imm_s and imm_r are undefined.
bool Assembler::IsImmLogical(uint64_t value,
                             unsigned width,
                             unsigned* n,
                             unsigned* imm_s,
                             unsigned* imm_r) {
  VIXL_ASSERT((width == kWRegSize) || (width == kXRegSize));

  bool negate = false;

  // Logical immediates are encoded using parameters n, imm_s and imm_r using
  // the following table:
  //
  //    N   imms    immr    size        S             R
  //    1  ssssss  rrrrrr    64    UInt(ssssss)  UInt(rrrrrr)
  //    0  0sssss  xrrrrr    32    UInt(sssss)   UInt(rrrrr)
  //    0  10ssss  xxrrrr    16    UInt(ssss)    UInt(rrrr)
  //    0  110sss  xxxrrr     8    UInt(sss)     UInt(rrr)
  //    0  1110ss  xxxxrr     4    UInt(ss)      UInt(rr)
  //    0  11110s  xxxxxr     2    UInt(s)       UInt(r)
  // (s bits must not be all set)
  //
  // A pattern is constructed of size bits, where the least significant S+1 bits
  // are set. The pattern is rotated right by R, and repeated across a 32 or
  // 64-bit value, depending on destination register width.
  //
  // Put another way: the basic format of a logical immediate is a single
  // contiguous stretch of 1 bits, repeated across the whole word at intervals
  // given by a power of 2. To identify them quickly, we first locate the
  // lowest stretch of 1 bits, then the next 1 bit above that; that combination
  // is different for every logical immediate, so it gives us all the
  // information we need to identify the only logical immediate that our input
  // could be, and then we simply check if that's the value we actually have.
  //
  // (The rotation parameter does give the possibility of the stretch of 1 bits
  // going 'round the end' of the word. To deal with that, we observe that in
  // any situation where that happens the bitwise NOT of the value is also a
  // valid logical immediate. So we simply invert the input whenever its low bit
  // is set, and then we know that the rotated case can't arise.)

  if (value & 1) {
    // If the low bit is 1, negate the value, and set a flag to remember that we
    // did (so that we can adjust the return values appropriately).
    negate = true;
    value = ~value;
  }

  if (width == kWRegSize) {
    // To handle 32-bit logical immediates, the very easiest thing is to repeat
    // the input value twice to make a 64-bit word. The correct encoding of that
    // as a logical immediate will also be the correct encoding of the 32-bit
    // value.

    // Avoid making the assumption that the most-significant 32 bits are zero by
    // shifting the value left and duplicating it.
    value <<= kWRegSize;
    value |= value >> kWRegSize;
  }

  // The basic analysis idea: imagine our input word looks like this.
  //
  //    0011111000111110001111100011111000111110001111100011111000111110
  //                                                          c  b    a
  //                                                          |<--d-->|
  //
  // We find the lowest set bit (as an actual power-of-2 value, not its index)
  // and call it a. Then we add a to our original number, which wipes out the
  // bottommost stretch of set bits and replaces it with a 1 carried into the
  // next zero bit. Then we look for the new lowest set bit, which is in
  // position b, and subtract it, so now our number is just like the original
  // but with the lowest stretch of set bits completely gone. Now we find the
  // lowest set bit again, which is position c in the diagram above. Then we'll
  // measure the distance d between bit positions a and c (using CLZ), and that
  // tells us that the only valid logical immediate that could possibly be equal
  // to this number is the one in which a stretch of bits running from a to just
  // below b is replicated every d bits.
  uint64_t a = LowestSetBit(value);
  uint64_t value_plus_a = value + a;
  uint64_t b = LowestSetBit(value_plus_a);
  uint64_t value_plus_a_minus_b = value_plus_a - b;
  uint64_t c = LowestSetBit(value_plus_a_minus_b);

  int d, clz_a, out_n;
  uint64_t mask;

  if (c != 0) {
    // The general case, in which there is more than one stretch of set bits.
    // Compute the repeat distance d, and set up a bitmask covering the basic
    // unit of repetition (i.e. a word with the bottom d bits set). Also, in all
    // of these cases the N bit of the output will be zero.
    clz_a = CountLeadingZeros(a, kXRegSize);
    int clz_c = CountLeadingZeros(c, kXRegSize);
    d = clz_a - clz_c;
    mask = ((UINT64_C(1) << d) - 1);
    out_n = 0;
  } else {
    // Handle degenerate cases.
    //
    // If any of those 'find lowest set bit' operations didn't find a set bit at
    // all, then the word will have been zero thereafter, so in particular the
    // last lowest_set_bit operation will have returned zero. So we can test for
    // all the special case conditions in one go by seeing if c is zero.
    if (a == 0) {
      // The input was zero (or all 1 bits, which will come to here too after we
      // inverted it at the start of the function), for which we just return
      // false.
      return false;
    } else {
      // Otherwise, if c was zero but a was not, then there's just one stretch
      // of set bits in our word, meaning that we have the trivial case of
      // d == 64 and only one 'repetition'. Set up all the same variables as in
      // the general case above, and set the N bit in the output.
      clz_a = CountLeadingZeros(a, kXRegSize);
      d = 64;
      mask = ~UINT64_C(0);
      out_n = 1;
    }
  }

  // If the repeat period d is not a power of two, it can't be encoded.
  if (!IsPowerOf2(d)) {
    return false;
  }

  if (((b - a) & ~mask) != 0) {
    // If the bit stretch (b - a) does not fit within the mask derived from the
    // repeat period, then fail.
    return false;
  }

  // The only possible option is b - a repeated every d bits. Now we're going to
  // actually construct the valid logical immediate derived from that
  // specification, and see if it equals our original input.
  //
  // To repeat a value every d bits, we multiply it by a number of the form
  // (1 + 2^d + 2^(2d) + ...), i.e. 0x0001000100010001 or similar. These can
  // be derived using a table lookup on CLZ(d).
  static const uint64_t multipliers[] = {
    0x0000000000000001UL,
    0x0000000100000001UL,
    0x0001000100010001UL,
    0x0101010101010101UL,
    0x1111111111111111UL,
    0x5555555555555555UL,
  };
  uint64_t multiplier = multipliers[CountLeadingZeros(d, kXRegSize) - 57];
  uint64_t candidate = (b - a) * multiplier;

  if (value != candidate) {
    // The candidate pattern doesn't match our input value, so fail.
    return false;
  }

  // We have a match! This is a valid logical immediate, so now we have to
  // construct the bits and pieces of the instruction encoding that generates
  // it.

  // Count the set bits in our basic stretch. The special case of clz(0) == -1
  // makes the answer come out right for stretches that reach the very top of
  // the word (e.g. numbers like 0xffffc00000000000).
  int clz_b = (b == 0) ? -1 : CountLeadingZeros(b, kXRegSize);
  int s = clz_a - clz_b;

  // Decide how many bits to rotate right by, to put the low bit of that basic
  // stretch in position a.
  int r;
  if (negate) {
    // If we inverted the input right at the start of this function, here's
    // where we compensate: the number of set bits becomes the number of clear
    // bits, and the rotation count is based on position b rather than position
    // a (since b is the location of the 'lowest' 1 bit after inversion).
    s = d - s;
    r = (clz_b + 1) & (d - 1);
  } else {
    r = (clz_a + 1) & (d - 1);
  }

  // Now we're done, except for having to encode the S output in such a way that
  // it gives both the number of set bits and the length of the repeated
  // segment. The s field is encoded like this:
  //
  //     imms    size        S
  //    ssssss    64    UInt(ssssss)
  //    0sssss    32    UInt(sssss)
  //    10ssss    16    UInt(ssss)
  //    110sss     8    UInt(sss)
  //    1110ss     4    UInt(ss)
  //    11110s     2    UInt(s)
  //
  // So we 'or' (-d << 1) with our computed s to form imms.
  if ((n != NULL) || (imm_s != NULL) || (imm_r != NULL)) {
    *n = out_n;
    *imm_s = ((-d << 1) | (s - 1)) & 0x3f;
    *imm_r = r;
  }

  return true;
}


LoadStoreOp Assembler::LoadOpFor(const CPURegister& rt) {
  VIXL_ASSERT(rt.IsValid());
  if (rt.IsRegister()) {
    return rt.Is64Bits() ? LDR_x : LDR_w;
  } else {
    VIXL_ASSERT(rt.IsVRegister());
    switch (rt.SizeInBits()) {
      case kBRegSize: return LDR_b;
      case kHRegSize: return LDR_h;
      case kSRegSize: return LDR_s;
      case kDRegSize: return LDR_d;
      default:
        VIXL_ASSERT(rt.IsQ());
        return LDR_q;
    }
  }
}


LoadStoreOp Assembler::StoreOpFor(const CPURegister& rt) {
  VIXL_ASSERT(rt.IsValid());
  if (rt.IsRegister()) {
    return rt.Is64Bits() ? STR_x : STR_w;
  } else {
    VIXL_ASSERT(rt.IsVRegister());
    switch (rt.SizeInBits()) {
      case kBRegSize: return STR_b;
      case kHRegSize: return STR_h;
      case kSRegSize: return STR_s;
      case kDRegSize: return STR_d;
      default:
        VIXL_ASSERT(rt.IsQ());
        return STR_q;
    }
  }
}


LoadStorePairOp Assembler::StorePairOpFor(const CPURegister& rt,
    const CPURegister& rt2) {
  VIXL_ASSERT(AreSameSizeAndType(rt, rt2));
  USE(rt2);
  if (rt.IsRegister()) {
    return rt.Is64Bits() ? STP_x : STP_w;
  } else {
    VIXL_ASSERT(rt.IsVRegister());
    switch (rt.SizeInBytes()) {
      case kSRegSizeInBytes: return STP_s;
      case kDRegSizeInBytes: return STP_d;
      default:
        VIXL_ASSERT(rt.IsQ());
        return STP_q;
    }
  }
}


LoadStorePairOp Assembler::LoadPairOpFor(const CPURegister& rt,
                                         const CPURegister& rt2) {
  VIXL_ASSERT((STP_w | LoadStorePairLBit) == LDP_w);
  return static_cast<LoadStorePairOp>(StorePairOpFor(rt, rt2) |
                                      LoadStorePairLBit);
}


LoadStorePairNonTemporalOp Assembler::StorePairNonTemporalOpFor(
    const CPURegister& rt, const CPURegister& rt2) {
  VIXL_ASSERT(AreSameSizeAndType(rt, rt2));
  USE(rt2);
  if (rt.IsRegister()) {
    return rt.Is64Bits() ? STNP_x : STNP_w;
  } else {
    VIXL_ASSERT(rt.IsVRegister());
    switch (rt.SizeInBytes()) {
      case kSRegSizeInBytes: return STNP_s;
      case kDRegSizeInBytes: return STNP_d;
      default:
        VIXL_ASSERT(rt.IsQ());
        return STNP_q;
    }
  }
}


LoadStorePairNonTemporalOp Assembler::LoadPairNonTemporalOpFor(
    const CPURegister& rt, const CPURegister& rt2) {
  VIXL_ASSERT((STNP_w | LoadStorePairNonTemporalLBit) == LDNP_w);
  return static_cast<LoadStorePairNonTemporalOp>(
      StorePairNonTemporalOpFor(rt, rt2) | LoadStorePairNonTemporalLBit);
}


LoadLiteralOp Assembler::LoadLiteralOpFor(const CPURegister& rt) {
  if (rt.IsRegister()) {
    return rt.IsX() ? LDR_x_lit : LDR_w_lit;
  } else {
    VIXL_ASSERT(rt.IsVRegister());
    switch (rt.SizeInBytes()) {
      case kSRegSizeInBytes: return LDR_s_lit;
      case kDRegSizeInBytes: return LDR_d_lit;
      default:
        VIXL_ASSERT(rt.IsQ());
        return LDR_q_lit;
    }
  }
}


bool Assembler::CPUHas(const CPURegister& rt) const {
  // Core registers are available without any particular CPU features.
  if (rt.IsRegister()) return true;
  VIXL_ASSERT(rt.IsVRegister());
  // The architecture does not allow FP and NEON to be implemented separately,
  // but we can crudely categorise them based on register size, since FP only
  // uses D, S and (occasionally) H registers.
  if (rt.IsH() || rt.IsS() || rt.IsD()) {
    return CPUHas(CPUFeatures::kFP) || CPUHas(CPUFeatures::kNEON);
  }
  VIXL_ASSERT(rt.IsB() || rt.IsQ());
  return CPUHas(CPUFeatures::kNEON);
}


bool Assembler::CPUHas(const CPURegister& rt, const CPURegister& rt2) const {
  // This is currently only used for loads and stores, where rt and rt2 must
  // have the same size and type. We could extend this to cover other cases if
  // necessary, but for now we can avoid checking both registers.
  VIXL_ASSERT(AreSameSizeAndType(rt, rt2));
  USE(rt2);
  return CPUHas(rt);
}


bool Assembler::CPUHas(SystemRegister sysreg) const {
  switch (sysreg) {
    case RNDR:
    case RNDRRS:
      return CPUHas(CPUFeatures::kRNG);
    case FPCR:
    case NZCV:
      break;
  }
  return true;
}


bool AreAliased(const CPURegister& reg1, const CPURegister& reg2,
                const CPURegister& reg3, const CPURegister& reg4,
                const CPURegister& reg5, const CPURegister& reg6,
                const CPURegister& reg7, const CPURegister& reg8) {
  int number_of_valid_regs = 0;
  int number_of_valid_fpregs = 0;

  RegList unique_regs = 0;
  RegList unique_fpregs = 0;

  const CPURegister regs[] = {reg1, reg2, reg3, reg4, reg5, reg6, reg7, reg8};

  for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
    if (regs[i].IsRegister()) {
      number_of_valid_regs++;
      unique_regs |= regs[i].Bit();
    } else if (regs[i].IsVRegister()) {
      number_of_valid_fpregs++;
      unique_fpregs |= regs[i].Bit();
    } else {
      VIXL_ASSERT(!regs[i].IsValid());
    }
  }

  int number_of_unique_regs = CountSetBits(unique_regs);
  int number_of_unique_fpregs = CountSetBits(unique_fpregs);

  VIXL_ASSERT(number_of_valid_regs >= number_of_unique_regs);
  VIXL_ASSERT(number_of_valid_fpregs >= number_of_unique_fpregs);

  return (number_of_valid_regs != number_of_unique_regs) ||
         (number_of_valid_fpregs != number_of_unique_fpregs);
}


bool AreSameSizeAndType(const CPURegister& reg1, const CPURegister& reg2,
                        const CPURegister& reg3, const CPURegister& reg4,
                        const CPURegister& reg5, const CPURegister& reg6,
                        const CPURegister& reg7, const CPURegister& reg8) {
  VIXL_ASSERT(reg1.IsValid());
  bool match = true;
  match &= !reg2.IsValid() || reg2.IsSameSizeAndType(reg1);
  match &= !reg3.IsValid() || reg3.IsSameSizeAndType(reg1);
  match &= !reg4.IsValid() || reg4.IsSameSizeAndType(reg1);
  match &= !reg5.IsValid() || reg5.IsSameSizeAndType(reg1);
  match &= !reg6.IsValid() || reg6.IsSameSizeAndType(reg1);
  match &= !reg7.IsValid() || reg7.IsSameSizeAndType(reg1);
  match &= !reg8.IsValid() || reg8.IsSameSizeAndType(reg1);
  return match;
}

bool AreEven(const CPURegister& reg1,
             const CPURegister& reg2,
             const CPURegister& reg3,
             const CPURegister& reg4,
             const CPURegister& reg5,
             const CPURegister& reg6,
             const CPURegister& reg7,
             const CPURegister& reg8) {
  VIXL_ASSERT(reg1.IsValid());
  bool even = (reg1.code() % 2) == 0;
  even &= !reg2.IsValid() || ((reg2.code() % 2) == 0);
  even &= !reg3.IsValid() || ((reg3.code() % 2) == 0);
  even &= !reg4.IsValid() || ((reg4.code() % 2) == 0);
  even &= !reg5.IsValid() || ((reg5.code() % 2) == 0);
  even &= !reg6.IsValid() || ((reg6.code() % 2) == 0);
  even &= !reg7.IsValid() || ((reg7.code() % 2) == 0);
  even &= !reg8.IsValid() || ((reg8.code() % 2) == 0);
  return even;
}

bool AreConsecutive(const CPURegister& reg1,
                    const CPURegister& reg2,
                    const CPURegister& reg3,
                    const CPURegister& reg4) {
  VIXL_ASSERT(reg1.IsValid());

  if (!reg2.IsValid()) {
    return true;
  } else if (reg2.code() != ((reg1.code() + 1) % kNumberOfRegisters)) {
    return false;
  }

  if (!reg3.IsValid()) {
    return true;
  } else if (reg3.code() != ((reg2.code() + 1) % kNumberOfRegisters)) {
    return false;
  }

  if (!reg4.IsValid()) {
    return true;
  } else if (reg4.code() != ((reg3.code() + 1) % kNumberOfRegisters)) {
    return false;
  }

  return true;
}

bool AreSameFormat(const VRegister& reg1, const VRegister& reg2,
                   const VRegister& reg3, const VRegister& reg4) {
  VIXL_ASSERT(reg1.IsValid());
  bool match = true;
  match &= !reg2.IsValid() || reg2.IsSameFormat(reg1);
  match &= !reg3.IsValid() || reg3.IsSameFormat(reg1);
  match &= !reg4.IsValid() || reg4.IsSameFormat(reg1);
  return match;
}


bool AreConsecutive(const VRegister& reg1, const VRegister& reg2,
                    const VRegister& reg3, const VRegister& reg4) {
  VIXL_ASSERT(reg1.IsValid());
  bool match = true;
  match &= !reg2.IsValid() ||
           (reg2.code() == ((reg1.code() + 1) % kNumberOfVRegisters));
  match &= !reg3.IsValid() ||
           (reg3.code() == ((reg1.code() + 2) % kNumberOfVRegisters));
  match &= !reg4.IsValid() ||
           (reg4.code() == ((reg1.code() + 3) % kNumberOfVRegisters));
  return match;
}
}  // namespace vixl
