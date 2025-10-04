/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/Lowering-arm64.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/arm64/Assembler-arm64.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "wasm/WasmFeatures.h"  // for wasm::ReportSimdAnalysis

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::FloorLog2;

LBoxAllocation LIRGeneratorARM64::useBoxFixed(MDefinition* mir, Register reg1,
                                              Register, bool useAtStart) {
  MOZ_ASSERT(mir->type() == MIRType::Value);

  ensureDefined(mir);
  return LBoxAllocation(LUse(reg1, mir->virtualRegister(), useAtStart));
}

LAllocation LIRGeneratorARM64::useByteOpRegister(MDefinition* mir) {
  return useRegister(mir);
}

LAllocation LIRGeneratorARM64::useByteOpRegisterAtStart(MDefinition* mir) {
  return useRegisterAtStart(mir);
}

LAllocation LIRGeneratorARM64::useByteOpRegisterOrNonDoubleConstant(
    MDefinition* mir) {
  return useRegisterOrNonDoubleConstant(mir);
}

LDefinition LIRGeneratorARM64::tempByteOpRegister() { return temp(); }

LDefinition LIRGeneratorARM64::tempToUnbox() { return temp(); }

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

  LUnboxBase* lir;
  if (IsFloatingPointType(unbox->type())) {
    lir = new (alloc())
        LUnboxFloatingPoint(useRegisterAtStart(box), unbox->type());
  } else if (unbox->fallible()) {
    // If the unbox is fallible, load the Value in a register first to
    // avoid multiple loads.
    lir = new (alloc()) LUnbox(useRegisterAtStart(box));
  } else {
    // FIXME: It should be possible to useAtStart() here, but the DEBUG
    // code in CodeGenerator::visitUnbox() needs to handle non-Register
    // cases. ARM64 doesn't have an Operand type.
    lir = new (alloc()) LUnbox(useRegisterAtStart(box));
  }

  if (unbox->fallible()) {
    assignSnapshot(lir, unbox->bailoutKind());
  }

  define(lir, unbox);
}

void LIRGenerator::visitReturnImpl(MDefinition* opd, bool isGenerator) {
  MOZ_ASSERT(opd->type() == MIRType::Value);

  LReturn* ins = new (alloc()) LReturn(isGenerator);
  ins->setOperand(0, useFixed(opd, JSReturnReg));
  add(ins);
}

// x = !y
void LIRGeneratorARM64::lowerForALU(LInstructionHelper<1, 1, 0>* ins,
                                    MDefinition* mir, MDefinition* input) {
  ins->setOperand(
      0, ins->snapshot() ? useRegister(input) : useRegisterAtStart(input));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

// z = x+y
void LIRGeneratorARM64::lowerForALU(LInstructionHelper<1, 2, 0>* ins,
                                    MDefinition* mir, MDefinition* lhs,
                                    MDefinition* rhs) {
  ins->setOperand(0,
                  ins->snapshot() ? useRegister(lhs) : useRegisterAtStart(lhs));
  ins->setOperand(1, ins->snapshot() ? useRegisterOrConstant(rhs)
                                     : useRegisterOrConstantAtStart(rhs));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

void LIRGeneratorARM64::lowerForFPU(LInstructionHelper<1, 1, 0>* ins,
                                    MDefinition* mir, MDefinition* input) {
  ins->setOperand(0, useRegisterAtStart(input));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template <size_t Temps>
void LIRGeneratorARM64::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins,
                                    MDefinition* mir, MDefinition* lhs,
                                    MDefinition* rhs) {
  ins->setOperand(0, useRegisterAtStart(lhs));
  ins->setOperand(1, useRegisterAtStart(rhs));
  define(
      ins, mir,
      LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template void LIRGeneratorARM64::lowerForFPU(LInstructionHelper<1, 2, 0>* ins,
                                             MDefinition* mir, MDefinition* lhs,
                                             MDefinition* rhs);
template void LIRGeneratorARM64::lowerForFPU(LInstructionHelper<1, 2, 1>* ins,
                                             MDefinition* mir, MDefinition* lhs,
                                             MDefinition* rhs);

void LIRGeneratorARM64::lowerForALUInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES, 0>* ins, MDefinition* mir,
    MDefinition* input) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(input));
  defineInt64(ins, mir);
}

// These all currently have codegen that depends on reuse but only because the
// masm API depends on that.  We need new three-address masm APIs, for both
// constant and variable rhs.
//
// MAdd => LAddI64
// MSub => LSubI64
// MBitAnd, MBitOr, MBitXor => LBitOpI64
void LIRGeneratorARM64::lowerForALUInt64(
    LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
  ins->setInt64Operand(INT64_PIECES, useInt64RegisterOrConstantAtStart(rhs));
  defineInt64(ins, mir);
}

void LIRGeneratorARM64::lowerForMulInt64(LMulI64* ins, MMul* mir,
                                         MDefinition* lhs, MDefinition* rhs) {
  ins->setInt64Operand(LMulI64::Lhs, useInt64RegisterAtStart(lhs));
  ins->setInt64Operand(LMulI64::Rhs, useInt64RegisterOrConstantAtStart(rhs));
  defineInt64(ins, mir);
}

template <size_t Temps>
void LIRGeneratorARM64::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));

  static_assert(LShiftI64::Rhs == INT64_PIECES,
                "Assume Rhs is located at INT64_PIECES.");
  static_assert(LRotateI64::Count == INT64_PIECES,
                "Assume Count is located at INT64_PIECES.");

  ins->setOperand(INT64_PIECES, useRegisterOrConstantAtStart(rhs));
  defineInt64(ins, mir);
}

template void LIRGeneratorARM64::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, 0>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorARM64::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, 1>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs);

void LIRGeneratorARM64::lowerForCompareI64AndBranch(MTest* mir, MCompare* comp,
                                                    JSOp op, MDefinition* left,
                                                    MDefinition* right,
                                                    MBasicBlock* ifTrue,
                                                    MBasicBlock* ifFalse) {
  auto* lir = new (alloc())
      LCompareI64AndBranch(comp, op, useInt64Register(left),
                           useInt64RegisterOrConstant(right), ifTrue, ifFalse);
  add(lir, mir);
}

void LIRGeneratorARM64::lowerForBitAndAndBranch(LBitAndAndBranch* baab,
                                                MInstruction* mir,
                                                MDefinition* lhs,
                                                MDefinition* rhs) {
  baab->setOperand(0, useRegisterAtStart(lhs));
  baab->setOperand(1, useRegisterOrConstantAtStart(rhs));
  add(baab, mir);
}

void LIRGeneratorARM64::lowerWasmBuiltinTruncateToInt32(
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

void LIRGeneratorARM64::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition,
                                             LBlock* block, size_t lirIndex) {
  lowerTypedPhiInput(phi, inputPosition, block, lirIndex);
}

void LIRGeneratorARM64::lowerForShift(LInstructionHelper<1, 2, 0>* ins,
                                      MDefinition* mir, MDefinition* lhs,
                                      MDefinition* rhs) {
  ins->setOperand(0, useRegister(lhs));
  ins->setOperand(1, useRegisterOrConstant(rhs));
  define(ins, mir);
}

void LIRGeneratorARM64::lowerDivI(MDiv* div) {
  if (div->isUnsigned()) {
    lowerUDiv(div);
    return;
  }

  if (div->rhs()->isConstant()) {
    LAllocation lhs = useRegister(div->lhs());
    int32_t rhs = div->rhs()->toConstant()->toInt32();
    int32_t shift = mozilla::FloorLog2(mozilla::Abs(rhs));

    if (rhs != 0 && uint32_t(1) << shift == mozilla::Abs(rhs)) {
      LDivPowTwoI* lir = new (alloc()) LDivPowTwoI(lhs, shift, rhs < 0);
      if (div->fallible()) {
        assignSnapshot(lir, div->bailoutKind());
      }
      define(lir, div);
      return;
    }
    if (rhs != 0) {
      LDivConstantI* lir = new (alloc()) LDivConstantI(lhs, rhs, temp());
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

void LIRGeneratorARM64::lowerNegI(MInstruction* ins, MDefinition* input) {
  define(new (alloc()) LNegI(useRegisterAtStart(input)), ins);
}

void LIRGeneratorARM64::lowerNegI64(MInstruction* ins, MDefinition* input) {
  defineInt64(new (alloc()) LNegI64(useInt64RegisterAtStart(input)), ins);
}

void LIRGeneratorARM64::lowerMulI(MMul* mul, MDefinition* lhs,
                                  MDefinition* rhs) {
  LMulI* lir = new (alloc()) LMulI;
  if (mul->fallible()) {
    assignSnapshot(lir, mul->bailoutKind());
  }
  lowerForALU(lir, mul, lhs, rhs);
}

void LIRGeneratorARM64::lowerModI(MMod* mod) {
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
    }
  }

  LModI* lir =
      new (alloc()) LModI(useRegister(mod->lhs()), useRegister(mod->rhs()));
  if (mod->fallible()) {
    assignSnapshot(lir, mod->bailoutKind());
  }
  define(lir, mod);
}

void LIRGeneratorARM64::lowerDivI64(MDiv* div) {
  if (div->isUnsigned()) {
    lowerUDivI64(div);
    return;
  }

  LDivOrModI64* lir = new (alloc())
      LDivOrModI64(useRegister(div->lhs()), useRegister(div->rhs()));
  defineInt64(lir, div);
}

void LIRGeneratorARM64::lowerUDivI64(MDiv* div) {
  LUDivOrModI64* lir = new (alloc())
      LUDivOrModI64(useRegister(div->lhs()), useRegister(div->rhs()));
  defineInt64(lir, div);
}

void LIRGeneratorARM64::lowerUModI64(MMod* mod) {
  LUDivOrModI64* lir = new (alloc())
      LUDivOrModI64(useRegister(mod->lhs()), useRegister(mod->rhs()));
  defineInt64(lir, mod);
}

void LIRGeneratorARM64::lowerWasmBuiltinDivI64(MWasmBuiltinDivI64* div) {
  MOZ_CRASH("We don't use runtime div for this architecture");
}

void LIRGeneratorARM64::lowerModI64(MMod* mod) {
  if (mod->isUnsigned()) {
    lowerUModI64(mod);
    return;
  }

  LDivOrModI64* lir = new (alloc())
      LDivOrModI64(useRegister(mod->lhs()), useRegister(mod->rhs()));
  defineInt64(lir, mod);
}

void LIRGeneratorARM64::lowerWasmBuiltinModI64(MWasmBuiltinModI64* mod) {
  MOZ_CRASH("We don't use runtime mod for this architecture");
}

void LIRGenerator::visitPowHalf(MPowHalf* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Double);
  LPowHalfD* lir = new (alloc()) LPowHalfD(useRegister(input));
  define(lir, ins);
}

void LIRGeneratorARM64::lowerWasmSelectI(MWasmSelect* select) {
  if (select->type() == MIRType::Simd128) {
    LAllocation t = useRegisterAtStart(select->trueExpr());
    LAllocation f = useRegister(select->falseExpr());
    LAllocation c = useRegister(select->condExpr());
    auto* lir = new (alloc()) LWasmSelect(t, f, c);
    defineReuseInput(lir, select, LWasmSelect::TrueExprIndex);
  } else {
    LAllocation t = useRegisterAtStart(select->trueExpr());
    LAllocation f = useRegisterAtStart(select->falseExpr());
    LAllocation c = useRegisterAtStart(select->condExpr());
    define(new (alloc()) LWasmSelect(t, f, c), select);
  }
}

void LIRGeneratorARM64::lowerWasmSelectI64(MWasmSelect* select) {
  LInt64Allocation t = useInt64RegisterAtStart(select->trueExpr());
  LInt64Allocation f = useInt64RegisterAtStart(select->falseExpr());
  LAllocation c = useRegisterAtStart(select->condExpr());
  defineInt64(new (alloc()) LWasmSelectI64(t, f, c), select);
}

// On arm64 we specialize the cases: compare is {{U,}Int32, {U,}Int64},
// Float32, Double}, and select is {{U,}Int32, {U,}Int64}, Float32, Double},
// independently.
bool LIRGeneratorARM64::canSpecializeWasmCompareAndSelect(
    MCompare::CompareType compTy, MIRType insTy) {
  return (insTy == MIRType::Int32 || insTy == MIRType::Int64 ||
          insTy == MIRType::Float32 || insTy == MIRType::Double) &&
         (compTy == MCompare::Compare_Int32 ||
          compTy == MCompare::Compare_UInt32 ||
          compTy == MCompare::Compare_Int64 ||
          compTy == MCompare::Compare_UInt64 ||
          compTy == MCompare::Compare_Float32 ||
          compTy == MCompare::Compare_Double);
}

void LIRGeneratorARM64::lowerWasmCompareAndSelect(MWasmSelect* ins,
                                                  MDefinition* lhs,
                                                  MDefinition* rhs,
                                                  MCompare::CompareType compTy,
                                                  JSOp jsop) {
  MOZ_ASSERT(canSpecializeWasmCompareAndSelect(compTy, ins->type()));
  LAllocation rhsAlloc;
  if (compTy == MCompare::Compare_Float32 ||
      compTy == MCompare::Compare_Double) {
    rhsAlloc = useRegisterAtStart(rhs);
  } else if (compTy == MCompare::Compare_Int32 ||
             compTy == MCompare::Compare_UInt32 ||
             compTy == MCompare::Compare_Int64 ||
             compTy == MCompare::Compare_UInt64) {
    rhsAlloc = useRegisterOrConstantAtStart(rhs);
  } else {
    MOZ_CRASH("Unexpected type");
  }
  auto* lir = new (alloc())
      LWasmCompareAndSelect(useRegisterAtStart(lhs), rhsAlloc, compTy, jsop,
                            useRegisterAtStart(ins->trueExpr()),
                            useRegisterAtStart(ins->falseExpr()));
  define(lir, ins);
}

void LIRGenerator::visitAbs(MAbs* ins) {
  define(allocateAbs(ins, useRegisterAtStart(ins->input())), ins);
}

LTableSwitch* LIRGeneratorARM64::newLTableSwitch(const LAllocation& in,
                                                 const LDefinition& inputCopy,
                                                 MTableSwitch* tableswitch) {
  return new (alloc()) LTableSwitch(in, inputCopy, temp(), tableswitch);
}

LTableSwitchV* LIRGeneratorARM64::newLTableSwitchV(MTableSwitch* tableswitch) {
  return new (alloc()) LTableSwitchV(useBox(tableswitch->getOperand(0)), temp(),
                                     tempDouble(), temp(), tableswitch);
}

void LIRGeneratorARM64::lowerUrshD(MUrsh* mir) {
  MDefinition* lhs = mir->lhs();
  MDefinition* rhs = mir->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Int32);
  MOZ_ASSERT(rhs->type() == MIRType::Int32);

  LUrshD* lir = new (alloc())
      LUrshD(useRegister(lhs), useRegisterOrConstant(rhs), temp());
  define(lir, mir);
}

void LIRGeneratorARM64::lowerPowOfTwoI(MPow* mir) {
  int32_t base = mir->input()->toConstant()->toInt32();
  MDefinition* power = mir->power();

  auto* lir = new (alloc()) LPowOfTwoI(useRegister(power), base);
  assignSnapshot(lir, mir->bailoutKind());
  define(lir, mir);
}

void LIRGeneratorARM64::lowerBigIntLsh(MBigIntLsh* ins) {
  auto* lir = new (alloc()) LBigIntLsh(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorARM64::lowerBigIntRsh(MBigIntRsh* ins) {
  auto* lir = new (alloc()) LBigIntRsh(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorARM64::lowerBigIntDiv(MBigIntDiv* ins) {
  auto* lir = new (alloc()) LBigIntDiv(useRegister(ins->lhs()),
                                       useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorARM64::lowerBigIntMod(MBigIntMod* ins) {
  auto* lir = new (alloc()) LBigIntMod(useRegister(ins->lhs()),
                                       useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

#ifdef ENABLE_WASM_SIMD

bool LIRGeneratorARM64::canFoldReduceSimd128AndBranch(wasm::SimdOp op) {
  switch (op) {
    case wasm::SimdOp::V128AnyTrue:
    case wasm::SimdOp::I8x16AllTrue:
    case wasm::SimdOp::I16x8AllTrue:
    case wasm::SimdOp::I32x4AllTrue:
    case wasm::SimdOp::I64x2AllTrue:
      return true;
    default:
      return false;
  }
}

bool LIRGeneratorARM64::canEmitWasmReduceSimd128AtUses(
    MWasmReduceSimd128* ins) {
  if (!ins->canEmitAtUses()) {
    return false;
  }
  // Only specific ops generating int32.
  if (ins->type() != MIRType::Int32) {
    return false;
  }
  if (!canFoldReduceSimd128AndBranch(ins->simdOp())) {
    return false;
  }
  // If never used then defer (it will be removed).
  MUseIterator iter(ins->usesBegin());
  if (iter == ins->usesEnd()) {
    return true;
  }
  // We require an MTest consumer.
  MNode* node = iter->consumer();
  if (!node->isDefinition() || !node->toDefinition()->isTest()) {
    return false;
  }
  // Defer only if there's only one use.
  iter++;
  return iter == ins->usesEnd();
}

#endif

void LIRGenerator::visitWasmNeg(MWasmNeg* ins) {
  switch (ins->type()) {
    case MIRType::Int32:
      define(new (alloc()) LNegI(useRegisterAtStart(ins->input())), ins);
      break;
    case MIRType::Float32:
      define(new (alloc()) LNegF(useRegisterAtStart(ins->input())), ins);
      break;
    case MIRType::Double:
      define(new (alloc()) LNegD(useRegisterAtStart(ins->input())), ins);
      break;
    default:
      MOZ_CRASH("unexpected type");
  }
}

void LIRGeneratorARM64::lowerUDiv(MDiv* div) {
  LAllocation lhs = useRegister(div->lhs());
  if (div->rhs()->isConstant()) {
    // NOTE: the result of toInt32 is coerced to uint32_t.
    uint32_t rhs = div->rhs()->toConstant()->toInt32();
    int32_t shift = mozilla::FloorLog2(rhs);

    if (rhs != 0 && uint32_t(1) << shift == rhs) {
      LDivPowTwoI* lir = new (alloc()) LDivPowTwoI(lhs, shift, false);
      if (div->fallible()) {
        assignSnapshot(lir, div->bailoutKind());
      }
      define(lir, div);
      return;
    }

    LUDivConstantI* lir = new (alloc()) LUDivConstantI(lhs, rhs, temp());
    if (div->fallible()) {
      assignSnapshot(lir, div->bailoutKind());
    }
    define(lir, div);
    return;
  }

  // Generate UDiv
  LAllocation rhs = useRegister(div->rhs());
  LDefinition remainder = LDefinition::BogusTemp();
  if (!div->canTruncateRemainder()) {
    remainder = temp();
  }

  LUDiv* lir = new (alloc()) LUDiv(lhs, rhs, remainder);
  if (div->fallible()) {
    assignSnapshot(lir, div->bailoutKind());
  }
  define(lir, div);
}

void LIRGeneratorARM64::lowerUMod(MMod* mod) {
  LUMod* lir = new (alloc())
      LUMod(useRegister(mod->getOperand(0)), useRegister(mod->getOperand(1)));
  if (mod->fallible()) {
    assignSnapshot(lir, mod->bailoutKind());
  }
  define(lir, mod);
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

void LIRGenerator::visitWasmCompareExchangeHeap(MWasmCompareExchangeHeap* ins) {
  MDefinition* base = ins->base();
  // See comment in visitWasmLoad re the type of 'base'.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);

  LAllocation memoryBase = ins->hasMemoryBase()
                               ? LAllocation(useRegister(ins->memoryBase()))
                               : LGeneralReg(HeapReg);

  // Note, the access type may be Int64 here.

  LWasmCompareExchangeHeap* lir = new (alloc())
      LWasmCompareExchangeHeap(useRegister(base), useRegister(ins->oldValue()),
                               useRegister(ins->newValue()), memoryBase);

  define(lir, ins);
}

void LIRGenerator::visitWasmAtomicExchangeHeap(MWasmAtomicExchangeHeap* ins) {
  MDefinition* base = ins->base();
  // See comment in visitWasmLoad re the type of 'base'.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);

  LAllocation memoryBase = ins->hasMemoryBase()
                               ? LAllocation(useRegister(ins->memoryBase()))
                               : LGeneralReg(HeapReg);

  // Note, the access type may be Int64 here.

  LWasmAtomicExchangeHeap* lir = new (alloc()) LWasmAtomicExchangeHeap(
      useRegister(base), useRegister(ins->value()), memoryBase);
  define(lir, ins);
}

void LIRGenerator::visitWasmAtomicBinopHeap(MWasmAtomicBinopHeap* ins) {
  MDefinition* base = ins->base();
  // See comment in visitWasmLoad re the type of 'base'.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);

  LAllocation memoryBase = ins->hasMemoryBase()
                               ? LAllocation(useRegister(ins->memoryBase()))
                               : LGeneralReg(HeapReg);

  // Note, the access type may be Int64 here.

  if (!ins->hasUses()) {
    LWasmAtomicBinopHeapForEffect* lir = new (alloc())
        LWasmAtomicBinopHeapForEffect(useRegister(base),
                                      useRegister(ins->value()),
                                      /* flagTemp= */ temp(), memoryBase);
    add(lir, ins);
    return;
  }

  LWasmAtomicBinopHeap* lir = new (alloc())
      LWasmAtomicBinopHeap(useRegister(base), useRegister(ins->value()),
                           /* temp= */ LDefinition::BogusTemp(),
                           /* flagTemp= */ temp(), memoryBase);
  define(lir, ins);
}

void LIRGeneratorARM64::lowerTruncateDToInt32(MTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double);
  define(new (alloc())
             LTruncateDToInt32(useRegister(opd), LDefinition::BogusTemp()),
         ins);
}

void LIRGeneratorARM64::lowerTruncateFToInt32(MTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Float32);
  define(new (alloc())
             LTruncateFToInt32(useRegister(opd), LDefinition::BogusTemp()),
         ins);
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

  LAllocation value = useRegister(ins->value());

  if (Scalar::isBigIntType(ins->arrayType())) {
    LInt64Definition temp1 = tempInt64();
    LInt64Definition temp2 = tempInt64();

    // Case 1: the result of the operation is not used.
    //
    // We can omit allocating the result BigInt.

    if (ins->isForEffect()) {
      auto* lir = new (alloc()) LAtomicTypedArrayElementBinopForEffect64(
          elements, index, value, temp1, temp2);
      add(lir, ins);
      return;
    }

    // Case 2: the result of the operation is used.

    auto* lir = new (alloc())
        LAtomicTypedArrayElementBinop64(elements, index, value, temp1, temp2);
    define(lir, ins);
    assignSafepoint(lir, ins);
    return;
  }

  if (ins->isForEffect()) {
    auto* lir = new (alloc())
        LAtomicTypedArrayElementBinopForEffect(elements, index, value, temp());
    add(lir, ins);
    return;
  }

  LDefinition tempDef1 = temp();
  LDefinition tempDef2 = LDefinition::BogusTemp();
  if (ins->arrayType() == Scalar::Uint32) {
    tempDef2 = temp();
  }

  LAtomicTypedArrayElementBinop* lir = new (alloc())
      LAtomicTypedArrayElementBinop(elements, index, value, tempDef1, tempDef2);

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
    LInt64Definition temp1 = tempInt64();
    LInt64Definition temp2 = tempInt64();

    auto* lir = new (alloc()) LCompareExchangeTypedArrayElement64(
        elements, index, oldval, newval, temp1, temp2);
    define(lir, ins);
    assignSafepoint(lir, ins);
    return;
  }

  // If the target is an FPReg then we need a temporary at the CodeGenerator
  // level for creating the result.

  LDefinition outTemp = LDefinition::BogusTemp();
  if (ins->arrayType() == Scalar::Uint32) {
    outTemp = temp();
  }

  LCompareExchangeTypedArrayElement* lir =
      new (alloc()) LCompareExchangeTypedArrayElement(elements, index, oldval,
                                                      newval, outTemp);

  define(lir, ins);
}

void LIRGenerator::visitAtomicExchangeTypedArrayElement(
    MAtomicExchangeTypedArrayElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());

  const LAllocation value = useRegister(ins->value());

  if (Scalar::isBigIntType(ins->arrayType())) {
    LInt64Definition temp1 = tempInt64();
    LDefinition temp2 = temp();

    auto* lir = new (alloc()) LAtomicExchangeTypedArrayElement64(
        elements, index, value, temp1, temp2);
    define(lir, ins);
    assignSafepoint(lir, ins);
    return;
  }

  MOZ_ASSERT(ins->arrayType() <= Scalar::Uint32);

  LDefinition tempDef = LDefinition::BogusTemp();
  if (ins->arrayType() == Scalar::Uint32) {
    tempDef = temp();
  }

  LAtomicExchangeTypedArrayElement* lir = new (alloc())
      LAtomicExchangeTypedArrayElement(elements, index, value, tempDef);

  define(lir, ins);
}

void LIRGeneratorARM64::lowerAtomicLoad64(MLoadUnboxedScalar* ins) {
  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->storageType());

  auto* lir = new (alloc()) LAtomicLoad64(elements, index, temp(), tempInt64());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorARM64::lowerAtomicStore64(MStoreUnboxedScalar* ins) {
  LUse elements = useRegister(ins->elements());
  LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->writeType());
  LAllocation value = useRegister(ins->value());

  add(new (alloc()) LAtomicStore64(elements, index, value, tempInt64()), ins);
}

void LIRGenerator::visitSubstr(MSubstr* ins) {
  LSubstr* lir = new (alloc())
      LSubstr(useRegister(ins->string()), useRegister(ins->begin()),
              useRegister(ins->length()), temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitWasmTruncateToInt64(MWasmTruncateToInt64* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

  defineInt64(new (alloc()) LWasmTruncateToInt64(useRegister(opd)), ins);
}

void LIRGeneratorARM64::lowerWasmBuiltinTruncateToInt64(
    MWasmBuiltinTruncateToInt64* ins) {
  MOZ_CRASH("We don't use WasmBuiltinTruncateToInt64 for arm64");
}

void LIRGeneratorARM64::lowerBuiltinInt64ToFloatingPoint(
    MBuiltinInt64ToFloatingPoint* ins) {
  MOZ_CRASH("We don't use it for this architecture");
}

void LIRGenerator::visitWasmLoad(MWasmLoad* ins) {
  MDefinition* base = ins->base();
  // 'base' is a GPR but may be of either type.  If it is 32-bit it is
  // zero-extended and can act as 64-bit.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);

  LAllocation memoryBase =
      ins->hasMemoryBase() ? LAllocation(useRegisterAtStart(ins->memoryBase()))
                           : LGeneralReg(HeapReg);
  LAllocation ptr = useRegisterOrConstantAtStart(base);

  if (ins->type() == MIRType::Int64) {
    auto* lir = new (alloc()) LWasmLoadI64(ptr, memoryBase);
    defineInt64(lir, ins);
  } else {
    auto* lir = new (alloc()) LWasmLoad(ptr, memoryBase);
    define(lir, ins);
  }
}

void LIRGenerator::visitWasmStore(MWasmStore* ins) {
  MDefinition* base = ins->base();
  // See comment in visitWasmLoad re the type of 'base'.
  MOZ_ASSERT(base->type() == MIRType::Int32 || base->type() == MIRType::Int64);

  MDefinition* value = ins->value();

  LAllocation memoryBase =
      ins->hasMemoryBase() ? LAllocation(useRegisterAtStart(ins->memoryBase()))
                           : LGeneralReg(HeapReg);

  if (ins->access().type() == Scalar::Int64) {
    LAllocation baseAlloc = useRegisterOrConstantAtStart(base);
    LInt64Allocation valueAlloc = useInt64RegisterAtStart(value);
    auto* lir = new (alloc()) LWasmStoreI64(baseAlloc, valueAlloc, memoryBase);
    add(lir, ins);
    return;
  }

  LAllocation baseAlloc = useRegisterOrConstantAtStart(base);
  LAllocation valueAlloc = useRegisterAtStart(value);
  auto* lir = new (alloc()) LWasmStore(baseAlloc, valueAlloc, memoryBase);
  add(lir, ins);
}

void LIRGenerator::visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Int64);
  MOZ_ASSERT(IsFloatingPointType(ins->type()));

  define(new (alloc()) LInt64ToFloatingPoint(useInt64Register(opd)), ins);
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

  lir->setOperand(0, useRegisterAtStart(lhs));
  lir->setOperand(1, willHaveDifferentLIRNodes(lhs, rhs)
                         ? useRegister(rhs)
                         : useRegisterAtStart(rhs));
  // The copySignDouble and copySignFloat32 are optimized for lhs == output.
  // It also prevents rhs == output when lhs != output, avoids clobbering.
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

void LIRGenerator::visitWasmTernarySimd128(MWasmTernarySimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MOZ_ASSERT(ins->v0()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->v1()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->v2()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  switch (ins->simdOp()) {
    case wasm::SimdOp::V128Bitselect: {
      auto* lir = new (alloc()) LWasmTernarySimd128(
          ins->simdOp(), useRegister(ins->v0()), useRegister(ins->v1()),
          useRegisterAtStart(ins->v2()));
      // On ARM64, control register is used as output at machine instruction.
      defineReuseInput(lir, ins, LWasmTernarySimd128::V2);
      break;
    }
    case wasm::SimdOp::F32x4RelaxedMadd:
    case wasm::SimdOp::F32x4RelaxedNmadd:
    case wasm::SimdOp::F64x2RelaxedMadd:
    case wasm::SimdOp::F64x2RelaxedNmadd: {
      auto* lir = new (alloc()) LWasmTernarySimd128(
          ins->simdOp(), useRegister(ins->v0()), useRegister(ins->v1()),
          useRegisterAtStart(ins->v2()));
      defineReuseInput(lir, ins, LWasmTernarySimd128::V2);
      break;
    }
    case wasm::SimdOp::I32x4DotI8x16I7x16AddS: {
      auto* lir = new (alloc()) LWasmTernarySimd128(
          ins->simdOp(), useRegister(ins->v0()), useRegister(ins->v1()),
          useRegisterAtStart(ins->v2()), tempSimd128());
      defineReuseInput(lir, ins, LWasmTernarySimd128::V2);
      break;
    }
    case wasm::SimdOp::I8x16RelaxedLaneSelect:
    case wasm::SimdOp::I16x8RelaxedLaneSelect:
    case wasm::SimdOp::I32x4RelaxedLaneSelect:
    case wasm::SimdOp::I64x2RelaxedLaneSelect: {
      auto* lir = new (alloc()) LWasmTernarySimd128(
          ins->simdOp(), useRegister(ins->v0()), useRegister(ins->v1()),
          useRegisterAtStart(ins->v2()));
      defineReuseInput(lir, ins, LWasmTernarySimd128::V2);
      break;
    }
    default:
      MOZ_CRASH("NYI");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmBinarySimd128(MWasmBinarySimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();
  wasm::SimdOp op = ins->simdOp();

  MOZ_ASSERT(lhs->type() == MIRType::Simd128);
  MOZ_ASSERT(rhs->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  LAllocation lhsAlloc = useRegisterAtStart(lhs);
  LAllocation rhsAlloc = useRegisterAtStart(rhs);
  LDefinition tempReg0 = LDefinition::BogusTemp();
  LDefinition tempReg1 = LDefinition::BogusTemp();
  if (op == wasm::SimdOp::I64x2Mul) {
    tempReg0 = tempSimd128();
    tempReg1 = tempSimd128();
  }
  auto* lir = new (alloc())
      LWasmBinarySimd128(op, lhsAlloc, rhsAlloc, tempReg0, tempReg1);
  define(lir, ins);
#else
  MOZ_CRASH("No SIMD");
#endif
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
#ifdef ENABLE_WASM_SIMD
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Simd128);
  MOZ_ASSERT(rhs->type() == MIRType::Int32);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  if (rhs->isConstant()) {
    int32_t shiftCount = rhs->toConstant()->toInt32();
    switch (ins->simdOp()) {
      case wasm::SimdOp::I8x16Shl:
      case wasm::SimdOp::I8x16ShrU:
      case wasm::SimdOp::I8x16ShrS:
        shiftCount &= 7;
        break;
      case wasm::SimdOp::I16x8Shl:
      case wasm::SimdOp::I16x8ShrU:
      case wasm::SimdOp::I16x8ShrS:
        shiftCount &= 15;
        break;
      case wasm::SimdOp::I32x4Shl:
      case wasm::SimdOp::I32x4ShrU:
      case wasm::SimdOp::I32x4ShrS:
        shiftCount &= 31;
        break;
      case wasm::SimdOp::I64x2Shl:
      case wasm::SimdOp::I64x2ShrU:
      case wasm::SimdOp::I64x2ShrS:
        shiftCount &= 63;
        break;
      default:
        MOZ_CRASH("Unexpected shift operation");
    }
#  ifdef DEBUG
    js::wasm::ReportSimdAnalysis("shift -> constant shift");
#  endif
    auto* lir = new (alloc())
        LWasmConstantShiftSimd128(useRegisterAtStart(lhs), shiftCount);
    define(lir, ins);
    return;
  }

#  ifdef DEBUG
  js::wasm::ReportSimdAnalysis("shift -> variable shift");
#  endif

  LAllocation lhsDestAlloc = useRegisterAtStart(lhs);
  LAllocation rhsAlloc = useRegisterAtStart(rhs);
  auto* lir = new (alloc()) LWasmVariableShiftSimd128(lhsDestAlloc, rhsAlloc,
                                                      LDefinition::BogusTemp());
  define(lir, ins);
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmShuffleSimd128(MWasmShuffleSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MOZ_ASSERT(ins->lhs()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->rhs()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  SimdShuffle s = ins->shuffle();
  switch (s.opd) {
    case SimdShuffle::Operand::LEFT:
    case SimdShuffle::Operand::RIGHT: {
      LAllocation src;
      switch (*s.permuteOp) {
        case SimdPermuteOp::MOVE:
        case SimdPermuteOp::BROADCAST_8x16:
        case SimdPermuteOp::BROADCAST_16x8:
        case SimdPermuteOp::PERMUTE_8x16:
        case SimdPermuteOp::PERMUTE_16x8:
        case SimdPermuteOp::PERMUTE_32x4:
        case SimdPermuteOp::ROTATE_RIGHT_8x16:
        case SimdPermuteOp::SHIFT_LEFT_8x16:
        case SimdPermuteOp::SHIFT_RIGHT_8x16:
        case SimdPermuteOp::REVERSE_16x8:
        case SimdPermuteOp::REVERSE_32x4:
        case SimdPermuteOp::REVERSE_64x2:
        case SimdPermuteOp::ZERO_EXTEND_8x16_TO_16x8:
        case SimdPermuteOp::ZERO_EXTEND_8x16_TO_32x4:
        case SimdPermuteOp::ZERO_EXTEND_8x16_TO_64x2:
        case SimdPermuteOp::ZERO_EXTEND_16x8_TO_32x4:
        case SimdPermuteOp::ZERO_EXTEND_16x8_TO_64x2:
        case SimdPermuteOp::ZERO_EXTEND_32x4_TO_64x2:
          break;
        default:
          MOZ_CRASH("Unexpected operator");
      }
      if (s.opd == SimdShuffle::Operand::LEFT) {
        src = useRegisterAtStart(ins->lhs());
      } else {
        src = useRegisterAtStart(ins->rhs());
      }
      auto* lir =
          new (alloc()) LWasmPermuteSimd128(src, *s.permuteOp, s.control);
      define(lir, ins);
      break;
    }
    case SimdShuffle::Operand::BOTH:
    case SimdShuffle::Operand::BOTH_SWAPPED: {
      LDefinition temp = LDefinition::BogusTemp();
      LAllocation lhs;
      LAllocation rhs;
      if (s.opd == SimdShuffle::Operand::BOTH) {
        lhs = useRegisterAtStart(ins->lhs());
        rhs = useRegisterAtStart(ins->rhs());
      } else {
        lhs = useRegisterAtStart(ins->rhs());
        rhs = useRegisterAtStart(ins->lhs());
      }
      auto* lir = new (alloc())
          LWasmShuffleSimd128(lhs, rhs, temp, *s.shuffleOp, s.control);
      define(lir, ins);
      break;
    }
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmReplaceLaneSimd128(MWasmReplaceLaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MOZ_ASSERT(ins->lhs()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  // Optimal code generation reuses the lhs register because the rhs scalar is
  // merged into a vector lhs.
  LAllocation lhs = useRegisterAtStart(ins->lhs());
  if (ins->rhs()->type() == MIRType::Int64) {
    auto* lir = new (alloc())
        LWasmReplaceInt64LaneSimd128(lhs, useInt64Register(ins->rhs()));
    defineReuseInput(lir, ins, 0);
  } else {
    auto* lir =
        new (alloc()) LWasmReplaceLaneSimd128(lhs, useRegister(ins->rhs()));
    defineReuseInput(lir, ins, 0);
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmScalarToSimd128(MWasmScalarToSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  switch (ins->input()->type()) {
    case MIRType::Int64: {
      // 64-bit integer splats.
      // Load-and-(sign|zero)extend.
      auto* lir = new (alloc())
          LWasmInt64ToSimd128(useInt64RegisterAtStart(ins->input()));
      define(lir, ins);
      break;
    }
    case MIRType::Float32:
    case MIRType::Double: {
      // Floating-point splats.
      auto* lir =
          new (alloc()) LWasmScalarToSimd128(useRegisterAtStart(ins->input()));
      define(lir, ins);
      break;
    }
    default: {
      // 32-bit integer splats.
      auto* lir =
          new (alloc()) LWasmScalarToSimd128(useRegisterAtStart(ins->input()));
      define(lir, ins);
      break;
    }
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmUnarySimd128(MWasmUnarySimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MOZ_ASSERT(ins->input()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  LDefinition tempReg = LDefinition::BogusTemp();
  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16Neg:
    case wasm::SimdOp::I16x8Neg:
    case wasm::SimdOp::I32x4Neg:
    case wasm::SimdOp::I64x2Neg:
    case wasm::SimdOp::F32x4Neg:
    case wasm::SimdOp::F64x2Neg:
    case wasm::SimdOp::F32x4Abs:
    case wasm::SimdOp::F64x2Abs:
    case wasm::SimdOp::V128Not:
    case wasm::SimdOp::F32x4Sqrt:
    case wasm::SimdOp::F64x2Sqrt:
    case wasm::SimdOp::I8x16Abs:
    case wasm::SimdOp::I16x8Abs:
    case wasm::SimdOp::I32x4Abs:
    case wasm::SimdOp::I64x2Abs:
    case wasm::SimdOp::I32x4TruncSatF32x4S:
    case wasm::SimdOp::F32x4ConvertI32x4U:
    case wasm::SimdOp::I32x4TruncSatF32x4U:
    case wasm::SimdOp::I16x8ExtendLowI8x16S:
    case wasm::SimdOp::I16x8ExtendHighI8x16S:
    case wasm::SimdOp::I16x8ExtendLowI8x16U:
    case wasm::SimdOp::I16x8ExtendHighI8x16U:
    case wasm::SimdOp::I32x4ExtendLowI16x8S:
    case wasm::SimdOp::I32x4ExtendHighI16x8S:
    case wasm::SimdOp::I32x4ExtendLowI16x8U:
    case wasm::SimdOp::I32x4ExtendHighI16x8U:
    case wasm::SimdOp::I64x2ExtendLowI32x4S:
    case wasm::SimdOp::I64x2ExtendHighI32x4S:
    case wasm::SimdOp::I64x2ExtendLowI32x4U:
    case wasm::SimdOp::I64x2ExtendHighI32x4U:
    case wasm::SimdOp::F32x4ConvertI32x4S:
    case wasm::SimdOp::F32x4Ceil:
    case wasm::SimdOp::F32x4Floor:
    case wasm::SimdOp::F32x4Trunc:
    case wasm::SimdOp::F32x4Nearest:
    case wasm::SimdOp::F64x2Ceil:
    case wasm::SimdOp::F64x2Floor:
    case wasm::SimdOp::F64x2Trunc:
    case wasm::SimdOp::F64x2Nearest:
    case wasm::SimdOp::F32x4DemoteF64x2Zero:
    case wasm::SimdOp::F64x2PromoteLowF32x4:
    case wasm::SimdOp::F64x2ConvertLowI32x4S:
    case wasm::SimdOp::F64x2ConvertLowI32x4U:
    case wasm::SimdOp::I16x8ExtaddPairwiseI8x16S:
    case wasm::SimdOp::I16x8ExtaddPairwiseI8x16U:
    case wasm::SimdOp::I32x4ExtaddPairwiseI16x8S:
    case wasm::SimdOp::I32x4ExtaddPairwiseI16x8U:
    case wasm::SimdOp::I8x16Popcnt:
    case wasm::SimdOp::I32x4RelaxedTruncF32x4S:
    case wasm::SimdOp::I32x4RelaxedTruncF32x4U:
    case wasm::SimdOp::I32x4RelaxedTruncF64x2SZero:
    case wasm::SimdOp::I32x4RelaxedTruncF64x2UZero:
      break;
    case wasm::SimdOp::I32x4TruncSatF64x2SZero:
    case wasm::SimdOp::I32x4TruncSatF64x2UZero:
      tempReg = tempSimd128();
      break;
    default:
      MOZ_CRASH("Unary SimdOp not implemented");
  }

  LUse input = useRegisterAtStart(ins->input());
  LWasmUnarySimd128* lir = new (alloc()) LWasmUnarySimd128(input, tempReg);
  define(lir, ins);
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmReduceSimd128(MWasmReduceSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  if (canEmitWasmReduceSimd128AtUses(ins)) {
    emitAtUses(ins);
    return;
  }

  // Reductions (any_true, all_true, bitmask, extract_lane) uniformly prefer
  // useRegisterAtStart:
  //
  // - In most cases, the input type differs from the output type, so there's no
  //   conflict and it doesn't really matter.
  //
  // - For extract_lane(0) on F32x4 and F64x2, input == output results in zero
  //   code being generated.
  //
  // - For extract_lane(k > 0) on F32x4 and F64x2, allowing the input register
  //   to be targeted lowers register pressure if it's the last use of the
  //   input.

  if (ins->type() == MIRType::Int64) {
    auto* lir = new (alloc())
        LWasmReduceSimd128ToInt64(useRegisterAtStart(ins->input()));
    defineInt64(lir, ins);
  } else {
    LDefinition tempReg = LDefinition::BogusTemp();
    switch (ins->simdOp()) {
      case wasm::SimdOp::I8x16Bitmask:
      case wasm::SimdOp::I16x8Bitmask:
      case wasm::SimdOp::I32x4Bitmask:
      case wasm::SimdOp::I64x2Bitmask:
        tempReg = tempSimd128();
        break;
      default:
        break;
    }

    // Ideally we would reuse the input register for floating extract_lane if
    // the lane is zero, but constraints in the register allocator require the
    // input and output register types to be the same.
    auto* lir = new (alloc())
        LWasmReduceSimd128(useRegisterAtStart(ins->input()), tempReg);
    define(lir, ins);
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmLoadLaneSimd128(MWasmLoadLaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  // On 64-bit systems, the base pointer can be 32 bits or 64 bits.  Either way,
  // it fits in a GPR so we can ignore the Register/Register64 distinction here.

  // Optimal allocation here reuses the value input for the output register
  // because codegen otherwise has to copy the input to the output; this is
  // because load-lane is implemented as load + replace-lane.  Bug 1706106 may
  // change all of that, so leave it alone for now.
  LUse base = useRegisterAtStart(ins->base());
  LUse inputUse = useRegisterAtStart(ins->value());
  LAllocation memoryBase =
      ins->hasMemoryBase() ? LAllocation(useRegisterAtStart(ins->memoryBase()))
                           : LGeneralReg(HeapReg);
  LWasmLoadLaneSimd128* lir =
      new (alloc()) LWasmLoadLaneSimd128(base, inputUse, temp(), memoryBase);
  define(lir, ins);
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmStoreLaneSimd128(MWasmStoreLaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  // See comment above about the base pointer.

  LUse base = useRegisterAtStart(ins->base());
  LUse input = useRegisterAtStart(ins->value());
  LAllocation memoryBase =
      ins->hasMemoryBase() ? LAllocation(useRegisterAtStart(ins->memoryBase()))
                           : LGeneralReg(HeapReg);
  LWasmStoreLaneSimd128* lir =
      new (alloc()) LWasmStoreLaneSimd128(base, input, temp(), memoryBase);
  add(lir, ins);
#else
  MOZ_CRASH("No SIMD");
#endif
}
