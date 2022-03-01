/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

/*
 * [SMDOC] WebAssembly baseline compiler (RabaldrMonkey)
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
 *   Old school:
 *   - if there is a single source + destination register, it is called 'r'
 *   - if there is one source and a different destination, they are called 'rs'
 *     and 'rd'
 *   - if there is one source + destination register and another source register
 *     they are called 'r' and 'rs'
 *   - if there are two source registers and a destination register they are
 *     called 'rs0', 'rs1', and 'rd'.
 *
 *   The new thing:
 *   - what is called 'r' in the old-school naming scheme is increasingly called
 *     'rsd' in source+dest cases.
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
#include "mozilla/ScopeExit.h"

#include <algorithm>
#include <utility>

#include "jit/AtomicOp.h"
#include "jit/IonTypes.h"
#include "jit/JitAllocPolicy.h"
#include "jit/Label.h"
#include "jit/MIR.h"
#include "jit/RegisterAllocator.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#if defined(JS_CODEGEN_ARM)
#  include "jit/arm/Assembler-arm.h"
#endif
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
#  include "jit/x86-shared/Architecture-x86-shared.h"
#  include "jit/x86-shared/Assembler-x86-shared.h"
#endif
#if defined(JS_CODEGEN_MIPS32)
#  include "jit/mips-shared/Assembler-mips-shared.h"
#  include "jit/mips32/Assembler-mips32.h"
#endif
#if defined(JS_CODEGEN_MIPS64)
#  include "jit/mips-shared/Assembler-mips-shared.h"
#  include "jit/mips64/Assembler-mips64.h"
#endif
#include "js/ScalarType.h"  // js::Scalar::Type
#include "util/Memory.h"
#include "wasm/TypedObject.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmValidate.h"

#include "jit/MacroAssembler-inl.h"

using mozilla::DebugOnly;
using mozilla::FloorLog2;
using mozilla::IsPowerOfTwo;
using mozilla::Maybe;

namespace js {
namespace wasm {

using namespace js::jit;

using HandleNaNSpecially = bool;
using InvertBranch = bool;
using IsKnownNotZero = bool;
using IsUnsigned = bool;
using NeedsBoundsCheck = bool;
using WantResult = bool;
using ZeroOnOverflow = bool;

class BaseStackFrame;

// Two flags, useABI and interModule, control how calls are made.
//
// UseABI::Wasm implies that the Tls/Heap/Global registers are nonvolatile,
// except when InterModule::True is also set, when they are volatile.
//
// UseABI::Builtin implies that the Tls/Heap/Global registers are volatile.
// In this case, we require InterModule::False.  The calling convention
// is otherwise like UseABI::Wasm.
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

enum class UseABI { Wasm, Builtin, System };
enum class InterModule { False = false, True = true };
enum class RhsDestOp { True = true };

#if defined(JS_CODEGEN_NONE)
#  define RABALDR_SCRATCH_I32
#  define RABALDR_SCRATCH_F32
#  define RABALDR_SCRATCH_F64

static constexpr Register RabaldrScratchI32 = Register::Invalid();
static constexpr FloatRegister RabaldrScratchF32 = InvalidFloatReg;
static constexpr FloatRegister RabaldrScratchF64 = InvalidFloatReg;
#endif

#ifdef JS_CODEGEN_ARM64
#  define RABALDR_CHUNKY_STACK
#  define RABALDR_SCRATCH_I32
#  define RABALDR_SCRATCH_F32
#  define RABALDR_SCRATCH_F64
#  define RABALDR_SCRATCH_V128
#  define RABALDR_SCRATCH_F32_ALIASES_F64

static constexpr Register RabaldrScratchI32{Registers::x15};

// Note, the float scratch regs cannot be registers that are used for parameter
// passing in any ABI we use.  Argregs tend to be low-numbered; register 30
// should be safe.

static constexpr FloatRegister RabaldrScratchF32{FloatRegisters::s30,
                                                 FloatRegisters::Single};
static constexpr FloatRegister RabaldrScratchF64{FloatRegisters::d30,
                                                 FloatRegisters::Double};
#  ifdef ENABLE_WASM_SIMD
static constexpr FloatRegister RabaldrScratchV128{FloatRegisters::d30,
                                                  FloatRegisters::Simd128};
#  endif

static_assert(RabaldrScratchF32 != ScratchFloat32Reg_, "Too busy");
static_assert(RabaldrScratchF64 != ScratchDoubleReg_, "Too busy");
#  ifdef ENABLE_WASM_SIMD
static_assert(RabaldrScratchV128 != ScratchSimd128Reg, "Too busy");
#  endif
#endif

#ifdef JS_CODEGEN_X86
// The selection of EBX here steps gingerly around: the need for EDX
// to be allocatable for multiply/divide; ECX to be allocatable for
// shift/rotate; EAX (= ReturnReg) to be allocatable as the result
// register; EBX not being one of the WasmTableCall registers; and
// needing a temp register for load/store that has a single-byte
// persona.
//
// The compiler assumes that RabaldrScratchI32 has a single-byte
// persona.  Code for 8-byte atomic operations assumes that
// RabaldrScratchI32 is in fact ebx.

#  define RABALDR_SCRATCH_I32
static constexpr Register RabaldrScratchI32 = ebx;

#  define RABALDR_INT_DIV_I64_CALLOUT
#endif

#ifdef JS_CODEGEN_ARM
// We use our own scratch register, because the macro assembler uses
// the regular scratch register(s) pretty liberally.  We could
// work around that in several cases but the mess does not seem
// worth it yet.  CallTempReg2 seems safe.

#  define RABALDR_SCRATCH_I32
static constexpr Register RabaldrScratchI32 = CallTempReg2;

#  define RABALDR_INT_DIV_I64_CALLOUT
#  define RABALDR_I64_TO_FLOAT_CALLOUT
#  define RABALDR_FLOAT_TO_I64_CALLOUT
#endif

#ifdef JS_CODEGEN_MIPS32
#  define RABALDR_SCRATCH_I32
static constexpr Register RabaldrScratchI32 = CallTempReg2;

#  define RABALDR_INT_DIV_I64_CALLOUT
#  define RABALDR_I64_TO_FLOAT_CALLOUT
#  define RABALDR_FLOAT_TO_I64_CALLOUT
#endif

#ifdef JS_CODEGEN_MIPS64
#  define RABALDR_SCRATCH_I32
static constexpr Register RabaldrScratchI32 = CallTempReg2;
#endif

#ifdef RABALDR_SCRATCH_F32_ALIASES_F64
#  if !defined(RABALDR_SCRATCH_F32) || !defined(RABALDR_SCRATCH_F64)
#    error "Bad configuration"
#  endif
#endif

template <MIRType t>
struct RegTypeOf {
#ifdef ENABLE_WASM_SIMD
  static_assert(t == MIRType::Float32 || t == MIRType::Double ||
                    t == MIRType::Simd128,
                "Float mask type");
#else
  static_assert(t == MIRType::Float32 || t == MIRType::Double,
                "Float mask type");
#endif
};

template <>
struct RegTypeOf<MIRType::Float32> {
  static constexpr RegTypeName value = RegTypeName::Float32;
};
template <>
struct RegTypeOf<MIRType::Double> {
  static constexpr RegTypeName value = RegTypeName::Float64;
};
#ifdef ENABLE_WASM_SIMD
template <>
struct RegTypeOf<MIRType::Simd128> {
  static constexpr RegTypeName value = RegTypeName::Vector128;
};
#endif

// The strongly typed register wrappers are especially useful to distinguish
// float registers from double registers, but they also clearly distinguish
// 32-bit registers from 64-bit register pairs on 32-bit systems.

struct RegI32 : public Register {
  RegI32() : Register(Register::Invalid()) {}
  explicit RegI32(Register reg) : Register(reg) {
    MOZ_ASSERT(reg != Invalid());
  }
  bool isInvalid() const { return *this == Invalid(); }
  bool isValid() const { return !isInvalid(); }
  static RegI32 Invalid() { return RegI32(); }
};

struct RegI64 : public Register64 {
  RegI64() : Register64(Register64::Invalid()) {}
  explicit RegI64(Register64 reg) : Register64(reg) {
    MOZ_ASSERT(reg != Invalid());
  }
  bool isInvalid() const { return *this == Invalid(); }
  bool isValid() const { return !isInvalid(); }
  static RegI64 Invalid() { return RegI64(); }
};

// RegRef is for GC-pointers, for non GC-pointers use RegPtr
struct RegRef : public Register {
  RegRef() : Register(Register::Invalid()) {}
  explicit RegRef(Register reg) : Register(reg) {
    MOZ_ASSERT(reg != Invalid());
  }
  bool isInvalid() const { return *this == Invalid(); }
  bool isValid() const { return !isInvalid(); }
  static RegRef Invalid() { return RegRef(); }
};

// RegPtr is for non GC-pointers, for GC-pointers use RegRef
struct RegPtr : public Register {
  RegPtr() : Register(Register::Invalid()) {}
  explicit RegPtr(Register reg) : Register(reg) {
    MOZ_ASSERT(reg != Invalid());
  }
  bool isInvalid() const { return *this == Invalid(); }
  bool isValid() const { return !isInvalid(); }
  static RegPtr Invalid() { return RegPtr(); }
};

struct RegF32 : public FloatRegister {
  RegF32() : FloatRegister() {}
  explicit RegF32(FloatRegister reg) : FloatRegister(reg) {
    MOZ_ASSERT(isSingle());
  }
  bool isValid() const { return !isInvalid(); }
  static RegF32 Invalid() { return RegF32(); }
};

struct RegF64 : public FloatRegister {
  RegF64() : FloatRegister() {}
  explicit RegF64(FloatRegister reg) : FloatRegister(reg) {
    MOZ_ASSERT(isDouble());
  }
  bool isValid() const { return !isInvalid(); }
  static RegF64 Invalid() { return RegF64(); }
};

#ifdef ENABLE_WASM_SIMD
struct RegV128 : public FloatRegister {
  RegV128() : FloatRegister() {}
  explicit RegV128(FloatRegister reg) : FloatRegister(reg) {
    MOZ_ASSERT(isSimd128());
  }
  bool isValid() const { return !isInvalid(); }
  static RegV128 Invalid() { return RegV128(); }
};
#endif

struct AnyReg {
  union {
    RegI32 i32_;
    RegI64 i64_;
    RegRef ref_;
    RegF32 f32_;
    RegF64 f64_;
#ifdef ENABLE_WASM_SIMD
    RegV128 v128_;
#endif
  };

  enum {
    I32,
    I64,
    REF,
    F32,
    F64,
#ifdef ENABLE_WASM_SIMD
    V128
#endif
  } tag;

  explicit AnyReg(RegI32 r) {
    tag = I32;
    i32_ = r;
  }
  explicit AnyReg(RegI64 r) {
    tag = I64;
    i64_ = r;
  }
  explicit AnyReg(RegF32 r) {
    tag = F32;
    f32_ = r;
  }
  explicit AnyReg(RegF64 r) {
    tag = F64;
    f64_ = r;
  }
#ifdef ENABLE_WASM_SIMD
  explicit AnyReg(RegV128 r) {
    tag = V128;
    v128_ = r;
  }
#endif
  explicit AnyReg(RegRef r) {
    tag = REF;
    ref_ = r;
  }

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
#ifdef ENABLE_WASM_SIMD
  RegV128 v128() const {
    MOZ_ASSERT(tag == V128);
    return v128_;
  }
#endif
  RegRef ref() const {
    MOZ_ASSERT(tag == REF);
    return ref_;
  }

  AnyRegister any() const {
    switch (tag) {
      case F32:
        return AnyRegister(f32_);
      case F64:
        return AnyRegister(f64_);
#ifdef ENABLE_WASM_SIMD
      case V128:
        return AnyRegister(v128_);
#endif
      case I32:
        return AnyRegister(i32_);
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
      case REF:
        MOZ_CRASH("AnyReg::any() not implemented for ref types");
      default:
        MOZ_CRASH();
    }
    // Work around GCC 5 analysis/warning bug.
    MOZ_CRASH("AnyReg::any(): impossible case");
  }
};

// Platform-specific registers.
//
// All platforms must define struct SpecificRegs.  All 32-bit platforms must
// have an abiReturnRegI64 member in that struct.

#if defined(JS_CODEGEN_X64)
struct SpecificRegs {
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
        rdx(RegI64(Register64(js::jit::rdx))) {}
};
#elif defined(JS_CODEGEN_X86)
struct SpecificRegs {
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
        abiReturnRegI64(edx_eax) {}
};
#elif defined(JS_CODEGEN_ARM)
struct SpecificRegs {
  RegI64 abiReturnRegI64;

  SpecificRegs() : abiReturnRegI64(ReturnReg64) {}
};
#elif defined(JS_CODEGEN_ARM64)
struct SpecificRegs {};
#elif defined(JS_CODEGEN_MIPS32)
struct SpecificRegs {
  RegI64 abiReturnRegI64;

  SpecificRegs() : abiReturnRegI64(ReturnReg64) {}
};
#elif defined(JS_CODEGEN_MIPS64)
struct SpecificRegs {};
#else
struct SpecificRegs {
#  ifndef JS_64BIT
  RegI64 abiReturnRegI64;
#  endif

  SpecificRegs() { MOZ_CRASH("BaseCompiler porting interface: SpecificRegs"); }
};
#endif

class BaseCompilerInterface {
 public:
  // Spill all spillable registers.
  //
  // TODO / OPTIMIZE (Bug 1316802): It's possible to do better here by
  // spilling only enough registers to satisfy current needs.
  virtual void sync() = 0;
  virtual void saveTempPtr(RegPtr r) = 0;
  virtual void restoreTempPtr(RegPtr r) = 0;
};

// Register allocator.

class BaseRegAlloc {
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

  BaseCompilerInterface* bc;
  AllocatableGeneralRegisterSet availGPR;
  AllocatableFloatRegisterSet availFPU;
#ifdef DEBUG
  // The registers available after removing ScratchReg, HeapReg, etc.
  AllocatableGeneralRegisterSet allGPR;
  AllocatableFloatRegisterSet allFPU;
  uint32_t scratchTaken;
#endif
#ifdef JS_CODEGEN_X86
  AllocatableGeneralRegisterSet singleByteRegs;
#endif

  bool hasGPR() { return !availGPR.empty(); }

  bool hasGPR64() {
#ifdef JS_PUNBOX64
    return !availGPR.empty();
#else
    if (availGPR.empty()) {
      return false;
    }
    Register r = allocGPR();
    bool available = !availGPR.empty();
    freeGPR(r);
    return available;
#endif
  }

  template <MIRType t>
  bool hasFPU() {
    return availFPU.hasAny<RegTypeOf<t>::value>();
  }

  bool isAvailableGPR(Register r) { return availGPR.has(r); }

  bool isAvailableFPU(FloatRegister r) { return availFPU.has(r); }

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

  static constexpr uint32_t PAIR_LIMIT = 10;

  bool hasGPRPair() {
    for (uint32_t i = 0; i <= PAIR_LIMIT; i += 2) {
      if (isAvailableGPR(Register::FromCode(i)) &&
          isAvailableGPR(Register::FromCode(i + 1))) {
        return true;
      }
    }
    return false;
  }

  void allocGPRPair(Register* low, Register* high) {
    MOZ_ASSERT(hasGPRPair());
    for (uint32_t i = 0; i <= PAIR_LIMIT; i += 2) {
      if (isAvailableGPR(Register::FromCode(i)) &&
          isAvailableGPR(Register::FromCode(i + 1))) {
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

  template <MIRType t>
  FloatRegister allocFPU() {
    return availFPU.takeAny<RegTypeOf<t>::value>();
  }

  void freeGPR(Register r) { availGPR.add(r); }

  void freeInt64(Register64 r) {
#ifdef JS_PUNBOX64
    freeGPR(r.reg);
#else
    freeGPR(r.low);
    freeGPR(r.high);
#endif
  }

  void freeFPU(FloatRegister r) { availFPU.add(r); }

 public:
  explicit BaseRegAlloc()
      : bc(nullptr),
        availGPR(GeneralRegisterSet::All()),
        availFPU(FloatRegisterSet::All())
#ifdef DEBUG
        ,
        scratchTaken(0)
#endif
#ifdef JS_CODEGEN_X86
        ,
        singleByteRegs(GeneralRegisterSet(Registers::SingleByteRegs))
#endif
  {
    RegisterAllocator::takeWasmRegisters(availGPR);

    // Allocate any private scratch registers.
#if defined(RABALDR_SCRATCH_I32)
    if (RabaldrScratchI32 != RegI32::Invalid()) {
      availGPR.take(RabaldrScratchI32);
    }
#endif

#ifdef RABALDR_SCRATCH_F32_ALIASES_F64
    static_assert(RabaldrScratchF32 != InvalidFloatReg, "Float reg definition");
    static_assert(RabaldrScratchF64 != InvalidFloatReg, "Float reg definition");
#endif

#if defined(RABALDR_SCRATCH_F32) && !defined(RABALDR_SCRATCH_F32_ALIASES_F64)
    if (RabaldrScratchF32 != RegF32::Invalid()) {
      availFPU.take(RabaldrScratchF32);
    }
#endif

#if defined(RABALDR_SCRATCH_F64)
#  ifdef RABALDR_SCRATCH_F32_ALIASES_F64
    MOZ_ASSERT(availFPU.has(RabaldrScratchF32));
#  endif
    if (RabaldrScratchF64 != RegF64::Invalid()) {
      availFPU.take(RabaldrScratchF64);
    }
#  ifdef RABALDR_SCRATCH_F32_ALIASES_F64
    MOZ_ASSERT(!availFPU.has(RabaldrScratchF32));
#  endif
#endif

#ifdef DEBUG
    allGPR = availGPR;
    allFPU = availFPU;
#endif
  }

  void init(BaseCompilerInterface* bc) { this->bc = bc; }

  enum class ScratchKind { I32 = 1, F32 = 2, F64 = 4, V128 = 8 };

#ifdef DEBUG
  bool isScratchRegisterTaken(ScratchKind s) const {
    return (scratchTaken & uint32_t(s)) != 0;
  }

  void setScratchRegisterTaken(ScratchKind s, bool state) {
    if (state) {
      scratchTaken |= uint32_t(s);
    } else {
      scratchTaken &= ~uint32_t(s);
    }
  }
#endif

#ifdef JS_CODEGEN_X86
  bool isSingleByteI32(Register r) { return singleByteRegs.has(r); }
#endif

  bool isAvailableI32(RegI32 r) { return isAvailableGPR(r); }

  bool isAvailableI64(RegI64 r) {
#ifdef JS_PUNBOX64
    return isAvailableGPR(r.reg);
#else
    return isAvailableGPR(r.low) && isAvailableGPR(r.high);
#endif
  }

  bool isAvailableRef(RegRef r) { return isAvailableGPR(r); }

  bool isAvailablePtr(RegPtr r) { return isAvailableGPR(r); }

  bool isAvailableF32(RegF32 r) { return isAvailableFPU(r); }

  bool isAvailableF64(RegF64 r) { return isAvailableFPU(r); }

#ifdef ENABLE_WASM_SIMD
  bool isAvailableV128(RegV128 r) { return isAvailableFPU(r); }
#endif

  // TODO / OPTIMIZE (Bug 1316802): Do not sync everything on allocation
  // failure, only as much as we need.

  [[nodiscard]] RegI32 needI32() {
    if (!hasGPR()) {
      bc->sync();
    }
    return RegI32(allocGPR());
  }

  void needI32(RegI32 specific) {
    if (!isAvailableI32(specific)) {
      bc->sync();
    }
    allocGPR(specific);
  }

  [[nodiscard]] RegI64 needI64() {
    if (!hasGPR64()) {
      bc->sync();
    }
    return RegI64(allocInt64());
  }

  void needI64(RegI64 specific) {
    if (!isAvailableI64(specific)) {
      bc->sync();
    }
    allocInt64(specific);
  }

  [[nodiscard]] RegRef needRef() {
    if (!hasGPR()) {
      bc->sync();
    }
    return RegRef(allocGPR());
  }

  void needRef(RegRef specific) {
    if (!isAvailableRef(specific)) {
      bc->sync();
    }
    allocGPR(specific);
  }

  [[nodiscard]] RegPtr needPtr() {
    if (!hasGPR()) {
      bc->sync();
    }
    return RegPtr(allocGPR());
  }

  void needPtr(RegPtr specific) {
    if (!isAvailablePtr(specific)) {
      bc->sync();
    }
    allocGPR(specific);
  }

  // Use when you need a register for a short time but explicitly want to avoid
  // a full sync().
  [[nodiscard]] RegPtr needTempPtr(RegPtr fallback, bool* saved) {
    if (hasGPR()) {
      *saved = false;
      return RegPtr(allocGPR());
    }
    *saved = true;
    bc->saveTempPtr(fallback);
    MOZ_ASSERT(isAvailablePtr(fallback));
    allocGPR(fallback);
    return RegPtr(fallback);
  }

  [[nodiscard]] RegF32 needF32() {
    if (!hasFPU<MIRType::Float32>()) {
      bc->sync();
    }
    return RegF32(allocFPU<MIRType::Float32>());
  }

  void needF32(RegF32 specific) {
    if (!isAvailableF32(specific)) {
      bc->sync();
    }
    allocFPU(specific);
  }

  [[nodiscard]] RegF64 needF64() {
    if (!hasFPU<MIRType::Double>()) {
      bc->sync();
    }
    return RegF64(allocFPU<MIRType::Double>());
  }

  void needF64(RegF64 specific) {
    if (!isAvailableF64(specific)) {
      bc->sync();
    }
    allocFPU(specific);
  }

#ifdef ENABLE_WASM_SIMD
  [[nodiscard]] RegV128 needV128() {
    if (!hasFPU<MIRType::Simd128>()) {
      bc->sync();
    }
    return RegV128(allocFPU<MIRType::Simd128>());
  }

  void needV128(RegV128 specific) {
    if (!isAvailableV128(specific)) {
      bc->sync();
    }
    allocFPU(specific);
  }
#endif

  void freeI32(RegI32 r) { freeGPR(r); }

  void freeI64(RegI64 r) { freeInt64(r); }

  void freeRef(RegRef r) { freeGPR(r); }

  void freePtr(RegPtr r) { freeGPR(r); }

  void freeF64(RegF64 r) { freeFPU(r); }

  void freeF32(RegF32 r) { freeFPU(r); }

#ifdef ENABLE_WASM_SIMD
  void freeV128(RegV128 r) { freeFPU(r); }
#endif

  void freeTempPtr(RegPtr r, bool saved) {
    freePtr(r);
    if (saved) {
      bc->restoreTempPtr(r);
      MOZ_ASSERT(!isAvailablePtr(r));
    }
  }

#ifdef JS_CODEGEN_ARM
  [[nodiscard]] RegI64 needI64Pair() {
    if (!hasGPRPair()) {
      bc->sync();
    }
    Register low, high;
    allocGPRPair(&low, &high);
    return RegI64(Register64(high, low));
  }
#endif

#ifdef DEBUG
  friend class LeakCheck;

  class MOZ_RAII LeakCheck {
   private:
    const BaseRegAlloc& ra;
    AllocatableGeneralRegisterSet knownGPR_;
    AllocatableFloatRegisterSet knownFPU_;

   public:
    explicit LeakCheck(const BaseRegAlloc& ra) : ra(ra) {
      knownGPR_ = ra.availGPR;
      knownFPU_ = ra.availFPU;
    }

    ~LeakCheck() {
      MOZ_ASSERT(knownGPR_.bits() == ra.allGPR.bits());
      MOZ_ASSERT(knownFPU_.bits() == ra.allFPU.bits());
    }

    void addKnownI32(RegI32 r) { knownGPR_.add(r); }

    void addKnownI64(RegI64 r) {
#  ifdef JS_PUNBOX64
      knownGPR_.add(r.reg);
#  else
      knownGPR_.add(r.high);
      knownGPR_.add(r.low);
#  endif
    }

    void addKnownF32(RegF32 r) { knownFPU_.add(r); }

    void addKnownF64(RegF64 r) { knownFPU_.add(r); }

#  ifdef ENABLE_WASM_SIMD
    void addKnownV128(RegV128 r) { knownFPU_.add(r); }
#  endif

    void addKnownRef(RegRef r) { knownGPR_.add(r); }
  };
#endif
};

// Scratch register abstractions.
//
// We define our own scratch registers when the platform doesn't provide what we
// need.  A notable use case is that we will need a private scratch register
// when the platform masm uses its scratch register very frequently (eg, ARM).

class BaseScratchRegister {
#ifdef DEBUG
  BaseRegAlloc& ra;
  BaseRegAlloc::ScratchKind kind_;

 public:
  explicit BaseScratchRegister(BaseRegAlloc& ra, BaseRegAlloc::ScratchKind kind)
      : ra(ra), kind_(kind) {
    MOZ_ASSERT(!ra.isScratchRegisterTaken(kind_));
    ra.setScratchRegisterTaken(kind_, true);
  }
  ~BaseScratchRegister() {
    MOZ_ASSERT(ra.isScratchRegisterTaken(kind_));
    ra.setScratchRegisterTaken(kind_, false);
  }
#else
 public:
  explicit BaseScratchRegister(BaseRegAlloc& ra,
                               BaseRegAlloc::ScratchKind kind) {}
#endif
};

#ifdef ENABLE_WASM_SIMD
#  ifdef RABALDR_SCRATCH_V128
class ScratchV128 : public BaseScratchRegister {
 public:
  explicit ScratchV128(BaseRegAlloc& ra)
      : BaseScratchRegister(ra, BaseRegAlloc::ScratchKind::V128) {}
  operator RegV128() const { return RegV128(RabaldrScratchV128); }
};
#  else
class ScratchV128 : public ScratchSimd128Scope {
 public:
  explicit ScratchV128(MacroAssembler& m) : ScratchSimd128Scope(m) {}
  operator RegV128() const { return RegV128(FloatRegister(*this)); }
};
#  endif
#endif

#ifdef RABALDR_SCRATCH_F64
class ScratchF64 : public BaseScratchRegister {
 public:
  explicit ScratchF64(BaseRegAlloc& ra)
      : BaseScratchRegister(ra, BaseRegAlloc::ScratchKind::F64) {}
  operator RegF64() const { return RegF64(RabaldrScratchF64); }
};
#else
class ScratchF64 : public ScratchDoubleScope {
 public:
  explicit ScratchF64(MacroAssembler& m) : ScratchDoubleScope(m) {}
  operator RegF64() const { return RegF64(FloatRegister(*this)); }
};
#endif

#ifdef RABALDR_SCRATCH_F32
class ScratchF32 : public BaseScratchRegister {
 public:
  explicit ScratchF32(BaseRegAlloc& ra)
      : BaseScratchRegister(ra, BaseRegAlloc::ScratchKind::F32) {}
  operator RegF32() const { return RegF32(RabaldrScratchF32); }
};
#else
class ScratchF32 : public ScratchFloat32Scope {
 public:
  explicit ScratchF32(MacroAssembler& m) : ScratchFloat32Scope(m) {}
  operator RegF32() const { return RegF32(FloatRegister(*this)); }
};
#endif

#ifdef RABALDR_SCRATCH_I32
template <class RegType>
class ScratchGPR : public BaseScratchRegister {
 public:
  explicit ScratchGPR(BaseRegAlloc& ra)
      : BaseScratchRegister(ra, BaseRegAlloc::ScratchKind::I32) {}
  operator RegType() const { return RegType(RabaldrScratchI32); }
};
#else
template <class RegType>
class ScratchGPR : public ScratchRegisterScope {
 public:
  explicit ScratchGPR(MacroAssembler& m) : ScratchRegisterScope(m) {}
  operator RegType() const { return RegType(Register(*this)); }
};
#endif

using ScratchI32 = ScratchGPR<RegI32>;
using ScratchPtr = ScratchGPR<RegPtr>;
using ScratchRef = ScratchGPR<RegRef>;

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
// The stack frame has four parts ("below" means at lower addresses):
//
//  - the Frame element;
//  - the Local area, including the DebugFrame element and possibly a spilled
//    pointer to stack results, if any; allocated below the header with various
//    forms of alignment;
//  - the Dynamic area, comprising the temporary storage the compiler uses for
//    register spilling, allocated below the Local area;
//  - the Arguments area, comprising memory allocated for outgoing calls,
//    allocated below the Dynamic area.
//
//                +==============================+
//                |    Incoming stack arg        |
//                |    ...                       |
// -------------  +==============================+
//                |    Frame (fixed size)        |
// -------------  +==============================+ <-------------------- FP
//         ^      |    DebugFrame (optional)     |    ^  ^             ^^
//   localSize    |    Register arg local        |    |  |             ||
//         |      |    ...                       |    |  |     framePushed
//         |      |    Register stack result ptr?|    |  |             ||
//         |      |    Non-arg local             |    |  |             ||
//         |      |    ...                       |    |  |             ||
//         |      |    (padding)                 |    |  |             ||
//         |      |    Tls pointer               |    |  |             ||
//         |      +------------------------------+    |  |             ||
//         v      |    (padding)                 |    |  v             ||
// -------------  +==============================+ currentStackHeight  ||
//         ^      |    Dynamic (variable size)   |    |                ||
//  dynamicSize   |    ...                       |    |                ||
//         v      |    ...                       |    v                ||
// -------------  |    (free space, sometimes)   | ---------           v|
//                +==============================+ <----- SP not-during calls
//                |    Arguments (sometimes)     |                      |
//                |    ...                       |                      v
//                +==============================+ <----- SP during calls
//
// The Frame is addressed off the stack pointer.  masm.framePushed() is always
// correct, and masm.getStackPointer() + masm.framePushed() always addresses the
// Frame, with the DebugFrame optionally below it.
//
// The Local area (including the DebugFrame and, if needed, the spilled value of
// the stack results area pointer) is laid out by BaseLocalIter and is allocated
// and deallocated by standard prologue and epilogue functions that manipulate
// the stack pointer, but it is accessed via BaseStackFrame.
//
// The Dynamic area is maintained by and accessed via BaseStackFrame.  On some
// systems (such as ARM64), the Dynamic memory may be allocated in chunks
// because the SP needs a specific alignment, and in this case there will
// normally be some free space directly above the SP.  The stack height does not
// include the free space, it reflects the logically used space only.
//
// The Dynamic area is where space for stack results is allocated when calling
// functions that return results on the stack.  If a function has stack results,
// a pointer to the low address of the stack result area is passed as an
// additional argument, according to the usual ABI.  See
// ABIResultIter::HasStackResults.
//
// The Arguments area is allocated and deallocated via BaseStackFrame (see
// comments later) but is accessed directly off the stack pointer.

// BaseLocalIter iterates over a vector of types of locals and provides offsets
// from the Frame address for those locals, and associated data.
//
// The implementation of BaseLocalIter is the property of the BaseStackFrame.
// But it is also exposed for eg the debugger to use.

BaseLocalIter::BaseLocalIter(const ValTypeVector& locals,
                             const ArgTypeVector& args, bool debugEnabled)
    : locals_(locals),
      args_(args),
      argsIter_(args_),
      index_(0),
      frameSize_(0),
      nextFrameSize_(debugEnabled ? DebugFrame::offsetOfFrame() : 0),
      frameOffset_(INT32_MAX),
      stackResultPointerOffset_(INT32_MAX),
      mirType_(MIRType::Undefined),
      done_(false) {
  MOZ_ASSERT(args.lengthWithoutStackResults() <= locals.length());
  settle();
}

int32_t BaseLocalIter::pushLocal(size_t nbytes) {
  MOZ_ASSERT(nbytes % 4 == 0 && nbytes <= 16);
  nextFrameSize_ = AlignBytes(frameSize_, nbytes) + nbytes;
  return nextFrameSize_;  // Locals grow down so capture base address.
}

void BaseLocalIter::settle() {
  MOZ_ASSERT(!done_);
  frameSize_ = nextFrameSize_;

  if (!argsIter_.done()) {
    mirType_ = argsIter_.mirType();
    MIRType concreteType = mirType_;
    switch (mirType_) {
      case MIRType::StackResults:
        // The pointer to stack results is handled like any other argument:
        // either addressed in place if it is passed on the stack, or we spill
        // it in the frame if it's in a register.
        MOZ_ASSERT(args_.isSyntheticStackResultPointerArg(index_));
        concreteType = MIRType::Pointer;
        [[fallthrough]];
      case MIRType::Int32:
      case MIRType::Int64:
      case MIRType::Double:
      case MIRType::Float32:
      case MIRType::RefOrNull:
#ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
#endif
        if (argsIter_->argInRegister()) {
          frameOffset_ = pushLocal(MIRTypeToSize(concreteType));
        } else {
          frameOffset_ = -(argsIter_->offsetFromArgBase() + sizeof(Frame));
        }
        break;
      default:
        MOZ_CRASH("Argument type");
    }
    if (mirType_ == MIRType::StackResults) {
      stackResultPointerOffset_ = frameOffset();
      // Advance past the synthetic stack result pointer argument and fall
      // through to the next case.
      argsIter_++;
      frameSize_ = nextFrameSize_;
      MOZ_ASSERT(argsIter_.done());
    } else {
      return;
    }
  }

  if (index_ < locals_.length()) {
    switch (locals_[index_].kind()) {
      case ValType::I32:
      case ValType::I64:
      case ValType::F32:
      case ValType::F64:
#ifdef ENABLE_WASM_SIMD
      case ValType::V128:
#endif
      case ValType::Ref:
        // TODO/AnyRef-boxing: With boxed immediates and strings, the
        // debugger must be made aware that AnyRef != Pointer.
        ASSERT_ANYREF_IS_JSOBJECT;
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

void BaseLocalIter::operator++(int) {
  MOZ_ASSERT(!done_);
  index_++;
  if (!argsIter_.done()) {
    argsIter_++;
  }
  settle();
}

// Abstraction of the height of the stack frame, to avoid type confusion.

class StackHeight {
  friend class BaseStackFrameAllocator;

  uint32_t height;

 public:
  explicit StackHeight(uint32_t h) : height(h) {}
  static StackHeight Invalid() { return StackHeight(UINT32_MAX); }
  bool isValid() const { return height != UINT32_MAX; }
  bool operator==(StackHeight rhs) const {
    MOZ_ASSERT(isValid() && rhs.isValid());
    return height == rhs.height;
  }
  bool operator!=(StackHeight rhs) const { return !(*this == rhs); }
};

// Abstraction for where multi-value results go on the machine stack.

class StackResultsLoc {
  uint32_t bytes_;
  size_t count_;
  Maybe<uint32_t> height_;

 public:
  StackResultsLoc() : bytes_(0), count_(0){};
  StackResultsLoc(uint32_t bytes, size_t count, uint32_t height)
      : bytes_(bytes), count_(count), height_(Some(height)) {
    MOZ_ASSERT(bytes != 0);
    MOZ_ASSERT(count != 0);
    MOZ_ASSERT(height != 0);
  }

  uint32_t bytes() const { return bytes_; }
  uint32_t count() const { return count_; }
  uint32_t height() const { return height_.value(); }

  bool hasStackResults() const { return bytes() != 0; }
  StackResults stackResults() const {
    return hasStackResults() ? StackResults::HasStackResults
                             : StackResults::NoStackResults;
  }
};

// Abstraction of the baseline compiler's stack frame (except for the Frame /
// DebugFrame parts).  See comments above for more.  Remember, "below" on the
// stack means at lower addresses.
//
// The abstraction is split into two parts: BaseStackFrameAllocator is
// responsible for allocating and deallocating space on the stack and for
// performing computations that are affected by how the allocation is performed;
// BaseStackFrame then provides a pleasant interface for stack frame management.

class BaseStackFrameAllocator {
  MacroAssembler& masm;

#ifdef RABALDR_CHUNKY_STACK
  // On platforms that require the stack pointer to be aligned on a boundary
  // greater than the typical stack item (eg, ARM64 requires 16-byte alignment
  // but items are 8 bytes), allocate stack memory in chunks, and use a
  // separate stack height variable to track the effective stack pointer
  // within the allocated area.  Effectively, there's a variable amount of
  // free space directly above the stack pointer.  See diagram above.

  // The following must be true in order for the stack height to be
  // predictable at control flow joins:
  //
  // - The Local area is always aligned according to WasmStackAlignment, ie,
  //   masm.framePushed() % WasmStackAlignment is zero after allocating
  //   locals.
  //
  // - ChunkSize is always a multiple of WasmStackAlignment.
  //
  // - Pushing and popping are always in units of ChunkSize (hence preserving
  //   alignment).
  //
  // - The free space on the stack (masm.framePushed() - currentStackHeight_)
  //   is a predictable (nonnegative) amount.

  // As an optimization, we pre-allocate some space on the stack, the size of
  // this allocation is InitialChunk and it must be a multiple of ChunkSize.
  // It is allocated as part of the function prologue and deallocated as part
  // of the epilogue, along with the locals.
  //
  // If ChunkSize is too large then we risk overflowing the stack on simple
  // recursions with few live values where stack overflow should not be a
  // risk; if it is too small we spend too much time adjusting the stack
  // pointer.
  //
  // Good values for ChunkSize are the subject of future empirical analysis;
  // eight words is just an educated guess.

  static constexpr uint32_t ChunkSize = 8 * sizeof(void*);
  static constexpr uint32_t InitialChunk = ChunkSize;

  // The current logical height of the frame is
  //   currentStackHeight_ = localSize_ + dynamicSize
  // where dynamicSize is not accounted for explicitly and localSize_ also
  // includes size for the DebugFrame.
  //
  // The allocated size of the frame, provided by masm.framePushed(), is usually
  // larger than currentStackHeight_, notably at the beginning of execution when
  // we've allocated InitialChunk extra space.

  uint32_t currentStackHeight_;
#endif

  // Size of the Local area in bytes (stable after BaseCompiler::init() has
  // called BaseStackFrame::setupLocals(), which in turn calls
  // BaseStackFrameAllocator::setLocalSize()), always rounded to the proper
  // stack alignment.  The Local area is then allocated in beginFunction(),
  // following the allocation of the Header.  See onFixedStackAllocated()
  // below.

  uint32_t localSize_;

 protected:
  ///////////////////////////////////////////////////////////////////////////
  //
  // Initialization

  explicit BaseStackFrameAllocator(MacroAssembler& masm)
      : masm(masm),
#ifdef RABALDR_CHUNKY_STACK
        currentStackHeight_(0),
#endif
        localSize_(UINT32_MAX) {
  }

 protected:
  //////////////////////////////////////////////////////////////////////
  //
  // The Local area - the static part of the frame.

  // Record the size of the Local area, once it is known.

  void setLocalSize(uint32_t localSize) {
    MOZ_ASSERT(localSize == AlignBytes(localSize, sizeof(void*)),
               "localSize_ should be aligned to at least a pointer");
    MOZ_ASSERT(localSize_ == UINT32_MAX);
    localSize_ = localSize;
  }

  // Record the current stack height, after it has become stable in
  // beginFunction().  See also BaseStackFrame::onFixedStackAllocated().

  void onFixedStackAllocated() {
    MOZ_ASSERT(localSize_ != UINT32_MAX);
#ifdef RABALDR_CHUNKY_STACK
    currentStackHeight_ = localSize_;
#endif
  }

 public:
  // The fixed amount of memory, in bytes, allocated on the stack below the
  // Header for purposes such as locals and other fixed values.  Includes all
  // necessary alignment, and on ARM64 also the initial chunk for the working
  // stack memory.

  uint32_t fixedAllocSize() const {
    MOZ_ASSERT(localSize_ != UINT32_MAX);
#ifdef RABALDR_CHUNKY_STACK
    return localSize_ + InitialChunk;
#else
    return localSize_;
#endif
  }

#ifdef RABALDR_CHUNKY_STACK
  // The allocated frame size is frequently larger than the logical stack
  // height; we round up to a chunk boundary, and special case the initial
  // chunk.
  uint32_t framePushedForHeight(uint32_t logicalHeight) {
    if (logicalHeight <= fixedAllocSize()) {
      return fixedAllocSize();
    }
    return fixedAllocSize() +
           AlignBytes(logicalHeight - fixedAllocSize(), ChunkSize);
  }
#endif

 protected:
  //////////////////////////////////////////////////////////////////////
  //
  // The Dynamic area - the dynamic part of the frame, for spilling and saving
  // intermediate values.

  // Offset off of sp_ for the slot at stack area location `offset`.

  int32_t stackOffset(int32_t offset) {
    MOZ_ASSERT(offset > 0);
    return masm.framePushed() - offset;
  }

  uint32_t computeHeightWithStackResults(StackHeight stackBase,
                                         uint32_t stackResultBytes) {
    MOZ_ASSERT(stackResultBytes);
    MOZ_ASSERT(currentStackHeight() >= stackBase.height);
    return stackBase.height + stackResultBytes;
  }

#ifdef RABALDR_CHUNKY_STACK
  void pushChunkyBytes(uint32_t bytes) {
    checkChunkyInvariants();
    uint32_t freeSpace = masm.framePushed() - currentStackHeight_;
    if (freeSpace < bytes) {
      uint32_t bytesToReserve = AlignBytes(bytes - freeSpace, ChunkSize);
      MOZ_ASSERT(bytesToReserve + freeSpace >= bytes);
      masm.reserveStack(bytesToReserve);
    }
    currentStackHeight_ += bytes;
    checkChunkyInvariants();
  }

  void popChunkyBytes(uint32_t bytes) {
    checkChunkyInvariants();
    currentStackHeight_ -= bytes;
    // Sometimes, popChunkyBytes() is used to pop a larger area, as when we drop
    // values consumed by a call, and we may need to drop several chunks.  But
    // never drop the initial chunk.  Crucially, the amount we drop is always an
    // integral number of chunks.
    uint32_t freeSpace = masm.framePushed() - currentStackHeight_;
    if (freeSpace >= ChunkSize) {
      uint32_t targetAllocSize = framePushedForHeight(currentStackHeight_);
      uint32_t amountToFree = masm.framePushed() - targetAllocSize;
      MOZ_ASSERT(amountToFree % ChunkSize == 0);
      if (amountToFree) {
        masm.freeStack(amountToFree);
      }
    }
    checkChunkyInvariants();
  }
#endif

  uint32_t currentStackHeight() const {
#ifdef RABALDR_CHUNKY_STACK
    return currentStackHeight_;
#else
    return masm.framePushed();
#endif
  }

 private:
#ifdef RABALDR_CHUNKY_STACK
  void checkChunkyInvariants() {
    MOZ_ASSERT(masm.framePushed() >= fixedAllocSize());
    MOZ_ASSERT(masm.framePushed() >= currentStackHeight_);
    MOZ_ASSERT(masm.framePushed() == fixedAllocSize() ||
               masm.framePushed() - currentStackHeight_ < ChunkSize);
    MOZ_ASSERT((masm.framePushed() - localSize_) % ChunkSize == 0);
  }
#endif

  // For a given stack height, return the appropriate size of the allocated
  // frame.

  uint32_t framePushedForHeight(StackHeight stackHeight) {
#ifdef RABALDR_CHUNKY_STACK
    // A more complicated adjustment is needed.
    return framePushedForHeight(stackHeight.height);
#else
    // The allocated frame size equals the stack height.
    return stackHeight.height;
#endif
  }

 public:
  // The current height of the stack area, not necessarily zero-based, in a
  // type-safe way.

  StackHeight stackHeight() const { return StackHeight(currentStackHeight()); }

  // Set the frame height to a previously recorded value.

  void setStackHeight(StackHeight amount) {
#ifdef RABALDR_CHUNKY_STACK
    currentStackHeight_ = amount.height;
    masm.setFramePushed(framePushedForHeight(amount));
    checkChunkyInvariants();
#else
    masm.setFramePushed(amount.height);
#endif
  }

  // The current height of the dynamic part of the stack area (ie, the backing
  // store for the evaluation stack), zero-based.

  uint32_t dynamicHeight() const { return currentStackHeight() - localSize_; }

  // Before branching to an outer control label, pop the execution stack to
  // the level expected by that region, but do not update masm.framePushed()
  // as that will happen as compilation leaves the block.
  //
  // Note these operate directly on the stack pointer register.

  void popStackBeforeBranch(StackHeight destStackHeight,
                            uint32_t stackResultBytes) {
    uint32_t framePushedHere = masm.framePushed();
    StackHeight heightThere =
        StackHeight(destStackHeight.height + stackResultBytes);
    uint32_t framePushedThere = framePushedForHeight(heightThere);
    if (framePushedHere > framePushedThere) {
      masm.addToStackPtr(Imm32(framePushedHere - framePushedThere));
    }
  }

  void popStackBeforeBranch(StackHeight destStackHeight, ResultType type) {
    popStackBeforeBranch(destStackHeight,
                         ABIResultIter::MeasureStackBytes(type));
  }

  // Given that there are |stackParamSize| bytes on the dynamic stack
  // corresponding to the stack results, return the stack height once these
  // parameters are popped.

  StackHeight stackResultsBase(uint32_t stackParamSize) {
    return StackHeight(currentStackHeight() - stackParamSize);
  }

  // For most of WebAssembly, adjacent instructions have fallthrough control
  // flow between them, which allows us to simply thread the current stack
  // height through the compiler.  There are two exceptions to this rule: when
  // leaving a block via dead code, and when entering the "else" arm of an "if".
  // In these cases, the stack height is the block entry height, plus any stack
  // values (results in the block exit case, parameters in the else entry case).

  void resetStackHeight(StackHeight destStackHeight, ResultType type) {
    uint32_t height = destStackHeight.height;
    height += ABIResultIter::MeasureStackBytes(type);
    setStackHeight(StackHeight(height));
  }

  // Return offset of stack result.

  uint32_t locateStackResult(const ABIResult& result, StackHeight stackBase,
                             uint32_t stackResultBytes) {
    MOZ_ASSERT(result.onStack());
    MOZ_ASSERT(result.stackOffset() + result.size() <= stackResultBytes);
    uint32_t end = computeHeightWithStackResults(stackBase, stackResultBytes);
    return end - result.stackOffset();
  }

 public:
  //////////////////////////////////////////////////////////////////////
  //
  // The Argument area - for outgoing calls.
  //
  // We abstract these operations as an optimization: we can merge the freeing
  // of the argument area and dropping values off the stack after a call.  But
  // they always amount to manipulating the real stack pointer by some amount.
  //
  // Note that we do not update currentStackHeight_ for this; the frame does
  // not know about outgoing arguments.  But we do update framePushed(), so we
  // can still index into the frame below the outgoing arguments area.

  // This is always equivalent to a masm.reserveStack() call.

  void allocArgArea(size_t argSize) {
    if (argSize) {
      masm.reserveStack(argSize);
    }
  }

  // This frees the argument area allocated by allocArgArea(), and `argSize`
  // must be equal to the `argSize` argument to allocArgArea().  In addition
  // we drop some values from the frame, corresponding to the values that were
  // consumed by the call.

  void freeArgAreaAndPopBytes(size_t argSize, size_t dropSize) {
#ifdef RABALDR_CHUNKY_STACK
    // Freeing the outgoing arguments and freeing the consumed values have
    // different semantics here, which is why the operation is split.
    if (argSize) {
      masm.freeStack(argSize);
    }
    popChunkyBytes(dropSize);
#else
    if (argSize + dropSize) {
      masm.freeStack(argSize + dropSize);
    }
#endif
  }
};

class BaseStackFrame final : public BaseStackFrameAllocator {
  MacroAssembler& masm;

  // The largest observed value of masm.framePushed(), ie, the size of the
  // stack frame.  Read this for its true value only when code generation is
  // finished.
  uint32_t maxFramePushed_;

  // Patch point where we check for stack overflow.
  CodeOffset stackAddOffset_;

  // Low byte offset of pointer to stack results, if any.
  Maybe<int32_t> stackResultsPtrOffset_;

  // The offset of TLS pointer.
  uint32_t tlsPointerOffset_;

  // Low byte offset of local area for true locals (not parameters).
  uint32_t varLow_;

  // High byte offset + 1 of local area for true locals.
  uint32_t varHigh_;

  // The stack pointer, cached for brevity.
  RegisterOrSP sp_;

 public:
  explicit BaseStackFrame(MacroAssembler& masm)
      : BaseStackFrameAllocator(masm),
        masm(masm),
        maxFramePushed_(0),
        stackAddOffset_(0),
        tlsPointerOffset_(UINT32_MAX),
        varLow_(UINT32_MAX),
        varHigh_(UINT32_MAX),
        sp_(masm.getStackPointer()) {}

  ///////////////////////////////////////////////////////////////////////////
  //
  // Stack management and overflow checking

  // This must be called once beginFunction has allocated space for the Header
  // (the Frame and DebugFrame) and the Local area, and will record the current
  // frame size for internal use by the stack abstractions.

  void onFixedStackAllocated() {
    maxFramePushed_ = masm.framePushed();
    BaseStackFrameAllocator::onFixedStackAllocated();
  }

  // We won't know until after we've generated code how big the frame will be
  // (we may need arbitrary spill slots and outgoing param slots) so emit a
  // patchable add that is patched in endFunction().
  //
  // Note the platform scratch register may be used by branchPtr(), so
  // generally tmp must be something else.

  void checkStack(Register tmp, BytecodeOffset trapOffset) {
    stackAddOffset_ = masm.sub32FromStackPtrWithPatch(tmp);
    Label ok;
    masm.branchPtr(Assembler::Below,
                   Address(WasmTlsReg, offsetof(wasm::TlsData, stackLimit)),
                   tmp, &ok);
    masm.wasmTrap(Trap::StackOverflow, trapOffset);
    masm.bind(&ok);
  }

  void patchCheckStack() {
    masm.patchSub32FromStackPtr(stackAddOffset_,
                                Imm32(int32_t(maxFramePushed_)));
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
    return maxFramePushed_ <= 512 * 1024;
  }

  ///////////////////////////////////////////////////////////////////////////
  //
  // Local area

  struct Local {
    // Type of the value.
    const MIRType type;

    // Byte offset from Frame "into" the locals, ie positive for true locals
    // and negative for incoming args that read directly from the arg area.
    // It assumes the stack is growing down and that locals are on the stack
    // at lower addresses than Frame, and is the offset from Frame of the
    // lowest-addressed byte of the local.
    const int32_t offs;

    Local(MIRType type, int32_t offs) : type(type), offs(offs) {}

    bool isStackArgument() const { return offs < 0; }
  };

  // Profiling shows that the number of parameters and locals frequently
  // touches or exceeds 8.  So 16 seems like a reasonable starting point.
  using LocalVector = Vector<Local, 16, SystemAllocPolicy>;

  // Initialize `localInfo` based on the types of `locals` and `args`.
  [[nodiscard]] bool setupLocals(const ValTypeVector& locals,
                                 const ArgTypeVector& args, bool debugEnabled,
                                 LocalVector* localInfo) {
    if (!localInfo->reserve(locals.length())) {
      return false;
    }

    DebugOnly<uint32_t> index = 0;
    BaseLocalIter i(locals, args, debugEnabled);
    for (; !i.done() && i.index() < args.lengthWithoutStackResults(); i++) {
      MOZ_ASSERT(i.isArg());
      MOZ_ASSERT(i.index() == index);
      localInfo->infallibleEmplaceBack(i.mirType(), i.frameOffset());
      index++;
    }

    varLow_ = i.frameSize();
    for (; !i.done(); i++) {
      MOZ_ASSERT(!i.isArg());
      MOZ_ASSERT(i.index() == index);
      localInfo->infallibleEmplaceBack(i.mirType(), i.frameOffset());
      index++;
    }
    varHigh_ = i.frameSize();

    // Reserve an additional stack slot for the TLS pointer.
    const uint32_t pointerAlignedVarHigh = AlignBytes(varHigh_, sizeof(void*));
    const uint32_t localSize = pointerAlignedVarHigh + sizeof(void*);
    tlsPointerOffset_ = localSize;

    setLocalSize(AlignBytes(localSize, WasmStackAlignment));

    if (args.hasSyntheticStackResultPointerArg()) {
      stackResultsPtrOffset_ = Some(i.stackResultPointerOffset());
    }

    return true;
  }

  void zeroLocals(BaseRegAlloc* ra);

  Address addressOfLocal(const Local& local, uint32_t additionalOffset = 0) {
    if (local.isStackArgument()) {
      return Address(FramePointer,
                     stackArgumentOffsetFromFp(local) + additionalOffset);
    }
    return Address(sp_, localOffsetFromSp(local) + additionalOffset);
  }

  void loadLocalI32(const Local& src, RegI32 dest) {
    masm.load32(addressOfLocal(src), dest);
  }

#ifndef JS_PUNBOX64
  void loadLocalI64Low(const Local& src, RegI32 dest) {
    masm.load32(addressOfLocal(src, INT64LOW_OFFSET), dest);
  }

  void loadLocalI64High(const Local& src, RegI32 dest) {
    masm.load32(addressOfLocal(src, INT64HIGH_OFFSET), dest);
  }
#endif

  void loadLocalI64(const Local& src, RegI64 dest) {
    masm.load64(addressOfLocal(src), dest);
  }

  void loadLocalRef(const Local& src, RegRef dest) {
    masm.loadPtr(addressOfLocal(src), dest);
  }

  void loadLocalF64(const Local& src, RegF64 dest) {
    masm.loadDouble(addressOfLocal(src), dest);
  }

  void loadLocalF32(const Local& src, RegF32 dest) {
    masm.loadFloat32(addressOfLocal(src), dest);
  }

#ifdef ENABLE_WASM_SIMD
  void loadLocalV128(const Local& src, RegV128 dest) {
    masm.loadUnalignedSimd128(addressOfLocal(src), dest);
  }
#endif

  void storeLocalI32(RegI32 src, const Local& dest) {
    masm.store32(src, addressOfLocal(dest));
  }

  void storeLocalI64(RegI64 src, const Local& dest) {
    masm.store64(src, addressOfLocal(dest));
  }

  void storeLocalRef(RegRef src, const Local& dest) {
    masm.storePtr(src, addressOfLocal(dest));
  }

  void storeLocalF64(RegF64 src, const Local& dest) {
    masm.storeDouble(src, addressOfLocal(dest));
  }

  void storeLocalF32(RegF32 src, const Local& dest) {
    masm.storeFloat32(src, addressOfLocal(dest));
  }

#ifdef ENABLE_WASM_SIMD
  void storeLocalV128(RegV128 src, const Local& dest) {
    masm.storeUnalignedSimd128(src, addressOfLocal(dest));
  }
#endif

  // Offset off of sp_ for `local`.
  int32_t localOffsetFromSp(const Local& local) {
    MOZ_ASSERT(!local.isStackArgument());
    return localOffset(local.offs);
  }

  // Offset off of frame pointer for `stack argument`.
  int32_t stackArgumentOffsetFromFp(const Local& local) {
    MOZ_ASSERT(local.isStackArgument());
    return -local.offs;
  }

  // The incoming stack result area pointer is for stack results of the function
  // being compiled.
  void loadIncomingStackResultAreaPtr(RegPtr reg) {
    const int32_t offset = stackResultsPtrOffset_.value();
    Address src = offset < 0 ? Address(FramePointer, -offset)
                             : Address(sp_, stackOffset(offset));
    masm.loadPtr(src, reg);
  }

  void storeIncomingStackResultAreaPtr(RegPtr reg) {
    // If we get here, that means the pointer to the stack results area was
    // passed in as a register, and therefore it will be spilled below the
    // frame, so the offset is a positive height.
    MOZ_ASSERT(stackResultsPtrOffset_.value() > 0);
    masm.storePtr(reg,
                  Address(sp_, stackOffset(stackResultsPtrOffset_.value())));
  }

  void loadTlsPtr(Register dst) {
    masm.loadPtr(Address(sp_, stackOffset(tlsPointerOffset_)), dst);
  }

  void storeTlsPtr(Register tls) {
    masm.storePtr(tls, Address(sp_, stackOffset(tlsPointerOffset_)));
  }

  int32_t getTlsPtrOffset() { return stackOffset(tlsPointerOffset_); }

  // An outgoing stack result area pointer is for stack results of callees of
  // the function being compiled.
  void computeOutgoingStackResultAreaPtr(const StackResultsLoc& results,
                                         RegPtr dest) {
    MOZ_ASSERT(results.height() <= masm.framePushed());
    uint32_t offsetFromSP = masm.framePushed() - results.height();
    masm.moveStackPtrTo(dest);
    if (offsetFromSP) {
      masm.addPtr(Imm32(offsetFromSP), dest);
    }
  }

 private:
  // Offset off of sp_ for a local with offset `offset` from Frame.
  int32_t localOffset(int32_t offset) { return masm.framePushed() - offset; }

 public:
  ///////////////////////////////////////////////////////////////////////////
  //
  // Dynamic area

  static constexpr size_t StackSizeOfPtr = ABIResult::StackSizeOfPtr;
  static constexpr size_t StackSizeOfInt64 = ABIResult::StackSizeOfInt64;
  static constexpr size_t StackSizeOfFloat = ABIResult::StackSizeOfFloat;
  static constexpr size_t StackSizeOfDouble = ABIResult::StackSizeOfDouble;
#ifdef ENABLE_WASM_SIMD
  static constexpr size_t StackSizeOfV128 = ABIResult::StackSizeOfV128;
#endif

  // Pushes the register `r` to the stack. This pushes the full 64-bit width on
  // 64-bit systems, and 32-bits otherwise.
  uint32_t pushGPR(Register r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    pushChunkyBytes(StackSizeOfPtr);
    masm.storePtr(r, Address(sp_, stackOffset(currentStackHeight())));
#else
    masm.Push(r);
#endif
    maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    MOZ_ASSERT(stackBefore + StackSizeOfPtr == currentStackHeight());
    return currentStackHeight();
  }

  uint32_t pushFloat32(FloatRegister r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    pushChunkyBytes(StackSizeOfFloat);
    masm.storeFloat32(r, Address(sp_, stackOffset(currentStackHeight())));
#else
    masm.Push(r);
#endif
    maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    MOZ_ASSERT(stackBefore + StackSizeOfFloat == currentStackHeight());
    return currentStackHeight();
  }

#ifdef ENABLE_WASM_SIMD
  uint32_t pushV128(RegV128 r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#  ifdef RABALDR_CHUNKY_STACK
    pushChunkyBytes(StackSizeOfV128);
#  else
    masm.adjustStack(-(int)StackSizeOfV128);
#  endif
    masm.storeUnalignedSimd128(r,
                               Address(sp_, stackOffset(currentStackHeight())));
    maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    MOZ_ASSERT(stackBefore + StackSizeOfV128 == currentStackHeight());
    return currentStackHeight();
  }
#endif

  uint32_t pushDouble(FloatRegister r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    pushChunkyBytes(StackSizeOfDouble);
    masm.storeDouble(r, Address(sp_, stackOffset(currentStackHeight())));
#else
    masm.Push(r);
#endif
    maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    MOZ_ASSERT(stackBefore + StackSizeOfDouble == currentStackHeight());
    return currentStackHeight();
  }

  // Pops the stack into the register `r`. This pops the full 64-bit width on
  // 64-bit systems, and 32-bits otherwise.
  void popGPR(Register r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    masm.loadPtr(Address(sp_, stackOffset(currentStackHeight())), r);
    popChunkyBytes(StackSizeOfPtr);
#else
    masm.Pop(r);
#endif
    MOZ_ASSERT(stackBefore - StackSizeOfPtr == currentStackHeight());
  }

  void popFloat32(FloatRegister r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    masm.loadFloat32(Address(sp_, stackOffset(currentStackHeight())), r);
    popChunkyBytes(StackSizeOfFloat);
#else
    masm.Pop(r);
#endif
    MOZ_ASSERT(stackBefore - StackSizeOfFloat == currentStackHeight());
  }

  void popDouble(FloatRegister r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    masm.loadDouble(Address(sp_, stackOffset(currentStackHeight())), r);
    popChunkyBytes(StackSizeOfDouble);
#else
    masm.Pop(r);
#endif
    MOZ_ASSERT(stackBefore - StackSizeOfDouble == currentStackHeight());
  }

#ifdef ENABLE_WASM_SIMD
  void popV128(RegV128 r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
    masm.loadUnalignedSimd128(Address(sp_, stackOffset(currentStackHeight())),
                              r);
#  ifdef RABALDR_CHUNKY_STACK
    popChunkyBytes(StackSizeOfV128);
#  else
    masm.adjustStack((int)StackSizeOfV128);
#  endif
    MOZ_ASSERT(stackBefore - StackSizeOfV128 == currentStackHeight());
  }
#endif

  void popBytes(size_t bytes) {
    if (bytes > 0) {
#ifdef RABALDR_CHUNKY_STACK
      popChunkyBytes(bytes);
#else
      masm.freeStack(bytes);
#endif
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

  void loadStackRef(int32_t offset, RegRef dest) {
    masm.loadPtr(Address(sp_, stackOffset(offset)), dest);
  }

  void loadStackF64(int32_t offset, RegF64 dest) {
    masm.loadDouble(Address(sp_, stackOffset(offset)), dest);
  }

  void loadStackF32(int32_t offset, RegF32 dest) {
    masm.loadFloat32(Address(sp_, stackOffset(offset)), dest);
  }

#ifdef ENABLE_WASM_SIMD
  void loadStackV128(int32_t offset, RegV128 dest) {
    masm.loadUnalignedSimd128(Address(sp_, stackOffset(offset)), dest);
  }
#endif

  uint32_t prepareStackResultArea(StackHeight stackBase,
                                  uint32_t stackResultBytes) {
    uint32_t end = computeHeightWithStackResults(stackBase, stackResultBytes);
    if (currentStackHeight() < end) {
      uint32_t bytes = end - currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
      pushChunkyBytes(bytes);
#else
      masm.reserveStack(bytes);
#endif
      maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    }
    return end;
  }

  void finishStackResultArea(StackHeight stackBase, uint32_t stackResultBytes) {
    uint32_t end = computeHeightWithStackResults(stackBase, stackResultBytes);
    MOZ_ASSERT(currentStackHeight() >= end);
    popBytes(currentStackHeight() - end);
  }

  // |srcHeight| and |destHeight| are stack heights *including* |bytes|.
  void shuffleStackResultsTowardFP(uint32_t srcHeight, uint32_t destHeight,
                                   uint32_t bytes, Register temp) {
    MOZ_ASSERT(destHeight < srcHeight);
    MOZ_ASSERT(bytes % sizeof(uint32_t) == 0);
    uint32_t destOffset = stackOffset(destHeight) + bytes;
    uint32_t srcOffset = stackOffset(srcHeight) + bytes;
    while (bytes >= sizeof(intptr_t)) {
      destOffset -= sizeof(intptr_t);
      srcOffset -= sizeof(intptr_t);
      bytes -= sizeof(intptr_t);
      masm.loadPtr(Address(sp_, srcOffset), temp);
      masm.storePtr(temp, Address(sp_, destOffset));
    }
    if (bytes) {
      MOZ_ASSERT(bytes == sizeof(uint32_t));
      destOffset -= sizeof(uint32_t);
      srcOffset -= sizeof(uint32_t);
      masm.load32(Address(sp_, srcOffset), temp);
      masm.store32(temp, Address(sp_, destOffset));
    }
  }

  // Unlike the overload that operates on raw heights, |srcHeight| and
  // |destHeight| are stack heights *not including* |bytes|.
  void shuffleStackResultsTowardFP(StackHeight srcHeight,
                                   StackHeight destHeight, uint32_t bytes,
                                   Register temp) {
    MOZ_ASSERT(srcHeight.isValid());
    MOZ_ASSERT(destHeight.isValid());
    uint32_t src = computeHeightWithStackResults(srcHeight, bytes);
    uint32_t dest = computeHeightWithStackResults(destHeight, bytes);
    MOZ_ASSERT(src <= currentStackHeight());
    MOZ_ASSERT(dest <= currentStackHeight());
    shuffleStackResultsTowardFP(src, dest, bytes, temp);
  }

  // |srcHeight| and |destHeight| are stack heights *including* |bytes|.
  void shuffleStackResultsTowardSP(uint32_t srcHeight, uint32_t destHeight,
                                   uint32_t bytes, Register temp) {
    MOZ_ASSERT(destHeight > srcHeight);
    MOZ_ASSERT(bytes % sizeof(uint32_t) == 0);
    uint32_t destOffset = stackOffset(destHeight);
    uint32_t srcOffset = stackOffset(srcHeight);
    while (bytes >= sizeof(intptr_t)) {
      masm.loadPtr(Address(sp_, srcOffset), temp);
      masm.storePtr(temp, Address(sp_, destOffset));
      destOffset += sizeof(intptr_t);
      srcOffset += sizeof(intptr_t);
      bytes -= sizeof(intptr_t);
    }
    if (bytes) {
      MOZ_ASSERT(bytes == sizeof(uint32_t));
      masm.load32(Address(sp_, srcOffset), temp);
      masm.store32(temp, Address(sp_, destOffset));
    }
  }

  // Copy results from the top of the current stack frame to an area of memory,
  // and pop the stack accordingly.  `dest` is the address of the low byte of
  // that memory.
  void popStackResultsToMemory(Register dest, uint32_t bytes, Register temp) {
    MOZ_ASSERT(bytes <= currentStackHeight());
    MOZ_ASSERT(bytes % sizeof(uint32_t) == 0);
    uint32_t bytesToPop = bytes;
    uint32_t srcOffset = stackOffset(currentStackHeight());
    uint32_t destOffset = 0;
    while (bytes >= sizeof(intptr_t)) {
      masm.loadPtr(Address(sp_, srcOffset), temp);
      masm.storePtr(temp, Address(dest, destOffset));
      destOffset += sizeof(intptr_t);
      srcOffset += sizeof(intptr_t);
      bytes -= sizeof(intptr_t);
    }
    if (bytes) {
      MOZ_ASSERT(bytes == sizeof(uint32_t));
      masm.load32(Address(sp_, srcOffset), temp);
      masm.store32(temp, Address(dest, destOffset));
    }
    popBytes(bytesToPop);
  }

 private:
  void store32BitsToStack(int32_t imm, uint32_t destHeight, Register temp) {
    masm.move32(Imm32(imm), temp);
    masm.store32(temp, Address(sp_, stackOffset(destHeight)));
  }

  void store64BitsToStack(int64_t imm, uint32_t destHeight, Register temp) {
#ifdef JS_PUNBOX64
    masm.move64(Imm64(imm), Register64(temp));
    masm.store64(Register64(temp), Address(sp_, stackOffset(destHeight)));
#else
    union {
      int64_t i64;
      int32_t i32[2];
    } bits = {.i64 = imm};
    static_assert(sizeof(bits) == 8);
    store32BitsToStack(bits.i32[0], destHeight, temp);
    store32BitsToStack(bits.i32[1], destHeight - sizeof(int32_t), temp);
#endif
  }

 public:
  void storeImmediatePtrToStack(intptr_t imm, uint32_t destHeight,
                                Register temp) {
#ifdef JS_PUNBOX64
    static_assert(StackSizeOfPtr == 8);
    store64BitsToStack(imm, destHeight, temp);
#else
    static_assert(StackSizeOfPtr == 4);
    store32BitsToStack(int32_t(imm), destHeight, temp);
#endif
  }

  void storeImmediateI64ToStack(int64_t imm, uint32_t destHeight,
                                Register temp) {
    store64BitsToStack(imm, destHeight, temp);
  }

  void storeImmediateF32ToStack(float imm, uint32_t destHeight, Register temp) {
    union {
      float f32;
      int32_t i32;
    } bits{imm};
    static_assert(sizeof(bits) == 4);
    // Do not store 4 bytes if StackSizeOfFloat == 8.  It's probably OK to do
    // so, but it costs little to store something predictable.
    if (StackSizeOfFloat == 4) {
      store32BitsToStack(bits.i32, destHeight, temp);
    } else {
      store64BitsToStack(uint32_t(bits.i32), destHeight, temp);
    }
  }

  void storeImmediateF64ToStack(double imm, uint32_t destHeight,
                                Register temp) {
    union {
      double f64;
      int64_t i64;
    } bits{imm};
    static_assert(sizeof(bits) == 8);
    store64BitsToStack(bits.i64, destHeight, temp);
  }

#ifdef ENABLE_WASM_SIMD
  void storeImmediateV128ToStack(V128 imm, uint32_t destHeight, Register temp) {
    union {
      int32_t i32[4];
      uint8_t bytes[16];
    } bits{};
    static_assert(sizeof(bits) == 16);
    memcpy(bits.bytes, imm.bytes, 16);
    for (unsigned i = 0; i < 4; i++) {
      store32BitsToStack(bits.i32[i], destHeight - i * sizeof(int32_t), temp);
    }
  }
#endif
};

void BaseStackFrame::zeroLocals(BaseRegAlloc* ra) {
  MOZ_ASSERT(varLow_ != UINT32_MAX);

  if (varLow_ == varHigh_) {
    return;
  }

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

  RegI32 zero = ra->needI32();
  masm.mov(ImmWord(0), zero);

  // For the general case we want to have a loop body of UNROLL_LIMIT stores
  // and then a tail of less than UNROLL_LIMIT stores.  When initWords is less
  // than 2*UNROLL_LIMIT the loop trip count is at most 1 and there is no
  // benefit to having the pointer calculations and the compare-and-branch.
  // So we completely unroll when we have initWords < 2 * UNROLL_LIMIT.  (In
  // this case we'll end up using 32-bit offsets on x64 for up to half of the
  // stores, though.)

  // Fully-unrolled case.

  if (initWords < 2 * UNROLL_LIMIT) {
    for (uint32_t i = low; i < high; i += wordSize) {
      masm.storePtr(zero, Address(sp_, localOffset(i + wordSize)));
    }
    ra->freeI32(zero);
    return;
  }

  // Unrolled loop with a tail. Stores will use negative offsets. That's OK
  // for x86 and ARM, at least.

  // Compute pointer to the highest-addressed slot on the frame.
  RegI32 p = ra->needI32();
  masm.computeEffectiveAddress(Address(sp_, localOffset(low + wordSize)), p);

  // Compute pointer to the lowest-addressed slot on the frame that will be
  // initialized by the loop body.
  RegI32 lim = ra->needI32();
  masm.computeEffectiveAddress(Address(sp_, localOffset(loopHigh + wordSize)),
                               lim);

  // The loop body.  Eventually we'll have p == lim and exit the loop.
  Label again;
  masm.bind(&again);
  for (uint32_t i = 0; i < UNROLL_LIMIT; ++i) {
    masm.storePtr(zero, Address(p, -(wordSize * i)));
  }
  masm.subPtr(Imm32(UNROLL_LIMIT * wordSize), p);
  masm.branchPtr(Assembler::LessThan, lim, p, &again);

  // The tail.
  for (uint32_t i = 0; i < tailWords; ++i) {
    masm.storePtr(zero, Address(p, -(wordSize * i)));
  }

  ra->freeI32(p);
  ra->freeI32(lim);
  ra->freeI32(zero);
}

// Value stack: stack elements

struct Stk {
 private:
  Stk() : kind_(Unknown), i64val_(0) {}

 public:
  enum Kind {
    // The Mem opcodes are all clustered at the beginning to
    // allow for a quick test within sync().
    MemI32,  // 32-bit integer stack value ("offs")
    MemI64,  // 64-bit integer stack value ("offs")
    MemF32,  // 32-bit floating stack value ("offs")
    MemF64,  // 64-bit floating stack value ("offs")
#ifdef ENABLE_WASM_SIMD
    MemV128,  // 128-bit vector stack value ("offs")
#endif
    MemRef,  // reftype (pointer wide) stack value ("offs")

    // The Local opcodes follow the Mem opcodes for a similar
    // quick test within hasLocal().
    LocalI32,  // Local int32 var ("slot")
    LocalI64,  // Local int64 var ("slot")
    LocalF32,  // Local float32 var ("slot")
    LocalF64,  // Local double var ("slot")
#ifdef ENABLE_WASM_SIMD
    LocalV128,  // Local v128 var ("slot")
#endif
    LocalRef,  // Local reftype (pointer wide) var ("slot")

    RegisterI32,  // 32-bit integer register ("i32reg")
    RegisterI64,  // 64-bit integer register ("i64reg")
    RegisterF32,  // 32-bit floating register ("f32reg")
    RegisterF64,  // 64-bit floating register ("f64reg")
#ifdef ENABLE_WASM_SIMD
    RegisterV128,  // 128-bit vector register ("v128reg")
#endif
    RegisterRef,  // reftype (pointer wide) register ("refReg")

    ConstI32,  // 32-bit integer constant ("i32val")
    ConstI64,  // 64-bit integer constant ("i64val")
    ConstF32,  // 32-bit floating constant ("f32val")
    ConstF64,  // 64-bit floating constant ("f64val")
#ifdef ENABLE_WASM_SIMD
    ConstV128,  // 128-bit vector constant ("v128val")
#endif
    ConstRef,  // reftype (pointer wide) constant ("refval")

    Unknown,
  };

  Kind kind_;

  static const Kind MemLast = MemRef;
  static const Kind LocalLast = LocalRef;

  union {
    RegI32 i32reg_;
    RegI64 i64reg_;
    RegRef refReg_;
    RegF32 f32reg_;
    RegF64 f64reg_;
#ifdef ENABLE_WASM_SIMD
    RegV128 v128reg_;
#endif
    int32_t i32val_;
    int64_t i64val_;
    intptr_t refval_;
    float f32val_;
    double f64val_;
#ifdef ENABLE_WASM_SIMD
    V128 v128val_;
#endif
    uint32_t slot_;
    uint32_t offs_;
  };

  explicit Stk(RegI32 r) : kind_(RegisterI32), i32reg_(r) {}
  explicit Stk(RegI64 r) : kind_(RegisterI64), i64reg_(r) {}
  explicit Stk(RegRef r) : kind_(RegisterRef), refReg_(r) {}
  explicit Stk(RegF32 r) : kind_(RegisterF32), f32reg_(r) {}
  explicit Stk(RegF64 r) : kind_(RegisterF64), f64reg_(r) {}
#ifdef ENABLE_WASM_SIMD
  explicit Stk(RegV128 r) : kind_(RegisterV128), v128reg_(r) {}
#endif
  explicit Stk(int32_t v) : kind_(ConstI32), i32val_(v) {}
  explicit Stk(uint32_t v) : kind_(ConstI32), i32val_(int32_t(v)) {}
  explicit Stk(int64_t v) : kind_(ConstI64), i64val_(v) {}
  explicit Stk(float v) : kind_(ConstF32), f32val_(v) {}
  explicit Stk(double v) : kind_(ConstF64), f64val_(v) {}
#ifdef ENABLE_WASM_SIMD
  explicit Stk(V128 v) : kind_(ConstV128), v128val_(v) {}
#endif
  explicit Stk(Kind k, uint32_t v) : kind_(k), slot_(v) {
    MOZ_ASSERT(k > MemLast && k <= LocalLast);
  }
  static Stk StkRef(intptr_t v) {
    Stk s;
    s.kind_ = ConstRef;
    s.refval_ = v;
    return s;
  }
  static Stk StackResult(ValType type, uint32_t offs) {
    Kind k;
    switch (type.kind()) {
      case ValType::I32:
        k = Stk::MemI32;
        break;
      case ValType::I64:
        k = Stk::MemI64;
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        k = Stk::MemV128;
        break;
#else
        MOZ_CRASH("No SIMD");
#endif
      case ValType::F32:
        k = Stk::MemF32;
        break;
      case ValType::F64:
        k = Stk::MemF64;
        break;
      case ValType::Rtt:
      case ValType::Ref:
        k = Stk::MemRef;
        break;
    }
    Stk s;
    s.setOffs(k, offs);
    return s;
  }

  void setOffs(Kind k, uint32_t v) {
    MOZ_ASSERT(k <= MemLast);
    kind_ = k;
    offs_ = v;
  }

  Kind kind() const { return kind_; }
  bool isMem() const { return kind_ <= MemLast; }

  RegI32 i32reg() const {
    MOZ_ASSERT(kind_ == RegisterI32);
    return i32reg_;
  }
  RegI64 i64reg() const {
    MOZ_ASSERT(kind_ == RegisterI64);
    return i64reg_;
  }
  RegRef refReg() const {
    MOZ_ASSERT(kind_ == RegisterRef);
    return refReg_;
  }
  RegF32 f32reg() const {
    MOZ_ASSERT(kind_ == RegisterF32);
    return f32reg_;
  }
  RegF64 f64reg() const {
    MOZ_ASSERT(kind_ == RegisterF64);
    return f64reg_;
  }
#ifdef ENABLE_WASM_SIMD
  RegV128 v128reg() const {
    MOZ_ASSERT(kind_ == RegisterV128);
    return v128reg_;
  }
#endif
  int32_t i32val() const {
    MOZ_ASSERT(kind_ == ConstI32);
    return i32val_;
  }
  int64_t i64val() const {
    MOZ_ASSERT(kind_ == ConstI64);
    return i64val_;
  }
  intptr_t refval() const {
    MOZ_ASSERT(kind_ == ConstRef);
    return refval_;
  }

  // For these two, use an out-param instead of simply returning, to
  // use the normal stack and not the x87 FP stack (which has effect on
  // NaNs with the signaling bit set).

  void f32val(float* out) const {
    MOZ_ASSERT(kind_ == ConstF32);
    *out = f32val_;
  }
  void f64val(double* out) const {
    MOZ_ASSERT(kind_ == ConstF64);
    *out = f64val_;
  }

#ifdef ENABLE_WASM_SIMD
  // For SIMD, do the same as for floats since we're using float registers to
  // hold vectors; this is just conservative.
  void v128val(V128* out) const {
    MOZ_ASSERT(kind_ == ConstV128);
    *out = v128val_;
  }
#endif

  uint32_t slot() const {
    MOZ_ASSERT(kind_ > MemLast && kind_ <= LocalLast);
    return slot_;
  }
  uint32_t offs() const {
    MOZ_ASSERT(isMem());
    return offs_;
  }
};

using StkVector = Vector<Stk, 0, SystemAllocPolicy>;

// MachineStackTracker, used for stack-slot pointerness tracking.

class MachineStackTracker {
  // Simulates the machine's stack, with one bool per word.  Index zero in
  // this vector corresponds to the highest address in the machine stack.  The
  // last entry corresponds to what SP currently points at.  This all assumes
  // a grow-down stack.
  //
  // numPtrs_ contains the number of "true" values in vec_, and is therefore
  // redundant.  But it serves as a constant-time way to detect the common
  // case where vec_ holds no "true" values.
  size_t numPtrs_;
  Vector<bool, 64, SystemAllocPolicy> vec_;

 public:
  MachineStackTracker() : numPtrs_(0) {}

  ~MachineStackTracker() {
#ifdef DEBUG
    size_t n = 0;
    for (bool b : vec_) {
      n += (b ? 1 : 0);
    }
    MOZ_ASSERT(n == numPtrs_);
#endif
  }

  // Clone this MachineStackTracker, writing the result at |dst|.
  [[nodiscard]] bool cloneTo(MachineStackTracker* dst) {
    MOZ_ASSERT(dst->vec_.empty());
    if (!dst->vec_.appendAll(vec_)) {
      return false;
    }
    dst->numPtrs_ = numPtrs_;
    return true;
  }

  // Notionally push |n| non-pointers on the stack.
  [[nodiscard]] bool pushNonGCPointers(size_t n) {
    return vec_.appendN(false, n);
  }

  // Mark the stack slot |offsetFromSP| up from the bottom as holding a
  // pointer.
  void setGCPointer(size_t offsetFromSP) {
    // offsetFromSP == 0 denotes the most recently pushed item, == 1 the
    // second most recently pushed item, etc.
    MOZ_ASSERT(offsetFromSP < vec_.length());

    size_t offsetFromTop = vec_.length() - 1 - offsetFromSP;
    numPtrs_ = numPtrs_ + 1 - (vec_[offsetFromTop] ? 1 : 0);
    vec_[offsetFromTop] = true;
  }

  // Query the pointerness of the slot |offsetFromSP| up from the bottom.
  bool isGCPointer(size_t offsetFromSP) {
    MOZ_ASSERT(offsetFromSP < vec_.length());

    size_t offsetFromTop = vec_.length() - 1 - offsetFromSP;
    return vec_[offsetFromTop];
  }

  // Return the number of words tracked by this MachineStackTracker.
  size_t length() { return vec_.length(); }

  // Return the number of pointer-typed words tracked by this
  // MachineStackTracker.
  size_t numPtrs() {
    MOZ_ASSERT(numPtrs_ <= length());
    return numPtrs_;
  }

  // Discard all contents, but (per mozilla::Vector::clear semantics) don't
  // free or reallocate any dynamic storage associated with |vec_|.
  void clear() {
    vec_.clear();
    numPtrs_ = 0;
  }
};

// StackMapGenerator, which carries all state needed to create stackmaps.

enum class HasDebugFrame { No, Yes };

struct StackMapGenerator {
 private:
  // --- These are constant for the life of the function's compilation ---

  // For generating stackmaps, we'll need to know the offsets of registers
  // as saved by the trap exit stub.
  const MachineState& trapExitLayout_;
  const size_t trapExitLayoutNumWords_;

  // Completed stackmaps are added here
  StackMaps* stackMaps_;

  // So as to be able to get current offset when creating stackmaps
  const MacroAssembler& masm_;

 public:
  // --- These are constant once we've completed beginFunction() ---

  // The number of words of arguments passed to this function in memory.
  size_t numStackArgWords;

  MachineStackTracker machineStackTracker;  // tracks machine stack pointerness

  // This holds masm.framePushed at entry to the function's body.  It is a
  // Maybe because createStackMap needs to know whether or not we're still
  // in the prologue.  It makes a Nothing-to-Some transition just once per
  // function.
  Maybe<uint32_t> framePushedAtEntryToBody;

  // --- These can change at any point ---

  // This holds masm.framePushed at it would be be for a function call
  // instruction, but excluding the stack area used to pass arguments in
  // memory.  That is, for an upcoming function call, this will hold
  //
  //   masm.framePushed() at the call instruction -
  //      StackArgAreaSizeUnaligned(argumentTypes)
  //
  // This value denotes the lowest-addressed stack word covered by the current
  // function's stackmap.  Words below this point form the highest-addressed
  // area of the callee's stackmap.  Note that all alignment padding above the
  // arguments-in-memory themselves belongs to the caller's stackmap, which
  // is why this is defined in terms of StackArgAreaSizeUnaligned() rather than
  // StackArgAreaSizeAligned().
  //
  // When not inside a function call setup/teardown sequence, it is Nothing.
  // It can make Nothing-to/from-Some transitions arbitrarily as we progress
  // through the function body.
  Maybe<uint32_t> framePushedExcludingOutboundCallArgs;

  // The number of memory-resident, ref-typed entries on the containing
  // BaseCompiler::stk_.
  size_t memRefsOnStk;

  // This is a copy of machineStackTracker that is used only within individual
  // calls to createStackMap. It is here only to avoid possible heap allocation
  // costs resulting from making it local to createStackMap().
  MachineStackTracker augmentedMst;

  StackMapGenerator(StackMaps* stackMaps, const MachineState& trapExitLayout,
                    const size_t trapExitLayoutNumWords,
                    const MacroAssembler& masm)
      : trapExitLayout_(trapExitLayout),
        trapExitLayoutNumWords_(trapExitLayoutNumWords),
        stackMaps_(stackMaps),
        masm_(masm),
        numStackArgWords(0),
        memRefsOnStk(0) {}

  // At the beginning of a function, we may have live roots in registers (as
  // arguments) at the point where we perform a stack overflow check.  This
  // method generates the "extra" stackmap entries to describe that, in the
  // case that the check fails and we wind up calling into the wasm exit
  // stub, as generated by GenerateTrapExit().
  //
  // The resulting map must correspond precisely with the stack layout
  // created for the integer registers as saved by (code generated by)
  // GenerateTrapExit().  To do that we use trapExitLayout_ and
  // trapExitLayoutNumWords_, which together comprise a description of the
  // layout and are created by GenerateTrapExitMachineState().
  [[nodiscard]] bool generateStackmapEntriesForTrapExit(
      const ArgTypeVector& args, ExitStubMapVector* extras) {
    return GenerateStackmapEntriesForTrapExit(args, trapExitLayout_,
                                              trapExitLayoutNumWords_, extras);
  }

  // Creates a stackmap associated with the instruction denoted by
  // |assemblerOffset|, incorporating pointers from the current operand
  // stack |stk|, incorporating possible extra pointers in |extra| at the
  // lower addressed end, and possibly with the associated frame having a
  // DebugFrame as indicated by |debugFrame|.
  [[nodiscard]] bool createStackMap(const char* who,
                                    const ExitStubMapVector& extras,
                                    uint32_t assemblerOffset,
                                    HasDebugFrame debugFrame,
                                    const StkVector& stk) {
    size_t countedPointers = machineStackTracker.numPtrs() + memRefsOnStk;
#ifndef DEBUG
    // An important optimization.  If there are obviously no pointers, as
    // we expect in the majority of cases, exit quickly.
    if (countedPointers == 0 && debugFrame == HasDebugFrame::No) {
      // We can skip creating the map if there are no |true| elements in
      // |extras|.
      bool extrasHasRef = false;
      for (bool b : extras) {
        if (b) {
          extrasHasRef = true;
          break;
        }
      }
      if (!extrasHasRef) {
        return true;
      }
    }
#else
    // In the debug case, create the stackmap regardless, and cross-check
    // the pointer-counting below.  We expect the final map to have
    // |countedPointers| in total.  This doesn't include those in the
    // DebugFrame, but they do not appear in the map's bitmap.  Note that
    // |countedPointers| is debug-only from this point onwards.
    for (bool b : extras) {
      countedPointers += (b ? 1 : 0);
    }
#endif

    // Start with the frame-setup map, and add operand-stack information to
    // that.  augmentedMst holds live data only within individual calls to
    // createStackMap.
    augmentedMst.clear();
    if (!machineStackTracker.cloneTo(&augmentedMst)) {
      return false;
    }

    // At this point, augmentedMst only contains entries covering the
    // incoming argument area (if any) and for the area allocated by this
    // function's prologue.  We now need to calculate how far the machine's
    // stack pointer is below where it was at the start of the body.  But we
    // must take care not to include any words pushed as arguments to an
    // upcoming function call, since those words "belong" to the stackmap of
    // the callee, not to the stackmap of this function.  Note however that
    // any alignment padding pushed prior to pushing the args *does* belong to
    // this function.
    //
    // That padding is taken into account at the point where
    // framePushedExcludingOutboundCallArgs is set, viz, in startCallArgs(),
    // and comprises two components:
    //
    // * call->frameAlignAdjustment
    // * the padding applied to the stack arg area itself.  That is:
    //   StackArgAreaSize(argTys) - StackArgAreaSizeUnpadded(argTys)
    Maybe<uint32_t> framePushedExcludingArgs;
    if (framePushedAtEntryToBody.isNothing()) {
      // Still in the prologue.  framePushedExcludingArgs remains Nothing.
      MOZ_ASSERT(framePushedExcludingOutboundCallArgs.isNothing());
    } else {
      // In the body.
      MOZ_ASSERT(masm_.framePushed() >= framePushedAtEntryToBody.value());
      if (framePushedExcludingOutboundCallArgs.isSome()) {
        // In the body, and we've potentially pushed some args onto the stack.
        // We must ignore them when sizing the stackmap.
        MOZ_ASSERT(masm_.framePushed() >=
                   framePushedExcludingOutboundCallArgs.value());
        MOZ_ASSERT(framePushedExcludingOutboundCallArgs.value() >=
                   framePushedAtEntryToBody.value());
        framePushedExcludingArgs =
            Some(framePushedExcludingOutboundCallArgs.value());
      } else {
        // In the body, but not with call args on the stack.  The stackmap
        // must be sized so as to extend all the way "down" to
        // masm_.framePushed().
        framePushedExcludingArgs = Some(masm_.framePushed());
      }
    }

    if (framePushedExcludingArgs.isSome()) {
      uint32_t bodyPushedBytes =
          framePushedExcludingArgs.value() - framePushedAtEntryToBody.value();
      MOZ_ASSERT(0 == bodyPushedBytes % sizeof(void*));
      if (!augmentedMst.pushNonGCPointers(bodyPushedBytes / sizeof(void*))) {
        return false;
      }
    }

    // Scan the operand stack, marking pointers in the just-added new
    // section.
    MOZ_ASSERT_IF(framePushedAtEntryToBody.isNothing(), stk.empty());
    MOZ_ASSERT_IF(framePushedExcludingArgs.isNothing(), stk.empty());

    for (const Stk& v : stk) {
#ifndef DEBUG
      // We don't track roots in registers, per rationale below, so if this
      // doesn't hold, something is seriously wrong, and we're likely to get a
      // GC-related crash.
      MOZ_RELEASE_ASSERT(v.kind() != Stk::RegisterRef);
      if (v.kind() != Stk::MemRef) {
        continue;
      }
#else
      // Take the opportunity to check everything we reasonably can about
      // operand stack elements.
      switch (v.kind()) {
        case Stk::MemI32:
        case Stk::MemI64:
        case Stk::MemF32:
        case Stk::MemF64:
        case Stk::ConstI32:
        case Stk::ConstI64:
        case Stk::ConstF32:
        case Stk::ConstF64:
#  ifdef ENABLE_WASM_SIMD
        case Stk::MemV128:
        case Stk::ConstV128:
#  endif
          // All of these have uninteresting type.
          continue;
        case Stk::LocalI32:
        case Stk::LocalI64:
        case Stk::LocalF32:
        case Stk::LocalF64:
#  ifdef ENABLE_WASM_SIMD
        case Stk::LocalV128:
#  endif
          // These also have uninteresting type.  Check that they live in the
          // section of stack set up by beginFunction().  The unguarded use of
          // |value()| here is safe due to the assertion above this loop.
          MOZ_ASSERT(v.offs() <= framePushedAtEntryToBody.value());
          continue;
        case Stk::RegisterI32:
        case Stk::RegisterI64:
        case Stk::RegisterF32:
        case Stk::RegisterF64:
#  ifdef ENABLE_WASM_SIMD
        case Stk::RegisterV128:
#  endif
          // These also have uninteresting type, but more to the point: all
          // registers holding live values should have been flushed to the
          // machine stack immediately prior to the instruction to which this
          // stackmap pertains.  So these can't happen.
          MOZ_CRASH("createStackMap: operand stack has Register-non-Ref");
        case Stk::MemRef:
          // This is the only case we care about.  We'll handle it after the
          // switch.
          break;
        case Stk::LocalRef:
          // We need the stackmap to mention this pointer, but it should
          // already be in the machineStackTracker section created by
          // beginFunction().
          MOZ_ASSERT(v.offs() <= framePushedAtEntryToBody.value());
          continue;
        case Stk::ConstRef:
          // This can currently only be a null pointer.
          MOZ_ASSERT(v.refval() == 0);
          continue;
        case Stk::RegisterRef:
          // This can't happen, per rationale above.
          MOZ_CRASH("createStackMap: operand stack contains RegisterRef");
        default:
          MOZ_CRASH("createStackMap: unknown operand stack element");
      }
#endif
      // v.offs() holds masm.framePushed() at the point immediately after it
      // was pushed on the stack.  Since it's still on the stack,
      // masm.framePushed() can't be less.
      MOZ_ASSERT(v.offs() <= framePushedExcludingArgs.value());
      uint32_t offsFromMapLowest = framePushedExcludingArgs.value() - v.offs();
      MOZ_ASSERT(0 == offsFromMapLowest % sizeof(void*));
      augmentedMst.setGCPointer(offsFromMapLowest / sizeof(void*));
    }

    // Create the final StackMap.  The initial map is zeroed out, so there's
    // no need to write zero bits in it.
    const uint32_t extraWords = extras.length();
    const uint32_t augmentedMstWords = augmentedMst.length();
    const uint32_t numMappedWords = extraWords + augmentedMstWords;
    StackMap* stackMap = StackMap::create(numMappedWords);
    if (!stackMap) {
      return false;
    }

    {
      // First the exit stub extra words, if any.
      uint32_t i = 0;
      for (bool b : extras) {
        if (b) {
          stackMap->setBit(i);
        }
        i++;
      }
    }
    // Followed by the "main" part of the map.
    for (uint32_t i = 0; i < augmentedMstWords; i++) {
      if (augmentedMst.isGCPointer(i)) {
        stackMap->setBit(extraWords + i);
      }
    }

    stackMap->setExitStubWords(extraWords);

    // Record in the map, how far down from the highest address the Frame* is.
    // Take the opportunity to check that we haven't marked any part of the
    // Frame itself as a pointer.
    stackMap->setFrameOffsetFromTop(numStackArgWords +
                                    sizeof(Frame) / sizeof(void*));
#ifdef DEBUG
    for (uint32_t i = 0; i < sizeof(Frame) / sizeof(void*); i++) {
      MOZ_ASSERT(stackMap->getBit(stackMap->numMappedWords -
                                  stackMap->frameOffsetFromTop + i) == 0);
    }
#endif

    // Note the presence of a DebugFrame, if any.
    if (debugFrame == HasDebugFrame::Yes) {
      stackMap->setHasDebugFrame();
    }

    // Add the completed map to the running collection thereof.
    if (!stackMaps_->add((uint8_t*)(uintptr_t)assemblerOffset, stackMap)) {
      stackMap->destroy();
      return false;
    }

#ifdef DEBUG
    {
      // Crosscheck the map pointer counting.
      uint32_t nw = stackMap->numMappedWords;
      uint32_t np = 0;
      for (uint32_t i = 0; i < nw; i++) {
        np += stackMap->getBit(i);
      }
      MOZ_ASSERT(size_t(np) == countedPointers);
    }
#endif

    return true;
  }
};

class BaseCompiler;

static void ClzI64(BaseCompiler& bc, RegI64 rsd);
static void CtzI64(BaseCompiler& bc, RegI64 rsd);
static RegI32 PopcntTemp(BaseCompiler& bc);
static void PopcntI32(BaseCompiler& bc, RegI32 rsd, RegI32 temp);
static void PopcntI64(BaseCompiler& bc, RegI64 rsd, RegI32 temp);
static void ShlI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd);
static void ShrI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd);
static void ShrUI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd);
static void MinF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd);
static void MaxF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd);
static void MinF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd);
static void MaxF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd);
static void ExtendI32_8(BaseCompiler& bc, RegI32 rsd);
#ifdef ENABLE_WASM_SIMD
static RegV128 BitselectV128(BaseCompiler& bc, RegV128 rs1, RegV128 rs2,
                             RegV128 rs3);
#endif

// The baseline compiler proper.

class BaseCompiler final : public BaseCompilerInterface {
  friend void ClzI64(BaseCompiler& bc, RegI64 rsd);
  friend void CtzI64(BaseCompiler& bc, RegI64 rsd);
  friend RegI32 PopcntTemp(BaseCompiler& bc);
  friend void PopcntI32(BaseCompiler& bc, RegI32 rsd, RegI32 temp);
  friend void PopcntI64(BaseCompiler& bc, RegI64 rsd, RegI32 temp);
  friend void ShlI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd);
  friend void ShrI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd);
  friend void ShrUI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd);
  friend void MinF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd);
  friend void MaxF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd);
  friend void MinF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd);
  friend void MaxF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd);
  friend void ExtendI32_8(BaseCompiler& bc, RegI32 rsd);
#ifdef ENABLE_WASM_SIMD
  friend RegV128 BitselectV128(BaseCompiler& bc, RegV128 rs1, RegV128 rs2,
                               RegV128 rs3);
#endif

  using Local = BaseStackFrame::Local;
  using LabelVector = Vector<NonAssertingLabel, 8, SystemAllocPolicy>;

  // Bit set used for simple bounds check elimination.  Capping this at 64
  // locals makes sense; even 32 locals would probably be OK in practice.
  //
  // For more information about BCE, see the block comment above
  // popMemory32Access(), below.

  using BCESet = uint64_t;

  // Information stored in the control node for generating exception handling
  // landing pads.

  struct CatchInfo {
    uint32_t eventIndex;      // Index for the associated exception.
    NonAssertingLabel label;  // The entry label for the handler.

    static const uint32_t CATCH_ALL_INDEX = UINT32_MAX;
    static_assert(CATCH_ALL_INDEX > MaxEvents);

    explicit CatchInfo(uint32_t eventIndex_) : eventIndex(eventIndex_) {}
  };

  using CatchInfoVector = Vector<CatchInfo, 0, SystemAllocPolicy>;

  // Control node, representing labels and stack heights at join points.

  struct Control {
    NonAssertingLabel label;       // The "exit" label
    NonAssertingLabel otherLabel;  // Used for the "else" branch of if-then-else
                                   // and to allow delegate to jump to catches.
    StackHeight stackHeight;       // From BaseStackFrame
    uint32_t stackSize;            // Value stack height
    BCESet bceSafeOnEntry;         // Bounds check info flowing into the item
    BCESet bceSafeOnExit;          // Bounds check info flowing out of the item
    bool deadOnArrival;            // deadCode_ was set on entry to the region
    bool deadThenBranch;           // deadCode_ was set on exit from "then"
    size_t tryNoteIndex;           // For tracking try branch code ranges.
    CatchInfoVector catchInfos;    // Used for try-catch handlers.

    Control()
        : stackHeight(StackHeight::Invalid()),
          stackSize(UINT32_MAX),
          bceSafeOnEntry(0),
          bceSafeOnExit(~BCESet(0)),
          deadOnArrival(false),
          deadThenBranch(false),
          tryNoteIndex(0) {}
  };

  class NothingVector {
    Nothing unused_;

   public:
    bool resize(size_t length) { return true; }
    Nothing& operator[](size_t) { return unused_; }
    Nothing& back() { return unused_; }
  };

  struct BaseCompilePolicy {
    // The baseline compiler tracks values on a stack of its own -- it
    // needs to scan that stack for spilling -- and thus has no need
    // for the values maintained by the iterator.
    using Value = Nothing;
    using ValueVector = NothingVector;

    // The baseline compiler uses the iterator's control stack, attaching
    // its own control information.
    using ControlItem = Control;
  };

  using BaseOpIter = OpIter<BaseCompilePolicy>;

  // The baseline compiler will use OOL code more sparingly than
  // Baldr since our code is not high performance and frills like
  // code density and branch prediction friendliness will be less
  // important.

  class OutOfLineCode : public TempObject {
   private:
    NonAssertingLabel entry_;
    NonAssertingLabel rejoin_;
    StackHeight stackHeight_;

   public:
    OutOfLineCode() : stackHeight_(StackHeight::Invalid()) {}

    Label* entry() { return &entry_; }
    Label* rejoin() { return &rejoin_; }

    void setStackHeight(StackHeight stackHeight) {
      MOZ_ASSERT(!stackHeight_.isValid());
      stackHeight_ = stackHeight;
    }

    void bind(BaseStackFrame* fr, MacroAssembler* masm) {
      MOZ_ASSERT(stackHeight_.isValid());
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

  enum class LatentOp { None, Compare, Eqz };

  struct AccessCheck {
    AccessCheck()
        : omitBoundsCheck(false),
          omitAlignmentCheck(false),
          onlyPointerAlignment(false) {}

    // If `omitAlignmentCheck` is true then we need check neither the
    // pointer nor the offset.  Otherwise, if `onlyPointerAlignment` is true
    // then we need check only the pointer.  Otherwise, check the sum of
    // pointer and offset.

    bool omitBoundsCheck;
    bool omitAlignmentCheck;
    bool onlyPointerAlignment;
  };

  const ModuleEnvironment& moduleEnv_;
  const CompilerEnvironment& compilerEnv_;
  BaseOpIter iter_;
  const FuncCompileInput& func_;
  size_t lastReadCallSite_;
  TempAllocator::Fallible alloc_;
  const ValTypeVector& locals_;  // Types of parameters and locals
  bool deadCode_;  // Flag indicating we should decode & discard the opcode
  BCESet
      bceSafe_;  // Locals that have been bounds checked and not updated since
  ValTypeVector SigD_;
  ValTypeVector SigF_;
  NonAssertingLabel returnLabel_;

  LatentOp latentOp_;   // Latent operation for branch (seen next)
  ValType latentType_;  // Operand type, if latentOp_ is true
  Assembler::Condition
      latentIntCmp_;  // Comparison operator, if latentOp_ == Compare, int types
  Assembler::DoubleCondition
      latentDoubleCmp_;  // Comparison operator, if latentOp_ == Compare, float
                         // types

  FuncOffsets offsets_;
  MacroAssembler& masm;  // No '_' suffix - too tedious...
  BaseRegAlloc ra;       // Ditto
  BaseStackFrame fr;

  StackMapGenerator stackMapGenerator_;

  BaseStackFrame::LocalVector localInfo_;
  Vector<OutOfLineCode*, 8, SystemAllocPolicy> outOfLine_;

  // On specific platforms we sometimes need to use specific registers.

  SpecificRegs specific_;

  // There are more members scattered throughout.

 public:
  BaseCompiler(const ModuleEnvironment& moduleEnv,
               const CompilerEnvironment& compilerEnv,
               const FuncCompileInput& func, const ValTypeVector& locals,
               const MachineState& trapExitLayout,
               size_t trapExitLayoutNumWords, Decoder& decoder,
               StkVector& stkSource, TempAllocator* alloc, MacroAssembler* masm,
               StackMaps* stackMaps);
  ~BaseCompiler();

  [[nodiscard]] bool init();

  FuncOffsets finish();

  [[nodiscard]] bool emitFunction();
  void emitInitStackLocals();

  const FuncType& funcType() const {
    return *moduleEnv_.funcs[func_.index].type;
  }

  const TypeIdDesc& funcTypeId() const {
    return *moduleEnv_.funcs[func_.index].typeId;
  }

  // Used by some of the ScratchRegister implementations.
  operator MacroAssembler&() const { return masm; }
  operator BaseRegAlloc&() { return ra; }

  bool usesSharedMemory() const { return moduleEnv_.usesSharedMemory(); }

 private:
  ////////////////////////////////////////////////////////////
  //
  // Out of line code management.

  [[nodiscard]] OutOfLineCode* addOutOfLineCode(OutOfLineCode* ool) {
    if (!ool || !outOfLine_.append(ool)) {
      return nullptr;
    }
    ool->setStackHeight(fr.stackHeight());
    return ool;
  }

  [[nodiscard]] bool generateOutOfLineCode() {
    for (auto* ool : outOfLine_) {
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
  bool isAvailableRef(RegRef r) { return ra.isAvailableRef(r); }
  bool isAvailablePtr(RegPtr r) { return ra.isAvailablePtr(r); }
  bool isAvailableF32(RegF32 r) { return ra.isAvailableF32(r); }
  bool isAvailableF64(RegF64 r) { return ra.isAvailableF64(r); }
#ifdef ENABLE_WASM_SIMD
  bool isAvailableV128(RegV128 r) { return ra.isAvailableV128(r); }
#endif

  [[nodiscard]] RegI32 needI32() { return ra.needI32(); }
  [[nodiscard]] RegI64 needI64() { return ra.needI64(); }
  [[nodiscard]] RegRef needRef() { return ra.needRef(); }
  [[nodiscard]] RegPtr needPtr() { return ra.needPtr(); }
  [[nodiscard]] RegF32 needF32() { return ra.needF32(); }
  [[nodiscard]] RegF64 needF64() { return ra.needF64(); }
#ifdef ENABLE_WASM_SIMD
  [[nodiscard]] RegV128 needV128() { return ra.needV128(); }
#endif

  void needI32(RegI32 specific) { ra.needI32(specific); }
  void needI64(RegI64 specific) { ra.needI64(specific); }
  void needRef(RegRef specific) { ra.needRef(specific); }
  void needPtr(RegPtr specific) { ra.needPtr(specific); }
  void needF32(RegF32 specific) { ra.needF32(specific); }
  void needF64(RegF64 specific) { ra.needF64(specific); }
#ifdef ENABLE_WASM_SIMD
  void needV128(RegV128 specific) { ra.needV128(specific); }
#endif

#if defined(JS_CODEGEN_ARM)
  [[nodiscard]] RegI64 needI64Pair() { return ra.needI64Pair(); }
#endif

  void freeAny(AnyReg r) {
    switch (r.tag) {
      case AnyReg::I32:
        freeI32(r.i32());
        break;
      case AnyReg::I64:
        freeI64(r.i64());
        break;
      case AnyReg::REF:
        freeRef(r.ref());
        break;
      case AnyReg::F32:
        freeF32(r.f32());
        break;
      case AnyReg::F64:
        freeF64(r.f64());
        break;
#ifdef ENABLE_WASM_SIMD
      case AnyReg::V128:
        freeV128(r.v128());
        break;
#endif
      default:
        MOZ_CRASH();
    }
  }
  void freeI32(RegI32 r) { ra.freeI32(r); }
  void freeI64(RegI64 r) { ra.freeI64(r); }
  void freeRef(RegRef r) { ra.freeRef(r); }
  void freePtr(RegPtr r) { ra.freePtr(r); }
  void freeF32(RegF32 r) { ra.freeF32(r); }
  void freeF64(RegF64 r) { ra.freeF64(r); }
#ifdef ENABLE_WASM_SIMD
  void freeV128(RegV128 r) { ra.freeV128(r); }
#endif

  void freeI64Except(RegI64 r, RegI32 except) {
#ifdef JS_PUNBOX64
    MOZ_ASSERT(r.reg == except);
#else
    MOZ_ASSERT(r.high == except || r.low == except);
    freeI64(r);
    needI32(except);
#endif
  }

  void maybeFree(RegI32 r) {
    if (r.isValid()) {
      freeI32(r);
    }
  }

  void maybeFree(RegI64 r) {
    if (r.isValid()) {
      freeI64(r);
    }
  }

  void maybeFree(RegF64 r) {
    if (r.isValid()) {
      freeF64(r);
    }
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

  RegI32 fromI64(RegI64 r) { return RegI32(lowPart(r)); }

#ifdef JS_PUNBOX64
  RegI64 fromI32(RegI32 r) { return RegI64(Register64(r)); }
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

  RegI32 narrowRef(RegRef r) { return RegI32(r); }

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
    if (src != dest) {
      masm.move32(src, dest);
    }
  }

  void moveI64(RegI64 src, RegI64 dest) {
    if (src != dest) {
      masm.move64(src, dest);
    }
  }

  void moveRef(RegRef src, RegRef dest) {
    if (src != dest) {
      masm.movePtr(src, dest);
    }
  }

  void moveF64(RegF64 src, RegF64 dest) {
    if (src != dest) {
      masm.moveDouble(src, dest);
    }
  }

  void moveF32(RegF32 src, RegF32 dest) {
    if (src != dest) {
      masm.moveFloat32(src, dest);
    }
  }

#ifdef ENABLE_WASM_SIMD
  void moveV128(RegV128 src, RegV128 dest) {
    if (src != dest) {
      masm.moveSimd128(src, dest);
    }
  }
#endif

  ////////////////////////////////////////////////////////////////////////////
  //
  // Block parameters and results.
  //
  // Blocks may have multiple parameters and multiple results.  Blocks can also
  // be the target of branches: the entry for loops, and the exit for
  // non-loops.
  //
  // Passing multiple values to a non-branch target (i.e., the entry of a
  // "block") falls out naturally: any items on the value stack can flow
  // directly from one block to another.
  //
  // However, for branch targets, we need to allocate well-known locations for
  // the branch values.  The approach taken in the baseline compiler is to
  // allocate registers to the top N values (currently N=1), and then stack
  // locations for the rest.
  //

  enum class RegKind { All, OnlyGPRs };

  inline void needResultRegisters(ResultType type, RegKind which) {
    if (type.empty()) {
      return;
    }

    for (ABIResultIter iter(type); !iter.done(); iter.next()) {
      ABIResult result = iter.cur();
      // Register results are visited first; when we see a stack result we're
      // done.
      if (!result.inRegister()) {
        return;
      }
      switch (result.type().kind()) {
        case ValType::I32:
          needI32(RegI32(result.gpr()));
          break;
        case ValType::I64:
          needI64(RegI64(result.gpr64()));
          break;
        case ValType::V128:
#ifdef ENABLE_WASM_SIMD
          if (which == RegKind::All) {
            needV128(RegV128(result.fpr()));
          }
          break;
#else
          MOZ_CRASH("No SIMD support");
#endif
        case ValType::F32:
          if (which == RegKind::All) {
            needF32(RegF32(result.fpr()));
          }
          break;
        case ValType::F64:
          if (which == RegKind::All) {
            needF64(RegF64(result.fpr()));
          }
          break;
        case ValType::Rtt:
        case ValType::Ref:
          needRef(RegRef(result.gpr()));
          break;
      }
    }
  }

#ifdef JS_CODEGEN_X64
  inline void maskResultRegisters(ResultType type) {
    MOZ_ASSERT(JitOptions.spectreIndexMasking);

    if (type.empty()) {
      return;
    }

    for (ABIResultIter iter(type); !iter.done(); iter.next()) {
      ABIResult result = iter.cur();
      if (result.inRegister() && result.type().kind() == ValType::I32) {
        masm.movl(result.gpr(), result.gpr());
      }
    }
  }
#endif

  inline void freeResultRegisters(ResultType type, RegKind which) {
    if (type.empty()) {
      return;
    }

    for (ABIResultIter iter(type); !iter.done(); iter.next()) {
      ABIResult result = iter.cur();
      // Register results are visited first; when we see a stack result we're
      // done.
      if (!result.inRegister()) {
        return;
      }
      switch (result.type().kind()) {
        case ValType::I32:
          freeI32(RegI32(result.gpr()));
          break;
        case ValType::I64:
          freeI64(RegI64(result.gpr64()));
          break;
        case ValType::V128:
#ifdef ENABLE_WASM_SIMD
          if (which == RegKind::All) {
            freeV128(RegV128(result.fpr()));
          }
          break;
#else
          MOZ_CRASH("No SIMD support");
#endif
        case ValType::F32:
          if (which == RegKind::All) {
            freeF32(RegF32(result.fpr()));
          }
          break;
        case ValType::F64:
          if (which == RegKind::All) {
            freeF64(RegF64(result.fpr()));
          }
          break;
        case ValType::Rtt:
        case ValType::Ref:
          freeRef(RegRef(result.gpr()));
          break;
      }
    }
  }

  void needIntegerResultRegisters(ResultType type) {
    needResultRegisters(type, RegKind::OnlyGPRs);
  }
  void freeIntegerResultRegisters(ResultType type) {
    freeResultRegisters(type, RegKind::OnlyGPRs);
  }

  void needResultRegisters(ResultType type) {
    needResultRegisters(type, RegKind::All);
  }
  void freeResultRegisters(ResultType type) {
    freeResultRegisters(type, RegKind::All);
  }

  void assertResultRegistersAvailable(ResultType type) {
#ifdef DEBUG
    for (ABIResultIter iter(type); !iter.done(); iter.next()) {
      ABIResult result = iter.cur();
      if (!result.inRegister()) {
        return;
      }
      switch (result.type().kind()) {
        case ValType::I32:
          MOZ_ASSERT(isAvailableI32(RegI32(result.gpr())));
          break;
        case ValType::I64:
          MOZ_ASSERT(isAvailableI64(RegI64(result.gpr64())));
          break;
        case ValType::V128:
#  ifdef ENABLE_WASM_SIMD
          MOZ_ASSERT(isAvailableV128(RegV128(result.fpr())));
          break;
#  else
          MOZ_CRASH("No SIMD support");
#  endif
        case ValType::F32:
          MOZ_ASSERT(isAvailableF32(RegF32(result.fpr())));
          break;
        case ValType::F64:
          MOZ_ASSERT(isAvailableF64(RegF64(result.fpr())));
          break;
        case ValType::Rtt:
        case ValType::Ref:
          MOZ_ASSERT(isAvailableRef(RegRef(result.gpr())));
          break;
      }
    }
#endif
  }

  void captureResultRegisters(ResultType type) {
    assertResultRegistersAvailable(type);
    needResultRegisters(type);
  }

  void captureCallResultRegisters(ResultType type) {
    captureResultRegisters(type);
#ifdef JS_CODEGEN_X64
    if (JitOptions.spectreIndexMasking) {
      maskResultRegisters(type);
    }
#endif
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

  // This is the value stack actually used during compilation.  It is a
  // StkVector rather than a StkVector& since constantly dereferencing a
  // StkVector& adds about 0.5% or more to the compiler's dynamic instruction
  // count.
  StkVector stk_;

  // Max number of pushes onto the value stack for any opcode or emitter that
  // does not push a variable, unbounded amount (anything with multiple
  // results).  This includes also intermediate pushes such as values pushed as
  // parameters for builtin calls.
  //
  // This limit is set quite high on purpose, so as to avoid brittleness.  The
  // true max value is likely no more than four or five.

  static constexpr size_t MaxPushesPerOpcode = 10;

  // BaselineCompileFunctions() "lends" us the StkVector to use in this
  // BaseCompiler object, and that is installed in |stk_| in our constructor.
  // This is so as to avoid having to malloc/free the vector's contents at
  // each creation/destruction of a BaseCompiler object.  It does however mean
  // that we need to hold on to a reference to BaselineCompileFunctions()'s
  // vector, so we can swap (give) its contents back when this BaseCompiler
  // object is destroyed.  This significantly reduces the heap turnover of the
  // baseline compiler.  See bug 1532592.
  StkVector& stkSource_;

#ifdef DEBUG
  size_t countMemRefsOnStk() {
    size_t nRefs = 0;
    for (Stk& v : stk_) {
      if (v.kind() == Stk::MemRef) {
        nRefs++;
      }
    }
    return nRefs;
  }
#endif

  template <typename T>
  void push(T item) {
    // None of the single-arg Stk constructors create a Stk::MemRef, so
    // there's no need to increment stackMapGenerator_.memRefsOnStk here.
    stk_.infallibleEmplaceBack(Stk(item));
  }

  void pushConstRef(intptr_t v) { stk_.infallibleEmplaceBack(Stk::StkRef(v)); }

  void loadConstI32(const Stk& src, RegI32 dest) {
    moveImm32(src.i32val(), dest);
  }

  void loadMemI32(const Stk& src, RegI32 dest) {
    fr.loadStackI32(src.offs(), dest);
  }

  void loadLocalI32(const Stk& src, RegI32 dest) {
    fr.loadLocalI32(localFromSlot(src.slot(), MIRType::Int32), dest);
  }

  void loadRegisterI32(const Stk& src, RegI32 dest) {
    moveI32(src.i32reg(), dest);
  }

  void loadConstI64(const Stk& src, RegI64 dest) {
    moveImm64(src.i64val(), dest);
  }

  void loadMemI64(const Stk& src, RegI64 dest) {
    fr.loadStackI64(src.offs(), dest);
  }

  void loadLocalI64(const Stk& src, RegI64 dest) {
    fr.loadLocalI64(localFromSlot(src.slot(), MIRType::Int64), dest);
  }

  void loadRegisterI64(const Stk& src, RegI64 dest) {
    moveI64(src.i64reg(), dest);
  }

  void loadConstRef(const Stk& src, RegRef dest) {
    moveImmRef(src.refval(), dest);
  }

  void loadMemRef(const Stk& src, RegRef dest) {
    fr.loadStackRef(src.offs(), dest);
  }

  void loadLocalRef(const Stk& src, RegRef dest) {
    fr.loadLocalRef(localFromSlot(src.slot(), MIRType::RefOrNull), dest);
  }

  void loadRegisterRef(const Stk& src, RegRef dest) {
    moveRef(src.refReg(), dest);
  }

  void loadConstF64(const Stk& src, RegF64 dest) {
    double d;
    src.f64val(&d);
    masm.loadConstantDouble(d, dest);
  }

  void loadMemF64(const Stk& src, RegF64 dest) {
    fr.loadStackF64(src.offs(), dest);
  }

  void loadLocalF64(const Stk& src, RegF64 dest) {
    fr.loadLocalF64(localFromSlot(src.slot(), MIRType::Double), dest);
  }

  void loadRegisterF64(const Stk& src, RegF64 dest) {
    moveF64(src.f64reg(), dest);
  }

  void loadConstF32(const Stk& src, RegF32 dest) {
    float f;
    src.f32val(&f);
    masm.loadConstantFloat32(f, dest);
  }

  void loadMemF32(const Stk& src, RegF32 dest) {
    fr.loadStackF32(src.offs(), dest);
  }

  void loadLocalF32(const Stk& src, RegF32 dest) {
    fr.loadLocalF32(localFromSlot(src.slot(), MIRType::Float32), dest);
  }

  void loadRegisterF32(const Stk& src, RegF32 dest) {
    moveF32(src.f32reg(), dest);
  }

#ifdef ENABLE_WASM_SIMD
  void loadConstV128(const Stk& src, RegV128 dest) {
    V128 f;
    src.v128val(&f);
    masm.loadConstantSimd128(SimdConstant::CreateX16((int8_t*)f.bytes), dest);
  }

  void loadMemV128(const Stk& src, RegV128 dest) {
    fr.loadStackV128(src.offs(), dest);
  }

  void loadLocalV128(const Stk& src, RegV128 dest) {
    fr.loadLocalV128(localFromSlot(src.slot(), MIRType::Simd128), dest);
  }

  void loadRegisterV128(const Stk& src, RegV128 dest) {
    moveV128(src.v128reg(), dest);
  }
#endif

  void loadI32(const Stk& src, RegI32 dest) {
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
      default:
        MOZ_CRASH("Compiler bug: Expected I32 on stack");
    }
  }

  void loadI64(const Stk& src, RegI64 dest) {
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
      default:
        MOZ_CRASH("Compiler bug: Expected I64 on stack");
    }
  }

#if !defined(JS_PUNBOX64)
  void loadI64Low(const Stk& src, RegI32 dest) {
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
      default:
        MOZ_CRASH("Compiler bug: Expected I64 on stack");
    }
  }

  void loadI64High(const Stk& src, RegI32 dest) {
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
      default:
        MOZ_CRASH("Compiler bug: Expected I64 on stack");
    }
  }
#endif

  void loadF64(const Stk& src, RegF64 dest) {
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
      default:
        MOZ_CRASH("Compiler bug: expected F64 on stack");
    }
  }

  void loadF32(const Stk& src, RegF32 dest) {
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
      default:
        MOZ_CRASH("Compiler bug: expected F32 on stack");
    }
  }

#ifdef ENABLE_WASM_SIMD
  void loadV128(const Stk& src, RegV128 dest) {
    switch (src.kind()) {
      case Stk::ConstV128:
        loadConstV128(src, dest);
        break;
      case Stk::MemV128:
        loadMemV128(src, dest);
        break;
      case Stk::LocalV128:
        loadLocalV128(src, dest);
        break;
      case Stk::RegisterV128:
        loadRegisterV128(src, dest);
        break;
      default:
        MOZ_CRASH("Compiler bug: expected V128 on stack");
    }
  }
#endif

  void loadRef(const Stk& src, RegRef dest) {
    switch (src.kind()) {
      case Stk::ConstRef:
        loadConstRef(src, dest);
        break;
      case Stk::MemRef:
        loadMemRef(src, dest);
        break;
      case Stk::LocalRef:
        loadLocalRef(src, dest);
        break;
      case Stk::RegisterRef:
        loadRegisterRef(src, dest);
        break;
      default:
        MOZ_CRASH("Compiler bug: expected ref on stack");
    }
  }

  // Duplicate the reference at the specified depth and load it into a register.
  void dupRefAt(uint32_t depth, RegRef dest) {
    MOZ_ASSERT(depth < stk_.length());
    Stk& src = peek(stk_.length() - depth - 1);
    loadRef(src, dest);
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
      // Memory opcodes are first in the enum, single check against MemLast is
      // fine.
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
          uint32_t offs = fr.pushGPR(scratch);
          v.setOffs(Stk::MemI32, offs);
          break;
        }
        case Stk::RegisterI32: {
          uint32_t offs = fr.pushGPR(v.i32reg());
          freeI32(v.i32reg());
          v.setOffs(Stk::MemI32, offs);
          break;
        }
        case Stk::LocalI64: {
          ScratchI32 scratch(*this);
#ifdef JS_PUNBOX64
          loadI64(v, fromI32(scratch));
          uint32_t offs = fr.pushGPR(scratch);
#else
          fr.loadLocalI64High(localFromSlot(v.slot(), MIRType::Int64), scratch);
          fr.pushGPR(scratch);
          fr.loadLocalI64Low(localFromSlot(v.slot(), MIRType::Int64), scratch);
          uint32_t offs = fr.pushGPR(scratch);
#endif
          v.setOffs(Stk::MemI64, offs);
          break;
        }
        case Stk::RegisterI64: {
#ifdef JS_PUNBOX64
          uint32_t offs = fr.pushGPR(v.i64reg().reg);
          freeI64(v.i64reg());
#else
          fr.pushGPR(v.i64reg().high);
          uint32_t offs = fr.pushGPR(v.i64reg().low);
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
#ifdef ENABLE_WASM_SIMD
        case Stk::LocalV128: {
          ScratchV128 scratch(*this);
          loadV128(v, scratch);
          uint32_t offs = fr.pushV128(scratch);
          v.setOffs(Stk::MemV128, offs);
          break;
        }
        case Stk::RegisterV128: {
          uint32_t offs = fr.pushV128(v.v128reg());
          freeV128(v.v128reg());
          v.setOffs(Stk::MemV128, offs);
          break;
        }
#endif
        case Stk::LocalRef: {
          ScratchRef scratch(*this);
          loadLocalRef(v, scratch);
          uint32_t offs = fr.pushGPR(scratch);
          v.setOffs(Stk::MemRef, offs);
          stackMapGenerator_.memRefsOnStk++;
          break;
        }
        case Stk::RegisterRef: {
          uint32_t offs = fr.pushGPR(v.refReg());
          freeRef(v.refReg());
          v.setOffs(Stk::MemRef, offs);
          stackMapGenerator_.memRefsOnStk++;
          break;
        }
        default: {
          break;
        }
      }
    }
  }

  void saveTempPtr(RegPtr r) final {
    MOZ_ASSERT(!ra.isAvailablePtr(r));
    fr.pushGPR(r);
    ra.freePtr(r);
    MOZ_ASSERT(ra.isAvailablePtr(r));
  }

  void restoreTempPtr(RegPtr r) final {
    MOZ_ASSERT(ra.isAvailablePtr(r));
    ra.needPtr(r);
    fr.popGPR(r);
    MOZ_ASSERT(!ra.isAvailablePtr(r));
  }

  // Various methods for creating a stackmap.  Stackmaps are indexed by the
  // lowest address of the instruction immediately *after* the instruction of
  // interest.  In practice that means either: the return point of a call, the
  // instruction immediately after a trap instruction (the "resume"
  // instruction), or the instruction immediately following a no-op (when
  // debugging is enabled).

  // Create a vanilla stackmap.
  [[nodiscard]] bool createStackMap(const char* who) {
    const ExitStubMapVector noExtras;
    return createStackMap(who, noExtras, masm.currentOffset());
  }

  // Create a stackmap as vanilla, but for a custom assembler offset.
  [[nodiscard]] bool createStackMap(const char* who,
                                    CodeOffset assemblerOffset) {
    const ExitStubMapVector noExtras;
    return createStackMap(who, noExtras, assemblerOffset.offset());
  }

  // The most general stackmap construction.
  [[nodiscard]] bool createStackMap(const char* who,
                                    const ExitStubMapVector& extras,
                                    uint32_t assemblerOffset) {
    auto debugFrame =
        compilerEnv_.debugEnabled() ? HasDebugFrame::Yes : HasDebugFrame::No;
    return stackMapGenerator_.createStackMap(who, extras, assemblerOffset,
                                             debugFrame, stk_);
  }

  // This is an optimization used to avoid calling sync() for
  // setLocal(): if the local does not exist unresolved on the stack
  // then we can skip the sync.

  bool hasLocal(uint32_t slot) {
    for (size_t i = stk_.length(); i > 0; i--) {
      // Memory opcodes are first in the enum, single check against MemLast is
      // fine.
      Stk::Kind kind = stk_[i - 1].kind();
      if (kind <= Stk::MemLast) {
        return false;
      }

      // Local opcodes follow memory opcodes in the enum, single check against
      // LocalLast is sufficient.
      if (kind <= Stk::LocalLast && stk_[i - 1].slot() == slot) {
        return true;
      }
    }
    return false;
  }

  void syncLocal(uint32_t slot) {
    if (hasLocal(slot)) {
      sync();  // TODO / OPTIMIZE: Improve this?  (Bug 1316817)
    }
  }

  // Push the register r onto the stack.

  void pushAny(AnyReg r) {
    switch (r.tag) {
      case AnyReg::I32: {
        pushI32(r.i32());
        break;
      }
      case AnyReg::I64: {
        pushI64(r.i64());
        break;
      }
      case AnyReg::F32: {
        pushF32(r.f32());
        break;
      }
      case AnyReg::F64: {
        pushF64(r.f64());
        break;
      }
#ifdef ENABLE_WASM_SIMD
      case AnyReg::V128: {
        pushV128(r.v128());
        break;
      }
#endif
      case AnyReg::REF: {
        pushRef(r.ref());
        break;
      }
    }
  }

  void pushI32(RegI32 r) {
    MOZ_ASSERT(!isAvailableI32(r));
    push(Stk(r));
  }

  void pushI64(RegI64 r) {
    MOZ_ASSERT(!isAvailableI64(r));
    push(Stk(r));
  }

  void pushRef(RegRef r) {
    MOZ_ASSERT(!isAvailableRef(r));
    push(Stk(r));
  }

  void pushPtr(RegPtr r) {
    MOZ_ASSERT(!isAvailablePtr(r));
#ifdef JS_64BIT
    pushI64(RegI64(Register64(r)));
#else
    pushI32(RegI32(r));
#endif
  }

  void pushF64(RegF64 r) {
    MOZ_ASSERT(!isAvailableF64(r));
    push(Stk(r));
  }

  void pushF32(RegF32 r) {
    MOZ_ASSERT(!isAvailableF32(r));
    push(Stk(r));
  }

#ifdef ENABLE_WASM_SIMD
  void pushV128(RegV128 r) {
    MOZ_ASSERT(!isAvailableV128(r));
    push(Stk(r));
  }
#endif

  // Push the value onto the stack.  PushI32 can also take uint32_t, and PushI64
  // can take uint64_t; the semantics are the same.  Appropriate sign extension
  // for a 32-bit value on a 64-bit architecture happens when the value is
  // popped, see the definition of moveImm32 below.

  void pushI32(int32_t v) { push(Stk(v)); }

  void pushI64(int64_t v) { push(Stk(v)); }

  void pushRef(intptr_t v) { pushConstRef(v); }

  void pushF64(double v) { push(Stk(v)); }

  void pushF32(float v) { push(Stk(v)); }

#ifdef ENABLE_WASM_SIMD
  void pushV128(V128 v) { push(Stk(v)); }
#endif

  // Push the local slot onto the stack.  The slot will not be read
  // here; it will be read when it is consumed, or when a side
  // effect to the slot forces its value to be saved.

  void pushLocalI32(uint32_t slot) {
    stk_.infallibleEmplaceBack(Stk(Stk::LocalI32, slot));
  }

  void pushLocalI64(uint32_t slot) {
    stk_.infallibleEmplaceBack(Stk(Stk::LocalI64, slot));
  }

  void pushLocalRef(uint32_t slot) {
    stk_.infallibleEmplaceBack(Stk(Stk::LocalRef, slot));
  }

  void pushLocalF64(uint32_t slot) {
    stk_.infallibleEmplaceBack(Stk(Stk::LocalF64, slot));
  }

  void pushLocalF32(uint32_t slot) {
    stk_.infallibleEmplaceBack(Stk(Stk::LocalF32, slot));
  }

#ifdef ENABLE_WASM_SIMD
  void pushLocalV128(uint32_t slot) {
    stk_.infallibleEmplaceBack(Stk(Stk::LocalV128, slot));
  }
#endif

  AnyReg dupAny(AnyReg r) {
    switch (r.tag) {
      case AnyReg::I32: {
        RegI32 x = needI32();
        moveI32(r.i32(), x);
        return AnyReg(x);
      }
      case AnyReg::I64: {
        RegI64 x = needI64();
        moveI64(r.i64(), x);
        return AnyReg(x);
      }
      case AnyReg::F32: {
        RegF32 x = needF32();
        moveF32(r.f32(), x);
        return AnyReg(x);
      }
      case AnyReg::F64: {
        RegF64 x = needF64();
        moveF64(r.f64(), x);
        return AnyReg(x);
      }
#ifdef ENABLE_WASM_SIMD
      case AnyReg::V128: {
        RegV128 x = needV128();
        moveV128(r.v128(), x);
        return AnyReg(x);
      }
#endif
      case AnyReg::REF: {
        RegRef x = needRef();
        moveRef(r.ref(), x);
        return AnyReg(x);
      }
      default:
        MOZ_CRASH();
    }
  }

  AnyReg popAny(AnyReg specific) {
    switch (stk_.back().kind()) {
      case Stk::MemI32:
      case Stk::LocalI32:
      case Stk::RegisterI32:
      case Stk::ConstI32:
        return AnyReg(popI32(specific.i32()));

      case Stk::MemI64:
      case Stk::LocalI64:
      case Stk::RegisterI64:
      case Stk::ConstI64:
        return AnyReg(popI64(specific.i64()));

      case Stk::MemF32:
      case Stk::LocalF32:
      case Stk::RegisterF32:
      case Stk::ConstF32:
        return AnyReg(popF32(specific.f32()));

      case Stk::MemF64:
      case Stk::LocalF64:
      case Stk::RegisterF64:
      case Stk::ConstF64:
        return AnyReg(popF64(specific.f64()));

#ifdef ENABLE_WASM_SIMD
      case Stk::MemV128:
      case Stk::LocalV128:
      case Stk::RegisterV128:
      case Stk::ConstV128:
        return AnyReg(popV128(specific.v128()));
#endif

      case Stk::MemRef:
      case Stk::LocalRef:
      case Stk::RegisterRef:
      case Stk::ConstRef:
        return AnyReg(popRef(specific.ref()));

      case Stk::Unknown:
        MOZ_CRASH();

      default:
        MOZ_CRASH();
    }
  }

  AnyReg popAny() {
    switch (stk_.back().kind()) {
      case Stk::MemI32:
      case Stk::LocalI32:
      case Stk::RegisterI32:
      case Stk::ConstI32:
        return AnyReg(popI32());

      case Stk::MemI64:
      case Stk::LocalI64:
      case Stk::RegisterI64:
      case Stk::ConstI64:
        return AnyReg(popI64());

      case Stk::MemF32:
      case Stk::LocalF32:
      case Stk::RegisterF32:
      case Stk::ConstF32:
        return AnyReg(popF32());

      case Stk::MemF64:
      case Stk::LocalF64:
      case Stk::RegisterF64:
      case Stk::ConstF64:
        return AnyReg(popF64());

#ifdef ENABLE_WASM_SIMD
      case Stk::MemV128:
      case Stk::LocalV128:
      case Stk::RegisterV128:
      case Stk::ConstV128:
        return AnyReg(popV128());
#endif

      case Stk::MemRef:
      case Stk::LocalRef:
      case Stk::RegisterRef:
      case Stk::ConstRef:
        return AnyReg(popRef());

      case Stk::Unknown:
        MOZ_CRASH();

      default:
        MOZ_CRASH();
    }
  }

  // Call only from other popI32() variants.
  // v must be the stack top.  May pop the CPU stack.

  void popI32(const Stk& v, RegI32 dest) {
    MOZ_ASSERT(&v == &stk_.back());
    switch (v.kind()) {
      case Stk::ConstI32:
        loadConstI32(v, dest);
        break;
      case Stk::LocalI32:
        loadLocalI32(v, dest);
        break;
      case Stk::MemI32:
        fr.popGPR(dest);
        break;
      case Stk::RegisterI32:
        loadRegisterI32(v, dest);
        break;
      default:
        MOZ_CRASH("Compiler bug: expected int on stack");
    }
  }

  [[nodiscard]] RegI32 popI32() {
    Stk& v = stk_.back();
    RegI32 r;
    if (v.kind() == Stk::RegisterI32) {
      r = v.i32reg();
    } else {
      popI32(v, (r = needI32()));
    }
    stk_.popBack();
    return r;
  }

  RegI32 popI32(RegI32 specific) {
    Stk& v = stk_.back();

    if (!(v.kind() == Stk::RegisterI32 && v.i32reg() == specific)) {
      needI32(specific);
      popI32(v, specific);
      if (v.kind() == Stk::RegisterI32) {
        freeI32(v.i32reg());
      }
    }

    stk_.popBack();
    return specific;
  }

#ifdef ENABLE_WASM_SIMD
  // Call only from other popV128() variants.
  // v must be the stack top.  May pop the CPU stack.

  void popV128(const Stk& v, RegV128 dest) {
    MOZ_ASSERT(&v == &stk_.back());
    switch (v.kind()) {
      case Stk::ConstV128:
        loadConstV128(v, dest);
        break;
      case Stk::LocalV128:
        loadLocalV128(v, dest);
        break;
      case Stk::MemV128:
        fr.popV128(dest);
        break;
      case Stk::RegisterV128:
        loadRegisterV128(v, dest);
        break;
      default:
        MOZ_CRASH("Compiler bug: expected int on stack");
    }
  }

  [[nodiscard]] RegV128 popV128() {
    Stk& v = stk_.back();
    RegV128 r;
    if (v.kind() == Stk::RegisterV128) {
      r = v.v128reg();
    } else {
      popV128(v, (r = needV128()));
    }
    stk_.popBack();
    return r;
  }

  RegV128 popV128(RegV128 specific) {
    Stk& v = stk_.back();

    if (!(v.kind() == Stk::RegisterV128 && v.v128reg() == specific)) {
      needV128(specific);
      popV128(v, specific);
      if (v.kind() == Stk::RegisterV128) {
        freeV128(v.v128reg());
      }
    }

    stk_.popBack();
    return specific;
  }
#endif

  // Call only from other popI64() variants.
  // v must be the stack top.  May pop the CPU stack.

  void popI64(const Stk& v, RegI64 dest) {
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
        fr.popGPR(dest.reg);
#else
        fr.popGPR(dest.low);
        fr.popGPR(dest.high);
#endif
        break;
      case Stk::RegisterI64:
        loadRegisterI64(v, dest);
        break;
      default:
        MOZ_CRASH("Compiler bug: expected long on stack");
    }
  }

  [[nodiscard]] RegI64 popI64() {
    Stk& v = stk_.back();
    RegI64 r;
    if (v.kind() == Stk::RegisterI64) {
      r = v.i64reg();
    } else {
      popI64(v, (r = needI64()));
    }
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
      if (v.kind() == Stk::RegisterI64) {
        freeI64(v.i64reg());
      }
    }

    stk_.popBack();
    return specific;
  }

  // Call only from other popRef() variants.
  // v must be the stack top.  May pop the CPU stack.

  void popRef(const Stk& v, RegRef dest) {
    MOZ_ASSERT(&v == &stk_.back());
    switch (v.kind()) {
      case Stk::ConstRef:
        loadConstRef(v, dest);
        break;
      case Stk::LocalRef:
        loadLocalRef(v, dest);
        break;
      case Stk::MemRef:
        fr.popGPR(dest);
        break;
      case Stk::RegisterRef:
        loadRegisterRef(v, dest);
        break;
      default:
        MOZ_CRASH("Compiler bug: expected ref on stack");
    }
  }

  RegRef popRef(RegRef specific) {
    Stk& v = stk_.back();

    if (!(v.kind() == Stk::RegisterRef && v.refReg() == specific)) {
      needRef(specific);
      popRef(v, specific);
      if (v.kind() == Stk::RegisterRef) {
        freeRef(v.refReg());
      }
    }

    stk_.popBack();
    if (v.kind() == Stk::MemRef) {
      stackMapGenerator_.memRefsOnStk--;
    }
    return specific;
  }

  [[nodiscard]] RegRef popRef() {
    Stk& v = stk_.back();
    RegRef r;
    if (v.kind() == Stk::RegisterRef) {
      r = v.refReg();
    } else {
      popRef(v, (r = needRef()));
    }
    stk_.popBack();
    if (v.kind() == Stk::MemRef) {
      stackMapGenerator_.memRefsOnStk--;
    }
    return r;
  }

  // Call only from other popPtr() variants.
  // v must be the stack top.  May pop the CPU stack.

  void popPtr(const Stk& v, RegPtr dest) {
#ifdef JS_64BIT
    popI64(v, RegI64(Register64(dest)));
#else
    popI32(v, RegI32(dest));
#endif
  }

  RegPtr popPtr(RegPtr specific) {
#ifdef JS_64BIT
    return RegPtr(popI64(RegI64(Register64(specific))).reg);
#else
    return RegPtr(popI32(RegI32(specific)));
#endif
  }

  [[nodiscard]] RegPtr popPtr() {
#ifdef JS_64BIT
    return RegPtr(popI64().reg);
#else
    return RegPtr(popI32());
#endif
  }

  // Call only from other popF64() variants.
  // v must be the stack top.  May pop the CPU stack.

  void popF64(const Stk& v, RegF64 dest) {
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
      default:
        MOZ_CRASH("Compiler bug: expected double on stack");
    }
  }

  [[nodiscard]] RegF64 popF64() {
    Stk& v = stk_.back();
    RegF64 r;
    if (v.kind() == Stk::RegisterF64) {
      r = v.f64reg();
    } else {
      popF64(v, (r = needF64()));
    }
    stk_.popBack();
    return r;
  }

  RegF64 popF64(RegF64 specific) {
    Stk& v = stk_.back();

    if (!(v.kind() == Stk::RegisterF64 && v.f64reg() == specific)) {
      needF64(specific);
      popF64(v, specific);
      if (v.kind() == Stk::RegisterF64) {
        freeF64(v.f64reg());
      }
    }

    stk_.popBack();
    return specific;
  }

  // Call only from other popF32() variants.
  // v must be the stack top.  May pop the CPU stack.

  void popF32(const Stk& v, RegF32 dest) {
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
      default:
        MOZ_CRASH("Compiler bug: expected float on stack");
    }
  }

  [[nodiscard]] RegF32 popF32() {
    Stk& v = stk_.back();
    RegF32 r;
    if (v.kind() == Stk::RegisterF32) {
      r = v.f32reg();
    } else {
      popF32(v, (r = needF32()));
    }
    stk_.popBack();
    return r;
  }

  RegF32 popF32(RegF32 specific) {
    Stk& v = stk_.back();

    if (!(v.kind() == Stk::RegisterF32 && v.f32reg() == specific)) {
      needF32(specific);
      popF32(v, specific);
      if (v.kind() == Stk::RegisterF32) {
        freeF32(v.f32reg());
      }
    }

    stk_.popBack();
    return specific;
  }

  [[nodiscard]] bool popConst(int32_t* c) {
    Stk& v = stk_.back();
    if (v.kind() != Stk::ConstI32) {
      return false;
    }
    *c = v.i32val();
    stk_.popBack();
    return true;
  }

  [[nodiscard]] bool popConst(int64_t* c) {
    Stk& v = stk_.back();
    if (v.kind() != Stk::ConstI64) {
      return false;
    }
    *c = v.i64val();
    stk_.popBack();
    return true;
  }

  [[nodiscard]] bool peekConst(int32_t* c) {
    Stk& v = stk_.back();
    if (v.kind() != Stk::ConstI32) {
      return false;
    }
    *c = v.i32val();
    return true;
  }

  [[nodiscard]] bool peekConst(int64_t* c) {
    Stk& v = stk_.back();
    if (v.kind() != Stk::ConstI64) {
      return false;
    }
    *c = v.i64val();
    return true;
  }

  [[nodiscard]] bool peek2xConst(int32_t* c0, int32_t* c1) {
    MOZ_ASSERT(stk_.length() >= 2);
    const Stk& v0 = *(stk_.end() - 1);
    const Stk& v1 = *(stk_.end() - 2);
    if (v0.kind() != Stk::ConstI32 || v1.kind() != Stk::ConstI32) {
      return false;
    }
    *c0 = v0.i32val();
    *c1 = v1.i32val();
    return true;
  }

  [[nodiscard]] bool popConstPositivePowerOfTwo(int32_t* c, uint_fast8_t* power,
                                                int32_t cutoff) {
    Stk& v = stk_.back();
    if (v.kind() != Stk::ConstI32) {
      return false;
    }
    *c = v.i32val();
    if (*c <= cutoff || !IsPowerOfTwo(static_cast<uint32_t>(*c))) {
      return false;
    }
    *power = FloorLog2(*c);
    stk_.popBack();
    return true;
  }

  [[nodiscard]] bool popConstPositivePowerOfTwo(int64_t* c, uint_fast8_t* power,
                                                int64_t cutoff) {
    Stk& v = stk_.back();
    if (v.kind() != Stk::ConstI64) {
      return false;
    }
    *c = v.i64val();
    if (*c <= cutoff || !IsPowerOfTwo(static_cast<uint64_t>(*c))) {
      return false;
    }
    *power = FloorLog2(*c);
    stk_.popBack();
    return true;
  }

  [[nodiscard]] bool peekLocalI32(uint32_t* local) {
    Stk& v = stk_.back();
    if (v.kind() != Stk::LocalI32) {
      return false;
    }
    *local = v.slot();
    return true;
  }

  // TODO / OPTIMIZE (Bug 1316818): At the moment we use the Wasm
  // inter-procedure ABI for block returns, which allocates ReturnReg as the
  // single block result register.  It is possible other choices would lead to
  // better register allocation, as ReturnReg is often first in the register set
  // and will be heavily wanted by the register allocator that uses takeFirst().
  //
  // Obvious options:
  //  - pick a register at the back of the register set
  //  - pick a random register per block (different blocks have
  //    different join regs)

  void popRegisterResults(ABIResultIter& iter) {
    // Pop register results.  Note that in the single-value case, popping to a
    // register may cause a sync(); for multi-value we sync'd already.
    for (; !iter.done(); iter.next()) {
      const ABIResult& result = iter.cur();
      if (!result.inRegister()) {
        // TODO / OPTIMIZE: We sync here to avoid solving the general parallel
        // move problem in popStackResults.  However we could avoid syncing the
        // values that are going to registers anyway, if they are already in
        // registers.
        sync();
        break;
      }
      switch (result.type().kind()) {
        case ValType::I32:
          popI32(RegI32(result.gpr()));
          break;
        case ValType::I64:
          popI64(RegI64(result.gpr64()));
          break;
        case ValType::F32:
          popF32(RegF32(result.fpr()));
          break;
        case ValType::F64:
          popF64(RegF64(result.fpr()));
          break;
        case ValType::Rtt:
        case ValType::Ref:
          popRef(RegRef(result.gpr()));
          break;
        case ValType::V128:
#ifdef ENABLE_WASM_SIMD
          popV128(RegV128(result.fpr()));
#else
          MOZ_CRASH("No SIMD support");
#endif
      }
    }
  }

  void popStackResults(ABIResultIter& iter, StackHeight stackBase) {
    MOZ_ASSERT(!iter.done());

    // The iterator should be advanced beyond register results, and register
    // results should be popped already from the value stack.
    uint32_t alreadyPopped = iter.index();

    // At this point, only stack arguments are remaining.  Iterate through them
    // to measure how much stack space they will take up.
    for (; !iter.done(); iter.next()) {
      MOZ_ASSERT(iter.cur().onStack());
    }

    // Calculate the space needed to store stack results, in bytes.
    uint32_t stackResultBytes = iter.stackBytesConsumedSoFar();
    MOZ_ASSERT(stackResultBytes);

    // Compute the stack height including the stack results.  Note that it's
    // possible that this call expands the stack, for example if some of the
    // results are supplied by constants and so are not already on the machine
    // stack.
    uint32_t endHeight = fr.prepareStackResultArea(stackBase, stackResultBytes);

    // Find a free GPR to use when shuffling stack values.  If none is
    // available, push ReturnReg and restore it after we're done.
    bool saved = false;
    RegPtr temp = ra.needTempPtr(RegPtr(ReturnReg), &saved);

    // The sequence of Stk values is in the same order on the machine stack as
    // the result locations, but there is a complication: constant values are
    // not actually pushed on the machine stack.  (At this point registers and
    // locals have been spilled already.)  So, moving the Stk values into place
    // isn't simply a shuffle-down or shuffle-up operation.  There is a part of
    // the Stk sequence that shuffles toward the FP, a part that's already in
    // place, and a part that shuffles toward the SP.  After shuffling, we have
    // to materialize the constants.

    // Shuffle mem values toward the frame pointer, copying deepest values
    // first.  Stop when we run out of results, get to a register result, or
    // find a Stk value that is closer to the FP than the result.
    for (iter.switchToPrev(); !iter.done(); iter.prev()) {
      const ABIResult& result = iter.cur();
      if (!result.onStack()) {
        break;
      }
      MOZ_ASSERT(result.stackOffset() < stackResultBytes);
      uint32_t destHeight = endHeight - result.stackOffset();
      uint32_t stkBase = stk_.length() - (iter.count() - alreadyPopped);
      Stk& v = stk_[stkBase + iter.index()];
      if (v.isMem()) {
        uint32_t srcHeight = v.offs();
        if (srcHeight <= destHeight) {
          break;
        }
        fr.shuffleStackResultsTowardFP(srcHeight, destHeight, result.size(),
                                       temp);
      }
    }

    // Reset iterator and skip register results.
    for (iter.reset(); !iter.done(); iter.next()) {
      if (iter.cur().onStack()) {
        break;
      }
    }

    // Revisit top stack values, shuffling mem values toward the stack pointer,
    // copying shallowest values first.
    for (; !iter.done(); iter.next()) {
      const ABIResult& result = iter.cur();
      MOZ_ASSERT(result.onStack());
      MOZ_ASSERT(result.stackOffset() < stackResultBytes);
      uint32_t destHeight = endHeight - result.stackOffset();
      Stk& v = stk_[stk_.length() - (iter.index() - alreadyPopped) - 1];
      if (v.isMem()) {
        uint32_t srcHeight = v.offs();
        if (srcHeight >= destHeight) {
          break;
        }
        fr.shuffleStackResultsTowardSP(srcHeight, destHeight, result.size(),
                                       temp);
      }
    }

    // Reset iterator and skip register results, which are already popped off
    // the value stack.
    for (iter.reset(); !iter.done(); iter.next()) {
      if (iter.cur().onStack()) {
        break;
      }
    }

    // Materialize constants and pop the remaining items from the value stack.
    for (; !iter.done(); iter.next()) {
      const ABIResult& result = iter.cur();
      uint32_t resultHeight = endHeight - result.stackOffset();
      Stk& v = stk_.back();
      switch (v.kind()) {
        case Stk::ConstI32:
          fr.storeImmediatePtrToStack(uint32_t(v.i32val_), resultHeight, temp);
          break;
        case Stk::ConstF32:
          fr.storeImmediateF32ToStack(v.f32val_, resultHeight, temp);
          break;
        case Stk::ConstI64:
          fr.storeImmediateI64ToStack(v.i64val_, resultHeight, temp);
          break;
        case Stk::ConstF64:
          fr.storeImmediateF64ToStack(v.f64val_, resultHeight, temp);
          break;
#ifdef ENABLE_WASM_SIMD
        case Stk::ConstV128:
          fr.storeImmediateV128ToStack(v.v128val_, resultHeight, temp);
          break;
#endif
        case Stk::ConstRef:
          fr.storeImmediatePtrToStack(v.refval_, resultHeight, temp);
          break;
        case Stk::MemRef:
          // Update bookkeeping as we pop the Stk entry.
          stackMapGenerator_.memRefsOnStk--;
          break;
        default:
          MOZ_ASSERT(v.isMem());
          break;
      }
      stk_.popBack();
    }

    ra.freeTempPtr(temp, saved);

    // This will pop the stack if needed.
    fr.finishStackResultArea(stackBase, stackResultBytes);
  }

  enum class ContinuationKind { Fallthrough, Jump };

  void popBlockResults(ResultType type, StackHeight stackBase,
                       ContinuationKind kind) {
    if (!type.empty()) {
      ABIResultIter iter(type);
      popRegisterResults(iter);
      if (!iter.done()) {
        popStackResults(iter, stackBase);
        // Because popStackResults might clobber the stack, it leaves the stack
        // pointer already in the right place for the continuation, whether the
        // continuation is a jump or fallthrough.
        return;
      }
    }
    // We get here if there are no stack results.  For a fallthrough, the stack
    // is already at the right height.  For a jump, we may need to pop the stack
    // pointer if the continuation's stack height is lower than the current
    // stack height.
    if (kind == ContinuationKind::Jump) {
      fr.popStackBeforeBranch(stackBase, type);
    }
  }

#ifdef ENABLE_WASM_EXCEPTIONS
  // This function is similar to popBlockResults, but additionally handles the
  // implicit exception pointer that is pushed to the value stack on entry to
  // a catch handler by dropping it appropriately.
  void popCatchResults(ResultType type, StackHeight stackBase) {
    if (!type.empty()) {
      ABIResultIter iter(type);
      popRegisterResults(iter);
      if (!iter.done()) {
        popStackResults(iter, stackBase);
        // Since popStackResults clobbers the stack, we only need to free the
        // exception off of the value stack.
        popValueStackBy(1);
      } else {
        // If there are no stack results, we have to adjust the stack by
        // dropping the exception reference that's now on the stack.
        dropValue();
      }
    } else {
      dropValue();
    }
    fr.popStackBeforeBranch(stackBase, type);
  }
#endif

  Stk captureStackResult(const ABIResult& result, StackHeight resultsBase,
                         uint32_t stackResultBytes) {
    MOZ_ASSERT(result.onStack());
    uint32_t offs = fr.locateStackResult(result, resultsBase, stackResultBytes);
    return Stk::StackResult(result.type(), offs);
  }

  [[nodiscard]] bool pushResults(ResultType type, StackHeight resultsBase) {
    if (type.empty()) {
      return true;
    }

    if (type.length() > 1) {
      // Reserve extra space on the stack for all the values we'll push.
      // Multi-value push is not accounted for by the pre-sizing of the stack in
      // the decoding loop.
      //
      // Also make sure we leave headroom for other pushes that will occur after
      // pushing results, just to be safe.
      if (!stk_.reserve(stk_.length() + type.length() + MaxPushesPerOpcode)) {
        return false;
      }
    }

    // We need to push the results in reverse order, so first iterate through
    // all results to determine the locations of stack result types.
    ABIResultIter iter(type);
    while (!iter.done()) {
      iter.next();
    }
    uint32_t stackResultBytes = iter.stackBytesConsumedSoFar();
    for (iter.switchToPrev(); !iter.done(); iter.prev()) {
      const ABIResult& result = iter.cur();
      if (!result.onStack()) {
        break;
      }
      Stk v = captureStackResult(result, resultsBase, stackResultBytes);
      push(v);
      if (v.kind() == Stk::MemRef) {
        stackMapGenerator_.memRefsOnStk++;
      }
    }

    for (; !iter.done(); iter.prev()) {
      const ABIResult& result = iter.cur();
      MOZ_ASSERT(result.inRegister());
      switch (result.type().kind()) {
        case ValType::I32:
          pushI32(RegI32(result.gpr()));
          break;
        case ValType::I64:
          pushI64(RegI64(result.gpr64()));
          break;
        case ValType::V128:
#ifdef ENABLE_WASM_SIMD
          pushV128(RegV128(result.fpr()));
          break;
#else
          MOZ_CRASH("No SIMD support");
#endif
        case ValType::F32:
          pushF32(RegF32(result.fpr()));
          break;
        case ValType::F64:
          pushF64(RegF64(result.fpr()));
          break;
        case ValType::Rtt:
        case ValType::Ref:
          pushRef(RegRef(result.gpr()));
          break;
      }
    }

    return true;
  }

  [[nodiscard]] bool pushBlockResults(ResultType type) {
    return pushResults(type, controlItem().stackHeight);
  }

  // A combination of popBlockResults + pushBlockResults, used when entering a
  // block with a control-flow join (loops) or split (if) to shuffle the
  // fallthrough block parameters into the locations expected by the
  // continuation.
  [[nodiscard]] bool topBlockParams(ResultType type) {
    // This function should only be called when entering a block with a
    // control-flow join at the entry, where there are no live temporaries in
    // the current block.
    StackHeight base = controlItem().stackHeight;
    MOZ_ASSERT(fr.stackResultsBase(stackConsumed(type.length())) == base);
    popBlockResults(type, base, ContinuationKind::Fallthrough);
    return pushBlockResults(type);
  }

  // A combination of popBlockResults + pushBlockResults, used before branches
  // where we don't know the target (br_if / br_table).  If and when the branch
  // is taken, the stack results will be shuffled down into place.  For br_if
  // that has fallthrough, the parameters for the untaken branch flow through to
  // the continuation.
  [[nodiscard]] bool topBranchParams(ResultType type, StackHeight* height) {
    if (type.empty()) {
      *height = fr.stackHeight();
      return true;
    }
    // There may be temporary values that need spilling; delay computation of
    // the stack results base until after the popRegisterResults(), which spills
    // if needed.
    ABIResultIter iter(type);
    popRegisterResults(iter);
    StackHeight base = fr.stackResultsBase(stackConsumed(iter.remaining()));
    if (!iter.done()) {
      popStackResults(iter, base);
    }
    if (!pushResults(type, base)) {
      return false;
    }
    *height = base;
    return true;
  }

  // Conditional branches with fallthrough are preceded by a topBranchParams, so
  // we know that there are no stack results that need to be materialized.  In
  // that case, we can just shuffle the whole block down before popping the
  // stack.
  void shuffleStackResultsBeforeBranch(StackHeight srcHeight,
                                       StackHeight destHeight,
                                       ResultType type) {
    uint32_t stackResultBytes = 0;

    if (ABIResultIter::HasStackResults(type)) {
      MOZ_ASSERT(stk_.length() >= type.length());
      ABIResultIter iter(type);
      for (; !iter.done(); iter.next()) {
#ifdef DEBUG
        const ABIResult& result = iter.cur();
        const Stk& v = stk_[stk_.length() - iter.index() - 1];
        MOZ_ASSERT(v.isMem() == result.onStack());
#endif
      }

      stackResultBytes = iter.stackBytesConsumedSoFar();
      MOZ_ASSERT(stackResultBytes > 0);

      if (srcHeight != destHeight) {
        // Find a free GPR to use when shuffling stack values.  If none
        // is available, push ReturnReg and restore it after we're done.
        bool saved = false;
        RegPtr temp = ra.needTempPtr(RegPtr(ReturnReg), &saved);
        fr.shuffleStackResultsTowardFP(srcHeight, destHeight, stackResultBytes,
                                       temp);
        ra.freeTempPtr(temp, saved);
      }
    }

    fr.popStackBeforeBranch(destHeight, stackResultBytes);
  }

  // Return the amount of execution stack consumed by the top numval
  // values on the value stack.

  size_t stackConsumed(size_t numval) {
    size_t size = 0;
    MOZ_ASSERT(numval <= stk_.length());
    for (uint32_t i = stk_.length() - 1; numval > 0; numval--, i--) {
      Stk& v = stk_[i];
      switch (v.kind()) {
        case Stk::MemRef:
          size += BaseStackFrame::StackSizeOfPtr;
          break;
        case Stk::MemI32:
          size += BaseStackFrame::StackSizeOfPtr;
          break;
        case Stk::MemI64:
          size += BaseStackFrame::StackSizeOfInt64;
          break;
        case Stk::MemF64:
          size += BaseStackFrame::StackSizeOfDouble;
          break;
        case Stk::MemF32:
          size += BaseStackFrame::StackSizeOfFloat;
          break;
#ifdef ENABLE_WASM_SIMD
        case Stk::MemV128:
          size += BaseStackFrame::StackSizeOfV128;
          break;
#endif
        default:
          break;
      }
    }
    return size;
  }

  void popValueStackTo(uint32_t stackSize) {
    for (uint32_t i = stk_.length(); i > stackSize; i--) {
      Stk& v = stk_[i - 1];
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
#ifdef ENABLE_WASM_SIMD
        case Stk::RegisterV128:
          freeV128(v.v128reg());
          break;
#endif
        case Stk::RegisterRef:
          freeRef(v.refReg());
          break;
        case Stk::MemRef:
          stackMapGenerator_.memRefsOnStk--;
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
    if (peek(0).isMem()) {
      fr.popBytes(stackConsumed(1));
    }
    popValueStackBy(1);
  }

  // Peek at the stack, for calls.

  Stk& peek(uint32_t relativeDepth) {
    return stk_[stk_.length() - 1 - relativeDepth];
  }

#ifdef DEBUG
  // Check that we're not leaking registers by comparing the
  // state of the stack + available registers with the set of
  // all available registers.

  // Call this between opcodes.
  void performRegisterLeakCheck() {
    BaseRegAlloc::LeakCheck check(ra);
    for (auto& item : stk_) {
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
#  ifdef ENABLE_WASM_SIMD
        case Stk::RegisterV128:
          check.addKnownV128(item.v128reg());
          break;
#  endif
        case Stk::RegisterRef:
          check.addKnownRef(item.refReg());
          break;
        default:
          break;
      }
    }
  }

  void assertStackInvariants() const {
    if (deadCode_) {
      // Nonlocal control flow can pass values in stack locations in a way that
      // isn't accounted for by the value stack.  In dead code, which occurs
      // after unconditional non-local control flow, there is no invariant to
      // assert.
      return;
    }
    size_t size = 0;
    for (const Stk& v : stk_) {
      switch (v.kind()) {
        case Stk::MemRef:
          size += BaseStackFrame::StackSizeOfPtr;
          break;
        case Stk::MemI32:
          size += BaseStackFrame::StackSizeOfPtr;
          break;
        case Stk::MemI64:
          size += BaseStackFrame::StackSizeOfInt64;
          break;
        case Stk::MemF64:
          size += BaseStackFrame::StackSizeOfDouble;
          break;
        case Stk::MemF32:
          size += BaseStackFrame::StackSizeOfFloat;
          break;
#  ifdef ENABLE_WASM_SIMD
        case Stk::MemV128:
          size += BaseStackFrame::StackSizeOfV128;
          break;
#  endif
        default:
          MOZ_ASSERT(!v.isMem());
          break;
      }
    }
    MOZ_ASSERT(size == fr.dynamicHeight());
  }

#endif

  ////////////////////////////////////////////////////////////
  //
  // Control stack

  void initControl(Control& item, ResultType params) {
    // Make sure the constructor was run properly
    MOZ_ASSERT(!item.stackHeight.isValid() && item.stackSize == UINT32_MAX);

    uint32_t paramCount = deadCode_ ? 0 : params.length();
    uint32_t stackParamSize = stackConsumed(paramCount);
    item.stackHeight = fr.stackResultsBase(stackParamSize);
    item.stackSize = stk_.length() - paramCount;
    item.deadOnArrival = deadCode_;
    item.bceSafeOnEntry = bceSafe_;
  }

  Control& controlItem() { return iter_.controlItem(); }

  Control& controlItem(uint32_t relativeDepth) {
    return iter_.controlItem(relativeDepth);
  }

  Control& controlOutermost() { return iter_.controlOutermost(); }

  ////////////////////////////////////////////////////////////
  //
  // Labels

  void insertBreakablePoint(CallSiteDesc::Kind kind) {
    fr.loadTlsPtr(WasmTlsReg);
    masm.nopPatchableToCall(CallSiteDesc(iter_.lastOpcodeOffset(), kind));
  }

  //////////////////////////////////////////////////////////////////////
  //
  // Function prologue and epilogue.

  [[nodiscard]] bool beginFunction() {
    JitSpew(JitSpew_Codegen, "# ========================================");
    JitSpew(JitSpew_Codegen, "# Emitting wasm baseline code");
    JitSpew(JitSpew_Codegen,
            "# beginFunction: start of function prologue for index %d",
            (int)func_.index);

    // Make a start on the stackmap for this function.  Inspect the args so
    // as to determine which of them are both in-memory and pointer-typed, and
    // add entries to machineStackTracker as appropriate.

    ArgTypeVector args(funcType());
    size_t inboundStackArgBytes = StackArgAreaSizeUnaligned(args);
    MOZ_ASSERT(inboundStackArgBytes % sizeof(void*) == 0);
    stackMapGenerator_.numStackArgWords = inboundStackArgBytes / sizeof(void*);

    MOZ_ASSERT(stackMapGenerator_.machineStackTracker.length() == 0);
    if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
            stackMapGenerator_.numStackArgWords)) {
      return false;
    }

    // Identify GC-managed pointers passed on the stack.
    for (WasmABIArgIter i(args); !i.done(); i++) {
      ABIArg argLoc = *i;
      if (argLoc.kind() == ABIArg::Stack &&
          args[i.index()] == MIRType::RefOrNull) {
        uint32_t offset = argLoc.offsetFromArgBase();
        MOZ_ASSERT(offset < inboundStackArgBytes);
        MOZ_ASSERT(offset % sizeof(void*) == 0);
        stackMapGenerator_.machineStackTracker.setGCPointer(offset /
                                                            sizeof(void*));
      }
    }

    GenerateFunctionPrologue(masm, *moduleEnv_.funcs[func_.index].typeId,
                             compilerEnv_.mode() == CompileMode::Tier1
                                 ? Some(func_.index)
                                 : Nothing(),
                             &offsets_);

    // GenerateFunctionPrologue pushes exactly one wasm::Frame's worth of
    // stuff, and none of the values are GC pointers.  Hence:
    if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
            sizeof(Frame) / sizeof(void*))) {
      return false;
    }

    // Initialize DebugFrame fields before the stack overflow trap so that
    // we have the invariant that all observable Frames in a debugEnabled
    // Module have valid DebugFrames.
    if (compilerEnv_.debugEnabled()) {
#ifdef JS_CODEGEN_ARM64
      static_assert(DebugFrame::offsetOfFrame() % WasmStackAlignment == 0,
                    "aligned");
#endif
      masm.reserveStack(DebugFrame::offsetOfFrame());
      if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
              DebugFrame::offsetOfFrame() / sizeof(void*))) {
        return false;
      }

      masm.store32(
          Imm32(func_.index),
          Address(masm.getStackPointer(), DebugFrame::offsetOfFuncIndex()));
      masm.store32(Imm32(0), Address(masm.getStackPointer(),
                                     DebugFrame::offsetOfFlags()));

      // No need to initialize cachedReturnJSValue_ or any ref-typed spilled
      // register results, as they are traced if and only if a corresponding
      // flag (hasCachedReturnJSValue or hasSpilledRefRegisterResult) is set.
    }

    // Generate a stack-overflow check and its associated stackmap.

    fr.checkStack(ABINonArgReg0, BytecodeOffset(func_.lineOrBytecode));

    ExitStubMapVector extras;
    if (!stackMapGenerator_.generateStackmapEntriesForTrapExit(args, &extras)) {
      return false;
    }
    if (!createStackMap("stack check", extras, masm.currentOffset())) {
      return false;
    }

    size_t reservedBytes = fr.fixedAllocSize() - masm.framePushed();
    MOZ_ASSERT(0 == (reservedBytes % sizeof(void*)));

    masm.reserveStack(reservedBytes);
    fr.onFixedStackAllocated();
    if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
            reservedBytes / sizeof(void*))) {
      return false;
    }

    // Locals are stack allocated.  Mark ref-typed ones in the stackmap
    // accordingly.
    for (const Local& l : localInfo_) {
      // Locals that are stack arguments were already added to the stackmap
      // before pushing the frame.
      if (l.type == MIRType::RefOrNull && !l.isStackArgument()) {
        uint32_t offs = fr.localOffsetFromSp(l);
        MOZ_ASSERT(0 == (offs % sizeof(void*)));
        stackMapGenerator_.machineStackTracker.setGCPointer(offs /
                                                            sizeof(void*));
      }
    }

    // Copy arguments from registers to stack.
    for (WasmABIArgIter i(args); !i.done(); i++) {
      if (args.isSyntheticStackResultPointerArg(i.index())) {
        // If there are stack results and the pointer to stack results
        // was passed in a register, store it to the stack.
        if (i->argInRegister()) {
          fr.storeIncomingStackResultAreaPtr(RegPtr(i->gpr()));
        }
        // If we're in a debug frame, copy the stack result pointer arg
        // to a well-known place.
        if (compilerEnv_.debugEnabled()) {
          Register target = ABINonArgReturnReg0;
          fr.loadIncomingStackResultAreaPtr(RegPtr(target));
          size_t debugFrameOffset =
              masm.framePushed() - DebugFrame::offsetOfFrame();
          size_t debugStackResultsPointerOffset =
              debugFrameOffset + DebugFrame::offsetOfStackResultsPointer();
          masm.storePtr(target, Address(masm.getStackPointer(),
                                        debugStackResultsPointerOffset));
        }
        continue;
      }
      if (!i->argInRegister()) {
        continue;
      }
      Local& l = localInfo_[args.naturalIndex(i.index())];
      switch (i.mirType()) {
        case MIRType::Int32:
          fr.storeLocalI32(RegI32(i->gpr()), l);
          break;
        case MIRType::Int64:
          fr.storeLocalI64(RegI64(i->gpr64()), l);
          break;
        case MIRType::RefOrNull: {
          DebugOnly<uint32_t> offs = fr.localOffsetFromSp(l);
          MOZ_ASSERT(0 == (offs % sizeof(void*)));
          fr.storeLocalRef(RegRef(i->gpr()), l);
          // We should have just visited this local in the preceding loop.
          MOZ_ASSERT(stackMapGenerator_.machineStackTracker.isGCPointer(
              offs / sizeof(void*)));
          break;
        }
        case MIRType::Double:
          fr.storeLocalF64(RegF64(i->fpu()), l);
          break;
        case MIRType::Float32:
          fr.storeLocalF32(RegF32(i->fpu()), l);
          break;
#ifdef ENABLE_WASM_SIMD
        case MIRType::Simd128:
          fr.storeLocalV128(RegV128(i->fpu()), l);
          break;
#endif
        default:
          MOZ_CRASH("Function argument type");
      }
    }

    fr.zeroLocals(&ra);
    fr.storeTlsPtr(WasmTlsReg);

    if (compilerEnv_.debugEnabled()) {
      insertBreakablePoint(CallSiteDesc::EnterFrame);
      if (!createStackMap("debug: enter-frame breakpoint")) {
        return false;
      }
    }

    JitSpew(JitSpew_Codegen,
            "# beginFunction: enter body with masm.framePushed = %u",
            masm.framePushed());
    MOZ_ASSERT(stackMapGenerator_.framePushedAtEntryToBody.isNothing());
    stackMapGenerator_.framePushedAtEntryToBody.emplace(masm.framePushed());

    return true;
  }

  void popStackReturnValues(const ResultType& resultType) {
    uint32_t bytes = ABIResultIter::MeasureStackBytes(resultType);
    if (bytes == 0) {
      return;
    }
    Register target = ABINonArgReturnReg0;
    Register temp = ABINonArgReturnReg1;
    fr.loadIncomingStackResultAreaPtr(RegPtr(target));
    fr.popStackResultsToMemory(target, bytes, temp);
  }

  void saveRegisterReturnValues(const ResultType& resultType) {
    MOZ_ASSERT(compilerEnv_.debugEnabled());
    size_t debugFrameOffset = masm.framePushed() - DebugFrame::offsetOfFrame();
    size_t registerResultIdx = 0;
    for (ABIResultIter i(resultType); !i.done(); i.next()) {
      const ABIResult result = i.cur();
      if (!result.inRegister()) {
#ifdef DEBUG
        for (i.next(); !i.done(); i.next()) {
          MOZ_ASSERT(!i.cur().inRegister());
        }
#endif
        break;
      }

      size_t resultOffset =
          DebugFrame::offsetOfRegisterResult(registerResultIdx);
      Address dest(masm.getStackPointer(), debugFrameOffset + resultOffset);
      switch (result.type().kind()) {
        case ValType::I32:
          masm.store32(RegI32(result.gpr()), dest);
          break;
        case ValType::I64:
          masm.store64(RegI64(result.gpr64()), dest);
          break;
        case ValType::F64:
          masm.storeDouble(RegF64(result.fpr()), dest);
          break;
        case ValType::F32:
          masm.storeFloat32(RegF32(result.fpr()), dest);
          break;
        case ValType::Rtt:
        case ValType::Ref: {
          uint32_t flag =
              DebugFrame::hasSpilledRegisterRefResultBitMask(registerResultIdx);
          // Tell Instance::traceFrame that we have a pointer to trace.
          masm.or32(Imm32(flag),
                    Address(masm.getStackPointer(),
                            debugFrameOffset + DebugFrame::offsetOfFlags()));
          masm.storePtr(RegRef(result.gpr()), dest);
          break;
        }
        case ValType::V128:
#ifdef ENABLE_WASM_SIMD
          masm.storeUnalignedSimd128(RegV128(result.fpr()), dest);
          break;
#else
          MOZ_CRASH("No SIMD support");
#endif
      }
      registerResultIdx++;
    }
  }

  void restoreRegisterReturnValues(const ResultType& resultType) {
    MOZ_ASSERT(compilerEnv_.debugEnabled());
    size_t debugFrameOffset = masm.framePushed() - DebugFrame::offsetOfFrame();
    size_t registerResultIdx = 0;
    for (ABIResultIter i(resultType); !i.done(); i.next()) {
      const ABIResult result = i.cur();
      if (!result.inRegister()) {
#ifdef DEBUG
        for (i.next(); !i.done(); i.next()) {
          MOZ_ASSERT(!i.cur().inRegister());
        }
#endif
        break;
      }
      size_t resultOffset =
          DebugFrame::offsetOfRegisterResult(registerResultIdx++);
      Address src(masm.getStackPointer(), debugFrameOffset + resultOffset);
      switch (result.type().kind()) {
        case ValType::I32:
          masm.load32(src, RegI32(result.gpr()));
          break;
        case ValType::I64:
          masm.load64(src, RegI64(result.gpr64()));
          break;
        case ValType::F64:
          masm.loadDouble(src, RegF64(result.fpr()));
          break;
        case ValType::F32:
          masm.loadFloat32(src, RegF32(result.fpr()));
          break;
        case ValType::Rtt:
        case ValType::Ref:
          masm.loadPtr(src, RegRef(result.gpr()));
          break;
        case ValType::V128:
#ifdef ENABLE_WASM_SIMD
          masm.loadUnalignedSimd128(src, RegV128(result.fpr()));
          break;
#else
          MOZ_CRASH("No SIMD support");
#endif
      }
    }
  }

  [[nodiscard]] bool endFunction() {
    JitSpew(JitSpew_Codegen, "# endFunction: start of function epilogue");

    // Always branch to returnLabel_.
    masm.breakpoint();

    // Patch the add in the prologue so that it checks against the correct
    // frame size. Flush the constant pool in case it needs to be patched.
    masm.flush();

    // Precondition for patching.
    if (masm.oom()) {
      return false;
    }

    fr.patchCheckStack();

    masm.bind(&returnLabel_);

    ResultType resultType(ResultType::Vector(funcType().results()));

    popStackReturnValues(resultType);

    if (compilerEnv_.debugEnabled()) {
      // Store and reload the return value from DebugFrame::return so that
      // it can be clobbered, and/or modified by the debug trap.
      saveRegisterReturnValues(resultType);
      insertBreakablePoint(CallSiteDesc::Breakpoint);
      if (!createStackMap("debug: return-point breakpoint")) {
        return false;
      }
      insertBreakablePoint(CallSiteDesc::LeaveFrame);
      if (!createStackMap("debug: leave-frame breakpoint")) {
        return false;
      }
      restoreRegisterReturnValues(resultType);
    }

    // To satisy Tls extent invariant we need to reload WasmTlsReg because
    // baseline can clobber it.
    fr.loadTlsPtr(WasmTlsReg);
    GenerateFunctionEpilogue(masm, fr.fixedAllocSize(), &offsets_);

#if defined(JS_ION_PERF)
    // FIXME - profiling code missing.  No bug for this.

    // Note the end of the inline code and start of the OOL code.
    // gen->perfSpewer().noteEndInlineCode(masm);
#endif

    JitSpew(JitSpew_Codegen, "# endFunction: end of function epilogue");
    JitSpew(JitSpew_Codegen, "# endFunction: start of OOL code");
    if (!generateOutOfLineCode()) {
      return false;
    }

    offsets_.end = masm.currentOffset();

    if (!fr.checkStackHeight()) {
      return false;
    }

    JitSpew(JitSpew_Codegen, "# endFunction: end of OOL code for index %d",
            (int)func_.index);
    return !masm.oom();
  }

  //////////////////////////////////////////////////////////////////////
  //
  // Calls.

  struct FunctionCall {
    explicit FunctionCall(uint32_t lineOrBytecode)
        : lineOrBytecode(lineOrBytecode),
          isInterModule(false),
          usesSystemAbi(false),
#ifdef JS_CODEGEN_ARM
          hardFP(true),
#endif
          frameAlignAdjustment(0),
          stackArgAreaSize(0) {
    }

    uint32_t lineOrBytecode;
    WasmABIArgGenerator abi;
    bool isInterModule;
    bool usesSystemAbi;
#ifdef JS_CODEGEN_ARM
    bool hardFP;
#endif
    size_t frameAlignAdjustment;
    size_t stackArgAreaSize;
  };

  void beginCall(FunctionCall& call, UseABI useABI, InterModule interModule) {
    MOZ_ASSERT_IF(useABI == UseABI::Builtin, interModule == InterModule::False);

    call.isInterModule = interModule == InterModule::True;
    call.usesSystemAbi = useABI == UseABI::System;

    if (call.usesSystemAbi) {
      // Call-outs need to use the appropriate system ABI.
#if defined(JS_CODEGEN_ARM)
      call.hardFP = UseHardFpABI();
      call.abi.setUseHardFp(call.hardFP);
#elif defined(JS_CODEGEN_MIPS32)
      call.abi.enforceO32ABI();
#endif
    } else {
#if defined(JS_CODEGEN_ARM)
      MOZ_ASSERT(call.hardFP,
                 "All private ABIs pass FP arguments in registers");
#endif
    }

    // Use masm.framePushed() because the value we want here does not depend
    // on the height of the frame's stack area, but the actual size of the
    // allocated frame.
    call.frameAlignAdjustment = ComputeByteAlignment(
        masm.framePushed() + sizeof(Frame), JitStackAlignment);
  }

  void endCall(FunctionCall& call, size_t stackSpace) {
    size_t adjustment = call.stackArgAreaSize + call.frameAlignAdjustment;
    fr.freeArgAreaAndPopBytes(adjustment, stackSpace);

    MOZ_ASSERT(
        stackMapGenerator_.framePushedExcludingOutboundCallArgs.isSome());
    stackMapGenerator_.framePushedExcludingOutboundCallArgs.reset();

    if (call.isInterModule) {
      fr.loadTlsPtr(WasmTlsReg);
      masm.loadWasmPinnedRegsFromTls();
      masm.switchToWasmTlsRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
    } else if (call.usesSystemAbi) {
      // On x86 there are no pinned registers, so don't waste time
      // reloading the Tls.
#ifndef JS_CODEGEN_X86
      fr.loadTlsPtr(WasmTlsReg);
      masm.loadWasmPinnedRegsFromTls();
#endif
    }
  }

  void startCallArgs(size_t stackArgAreaSizeUnaligned, FunctionCall* call) {
    size_t stackArgAreaSizeAligned =
        AlignStackArgAreaSize(stackArgAreaSizeUnaligned);
    MOZ_ASSERT(stackArgAreaSizeUnaligned <= stackArgAreaSizeAligned);

    // Record the masm.framePushed() value at this point, before we push args
    // for the call, but including the alignment space placed above the args.
    // This defines the lower limit of the stackmap that will be created for
    // this call.
    MOZ_ASSERT(
        stackMapGenerator_.framePushedExcludingOutboundCallArgs.isNothing());
    stackMapGenerator_.framePushedExcludingOutboundCallArgs.emplace(
        // However much we've pushed so far
        masm.framePushed() +
        // Extra space we'll push to get the frame aligned
        call->frameAlignAdjustment +
        // Extra space we'll push to get the outbound arg area 16-aligned
        (stackArgAreaSizeAligned - stackArgAreaSizeUnaligned));

    call->stackArgAreaSize = stackArgAreaSizeAligned;

    size_t adjustment = call->stackArgAreaSize + call->frameAlignAdjustment;
    fr.allocArgArea(adjustment);
  }

  ABIArg reservePointerArgument(FunctionCall* call) {
    return call->abi.next(MIRType::Pointer);
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
  // Notably, since next() is so expensive, StackArgAreaSizeUnaligned()
  // becomes expensive too.
  //
  // Somehow there could be a trick here where the sequence of argument types
  // (read from the input stream) leads to a cached entry for
  // StackArgAreaSizeUnaligned() and for how to pass arguments...
  //
  // But at least we could reduce the cost of StackArgAreaSizeUnaligned() by
  // first reading the argument types into a (reusable) vector, then we have
  // the outgoing size at low cost, and then we can pass args based on the
  // info we read.

  void passArg(ValType type, const Stk& arg, FunctionCall* call) {
    switch (type.kind()) {
      case ValType::I32: {
        ABIArg argLoc = call->abi.next(MIRType::Int32);
        if (argLoc.kind() == ABIArg::Stack) {
          ScratchI32 scratch(*this);
          loadI32(arg, scratch);
          masm.store32(scratch, Address(masm.getStackPointer(),
                                        argLoc.offsetFromArgBase()));
        } else {
          loadI32(arg, RegI32(argLoc.gpr()));
        }
        break;
      }
      case ValType::I64: {
        ABIArg argLoc = call->abi.next(MIRType::Int64);
        if (argLoc.kind() == ABIArg::Stack) {
          ScratchI32 scratch(*this);
#ifdef JS_PUNBOX64
          loadI64(arg, fromI32(scratch));
          masm.storePtr(scratch, Address(masm.getStackPointer(),
                                         argLoc.offsetFromArgBase()));
#else
          loadI64Low(arg, scratch);
          masm.store32(scratch, LowWord(Address(masm.getStackPointer(),
                                                argLoc.offsetFromArgBase())));
          loadI64High(arg, scratch);
          masm.store32(scratch, HighWord(Address(masm.getStackPointer(),
                                                 argLoc.offsetFromArgBase())));
#endif
        } else {
          loadI64(arg, RegI64(argLoc.gpr64()));
        }
        break;
      }
      case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
        ABIArg argLoc = call->abi.next(MIRType::Simd128);
        switch (argLoc.kind()) {
          case ABIArg::Stack: {
            ScratchV128 scratch(*this);
            loadV128(arg, scratch);
            masm.storeUnalignedSimd128(
                (RegV128)scratch,
                Address(masm.getStackPointer(), argLoc.offsetFromArgBase()));
            break;
          }
          case ABIArg::GPR: {
            MOZ_CRASH("Unexpected parameter passing discipline");
          }
          case ABIArg::FPU: {
            loadV128(arg, RegV128(argLoc.fpu()));
            break;
          }
#  if defined(JS_CODEGEN_REGISTER_PAIR)
          case ABIArg::GPR_PAIR: {
            MOZ_CRASH("Unexpected parameter passing discipline");
          }
#  endif
          case ABIArg::Uninitialized:
            MOZ_CRASH("Uninitialized ABIArg kind");
        }
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
      }
      case ValType::F64: {
        ABIArg argLoc = call->abi.next(MIRType::Double);
        switch (argLoc.kind()) {
          case ABIArg::Stack: {
            ScratchF64 scratch(*this);
            loadF64(arg, scratch);
            masm.storeDouble(scratch, Address(masm.getStackPointer(),
                                              argLoc.offsetFromArgBase()));
            break;
          }
#if defined(JS_CODEGEN_REGISTER_PAIR)
          case ABIArg::GPR_PAIR: {
#  if defined(JS_CODEGEN_ARM)
            ScratchF64 scratch(*this);
            loadF64(arg, scratch);
            masm.ma_vxfer(scratch, argLoc.evenGpr(), argLoc.oddGpr());
            break;
#  elif defined(JS_CODEGEN_MIPS32)
            ScratchF64 scratch(*this);
            loadF64(arg, scratch);
            MOZ_ASSERT(MOZ_LITTLE_ENDIAN());
            masm.moveFromDoubleLo(scratch, argLoc.evenGpr());
            masm.moveFromDoubleHi(scratch, argLoc.oddGpr());
            break;
#  else
            MOZ_CRASH("BaseCompiler platform hook: passArg F64 pair");
#  endif
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
        ABIArg argLoc = call->abi.next(MIRType::Float32);
        switch (argLoc.kind()) {
          case ABIArg::Stack: {
            ScratchF32 scratch(*this);
            loadF32(arg, scratch);
            masm.storeFloat32(scratch, Address(masm.getStackPointer(),
                                               argLoc.offsetFromArgBase()));
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
      case ValType::Rtt:
      case ValType::Ref: {
        ABIArg argLoc = call->abi.next(MIRType::RefOrNull);
        if (argLoc.kind() == ABIArg::Stack) {
          ScratchRef scratch(*this);
          loadRef(arg, scratch);
          masm.storePtr(scratch, Address(masm.getStackPointer(),
                                         argLoc.offsetFromArgBase()));
        } else {
          loadRef(arg, RegRef(argLoc.gpr()));
        }
        break;
      }
    }
  }

  CodeOffset callDefinition(uint32_t funcIndex, const FunctionCall& call) {
    CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Func);
    return masm.call(desc, funcIndex);
  }

  CodeOffset callSymbolic(SymbolicAddress callee, const FunctionCall& call) {
    CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Symbolic);
    return masm.call(desc, callee);
  }

  // Precondition: sync()

  CodeOffset callIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                          const Stk& indexVal, const FunctionCall& call) {
    const TypeIdDesc& funcTypeId = moduleEnv_.typeIds[funcTypeIndex];
    MOZ_ASSERT(funcTypeId.kind() != TypeIdDescKind::None);

    const TableDesc& table = moduleEnv_.tables[tableIndex];

    loadI32(indexVal, RegI32(WasmTableCallIndexReg));

    CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Dynamic);
    CalleeDesc callee = CalleeDesc::wasmTable(table, funcTypeId);
    return masm.wasmCallIndirect(desc, callee, NeedsBoundsCheck(true));
  }

  // Precondition: sync()

  CodeOffset callImport(unsigned globalDataOffset, const FunctionCall& call) {
    CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Dynamic);
    CalleeDesc callee = CalleeDesc::import(globalDataOffset);
    return masm.wasmCallImport(desc, callee);
  }

  CodeOffset builtinCall(SymbolicAddress builtin, const FunctionCall& call) {
    return callSymbolic(builtin, call);
  }

  CodeOffset builtinInstanceMethodCall(const SymbolicAddressSignature& builtin,
                                       const ABIArg& instanceArg,
                                       const FunctionCall& call) {
    // Builtin method calls assume the TLS register has been set.
    fr.loadTlsPtr(WasmTlsReg);

    CallSiteDesc desc(call.lineOrBytecode, CallSiteDesc::Symbolic);
    return masm.wasmCallBuiltinInstanceMethod(
        desc, instanceArg, builtin.identity, builtin.failureMode);
  }

  [[nodiscard]] bool pushCallResults(const FunctionCall& call, ResultType type,
                                     const StackResultsLoc& loc) {
#if defined(JS_CODEGEN_ARM)
    // pushResults currently bypasses special case code in captureReturnedFxx()
    // that converts GPR results to FPR results for systemABI+softFP.  If we
    // ever start using that combination for calls we need more code.  This
    // assert is stronger than we need - we only care about results in return
    // registers - but that's OK.
    MOZ_ASSERT(!call.usesSystemAbi || call.hardFP);
#endif
    return pushResults(type, fr.stackResultsBase(loc.bytes()));
  }

  //////////////////////////////////////////////////////////////////////
  //
  // Sundry low-level code generators.

  // The compiler depends on moveImm32() clearing the high bits of a 64-bit
  // register on 64-bit systems except MIPS64 where high bits are sign extended
  // from lower bits, see doc block "64-bit GPRs carrying 32-bit values" in
  // MacroAssembler.h.

  void moveImm32(int32_t v, RegI32 dest) { masm.move32(Imm32(v), dest); }

  void moveImm64(int64_t v, RegI64 dest) { masm.move64(Imm64(v), dest); }

  void moveImmRef(intptr_t v, RegRef dest) { masm.movePtr(ImmWord(v), dest); }

  [[nodiscard]] bool addInterruptCheck() {
    ScratchI32 tmp(*this);
    fr.loadTlsPtr(tmp);
    masm.wasmInterruptCheck(tmp, bytecodeOffset());
    return createStackMap("addInterruptCheck");
  }

  void jumpTable(const LabelVector& labels, Label* theTable) {
    // Flush constant pools to ensure that the table is never interrupted by
    // constant pool entries.
    masm.flush();

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
    // Prevent nop sequences to appear in the jump table.
    AutoForbidNops afn(&masm);
#endif
    masm.bind(theTable);

    for (const auto& label : labels) {
      CodeLabel cl;
      masm.writeCodePointer(&cl);
      cl.target()->bind(label.offset());
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
    AutoForbidPoolsAndNops afp(&masm,
                               /* number of instructions in scope = */ 5);

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
    masm.ma_ldr(DTRAddr(scratch, DtrRegImmShift(switchValue, LSL, 2)), pc,
                Offset, Assembler::Always);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    ScratchI32 scratch(*this);
    CodeLabel tableCl;

    masm.ma_li(scratch, &tableCl);

    tableCl.target()->bind(theTable->offset());
    masm.addCodeLabel(tableCl);

    masm.branchToComputedAddress(BaseIndex(scratch, switchValue, ScalePointer));
#elif defined(JS_CODEGEN_ARM64)
    AutoForbidPoolsAndNops afp(&masm,
                               /* number of instructions in scope = */ 4);

    ScratchI32 scratch(*this);

    ARMRegister s(scratch, 64);
    ARMRegister v(switchValue, 64);
    masm.Adr(s, theTable);
    masm.Add(s, s, Operand(v, vixl::LSL, 3));
    masm.Ldr(s, MemOperand(s, 0));
    masm.Br(s);
#else
    MOZ_CRASH("BaseCompiler platform hook: tableSwitch");
#endif
  }

  RegI32 captureReturnedI32() {
    RegI32 r = RegI32(ReturnReg);
    MOZ_ASSERT(isAvailableI32(r));
    needI32(r);
#if defined(JS_CODEGEN_X64)
    if (JitOptions.spectreIndexMasking) {
      masm.movl(r, r);
    }
#endif
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
    if (call.usesSystemAbi && !call.hardFP) {
      masm.ma_vxfer(ReturnReg, r);
    }
#endif
    return r;
  }

  RegF64 captureReturnedF64(const FunctionCall& call) {
    RegF64 r = RegF64(ReturnDoubleReg);
    MOZ_ASSERT(isAvailableF64(r));
    needF64(r);
#if defined(JS_CODEGEN_ARM)
    if (call.usesSystemAbi && !call.hardFP) {
      masm.ma_vxfer(ReturnReg64.low, ReturnReg64.high, r);
    }
#endif
    return r;
  }

#ifdef ENABLE_WASM_SIMD
  RegV128 captureReturnedV128(const FunctionCall& call) {
    RegV128 r = RegV128(ReturnSimd128Reg);
    MOZ_ASSERT(isAvailableV128(r));
    needV128(r);
    return r;
  }
#endif

  RegRef captureReturnedRef() {
    RegRef r = RegRef(ReturnReg);
    MOZ_ASSERT(isAvailableRef(r));
    needRef(r);
    return r;
  }

  void checkDivideByZeroI32(RegI32 rhs) {
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

  void checkDivideSignedOverflowI32(RegI32 rhs, RegI32 srcDest, Label* done,
                                    bool zeroOnOverflow) {
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

  void checkDivideSignedOverflowI64(RegI64 rhs, RegI64 srcDest, Label* done,
                                    bool zeroOnOverflow) {
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
  void quotientI64(RegI64 rhs, RegI64 srcDest, RegI64 reserved,
                   IsUnsigned isUnsigned, bool isConst, int64_t c) {
    Label done;

    if (!isConst || c == 0) {
      checkDivideByZeroI64(rhs);
    }

    if (!isUnsigned && (!isConst || c == -1)) {
      checkDivideSignedOverflowI64(rhs, srcDest, &done, ZeroOnOverflow(false));
    }

#  if defined(JS_CODEGEN_X64)
    // The caller must set up the following situation.
    MOZ_ASSERT(srcDest.reg == rax);
    MOZ_ASSERT(reserved == specific_.rdx);
    if (isUnsigned) {
      masm.xorq(rdx, rdx);
      masm.udivq(rhs.reg);
    } else {
      masm.cqo();
      masm.idivq(rhs.reg);
    }
#  elif defined(JS_CODEGEN_MIPS64)
    if (isUnsigned) {
      masm.as_ddivu(srcDest.reg, rhs.reg);
    } else {
      masm.as_ddiv(srcDest.reg, rhs.reg);
    }
    masm.as_mflo(srcDest.reg);
#  elif defined(JS_CODEGEN_ARM64)
    ARMRegister sd(srcDest.reg, 64);
    ARMRegister r(rhs.reg, 64);
    if (isUnsigned) {
      masm.Udiv(sd, sd, r);
    } else {
      masm.Sdiv(sd, sd, r);
    }
#  else
    MOZ_CRASH("BaseCompiler platform hook: quotientI64");
#  endif
    masm.bind(&done);
  }

  void remainderI64(RegI64 rhs, RegI64 srcDest, RegI64 reserved,
                    IsUnsigned isUnsigned, bool isConst, int64_t c) {
    Label done;

    if (!isConst || c == 0) {
      checkDivideByZeroI64(rhs);
    }

    if (!isUnsigned && (!isConst || c == -1)) {
      checkDivideSignedOverflowI64(rhs, srcDest, &done, ZeroOnOverflow(true));
    }

#  if defined(JS_CODEGEN_X64)
    // The caller must set up the following situation.
    MOZ_ASSERT(srcDest.reg == rax);
    MOZ_ASSERT(reserved == specific_.rdx);

    if (isUnsigned) {
      masm.xorq(rdx, rdx);
      masm.udivq(rhs.reg);
    } else {
      masm.cqo();
      masm.idivq(rhs.reg);
    }
    masm.movq(rdx, rax);
#  elif defined(JS_CODEGEN_MIPS64)
    if (isUnsigned) {
      masm.as_ddivu(srcDest.reg, rhs.reg);
    } else {
      masm.as_ddiv(srcDest.reg, rhs.reg);
    }
    masm.as_mfhi(srcDest.reg);
#  elif defined(JS_CODEGEN_ARM64)
    MOZ_ASSERT(reserved.isInvalid());
    ARMRegister sd(srcDest.reg, 64);
    ARMRegister r(rhs.reg, 64);
    ScratchI32 temp(*this);
    ARMRegister t(temp, 64);
    if (isUnsigned) {
      masm.Udiv(t, sd, r);
    } else {
      masm.Sdiv(t, sd, r);
    }
    masm.Mul(t, t, r);
    masm.Sub(sd, sd, t);
#  else
    MOZ_CRASH("BaseCompiler platform hook: remainderI64");
#  endif
    masm.bind(&done);
  }
#endif  // RABALDR_INT_DIV_I64_CALLOUT

  RegI32 needRotate64Temp() {
#if defined(JS_CODEGEN_X86)
    return needI32();
#elif defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) ||    \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || \
    defined(JS_CODEGEN_MIPS64)
    return RegI32::Invalid();
#else
    MOZ_CRASH("BaseCompiler platform hook: needRotate64Temp");
#endif
  }

  class OutOfLineTruncateCheckF32OrF64ToI32 : public OutOfLineCode {
    AnyReg src;
    RegI32 dest;
    TruncFlags flags;
    BytecodeOffset off;

   public:
    OutOfLineTruncateCheckF32OrF64ToI32(AnyReg src, RegI32 dest,
                                        TruncFlags flags, BytecodeOffset off)
        : src(src), dest(dest), flags(flags), off(off) {}

    virtual void generate(MacroAssembler* masm) override {
      if (src.tag == AnyReg::F32) {
        masm->oolWasmTruncateCheckF32ToI32(src.f32(), dest, flags, off,
                                           rejoin());
      } else if (src.tag == AnyReg::F64) {
        masm->oolWasmTruncateCheckF64ToI32(src.f64(), dest, flags, off,
                                           rejoin());
      } else {
        MOZ_CRASH("unexpected type");
      }
    }
  };

  [[nodiscard]] bool truncateF32ToI32(RegF32 src, RegI32 dest,
                                      TruncFlags flags) {
    BytecodeOffset off = bytecodeOffset();
    OutOfLineCode* ool =
        addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI32(
            AnyReg(src), dest, flags, off));
    if (!ool) {
      return false;
    }
    bool isSaturating = flags & TRUNC_SATURATING;
    if (flags & TRUNC_UNSIGNED) {
      masm.wasmTruncateFloat32ToUInt32(src, dest, isSaturating, ool->entry());
    } else {
      masm.wasmTruncateFloat32ToInt32(src, dest, isSaturating, ool->entry());
    }
    masm.bind(ool->rejoin());
    return true;
  }

  [[nodiscard]] bool truncateF64ToI32(RegF64 src, RegI32 dest,
                                      TruncFlags flags) {
    BytecodeOffset off = bytecodeOffset();
    OutOfLineCode* ool =
        addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI32(
            AnyReg(src), dest, flags, off));
    if (!ool) {
      return false;
    }
    bool isSaturating = flags & TRUNC_SATURATING;
    if (flags & TRUNC_UNSIGNED) {
      masm.wasmTruncateDoubleToUInt32(src, dest, isSaturating, ool->entry());
    } else {
      masm.wasmTruncateDoubleToInt32(src, dest, isSaturating, ool->entry());
    }
    masm.bind(ool->rejoin());
    return true;
  }

  class OutOfLineTruncateCheckF32OrF64ToI64 : public OutOfLineCode {
    AnyReg src;
    RegI64 dest;
    TruncFlags flags;
    BytecodeOffset off;

   public:
    OutOfLineTruncateCheckF32OrF64ToI64(AnyReg src, RegI64 dest,
                                        TruncFlags flags, BytecodeOffset off)
        : src(src), dest(dest), flags(flags), off(off) {}

    virtual void generate(MacroAssembler* masm) override {
      if (src.tag == AnyReg::F32) {
        masm->oolWasmTruncateCheckF32ToI64(src.f32(), dest, flags, off,
                                           rejoin());
      } else if (src.tag == AnyReg::F64) {
        masm->oolWasmTruncateCheckF64ToI64(src.f64(), dest, flags, off,
                                           rejoin());
      } else {
        MOZ_CRASH("unexpected type");
      }
    }
  };

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT
  [[nodiscard]] RegF64 needTempForFloatingToI64(TruncFlags flags) {
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    if (flags & TRUNC_UNSIGNED) {
      return needF64();
    }
#  endif
    return RegF64::Invalid();
  }

  [[nodiscard]] bool truncateF32ToI64(RegF32 src, RegI64 dest, TruncFlags flags,
                                      RegF64 temp) {
    OutOfLineCode* ool =
        addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
            AnyReg(src), dest, flags, bytecodeOffset()));
    if (!ool) {
      return false;
    }
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

  [[nodiscard]] bool truncateF64ToI64(RegF64 src, RegI64 dest, TruncFlags flags,
                                      RegF64 temp) {
    OutOfLineCode* ool =
        addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
            AnyReg(src), dest, flags, bytecodeOffset()));
    if (!ool) {
      return false;
    }
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
#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
  RegI32 needConvertI64ToFloatTemp(ValType to, bool isUnsigned) {
    bool needs = false;
    if (to == ValType::F64) {
      needs = isUnsigned && masm.convertUInt64ToDoubleNeedsTemp();
    } else {
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
      needs = true;
#  endif
    }
    return needs ? needI32() : RegI32::Invalid();
  }

  void convertI64ToF32(RegI64 src, bool isUnsigned, RegF32 dest, RegI32 temp) {
    if (isUnsigned) {
      masm.convertUInt64ToFloat32(src, dest, temp);
    } else {
      masm.convertInt64ToFloat32(src, dest);
    }
  }

  void convertI64ToF64(RegI64 src, bool isUnsigned, RegF64 dest, RegI32 temp) {
    if (isUnsigned) {
      masm.convertUInt64ToDouble(src, dest, temp);
    } else {
      masm.convertInt64ToDouble(src, dest);
    }
  }
#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

  void cmp64Set(Assembler::Condition cond, RegI64 lhs, RegI64 rhs,
                RegI32 dest) {
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

  [[nodiscard]] bool supportsRoundInstruction(RoundingMode mode) {
    return Assembler::HasRoundInstruction(mode);
  }

  void roundF32(RoundingMode roundingMode, RegF32 f0) {
    masm.nearbyIntFloat32(roundingMode, f0, f0);
  }

  void roundF64(RoundingMode roundingMode, RegF64 f0) {
    masm.nearbyIntDouble(roundingMode, f0, f0);
  }

  //////////////////////////////////////////////////////////////////////
  //
  // Global variable access.

  Address addressOfGlobalVar(const GlobalDesc& global, RegI32 tmp) {
    uint32_t globalToTlsOffset =
        offsetof(TlsData, globalArea) + global.offset();
    fr.loadTlsPtr(tmp);
    if (global.isIndirect()) {
      masm.loadPtr(Address(tmp, globalToTlsOffset), tmp);
      return Address(tmp, 0);
    }
    return Address(tmp, globalToTlsOffset);
  }

  //////////////////////////////////////////////////////////////////////
  //
  // Heap access.

  void bceCheckLocal(MemoryAccessDesc* access, AccessCheck* check,
                     uint32_t local) {
    if (local >= sizeof(BCESet) * 8) {
      return;
    }

    uint32_t offsetGuardLimit =
        GetMaxOffsetGuardLimit(moduleEnv_.hugeMemoryEnabled());

    if ((bceSafe_ & (BCESet(1) << local)) &&
        access->offset() < offsetGuardLimit) {
      check->omitBoundsCheck = true;
    }

    // The local becomes safe even if the offset is beyond the guard limit.
    bceSafe_ |= (BCESet(1) << local);
  }

  void bceLocalIsUpdated(uint32_t local) {
    if (local >= sizeof(BCESet) * 8) {
      return;
    }

    bceSafe_ &= ~(BCESet(1) << local);
  }

  void prepareMemoryAccess(MemoryAccessDesc* access, AccessCheck* check,
                           RegI32 tls, RegI32 ptr) {
    uint32_t offsetGuardLimit =
        GetMaxOffsetGuardLimit(moduleEnv_.hugeMemoryEnabled());

    // Fold offset if necessary for further computations.
    if (access->offset() >= offsetGuardLimit ||
        (access->isAtomic() && !check->omitAlignmentCheck &&
         !check->onlyPointerAlignment)) {
      Label ok;
      masm.branchAdd32(Assembler::CarryClear, Imm32(access->offset()), ptr,
                       &ok);
      masm.wasmTrap(Trap::OutOfBounds, bytecodeOffset());
      masm.bind(&ok);
      access->clearOffset();
      check->onlyPointerAlignment = true;
    }

    // Alignment check if required.

    if (access->isAtomic() && !check->omitAlignmentCheck) {
      MOZ_ASSERT(check->onlyPointerAlignment);
      // We only care about the low pointer bits here.
      Label ok;
      masm.branchTest32(Assembler::Zero, ptr, Imm32(access->byteSize() - 1),
                        &ok);
      masm.wasmTrap(Trap::UnalignedAccess, bytecodeOffset());
      masm.bind(&ok);
    }

    // Ensure no tls if we don't need it.

    if (moduleEnv_.hugeMemoryEnabled()) {
      // We have HeapReg and no bounds checking and need load neither
      // memoryBase nor boundsCheckLimit from tls.
      MOZ_ASSERT_IF(check->omitBoundsCheck, tls.isInvalid());
    }
#ifdef JS_CODEGEN_ARM
    // We have HeapReg on ARM and don't need to load the memoryBase from tls.
    MOZ_ASSERT_IF(check->omitBoundsCheck, tls.isInvalid());
#endif

    // Bounds check if required.

    if (!moduleEnv_.hugeMemoryEnabled() && !check->omitBoundsCheck) {
      Label ok;
#ifdef JS_64BIT
      // If the bounds check uses the full 64 bits of the bounds check limit,
      // then the index must be zero-extended to 64 bits before checking and
      // wrapped back to 32-bits after Spectre masking.  (And it's important
      // that the value we end up with has flowed through the Spectre mask.)
      //
      // If the memory's max size is known to be smaller than 64K pages exactly,
      // we can use a 32-bit check and avoid extension and wrapping.
      if (!moduleEnv_.memory->boundsCheckLimitIs32Bits() &&
          ArrayBufferObject::maxBufferByteLength() >= 0x100000000) {
        // Note, ptr and ptr64 are the same register.
        RegI64 ptr64 = fromI32(ptr);

        // In principle there may be non-zero bits in the upper bits of the
        // register; clear them.
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64)
        // The canonical value is zero-extended (see comment block "64-bit GPRs
        // carrying 32-bit values" in MacroAssembler.h); we already have that.
        masm.assertCanonicalInt32(ptr);
#  else
        MOZ_CRASH("Platform code needed here");
#  endif

        // Any Spectre mitigation will appear to update the ptr64 register.
        masm.wasmBoundsCheck64(
            Assembler::Below, ptr64,
            Address(tls, offsetof(TlsData, boundsCheckLimit)), &ok);

        // Restore the value to the canonical form for a 32-bit value in a
        // 64-bit register and/or the appropriate form for further use in the
        // indexing instruction.
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64)
        // The canonical value is zero-extended; we already have that.
#  else
        MOZ_CRASH("Platform code needed here");
#  endif
      } else {
        masm.wasmBoundsCheck32(
            Assembler::Below, ptr,
            Address(tls, offsetof(TlsData, boundsCheckLimit)), &ok);
      }
#else
      masm.wasmBoundsCheck32(Assembler::Below, ptr,
                             Address(tls, offsetof(TlsData, boundsCheckLimit)),
                             &ok);
#endif
      masm.wasmTrap(Trap::OutOfBounds, bytecodeOffset());
      masm.bind(&ok);
    }
  }

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) ||      \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || \
    defined(JS_CODEGEN_MIPS64)
  BaseIndex prepareAtomicMemoryAccess(MemoryAccessDesc* access,
                                      AccessCheck* check, RegI32 tls,
                                      RegI32 ptr) {
    MOZ_ASSERT(needTlsForAccess(*check) == tls.isValid());
    prepareMemoryAccess(access, check, tls, ptr);
    return BaseIndex(HeapReg, ptr, TimesOne, access->offset());
  }
#elif defined(JS_CODEGEN_X86)
  // Some consumers depend on the address not retaining tls, as tls may be the
  // scratch register.

  Address prepareAtomicMemoryAccess(MemoryAccessDesc* access,
                                    AccessCheck* check, RegI32 tls,
                                    RegI32 ptr) {
    MOZ_ASSERT(needTlsForAccess(*check) == tls.isValid());
    prepareMemoryAccess(access, check, tls, ptr);
    masm.addPtr(Address(tls, offsetof(TlsData, memoryBase)), ptr);
    return Address(ptr, access->offset());
  }
#else
  Address prepareAtomicMemoryAccess(MemoryAccessDesc* access,
                                    AccessCheck* check, RegI32 tls,
                                    RegI32 ptr) {
    MOZ_CRASH("BaseCompiler platform hook: prepareAtomicMemoryAccess");
  }
#endif

  void computeEffectiveAddress(MemoryAccessDesc* access) {
    if (access->offset()) {
      Label ok;
      RegI32 ptr = popI32();
      masm.branchAdd32(Assembler::CarryClear, Imm32(access->offset()), ptr,
                       &ok);
      masm.wasmTrap(Trap::OutOfBounds, bytecodeOffset());
      masm.bind(&ok);
      access->clearOffset();
      pushI32(ptr);
    }
  }

  void needLoadTemps(const MemoryAccessDesc& access, RegI32* temp1,
                     RegI32* temp2, RegI32* temp3) {
#if defined(JS_CODEGEN_ARM)
    if (IsUnaligned(access)) {
      switch (access.type()) {
        case Scalar::Float64:
          *temp3 = needI32();
          [[fallthrough]];
        case Scalar::Float32:
          *temp2 = needI32();
          [[fallthrough]];
        default:
          *temp1 = needI32();
          break;
      }
    }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    *temp1 = needI32();
#endif
  }

  [[nodiscard]] bool needTlsForAccess(const AccessCheck& check) {
#if defined(JS_CODEGEN_X86)
    // x86 requires Tls for memory base
    return true;
#else
    return !moduleEnv_.hugeMemoryEnabled() && !check.omitBoundsCheck;
#endif
  }

  // ptr and dest may be the same iff dest is I32.
  // This may destroy ptr even if ptr and dest are not the same.
  [[nodiscard]] bool load(MemoryAccessDesc* access, AccessCheck* check,
                          RegI32 tls, RegI32 ptr, AnyReg dest, RegI32 temp1,
                          RegI32 temp2, RegI32 temp3) {
    prepareMemoryAccess(access, check, tls, ptr);

#if defined(JS_CODEGEN_X64)
    Operand srcAddr(HeapReg, ptr, TimesOne, access->offset());

    if (dest.tag == AnyReg::I64) {
      masm.wasmLoadI64(*access, srcAddr, dest.i64());
    } else {
      masm.wasmLoad(*access, srcAddr, dest.any());
    }
#elif defined(JS_CODEGEN_X86)
    masm.addPtr(Address(tls, offsetof(TlsData, memoryBase)), ptr);
    Operand srcAddr(ptr, access->offset());

    if (dest.tag == AnyReg::I64) {
      MOZ_ASSERT(dest.i64() == specific_.abiReturnRegI64);
      masm.wasmLoadI64(*access, srcAddr, dest.i64());
    } else {
      // For 8 bit loads, this will generate movsbl or movzbl, so
      // there's no constraint on what the output register may be.
      masm.wasmLoad(*access, srcAddr, dest.any());
    }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    if (IsUnaligned(*access)) {
      switch (dest.tag) {
        case AnyReg::I64:
          masm.wasmUnalignedLoadI64(*access, HeapReg, ptr, ptr, dest.i64(),
                                    temp1);
          break;
        case AnyReg::F32:
          masm.wasmUnalignedLoadFP(*access, HeapReg, ptr, ptr, dest.f32(),
                                   temp1, temp2, RegI32::Invalid());
          break;
        case AnyReg::F64:
          masm.wasmUnalignedLoadFP(*access, HeapReg, ptr, ptr, dest.f64(),
                                   temp1, temp2, temp3);
          break;
        case AnyReg::I32:
          masm.wasmUnalignedLoad(*access, HeapReg, ptr, ptr, dest.i32(), temp1);
          break;
        default:
          MOZ_CRASH("Unexpected type");
      }
    } else {
      if (dest.tag == AnyReg::I64) {
        masm.wasmLoadI64(*access, HeapReg, ptr, ptr, dest.i64());
      } else {
        masm.wasmLoad(*access, HeapReg, ptr, ptr, dest.any());
      }
    }
#elif defined(JS_CODEGEN_ARM)
    if (dest.tag == AnyReg::I64) {
      masm.wasmLoadI64(*access, HeapReg, ptr, ptr, dest.i64());
    } else {
      masm.wasmLoad(*access, HeapReg, ptr, ptr, dest.any());
    }
#elif defined(JS_CODEGEN_ARM64)
    if (dest.tag == AnyReg::I64) {
      masm.wasmLoadI64(*access, HeapReg, ptr, dest.i64());
    } else {
      masm.wasmLoad(*access, HeapReg, ptr, dest.any());
    }
#else
    MOZ_CRASH("BaseCompiler platform hook: load");
#endif

    return true;
  }

  RegI32 needStoreTemp(const MemoryAccessDesc& access, ValType srcType) {
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    return needI32();
#endif
    return RegI32::Invalid();
  }

  // ptr and src must not be the same register.
  // This may destroy ptr and src.
  [[nodiscard]] bool store(MemoryAccessDesc* access, AccessCheck* check,
                           RegI32 tls, RegI32 ptr, AnyReg src, RegI32 temp) {
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
    MOZ_ASSERT(temp.isInvalid());
    if (access->type() == Scalar::Int64) {
      masm.wasmStoreI64(*access, src.i64(), HeapReg, ptr, ptr);
    } else if (src.tag == AnyReg::I64) {
      masm.wasmStore(*access, AnyRegister(src.i64().low), HeapReg, ptr, ptr);
    } else {
      masm.wasmStore(*access, src.any(), HeapReg, ptr, ptr);
    }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    if (IsUnaligned(*access)) {
      switch (src.tag) {
        case AnyReg::I64:
          masm.wasmUnalignedStoreI64(*access, src.i64(), HeapReg, ptr, ptr,
                                     temp);
          break;
        case AnyReg::F32:
          masm.wasmUnalignedStoreFP(*access, src.f32(), HeapReg, ptr, ptr,
                                    temp);
          break;
        case AnyReg::F64:
          masm.wasmUnalignedStoreFP(*access, src.f64(), HeapReg, ptr, ptr,
                                    temp);
          break;
        case AnyReg::I32:
          masm.wasmUnalignedStore(*access, src.i32(), HeapReg, ptr, ptr, temp);
          break;
        default:
          MOZ_CRASH("Unexpected type");
      }
    } else {
      if (src.tag == AnyReg::I64) {
        masm.wasmStoreI64(*access, src.i64(), HeapReg, ptr, ptr);
      } else {
        masm.wasmStore(*access, src.any(), HeapReg, ptr, ptr);
      }
    }
#elif defined(JS_CODEGEN_ARM64)
    MOZ_ASSERT(temp.isInvalid());
    if (access->type() == Scalar::Int64) {
      masm.wasmStoreI64(*access, src.i64(), HeapReg, ptr);
    } else {
      masm.wasmStore(*access, src.any(), HeapReg, ptr);
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
      static_assert(Count != 0);
      for (size_t i = 0; i < allocate; ++i) {
        this->operator[](i) = bc->needI32();
      }
    }
    void maybeFree(BaseCompiler* bc) {
      for (size_t i = 0; i < Count; ++i) {
        bc->maybeFree(this->operator[](i));
      }
    }
  };

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  using AtomicRMW32Temps = Atomic32Temps<3>;
#else
  using AtomicRMW32Temps = Atomic32Temps<1>;
#endif

  template <typename T>
  void atomicRMW32(const MemoryAccessDesc& access, T srcAddr, AtomicOp op,
                   RegI32 rv, RegI32 rd, const AtomicRMW32Temps& temps) {
    switch (access.type()) {
      case Scalar::Uint8:
#ifdef JS_CODEGEN_X86
      {
        RegI32 temp = temps[0];
        // The temp, if used, must be a byte register.
        MOZ_ASSERT(temp.isInvalid());
        ScratchI8 scratch(*this);
        if (op != AtomicFetchAddOp && op != AtomicFetchSubOp) {
          temp = scratch;
        }
        masm.wasmAtomicFetchOp(access, op, rv, srcAddr, temp, rd);
        break;
      }
#endif
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        masm.wasmAtomicFetchOp(access, op, rv, srcAddr, temps[0], temps[1],
                               temps[2], rd);
#else
        masm.wasmAtomicFetchOp(access, op, rv, srcAddr, temps[0], rd);
#endif
        break;
      default: {
        MOZ_CRASH("Bad type for atomic operation");
      }
    }
  }

  // On x86, V is Address.  On other platforms, it is Register64.
  // T is BaseIndex or Address.
  template <typename T, typename V>
  void atomicRMW64(const MemoryAccessDesc& access, const T& srcAddr,
                   AtomicOp op, V value, Register64 temp, Register64 rd) {
    masm.wasmAtomicFetchOp64(access, op, value, srcAddr, temp, rd);
  }

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  using AtomicCmpXchg32Temps = Atomic32Temps<3>;
#else
  using AtomicCmpXchg32Temps = Atomic32Temps<0>;
#endif

  template <typename T>
  void atomicCmpXchg32(const MemoryAccessDesc& access, T srcAddr,
                       RegI32 rexpect, RegI32 rnew, RegI32 rd,
                       const AtomicCmpXchg32Temps& temps) {
    switch (access.type()) {
      case Scalar::Uint8:
#if defined(JS_CODEGEN_X86)
      {
        ScratchI8 scratch(*this);
        MOZ_ASSERT(rd == specific_.eax);
        if (!ra.isSingleByteI32(rnew)) {
          // The replacement value must have a byte persona.
          masm.movl(rnew, scratch);
          rnew = scratch;
        }
        masm.wasmCompareExchange(access, srcAddr, rexpect, rnew, rd);
        break;
      }
#endif
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        masm.wasmCompareExchange(access, srcAddr, rexpect, rnew, temps[0],
                                 temps[1], temps[2], rd);
#else
        masm.wasmCompareExchange(access, srcAddr, rexpect, rnew, rd);
#endif
        break;
      default:
        MOZ_CRASH("Bad type for atomic operation");
    }
  }

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  using AtomicXchg32Temps = Atomic32Temps<3>;
#else
  using AtomicXchg32Temps = Atomic32Temps<0>;
#endif

  template <typename T>
  void atomicXchg32(const MemoryAccessDesc& access, T srcAddr, RegI32 rv,
                    RegI32 rd, const AtomicXchg32Temps& temps) {
    switch (access.type()) {
      case Scalar::Uint8:
#if defined(JS_CODEGEN_X86)
      {
        if (!ra.isSingleByteI32(rd)) {
          ScratchI8 scratch(*this);
          // The output register must have a byte persona.
          masm.wasmAtomicExchange(access, srcAddr, rv, scratch);
          masm.movl(scratch, rd);
        } else {
          masm.wasmAtomicExchange(access, srcAddr, rv, rd);
        }
        break;
      }
#endif
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        masm.wasmAtomicExchange(access, srcAddr, rv, temps[0], temps[1],
                                temps[2], rd);
#else
        masm.wasmAtomicExchange(access, srcAddr, rv, rd);
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
    need2xI32(specific_.eax, specific_.edx);
    *r1 = popI32();
    *r0 = popI32ToSpecific(specific_.eax);
    *reserved = specific_.edx;
#else
    pop2xI32(r0, r1);
#endif
  }

  void pop2xI64ForMulI64(RegI64* r0, RegI64* r1, RegI32* temp,
                         RegI64* reserved) {
#if defined(JS_CODEGEN_X64)
    // r0 must be rax, and rdx will be clobbered.
    need2xI64(specific_.rax, specific_.rdx);
    *r1 = popI64();
    *r0 = popI64ToSpecific(specific_.rax);
    *reserved = specific_.rdx;
#elif defined(JS_CODEGEN_X86)
    // As for x64, though edx is part of r0.
    need2xI32(specific_.eax, specific_.edx);
    *r1 = popI64();
    *r0 = popI64ToSpecific(specific_.edx_eax);
    *temp = needI32();
#elif defined(JS_CODEGEN_MIPS64)
    pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_MIPS32)
    pop2xI64(r0, r1);
    *temp = needI32();
#elif defined(JS_CODEGEN_ARM)
    pop2xI64(r0, r1);
    *temp = needI32();
#elif defined(JS_CODEGEN_ARM64)
    pop2xI64(r0, r1);
#else
    MOZ_CRASH("BaseCompiler porting interface: pop2xI64ForMulI64");
#endif
  }

  void pop2xI64ForDivI64(RegI64* r0, RegI64* r1, RegI64* reserved) {
#if defined(JS_CODEGEN_X64)
    // r0 must be rax, and rdx will be clobbered.
    need2xI64(specific_.rax, specific_.rdx);
    *r1 = popI64();
    *r0 = popI64ToSpecific(specific_.rax);
    *reserved = specific_.rdx;
#else
    pop2xI64(r0, r1);
#endif
  }

  RegI32 popI32RhsForShift() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    // r1 must be ecx for a variable shift, unless BMI2 is available.
    if (!Assembler::HasBMI2()) {
      return popI32(specific_.ecx);
    }
#endif
    RegI32 r = popI32();
#if defined(JS_CODEGEN_ARM)
    masm.and32(Imm32(31), r);
#endif
    return r;
  }

  RegI32 popI32RhsForShiftI64() {
#if defined(JS_CODEGEN_X86)
    // A limitation in the x86 masm requires ecx here
    return popI32(specific_.ecx);
#elif defined(JS_CODEGEN_X64)
    if (!Assembler::HasBMI2()) {
      return popI32(specific_.ecx);
    }
    return popI32();
#else
    return popI32();
#endif
  }

  RegI64 popI64RhsForShift() {
#if defined(JS_CODEGEN_X86)
    // r1 must be ecx for a variable shift.
    needI32(specific_.ecx);
    return popI64ToSpecific(widenI32(specific_.ecx));
#else
#  if defined(JS_CODEGEN_X64)
    // r1 must be rcx for a variable shift, unless BMI2 is available.
    if (!Assembler::HasBMI2()) {
      needI64(specific_.rcx);
      return popI64ToSpecific(specific_.rcx);
    }
#  endif
    // No masking is necessary on 64-bit platforms, and on arm32 the masm
    // implementation masks.
    return popI64();
#endif
  }

  RegI32 popI32RhsForRotate() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    // r1 must be ecx for a variable rotate.
    return popI32(specific_.ecx);
#else
    return popI32();
#endif
  }

  RegI64 popI64RhsForRotate() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    // r1 must be ecx for a variable rotate.
    needI32(specific_.ecx);
    return popI64ToSpecific(widenI32(specific_.ecx));
#else
    return popI64();
#endif
  }

  void popI32ForSignExtendI64(RegI64* r0) {
#if defined(JS_CODEGEN_X86)
    // r0 must be edx:eax for cdq
    need2xI32(specific_.edx, specific_.eax);
    *r0 = specific_.edx_eax;
    popI32ToSpecific(specific_.eax);
#else
    *r0 = widenI32(popI32());
#endif
  }

  void popI64ForSignExtendI64(RegI64* r0) {
#if defined(JS_CODEGEN_X86)
    // r0 must be edx:eax for cdq
    need2xI32(specific_.edx, specific_.eax);
    // Low on top, high underneath
    *r0 = popI64ToSpecific(specific_.edx_eax);
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

  template <typename T>
  class PopBase {
    T rd_;

    void maybeFree(RegI32 r) { bc->maybeFree(r); }
    void maybeFree(RegI64 r) { bc->maybeFree(r); }

   protected:
    BaseCompiler* const bc;

    void setRd(T r) {
      MOZ_ASSERT(rd_.isInvalid());
      rd_ = r;
    }
    T getRd() const {
      MOZ_ASSERT(rd_.isValid());
      return rd_;
    }

   public:
    explicit PopBase(BaseCompiler* bc) : bc(bc) {}
    ~PopBase() { maybeFree(rd_); }

    // Take and clear the Rd - use this when pushing Rd.
    T takeRd() {
      MOZ_ASSERT(rd_.isValid());
      T r = rd_;
      rd_ = T::Invalid();
      return r;
    }
  };

  friend class PopAtomicCmpXchg32Regs;
  class PopAtomicCmpXchg32Regs : public PopBase<RegI32> {
    using Base = PopBase<RegI32>;
    RegI32 rexpect, rnew;
    AtomicCmpXchg32Temps temps;

   public:
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
    explicit PopAtomicCmpXchg32Regs(BaseCompiler* bc, ValType type,
                                    Scalar::Type viewType)
        : Base(bc) {
      // For cmpxchg, the expected value and the result are both in eax.
      bc->needI32(bc->specific_.eax);
      if (type == ValType::I64) {
        rnew = bc->popI64ToI32();
        rexpect = bc->popI64ToSpecificI32(bc->specific_.eax);
      } else {
        rnew = bc->popI32();
        rexpect = bc->popI32ToSpecific(bc->specific_.eax);
      }
      setRd(rexpect);
    }
    ~PopAtomicCmpXchg32Regs() { bc->freeI32(rnew); }
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
    explicit PopAtomicCmpXchg32Regs(BaseCompiler* bc, ValType type,
                                    Scalar::Type viewType)
        : Base(bc) {
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
    explicit PopAtomicCmpXchg32Regs(BaseCompiler* bc, ValType type,
                                    Scalar::Type viewType)
        : Base(bc) {
      if (type == ValType::I64) {
        rnew = bc->popI64ToI32();
        rexpect = bc->popI64ToI32();
      } else {
        rnew = bc->popI32();
        rexpect = bc->popI32();
      }
      if (Scalar::byteSize(viewType) < 4) {
        temps.allocate(bc);
      }
      setRd(bc->needI32());
    }
    ~PopAtomicCmpXchg32Regs() {
      bc->freeI32(rnew);
      bc->freeI32(rexpect);
      temps.maybeFree(bc);
    }
#else
    explicit PopAtomicCmpXchg32Regs(BaseCompiler* bc, ValType type,
                                    Scalar::Type viewType)
        : Base(bc) {
      MOZ_CRASH("BaseCompiler porting interface: PopAtomicCmpXchg32Regs");
    }
#endif

    template <typename T>
    void atomicCmpXchg32(const MemoryAccessDesc& access, T srcAddr) {
      bc->atomicCmpXchg32(access, srcAddr, rexpect, rnew, getRd(), temps);
    }
  };

  friend class PopAtomicCmpXchg64Regs;
  class PopAtomicCmpXchg64Regs : public PopBase<RegI64> {
    using Base = PopBase<RegI64>;
    RegI64 rexpect, rnew;

   public:
#ifdef JS_CODEGEN_X64
    explicit PopAtomicCmpXchg64Regs(BaseCompiler* bc) : Base(bc) {
      // For cmpxchg, the expected value and the result are both in rax.
      bc->needI64(bc->specific_.rax);
      rnew = bc->popI64();
      rexpect = bc->popI64ToSpecific(bc->specific_.rax);
      setRd(rexpect);
    }
    ~PopAtomicCmpXchg64Regs() { bc->freeI64(rnew); }
#elif defined(JS_CODEGEN_X86)
    explicit PopAtomicCmpXchg64Regs(BaseCompiler* bc) : Base(bc) {
      // For cmpxchg8b, the expected value and the result are both in
      // edx:eax, and the replacement value is in ecx:ebx.  But we can't
      // allocate ebx here, so instead we allocate a temp to hold the low
      // word of 'new'.
      bc->needI64(bc->specific_.edx_eax);
      bc->needI32(bc->specific_.ecx);

      rnew = bc->popI64ToSpecific(
          RegI64(Register64(bc->specific_.ecx, bc->needI32())));
      rexpect = bc->popI64ToSpecific(bc->specific_.edx_eax);
      setRd(rexpect);
    }
    ~PopAtomicCmpXchg64Regs() { bc->freeI64(rnew); }
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
#elif defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || \
    defined(JS_CODEGEN_MIPS64)
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
    template <typename T>
    void atomicCmpXchg64(const MemoryAccessDesc& access, T srcAddr,
                         RegI32 ebx) {
      MOZ_ASSERT(ebx == js::jit::ebx);
      bc->masm.move32(rnew.low, ebx);
      bc->masm.wasmCompareExchange64(access, srcAddr, rexpect,
                                     bc->specific_.ecx_ebx, getRd());
    }
#else
    template <typename T>
    void atomicCmpXchg64(const MemoryAccessDesc& access, T srcAddr) {
      bc->masm.wasmCompareExchange64(access, srcAddr, rexpect, rnew, getRd());
    }
#endif
  };

#ifndef JS_64BIT
  class PopAtomicLoad64Regs : public PopBase<RegI64> {
    using Base = PopBase<RegI64>;

   public:
#  if defined(JS_CODEGEN_X86)
    explicit PopAtomicLoad64Regs(BaseCompiler* bc) : Base(bc) {
      // The result is in edx:eax, and we need ecx:ebx as a temp.  But we
      // can't reserve ebx yet, so we'll accept it as an argument to the
      // operation (below).
      bc->needI32(bc->specific_.ecx);
      bc->needI64(bc->specific_.edx_eax);
      setRd(bc->specific_.edx_eax);
    }
    ~PopAtomicLoad64Regs() { bc->freeI32(bc->specific_.ecx); }
#  elif defined(JS_CODEGEN_ARM)
    explicit PopAtomicLoad64Regs(BaseCompiler* bc) : Base(bc) {
      setRd(bc->needI64Pair());
    }
#  elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    explicit PopAtomicLoad64Regs(BaseCompiler* bc) : Base(bc) {
      setRd(bc->needI64());
    }
#  else
    explicit PopAtomicLoad64Regs(BaseCompiler* bc) : Base(bc) {
      MOZ_CRASH("BaseCompiler porting interface: PopAtomicLoad64Regs");
    }
#  endif

#  ifdef JS_CODEGEN_X86
    template <typename T>
    void atomicLoad64(const MemoryAccessDesc& access, T srcAddr, RegI32 ebx) {
      MOZ_ASSERT(ebx == js::jit::ebx);
      bc->masm.wasmAtomicLoad64(access, srcAddr, bc->specific_.ecx_ebx,
                                getRd());
    }
#  else  // ARM, MIPS32
    template <typename T>
    void atomicLoad64(const MemoryAccessDesc& access, T srcAddr) {
      bc->masm.wasmAtomicLoad64(access, srcAddr, RegI64::Invalid(), getRd());
    }
#  endif
  };
#endif  // JS_64BIT

  friend class PopAtomicRMW32Regs;
  class PopAtomicRMW32Regs : public PopBase<RegI32> {
    using Base = PopBase<RegI32>;
    RegI32 rv;
    AtomicRMW32Temps temps;

   public:
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
    explicit PopAtomicRMW32Regs(BaseCompiler* bc, ValType type,
                                Scalar::Type viewType, AtomicOp op)
        : Base(bc) {
      bc->needI32(bc->specific_.eax);
      if (op == AtomicFetchAddOp || op == AtomicFetchSubOp) {
        // We use xadd, so source and destination are the same.  Using
        // eax here is overconstraining, but for byte operations on x86
        // we do need something with a byte register.
        if (type == ValType::I64) {
          rv = bc->popI64ToSpecificI32(bc->specific_.eax);
        } else {
          rv = bc->popI32ToSpecific(bc->specific_.eax);
        }
        setRd(rv);
      } else {
        // We use a cmpxchg loop.  The output must be eax; the input
        // must be in a separate register since it may be used several
        // times.
        if (type == ValType::I64) {
          rv = bc->popI64ToI32();
        } else {
          rv = bc->popI32();
        }
        setRd(bc->specific_.eax);
#  if defined(JS_CODEGEN_X86)
        // Single-byte is a special case handled very locally with
        // ScratchReg, see atomicRMW32 above.
        if (Scalar::byteSize(viewType) > 1) {
          temps.allocate(bc);
        }
#  else
        temps.allocate(bc);
#  endif
      }
    }
    ~PopAtomicRMW32Regs() {
      if (rv != bc->specific_.eax) {
        bc->freeI32(rv);
      }
      temps.maybeFree(bc);
    }
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
    explicit PopAtomicRMW32Regs(BaseCompiler* bc, ValType type,
                                Scalar::Type viewType, AtomicOp op)
        : Base(bc) {
      rv = type == ValType::I64 ? bc->popI64ToI32() : bc->popI32();
      temps.allocate(bc);
      setRd(bc->needI32());
    }
    ~PopAtomicRMW32Regs() {
      bc->freeI32(rv);
      temps.maybeFree(bc);
    }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    explicit PopAtomicRMW32Regs(BaseCompiler* bc, ValType type,
                                Scalar::Type viewType, AtomicOp op)
        : Base(bc) {
      rv = type == ValType::I64 ? bc->popI64ToI32() : bc->popI32();
      if (Scalar::byteSize(viewType) < 4) {
        temps.allocate(bc);
      }

      setRd(bc->needI32());
    }
    ~PopAtomicRMW32Regs() {
      bc->freeI32(rv);
      temps.maybeFree(bc);
    }
#else
    explicit PopAtomicRMW32Regs(BaseCompiler* bc, ValType type,
                                Scalar::Type viewType, AtomicOp op)
        : Base(bc) {
      MOZ_CRASH("BaseCompiler porting interface: PopAtomicRMW32Regs");
    }
#endif

    template <typename T>
    void atomicRMW32(const MemoryAccessDesc& access, T srcAddr, AtomicOp op) {
      bc->atomicRMW32(access, srcAddr, op, rv, getRd(), temps);
    }
  };

  friend class PopAtomicRMW64Regs;
  class PopAtomicRMW64Regs : public PopBase<RegI64> {
    using Base = PopBase<RegI64>;
#if defined(JS_CODEGEN_X64)
    AtomicOp op;
#endif
    RegI64 rv, temp;

   public:
#if defined(JS_CODEGEN_X64)
    explicit PopAtomicRMW64Regs(BaseCompiler* bc, AtomicOp op)
        : Base(bc), op(op) {
      if (op == AtomicFetchAddOp || op == AtomicFetchSubOp) {
        // We use xaddq, so input and output must be the same register.
        rv = bc->popI64();
        setRd(rv);
      } else {
        // We use a cmpxchgq loop, so the output must be rax.
        bc->needI64(bc->specific_.rax);
        rv = bc->popI64();
        temp = bc->needI64();
        setRd(bc->specific_.rax);
      }
    }
    ~PopAtomicRMW64Regs() {
      bc->maybeFree(temp);
      if (op != AtomicFetchAddOp && op != AtomicFetchSubOp) {
        bc->freeI64(rv);
      }
    }
#elif defined(JS_CODEGEN_X86)
    // We'll use cmpxchg8b, so rv must be in ecx:ebx, and rd must be
    // edx:eax.  But we can't reserve ebx here because we need it later, so
    // use a separate temp and set up ebx when we perform the operation.
    explicit PopAtomicRMW64Regs(BaseCompiler* bc, AtomicOp) : Base(bc) {
      bc->needI32(bc->specific_.ecx);
      bc->needI64(bc->specific_.edx_eax);

      temp = RegI64(Register64(bc->specific_.ecx, bc->needI32()));
      bc->popI64ToSpecific(temp);

      setRd(bc->specific_.edx_eax);
    }
    ~PopAtomicRMW64Regs() { bc->freeI64(temp); }
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
#elif defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || \
    defined(JS_CODEGEN_MIPS64)
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
    template <typename T, typename V>
    void atomicRMW64(const MemoryAccessDesc& access, T srcAddr, AtomicOp op,
                     const V& value, RegI32 ebx) {
      MOZ_ASSERT(ebx == js::jit::ebx);
      bc->atomicRMW64(access, srcAddr, op, value, bc->specific_.ecx_ebx,
                      getRd());
    }
#else
    template <typename T>
    void atomicRMW64(const MemoryAccessDesc& access, T srcAddr, AtomicOp op) {
      bc->atomicRMW64(access, srcAddr, op, rv, temp, getRd());
    }
#endif
  };

  friend class PopAtomicXchg32Regs;
  class PopAtomicXchg32Regs : public PopBase<RegI32> {
    using Base = PopBase<RegI32>;
    RegI32 rv;
    AtomicXchg32Temps temps;

   public:
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
    explicit PopAtomicXchg32Regs(BaseCompiler* bc, ValType type,
                                 Scalar::Type viewType)
        : Base(bc) {
      // The xchg instruction reuses rv as rd.
      rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
      setRd(rv);
    }
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
    explicit PopAtomicXchg32Regs(BaseCompiler* bc, ValType type,
                                 Scalar::Type viewType)
        : Base(bc) {
      rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
      setRd(bc->needI32());
    }
    ~PopAtomicXchg32Regs() { bc->freeI32(rv); }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    explicit PopAtomicXchg32Regs(BaseCompiler* bc, ValType type,
                                 Scalar::Type viewType)
        : Base(bc) {
      rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
      if (Scalar::byteSize(viewType) < 4) {
        temps.allocate(bc);
      }
      setRd(bc->needI32());
    }
    ~PopAtomicXchg32Regs() {
      temps.maybeFree(bc);
      bc->freeI32(rv);
    }
#else
    explicit PopAtomicXchg32Regs(BaseCompiler* bc, ValType type,
                                 Scalar::Type viewType)
        : Base(bc) {
      MOZ_CRASH("BaseCompiler porting interface: PopAtomicXchg32Regs");
    }
#endif

    template <typename T>
    void atomicXchg32(const MemoryAccessDesc& access, T srcAddr) {
      bc->atomicXchg32(access, srcAddr, rv, getRd(), temps);
    }
  };

  friend class PopAtomicXchg64Regs;
  class PopAtomicXchg64Regs : public PopBase<RegI64> {
    using Base = PopBase<RegI64>;
    RegI64 rv;

   public:
#if defined(JS_CODEGEN_X64)
    explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
      rv = bc->popI64();
      setRd(rv);
    }
#elif defined(JS_CODEGEN_ARM64)
    explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
      rv = bc->popI64();
      setRd(bc->needI64());
    }
    ~PopAtomicXchg64Regs() { bc->freeI64(rv); }
#elif defined(JS_CODEGEN_X86)
    // We'll use cmpxchg8b, so rv must be in ecx:ebx, and rd must be
    // edx:eax.  But we can't reserve ebx here because we need it later, so
    // use a separate temp and set up ebx when we perform the operation.
    explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
      bc->needI32(bc->specific_.ecx);
      bc->needI64(bc->specific_.edx_eax);

      rv = RegI64(Register64(bc->specific_.ecx, bc->needI32()));
      bc->popI64ToSpecific(rv);

      setRd(bc->specific_.edx_eax);
    }
    ~PopAtomicXchg64Regs() { bc->freeI64(rv); }
#elif defined(JS_CODEGEN_ARM)
    // Both rv and rd must be odd/even pairs.
    explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
      rv = bc->popI64ToSpecific(bc->needI64Pair());
      setRd(bc->needI64Pair());
    }
    ~PopAtomicXchg64Regs() { bc->freeI64(rv); }
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
      rv = bc->popI64ToSpecific(bc->needI64());
      setRd(bc->needI64());
    }
    ~PopAtomicXchg64Regs() { bc->freeI64(rv); }
#else
    explicit PopAtomicXchg64Regs(BaseCompiler* bc) : Base(bc) {
      MOZ_CRASH("BaseCompiler porting interface: xchg64");
    }
#endif

#ifdef JS_CODEGEN_X86
    template <typename T>
    void atomicXchg64(const MemoryAccessDesc& access, T srcAddr,
                      RegI32 ebx) const {
      MOZ_ASSERT(ebx == js::jit::ebx);
      bc->masm.move32(rv.low, ebx);
      bc->masm.wasmAtomicExchange64(access, srcAddr, bc->specific_.ecx_ebx,
                                    getRd());
    }
#else
    template <typename T>
    void atomicXchg64(const MemoryAccessDesc& access, T srcAddr) const {
      bc->masm.wasmAtomicExchange64(access, srcAddr, rv, getRd());
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

#ifdef ENABLE_WASM_SIMD
  void pop2xV128(RegV128* r0, RegV128* r1) {
    *r1 = popV128();
    *r0 = popV128();
  }
#endif

  void pop2xRef(RegRef* r0, RegRef* r1) {
    *r1 = popRef();
    *r0 = popRef();
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

  RegI32 popMemory32Access(MemoryAccessDesc* access, AccessCheck* check);

  void pushHeapBase();

  template <typename RegType>
  RegType pop();
  template <typename RegType>
  RegType need();

  void free(RegI32 r) { freeI32(r); }
  void free(RegI64 r) { freeI64(r); }
  void free(RegF32 r) { freeF32(r); }
  void free(RegF64 r) { freeF64(r); }
#ifdef ENABLE_WASM_SIMD
  void free(RegV128 r) { freeV128(r); }
#endif

  ////////////////////////////////////////////////////////////
  //
  // Sundry helpers.

  uint32_t readCallSiteLineOrBytecode() {
    if (!func_.callSiteLineNums.empty()) {
      return func_.callSiteLineNums[lastReadCallSite_++];
    }
    return iter_.lastOpcodeOffset();
  }

  bool done() const { return iter_.done(); }

  BytecodeOffset bytecodeOffset() const { return iter_.bytecodeOffset(); }

  void trap(Trap t) const { masm.wasmTrap(t, bytecodeOffset()); }

#ifdef ENABLE_WASM_EXCEPTIONS
  // Abstracted helper for throwing, used for throw, rethrow, and rethrowing
  // at the end of a series of catch blocks (if none matched the exception).
  [[nodiscard]] bool throwFrom(RegRef exn, uint32_t lineOrBytecode) {
    pushRef(exn);

    // ThrowException invokes a trap, and the rest is dead code.
    if (!emitInstanceCall(lineOrBytecode, SASigThrowException)) {
      return false;
    }
    freeRef(popRef());

    return true;
  }
#endif

  ////////////////////////////////////////////////////////////
  //
  // Object support.

  // This emits a GC pre-write barrier.  The pre-barrier is needed when we
  // replace a member field with a new value, and the previous field value
  // might have no other referents, and incremental GC is ongoing. The field
  // might belong to an object or be a stack slot or a register or a heap
  // allocated value.
  //
  // let obj = { field: previousValue };
  // obj.field = newValue; // previousValue must be marked with a pre-barrier.
  //
  // The `valueAddr` is the address of the location that we are about to
  // update.  This function preserves that register.

  void emitPreBarrier(RegPtr valueAddr) {
    Label skipBarrier;
    ScratchPtr scratch(*this);

    fr.loadTlsPtr(scratch);
    EmitWasmPreBarrierGuard(masm, scratch, scratch, valueAddr, &skipBarrier);

    fr.loadTlsPtr(scratch);
#ifdef JS_CODEGEN_ARM64
    // The prebarrier stub assumes the PseudoStackPointer is set up.  It is OK
    // to just move the sp to x28 here because x28 is not being used by the
    // baseline compiler and need not be saved or restored.
    MOZ_ASSERT(!GeneralRegisterSet::All().hasRegisterIndex(x28.asUnsized()));
    masm.Mov(x28, sp);
#endif
    // The prebarrier call preserves all volatile registers
    EmitWasmPreBarrierCall(masm, scratch, scratch, valueAddr);

    masm.bind(&skipBarrier);
  }

  // This frees the register `valueAddr`.

  [[nodiscard]] bool emitPostBarrierCall(RegPtr valueAddr) {
    uint32_t bytecodeOffset = iter_.lastOpcodeOffset();

    // The `valueAddr` is a raw pointer to the cell within some GC object or
    // TLS area, and we guarantee that the GC will not run while the
    // postbarrier call is active, so push a uintptr_t value.
    pushPtr(valueAddr);
    return emitInstanceCall(bytecodeOffset, SASigPostBarrier);
  }

  // Emits a store to a JS object pointer at the address valueAddr, which is
  // inside the GC cell `object`. Preserves `object` and `value`.
  [[nodiscard]] bool emitBarrieredStore(const Maybe<RegRef>& object,
                                        RegPtr valueAddr, RegRef value) {
    // TODO/AnyRef-boxing: With boxed immediates and strings, the write
    // barrier is going to have to be more complicated.
    ASSERT_ANYREF_IS_JSOBJECT;

    emitPreBarrier(valueAddr);  // Preserves valueAddr
    masm.storePtr(value, Address(valueAddr, 0));

    Label skipBarrier;
    sync();

    RegRef otherScratch = needRef();
    EmitWasmPostBarrierGuard(masm, object, otherScratch, value, &skipBarrier);
    freeRef(otherScratch);

    if (object) {
      pushRef(*object);
    }
    pushRef(value);

    // Consumes valueAddr
    if (!emitPostBarrierCall(valueAddr)) {
      return false;
    }

    // Consume all other operands as they may have been clobbered by the post
    // barrier call
    popRef(value);
    if (object) {
      popRef(*object);
    }

    masm.bind(&skipBarrier);
    return true;
  }

  ////////////////////////////////////////////////////////////
  //
  // Machinery for optimized conditional branches.
  //
  // To disable this optimization it is enough always to return false from
  // sniffConditionalControl{Cmp,Eqz}.

  struct BranchState {
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

    Label* const label;             // The target of the branch, never NULL
    const StackHeight stackHeight;  // The stack base above which to place
                                    // stack-spilled block results, if
                                    // hasBlockResults().
    const bool invertBranch;        // If true, invert the sense of the branch
    const ResultType resultType;    // The result propagated along the edges

    explicit BranchState(Label* label)
        : label(label),
          stackHeight(StackHeight::Invalid()),
          invertBranch(false),
          resultType(ResultType::Empty()) {}

    BranchState(Label* label, bool invertBranch)
        : label(label),
          stackHeight(StackHeight::Invalid()),
          invertBranch(invertBranch),
          resultType(ResultType::Empty()) {}

    BranchState(Label* label, StackHeight stackHeight, bool invertBranch,
                ResultType resultType)
        : label(label),
          stackHeight(stackHeight),
          invertBranch(invertBranch),
          resultType(resultType) {}

    bool hasBlockResults() const { return stackHeight.isValid(); }
  };

  void setLatentCompare(Assembler::Condition compareOp, ValType operandType) {
    latentOp_ = LatentOp::Compare;
    latentType_ = operandType;
    latentIntCmp_ = compareOp;
  }

  void setLatentCompare(Assembler::DoubleCondition compareOp,
                        ValType operandType) {
    latentOp_ = LatentOp::Compare;
    latentType_ = operandType;
    latentDoubleCmp_ = compareOp;
  }

  void setLatentEqz(ValType operandType) {
    latentOp_ = LatentOp::Eqz;
    latentType_ = operandType;
  }

  bool hasLatentOp() const { return latentOp_ != LatentOp::None; }

  void resetLatentOp() { latentOp_ = LatentOp::None; }

  void branchTo(Assembler::DoubleCondition c, RegF64 lhs, RegF64 rhs,
                Label* l) {
    masm.branchDouble(c, lhs, rhs, l);
  }

  void branchTo(Assembler::DoubleCondition c, RegF32 lhs, RegF32 rhs,
                Label* l) {
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

  void branchTo(Assembler::Condition c, RegRef lhs, ImmWord rhs, Label* l) {
    masm.branchPtr(c, lhs, rhs, l);
  }

  // Emit a conditional branch that optionally and optimally cleans up the CPU
  // stack before we branch.
  //
  // Cond is either Assembler::Condition or Assembler::DoubleCondition.
  //
  // Lhs is RegI32, RegI64, or RegF32, RegF64, or RegRef.
  //
  // Rhs is either the same as Lhs, or an immediate expression compatible with
  // Lhs "when applicable".

  template <typename Cond, typename Lhs, typename Rhs>
  [[nodiscard]] bool jumpConditionalWithResults(BranchState* b, Cond cond,
                                                Lhs lhs, Rhs rhs) {
    if (b->hasBlockResults()) {
      StackHeight resultsBase(0);
      if (!topBranchParams(b->resultType, &resultsBase)) {
        return false;
      }
      if (b->stackHeight != resultsBase) {
        Label notTaken;
        branchTo(b->invertBranch ? cond : Assembler::InvertCondition(cond), lhs,
                 rhs, &notTaken);

        // Shuffle stack args.
        shuffleStackResultsBeforeBranch(resultsBase, b->stackHeight,
                                        b->resultType);
        masm.jump(b->label);
        masm.bind(&notTaken);
        return true;
      }
    }

    branchTo(b->invertBranch ? Assembler::InvertCondition(cond) : cond, lhs,
             rhs, b->label);
    return true;
  }

  // sniffConditionalControl{Cmp,Eqz} may modify the latentWhatever_ state in
  // the BaseCompiler so that a subsequent conditional branch can be compiled
  // optimally.  emitBranchSetup() and emitBranchPerform() will consume that
  // state.  If the latter methods are not called because deadCode_ is true
  // then the compiler MUST instead call resetLatentOp() to reset the state.

  template <typename Cond>
  [[nodiscard]] bool sniffConditionalControlCmp(Cond compareOp,
                                                ValType operandType);
  [[nodiscard]] bool sniffConditionalControlEqz(ValType operandType);
  void emitBranchSetup(BranchState* b);
  [[nodiscard]] bool emitBranchPerform(BranchState* b);

  //////////////////////////////////////////////////////////////////////

  [[nodiscard]] bool emitBody();
  [[nodiscard]] bool emitBlock();
  [[nodiscard]] bool emitLoop();
  [[nodiscard]] bool emitIf();
  [[nodiscard]] bool emitElse();
#ifdef ENABLE_WASM_EXCEPTIONS
  // Used for common setup for catch and catch_all.
  void emitCatchSetup(LabelKind kind, Control& tryCatch,
                      const ResultType& resultType);
  // Helper function used to generate landing pad code for the special
  // case in which `delegate` jumps to a function's body block.
  [[nodiscard]] bool emitBodyDelegateThrowPad();

  [[nodiscard]] bool emitTry();
  [[nodiscard]] bool emitCatch();
  [[nodiscard]] bool emitCatchAll();
  [[nodiscard]] bool emitDelegate();
  [[nodiscard]] bool emitThrow();
  [[nodiscard]] bool emitRethrow();
#endif
  [[nodiscard]] bool emitEnd();
  [[nodiscard]] bool emitBr();
  [[nodiscard]] bool emitBrIf();
  [[nodiscard]] bool emitBrTable();
  [[nodiscard]] bool emitDrop();
  [[nodiscard]] bool emitReturn();

  enum class CalleeOnStack {
    // After the arguments to the call, there is a callee pushed onto value
    // stack.  This is only the case for callIndirect.  To get the arguments to
    // the call, emitCallArgs has to reach one element deeper into the value
    // stack, to skip the callee.
    True,

    // No callee on the stack.
    False
  };

  [[nodiscard]] bool emitCallArgs(const ValTypeVector& argTypes,
                                  const StackResultsLoc& results,
                                  FunctionCall* baselineCall,
                                  CalleeOnStack calleeOnStack);

  [[nodiscard]] bool emitCall();
  [[nodiscard]] bool emitCallIndirect();
  [[nodiscard]] bool emitUnaryMathBuiltinCall(SymbolicAddress callee,
                                              ValType operandType);
  [[nodiscard]] bool emitGetLocal();
  [[nodiscard]] bool emitSetLocal();
  [[nodiscard]] bool emitTeeLocal();
  [[nodiscard]] bool emitGetGlobal();
  [[nodiscard]] bool emitSetGlobal();
  [[nodiscard]] RegI32 maybeLoadTlsForAccess(const AccessCheck& check);
  [[nodiscard]] RegI32 maybeLoadTlsForAccess(const AccessCheck& check,
                                             RegI32 specific);
  [[nodiscard]] bool emitLoad(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool loadCommon(MemoryAccessDesc* access, AccessCheck check,
                                ValType type);
  [[nodiscard]] bool emitStore(ValType resultType, Scalar::Type viewType);
  [[nodiscard]] bool storeCommon(MemoryAccessDesc* access, AccessCheck check,
                                 ValType resultType);
  [[nodiscard]] bool emitSelect(bool typed);

  template <bool isSetLocal>
  [[nodiscard]] bool emitSetOrTeeLocal(uint32_t slot);

  [[nodiscard]] bool endBlock(ResultType type);
  [[nodiscard]] bool endIfThen(ResultType type);
  [[nodiscard]] bool endIfThenElse(ResultType type);
#ifdef ENABLE_WASM_EXCEPTIONS
  [[nodiscard]] bool endTryCatch(ResultType type);
#endif

  void doReturn(ContinuationKind kind);
  void pushReturnValueOfCall(const FunctionCall& call, MIRType type);

  [[nodiscard]] bool pushStackResultsForCall(const ResultType& type,
                                             RegPtr temp, StackResultsLoc* loc);
  void popStackResultsAfterCall(const StackResultsLoc& results,
                                uint32_t stackArgBytes);

  void emitCompareI32(Assembler::Condition compareOp, ValType compareType);
  void emitCompareI64(Assembler::Condition compareOp, ValType compareType);
  void emitCompareF32(Assembler::DoubleCondition compareOp,
                      ValType compareType);
  void emitCompareF64(Assembler::DoubleCondition compareOp,
                      ValType compareType);
  void emitCompareRef(Assembler::Condition compareOp, ValType compareType);

  template <typename CompilerType>
  CompilerType& selectCompiler();

  template <typename SourceType, typename DestType>
  void emitUnop(void (*op)(MacroAssembler& masm, SourceType rs, DestType rd));

  template <typename SourceType, typename DestType, typename TempType>
  void emitUnop(void (*op)(MacroAssembler& masm, SourceType rs, DestType rd,
                           TempType temp));

  template <typename SourceType, typename DestType, typename ImmType>
  void emitUnop(ImmType immediate,
                void (*op)(MacroAssembler&, ImmType, SourceType, DestType));

  template <typename CompilerType, typename RegType>
  void emitUnop(void (*op)(CompilerType& compiler, RegType rsd));

  template <typename RegType, typename TempType>
  void emitUnop(void (*op)(BaseCompiler& bc, RegType rsd, TempType rt),
                TempType (*getSpecializedTemp)(BaseCompiler& bc));

  template <typename CompilerType, typename RhsType, typename LhsDestType>
  void emitBinop(void (*op)(CompilerType& masm, RhsType src,
                            LhsDestType srcDest));

  template <typename RhsDestType, typename LhsType>
  void emitBinop(void (*op)(MacroAssembler& masm, RhsDestType src,
                            LhsType srcDest, RhsDestOp));

  template <typename RhsType, typename LhsDestType, typename TempType>
  void emitBinop(void (*)(MacroAssembler& masm, RhsType rs, LhsDestType rsd,
                          TempType temp));

  template <typename RhsType, typename LhsDestType, typename TempType1,
            typename TempType2>
  void emitBinop(void (*)(MacroAssembler& masm, RhsType rs, LhsDestType rsd,
                          TempType1 temp1, TempType2 temp2));

  template <typename RhsType, typename LhsDestType, typename ImmType>
  void emitBinop(ImmType immediate,
                 void (*op)(MacroAssembler&, ImmType, RhsType, LhsDestType));

  template <typename RhsType, typename LhsDestType, typename ImmType,
            typename TempType1, typename TempType2>
  void emitBinop(ImmType immediate,
                 void (*op)(MacroAssembler&, ImmType, RhsType, LhsDestType,
                            TempType1 temp1, TempType2 temp2));

  template <typename CompilerType1, typename CompilerType2, typename RegType,
            typename ImmType>
  void emitBinop(void (*op)(CompilerType1& compiler1, RegType rs, RegType rd),
                 void (*opConst)(CompilerType2& compiler2, ImmType c,
                                 RegType rd),
                 RegType (BaseCompiler::*rhsPopper)() = nullptr);

  template <typename R>
  [[nodiscard]] bool emitInstanceCallOp(const SymbolicAddressSignature& fn,
                                        R reader);

  template <typename A1, typename R>
  [[nodiscard]] bool emitInstanceCallOp(const SymbolicAddressSignature& fn,
                                        R reader);

  template <typename A1, typename A2, typename R>
  [[nodiscard]] bool emitInstanceCallOp(const SymbolicAddressSignature& fn,
                                        R reader);

  void emitMultiplyI32();
  void emitMultiplyI64();
  void emitQuotientI32();
  void emitQuotientU32();
  void emitRemainderI32();
  void emitRemainderU32();
#ifdef RABALDR_INT_DIV_I64_CALLOUT
  [[nodiscard]] bool emitDivOrModI64BuiltinCall(SymbolicAddress callee,
                                                ValType operandType);
#else
  void emitQuotientI64();
  void emitQuotientU64();
  void emitRemainderI64();
  void emitRemainderU64();
#endif
  void emitRotrI64();
  void emitRotlI64();
  void emitEqzI32();
  void emitEqzI64();
  template <TruncFlags flags>
  [[nodiscard]] bool emitTruncateF32ToI32();
  template <TruncFlags flags>
  [[nodiscard]] bool emitTruncateF64ToI32();
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
  [[nodiscard]] bool emitConvertFloatingToInt64Callout(SymbolicAddress callee,
                                                       ValType operandType,
                                                       ValType resultType);
#else
  template <TruncFlags flags>
  [[nodiscard]] bool emitTruncateF32ToI64();
  template <TruncFlags flags>
  [[nodiscard]] bool emitTruncateF64ToI64();
#endif
  void emitExtendI64_8();
  void emitExtendI64_16();
  void emitExtendI64_32();
  void emitExtendI32ToI64();
  void emitExtendU32ToI64();
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
  [[nodiscard]] bool emitConvertInt64ToFloatingCallout(SymbolicAddress callee,
                                                       ValType operandType,
                                                       ValType resultType);
#else
  void emitConvertU64ToF32();
  void emitConvertU64ToF64();
#endif
  void emitRound(RoundingMode roundingMode, ValType operandType);
  [[nodiscard]] bool emitInstanceCall(uint32_t lineOrBytecode,
                                      const SymbolicAddressSignature& builtin);
  [[nodiscard]] bool emitMemoryGrow();
  [[nodiscard]] bool emitMemorySize();

  [[nodiscard]] bool emitRefFunc();
  [[nodiscard]] bool emitRefNull();
  [[nodiscard]] bool emitRefIsNull();
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
  [[nodiscard]] bool emitRefAsNonNull();
  [[nodiscard]] bool emitBrOnNull();
#endif

  [[nodiscard]] bool emitAtomicCmpXchg(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool emitAtomicLoad(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool emitAtomicRMW(ValType type, Scalar::Type viewType,
                                   AtomicOp op);
  [[nodiscard]] bool emitAtomicStore(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool emitWait(ValType type, uint32_t byteSize);
  [[nodiscard]] bool emitWake();
  [[nodiscard]] bool emitFence();
  [[nodiscard]] bool emitAtomicXchg(ValType type, Scalar::Type viewType);
  void emitAtomicXchg64(MemoryAccessDesc* access, WantResult wantResult);
  [[nodiscard]] bool emitMemInit();
  [[nodiscard]] bool emitMemCopy();
  [[nodiscard]] bool emitMemCopyCall(uint32_t lineOrBytecode);
  [[nodiscard]] bool emitMemCopyInline();
  [[nodiscard]] bool emitTableCopy();
  [[nodiscard]] bool emitDataOrElemDrop(bool isData);
  [[nodiscard]] bool emitMemFill();
  [[nodiscard]] bool emitMemFillCall(uint32_t lineOrBytecode);
  [[nodiscard]] bool emitMemFillInline();
  [[nodiscard]] bool emitTableInit();
  [[nodiscard]] bool emitTableFill();
  [[nodiscard]] bool emitTableGet();
  [[nodiscard]] bool emitTableGrow();
  [[nodiscard]] bool emitTableSet();
  [[nodiscard]] bool emitTableSize();
  [[nodiscard]] bool emitStructNewWithRtt();
  [[nodiscard]] bool emitStructNewDefaultWithRtt();
  [[nodiscard]] bool emitStructGet(FieldExtension extension);
  [[nodiscard]] bool emitStructSet();
  [[nodiscard]] bool emitArrayNewWithRtt();
  [[nodiscard]] bool emitArrayNewDefaultWithRtt();
  [[nodiscard]] bool emitArrayGet(FieldExtension extension);
  [[nodiscard]] bool emitArraySet();
  [[nodiscard]] bool emitArrayLen();
  [[nodiscard]] bool emitRttCanon();
  [[nodiscard]] bool emitRttSub();
  [[nodiscard]] bool emitRefTest();
  [[nodiscard]] bool emitRefCast();
  [[nodiscard]] bool emitBrOnCast();

  void emitGcNullCheck(RegRef rp);
  RegPtr emitGcArrayGetData(RegRef rp);
  RegI32 emitGcArrayGetLength(RegPtr rdata, bool adjustDataPointer);
  void emitGcArrayBoundsCheck(RegI32 index, RegI32 length);
  template <typename T>
  void emitGcGet(FieldType type, FieldExtension extension, const T& src);
  template <typename T>
  void emitGcSetScalar(const T& dst, FieldType type, AnyReg value);
  [[nodiscard]] bool emitGcStructSet(RegRef object, RegPtr data,
                                     const StructField& field, AnyReg value);
  [[nodiscard]] bool emitGcArraySet(RegRef object, RegPtr data, RegI32 index,
                                    const ArrayType& array, AnyReg value);

#ifdef ENABLE_WASM_SIMD
  void emitVectorAndNot();

  [[nodiscard]] bool emitLoadSplat(Scalar::Type viewType);
  [[nodiscard]] bool emitLoadZero(Scalar::Type viewType);
  [[nodiscard]] bool emitLoadExtend(Scalar::Type viewType);
  [[nodiscard]] bool emitLoadLane(uint32_t laneSize);
  [[nodiscard]] bool emitStoreLane(uint32_t laneSize);
  [[nodiscard]] bool emitBitselect();
  [[nodiscard]] bool emitVectorShuffle();
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  [[nodiscard]] bool emitVectorShiftRightI64x2();
#  endif
#endif
};

// TODO: We want these to be inlined for sure; do we need an `inline` somewhere?

template <>
RegI32 BaseCompiler::need<RegI32>() {
  return needI32();
}
template <>
RegI64 BaseCompiler::need<RegI64>() {
  return needI64();
}
template <>
RegF32 BaseCompiler::need<RegF32>() {
  return needF32();
}
template <>
RegF64 BaseCompiler::need<RegF64>() {
  return needF64();
}

template <>
RegI32 BaseCompiler::pop<RegI32>() {
  return popI32();
}
template <>
RegI64 BaseCompiler::pop<RegI64>() {
  return popI64();
}
template <>
RegF32 BaseCompiler::pop<RegF32>() {
  return popF32();
}
template <>
RegF64 BaseCompiler::pop<RegF64>() {
  return popF64();
}

#ifdef ENABLE_WASM_SIMD
template <>
RegV128 BaseCompiler::need<RegV128>() {
  return needV128();
}
template <>
RegV128 BaseCompiler::pop<RegV128>() {
  return popV128();
}
#endif

template <>
BaseCompiler& BaseCompiler::selectCompiler<BaseCompiler>() {
  return *this;
}

template <>
MacroAssembler& BaseCompiler::selectCompiler<MacroAssembler>() {
  return masm;
}

template <typename SourceType, typename DestType>
void BaseCompiler::emitUnop(void (*op)(MacroAssembler& masm, SourceType rs,
                                       DestType rd)) {
  SourceType rs = pop<SourceType>();
  DestType rd = need<DestType>();
  op(masm, rs, rd);
  free(rs);
  push(rd);
}

// Specialize narrowing reuse.  Consumers may assume that rs.reg==rd on 64-bit
// platforms, or rs.low==rd on 32-bit platforms.
template <>
void BaseCompiler::emitUnop(void (*op)(MacroAssembler& masm, RegI64 rs,
                                       RegI32 rd)) {
  RegI64 rs = pop<RegI64>();
  RegI32 rd = fromI64(rs);
  op(masm, rs, rd);
  freeI64Except(rs, rd);
  push(rd);
}

template <typename CompilerType, typename RegType>
void BaseCompiler::emitUnop(void (*op)(CompilerType& compiler, RegType rsd)) {
  RegType rsd = pop<RegType>();
  op(selectCompiler<CompilerType>(), rsd);
  push(rsd);
}

template <typename RegType, typename TempType>
void BaseCompiler::emitUnop(void (*op)(BaseCompiler& bc, RegType rsd,
                                       TempType rt),
                            TempType (*getSpecializedTemp)(BaseCompiler& bc)) {
  RegType rsd = pop<RegType>();
  TempType temp = getSpecializedTemp(*this);
  op(*this, rsd, temp);
  maybeFree(temp);
  push(rsd);
}

template <typename SourceType, typename DestType, typename TempType>
void BaseCompiler::emitUnop(void (*op)(MacroAssembler& masm, SourceType rs,
                                       DestType rd, TempType temp)) {
  SourceType rs = pop<SourceType>();
  DestType rd = need<DestType>();
  TempType temp = need<TempType>();
  op(masm, rs, rd, temp);
  free(rs);
  free(temp);
  push(rd);
}

template <typename SourceType, typename DestType, typename ImmType>
void BaseCompiler::emitUnop(ImmType immediate,
                            void (*op)(MacroAssembler&, ImmType, SourceType,
                                       DestType)) {
  SourceType rs = pop<SourceType>();
  DestType rd = need<DestType>();
  op(masm, immediate, rs, rd);
  free(rs);
  push(rd);
}

template <typename CompilerType, typename RhsType, typename LhsDestType>
void BaseCompiler::emitBinop(void (*op)(CompilerType& masm, RhsType src,
                                        LhsDestType srcDest)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  op(selectCompiler<CompilerType>(), rs, rsd);
  free(rs);
  push(rsd);
}

template <typename RhsDestType, typename LhsType>
void BaseCompiler::emitBinop(void (*op)(MacroAssembler& masm, RhsDestType src,
                                        LhsType srcDest, RhsDestOp)) {
  RhsDestType rsd = pop<RhsDestType>();
  LhsType rs = pop<LhsType>();
  op(masm, rsd, rs, RhsDestOp::True);
  free(rs);
  push(rsd);
}

template <typename RhsType, typename LhsDestType, typename TempType>
void BaseCompiler::emitBinop(void (*op)(MacroAssembler& masm, RhsType rs,
                                        LhsDestType rsd, TempType temp)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  TempType temp = need<TempType>();
  op(masm, rs, rsd, temp);
  free(rs);
  free(temp);
  push(rsd);
}

template <typename RhsType, typename LhsDestType, typename TempType1,
          typename TempType2>
void BaseCompiler::emitBinop(void (*op)(MacroAssembler& masm, RhsType rs,
                                        LhsDestType rsd, TempType1 temp1,
                                        TempType2 temp2)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  TempType1 temp1 = need<TempType1>();
  TempType2 temp2 = need<TempType2>();
  op(masm, rs, rsd, temp1, temp2);
  free(rs);
  free(temp1);
  free(temp2);
  push(rsd);
}

template <typename RhsType, typename LhsDestType, typename ImmType>
void BaseCompiler::emitBinop(ImmType immediate,
                             void (*op)(MacroAssembler&, ImmType, RhsType,
                                        LhsDestType)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  op(masm, immediate, rs, rsd);
  free(rs);
  push(rsd);
}

template <typename RhsType, typename LhsDestType, typename ImmType,
          typename TempType1, typename TempType2>
void BaseCompiler::emitBinop(ImmType immediate,
                             void (*op)(MacroAssembler&, ImmType, RhsType,
                                        LhsDestType, TempType1 temp1,
                                        TempType2 temp2)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  TempType1 temp1 = need<TempType1>();
  TempType2 temp2 = need<TempType2>();
  op(masm, immediate, rs, rsd, temp1, temp2);
  free(rs);
  free(temp1);
  free(temp2);
  push(rsd);
}

template <typename CompilerType1, typename CompilerType2, typename RegType,
          typename ImmType>
void BaseCompiler::emitBinop(void (*op)(CompilerType1& compiler, RegType rs,
                                        RegType rsd),
                             void (*opConst)(CompilerType2& compiler, ImmType c,
                                             RegType rsd),
                             RegType (BaseCompiler::*rhsPopper)()) {
  ImmType c;
  if (popConst(&c)) {
    RegType rsd = pop<RegType>();
    opConst(selectCompiler<CompilerType2>(), c, rsd);
    push(rsd);
  } else {
    RegType rs = rhsPopper ? (this->*rhsPopper)() : pop<RegType>();
    RegType rsd = pop<RegType>();
    op(selectCompiler<CompilerType1>(), rs, rsd);
    free(rs);
    push(rsd);
  }
}

template <typename R>
[[nodiscard]] bool BaseCompiler::emitInstanceCallOp(
    const SymbolicAddressSignature& fn, R reader) {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();
  if (!reader()) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  return emitInstanceCall(lineOrBytecode, fn);
}

template <typename A1, typename R>
[[nodiscard]] bool BaseCompiler::emitInstanceCallOp(
    const SymbolicAddressSignature& fn, R reader) {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();
  A1 arg = 0;
  if (!reader(&arg)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  push(arg);
  return emitInstanceCall(lineOrBytecode, fn);
}

template <typename A1, typename A2, typename R>
[[nodiscard]] bool BaseCompiler::emitInstanceCallOp(
    const SymbolicAddressSignature& fn, R reader) {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();
  A1 arg1 = 0;
  A2 arg2 = 0;
  if (!reader(&arg1, &arg2)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  // Note order of arguments must be the same as for the reader.
  push(arg1);
  push(arg2);
  return emitInstanceCall(lineOrBytecode, fn);
}

static void AddI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.add32(rs, rsd);
}

static void AddImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.add32(Imm32(c), rsd);
}

static void SubI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.sub32(rs, rsd);
}

static void SubImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.sub32(Imm32(c), rsd);
}

static void OrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.or32(rs, rsd);
}

static void OrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.or32(Imm32(c), rsd);
}

static void AndI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.and32(rs, rsd);
}

static void AndImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.and32(Imm32(c), rsd);
}

static void XorI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.xor32(rs, rsd);
}

static void XorImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.xor32(Imm32(c), rsd);
}

static void ClzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.clz32(rsd, rsd, IsKnownNotZero(false));
}

static void CtzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.ctz32(rsd, rsd, IsKnownNotZero(false));
}

// Currently common to PopcntI32 and PopcntI64
static RegI32 PopcntTemp(BaseCompiler& bc) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  return AssemblerX86Shared::HasPOPCNT() ? RegI32::Invalid() : bc.needI32();
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  return bc.needI32();
#else
  MOZ_CRASH("BaseCompiler platform hook: PopcntTemp");
#endif
}

static void PopcntI32(BaseCompiler& bc, RegI32 rsd, RegI32 temp) {
  bc.masm.popcnt32(rsd, rsd, temp);
}

static void ShlI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.lshift32(rs, rsd);
}

static void ShlImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.lshift32(Imm32(c & 31), rsd);
}

static void ShrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rshift32Arithmetic(rs, rsd);
}

static void ShrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rshift32Arithmetic(Imm32(c & 31), rsd);
}

static void ShrUI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rshift32(rs, rsd);
}

static void ShrUImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rshift32(Imm32(c & 31), rsd);
}

static void RotlI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rotateLeft(rs, rsd, rsd);
}

static void RotlImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rotateLeft(Imm32(c & 31), rsd, rsd);
}

static void RotrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rotateRight(rs, rsd, rsd);
}

static void RotrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rotateRight(Imm32(c & 31), rsd, rsd);
}

static void EqzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.cmp32Set(Assembler::Equal, rsd, Imm32(0), rsd);
}

static void WrapI64ToI32(MacroAssembler& masm, RegI64 rs, RegI32 rd) {
  masm.move64To32(rs, rd);
}

static void AddI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.add64(rs, rsd);
}

static void AddImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.add64(Imm64(c), rsd);
}

static void SubI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.sub64(rs, rsd);
}

static void SubImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.sub64(Imm64(c), rsd);
}

static void OrI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.or64(rs, rsd);
}

static void OrImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.or64(Imm64(c), rsd);
}

static void AndI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.and64(rs, rsd);
}

static void AndImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.and64(Imm64(c), rsd);
}

static void XorI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.xor64(rs, rsd);
}

static void XorImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.xor64(Imm64(c), rsd);
}

static void ClzI64(BaseCompiler& bc, RegI64 rsd) {
  bc.masm.clz64(rsd, bc.lowPart(rsd));
  bc.maybeClearHighPart(rsd);
}

static void CtzI64(BaseCompiler& bc, RegI64 rsd) {
  bc.masm.ctz64(rsd, bc.lowPart(rsd));
  bc.maybeClearHighPart(rsd);
}

static void PopcntI64(BaseCompiler& bc, RegI64 rsd, RegI32 temp) {
  bc.masm.popcnt64(rsd, rsd, temp);
}

static void ShlI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.lshift64(bc.lowPart(rs), rsd);
}

static void ShlImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.lshift64(Imm32(c & 63), rsd);
}

static void ShrI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.rshift64Arithmetic(bc.lowPart(rs), rsd);
}

static void ShrImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.rshift64Arithmetic(Imm32(c & 63), rsd);
}

static void ShrUI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.rshift64(bc.lowPart(rs), rsd);
}

static void ShrUImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.rshift64(Imm32(c & 63), rsd);
}

static void EqzI64(MacroAssembler& masm, RegI64 rs, RegI32 rd) {
#ifdef JS_PUNBOX64
  masm.cmpPtrSet(Assembler::Equal, rs.reg, ImmWord(0), rd);
#else
  MOZ_ASSERT(rs.low == rd);
  masm.or32(rs.high, rs.low);
  masm.cmp32Set(Assembler::Equal, rs.low, Imm32(0), rd);
#endif
}

static void AddF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.addDouble(rs, rsd);
}

static void SubF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.subDouble(rs, rsd);
}

static void MulF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.mulDouble(rs, rsd);
}

static void DivF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.divDouble(rs, rsd);
}

static void MinF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F64
  ScratchF64 zero(bc.ra);
#else
  ScratchF64 zero(bc.masm);
#endif
  bc.masm.loadConstantDouble(0, zero);
  bc.masm.subDouble(zero, rsd);
  bc.masm.subDouble(zero, rs);
  bc.masm.minDouble(rs, rsd, HandleNaNSpecially(true));
}

static void MaxF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F64
  ScratchF64 zero(bc.ra);
#else
  ScratchF64 zero(bc.masm);
#endif
  bc.masm.loadConstantDouble(0, zero);
  bc.masm.subDouble(zero, rsd);
  bc.masm.subDouble(zero, rs);
  bc.masm.maxDouble(rs, rsd, HandleNaNSpecially(true));
}

static void CopysignF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd,
                        RegI64 temp0, RegI64 temp1) {
  masm.moveDoubleToGPR64(rsd, temp0);
  masm.moveDoubleToGPR64(rs, temp1);
  masm.and64(Imm64(INT64_MAX), temp0);
  masm.and64(Imm64(INT64_MIN), temp1);
  masm.or64(temp1, temp0);
  masm.moveGPR64ToDouble(temp0, rsd);
}

static void AbsF64(MacroAssembler& masm, RegF64 rsd) {
  masm.absDouble(rsd, rsd);
}

static void NegateF64(MacroAssembler& masm, RegF64 rsd) {
  masm.negateDouble(rsd);
}

static void SqrtF64(MacroAssembler& masm, RegF64 rsd) {
  masm.sqrtDouble(rsd, rsd);
}

static void AddF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.addFloat32(rs, rsd);
}

static void SubF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.subFloat32(rs, rsd);
}

static void MulF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.mulFloat32(rs, rsd);
}

static void DivF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.divFloat32(rs, rsd);
}

static void MinF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): Don't do this if one of the operands
  // is known to be a constant.
#ifdef RABALDR_SCRATCH_F32
  ScratchF32 zero(bc.ra);
#else
  ScratchF32 zero(bc.masm);
#endif
  bc.masm.loadConstantFloat32(0.f, zero);
  bc.masm.subFloat32(zero, rsd);
  bc.masm.subFloat32(zero, rs);
  bc.masm.minFloat32(rs, rsd, HandleNaNSpecially(true));
}

static void MaxF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F32
  ScratchF32 zero(bc.ra);
#else
  ScratchF32 zero(bc.masm);
#endif
  bc.masm.loadConstantFloat32(0.f, zero);
  bc.masm.subFloat32(zero, rsd);
  bc.masm.subFloat32(zero, rs);
  bc.masm.maxFloat32(rs, rsd, HandleNaNSpecially(true));
}

static void CopysignF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd,
                        RegI32 temp0, RegI32 temp1) {
  masm.moveFloat32ToGPR(rsd, temp0);
  masm.moveFloat32ToGPR(rs, temp1);
  masm.and32(Imm32(INT32_MAX), temp0);
  masm.and32(Imm32(INT32_MIN), temp1);
  masm.or32(temp1, temp0);
  masm.moveGPRToFloat32(temp0, rsd);
}

static void AbsF32(MacroAssembler& masm, RegF32 rsd) {
  masm.absFloat32(rsd, rsd);
}

static void NegateF32(MacroAssembler& masm, RegF32 rsd) {
  masm.negateFloat(rsd);
}

static void SqrtF32(MacroAssembler& masm, RegF32 rsd) {
  masm.sqrtFloat32(rsd, rsd);
}

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
static void ConvertI64ToF32(MacroAssembler& masm, RegI64 rs, RegF32 rd) {
  masm.convertInt64ToFloat32(rs, rd);
}

static void ConvertI64ToF64(MacroAssembler& masm, RegI64 rs, RegF64 rd) {
  masm.convertInt64ToDouble(rs, rd);
}
#endif

static void ReinterpretF32AsI32(MacroAssembler& masm, RegF32 rs, RegI32 rd) {
  masm.moveFloat32ToGPR(rs, rd);
}

static void ReinterpretF64AsI64(MacroAssembler& masm, RegF64 rs, RegI64 rd) {
  masm.moveDoubleToGPR64(rs, rd);
}

static void ConvertF64ToF32(MacroAssembler& masm, RegF64 rs, RegF32 rd) {
  masm.convertDoubleToFloat32(rs, rd);
}

static void ConvertI32ToF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.convertInt32ToFloat32(rs, rd);
}

static void ConvertU32ToF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.convertUInt32ToFloat32(rs, rd);
}

static void ConvertF32ToF64(MacroAssembler& masm, RegF32 rs, RegF64 rd) {
  masm.convertFloat32ToDouble(rs, rd);
}

static void ConvertI32ToF64(MacroAssembler& masm, RegI32 rs, RegF64 rd) {
  masm.convertInt32ToDouble(rs, rd);
}

static void ConvertU32ToF64(MacroAssembler& masm, RegI32 rs, RegF64 rd) {
  masm.convertUInt32ToDouble(rs, rd);
}

static void ReinterpretI32AsF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.moveGPRToFloat32(rs, rd);
}

static void ReinterpretI64AsF64(MacroAssembler& masm, RegI64 rs, RegF64 rd) {
  masm.moveGPR64ToDouble(rs, rd);
}

static void ExtendI32_8(BaseCompiler& bc, RegI32 rsd) {
#ifdef JS_CODEGEN_X86
  if (!bc.ra.isSingleByteI32(rsd)) {
    ScratchI8 scratch(bc.ra);
    bc.masm.move32(rsd, scratch);
    bc.masm.move8SignExtend(scratch, rsd);
    return;
  }
#endif
  bc.masm.move8SignExtend(rsd, rsd);
}

static void ExtendI32_16(MacroAssembler& masm, RegI32 rsd) {
  masm.move16SignExtend(rsd, rsd);
}

void BaseCompiler::emitMultiplyI32() {
  RegI32 r, rs, reserved;
  pop2xI32ForMulDivI32(&r, &rs, &reserved);
  masm.mul32(rs, r);
  maybeFree(reserved);
  freeI32(rs);
  pushI32(r);
}

void BaseCompiler::emitMultiplyI64() {
  RegI64 r, rs, reserved;
  RegI32 temp;
  pop2xI64ForMulI64(&r, &rs, &temp, &reserved);
  masm.mul64(rs, r, temp);
  maybeFree(reserved);
  maybeFree(temp);
  freeI64(rs);
  pushI64(r);
}

void BaseCompiler::emitQuotientI32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI32 r = popI32();
      Label positive;
      masm.branchTest32(Assembler::NotSigned, r, r, &positive);
      masm.add32(Imm32(c - 1), r);
      masm.bind(&positive);

      masm.rshift32Arithmetic(Imm32(power & 31), r);
      pushI32(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    pop2xI32ForMulDivI32(&r, &rs, &reserved);

    if (!isConst || c == 0) {
      checkDivideByZeroI32(rs);
    }

    Label done;
    if (!isConst || c == -1) {
      checkDivideSignedOverflowI32(rs, r, &done, ZeroOnOverflow(false));
    }
    masm.quotient32(rs, r, IsUnsigned(false));
    masm.bind(&done);

    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitQuotientU32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI32 r = popI32();
      masm.rshift32(Imm32(power & 31), r);
      pushI32(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    pop2xI32ForMulDivI32(&r, &rs, &reserved);

    if (!isConst || c == 0) {
      checkDivideByZeroI32(rs);
    }
    masm.quotient32(rs, r, IsUnsigned(true));

    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitRemainderI32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI32 r = popI32();
    RegI32 temp = needI32();
    moveI32(r, temp);

    Label positive;
    masm.branchTest32(Assembler::NotSigned, temp, temp, &positive);
    masm.add32(Imm32(c - 1), temp);
    masm.bind(&positive);

    masm.rshift32Arithmetic(Imm32(power & 31), temp);
    masm.lshift32(Imm32(power & 31), temp);
    masm.sub32(temp, r);
    freeI32(temp);

    pushI32(r);
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    pop2xI32ForMulDivI32(&r, &rs, &reserved);

    if (!isConst || c == 0) {
      checkDivideByZeroI32(rs);
    }

    Label done;
    if (!isConst || c == -1) {
      checkDivideSignedOverflowI32(rs, r, &done, ZeroOnOverflow(true));
    }
    masm.remainder32(rs, r, IsUnsigned(false));
    masm.bind(&done);

    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitRemainderU32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI32 r = popI32();
    masm.and32(Imm32(c - 1), r);
    pushI32(r);
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    pop2xI32ForMulDivI32(&r, &rs, &reserved);

    if (!isConst || c == 0) {
      checkDivideByZeroI32(rs);
    }
    masm.remainder32(rs, r, IsUnsigned(true));

    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

#ifndef RABALDR_INT_DIV_I64_CALLOUT
void BaseCompiler::emitQuotientI64() {
#  ifdef JS_64BIT
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI64 r = popI64();
      Label positive;
      masm.branchTest64(Assembler::NotSigned, r, r, RegI32::Invalid(),
                        &positive);
      masm.add64(Imm64(c - 1), r);
      masm.bind(&positive);

      masm.rshift64Arithmetic(Imm32(power & 63), r);
      pushI64(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    pop2xI64ForDivI64(&r, &rs, &reserved);
    quotientI64(rs, r, reserved, IsUnsigned(false), isConst, c);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
#  else
  MOZ_CRASH("BaseCompiler platform hook: emitQuotientI64");
#  endif
}

void BaseCompiler::emitQuotientU64() {
#  ifdef JS_64BIT
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI64 r = popI64();
      masm.rshift64(Imm32(power & 63), r);
      pushI64(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    pop2xI64ForDivI64(&r, &rs, &reserved);
    quotientI64(rs, r, reserved, IsUnsigned(true), isConst, c);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
#  else
  MOZ_CRASH("BaseCompiler platform hook: emitQuotientU64");
#  endif
}

void BaseCompiler::emitRemainderI64() {
#  ifdef JS_64BIT
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI64 r = popI64();
    RegI64 temp = needI64();
    moveI64(r, temp);

    Label positive;
    masm.branchTest64(Assembler::NotSigned, temp, temp, RegI32::Invalid(),
                      &positive);
    masm.add64(Imm64(c - 1), temp);
    masm.bind(&positive);

    masm.rshift64Arithmetic(Imm32(power & 63), temp);
    masm.lshift64(Imm32(power & 63), temp);
    masm.sub64(temp, r);
    freeI64(temp);

    pushI64(r);
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    pop2xI64ForDivI64(&r, &rs, &reserved);
    remainderI64(rs, r, reserved, IsUnsigned(false), isConst, c);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
#  else
  MOZ_CRASH("BaseCompiler platform hook: emitRemainderI64");
#  endif
}

void BaseCompiler::emitRemainderU64() {
#  ifdef JS_64BIT
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI64 r = popI64();
    masm.and64(Imm64(c - 1), r);
    pushI64(r);
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    pop2xI64ForDivI64(&r, &rs, &reserved);
    remainderI64(rs, r, reserved, IsUnsigned(true), isConst, c);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
#  else
  MOZ_CRASH("BaseCompiler platform hook: emitRemainderU64");
#  endif
}
#endif  // RABALDR_INT_DIV_I64_CALLOUT

void BaseCompiler::emitRotrI64() {
  int64_t c;
  if (popConst(&c)) {
    RegI64 r = popI64();
    RegI32 temp = needRotate64Temp();
    masm.rotateRight64(Imm32(c & 63), r, r, temp);
    maybeFree(temp);
    pushI64(r);
  } else {
    RegI64 rs = popI64RhsForRotate();
    RegI64 r = popI64();
    masm.rotateRight64(lowPart(rs), r, r, maybeHighPart(rs));
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitRotlI64() {
  int64_t c;
  if (popConst(&c)) {
    RegI64 r = popI64();
    RegI32 temp = needRotate64Temp();
    masm.rotateLeft64(Imm32(c & 63), r, r, temp);
    maybeFree(temp);
    pushI64(r);
  } else {
    RegI64 rs = popI64RhsForRotate();
    RegI64 r = popI64();
    masm.rotateLeft64(lowPart(rs), r, r, maybeHighPart(rs));
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitEqzI32() {
  if (sniffConditionalControlEqz(ValType::I32)) {
    return;
  }
  emitUnop(EqzI32);
}

void BaseCompiler::emitEqzI64() {
  if (sniffConditionalControlEqz(ValType::I64)) {
    return;
  }
  emitUnop(EqzI64);
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF32ToI32() {
  RegF32 rs = popF32();
  RegI32 rd = needI32();
  if (!truncateF32ToI32(rs, rd, flags)) {
    return false;
  }
  freeF32(rs);
  pushI32(rd);
  return true;
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF64ToI32() {
  RegF64 rs = popF64();
  RegI32 rd = needI32();
  if (!truncateF64ToI32(rs, rd, flags)) {
    return false;
  }
  freeF64(rs);
  pushI32(rd);
  return true;
}

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT
template <TruncFlags flags>
bool BaseCompiler::emitTruncateF32ToI64() {
  RegF32 rs = popF32();
  RegI64 rd = needI64();
  RegF64 temp = needTempForFloatingToI64(flags);
  if (!truncateF32ToI64(rs, rd, flags, temp)) {
    return false;
  }
  maybeFree(temp);
  freeF32(rs);
  pushI64(rd);
  return true;
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF64ToI64() {
  RegF64 rs = popF64();
  RegI64 rd = needI64();
  RegF64 temp = needTempForFloatingToI64(flags);
  if (!truncateF64ToI64(rs, rd, flags, temp)) {
    return false;
  }
  maybeFree(temp);
  freeF64(rs);
  pushI64(rd);
  return true;
}
#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

void BaseCompiler::emitExtendI64_8() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move8To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI64_16() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move16To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI64_32() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move32To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI32ToI64() {
  RegI64 r;
  popI32ForSignExtendI64(&r);
  masm.move32To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendU32ToI64() {
  RegI32 rs = popI32();
  RegI64 rd = widenI32(rs);
  masm.move32To64ZeroExtend(rs, rd);
  pushI64(rd);
}

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
void BaseCompiler::emitConvertU64ToF32() {
  RegI64 rs = popI64();
  RegF32 rd = needF32();
  RegI32 temp = needConvertI64ToFloatTemp(ValType::F32, IsUnsigned(true));
  convertI64ToF32(rs, IsUnsigned(true), rd, temp);
  maybeFree(temp);
  freeI64(rs);
  pushF32(rd);
}

void BaseCompiler::emitConvertU64ToF64() {
  RegI64 rs = popI64();
  RegF64 rd = needF64();
  RegI32 temp = needConvertI64ToFloatTemp(ValType::F64, IsUnsigned(true));
  convertI64ToF64(rs, IsUnsigned(true), rd, temp);
  maybeFree(temp);
  freeI64(rs);
  pushF64(rd);
}
#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

template <typename Cond>
bool BaseCompiler::sniffConditionalControlCmp(Cond compareOp,
                                              ValType operandType) {
  MOZ_ASSERT(latentOp_ == LatentOp::None,
             "Latent comparison state not properly reset");

#ifdef JS_CODEGEN_X86
  // On x86, latent i64 binary comparisons use too many registers: the
  // reserved join register and the lhs and rhs operands require six, but we
  // only have five.
  if (operandType == ValType::I64) {
    return false;
  }
#endif

  // No optimization for pointer compares yet.
  if (operandType.isReference()) {
    return false;
  }

  OpBytes op{};
  iter_.peekOp(&op);
  switch (op.b0) {
    case uint16_t(Op::BrIf):
    case uint16_t(Op::If):
    case uint16_t(Op::SelectNumeric):
    case uint16_t(Op::SelectTyped):
      setLatentCompare(compareOp, operandType);
      return true;
    default:
      return false;
  }
}

bool BaseCompiler::sniffConditionalControlEqz(ValType operandType) {
  MOZ_ASSERT(latentOp_ == LatentOp::None,
             "Latent comparison state not properly reset");

  OpBytes op{};
  iter_.peekOp(&op);
  switch (op.b0) {
    case uint16_t(Op::BrIf):
    case uint16_t(Op::SelectNumeric):
    case uint16_t(Op::SelectTyped):
    case uint16_t(Op::If):
      setLatentEqz(operandType);
      return true;
    default:
      return false;
  }
}

void BaseCompiler::emitBranchSetup(BranchState* b) {
  // Avoid allocating operands to latentOp_ to result registers.
  if (b->hasBlockResults()) {
    needResultRegisters(b->resultType);
  }

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
      switch (latentType_.kind()) {
        case ValType::I32: {
          if (popConst(&b->i32.imm)) {
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
      switch (latentType_.kind()) {
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

  if (b->hasBlockResults()) {
    freeResultRegisters(b->resultType);
  }
}

bool BaseCompiler::emitBranchPerform(BranchState* b) {
  switch (latentType_.kind()) {
    case ValType::I32: {
      if (b->i32.rhsImm) {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i32.lhs,
                                        Imm32(b->i32.imm))) {
          return false;
        }
      } else {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i32.lhs,
                                        b->i32.rhs)) {
          return false;
        }
        freeI32(b->i32.rhs);
      }
      freeI32(b->i32.lhs);
      break;
    }
    case ValType::I64: {
      if (b->i64.rhsImm) {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i64.lhs,
                                        Imm64(b->i64.imm))) {
          return false;
        }
      } else {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i64.lhs,
                                        b->i64.rhs)) {
          return false;
        }
        freeI64(b->i64.rhs);
      }
      freeI64(b->i64.lhs);
      break;
    }
    case ValType::F32: {
      if (!jumpConditionalWithResults(b, latentDoubleCmp_, b->f32.lhs,
                                      b->f32.rhs)) {
        return false;
      }
      freeF32(b->f32.lhs);
      freeF32(b->f32.rhs);
      break;
    }
    case ValType::F64: {
      if (!jumpConditionalWithResults(b, latentDoubleCmp_, b->f64.lhs,
                                      b->f64.rhs)) {
        return false;
      }
      freeF64(b->f64.lhs);
      freeF64(b->f64.rhs);
      break;
    }
    default: {
      MOZ_CRASH("Unexpected type for LatentOp::Compare");
    }
  }
  resetLatentOp();
  return true;
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

bool BaseCompiler::emitBlock() {
  ResultType params;
  if (!iter_.readBlock(&params)) {
    return false;
  }

  if (!deadCode_) {
    sync();  // Simplifies branching out from block
  }

  initControl(controlItem(), params);

  return true;
}

bool BaseCompiler::endBlock(ResultType type) {
  Control& block = controlItem();

  if (deadCode_) {
    // Block does not fall through; reset stack.
    fr.resetStackHeight(block.stackHeight, type);
    popValueStackTo(block.stackSize);
  } else {
    // If the block label is used, we have a control join, so we need to shuffle
    // fallthrough values into place.  Otherwise if it's not a control join, we
    // can leave the value stack alone.
    MOZ_ASSERT(stk_.length() == block.stackSize + type.length());
    if (block.label.used()) {
      popBlockResults(type, block.stackHeight, ContinuationKind::Fallthrough);
    }
    block.bceSafeOnExit &= bceSafe_;
  }

  // Bind after cleanup: branches out will have popped the stack.
  if (block.label.used()) {
    masm.bind(&block.label);
    if (deadCode_) {
      captureResultRegisters(type);
      deadCode_ = false;
    }
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  bceSafe_ = block.bceSafeOnExit;

  return true;
}

bool BaseCompiler::emitLoop() {
  ResultType params;
  if (!iter_.readLoop(&params)) {
    return false;
  }

  if (!deadCode_) {
    sync();  // Simplifies branching out from block
  }

  initControl(controlItem(), params);
  bceSafe_ = 0;

  if (!deadCode_) {
    // Loop entry is a control join, so shuffle the entry parameters into the
    // well-known locations.
    if (!topBlockParams(params)) {
      return false;
    }
    masm.nopAlign(CodeAlignment);
    masm.bind(&controlItem(0).label);
    // The interrupt check barfs if there are live registers.
    sync();
    if (!addInterruptCheck()) {
      return false;
    }
  }

  return true;
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

bool BaseCompiler::emitIf() {
  ResultType params;
  Nothing unused_cond;
  if (!iter_.readIf(&params, &unused_cond)) {
    return false;
  }

  BranchState b(&controlItem().otherLabel, InvertBranch(true));
  if (!deadCode_) {
    needResultRegisters(params);
    emitBranchSetup(&b);
    freeResultRegisters(params);
    sync();
  } else {
    resetLatentOp();
  }

  initControl(controlItem(), params);

  if (!deadCode_) {
    // Because params can flow immediately to results in the case of an empty
    // "then" or "else" block, and the result of an if/then is a join in
    // general, we shuffle params eagerly to the result allocations.
    if (!topBlockParams(params)) {
      return false;
    }
    if (!emitBranchPerform(&b)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::endIfThen(ResultType type) {
  Control& ifThen = controlItem();

  // The parameters to the "if" logically flow to both the "then" and "else"
  // blocks, but the "else" block is empty.  Since we know that the "if"
  // type-checks, that means that the "else" parameters are the "else" results,
  // and that the "if"'s result type is the same as its parameter type.

  if (deadCode_) {
    // "then" arm does not fall through; reset stack.
    fr.resetStackHeight(ifThen.stackHeight, type);
    popValueStackTo(ifThen.stackSize);
    if (!ifThen.deadOnArrival) {
      captureResultRegisters(type);
    }
  } else {
    MOZ_ASSERT(stk_.length() == ifThen.stackSize + type.length());
    // Assume we have a control join, so place results in block result
    // allocations.
    popBlockResults(type, ifThen.stackHeight, ContinuationKind::Fallthrough);
    MOZ_ASSERT(!ifThen.deadOnArrival);
  }

  if (ifThen.otherLabel.used()) {
    masm.bind(&ifThen.otherLabel);
  }

  if (ifThen.label.used()) {
    masm.bind(&ifThen.label);
  }

  if (!deadCode_) {
    ifThen.bceSafeOnExit &= bceSafe_;
  }

  deadCode_ = ifThen.deadOnArrival;
  if (!deadCode_) {
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  bceSafe_ = ifThen.bceSafeOnExit & ifThen.bceSafeOnEntry;

  return true;
}

bool BaseCompiler::emitElse() {
  ResultType params, results;
  NothingVector unused_thenValues{};

  if (!iter_.readElse(&params, &results, &unused_thenValues)) {
    return false;
  }

  Control& ifThenElse = controlItem(0);

  // See comment in endIfThenElse, below.

  // Exit the "then" branch.

  ifThenElse.deadThenBranch = deadCode_;

  if (deadCode_) {
    fr.resetStackHeight(ifThenElse.stackHeight, results);
    popValueStackTo(ifThenElse.stackSize);
  } else {
    MOZ_ASSERT(stk_.length() == ifThenElse.stackSize + results.length());
    popBlockResults(results, ifThenElse.stackHeight, ContinuationKind::Jump);
    freeResultRegisters(results);
    MOZ_ASSERT(!ifThenElse.deadOnArrival);
  }

  if (!deadCode_) {
    masm.jump(&ifThenElse.label);
  }

  if (ifThenElse.otherLabel.used()) {
    masm.bind(&ifThenElse.otherLabel);
  }

  // Reset to the "else" branch.

  if (!deadCode_) {
    ifThenElse.bceSafeOnExit &= bceSafe_;
  }

  deadCode_ = ifThenElse.deadOnArrival;
  bceSafe_ = ifThenElse.bceSafeOnEntry;

  fr.resetStackHeight(ifThenElse.stackHeight, params);

  if (!deadCode_) {
    captureResultRegisters(params);
    if (!pushBlockResults(params)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::endIfThenElse(ResultType type) {
  Control& ifThenElse = controlItem();

  // The expression type is not a reliable guide to what we'll find
  // on the stack, we could have (if E (i32.const 1) (unreachable))
  // in which case the "else" arm is AnyType but the type of the
  // full expression is I32.  So restore whatever's there, not what
  // we want to find there.  The "then" arm has the same constraint.

  if (deadCode_) {
    // "then" arm does not fall through; reset stack.
    fr.resetStackHeight(ifThenElse.stackHeight, type);
    popValueStackTo(ifThenElse.stackSize);
  } else {
    MOZ_ASSERT(stk_.length() == ifThenElse.stackSize + type.length());
    // Assume we have a control join, so place results in block result
    // allocations.
    popBlockResults(type, ifThenElse.stackHeight,
                    ContinuationKind::Fallthrough);
    ifThenElse.bceSafeOnExit &= bceSafe_;
    MOZ_ASSERT(!ifThenElse.deadOnArrival);
  }

  if (ifThenElse.label.used()) {
    masm.bind(&ifThenElse.label);
  }

  bool joinLive =
      !ifThenElse.deadOnArrival &&
      (!ifThenElse.deadThenBranch || !deadCode_ || ifThenElse.label.bound());

  if (joinLive) {
    // No values were provided by the "then" path, but capture the values
    // provided by the "else" path.
    if (deadCode_) {
      captureResultRegisters(type);
    }
    deadCode_ = false;
  }

  bceSafe_ = ifThenElse.bceSafeOnExit;

  if (!deadCode_) {
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::emitEnd() {
  LabelKind kind;
  ResultType type;
  NothingVector unused_values{};
  if (!iter_.readEnd(&kind, &type, &unused_values, &unused_values)) {
    return false;
  }

  switch (kind) {
    case LabelKind::Body:
      if (!endBlock(type)) {
        return false;
      }
      doReturn(ContinuationKind::Fallthrough);
      // This is emitted here after `doReturn` to avoid being executed in the
      // normal return path of a function, and instead only when a `delegate`
      // jumps to it.
#ifdef ENABLE_WASM_EXCEPTIONS
      if (!emitBodyDelegateThrowPad()) {
        return false;
      }
#endif
      iter_.popEnd();
      MOZ_ASSERT(iter_.controlStackEmpty());
      return iter_.endFunction(iter_.end());
    case LabelKind::Block:
      if (!endBlock(type)) {
        return false;
      }
      break;
    case LabelKind::Loop:
      // The end of a loop isn't a branch target, so we can just leave its
      // results on the expression stack to be consumed by the outer block.
      break;
    case LabelKind::Then:
      if (!endIfThen(type)) {
        return false;
      }
      break;
    case LabelKind::Else:
      if (!endIfThenElse(type)) {
        return false;
      }
      break;
#ifdef ENABLE_WASM_EXCEPTIONS
    case LabelKind::Try:
    case LabelKind::Catch:
    case LabelKind::CatchAll:
      if (!endTryCatch(type)) {
        return false;
      }
      break;
#endif
  }

  iter_.popEnd();

  return true;
}

bool BaseCompiler::emitBr() {
  uint32_t relativeDepth;
  ResultType type;
  NothingVector unused_values{};
  if (!iter_.readBr(&relativeDepth, &type, &unused_values)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  // Save any values in the designated join registers, as if the target block
  // returned normally.

  popBlockResults(type, target.stackHeight, ContinuationKind::Jump);
  masm.jump(&target.label);

  // The registers holding the join values are free for the remainder of this
  // block.

  freeResultRegisters(type);

  deadCode_ = true;

  return true;
}

bool BaseCompiler::emitBrIf() {
  uint32_t relativeDepth;
  ResultType type;
  NothingVector unused_values{};
  Nothing unused_condition;
  if (!iter_.readBrIf(&relativeDepth, &type, &unused_values,
                      &unused_condition)) {
    return false;
  }

  if (deadCode_) {
    resetLatentOp();
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  BranchState b(&target.label, target.stackHeight, InvertBranch(false), type);
  emitBranchSetup(&b);
  return emitBranchPerform(&b);
}

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
bool BaseCompiler::emitBrOnNull() {
  MOZ_ASSERT(!hasLatentOp());

  uint32_t relativeDepth;
  ResultType type;
  NothingVector unused_values{};
  Nothing unused_condition;
  if (!iter_.readBrOnNull(&relativeDepth, &type, &unused_values,
                          &unused_condition)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  BranchState b(&target.label, target.stackHeight, InvertBranch(false), type);
  if (b.hasBlockResults()) {
    needResultRegisters(b.resultType);
  }
  RegRef rp = popRef();
  if (b.hasBlockResults()) {
    freeResultRegisters(b.resultType);
  }
  if (!jumpConditionalWithResults(&b, Assembler::Equal, rp,
                                  ImmWord(NULLREF_VALUE))) {
    return false;
  }
  pushRef(rp);

  return true;
}
#endif

bool BaseCompiler::emitBrTable() {
  Uint32Vector depths;
  uint32_t defaultDepth;
  ResultType branchParams;
  NothingVector unused_values{};
  Nothing unused_index;
  // N.B., `branchParams' gets set to the type of the default branch target.  In
  // the presence of subtyping, it could be that the different branch targets
  // have different types.  Here we rely on the assumption that the value
  // representations (e.g. Stk value types) of all branch target types are the
  // same, in the baseline compiler.  Notably, this means that all Ref types
  // should be represented the same.
  if (!iter_.readBrTable(&depths, &defaultDepth, &branchParams, &unused_values,
                         &unused_index)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  // Don't use param registers for rc
  needIntegerResultRegisters(branchParams);

  // Table switch value always on top.
  RegI32 rc = popI32();

  freeIntegerResultRegisters(branchParams);

  StackHeight resultsBase(0);
  if (!topBranchParams(branchParams, &resultsBase)) {
    return false;
  }

  Label dispatchCode;
  masm.branch32(Assembler::Below, rc, Imm32(depths.length()), &dispatchCode);

  // This is the out-of-range stub.  rc is dead here but we don't need it.

  shuffleStackResultsBeforeBranch(
      resultsBase, controlItem(defaultDepth).stackHeight, branchParams);
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
  if (!stubs.reserve(depths.length())) {
    return false;
  }

  for (uint32_t depth : depths) {
    stubs.infallibleEmplaceBack(NonAssertingLabel());
    masm.bind(&stubs.back());
    shuffleStackResultsBeforeBranch(resultsBase, controlItem(depth).stackHeight,
                                    branchParams);
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
  popValueStackBy(branchParams.length());

  return true;
}

#ifdef ENABLE_WASM_EXCEPTIONS
bool BaseCompiler::emitTry() {
  ResultType params;
  if (!iter_.readTry(&params)) {
    return false;
  }

  if (!deadCode_) {
    // Simplifies jumping out, but it is also necessary so that control
    // can re-enter the catch handler without restoring registers.
    sync();
  }

  initControl(controlItem(), params);

  if (!deadCode_) {
    // Be conservative for BCE due to complex control flow in try blocks.
    controlItem().bceSafeOnExit = 0;
    // Mark the beginning of the try block, the rest is filled in by catch.
    controlItem().tryNoteIndex = masm.wasmStartTry();
  }

  return true;
}

void BaseCompiler::emitCatchSetup(LabelKind kind, Control& tryCatch,
                                  const ResultType& resultType) {
  // Catch ends the try or last catch, so we finish this like endIfThen.
  if (deadCode_) {
    fr.resetStackHeight(tryCatch.stackHeight, resultType);
    popValueStackTo(tryCatch.stackSize);
  } else {
    // If the previous block is a catch, we need to handle the extra exception
    // reference on the stack (for rethrow) and thus the stack size is 1 more.
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize + resultType.length() +
                                    (kind == LabelKind::Try ? 0 : 1));
    // Try jumps to the end of the try-catch block unless a throw is done.
    if (kind == LabelKind::Try) {
      popBlockResults(resultType, tryCatch.stackHeight, ContinuationKind::Jump);
    } else {
      popCatchResults(resultType, tryCatch.stackHeight);
    }
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize);
    freeResultRegisters(resultType);
    MOZ_ASSERT(!tryCatch.deadOnArrival);
  }

  // Reset to this "catch" branch.
  deadCode_ = tryCatch.deadOnArrival;

  // We use the empty result type here because catch does *not* take the
  // try-catch block parameters.
  fr.resetStackHeight(tryCatch.stackHeight, ResultType::Empty());

  if (deadCode_) {
    return;
  }

  bceSafe_ = 0;

  // The end of the previous try/catch jumps to the join point.
  masm.jump(&tryCatch.label);

  // Note end of try block for finding the catch block target. This needs
  // to happen after the stack is reset to the correct height.
  if (kind == LabelKind::Try) {
    WasmTryNoteVector& tryNotes = masm.tryNotes();
    WasmTryNote& tryNote = tryNotes[controlItem().tryNoteIndex];
    tryNote.end = masm.currentOffset();
  }
}

bool BaseCompiler::emitCatch() {
  LabelKind kind;
  uint32_t eventIndex;
  ResultType paramType, resultType;
  NothingVector unused_tryValues{};

  if (!iter_.readCatch(&kind, &eventIndex, &paramType, &resultType,
                       &unused_tryValues)) {
    return false;
  }

  Control& tryCatch = controlItem();

  emitCatchSetup(kind, tryCatch, resultType);

  if (deadCode_) {
    return true;
  }

  // Construct info used for the exception landing pad.
  CatchInfo catchInfo(eventIndex);
  if (!tryCatch.catchInfos.emplaceBack(catchInfo)) {
    return false;
  }

  masm.bind(&tryCatch.catchInfos.back().label);

  // Extract the arguments in the exception package and push them.
  const ResultType params = moduleEnv_.events[eventIndex].resultType();

  uint32_t refCount = 0;
  for (uint32_t i = 0; i < params.length(); i++) {
    if (params[i].isReference()) {
      refCount++;
    }
  }

  const uint32_t dataOffset =
      NativeObject::getFixedSlotOffset(ArrayBufferObject::DATA_SLOT);

  // The code in the landing pad guarantees us that the exception reference
  // is live in this register.
  RegRef exn = RegRef(WasmExceptionReg);
  needRef(exn);
  RegRef values = needRef();
  RegRef refs = needRef();

  masm.unboxObject(Address(exn, WasmRuntimeExceptionObject::offsetOfValues()),
                   values);
  masm.unboxObject(Address(exn, WasmRuntimeExceptionObject::offsetOfRefs()),
                   refs);

#  ifdef DEBUG
  Label ok;
  RegI32 scratch = needI32();
  masm.load32(Address(refs, NativeObject::offsetOfFixedElements() +
                                ObjectElements::offsetOfLength()),
              scratch);
  masm.branch32(Assembler::Equal, scratch, Imm32(refCount), &ok);
  masm.assumeUnreachable("Array length should be equal to exn ref count.");
  masm.bind(&ok);
  freeI32(scratch);
#  endif
  masm.loadPtr(Address(refs, NativeObject::offsetOfElements()), refs);

  // This reference is pushed onto the stack because a potential rethrow
  // may need to access it. It is always popped at the end of the block.
  pushRef(exn);

  masm.loadPtr(Address(values, dataOffset), values);
  size_t argOffset = 0;
  // The ref values have been pushed into the ArrayObject in a stacklike
  // fashion so we need to load them starting from the last element.
  int32_t refIndex = refCount - 1;
  for (uint32_t i = 0; i < params.length(); i++) {
    switch (params[i].kind()) {
      case ValType::I32: {
        RegI32 reg = needI32();
        masm.load32(Address(values, argOffset), reg);
        pushI32(reg);
        break;
      }
      case ValType::I64: {
        RegI64 reg = needI64();
        masm.load64(Address(values, argOffset), reg);
        pushI64(reg);
        break;
      }
      case ValType::F32: {
        RegF32 reg = needF32();
        masm.loadFloat32(Address(values, argOffset), reg);
        pushF32(reg);
        break;
      }
      case ValType::F64: {
        RegF64 reg = needF64();
        masm.loadDouble(Address(values, argOffset), reg);
        pushF64(reg);
        break;
      }
      case ValType::V128: {
#  ifdef ENABLE_WASM_SIMD
        RegV128 reg = needV128();
        masm.loadUnalignedSimd128(Address(values, argOffset), reg);
        pushV128(reg);
        break;
#  else
        MOZ_CRASH("No SIMD support");
#  endif
      }
      case ValType::Rtt:
      case ValType::Ref: {
        // TODO/AnyRef-boxing: With boxed immediates and strings, this may need
        // to handle other kinds of values.
        ASSERT_ANYREF_IS_JSOBJECT;

        RegRef reg = needRef();
        NativeObject::elementsSizeMustNotOverflow();
        uint32_t offset = refIndex * sizeof(Value);
        masm.unboxObjectOrNull(Address(refs, offset), reg);
        pushRef(reg);
        refIndex--;
        break;
      }
    }
    if (!params[i].isReference()) {
      argOffset += SizeOf(params[i]);
    }
  }
  MOZ_ASSERT(refIndex == -1);
  freeRef(values);
  freeRef(refs);

  return true;
}

bool BaseCompiler::emitCatchAll() {
  LabelKind kind;
  ResultType paramType, resultType;
  NothingVector unused_tryValues{};

  if (!iter_.readCatchAll(&kind, &paramType, &resultType, &unused_tryValues)) {
    return false;
  }

  Control& tryCatch = controlItem();

  emitCatchSetup(kind, tryCatch, resultType);

  if (deadCode_) {
    return true;
  }

  CatchInfo catchInfo(CatchInfo::CATCH_ALL_INDEX);
  if (!tryCatch.catchInfos.emplaceBack(catchInfo)) {
    return false;
  }

  masm.bind(&tryCatch.catchInfos.back().label);

  // The code in the landing pad guarantees us that the exception reference
  // is live in this register.
  RegRef exn = RegRef(WasmExceptionReg);
  needRef(exn);
  // This reference is pushed onto the stack because a potential rethrow
  // may need to access it. It is always popped at the end of the block.
  pushRef(exn);

  return true;
}

bool BaseCompiler::emitBodyDelegateThrowPad() {
  Control& block = controlItem();

  // Only emit a landing pad if a `delegate` has generated a jump to here.
  if (block.otherLabel.used()) {
    StackHeight savedHeight = fr.stackHeight();
    fr.setStackHeight(block.stackHeight);
    masm.bind(&block.otherLabel);

    // We can assume this is live because `delegate` received it from a throw.
    RegRef exn = RegRef(WasmExceptionReg);
    needRef(exn);
    if (!throwFrom(exn, readCallSiteLineOrBytecode())) {
      return false;
    }
    fr.setStackHeight(savedHeight);
  }

  return true;
}

bool BaseCompiler::emitDelegate() {
  uint32_t relativeDepth;
  ResultType resultType;
  NothingVector unused_tryValues{};

  if (!iter_.readDelegate(&relativeDepth, &resultType, &unused_tryValues)) {
    return false;
  }

  Control& tryDelegate = controlItem();
  Control& target = controlItem(relativeDepth);

  // End the try branch like a plain catch block without exception ref handling.
  if (deadCode_) {
    fr.resetStackHeight(tryDelegate.stackHeight, resultType);
    popValueStackTo(tryDelegate.stackSize);
  } else {
    MOZ_ASSERT(stk_.length() == tryDelegate.stackSize + resultType.length());
    popBlockResults(resultType, tryDelegate.stackHeight,
                    ContinuationKind::Jump);
    freeResultRegisters(resultType);
    masm.jump(&tryDelegate.label);
    MOZ_ASSERT(!tryDelegate.deadOnArrival);
  }

  deadCode_ = tryDelegate.deadOnArrival;

  if (deadCode_) {
    return true;
  }

  // Create an exception landing pad that immediately branches to the landing
  // pad of the delegated try block.
  masm.bind(&tryDelegate.otherLabel);

  StackHeight savedHeight = fr.stackHeight();
  fr.setStackHeight(tryDelegate.stackHeight);

  WasmTryNoteVector& tryNotes = masm.tryNotes();
  WasmTryNote& tryNote = tryNotes[controlItem().tryNoteIndex];
  tryNote.end = masm.currentOffset();
  tryNote.entryPoint = tryNote.end;
  tryNote.framePushed = masm.framePushed();

  masm.jump(&target.otherLabel);

  fr.setStackHeight(savedHeight);

  // Where the try branch jumps to, if it's not dead.
  if (tryDelegate.label.used()) {
    masm.bind(&tryDelegate.label);
  }

  captureResultRegisters(resultType);
  bceSafe_ = tryDelegate.bceSafeOnExit;

  return pushBlockResults(resultType);
}

bool BaseCompiler::endTryCatch(ResultType type) {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  Control& tryCatch = controlItem();
  LabelKind tryKind = iter_.controlKind(0);

  if (deadCode_) {
    fr.resetStackHeight(tryCatch.stackHeight, type);
    popValueStackTo(tryCatch.stackSize);
  } else {
    // If the previous block is a catch, we must handle the extra exception
    // reference on the stack (for rethrow) and thus the stack size is 1 more.
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize + type.length() +
                                    (tryKind == LabelKind::Try ? 0 : 1));
    // Assume we have a control join, so place results in block result
    // allocations and also handle the implicit exception reference if needed.
    if (tryKind == LabelKind::Try) {
      popBlockResults(type, tryCatch.stackHeight, ContinuationKind::Jump);
    } else {
      popCatchResults(type, tryCatch.stackHeight);
    }
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize);
    // Since we will emit a landing pad after this and jump over it to get to
    // the control join, we free these here and re-capture at the join.
    freeResultRegisters(type);
    masm.jump(&tryCatch.label);
    MOZ_ASSERT(!tryCatch.bceSafeOnExit);
    MOZ_ASSERT(!tryCatch.deadOnArrival);
  }

  deadCode_ = tryCatch.deadOnArrival;

  if (deadCode_) {
    return true;
  }

  // Create landing pad for all catch handlers in this block.
  // When used for a catchless try block, this will generate a landing pad
  // with no handlers and only the fall-back rethrow.
  masm.bind(&tryCatch.otherLabel);

  // The stack height also needs to be set not for a block result, but for the
  // entry to the exception handlers. This is reset again below for the join.
  StackHeight prePadHeight = fr.stackHeight();
  fr.setStackHeight(tryCatch.stackHeight);

  WasmTryNoteVector& tryNotes = masm.tryNotes();
  WasmTryNote& tryNote = tryNotes[controlItem().tryNoteIndex];
  tryNote.entryPoint = masm.currentOffset();
  tryNote.framePushed = masm.framePushed();

  // If we are in a catchless try block, then there were no catch blocks to
  // mark the end of the try note, so we need to end it here.
  if (tryKind == LabelKind::Try) {
    tryNote.end = tryNote.entryPoint;
  }

  RegRef exn = RegRef(WasmExceptionReg);
  needRef(exn);

  // Explicitly restore the tls data in case the throw was across instances.
  fr.loadTlsPtr(WasmTlsReg);
  masm.loadWasmPinnedRegsFromTls();
  RegRef scratch = needRef();
  RegRef scratch2 = needRef();
  masm.switchToWasmTlsRealm(scratch, scratch2);
  freeRef(scratch2);

  // Make sure that the exception pointer is saved across the call.
  masm.movePtr(exn, scratch);
  pushRef(exn);
  pushRef(scratch);

  if (!emitInstanceCall(lineOrBytecode, SASigGetLocalExceptionIndex)) {
    return false;
  }

  // Prevent conflict with exn register when popping this result.
  needRef(exn);
  RegI32 index = popI32();
  freeRef(exn);

  // Ensure that the exception is materialized before branching.
  exn = popRef(RegRef(WasmExceptionReg));

  bool hasCatchAll = false;
  for (CatchInfo& info : tryCatch.catchInfos) {
    if (info.eventIndex != CatchInfo::CATCH_ALL_INDEX) {
      MOZ_ASSERT(!hasCatchAll);
      masm.branch32(Assembler::Equal, index, Imm32(info.eventIndex),
                    &info.label);
    } else {
      masm.jump(&info.label);
      hasCatchAll = true;
      // `catch_all` must be the last clause and we won't call throwFrom
      // below due to the catch_all, so we can free exn here.
      freeRef(exn);
    }
  }
  freeI32(index);

  // If none of the tag checks succeed and there is no catch_all,
  // then we rethrow the exception.
  if (!hasCatchAll && !throwFrom(exn, lineOrBytecode)) {
    return false;
  }

  // Reset stack height for join.
  fr.setStackHeight(prePadHeight);

  // Create join point.
  if (tryCatch.label.used()) {
    masm.bind(&tryCatch.label);
  }

  captureResultRegisters(type);
  deadCode_ = tryCatch.deadOnArrival;
  bceSafe_ = tryCatch.bceSafeOnExit;

  return pushBlockResults(type);
}

bool BaseCompiler::emitThrow() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();
  uint32_t exnIndex;
  NothingVector unused_argValues{};

  if (!iter_.readThrow(&exnIndex, &unused_argValues)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ResultType& params = moduleEnv_.events[exnIndex].resultType();

  // Measure space we need for all the args to put in the exception.
  uint32_t exnBytes = 0;
  for (size_t i = 0; i < params.length(); i++) {
    if (!params[i].isReference()) {
      exnBytes += SizeOf(params[i]);
    }
  }

  // Create the new exception object that we will throw.
  pushI32(exnIndex);
  pushI32(exnBytes);
  if (!emitInstanceCall(lineOrBytecode, SASigExceptionNew)) {
    return false;
  }

  RegRef exn = popRef();
  // Create scratch register, to store the exception package values.
  RegRef scratch = needRef();
  const uint32_t dataOffset =
      NativeObject::getFixedSlotOffset(ArrayBufferObject::DATA_SLOT);

  Address exnValuesAddress(exn, WasmRuntimeExceptionObject::offsetOfValues());
  masm.unboxObject(exnValuesAddress, scratch);
  masm.loadPtr(Address(scratch, dataOffset), scratch);

  size_t argOffset = exnBytes;
  for (int32_t i = params.length() - 1; i >= 0; i--) {
    if (!params[i].isReference()) {
      argOffset -= SizeOf(params[i]);
    }
    switch (params[i].kind()) {
      case ValType::I32: {
        RegI32 reg = popI32();
        masm.store32(reg, Address(scratch, argOffset));
        freeI32(reg);
        break;
      }
      case ValType::I64: {
        RegI64 reg = popI64();
        masm.store64(reg, Address(scratch, argOffset));
        freeI64(reg);
        break;
      }
      case ValType::F32: {
        RegF32 reg = popF32();
        masm.storeFloat32(reg, Address(scratch, argOffset));
        freeF32(reg);
        break;
      }
      case ValType::F64: {
        RegF64 reg = popF64();
        masm.storeDouble(reg, Address(scratch, argOffset));
        freeF64(reg);
        break;
      }
      case ValType::V128: {
#  ifdef ENABLE_WASM_SIMD
        RegV128 reg = popV128();
        masm.storeUnalignedSimd128(reg, Address(scratch, argOffset));
        freeV128(reg);
        break;
#  else
        MOZ_CRASH("No SIMD support");
#  endif
      }
      case ValType::Rtt:
      case ValType::Ref: {
        RegRef refArg = popRef();

        // Keep exn on the stack to preserve it across the call.
        RegRef tmp = needRef();
        moveRef(exn, tmp);
        pushRef(tmp);

        // Arguments to the instance call start here.
        pushRef(exn);
        pushRef(refArg);

        if (!emitInstanceCall(lineOrBytecode, SASigPushRefIntoExn)) {
          return false;
        }

        // The call result is checked by the instance call failure handling,
        // so we do not need to use the result here.
        freeI32(popI32());

        exn = popRef();

        // Restore scratch register contents that got clobbered.
        masm.unboxObject(exnValuesAddress, scratch);
        masm.loadPtr(Address(scratch, dataOffset), scratch);
        break;
      }
    }
  }
  MOZ_ASSERT(argOffset == 0);
  freeRef(scratch);

  deadCode_ = true;

  return throwFrom(exn, lineOrBytecode);
}

bool BaseCompiler::emitRethrow() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t relativeDepth;
  if (!iter_.readRethrow(&relativeDepth)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Control& tryCatch = controlItem(relativeDepth);
  RegRef exn = needRef();
  dupRefAt(tryCatch.stackSize, exn);

  return throwFrom(exn, lineOrBytecode);
}
#endif

bool BaseCompiler::emitDrop() {
  if (!iter_.readDrop()) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  dropValue();
  return true;
}

void BaseCompiler::doReturn(ContinuationKind kind) {
  if (deadCode_) {
    return;
  }

  StackHeight height = controlOutermost().stackHeight;
  ResultType type = ResultType::Vector(funcType().results());
  popBlockResults(type, height, kind);
  masm.jump(&returnLabel_);
  freeResultRegisters(type);
}

bool BaseCompiler::emitReturn() {
  NothingVector unused_values{};
  if (!iter_.readReturn(&unused_values)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  doReturn(ContinuationKind::Jump);
  deadCode_ = true;

  return true;
}

bool BaseCompiler::emitCallArgs(const ValTypeVector& argTypes,
                                const StackResultsLoc& results,
                                FunctionCall* baselineCall,
                                CalleeOnStack calleeOnStack) {
  MOZ_ASSERT(!deadCode_);

  ArgTypeVector args(argTypes, results.stackResults());
  uint32_t naturalArgCount = argTypes.length();
  uint32_t abiArgCount = args.lengthWithStackResults();
  startCallArgs(StackArgAreaSizeUnaligned(args), baselineCall);

  // Args are deeper on the stack than the stack result area, if any.
  size_t argsDepth = results.count();
  // They're deeper than the callee too, for callIndirect.
  if (calleeOnStack == CalleeOnStack::True) {
    argsDepth++;
  }

  for (size_t i = 0; i < abiArgCount; ++i) {
    if (args.isNaturalArg(i)) {
      size_t naturalIndex = args.naturalIndex(i);
      size_t stackIndex = naturalArgCount - 1 - naturalIndex + argsDepth;
      passArg(argTypes[naturalIndex], peek(stackIndex), baselineCall);
    } else {
      // The synthetic stack result area pointer.
      ABIArg argLoc = baselineCall->abi.next(MIRType::Pointer);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchPtr scratch(*this);
        fr.computeOutgoingStackResultAreaPtr(results, scratch);
        masm.storePtr(scratch, Address(masm.getStackPointer(),
                                       argLoc.offsetFromArgBase()));
      } else {
        fr.computeOutgoingStackResultAreaPtr(results, RegPtr(argLoc.gpr()));
      }
    }
  }

  fr.loadTlsPtr(WasmTlsReg);
  return true;
}

void BaseCompiler::pushReturnValueOfCall(const FunctionCall& call,
                                         MIRType type) {
  switch (type) {
    case MIRType::Int32: {
      RegI32 rv = captureReturnedI32();
      pushI32(rv);
      break;
    }
    case MIRType::Int64: {
      RegI64 rv = captureReturnedI64();
      pushI64(rv);
      break;
    }
    case MIRType::Float32: {
      RegF32 rv = captureReturnedF32(call);
      pushF32(rv);
      break;
    }
    case MIRType::Double: {
      RegF64 rv = captureReturnedF64(call);
      pushF64(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128: {
      RegV128 rv = captureReturnedV128(call);
      pushV128(rv);
      break;
    }
#endif
    case MIRType::RefOrNull: {
      RegRef rv = captureReturnedRef();
      pushRef(rv);
      break;
    }
    default:
      // In particular, passing |type| as MIRType::Void or MIRType::Pointer to
      // this function is an error.
      MOZ_CRASH("Function return type");
  }
}

bool BaseCompiler::pushStackResultsForCall(const ResultType& type, RegPtr temp,
                                           StackResultsLoc* loc) {
  if (!ABIResultIter::HasStackResults(type)) {
    return true;
  }

  // This method is the only one in the class that can increase stk_.length() by
  // an unbounded amount, so it's the only one that requires an allocation.
  // (The general case is handled in emitBody.)
  if (!stk_.reserve(stk_.length() + type.length())) {
    return false;
  }

  // Measure stack results.
  ABIResultIter i(type);
  size_t count = 0;
  for (; !i.done(); i.next()) {
    if (i.cur().onStack()) {
      count++;
    }
  }
  uint32_t bytes = i.stackBytesConsumedSoFar();

  // Reserve space for the stack results.
  StackHeight resultsBase = fr.stackHeight();
  uint32_t height = fr.prepareStackResultArea(resultsBase, bytes);

  // Push Stk values onto the value stack, and zero out Ref values.
  for (i.switchToPrev(); !i.done(); i.prev()) {
    const ABIResult& result = i.cur();
    if (result.onStack()) {
      Stk v = captureStackResult(result, resultsBase, bytes);
      push(v);
      if (v.kind() == Stk::MemRef) {
        stackMapGenerator_.memRefsOnStk++;
        fr.storeImmediatePtrToStack(intptr_t(0), v.offs(), temp);
      }
    }
  }

  *loc = StackResultsLoc(bytes, count, height);

  return true;
}

// After a call, some results may be written to the stack result locations that
// are pushed on the machine stack after any stack args.  If there are stack
// args and stack results, these results need to be shuffled down, as the args
// are "consumed" by the call.
void BaseCompiler::popStackResultsAfterCall(const StackResultsLoc& results,
                                            uint32_t stackArgBytes) {
  if (results.bytes() != 0) {
    popValueStackBy(results.count());
    if (stackArgBytes != 0) {
      uint32_t srcHeight = results.height();
      MOZ_ASSERT(srcHeight >= stackArgBytes + results.bytes());
      uint32_t destHeight = srcHeight - stackArgBytes;

      fr.shuffleStackResultsTowardFP(srcHeight, destHeight, results.bytes(),
                                     ABINonArgReturnVolatileReg);
    }
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

bool BaseCompiler::emitCall() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t funcIndex;
  NothingVector args_{};
  if (!iter_.readCall(&funcIndex, &args_)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  sync();

  const FuncType& funcType = *moduleEnv_.funcs[funcIndex].type;
  bool import = moduleEnv_.funcIsImport(funcIndex);

  uint32_t numArgs = funcType.args().length();
  size_t stackArgBytes = stackConsumed(numArgs);

  ResultType resultType(ResultType::Vector(funcType.results()));
  StackResultsLoc results;
  if (!pushStackResultsForCall(resultType, RegPtr(ABINonArgReg0), &results)) {
    return false;
  }

  FunctionCall baselineCall(lineOrBytecode);
  beginCall(baselineCall, UseABI::Wasm,
            import ? InterModule::True : InterModule::False);

  if (!emitCallArgs(funcType.args(), results, &baselineCall,
                    CalleeOnStack::False)) {
    return false;
  }

  CodeOffset raOffset;
  if (import) {
    raOffset = callImport(moduleEnv_.funcImportGlobalDataOffsets[funcIndex],
                          baselineCall);
  } else {
    raOffset = callDefinition(funcIndex, baselineCall);
  }

  if (!createStackMap("emitCall", raOffset)) {
    return false;
  }

  popStackResultsAfterCall(results, stackArgBytes);

  endCall(baselineCall, stackArgBytes);

  popValueStackBy(numArgs);

  captureCallResultRegisters(resultType);
  return pushCallResults(baselineCall, resultType, results);
}

bool BaseCompiler::emitCallIndirect() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t funcTypeIndex;
  uint32_t tableIndex;
  Nothing callee_;
  NothingVector args_{};
  if (!iter_.readCallIndirect(&funcTypeIndex, &tableIndex, &callee_, &args_)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  sync();

  const FuncType& funcType = moduleEnv_.types[funcTypeIndex].funcType();

  // Stack: ... arg1 .. argn callee

  uint32_t numArgs = funcType.args().length() + 1;
  size_t stackArgBytes = stackConsumed(numArgs);

  ResultType resultType(ResultType::Vector(funcType.results()));
  StackResultsLoc results;
  if (!pushStackResultsForCall(resultType, RegPtr(ABINonArgReg0), &results)) {
    return false;
  }

  FunctionCall baselineCall(lineOrBytecode);
  beginCall(baselineCall, UseABI::Wasm, InterModule::True);

  if (!emitCallArgs(funcType.args(), results, &baselineCall,
                    CalleeOnStack::True)) {
    return false;
  }

  const Stk& callee = peek(results.count());
  CodeOffset raOffset =
      callIndirect(funcTypeIndex, tableIndex, callee, baselineCall);
  if (!createStackMap("emitCallIndirect", raOffset)) {
    return false;
  }

  popStackResultsAfterCall(results, stackArgBytes);

  endCall(baselineCall, stackArgBytes);

  popValueStackBy(numArgs);

  captureCallResultRegisters(resultType);
  return pushCallResults(baselineCall, resultType, results);
}

void BaseCompiler::emitRound(RoundingMode roundingMode, ValType operandType) {
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

bool BaseCompiler::emitUnaryMathBuiltinCall(SymbolicAddress callee,
                                            ValType operandType) {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  Nothing operand_;
  if (!iter_.readUnary(operandType, &operand_)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RoundingMode roundingMode;
  if (IsRoundingFunction(callee, &roundingMode) &&
      supportsRoundInstruction(roundingMode)) {
    emitRound(roundingMode, operandType);
    return true;
  }

  sync();

  ValTypeVector& signature = operandType == ValType::F32 ? SigF_ : SigD_;
  ValType retType = operandType;
  uint32_t numArgs = signature.length();
  size_t stackSpace = stackConsumed(numArgs);
  StackResultsLoc noStackResults;

  FunctionCall baselineCall(lineOrBytecode);
  beginCall(baselineCall, UseABI::Builtin, InterModule::False);

  if (!emitCallArgs(signature, noStackResults, &baselineCall,
                    CalleeOnStack::False)) {
    return false;
  }

  CodeOffset raOffset = builtinCall(callee, baselineCall);
  if (!createStackMap("emitUnaryMathBuiltin[..]", raOffset)) {
    return false;
  }

  endCall(baselineCall, stackSpace);

  popValueStackBy(numArgs);

  pushReturnValueOfCall(baselineCall, ToMIRType(retType));

  return true;
}

#ifdef RABALDR_INT_DIV_I64_CALLOUT
bool BaseCompiler::emitDivOrModI64BuiltinCall(SymbolicAddress callee,
                                              ValType operandType) {
  MOZ_ASSERT(operandType == ValType::I64);
  MOZ_ASSERT(!deadCode_);

  sync();

  needI64(specific_.abiReturnRegI64);

  RegI64 rhs = popI64();
  RegI64 srcDest = popI64ToSpecific(specific_.abiReturnRegI64);

  Label done;

  checkDivideByZeroI64(rhs);

  if (callee == SymbolicAddress::DivI64) {
    checkDivideSignedOverflowI64(rhs, srcDest, &done, ZeroOnOverflow(false));
  } else if (callee == SymbolicAddress::ModI64) {
    checkDivideSignedOverflowI64(rhs, srcDest, &done, ZeroOnOverflow(true));
  }

  masm.setupWasmABICall();
  masm.passABIArg(srcDest.high);
  masm.passABIArg(srcDest.low);
  masm.passABIArg(rhs.high);
  masm.passABIArg(rhs.low);
  CodeOffset raOffset = masm.callWithABI(bytecodeOffset(), callee,
                                         mozilla::Some(fr.getTlsPtrOffset()));
  if (!createStackMap("emitDivOrModI64Bui[..]", raOffset)) {
    return false;
  }

  masm.bind(&done);

  freeI64(rhs);
  pushI64(srcDest);
  return true;
}
#endif  // RABALDR_INT_DIV_I64_CALLOUT

#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
bool BaseCompiler::emitConvertInt64ToFloatingCallout(SymbolicAddress callee,
                                                     ValType operandType,
                                                     ValType resultType) {
  sync();

  RegI64 input = popI64();

  FunctionCall call(0);

  masm.setupWasmABICall();
#  ifdef JS_PUNBOX64
  MOZ_CRASH("BaseCompiler platform hook: emitConvertInt64ToFloatingCallout");
#  else
  masm.passABIArg(input.high);
  masm.passABIArg(input.low);
#  endif
  CodeOffset raOffset = masm.callWithABI(
      bytecodeOffset(), callee, mozilla::Some(fr.getTlsPtrOffset()),
      resultType == ValType::F32 ? MoveOp::FLOAT32 : MoveOp::DOUBLE);
  if (!createStackMap("emitConvertInt64To[..]", raOffset)) {
    return false;
  }

  freeI64(input);

  if (resultType == ValType::F32) {
    pushF32(captureReturnedF32(call));
  } else {
    pushF64(captureReturnedF64(call));
  }

  return true;
}
#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
// `Callee` always takes a double, so a float32 input must be converted.
bool BaseCompiler::emitConvertFloatingToInt64Callout(SymbolicAddress callee,
                                                     ValType operandType,
                                                     ValType resultType) {
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
  CodeOffset raOffset = masm.callWithABI(bytecodeOffset(), callee,
                                         mozilla::Some(fr.getTlsPtrOffset()));
  if (!createStackMap("emitConvertFloatin[..]", raOffset)) {
    return false;
  }

  freeF64(doubleInput);

  RegI64 rv = captureReturnedI64();

  RegF64 inputVal = popF64();

  TruncFlags flags = 0;
  if (callee == SymbolicAddress::TruncateDoubleToUint64) {
    flags |= TRUNC_UNSIGNED;
  }
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
    ool = addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
        AnyReg(inputVal), rv, flags, bytecodeOffset()));
    if (!ool) {
      return false;
    }

    masm.branch64(Assembler::Equal, rv, Imm64(0x8000000000000000),
                  ool->entry());
    masm.bind(ool->rejoin());
  }

  pushI64(rv);
  freeF64(inputVal);

  return true;
}
#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

bool BaseCompiler::emitGetLocal() {
  uint32_t slot;
  if (!iter_.readGetLocal(locals_, &slot)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  // Local loads are pushed unresolved, ie, they may be deferred
  // until needed, until they may be affected by a store, or until a
  // sync.  This is intended to reduce register pressure.

  switch (locals_[slot].kind()) {
    case ValType::I32:
      pushLocalI32(slot);
      break;
    case ValType::I64:
      pushLocalI64(slot);
      break;
    case ValType::V128:
#ifdef ENABLE_WASM_SIMD
      pushLocalV128(slot);
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    case ValType::F64:
      pushLocalF64(slot);
      break;
    case ValType::F32:
      pushLocalF32(slot);
      break;
    case ValType::Rtt:
    case ValType::Ref:
      pushLocalRef(slot);
      break;
  }

  return true;
}

template <bool isSetLocal>
bool BaseCompiler::emitSetOrTeeLocal(uint32_t slot) {
  if (deadCode_) {
    return true;
  }

  bceLocalIsUpdated(slot);
  switch (locals_[slot].kind()) {
    case ValType::I32: {
      RegI32 rv = popI32();
      syncLocal(slot);
      fr.storeLocalI32(rv, localFromSlot(slot, MIRType::Int32));
      if (isSetLocal) {
        freeI32(rv);
      } else {
        pushI32(rv);
      }
      break;
    }
    case ValType::I64: {
      RegI64 rv = popI64();
      syncLocal(slot);
      fr.storeLocalI64(rv, localFromSlot(slot, MIRType::Int64));
      if (isSetLocal) {
        freeI64(rv);
      } else {
        pushI64(rv);
      }
      break;
    }
    case ValType::F64: {
      RegF64 rv = popF64();
      syncLocal(slot);
      fr.storeLocalF64(rv, localFromSlot(slot, MIRType::Double));
      if (isSetLocal) {
        freeF64(rv);
      } else {
        pushF64(rv);
      }
      break;
    }
    case ValType::F32: {
      RegF32 rv = popF32();
      syncLocal(slot);
      fr.storeLocalF32(rv, localFromSlot(slot, MIRType::Float32));
      if (isSetLocal) {
        freeF32(rv);
      } else {
        pushF32(rv);
      }
      break;
    }
    case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
      RegV128 rv = popV128();
      syncLocal(slot);
      fr.storeLocalV128(rv, localFromSlot(slot, MIRType::Simd128));
      if (isSetLocal) {
        freeV128(rv);
      } else {
        pushV128(rv);
      }
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    }
    case ValType::Rtt:
    case ValType::Ref: {
      RegRef rv = popRef();
      syncLocal(slot);
      fr.storeLocalRef(rv, localFromSlot(slot, MIRType::RefOrNull));
      if (isSetLocal) {
        freeRef(rv);
      } else {
        pushRef(rv);
      }
      break;
    }
  }

  return true;
}

bool BaseCompiler::emitSetLocal() {
  uint32_t slot;
  Nothing unused_value;
  if (!iter_.readSetLocal(locals_, &slot, &unused_value)) {
    return false;
  }
  return emitSetOrTeeLocal<true>(slot);
}

bool BaseCompiler::emitTeeLocal() {
  uint32_t slot;
  Nothing unused_value;
  if (!iter_.readTeeLocal(locals_, &slot, &unused_value)) {
    return false;
  }
  return emitSetOrTeeLocal<false>(slot);
}

bool BaseCompiler::emitGetGlobal() {
  uint32_t id;
  if (!iter_.readGetGlobal(&id)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const GlobalDesc& global = moduleEnv_.globals[id];

  if (global.isConstant()) {
    LitVal value = global.constantValue();
    switch (value.type().kind()) {
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
      case ValType::Ref:
        pushRef(intptr_t(value.ref().forCompiledCode()));
        break;
#ifdef ENABLE_WASM_SIMD
      case ValType::V128:
        pushV128(value.v128());
        break;
#endif
      default:
        MOZ_CRASH("Global constant type");
    }
    return true;
  }

  switch (global.type().kind()) {
    case ValType::I32: {
      RegI32 rv = needI32();
      ScratchI32 tmp(*this);
      masm.load32(addressOfGlobalVar(global, tmp), rv);
      pushI32(rv);
      break;
    }
    case ValType::I64: {
      RegI64 rv = needI64();
      ScratchI32 tmp(*this);
      masm.load64(addressOfGlobalVar(global, tmp), rv);
      pushI64(rv);
      break;
    }
    case ValType::F32: {
      RegF32 rv = needF32();
      ScratchI32 tmp(*this);
      masm.loadFloat32(addressOfGlobalVar(global, tmp), rv);
      pushF32(rv);
      break;
    }
    case ValType::F64: {
      RegF64 rv = needF64();
      ScratchI32 tmp(*this);
      masm.loadDouble(addressOfGlobalVar(global, tmp), rv);
      pushF64(rv);
      break;
    }
    case ValType::Ref: {
      RegRef rv = needRef();
      ScratchI32 tmp(*this);
      masm.loadPtr(addressOfGlobalVar(global, tmp), rv);
      pushRef(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 rv = needV128();
      ScratchI32 tmp(*this);
      masm.loadUnalignedSimd128(addressOfGlobalVar(global, tmp), rv);
      pushV128(rv);
      break;
    }
#endif
    default:
      MOZ_CRASH("Global variable type");
      break;
  }
  return true;
}

bool BaseCompiler::emitSetGlobal() {
  uint32_t id;
  Nothing unused_value;
  if (!iter_.readSetGlobal(&id, &unused_value)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const GlobalDesc& global = moduleEnv_.globals[id];

  switch (global.type().kind()) {
    case ValType::I32: {
      RegI32 rv = popI32();
      ScratchI32 tmp(*this);
      masm.store32(rv, addressOfGlobalVar(global, tmp));
      freeI32(rv);
      break;
    }
    case ValType::I64: {
      RegI64 rv = popI64();
      ScratchI32 tmp(*this);
      masm.store64(rv, addressOfGlobalVar(global, tmp));
      freeI64(rv);
      break;
    }
    case ValType::F32: {
      RegF32 rv = popF32();
      ScratchI32 tmp(*this);
      masm.storeFloat32(rv, addressOfGlobalVar(global, tmp));
      freeF32(rv);
      break;
    }
    case ValType::F64: {
      RegF64 rv = popF64();
      ScratchI32 tmp(*this);
      masm.storeDouble(rv, addressOfGlobalVar(global, tmp));
      freeF64(rv);
      break;
    }
    case ValType::Ref: {
      RegPtr valueAddr(PreBarrierReg);
      needPtr(valueAddr);
      {
        ScratchI32 tmp(*this);
        masm.computeEffectiveAddress(addressOfGlobalVar(global, tmp),
                                     valueAddr);
      }
      RegRef rv = popRef();
      // emitBarrieredStore preserves rv
      if (!emitBarrieredStore(Nothing(), valueAddr, rv)) {
        return false;
      }
      freeRef(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 rv = popV128();
      ScratchI32 tmp(*this);
      masm.storeUnalignedSimd128(rv, addressOfGlobalVar(global, tmp));
      freeV128(rv);
      break;
    }
#endif
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
// compilerEnv_.debugEnabled() is true.
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

RegI32 BaseCompiler::popMemory32Access(MemoryAccessDesc* access,
                                       AccessCheck* check) {
  check->onlyPointerAlignment =
      (access->offset() & (access->byteSize() - 1)) == 0;

  int32_t addrTemp;
  if (popConst(&addrTemp)) {
    uint32_t addr = addrTemp;

    uint32_t offsetGuardLimit =
        GetMaxOffsetGuardLimit(moduleEnv_.hugeMemoryEnabled());

    uint64_t ea = uint64_t(addr) + uint64_t(access->offset());
    uint64_t limit = moduleEnv_.memory->initialLength32() + offsetGuardLimit;

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
  if (peekLocalI32(&local)) {
    bceCheckLocal(access, check, local);
  }

  return popI32();
}

void BaseCompiler::pushHeapBase() {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS64)
  RegI64 heapBase = needI64();
  moveI64(RegI64(Register64(HeapReg)), heapBase);
  pushI64(heapBase);
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32)
  RegI32 heapBase = needI32();
  moveI32(RegI32(HeapReg), heapBase);
  pushI32(heapBase);
#elif defined(JS_CODEGEN_X86)
  RegI32 heapBase = needI32();
  fr.loadTlsPtr(heapBase);
  masm.loadPtr(Address(heapBase, offsetof(TlsData, memoryBase)), heapBase);
  pushI32(heapBase);
#else
  MOZ_CRASH("BaseCompiler platform hook: pushHeapBase");
#endif
}

RegI32 BaseCompiler::maybeLoadTlsForAccess(const AccessCheck& check) {
  RegI32 tls;
  if (needTlsForAccess(check)) {
    tls = needI32();
    fr.loadTlsPtr(tls);
  }
  return tls;
}

RegI32 BaseCompiler::maybeLoadTlsForAccess(const AccessCheck& check,
                                           RegI32 specific) {
  if (needTlsForAccess(check)) {
    fr.loadTlsPtr(specific);
    return specific;
  }
  return RegI32::Invalid();
}

bool BaseCompiler::loadCommon(MemoryAccessDesc* access, AccessCheck check,
                              ValType type) {
  RegI32 tls, temp1, temp2, temp3;
  needLoadTemps(*access, &temp1, &temp2, &temp3);

  switch (type.kind()) {
    case ValType::I32: {
      RegI32 rp = popMemory32Access(access, &check);
#ifdef JS_CODEGEN_ARM
      RegI32 rv = IsUnaligned(*access) ? needI32() : rp;
#else
      RegI32 rv = rp;
#endif
      tls = maybeLoadTlsForAccess(check);
      if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3)) {
        return false;
      }
      pushI32(rv);
      if (rp != rv) {
        freeI32(rp);
      }
      break;
    }
    case ValType::I64: {
      RegI64 rv;
      RegI32 rp;
#ifdef JS_CODEGEN_X86
      rv = specific_.abiReturnRegI64;
      needI64(rv);
      rp = popMemory32Access(access, &check);
#else
      rp = popMemory32Access(access, &check);
      rv = needI64();
#endif
      tls = maybeLoadTlsForAccess(check);
      if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3)) {
        return false;
      }
      pushI64(rv);
      freeI32(rp);
      break;
    }
    case ValType::F32: {
      RegI32 rp = popMemory32Access(access, &check);
      RegF32 rv = needF32();
      tls = maybeLoadTlsForAccess(check);
      if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3)) {
        return false;
      }
      pushF32(rv);
      freeI32(rp);
      break;
    }
    case ValType::F64: {
      RegI32 rp = popMemory32Access(access, &check);
      RegF64 rv = needF64();
      tls = maybeLoadTlsForAccess(check);
      if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3)) {
        return false;
      }
      pushF64(rv);
      freeI32(rp);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegI32 rp = popMemory32Access(access, &check);
      RegV128 rv = needV128();
      tls = maybeLoadTlsForAccess(check);
      if (!load(access, &check, tls, rp, AnyReg(rv), temp1, temp2, temp3)) {
        return false;
      }
      pushV128(rv);
      freeI32(rp);
      break;
    }
#endif
    default:
      MOZ_CRASH("load type");
      break;
  }

  maybeFree(tls);
  maybeFree(temp1);
  maybeFree(temp2);
  maybeFree(temp3);

  return true;
}

bool BaseCompiler::emitLoad(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoad(type, Scalar::byteSize(viewType), &addr)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset());
  return loadCommon(&access, AccessCheck(), type);
}

bool BaseCompiler::storeCommon(MemoryAccessDesc* access, AccessCheck check,
                               ValType resultType) {
  RegI32 tls;
  RegI32 temp = needStoreTemp(*access, resultType);

  switch (resultType.kind()) {
    case ValType::I32: {
      RegI32 rv = popI32();
      RegI32 rp = popMemory32Access(access, &check);
      tls = maybeLoadTlsForAccess(check);
      if (!store(access, &check, tls, rp, AnyReg(rv), temp)) {
        return false;
      }
      freeI32(rp);
      freeI32(rv);
      break;
    }
    case ValType::I64: {
      RegI64 rv = popI64();
      RegI32 rp = popMemory32Access(access, &check);
      tls = maybeLoadTlsForAccess(check);
      if (!store(access, &check, tls, rp, AnyReg(rv), temp)) {
        return false;
      }
      freeI32(rp);
      freeI64(rv);
      break;
    }
    case ValType::F32: {
      RegF32 rv = popF32();
      RegI32 rp = popMemory32Access(access, &check);
      tls = maybeLoadTlsForAccess(check);
      if (!store(access, &check, tls, rp, AnyReg(rv), temp)) {
        return false;
      }
      freeI32(rp);
      freeF32(rv);
      break;
    }
    case ValType::F64: {
      RegF64 rv = popF64();
      RegI32 rp = popMemory32Access(access, &check);
      tls = maybeLoadTlsForAccess(check);
      if (!store(access, &check, tls, rp, AnyReg(rv), temp)) {
        return false;
      }
      freeI32(rp);
      freeF64(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 rv = popV128();
      RegI32 rp = popMemory32Access(access, &check);
      tls = maybeLoadTlsForAccess(check);
      if (!store(access, &check, tls, rp, AnyReg(rv), temp)) {
        return false;
      }
      freeI32(rp);
      freeV128(rv);
      break;
    }
#endif
    default:
      MOZ_CRASH("store type");
      break;
  }

  maybeFree(tls);
  maybeFree(temp);

  return true;
}

bool BaseCompiler::emitStore(ValType resultType, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused_value;
  if (!iter_.readStore(resultType, Scalar::byteSize(viewType), &addr,
                       &unused_value)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset());
  return storeCommon(&access, AccessCheck(), resultType);
}

bool BaseCompiler::emitSelect(bool typed) {
  StackType type;
  Nothing unused_trueValue;
  Nothing unused_falseValue;
  Nothing unused_condition;
  if (!iter_.readSelect(typed, &type, &unused_trueValue, &unused_falseValue,
                        &unused_condition)) {
    return false;
  }

  if (deadCode_) {
    resetLatentOp();
    return true;
  }

  // I32 condition on top, then false, then true.

  Label done;
  BranchState b(&done);
  emitBranchSetup(&b);

  switch (type.valType().kind()) {
    case ValType::I32: {
      RegI32 r, rs;
      pop2xI32(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
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
      if (!emitBranchPerform(&b)) {
        return false;
      }
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
      if (!emitBranchPerform(&b)) {
        return false;
      }
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
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveF32(rs, r);
      masm.bind(&done);
      freeF32(rs);
      pushF32(r);
      break;
    }
    case ValType::F64: {
      RegF64 r, rs;
      pop2xF64(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveF64(rs, r);
      masm.bind(&done);
      freeF64(rs);
      pushF64(r);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 r, rs;
      pop2xV128(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveV128(rs, r);
      masm.bind(&done);
      freeV128(rs);
      pushV128(r);
      break;
    }
#endif
    case ValType::Ref: {
      RegRef r, rs;
      pop2xRef(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveRef(rs, r);
      masm.bind(&done);
      freeRef(rs);
      pushRef(r);
      break;
    }
    default: {
      MOZ_CRASH("select type");
    }
  }

  return true;
}

void BaseCompiler::emitCompareI32(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::I32);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  int32_t c;
  if (popConst(&c)) {
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

void BaseCompiler::emitCompareI64(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::I64);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  RegI64 rs0, rs1;
  pop2xI64(&rs0, &rs1);
  RegI32 rd(fromI64(rs0));
  cmp64Set(compareOp, rs0, rs1, rd);
  freeI64(rs1);
  freeI64Except(rs0, rd);
  pushI32(rd);
}

void BaseCompiler::emitCompareF32(Assembler::DoubleCondition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::F32);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

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

void BaseCompiler::emitCompareF64(Assembler::DoubleCondition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::F64);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

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

void BaseCompiler::emitCompareRef(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(!sniffConditionalControlCmp(compareOp, compareType));

  RegRef rs1, rs2;
  pop2xRef(&rs1, &rs2);
  RegI32 rd = needI32();
  masm.cmpPtrSet(compareOp, rs1, rs2, rd);
  freeRef(rs1);
  freeRef(rs2);
  pushI32(rd);
}

bool BaseCompiler::emitInstanceCall(uint32_t lineOrBytecode,
                                    const SymbolicAddressSignature& builtin) {
  const MIRType* argTypes = builtin.argTypes;
  MOZ_ASSERT(argTypes[0] == MIRType::Pointer);

  sync();

  uint32_t numNonInstanceArgs = builtin.numArgs - 1 /* instance */;
  size_t stackSpace = stackConsumed(numNonInstanceArgs);

  FunctionCall baselineCall(lineOrBytecode);
  beginCall(baselineCall, UseABI::System, InterModule::True);

  ABIArg instanceArg = reservePointerArgument(&baselineCall);

  startCallArgs(StackArgAreaSizeUnaligned(builtin), &baselineCall);
  for (uint32_t i = 1; i < builtin.numArgs; i++) {
    ValType t;
    switch (argTypes[i]) {
      case MIRType::Int32:
        t = ValType::I32;
        break;
      case MIRType::Int64:
        t = ValType::I64;
        break;
      case MIRType::RefOrNull:
        t = RefType::extern_();
        break;
      case MIRType::Pointer:
        // Instance function args can now be uninterpreted pointers (eg, for
        // the cases PostBarrier and PostBarrierFilter) so we simply treat
        // them like the equivalently sized integer.
        t = ValType::hostPtr();
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
    passArg(t, peek(numNonInstanceArgs - i), &baselineCall);
  }
  CodeOffset raOffset =
      builtinInstanceMethodCall(builtin, instanceArg, baselineCall);
  if (!createStackMap("emitInstanceCall", raOffset)) {
    return false;
  }

  endCall(baselineCall, stackSpace);

  popValueStackBy(numNonInstanceArgs);

  // Note, many clients of emitInstanceCall currently assume that pushing the
  // result here does not destroy ReturnReg.
  //
  // Furthermore, clients assume that if builtin.retType != MIRType::None, the
  // callee will have returned a result and left it in ReturnReg for us to
  // find, and that that register will not be destroyed here (or above).

  // For the return type only, MIRType::None is used to indicate that the
  // call doesn't return a result, that is, returns a C/C++ "void".

  if (builtin.retType != MIRType::None) {
    pushReturnValueOfCall(baselineCall, builtin.retType);
  }
  return true;
}

bool BaseCompiler::emitMemoryGrow() {
  return emitInstanceCallOp(SASigMemoryGrow, [this]() -> bool {
    Nothing arg;
    return iter_.readMemoryGrow(&arg);
  });
}

bool BaseCompiler::emitMemorySize() {
  return emitInstanceCallOp(
      SASigMemorySize, [this]() -> bool { return iter_.readMemorySize(); });
}

bool BaseCompiler::emitRefFunc() {
  return emitInstanceCallOp<uint32_t>(SASigRefFunc,
                                      [this](uint32_t* funcIndex) -> bool {
                                        return iter_.readRefFunc(funcIndex);
                                      });
}

bool BaseCompiler::emitRefNull() {
  RefType type;
  if (!iter_.readRefNull(&type)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  pushRef(NULLREF_VALUE);
  return true;
}

bool BaseCompiler::emitRefIsNull() {
  Nothing nothing;
  if (!iter_.readRefIsNull(&nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef r = popRef();
  RegI32 rd = narrowRef(r);

  masm.cmpPtrSet(Assembler::Equal, r, ImmWord(NULLREF_VALUE), rd);
  pushI32(rd);
  return true;
}

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
bool BaseCompiler::emitRefAsNonNull() {
  Nothing nothing;
  if (!iter_.readRefAsNonNull(&nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef rp = popRef();
  Label ok;
  masm.branchTestPtr(Assembler::NonZero, rp, rp, &ok);
  trap(Trap::NullPointerDereference);
  masm.bind(&ok);
  pushRef(rp);

  return true;
}
#endif

bool BaseCompiler::emitAtomicCmpXchg(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused{};

  if (!iter_.readAtomicCmpXchg(&addr, type, Scalar::byteSize(viewType), &unused,
                               &unused)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset(),
                          Synchronization::Full());

  if (Scalar::byteSize(viewType) <= 4) {
    PopAtomicCmpXchg32Regs regs(this, type, viewType);

    AccessCheck check;
    RegI32 rp = popMemory32Access(&access, &check);
    RegI32 tls = maybeLoadTlsForAccess(check);

    auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
    regs.atomicCmpXchg32(access, memaddr);

    maybeFree(tls);
    freeI32(rp);

    if (type == ValType::I64) {
      pushU32AsI64(regs.takeRd());
    } else {
      pushI32(regs.takeRd());
    }

    return true;
  }

  MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

  PopAtomicCmpXchg64Regs regs(this);

  AccessCheck check;
  RegI32 rp = popMemory32Access(&access, &check);

#ifdef JS_CODEGEN_X86
  ScratchEBX ebx(*this);
  RegI32 tls = maybeLoadTlsForAccess(check, ebx);
  auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
  regs.atomicCmpXchg64(access, memaddr, ebx);
#else
  RegI32 tls = maybeLoadTlsForAccess(check);
  auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
  regs.atomicCmpXchg64(access, memaddr);
  maybeFree(tls);
#endif

  freeI32(rp);

  pushI64(regs.takeRd());
  return true;
}

bool BaseCompiler::emitAtomicLoad(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readAtomicLoad(&addr, type, Scalar::byteSize(viewType))) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset(),
                          Synchronization::Load());

  if (Scalar::byteSize(viewType) <= sizeof(void*)) {
    return loadCommon(&access, AccessCheck(), type);
  }

  MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

#if defined(JS_64BIT)
  MOZ_CRASH("Should not happen");
#else
  PopAtomicLoad64Regs regs(this);

  AccessCheck check;
  RegI32 rp = popMemory32Access(&access, &check);

#  ifdef JS_CODEGEN_X86
  ScratchEBX ebx(*this);
  RegI32 tls = maybeLoadTlsForAccess(check, ebx);
  auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
  regs.atomicLoad64(access, memaddr, ebx);
#  else
  RegI32 tls = maybeLoadTlsForAccess(check);
  auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
  regs.atomicLoad64(access, memaddr);
  maybeFree(tls);
#  endif

  freeI32(rp);

  pushI64(regs.takeRd());
  return true;
#endif  // JS_64BIT
}

bool BaseCompiler::emitAtomicRMW(ValType type, Scalar::Type viewType,
                                 AtomicOp op) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused_value;
  if (!iter_.readAtomicRMW(&addr, type, Scalar::byteSize(viewType),
                           &unused_value)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset(),
                          Synchronization::Full());

  if (Scalar::byteSize(viewType) <= 4) {
    PopAtomicRMW32Regs regs(this, type, viewType, op);

    AccessCheck check;
    RegI32 rp = popMemory32Access(&access, &check);
    RegI32 tls = maybeLoadTlsForAccess(check);

    auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
    regs.atomicRMW32(access, memaddr, op);

    maybeFree(tls);
    freeI32(rp);

    if (type == ValType::I64) {
      pushU32AsI64(regs.takeRd());
    } else {
      pushI32(regs.takeRd());
    }
    return true;
  }

  MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

  PopAtomicRMW64Regs regs(this, op);

  AccessCheck check;
  RegI32 rp = popMemory32Access(&access, &check);

#ifdef JS_CODEGEN_X86
  ScratchEBX ebx(*this);
  RegI32 tls = maybeLoadTlsForAccess(check, ebx);

  fr.pushGPR(regs.valueHigh());
  fr.pushGPR(regs.valueLow());
  Address value(esp, 0);

  auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
  regs.atomicRMW64(access, memaddr, op, value, ebx);

  fr.popBytes(8);
#else
  RegI32 tls = maybeLoadTlsForAccess(check);
  auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
  regs.atomicRMW64(access, memaddr, op);
  maybeFree(tls);
#endif

  freeI32(rp);

  pushI64(regs.takeRd());
  return true;
}

bool BaseCompiler::emitAtomicStore(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused_value;
  if (!iter_.readAtomicStore(&addr, type, Scalar::byteSize(viewType),
                             &unused_value)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset(),
                          Synchronization::Store());

  if (Scalar::byteSize(viewType) <= sizeof(void*)) {
    return storeCommon(&access, AccessCheck(), type);
  }

  MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

#ifdef JS_64BIT
  MOZ_CRASH("Should not happen");
#else
  emitAtomicXchg64(&access, WantResult(false));
  return true;
#endif
}

bool BaseCompiler::emitAtomicXchg(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  Nothing unused_value;
  if (!iter_.readAtomicRMW(&addr, type, Scalar::byteSize(viewType),
                           &unused_value)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  AccessCheck check;
  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset(),
                          Synchronization::Full());

  if (Scalar::byteSize(viewType) <= 4) {
    PopAtomicXchg32Regs regs(this, type, viewType);
    RegI32 rp = popMemory32Access(&access, &check);
    RegI32 tls = maybeLoadTlsForAccess(check);

    auto memaddr = prepareAtomicMemoryAccess(&access, &check, tls, rp);
    regs.atomicXchg32(access, memaddr);

    maybeFree(tls);
    freeI32(rp);

    if (type == ValType::I64) {
      pushU32AsI64(regs.takeRd());
    } else {
      pushI32(regs.takeRd());
    }
    return true;
  }

  MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

  emitAtomicXchg64(&access, WantResult(true));
  return true;
}

void BaseCompiler::emitAtomicXchg64(MemoryAccessDesc* access,
                                    WantResult wantResult) {
  PopAtomicXchg64Regs regs(this);

  AccessCheck check;
  RegI32 rp = popMemory32Access(access, &check);

#ifdef JS_CODEGEN_X86
  ScratchEBX ebx(*this);
  RegI32 tls = maybeLoadTlsForAccess(check, ebx);
  auto memaddr = prepareAtomicMemoryAccess(access, &check, tls, rp);
  regs.atomicXchg64(*access, memaddr, ebx);
#else
  RegI32 tls = maybeLoadTlsForAccess(check);
  auto memaddr = prepareAtomicMemoryAccess(access, &check, tls, rp);
  regs.atomicXchg64(*access, memaddr);
  maybeFree(tls);
#endif

  freeI32(rp);

  if (wantResult) {
    pushI64(regs.takeRd());
  }
}

bool BaseCompiler::emitWait(ValType type, uint32_t byteSize) {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readWait(&addr, type, byteSize, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  switch (type.kind()) {
    case ValType::I32: {
      RegI64 timeout = popI64();
      RegI32 val = popI32();

      MemoryAccessDesc access(Scalar::Int32, addr.align, addr.offset,
                              bytecodeOffset());
      computeEffectiveAddress(&access);

      pushI32(val);
      pushI64(timeout);

      if (!emitInstanceCall(lineOrBytecode, SASigWaitI32)) {
        return false;
      }
      break;
    }
    case ValType::I64: {
      RegI64 timeout = popI64();
      RegI64 val = popI64();

      MemoryAccessDesc access(Scalar::Int64, addr.align, addr.offset,
                              bytecodeOffset());
      computeEffectiveAddress(&access);

      pushI64(val);
      pushI64(timeout);

      if (!emitInstanceCall(lineOrBytecode, SASigWaitI64)) {
        return false;
      }
      break;
    }
    default:
      MOZ_CRASH();
  }

  return true;
}

bool BaseCompiler::emitWake() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readWake(&addr, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegI32 count = popI32();

  MemoryAccessDesc access(Scalar::Int32, addr.align, addr.offset,
                          bytecodeOffset());
  computeEffectiveAddress(&access);

  pushI32(count);

  return emitInstanceCall(lineOrBytecode, SASigWake);
}

bool BaseCompiler::emitFence() {
  if (!iter_.readFence()) {
    return false;
  }
  if (deadCode_) {
    return true;
  }

  masm.memoryBarrier(MembarFull);
  return true;
}

bool BaseCompiler::emitMemCopy() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t dstMemOrTableIndex = 0;
  uint32_t srcMemOrTableIndex = 0;
  Nothing nothing;
  if (!iter_.readMemOrTableCopy(true, &dstMemOrTableIndex, &nothing,
                                &srcMemOrTableIndex, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  int32_t signedLength;
  if (MacroAssembler::SupportsFastUnalignedAccesses() &&
      peekConst(&signedLength) && signedLength != 0 &&
      uint32_t(signedLength) <= MaxInlineMemoryCopyLength) {
    return emitMemCopyInline();
  }

  return emitMemCopyCall(lineOrBytecode);
}

bool BaseCompiler::emitMemCopyCall(uint32_t lineOrBytecode) {
  pushHeapBase();
  return emitInstanceCall(lineOrBytecode, usesSharedMemory()
                                              ? SASigMemCopyShared32
                                              : SASigMemCopy32);
}

bool BaseCompiler::emitMemCopyInline() {
  MOZ_ASSERT(MaxInlineMemoryCopyLength != 0);

  int32_t signedLength;
  MOZ_ALWAYS_TRUE(popConst(&signedLength));
  uint32_t length = signedLength;
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryCopyLength);

  RegI32 src = popI32();
  RegI32 dest = popI32();

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  // Load all source bytes onto the value stack from low to high using the
  // widest transfer width we can for the system. We will trap without writing
  // anything if any source byte is out-of-bounds.
  bool omitBoundsCheck = false;
  size_t offset = 0;

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(Scalar::Int64, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!loadCommon(&access, check, ValType::I64)) {
      return false;
    }

    offset += sizeof(uint64_t);
    omitBoundsCheck = true;
  }
#endif

  for (uint32_t i = 0; i < numCopies4; i++) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(Scalar::Uint32, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!loadCommon(&access, check, ValType::I32)) {
      return false;
    }

    offset += sizeof(uint32_t);
    omitBoundsCheck = true;
  }

  if (numCopies2) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(Scalar::Uint16, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!loadCommon(&access, check, ValType::I32)) {
      return false;
    }

    offset += sizeof(uint16_t);
    omitBoundsCheck = true;
  }

  if (numCopies1) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(Scalar::Uint8, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!loadCommon(&access, check, ValType::I32)) {
      return false;
    }
  }

  // Store all source bytes from the value stack to the destination from
  // high to low. We will trap without writing anything on the first store
  // if any dest byte is out-of-bounds.
  offset = length;
  omitBoundsCheck = false;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    RegI32 value = popI32();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(value);

    MemoryAccessDesc access(Scalar::Uint8, 1, offset, bytecodeOffset());
    AccessCheck check;
    if (!storeCommon(&access, check, ValType::I32)) {
      return false;
    }

    omitBoundsCheck = true;
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    RegI32 value = popI32();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(value);

    MemoryAccessDesc access(Scalar::Uint16, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!storeCommon(&access, check, ValType::I32)) {
      return false;
    }

    omitBoundsCheck = true;
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    RegI32 value = popI32();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(value);

    MemoryAccessDesc access(Scalar::Uint32, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!storeCommon(&access, check, ValType::I32)) {
      return false;
    }

    omitBoundsCheck = true;
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    RegI64 value = popI64();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI64(value);

    MemoryAccessDesc access(Scalar::Int64, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!storeCommon(&access, check, ValType::I64)) {
      return false;
    }

    omitBoundsCheck = true;
  }
#endif

  freeI32(dest);
  freeI32(src);
  return true;
}

bool BaseCompiler::emitTableCopy() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t dstMemOrTableIndex = 0;
  uint32_t srcMemOrTableIndex = 0;
  Nothing nothing;
  if (!iter_.readMemOrTableCopy(false, &dstMemOrTableIndex, &nothing,
                                &srcMemOrTableIndex, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  pushI32(dstMemOrTableIndex);
  pushI32(srcMemOrTableIndex);
  return emitInstanceCall(lineOrBytecode, SASigTableCopy);
}

bool BaseCompiler::emitDataOrElemDrop(bool isData) {
  return emitInstanceCallOp<uint32_t>(
      isData ? SASigDataDrop : SASigElemDrop, [&](uint32_t* segIndex) -> bool {
        return iter_.readDataOrElemDrop(isData, segIndex);
      });
}

bool BaseCompiler::emitMemFill() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  Nothing nothing;
  if (!iter_.readMemFill(&nothing, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  int32_t signedLength;
  int32_t signedValue;
  if (MacroAssembler::SupportsFastUnalignedAccesses() &&
      peek2xConst(&signedLength, &signedValue) && signedLength != 0 &&
      uint32_t(signedLength) <= MaxInlineMemoryFillLength) {
    return emitMemFillInline();
  }
  return emitMemFillCall(lineOrBytecode);
}

bool BaseCompiler::emitMemFillCall(uint32_t lineOrBytecode) {
  pushHeapBase();
  return emitInstanceCall(lineOrBytecode, usesSharedMemory()
                                              ? SASigMemFillShared32
                                              : SASigMemFill32);
}

bool BaseCompiler::emitMemFillInline() {
  MOZ_ASSERT(MaxInlineMemoryFillLength != 0);

  int32_t signedLength;
  int32_t signedValue;
  MOZ_ALWAYS_TRUE(popConst(&signedLength));
  MOZ_ALWAYS_TRUE(popConst(&signedValue));
  uint32_t length = uint32_t(signedLength);
  uint32_t value = uint32_t(signedValue);
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryFillLength);

  RegI32 dest = popI32();

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  MOZ_ASSERT(numCopies2 <= 1 && numCopies1 <= 1);

  // Generate splatted definitions for wider fills as needed
#ifdef JS_64BIT
  uint64_t val8 = SplatByteToUInt<uint64_t>(value, 8);
#endif
  uint32_t val4 = SplatByteToUInt<uint32_t>(value, 4);
  uint32_t val2 = SplatByteToUInt<uint32_t>(value, 2);
  uint32_t val1 = value;

  // Store the fill value to the destination from high to low. We will trap
  // without writing anything on the first store if any dest byte is
  // out-of-bounds.
  size_t offset = length;
  bool omitBoundsCheck = false;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(val1);

    MemoryAccessDesc access(Scalar::Uint8, 1, offset, bytecodeOffset());
    AccessCheck check;
    if (!storeCommon(&access, check, ValType::I32)) {
      return false;
    }

    omitBoundsCheck = true;
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(val2);

    MemoryAccessDesc access(Scalar::Uint16, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!storeCommon(&access, check, ValType::I32)) {
      return false;
    }

    omitBoundsCheck = true;
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(val4);

    MemoryAccessDesc access(Scalar::Uint32, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!storeCommon(&access, check, ValType::I32)) {
      return false;
    }

    omitBoundsCheck = true;
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI64(val8);

    MemoryAccessDesc access(Scalar::Int64, 1, offset, bytecodeOffset());
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    if (!storeCommon(&access, check, ValType::I64)) {
      return false;
    }

    omitBoundsCheck = true;
  }
#endif

  freeI32(dest);
  return true;
}

bool BaseCompiler::emitMemInit() {
  return emitInstanceCallOp<uint32_t>(
      SASigMemInit32, [this](uint32_t* segIndex) -> bool {
        uint32_t dstTableIndex;
        Nothing nothing;
        return iter_.readMemOrTableInit(/*isMem*/ true, segIndex,
                                        &dstTableIndex, &nothing, &nothing,
                                        &nothing);
      });
}

bool BaseCompiler::emitTableInit() {
  return emitInstanceCallOp<uint32_t, uint32_t>(
      SASigTableInit,
      [this](uint32_t* segIndex, uint32_t* dstTableIndex) -> bool {
        Nothing nothing;
        return iter_.readMemOrTableInit(/*isMem*/ false, segIndex,
                                        dstTableIndex, &nothing, &nothing,
                                        &nothing);
      });
}

[[nodiscard]] bool BaseCompiler::emitTableFill() {
  // fill(start:u32, val:ref, len:u32, table:u32) -> void
  return emitInstanceCallOp<uint32_t>(
      SASigTableFill, [this](uint32_t* tableIndex) -> bool {
        Nothing nothing;
        return iter_.readTableFill(tableIndex, &nothing, &nothing, &nothing);
      });
}

[[nodiscard]] bool BaseCompiler::emitTableGet() {
  // get(index:u32, table:u32) -> AnyRef
  return emitInstanceCallOp<uint32_t>(
      SASigTableGet, [this](uint32_t* tableIndex) -> bool {
        Nothing nothing;
        return iter_.readTableGet(tableIndex, &nothing);
      });
}

[[nodiscard]] bool BaseCompiler::emitTableGrow() {
  // grow(initValue:anyref, delta:u32, table:u32) -> u32
  return emitInstanceCallOp<uint32_t>(
      SASigTableGrow, [this](uint32_t* tableIndex) -> bool {
        Nothing nothing;
        return iter_.readTableGrow(tableIndex, &nothing, &nothing);
      });
}

[[nodiscard]] bool BaseCompiler::emitTableSet() {
  // set(index:u32, value:ref, table:u32) -> void
  return emitInstanceCallOp<uint32_t>(
      SASigTableSet, [this](uint32_t* tableIndex) -> bool {
        Nothing nothing;
        return iter_.readTableSet(tableIndex, &nothing, &nothing);
      });
}

[[nodiscard]] bool BaseCompiler::emitTableSize() {
  // size(table:u32) -> u32
  return emitInstanceCallOp<uint32_t>(SASigTableSize,
                                      [this](uint32_t* tableIndex) -> bool {
                                        return iter_.readTableSize(tableIndex);
                                      });
}

void BaseCompiler::emitGcNullCheck(RegRef rp) {
  Label ok;
  masm.branchTestPtr(Assembler::NonZero, rp, rp, &ok);
  trap(Trap::NullPointerDereference);
  masm.bind(&ok);
}

RegPtr BaseCompiler::emitGcArrayGetData(RegRef rp) {
  RegPtr rdata = needPtr();
  // An array is always an outline typed object
  masm.loadPtr(Address(rp, OutlineTypedObject::offsetOfData()), rdata);
  return rdata;
}

RegI32 BaseCompiler::emitGcArrayGetLength(RegPtr rdata,
                                          bool adjustDataPointer) {
  STATIC_ASSERT_ARRAYLENGTH_IS_U32;
  RegI32 length = needI32();
  masm.load32(Address(rdata, OutlineTypedObject::offsetOfArrayLength()),
              length);
  if (adjustDataPointer) {
    masm.addPtr(ImmWord(OutlineTypedObject::offsetOfArrayLength() +
                        sizeof(OutlineTypedObject::ArrayLength)),
                rdata);
  }
  return length;
}

void BaseCompiler::emitGcArrayBoundsCheck(RegI32 index, RegI32 length) {
  Label inBounds;
  masm.branch32(Assembler::Below, index, length, &inBounds);
  masm.wasmTrap(Trap::OutOfBounds, bytecodeOffset());
  masm.bind(&inBounds);
}

template <typename T>
void BaseCompiler::emitGcGet(FieldType type, FieldExtension extension,
                             const T& src) {
  switch (type.kind()) {
    case FieldType::I8: {
      MOZ_ASSERT(extension != FieldExtension::None);
      RegI32 r = needI32();
      if (extension == FieldExtension::Unsigned) {
        masm.load8ZeroExtend(src, r);
      } else {
        masm.load8SignExtend(src, r);
      }
      pushI32(r);
      break;
    }
    case FieldType::I16: {
      MOZ_ASSERT(extension != FieldExtension::None);
      RegI32 r = needI32();
      if (extension == FieldExtension::Unsigned) {
        masm.load16ZeroExtend(src, r);
      } else {
        masm.load16SignExtend(src, r);
      }
      pushI32(r);
      break;
    }
    case FieldType::I32: {
      MOZ_ASSERT(extension == FieldExtension::None);
      RegI32 r = needI32();
      masm.load32(src, r);
      pushI32(r);
      break;
    }
    case FieldType::I64: {
      MOZ_ASSERT(extension == FieldExtension::None);
      RegI64 r = needI64();
      masm.load64(src, r);
      pushI64(r);
      break;
    }
    case FieldType::F32: {
      MOZ_ASSERT(extension == FieldExtension::None);
      RegF32 r = needF32();
      masm.loadFloat32(src, r);
      pushF32(r);
      break;
    }
    case FieldType::F64: {
      MOZ_ASSERT(extension == FieldExtension::None);
      RegF64 r = needF64();
      masm.loadDouble(src, r);
      pushF64(r);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case FieldType::V128: {
      MOZ_ASSERT(extension == FieldExtension::None);
      RegV128 r = needV128();
      masm.loadUnalignedSimd128(src, r);
      pushV128(r);
      break;
    }
#endif
    case FieldType::Ref: {
      MOZ_ASSERT(extension == FieldExtension::None);
      RegRef r = needRef();
      masm.loadPtr(src, r);
      pushRef(r);
      break;
    }
    default: {
      MOZ_CRASH("Unexpected field type");
    }
  }
}

template <typename T>
void BaseCompiler::emitGcSetScalar(const T& dst, FieldType type, AnyReg value) {
  switch (type.kind()) {
    case FieldType::I8: {
      masm.store8(value.i32(), dst);
      break;
    }
    case FieldType::I16: {
      masm.store16(value.i32(), dst);
      break;
    }
    case FieldType::I32: {
      masm.store32(value.i32(), dst);
      break;
    }
    case FieldType::I64: {
      masm.store64(value.i64(), dst);
      break;
    }
    case FieldType::F32: {
      masm.storeFloat32(value.f32(), dst);
      break;
    }
    case FieldType::F64: {
      masm.storeDouble(value.f64(), dst);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case FieldType::V128: {
      masm.storeUnalignedSimd128(value.v128(), dst);
      break;
    }
#endif
    default: {
      MOZ_CRASH("Unexpected field type");
    }
  }
}

bool BaseCompiler::emitGcStructSet(RegRef object, RegPtr data,
                                   const StructField& field, AnyReg value) {
  // Easy path if the field is a scalar
  if (!field.type.isReference()) {
    emitGcSetScalar(Address(data, field.offset), field.type, value);
    freeAny(value);
    return true;
  }

  // Create temporary for the valueAddr that is not in the prebarrier register
  // and can be consumed by the barrier operation
  RegPtr valueAddr = RegPtr(PreBarrierReg);
  needPtr(valueAddr);
  masm.computeEffectiveAddress(Address(data, field.offset), valueAddr);

  // Save state for after barriered write
  pushPtr(data);

  // emitBarrieredStore preserves object and value
  if (!emitBarrieredStore(Some(object), valueAddr, value.ref())) {
    return false;
  }
  freeRef(value.ref());

  // Restore state
  popPtr(data);

  return true;
}

bool BaseCompiler::emitGcArraySet(RegRef object, RegPtr data, RegI32 index,
                                  const ArrayType& arrayType, AnyReg value) {
  // Try to use a base index store instruction if the field type fits in a
  // shift immediate. If not we shift the index manually and then unshift
  // it after the store. We don't use an extra register for this because we
  // don't have any to spare on x86.
  uint32_t shift = arrayType.elementType_.indexingShift();
  Scale scale;
  bool shiftedIndex = false;
  if (IsShiftInScaleRange(shift)) {
    scale = ShiftToScale(shift);
  } else {
    masm.lshiftPtr(Imm32(shift), index);
    scale = TimesOne;
    shiftedIndex = true;
  }
  auto unshiftIndex = mozilla::MakeScopeExit([&] {
    if (shiftedIndex) {
      masm.rshiftPtr(Imm32(shift), index);
    }
  });

  // Easy path if the field is a scalar
  if (!arrayType.elementType_.isReference()) {
    emitGcSetScalar(BaseIndex(data, index, scale, 0), arrayType.elementType_,
                    value);
    return true;
  }

  // Create temporaries for valueAddr that is not in the prebarrier register
  // and can be consumed by the barrier operation
  RegPtr valueAddr = RegPtr(PreBarrierReg);
  needPtr(valueAddr);
  masm.computeEffectiveAddress(BaseIndex(data, index, scale, 0), valueAddr);

  // Save state for after barriered write
  pushPtr(data);
  pushI32(index);

  // emitBarrieredStore preserves object and value
  if (!emitBarrieredStore(Some(object), valueAddr, value.ref())) {
    return false;
  }

  // Restore state
  popI32(index);
  popPtr(data);

  return true;
}

bool BaseCompiler::emitStructNewWithRtt() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex;
  Nothing rtt;
  NothingVector args{};
  if (!iter_.readStructNewWithRtt(&typeIndex, &rtt, &args)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const StructType& structType = moduleEnv_.types[typeIndex].structType();

  // Allocate zeroed storage.  The parameter to StructNew is a rtt value that is
  // guaranteed to be at the top of the stack by validation.
  //
  // Traps on OOM.
  if (!emitInstanceCall(lineOrBytecode, SASigStructNew)) {
    return false;
  }

  // Optimization opportunity: Iterate backward to pop arguments off the
  // stack.  This will generate more instructions than we want, since we
  // really only need to pop the stack once at the end, not for every element,
  // but to do better we need a bit more machinery to load elements off the
  // stack into registers.

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  needPtr(RegPtr(PreBarrierReg));

  RegRef rp = popRef();
  RegPtr rdata = needPtr();

  // Free the barrier reg after we've allocated all registers
  freePtr(RegPtr(PreBarrierReg));

  // The struct allocated above is guaranteed to have the exact shape of
  // structType, we don't need to branch on whether it's inline or not.
  if (InlineTypedObject::canAccommodateSize(structType.size_)) {
    masm.computeEffectiveAddress(
        Address(rp, InlineTypedObject::offsetOfDataStart()), rdata);
  } else {
    masm.loadPtr(Address(rp, OutlineTypedObject::offsetOfData()), rdata);
  }

  // Optimization opportunity: when the value being stored is a known
  // zero/null we need store nothing.  This case may be somewhat common
  // because struct.new forces a value to be specified for every field.

  uint32_t fieldIndex = structType.fields_.length();
  while (fieldIndex-- > 0) {
    const StructField& structField = structType.fields_[fieldIndex];
    // Reserve the barrier reg if we might need it for this store
    if (structField.type.isReference()) {
      needPtr(RegPtr(PreBarrierReg));
    }

    AnyReg value = popAny();

    // Free the barrier reg now that we've loaded the value
    if (structField.type.isReference()) {
      freePtr(RegPtr(PreBarrierReg));
    }

    // Consumes value. rp, and rdata are preserved
    if (!emitGcStructSet(rp, rdata, structField, value)) {
      return false;
    }
  }

  freePtr(rdata);
  pushRef(rp);

  return true;
}

bool BaseCompiler::emitStructNewDefaultWithRtt() {
  // Allocate zeroed storage.  The parameter to StructNew is a rtt value that is
  // guaranteed to be at the top of the stack by validation.
  //
  // Traps on OOM.

  // (rtt) -> ref; the type index is just dropped on the floor
  return emitInstanceCallOp(SASigStructNew, [this]() -> bool {
    uint32_t unusedTypeIndex;
    Nothing unusedRtt;
    return iter_.readStructNewDefaultWithRtt(&unusedTypeIndex, &unusedRtt);
  });
}

bool BaseCompiler::emitStructGet(FieldExtension extension) {
  uint32_t typeIndex;
  uint32_t fieldIndex;
  Nothing nothing;
  if (!iter_.readStructGet(&typeIndex, &fieldIndex, extension, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const StructType& structType = moduleEnv_.types[typeIndex].structType();

  RegRef rp = popRef();

  // Check for null
  emitGcNullCheck(rp);

  // Acquire the data pointer from the object
  RegPtr rdata = needPtr();
  {
    RegPtr scratch = needPtr();
    RegPtr clasp = needPtr();
    masm.movePtr(SymbolicAddress::InlineTypedObjectClass, clasp);
    Label join;
    Label isInline;
    masm.branchTestObjClass(Assembler::Equal, rp, clasp, scratch, rp,
                            &isInline);
    freePtr(clasp);
    freePtr(scratch);

    masm.loadPtr(Address(rp, OutlineTypedObject::offsetOfData()), rdata);
    masm.jump(&join);

    masm.bind(&isInline);
    masm.computeEffectiveAddress(
        Address(rp, InlineTypedObject::offsetOfDataStart()), rdata);
    masm.bind(&join);
  }

  // Load the value
  FieldType type = structType.fields_[fieldIndex].type;
  uint32_t offset = structType.fields_[fieldIndex].offset;
  emitGcGet(type, extension, Address(rdata, offset));

  freePtr(rdata);
  freeRef(rp);

  return true;
}

bool BaseCompiler::emitStructSet() {
  uint32_t typeIndex;
  uint32_t fieldIndex;
  Nothing nothing;
  if (!iter_.readStructSet(&typeIndex, &fieldIndex, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const StructType& structType = moduleEnv_.types[typeIndex].structType();
  const StructField& structField = structType.fields_[fieldIndex];

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  if (structField.type.isReference()) {
    needPtr(RegPtr(PreBarrierReg));
  }

  AnyReg value = popAny();
  RegRef rp = popRef();
  RegPtr rdata = needPtr();

  // Free the barrier reg after we've allocated all registers
  if (structField.type.isReference()) {
    freePtr(RegPtr(PreBarrierReg));
  }

  // Check for null
  emitGcNullCheck(rp);

  // Acquire the data pointer from the object.
  {
    // We don't have a register to spare at this point on x86, so carefully
    // borrow the `rdata` as a scratch pointer during the instruction sequence
    // that loads `rdata`.
    RegPtr scratch = rdata;
    RegPtr clasp = needPtr();
    masm.movePtr(SymbolicAddress::InlineTypedObjectClass, clasp);
    Label join;
    Label isInline;
    masm.branchTestObjClass(Assembler::Equal, rp, clasp, scratch, rp,
                            &isInline);
    freePtr(clasp);
    masm.loadPtr(Address(rp, OutlineTypedObject::offsetOfData()), rdata);
    masm.jump(&join);

    masm.bind(&isInline);
    masm.computeEffectiveAddress(
        Address(rp, InlineTypedObject::offsetOfDataStart()), rdata);
    masm.bind(&join);
  }

  // Consumes value. rp, and rdata are preserved
  if (!emitGcStructSet(rp, rdata, structField, value)) {
    return false;
  }

  freePtr(rdata);
  freeRef(rp);

  return true;
}

bool BaseCompiler::emitArrayNewWithRtt() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArrayNewWithRtt(&typeIndex, &nothing, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ArrayType& arrayType = moduleEnv_.types[typeIndex].arrayType();

  // Allocate zeroed storage.  The parameter to ArrayNew is a rtt value and
  // length that are guaranteed to be at the top of the stack by validation.
  //
  // Traps on OOM.
  if (!emitInstanceCall(lineOrBytecode, SASigArrayNew)) {
    return false;
  }

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  if (arrayType.elementType_.isReference()) {
    needPtr(RegPtr(PreBarrierReg));
  }

  RegRef rp = popRef();
  AnyReg value = popAny();

  // Acquire the data pointers from the object
  RegPtr rdata = emitGcArrayGetData(rp);

  // Acquire the array length and adjust the data pointer to be immediately
  // after the array length header
  RegI32 length = emitGcArrayGetLength(rdata, true);

  // Free the barrier reg after we've allocated all registers
  if (arrayType.elementType_.isReference()) {
    freePtr(RegPtr(PreBarrierReg));
  }

  // Perform an initialization loop using `length` as the loop variable,
  // counting down to zero.
  Label done;
  Label loop;
  // Skip initialization if length = 0
  masm.branch32(Assembler::Equal, length, Imm32(0), &done);
  masm.bind(&loop);

  // Move to the next element
  masm.sub32(Imm32(1), length);

  // Assign value to array[length]. All registers are preserved
  if (!emitGcArraySet(rp, rdata, length, arrayType, value)) {
    return false;
  }

  // Loop back if there are still elements to initialize
  masm.branch32(Assembler::GreaterThan, length, Imm32(0), &loop);
  masm.bind(&done);

  freeI32(length);
  freeAny(value);
  freePtr(rdata);
  pushRef(rp);

  return true;
}

bool BaseCompiler::emitArrayNewDefaultWithRtt() {
  // Allocate zeroed storage. The parameter to ArrayNew is a rtt value that is
  // guaranteed to be at the top of the stack by validation.
  //
  // Traps on OOM.

  // (rtt) -> ref; the type index is dropped on the floor.
  return emitInstanceCallOp(SASigArrayNew, [this]() -> bool {
    uint32_t unusedTypeIndex;
    Nothing nothing;
    return iter_.readArrayNewDefaultWithRtt(&unusedTypeIndex, &nothing,
                                            &nothing);
  });
}

bool BaseCompiler::emitArrayGet(FieldExtension extension) {
  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArrayGet(&typeIndex, extension, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ArrayType& arrayType = moduleEnv_.types[typeIndex].arrayType();

  RegI32 index = popI32();
  RegRef rp = popRef();

  // Check for null
  emitGcNullCheck(rp);

  // Acquire the data pointer from the object
  RegPtr rdata = emitGcArrayGetData(rp);

  // Acquire the array length and adjust the data pointer to be immediately
  // after the array length header
  RegI32 length = emitGcArrayGetLength(rdata, true);

  // Bounds check the index
  emitGcArrayBoundsCheck(index, length);
  freeI32(length);

  // Load the value
  uint32_t shift = arrayType.elementType_.indexingShift();
  if (IsShiftInScaleRange(shift)) {
    emitGcGet(arrayType.elementType_, extension,
              BaseIndex(rdata, index, ShiftToScale(shift), 0));
  } else {
    masm.lshiftPtr(Imm32(shift), index);
    emitGcGet(arrayType.elementType_, extension,
              BaseIndex(rdata, index, TimesOne, 0));
  }

  freePtr(rdata);
  freeRef(rp);
  freeI32(index);

  return true;
}

bool BaseCompiler::emitArraySet() {
  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArraySet(&typeIndex, &nothing, &nothing, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const ArrayType& arrayType = moduleEnv_.types[typeIndex].arrayType();

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  if (arrayType.elementType_.isReference()) {
    needPtr(RegPtr(PreBarrierReg));
  }

  AnyReg value = popAny();
  RegI32 index = popI32();
  RegRef rp = popRef();

  // We run out of registers on x86 with this instruction, so stash `value` on
  // the stack until it is needed later.
  pushAny(value);

  // Check for null
  emitGcNullCheck(rp);

  // Acquire the data pointer from the object
  RegPtr rdata = emitGcArrayGetData(rp);

  // Acquire the array length and adjust the data pointer to be immediately
  // after the array length header
  RegI32 length = emitGcArrayGetLength(rdata, true);

  // Free the barrier reg after we've allocated all registers
  if (arrayType.elementType_.isReference()) {
    freePtr(RegPtr(PreBarrierReg));
  }

  // Bounds check the index
  emitGcArrayBoundsCheck(index, length);
  freeI32(length);

  // Pull the value out of the stack now that we need it.
  popAny(value);

  // All registers are preserved. This isn't strictly necessary, as we'll just
  // be freeing them all after this is done. But this is needed for repeated
  // assignments used in array.new/new_default.
  if (!emitGcArraySet(rp, rdata, index, arrayType, value)) {
    return false;
  }

  freePtr(rdata);
  freeRef(rp);
  freeI32(index);
  freeAny(value);

  return true;
}

bool BaseCompiler::emitArrayLen() {
  uint32_t typeIndex;
  Nothing nothing;
  if (!iter_.readArrayLen(&typeIndex, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef rp = popRef();

  // Check for null
  emitGcNullCheck(rp);

  // Acquire the data pointer from the object
  RegPtr rdata = emitGcArrayGetData(rp);
  freeRef(rp);

  // Acquire the length from the array
  pushI32(emitGcArrayGetLength(rdata, false));
  freePtr(rdata);

  return true;
}

bool BaseCompiler::emitRttCanon() {
  ValType rttType;
  if (!iter_.readRttCanon(&rttType)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  const TypeIdDesc& typeId = moduleEnv_.typeIds[rttType.typeIndex()];
  RegRef rp = needRef();
  fr.loadTlsPtr(WasmTlsReg);
  masm.loadWasmGlobalPtr(typeId.globalDataOffset(), rp);
  pushRef(rp);
  return true;
}

bool BaseCompiler::emitRttSub() {
  // rttSub builtin has same signature as rtt.sub instruction, stack is
  // guaranteed to be in the right condition due to validation.
  return emitInstanceCallOp(SASigRttSub, [this]() -> bool {
    Nothing nothing;
    return iter_.readRttSub(&nothing);
  });
}

bool BaseCompiler::emitRefTest() {
  // refTest builtin has same signature as ref.test instruction, stack is
  // guaranteed to be in the right condition due to validation.
  return emitInstanceCallOp(SASigRefTest, [this]() -> bool {
    Nothing nothing;
    uint32_t unusedRttTypeIndex;
    uint32_t unusedRttDepth;
    return iter_.readRefTest(&nothing, &unusedRttTypeIndex, &unusedRttDepth,
                             &nothing);
  });
}

bool BaseCompiler::emitRefCast() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  Nothing nothing;
  uint32_t rttTypeIndex;
  uint32_t rttDepth;
  if (!iter_.readRefCast(&nothing, &rttTypeIndex, &rttDepth, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegRef rttPtr = popRef();
  RegRef refPtr = popRef();

  // 1. duplicate and shuffle from [ref, rtt] to [ref, ref, rtt]
  RegRef castedPtr = needRef();
  moveRef(refPtr, castedPtr);
  pushRef(castedPtr);
  pushRef(refPtr);
  pushRef(rttPtr);

  // 2. ref.test : [ref, rtt] -> [i32]
  if (!emitInstanceCall(lineOrBytecode, SASigRefTest)) {
    return false;
  }

  // 3. trap if result is zero, leaving [ref] as result
  RegI32 result = popI32();
  Label nonZero;
  masm.branchTest32(Assembler::NonZero, result, result, &nonZero);
  masm.wasmTrap(Trap::BadCast, bytecodeOffset());
  masm.bind(&nonZero);
  freeI32(result);

  return true;
}

bool BaseCompiler::emitBrOnCast() {
  MOZ_ASSERT(!hasLatentOp());

  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();
  uint32_t relativeDepth;
  Nothing unused{};
  NothingVector unused_values{};
  uint32_t rttTypeIndex;
  uint32_t rttDepth;
  ResultType branchTargetType;
  if (!iter_.readBrOnCast(&relativeDepth, &unused, &rttTypeIndex, &rttDepth,
                          &branchTargetType, &unused_values)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  RegRef rttPtr = popRef();
  RegRef refPtr = popRef();

  // 1. duplicate and shuffle from [T*, ref, rtt] to [T*, ref, ref, rtt]
  RegRef castedPtr = needRef();
  moveRef(refPtr, castedPtr);
  pushRef(castedPtr);
  pushRef(refPtr);
  pushRef(rttPtr);

  // 2. ref.test : [ref, rtt] -> [i32]
  if (!emitInstanceCall(lineOrBytecode, SASigRefTest)) {
    return false;
  }

  // 3. br_if $l : [T*, ref, i32] -> [T*, ref]
  BranchState b(&target.label, target.stackHeight, InvertBranch(false),
                branchTargetType);
  if (b.hasBlockResults()) {
    needResultRegisters(b.resultType);
  }
  RegI32 condition = popI32();
  if (b.hasBlockResults()) {
    freeResultRegisters(b.resultType);
  }
  if (!jumpConditionalWithResults(&b, Assembler::NotEqual, condition,
                                  Imm32(0))) {
    return false;
  }
  freeI32(condition);

  return true;
}

#ifdef ENABLE_WASM_SIMD

// Emitter trampolines used by abstracted SIMD operations.  Naming here follows
// the SIMD spec pretty closely.

static void AndV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseAndSimd128(rs, rsd);
}

static void OrV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseOrSimd128(rs, rsd);
}

static void XorV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseXorSimd128(rs, rsd);
}

static void AddI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt8x16(rs, rsd);
}

static void AddI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt16x8(rs, rsd);
}

static void AddI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt32x4(rs, rsd);
}

static void AddF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addFloat32x4(rs, rsd);
}

static void AddI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt64x2(rs, rsd);
}

static void AddF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addFloat64x2(rs, rsd);
}

static void AddSatI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addSatInt8x16(rs, rsd);
}

static void AddSatUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAddSatInt8x16(rs, rsd);
}

static void AddSatI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addSatInt16x8(rs, rsd);
}

static void AddSatUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAddSatInt16x8(rs, rsd);
}

static void SubI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt8x16(rs, rsd);
}

static void SubI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt16x8(rs, rsd);
}

static void SubI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt32x4(rs, rsd);
}

static void SubF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subFloat32x4(rs, rsd);
}

static void SubI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt64x2(rs, rsd);
}

static void SubF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subFloat64x2(rs, rsd);
}

static void SubSatI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subSatInt8x16(rs, rsd);
}

static void SubSatUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedSubSatInt8x16(rs, rsd);
}

static void SubSatI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subSatInt16x8(rs, rsd);
}

static void SubSatUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedSubSatInt16x8(rs, rsd);
}

static void MulI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulInt16x8(rs, rsd);
}

static void MulI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulInt32x4(rs, rsd);
}

static void MulF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulFloat32x4(rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void MulI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp) {
  masm.mulInt64x2(rsd, rs, rsd, temp);
}
#  elif defined(JS_CODEGEN_ARM64)
static void MulI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.mulInt64x2(rsd, rs, rsd, temp1, temp2);
}
#  endif

static void MulF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulFloat64x2(rs, rsd);
}

static void DivF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.divFloat32x4(rs, rsd);
}

static void DivF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.divFloat64x2(rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void MinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.minFloat32x4(rs, rsd, temp1, temp2);
}

static void MinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.minFloat64x2(rs, rsd, temp1, temp2);
}

static void MaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.maxFloat32x4(rs, rsd, temp1, temp2);
}

static void MaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.maxFloat64x2(rs, rsd, temp1, temp2);
}

static void PMinF32x4(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMinFloat32x4(rsd, rs);
}

static void PMinF64x2(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMinFloat64x2(rsd, rs);
}

static void PMaxF32x4(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMaxFloat32x4(rsd, rs);
}

static void PMaxF64x2(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMaxFloat64x2(rsd, rs);
}
#  elif defined(JS_CODEGEN_ARM64)
static void MinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat32x4(rs, rsd);
}

static void MinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat64x2(rs, rsd);
}

static void MaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat32x4(rs, rsd);
}

static void MaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat64x2(rs, rsd);
}

static void PMinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMinFloat32x4(rs, rsd);
}

static void PMinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMinFloat64x2(rs, rsd);
}

static void PMaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMaxFloat32x4(rs, rsd);
}

static void PMaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMaxFloat64x2(rs, rsd);
}
#  endif

static void DotI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.widenDotInt16x8(rs, rsd);
}

static void ExtMulLowI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt8x16(rs, rsd);
}

static void ExtMulHighI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt8x16(rs, rsd);
}

static void ExtMulLowUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt8x16(rs, rsd);
}

static void ExtMulHighUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt8x16(rs, rsd);
}

static void ExtMulLowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt16x8(rs, rsd);
}

static void ExtMulHighI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt16x8(rs, rsd);
}

static void ExtMulLowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt16x8(rs, rsd);
}

static void ExtMulHighUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt16x8(rs, rsd);
}

static void ExtMulLowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt32x4(rs, rsd);
}

static void ExtMulHighI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt32x4(rs, rsd);
}

static void ExtMulLowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt32x4(rs, rsd);
}

static void ExtMulHighUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt32x4(rs, rsd);
}

static void Q15MulrSatS(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.q15MulrSatInt16x8(rs, rsd);
}

static void CmpI8x16(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt8x16(cond, rs, rsd);
}

static void CmpI16x8(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt16x8(cond, rs, rsd);
}

static void CmpI32x4(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt32x4(cond, rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void CmpI64x2ForEquality(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareForEqualityInt64x2(cond, rs, rsd);
}

static void CmpI64x2ForOrdering(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd, RegV128 temp1,
                                RegV128 temp2) {
  masm.compareForOrderingInt64x2(cond, rs, rsd, temp1, temp2);
}
#  else
static void CmpI64x2ForEquality(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareInt64x2(cond, rs, rsd);
}

static void CmpI64x2ForOrdering(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareInt64x2(cond, rs, rsd);
}
#  endif  // JS_CODEGEN_X86 || JS_CODEGEN_X64

static void CmpUI8x16(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt8x16(cond, rs, rsd);
}

static void CmpUI16x8(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt16x8(cond, rs, rsd);
}

static void CmpUI32x4(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt32x4(cond, rs, rsd);
}

static void CmpF32x4(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareFloat32x4(cond, rs, rsd);
}

static void CmpF64x2(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareFloat64x2(cond, rs, rsd);
}

static void NegI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt8x16(rs, rd);
}

static void NegI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt16x8(rs, rd);
}

static void NegI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt32x4(rs, rd);
}

static void NegI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt64x2(rs, rd);
}

static void NegF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negFloat32x4(rs, rd);
}

static void NegF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negFloat64x2(rs, rd);
}

static void AbsF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absFloat32x4(rs, rd);
}

static void AbsF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absFloat64x2(rs, rd);
}

static void SqrtF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.sqrtFloat32x4(rs, rd);
}

static void SqrtF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.sqrtFloat64x2(rs, rd);
}

static void CeilF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.ceilFloat32x4(rs, rd);
}

static void FloorF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.floorFloat32x4(rs, rd);
}

static void TruncF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncFloat32x4(rs, rd);
}

static void NearestF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.nearestFloat32x4(rs, rd);
}

static void CeilF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.ceilFloat64x2(rs, rd);
}

static void FloorF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.floorFloat64x2(rs, rd);
}

static void TruncF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncFloat64x2(rs, rd);
}

static void NearestF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.nearestFloat64x2(rs, rd);
}

static void NotV128(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.bitwiseNotSimd128(rs, rd);
}

static void ExtAddPairwiseI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extAddPairwiseInt8x16(rs, rsd);
}

static void ExtAddPairwiseUI8x16(MacroAssembler& masm, RegV128 rs,
                                 RegV128 rsd) {
  masm.unsignedExtAddPairwiseInt8x16(rs, rsd);
}

static void ExtAddPairwiseI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extAddPairwiseInt16x8(rs, rsd);
}

static void ExtAddPairwiseUI16x8(MacroAssembler& masm, RegV128 rs,
                                 RegV128 rsd) {
  masm.unsignedExtAddPairwiseInt16x8(rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void ShiftLeftI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp1, RegV128 temp2) {
  masm.leftShiftInt8x16(rs, rsd, temp1, temp2);
}

static void ShiftLeftI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  masm.leftShiftInt16x8(rs, rsd, temp);
}

static void ShiftLeftI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  masm.leftShiftInt32x4(rs, rsd, temp);
}

static void ShiftLeftI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  masm.leftShiftInt64x2(rs, rsd, temp);
}

static void ShiftRightI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp1, RegV128 temp2) {
  masm.rightShiftInt8x16(rs, rsd, temp1, temp2);
}

static void ShiftRightUI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp1, RegV128 temp2) {
  masm.unsignedRightShiftInt8x16(rs, rsd, temp1, temp2);
}

static void ShiftRightI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  masm.rightShiftInt16x8(rs, rsd, temp);
}

static void ShiftRightUI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  masm.unsignedRightShiftInt16x8(rs, rsd, temp);
}

static void ShiftRightI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  masm.rightShiftInt32x4(rs, rsd, temp);
}

static void ShiftRightUI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  masm.unsignedRightShiftInt32x4(rs, rsd, temp);
}

static void ShiftRightUI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  masm.unsignedRightShiftInt64x2(rs, rsd, temp);
}
#  elif defined(JS_CODEGEN_ARM64)
static void ShiftLeftI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.leftShiftInt8x16(rsd, rs, rsd);
}

static void ShiftLeftI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.leftShiftInt16x8(rsd, rs, rsd);
}

static void ShiftLeftI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.leftShiftInt32x4(rsd, rs, rsd);
}

static void ShiftLeftI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.leftShiftInt64x2(rsd, rs, rsd);
}

static void ShiftRightI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.rightShiftInt8x16(rsd, rs, rsd);
}

static void ShiftRightUI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.unsignedRightShiftInt8x16(rsd, rs, rsd);
}

static void ShiftRightI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.rightShiftInt16x8(rsd, rs, rsd);
}

static void ShiftRightUI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.unsignedRightShiftInt16x8(rsd, rs, rsd);
}

static void ShiftRightI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.rightShiftInt32x4(rsd, rs, rsd);
}

static void ShiftRightUI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.unsignedRightShiftInt32x4(rsd, rs, rsd);
}

static void ShiftRightI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.rightShiftInt64x2(rsd, rs, rsd);
}

static void ShiftRightUI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd) {
  masm.unsignedRightShiftInt64x2(rsd, rs, rsd);
}
#  endif

static void AverageUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAverageInt8x16(rs, rsd);
}

static void AverageUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAverageInt16x8(rs, rsd);
}

static void MinI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt8x16(rs, rsd);
}

static void MinUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt8x16(rs, rsd);
}

static void MaxI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt8x16(rs, rsd);
}

static void MaxUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt8x16(rs, rsd);
}

static void MinI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt16x8(rs, rsd);
}

static void MinUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt16x8(rs, rsd);
}

static void MaxI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt16x8(rs, rsd);
}

static void MaxUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt16x8(rs, rsd);
}

static void MinI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt32x4(rs, rsd);
}

static void MinUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt32x4(rs, rsd);
}

static void MaxI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt32x4(rs, rsd);
}

static void MaxUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt32x4(rs, rsd);
}

static void NarrowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.narrowInt16x8(rs, rsd);
}

static void NarrowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedNarrowInt16x8(rs, rsd);
}

static void NarrowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.narrowInt32x4(rs, rsd);
}

static void NarrowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedNarrowInt32x4(rs, rsd);
}

static void WidenLowI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt8x16(rs, rd);
}

static void WidenHighI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt8x16(rs, rd);
}

static void WidenLowUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt8x16(rs, rd);
}

static void WidenHighUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt8x16(rs, rd);
}

static void WidenLowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt16x8(rs, rd);
}

static void WidenHighI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt16x8(rs, rd);
}

static void WidenLowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt16x8(rs, rd);
}

static void WidenHighUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt16x8(rs, rd);
}

static void WidenLowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt32x4(rs, rd);
}

static void WidenHighI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt32x4(rs, rd);
}

static void WidenLowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt32x4(rs, rd);
}

static void WidenHighUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt32x4(rs, rd);
}

#  if defined(JS_CODEGEN_ARM64)
static void PopcntI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.popcntInt8x16(rs, rd);
}
#  else
static void PopcntI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                        RegV128 temp) {
  masm.popcntInt8x16(rs, rd, temp);
}
#  endif  // JS_CODEGEN_ARM64

static void AbsI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt8x16(rs, rd);
}

static void AbsI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt16x8(rs, rd);
}

static void AbsI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt32x4(rs, rd);
}

static void AbsI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt64x2(rs, rd);
}

static void ExtractLaneI8x16(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt8x16(laneIndex, rs, rd);
}

static void ExtractLaneUI8x16(MacroAssembler& masm, uint32_t laneIndex,
                              RegV128 rs, RegI32 rd) {
  masm.unsignedExtractLaneInt8x16(laneIndex, rs, rd);
}

static void ExtractLaneI16x8(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt16x8(laneIndex, rs, rd);
}

static void ExtractLaneUI16x8(MacroAssembler& masm, uint32_t laneIndex,
                              RegV128 rs, RegI32 rd) {
  masm.unsignedExtractLaneInt16x8(laneIndex, rs, rd);
}

static void ExtractLaneI32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt32x4(laneIndex, rs, rd);
}

static void ExtractLaneI64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI64 rd) {
  masm.extractLaneInt64x2(laneIndex, rs, rd);
}

static void ExtractLaneF32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegF32 rd) {
  masm.extractLaneFloat32x4(laneIndex, rs, rd);
}

static void ExtractLaneF64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegF64 rd) {
  masm.extractLaneFloat64x2(laneIndex, rs, rd);
}

static void ReplaceLaneI8x16(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt8x16(laneIndex, rs, rsd);
}

static void ReplaceLaneI16x8(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt16x8(laneIndex, rs, rsd);
}

static void ReplaceLaneI32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt32x4(laneIndex, rs, rsd);
}

static void ReplaceLaneI64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegI64 rs, RegV128 rsd) {
  masm.replaceLaneInt64x2(laneIndex, rs, rsd);
}

static void ReplaceLaneF32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegF32 rs, RegV128 rsd) {
  masm.replaceLaneFloat32x4(laneIndex, rs, rsd);
}

static void ReplaceLaneF64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegF64 rs, RegV128 rsd) {
  masm.replaceLaneFloat64x2(laneIndex, rs, rsd);
}

static void SplatI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX16(rs, rd);
}

static void SplatI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX8(rs, rd);
}

static void SplatI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX4(rs, rd);
}

static void SplatI64x2(MacroAssembler& masm, RegI64 rs, RegV128 rd) {
  masm.splatX2(rs, rd);
}

static void SplatF32x4(MacroAssembler& masm, RegF32 rs, RegV128 rd) {
  masm.splatX4(rs, rd);
}

static void SplatF64x2(MacroAssembler& masm, RegF64 rs, RegV128 rd) {
  masm.splatX2(rs, rd);
}

// This is the same op independent of lanes: it tests for any nonzero bit.
static void AnyTrue(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.anyTrueSimd128(rs, rd);
}

static void AllTrueI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt8x16(rs, rd);
}

static void AllTrueI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt16x8(rs, rd);
}

static void AllTrueI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt32x4(rs, rd);
}

static void AllTrueI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt64x2(rs, rd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void BitmaskI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt8x16(rs, rd);
}

static void BitmaskI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt16x8(rs, rd);
}

static void BitmaskI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt32x4(rs, rd);
}

static void BitmaskI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt64x2(rs, rd);
}
#  elif defined(JS_CODEGEN_ARM64)
static void BitmaskI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt8x16(rs, rd, temp);
}

static void BitmaskI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt16x8(rs, rd, temp);
}

static void BitmaskI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt32x4(rs, rd, temp);
}

static void BitmaskI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt64x2(rs, rd, temp);
}
#  endif

static void Swizzle(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.swizzleInt8x16(rs, rsd);
}

static void ConvertI32x4ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertInt32x4ToFloat32x4(rs, rd);
}

static void ConvertUI32x4ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedConvertInt32x4ToFloat32x4(rs, rd);
}

static void ConvertF32x4ToI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncSatFloat32x4ToInt32x4(rs, rd);
}

#  if defined(JS_CODEGEN_ARM64)
static void ConvertF32x4ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedTruncSatFloat32x4ToInt32x4(rs, rd);
}
#  else
static void ConvertF32x4ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                 RegV128 temp) {
  masm.unsignedTruncSatFloat32x4ToInt32x4(rs, rd, temp);
}
#  endif  // JS_CODEGEN_ARM64

static void ConvertI32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertInt32x4ToFloat64x2(rs, rd);
}

static void ConvertUI32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedConvertInt32x4ToFloat64x2(rs, rd);
}

static void ConvertF64x2ToI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                RegV128 temp) {
  masm.truncSatFloat64x2ToInt32x4(rs, rd, temp);
}

static void ConvertF64x2ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                 RegV128 temp) {
  masm.unsignedTruncSatFloat64x2ToInt32x4(rs, rd, temp);
}

static void DemoteF64x2ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertFloat64x2ToFloat32x4(rs, rd);
}

static void PromoteF32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertFloat32x4ToFloat64x2(rs, rd);
}

// Bitselect: rs1: ifTrue, rs2: ifFalse, rs3: control
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static RegV128 BitselectV128(BaseCompiler& bc, RegV128 rs1, RegV128 rs2,
                             RegV128 rs3) {
  // On x86, certain register assignments will result in more compact code: we
  // want output=rs1 and tmp=rs3.  Attend to this after we see what other
  // platforms want/need.
  RegV128 tmp = bc.needV128();  // Distinguished tmp, for now
  bc.masm.bitwiseSelectSimd128(rs3, rs1, rs2, rs1, tmp);
  bc.freeV128(tmp);
  return rs1;
}
#  elif defined(JS_CODEGEN_ARM64)
static RegV128 BitselectV128(BaseCompiler& bc, RegV128 rs1, RegV128 rs2,
                             RegV128 rs3) {
  bc.masm.bitwiseSelectSimd128(rs1, rs2, rs3);
  return rs3;
}
#  endif

void BaseCompiler::emitVectorAndNot() {
  // We want x & ~y but the available operation is ~x & y, so reverse the
  // operands.
  RegV128 r, rs;
  pop2xV128(&r, &rs);
  masm.bitwiseNotAndSimd128(r, rs);
  freeV128(r);
  pushV128(rs);
}

bool BaseCompiler::emitLoadSplat(Scalar::Type viewType) {
  // We can implement loadSplat mostly as load + splat because the push of the
  // result onto the value stack in loadCommon normally will not generate any
  // code, it will leave the value in a register which we will consume.

  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  // We use uint types when we can on the general assumption that unsigned loads
  // might be smaller/faster on some platforms, because no sign extension needs
  // to be done after the sub-register load.

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset());
  switch (viewType) {
    case Scalar::Uint8:
      if (!loadCommon(&access, AccessCheck(), ValType::I32)) {
        return false;
      }
      emitUnop(SplatI8x16);
      break;
    case Scalar::Uint16:
      if (!loadCommon(&access, AccessCheck(), ValType::I32)) {
        return false;
      }
      emitUnop(SplatI16x8);
      break;
    case Scalar::Uint32:
      if (!loadCommon(&access, AccessCheck(), ValType::I32)) {
        return false;
      }
      emitUnop(SplatI32x4);
      break;
    case Scalar::Int64:
      if (!loadCommon(&access, AccessCheck(), ValType::I64)) {
        return false;
      }
      emitUnop(SplatI64x2);
      break;
    default:
      MOZ_CRASH();
  }
  return true;
}

bool BaseCompiler::emitLoadZero(Scalar::Type viewType) {
  // LoadZero has the structure of LoadSplat
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset());
  access.setZeroExtendSimd128Load();
  return loadCommon(&access, AccessCheck(), ValType::V128);
}

bool BaseCompiler::emitLoadExtend(Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadExtend(&addr)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  MemoryAccessDesc access(Scalar::Int64, addr.align, addr.offset,
                          bytecodeOffset());
  if (!loadCommon(&access, AccessCheck(), ValType::I64)) {
    return false;
  }

  RegI64 rs = popI64();
  RegV128 rd = needV128();
  masm.moveGPR64ToDouble(rs, rd);
  switch (viewType) {
    case Scalar::Int8:
      masm.widenLowInt8x16(rd, rd);
      break;
    case Scalar::Uint8:
      masm.unsignedWidenLowInt8x16(rd, rd);
      break;
    case Scalar::Int16:
      masm.widenLowInt16x8(rd, rd);
      break;
    case Scalar::Uint16:
      masm.unsignedWidenLowInt16x8(rd, rd);
      break;
    case Scalar::Int32:
      masm.widenLowInt32x4(rd, rd);
      break;
    case Scalar::Uint32:
      masm.unsignedWidenLowInt32x4(rd, rd);
      break;
    default:
      MOZ_CRASH();
  }
  freeI64(rs);
  pushV128(rd);

  return true;
}

bool BaseCompiler::emitLoadLane(uint32_t laneSize) {
  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  uint32_t laneIndex;
  if (!iter_.readLoadLane(laneSize, &addr, &laneIndex, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegV128 rsd = popV128();

  Scalar::Type viewType;
  ValType type;
  switch (laneSize) {
    case 1:
      viewType = Scalar::Uint8;
      type = ValType::I32;
      break;
    case 2:
      viewType = Scalar::Uint16;
      type = ValType::I32;
      break;
    case 4:
      viewType = Scalar::Int32;
      type = ValType::I32;
      break;
    case 8:
      viewType = Scalar::Int64;
      type = ValType::I64;
      break;
    default:
      MOZ_CRASH("unsupported laneSize");
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset());
  if (!loadCommon(&access, AccessCheck(), type)) {
    return false;
  }

  if (type == ValType::I32) {
    RegI32 rs = popI32();
    switch (laneSize) {
      case 1:
        masm.replaceLaneInt8x16(laneIndex, rs, rsd);
        break;
      case 2:
        masm.replaceLaneInt16x8(laneIndex, rs, rsd);
        break;
      case 4:
        masm.replaceLaneInt32x4(laneIndex, rs, rsd);
        break;
    }
    freeI32(rs);
  } else {
    MOZ_ASSERT(type == ValType::I64);
    RegI64 rs = popI64();
    masm.replaceLaneInt64x2(laneIndex, rs, rsd);
    freeI64(rs);
  }

  pushV128(rsd);

  return true;
}

bool BaseCompiler::emitStoreLane(uint32_t laneSize) {
  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  uint32_t laneIndex;
  if (!iter_.readStoreLane(laneSize, &addr, &laneIndex, &nothing)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegV128 rs = popV128();
  Scalar::Type viewType;
  ValType type;
  if (laneSize <= 4) {
    RegI32 tmp = needI32();
    switch (laneSize) {
      case 1:
        viewType = Scalar::Uint8;
        masm.extractLaneInt8x16(laneIndex, rs, tmp);
        break;
      case 2:
        viewType = Scalar::Uint16;
        masm.extractLaneInt16x8(laneIndex, rs, tmp);
        break;
      case 4:
        viewType = Scalar::Int32;
        masm.extractLaneInt32x4(laneIndex, rs, tmp);
        break;
      default:
        MOZ_CRASH("unsupported laneSize");
    }
    pushI32(tmp);
    type = ValType::I32;
  } else {
    MOZ_ASSERT(laneSize == 8);
    RegI64 tmp = needI64();
    masm.extractLaneInt64x2(laneIndex, rs, tmp);
    pushI64(tmp);
    type = ValType::I64;
    viewType = Scalar::Int64;
  }
  freeV128(rs);

  MemoryAccessDesc access(viewType, addr.align, addr.offset, bytecodeOffset());
  return storeCommon(&access, AccessCheck(), type);
}

bool BaseCompiler::emitBitselect() {
  Nothing unused_a, unused_b, unused_c;

  if (!iter_.readVectorSelect(&unused_a, &unused_b, &unused_c)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegV128 rs3 = popV128();  // Control
  RegV128 rs2 = popV128();  // 'false' vector
  RegV128 rs1 = popV128();  // 'true' vector

  RegV128 result = BitselectV128(*this, rs1, rs2, rs3);

  if (rs1 != result) {
    freeV128(rs1);
  }
  if (rs2 != result) {
    freeV128(rs2);
  }
  if (rs3 != result) {
    freeV128(rs3);
  }
  pushV128(result);
  return true;
}

bool BaseCompiler::emitVectorShuffle() {
  Nothing unused_a, unused_b;
  V128 shuffleMask;

  if (!iter_.readVectorShuffle(&unused_a, &unused_b, &shuffleMask)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegV128 rd, rs;
  pop2xV128(&rd, &rs);

  bool emitShuffle = true;

#  ifdef ENABLE_WASM_SIMD_WORMHOLE
  if (moduleEnv_.simdWormholeEnabled() && IsWormholeTrigger(shuffleMask)) {
    emitShuffle = false;
    switch (shuffleMask.bytes[15]) {
      case 0:
        masm.loadConstantSimd128(WormholeSignature(), rd);
        break;
#    if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
      case 1:
        masm.vpmaddubsw(rs, rd, rd);
        break;
      case 2:
        masm.vpmaddwd(Operand(rs), rd, rd);
        break;
#    endif
      default:
        return iter_.fail("Unrecognized wormhole opcode");
    }
  }
#  endif

  if (emitShuffle) {
    masm.shuffleInt8x16(shuffleMask.bytes, rs, rd);
  }

  freeV128(rs);
  pushV128(rd);

  return true;
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
bool BaseCompiler::emitVectorShiftRightI64x2() {
  Nothing unused_a, unused_b;

  if (!iter_.readVectorShift(&unused_a, &unused_b)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegI32 count = popI32RhsForShiftI64();
  RegV128 lhsDest = popV128();
  RegI64 tmp = needI64();
  masm.and32(Imm32(63), count);
  masm.extractLaneInt64x2(0, lhsDest, tmp);
  masm.rshift64Arithmetic(count, tmp);
  masm.replaceLaneInt64x2(0, tmp, lhsDest);
  masm.extractLaneInt64x2(1, lhsDest, tmp);
  masm.rshift64Arithmetic(count, tmp);
  masm.replaceLaneInt64x2(1, tmp, lhsDest);
  freeI64(tmp);
  freeI32(count);
  pushV128(lhsDest);

  return true;
}
#  endif
#endif  // ENABLE_WASM_SIMD

bool BaseCompiler::emitBody() {
  MOZ_ASSERT(stackMapGenerator_.framePushedAtEntryToBody.isSome());

  if (!iter_.startFunction(func_.index)) {
    return false;
  }

  initControl(controlItem(), ResultType::Empty());

  for (;;) {
    Nothing unused_a, unused_b;

#ifdef DEBUG
    performRegisterLeakCheck();
    assertStackInvariants();
#endif

#define dispatchBinary0(doEmit, type)             \
  iter_.readBinary(type, &unused_a, &unused_b) && \
      (deadCode_ || (doEmit(), true))

#define dispatchBinary1(arg1, type)               \
  iter_.readBinary(type, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(arg1), true))

#define dispatchBinary2(arg1, arg2, type)         \
  iter_.readBinary(type, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(arg1, arg2), true))

#define dispatchBinary3(arg1, arg2, arg3, type)   \
  iter_.readBinary(type, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(arg1, arg2, arg3), true))

#define dispatchUnary0(doEmit, type) \
  iter_.readUnary(type, &unused_a) && (deadCode_ || (doEmit(), true))

#define dispatchUnary1(arg1, type) \
  iter_.readUnary(type, &unused_a) && (deadCode_ || (emitUnop(arg1), true))

#define dispatchUnary2(arg1, arg2, type) \
  iter_.readUnary(type, &unused_a) &&    \
      (deadCode_ || (emitUnop(arg1, arg2), true))

#define dispatchComparison0(doEmit, operandType, compareOp)  \
  iter_.readComparison(operandType, &unused_a, &unused_b) && \
      (deadCode_ || (doEmit(compareOp, operandType), true))

#define dispatchConversion0(doEmit, inType, outType)  \
  iter_.readConversion(inType, outType, &unused_a) && \
      (deadCode_ || (doEmit(), true))

#define dispatchConversion1(arg1, inType, outType)    \
  iter_.readConversion(inType, outType, &unused_a) && \
      (deadCode_ || (emitUnop(arg1), true))

#define dispatchConversionOOM(doEmit, inType, outType) \
  iter_.readConversion(inType, outType, &unused_a) && (deadCode_ || doEmit())

#define dispatchCalloutConversionOOM(doEmit, symbol, inType, outType) \
  iter_.readConversion(inType, outType, &unused_a) &&                 \
      (deadCode_ || doEmit(symbol, inType, outType))

#define dispatchIntDivCallout(doEmit, symbol, type) \
  iter_.readBinary(type, &unused_a, &unused_b) &&   \
      (deadCode_ || doEmit(symbol, type))

#define dispatchVectorBinary(op)                           \
  iter_.readBinary(ValType::V128, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(op), true))

#define dispatchVectorUnary(op)                \
  iter_.readUnary(ValType::V128, &unused_a) && \
      (deadCode_ || (emitUnop(op), true))

#define dispatchVectorComparison(op, compareOp)            \
  iter_.readBinary(ValType::V128, &unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(compareOp, op), true))

#define dispatchVectorVariableShift(op)          \
  iter_.readVectorShift(&unused_a, &unused_b) && \
      (deadCode_ || (emitBinop(op), true))

#define dispatchExtractLane(op, outType, laneLimit)                   \
  iter_.readExtractLane(outType, laneLimit, &laneIndex, &unused_a) && \
      (deadCode_ || (emitUnop(laneIndex, op), true))

#define dispatchReplaceLane(op, inType, laneLimit)                \
  iter_.readReplaceLane(inType, laneLimit, &laneIndex, &unused_a, \
                        &unused_b) &&                             \
      (deadCode_ || (emitBinop(laneIndex, op), true))

#define dispatchSplat(op, inType)                           \
  iter_.readConversion(inType, ValType::V128, &unused_a) && \
      (deadCode_ || (emitUnop(op), true))

#define dispatchVectorReduction(op)                               \
  iter_.readConversion(ValType::V128, ValType::I32, &unused_a) && \
      (deadCode_ || (emitUnop(op), true))

#ifdef DEBUG
    // Check that the number of ref-typed entries in the operand stack matches
    // reality.
#  define CHECK_POINTER_COUNT                                             \
    do {                                                                  \
      MOZ_ASSERT(countMemRefsOnStk() == stackMapGenerator_.memRefsOnStk); \
    } while (0)
#else
#  define CHECK_POINTER_COUNT \
    do {                      \
    } while (0)
#endif

#define CHECK(E) \
  if (!(E)) return false
#define NEXT()           \
  {                      \
    CHECK_POINTER_COUNT; \
    continue;            \
  }
#define CHECK_NEXT(E)     \
  if (!(E)) return false; \
  {                       \
    CHECK_POINTER_COUNT;  \
    continue;             \
  }

    // Opcodes that push more than MaxPushesPerOpcode (anything with multiple
    // results) will perform additional reservation.
    CHECK(stk_.reserve(stk_.length() + MaxPushesPerOpcode));

    OpBytes op{};
    CHECK(iter_.readOp(&op));

    // When compilerEnv_.debugEnabled(), every operator has a breakpoint site
    // except Op::End.
    if (compilerEnv_.debugEnabled() && op.b0 != (uint16_t)Op::End) {
      // TODO sync only registers that can be clobbered by the exit
      // prologue/epilogue or disable these registers for use in
      // baseline compiler when compilerEnv_.debugEnabled() is set.
      sync();

      insertBreakablePoint(CallSiteDesc::Breakpoint);
      if (!createStackMap("debug: per-insn breakpoint")) {
        return false;
      }
    }

    // Going below framePushedAtEntryToBody would imply that we've
    // popped off the machine stack, part of the frame created by
    // beginFunction().
    MOZ_ASSERT(masm.framePushed() >=
               stackMapGenerator_.framePushedAtEntryToBody.value());

    // At this point we're definitely not generating code for a function call.
    MOZ_ASSERT(
        stackMapGenerator_.framePushedExcludingOutboundCallArgs.isNothing());

    switch (op.b0) {
      case uint16_t(Op::End):
        if (!emitEnd()) {
          return false;
        }
        if (iter_.controlStackEmpty()) {
          return true;
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
#ifdef ENABLE_WASM_EXCEPTIONS
      case uint16_t(Op::Try):
        if (!moduleEnv_.exceptionsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitTry());
      case uint16_t(Op::Catch):
        if (!moduleEnv_.exceptionsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitCatch());
      case uint16_t(Op::CatchAll):
        if (!moduleEnv_.exceptionsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitCatchAll());
      case uint16_t(Op::Delegate):
        if (!moduleEnv_.exceptionsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK(emitDelegate());
        iter_.popDelegate();
        NEXT();
      case uint16_t(Op::Throw):
        if (!moduleEnv_.exceptionsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitThrow());
      case uint16_t(Op::Rethrow):
        if (!moduleEnv_.exceptionsEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitRethrow());
#endif
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
      case uint16_t(Op::TableGet):
        CHECK_NEXT(emitTableGet());
      case uint16_t(Op::TableSet):
        CHECK_NEXT(emitTableSet());

      // Select
      case uint16_t(Op::SelectNumeric):
        CHECK_NEXT(emitSelect(/*typed*/ false));
      case uint16_t(Op::SelectTyped):
        CHECK_NEXT(emitSelect(/*typed*/ true));

      // I32
      case uint16_t(Op::I32Const): {
        int32_t i32;
        CHECK(iter_.readI32Const(&i32));
        if (!deadCode_) {
          pushI32(i32);
        }
        NEXT();
      }
      case uint16_t(Op::I32Add):
        CHECK_NEXT(dispatchBinary2(AddI32, AddImmI32, ValType::I32));
      case uint16_t(Op::I32Sub):
        CHECK_NEXT(dispatchBinary2(SubI32, SubImmI32, ValType::I32));
      case uint16_t(Op::I32Mul):
        CHECK_NEXT(dispatchBinary0(emitMultiplyI32, ValType::I32));
      case uint16_t(Op::I32DivS):
        CHECK_NEXT(dispatchBinary0(emitQuotientI32, ValType::I32));
      case uint16_t(Op::I32DivU):
        CHECK_NEXT(dispatchBinary0(emitQuotientU32, ValType::I32));
      case uint16_t(Op::I32RemS):
        CHECK_NEXT(dispatchBinary0(emitRemainderI32, ValType::I32));
      case uint16_t(Op::I32RemU):
        CHECK_NEXT(dispatchBinary0(emitRemainderU32, ValType::I32));
      case uint16_t(Op::I32Eqz):
        CHECK_NEXT(dispatchConversion0(emitEqzI32, ValType::I32, ValType::I32));
      case uint16_t(Op::I32TruncSF32):
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF32ToI32<0>, ValType::F32,
                                         ValType::I32));
      case uint16_t(Op::I32TruncUF32):
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF32ToI32<TRUNC_UNSIGNED>,
                                         ValType::F32, ValType::I32));
      case uint16_t(Op::I32TruncSF64):
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF64ToI32<0>, ValType::F64,
                                         ValType::I32));
      case uint16_t(Op::I32TruncUF64):
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF64ToI32<TRUNC_UNSIGNED>,
                                         ValType::F64, ValType::I32));
      case uint16_t(Op::I32WrapI64):
        CHECK_NEXT(
            dispatchConversion1(WrapI64ToI32, ValType::I64, ValType::I32));
      case uint16_t(Op::I32ReinterpretF32):
        CHECK_NEXT(dispatchConversion1(ReinterpretF32AsI32, ValType::F32,
                                       ValType::I32));
      case uint16_t(Op::I32Clz):
        CHECK_NEXT(dispatchUnary1(ClzI32, ValType::I32));
      case uint16_t(Op::I32Ctz):
        CHECK_NEXT(dispatchUnary1(CtzI32, ValType::I32));
      case uint16_t(Op::I32Popcnt):
        CHECK_NEXT(dispatchUnary2(PopcntI32, PopcntTemp, ValType::I32));
      case uint16_t(Op::I32Or):
        CHECK_NEXT(dispatchBinary2(OrI32, OrImmI32, ValType::I32));
      case uint16_t(Op::I32And):
        CHECK_NEXT(dispatchBinary2(AndI32, AndImmI32, ValType::I32));
      case uint16_t(Op::I32Xor):
        CHECK_NEXT(dispatchBinary2(XorI32, XorImmI32, ValType::I32));
      case uint16_t(Op::I32Shl):
        CHECK_NEXT(dispatchBinary3(
            ShlI32, ShlImmI32, &BaseCompiler::popI32RhsForShift, ValType::I32));
      case uint16_t(Op::I32ShrS):
        CHECK_NEXT(dispatchBinary3(
            ShrI32, ShrImmI32, &BaseCompiler::popI32RhsForShift, ValType::I32));
      case uint16_t(Op::I32ShrU):
        CHECK_NEXT(dispatchBinary3(ShrUI32, ShrUImmI32,
                                   &BaseCompiler::popI32RhsForShift,
                                   ValType::I32));
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
        CHECK_NEXT(dispatchBinary3(RotrI32, RotrImmI32,
                                   &BaseCompiler::popI32RhsForRotate,
                                   ValType::I32));
      case uint16_t(Op::I32Rotl):
        CHECK_NEXT(dispatchBinary3(RotlI32, RotlImmI32,
                                   &BaseCompiler::popI32RhsForRotate,
                                   ValType::I32));

      // I64
      case uint16_t(Op::I64Const): {
        int64_t i64;
        CHECK(iter_.readI64Const(&i64));
        if (!deadCode_) {
          pushI64(i64);
        }
        NEXT();
      }
      case uint16_t(Op::I64Add):
        CHECK_NEXT(dispatchBinary2(AddI64, AddImmI64, ValType::I64));
      case uint16_t(Op::I64Sub):
        CHECK_NEXT(dispatchBinary2(SubI64, SubImmI64, ValType::I64));
      case uint16_t(Op::I64Mul):
        CHECK_NEXT(dispatchBinary0(emitMultiplyI64, ValType::I64));
      case uint16_t(Op::I64DivS):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
        CHECK_NEXT(dispatchIntDivCallout(
            emitDivOrModI64BuiltinCall, SymbolicAddress::DivI64, ValType::I64));
#else
        CHECK_NEXT(dispatchBinary0(emitQuotientI64, ValType::I64));
#endif
      case uint16_t(Op::I64DivU):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
        CHECK_NEXT(dispatchIntDivCallout(emitDivOrModI64BuiltinCall,
                                         SymbolicAddress::UDivI64,
                                         ValType::I64));
#else
        CHECK_NEXT(dispatchBinary0(emitQuotientU64, ValType::I64));
#endif
      case uint16_t(Op::I64RemS):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
        CHECK_NEXT(dispatchIntDivCallout(
            emitDivOrModI64BuiltinCall, SymbolicAddress::ModI64, ValType::I64));
#else
        CHECK_NEXT(dispatchBinary0(emitRemainderI64, ValType::I64));
#endif
      case uint16_t(Op::I64RemU):
#ifdef RABALDR_INT_DIV_I64_CALLOUT
        CHECK_NEXT(dispatchIntDivCallout(emitDivOrModI64BuiltinCall,
                                         SymbolicAddress::UModI64,
                                         ValType::I64));
#else
        CHECK_NEXT(dispatchBinary0(emitRemainderU64, ValType::I64));
#endif
      case uint16_t(Op::I64TruncSF32):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
        CHECK_NEXT(
            dispatchCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                         SymbolicAddress::TruncateDoubleToInt64,
                                         ValType::F32, ValType::I64));
#else
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF32ToI64<0>, ValType::F32,
                                         ValType::I64));
#endif
      case uint16_t(Op::I64TruncUF32):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertFloatingToInt64Callout,
            SymbolicAddress::TruncateDoubleToUint64, ValType::F32,
            ValType::I64));
#else
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF32ToI64<TRUNC_UNSIGNED>,
                                         ValType::F32, ValType::I64));
#endif
      case uint16_t(Op::I64TruncSF64):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
        CHECK_NEXT(
            dispatchCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                         SymbolicAddress::TruncateDoubleToInt64,
                                         ValType::F64, ValType::I64));
#else
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF64ToI64<0>, ValType::F64,
                                         ValType::I64));
#endif
      case uint16_t(Op::I64TruncUF64):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertFloatingToInt64Callout,
            SymbolicAddress::TruncateDoubleToUint64, ValType::F64,
            ValType::I64));
#else
        CHECK_NEXT(dispatchConversionOOM(emitTruncateF64ToI64<TRUNC_UNSIGNED>,
                                         ValType::F64, ValType::I64));
#endif
      case uint16_t(Op::I64ExtendSI32):
        CHECK_NEXT(dispatchConversion0(emitExtendI32ToI64, ValType::I32,
                                       ValType::I64));
      case uint16_t(Op::I64ExtendUI32):
        CHECK_NEXT(dispatchConversion0(emitExtendU32ToI64, ValType::I32,
                                       ValType::I64));
      case uint16_t(Op::I64ReinterpretF64):
        CHECK_NEXT(dispatchConversion1(ReinterpretF64AsI64, ValType::F64,
                                       ValType::I64));
      case uint16_t(Op::I64Or):
        CHECK_NEXT(dispatchBinary2(OrI64, OrImmI64, ValType::I64));
      case uint16_t(Op::I64And):
        CHECK_NEXT(dispatchBinary2(AndI64, AndImmI64, ValType::I64));
      case uint16_t(Op::I64Xor):
        CHECK_NEXT(dispatchBinary2(XorI64, XorImmI64, ValType::I64));
      case uint16_t(Op::I64Shl):
        CHECK_NEXT(dispatchBinary3(
            ShlI64, ShlImmI64, &BaseCompiler::popI64RhsForShift, ValType::I64));
      case uint16_t(Op::I64ShrS):
        CHECK_NEXT(dispatchBinary3(
            ShrI64, ShrImmI64, &BaseCompiler::popI64RhsForShift, ValType::I64));
      case uint16_t(Op::I64ShrU):
        CHECK_NEXT(dispatchBinary3(ShrUI64, ShrUImmI64,
                                   &BaseCompiler::popI64RhsForShift,
                                   ValType::I64));
      case uint16_t(Op::I64Rotr):
        CHECK_NEXT(dispatchBinary0(emitRotrI64, ValType::I64));
      case uint16_t(Op::I64Rotl):
        CHECK_NEXT(dispatchBinary0(emitRotlI64, ValType::I64));
      case uint16_t(Op::I64Clz):
        CHECK_NEXT(dispatchUnary1(ClzI64, ValType::I64));
      case uint16_t(Op::I64Ctz):
        CHECK_NEXT(dispatchUnary1(CtzI64, ValType::I64));
      case uint16_t(Op::I64Popcnt):
        CHECK_NEXT(dispatchUnary2(PopcntI64, PopcntTemp, ValType::I64));
      case uint16_t(Op::I64Eqz):
        CHECK_NEXT(dispatchConversion0(emitEqzI64, ValType::I64, ValType::I32));
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
        if (!deadCode_) {
          pushF32(f32);
        }
        NEXT();
      }
      case uint16_t(Op::F32Add):
        CHECK_NEXT(dispatchBinary1(AddF32, ValType::F32))
      case uint16_t(Op::F32Sub):
        CHECK_NEXT(dispatchBinary1(SubF32, ValType::F32));
      case uint16_t(Op::F32Mul):
        CHECK_NEXT(dispatchBinary1(MulF32, ValType::F32));
      case uint16_t(Op::F32Div):
        CHECK_NEXT(dispatchBinary1(DivF32, ValType::F32));
      case uint16_t(Op::F32Min):
        CHECK_NEXT(dispatchBinary1(MinF32, ValType::F32));
      case uint16_t(Op::F32Max):
        CHECK_NEXT(dispatchBinary1(MaxF32, ValType::F32));
      case uint16_t(Op::F32Neg):
        CHECK_NEXT(dispatchUnary1(NegateF32, ValType::F32));
      case uint16_t(Op::F32Abs):
        CHECK_NEXT(dispatchUnary1(AbsF32, ValType::F32));
      case uint16_t(Op::F32Sqrt):
        CHECK_NEXT(dispatchUnary1(SqrtF32, ValType::F32));
      case uint16_t(Op::F32Ceil):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::CeilF, ValType::F32));
      case uint16_t(Op::F32Floor):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::FloorF, ValType::F32));
      case uint16_t(Op::F32DemoteF64):
        CHECK_NEXT(
            dispatchConversion1(ConvertF64ToF32, ValType::F64, ValType::F32));
      case uint16_t(Op::F32ConvertSI32):
        CHECK_NEXT(
            dispatchConversion1(ConvertI32ToF32, ValType::I32, ValType::F32));
      case uint16_t(Op::F32ConvertUI32):
        CHECK_NEXT(
            dispatchConversion1(ConvertU32ToF32, ValType::I32, ValType::F32));
      case uint16_t(Op::F32ConvertSI64):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertInt64ToFloatingCallout, SymbolicAddress::Int64ToFloat32,
            ValType::I64, ValType::F32));
#else
        CHECK_NEXT(
            dispatchConversion1(ConvertI64ToF32, ValType::I64, ValType::F32));
#endif
      case uint16_t(Op::F32ConvertUI64):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertInt64ToFloatingCallout, SymbolicAddress::Uint64ToFloat32,
            ValType::I64, ValType::F32));
#else
        CHECK_NEXT(dispatchConversion0(emitConvertU64ToF32, ValType::I64,
                                       ValType::F32));
#endif
      case uint16_t(Op::F32ReinterpretI32):
        CHECK_NEXT(dispatchConversion1(ReinterpretI32AsF32, ValType::I32,
                                       ValType::F32));
      case uint16_t(Op::F32Load):
        CHECK_NEXT(emitLoad(ValType::F32, Scalar::Float32));
      case uint16_t(Op::F32Store):
        CHECK_NEXT(emitStore(ValType::F32, Scalar::Float32));
      case uint16_t(Op::F32CopySign):
        CHECK_NEXT(dispatchBinary1(CopysignF32, ValType::F32));
      case uint16_t(Op::F32Nearest):
        CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::NearbyIntF,
                                            ValType::F32));
      case uint16_t(Op::F32Trunc):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::TruncF, ValType::F32));

      // F64
      case uint16_t(Op::F64Const): {
        double f64;
        CHECK(iter_.readF64Const(&f64));
        if (!deadCode_) {
          pushF64(f64);
        }
        NEXT();
      }
      case uint16_t(Op::F64Add):
        CHECK_NEXT(dispatchBinary1(AddF64, ValType::F64))
      case uint16_t(Op::F64Sub):
        CHECK_NEXT(dispatchBinary1(SubF64, ValType::F64));
      case uint16_t(Op::F64Mul):
        CHECK_NEXT(dispatchBinary1(MulF64, ValType::F64));
      case uint16_t(Op::F64Div):
        CHECK_NEXT(dispatchBinary1(DivF64, ValType::F64));
      case uint16_t(Op::F64Min):
        CHECK_NEXT(dispatchBinary1(MinF64, ValType::F64));
      case uint16_t(Op::F64Max):
        CHECK_NEXT(dispatchBinary1(MaxF64, ValType::F64));
      case uint16_t(Op::F64Neg):
        CHECK_NEXT(dispatchUnary1(NegateF64, ValType::F64));
      case uint16_t(Op::F64Abs):
        CHECK_NEXT(dispatchUnary1(AbsF64, ValType::F64));
      case uint16_t(Op::F64Sqrt):
        CHECK_NEXT(dispatchUnary1(SqrtF64, ValType::F64));
      case uint16_t(Op::F64Ceil):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::CeilD, ValType::F64));
      case uint16_t(Op::F64Floor):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::FloorD, ValType::F64));
      case uint16_t(Op::F64PromoteF32):
        CHECK_NEXT(
            dispatchConversion1(ConvertF32ToF64, ValType::F32, ValType::F64));
      case uint16_t(Op::F64ConvertSI32):
        CHECK_NEXT(
            dispatchConversion1(ConvertI32ToF64, ValType::I32, ValType::F64));
      case uint16_t(Op::F64ConvertUI32):
        CHECK_NEXT(
            dispatchConversion1(ConvertU32ToF64, ValType::I32, ValType::F64));
      case uint16_t(Op::F64ConvertSI64):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertInt64ToFloatingCallout, SymbolicAddress::Int64ToDouble,
            ValType::I64, ValType::F64));
#else
        CHECK_NEXT(
            dispatchConversion1(ConvertI64ToF64, ValType::I64, ValType::F64));
#endif
      case uint16_t(Op::F64ConvertUI64):
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
        CHECK_NEXT(dispatchCalloutConversionOOM(
            emitConvertInt64ToFloatingCallout, SymbolicAddress::Uint64ToDouble,
            ValType::I64, ValType::F64));
#else
        CHECK_NEXT(dispatchConversion0(emitConvertU64ToF64, ValType::I64,
                                       ValType::F64));
#endif
      case uint16_t(Op::F64Load):
        CHECK_NEXT(emitLoad(ValType::F64, Scalar::Float64));
      case uint16_t(Op::F64Store):
        CHECK_NEXT(emitStore(ValType::F64, Scalar::Float64));
      case uint16_t(Op::F64ReinterpretI64):
        CHECK_NEXT(dispatchConversion1(ReinterpretI64AsF64, ValType::I64,
                                       ValType::F64));
      case uint16_t(Op::F64CopySign):
        CHECK_NEXT(dispatchBinary1(CopysignF64, ValType::F64));
      case uint16_t(Op::F64Nearest):
        CHECK_NEXT(emitUnaryMathBuiltinCall(SymbolicAddress::NearbyIntD,
                                            ValType::F64));
      case uint16_t(Op::F64Trunc):
        CHECK_NEXT(
            emitUnaryMathBuiltinCall(SymbolicAddress::TruncD, ValType::F64));

      // Comparisons
      case uint16_t(Op::I32Eq):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::Equal));
      case uint16_t(Op::I32Ne):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::NotEqual));
      case uint16_t(Op::I32LtS):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::LessThan));
      case uint16_t(Op::I32LeS):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::LessThanOrEqual));
      case uint16_t(Op::I32GtS):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::GreaterThan));
      case uint16_t(Op::I32GeS):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::GreaterThanOrEqual));
      case uint16_t(Op::I32LtU):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::Below));
      case uint16_t(Op::I32LeU):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::BelowOrEqual));
      case uint16_t(Op::I32GtU):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::Above));
      case uint16_t(Op::I32GeU):
        CHECK_NEXT(dispatchComparison0(emitCompareI32, ValType::I32,
                                       Assembler::AboveOrEqual));
      case uint16_t(Op::I64Eq):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::Equal));
      case uint16_t(Op::I64Ne):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::NotEqual));
      case uint16_t(Op::I64LtS):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::LessThan));
      case uint16_t(Op::I64LeS):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::LessThanOrEqual));
      case uint16_t(Op::I64GtS):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::GreaterThan));
      case uint16_t(Op::I64GeS):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::GreaterThanOrEqual));
      case uint16_t(Op::I64LtU):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::Below));
      case uint16_t(Op::I64LeU):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::BelowOrEqual));
      case uint16_t(Op::I64GtU):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::Above));
      case uint16_t(Op::I64GeU):
        CHECK_NEXT(dispatchComparison0(emitCompareI64, ValType::I64,
                                       Assembler::AboveOrEqual));
      case uint16_t(Op::F32Eq):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleEqual));
      case uint16_t(Op::F32Ne):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleNotEqualOrUnordered));
      case uint16_t(Op::F32Lt):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleLessThan));
      case uint16_t(Op::F32Le):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleLessThanOrEqual));
      case uint16_t(Op::F32Gt):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleGreaterThan));
      case uint16_t(Op::F32Ge):
        CHECK_NEXT(dispatchComparison0(emitCompareF32, ValType::F32,
                                       Assembler::DoubleGreaterThanOrEqual));
      case uint16_t(Op::F64Eq):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleEqual));
      case uint16_t(Op::F64Ne):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleNotEqualOrUnordered));
      case uint16_t(Op::F64Lt):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleLessThan));
      case uint16_t(Op::F64Le):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleLessThanOrEqual));
      case uint16_t(Op::F64Gt):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleGreaterThan));
      case uint16_t(Op::F64Ge):
        CHECK_NEXT(dispatchComparison0(emitCompareF64, ValType::F64,
                                       Assembler::DoubleGreaterThanOrEqual));

      // Sign extensions
      case uint16_t(Op::I32Extend8S):
        CHECK_NEXT(
            dispatchConversion1(ExtendI32_8, ValType::I32, ValType::I32));
      case uint16_t(Op::I32Extend16S):
        CHECK_NEXT(
            dispatchConversion1(ExtendI32_16, ValType::I32, ValType::I32));
      case uint16_t(Op::I64Extend8S):
        CHECK_NEXT(
            dispatchConversion0(emitExtendI64_8, ValType::I64, ValType::I64));
      case uint16_t(Op::I64Extend16S):
        CHECK_NEXT(
            dispatchConversion0(emitExtendI64_16, ValType::I64, ValType::I64));
      case uint16_t(Op::I64Extend32S):
        CHECK_NEXT(
            dispatchConversion0(emitExtendI64_32, ValType::I64, ValType::I64));

      // Memory Related
      case uint16_t(Op::MemoryGrow):
        CHECK_NEXT(emitMemoryGrow());
      case uint16_t(Op::MemorySize):
        CHECK_NEXT(emitMemorySize());

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
      case uint16_t(Op::RefAsNonNull):
        if (!moduleEnv_.functionReferencesEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitRefAsNonNull());
      case uint16_t(Op::BrOnNull):
        if (!moduleEnv_.functionReferencesEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(emitBrOnNull());
#endif
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::RefEq):
        if (!moduleEnv_.gcEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        CHECK_NEXT(dispatchComparison0(emitCompareRef, RefType::eq(),
                                       Assembler::Equal));
#endif
      case uint16_t(Op::RefFunc):
        CHECK_NEXT(emitRefFunc());
        break;
      case uint16_t(Op::RefNull):
        CHECK_NEXT(emitRefNull());
        break;
      case uint16_t(Op::RefIsNull):
        CHECK_NEXT(emitRefIsNull());
        break;

#ifdef ENABLE_WASM_GC
      // "GC" operations
      case uint16_t(Op::GcPrefix): {
        if (!moduleEnv_.gcEnabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(GcOp::StructNewWithRtt):
            CHECK_NEXT(emitStructNewWithRtt());
          case uint32_t(GcOp::StructNewDefaultWithRtt):
            CHECK_NEXT(emitStructNewDefaultWithRtt());
          case uint32_t(GcOp::StructGet):
            CHECK_NEXT(emitStructGet(FieldExtension::None));
          case uint32_t(GcOp::StructGetS):
            CHECK_NEXT(emitStructGet(FieldExtension::Signed));
          case uint32_t(GcOp::StructGetU):
            CHECK_NEXT(emitStructGet(FieldExtension::Unsigned));
          case uint32_t(GcOp::StructSet):
            CHECK_NEXT(emitStructSet());
          case uint32_t(GcOp::ArrayNewWithRtt):
            CHECK_NEXT(emitArrayNewWithRtt());
          case uint32_t(GcOp::ArrayNewDefaultWithRtt):
            CHECK_NEXT(emitArrayNewDefaultWithRtt());
          case uint32_t(GcOp::ArrayGet):
            CHECK_NEXT(emitArrayGet(FieldExtension::None));
          case uint32_t(GcOp::ArrayGetS):
            CHECK_NEXT(emitArrayGet(FieldExtension::Signed));
          case uint32_t(GcOp::ArrayGetU):
            CHECK_NEXT(emitArrayGet(FieldExtension::Unsigned));
          case uint32_t(GcOp::ArraySet):
            CHECK_NEXT(emitArraySet());
          case uint32_t(GcOp::ArrayLen):
            CHECK_NEXT(emitArrayLen());
          case uint32_t(GcOp::RttCanon):
            CHECK_NEXT(emitRttCanon());
          case uint32_t(GcOp::RttSub):
            CHECK_NEXT(emitRttSub());
          case uint32_t(GcOp::RefTest):
            CHECK_NEXT(emitRefTest());
          case uint32_t(GcOp::RefCast):
            CHECK_NEXT(emitRefCast());
          case uint32_t(GcOp::BrOnCast):
            CHECK_NEXT(emitBrOnCast());
          default:
            break;
        }  // switch (op.b1)
        return iter_.unrecognizedOpcode(&op);
      }
#endif

#ifdef ENABLE_WASM_SIMD
      // SIMD operations
      case uint16_t(Op::SimdPrefix): {
        uint32_t laneIndex;
        if (!moduleEnv_.v128Enabled()) {
          return iter_.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(SimdOp::I8x16ExtractLaneS):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneI8x16, ValType::I32, 16));
          case uint32_t(SimdOp::I8x16ExtractLaneU):
            CHECK_NEXT(
                dispatchExtractLane(ExtractLaneUI8x16, ValType::I32, 16));
          case uint32_t(SimdOp::I16x8ExtractLaneS):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneI16x8, ValType::I32, 8));
          case uint32_t(SimdOp::I16x8ExtractLaneU):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneUI16x8, ValType::I32, 8));
          case uint32_t(SimdOp::I32x4ExtractLane):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneI32x4, ValType::I32, 4));
          case uint32_t(SimdOp::I64x2ExtractLane):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneI64x2, ValType::I64, 2));
          case uint32_t(SimdOp::F32x4ExtractLane):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneF32x4, ValType::F32, 4));
          case uint32_t(SimdOp::F64x2ExtractLane):
            CHECK_NEXT(dispatchExtractLane(ExtractLaneF64x2, ValType::F64, 2));
          case uint32_t(SimdOp::I8x16Splat):
            CHECK_NEXT(dispatchSplat(SplatI8x16, ValType::I32));
          case uint32_t(SimdOp::I16x8Splat):
            CHECK_NEXT(dispatchSplat(SplatI16x8, ValType::I32));
          case uint32_t(SimdOp::I32x4Splat):
            CHECK_NEXT(dispatchSplat(SplatI32x4, ValType::I32));
          case uint32_t(SimdOp::I64x2Splat):
            CHECK_NEXT(dispatchSplat(SplatI64x2, ValType::I64));
          case uint32_t(SimdOp::F32x4Splat):
            CHECK_NEXT(dispatchSplat(SplatF32x4, ValType::F32));
          case uint32_t(SimdOp::F64x2Splat):
            CHECK_NEXT(dispatchSplat(SplatF64x2, ValType::F64));
          case uint32_t(SimdOp::V128AnyTrue):
            CHECK_NEXT(dispatchVectorReduction(AnyTrue));
          case uint32_t(SimdOp::I8x16AllTrue):
            CHECK_NEXT(dispatchVectorReduction(AllTrueI8x16));
          case uint32_t(SimdOp::I16x8AllTrue):
            CHECK_NEXT(dispatchVectorReduction(AllTrueI16x8));
          case uint32_t(SimdOp::I32x4AllTrue):
            CHECK_NEXT(dispatchVectorReduction(AllTrueI32x4));
          case uint32_t(SimdOp::I64x2AllTrue):
            CHECK_NEXT(dispatchVectorReduction(AllTrueI64x2));
          case uint32_t(SimdOp::I8x16Bitmask):
            CHECK_NEXT(dispatchVectorReduction(BitmaskI8x16));
          case uint32_t(SimdOp::I16x8Bitmask):
            CHECK_NEXT(dispatchVectorReduction(BitmaskI16x8));
          case uint32_t(SimdOp::I32x4Bitmask):
            CHECK_NEXT(dispatchVectorReduction(BitmaskI32x4));
          case uint32_t(SimdOp::I64x2Bitmask):
            CHECK_NEXT(dispatchVectorReduction(BitmaskI64x2));
          case uint32_t(SimdOp::I8x16ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneI8x16, ValType::I32, 16));
          case uint32_t(SimdOp::I16x8ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneI16x8, ValType::I32, 8));
          case uint32_t(SimdOp::I32x4ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneI32x4, ValType::I32, 4));
          case uint32_t(SimdOp::I64x2ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneI64x2, ValType::I64, 2));
          case uint32_t(SimdOp::F32x4ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneF32x4, ValType::F32, 4));
          case uint32_t(SimdOp::F64x2ReplaceLane):
            CHECK_NEXT(dispatchReplaceLane(ReplaceLaneF64x2, ValType::F64, 2));
          case uint32_t(SimdOp::I8x16Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpI8x16, Assembler::Equal));
          case uint32_t(SimdOp::I8x16Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpI8x16, Assembler::NotEqual));
          case uint32_t(SimdOp::I8x16LtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI8x16, Assembler::LessThan));
          case uint32_t(SimdOp::I8x16LtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI8x16, Assembler::Below));
          case uint32_t(SimdOp::I8x16GtS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI8x16, Assembler::GreaterThan));
          case uint32_t(SimdOp::I8x16GtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI8x16, Assembler::Above));
          case uint32_t(SimdOp::I8x16LeS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI8x16, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::I8x16LeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI8x16, Assembler::BelowOrEqual));
          case uint32_t(SimdOp::I8x16GeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI8x16,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::I8x16GeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI8x16, Assembler::AboveOrEqual));
          case uint32_t(SimdOp::I16x8Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpI16x8, Assembler::Equal));
          case uint32_t(SimdOp::I16x8Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpI16x8, Assembler::NotEqual));
          case uint32_t(SimdOp::I16x8LtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI16x8, Assembler::LessThan));
          case uint32_t(SimdOp::I16x8LtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI16x8, Assembler::Below));
          case uint32_t(SimdOp::I16x8GtS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI16x8, Assembler::GreaterThan));
          case uint32_t(SimdOp::I16x8GtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI16x8, Assembler::Above));
          case uint32_t(SimdOp::I16x8LeS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI16x8, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::I16x8LeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI16x8, Assembler::BelowOrEqual));
          case uint32_t(SimdOp::I16x8GeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI16x8,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::I16x8GeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI16x8, Assembler::AboveOrEqual));
          case uint32_t(SimdOp::I32x4Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpI32x4, Assembler::Equal));
          case uint32_t(SimdOp::I32x4Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpI32x4, Assembler::NotEqual));
          case uint32_t(SimdOp::I32x4LtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI32x4, Assembler::LessThan));
          case uint32_t(SimdOp::I32x4LtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI32x4, Assembler::Below));
          case uint32_t(SimdOp::I32x4GtS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI32x4, Assembler::GreaterThan));
          case uint32_t(SimdOp::I32x4GtU):
            CHECK_NEXT(dispatchVectorComparison(CmpUI32x4, Assembler::Above));
          case uint32_t(SimdOp::I32x4LeS):
            CHECK_NEXT(
                dispatchVectorComparison(CmpI32x4, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::I32x4LeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI32x4, Assembler::BelowOrEqual));
          case uint32_t(SimdOp::I32x4GeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI32x4,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::I32x4GeU):
            CHECK_NEXT(
                dispatchVectorComparison(CmpUI32x4, Assembler::AboveOrEqual));
          case uint32_t(SimdOp::I64x2Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForEquality,
                                                Assembler::Equal));
          case uint32_t(SimdOp::I64x2Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForEquality,
                                                Assembler::NotEqual));
          case uint32_t(SimdOp::I64x2LtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForOrdering,
                                                Assembler::LessThan));
          case uint32_t(SimdOp::I64x2GtS):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForOrdering,
                                                Assembler::GreaterThan));
          case uint32_t(SimdOp::I64x2LeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForOrdering,
                                                Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::I64x2GeS):
            CHECK_NEXT(dispatchVectorComparison(CmpI64x2ForOrdering,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::F32x4Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpF32x4, Assembler::Equal));
          case uint32_t(SimdOp::F32x4Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpF32x4, Assembler::NotEqual));
          case uint32_t(SimdOp::F32x4Lt):
            CHECK_NEXT(dispatchVectorComparison(CmpF32x4, Assembler::LessThan));
          case uint32_t(SimdOp::F32x4Gt):
            CHECK_NEXT(
                dispatchVectorComparison(CmpF32x4, Assembler::GreaterThan));
          case uint32_t(SimdOp::F32x4Le):
            CHECK_NEXT(
                dispatchVectorComparison(CmpF32x4, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::F32x4Ge):
            CHECK_NEXT(dispatchVectorComparison(CmpF32x4,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::F64x2Eq):
            CHECK_NEXT(dispatchVectorComparison(CmpF64x2, Assembler::Equal));
          case uint32_t(SimdOp::F64x2Ne):
            CHECK_NEXT(dispatchVectorComparison(CmpF64x2, Assembler::NotEqual));
          case uint32_t(SimdOp::F64x2Lt):
            CHECK_NEXT(dispatchVectorComparison(CmpF64x2, Assembler::LessThan));
          case uint32_t(SimdOp::F64x2Gt):
            CHECK_NEXT(
                dispatchVectorComparison(CmpF64x2, Assembler::GreaterThan));
          case uint32_t(SimdOp::F64x2Le):
            CHECK_NEXT(
                dispatchVectorComparison(CmpF64x2, Assembler::LessThanOrEqual));
          case uint32_t(SimdOp::F64x2Ge):
            CHECK_NEXT(dispatchVectorComparison(CmpF64x2,
                                                Assembler::GreaterThanOrEqual));
          case uint32_t(SimdOp::V128And):
            CHECK_NEXT(dispatchVectorBinary(AndV128));
          case uint32_t(SimdOp::V128Or):
            CHECK_NEXT(dispatchVectorBinary(OrV128));
          case uint32_t(SimdOp::V128Xor):
            CHECK_NEXT(dispatchVectorBinary(XorV128));
          case uint32_t(SimdOp::V128AndNot):
            CHECK_NEXT(dispatchBinary0(emitVectorAndNot, ValType::V128));
          case uint32_t(SimdOp::I8x16AvgrU):
            CHECK_NEXT(dispatchVectorBinary(AverageUI8x16));
          case uint32_t(SimdOp::I16x8AvgrU):
            CHECK_NEXT(dispatchVectorBinary(AverageUI16x8));
          case uint32_t(SimdOp::I8x16Add):
            CHECK_NEXT(dispatchVectorBinary(AddI8x16));
          case uint32_t(SimdOp::I8x16AddSaturateS):
            CHECK_NEXT(dispatchVectorBinary(AddSatI8x16));
          case uint32_t(SimdOp::I8x16AddSaturateU):
            CHECK_NEXT(dispatchVectorBinary(AddSatUI8x16));
          case uint32_t(SimdOp::I8x16Sub):
            CHECK_NEXT(dispatchVectorBinary(SubI8x16));
          case uint32_t(SimdOp::I8x16SubSaturateS):
            CHECK_NEXT(dispatchVectorBinary(SubSatI8x16));
          case uint32_t(SimdOp::I8x16SubSaturateU):
            CHECK_NEXT(dispatchVectorBinary(SubSatUI8x16));
          case uint32_t(SimdOp::I8x16MinS):
            CHECK_NEXT(dispatchVectorBinary(MinI8x16));
          case uint32_t(SimdOp::I8x16MinU):
            CHECK_NEXT(dispatchVectorBinary(MinUI8x16));
          case uint32_t(SimdOp::I8x16MaxS):
            CHECK_NEXT(dispatchVectorBinary(MaxI8x16));
          case uint32_t(SimdOp::I8x16MaxU):
            CHECK_NEXT(dispatchVectorBinary(MaxUI8x16));
          case uint32_t(SimdOp::I16x8Add):
            CHECK_NEXT(dispatchVectorBinary(AddI16x8));
          case uint32_t(SimdOp::I16x8AddSaturateS):
            CHECK_NEXT(dispatchVectorBinary(AddSatI16x8));
          case uint32_t(SimdOp::I16x8AddSaturateU):
            CHECK_NEXT(dispatchVectorBinary(AddSatUI16x8));
          case uint32_t(SimdOp::I16x8Sub):
            CHECK_NEXT(dispatchVectorBinary(SubI16x8));
          case uint32_t(SimdOp::I16x8SubSaturateS):
            CHECK_NEXT(dispatchVectorBinary(SubSatI16x8));
          case uint32_t(SimdOp::I16x8SubSaturateU):
            CHECK_NEXT(dispatchVectorBinary(SubSatUI16x8));
          case uint32_t(SimdOp::I16x8Mul):
            CHECK_NEXT(dispatchVectorBinary(MulI16x8));
          case uint32_t(SimdOp::I16x8MinS):
            CHECK_NEXT(dispatchVectorBinary(MinI16x8));
          case uint32_t(SimdOp::I16x8MinU):
            CHECK_NEXT(dispatchVectorBinary(MinUI16x8));
          case uint32_t(SimdOp::I16x8MaxS):
            CHECK_NEXT(dispatchVectorBinary(MaxI16x8));
          case uint32_t(SimdOp::I16x8MaxU):
            CHECK_NEXT(dispatchVectorBinary(MaxUI16x8));
          case uint32_t(SimdOp::I32x4Add):
            CHECK_NEXT(dispatchVectorBinary(AddI32x4));
          case uint32_t(SimdOp::I32x4Sub):
            CHECK_NEXT(dispatchVectorBinary(SubI32x4));
          case uint32_t(SimdOp::I32x4Mul):
            CHECK_NEXT(dispatchVectorBinary(MulI32x4));
          case uint32_t(SimdOp::I32x4MinS):
            CHECK_NEXT(dispatchVectorBinary(MinI32x4));
          case uint32_t(SimdOp::I32x4MinU):
            CHECK_NEXT(dispatchVectorBinary(MinUI32x4));
          case uint32_t(SimdOp::I32x4MaxS):
            CHECK_NEXT(dispatchVectorBinary(MaxI32x4));
          case uint32_t(SimdOp::I32x4MaxU):
            CHECK_NEXT(dispatchVectorBinary(MaxUI32x4));
          case uint32_t(SimdOp::I64x2Add):
            CHECK_NEXT(dispatchVectorBinary(AddI64x2));
          case uint32_t(SimdOp::I64x2Sub):
            CHECK_NEXT(dispatchVectorBinary(SubI64x2));
          case uint32_t(SimdOp::I64x2Mul):
            CHECK_NEXT(dispatchVectorBinary(MulI64x2));
          case uint32_t(SimdOp::F32x4Add):
            CHECK_NEXT(dispatchVectorBinary(AddF32x4));
          case uint32_t(SimdOp::F32x4Sub):
            CHECK_NEXT(dispatchVectorBinary(SubF32x4));
          case uint32_t(SimdOp::F32x4Mul):
            CHECK_NEXT(dispatchVectorBinary(MulF32x4));
          case uint32_t(SimdOp::F32x4Div):
            CHECK_NEXT(dispatchVectorBinary(DivF32x4));
          case uint32_t(SimdOp::F32x4Min):
            CHECK_NEXT(dispatchVectorBinary(MinF32x4));
          case uint32_t(SimdOp::F32x4Max):
            CHECK_NEXT(dispatchVectorBinary(MaxF32x4));
          case uint32_t(SimdOp::F64x2Add):
            CHECK_NEXT(dispatchVectorBinary(AddF64x2));
          case uint32_t(SimdOp::F64x2Sub):
            CHECK_NEXT(dispatchVectorBinary(SubF64x2));
          case uint32_t(SimdOp::F64x2Mul):
            CHECK_NEXT(dispatchVectorBinary(MulF64x2));
          case uint32_t(SimdOp::F64x2Div):
            CHECK_NEXT(dispatchVectorBinary(DivF64x2));
          case uint32_t(SimdOp::F64x2Min):
            CHECK_NEXT(dispatchVectorBinary(MinF64x2));
          case uint32_t(SimdOp::F64x2Max):
            CHECK_NEXT(dispatchVectorBinary(MaxF64x2));
          case uint32_t(SimdOp::I8x16NarrowSI16x8):
            CHECK_NEXT(dispatchVectorBinary(NarrowI16x8));
          case uint32_t(SimdOp::I8x16NarrowUI16x8):
            CHECK_NEXT(dispatchVectorBinary(NarrowUI16x8));
          case uint32_t(SimdOp::I16x8NarrowSI32x4):
            CHECK_NEXT(dispatchVectorBinary(NarrowI32x4));
          case uint32_t(SimdOp::I16x8NarrowUI32x4):
            CHECK_NEXT(dispatchVectorBinary(NarrowUI32x4));
          case uint32_t(SimdOp::V8x16Swizzle):
            CHECK_NEXT(dispatchVectorBinary(Swizzle));
          case uint32_t(SimdOp::F32x4PMax):
            CHECK_NEXT(dispatchVectorBinary(PMaxF32x4));
          case uint32_t(SimdOp::F32x4PMin):
            CHECK_NEXT(dispatchVectorBinary(PMinF32x4));
          case uint32_t(SimdOp::F64x2PMax):
            CHECK_NEXT(dispatchVectorBinary(PMaxF64x2));
          case uint32_t(SimdOp::F64x2PMin):
            CHECK_NEXT(dispatchVectorBinary(PMinF64x2));
          case uint32_t(SimdOp::I32x4DotSI16x8):
            CHECK_NEXT(dispatchVectorBinary(DotI16x8));
          case uint32_t(SimdOp::I16x8ExtMulLowSI8x16):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowI8x16));
          case uint32_t(SimdOp::I16x8ExtMulHighSI8x16):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighI8x16));
          case uint32_t(SimdOp::I16x8ExtMulLowUI8x16):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowUI8x16));
          case uint32_t(SimdOp::I16x8ExtMulHighUI8x16):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighUI8x16));
          case uint32_t(SimdOp::I32x4ExtMulLowSI16x8):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowI16x8));
          case uint32_t(SimdOp::I32x4ExtMulHighSI16x8):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighI16x8));
          case uint32_t(SimdOp::I32x4ExtMulLowUI16x8):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowUI16x8));
          case uint32_t(SimdOp::I32x4ExtMulHighUI16x8):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighUI16x8));
          case uint32_t(SimdOp::I64x2ExtMulLowSI32x4):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowI32x4));
          case uint32_t(SimdOp::I64x2ExtMulHighSI32x4):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighI32x4));
          case uint32_t(SimdOp::I64x2ExtMulLowUI32x4):
            CHECK_NEXT(dispatchVectorBinary(ExtMulLowUI32x4));
          case uint32_t(SimdOp::I64x2ExtMulHighUI32x4):
            CHECK_NEXT(dispatchVectorBinary(ExtMulHighUI32x4));
          case uint32_t(SimdOp::I16x8Q15MulrSatS):
            CHECK_NEXT(dispatchVectorBinary(Q15MulrSatS));
          case uint32_t(SimdOp::I8x16Neg):
            CHECK_NEXT(dispatchVectorUnary(NegI8x16));
          case uint32_t(SimdOp::I16x8Neg):
            CHECK_NEXT(dispatchVectorUnary(NegI16x8));
          case uint32_t(SimdOp::I16x8WidenLowSI8x16):
            CHECK_NEXT(dispatchVectorUnary(WidenLowI8x16));
          case uint32_t(SimdOp::I16x8WidenHighSI8x16):
            CHECK_NEXT(dispatchVectorUnary(WidenHighI8x16));
          case uint32_t(SimdOp::I16x8WidenLowUI8x16):
            CHECK_NEXT(dispatchVectorUnary(WidenLowUI8x16));
          case uint32_t(SimdOp::I16x8WidenHighUI8x16):
            CHECK_NEXT(dispatchVectorUnary(WidenHighUI8x16));
          case uint32_t(SimdOp::I32x4Neg):
            CHECK_NEXT(dispatchVectorUnary(NegI32x4));
          case uint32_t(SimdOp::I32x4WidenLowSI16x8):
            CHECK_NEXT(dispatchVectorUnary(WidenLowI16x8));
          case uint32_t(SimdOp::I32x4WidenHighSI16x8):
            CHECK_NEXT(dispatchVectorUnary(WidenHighI16x8));
          case uint32_t(SimdOp::I32x4WidenLowUI16x8):
            CHECK_NEXT(dispatchVectorUnary(WidenLowUI16x8));
          case uint32_t(SimdOp::I32x4WidenHighUI16x8):
            CHECK_NEXT(dispatchVectorUnary(WidenHighUI16x8));
          case uint32_t(SimdOp::I32x4TruncSSatF32x4):
            CHECK_NEXT(dispatchVectorUnary(ConvertF32x4ToI32x4));
          case uint32_t(SimdOp::I32x4TruncUSatF32x4):
            CHECK_NEXT(dispatchVectorUnary(ConvertF32x4ToUI32x4));
          case uint32_t(SimdOp::I64x2Neg):
            CHECK_NEXT(dispatchVectorUnary(NegI64x2));
          case uint32_t(SimdOp::I64x2WidenLowSI32x4):
            CHECK_NEXT(dispatchVectorUnary(WidenLowI32x4));
          case uint32_t(SimdOp::I64x2WidenHighSI32x4):
            CHECK_NEXT(dispatchVectorUnary(WidenHighI32x4));
          case uint32_t(SimdOp::I64x2WidenLowUI32x4):
            CHECK_NEXT(dispatchVectorUnary(WidenLowUI32x4));
          case uint32_t(SimdOp::I64x2WidenHighUI32x4):
            CHECK_NEXT(dispatchVectorUnary(WidenHighUI32x4));
          case uint32_t(SimdOp::F32x4Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsF32x4));
          case uint32_t(SimdOp::F32x4Neg):
            CHECK_NEXT(dispatchVectorUnary(NegF32x4));
          case uint32_t(SimdOp::F32x4Sqrt):
            CHECK_NEXT(dispatchVectorUnary(SqrtF32x4));
          case uint32_t(SimdOp::F32x4ConvertSI32x4):
            CHECK_NEXT(dispatchVectorUnary(ConvertI32x4ToF32x4));
          case uint32_t(SimdOp::F32x4ConvertUI32x4):
            CHECK_NEXT(dispatchVectorUnary(ConvertUI32x4ToF32x4));
          case uint32_t(SimdOp::F32x4DemoteF64x2Zero):
            CHECK_NEXT(dispatchVectorUnary(DemoteF64x2ToF32x4));
          case uint32_t(SimdOp::F64x2PromoteLowF32x4):
            CHECK_NEXT(dispatchVectorUnary(PromoteF32x4ToF64x2));
          case uint32_t(SimdOp::F64x2ConvertLowI32x4S):
            CHECK_NEXT(dispatchVectorUnary(ConvertI32x4ToF64x2));
          case uint32_t(SimdOp::F64x2ConvertLowI32x4U):
            CHECK_NEXT(dispatchVectorUnary(ConvertUI32x4ToF64x2));
          case uint32_t(SimdOp::I32x4TruncSatF64x2SZero):
            CHECK_NEXT(dispatchVectorUnary(ConvertF64x2ToI32x4));
          case uint32_t(SimdOp::I32x4TruncSatF64x2UZero):
            CHECK_NEXT(dispatchVectorUnary(ConvertF64x2ToUI32x4));
          case uint32_t(SimdOp::F64x2Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsF64x2));
          case uint32_t(SimdOp::F64x2Neg):
            CHECK_NEXT(dispatchVectorUnary(NegF64x2));
          case uint32_t(SimdOp::F64x2Sqrt):
            CHECK_NEXT(dispatchVectorUnary(SqrtF64x2));
          case uint32_t(SimdOp::V128Not):
            CHECK_NEXT(dispatchVectorUnary(NotV128));
          case uint32_t(SimdOp::I8x16Popcnt):
            CHECK_NEXT(dispatchVectorUnary(PopcntI8x16));
          case uint32_t(SimdOp::I8x16Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsI8x16));
          case uint32_t(SimdOp::I16x8Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsI16x8));
          case uint32_t(SimdOp::I32x4Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsI32x4));
          case uint32_t(SimdOp::I64x2Abs):
            CHECK_NEXT(dispatchVectorUnary(AbsI64x2));
          case uint32_t(SimdOp::F32x4Ceil):
            CHECK_NEXT(dispatchVectorUnary(CeilF32x4));
          case uint32_t(SimdOp::F32x4Floor):
            CHECK_NEXT(dispatchVectorUnary(FloorF32x4));
          case uint32_t(SimdOp::F32x4Trunc):
            CHECK_NEXT(dispatchVectorUnary(TruncF32x4));
          case uint32_t(SimdOp::F32x4Nearest):
            CHECK_NEXT(dispatchVectorUnary(NearestF32x4));
          case uint32_t(SimdOp::F64x2Ceil):
            CHECK_NEXT(dispatchVectorUnary(CeilF64x2));
          case uint32_t(SimdOp::F64x2Floor):
            CHECK_NEXT(dispatchVectorUnary(FloorF64x2));
          case uint32_t(SimdOp::F64x2Trunc):
            CHECK_NEXT(dispatchVectorUnary(TruncF64x2));
          case uint32_t(SimdOp::F64x2Nearest):
            CHECK_NEXT(dispatchVectorUnary(NearestF64x2));
          case uint32_t(SimdOp::I16x8ExtAddPairwiseI8x16S):
            CHECK_NEXT(dispatchVectorUnary(ExtAddPairwiseI8x16));
          case uint32_t(SimdOp::I16x8ExtAddPairwiseI8x16U):
            CHECK_NEXT(dispatchVectorUnary(ExtAddPairwiseUI8x16));
          case uint32_t(SimdOp::I32x4ExtAddPairwiseI16x8S):
            CHECK_NEXT(dispatchVectorUnary(ExtAddPairwiseI16x8));
          case uint32_t(SimdOp::I32x4ExtAddPairwiseI16x8U):
            CHECK_NEXT(dispatchVectorUnary(ExtAddPairwiseUI16x8));
          case uint32_t(SimdOp::I8x16Shl):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftLeftI8x16));
          case uint32_t(SimdOp::I8x16ShrS):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightI8x16));
          case uint32_t(SimdOp::I8x16ShrU):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightUI8x16));
          case uint32_t(SimdOp::I16x8Shl):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftLeftI16x8));
          case uint32_t(SimdOp::I16x8ShrS):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightI16x8));
          case uint32_t(SimdOp::I16x8ShrU):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightUI16x8));
          case uint32_t(SimdOp::I32x4Shl):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftLeftI32x4));
          case uint32_t(SimdOp::I32x4ShrS):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightI32x4));
          case uint32_t(SimdOp::I32x4ShrU):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightUI32x4));
          case uint32_t(SimdOp::I64x2Shl):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftLeftI64x2));
          case uint32_t(SimdOp::I64x2ShrS):
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
            CHECK_NEXT(emitVectorShiftRightI64x2());
#  else
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightI64x2));
#  endif
          case uint32_t(SimdOp::I64x2ShrU):
            CHECK_NEXT(dispatchVectorVariableShift(ShiftRightUI64x2));
          case uint32_t(SimdOp::V128Bitselect):
            CHECK_NEXT(emitBitselect());
          case uint32_t(SimdOp::V8x16Shuffle):
            CHECK_NEXT(emitVectorShuffle());
          case uint32_t(SimdOp::V128Const): {
            V128 v128;
            CHECK(iter_.readV128Const(&v128));
            if (!deadCode_) {
              pushV128(v128);
            }
            NEXT();
          }
          case uint32_t(SimdOp::V128Load):
            CHECK_NEXT(emitLoad(ValType::V128, Scalar::Simd128));
          case uint32_t(SimdOp::V8x16LoadSplat):
            CHECK_NEXT(emitLoadSplat(Scalar::Uint8));
          case uint32_t(SimdOp::V16x8LoadSplat):
            CHECK_NEXT(emitLoadSplat(Scalar::Uint16));
          case uint32_t(SimdOp::V32x4LoadSplat):
            CHECK_NEXT(emitLoadSplat(Scalar::Uint32));
          case uint32_t(SimdOp::V64x2LoadSplat):
            CHECK_NEXT(emitLoadSplat(Scalar::Int64));
          case uint32_t(SimdOp::I16x8LoadS8x8):
            CHECK_NEXT(emitLoadExtend(Scalar::Int8));
          case uint32_t(SimdOp::I16x8LoadU8x8):
            CHECK_NEXT(emitLoadExtend(Scalar::Uint8));
          case uint32_t(SimdOp::I32x4LoadS16x4):
            CHECK_NEXT(emitLoadExtend(Scalar::Int16));
          case uint32_t(SimdOp::I32x4LoadU16x4):
            CHECK_NEXT(emitLoadExtend(Scalar::Uint16));
          case uint32_t(SimdOp::I64x2LoadS32x2):
            CHECK_NEXT(emitLoadExtend(Scalar::Int32));
          case uint32_t(SimdOp::I64x2LoadU32x2):
            CHECK_NEXT(emitLoadExtend(Scalar::Uint32));
          case uint32_t(SimdOp::V128Load32Zero):
            CHECK_NEXT(emitLoadZero(Scalar::Float32));
          case uint32_t(SimdOp::V128Load64Zero):
            CHECK_NEXT(emitLoadZero(Scalar::Float64));
          case uint32_t(SimdOp::V128Store):
            CHECK_NEXT(emitStore(ValType::V128, Scalar::Simd128));
          case uint32_t(SimdOp::V128Load8Lane):
            CHECK_NEXT(emitLoadLane(1));
          case uint32_t(SimdOp::V128Load16Lane):
            CHECK_NEXT(emitLoadLane(2));
          case uint32_t(SimdOp::V128Load32Lane):
            CHECK_NEXT(emitLoadLane(4));
          case uint32_t(SimdOp::V128Load64Lane):
            CHECK_NEXT(emitLoadLane(8));
          case uint32_t(SimdOp::V128Store8Lane):
            CHECK_NEXT(emitStoreLane(1));
          case uint32_t(SimdOp::V128Store16Lane):
            CHECK_NEXT(emitStoreLane(2));
          case uint32_t(SimdOp::V128Store32Lane):
            CHECK_NEXT(emitStoreLane(4));
          case uint32_t(SimdOp::V128Store64Lane):
            CHECK_NEXT(emitStoreLane(8));
          default:
            break;
        }  // switch (op.b1)
        return iter_.unrecognizedOpcode(&op);
      }
#endif  // ENABLE_WASM_SIMD

      // "Miscellaneous" operations
      case uint16_t(Op::MiscPrefix): {
        switch (op.b1) {
          case uint32_t(MiscOp::I32TruncSSatF32):
            CHECK_NEXT(
                dispatchConversionOOM(emitTruncateF32ToI32<TRUNC_SATURATING>,
                                      ValType::F32, ValType::I32));
          case uint32_t(MiscOp::I32TruncUSatF32):
            CHECK_NEXT(dispatchConversionOOM(
                emitTruncateF32ToI32<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                ValType::F32, ValType::I32));
          case uint32_t(MiscOp::I32TruncSSatF64):
            CHECK_NEXT(
                dispatchConversionOOM(emitTruncateF64ToI32<TRUNC_SATURATING>,
                                      ValType::F64, ValType::I32));
          case uint32_t(MiscOp::I32TruncUSatF64):
            CHECK_NEXT(dispatchConversionOOM(
                emitTruncateF64ToI32<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                ValType::F64, ValType::I32));
          case uint32_t(MiscOp::I64TruncSSatF32):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(dispatchCalloutConversionOOM(
                emitConvertFloatingToInt64Callout,
                SymbolicAddress::SaturatingTruncateDoubleToInt64, ValType::F32,
                ValType::I64));
#else
            CHECK_NEXT(
                dispatchConversionOOM(emitTruncateF32ToI64<TRUNC_SATURATING>,
                                      ValType::F32, ValType::I64));
#endif
          case uint32_t(MiscOp::I64TruncUSatF32):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(dispatchCalloutConversionOOM(
                emitConvertFloatingToInt64Callout,
                SymbolicAddress::SaturatingTruncateDoubleToUint64, ValType::F32,
                ValType::I64));
#else
            CHECK_NEXT(dispatchConversionOOM(
                emitTruncateF32ToI64<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                ValType::F32, ValType::I64));
#endif
          case uint32_t(MiscOp::I64TruncSSatF64):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(dispatchCalloutConversionOOM(
                emitConvertFloatingToInt64Callout,
                SymbolicAddress::SaturatingTruncateDoubleToInt64, ValType::F64,
                ValType::I64));
#else
            CHECK_NEXT(
                dispatchConversionOOM(emitTruncateF64ToI64<TRUNC_SATURATING>,
                                      ValType::F64, ValType::I64));
#endif
          case uint32_t(MiscOp::I64TruncUSatF64):
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
            CHECK_NEXT(dispatchCalloutConversionOOM(
                emitConvertFloatingToInt64Callout,
                SymbolicAddress::SaturatingTruncateDoubleToUint64, ValType::F64,
                ValType::I64));
#else
            CHECK_NEXT(dispatchConversionOOM(
                emitTruncateF64ToI64<TRUNC_UNSIGNED | TRUNC_SATURATING>,
                ValType::F64, ValType::I64));
#endif
          case uint32_t(MiscOp::MemCopy):
            CHECK_NEXT(emitMemCopy());
          case uint32_t(MiscOp::DataDrop):
            CHECK_NEXT(emitDataOrElemDrop(/*isData=*/true));
          case uint32_t(MiscOp::MemFill):
            CHECK_NEXT(emitMemFill());
          case uint32_t(MiscOp::MemInit):
            CHECK_NEXT(emitMemInit());
          case uint32_t(MiscOp::TableCopy):
            CHECK_NEXT(emitTableCopy());
          case uint32_t(MiscOp::ElemDrop):
            CHECK_NEXT(emitDataOrElemDrop(/*isData=*/false));
          case uint32_t(MiscOp::TableInit):
            CHECK_NEXT(emitTableInit());
          case uint32_t(MiscOp::TableFill):
            CHECK_NEXT(emitTableFill());
          case uint32_t(MiscOp::TableGrow):
            CHECK_NEXT(emitTableGrow());
          case uint32_t(MiscOp::TableSize):
            CHECK_NEXT(emitTableSize());
          default:
            break;
        }  // switch (op.b1)
        return iter_.unrecognizedOpcode(&op);
      }

      // Thread operations
      case uint16_t(Op::ThreadPrefix): {
        // Though thread ops can be used on nonshared memories, we make them
        // unavailable if shared memory has been disabled in the prefs, for
        // maximum predictability and safety and consistency with JS.
        if (moduleEnv_.sharedMemoryEnabled() == Shareable::False) {
          return iter_.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(ThreadOp::Wake):
            CHECK_NEXT(emitWake());

          case uint32_t(ThreadOp::I32Wait):
            CHECK_NEXT(emitWait(ValType::I32, 4));
          case uint32_t(ThreadOp::I64Wait):
            CHECK_NEXT(emitWait(ValType::I64, 8));
          case uint32_t(ThreadOp::Fence):
            CHECK_NEXT(emitFence());

          case uint32_t(ThreadOp::I32AtomicLoad):
            CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicLoad):
            CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicLoad8U):
            CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicLoad16U):
            CHECK_NEXT(emitAtomicLoad(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicLoad8U):
            CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicLoad16U):
            CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicLoad32U):
            CHECK_NEXT(emitAtomicLoad(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicStore):
            CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicStore):
            CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicStore8U):
            CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicStore16U):
            CHECK_NEXT(emitAtomicStore(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicStore8U):
            CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicStore16U):
            CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicStore32U):
            CHECK_NEXT(emitAtomicStore(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicAdd):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchAddOp));
          case uint32_t(ThreadOp::I64AtomicAdd):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchAddOp));
          case uint32_t(ThreadOp::I32AtomicAdd8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchAddOp));
          case uint32_t(ThreadOp::I32AtomicAdd16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchAddOp));
          case uint32_t(ThreadOp::I64AtomicAdd8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchAddOp));
          case uint32_t(ThreadOp::I64AtomicAdd16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchAddOp));
          case uint32_t(ThreadOp::I64AtomicAdd32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchAddOp));

          case uint32_t(ThreadOp::I32AtomicSub):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchSubOp));
          case uint32_t(ThreadOp::I64AtomicSub):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchSubOp));
          case uint32_t(ThreadOp::I32AtomicSub8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchSubOp));
          case uint32_t(ThreadOp::I32AtomicSub16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchSubOp));
          case uint32_t(ThreadOp::I64AtomicSub8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchSubOp));
          case uint32_t(ThreadOp::I64AtomicSub16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchSubOp));
          case uint32_t(ThreadOp::I64AtomicSub32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchSubOp));

          case uint32_t(ThreadOp::I32AtomicAnd):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchAndOp));
          case uint32_t(ThreadOp::I64AtomicAnd):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchAndOp));
          case uint32_t(ThreadOp::I32AtomicAnd8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchAndOp));
          case uint32_t(ThreadOp::I32AtomicAnd16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchAndOp));
          case uint32_t(ThreadOp::I64AtomicAnd8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchAndOp));
          case uint32_t(ThreadOp::I64AtomicAnd16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchAndOp));
          case uint32_t(ThreadOp::I64AtomicAnd32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchAndOp));

          case uint32_t(ThreadOp::I32AtomicOr):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I64AtomicOr):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I32AtomicOr8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I32AtomicOr16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I64AtomicOr8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I64AtomicOr16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I64AtomicOr32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchOrOp));

          case uint32_t(ThreadOp::I32AtomicXor):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicFetchXorOp));
          case uint32_t(ThreadOp::I64AtomicXor):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicFetchXorOp));
          case uint32_t(ThreadOp::I32AtomicXor8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicFetchXorOp));
          case uint32_t(ThreadOp::I32AtomicXor16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicFetchXorOp));
          case uint32_t(ThreadOp::I64AtomicXor8U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicFetchXorOp));
          case uint32_t(ThreadOp::I64AtomicXor16U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicFetchXorOp));
          case uint32_t(ThreadOp::I64AtomicXor32U):
            CHECK_NEXT(
                emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicFetchXorOp));

          case uint32_t(ThreadOp::I32AtomicXchg):
            CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicXchg):
            CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicXchg8U):
            CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicXchg16U):
            CHECK_NEXT(emitAtomicXchg(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicXchg8U):
            CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicXchg16U):
            CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicXchg32U):
            CHECK_NEXT(emitAtomicXchg(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicCmpXchg):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicCmpXchg):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicCmpXchg8U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicCmpXchg16U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicCmpXchg8U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicCmpXchg16U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicCmpXchg32U):
            CHECK_NEXT(emitAtomicCmpXchg(ValType::I64, Scalar::Uint32));

          default:
            return iter_.unrecognizedOpcode(&op);
        }
        break;
      }

      // asm.js and other private operations
      case uint16_t(Op::MozPrefix):
        return iter_.unrecognizedOpcode(&op);

      default:
        return iter_.unrecognizedOpcode(&op);
    }

#undef CHECK
#undef NEXT
#undef CHECK_NEXT
#undef CHECK_POINTER_COUNT
#undef dispatchBinary0
#undef dispatchBinary1
#undef dispatchBinary2
#undef dispatchBinary3
#undef dispatchUnary0
#undef dispatchUnary1
#undef dispatchUnary2
#undef dispatchComparison0
#undef dispatchConversion0
#undef dispatchConversion1
#undef dispatchConversionOOM
#undef dispatchCalloutConversionOOM
#undef dispatchIntDivCallout
#undef dispatchVectorBinary
#undef dispatchVectorUnary
#undef dispatchVectorComparison
#undef dispatchExtractLane
#undef dispatchReplaceLane
#undef dispatchSplat
#undef dispatchVectorReduction

    MOZ_CRASH("unreachable");
  }

  MOZ_CRASH("unreachable");
}

bool BaseCompiler::emitFunction() {
  if (!beginFunction()) {
    return false;
  }

  if (!emitBody()) {
    return false;
  }

  if (!endFunction()) {
    return false;
  }

  return true;
}

BaseCompiler::BaseCompiler(const ModuleEnvironment& moduleEnv,
                           const CompilerEnvironment& compilerEnv,
                           const FuncCompileInput& func,
                           const ValTypeVector& locals,
                           const MachineState& trapExitLayout,
                           size_t trapExitLayoutNumWords, Decoder& decoder,
                           StkVector& stkSource, TempAllocator* alloc,
                           MacroAssembler* masm, StackMaps* stackMaps)
    : moduleEnv_(moduleEnv),
      compilerEnv_(compilerEnv),
      iter_(moduleEnv, decoder),
      func_(func),
      lastReadCallSite_(0),
      alloc_(alloc->fallible()),
      locals_(locals),
      deadCode_(false),
      bceSafe_(0),
      latentOp_(LatentOp::None),
      latentType_(ValType::I32),
      latentIntCmp_(Assembler::Equal),
      latentDoubleCmp_(Assembler::DoubleEqual),
      masm(*masm),
      fr(*masm),
      stackMapGenerator_(stackMaps, trapExitLayout, trapExitLayoutNumWords,
                         *masm),
      stkSource_(stkSource) {
  // Our caller, BaselineCompileFunctions, will lend us the vector contents to
  // use for the eval stack.  To get hold of those contents, we'll temporarily
  // installing an empty one in its place.
  MOZ_ASSERT(stk_.empty());
  stk_.swap(stkSource_);

  // Assuming that previously processed wasm functions are well formed, the
  // eval stack should now be empty.  But empty it anyway; any non-emptyness
  // at this point will cause chaos.
  stk_.clear();
}

BaseCompiler::~BaseCompiler() {
  stk_.swap(stkSource_);
  // We've returned the eval stack vector contents to our caller,
  // BaselineCompileFunctions.  We expect the vector we get in return to be
  // empty since that's what we swapped for the stack vector in our
  // constructor.
  MOZ_ASSERT(stk_.empty());
}

bool BaseCompiler::init() {
  ra.init(this);

  if (!SigD_.append(ValType::F64)) {
    return false;
  }
  if (!SigF_.append(ValType::F32)) {
    return false;
  }

  ArgTypeVector args(funcType());
  return fr.setupLocals(locals_, args, compilerEnv_.debugEnabled(),
                        &localInfo_);
}

FuncOffsets BaseCompiler::finish() {
  MOZ_ASSERT(done(), "all bytes must be consumed");
  MOZ_ASSERT(func_.callSiteLineNums.length() == lastReadCallSite_);

  MOZ_ASSERT(stk_.empty());
  MOZ_ASSERT(stackMapGenerator_.memRefsOnStk == 0);

  masm.flushBuffer();

  return offsets_;
}

}  // namespace wasm
}  // namespace js

bool js::wasm::BaselinePlatformSupport() {
#if defined(JS_CODEGEN_ARM)
  // Simplifying assumption: require SDIV and UDIV.
  //
  // I have no good data on ARM populations allowing me to say that
  // X% of devices in the market implement SDIV and UDIV.  However,
  // they are definitely implemented on the Cortex-A7 and Cortex-A15
  // and on all ARMv8 systems.
  if (!HasIDIV()) {
    return false;
  }
#endif
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) ||   \
    defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  return true;
#else
  return false;
#endif
}

bool js::wasm::BaselineCompileFunctions(const ModuleEnvironment& moduleEnv,
                                        const CompilerEnvironment& compilerEnv,
                                        LifoAlloc& lifo,
                                        const FuncCompileInputVector& inputs,
                                        CompiledCode* code,
                                        UniqueChars* error) {
  MOZ_ASSERT(compilerEnv.tier() == Tier::Baseline);
  MOZ_ASSERT(moduleEnv.kind == ModuleKind::Wasm);

  // The MacroAssembler will sometimes access the jitContext.

  TempAllocator alloc(&lifo);
  JitContext jitContext(&alloc);
  MOZ_ASSERT(IsCompilingWasm());
  WasmMacroAssembler masm(alloc, moduleEnv);

  // Swap in already-allocated empty vectors to avoid malloc/free.
  MOZ_ASSERT(code->empty());
  if (!code->swap(masm)) {
    return false;
  }

  // Create a description of the stack layout created by GenerateTrapExit().
  MachineState trapExitLayout;
  size_t trapExitLayoutNumWords;
  GenerateTrapExitMachineState(&trapExitLayout, &trapExitLayoutNumWords);

  // The compiler's operand stack.  We reuse it across all functions so as to
  // avoid malloc/free.  Presize it to 128 elements in the hope of avoiding
  // reallocation later.
  StkVector stk;
  if (!stk.reserve(128)) {
    return false;
  }

  for (const FuncCompileInput& func : inputs) {
    Decoder d(func.begin, func.end, func.lineOrBytecode, error);

    // Build the local types vector.

    ValTypeVector locals;
    if (!locals.appendAll(moduleEnv.funcs[func.index].type->args())) {
      return false;
    }
    if (!DecodeLocalEntries(d, moduleEnv.types, moduleEnv.features, &locals)) {
      return false;
    }

    // One-pass baseline compilation.

    BaseCompiler f(moduleEnv, compilerEnv, func, locals, trapExitLayout,
                   trapExitLayoutNumWords, d, stk, &alloc, &masm,
                   &code->stackMaps);
    if (!f.init()) {
      return false;
    }
    if (!f.emitFunction()) {
      return false;
    }
    if (!code->codeRanges.emplaceBack(func.index, func.lineOrBytecode,
                                      f.finish())) {
      return false;
    }
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  return code->swap(masm);
}

#undef RABALDR_INT_DIV_I64_CALLOUT
#undef RABALDR_I64_TO_FLOAT_CALLOUT
#undef RABALDR_FLOAT_TO_I64_CALLOUT
