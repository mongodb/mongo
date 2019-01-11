/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* WebAssembly baseline compiler ("RabaldrMonkey")
 *
 * General assumptions for 32-bit vs 64-bit code:
 *
 * - A 32-bit register can be extended in-place to a 64-bit register on 64-bit
 *   systems.
 *
 * - Code that knows that Register64 has a '.reg' member on 64-bit systems and
 *   '.high' and '.low' members on 32-bit systems, or knows the implications
 *   thereof, is #ifdef JS_PUNBOX64.  All other code is #if(n)?def JS_64BIT.
 *
 *
 * Coding standards:
 *
 * - In "small" code generating functions (eg emitMultiplyF64, emitQuotientI32,
 *   and surrounding functions; most functions fall into this class) where the
 *   meaning is obvious:
 *
 *   - if there is a single source + destination register, it is called 'r'
 *   - if there is one source and a different destination, they are called 'rs'
 *     and 'rd'
 *   - if there is one source + destination register and another source register
 *     they are called 'r' and 'rs'
 *   - if there are two source registers and a destination register they are
 *     called 'rs0', 'rs1', and 'rd'.
 *
 * - Generic temp registers are named /temp[0-9]?/ not /tmp[0-9]?/.
 *
 * - Registers can be named non-generically for their function ('rp' for the
 *   'pointer' register and 'rv' for the 'value' register are typical) and those
 *   names may or may not have an 'r' prefix.
 *
 * - "Larger" code generating functions make their own rules.
 *
 *
 * General status notes:
 *
 * "FIXME" indicates a known or suspected bug.  Always has a bug#.
 *
 * "TODO" indicates an opportunity for a general improvement, with an additional
 * tag to indicate the area of improvement.  Usually has a bug#.
 *
 * There are lots of machine dependencies here but they are pretty well isolated
 * to a segment of the compiler.  Many dependencies will eventually be factored
 * into the MacroAssembler layer and shared with other code generators.
 *
 *
 * High-value compiler performance improvements:
 *
 * - (Bug 1316802) The specific-register allocator (the needI32(r), needI64(r)
 *   etc methods) can avoid syncing the value stack if the specific register is
 *   in use but there is a free register to shuffle the specific register into.
 *   (This will also improve the generated code.)  The sync happens often enough
 *   here to show up in profiles, because it is triggered by integer multiply
 *   and divide.
 *
 *
 * High-value code generation improvements:
 *
 * - (Bug 1316804) brTable pessimizes by always dispatching to code that pops
 *   the stack and then jumps to the code for the target case.  If no cleanup is
 *   needed we could just branch conditionally to the target; if the same amount
 *   of cleanup is needed for all cases then the cleanup can be done before the
 *   dispatch.  Both are highly likely.
 *
 * - (Bug 1316806) Register management around calls: At the moment we sync the
 *   value stack unconditionally (this is simple) but there are probably many
 *   common cases where we could instead save/restore live caller-saves
 *   registers and perform parallel assignment into argument registers.  This
 *   may be important if we keep some locals in registers.
 *
 * - (Bug 1316808) Allocate some locals to registers on machines where there are
 *   enough registers.  This is probably hard to do well in a one-pass compiler
 *   but it might be that just keeping register arguments and the first few
 *   locals in registers is a viable strategy; another (more general) strategy
 *   is caching locals in registers in straight-line code.  Such caching could
 *   also track constant values in registers, if that is deemed valuable.  A
 *   combination of techniques may be desirable: parameters and the first few
 *   locals could be cached on entry to the function but not statically assigned
 *   to registers throughout.
 *
 *   (On a large corpus of code it should be possible to compute, for every
 *   signature comprising the types of parameters and locals, and using a static
 *   weight for loops, a list in priority order of which parameters and locals
 *   that should be assigned to registers.  Or something like that.  Wasm makes
 *   this simple.  Static assignments are desirable because they are not flushed
 *   to memory by the pre-block sync() call.)
 */

#include "wasm/WasmBaselineCompile.h"

#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"

#include "jit/AtomicOp.h"
#include "jit/IonTypes.h"
#include "jit/JitAllocPolicy.h"
#include "jit/Label.h"
#include "jit/MacroAssembler.h"
#include "jit/MIR.h"
#include "jit/RegisterAllocator.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#if defined(JS_CODEGEN_ARM)
# include "jit/arm/Assembler-arm.h"
#endif
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
# include "jit/x86-shared/Architecture-x86-shared.h"
# include "jit/x86-shared/Assembler-x86-shared.h"
#endif
#if defined(JS_CODEGEN_MIPS32)
# include "jit/mips-shared/Assembler-mips-shared.h"
# include "jit/mips32/Assembler-mips32.h"
#endif
#if defined(JS_CODEGEN_MIPS64)
# include "jit/mips-shared/Assembler-mips-shared.h"
# include "jit/mips64/Assembler-mips64.h"
#endif

#include "wasm/WasmBinaryIterator.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmValidate.h"

#include "jit/MacroAssembler-inl.h"

using mozilla::DebugOnly;
using mozilla::FloorLog2;
using mozilla::IsPowerOfTwo;
using mozilla::Maybe;

namespace js {
namespace wasm {

using namespace js::jit;

typedef bool HandleNaNSpecially;
typedef bool InvertBranch;
typedef bool IsKnownNotZero;
typedef bool IsSigned;
typedef bool IsUnsigned;
typedef bool NeedsBoundsCheck;
typedef bool PopStack;
typedef bool WantResult;
typedef bool ZeroOnOverflow;

typedef unsigned ByteSize;
typedef unsigned BitSize;

// UseABI::Wasm implies that the Tls/Heap/Global registers are nonvolatile,
// except when InterModule::True is also set, when they are volatile.
//
// UseABI::System implies that the Tls/Heap/Global registers are volatile.
// Additionally, the parameter passing mechanism may be slightly different from
// the UseABI::Wasm convention.
//
// When the Tls/Heap/Global registers are not volatile, the baseline compiler
// will restore the Tls register from its save slot before the call, since the
// baseline compiler uses the Tls register for other things.
//
// When those registers are volatile, the baseline compiler will reload them
// after the call (it will restore the Tls register from the save slot and load
// the other two from the Tls data).

enum class UseABI { Wasm, System };
enum class InterModule { False = false, True = true };

#if defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_NONE)
# define RABALDR_SCRATCH_I32
# define RABALDR_SCRATCH_F32
# define RABALDR_SCRATCH_F64

static const Register RabaldrScratchI32 = Register::Invalid();
static const FloatRegister RabaldrScratchF32 = InvalidFloatReg;
static const FloatRegister RabaldrScratchF64 = InvalidFloatReg;
#endif

#ifdef JS_CODEGEN_X86
// The selection of EBX here steps gingerly around: the need for EDX
// to be allocatable for multiply/divide; ECX to be allocatable for
// shift/rotate; EAX (= ReturnReg) to be allocatable as the joinreg;
// EBX not being one of the WasmTableCall registers; and needing a
// temp register for load/store that has a single-byte persona.
//
// The compiler assumes that RabaldrScratchI32 has a single-byte
// persona.  Code for 8-byte atomic operations assumes that
// RabaldrScratchI32 is in fact ebx.

# define RABALDR_SCRATCH_I32
static const Register RabaldrScratchI32 = ebx;

# define RABALDR_INT_DIV_I64_CALLOUT
#endif

#ifdef JS_CODEGEN_ARM
// We use our own scratch register, because the macro assembler uses
// the regular scratch register(s) pretty liberally.  We could
// work around that in several cases but the mess does not seem
// worth it yet.  CallTempReg2 seems safe.

# define RABALDR_SCRATCH_I32
static const Register RabaldrScratchI32 = CallTempReg2;

# define RABALDR_INT_DIV_I64_CALLOUT
# define RABALDR_I64_TO_FLOAT_CALLOUT
# define RABALDR_FLOAT_TO_I64_CALLOUT
#endif

#ifdef JS_CODEGEN_MIPS32
# define RABALDR_SCRATCH_I32
static const Register RabaldrScratchI32 = CallTempReg2;

# define RABALDR_INT_DIV_I64_CALLOUT
# define RABALDR_I64_TO_FLOAT_CALLOUT
# define RABALDR_FLOAT_TO_I64_CALLOUT
#endif

#ifdef JS_CODEGEN_MIPS64
# define RABALDR_SCRATCH_I32
static const Register RabaldrScratchI32 = CallTempReg2;
#endif

template<MIRType t>
struct RegTypeOf {
    static_assert(t == MIRType::Float32 || t == MIRType::Double, "Float mask type");
};

template<> struct RegTypeOf<MIRType::Float32> {
    static constexpr RegTypeName value = RegTypeName::Float32;
};
template<> struct RegTypeOf<MIRType::Double> {
    static constexpr RegTypeName value = RegTypeName::Float64;
};

// The strongly typed register wrappers are especially useful to distinguish
// float registers from double registers, but they also clearly distinguish
// 32-bit registers from 64-bit register pairs on 32-bit systems.

struct RegI32 : public Register
{
    RegI32() : Register(Register::Invalid()) {}
    explicit RegI32(Register reg) : Register(reg) {}
    bool isValid() const { return *this != Invalid(); }
    bool isInvalid() const { return !isValid(); }
    static RegI32 Invalid() { return RegI32(Register::Invalid()); }
};

struct RegI64 : public Register64
{
    RegI64() : Register64(Register64::Invalid()) {}
    explicit RegI64(Register64 reg) : Register64(reg) {}
    bool isValid() const { return *this != Invalid(); }
    bool isInvalid() const { return !isValid(); }
    static RegI64 Invalid() { return RegI64(Register64::Invalid()); }
};

struct RegF32 : public FloatRegister
{
    RegF32() : FloatRegister() {}
    explicit RegF32(FloatRegister reg) : FloatRegister(reg) {}
    bool isValid() const { return *this != Invalid(); }
    bool isInvalid() const { return !isValid(); }
    static RegF32 Invalid() { return RegF32(InvalidFloatReg); }
};

struct RegF64 : public FloatRegister
{
    RegF64() : FloatRegister() {}
    explicit RegF64(FloatRegister reg) : FloatRegister(reg) {}
    bool isValid() const { return *this != Invalid(); }
    bool isInvalid() const { return !isValid(); }
    static RegF64 Invalid() { return RegF64(InvalidFloatReg); }
};

struct AnyReg
{
    explicit AnyReg(RegI32 r) { tag = I32; i32_ = r; }
    explicit AnyReg(RegI64 r) { tag = I64; i64_ = r; }
    explicit AnyReg(RegF32 r) { tag = F32; f32_ = r; }
    explicit AnyReg(RegF64 r) { tag = F64; f64_ = r; }

    RegI32 i32() const {
        MOZ_ASSERT(tag == I32);
        return i32_;
    }
    RegI64 i64() const {
        MOZ_ASSERT(tag == I64);
        return i64_;
    }
    RegF32 f32() const {
        MOZ_ASSERT(tag == F32);
        return f32_;
    }
    RegF64 f64() const {
        MOZ_ASSERT(tag == F64);
        return f64_;
    }
    AnyRegister any() const {
        switch (tag) {
          case F32: return AnyRegister(f32_);
          case F64: return AnyRegister(f64_);
          case I32: return AnyRegister(i32_);
          case I64:
#ifdef JS_PUNBOX64
            return AnyRegister(i64_.reg);
#else
            // The compiler is written so that this is never needed: any() is
            // called on arbitrary registers for asm.js but asm.js does not have
            // 64-bit ints.  For wasm, any() is called on arbitrary registers
            // only on 64-bit platforms.
            MOZ_CRASH("AnyReg::any() on 32-bit platform");
#endif
          default:
            MOZ_CRASH();
        }
        // Work around GCC 5 analysis/warning bug.
        MOZ_CRASH("AnyReg::any(): impossible case");
    }

    union {
        RegI32 i32_;
        RegI64 i64_;
        RegF32 f32_;
        RegF64 f64_;
    };
    enum { I32, I64, F32, F64 } tag;
};

// Platform-specific registers.
//
// All platforms must define struct SpecificRegs.  All 32-bit platforms must
// have an abiReturnRegI64 member in that struct.

#if defined(JS_CODEGEN_X64)
struct SpecificRegs
{
    RegI32 eax, ecx, edx, edi, esi;
    RegI64 rax, rcx, rdx;

    SpecificRegs()
      : eax(RegI32(js::jit::eax)),
        ecx(RegI32(js::jit::ecx)),
        edx(RegI32(js::jit::edx)),
        edi(RegI32(js::jit::edi)),
        esi(RegI32(js::jit::esi)),
        rax(RegI64(Register64(js::jit::rax))),
        rcx(RegI64(Register64(js::jit::rcx))),
        rdx(RegI64(Register64(js::jit::rdx)))
    {}
};
#elif defined(JS_CODEGEN_X86)
struct SpecificRegs
{
    RegI32 eax, ecx, edx, edi, esi;
    RegI64 ecx_ebx, edx_eax, abiReturnRegI64;

    SpecificRegs()
      : eax(RegI32(js::jit::eax)),
        ecx(RegI32(js::jit::ecx)),
        edx(RegI32(js::jit::edx)),
        edi(RegI32(js::jit::edi)),
        esi(RegI32(js::jit::esi)),
        ecx_ebx(RegI64(Register64(js::jit::ecx, js::jit::ebx))),
        edx_eax(RegI64(Register64(js::jit::edx, js::jit::eax))),
        abiReturnRegI64(edx_eax)
    {}
};
#elif defined(JS_CODEGEN_ARM)
struct SpecificRegs
{
    RegI64 abiReturnRegI64;

    SpecificRegs()
      : abiReturnRegI64(ReturnReg64)
    {}
};
#elif defined(JS_CODEGEN_MIPS32)
struct SpecificRegs
{
    RegI64 abiReturnRegI64;

    SpecificRegs()
      : abiReturnRegI64(ReturnReg64)
    {}
};
#elif defined(JS_CODEGEN_MIPS64)
struct SpecificRegs {};
#else
struct SpecificRegs
{
# ifndef JS_64BIT
    RegI64 abiReturnRegI64;
# endif

    SpecificRegs() {
        MOZ_CRASH("BaseCompiler porting interface: SpecificRegs");
    }
};
#endif

class BaseCompilerInterface
{
  public:
    // Spill all spillable registers.
    //
    // TODO / OPTIMIZE (Bug 1316802): It's possible to do better here by
    // spilling only enough registers to satisfy current needs.
    virtual void sync() = 0;
};

// Register allocator.

class BaseRegAlloc
{
    // Notes on float register allocation.
    //
    // The general rule in SpiderMonkey is that float registers can alias double
    // registers, but there are predicates to handle exceptions to that rule:
    // hasUnaliasedDouble() and hasMultiAlias().  The way aliasing actually
    // works is platform dependent and exposed through the aliased(n, &r)
    // predicate, etc.
    //
    //  - hasUnaliasedDouble(): on ARM VFPv3-D32 there are double registers that
    //    cannot be treated as float.
    //  - hasMultiAlias(): on ARM and MIPS a double register aliases two float
    //    registers.
    //
    // On some platforms (x86, x64, ARM64) but not all (ARM)
    // ScratchFloat32Register is the same as ScratchDoubleRegister.
    //
    // It's a basic invariant of the AllocatableRegisterSet that it deals
    // properly with aliasing of registers: if s0 or s1 are allocated then d0 is
    // not allocatable; if s0 and s1 are freed individually then d0 becomes
    // allocatable.

    BaseCompilerInterface&        bc;
    AllocatableGeneralRegisterSet availGPR;
    AllocatableFloatRegisterSet   availFPU;
#ifdef DEBUG
    AllocatableGeneralRegisterSet allGPR;       // The registers available to the compiler
    AllocatableFloatRegisterSet   allFPU;       //   after removing ScratchReg, HeapReg, etc
    uint32_t                      scratchTaken;
#endif
#ifdef JS_CODEGEN_X86
    AllocatableGeneralRegisterSet singleByteRegs;
#endif

    bool hasGPR() {
        return !availGPR.empty();
    }

    bool hasGPR64() {
#ifdef JS_PUNBOX64
        return !availGPR.empty();
#else
        if (availGPR.empty())
            return false;
        Register r = allocGPR();
        bool available = !availGPR.empty();
        freeGPR(r);
        return available;
#endif
    }

    template<MIRType t>
    bool hasFPU() {
        return availFPU.hasAny<RegTypeOf<t>::value>();
    }

    bool isAvailableGPR(Register r) {
        return availGPR.has(r);
    }

    bool isAvailableFPU(FloatRegister r) {
        return availFPU.has(r);
    }

    void allocGPR(Register r) {
        MOZ_ASSERT(isAvailableGPR(r));
        availGPR.take(r);
    }

    Register allocGPR() {
        MOZ_ASSERT(hasGPR());
        return availGPR.takeAny();
    }

    void allocInt64(Register64 r) {
#ifdef JS_PUNBOX64
        allocGPR(r.reg);
#else
        allocGPR(r.low);
        allocGPR(r.high);
#endif
    }

    Register64 allocInt64() {
        MOZ_ASSERT(hasGPR64());
#ifdef JS_PUNBOX64
        return Register64(availGPR.takeAny());
#else
        Register high = availGPR.takeAny();
        Register low = availGPR.takeAny();
        return Register64(high, low);
#endif
    }

#ifdef JS_CODEGEN_ARM
    // r12 is normally the ScratchRegister and r13 is always the stack pointer,
    // so the highest possible pair has r10 as the even-numbered register.

    static const uint32_t pairLimit = 10;

    bool hasGPRPair() {
        for (uint32_t i = 0; i <= pairLimit; i += 2) {
            if (isAvailableGPR(Register::FromCode(i)) && isAvailableGPR(Register::FromCode(i + 1)))
                return true;
        }
        return false;
    }

    void allocGPRPair(Register* low, Register* high) {
        MOZ_ASSERT(hasGPRPair());
        for (uint32_t i = 0; i <= pairLimit; i += 2) {
            if (isAvailableGPR(Register::FromCode(i)) &&
                isAvailableGPR(Register::FromCode(i + 1)))
            {
                *low = Register::FromCode(i);
                *high = Register::FromCode(i + 1);
                allocGPR(*low);
                allocGPR(*high);
                return;
            }
        }
        MOZ_CRASH("No pair");
    }
#endif

    void allocFPU(FloatRegister r) {
        MOZ_ASSERT(isAvailableFPU(r));
        availFPU.take(r);
    }

    template<MIRType t>
    FloatRegister allocFPU() {
        return availFPU.takeAny<RegTypeOf<t>::value>();
    }

    void freeGPR(Register r) {
        availGPR.add(r);
    }

    void freeInt64(Register64 r) {
#ifdef JS_PUNBOX64
        freeGPR(r.reg);
#else
        freeGPR(r.low);
        freeGPR(r.high);
#endif
    }

    void freeFPU(FloatRegister r) {
        availFPU.add(r);
    }

  public:
    explicit BaseRegAlloc(BaseCompilerInterface& bc)
      : bc(bc)
      , availGPR(GeneralRegisterSet::All())
      , availFPU(FloatRegisterSet::All())
#ifdef DEBUG
      , scratchTaken(0)
#endif
#ifdef JS_CODEGEN_X86
      , singleByteRegs(GeneralRegisterSet(Registers::SingleByteRegs))
#endif
    {
        RegisterAllocator::takeWasmRegisters(availGPR);

        // Allocate any private scratch registers.  For now we assume none of
        // these registers alias.
#if defined(RABALDR_SCRATCH_I32)
        if (RabaldrScratchI32 != RegI32::Invalid())
            availGPR.take(RabaldrScratchI32);
#endif
#if defined(RABALDR_SCRATCH_F32)
        if (RabaldrScratchF32 != RegF32::Invalid())
            availFPU.take(RabaldrScratchF32);
#endif
#if defined(RABALDR_SCRATCH_F64)
        if (RabaldrScratchF64 != RegF64::Invalid())
            availFPU.take(RabaldrScratchF64);
#endif

#ifdef DEBUG
        allGPR = availGPR;
        allFPU = availFPU;
#endif
    }

    enum class ScratchKind {
        I32 = 1,
        F32 = 2,
        F64 = 4
    };

#ifdef DEBUG
    bool isScratchRegisterTaken(ScratchKind s) const {
        return (scratchTaken & uint32_t(s)) != 0;
    }

    void setScratchRegisterTaken(ScratchKind s, bool state) {
        if (state)
            scratchTaken |= uint32_t(s);
        else
            scratchTaken &= ~uint32_t(s);
    }
#endif

#ifdef JS_CODEGEN_X86
    bool isSingleByteI32(Register r) {
        return singleByteRegs.has(r);
    }
#endif

    bool isAvailableI32(RegI32 r) {
        return isAvailableGPR(r);
    }

    bool isAvailableI64(RegI64 r) {
#ifdef JS_PUNBOX64
        return isAvailableGPR(r.reg);
#else
        return isAvailableGPR(r.low) && isAvailableGPR(r.high);
#endif
    }

    bool isAvailableF32(RegF32 r) {
        return isAvailableFPU(r);
    }

    bool isAvailableF64(RegF64 r) {
        return isAvailableFPU(r);
    }

    // TODO / OPTIMIZE (Bug 1316802): Do not sync everything on allocation
    // failure, only as much as we need.

    MOZ_MUST_USE RegI32 needI32() {
        if (!hasGPR())
            bc.sync();
        return RegI32(allocGPR());
    }

    void needI32(RegI32 specific) {
        if (!isAvailableI32(specific))
            bc.sync();
        allocGPR(specific);
    }

    MOZ_MUST_USE RegI64 needI64() {
        if (!hasGPR64())
            bc.sync();
        return RegI64(allocInt64());
    }

    void needI64(RegI64 specific) {
        if (!isAvailableI64(specific))
            bc.sync();
        allocInt64(specific);
    }

    MOZ_MUST_USE RegF32 needF32() {
        if (!hasFPU<MIRType::Float32>())
            bc.sync();
        return RegF32(allocFPU<MIRType::Float32>());
    }

    void needF32(RegF32 specific) {
        if (!isAvailableF32(specific))
            bc.sync();
        allocFPU(specific);
    }

    MOZ_MUST_USE RegF64 needF64() {
        if (!hasFPU<MIRType::Double>())
            bc.sync();
        return RegF64(allocFPU<MIRType::Double>());
    }

    void needF64(RegF64 specific) {
        if (!isAvailableF64(specific))
            bc.sync();
        allocFPU(specific);
    }

    void freeI32(RegI32 r) {
        freeGPR(r);
    }

    void freeI64(RegI64 r) {
        freeInt64(r);
    }

    void freeF64(RegF64 r) {
        freeFPU(r);
    }

    void freeF32(RegF32 r) {
        freeFPU(r);
    }

#ifdef JS_CODEGEN_ARM
    MOZ_MUST_USE RegI64 needI64Pair() {
        if (!hasGPRPair())
            bc.sync();
        Register low, high;
        allocGPRPair(&low, &high);
        return RegI64(Register64(high, low));
    }
#endif

#ifdef DEBUG
    friend class LeakCheck;

    class MOZ_RAII LeakCheck
    {
      private:
        const BaseRegAlloc&           ra;
        AllocatableGeneralRegisterSet knownGPR;
        AllocatableFloatRegisterSet   knownFPU;

      public:
        explicit LeakCheck(const BaseRegAlloc& ra) : ra(ra) {
            knownGPR = ra.availGPR;
            knownFPU = ra.availFPU;
        }

        ~LeakCheck() {
            MOZ_ASSERT(knownGPR.bits() == ra.allGPR.bits());
            MOZ_ASSERT(knownFPU.bits() == ra.allFPU.bits());
        }

        void addKnownI32(RegI32 r) {
            knownGPR.add(r);
        }

        void addKnownI64(RegI64 r) {
# ifdef JS_PUNBOX64
            knownGPR.add(r.reg);
# else
            knownGPR.add(r.high);
            knownGPR.add(r.low);
# endif
        }

        void addKnownF32(RegF32 r) {
            knownFPU.add(r);
        }

        void addKnownF64(RegF64 r) {
            knownFPU.add(r);
        }
    };
#endif
};

// Scratch register abstractions.
//
// We define our own scratch registers when the platform doesn't provide what we
// need.  A notable use case is that we will need a private scratch register
// when the platform masm uses its scratch register very frequently (eg, ARM).

class BaseScratchRegister
{
#ifdef DEBUG
    BaseRegAlloc& ra;
    BaseRegAlloc::ScratchKind s;

  public:
    explicit BaseScratchRegister(BaseRegAlloc& ra, BaseRegAlloc::ScratchKind s)
      : ra(ra),
        s(s)
    {
        MOZ_ASSERT(!ra.isScratchRegisterTaken(s));
        ra.setScratchRegisterTaken(s, true);
    }
    ~BaseScratchRegister() {
        MOZ_ASSERT(ra.isScratchRegisterTaken(s));
        ra.setScratchRegisterTaken(s, false);
    }
#else
  public:
    explicit BaseScratchRegister(BaseRegAlloc& ra, BaseRegAlloc::ScratchKind s) {}
#endif
};

#ifdef RABALDR_SCRATCH_F64
class ScratchF64 : public BaseScratchRegister
{
  public:
    explicit ScratchF64(BaseRegAlloc& ra)
      : BaseScratchRegister(ra, BaseRegAlloc::ScratchKind::F64)
    {}
    operator RegF64() const { return RegF64(RabaldrScratchF64); }
};
#else
class ScratchF64 : public ScratchDoubleScope
{
  public:
    explicit ScratchF64(MacroAssembler& m) : ScratchDoubleScope(m) {}
    operator RegF64() const { return RegF64(FloatRegister(*this)); }
};
#endif

#ifdef RABALDR_SCRATCH_F32
class ScratchF32 : public BaseScratchRegister
{
  public:
    explicit ScratchF32(BaseRegAlloc& ra)
      : BaseScratchRegister(ra, BaseRegAlloc::ScratchKind::F32)
    {}
    operator RegF32() const { return RegF32(RabaldrScratchF32); }
};
#else
class ScratchF32 : public ScratchFloat32Scope
{
  public:
    explicit ScratchF32(MacroAssembler& m) : ScratchFloat32Scope(m) {}
    operator RegF32() const { return RegF32(FloatRegister(*this)); }
};
#endif

#ifdef RABALDR_SCRATCH_I32
class ScratchI32 : public BaseScratchRegister
{
  public:
    explicit ScratchI32(BaseRegAlloc& ra)
      : BaseScratchRegister(ra, BaseRegAlloc::ScratchKind::I32)
    {}
    operator RegI32() const { return RegI32(RabaldrScratchI32); }
};
#else
class ScratchI32 : public ScratchRegisterScope
{
  public:
    explicit ScratchI32(MacroAssembler& m) : ScratchRegisterScope(m) {}
    operator RegI32() const { return RegI32(Register(*this)); }
};
#endif

#if defined(JS_CODEGEN_X86)
// ScratchEBX is a mnemonic device: For some atomic ops we really need EBX,
// no other register will do.  And we would normally have to allocate that
// register using ScratchI32 since normally the scratch register is EBX.
// But the whole point of ScratchI32 is to hide that relationship.  By using
// the ScratchEBX alias, we document that at that point we require the
// scratch register to be EBX.
using ScratchEBX = ScratchI32;

// ScratchI8 is a mnemonic device: For some ops we need a register with a
// byte subregister.
using ScratchI8 = ScratchI32;
#endif

// The stack frame.
//
// The frame has four parts ("below" means at lower addresses):
//
//  - the Header, comprising the Frame and DebugFrame elements;
//  - the Local area, allocated below the header with various forms of
//    alignment;
//  - the Stack area, comprising the temporary storage the compiler uses
//    for register spilling, allocated below the Local area;
//  - the Arguments area, comprising memory allocated for outgoing calls,
//    allocated below the stack area.
//
// The header is addressed off the stack pointer.  masm.framePushed() is always
// correct, and masm.getStackPointer() + masm.framePushed() always addresses the
// Frame, with the DebugFrame optionally below it.
//
// The local area is laid out by BaseLocalIter and is allocated and deallocated
// by standard prologue and epilogue functions that manipulate the stack
// pointer, but it is accessed via BaseStackFrame.
//
// The stack area is maintained by and accessed via BaseStackFrame.  On some
// systems, the stack memory may be allocated in chunks because the SP needs a
// specific alignment.
//
// The arguments area is allocated and deallocated via BaseStackFrame (see
// comments later) but is accessed directly off the stack pointer.

// BaseLocalIter iterates over a vector of types of locals and provides offsets
// from the Frame address for those locals, and associated data.
//
// The implementation of BaseLocalIter is the property of the BaseStackFrame.
// But it is also exposed for eg the debugger to use.

BaseLocalIter::BaseLocalIter(const ValTypeVector& locals,
                             size_t argsLength,
                             bool debugEnabled)
  : locals_(locals),
    argsLength_(argsLength),
    argsRange_(locals.begin(), argsLength),
    argsIter_(argsRange_),
    index_(0),
    localSize_(debugEnabled ? DebugFrame::offsetOfFrame() : 0),
    reservedSize_(localSize_),
    done_(false)
{
    MOZ_ASSERT(argsLength <= locals.length());

    settle();
}

int32_t
BaseLocalIter::pushLocal(size_t nbytes)
{
    MOZ_ASSERT(nbytes % 4 == 0 && nbytes <= 16);
    localSize_ = AlignBytes(localSize_, nbytes) + nbytes;
    return localSize_;          // Locals grow down so capture base address
}

void
BaseLocalIter::settle()
{
    if (index_ < argsLength_) {
        MOZ_ASSERT(!argsIter_.done());
        mirType_ = argsIter_.mirType();
        switch (mirType_) {
          case MIRType::Int32:
          case MIRType::Int64:
          case MIRType::Double:
          case MIRType::Float32:
            if (argsIter_->argInRegister())
                frameOffset_ = pushLocal(MIRTypeToSize(mirType_));
            else
                frameOffset_ = -(argsIter_->offsetFromArgBase() + sizeof(Frame));
            break;
          default:
            MOZ_CRASH("Argument type");
        }
        return;
    }

    MOZ_ASSERT(argsIter_.done());
    if (index_ < locals_.length()) {
        switch (locals_[index_]) {
          case ValType::I32:
          case ValType::I64:
          case ValType::F32:
          case ValType::F64:
            mirType_ = ToMIRType(locals_[index_]);
            frameOffset_ = pushLocal(MIRTypeToSize(mirType_));
            break;
          default:
            MOZ_CRASH("Compiler bug: Unexpected local type");
        }
        return;
    }

    done_ = true;
}

void
BaseLocalIter::operator++(int)
{
    MOZ_ASSERT(!done_);
    index_++;
    if (!argsIter_.done())
        argsIter_++;
    settle();
}

// Abstraction of the baseline compiler's stack frame (except for the Frame /
// DebugFrame parts).  See comments above for more.

class BaseStackFrame
{
    MacroAssembler& masm;

    // Size of local area in bytes (stable after beginFunction).
    uint32_t localSize_;

    // Low byte offset of local area for true locals (not parameters).
    uint32_t varLow_;

    // High byte offset + 1 of local area for true locals.
    uint32_t varHigh_;

    // The largest stack height, not necessarily zero-based.  Read this for its
    // true value only when code generation is finished.
    uint32_t maxStackHeight_;

    // Patch point where we check for stack overflow.
    CodeOffset stackAddOffset_;

    // The stack pointer, cached for brevity.
    RegisterOrSP sp_;

  public:

    explicit BaseStackFrame(MacroAssembler& masm)
      : masm(masm),
        localSize_(UINT32_MAX),
        varLow_(UINT32_MAX),
        varHigh_(UINT32_MAX),
        maxStackHeight_(0),
        stackAddOffset_(0),
        sp_(masm.getStackPointer())
    {}

    //////////////////////////////////////////////////////////////////////
    //
    // The local area.

    // Locals - the static part of the frame.

    struct Local
    {
        // Type of the value.
        const MIRType type;

        // Byte offset from Frame "into" the locals, ie positive for true locals
        // and negative for incoming args that read directly from the arg area.
        // It assumes the stack is growing down and that locals are on the stack
        // at lower addresses than Frame, and is the offset from Frame of the
        // lowest-addressed byte of the local.
        const int32_t offs;

        Local(MIRType type, int32_t offs) : type(type), offs(offs) {}
    };

    typedef Vector<Local, 8, SystemAllocPolicy> LocalVector;

  private:

    // Offset off of sp_ for `local`.
    int32_t localOffset(const Local& local) {
        return localOffset(local.offs);
    }

    // Offset off of sp_ for a local with offset `offset` from Frame.
    int32_t localOffset(int32_t offset) {
        return masm.framePushed() - offset;
    }

  public:

    void endFunctionPrologue() {
        MOZ_ASSERT(masm.framePushed() == localSize_);
        MOZ_ASSERT(localSize_ != UINT32_MAX);
        MOZ_ASSERT(localSize_ % WasmStackAlignment == 0);
        maxStackHeight_ = localSize_;
    }

    // Initialize `localInfo` based on the types of `locals` and `args`.
    bool setupLocals(const ValTypeVector& locals, const ValTypeVector& args, bool debugEnabled,
                     LocalVector* localInfo)
    {
        MOZ_ASSERT(maxStackHeight_ != UINT32_MAX);

        if (!localInfo->reserve(locals.length()))
            return false;

        DebugOnly<uint32_t> index = 0;
        BaseLocalIter i(locals, args.length(), debugEnabled);
        varLow_ = i.reservedSize();
        for (; !i.done() && i.index() < args.length(); i++) {
            MOZ_ASSERT(i.isArg());
            MOZ_ASSERT(i.index() == index);
            localInfo->infallibleEmplaceBack(i.mirType(), i.frameOffset());
            varLow_ = i.currentLocalSize();
            index++;
        }

        varHigh_ = varLow_;
        for (; !i.done() ; i++) {
            MOZ_ASSERT(!i.isArg());
            MOZ_ASSERT(i.index() == index);
            localInfo->infallibleEmplaceBack(i.mirType(), i.frameOffset());
            varHigh_ = i.currentLocalSize();
            index++;
        }

        localSize_ = AlignBytes(varHigh_, WasmStackAlignment);

        return true;
    }

    // The fixed amount of memory, in bytes, allocated on the stack below the
    // Frame for purposes such as locals and other fixed values.  Includes all
    // necessary alignment.

    uint32_t initialSize() const {
        MOZ_ASSERT(localSize_ != UINT32_MAX);

        return localSize_;
    }

    void zeroLocals(BaseRegAlloc& ra);

    void loadLocalI32(const Local& src, RegI32 dest) {
        masm.load32(Address(sp_, localOffset(src)), dest);
    }

#ifndef JS_PUNBOX64
    void loadLocalI64Low(const Local& src, RegI32 dest) {
        masm.load32(Address(sp_, localOffset(src) + INT64LOW_OFFSET), dest);
    }

    void loadLocalI64High(const Local& src, RegI32 dest) {
        masm.load32(Address(sp_, localOffset(src) + INT64HIGH_OFFSET), dest);
    }
#endif

    void loadLocalI64(const Local& src, RegI64 dest) {
        masm.load64(Address(sp_, localOffset(src)), dest);
    }

    void loadLocalPtr(const Local& src, Register dest) {
        masm.loadPtr(Address(sp_, localOffset(src)), dest);
    }

    void loadLocalF64(const Local& src, RegF64 dest) {
        masm.loadDouble(Address(sp_, localOffset(src)), dest);
    }

    void loadLocalF32(const Local& src, RegF32 dest) {
        masm.loadFloat32(Address(sp_, localOffset(src)), dest);
    }

    void storeLocalI32(RegI32 src, const Local& dest) {
        masm.store32(src, Address(sp_, localOffset(dest)));
    }

    void storeLocalI64(RegI64 src, const Local& dest) {
        masm.store64(src, Address(sp_, localOffset(dest)));
    }

    void storeLocalPtr(Register src, const Local& dest) {
        masm.storePtr(src, Address(sp_, localOffset(dest)));
    }

    void storeLocalF64(RegF64 src, const Local& dest) {
        masm.storeDouble(src, Address(sp_, localOffset(dest)));
    }

    void storeLocalF32(RegF32 src, const Local& dest) {
        masm.storeFloat32(src, Address(sp_, localOffset(dest)));
    }

    //////////////////////////////////////////////////////////////////////
    //
    // The stack area - the dynamic part of the frame.

  private:

    // Offset off of sp_ for the slot at stack area location `offset`
    int32_t stackOffset(int32_t offset) {
        return masm.framePushed() - offset;
    }

  public:

    // Sizes of items in the stack area.
    //
    // The size values come from the implementations of Push() in
    // MacroAssembler-x86-shared.cpp and MacroAssembler-arm-shared.cpp, and from
    // VFPRegister::size() in Architecture-arm.h.
    //
    // On ARM unlike on x86 we push a single for float.

    static const size_t StackSizeOfPtr    = sizeof(intptr_t);
    static const size_t StackSizeOfInt64  = sizeof(int64_t);
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32)
    static const size_t StackSizeOfFloat  = sizeof(float);
#else
    static const size_t StackSizeOfFloat  = sizeof(double);
#endif
    static const size_t StackSizeOfDouble = sizeof(double);

    // We won't know until after we've generated code how big the frame will be
    // (we may need arbitrary spill slots and outgoing param slots) so emit a
    // patchable add that is patched in endFunction().
    //
    // Note the platform scratch register may be used by branchPtr(), so
    // generally tmp must be something else.

    void allocStack(Register tmp, BytecodeOffset trapOffset) {
        stackAddOffset_ = masm.sub32FromStackPtrWithPatch(tmp);
        Label ok;
        masm.branchPtr(Assembler::Below,
                       Address(WasmTlsReg, offsetof(wasm::TlsData, stackLimit)),
                       tmp, &ok);
        masm.wasmTrap(Trap::StackOverflow, trapOffset);
        masm.bind(&ok);
    }

    void patchAllocStack() {
        masm.patchSub32FromStackPtr(stackAddOffset_,
                                    Imm32(int32_t(maxStackHeight_ - localSize_)));
    }

    // Very large frames are implausible, probably an attack.
    bool checkStackHeight() {
        // 512KiB should be enough, considering how Rabaldr uses the stack and
        // what the standard limits are:
        //
        // - 1,000 parameters
        // - 50,000 locals
        // - 10,000 values on the eval stack (not an official limit)
        //
        // At sizeof(int64) bytes per slot this works out to about 480KiB.
        return maxStackHeight_ <= 512 * 1024;
    }

    // The current height of the stack area, not necessarily zero-based.
    uint32_t stackHeight() const {
        return masm.framePushed();
    }

    // Set the frame height.  This must only be called with a value returned
    // from stackHeight().
    void setStackHeight(uint32_t amount) {
        masm.setFramePushed(amount);
    }

    uint32_t pushPtr(Register r) {
        DebugOnly<uint32_t> stackBefore = stackHeight();
        masm.Push(r);
        maxStackHeight_ = Max(maxStackHeight_, stackHeight());
        MOZ_ASSERT(stackBefore + StackSizeOfPtr == stackHeight());
        return stackHeight();
    }

    uint32_t pushFloat32(FloatRegister r) {
        DebugOnly<uint32_t> stackBefore = stackHeight();
        masm.Push(r);
        maxStackHeight_ = Max(maxStackHeight_, stackHeight());
        MOZ_ASSERT(stackBefore + StackSizeOfFloat == stackHeight());
        return stackHeight();
    }

    uint32_t pushDouble(FloatRegister r) {
        DebugOnly<uint32_t> stackBefore = stackHeight();
        masm.Push(r);
        maxStackHeight_ = Max(maxStackHeight_, stackHeight());
        MOZ_ASSERT(stackBefore + StackSizeOfDouble == stackHeight());
        return stackHeight();
    }

   void popPtr(Register r) {
        DebugOnly<uint32_t> stackBefore = stackHeight();
        masm.Pop(r);
        MOZ_ASSERT(stackBefore - StackSizeOfPtr == stackHeight());
    }

    void popFloat32(FloatRegister r) {
        DebugOnly<uint32_t> stackBefore = stackHeight();
        masm.Pop(r);
        MOZ_ASSERT(stackBefore - StackSizeOfFloat == stackHeight());
    }

    void popDouble(FloatRegister r) {
        DebugOnly<uint32_t> stackBefore = stackHeight();
        masm.Pop(r);
        MOZ_ASSERT(stackBefore - StackSizeOfDouble == stackHeight());
    }

    void popBytes(size_t bytes) {
        if (bytes > 0)
            masm.freeStack(bytes);
    }

    // Before branching to an outer control label, pop the execution stack to
    // the level expected by that region, but do not update masm.framePushed()
    // as that will happen as compilation leaves the block.

    void popStackBeforeBranch(uint32_t destStackHeight) {
        uint32_t stackHere = stackHeight();
        if (stackHere > destStackHeight)
            masm.addToStackPtr(Imm32(stackHere - destStackHeight));
    }

    bool willPopStackBeforeBranch(uint32_t destStackHeight) {
        uint32_t stackHere = stackHeight();
        return stackHere > destStackHeight;
    }

    // Before exiting a nested control region, pop the execution stack
    // to the level expected by the nesting region, and free the
    // stack.

    void popStackOnBlockExit(uint32_t destStackHeight, bool deadCode) {
        uint32_t stackHere = stackHeight();
        if (stackHere > destStackHeight) {
            if (deadCode)
                masm.setFramePushed(destStackHeight);
            else
                masm.freeStack(stackHere - destStackHeight);
        }
    }

    void loadStackI32(int32_t offset, RegI32 dest) {
        masm.load32(Address(sp_, stackOffset(offset)), dest);
    }

    void loadStackI64(int32_t offset, RegI64 dest) {
        masm.load64(Address(sp_, stackOffset(offset)), dest);
    }

#ifndef JS_PUNBOX64
    void loadStackI64Low(int32_t offset, RegI32 dest) {
        masm.load32(Address(sp_, stackOffset(offset - INT64LOW_OFFSET)), dest);
    }

    void loadStackI64High(int32_t offset, RegI32 dest) {
        masm.load32(Address(sp_, stackOffset(offset - INT64HIGH_OFFSET)), dest);
    }
#endif

    // Disambiguation: this loads a "Ptr" value from the stack, it does not load
    // the "StackPtr".

    void loadStackPtr(int32_t offset, Register dest) {
        masm.loadPtr(Address(sp_, stackOffset(offset)), dest);
    }

    void loadStackF64(int32_t offset, RegF64 dest) {
        masm.loadDouble(Address(sp_, stackOffset(offset)), dest);
    }

    void loadStackF32(int32_t offset, RegF32 dest) {
        masm.loadFloat32(Address(sp_, stackOffset(offset)), dest);
    }

    //////////////////////////////////////////////////////////////////////
    //
    // The argument area - for outgoing calls.
    //
    // We abstract these operations as an optimization: we can merge the freeing
    // of the argument area and dropping values off the stack after a call.  But
    // they always amount to manipulating the real stack pointer by some amount.

    // This is always equivalent to a masm.reserveStack() call.
    void allocArgArea(size_t size) {
        if (size)
            masm.reserveStack(size);
    }

    // This is always equivalent to a sequence of masm.freeStack() calls.
    void freeArgAreaAndPopBytes(size_t argSize, size_t dropSize) {
        if (argSize + dropSize)
            masm.freeStack(argSize + dropSize);
    }
};

void
BaseStackFrame::zeroLocals(BaseRegAlloc& ra)
{
    MOZ_ASSERT(varLow_ != UINT32_MAX);

    if (varLow_ == varHigh_)
        return;

    static const uint32_t wordSize = sizeof(void*);

    // The adjustments to 'low' by the size of the item being stored compensates
    // for the fact that locals offsets are the offsets from Frame to the bytes
    // directly "above" the locals in the locals area.  See comment at Local.

    // On 64-bit systems we may have 32-bit alignment for the local area as it
    // may be preceded by parameters and prologue/debug data.

    uint32_t low = varLow_;
    if (low % wordSize) {
        masm.store32(Imm32(0), Address(sp_, localOffset(low + 4)));
        low += 4;
    }
    MOZ_ASSERT(low % wordSize == 0);

    const uint32_t high = AlignBytes(varHigh_, wordSize);
    MOZ_ASSERT(high <= localSize_, "localSize_ should be aligned at least that");

    // An UNROLL_LIMIT of 16 is chosen so that we only need an 8-bit signed
    // immediate to represent the offset in the store instructions in the loop
    // on x64.

    const uint32_t UNROLL_LIMIT = 16;
    const uint32_t initWords = (high - low) / wordSize;
    const uint32_t tailWords = initWords % UNROLL_LIMIT;
    const uint32_t loopHigh = high - (tailWords * wordSize);

    // With only one word to initialize, just store an immediate zero.

    if (initWords == 1) {
        masm.storePtr(ImmWord(0), Address(sp_, localOffset(low + wordSize)));
        return;
    }

    // For other cases, it's best to have a zero in a register.
    //
    // One can do more here with SIMD registers (store 16 bytes at a time) or
    // with instructions like STRD on ARM (store 8 bytes at a time), but that's
    // for another day.

    RegI32 zero = ra.needI32();
    masm.mov(ImmWord(0), zero);

    // For the general case we want to have a loop body of UNROLL_LIMIT stores
    // and then a tail of less than UNROLL_LIMIT stores.  When initWords is less
    // than 2*UNROLL_LIMIT the loop trip count is at most 1 and there is no
    // benefit to having the pointer calculations and the compare-and-branch.
    // So we completely unroll when we have initWords < 2 * UNROLL_LIMIT.  (In
    // this case we'll end up using 32-bit offsets on x64 for up to half of the
    // stores, though.)

    // Fully-unrolled case.

    if (initWords < 2 * UNROLL_LIMIT)  {
        for (uint32_t i = low; i < high; i += wordSize)
            masm.storePtr(zero, Address(sp_, localOffset(i + wordSize)));
        ra.freeI32(zero);
        return;
    }

    // Unrolled loop with a tail. Stores will use negative offsets. That's OK
    // for x86 and ARM, at least.

    // Compute pointer to the highest-addressed slot on the frame.
    RegI32 p = ra.needI32();
    masm.computeEffectiveAddress(Address(sp_, localOffset(low + wordSize)),
                                 p);

    // Compute pointer to the lowest-addressed slot on the frame that will be
    // initialized by the loop body.
    RegI32 lim = ra.needI32();
    masm.computeEffectiveAddress(Address(sp_, localOffset(loopHigh + wordSize)), lim);

    // The loop body.  Eventually we'll have p == lim and exit the loop.
    Label again;
    masm.bind(&again);
    for (uint32_t i = 0; i < UNROLL_LIMIT; ++i)
        masm.storePtr(zero, Address(p, -(wordSize * i)));
    masm.subPtr(Imm32(UNROLL_LIMIT * wordSize), p);
    masm.branchPtr(Assembler::LessThan, lim, p, &again);

    // The tail.
    for (uint32_t i = 0; i < tailWords; ++i)
        masm.storePtr(zero, Address(p, -(wordSize * i)));

    ra.freeI32(p);
    ra.freeI32(lim);
    ra.freeI32(zero);
}

// The baseline compiler proper.

class BaseCompiler final : public BaseCompilerInterface
{
    typedef BaseStackFrame::Local Local;
    typedef Vector<NonAssertingLabel, 8, SystemAllocPolicy> LabelVector;
    typedef Vector<MIRType, 8, SystemAllocPolicy> MIRTypeVector;

    // Bit set used for simple bounds check elimination.  Capping this at 64
    // locals makes sense; even 32 locals would probably be OK in practice.
    //
    // For more information about BCE, see the block comment above
    // popMemoryAccess(), below.

    typedef uint64_t BCESet;

    // Control node, representing labels and stack heights at join points.

    struct Control
    {
        Control()
            : stackHeight(UINT32_MAX),
              stackSize(UINT32_MAX),
              bceSafeOnEntry(0),
              bceSafeOnExit(~BCESet(0)),
              deadOnArrival(false),
              deadThenBranch(false)
        {}

        NonAssertingLabel label;        // The "exit" label
        NonAssertingLabel otherLabel;   // Used for the "else" branch of if-then-else
        uint32_t stackHeight;           // From BaseStackFrame
        uint32_t stackSize;             // Value stack height
        BCESet bceSafeOnEntry;          // Bounds check info flowing into the item
        BCESet bceSafeOnExit;           // Bounds check info flowing out of the item
        bool deadOnArrival;             // deadCode_ was set on entry to the region
        bool deadThenBranch;            // deadCode_ was set on exit from "then"
    };

    struct BaseCompilePolicy
    {
        // The baseline compiler tracks values on a stack of its own -- it
        // needs to scan that stack for spilling -- and thus has no need
        // for the values maintained by the iterator.
        typedef Nothing Value;

        // The baseline compiler uses the iterator's control stack, attaching
        // its own control information.
        typedef Control ControlItem;
    };

    typedef OpIter<BaseCompilePolicy> BaseOpIter;

    // The baseline compiler will use OOL code more sparingly than
    // Baldr since our code is not high performance and frills like
    // code density and branch prediction friendliness will be less
    // important.

    class OutOfLineCode : public TempObject
    {
      private:
        NonAssertingLabel entry_;
        NonAssertingLabel rejoin_;
        uint32_t stackHeight_;

      public:
        OutOfLineCode() : stackHeight_(UINT32_MAX) {}

        Label* entry() { return &entry_; }
        Label* rejoin() { return &rejoin_; }

        void setStackHeight(uint32_t stackHeight) {
            MOZ_ASSERT(stackHeight_ == UINT32_MAX);
            stackHeight_ = stackHeight;
        }

        void bind(BaseStackFrame* fr, MacroAssembler* masm) {
            MOZ_ASSERT(stackHeight_ != UINT32_MAX);
            masm->bind(&entry_);
            fr->setStackHeight(stackHeight_);
        }

        // The generate() method must be careful about register use
        // because it will be invoked when there is a register
        // assignment in the BaseCompiler that does not correspond
        // to the available registers when the generated OOL code is
        // executed.  The register allocator *must not* be called.
        //
        // The best strategy is for the creator of the OOL object to
        // allocate all temps that the OOL code will need.
        //
        // Input, output, and temp registers are embedded in the OOL
        // object and are known to the code generator.
        //
        // Scratch registers are available to use in OOL code.
        //
        // All other registers must be explicitly saved and restored
        // by the OOL code before being used.

        virtual void generate(MacroAssembler* masm) = 0;
    };

    enum class LatentOp {
        None,
        Compare,
        Eqz
    };

    struct AccessCheck {
        AccessCheck()
          : omitBoundsCheck(false),
            omitAlignmentCheck(false),
            onlyPointerAlignment(false)
        {}

        // If `omitAlignmentCheck` is true then we need check neither the
        // pointer nor the offset.  Otherwise, if `onlyPointerAlignment` is true
        // then we need check only the pointer.  Otherwise, check the sum of
        // pointer and offset.

        bool omitBoundsCheck;
        bool omitAlignmentCheck;
        bool onlyPointerAlignment;
    };

    const ModuleEnvironment&    env_;
    BaseOpIter                  iter_;
    const FuncCompileInput&     func_;
    size_t                      lastReadCallSite_;
    TempAllocator&              alloc_;
    const ValTypeVector&        locals_;         // Types of parameters and locals
    bool                        deadCode_;       // Flag indicating we should decode & discard the opcode
    bool                        debugEnabled_;
    BCESet                      bceSafe_;        // Locals that have been bounds checked and not updated since
    ValTypeVector               SigD_;
    ValTypeVector               SigF_;
    MIRTypeVector               SigP_;
    MIRTypeVector               SigPI_;
    MIRTypeVector               SigPII_;
    MIRTypeVector               SigPIIL_;
    MIRTypeVector               SigPILL_;
    NonAssertingLabel           returnLabel_;
    CompileMode                 mode_;

    LatentOp                    latentOp_;       // Latent operation for branch (seen next)
    ValType                     latentType_;     // Operand type, if latentOp_ is true
    Assembler::Condition        latentIntCmp_;   // Comparison operator, if latentOp_ == Compare, int types
    Assembler::DoubleCondition  latentDoubleCmp_;// Comparison operator, if latentOp_ == Compare, float types

    FuncOffsets                 offsets_;
    MacroAssembler&             masm;            // No '_' suffix - too tedious...
    BaseRegAlloc                ra;              // Ditto
    BaseStackFrame              fr;

    BaseStackFrame::LocalVector localInfo_;
    Vector<OutOfLineCode*, 8, SystemAllocPolicy> outOfLine_;

    // On specific platforms we sometimes need to use specific registers.

    SpecificRegs                specific;

    // The join registers are used to carry values out of blocks.
    // JoinRegI32 and joinRegI64 must overlap: emitBrIf and
    // emitBrTable assume that.

    RegI32 joinRegI32;
    RegI64 joinRegI64;
    RegF32 joinRegF32;
    RegF64 joinRegF64;

    // There are more members scattered throughout.

  public:
    BaseCompiler(const ModuleEnvironment& env,
                 Decoder& decoder,
                 const FuncCompileInput& input,
                 const ValTypeVector& locals,
                 bool debugEnabled,
                 TempAllocator* alloc,
                 MacroAssembler* masm,
                 CompileMode mode);

    MOZ_MUST_USE bool init();

    FuncOffsets finish();

    MOZ_MUST_USE bool emitFunction();
    void emitInitStackLocals();

    const SigWithId& sig() const { return *env_.funcSigs[func_.index]; }

    // Used by some of the ScratchRegister implementations.
    operator MacroAssembler&() const { return masm; }
    operator BaseRegAlloc&() { return ra; }

  private:

    ////////////////////////////////////////////////////////////
    //
    // Out of line code management.

    MOZ_MUST_USE OutOfLineCode* addOutOfLineCode(OutOfLineCode* ool) {
        if (!ool || !outOfLine_.append(ool))
            return nullptr;
        ool->setStackHeight(fr.stackHeight());
        return ool;
    }

    MOZ_MUST_USE bool generateOutOfLineCode() {
        for (uint32_t i = 0; i < outOfLine_.length(); i++) {
            OutOfLineCode* ool = outOfLine_[i];
            ool->bind(&fr, &masm);
            ool->generate(&masm);
        }

        return !masm.oom();
    }

    // Utility.

    const Local& localFromSlot(uint32_t slot, MIRType type) {
        MOZ_ASSERT(localInfo_[slot].type == type);
        return localInfo_[slot];
    }

    ////////////////////////////////////////////////////////////
    //
    // High-level register management.

    bool isAvailableI32(RegI32 r) { return ra.isAvailableI32(r); }
    bool isAvailableI64(RegI64 r) { return ra.isAvailableI64(r); }
    bool isAvailableF32(RegF32 r) { return ra.isAvailableF32(r); }
    bool isAvailableF64(RegF64 r) { return ra.isAvailableF64(r); }

    MOZ_MUST_USE RegI32 needI32() { return ra.needI32(); }
    MOZ_MUST_USE RegI64 needI64() { return ra.needI64(); }
    MOZ_MUST_USE RegF32 needF32() { return ra.needF32(); }
    MOZ_MUST_USE RegF64 needF64() { return ra.needF64(); }

    void needI32(RegI32 specific) { ra.needI32(specific); }
    void needI64(RegI64 specific) { ra.needI64(specific); }
    void needF32(RegF32 specific) { ra.needF32(specific); }
    void needF64(RegF64 specific) { ra.needF64(specific); }

#if defined(JS_CODEGEN_ARM)
    MOZ_MUST_USE RegI64 needI64Pair() { return ra.needI64Pair(); }
#endif

    void freeI32(RegI32 r) { ra.freeI32(r); }
    void freeI64(RegI64 r) { ra.freeI64(r); }
    void freeF32(RegF32 r) { ra.freeF32(r); }
    void freeF64(RegF64 r) { ra.freeF64(r); }

    void freeI64Except(RegI64 r, RegI32 except) {
#ifdef JS_PUNBOX64
        MOZ_ASSERT(r.reg == except);
#else
        MOZ_ASSERT(r.high == except || r.low == except);
        freeI64(r);
        needI32(except);
#endif
    }

    void maybeFreeI32(RegI32 r) {
        if (r.isValid())
            freeI32(r);
    }

    void maybeFreeI64(RegI64 r) {
        if (r.isValid())
            freeI64(r);
    }

    void maybeFreeF64(RegF64 r) {
        if (r.isValid())
            freeF64(r);
    }

    void needI32NoSync(RegI32 r) {
        MOZ_ASSERT(isAvailableI32(r));
        needI32(r);
    }

    // TODO / OPTIMIZE: need2xI32() can be optimized along with needI32()
    // to avoid sync(). (Bug 1316802)

    void need2xI32(RegI32 r0, RegI32 r1) {
        needI32(r0);
        needI32(r1);
    }

    void need2xI64(RegI64 r0, RegI64 r1) {
        needI64(r0);
        needI64(r1);
    }

    RegI32 fromI64(RegI64 r) {
        return RegI32(lowPart(r));
    }

#ifdef JS_PUNBOX64
    RegI64 fromI32(RegI32 r) {
        return RegI64(Register64(r));
    }
#endif

    RegI64 widenI32(RegI32 r) {
        MOZ_ASSERT(!isAvailableI32(r));
#ifdef JS_PUNBOX64
        return fromI32(r);
#else
        RegI32 high = needI32();
        return RegI64(Register64(high, r));
#endif
    }

    RegI32 narrowI64(RegI64 r) {
#ifdef JS_PUNBOX64
        return RegI32(r.reg);
#else
        freeI32(RegI32(r.high));
        return RegI32(r.low);
#endif
    }

    RegI32 lowPart(RegI64 r) {
#ifdef JS_PUNBOX64
        return RegI32(r.reg);
#else
        return RegI32(r.low);
#endif
    }

    RegI32 maybeHighPart(RegI64 r) {
#ifdef JS_PUNBOX64
        return RegI32::Invalid();
#else
        return RegI32(r.high);
#endif
    }

    void maybeClearHighPart(RegI64 r) {
#if !defined(JS_PUNBOX64)
        moveImm32(0, RegI32(r.high));
#endif
    }

    void moveI32(RegI32 src, RegI32 dest) {
        if (src != dest)
            masm.move32(src, dest);
    }

    void moveI64(RegI64 src, RegI64 dest) {
        if (src != dest)
            masm.move64(src, dest);
    }

    void moveF64(RegF64 src, RegF64 dest) {
        if (src != dest)
            masm.moveDouble(src, dest);
    }

    void moveF32(RegF32 src, RegF32 dest) {
        if (src != dest)
            masm.moveFloat32(src, dest);
    }

    void maybeReserveJoinRegI(ExprType type) {
        if (type == ExprType::I32)
            needI32(joinRegI32);
        else if (type == ExprType::I64)
            needI64(joinRegI64);
    }

    void maybeUnreserveJoinRegI(ExprType type) {
        if (type == ExprType::I32)
            freeI32(joinRegI32);
        else if (type == ExprType::I64)
            freeI64(joinRegI64);
    }

    void maybeReserveJoinReg(ExprType type) {
        switch (type) {
          case ExprType::I32:
            needI32(joinRegI32);
            break;
          case ExprType::I64:
            needI64(joinRegI64);
            break;
          case ExprType::F32:
            needF32(joinRegF32);
            break;
          case ExprType::F64:
            needF64(joinRegF64);
            break;
          default:
            break;
        }
    }

    void maybeUnreserveJoinReg(ExprType type) {
        switch (type) {
          case ExprType::I32:
            freeI32(joinRegI32);
            break;
          case ExprType::I64:
            freeI64(joinRegI64);
            break;
          case ExprType::F32:
            freeF32(joinRegF32);
            break;
          case ExprType::F64:
            freeF64(joinRegF64);
            break;
          default:
            break;
        }
    }

    ////////////////////////////////////////////////////////////
    //
    // Value stack and spilling.
    //
    // The value stack facilitates some on-the-fly register allocation
    // and immediate-constant use.  It tracks constants, latent
    // references to locals, register contents, and values on the CPU
    // stack.
    //
    // The stack can be flushed to memory using sync().  This is handy
    // to avoid problems with control flow and messy register usage
    // patterns.

    struct Stk
    {
        enum Kind
        {
            // The Mem opcodes are all clustered at the beginning to
            // allow for a quick test within sync().
            MemI32,               // 32-bit integer stack value ("offs")
            MemI64,               // 64-bit integer stack value ("offs")
            MemF32,               // 32-bit floating stack value ("offs")
            MemF64,               // 64-bit floating stack value ("offs")

            // The Local opcodes follow the Mem opcodes for a similar
            // quick test within hasLocal().
            LocalI32,             // Local int32 var ("slot")
            LocalI64,             // Local int64 var ("slot")
            LocalF32,             // Local float32 var ("slot")
            LocalF64,             // Local double var ("slot")

            RegisterI32,          // 32-bit integer register ("i32reg")
            RegisterI64,          // 64-bit integer register ("i64reg")
            RegisterF32,          // 32-bit floating register ("f32reg")
            RegisterF64,          // 64-bit floating register ("f64reg")

            ConstI32,             // 32-bit integer constant ("i32val")
            ConstI64,             // 64-bit integer constant ("i64val")
            ConstF32,             // 32-bit floating constant ("f32val")
            ConstF64,             // 64-bit floating constant ("f64val")

            None                  // Uninitialized or void
        };

        Kind kind_;

        static const Kind MemLast = MemF64;
        static const Kind LocalLast = LocalF64;

        union {
            RegI32   i32reg_;
            RegI64   i64reg_;
            RegF32   f32reg_;
            RegF64   f64reg_;
            int32_t  i32val_;
            int64_t  i64val_;
            float    f32val_;
            double   f64val_;
            uint32_t slot_;
            uint32_t offs_;
        };

        Stk() { kind_ = None; }

        Kind kind() const { return kind_; }
        bool isMem() const { return kind_ <= MemLast; }

        RegI32   i32reg() const { MOZ_ASSERT(kind_ == RegisterI32); return i32reg_; }
        RegI64   i64reg() const { MOZ_ASSERT(kind_ == RegisterI64); return i64reg_; }
        RegF32   f32reg() const { MOZ_ASSERT(kind_ == RegisterF32); return f32reg_; }
        RegF64   f64reg() const { MOZ_ASSERT(kind_ == RegisterF64); return f64reg_; }
        int32_t  i32val() const { MOZ_ASSERT(kind_ == ConstI32); return i32val_; }
        int64_t  i64val() const { MOZ_ASSERT(kind_ == ConstI64); return i64val_; }
        // For these two, use an out-param instead of simply returning, to
        // use the normal stack and not the x87 FP stack (which has effect on
        // NaNs with the signaling bit set).
        void     f32val(float* out) const { MOZ_ASSERT(kind_ == ConstF32); *out = f32val_; }
        void     f64val(double* out) const { MOZ_ASSERT(kind_ == ConstF64); *out = f64val_; }
        uint32_t slot() const { MOZ_ASSERT(kind_ > MemLast && kind_ <= LocalLast); return slot_; }
        uint32_t offs() const { MOZ_ASSERT(isMem()); return offs_; }

        void setI32Reg(RegI32 r) { kind_ = RegisterI32; i32reg_ = r; }
        void setI64Reg(RegI64 r) { kind_ = RegisterI64; i64reg_ = r; }
        void setF32Reg(RegF32 r) { kind_ = RegisterF32; f32reg_ = r; }
        void setF64Reg(RegF64 r) { kind_ = RegisterF64; f64reg_ = r; }
        void setI32Val(int32_t v) { kind_ = ConstI32; i32val_ = v; }
        void setI64Val(int64_t v) { kind_ = ConstI64; i64val_ = v; }
        void setF32Val(float v) { kind_ = ConstF32; f32val_ = v; }
        void setF64Val(double v) { kind_ = ConstF64; f64val_ = v; }
        void setSlot(Kind k, uint32_t v) { MOZ_ASSERT(k > MemLast && k <= LocalLast); kind_ = k; slot_ = v; }
        void setOffs(Kind k, uint32_t v) { MOZ_ASSERT(k <= MemLast); kind_ = k; offs_ = v; }
    };

    Vector<Stk, 8, SystemAllocPolicy> stk_;

    Stk& push() {
        stk_.infallibleEmplaceBack(Stk());
        return stk_.back();
    }

    void loadConstI32(Stk& src, RegI32 dest) {
        moveImm32(src.i32val(), dest);
    }

    void loadMemI32(Stk& src, RegI32 dest) {
        fr.loadStackI32(src.offs(), dest);
    }

    void loadLocalI32(Stk& src, RegI32 dest) {
        fr.loadLocalI32(localFromSlot(src.slot(), MIRType::Int32), dest);
    }

    void loadRegisterI32(Stk& src, RegI32 dest) {
        moveI32(src.i32reg(), dest);
    }

    void loadConstI64(Stk &src, RegI64 dest) {
        moveImm64(src.i64val(), dest);
    }

    void loadMemI64(Stk& src, RegI64 dest) {
        fr.loadStackI64(src.offs(), dest);
    }

    void loadLocalI64(Stk& src, RegI64 dest) {
        fr.loadLocalI64(localFromSlot(src.slot(), MIRType::Int64), dest);
    }

    void loadRegisterI64(Stk& src, RegI64 dest) {
        moveI64(src.i64reg(), dest);
    }

    void loadConstF64(Stk &src, RegF64 dest) {
        double d;
        src.f64val(&d);
        masm.loadConstantDouble(d, dest);
    }

    void loadMemF64(Stk& src, RegF64 dest) {
        fr.loadStackF64(src.offs(), dest);
    }

    void loadLocalF64(Stk& src, RegF64 dest) {
        fr.loadLocalF64(localFromSlot(src.slot(), MIRType::Double), dest);
    }

    void loadRegisterF64(Stk& src, RegF64 dest) {
        moveF64(src.f64reg(), dest);
    }

    void loadConstF32(Stk &src, RegF32 dest) {
        float f;
        src.f32val(&f);
        masm.loadConstantFloat32(f, dest);
    }

    void loadMemF32(Stk& src, RegF32 dest) {
        fr.loadStackF32(src.offs(), dest);
    }

    void loadLocalF32(Stk& src, RegF32 dest) {
        fr.loadLocalF32(localFromSlot(src.slot(), MIRType::Float32), dest);
    }

    void loadRegisterF32(Stk& src, RegF32 dest) {
        moveF32(src.f32reg(), dest);
    }

    void loadI32(Stk& src, RegI32 dest) {
        switch (src.kind()) {
          case Stk::ConstI32:
            loadConstI32(src, dest);
            break;
          case Stk::MemI32:
            loadMemI32(src, dest);
            break;
          case Stk::LocalI32:
            loadLocalI32(src, dest);
            break;
          case Stk::RegisterI32:
            loadRegisterI32(src, dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: Expected I32 on stack");
        }
    }

    void loadI64(Stk& src, RegI64 dest) {
        switch (src.kind()) {
          case Stk::ConstI64:
            loadConstI64(src, dest);
            break;
          case Stk::MemI64:
            loadMemI64(src, dest);
            break;
          case Stk::LocalI64:
            loadLocalI64(src, dest);
            break;
          case Stk::RegisterI64:
            loadRegisterI64(src, dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: Expected I64 on stack");
        }
    }

#if !defined(JS_PUNBOX64)
    void loadI64Low(Stk& src, RegI32 dest) {
        switch (src.kind()) {
          case Stk::ConstI64:
            moveImm32(int32_t(src.i64val()), dest);
            break;
          case Stk::MemI64:
            fr.loadStackI64Low(src.offs(), dest);
            break;
          case Stk::LocalI64:
            fr.loadLocalI64Low(localFromSlot(src.slot(), MIRType::Int64), dest);
            break;
          case Stk::RegisterI64:
            moveI32(RegI32(src.i64reg().low), dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: Expected I64 on stack");
        }
    }

    void loadI64High(Stk& src, RegI32 dest) {
        switch (src.kind()) {
          case Stk::ConstI64:
            moveImm32(int32_t(src.i64val() >> 32), dest);
            break;
          case Stk::MemI64:
            fr.loadStackI64High(src.offs(), dest);
            break;
          case Stk::LocalI64:
            fr.loadLocalI64High(localFromSlot(src.slot(), MIRType::Int64), dest);
            break;
          case Stk::RegisterI64:
            moveI32(RegI32(src.i64reg().high), dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: Expected I64 on stack");
        }
    }
#endif

    void loadF64(Stk& src, RegF64 dest) {
        switch (src.kind()) {
          case Stk::ConstF64:
            loadConstF64(src, dest);
            break;
          case Stk::MemF64:
            loadMemF64(src, dest);
            break;
          case Stk::LocalF64:
            loadLocalF64(src, dest);
            break;
          case Stk::RegisterF64:
            loadRegisterF64(src, dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: expected F64 on stack");
        }
    }

    void loadF32(Stk& src, RegF32 dest) {
        switch (src.kind()) {
          case Stk::ConstF32:
            loadConstF32(src, dest);
            break;
          case Stk::MemF32:
            loadMemF32(src, dest);
            break;
          case Stk::LocalF32:
            loadLocalF32(src, dest);
            break;
          case Stk::RegisterF32:
            loadRegisterF32(src, dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: expected F32 on stack");
        }
    }

    // Flush all local and register value stack elements to memory.
    //
    // TODO / OPTIMIZE: As this is fairly expensive and causes worse
    // code to be emitted subsequently, it is useful to avoid calling
    // it.  (Bug 1316802)
    //
    // Some optimization has been done already.  Remaining
    // opportunities:
    //
    //  - It would be interesting to see if we can specialize it
    //    before calls with particularly simple signatures, or where
    //    we can do parallel assignment of register arguments, or
    //    similar.  See notes in emitCall().
    //
    //  - Operations that need specific registers: multiply, quotient,
    //    remainder, will tend to sync because the registers we need
    //    will tend to be allocated.  We may be able to avoid that by
    //    prioritizing registers differently (takeLast instead of
    //    takeFirst) but we may also be able to allocate an unused
    //    register on demand to free up one we need, thus avoiding the
    //    sync.  That type of fix would go into needI32().

    void sync() final {
        size_t start = 0;
        size_t lim = stk_.length();

        for (size_t i = lim; i > 0; i--) {
            // Memory opcodes are first in the enum, single check against MemLast is fine.
            if (stk_[i - 1].kind() <= Stk::MemLast) {
                start = i;
                break;
            }
        }

        for (size_t i = start; i < lim; i++) {
            Stk& v = stk_[i];
            switch (v.kind()) {
              case Stk::LocalI32: {
                ScratchI32 scratch(*this);
                loadLocalI32(v, scratch);
                uint32_t offs = fr.pushPtr(scratch);
                v.setOffs(Stk::MemI32, offs);
                break;
              }
              case Stk::RegisterI32: {
                uint32_t offs = fr.pushPtr(v.i32reg());
                freeI32(v.i32reg());
                v.setOffs(Stk::MemI32, offs);
                break;
              }
              case Stk::LocalI64: {
                ScratchI32 scratch(*this);
#ifdef JS_PUNBOX64
                loadI64(v, fromI32(scratch));
                uint32_t offs = fr.pushPtr(scratch);
#else
                fr.loadLocalI64High(localFromSlot(v.slot(), MIRType::Int64), scratch);
                fr.pushPtr(scratch);
                fr.loadLocalI64Low(localFromSlot(v.slot(), MIRType::Int64), scratch);
                uint32_t offs = fr.pushPtr(scratch);
#endif
                v.setOffs(Stk::MemI64, offs);
                break;
              }
              case Stk::RegisterI64: {
#ifdef JS_PUNBOX64
                uint32_t offs = fr.pushPtr(v.i64reg().reg);
                freeI64(v.i64reg());
#else
                fr.pushPtr(v.i64reg().high);
                uint32_t offs = fr.pushPtr(v.i64reg().low);
                freeI64(v.i64reg());
#endif
                v.setOffs(Stk::MemI64, offs);
                break;
              }
              case Stk::LocalF64: {
                ScratchF64 scratch(*this);
                loadF64(v, scratch);
                uint32_t offs = fr.pushDouble(scratch);
                v.setOffs(Stk::MemF64, offs);
                break;
              }
              case Stk::RegisterF64: {
                uint32_t offs = fr.pushDouble(v.f64reg());
                freeF64(v.f64reg());
                v.setOffs(Stk::MemF64, offs);
                break;
              }
              case Stk::LocalF32: {
                ScratchF32 scratch(*this);
                loadF32(v, scratch);
                uint32_t offs = fr.pushFloat32(scratch);
                v.setOffs(Stk::MemF32, offs);
                break;
              }
              case Stk::RegisterF32: {
                uint32_t offs = fr.pushFloat32(v.f32reg());
                freeF32(v.f32reg());
                v.setOffs(Stk::MemF32, offs);
                break;
              }
              default: {
                break;
              }
            }
        }
    }

    // This is an optimization used to avoid calling sync() for
    // setLocal(): if the local does not exist unresolved on the stack
    // then we can skip the sync.

    bool hasLocal(uint32_t slot) {
        for (size_t i = stk_.length(); i > 0; i--) {
            // Memory opcodes are first in the enum, single check against MemLast is fine.
            Stk::Kind kind = stk_[i-1].kind();
            if (kind <= Stk::MemLast)
                return false;

            // Local opcodes follow memory opcodes in the enum, single check against
            // LocalLast is sufficient.
            if (kind <= Stk::LocalLast && stk_[i-1].slot() == slot)
                return true;
        }
        return false;
    }

    void syncLocal(uint32_t slot) {
        if (hasLocal(slot))
            sync();            // TODO / OPTIMIZE: Improve this?  (Bug 1316817)
    }

    // Push the register r onto the stack.

    void pushI32(RegI32 r) {
        MOZ_ASSERT(!isAvailableI32(r));
        Stk& x = push();
        x.setI32Reg(r);
    }

    void pushI64(RegI64 r) {
        MOZ_ASSERT(!isAvailableI64(r));
        Stk& x = push();
        x.setI64Reg(r);
    }

    void pushF64(RegF64 r) {
        MOZ_ASSERT(!isAvailableF64(r));
        Stk& x = push();
        x.setF64Reg(r);
    }

    void pushF32(RegF32 r) {
        MOZ_ASSERT(!isAvailableF32(r));
        Stk& x = push();
        x.setF32Reg(r);
    }

    // Push the value onto the stack.

    void pushI32(int32_t v) {
        Stk& x = push();
        x.setI32Val(v);
    }

    void pushI64(int64_t v) {
        Stk& x = push();
        x.setI64Val(v);
    }

    void pushF64(double v) {
        Stk& x = push();
        x.setF64Val(v);
    }

    void pushF32(float v) {
        Stk& x = push();
        x.setF32Val(v);
    }

    // Push the local slot onto the stack.  The slot will not be read
    // here; it will be read when it is consumed, or when a side
    // effect to the slot forces its value to be saved.

    void pushLocalI32(uint32_t slot) {
        Stk& x = push();
        x.setSlot(Stk::LocalI32, slot);
    }

    void pushLocalI64(uint32_t slot) {
        Stk& x = push();
        x.setSlot(Stk::LocalI64, slot);
    }

    void pushLocalF64(uint32_t slot) {
        Stk& x = push();
        x.setSlot(Stk::LocalF64, slot);
    }

    void pushLocalF32(uint32_t slot) {
        Stk& x = push();
        x.setSlot(Stk::LocalF32, slot);
    }

    // Call only from other popI32() variants.
    // v must be the stack top.  May pop the CPU stack.

    void popI32(Stk& v, RegI32 dest) {
        MOZ_ASSERT(&v == &stk_.back());
        switch (v.kind()) {
          case Stk::ConstI32:
            loadConstI32(v, dest);
            break;
          case Stk::LocalI32:
            loadLocalI32(v, dest);
            break;
          case Stk::MemI32:
            fr.popPtr(dest);
            break;
          case Stk::RegisterI32:
            loadRegisterI32(v, dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: expected int on stack");
        }
    }

    MOZ_MUST_USE RegI32 popI32() {
        Stk& v = stk_.back();
        RegI32 r;
        if (v.kind() == Stk::RegisterI32)
            r = v.i32reg();
        else
            popI32(v, (r = needI32()));
        stk_.popBack();
        return r;
    }

    RegI32 popI32(RegI32 specific) {
        Stk& v = stk_.back();

        if (!(v.kind() == Stk::RegisterI32 && v.i32reg() == specific)) {
            needI32(specific);
            popI32(v, specific);
            if (v.kind() == Stk::RegisterI32)
                freeI32(v.i32reg());
        }

        stk_.popBack();
        return specific;
    }

    // Call only from other popI64() variants.
    // v must be the stack top.  May pop the CPU stack.

    void popI64(Stk& v, RegI64 dest) {
        MOZ_ASSERT(&v == &stk_.back());
        switch (v.kind()) {
          case Stk::ConstI64:
            loadConstI64(v, dest);
            break;
          case Stk::LocalI64:
            loadLocalI64(v, dest);
            break;
          case Stk::MemI64:
#ifdef JS_PUNBOX64
            fr.popPtr(dest.reg);
#else
            fr.popPtr(dest.low);
            fr.popPtr(dest.high);
#endif
            break;
          case Stk::RegisterI64:
            loadRegisterI64(v, dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: expected long on stack");
        }
    }

    MOZ_MUST_USE RegI64 popI64() {
        Stk& v = stk_.back();
        RegI64 r;
        if (v.kind() == Stk::RegisterI64)
            r = v.i64reg();
        else
            popI64(v, (r = needI64()));
        stk_.popBack();
        return r;
    }

    // Note, the stack top can be in one half of "specific" on 32-bit
    // systems.  We can optimize, but for simplicity, if the register
    // does not match exactly, then just force the stack top to memory
    // and then read it back in.

    RegI64 popI64(RegI64 specific) {
        Stk& v = stk_.back();

        if (!(v.kind() == Stk::RegisterI64 && v.i64reg() == specific)) {
            needI64(specific);
            popI64(v, specific);
            if (v.kind() == Stk::RegisterI64)
                freeI64(v.i64reg());
        }

        stk_.popBack();
        return specific;
    }

    // Call only from other popF64() variants.
    // v must be the stack top.  May pop the CPU stack.

    void popF64(Stk& v, RegF64 dest) {
        MOZ_ASSERT(&v == &stk_.back());
        switch (v.kind()) {
          case Stk::ConstF64:
            loadConstF64(v, dest);
            break;
          case Stk::LocalF64:
            loadLocalF64(v, dest);
            break;
          case Stk::MemF64:
            fr.popDouble(dest);
            break;
          case Stk::RegisterF64:
            loadRegisterF64(v, dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: expected double on stack");
        }
    }

    MOZ_MUST_USE RegF64 popF64() {
        Stk& v = stk_.back();
        RegF64 r;
        if (v.kind() == Stk::RegisterF64)
            r = v.f64reg();
        else
            popF64(v, (r = needF64()));
        stk_.popBack();
        return r;
    }

    RegF64 popF64(RegF64 specific) {
        Stk& v = stk_.back();

        if (!(v.kind() == Stk::RegisterF64 && v.f64reg() == specific)) {
            needF64(specific);
            popF64(v, specific);
            if (v.kind() == Stk::RegisterF64)
                freeF64(v.f64reg());
        }

        stk_.popBack();
        return specific;
    }

    // Call only from other popF32() variants.
    // v must be the stack top.  May pop the CPU stack.

    void popF32(Stk& v, RegF32 dest) {
        MOZ_ASSERT(&v == &stk_.back());
        switch (v.kind()) {
          case Stk::ConstF32:
            loadConstF32(v, dest);
            break;
          case Stk::LocalF32:
            loadLocalF32(v, dest);
            break;
          case Stk::MemF32:
            fr.popFloat32(dest);
            break;
          case Stk::RegisterF32:
            loadRegisterF32(v, dest);
            break;
          case Stk::None:
          default:
            MOZ_CRASH("Compiler bug: expected float on stack");
        }
    }

    MOZ_MUST_USE RegF32 popF32() {
        Stk& v = stk_.back();
        RegF32 r;
        if (v.kind() == Stk::RegisterF32)
            r = v.f32reg();
        else
            popF32(v, (r = needF32()));
        stk_.popBack();
        return r;
    }

    RegF32 popF32(RegF32 specific) {
        Stk& v = stk_.back();

        if (!(v.kind() == Stk::RegisterF32 && v.f32reg() == specific)) {
            needF32(specific);
            popF32(v, specific);
            if (v.kind() == Stk::RegisterF32)
                freeF32(v.f32reg());
        }

        stk_.popBack();
        return specific;
    }

    MOZ_MUST_USE bool popConstI32(int32_t* c) {
        Stk& v = stk_.back();
        if (v.kind() != Stk::ConstI32)
            return false;
        *c = v.i32val();
        stk_.popBack();
        return true;
    }

    MOZ_MUST_USE bool popConstI64(int64_t* c) {
        Stk& v = stk_.back();
        if (v.kind() != Stk::ConstI64)
            return false;
        *c = v.i64val();
        stk_.popBack();
        return true;
    }

    MOZ_MUST_USE bool peekConstI32(int32_t* c) {
        Stk& v = stk_.back();
        if (v.kind() != Stk::ConstI32)
            return false;
        *c = v.i32val();
        return true;
    }

    MOZ_MUST_USE bool peekConstI64(int64_t* c) {
        Stk& v = stk_.back();
        if (v.kind() != Stk::ConstI64)
            return false;
        *c = v.i64val();
        return true;
    }

    MOZ_MUST_USE bool popConstPositivePowerOfTwoI32(int32_t* c,
                                                    uint_fast8_t* power,
                                                    int32_t cutoff)
    {
        Stk& v = stk_.back();
        if (v.kind() != Stk::ConstI32)
            return false;
        *c = v.i32val();
        if (*c <= cutoff || !IsPowerOfTwo(static_cast<uint32_t>(*c)))
            return false;
        *power = FloorLog2(*c);
        stk_.popBack();
        return true;
    }

    MOZ_MUST_USE bool popConstPositivePowerOfTwoI64(int64_t* c,
                                                    uint_fast8_t* power,
                                                    int64_t cutoff)
    {
        Stk& v = stk_.back();
        if (v.kind() != Stk::ConstI64)
            return false;
        *c = v.i64val();
        if (*c <= cutoff || !IsPowerOfTwo(static_cast<uint64_t>(*c)))
            return false;
        *power = FloorLog2(*c);
        stk_.popBack();
        return true;
    }

    MOZ_MUST_USE bool peekLocalI32(uint32_t* local) {
        Stk& v = stk_.back();
        if (v.kind() != Stk::LocalI32)
            return false;
        *local = v.slot();
        return true;
    }

    // TODO / OPTIMIZE (Bug 1316818): At the moment we use ReturnReg
    // for JoinReg.  It is possible other choices would lead to better
    // register allocation, as ReturnReg is often first in the
    // register set and will be heavily wanted by the register
    // allocator that uses takeFirst().
    //
    // Obvious options:
    //  - pick a register at the back of the register set
    //  - pick a random register per block (different blocks have
    //    different join regs)
    //
    // On the other hand, we sync() before every block and only the
    // JoinReg is live out of the block.  But on the way out, we
    // currently pop the JoinReg before freeing regs to be discarded,
    // so there is a real risk of some pointless shuffling there.  If
    // we instead integrate the popping of the join reg into the
    // popping of the stack we can just use the JoinReg as it will
    // become available in that process.

    MOZ_MUST_USE Maybe<AnyReg> popJoinRegUnlessVoid(ExprType type) {
        switch (type) {
          case ExprType::Void: {
            return Nothing();
          }
          case ExprType::I32: {
            DebugOnly<Stk::Kind> k(stk_.back().kind());
            MOZ_ASSERT(k == Stk::RegisterI32 || k == Stk::ConstI32 || k == Stk::MemI32 ||
                       k == Stk::LocalI32);
            return Some(AnyReg(popI32(joinRegI32)));
          }
          case ExprType::I64: {
            DebugOnly<Stk::Kind> k(stk_.back().kind());
            MOZ_ASSERT(k == Stk::RegisterI64 || k == Stk::ConstI64 || k == Stk::MemI64 ||
                       k == Stk::LocalI64);
            return Some(AnyReg(popI64(joinRegI64)));
          }
          case ExprType::F64: {
            DebugOnly<Stk::Kind> k(stk_.back().kind());
            MOZ_ASSERT(k == Stk::RegisterF64 || k == Stk::ConstF64 || k == Stk::MemF64 ||
                       k == Stk::LocalF64);
            return Some(AnyReg(popF64(joinRegF64)));
          }
          case ExprType::F32: {
            DebugOnly<Stk::Kind> k(stk_.back().kind());
            MOZ_ASSERT(k == Stk::RegisterF32 || k == Stk::ConstF32 || k == Stk::MemF32 ||
                       k == Stk::LocalF32);
            return Some(AnyReg(popF32(joinRegF32)));
          }
          default: {
            MOZ_CRASH("Compiler bug: unexpected expression type");
          }
        }
    }

    // If we ever start not sync-ing on entry to Block (but instead try to sync
    // lazily) then this may start asserting because it does not spill the
    // joinreg if the joinreg is already allocated.  Note, it *can't* spill the
    // joinreg in the contexts it's being used, so some other solution will need
    // to be found.

    MOZ_MUST_USE Maybe<AnyReg> captureJoinRegUnlessVoid(ExprType type) {
        switch (type) {
          case ExprType::I32:
            MOZ_ASSERT(isAvailableI32(joinRegI32));
            needI32(joinRegI32);
            return Some(AnyReg(joinRegI32));
          case ExprType::I64:
            MOZ_ASSERT(isAvailableI64(joinRegI64));
            needI64(joinRegI64);
            return Some(AnyReg(joinRegI64));
          case ExprType::F32:
            MOZ_ASSERT(isAvailableF32(joinRegF32));
            needF32(joinRegF32);
            return Some(AnyReg(joinRegF32));
          case ExprType::F64:
            MOZ_ASSERT(isAvailableF64(joinRegF64));
            needF64(joinRegF64);
            return Some(AnyReg(joinRegF64));
          case ExprType::Void:
            return Nothing();
          default:
            MOZ_CRASH("Compiler bug: unexpected type");
        }
    }

    void pushJoinRegUnlessVoid(const Maybe<AnyReg>& r) {
        if (!r)
            return;
        switch (r->tag) {
          case AnyReg::I32:
            pushI32(r->i32());
            break;
          case AnyReg::I64:
            pushI64(r->i64());
            break;
          case AnyReg::F64:
            pushF64(r->f64());
            break;
          case AnyReg::F32:
            pushF32(r->f32());
            break;
        }
    }

    void freeJoinRegUnlessVoid(const Maybe<AnyReg>& r) {
        if (!r)
            return;
        switch (r->tag) {
          case AnyReg::I32:
            freeI32(r->i32());
            break;
          case AnyReg::I64:
            freeI64(r->i64());
            break;
          case AnyReg::F64:
            freeF64(r->f64());
            break;
          case AnyReg::F32:
            freeF32(r->f32());
            break;
        }
    }

    // Return the amount of execution stack consumed by the top numval
    // values on the value stack.

    size_t stackConsumed(size_t numval) {
        size_t size = 0;
        MOZ_ASSERT(numval <= stk_.length());
        for (uint32_t i = stk_.length() - 1; numval > 0; numval--, i--) {
            Stk& v = stk_[i];
            switch (v.kind()) {
              case Stk::MemI32: size += BaseStackFrame::StackSizeOfPtr;    break;
              case Stk::MemI64: size += BaseStackFrame::StackSizeOfInt64;  break;
              case Stk::MemF64: size += BaseStackFrame::StackSizeOfDouble; break;
              case Stk::MemF32: size += BaseStackFrame::StackSizeOfFloat;  break;
              default: break;
            }
        }
        return size;
    }

    void popValueStackTo(uint32_t stackSize) {
        for (uint32_t i = stk_.length(); i > stackSize; i--) {
            Stk& v = stk_[i-1];
            switch (v.kind()) {
              case Stk::RegisterI32:
                freeI32(v.i32reg());
                break;
              case Stk::RegisterI64:
                freeI64(v.i64reg());
                break;
              case Stk::RegisterF64:
                freeF64(v.f64reg());
                break;
              case Stk::RegisterF32:
                freeF32(v.f32reg());
                break;
              default:
                break;
            }
        }
        stk_.shrinkTo(stackSize);
    }

    void popValueStackBy(uint32_t items) {
        popValueStackTo(stk_.length() - items);
    }

    void dropValue() {
        if (peek(0).isMem())
            fr.popBytes(stackConsumed(1));
        popValueStackBy(1);
    }

    // Peek at the stack, for calls.

    Stk& peek(uint32_t relativeDepth) {
        return stk_[stk_.length()-1-relativeDepth];
    }

#ifdef DEBUG
    // Check that we're not leaking registers by comparing the
    // state of the stack + available registers with the set of
    // all available registers.

    // Call this between opcodes.
    void performRegisterLeakCheck() {
        BaseRegAlloc::LeakCheck check(ra);
        for (size_t i = 0 ; i < stk_.length() ; i++) {
            Stk& item = stk_[i];
            switch (item.kind_) {
              case Stk::RegisterI32:
                check.addKnownI32(item.i32reg());
                break;
              case Stk::RegisterI64:
                check.addKnownI64(item.i64reg());
                break;
              case Stk::RegisterF32:
                check.addKnownF32(item.f32reg());
                break;
              case Stk::RegisterF64:
                check.addKnownF64(item.f64reg());
                break;
              default:
                break;
            }
        }
    }
#endif

    ////////////////////////////////////////////////////////////
    //
    // Control stack

    void initControl(Control& item)
    {
        // Make sure the constructor was run properly
        MOZ_ASSERT(item.stackHeight == UINT32_MAX && item.stackSize == UINT32_MAX);

        item.stackHeight = fr.stackHeight();
        item.stackSize = stk_.length();
        item.deadOnArrival = deadCode_;
        item.bceSafeOnEntry = bceSafe_;
    }

    Control& controlItem() {
        return iter_.controlItem();
    }

    Control& controlItem(uint32_t relativeDepth) {
        return iter_.controlItem(relativeDepth);
    }

    Control& controlOutermost() {
        return iter_.controlOutermost();
    }

    ////////////////////////////////////////////////////////////
    //
    // Labels

    void insertBreakablePoint(CallSiteDesc::Kind kind) {
        // The debug trap exit requires WasmTlsReg be loaded. However, since we
        // are emitting millions of these breakable points inline, we push this
        // loading of TLS into the FarJumpIsland created by linkCallSites.
        masm.nopPatchableToCall(CallSiteDesc(iter_.lastOpcodeOffset(), kind));
    }

    //////////////////////////////////////////////////////////////////////
    //
    // Function prologue and epilogue.

    void beginFunction() {
        JitSpew(JitSpew_Codegen, "# Emitting wasm baseline code");

        // We are unconditionally checking for overflow in fr.allocStack(), so
        // pass IsLeaf = true to avoid a second check in the prologue.
        IsLeaf isLeaf = true;
        SigIdDesc sigId = env_.funcSigs[func_.index]->id;
        BytecodeOffset trapOffset(func_.lineOrBytecode);
        GenerateFunctionPrologue(masm, fr.initialSize(), isLeaf, sigId, trapOffset, &offsets_,
                                 mode_ == CompileMode::Tier1 ? Some(func_.index) : Nothing());

        fr.endFunctionPrologue();

        if (debugEnabled_) {
            // Initialize funcIndex and flag fields of DebugFrame.
            size_t debugFrame = masm.framePushed() - DebugFrame::offsetOfFrame();
            masm.store32(Imm32(func_.index),
                         Address(masm.getStackPointer(), debugFrame + DebugFrame::offsetOfFuncIndex()));
            masm.storePtr(ImmWord(0),
                          Address(masm.getStackPointer(), debugFrame + DebugFrame::offsetOfFlagsWord()));
        }

        fr.allocStack(ABINonArgReg0, trapOffset);

        // Copy arguments from registers to stack.

        const ValTypeVector& args = sig().args();

        for (ABIArgIter<const ValTypeVector> i(args); !i.done(); i++) {
            Local& l = localInfo_[i.index()];
            switch (i.mirType()) {
              case MIRType::Int32:
                if (i->argInRegister())
                    fr.storeLocalI32(RegI32(i->gpr()), l);
                break;
              case MIRType::Int64:
                if (i->argInRegister())
                    fr.storeLocalI64(RegI64(i->gpr64()), l);
                break;
              case MIRType::Double:
                if (i->argInRegister())
                    fr.storeLocalF64(RegF64(i->fpu()), l);
                break;
              case MIRType::Float32:
                if (i->argInRegister())
                    fr.storeLocalF32(RegF32(i->fpu()), l);
                break;
              default:
                MOZ_CRASH("Function argument type");
            }
        }

        fr.zeroLocals(ra);

        if (debugEnabled_)
            insertBreakablePoint(CallSiteDesc::EnterFrame);
    }

    void saveResult() {
        MOZ_ASSERT(debugEnabled_);
        size_t debugFrameOffset = masm.framePushed() - DebugFrame::offsetOfFrame();
        Address resultsAddress(masm.getStackPointer(), debugFrameOffset + DebugFrame::offsetOfResults());
        switch (sig().ret()) {
          case ExprType::Void:
            break;
          case ExprType::I32:
            masm.store32(RegI32(ReturnReg), resultsAddress);
            break;
          case ExprType::I64:
            masm.store64(RegI64(ReturnReg64), resultsAddress);
            break;
          case ExprType::F64:
            masm.storeDouble(RegF64(ReturnDoubleReg), resultsAddress);
            break;
          case ExprType::F32:
            masm.storeFloat32(RegF32(ReturnFloat32Reg), resultsAddress);
            break;
          default:
            MOZ_CRASH("Function return type");
        }
    }

    void restoreResult() {
        MOZ_ASSERT(debugEnabled_);
        size_t debugFrameOffset = masm.framePushed() - DebugFrame::offsetOfFrame();
        Address resultsAddress(masm.getStackPointer(), debugFrameOffset + DebugFrame::offsetOfResults());
        switch (sig().ret()) {
          case ExprType::Void:
            break;
          case ExprType::I32:
            masm.load32(resultsAddress, RegI32(ReturnReg));
            break;
          case ExprType::I64:
            masm.load64(resultsAddress, RegI64(ReturnReg64));
            break;
          case ExprType::F64:
            masm.loadDouble(resultsAddress, RegF64(ReturnDoubleReg));
            break;
          case ExprType::F32:
            masm.loadFloat32(resultsAddress, RegF32(ReturnFloat32Reg));
            break;
          default:
            MOZ_CRASH("Function return type");
        }
    }

    bool endFunction() {
        // Always branch to returnLabel_.
        masm.breakpoint();

        // Patch the add in the prologue so that it checks against the correct
        // frame size. Flush the constant pool in case it needs to be patched.
        masm.flush();

        // Precondition for patching.
        if (masm.oom())
            return false;

        fr.patchAllocStack();

        masm.bind(&returnLabel_);

        if (debugEnabled_) {
            // Store and reload the return value from DebugFrame::return so that
            // it can be clobbered, and/or modified by the debug trap.
            saveResult();
            insertBreakablePoint(CallSiteDesc::Breakpoint);
            insertBreakablePoint(CallSiteDesc::LeaveFrame);
            restoreResult();
        }

        GenerateFunctionEpilogue(masm, fr.initialSize(), &offsets_);

#if defined(JS_ION_PERF)
        // FIXME - profiling code missing.  No bug for this.

        // Note the end of the inline code and start of the OOL code.
        //gen->perfSpewer().noteEndInlineCode(masm);
#endif

        if (!generateOutOfLineCode())
            return false;

        masm.wasmEmitOldTrapOutOfLineCode();

        offsets_.end = masm.currentOffset();

        if (!fr.checkStackHeight())
            return false;

        return !masm.oom();
    }

    //////////////////////////////////////////////////////////////////////
    //
    // Calls.

    struct FunctionCall
    {
        explicit FunctionCall(uint32_t lineOrBytecode)
          : lineOrBytecode(lineOrBytecode),
            reloadMachineStateAfter(false),
            usesSystemAbi(false),
#ifdef JS_CODEGEN_ARM
            hardFP(true),
#endif
            frameAlignAdjustment(0),
            stackArgAreaSize(0)
        {}

        uint32_t lineOrBytecode;
        ABIArgGenerator abi;
        bool reloadMachineStateAfter;
        bool usesSystemAbi;
#ifdef JS_CODEGEN_ARM
        bool hardFP;
#endif
        size_t frameAlignAdjustment;
        size_t stackArgAreaSize;
    };

    void beginCall(FunctionCall& call, UseABI useABI, InterModule interModule)
    {
        call.reloadMachineStateAfter = interModule == InterModule::True || useABI == UseABI::System;
        call.usesSystemAbi = useABI == UseABI::System;

        if (call.usesSystemAbi) {
            // Call-outs need to use the appropriate system ABI.
#if defined(JS_CODEGEN_ARM)
# if defined(JS_SIMULATOR_ARM)
            call.hardFP = UseHardFpABI();
# elif defined(JS_CODEGEN_ARM_HARDFP)
            call.hardFP = true;
# else
            call.hardFP = false;
# endif
            call.abi.setUseHardFp(call.hardFP);
#elif defined(JS_CODEGEN_MIPS32)
            call.abi.enforceO32ABI();
#endif
        }

        // Use masm.framePushed() because the value we want here does not depend
        // on the height of the frame's stack area, but the actual size of the
        // allocated frame.
        call.frameAlignAdjustment = ComputeByteAlignment(masm.framePushed() + sizeof(Frame),
                                                         JitStackAlignment);
    }

    void endCall(FunctionCall& call, size_t stackSpace)
    {
        size_t adjustment = call.stackArgAreaSize + call.frameAlignAdjustment;
        fr.freeArgAreaAndPopBytes(adjustment, stackSpace);

        if (call.reloadMachineStateAfter) {
            // On x86 there are no pinned registers, so don't waste time
            // reloading the Tls.
#ifndef JS_CODEGEN_X86
            masm.loadWasmTlsRegFromFrame();
            masm.loadWasmPinnedRegsFromTls();
#endif
        }
    }

    // TODO / OPTIMIZE (Bug 1316821): This is expensive; let's roll the iterator
    // walking into the walking done for passArg.  See comments in passArg.

    // Note, stackArgAreaSize() must process all the arguments to get the
    // alignment right; the signature must therefore be the complete call
    // signature.

    template<class T>
    size_t stackArgAreaSize(const T& args) {
        ABIArgIter<const T> i(args);
        while (!i.done())
            i++;
        return AlignBytes(i.stackBytesConsumedSoFar(), 16u);
    }

    void startCallArgs(FunctionCall& call, size_t stackArgAreaSize)
    {
        call.stackArgAreaSize = stackArgAreaSize;

        size_t adjustment = call.stackArgAreaSize + call.frameAlignAdjustment;
        fr.allocArgArea(adjustment);
    }

    const ABIArg reservePointerArgument(FunctionCall& call) {
        return call.abi.next(MIRType::Pointer);
    }

    // TODO / OPTIMIZE (Bug 1316821): Note passArg is used only in one place.
    // (Or it was, until Luke wandered through, but that can be fixed again.)
    // I'm not saying we should manually inline it, but we could hoist the
    // dispatch into the caller and have type-specific implementations of
    // passArg: passArgI32(), etc.  Then those might be inlined, at least in PGO
    // builds.
    //
    // The bulk of the work here (60%) is in the next() call, though.
    //
    // Notably, since next() is so expensive, stackArgAreaSize() becomes
    // expensive too.
    //
    // Somehow there could be a trick here where the sequence of
    // argument types (read from the input stream) leads to a cached
    // entry for stackArgAreaSize() and for how to pass arguments...
    //
    // But at least we could reduce the cost of stackArgAreaSize() by
    // first reading the argument types into a (reusable) vector, then
    // we have the outgoing size at low cost, and then we can pass
    // args based on the info we read.

    void passArg(FunctionCall& call, ValType type, Stk& arg) {
        switch (type) {
          case ValType::I32: {
            ABIArg argLoc = call.abi.next(MIRType::Int32);
            if (argLoc.kind() == ABIArg::Stack) {
                ScratchI32 scratch(*this);
                loadI32(arg, scratch);
                masm.store32(scratch, Address(masm.getStackPointer(), argLoc.offsetFromArgBase()));
            } else {
                loadI32(arg, RegI32(argLoc.gpr()));
            }
            break;
          }
          case ValType::I64: {
            ABIArg argLoc = call.abi.next(MIRType::Int64);
            if (argLoc.kind() == ABIArg::Stack) {
                ScratchI32 scratch(*this);
#ifdef JS_PUNBOX64
                loadI64(arg, fromI32(scratch));
                masm.storePtr(scratch, Address(masm.getStackPointer(), argLoc.offsetFromArgBase()));
#else
                loadI64Low(arg, scratch);
                masm.store32(scratch, LowWord(Address(masm.getStackPointer(), argLoc.offsetFromArgBase())));
                loadI64High(arg, scratch);
                masm.store32(scratch, HighWord(Address(masm.getStackPointer(), argLoc.offsetFromArgBase())));
#endif
            } else {
                loadI64(arg, RegI64(argLoc.gpr64()));
            }
            break;
          }
          case ValType::F64: {
            ABIArg argLoc = call.abi.next(MIRType::Double);
            switch (argLoc.kind()) {
              case ABIArg::Stack: {
                ScratchF64 scratch(*this);
                loadF64(arg, scratch);
                masm.storeDouble(scratch, Address(masm.getStackPointer(), argLoc.offsetFromArgBase()));
                break;
              }
#if defined(JS_CODEGEN_REGISTER_PAIR)
              case ABIArg::GPR_PAIR: {
# if defined(JS_CODEGEN_ARM)
                ScratchF64 scratch(*this);
                loadF64(arg, scratch);
                masm.ma_vxfer(scratch, argLoc.evenGpr(), argLoc.oddGpr());
                break;
# elif defined(JS_CODEGEN_MIPS32)
                ScratchF64 scratch(*this);
                loadF64(arg, scratch);
                MOZ_ASSERT(MOZ_LITTLE_ENDIAN);
                masm.moveFromDoubleLo(scratch, argLoc.evenGpr());
                masm.moveFromDoubleHi(scratch, argLoc.oddGpr());
                break;
# else
                MOZ_CRASH("BaseCompiler platform hook: passArg F64 pair");
# endif
              }
#endif
              case ABIArg::FPU: {
                loadF64(arg, RegF64(argLoc.fpu()));
                break;
              }
              case ABIArg::GPR: {
                MOZ_CRASH("Unexpected parameter passing discipline");
              }
              case ABIArg::Uninitialized:
                MOZ_CRASH("Uninitialized ABIArg kind");
            }
            break;
          }
          case ValType::F32: {
            ABIArg argLoc = call.abi.next(MIRType::Float32);
            switch (argLoc.kind()) {
              case ABIArg::Stack: {
                ScratchF32 scratch(*this);
                loadF32(arg, scratch);
                masm.storeFloat32(scratch, Address(masm.getStackPointer(), argLoc.offsetFromArgBase()));
                break;
              }
              case ABIArg::GPR: {
                ScratchF32 scratch(*this);
                loadF32(arg, scratch);
                masm.moveFloat32ToGPR(scratch, argLoc.gpr());
                break;
              }
              case ABIArg::FPU: {
                loadF32(arg, RegF32(argLoc.fpu()));
                break;
              }
#if defined(JS_CODEGEN_REGISTER_PAIR)
              case ABIArg::GPR_PAIR: {
                MOZ_CRASH("Unexpected parameter passing discipline");
              }
#endif
              case ABIArg::Uninitialized:
                MOZ_CRASH("Uninitialized ABIArg kind");
            }
            break;
          }
          default:
            MOZ_CRASH("Function argument type");
        }
    }

    void callDefinition(uint32_t funcIndex, const FunctionCall& call)
    {
        CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Func);
        masm.call(desc, funcIndex);
    }

    void callSymbolic(SymbolicAddress callee, const FunctionCall& call) {
        CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Symbolic);
        masm.call(desc, callee);
    }

    // Precondition: sync()

    void callIndirect(uint32_t sigIndex, Stk& indexVal, const FunctionCall& call)
    {
        const SigWithId& sig = env_.sigs[sigIndex];
        MOZ_ASSERT(sig.id.kind() != SigIdDesc::Kind::None);

        MOZ_ASSERT(env_.tables.length() == 1);
        const TableDesc& table = env_.tables[0];

        loadI32(indexVal, RegI32(WasmTableCallIndexReg));

        CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Dynamic);
        CalleeDesc callee = CalleeDesc::wasmTable(table, sig.id);
        masm.wasmCallIndirect(desc, callee, NeedsBoundsCheck(true));
    }

    // Precondition: sync()

    void callImport(unsigned globalDataOffset, const FunctionCall& call)
    {
        CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Dynamic);
        CalleeDesc callee = CalleeDesc::import(globalDataOffset);
        masm.wasmCallImport(desc, callee);
    }

    void builtinCall(SymbolicAddress builtin, const FunctionCall& call)
    {
        callSymbolic(builtin, call);
    }

    void builtinInstanceMethodCall(SymbolicAddress builtin, const ABIArg& instanceArg,
                                   const FunctionCall& call)
    {
        // Builtin method calls assume the TLS register has been set.
        masm.loadWasmTlsRegFromFrame();

        CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Symbolic);
        masm.wasmCallBuiltinInstanceMethod(desc, instanceArg, builtin);
    }

    //////////////////////////////////////////////////////////////////////
    //
    // Sundry low-level code generators.

    // The compiler depends on moveImm32() clearing the high bits of a 64-bit
    // register on 64-bit systems except MIPS64 where high bits are sign extended
    // from lower bits.

    void moveImm32(int32_t v, RegI32 dest) {
        masm.move32(Imm32(v), dest);
    }

    void moveImm64(int64_t v, RegI64 dest) {
        masm.move64(Imm64(v), dest);
    }

    void moveImmF32(float f, RegF32 dest) {
        masm.loadConstantFloat32(f, dest);
    }

    void moveImmF64(double d, RegF64 dest) {
        masm.loadConstantDouble(d, dest);
    }

    void addInterruptCheck()
    {
        // Always use signals for interrupts with Asm.JS/Wasm
        MOZ_RELEASE_ASSERT(HaveSignalHandlers());
    }

    void jumpTable(const LabelVector& labels, Label* theTable) {
        // Flush constant pools to ensure that the table is never interrupted by
        // constant pool entries.
        masm.flush();

        masm.bind(theTable);

        for (uint32_t i = 0; i < labels.length(); i++) {
            CodeLabel cl;
            masm.writeCodePointer(&cl);
            cl.target()->bind(labels[i].offset());
            masm.addCodeLabel(cl);
        }
    }

    void tableSwitch(Label* theTable, RegI32 switchValue, Label* dispatchCode) {
        masm.bind(dispatchCode);

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        ScratchI32 scratch(*this);
        CodeLabel tableCl;

        masm.mov(&tableCl, scratch);

        tableCl.target()->bind(theTable->offset());
        masm.addCodeLabel(tableCl);

        masm.jmp(Operand(scratch, switchValue, ScalePointer));
#elif defined(JS_CODEGEN_ARM)
        // Flush constant pools: offset must reflect the distance from the MOV
        // to the start of the table; as the address of the MOV is given by the
        // label, nothing must come between the bind() and the ma_mov().
        masm.flush();

        ScratchI32 scratch(*this);

        // Compute the offset from the ma_mov instruction to the jump table.
        Label here;
        masm.bind(&here);
        uint32_t offset = here.offset() - theTable->offset();

        // Read PC+8
        masm.ma_mov(pc, scratch);

        // ARM scratch register is required by ma_sub.
        ScratchRegisterScope arm_scratch(*this);

        // Compute the absolute table base pointer into `scratch`, offset by 8
        // to account for the fact that ma_mov read PC+8.
        masm.ma_sub(Imm32(offset + 8), scratch, arm_scratch);

        // Jump indirect via table element.
        masm.ma_ldr(DTRAddr(scratch, DtrRegImmShift(switchValue, LSL, 2)), pc, Offset,
                    Assembler::Always);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        ScratchI32 scratch(*this);
        CodeLabel tableCl;

        masm.ma_li(scratch, &tableCl);

        tableCl.target()->bind(theTable->offset());
        masm.addCodeLabel(tableCl);

        masm.branchToComputedAddress(BaseIndex(scratch, switchValue, ScalePointer));
#else
        MOZ_CRASH("BaseCompiler platform hook: tableSwitch");
#endif
    }

    RegI32 captureReturnedI32() {
        RegI32 r = RegI32(ReturnReg);
        MOZ_ASSERT(isAvailableI32(r));
        needI32(r);
        return r;
    }

    RegI64 captureReturnedI64() {
        RegI64 r = RegI64(ReturnReg64);
        MOZ_ASSERT(isAvailableI64(r));
        needI64(r);
        return r;
    }

    RegF32 captureReturnedF32(const FunctionCall& call) {
        RegF32 r = RegF32(ReturnFloat32Reg);
        MOZ_ASSERT(isAvailableF32(r));
        needF32(r);
#if defined(JS_CODEGEN_ARM)
        if (call.usesSystemAbi && !call.hardFP)
            masm.ma_vxfer(ReturnReg, r);
#endif
        return r;
    }

    RegF64 captureReturnedF64(const FunctionCall& call) {
        RegF64 r = RegF64(ReturnDoubleReg);
        MOZ_ASSERT(isAvailableF64(r));
        needF64(r);
#if defined(JS_CODEGEN_ARM)
        if (call.usesSystemAbi && !call.hardFP)
            masm.ma_vxfer(ReturnReg64.low, ReturnReg64.high, r);
#endif
        return r;
    }

    void returnCleanup(bool popStack) {
        if (popStack)
            fr.popStackBeforeBranch(controlOutermost().stackHeight);
        masm.jump(&returnLabel_);
    }

    void checkDivideByZeroI32(RegI32 rhs, RegI32 srcDest, Label* done) {
        Label nonZero;
        masm.branchTest32(Assembler::NonZero, rhs, rhs, &nonZero);
        trap(Trap::IntegerDivideByZero);
        masm.bind(&nonZero);
    }

    void checkDivideByZeroI64(RegI64 r) {
        Label nonZero;
        ScratchI32 scratch(*this);
        masm.branchTest64(Assembler::NonZero, r, r, scratch, &nonZero);
        trap(Trap::IntegerDivideByZero);
        masm.bind(&nonZero);
    }

    void checkDivideSignedOverflowI32(RegI32 rhs, RegI32 srcDest, Label* done, bool zeroOnOverflow) {
        Label notMin;
        masm.branch32(Assembler::NotEqual, srcDest, Imm32(INT32_MIN), &notMin);
        if (zeroOnOverflow) {
            masm.branch32(Assembler::NotEqual, rhs, Imm32(-1), &notMin);
            moveImm32(0, srcDest);
            masm.jump(done);
        } else {
            masm.branch32(Assembler::NotEqual, rhs, Imm32(-1), &notMin);
            trap(Trap::IntegerOverflow);
        }
        masm.bind(&notMin);
    }

    void checkDivideSignedOverflowI64(RegI64 rhs, RegI64 srcDest, Label* done, bool zeroOnOverflow) {
        Label notmin;
        masm.branch64(Assembler::NotEqual, srcDest, Imm64(INT64_MIN), &notmin);
        masm.branch64(Assembler::NotEqual, rhs, Imm64(-1), &notmin);
        if (zeroOnOverflow) {
            masm.xor64(srcDest, srcDest);
            masm.jump(done);
        } else {
            trap(Trap::IntegerOverflow);
        }
        masm.bind(&notmin);
    }

#ifndef RABALDR_INT_DIV_I64_CALLOUT
    void quotientI64(RegI64 rhs, RegI64 srcDest, RegI64 reserved, IsUnsigned isUnsigned,
                     bool isConst, int64_t c)
    {
        Label done;

        if (!isConst || c == 0)
            checkDivideByZeroI64(rhs);

        if (!isUnsigned && (!isConst || c == -1))
            checkDivideSignedOverflowI64(rhs, srcDest, &done, ZeroOnOverflow(false));

# if defined(JS_CODEGEN_X64)
        // The caller must set up the following situation.
        MOZ_ASSERT(srcDest.reg == rax);
        MOZ_ASSERT(reserved == specific.rdx);
        if (isUnsigned) {
            masm.xorq(rdx, rdx);
            masm.udivq(rhs.reg);
        } else {
            masm.cqo();
            masm.idivq(rhs.reg);
        }
# elif defined(JS_CODEGEN_MIPS64)
        if (isUnsigned)
            masm.as_ddivu(srcDest.reg, rhs.reg);
        else
            masm.as_ddiv(srcDest.reg, rhs.reg);
        masm.as_mflo(srcDest.reg);
# else
        MOZ_CRASH("BaseCompiler platform hook: quotientI64");
# endif
        masm.bind(&done);
    }

    void remainderI64(RegI64 rhs, RegI64 srcDest, RegI64 reserved, IsUnsigned isUnsigned,
                      bool isConst, int64_t c)
    {
        Label done;

        if (!isConst || c == 0)
            checkDivideByZeroI64(rhs);

        if (!isUnsigned && (!isConst || c == -1))
            checkDivideSignedOverflowI64(rhs, srcDest, &done, ZeroOnOverflow(true));

# if defined(JS_CODEGEN_X64)
        // The caller must set up the following situation.
        MOZ_ASSERT(srcDest.reg == rax);
        MOZ_ASSERT(reserved == specific.rdx);

        if (isUnsigned) {
            masm.xorq(rdx, rdx);
            masm.udivq(rhs.reg);
        } else {
            masm.cqo();
            masm.idivq(rhs.reg);
        }
        masm.movq(rdx, rax);
# elif defined(JS_CODEGEN_MIPS64)
        if (isUnsigned)
            masm.as_ddivu(srcDest.reg, rhs.reg);
        else
            masm.as_ddiv(srcDest.reg, rhs.reg);
        masm.as_mfhi(srcDest.reg);
# else
        MOZ_CRASH("BaseCompiler platform hook: remainderI64");
# endif
        masm.bind(&done);
    }
#endif // RABALDR_INT_DIV_I64_CALLOUT

    RegI32 needRotate64Temp() {
#if defined(JS_CODEGEN_X86)
        return needI32();
#elif defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) || \
      defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        return RegI32::Invalid();
#else
        MOZ_CRASH("BaseCompiler platform hook: needRotate64Temp");
#endif
    }

    void maskShiftCount32(RegI32 r) {
#if defined(JS_CODEGEN_ARM)
        masm.and32(Imm32(31), r);
#endif
    }

    RegI32 needPopcnt32Temp() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        return AssemblerX86Shared::HasPOPCNT() ? RegI32::Invalid() : needI32();
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        return needI32();
#else
        MOZ_CRASH("BaseCompiler platform hook: needPopcnt32Temp");
#endif
    }

    RegI32 needPopcnt64Temp() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        return AssemblerX86Shared::HasPOPCNT() ? RegI32::Invalid() : needI32();
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        return needI32();
#else
        MOZ_CRASH("BaseCompiler platform hook: needPopcnt64Temp");
#endif
    }

    class OutOfLineTruncateCheckF32OrF64ToI32 : public OutOfLineCode
    {
        AnyReg src;
        RegI32 dest;
        TruncFlags flags;
        BytecodeOffset off;

      public:
        OutOfLineTruncateCheckF32OrF64ToI32(AnyReg src, RegI32 dest, TruncFlags flags,
                                            BytecodeOffset off)
          : src(src),
            dest(dest),
            flags(flags),
            off(off)
        {}

        virtual void generate(MacroAssembler* masm) override {
            if (src.tag == AnyReg::F32)
                masm->oolWasmTruncateCheckF32ToI32(src.f32(), dest, flags, off, rejoin());
            else if (src.tag == AnyReg::F64)
                masm->oolWasmTruncateCheckF64ToI32(src.f64(), dest, flags, off, rejoin());
            else
                MOZ_CRASH("unexpected type");
        }
    };

    MOZ_MUST_USE bool truncateF32ToI32(RegF32 src, RegI32 dest, TruncFlags flags) {
        BytecodeOffset off = bytecodeOffset();
        OutOfLineCode* ool =
            addOutOfLineCode(new(alloc_) OutOfLineTruncateCheckF32OrF64ToI32(AnyReg(src),
                                                                             dest,
                                                                             flags,
                                                                             off));
        if (!ool)
            return false;
        bool isSaturating = flags & TRUNC_SATURATING;
        if (flags & TRUNC_UNSIGNED)
            masm.wasmTruncateFloat32ToUInt32(src, dest, isSaturating, ool->entry());
        else
            masm.wasmTruncateFloat32ToInt32(src, dest, isSaturating, ool->entry());
        masm.bind(ool->rejoin());
        return true;
    }

    MOZ_MUST_USE bool truncateF64ToI32(RegF64 src, RegI32 dest, TruncFlags flags) {
        BytecodeOffset off = bytecodeOffset();
        OutOfLineCode* ool =
            addOutOfLineCode(new(alloc_) OutOfLineTruncateCheckF32OrF64ToI32(AnyReg(src),
                                                                             dest,
                                                                             flags,
                                                                             off));
        if (!ool)
            return false;
        bool isSaturating = flags & TRUNC_SATURATING;
        if (flags & TRUNC_UNSIGNED)
            masm.wasmTruncateDoubleToUInt32(src, dest, isSaturating, ool->entry());
        else
            masm.wasmTruncateDoubleToInt32(src, dest, isSaturating, ool->entry());
        masm.bind(ool->rejoin());
        return true;
    }

    class OutOfLineTruncateCheckF32OrF64ToI64 : public OutOfLineCode
    {
        AnyReg src;
        RegI64 dest;
        TruncFlags flags;
        BytecodeOffset off;

      public:
        OutOfLineTruncateCheckF32OrF64ToI64(AnyReg src, RegI64 dest, TruncFlags flags, BytecodeOffset off)
          : src(src),
            dest(dest),
            flags(flags),
            off(off)
        {}

        virtual void generate(MacroAssembler* masm) override {
            if (src.tag == AnyReg::F32)
                masm->oolWasmTruncateCheckF32ToI64(src.f32(), dest, flags, off, rejoin());
            else if (src.tag == AnyReg::F64)
                masm->oolWasmTruncateCheckF64ToI64(src.f64(), dest, flags, off, rejoin());
            else
                MOZ_CRASH("unexpected type");
        }
    };

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT
    MOZ_MUST_USE RegF64 needTempForFloatingToI64(TruncFlags flags) {
# if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        if (flags & TRUNC_UNSIGNED)
            return needF64();
# endif
        return RegF64::Invalid();
    }

    MOZ_MUST_USE bool truncateF32ToI64(RegF32 src, RegI64 dest, TruncFlags flags, RegF64 temp) {
        OutOfLineCode* ool = addOutOfLineCode(
            new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(AnyReg(src),
                                                             dest,
                                                             flags,
                                                             bytecodeOffset()));
        if (!ool)
            return false;
        bool isSaturating = flags & TRUNC_SATURATING;
        if (flags & TRUNC_UNSIGNED) {
            masm.wasmTruncateFloat32ToUInt64(src, dest, isSaturating, ool->entry(),
                                             ool->rejoin(), temp);
        } else {
            masm.wasmTruncateFloat32ToInt64(src, dest, isSaturating, ool->entry(),
                                            ool->rejoin(), temp);
        }
        return true;
    }

    MOZ_MUST_USE bool truncateF64ToI64(RegF64 src, RegI64 dest, TruncFlags flags, RegF64 temp) {
        OutOfLineCode* ool = addOutOfLineCode(
            new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(AnyReg(src),
                                                             dest,
                                                             flags,
                                                             bytecodeOffset()));
        if (!ool)
            return false;
        bool isSaturating = flags & TRUNC_SATURATING;
        if (flags & TRUNC_UNSIGNED) {
            masm.wasmTruncateDoubleToUInt64(src, dest, isSaturating, ool->entry(),
                                            ool->rejoin(), temp);
        } else {
            masm.wasmTruncateDoubleToInt64(src, dest, isSaturating, ool->entry(),
                                           ool->rejoin(), temp);
        }
        return true;
    }
#endif // RABALDR_FLOAT_TO_I64_CALLOUT

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
    RegI32 needConvertI64ToFloatTemp(ValType to, bool isUnsigned) {
        bool needs = false;
        if (to == ValType::F64) {
            needs = isUnsigned && masm.convertUInt64ToDoubleNeedsTemp();
        } else {
# if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
            needs = true;
# endif
        }
        return needs ? needI32() : RegI32::Invalid();
    }

    void convertI64ToF32(RegI64 src, bool isUnsigned, RegF32 dest, RegI32 temp) {
        if (isUnsigned)
            masm.convertUInt64ToFloat32(src, dest, temp);
        else
            masm.convertInt64ToFloat32(src, dest);
    }

    void convertI64ToF64(RegI64 src, bool isUnsigned, RegF64 dest, RegI32 temp) {
        if (isUnsigned)
            masm.convertUInt64ToDouble(src, dest, temp);
        else
            masm.convertInt64ToDouble(src, dest);
    }
#endif // RABALDR_I64_TO_FLOAT_CALLOUT

    void cmp64Set(Assembler::Condition cond, RegI64 lhs, RegI64 rhs, RegI32 dest) {
#if defined(JS_PUNBOX64)
        masm.cmpPtrSet(cond, lhs.reg, rhs.reg, dest);
#elif defined(JS_CODEGEN_MIPS32)
        masm.cmp64Set(cond, lhs, rhs, dest);
#else
        // TODO / OPTIMIZE (Bug 1316822): This is pretty branchy, we should be
        // able to do better.
        Label done, condTrue;
        masm.branch64(cond, lhs, rhs, &condTrue);
        moveImm32(0, dest);
        masm.jump(&done);
        masm.bind(&condTrue);
        moveImm32(1, dest);
        masm.bind(&done);
#endif
    }

    void eqz64(RegI64 src, RegI32 dest) {
#ifdef JS_PUNBOX64
        masm.cmpPtrSet(Assembler::Equal, src.reg, ImmWord(0), dest);
#else
        masm.or32(src.high, src.low);
        masm.cmp32Set(Assembler::Equal, src.low, Imm32(0), dest);
#endif
    }

    MOZ_MUST_USE bool
    supportsRoundInstruction(RoundingMode mode)
    {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        return Assembler::HasRoundInstruction(mode);
#else
        return false;
#endif
    }

    void
    roundF32(RoundingMode roundingMode, RegF32 f0)
    {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        masm.vroundss(Assembler::ToX86RoundingMode(roundingMode), f0, f0, f0);
#else
        MOZ_CRASH("NYI");
#endif
    }

    void
    roundF64(RoundingMode roundingMode, RegF64 f0)
    {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        masm.vroundsd(Assembler::ToX86RoundingMode(roundingMode), f0, f0, f0);
#else
        MOZ_CRASH("NYI");
#endif
    }

    //////////////////////////////////////////////////////////////////////
    //
    // Global variable access.

    uint32_t globalToTlsOffset(uint32_t globalOffset) {
        return offsetof(TlsData, globalArea) + globalOffset;
    }

    void loadGlobalVarI32(unsigned globalDataOffset, RegI32 r)
    {
        ScratchI32 tmp(*this);
        masm.loadWasmTlsRegFromFrame(tmp);
        masm.load32(Address(tmp, globalToTlsOffset(globalDataOffset)), r);
    }

    void loadGlobalVarI64(unsigned globalDataOffset, RegI64 r)
    {
        ScratchI32 tmp(*this);
        masm.loadWasmTlsRegFromFrame(tmp);
        masm.load64(Address(tmp, globalToTlsOffset(globalDataOffset)), r);
    }

    void loadGlobalVarF32(unsigned globalDataOffset, RegF32 r)
    {
        ScratchI32 tmp(*this);
        masm.loadWasmTlsRegFromFrame(tmp);
        masm.loadFloat32(Address(tmp, globalToTlsOffset(globalDataOffset)), r);
    }

    void loadGlobalVarF64(unsigned globalDataOffset, RegF64 r)
    {
        ScratchI32 tmp(*this);
        masm.loadWasmTlsRegFromFrame(tmp);
        masm.loadDouble(Address(tmp, globalToTlsOffset(globalDataOffset)), r);
    }

    void storeGlobalVarI32(unsigned globalDataOffset, RegI32 r)
    {
        ScratchI32 tmp(*this);
        masm.loadWasmTlsRegFromFrame(tmp);
        masm.store32(r, Address(tmp, globalToTlsOffset(globalDataOffset)));
    }

    void storeGlobalVarI64(unsigned globalDataOffset, RegI64 r)
    {
        ScratchI32 tmp(*this);
        masm.loadWasmTlsRegFromFrame(tmp);
        masm.store64(r, Address(tmp, globalToTlsOffset(globalDataOffset)));
    }

    void storeGlobalVarF32(unsigned globalDataOffset, RegF32 r)
    {
        ScratchI32 tmp(*this);
        masm.loadWasmTlsRegFromFrame(tmp);
        masm.storeFloat32(r, Address(tmp, globalToTlsOffset(globalDataOffset)));
    }

    void storeGlobalVarF64(unsigned globalDataOffset, RegF64 r)
    {
        ScratchI32 tmp(*this);
        masm.loadWasmTlsRegFromFrame(tmp);
        masm.storeDouble(r, Address(tmp, globalToTlsOffset(globalDataOffset)));
    }

    //////////////////////////////////////////////////////////////////////
    //
    // Heap access.

    void bceCheckLocal(MemoryAccessDesc* access, AccessCheck* check, uint32_t local) {
        if (local >= sizeof(BCESet)*8)
            return;

        if ((bceSafe_ & (BCESet(1) << local)) && access->offset() < wasm::OffsetGuardLimit)
            check->omitBoundsCheck = true;

        // The local becomes safe even if the offset is beyond the guard limit.
        bceSafe_ |= (BCESet(1) << local);
    }

    void bceLocalIsUpdated(uint32_t local) {
        if (local >= sizeof(BCESet)*8)
            return;

        bceSafe_ &= ~(BCESet(1) << local);
    }

    void prepareMemoryAccess(MemoryAccessDesc* access, AccessCheck* check, RegI32 tls, RegI32 ptr) {

        // Fold offset if necessary for further computations.

        if (access->offset() >= OffsetGuardLimit ||
            (access->isAtomic() && !check->omitAlignmentCheck && !check->onlyPointerAlignment))
        {
            masm.branchAdd32(Assembler::CarrySet, Imm32(access->offset()), ptr,
                             oldTrap(Trap::OutOfBounds));
            access->clearOffset();
            check->onlyPointerAlignment = true;
        }

        // Alignment check if required.

        if (access->isAtomic() && !check->omitAlignmentCheck) {
            MOZ_ASSERT(check->onlyPointerAlignment);
            // We only care about the low pointer bits here.
            masm.branchTest32(Assembler::NonZero, ptr, Imm32(access->byteSize() - 1),
                              oldTrap(Trap::UnalignedAccess));
        }

        // Ensure no tls if we don't need it.

#ifdef WASM_HUGE_MEMORY
        // We have HeapReg and no bounds checking and need load neither
        // memoryBase nor boundsCheckLimit from tls.
        MOZ_ASSERT_IF(check->omitBoundsCheck, tls.isInvalid());
#endif
#ifdef JS_CODEGEN_ARM
        // We have HeapReg on ARM and don't need to load the memoryBase from tls.
        MOZ_ASSERT_IF(check->omitBoundsCheck, tls.isInvalid());
#endif

        // Bounds check if required.

#ifndef WASM_HUGE_MEMORY
        if (!check->omitBoundsCheck) {
            masm.wasmBoundsCheck(Assembler::AboveOrEqual, ptr,
                                 Address(tls, offsetof(TlsData, boundsCheckLimit)),
                                 oldTrap(Trap::OutOfBounds));
        }
#endif
    }

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    BaseIndex prepareAtomicMemoryAccess(MemoryAccessDesc* access, AccessCheck* check, RegI32 tls,
                                        RegI32 ptr)
    {
        MOZ_ASSERT(needTlsForAccess(*check) == tls.isValid());
        prepareMemoryAccess(access, check, tls, ptr);
        return BaseIndex(HeapReg, ptr, TimesOne, access->offset());
    }
#elif defined(JS_CODEGEN_X86)
    // Some consumers depend on the address not retaining tls, as tls may be the
    // scratch register.

    Address prepareAtomicMemoryAccess(MemoryAccessDesc* access, AccessCheck* check, RegI32 tls,
                                      RegI32 ptr)
    {
        MOZ_ASSERT(needTlsForAccess(*check) == tls.isValid());
        prepareMemoryAccess(access, check, tls, ptr);
        masm.addPtr(Address(tls, offsetof(TlsData, memoryBase)), ptr);
        return Address(ptr, access->offset());
    }
#else
    Address prepareAtomicMemoryAccess(MemoryAccessDesc* access, AccessCheck* check, RegI32 tls,
                                      RegI32 ptr)
    {
        MOZ_CRASH("BaseCompiler platform hook: prepareAtomicMemoryAccess");
    }
#endif

    void needLoadTemps(const MemoryAccessDesc& access, RegI32* temp1, RegI32* temp2,
                       RegI32* temp3)
    {
#if defined(JS_CODEGEN_ARM)
        if (IsUnaligned(access)) {
            switch (access.type()) {
              case Scalar::Float64:
                *temp3 = needI32();
                MOZ_FALLTHROUGH;
              case Scalar::Float32:
                *temp2 = needI32();
                MOZ_FALLTHROUGH;
              default:
                *temp1 = needI32();
                break;
            }
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        *temp1 = needI32();
#endif
    }

    MOZ_MUST_USE bool needTlsForAccess(const AccessCheck& check) {
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        return !check.omitBoundsCheck;
#elif defined(JS_CODEGEN_X86)
        return true;
#else
        return false;
#endif
    }

    // ptr and dest may be the same iff dest is I32.
    // This may destroy ptr even if ptr and dest are not the same.
    MOZ_MUST_USE bool load(MemoryAccessDesc* access, AccessCheck* check, RegI32 tls, RegI32 ptr,
                           AnyReg dest, RegI32 temp1, RegI32 temp2, RegI32 temp3)
    {
        prepareMemoryAccess(access, check, tls, ptr);

#if defined(JS_CODEGEN_X64)
        Operand srcAddr(HeapReg, ptr, TimesOne, access->offset());

        if (dest.tag == AnyReg::I64)
            masm.wasmLoadI64(*access, srcAddr, dest.i64());
        else
            masm.wasmLoad(*access, srcAddr, dest.any());
#elif defined(JS_CODEGEN_X86)
        masm.addPtr(Address(tls, offsetof(TlsData, memoryBase)), ptr);
        Operand srcAddr(ptr, access->offset());

        if (dest.tag == AnyReg::I64) {
            MOZ_ASSERT(dest.i64() == specific.abiReturnRegI64);
            masm.wasmLoadI64(*access, srcAddr, dest.i64());
        } else {
            ScratchI8 scratch(*this);
            bool byteRegConflict = access->byteSize() == 1 && !ra.isSingleByteI32(dest.i32());
            AnyRegister out = byteRegConflict ? AnyRegister(scratch) : dest.any();

            masm.wasmLoad(*access, srcAddr, out);

            if (byteRegConflict)
                masm.mov(scratch, dest.i32());
        }
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        if (IsUnaligned(*access)) {
            switch (dest.tag) {
              case AnyReg::I64:
                masm.wasmUnalignedLoadI64(*access, HeapReg, ptr, ptr, dest.i64(), temp1);
                break;
              case AnyReg::F32:
                masm.wasmUnalignedLoadFP(*access, HeapReg, ptr, ptr, dest.f32(), temp1, temp2,
                                         RegI32::Invalid());
                break;
              case AnyReg::F64:
                masm.wasmUnalignedLoadFP(*access, HeapReg, ptr, ptr, dest.f64(), temp1, temp2,
                                         temp3);
                break;
              default:
                masm.wasmUnalignedLoad(*access, HeapReg, ptr, ptr, dest.i32(), temp1);
                break;
            }
        } else {
            if (dest.tag == AnyReg::I64)
                masm.wasmLoadI64(*access, HeapReg, ptr, ptr, dest.i64());
            else
                masm.wasmLoad(*access, HeapReg, ptr, ptr, dest.any());
        }
#else
        MOZ_CRASH("BaseCompiler platform hook: load");
#endif

        return true;
    }

    RegI32 needStoreTemp(const MemoryAccessDesc& access, ValType srcType) {
#if defined(JS_CODEGEN_ARM)
        if (IsUnaligned(access) && srcType != ValType::I32)
            return needI32();
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        return needI32();
#endif
        return RegI32::Invalid();
    }

    // ptr and src must not be the same register.
    // This may destroy ptr and src.
    MOZ_MUST_USE bool store(MemoryAccessDesc* access, AccessCheck* check, RegI32 tls, RegI32 ptr,
                            AnyReg src, RegI32 temp)
    {
        prepareMemoryAccess(access, check, tls, ptr);

        // Emit the store
#if defined(JS_CODEGEN_X64)
        MOZ_ASSERT(temp.isInvalid());
        Operand dstAddr(HeapReg, ptr, TimesOne, access->offset());

        masm.wasmStore(*access, src.any(), dstAddr);
#elif defined(JS_CODEGEN_X86)
        MOZ_ASSERT(temp.isInvalid());
        masm.addPtr(Address(tls, offsetof(TlsData, memoryBase)), ptr);
        Operand dstAddr(ptr, access->offset());

        if (access->type() == Scalar::Int64) {
            masm.wasmStoreI64(*access, src.i64(), dstAddr);
        } else {
            AnyRegister value;
            ScratchI8 scratch(*this);
            if (src.tag == AnyReg::I64) {
                if (access->byteSize() == 1 && !ra.isSingleByteI32(src.i64().low)) {
                    masm.mov(src.i64().low, scratch);
                    value = AnyRegister(scratch);
                } else {
                    value = AnyRegister(src.i64().low);
                }
            } else if (access->byteSize() == 1 && !ra.isSingleByteI32(src.i32())) {
                masm.mov(src.i32(), scratch);
                value = AnyRegister(scratch);
            } else {
                value = src.any();
            }

            masm.wasmStore(*access, value, dstAddr);
        }
#elif defined(JS_CODEGEN_ARM)
        if (IsUnaligned(*access)) {
            switch (src.tag) {
              case AnyReg::I64:
                masm.wasmUnalignedStoreI64(*access, src.i64(), HeapReg, ptr, ptr, temp);
                break;
              case AnyReg::F32:
                masm.wasmUnalignedStoreFP(*access, src.f32(), HeapReg, ptr, ptr, temp);
                break;
              case AnyReg::F64:
                masm.wasmUnalignedStoreFP(*access, src.f64(), HeapReg, ptr, ptr, temp);
                break;
              default:
                MOZ_ASSERT(temp.isInvalid());
                masm.wasmUnalignedStore(*access, src.i32(), HeapReg, ptr, ptr, temp);
                break;
            }
        } else {
            MOZ_ASSERT(temp.isInvalid());
            if (access->type() == Scalar::Int64)
                masm.wasmStoreI64(*access, src.i64(), HeapReg, ptr, ptr);
            else if (src.tag == AnyReg::I64)
                masm.wasmStore(*access, AnyRegister(src.i64().low), HeapReg, ptr, ptr);
            else
                masm.wasmStore(*access, src.any(), HeapReg, ptr, ptr);
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        if (IsUnaligned(*access)) {
            switch (src.tag) {
                case AnyReg::I64:
                masm.wasmUnalignedStoreI64(*access, src.i64(), HeapReg, ptr, ptr, temp);
                break;
                case AnyReg::F32:
                masm.wasmUnalignedStoreFP(*access, src.f32(), HeapReg, ptr, ptr, temp);
                break;
                case AnyReg::F64:
                masm.wasmUnalignedStoreFP(*access, src.f64(), HeapReg, ptr, ptr, temp);
                break;
                default:
                masm.wasmUnalignedStore(*access, src.i32(), HeapReg, ptr, ptr, temp);
                break;
            }
        } else {
            if (src.tag == AnyReg::I64)
                masm.wasmStoreI64(*access, src.i64(), HeapReg, ptr, ptr);
            else
                masm.wasmStore(*access, src.any(), HeapReg, ptr, ptr);
        }
#else
        MOZ_CRASH("BaseCompiler platform hook: store");
#endif

        return true;
    }

    template <size_t Count>
    struct Atomic32Temps : mozilla::Array<RegI32, Count> {

        // Allocate all temp registers if 'allocate' is not specified.
        void allocate(BaseCompiler* bc, size_t allocate = Count) {
            MOZ_ASSERT(Count != 0);
            for (size_t i = 0; i < allocate; ++i)
                this->operator[](i) = bc->needI32();
        }
        void maybeFree(BaseCompiler* bc){
            for (size_t i = 0; i < Count; ++i)
                bc->maybeFreeI32(this->operator[](i));
        }
    };

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    typedef Atomic32Temps<3> AtomicRMW32Temps;
#else
    typedef Atomic32Temps<1> AtomicRMW32Temps;
#endif

    template<typename T>
    void atomicRMW32(T srcAddr, Scalar::Type viewType, AtomicOp op, RegI32 rv, RegI32 rd,
                     const AtomicRMW32Temps& temps)
    {
        Synchronization sync = Synchronization::Full();
        switch (viewType) {
          case Scalar::Uint8:
#ifdef JS_CODEGEN_X86
          {
            RegI32 temp = temps[0];
            // The temp, if used, must be a byte register.
            MOZ_ASSERT(temp.isInvalid());
            ScratchI8 scratch(*this);
            if (op != AtomicFetchAddOp && op != AtomicFetchSubOp)
                temp = scratch;
            masm.atomicFetchOp(viewType, sync, op, rv, srcAddr, temp, rd);
            break;
          }
#endif
          case Scalar::Uint16:
          case Scalar::Int32:
          case Scalar::Uint32:
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
            masm.atomicFetchOp(viewType, sync, op, rv, srcAddr, temps[0], temps[1], temps[2], rd);
#else
            masm.atomicFetchOp(viewType, sync, op, rv, srcAddr, temps[0], rd);
#endif
            break;
          default: {
            MOZ_CRASH("Bad type for atomic operation");
          }
        }
    }

    // On x86, V is Address.  On other platforms, it is Register64.
    // T is BaseIndex or Address.
    template<typename T, typename V>
    void atomicRMW64(const T& srcAddr, AtomicOp op, V value, Register64 temp, Register64 rd)
    {
        masm.atomicFetchOp64(Synchronization::Full(), op, value, srcAddr, temp, rd);
    }

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    typedef Atomic32Temps<3> AtomicCmpXchg32Temps;
#else
    typedef Atomic32Temps<0> AtomicCmpXchg32Temps;
#endif

    template<typename T>
    void atomicCmpXchg32(T srcAddr, Scalar::Type viewType, RegI32 rexpect, RegI32 rnew, RegI32 rd,
                         const AtomicCmpXchg32Temps& temps)
    {
        Synchronization sync = Synchronization::Full();
        switch (viewType) {
          case Scalar::Uint8:
#if defined(JS_CODEGEN_X86)
          {
            ScratchI8 scratch(*this);
            MOZ_ASSERT(rd == specific.eax);
            if (!ra.isSingleByteI32(rnew)) {
                // The replacement value must have a byte persona.
                masm.movl(rnew, scratch);
                rnew = scratch;
            }
            masm.compareExchange(viewType, sync, srcAddr, rexpect, rnew, rd);
            break;
          }
#endif
          case Scalar::Uint16:
          case Scalar::Int32:
          case Scalar::Uint32:
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
            masm.compareExchange(viewType, sync, srcAddr, rexpect, rnew, temps[0], temps[1],
                                 temps[2], rd);
#else
            masm.compareExchange(viewType, sync, srcAddr, rexpect, rnew, rd);
#endif
            break;
          default:
            MOZ_CRASH("Bad type for atomic operation");
        }
    }

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    typedef Atomic32Temps<3> AtomicXchg32Temps;
#else
    typedef Atomic32Temps<0> AtomicXchg32Temps;
#endif

    template<typename T>
    void atomicXchg32(T srcAddr, Scalar::Type viewType, RegI32 rv, RegI32 rd,
                      const AtomicXchg32Temps& temps)
    {
        Synchronization sync = Synchronization::Full();
        switch (viewType) {
          case Scalar::Uint8:
#if defined(JS_CODEGEN_X86)
          {
            if (!ra.isSingleByteI32(rd)) {
                ScratchI8 scratch(*this);
                // The output register must have a byte persona.
                masm.atomicExchange(viewType, sync, srcAddr, rv, scratch);
                masm.movl(scratch, rd);
            } else {
                masm.atomicExchange(viewType, sync, srcAddr, rv, rd);
            }
            break;
          }
#endif
          case Scalar::Uint16:
          case Scalar::Int32:
          case Scalar::Uint32:
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
            masm.atomicExchange(viewType, sync, srcAddr, rv, temps[0], temps[1], temps[2], rd);
#else
            masm.atomicExchange(viewType, sync, srcAddr, rv, rd);
#endif
            break;
          default:
            MOZ_CRASH("Bad type for atomic operation");
        }
    }

    ////////////////////////////////////////////////////////////
    //
    // Generally speaking, ABOVE this point there should be no
    // value stack manipulation (calls to popI32 etc).
    //
    ////////////////////////////////////////////////////////////

    ////////////////////////////////////////////////////////////
    //
    // Platform-specific popping and register targeting.
    //
    // These fall into two groups, popping methods for simple needs, and RAII
    // wrappers for more complex behavior.

    // The simple popping methods pop values into targeted registers; the caller
    // can free registers using standard functions.  These are always called
    // popXForY where X says something about types and Y something about the
    // operation being targeted.

    void pop2xI32ForMulDivI32(RegI32* r0, RegI32* r1, RegI32* reserved) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        // r0 must be eax, and edx will be clobbered.
        need2xI32(specific.eax, specific.edx);
        *r1 = popI32();
        *r0 = popI32ToSpecific(specific.eax);
        *reserved = specific.edx;
#else
        pop2xI32(r0, r1);
#endif
    }

    void pop2xI64ForMulI64(RegI64* r0, RegI64* r1, RegI32* temp, RegI64* reserved) {
#if defined(JS_CODEGEN_X64)
        // r0 must be rax, and rdx will be clobbered.
        need2xI64(specific.rax, specific.rdx);
        *r1 = popI64();
        *r0 = popI64ToSpecific(specific.rax);
        *reserved = specific.rdx;
#elif defined(JS_CODEGEN_X86)
        // As for x64, though edx is part of r0.
        need2xI32(specific.eax, specific.edx);
        *r1 = popI64();
        *r0 = popI64ToSpecific(specific.edx_eax);
        *temp = needI32();
#elif defined(JS_CODEGEN_MIPS64)
        pop2xI64(r0, r1);
#else
        pop2xI64(r0, r1);
        *temp = needI32();
#endif
    }

    void pop2xI64ForDivI64(RegI64* r0, RegI64* r1, RegI64* reserved) {
#ifdef JS_CODEGEN_X64
        // r0 must be rax, and rdx will be clobbered.
        need2xI64(specific.rax, specific.rdx);
        *r1 = popI64();
        *r0 = popI64ToSpecific(specific.rax);
        *reserved = specific.rdx;
#else
        pop2xI64(r0, r1);
#endif
    }

    void pop2xI32ForShiftOrRotate(RegI32* r0, RegI32* r1) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        // r1 must be ecx for a variable shift.
        *r1 = popI32(specific.ecx);
        *r0 = popI32();
#else
        pop2xI32(r0, r1);
#endif
    }

    void pop2xI64ForShiftOrRotate(RegI64* r0, RegI64* r1) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        // r1 must be ecx for a variable shift.
        needI32(specific.ecx);
        *r1 = popI64ToSpecific(widenI32(specific.ecx));
        *r0 = popI64();
#else
        pop2xI64(r0, r1);
#endif
    }

    void popI32ForSignExtendI64(RegI64* r0) {
#if defined(JS_CODEGEN_X86)
        // r0 must be edx:eax for cdq
        need2xI32(specific.edx, specific.eax);
        *r0 = specific.edx_eax;
        popI32ToSpecific(specific.eax);
#else
        *r0 = widenI32(popI32());
#endif
    }

    void popI64ForSignExtendI64(RegI64* r0) {
#if defined(JS_CODEGEN_X86)
        // r0 must be edx:eax for cdq
        need2xI32(specific.edx, specific.eax);
        // Low on top, high underneath
        *r0 = popI64ToSpecific(specific.edx_eax);
#else
        *r0 = popI64();
#endif
    }

    // The RAII wrappers are used because we sometimes have to free partial
    // registers, as when part of a register is the scratch register that has
    // been temporarily used, or not free a register at all, as when the
    // register is the same as the destination register (but only on some
    // platforms, not on all).  These are called PopX{32,64}Regs where X is the
    // operation being targeted.

    // Utility struct that holds the BaseCompiler and the destination, and frees
    // the destination if it has not been extracted.

    template<typename T>
    class PopBase
    {
        T rd_;

        void maybeFree(RegI32 r) { bc->maybeFreeI32(r); }
        void maybeFree(RegI64 r) { bc->maybeFreeI64(r); }

      protected:
        BaseCompiler* const bc;

        void setRd(T r) { MOZ_ASSERT(rd_.isInvalid()); rd_ = r; }
        T getRd() const { MOZ_ASSERT(rd_.isValid()); return rd_; }

      public:
        explicit PopBase(BaseCompiler* bc) : bc(bc) {}
        ~PopBase() {
            maybeFree(rd_);
        }

        // Take and clear the Rd - use this when pushing Rd.
        T takeRd() {
            MOZ_ASSERT(rd_.isValid());
            T r = rd_;
            rd_ = T::Invalid();
            return r;
        }
    };

    friend class PopAtomicCmpXchg32Regs;
    class PopAtomicCmpXchg32Regs : public PopBase<RegI32>
    {
        using Base = PopBase<RegI32>;
        RegI32 rexpect, rnew;
        AtomicCmpXchg32Temps temps;

      public:
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        explicit PopAtomicCmpXchg32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType) : Base(bc) {
            // For cmpxchg, the expected value and the result are both in eax.
            bc->needI32(bc->specific.eax);
            if (type == ValType::I64) {
                rnew = bc->popI64ToI32();
                rexpect = bc->popI64ToSpecificI32(bc->specific.eax);
            } else {
                rnew = bc->popI32();
                rexpect = bc->popI32ToSpecific(bc->specific.eax);
            }
            setRd(rexpect);
        }
        ~PopAtomicCmpXchg32Regs() {
            bc->freeI32(rnew);
        }
#elif defined(JS_CODEGEN_ARM)
        explicit PopAtomicCmpXchg32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType) : Base(bc) {
            if (type == ValType::I64) {
                rnew = bc->popI64ToI32();
                rexpect = bc->popI64ToI32();
            } else {
                rnew = bc->popI32();
                rexpect = bc->popI32();
            }
            setRd(bc->needI32());
        }
        ~PopAtomicCmpXchg32Regs() {
            bc->freeI32(rnew);
            bc->freeI32(rexpect);
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        explicit PopAtomicCmpXchg32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType) : Base(bc) {
            if (type == ValType::I64) {
                rnew = bc->popI64ToI32();
                rexpect = bc->popI64ToI32();
            } else {
                rnew = bc->popI32();
                rexpect = bc->popI32();
            }
            if (Scalar::byteSize(viewType) < 4)
                temps.allocate(bc);
            setRd(bc->needI32());
        }
        ~PopAtomicCmpXchg32Regs() {
            bc->freeI32(rnew);
            bc->freeI32(rexpect);
            temps.maybeFree(bc);
        }
#else
        explicit PopAtomicCmpXchg32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType) : Base(bc) {
            MOZ_CRASH("BaseCompiler porting interface: PopAtomicCmpXchg32Regs");
        }
#endif

        template<typename T>
        void atomicCmpXchg32(T srcAddr, Scalar::Type viewType) {
            bc->atomicCmpXchg32(srcAddr, viewType, rexpect, rnew, getRd(), temps);
        }
    };

    friend class PopAtomicCmpXchg64Regs;
    class PopAtomicCmpXchg64Regs : public PopBase<RegI64>
    {
        using Base = PopBase<RegI64>;
        RegI64 rexpect, rnew;

      public:
#ifdef JS_CODEGEN_X64
        explicit PopAtomicCmpXchg64Regs(BaseCompiler* bc) : Base(bc) {
            // For cmpxchg, the expected value and the result are both in rax.
            bc->needI64(bc->specific.rax);
            rnew = bc->popI64();
            rexpect = bc->popI64ToSpecific(bc->specific.rax);
            setRd(rexpect);
        }
        ~PopAtomicCmpXchg64Regs() {
            bc->freeI64(rnew);
        }
#elif defined(JS_CODEGEN_X86)
        explicit PopAtomicCmpXchg64Regs(BaseCompiler* bc) : Base(bc) {
            // For cmpxchg8b, the expected value and the result are both in
            // edx:eax, and the replacement value is in ecx:ebx.  But we can't
            // allocate ebx here, so instead we allocate a temp to hold the low
            // word of 'new'.
            bc->needI64(bc->specific.edx_eax);
            bc->needI32(bc->specific.ecx);

            rnew = bc->popI64ToSpecific(RegI64(Register64(bc->specific.ecx, bc->needI32())));
            rexpect = bc->popI64ToSpecific(bc->specific.edx_eax);
            setRd(rexpect);
        }
        ~PopAtomicCmpXchg64Regs() {
            bc->freeI64(rnew);
        }
#elif defined(JS_CODEGEN_ARM)
        explicit PopAtomicCmpXchg64Regs(BaseCompiler* bc) : Base(bc) {
            // The replacement value and the result must both be odd/even pairs.
            rnew = bc->popI64Pair();
            rexpect = bc->popI64();
            setRd(bc->needI64Pair());
        }
        ~PopAtomicCmpXchg64Regs() {
            bc->freeI64(rexpect);
            bc->freeI64(rnew);
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        explicit PopAtomicCmpXchg64Regs(BaseCompiler* bc) : Base(bc) {
            rnew = bc->popI64();
            rexpect = bc->popI64();
            setRd(bc->needI64());
        }
        ~PopAtomicCmpXchg64Regs() {
            bc->freeI64(rexpect);
            bc->freeI64(rnew);
        }
#else
        explicit PopAtomicCmpXchg64Regs(BaseCompiler* bc) : Base(bc) {
            MOZ_CRASH("BaseCompiler porting interface: PopAtomicCmpXchg64Regs");
        }
#endif

#ifdef JS_CODEGEN_X86
        template<typename T>
        void atomicCmpXchg64(T srcAddr, RegI32 ebx) {
            MOZ_ASSERT(ebx == js::jit::ebx);
            bc->masm.move32(rnew.low, ebx);
            bc->masm.compareExchange64(Synchronization::Full(), srcAddr, rexpect,
                                       bc->specific.ecx_ebx, getRd());
        }
#else
        template<typename T>
        void atomicCmpXchg64(T srcAddr) {
            bc->masm.compareExchange64(Synchronization::Full(), srcAddr, rexpect, rnew, getRd());
        }
#endif
    };

#ifndef JS_64BIT
    class PopAtomicLoad64Regs : public PopBase<RegI64>
    {
        using Base = PopBase<RegI64>;

      public:
# if defined(JS_CODEGEN_X86)
        explicit PopAtomicLoad64Regs(BaseCompiler* bc) : Base(bc) {
            // The result is in edx:eax, and we need ecx:ebx as a temp.  But we
            // can't reserve ebx yet, so we'll accept it as an argument to the
            // operation (below).
            bc->needI32(bc->specific.ecx);
            bc->needI64(bc->specific.edx_eax);
            setRd(bc->specific.edx_eax);
        }
        ~PopAtomicLoad64Regs() {
            bc->freeI32(bc->specific.ecx);
        }
# elif defined(JS_CODEGEN_ARM)
        explicit PopAtomicLoad64Regs(BaseCompiler* bc) : Base(bc) {
            setRd(bc->needI64Pair());
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        explicit PopAtomicLoad64Regs(BaseCompiler* bc) : Base(bc) {
            setRd(bc->needI64());
        }
# else
        explicit PopAtomicLoad64Regs(BaseCompiler* bc) : Base(bc) {
            MOZ_CRASH("BaseCompiler porting interface: PopAtomicLoad64Regs");
        }
# endif

# ifdef JS_CODEGEN_X86
        template<typename T>
        void atomicLoad64(T srcAddr, RegI32 ebx) {
            MOZ_ASSERT(ebx == js::jit::ebx);
            bc->masm.atomicLoad64(Synchronization::Full(), srcAddr, bc->specific.ecx_ebx, getRd());
        }
# else
        template<typename T>
        void atomicLoad64(T srcAddr) {
            bc->masm.atomicLoad64(Synchronization::Full(), srcAddr, RegI64::Invalid(), getRd());
        }
# endif
    };
#endif // JS_64BIT

    friend class PopAtomicRMW32Regs;
    class PopAtomicRMW32Regs : public PopBase<RegI32>
    {
        using Base = PopBase<RegI32>;
        RegI32 rv;
        AtomicRMW32Temps temps;

      public:
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        explicit PopAtomicRMW32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType,
                                    AtomicOp op)
          : Base(bc)
        {
            bc->needI32(bc->specific.eax);
            if (op == AtomicFetchAddOp || op == AtomicFetchSubOp) {
                // We use xadd, so source and destination are the same.  Using
                // eax here is overconstraining, but for byte operations on x86
                // we do need something with a byte register.
                if (type == ValType::I64)
                    rv = bc->popI64ToSpecificI32(bc->specific.eax);
                else
                    rv = bc->popI32ToSpecific(bc->specific.eax);
                setRd(rv);
            } else {
                // We use a cmpxchg loop.  The output must be eax; the input
                // must be in a separate register since it may be used several
                // times.
                if (type == ValType::I64)
                    rv = bc->popI64ToI32();
                else
                    rv = bc->popI32();
                setRd(bc->specific.eax);
# if defined(JS_CODEGEN_X86)
                // Single-byte is a special case handled very locally with
                // ScratchReg, see atomicRMW32 above.
                if (Scalar::byteSize(viewType) > 1)
                    temps.allocate(bc);
# else
                temps.allocate(bc);
# endif
            }
        }
        ~PopAtomicRMW32Regs() {
            if (rv != bc->specific.eax)
                bc->freeI32(rv);
            temps.maybeFree(bc);
        }
#elif defined(JS_CODEGEN_ARM)
        explicit PopAtomicRMW32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType,
                                    AtomicOp op)
          : Base(bc)
        {
            rv = type == ValType::I64 ? bc->popI64ToI32() : bc->popI32();
            temps.allocate(bc);
            setRd(bc->needI32());
        }
        ~PopAtomicRMW32Regs() {
            bc->freeI32(rv);
            temps.maybeFree(bc);
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        explicit PopAtomicRMW32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType,
                                    AtomicOp op)
          : Base(bc)
        {
            rv = type == ValType::I64 ? bc->popI64ToI32() : bc->popI32();
            if (Scalar::byteSize(viewType) < 4)
                temps.allocate(bc);

            setRd(bc->needI32());
        }
        ~PopAtomicRMW32Regs() {
            bc->freeI32(rv);
            temps.maybeFree(bc);
        }
#else
        explicit PopAtomicRMW32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType,
                                    AtomicOp op)
          : Base(bc)
        {
            MOZ_CRASH("BaseCompiler porting interface: PopAtomicRMW32Regs");
        }
#endif

        template<typename T>
        void atomicRMW32(T srcAddr, Scalar::Type viewType, AtomicOp op) {
            bc->atomicRMW32(srcAddr, viewType, op, rv, getRd(), temps);
        }
    };

    friend class PopAtomicRMW64Regs;
    class PopAtomicRMW64Regs : public PopBase<RegI64>
    {
        using Base = PopBase<RegI64>;
#if defined(JS_CODEGEN_X64)
        AtomicOp op;
#endif
        RegI64 rv, temp;

      public:
#if defined(JS_CODEGEN_X64)
        explicit PopAtomicRMW64Regs(BaseCompiler* bc, AtomicOp op) : Base(bc), op(op) {
            if (op == AtomicFetchAddOp || op == AtomicFetchSubOp) {
                // We use xaddq, so input and output must be the same register.
                rv = bc->popI64();
                setRd(rv);
            } else {
                // We use a cmpxchgq loop, so the output must be rax.
                bc->needI64(bc->specific.rax);
                rv = bc->popI64();
                temp = bc->needI64();
                setRd(bc->specific.rax);
            }
        }
        ~PopAtomicRMW64Regs() {
            bc->maybeFreeI64(temp);
            if (op != AtomicFetchAddOp && op != AtomicFetchSubOp)
                bc->freeI64(rv);
        }
#elif defined(JS_CODEGEN_X86)
        // We'll use cmpxchg8b, so rv must be in ecx:ebx, and rd must be
        // edx:eax.  But we can't reserve ebx here because we need it later, so
        // use a separate temp and set up ebx when we perform the operation.
        explicit PopAtomicRMW64Regs(BaseCompiler* bc, AtomicOp) : Base(bc) {
            bc->needI32(bc->specific.ecx);
            bc->needI64(bc->specific.edx_eax);

            temp = RegI64(Register64(bc->specific.ecx, bc->needI32()));
            bc->popI64ToSpecific(temp);

            setRd(bc->specific.edx_eax);
        }
        ~PopAtomicRMW64Regs() {
            bc->freeI64(temp);
        }
        RegI32 valueHigh() const { return RegI32(temp.high); }
        RegI32 valueLow() const { return RegI32(temp.low); }
#elif defined(JS_CODEGEN_ARM)
        explicit PopAtomicRMW64Regs(BaseCompiler* bc, AtomicOp) : Base(bc) {
            // We use a ldrex/strexd loop so the temp and the output must be
            // odd/even pairs.
            rv = bc->popI64();
            temp = bc->needI64Pair();
            setRd(bc->needI64Pair());
        }
        ~PopAtomicRMW64Regs() {
            bc->freeI64(rv);
            bc->freeI64(temp);
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        explicit PopAtomicRMW64Regs(BaseCompiler* bc, AtomicOp) : Base(bc) {
            rv = bc->popI64();
            temp = bc->needI64();
            setRd(bc->needI64());
        }
        ~PopAtomicRMW64Regs() {
            bc->freeI64(rv);
            bc->freeI64(temp);
        }
#else
        explicit PopAtomicRMW64Regs(BaseCompiler* bc, AtomicOp) : Base(bc) {
            MOZ_CRASH("BaseCompiler porting interface: PopAtomicRMW64Regs");
        }
#endif

#ifdef JS_CODEGEN_X86
        template<typename T, typename V>
        void atomicRMW64(T srcAddr, AtomicOp op, const V& value, RegI32 ebx) {
            MOZ_ASSERT(ebx == js::jit::ebx);
            bc->atomicRMW64(srcAddr, op, value, bc->specific.ecx_ebx, getRd());
        }
#else
        template<typename T>
        void atomicRMW64(T srcAddr, AtomicOp op) {
            bc->atomicRMW64(srcAddr, op, rv, temp, getRd());
        }
#endif
    };

    friend class PopAtomicXchg32Regs;
    class PopAtomicXchg32Regs : public PopBase<RegI32>
    {
        using Base = PopBase<RegI32>;
        RegI32 rv;
        AtomicXchg32Temps temps;

      public:
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        explicit PopAtomicXchg32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType) : Base(bc) {
            // The xchg instruction reuses rv as rd.
            rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
            setRd(rv);
        }
#elif defined(JS_CODEGEN_ARM)
        explicit PopAtomicXchg32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType) : Base(bc) {
            rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
            setRd(bc->needI32());
        }
        ~PopAtomicXchg32Regs() {
            bc->freeI32(rv);
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        explicit PopAtomicXchg32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType) : Base(bc) {
            rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
            if (Scalar::byteSize(viewType) < 4)
                temps.allocate(bc);
            setRd(bc->needI32());
        }
        ~PopAtomicXchg32Regs() {
            temps.maybeFree(bc);
            bc->freeI32(rv);
        }
#else
        explicit PopAtomicXchg32Regs(BaseCompiler* bc, ValType type, Scalar::Type viewType) : Base(bc) {
            MOZ_CRASH("BaseCompiler porting interface: PopAtomicXchg32Regs");
        }
#endif

        template<typename T>
        void atomicXchg32(T srcAddr, Scalar::Type viewType) {
            bc->atomicXchg32(srcAddr, viewType, rv, getRd(), temps);
        }
    };

    friend class PopAtomicXchg64Regs;
    class PopAtomicXchg64Regs : public PopBase<RegI64>
    {
        using Base = PopBase<RegI64>;
        RegI64 rv;

      public:
#ifdef JS_CODEGEN_X64
        explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
            rv = bc->popI64();
            setRd(rv);
        }
#elif defined(JS_CODEGEN_X86)
        // We'll use cmpxchg8b, so rv must be in ecx:ebx, and rd must be
        // edx:eax.  But we can't reserve ebx here because we need it later, so
        // use a separate temp and set up ebx when we perform the operation.
        explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
            bc->needI32(bc->specific.ecx);
            bc->needI64(bc->specific.edx_eax);

            rv = RegI64(Register64(bc->specific.ecx, bc->needI32()));
            bc->popI64ToSpecific(rv);

            setRd(bc->specific.edx_eax);
        }
        ~PopAtomicXchg64Regs() {
            bc->freeI64(rv);
        }
#elif defined(JS_CODEGEN_ARM)
        // Both rv and rd must be odd/even pairs.
        explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
            rv = bc->popI64ToSpecific(bc->needI64Pair());
            setRd(bc->needI64Pair());
        }
        ~PopAtomicXchg64Regs() {
            bc->freeI64(rv);
        }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
            rv = bc->popI64ToSpecific(bc->needI64());
            setRd(bc->needI64());
        }
        ~PopAtomicXchg64Regs() {
            bc->freeI64(rv);
        }
#else
        explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
            MOZ_CRASH("BaseCompiler porting interface: xchg64");
        }
#endif

#ifdef JS_CODEGEN_X86
        template<typename T>
        void atomicXchg64(T srcAddr, RegI32 ebx) const {
            MOZ_ASSERT(ebx == js::jit::ebx);
            bc->masm.move32(rv.low, ebx);
            bc->masm.atomicExchange64(Synchronization::Full(), srcAddr, bc->specific.ecx_ebx, getRd());
        }
#else
        template<typename T>
        void atomicXchg64(T srcAddr) const {
            bc->masm.atomicExchange64(Synchronization::Full(), srcAddr, rv, getRd());
        }
#endif
    };

    ////////////////////////////////////////////////////////////
    //
    // Generally speaking, BELOW this point there should be no
    // platform dependencies.  We make very occasional exceptions
    // when it doesn't become messy and further abstraction is
    // not desirable.
    //
    ////////////////////////////////////////////////////////////

    ////////////////////////////////////////////////////////////
    //
    // Sundry wrappers.

    void pop2xI32(RegI32* r0, RegI32* r1) {
        *r1 = popI32();
        *r0 = popI32();
    }

    RegI32 popI32ToSpecific(RegI32 specific) {
        freeI32(specific);
        return popI32(specific);
    }

    void pop2xI64(RegI64* r0, RegI64* r1) {
        *r1 = popI64();
        *r0 = popI64();
    }

    RegI64 popI64ToSpecific(RegI64 specific) {
        freeI64(specific);
        return popI64(specific);
    }

#ifdef JS_CODEGEN_ARM
    RegI64 popI64Pair() {
        RegI64 r = needI64Pair();
        popI64ToSpecific(r);
        return r;
    }
#endif

    void pop2xF32(RegF32* r0, RegF32* r1) {
        *r1 = popF32();
        *r0 = popF32();
    }

    void pop2xF64(RegF64* r0, RegF64* r1) {
        *r1 = popF64();
        *r0 = popF64();
    }

    RegI32 popI64ToI32() {
        RegI64 r = popI64();
        return narrowI64(r);
    }

    RegI32 popI64ToSpecificI32(RegI32 specific) {
        RegI64 rd = widenI32(specific);
        popI64ToSpecific(rd);
        return narrowI64(rd);
    }

    void pushU32AsI64(RegI32 rs) {
        RegI64 rd = widenI32(rs);
        masm.move32To64ZeroExtend(rs, rd);
        pushI64(rd);
    }

    RegI32 popMemoryAccess(MemoryAccessDesc* access, AccessCheck* check);

    ////////////////////////////////////////////////////////////
    //
    // Sundry helpers.

    uint32_t readCallSiteLineOrBytecode() {
        if (!func_.callSiteLineNums.empty())
            return func_.callSiteLineNums[lastReadCallSite_++];
        return iter_.lastOpcodeOffset();
    }

    bool done() const {
        return iter_.done();
    }

    BytecodeOffset bytecodeOffset() const {
        return iter_.bytecodeOffset();
    }

    void trap(Trap t) const {
        masm.wasmTrap(t, bytecodeOffset());
    }

    OldTrapDesc oldTrap(Trap t) const {
        // Use masm.framePushed() because the value needed by the trap machinery
        // is the size of the frame overall, not the height of the stack area of
        // the frame.
        return OldTrapDesc(bytecodeOffset(), t, masm.framePushed());
    }

    ////////////////////////////////////////////////////////////
    //
    // Machinery for optimized conditional branches.
    //
    // To disable this optimization it is enough always to return false from
    // sniffConditionalControl{Cmp,Eqz}.

    struct BranchState {
        static const int32_t NoPop = ~0;

        union {
            struct {
                RegI32 lhs;
                RegI32 rhs;
                int32_t imm;
                bool rhsImm;
            } i32;
            struct {
                RegI64 lhs;
                RegI64 rhs;
                int64_t imm;
                bool rhsImm;
            } i64;
            struct {
                RegF32 lhs;
                RegF32 rhs;
            } f32;
            struct {
                RegF64 lhs;
                RegF64 rhs;
            } f64;
        };

        Label* const label;        // The target of the branch, never NULL
        const int32_t stackHeight; // Either NoPop, or the value to pop to along the taken edge
        const bool invertBranch;   // If true, invert the sense of the branch
        const ExprType resultType; // The result propagated along the edges, or Void

        explicit BranchState(Label* label, int32_t stackHeight = NoPop,
                             uint32_t invertBranch = false, ExprType resultType = ExprType::Void)
          : label(label),
            stackHeight(stackHeight),
            invertBranch(invertBranch),
            resultType(resultType)
        {}
    };

    void setLatentCompare(Assembler::Condition compareOp, ValType operandType) {
        latentOp_ = LatentOp::Compare;
        latentType_ = operandType;
        latentIntCmp_ = compareOp;
    }

    void setLatentCompare(Assembler::DoubleCondition compareOp, ValType operandType) {
        latentOp_ = LatentOp::Compare;
        latentType_ = operandType;
        latentDoubleCmp_ = compareOp;
    }

    void setLatentEqz(ValType operandType) {
        latentOp_ = LatentOp::Eqz;
        latentType_ = operandType;
    }

    void resetLatentOp() {
        latentOp_ = LatentOp::None;
    }

    void branchTo(Assembler::DoubleCondition c, RegF64 lhs, RegF64 rhs, Label* l) {
        masm.branchDouble(c, lhs, rhs, l);
    }

    void branchTo(Assembler::DoubleCondition c, RegF32 lhs, RegF32 rhs, Label* l) {
        masm.branchFloat(c, lhs, rhs, l);
    }

    void branchTo(Assembler::Condition c, RegI32 lhs, RegI32 rhs, Label* l) {
        masm.branch32(c, lhs, rhs, l);
    }

    void branchTo(Assembler::Condition c, RegI32 lhs, Imm32 rhs, Label* l) {
        masm.branch32(c, lhs, rhs, l);
    }

    void branchTo(Assembler::Condition c, RegI64 lhs, RegI64 rhs, Label* l) {
        masm.branch64(c, lhs, rhs, l);
    }

    void branchTo(Assembler::Condition c, RegI64 lhs, Imm64 rhs, Label* l) {
        masm.branch64(c, lhs, rhs, l);
    }

    // Emit a conditional branch that optionally and optimally cleans up the CPU
    // stack before we branch.
    //
    // Cond is either Assembler::Condition or Assembler::DoubleCondition.
    //
    // Lhs is RegI32, RegI64, or RegF32, or RegF64.
    //
    // Rhs is either the same as Lhs, or an immediate expression compatible with
    // Lhs "when applicable".

    template<typename Cond, typename Lhs, typename Rhs>
    void jumpConditionalWithJoinReg(BranchState* b, Cond cond, Lhs lhs, Rhs rhs)
    {
        Maybe<AnyReg> r = popJoinRegUnlessVoid(b->resultType);

        if (b->stackHeight != BranchState::NoPop && fr.willPopStackBeforeBranch(b->stackHeight)) {
            Label notTaken;
            branchTo(b->invertBranch ? cond : Assembler::InvertCondition(cond), lhs, rhs, &notTaken);
            fr.popStackBeforeBranch(b->stackHeight);
            masm.jump(b->label);
            masm.bind(&notTaken);
        } else {
            branchTo(b->invertBranch ? Assembler::InvertCondition(cond) : cond, lhs, rhs, b->label);
        }

        pushJoinRegUnlessVoid(r);
    }

    // sniffConditionalControl{Cmp,Eqz} may modify the latentWhatever_ state in
    // the BaseCompiler so that a subsequent conditional branch can be compiled
    // optimally.  emitBranchSetup() and emitBranchPerform() will consume that
    // state.  If the latter methods are not called because deadCode_ is true
    // then the compiler MUST instead call resetLatentOp() to reset the state.

    template<typename Cond> bool sniffConditionalControlCmp(Cond compareOp, ValType operandType);
    bool sniffConditionalControlEqz(ValType operandType);
    void emitBranchSetup(BranchState* b);
    void emitBranchPerform(BranchState* b);

    //////////////////////////////////////////////////////////////////////

    MOZ_MUST_USE bool emitBody();
    MOZ_MUST_USE bool emitBlock();
    MOZ_MUST_USE bool emitLoop();
    MOZ_MUST_USE bool emitIf();
    MOZ_MUST_USE bool emitElse();
    MOZ_MUST_USE bool emitEnd();
    MOZ_MUST_USE bool emitBr();
    MOZ_MUST_USE bool emitBrIf();
    MOZ_MUST_USE bool emitBrTable();
    MOZ_MUST_USE bool emitDrop();
    MOZ_MUST_USE bool emitReturn();
    MOZ_MUST_USE bool emitCallArgs(const ValTypeVector& args, FunctionCall& baselineCall);
    MOZ_MUST_USE bool emitCall();
    MOZ_MUST_USE bool emitCallIndirect();
    MOZ_MUST_USE bool emitUnaryMathBuiltinCall(SymbolicAddress callee, ValType operandType);
    MOZ_MUST_USE bool emitGetLocal();
    MOZ_MUST_USE bool emitSetLocal();
    MOZ_MUST_USE bool emitTeeLocal();
    MOZ_MUST_USE bool emitGetGlobal();
    MOZ_MUST_USE bool emitSetGlobal();
    MOZ_MUST_USE RegI32 maybeLoadTlsForAccess(const AccessCheck& check);
    MOZ_MUST_USE RegI32 maybeLoadTlsForAccess(const AccessCheck& check, RegI32 specific);
    MOZ_MUST_USE bool emitLoad(ValType type, Scalar::Type viewType);
    MOZ_MUST_USE bool loadCommon(MemoryAccessDesc* access, ValType type);
    MOZ_MUST_USE bool emitStore(ValType resultType, Scalar::Type viewType);
    MOZ_MUST_USE bool storeCommon(MemoryAccessDesc* access, ValType resultType);
    MOZ_MUST_USE bool emitSelect();

    // Mark these templates as inline to work around a compiler crash in
    // gcc 4.8.5 when compiling for linux64-opt.

    template<bool isSetLocal> MOZ_MUST_USE inline bool emitSetOrTeeLocal(uint32_t slot);

    void endBlock(ExprType type);
    void endLoop(ExprType type);
    void endIfThen();
    void endIfThenElse(ExprType type);

    void doReturn(ExprType returnType, bool popStack);
    void pushReturned(const FunctionCall& call, ExprType type);

    void emitCompareI32(Assembler::Condition compareOp, ValType compareType);
    void emitCompareI64(Assembler::Condition compareOp, ValType compareType);
    void emitCompareF32(Assembler::DoubleCondition compareOp, ValType compareType);
    void emitCompareF64(Assembler::DoubleCondition compareOp, ValType compareType);

    void emitAddI32();
    void emitAddI64();
    void emitAddF64();
    void emitAddF32();
    void emitSubtractI32();
    void emitSubtractI64();
    void emitSubtractF32();
    void emitSubtractF64();
    void emitMultiplyI32();
    void emitMultiplyI64();
    void emitMultiplyF32();
    void emitMultiplyF64();
    void emitQuotientI32();
    void emitQuotientU32();
    void emitRemainderI32();
    void emitRemainderU32();
#ifdef RABALDR_INT_DIV_I64_CALLOUT
    void emitDivOrModI64BuiltinCall(SymbolicAddress callee, ValType operandType);
#else
    void emitQuotientI64();
    void emitQuotientU64();
    void emitRemainderI64();
    void emitRemainderU64();
#endif
    void emitDivideF32();
    void emitDivideF64();
    void emitMinF32();
    void emitMaxF32();
    void emitMinF64();
    void emitMaxF64();
    void emitCopysignF32();
    void emitCopysignF64();
    void emitOrI32();
    void emitOrI64();
    void emitAndI32();
    void emitAndI64();
    void emitXorI32();
    void emitXorI64();
    void emitShlI32();
    void emitShlI64();
    void emitShrI32();
    void emitShrI64();
    void emitShrU32();
    void emitShrU64();
    void emitRotrI32();
    void emitRotrI64();
    void emitRotlI32();
    void emitRotlI64();
    void emitEqzI32();
    void emitEqzI64();
    void emitClzI32();
    void emitClzI64();
    void emitCtzI32();
    void emitCtzI64();
    void emitPopcntI32();
    void emitPopcntI64();
    void emitAbsF32();
    void emitAbsF64();
    void emitNegateF32();
    void emitNegateF64();
    void emitSqrtF32();
    void emitSqrtF64();
    template<TruncFlags flags> MOZ_MUST_USE bool emitTruncateF32ToI32();
    template<TruncFlags flags> MOZ_MUST_USE bool emitTruncateF64ToI32();
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
    MOZ_MUST_USE bool emitConvertFloatingToInt64Callout(SymbolicAddress callee, ValType operandType,
                                                        ValType resultType);
#else
    template<TruncFlags flags> MOZ_MUST_USE bool emitTruncateF32ToI64();
    template<TruncFlags flags> MOZ_MUST_USE bool emitTruncateF64ToI64();
#endif
    void emitWrapI64ToI32();
    void emitExtendI32_8();
    void emitExtendI32_16();
    void emitExtendI64_8();
    void emitExtendI64_16();
    void emitExtendI64_32();
    void emitExtendI32ToI64();
    void emitExtendU32ToI64();
    void emitReinterpretF32AsI32();
    void emitReinterpretF64AsI64();
    void emitConvertF64ToF32();
    void emitConvertI32ToF32();
    void emitConvertU32ToF32();
    void emitConvertF32ToF64();
    void emitConvertI32ToF64();
    void emitConvertU32ToF64();
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
    MOZ_MUST_USE bool emitConvertInt64ToFloatingCallout(SymbolicAddress callee, ValType operandType,
                                                        ValType resultType);
#else
    void emitConvertI64ToF32();
    void emitConvertU64ToF32();
    void emitConvertI64ToF64();
    void emitConvertU64ToF64();
#endif
    void emitReinterpretI32AsF32();
    void emitReinterpretI64AsF64();
    void emitRound(RoundingMode roundingMode, ValType operandType);
    void emitInstanceCall(uint32_t lineOrBytecode, const MIRTypeVector& sig,
                          ExprType retType, SymbolicAddress builtin);
    MOZ_MUST_USE bool emitGrowMemory();
    MOZ_MUST_USE bool emitCurrentMemory();

    MOZ_MUST_USE bool emitAtomicCmpXchg(ValType type, Scalar::Type viewType);
    MOZ_MUST_USE bool emitAtomicLoad(ValType type, Scalar::Type viewType);
    MOZ_MUST_USE bool emitAtomicRMW(ValType type, Scalar::Type viewType, AtomicOp op);
    MOZ_MUST_USE bool emitAtomicStore(ValType type, Scalar::Type viewType);
    MOZ_MUST_USE bool emitWait(ValType type, uint32_t byteSize);
    MOZ_MUST_USE bool emitWake();
    MOZ_MUST_USE bool emitAtomicXchg(ValType type, Scalar::Type viewType);
    void emitAtomicXchg64(MemoryAccessDesc* access, ValType type, WantResult wantResult);
};

void
BaseCompiler::emitAddI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.add32(Imm32(c), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32(&r, &rs);
        masm.add32(rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitAddI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        masm.add64(Imm64(c), r);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64(&r, &rs);
        masm.add64(rs, r);
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitAddF64()
{
    RegF64 r, rs;
    pop2xF64(&r, &rs);
    masm.addDouble(rs, r);
    freeF64(rs);
    pushF64(r);
}

void
BaseCompiler::emitAddF32()
{
    RegF32 r, rs;
    pop2xF32(&r, &rs);
    masm.addFloat32(rs, r);
    freeF32(rs);
    pushF32(r);
}

void
BaseCompiler::emitSubtractI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.sub32(Imm32(c), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32(&r, &rs);
        masm.sub32(rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitSubtractI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        masm.sub64(Imm64(c), r);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64(&r, &rs);
        masm.sub64(rs, r);
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitSubtractF32()
{
    RegF32 r, rs;
    pop2xF32(&r, &rs);
    masm.subFloat32(rs, r);
    freeF32(rs);
    pushF32(r);
}

void
BaseCompiler::emitSubtractF64()
{
    RegF64 r, rs;
    pop2xF64(&r, &rs);
    masm.subDouble(rs, r);
    freeF64(rs);
    pushF64(r);
}

void
BaseCompiler::emitMultiplyI32()
{
    RegI32 r, rs, reserved;
    pop2xI32ForMulDivI32(&r, &rs, &reserved);
    masm.mul32(rs, r);
    maybeFreeI32(reserved);
    freeI32(rs);
    pushI32(r);
}

void
BaseCompiler::emitMultiplyI64()
{
    RegI64 r, rs, reserved;
    RegI32 temp;
    pop2xI64ForMulI64(&r, &rs, &temp, &reserved);
    masm.mul64(rs, r, temp);
    maybeFreeI64(reserved);
    maybeFreeI32(temp);
    freeI64(rs);
    pushI64(r);
}

void
BaseCompiler::emitMultiplyF32()
{
    RegF32 r, rs;
    pop2xF32(&r, &rs);
    masm.mulFloat32(rs, r);
    freeF32(rs);
    pushF32(r);
}

void
BaseCompiler::emitMultiplyF64()
{
    RegF64 r, rs;
    pop2xF64(&r, &rs);
    masm.mulDouble(rs, r);
    freeF64(rs);
    pushF64(r);
}

void
BaseCompiler::emitQuotientI32()
{
    int32_t c;
    uint_fast8_t power;
    if (popConstPositivePowerOfTwoI32(&c, &power, 0)) {
        if (power != 0) {
            RegI32 r = popI32();
            Label positive;
            masm.branchTest32(Assembler::NotSigned, r, r, &positive);
            masm.add32(Imm32(c-1), r);
            masm.bind(&positive);

            masm.rshift32Arithmetic(Imm32(power & 31), r);
            pushI32(r);
        }
    } else {
        bool isConst = peekConstI32(&c);
        RegI32 r, rs, reserved;
        pop2xI32ForMulDivI32(&r, &rs, &reserved);

        Label done;
        if (!isConst || c == 0)
            checkDivideByZeroI32(rs, r, &done);
        if (!isConst || c == -1)
            checkDivideSignedOverflowI32(rs, r, &done, ZeroOnOverflow(false));
        masm.quotient32(rs, r, IsUnsigned(false));
        masm.bind(&done);

        maybeFreeI32(reserved);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitQuotientU32()
{
    int32_t c;
    uint_fast8_t power;
    if (popConstPositivePowerOfTwoI32(&c, &power, 0)) {
        if (power != 0) {
            RegI32 r = popI32();
            masm.rshift32(Imm32(power & 31), r);
            pushI32(r);
        }
    } else {
        bool isConst = peekConstI32(&c);
        RegI32 r, rs, reserved;
        pop2xI32ForMulDivI32(&r, &rs, &reserved);

        Label done;
        if (!isConst || c == 0)
            checkDivideByZeroI32(rs, r, &done);
        masm.quotient32(rs, r, IsUnsigned(true));
        masm.bind(&done);

        maybeFreeI32(reserved);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitRemainderI32()
{
    int32_t c;
    uint_fast8_t power;
    if (popConstPositivePowerOfTwoI32(&c, &power, 1)) {
        RegI32 r = popI32();
        RegI32 temp = needI32();
        moveI32(r, temp);

        Label positive;
        masm.branchTest32(Assembler::NotSigned, temp, temp, &positive);
        masm.add32(Imm32(c-1), temp);
        masm.bind(&positive);

        masm.rshift32Arithmetic(Imm32(power & 31), temp);
        masm.lshift32(Imm32(power & 31), temp);
        masm.sub32(temp, r);
        freeI32(temp);

        pushI32(r);
    } else {
        bool isConst = peekConstI32(&c);
        RegI32 r, rs, reserved;
        pop2xI32ForMulDivI32(&r, &rs, &reserved);

        Label done;
        if (!isConst || c == 0)
            checkDivideByZeroI32(rs, r, &done);
        if (!isConst || c == -1)
            checkDivideSignedOverflowI32(rs, r, &done, ZeroOnOverflow(true));
        masm.remainder32(rs, r, IsUnsigned(false));
        masm.bind(&done);

        maybeFreeI32(reserved);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitRemainderU32()
{
    int32_t c;
    uint_fast8_t power;
    if (popConstPositivePowerOfTwoI32(&c, &power, 1)) {
        RegI32 r = popI32();
        masm.and32(Imm32(c-1), r);
        pushI32(r);
    } else {
        bool isConst = peekConstI32(&c);
        RegI32 r, rs, reserved;
        pop2xI32ForMulDivI32(&r, &rs, &reserved);

        Label done;
        if (!isConst || c == 0)
            checkDivideByZeroI32(rs, r, &done);
        masm.remainder32(rs, r, IsUnsigned(true));
        masm.bind(&done);

        maybeFreeI32(reserved);
        freeI32(rs);
        pushI32(r);
    }
}

#ifndef RABALDR_INT_DIV_I64_CALLOUT
void
BaseCompiler::emitQuotientI64()
{
# ifdef JS_64BIT
    int64_t c;
    uint_fast8_t power;
    if (popConstPositivePowerOfTwoI64(&c, &power, 0)) {
        if (power != 0) {
            RegI64 r = popI64();
            Label positive;
            masm.branchTest64(Assembler::NotSigned, r, r, RegI32::Invalid(),
                              &positive);
            masm.add64(Imm64(c-1), r);
            masm.bind(&positive);

            masm.rshift64Arithmetic(Imm32(power & 63), r);
            pushI64(r);
        }
    } else {
        bool isConst = peekConstI64(&c);
        RegI64 r, rs, reserved;
        pop2xI64ForDivI64(&r, &rs, &reserved);
        quotientI64(rs, r, reserved, IsUnsigned(false), isConst, c);
        maybeFreeI64(reserved);
        freeI64(rs);
        pushI64(r);
    }
# else
    MOZ_CRASH("BaseCompiler platform hook: emitQuotientI64");
# endif
}

void
BaseCompiler::emitQuotientU64()
{
# ifdef JS_64BIT
    int64_t c;
    uint_fast8_t power;
    if (popConstPositivePowerOfTwoI64(&c, &power, 0)) {
        if (power != 0) {
            RegI64 r = popI64();
            masm.rshift64(Imm32(power & 63), r);
            pushI64(r);
        }
    } else {
        bool isConst = peekConstI64(&c);
        RegI64 r, rs, reserved;
        pop2xI64ForDivI64(&r, &rs, &reserved);
        quotientI64(rs, r, reserved, IsUnsigned(true), isConst, c);
        maybeFreeI64(reserved);
        freeI64(rs);
        pushI64(r);
    }
# else
    MOZ_CRASH("BaseCompiler platform hook: emitQuotientU64");
# endif
}

void
BaseCompiler::emitRemainderI64()
{
# ifdef JS_64BIT
    int64_t c;
    uint_fast8_t power;
    if (popConstPositivePowerOfTwoI64(&c, &power, 1)) {
        RegI64 r = popI64();
        RegI64 temp = needI64();
        moveI64(r, temp);

        Label positive;
        masm.branchTest64(Assembler::NotSigned, temp, temp, RegI32::Invalid(), &positive);
        masm.add64(Imm64(c-1), temp);
        masm.bind(&positive);

        masm.rshift64Arithmetic(Imm32(power & 63), temp);
        masm.lshift64(Imm32(power & 63), temp);
        masm.sub64(temp, r);
        freeI64(temp);

        pushI64(r);
    } else {
        bool isConst = peekConstI64(&c);
        RegI64 r, rs, reserved;
        pop2xI64ForDivI64(&r, &rs, &reserved);
        remainderI64(rs, r, reserved, IsUnsigned(false), isConst, c);
        maybeFreeI64(reserved);
        freeI64(rs);
        pushI64(r);
    }
# else
    MOZ_CRASH("BaseCompiler platform hook: emitRemainderI64");
# endif
}

void
BaseCompiler::emitRemainderU64()
{
# ifdef JS_64BIT
    int64_t c;
    uint_fast8_t power;
    if (popConstPositivePowerOfTwoI64(&c, &power, 1)) {
        RegI64 r = popI64();
        masm.and64(Imm64(c-1), r);
        pushI64(r);
    } else {
        bool isConst = peekConstI64(&c);
        RegI64 r, rs, reserved;
        pop2xI64ForDivI64(&r, &rs, &reserved);
        remainderI64(rs, r, reserved, IsUnsigned(true), isConst, c);
        maybeFreeI64(reserved);
        freeI64(rs);
        pushI64(r);
    }
# else
    MOZ_CRASH("BaseCompiler platform hook: emitRemainderU64");
# endif
}
#endif // RABALDR_INT_DIV_I64_CALLOUT

void
BaseCompiler::emitDivideF32()
{
    RegF32 r, rs;
    pop2xF32(&r, &rs);
    masm.divFloat32(rs, r);
    freeF32(rs);
    pushF32(r);
}

void
BaseCompiler::emitDivideF64()
{
    RegF64 r, rs;
    pop2xF64(&r, &rs);
    masm.divDouble(rs, r);
    freeF64(rs);
    pushF64(r);
}

void
BaseCompiler::emitMinF32()
{
    RegF32 r, rs;
    pop2xF32(&r, &rs);
    // Convert signaling NaN to quiet NaNs.
    //
    // TODO / OPTIMIZE (bug 1316824): Don't do this if one of the operands
    // is known to be a constant.
    ScratchF32 zero(*this);
    moveImmF32(0.f, zero);
    masm.subFloat32(zero, r);
    masm.subFloat32(zero, rs);
    masm.minFloat32(rs, r, HandleNaNSpecially(true));
    freeF32(rs);
    pushF32(r);
}

void
BaseCompiler::emitMaxF32()
{
    RegF32 r, rs;
    pop2xF32(&r, &rs);
    // Convert signaling NaN to quiet NaNs.
    //
    // TODO / OPTIMIZE (bug 1316824): see comment in emitMinF32.
    ScratchF32 zero(*this);
    moveImmF32(0.f, zero);
    masm.subFloat32(zero, r);
    masm.subFloat32(zero, rs);
    masm.maxFloat32(rs, r, HandleNaNSpecially(true));
    freeF32(rs);
    pushF32(r);
}

void
BaseCompiler::emitMinF64()
{
    RegF64 r, rs;
    pop2xF64(&r, &rs);
    // Convert signaling NaN to quiet NaNs.
    //
    // TODO / OPTIMIZE (bug 1316824): see comment in emitMinF32.
    ScratchF64 zero(*this);
    moveImmF64(0, zero);
    masm.subDouble(zero, r);
    masm.subDouble(zero, rs);
    masm.minDouble(rs, r, HandleNaNSpecially(true));
    freeF64(rs);
    pushF64(r);
}

void
BaseCompiler::emitMaxF64()
{
    RegF64 r, rs;
    pop2xF64(&r, &rs);
    // Convert signaling NaN to quiet NaNs.
    //
    // TODO / OPTIMIZE (bug 1316824): see comment in emitMinF32.
    ScratchF64 zero(*this);
    moveImmF64(0, zero);
    masm.subDouble(zero, r);
    masm.subDouble(zero, rs);
    masm.maxDouble(rs, r, HandleNaNSpecially(true));
    freeF64(rs);
    pushF64(r);
}

void
BaseCompiler::emitCopysignF32()
{
    RegF32 r, rs;
    pop2xF32(&r, &rs);
    RegI32 temp0 = needI32();
    RegI32 temp1 = needI32();
    masm.moveFloat32ToGPR(r, temp0);
    masm.moveFloat32ToGPR(rs, temp1);
    masm.and32(Imm32(INT32_MAX), temp0);
    masm.and32(Imm32(INT32_MIN), temp1);
    masm.or32(temp1, temp0);
    masm.moveGPRToFloat32(temp0, r);
    freeI32(temp0);
    freeI32(temp1);
    freeF32(rs);
    pushF32(r);
}

void
BaseCompiler::emitCopysignF64()
{
    RegF64 r, rs;
    pop2xF64(&r, &rs);
    RegI64 temp0 = needI64();
    RegI64 temp1 = needI64();
    masm.moveDoubleToGPR64(r, temp0);
    masm.moveDoubleToGPR64(rs, temp1);
    masm.and64(Imm64(INT64_MAX), temp0);
    masm.and64(Imm64(INT64_MIN), temp1);
    masm.or64(temp1, temp0);
    masm.moveGPR64ToDouble(temp0, r);
    freeI64(temp0);
    freeI64(temp1);
    freeF64(rs);
    pushF64(r);
}

void
BaseCompiler::emitOrI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.or32(Imm32(c), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32(&r, &rs);
        masm.or32(rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitOrI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        masm.or64(Imm64(c), r);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64(&r, &rs);
        masm.or64(rs, r);
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitAndI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.and32(Imm32(c), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32(&r, &rs);
        masm.and32(rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitAndI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        masm.and64(Imm64(c), r);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64(&r, &rs);
        masm.and64(rs, r);
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitXorI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.xor32(Imm32(c), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32(&r, &rs);
        masm.xor32(rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitXorI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        masm.xor64(Imm64(c), r);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64(&r, &rs);
        masm.xor64(rs, r);
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitShlI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.lshift32(Imm32(c & 31), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32ForShiftOrRotate(&r, &rs);
        maskShiftCount32(rs);
        masm.lshift32(rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitShlI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        masm.lshift64(Imm32(c & 63), r);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64ForShiftOrRotate(&r, &rs);
        masm.lshift64(lowPart(rs), r);
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitShrI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.rshift32Arithmetic(Imm32(c & 31), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32ForShiftOrRotate(&r, &rs);
        maskShiftCount32(rs);
        masm.rshift32Arithmetic(rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitShrI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        masm.rshift64Arithmetic(Imm32(c & 63), r);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64ForShiftOrRotate(&r, &rs);
        masm.rshift64Arithmetic(lowPart(rs), r);
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitShrU32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.rshift32(Imm32(c & 31), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32ForShiftOrRotate(&r, &rs);
        maskShiftCount32(rs);
        masm.rshift32(rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitShrU64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        masm.rshift64(Imm32(c & 63), r);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64ForShiftOrRotate(&r, &rs);
        masm.rshift64(lowPart(rs), r);
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitRotrI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.rotateRight(Imm32(c & 31), r, r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32ForShiftOrRotate(&r, &rs);
        masm.rotateRight(rs, r, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitRotrI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        RegI32 temp = needRotate64Temp();
        masm.rotateRight64(Imm32(c & 63), r, r, temp);
        maybeFreeI32(temp);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64ForShiftOrRotate(&r, &rs);
        masm.rotateRight64(lowPart(rs), r, r, maybeHighPart(rs));
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitRotlI32()
{
    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.rotateLeft(Imm32(c & 31), r, r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32ForShiftOrRotate(&r, &rs);
        masm.rotateLeft(rs, r, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitRotlI64()
{
    int64_t c;
    if (popConstI64(&c)) {
        RegI64 r = popI64();
        RegI32 temp = needRotate64Temp();
        masm.rotateLeft64(Imm32(c & 63), r, r, temp);
        maybeFreeI32(temp);
        pushI64(r);
    } else {
        RegI64 r, rs;
        pop2xI64ForShiftOrRotate(&r, &rs);
        masm.rotateLeft64(lowPart(rs), r, r, maybeHighPart(rs));
        freeI64(rs);
        pushI64(r);
    }
}

void
BaseCompiler::emitEqzI32()
{
    if (sniffConditionalControlEqz(ValType::I32))
        return;

    RegI32 r = popI32();
    masm.cmp32Set(Assembler::Equal, r, Imm32(0), r);
    pushI32(r);
}

void
BaseCompiler::emitEqzI64()
{
    if (sniffConditionalControlEqz(ValType::I64))
        return;

    RegI64 rs = popI64();
    RegI32 rd = fromI64(rs);
    eqz64(rs, rd);
    freeI64Except(rs, rd);
    pushI32(rd);
}

void
BaseCompiler::emitClzI32()
{
    RegI32 r = popI32();
    masm.clz32(r, r, IsKnownNotZero(false));
    pushI32(r);
}

void
BaseCompiler::emitClzI64()
{
    RegI64 r = popI64();
    masm.clz64(r, lowPart(r));
    maybeClearHighPart(r);
    pushI64(r);
}

void
BaseCompiler::emitCtzI32()
{
    RegI32 r = popI32();
    masm.ctz32(r, r, IsKnownNotZero(false));
    pushI32(r);
}

void
BaseCompiler::emitCtzI64()
{
    RegI64 r = popI64();
    masm.ctz64(r, lowPart(r));
    maybeClearHighPart(r);
    pushI64(r);
}

void
BaseCompiler::emitPopcntI32()
{
    RegI32 r = popI32();
    RegI32 temp = needPopcnt32Temp();
    masm.popcnt32(r, r, temp);
    maybeFreeI32(temp);
    pushI32(r);
}

void
BaseCompiler::emitPopcntI64()
{
    RegI64 r = popI64();
    RegI32 temp = needPopcnt64Temp();
    masm.popcnt64(r, r, temp);
    maybeFreeI32(temp);
    pushI64(r);
}

void
BaseCompiler::emitAbsF32()
{
    RegF32 r = popF32();
    masm.absFloat32(r, r);
    pushF32(r);
}

void
BaseCompiler::emitAbsF64()
{
    RegF64 r = popF64();
    masm.absDouble(r, r);
    pushF64(r);
}

void
BaseCompiler::emitNegateF32()
{
    RegF32 r = popF32();
    masm.negateFloat(r);
    pushF32(r);
}

void
BaseCompiler::emitNegateF64()
{
    RegF64 r = popF64();
    masm.negateDouble(r);
    pushF64(r);
}

void
BaseCompiler::emitSqrtF32()
{
    RegF32 r = popF32();
    masm.sqrtFloat32(r, r);
    pushF32(r);
}

void
BaseCompiler::emitSqrtF64()
{
    RegF64 r = popF64();
    masm.sqrtDouble(r, r);
    pushF64(r);
}

template<TruncFlags flags>
bool
BaseCompiler::emitTruncateF32ToI32()
{
    RegF32 rs = popF32();
    RegI32 rd = needI32();
    if (!truncateF32ToI32(rs, rd, flags))
        return false;
    freeF32(rs);
    pushI32(rd);
    return true;
}

template<TruncFlags flags>
bool
BaseCompiler::emitTruncateF64ToI32()
{
    RegF64 rs = popF64();
    RegI32 rd = needI32();
    if (!truncateF64ToI32(rs, rd, flags))
        return false;
    freeF64(rs);
    pushI32(rd);
    return true;
}

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT
template<TruncFlags flags>
bool
BaseCompiler::emitTruncateF32ToI64()
{
    RegF32 rs = popF32();
    RegI64 rd = needI64();
    RegF64 temp = needTempForFloatingToI64(flags);
    if (!truncateF32ToI64(rs, rd, flags, temp))
        return false;
    maybeFreeF64(temp);
    freeF32(rs);
    pushI64(rd);
    return true;
}

template<TruncFlags flags>
bool
BaseCompiler::emitTruncateF64ToI64()
{
    RegF64 rs = popF64();
    RegI64 rd = needI64();
    RegF64 temp = needTempForFloatingToI64(flags);
    if (!truncateF64ToI64(rs, rd, flags, temp))
        return false;
    maybeFreeF64(temp);
    freeF64(rs);
    pushI64(rd);
    return true;
}
#endif // RABALDR_FLOAT_TO_I64_CALLOUT

void
BaseCompiler::emitWrapI64ToI32()
{
    RegI64 rs = popI64();
    RegI32 rd = fromI64(rs);
    masm.move64To32(rs, rd);
    freeI64Except(rs, rd);
    pushI32(rd);
}

void
BaseCompiler::emitExtendI32_8()
{
    RegI32 r = popI32();
    masm.move8SignExtend(r, r);
    pushI32(r);
}

void
BaseCompiler::emitExtendI32_16()
{
    RegI32 r = popI32();
    masm.move16SignExtend(r, r);
    pushI32(r);
}

void
BaseCompiler::emitExtendI64_8()
{
    RegI64 r;
    popI64ForSignExtendI64(&r);
    masm.move8To64SignExtend(lowPart(r), r);
    pushI64(r);
}

void
BaseCompiler::emitExtendI64_16()
{
    RegI64 r;
    popI64ForSignExtendI64(&r);
    masm.move16To64SignExtend(lowPart(r), r);
    pushI64(r);
}

void
BaseCompiler::emitExtendI64_32()
{
    RegI64 r;
    popI64ForSignExtendI64(&r);
    masm.move32To64SignExtend(lowPart(r), r);
    pushI64(r);
}

void
BaseCompiler::emitExtendI32ToI64()
{
    RegI64 r;
    popI32ForSignExtendI64(&r);
    masm.move32To64SignExtend(lowPart(r), r);
    pushI64(r);
}

void
BaseCompiler::emitExtendU32ToI64()
{
    RegI32 rs = popI32();
    RegI64 rd = widenI32(rs);
    masm.move32To64ZeroExtend(rs, rd);
    pushI64(rd);
}

void
BaseCompiler::emitReinterpretF32AsI32()
{
    RegF32 rs = popF32();
    RegI32 rd = needI32();
    masm.moveFloat32ToGPR(rs, rd);
    freeF32(rs);
    pushI32(rd);
}

void
BaseCompiler::emitReinterpretF64AsI64()
{
    RegF64 rs = popF64();
    RegI64 rd = needI64();
    masm.moveDoubleToGPR64(rs, rd);
    freeF64(rs);
    pushI64(rd);
}

void
BaseCompiler::emitConvertF64ToF32()
{
    RegF64 rs = popF64();
    RegF32 rd = needF32();
    masm.convertDoubleToFloat32(rs, rd);
    freeF64(rs);
    pushF32(rd);
}

void
BaseCompiler::emitConvertI32ToF32()
{
    RegI32 rs = popI32();
    RegF32 rd = needF32();
    masm.convertInt32ToFloat32(rs, rd);
    freeI32(rs);
    pushF32(rd);
}

void
BaseCompiler::emitConvertU32ToF32()
{
    RegI32 rs = popI32();
    RegF32 rd = needF32();
    masm.convertUInt32ToFloat32(rs, rd);
    freeI32(rs);
    pushF32(rd);
}

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
void
BaseCompiler::emitConvertI64ToF32()
{
    RegI64 rs = popI64();
    RegF32 rd = needF32();
    convertI64ToF32(rs, IsUnsigned(false), rd, RegI32());
    freeI64(rs);
    pushF32(rd);
}

void
BaseCompiler::emitConvertU64ToF32()
{
    RegI64 rs = popI64();
    RegF32 rd = needF32();
    RegI32 temp = needConvertI64ToFloatTemp(ValType::F32, IsUnsigned(true));
    convertI64ToF32(rs, IsUnsigned(true), rd, temp);
    maybeFreeI32(temp);
    freeI64(rs);
    pushF32(rd);
}
#endif

void
BaseCompiler::emitConvertF32ToF64()
{
    RegF32 rs = popF32();
    RegF64 rd = needF64();
    masm.convertFloat32ToDouble(rs, rd);
    freeF32(rs);
    pushF64(rd);
}

void
BaseCompiler::emitConvertI32ToF64()
{
    RegI32 rs = popI32();
    RegF64 rd = needF64();
    masm.convertInt32ToDouble(rs, rd);
    freeI32(rs);
    pushF64(rd);
}

void
BaseCompiler::emitConvertU32ToF64()
{
    RegI32 rs = popI32();
    RegF64 rd = needF64();
    masm.convertUInt32ToDouble(rs, rd);
    freeI32(rs);
    pushF64(rd);
}

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
void
BaseCompiler::emitConvertI64ToF64()
{
    RegI64 rs = popI64();
    RegF64 rd = needF64();
    convertI64ToF64(rs, IsUnsigned(false), rd, RegI32());
    freeI64(rs);
    pushF64(rd);
}

void
BaseCompiler::emitConvertU64ToF64()
{
    RegI64 rs = popI64();
    RegF64 rd = needF64();
    RegI32 temp = needConvertI64ToFloatTemp(ValType::F64, IsUnsigned(true));
    convertI64ToF64(rs, IsUnsigned(true), rd, temp);
    maybeFreeI32(temp);
    freeI64(rs);
    pushF64(rd);
}
#endif // RABALDR_I64_TO_FLOAT_CALLOUT

void
BaseCompiler::emitReinterpretI32AsF32()
{
    RegI32 rs = popI32();
    RegF32 rd = needF32();
    masm.moveGPRToFloat32(rs, rd);
    freeI32(rs);
    pushF32(rd);
}

void
BaseCompiler::emitReinterpretI64AsF64()
{
    RegI64 rs = popI64();
    RegF64 rd = needF64();
    masm.moveGPR64ToDouble(rs, rd);
    freeI64(rs);
    pushF64(rd);
}

template<typename Cond>
bool
BaseCompiler::sniffConditionalControlCmp(Cond compareOp, ValType operandType)
{
    MOZ_ASSERT(latentOp_ == LatentOp::None, "Latent comparison state not properly reset");

#ifdef JS_CODEGEN_X86
    // On x86, latent i64 binary comparisons use too many registers: the
    // reserved join register and the lhs and rhs operands require six, but we
    // only have five.
    if (operandType == ValType::I64)
        return false;
#endif

    OpBytes op;
    iter_.peekOp(&op);
    switch (op.b0) {
      case uint16_t(Op::Select):
        MOZ_FALLTHROUGH;
      case uint16_t(Op::BrIf):
      case uint16_t(Op::If):
        setLatentCompare(compareOp, operandType);
        return true;
      default:
        return false;
    }
}

bool
BaseCompiler::sniffConditionalControlEqz(ValType operandType)
{
    MOZ_ASSERT(latentOp_ == LatentOp::None, "Latent comparison state not properly reset");

    OpBytes op;
    iter_.peekOp(&op);
    switch (op.b0) {
      case uint16_t(Op::BrIf):
      case uint16_t(Op::Select):
      case uint16_t(Op::If):
        setLatentEqz(operandType);
        return true;
      default:
        return false;
    }
}

void
BaseCompiler::emitBranchSetup(BranchState* b)
{
    maybeReserveJoinReg(b->resultType);

    // Set up fields so that emitBranchPerform() need not switch on latentOp_.
    switch (latentOp_) {
      case LatentOp::None: {
        latentIntCmp_ = Assembler::NotEqual;
        latentType_ = ValType::I32;
        b->i32.lhs = popI32();
        b->i32.rhsImm = true;
        b->i32.imm = 0;
        break;
      }
      case LatentOp::Compare: {
        switch (latentType_) {
          case ValType::I32: {
            if (popConstI32(&b->i32.imm)) {
                b->i32.lhs = popI32();
                b->i32.rhsImm = true;
            } else {
                pop2xI32(&b->i32.lhs, &b->i32.rhs);
                b->i32.rhsImm = false;
            }
            break;
          }
          case ValType::I64: {
            pop2xI64(&b->i64.lhs, &b->i64.rhs);
            b->i64.rhsImm = false;
            break;
          }
          case ValType::F32: {
            pop2xF32(&b->f32.lhs, &b->f32.rhs);
            break;
          }
          case ValType::F64: {
            pop2xF64(&b->f64.lhs, &b->f64.rhs);
            break;
          }
          default: {
            MOZ_CRASH("Unexpected type for LatentOp::Compare");
          }
        }
        break;
      }
      case LatentOp::Eqz: {
        switch (latentType_) {
          case ValType::I32: {
            latentIntCmp_ = Assembler::Equal;
            b->i32.lhs = popI32();
            b->i32.rhsImm = true;
            b->i32.imm = 0;
            break;
          }
          case ValType::I64: {
            latentIntCmp_ = Assembler::Equal;
            b->i64.lhs = popI64();
            b->i64.rhsImm = true;
            b->i64.imm = 0;
            break;
          }
          default: {
            MOZ_CRASH("Unexpected type for LatentOp::Eqz");
          }
        }
        break;
      }
    }

    maybeUnreserveJoinReg(b->resultType);
}

void
BaseCompiler::emitBranchPerform(BranchState* b)
{
    switch (latentType_) {
      case ValType::I32: {
        if (b->i32.rhsImm) {
            jumpConditionalWithJoinReg(b, latentIntCmp_, b->i32.lhs, Imm32(b->i32.imm));
        } else {
            jumpConditionalWithJoinReg(b, latentIntCmp_, b->i32.lhs, b->i32.rhs);
            freeI32(b->i32.rhs);
        }
        freeI32(b->i32.lhs);
        break;
      }
      case ValType::I64: {
        if (b->i64.rhsImm) {
            jumpConditionalWithJoinReg(b, latentIntCmp_, b->i64.lhs, Imm64(b->i64.imm));
        } else {
            jumpConditionalWithJoinReg(b, latentIntCmp_, b->i64.lhs, b->i64.rhs);
            freeI64(b->i64.rhs);
        }
        freeI64(b->i64.lhs);
        break;
      }
      case ValType::F32: {
        jumpConditionalWithJoinReg(b, latentDoubleCmp_, b->f32.lhs, b->f32.rhs);
        freeF32(b->f32.lhs);
        freeF32(b->f32.rhs);
        break;
      }
      case ValType::F64: {
        jumpConditionalWithJoinReg(b, latentDoubleCmp_, b->f64.lhs, b->f64.rhs);
        freeF64(b->f64.lhs);
        freeF64(b->f64.rhs);
        break;
      }
      default: {
        MOZ_CRASH("Unexpected type for LatentOp::Compare");
      }
    }
    resetLatentOp();
}

// For blocks and loops and ifs:
//
//  - Sync the value stack before going into the block in order to simplify exit
//    from the block: all exits from the block can assume that there are no
//    live registers except the one carrying the exit value.
//  - The block can accumulate a number of dead values on the stacks, so when
//    branching out of the block or falling out at the end be sure to
//    pop the appropriate stacks back to where they were on entry, while
//    preserving the exit value.
//  - A continue branch in a loop is much like an exit branch, but the branch
//    value must not be preserved.
//  - The exit value is always in a designated join register (type dependent).

bool
BaseCompiler::emitBlock()
{
    if (!iter_.readBlock())
        return false;

    if (!deadCode_)
        sync();                    // Simplifies branching out from block

    initControl(controlItem());

    return true;
}

void
BaseCompiler::endBlock(ExprType type)
{
    Control& block = controlItem();

    // Save the value.
    Maybe<AnyReg> r;
    if (!deadCode_) {
        r = popJoinRegUnlessVoid(type);
        block.bceSafeOnExit &= bceSafe_;
    }

    // Leave the block.
    fr.popStackOnBlockExit(block.stackHeight, deadCode_);
    popValueStackTo(block.stackSize);

    // Bind after cleanup: branches out will have popped the stack.
    if (block.label.used()) {
        masm.bind(&block.label);
        // No value was provided by the fallthrough but the branch out will
        // have stored one in joinReg, so capture that.
        if (deadCode_)
            r = captureJoinRegUnlessVoid(type);
        deadCode_ = false;
    }

    bceSafe_ = block.bceSafeOnExit;

    // Retain the value stored in joinReg by all paths, if there are any.
    if (!deadCode_)
        pushJoinRegUnlessVoid(r);
}

bool
BaseCompiler::emitLoop()
{
    if (!iter_.readLoop())
        return false;

    if (!deadCode_)
        sync();                    // Simplifies branching out from block

    initControl(controlItem());
    bceSafe_ = 0;

    if (!deadCode_) {
        masm.nopAlign(CodeAlignment);
        masm.bind(&controlItem(0).label);
        addInterruptCheck();
    }

    return true;
}

void
BaseCompiler::endLoop(ExprType type)
{
    Control& block = controlItem();

    Maybe<AnyReg> r;
    if (!deadCode_) {
        r = popJoinRegUnlessVoid(type);
        // block.bceSafeOnExit need not be updated because it won't be used for
        // the fallthrough path.
    }

    fr.popStackOnBlockExit(block.stackHeight, deadCode_);
    popValueStackTo(block.stackSize);

    // bceSafe_ stays the same along the fallthrough path because branches to
    // loops branch to the top.

    // Retain the value stored in joinReg by all paths.
    if (!deadCode_)
        pushJoinRegUnlessVoid(r);
}

// The bodies of the "then" and "else" arms can be arbitrary sequences
// of expressions, they push control and increment the nesting and can
// even be targeted by jumps.  A branch to the "if" block branches to
// the exit of the if, ie, it's like "break".  Consider:
//
//      (func (result i32)
//       (if (i32.const 1)
//           (begin (br 1) (unreachable))
//           (begin (unreachable)))
//       (i32.const 1))
//
// The branch causes neither of the unreachable expressions to be
// evaluated.

bool
BaseCompiler::emitIf()
{
    Nothing unused_cond;
    if (!iter_.readIf(&unused_cond))
        return false;

    BranchState b(&controlItem().otherLabel, BranchState::NoPop, InvertBranch(true));
    if (!deadCode_) {
        emitBranchSetup(&b);
        sync();
    } else {
        resetLatentOp();
    }

    initControl(controlItem());

    if (!deadCode_)
        emitBranchPerform(&b);

    return true;
}

void
BaseCompiler::endIfThen()
{
    Control& ifThen = controlItem();

    fr.popStackOnBlockExit(ifThen.stackHeight, deadCode_);
    popValueStackTo(ifThen.stackSize);

    if (ifThen.otherLabel.used())
        masm.bind(&ifThen.otherLabel);

    if (ifThen.label.used())
        masm.bind(&ifThen.label);

    if (!deadCode_)
        ifThen.bceSafeOnExit &= bceSafe_;

    deadCode_ = ifThen.deadOnArrival;

    bceSafe_ = ifThen.bceSafeOnExit & ifThen.bceSafeOnEntry;
}

bool
BaseCompiler::emitElse()
{
    ExprType thenType;
    Nothing unused_thenValue;

    if (!iter_.readElse(&thenType, &unused_thenValue))
        return false;

    Control& ifThenElse = controlItem(0);

    // See comment in endIfThenElse, below.

    // Exit the "then" branch.

    ifThenElse.deadThenBranch = deadCode_;

    Maybe<AnyReg> r;
    if (!deadCode_)
        r = popJoinRegUnlessVoid(thenType);

    fr.popStackOnBlockExit(ifThenElse.stackHeight, deadCode_);
    popValueStackTo(ifThenElse.stackSize);

    if (!deadCode_)
        masm.jump(&ifThenElse.label);

    if (ifThenElse.otherLabel.used())
        masm.bind(&ifThenElse.otherLabel);

    // Reset to the "else" branch.

    if (!deadCode_) {
        freeJoinRegUnlessVoid(r);
        ifThenElse.bceSafeOnExit &= bceSafe_;
    }

    deadCode_ = ifThenElse.deadOnArrival;
    bceSafe_ = ifThenElse.bceSafeOnEntry;

    return true;
}

void
BaseCompiler::endIfThenElse(ExprType type)
{
    Control& ifThenElse = controlItem();

    // The expression type is not a reliable guide to what we'll find
    // on the stack, we could have (if E (i32.const 1) (unreachable))
    // in which case the "else" arm is AnyType but the type of the
    // full expression is I32.  So restore whatever's there, not what
    // we want to find there.  The "then" arm has the same constraint.

    Maybe<AnyReg> r;
    if (!deadCode_) {
        r = popJoinRegUnlessVoid(type);
        ifThenElse.bceSafeOnExit &= bceSafe_;
    }

    fr.popStackOnBlockExit(ifThenElse.stackHeight, deadCode_);
    popValueStackTo(ifThenElse.stackSize);

    if (ifThenElse.label.used())
        masm.bind(&ifThenElse.label);

    bool joinLive = !ifThenElse.deadOnArrival &&
                    (!ifThenElse.deadThenBranch || !deadCode_ || ifThenElse.label.bound());

    if (joinLive) {
        // No value was provided by the "then" path but capture the one
        // provided by the "else" path.
        if (deadCode_)
            r = captureJoinRegUnlessVoid(type);
        deadCode_ = false;
    }

    bceSafe_ = ifThenElse.bceSafeOnExit;

    if (!deadCode_)
        pushJoinRegUnlessVoid(r);
}

bool
BaseCompiler::emitEnd()
{
    LabelKind kind;
    ExprType type;
    Nothing unused_value;
    if (!iter_.readEnd(&kind, &type, &unused_value))
        return false;

    switch (kind) {
      case LabelKind::Block: endBlock(type); break;
      case LabelKind::Loop:  endLoop(type); break;
      case LabelKind::Then:  endIfThen(); break;
      case LabelKind::Else:  endIfThenElse(type); break;
    }

    iter_.popEnd();

    return true;
}

bool
BaseCompiler::emitBr()
{
    uint32_t relativeDepth;
    ExprType type;
    Nothing unused_value;
    if (!iter_.readBr(&relativeDepth, &type, &unused_value))
        return false;

    if (deadCode_)
        return true;

    Control& target = controlItem(relativeDepth);
    target.bceSafeOnExit &= bceSafe_;

    // Save any value in the designated join register, where the
    // normal block exit code will also leave it.

    Maybe<AnyReg> r = popJoinRegUnlessVoid(type);

    fr.popStackBeforeBranch(target.stackHeight);
    masm.jump(&target.label);

    // The register holding the join value is free for the remainder
    // of this block.

    freeJoinRegUnlessVoid(r);

    deadCode_ = true;

    return true;
}

bool
BaseCompiler::emitBrIf()
{
    uint32_t relativeDepth;
    ExprType type;
    Nothing unused_value, unused_condition;
    if (!iter_.readBrIf(&relativeDepth, &type, &unused_value, &unused_condition))
        return false;

    if (deadCode_) {
        resetLatentOp();
        return true;
    }

    Control& target = controlItem(relativeDepth);
    target.bceSafeOnExit &= bceSafe_;

    BranchState b(&target.label, target.stackHeight, InvertBranch(false), type);
    emitBranchSetup(&b);
    emitBranchPerform(&b);

    return true;
}

bool
BaseCompiler::emitBrTable()
{
    Uint32Vector depths;
    uint32_t defaultDepth;
    ExprType branchValueType;
    Nothing unused_value, unused_index;
    if (!iter_.readBrTable(&depths, &defaultDepth, &branchValueType, &unused_value, &unused_index))
        return false;

    if (deadCode_)
        return true;

    // Don't use joinReg for rc
    maybeReserveJoinRegI(branchValueType);

    // Table switch value always on top.
    RegI32 rc = popI32();

    maybeUnreserveJoinRegI(branchValueType);

    Maybe<AnyReg> r = popJoinRegUnlessVoid(branchValueType);

    Label dispatchCode;
    masm.branch32(Assembler::Below, rc, Imm32(depths.length()), &dispatchCode);

    // This is the out-of-range stub.  rc is dead here but we don't need it.

    fr.popStackBeforeBranch(controlItem(defaultDepth).stackHeight);
    controlItem(defaultDepth).bceSafeOnExit &= bceSafe_;
    masm.jump(&controlItem(defaultDepth).label);

    // Emit stubs.  rc is dead in all of these but we don't need it.
    //
    // The labels in the vector are in the TempAllocator and will
    // be freed by and by.
    //
    // TODO / OPTIMIZE (Bug 1316804): Branch directly to the case code if we
    // can, don't emit an intermediate stub.

    LabelVector stubs;
    if (!stubs.reserve(depths.length()))
        return false;

    for (uint32_t depth : depths) {
        stubs.infallibleEmplaceBack(NonAssertingLabel());
        masm.bind(&stubs.back());
        fr.popStackBeforeBranch(controlItem(depth).stackHeight);
        controlItem(depth).bceSafeOnExit &= bceSafe_;
        masm.jump(&controlItem(depth).label);
    }

    // Emit table.

    Label theTable;
    jumpTable(stubs, &theTable);

    // Emit indirect jump.  rc is live here.

    tableSwitch(&theTable, rc, &dispatchCode);

    deadCode_ = true;

    // Clean up.

    freeI32(rc);
    freeJoinRegUnlessVoid(r);

    return true;
}

bool
BaseCompiler::emitDrop()
{
    if (!iter_.readDrop())
        return false;

    if (deadCode_)
        return true;

    dropValue();
    return true;
}

void
BaseCompiler::doReturn(ExprType type, bool popStack)
{
    switch (type) {
      case ExprType::Void: {
        returnCleanup(popStack);
        break;
      }
      case ExprType::I32: {
        RegI32 rv = popI32(RegI32(ReturnReg));
        returnCleanup(popStack);
        freeI32(rv);
        break;
      }
      case ExprType::I64: {
        RegI64 rv = popI64(RegI64(ReturnReg64));
        returnCleanup(popStack);
        freeI64(rv);
        break;
      }
      case ExprType::F64: {
        RegF64 rv = popF64(RegF64(ReturnDoubleReg));
        returnCleanup(popStack);
        freeF64(rv);
        break;
      }
      case ExprType::F32: {
        RegF32 rv = popF32(RegF32(ReturnFloat32Reg));
        returnCleanup(popStack);
        freeF32(rv);
        break;
      }
      default: {
        MOZ_CRASH("Function return type");
      }
    }
}

bool
BaseCompiler::emitReturn()
{
    Nothing unused_value;
    if (!iter_.readReturn(&unused_value))
        return false;

    if (deadCode_)
        return true;

    doReturn(sig().ret(), PopStack(true));
    deadCode_ = true;

    return true;
}

bool
BaseCompiler::emitCallArgs(const ValTypeVector& argTypes, FunctionCall& baselineCall)
{
    MOZ_ASSERT(!deadCode_);

    startCallArgs(baselineCall, stackArgAreaSize(argTypes));

    uint32_t numArgs = argTypes.length();
    for (size_t i = 0; i < numArgs; ++i)
        passArg(baselineCall, argTypes[i], peek(numArgs - 1 - i));

    masm.loadWasmTlsRegFromFrame();
    return true;
}

void
BaseCompiler::pushReturned(const FunctionCall& call, ExprType type)
{
    switch (type) {
      case ExprType::Void:
        MOZ_CRASH("Compiler bug: attempt to push void return");
        break;
      case ExprType::I32: {
        RegI32 rv = captureReturnedI32();
        pushI32(rv);
        break;
      }
      case ExprType::I64: {
        RegI64 rv = captureReturnedI64();
        pushI64(rv);
        break;
      }
      case ExprType::F32: {
        RegF32 rv = captureReturnedF32(call);
        pushF32(rv);
        break;
      }
      case ExprType::F64: {
        RegF64 rv = captureReturnedF64(call);
        pushF64(rv);
        break;
      }
      default:
        MOZ_CRASH("Function return type");
    }
}

// For now, always sync() at the beginning of the call to easily save live
// values.
//
// TODO / OPTIMIZE (Bug 1316806): We may be able to avoid a full sync(), since
// all we want is to save live registers that won't be saved by the callee or
// that we need for outgoing args - we don't need to sync the locals.  We can
// just push the necessary registers, it'll be like a lightweight sync.
//
// Even some of the pushing may be unnecessary if the registers will be consumed
// by the call, because then what we want is parallel assignment to the argument
// registers or onto the stack for outgoing arguments.  A sync() is just
// simpler.

bool
BaseCompiler::emitCall()
{
    uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

    uint32_t funcIndex;
    BaseOpIter::ValueVector args_;
    if (!iter_.readCall(&funcIndex, &args_))
        return false;

    if (deadCode_)
        return true;

    sync();

    const Sig& sig = *env_.funcSigs[funcIndex];
    bool import = env_.funcIsImport(funcIndex);

    uint32_t numArgs = sig.args().length();
    size_t stackSpace = stackConsumed(numArgs);

    FunctionCall baselineCall(lineOrBytecode);
    beginCall(baselineCall, UseABI::Wasm, import ? InterModule::True : InterModule::False);

    if (!emitCallArgs(sig.args(), baselineCall))
        return false;

    if (import)
        callImport(env_.funcImportGlobalDataOffsets[funcIndex], baselineCall);
    else
        callDefinition(funcIndex, baselineCall);

    endCall(baselineCall, stackSpace);

    popValueStackBy(numArgs);

    if (!IsVoid(sig.ret()))
        pushReturned(baselineCall, sig.ret());

    return true;
}

bool
BaseCompiler::emitCallIndirect()
{
    uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

    uint32_t sigIndex;
    Nothing callee_;
    BaseOpIter::ValueVector args_;
    if (!iter_.readCallIndirect(&sigIndex, &callee_, &args_))
        return false;

    if (deadCode_)
        return true;

    sync();

    const SigWithId& sig = env_.sigs[sigIndex];

    // Stack: ... arg1 .. argn callee

    uint32_t numArgs = sig.args().length();
    size_t stackSpace = stackConsumed(numArgs + 1);

    // The arguments must be at the stack top for emitCallArgs, so pop the
    // callee if it is on top.  Note this only pops the compiler's stack,
    // not the CPU stack.

    Stk callee = stk_.popCopy();

    FunctionCall baselineCall(lineOrBytecode);
    beginCall(baselineCall, UseABI::Wasm, InterModule::True);

    if (!emitCallArgs(sig.args(), baselineCall))
        return false;

    callIndirect(sigIndex, callee, baselineCall);

    endCall(baselineCall, stackSpace);

    popValueStackBy(numArgs);

    if (!IsVoid(sig.ret()))
        pushReturned(baselineCall, sig.ret());

    return true;
}

void
BaseCompiler::emitRound(RoundingMode roundingMode, ValType operandType)
{
    if (operandType == ValType::F32) {
        RegF32 f0 = popF32();
        roundF32(roundingMode, f0);
        pushF32(f0);
    } else if (operandType == ValType::F64) {
        RegF64 f0 = popF64();
        roundF64(roundingMode, f0);
        pushF64(f0);
    } else {
        MOZ_CRASH("unexpected type");
    }
}

bool
BaseCompiler::emitUnaryMathBuiltinCall(SymbolicAddress callee, ValType operandType)
{
    uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

    Nothing operand_;
    if (!iter_.readUnary(operandType, &operand_))
        return false;

    if (deadCode_)
        return true;

    RoundingMode roundingMode;
    if (IsRoundingFunction(callee, &roundingMode) && supportsRoundInstruction(roundingMode)) {
        emitRound(roundingMode, operandType);
        return true;
    }

    sync();

    ValTypeVector& signature = operandType == ValType::F32 ? SigF_ : SigD_;
    ExprType retType = operandType == ValType::F32 ? ExprType::F32 : ExprType::F64;
    uint32_t numArgs = signature.length();
    size_t stackSpace = stackConsumed(numArgs);

    FunctionCall baselineCall(lineOrBytecode);
    beginCall(baselineCall, UseABI::System, InterModule::False);

    if (!emitCallArgs(signature, baselineCall))
        return false;

    builtinCall(callee, baselineCall);

    endCall(baselineCall, stackSpace);

    popValueStackBy(numArgs);

    pushReturned(baselineCall, retType);

    return true;
}

#ifdef RABALDR_INT_DIV_I64_CALLOUT
void
BaseCompiler::emitDivOrModI64BuiltinCall(SymbolicAddress callee, ValType operandType)
{
    MOZ_ASSERT(operandType == ValType::I64);
    MOZ_ASSERT(!deadCode_);

    sync();

    needI64(specific.abiReturnRegI64);

    RegI64 rhs = popI64();
    RegI64 srcDest = popI64ToSpecific(specific.abiReturnRegI64);

    Label done;

    checkDivideByZeroI64(rhs);

    if (callee == SymbolicAddress::DivI64)
        checkDivideSignedOverflowI64(rhs, srcDest, &done, ZeroOnOverflow(false));
    else if (callee == SymbolicAddress::ModI64)
        checkDivideSignedOverflowI64(rhs, srcDest, &done, ZeroOnOverflow(true));

    masm.setupWasmABICall();
    masm.passABIArg(srcDest.high);
    masm.passABIArg(srcDest.low);
    masm.passABIArg(rhs.high);
    masm.passABIArg(rhs.low);
    masm.callWithABI(bytecodeOffset(), callee);

    masm.bind(&done);

    freeI64(rhs);
    pushI64(srcDest);
}
#endif // RABALDR_INT_DIV_I64_CALLOUT

#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
bool
BaseCompiler::emitConvertInt64ToFloatingCallout(SymbolicAddress callee, ValType operandType,
                                                ValType resultType)
{
    sync();

    RegI64 input = popI64();

    FunctionCall call(0);

    masm.setupWasmABICall();
# ifdef JS_PUNBOX64
    MOZ_CRASH("BaseCompiler platform hook: emitConvertInt64ToFloatingCallout");
# else
    masm.passABIArg(input.high);
    masm.passABIArg(input.low);
# endif
    masm.callWithABI(bytecodeOffset(), callee,
                     resultType == ValType::F32 ? MoveOp::FLOAT32 : MoveOp::DOUBLE);

    freeI64(input);

    if (resultType == ValType::F32)
        pushF32(captureReturnedF32(call));
    else
        pushF64(captureReturnedF64(call));

    return true;
}
#endif // RABALDR_I64_TO_FLOAT_CALLOUT

#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
// `Callee` always takes a double, so a float32 input must be converted.
bool
BaseCompiler::emitConvertFloatingToInt64Callout(SymbolicAddress callee, ValType operandType,
                                                ValType resultType)
{
    RegF64 doubleInput;
    if (operandType == ValType::F32) {
        doubleInput = needF64();
        RegF32 input = popF32();
        masm.convertFloat32ToDouble(input, doubleInput);
        freeF32(input);
    } else {
        doubleInput = popF64();
    }

    // We may need the value after the call for the ool check.
    RegF64 otherReg = needF64();
    moveF64(doubleInput, otherReg);
    pushF64(otherReg);

    sync();

    FunctionCall call(0);

    masm.setupWasmABICall();
    masm.passABIArg(doubleInput, MoveOp::DOUBLE);
    masm.callWithABI(bytecodeOffset(), callee);

    freeF64(doubleInput);

    RegI64 rv = captureReturnedI64();

    RegF64 inputVal = popF64();

    TruncFlags flags = 0;
    if (callee == SymbolicAddress::TruncateDoubleToUint64)
        flags |= TRUNC_UNSIGNED;
    if (callee == SymbolicAddress::SaturatingTruncateDoubleToInt64 ||
        callee == SymbolicAddress::SaturatingTruncateDoubleToUint64) {
        flags |= TRUNC_SATURATING;
    }

    // If we're saturating, the callout will always produce the final result
    // value. Otherwise, the callout value will return 0x8000000000000000
    // and we need to produce traps.
    OutOfLineCode* ool = nullptr;
    if (!(flags & TRUNC_SATURATING)) {
        // The OOL check just succeeds or fails, it does not generate a value.
        ool = addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(AnyReg(inputVal),
                                                                                rv, flags,
                                                                                bytecodeOffset()));
        if (!ool)
            return false;

        masm.branch64(Assembler::Equal, rv, Imm64(0x8000000000000000), ool->entry());
        masm.bind(ool->rejoin());
    }

    pushI64(rv);
    freeF64(inputVal);

    return true;
}
#endif // RABALDR_FLOAT_TO_I64_CALLOUT

bool
BaseCompiler::emitGetLocal()
{
    uint32_t slot;
    if (!iter_.readGetLocal(locals_, &slot))
        return false;

    if (deadCode_)
        return true;

    // Local loads are pushed unresolved, ie, they may be deferred
    // until needed, until they may be affected by a store, or until a
    // sync.  This is intended to reduce register pressure.

    switch (locals_[slot]) {
      case ValType::I32:
        pushLocalI32(slot);
        break;
      case ValType::I64:
        pushLocalI64(slot);
        break;
      case ValType::F64:
        pushLocalF64(slot);
        break;
      case ValType::F32:
        pushLocalF32(slot);
        break;
      default:
        MOZ_CRASH("Local variable type");
    }

    return true;
}

template<bool isSetLocal>
bool
BaseCompiler::emitSetOrTeeLocal(uint32_t slot)
{
    if (deadCode_)
        return true;

    bceLocalIsUpdated(slot);
    switch (locals_[slot]) {
      case ValType::I32: {
        RegI32 rv = popI32();
        syncLocal(slot);
        fr.storeLocalI32(rv, localFromSlot(slot, MIRType::Int32));
        if (isSetLocal)
            freeI32(rv);
        else
            pushI32(rv);
        break;
      }
      case ValType::I64: {
        RegI64 rv = popI64();
        syncLocal(slot);
        fr.storeLocalI64(rv, localFromSlot(slot, MIRType::Int64));
        if (isSetLocal)
            freeI64(rv);
        else
            pushI64(rv);
        break;
      }
      case ValType::F64: {
        RegF64 rv = popF64();
        syncLocal(slot);
        fr.storeLocalF64(rv, localFromSlot(slot, MIRType::Double));
        if (isSetLocal)
            freeF64(rv);
        else
            pushF64(rv);
        break;
      }
      case ValType::F32: {
        RegF32 rv = popF32();
        syncLocal(slot);
        fr.storeLocalF32(rv, localFromSlot(slot, MIRType::Float32));
        if (isSetLocal)
            freeF32(rv);
        else
            pushF32(rv);
        break;
      }
      default:
        MOZ_CRASH("Local variable type");
    }

    return true;
}

bool
BaseCompiler::emitSetLocal()
{
    uint32_t slot;
    Nothing unused_value;
    if (!iter_.readSetLocal(locals_, &slot, &unused_value))
        return false;
    return emitSetOrTeeLocal<true>(slot);
}

bool
BaseCompiler::emitTeeLocal()
{
    uint32_t slot;
    Nothing unused_value;
    if (!iter_.readTeeLocal(locals_, &slot, &unused_value))
        return false;
    return emitSetOrTeeLocal<false>(slot);
}

bool
BaseCompiler::emitGetGlobal()
{
    uint32_t id;
    if (!iter_.readGetGlobal(&id))
        return false;

    if (deadCode_)
        return true;

    const GlobalDesc& global = env_.globals[id];

    if (global.isConstant()) {
        Val value = global.constantValue();
        switch (value.type()) {
          case ValType::I32:
            pushI32(value.i32());
            break;
          case ValType::I64:
            pushI64(value.i64());
            break;
          case ValType::F32:
            pushF32(value.f32());
            break;
          case ValType::F64:
            pushF64(value.f64());
            break;
          default:
            MOZ_CRASH("Global constant type");
        }
        return true;
    }

    switch (global.type()) {
      case ValType::I32: {
        RegI32 rv = needI32();
        loadGlobalVarI32(global.offset(), rv);
        pushI32(rv);
        break;
      }
      case ValType::I64: {
        RegI64 rv = needI64();
        loadGlobalVarI64(global.offset(), rv);
        pushI64(rv);
        break;
      }
      case ValType::F32: {
        RegF32 rv = needF32();
        loadGlobalVarF32(global.offset(), rv);
        pushF32(rv);
        break;
      }
      case ValType::F64: {
        RegF64 rv = needF64();
        loadGlobalVarF64(global.offset(), rv);
        pushF64(rv);
        break;
      }
      default:
        MOZ_CRASH("Global variable type");
        break;
    }
    return true;
}

bool
BaseCompiler::emitSetGlobal()
{
    uint32_t id;
    Nothing unused_value;
    if (!iter_.readSetGlobal(&id, &unused_value))
        return false;

    if (deadCode_)
        return true;

    const GlobalDesc& global = env_.globals[id];

    switch (global.type()) {
      case ValType::I32: {
        RegI32 rv = popI32();
        storeGlobalVarI32(global.offset(), rv);
        freeI32(rv);
        break;
      }
      case ValType::I64: {
        RegI64 rv = popI64();
        storeGlobalVarI64(global.offset(), rv);
        freeI64(rv);
        break;
      }
      case ValType::F32: {
        RegF32 rv = popF32();
        storeGlobalVarF32(global.offset(), rv);
        freeF32(rv);
        break;
      }
      case ValType::F64: {
        RegF64 rv = popF64();
        storeGlobalVarF64(global.offset(), rv);
        freeF64(rv);
        break;
      }
      default:
        MOZ_CRASH("Global variable type");
        break;
    }
    return true;
}

// Bounds check elimination.
//
// We perform BCE on two kinds of address expressions: on constant heap pointers
// that are known to be in the heap or will be handled by the out-of-bounds trap
// handler; and on local variables that have been checked in dominating code
// without being updated since.
//
// For an access through a constant heap pointer + an offset we can eliminate
// the bounds check if the sum of the address and offset is below the sum of the
// minimum memory length and the offset guard length.
//
// For an access through a local variable + an offset we can eliminate the
// bounds check if the local variable has already been checked and has not been
// updated since, and the offset is less than the guard limit.
//
// To track locals for which we can eliminate checks we use a bit vector
// bceSafe_ that has a bit set for those locals whose bounds have been checked
// and which have not subsequently been set.  Initially this vector is zero.
//
// In straight-line code a bit is set when we perform a bounds check on an
// access via the local and is reset when the variable is updated.
//
// In control flow, the bit vector is manipulated as follows.  Each ControlItem
// has a value bceSafeOnEntry, which is the value of bceSafe_ on entry to the
// item, and a value bceSafeOnExit, which is initially ~0.  On a branch (br,
// brIf, brTable), we always AND the branch target's bceSafeOnExit with the
// value of bceSafe_ at the branch point.  On exiting an item by falling out of
// it, provided we're not in dead code, we AND the current value of bceSafe_
// into the item's bceSafeOnExit.  Additional processing depends on the item
// type:
//
//  - After a block, set bceSafe_ to the block's bceSafeOnExit.
//
//  - On loop entry, after pushing the ControlItem, set bceSafe_ to zero; the
//    back edges would otherwise require us to iterate to a fixedpoint.
//
//  - After a loop, the bceSafe_ is left unchanged, because only fallthrough
//    control flow will reach that point and the bceSafe_ value represents the
//    correct state of the fallthrough path.
//
//  - Set bceSafe_ to the ControlItem's bceSafeOnEntry at both the 'then' branch
//    and the 'else' branch.
//
//  - After an if-then-else, set bceSafe_ to the if-then-else's bceSafeOnExit.
//
//  - After an if-then, set bceSafe_ to the if-then's bceSafeOnExit AND'ed with
//    the if-then's bceSafeOnEntry.
//
// Finally, when the debugger allows locals to be mutated we must disable BCE
// for references via a local, by returning immediately from bceCheckLocal if
// debugEnabled_ is true.
//
//
// Alignment check elimination.
//
// Alignment checks for atomic operations can be omitted if the pointer is a
// constant and the pointer + offset is aligned.  Alignment checking that can't
// be omitted can still be simplified by checking only the pointer if the offset
// is aligned.
//
// (In addition, alignment checking of the pointer can be omitted if the pointer
// has been checked in dominating code, but we don't do that yet.)

// TODO / OPTIMIZE (bug 1329576): There are opportunities to generate better
// code by not moving a constant address with a zero offset into a register.

RegI32
BaseCompiler::popMemoryAccess(MemoryAccessDesc* access, AccessCheck* check)
{
    check->onlyPointerAlignment = (access->offset() & (access->byteSize() - 1)) == 0;

    int32_t addrTemp;
    if (popConstI32(&addrTemp)) {
        uint32_t addr = addrTemp;

        uint64_t ea = uint64_t(addr) + uint64_t(access->offset());
        uint64_t limit = uint64_t(env_.minMemoryLength) + uint64_t(wasm::OffsetGuardLimit);

        check->omitBoundsCheck = ea < limit;
        check->omitAlignmentCheck = (ea & (access->byteSize() - 1)) == 0;

        // Fold the offset into the pointer if we can, as this is always
        // beneficial.

        if (ea <= UINT32_MAX) {
            addr = uint32_t(ea);
            access->clearOffset();
        }

        RegI32 r = needI32();
        moveImm32(int32_t(addr), r);
        return r;
    }

    uint32_t local;
    if (peekLocalI32(&local))
        bceCheckLocal(access, check, local);

    return popI32();
}

RegI32
BaseCompiler::maybeLoadTlsForAccess(const AccessCheck& check)
{
    RegI32 tls;
    if (needTlsForAccess(check)) {
        tls = needI32();
        masm.loadWasmTlsRegFromFrame(tls);
    }
    return tls;
}

RegI32
BaseCompiler::maybeLoadTlsForAccess(const AccessCheck& check, RegI32 specific)
{
    if (needTlsForAccess(check)) {
        masm.loadWasmTlsRegFromFrame(specific);
        return specific;
    }
    return RegI32::Invalid();
}

bool
BaseCompiler::loadCommon(MemoryAccessDesc* access, ValType type)
{
    AccessCheck check;

    RegI32 tls, temp1, temp2, temp3;
    needLoadTemps(*access, &temp1, &temp2, &temp3);

    switch (type) {
      case ValType::I32: {
        RegI32 rp = popMemoryAccess(access, &check);
#ifdef JS_CODEGEN_ARM
        RegI32 rv = IsUnaligned(*access) ? needI32() : rp;
#else
        RegI32 rv = rp;
#endif
        tls = maybeLoadTlsForAccess(check);
        if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3))
            return false;
        pushI32(rv);
        if (rp != rv)
            freeI32(rp);
        break;
      }
      case ValType::I64: {
        RegI64 rv;
        RegI32 rp;
#ifdef JS_CODEGEN_X86
        rv = specific.abiReturnRegI64;
        needI64(rv);
        rp = popMemoryAccess(access, &check);
#else
        rp = popMemoryAccess(access, &check);
        rv = needI64();
#endif
        tls = maybeLoadTlsForAccess(check);
        if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3))
            return false;
        pushI64(rv);
        freeI32(rp);
        break;
      }
      case ValType::F32: {
        RegI32 rp = popMemoryAccess(access, &check);
        RegF32 rv = needF32();
        tls = maybeLoadTlsForAccess(check);
        if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3))
            return false;
        pushF32(rv);
        freeI32(rp);
        break;
      }
      case ValType::F64: {
        RegI32 rp = popMemoryAccess(access, &check);
        RegF64 rv = needF64();
        tls = maybeLoadTlsForAccess(check);
        if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3))
            return false;
        pushF64(rv);
        freeI32(rp);
        break;
      }
      default:
        MOZ_CRASH("load type");
        break;
    }

    maybeFreeI32(tls);
    maybeFreeI32(temp1);
    maybeFreeI32(temp2);
    maybeFreeI32(temp3);

    return true;
}

bool
BaseCompiler::emitLoad(ValType type, Scalar::Type viewType)
{
    LinearMemoryAddress<Nothing> addr;
    if (!iter_.readLoad(type, Scalar::byteSize(viewType), &addr))
        return false;

    if (deadCode_)
        return true;

    MemoryAccessDesc access(viewType, addr.align, addr.offset, Some(bytecodeOffset()));
    return loadCommon(&access, type);
}

bool
BaseCompiler::storeCommon(MemoryAccessDesc* access, ValType resultType)
{
    AccessCheck check;

    RegI32 tls;
    RegI32 temp = needStoreTemp(*access, resultType);

    switch (resultType) {
      case ValType::I32: {
        RegI32 rv = popI32();
        RegI32 rp = popMemoryAccess(access, &check);
        tls = maybeLoadTlsForAccess(check);
        if (!store(access, &check, tls, rp, AnyReg(rv), temp))
            return false;
        freeI32(rp);
        freeI32(rv);
        break;
      }
      case ValType::I64: {
        RegI64 rv = popI64();
        RegI32 rp = popMemoryAccess(access, &check);
        tls = maybeLoadTlsForAccess(check);
        if (!store(access, &check, tls, rp, AnyReg(rv), temp))
            return false;
        freeI32(rp);
        freeI64(rv);
        break;
      }
      case ValType::F32: {
        RegF32 rv = popF32();
        RegI32 rp = popMemoryAccess(access, &check);
        tls = maybeLoadTlsForAccess(check);
        if (!store(access, &check, tls, rp, AnyReg(rv), temp))
            return false;
        freeI32(rp);
        freeF32(rv);
        break;
      }
      case ValType::F64: {
        RegF64 rv = popF64();
        RegI32 rp = popMemoryAccess(access, &check);
        tls = maybeLoadTlsForAccess(check);
        if (!store(access, &check, tls, rp, AnyReg(rv), temp))
            return false;
        freeI32(rp);
        freeF64(rv);
        break;
      }
      default:
        MOZ_CRASH("store type");
        break;
    }

    maybeFreeI32(tls);
    maybeFreeI32(temp);

    return true;
}

bool
BaseCompiler::emitStore(ValType resultType, Scalar::Type viewType)
{
    LinearMemoryAddress<Nothing> addr;
    Nothing unused_value;
    if (!iter_.readStore(resultType, Scalar::byteSize(viewType), &addr, &unused_value))
        return false;

    if (deadCode_)
        return true;

    MemoryAccessDesc access(viewType, addr.align, addr.offset, Some(bytecodeOffset()));
    return storeCommon(&access, resultType);
}

bool
BaseCompiler::emitSelect()
{
    StackType type;
    Nothing unused_trueValue;
    Nothing unused_falseValue;
    Nothing unused_condition;
    if (!iter_.readSelect(&type, &unused_trueValue, &unused_falseValue, &unused_condition))
        return false;

    if (deadCode_) {
        resetLatentOp();
        return true;
    }

    // I32 condition on top, then false, then true.

    Label done;
    BranchState b(&done);
    emitBranchSetup(&b);

    switch (NonAnyToValType(type)) {
      case ValType::I32: {
        RegI32 r, rs;
        pop2xI32(&r, &rs);
        emitBranchPerform(&b);
        moveI32(rs, r);
        masm.bind(&done);
        freeI32(rs);
        pushI32(r);
        break;
      }
      case ValType::I64: {
#ifdef JS_CODEGEN_X86
        // There may be as many as four Int64 values in registers at a time: two
        // for the latent branch operands, and two for the true/false values we
        // normally pop before executing the branch.  On x86 this is one value
        // too many, so we need to generate more complicated code here, and for
        // simplicity's sake we do so even if the branch operands are not Int64.
        // However, the resulting control flow diamond is complicated since the
        // arms of the diamond will have to stay synchronized with respect to
        // their evaluation stack and regalloc state.  To simplify further, we
        // use a double branch and a temporary boolean value for now.
        RegI32 temp = needI32();
        moveImm32(0, temp);
        emitBranchPerform(&b);
        moveImm32(1, temp);
        masm.bind(&done);

        Label trueValue;
        RegI64 r, rs;
        pop2xI64(&r, &rs);
        masm.branch32(Assembler::Equal, temp, Imm32(0), &trueValue);
        moveI64(rs, r);
        masm.bind(&trueValue);
        freeI32(temp);
        freeI64(rs);
        pushI64(r);
#else
        RegI64 r, rs;
        pop2xI64(&r, &rs);
        emitBranchPerform(&b);
        moveI64(rs, r);
        masm.bind(&done);
        freeI64(rs);
        pushI64(r);
#endif
        break;
      }
      case ValType::F32: {
        RegF32 r, rs;
        pop2xF32(&r, &rs);
        emitBranchPerform(&b);
        moveF32(rs, r);
        masm.bind(&done);
        freeF32(rs);
        pushF32(r);
        break;
      }
      case ValType::F64: {
        RegF64 r, rs;
        pop2xF64(&r, &rs);
        emitBranchPerform(&b);
        moveF64(rs, r);
        masm.bind(&done);
        freeF64(rs);
        pushF64(r);
        break;
      }
      default: {
        MOZ_CRASH("select type");
      }
    }

    return true;
}

void
BaseCompiler::emitCompareI32(Assembler::Condition compareOp, ValType compareType)
{
    MOZ_ASSERT(compareType == ValType::I32);

    if (sniffConditionalControlCmp(compareOp, compareType))
        return;

    int32_t c;
    if (popConstI32(&c)) {
        RegI32 r = popI32();
        masm.cmp32Set(compareOp, r, Imm32(c), r);
        pushI32(r);
    } else {
        RegI32 r, rs;
        pop2xI32(&r, &rs);
        masm.cmp32Set(compareOp, r, rs, r);
        freeI32(rs);
        pushI32(r);
    }
}

void
BaseCompiler::emitCompareI64(Assembler::Condition compareOp, ValType compareType)
{
    MOZ_ASSERT(compareType == ValType::I64);

    if (sniffConditionalControlCmp(compareOp, compareType))
        return;

    RegI64 rs0, rs1;
    pop2xI64(&rs0, &rs1);
    RegI32 rd(fromI64(rs0));
    cmp64Set(compareOp, rs0, rs1, rd);
    freeI64(rs1);
    freeI64Except(rs0, rd);
    pushI32(rd);
}

void
BaseCompiler::emitCompareF32(Assembler::DoubleCondition compareOp, ValType compareType)
{
    MOZ_ASSERT(compareType == ValType::F32);

    if (sniffConditionalControlCmp(compareOp, compareType))
        return;

    Label across;
    RegF32 rs0, rs1;
    pop2xF32(&rs0, &rs1);
    RegI32 rd = needI32();
    moveImm32(1, rd);
    masm.branchFloat(compareOp, rs0, rs1, &across);
    moveImm32(0, rd);
    masm.bind(&across);
    freeF32(rs0);
    freeF32(rs1);
    pushI32(rd);
}

void
BaseCompiler::emitCompareF64(Assembler::DoubleCondition compareOp, ValType compareType)
{
    MOZ_ASSERT(compareType == ValType::F64);

    if (sniffConditionalControlCmp(compareOp, compareType))
        return;

    Label across;
    RegF64 rs0, rs1;
    pop2xF64(&rs0, &rs1);
    RegI32 rd = needI32();
    moveImm32(1, rd);
    masm.branchDouble(compareOp, rs0, rs1, &across);
    moveImm32(0, rd);
    masm.bind(&across);
    freeF64(rs0);
    freeF64(rs1);
    pushI32(rd);
}

void
BaseCompiler::emitInstanceCall(uint32_t lineOrBytecode, const MIRTypeVector& sig,
                               ExprType retType, SymbolicAddress builtin)
{
    MOZ_ASSERT(sig[0] == MIRType::Pointer);

    sync();

    uint32_t numArgs = sig.length() - 1 /* instance */;
    size_t stackSpace = stackConsumed(numArgs);

    FunctionCall baselineCall(lineOrBytecode);
    beginCall(baselineCall, UseABI::System, InterModule::True);

    ABIArg instanceArg = reservePointerArgument(baselineCall);

    startCallArgs(baselineCall, stackArgAreaSize(sig));
    for (uint32_t i = 1; i < sig.length(); i++) {
        ValType t;
        switch (sig[i]) {
          case MIRType::Int32: t = ValType::I32; break;
          case MIRType::Int64: t = ValType::I64; break;
          default:             MOZ_CRASH("Unexpected type");
        }
        passArg(baselineCall, t, peek(numArgs - i));
    }
    builtinInstanceMethodCall(builtin, instanceArg, baselineCall);
    endCall(baselineCall, stackSpace);

    popValueStackBy(numArgs);

    pushReturned(baselineCall, retType);
}

bool
BaseCompiler::emitGrowMemory()
{
    uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

    Nothing arg;
    if (!iter_.readGrowMemory(&arg))
        return false;

    if (deadCode_)
        return true;

    emitInstanceCall(lineOrBytecode, SigPI_, ExprType::I32, SymbolicAddress::GrowMemory);
    return true;
}

bool
BaseCompiler::emitCurrentMemory()
{
    uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

    if (!iter_.readCurrentMemory())
        return false;

    if (deadCode_)
        return true;

    emitInstanceCall(lineOrBytecode, SigP_, ExprType::I32, SymbolicAddress::CurrentMemory);
    return true;
}

bool
BaseCompiler::emitAtomicCmpXchg(ValType type, Scalar::Type viewType)
{
    LinearMemoryAddress<Nothing> addr;
    Nothing unused;

    if (!iter_.readAtomicCmpXchg(&addr, type, Scalar::byteSize(viewType), &unused, &unused))
        return false;

    if (deadCode_)
        return true;

    MemoryAccessDesc access(viewType, addr.align, addr.offset, Some(bytecodeOffset()),
                            /*numSimdExprs=*/ 0, Synchronization::Full());

    if (Scalar::byteSize(viewType) <= 4) {
        PopAtomicCmpXchg32Regs regs(this, type, viewType);

        AccessCheck check;
        RegI32 rp = popMemoryAccess(&access, &check);
        RegI32 tls = maybeLoadTlsForAccess(check);

        regs.atomicCmpXchg32(prepareAtomicMemoryAccess(&access, &check, tls, rp), viewType);

        maybeFreeI32(tls);
        freeI32(rp);

        if (type == ValType::I64)
            pushU32AsI64(regs.takeRd());
        else
            pushI32(regs.takeRd());

        return true;
    }

    MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

    PopAtomicCmpXchg64Regs regs(this);

    AccessCheck check;
    RegI32 rp = popMemoryAccess(&access, &check);

#ifdef JS_CODEGEN_X86
    ScratchEBX ebx(*this);
    RegI32 tls = maybeLoadTlsForAccess(check, ebx);
    regs.atomicCmpXchg64(prepareAtomicMemoryAccess(&access, &check, tls, rp), ebx);
#else
    RegI32 tls = maybeLoadTlsForAccess(check);
    regs.atomicCmpXchg64(prepareAtomicMemoryAccess(&access, &check, tls, rp));
    maybeFreeI32(tls);
#endif

    freeI32(rp);

    pushI64(regs.takeRd());
    return true;
}

bool
BaseCompiler::emitAtomicLoad(ValType type, Scalar::Type viewType)
{
    LinearMemoryAddress<Nothing> addr;
    if (!iter_.readAtomicLoad(&addr, type, Scalar::byteSize(viewType)))
        return false;

    if (deadCode_)
        return true;

    MemoryAccessDesc access(viewType, addr.align, addr.offset, Some(bytecodeOffset()),
                            /*numSimdElems=*/ 0, Synchronization::Load());

    if (Scalar::byteSize(viewType) <= sizeof(void*))
        return loadCommon(&access, type);

    MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

#if defined(JS_64BIT)
    MOZ_CRASH("Should not happen");
#else
    PopAtomicLoad64Regs regs(this);

    AccessCheck check;
    RegI32 rp = popMemoryAccess(&access, &check);

# ifdef JS_CODEGEN_X86
    ScratchEBX ebx(*this);
    RegI32 tls = maybeLoadTlsForAccess(check, ebx);
    regs.atomicLoad64(prepareAtomicMemoryAccess(&access, &check, tls, rp), ebx);
# else
    RegI32 tls = maybeLoadTlsForAccess(check);
    regs.atomicLoad64(prepareAtomicMemoryAccess(&access, &check, tls, rp));
    maybeFreeI32(tls);
# endif

    freeI32(rp);

    pushI64(regs.takeRd());
    return true;
#endif // JS_64BIT
}

bool
BaseCompiler::emitAtomicRMW(ValType type, Scalar::Type viewType, AtomicOp op)
{
    LinearMemoryAddress<Nothing> addr;
    Nothing unused_value;
    if (!iter_.readAtomicRMW(&addr, type, Scalar::byteSize(viewType), &unused_value))
        return false;

    if (deadCode_)
        return true;

    MemoryAccessDesc access(viewType, addr.align, addr.offset, Some(bytecodeOffset()),
                            /*numSimdElems=*/ 0, Synchronization::Full());

    if (Scalar::byteSize(viewType) <= 4) {
        PopAtomicRMW32Regs regs(this, type, viewType, op);

        AccessCheck check;
        RegI32 rp = popMemoryAccess(&access, &check);
        RegI32 tls = maybeLoadTlsForAccess(check);

        regs.atomicRMW32(prepareAtomicMemoryAccess(&access, &check, tls, rp), viewType, op);

        maybeFreeI32(tls);
        freeI32(rp);

        if (type == ValType::I64)
            pushU32AsI64(regs.takeRd());
        else
            pushI32(regs.takeRd());
        return true;
    }

    MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

    PopAtomicRMW64Regs regs(this, op);

    AccessCheck check;
    RegI32 rp = popMemoryAccess(&access, &check);

#ifdef JS_CODEGEN_X86
    ScratchEBX ebx(*this);
    RegI32 tls = maybeLoadTlsForAccess(check, ebx);

    fr.pushPtr(regs.valueHigh());
    fr.pushPtr(regs.valueLow());
    Address value(esp, 0);

    regs.atomicRMW64(prepareAtomicMemoryAccess(&access, &check, tls, rp), op, value, ebx);

    fr.popBytes(8);
#else
    RegI32 tls = maybeLoadTlsForAccess(check);
    regs.atomicRMW64(prepareAtomicMemoryAccess(&access, &check, tls, rp), op);
    maybeFreeI32(tls);
#endif

    freeI32(rp);

    pushI64(regs.takeRd());
    return true;
}

bool
BaseCompiler::emitAtomicStore(ValType type, Scalar::Type viewType)
{
    LinearMemoryAddress<Nothing> addr;
    Nothing unused_value;
    if (!iter_.readAtomicStore(&addr, type, Scalar::byteSize(viewType), &unused_value))
        return false;

    if (deadCode_)
        return true;

    MemoryAccessDesc access(viewType, addr.align, addr.offset, Some(bytecodeOffset()),
                            /*numSimdElems=*/ 0, Synchronization::Store());

    if (Scalar::byteSize(viewType) <= sizeof(void*))
        return storeCommon(&access, type);

    MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

#ifdef JS_64BIT
    MOZ_CRASH("Should not happen");
#else
    emitAtomicXchg64(&access, type, WantResult(false));
    return true;
#endif
}

bool
BaseCompiler::emitAtomicXchg(ValType type, Scalar::Type viewType)
{
    LinearMemoryAddress<Nothing> addr;
    Nothing unused_value;
    if (!iter_.readAtomicRMW(&addr, type, Scalar::byteSize(viewType), &unused_value))
        return false;

    if (deadCode_)
        return true;

    AccessCheck check;
    MemoryAccessDesc access(viewType, addr.align, addr.offset, Some(bytecodeOffset()),
                            /*numSimdElems=*/ 0, Synchronization::Full());

    if (Scalar::byteSize(viewType) <= 4) {
        PopAtomicXchg32Regs regs(this, type, viewType);
        RegI32 rp = popMemoryAccess(&access, &check);
        RegI32 tls = maybeLoadTlsForAccess(check);

        regs.atomicXchg32(prepareAtomicMemoryAccess(&access, &check, tls, rp), viewType);

        maybeFreeI32(tls);
        freeI32(rp);

        if (type == ValType::I64)
            pushU32AsI64(regs.takeRd());
        else
            pushI32(regs.takeRd());
        return true;
    }

    MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

    emitAtomicXchg64(&access, type, WantResult(true));
    return true;
}

void
BaseCompiler::emitAtomicXchg64(MemoryAccessDesc* access, ValType type, WantResult wantResult)
{
    PopAtomicXchg64Regs regs(this);

    AccessCheck check;
    RegI32 rp = popMemoryAccess(access, &check);

#ifdef JS_CODEGEN_X86
    ScratchEBX ebx(*this);
    RegI32 tls = maybeLoadTlsForAccess(check, ebx);
    regs.atomicXchg64(prepareAtomicMemoryAccess(access, &check, tls, rp), ebx);
#else
    RegI32 tls = maybeLoadTlsForAccess(check);
    regs.atomicXchg64(prepareAtomicMemoryAccess(access, &check, tls, rp));
    maybeFreeI32(tls);
#endif

    freeI32(rp);

    if (wantResult)
        pushI64(regs.takeRd());
}

bool
BaseCompiler::emitWait(ValType type, uint32_t byteSize)
{
    uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

    Nothing nothing;
    LinearMemoryAddress<Nothing> addr;
    if (!iter_.readWait(&addr, type, byteSize, &nothing, &nothing))
        return false;

    if (deadCode_)
        return true;

    switch (type) {
      case ValType::I32:
        emitInstanceCall(lineOrBytecode, SigPIIL_, ExprType::I32, SymbolicAddress::WaitI32);
        break;
      case ValType::I64:
        emitInstanceCall(lineOrBytecode, SigPILL_, ExprType::I32, SymbolicAddress::WaitI64);
        break;
      default:
        MOZ_CRASH();
    }

    Label ok;
    masm.branchTest32(Assembler::NotSigned, ReturnReg, ReturnReg, &ok);
    trap(Trap::ThrowReported);
    masm.bind(&ok);

    return true;
}

bool
BaseCompiler::emitWake()
{
    uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

    Nothing nothing;
    LinearMemoryAddress<Nothing> addr;
    if (!iter_.readWake(&addr, &nothing))
        return false;

    if (deadCode_)
        return true;

    emitInstanceCall(lineOrBytecode, SigPII_, ExprType::I32, SymbolicAddress::Wake);

    Label ok;
    masm.branchTest32(Assembler::NotSigned, ReturnReg, ReturnReg, &ok);
    trap(Trap::ThrowReported);
    masm.bind(&ok);

    return true;
}

bool
BaseCompiler::emitBody()
{
    if (!iter_.readFunctionStart(sig().ret()))
        return false;

    initControl(controlItem());

    uint32_t overhead = 0;

    for (;;) {

        Nothing unused_a, unused_b;

#ifdef DEBUG
        performRegisterLeakCheck();
#endif

#define emitBinary(doEmit, type) \
        iter_.readBinary(type, &unused_a, &unused_b) && (deadCode_ || (doEmit(), true))

#define emitUnary(doEmit, type) \
        iter_.readUnary(type, &unused_a) && (deadCode_ || (doEmit(), true))

#define emitComparison(doEmit, operandType, compareOp) \
        iter_.readComparison(operandType, &unused_a, &unused_b) && \
            (deadCode_ || (doEmit(compareOp, operandType), true))

#define emitConversion(doEmit, inType, outType) \
        iter_.readConversion(inType, outType, &unused_a) && (deadCode_ || (doEmit(), true))

#define emitConversionOOM(doEmit, inType, outType) \
        iter_.readConversion(inType, outType, &unused_a) && (deadCode_ || doEmit())

#define emitCalloutConversionOOM(doEmit, symbol, inType, outType) \
        iter_.readConversion(inType, outType, &unused_a) && \
            (deadCode_ || doEmit(symbol, inType, outType))

#define emitIntDivCallout(doEmit, symbol, type) \
        iter_.readBinary(type, &unused_a, &unused_b) && (deadCode_ || (doEmit(symbol, type), true))

#define CHECK(E)      if (!(E)) return false
#define NEXT()        continue
#define CHECK_NEXT(E) if (!(E)) return false; continue

        // TODO / EVALUATE (bug 1316845): Not obvious that this attempt at
        // reducing overhead is really paying off relative to making the check
        // every iteration.

        if (overhead == 0) {
            // Check every 50 expressions -- a happy medium between
            // memory usage and checking overhead.
            overhead = 50;

            // Checking every 50 expressions should be safe, as the
            // baseline JIT does very little allocation per expression.
            CHECK(alloc_.ensureBallast());

            // The pushiest opcode is LOOP, which pushes two values
            // per instance.
            CHECK(stk_.reserve(stk_.length() + overhead * 2));
        }

        overhead--;

        OpBytes op;
        CHECK(iter_.readOp(&op));

        // When debugEnabled_, every operator has breakpoint site but Op::End.
        if (debugEnabled_ && op.b0 != (uint16_t)Op::End) {
            // TODO sync only registers that can be clobbered by the exit
            // prologue/epilogue or disable these registers for use in
            // baseline compiler when debugEnabled_ is set.
            sync();

            insertBreakablePoint(CallSiteDesc::Breakpoint);
        }

        switch (op.b0) {
          case uint16_t(Op::End):
            if (!emitEnd())
                return false;

            if (iter_.controlStackEmpty()) {
                if (!deadCode_)
                    doReturn(sig().ret(), PopStack(false));
                return iter_.readFunctionEnd(iter_.end());
            }
            NEXT();

          // Control opcodes
          case uint16_t(Op::Nop):
            CHECK_NEXT(iter_.readNop());
          case uint16_t(Op::Drop):
            CHECK_NEXT(emitDrop());
          case uint16_t(Op::Block):
            CHECK_NEXT(emitBlock());
          case uint16_t(Op::Loop):
            CHECK_NEXT(emitLoop());
          case uint16_t(Op::If):
            CHECK_NEXT(emitIf());
          case uint16_t(Op::Else):
            CHECK_NEXT(emitElse());
          case uint16_t(Op::Br):
            CHECK_NEXT(emitBr());
          case uint16_t(Op::BrIf):
            CHECK_NEXT(emitBrIf());
          case uint16_t(Op::BrTable):
            CHECK_NEXT(emitBrTable());
          case uint16_t(Op::Return):
            CHECK_NEXT(emitReturn());
          case uint16_t(Op::Unreachable):
            CHECK(iter_.readUnreachable());
            if (!deadCode_) {
                trap(Trap::Unreachable);
                deadCode_ = true;
            }
            NEXT();

          // Calls
          case uint16_t(Op::Call):
            CHECK_NEXT(emitCall());
          case uint16_t(Op::CallIndirect):
            CHECK_NEXT(emitCallIndirect());

          // Locals and globals
          case uint16_t(Op::GetLocal):
            CHECK_NEXT(emitGetLocal());
          case uint16_t(Op::SetLocal):
            CHECK_NEXT(emitSetLocal());
          case uint16_t(Op::TeeLocal):
            CHECK_NEXT(emitTeeLocal());
          case uint16_t(Op::GetGlobal):
            CHECK_NEXT(emitGetGlobal());
          case uint16_t(Op::SetGlobal):
            CHECK_NEXT(emitSetGlobal());

          // Select
          case uint16_t(Op::Select):
            CHECK_NEXT(emitSelect());

          // I32
          case uint16_t(Op::I32Const): {
            int32_t i32;
            CHECK(iter_.readI32Const(&i32));
            if (!deadCode_)
                pushI32(i32);
            NEXT();
          }
          case uint16_t(Op::I32Add):
            CHECK_NEXT(emitBinary(emitAddI32, ValType::I32));
          case uint16_t(Op::I32Sub):
            CHECK_NEXT(emitBinary(emitSubtractI32, ValType::I32));
          case uint16_t(Op::I32Mul):
            CHECK_NEXT(emitBinary(emitMultiplyI32, ValType::I32));
          case uint16_t(Op::I32DivS):
            CHECK_NEXT(emitBinary(emitQuotientI32, ValType::I32));
          case uint16_t(Op::I32DivU):
            CHECK_NEXT(emitBinary(emitQuotientU32, ValType::I32));
          case uint16_t(Op::I32RemS):
            CHECK_NEXT(emitBinary(emitRemainderI32, ValType::I32));
          case uint16_t(Op::I32RemU):
            CHECK_NEXT(emitBinary(emitRemainderU32, ValType::I32));
          case uint16_t(Op::I32Eqz):
            CHECK_NEXT(emitConversion(emitEqzI32, ValType::I32, ValType::I32));
          case uint16_t(Op::I32TruncSF32):
            CHECK_NEXT(emitConversionOOM(emitTruncateF32ToI32<0>, ValType::F32, ValType::I32));
          case uint16_t(Op::I32TruncUF32):
            CHECK_NEXT(emitConversionOOM(emitTruncateF32ToI32<TRUNC_UNSIGNED>, ValType::F32, ValType::I32));
          case uint16_t(Op::I32TruncSF64):
            CHECK_NEXT(emitConversionOOM(emitTruncateF64ToI32<0>, ValType::F64, ValType::I32));
          case uint16_t(Op::I32TruncUF64):
            CHECK_NEXT(emitConversionOOM(emitTruncateF64ToI32<TRUNC_UNSIGNED>, ValType::F64, ValType::I32));
          case uint16_t(Op::I32WrapI64):
            CHECK_NEXT(emitConversion(emitWrapI64ToI32, ValType::I64, ValType::I32));
          case uint16_t(Op::I32ReinterpretF32):
            CHECK_NEXT(emitConversion(emitReinterpretF32AsI32, ValType::F32, ValType::I32));
          case uint16_t(Op::I32Clz):
            CHECK_NEXT(emitUnary(emitClzI32, ValType::I32));
          case uint16_t(Op::I32Ctz):
            CHECK_NEXT(emitUnary(emitCtzI32, ValType::I32));
          case uint16_t(Op::I32Popcnt):
            CHECK_NEXT(emitUnary(emitPopcntI32, ValType::I32));
          case uint16_t(Op::I32Or):
            CHECK_NEXT(emitBinary(emitOrI32, ValType::I32));
          case uint16_t(Op::I32And):
            CHECK_NEXT(emitBinary(emitAndI32, ValType::I32));
          case uint16_t(Op::I32Xor):
            CHECK_NEXT(emitBinary(emitXorI32, ValType::I32));
          case uint16_t(Op::I32Shl):
            CHECK_NEXT(emitBinary(emitShlI32, ValType::I32));
          case uint16_t(Op::I32ShrS):
            CHECK_NEXT(emitBinary(emitShrI32, ValType::I32));
          case uint16_t(Op::I32ShrU):
            CHECK_NEXT(emitBinary(emitShrU32, ValType::I32));
          case uint16_t(Op::I32Load8S):
            CHECK_NEXT(emitLoad(ValType::I32, Scalar::Int8));
          case uint16_t(Op::I32Load8U):
            CHECK_NEXT(emitLoad(ValType::I32, Scalar::Uint8));
          case uint16_t(Op::I32Load16S):
            CHECK_NEXT(emitLoad(ValType::I32, Scalar::Int16));
          case uint16_t(Op::I32Load16U):
            CHECK_NEXT(emitLoad(ValType::I32, Scalar::Uint16));
          case uint16_t(Op::I32Load):
            CHECK_NEXT(emitLoad(ValType::I32, Scalar::Int32));
          case uint16_t(Op::I32Store8):
            CHECK_NEXT(emitStore(ValType::I32, Scalar::Int8));
          case uint16_t(Op::I32Store16):
            CHECK_NEXT(emitStore(ValType::I32, Scalar::Int16));
          case uint16_t(Op::I32Store):
            CHECK_NEXT(emitStore(ValType::I32, Scalar::Int32));
          case uint16_t(Op::I32Rotr):
            CHECK_NEXT(emitBinary(emitRotrI32, ValType::I32));
          case uint16_t(Op::I32Rotl):
            CHECK_NEXT(emitBinary(emitRotlI32, ValType::I32));

          // I64
          case uint16_t(Op::I64Const): {
            int64_t i64;
            CHECK(iter_.readI64Const(&i64));
            if (!deadCode_)
                pushI64(i64);
            NEXT();
          }
          case uint16_t(Op::I64Add):
            CHECK_NEXT(emitBinary(emitAddI64, ValType::I64));
          case uint16_t(Op::I64Sub):
            CHECK_NEXT(emitBinary(emitSubtractI64, ValType::I64));
          case uint16_t(Op::I64Mul):
            CHECK_NEXT(emitBinary(emitMultiplyI64, ValType::I64));
          case uint16_t(Op::I64DivS):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
            CHECK_NEXT(emitIntDivCallout(emitDivOrModI64BuiltinCall, SymbolicAddress::DivI64,
                                         ValType::I64));
#else
            CHECK_NEXT(emitBinary(emitQuotientI64, ValType::I64));
#endif
          case uint16_t(Op::I64DivU):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
            CHECK_NEXT(emitIntDivCallout(emitDivOrModI64BuiltinCall, SymbolicAddress::UDivI64,
                                         ValType::I64));
#else
            CHECK_NEXT(emitBinary(emitQuotientU64, ValType::I64));
#endif
          case uint16_t(Op::I64RemS):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
            CHECK_NEXT(emitIntDivCallout(emitDivOrModI64BuiltinCall, SymbolicAddress::ModI64,
                                         ValType::I64));
#else
            CHECK_NEXT(emitBinary(emitRemainderI64, ValType::I64));
#endif
          case uint16_t(Op::I64RemU):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
            CHECK_NEXT(emitIntDivCallout(emitDivOrModI64BuiltinCall, SymbolicAddress::UModI64,
                                         ValType::I64));
#else
            CHECK_NEXT(emitBinary(emitRemainderU64, ValType::I64));
#endif
          case uint16_t(Op::I64TruncSF32):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(emitCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                                SymbolicAddress::TruncateDoubleToInt64,
                                                ValType::F32, ValType::I64));
#else
            CHECK_NEXT(emitConversionOOM(emitTruncateF32ToI64<0>, ValType::F32, ValType::I64));
#endif
          case uint16_t(Op::I64TruncUF32):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(emitCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                                SymbolicAddress::TruncateDoubleToUint64,
                                                ValType::F32, ValType::I64));
#else
            CHECK_NEXT(emitConversionOOM(emitTruncateF32ToI64<TRUNC_UNSIGNED>, ValType::F32, ValType::I64));
#endif
          case uint16_t(Op::I64TruncSF64):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(emitCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                                SymbolicAddress::TruncateDoubleToInt64,
                                                ValType::F64, ValType::I64));
#else
            CHECK_NEXT(emitConversionOOM(emitTruncateF64ToI64<0>, ValType::F64, ValType::I64));
#endif
          case uint16_t(Op::I64TruncUF64):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(emitCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                                SymbolicAddress::TruncateDoubleToUint64,
                                                ValType::F64, ValType::I64));
#else
            CHECK_NEXT(emitConversionOOM(emitTruncateF64ToI64<TRUNC_UNSIGNED>, ValType::F64, ValType::I64));
#endif
          case uint16_t(Op::I64ExtendSI32):
            CHECK_NEXT(emitConversion(emitExtendI32ToI64, ValType::I32, ValType::I64));
          case uint16_t(Op::I64ExtendUI32):
            CHECK_NEXT(emitConversion(emitExtendU32ToI64, ValType::I32, ValType::I64));
          case uint16_t(Op::I64ReinterpretF64):
            CHECK_NEXT(emitConversion(emitReinterpretF64AsI64, ValType::F64, ValType::I64));
          case uint16_t(Op::I64Or):
            CHECK_NEXT(emitBinary(emitOrI64, ValType::I64));
          case uint16_t(Op::I64And):
            CHECK_NEXT(emitBinary(emitAndI64, ValType::I64));
          case uint16_t(Op::I64Xor):
            CHECK_NEXT(emitBinary(emitXorI64, ValType::I64));
          case uint16_t(Op::I64Shl):
            CHECK_NEXT(emitBinary(emitShlI64, ValType::I64));
          case uint16_t(Op::I64ShrS):
            CHECK_NEXT(emitBinary(emitShrI64, ValType::I64));
          case uint16_t(Op::I64ShrU):
            CHECK_NEXT(emitBinary(emitShrU64, ValType::I64));
          case uint16_t(Op::I64Rotr):
            CHECK_NEXT(emitBinary(emitRotrI64, ValType::I64));
          case uint16_t(Op::I64Rotl):
            CHECK_NEXT(emitBinary(emitRotlI64, ValType::I64));
          case uint16_t(Op::I64Clz):
            CHECK_NEXT(emitUnary(emitClzI64, ValType::I64));
          case uint16_t(Op::I64Ctz):
            CHECK_NEXT(emitUnary(emitCtzI64, ValType::I64));
          case uint16_t(Op::I64Popcnt):
            CHECK_NEXT(emitUnary(emitPopcntI64, ValType::I64));
          case uint16_t(Op::I64Eqz):
            CHECK_NEXT(emitConversion(emitEqzI64, ValType::I64, ValType::I32));
          case uint16_t(Op::I64Load8S):
            CHECK_NEXT(emitLoad(ValType::I64, Scalar::Int8));
          case uint16_t(Op::I64Load16S):
            CHECK_NEXT(emitLoad(ValType::I64, Scalar::Int16));
          case uint16_t(Op::I64Load32S):
            CHECK_NEXT(emitLoad(ValType::I64, Scalar::Int32));
          case uint16_t(Op::I64Load8U):
            CHECK_NEXT(emitLoad(ValType::I64, Scalar::Uint8));
          case uint16_t(Op::I64Load16U):
            CHECK_NEXT(emitLoad(ValType::I64, Scalar::Uint16));
          case uint16_t(Op::I64Load32U):
            CHECK_NEXT(emitLoad(ValType::I64, Scalar::Uint32));
          case uint16_t(Op::I64Load):
            CHECK_NEXT(emitLoad(ValType::I64, Scalar::Int64));
          case uint16_t(Op::I64Store8):
            CHECK_NEXT(emitStore(ValType::I64, Scalar::Int8));
          case uint16_t(Op::I64Store16):
            CHECK_NEXT(emitStore(ValType::I64, Scalar::Int16));
          case uint16_t(Op::I64Store32):
            CHECK_NEXT(emitStore(ValType::I64, Scalar::Int32));
          case uint16_t(Op::I64Store):
            CHECK_NEXT(emitStore(ValType::I64, Scalar::Int64));

          // F32
          case uint16_t(Op::F32Const): {
            float f32;
            CHECK(iter_.readF32Const(&f32));
            if (!deadCode_)
                pushF32(f32);
            NEXT();
          }
          case uint16_t(Op::F32Add):
            CHECK_NEXT(emitBinary(emitAddF32, ValType::F32));
          case uint16_t(Op::F32Sub):
            CHECK_NEXT(emitBinary(emitSubtractF32, ValType::F32));
          case uint16_t(Op::F32Mul):
            CHECK_NEXT(emitBinary(emitMultiplyF32, ValType::F32));
          case uint16_t(Op::F32Div):
            CHECK_NEXT(emitBinary(emitDivideF32, ValType::F32));
          case uint16_t(Op::F32Min):
            CHECK_NEXT(emitBinary(emitMinF32, ValType::F32));
          case uint16_t(Op::F32Max):
            CHECK_NEXT(emitBinary(emitMaxF32, ValType::F32));
          case uint16_t(Op::F32Neg):
            CHECK_NEXT(emitUnary(emitNegateF32, ValType::F32));
          case uint16_t(Op::F32Abs):
            CHECK_NEXT(emitUnary(emitAbsF32, ValType::F32));
          case uint16_t(Op::F32Sqrt):
            CHECK_NEXT(emitUnary(emitSqrtF32, ValType::F32));
          case uint16_t(Op::F32Ceil):
            CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::CeilF, ValType::F32));
          case uint16_t(Op::F32Floor):
            CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::FloorF, ValType::F32));
          case uint16_t(Op::F32DemoteF64):
            CHECK_NEXT(emitConversion(emitConvertF64ToF32, ValType::F64, ValType::F32));
          case uint16_t(Op::F32ConvertSI32):
            CHECK_NEXT(emitConversion(emitConvertI32ToF32, ValType::I32, ValType::F32));
          case uint16_t(Op::F32ConvertUI32):
            CHECK_NEXT(emitConversion(emitConvertU32ToF32, ValType::I32, ValType::F32));
          case uint16_t(Op::F32ConvertSI64):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
            CHECK_NEXT(emitCalloutConversionOOM(emitConvertInt64ToFloatingCallout,
                                                SymbolicAddress::Int64ToFloat32,
                                                ValType::I64, ValType::F32));
#else
            CHECK_NEXT(emitConversion(emitConvertI64ToF32, ValType::I64, ValType::F32));
#endif
          case uint16_t(Op::F32ConvertUI64):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
            CHECK_NEXT(emitCalloutConversionOOM(emitConvertInt64ToFloatingCallout,
                                                SymbolicAddress::Uint64ToFloat32,
                                                ValType::I64, ValType::F32));
#else
            CHECK_NEXT(emitConversion(emitConvertU64ToF32, ValType::I64, ValType::F32));
#endif
          case uint16_t(Op::F32ReinterpretI32):
            CHECK_NEXT(emitConversion(emitReinterpretI32AsF32, ValType::I32, ValType::F32));
          case uint16_t(Op::F32Load):
            CHECK_NEXT(emitLoad(ValType::F32, Scalar::Float32));
          case uint16_t(Op::F32Store):
            CHECK_NEXT(emitStore(ValType::F32, Scalar::Float32));
          case uint16_t(Op::F32CopySign):
            CHECK_NEXT(emitBinary(emitCopysignF32, ValType::F32));
          case uint16_t(Op::F32Nearest):
            CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::NearbyIntF, ValType::F32));
          case uint16_t(Op::F32Trunc):
            CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::TruncF, ValType::F32));

          // F64
          case uint16_t(Op::F64Const): {
            double f64;
            CHECK(iter_.readF64Const(&f64));
            if (!deadCode_)
                pushF64(f64);
            NEXT();
          }
          case uint16_t(Op::F64Add):
            CHECK_NEXT(emitBinary(emitAddF64, ValType::F64));
          case uint16_t(Op::F64Sub):
            CHECK_NEXT(emitBinary(emitSubtractF64, ValType::F64));
          case uint16_t(Op::F64Mul):
            CHECK_NEXT(emitBinary(emitMultiplyF64, ValType::F64));
          case uint16_t(Op::F64Div):
            CHECK_NEXT(emitBinary(emitDivideF64, ValType::F64));
          case uint16_t(Op::F64Min):
            CHECK_NEXT(emitBinary(emitMinF64, ValType::F64));
          case uint16_t(Op::F64Max):
            CHECK_NEXT(emitBinary(emitMaxF64, ValType::F64));
          case uint16_t(Op::F64Neg):
            CHECK_NEXT(emitUnary(emitNegateF64, ValType::F64));
          case uint16_t(Op::F64Abs):
            CHECK_NEXT(emitUnary(emitAbsF64, ValType::F64));
          case uint16_t(Op::F64Sqrt):
            CHECK_NEXT(emitUnary(emitSqrtF64, ValType::F64));
          case uint16_t(Op::F64Ceil):
            CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::CeilD, ValType::F64));
          case uint16_t(Op::F64Floor):
            CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::FloorD, ValType::F64));
          case uint16_t(Op::F64PromoteF32):
            CHECK_NEXT(emitConversion(emitConvertF32ToF64, ValType::F32, ValType::F64));
          case uint16_t(Op::F64ConvertSI32):
            CHECK_NEXT(emitConversion(emitConvertI32ToF64, ValType::I32, ValType::F64));
          case uint16_t(Op::F64ConvertUI32):
            CHECK_NEXT(emitConversion(emitConvertU32ToF64, ValType::I32, ValType::F64));
          case uint16_t(Op::F64ConvertSI64):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
            CHECK_NEXT(emitCalloutConversionOOM(emitConvertInt64ToFloatingCallout,
                                                SymbolicAddress::Int64ToDouble,
                                                ValType::I64, ValType::F64));
#else
            CHECK_NEXT(emitConversion(emitConvertI64ToF64, ValType::I64, ValType::F64));
#endif
          case uint16_t(Op::F64ConvertUI64):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
            CHECK_NEXT(emitCalloutConversionOOM(emitConvertInt64ToFloatingCallout,
                                                SymbolicAddress::Uint64ToDouble,
                                                ValType::I64, ValType::F64));
#else
            CHECK_NEXT(emitConversion(emitConvertU64ToF64, ValType::I64, ValType::F64));
#endif
          case uint16_t(Op::F64Load):
            CHECK_NEXT(emitLoad(ValType::F64, Scalar::Float64));
          case uint16_t(Op::F64Store):
            CHECK_NEXT(emitStore(ValType::F64, Scalar::Float64));
          case uint16_t(Op::F64ReinterpretI64):
            CHECK_NEXT(emitConversion(emitReinterpretI64AsF64, ValType::I64, ValType::F64));
          case uint16_t(Op::F64CopySign):
            CHECK_NEXT(emitBinary(emitCopysignF64, ValType::F64));
          case uint16_t(Op::F64Nearest):
            CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::NearbyIntD, ValType::F64));
          case uint16_t(Op::F64Trunc):
            CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::TruncD, ValType::F64));

          // Comparisons
          case uint16_t(Op::I32Eq):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::Equal));
          case uint16_t(Op::I32Ne):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::NotEqual));
          case uint16_t(Op::I32LtS):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::LessThan));
          case uint16_t(Op::I32LeS):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::LessThanOrEqual));
          case uint16_t(Op::I32GtS):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::GreaterThan));
          case uint16_t(Op::I32GeS):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::GreaterThanOrEqual));
          case uint16_t(Op::I32LtU):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::Below));
          case uint16_t(Op::I32LeU):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::BelowOrEqual));
          case uint16_t(Op::I32GtU):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::Above));
          case uint16_t(Op::I32GeU):
            CHECK_NEXT(emitComparison(emitCompareI32, ValType::I32, Assembler::AboveOrEqual));
          case uint16_t(Op::I64Eq):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::Equal));
          case uint16_t(Op::I64Ne):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::NotEqual));
          case uint16_t(Op::I64LtS):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::LessThan));
          case uint16_t(Op::I64LeS):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::LessThanOrEqual));
          case uint16_t(Op::I64GtS):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::GreaterThan));
          case uint16_t(Op::I64GeS):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::GreaterThanOrEqual));
          case uint16_t(Op::I64LtU):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::Below));
          case uint16_t(Op::I64LeU):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::BelowOrEqual));
          case uint16_t(Op::I64GtU):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::Above));
          case uint16_t(Op::I64GeU):
            CHECK_NEXT(emitComparison(emitCompareI64, ValType::I64, Assembler::AboveOrEqual));
          case uint16_t(Op::F32Eq):
            CHECK_NEXT(emitComparison(emitCompareF32, ValType::F32, Assembler::DoubleEqual));
          case uint16_t(Op::F32Ne):
            CHECK_NEXT(emitComparison(emitCompareF32, ValType::F32, Assembler::DoubleNotEqualOrUnordered));
          case uint16_t(Op::F32Lt):
            CHECK_NEXT(emitComparison(emitCompareF32, ValType::F32, Assembler::DoubleLessThan));
          case uint16_t(Op::F32Le):
            CHECK_NEXT(emitComparison(emitCompareF32, ValType::F32, Assembler::DoubleLessThanOrEqual));
          case uint16_t(Op::F32Gt):
            CHECK_NEXT(emitComparison(emitCompareF32, ValType::F32, Assembler::DoubleGreaterThan));
          case uint16_t(Op::F32Ge):
            CHECK_NEXT(emitComparison(emitCompareF32, ValType::F32, Assembler::DoubleGreaterThanOrEqual));
          case uint16_t(Op::F64Eq):
            CHECK_NEXT(emitComparison(emitCompareF64, ValType::F64, Assembler::DoubleEqual));
          case uint16_t(Op::F64Ne):
            CHECK_NEXT(emitComparison(emitCompareF64, ValType::F64, Assembler::DoubleNotEqualOrUnordered));
          case uint16_t(Op::F64Lt):
            CHECK_NEXT(emitComparison(emitCompareF64, ValType::F64, Assembler::DoubleLessThan));
          case uint16_t(Op::F64Le):
            CHECK_NEXT(emitComparison(emitCompareF64, ValType::F64, Assembler::DoubleLessThanOrEqual));
          case uint16_t(Op::F64Gt):
            CHECK_NEXT(emitComparison(emitCompareF64, ValType::F64, Assembler::DoubleGreaterThan));
          case uint16_t(Op::F64Ge):
            CHECK_NEXT(emitComparison(emitCompareF64, ValType::F64, Assembler::DoubleGreaterThanOrEqual));

          // Sign extensions
#ifdef ENABLE_WASM_SIGNEXTEND_OPS
          case uint16_t(Op::I32Extend8S):
            CHECK_NEXT(emitConversion(emitExtendI32_8, ValType::I32, ValType::I32));
          case uint16_t(Op::I32Extend16S):
            CHECK_NEXT(emitConversion(emitExtendI32_16, ValType::I32, ValType::I32));
          case uint16_t(Op::I64Extend8S):
            CHECK_NEXT(emitConversion(emitExtendI64_8, ValType::I64, ValType::I64));
          case uint16_t(Op::I64Extend16S):
            CHECK_NEXT(emitConversion(emitExtendI64_16, ValType::I64, ValType::I64));
          case uint16_t(Op::I64Extend32S):
            CHECK_NEXT(emitConversion(emitExtendI64_32, ValType::I64, ValType::I64));
#endif

          // Memory Related
          case uint16_t(Op::GrowMemory):
            CHECK_NEXT(emitGrowMemory());
          case uint16_t(Op::CurrentMemory):
            CHECK_NEXT(emitCurrentMemory());

          // Numeric operations
          case uint16_t(Op::NumericPrefix): {
#ifdef ENABLE_WASM_SATURATING_TRUNC_OPS
            switch (op.b1) {
              case uint16_t(NumericOp::I32TruncSSatF32):
                CHECK_NEXT(emitConversionOOM(emitTruncateF32ToI32<TRUNC_SATURATING>,
                                             ValType::F32, ValType::I32));
              case uint16_t(NumericOp::I32TruncUSatF32):
                CHECK_NEXT(emitConversionOOM(emitTruncateF32ToI32<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                                             ValType::F32, ValType::I32));
              case uint16_t(NumericOp::I32TruncSSatF64):
                CHECK_NEXT(emitConversionOOM(emitTruncateF64ToI32<TRUNC_SATURATING>,
                                             ValType::F64, ValType::I32));
              case uint16_t(NumericOp::I32TruncUSatF64):
                CHECK_NEXT(emitConversionOOM(emitTruncateF64ToI32<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                                             ValType::F64, ValType::I32));
              case uint16_t(NumericOp::I64TruncSSatF32):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
                CHECK_NEXT(emitCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                                    SymbolicAddress::SaturatingTruncateDoubleToInt64,
                                                    ValType::F32, ValType::I64));
#else
                CHECK_NEXT(emitConversionOOM(emitTruncateF32ToI64<TRUNC_SATURATING>,
                                             ValType::F32, ValType::I64));
#endif
              case uint16_t(NumericOp::I64TruncUSatF32):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
                CHECK_NEXT(emitCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                                    SymbolicAddress::SaturatingTruncateDoubleToUint64,
                                                    ValType::F32, ValType::I64));
#else
                CHECK_NEXT(emitConversionOOM(emitTruncateF32ToI64<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                                             ValType::F32, ValType::I64));
#endif
              case uint16_t(NumericOp::I64TruncSSatF64):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
                CHECK_NEXT(emitCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                                    SymbolicAddress::SaturatingTruncateDoubleToInt64,
                                                    ValType::F64, ValType::I64));
#else
                CHECK_NEXT(emitConversionOOM(emitTruncateF64ToI64<TRUNC_SATURATING>,
                                             ValType::F64, ValType::I64));
#endif
              case uint16_t(NumericOp::I64TruncUSatF64):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
                CHECK_NEXT(emitCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                                    SymbolicAddress::SaturatingTruncateDoubleToUint64,
                                                    ValType::F64, ValType::I64));
#else
                CHECK_NEXT(emitConversionOOM(emitTruncateF64ToI64<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                                             ValType::F64, ValType::I64));
#endif
              default:
                return iter_.unrecognizedOpcode(&op);
            }
            break;
#else
            return iter_.unrecognizedOpcode(&op);
#endif
          }

          // Thread operations
          case uint16_t(Op::ThreadPrefix): {
#ifdef ENABLE_WASM_THREAD_OPS
            switch (op.b1) {
              case uint16_t(ThreadOp::Wake):
                CHECK_NEXT(emitWake());

              case uint16_t(ThreadOp::I32Wait):
                CHECK_NEXT(emitWait(ValType::I32, 4));
              case uint16_t(ThreadOp::I64Wait):
                CHECK_NEXT(emitWait(ValType::I64, 8));

              case uint16_t(ThreadOp::I32AtomicLoad):
                CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Int32));
              case uint16_t(ThreadOp::I64AtomicLoad):
                CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Int64));
              case uint16_t(ThreadOp::I32AtomicLoad8U):
                CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Uint8));
              case uint16_t(ThreadOp::I32AtomicLoad16U):
                CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Uint16));
              case uint16_t(ThreadOp::I64AtomicLoad8U):
                CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint8));
              case uint16_t(ThreadOp::I64AtomicLoad16U):
                CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint16));
              case uint16_t(ThreadOp::I64AtomicLoad32U):
                CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint32));

              case uint16_t(ThreadOp::I32AtomicStore):
                CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Int32));
              case uint16_t(ThreadOp::I64AtomicStore):
                CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Int64));
              case uint16_t(ThreadOp::I32AtomicStore8U):
                CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Uint8));
              case uint16_t(ThreadOp::I32AtomicStore16U):
                CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Uint16));
              case uint16_t(ThreadOp::I64AtomicStore8U):
                CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint8));
              case uint16_t(ThreadOp::I64AtomicStore16U):
                CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint16));
              case uint16_t(ThreadOp::I64AtomicStore32U):
                CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint32));

              case uint16_t(ThreadOp::I32AtomicAdd):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchAddOp));
              case uint16_t(ThreadOp::I64AtomicAdd):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchAddOp));
              case uint16_t(ThreadOp::I32AtomicAdd8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchAddOp));
              case uint16_t(ThreadOp::I32AtomicAdd16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchAddOp));
              case uint16_t(ThreadOp::I64AtomicAdd8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchAddOp));
              case uint16_t(ThreadOp::I64AtomicAdd16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchAddOp));
              case uint16_t(ThreadOp::I64AtomicAdd32U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchAddOp));

              case uint16_t(ThreadOp::I32AtomicSub):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchSubOp));
              case uint16_t(ThreadOp::I64AtomicSub):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchSubOp));
              case uint16_t(ThreadOp::I32AtomicSub8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchSubOp));
              case uint16_t(ThreadOp::I32AtomicSub16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchSubOp));
              case uint16_t(ThreadOp::I64AtomicSub8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchSubOp));
              case uint16_t(ThreadOp::I64AtomicSub16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchSubOp));
              case uint16_t(ThreadOp::I64AtomicSub32U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchSubOp));

              case uint16_t(ThreadOp::I32AtomicAnd):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchAndOp));
              case uint16_t(ThreadOp::I64AtomicAnd):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchAndOp));
              case uint16_t(ThreadOp::I32AtomicAnd8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchAndOp));
              case uint16_t(ThreadOp::I32AtomicAnd16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchAndOp));
              case uint16_t(ThreadOp::I64AtomicAnd8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchAndOp));
              case uint16_t(ThreadOp::I64AtomicAnd16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchAndOp));
              case uint16_t(ThreadOp::I64AtomicAnd32U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchAndOp));

              case uint16_t(ThreadOp::I32AtomicOr):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchOrOp));
              case uint16_t(ThreadOp::I64AtomicOr):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchOrOp));
              case uint16_t(ThreadOp::I32AtomicOr8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchOrOp));
              case uint16_t(ThreadOp::I32AtomicOr16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchOrOp));
              case uint16_t(ThreadOp::I64AtomicOr8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchOrOp));
              case uint16_t(ThreadOp::I64AtomicOr16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchOrOp));
              case uint16_t(ThreadOp::I64AtomicOr32U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchOrOp));

              case uint16_t(ThreadOp::I32AtomicXor):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchXorOp));
              case uint16_t(ThreadOp::I64AtomicXor):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchXorOp));
              case uint16_t(ThreadOp::I32AtomicXor8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchXorOp));
              case uint16_t(ThreadOp::I32AtomicXor16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchXorOp));
              case uint16_t(ThreadOp::I64AtomicXor8U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchXorOp));
              case uint16_t(ThreadOp::I64AtomicXor16U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchXorOp));
              case uint16_t(ThreadOp::I64AtomicXor32U):
                CHECK_NEXT(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchXorOp));

              case uint16_t(ThreadOp::I32AtomicXchg):
                CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Int32));
              case uint16_t(ThreadOp::I64AtomicXchg):
                CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Int64));
              case uint16_t(ThreadOp::I32AtomicXchg8U):
                CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Uint8));
              case uint16_t(ThreadOp::I32AtomicXchg16U):
                CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Uint16));
              case uint16_t(ThreadOp::I64AtomicXchg8U):
                CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint8));
              case uint16_t(ThreadOp::I64AtomicXchg16U):
                CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint16));
              case uint16_t(ThreadOp::I64AtomicXchg32U):
                CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint32));

              case uint16_t(ThreadOp::I32AtomicCmpXchg):
                CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Int32));
              case uint16_t(ThreadOp::I64AtomicCmpXchg):
                CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Int64));
              case uint16_t(ThreadOp::I32AtomicCmpXchg8U):
                CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Uint8));
              case uint16_t(ThreadOp::I32AtomicCmpXchg16U):
                CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Uint16));
              case uint16_t(ThreadOp::I64AtomicCmpXchg8U):
                CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint8));
              case uint16_t(ThreadOp::I64AtomicCmpXchg16U):
                CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint16));
              case uint16_t(ThreadOp::I64AtomicCmpXchg32U):
                CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint32));

              default:
                return iter_.unrecognizedOpcode(&op);
            }
#else
            return iter_.unrecognizedOpcode(&op);
#endif  // ENABLE_WASM_THREAD_OPS
            break;
          }

          // asm.js operations
          case uint16_t(Op::MozPrefix):
            return iter_.unrecognizedOpcode(&op);

          default:
            return iter_.unrecognizedOpcode(&op);
        }

#undef CHECK
#undef NEXT
#undef CHECK_NEXT
#undef emitBinary
#undef emitUnary
#undef emitComparison
#undef emitConversion
#undef emitConversionOOM
#undef emitCalloutConversionOOM

        MOZ_CRASH("unreachable");
    }

    MOZ_CRASH("unreachable");
}

bool
BaseCompiler::emitFunction()
{
    beginFunction();

    if (!emitBody())
        return false;

    if (!endFunction())
        return false;

    return true;
}

BaseCompiler::BaseCompiler(const ModuleEnvironment& env,
                           Decoder& decoder,
                           const FuncCompileInput& func,
                           const ValTypeVector& locals,
                           bool debugEnabled,
                           TempAllocator* alloc,
                           MacroAssembler* masm,
                           CompileMode mode)
    : env_(env),
      iter_(env, decoder),
      func_(func),
      lastReadCallSite_(0),
      alloc_(*alloc),
      locals_(locals),
      deadCode_(false),
      debugEnabled_(debugEnabled),
      bceSafe_(0),
      mode_(mode),
      latentOp_(LatentOp::None),
      latentType_(ValType::I32),
      latentIntCmp_(Assembler::Equal),
      latentDoubleCmp_(Assembler::DoubleEqual),
      masm(*masm),
      ra(*this),
      fr(*masm),
      joinRegI32(RegI32(ReturnReg)),
      joinRegI64(RegI64(ReturnReg64)),
      joinRegF32(RegF32(ReturnFloat32Reg)),
      joinRegF64(RegF64(ReturnDoubleReg))
{
}

bool
BaseCompiler::init()
{
    if (!SigD_.append(ValType::F64))
        return false;
    if (!SigF_.append(ValType::F32))
        return false;
    if (!SigP_.append(MIRType::Pointer))
        return false;
    if (!SigPI_.append(MIRType::Pointer) || !SigPI_.append(MIRType::Int32))
        return false;
    if (!SigPII_.append(MIRType::Pointer) || !SigPII_.append(MIRType::Int32) ||
        !SigPII_.append(MIRType::Int32))
    {
        return false;
    }
    if (!SigPIIL_.append(MIRType::Pointer) || !SigPIIL_.append(MIRType::Int32) ||
        !SigPIIL_.append(MIRType::Int32) || !SigPIIL_.append(MIRType::Int64))
    {
        return false;
    }
    if (!SigPILL_.append(MIRType::Pointer) || !SigPILL_.append(MIRType::Int32) ||
        !SigPILL_.append(MIRType::Int64) || !SigPILL_.append(MIRType::Int64))
    {
        return false;
    }

    if (!fr.setupLocals(locals_, sig().args(), debugEnabled_, &localInfo_))
        return false;

    addInterruptCheck();

    return true;
}

FuncOffsets
BaseCompiler::finish()
{
    MOZ_ASSERT(done(), "all bytes must be consumed");
    MOZ_ASSERT(func_.callSiteLineNums.length() == lastReadCallSite_);

    masm.flushBuffer();

    return offsets_;
}

} // wasm
} // js

bool
js::wasm::BaselineCanCompile()
{
    // On all platforms we require signals for Wasm.
    // If we made it this far we must have signals.
    MOZ_RELEASE_ASSERT(wasm::HaveSignalHandlers());

#if defined(JS_CODEGEN_ARM)
    // Simplifying assumption: require SDIV and UDIV.
    //
    // I have no good data on ARM populations allowing me to say that
    // X% of devices in the market implement SDIV and UDIV.  However,
    // they are definitely implemented on the Cortex-A7 and Cortex-A15
    // and on all ARMv8 systems.
    if (!HasIDIV())
        return false;
#endif

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_ARM) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    return true;
#else
    return false;
#endif
}

bool
js::wasm::BaselineCompileFunctions(const ModuleEnvironment& env, LifoAlloc& lifo,
                                   const FuncCompileInputVector& inputs, CompiledCode* code,
                                   UniqueChars* error)
{
    MOZ_ASSERT(env.tier == Tier::Baseline);
    MOZ_ASSERT(env.kind == ModuleKind::Wasm);

    // The MacroAssembler will sometimes access the jitContext.

    TempAllocator alloc(&lifo);
    JitContext jitContext(&alloc);
    MOZ_ASSERT(IsCompilingWasm());
    MacroAssembler masm(MacroAssembler::WasmToken(), alloc);

    // Swap in already-allocated empty vectors to avoid malloc/free.
    MOZ_ASSERT(code->empty());
    if (!code->swap(masm))
        return false;

    for (const FuncCompileInput& func : inputs) {
        Decoder d(func.begin, func.end, func.lineOrBytecode, error);

        // Build the local types vector.

        ValTypeVector locals;
        if (!locals.appendAll(env.funcSigs[func.index]->args()))
            return false;
        if (!DecodeLocalEntries(d, env.kind, &locals))
            return false;

        // One-pass baseline compilation.

        BaseCompiler f(env, d, func, locals, env.debugEnabled(), &alloc, &masm, env.mode);
        if (!f.init())
            return false;

        if (!f.emitFunction())
            return false;

        if (!code->codeRanges.emplaceBack(func.index, func.lineOrBytecode, f.finish()))
            return false;
    }

    masm.finish();
    if (masm.oom())
        return false;

    return code->swap(masm);
}

#undef RABALDR_INT_DIV_I64_CALLOUT
#undef RABALDR_I64_TO_FLOAT_CALLOUT
#undef RABALDR_FLOAT_TO_I64_CALLOUT
