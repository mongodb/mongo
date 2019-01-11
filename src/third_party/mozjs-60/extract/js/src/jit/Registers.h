/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Registers_h
#define jit_Registers_h

#include "mozilla/Array.h"

#include "jit/IonTypes.h"
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
# include "jit/x86-shared/Architecture-x86-shared.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/Architecture-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/Architecture-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/Architecture-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/Architecture-mips64.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/Architecture-none.h"
#else
# error "Unknown architecture!"
#endif

namespace js {
namespace jit {

struct Register {
    typedef Registers Codes;
    typedef Codes::Encoding Encoding;
    typedef Codes::Code Code;
    typedef Codes::SetType SetType;

    Encoding reg_;
    explicit constexpr Register(Encoding e)
     : reg_(e)
    { }
    Register() = default;

    static Register FromCode(Code i) {
        MOZ_ASSERT(i < Registers::Total);
        Register r { Encoding(i) };
        return r;
    }
    static Register FromName(const char* name) {
        Code code = Registers::FromName(name);
        Register r { Encoding(code) };
        return r;
    }
    static Register Invalid() {
        Register r { Encoding(Codes::Invalid) };
        return r;
    }
    constexpr Code code() const {
        return Code(reg_);
    }
    Encoding encoding() const {
        MOZ_ASSERT(Code(reg_) < Registers::Total);
        return reg_;
    }
    const char* name() const {
        return Registers::GetName(code());
    }
    constexpr bool operator==(Register other) const {
        return reg_ == other.reg_;
    }
    constexpr bool operator!=(Register other) const {
        return reg_ != other.reg_;
    }
    bool volatile_() const {
        return !!((SetType(1) << code()) & Registers::VolatileMask);
    }
    bool aliases(const Register& other) const {
        return reg_ == other.reg_;
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

    SetType alignedOrDominatedAliasedSet() const {
        return SetType(1) << code();
    }

    static constexpr RegTypeName DefaultType = RegTypeName::GPR;

    template <RegTypeName = DefaultType>
    static SetType LiveAsIndexableSet(SetType s) {
        return SetType(0);
    }

    template <RegTypeName Name = DefaultType>
    static SetType AllocatableAsIndexableSet(SetType s) {
        static_assert(Name != RegTypeName::Any, "Allocatable set are not iterable");
        return SetType(0);
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

// Architectures where the stack pointer is not a plain register with a standard
// register encoding must define JS_HAS_HIDDEN_SP and HiddenSPEncoding.

#ifdef JS_HAS_HIDDEN_SP
struct RegisterOrSP
{
    // The register code -- but possibly one that cannot be represented as a bit
    // position in a 32-bit vector.
    const uint32_t code;

    explicit RegisterOrSP(uint32_t code) : code(code) {}
    explicit RegisterOrSP(Register r) : code(r.code()) {}
};

static inline bool
IsHiddenSP(RegisterOrSP r)
{
    return r.code == HiddenSPEncoding;
}

static inline Register
AsRegister(RegisterOrSP r)
{
    MOZ_ASSERT(!IsHiddenSP(r));
    return Register::FromCode(r.code);
}

inline bool
operator == (Register r, RegisterOrSP e) {
    return r.code() == e.code;
}

inline bool
operator != (Register r, RegisterOrSP e) {
    return !(r == e);
}

inline bool
operator == (RegisterOrSP e, Register r) {
    return r == e;
}

inline bool
operator != (RegisterOrSP e, Register r) {
    return r != e;
}

inline bool
operator == (RegisterOrSP lhs, RegisterOrSP rhs) {
    return lhs.code == rhs.code;
}

inline bool
operator != (RegisterOrSP lhs, RegisterOrSP rhs) {
    return !(lhs == rhs);
}
#else
// On platforms where there's nothing special about SP, make RegisterOrSP be
// just Register, and return false for IsHiddenSP(r) for any r so that we use
// "normal" code for handling the SP.  This reduces ifdeffery throughout the
// jit.
typedef Register RegisterOrSP;

static inline bool
IsHiddenSP(RegisterOrSP r)
{
    return false;
}

static inline Register
AsRegister(RegisterOrSP r)
{
    return r;
}
#endif

template <> inline Register::SetType
Register::LiveAsIndexableSet<RegTypeName::GPR>(SetType set)
{
    return set;
}

template <> inline Register::SetType
Register::LiveAsIndexableSet<RegTypeName::Any>(SetType set)
{
    return set;
}

template <> inline Register::SetType
Register::AllocatableAsIndexableSet<RegTypeName::GPR>(SetType set)
{
    return set;
}

#if JS_BITS_PER_WORD == 32
// Note, some platform code depends on INT64LOW_OFFSET being zero.
static const uint32_t INT64LOW_OFFSET = 0 * sizeof(int32_t);
static const uint32_t INT64HIGH_OFFSET = 1 * sizeof(int32_t);
#endif

struct Register64
{
#ifdef JS_PUNBOX64
    Register reg;
#else
    Register high;
    Register low;
#endif

#ifdef JS_PUNBOX64
    explicit constexpr Register64(Register r)
      : reg(r)
    {}
    bool operator ==(Register64 other) const {
        return reg == other.reg;
    }
    bool operator !=(Register64 other) const {
        return reg != other.reg;
    }
    static Register64 Invalid() {
        return Register64(Register::Invalid());
    }
#else
    constexpr Register64(Register h, Register l)
      : high(h), low(l)
    {}
    bool operator ==(Register64 other) const {
        return high == other.high && low == other.low;
    }
    bool operator !=(Register64 other) const {
        return high != other.high || low != other.low;
    }
    static Register64 Invalid() {
        return Register64(Register::Invalid(), Register::Invalid());
    }
#endif
};

class RegisterDump
{
  public:
    typedef mozilla::Array<Registers::RegisterContent, Registers::Total> GPRArray;
    typedef mozilla::Array<FloatRegisters::RegisterContent, FloatRegisters::TotalPhys> FPUArray;

  protected: // Silence Clang warning.
    GPRArray regs_;
    FPUArray fpregs_;

  public:
    static size_t offsetOfRegister(Register reg) {
        return offsetof(RegisterDump, regs_) + reg.code() * sizeof(uintptr_t);
    }
    static size_t offsetOfRegister(FloatRegister reg) {
        return offsetof(RegisterDump, fpregs_) + reg.getRegisterDumpOffsetInBytes();
    }
};

// Information needed to recover machine register state. This records the
// location of spilled register and not the content of the spilled
// registers. Thus we can safely assume that this structure is unchanged, even
// if the GC pointers mapped by this structure are relocated.
class MachineState
{
    mozilla::Array<Registers::RegisterContent*, Registers::Total> regs_;
    mozilla::Array<FloatRegisters::RegisterContent*, FloatRegisters::Total> fpregs_;

  public:
    MachineState() {
#ifndef JS_CODEGEN_NONE
        for (uintptr_t i = 0; i < Registers::Total; i++)
            regs_[i] = reinterpret_cast<Registers::RegisterContent*>(i + 0x100);
        for (uintptr_t i = 0; i < FloatRegisters::Total; i++)
            fpregs_[i] = reinterpret_cast<FloatRegisters::RegisterContent*>(i + 0x200);
#endif
    }

    static MachineState FromBailout(RegisterDump::GPRArray& regs, RegisterDump::FPUArray& fpregs);

    void setRegisterLocation(Register reg, uintptr_t* up) {
        regs_[reg.code()] = (Registers::RegisterContent*) up;
    }
    void setRegisterLocation(FloatRegister reg, float* fp) {
        MOZ_ASSERT(reg.isSingle());
        fpregs_[reg.code()] = (FloatRegisters::RegisterContent*) fp;
    }
    void setRegisterLocation(FloatRegister reg, double* dp) {
        fpregs_[reg.code()] = (FloatRegisters::RegisterContent*) dp;
    }
    void setRegisterLocation(FloatRegister reg, FloatRegisters::RegisterContent* rp) {
        fpregs_[reg.code()] = rp;
    }

    bool has(Register reg) const {
        return regs_[reg.code()] != nullptr;
    }
    bool has(FloatRegister reg) const {
        return fpregs_[reg.code()] != nullptr;
    }
    uintptr_t read(Register reg) const {
        return regs_[reg.code()]->r;
    }
    double read(FloatRegister reg) const {
        return fpregs_[reg.code()]->d;
    }
    void write(Register reg, uintptr_t value) const {
        regs_[reg.code()]->r = value;
    }
    const FloatRegisters::RegisterContent* address(FloatRegister reg) const {
        return fpregs_[reg.code()];
    }
};

class MacroAssembler;

// Declares a register as owned within the scope of the object.
// In debug mode, owned register state is tracked within the MacroAssembler,
// and an assert will fire if ownership is conflicting.
// In contrast to ARM64's UseScratchRegisterScope, this class has no overhead
// in non-debug builds.
template <class RegisterType>
struct AutoGenericRegisterScope : public RegisterType
{
    // Prevent MacroAssembler templates from creating copies,
    // which causes the destructor to fire more than once.
    AutoGenericRegisterScope(const AutoGenericRegisterScope& other) = delete;

#ifdef DEBUG
    MacroAssembler& masm_;
    bool released_;
    explicit AutoGenericRegisterScope(MacroAssembler& masm, RegisterType reg);
    ~AutoGenericRegisterScope();
    void release();
    void reacquire();
#else
    constexpr explicit AutoGenericRegisterScope(MacroAssembler& masm, RegisterType reg)
      : RegisterType(reg)
    { }
    void release() {}
    void reacquire() {}
#endif
};

typedef AutoGenericRegisterScope<Register> AutoRegisterScope;
typedef AutoGenericRegisterScope<FloatRegister> AutoFloatRegisterScope;

} // namespace jit
} // namespace js

#endif /* jit_Registers_h */
