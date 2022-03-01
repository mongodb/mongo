/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/Lowering-x86-shared.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/Lowering.h"
#include "jit/MIR.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Abs;
using mozilla::FloorLog2;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

LTableSwitch* LIRGeneratorX86Shared::newLTableSwitch(
    const LAllocation& in, const LDefinition& inputCopy,
    MTableSwitch* tableswitch) {
  return new (alloc()) LTableSwitch(in, inputCopy, temp(), tableswitch);
}

LTableSwitchV* LIRGeneratorX86Shared::newLTableSwitchV(
    MTableSwitch* tableswitch) {
  return new (alloc()) LTableSwitchV(useBox(tableswitch->getOperand(0)), temp(),
                                     tempDouble(), temp(), tableswitch);
}

void LIRGenerator::visitPowHalf(MPowHalf* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Double);
  LPowHalfD* lir = new (alloc()) LPowHalfD(useRegisterAtStart(input));
  define(lir, ins);
}

void LIRGeneratorX86Shared::lowerForShift(LInstructionHelper<1, 2, 0>* ins,
                                          MDefinition* mir, MDefinition* lhs,
                                          MDefinition* rhs) {
  ins->setOperand(0, useRegisterAtStart(lhs));

  // Shift operand should be constant or, unless BMI2 is available, in register
  // ecx. x86 can't shift a non-ecx register.
  if (rhs->isConstant()) {
    ins->setOperand(1, useOrConstantAtStart(rhs));
  } else if (Assembler::HasBMI2() && !mir->isRotate()) {
    ins->setOperand(1, willHaveDifferentLIRNodes(lhs, rhs)
                           ? useRegister(rhs)
                           : useRegisterAtStart(rhs));
  } else {
    ins->setOperand(1, willHaveDifferentLIRNodes(lhs, rhs)
                           ? useFixed(rhs, ecx)
                           : useFixedAtStart(rhs, ecx));
  }

  defineReuseInput(ins, mir, 0);
}

template <size_t Temps>
void LIRGeneratorX86Shared::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs) {
  ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
#if defined(JS_NUNBOX32)
  if (mir->isRotate()) {
    ins->setTemp(0, temp());
  }
#endif

  static_assert(LShiftI64::Rhs == INT64_PIECES,
                "Assume Rhs is located at INT64_PIECES.");
  static_assert(LRotateI64::Count == INT64_PIECES,
                "Assume Count is located at INT64_PIECES.");

  // Shift operand should be constant or, unless BMI2 is available, in register
  // ecx. x86 can't shift a non-ecx register.
  if (rhs->isConstant()) {
    ins->setOperand(INT64_PIECES, useOrConstantAtStart(rhs));
#ifdef JS_CODEGEN_X64
  } else if (Assembler::HasBMI2() && !mir->isRotate()) {
    ins->setOperand(INT64_PIECES, useRegister(rhs));
#endif
  } else {
    // The operands are int64, but we only care about the lower 32 bits of
    // the RHS. On 32-bit, the code below will load that part in ecx and
    // will discard the upper half.
    ensureDefined(rhs);
    LUse use(ecx);
    use.setVirtualRegister(rhs->virtualRegister());
    ins->setOperand(INT64_PIECES, use);
  }

  defineInt64ReuseInput(ins, mir, 0);
}

template void LIRGeneratorX86Shared::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, 0>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorX86Shared::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, 1>* ins,
    MDefinition* mir, MDefinition* lhs, MDefinition* rhs);

void LIRGeneratorX86Shared::lowerForCompareI64AndBranch(
    MTest* mir, MCompare* comp, JSOp op, MDefinition* left, MDefinition* right,
    MBasicBlock* ifTrue, MBasicBlock* ifFalse) {
  auto* lir = new (alloc())
      LCompareI64AndBranch(comp, op, useInt64Register(left),
                           useInt64OrConstant(right), ifTrue, ifFalse);
  add(lir, mir);
}

void LIRGeneratorX86Shared::lowerForALU(LInstructionHelper<1, 1, 0>* ins,
                                        MDefinition* mir, MDefinition* input) {
  ins->setOperand(0, useRegisterAtStart(input));
  defineReuseInput(ins, mir, 0);
}

void LIRGeneratorX86Shared::lowerForALU(LInstructionHelper<1, 2, 0>* ins,
                                        MDefinition* mir, MDefinition* lhs,
                                        MDefinition* rhs) {
  ins->setOperand(0, useRegisterAtStart(lhs));
  ins->setOperand(1, willHaveDifferentLIRNodes(lhs, rhs)
                         ? useOrConstant(rhs)
                         : useOrConstantAtStart(rhs));
  defineReuseInput(ins, mir, 0);
}

template <size_t Temps>
void LIRGeneratorX86Shared::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins,
                                        MDefinition* mir, MDefinition* lhs,
                                        MDefinition* rhs) {
  // Without AVX, we'll need to use the x86 encodings where one of the
  // inputs must be the same location as the output.
  if (!Assembler::HasAVX()) {
    ins->setOperand(0, useRegisterAtStart(lhs));
    ins->setOperand(
        1, willHaveDifferentLIRNodes(lhs, rhs) ? use(rhs) : useAtStart(rhs));
    defineReuseInput(ins, mir, 0);
  } else {
    ins->setOperand(0, useRegisterAtStart(lhs));
    ins->setOperand(1, useAtStart(rhs));
    define(ins, mir);
  }
}

template void LIRGeneratorX86Shared::lowerForFPU(
    LInstructionHelper<1, 2, 0>* ins, MDefinition* mir, MDefinition* lhs,
    MDefinition* rhs);
template void LIRGeneratorX86Shared::lowerForFPU(
    LInstructionHelper<1, 2, 1>* ins, MDefinition* mir, MDefinition* lhs,
    MDefinition* rhs);

void LIRGeneratorX86Shared::lowerForBitAndAndBranch(LBitAndAndBranch* baab,
                                                    MInstruction* mir,
                                                    MDefinition* lhs,
                                                    MDefinition* rhs) {
  baab->setOperand(0, useRegisterAtStart(lhs));
  baab->setOperand(1, useRegisterOrConstantAtStart(rhs));
  add(baab, mir);
}

void LIRGeneratorX86Shared::lowerNegI(MInstruction* ins, MDefinition* input) {
  defineReuseInput(new (alloc()) LNegI(useRegisterAtStart(input)), ins, 0);
}

void LIRGeneratorX86Shared::lowerNegI64(MInstruction* ins, MDefinition* input) {
  defineInt64ReuseInput(new (alloc()) LNegI64(useInt64RegisterAtStart(input)),
                        ins, 0);
}

void LIRGenerator::visitAbs(MAbs* ins) {
  defineReuseInput(allocateAbs(ins, useRegisterAtStart(ins->input())), ins, 0);
}

void LIRGeneratorX86Shared::lowerMulI(MMul* mul, MDefinition* lhs,
                                      MDefinition* rhs) {
  // Note: If we need a negative zero check, lhs is used twice.
  LAllocation lhsCopy = mul->canBeNegativeZero() ? use(lhs) : LAllocation();
  LMulI* lir = new (alloc())
      LMulI(useRegisterAtStart(lhs),
            willHaveDifferentLIRNodes(lhs, rhs) ? useOrConstant(rhs)
                                                : useOrConstantAtStart(rhs),
            lhsCopy);
  if (mul->fallible()) {
    assignSnapshot(lir, mul->bailoutKind());
  }
  defineReuseInput(lir, mul, 0);
}

void LIRGeneratorX86Shared::lowerDivI(MDiv* div) {
  if (div->isUnsigned()) {
    lowerUDiv(div);
    return;
  }

  // Division instructions are slow. Division by constant denominators can be
  // rewritten to use other instructions.
  if (div->rhs()->isConstant()) {
    int32_t rhs = div->rhs()->toConstant()->toInt32();

    // Division by powers of two can be done by shifting, and division by
    // other numbers can be done by a reciprocal multiplication technique.
    int32_t shift = FloorLog2(Abs(rhs));
    if (rhs != 0 && uint32_t(1) << shift == Abs(rhs)) {
      LAllocation lhs = useRegisterAtStart(div->lhs());
      LDivPowTwoI* lir;
      // When truncated with maybe a non-zero remainder, we have to round the
      // result toward 0. This requires an extra register to round up/down
      // whether the left-hand-side is signed.
      bool needRoundNeg = div->canBeNegativeDividend() && div->isTruncated();
      if (!needRoundNeg) {
        // Numerator is unsigned, so does not need adjusting.
        lir = new (alloc()) LDivPowTwoI(lhs, lhs, shift, rhs < 0);
      } else {
        // Numerator might be signed, and needs adjusting, and an extra lhs copy
        // is needed to round the result of the integer division towards zero.
        lir = new (alloc())
            LDivPowTwoI(lhs, useRegister(div->lhs()), shift, rhs < 0);
      }
      if (div->fallible()) {
        assignSnapshot(lir, div->bailoutKind());
      }
      defineReuseInput(lir, div, 0);
      return;
    }
    if (rhs != 0) {
      LDivOrModConstantI* lir;
      lir = new (alloc())
          LDivOrModConstantI(useRegister(div->lhs()), rhs, tempFixed(eax));
      if (div->fallible()) {
        assignSnapshot(lir, div->bailoutKind());
      }
      defineFixed(lir, div, LAllocation(AnyRegister(edx)));
      return;
    }
  }

  LDivI* lir = new (alloc())
      LDivI(useRegister(div->lhs()), useRegister(div->rhs()), tempFixed(edx));
  if (div->fallible()) {
    assignSnapshot(lir, div->bailoutKind());
  }
  defineFixed(lir, div, LAllocation(AnyRegister(eax)));
}

void LIRGeneratorX86Shared::lowerModI(MMod* mod) {
  if (mod->isUnsigned()) {
    lowerUMod(mod);
    return;
  }

  if (mod->rhs()->isConstant()) {
    int32_t rhs = mod->rhs()->toConstant()->toInt32();
    int32_t shift = FloorLog2(Abs(rhs));
    if (rhs != 0 && uint32_t(1) << shift == Abs(rhs)) {
      LModPowTwoI* lir =
          new (alloc()) LModPowTwoI(useRegisterAtStart(mod->lhs()), shift);
      if (mod->fallible()) {
        assignSnapshot(lir, mod->bailoutKind());
      }
      defineReuseInput(lir, mod, 0);
      return;
    }
    if (rhs != 0) {
      LDivOrModConstantI* lir;
      lir = new (alloc())
          LDivOrModConstantI(useRegister(mod->lhs()), rhs, tempFixed(edx));
      if (mod->fallible()) {
        assignSnapshot(lir, mod->bailoutKind());
      }
      defineFixed(lir, mod, LAllocation(AnyRegister(eax)));
      return;
    }
  }

  LModI* lir = new (alloc())
      LModI(useRegister(mod->lhs()), useRegister(mod->rhs()), tempFixed(eax));
  if (mod->fallible()) {
    assignSnapshot(lir, mod->bailoutKind());
  }
  defineFixed(lir, mod, LAllocation(AnyRegister(edx)));
}

void LIRGenerator::visitWasmNeg(MWasmNeg* ins) {
  switch (ins->type()) {
    case MIRType::Int32:
      defineReuseInput(new (alloc()) LNegI(useRegisterAtStart(ins->input())),
                       ins, 0);
      break;
    case MIRType::Float32:
      defineReuseInput(new (alloc()) LNegF(useRegisterAtStart(ins->input())),
                       ins, 0);
      break;
    case MIRType::Double:
      defineReuseInput(new (alloc()) LNegD(useRegisterAtStart(ins->input())),
                       ins, 0);
      break;
    default:
      MOZ_CRASH();
  }
}

void LIRGeneratorX86Shared::lowerWasmSelectI(MWasmSelect* select) {
  auto* lir = new (alloc())
      LWasmSelect(useRegisterAtStart(select->trueExpr()),
                  useAny(select->falseExpr()), useRegister(select->condExpr()));
  defineReuseInput(lir, select, LWasmSelect::TrueExprIndex);
}

void LIRGeneratorX86Shared::lowerWasmSelectI64(MWasmSelect* select) {
  auto* lir = new (alloc()) LWasmSelectI64(
      useInt64RegisterAtStart(select->trueExpr()),
      useInt64(select->falseExpr()), useRegister(select->condExpr()));
  defineInt64ReuseInput(lir, select, LWasmSelectI64::TrueExprIndex);
}

void LIRGenerator::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
  MOZ_ASSERT_IF(ins->needsBoundsCheck(),
                boundsCheckLimit->type() == MIRType::Int32);

  // For simplicity, require a register if we're going to emit a bounds-check
  // branch, so that we don't have special cases for constants. This should
  // only happen in rare constant-folding cases since asm.js sets the minimum
  // heap size based when accessed via constant.
  LAllocation baseAlloc = ins->needsBoundsCheck()
                              ? useRegisterAtStart(base)
                              : useRegisterOrZeroAtStart(base);

  LAllocation limitAlloc = ins->needsBoundsCheck()
                               ? useRegisterAtStart(boundsCheckLimit)
                               : LAllocation();
  LAllocation memoryBaseAlloc = ins->hasMemoryBase()
                                    ? useRegisterAtStart(ins->memoryBase())
                                    : LAllocation();

  auto* lir =
      new (alloc()) LAsmJSLoadHeap(baseAlloc, limitAlloc, memoryBaseAlloc);
  define(lir, ins);
}

void LIRGenerator::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins) {
  MDefinition* base = ins->base();
  MOZ_ASSERT(base->type() == MIRType::Int32);

  MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
  MOZ_ASSERT_IF(ins->needsBoundsCheck(),
                boundsCheckLimit->type() == MIRType::Int32);

  // For simplicity, require a register if we're going to emit a bounds-check
  // branch, so that we don't have special cases for constants. This should
  // only happen in rare constant-folding cases since asm.js sets the minimum
  // heap size based when accessed via constant.
  LAllocation baseAlloc = ins->needsBoundsCheck()
                              ? useRegisterAtStart(base)
                              : useRegisterOrZeroAtStart(base);

  LAllocation limitAlloc = ins->needsBoundsCheck()
                               ? useRegisterAtStart(boundsCheckLimit)
                               : LAllocation();
  LAllocation memoryBaseAlloc = ins->hasMemoryBase()
                                    ? useRegisterAtStart(ins->memoryBase())
                                    : LAllocation();

  LAsmJSStoreHeap* lir = nullptr;
  switch (ins->access().type()) {
    case Scalar::Int8:
    case Scalar::Uint8:
#ifdef JS_CODEGEN_X86
      // See comment for LIRGeneratorX86::useByteOpRegister.
      lir = new (alloc()) LAsmJSStoreHeap(
          baseAlloc, useFixed(ins->value(), eax), limitAlloc, memoryBaseAlloc);
      break;
#endif
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
    case Scalar::Float64:
      // For now, don't allow constant values. The immediate operand affects
      // instruction layout which affects patching.
      lir = new (alloc())
          LAsmJSStoreHeap(baseAlloc, useRegisterAtStart(ins->value()),
                          limitAlloc, memoryBaseAlloc);
      break;
    case Scalar::Int64:
    case Scalar::Simd128:
      MOZ_CRASH("NYI");
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected array type");
  }
  add(lir, ins);
}

void LIRGeneratorX86Shared::lowerUDiv(MDiv* div) {
  if (div->rhs()->isConstant()) {
    uint32_t rhs = div->rhs()->toConstant()->toInt32();
    int32_t shift = FloorLog2(rhs);

    LAllocation lhs = useRegisterAtStart(div->lhs());
    if (rhs != 0 && uint32_t(1) << shift == rhs) {
      LDivPowTwoI* lir = new (alloc()) LDivPowTwoI(lhs, lhs, shift, false);
      if (div->fallible()) {
        assignSnapshot(lir, div->bailoutKind());
      }
      defineReuseInput(lir, div, 0);
    } else {
      LUDivOrModConstant* lir = new (alloc())
          LUDivOrModConstant(useRegister(div->lhs()), rhs, tempFixed(eax));
      if (div->fallible()) {
        assignSnapshot(lir, div->bailoutKind());
      }
      defineFixed(lir, div, LAllocation(AnyRegister(edx)));
    }
    return;
  }

  LUDivOrMod* lir = new (alloc()) LUDivOrMod(
      useRegister(div->lhs()), useRegister(div->rhs()), tempFixed(edx));
  if (div->fallible()) {
    assignSnapshot(lir, div->bailoutKind());
  }
  defineFixed(lir, div, LAllocation(AnyRegister(eax)));
}

void LIRGeneratorX86Shared::lowerUMod(MMod* mod) {
  if (mod->rhs()->isConstant()) {
    uint32_t rhs = mod->rhs()->toConstant()->toInt32();
    int32_t shift = FloorLog2(rhs);

    if (rhs != 0 && uint32_t(1) << shift == rhs) {
      LModPowTwoI* lir =
          new (alloc()) LModPowTwoI(useRegisterAtStart(mod->lhs()), shift);
      if (mod->fallible()) {
        assignSnapshot(lir, mod->bailoutKind());
      }
      defineReuseInput(lir, mod, 0);
    } else {
      LUDivOrModConstant* lir = new (alloc())
          LUDivOrModConstant(useRegister(mod->lhs()), rhs, tempFixed(edx));
      if (mod->fallible()) {
        assignSnapshot(lir, mod->bailoutKind());
      }
      defineFixed(lir, mod, LAllocation(AnyRegister(eax)));
    }
    return;
  }

  LUDivOrMod* lir = new (alloc()) LUDivOrMod(
      useRegister(mod->lhs()), useRegister(mod->rhs()), tempFixed(eax));
  if (mod->fallible()) {
    assignSnapshot(lir, mod->bailoutKind());
  }
  defineFixed(lir, mod, LAllocation(AnyRegister(edx)));
}

void LIRGeneratorX86Shared::lowerUrshD(MUrsh* mir) {
  MDefinition* lhs = mir->lhs();
  MDefinition* rhs = mir->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Int32);
  MOZ_ASSERT(rhs->type() == MIRType::Int32);
  MOZ_ASSERT(mir->type() == MIRType::Double);

#ifdef JS_CODEGEN_X64
  static_assert(ecx == rcx);
#endif

  // Without BMI2, x86 can only shift by ecx.
  LUse lhsUse = useRegisterAtStart(lhs);
  LAllocation rhsAlloc;
  if (rhs->isConstant()) {
    rhsAlloc = useOrConstant(rhs);
  } else if (Assembler::HasBMI2()) {
    rhsAlloc = useRegister(rhs);
  } else {
    rhsAlloc = useFixed(rhs, ecx);
  }

  LUrshD* lir = new (alloc()) LUrshD(lhsUse, rhsAlloc, tempCopy(lhs, 0));
  define(lir, mir);
}

void LIRGeneratorX86Shared::lowerPowOfTwoI(MPow* mir) {
  int32_t base = mir->input()->toConstant()->toInt32();
  MDefinition* power = mir->power();

  // Shift operand should be in register ecx, unless BMI2 is available.
  // x86 can't shift a non-ecx register.
  LAllocation powerAlloc =
      Assembler::HasBMI2() ? useRegister(power) : useFixed(power, ecx);
  auto* lir = new (alloc()) LPowOfTwoI(base, powerAlloc);
  assignSnapshot(lir, mir->bailoutKind());
  define(lir, mir);
}

void LIRGeneratorX86Shared::lowerBigIntLsh(MBigIntLsh* ins) {
  // Shift operand should be in register ecx, unless BMI2 is available.
  // x86 can't shift a non-ecx register.
  LDefinition shiftAlloc = Assembler::HasBMI2() ? temp() : tempFixed(ecx);
  auto* lir =
      new (alloc()) LBigIntLsh(useRegister(ins->lhs()), useRegister(ins->rhs()),
                               temp(), shiftAlloc, temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorX86Shared::lowerBigIntRsh(MBigIntRsh* ins) {
  // Shift operand should be in register ecx, unless BMI2 is available.
  // x86 can't shift a non-ecx register.
  LDefinition shiftAlloc = Assembler::HasBMI2() ? temp() : tempFixed(ecx);
  auto* lir =
      new (alloc()) LBigIntRsh(useRegister(ins->lhs()), useRegister(ins->rhs()),
                               temp(), shiftAlloc, temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGeneratorX86Shared::lowerWasmBuiltinTruncateToInt32(
    MWasmBuiltinTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

  LDefinition maybeTemp =
      Assembler::HasSSE3() ? LDefinition::BogusTemp() : tempDouble();
  if (opd->type() == MIRType::Double) {
    define(new (alloc()) LWasmBuiltinTruncateDToInt32(
               useRegister(opd), useFixed(ins->tls(), WasmTlsReg), maybeTemp),
           ins);
    return;
  }

  define(new (alloc()) LWasmBuiltinTruncateFToInt32(
             useRegister(opd), useFixed(ins->tls(), WasmTlsReg), maybeTemp),
         ins);
}

void LIRGeneratorX86Shared::lowerTruncateDToInt32(MTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Double);

  LDefinition maybeTemp =
      Assembler::HasSSE3() ? LDefinition::BogusTemp() : tempDouble();
  define(new (alloc()) LTruncateDToInt32(useRegister(opd), maybeTemp), ins);
}

void LIRGeneratorX86Shared::lowerTruncateFToInt32(MTruncateToInt32* ins) {
  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Float32);

  LDefinition maybeTemp =
      Assembler::HasSSE3() ? LDefinition::BogusTemp() : tempFloat32();
  define(new (alloc()) LTruncateFToInt32(useRegister(opd), maybeTemp), ins);
}

void LIRGeneratorX86Shared::lowerCompareExchangeTypedArrayElement(
    MCompareExchangeTypedArrayElement* ins, bool useI386ByteRegisters) {
  MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
  MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());

  // If the target is a floating register then we need a temp at the
  // lower level; that temp must be eax.
  //
  // Otherwise the target (if used) is an integer register, which
  // must be eax.  If the target is not used the machine code will
  // still clobber eax, so just pretend it's used.
  //
  // oldval must be in a register.
  //
  // newval must be in a register.  If the source is a byte array
  // then newval must be a register that has a byte size: on x86
  // this must be ebx, ecx, or edx (eax is taken for the output).
  //
  // Bug #1077036 describes some further optimization opportunities.

  bool fixedOutput = false;
  LDefinition tempDef = LDefinition::BogusTemp();
  LAllocation newval;
  if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type())) {
    tempDef = tempFixed(eax);
    newval = useRegister(ins->newval());
  } else {
    fixedOutput = true;
    if (useI386ByteRegisters && ins->isByteArray()) {
      newval = useFixed(ins->newval(), ebx);
    } else {
      newval = useRegister(ins->newval());
    }
  }

  const LAllocation oldval = useRegister(ins->oldval());

  LCompareExchangeTypedArrayElement* lir =
      new (alloc()) LCompareExchangeTypedArrayElement(elements, index, oldval,
                                                      newval, tempDef);

  if (fixedOutput) {
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
  } else {
    define(lir, ins);
  }
}

void LIRGeneratorX86Shared::lowerAtomicExchangeTypedArrayElement(
    MAtomicExchangeTypedArrayElement* ins, bool useI386ByteRegisters) {
  MOZ_ASSERT(ins->arrayType() <= Scalar::Uint32);

  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());
  const LAllocation value = useRegister(ins->value());

  // The underlying instruction is XCHG, which can operate on any
  // register.
  //
  // If the target is a floating register (for Uint32) then we need
  // a temp into which to exchange.
  //
  // If the source is a byte array then we need a register that has
  // a byte size; in this case -- on x86 only -- pin the output to
  // an appropriate register and use that as a temp in the back-end.

  LDefinition tempDef = LDefinition::BogusTemp();
  if (ins->arrayType() == Scalar::Uint32) {
    MOZ_ASSERT(ins->type() == MIRType::Double);
    tempDef = temp();
  }

  LAtomicExchangeTypedArrayElement* lir = new (alloc())
      LAtomicExchangeTypedArrayElement(elements, index, value, tempDef);

  if (useI386ByteRegisters && ins->isByteArray()) {
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
  } else {
    define(lir, ins);
  }
}

void LIRGeneratorX86Shared::lowerAtomicTypedArrayElementBinop(
    MAtomicTypedArrayElementBinop* ins, bool useI386ByteRegisters) {
  MOZ_ASSERT(ins->arrayType() != Scalar::Uint8Clamped);
  MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
  MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->arrayType());

  // Case 1: the result of the operation is not used.
  //
  // We'll emit a single instruction: LOCK ADD, LOCK SUB, LOCK AND,
  // LOCK OR, or LOCK XOR.  We can do this even for the Uint32 case.

  if (ins->isForEffect()) {
    LAllocation value;
    if (useI386ByteRegisters && ins->isByteArray() &&
        !ins->value()->isConstant()) {
      value = useFixed(ins->value(), ebx);
    } else {
      value = useRegisterOrConstant(ins->value());
    }

    LAtomicTypedArrayElementBinopForEffect* lir = new (alloc())
        LAtomicTypedArrayElementBinopForEffect(elements, index, value);

    add(lir, ins);
    return;
  }

  // Case 2: the result of the operation is used.
  //
  // For ADD and SUB we'll use XADD:
  //
  //    movl       src, output
  //    lock xaddl output, mem
  //
  // For the 8-bit variants XADD needs a byte register for the output.
  //
  // For AND/OR/XOR we need to use a CMPXCHG loop:
  //
  //    movl          *mem, eax
  // L: mov           eax, temp
  //    andl          src, temp
  //    lock cmpxchg  temp, mem  ; reads eax also
  //    jnz           L
  //    ; result in eax
  //
  // Note the placement of L, cmpxchg will update eax with *mem if
  // *mem does not have the expected value, so reloading it at the
  // top of the loop would be redundant.
  //
  // If the array is not a uint32 array then:
  //  - eax should be the output (one result of the cmpxchg)
  //  - there is a temp, which must have a byte register if
  //    the array has 1-byte elements elements
  //
  // If the array is a uint32 array then:
  //  - eax is the first temp
  //  - we also need a second temp
  //
  // There are optimization opportunities:
  //  - better register allocation in the x86 8-bit case, Bug #1077036.

  bool bitOp = !(ins->operation() == AtomicFetchAddOp ||
                 ins->operation() == AtomicFetchSubOp);
  bool fixedOutput = true;
  bool reuseInput = false;
  LDefinition tempDef1 = LDefinition::BogusTemp();
  LDefinition tempDef2 = LDefinition::BogusTemp();
  LAllocation value;

  if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type())) {
    value = useRegisterOrConstant(ins->value());
    fixedOutput = false;
    if (bitOp) {
      tempDef1 = tempFixed(eax);
      tempDef2 = temp();
    } else {
      tempDef1 = temp();
    }
  } else if (useI386ByteRegisters && ins->isByteArray()) {
    if (ins->value()->isConstant()) {
      value = useRegisterOrConstant(ins->value());
    } else {
      value = useFixed(ins->value(), ebx);
    }
    if (bitOp) {
      tempDef1 = tempFixed(ecx);
    }
  } else if (bitOp) {
    value = useRegisterOrConstant(ins->value());
    tempDef1 = temp();
  } else if (ins->value()->isConstant()) {
    fixedOutput = false;
    value = useRegisterOrConstant(ins->value());
  } else {
    fixedOutput = false;
    reuseInput = true;
    value = useRegisterAtStart(ins->value());
  }

  LAtomicTypedArrayElementBinop* lir = new (alloc())
      LAtomicTypedArrayElementBinop(elements, index, value, tempDef1, tempDef2);

  if (fixedOutput) {
    defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
  } else if (reuseInput) {
    defineReuseInput(lir, ins, LAtomicTypedArrayElementBinop::valueOp);
  } else {
    define(lir, ins);
  }
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

  // As lowerForFPU, but we want rhs to be in a FP register too.
  lir->setOperand(0, useRegisterAtStart(lhs));
  if (!Assembler::HasAVX()) {
    lir->setOperand(1, willHaveDifferentLIRNodes(lhs, rhs)
                           ? useRegister(rhs)
                           : useRegisterAtStart(rhs));
    defineReuseInput(lir, ins, 0);
  } else {
    lir->setOperand(1, useRegisterAtStart(rhs));
    define(lir, ins);
  }
}

// These lowerings are really x86-shared but some Masm APIs are not yet
// available on x86.

// Ternary and binary operators require the dest register to be the same as
// their first input register, leading to a pattern of useRegisterAtStart +
// defineReuseInput.

void LIRGenerator::visitWasmBitselectSimd128(MWasmBitselectSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MOZ_ASSERT(ins->lhs()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->rhs()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->control()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  // Enforcing lhs == output avoids one setup move.  We would like to also
  // enforce merging the control with the temp (with usRegisterAtStart(control)
  // and tempCopy()), but the register allocator ignores those constraints
  // at present.

  auto* lir = new (alloc()) LWasmBitselectSimd128(
      useRegisterAtStart(ins->lhs()), useRegister(ins->rhs()),
      useRegister(ins->control()), tempSimd128());
  defineReuseInput(lir, ins, LWasmBitselectSimd128::LhsDest);
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

  // Note MWasmBinarySimd128::foldsTo has already specialized operations that
  // have a constant operand, so this takes care of more general cases of
  // reordering, see ReorderCommutative.
  if (ins->isCommutative()) {
    ReorderCommutative(&lhs, &rhs, ins);
  }

  // Swap operands and change operation if necessary, these are all x86/x64
  // dependent transformations.  Except where noted, this is about avoiding
  // unnecessary moves and fixups in the code generator macros.
  bool swap = false;
  switch (op) {
    case wasm::SimdOp::V128AndNot: {
      // Code generation requires the operands to be reversed.
      swap = true;
      break;
    }
    case wasm::SimdOp::I8x16LtS: {
      swap = true;
      op = wasm::SimdOp::I8x16GtS;
      break;
    }
    case wasm::SimdOp::I8x16GeS: {
      swap = true;
      op = wasm::SimdOp::I8x16LeS;
      break;
    }
    case wasm::SimdOp::I16x8LtS: {
      swap = true;
      op = wasm::SimdOp::I16x8GtS;
      break;
    }
    case wasm::SimdOp::I16x8GeS: {
      swap = true;
      op = wasm::SimdOp::I16x8LeS;
      break;
    }
    case wasm::SimdOp::I32x4LtS: {
      swap = true;
      op = wasm::SimdOp::I32x4GtS;
      break;
    }
    case wasm::SimdOp::I32x4GeS: {
      swap = true;
      op = wasm::SimdOp::I32x4LeS;
      break;
    }
    case wasm::SimdOp::F32x4Gt: {
      swap = true;
      op = wasm::SimdOp::F32x4Lt;
      break;
    }
    case wasm::SimdOp::F32x4Ge: {
      swap = true;
      op = wasm::SimdOp::F32x4Le;
      break;
    }
    case wasm::SimdOp::F64x2Gt: {
      swap = true;
      op = wasm::SimdOp::F64x2Lt;
      break;
    }
    case wasm::SimdOp::F64x2Ge: {
      swap = true;
      op = wasm::SimdOp::F64x2Le;
      break;
    }
    case wasm::SimdOp::F32x4PMin:
    case wasm::SimdOp::F32x4PMax:
    case wasm::SimdOp::F64x2PMin:
    case wasm::SimdOp::F64x2PMax: {
      // Code generation requires the operations to be reversed (the rhs is the
      // output register).
      swap = true;
      break;
    }
    default:
      break;
  }
  if (swap) {
    MDefinition* tmp = lhs;
    lhs = rhs;
    rhs = tmp;
  }

  // Allocate temp registers
  LDefinition tempReg0 = LDefinition::BogusTemp();
  LDefinition tempReg1 = LDefinition::BogusTemp();
  switch (op) {
    case wasm::SimdOp::I64x2Mul:
      tempReg0 = tempSimd128();
      break;
    case wasm::SimdOp::F32x4Min:
    case wasm::SimdOp::F32x4Max:
    case wasm::SimdOp::F64x2Min:
    case wasm::SimdOp::F64x2Max:
    case wasm::SimdOp::I64x2LtS:
    case wasm::SimdOp::I64x2GtS:
    case wasm::SimdOp::I64x2LeS:
    case wasm::SimdOp::I64x2GeS:
      tempReg0 = tempSimd128();
      tempReg1 = tempSimd128();
      break;
    default:
      break;
  }

  // For binary ops, the Masm API always is usually (rhs, lhsDest) and requires
  // AtStart+ReuseInput for the lhs.
  //
  // For a few ops, the API is actually (rhsDest, lhs) and the rules are the
  // same but the reversed.  We swapped operands above; they will be swapped
  // again in the code generator to emit the right code.

  LAllocation lhsDestAlloc = useRegisterAtStart(lhs);
  LAllocation rhsAlloc = willHaveDifferentLIRNodes(lhs, rhs)
                             ? useRegister(rhs)
                             : useRegisterAtStart(rhs);
  auto* lir = new (alloc())
      LWasmBinarySimd128(op, lhsDestAlloc, rhsAlloc, tempReg0, tempReg1);
  defineReuseInput(lir, ins, LWasmBinarySimd128::LhsDest);
#else
  MOZ_CRASH("No SIMD");
#endif
}

#ifdef ENABLE_WASM_SIMD
bool MWasmBitselectSimd128::specializeConstantMaskAsShuffle(
    int8_t shuffle[16]) {
  // Optimization when control vector is a mask with all 0 or all 1 per lane.
  // On x86, there is no bitselect, blend operations will be a win,
  // e.g. via PBLENDVB or PBLENDW.
  SimdConstant constant =
      static_cast<MWasmFloatConstant*>(control())->toSimd128();
  const SimdConstant::I8x16& bytes = constant.asInt8x16();
  for (int8_t i = 0; i < 16; i++) {
    if (bytes[i] == -1) {
      shuffle[i] = i + 16;
    } else if (bytes[i] == 0) {
      shuffle[i] = i;
    } else {
      return false;
    }
  }
  return true;
}
#endif

bool MWasmBinarySimd128::specializeForConstantRhs() {
  // The order follows MacroAssembler.h, generally
  switch (simdOp()) {
    // Operations implemented by a single native instruction where it is
    // plausible that the rhs (after commutation if available) could be a
    // constant.
    //
    // Swizzle is not here because it was handled earlier in the pipeline.
    //
    // Integer compares >= and < are not here because they are not supported in
    // the hardware.
    //
    // Floating compares are not here because our patching machinery can't
    // handle them yet.
    //
    // Floating-point min and max (including pmin and pmax) are not here because
    // they are not straightforward to implement.
    case wasm::SimdOp::I8x16Add:
    case wasm::SimdOp::I16x8Add:
    case wasm::SimdOp::I32x4Add:
    case wasm::SimdOp::I64x2Add:
    case wasm::SimdOp::I8x16Sub:
    case wasm::SimdOp::I16x8Sub:
    case wasm::SimdOp::I32x4Sub:
    case wasm::SimdOp::I64x2Sub:
    case wasm::SimdOp::I16x8Mul:
    case wasm::SimdOp::I32x4Mul:
    case wasm::SimdOp::I8x16AddSaturateS:
    case wasm::SimdOp::I8x16AddSaturateU:
    case wasm::SimdOp::I16x8AddSaturateS:
    case wasm::SimdOp::I16x8AddSaturateU:
    case wasm::SimdOp::I8x16SubSaturateS:
    case wasm::SimdOp::I8x16SubSaturateU:
    case wasm::SimdOp::I16x8SubSaturateS:
    case wasm::SimdOp::I16x8SubSaturateU:
    case wasm::SimdOp::I8x16MinS:
    case wasm::SimdOp::I8x16MinU:
    case wasm::SimdOp::I16x8MinS:
    case wasm::SimdOp::I16x8MinU:
    case wasm::SimdOp::I32x4MinS:
    case wasm::SimdOp::I32x4MinU:
    case wasm::SimdOp::I8x16MaxS:
    case wasm::SimdOp::I8x16MaxU:
    case wasm::SimdOp::I16x8MaxS:
    case wasm::SimdOp::I16x8MaxU:
    case wasm::SimdOp::I32x4MaxS:
    case wasm::SimdOp::I32x4MaxU:
    case wasm::SimdOp::V128And:
    case wasm::SimdOp::V128Or:
    case wasm::SimdOp::V128Xor:
    case wasm::SimdOp::I8x16Eq:
    case wasm::SimdOp::I8x16Ne:
    case wasm::SimdOp::I8x16GtS:
    case wasm::SimdOp::I8x16LeS:
    case wasm::SimdOp::I16x8Eq:
    case wasm::SimdOp::I16x8Ne:
    case wasm::SimdOp::I16x8GtS:
    case wasm::SimdOp::I16x8LeS:
    case wasm::SimdOp::I32x4Eq:
    case wasm::SimdOp::I32x4Ne:
    case wasm::SimdOp::I32x4GtS:
    case wasm::SimdOp::I32x4LeS:
    case wasm::SimdOp::F32x4Eq:
    case wasm::SimdOp::F32x4Ne:
    case wasm::SimdOp::F32x4Lt:
    case wasm::SimdOp::F32x4Le:
    case wasm::SimdOp::F64x2Eq:
    case wasm::SimdOp::F64x2Ne:
    case wasm::SimdOp::F64x2Lt:
    case wasm::SimdOp::F64x2Le:
    case wasm::SimdOp::I32x4DotSI16x8:
    case wasm::SimdOp::F32x4Add:
    case wasm::SimdOp::F64x2Add:
    case wasm::SimdOp::F32x4Sub:
    case wasm::SimdOp::F64x2Sub:
    case wasm::SimdOp::F32x4Div:
    case wasm::SimdOp::F64x2Div:
    case wasm::SimdOp::F32x4Mul:
    case wasm::SimdOp::F64x2Mul:
    case wasm::SimdOp::I8x16NarrowSI16x8:
    case wasm::SimdOp::I8x16NarrowUI16x8:
    case wasm::SimdOp::I16x8NarrowSI32x4:
    case wasm::SimdOp::I16x8NarrowUI32x4:
      return true;
    default:
      return false;
  }
}

void LIRGenerator::visitWasmBinarySimd128WithConstant(
    MWasmBinarySimd128WithConstant* ins) {
#ifdef ENABLE_WASM_SIMD
  MDefinition* lhs = ins->lhs();

  MOZ_ASSERT(lhs->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  // Always beneficial to reuse the lhs register here, see discussion in
  // visitWasmBinarySimd128() and also code in specializeForConstantRhs().

  LAllocation lhsDestAlloc = useRegisterAtStart(lhs);
  auto* lir =
      new (alloc()) LWasmBinarySimd128WithConstant(lhsDestAlloc, ins->rhs());
  defineReuseInput(lir, ins, LWasmBinarySimd128WithConstant::LhsDest);
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmShiftSimd128(MWasmShiftSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Simd128);
  MOZ_ASSERT(rhs->type() == MIRType::Int32);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  if (rhs->isConstant()) {
    int32_t shiftCountMask;
    switch (ins->simdOp()) {
      case wasm::SimdOp::I8x16Shl:
      case wasm::SimdOp::I8x16ShrU:
      case wasm::SimdOp::I8x16ShrS:
        shiftCountMask = 7;
        break;
      case wasm::SimdOp::I16x8Shl:
      case wasm::SimdOp::I16x8ShrU:
      case wasm::SimdOp::I16x8ShrS:
        shiftCountMask = 15;
        break;
      case wasm::SimdOp::I32x4Shl:
      case wasm::SimdOp::I32x4ShrU:
      case wasm::SimdOp::I32x4ShrS:
        shiftCountMask = 31;
        break;
      case wasm::SimdOp::I64x2Shl:
      case wasm::SimdOp::I64x2ShrU:
      case wasm::SimdOp::I64x2ShrS:
        shiftCountMask = 63;
        break;
      default:
        MOZ_CRASH("Unexpected shift operation");
    }

    int32_t shiftCount = rhs->toConstant()->toInt32() & shiftCountMask;
    if (shiftCount == shiftCountMask) {
      // Check if possible to apply sign replication optimization.
      // For some ops the input shall be reused.
      switch (ins->simdOp()) {
        case wasm::SimdOp::I8x16ShrS: {
          auto* lir =
              new (alloc()) LWasmSignReplicationSimd128(useRegister(lhs));
          define(lir, ins);
          return;
        }
        case wasm::SimdOp::I16x8ShrS:
        case wasm::SimdOp::I32x4ShrS:
        case wasm::SimdOp::I64x2ShrS: {
          auto* lir = new (alloc())
              LWasmSignReplicationSimd128(useRegisterAtStart(lhs));
          defineReuseInput(lir, ins, LWasmConstantShiftSimd128::Src);
          return;
        }
        default:
          break;
      }
    }

#  ifdef DEBUG
    js::wasm::ReportSimdAnalysis("shift -> constant shift");
#  endif
    // Almost always beneficial, and never detrimental, to reuse the input if
    // possible.
    auto* lir = new (alloc())
        LWasmConstantShiftSimd128(useRegisterAtStart(lhs), shiftCount);
    defineReuseInput(lir, ins, LWasmConstantShiftSimd128::Src);
    return;
  }

#  ifdef DEBUG
  js::wasm::ReportSimdAnalysis("shift -> variable shift");
#  endif

  LDefinition tempReg0 = LDefinition::BogusTemp();
  LDefinition tempReg1 = LDefinition::BogusTemp();
  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16Shl:
    case wasm::SimdOp::I8x16ShrS:
    case wasm::SimdOp::I8x16ShrU:
    case wasm::SimdOp::I64x2ShrS:
      tempReg0 = temp();
      tempReg1 = tempSimd128();
      break;
    default:
      tempReg0 = temp();
      break;
  }

  // Reusing the input if possible is never detrimental.
  LAllocation lhsDestAlloc = useRegisterAtStart(lhs);
  LAllocation rhsAlloc = useRegisterAtStart(rhs);
  auto* lir = new (alloc())
      LWasmVariableShiftSimd128(lhsDestAlloc, rhsAlloc, tempReg0, tempReg1);
  defineReuseInput(lir, ins, LWasmVariableShiftSimd128::LhsDest);
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmShuffleSimd128(MWasmShuffleSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MOZ_ASSERT(ins->lhs()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->rhs()->type() == MIRType::Simd128);
  MOZ_ASSERT(ins->type() == MIRType::Simd128);

  Shuffle s = AnalyzeShuffle(ins);
#  ifdef DEBUG
  ReportShuffleSpecialization(s);
#  endif
  switch (s.opd) {
    case Shuffle::Operand::LEFT:
    case Shuffle::Operand::RIGHT: {
      LAllocation src;
      // All permute operators currently favor reusing the input register so
      // we're not currently exercising code paths below that do not reuse.
      // Those paths have been exercised in the past however and are believed
      // to be correct.
      bool useAtStartAndReuse = false;
      switch (*s.permuteOp) {
        case LWasmPermuteSimd128::MOVE:
        case LWasmPermuteSimd128::BROADCAST_8x16:
        case LWasmPermuteSimd128::BROADCAST_16x8:
        case LWasmPermuteSimd128::PERMUTE_8x16:
        case LWasmPermuteSimd128::PERMUTE_16x8:
        case LWasmPermuteSimd128::PERMUTE_32x4:
        case LWasmPermuteSimd128::ROTATE_RIGHT_8x16:
        case LWasmPermuteSimd128::SHIFT_LEFT_8x16:
        case LWasmPermuteSimd128::SHIFT_RIGHT_8x16:
          useAtStartAndReuse = true;
          break;
        default:
          MOZ_CRASH("Unexpected operator");
      }
      if (s.opd == Shuffle::Operand::LEFT) {
        if (useAtStartAndReuse) {
          src = useRegisterAtStart(ins->lhs());
        } else {
          src = useRegister(ins->lhs());
        }
      } else {
        if (useAtStartAndReuse) {
          src = useRegisterAtStart(ins->rhs());
        } else {
          src = useRegister(ins->rhs());
        }
      }
      auto* lir =
          new (alloc()) LWasmPermuteSimd128(src, *s.permuteOp, s.control);
      if (useAtStartAndReuse) {
        defineReuseInput(lir, ins, LWasmPermuteSimd128::Src);
      } else {
        define(lir, ins);
      }
      break;
    }
    case Shuffle::Operand::BOTH:
    case Shuffle::Operand::BOTH_SWAPPED: {
      LDefinition temp = LDefinition::BogusTemp();
      switch (*s.shuffleOp) {
        case LWasmShuffleSimd128::BLEND_8x16:
          temp = tempFixed(xmm0);
          break;
        default:
          break;
      }
      LAllocation lhs;
      LAllocation rhs;
      if (s.opd == Shuffle::Operand::BOTH) {
        lhs = useRegisterAtStart(ins->lhs());
        rhs = useRegister(ins->rhs());
      } else {
        lhs = useRegisterAtStart(ins->rhs());
        rhs = useRegister(ins->lhs());
      }
      auto* lir = new (alloc())
          LWasmShuffleSimd128(lhs, rhs, temp, *s.shuffleOp, s.control);
      defineReuseInput(lir, ins, LWasmShuffleSimd128::LhsDest);
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

  // The Masm API is (rhs, lhsDest) and requires AtStart+ReuseInput for the lhs.
  // For type reasons, the rhs will never be the same as the lhs and is
  // therefore a plain Use.

  if (ins->rhs()->type() == MIRType::Int64) {
    auto* lir = new (alloc()) LWasmReplaceInt64LaneSimd128(
        useRegisterAtStart(ins->lhs()), useInt64Register(ins->rhs()));
    defineReuseInput(lir, ins, LWasmReplaceInt64LaneSimd128::LhsDest);
  } else {
    auto* lir = new (alloc()) LWasmReplaceLaneSimd128(
        useRegisterAtStart(ins->lhs()), useRegister(ins->rhs()));
    defineReuseInput(lir, ins, LWasmReplaceLaneSimd128::LhsDest);
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
      // Ideally we save a move on SSE systems by reusing the input register,
      // but since the input and output register types differ, we can't.
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

  bool useAtStart = false;
  bool reuseInput = false;
  LDefinition tempReg = LDefinition::BogusTemp();
  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16Neg:
    case wasm::SimdOp::I16x8Neg:
    case wasm::SimdOp::I32x4Neg:
    case wasm::SimdOp::I64x2Neg:
      // Prefer src != dest to avoid an unconditional src->temp move.
      MOZ_ASSERT(!useAtStart && !reuseInput);
      break;
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
    case wasm::SimdOp::I32x4TruncSSatF32x4:
    case wasm::SimdOp::F32x4ConvertUI32x4:
    case wasm::SimdOp::I16x8ExtAddPairwiseI8x16S:
    case wasm::SimdOp::I16x8ExtAddPairwiseI8x16U:
    case wasm::SimdOp::I32x4ExtAddPairwiseI16x8S:
    case wasm::SimdOp::I32x4ExtAddPairwiseI16x8U:
      // Prefer src == dest to avoid an unconditional src->dest move.
      useAtStart = true;
      reuseInput = true;
      break;
    case wasm::SimdOp::I32x4TruncUSatF32x4:
    case wasm::SimdOp::I32x4TruncSatF64x2SZero:
    case wasm::SimdOp::I32x4TruncSatF64x2UZero:
    case wasm::SimdOp::I8x16Popcnt:
      tempReg = tempSimd128();
      // Prefer src == dest to avoid an unconditional src->dest move.
      useAtStart = true;
      reuseInput = true;
      break;
    case wasm::SimdOp::I16x8WidenLowSI8x16:
    case wasm::SimdOp::I16x8WidenHighSI8x16:
    case wasm::SimdOp::I16x8WidenLowUI8x16:
    case wasm::SimdOp::I16x8WidenHighUI8x16:
    case wasm::SimdOp::I32x4WidenLowSI16x8:
    case wasm::SimdOp::I32x4WidenHighSI16x8:
    case wasm::SimdOp::I32x4WidenLowUI16x8:
    case wasm::SimdOp::I32x4WidenHighUI16x8:
    case wasm::SimdOp::I64x2WidenLowSI32x4:
    case wasm::SimdOp::I64x2WidenHighSI32x4:
    case wasm::SimdOp::I64x2WidenLowUI32x4:
    case wasm::SimdOp::I64x2WidenHighUI32x4:
    case wasm::SimdOp::F32x4ConvertSI32x4:
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
      // Prefer src == dest to exert the lowest register pressure on the
      // surrounding code.
      useAtStart = true;
      MOZ_ASSERT(!reuseInput);
      break;
    default:
      MOZ_CRASH("Unary SimdOp not implemented");
  }

  LUse inputUse =
      useAtStart ? useRegisterAtStart(ins->input()) : useRegister(ins->input());
  LWasmUnarySimd128* lir = new (alloc()) LWasmUnarySimd128(inputUse, tempReg);
  if (reuseInput) {
    defineReuseInput(lir, ins, LWasmUnarySimd128::Src);
  } else {
    define(lir, ins);
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmLoadLaneSimd128(MWasmLoadLaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  LUse base = useRegisterAtStart(ins->base());
  LUse inputUse = useRegisterAtStart(ins->value());
  LAllocation memoryBase = ins->hasMemoryBase()
                               ? useRegisterAtStart(ins->memoryBase())
                               : LAllocation();
  LWasmLoadLaneSimd128* lir = new (alloc()) LWasmLoadLaneSimd128(
      base, inputUse, LDefinition::BogusTemp(), memoryBase);
  defineReuseInput(lir, ins, LWasmLoadLaneSimd128::Src);
#else
  MOZ_CRASH("No SIMD");
#endif
}

void LIRGenerator::visitWasmStoreLaneSimd128(MWasmStoreLaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  LUse base = useRegisterAtStart(ins->base());
  LUse input = useRegisterAtStart(ins->value());
  LAllocation memoryBase = ins->hasMemoryBase()
                               ? useRegisterAtStart(ins->memoryBase())
                               : LAllocation();
  LWasmStoreLaneSimd128* lir = new (alloc())
      LWasmStoreLaneSimd128(base, input, LDefinition::BogusTemp(), memoryBase);
  add(lir, ins);
#else
  MOZ_CRASH("No SIMD");
#endif
}

#ifdef ENABLE_WASM_SIMD

bool LIRGeneratorX86Shared::canFoldReduceSimd128AndBranch(wasm::SimdOp op) {
  switch (op) {
    case wasm::SimdOp::V128AnyTrue:
    case wasm::SimdOp::I8x16AllTrue:
    case wasm::SimdOp::I16x8AllTrue:
    case wasm::SimdOp::I32x4AllTrue:
    case wasm::SimdOp::I64x2AllTrue:
    case wasm::SimdOp::I16x8Bitmask:
      return true;
    default:
      return false;
  }
}

bool LIRGeneratorX86Shared::canEmitWasmReduceSimd128AtUses(
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

#endif  // ENABLE_WASM_SIMD

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
    // Ideally we would reuse the input register for floating extract_lane if
    // the lane is zero, but constraints in the register allocator require the
    // input and output register types to be the same.
    auto* lir = new (alloc()) LWasmReduceSimd128(
        useRegisterAtStart(ins->input()), LDefinition::BogusTemp());
    define(lir, ins);
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}
