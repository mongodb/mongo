/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/Lowering-x86-shared.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/MIR.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Abs;
using mozilla::FloorLog2;
using mozilla::Swap;

LTableSwitch*
LIRGeneratorX86Shared::newLTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                                       MTableSwitch* tableswitch)
{
    return new(alloc()) LTableSwitch(in, inputCopy, temp(), tableswitch);
}

LTableSwitchV*
LIRGeneratorX86Shared::newLTableSwitchV(MTableSwitch* tableswitch)
{
    return new(alloc()) LTableSwitchV(useBox(tableswitch->getOperand(0)),
                                      temp(), tempDouble(), temp(), tableswitch);
}

void
LIRGeneratorX86Shared::visitPowHalf(MPowHalf* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(input->type() == MIRType::Double);
    LPowHalfD* lir = new(alloc()) LPowHalfD(useRegisterAtStart(input));
    define(lir, ins);
}

void
LIRGeneratorX86Shared::lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                     MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegisterAtStart(lhs));

    // shift operator should be constant or in register ecx
    // x86 can't shift a non-ecx register
    if (rhs->isConstant())
        ins->setOperand(1, useOrConstantAtStart(rhs));
    else
        ins->setOperand(1, lhs != rhs ? useFixed(rhs, ecx) : useFixedAtStart(rhs, ecx));

    defineReuseInput(ins, mir, 0);
}

template<size_t Temps>
void
LIRGeneratorX86Shared::lowerForShiftInt64(LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
                                          MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    ins->setInt64Operand(0, useInt64RegisterAtStart(lhs));
#if defined(JS_NUNBOX32)
    if (mir->isRotate())
        ins->setTemp(0, temp());
#endif

    static_assert(LShiftI64::Rhs == INT64_PIECES, "Assume Rhs is located at INT64_PIECES.");
    static_assert(LRotateI64::Count == INT64_PIECES, "Assume Count is located at INT64_PIECES.");

    // shift operator should be constant or in register ecx
    // x86 can't shift a non-ecx register
    if (rhs->isConstant()) {
        ins->setOperand(INT64_PIECES, useOrConstantAtStart(rhs));
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
    LInstructionHelper<INT64_PIECES, INT64_PIECES+1, 0>* ins, MDefinition* mir,
    MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorX86Shared::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES+1, 1>* ins, MDefinition* mir,
    MDefinition* lhs, MDefinition* rhs);

void
LIRGeneratorX86Shared::lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                                   MDefinition* input)
{
    ins->setOperand(0, useRegisterAtStart(input));
    defineReuseInput(ins, mir, 0);
}

void
LIRGeneratorX86Shared::lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                   MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegisterAtStart(lhs));
    ins->setOperand(1, lhs != rhs ? useOrConstant(rhs) : useOrConstantAtStart(rhs));
    defineReuseInput(ins, mir, 0);
}

template<size_t Temps>
void
LIRGeneratorX86Shared::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    // Without AVX, we'll need to use the x86 encodings where one of the
    // inputs must be the same location as the output.
    if (!Assembler::HasAVX()) {
        ins->setOperand(0, useRegisterAtStart(lhs));
        ins->setOperand(1, lhs != rhs ? use(rhs) : useAtStart(rhs));
        defineReuseInput(ins, mir, 0);
    } else {
        ins->setOperand(0, useRegisterAtStart(lhs));
        ins->setOperand(1, useAtStart(rhs));
        define(ins, mir);
    }
}

template void LIRGeneratorX86Shared::lowerForFPU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                                 MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorX86Shared::lowerForFPU(LInstructionHelper<1, 2, 1>* ins, MDefinition* mir,
                                                 MDefinition* lhs, MDefinition* rhs);

void
LIRGeneratorX86Shared::lowerForCompIx4(LSimdBinaryCompIx4* ins, MSimdBinaryComp* mir, MDefinition* lhs, MDefinition* rhs)
{
    lowerForALU(ins, mir, lhs, rhs);
}

void
LIRGeneratorX86Shared::lowerForCompFx4(LSimdBinaryCompFx4* ins, MSimdBinaryComp* mir, MDefinition* lhs, MDefinition* rhs)
{
    // Swap the operands around to fit the instructions that x86 actually has.
    // We do this here, before register allocation, so that we don't need
    // temporaries and copying afterwards.
    switch (mir->operation()) {
      case MSimdBinaryComp::greaterThan:
      case MSimdBinaryComp::greaterThanOrEqual:
        mir->reverse();
        Swap(lhs, rhs);
        break;
      default:
        break;
    }

    lowerForFPU(ins, mir, lhs, rhs);
}

void
LIRGeneratorX86Shared::lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                                               MDefinition* lhs, MDefinition* rhs)
{
    baab->setOperand(0, useRegisterAtStart(lhs));
    baab->setOperand(1, useRegisterOrConstantAtStart(rhs));
    add(baab, mir);
}

void
LIRGeneratorX86Shared::lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs)
{
    // Note: If we need a negative zero check, lhs is used twice.
    LAllocation lhsCopy = mul->canBeNegativeZero() ? use(lhs) : LAllocation();
    LMulI* lir = new(alloc()) LMulI(useRegisterAtStart(lhs), useOrConstant(rhs), lhsCopy);
    if (mul->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    defineReuseInput(lir, mul, 0);
}

void
LIRGeneratorX86Shared::lowerDivI(MDiv* div)
{
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
            if (!div->canBeNegativeDividend()) {
                // Numerator is unsigned, so does not need adjusting.
                lir = new(alloc()) LDivPowTwoI(lhs, lhs, shift, rhs < 0);
            } else {
                // Numerator is signed, and needs adjusting, and an extra
                // lhs copy register is needed.
                lir = new(alloc()) LDivPowTwoI(lhs, useRegister(div->lhs()), shift, rhs < 0);
            }
            if (div->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            defineReuseInput(lir, div, 0);
            return;
        }
        if (rhs != 0) {
            LDivOrModConstantI* lir;
            lir = new(alloc()) LDivOrModConstantI(useRegister(div->lhs()), rhs, tempFixed(eax));
            if (div->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            defineFixed(lir, div, LAllocation(AnyRegister(edx)));
            return;
        }
    }

    LDivI* lir = new(alloc()) LDivI(useRegister(div->lhs()), useRegister(div->rhs()),
                                    tempFixed(edx));
    if (div->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    defineFixed(lir, div, LAllocation(AnyRegister(eax)));
}

void
LIRGeneratorX86Shared::lowerModI(MMod* mod)
{
    if (mod->isUnsigned()) {
        lowerUMod(mod);
        return;
    }

    if (mod->rhs()->isConstant()) {
        int32_t rhs = mod->rhs()->toConstant()->toInt32();
        int32_t shift = FloorLog2(Abs(rhs));
        if (rhs != 0 && uint32_t(1) << shift == Abs(rhs)) {
            LModPowTwoI* lir = new(alloc()) LModPowTwoI(useRegisterAtStart(mod->lhs()), shift);
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            defineReuseInput(lir, mod, 0);
            return;
        }
        if (rhs != 0) {
            LDivOrModConstantI* lir;
            lir = new(alloc()) LDivOrModConstantI(useRegister(mod->lhs()), rhs, tempFixed(edx));
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            defineFixed(lir, mod, LAllocation(AnyRegister(eax)));
            return;
        }
    }

    LModI* lir = new(alloc()) LModI(useRegister(mod->lhs()),
                                    useRegister(mod->rhs()),
                                    tempFixed(eax));
    if (mod->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    defineFixed(lir, mod, LAllocation(AnyRegister(edx)));
}

void
LIRGeneratorX86Shared::visitWasmSelect(MWasmSelect* ins)
{
    if (ins->type() == MIRType::Int64) {
        auto* lir = new(alloc()) LWasmSelectI64(useInt64RegisterAtStart(ins->trueExpr()),
                                                useInt64(ins->falseExpr()),
                                                useRegister(ins->condExpr()));

        defineInt64ReuseInput(lir, ins, LWasmSelectI64::TrueExprIndex);
        return;
    }

    auto* lir = new(alloc()) LWasmSelect(useRegisterAtStart(ins->trueExpr()),
                                         use(ins->falseExpr()),
                                         useRegister(ins->condExpr()));

    defineReuseInput(lir, ins, LWasmSelect::TrueExprIndex);
}

void
LIRGeneratorX86Shared::visitWasmNeg(MWasmNeg* ins)
{
    switch (ins->type()) {
      case MIRType::Int32:
        defineReuseInput(new(alloc()) LNegI(useRegisterAtStart(ins->input())), ins, 0);
        break;
      case MIRType::Float32:
        defineReuseInput(new(alloc()) LNegF(useRegisterAtStart(ins->input())), ins, 0);
        break;
      case MIRType::Double:
        defineReuseInput(new(alloc()) LNegD(useRegisterAtStart(ins->input())), ins, 0);
        break;
      default:
        MOZ_CRASH();
    }
}

void
LIRGeneratorX86Shared::lowerUDiv(MDiv* div)
{
    if (div->rhs()->isConstant()) {
        uint32_t rhs = div->rhs()->toConstant()->toInt32();
        int32_t shift = FloorLog2(rhs);

        LAllocation lhs = useRegisterAtStart(div->lhs());
        if (rhs != 0 && uint32_t(1) << shift == rhs) {
            LDivPowTwoI* lir = new(alloc()) LDivPowTwoI(lhs, lhs, shift, false);
            if (div->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            defineReuseInput(lir, div, 0);
        } else {
            LUDivOrModConstant* lir = new(alloc()) LUDivOrModConstant(useRegister(div->lhs()),
                                                                      rhs, tempFixed(eax));
            if (div->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            defineFixed(lir, div, LAllocation(AnyRegister(edx)));
        }
        return;
    }

    LUDivOrMod* lir = new(alloc()) LUDivOrMod(useRegister(div->lhs()),
                                              useRegister(div->rhs()),
                                              tempFixed(edx));
    if (div->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    defineFixed(lir, div, LAllocation(AnyRegister(eax)));
}

void
LIRGeneratorX86Shared::lowerUMod(MMod* mod)
{
    if (mod->rhs()->isConstant()) {
        uint32_t rhs = mod->rhs()->toConstant()->toInt32();
        int32_t shift = FloorLog2(rhs);

        if (rhs != 0 && uint32_t(1) << shift == rhs) {
            LModPowTwoI* lir = new(alloc()) LModPowTwoI(useRegisterAtStart(mod->lhs()), shift);
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            defineReuseInput(lir, mod, 0);
        } else {
            LUDivOrModConstant* lir = new(alloc()) LUDivOrModConstant(useRegister(mod->lhs()),
                                                                      rhs, tempFixed(edx));
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            defineFixed(lir, mod, LAllocation(AnyRegister(eax)));
        }
        return;
    }

    LUDivOrMod* lir = new(alloc()) LUDivOrMod(useRegister(mod->lhs()),
                                              useRegister(mod->rhs()),
                                              tempFixed(eax));
    if (mod->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    defineFixed(lir, mod, LAllocation(AnyRegister(edx)));
}

void
LIRGeneratorX86Shared::lowerUrshD(MUrsh* mir)
{
    MDefinition* lhs = mir->lhs();
    MDefinition* rhs = mir->rhs();

    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    MOZ_ASSERT(rhs->type() == MIRType::Int32);
    MOZ_ASSERT(mir->type() == MIRType::Double);

#ifdef JS_CODEGEN_X64
    MOZ_ASSERT(ecx == rcx);
#endif

    LUse lhsUse = useRegisterAtStart(lhs);
    LAllocation rhsAlloc = rhs->isConstant() ? useOrConstant(rhs) : useFixed(rhs, ecx);

    LUrshD* lir = new(alloc()) LUrshD(lhsUse, rhsAlloc, tempCopy(lhs, 0));
    define(lir, mir);
}

void
LIRGeneratorX86Shared::lowerTruncateDToInt32(MTruncateToInt32* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Double);

    LDefinition maybeTemp = Assembler::HasSSE3() ? LDefinition::BogusTemp() : tempDouble();
    define(new(alloc()) LTruncateDToInt32(useRegister(opd), maybeTemp), ins);
}

void
LIRGeneratorX86Shared::lowerTruncateFToInt32(MTruncateToInt32* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Float32);

    LDefinition maybeTemp = Assembler::HasSSE3() ? LDefinition::BogusTemp() : tempFloat32();
    define(new(alloc()) LTruncateFToInt32(useRegister(opd), maybeTemp), ins);
}

void
LIRGeneratorX86Shared::lowerCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins,
                                                             bool useI386ByteRegisters)
{
    MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
    MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

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
        if (useI386ByteRegisters && ins->isByteArray())
            newval = useFixed(ins->newval(), ebx);
        else
            newval = useRegister(ins->newval());
    }

    const LAllocation oldval = useRegister(ins->oldval());

    LCompareExchangeTypedArrayElement* lir =
        new(alloc()) LCompareExchangeTypedArrayElement(elements, index, oldval, newval, tempDef);

    if (fixedOutput)
        defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    else
        define(lir, ins);
}

void
LIRGeneratorX86Shared::lowerAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins,
                                                            bool useI386ByteRegisters)
{
    MOZ_ASSERT(ins->arrayType() <= Scalar::Uint32);

    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());
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
        // This restriction is bug 1077305.
        MOZ_ASSERT(ins->type() == MIRType::Double);
        tempDef = temp();
    }

    LAtomicExchangeTypedArrayElement* lir =
        new(alloc()) LAtomicExchangeTypedArrayElement(elements, index, value, tempDef);

    if (useI386ByteRegisters && ins->isByteArray())
        defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    else
        define(lir, ins);
}

void
LIRGeneratorX86Shared::lowerAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins,
                                                         bool useI386ByteRegisters)
{
    MOZ_ASSERT(ins->arrayType() != Scalar::Uint8Clamped);
    MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
    MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    // Case 1: the result of the operation is not used.
    //
    // We'll emit a single instruction: LOCK ADD, LOCK SUB, LOCK AND,
    // LOCK OR, or LOCK XOR.  We can do this even for the Uint32 case.

    if (!ins->hasUses()) {
        LAllocation value;
        if (useI386ByteRegisters && ins->isByteArray() && !ins->value()->isConstant())
            value = useFixed(ins->value(), ebx);
        else
            value = useRegisterOrConstant(ins->value());

        LAtomicTypedArrayElementBinopForEffect* lir =
            new(alloc()) LAtomicTypedArrayElementBinopForEffect(elements, index, value);

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

    bool bitOp = !(ins->operation() == AtomicFetchAddOp || ins->operation() == AtomicFetchSubOp);
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
        if (ins->value()->isConstant())
            value = useRegisterOrConstant(ins->value());
        else
            value = useFixed(ins->value(), ebx);
        if (bitOp)
            tempDef1 = tempFixed(ecx);
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

    LAtomicTypedArrayElementBinop* lir =
        new(alloc()) LAtomicTypedArrayElementBinop(elements, index, value, tempDef1, tempDef2);

    if (fixedOutput)
        defineFixed(lir, ins, LAllocation(AnyRegister(eax)));
    else if (reuseInput)
        defineReuseInput(lir, ins, LAtomicTypedArrayElementBinop::valueOp);
    else
        define(lir, ins);
}

void
LIRGeneratorX86Shared::visitSimdInsertElement(MSimdInsertElement* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));

    LUse vec = useRegisterAtStart(ins->vector());
    LUse val = useRegister(ins->value());
    switch (ins->type()) {
      case MIRType::Int8x16:
      case MIRType::Bool8x16:
        // When SSE 4.1 is not available, we need to go via the stack.
        // This requires the value to be inserted to be in %eax-%edx.
        // Pick %ebx since other instructions use %eax or %ecx hard-wired.
#if defined(JS_CODEGEN_X86)
        if (!AssemblerX86Shared::HasSSE41())
            val = useFixed(ins->value(), ebx);
#endif
        defineReuseInput(new(alloc()) LSimdInsertElementI(vec, val), ins, 0);
        break;
      case MIRType::Int16x8:
      case MIRType::Int32x4:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4:
        defineReuseInput(new(alloc()) LSimdInsertElementI(vec, val), ins, 0);
        break;
      case MIRType::Float32x4:
        defineReuseInput(new(alloc()) LSimdInsertElementF(vec, val), ins, 0);
        break;
      default:
        MOZ_CRASH("Unknown SIMD kind when generating constant");
    }
}

void
LIRGeneratorX86Shared::visitSimdExtractElement(MSimdExtractElement* ins)
{
    MOZ_ASSERT(IsSimdType(ins->input()->type()));
    MOZ_ASSERT(!IsSimdType(ins->type()));

    switch (ins->input()->type()) {
      case MIRType::Int8x16:
      case MIRType::Int16x8:
      case MIRType::Int32x4: {
        MOZ_ASSERT(ins->signedness() != SimdSign::NotApplicable);
        LUse use = useRegisterAtStart(ins->input());
        if (ins->type() == MIRType::Double) {
            // Extract an Uint32 lane into a double.
            MOZ_ASSERT(ins->signedness() == SimdSign::Unsigned);
            define(new (alloc()) LSimdExtractElementU2D(use, temp()), ins);
        } else {
            auto* lir = new (alloc()) LSimdExtractElementI(use);
#if defined(JS_CODEGEN_X86)
            // On x86 (32-bit), we may need to use movsbl or movzbl instructions
            // to sign or zero extend the extracted lane to 32 bits. The 8-bit
            // version of these instructions require a source register that is
            // %al, %bl, %cl, or %dl.
            // Fix it to %ebx since we can't express that constraint better.
            if (ins->input()->type() == MIRType::Int8x16) {
                defineFixed(lir, ins, LAllocation(AnyRegister(ebx)));
                return;
            }
#endif
            define(lir, ins);
        }
        break;
      }
      case MIRType::Float32x4: {
        MOZ_ASSERT(ins->signedness() == SimdSign::NotApplicable);
        LUse use = useRegisterAtStart(ins->input());
        define(new(alloc()) LSimdExtractElementF(use), ins);
        break;
      }
      case MIRType::Bool8x16:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4: {
        MOZ_ASSERT(ins->signedness() == SimdSign::NotApplicable);
        LUse use = useRegisterAtStart(ins->input());
        define(new(alloc()) LSimdExtractElementB(use), ins);
        break;
      }
      default:
        MOZ_CRASH("Unknown SIMD kind when extracting element");
    }
}

void
LIRGeneratorX86Shared::visitSimdBinaryArith(MSimdBinaryArith* ins)
{
    MOZ_ASSERT(IsSimdType(ins->lhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->rhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));

    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    if (ins->isCommutative())
        ReorderCommutative(&lhs, &rhs, ins);

    switch (ins->type()) {
      case MIRType::Int8x16: {
          LSimdBinaryArithIx16* lir = new (alloc()) LSimdBinaryArithIx16();
          lir->setTemp(0, LDefinition::BogusTemp());
          lowerForFPU(lir, ins, lhs, rhs);
          return;
      }

      case MIRType::Int16x8: {
          LSimdBinaryArithIx8* lir = new (alloc()) LSimdBinaryArithIx8();
          lir->setTemp(0, LDefinition::BogusTemp());
          lowerForFPU(lir, ins, lhs, rhs);
          return;
      }

      case MIRType::Int32x4: {
          LSimdBinaryArithIx4* lir = new (alloc()) LSimdBinaryArithIx4();
          bool needsTemp =
              ins->operation() == MSimdBinaryArith::Op_mul && !MacroAssembler::HasSSE41();
          lir->setTemp(0, needsTemp ? temp(LDefinition::SIMD128INT) : LDefinition::BogusTemp());
          lowerForFPU(lir, ins, lhs, rhs);
          return;
      }

      case MIRType::Float32x4: {
          LSimdBinaryArithFx4* lir = new (alloc()) LSimdBinaryArithFx4();

          bool needsTemp = ins->operation() == MSimdBinaryArith::Op_max ||
              ins->operation() == MSimdBinaryArith::Op_minNum ||
              ins->operation() == MSimdBinaryArith::Op_maxNum;
          lir->setTemp(0,
                       needsTemp ? temp(LDefinition::SIMD128FLOAT) : LDefinition::BogusTemp());
          lowerForFPU(lir, ins, lhs, rhs);
          return;
      }

      default:
        MOZ_CRASH("unknown simd type on binary arith operation");
    }
}

void
LIRGeneratorX86Shared::visitSimdBinarySaturating(MSimdBinarySaturating* ins)
{
    MOZ_ASSERT(IsSimdType(ins->lhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->rhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));

    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    if (ins->isCommutative())
        ReorderCommutative(&lhs, &rhs, ins);

    LSimdBinarySaturating* lir = new (alloc()) LSimdBinarySaturating();
    lowerForFPU(lir, ins, lhs, rhs);
}

void
LIRGeneratorX86Shared::visitSimdSelect(MSimdSelect* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));

    LSimdSelect* lins = new(alloc()) LSimdSelect;
    MDefinition* r0 = ins->getOperand(0);
    MDefinition* r1 = ins->getOperand(1);
    MDefinition* r2 = ins->getOperand(2);

    lins->setOperand(0, useRegister(r0));
    lins->setOperand(1, useRegister(r1));
    lins->setOperand(2, useRegister(r2));
    lins->setTemp(0, temp(LDefinition::SIMD128FLOAT));

    define(lins, ins);
}

void
LIRGeneratorX86Shared::visitSimdSplat(MSimdSplat* ins)
{
    LAllocation x = useRegisterAtStart(ins->getOperand(0));

    switch (ins->type()) {
      case MIRType::Int8x16:
        define(new (alloc()) LSimdSplatX16(x), ins);
        break;
      case MIRType::Int16x8:
        define(new (alloc()) LSimdSplatX8(x), ins);
        break;
      case MIRType::Int32x4:
      case MIRType::Float32x4:
      case MIRType::Bool8x16:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4:
        // Use the SplatX4 instruction for all boolean splats. Since the input
        // value is a 32-bit int that is either 0 or -1, the X4 splat gives
        // the right result for all boolean geometries.
        // For floats, (Non-AVX) codegen actually wants the input and the output
        // to be in the same register, but we can't currently use
        // defineReuseInput because they have different types (scalar vs
        // vector), so a spill slot for one may not be suitable for the other.
        define(new (alloc()) LSimdSplatX4(x), ins);
        break;
      default:
        MOZ_CRASH("Unknown SIMD kind");
    }
}

void
LIRGeneratorX86Shared::visitSimdValueX4(MSimdValueX4* ins)
{
    switch (ins->type()) {
      case MIRType::Float32x4: {
        // Ideally, x would be used at start and reused for the output, however
        // register allocation currently doesn't permit us to tie together two
        // virtual registers with different types.
        LAllocation x = useRegister(ins->getOperand(0));
        LAllocation y = useRegister(ins->getOperand(1));
        LAllocation z = useRegister(ins->getOperand(2));
        LAllocation w = useRegister(ins->getOperand(3));
        LDefinition t = temp(LDefinition::SIMD128FLOAT);
        define(new (alloc()) LSimdValueFloat32x4(x, y, z, w, t), ins);
        break;
      }
      case MIRType::Bool32x4:
      case MIRType::Int32x4: {
        // No defineReuseInput => useAtStart for everyone.
        LAllocation x = useRegisterAtStart(ins->getOperand(0));
        LAllocation y = useRegisterAtStart(ins->getOperand(1));
        LAllocation z = useRegisterAtStart(ins->getOperand(2));
        LAllocation w = useRegisterAtStart(ins->getOperand(3));
        define(new(alloc()) LSimdValueInt32x4(x, y, z, w), ins);
        break;
      }
      default:
        MOZ_CRASH("Unknown SIMD kind");
    }
}

void
LIRGeneratorX86Shared::visitSimdSwizzle(MSimdSwizzle* ins)
{
    MOZ_ASSERT(IsSimdType(ins->input()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));

    if (IsIntegerSimdType(ins->input()->type())) {
        LUse use = useRegisterAtStart(ins->input());
        LSimdSwizzleI* lir = new (alloc()) LSimdSwizzleI(use);
        define(lir, ins);
        // We need a GPR temp register for pre-SSSE3 codegen (no vpshufb).
        if (Assembler::HasSSSE3()) {
            lir->setTemp(0, LDefinition::BogusTemp());
        } else {
            // The temp must be a GPR usable with 8-bit loads and stores.
#if defined(JS_CODEGEN_X86)
            lir->setTemp(0, tempFixed(ebx));
#else
            lir->setTemp(0, temp());
#endif
        }
    } else if (ins->input()->type() == MIRType::Float32x4) {
        LUse use = useRegisterAtStart(ins->input());
        LSimdSwizzleF* lir = new (alloc()) LSimdSwizzleF(use);
        define(lir, ins);
        lir->setTemp(0, LDefinition::BogusTemp());
    } else {
        MOZ_CRASH("Unknown SIMD kind when getting lane");
    }
}

void
LIRGeneratorX86Shared::visitSimdShuffle(MSimdShuffle* ins)
{
    MOZ_ASSERT(IsSimdType(ins->lhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->rhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));
    if (ins->type() == MIRType::Int32x4 || ins->type() == MIRType::Float32x4) {
        bool zFromLHS = ins->lane(2) < 4;
        bool wFromLHS = ins->lane(3) < 4;
        uint32_t lanesFromLHS = (ins->lane(0) < 4) + (ins->lane(1) < 4) + zFromLHS + wFromLHS;

        LSimdShuffleX4* lir = new (alloc()) LSimdShuffleX4();
        lowerForFPU(lir, ins, ins->lhs(), ins->rhs());

        // See codegen for requirements details.
        LDefinition temp =
          (lanesFromLHS == 3) ? tempCopy(ins->rhs(), 1) : LDefinition::BogusTemp();
        lir->setTemp(0, temp);
    } else {
        MOZ_ASSERT(ins->type() == MIRType::Int8x16 || ins->type() == MIRType::Int16x8);
        LSimdShuffle* lir = new (alloc()) LSimdShuffle();
        lir->setOperand(0, useRegister(ins->lhs()));
        lir->setOperand(1, useRegister(ins->rhs()));
        define(lir, ins);
        // We need a GPR temp register for pre-SSSE3 codegen, and an SSE temp
        // when using pshufb.
        if (Assembler::HasSSSE3()) {
            lir->setTemp(0, temp(LDefinition::SIMD128INT));
        } else {
            // The temp must be a GPR usable with 8-bit loads and stores.
#if defined(JS_CODEGEN_X86)
            lir->setTemp(0, tempFixed(ebx));
#else
            lir->setTemp(0, temp());
#endif
        }
    }
}

void
LIRGeneratorX86Shared::visitSimdGeneralShuffle(MSimdGeneralShuffle* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));

    size_t numOperands = ins->numVectors() + ins->numLanes();

    LSimdGeneralShuffleBase* lir;
    if (IsIntegerSimdType(ins->type())) {
#if defined(JS_CODEGEN_X86)
        // The temp register must be usable with 8-bit load and store
        // instructions, so one of %eax-%edx.
        LDefinition t;
        if (ins->type() == MIRType::Int8x16)
            t = tempFixed(ebx);
        else
            t = temp();
#else
        LDefinition t = temp();
#endif
        lir = allocateVariadic<LSimdGeneralShuffleI>(numOperands, t);
    } else if (ins->type() == MIRType::Float32x4) {
        lir = allocateVariadic<LSimdGeneralShuffleF>(numOperands, temp());
    } else {
        MOZ_CRASH("Unknown SIMD kind when doing a shuffle");
    }

    if (!lir)
        return;

    for (unsigned i = 0; i < ins->numVectors(); i++) {
        MOZ_ASSERT(IsSimdType(ins->vector(i)->type()));
        lir->setOperand(i, useRegister(ins->vector(i)));
    }

    for (unsigned i = 0; i < ins->numLanes(); i++) {
        MOZ_ASSERT(ins->lane(i)->type() == MIRType::Int32);
        // Note that there can be up to 16 lane arguments, so we can't assume
        // that they all get an allocated register.
        lir->setOperand(i + ins->numVectors(), use(ins->lane(i)));
    }

    assignSnapshot(lir, Bailout_BoundsCheck);
    define(lir, ins);
}

void
LIRGeneratorX86Shared::visitCopySign(MCopySign* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    MOZ_ASSERT(IsFloatingPointType(lhs->type()));
    MOZ_ASSERT(lhs->type() == rhs->type());
    MOZ_ASSERT(lhs->type() == ins->type());

    LInstructionHelper<1, 2, 2>* lir;
    if (lhs->type() == MIRType::Double)
        lir = new(alloc()) LCopySignD();
    else
        lir = new(alloc()) LCopySignF();

    // As lowerForFPU, but we want rhs to be in a FP register too.
    lir->setOperand(0, useRegisterAtStart(lhs));
    lir->setOperand(1, lhs != rhs ? useRegister(rhs) : useRegisterAtStart(rhs));
    if (!Assembler::HasAVX())
        defineReuseInput(lir, ins, 0);
    else
        define(lir, ins);
}
