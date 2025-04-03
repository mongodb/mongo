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

// This is an INTERNAL header for Wasm baseline compiler: definitions of
// registers and the register allocator.

#ifndef wasm_wasm_baseline_regdefs_h
#define wasm_wasm_baseline_regdefs_h

#include "wasm/WasmBCDefs.h"

namespace js {
namespace wasm {

struct BaseCompiler;

using namespace js::jit;

//////////////////////////////////////////////////////////////////////////////
//
// Scratch register configuration.

#if defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)
#  define RABALDR_SCRATCH_I32
#  define RABALDR_SCRATCH_F32
#  define RABALDR_SCRATCH_F64

static constexpr Register RabaldrScratchI32 = Register::Invalid();
static constexpr FloatRegister RabaldrScratchF32 = InvalidFloatReg;
static constexpr FloatRegister RabaldrScratchF64 = InvalidFloatReg;
#endif

#ifdef JS_CODEGEN_ARM64
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
#endif

#ifdef JS_CODEGEN_ARM
// We use our own scratch register, because the macro assembler uses
// the regular scratch register(s) pretty liberally.  We could
// work around that in several cases but the mess does not seem
// worth it yet.  CallTempReg2 seems safe.

#  define RABALDR_SCRATCH_I32
static constexpr Register RabaldrScratchI32 = CallTempReg2;
#endif

#ifdef JS_CODEGEN_MIPS64
#  define RABALDR_SCRATCH_I32
static constexpr Register RabaldrScratchI32 = CallTempReg2;
#endif

#ifdef JS_CODEGEN_LOONG64
// We use our own scratch register, because the macro assembler uses
// the regular scratch register(s) pretty liberally.  We could
// work around that in several cases but the mess does not seem
// worth it yet.  CallTempReg2 seems safe.

#  define RABALDR_SCRATCH_I32
static constexpr Register RabaldrScratchI32 = CallTempReg2;
#endif

#ifdef RABALDR_SCRATCH_F32_ALIASES_F64
#  if !defined(RABALDR_SCRATCH_F32) || !defined(RABALDR_SCRATCH_F64)
#    error "Bad configuration"
#  endif
#endif

//////////////////////////////////////////////////////////////////////////////
//
// ...

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

//////////////////////////////////////////////////////////////////////////////
//
// Strongly typed register wrappers.

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

//////////////////////////////////////////////////////////////////////////////
//
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
#elif defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64)
struct SpecificRegs {
  // Required by gcc.
  SpecificRegs() {}
};
#else
struct SpecificRegs {
#  ifndef JS_64BIT
  RegI64 abiReturnRegI64;
#  endif

  SpecificRegs() { MOZ_CRASH("BaseCompiler porting interface: SpecificRegs"); }
};
#endif

//////////////////////////////////////////////////////////////////////////////
//
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

  BaseCompiler* bc;
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

#ifdef RABALDR_PIN_INSTANCE
    // If the InstanceReg is pinned then it is never available for
    // allocation.
    availGPR.take(InstanceReg);
#endif

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

  void init(BaseCompiler* bc) { this->bc = bc; }

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

  [[nodiscard]] inline RegI32 needI32();
  inline void needI32(RegI32 specific);

  [[nodiscard]] inline RegI64 needI64();
  inline void needI64(RegI64 specific);

  [[nodiscard]] inline RegRef needRef();
  inline void needRef(RegRef specific);

  [[nodiscard]] inline RegPtr needPtr();
  inline void needPtr(RegPtr specific);

  [[nodiscard]] inline RegF32 needF32();
  inline void needF32(RegF32 specific);

  [[nodiscard]] inline RegF64 needF64();
  inline void needF64(RegF64 specific);

#ifdef ENABLE_WASM_SIMD
  [[nodiscard]] inline RegV128 needV128();
  inline void needV128(RegV128 specific);
#endif

  inline void freeI32(RegI32 r);
  inline void freeI64(RegI64 r);
  inline void freeRef(RegRef r);
  inline void freePtr(RegPtr r);
  inline void freeF64(RegF64 r);
  inline void freeF32(RegF32 r);
#ifdef ENABLE_WASM_SIMD
  inline void freeV128(RegV128 r);
#endif

  // Use when you need a register for a short time but explicitly want to avoid
  // a full sync().
  [[nodiscard]] inline RegPtr needTempPtr(RegPtr fallback, bool* saved);
  inline void freeTempPtr(RegPtr r, bool saved);

#ifdef JS_CODEGEN_ARM
  [[nodiscard]] inline RegI64 needI64Pair();
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

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_regdefs_h
