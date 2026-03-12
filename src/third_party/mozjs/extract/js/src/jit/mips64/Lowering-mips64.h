/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_Lowering_mips64_h
#define jit_mips64_Lowering_mips64_h

#include "jit/mips-shared/Lowering-mips-shared.h"

namespace js {
namespace jit {

class LIRGeneratorMIPS64 : public LIRGeneratorMIPSShared {
 protected:
  LIRGeneratorMIPS64(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorMIPSShared(gen, graph, lirGraph) {}

  void lowerInt64PhiInput(MPhi*, uint32_t, LBlock*, size_t);
  void defineInt64Phi(MPhi*, size_t);

  // Returns a box allocation. reg2 is ignored on 64-bit platforms.
  LBoxAllocation useBoxFixed(MDefinition* mir, Register reg1, Register reg2,
                             bool useAtStart = false);

  inline LDefinition tempToUnbox() { return temp(); }

  void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block,
                            size_t lirIndex);

  void lowerBuiltinInt64ToFloatingPoint(MBuiltinInt64ToFloatingPoint* ins);
  void lowerWasmBuiltinTruncateToInt64(MWasmBuiltinTruncateToInt64* ins);
  void lowerTruncateDToInt32(MTruncateToInt32* ins);
  void lowerTruncateFToInt32(MTruncateToInt32* ins);

  void lowerDivI64(MDiv* div);
  void lowerWasmBuiltinDivI64(MWasmBuiltinDivI64* div);
  void lowerModI64(MMod* mod);
  void lowerWasmBuiltinModI64(MWasmBuiltinModI64* mod);
  void lowerUDivI64(MDiv* div);
  void lowerUModI64(MMod* mod);

  void lowerAtomicLoad64(MLoadUnboxedScalar* ins);
  void lowerAtomicStore64(MStoreUnboxedScalar* ins);
};

typedef LIRGeneratorMIPS64 LIRGeneratorSpecific;

}  // namespace jit
}  // namespace js

#endif /* jit_mips64_Lowering_mips64_h */
