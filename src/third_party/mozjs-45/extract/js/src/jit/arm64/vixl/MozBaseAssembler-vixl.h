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

#include "jit/arm64/vixl/Constants-vixl.h"
#include "jit/arm64/vixl/Instructions-vixl.h"

#include "jit/shared/Assembler-shared.h"
#include "jit/shared/IonAssemblerBufferWithConstantPools.h"

namespace vixl {


using js::jit::BufferOffset;


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
  static const uint32_t BufferAlignmentFillInstruction = BRK | (0xdead << ImmException_offset);
  static const uint32_t BufferNopFillInstruction = HINT | (31 << Rt_offset);
  static const unsigned BufferNumDebugNopsToInsert = 0;

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
  { }

 public:
  // Helper function for use with the ARMBuffer.
  // The MacroAssembler must create an AutoJitContextAlloc before initializing the buffer.
  void initWithAllocator() {
    armbuffer_.initWithAllocator();
  }

  // Return the Instruction at a given byte offset.
  Instruction* getInstructionAt(BufferOffset offset) {
    return armbuffer_.getInst(offset);
  }

  // Return the byte offset of a bound label.
  template <typename T>
  inline T GetLabelByteOffset(const js::jit::Label* label) {
    VIXL_ASSERT(label->bound());
    JS_STATIC_ASSERT(sizeof(T) >= sizeof(uint32_t));
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
  BufferOffset allocEntry(size_t numInst, unsigned numPoolEntries,
                          uint8_t* inst, uint8_t* data,
                          ARMBuffer::PoolEntry* pe = nullptr,
                          bool markAsBranch = false)
  {
    BufferOffset offset = armbuffer_.allocEntry(numInst, numPoolEntries, inst,
                                                data, pe, markAsBranch);
    propagateOOM(offset.assigned());
    return offset;
  }

  // Emit the instruction, returning its offset.
  BufferOffset Emit(Instr instruction, bool isBranch = false) {
    JS_STATIC_ASSERT(sizeof(instruction) == kInstructionSize);
    return armbuffer_.putInt(*(uint32_t*)(&instruction), isBranch);
  }

  BufferOffset EmitBranch(Instr instruction) {
    return Emit(instruction, true);
  }

 public:
  // Emit the instruction at |at|.
  static void Emit(Instruction* at, Instr instruction) {
    JS_STATIC_ASSERT(sizeof(instruction) == kInstructionSize);
    memcpy(at, &instruction, sizeof(instruction));
  }

  static void EmitBranch(Instruction* at, Instr instruction) {
    // TODO: Assert that the buffer already has the instruction marked as a branch.
    Emit(at, instruction);
  }

  // Emit data inline in the instruction stream.
  BufferOffset EmitData(void const * data, unsigned size) {
    VIXL_ASSERT(size % 4 == 0);
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

  static ptrdiff_t GetBranchOffset(const Instruction* i);
  static void RetargetNearBranch(Instruction* i, int offset, Condition cond, bool final = true);
  static void RetargetNearBranch(Instruction* i, int offset, bool final = true);
  static void RetargetFarBranch(Instruction* i, uint8_t** slot, uint8_t* dest, Condition cond);

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

  js::jit::CompactBufferWriter jumpRelocations_;
  js::jit::CompactBufferWriter dataRelocations_;
  js::jit::CompactBufferWriter relocations_;
  js::jit::CompactBufferWriter preBarriers_;
};


}  // namespace vixl


#endif  // jit_arm64_vixl_MozBaseAssembler_vixl_h

