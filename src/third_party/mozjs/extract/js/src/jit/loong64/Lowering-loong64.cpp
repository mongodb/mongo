/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/loong64/Lowering-loong64.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/loong64/Assembler-loong64.h"
#include "jit/Lowering.h"
#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::FloorLog2;

LTableSwitch* LIRGeneratorLOONG64::newLTableSwitch(
    const LAllocation& in, const LDefinition& inputCopy) {
  return new (alloc()) LTableSwitch(in, inputCopy, temp());
}

LTableSwitchV* LIRGeneratorLOONG64::newLTableSwitchV(const LBoxAllocation& in) {
  return new (alloc()) LTableSwitchV(in, temp(), tempDouble(), temp());
}

void LIRGeneratorLOONG64::lowerForShift(LInstructionHelper<1, 2, 0>* ins,
                                        MDefinition* mir, MDefinition* lhs,
                                        MDefinition* rhs) {
  ins->setOperand(0, useRegister(lhs));
  ins->setOperand(1, useRegisterOrConstant(rhs));
  define(ins, mir);
}

template <class LInstr>
void LIRGeneratorLOONG64::lowerForShiftInt64(LInstr* ins, MDefinition* mir,
                                             MDefinition* lhs,
                                             MDefinition* rhs) {
  if constexpr (std::is_same_v<LInstr, LShiftI64>) {
    ins->setLhs(useInt64RegisterAtStart(lhs));
    ins->setRhs(useRegisterOrConstant(rhs));
    defineInt64ReuseInput(ins, mir, LShiftI64::LhsIndex);
  } else {
    ins->setInput(useInt64RegisterAtStart(lhs));
    ins->setCount(useRegisterOrConstant(rhs));
    defineInt64ReuseInput(ins, mir, LRotateI64::InputIndex);
  }
}

template void LIRGeneratorLOONG64::lowerForShiftInt64(LShiftI64* ins,
                                                      MDefinition* mir,
                                                      MDefinition* lhs,
                                                      MDefinition* rhs);
template void LIRGeneratorLOONG64::lowerForShiftInt64(LRotateI64* ins,
                                                      MDefinition* mir,
                                                      MDefinition* lhs,
                                                      MDefinition* rhs);

// x = !y
void LIRGeneratorLOONG64::lowerForALU(LInstructionHelper<1, 1, 0>* ins,
                                      MDefinition* mir, MDefinition* input) {
  ins->setOperand(0, useRegister(input));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

// z = x + y
void LIRGeneratorLOONG64::lowerForALU(LInstructionHelper<1, 2, 0>* ins,
                                      MDefinition* mir, MDefinition* lhs,
                                      MDefinition* rhs) {
  ins->setOperand(0, useRegister(lhs));
  ins->setOperand(1, useRegisterOrConstant(rhs));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

void LIRGeneratorLOONG64::lowerForALUInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES, 0>* ins, MDefinition* mir,
    MDefinition* input) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(input));
  defineInt64ReuseInput(ins, mir, 0);
}

void LIRGeneratorLOONG64::lowerForALUInt64(
    LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
  ins->setInt64Operand(INT64_PIECES, willHaveDifferentLIRNodes(lhs, rhs)
                                         ? useInt64OrConstant(rhs)
                                         : useInt64OrConstantAtStart(rhs));
  defineInt64ReuseInput(ins, mir, 0);
}

void LIRGeneratorLOONG64::lowerForMulInt64(LMulI64* ins, MMul* mir,
                                           MDefinition* lhs, MDefinition* rhs) {
  bool needsTemp = false;
  bool cannotAliasRhs = false;
  bool reuseInput = true;

  ins->setLhs(useInt64RegisterAtStart(lhs));
  ins->setRhs((willHaveDifferentLIRNodes(lhs, rhs) || cannotAliasRhs)
                  ? useInt64OrConstant(rhs)
                  : useInt64OrConstantAtStart(rhs));

  if (needsTemp) {
    ins->setTemp0(temp());
  }
  if (reuseInput) {
    defineInt64ReuseInput(ins, mir, 0);
  } else {
    defineInt64(ins, mir);
  }
}

void LIRGeneratorLOONG64::lowerForFPU(LInstructionHelper<1, 1, 0>* ins,
                                      MDefinition* mir, MDefinition* input) {
  ins->setOperand(0, useRegister(input));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template <size_t Temps>
void LIRGeneratorLOONG64::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins,
                                      MDefinition* mir, MDefinition* lhs,
                                      MDefinition* rhs) {
  ins->setOperand(0, useRegister(lhs));
  ins->setOperand(1, useRegister(rhs));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template void LIRGeneratorLOONG64::lowerForFPU(LInstructionHelper<1, 2, 0>* ins,
                                               MDefinition* mir,
                                               MDefinition* lhs,
                                               MDefinition* rhs);
template void LIRGeneratorLOONG64::lowerForFPU(LInstructionHelper<1, 2, 1>* ins,
                                               MDefinition* mir,
                                               MDefinition* lhs,
                                               MDefinition* rhs);

LBoxAllocation LIRGeneratorLOONG64::useBoxFixed(MDefinition* mir, Register reg1,
                                                Register reg2,
                                                bool useAtStart) {
  MOZ_ASSERT(mir->type() == MIRType::Value);

  ensureDefined(mir);
  return LBoxAllocation(LUse(reg1, mir->virtualRegister(), useAtStart));
}

LAllocation LIRGeneratorLOONG64::useByteOpRegister(MDefinition* mir) {
  return useRegister(mir);
}

LAllocation LIRGeneratorLOONG64::useByteOpRegisterAtStart(MDefinition* mir) {
  return useRegisterAtStart(mir);
}

LAllocation LIRGeneratorLOONG64::useByteOpRegisterOrNonDoubleConstant(
    MDefinition* mir) {
  return useRegisterOrNonDoubleConstant(mir);
}

LDefinition LIRGeneratorLOONG64::tempByteOpRegister() { return temp(); }

LDefinition LIRGeneratorLOONG64::tempToUnbox() { return temp(); }

void LIRGeneratorLOONG64::lowerUntypedPhiInput(MPhi* phi,
                                               uint32_t inputPosition,
                                               LBlock* block, size_t lirIndex) {
  lowerTypedPhiInput(phi, inputPosition, block, lirIndex);
}
void LIRGeneratorLOONG64::lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition,
                                             LBlock* block, size_t lirIndex) {
  lowerTypedPhiInput(phi, inputPosition, block, lirIndex);
}
void LIRGeneratorLOONG64::defineInt64Phi(MPhi* phi, size_t lirIndex) {
  defineTypedPhi(phi, lirIndex);
}

void LIRGeneratorLOONG64::lowerNegI(MInstruction* ins, MDefinition* input) {
  define(new (alloc()) LNegI(useRegisterAtStart(input)), ins);
}
void LIRGeneratorLOONG64::lowerNegI64(MInstruction* ins, MDefinition* input) {
  defineInt64ReuseInput(new (alloc()) LNegI64(useInt64RegisterAtStart(input)),
                        ins, 0);
}

void LIRGeneratorLOONG64::lowerMulI(MMul* mul, MDefinition* lhs,
                                    MDefinition* rhs) {
  LMulI* lir = new (alloc()) LMulI;
  if (mul->fallible()) {
    assignSnapshot(lir, mul->bailoutKind());
  }

  lowerForALU(lir, mul, lhs, rhs);
}

void LIRGeneratorLOONG64::lowerDivI(MDiv* div) {
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
          new (alloc()) LDivPowTwoI(useRegister(div->lhs()), temp(), shift);
      if (div->fallible()) {
        assignSnapshot(lir, div->bailoutKind());
      }
      define(lir, div);
      return;
    }
  }

  LDivI* lir = new (alloc())
      LDivI(useRegister(div->lhs()), useRegister(div->rhs()), temp());
  if (div->fallible()) {
    assignSnapshot(lir, div->bailoutKind());
  }
  define(lir, div);
}

void LIRGeneratorLOONG64::lowerDivI64(MDiv* div) {
  if (div->isUnsigned()) {
    lowerUDivI64(div);
    return;
  }

  LDivOrModI64* lir = new (alloc())
      LDivOrModI64(useRegister(div->lhs()), useRegister(div->rhs()), temp());
  defineInt64(lir, div);
}

void LIRGeneratorLOONG64::lowerModI(MMod* mod) {
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
    } else if (shift < 31 && (1 << (shift + 1)) - 1 == rhs) {
      LModMaskI* lir = new (alloc())
          LModMaskI(useRegister(mod->lhs()), temp(), temp(), shift + 1);
      if (mod->fallible()) {
        assignSnapshot(lir, mod->bailoutKind());
      }
      define(lir, mod);
      return;
    }
  }
  LModI* lir = new (alloc())
      LModI(useRegister(mod->lhs()), useRegister(mod->rhs()), temp());

  if (mod->fallible()) {
    assignSnapshot(lir, mod->bailoutKind());
  }
  define(lir, mod);
}

void LIRGeneratorLOONG64::lowerModI64(MMod* mod) {
  if (mod->isUnsigned()) {
    lowerUModI64(mod);
    return;
  }

  LDivOrModI64* lir = new (alloc())
      LDivOrModI64(useRegister(mod->lhs()), useRegister(mod->rhs()), temp());
  defineInt64(lir, mod);
}

void LIRGeneratorLOONG64::lowerUDiv(MDiv* div) {
  MDefinition* lhs = div->getOperand(0);
  MDefinition* rhs = div->getOperand(1);

  LUDivOrMod* lir = new (alloc()) LUDivOrMod;
  lir->setOperand(0, useRegister(lhs));
  lir->setOperand(1, useRegister(rhs));
  if (div->fallible()) {
    assignSnapshot(lir, div->bailoutKind());
  }

  define(lir, div);
}

void LIRGeneratorLOONG64::lowerUDivI64(MDiv* div) {
  LUDivOrModI64* lir = new (alloc())
      LUDivOrModI64(useRegister(div->lhs()), useRegister(div->rhs()), temp());
  defineInt64(lir, div);
}

void LIRGeneratorLOONG64::lowerUMod(MMod* mod) {
  MDefinition* lhs = mod->getOperand(0);
  MDefinition* rhs = mod->getOperand(1);

  LUDivOrMod* lir = new (alloc()) LUDivOrMod;
  lir->setOperand(0, useRegister(lhs));
  lir->setOperand(1, useRegister(rhs));
  if (mod->fallible()) {
    assignSnapshot(lir, mod->bailoutKind());
  }

  define(lir, mod);
}

void LIRGeneratorLOONG64::lowerUModI64(MMod* mod) {
  LUDivOrModI64* lir = new (alloc())
      LUDivOrModI64(useRegister(mod->lhs()), useRegister(mod->rhs()), temp());
  defineInt64(lir, mod);
}

void LIRGeneratorLOONG64::lowerUrshD(MUrsh* mir) {
  MDefinition* lhs = mir->lhs();
  MDefinition* rhs = mir->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Int32);
  MOZ_ASSERT(rhs->type() == MIRType::Int32);

  LUrshD* lir = new (alloc())
      LUrshD(useRegister(lhs), useRegisterOrConstant(rhs), temp());
  define(lir, mir);
}

void LIRGeneratorLOONG64::lowerPowOfTwoI(MPow* mir) {
  int32_t base = mir->input()->toConstant()->toInt32();
  MDefinition* power = mir->power();

  auto* lir = new (alloc()) LPowOfTwoI(useRegister(power), base);
  assignSnapshot(lir, mir->bailoutKind());
  define(lir, mir);
}

void LIRGeneratorLOONG64::lowerBigIntPtrLsh(MBigIntPtrLsh* ins) {
  auto* lir = new (alloc()) LBigIntPtrLsh(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp());
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGeneratorLOONG64::lowerBigIntPtrRsh(MBigIntPtrRsh* ins) {
  auto* lir = new (alloc()) LBigIntPtrRsh(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp());
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGeneratorLOONG64::lowerBigIntPtrDiv(MBigIntPtrDiv* ins) {
  auto* lir = new (alloc())
      LBigIntPtrDiv(useRegister(ins->lhs()), useRegister(ins->rhs()),
                    LDefinition::BogusTemp(), LDefinition::BogusTemp());
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGeneratorLOONG64::lowerBigIntPtrMod(MBigIntPtrMod* ins) {
  auto* lir = new (alloc())
      LBigIntPtrMod(useRegister(ins->lhs()), useRegister(ins->rhs()), temp(),
                    LDefinition::BogusTemp());
  if (ins->canBeDivideByZero()) {
    assignSnapshot(lir, ins->bailoutKind());
  }
  define(lir, ins);
}

void LIRGeneratorLOONG64::lowerTruncateDToInt32(MTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double);

  define(new (alloc()) LTruncateDToInt32(useRegister(opd), tempDouble()), ins);
}

void LIRGeneratorLOONG64::lowerTruncateFToInt32(MTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Float32);

  define(new (alloc()) LTruncateFToInt32(useRegister(opd), tempFloat32()), ins);
}

void LIRGeneratorLOONG64::lowerBuiltinInt64ToFloatingPoint(
    MBuiltinInt64ToFloatingPoint* ins) {
  MOZ_CRASH("We don't use it for this architecture");
}

void LIRGeneratorLOONG64::lowerWasmSelectI(MWasmSelect* select) {
  auto* lir = new (alloc())
      LWasmSelect(useRegisterAtStart(select->trueExpr()),
                  useAny(select->falseExpr()), useRegister(select->condExpr()));
  defineReuseInput(lir, select, LWasmSelect::TrueExprIndex);
}

void LIRGeneratorLOONG64::lowerWasmSelectI64(MWasmSelect* select) {
  auto* lir = new (alloc()) LWasmSelectI64(
      useInt64RegisterAtStart(select->trueExpr()),
      useInt64(select->falseExpr()), useRegister(select->condExpr()));
  defineInt64ReuseInput(lir, select, LWasmSelectI64::TrueExprIndex);
}

// On loong64 we specialize the only cases where compare is {U,}Int32 and select
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
      useRegister(lhs), useRegister(rhs), useRegisterAtStart(ins->trueExpr()),
      useRegister(ins->falseExpr()), compTy, jsop);
  defineReuseInput(lir, ins, LWasmCompareAndSelect::IfTrueExprIndex);
}

void LIRGeneratorLOONG64::lowerWasmBuiltinTruncateToInt32(
    MWasmBuiltinTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

  if (opd->type() == MIRType::Double) {
    define(new (alloc()) LWasmBuiltinTruncateDToInt32(
               useRegister(opd), useFixed(ins->instance(), InstanceReg),
               LDefinition::BogusTemp()),
           ins);
    return;
  }

  define(new (alloc()) LWasmBuiltinTruncateFToInt32(
             useRegister(opd), useFixed(ins->instance(), InstanceReg),
             LDefinition::BogusTemp()),
         ins);
}

void LIRGeneratorLOONG64::lowerWasmBuiltinTruncateToInt64(
    MWasmBuiltinTruncateToInt64* ins) {
  MOZ_CRASH("We don't use it for this architecture");
}

void LIRGeneratorLOONG64::lowerWasmBuiltinDivI64(MWasmBuiltinDivI64* div) {
  MOZ_CRASH("We don't use runtime div for this architecture");
}

void LIRGeneratorLOONG64::lowerWasmBuiltinModI64(MWasmBuiltinModI64* mod) {
  MOZ_CRASH("We don't use runtime mod for this architecture");
}

void LIRGeneratorLOONG64::lowerAtomicLoad64(MLoadUnboxedScalar* ins) {
  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->storageType());

  auto* lir = new (alloc()) LAtomicLoad64(elements, index);
  defineInt64(lir, ins);
}

void LIRGeneratorLOONG64::lowerAtomicStore64(MStoreUnboxedScalar* ins) {
  LUse elements = useRegister(ins->elements());
  LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->writeType());
  LInt64Allocation value = useInt64Register(ins->value());

  add(new (alloc()) LAtomicStore64(elements, index, value), ins);
}

void LIRGenerator::visitBox(MBox* box) {
  MDefinition* opd = box->getOperand(0);

  // If the operand is a constant, emit near its uses.
  if (opd->isConstant() && box->canEmitAtUses()) {
    emitAtUses(box);
    return;
  }

  if (opd->isConstant()) {
    define(new (alloc()) LValue(opd->toConstant()->toJSValue()), box,
           LDefinition(LDefinition::BOX));
  } else {
    LBox* ins = new (alloc()) LBox(useRegister(opd), opd->type());
    define(ins, box, LDefinition(LDefinition::BOX));
  }
}

void LIRGenerator::visitUnbox(MUnbox* unbox) {
  MDefinition* box = unbox->getOperand(0);
  MOZ_ASSERT(box->type() == MIRType::Value);

  LInstructionHelper<1, BOX_PIECES, 0>* lir;
  if (IsFloatingPointType(unbox->type())) {
    MOZ_ASSERT(unbox->type() == MIRType::Double);
    lir = new (alloc()) LUnboxFloatingPoint(useBoxAtStart(box));
  } else if (unbox->fallible()) {
    // If the unbox is fallible, load the Value in a register first to
    // avoid multiple loads.
    lir = new (alloc()) LUnbox(useRegisterAtStart(box));
  } else {
    lir = new (alloc()) LUnbox(useAtStart(box));
  }

  if (unbox->fallible()) {
    assignSnapshot(lir, unbox->bailoutKind());
  }

  define(lir, unbox);
}

void LIRGenerator::visitAbs(MAbs* ins) {
  define(allocateAbs(ins, useRegisterAtStart(ins->input())), ins);
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

  lir->setOperand(0, useRegisterAtStart(lhs));
  lir->setOperand(1, willHaveDifferentLIRNodes(lhs, rhs)
                         ? useRegister(rhs)
                         : useRegisterAtStart(rhs));
  defineReuseInput(lir, ins, 0);
}

void LIRGenerator::visitPowHalf(MPowHalf* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Double);
  LPowHalfD* lir = new (alloc()) LPowHalfD(useRegisterAtStart(input));
  defineReuseInput(lir, ins, 0);
}

void LIRGenerator::visitExtendInt32ToInt64(MExtendInt32ToInt64* ins) {
  defineInt64(
      new (alloc()) LExtendInt32ToInt64(useRegisterAtStart(ins->input())), ins);
}

void LIRGenerator::visitSignExtendInt64(MSignExtendInt64* ins) {
  defineInt64(new (alloc())
                  LSignExtendInt64(useInt64RegisterAtStart(ins->input())),
              ins);
}

void LIRGenerator::visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Int64);
  MOZ_ASSERT(IsFloatingPointType(ins->type()));

  define(new (alloc()) LInt64ToFloatingPoint(useInt64Register(opd)), ins);
}

void LIRGenerator::visitSubstr(MSubstr* ins) {
  LSubstr* lir = new (alloc())
      LSubstr(useRegister(ins->string()), useRegister(ins->begin()),
              useRegister(ins->length()), temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCompareExchangeTypedArrayElement(
    MCompareExchangeTypedArrayElement* ins) {
  MOZ_ASSERT(!Scalar::isFloatingType(ins->arrayType()));
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());

  if (Scalar::isBigIntType(ins->arrayType())) {
    LInt64Allocation oldval = useInt64Register(ins->oldval());
    LInt64Allocation newval = useInt64Register(ins->newval());

    auto* lir = new (alloc())
        LCompareExchangeTypedArrayElement64(elements, index, oldval, newval);
    defineInt64(lir, ins);
    return;
  }

  const LAllocation oldval = useRegister(ins->oldval());
  const LAllocation newval = useRegister(ins->newval());

  // If the target is a floating register then we need a temp at the
  // CodeGenerator level for creating the result.

  LDefinition outTemp = LDefinition::BogusTemp();
  LDefinition valueTemp = LDefinition::BogusTemp();
  LDefinition offsetTemp = LDefinition::BogusTemp();
  LDefinition maskTemp = LDefinition::BogusTemp();

  if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type())) {
    outTemp = temp();
  }

  if (Scalar::byteSize(ins->arrayType()) < 4) {
    valueTemp = temp();
    offsetTemp = temp();
    maskTemp = temp();
  }

  LCompareExchangeTypedArrayElement* lir = new (alloc())
      LCompareExchangeTypedArrayElement(elements, index, oldval, newval,
                                        outTemp, valueTemp, offsetTemp,
                                        maskTemp);

  define(lir, ins);
}

void LIRGenerator::visitAtomicExchangeTypedArrayElement(
    MAtomicExchangeTypedArrayElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());

  if (Scalar::isBigIntType(ins->arrayType())) {
    LInt64Allocation value = useInt64Register(ins->value());

    auto* lir = new (alloc())
        LAtomicExchangeTypedArrayElement64(elements, index, value);
    defineInt64(lir, ins);
    return;
  }

  // If the target is a floating register then we need a temp at the
  // CodeGenerator level for creating the result.

  MOZ_ASSERT(ins->arrayType() <= Scalar::Uint32);

  const LAllocation value = useRegister(ins->value());

  LDefinition outTemp = LDefinition::BogusTemp();
  LDefinition valueTemp = LDefinition::BogusTemp();
  LDefinition offsetTemp = LDefinition::BogusTemp();
  LDefinition maskTemp = LDefinition::BogusTemp();

  if (ins->arrayType() == Scalar::Uint32) {
    MOZ_ASSERT(ins->type() == MIRType::Double);
    outTemp = temp();
  }

  if (Scalar::byteSize(ins->arrayType()) < 4) {
    valueTemp = temp();
    offsetTemp = temp();
    maskTemp = temp();
  }

  LAtomicExchangeTypedArrayElement* lir =
      new (alloc()) LAtomicExchangeTypedArrayElement(
          elements, index, value, outTemp, valueTemp, offsetTemp, maskTemp);

  define(lir, ins);
}

void LIRGenerator::visitAtomicTypedArrayElementBinop(
    MAtomicTypedArrayElementBinop* ins) {
  MOZ_ASSERT(ins->arrayType() != Scalar::Uint8Clamped);
  MOZ_ASSERT(!Scalar::isFloatingType(ins->arrayType()));
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());

  if (Scalar::isBigIntType(ins->arrayType())) {
    LInt64Allocation value = useInt64Register(ins->value());
    LInt64Definition temp = tempInt64();

    // Case 1: the result of the operation is not used.

    if (ins->isForEffect()) {
      auto* lir = new (alloc()) LAtomicTypedArrayElementBinopForEffect64(
          elements, index, value, temp);
      add(lir, ins);
      return;
    }

    // Case 2: the result of the operation is used.

    auto* lir = new (alloc())
        LAtomicTypedArrayElementBinop64(elements, index, value, temp);
    defineInt64(lir, ins);
    return;
  }

  LAllocation value = useRegister(ins->value());
  LDefinition valueTemp = LDefinition::BogusTemp();
  LDefinition offsetTemp = LDefinition::BogusTemp();
  LDefinition maskTemp = LDefinition::BogusTemp();

  if (Scalar::byteSize(ins->arrayType()) < 4) {
    valueTemp = temp();
    offsetTemp = temp();
    maskTemp = temp();
  }

  if (ins->isForEffect()) {
    LAtomicTypedArrayElementBinopForEffect* lir =
        new (alloc()) LAtomicTypedArrayElementBinopForEffect(
            elements, index, value, valueTemp, offsetTemp, maskTemp);
    add(lir, ins);
    return;
  }

  // For a Uint32Array with a known double result we need a temp for
  // the intermediate output.

  LDefinition outTemp = LDefinition::BogusTemp();

  if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type())) {
    outTemp = temp();
  }

  LAtomicTypedArrayElementBinop* lir =
      new (alloc()) LAtomicTypedArrayElementBinop(
          elements, index, value, outTemp, valueTemp, offsetTemp, maskTemp);
  define(lir, ins);
}

void LIRGenerator::visitReturnImpl(MDefinition* opd, bool isGenerator) {
  MOZ_ASSERT(opd->type() == MIRType::Value);

  LReturn* ins = new (alloc()) LReturn(isGenerator);
  ins->setOperand(0, useFixed(opd, JSReturnReg));
  add(ins);
}

void LIRGenerator::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
  MOZ_ASSERT_IF(ins->needsBoundsCheck(),
                boundsCheckLimit->type() == MIRType::Int32);

  LAllocation baseAlloc = useRegisterAtStart(base);

  LAllocation limitAlloc = ins->needsBoundsCheck()
                               ? useRegisterAtStart(boundsCheckLimit)
                               : LAllocation();

  // We have no memory-base value, meaning that HeapReg is to be used as the
  // memory base.  This follows from the definition of
  // FunctionCompiler::maybeLoadMemoryBase() in WasmIonCompile.cpp.
  MOZ_ASSERT(!ins->hasMemoryBase());
  auto* lir =
      new (alloc()) LAsmJSLoadHeap(baseAlloc, limitAlloc, LAllocation());
  define(lir, ins);
}

void LIRGenerator::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
  MOZ_ASSERT_IF(ins->needsBoundsCheck(),
                boundsCheckLimit->type() == MIRType::Int32);

  LAllocation baseAlloc = useRegisterAtStart(base);

  LAllocation limitAlloc = ins->needsBoundsCheck()
                               ? useRegisterAtStart(boundsCheckLimit)
                               : LAllocation();

  // See comment in LIRGenerator::visitAsmJSStoreHeap just above.
  MOZ_ASSERT(!ins->hasMemoryBase());
  add(new (alloc()) LAsmJSStoreHeap(baseAlloc, useRegisterAtStart(ins->value()),
                                    limitAlloc, LAllocation()),
      ins);
}

void LIRGenerator::visitWasmLoad(MWasmLoad* ins) {
  MDefinition* base = ins->base();
  // 'base' is a GPR but may be of either type. If it is 32-bit, it is
  // sign-extended on loongarch64 platform and we should explicitly promote it
  // to 64-bit by zero-extension when use it as an index register in memory
  // accesses.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);

  LAllocation memoryBase =
      ins->hasMemoryBase() ? LAllocation(useRegisterAtStart(ins->memoryBase()))
                           : LGeneralReg(HeapReg);

  LAllocation ptr = useRegisterAtStart(base);

  LDefinition ptrCopy = LDefinition::BogusTemp();
  if (ins->access().offset32()) {
    ptrCopy = tempCopy(base, 0);
  }

  if (ins->type() == MIRType::Int64) {
    auto* lir = new (alloc()) LWasmLoadI64(ptr, memoryBase, ptrCopy);
    defineInt64(lir, ins);
    return;
  }

  auto* lir = new (alloc()) LWasmLoad(ptr, memoryBase, ptrCopy);
  define(lir, ins);
}

void LIRGenerator::visitWasmStore(MWasmStore* ins) {
  MDefinition* base = ins->base();
  // See comment in visitWasmLoad re the type of 'base'.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);

  MDefinition* value = ins->value();
  LAllocation memoryBase =
      ins->hasMemoryBase() ? LAllocation(useRegisterAtStart(ins->memoryBase()))
                           : LGeneralReg(HeapReg);

  LAllocation baseAlloc = useRegisterAtStart(base);

  LDefinition ptrCopy = LDefinition::BogusTemp();
  if (ins->access().offset32()) {
    ptrCopy = tempCopy(base, 0);
  }

  if (ins->access().type() == Scalar::Int64) {
    LInt64Allocation valueAlloc = useInt64RegisterAtStart(value);
    auto* lir =
        new (alloc()) LWasmStoreI64(baseAlloc, valueAlloc, memoryBase, ptrCopy);
    add(lir, ins);
    return;
  }

  LAllocation valueAlloc = useRegisterAtStart(value);
  auto* lir =
      new (alloc()) LWasmStore(baseAlloc, valueAlloc, memoryBase, ptrCopy);
  add(lir, ins);
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

void LIRGenerator::visitWasmTruncateToInt64(MWasmTruncateToInt64* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

  defineInt64(new (alloc()) LWasmTruncateToInt64(useRegister(opd)), ins);
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

void LIRGenerator::visitWasmCompareExchangeHeap(MWasmCompareExchangeHeap* ins) {
  MDefinition* base = ins->base();
  // See comment in visitWasmLoad re the type of 'base'.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);
  LAllocation memoryBase = ins->hasMemoryBase()
                               ? LAllocation(useRegister(ins->memoryBase()))
                               : LGeneralReg(HeapReg);

  if (ins->access().type() == Scalar::Int64) {
    auto* lir = new (alloc()) LWasmCompareExchangeI64(
        useRegister(base), useInt64Register(ins->oldValue()),
        useInt64Register(ins->newValue()), memoryBase);
    defineInt64(lir, ins);
    return;
  }

  LDefinition valueTemp = LDefinition::BogusTemp();
  LDefinition offsetTemp = LDefinition::BogusTemp();
  LDefinition maskTemp = LDefinition::BogusTemp();

  if (ins->access().byteSize() < 4) {
    valueTemp = temp();
    offsetTemp = temp();
    maskTemp = temp();
  }

  auto* lir = new (alloc())
      LWasmCompareExchangeHeap(useRegister(base), useRegister(ins->oldValue()),
                               useRegister(ins->newValue()), memoryBase,
                               valueTemp, offsetTemp, maskTemp);

  define(lir, ins);
}

void LIRGenerator::visitWasmAtomicExchangeHeap(MWasmAtomicExchangeHeap* ins) {
  MDefinition* base = ins->base();
  // See comment in visitWasmLoad re the type of 'base'.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);
  LAllocation memoryBase = ins->hasMemoryBase()
                               ? LAllocation(useRegister(ins->memoryBase()))
                               : LGeneralReg(HeapReg);

  if (ins->access().type() == Scalar::Int64) {
    auto* lir = new (alloc()) LWasmAtomicExchangeI64(
        useRegister(base), useInt64Register(ins->value()), memoryBase);
    defineInt64(lir, ins);
    return;
  }

  LDefinition valueTemp = LDefinition::BogusTemp();
  LDefinition offsetTemp = LDefinition::BogusTemp();
  LDefinition maskTemp = LDefinition::BogusTemp();

  if (ins->access().byteSize() < 4) {
    valueTemp = temp();
    offsetTemp = temp();
    maskTemp = temp();
  }

  auto* lir = new (alloc())
      LWasmAtomicExchangeHeap(useRegister(base), useRegister(ins->value()),
                              memoryBase, valueTemp, offsetTemp, maskTemp);
  define(lir, ins);
}

void LIRGenerator::visitWasmAtomicBinopHeap(MWasmAtomicBinopHeap* ins) {
  MDefinition* base = ins->base();
  // See comment in visitWasmLoad re the type of 'base'.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);
  LAllocation memoryBase = ins->hasMemoryBase()
                               ? LAllocation(useRegister(ins->memoryBase()))
                               : LGeneralReg(HeapReg);

  if (ins->access().type() == Scalar::Int64) {
    auto* lir = new (alloc())
        LWasmAtomicBinopI64(useRegister(base), useInt64Register(ins->value()),
                            memoryBase, tempInt64());
    defineInt64(lir, ins);
    return;
  }

  LDefinition valueTemp = LDefinition::BogusTemp();
  LDefinition offsetTemp = LDefinition::BogusTemp();
  LDefinition maskTemp = LDefinition::BogusTemp();

  if (ins->access().byteSize() < 4) {
    valueTemp = temp();
    offsetTemp = temp();
    maskTemp = temp();
  }

  if (!ins->hasUses()) {
    LWasmAtomicBinopHeapForEffect* lir = new (alloc())
        LWasmAtomicBinopHeapForEffect(useRegister(base),
                                      useRegister(ins->value()), memoryBase,
                                      valueTemp, offsetTemp, maskTemp);
    add(lir, ins);
    return;
  }

  auto* lir = new (alloc())
      LWasmAtomicBinopHeap(useRegister(base), useRegister(ins->value()),
                           memoryBase, valueTemp, offsetTemp, maskTemp);

  define(lir, ins);
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
