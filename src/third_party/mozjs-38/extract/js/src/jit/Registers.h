/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Registers_h
#define jit_Registers_h

#include "mozilla/Array.h"

#include "jit/IonTypes.h"
#if defined(JS_CODEGEN_X86)
# include "jit/x86/Architecture-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/Architecture-x64.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/Architecture-arm.h"
#elif defined(JS_CODEGEN_MIPS)
# include "jit/mips/Architecture-mips.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/Architecture-none.h"
#else
# error "Unknown architecture!"
#endif

namespace js {
namespace jit {

struct Register {
    typedef Registers Codes;
    typedef Codes::Code Code;
    typedef Codes::SetType SetType;
    Code code_;
    static Register FromCode(uint32_t i) {
        MOZ_ASSERT(i < Registers::Total);
        Register r = { (Registers::Code)i };
        return r;
    }
    static Register FromName(const char* name) {
        Registers::Code code = Registers::FromName(name);
        Register r = { code };
        return r;
    }
    Code code() const {
        MOZ_ASSERT((uint32_t)code_ < Registers::Total);
        return code_;
    }
    const char* name() const {
        return Registers::GetName(code());
    }
    bool operator ==(Register other) const {
        return code_ == other.code_;
    }
    bool operator !=(Register other) const {
        return code_ != other.code_;
    }
    bool volatile_() const {
        return !!((1 << code()) & Registers::VolatileMask);
    }
    bool aliases(const Register& other) const {
        return code_ == other.code_;
    }
    uint32_t numAliased() const {
        return 1;
    }

    // N.B. FloatRegister is an explicit outparam here because msvc-2010
    // miscompiled it on win64 when the value was simply returned.  This
    // now has an explicit outparam for compatability.
    void aliased(uint32_t aliasIdx, Register* ret) const {
        MOZ_ASSERT(aliasIdx == 0);
        *ret = *this;
    }
    static uint32_t SetSize(SetType x) {
        return Codes::SetSize(x);
    }
    static uint32_t FirstBit(SetType x) {
        return Codes::FirstBit(x);
    }
    static uint32_t LastBit(SetType x) {
        return Codes::LastBit(x);
    }
};

class RegisterDump
{
  protected: // Silence Clang warning.
    mozilla::Array<uintptr_t, Registers::Total> regs_;
    mozilla::Array<double, FloatRegisters::TotalPhys> fpregs_;

  public:
    static size_t offsetOfRegister(Register reg) {
        return offsetof(RegisterDump, regs_) + reg.code() * sizeof(uintptr_t);
    }
    static size_t offsetOfRegister(FloatRegister reg) {
        return offsetof(RegisterDump, fpregs_) + reg.getRegisterDumpOffsetInBytes();
    }
};

// Information needed to recover machine register state.
class MachineState
{
    mozilla::Array<uintptr_t*, Registers::Total> regs_;
    mozilla::Array<double*, FloatRegisters::Total> fpregs_;

  public:
    static MachineState FromBailout(mozilla::Array<uintptr_t, Registers::Total>& regs,
                                    mozilla::Array<double, FloatRegisters::TotalPhys>& fpregs);

    void setRegisterLocation(Register reg, uintptr_t* up) {
        regs_[reg.code()] = up;
    }
    void setRegisterLocation(FloatRegister reg, double* dp) {
        fpregs_[reg.code()] = dp;
    }

    bool has(Register reg) const {
        return regs_[reg.code()] != nullptr;
    }
    bool has(FloatRegister reg) const {
        return fpregs_[reg.code()] != nullptr;
    }
    uintptr_t read(Register reg) const {
        return *regs_[reg.code()];
    }
    double read(FloatRegister reg) const {
        return *fpregs_[reg.code()];
    }
    void write(Register reg, uintptr_t value) const {
        *regs_[reg.code()] = value;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_Registers_h */
