/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef jit_riscv64_MacroAssembler_riscv64_h
#define jit_riscv64_MacroAssembler_riscv64_h

#include <iterator>

#include "jit/MoveResolver.h"
#include "jit/riscv64/Assembler-riscv64.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace jit {

static Register CallReg = t6;

enum LiFlags {
  Li64 = 0,
  Li48 = 1,
};

class CompactBufferReader;
enum LoadStoreSize {
  SizeByte = 8,
  SizeHalfWord = 16,
  SizeWord = 32,
  SizeDouble = 64
};

enum LoadStoreExtension { ZeroExtend = 0, SignExtend = 1 };
enum JumpKind { LongJump = 0, ShortJump = 1 };
enum FloatFormat { SingleFloat, DoubleFloat };
class ScratchTagScope : public ScratchRegisterScope {
 public:
  ScratchTagScope(MacroAssembler& masm, const ValueOperand&)
      : ScratchRegisterScope(masm) {}
};

class ScratchTagScopeRelease {
  ScratchTagScope* ts_;

 public:
  explicit ScratchTagScopeRelease(ScratchTagScope* ts) : ts_(ts) {
    ts_->release();
  }
  ~ScratchTagScopeRelease() { ts_->reacquire(); }
};

struct ImmTag : public Imm32 {
  ImmTag(JSValueTag mask) : Imm32(int32_t(mask)) {}
};

class MacroAssemblerRiscv64 : public Assembler {
 public:
  MacroAssemblerRiscv64() {}

#ifdef JS_SIMULATOR_RISCV64
  // See riscv64/base-constants-riscv.h DebugParameters.
  void Debug(uint32_t parameters) { break_(parameters, false); }
#endif

  // Perform a downcast. Should be removed by Bug 996602.
  MacroAssembler& asMasm();
  const MacroAssembler& asMasm() const;

  MoveResolver moveResolver_;

  static bool SupportsFloatingPoint() { return true; }
  static bool SupportsUnalignedAccesses() { return true; }
  static bool SupportsFastUnalignedFPAccesses() { return true; }
  void haltingAlign(int alignment) {
    // TODO(loong64): Implement a proper halting align.
    nopAlign(alignment);
  }

  // TODO(RISCV) Reorder parameters so out parameters come last.
  bool CalculateOffset(Label* L, int32_t* offset, OffsetSize bits);
  int32_t GetOffset(int32_t offset, Label* L, OffsetSize bits);

  inline void GenPCRelativeJump(Register rd, int32_t imm32) {
    MOZ_ASSERT(is_int32(imm32 + 0x800));
    int32_t Hi20 = ((imm32 + 0x800) >> 12);
    int32_t Lo12 = imm32 << 20 >> 20;
    auipc(rd, Hi20);  // Read PC + Hi20 into scratch.
    jr(rd, Lo12);     // jump PC + Hi20 + Lo12
  }

  // load
  FaultingCodeOffset ma_load(Register dest, Address address,
                             LoadStoreSize size = SizeWord,
                             LoadStoreExtension extension = SignExtend);
  FaultingCodeOffset ma_load(Register dest, const BaseIndex& src,
                             LoadStoreSize size = SizeWord,
                             LoadStoreExtension extension = SignExtend);
  FaultingCodeOffset ma_loadDouble(FloatRegister dest, Address address);
  FaultingCodeOffset ma_loadFloat(FloatRegister dest, Address address);
  // store
  FaultingCodeOffset ma_store(Register data, Address address,
                              LoadStoreSize size = SizeWord,
                              LoadStoreExtension extension = SignExtend);
  FaultingCodeOffset ma_store(Register data, const BaseIndex& dest,
                              LoadStoreSize size = SizeWord,
                              LoadStoreExtension extension = SignExtend);
  FaultingCodeOffset ma_store(Imm32 imm, const BaseIndex& dest,
                              LoadStoreSize size = SizeWord,
                              LoadStoreExtension extension = SignExtend);
  FaultingCodeOffset ma_store(Imm32 imm, Address address,
                              LoadStoreSize size = SizeWord,
                              LoadStoreExtension extension = SignExtend);
  void ma_storeDouble(FloatRegister dest, Address address);
  void ma_storeFloat(FloatRegister dest, Address address);
  void ma_liPatchable(Register dest, Imm32 imm);
  void ma_liPatchable(Register dest, ImmPtr imm);
  void ma_liPatchable(Register dest, ImmWord imm, LiFlags flags = Li48);
  void ma_li(Register dest, ImmGCPtr ptr);
  void ma_li(Register dest, Imm32 imm);
  void ma_li(Register dest, Imm64 imm);
  void ma_li(Register dest, intptr_t imm) { RV_li(dest, imm); }
  void ma_li(Register dest, CodeLabel* label);
  void ma_li(Register dest, ImmWord imm);

  // branches when done from within la-specific code
  void ma_b(Register lhs, Register rhs, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Register lhs, Imm32 imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void BranchAndLinkShort(Label* L);
  void BranchAndLink(Label* label);
  void BranchAndLinkShort(int32_t offset);
  void BranchAndLinkShortHelper(int32_t offset, Label* L);
  void BranchAndLinkLong(Label* L);
  void GenPCRelativeJumpAndLink(Register rd, int32_t imm32);

#define DEFINE_INSTRUCTION(instr)                                           \
  void instr(Register rd, Register rj, Operand rt);                         \
  void instr(Register rd, Register rj, Imm32 imm) {                         \
    instr(rd, rj, Operand(imm.value));                                      \
  }                                                                         \
  void instr(Register rd, Imm32 imm) { instr(rd, rd, Operand(imm.value)); } \
  void instr(Register rd, Register rs) { instr(rd, rd, Operand(rs)); }

#define DEFINE_INSTRUCTION2(instr)                                 \
  void instr(Register rs, const Operand& rt);                      \
  void instr(Register rs, Register rt) { instr(rs, Operand(rt)); } \
  void instr(Register rs, Imm32 j) { instr(rs, Operand(j.value)); }

  DEFINE_INSTRUCTION(ma_and);
  DEFINE_INSTRUCTION(ma_or);
  DEFINE_INSTRUCTION(ma_xor);
  DEFINE_INSTRUCTION(ma_nor);
  DEFINE_INSTRUCTION(ma_sub32)
  DEFINE_INSTRUCTION(ma_sub64)
  DEFINE_INSTRUCTION(ma_add32)
  DEFINE_INSTRUCTION(ma_add64)
  DEFINE_INSTRUCTION(ma_div32)
  DEFINE_INSTRUCTION(ma_divu32)
  DEFINE_INSTRUCTION(ma_div64)
  DEFINE_INSTRUCTION(ma_divu64)
  DEFINE_INSTRUCTION(ma_mod32)
  DEFINE_INSTRUCTION(ma_modu32)
  DEFINE_INSTRUCTION(ma_mod64)
  DEFINE_INSTRUCTION(ma_modu64)
  DEFINE_INSTRUCTION(ma_mul32)
  DEFINE_INSTRUCTION(ma_mulh32)
  DEFINE_INSTRUCTION(ma_mulhu32)
  DEFINE_INSTRUCTION(ma_mul64)
  DEFINE_INSTRUCTION(ma_mulh64)
  DEFINE_INSTRUCTION(ma_sll64)
  DEFINE_INSTRUCTION(ma_sra64)
  DEFINE_INSTRUCTION(ma_srl64)
  DEFINE_INSTRUCTION(ma_sll32)
  DEFINE_INSTRUCTION(ma_sra32)
  DEFINE_INSTRUCTION(ma_srl32)
  DEFINE_INSTRUCTION(ma_slt)
  DEFINE_INSTRUCTION(ma_sltu)
  DEFINE_INSTRUCTION(ma_sle)
  DEFINE_INSTRUCTION(ma_sleu)
  DEFINE_INSTRUCTION(ma_sgt)
  DEFINE_INSTRUCTION(ma_sgtu)
  DEFINE_INSTRUCTION(ma_sge)
  DEFINE_INSTRUCTION(ma_sgeu)
  DEFINE_INSTRUCTION(ma_seq)
  DEFINE_INSTRUCTION(ma_sne)

  DEFINE_INSTRUCTION2(ma_seqz)
  DEFINE_INSTRUCTION2(ma_snez)
  DEFINE_INSTRUCTION2(ma_neg);

#undef DEFINE_INSTRUCTION2
#undef DEFINE_INSTRUCTION
  // arithmetic based ops
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
  void ma_sub32TestOverflow(Register rd, Register rj, Register rk,
                            Label* overflow);
  void ma_subPtrTestOverflow(Register rd, Register rj, Register rk,
                             Label* overflow);
  void ma_subPtrTestOverflow(Register rd, Register rj, Imm32 imm,
                             Label* overflow);

  // multiplies.  For now, there are only few that we care about.
  void ma_mulPtrTestOverflow(Register rd, Register rj, Register rk,
                             Label* overflow);

  // branches when done from within la-specific code
  void ma_b(Register lhs, ImmWord imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Register lhs, ImmPtr imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Register lhs, ImmGCPtr imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump) {
    UseScratchRegisterScope temps(this);
    Register ScratchRegister = temps.Acquire();
    ma_li(ScratchRegister, imm);
    ma_b(lhs, ScratchRegister, l, c, jumpKind);
  }
  void ma_b(Register lhs, Address addr, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Address addr, Imm32 imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Address addr, ImmGCPtr imm, Label* l, Condition c,
            JumpKind jumpKind = LongJump);
  void ma_b(Address addr, Register rhs, Label* l, Condition c,
            JumpKind jumpKind = LongJump) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    MOZ_ASSERT(rhs != scratch);
    ma_load(scratch, addr, SizeDouble);
    ma_b(scratch, rhs, l, c, jumpKind);
  }

  void ma_branch(Label* target, Condition cond, Register r1, const Operand& r2,
                 JumpKind jumpKind = ShortJump);

  void ma_branch(Label* target, JumpKind jumpKind = ShortJump) {
    ma_branch(target, Always, zero, zero, jumpKind);
  }

  // fp instructions
  void ma_lid(FloatRegister dest, double value);

  // fp instructions
  void ma_lis(FloatRegister dest, float value);

  FaultingCodeOffset ma_fst_d(FloatRegister src, BaseIndex address);
  FaultingCodeOffset ma_fst_s(FloatRegister src, BaseIndex address);

  void ma_fld_d(FloatRegister dest, const BaseIndex& src);
  void ma_fld_s(FloatRegister dest, const BaseIndex& src);

  void ma_fmv_d(FloatRegister src, ValueOperand dest);
  void ma_fmv_d(ValueOperand src, FloatRegister dest);

  void ma_fmv_w(FloatRegister src, ValueOperand dest);
  void ma_fmv_w(ValueOperand src, FloatRegister dest);

  FaultingCodeOffset ma_fld_s(FloatRegister ft, Address address);
  FaultingCodeOffset ma_fld_d(FloatRegister ft, Address address);
  FaultingCodeOffset ma_fst_d(FloatRegister ft, Address address);
  FaultingCodeOffset ma_fst_s(FloatRegister ft, Address address);

  // stack
  void ma_pop(Register r);
  void ma_push(Register r);
  void ma_pop(FloatRegister f);
  void ma_push(FloatRegister f);

  Condition ma_cmp(Register rd, Register lhs, Register rhs, Condition c);
  Condition ma_cmp(Register rd, Register lhs, Imm32 imm, Condition c);
  void ma_cmp_set(Register dst, Register lhs, ImmWord imm, Condition c);
  void ma_cmp_set(Register dst, Register lhs, ImmPtr imm, Condition c);
  void ma_cmp_set(Register dst, Address address, Imm32 imm, Condition c);
  void ma_cmp_set(Register dst, Address address, ImmWord imm, Condition c);

  void ma_rotr_w(Register rd, Register rj, Imm32 shift);

  void ma_fmovz(FloatFormat fmt, FloatRegister fd, FloatRegister fj,
                Register rk);
  void ma_fmovn(FloatFormat fmt, FloatRegister fd, FloatRegister fj,
                Register rk);

  // arithmetic based ops
  void ma_add32TestCarry(Condition cond, Register rd, Register rj, Register rk,
                         Label* overflow);
  void ma_add32TestCarry(Condition cond, Register rd, Register rj, Imm32 imm,
                         Label* overflow);

  // subtract
  void ma_sub32TestOverflow(Register rd, Register rj, Imm32 imm,
                            Label* overflow);

  void MulOverflow32(Register dst, Register left, const Operand& right,
                     Register overflow);
  // multiplies.  For now, there are only few that we care about.
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

  // FP branches
  void ma_compareF32(Register rd, DoubleCondition cc, FloatRegister cmp1,
                     FloatRegister cmp2);
  void ma_compareF64(Register rd, DoubleCondition cc, FloatRegister cmp1,
                     FloatRegister cmp2);

  void CompareIsNotNanF32(Register rd, FPURegister cmp1, FPURegister cmp2);
  void CompareIsNotNanF64(Register rd, FPURegister cmp1, FPURegister cmp2);
  void CompareIsNanF32(Register rd, FPURegister cmp1, FPURegister cmp2);
  void CompareIsNanF64(Register rd, FPURegister cmp1, FPURegister cmp2);

  void ma_call(ImmPtr dest);

  void ma_jump(ImmPtr dest);

  void jump(Label* label) { ma_branch(label); }
  void jump(Register reg) { jr(reg); }

  void ma_cmp_set(Register dst, Register lhs, Register rhs, Condition c);
  void ma_cmp_set(Register dst, Register lhs, Imm32 imm, Condition c);

  void computeScaledAddress(const BaseIndex& address, Register dest);

  void BranchShort(Label* L);

  void BranchShort(int32_t offset, Condition cond, Register rs,
                   const Operand& rt);
  void BranchShort(Label* L, Condition cond, Register rs, const Operand& rt);
  void BranchShortHelper(int32_t offset, Label* L);
  bool BranchShortHelper(int32_t offset, Label* L, Condition cond, Register rs,
                         const Operand& rt);
  bool BranchShortCheck(int32_t offset, Label* L, Condition cond, Register rs,
                        const Operand& rt);
  void BranchLong(Label* L);

  // Floating point branches
  void BranchTrueShortF(Register rs, Label* target);
  void BranchFalseShortF(Register rs, Label* target);

  void BranchTrueF(Register rs, Label* target);
  void BranchFalseF(Register rs, Label* target);

  void moveFromDoubleHi(FloatRegister src, Register dest) {
    fmv_x_d(dest, src);
    srli(dest, dest, 32);
  }
  // Bit field starts at bit pos and extending for size bits is extracted from
  // rs and stored zero/sign-extended and right-justified in rt
  void ExtractBits(Register rt, Register rs, uint16_t pos, uint16_t size,
                   bool sign_extend = false);
  void ExtractBits(Register dest, Register source, Register pos, int size,
                   bool sign_extend = false) {
    sra(dest, source, pos);
    ExtractBits(dest, dest, 0, size, sign_extend);
  }

  // Insert bits [0, size) of source to bits [pos, pos+size) of dest
  void InsertBits(Register dest, Register source, Register pos, int size);

  // Insert bits [0, size) of source to bits [pos, pos+size) of dest
  void InsertBits(Register dest, Register source, int pos, int size);

  template <typename F_TYPE>
  void RoundHelper(FPURegister dst, FPURegister src, FPURegister fpu_scratch,
                   FPURoundingMode mode);

  template <typename TruncFunc>
  void RoundFloatingPointToInteger(Register rd, FPURegister fs, Register result,
                                   TruncFunc trunc, bool Inexact = false);

  void Clear_if_nan_d(Register rd, FPURegister fs);
  void Clear_if_nan_s(Register rd, FPURegister fs);
  // Convert double to unsigned word.
  void Trunc_uw_d(Register rd, FPURegister fs, Register result = InvalidReg,
                  bool Inexact = false);

  // Convert double to signed word.
  void Trunc_w_d(Register rd, FPURegister fs, Register result = InvalidReg,
                 bool Inexact = false);

  // Convert double to unsigned long.
  void Trunc_ul_d(Register rd, FPURegister fs, Register result = InvalidReg,
                  bool Inexact = false);

  // Convert singled to signed long.
  void Trunc_l_d(Register rd, FPURegister fs, Register result = InvalidReg,
                 bool Inexact = false);

  // Convert single to signed word.
  void Trunc_w_s(Register rd, FPURegister fs, Register result = InvalidReg,
                 bool Inexact = false);

  // Convert single to unsigned word.
  void Trunc_uw_s(Register rd, FPURegister fs, Register result = InvalidReg,
                  bool Inexact = false);

  // Convert single to unsigned long.
  void Trunc_ul_s(Register rd, FPURegister fs, Register result = InvalidReg,
                  bool Inexact = false);

  // Convert singled to signed long.
  void Trunc_l_s(Register rd, FPURegister fs, Register result = InvalidReg,
                 bool Inexact = false);

  // Round double functions
  void Trunc_d_d(FPURegister fd, FPURegister fs, FPURegister fpu_scratch);
  void Round_d_d(FPURegister fd, FPURegister fs, FPURegister fpu_scratch);
  void Floor_d_d(FPURegister fd, FPURegister fs, FPURegister fpu_scratch);
  void Ceil_d_d(FPURegister fd, FPURegister fs, FPURegister fpu_scratch);

  // Round float functions
  void Trunc_s_s(FPURegister fd, FPURegister fs, FPURegister fpu_scratch);
  void Round_s_s(FPURegister fd, FPURegister fs, FPURegister fpu_scratch);
  void Floor_s_s(FPURegister fd, FPURegister fs, FPURegister fpu_scratch);
  void Ceil_s_s(FPURegister fd, FPURegister fs, FPURegister fpu_scratch);

  // Round single to signed word.
  void Round_w_s(Register rd, FPURegister fs, Register result = InvalidReg,
                 bool Inexact = false);

  // Round double to signed word.
  void Round_w_d(Register rd, FPURegister fs, Register result = InvalidReg,
                 bool Inexact = false);

  // Ceil single to signed word.
  void Ceil_w_s(Register rd, FPURegister fs, Register result = InvalidReg,
                bool Inexact = false);

  // Ceil double to signed word.
  void Ceil_w_d(Register rd, FPURegister fs, Register result = InvalidReg,
                bool Inexact = false);

  // Floor single to signed word.
  void Floor_w_s(Register rd, FPURegister fs, Register result = InvalidReg,
                 bool Inexact = false);

  // Floor double to signed word.
  void Floor_w_d(Register rd, FPURegister fs, Register result = InvalidReg,
                 bool Inexact = false);

  void Clz32(Register rd, Register rs);
  void Ctz32(Register rd, Register rs);
  void Popcnt32(Register rd, Register rs, Register scratch);

  void Popcnt64(Register rd, Register rs, Register scratch);
  void Ctz64(Register rd, Register rs);
  void Clz64(Register rd, Register rs);

  // Change endianness
  void ByteSwap(Register dest, Register src, int operand_size,
                Register scratch);

  void Ror(Register rd, Register rs, const Operand& rt);
  void Dror(Register rd, Register rs, const Operand& rt);

  void Float32Max(FPURegister dst, FPURegister src1, FPURegister src2);
  void Float32Min(FPURegister dst, FPURegister src1, FPURegister src2);
  void Float64Max(FPURegister dst, FPURegister src1, FPURegister src2);
  void Float64Min(FPURegister dst, FPURegister src1, FPURegister src2);

  template <typename F>
  void FloatMinMaxHelper(FPURegister dst, FPURegister src1, FPURegister src2,
                         MaxMinKind kind);

  inline void NegateBool(Register rd, Register rs) { xori(rd, rs, 1); }

 protected:
  void wasmLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase,
                    Register ptr, Register ptrScratch, AnyRegister output,
                    Register tmp);
  void wasmStoreImpl(const wasm::MemoryAccessDesc& access, AnyRegister value,
                     Register memoryBase, Register ptr, Register ptrScratch,
                     Register tmp);
};

class MacroAssemblerRiscv64Compat : public MacroAssemblerRiscv64 {
 public:
  using MacroAssemblerRiscv64::call;

  MacroAssemblerRiscv64Compat() {}

  void convertBoolToInt32(Register src, Register dest) {
    ma_and(dest, src, Imm32(0xff));
  };
  void convertInt32ToDouble(Register src, FloatRegister dest) {
    fcvt_d_w(dest, src);
  };
  void convertInt32ToDouble(const Address& src, FloatRegister dest) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    ma_load(scratch, src, SizeWord, SignExtend);
    fcvt_d_w(dest, scratch);
  };
  void convertInt32ToDouble(const BaseIndex& src, FloatRegister dest) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
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

  void computeEffectiveAddress(const Address& address, Register dest) {
    ma_add64(dest, address.base, Imm32(address.offset));
  }

  void computeEffectiveAddress(const BaseIndex& address, Register dest) {
    computeScaledAddress(address, dest);
    if (address.offset) {
      ma_add64(dest, dest, Imm32(address.offset));
    }
  }

  void j(Label* dest) { ma_branch(dest); }

  void mov(Register src, Register dest) { addi(dest, src, 0); }
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
    BlockTrampolinePoolScope block_trampoline_pool(this, 7);
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    BufferOffset bo = m_buffer.nextOffset();
    addPendingJump(bo, ImmPtr(c->raw()), RelocationKind::JITCODE);
    ma_liPatchable(scratch, ImmPtr(c->raw()));
    jr(scratch);
  }
  void branch(const Register reg) { jr(reg); }
  void ret() {
    ma_pop(ra);
    jalr(zero_reg, ra, 0);
  }
  inline void retn(Imm32 n);
  void push(Imm32 imm) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    ma_li(scratch, imm);
    ma_push(scratch);
  }
  void push(ImmWord imm) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    ma_li(scratch, imm);
    ma_push(scratch);
  }
  void push(ImmGCPtr imm) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    ma_li(scratch, imm);
    ma_push(scratch);
  }
  void push(const Address& address) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    loadPtr(address, scratch);
    ma_push(scratch);
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
    // Four instructions used in: MacroAssemblerRiscv64Compat::toggledCall
    return 7 * sizeof(uint32_t);
  }

  CodeOffset pushWithPatch(ImmWord imm) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    CodeOffset offset = movWithPatch(imm, scratch);
    ma_push(scratch);
    return offset;
  }

  CodeOffset movWithPatch(ImmWord imm, Register dest) {
    BlockTrampolinePoolScope block_trampoline_pool(this, 8);
    CodeOffset offset = CodeOffset(currentOffset());
    ma_liPatchable(dest, imm, Li64);
    return offset;
  }
  CodeOffset movWithPatch(ImmPtr imm, Register dest) {
    BlockTrampolinePoolScope block_trampoline_pool(this, 6);
    CodeOffset offset = CodeOffset(currentOffset());
    ma_liPatchable(dest, imm);
    return offset;
  }

  void writeCodePointer(CodeLabel* label) {
    label->patchAt()->bind(currentOffset());
    label->setLinkMode(CodeLabel::RawPointer);
    m_buffer.ensureSpace(sizeof(void*));
    emit(uint32_t(-1));
    emit(uint32_t(-1));
  }

  void jump(Label* label) { ma_branch(label); }
  void jump(Register reg) { jr(reg); }
  void jump(const Address& address) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    loadPtr(address, scratch);
    jr(scratch);
  }

  void jump(JitCode* code) { branch(code); }

  void jump(ImmPtr ptr) {
    BufferOffset bo = m_buffer.nextOffset();
    addPendingJump(bo, ptr, RelocationKind::HARDCODED);
    ma_jump(ptr);
  }

  void jump(TrampolinePtr code) { jump(ImmPtr(code.value)); }

  void splitTag(Register src, Register dest) {
    srli(dest, src, JSVAL_TAG_SHIFT);
  }

  void splitTag(const ValueOperand& operand, Register dest) {
    splitTag(operand.valueReg(), dest);
  }

  void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag) {
    splitTag(value, tag);
  }

  void moveIfZero(Register dst, Register src, Register cond) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(dst != scratch && cond != scratch);
    Label done;
    ma_branch(&done, NotEqual, cond, zero);
    mv(dst, src);
    bind(&done);
  }

  void moveIfNotZero(Register dst, Register src, Register cond) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(dst != scratch && cond != scratch);
    Label done;
    ma_branch(&done, Equal, cond, zero);
    mv(dst, src);
    bind(&done);
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
      slliw(dest, src, 0);
      return;
    }
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    MOZ_ASSERT(scratch != src);
    mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), scratch);
    xor_(dest, src, scratch);
  }

  template <typename T>
  void unboxObjectOrNull(const T& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    static_assert(JS::detail::ValueObjectOrNullBit ==
                  (uint64_t(0x8) << JSVAL_TAG_SHIFT));
    InsertBits(dest, zero, JSVAL_TAG_SHIFT + 3, 1);
  }

  void unboxGCThingForGCBarrier(const Address& src, Register dest) {
    loadPtr(src, dest);
    ExtractBits(dest, dest, 0, JSVAL_TAG_SHIFT - 1);
  }
  void unboxGCThingForGCBarrier(const ValueOperand& src, Register dest) {
    ExtractBits(dest, src.valueReg(), 0, JSVAL_TAG_SHIFT - 1);
  }

  void unboxWasmAnyRefGCThingForGCBarrier(const Address& src, Register dest) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(scratch != dest);
    movePtr(ImmWord(wasm::AnyRef::GCThingMask), scratch);
    loadPtr(src, dest);
    ma_and(dest, dest, scratch);
  }

  void getWasmAnyRefGCThingChunk(Register src, Register dest) {
    MOZ_ASSERT(src != dest);
    movePtr(ImmWord(wasm::AnyRef::GCThingChunkMask), dest);
    ma_and(dest, dest, src);
  }

  // Like unboxGCThingForGCBarrier, but loads the GC thing's chunk base.
  void getGCThingValueChunk(const Address& src, Register dest) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(scratch != dest);
    loadPtr(src, dest);
    movePtr(ImmWord(JS::detail::ValueGCThingPayloadChunkMask), scratch);
    and_(dest, dest, scratch);
  }
  void getGCThingValueChunk(const ValueOperand& src, Register dest) {
    MOZ_ASSERT(src.valueReg() != dest);
    movePtr(ImmWord(JS::detail::ValueGCThingPayloadChunkMask), dest);
    and_(dest, dest, src.valueReg());
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
    xori(val.valueReg(), val.valueReg(), 1);
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

  void ensureDouble(const ValueOperand& source, FloatRegister dest,
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

  // higher level tag testing code
  Address ToPayload(Address value) { return value; }

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
        UseScratchRegisterScope temps(this);
        Register scratch = temps.Acquire();
        Register scratch2 = temps.Acquire();
        if (type == JSVAL_TYPE_OBJECT) {
          unboxObjectOrNull(value, scratch2);
        } else {
          unboxNonDouble(value, scratch2, type);
        }
        computeEffectiveAddress(address, scratch);
        sd(scratch2, scratch, 0);
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
        UseScratchRegisterScope temps(this);
        Register scratch = temps.Acquire();
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

  void boxValue(JSValueType type, Register src, Register dest) {
    MOZ_ASSERT(src != dest);

    JSValueTag tag = (JSValueTag)JSVAL_TYPE_TO_TAG(type);
    ma_li(dest, Imm32(tag));
    slli(dest, dest, JSVAL_TAG_SHIFT);
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      InsertBits(dest, src, 0, 32);
    } else {
      InsertBits(dest, src, 0, JSVAL_TAG_SHIFT);
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
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      writeDataRelocation(val);
      movWithPatch(ImmWord(val.asRawBits()), scratch);
      push(scratch);
    } else {
      push(ImmWord(val.asRawBits()));
    }
  }
  void pushValue(JSValueType type, Register reg) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    boxValue(type, reg, scratch);
    push(scratch);
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

  void SignExtendByte(Register rd, Register rs) {
    slli(rd, rs, xlen - 8);
    srai(rd, rd, xlen - 8);
  }

  void SignExtendShort(Register rd, Register rs) {
    slli(rd, rs, xlen - 16);
    srai(rd, rd, xlen - 16);
  }

  void SignExtendWord(Register rd, Register rs) { sext_w(rd, rs); }
  void ZeroExtendWord(Register rd, Register rs) {
    slli(rd, rs, 32);
    srli(rd, rd, 32);
  }

  template <typename S>
  void load16UnalignedZeroExtend(const S& src, Register dest) {
    load16ZeroExtend(src, dest);
  }

  FaultingCodeOffset load32(const Address& address, Register dest);
  FaultingCodeOffset load32(const BaseIndex& address, Register dest);
  FaultingCodeOffset load32(AbsoluteAddress address, Register dest);
  FaultingCodeOffset load32(wasm::SymbolicAddress address, Register dest);

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

  FaultingCodeOffset loadDouble(const Address& addr, FloatRegister dest) {
    return ma_loadDouble(dest, addr);
  }
  FaultingCodeOffset loadDouble(const BaseIndex& src, FloatRegister dest) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    computeScaledAddress(src, scratch);
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    fld(dest, scratch, 0);
    return fco;
  }

  FaultingCodeOffset loadFloat32(const Address& addr, FloatRegister dest) {
    return ma_loadFloat(dest, addr);
  }

  FaultingCodeOffset loadFloat32(const BaseIndex& src, FloatRegister dest) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    computeScaledAddress(src, scratch);
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    flw(dest, scratch, 0);
    return fco;
  }

  template <typename S>
  FaultingCodeOffset load64Unaligned(const S& src, Register64 dest) {
    return load64(src, dest);
  }

  FaultingCodeOffset loadPtr(const Address& address, Register dest);
  FaultingCodeOffset loadPtr(const BaseIndex& src, Register dest);
  FaultingCodeOffset loadPtr(AbsoluteAddress address, Register dest);
  FaultingCodeOffset loadPtr(wasm::SymbolicAddress address, Register dest);

  FaultingCodeOffset loadPrivate(const Address& address, Register dest);

  FaultingCodeOffset store8(Register src, const Address& address);
  FaultingCodeOffset store8(Imm32 imm, const Address& address);
  FaultingCodeOffset store8(Register src, const BaseIndex& address);
  FaultingCodeOffset store8(Imm32 imm, const BaseIndex& address);

  FaultingCodeOffset store16(Register src, const Address& address);
  FaultingCodeOffset store16(Imm32 imm, const Address& address);
  FaultingCodeOffset store16(Register src, const BaseIndex& address);
  FaultingCodeOffset store16(Imm32 imm, const BaseIndex& address);

  template <typename T>
  FaultingCodeOffset store16Unaligned(Register src, const T& dest) {
    return store16(src, dest);
  }

  FaultingCodeOffset store32(Register src, AbsoluteAddress address);
  FaultingCodeOffset store32(Register src, const Address& address);
  FaultingCodeOffset store32(Register src, const BaseIndex& address);
  FaultingCodeOffset store32(Imm32 src, const Address& address);
  FaultingCodeOffset store32(Imm32 src, const BaseIndex& address);

  // NOTE: This will use second scratch on LOONG64. Only ARM needs the
  // implementation without second scratch.
  void store32_NoSecondScratch(Imm32 src, const Address& address) {
    store32(src, address);
  }

  template <typename T>
  void store32Unaligned(Register src, const T& dest) {
    store32(src, dest);
  }

  FaultingCodeOffset store64(Imm64 imm, Address address) {
    return storePtr(ImmWord(imm.value), address);
  }
  FaultingCodeOffset store64(Imm64 imm, const BaseIndex& address) {
    return storePtr(ImmWord(imm.value), address);
  }

  FaultingCodeOffset store64(Register64 src, Address address) {
    return storePtr(src.reg, address);
  }
  FaultingCodeOffset store64(Register64 src, const BaseIndex& address) {
    return storePtr(src.reg, address);
  }

  template <typename T>
  FaultingCodeOffset store64Unaligned(Register64 src, const T& dest) {
    return store64(src, dest);
  }

  template <typename T>
  FaultingCodeOffset storePtr(ImmWord imm, T address);
  template <typename T>
  FaultingCodeOffset storePtr(ImmPtr imm, T address);
  template <typename T>
  FaultingCodeOffset storePtr(ImmGCPtr imm, T address);
  FaultingCodeOffset storePtr(Register src, const Address& address);
  FaultingCodeOffset storePtr(Register src, const BaseIndex& address);
  FaultingCodeOffset storePtr(Register src, AbsoluteAddress dest);

  void moveDouble(FloatRegister src, FloatRegister dest) { fmv_d(dest, src); }

  void zeroDouble(FloatRegister reg) { fmv_d_x(reg, zero); }

  void convertUInt64ToDouble(Register src, FloatRegister dest);

  void breakpoint(uint32_t value = 0);

  void checkStackAlignment() {
#ifdef DEBUG
    Label aligned;
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    andi(scratch, sp, ABIStackAlignment - 1);
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
  void abiret() { jr(ra); }

  void moveFloat32(FloatRegister src, FloatRegister dest) { fmv_s(dest, src); }

  // Instrumentation for entering and leaving the profiler.
  void profilerEnterFrame(Register framePtr, Register scratch);
  void profilerExitFrame();
};

typedef MacroAssemblerRiscv64Compat MacroAssemblerSpecific;

}  // namespace jit
}  // namespace js

#endif /* jit_riscv64_MacroAssembler_riscv64_h */
