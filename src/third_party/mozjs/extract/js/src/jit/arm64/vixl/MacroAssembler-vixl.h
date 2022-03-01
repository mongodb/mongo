// Copyright 2015, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef VIXL_A64_MACRO_ASSEMBLER_A64_H_
#define VIXL_A64_MACRO_ASSEMBLER_A64_H_

#include <algorithm>
#include <limits>

#include "jit/arm64/Assembler-arm64.h"
#include "jit/arm64/vixl/Debugger-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Instrument-vixl.h"
#include "jit/arm64/vixl/Simulator-Constants-vixl.h"

#define LS_MACRO_LIST(V)                                      \
  V(Ldrb, Register&, rt, LDRB_w)                              \
  V(Strb, Register&, rt, STRB_w)                              \
  V(Ldrsb, Register&, rt, rt.Is64Bits() ? LDRSB_x : LDRSB_w)  \
  V(Ldrh, Register&, rt, LDRH_w)                              \
  V(Strh, Register&, rt, STRH_w)                              \
  V(Ldrsh, Register&, rt, rt.Is64Bits() ? LDRSH_x : LDRSH_w)  \
  V(Ldr, CPURegister&, rt, LoadOpFor(rt))                     \
  V(Str, CPURegister&, rt, StoreOpFor(rt))                    \
  V(Ldrsw, Register&, rt, LDRSW_x)


#define LSPAIR_MACRO_LIST(V)                              \
  V(Ldp, CPURegister&, rt, rt2, LoadPairOpFor(rt, rt2))   \
  V(Stp, CPURegister&, rt, rt2, StorePairOpFor(rt, rt2))  \
  V(Ldpsw, CPURegister&, rt, rt2, LDPSW_x)

namespace vixl {

// Forward declaration
class MacroAssembler;
class UseScratchRegisterScope;

// This scope has the following purposes:
//  * Acquire/Release the underlying assembler's code buffer.
//     * This is mandatory before emitting.
//  * Emit the literal or veneer pools if necessary before emitting the
//    macro-instruction.
//  * Ensure there is enough space to emit the macro-instruction.
class EmissionCheckScope {
 public:
  EmissionCheckScope(MacroAssembler* masm, size_t size)
    : masm_(masm)
  { }

 protected:
  MacroAssembler* masm_;
#ifdef DEBUG
  Label start_;
  size_t size_;
#endif
};


// Helper for common Emission checks.
// The macro-instruction maps to a single instruction.
class SingleEmissionCheckScope : public EmissionCheckScope {
 public:
  explicit SingleEmissionCheckScope(MacroAssembler* masm)
      : EmissionCheckScope(masm, kInstructionSize) {}
};


// The macro instruction is a "typical" macro-instruction. Typical macro-
// instruction only emit a few instructions, a few being defined as 8 here.
class MacroEmissionCheckScope : public EmissionCheckScope {
 public:
  explicit MacroEmissionCheckScope(MacroAssembler* masm)
      : EmissionCheckScope(masm, kTypicalMacroInstructionMaxSize) {}

 private:
  static const size_t kTypicalMacroInstructionMaxSize = 8 * kInstructionSize;
};


enum BranchType {
  // Copies of architectural conditions.
  // The associated conditions can be used in place of those, the code will
  // take care of reinterpreting them with the correct type.
  integer_eq = eq,
  integer_ne = ne,
  integer_hs = hs,
  integer_lo = lo,
  integer_mi = mi,
  integer_pl = pl,
  integer_vs = vs,
  integer_vc = vc,
  integer_hi = hi,
  integer_ls = ls,
  integer_ge = ge,
  integer_lt = lt,
  integer_gt = gt,
  integer_le = le,
  integer_al = al,
  integer_nv = nv,

  // These two are *different* from the architectural codes al and nv.
  // 'always' is used to generate unconditional branches.
  // 'never' is used to not generate a branch (generally as the inverse
  // branch type of 'always).
  always, never,
  // cbz and cbnz
  reg_zero, reg_not_zero,
  // tbz and tbnz
  reg_bit_clear, reg_bit_set,

  // Aliases.
  kBranchTypeFirstCondition = eq,
  kBranchTypeLastCondition = nv,
  kBranchTypeFirstUsingReg = reg_zero,
  kBranchTypeFirstUsingBit = reg_bit_clear
};


enum DiscardMoveMode { kDontDiscardForSameWReg, kDiscardForSameWReg };

// The macro assembler supports moving automatically pre-shifted immediates for
// arithmetic and logical instructions, and then applying a post shift in the
// instruction to undo the modification, in order to reduce the code emitted for
// an operation. For example:
//
//  Add(x0, x0, 0x1f7de) => movz x16, 0xfbef; add x0, x0, x16, lsl #1.
//
// This optimisation can be only partially applied when the stack pointer is an
// operand or destination, so this enumeration is used to control the shift.
enum PreShiftImmMode {
  kNoShift,          // Don't pre-shift.
  kLimitShiftForSP,  // Limit pre-shift for add/sub extend use.
  kAnyShift          // Allow any pre-shift.
};


class MacroAssembler : public js::jit::Assembler {
 public:
  MacroAssembler();

  // Finalize a code buffer of generated instructions. This function must be
  // called before executing or copying code from the buffer.
  void FinalizeCode();


  // Constant generation helpers.
  // These functions return the number of instructions required to move the
  // immediate into the destination register. Also, if the masm pointer is
  // non-null, it generates the code to do so.
  // The two features are implemented using one function to avoid duplication of
  // the logic.
  // The function can be used to evaluate the cost of synthesizing an
  // instruction using 'mov immediate' instructions. A user might prefer loading
  // a constant using the literal pool instead of using multiple 'mov immediate'
  // instructions.
  static int MoveImmediateHelper(MacroAssembler* masm,
                                 const Register &rd,
                                 uint64_t imm);
  static bool OneInstrMoveImmediateHelper(MacroAssembler* masm,
                                          const Register& dst,
                                          int64_t imm);


  // Logical macros.
  void And(const Register& rd,
           const Register& rn,
           const Operand& operand);
  void Ands(const Register& rd,
            const Register& rn,
            const Operand& operand);
  void Bic(const Register& rd,
           const Register& rn,
           const Operand& operand);
  void Bics(const Register& rd,
            const Register& rn,
            const Operand& operand);
  void Orr(const Register& rd,
           const Register& rn,
           const Operand& operand);
  void Orn(const Register& rd,
           const Register& rn,
           const Operand& operand);
  void Eor(const Register& rd,
           const Register& rn,
           const Operand& operand);
  void Eon(const Register& rd,
           const Register& rn,
           const Operand& operand);
  void Tst(const Register& rn, const Operand& operand);
  void LogicalMacro(const Register& rd,
                    const Register& rn,
                    const Operand& operand,
                    LogicalOp op);

  // Add and sub macros.
  void Add(const Register& rd,
           const Register& rn,
           const Operand& operand,
           FlagsUpdate S = LeaveFlags);
  void Adds(const Register& rd,
            const Register& rn,
            const Operand& operand);
  void Sub(const Register& rd,
           const Register& rn,
           const Operand& operand,
           FlagsUpdate S = LeaveFlags);
  void Subs(const Register& rd,
            const Register& rn,
            const Operand& operand);
  void Cmn(const Register& rn, const Operand& operand);
  void Cmp(const Register& rn, const Operand& operand);
  void Neg(const Register& rd,
           const Operand& operand);
  void Negs(const Register& rd,
            const Operand& operand);

  void AddSubMacro(const Register& rd,
                   const Register& rn,
                   const Operand& operand,
                   FlagsUpdate S,
                   AddSubOp op);

  // Add/sub with carry macros.
  void Adc(const Register& rd,
           const Register& rn,
           const Operand& operand);
  void Adcs(const Register& rd,
            const Register& rn,
            const Operand& operand);
  void Sbc(const Register& rd,
           const Register& rn,
           const Operand& operand);
  void Sbcs(const Register& rd,
            const Register& rn,
            const Operand& operand);
  void Ngc(const Register& rd,
           const Operand& operand);
  void Ngcs(const Register& rd,
            const Operand& operand);
  void AddSubWithCarryMacro(const Register& rd,
                            const Register& rn,
                            const Operand& operand,
                            FlagsUpdate S,
                            AddSubWithCarryOp op);

  // Move macros.
  void Mov(const Register& rd, uint64_t imm);
  void Mov(const Register& rd,
           const Operand& operand,
           DiscardMoveMode discard_mode = kDontDiscardForSameWReg);
  void Mvn(const Register& rd, uint64_t imm) {
    Mov(rd, (rd.size() == kXRegSize) ? ~imm : (~imm & kWRegMask));
  }
  void Mvn(const Register& rd, const Operand& operand);

  // Try to move an immediate into the destination register in a single
  // instruction. Returns true for success, and updates the contents of dst.
  // Returns false, otherwise.
  bool TryOneInstrMoveImmediate(const Register& dst, int64_t imm);

  // Move an immediate into register dst, and return an Operand object for
  // use with a subsequent instruction that accepts a shift. The value moved
  // into dst is not necessarily equal to imm; it may have had a shifting
  // operation applied to it that will be subsequently undone by the shift
  // applied in the Operand.
  Operand MoveImmediateForShiftedOp(const Register& dst,
		                    int64_t imm,
				    PreShiftImmMode mode);

  // Synthesises the address represented by a MemOperand into a register.
  void ComputeAddress(const Register& dst, const MemOperand& mem_op);

  // Conditional macros.
  void Ccmp(const Register& rn,
            const Operand& operand,
            StatusFlags nzcv,
            Condition cond);
  void Ccmn(const Register& rn,
            const Operand& operand,
            StatusFlags nzcv,
            Condition cond);
  void ConditionalCompareMacro(const Register& rn,
                               const Operand& operand,
                               StatusFlags nzcv,
                               Condition cond,
                               ConditionalCompareOp op);
  void Csel(const Register& rd,
            const Register& rn,
            const Operand& operand,
            Condition cond);

  // Load/store macros.
#define DECLARE_FUNCTION(FN, REGTYPE, REG, OP) \
  void FN(const REGTYPE REG, const MemOperand& addr);
  LS_MACRO_LIST(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION

  void LoadStoreMacro(const CPURegister& rt,
                      const MemOperand& addr,
                      LoadStoreOp op);

#define DECLARE_FUNCTION(FN, REGTYPE, REG, REG2, OP) \
  void FN(const REGTYPE REG, const REGTYPE REG2, const MemOperand& addr);
  LSPAIR_MACRO_LIST(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION

  void LoadStorePairMacro(const CPURegister& rt,
                          const CPURegister& rt2,
                          const MemOperand& addr,
                          LoadStorePairOp op);

  void Prfm(PrefetchOperation op, const MemOperand& addr);

  // Push or pop up to 4 registers of the same width to or from the stack,
  // using the current stack pointer as set by SetStackPointer.
  //
  // If an argument register is 'NoReg', all further arguments are also assumed
  // to be 'NoReg', and are thus not pushed or popped.
  //
  // Arguments are ordered such that "Push(a, b);" is functionally equivalent
  // to "Push(a); Push(b);".
  //
  // It is valid to push the same register more than once, and there is no
  // restriction on the order in which registers are specified.
  //
  // It is not valid to pop into the same register more than once in one
  // operation, not even into the zero register.
  //
  // If the current stack pointer (as set by SetStackPointer) is sp, then it
  // must be aligned to 16 bytes on entry and the total size of the specified
  // registers must also be a multiple of 16 bytes.
  //
  // Even if the current stack pointer is not the system stack pointer (sp),
  // Push (and derived methods) will still modify the system stack pointer in
  // order to comply with ABI rules about accessing memory below the system
  // stack pointer.
  //
  // Other than the registers passed into Pop, the stack pointer and (possibly)
  // the system stack pointer, these methods do not modify any other registers.
  void Push(const CPURegister& src0, const CPURegister& src1 = NoReg,
            const CPURegister& src2 = NoReg, const CPURegister& src3 = NoReg);
  void Pop(const CPURegister& dst0, const CPURegister& dst1 = NoReg,
           const CPURegister& dst2 = NoReg, const CPURegister& dst3 = NoReg);
  void PushStackPointer();

  // Alternative forms of Push and Pop, taking a RegList or CPURegList that
  // specifies the registers that are to be pushed or popped. Higher-numbered
  // registers are associated with higher memory addresses (as in the A32 push
  // and pop instructions).
  //
  // (Push|Pop)SizeRegList allow you to specify the register size as a
  // parameter. Only kXRegSize, kWRegSize, kDRegSize and kSRegSize are
  // supported.
  //
  // Otherwise, (Push|Pop)(CPU|X|W|D|S)RegList is preferred.
  void PushCPURegList(CPURegList registers);
  void PopCPURegList(CPURegList registers);

  void PushSizeRegList(RegList registers, unsigned reg_size,
      CPURegister::RegisterType type = CPURegister::kRegister) {
    PushCPURegList(CPURegList(type, reg_size, registers));
  }
  void PopSizeRegList(RegList registers, unsigned reg_size,
      CPURegister::RegisterType type = CPURegister::kRegister) {
    PopCPURegList(CPURegList(type, reg_size, registers));
  }
  void PushXRegList(RegList regs) {
    PushSizeRegList(regs, kXRegSize);
  }
  void PopXRegList(RegList regs) {
    PopSizeRegList(regs, kXRegSize);
  }
  void PushWRegList(RegList regs) {
    PushSizeRegList(regs, kWRegSize);
  }
  void PopWRegList(RegList regs) {
    PopSizeRegList(regs, kWRegSize);
  }
  void PushDRegList(RegList regs) {
    PushSizeRegList(regs, kDRegSize, CPURegister::kVRegister);
  }
  void PopDRegList(RegList regs) {
    PopSizeRegList(regs, kDRegSize, CPURegister::kVRegister);
  }
  void PushSRegList(RegList regs) {
    PushSizeRegList(regs, kSRegSize, CPURegister::kVRegister);
  }
  void PopSRegList(RegList regs) {
    PopSizeRegList(regs, kSRegSize, CPURegister::kVRegister);
  }

  // Push the specified register 'count' times.
  void PushMultipleTimes(int count, Register src);

  // Poke 'src' onto the stack. The offset is in bytes.
  //
  // If the current stack pointer (as set by SetStackPointer) is sp, then sp
  // must be aligned to 16 bytes.
  void Poke(const Register& src, const Operand& offset);

  // Peek at a value on the stack, and put it in 'dst'. The offset is in bytes.
  //
  // If the current stack pointer (as set by SetStackPointer) is sp, then sp
  // must be aligned to 16 bytes.
  void Peek(const Register& dst, const Operand& offset);

  // Alternative forms of Peek and Poke, taking a RegList or CPURegList that
  // specifies the registers that are to be pushed or popped. Higher-numbered
  // registers are associated with higher memory addresses.
  //
  // (Peek|Poke)SizeRegList allow you to specify the register size as a
  // parameter. Only kXRegSize, kWRegSize, kDRegSize and kSRegSize are
  // supported.
  //
  // Otherwise, (Peek|Poke)(CPU|X|W|D|S)RegList is preferred.
  void PeekCPURegList(CPURegList registers, int64_t offset) {
    LoadCPURegList(registers, MemOperand(StackPointer(), offset));
  }
  void PokeCPURegList(CPURegList registers, int64_t offset) {
    StoreCPURegList(registers, MemOperand(StackPointer(), offset));
  }

  void PeekSizeRegList(RegList registers, int64_t offset, unsigned reg_size,
      CPURegister::RegisterType type = CPURegister::kRegister) {
    PeekCPURegList(CPURegList(type, reg_size, registers), offset);
  }
  void PokeSizeRegList(RegList registers, int64_t offset, unsigned reg_size,
      CPURegister::RegisterType type = CPURegister::kRegister) {
    PokeCPURegList(CPURegList(type, reg_size, registers), offset);
  }
  void PeekXRegList(RegList regs, int64_t offset) {
    PeekSizeRegList(regs, offset, kXRegSize);
  }
  void PokeXRegList(RegList regs, int64_t offset) {
    PokeSizeRegList(regs, offset, kXRegSize);
  }
  void PeekWRegList(RegList regs, int64_t offset) {
    PeekSizeRegList(regs, offset, kWRegSize);
  }
  void PokeWRegList(RegList regs, int64_t offset) {
    PokeSizeRegList(regs, offset, kWRegSize);
  }
  void PeekDRegList(RegList regs, int64_t offset) {
    PeekSizeRegList(regs, offset, kDRegSize, CPURegister::kVRegister);
  }
  void PokeDRegList(RegList regs, int64_t offset) {
    PokeSizeRegList(regs, offset, kDRegSize, CPURegister::kVRegister);
  }
  void PeekSRegList(RegList regs, int64_t offset) {
    PeekSizeRegList(regs, offset, kSRegSize, CPURegister::kVRegister);
  }
  void PokeSRegList(RegList regs, int64_t offset) {
    PokeSizeRegList(regs, offset, kSRegSize, CPURegister::kVRegister);
  }


  // Claim or drop stack space without actually accessing memory.
  //
  // If the current stack pointer (as set by SetStackPointer) is sp, then it
  // must be aligned to 16 bytes and the size claimed or dropped must be a
  // multiple of 16 bytes.
  void Claim(const Operand& size);
  void Drop(const Operand& size);

  // Preserve the callee-saved registers (as defined by AAPCS64).
  //
  // Higher-numbered registers are pushed before lower-numbered registers, and
  // thus get higher addresses.
  // Floating-point registers are pushed before general-purpose registers, and
  // thus get higher addresses.
  //
  // This method must not be called unless StackPointer() is sp, and it is
  // aligned to 16 bytes.
  void PushCalleeSavedRegisters();

  // Restore the callee-saved registers (as defined by AAPCS64).
  //
  // Higher-numbered registers are popped after lower-numbered registers, and
  // thus come from higher addresses.
  // Floating-point registers are popped after general-purpose registers, and
  // thus come from higher addresses.
  //
  // This method must not be called unless StackPointer() is sp, and it is
  // aligned to 16 bytes.
  void PopCalleeSavedRegisters();

  void LoadCPURegList(CPURegList registers, const MemOperand& src);
  void StoreCPURegList(CPURegList registers, const MemOperand& dst);

  // Remaining instructions are simple pass-through calls to the assembler.
  void Adr(const Register& rd, Label* label) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    adr(rd, label);
  }
  void Adrp(const Register& rd, Label* label) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    adrp(rd, label);
  }
  void Asr(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    asr(rd, rn, shift);
  }
  void Asr(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    asrv(rd, rn, rm);
  }

  // Branch type inversion relies on these relations.
  VIXL_STATIC_ASSERT((reg_zero      == (reg_not_zero ^ 1)) &&
                     (reg_bit_clear == (reg_bit_set ^ 1)) &&
                     (always        == (never ^ 1)));

  BranchType InvertBranchType(BranchType type) {
    if (kBranchTypeFirstCondition <= type && type <= kBranchTypeLastCondition) {
      return static_cast<BranchType>(
          InvertCondition(static_cast<Condition>(type)));
    } else {
      return static_cast<BranchType>(type ^ 1);
    }
  }

  void B(Label* label, BranchType type, Register reg = NoReg, int bit = -1);

  void B(Label* label);
  void B(Label* label, Condition cond);
  void B(Condition cond, Label* label) {
    B(label, cond);
  }
  void Bfm(const Register& rd,
           const Register& rn,
           unsigned immr,
           unsigned imms) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    bfm(rd, rn, immr, imms);
  }
  void Bfi(const Register& rd,
           const Register& rn,
           unsigned lsb,
           unsigned width) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    bfi(rd, rn, lsb, width);
  }
  void Bfxil(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    bfxil(rd, rn, lsb, width);
  }
  void Bind(Label* label);
  // Bind a label to a specified offset from the start of the buffer.
  void BindToOffset(Label* label, ptrdiff_t offset);
  void Bl(Label* label) {
    SingleEmissionCheckScope guard(this);
    bl(label);
  }
  void Blr(const Register& xn) {
    VIXL_ASSERT(!xn.IsZero());
    SingleEmissionCheckScope guard(this);
    blr(xn);
  }
  void Br(const Register& xn) {
    VIXL_ASSERT(!xn.IsZero());
    SingleEmissionCheckScope guard(this);
    br(xn);
  }
  void Brk(int code = 0) {
    SingleEmissionCheckScope guard(this);
    brk(code);
  }
  void Cbnz(const Register& rt, Label* label);
  void Cbz(const Register& rt, Label* label);
  void Cinc(const Register& rd, const Register& rn, Condition cond) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    cinc(rd, rn, cond);
  }
  void Cinv(const Register& rd, const Register& rn, Condition cond) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    cinv(rd, rn, cond);
  }
  void Clrex() {
    SingleEmissionCheckScope guard(this);
    clrex();
  }
  void Cls(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    cls(rd, rn);
  }
  void Clz(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    clz(rd, rn);
  }
  void Cneg(const Register& rd, const Register& rn, Condition cond) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    cneg(rd, rn, cond);
  }
  void Cset(const Register& rd, Condition cond) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    cset(rd, cond);
  }
  void Csetm(const Register& rd, Condition cond) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    csetm(rd, cond);
  }
  void Csinc(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond) {
    VIXL_ASSERT(!rd.IsZero());
    // The VIXL source code contains these assertions, but the AArch64 ISR
    // explicitly permits the use of zero registers. CSET itself is defined
    // in terms of CSINC with WZR/XZR.
    //
    // VIXL_ASSERT(!rn.IsZero());
    // VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    csinc(rd, rn, rm, cond);
  }
  void Csinv(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    csinv(rd, rn, rm, cond);
  }
  void Csneg(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    csneg(rd, rn, rm, cond);
  }
  void Dmb(BarrierDomain domain, BarrierType type) {
    SingleEmissionCheckScope guard(this);
    dmb(domain, type);
  }
  void Dsb(BarrierDomain domain, BarrierType type) {
    SingleEmissionCheckScope guard(this);
    dsb(domain, type);
  }
  void Extr(const Register& rd,
            const Register& rn,
            const Register& rm,
            unsigned lsb) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    extr(rd, rn, rm, lsb);
  }
  void Fadd(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fadd(vd, vn, vm);
  }
  void Fccmp(const VRegister& vn,
             const VRegister& vm,
             StatusFlags nzcv,
             Condition cond,
             FPTrapFlags trap = DisableTrap) {
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    FPCCompareMacro(vn, vm, nzcv, cond, trap);
  }
  void Fccmpe(const VRegister& vn,
              const VRegister& vm,
              StatusFlags nzcv,
              Condition cond) {
    Fccmp(vn, vm, nzcv, cond, EnableTrap);
  }
  void Fcmp(const VRegister& vn, const VRegister& vm,
            FPTrapFlags trap = DisableTrap) {
    SingleEmissionCheckScope guard(this);
    FPCompareMacro(vn, vm, trap);
  }
  void Fcmp(const VRegister& vn, double value,
            FPTrapFlags trap = DisableTrap);
  void Fcmpe(const VRegister& vn, double value);
  void Fcmpe(const VRegister& vn, const VRegister& vm) {
    Fcmp(vn, vm, EnableTrap);
  }
  void Fcsel(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             Condition cond) {
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    fcsel(vd, vn, vm, cond);
  }
  void Fcvt(const VRegister& vd, const VRegister& vn) {
    SingleEmissionCheckScope guard(this);
    fcvt(vd, vn);
  }
  void Fcvtl(const VRegister& vd, const VRegister& vn) {
    SingleEmissionCheckScope guard(this);
    fcvtl(vd, vn);
  }
  void Fcvtl2(const VRegister& vd, const VRegister& vn) {
    SingleEmissionCheckScope guard(this);
    fcvtl2(vd, vn);
  }
  void Fcvtn(const VRegister& vd, const VRegister& vn) {
    SingleEmissionCheckScope guard(this);
    fcvtn(vd, vn);
  }
  void Fcvtn2(const VRegister& vd, const VRegister& vn) {
    SingleEmissionCheckScope guard(this);
    fcvtn2(vd, vn);
  }
  void Fcvtxn(const VRegister& vd, const VRegister& vn) {
    SingleEmissionCheckScope guard(this);
    fcvtxn(vd, vn);
  }
  void Fcvtxn2(const VRegister& vd, const VRegister& vn) {
    SingleEmissionCheckScope guard(this);
    fcvtxn2(vd, vn);
  }
  void Fcvtas(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtas(rd, vn);
  }
  void Fcvtau(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtau(rd, vn);
  }
  void Fcvtms(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtms(rd, vn);
  }
  void Fcvtmu(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtmu(rd, vn);
  }
  void Fcvtns(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtns(rd, vn);
  }
  void Fcvtnu(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtnu(rd, vn);
  }
  void Fcvtps(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtps(rd, vn);
  }
  void Fcvtpu(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtpu(rd, vn);
  }
  void Fcvtzs(const Register& rd, const VRegister& vn, int fbits = 0) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtzs(rd, vn, fbits);
  }
  void Fjcvtzs(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fjcvtzs(rd, vn);
  }
  void Fcvtzu(const Register& rd, const VRegister& vn, int fbits = 0) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtzu(rd, vn, fbits);
  }
  void Fdiv(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fdiv(vd, vn, vm);
  }
  void Fmax(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fmax(vd, vn, vm);
  }
  void Fmaxnm(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fmaxnm(vd, vn, vm);
  }
  void Fmin(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fmin(vd, vn, vm);
  }
  void Fminnm(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fminnm(vd, vn, vm);
  }
  void Fmov(VRegister vd, VRegister vn) {
    SingleEmissionCheckScope guard(this);
    // Only emit an instruction if vd and vn are different, and they are both D
    // registers. fmov(s0, s0) is not a no-op because it clears the top word of
    // d0. Technically, fmov(d0, d0) is not a no-op either because it clears
    // the top of q0, but VRegister does not currently support Q registers.
    if (!vd.Is(vn) || !vd.Is64Bits()) {
      fmov(vd, vn);
    }
  }
  void Fmov(VRegister vd, Register rn) {
    SingleEmissionCheckScope guard(this);
    fmov(vd, rn);
  }
  void Fmov(const VRegister& vd, int index, const Register& rn) {
    SingleEmissionCheckScope guard(this);
    fmov(vd, index, rn);
  }
  void Fmov(const Register& rd, const VRegister& vn, int index) {
    SingleEmissionCheckScope guard(this);
    fmov(rd, vn, index);
  }

  // Provide explicit double and float interfaces for FP immediate moves, rather
  // than relying on implicit C++ casts. This allows signalling NaNs to be
  // preserved when the immediate matches the format of vd. Most systems convert
  // signalling NaNs to quiet NaNs when converting between float and double.
  void Fmov(VRegister vd, double imm);
  void Fmov(VRegister vd, float imm);
  // Provide a template to allow other types to be converted automatically.
  template<typename T>
  void Fmov(VRegister vd, T imm) {
    Fmov(vd, static_cast<double>(imm));
  }
  void Fmov(Register rd, VRegister vn) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fmov(rd, vn);
  }
  void Fmul(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fmul(vd, vn, vm);
  }
  void Fnmul(const VRegister& vd, const VRegister& vn,
             const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fnmul(vd, vn, vm);
  }
  void Fmadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             const VRegister& va) {
    SingleEmissionCheckScope guard(this);
    fmadd(vd, vn, vm, va);
  }
  void Fmsub(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             const VRegister& va) {
    SingleEmissionCheckScope guard(this);
    fmsub(vd, vn, vm, va);
  }
  void Fnmadd(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              const VRegister& va) {
    SingleEmissionCheckScope guard(this);
    fnmadd(vd, vn, vm, va);
  }
  void Fnmsub(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              const VRegister& va) {
    SingleEmissionCheckScope guard(this);
    fnmsub(vd, vn, vm, va);
  }
  void Fsub(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    fsub(vd, vn, vm);
  }
  void Hint(SystemHint code) {
    SingleEmissionCheckScope guard(this);
    hint(code);
  }
  void Hlt(int code) {
    SingleEmissionCheckScope guard(this);
    hlt(code);
  }
  void Isb() {
    SingleEmissionCheckScope guard(this);
    isb();
  }
  void Ldar(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldar(rt, src);
  }
  void Ldarb(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldarb(rt, src);
  }
  void Ldarh(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldarh(rt, src);
  }
  void Ldaxp(const Register& rt, const Register& rt2, const MemOperand& src) {
    VIXL_ASSERT(!rt.Aliases(rt2));
    SingleEmissionCheckScope guard(this);
    ldaxp(rt, rt2, src);
  }
  void Ldaxr(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldaxr(rt, src);
  }
  void Ldaxrb(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldaxrb(rt, src);
  }
  void Ldaxrh(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldaxrh(rt, src);
  }
  void Ldnp(const CPURegister& rt,
            const CPURegister& rt2,
            const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldnp(rt, rt2, src);
  }
  // Provide both double and float interfaces for FP immediate loads, rather
  // than relying on implicit C++ casts. This allows signalling NaNs to be
  // preserved when the immediate matches the format of fd. Most systems convert
  // signalling NaNs to quiet NaNs when converting between float and double.
  void Ldr(const VRegister& vt, double imm) {
    SingleEmissionCheckScope guard(this);
    if (vt.Is64Bits()) {
      ldr(vt, imm);
    } else {
      ldr(vt, static_cast<float>(imm));
    }
  }
  void Ldr(const VRegister& vt, float imm) {
    SingleEmissionCheckScope guard(this);
    if (vt.Is32Bits()) {
      ldr(vt, imm);
    } else {
      ldr(vt, static_cast<double>(imm));
    }
  }
  /*
  void Ldr(const VRegister& vt, uint64_t high64, uint64_t low64) {
    VIXL_ASSERT(vt.IsQ());
    SingleEmissionCheckScope guard(this);
    ldr(vt, new Literal<uint64_t>(high64, low64,
                                  &literal_pool_,
                                  RawLiteral::kDeletedOnPlacementByPool));
  }
  */
  void Ldr(const Register& rt, uint64_t imm) {
    VIXL_ASSERT(!rt.IsZero());
    SingleEmissionCheckScope guard(this);
    ldr(rt, imm);
  }
  void Ldrsw(const Register& rt, uint32_t imm) {
    VIXL_ASSERT(!rt.IsZero());
    SingleEmissionCheckScope guard(this);
    ldrsw(rt, imm);
  }
  void Ldxp(const Register& rt, const Register& rt2, const MemOperand& src) {
    VIXL_ASSERT(!rt.Aliases(rt2));
    SingleEmissionCheckScope guard(this);
    ldxp(rt, rt2, src);
  }
  void Ldxr(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldxr(rt, src);
  }
  void Ldxrb(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldxrb(rt, src);
  }
  void Ldxrh(const Register& rt, const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ldxrh(rt, src);
  }
  void Lsl(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    lsl(rd, rn, shift);
  }
  void Lsl(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    lslv(rd, rn, rm);
  }
  void Lsr(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    lsr(rd, rn, shift);
  }
  void Lsr(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    lsrv(rd, rn, rm);
  }
  void Madd(const Register& rd,
            const Register& rn,
            const Register& rm,
            const Register& ra) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    madd(rd, rn, rm, ra);
  }
  void Mneg(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    mneg(rd, rn, rm);
  }
  void Mov(const Register& rd, const Register& rn) {
    SingleEmissionCheckScope guard(this);
    mov(rd, rn);
  }
  void Movk(const Register& rd, uint64_t imm, int shift = -1) {
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    movk(rd, imm, shift);
  }
  void Mrs(const Register& rt, SystemRegister sysreg) {
    VIXL_ASSERT(!rt.IsZero());
    SingleEmissionCheckScope guard(this);
    mrs(rt, sysreg);
  }
  void Msr(SystemRegister sysreg, const Register& rt) {
    VIXL_ASSERT(!rt.IsZero());
    SingleEmissionCheckScope guard(this);
    msr(sysreg, rt);
  }
  void Sys(int op1, int crn, int crm, int op2, const Register& rt = xzr) {
    SingleEmissionCheckScope guard(this);
    sys(op1, crn, crm, op2, rt);
  }
  void Dc(DataCacheOp op, const Register& rt) {
    SingleEmissionCheckScope guard(this);
    dc(op, rt);
  }
  void Ic(InstructionCacheOp op, const Register& rt) {
    SingleEmissionCheckScope guard(this);
    ic(op, rt);
  }
  void Msub(const Register& rd,
            const Register& rn,
            const Register& rm,
            const Register& ra) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    msub(rd, rn, rm, ra);
  }
  void Mul(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    mul(rd, rn, rm);
  }
  void Nop() {
    SingleEmissionCheckScope guard(this);
    nop();
  }
  void Csdb() {
    SingleEmissionCheckScope guard(this);
    csdb();
  }
  void Rbit(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rbit(rd, rn);
  }
  void Ret(const Register& xn = lr) {
    VIXL_ASSERT(!xn.IsZero());
    SingleEmissionCheckScope guard(this);
    ret(xn);
  }
  void Rev(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rev(rd, rn);
  }
  void Rev16(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rev16(rd, rn);
  }
  void Rev32(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rev32(rd, rn);
  }
  void Ror(const Register& rd, const Register& rs, unsigned shift) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rs.IsZero());
    SingleEmissionCheckScope guard(this);
    ror(rd, rs, shift);
  }
  void Ror(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    rorv(rd, rn, rm);
  }
  void Sbfiz(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sbfiz(rd, rn, lsb, width);
  }
  void Sbfm(const Register& rd,
            const Register& rn,
            unsigned immr,
            unsigned imms) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sbfm(rd, rn, immr, imms);
  }
  void Sbfx(const Register& rd,
            const Register& rn,
            unsigned lsb,
            unsigned width) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sbfx(rd, rn, lsb, width);
  }
  void Scvtf(const VRegister& vd, const Register& rn, int fbits = 0) {
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    scvtf(vd, rn, fbits);
  }
  void Sdiv(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    sdiv(rd, rn, rm);
  }
  void Smaddl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    smaddl(rd, rn, rm, ra);
  }
  void Smsubl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    smsubl(rd, rn, rm, ra);
  }
  void Smull(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    smull(rd, rn, rm);
  }
  void Smulh(const Register& xd, const Register& xn, const Register& xm) {
    VIXL_ASSERT(!xd.IsZero());
    VIXL_ASSERT(!xn.IsZero());
    VIXL_ASSERT(!xm.IsZero());
    SingleEmissionCheckScope guard(this);
    smulh(xd, xn, xm);
  }
  void Stlr(const Register& rt, const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    stlr(rt, dst);
  }
  void Stlrb(const Register& rt, const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    stlrb(rt, dst);
  }
  void Stlrh(const Register& rt, const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    stlrh(rt, dst);
  }
  void Stlxp(const Register& rs,
             const Register& rt,
             const Register& rt2,
             const MemOperand& dst) {
    VIXL_ASSERT(!rs.Aliases(dst.base()));
    VIXL_ASSERT(!rs.Aliases(rt));
    VIXL_ASSERT(!rs.Aliases(rt2));
    SingleEmissionCheckScope guard(this);
    stlxp(rs, rt, rt2, dst);
  }
  void Stlxr(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(!rs.Aliases(dst.base()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stlxr(rs, rt, dst);
  }
  void Stlxrb(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(!rs.Aliases(dst.base()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stlxrb(rs, rt, dst);
  }
  void Stlxrh(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(!rs.Aliases(dst.base()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stlxrh(rs, rt, dst);
  }
  void Stnp(const CPURegister& rt,
            const CPURegister& rt2,
            const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    stnp(rt, rt2, dst);
  }
  void Stxp(const Register& rs,
            const Register& rt,
            const Register& rt2,
            const MemOperand& dst) {
    VIXL_ASSERT(!rs.Aliases(dst.base()));
    VIXL_ASSERT(!rs.Aliases(rt));
    VIXL_ASSERT(!rs.Aliases(rt2));
    SingleEmissionCheckScope guard(this);
    stxp(rs, rt, rt2, dst);
  }
  void Stxr(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(!rs.Aliases(dst.base()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stxr(rs, rt, dst);
  }
  void Stxrb(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(!rs.Aliases(dst.base()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stxrb(rs, rt, dst);
  }
  void Stxrh(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(!rs.Aliases(dst.base()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stxrh(rs, rt, dst);
  }
  void Svc(int code) {
    SingleEmissionCheckScope guard(this);
    svc(code);
  }
  void Sxtb(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sxtb(rd, rn);
  }
  void Sxth(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sxth(rd, rn);
  }
  void Sxtw(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sxtw(rd, rn);
  }
  void Tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    tbl(vd, vn, vm);
  }
  void Tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    tbl(vd, vn, vn2, vm);
  }
  void Tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    tbl(vd, vn, vn2, vn3, vm);
  }
  void Tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vn4,
           const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    tbl(vd, vn, vn2, vn3, vn4, vm);
  }
  void Tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    tbx(vd, vn, vm);
  }
  void Tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    tbx(vd, vn, vn2, vm);
  }
  void Tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    tbx(vd, vn, vn2, vn3, vm);
  }
  void Tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vn4,
           const VRegister& vm) {
    SingleEmissionCheckScope guard(this);
    tbx(vd, vn, vn2, vn3, vn4, vm);
  }
  void Tbnz(const Register& rt, unsigned bit_pos, Label* label);
  void Tbz(const Register& rt, unsigned bit_pos, Label* label);
  void Ubfiz(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    ubfiz(rd, rn, lsb, width);
  }
  void Ubfm(const Register& rd,
            const Register& rn,
            unsigned immr,
            unsigned imms) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    ubfm(rd, rn, immr, imms);
  }
  void Ubfx(const Register& rd,
            const Register& rn,
            unsigned lsb,
            unsigned width) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    ubfx(rd, rn, lsb, width);
  }
  void Ucvtf(const VRegister& vd, const Register& rn, int fbits = 0) {
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    ucvtf(vd, rn, fbits);
  }
  void Udiv(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    udiv(rd, rn, rm);
  }
  void Umaddl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    umaddl(rd, rn, rm, ra);
  }
  void Umull(const Register& rd,
             const Register& rn,
             const Register& rm) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    umull(rd, rn, rm);
  }
  void Umulh(const Register& xd, const Register& xn, const Register& xm) {
    VIXL_ASSERT(!xd.IsZero());
    VIXL_ASSERT(!xn.IsZero());
    VIXL_ASSERT(!xm.IsZero());
    SingleEmissionCheckScope guard(this);
    umulh(xd, xn, xm);
  }
  void Umsubl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    umsubl(rd, rn, rm, ra);
  }

  void Unreachable() {
    SingleEmissionCheckScope guard(this);
    Emit(UNDEFINED_INST_PATTERN);
  }

  void Uxtb(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    uxtb(rd, rn);
  }
  void Uxth(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    uxth(rd, rn);
  }
  void Uxtw(const Register& rd, const Register& rn) {
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    uxtw(rd, rn);
  }

  // NEON 3 vector register instructions.
  #define NEON_3VREG_MACRO_LIST(V) \
    V(add, Add)                    \
    V(addhn, Addhn)                \
    V(addhn2, Addhn2)              \
    V(addp, Addp)                  \
    V(and_, And)                   \
    V(bic, Bic)                    \
    V(bif, Bif)                    \
    V(bit, Bit)                    \
    V(bsl, Bsl)                    \
    V(cmeq, Cmeq)                  \
    V(cmge, Cmge)                  \
    V(cmgt, Cmgt)                  \
    V(cmhi, Cmhi)                  \
    V(cmhs, Cmhs)                  \
    V(cmtst, Cmtst)                \
    V(eor, Eor)                    \
    V(fabd, Fabd)                  \
    V(facge, Facge)                \
    V(facgt, Facgt)                \
    V(faddp, Faddp)                \
    V(fcmeq, Fcmeq)                \
    V(fcmge, Fcmge)                \
    V(fcmgt, Fcmgt)                \
    V(fmaxnmp, Fmaxnmp)            \
    V(fmaxp, Fmaxp)                \
    V(fminnmp, Fminnmp)            \
    V(fminp, Fminp)                \
    V(fmla, Fmla)                  \
    V(fmls, Fmls)                  \
    V(fmulx, Fmulx)                \
    V(frecps, Frecps)              \
    V(frsqrts, Frsqrts)            \
    V(mla, Mla)                    \
    V(mls, Mls)                    \
    V(mul, Mul)                    \
    V(orn, Orn)                    \
    V(orr, Orr)                    \
    V(pmul, Pmul)                  \
    V(pmull, Pmull)                \
    V(pmull2, Pmull2)              \
    V(raddhn, Raddhn)              \
    V(raddhn2, Raddhn2)            \
    V(rsubhn, Rsubhn)              \
    V(rsubhn2, Rsubhn2)            \
    V(saba, Saba)                  \
    V(sabal, Sabal)                \
    V(sabal2, Sabal2)              \
    V(sabd, Sabd)                  \
    V(sabdl, Sabdl)                \
    V(sabdl2, Sabdl2)              \
    V(saddl, Saddl)                \
    V(saddl2, Saddl2)              \
    V(saddw, Saddw)                \
    V(saddw2, Saddw2)              \
    V(shadd, Shadd)                \
    V(shsub, Shsub)                \
    V(smax, Smax)                  \
    V(smaxp, Smaxp)                \
    V(smin, Smin)                  \
    V(sminp, Sminp)                \
    V(smlal, Smlal)                \
    V(smlal2, Smlal2)              \
    V(smlsl, Smlsl)                \
    V(smlsl2, Smlsl2)              \
    V(smull, Smull)                \
    V(smull2, Smull2)              \
    V(sqadd, Sqadd)                \
    V(sqdmlal, Sqdmlal)            \
    V(sqdmlal2, Sqdmlal2)          \
    V(sqdmlsl, Sqdmlsl)            \
    V(sqdmlsl2, Sqdmlsl2)          \
    V(sqdmulh, Sqdmulh)            \
    V(sqdmull, Sqdmull)            \
    V(sqdmull2, Sqdmull2)          \
    V(sqrdmulh, Sqrdmulh)          \
    V(sqrshl, Sqrshl)              \
    V(sqshl, Sqshl)                \
    V(sqsub, Sqsub)                \
    V(srhadd, Srhadd)              \
    V(srshl, Srshl)                \
    V(sshl, Sshl)                  \
    V(ssubl, Ssubl)                \
    V(ssubl2, Ssubl2)              \
    V(ssubw, Ssubw)                \
    V(ssubw2, Ssubw2)              \
    V(sub, Sub)                    \
    V(subhn, Subhn)                \
    V(subhn2, Subhn2)              \
    V(trn1, Trn1)                  \
    V(trn2, Trn2)                  \
    V(uaba, Uaba)                  \
    V(uabal, Uabal)                \
    V(uabal2, Uabal2)              \
    V(uabd, Uabd)                  \
    V(uabdl, Uabdl)                \
    V(uabdl2, Uabdl2)              \
    V(uaddl, Uaddl)                \
    V(uaddl2, Uaddl2)              \
    V(uaddw, Uaddw)                \
    V(uaddw2, Uaddw2)              \
    V(uhadd, Uhadd)                \
    V(uhsub, Uhsub)                \
    V(umax, Umax)                  \
    V(umaxp, Umaxp)                \
    V(umin, Umin)                  \
    V(uminp, Uminp)                \
    V(umlal, Umlal)                \
    V(umlal2, Umlal2)              \
    V(umlsl, Umlsl)                \
    V(umlsl2, Umlsl2)              \
    V(umull, Umull)                \
    V(umull2, Umull2)              \
    V(uqadd, Uqadd)                \
    V(uqrshl, Uqrshl)              \
    V(uqshl, Uqshl)                \
    V(uqsub, Uqsub)                \
    V(urhadd, Urhadd)              \
    V(urshl, Urshl)                \
    V(ushl, Ushl)                  \
    V(usubl, Usubl)                \
    V(usubl2, Usubl2)              \
    V(usubw, Usubw)                \
    V(usubw2, Usubw2)              \
    V(uzp1, Uzp1)                  \
    V(uzp2, Uzp2)                  \
    V(zip1, Zip1)                  \
    V(zip2, Zip2)

  #define DEFINE_MACRO_ASM_FUNC(ASM, MASM)   \
  void MASM(const VRegister& vd,             \
            const VRegister& vn,             \
            const VRegister& vm) {           \
    SingleEmissionCheckScope guard(this);    \
    ASM(vd, vn, vm);                         \
  }
  NEON_3VREG_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
  #undef DEFINE_MACRO_ASM_FUNC

  // NEON 2 vector register instructions.
  #define NEON_2VREG_MACRO_LIST(V) \
    V(abs,     Abs)                \
    V(addp,    Addp)               \
    V(addv,    Addv)               \
    V(cls,     Cls)                \
    V(clz,     Clz)                \
    V(cnt,     Cnt)                \
    V(fabs,    Fabs)               \
    V(faddp,   Faddp)              \
    V(fcvtas,  Fcvtas)             \
    V(fcvtau,  Fcvtau)             \
    V(fcvtms,  Fcvtms)             \
    V(fcvtmu,  Fcvtmu)             \
    V(fcvtns,  Fcvtns)             \
    V(fcvtnu,  Fcvtnu)             \
    V(fcvtps,  Fcvtps)             \
    V(fcvtpu,  Fcvtpu)             \
    V(fmaxnmp, Fmaxnmp)            \
    V(fmaxnmv, Fmaxnmv)            \
    V(fmaxp,   Fmaxp)              \
    V(fmaxv,   Fmaxv)              \
    V(fminnmp, Fminnmp)            \
    V(fminnmv, Fminnmv)            \
    V(fminp,   Fminp)              \
    V(fminv,   Fminv)              \
    V(fneg,    Fneg)               \
    V(frecpe,  Frecpe)             \
    V(frecpx,  Frecpx)             \
    V(frinta,  Frinta)             \
    V(frinti,  Frinti)             \
    V(frintm,  Frintm)             \
    V(frintn,  Frintn)             \
    V(frintp,  Frintp)             \
    V(frintx,  Frintx)             \
    V(frintz,  Frintz)             \
    V(frsqrte, Frsqrte)            \
    V(fsqrt,   Fsqrt)              \
    V(mov,     Mov)                \
    V(mvn,     Mvn)                \
    V(neg,     Neg)                \
    V(not_,    Not)                \
    V(rbit,    Rbit)               \
    V(rev16,   Rev16)              \
    V(rev32,   Rev32)              \
    V(rev64,   Rev64)              \
    V(sadalp,  Sadalp)             \
    V(saddlp,  Saddlp)             \
    V(saddlv,  Saddlv)             \
    V(smaxv,   Smaxv)              \
    V(sminv,   Sminv)              \
    V(sqabs,   Sqabs)              \
    V(sqneg,   Sqneg)              \
    V(sqxtn,   Sqxtn)              \
    V(sqxtn2,  Sqxtn2)             \
    V(sqxtun,  Sqxtun)             \
    V(sqxtun2, Sqxtun2)            \
    V(suqadd,  Suqadd)             \
    V(sxtl,    Sxtl)               \
    V(sxtl2,   Sxtl2)              \
    V(uadalp,  Uadalp)             \
    V(uaddlp,  Uaddlp)             \
    V(uaddlv,  Uaddlv)             \
    V(umaxv,   Umaxv)              \
    V(uminv,   Uminv)              \
    V(uqxtn,   Uqxtn)              \
    V(uqxtn2,  Uqxtn2)             \
    V(urecpe,  Urecpe)             \
    V(ursqrte, Ursqrte)            \
    V(usqadd,  Usqadd)             \
    V(uxtl,    Uxtl)               \
    V(uxtl2,   Uxtl2)              \
    V(xtn,     Xtn)                \
    V(xtn2,    Xtn2)

  #define DEFINE_MACRO_ASM_FUNC(ASM, MASM)   \
  void MASM(const VRegister& vd,             \
            const VRegister& vn) {           \
    SingleEmissionCheckScope guard(this);    \
    ASM(vd, vn);                             \
  }
  NEON_2VREG_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
  #undef DEFINE_MACRO_ASM_FUNC

  // NEON 2 vector register with immediate instructions.
  #define NEON_2VREG_FPIMM_MACRO_LIST(V) \
    V(fcmeq, Fcmeq)                      \
    V(fcmge, Fcmge)                      \
    V(fcmgt, Fcmgt)                      \
    V(fcmle, Fcmle)                      \
    V(fcmlt, Fcmlt)

  #define DEFINE_MACRO_ASM_FUNC(ASM, MASM)   \
  void MASM(const VRegister& vd,             \
            const VRegister& vn,             \
            double imm) {                    \
    SingleEmissionCheckScope guard(this);    \
    ASM(vd, vn, imm);                        \
  }
  NEON_2VREG_FPIMM_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
  #undef DEFINE_MACRO_ASM_FUNC

  // NEON by element instructions.
  #define NEON_BYELEMENT_MACRO_LIST(V) \
    V(fmul, Fmul)                      \
    V(fmla, Fmla)                      \
    V(fmls, Fmls)                      \
    V(fmulx, Fmulx)                    \
    V(mul, Mul)                        \
    V(mla, Mla)                        \
    V(mls, Mls)                        \
    V(sqdmulh, Sqdmulh)                \
    V(sqrdmulh, Sqrdmulh)              \
    V(sqdmull,  Sqdmull)               \
    V(sqdmull2, Sqdmull2)              \
    V(sqdmlal,  Sqdmlal)               \
    V(sqdmlal2, Sqdmlal2)              \
    V(sqdmlsl,  Sqdmlsl)               \
    V(sqdmlsl2, Sqdmlsl2)              \
    V(smull,  Smull)                   \
    V(smull2, Smull2)                  \
    V(smlal,  Smlal)                   \
    V(smlal2, Smlal2)                  \
    V(smlsl,  Smlsl)                   \
    V(smlsl2, Smlsl2)                  \
    V(umull,  Umull)                   \
    V(umull2, Umull2)                  \
    V(umlal,  Umlal)                   \
    V(umlal2, Umlal2)                  \
    V(umlsl,  Umlsl)                   \
    V(umlsl2, Umlsl2)

  #define DEFINE_MACRO_ASM_FUNC(ASM, MASM)   \
  void MASM(const VRegister& vd,             \
            const VRegister& vn,             \
            const VRegister& vm,             \
            int vm_index                     \
            ) {                              \
    SingleEmissionCheckScope guard(this);    \
    ASM(vd, vn, vm, vm_index);               \
  }
  NEON_BYELEMENT_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
  #undef DEFINE_MACRO_ASM_FUNC

  #define NEON_2VREG_SHIFT_MACRO_LIST(V) \
    V(rshrn,     Rshrn)                  \
    V(rshrn2,    Rshrn2)                 \
    V(shl,       Shl)                    \
    V(shll,      Shll)                   \
    V(shll2,     Shll2)                  \
    V(shrn,      Shrn)                   \
    V(shrn2,     Shrn2)                  \
    V(sli,       Sli)                    \
    V(sqrshrn,   Sqrshrn)                \
    V(sqrshrn2,  Sqrshrn2)               \
    V(sqrshrun,  Sqrshrun)               \
    V(sqrshrun2, Sqrshrun2)              \
    V(sqshl,     Sqshl)                  \
    V(sqshlu,    Sqshlu)                 \
    V(sqshrn,    Sqshrn)                 \
    V(sqshrn2,   Sqshrn2)                \
    V(sqshrun,   Sqshrun)                \
    V(sqshrun2,  Sqshrun2)               \
    V(sri,       Sri)                    \
    V(srshr,     Srshr)                  \
    V(srsra,     Srsra)                  \
    V(sshll,     Sshll)                  \
    V(sshll2,    Sshll2)                 \
    V(sshr,      Sshr)                   \
    V(ssra,      Ssra)                   \
    V(uqrshrn,   Uqrshrn)                \
    V(uqrshrn2,  Uqrshrn2)               \
    V(uqshl,     Uqshl)                  \
    V(uqshrn,    Uqshrn)                 \
    V(uqshrn2,   Uqshrn2)                \
    V(urshr,     Urshr)                  \
    V(ursra,     Ursra)                  \
    V(ushll,     Ushll)                  \
    V(ushll2,    Ushll2)                 \
    V(ushr,      Ushr)                   \
    V(usra,      Usra)                   \

  #define DEFINE_MACRO_ASM_FUNC(ASM, MASM)   \
  void MASM(const VRegister& vd,             \
            const VRegister& vn,             \
            int shift) {                     \
    SingleEmissionCheckScope guard(this);    \
    ASM(vd, vn, shift);                      \
  }
  NEON_2VREG_SHIFT_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
  #undef DEFINE_MACRO_ASM_FUNC

  void Bic(const VRegister& vd,
           const int imm8,
           const int left_shift = 0) {
    SingleEmissionCheckScope guard(this);
    bic(vd, imm8, left_shift);
  }
  void Cmeq(const VRegister& vd,
            const VRegister& vn,
            int imm) {
    SingleEmissionCheckScope guard(this);
    cmeq(vd, vn, imm);
  }
  void Cmge(const VRegister& vd,
            const VRegister& vn,
            int imm) {
    SingleEmissionCheckScope guard(this);
    cmge(vd, vn, imm);
  }
  void Cmgt(const VRegister& vd,
            const VRegister& vn,
            int imm) {
    SingleEmissionCheckScope guard(this);
    cmgt(vd, vn, imm);
  }
  void Cmle(const VRegister& vd,
            const VRegister& vn,
            int imm) {
    SingleEmissionCheckScope guard(this);
    cmle(vd, vn, imm);
  }
  void Cmlt(const VRegister& vd,
            const VRegister& vn,
            int imm) {
    SingleEmissionCheckScope guard(this);
    cmlt(vd, vn, imm);
  }
  void Dup(const VRegister& vd,
           const VRegister& vn,
           int index) {
    SingleEmissionCheckScope guard(this);
    dup(vd, vn, index);
  }
  void Dup(const VRegister& vd,
           const Register& rn) {
    SingleEmissionCheckScope guard(this);
    dup(vd, rn);
  }
  void Ext(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int index) {
    SingleEmissionCheckScope guard(this);
    ext(vd, vn, vm, index);
  }
  void Ins(const VRegister& vd,
           int vd_index,
           const VRegister& vn,
           int vn_index) {
    SingleEmissionCheckScope guard(this);
    ins(vd, vd_index, vn, vn_index);
  }
  void Ins(const VRegister& vd,
           int vd_index,
           const Register& rn) {
    SingleEmissionCheckScope guard(this);
    ins(vd, vd_index, rn);
  }
  void Ld1(const VRegister& vt,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld1(vt, src);
  }
  void Ld1(const VRegister& vt,
           const VRegister& vt2,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld1(vt, vt2, src);
  }
  void Ld1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld1(vt, vt2, vt3, src);
  }
  void Ld1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld1(vt, vt2, vt3, vt4, src);
  }
  void Ld1(const VRegister& vt,
           int lane,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld1(vt, lane, src);
  }
  void Ld1r(const VRegister& vt,
            const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld1r(vt, src);
  }
  void Ld2(const VRegister& vt,
           const VRegister& vt2,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld2(vt, vt2, src);
  }
  void Ld2(const VRegister& vt,
           const VRegister& vt2,
           int lane,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld2(vt, vt2, lane, src);
  }
  void Ld2r(const VRegister& vt,
            const VRegister& vt2,
            const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld2r(vt, vt2, src);
  }
  void Ld3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld3(vt, vt2, vt3, src);
  }
  void Ld3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           int lane,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld3(vt, vt2, vt3, lane, src);
  }
  void Ld3r(const VRegister& vt,
            const VRegister& vt2,
            const VRegister& vt3,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld3r(vt, vt2, vt3, src);
  }
  void Ld4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld4(vt, vt2, vt3, vt4, src);
  }
  void Ld4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           int lane,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld4(vt, vt2, vt3, vt4, lane, src);
  }
  void Ld4r(const VRegister& vt,
            const VRegister& vt2,
            const VRegister& vt3,
            const VRegister& vt4,
           const MemOperand& src) {
    SingleEmissionCheckScope guard(this);
    ld4r(vt, vt2, vt3, vt4, src);
  }
  void Mov(const VRegister& vd,
           int vd_index,
           const VRegister& vn,
           int vn_index) {
    SingleEmissionCheckScope guard(this);
    mov(vd, vd_index, vn, vn_index);
  }
  void Mov(const VRegister& vd,
           const VRegister& vn,
           int index) {
    SingleEmissionCheckScope guard(this);
    mov(vd, vn, index);
  }
  void Mov(const VRegister& vd,
           int vd_index,
           const Register& rn) {
    SingleEmissionCheckScope guard(this);
    mov(vd, vd_index, rn);
  }
  void Mov(const Register& rd,
           const VRegister& vn,
           int vn_index) {
    SingleEmissionCheckScope guard(this);
    mov(rd, vn, vn_index);
  }
  void Movi(const VRegister& vd,
            uint64_t imm,
            Shift shift = LSL,
            int shift_amount = 0);
  void Movi(const VRegister& vd, uint64_t hi, uint64_t lo);
  void Mvni(const VRegister& vd,
            const int imm8,
            Shift shift = LSL,
            const int shift_amount = 0) {
    SingleEmissionCheckScope guard(this);
    mvni(vd, imm8, shift, shift_amount);
  }
  void Orr(const VRegister& vd,
           const int imm8,
           const int left_shift = 0) {
    SingleEmissionCheckScope guard(this);
    orr(vd, imm8, left_shift);
  }
  void Scvtf(const VRegister& vd,
             const VRegister& vn,
             int fbits = 0) {
    SingleEmissionCheckScope guard(this);
    scvtf(vd, vn, fbits);
  }
  void Ucvtf(const VRegister& vd,
             const VRegister& vn,
             int fbits = 0) {
    SingleEmissionCheckScope guard(this);
    ucvtf(vd, vn, fbits);
  }
  void Fcvtzs(const VRegister& vd,
              const VRegister& vn,
              int fbits = 0) {
    SingleEmissionCheckScope guard(this);
    fcvtzs(vd, vn, fbits);
  }
  void Fcvtzu(const VRegister& vd,
              const VRegister& vn,
              int fbits = 0) {
    SingleEmissionCheckScope guard(this);
    fcvtzu(vd, vn, fbits);
  }
  void St1(const VRegister& vt,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st1(vt, dst);
  }
  void St1(const VRegister& vt,
           const VRegister& vt2,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st1(vt, vt2, dst);
  }
  void St1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st1(vt, vt2, vt3, dst);
  }
  void St1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st1(vt, vt2, vt3, vt4, dst);
  }
  void St1(const VRegister& vt,
           int lane,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st1(vt, lane, dst);
  }
  void St2(const VRegister& vt,
           const VRegister& vt2,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st2(vt, vt2, dst);
  }
  void St3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st3(vt, vt2, vt3, dst);
  }
  void St4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st4(vt, vt2, vt3, vt4, dst);
  }
  void St2(const VRegister& vt,
           const VRegister& vt2,
           int lane,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st2(vt, vt2, lane, dst);
  }
  void St3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           int lane,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st3(vt, vt2, vt3, lane, dst);
  }
  void St4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           int lane,
           const MemOperand& dst) {
    SingleEmissionCheckScope guard(this);
    st4(vt, vt2, vt3, vt4, lane, dst);
  }
  void Smov(const Register& rd,
            const VRegister& vn,
            int vn_index) {
    SingleEmissionCheckScope guard(this);
    smov(rd, vn, vn_index);
  }
  void Umov(const Register& rd,
            const VRegister& vn,
            int vn_index) {
    SingleEmissionCheckScope guard(this);
    umov(rd, vn, vn_index);
  }
  void Crc32b(const Register& rd,
              const Register& rn,
              const Register& rm) {
    SingleEmissionCheckScope guard(this);
    crc32b(rd, rn, rm);
  }
  void Crc32h(const Register& rd,
              const Register& rn,
              const Register& rm) {
    SingleEmissionCheckScope guard(this);
    crc32h(rd, rn, rm);
  }
  void Crc32w(const Register& rd,
              const Register& rn,
              const Register& rm) {
    SingleEmissionCheckScope guard(this);
    crc32w(rd, rn, rm);
  }
  void Crc32x(const Register& rd,
              const Register& rn,
              const Register& rm) {
    SingleEmissionCheckScope guard(this);
    crc32x(rd, rn, rm);
  }
  void Crc32cb(const Register& rd,
               const Register& rn,
               const Register& rm) {
    SingleEmissionCheckScope guard(this);
    crc32cb(rd, rn, rm);
  }
  void Crc32ch(const Register& rd,
               const Register& rn,
               const Register& rm) {
    SingleEmissionCheckScope guard(this);
    crc32ch(rd, rn, rm);
  }
  void Crc32cw(const Register& rd,
               const Register& rn,
               const Register& rm) {
    SingleEmissionCheckScope guard(this);
    crc32cw(rd, rn, rm);
  }
  void Crc32cx(const Register& rd,
               const Register& rn,
               const Register& rm) {
    SingleEmissionCheckScope guard(this);
    crc32cx(rd, rn, rm);
  }

  // Push the system stack pointer (sp) down to allow the same to be done to
  // the current stack pointer (according to StackPointer()). This must be
  // called _before_ accessing the memory.
  //
  // This is necessary when pushing or otherwise adding things to the stack, to
  // satisfy the AAPCS64 constraint that the memory below the system stack
  // pointer is not accessed.
  //
  // This method asserts that StackPointer() is not sp, since the call does
  // not make sense in that context.
  //
  // TODO: This method can only accept values of 'space' that can be encoded in
  // one instruction. Refer to the implementation for details.
  void BumpSystemStackPointer(const Operand& space);

  // Set the current stack pointer, but don't generate any code.
  void SetStackPointer64(const Register& stack_pointer) {
    VIXL_ASSERT(!TmpList()->IncludesAliasOf(stack_pointer));
    sp_ = stack_pointer;
  }

  // Return the current stack pointer, as set by SetStackPointer.
  const Register& StackPointer() const {
    return sp_;
  }

  const Register& GetStackPointer64() const {
    return sp_;
  }

  js::jit::RegisterOrSP getStackPointer() const {
      return js::jit::RegisterOrSP(sp_.code());
  }

  CPURegList* TmpList() { return &tmp_list_; }
  CPURegList* FPTmpList() { return &fptmp_list_; }

  // Trace control when running the debug simulator.
  //
  // For example:
  //
  // __ Trace(LOG_REGS, TRACE_ENABLE);
  // Will add registers to the trace if it wasn't already the case.
  //
  // __ Trace(LOG_DISASM, TRACE_DISABLE);
  // Will stop logging disassembly. It has no effect if the disassembly wasn't
  // already being logged.
  void Trace(TraceParameters parameters, TraceCommand command);

  // Log the requested data independently of what is being traced.
  //
  // For example:
  //
  // __ Log(LOG_FLAGS)
  // Will output the flags.
  void Log(TraceParameters parameters);

  // Enable or disable instrumentation when an Instrument visitor is attached to
  // the simulator.
  void EnableInstrumentation();
  void DisableInstrumentation();

  // Add a marker to the instrumentation data produced by an Instrument visitor.
  // The name is a two character string that will be attached to the marker in
  // the output data.
  void AnnotateInstrumentation(const char* marker_name);

 private:
  // The actual Push and Pop implementations. These don't generate any code
  // other than that required for the push or pop. This allows
  // (Push|Pop)CPURegList to bundle together setup code for a large block of
  // registers.
  //
  // Note that size is per register, and is specified in bytes.
  void PushHelper(int count, int size,
                  const CPURegister& src0, const CPURegister& src1,
                  const CPURegister& src2, const CPURegister& src3);
  void PopHelper(int count, int size,
                 const CPURegister& dst0, const CPURegister& dst1,
                 const CPURegister& dst2, const CPURegister& dst3);

  void Movi16bitHelper(const VRegister& vd, uint64_t imm);
  void Movi32bitHelper(const VRegister& vd, uint64_t imm);
  void Movi64bitHelper(const VRegister& vd, uint64_t imm);

  // Perform necessary maintenance operations before a push or pop.
  //
  // Note that size is per register, and is specified in bytes.
  void PrepareForPush(int count, int size);
  void PrepareForPop(int count, int size);

  // The actual implementation of load and store operations for CPURegList.
  enum LoadStoreCPURegListAction {
    kLoad,
    kStore
  };
  void LoadStoreCPURegListHelper(LoadStoreCPURegListAction operation,
                                 CPURegList registers,
                                 const MemOperand& mem);
  // Returns a MemOperand suitable for loading or storing a CPURegList at `dst`.
  // This helper may allocate registers from `scratch_scope` and generate code
  // to compute an intermediate address. The resulting MemOperand is only valid
  // as long as `scratch_scope` remains valid.
  MemOperand BaseMemOperandForLoadStoreCPURegList(
      const CPURegList& registers,
      const MemOperand& mem,
      UseScratchRegisterScope* scratch_scope);

  bool LabelIsOutOfRange(Label* label, ImmBranchType branch_type) {
    return !Instruction::IsValidImmPCOffset(branch_type, nextOffset().getOffset() - label->offset());
  }

  // The register to use as a stack pointer for stack operations.
  Register sp_;

  // Scratch registers available for use by the MacroAssembler.
  CPURegList tmp_list_;
  CPURegList fptmp_list_;

  ptrdiff_t checkpoint_;
  ptrdiff_t recommended_checkpoint_;
};


// All Assembler emits MUST acquire/release the underlying code buffer. The
// helper scope below will do so and optionally ensure the buffer is big enough
// to receive the emit. It is possible to request the scope not to perform any
// checks (kNoCheck) if for example it is known in advance the buffer size is
// adequate or there is some other size checking mechanism in place.
class CodeBufferCheckScope {
 public:
  // Tell whether or not the scope needs to ensure the associated CodeBuffer
  // has enough space for the requested size.
  enum CheckPolicy {
    kNoCheck,
    kCheck
  };

  // Tell whether or not the scope should assert the amount of code emitted
  // within the scope is consistent with the requested amount.
  enum AssertPolicy {
    kNoAssert,    // No assert required.
    kExactSize,   // The code emitted must be exactly size bytes.
    kMaximumSize  // The code emitted must be at most size bytes.
  };

  CodeBufferCheckScope(Assembler* assm,
                       size_t size,
                       CheckPolicy check_policy = kCheck,
                       AssertPolicy assert_policy = kMaximumSize)
  { }

  // This is a shortcut for CodeBufferCheckScope(assm, 0, kNoCheck, kNoAssert).
  explicit CodeBufferCheckScope(Assembler* assm) {}
};


// Use this scope when you need a one-to-one mapping between methods and
// instructions. This scope prevents the MacroAssembler from being called and
// literal pools from being emitted. It also asserts the number of instructions
// emitted is what you specified when creating the scope.
// FIXME: Because of the disabled calls below, this class asserts nothing.
class InstructionAccurateScope : public CodeBufferCheckScope {
 public:
  InstructionAccurateScope(MacroAssembler* masm,
                           int64_t count,
                           AssertPolicy policy = kExactSize)
      : CodeBufferCheckScope(masm,
                             (count * kInstructionSize),
                             kCheck,
                             policy) {
  }
};


// This scope utility allows scratch registers to be managed safely. The
// MacroAssembler's TmpList() (and FPTmpList()) is used as a pool of scratch
// registers. These registers can be allocated on demand, and will be returned
// at the end of the scope.
//
// When the scope ends, the MacroAssembler's lists will be restored to their
// original state, even if the lists were modified by some other means.
class UseScratchRegisterScope {
 public:
  // This constructor implicitly calls the `Open` function to initialise the
  // scope, so it is ready to use immediately after it has been constructed.
  explicit UseScratchRegisterScope(MacroAssembler* masm);
  // This constructor allows deferred and optional initialisation of the scope.
  // The user is required to explicitly call the `Open` function before using
  // the scope.
  UseScratchRegisterScope();
  // This function performs the actual initialisation work.
  void Open(MacroAssembler* masm);

  // The destructor always implicitly calls the `Close` function.
  ~UseScratchRegisterScope();
  // This function performs the cleaning-up work. It must succeed even if the
  // scope has not been opened. It is safe to call multiple times.
  void Close();


  bool IsAvailable(const CPURegister& reg) const;


  // Take a register from the appropriate temps list. It will be returned
  // automatically when the scope ends.
  Register AcquireW() { return AcquireNextAvailable(available_).W(); }
  Register AcquireX() { return AcquireNextAvailable(available_).X(); }
  VRegister AcquireS() { return AcquireNextAvailable(availablefp_).S(); }
  VRegister AcquireD() { return AcquireNextAvailable(availablefp_).D(); }
  VRegister AcquireQ() { return AcquireNextAvailable(availablefp_).Q(); }


  Register AcquireSameSizeAs(const Register& reg);
  VRegister AcquireSameSizeAs(const VRegister& reg);


  // Explicitly release an acquired (or excluded) register, putting it back in
  // the appropriate temps list.
  void Release(const CPURegister& reg);


  // Make the specified registers available as scratch registers for the
  // duration of this scope.
  void Include(const CPURegList& list);
  void Include(const Register& reg1,
               const Register& reg2 = NoReg,
               const Register& reg3 = NoReg,
               const Register& reg4 = NoReg);
  void Include(const VRegister& reg1,
               const VRegister& reg2 = NoVReg,
               const VRegister& reg3 = NoVReg,
               const VRegister& reg4 = NoVReg);


  // Make sure that the specified registers are not available in this scope.
  // This can be used to prevent helper functions from using sensitive
  // registers, for example.
  void Exclude(const CPURegList& list);
  void Exclude(const Register& reg1,
               const Register& reg2 = NoReg,
               const Register& reg3 = NoReg,
               const Register& reg4 = NoReg);
  void Exclude(const VRegister& reg1,
               const VRegister& reg2 = NoVReg,
               const VRegister& reg3 = NoVReg,
               const VRegister& reg4 = NoVReg);
  void Exclude(const CPURegister& reg1,
               const CPURegister& reg2 = NoCPUReg,
               const CPURegister& reg3 = NoCPUReg,
               const CPURegister& reg4 = NoCPUReg);


  // Prevent any scratch registers from being used in this scope.
  void ExcludeAll();


 private:
  static CPURegister AcquireNextAvailable(CPURegList* available);

  static void ReleaseByCode(CPURegList* available, int code);

  static void ReleaseByRegList(CPURegList* available,
                               RegList regs);

  static void IncludeByRegList(CPURegList* available,
                               RegList exclude);

  static void ExcludeByRegList(CPURegList* available,
                               RegList exclude);

  // Available scratch registers.
  CPURegList* available_;     // kRegister
  CPURegList* availablefp_;   // kVRegister

  // The state of the available lists at the start of this scope.
  RegList old_available_;     // kRegister
  RegList old_availablefp_;   // kVRegister
#ifdef DEBUG
  bool initialised_;
#endif

  // Disallow copy constructor and operator=.
  UseScratchRegisterScope(const UseScratchRegisterScope&) {
    VIXL_UNREACHABLE();
  }
  void operator=(const UseScratchRegisterScope&) {
    VIXL_UNREACHABLE();
  }
};


}  // namespace vixl

#endif  // VIXL_A64_MACRO_ASSEMBLER_A64_H_
