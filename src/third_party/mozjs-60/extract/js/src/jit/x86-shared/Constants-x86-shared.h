/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_Constants_x86_shared_h
#define jit_x86_shared_Constants_x86_shared_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"

#include <stddef.h>
#include <stdint.h>

namespace js {
namespace jit {

namespace X86Encoding {

enum RegisterID : uint8_t {
    rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi
#ifdef JS_CODEGEN_X64
   ,r8, r9, r10, r11, r12, r13, r14, r15
#endif
   ,invalid_reg
};

enum HRegisterID {
    ah = rsp,
    ch = rbp,
    dh = rsi,
    bh = rdi
};

enum XMMRegisterID {
    xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7
#ifdef JS_CODEGEN_X64
   ,xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14, xmm15
#endif
   ,invalid_xmm
};

inline const char* XMMRegName(XMMRegisterID reg)
{
    static const char* const names[] = {
        "%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5", "%xmm6", "%xmm7"
#ifdef JS_CODEGEN_X64
       ,"%xmm8", "%xmm9", "%xmm10", "%xmm11", "%xmm12", "%xmm13", "%xmm14", "%xmm15"
#endif
    };
    MOZ_ASSERT(size_t(reg) < mozilla::ArrayLength(names));
    return names[reg];
}

#ifdef JS_CODEGEN_X64
inline const char* GPReg64Name(RegisterID reg)
{
    static const char* const names[] = {
        "%rax", "%rcx", "%rdx", "%rbx", "%rsp", "%rbp", "%rsi", "%rdi"
#ifdef JS_CODEGEN_X64
       ,"%r8", "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15"
#endif
    };
    MOZ_ASSERT(size_t(reg) < mozilla::ArrayLength(names));
    return names[reg];
}
#endif

inline const char* GPReg32Name(RegisterID reg)
{
    static const char* const names[] = {
        "%eax", "%ecx", "%edx", "%ebx", "%esp", "%ebp", "%esi", "%edi"
#ifdef JS_CODEGEN_X64
       ,"%r8d", "%r9d", "%r10d", "%r11d", "%r12d", "%r13d", "%r14d", "%r15d"
#endif
    };
    MOZ_ASSERT(size_t(reg) < mozilla::ArrayLength(names));
    return names[reg];
}

inline const char* GPReg16Name(RegisterID reg)
{
    static const char* const names[] = {
        "%ax", "%cx", "%dx", "%bx", "%sp", "%bp", "%si", "%di"
#ifdef JS_CODEGEN_X64
       ,"%r8w", "%r9w", "%r10w", "%r11w", "%r12w", "%r13w", "%r14w", "%r15w"
#endif
    };
    MOZ_ASSERT(size_t(reg) < mozilla::ArrayLength(names));
    return names[reg];
}

inline const char* GPReg8Name(RegisterID reg)
{
    static const char* const names[] = {
        "%al", "%cl", "%dl", "%bl"
#ifdef JS_CODEGEN_X64
       ,"%spl", "%bpl", "%sil", "%dil",
        "%r8b", "%r9b", "%r10b", "%r11b", "%r12b", "%r13b", "%r14b", "%r15b"
#endif
    };
    MOZ_ASSERT(size_t(reg) < mozilla::ArrayLength(names));
    return names[reg];
}

inline const char* GPRegName(RegisterID reg)
{
#ifdef JS_CODEGEN_X64
    return GPReg64Name(reg);
#else
    return GPReg32Name(reg);
#endif
}

inline bool HasSubregL(RegisterID reg)
{
#ifdef JS_CODEGEN_X64
    // In 64-bit mode, all registers have an 8-bit lo subreg.
    return true;
#else
    // In 32-bit mode, only the first four registers do.
    return reg <= rbx;
#endif
}

inline bool HasSubregH(RegisterID reg)
{
    // The first four registers always have h registers. However, note that
    // on x64, h registers may not be used in instructions using REX
    // prefixes. Also note that this may depend on what other registers are
    // used!
    return reg <= rbx;
}

inline HRegisterID GetSubregH(RegisterID reg)
{
    MOZ_ASSERT(HasSubregH(reg));
    return HRegisterID(reg + 4);
}

inline const char* HRegName8(HRegisterID reg)
{
    static const char* const names[] = {
        "%ah", "%ch", "%dh", "%bh"
    };
    size_t index = reg - GetSubregH(rax);
    MOZ_ASSERT(index < mozilla::ArrayLength(names));
    return names[index];
}

enum Condition {
    ConditionO,
    ConditionNO,
    ConditionB,
    ConditionAE,
    ConditionE,
    ConditionNE,
    ConditionBE,
    ConditionA,
    ConditionS,
    ConditionNS,
    ConditionP,
    ConditionNP,
    ConditionL,
    ConditionGE,
    ConditionLE,
    ConditionG,

    ConditionC  = ConditionB,
    ConditionNC = ConditionAE
};

inline const char* CCName(Condition cc)
{
    static const char* const names[] = {
        "o ", "no", "b ", "ae", "e ", "ne", "be", "a ",
        "s ", "ns", "p ", "np", "l ", "ge", "le", "g "
    };
    MOZ_ASSERT(size_t(cc) < mozilla::ArrayLength(names));
    return names[cc];
}

// Conditions for CMP instructions (CMPSS, CMPSD, CMPPS, CMPPD, etc).
enum ConditionCmp {
    ConditionCmp_EQ    = 0x0,
    ConditionCmp_LT    = 0x1,
    ConditionCmp_LE    = 0x2,
    ConditionCmp_UNORD = 0x3,
    ConditionCmp_NEQ   = 0x4,
    ConditionCmp_NLT   = 0x5,
    ConditionCmp_NLE   = 0x6,
    ConditionCmp_ORD   = 0x7,
};

// Rounding modes for ROUNDSD.
enum RoundingMode {
    RoundToNearest = 0x0,
    RoundDown      = 0x1,
    RoundUp        = 0x2,
    RoundToZero    = 0x3
};

// Test whether the given address will fit in an address immediate field.
// This is always true on x86, but on x64 it's only true for addreses which
// fit in the 32-bit immediate field.
inline bool IsAddressImmediate(const void* address)
{
    intptr_t value = reinterpret_cast<intptr_t>(address);
    int32_t immediate = static_cast<int32_t>(value);
    return value == immediate;
}

// Convert the given address to a 32-bit immediate field value. This is a
// no-op on x86, but on x64 it asserts that the address is actually a valid
// address immediate.
inline int32_t AddressImmediate(const void* address)
{
    MOZ_ASSERT(IsAddressImmediate(address));
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(address));
}

} // namespace X86Encoding

} // namespace jit
} // namespace js

#endif /* jit_x86_shared_Constants_x86_shared_h */
