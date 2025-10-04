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

#ifndef jit_arm64_vixl_MozBaseAssembler_vixl_h
#define jit_arm64_vixl_MozBaseAssembler_vixl_h


#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Sprintf.h"     // SprintfLiteral

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t
#include <string.h>  // strstr

#include "jit/arm64/vixl/Constants-vixl.h"     // vixl::{HINT, NOP, ImmHint_offset}
#include "jit/arm64/vixl/Globals-vixl.h"       // VIXL_ASSERT
#include "jit/arm64/vixl/Instructions-vixl.h"  // vixl::{Instruction, NumShortBranchRangeTypes, Instr, ImmBranchRangeType}

#include "jit/Label.h"                       // jit::Label
#include "jit/shared/Assembler-shared.h"     // jit::AssemblerShared
#include "jit/shared/Disassembler-shared.h"  // jit::DisassemblerSpew
#include "jit/shared/IonAssemblerBuffer.h"   // jit::BufferOffset
#include "jit/shared/IonAssemblerBufferWithConstantPools.h"  // jit::AssemblerBufferWithConstantPools

namespace vixl {


using js::jit::BufferOffset;
using js::jit::DisassemblerSpew;
using js::jit::Label;

using LabelDoc = DisassemblerSpew::LabelDoc;
using LiteralDoc = DisassemblerSpew::LiteralDoc;

#ifdef JS_DISASM_ARM64
void DisassembleInstruction(char* buffer, size_t bufsize, const Instruction* instr);
#endif

class MozBaseAssembler;
typedef js::jit::AssemblerBufferWithConstantPools<1024, 4, Instruction, MozBaseAssembler,
                                                  NumShortBranchRangeTypes> ARMBuffer;

// Base class for vixl::Assembler, for isolating Moz-specific changes to VIXL.
class MozBaseAssembler : public js::jit::AssemblerShared {
  // Buffer initialization constants.
  static const unsigned BufferGuardSize = 1;
  static const unsigned BufferHeaderSize = 1;
  static const size_t   BufferCodeAlignment = 8;
  static const size_t   BufferMaxPoolOffset = 1024;
  static const unsigned BufferPCBias = 0;
  static const uint32_t BufferAlignmentFillInstruction = HINT | (NOP << ImmHint_offset);
  static const uint32_t BufferNopFillInstruction = HINT | (NOP << ImmHint_offset);
  static const unsigned BufferNumDebugNopsToInsert = 0;

#ifdef JS_DISASM_ARM64
  static constexpr const char* const InstrIndent = "        ";
  static constexpr const char* const LabelIndent = "                 ";
  static constexpr const char* const TargetIndent = "                    ";
#endif

 public:
  MozBaseAssembler()
    : armbuffer_(BufferGuardSize,
                 BufferHeaderSize,
                 BufferCodeAlignment,
                 BufferMaxPoolOffset,
                 BufferPCBias,
                 BufferAlignmentFillInstruction,
                 BufferNopFillInstruction,
                 BufferNumDebugNopsToInsert)
  {
#ifdef JS_DISASM_ARM64
      spew_.setLabelIndent(LabelIndent);
      spew_.setTargetIndent(TargetIndent);
#endif
}
  ~MozBaseAssembler()
  {
#ifdef JS_DISASM_ARM64
      spew_.spewOrphans();
#endif
  }

 public:
  // Return the Instruction at a given byte offset.
  Instruction* getInstructionAt(BufferOffset offset) {
    return armbuffer_.getInst(offset);
  }

  // Return the byte offset of a bound label.
  template <typename T>
  inline T GetLabelByteOffset(const js::jit::Label* label) {
    VIXL_ASSERT(label->bound());
    static_assert(sizeof(T) >= sizeof(uint32_t));
    return reinterpret_cast<T>(label->offset());
  }

 protected:
  // Get the buffer offset of the next inserted instruction. This may flush
  // constant pools.
  BufferOffset nextInstrOffset() {
    return armbuffer_.nextInstrOffset();
  }

  // Get the next usable buffer offset. Note that a constant pool may be placed
  // here before the next instruction is emitted.
  BufferOffset nextOffset() const {
    return armbuffer_.nextOffset();
  }

  // Allocate memory in the buffer by forwarding to armbuffer_.
  // Propagate OOM errors.
  BufferOffset allocLiteralLoadEntry(size_t numInst, unsigned numPoolEntries,
				     uint8_t* inst, uint8_t* data,
				     const LiteralDoc& doc = LiteralDoc(),
				     ARMBuffer::PoolEntry* pe = nullptr)
  {
    MOZ_ASSERT(inst);
    MOZ_ASSERT(numInst == 1);	/* If not, then fix disassembly */
    BufferOffset offset = armbuffer_.allocEntry(numInst, numPoolEntries, inst,
                                                data, pe);
    propagateOOM(offset.assigned());
#ifdef JS_DISASM_ARM64
    Instruction* instruction = armbuffer_.getInstOrNull(offset);
    if (instruction)
        spewLiteralLoad(offset,
                        reinterpret_cast<vixl::Instruction*>(instruction), doc);
#endif
    return offset;
  }

#ifdef JS_DISASM_ARM64
  DisassemblerSpew spew_;

  void spew(BufferOffset offs, const vixl::Instruction* instr) {
    if (spew_.isDisabled() || !instr)
      return;

    char buffer[2048];
    DisassembleInstruction(buffer, sizeof(buffer), instr);
    spew_.spew("%06" PRIx32 " %08" PRIx32 "%s%s",
               (uint32_t)offs.getOffset(),
               instr->InstructionBits(), InstrIndent, buffer);
  }

  void spewBranch(BufferOffset offs,
                  const vixl::Instruction* instr, const LabelDoc& target) {
    if (spew_.isDisabled() || !instr)
      return;

    char buffer[2048];
    DisassembleInstruction(buffer, sizeof(buffer), instr);

    char labelBuf[128];
    labelBuf[0] = 0;

    bool hasTarget = target.valid;
    if (!hasTarget)
      SprintfLiteral(labelBuf, "-> (link-time target)");

    if (instr->IsImmBranch() && hasTarget) {
      // The target information in the instruction is likely garbage, so remove it.
      // The target label will in any case be printed if we have it.
      //
      // The format of the instruction disassembly is /.*#.*/.  Strip the # and later.
      size_t i;
      const size_t BUFLEN = sizeof(buffer)-1;
      for ( i=0 ; i < BUFLEN && buffer[i] && buffer[i] != '#' ; i++ )
	;
      buffer[i] = 0;

      SprintfLiteral(labelBuf, "-> %d%s", target.doc, !target.bound ? "f" : "");
      hasTarget = false;
    }

    spew_.spew("%06" PRIx32 " %08" PRIx32 "%s%s%s",
               (uint32_t)offs.getOffset(),
               instr->InstructionBits(), InstrIndent, buffer, labelBuf);

    if (hasTarget)
      spew_.spewRef(target);
  }

  void spewLiteralLoad(BufferOffset offs,
                       const vixl::Instruction* instr, const LiteralDoc& doc) {
    if (spew_.isDisabled() || !instr)
      return;

    char buffer[2048];
    DisassembleInstruction(buffer, sizeof(buffer), instr);

    char litbuf[2048];
    spew_.formatLiteral(doc, litbuf, sizeof(litbuf));

    // The instruction will have the form /^.*pc\+0/ followed by junk that we
    // don't need; try to strip it.

    char *probe = strstr(buffer, "pc+0");
    if (probe)
      *(probe + 4) = 0;
    spew_.spew("%06" PRIx32 " %08" PRIx32 "%s%s    ; .const %s",
               (uint32_t)offs.getOffset(),
               instr->InstructionBits(), InstrIndent, buffer, litbuf);
  }

  LabelDoc refLabel(Label* label) {
    if (spew_.isDisabled())
      return LabelDoc();

    return spew_.refLabel(label);
  }
#else
  LabelDoc refLabel(js::jit::Label*) {
      return LabelDoc();
  }
#endif

  // Emit the instruction, returning its offset.
  BufferOffset Emit(Instr instruction, bool isBranch = false) {
    static_assert(sizeof(instruction) == kInstructionSize);
    // TODO: isBranch is obsolete and should be removed.
    (void)isBranch;
    MOZ_ASSERT(hasCreator());
    BufferOffset offs = armbuffer_.putInt(*(uint32_t*)(&instruction));
#ifdef JS_DISASM_ARM64
    if (!isBranch)
        spew(offs, armbuffer_.getInstOrNull(offs));
#endif
    return offs;
  }

  BufferOffset EmitBranch(Instr instruction, const LabelDoc& doc) {
    BufferOffset offs = Emit(instruction, true);
#ifdef JS_DISASM_ARM64
    spewBranch(offs, armbuffer_.getInstOrNull(offs), doc);
#endif
    return offs;
  }

 public:
  // Emit the instruction at |at|.
  static void Emit(Instruction* at, Instr instruction) {
    static_assert(sizeof(instruction) == kInstructionSize);
    memcpy(at, &instruction, sizeof(instruction));
  }

  static void EmitBranch(Instruction* at, Instr instruction) {
    // TODO: Assert that the buffer already has the instruction marked as a branch.
    Emit(at, instruction);
  }

  // Emit data inline in the instruction stream.
  BufferOffset EmitData(void const * data, unsigned size) {
    VIXL_ASSERT(size % 4 == 0);
    MOZ_ASSERT(hasCreator());
    return armbuffer_.allocEntry(size / sizeof(uint32_t), 0, (uint8_t*)(data), nullptr);
  }

 public:
  // Size of the code generated in bytes, including pools.
  size_t SizeOfCodeGenerated() const {
    return armbuffer_.size();
  }

  // Move the pool into the instruction stream.
  void flushBuffer() {
    armbuffer_.flushPool();
  }

  // Inhibit pool flushing for the given number of instructions.
  // Generating more than |maxInst| instructions in a no-pool region
  // triggers an assertion within the ARMBuffer.
  // Does not nest.
  void enterNoPool(size_t maxInst) {
    armbuffer_.enterNoPool(maxInst);
  }

  // Marks the end of a no-pool region.
  void leaveNoPool() {
    armbuffer_.leaveNoPool();
  }

  void enterNoNops() {
    armbuffer_.enterNoNops();
  }
  void leaveNoNops() {
    armbuffer_.leaveNoNops();
  }

 public:
  // Static interface used by IonAssemblerBufferWithConstantPools.
  static void InsertIndexIntoTag(uint8_t* load, uint32_t index);
  static bool PatchConstantPoolLoad(void* loadAddr, void* constPoolAddr);
  static void PatchShortRangeBranchToVeneer(ARMBuffer*, unsigned rangeIdx, BufferOffset deadline,
                                            BufferOffset veneer);
  static uint32_t PlaceConstantPoolBarrier(int offset);

  static void WritePoolHeader(uint8_t* start, js::jit::Pool* p, bool isNatural);
  static void WritePoolFooter(uint8_t* start, js::jit::Pool* p, bool isNatural);
  static void WritePoolGuard(BufferOffset branch, Instruction* inst, BufferOffset dest);

 protected:
  // Functions for managing Labels and linked lists of Label uses.

  // Get the next Label user in the linked list of Label uses.
  // Return an unassigned BufferOffset when the end of the list is reached.
  BufferOffset NextLink(BufferOffset cur);

  // Patch the instruction at cur to link to the instruction at next.
  void SetNextLink(BufferOffset cur, BufferOffset next);

  // Link the current (not-yet-emitted) instruction to the specified label,
  // then return a raw offset to be encoded in the instruction.
  ptrdiff_t LinkAndGetByteOffsetTo(BufferOffset branch, js::jit::Label* label);
  ptrdiff_t LinkAndGetInstructionOffsetTo(BufferOffset branch, ImmBranchRangeType branchRange,
                                          js::jit::Label* label);
  ptrdiff_t LinkAndGetPageOffsetTo(BufferOffset branch, js::jit::Label* label);

  // A common implementation for the LinkAndGet<Type>OffsetTo helpers.
  ptrdiff_t LinkAndGetOffsetTo(BufferOffset branch, ImmBranchRangeType branchRange,
                               unsigned elementSizeBits, js::jit::Label* label);

 protected:
  // The buffer into which code and relocation info are generated.
  ARMBuffer armbuffer_;
};


}  // namespace vixl


#endif  // jit_arm64_vixl_MozBaseAssembler_vixl_h

