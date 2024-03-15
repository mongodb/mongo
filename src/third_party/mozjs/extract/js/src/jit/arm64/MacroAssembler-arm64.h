/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_MacroAssembler_arm64_h
#define jit_arm64_MacroAssembler_arm64_h

#include "jit/arm64/Assembler-arm64.h"
#include "jit/arm64/vixl/Debugger-vixl.h"
#include "jit/arm64/vixl/MacroAssembler-vixl.h"
#include "jit/AtomicOp.h"
#include "jit/MoveResolver.h"
#include "vm/BigIntType.h"  // JS::BigInt
#include "wasm/WasmBuiltins.h"

#ifdef _M_ARM64
#  ifdef move32
#    undef move32
#  endif
#  ifdef move64
#    undef move64
#  endif
#endif

namespace js {
namespace jit {

// Import VIXL operands directly into the jit namespace for shared code.
using vixl::MemOperand;
using vixl::Operand;

struct ImmShiftedTag : public ImmWord {
  explicit ImmShiftedTag(JSValueShiftedTag shtag) : ImmWord((uintptr_t)shtag) {}

  explicit ImmShiftedTag(JSValueType type)
      : ImmWord(uintptr_t(JSValueShiftedTag(JSVAL_TYPE_TO_SHIFTED_TAG(type)))) {
  }
};

struct ImmTag : public Imm32 {
  explicit ImmTag(JSValueTag tag) : Imm32(tag) {}
};

class ScratchTagScope;

class MacroAssemblerCompat : public vixl::MacroAssembler {
 public:
  typedef vixl::Condition Condition;

 private:
  // Perform a downcast. Should be removed by Bug 996602.
  js::jit::MacroAssembler& asMasm();
  const js::jit::MacroAssembler& asMasm() const;

 public:
  // Restrict to only VIXL-internal functions.
  vixl::MacroAssembler& asVIXL();
  const MacroAssembler& asVIXL() const;

 protected:
  bool enoughMemory_;
  uint32_t framePushed_;

  MacroAssemblerCompat()
      : vixl::MacroAssembler(), enoughMemory_(true), framePushed_(0) {}

 protected:
  MoveResolver moveResolver_;

 public:
  bool oom() const { return Assembler::oom() || !enoughMemory_; }
  static ARMRegister toARMRegister(RegisterOrSP r, size_t size) {
    if (IsHiddenSP(r)) {
      MOZ_ASSERT(size == 64);
      return sp;
    }
    return ARMRegister(AsRegister(r), size);
  }
  static MemOperand toMemOperand(const Address& a) {
    return MemOperand(toARMRegister(a.base, 64), a.offset);
  }
  void doBaseIndex(const vixl::CPURegister& rt, const BaseIndex& addr,
                   vixl::LoadStoreOp op) {
    const ARMRegister base = toARMRegister(addr.base, 64);
    const ARMRegister index = ARMRegister(addr.index, 64);
    const unsigned scale = addr.scale;

    if (!addr.offset &&
        (!scale || scale == static_cast<unsigned>(CalcLSDataSize(op)))) {
      LoadStoreMacro(rt, MemOperand(base, index, vixl::LSL, scale), op);
      return;
    }

    vixl::UseScratchRegisterScope temps(this);
    ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(!scratch64.Is(rt));
    MOZ_ASSERT(!scratch64.Is(base));
    MOZ_ASSERT(!scratch64.Is(index));

    Add(scratch64, base, Operand(index, vixl::LSL, scale));
    LoadStoreMacro(rt, MemOperand(scratch64, addr.offset), op);
  }
  void Push(ARMRegister reg) {
    push(reg);
    adjustFrame(reg.size() / 8);
  }
  void Push(Register reg) {
    vixl::MacroAssembler::Push(ARMRegister(reg, 64));
    adjustFrame(8);
  }
  void Push(Imm32 imm) {
    push(imm);
    adjustFrame(8);
  }
  void Push(FloatRegister f) {
    push(ARMFPRegister(f, 64));
    adjustFrame(8);
  }
  void Push(ImmPtr imm) {
    push(imm);
    adjustFrame(sizeof(void*));
  }
  void push(FloatRegister f) {
    vixl::MacroAssembler::Push(ARMFPRegister(f, 64));
  }
  void push(ARMFPRegister f) { vixl::MacroAssembler::Push(f); }
  void push(Imm32 imm) {
    if (imm.value == 0) {
      vixl::MacroAssembler::Push(vixl::xzr);
    } else {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch64 = temps.AcquireX();
      move32(imm, scratch64.asUnsized());
      vixl::MacroAssembler::Push(scratch64);
    }
  }
  void push(ImmWord imm) {
    if (imm.value == 0) {
      vixl::MacroAssembler::Push(vixl::xzr);
    } else {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch64 = temps.AcquireX();
      Mov(scratch64, imm.value);
      vixl::MacroAssembler::Push(scratch64);
    }
  }
  void push(ImmPtr imm) {
    if (imm.value == nullptr) {
      vixl::MacroAssembler::Push(vixl::xzr);
    } else {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch64 = temps.AcquireX();
      movePtr(imm, scratch64.asUnsized());
      vixl::MacroAssembler::Push(scratch64);
    }
  }
  void push(ImmGCPtr imm) {
    if (imm.value == nullptr) {
      vixl::MacroAssembler::Push(vixl::xzr);
    } else {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch64 = temps.AcquireX();
      movePtr(imm, scratch64.asUnsized());
      vixl::MacroAssembler::Push(scratch64);
    }
  }
  void push(ARMRegister reg) { vixl::MacroAssembler::Push(reg); }
  void push(Address a) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(a.base != scratch64.asUnsized());
    loadPtr(a, scratch64.asUnsized());
    vixl::MacroAssembler::Push(scratch64);
  }

  // Push registers.
  void push(Register reg) { vixl::MacroAssembler::Push(ARMRegister(reg, 64)); }
  void push(RegisterOrSP reg) {
    if (IsHiddenSP(reg)) {
      vixl::MacroAssembler::Push(sp);
    }
    vixl::MacroAssembler::Push(toARMRegister(reg, 64));
  }
  void push(Register r0, Register r1) {
    vixl::MacroAssembler::Push(ARMRegister(r0, 64), ARMRegister(r1, 64));
  }
  void push(Register r0, Register r1, Register r2) {
    vixl::MacroAssembler::Push(ARMRegister(r0, 64), ARMRegister(r1, 64),
                               ARMRegister(r2, 64));
  }
  void push(Register r0, Register r1, Register r2, Register r3) {
    vixl::MacroAssembler::Push(ARMRegister(r0, 64), ARMRegister(r1, 64),
                               ARMRegister(r2, 64), ARMRegister(r3, 64));
  }
  void push(ARMFPRegister r0, ARMFPRegister r1, ARMFPRegister r2,
            ARMFPRegister r3) {
    vixl::MacroAssembler::Push(r0, r1, r2, r3);
  }

  // Pop registers.
  void pop(Register reg) { vixl::MacroAssembler::Pop(ARMRegister(reg, 64)); }
  void pop(Register r0, Register r1) {
    vixl::MacroAssembler::Pop(ARMRegister(r0, 64), ARMRegister(r1, 64));
  }
  void pop(Register r0, Register r1, Register r2) {
    vixl::MacroAssembler::Pop(ARMRegister(r0, 64), ARMRegister(r1, 64),
                              ARMRegister(r2, 64));
  }
  void pop(Register r0, Register r1, Register r2, Register r3) {
    vixl::MacroAssembler::Pop(ARMRegister(r0, 64), ARMRegister(r1, 64),
                              ARMRegister(r2, 64), ARMRegister(r3, 64));
  }
  void pop(ARMFPRegister r0, ARMFPRegister r1, ARMFPRegister r2,
           ARMFPRegister r3) {
    vixl::MacroAssembler::Pop(r0, r1, r2, r3);
  }

  void pop(const ValueOperand& v) { pop(v.valueReg()); }
  void pop(const FloatRegister& f) {
    vixl::MacroAssembler::Pop(ARMFPRegister(f, 64));
  }

  void implicitPop(uint32_t args) {
    MOZ_ASSERT(args % sizeof(intptr_t) == 0);
    adjustFrame(0 - args);
  }
  void Pop(ARMRegister r) {
    vixl::MacroAssembler::Pop(r);
    adjustFrame(0 - r.size() / 8);
  }
  // FIXME: This is the same on every arch.
  // FIXME: If we can share framePushed_, we can share this.
  // FIXME: Or just make it at the highest level.
  CodeOffset PushWithPatch(ImmWord word) {
    framePushed_ += sizeof(word.value);
    return pushWithPatch(word);
  }
  CodeOffset PushWithPatch(ImmPtr ptr) {
    return PushWithPatch(ImmWord(uintptr_t(ptr.value)));
  }

  uint32_t framePushed() const { return framePushed_; }
  void adjustFrame(int32_t diff) { setFramePushed(framePushed_ + diff); }

  void setFramePushed(uint32_t framePushed) { framePushed_ = framePushed; }

  void freeStack(Register amount) {
    vixl::MacroAssembler::Drop(Operand(ARMRegister(amount, 64)));
  }

  // Update sp with the value of the current active stack pointer, if necessary.
  void syncStackPtr() {
    if (!GetStackPointer64().Is(vixl::sp)) {
      Mov(vixl::sp, GetStackPointer64());
    }
  }
  void initPseudoStackPtr() {
    if (!GetStackPointer64().Is(vixl::sp)) {
      Mov(GetStackPointer64(), vixl::sp);
    }
  }
  // In debug builds only, cause a trap if PSP is active and PSP != SP
  void assertStackPtrsSynced(uint32_t id) {
#ifdef DEBUG
    // The add and sub instructions below will only take a 12-bit immediate.
    MOZ_ASSERT(id <= 0xFFF);
    if (!GetStackPointer64().Is(vixl::sp)) {
      Label ok;
      // Add a marker, so we can figure out who requested the check when
      // inspecting the generated code.  Note, a more concise way to encode
      // the marker would be to use it as an immediate for the `brk`
      // instruction as generated by `Unreachable()`, and removing the add/sub.
      Add(GetStackPointer64(), GetStackPointer64(), Operand(id));
      Sub(GetStackPointer64(), GetStackPointer64(), Operand(id));
      Cmp(vixl::sp, GetStackPointer64());
      B(Equal, &ok);
      Unreachable();
      bind(&ok);
    }
#endif
  }
  // In debug builds only, add a marker that doesn't change the machine's
  // state.  Note these markers are x16-based, as opposed to the x28-based
  // ones made by `assertStackPtrsSynced`.
  void addMarker(uint32_t id) {
#ifdef DEBUG
    // Only 12 bits of immediate are allowed.
    MOZ_ASSERT(id <= 0xFFF);
    ARMRegister x16 = ARMRegister(r16, 64);
    Add(x16, x16, Operand(id));
    Sub(x16, x16, Operand(id));
#endif
  }

  void storeValue(ValueOperand val, const Address& dest) {
    storePtr(val.valueReg(), dest);
  }

  template <typename T>
  void storeValue(JSValueType type, Register reg, const T& dest) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != reg);
    tagValue(type, reg, ValueOperand(scratch));
    storeValue(ValueOperand(scratch), dest);
  }
  template <typename T>
  void storeValue(const Value& val, const T& dest) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    moveValue(val, ValueOperand(scratch));
    storeValue(ValueOperand(scratch), dest);
  }
  void storeValue(ValueOperand val, BaseIndex dest) {
    storePtr(val.valueReg(), dest);
  }
  void storeValue(const Address& src, const Address& dest, Register temp) {
    loadPtr(src, temp);
    storePtr(temp, dest);
  }

  void storePrivateValue(Register src, const Address& dest) {
    storePtr(src, dest);
  }
  void storePrivateValue(ImmGCPtr imm, const Address& dest) {
    storePtr(imm, dest);
  }

  void loadValue(Address src, Register val) {
    Ldr(ARMRegister(val, 64), MemOperand(src));
  }
  void loadValue(Address src, ValueOperand val) {
    Ldr(ARMRegister(val.valueReg(), 64), MemOperand(src));
  }
  void loadValue(const BaseIndex& src, ValueOperand val) {
    doBaseIndex(ARMRegister(val.valueReg(), 64), src, vixl::LDR_x);
  }
  void loadUnalignedValue(const Address& src, ValueOperand dest) {
    loadValue(src, dest);
  }
  void tagValue(JSValueType type, Register payload, ValueOperand dest) {
    // This could be cleverer, but the first attempt had bugs.
    Orr(ARMRegister(dest.valueReg(), 64), ARMRegister(payload, 64),
        Operand(ImmShiftedTag(type).value));
  }
  void pushValue(ValueOperand val) {
    vixl::MacroAssembler::Push(ARMRegister(val.valueReg(), 64));
  }
  void popValue(ValueOperand val) {
    vixl::MacroAssembler::Pop(ARMRegister(val.valueReg(), 64));
    // SP may be < PSP now (that's OK).
    // eg testcase: tests/backup-point-bug1315634.js
  }
  void pushValue(const Value& val) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    if (val.isGCThing()) {
      BufferOffset load =
          movePatchablePtr(ImmPtr(val.bitsAsPunboxPointer()), scratch);
      writeDataRelocation(val, load);
      push(scratch);
    } else {
      moveValue(val, scratch);
      push(scratch);
    }
  }
  void pushValue(JSValueType type, Register reg) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != reg);
    tagValue(type, reg, ValueOperand(scratch));
    push(scratch);
  }
  void pushValue(const Address& addr) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != addr.base);
    loadValue(addr, scratch);
    push(scratch);
  }
  void pushValue(const BaseIndex& addr, Register scratch) {
    loadValue(addr, ValueOperand(scratch));
    pushValue(ValueOperand(scratch));
  }
  template <typename T>
  void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes,
                           JSValueType type) {
    switch (nbytes) {
      case 8: {
        vixl::UseScratchRegisterScope temps(this);
        const Register scratch = temps.AcquireX().asUnsized();
        if (type == JSVAL_TYPE_OBJECT) {
          unboxObjectOrNull(value, scratch);
        } else {
          unboxNonDouble(value, scratch, type);
        }
        storePtr(scratch, address);
        return;
      }
      case 4:
        store32(value.valueReg(), address);
        return;
      case 1:
        store8(value.valueReg(), address);
        return;
      default:
        MOZ_CRASH("Bad payload width");
    }
  }
  void moveValue(const Value& val, Register dest) {
    if (val.isGCThing()) {
      BufferOffset load =
          movePatchablePtr(ImmPtr(val.bitsAsPunboxPointer()), dest);
      writeDataRelocation(val, load);
    } else {
      movePtr(ImmWord(val.asRawBits()), dest);
    }
  }
  void moveValue(const Value& src, const ValueOperand& dest) {
    moveValue(src, dest.valueReg());
  }

  CodeOffset pushWithPatch(ImmWord imm) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    CodeOffset label = movWithPatch(imm, scratch);
    push(scratch);
    return label;
  }

  CodeOffset movWithPatch(ImmWord imm, Register dest) {
    BufferOffset off = immPool64(ARMRegister(dest, 64), imm.value);
    return CodeOffset(off.getOffset());
  }
  CodeOffset movWithPatch(ImmPtr imm, Register dest) {
    BufferOffset off = immPool64(ARMRegister(dest, 64), uint64_t(imm.value));
    return CodeOffset(off.getOffset());
  }

  void boxValue(JSValueType type, Register src, Register dest);

  void splitSignExtTag(Register src, Register dest) {
    sbfx(ARMRegister(dest, 64), ARMRegister(src, 64), JSVAL_TAG_SHIFT,
         (64 - JSVAL_TAG_SHIFT));
  }
  [[nodiscard]] Register extractTag(const Address& address, Register scratch) {
    loadPtr(address, scratch);
    splitSignExtTag(scratch, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractTag(const ValueOperand& value,
                                    Register scratch) {
    splitSignExtTag(value.valueReg(), scratch);
    return scratch;
  }
  [[nodiscard]] Register extractObject(const Address& address,
                                       Register scratch) {
    loadPtr(address, scratch);
    unboxObject(scratch, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractObject(const ValueOperand& value,
                                       Register scratch) {
    unboxObject(value, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractSymbol(const ValueOperand& value,
                                       Register scratch) {
    unboxSymbol(value, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractInt32(const ValueOperand& value,
                                      Register scratch) {
    unboxInt32(value, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractBoolean(const ValueOperand& value,
                                        Register scratch) {
    unboxBoolean(value, scratch);
    return scratch;
  }

  inline void ensureDouble(const ValueOperand& source, FloatRegister dest,
                           Label* failure);

  void emitSet(Condition cond, Register dest) {
    Cset(ARMRegister(dest, 64), cond);
  }

  void testNullSet(Condition cond, const ValueOperand& value, Register dest) {
    cond = testNull(cond, value);
    emitSet(cond, dest);
  }
  void testObjectSet(Condition cond, const ValueOperand& value, Register dest) {
    cond = testObject(cond, value);
    emitSet(cond, dest);
  }
  void testUndefinedSet(Condition cond, const ValueOperand& value,
                        Register dest) {
    cond = testUndefined(cond, value);
    emitSet(cond, dest);
  }

  void convertBoolToInt32(Register source, Register dest) {
    Uxtb(ARMRegister(dest, 64), ARMRegister(source, 64));
  }

  void convertInt32ToDouble(Register src, FloatRegister dest) {
    Scvtf(ARMFPRegister(dest, 64),
          ARMRegister(src, 32));  // Uses FPCR rounding mode.
  }
  void convertInt32ToDouble(const Address& src, FloatRegister dest) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != src.base);
    load32(src, scratch);
    convertInt32ToDouble(scratch, dest);
  }
  void convertInt32ToDouble(const BaseIndex& src, FloatRegister dest) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != src.base);
    MOZ_ASSERT(scratch != src.index);
    load32(src, scratch);
    convertInt32ToDouble(scratch, dest);
  }

  void convertInt32ToFloat32(Register src, FloatRegister dest) {
    Scvtf(ARMFPRegister(dest, 32),
          ARMRegister(src, 32));  // Uses FPCR rounding mode.
  }
  void convertInt32ToFloat32(const Address& src, FloatRegister dest) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != src.base);
    load32(src, scratch);
    convertInt32ToFloat32(scratch, dest);
  }

  void convertUInt32ToDouble(Register src, FloatRegister dest) {
    Ucvtf(ARMFPRegister(dest, 64),
          ARMRegister(src, 32));  // Uses FPCR rounding mode.
  }
  void convertUInt32ToDouble(const Address& src, FloatRegister dest) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != src.base);
    load32(src, scratch);
    convertUInt32ToDouble(scratch, dest);
  }

  void convertUInt32ToFloat32(Register src, FloatRegister dest) {
    Ucvtf(ARMFPRegister(dest, 32),
          ARMRegister(src, 32));  // Uses FPCR rounding mode.
  }
  void convertUInt32ToFloat32(const Address& src, FloatRegister dest) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != src.base);
    load32(src, scratch);
    convertUInt32ToFloat32(scratch, dest);
  }

  void convertFloat32ToDouble(FloatRegister src, FloatRegister dest) {
    Fcvt(ARMFPRegister(dest, 64), ARMFPRegister(src, 32));
  }
  void convertDoubleToFloat32(FloatRegister src, FloatRegister dest) {
    Fcvt(ARMFPRegister(dest, 32), ARMFPRegister(src, 64));
  }

  using vixl::MacroAssembler::B;

  void convertDoubleToInt32(FloatRegister src, Register dest, Label* fail,
                            bool negativeZeroCheck = true) {
    ARMFPRegister fsrc64(src, 64);
    ARMRegister dest32(dest, 32);

    // ARMv8.3 chips support the FJCVTZS instruction, which handles exactly this
    // logic.  But the simulator does not implement it, and when the simulator
    // runs on ARM64 hardware we want to override vixl's detection of it.
#if defined(JS_SIMULATOR_ARM64) && (defined(__aarch64__) || defined(_M_ARM64))
    const bool fjscvt = false;
#else
    const bool fjscvt =
        CPUHas(vixl::CPUFeatures::kFP, vixl::CPUFeatures::kJSCVT);
#endif
    if (fjscvt) {
      // Convert double to integer, rounding toward zero.
      // The Z-flag is set iff the conversion is exact. -0 unsets the Z-flag.
      Fjcvtzs(dest32, fsrc64);

      if (negativeZeroCheck) {
        B(fail, Assembler::NonZero);
      } else {
        Label done;
        B(&done, Assembler::Zero);  // If conversion was exact, go to end.

        // The conversion was inexact, but the caller intends to allow -0.

        // Compare fsrc64 to 0.
        // If fsrc64 == 0 and FJCVTZS conversion was inexact, then fsrc64 is -0.
        Fcmp(fsrc64, 0.0);
        B(fail, Assembler::NotEqual);  // Pass through -0; fail otherwise.

        bind(&done);
      }
    } else {
      // Older processors use a significantly slower path.
      ARMRegister dest64(dest, 64);

      vixl::UseScratchRegisterScope temps(this);
      const ARMFPRegister scratch64 = temps.AcquireD();
      MOZ_ASSERT(!scratch64.Is(fsrc64));

      Fcvtzs(dest32, fsrc64);    // Convert, rounding toward zero.
      Scvtf(scratch64, dest32);  // Convert back, using FPCR rounding mode.
      Fcmp(scratch64, fsrc64);
      B(fail, Assembler::NotEqual);

      if (negativeZeroCheck) {
        Label nonzero;
        Cbnz(dest32, &nonzero);
        Fmov(dest64, fsrc64);
        Cbnz(dest64, fail);
        bind(&nonzero);
      }
    }
  }
  void convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail,
                             bool negativeZeroCheck = true) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMFPRegister scratch32 = temps.AcquireS();

    ARMFPRegister fsrc(src, 32);
    ARMRegister dest32(dest, 32);
    ARMRegister dest64(dest, 64);

    MOZ_ASSERT(!scratch32.Is(fsrc));

    Fcvtzs(dest64, fsrc);      // Convert, rounding toward zero.
    Scvtf(scratch32, dest32);  // Convert back, using FPCR rounding mode.
    Fcmp(scratch32, fsrc);
    B(fail, Assembler::NotEqual);

    if (negativeZeroCheck) {
      Label nonzero;
      Cbnz(dest32, &nonzero);
      Fmov(dest32, fsrc);
      Cbnz(dest32, fail);
      bind(&nonzero);
    }
    Uxtw(dest64, dest64);
  }

  void convertDoubleToPtr(FloatRegister src, Register dest, Label* fail,
                          bool negativeZeroCheck = true) {
    ARMFPRegister fsrc64(src, 64);
    ARMRegister dest64(dest, 64);

    vixl::UseScratchRegisterScope temps(this);
    const ARMFPRegister scratch64 = temps.AcquireD();
    MOZ_ASSERT(!scratch64.Is(fsrc64));

    // Note: we can't use the FJCVTZS instruction here because that only works
    // for 32-bit values.

    Fcvtzs(dest64, fsrc64);    // Convert, rounding toward zero.
    Scvtf(scratch64, dest64);  // Convert back, using FPCR rounding mode.
    Fcmp(scratch64, fsrc64);
    B(fail, Assembler::NotEqual);

    if (negativeZeroCheck) {
      Label nonzero;
      Cbnz(dest64, &nonzero);
      Fmov(dest64, fsrc64);
      Cbnz(dest64, fail);
      bind(&nonzero);
    }
  }

  void jump(Label* label) { B(label); }
  void jump(JitCode* code) { branch(code); }
  void jump(ImmPtr ptr) {
    // It is unclear why this sync is necessary:
    // * PSP and SP have been observed to be different in testcase
    //   tests/asm.js/testBug1046688.js.
    // * Removing the sync causes no failures in all of jit-tests.
    //
    // Also see branch(JitCode*) below. This version of jump() is called only
    // from jump(TrampolinePtr) which is called on various very slow paths,
    // probably only in JS.
    syncStackPtr();
    BufferOffset loc =
        b(-1,
          LabelDoc());  // The jump target will be patched by executableCopy().
    addPendingJump(loc, ptr, RelocationKind::HARDCODED);
  }
  void jump(TrampolinePtr code) { jump(ImmPtr(code.value)); }
  void jump(Register reg) { Br(ARMRegister(reg, 64)); }
  void jump(const Address& addr) {
    vixl::UseScratchRegisterScope temps(this);
    MOZ_ASSERT(temps.IsAvailable(ScratchReg64));  // ip0
    temps.Exclude(ScratchReg64);
    MOZ_ASSERT(addr.base != ScratchReg64.asUnsized());
    loadPtr(addr, ScratchReg64.asUnsized());
    br(ScratchReg64);
  }

  void align(int alignment) { armbuffer_.align(alignment); }

  void haltingAlign(int alignment) {
    armbuffer_.align(alignment, vixl::HLT | ImmException(0xBAAD));
  }
  void nopAlign(int alignment) { armbuffer_.align(alignment); }

  void movePtr(Register src, Register dest) {
    Mov(ARMRegister(dest, 64), ARMRegister(src, 64));
  }
  void movePtr(ImmWord imm, Register dest) {
    Mov(ARMRegister(dest, 64), int64_t(imm.value));
  }
  void movePtr(ImmPtr imm, Register dest) {
    Mov(ARMRegister(dest, 64), int64_t(imm.value));
  }
  void movePtr(wasm::SymbolicAddress imm, Register dest) {
    BufferOffset off = movePatchablePtr(ImmWord(0xffffffffffffffffULL), dest);
    append(wasm::SymbolicAccess(CodeOffset(off.getOffset()), imm));
  }
  void movePtr(ImmGCPtr imm, Register dest) {
    BufferOffset load = movePatchablePtr(ImmPtr(imm.value), dest);
    writeDataRelocation(imm, load);
  }

  void mov(ImmWord imm, Register dest) { movePtr(imm, dest); }
  void mov(ImmPtr imm, Register dest) { movePtr(imm, dest); }
  void mov(wasm::SymbolicAddress imm, Register dest) { movePtr(imm, dest); }
  void mov(Register src, Register dest) { movePtr(src, dest); }
  void mov(CodeLabel* label, Register dest);

  void move32(Imm32 imm, Register dest) {
    Mov(ARMRegister(dest, 32), (int64_t)imm.value);
  }
  void move32(Register src, Register dest) {
    Mov(ARMRegister(dest, 32), ARMRegister(src, 32));
  }

  // Move a pointer using a literal pool, so that the pointer
  // may be easily patched or traced.
  // Returns the BufferOffset of the load instruction emitted.
  BufferOffset movePatchablePtr(ImmWord ptr, Register dest);
  BufferOffset movePatchablePtr(ImmPtr ptr, Register dest);

  void loadPtr(wasm::SymbolicAddress address, Register dest) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch = temps.AcquireX();
    movePtr(address, scratch.asUnsized());
    Ldr(ARMRegister(dest, 64), MemOperand(scratch));
  }
  void loadPtr(AbsoluteAddress address, Register dest) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch = temps.AcquireX();
    movePtr(ImmWord((uintptr_t)address.addr), scratch.asUnsized());
    Ldr(ARMRegister(dest, 64), MemOperand(scratch));
  }
  void loadPtr(const Address& address, Register dest) {
    Ldr(ARMRegister(dest, 64), MemOperand(address));
  }
  void loadPtr(const BaseIndex& src, Register dest) {
    ARMRegister base = toARMRegister(src.base, 64);
    uint32_t scale = Imm32::ShiftOf(src.scale).value;
    ARMRegister dest64(dest, 64);
    ARMRegister index64(src.index, 64);

    if (src.offset) {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch = temps.AcquireX();
      MOZ_ASSERT(!scratch.Is(base));
      MOZ_ASSERT(!scratch.Is(dest64));
      MOZ_ASSERT(!scratch.Is(index64));

      Add(scratch, base, Operand(int64_t(src.offset)));
      Ldr(dest64, MemOperand(scratch, index64, vixl::LSL, scale));
      return;
    }

    Ldr(dest64, MemOperand(base, index64, vixl::LSL, scale));
  }
  void loadPrivate(const Address& src, Register dest);

  void store8(Register src, const Address& address) {
    Strb(ARMRegister(src, 32), toMemOperand(address));
  }
  void store8(Imm32 imm, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != address.base);
    move32(imm, scratch32.asUnsized());
    Strb(scratch32, toMemOperand(address));
  }
  void store8(Register src, const BaseIndex& address) {
    doBaseIndex(ARMRegister(src, 32), address, vixl::STRB_w);
  }
  void store8(Imm32 imm, const BaseIndex& address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != address.base);
    MOZ_ASSERT(scratch32.asUnsized() != address.index);
    Mov(scratch32, Operand(imm.value));
    doBaseIndex(scratch32, address, vixl::STRB_w);
  }

  void store16(Register src, const Address& address) {
    Strh(ARMRegister(src, 32), toMemOperand(address));
  }
  void store16(Imm32 imm, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != address.base);
    move32(imm, scratch32.asUnsized());
    Strh(scratch32, toMemOperand(address));
  }
  void store16(Register src, const BaseIndex& address) {
    doBaseIndex(ARMRegister(src, 32), address, vixl::STRH_w);
  }
  void store16(Imm32 imm, const BaseIndex& address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != address.base);
    MOZ_ASSERT(scratch32.asUnsized() != address.index);
    Mov(scratch32, Operand(imm.value));
    doBaseIndex(scratch32, address, vixl::STRH_w);
  }
  template <typename S, typename T>
  void store16Unaligned(const S& src, const T& dest) {
    store16(src, dest);
  }

  void storePtr(ImmWord imm, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != address.base);
    movePtr(imm, scratch);
    storePtr(scratch, address);
  }
  void storePtr(ImmPtr imm, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != address.base);
    Mov(scratch64, uint64_t(imm.value));
    Str(scratch64, toMemOperand(address));
  }
  void storePtr(ImmGCPtr imm, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != address.base);
    movePtr(imm, scratch);
    storePtr(scratch, address);
  }
  void storePtr(Register src, const Address& address) {
    Str(ARMRegister(src, 64), toMemOperand(address));
  }

  void storePtr(ImmWord imm, const BaseIndex& address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != address.base);
    MOZ_ASSERT(scratch64.asUnsized() != address.index);
    Mov(scratch64, Operand(imm.value));
    doBaseIndex(scratch64, address, vixl::STR_x);
  }
  void storePtr(ImmGCPtr imm, const BaseIndex& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != address.base);
    MOZ_ASSERT(scratch != address.index);
    movePtr(imm, scratch);
    doBaseIndex(ARMRegister(scratch, 64), address, vixl::STR_x);
  }
  void storePtr(Register src, const BaseIndex& address) {
    doBaseIndex(ARMRegister(src, 64), address, vixl::STR_x);
  }

  void storePtr(Register src, AbsoluteAddress address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    Mov(scratch64, uint64_t(address.addr));
    Str(ARMRegister(src, 64), MemOperand(scratch64));
  }

  void store32(Register src, AbsoluteAddress address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    Mov(scratch64, uint64_t(address.addr));
    Str(ARMRegister(src, 32), MemOperand(scratch64));
  }
  void store32(Imm32 imm, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != address.base);
    Mov(scratch32, uint64_t(imm.value));
    Str(scratch32, toMemOperand(address));
  }
  void store32(Register r, const Address& address) {
    Str(ARMRegister(r, 32), toMemOperand(address));
  }
  void store32(Imm32 imm, const BaseIndex& address) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != address.base);
    MOZ_ASSERT(scratch32.asUnsized() != address.index);
    Mov(scratch32, imm.value);
    doBaseIndex(scratch32, address, vixl::STR_w);
  }
  void store32(Register r, const BaseIndex& address) {
    doBaseIndex(ARMRegister(r, 32), address, vixl::STR_w);
  }

  template <typename S, typename T>
  void store32Unaligned(const S& src, const T& dest) {
    store32(src, dest);
  }

  void store64(Register64 src, Address address) { storePtr(src.reg, address); }

  void store64(Register64 src, const BaseIndex& address) {
    storePtr(src.reg, address);
  }

  void store64(Imm64 imm, const BaseIndex& address) {
    storePtr(ImmWord(imm.value), address);
  }

  void store64(Imm64 imm, const Address& address) {
    storePtr(ImmWord(imm.value), address);
  }

  template <typename S, typename T>
  void store64Unaligned(const S& src, const T& dest) {
    store64(src, dest);
  }

  // StackPointer manipulation.
  inline void addToStackPtr(Register src);
  inline void addToStackPtr(Imm32 imm);
  inline void addToStackPtr(const Address& src);
  inline void addStackPtrTo(Register dest);

  inline void subFromStackPtr(Register src);
  inline void subFromStackPtr(Imm32 imm);
  inline void subStackPtrFrom(Register dest);

  inline void andToStackPtr(Imm32 t);

  inline void moveToStackPtr(Register src);
  inline void moveStackPtrTo(Register dest);

  inline void loadStackPtr(const Address& src);
  inline void storeStackPtr(const Address& dest);

  // StackPointer testing functions.
  inline void branchTestStackPtr(Condition cond, Imm32 rhs, Label* label);
  inline void branchStackPtr(Condition cond, Register rhs, Label* label);
  inline void branchStackPtrRhs(Condition cond, Address lhs, Label* label);
  inline void branchStackPtrRhs(Condition cond, AbsoluteAddress lhs,
                                Label* label);

  void testPtr(Register lhs, Register rhs) {
    Tst(ARMRegister(lhs, 64), Operand(ARMRegister(rhs, 64)));
  }
  void test32(Register lhs, Register rhs) {
    Tst(ARMRegister(lhs, 32), Operand(ARMRegister(rhs, 32)));
  }
  void test32(const Address& addr, Imm32 imm) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != addr.base);
    load32(addr, scratch32.asUnsized());
    Tst(scratch32, Operand(imm.value));
  }
  void test32(Register lhs, Imm32 rhs) {
    Tst(ARMRegister(lhs, 32), Operand(rhs.value));
  }
  void cmp32(Register lhs, Imm32 rhs) {
    Cmp(ARMRegister(lhs, 32), Operand(rhs.value));
  }
  void cmp32(Register a, Register b) {
    Cmp(ARMRegister(a, 32), Operand(ARMRegister(b, 32)));
  }
  void cmp32(const Address& lhs, Imm32 rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != lhs.base);
    Ldr(scratch32, toMemOperand(lhs));
    Cmp(scratch32, Operand(rhs.value));
  }
  void cmp32(const Address& lhs, Register rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != lhs.base);
    MOZ_ASSERT(scratch32.asUnsized() != rhs);
    Ldr(scratch32, toMemOperand(lhs));
    Cmp(scratch32, Operand(ARMRegister(rhs, 32)));
  }
  void cmp32(const vixl::Operand& lhs, Imm32 rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    Mov(scratch32, lhs);
    Cmp(scratch32, Operand(rhs.value));
  }
  void cmp32(const vixl::Operand& lhs, Register rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    Mov(scratch32, lhs);
    Cmp(scratch32, Operand(ARMRegister(rhs, 32)));
  }

  void cmn32(Register lhs, Imm32 rhs) {
    Cmn(ARMRegister(lhs, 32), Operand(rhs.value));
  }

  void cmpPtr(Register lhs, Imm32 rhs) {
    Cmp(ARMRegister(lhs, 64), Operand(rhs.value));
  }
  void cmpPtr(Register lhs, ImmWord rhs) {
    Cmp(ARMRegister(lhs, 64), Operand(rhs.value));
  }
  void cmpPtr(Register lhs, ImmPtr rhs) {
    Cmp(ARMRegister(lhs, 64), Operand(uint64_t(rhs.value)));
  }
  void cmpPtr(Register lhs, Imm64 rhs) {
    Cmp(ARMRegister(lhs, 64), Operand(uint64_t(rhs.value)));
  }
  void cmpPtr(Register lhs, Register rhs) {
    Cmp(ARMRegister(lhs, 64), ARMRegister(rhs, 64));
  }
  void cmpPtr(Register lhs, ImmGCPtr rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs);
    movePtr(rhs, scratch);
    cmpPtr(lhs, scratch);
  }

  void cmpPtr(const Address& lhs, Register rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != lhs.base);
    MOZ_ASSERT(scratch64.asUnsized() != rhs);
    Ldr(scratch64, toMemOperand(lhs));
    Cmp(scratch64, Operand(ARMRegister(rhs, 64)));
  }
  void cmpPtr(const Address& lhs, ImmWord rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != lhs.base);
    Ldr(scratch64, toMemOperand(lhs));
    Cmp(scratch64, Operand(rhs.value));
  }
  void cmpPtr(const Address& lhs, ImmPtr rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != lhs.base);
    Ldr(scratch64, toMemOperand(lhs));
    Cmp(scratch64, Operand(uint64_t(rhs.value)));
  }
  void cmpPtr(const Address& lhs, ImmGCPtr rhs) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != lhs.base);
    loadPtr(lhs, scratch);
    cmpPtr(scratch, rhs);
  }

  void loadDouble(const Address& src, FloatRegister dest) {
    Ldr(ARMFPRegister(dest, 64), MemOperand(src));
  }
  void loadDouble(const BaseIndex& src, FloatRegister dest) {
    ARMRegister base = toARMRegister(src.base, 64);
    ARMRegister index(src.index, 64);

    if (src.offset == 0) {
      Ldr(ARMFPRegister(dest, 64),
          MemOperand(base, index, vixl::LSL, unsigned(src.scale)));
      return;
    }

    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT(scratch64.asUnsized() != src.base);
    MOZ_ASSERT(scratch64.asUnsized() != src.index);

    Add(scratch64, base, Operand(index, vixl::LSL, unsigned(src.scale)));
    Ldr(ARMFPRegister(dest, 64), MemOperand(scratch64, src.offset));
  }
  void loadFloatAsDouble(const Address& addr, FloatRegister dest) {
    Ldr(ARMFPRegister(dest, 32), toMemOperand(addr));
    fcvt(ARMFPRegister(dest, 64), ARMFPRegister(dest, 32));
  }
  void loadFloatAsDouble(const BaseIndex& src, FloatRegister dest) {
    ARMRegister base = toARMRegister(src.base, 64);
    ARMRegister index(src.index, 64);
    if (src.offset == 0) {
      Ldr(ARMFPRegister(dest, 32),
          MemOperand(base, index, vixl::LSL, unsigned(src.scale)));
    } else {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch64 = temps.AcquireX();
      MOZ_ASSERT(scratch64.asUnsized() != src.base);
      MOZ_ASSERT(scratch64.asUnsized() != src.index);

      Add(scratch64, base, Operand(index, vixl::LSL, unsigned(src.scale)));
      Ldr(ARMFPRegister(dest, 32), MemOperand(scratch64, src.offset));
    }
    fcvt(ARMFPRegister(dest, 64), ARMFPRegister(dest, 32));
  }

  void loadFloat32(const Address& addr, FloatRegister dest) {
    Ldr(ARMFPRegister(dest, 32), toMemOperand(addr));
  }
  void loadFloat32(const BaseIndex& src, FloatRegister dest) {
    ARMRegister base = toARMRegister(src.base, 64);
    ARMRegister index(src.index, 64);
    if (src.offset == 0) {
      Ldr(ARMFPRegister(dest, 32),
          MemOperand(base, index, vixl::LSL, unsigned(src.scale)));
    } else {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch64 = temps.AcquireX();
      MOZ_ASSERT(scratch64.asUnsized() != src.base);
      MOZ_ASSERT(scratch64.asUnsized() != src.index);

      Add(scratch64, base, Operand(index, vixl::LSL, unsigned(src.scale)));
      Ldr(ARMFPRegister(dest, 32), MemOperand(scratch64, src.offset));
    }
  }

  void moveDouble(FloatRegister src, FloatRegister dest) {
    fmov(ARMFPRegister(dest, 64), ARMFPRegister(src, 64));
  }
  void zeroDouble(FloatRegister reg) {
    fmov(ARMFPRegister(reg, 64), vixl::xzr);
  }
  void zeroFloat32(FloatRegister reg) {
    fmov(ARMFPRegister(reg, 32), vixl::wzr);
  }

  void moveFloat32(FloatRegister src, FloatRegister dest) {
    fmov(ARMFPRegister(dest, 32), ARMFPRegister(src, 32));
  }
  void moveFloatAsDouble(Register src, FloatRegister dest) {
    MOZ_CRASH("moveFloatAsDouble");
  }

  void moveSimd128(FloatRegister src, FloatRegister dest) {
    fmov(ARMFPRegister(dest, 128), ARMFPRegister(src, 128));
  }

  void splitSignExtTag(const ValueOperand& operand, Register dest) {
    splitSignExtTag(operand.valueReg(), dest);
  }
  void splitSignExtTag(const Address& operand, Register dest) {
    loadPtr(operand, dest);
    splitSignExtTag(dest, dest);
  }
  void splitSignExtTag(const BaseIndex& operand, Register dest) {
    loadPtr(operand, dest);
    splitSignExtTag(dest, dest);
  }

  // Extracts the tag of a value and places it in tag
  inline void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag);
  void cmpTag(const ValueOperand& operand, ImmTag tag) { MOZ_CRASH("cmpTag"); }

  void load32(const Address& address, Register dest) {
    Ldr(ARMRegister(dest, 32), toMemOperand(address));
  }
  void load32(const BaseIndex& src, Register dest) {
    doBaseIndex(ARMRegister(dest, 32), src, vixl::LDR_w);
  }
  void load32(AbsoluteAddress address, Register dest) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    movePtr(ImmWord((uintptr_t)address.addr), scratch64.asUnsized());
    ldr(ARMRegister(dest, 32), MemOperand(scratch64));
  }
  template <typename S>
  void load32Unaligned(const S& src, Register dest) {
    load32(src, dest);
  }
  void load64(const Address& address, Register64 dest) {
    loadPtr(address, dest.reg);
  }
  void load64(const BaseIndex& address, Register64 dest) {
    loadPtr(address, dest.reg);
  }
  template <typename S>
  void load64Unaligned(const S& src, Register64 dest) {
    load64(src, dest);
  }

  void load8SignExtend(const Address& address, Register dest) {
    Ldrsb(ARMRegister(dest, 32), toMemOperand(address));
  }
  void load8SignExtend(const BaseIndex& src, Register dest) {
    doBaseIndex(ARMRegister(dest, 32), src, vixl::LDRSB_w);
  }

  void load8ZeroExtend(const Address& address, Register dest) {
    Ldrb(ARMRegister(dest, 32), toMemOperand(address));
  }
  void load8ZeroExtend(const BaseIndex& src, Register dest) {
    doBaseIndex(ARMRegister(dest, 32), src, vixl::LDRB_w);
  }

  void load16SignExtend(const Address& address, Register dest) {
    Ldrsh(ARMRegister(dest, 32), toMemOperand(address));
  }
  void load16SignExtend(const BaseIndex& src, Register dest) {
    doBaseIndex(ARMRegister(dest, 32), src, vixl::LDRSH_w);
  }
  template <typename S>
  void load16UnalignedSignExtend(const S& src, Register dest) {
    load16SignExtend(src, dest);
  }

  void load16ZeroExtend(const Address& address, Register dest) {
    Ldrh(ARMRegister(dest, 32), toMemOperand(address));
  }
  void load16ZeroExtend(const BaseIndex& src, Register dest) {
    doBaseIndex(ARMRegister(dest, 32), src, vixl::LDRH_w);
  }
  template <typename S>
  void load16UnalignedZeroExtend(const S& src, Register dest) {
    load16ZeroExtend(src, dest);
  }

  void adds32(Register src, Register dest) {
    Adds(ARMRegister(dest, 32), ARMRegister(dest, 32),
         Operand(ARMRegister(src, 32)));
  }
  void adds32(Imm32 imm, Register dest) {
    Adds(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(imm.value));
  }
  void adds32(Imm32 imm, const Address& dest) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != dest.base);

    Ldr(scratch32, toMemOperand(dest));
    Adds(scratch32, scratch32, Operand(imm.value));
    Str(scratch32, toMemOperand(dest));
  }
  void adds64(Imm32 imm, Register dest) {
    Adds(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(imm.value));
  }
  void adds64(ImmWord imm, Register dest) {
    Adds(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(imm.value));
  }
  void adds64(Register src, Register dest) {
    Adds(ARMRegister(dest, 64), ARMRegister(dest, 64),
         Operand(ARMRegister(src, 64)));
  }

  void subs32(Imm32 imm, Register dest) {
    Subs(ARMRegister(dest, 32), ARMRegister(dest, 32), Operand(imm.value));
  }
  void subs32(Register src, Register dest) {
    Subs(ARMRegister(dest, 32), ARMRegister(dest, 32),
         Operand(ARMRegister(src, 32)));
  }
  void subs64(Imm32 imm, Register dest) {
    Subs(ARMRegister(dest, 64), ARMRegister(dest, 64), Operand(imm.value));
  }
  void subs64(Register src, Register dest) {
    Subs(ARMRegister(dest, 64), ARMRegister(dest, 64),
         Operand(ARMRegister(src, 64)));
  }

  void negs32(Register reg) {
    Negs(ARMRegister(reg, 32), Operand(ARMRegister(reg, 32)));
  }

  void ret() {
    pop(lr);
    abiret();
  }

  void retn(Imm32 n) {
    vixl::UseScratchRegisterScope temps(this);
    MOZ_ASSERT(temps.IsAvailable(ScratchReg64));  // ip0
    temps.Exclude(ScratchReg64);
    // ip0 <- [sp]; sp += n; ret ip0
    Ldr(ScratchReg64,
        MemOperand(GetStackPointer64(), ptrdiff_t(n.value), vixl::PostIndex));
    syncStackPtr();  // SP is always used to transmit the stack between calls.
    Ret(ScratchReg64);
  }

  void j(Condition cond, Label* dest) { B(dest, cond); }

  void branch(Condition cond, Label* label) { B(label, cond); }
  void branch(JitCode* target) {
    // It is unclear why this sync is necessary:
    // * PSP and SP have been observed to be different in testcase
    //   tests/async/debugger-reject-after-fulfill.js
    // * Removing the sync causes no failures in all of jit-tests.
    //
    // Also see jump() above.  This is used only to implement jump(JitCode*)
    // and only for JS, it appears.
    syncStackPtr();
    BufferOffset loc =
        b(-1,
          LabelDoc());  // The jump target will be patched by executableCopy().
    addPendingJump(loc, ImmPtr(target->raw()), RelocationKind::JITCODE);
  }

  void compareDouble(DoubleCondition cond, FloatRegister lhs,
                     FloatRegister rhs) {
    Fcmp(ARMFPRegister(lhs, 64), ARMFPRegister(rhs, 64));
  }

  void compareFloat(DoubleCondition cond, FloatRegister lhs,
                    FloatRegister rhs) {
    Fcmp(ARMFPRegister(lhs, 32), ARMFPRegister(rhs, 32));
  }

  void compareSimd128Int(Assembler::Condition cond, ARMFPRegister dest,
                         ARMFPRegister lhs, ARMFPRegister rhs);
  void compareSimd128Float(Assembler::Condition cond, ARMFPRegister dest,
                           ARMFPRegister lhs, ARMFPRegister rhs);
  void rightShiftInt8x16(FloatRegister lhs, Register rhs, FloatRegister dest,
                         bool isUnsigned);
  void rightShiftInt16x8(FloatRegister lhs, Register rhs, FloatRegister dest,
                         bool isUnsigned);
  void rightShiftInt32x4(FloatRegister lhs, Register rhs, FloatRegister dest,
                         bool isUnsigned);
  void rightShiftInt64x2(FloatRegister lhs, Register rhs, FloatRegister dest,
                         bool isUnsigned);

  void branchNegativeZero(FloatRegister reg, Register scratch, Label* label) {
    MOZ_CRASH("branchNegativeZero");
  }
  void branchNegativeZeroFloat32(FloatRegister reg, Register scratch,
                                 Label* label) {
    MOZ_CRASH("branchNegativeZeroFloat32");
  }

  void boxDouble(FloatRegister src, const ValueOperand& dest, FloatRegister) {
    Fmov(ARMRegister(dest.valueReg(), 64), ARMFPRegister(src, 64));
  }
  void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest) {
    boxValue(type, src, dest.valueReg());
  }

  // Note that the |dest| register here may be ScratchReg, so we shouldn't use
  // it.
  void unboxInt32(const ValueOperand& src, Register dest) {
    move32(src.valueReg(), dest);
  }
  void unboxInt32(const Address& src, Register dest) { load32(src, dest); }
  void unboxInt32(const BaseIndex& src, Register dest) { load32(src, dest); }

  template <typename T>
  void unboxDouble(const T& src, FloatRegister dest) {
    loadDouble(src, dest);
  }
  void unboxDouble(const ValueOperand& src, FloatRegister dest) {
    Fmov(ARMFPRegister(dest, 64), ARMRegister(src.valueReg(), 64));
  }

  void unboxArgObjMagic(const ValueOperand& src, Register dest) {
    MOZ_CRASH("unboxArgObjMagic");
  }
  void unboxArgObjMagic(const Address& src, Register dest) {
    MOZ_CRASH("unboxArgObjMagic");
  }

  void unboxBoolean(const ValueOperand& src, Register dest) {
    move32(src.valueReg(), dest);
  }
  void unboxBoolean(const Address& src, Register dest) { load32(src, dest); }
  void unboxBoolean(const BaseIndex& src, Register dest) { load32(src, dest); }

  void unboxMagic(const ValueOperand& src, Register dest) {
    move32(src.valueReg(), dest);
  }
  void unboxNonDouble(const ValueOperand& src, Register dest,
                      JSValueType type) {
    unboxNonDouble(src.valueReg(), dest, type);
  }

  template <typename T>
  void unboxNonDouble(T src, Register dest, JSValueType type) {
    MOZ_ASSERT(type != JSVAL_TYPE_DOUBLE);
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      load32(src, dest);
      return;
    }
    loadPtr(src, dest);
    unboxNonDouble(dest, dest, type);
  }

  void unboxNonDouble(Register src, Register dest, JSValueType type) {
    MOZ_ASSERT(type != JSVAL_TYPE_DOUBLE);
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      move32(src, dest);
      return;
    }
    Eor(ARMRegister(dest, 64), ARMRegister(src, 64),
        Operand(JSVAL_TYPE_TO_SHIFTED_TAG(type)));
  }

  void notBoolean(const ValueOperand& val) {
    ARMRegister r(val.valueReg(), 64);
    eor(r, r, Operand(1));
  }
  void unboxObject(const ValueOperand& src, Register dest) {
    unboxNonDouble(src.valueReg(), dest, JSVAL_TYPE_OBJECT);
  }
  void unboxObject(Register src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
  }
  void unboxObject(const Address& src, Register dest) {
    loadPtr(src, dest);
    unboxNonDouble(dest, dest, JSVAL_TYPE_OBJECT);
  }
  void unboxObject(const BaseIndex& src, Register dest) {
    doBaseIndex(ARMRegister(dest, 64), src, vixl::LDR_x);
    unboxNonDouble(dest, dest, JSVAL_TYPE_OBJECT);
  }

  template <typename T>
  void unboxObjectOrNull(const T& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    And(ARMRegister(dest, 64), ARMRegister(dest, 64),
        Operand(~JS::detail::ValueObjectOrNullBit));
  }

  // See comment in MacroAssembler-x64.h.
  void unboxGCThingForGCBarrier(const Address& src, Register dest) {
    loadPtr(src, dest);
    And(ARMRegister(dest, 64), ARMRegister(dest, 64),
        Operand(JS::detail::ValueGCThingPayloadMask));
  }
  void unboxGCThingForGCBarrier(const ValueOperand& src, Register dest) {
    And(ARMRegister(dest, 64), ARMRegister(src.valueReg(), 64),
        Operand(JS::detail::ValueGCThingPayloadMask));
  }

  // Like unboxGCThingForGCBarrier, but loads the GC thing's chunk base.
  void getGCThingValueChunk(const Address& src, Register dest) {
    loadPtr(src, dest);
    And(ARMRegister(dest, 64), ARMRegister(dest, 64),
        Operand(JS::detail::ValueGCThingPayloadChunkMask));
  }
  void getGCThingValueChunk(const ValueOperand& src, Register dest) {
    And(ARMRegister(dest, 64), ARMRegister(src.valueReg(), 64),
        Operand(JS::detail::ValueGCThingPayloadChunkMask));
  }

  inline void unboxValue(const ValueOperand& src, AnyRegister dest,
                         JSValueType type);

  void unboxString(const ValueOperand& operand, Register dest) {
    unboxNonDouble(operand, dest, JSVAL_TYPE_STRING);
  }
  void unboxString(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
  }
  void unboxSymbol(const ValueOperand& operand, Register dest) {
    unboxNonDouble(operand, dest, JSVAL_TYPE_SYMBOL);
  }
  void unboxSymbol(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
  }
  void unboxBigInt(const ValueOperand& operand, Register dest) {
    unboxNonDouble(operand, dest, JSVAL_TYPE_BIGINT);
  }
  void unboxBigInt(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
  }
  // These two functions use the low 32-bits of the full value register.
  void boolValueToDouble(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToDouble(operand.valueReg(), dest);
  }
  void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToDouble(operand.valueReg(), dest);
  }

  void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToFloat32(operand.valueReg(), dest);
  }
  void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToFloat32(operand.valueReg(), dest);
  }

  void loadConstantDouble(double d, FloatRegister dest) {
    ARMFPRegister r(dest, 64);
    if (d == 0.0) {
      // Clang11 does movi for 0 and movi+fneg for -0, and this seems like a
      // good implementation-independent strategy as it avoids any gpr->fpr
      // moves or memory traffic.
      Movi(r, 0);
      if (std::signbit(d)) {
        Fneg(r, r);
      }
    } else {
      Fmov(r, d);
    }
  }
  void loadConstantFloat32(float f, FloatRegister dest) {
    ARMFPRegister r(dest, 32);
    if (f == 0.0) {
      // See comments above.  There's not a movi variant for a single register,
      // so clear the double.
      Movi(ARMFPRegister(dest, 64), 0);
      if (std::signbit(f)) {
        Fneg(r, r);
      }
    } else {
      Fmov(r, f);
    }
  }

  void cmpTag(Register tag, ImmTag ref) {
    // As opposed to other architecture, splitTag is replaced by splitSignExtTag
    // which extract the tag with a sign extension. The reason being that cmp32
    // with a tag value would be too large to fit as a 12 bits immediate value,
    // and would require the VIXL macro assembler to add an extra instruction
    // and require extra scratch register to load the Tag value.
    //
    // Instead, we compare with the negative value of the sign extended tag with
    // the CMN instruction. The sign extended tag is expected to be a negative
    // value. Therefore the negative of the sign extended tag is expected to be
    // near 0 and fit on 12 bits.
    //
    // Ignoring the sign extension, the logic is the following:
    //
    //   CMP32(Reg, Tag) = Reg - Tag
    //                   = Reg + (-Tag)
    //                   = CMN32(Reg, -Tag)
    //
    // Note: testGCThing, testPrimitive and testNumber which are checking for
    // inequalities should use unsigned comparisons (as done by default) in
    // order to keep the same relation order after the sign extension, i.e.
    // using Above or Below which are based on the carry flag.
    uint32_t hiShift = JSVAL_TAG_SHIFT - 32;
    int32_t seTag = int32_t(ref.value);
    seTag = (seTag << hiShift) >> hiShift;
    MOZ_ASSERT(seTag < 0);
    int32_t negTag = -seTag;
    // Check thest negTag is encoded on a 12 bits immediate value.
    MOZ_ASSERT((negTag & ~0xFFF) == 0);
    cmn32(tag, Imm32(negTag));
  }

  // Register-based tests.
  Condition testUndefined(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_UNDEFINED));
    return cond;
  }
  Condition testInt32(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_INT32));
    return cond;
  }
  Condition testBoolean(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_BOOLEAN));
    return cond;
  }
  Condition testNull(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_NULL));
    return cond;
  }
  Condition testString(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_STRING));
    return cond;
  }
  Condition testSymbol(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_SYMBOL));
    return cond;
  }
  Condition testBigInt(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_BIGINT));
    return cond;
  }
  Condition testObject(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_OBJECT));
    return cond;
  }
  Condition testDouble(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_MAX_DOUBLE));
    // Requires unsigned comparison due to cmpTag internals.
    return (cond == Equal) ? BelowOrEqual : Above;
  }
  Condition testNumber(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JS::detail::ValueUpperInclNumberTag));
    // Requires unsigned comparison due to cmpTag internals.
    return (cond == Equal) ? BelowOrEqual : Above;
  }
  Condition testGCThing(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JS::detail::ValueLowerInclGCThingTag));
    // Requires unsigned comparison due to cmpTag internals.
    return (cond == Equal) ? AboveOrEqual : Below;
  }
  Condition testMagic(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
  }
  Condition testPrimitive(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmpTag(tag, ImmTag(JS::detail::ValueUpperExclPrimitiveTag));
    // Requires unsigned comparison due to cmpTag internals.
    return (cond == Equal) ? Below : AboveOrEqual;
  }
  Condition testError(Condition cond, Register tag) {
    return testMagic(cond, tag);
  }

  // ValueOperand-based tests.
  Condition testInt32(Condition cond, const ValueOperand& value) {
    // The incoming ValueOperand may use scratch registers.
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(scratch != value.valueReg());

    splitSignExtTag(value, scratch);
    return testInt32(cond, scratch);
  }
  Condition testBoolean(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testBoolean(cond, scratch);
  }
  Condition testDouble(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testDouble(cond, scratch);
  }
  Condition testNull(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testNull(cond, scratch);
  }
  Condition testUndefined(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testUndefined(cond, scratch);
  }
  Condition testString(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testString(cond, scratch);
  }
  Condition testSymbol(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testSymbol(cond, scratch);
  }
  Condition testBigInt(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testBigInt(cond, scratch);
  }
  Condition testObject(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testObject(cond, scratch);
  }
  Condition testNumber(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testNumber(cond, scratch);
  }
  Condition testPrimitive(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testPrimitive(cond, scratch);
  }
  Condition testMagic(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testMagic(cond, scratch);
  }
  Condition testGCThing(Condition cond, const ValueOperand& value) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(value.valueReg() != scratch);
    splitSignExtTag(value, scratch);
    return testGCThing(cond, scratch);
  }
  Condition testError(Condition cond, const ValueOperand& value) {
    return testMagic(cond, value);
  }

  // Address-based tests.
  Condition testGCThing(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testGCThing(cond, scratch);
  }
  Condition testMagic(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testMagic(cond, scratch);
  }
  Condition testInt32(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testInt32(cond, scratch);
  }
  Condition testDouble(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testDouble(cond, scratch);
  }
  Condition testBoolean(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testBoolean(cond, scratch);
  }
  Condition testNull(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testNull(cond, scratch);
  }
  Condition testUndefined(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testUndefined(cond, scratch);
  }
  Condition testString(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testString(cond, scratch);
  }
  Condition testSymbol(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testSymbol(cond, scratch);
  }
  Condition testBigInt(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testBigInt(cond, scratch);
  }
  Condition testObject(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testObject(cond, scratch);
  }
  Condition testNumber(Condition cond, const Address& address) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(address.base != scratch);
    splitSignExtTag(address, scratch);
    return testNumber(cond, scratch);
  }

  // BaseIndex-based tests.
  Condition testUndefined(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testUndefined(cond, scratch);
  }
  Condition testNull(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testNull(cond, scratch);
  }
  Condition testBoolean(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testBoolean(cond, scratch);
  }
  Condition testString(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testString(cond, scratch);
  }
  Condition testSymbol(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testSymbol(cond, scratch);
  }
  Condition testBigInt(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testBigInt(cond, scratch);
  }
  Condition testInt32(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testInt32(cond, scratch);
  }
  Condition testObject(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testObject(cond, scratch);
  }
  Condition testDouble(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testDouble(cond, scratch);
  }
  Condition testMagic(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testMagic(cond, scratch);
  }
  Condition testGCThing(Condition cond, const BaseIndex& src) {
    vixl::UseScratchRegisterScope temps(this);
    const Register scratch = temps.AcquireX().asUnsized();
    MOZ_ASSERT(src.base != scratch);
    MOZ_ASSERT(src.index != scratch);
    splitSignExtTag(src, scratch);
    return testGCThing(cond, scratch);
  }

  Condition testInt32Truthy(bool truthy, const ValueOperand& operand) {
    ARMRegister payload32(operand.valueReg(), 32);
    Tst(payload32, payload32);
    return truthy ? NonZero : Zero;
  }

  Condition testBooleanTruthy(bool truthy, const ValueOperand& operand) {
    ARMRegister payload32(operand.valueReg(), 32);
    Tst(payload32, payload32);
    return truthy ? NonZero : Zero;
  }

  Condition testBigIntTruthy(bool truthy, const ValueOperand& value);
  Condition testStringTruthy(bool truthy, const ValueOperand& value);

  void int32OrDouble(Register src, ARMFPRegister dest) {
    Label isInt32;
    Label join;
    testInt32(Equal, ValueOperand(src));
    B(&isInt32, Equal);
    // is double, move the bits as is
    Fmov(dest, ARMRegister(src, 64));
    B(&join);
    bind(&isInt32);
    // is int32, do a conversion while moving
    Scvtf(dest, ARMRegister(src, 32));
    bind(&join);
  }
  void loadUnboxedValue(Address address, MIRType type, AnyRegister dest) {
    if (dest.isFloat()) {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch64 = temps.AcquireX();
      MOZ_ASSERT(scratch64.asUnsized() != address.base);
      Ldr(scratch64, toMemOperand(address));
      int32OrDouble(scratch64.asUnsized(), ARMFPRegister(dest.fpu(), 64));
    } else {
      unboxNonDouble(address, dest.gpr(), ValueTypeFromMIRType(type));
    }
  }

  void loadUnboxedValue(BaseIndex address, MIRType type, AnyRegister dest) {
    if (dest.isFloat()) {
      vixl::UseScratchRegisterScope temps(this);
      const ARMRegister scratch64 = temps.AcquireX();
      MOZ_ASSERT(scratch64.asUnsized() != address.base);
      MOZ_ASSERT(scratch64.asUnsized() != address.index);
      doBaseIndex(scratch64, address, vixl::LDR_x);
      int32OrDouble(scratch64.asUnsized(), ARMFPRegister(dest.fpu(), 64));
    } else {
      unboxNonDouble(address, dest.gpr(), ValueTypeFromMIRType(type));
    }
  }

  // Emit a B that can be toggled to a CMP. See ToggleToJmp(), ToggleToCmp().
  CodeOffset toggledJump(Label* label) {
    BufferOffset offset = b(label, Always);
    CodeOffset ret(offset.getOffset());
    return ret;
  }

  // load: offset to the load instruction obtained by movePatchablePtr().
  void writeDataRelocation(ImmGCPtr ptr, BufferOffset load) {
    // Raw GC pointer relocations and Value relocations both end up in
    // Assembler::TraceDataRelocations.
    if (ptr.value) {
      if (gc::IsInsideNursery(ptr.value)) {
        embedsNurseryPointers_ = true;
      }
      dataRelocations_.writeUnsigned(load.getOffset());
    }
  }
  void writeDataRelocation(const Value& val, BufferOffset load) {
    // Raw GC pointer relocations and Value relocations both end up in
    // Assembler::TraceDataRelocations.
    if (val.isGCThing()) {
      gc::Cell* cell = val.toGCThing();
      if (cell && gc::IsInsideNursery(cell)) {
        embedsNurseryPointers_ = true;
      }
      dataRelocations_.writeUnsigned(load.getOffset());
    }
  }

  void computeEffectiveAddress(const Address& address, Register dest) {
    Add(ARMRegister(dest, 64), toARMRegister(address.base, 64),
        Operand(address.offset));
  }
  void computeEffectiveAddress(const Address& address, RegisterOrSP dest) {
    Add(toARMRegister(dest, 64), toARMRegister(address.base, 64),
        Operand(address.offset));
  }
  void computeEffectiveAddress(const BaseIndex& address, Register dest) {
    ARMRegister dest64(dest, 64);
    ARMRegister base64 = toARMRegister(address.base, 64);
    ARMRegister index64(address.index, 64);

    Add(dest64, base64, Operand(index64, vixl::LSL, address.scale));
    if (address.offset) {
      Add(dest64, dest64, Operand(address.offset));
    }
  }

 public:
  void handleFailureWithHandlerTail(Label* profilerExitTail,
                                    Label* bailoutTail);

  void profilerEnterFrame(Register framePtr, Register scratch);
  void profilerExitFrame();

  void wasmLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase,
                    Register ptr, AnyRegister outany, Register64 out64);
  void wasmLoadImpl(const wasm::MemoryAccessDesc& access, MemOperand srcAddr,
                    AnyRegister outany, Register64 out64);
  void wasmStoreImpl(const wasm::MemoryAccessDesc& access, AnyRegister valany,
                     Register64 val64, Register memoryBase, Register ptr);
  void wasmStoreImpl(const wasm::MemoryAccessDesc& access, MemOperand destAddr,
                     AnyRegister valany, Register64 val64);
  // The complete address is in `address`, and `access` is used for its type
  // attributes only; its `offset` is ignored.
  void wasmLoadAbsolute(const wasm::MemoryAccessDesc& access,
                        Register memoryBase, uint64_t address, AnyRegister out,
                        Register64 out64);
  void wasmStoreAbsolute(const wasm::MemoryAccessDesc& access,
                         AnyRegister value, Register64 value64,
                         Register memoryBase, uint64_t address);

  // Emit a BLR or NOP instruction. ToggleCall can be used to patch
  // this instruction.
  CodeOffset toggledCall(JitCode* target, bool enabled) {
    // The returned offset must be to the first instruction generated,
    // for the debugger to match offset with Baseline's pcMappingEntries_.
    BufferOffset offset = nextOffset();

    // It is unclear why this sync is necessary:
    // * PSP and SP have been observed to be different in testcase
    //   tests/cacheir/bug1448136.js
    // * Removing the sync causes no failures in all of jit-tests.
    syncStackPtr();

    BufferOffset loadOffset;
    {
      vixl::UseScratchRegisterScope temps(this);

      // The register used for the load is hardcoded, so that ToggleCall
      // can patch in the branch instruction easily. This could be changed,
      // but then ToggleCall must read the target register from the load.
      MOZ_ASSERT(temps.IsAvailable(ScratchReg2_64));
      temps.Exclude(ScratchReg2_64);

      loadOffset = immPool64(ScratchReg2_64, uint64_t(target->raw()));

      if (enabled) {
        blr(ScratchReg2_64);
      } else {
        nop();
      }
    }

    addPendingJump(loadOffset, ImmPtr(target->raw()), RelocationKind::JITCODE);
    CodeOffset ret(offset.getOffset());
    return ret;
  }

  static size_t ToggledCallSize(uint8_t* code) {
    // The call site is a sequence of two or three instructions:
    //
    //   syncStack (optional)
    //   ldr/adr
    //   nop/blr
    //
    // Flushed constant pools can appear before any of the instructions.

    const Instruction* cur = (const Instruction*)code;
    cur = cur->skipPool();
    if (cur->IsStackPtrSync()) cur = cur->NextInstruction();
    cur = cur->skipPool();
    cur = cur->NextInstruction();  // LDR/ADR
    cur = cur->skipPool();
    cur = cur->NextInstruction();  // NOP/BLR
    return (uint8_t*)cur - code;
  }

  void checkARMRegAlignment(const ARMRegister& reg) {
#ifdef DEBUG
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch64 = temps.AcquireX();
    MOZ_ASSERT_IF(!reg.IsSP(), scratch64.asUnsized() != reg.asUnsized());
    Label aligned;
    Mov(scratch64, reg);
    Tst(scratch64, Operand(StackAlignment - 1));
    B(Zero, &aligned);
    breakpoint();
    bind(&aligned);
    Mov(scratch64, vixl::xzr);  // Clear the scratch register for sanity.
#endif
  }

  void checkStackAlignment() {
#ifdef DEBUG
    checkARMRegAlignment(GetStackPointer64());

    // If another register is being used to track pushes, check sp explicitly.
    if (!GetStackPointer64().Is(vixl::sp)) {
      checkARMRegAlignment(vixl::sp);
    }
#endif
  }

  void abiret() {
    syncStackPtr();  // SP is always used to transmit the stack between calls.
    vixl::MacroAssembler::Ret(vixl::lr);
  }

  void incrementInt32Value(const Address& addr) {
    vixl::UseScratchRegisterScope temps(this);
    const ARMRegister scratch32 = temps.AcquireW();
    MOZ_ASSERT(scratch32.asUnsized() != addr.base);

    load32(addr, scratch32.asUnsized());
    Add(scratch32, scratch32, Operand(1));
    store32(scratch32.asUnsized(), addr);
  }

  void breakpoint();

  // Emits a simulator directive to save the current sp on an internal stack.
  void simulatorMarkSP() {
#ifdef JS_SIMULATOR_ARM64
    svc(vixl::kMarkStackPointer);
#endif
  }

  // Emits a simulator directive to pop from its internal stack
  // and assert that the value is equal to the current sp.
  void simulatorCheckSP() {
#ifdef JS_SIMULATOR_ARM64
    svc(vixl::kCheckStackPointer);
#endif
  }

 protected:
  bool buildOOLFakeExitFrame(void* fakeReturnAddr);
};

// See documentation for ScratchTagScope and ScratchTagScopeRelease in
// MacroAssembler-x64.h.

class ScratchTagScope {
  vixl::UseScratchRegisterScope temps_;
  ARMRegister scratch64_;
  bool owned_;
  mozilla::DebugOnly<bool> released_;

 public:
  ScratchTagScope(MacroAssemblerCompat& masm, const ValueOperand&)
      : temps_(&masm), owned_(true), released_(false) {
    scratch64_ = temps_.AcquireX();
  }

  operator Register() {
    MOZ_ASSERT(!released_);
    return scratch64_.asUnsized();
  }

  void release() {
    MOZ_ASSERT(!released_);
    released_ = true;
    if (owned_) {
      temps_.Release(scratch64_);
      owned_ = false;
    }
  }

  void reacquire() {
    MOZ_ASSERT(released_);
    released_ = false;
  }
};

class ScratchTagScopeRelease {
  ScratchTagScope* ts_;

 public:
  explicit ScratchTagScopeRelease(ScratchTagScope* ts) : ts_(ts) {
    ts_->release();
  }
  ~ScratchTagScopeRelease() { ts_->reacquire(); }
};

inline void MacroAssemblerCompat::splitTagForTest(const ValueOperand& value,
                                                  ScratchTagScope& tag) {
  splitSignExtTag(value, tag);
}

typedef MacroAssemblerCompat MacroAssemblerSpecific;

}  // namespace jit
}  // namespace js

#endif  // jit_arm64_MacroAssembler_arm64_h
