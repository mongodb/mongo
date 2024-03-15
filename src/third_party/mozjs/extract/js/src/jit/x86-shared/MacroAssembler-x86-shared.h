/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_MacroAssembler_x86_shared_h
#define jit_x86_shared_MacroAssembler_x86_shared_h

#if defined(JS_CODEGEN_X86)
#  include "jit/x86/Assembler-x86.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/Assembler-x64.h"
#endif

namespace js {
namespace jit {

class MacroAssembler;

class MacroAssemblerX86Shared : public Assembler {
 private:
  // Perform a downcast. Should be removed by Bug 996602.
  MacroAssembler& asMasm();
  const MacroAssembler& asMasm() const;

 public:
#ifdef JS_CODEGEN_X64
  typedef X86Encoding::JmpSrc UsesItem;
#else
  typedef CodeOffset UsesItem;
#endif

  typedef Vector<UsesItem, 0, SystemAllocPolicy> UsesVector;
  static_assert(sizeof(UsesItem) == 4);

 protected:
  // For Double, Float and SimdData, make the move ctors explicit so that MSVC
  // knows what to use instead of copying these data structures.
  template <class T>
  struct Constant {
    using Pod = T;

    T value;
    UsesVector uses;

    explicit Constant(const T& value) : value(value) {}
    Constant(Constant<T>&& other)
        : value(other.value), uses(std::move(other.uses)) {}
    explicit Constant(const Constant<T>&) = delete;
  };

  // Containers use SystemAllocPolicy since wasm releases memory after each
  // function is compiled, and these need to live until after all functions
  // are compiled.
  using Double = Constant<double>;
  Vector<Double, 0, SystemAllocPolicy> doubles_;
  typedef HashMap<double, size_t, DefaultHasher<double>, SystemAllocPolicy>
      DoubleMap;
  DoubleMap doubleMap_;

  using Float = Constant<float>;
  Vector<Float, 0, SystemAllocPolicy> floats_;
  typedef HashMap<float, size_t, DefaultHasher<float>, SystemAllocPolicy>
      FloatMap;
  FloatMap floatMap_;

  struct SimdData : public Constant<SimdConstant> {
    explicit SimdData(SimdConstant d) : Constant<SimdConstant>(d) {}
    SimdData(SimdData&& d) : Constant<SimdConstant>(std::move(d)) {}
    explicit SimdData(const SimdData&) = delete;
    SimdConstant::Type type() const { return value.type(); }
  };

  Vector<SimdData, 0, SystemAllocPolicy> simds_;
  typedef HashMap<SimdConstant, size_t, SimdConstant, SystemAllocPolicy>
      SimdMap;
  SimdMap simdMap_;

  template <class T, class Map>
  T* getConstant(const typename T::Pod& value, Map& map,
                 Vector<T, 0, SystemAllocPolicy>& vec);

  Float* getFloat(float f);
  Double* getDouble(double d);
  SimdData* getSimdData(const SimdConstant& v);

 public:
  using Assembler::call;

  MacroAssemblerX86Shared() = default;

  bool appendRawCode(const uint8_t* code, size_t numBytes) {
    return masm.appendRawCode(code, numBytes);
  }

  void addToPCRel4(uint32_t offset, int32_t bias) {
    return masm.addToPCRel4(offset, bias);
  }

  // Evaluate srcDest = minmax<isMax>{Float32,Double}(srcDest, second).
  // Checks for NaN if canBeNaN is true.
  void minMaxDouble(FloatRegister srcDest, FloatRegister second, bool canBeNaN,
                    bool isMax);
  void minMaxFloat32(FloatRegister srcDest, FloatRegister second, bool canBeNaN,
                     bool isMax);

  void compareDouble(DoubleCondition cond, FloatRegister lhs,
                     FloatRegister rhs) {
    if (cond & DoubleConditionBitInvert) {
      vucomisd(lhs, rhs);
    } else {
      vucomisd(rhs, lhs);
    }
  }

  void compareFloat(DoubleCondition cond, FloatRegister lhs,
                    FloatRegister rhs) {
    if (cond & DoubleConditionBitInvert) {
      vucomiss(lhs, rhs);
    } else {
      vucomiss(rhs, lhs);
    }
  }

  void branchNegativeZero(FloatRegister reg, Register scratch, Label* label,
                          bool maybeNonZero = true);
  void branchNegativeZeroFloat32(FloatRegister reg, Register scratch,
                                 Label* label);

  void move32(Imm32 imm, Register dest) {
    // Use the ImmWord version of mov to register, which has special
    // optimizations. Casting to uint32_t here ensures that the value
    // is zero-extended.
    mov(ImmWord(uint32_t(imm.value)), dest);
  }
  void move32(Imm32 imm, const Operand& dest) { movl(imm, dest); }
  void move32(Register src, Register dest) { movl(src, dest); }
  void move32(Register src, const Operand& dest) { movl(src, dest); }
  void test32(Register lhs, Register rhs) { testl(rhs, lhs); }
  void test32(const Address& addr, Imm32 imm) { testl(imm, Operand(addr)); }
  void test32(const Operand lhs, Imm32 imm) { testl(imm, lhs); }
  void test32(Register lhs, Imm32 rhs) { testl(rhs, lhs); }
  void cmp32(Register lhs, Imm32 rhs) { cmpl(rhs, lhs); }
  void cmp32(Register lhs, Register rhs) { cmpl(rhs, lhs); }
  void cmp32(const Address& lhs, Register rhs) { cmp32(Operand(lhs), rhs); }
  void cmp32(const Address& lhs, Imm32 rhs) { cmp32(Operand(lhs), rhs); }
  void cmp32(const Operand& lhs, Imm32 rhs) { cmpl(rhs, lhs); }
  void cmp32(const Operand& lhs, Register rhs) { cmpl(rhs, lhs); }
  void cmp32(Register lhs, const Operand& rhs) { cmpl(rhs, lhs); }

  void cmp16(const Address& lhs, Imm32 rhs) { cmp16(Operand(lhs), rhs); }
  void cmp16(const Operand& lhs, Imm32 rhs) { cmpw(rhs, lhs); }

  void cmp8(const Address& lhs, Imm32 rhs) { cmp8(Operand(lhs), rhs); }
  void cmp8(const Operand& lhs, Imm32 rhs) { cmpb(rhs, lhs); }
  void cmp8(const Operand& lhs, Register rhs) { cmpb(rhs, lhs); }

  void atomic_inc32(const Operand& addr) { lock_incl(addr); }
  void atomic_dec32(const Operand& addr) { lock_decl(addr); }

  void storeLoadFence() {
    // This implementation follows Linux.
    if (HasSSE2()) {
      masm.mfence();
    } else {
      lock_addl(Imm32(0), Operand(Address(esp, 0)));
    }
  }

  void branch16(Condition cond, Register lhs, Register rhs, Label* label) {
    cmpw(rhs, lhs);
    j(cond, label);
  }
  void branchTest16(Condition cond, Register lhs, Register rhs, Label* label) {
    testw(rhs, lhs);
    j(cond, label);
  }

  void jump(Label* label) { jmp(label); }
  void jump(JitCode* code) { jmp(code); }
  void jump(TrampolinePtr code) { jmp(ImmPtr(code.value)); }
  void jump(ImmPtr ptr) { jmp(ptr); }
  void jump(Register reg) { jmp(Operand(reg)); }
  void jump(const Address& addr) { jmp(Operand(addr)); }

  void convertInt32ToDouble(Register src, FloatRegister dest) {
    // vcvtsi2sd and friends write only part of their output register, which
    // causes slowdowns on out-of-order processors. Explicitly break
    // dependencies with vxorpd (and vxorps elsewhere), which are handled
    // specially in modern CPUs, for this purpose. See sections 8.14, 9.8,
    // 10.8, 12.9, 13.16, 14.14, and 15.8 of Agner's Microarchitecture
    // document.
    zeroDouble(dest);
    vcvtsi2sd(src, dest, dest);
  }
  void convertInt32ToDouble(const Address& src, FloatRegister dest) {
    convertInt32ToDouble(Operand(src), dest);
  }
  void convertInt32ToDouble(const BaseIndex& src, FloatRegister dest) {
    convertInt32ToDouble(Operand(src), dest);
  }
  void convertInt32ToDouble(const Operand& src, FloatRegister dest) {
    // Clear the output register first to break dependencies; see above;
    zeroDouble(dest);
    vcvtsi2sd(Operand(src), dest, dest);
  }
  void convertInt32ToFloat32(Register src, FloatRegister dest) {
    // Clear the output register first to break dependencies; see above;
    zeroFloat32(dest);
    vcvtsi2ss(src, dest, dest);
  }
  void convertInt32ToFloat32(const Address& src, FloatRegister dest) {
    convertInt32ToFloat32(Operand(src), dest);
  }
  void convertInt32ToFloat32(const Operand& src, FloatRegister dest) {
    // Clear the output register first to break dependencies; see above;
    zeroFloat32(dest);
    vcvtsi2ss(src, dest, dest);
  }
  Condition testDoubleTruthy(bool truthy, FloatRegister reg) {
    ScratchDoubleScope scratch(asMasm());
    zeroDouble(scratch);
    vucomisd(reg, scratch);
    return truthy ? NonZero : Zero;
  }

  // Class which ensures that registers used in byte ops are compatible with
  // such instructions, even if the original register passed in wasn't. This
  // only applies to x86, as on x64 all registers are valid single byte regs.
  // This doesn't lead to great code but helps to simplify code generation.
  //
  // Note that this can currently only be used in cases where the register is
  // read from by the guarded instruction, not written to.
  class AutoEnsureByteRegister {
    MacroAssemblerX86Shared* masm;
    Register original_;
    Register substitute_;

   public:
    template <typename T>
    AutoEnsureByteRegister(MacroAssemblerX86Shared* masm, T address,
                           Register reg)
        : masm(masm), original_(reg) {
      AllocatableGeneralRegisterSet singleByteRegs(Registers::SingleByteRegs);
      if (singleByteRegs.has(reg)) {
        substitute_ = reg;
      } else {
        MOZ_ASSERT(address.base != StackPointer);
        do {
          substitute_ = singleByteRegs.takeAny();
        } while (Operand(address).containsReg(substitute_));

        masm->push(substitute_);
        masm->mov(reg, substitute_);
      }
    }

    ~AutoEnsureByteRegister() {
      if (original_ != substitute_) {
        masm->pop(substitute_);
      }
    }

    Register reg() { return substitute_; }
  };

  void load8ZeroExtend(const Operand& src, Register dest) { movzbl(src, dest); }
  void load8ZeroExtend(const Address& src, Register dest) {
    movzbl(Operand(src), dest);
  }
  void load8ZeroExtend(const BaseIndex& src, Register dest) {
    movzbl(Operand(src), dest);
  }
  void load8SignExtend(const Operand& src, Register dest) { movsbl(src, dest); }
  void load8SignExtend(const Address& src, Register dest) {
    movsbl(Operand(src), dest);
  }
  void load8SignExtend(const BaseIndex& src, Register dest) {
    movsbl(Operand(src), dest);
  }
  template <typename T>
  void store8(Imm32 src, const T& dest) {
    movb(src, Operand(dest));
  }
  template <typename T>
  void store8(Register src, const T& dest) {
    AutoEnsureByteRegister ensure(this, dest, src);
    movb(ensure.reg(), Operand(dest));
  }
  void load16ZeroExtend(const Operand& src, Register dest) {
    movzwl(src, dest);
  }
  void load16ZeroExtend(const Address& src, Register dest) {
    movzwl(Operand(src), dest);
  }
  void load16ZeroExtend(const BaseIndex& src, Register dest) {
    movzwl(Operand(src), dest);
  }
  template <typename S>
  void load16UnalignedZeroExtend(const S& src, Register dest) {
    load16ZeroExtend(src, dest);
  }
  template <typename S, typename T>
  void store16(const S& src, const T& dest) {
    movw(src, Operand(dest));
  }
  template <typename S, typename T>
  void store16Unaligned(const S& src, const T& dest) {
    store16(src, dest);
  }
  void load16SignExtend(const Operand& src, Register dest) {
    movswl(src, dest);
  }
  void load16SignExtend(const Address& src, Register dest) {
    movswl(Operand(src), dest);
  }
  void load16SignExtend(const BaseIndex& src, Register dest) {
    movswl(Operand(src), dest);
  }
  template <typename S>
  void load16UnalignedSignExtend(const S& src, Register dest) {
    load16SignExtend(src, dest);
  }
  void load32(const Address& address, Register dest) {
    movl(Operand(address), dest);
  }
  void load32(const BaseIndex& src, Register dest) { movl(Operand(src), dest); }
  void load32(const Operand& src, Register dest) { movl(src, dest); }
  template <typename S>
  void load32Unaligned(const S& src, Register dest) {
    load32(src, dest);
  }
  template <typename S, typename T>
  void store32(const S& src, const T& dest) {
    movl(src, Operand(dest));
  }
  template <typename S, typename T>
  void store32Unaligned(const S& src, const T& dest) {
    store32(src, dest);
  }
  void loadDouble(const Address& src, FloatRegister dest) { vmovsd(src, dest); }
  void loadDouble(const BaseIndex& src, FloatRegister dest) {
    vmovsd(src, dest);
  }
  void loadDouble(const Operand& src, FloatRegister dest) {
    switch (src.kind()) {
      case Operand::MEM_REG_DISP:
        loadDouble(src.toAddress(), dest);
        break;
      case Operand::MEM_SCALE:
        loadDouble(src.toBaseIndex(), dest);
        break;
      default:
        MOZ_CRASH("unexpected operand kind");
    }
  }
  void moveDouble(FloatRegister src, FloatRegister dest) {
    // Use vmovapd instead of vmovsd to avoid dependencies.
    vmovapd(src, dest);
  }
  void zeroDouble(FloatRegister reg) { vxorpd(reg, reg, reg); }
  void zeroFloat32(FloatRegister reg) { vxorps(reg, reg, reg); }
  void convertFloat32ToDouble(FloatRegister src, FloatRegister dest) {
    vcvtss2sd(src, dest, dest);
  }
  void convertDoubleToFloat32(FloatRegister src, FloatRegister dest) {
    vcvtsd2ss(src, dest, dest);
  }

  void loadInt32x4(const Address& addr, FloatRegister dest) {
    vmovdqa(Operand(addr), dest);
  }
  void loadFloat32x4(const Address& addr, FloatRegister dest) {
    vmovaps(Operand(addr), dest);
  }
  void storeInt32x4(FloatRegister src, const Address& addr) {
    vmovdqa(src, Operand(addr));
  }
  void storeFloat32x4(FloatRegister src, const Address& addr) {
    vmovaps(src, Operand(addr));
  }

  void convertFloat32x4ToInt32x4(FloatRegister src, FloatRegister dest) {
    // Note that if the conversion failed (because the converted
    // result is larger than the maximum signed int32, or less than the
    // least signed int32, or NaN), this will return the undefined integer
    // value (0x8000000).
    vcvttps2dq(src, dest);
  }
  void convertInt32x4ToFloat32x4(FloatRegister src, FloatRegister dest) {
    vcvtdq2ps(src, dest);
  }

  void binarySimd128(const SimdConstant& rhs, FloatRegister lhsDest,
                     void (MacroAssembler::*regOp)(const Operand&,
                                                   FloatRegister,
                                                   FloatRegister),
                     void (MacroAssembler::*constOp)(const SimdConstant&,
                                                     FloatRegister));
  void binarySimd128(
      FloatRegister lhs, const SimdConstant& rhs, FloatRegister dest,
      void (MacroAssembler::*regOp)(const Operand&, FloatRegister,
                                    FloatRegister),
      void (MacroAssembler::*constOp)(const SimdConstant&, FloatRegister,
                                      FloatRegister));
  void binarySimd128(const SimdConstant& rhs, FloatRegister lhsDest,
                     void (MacroAssembler::*regOp)(const Operand&,
                                                   FloatRegister),
                     void (MacroAssembler::*constOp)(const SimdConstant&,
                                                     FloatRegister));

  // SIMD methods, defined in MacroAssembler-x86-shared-SIMD.cpp.

  void unsignedConvertInt32x4ToFloat32x4(FloatRegister src, FloatRegister dest);
  void unsignedConvertInt32x4ToFloat64x2(FloatRegister src, FloatRegister dest);
  void bitwiseTestSimd128(const SimdConstant& rhs, FloatRegister lhs);

  void truncSatFloat32x4ToInt32x4(FloatRegister src, FloatRegister dest);
  void unsignedTruncSatFloat32x4ToInt32x4(FloatRegister src, FloatRegister temp,
                                          FloatRegister dest);
  void unsignedTruncFloat32x4ToInt32x4Relaxed(FloatRegister src,
                                              FloatRegister dest);
  void truncSatFloat64x2ToInt32x4(FloatRegister src, FloatRegister temp,
                                  FloatRegister dest);
  void unsignedTruncSatFloat64x2ToInt32x4(FloatRegister src, FloatRegister temp,
                                          FloatRegister dest);
  void unsignedTruncFloat64x2ToInt32x4Relaxed(FloatRegister src,
                                              FloatRegister dest);

  void splatX16(Register input, FloatRegister output);
  void splatX8(Register input, FloatRegister output);
  void splatX4(Register input, FloatRegister output);
  void splatX4(FloatRegister input, FloatRegister output);
  void splatX2(FloatRegister input, FloatRegister output);

  void extractLaneInt32x4(FloatRegister input, Register output, unsigned lane);
  void extractLaneFloat32x4(FloatRegister input, FloatRegister output,
                            unsigned lane);
  void extractLaneFloat64x2(FloatRegister input, FloatRegister output,
                            unsigned lane);
  void extractLaneInt16x8(FloatRegister input, Register output, unsigned lane,
                          SimdSign sign);
  void extractLaneInt8x16(FloatRegister input, Register output, unsigned lane,
                          SimdSign sign);

  void replaceLaneFloat32x4(unsigned lane, FloatRegister lhs, FloatRegister rhs,
                            FloatRegister dest);
  void replaceLaneFloat64x2(unsigned lane, FloatRegister lhs, FloatRegister rhs,
                            FloatRegister dest);

  void shuffleInt8x16(FloatRegister lhs, FloatRegister rhs,
                      FloatRegister output, const uint8_t lanes[16]);
  void blendInt8x16(FloatRegister lhs, FloatRegister rhs, FloatRegister output,
                    FloatRegister temp, const uint8_t lanes[16]);
  void blendInt16x8(FloatRegister lhs, FloatRegister rhs, FloatRegister output,
                    const uint16_t lanes[8]);
  void laneSelectSimd128(FloatRegister mask, FloatRegister lhs,
                         FloatRegister rhs, FloatRegister output);

  void compareInt8x16(FloatRegister lhs, Operand rhs, Assembler::Condition cond,
                      FloatRegister output);
  void compareInt8x16(Assembler::Condition cond, FloatRegister lhs,
                      const SimdConstant& rhs, FloatRegister dest);
  void compareInt16x8(FloatRegister lhs, Operand rhs, Assembler::Condition cond,
                      FloatRegister output);
  void compareInt16x8(Assembler::Condition cond, FloatRegister lhs,
                      const SimdConstant& rhs, FloatRegister dest);
  void compareInt32x4(FloatRegister lhs, Operand rhs, Assembler::Condition cond,
                      FloatRegister output);
  void compareInt32x4(Assembler::Condition cond, FloatRegister lhs,
                      const SimdConstant& rhs, FloatRegister dest);
  void compareForEqualityInt64x2(FloatRegister lhs, Operand rhs,
                                 Assembler::Condition cond,
                                 FloatRegister output);
  void compareForOrderingInt64x2(FloatRegister lhs, Operand rhs,
                                 Assembler::Condition cond, FloatRegister temp1,
                                 FloatRegister temp2, FloatRegister output);
  void compareForOrderingInt64x2AVX(FloatRegister lhs, FloatRegister rhs,
                                    Assembler::Condition cond,
                                    FloatRegister output);
  void compareFloat32x4(FloatRegister lhs, Operand rhs,
                        Assembler::Condition cond, FloatRegister output);
  void compareFloat32x4(Assembler::Condition cond, FloatRegister lhs,
                        const SimdConstant& rhs, FloatRegister dest);
  void compareFloat64x2(FloatRegister lhs, Operand rhs,
                        Assembler::Condition cond, FloatRegister output);
  void compareFloat64x2(Assembler::Condition cond, FloatRegister lhs,
                        const SimdConstant& rhs, FloatRegister dest);

  void minMaxFloat32x4(bool isMin, FloatRegister lhs, Operand rhs,
                       FloatRegister temp1, FloatRegister temp2,
                       FloatRegister output);
  void minMaxFloat32x4AVX(bool isMin, FloatRegister lhs, FloatRegister rhs,
                          FloatRegister temp1, FloatRegister temp2,
                          FloatRegister output);
  void minMaxFloat64x2(bool isMin, FloatRegister lhs, Operand rhs,
                       FloatRegister temp1, FloatRegister temp2,
                       FloatRegister output);
  void minMaxFloat64x2AVX(bool isMin, FloatRegister lhs, FloatRegister rhs,
                          FloatRegister temp1, FloatRegister temp2,
                          FloatRegister output);
  void minFloat32x4(FloatRegister lhs, FloatRegister rhs, FloatRegister temp1,
                    FloatRegister temp2, FloatRegister output);
  void maxFloat32x4(FloatRegister lhs, FloatRegister rhs, FloatRegister temp1,
                    FloatRegister temp2, FloatRegister output);

  void minFloat64x2(FloatRegister lhs, FloatRegister rhs, FloatRegister temp1,
                    FloatRegister temp2, FloatRegister output);
  void maxFloat64x2(FloatRegister lhs, FloatRegister rhs, FloatRegister temp1,
                    FloatRegister temp2, FloatRegister output);

  void packedShiftByScalarInt8x16(
      FloatRegister in, Register count, FloatRegister xtmp, FloatRegister dest,
      void (MacroAssemblerX86Shared::*shift)(FloatRegister, FloatRegister,
                                             FloatRegister),
      void (MacroAssemblerX86Shared::*extend)(const Operand&, FloatRegister));

  void packedLeftShiftByScalarInt8x16(FloatRegister in, Register count,
                                      FloatRegister xtmp, FloatRegister dest);
  void packedLeftShiftByScalarInt8x16(Imm32 count, FloatRegister src,
                                      FloatRegister dest);
  void packedRightShiftByScalarInt8x16(FloatRegister in, Register count,
                                       FloatRegister xtmp, FloatRegister dest);
  void packedRightShiftByScalarInt8x16(Imm32 count, FloatRegister src,
                                       FloatRegister dest);
  void packedUnsignedRightShiftByScalarInt8x16(FloatRegister in, Register count,
                                               FloatRegister xtmp,
                                               FloatRegister dest);
  void packedUnsignedRightShiftByScalarInt8x16(Imm32 count, FloatRegister src,
                                               FloatRegister dest);

  void packedLeftShiftByScalarInt16x8(FloatRegister in, Register count,
                                      FloatRegister dest);
  void packedRightShiftByScalarInt16x8(FloatRegister in, Register count,
                                       FloatRegister dest);
  void packedUnsignedRightShiftByScalarInt16x8(FloatRegister in, Register count,
                                               FloatRegister dest);

  void packedLeftShiftByScalarInt32x4(FloatRegister in, Register count,
                                      FloatRegister dest);
  void packedRightShiftByScalarInt32x4(FloatRegister in, Register count,
                                       FloatRegister dest);
  void packedUnsignedRightShiftByScalarInt32x4(FloatRegister in, Register count,
                                               FloatRegister dest);
  void packedLeftShiftByScalarInt64x2(FloatRegister in, Register count,
                                      FloatRegister dest);
  void packedRightShiftByScalarInt64x2(FloatRegister in, Register count,
                                       FloatRegister temp, FloatRegister dest);
  void packedRightShiftByScalarInt64x2(Imm32 count, FloatRegister src,
                                       FloatRegister dest);
  void packedUnsignedRightShiftByScalarInt64x2(FloatRegister in, Register count,
                                               FloatRegister dest);
  void selectSimd128(FloatRegister mask, FloatRegister onTrue,
                     FloatRegister onFalse, FloatRegister temp,
                     FloatRegister output);
  void popcntInt8x16(FloatRegister src, FloatRegister temp,
                     FloatRegister output);

  // SIMD inline methods private to the implementation, that appear to be used.

  template <class T, class Reg>
  inline void loadScalar(const Operand& src, Reg dest);
  template <class T, class Reg>
  inline void storeScalar(Reg src, const Address& dest);
  template <class T>
  inline void loadAlignedVector(const Address& src, FloatRegister dest);
  template <class T>
  inline void storeAlignedVector(FloatRegister src, const Address& dest);

  void loadAlignedSimd128Int(const Address& src, FloatRegister dest) {
    vmovdqa(Operand(src), dest);
  }
  void loadAlignedSimd128Int(const Operand& src, FloatRegister dest) {
    vmovdqa(src, dest);
  }
  void storeAlignedSimd128Int(FloatRegister src, const Address& dest) {
    vmovdqa(src, Operand(dest));
  }
  void moveSimd128Int(FloatRegister src, FloatRegister dest) {
    if (src != dest) {
      vmovdqa(src, dest);
    }
  }
  FloatRegister moveSimd128IntIfNotAVX(FloatRegister src, FloatRegister dest) {
    MOZ_ASSERT(src.isSimd128() && dest.isSimd128());
    if (HasAVX()) {
      return src;
    }
    moveSimd128Int(src, dest);
    return dest;
  }
  FloatRegister selectDestIfAVX(FloatRegister src, FloatRegister dest) {
    MOZ_ASSERT(src.isSimd128() && dest.isSimd128());
    return HasAVX() ? dest : src;
  }
  void loadUnalignedSimd128Int(const Address& src, FloatRegister dest) {
    vmovdqu(Operand(src), dest);
  }
  void loadUnalignedSimd128Int(const BaseIndex& src, FloatRegister dest) {
    vmovdqu(Operand(src), dest);
  }
  void loadUnalignedSimd128Int(const Operand& src, FloatRegister dest) {
    vmovdqu(src, dest);
  }
  void storeUnalignedSimd128Int(FloatRegister src, const Address& dest) {
    vmovdqu(src, Operand(dest));
  }
  void storeUnalignedSimd128Int(FloatRegister src, const BaseIndex& dest) {
    vmovdqu(src, Operand(dest));
  }
  void storeUnalignedSimd128Int(FloatRegister src, const Operand& dest) {
    vmovdqu(src, dest);
  }
  void packedLeftShiftByScalarInt16x8(Imm32 count, FloatRegister dest) {
    count.value &= 15;
    vpsllw(count, dest, dest);
  }
  void packedRightShiftByScalarInt16x8(Imm32 count, FloatRegister dest) {
    count.value &= 15;
    vpsraw(count, dest, dest);
  }
  void packedUnsignedRightShiftByScalarInt16x8(Imm32 count,
                                               FloatRegister dest) {
    count.value &= 15;
    vpsrlw(count, dest, dest);
  }
  void packedLeftShiftByScalarInt32x4(Imm32 count, FloatRegister dest) {
    count.value &= 31;
    vpslld(count, dest, dest);
  }
  void packedRightShiftByScalarInt32x4(Imm32 count, FloatRegister dest) {
    count.value &= 31;
    vpsrad(count, dest, dest);
  }
  void packedUnsignedRightShiftByScalarInt32x4(Imm32 count,
                                               FloatRegister dest) {
    count.value &= 31;
    vpsrld(count, dest, dest);
  }
  void loadAlignedSimd128Float(const Address& src, FloatRegister dest) {
    vmovaps(Operand(src), dest);
  }
  void loadAlignedSimd128Float(const Operand& src, FloatRegister dest) {
    vmovaps(src, dest);
  }
  void storeAlignedSimd128Float(FloatRegister src, const Address& dest) {
    vmovaps(src, Operand(dest));
  }
  void moveSimd128Float(FloatRegister src, FloatRegister dest) {
    if (src != dest) {
      vmovaps(src, dest);
    }
  }
  FloatRegister moveSimd128FloatIfNotAVX(FloatRegister src,
                                         FloatRegister dest) {
    MOZ_ASSERT(src.isSimd128() && dest.isSimd128());
    if (HasAVX()) {
      return src;
    }
    moveSimd128Float(src, dest);
    return dest;
  }
  FloatRegister moveSimd128FloatIfEqual(FloatRegister src, FloatRegister dest,
                                        FloatRegister other) {
    MOZ_ASSERT(src.isSimd128() && dest.isSimd128());
    if (src != other) {
      return src;
    }
    moveSimd128Float(src, dest);
    return dest;
  }
  FloatRegister moveSimd128FloatIfNotAVXOrOther(FloatRegister src,
                                                FloatRegister dest,
                                                FloatRegister other) {
    MOZ_ASSERT(src.isSimd128() && dest.isSimd128());
    if (HasAVX() && src != other) {
      return src;
    }
    moveSimd128Float(src, dest);
    return dest;
  }

  void loadUnalignedSimd128(const Operand& src, FloatRegister dest) {
    vmovups(src, dest);
  }
  void storeUnalignedSimd128(FloatRegister src, const Operand& dest) {
    vmovups(src, dest);
  }

  static uint32_t ComputeShuffleMask(uint32_t x = 0, uint32_t y = 1,
                                     uint32_t z = 2, uint32_t w = 3) {
    MOZ_ASSERT(x < 4 && y < 4 && z < 4 && w < 4);
    uint32_t r = (w << 6) | (z << 4) | (y << 2) | (x << 0);
    MOZ_ASSERT(r < 256);
    return r;
  }

  void shuffleInt32(uint32_t mask, FloatRegister src, FloatRegister dest) {
    vpshufd(mask, src, dest);
  }
  void moveLowInt32(FloatRegister src, Register dest) { vmovd(src, dest); }

  void moveHighPairToLowPairFloat32(FloatRegister src, FloatRegister dest) {
    vmovhlps(src, dest, dest);
  }
  void moveFloatAsDouble(Register src, FloatRegister dest) {
    vmovd(src, dest);
    vcvtss2sd(dest, dest, dest);
  }
  void loadFloatAsDouble(const Address& src, FloatRegister dest) {
    vmovss(src, dest);
    vcvtss2sd(dest, dest, dest);
  }
  void loadFloatAsDouble(const BaseIndex& src, FloatRegister dest) {
    vmovss(src, dest);
    vcvtss2sd(dest, dest, dest);
  }
  void loadFloatAsDouble(const Operand& src, FloatRegister dest) {
    loadFloat32(src, dest);
    vcvtss2sd(dest, dest, dest);
  }
  void loadFloat32(const Address& src, FloatRegister dest) {
    vmovss(src, dest);
  }
  void loadFloat32(const BaseIndex& src, FloatRegister dest) {
    vmovss(src, dest);
  }
  void loadFloat32(const Operand& src, FloatRegister dest) {
    switch (src.kind()) {
      case Operand::MEM_REG_DISP:
        loadFloat32(src.toAddress(), dest);
        break;
      case Operand::MEM_SCALE:
        loadFloat32(src.toBaseIndex(), dest);
        break;
      default:
        MOZ_CRASH("unexpected operand kind");
    }
  }
  void moveFloat32(FloatRegister src, FloatRegister dest) {
    // Use vmovaps instead of vmovss to avoid dependencies.
    vmovaps(src, dest);
  }

  // Checks whether a double is representable as a 32-bit integer. If so, the
  // integer is written to the output register. Otherwise, a bailout is taken to
  // the given snapshot. This function overwrites the scratch float register.
  void convertDoubleToInt32(FloatRegister src, Register dest, Label* fail,
                            bool negativeZeroCheck = true) {
    // Check for -0.0
    if (negativeZeroCheck) {
      branchNegativeZero(src, dest, fail);
    }

    ScratchDoubleScope scratch(asMasm());
    vcvttsd2si(src, dest);
    convertInt32ToDouble(dest, scratch);
    vucomisd(scratch, src);
    j(Assembler::Parity, fail);
    j(Assembler::NotEqual, fail);
  }

  // Checks whether a float32 is representable as a 32-bit integer. If so, the
  // integer is written to the output register. Otherwise, a bailout is taken to
  // the given snapshot. This function overwrites the scratch float register.
  void convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail,
                             bool negativeZeroCheck = true) {
    // Check for -0.0
    if (negativeZeroCheck) {
      branchNegativeZeroFloat32(src, dest, fail);
    }

    ScratchFloat32Scope scratch(asMasm());
    vcvttss2si(src, dest);
    convertInt32ToFloat32(dest, scratch);
    vucomiss(scratch, src);
    j(Assembler::Parity, fail);
    j(Assembler::NotEqual, fail);
  }

  void truncateDoubleToInt32(FloatRegister src, Register dest, Label* fail) {
    // vcvttsd2si returns 0x80000000 on failure. Test for it by
    // subtracting 1 and testing overflow. The other possibility is to test
    // equality for INT_MIN after a comparison, but 1 costs fewer bytes to
    // materialize.
    vcvttsd2si(src, dest);
    cmp32(dest, Imm32(1));
    j(Assembler::Overflow, fail);
  }
  void truncateFloat32ToInt32(FloatRegister src, Register dest, Label* fail) {
    // Same trick as explained in the above comment.
    vcvttss2si(src, dest);
    cmp32(dest, Imm32(1));
    j(Assembler::Overflow, fail);
  }

  inline void clampIntToUint8(Register reg);

  bool maybeInlineDouble(double d, FloatRegister dest) {
    // Loading zero with xor is specially optimized in hardware.
    if (mozilla::IsPositiveZero(d)) {
      zeroDouble(dest);
      return true;
    }

    // It is also possible to load several common constants using vpcmpeqw
    // to get all ones and then vpsllq and vpsrlq to get zeros at the ends,
    // as described in "13.4 Generating constants" of
    // "2. Optimizing subroutines in assembly language" by Agner Fog, and as
    // previously implemented here. However, with x86 and x64 both using
    // constant pool loads for double constants, this is probably only
    // worthwhile in cases where a load is likely to be delayed.

    return false;
  }

  bool maybeInlineFloat(float f, FloatRegister dest) {
    // See comment above
    if (mozilla::IsPositiveZero(f)) {
      zeroFloat32(dest);
      return true;
    }
    return false;
  }

  bool maybeInlineSimd128Int(const SimdConstant& v, const FloatRegister& dest) {
    if (v.isZeroBits()) {
      vpxor(dest, dest, dest);
      return true;
    }
    if (v.isOneBits()) {
      vpcmpeqw(Operand(dest), dest, dest);
      return true;
    }
    return false;
  }
  bool maybeInlineSimd128Float(const SimdConstant& v,
                               const FloatRegister& dest) {
    if (v.isZeroBits()) {
      vxorps(dest, dest, dest);
      return true;
    }
    return false;
  }

  void convertBoolToInt32(Register source, Register dest) {
    // Note that C++ bool is only 1 byte, so zero extend it to clear the
    // higher-order bits.
    movzbl(source, dest);
  }

  void emitSet(Assembler::Condition cond, Register dest,
               Assembler::NaNCond ifNaN = Assembler::NaN_HandledByCond) {
    if (AllocatableGeneralRegisterSet(Registers::SingleByteRegs).has(dest)) {
      // If the register we're defining is a single byte register,
      // take advantage of the setCC instruction
      setCC(cond, dest);
      movzbl(dest, dest);

      if (ifNaN != Assembler::NaN_HandledByCond) {
        Label noNaN;
        j(Assembler::NoParity, &noNaN);
        mov(ImmWord(ifNaN == Assembler::NaN_IsTrue), dest);
        bind(&noNaN);
      }
    } else {
      Label end;
      Label ifFalse;

      if (ifNaN == Assembler::NaN_IsFalse) {
        j(Assembler::Parity, &ifFalse);
      }
      // Note a subtlety here: FLAGS is live at this point, and the
      // mov interface doesn't guarantee to preserve FLAGS. Use
      // movl instead of mov, because the movl instruction
      // preserves FLAGS.
      movl(Imm32(1), dest);
      j(cond, &end);
      if (ifNaN == Assembler::NaN_IsTrue) {
        j(Assembler::Parity, &end);
      }
      bind(&ifFalse);
      mov(ImmWord(0), dest);

      bind(&end);
    }
  }

  void emitSetRegisterIf(AssemblerX86Shared::Condition cond, Register dest) {
    if (AllocatableGeneralRegisterSet(Registers::SingleByteRegs).has(dest)) {
      // If the register we're defining is a single byte register,
      // take advantage of the setCC instruction
      setCC(cond, dest);
      movzbl(dest, dest);
    } else {
      Label end;
      movl(Imm32(1), dest);
      j(cond, &end);
      mov(ImmWord(0), dest);
      bind(&end);
    }
  }

  // Emit a JMP that can be toggled to a CMP. See ToggleToJmp(), ToggleToCmp().
  CodeOffset toggledJump(Label* label) {
    CodeOffset offset(size());
    jump(label);
    return offset;
  }

  template <typename T>
  void computeEffectiveAddress(const T& address, Register dest) {
    lea(Operand(address), dest);
  }

  void checkStackAlignment() {
    // Exists for ARM compatibility.
  }

  void abiret() { ret(); }

 protected:
  bool buildOOLFakeExitFrame(void* fakeReturnAddr);
};

// Specialize for float to use movaps. Use movdqa for everything else.
template <>
inline void MacroAssemblerX86Shared::loadAlignedVector<float>(
    const Address& src, FloatRegister dest) {
  loadAlignedSimd128Float(src, dest);
}

template <typename T>
inline void MacroAssemblerX86Shared::loadAlignedVector(const Address& src,
                                                       FloatRegister dest) {
  loadAlignedSimd128Int(src, dest);
}

// Specialize for float to use movaps. Use movdqa for everything else.
template <>
inline void MacroAssemblerX86Shared::storeAlignedVector<float>(
    FloatRegister src, const Address& dest) {
  storeAlignedSimd128Float(src, dest);
}

template <typename T>
inline void MacroAssemblerX86Shared::storeAlignedVector(FloatRegister src,
                                                        const Address& dest) {
  storeAlignedSimd128Int(src, dest);
}

template <>
inline void MacroAssemblerX86Shared::loadScalar<int8_t>(const Operand& src,
                                                        Register dest) {
  load8ZeroExtend(src, dest);
}
template <>
inline void MacroAssemblerX86Shared::loadScalar<int16_t>(const Operand& src,
                                                         Register dest) {
  load16ZeroExtend(src, dest);
}
template <>
inline void MacroAssemblerX86Shared::loadScalar<int32_t>(const Operand& src,
                                                         Register dest) {
  load32(src, dest);
}
template <>
inline void MacroAssemblerX86Shared::loadScalar<float>(const Operand& src,
                                                       FloatRegister dest) {
  loadFloat32(src, dest);
}

template <>
inline void MacroAssemblerX86Shared::storeScalar<int8_t>(Register src,
                                                         const Address& dest) {
  store8(src, dest);
}
template <>
inline void MacroAssemblerX86Shared::storeScalar<int16_t>(Register src,
                                                          const Address& dest) {
  store16(src, dest);
}
template <>
inline void MacroAssemblerX86Shared::storeScalar<int32_t>(Register src,
                                                          const Address& dest) {
  store32(src, dest);
}
template <>
inline void MacroAssemblerX86Shared::storeScalar<float>(FloatRegister src,
                                                        const Address& dest) {
  vmovss(src, dest);
}

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_MacroAssembler_x86_shared_h */
