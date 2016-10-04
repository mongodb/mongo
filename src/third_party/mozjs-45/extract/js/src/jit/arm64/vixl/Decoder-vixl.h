// Copyright 2014, ARM Limited
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

#ifndef VIXL_A64_DECODER_A64_H_
#define VIXL_A64_DECODER_A64_H_

#include "mozilla/Vector.h"

#include "jsalloc.h"

#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Instructions-vixl.h"


// List macro containing all visitors needed by the decoder class.

#define VISITOR_LIST(V)             \
  V(PCRelAddressing)                \
  V(AddSubImmediate)                \
  V(LogicalImmediate)               \
  V(MoveWideImmediate)              \
  V(Bitfield)                       \
  V(Extract)                        \
  V(UnconditionalBranch)            \
  V(UnconditionalBranchToRegister)  \
  V(CompareBranch)                  \
  V(TestBranch)                     \
  V(ConditionalBranch)              \
  V(System)                         \
  V(Exception)                      \
  V(LoadStorePairPostIndex)         \
  V(LoadStorePairOffset)            \
  V(LoadStorePairPreIndex)          \
  V(LoadStorePairNonTemporal)       \
  V(LoadLiteral)                    \
  V(LoadStoreUnscaledOffset)        \
  V(LoadStorePostIndex)             \
  V(LoadStorePreIndex)              \
  V(LoadStoreRegisterOffset)        \
  V(LoadStoreUnsignedOffset)        \
  V(LoadStoreExclusive)             \
  V(LogicalShifted)                 \
  V(AddSubShifted)                  \
  V(AddSubExtended)                 \
  V(AddSubWithCarry)                \
  V(ConditionalCompareRegister)     \
  V(ConditionalCompareImmediate)    \
  V(ConditionalSelect)              \
  V(DataProcessing1Source)          \
  V(DataProcessing2Source)          \
  V(DataProcessing3Source)          \
  V(FPCompare)                      \
  V(FPConditionalCompare)           \
  V(FPConditionalSelect)            \
  V(FPImmediate)                    \
  V(FPDataProcessing1Source)        \
  V(FPDataProcessing2Source)        \
  V(FPDataProcessing3Source)        \
  V(FPIntegerConvert)               \
  V(FPFixedPointConvert)            \
  V(Crypto2RegSHA)                  \
  V(Crypto3RegSHA)                  \
  V(CryptoAES)                      \
  V(NEON2RegMisc)                   \
  V(NEON3Different)                 \
  V(NEON3Same)                      \
  V(NEONAcrossLanes)                \
  V(NEONByIndexedElement)           \
  V(NEONCopy)                       \
  V(NEONExtract)                    \
  V(NEONLoadStoreMultiStruct)       \
  V(NEONLoadStoreMultiStructPostIndex)  \
  V(NEONLoadStoreSingleStruct)      \
  V(NEONLoadStoreSingleStructPostIndex) \
  V(NEONModifiedImmediate)          \
  V(NEONScalar2RegMisc)             \
  V(NEONScalar3Diff)                \
  V(NEONScalar3Same)                \
  V(NEONScalarByIndexedElement)     \
  V(NEONScalarCopy)                 \
  V(NEONScalarPairwise)             \
  V(NEONScalarShiftImmediate)       \
  V(NEONShiftImmediate)             \
  V(NEONTable)                      \
  V(NEONPerm)                       \
  V(Unallocated)                    \
  V(Unimplemented)

namespace vixl {

// The Visitor interface. Disassembler and simulator (and other tools)
// must provide implementations for all of these functions.
class DecoderVisitor {
 public:
  enum VisitorConstness {
    kConstVisitor,
    kNonConstVisitor
  };
  explicit DecoderVisitor(VisitorConstness constness = kConstVisitor)
      : constness_(constness) {}

  virtual ~DecoderVisitor() {}

  #define DECLARE(A) virtual void Visit##A(const Instruction* instr) = 0;
  VISITOR_LIST(DECLARE)
  #undef DECLARE

  bool IsConstVisitor() const { return constness_ == kConstVisitor; }
  Instruction* MutableInstruction(const Instruction* instr) {
    VIXL_ASSERT(!IsConstVisitor());
    return const_cast<Instruction*>(instr);
  }

 private:
  const VisitorConstness constness_;
};


class Decoder {
 public:
  Decoder() {}

  // Top-level wrappers around the actual decoding function.
  void Decode(const Instruction* instr) {
    for (auto visitor : visitors_) {
      VIXL_ASSERT(visitor->IsConstVisitor());
    }
    DecodeInstruction(instr);
  }
  void Decode(Instruction* instr) {
    DecodeInstruction(const_cast<const Instruction*>(instr));
  }

  // Register a new visitor class with the decoder.
  // Decode() will call the corresponding visitor method from all registered
  // visitor classes when decoding reaches the leaf node of the instruction
  // decode tree.
  // Visitors are called in order.
  // A visitor can be registered multiple times.
  //
  //   d.AppendVisitor(V1);
  //   d.AppendVisitor(V2);
  //   d.PrependVisitor(V2);
  //   d.AppendVisitor(V3);
  //
  //   d.Decode(i);
  //
  // will call in order visitor methods in V2, V1, V2, V3.
  void AppendVisitor(DecoderVisitor* visitor);
  void PrependVisitor(DecoderVisitor* visitor);
  // These helpers register `new_visitor` before or after the first instance of
  // `registered_visiter` in the list.
  // So if
  //   V1, V2, V1, V2
  // are registered in this order in the decoder, calls to
  //   d.InsertVisitorAfter(V3, V1);
  //   d.InsertVisitorBefore(V4, V2);
  // will yield the order
  //   V1, V3, V4, V2, V1, V2
  //
  // For more complex modifications of the order of registered visitors, one can
  // directly access and modify the list of visitors via the `visitors()'
  // accessor.
  void InsertVisitorBefore(DecoderVisitor* new_visitor,
                           DecoderVisitor* registered_visitor);
  void InsertVisitorAfter(DecoderVisitor* new_visitor,
                          DecoderVisitor* registered_visitor);

  // Remove all instances of a previously registered visitor class from the list
  // of visitors stored by the decoder.
  void RemoveVisitor(DecoderVisitor* visitor);

  #define DECLARE(A) void Visit##A(const Instruction* instr);
  VISITOR_LIST(DECLARE)
  #undef DECLARE


 private:
  // Decodes an instruction and calls the visitor functions registered with the
  // Decoder class.
  void DecodeInstruction(const Instruction* instr);

  // Decode the PC relative addressing instruction, and call the corresponding
  // visitors.
  // On entry, instruction bits 27:24 = 0x0.
  void DecodePCRelAddressing(const Instruction* instr);

  // Decode the add/subtract immediate instruction, and call the correspoding
  // visitors.
  // On entry, instruction bits 27:24 = 0x1.
  void DecodeAddSubImmediate(const Instruction* instr);

  // Decode the branch, system command, and exception generation parts of
  // the instruction tree, and call the corresponding visitors.
  // On entry, instruction bits 27:24 = {0x4, 0x5, 0x6, 0x7}.
  void DecodeBranchSystemException(const Instruction* instr);

  // Decode the load and store parts of the instruction tree, and call
  // the corresponding visitors.
  // On entry, instruction bits 27:24 = {0x8, 0x9, 0xC, 0xD}.
  void DecodeLoadStore(const Instruction* instr);

  // Decode the logical immediate and move wide immediate parts of the
  // instruction tree, and call the corresponding visitors.
  // On entry, instruction bits 27:24 = 0x2.
  void DecodeLogical(const Instruction* instr);

  // Decode the bitfield and extraction parts of the instruction tree,
  // and call the corresponding visitors.
  // On entry, instruction bits 27:24 = 0x3.
  void DecodeBitfieldExtract(const Instruction* instr);

  // Decode the data processing parts of the instruction tree, and call the
  // corresponding visitors.
  // On entry, instruction bits 27:24 = {0x1, 0xA, 0xB}.
  void DecodeDataProcessing(const Instruction* instr);

  // Decode the floating point parts of the instruction tree, and call the
  // corresponding visitors.
  // On entry, instruction bits 27:24 = {0xE, 0xF}.
  void DecodeFP(const Instruction* instr);

  // Decode the Advanced SIMD (NEON) load/store part of the instruction tree,
  // and call the corresponding visitors.
  // On entry, instruction bits 29:25 = 0x6.
  void DecodeNEONLoadStore(const Instruction* instr);

  // Decode the Advanced SIMD (NEON) vector data processing part of the
  // instruction tree, and call the corresponding visitors.
  // On entry, instruction bits 28:25 = 0x7.
  void DecodeNEONVectorDataProcessing(const Instruction* instr);

  // Decode the Advanced SIMD (NEON) scalar data processing part of the
  // instruction tree, and call the corresponding visitors.
  // On entry, instruction bits 28:25 = 0xF.
  void DecodeNEONScalarDataProcessing(const Instruction* instr);

 private:
  // Visitors are registered in a list.
  mozilla::Vector<DecoderVisitor*, 8, js::SystemAllocPolicy> visitors_;
};

}  // namespace vixl

#endif  // VIXL_A64_DECODER_A64_H_
