// Copyright 2013, ARM Limited
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

#include "jit/arm64/Architecture-arm64.h"
#include "jit/arm64/vixl/Assembler-vixl.h"
#include "jit/arm64/vixl/Instructions-vixl.h"

namespace vixl {

bool Instruction::IsUncondB() const {
  return Mask(UnconditionalBranchMask) == (UnconditionalBranchFixed | B);
}


bool Instruction::IsCondB() const {
  return Mask(ConditionalBranchMask) == (ConditionalBranchFixed | B_cond);
}


bool Instruction::IsBL() const {
  return Mask(UnconditionalBranchMask) == (UnconditionalBranchFixed | BL);
}


bool Instruction::IsBR() const {
  return Mask(UnconditionalBranchToRegisterMask) == (UnconditionalBranchToRegisterFixed | BR);
}


bool Instruction::IsBLR() const {
  return Mask(UnconditionalBranchToRegisterMask) == (UnconditionalBranchToRegisterFixed | BLR);
}


bool Instruction::IsTBZ() const {
  return Mask(TestBranchMask) == TBZ;
}


bool Instruction::IsTBNZ() const {
  return Mask(TestBranchMask) == TBNZ;
}


bool Instruction::IsCBZ() const {
  return Mask(CompareBranchMask) == CBZ_w || Mask(CompareBranchMask) == CBZ_x;
}


bool Instruction::IsCBNZ() const {
  return Mask(CompareBranchMask) == CBNZ_w || Mask(CompareBranchMask) == CBNZ_x;
}


bool Instruction::IsLDR() const {
  return Mask(LoadLiteralMask) == LDR_x_lit;
}


bool Instruction::IsNOP() const {
  return Mask(SystemHintMask) == HINT && ImmHint() == NOP;
}


bool Instruction::IsCSDB() const {
  return Mask(SystemHintMask) == HINT && ImmHint() == CSDB;
}


bool Instruction::IsADR() const {
  return Mask(PCRelAddressingMask) == ADR;
}


bool Instruction::IsADRP() const {
  return Mask(PCRelAddressingMask) == ADRP;
}


bool Instruction::IsMovz() const {
  return (Mask(MoveWideImmediateMask) == MOVZ_x) ||
         (Mask(MoveWideImmediateMask) == MOVZ_w);
}


bool Instruction::IsMovk() const {
  return (Mask(MoveWideImmediateMask) == MOVK_x) ||
         (Mask(MoveWideImmediateMask) == MOVK_w);
}

bool Instruction::IsBranchLinkImm() const {
  return Mask(UnconditionalBranchFMask) == (UnconditionalBranchFixed | BL);
}


bool Instruction::IsTargetReachable(Instruction* target) const {
    VIXL_ASSERT(((target - this) & 3) == 0);
    int offset = (target - this) >> kInstructionSizeLog2;
    switch (BranchType()) {
      case CondBranchType:
        return is_int19(offset);
      case UncondBranchType:
        return is_int26(offset);
      case CompareBranchType:
        return is_int19(offset);
      case TestBranchType:
        return is_int14(offset);
      default:
        VIXL_UNREACHABLE();
    }
}


ptrdiff_t Instruction::ImmPCRawOffset() const {
  ptrdiff_t offset;
  if (IsPCRelAddressing()) {
    // ADR and ADRP.
    offset = ImmPCRel();
  } else if (BranchType() == UnknownBranchType) {
    offset = ImmLLiteral();
  } else {
    offset = ImmBranch();
  }
  return offset;
}

void
Instruction::SetImmPCRawOffset(ptrdiff_t offset)
{
  if (IsPCRelAddressing()) {
    // ADR and ADRP. We're encoding a raw offset here.
    // See also SetPCRelImmTarget().
    Instr imm = vixl::Assembler::ImmPCRelAddress(offset);
    SetInstructionBits(Mask(~ImmPCRel_mask) | imm);
  } else {
    SetBranchImmTarget(this + (offset << kInstructionSizeLog2));
  }
}

// Is this a stack pointer synchronization instruction as inserted by
// MacroAssembler::syncStackPtr()?
bool
Instruction::IsStackPtrSync() const
{
    // The stack pointer sync is a move to the stack pointer.
    // This is encoded as 'add sp, Rs, #0'.
    return IsAddSubImmediate() && Rd() == js::jit::Registers::sp && ImmAddSub() == 0;
}

// Skip over a constant pool at |this| if there is one.
//
// If |this| is pointing to the artifical guard branch around a constant pool,
// return the instruction after the pool. Otherwise return |this| itself.
//
// This function does not skip constant pools with a natural guard branch. It
// is assumed that anyone inspecting the instruction stream understands about
// branches that were inserted naturally.
const Instruction*
Instruction::skipPool() const
{
    // Artificial pool guards can only be B (rather than BR), and they must be
    // forward branches.
    if (!IsUncondB() || ImmUncondBranch() <= 0)
        return this;

    // Check for a constant pool header which has the high 16 bits set. See
    // struct PoolHeader. Bit 15 indicates a natural pool guard when set. It
    // must be clear which indicates an artificial pool guard.
    const Instruction *header = InstructionAtOffset(kInstructionSize);
    if (header->Mask(0xffff8000) != 0xffff0000)
        return this;

    // OK, this is an artificial jump around a constant pool.
    return ImmPCOffsetTarget();
}


void Instruction::SetBits32(int msb, int lsb, unsigned value) {
  uint32_t me;
  memcpy(&me, this, sizeof(me));
  uint32_t new_mask = (1 << (msb+1)) - (1 << lsb);
  uint32_t keep_mask = ~new_mask;
  me = (me & keep_mask) | ((value << lsb) & new_mask);
  memcpy(this, &me, sizeof(me));
}


} // namespace vixl
