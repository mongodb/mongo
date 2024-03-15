/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/Lowering-arm.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/arm/Assembler-arm.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::FloorLog2;

LBoxAllocation LIRGeneratorARM::useBoxFixed(MDefinition* mir, Register reg1,
                                            Register reg2, bool useAtStart) {
  MOZ_ASSERT(mir->type() == MIRType::Value);
  MOZ_ASSERT(reg1 != reg2);

  ensureDefined(mir);
  return LBoxAllocation(LUse(reg1, mir->virtualRegister(), useAtStart),
                        LUse(reg2, VirtualRegisterOfPayload(mir), useAtStart));
}

LAllocation LIRGeneratorARM::useByteOpRegister(MDefinition* mir) {
  return useRegister(mir);
}

LAllocation LIRGeneratorARM::useByteOpRegisterAtStart(MDefinition* mir) {
  return useRegisterAtStart(mir);
}

LAllocation LIRGeneratorARM::useByteOpRegisterOrNonDoubleConstant(
    MDefinition* mir) {
  return useRegisterOrNonDoubleConstant(mir);
}

LDefinition LIRGeneratorARM::tempByteOpRegister() { return temp(); }

void LIRGenerator::visitBox(MBox* box) {
  MDefinition* inner = box->getOperand(0);

  // If the box wrapped a double, it needs a new register.
  if (IsFloatingPointType(inner->type())) {
    defineBox(new (alloc()) LBoxFloatingPoint(
                  useRegisterAtStart(inner), tempCopy(inner, 0), inner->type()),
              box);
    return;
  }

  if (box->canEmitAtUses()) {
    emitAtUses(box);
    return;
  }

  if (inner->isConstant()) {
    defineBox(new (alloc()) LValue(inner->toConstant()->toJSValue()), box);
    return;
  }

  LBox* lir = new (alloc()) LBox(use(inner), inner->type());

  // Otherwise, we should not define a new register for the payload portion
  // of the output, so bypass defineBox().
  uint32_t vreg = getVirtualRegister();

  // Note that because we're using BogusTemp(), we do not change the type of
  // the definition. We also do not define the first output as "TYPE",
  // because it has no corresponding payload at (vreg + 1). Also note that
  // although we copy the input's original type for the payload half of the
  // definition, this is only for clarity. BogusTemp() definitions are
  // ignored.
  lir->setDef(0, LDefinition(vreg, LDefinition::GENERAL));
  lir->setDef(1, LDefinition::BogusTemp());
  box->setVirtualRegister(vreg);
  add(lir);
}

void LIRGenerator::visitUnbox(MUnbox* unbox) {
  MDefinition* inner = unbox->getOperand(0);

  // An unbox on arm reads in a type tag (either in memory or a register) and
  // a payload. Unlike most instructions consuming a box, we ask for the type
  // second, so that the result can re-use the first input.
  MOZ_ASSERT(inner->type() == MIRType::Value);

  ensureDefined(inner);

  if (IsFloatingPointType(unbox->type())) {
    LUnboxFloatingPoint* lir =
        new (alloc()) LUnboxFloatingPoint(useBox(inner), unbox->type());
    if (unbox->fallible()) {
      assignSnapshot(lir, unbox->bailoutKind());
    }
    define(lir, unbox);
    return;
  }

  // Swap the order we use the box pieces so we can re-use the payload register.
  LUnbox* lir = new (alloc()) LUnbox;
  lir->setOperand(0, usePayloadInRegisterAtStart(inner));
  lir->setOperand(1, useType(inner, LUse::REGISTER));

  if (unbox->fallible()) {
    assignSnapshot(lir, unbox->bailoutKind());
  }

  // Types and payloads form two separate intervals. If the type becomes dead
  // before the payload, it could be used as a Value without the type being
  // recoverable. Unbox's purpose is to eagerly kill the definition of a type
  // tag, so keeping both alive (for the purpose of gcmaps) is unappealing.
  // Instead, we create a new virtual register.
  defineReuseInput(lir, unbox, 0);
}

void LIRGenerator::visitReturnImpl(MDefinition* opd, bool isGenerator) {
  MOZ_ASSERT(opd->type() == MIRType::Value);

  LReturn* ins = new (alloc()) LReturn(isGenerator);
  ins->setOperand(0, LUse(JSReturnReg_Type));
  ins->setOperand(1, LUse(JSReturnReg_Data));
  fillBoxUses(ins, 0, opd);
  add(ins);
}

void LIRGeneratorARM::defineInt64Phi(MPhi* phi, size_t lirIndex) {
  LPhi* low = current->getPhi(lirIndex + INT64LOW_INDEX);
  LPhi* high = current->getPhi(lirIndex + INT64HIGH_INDEX);

  uint32_t lowVreg = getVirtualRegister();

  phi->setVirtualRegister(lowVreg);

  uint32_t highVreg = getVirtualRegister();
  MOZ_ASSERT(lowVreg + INT64HIGH_INDEX == highVreg + INT64LOW_INDEX);

  low->setDef(0, LDefinition(lowVreg, LDefinition::INT32));
  high->setDef(0, LDefinition(highVreg, LDefinition::INT32));
  annotate(high);
  annotate(low);
}

void LIRGeneratorARM::lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition,
                                         LBlock* block, size_t lirIndex) {
  MDefinition* operand = phi->getOperand(inputPosition);
  LPhi* low = block->getPhi(lirIndex + INT64LOW_INDEX);
  LPhi* high = block->getPhi(lirIndex + INT64HIGH_INDEX);
  low->setOperand(inputPosition,
                  LUse(operand->virtualRegister() + INT64LOW_INDEX, LUse::ANY));
  high->setOperand(
      inputPosition,
      LUse(operand->virtualRegister() + INT64HIGH_INDEX, LUse::ANY));
}

// x = !y
void LIRGeneratorARM::lowerForALU(LInstructionHelper<1, 1, 0>* ins,
                                  MDefinition* mir, MDefinition* input) {
  ins->setOperand(
      0, ins->snapshot() ? useRegister(input) : useRegisterAtStart(input));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

// z = x+y
void LIRGeneratorARM::lowerForALU(LInstructionHelper<1, 2, 0>* ins,
                                  MDefinition* mir, MDefinition* lhs,
                                  MDefinition* rhs) {
  // Some operations depend on checking inputs after writing the result, e.g.
  // MulI, but only for bail out paths so useAtStart when no bailouts.
  ins->setOperand(0,
                  ins->snapshot() ? useRegister(lhs) : useRegisterAtStart(lhs));
  ins->setOperand(1, ins->snapshot() ? useRegisterOrConstant(rhs)
                                     : useRegisterOrConstantAtStart(rhs));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

void LIRGeneratorARM::lowerForALUInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES, 0>* ins, MDefinition* mir,
    MDefinition* input) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(input));
  defineInt64ReuseInput(ins, mir, 0);
}

void LIRGeneratorARM::lowerForALUInt64(
    LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
  ins->setInt64Operand(INT64_PIECES, useInt64OrConstant(rhs));
  defineInt64ReuseInput(ins, mir, 0);
}

void LIRGeneratorARM::lowerForMulInt64(LMulI64* ins, MMul* mir,
                                       MDefinition* lhs, MDefinition* rhs) {
  bool needsTemp = true;

  if (rhs->isConstant()) {
    int64_t constant = rhs->toConstant()->toInt64();
    int32_t shift = mozilla::FloorLog2(constant);
    // See special cases in CodeGeneratorARM::visitMulI64
    if (constant >= -1 && constant <= 2) {
      needsTemp = false;
    }
    if (constant > 0 && int64_t(1) << shift == constant) {
      needsTemp = false;
    }
  }

  ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
  ins->setInt64Operand(INT64_PIECES, useInt64OrConstant(rhs));
  if (needsTemp) {
    ins->setTemp(0, temp());
  }

  defineInt64ReuseInput(ins, mir, 0);
}

void LIRGeneratorARM::lowerForCompareI64AndBranch(MTest* mir, MCompare* comp,
                                                  JSOp op, MDefinition* left,
                                                  MDefinition* right,
                                                  MBasicBlock* ifTrue,
                                                  MBasicBlock* ifFalse) {
  LCompareI64AndBranch* lir = new (alloc())
      LCompareI64AndBranch(comp, op, useInt64Register(left),
                           useInt64OrConstant(right), ifTrue, ifFalse);
  add(lir, mir);
}

void LIRGeneratorARM::lowerForFPU(LInstructionHelper<1, 1, 0>* ins,
                                  MDefinition* mir, MDefinition* input) {
  ins->setOperand(0, useRegisterAtStart(input));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template <size_t Temps>
void LIRGeneratorARM::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins,
                                  MDefinition* mir, MDefinition* lhs,
                                  MDefinition* rhs) {
  ins->setOperand(0, useRegisterAtStart(lhs));
  ins->setOperand(1, useRegisterAtStart(rhs));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template void LIRGeneratorARM::lowerForFPU(LInstructionHelper<1, 2, 0>* ins,
                                           MDefinition* mir, MDefinition* lhs,
                                           MDefinition* rhs);
template void LIRGeneratorARM::lowerForFPU(LInstructionHelper<1, 2, 1>* ins,
                                           MDefinition* mir, MDefinition* lhs,
                                           MDefinition* rhs);

void LIRGeneratorARM::lowerForBitAndAndBranch(LBitAndAndBranch* baab,
                                              MInstruction* mir,
                                              MDefinition* lhs,
                                              MDefinition* rhs) {
  baab->setOperand(0, useRegisterAtStart(lhs));
  baab->setOperand(1, useRegisterOrConstantAtStart(rhs));
  add(baab, mir);
}

void LIRGeneratorARM::lowerWasmBuiltinTruncateToInt32(
    MWasmBuiltinTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

  if (opd->type() == MIRType::Double) {
    define(new (alloc()) LWasmBuiltinTruncateDToInt32(
               useRegister(opd), useFixedAtStart(ins->instance(), InstanceReg),
               LDefinition::BogusTemp()),
           ins);
    return;
  }

  define(new (alloc()) LWasmBuiltinTruncateFToInt32(
             useRegister(opd), useFixedAtStart(ins->instance(), InstanceReg),
             LDefinition::BogusTemp()),
         ins);
}

void LIRGeneratorARM::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition,
                                           LBlock* block, size_t lirIndex) {
  MDefinition* operand = phi->getOperand(inputPosition);
  LPhi* type = block->getPhi(lirIndex + VREG_TYPE_OFFSET);
  LPhi* payload = block->getPhi(lirIndex + VREG_DATA_OFFSET);
  type->setOperand(
      inputPosition,
      LUse(operand->virtualRegister() + VREG_TYPE_OFFSET, LUse::ANY));
  payload->setOperand(inputPosition,
                      LUse(VirtualRegisterOfPayload(operand), LUse::ANY));
}

void LIRGeneratorARM::lowerForShift(LInstructionHelper<1, 2, 0>* ins,
                                    MDefinition* mir, MDefinition* lhs,
                                    MDefinition* rhs) {
  ins->setOperand(0, useRegister(lhs));
  ins->setOperand(1, useRegisterOrConstant(rhs));
  define(ins, mir);
}

template <size_t Temps>
void LIRGeneratorARM::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs) {
  if (mir->isRotate() && !rhs->isConstant()) {
    ins->setTemp(0, temp());
  }

  ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
  ins->setOperand(INT64_PIECES, useRegisterOrConstant(rhs));
  defineInt64ReuseInput(ins, mir, 0);
}

template void LIRGeneratorARM::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, 0>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorARM::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, 1>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs);

void LIRGeneratorARM::lowerDivI(MDiv* div) {
  if (div->isUnsigned()) {
    lowerUDiv(div);
    return;
  }

  // Division instructions are slow. Division by constant denominators can be
  // rewritten to use other instructions.
  if (div->rhs()->isConstant()) {
    int32_t rhs = div->rhs()->toConstant()->toInt32();
    // Check for division by a positive power of two, which is an easy and
    // important case to optimize. Note that other optimizations are also
    // possible; division by negative powers of two can be optimized in a
    // similar manner as positive powers of two, and division by other
    // constants can be optimized by a reciprocal multiplication technique.
    int32_t shift = FloorLog2(rhs);
    if (rhs > 0 && 1 << shift == rhs) {
      LDivPowTwoI* lir =
          new (alloc()) LDivPowTwoI(useRegisterAtStart(div->lhs()), shift);
      if (div->fallible()) {
        assignSnapshot(lir, div->bailoutKind());
      }
      define(lir, div);
      return;
    }
  }

  if (HasIDIV()) {
    LDivI* lir = new (alloc())
        LDivI(useRegister(div->lhs()), useRegister(div->rhs()), temp());
    if (div->fallible()) {
      assignSnapshot(lir, div->bailoutKind());
    }
    define(lir, div);
    return;
  }

  LSoftDivI* lir = new (alloc()) LSoftDivI(useFixedAtStart(div->lhs(), r0),
                                           useFixedAtStart(div->rhs(), r1));

  if (div->fallible()) {
    assignSnapshot(lir, div->bailoutKind());
  }

  defineReturn(lir, div);
}

void LIRGeneratorARM::lowerNegI(MInstruction* ins, MDefinition* input) {
  define(new (alloc()) LNegI(useRegisterAtStart(input)), ins);
}

void LIRGeneratorARM::lowerNegI64(MInstruction* ins, MDefinition* input) {
  // Reuse the input.  Define + use-at-start would create risk that the output
  // uses the same register pair as the input but in reverse order.  Reusing
  // probably has less spilling than the alternative, define + use.
  defineInt64ReuseInput(new (alloc()) LNegI64(useInt64RegisterAtStart(input)),
                        ins, 0);
}

void LIRGenerator::visitAbs(MAbs* ins) {
  define(allocateAbs(ins, useRegisterAtStart(ins->input())), ins);
}

void LIRGeneratorARM::lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs) {
  LMulI* lir = new (alloc()) LMulI;
  if (mul->fallible()) {
    assignSnapshot(lir, mul->bailoutKind());
  }
  lowerForALU(lir, mul, lhs, rhs);
}

void LIRGeneratorARM::lowerModI(MMod* mod) {
  if (mod->isUnsigned()) {
    lowerUMod(mod);
    return;
  }

  if (mod->rhs()->isConstant()) {
    int32_t rhs = mod->rhs()->toConstant()->toInt32();
    int32_t shift = FloorLog2(rhs);
    if (rhs > 0 && 1 << shift == rhs) {
      LModPowTwoI* lir =
          new (alloc()) LModPowTwoI(useRegister(mod->lhs()), shift);
      if (mod->fallible()) {
        assignSnapshot(lir, mod->bailoutKind());
      }
      define(lir, mod);
      return;
    }
    if (shift < 31 && (1 << (shift + 1)) - 1 == rhs) {
      MOZ_ASSERT(rhs);
      LModMaskI* lir = new (alloc())
          LModMaskI(useRegister(mod->lhs()), temp(), temp(), shift + 1);
      if (mod->fallible()) {
        assignSnapshot(lir, mod->bailoutKind());
      }
      define(lir, mod);
      return;
    }
  }

  if (HasIDIV()) {
    LModI* lir =
        new (alloc()) LModI(useRegister(mod->lhs()), useRegister(mod->rhs()));
    if (mod->fallible()) {
      assignSnapshot(lir, mod->bailoutKind());
    }
    define(lir, mod);
    return;
  }

  LSoftModI* lir =
      new (alloc()) LSoftModI(useFixedAtStart(mod->lhs(), r0),
                              useFixedAtStart(mod->rhs(), r1), tempFixed(r2));

  if (mod->fallible()) {
    assignSnapshot(lir, mod->bailoutKind());
  }

  defineReturn(lir, mod);
}

void LIRGeneratorARM::lowerDivI64(MDiv* div) {
  MOZ_CRASH("We use MWasmBuiltinDivI64 instead.");
}

void LIRGeneratorARM::lowerWasmBuiltinDivI64(MWasmBuiltinDivI64* div) {
  if (div->isUnsigned()) {
    LUDivOrModI64* lir = new (alloc())
        LUDivOrModI64(useInt64RegisterAtStart(div->lhs()),
                      useInt64RegisterAtStart(div->rhs()),
                      useFixedAtStart(div->instance(), InstanceReg));
    defineReturn(lir, div);
    return;
  }

  LDivOrModI64* lir = new (alloc()) LDivOrModI64(
      useInt64RegisterAtStart(div->lhs()), useInt64RegisterAtStart(div->rhs()),
      useFixedAtStart(div->instance(), InstanceReg));
  defineReturn(lir, div);
}

void LIRGeneratorARM::lowerModI64(MMod* mod) {
  MOZ_CRASH("We use MWasmBuiltinModI64 instead.");
}

void LIRGeneratorARM::lowerWasmBuiltinModI64(MWasmBuiltinModI64* mod) {
  if (mod->isUnsigned()) {
    LUDivOrModI64* lir = new (alloc())
        LUDivOrModI64(useInt64RegisterAtStart(mod->lhs()),
                      useInt64RegisterAtStart(mod->rhs()),
                      useFixedAtStart(mod->instance(), InstanceReg));
    defineReturn(lir, mod);
    return;
  }

  LDivOrModI64* lir = new (alloc()) LDivOrModI64(
      useInt64RegisterAtStart(mod->lhs()), useInt64RegisterAtStart(mod->rhs()),
      useFixedAtStart(mod->instance(), InstanceReg));
  defineReturn(lir, mod);
}

void LIRGeneratorARM::lowerUDivI64(MDiv* div) {
  MOZ_CRASH("We use MWasmBuiltinDivI64 instead.");
}

void LIRGeneratorARM::lowerUModI64(MMod* mod) {
  MOZ_CRASH("We use MWasmBuiltinModI64 instead.");
}

void LIRGenerator::visitPowHalf(MPowHalf* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Double);
  LPowHalfD* lir = new (alloc()) LPowHalfD(useRegisterAtStart(input));
  defineReuseInput(lir, ins, 0);
}

void LIRGeneratorARM::lowerWasmSelectI(MWasmSelect* select) {
  auto* lir = new (alloc())
      LWasmSelect(useRegisterAtStart(select->trueExpr()),
                  useAny(select->falseExpr()), useRegister(select->condExpr()));
  defineReuseInput(lir, select, LWasmSelect::TrueExprIndex);
}

void LIRGeneratorARM::lowerWasmSelectI64(MWasmSelect* select) {
  auto* lir = new (alloc()) LWasmSelectI64(
      useInt64RegisterAtStart(select->trueExpr()),
      useInt64(select->falseExpr()), useRegister(select->condExpr()));
  defineInt64ReuseInput(lir, select, LWasmSelectI64::TrueExprIndex);
}

LTableSwitch* LIRGeneratorARM::newLTableSwitch(const LAllocation& in,
                                               const LDefinition& inputCopy,
                                               MTableSwitch* tableswitch) {
  return new (alloc()) LTableSwitch(in, inputCopy, tableswitch);
}

LTableSwitchV* LIRGeneratorARM::newLTableSwitchV(MTableSwitch* tableswitch) {
  return new (alloc()) LTableSwitchV(useBox(tableswitch->getOperand(0)), temp(),
                                     tempDouble(), tableswitch);
}

void LIRGeneratorARM::lowerUrshD(MUrsh* mir) {
  MDefinition* lhs = mir->lhs();
  MDefinition* rhs = mir->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Int32);
  MOZ_ASSERT(rhs->type() == MIRType::Int32);

  LUrshD* lir = new (alloc())
      LUrshD(useRegister(lhs), useRegisterOrConstant(rhs), temp());
  define(lir, mir);
}

void LIRGeneratorARM::lowerPowOfTwoI(MPow* mir) {
  int32_t base = mir->input()->toConstant()->toInt32();
  MDefinition* power = mir->power();

  auto* lir = new (alloc()) LPowOfTwoI(useRegister(power), base);
  assignSnapshot(lir, mir->bailoutKind());
  define(lir, mir);
}

void LIRGeneratorARM::lowerBigIntLsh(MBigIntLsh* ins) {
  auto* lir = new (alloc()) LBigIntLsh(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorARM::lowerBigIntRsh(MBigIntRsh* ins) {
  auto* lir = new (alloc()) LBigIntRsh(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorARM::lowerBigIntDiv(MBigIntDiv* ins) {
  LDefinition temp1, temp2;
  if (HasIDIV()) {
    temp1 = temp();
    temp2 = temp();
  } else {
    temp1 = tempFixed(r0);
    temp2 = tempFixed(r1);
  }
  auto* lir = new (alloc()) LBigIntDiv(useRegister(ins->lhs()),
                                       useRegister(ins->rhs()), temp1, temp2);
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorARM::lowerBigIntMod(MBigIntMod* ins) {
  LDefinition temp1, temp2;
  if (HasIDIV()) {
    temp1 = temp();
    temp2 = temp();
  } else {
    temp1 = tempFixed(r0);
    temp2 = tempFixed(r1);
  }
  auto* lir = new (alloc()) LBigIntMod(useRegister(ins->lhs()),
                                       useRegister(ins->rhs()), temp1, temp2);
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitWasmNeg(MWasmNeg* ins) {
  if (ins->type() == MIRType::Int32) {
    define(new (alloc()) LNegI(useRegisterAtStart(ins->input())), ins);
  } else if (ins->type() == MIRType::Float32) {
    define(new (alloc()) LNegF(useRegisterAtStart(ins->input())), ins);
  } else {
    MOZ_ASSERT(ins->type() == MIRType::Double);
    define(new (alloc()) LNegD(useRegisterAtStart(ins->input())), ins);
  }
}

void LIRGeneratorARM::lowerUDiv(MDiv* div) {
  MDefinition* lhs = div->getOperand(0);
  MDefinition* rhs = div->getOperand(1);

  if (HasIDIV()) {
    LUDiv* lir = new (alloc()) LUDiv;
    lir->setOperand(0, useRegister(lhs));
    lir->setOperand(1, useRegister(rhs));
    if (div->fallible()) {
      assignSnapshot(lir, div->bailoutKind());
    }
    define(lir, div);
    return;
  }

  LSoftUDivOrMod* lir = new (alloc())
      LSoftUDivOrMod(useFixedAtStart(lhs, r0), useFixedAtStart(rhs, r1));

  if (div->fallible()) {
    assignSnapshot(lir, div->bailoutKind());
  }

  defineReturn(lir, div);
}

void LIRGeneratorARM::lowerUMod(MMod* mod) {
  MDefinition* lhs = mod->getOperand(0);
  MDefinition* rhs = mod->getOperand(1);

  if (HasIDIV()) {
    LUMod* lir = new (alloc()) LUMod;
    lir->setOperand(0, useRegister(lhs));
    lir->setOperand(1, useRegister(rhs));
    if (mod->fallible()) {
      assignSnapshot(lir, mod->bailoutKind());
    }
    define(lir, mod);
    return;
  }

  LSoftUDivOrMod* lir = new (alloc())
      LSoftUDivOrMod(useFixedAtStart(lhs, r0), useFixedAtStart(rhs, r1));

  if (mod->fallible()) {
    assignSnapshot(lir, mod->bailoutKind());
  }

  defineReturn(lir, mod);
}

void LIRGenerator::visitWasmUnsignedToDouble(MWasmUnsignedToDouble* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::Int32);
  LWasmUint32ToDouble* lir =
      new (alloc()) LWasmUint32ToDouble(useRegisterAtStart(ins->input()));
  define(lir, ins);
}

void LIRGenerator::visitWasmUnsignedToFloat32(MWasmUnsignedToFloat32* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::Int32);
  LWasmUint32ToFloat32* lir =
      new (alloc()) LWasmUint32ToFloat32(useRegisterAtStart(ins->input()));
  define(lir, ins);
}

void LIRGenerator::visitWasmHeapBase(MWasmHeapBase* ins) {
  auto* lir = new (alloc()) LWasmHeapBase(LAllocation());
  define(lir, ins);
}

void LIRGenerator::visitWasmLoad(MWasmLoad* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  if (ins->access().type() == Scalar::Int64 && ins->access().isAtomic()) {
    auto* lir = new (alloc()) LWasmAtomicLoadI64(useRegisterAtStart(base));
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(IntArgReg1)),
                                      LAllocation(AnyRegister(IntArgReg0))));
    return;
  }

  LAllocation ptr = useRegisterAtStart(base);

  if (ins->type() == MIRType::Int64) {
    auto* lir = new (alloc()) LWasmLoadI64(ptr);
    if (ins->access().offset() || ins->access().type() == Scalar::Int64) {
      lir->setTemp(0, tempCopy(base, 0));
    }
    defineInt64(lir, ins);
    return;
  }

  auto* lir = new (alloc()) LWasmLoad(ptr);
  if (ins->access().offset()) {
    lir->setTemp(0, tempCopy(base, 0));
  }

  define(lir, ins);
}

void LIRGenerator::visitWasmStore(MWasmStore* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  if (ins->access().type() == Scalar::Int64 && ins->access().isAtomic()) {
    auto* lir = new (alloc()) LWasmAtomicStoreI64(
        useRegister(base),
        useInt64Fixed(ins->value(), Register64(IntArgReg1, IntArgReg0)),
        tempFixed(IntArgReg2), tempFixed(IntArgReg3));
    add(lir, ins);
    return;
  }

  LAllocation ptr = useRegisterAtStart(base);

  if (ins->value()->type() == MIRType::Int64) {
    LInt64Allocation value = useInt64RegisterAtStart(ins->value());
    auto* lir = new (alloc()) LWasmStoreI64(ptr, value);
    if (ins->access().offset() || ins->access().type() == Scalar::Int64) {
      lir->setTemp(0, tempCopy(base, 0));
    }
    add(lir, ins);
    return;
  }

  LAllocation value = useRegisterAtStart(ins->value());
  auto* lir = new (alloc()) LWasmStore(ptr, value);

  if (ins->access().offset()) {
    lir->setTemp(0, tempCopy(base, 0));
  }

  add(lir, ins);
}

void LIRGenerator::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  // For the ARM it is best to keep the 'base' in a register if a bounds check
  // is needed.
  LAllocation baseAlloc;
  LAllocation limitAlloc;

  if (base->isConstant() && !ins->needsBoundsCheck()) {
    // A bounds check is only skipped for a positive index.
    MOZ_ASSERT(base->toConstant()->toInt32() >= 0);
    baseAlloc = LAllocation(base->toConstant());
  } else {
    baseAlloc = useRegisterAtStart(base);
    if (ins->needsBoundsCheck()) {
      MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
      MOZ_ASSERT(boundsCheckLimit->type() == MIRType::Int32);
      limitAlloc = useRegisterAtStart(boundsCheckLimit);
    }
  }

  define(new (alloc()) LAsmJSLoadHeap(baseAlloc, limitAlloc, LAllocation()),
         ins);
}

void LIRGenerator::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  LAllocation baseAlloc;
  LAllocation limitAlloc;

  if (base->isConstant() && !ins->needsBoundsCheck()) {
    MOZ_ASSERT(base->toConstant()->toInt32() >= 0);
    baseAlloc = LAllocation(base->toConstant());
  } else {
    baseAlloc = useRegisterAtStart(base);
    if (ins->needsBoundsCheck()) {
      MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
      MOZ_ASSERT(boundsCheckLimit->type() == MIRType::Int32);
      limitAlloc = useRegisterAtStart(boundsCheckLimit);
    }
  }

  add(new (alloc()) LAsmJSStoreHeap(baseAlloc, useRegisterAtStart(ins->value()),
                                    limitAlloc, LAllocation()),
      ins);
}

void LIRGeneratorARM::lowerTruncateDToInt32(MTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double);

  define(new (alloc())
             LTruncateDToInt32(useRegister(opd), LDefinition::BogusTemp()),
         ins);
}

void LIRGeneratorARM::lowerTruncateFToInt32(MTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Float32);

  define(new (alloc())
             LTruncateFToInt32(useRegister(opd), LDefinition::BogusTemp()),
         ins);
}

void LIRGenerator::visitAtomicExchangeTypedArrayElement(
    MAtomicExchangeTypedArrayElement* ins) {
  MOZ_ASSERT(HasLDSTREXBHD());

  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());
  const LAllocation value = useRegister(ins->value());

  if (Scalar::isBigIntType(ins->arrayType())) {
    // The two register pairs must be distinct.
    LInt64Definition temp1 = tempInt64Fixed(Register64(IntArgReg3, IntArgReg2));
    LDefinition temp2 = tempFixed(IntArgReg1);

    auto* lir = new (alloc()) LAtomicExchangeTypedArrayElement64(
        elements, index, value, temp1, temp2);
    defineFixed(lir, ins, LAllocation(AnyRegister(IntArgReg0)));
    assignSafepoint(lir, ins);
    return;
  }

  MOZ_ASSERT(ins->arrayType() <= Scalar::Uint32);

  // If the target is a floating register then we need a temp at the
  // CodeGenerator level for creating the result.

  LDefinition tempDef = LDefinition::BogusTemp();
  if (ins->arrayType() == Scalar::Uint32) {
    MOZ_ASSERT(ins->type() == MIRType::Double);
    tempDef = temp();
  }

  LAtomicExchangeTypedArrayElement* lir = new (alloc())
      LAtomicExchangeTypedArrayElement(elements, index, value, tempDef);

  define(lir, ins);
}

void LIRGenerator::visitAtomicTypedArrayElementBinop(
    MAtomicTypedArrayElementBinop* ins) {
  MOZ_ASSERT(ins->arrayType() != Scalar::Uint8Clamped);
  MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
  MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());
  const LAllocation value = useRegister(ins->value());

  if (Scalar::isBigIntType(ins->arrayType())) {
    // Wasm additionally pins the value register to `FetchOpVal64`, but it's
    // unclear why this was deemed necessary.
    LInt64Definition temp1 = tempInt64();
    LInt64Definition temp2 = tempInt64Fixed(FetchOpTmp64);

    if (ins->isForEffect()) {
      auto* lir = new (alloc()) LAtomicTypedArrayElementBinopForEffect64(
          elements, index, value, temp1, temp2);
      add(lir, ins);
      return;
    }

    LInt64Definition temp3 = tempInt64Fixed(FetchOpOut64);

    auto* lir = new (alloc()) LAtomicTypedArrayElementBinop64(
        elements, index, value, temp1, temp2, temp3);
    define(lir, ins);
    assignSafepoint(lir, ins);
    return;
  }

  if (ins->isForEffect()) {
    LAtomicTypedArrayElementBinopForEffect* lir = new (alloc())
        LAtomicTypedArrayElementBinopForEffect(elements, index, value,
                                               /* flagTemp= */ temp());
    add(lir, ins);
    return;
  }

  // For a Uint32Array with a known double result we need a temp for
  // the intermediate output.
  //
  // Optimization opportunity (bug 1077317): We can do better by
  // allowing 'value' to remain as an imm32 if it is small enough to
  // fit in an instruction.

  LDefinition flagTemp = temp();
  LDefinition outTemp = LDefinition::BogusTemp();

  if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type())) {
    outTemp = temp();
  }

  // On arm, map flagTemp to temp1 and outTemp to temp2, at least for now.

  LAtomicTypedArrayElementBinop* lir = new (alloc())
      LAtomicTypedArrayElementBinop(elements, index, value, flagTemp, outTemp);
  define(lir, ins);
}

void LIRGenerator::visitCompareExchangeTypedArrayElement(
    MCompareExchangeTypedArrayElement* ins) {
  MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
  MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());

  const LAllocation newval = useRegister(ins->newval());
  const LAllocation oldval = useRegister(ins->oldval());

  if (Scalar::isBigIntType(ins->arrayType())) {
    // The three register pairs must be distinct.
    LInt64Definition temp1 = tempInt64Fixed(CmpXchgOld64);
    LInt64Definition temp2 = tempInt64Fixed(CmpXchgNew64);
    LInt64Definition temp3 = tempInt64Fixed(CmpXchgOut64);

    auto* lir = new (alloc()) LCompareExchangeTypedArrayElement64(
        elements, index, oldval, newval, temp1, temp2, temp3);
    define(lir, ins);
    assignSafepoint(lir, ins);
    return;
  }

  // If the target is a floating register then we need a temp at the
  // CodeGenerator level for creating the result.
  //
  // Optimization opportunity (bug 1077317): We could do better by
  // allowing oldval to remain an immediate, if it is small enough
  // to fit in an instruction.

  LDefinition tempDef = LDefinition::BogusTemp();
  if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type())) {
    tempDef = temp();
  }

  LCompareExchangeTypedArrayElement* lir =
      new (alloc()) LCompareExchangeTypedArrayElement(elements, index, oldval,
                                                      newval, tempDef);

  define(lir, ins);
}

void LIRGeneratorARM::lowerAtomicLoad64(MLoadUnboxedScalar* ins) {
  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->storageType());

  auto* lir = new (alloc())
      LAtomicLoad64(elements, index, temp(),
                    tempInt64Fixed(Register64(IntArgReg1, IntArgReg0)));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorARM::lowerAtomicStore64(MStoreUnboxedScalar* ins) {
  LUse elements = useRegister(ins->elements());
  LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->writeType());
  LAllocation value = useRegister(ins->value());
  LInt64Definition temp1 = tempInt64Fixed(Register64(IntArgReg1, IntArgReg0));
  LInt64Definition temp2 = tempInt64Fixed(Register64(IntArgReg3, IntArgReg2));

  add(new (alloc()) LAtomicStore64(elements, index, value, temp1, temp2), ins);
}

void LIRGenerator::visitWasmCompareExchangeHeap(MWasmCompareExchangeHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  if (ins->access().type() == Scalar::Int64) {
    // The three register pairs must be distinct.
    auto* lir = new (alloc()) LWasmCompareExchangeI64(
        useRegister(base), useInt64Fixed(ins->oldValue(), CmpXchgOld64),
        useInt64Fixed(ins->newValue(), CmpXchgNew64));
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(CmpXchgOutHi)),
                                      LAllocation(AnyRegister(CmpXchgOutLo))));
    return;
  }

  MOZ_ASSERT(ins->access().type() < Scalar::Float32);
  MOZ_ASSERT(HasLDSTREXBHD(), "by HasPlatformSupport() constraints");

  LWasmCompareExchangeHeap* lir = new (alloc())
      LWasmCompareExchangeHeap(useRegister(base), useRegister(ins->oldValue()),
                               useRegister(ins->newValue()));

  define(lir, ins);
}

void LIRGenerator::visitWasmAtomicExchangeHeap(MWasmAtomicExchangeHeap* ins) {
  MOZ_ASSERT(ins->base()->type() == MIRType::Int32);

  if (ins->access().type() == Scalar::Int64) {
    auto* lir = new (alloc()) LWasmAtomicExchangeI64(
        useRegister(ins->base()), useInt64Fixed(ins->value(), XchgNew64),
        ins->access());
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(XchgOutHi)),
                                      LAllocation(AnyRegister(XchgOutLo))));
    return;
  }

  MOZ_ASSERT(ins->access().type() < Scalar::Float32);
  MOZ_ASSERT(HasLDSTREXBHD(), "by HasPlatformSupport() constraints");

  const LAllocation base = useRegister(ins->base());
  const LAllocation value = useRegister(ins->value());
  define(new (alloc()) LWasmAtomicExchangeHeap(base, value), ins);
}

void LIRGenerator::visitWasmAtomicBinopHeap(MWasmAtomicBinopHeap* ins) {
  if (ins->access().type() == Scalar::Int64) {
    auto* lir = new (alloc()) LWasmAtomicBinopI64(
        useRegister(ins->base()), useInt64Fixed(ins->value(), FetchOpVal64),
        tempFixed(FetchOpTmpLo), tempFixed(FetchOpTmpHi), ins->access(),
        ins->operation());
    defineInt64Fixed(lir, ins,
                     LInt64Allocation(LAllocation(AnyRegister(FetchOpOutHi)),
                                      LAllocation(AnyRegister(FetchOpOutLo))));
    return;
  }

  MOZ_ASSERT(ins->access().type() < Scalar::Float32);
  MOZ_ASSERT(HasLDSTREXBHD(), "by HasPlatformSupport() constraints");

  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  if (!ins->hasUses()) {
    LWasmAtomicBinopHeapForEffect* lir =
        new (alloc()) LWasmAtomicBinopHeapForEffect(useRegister(base),
                                                    useRegister(ins->value()),
                                                    /* flagTemp= */ temp());
    add(lir, ins);
    return;
  }

  LWasmAtomicBinopHeap* lir = new (alloc())
      LWasmAtomicBinopHeap(useRegister(base), useRegister(ins->value()),
                           /* temp = */ LDefinition::BogusTemp(),
                           /* flagTemp= */ temp());
  define(lir, ins);
}

void LIRGenerator::visitSubstr(MSubstr* ins) {
  LSubstr* lir = new (alloc())
      LSubstr(useRegister(ins->string()), useRegister(ins->begin()),
              useRegister(ins->length()), temp(), temp(), tempByteOpRegister());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitWasmTruncateToInt64(MWasmTruncateToInt64* ins) {
  MOZ_CRASH("We don't use MWasmTruncateToInt64 for arm");
}

void LIRGeneratorARM::lowerWasmBuiltinTruncateToInt64(
    MWasmBuiltinTruncateToInt64* ins) {
  MDefinition* opd = ins->input();
  MDefinition* instance = ins->instance();
  MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

  defineReturn(new (alloc())
                   LWasmTruncateToInt64(useRegisterAtStart(opd),
                                        useFixedAtStart(instance, InstanceReg)),
               ins);
}

void LIRGenerator::visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins) {
  MOZ_CRASH("We use BuiltinInt64ToFloatingPoint instead.");
}

void LIRGeneratorARM::lowerBuiltinInt64ToFloatingPoint(
    MBuiltinInt64ToFloatingPoint* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Double || ins->type() == MIRType::Float32);

  auto* lir = new (alloc())
      LInt64ToFloatingPointCall(useInt64RegisterAtStart(ins->input()),
                                useFixedAtStart(ins->instance(), InstanceReg));
  defineReturn(lir, ins);
}

void LIRGenerator::visitCopySign(MCopySign* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(IsFloatingPointType(lhs->type()));
  MOZ_ASSERT(lhs->type() == rhs->type());
  MOZ_ASSERT(lhs->type() == ins->type());

  LInstructionHelper<1, 2, 2>* lir;
  if (lhs->type() == MIRType::Double) {
    lir = new (alloc()) LCopySignD();
  } else {
    lir = new (alloc()) LCopySignF();
  }

  lir->setTemp(0, temp());
  lir->setTemp(1, temp());

  lowerForFPU(lir, ins, lhs, rhs);
}

void LIRGenerator::visitExtendInt32ToInt64(MExtendInt32ToInt64* ins) {
  auto* lir =
      new (alloc()) LExtendInt32ToInt64(useRegisterAtStart(ins->input()));
  defineInt64(lir, ins);

  LDefinition def(LDefinition::GENERAL, LDefinition::MUST_REUSE_INPUT);
  def.setReusedInput(0);
  def.setVirtualRegister(ins->virtualRegister());

  lir->setDef(0, def);
}

void LIRGenerator::visitSignExtendInt64(MSignExtendInt64* ins) {
  defineInt64(new (alloc())
                  LSignExtendInt64(useInt64RegisterAtStart(ins->input())),
              ins);
}

// On arm we specialize the only cases where compare is {U,}Int32 and select
// is {U,}Int32.
bool LIRGeneratorShared::canSpecializeWasmCompareAndSelect(
    MCompare::CompareType compTy, MIRType insTy) {
  return insTy == MIRType::Int32 && (compTy == MCompare::Compare_Int32 ||
                                     compTy == MCompare::Compare_UInt32);
}

void LIRGeneratorShared::lowerWasmCompareAndSelect(MWasmSelect* ins,
                                                   MDefinition* lhs,
                                                   MDefinition* rhs,
                                                   MCompare::CompareType compTy,
                                                   JSOp jsop) {
  MOZ_ASSERT(canSpecializeWasmCompareAndSelect(compTy, ins->type()));
  auto* lir = new (alloc()) LWasmCompareAndSelect(
      useRegister(lhs), useRegister(rhs), compTy, jsop,
      useRegisterAtStart(ins->trueExpr()), useRegister(ins->falseExpr()));
  defineReuseInput(lir, ins, LWasmCompareAndSelect::IfTrueExprIndex);
}

void LIRGenerator::visitWasmTernarySimd128(MWasmTernarySimd128* ins) {
  MOZ_CRASH("ternary SIMD NYI");
}

void LIRGenerator::visitWasmBinarySimd128(MWasmBinarySimd128* ins) {
  MOZ_CRASH("binary SIMD NYI");
}

#ifdef ENABLE_WASM_SIMD
bool MWasmTernarySimd128::specializeBitselectConstantMaskAsShuffle(
    int8_t shuffle[16]) {
  return false;
}
bool MWasmTernarySimd128::canRelaxBitselect() { return false; }

bool MWasmBinarySimd128::canPmaddubsw() { return false; }
#endif

bool MWasmBinarySimd128::specializeForConstantRhs() {
  // Probably many we want to do here
  return false;
}

void LIRGenerator::visitWasmBinarySimd128WithConstant(
    MWasmBinarySimd128WithConstant* ins) {
  MOZ_CRASH("binary SIMD with constant NYI");
}

void LIRGenerator::visitWasmShiftSimd128(MWasmShiftSimd128* ins) {
  MOZ_CRASH("shift SIMD NYI");
}

void LIRGenerator::visitWasmShuffleSimd128(MWasmShuffleSimd128* ins) {
  MOZ_CRASH("shuffle SIMD NYI");
}

void LIRGenerator::visitWasmReplaceLaneSimd128(MWasmReplaceLaneSimd128* ins) {
  MOZ_CRASH("replace-lane SIMD NYI");
}

void LIRGenerator::visitWasmScalarToSimd128(MWasmScalarToSimd128* ins) {
  MOZ_CRASH("scalar-to-SIMD NYI");
}

void LIRGenerator::visitWasmUnarySimd128(MWasmUnarySimd128* ins) {
  MOZ_CRASH("unary SIMD NYI");
}

void LIRGenerator::visitWasmReduceSimd128(MWasmReduceSimd128* ins) {
  MOZ_CRASH("reduce-SIMD NYI");
}

void LIRGenerator::visitWasmLoadLaneSimd128(MWasmLoadLaneSimd128* ins) {
  MOZ_CRASH("load-lane SIMD NYI");
}

void LIRGenerator::visitWasmStoreLaneSimd128(MWasmStoreLaneSimd128* ins) {
  MOZ_CRASH("store-lane SIMD NYI");
}
