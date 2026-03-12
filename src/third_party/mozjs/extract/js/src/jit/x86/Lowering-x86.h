/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_Lowering_x86_h
#define jit_x86_Lowering_x86_h

#include "jit/x86-shared/Lowering-x86-shared.h"

namespace js {
namespace jit {

class LIRGeneratorX86 : public LIRGeneratorX86Shared {
 protected:
  LIRGeneratorX86(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorX86Shared(gen, graph, lirGraph) {}

  // Returns a box allocation with type set to reg1 and payload set to reg2.
  LBoxAllocation useBoxFixed(MDefinition* mir, Register reg1, Register reg2,
                             bool useAtStart = false);

  // It's a trap! On x86, the 1-byte store can only use one of
  // {al,bl,cl,dl,ah,bh,ch,dh}. That means if the register allocator
  // gives us one of {edi,esi,ebp,esp}, we're out of luck. (The formatter
  // will assert on us.) Ideally, we'd just ask the register allocator to
  // give us one of {al,bl,cl,dl}. For now, just useFixed(al).
  LAllocation useByteOpRegister(MDefinition* mir);
  LAllocation useByteOpRegisterAtStart(MDefinition* mir);
  LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition* mir);
  LDefinition tempByteOpRegister();

  inline LDefinition tempToUnbox() { return LDefinition::BogusTemp(); }

  bool needTempForPostBarrier() { return true; }

  void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block,
                            size_t lirIndex);

  void lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block,
                          size_t lirIndex);
  void defineInt64Phi(MPhi* phi, size_t lirIndex);

  void lowerForALUInt64(LInstructionHelper<INT64_PIECES, INT64_PIECES, 0>* ins,
                        MDefinition* mir, MDefinition* input);
  void lowerForALUInt64(
      LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
      MDefinition* mir, MDefinition* lhs, MDefinition* rhs);
  void lowerForMulInt64(LMulI64* ins, MMul* mir, MDefinition* lhs,
                        MDefinition* rhs);

  void lowerTruncateDToInt32(MTruncateToInt32* ins);
  void lowerTruncateFToInt32(MTruncateToInt32* ins);
  void lowerBuiltinInt64ToFloatingPoint(MBuiltinInt64ToFloatingPoint* ins);
  void lowerWasmBuiltinTruncateToInt32(MWasmBuiltinTruncateToInt32* ins);
  void lowerWasmBuiltinTruncateToInt64(MWasmBuiltinTruncateToInt64* ins);
  void lowerDivI64(MDiv* div);
  void lowerWasmBuiltinDivI64(MWasmBuiltinDivI64* div);
  void lowerModI64(MMod* mod);
  void lowerWasmBuiltinModI64(MWasmBuiltinModI64* mod);
  void lowerUDivI64(MDiv* div);
  void lowerUModI64(MMod* mod);

  void lowerBigIntPtrDiv(MBigIntPtrDiv* ins);
  void lowerBigIntPtrMod(MBigIntPtrMod* ins);

  void lowerAtomicLoad64(MLoadUnboxedScalar* ins);
  void lowerAtomicStore64(MStoreUnboxedScalar* ins);

  void lowerPhi(MPhi* phi);

 public:
  static bool allowTypedElementHoleCheck() { return true; }
};

using LIRGeneratorSpecific = LIRGeneratorX86;

}  // namespace jit
}  // namespace js

#endif /* jit_x86_Lowering_x86_h */
