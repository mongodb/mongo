/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_loong64_MacroAssembler_loong64_h
#define jit_loong64_MacroAssembler_loong64_h

#include "jit/loong64/Assembler-loong64.h"
#include "jit/MoveResolver.h"
#include "wasm/WasmBuiltins.h"

namespace js {
namespace jit {

enum LoadStoreSize {
  SizeByte = 8,
  SizeHalfWord = 16,
  SizeWord = 32,
  SizeDouble = 64
};

enum LoadStoreExtension { ZeroExtend = 0, SignExtend = 1 };

enum JumpKind { LongJump = 0, ShortJump = 1 };

static Register CallReg = t8;

enum LiFlags {
  Li64 = 0,
  Li48 = 1,
};

struct ImmShiftedTag : public ImmWord {
  explicit ImmShiftedTag(JSValueShiftedTag shtag) : ImmWord((uintptr_t)shtag) {}

  explicit ImmShiftedTag(JSValueType type)
      : ImmWord(uintptr_t(JSValueShiftedTag(JSVAL_TYPE_TO_SHIFTED_TAG(type)))) {
  }
};

struct ImmTag : public Imm32 {
  ImmTag(JSValueTag mask) : Imm32(int32_t(mask)) {}
};

static const int defaultShift = 3;
static_assert(1 << defaultShift == sizeof(JS::Value),
              "The defaultShift is wrong");

// See documentation for ScratchTagScope and ScratchTagScopeRelease in
// MacroAssembler-x64.h.

class ScratchTagScope : public SecondScratchRegisterScope {
 public:
  ScratchTagScope(MacroAssembler& masm, const ValueOperand&)
      : SecondScratchRegisterScope(masm) {}
};

class ScratchTagScopeRelease {
  ScratchTagScope* ts_;

 public:
  explicit ScratchTagScopeRelease(ScratchTagScope* ts) : ts_(ts) {
    ts_->release();
  }
  ~ScratchTagScopeRelease() { ts_->reacquire(); }
};

class MacroAssemblerLOONG64 : public Assembler {
 protected:
  // Perform a downcast. Should be removed by Bug 996602.
  MacroAssembler& asMasm();
  const MacroAssembler& asMasm() const;

  Condition ma_cmp(Register rd, Register lhs, Register rhs, Condition c);
  Condition ma_cmp(Register rd, Register lhs, Imm32 imm, Condition c);

  void compareFloatingPoint(FloatFormat fmt, FloatRegister lhs,
                            FloatRegister rhs, DoubleCondition c,
                            FPConditionBit fcc = FCC0);

 public:
  void ma_li(Register dest, CodeLabel* label);
  void ma_li(Register dest, ImmWord imm);
  void ma_liPatchable(Register dest, ImmPtr imm);
  void ma_liPatchable(Register dest, ImmWord imm, LiFlags flags = Li48);

  // load
  FaultingCodeOffset ma_ld_b(Register dest, Address address);
  FaultingCodeOffset ma_ld_h(Register dest, Address address);
  FaultingCodeOffset ma_ld_w(Register dest, Address address);
  FaultingCodeOffset ma_ld_d(Register dest, Address address);
  FaultingCodeOffset ma_ld_bu(Register dest, Address address);
  FaultingCodeOffset ma_ld_hu(Register dest, Address address);
  FaultingCodeOffset ma_ld_wu(Register dest, Address address);
  FaultingCodeOffset ma_load(Register dest, Address address,
                             LoadStoreSize size = SizeWord,
                             LoadStoreExtension extension = SignExtend);

  // store
  FaultingCodeOffset ma_st_b(Register src, Address address);
  FaultingCodeOffset ma_st_h(Register src, Address address);
  FaultingCodeOffset ma_st_w(Register src, Address address);
  FaultingCodeOffset ma_st_d(Register src, Address address);
  FaultingCodeOffset ma_store(Register data, Address address,
                              LoadStoreSize size = SizeWord,
                              LoadStoreExtension extension = SignExtend);

  // arithmetic based ops
  // add
  void ma_add_d(Register rd, Register rj, Imm32 imm);
  void ma_add32TestOverflow(Register rd, Register rj, Register rk,
                            Label* overflow);
  void ma_add32TestOverflow(Register rd, Register rj, Imm32 imm,
                            Label* overflow);
  void ma_addPtrTestOverflow(Register rd, Register rj, Register rk,
                             Label* overflow);
  void ma_addPtrTestOverflow(Register rd, Register rj, Imm32 imm,
                             Label* overflow);
  void ma_addPtrTestOverflow(Register rd, Register rj, ImmWord imm,
                             Label* overflow);
  void ma_addPtrTestCarry(Condition cond, Register rd, Register rj, Register rk,
                          Label* overflow);
  void ma_addPtrTestCarry(Condition cond, Register rd, Register rj, Imm32 imm,
                          Label* overflow);
  void ma_addPtrTestCarry(Condition cond, Register rd, Register rj, ImmWord imm,
                          Label* overflow);

  // subtract
  void ma_sub_d(Register rd, Register rj, Imm32 imm);
  void ma_sub32TestOverflow(Register rd, Register rj, Register rk,
                            Label* overflow);
  void ma_subPtrTestOverflow(Register rd, Register rj, Register rk,
                             Label* overflow);
  void ma_subPtrTestOverflow(Register rd, Register rj, Imm32 imm,
                             Label* overflow);

  // multiplies.  For now, there are only few that we care about.
  void ma_mul_d(Register rd, Register rj, Imm32 imm);
  void ma_mulh_d(Register rd, Register rj, Imm32 imm);
  void ma_mulPtrTestOverflow(Register rd, Register rj, Register rk,
                             Label* overflow);

  // stack
  void ma_pop(Register r);
  void ma_push(Register r);

  void branchWithCode(InstImm code, Label* label, JumpKind jumpKind,
                      Register scratch = Register::Invalid());
  // branches when done from within la-specific code
  void ma_b(Register lhs, ImmWord imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Register lhs, Address addr, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Address addr, Imm32 imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Address addr, ImmGCPtr imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Address addr, Register rhs, Label* l, Condition c,
            JumpKind jumpKind = LongJump) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(rhs != scratch);
    ma_ld_d(scratch, addr);
    ma_b(scratch, rhs, l, c, jumpKind);
  }

  void ma_bl(Label* l);

  // fp instructions
  void ma_lid(FloatRegister dest, double value);

  void ma_mv(FloatRegister src, ValueOperand dest);
  void ma_mv(ValueOperand src, FloatRegister dest);

  FaultingCodeOffset ma_fld_s(FloatRegister ft, Address address);
  FaultingCodeOffset ma_fld_d(FloatRegister ft, Address address);
  FaultingCodeOffset ma_fst_d(FloatRegister ft, Address address);
  FaultingCodeOffset ma_fst_s(FloatRegister ft, Address address);

  void ma_pop(FloatRegister f);
  void ma_push(FloatRegister f);

  void ma_cmp_set(Register dst, Register lhs, ImmWord imm, Condition c);
  void ma_cmp_set(Register dst, Register lhs, ImmPtr imm, Condition c);
  void ma_cmp_set(Register dst, Address address, Imm32 imm, Condition c);
  void ma_cmp_set(Register dst, Address address, ImmWord imm, Condition c);

  void moveIfZero(Register dst, Register src, Register cond) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(dst != scratch && cond != scratch);
    as_masknez(scratch, src, cond);
    as_maskeqz(dst, dst, cond);
    as_or(dst, dst, scratch);
  }
  void moveIfNotZero(Register dst, Register src, Register cond) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(dst != scratch && cond != scratch);
    as_maskeqz(scratch, src, cond);
    as_masknez(dst, dst, cond);
    as_or(dst, dst, scratch);
  }

  // These functions abstract the access to high part of the double precision
  // float register. They are intended to work on both 32 bit and 64 bit
  // floating point coprocessor.
  void moveToDoubleHi(Register src, FloatRegister dest) {
    as_movgr2frh_w(dest, src);
  }
  void moveFromDoubleHi(FloatRegister src, Register dest) {
    as_movfrh2gr_s(dest, src);
  }

  void moveToDouble(Register src, FloatRegister dest) {
    as_movgr2fr_d(dest, src);
  }
  void moveFromDouble(FloatRegister src, Register dest) {
    as_movfr2gr_d(dest, src);
  }

 public:
  void ma_li(Register dest, ImmGCPtr ptr);

  void ma_li(Register dest, Imm32 imm);
  void ma_liPatchable(Register dest, Imm32 imm);

  void ma_rotr_w(Register rd, Register rj, Imm32 shift);

  void ma_fmovz(FloatFormat fmt, FloatRegister fd, FloatRegister fj,
                Register rk);
  void ma_fmovn(FloatFormat fmt, FloatRegister fd, FloatRegister fj,
                Register rk);

  void ma_and(Register rd, Register rj, Imm32 imm, bool bit32 = false);

  void ma_or(Register rd, Register rj, Imm32 imm, bool bit32 = false);

  void ma_xor(Register rd, Register rj, Imm32 imm, bool bit32 = false);

  // load
  FaultingCodeOffset ma_load(Register dest, const BaseIndex& src,
                             LoadStoreSize size = SizeWord,
                             LoadStoreExtension extension = SignExtend);

  // store
  FaultingCodeOffset ma_store(Register data, const BaseIndex& dest,
                              LoadStoreSize size = SizeWord,
                              LoadStoreExtension extension = SignExtend);
  void ma_store(Imm32 imm, const BaseIndex& dest, LoadStoreSize size = SizeWord,
                LoadStoreExtension extension = SignExtend);

  // arithmetic based ops
  // add
  void ma_add_w(Register rd, Register rj, Imm32 imm);
  void ma_add32TestCarry(Condition cond, Register rd, Register rj, Register rk,
                         Label* overflow);
  void ma_add32TestCarry(Condition cond, Register rd, Register rj, Imm32 imm,
                         Label* overflow);

  // subtract
  void ma_sub_w(Register rd, Register rj, Imm32 imm);
  void ma_sub_w(Register rd, Register rj, Register rk);
  void ma_sub32TestOverflow(Register rd, Register rj, Imm32 imm,
                            Label* overflow);

  // multiplies.  For now, there are only few that we care about.
  void ma_mul(Register rd, Register rj, Imm32 imm);
  void ma_mul32TestOverflow(Register rd, Register rj, Register rk,
                            Label* overflow);
  void ma_mul32TestOverflow(Register rd, Register rj, Imm32 imm,
                            Label* overflow);

  // divisions
  void ma_div_branch_overflow(Register rd, Register rj, Register rk,
                              Label* overflow);
  void ma_div_branch_overflow(Register rd, Register rj, Imm32 imm,
                              Label* overflow);

  // fast mod, uses scratch registers, and thus needs to be in the assembler
  // implicitly assumes that we can overwrite dest at the beginning of the
  // sequence
  void ma_mod_mask(Register src, Register dest, Register hold, Register remain,
                   int32_t shift, Label* negZero = nullptr);

  // branches when done from within la-specific code
  void ma_b(Register lhs, Register rhs, Label* l, Condition c,
            JumpKind jumpKind = LongJump,
            Register scratch = Register::Invalid());
  void ma_b(Register lhs, Imm32 imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Register lhs, ImmPtr imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Register lhs, ImmGCPtr imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(lhs != scratch);
    ma_li(scratch, imm);
    ma_b(lhs, scratch, l, c, jumpKind);
  }

  void ma_b(Label* l, JumpKind jumpKind = LongJump);

  // fp instructions
  void ma_lis(FloatRegister dest, float value);

  FaultingCodeOffset ma_fst_d(FloatRegister src, BaseIndex address);
  FaultingCodeOffset ma_fst_s(FloatRegister src, BaseIndex address);

  FaultingCodeOffset ma_fld_d(FloatRegister dest, const BaseIndex& src);
  FaultingCodeOffset ma_fld_s(FloatRegister dest, const BaseIndex& src);

  // FP branches
  void ma_bc_s(FloatRegister lhs, FloatRegister rhs, Label* label,
               DoubleCondition c, JumpKind jumpKind = LongJump,
               FPConditionBit fcc = FCC0);
  void ma_bc_d(FloatRegister lhs, FloatRegister rhs, Label* label,
               DoubleCondition c, JumpKind jumpKind = LongJump,
               FPConditionBit fcc = FCC0);

  void ma_call(ImmPtr dest);

  void ma_jump(ImmPtr dest);

  void ma_cmp_set(Register dst, Register lhs, Register rhs, Condition c);
  void ma_cmp_set(Register dst, Register lhs, Imm32 imm, Condition c);
  void ma_cmp_set_double(Register dst, FloatRegister lhs, FloatRegister rhs,
                         DoubleCondition c);
  void ma_cmp_set_float32(Register dst, FloatRegister lhs, FloatRegister rhs,
                          DoubleCondition c);

  void moveToDoubleLo(Register src, FloatRegister dest) {
    as_movgr2fr_w(dest, src);
  }
  void moveFromDoubleLo(FloatRegister src, Register dest) {
    as_movfr2gr_s(dest, src);
  }

  void moveToFloat32(Register src, FloatRegister dest) {
    as_movgr2fr_w(dest, src);
  }
  void moveFromFloat32(FloatRegister src, Register dest) {
    as_movfr2gr_s(dest, src);
  }

  // Evaluate srcDest = minmax<isMax>{Float32,Double}(srcDest, other).
  // Handle NaN specially if handleNaN is true.
  void minMaxDouble(FloatRegister srcDest, FloatRegister other, bool handleNaN,
                    bool isMax);
  void minMaxFloat32(FloatRegister srcDest, FloatRegister other, bool handleNaN,
                     bool isMax);

  FaultingCodeOffset loadDouble(const Address& addr, FloatRegister dest);
  FaultingCodeOffset loadDouble(const BaseIndex& src, FloatRegister dest);

  // Load a float value into a register, then expand it to a double.
  void loadFloatAsDouble(const Address& addr, FloatRegister dest);
  void loadFloatAsDouble(const BaseIndex& src, FloatRegister dest);

  FaultingCodeOffset loadFloat32(const Address& addr, FloatRegister dest);
  FaultingCodeOffset loadFloat32(const BaseIndex& src, FloatRegister dest);

  void outOfLineWasmTruncateToInt32Check(FloatRegister input, Register output,
                                         MIRType fromType, TruncFlags flags,
                                         Label* rejoin,
                                         wasm::BytecodeOffset trapOffset);
  void outOfLineWasmTruncateToInt64Check(FloatRegister input, Register64 output,
                                         MIRType fromType, TruncFlags flags,
                                         Label* rejoin,
                                         wasm::BytecodeOffset trapOffset);

 protected:
  void wasmLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase,
                    Register ptr, Register ptrScratch, AnyRegister output,
                    Register tmp);
  void wasmStoreImpl(const wasm::MemoryAccessDesc& access, AnyRegister value,
                     Register memoryBase, Register ptr, Register ptrScratch,
                     Register tmp);
};

class MacroAssembler;

class MacroAssemblerLOONG64Compat : public MacroAssemblerLOONG64 {
 public:
  using MacroAssemblerLOONG64::call;

  MacroAssemblerLOONG64Compat() {}

  void convertBoolToInt32(Register src, Register dest) {
    ma_and(dest, src, Imm32(0xff));
  };
  void convertInt32ToDouble(Register src, FloatRegister dest) {
    as_movgr2fr_w(dest, src);
    as_ffint_d_w(dest, dest);
  };
  void convertInt32ToDouble(const Address& src, FloatRegister dest) {
    ma_fld_s(dest, src);
    as_ffint_d_w(dest, dest);
  };
  void convertInt32ToDouble(const BaseIndex& src, FloatRegister dest) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(scratch != src.base);
    MOZ_ASSERT(scratch != src.index);
    computeScaledAddress(src, scratch);
    convertInt32ToDouble(Address(scratch, src.offset), dest);
  };
  void convertUInt32ToDouble(Register src, FloatRegister dest);
  void convertUInt32ToFloat32(Register src, FloatRegister dest);
  void convertDoubleToFloat32(FloatRegister src, FloatRegister dest);
  void convertDoubleToInt32(FloatRegister src, Register dest, Label* fail,
                            bool negativeZeroCheck = true);
  void convertDoubleToPtr(FloatRegister src, Register dest, Label* fail,
                          bool negativeZeroCheck = true);
  void convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail,
                             bool negativeZeroCheck = true);

  void convertFloat32ToDouble(FloatRegister src, FloatRegister dest);
  void convertInt32ToFloat32(Register src, FloatRegister dest);
  void convertInt32ToFloat32(const Address& src, FloatRegister dest);

  void movq(Register rj, Register rd);

  void computeScaledAddress(const BaseIndex& address, Register dest);

  void computeEffectiveAddress(const Address& address, Register dest) {
    ma_add_d(dest, address.base, Imm32(address.offset));
  }

  void computeEffectiveAddress(const BaseIndex& address, Register dest) {
    computeScaledAddress(address, dest);
    if (address.offset) {
      ma_add_d(dest, dest, Imm32(address.offset));
    }
  }

  void j(Label* dest) { ma_b(dest); }

  void mov(Register src, Register dest) { as_ori(dest, src, 0); }
  void mov(ImmWord imm, Register dest) { ma_li(dest, imm); }
  void mov(ImmPtr imm, Register dest) {
    mov(ImmWord(uintptr_t(imm.value)), dest);
  }
  void mov(CodeLabel* label, Register dest) { ma_li(dest, label); }
  void mov(Register src, Address dest) { MOZ_CRASH("NYI-IC"); }
  void mov(Address src, Register dest) { MOZ_CRASH("NYI-IC"); }

  void writeDataRelocation(const Value& val) {
    // Raw GC pointer relocations and Value relocations both end up in
    // TraceOneDataRelocation.
    if (val.isGCThing()) {
      gc::Cell* cell = val.toGCThing();
      if (cell && gc::IsInsideNursery(cell)) {
        embedsNurseryPointers_ = true;
      }
      dataRelocations_.writeUnsigned(currentOffset());
    }
  }

  void branch(JitCode* c) {
    ScratchRegisterScope scratch(asMasm());
    BufferOffset bo = m_buffer.nextOffset();
    addPendingJump(bo, ImmPtr(c->raw()), RelocationKind::JITCODE);
    ma_liPatchable(scratch, ImmPtr(c->raw()));
    as_jirl(zero, scratch, BOffImm16(0));
  }
  void branch(const Register reg) { as_jirl(zero, reg, BOffImm16(0)); }
  void nop() { as_nop(); }
  void ret() {
    ma_pop(ra);
    as_jirl(zero, ra, BOffImm16(0));
  }
  inline void retn(Imm32 n);
  void push(Imm32 imm) {
    ScratchRegisterScope scratch(asMasm());
    ma_li(scratch, imm);
    ma_push(scratch);
  }
  void push(ImmWord imm) {
    ScratchRegisterScope scratch(asMasm());
    ma_li(scratch, imm);
    ma_push(scratch);
  }
  void push(ImmGCPtr imm) {
    ScratchRegisterScope scratch(asMasm());
    ma_li(scratch, imm);
    ma_push(scratch);
  }
  void push(const Address& address) {
    SecondScratchRegisterScope scratch2(asMasm());
    loadPtr(address, scratch2);
    ma_push(scratch2);
  }
  void push(Register reg) { ma_push(reg); }
  void push(FloatRegister reg) { ma_push(reg); }
  void pop(Register reg) { ma_pop(reg); }
  void pop(FloatRegister reg) { ma_pop(reg); }

  // Emit a branch that can be toggled to a non-operation. On LOONG64 we use
  // "andi" instruction to toggle the branch.
  // See ToggleToJmp(), ToggleToCmp().
  CodeOffset toggledJump(Label* label);

  // Emit a "jalr" or "nop" instruction. ToggleCall can be used to patch
  // this instruction.
  CodeOffset toggledCall(JitCode* target, bool enabled);

  static size_t ToggledCallSize(uint8_t* code) {
    // Four instructions used in: MacroAssemblerLOONG64Compat::toggledCall
    return 4 * sizeof(uint32_t);
  }

  CodeOffset pushWithPatch(ImmWord imm) {
    ScratchRegisterScope scratch(asMasm());
    CodeOffset offset = movWithPatch(imm, scratch);
    ma_push(scratch);
    return offset;
  }

  CodeOffset movWithPatch(ImmWord imm, Register dest) {
    CodeOffset offset = CodeOffset(currentOffset());
    ma_liPatchable(dest, imm, Li64);
    return offset;
  }
  CodeOffset movWithPatch(ImmPtr imm, Register dest) {
    CodeOffset offset = CodeOffset(currentOffset());
    ma_liPatchable(dest, imm);
    return offset;
  }

  void writeCodePointer(CodeLabel* label) {
    label->patchAt()->bind(currentOffset());
    label->setLinkMode(CodeLabel::RawPointer);
    m_buffer.ensureSpace(sizeof(void*));
    writeInst(-1);
    writeInst(-1);
  }

  void jump(Label* label) { ma_b(label); }
  void jump(Register reg) { as_jirl(zero, reg, BOffImm16(0)); }
  void jump(const Address& address) {
    ScratchRegisterScope scratch(asMasm());
    loadPtr(address, scratch);
    as_jirl(zero, scratch, BOffImm16(0));
  }

  void jump(JitCode* code) { branch(code); }

  void jump(ImmPtr ptr) {
    BufferOffset bo = m_buffer.nextOffset();
    addPendingJump(bo, ptr, RelocationKind::HARDCODED);
    ma_jump(ptr);
  }

  void jump(TrampolinePtr code) { jump(ImmPtr(code.value)); }

  void splitTag(Register src, Register dest) {
    as_srli_d(dest, src, JSVAL_TAG_SHIFT);
  }

  void splitTag(const ValueOperand& operand, Register dest) {
    splitTag(operand.valueReg(), dest);
  }

  void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag) {
    splitTag(value, tag);
  }

  // unboxing code
  void unboxNonDouble(const ValueOperand& operand, Register dest,
                      JSValueType type) {
    unboxNonDouble(operand.valueReg(), dest, type);
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
      as_slli_w(dest, src, 0);
      return;
    }
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(scratch != src);
    mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), scratch);
    as_xor(dest, src, scratch);
  }

  template <typename T>
  void unboxObjectOrNull(const T& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    static_assert(JS::detail::ValueObjectOrNullBit ==
                  (uint64_t(0x8) << JSVAL_TAG_SHIFT));
    as_bstrins_d(dest, zero, JSVAL_TAG_SHIFT + 3, JSVAL_TAG_SHIFT + 3);
  }

  void unboxGCThingForGCBarrier(const Address& src, Register dest) {
    loadPtr(src, dest);
    as_bstrpick_d(dest, dest, JSVAL_TAG_SHIFT - 1, 0);
  }
  void unboxGCThingForGCBarrier(const ValueOperand& src, Register dest) {
    as_bstrpick_d(dest, src.valueReg(), JSVAL_TAG_SHIFT - 1, 0);
  }

  void unboxWasmAnyRefGCThingForGCBarrier(const Address& src, Register dest) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(scratch != dest);
    movePtr(ImmWord(wasm::AnyRef::GCThingMask), scratch);
    loadPtr(src, dest);
    as_and(dest, dest, scratch);
  }

  // Like unboxGCThingForGCBarrier, but loads the GC thing's chunk base.
  void getGCThingValueChunk(const Address& src, Register dest) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(scratch != dest);
    loadPtr(src, dest);
    movePtr(ImmWord(JS::detail::ValueGCThingPayloadChunkMask), scratch);
    as_and(dest, dest, scratch);
  }
  void getGCThingValueChunk(const ValueOperand& src, Register dest) {
    MOZ_ASSERT(src.valueReg() != dest);
    movePtr(ImmWord(JS::detail::ValueGCThingPayloadChunkMask), dest);
    as_and(dest, dest, src.valueReg());
  }

  void getWasmAnyRefGCThingChunk(Register src, Register dest) {
    MOZ_ASSERT(src != dest);
    movePtr(ImmWord(wasm::AnyRef::GCThingChunkMask), dest);
    as_and(dest, dest, src);
  }

  void unboxInt32(const ValueOperand& operand, Register dest);
  void unboxInt32(Register src, Register dest);
  void unboxInt32(const Address& src, Register dest);
  void unboxInt32(const BaseIndex& src, Register dest);
  void unboxBoolean(const ValueOperand& operand, Register dest);
  void unboxBoolean(Register src, Register dest);
  void unboxBoolean(const Address& src, Register dest);
  void unboxBoolean(const BaseIndex& src, Register dest);
  void unboxDouble(const ValueOperand& operand, FloatRegister dest);
  void unboxDouble(Register src, Register dest);
  void unboxDouble(const Address& src, FloatRegister dest);
  void unboxDouble(const BaseIndex& src, FloatRegister dest);
  void unboxString(const ValueOperand& operand, Register dest);
  void unboxString(Register src, Register dest);
  void unboxString(const Address& src, Register dest);
  void unboxSymbol(const ValueOperand& src, Register dest);
  void unboxSymbol(Register src, Register dest);
  void unboxSymbol(const Address& src, Register dest);
  void unboxBigInt(const ValueOperand& operand, Register dest);
  void unboxBigInt(Register src, Register dest);
  void unboxBigInt(const Address& src, Register dest);
  void unboxObject(const ValueOperand& src, Register dest);
  void unboxObject(Register src, Register dest);
  void unboxObject(const Address& src, Register dest);
  void unboxObject(const BaseIndex& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
  }
  void unboxValue(const ValueOperand& src, AnyRegister dest, JSValueType type);

  void notBoolean(const ValueOperand& val) {
    as_xori(val.valueReg(), val.valueReg(), 1);
  }

  // boxing code
  void boxDouble(FloatRegister src, const ValueOperand& dest, FloatRegister);
  void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest);

  // Extended unboxing API. If the payload is already in a register, returns
  // that register. Otherwise, provides a move to the given scratch register,
  // and returns that.
  [[nodiscard]] Register extractObject(const Address& address,
                                       Register scratch);
  [[nodiscard]] Register extractObject(const ValueOperand& value,
                                       Register scratch) {
    unboxObject(value, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractString(const ValueOperand& value,
                                       Register scratch) {
    unboxString(value, scratch);
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
  [[nodiscard]] Register extractTag(const Address& address, Register scratch);
  [[nodiscard]] Register extractTag(const BaseIndex& address, Register scratch);
  [[nodiscard]] Register extractTag(const ValueOperand& value,
                                    Register scratch) {
    splitTag(value, scratch);
    return scratch;
  }

  inline void ensureDouble(const ValueOperand& source, FloatRegister dest,
                           Label* failure);

  void boolValueToDouble(const ValueOperand& operand, FloatRegister dest);
  void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest);
  void loadInt32OrDouble(const Address& src, FloatRegister dest);
  void loadInt32OrDouble(const BaseIndex& addr, FloatRegister dest);
  void loadConstantDouble(double dp, FloatRegister dest);

  void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest);
  void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest);
  void loadConstantFloat32(float f, FloatRegister dest);

  void testNullSet(Condition cond, const ValueOperand& value, Register dest);

  void testObjectSet(Condition cond, const ValueOperand& value, Register dest);

  void testUndefinedSet(Condition cond, const ValueOperand& value,
                        Register dest);

  template <typename T>
  void loadUnboxedValue(const T& address, MIRType type, AnyRegister dest) {
    if (dest.isFloat()) {
      loadInt32OrDouble(address, dest.fpu());
    } else {
      unboxNonDouble(address, dest.gpr(), ValueTypeFromMIRType(type));
    }
  }

  void storeUnboxedPayload(ValueOperand value, BaseIndex address, size_t nbytes,
                           JSValueType type) {
    switch (nbytes) {
      case 8: {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());
        if (type == JSVAL_TYPE_OBJECT) {
          unboxObjectOrNull(value, scratch2);
        } else {
          unboxNonDouble(value, scratch2, type);
        }
        computeEffectiveAddress(address, scratch);
        as_st_d(scratch2, scratch, 0);
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

  void storeUnboxedPayload(ValueOperand value, Address address, size_t nbytes,
                           JSValueType type) {
    switch (nbytes) {
      case 8: {
        SecondScratchRegisterScope scratch2(asMasm());
        if (type == JSVAL_TYPE_OBJECT) {
          unboxObjectOrNull(value, scratch2);
        } else {
          unboxNonDouble(value, scratch2, type);
        }
        storePtr(scratch2, address);
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

  void boxValue(JSValueType type, Register src, Register dest) {
    ScratchRegisterScope scratch(asMasm());
    if (src == dest) {
      as_ori(scratch, src, 0);
      src = scratch;
    }
#ifdef DEBUG
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      Label upper32BitsSignExtended;
      as_slli_w(dest, src, 0);
      ma_b(src, dest, &upper32BitsSignExtended, Equal, ShortJump);
      breakpoint();
      bind(&upper32BitsSignExtended);
    }
#endif
    ma_li(dest, ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)));
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      as_bstrins_d(dest, src, 31, 0);
    } else {
      as_bstrins_d(dest, src, JSVAL_TAG_SHIFT - 1, 0);
    }
  }

  void storeValue(ValueOperand val, const Address& dest);
  void storeValue(ValueOperand val, const BaseIndex& dest);
  void storeValue(JSValueType type, Register reg, Address dest);
  void storeValue(JSValueType type, Register reg, BaseIndex dest);
  void storeValue(const Value& val, Address dest);
  void storeValue(const Value& val, BaseIndex dest);
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

  void loadValue(Address src, ValueOperand val);
  void loadValue(const BaseIndex& src, ValueOperand val);

  void loadUnalignedValue(const Address& src, ValueOperand dest) {
    loadValue(src, dest);
  }

  void tagValue(JSValueType type, Register payload, ValueOperand dest);

  void pushValue(ValueOperand val);
  void popValue(ValueOperand val);
  void pushValue(const Value& val) {
    if (val.isGCThing()) {
      ScratchRegisterScope scratch(asMasm());
      writeDataRelocation(val);
      movWithPatch(ImmWord(val.asRawBits()), scratch);
      push(scratch);
    } else {
      push(ImmWord(val.asRawBits()));
    }
  }
  void pushValue(JSValueType type, Register reg) {
    SecondScratchRegisterScope scratch2(asMasm());
    boxValue(type, reg, scratch2);
    push(scratch2);
  }
  void pushValue(const Address& addr);
  void pushValue(const BaseIndex& addr, Register scratch) {
    loadValue(addr, ValueOperand(scratch));
    pushValue(ValueOperand(scratch));
  }

  void handleFailureWithHandlerTail(Label* profilerExitTail,
                                    Label* bailoutTail);

  /////////////////////////////////////////////////////////////////
  // Common interface.
  /////////////////////////////////////////////////////////////////
 public:
  // The following functions are exposed for use in platform-shared code.

  inline void incrementInt32Value(const Address& addr);

  void move32(Imm32 imm, Register dest);
  void move32(Register src, Register dest);

  void movePtr(Register src, Register dest);
  void movePtr(ImmWord imm, Register dest);
  void movePtr(ImmPtr imm, Register dest);
  void movePtr(wasm::SymbolicAddress imm, Register dest);
  void movePtr(ImmGCPtr imm, Register dest);

  FaultingCodeOffset load8SignExtend(const Address& address, Register dest);
  FaultingCodeOffset load8SignExtend(const BaseIndex& src, Register dest);

  FaultingCodeOffset load8ZeroExtend(const Address& address, Register dest);
  FaultingCodeOffset load8ZeroExtend(const BaseIndex& src, Register dest);

  FaultingCodeOffset load16SignExtend(const Address& address, Register dest);
  FaultingCodeOffset load16SignExtend(const BaseIndex& src, Register dest);

  template <typename S>
  void load16UnalignedSignExtend(const S& src, Register dest) {
    load16SignExtend(src, dest);
  }

  FaultingCodeOffset load16ZeroExtend(const Address& address, Register dest);
  FaultingCodeOffset load16ZeroExtend(const BaseIndex& src, Register dest);

  template <typename S>
  void load16UnalignedZeroExtend(const S& src, Register dest) {
    load16ZeroExtend(src, dest);
  }

  FaultingCodeOffset load32(const Address& address, Register dest);
  FaultingCodeOffset load32(const BaseIndex& address, Register dest);
  void load32(AbsoluteAddress address, Register dest);
  void load32(wasm::SymbolicAddress address, Register dest);

  template <typename S>
  void load32Unaligned(const S& src, Register dest) {
    load32(src, dest);
  }

  FaultingCodeOffset load64(const Address& address, Register64 dest) {
    return loadPtr(address, dest.reg);
  }
  FaultingCodeOffset load64(const BaseIndex& address, Register64 dest) {
    return loadPtr(address, dest.reg);
  }

  template <typename S>
  void load64Unaligned(const S& src, Register64 dest) {
    load64(src, dest);
  }

  FaultingCodeOffset loadPtr(const Address& address, Register dest);
  FaultingCodeOffset loadPtr(const BaseIndex& src, Register dest);
  void loadPtr(AbsoluteAddress address, Register dest);
  void loadPtr(wasm::SymbolicAddress address, Register dest);

  void loadPrivate(const Address& address, Register dest);

  FaultingCodeOffset store8(Register src, const Address& address);
  FaultingCodeOffset store8(Register src, const BaseIndex& address);
  void store8(Imm32 imm, const Address& address);
  void store8(Imm32 imm, const BaseIndex& address);

  FaultingCodeOffset store16(Register src, const Address& address);
  FaultingCodeOffset store16(Register src, const BaseIndex& address);
  void store16(Imm32 imm, const Address& address);
  void store16(Imm32 imm, const BaseIndex& address);

  template <typename T>
  void store16Unaligned(Register src, const T& dest) {
    store16(src, dest);
  }

  FaultingCodeOffset store32(Register src, const Address& address);
  FaultingCodeOffset store32(Register src, const BaseIndex& address);
  void store32(Register src, AbsoluteAddress address);
  void store32(Imm32 src, const Address& address);
  void store32(Imm32 src, const BaseIndex& address);

  template <typename T>
  void store32Unaligned(Register src, const T& dest) {
    store32(src, dest);
  }

  void store64(Imm64 imm, Address address) {
    storePtr(ImmWord(imm.value), address);
  }
  void store64(Imm64 imm, const BaseIndex& address) {
    storePtr(ImmWord(imm.value), address);
  }

  FaultingCodeOffset store64(Register64 src, Address address) {
    return storePtr(src.reg, address);
  }

  FaultingCodeOffset store64(Register64 src, const BaseIndex& address) {
    return storePtr(src.reg, address);
  }

  template <typename T>
  void store64Unaligned(Register64 src, const T& dest) {
    store64(src, dest);
  }

  template <typename T>
  void storePtr(ImmWord imm, T address);
  template <typename T>
  void storePtr(ImmPtr imm, T address);
  template <typename T>
  void storePtr(ImmGCPtr imm, T address);
  void storePtr(Register src, AbsoluteAddress dest);
  FaultingCodeOffset storePtr(Register src, const Address& address);
  FaultingCodeOffset storePtr(Register src, const BaseIndex& address);

  void moveDouble(FloatRegister src, FloatRegister dest) {
    as_fmov_d(dest, src);
  }

  void zeroDouble(FloatRegister reg) { moveToDouble(zero, reg); }

  void convertUInt64ToDouble(Register src, FloatRegister dest);

  void breakpoint(uint32_t value = 0);

  void checkStackAlignment() {
#ifdef DEBUG
    Label aligned;
    ScratchRegisterScope scratch(asMasm());
    as_andi(scratch, sp, ABIStackAlignment - 1);
    ma_b(scratch, zero, &aligned, Equal, ShortJump);
    breakpoint();
    bind(&aligned);
#endif
  };

  static void calculateAlignedStackPointer(void** stackPointer);

  void cmpPtrSet(Assembler::Condition cond, Address lhs, ImmPtr rhs,
                 Register dest);
  void cmpPtrSet(Assembler::Condition cond, Register lhs, Address rhs,
                 Register dest);
  void cmpPtrSet(Assembler::Condition cond, Address lhs, Register rhs,
                 Register dest);

  void cmp32Set(Assembler::Condition cond, Register lhs, Address rhs,
                Register dest);

 protected:
  bool buildOOLFakeExitFrame(void* fakeReturnAddr);

  void wasmLoadI64Impl(const wasm::MemoryAccessDesc& access,
                       Register memoryBase, Register ptr, Register ptrScratch,
                       Register64 output, Register tmp);
  void wasmStoreI64Impl(const wasm::MemoryAccessDesc& access, Register64 value,
                        Register memoryBase, Register ptr, Register ptrScratch,
                        Register tmp);

 public:
  void lea(Operand addr, Register dest) {
    ma_add_d(dest, addr.baseReg(), Imm32(addr.disp()));
  }

  void abiret() { as_jirl(zero, ra, BOffImm16(0)); }

  void moveFloat32(FloatRegister src, FloatRegister dest) {
    as_fmov_s(dest, src);
  }

  // Instrumentation for entering and leaving the profiler.
  void profilerEnterFrame(Register framePtr, Register scratch);
  void profilerExitFrame();
};

typedef MacroAssemblerLOONG64Compat MacroAssemblerSpecific;

}  // namespace jit
}  // namespace js

#endif /* jit_loong64_MacroAssembler_loong64_h */
