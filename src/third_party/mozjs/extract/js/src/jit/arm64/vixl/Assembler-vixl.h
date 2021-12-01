// Copyright 2015, ARM Limited
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

#ifndef VIXL_A64_ASSEMBLER_A64_H_
#define VIXL_A64_ASSEMBLER_A64_H_

#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Instructions-vixl.h"
#include "jit/arm64/vixl/MozBaseAssembler-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"

#include "jit/JitSpewer.h"

#include "jit/shared/Assembler-shared.h"
#include "jit/shared/Disassembler-shared.h"
#include "jit/shared/IonAssemblerBufferWithConstantPools.h"

namespace vixl {

using js::jit::BufferOffset;
using js::jit::Label;
using js::jit::Address;
using js::jit::BaseIndex;
using js::jit::DisassemblerSpew;

using LabelDoc = DisassemblerSpew::LabelDoc;

typedef uint64_t RegList;
static const int kRegListSizeInBits = sizeof(RegList) * 8;


// Registers.

// Some CPURegister methods can return Register or VRegister types, so we need
// to declare them in advance.
class Register;
class VRegister;

class CPURegister {
 public:
  enum RegisterType {
    // The kInvalid value is used to detect uninitialized static instances,
    // which are always zero-initialized before any constructors are called.
    kInvalid = 0,
    kRegister,
    kVRegister,
    kFPRegister = kVRegister,
    kNoRegister
  };

  constexpr CPURegister() : code_(0), size_(0), type_(kNoRegister) {
  }

  constexpr CPURegister(unsigned code, unsigned size, RegisterType type)
      : code_(code), size_(size), type_(type) {
  }

  unsigned code() const {
    VIXL_ASSERT(IsValid());
    return code_;
  }

  RegisterType type() const {
    VIXL_ASSERT(IsValidOrNone());
    return type_;
  }

  RegList Bit() const {
    VIXL_ASSERT(code_ < (sizeof(RegList) * 8));
    return IsValid() ? (static_cast<RegList>(1) << code_) : 0;
  }

  unsigned size() const {
    VIXL_ASSERT(IsValid());
    return size_;
  }

  int SizeInBytes() const {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(size() % 8 == 0);
    return size_ / 8;
  }

  int SizeInBits() const {
    VIXL_ASSERT(IsValid());
    return size_;
  }

  bool Is8Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 8;
  }

  bool Is16Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 16;
  }

  bool Is32Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 32;
  }

  bool Is64Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 64;
  }

  bool Is128Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 128;
  }

  bool IsValid() const {
    if (IsValidRegister() || IsValidVRegister()) {
      VIXL_ASSERT(!IsNone());
      return true;
    } else {
      // This assert is hit when the register has not been properly initialized.
      // One cause for this can be an initialisation order fiasco. See
      // https://isocpp.org/wiki/faq/ctors#static-init-order for some details.
      VIXL_ASSERT(IsNone());
      return false;
    }
  }

  bool IsValidRegister() const {
    return IsRegister() &&
           ((size_ == kWRegSize) || (size_ == kXRegSize)) &&
           ((code_ < kNumberOfRegisters) || (code_ == kSPRegInternalCode));
  }

  bool IsValidVRegister() const {
    return IsVRegister() &&
           ((size_ == kBRegSize) || (size_ == kHRegSize) ||
            (size_ == kSRegSize) || (size_ == kDRegSize) ||
            (size_ == kQRegSize)) &&
           (code_ < kNumberOfVRegisters);
  }

  bool IsValidFPRegister() const {
    return IsFPRegister() && (code_ < kNumberOfVRegisters);
  }

  bool IsNone() const {
    // kNoRegister types should always have size 0 and code 0.
    VIXL_ASSERT((type_ != kNoRegister) || (code_ == 0));
    VIXL_ASSERT((type_ != kNoRegister) || (size_ == 0));

    return type_ == kNoRegister;
  }

  bool Aliases(const CPURegister& other) const {
    VIXL_ASSERT(IsValidOrNone() && other.IsValidOrNone());
    return (code_ == other.code_) && (type_ == other.type_);
  }

  bool Is(const CPURegister& other) const {
    VIXL_ASSERT(IsValidOrNone() && other.IsValidOrNone());
    return Aliases(other) && (size_ == other.size_);
  }

  bool IsZero() const {
    VIXL_ASSERT(IsValid());
    return IsRegister() && (code_ == kZeroRegCode);
  }

  bool IsSP() const {
    VIXL_ASSERT(IsValid());
    return IsRegister() && (code_ == kSPRegInternalCode);
  }

  bool IsRegister() const {
    return type_ == kRegister;
  }

  bool IsVRegister() const {
    return type_ == kVRegister;
  }

  bool IsFPRegister() const {
    return IsS() || IsD();
  }

  bool IsW() const { return IsValidRegister() && Is32Bits(); }
  bool IsX() const { return IsValidRegister() && Is64Bits(); }

  // These assertions ensure that the size and type of the register are as
  // described. They do not consider the number of lanes that make up a vector.
  // So, for example, Is8B() implies IsD(), and Is1D() implies IsD, but IsD()
  // does not imply Is1D() or Is8B().
  // Check the number of lanes, ie. the format of the vector, using methods such
  // as Is8B(), Is1D(), etc. in the VRegister class.
  bool IsV() const { return IsVRegister(); }
  bool IsB() const { return IsV() && Is8Bits(); }
  bool IsH() const { return IsV() && Is16Bits(); }
  bool IsS() const { return IsV() && Is32Bits(); }
  bool IsD() const { return IsV() && Is64Bits(); }
  bool IsQ() const { return IsV() && Is128Bits(); }

  const Register& W() const;
  const Register& X() const;
  const VRegister& V() const;
  const VRegister& B() const;
  const VRegister& H() const;
  const VRegister& S() const;
  const VRegister& D() const;
  const VRegister& Q() const;

  bool IsSameSizeAndType(const CPURegister& other) const {
    return (size_ == other.size_) && (type_ == other.type_);
  }

 protected:
  unsigned code_;
  unsigned size_;
  RegisterType type_;

 private:
  bool IsValidOrNone() const {
    return IsValid() || IsNone();
  }
};


class Register : public CPURegister {
 public:
  Register() : CPURegister() {}
  explicit Register(const CPURegister& other)
      : CPURegister(other.code(), other.size(), other.type()) {
    VIXL_ASSERT(IsValidRegister());
  }
  constexpr Register(unsigned code, unsigned size)
      : CPURegister(code, size, kRegister) {}

  constexpr Register(js::jit::Register r, unsigned size)
    : CPURegister(r.code(), size, kRegister) {}

  bool IsValid() const {
    VIXL_ASSERT(IsRegister() || IsNone());
    return IsValidRegister();
  }

  js::jit::Register asUnsized() const {
    // asUnsized() is only ever used on temp registers or on registers that
    // are known not to be SP, and there should be no risk of it being
    // applied to SP.  Check anyway.
    VIXL_ASSERT(code_ != kSPRegInternalCode);
    return js::jit::Register::FromCode((js::jit::Register::Code)code_);
  }


  static const Register& WRegFromCode(unsigned code);
  static const Register& XRegFromCode(unsigned code);

 private:
  static const Register wregisters[];
  static const Register xregisters[];
};


class VRegister : public CPURegister {
 public:
  VRegister() : CPURegister(), lanes_(1) {}
  explicit VRegister(const CPURegister& other)
      : CPURegister(other.code(), other.size(), other.type()), lanes_(1) {
    VIXL_ASSERT(IsValidVRegister());
    VIXL_ASSERT(IsPowerOf2(lanes_) && (lanes_ <= 16));
  }
  constexpr VRegister(unsigned code, unsigned size, unsigned lanes = 1)
      : CPURegister(code, size, kVRegister), lanes_(lanes) {
    // VIXL_ASSERT(IsPowerOf2(lanes_) && (lanes_ <= 16));
  }
  constexpr VRegister(js::jit::FloatRegister r)
      : CPURegister(r.code_, r.size() * 8, kVRegister), lanes_(1) {
  }
  constexpr VRegister(js::jit::FloatRegister r, unsigned size)
      : CPURegister(r.code_, size, kVRegister), lanes_(1) {
  }
  VRegister(unsigned code, VectorFormat format)
      : CPURegister(code, RegisterSizeInBitsFromFormat(format), kVRegister),
        lanes_(IsVectorFormat(format) ? LaneCountFromFormat(format) : 1) {
    VIXL_ASSERT(IsPowerOf2(lanes_) && (lanes_ <= 16));
  }

  bool IsValid() const {
    VIXL_ASSERT(IsVRegister() || IsNone());
    return IsValidVRegister();
  }

  static const VRegister& BRegFromCode(unsigned code);
  static const VRegister& HRegFromCode(unsigned code);
  static const VRegister& SRegFromCode(unsigned code);
  static const VRegister& DRegFromCode(unsigned code);
  static const VRegister& QRegFromCode(unsigned code);
  static const VRegister& VRegFromCode(unsigned code);

  VRegister V8B() const { return VRegister(code_, kDRegSize, 8); }
  VRegister V16B() const { return VRegister(code_, kQRegSize, 16); }
  VRegister V4H() const { return VRegister(code_, kDRegSize, 4); }
  VRegister V8H() const { return VRegister(code_, kQRegSize, 8); }
  VRegister V2S() const { return VRegister(code_, kDRegSize, 2); }
  VRegister V4S() const { return VRegister(code_, kQRegSize, 4); }
  VRegister V2D() const { return VRegister(code_, kQRegSize, 2); }
  VRegister V1D() const { return VRegister(code_, kDRegSize, 1); }

  bool Is8B() const { return (Is64Bits() && (lanes_ == 8)); }
  bool Is16B() const { return (Is128Bits() && (lanes_ == 16)); }
  bool Is4H() const { return (Is64Bits() && (lanes_ == 4)); }
  bool Is8H() const { return (Is128Bits() && (lanes_ == 8)); }
  bool Is2S() const { return (Is64Bits() && (lanes_ == 2)); }
  bool Is4S() const { return (Is128Bits() && (lanes_ == 4)); }
  bool Is1D() const { return (Is64Bits() && (lanes_ == 1)); }
  bool Is2D() const { return (Is128Bits() && (lanes_ == 2)); }

  // For consistency, we assert the number of lanes of these scalar registers,
  // even though there are no vectors of equivalent total size with which they
  // could alias.
  bool Is1B() const {
    VIXL_ASSERT(!(Is8Bits() && IsVector()));
    return Is8Bits();
  }
  bool Is1H() const {
    VIXL_ASSERT(!(Is16Bits() && IsVector()));
    return Is16Bits();
  }
  bool Is1S() const {
    VIXL_ASSERT(!(Is32Bits() && IsVector()));
    return Is32Bits();
  }

  bool IsLaneSizeB() const { return LaneSizeInBits() == kBRegSize; }
  bool IsLaneSizeH() const { return LaneSizeInBits() == kHRegSize; }
  bool IsLaneSizeS() const { return LaneSizeInBits() == kSRegSize; }
  bool IsLaneSizeD() const { return LaneSizeInBits() == kDRegSize; }

  int lanes() const {
    return lanes_;
  }

  bool IsScalar() const {
    return lanes_ == 1;
  }

  bool IsVector() const {
    return lanes_ > 1;
  }

  bool IsSameFormat(const VRegister& other) const {
    return (size_ == other.size_) && (lanes_ == other.lanes_);
  }

  unsigned LaneSizeInBytes() const {
    return SizeInBytes() / lanes_;
  }

  unsigned LaneSizeInBits() const {
    return LaneSizeInBytes() * 8;
  }

 private:
  static const VRegister bregisters[];
  static const VRegister hregisters[];
  static const VRegister sregisters[];
  static const VRegister dregisters[];
  static const VRegister qregisters[];
  static const VRegister vregisters[];
  int lanes_;
};


// Backward compatibility for FPRegisters.
typedef VRegister FPRegister;

// No*Reg is used to indicate an unused argument, or an error case. Note that
// these all compare equal (using the Is() method). The Register and VRegister
// variants are provided for convenience.
const Register NoReg;
const VRegister NoVReg;
const FPRegister NoFPReg;  // For backward compatibility.
const CPURegister NoCPUReg;


#define DEFINE_REGISTERS(N)  \
constexpr Register w##N(N, kWRegSize);  \
constexpr Register x##N(N, kXRegSize);
REGISTER_CODE_LIST(DEFINE_REGISTERS)
#undef DEFINE_REGISTERS
constexpr Register wsp(kSPRegInternalCode, kWRegSize);
constexpr Register sp(kSPRegInternalCode, kXRegSize);


#define DEFINE_VREGISTERS(N)  \
constexpr VRegister b##N(N, kBRegSize);  \
constexpr VRegister h##N(N, kHRegSize);  \
constexpr VRegister s##N(N, kSRegSize);  \
constexpr VRegister d##N(N, kDRegSize);  \
constexpr VRegister q##N(N, kQRegSize);  \
constexpr VRegister v##N(N, kQRegSize);
REGISTER_CODE_LIST(DEFINE_VREGISTERS)
#undef DEFINE_VREGISTERS


// Registers aliases.
constexpr Register ip0 = x16;
constexpr Register ip1 = x17;
constexpr Register lr = x30;
constexpr Register xzr = x31;
constexpr Register wzr = w31;


// AreAliased returns true if any of the named registers overlap. Arguments
// set to NoReg are ignored. The system stack pointer may be specified.
bool AreAliased(const CPURegister& reg1,
                const CPURegister& reg2,
                const CPURegister& reg3 = NoReg,
                const CPURegister& reg4 = NoReg,
                const CPURegister& reg5 = NoReg,
                const CPURegister& reg6 = NoReg,
                const CPURegister& reg7 = NoReg,
                const CPURegister& reg8 = NoReg);


// AreSameSizeAndType returns true if all of the specified registers have the
// same size, and are of the same type. The system stack pointer may be
// specified. Arguments set to NoReg are ignored, as are any subsequent
// arguments. At least one argument (reg1) must be valid (not NoCPUReg).
bool AreSameSizeAndType(const CPURegister& reg1,
                        const CPURegister& reg2,
                        const CPURegister& reg3 = NoCPUReg,
                        const CPURegister& reg4 = NoCPUReg,
                        const CPURegister& reg5 = NoCPUReg,
                        const CPURegister& reg6 = NoCPUReg,
                        const CPURegister& reg7 = NoCPUReg,
                        const CPURegister& reg8 = NoCPUReg);


// AreSameFormat returns true if all of the specified VRegisters have the same
// vector format. Arguments set to NoReg are ignored, as are any subsequent
// arguments. At least one argument (reg1) must be valid (not NoVReg).
bool AreSameFormat(const VRegister& reg1,
                   const VRegister& reg2,
                   const VRegister& reg3 = NoVReg,
                   const VRegister& reg4 = NoVReg);


// AreConsecutive returns true if all of the specified VRegisters are
// consecutive in the register file. Arguments set to NoReg are ignored, as are
// any subsequent arguments. At least one argument (reg1) must be valid
// (not NoVReg).
bool AreConsecutive(const VRegister& reg1,
                    const VRegister& reg2,
                    const VRegister& reg3 = NoVReg,
                    const VRegister& reg4 = NoVReg);


// Lists of registers.
class CPURegList {
 public:
  explicit CPURegList(CPURegister reg1,
                      CPURegister reg2 = NoCPUReg,
                      CPURegister reg3 = NoCPUReg,
                      CPURegister reg4 = NoCPUReg)
      : list_(reg1.Bit() | reg2.Bit() | reg3.Bit() | reg4.Bit()),
        size_(reg1.size()), type_(reg1.type()) {
    VIXL_ASSERT(AreSameSizeAndType(reg1, reg2, reg3, reg4));
    VIXL_ASSERT(IsValid());
  }

  CPURegList(CPURegister::RegisterType type, unsigned size, RegList list)
      : list_(list), size_(size), type_(type) {
    VIXL_ASSERT(IsValid());
  }

  CPURegList(CPURegister::RegisterType type, unsigned size,
             unsigned first_reg, unsigned last_reg)
      : size_(size), type_(type) {
    VIXL_ASSERT(((type == CPURegister::kRegister) &&
                 (last_reg < kNumberOfRegisters)) ||
                ((type == CPURegister::kVRegister) &&
                 (last_reg < kNumberOfVRegisters)));
    VIXL_ASSERT(last_reg >= first_reg);
    list_ = (UINT64_C(1) << (last_reg + 1)) - 1;
    list_ &= ~((UINT64_C(1) << first_reg) - 1);
    VIXL_ASSERT(IsValid());
  }

  CPURegister::RegisterType type() const {
    VIXL_ASSERT(IsValid());
    return type_;
  }

  // Combine another CPURegList into this one. Registers that already exist in
  // this list are left unchanged. The type and size of the registers in the
  // 'other' list must match those in this list.
  void Combine(const CPURegList& other) {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(other.type() == type_);
    VIXL_ASSERT(other.RegisterSizeInBits() == size_);
    list_ |= other.list();
  }

  // Remove every register in the other CPURegList from this one. Registers that
  // do not exist in this list are ignored. The type and size of the registers
  // in the 'other' list must match those in this list.
  void Remove(const CPURegList& other) {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(other.type() == type_);
    VIXL_ASSERT(other.RegisterSizeInBits() == size_);
    list_ &= ~other.list();
  }

  // Variants of Combine and Remove which take a single register.
  void Combine(const CPURegister& other) {
    VIXL_ASSERT(other.type() == type_);
    VIXL_ASSERT(other.size() == size_);
    Combine(other.code());
  }

  void Remove(const CPURegister& other) {
    VIXL_ASSERT(other.type() == type_);
    VIXL_ASSERT(other.size() == size_);
    Remove(other.code());
  }

  // Variants of Combine and Remove which take a single register by its code;
  // the type and size of the register is inferred from this list.
  void Combine(int code) {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(CPURegister(code, size_, type_).IsValid());
    list_ |= (UINT64_C(1) << code);
  }

  void Remove(int code) {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(CPURegister(code, size_, type_).IsValid());
    list_ &= ~(UINT64_C(1) << code);
  }

  static CPURegList Union(const CPURegList& list_1, const CPURegList& list_2) {
    VIXL_ASSERT(list_1.type_ == list_2.type_);
    VIXL_ASSERT(list_1.size_ == list_2.size_);
    return CPURegList(list_1.type_, list_1.size_, list_1.list_ | list_2.list_);
  }
  static CPURegList Union(const CPURegList& list_1,
                          const CPURegList& list_2,
                          const CPURegList& list_3);
  static CPURegList Union(const CPURegList& list_1,
                          const CPURegList& list_2,
                          const CPURegList& list_3,
                          const CPURegList& list_4);

  static CPURegList Intersection(const CPURegList& list_1,
                                 const CPURegList& list_2) {
    VIXL_ASSERT(list_1.type_ == list_2.type_);
    VIXL_ASSERT(list_1.size_ == list_2.size_);
    return CPURegList(list_1.type_, list_1.size_, list_1.list_ & list_2.list_);
  }
  static CPURegList Intersection(const CPURegList& list_1,
                                 const CPURegList& list_2,
                                 const CPURegList& list_3);
  static CPURegList Intersection(const CPURegList& list_1,
                                 const CPURegList& list_2,
                                 const CPURegList& list_3,
                                 const CPURegList& list_4);

  bool Overlaps(const CPURegList& other) const {
    return (type_ == other.type_) && ((list_ & other.list_) != 0);
  }

  RegList list() const {
    VIXL_ASSERT(IsValid());
    return list_;
  }

  void set_list(RegList new_list) {
    VIXL_ASSERT(IsValid());
    list_ = new_list;
  }

  // Remove all callee-saved registers from the list. This can be useful when
  // preparing registers for an AAPCS64 function call, for example.
  void RemoveCalleeSaved();

  CPURegister PopLowestIndex();
  CPURegister PopHighestIndex();

  // AAPCS64 callee-saved registers.
  static CPURegList GetCalleeSaved(unsigned size = kXRegSize);
  static CPURegList GetCalleeSavedV(unsigned size = kDRegSize);

  // AAPCS64 caller-saved registers. Note that this includes lr.
  // TODO(all): Determine how we handle d8-d15 being callee-saved, but the top
  // 64-bits being caller-saved.
  static CPURegList GetCallerSaved(unsigned size = kXRegSize);
  static CPURegList GetCallerSavedV(unsigned size = kDRegSize);

  bool IsEmpty() const {
    VIXL_ASSERT(IsValid());
    return list_ == 0;
  }

  bool IncludesAliasOf(const CPURegister& other) const {
    VIXL_ASSERT(IsValid());
    return (type_ == other.type()) && ((other.Bit() & list_) != 0);
  }

  bool IncludesAliasOf(int code) const {
    VIXL_ASSERT(IsValid());
    return ((code & list_) != 0);
  }

  int Count() const {
    VIXL_ASSERT(IsValid());
    return CountSetBits(list_);
  }

  unsigned RegisterSizeInBits() const {
    VIXL_ASSERT(IsValid());
    return size_;
  }

  unsigned RegisterSizeInBytes() const {
    int size_in_bits = RegisterSizeInBits();
    VIXL_ASSERT((size_in_bits % 8) == 0);
    return size_in_bits / 8;
  }

  unsigned TotalSizeInBytes() const {
    VIXL_ASSERT(IsValid());
    return RegisterSizeInBytes() * Count();
  }

 private:
  RegList list_;
  unsigned size_;
  CPURegister::RegisterType type_;

  bool IsValid() const;
};


// AAPCS64 callee-saved registers.
extern const CPURegList kCalleeSaved;
extern const CPURegList kCalleeSavedV;


// AAPCS64 caller-saved registers. Note that this includes lr.
extern const CPURegList kCallerSaved;
extern const CPURegList kCallerSavedV;


// Operand.
class Operand {
 public:
  // #<immediate>
  // where <immediate> is int64_t.
  // This is allowed to be an implicit constructor because Operand is
  // a wrapper class that doesn't normally perform any type conversion.
  Operand(int64_t immediate = 0);           // NOLINT(runtime/explicit)

  // rm, {<shift> #<shift_amount>}
  // where <shift> is one of {LSL, LSR, ASR, ROR}.
  //       <shift_amount> is uint6_t.
  // This is allowed to be an implicit constructor because Operand is
  // a wrapper class that doesn't normally perform any type conversion.
  Operand(Register reg,
          Shift shift = LSL,
          unsigned shift_amount = 0);   // NOLINT(runtime/explicit)

  // rm, {<extend> {#<shift_amount>}}
  // where <extend> is one of {UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW, SXTX}.
  //       <shift_amount> is uint2_t.
  explicit Operand(Register reg, Extend extend, unsigned shift_amount = 0);

  // FIXME: Temporary constructors for compilation.
  // FIXME: These should be removed -- Operand should not leak into shared code.
  // FIXME: Something like an LAllocationUnion for {gpreg, fpreg, Address} is wanted.
  explicit Operand(js::jit::Register) {
    MOZ_CRASH("Operand with Register");
  }
  explicit Operand(js::jit::FloatRegister) {
    MOZ_CRASH("Operand with FloatRegister");
  }
  explicit Operand(js::jit::Register, int32_t) {
    MOZ_CRASH("Operand with implicit Address");
  }
  explicit Operand(js::jit::RegisterOrSP, int32_t) {
    MOZ_CRASH("Operand with implicit Address");
  }

  bool IsImmediate() const;
  bool IsShiftedRegister() const;
  bool IsExtendedRegister() const;
  bool IsZero() const;

  // This returns an LSL shift (<= 4) operand as an equivalent extend operand,
  // which helps in the encoding of instructions that use the stack pointer.
  Operand ToExtendedRegister() const;

  int64_t immediate() const {
    VIXL_ASSERT(IsImmediate());
    return immediate_;
  }

  Register reg() const {
    VIXL_ASSERT(IsShiftedRegister() || IsExtendedRegister());
    return reg_;
  }

  CPURegister maybeReg() const {
    if (IsShiftedRegister() || IsExtendedRegister())
      return reg_;
    return NoCPUReg;
  }

  Shift shift() const {
    VIXL_ASSERT(IsShiftedRegister());
    return shift_;
  }

  Extend extend() const {
    VIXL_ASSERT(IsExtendedRegister());
    return extend_;
  }

  unsigned shift_amount() const {
    VIXL_ASSERT(IsShiftedRegister() || IsExtendedRegister());
    return shift_amount_;
  }

 private:
  int64_t immediate_;
  Register reg_;
  Shift shift_;
  Extend extend_;
  unsigned shift_amount_;
};


// MemOperand represents the addressing mode of a load or store instruction.
class MemOperand {
 public:
  explicit MemOperand(Register base,
                      int64_t offset = 0,
                      AddrMode addrmode = Offset);
  MemOperand(Register base,
             Register regoffset,
             Shift shift = LSL,
             unsigned shift_amount = 0);
  MemOperand(Register base,
             Register regoffset,
             Extend extend,
             unsigned shift_amount = 0);
  MemOperand(Register base,
             const Operand& offset,
             AddrMode addrmode = Offset);

  // Adapter constructors using C++11 delegating.
  // TODO: If sp == kSPRegInternalCode, the xzr check isn't necessary.
  explicit MemOperand(js::jit::Address addr)
    : MemOperand(IsHiddenSP(addr.base) ? sp : Register(AsRegister(addr.base), 64),
                 (ptrdiff_t)addr.offset) {
  }

  const Register& base() const { return base_; }
  const Register& regoffset() const { return regoffset_; }
  int64_t offset() const { return offset_; }
  AddrMode addrmode() const { return addrmode_; }
  Shift shift() const { return shift_; }
  Extend extend() const { return extend_; }
  unsigned shift_amount() const { return shift_amount_; }
  bool IsImmediateOffset() const;
  bool IsRegisterOffset() const;
  bool IsPreIndex() const;
  bool IsPostIndex() const;

  void AddOffset(int64_t offset);

 private:
  Register base_;
  Register regoffset_;
  int64_t offset_;
  AddrMode addrmode_;
  Shift shift_;
  Extend extend_;
  unsigned shift_amount_;
};


// Control whether or not position-independent code should be emitted.
enum PositionIndependentCodeOption {
  // All code generated will be position-independent; all branches and
  // references to labels generated with the Label class will use PC-relative
  // addressing.
  PositionIndependentCode,

  // Allow VIXL to generate code that refers to absolute addresses. With this
  // option, it will not be possible to copy the code buffer and run it from a
  // different address; code must be generated in its final location.
  PositionDependentCode,

  // Allow VIXL to assume that the bottom 12 bits of the address will be
  // constant, but that the top 48 bits may change. This allows `adrp` to
  // function in systems which copy code between pages, but otherwise maintain
  // 4KB page alignment.
  PageOffsetDependentCode
};


// Control how scaled- and unscaled-offset loads and stores are generated.
enum LoadStoreScalingOption {
  // Prefer scaled-immediate-offset instructions, but emit unscaled-offset,
  // register-offset, pre-index or post-index instructions if necessary.
  PreferScaledOffset,

  // Prefer unscaled-immediate-offset instructions, but emit scaled-offset,
  // register-offset, pre-index or post-index instructions if necessary.
  PreferUnscaledOffset,

  // Require scaled-immediate-offset instructions.
  RequireScaledOffset,

  // Require unscaled-immediate-offset instructions.
  RequireUnscaledOffset
};


// Assembler.
class Assembler : public MozBaseAssembler {
 public:
  Assembler(PositionIndependentCodeOption pic = PositionIndependentCode);

  // System functions.

  // Finalize a code buffer of generated instructions. This function must be
  // called before executing or copying code from the buffer.
  void FinalizeCode();

#define COPYENUM(v) static const Condition v = vixl::v
#define COPYENUM_(v) static const Condition v = vixl::v##_
  COPYENUM(Equal);
  COPYENUM(Zero);
  COPYENUM(NotEqual);
  COPYENUM(NonZero);
  COPYENUM(AboveOrEqual);
  COPYENUM(CarrySet);
  COPYENUM(Below);
  COPYENUM(CarryClear);
  COPYENUM(Signed);
  COPYENUM(NotSigned);
  COPYENUM(Overflow);
  COPYENUM(NoOverflow);
  COPYENUM(Above);
  COPYENUM(BelowOrEqual);
  COPYENUM_(GreaterThanOrEqual);
  COPYENUM_(LessThan);
  COPYENUM_(GreaterThan);
  COPYENUM_(LessThanOrEqual);
  COPYENUM(Always);
  COPYENUM(Never);
#undef COPYENUM
#undef COPYENUM_

  // Bit set when a DoubleCondition does not map to a single ARM condition.
  // The MacroAssembler must special-case these conditions, or else
  // ConditionFromDoubleCondition will complain.
  static const int DoubleConditionBitSpecial = 0x100;

  enum DoubleCondition {
    DoubleOrdered                        = Condition::vc,
    DoubleEqual                          = Condition::eq,
    DoubleNotEqual                       = Condition::ne | DoubleConditionBitSpecial,
    DoubleGreaterThan                    = Condition::gt,
    DoubleGreaterThanOrEqual             = Condition::ge,
    DoubleLessThan                       = Condition::lo, // Could also use Condition::mi.
    DoubleLessThanOrEqual                = Condition::ls,

    // If either operand is NaN, these conditions always evaluate to true.
    DoubleUnordered                      = Condition::vs,
    DoubleEqualOrUnordered               = Condition::eq | DoubleConditionBitSpecial,
    DoubleNotEqualOrUnordered            = Condition::ne,
    DoubleGreaterThanOrUnordered         = Condition::hi,
    DoubleGreaterThanOrEqualOrUnordered  = Condition::hs,
    DoubleLessThanOrUnordered            = Condition::lt,
    DoubleLessThanOrEqualOrUnordered     = Condition::le
  };

  static inline Condition InvertCondition(Condition cond) {
    // Conditions al and nv behave identically, as "always true". They can't be
    // inverted, because there is no "always false" condition.
    VIXL_ASSERT((cond != al) && (cond != nv));
    return static_cast<Condition>(cond ^ 1);
  }

  // This is chaging the condition codes for cmp a, b to the same codes for cmp b, a.
  static inline Condition InvertCmpCondition(Condition cond) {
    // Conditions al and nv behave identically, as "always true". They can't be
    // inverted, because there is no "always false" condition.
    switch (cond) {
    case eq:
    case ne:
      return cond;
    case gt:
      return le;
    case le:
      return gt;
    case ge:
      return lt;
    case lt:
      return ge;
    case hi:
      return lo;
    case lo:
      return hi;
    case hs:
      return ls;
    case ls:
      return hs;
    case mi:
      return pl;
    case pl:
      return mi;
    default:
      MOZ_CRASH("TODO: figure this case out.");
    }
    return static_cast<Condition>(cond ^ 1);
  }

  static inline DoubleCondition InvertCondition(DoubleCondition cond) {
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
	  MOZ_CRASH("Bad condition");
    }
  }

  static inline Condition ConditionFromDoubleCondition(DoubleCondition cond) {
    VIXL_ASSERT(!(cond & DoubleConditionBitSpecial));
    return static_cast<Condition>(cond);
  }

  // Instruction set functions.

  // Branch / Jump instructions.
  // Branch to register.
  void br(const Register& xn);
  static void br(Instruction* at, const Register& xn);

  // Branch with link to register.
  void blr(const Register& xn);
  static void blr(Instruction* at, const Register& blr);

  // Branch to register with return hint.
  void ret(const Register& xn = lr);

  // Unconditional branch to label.
  BufferOffset b(Label* label);

  // Conditional branch to label.
  BufferOffset b(Label* label, Condition cond);

  // Unconditional branch to PC offset.
  BufferOffset b(int imm26, const LabelDoc& doc);
  static void b(Instruction* at, int imm26);

  // Conditional branch to PC offset.
  BufferOffset b(int imm19, Condition cond, const LabelDoc& doc);
  static void b(Instruction*at, int imm19, Condition cond);

  // Branch with link to label.
  void bl(Label* label);

  // Branch with link to PC offset.
  void bl(int imm26, const LabelDoc& doc);
  static void bl(Instruction* at, int imm26);

  // Compare and branch to label if zero.
  void cbz(const Register& rt, Label* label);

  // Compare and branch to PC offset if zero.
  void cbz(const Register& rt, int imm19, const LabelDoc& doc);
  static void cbz(Instruction* at, const Register& rt, int imm19);

  // Compare and branch to label if not zero.
  void cbnz(const Register& rt, Label* label);

  // Compare and branch to PC offset if not zero.
  void cbnz(const Register& rt, int imm19, const LabelDoc& doc);
  static void cbnz(Instruction* at, const Register& rt, int imm19);

  // Table lookup from one register.
  void tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Table lookup from two registers.
  void tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vm);

  // Table lookup from three registers.
  void tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vm);

  // Table lookup from four registers.
  void tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vn4,
           const VRegister& vm);

  // Table lookup extension from one register.
  void tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Table lookup extension from two registers.
  void tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vm);

  // Table lookup extension from three registers.
  void tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vm);

  // Table lookup extension from four registers.
  void tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vn4,
           const VRegister& vm);

  // Test bit and branch to label if zero.
  void tbz(const Register& rt, unsigned bit_pos, Label* label);

  // Test bit and branch to PC offset if zero.
  void tbz(const Register& rt, unsigned bit_pos, int imm14, const LabelDoc& doc);
  static void tbz(Instruction* at, const Register& rt, unsigned bit_pos, int imm14);

  // Test bit and branch to label if not zero.
  void tbnz(const Register& rt, unsigned bit_pos, Label* label);

  // Test bit and branch to PC offset if not zero.
  void tbnz(const Register& rt, unsigned bit_pos, int imm14, const LabelDoc& doc);
  static void tbnz(Instruction* at, const Register& rt, unsigned bit_pos, int imm14);

  // Address calculation instructions.
  // Calculate a PC-relative address. Unlike for branches the offset in adr is
  // unscaled (i.e. the result can be unaligned).

  // Calculate the address of a label.
  void adr(const Register& rd, Label* label);

  // Calculate the address of a PC offset.
  void adr(const Register& rd, int imm21, const LabelDoc& doc);
  static void adr(Instruction* at, const Register& rd, int imm21);

  // Calculate the page address of a label.
  void adrp(const Register& rd, Label* label);

  // Calculate the page address of a PC offset.
  void adrp(const Register& rd, int imm21, const LabelDoc& doc);
  static void adrp(Instruction* at, const Register& rd, int imm21);

  // Data Processing instructions.
  // Add.
  void add(const Register& rd,
           const Register& rn,
           const Operand& operand);

  // Add and update status flags.
  void adds(const Register& rd,
            const Register& rn,
            const Operand& operand);

  // Compare negative.
  void cmn(const Register& rn, const Operand& operand);

  // Subtract.
  void sub(const Register& rd,
           const Register& rn,
           const Operand& operand);

  // Subtract and update status flags.
  void subs(const Register& rd,
            const Register& rn,
            const Operand& operand);

  // Compare.
  void cmp(const Register& rn, const Operand& operand);

  // Negate.
  void neg(const Register& rd,
           const Operand& operand);

  // Negate and update status flags.
  void negs(const Register& rd,
            const Operand& operand);

  // Add with carry bit.
  void adc(const Register& rd,
           const Register& rn,
           const Operand& operand);

  // Add with carry bit and update status flags.
  void adcs(const Register& rd,
            const Register& rn,
            const Operand& operand);

  // Subtract with carry bit.
  void sbc(const Register& rd,
           const Register& rn,
           const Operand& operand);

  // Subtract with carry bit and update status flags.
  void sbcs(const Register& rd,
            const Register& rn,
            const Operand& operand);

  // Negate with carry bit.
  void ngc(const Register& rd,
           const Operand& operand);

  // Negate with carry bit and update status flags.
  void ngcs(const Register& rd,
            const Operand& operand);

  // Logical instructions.
  // Bitwise and (A & B).
  void and_(const Register& rd,
            const Register& rn,
            const Operand& operand);

  // Bitwise and (A & B) and update status flags.
  BufferOffset ands(const Register& rd,
                    const Register& rn,
                    const Operand& operand);

  // Bit test and set flags.
  BufferOffset tst(const Register& rn, const Operand& operand);

  // Bit clear (A & ~B).
  void bic(const Register& rd,
           const Register& rn,
           const Operand& operand);

  // Bit clear (A & ~B) and update status flags.
  void bics(const Register& rd,
            const Register& rn,
            const Operand& operand);

  // Bitwise or (A | B).
  void orr(const Register& rd, const Register& rn, const Operand& operand);

  // Bitwise nor (A | ~B).
  void orn(const Register& rd, const Register& rn, const Operand& operand);

  // Bitwise eor/xor (A ^ B).
  void eor(const Register& rd, const Register& rn, const Operand& operand);

  // Bitwise enor/xnor (A ^ ~B).
  void eon(const Register& rd, const Register& rn, const Operand& operand);

  // Logical shift left by variable.
  void lslv(const Register& rd, const Register& rn, const Register& rm);

  // Logical shift right by variable.
  void lsrv(const Register& rd, const Register& rn, const Register& rm);

  // Arithmetic shift right by variable.
  void asrv(const Register& rd, const Register& rn, const Register& rm);

  // Rotate right by variable.
  void rorv(const Register& rd, const Register& rn, const Register& rm);

  // Bitfield instructions.
  // Bitfield move.
  void bfm(const Register& rd,
           const Register& rn,
           unsigned immr,
           unsigned imms);

  // Signed bitfield move.
  void sbfm(const Register& rd,
            const Register& rn,
            unsigned immr,
            unsigned imms);

  // Unsigned bitfield move.
  void ubfm(const Register& rd,
            const Register& rn,
            unsigned immr,
            unsigned imms);

  // Bfm aliases.
  // Bitfield insert.
  void bfi(const Register& rd,
           const Register& rn,
           unsigned lsb,
           unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= rn.size());
    bfm(rd, rn, (rd.size() - lsb) & (rd.size() - 1), width - 1);
  }

  // Bitfield extract and insert low.
  void bfxil(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= rn.size());
    bfm(rd, rn, lsb, lsb + width - 1);
  }

  // Sbfm aliases.
  // Arithmetic shift right.
  void asr(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(shift < rd.size());
    sbfm(rd, rn, shift, rd.size() - 1);
  }

  // Signed bitfield insert with zero at right.
  void sbfiz(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= rn.size());
    sbfm(rd, rn, (rd.size() - lsb) & (rd.size() - 1), width - 1);
  }

  // Signed bitfield extract.
  void sbfx(const Register& rd,
            const Register& rn,
            unsigned lsb,
            unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= rn.size());
    sbfm(rd, rn, lsb, lsb + width - 1);
  }

  // Signed extend byte.
  void sxtb(const Register& rd, const Register& rn) {
    sbfm(rd, rn, 0, 7);
  }

  // Signed extend halfword.
  void sxth(const Register& rd, const Register& rn) {
    sbfm(rd, rn, 0, 15);
  }

  // Signed extend word.
  void sxtw(const Register& rd, const Register& rn) {
    sbfm(rd, rn, 0, 31);
  }

  // Ubfm aliases.
  // Logical shift left.
  void lsl(const Register& rd, const Register& rn, unsigned shift) {
    unsigned reg_size = rd.size();
    VIXL_ASSERT(shift < reg_size);
    ubfm(rd, rn, (reg_size - shift) % reg_size, reg_size - shift - 1);
  }

  // Logical shift right.
  void lsr(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(shift < rd.size());
    ubfm(rd, rn, shift, rd.size() - 1);
  }

  // Unsigned bitfield insert with zero at right.
  void ubfiz(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= rn.size());
    ubfm(rd, rn, (rd.size() - lsb) & (rd.size() - 1), width - 1);
  }

  // Unsigned bitfield extract.
  void ubfx(const Register& rd,
            const Register& rn,
            unsigned lsb,
            unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= rn.size());
    ubfm(rd, rn, lsb, lsb + width - 1);
  }

  // Unsigned extend byte.
  void uxtb(const Register& rd, const Register& rn) {
    ubfm(rd, rn, 0, 7);
  }

  // Unsigned extend halfword.
  void uxth(const Register& rd, const Register& rn) {
    ubfm(rd, rn, 0, 15);
  }

  // Unsigned extend word.
  void uxtw(const Register& rd, const Register& rn) {
    ubfm(rd, rn, 0, 31);
  }

  // Extract.
  void extr(const Register& rd,
            const Register& rn,
            const Register& rm,
            unsigned lsb);

  // Conditional select: rd = cond ? rn : rm.
  void csel(const Register& rd,
            const Register& rn,
            const Register& rm,
            Condition cond);

  // Conditional select increment: rd = cond ? rn : rm + 1.
  void csinc(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond);

  // Conditional select inversion: rd = cond ? rn : ~rm.
  void csinv(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond);

  // Conditional select negation: rd = cond ? rn : -rm.
  void csneg(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond);

  // Conditional set: rd = cond ? 1 : 0.
  void cset(const Register& rd, Condition cond);

  // Conditional set mask: rd = cond ? -1 : 0.
  void csetm(const Register& rd, Condition cond);

  // Conditional increment: rd = cond ? rn + 1 : rn.
  void cinc(const Register& rd, const Register& rn, Condition cond);

  // Conditional invert: rd = cond ? ~rn : rn.
  void cinv(const Register& rd, const Register& rn, Condition cond);

  // Conditional negate: rd = cond ? -rn : rn.
  void cneg(const Register& rd, const Register& rn, Condition cond);

  // Rotate right.
  void ror(const Register& rd, const Register& rs, unsigned shift) {
    extr(rd, rs, rs, shift);
  }

  // Conditional comparison.
  // Conditional compare negative.
  void ccmn(const Register& rn,
            const Operand& operand,
            StatusFlags nzcv,
            Condition cond);

  // Conditional compare.
  void ccmp(const Register& rn,
            const Operand& operand,
            StatusFlags nzcv,
            Condition cond);

  // CRC-32 checksum from byte.
  void crc32b(const Register& rd,
              const Register& rn,
              const Register& rm);

  // CRC-32 checksum from half-word.
  void crc32h(const Register& rd,
              const Register& rn,
              const Register& rm);

  // CRC-32 checksum from word.
  void crc32w(const Register& rd,
              const Register& rn,
              const Register& rm);

  // CRC-32 checksum from double word.
  void crc32x(const Register& rd,
              const Register& rn,
              const Register& rm);

  // CRC-32 C checksum from byte.
  void crc32cb(const Register& rd,
               const Register& rn,
               const Register& rm);

  // CRC-32 C checksum from half-word.
  void crc32ch(const Register& rd,
               const Register& rn,
               const Register& rm);

  // CRC-32 C checksum from word.
  void crc32cw(const Register& rd,
               const Register& rn,
               const Register& rm);

  // CRC-32C checksum from double word.
  void crc32cx(const Register& rd,
               const Register& rn,
               const Register& rm);

  // Multiply.
  void mul(const Register& rd, const Register& rn, const Register& rm);

  // Negated multiply.
  void mneg(const Register& rd, const Register& rn, const Register& rm);

  // Signed long multiply: 32 x 32 -> 64-bit.
  void smull(const Register& rd, const Register& rn, const Register& rm);

  // Signed multiply high: 64 x 64 -> 64-bit <127:64>.
  void smulh(const Register& xd, const Register& xn, const Register& xm);

  // Multiply and accumulate.
  void madd(const Register& rd,
            const Register& rn,
            const Register& rm,
            const Register& ra);

  // Multiply and subtract.
  void msub(const Register& rd,
            const Register& rn,
            const Register& rm,
            const Register& ra);

  // Signed long multiply and accumulate: 32 x 32 + 64 -> 64-bit.
  void smaddl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra);

  // Unsigned long multiply and accumulate: 32 x 32 + 64 -> 64-bit.
  void umaddl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra);

  // Unsigned long multiply: 32 x 32 -> 64-bit.
  void umull(const Register& rd,
             const Register& rn,
             const Register& rm) {
    umaddl(rd, rn, rm, xzr);
  }

  // Unsigned multiply high: 64 x 64 -> 64-bit <127:64>.
  void umulh(const Register& xd,
             const Register& xn,
             const Register& xm);

  // Signed long multiply and subtract: 64 - (32 x 32) -> 64-bit.
  void smsubl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra);

  // Unsigned long multiply and subtract: 64 - (32 x 32) -> 64-bit.
  void umsubl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra);

  // Signed integer divide.
  void sdiv(const Register& rd, const Register& rn, const Register& rm);

  // Unsigned integer divide.
  void udiv(const Register& rd, const Register& rn, const Register& rm);

  // Bit reverse.
  void rbit(const Register& rd, const Register& rn);

  // Reverse bytes in 16-bit half words.
  void rev16(const Register& rd, const Register& rn);

  // Reverse bytes in 32-bit words.
  void rev32(const Register& rd, const Register& rn);

  // Reverse bytes.
  void rev(const Register& rd, const Register& rn);

  // Count leading zeroes.
  void clz(const Register& rd, const Register& rn);

  // Count leading sign bits.
  void cls(const Register& rd, const Register& rn);

  // Memory instructions.
  // Load integer or FP register.
  void ldr(const CPURegister& rt, const MemOperand& src,
           LoadStoreScalingOption option = PreferScaledOffset);

  // Store integer or FP register.
  void str(const CPURegister& rt, const MemOperand& dst,
           LoadStoreScalingOption option = PreferScaledOffset);

  // Load word with sign extension.
  void ldrsw(const Register& rt, const MemOperand& src,
             LoadStoreScalingOption option = PreferScaledOffset);

  // Load byte.
  void ldrb(const Register& rt, const MemOperand& src,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Store byte.
  void strb(const Register& rt, const MemOperand& dst,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Load byte with sign extension.
  void ldrsb(const Register& rt, const MemOperand& src,
             LoadStoreScalingOption option = PreferScaledOffset);

  // Load half-word.
  void ldrh(const Register& rt, const MemOperand& src,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Store half-word.
  void strh(const Register& rt, const MemOperand& dst,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Load half-word with sign extension.
  void ldrsh(const Register& rt, const MemOperand& src,
             LoadStoreScalingOption option = PreferScaledOffset);

  // Load integer or FP register (with unscaled offset).
  void ldur(const CPURegister& rt, const MemOperand& src,
            LoadStoreScalingOption option = PreferUnscaledOffset);

  // Store integer or FP register (with unscaled offset).
  void stur(const CPURegister& rt, const MemOperand& src,
            LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load word with sign extension.
  void ldursw(const Register& rt, const MemOperand& src,
              LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load byte (with unscaled offset).
  void ldurb(const Register& rt, const MemOperand& src,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Store byte (with unscaled offset).
  void sturb(const Register& rt, const MemOperand& dst,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load byte with sign extension (and unscaled offset).
  void ldursb(const Register& rt, const MemOperand& src,
              LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load half-word (with unscaled offset).
  void ldurh(const Register& rt, const MemOperand& src,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Store half-word (with unscaled offset).
  void sturh(const Register& rt, const MemOperand& dst,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load half-word with sign extension (and unscaled offset).
  void ldursh(const Register& rt, const MemOperand& src,
              LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load integer or FP register pair.
  void ldp(const CPURegister& rt, const CPURegister& rt2,
           const MemOperand& src);

  // Store integer or FP register pair.
  void stp(const CPURegister& rt, const CPURegister& rt2,
           const MemOperand& dst);

  // Load word pair with sign extension.
  void ldpsw(const Register& rt, const Register& rt2, const MemOperand& src);

  // Load integer or FP register pair, non-temporal.
  void ldnp(const CPURegister& rt, const CPURegister& rt2,
            const MemOperand& src);

  // Store integer or FP register pair, non-temporal.
  void stnp(const CPURegister& rt, const CPURegister& rt2,
            const MemOperand& dst);

  // Load integer or FP register from pc + imm19 << 2.
  void ldr(const CPURegister& rt, int imm19);
  static void ldr(Instruction* at, const CPURegister& rt, int imm19);

  // Load word with sign extension from pc + imm19 << 2.
  void ldrsw(const Register& rt, int imm19);

  // Store exclusive byte.
  void stxrb(const Register& rs, const Register& rt, const MemOperand& dst);

  // Store exclusive half-word.
  void stxrh(const Register& rs, const Register& rt, const MemOperand& dst);

  // Store exclusive register.
  void stxr(const Register& rs, const Register& rt, const MemOperand& dst);

  // Load exclusive byte.
  void ldxrb(const Register& rt, const MemOperand& src);

  // Load exclusive half-word.
  void ldxrh(const Register& rt, const MemOperand& src);

  // Load exclusive register.
  void ldxr(const Register& rt, const MemOperand& src);

  // Store exclusive register pair.
  void stxp(const Register& rs,
            const Register& rt,
            const Register& rt2,
            const MemOperand& dst);

  // Load exclusive register pair.
  void ldxp(const Register& rt, const Register& rt2, const MemOperand& src);

  // Store-release exclusive byte.
  void stlxrb(const Register& rs, const Register& rt, const MemOperand& dst);

  // Store-release exclusive half-word.
  void stlxrh(const Register& rs, const Register& rt, const MemOperand& dst);

  // Store-release exclusive register.
  void stlxr(const Register& rs, const Register& rt, const MemOperand& dst);

  // Load-acquire exclusive byte.
  void ldaxrb(const Register& rt, const MemOperand& src);

  // Load-acquire exclusive half-word.
  void ldaxrh(const Register& rt, const MemOperand& src);

  // Load-acquire exclusive register.
  void ldaxr(const Register& rt, const MemOperand& src);

  // Store-release exclusive register pair.
  void stlxp(const Register& rs,
             const Register& rt,
             const Register& rt2,
             const MemOperand& dst);

  // Load-acquire exclusive register pair.
  void ldaxp(const Register& rt, const Register& rt2, const MemOperand& src);

  // Store-release byte.
  void stlrb(const Register& rt, const MemOperand& dst);

  // Store-release half-word.
  void stlrh(const Register& rt, const MemOperand& dst);

  // Store-release register.
  void stlr(const Register& rt, const MemOperand& dst);

  // Load-acquire byte.
  void ldarb(const Register& rt, const MemOperand& src);

  // Load-acquire half-word.
  void ldarh(const Register& rt, const MemOperand& src);

  // Load-acquire register.
  void ldar(const Register& rt, const MemOperand& src);

  // Prefetch memory.
  void prfm(PrefetchOperation op, const MemOperand& addr,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Prefetch memory (with unscaled offset).
  void prfum(PrefetchOperation op, const MemOperand& addr,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Prefetch from pc + imm19 << 2.
  void prfm(PrefetchOperation op, int imm19);

  // Move instructions. The default shift of -1 indicates that the move
  // instruction will calculate an appropriate 16-bit immediate and left shift
  // that is equal to the 64-bit immediate argument. If an explicit left shift
  // is specified (0, 16, 32 or 48), the immediate must be a 16-bit value.
  //
  // For movk, an explicit shift can be used to indicate which half word should
  // be overwritten, eg. movk(x0, 0, 0) will overwrite the least-significant
  // half word with zero, whereas movk(x0, 0, 48) will overwrite the
  // most-significant.

  // Move immediate and keep.
  void movk(const Register& rd, uint64_t imm, int shift = -1) {
    MoveWide(rd, imm, shift, MOVK);
  }

  // Move inverted immediate.
  void movn(const Register& rd, uint64_t imm, int shift = -1) {
    MoveWide(rd, imm, shift, MOVN);
  }

  // Move immediate.
  void movz(const Register& rd, uint64_t imm, int shift = -1) {
    MoveWide(rd, imm, shift, MOVZ);
  }

  // Misc instructions.
  // Monitor debug-mode breakpoint.
  void brk(int code);

  // Halting debug-mode breakpoint.
  void hlt(int code);

  // Generate exception targeting EL1.
  void svc(int code);
  static void svc(Instruction* at, int code);

  // Move register to register.
  void mov(const Register& rd, const Register& rn);

  // Move inverted operand to register.
  void mvn(const Register& rd, const Operand& operand);

  // System instructions.
  // Move to register from system register.
  void mrs(const Register& rt, SystemRegister sysreg);

  // Move from register to system register.
  void msr(SystemRegister sysreg, const Register& rt);

  // System instruction.
  void sys(int op1, int crn, int crm, int op2, const Register& rt = xzr);

  // System instruction with pre-encoded op (op1:crn:crm:op2).
  void sys(int op, const Register& rt = xzr);

  // System data cache operation.
  void dc(DataCacheOp op, const Register& rt);

  // System instruction cache operation.
  void ic(InstructionCacheOp op, const Register& rt);

  // System hint.
  BufferOffset hint(SystemHint code);
  static void hint(Instruction* at, SystemHint code);

  // Clear exclusive monitor.
  void clrex(int imm4 = 0xf);

  // Data memory barrier.
  void dmb(BarrierDomain domain, BarrierType type);

  // Data synchronization barrier.
  void dsb(BarrierDomain domain, BarrierType type);

  // Instruction synchronization barrier.
  void isb();

  // Alias for system instructions.
  // No-op.
  BufferOffset nop() {
    return hint(NOP);
  }
  static void nop(Instruction* at);

  // Alias for system instructions.
  // Conditional speculation barrier.
  BufferOffset csdb() {
    return hint(CSDB);
  }
  static void csdb(Instruction* at);

  // FP and NEON instructions.
  // Move double precision immediate to FP register.
  void fmov(const VRegister& vd, double imm);

  // Move single precision immediate to FP register.
  void fmov(const VRegister& vd, float imm);

  // Move FP register to register.
  void fmov(const Register& rd, const VRegister& fn);

  // Move register to FP register.
  void fmov(const VRegister& vd, const Register& rn);

  // Move FP register to FP register.
  void fmov(const VRegister& vd, const VRegister& fn);

  // Move 64-bit register to top half of 128-bit FP register.
  void fmov(const VRegister& vd, int index, const Register& rn);

  // Move top half of 128-bit FP register to 64-bit register.
  void fmov(const Register& rd, const VRegister& vn, int index);

  // FP add.
  void fadd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP subtract.
  void fsub(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP multiply.
  void fmul(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP fused multiply-add.
  void fmadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             const VRegister& va);

  // FP fused multiply-subtract.
  void fmsub(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             const VRegister& va);

  // FP fused multiply-add and negate.
  void fnmadd(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              const VRegister& va);

  // FP fused multiply-subtract and negate.
  void fnmsub(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              const VRegister& va);

  // FP multiply-negate scalar.
  void fnmul(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP reciprocal exponent scalar.
  void frecpx(const VRegister& vd,
              const VRegister& vn);

  // FP divide.
  void fdiv(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP maximum.
  void fmax(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP minimum.
  void fmin(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP maximum number.
  void fmaxnm(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP minimum number.
  void fminnm(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP absolute.
  void fabs(const VRegister& vd, const VRegister& vn);

  // FP negate.
  void fneg(const VRegister& vd, const VRegister& vn);

  // FP square root.
  void fsqrt(const VRegister& vd, const VRegister& vn);

  // FP round to integer, nearest with ties to away.
  void frinta(const VRegister& vd, const VRegister& vn);

  // FP round to integer, implicit rounding.
  void frinti(const VRegister& vd, const VRegister& vn);

  // FP round to integer, toward minus infinity.
  void frintm(const VRegister& vd, const VRegister& vn);

  // FP round to integer, nearest with ties to even.
  void frintn(const VRegister& vd, const VRegister& vn);

  // FP round to integer, toward plus infinity.
  void frintp(const VRegister& vd, const VRegister& vn);

  // FP round to integer, exact, implicit rounding.
  void frintx(const VRegister& vd, const VRegister& vn);

  // FP round to integer, towards zero.
  void frintz(const VRegister& vd, const VRegister& vn);

  void FPCompareMacro(const VRegister& vn,
                      double value,
                      FPTrapFlags trap);

  void FPCompareMacro(const VRegister& vn,
                      const VRegister& vm,
                      FPTrapFlags trap);

  // FP compare registers.
  void fcmp(const VRegister& vn, const VRegister& vm);

  // FP compare immediate.
  void fcmp(const VRegister& vn, double value);

  void FPCCompareMacro(const VRegister& vn,
                       const VRegister& vm,
                       StatusFlags nzcv,
                       Condition cond,
                       FPTrapFlags trap);

  // FP conditional compare.
  void fccmp(const VRegister& vn,
             const VRegister& vm,
             StatusFlags nzcv,
             Condition cond);

  // FP signaling compare registers.
  void fcmpe(const VRegister& vn, const VRegister& vm);

  // FP signaling compare immediate.
  void fcmpe(const VRegister& vn, double value);

  // FP conditional signaling compare.
  void fccmpe(const VRegister& vn,
              const VRegister& vm,
              StatusFlags nzcv,
              Condition cond);

  // FP conditional select.
  void fcsel(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             Condition cond);

  // Common FP Convert functions.
  void NEONFPConvertToInt(const Register& rd,
                          const VRegister& vn,
                          Instr op);
  void NEONFPConvertToInt(const VRegister& vd,
                          const VRegister& vn,
                          Instr op);

  // FP convert between precisions.
  void fcvt(const VRegister& vd, const VRegister& vn);

  // FP convert to higher precision.
  void fcvtl(const VRegister& vd, const VRegister& vn);

  // FP convert to higher precision (second part).
  void fcvtl2(const VRegister& vd, const VRegister& vn);

  // FP convert to lower precision.
  void fcvtn(const VRegister& vd, const VRegister& vn);

  // FP convert to lower prevision (second part).
  void fcvtn2(const VRegister& vd, const VRegister& vn);

  // FP convert to lower precision, rounding to odd.
  void fcvtxn(const VRegister& vd, const VRegister& vn);

  // FP convert to lower precision, rounding to odd (second part).
  void fcvtxn2(const VRegister& vd, const VRegister& vn);

  // FP convert to signed integer, nearest with ties to away.
  void fcvtas(const Register& rd, const VRegister& vn);

  // FP convert to unsigned integer, nearest with ties to away.
  void fcvtau(const Register& rd, const VRegister& vn);

  // FP convert to signed integer, nearest with ties to away.
  void fcvtas(const VRegister& vd, const VRegister& vn);

  // FP convert to unsigned integer, nearest with ties to away.
  void fcvtau(const VRegister& vd, const VRegister& vn);

  // FP convert to signed integer, round towards -infinity.
  void fcvtms(const Register& rd, const VRegister& vn);

  // FP convert to unsigned integer, round towards -infinity.
  void fcvtmu(const Register& rd, const VRegister& vn);

  // FP convert to signed integer, round towards -infinity.
  void fcvtms(const VRegister& vd, const VRegister& vn);

  // FP convert to unsigned integer, round towards -infinity.
  void fcvtmu(const VRegister& vd, const VRegister& vn);

  // FP convert to signed integer, nearest with ties to even.
  void fcvtns(const Register& rd, const VRegister& vn);

  // FP convert to unsigned integer, nearest with ties to even.
  void fcvtnu(const Register& rd, const VRegister& vn);

  // FP convert to signed integer, nearest with ties to even.
  void fcvtns(const VRegister& rd, const VRegister& vn);

  // FP convert to unsigned integer, nearest with ties to even.
  void fcvtnu(const VRegister& rd, const VRegister& vn);

  // FP convert to signed integer or fixed-point, round towards zero.
  void fcvtzs(const Register& rd, const VRegister& vn, int fbits = 0);

  // FP convert to unsigned integer or fixed-point, round towards zero.
  void fcvtzu(const Register& rd, const VRegister& vn, int fbits = 0);

  // FP convert to signed integer or fixed-point, round towards zero.
  void fcvtzs(const VRegister& vd, const VRegister& vn, int fbits = 0);

  // FP convert to unsigned integer or fixed-point, round towards zero.
  void fcvtzu(const VRegister& vd, const VRegister& vn, int fbits = 0);

  // FP convert to signed integer, round towards +infinity.
  void fcvtps(const Register& rd, const VRegister& vn);

  // FP convert to unsigned integer, round towards +infinity.
  void fcvtpu(const Register& rd, const VRegister& vn);

  // FP convert to signed integer, round towards +infinity.
  void fcvtps(const VRegister& vd, const VRegister& vn);

  // FP convert to unsigned integer, round towards +infinity.
  void fcvtpu(const VRegister& vd, const VRegister& vn);

  // Convert signed integer or fixed point to FP.
  void scvtf(const VRegister& fd, const Register& rn, int fbits = 0);

  // Convert unsigned integer or fixed point to FP.
  void ucvtf(const VRegister& fd, const Register& rn, int fbits = 0);

  // Convert signed integer or fixed-point to FP.
  void scvtf(const VRegister& fd, const VRegister& vn, int fbits = 0);

  // Convert unsigned integer or fixed-point to FP.
  void ucvtf(const VRegister& fd, const VRegister& vn, int fbits = 0);

  // Unsigned absolute difference.
  void uabd(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Signed absolute difference.
  void sabd(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Unsigned absolute difference and accumulate.
  void uaba(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Signed absolute difference and accumulate.
  void saba(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Add.
  void add(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Subtract.
  void sub(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Unsigned halving add.
  void uhadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed halving add.
  void shadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned rounding halving add.
  void urhadd(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed rounding halving add.
  void srhadd(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned halving sub.
  void uhsub(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed halving sub.
  void shsub(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned saturating add.
  void uqadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed saturating add.
  void sqadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned saturating subtract.
  void uqsub(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed saturating subtract.
  void sqsub(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Add pairwise.
  void addp(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Add pair of elements scalar.
  void addp(const VRegister& vd,
            const VRegister& vn);

  // Multiply-add to accumulator.
  void mla(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Multiply-subtract to accumulator.
  void mls(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Multiply.
  void mul(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Multiply by scalar element.
  void mul(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int vm_index);

  // Multiply-add by scalar element.
  void mla(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int vm_index);

  // Multiply-subtract by scalar element.
  void mls(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int vm_index);

  // Signed long multiply-add by scalar element.
  void smlal(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Signed long multiply-add by scalar element (second part).
  void smlal2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Unsigned long multiply-add by scalar element.
  void umlal(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Unsigned long multiply-add by scalar element (second part).
  void umlal2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Signed long multiply-sub by scalar element.
  void smlsl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Signed long multiply-sub by scalar element (second part).
  void smlsl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Unsigned long multiply-sub by scalar element.
  void umlsl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Unsigned long multiply-sub by scalar element (second part).
  void umlsl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Signed long multiply by scalar element.
  void smull(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Signed long multiply by scalar element (second part).
  void smull2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Unsigned long multiply by scalar element.
  void umull(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Unsigned long multiply by scalar element (second part).
  void umull2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Signed saturating double long multiply by element.
  void sqdmull(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm,
               int vm_index);

  // Signed saturating double long multiply by element (second part).
  void sqdmull2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Signed saturating doubling long multiply-add by element.
  void sqdmlal(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm,
               int vm_index);

  // Signed saturating doubling long multiply-add by element (second part).
  void sqdmlal2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Signed saturating doubling long multiply-sub by element.
  void sqdmlsl(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm,
               int vm_index);

  // Signed saturating doubling long multiply-sub by element (second part).
  void sqdmlsl2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Compare equal.
  void cmeq(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Compare signed greater than or equal.
  void cmge(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Compare signed greater than.
  void cmgt(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Compare unsigned higher.
  void cmhi(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Compare unsigned higher or same.
  void cmhs(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Compare bitwise test bits nonzero.
  void cmtst(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Compare bitwise to zero.
  void cmeq(const VRegister& vd,
            const VRegister& vn,
            int value);

  // Compare signed greater than or equal to zero.
  void cmge(const VRegister& vd,
            const VRegister& vn,
            int value);

  // Compare signed greater than zero.
  void cmgt(const VRegister& vd,
            const VRegister& vn,
            int value);

  // Compare signed less than or equal to zero.
  void cmle(const VRegister& vd,
            const VRegister& vn,
            int value);

  // Compare signed less than zero.
  void cmlt(const VRegister& vd,
            const VRegister& vn,
            int value);

  // Signed shift left by register.
  void sshl(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Unsigned shift left by register.
  void ushl(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Signed saturating shift left by register.
  void sqshl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned saturating shift left by register.
  void uqshl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed rounding shift left by register.
  void srshl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned rounding shift left by register.
  void urshl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed saturating rounding shift left by register.
  void sqrshl(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned saturating rounding shift left by register.
  void uqrshl(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Bitwise and.
  void and_(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Bitwise or.
  void orr(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Bitwise or immediate.
  void orr(const VRegister& vd,
           const int imm8,
           const int left_shift = 0);

  // Move register to register.
  void mov(const VRegister& vd,
           const VRegister& vn);

  // Bitwise orn.
  void orn(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Bitwise eor.
  void eor(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Bit clear immediate.
  void bic(const VRegister& vd,
           const int imm8,
           const int left_shift = 0);

  // Bit clear.
  void bic(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Bitwise insert if false.
  void bif(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Bitwise insert if true.
  void bit(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Bitwise select.
  void bsl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm);

  // Polynomial multiply.
  void pmul(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Vector move immediate.
  void movi(const VRegister& vd,
            const uint64_t imm,
            Shift shift = LSL,
            const int shift_amount = 0);

  // Bitwise not.
  void mvn(const VRegister& vd,
           const VRegister& vn);

  // Vector move inverted immediate.
  void mvni(const VRegister& vd,
            const int imm8,
            Shift shift = LSL,
            const int shift_amount = 0);

  // Signed saturating accumulate of unsigned value.
  void suqadd(const VRegister& vd,
              const VRegister& vn);

  // Unsigned saturating accumulate of signed value.
  void usqadd(const VRegister& vd,
              const VRegister& vn);

  // Absolute value.
  void abs(const VRegister& vd,
           const VRegister& vn);

  // Signed saturating absolute value.
  void sqabs(const VRegister& vd,
             const VRegister& vn);

  // Negate.
  void neg(const VRegister& vd,
           const VRegister& vn);

  // Signed saturating negate.
  void sqneg(const VRegister& vd,
             const VRegister& vn);

  // Bitwise not.
  void not_(const VRegister& vd,
            const VRegister& vn);

  // Extract narrow.
  void xtn(const VRegister& vd,
           const VRegister& vn);

  // Extract narrow (second part).
  void xtn2(const VRegister& vd,
            const VRegister& vn);

  // Signed saturating extract narrow.
  void sqxtn(const VRegister& vd,
             const VRegister& vn);

  // Signed saturating extract narrow (second part).
  void sqxtn2(const VRegister& vd,
              const VRegister& vn);

  // Unsigned saturating extract narrow.
  void uqxtn(const VRegister& vd,
             const VRegister& vn);

  // Unsigned saturating extract narrow (second part).
  void uqxtn2(const VRegister& vd,
              const VRegister& vn);

  // Signed saturating extract unsigned narrow.
  void sqxtun(const VRegister& vd,
              const VRegister& vn);

  // Signed saturating extract unsigned narrow (second part).
  void sqxtun2(const VRegister& vd,
               const VRegister& vn);

  // Extract vector from pair of vectors.
  void ext(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int index);

  // Duplicate vector element to vector or scalar.
  void dup(const VRegister& vd,
           const VRegister& vn,
           int vn_index);

  // Move vector element to scalar.
  void mov(const VRegister& vd,
           const VRegister& vn,
           int vn_index);

  // Duplicate general-purpose register to vector.
  void dup(const VRegister& vd,
           const Register& rn);

  // Insert vector element from another vector element.
  void ins(const VRegister& vd,
           int vd_index,
           const VRegister& vn,
           int vn_index);

  // Move vector element to another vector element.
  void mov(const VRegister& vd,
           int vd_index,
           const VRegister& vn,
           int vn_index);

  // Insert vector element from general-purpose register.
  void ins(const VRegister& vd,
           int vd_index,
           const Register& rn);

  // Move general-purpose register to a vector element.
  void mov(const VRegister& vd,
           int vd_index,
           const Register& rn);

  // Unsigned move vector element to general-purpose register.
  void umov(const Register& rd,
            const VRegister& vn,
            int vn_index);

  // Move vector element to general-purpose register.
  void mov(const Register& rd,
           const VRegister& vn,
           int vn_index);

  // Signed move vector element to general-purpose register.
  void smov(const Register& rd,
            const VRegister& vn,
            int vn_index);

  // One-element structure load to one register.
  void ld1(const VRegister& vt,
           const MemOperand& src);

  // One-element structure load to two registers.
  void ld1(const VRegister& vt,
           const VRegister& vt2,
           const MemOperand& src);

  // One-element structure load to three registers.
  void ld1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src);

  // One-element structure load to four registers.
  void ld1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src);

  // One-element single structure load to one lane.
  void ld1(const VRegister& vt,
           int lane,
           const MemOperand& src);

  // One-element single structure load to all lanes.
  void ld1r(const VRegister& vt,
            const MemOperand& src);

  // Two-element structure load.
  void ld2(const VRegister& vt,
           const VRegister& vt2,
           const MemOperand& src);

  // Two-element single structure load to one lane.
  void ld2(const VRegister& vt,
           const VRegister& vt2,
           int lane,
           const MemOperand& src);

  // Two-element single structure load to all lanes.
  void ld2r(const VRegister& vt,
            const VRegister& vt2,
            const MemOperand& src);

  // Three-element structure load.
  void ld3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src);

  // Three-element single structure load to one lane.
  void ld3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           int lane,
           const MemOperand& src);

  // Three-element single structure load to all lanes.
  void ld3r(const VRegister& vt,
            const VRegister& vt2,
            const VRegister& vt3,
            const MemOperand& src);

  // Four-element structure load.
  void ld4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src);

  // Four-element single structure load to one lane.
  void ld4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           int lane,
           const MemOperand& src);

  // Four-element single structure load to all lanes.
  void ld4r(const VRegister& vt,
            const VRegister& vt2,
            const VRegister& vt3,
            const VRegister& vt4,
            const MemOperand& src);

  // Count leading sign bits.
  void cls(const VRegister& vd,
           const VRegister& vn);

  // Count leading zero bits (vector).
  void clz(const VRegister& vd,
           const VRegister& vn);

  // Population count per byte.
  void cnt(const VRegister& vd,
           const VRegister& vn);

  // Reverse bit order.
  void rbit(const VRegister& vd,
            const VRegister& vn);

  // Reverse elements in 16-bit halfwords.
  void rev16(const VRegister& vd,
             const VRegister& vn);

  // Reverse elements in 32-bit words.
  void rev32(const VRegister& vd,
             const VRegister& vn);

  // Reverse elements in 64-bit doublewords.
  void rev64(const VRegister& vd,
             const VRegister& vn);

  // Unsigned reciprocal square root estimate.
  void ursqrte(const VRegister& vd,
               const VRegister& vn);

  // Unsigned reciprocal estimate.
  void urecpe(const VRegister& vd,
              const VRegister& vn);

  // Signed pairwise long add.
  void saddlp(const VRegister& vd,
              const VRegister& vn);

  // Unsigned pairwise long add.
  void uaddlp(const VRegister& vd,
              const VRegister& vn);

  // Signed pairwise long add and accumulate.
  void sadalp(const VRegister& vd,
              const VRegister& vn);

  // Unsigned pairwise long add and accumulate.
  void uadalp(const VRegister& vd,
              const VRegister& vn);

  // Shift left by immediate.
  void shl(const VRegister& vd,
           const VRegister& vn,
           int shift);

  // Signed saturating shift left by immediate.
  void sqshl(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Signed saturating shift left unsigned by immediate.
  void sqshlu(const VRegister& vd,
              const VRegister& vn,
              int shift);

  // Unsigned saturating shift left by immediate.
  void uqshl(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Signed shift left long by immediate.
  void sshll(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Signed shift left long by immediate (second part).
  void sshll2(const VRegister& vd,
              const VRegister& vn,
              int shift);

  // Signed extend long.
  void sxtl(const VRegister& vd,
            const VRegister& vn);

  // Signed extend long (second part).
  void sxtl2(const VRegister& vd,
             const VRegister& vn);

  // Unsigned shift left long by immediate.
  void ushll(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Unsigned shift left long by immediate (second part).
  void ushll2(const VRegister& vd,
              const VRegister& vn,
              int shift);

  // Shift left long by element size.
  void shll(const VRegister& vd,
            const VRegister& vn,
            int shift);

  // Shift left long by element size (second part).
  void shll2(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Unsigned extend long.
  void uxtl(const VRegister& vd,
            const VRegister& vn);

  // Unsigned extend long (second part).
  void uxtl2(const VRegister& vd,
             const VRegister& vn);

  // Shift left by immediate and insert.
  void sli(const VRegister& vd,
           const VRegister& vn,
           int shift);

  // Shift right by immediate and insert.
  void sri(const VRegister& vd,
           const VRegister& vn,
           int shift);

  // Signed maximum.
  void smax(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Signed pairwise maximum.
  void smaxp(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Add across vector.
  void addv(const VRegister& vd,
            const VRegister& vn);

  // Signed add long across vector.
  void saddlv(const VRegister& vd,
              const VRegister& vn);

  // Unsigned add long across vector.
  void uaddlv(const VRegister& vd,
              const VRegister& vn);

  // FP maximum number across vector.
  void fmaxnmv(const VRegister& vd,
               const VRegister& vn);

  // FP maximum across vector.
  void fmaxv(const VRegister& vd,
             const VRegister& vn);

  // FP minimum number across vector.
  void fminnmv(const VRegister& vd,
               const VRegister& vn);

  // FP minimum across vector.
  void fminv(const VRegister& vd,
             const VRegister& vn);

  // Signed maximum across vector.
  void smaxv(const VRegister& vd,
             const VRegister& vn);

  // Signed minimum.
  void smin(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Signed minimum pairwise.
  void sminp(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed minimum across vector.
  void sminv(const VRegister& vd,
             const VRegister& vn);

  // One-element structure store from one register.
  void st1(const VRegister& vt,
           const MemOperand& src);

  // One-element structure store from two registers.
  void st1(const VRegister& vt,
           const VRegister& vt2,
           const MemOperand& src);

  // One-element structure store from three registers.
  void st1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src);

  // One-element structure store from four registers.
  void st1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src);

  // One-element single structure store from one lane.
  void st1(const VRegister& vt,
           int lane,
           const MemOperand& src);

  // Two-element structure store from two registers.
  void st2(const VRegister& vt,
           const VRegister& vt2,
           const MemOperand& src);

  // Two-element single structure store from two lanes.
  void st2(const VRegister& vt,
           const VRegister& vt2,
           int lane,
           const MemOperand& src);

  // Three-element structure store from three registers.
  void st3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src);

  // Three-element single structure store from three lanes.
  void st3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           int lane,
           const MemOperand& src);

  // Four-element structure store from four registers.
  void st4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src);

  // Four-element single structure store from four lanes.
  void st4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           int lane,
           const MemOperand& src);

  // Unsigned add long.
  void uaddl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned add long (second part).
  void uaddl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned add wide.
  void uaddw(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned add wide (second part).
  void uaddw2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed add long.
  void saddl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed add long (second part).
  void saddl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed add wide.
  void saddw(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed add wide (second part).
  void saddw2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned subtract long.
  void usubl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned subtract long (second part).
  void usubl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned subtract wide.
  void usubw(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned subtract wide (second part).
  void usubw2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed subtract long.
  void ssubl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed subtract long (second part).
  void ssubl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed integer subtract wide.
  void ssubw(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed integer subtract wide (second part).
  void ssubw2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned maximum.
  void umax(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Unsigned pairwise maximum.
  void umaxp(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned maximum across vector.
  void umaxv(const VRegister& vd,
             const VRegister& vn);

  // Unsigned minimum.
  void umin(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Unsigned pairwise minimum.
  void uminp(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned minimum across vector.
  void uminv(const VRegister& vd,
             const VRegister& vn);

  // Transpose vectors (primary).
  void trn1(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Transpose vectors (secondary).
  void trn2(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Unzip vectors (primary).
  void uzp1(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Unzip vectors (secondary).
  void uzp2(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Zip vectors (primary).
  void zip1(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Zip vectors (secondary).
  void zip2(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // Signed shift right by immediate.
  void sshr(const VRegister& vd,
            const VRegister& vn,
            int shift);

  // Unsigned shift right by immediate.
  void ushr(const VRegister& vd,
            const VRegister& vn,
            int shift);

  // Signed rounding shift right by immediate.
  void srshr(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Unsigned rounding shift right by immediate.
  void urshr(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Signed shift right by immediate and accumulate.
  void ssra(const VRegister& vd,
            const VRegister& vn,
            int shift);

  // Unsigned shift right by immediate and accumulate.
  void usra(const VRegister& vd,
            const VRegister& vn,
            int shift);

  // Signed rounding shift right by immediate and accumulate.
  void srsra(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Unsigned rounding shift right by immediate and accumulate.
  void ursra(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Shift right narrow by immediate.
  void shrn(const VRegister& vd,
            const VRegister& vn,
            int shift);

  // Shift right narrow by immediate (second part).
  void shrn2(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Rounding shift right narrow by immediate.
  void rshrn(const VRegister& vd,
             const VRegister& vn,
             int shift);

  // Rounding shift right narrow by immediate (second part).
  void rshrn2(const VRegister& vd,
              const VRegister& vn,
              int shift);

  // Unsigned saturating shift right narrow by immediate.
  void uqshrn(const VRegister& vd,
              const VRegister& vn,
              int shift);

  // Unsigned saturating shift right narrow by immediate (second part).
  void uqshrn2(const VRegister& vd,
               const VRegister& vn,
               int shift);

  // Unsigned saturating rounding shift right narrow by immediate.
  void uqrshrn(const VRegister& vd,
               const VRegister& vn,
               int shift);

  // Unsigned saturating rounding shift right narrow by immediate (second part).
  void uqrshrn2(const VRegister& vd,
                const VRegister& vn,
                int shift);

  // Signed saturating shift right narrow by immediate.
  void sqshrn(const VRegister& vd,
              const VRegister& vn,
              int shift);

  // Signed saturating shift right narrow by immediate (second part).
  void sqshrn2(const VRegister& vd,
               const VRegister& vn,
               int shift);

  // Signed saturating rounded shift right narrow by immediate.
  void sqrshrn(const VRegister& vd,
               const VRegister& vn,
               int shift);

  // Signed saturating rounded shift right narrow by immediate (second part).
  void sqrshrn2(const VRegister& vd,
                const VRegister& vn,
                int shift);

  // Signed saturating shift right unsigned narrow by immediate.
  void sqshrun(const VRegister& vd,
               const VRegister& vn,
               int shift);

  // Signed saturating shift right unsigned narrow by immediate (second part).
  void sqshrun2(const VRegister& vd,
                const VRegister& vn,
                int shift);

  // Signed sat rounded shift right unsigned narrow by immediate.
  void sqrshrun(const VRegister& vd,
                const VRegister& vn,
                int shift);

  // Signed sat rounded shift right unsigned narrow by immediate (second part).
  void sqrshrun2(const VRegister& vd,
                 const VRegister& vn,
                 int shift);

  // FP reciprocal step.
  void frecps(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // FP reciprocal estimate.
  void frecpe(const VRegister& vd,
              const VRegister& vn);

  // FP reciprocal square root estimate.
  void frsqrte(const VRegister& vd,
               const VRegister& vn);

  // FP reciprocal square root step.
  void frsqrts(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // Signed absolute difference and accumulate long.
  void sabal(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed absolute difference and accumulate long (second part).
  void sabal2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned absolute difference and accumulate long.
  void uabal(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned absolute difference and accumulate long (second part).
  void uabal2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed absolute difference long.
  void sabdl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed absolute difference long (second part).
  void sabdl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned absolute difference long.
  void uabdl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned absolute difference long (second part).
  void uabdl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Polynomial multiply long.
  void pmull(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Polynomial multiply long (second part).
  void pmull2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed long multiply-add.
  void smlal(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed long multiply-add (second part).
  void smlal2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned long multiply-add.
  void umlal(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned long multiply-add (second part).
  void umlal2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed long multiply-sub.
  void smlsl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed long multiply-sub (second part).
  void smlsl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Unsigned long multiply-sub.
  void umlsl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned long multiply-sub (second part).
  void umlsl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed long multiply.
  void smull(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Signed long multiply (second part).
  void smull2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Signed saturating doubling long multiply-add.
  void sqdmlal(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // Signed saturating doubling long multiply-add (second part).
  void sqdmlal2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm);

  // Signed saturating doubling long multiply-subtract.
  void sqdmlsl(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // Signed saturating doubling long multiply-subtract (second part).
  void sqdmlsl2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm);

  // Signed saturating doubling long multiply.
  void sqdmull(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // Signed saturating doubling long multiply (second part).
  void sqdmull2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm);

  // Signed saturating doubling multiply returning high half.
  void sqdmulh(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // Signed saturating rounding doubling multiply returning high half.
  void sqrdmulh(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm);

  // Signed saturating doubling multiply element returning high half.
  void sqdmulh(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm,
               int vm_index);

  // Signed saturating rounding doubling multiply element returning high half.
  void sqrdmulh(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Unsigned long multiply long.
  void umull(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Unsigned long multiply (second part).
  void umull2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Add narrow returning high half.
  void addhn(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Add narrow returning high half (second part).
  void addhn2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Rounding add narrow returning high half.
  void raddhn(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Rounding add narrow returning high half (second part).
  void raddhn2(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // Subtract narrow returning high half.
  void subhn(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // Subtract narrow returning high half (second part).
  void subhn2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Rounding subtract narrow returning high half.
  void rsubhn(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm);

  // Rounding subtract narrow returning high half (second part).
  void rsubhn2(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // FP vector multiply accumulate.
  void fmla(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // FP vector multiply subtract.
  void fmls(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // FP vector multiply extended.
  void fmulx(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP absolute greater than or equal.
  void facge(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP absolute greater than.
  void facgt(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP multiply by element.
  void fmul(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm,
            int vm_index);

  // FP fused multiply-add to accumulator by element.
  void fmla(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm,
            int vm_index);

  // FP fused multiply-sub from accumulator by element.
  void fmls(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm,
            int vm_index);

  // FP multiply extended by element.
  void fmulx(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // FP compare equal.
  void fcmeq(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP greater than.
  void fcmgt(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP greater than or equal.
  void fcmge(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP compare equal to zero.
  void fcmeq(const VRegister& vd,
             const VRegister& vn,
             double imm);

  // FP greater than zero.
  void fcmgt(const VRegister& vd,
             const VRegister& vn,
             double imm);

  // FP greater than or equal to zero.
  void fcmge(const VRegister& vd,
             const VRegister& vn,
             double imm);

  // FP less than or equal to zero.
  void fcmle(const VRegister& vd,
             const VRegister& vn,
             double imm);

  // FP less than to zero.
  void fcmlt(const VRegister& vd,
             const VRegister& vn,
             double imm);

  // FP absolute difference.
  void fabd(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm);

  // FP pairwise add vector.
  void faddp(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP pairwise add scalar.
  void faddp(const VRegister& vd,
             const VRegister& vn);

  // FP pairwise maximum vector.
  void fmaxp(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP pairwise maximum scalar.
  void fmaxp(const VRegister& vd,
             const VRegister& vn);

  // FP pairwise minimum vector.
  void fminp(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm);

  // FP pairwise minimum scalar.
  void fminp(const VRegister& vd,
             const VRegister& vn);

  // FP pairwise maximum number vector.
  void fmaxnmp(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // FP pairwise maximum number scalar.
  void fmaxnmp(const VRegister& vd,
               const VRegister& vn);

  // FP pairwise minimum number vector.
  void fminnmp(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm);

  // FP pairwise minimum number scalar.
  void fminnmp(const VRegister& vd,
               const VRegister& vn);

  // Emit generic instructions.
  // Emit raw instructions into the instruction stream.
  void dci(Instr raw_inst) { Emit(raw_inst); }

  // Emit 32 bits of data into the instruction stream.
  void dc32(uint32_t data) {
    EmitData(&data, sizeof(data));
  }

  // Emit 64 bits of data into the instruction stream.
  void dc64(uint64_t data) {
    EmitData(&data, sizeof(data));
  }

  // Code generation helpers.

  // Register encoding.
  static Instr Rd(CPURegister rd) {
    VIXL_ASSERT(rd.code() != kSPRegInternalCode);
    return rd.code() << Rd_offset;
  }

  static Instr Rn(CPURegister rn) {
    VIXL_ASSERT(rn.code() != kSPRegInternalCode);
    return rn.code() << Rn_offset;
  }

  static Instr Rm(CPURegister rm) {
    VIXL_ASSERT(rm.code() != kSPRegInternalCode);
    return rm.code() << Rm_offset;
  }

  static Instr RmNot31(CPURegister rm) {
    VIXL_ASSERT(rm.code() != kSPRegInternalCode);
    VIXL_ASSERT(!rm.IsZero());
    return Rm(rm);
  }

  static Instr Ra(CPURegister ra) {
    VIXL_ASSERT(ra.code() != kSPRegInternalCode);
    return ra.code() << Ra_offset;
  }

  static Instr Rt(CPURegister rt) {
    VIXL_ASSERT(rt.code() != kSPRegInternalCode);
    return rt.code() << Rt_offset;
  }

  static Instr Rt2(CPURegister rt2) {
    VIXL_ASSERT(rt2.code() != kSPRegInternalCode);
    return rt2.code() << Rt2_offset;
  }

  static Instr Rs(CPURegister rs) {
    VIXL_ASSERT(rs.code() != kSPRegInternalCode);
    return rs.code() << Rs_offset;
  }

  // These encoding functions allow the stack pointer to be encoded, and
  // disallow the zero register.
  static Instr RdSP(Register rd) {
    VIXL_ASSERT(!rd.IsZero());
    return (rd.code() & kRegCodeMask) << Rd_offset;
  }

  static Instr RnSP(Register rn) {
    VIXL_ASSERT(!rn.IsZero());
    return (rn.code() & kRegCodeMask) << Rn_offset;
  }

  // Flags encoding.
  static Instr Flags(FlagsUpdate S) {
    if (S == SetFlags) {
      return 1 << FlagsUpdate_offset;
    } else if (S == LeaveFlags) {
      return 0 << FlagsUpdate_offset;
    }
    VIXL_UNREACHABLE();
    return 0;
  }

  static Instr Cond(Condition cond) {
    return cond << Condition_offset;
  }

  // PC-relative address encoding.
  static Instr ImmPCRelAddress(int imm21) {
    VIXL_ASSERT(is_int21(imm21));
    Instr imm = static_cast<Instr>(truncate_to_int21(imm21));
    Instr immhi = (imm >> ImmPCRelLo_width) << ImmPCRelHi_offset;
    Instr immlo = imm << ImmPCRelLo_offset;
    return (immhi & ImmPCRelHi_mask) | (immlo & ImmPCRelLo_mask);
  }

  // Branch encoding.
  static Instr ImmUncondBranch(int imm26) {
    VIXL_ASSERT(is_int26(imm26));
    return truncate_to_int26(imm26) << ImmUncondBranch_offset;
  }

  static Instr ImmCondBranch(int imm19) {
    VIXL_ASSERT(is_int19(imm19));
    return truncate_to_int19(imm19) << ImmCondBranch_offset;
  }

  static Instr ImmCmpBranch(int imm19) {
    VIXL_ASSERT(is_int19(imm19));
    return truncate_to_int19(imm19) << ImmCmpBranch_offset;
  }

  static Instr ImmTestBranch(int imm14) {
    VIXL_ASSERT(is_int14(imm14));
    return truncate_to_int14(imm14) << ImmTestBranch_offset;
  }

  static Instr ImmTestBranchBit(unsigned bit_pos) {
    VIXL_ASSERT(is_uint6(bit_pos));
    // Subtract five from the shift offset, as we need bit 5 from bit_pos.
    unsigned b5 = bit_pos << (ImmTestBranchBit5_offset - 5);
    unsigned b40 = bit_pos << ImmTestBranchBit40_offset;
    b5 &= ImmTestBranchBit5_mask;
    b40 &= ImmTestBranchBit40_mask;
    return b5 | b40;
  }

  // Data Processing encoding.
  static Instr SF(Register rd) {
      return rd.Is64Bits() ? SixtyFourBits : ThirtyTwoBits;
  }

  static Instr ImmAddSub(int imm) {
    VIXL_ASSERT(IsImmAddSub(imm));
    if (is_uint12(imm)) {  // No shift required.
      imm <<= ImmAddSub_offset;
    } else {
      imm = ((imm >> 12) << ImmAddSub_offset) | (1 << ShiftAddSub_offset);
    }
    return imm;
  }

  static Instr ImmS(unsigned imms, unsigned reg_size) {
    VIXL_ASSERT(((reg_size == kXRegSize) && is_uint6(imms)) ||
           ((reg_size == kWRegSize) && is_uint5(imms)));
    USE(reg_size);
    return imms << ImmS_offset;
  }

  static Instr ImmR(unsigned immr, unsigned reg_size) {
    VIXL_ASSERT(((reg_size == kXRegSize) && is_uint6(immr)) ||
           ((reg_size == kWRegSize) && is_uint5(immr)));
    USE(reg_size);
    VIXL_ASSERT(is_uint6(immr));
    return immr << ImmR_offset;
  }

  static Instr ImmSetBits(unsigned imms, unsigned reg_size) {
    VIXL_ASSERT((reg_size == kWRegSize) || (reg_size == kXRegSize));
    VIXL_ASSERT(is_uint6(imms));
    VIXL_ASSERT((reg_size == kXRegSize) || is_uint6(imms + 3));
    USE(reg_size);
    return imms << ImmSetBits_offset;
  }

  static Instr ImmRotate(unsigned immr, unsigned reg_size) {
    VIXL_ASSERT((reg_size == kWRegSize) || (reg_size == kXRegSize));
    VIXL_ASSERT(((reg_size == kXRegSize) && is_uint6(immr)) ||
           ((reg_size == kWRegSize) && is_uint5(immr)));
    USE(reg_size);
    return immr << ImmRotate_offset;
  }

  static Instr ImmLLiteral(int imm19) {
    VIXL_ASSERT(is_int19(imm19));
    return truncate_to_int19(imm19) << ImmLLiteral_offset;
  }

  static Instr BitN(unsigned bitn, unsigned reg_size) {
    VIXL_ASSERT((reg_size == kWRegSize) || (reg_size == kXRegSize));
    VIXL_ASSERT((reg_size == kXRegSize) || (bitn == 0));
    USE(reg_size);
    return bitn << BitN_offset;
  }

  static Instr ShiftDP(Shift shift) {
    VIXL_ASSERT(shift == LSL || shift == LSR || shift == ASR || shift == ROR);
    return shift << ShiftDP_offset;
  }

  static Instr ImmDPShift(unsigned amount) {
    VIXL_ASSERT(is_uint6(amount));
    return amount << ImmDPShift_offset;
  }

  static Instr ExtendMode(Extend extend) {
    return extend << ExtendMode_offset;
  }

  static Instr ImmExtendShift(unsigned left_shift) {
    VIXL_ASSERT(left_shift <= 4);
    return left_shift << ImmExtendShift_offset;
  }

  static Instr ImmCondCmp(unsigned imm) {
    VIXL_ASSERT(is_uint5(imm));
    return imm << ImmCondCmp_offset;
  }

  static Instr Nzcv(StatusFlags nzcv) {
    return ((nzcv >> Flags_offset) & 0xf) << Nzcv_offset;
  }

  // MemOperand offset encoding.
  static Instr ImmLSUnsigned(int imm12) {
    VIXL_ASSERT(is_uint12(imm12));
    return imm12 << ImmLSUnsigned_offset;
  }

  static Instr ImmLS(int imm9) {
    VIXL_ASSERT(is_int9(imm9));
    return truncate_to_int9(imm9) << ImmLS_offset;
  }

  static Instr ImmLSPair(int imm7, unsigned access_size) {
    VIXL_ASSERT(((imm7 >> access_size) << access_size) == imm7);
    int scaled_imm7 = imm7 >> access_size;
    VIXL_ASSERT(is_int7(scaled_imm7));
    return truncate_to_int7(scaled_imm7) << ImmLSPair_offset;
  }

  static Instr ImmShiftLS(unsigned shift_amount) {
    VIXL_ASSERT(is_uint1(shift_amount));
    return shift_amount << ImmShiftLS_offset;
  }

  static Instr ImmPrefetchOperation(int imm5) {
    VIXL_ASSERT(is_uint5(imm5));
    return imm5 << ImmPrefetchOperation_offset;
  }

  static Instr ImmException(int imm16) {
    VIXL_ASSERT(is_uint16(imm16));
    return imm16 << ImmException_offset;
  }

  static Instr ImmSystemRegister(int imm15) {
    VIXL_ASSERT(is_uint15(imm15));
    return imm15 << ImmSystemRegister_offset;
  }

  static Instr ImmHint(int imm7) {
    VIXL_ASSERT(is_uint7(imm7));
    return imm7 << ImmHint_offset;
  }

  static Instr CRm(int imm4) {
    VIXL_ASSERT(is_uint4(imm4));
    return imm4 << CRm_offset;
  }

  static Instr CRn(int imm4) {
    VIXL_ASSERT(is_uint4(imm4));
    return imm4 << CRn_offset;
  }

  static Instr SysOp(int imm14) {
    VIXL_ASSERT(is_uint14(imm14));
    return imm14 << SysOp_offset;
  }

  static Instr ImmSysOp1(int imm3) {
    VIXL_ASSERT(is_uint3(imm3));
    return imm3 << SysOp1_offset;
  }

  static Instr ImmSysOp2(int imm3) {
    VIXL_ASSERT(is_uint3(imm3));
    return imm3 << SysOp2_offset;
  }

  static Instr ImmBarrierDomain(int imm2) {
    VIXL_ASSERT(is_uint2(imm2));
    return imm2 << ImmBarrierDomain_offset;
  }

  static Instr ImmBarrierType(int imm2) {
    VIXL_ASSERT(is_uint2(imm2));
    return imm2 << ImmBarrierType_offset;
  }

  // Move immediates encoding.
  static Instr ImmMoveWide(uint64_t imm) {
    VIXL_ASSERT(is_uint16(imm));
    return static_cast<Instr>(imm << ImmMoveWide_offset);
  }

  static Instr ShiftMoveWide(int64_t shift) {
    VIXL_ASSERT(is_uint2(shift));
    return static_cast<Instr>(shift << ShiftMoveWide_offset);
  }

  // FP Immediates.
  static Instr ImmFP32(float imm);
  static Instr ImmFP64(double imm);

  // FP register type.
  static Instr FPType(FPRegister fd) {
    return fd.Is64Bits() ? FP64 : FP32;
  }

  static Instr FPScale(unsigned scale) {
    VIXL_ASSERT(is_uint6(scale));
    return scale << FPScale_offset;
  }

  // Immediate field checking helpers.
  static bool IsImmAddSub(int64_t immediate);
  static bool IsImmConditionalCompare(int64_t immediate);
  static bool IsImmFP32(float imm);
  static bool IsImmFP64(double imm);
  static bool IsImmLogical(uint64_t value,
                           unsigned width,
                           unsigned* n = NULL,
                           unsigned* imm_s = NULL,
                           unsigned* imm_r = NULL);
  static bool IsImmLSPair(int64_t offset, unsigned access_size);
  static bool IsImmLSScaled(int64_t offset, unsigned access_size);
  static bool IsImmLSUnscaled(int64_t offset);
  static bool IsImmMovn(uint64_t imm, unsigned reg_size);
  static bool IsImmMovz(uint64_t imm, unsigned reg_size);

  // Instruction bits for vector format in data processing operations.
  static Instr VFormat(VRegister vd) {
    if (vd.Is64Bits()) {
      switch (vd.lanes()) {
        case 2: return NEON_2S;
        case 4: return NEON_4H;
        case 8: return NEON_8B;
        default: return 0xffffffff;
      }
    } else {
      VIXL_ASSERT(vd.Is128Bits());
      switch (vd.lanes()) {
        case 2: return NEON_2D;
        case 4: return NEON_4S;
        case 8: return NEON_8H;
        case 16: return NEON_16B;
        default: return 0xffffffff;
      }
    }
  }

  // Instruction bits for vector format in floating point data processing
  // operations.
  static Instr FPFormat(VRegister vd) {
    if (vd.lanes() == 1) {
      // Floating point scalar formats.
      VIXL_ASSERT(vd.Is32Bits() || vd.Is64Bits());
      return vd.Is64Bits() ? FP64 : FP32;
    }

    // Two lane floating point vector formats.
    if (vd.lanes() == 2) {
      VIXL_ASSERT(vd.Is64Bits() || vd.Is128Bits());
      return vd.Is128Bits() ? NEON_FP_2D : NEON_FP_2S;
    }

    // Four lane floating point vector format.
    VIXL_ASSERT((vd.lanes() == 4) && vd.Is128Bits());
    return NEON_FP_4S;
  }

  // Instruction bits for vector format in load and store operations.
  static Instr LSVFormat(VRegister vd) {
    if (vd.Is64Bits()) {
      switch (vd.lanes()) {
        case 1: return LS_NEON_1D;
        case 2: return LS_NEON_2S;
        case 4: return LS_NEON_4H;
        case 8: return LS_NEON_8B;
        default: return 0xffffffff;
      }
    } else {
      VIXL_ASSERT(vd.Is128Bits());
      switch (vd.lanes()) {
        case 2: return LS_NEON_2D;
        case 4: return LS_NEON_4S;
        case 8: return LS_NEON_8H;
        case 16: return LS_NEON_16B;
        default: return 0xffffffff;
      }
    }
  }

  // Instruction bits for scalar format in data processing operations.
  static Instr SFormat(VRegister vd) {
    VIXL_ASSERT(vd.lanes() == 1);
    switch (vd.SizeInBytes()) {
      case 1: return NEON_B;
      case 2: return NEON_H;
      case 4: return NEON_S;
      case 8: return NEON_D;
      default: return 0xffffffff;
    }
  }

  static Instr ImmNEONHLM(int index, int num_bits) {
    int h, l, m;
    if (num_bits == 3) {
      VIXL_ASSERT(is_uint3(index));
      h  = (index >> 2) & 1;
      l  = (index >> 1) & 1;
      m  = (index >> 0) & 1;
    } else if (num_bits == 2) {
      VIXL_ASSERT(is_uint2(index));
      h  = (index >> 1) & 1;
      l  = (index >> 0) & 1;
      m  = 0;
    } else {
      VIXL_ASSERT(is_uint1(index) && (num_bits == 1));
      h  = (index >> 0) & 1;
      l  = 0;
      m  = 0;
    }
    return (h << NEONH_offset) | (l << NEONL_offset) | (m << NEONM_offset);
  }

  static Instr ImmNEONExt(int imm4) {
    VIXL_ASSERT(is_uint4(imm4));
    return imm4 << ImmNEONExt_offset;
  }

  static Instr ImmNEON5(Instr format, int index) {
    VIXL_ASSERT(is_uint4(index));
    int s = LaneSizeInBytesLog2FromFormat(static_cast<VectorFormat>(format));
    int imm5 = (index << (s + 1)) | (1 << s);
    return imm5 << ImmNEON5_offset;
  }

  static Instr ImmNEON4(Instr format, int index) {
    VIXL_ASSERT(is_uint4(index));
    int s = LaneSizeInBytesLog2FromFormat(static_cast<VectorFormat>(format));
    int imm4 = index << s;
    return imm4 << ImmNEON4_offset;
  }

  static Instr ImmNEONabcdefgh(int imm8) {
    VIXL_ASSERT(is_uint8(imm8));
    Instr instr;
    instr  = ((imm8 >> 5) & 7) << ImmNEONabc_offset;
    instr |= (imm8 & 0x1f) << ImmNEONdefgh_offset;
    return instr;
  }

  static Instr NEONCmode(int cmode) {
    VIXL_ASSERT(is_uint4(cmode));
    return cmode << NEONCmode_offset;
  }

  static Instr NEONModImmOp(int op) {
    VIXL_ASSERT(is_uint1(op));
    return op << NEONModImmOp_offset;
  }

  size_t size() const {
    return SizeOfCodeGenerated();
  }

  size_t SizeOfCodeGenerated() const {
    return armbuffer_.size();
  }

  PositionIndependentCodeOption pic() const {
    return pic_;
  }

  bool AllowPageOffsetDependentCode() const {
    return (pic() == PageOffsetDependentCode) ||
           (pic() == PositionDependentCode);
  }

  static const Register& AppropriateZeroRegFor(const CPURegister& reg) {
    return reg.Is64Bits() ? xzr : wzr;
  }


 protected:
  void LoadStore(const CPURegister& rt,
                 const MemOperand& addr,
                 LoadStoreOp op,
                 LoadStoreScalingOption option = PreferScaledOffset);

  void LoadStorePair(const CPURegister& rt,
                     const CPURegister& rt2,
                     const MemOperand& addr,
                     LoadStorePairOp op);
  void LoadStoreStruct(const VRegister& vt,
                       const MemOperand& addr,
                       NEONLoadStoreMultiStructOp op);
  void LoadStoreStruct1(const VRegister& vt,
                        int reg_count,
                        const MemOperand& addr);
  void LoadStoreStructSingle(const VRegister& vt,
                             uint32_t lane,
                             const MemOperand& addr,
                             NEONLoadStoreSingleStructOp op);
  void LoadStoreStructSingleAllLanes(const VRegister& vt,
                                     const MemOperand& addr,
                                     NEONLoadStoreSingleStructOp op);
  void LoadStoreStructVerify(const VRegister& vt,
                             const MemOperand& addr,
                             Instr op);

  void Prefetch(PrefetchOperation op,
                const MemOperand& addr,
                LoadStoreScalingOption option = PreferScaledOffset);

  // TODO(all): The third parameter should be passed by reference but gcc 4.8.2
  // reports a bogus uninitialised warning then.
  BufferOffset Logical(const Register& rd,
                       const Register& rn,
                       const Operand operand,
                       LogicalOp op);
  BufferOffset LogicalImmediate(const Register& rd,
                                const Register& rn,
                                unsigned n,
                                unsigned imm_s,
                                unsigned imm_r,
                                LogicalOp op);

  void ConditionalCompare(const Register& rn,
                          const Operand& operand,
                          StatusFlags nzcv,
                          Condition cond,
                          ConditionalCompareOp op);

  void AddSubWithCarry(const Register& rd,
                       const Register& rn,
                       const Operand& operand,
                       FlagsUpdate S,
                       AddSubWithCarryOp op);


  // Functions for emulating operands not directly supported by the instruction
  // set.
  void EmitShift(const Register& rd,
                 const Register& rn,
                 Shift shift,
                 unsigned amount);
  void EmitExtendShift(const Register& rd,
                       const Register& rn,
                       Extend extend,
                       unsigned left_shift);

  void AddSub(const Register& rd,
              const Register& rn,
              const Operand& operand,
              FlagsUpdate S,
              AddSubOp op);

  void NEONTable(const VRegister& vd,
                 const VRegister& vn,
                 const VRegister& vm,
                 NEONTableOp op);

  // Find an appropriate LoadStoreOp or LoadStorePairOp for the specified
  // registers. Only simple loads are supported; sign- and zero-extension (such
  // as in LDPSW_x or LDRB_w) are not supported.
  static LoadStoreOp LoadOpFor(const CPURegister& rt);
  static LoadStorePairOp LoadPairOpFor(const CPURegister& rt,
                                       const CPURegister& rt2);
  static LoadStoreOp StoreOpFor(const CPURegister& rt);
  static LoadStorePairOp StorePairOpFor(const CPURegister& rt,
                                        const CPURegister& rt2);
  static LoadStorePairNonTemporalOp LoadPairNonTemporalOpFor(
    const CPURegister& rt, const CPURegister& rt2);
  static LoadStorePairNonTemporalOp StorePairNonTemporalOpFor(
    const CPURegister& rt, const CPURegister& rt2);
  static LoadLiteralOp LoadLiteralOpFor(const CPURegister& rt);


 private:
  static uint32_t FP32ToImm8(float imm);
  static uint32_t FP64ToImm8(double imm);

  // Instruction helpers.
  void MoveWide(const Register& rd,
                uint64_t imm,
                int shift,
                MoveWideImmediateOp mov_op);
  BufferOffset DataProcShiftedRegister(const Register& rd,
                                       const Register& rn,
                                       const Operand& operand,
                                       FlagsUpdate S,
                                       Instr op);
  void DataProcExtendedRegister(const Register& rd,
                                const Register& rn,
                                const Operand& operand,
                                FlagsUpdate S,
                                Instr op);
  void LoadStorePairNonTemporal(const CPURegister& rt,
                                const CPURegister& rt2,
                                const MemOperand& addr,
                                LoadStorePairNonTemporalOp op);
  void LoadLiteral(const CPURegister& rt, uint64_t imm, LoadLiteralOp op);
  void ConditionalSelect(const Register& rd,
                         const Register& rn,
                         const Register& rm,
                         Condition cond,
                         ConditionalSelectOp op);
  void DataProcessing1Source(const Register& rd,
                             const Register& rn,
                             DataProcessing1SourceOp op);
  void DataProcessing3Source(const Register& rd,
                             const Register& rn,
                             const Register& rm,
                             const Register& ra,
                             DataProcessing3SourceOp op);
  void FPDataProcessing1Source(const VRegister& fd,
                               const VRegister& fn,
                               FPDataProcessing1SourceOp op);
  void FPDataProcessing3Source(const VRegister& fd,
                               const VRegister& fn,
                               const VRegister& fm,
                               const VRegister& fa,
                               FPDataProcessing3SourceOp op);
  void NEONAcrossLanesL(const VRegister& vd,
                        const VRegister& vn,
                        NEONAcrossLanesOp op);
  void NEONAcrossLanes(const VRegister& vd,
                       const VRegister& vn,
                       NEONAcrossLanesOp op);
  void NEONModifiedImmShiftLsl(const VRegister& vd,
                               const int imm8,
                               const int left_shift,
                               NEONModifiedImmediateOp op);
  void NEONModifiedImmShiftMsl(const VRegister& vd,
                               const int imm8,
                               const int shift_amount,
                               NEONModifiedImmediateOp op);
  void NEONFP2Same(const VRegister& vd,
                   const VRegister& vn,
                   Instr vop);
  void NEON3Same(const VRegister& vd,
                 const VRegister& vn,
                 const VRegister& vm,
                 NEON3SameOp vop);
  void NEONFP3Same(const VRegister& vd,
                   const VRegister& vn,
                   const VRegister& vm,
                   Instr op);
  void NEON3DifferentL(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm,
                       NEON3DifferentOp vop);
  void NEON3DifferentW(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm,
                       NEON3DifferentOp vop);
  void NEON3DifferentHN(const VRegister& vd,
                        const VRegister& vn,
                        const VRegister& vm,
                        NEON3DifferentOp vop);
  void NEONFP2RegMisc(const VRegister& vd,
                      const VRegister& vn,
                      NEON2RegMiscOp vop,
                      double value = 0.0);
  void NEON2RegMisc(const VRegister& vd,
                    const VRegister& vn,
                    NEON2RegMiscOp vop,
                    int value = 0);
  void NEONFP2RegMisc(const VRegister& vd,
                      const VRegister& vn,
                      Instr op);
  void NEONAddlp(const VRegister& vd,
                 const VRegister& vn,
                 NEON2RegMiscOp op);
  void NEONPerm(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                NEONPermOp op);
  void NEONFPByElement(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm,
                       int vm_index,
                       NEONByIndexedElementOp op);
  void NEONByElement(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm,
                     int vm_index,
                     NEONByIndexedElementOp op);
  void NEONByElementL(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm,
                      int vm_index,
                      NEONByIndexedElementOp op);
  void NEONShiftImmediate(const VRegister& vd,
                          const VRegister& vn,
                          NEONShiftImmediateOp op,
                          int immh_immb);
  void NEONShiftLeftImmediate(const VRegister& vd,
                              const VRegister& vn,
                              int shift,
                              NEONShiftImmediateOp op);
  void NEONShiftRightImmediate(const VRegister& vd,
                               const VRegister& vn,
                               int shift,
                               NEONShiftImmediateOp op);
  void NEONShiftImmediateL(const VRegister& vd,
                           const VRegister& vn,
                           int shift,
                           NEONShiftImmediateOp op);
  void NEONShiftImmediateN(const VRegister& vd,
                           const VRegister& vn,
                           int shift,
                           NEONShiftImmediateOp op);
  void NEONXtn(const VRegister& vd,
               const VRegister& vn,
               NEON2RegMiscOp vop);

  Instr LoadStoreStructAddrModeField(const MemOperand& addr);

  // Encode the specified MemOperand for the specified access size and scaling
  // preference.
  Instr LoadStoreMemOperand(const MemOperand& addr,
                            unsigned access_size,
                            LoadStoreScalingOption option);

 protected:
  // Prevent generation of a literal pool for the next |maxInst| instructions.
  // Guarantees instruction linearity.
  class AutoBlockLiteralPool {
    ARMBuffer* armbuffer_;

   public:
    AutoBlockLiteralPool(Assembler* assembler, size_t maxInst)
      : armbuffer_(&assembler->armbuffer_) {
      armbuffer_->enterNoPool(maxInst);
    }
    ~AutoBlockLiteralPool() {
      armbuffer_->leaveNoPool();
    }
  };

 protected:
  // Buffer where the code is emitted.
  PositionIndependentCodeOption pic_;

#ifdef DEBUG
  bool finalized_;
#endif
};

}  // namespace vixl

#endif  // VIXL_A64_ASSEMBLER_A64_H_
