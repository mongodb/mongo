/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Disassembler_h
#define jit_Disassembler_h

#include "jit/MacroAssembler.h"
#include "jit/Registers.h"

namespace js {
namespace jit {

namespace Disassembler {

class ComplexAddress {
    int32_t disp_;
    Register::Encoding base_ : 8;
    Register::Encoding index_ : 8;
    int8_t scale_; // log2 encoding
    bool isPCRelative_;

  public:
    ComplexAddress()
      : disp_(0),
        base_(Registers::Invalid),
        index_(Registers::Invalid),
        scale_(0),
        isPCRelative_(false)
    {
        MOZ_ASSERT(*this == *this);
    }

    ComplexAddress(int32_t disp, Register::Encoding base)
      : disp_(disp),
        base_(base),
        index_(Registers::Invalid),
        scale_(0),
        isPCRelative_(false)
    {
        MOZ_ASSERT(*this == *this);
        MOZ_ASSERT(base != Registers::Invalid);
        MOZ_ASSERT(base_ == base);
    }

    ComplexAddress(int32_t disp, Register::Encoding base, Register::Encoding index, int scale)
      : disp_(disp),
        base_(base),
        index_(index),
        scale_(scale),
        isPCRelative_(false)
    {
        MOZ_ASSERT(scale >= 0 && scale < 4);
        MOZ_ASSERT_IF(index == Registers::Invalid, scale == 0);
        MOZ_ASSERT(*this == *this);
        MOZ_ASSERT(base_ == base);
        MOZ_ASSERT(index_ == index);
    }

    explicit ComplexAddress(const void* addr)
      : disp_(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr))),
        base_(Registers::Invalid),
        index_(Registers::Invalid),
        scale_(0),
        isPCRelative_(false)
    {
        MOZ_ASSERT(*this == *this);
        MOZ_ASSERT(reinterpret_cast<const void*>(uintptr_t(disp_)) == addr);
    }

    explicit ComplexAddress(const Operand& op) {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        switch (op.kind()) {
          case Operand::MEM_REG_DISP:
            *this = ComplexAddress(op.disp(), op.base());
            return;
          case Operand::MEM_SCALE:
            *this = ComplexAddress(op.disp(), op.base(), op.index(), op.scale());
            return;
          case Operand::MEM_ADDRESS32:
            *this = ComplexAddress(op.address());
            return;
          default:
            break;
        }
#endif
        MOZ_CRASH("Unexpected Operand kind");
    }

    bool isPCRelative() const {
        return isPCRelative_;
    }

    int32_t disp() const {
        return disp_;
    }

    bool hasBase() const {
        return base_ != Registers::Invalid;
    }

    Register::Encoding base() const {
        MOZ_ASSERT(hasBase());
        return base_;
    }

    bool hasIndex() const {
        return index_ != Registers::Invalid;
    }

    Register::Encoding index() const {
        MOZ_ASSERT(hasIndex());
        return index_;
    }

    uint32_t scale() const {
        return scale_;
    }

#ifdef DEBUG
    bool operator==(const ComplexAddress& other) const;
    bool operator!=(const ComplexAddress& other) const;
#endif
};

// An operand other than a memory operand -- a register or an immediate.
class OtherOperand {
  public:
    enum Kind {
        Imm,
        GPR,
        FPR,
    };

  private:
    Kind kind_;
    union {
        int32_t imm;
        Register::Encoding gpr;
        FloatRegister::Encoding fpr;
    } u_;

  public:
    OtherOperand()
      : kind_(Imm)
    {
        u_.imm = 0;
        MOZ_ASSERT(*this == *this);
    }

    explicit OtherOperand(int32_t imm)
      : kind_(Imm)
    {
        u_.imm = imm;
        MOZ_ASSERT(*this == *this);
    }

    explicit OtherOperand(Register::Encoding gpr)
      : kind_(GPR)
    {
        u_.gpr = gpr;
        MOZ_ASSERT(*this == *this);
    }

    explicit OtherOperand(FloatRegister::Encoding fpr)
      : kind_(FPR)
    {
        u_.fpr = fpr;
        MOZ_ASSERT(*this == *this);
    }

    Kind kind() const {
        return kind_;
    }

    int32_t imm() const {
        MOZ_ASSERT(kind_ == Imm);
        return u_.imm;
    }

    Register::Encoding gpr() const {
        MOZ_ASSERT(kind_ == GPR);
        return u_.gpr;
    }

    FloatRegister::Encoding fpr() const {
        MOZ_ASSERT(kind_ == FPR);
        return u_.fpr;
    }

#ifdef DEBUG
    bool operator==(const OtherOperand& other) const;
    bool operator!=(const OtherOperand& other) const;
#endif
};

class HeapAccess {
  public:
    enum Kind {
        Unknown,
        Load,       // any bits not covered by the load are zeroed
        LoadSext32, // like Load, but sign-extend to 32 bits
        Store
    };

  private:
    Kind kind_;
    size_t size_; // The number of bytes of memory accessed
    ComplexAddress address_;
    OtherOperand otherOperand_;

  public:
    HeapAccess()
      : kind_(Unknown),
        size_(0)
    {
        MOZ_ASSERT(*this == *this);
    }

    HeapAccess(Kind kind, size_t size, const ComplexAddress& address, const OtherOperand& otherOperand)
      : kind_(kind),
        size_(size),
        address_(address),
        otherOperand_(otherOperand)
    {
        MOZ_ASSERT(kind != Unknown);
        MOZ_ASSERT_IF(kind == LoadSext32, otherOperand.kind() != OtherOperand::FPR);
        MOZ_ASSERT_IF(kind == Load || kind == LoadSext32, otherOperand.kind() != OtherOperand::Imm);
        MOZ_ASSERT(*this == *this);
    }

    Kind kind() const {
        return kind_;
    }

    size_t size() const {
        MOZ_ASSERT(kind_ != Unknown);
        return size_;
    }

    const ComplexAddress& address() const {
        return address_;
    }

    const OtherOperand& otherOperand() const {
        return otherOperand_;
    }

#ifdef DEBUG
    bool operator==(const HeapAccess& other) const;
    bool operator!=(const HeapAccess& other) const;
#endif
};

MOZ_COLD uint8_t* DisassembleHeapAccess(uint8_t* ptr, HeapAccess* access);

#ifdef DEBUG
void DumpHeapAccess(const HeapAccess& access);

inline void
VerifyHeapAccess(uint8_t* begin, uint8_t* end, const HeapAccess& expected)
{
    HeapAccess disassembled;
    uint8_t* e = DisassembleHeapAccess(begin, &disassembled);
    MOZ_ASSERT(e == end);
    MOZ_ASSERT(disassembled == expected);
}
#endif

} // namespace Disassembler

} // namespace jit
} // namespace js

#endif /* jit_Disassembler_h */
