/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/TypePolicy.h"

#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using JS::DoubleNaNValue;

static void
EnsureOperandNotFloat32(TempAllocator& alloc, MInstruction* def, unsigned op)
{
    MDefinition* in = def->getOperand(op);
    if (in->type() == MIRType::Float32) {
        MToDouble* replace = MToDouble::New(alloc, in);
        def->block()->insertBefore(def, replace);
        if (def->isRecoveredOnBailout())
            replace->setRecoveredOnBailout();
        def->replaceOperand(op, replace);
    }
}

MDefinition*
js::jit::AlwaysBoxAt(TempAllocator& alloc, MInstruction* at, MDefinition* operand)
{
    MDefinition* boxedOperand = operand;
    // Replace Float32 by double
    if (operand->type() == MIRType::Float32) {
        MInstruction* replace = MToDouble::New(alloc, operand);
        at->block()->insertBefore(at, replace);
        boxedOperand = replace;
    }
    MBox* box = MBox::New(alloc, boxedOperand);
    at->block()->insertBefore(at, box);
    return box;
}

static MDefinition*
BoxAt(TempAllocator& alloc, MInstruction* at, MDefinition* operand)
{
    if (operand->isUnbox())
        return operand->toUnbox()->input();
    return AlwaysBoxAt(alloc, at, operand);
}

bool
BoxInputsPolicy::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
        MDefinition* in = ins->getOperand(i);
        if (in->type() == MIRType::Value)
            continue;
        ins->replaceOperand(i, BoxAt(alloc, ins, in));
    }
    return true;
}

bool
ArithPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MIRType specialization = ins->typePolicySpecialization();
    if (specialization == MIRType::None)
        return BoxInputsPolicy::staticAdjustInputs(alloc, ins);

    MOZ_ASSERT(ins->type() == MIRType::Double || ins->type() == MIRType::Int32 || ins->type() == MIRType::Float32);

    for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
        MDefinition* in = ins->getOperand(i);
        if (in->type() == ins->type())
            continue;

        MInstruction* replace;

        if (ins->type() == MIRType::Double)
            replace = MToDouble::New(alloc, in);
        else if (ins->type() == MIRType::Float32)
            replace = MToFloat32::New(alloc, in);
        else
            replace = MToNumberInt32::New(alloc, in);

        ins->block()->insertBefore(ins, replace);
        ins->replaceOperand(i, replace);

        if (!replace->typePolicy()->adjustInputs(alloc, replace))
            return false;
    }

    return true;
}

bool
AllDoublePolicy::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
        MDefinition* in = ins->getOperand(i);
        if (in->type() == MIRType::Double)
            continue;

        if (!alloc.ensureBallast())
            return false;
        MInstruction* replace = MToDouble::New(alloc, in);

        ins->block()->insertBefore(ins, replace);
        ins->replaceOperand(i, replace);

        if (!replace->typePolicy()->adjustInputs(alloc, replace))
            return false;
    }

    return true;
}

bool
ComparePolicy::adjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MOZ_ASSERT(def->isCompare());
    MCompare* compare = def->toCompare();

    // Convert Float32 operands to doubles
    for (size_t i = 0; i < 2; i++) {
        MDefinition* in = def->getOperand(i);
        if (in->type() == MIRType::Float32) {
            MInstruction* replace = MToDouble::New(alloc, in);
            def->block()->insertBefore(def, replace);
            def->replaceOperand(i, replace);
        }
    }

    // Box inputs to get value
    if (compare->compareType() == MCompare::Compare_Unknown ||
        compare->compareType() == MCompare::Compare_Bitwise)
    {
        return BoxInputsPolicy::staticAdjustInputs(alloc, def);
    }

    // Compare_Boolean specialization is done for "Anything === Bool"
    // If the LHS is boolean, we set the specialization to Compare_Int32.
    // This matches other comparisons of the form bool === bool and
    // generated code of Compare_Int32 is more efficient.
    if (compare->compareType() == MCompare::Compare_Boolean &&
        def->getOperand(0)->type() == MIRType::Boolean)
    {
       compare->setCompareType(MCompare::Compare_Int32MaybeCoerceBoth);
    }

    // Compare_Boolean specialization is done for "Anything === Bool"
    // As of previous line Anything can't be Boolean
    if (compare->compareType() == MCompare::Compare_Boolean) {
        // Unbox rhs that is definitely Boolean
        MDefinition* rhs = def->getOperand(1);
        if (rhs->type() != MIRType::Boolean) {
            MInstruction* unbox = MUnbox::New(alloc, rhs, MIRType::Boolean, MUnbox::Infallible);
            def->block()->insertBefore(def, unbox);
            def->replaceOperand(1, unbox);
            if (!unbox->typePolicy()->adjustInputs(alloc, unbox))
                return false;
        }

        MOZ_ASSERT(def->getOperand(0)->type() != MIRType::Boolean);
        MOZ_ASSERT(def->getOperand(1)->type() == MIRType::Boolean);
        return true;
    }

    // Compare_StrictString specialization is done for "Anything === String"
    // If the LHS is string, we set the specialization to Compare_String.
    if (compare->compareType() == MCompare::Compare_StrictString &&
        def->getOperand(0)->type() == MIRType::String)
    {
       compare->setCompareType(MCompare::Compare_String);
    }

    // Compare_StrictString specialization is done for "Anything === String"
    // As of previous line Anything can't be String
    if (compare->compareType() == MCompare::Compare_StrictString) {
        // Unbox rhs that is definitely String
        MDefinition* rhs = def->getOperand(1);
        if (rhs->type() != MIRType::String) {
            MInstruction* unbox = MUnbox::New(alloc, rhs, MIRType::String, MUnbox::Infallible);
            def->block()->insertBefore(def, unbox);
            def->replaceOperand(1, unbox);
            if (!unbox->typePolicy()->adjustInputs(alloc, unbox))
                return false;
        }

        MOZ_ASSERT(def->getOperand(0)->type() != MIRType::String);
        MOZ_ASSERT(def->getOperand(1)->type() == MIRType::String);
        return true;
    }

    if (compare->compareType() == MCompare::Compare_Undefined ||
        compare->compareType() == MCompare::Compare_Null)
    {
        // Nothing to do for undefined and null, lowering handles all types.
        return true;
    }

    // Convert all inputs to the right input type
    MIRType type = compare->inputType();
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Double || type == MIRType::Float32 ||
               type == MIRType::Object || type == MIRType::String || type == MIRType::Symbol);
    for (size_t i = 0; i < 2; i++) {
        MDefinition* in = def->getOperand(i);
        if (in->type() == type)
            continue;

        MInstruction* replace;

        switch (type) {
          case MIRType::Double: {
            MToFPInstruction::ConversionKind convert = MToFPInstruction::NumbersOnly;
            if (compare->compareType() == MCompare::Compare_DoubleMaybeCoerceLHS && i == 0)
                convert = MToFPInstruction::NonNullNonStringPrimitives;
            else if (compare->compareType() == MCompare::Compare_DoubleMaybeCoerceRHS && i == 1)
                convert = MToFPInstruction::NonNullNonStringPrimitives;
            replace = MToDouble::New(alloc, in, convert);
            break;
          }
          case MIRType::Float32: {
            MToFPInstruction::ConversionKind convert = MToFPInstruction::NumbersOnly;
            if (compare->compareType() == MCompare::Compare_DoubleMaybeCoerceLHS && i == 0)
                convert = MToFPInstruction::NonNullNonStringPrimitives;
            else if (compare->compareType() == MCompare::Compare_DoubleMaybeCoerceRHS && i == 1)
                convert = MToFPInstruction::NonNullNonStringPrimitives;
            replace = MToFloat32::New(alloc, in, convert);
            break;
          }
          case MIRType::Int32: {
            IntConversionInputKind convert = IntConversionInputKind::NumbersOnly;
            if (compare->compareType() == MCompare::Compare_Int32MaybeCoerceBoth ||
                (compare->compareType() == MCompare::Compare_Int32MaybeCoerceLHS && i == 0) ||
                (compare->compareType() == MCompare::Compare_Int32MaybeCoerceRHS && i == 1))
            {
                convert = IntConversionInputKind::NumbersOrBoolsOnly;
            }
            replace = MToNumberInt32::New(alloc, in, convert);
            break;
          }
          case MIRType::Object:
            replace = MUnbox::New(alloc, in, MIRType::Object, MUnbox::Infallible);
            break;
          case MIRType::String:
            replace = MUnbox::New(alloc, in, MIRType::String, MUnbox::Infallible);
            break;
          case MIRType::Symbol:
            replace = MUnbox::New(alloc, in, MIRType::Symbol, MUnbox::Infallible);
            break;
          default:
            MOZ_CRASH("Unknown compare specialization");
        }

        def->block()->insertBefore(def, replace);
        def->replaceOperand(i, replace);

        if (!replace->typePolicy()->adjustInputs(alloc, replace))
            return false;
    }

    return true;
}

bool
SameValuePolicy::adjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MOZ_ASSERT(def->isSameValue());
    MSameValue* sameValue = def->toSameValue();
    MIRType lhsType = sameValue->lhs()->type();
    MIRType rhsType = sameValue->rhs()->type();

    // If both operands are numbers, convert them to doubles.
    if (IsNumberType(lhsType) && IsNumberType(rhsType))
        return AllDoublePolicy::staticAdjustInputs(alloc, def);

    // SameValue(Anything, Double) is specialized, so convert the rhs if it's
    // not already a double.
    if (lhsType == MIRType::Value && IsNumberType(rhsType)) {
        if (rhsType != MIRType::Double) {
            MInstruction* replace = MToDouble::New(alloc, sameValue->rhs());
            def->block()->insertBefore(def, replace);
            def->replaceOperand(1, replace);

            if (!replace->typePolicy()->adjustInputs(alloc, replace))
                return false;
        }

        return true;
    }

    // Otherwise box both operands.
    return BoxInputsPolicy::staticAdjustInputs(alloc, def);
}

bool
TypeBarrierPolicy::adjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MTypeBarrier* ins = def->toTypeBarrier();
    MIRType inputType = ins->getOperand(0)->type();
    MIRType outputType = ins->type();

    // Input and output type are already in accordance.
    if (inputType == outputType)
        return true;

    // Output is a value, currently box the input.
    if (outputType == MIRType::Value) {
        // XXX: Possible optimization: decrease resultTypeSet to only include
        // the inputType. This will remove the need for boxing.
        MOZ_ASSERT(inputType != MIRType::Value);
        ins->replaceOperand(0, BoxAt(alloc, ins, ins->getOperand(0)));
        return true;
    }

    // Box input if needed.
    if (inputType != MIRType::Value) {
        MOZ_ASSERT(ins->alwaysBails());
        ins->replaceOperand(0, BoxAt(alloc, ins, ins->getOperand(0)));
    }

    // We can't unbox a value to null/undefined/lazyargs. So keep output
    // also a value.
    // Note: Using setResultType shouldn't be done in TypePolicies,
    //       Here it is fine, since the type barrier has no uses.
    if (IsNullOrUndefined(outputType) || outputType == MIRType::MagicOptimizedArguments) {
        MOZ_ASSERT(!ins->hasDefUses());
        ins->setResultType(MIRType::Value);
        return true;
    }

    // Unbox / propagate the right type.
    MUnbox::Mode mode = MUnbox::TypeBarrier;
    MInstruction* replace = MUnbox::New(alloc, ins->getOperand(0), ins->type(), mode);
    if (!ins->isMovable())
        replace->setNotMovable();

    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(0, replace);
    if (!replace->typePolicy()->adjustInputs(alloc, replace))
        return false;

    // The TypeBarrier is equivalent to removing branches with unexpected
    // types.  The unexpected types would have changed Range Analysis
    // predictions.  As such, we need to prevent destructive optimizations.
    ins->block()->flagOperandsOfPrunedBranches(replace);

    return true;
}

bool
TestPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MDefinition* op = ins->getOperand(0);
    switch (op->type()) {
      case MIRType::Value:
      case MIRType::Null:
      case MIRType::Undefined:
      case MIRType::Boolean:
      case MIRType::Int32:
      case MIRType::Double:
      case MIRType::Float32:
      case MIRType::Symbol:
      case MIRType::Object:
        break;

      case MIRType::String:
      {
        MStringLength* length = MStringLength::New(alloc, op);
        ins->block()->insertBefore(ins, length);
        ins->replaceOperand(0, length);
        break;
      }

      default:
        ins->replaceOperand(0, BoxAt(alloc, ins, op));
        break;
    }
    return true;
}

bool
BitwisePolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MIRType specialization = ins->typePolicySpecialization();
    if (specialization == MIRType::None)
        return BoxInputsPolicy::staticAdjustInputs(alloc, ins);

    MOZ_ASSERT(ins->type() == specialization);
    MOZ_ASSERT(specialization == MIRType::Int32 || specialization == MIRType::Double);

    // This policy works for both unary and binary bitwise operations.
    for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
        MDefinition* in = ins->getOperand(i);
        if (in->type() == MIRType::Int32)
            continue;

        MInstruction* replace = MTruncateToInt32::New(alloc, in);
        ins->block()->insertBefore(ins, replace);
        ins->replaceOperand(i, replace);

        if (!replace->typePolicy()->adjustInputs(alloc, replace))
            return false;
    }

    return true;
}

bool
PowPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MIRType specialization = ins->typePolicySpecialization();
    MOZ_ASSERT(specialization == MIRType::Int32 ||
               specialization == MIRType::Double ||
               specialization == MIRType::None);

    // Inputs will be boxed if either is non-numeric.
    if (specialization == MIRType::None)
        return BoxInputsPolicy::staticAdjustInputs(alloc, ins);

    // Otherwise, input must be a double.
    if (!DoublePolicy<0>::staticAdjustInputs(alloc, ins))
        return false;

    // Power may be an int32 or a double. Integers receive a faster path.
    if (specialization == MIRType::Double)
        return DoublePolicy<1>::staticAdjustInputs(alloc, ins);
    return UnboxedInt32Policy<1>::staticAdjustInputs(alloc, ins);
}

template <unsigned Op>
bool
StringPolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MDefinition* in = ins->getOperand(Op);
    if (in->type() == MIRType::String)
        return true;

    MUnbox* replace = MUnbox::New(alloc, in, MIRType::String, MUnbox::Fallible);
    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool StringPolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool StringPolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool StringPolicy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);

template <unsigned Op>
bool
ConvertToStringPolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MDefinition* in = ins->getOperand(Op);
    if (in->type() == MIRType::String)
        return true;

    MToString* replace = MToString::New(alloc, in);
    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(Op, replace);

    if (!ToStringPolicy::staticAdjustInputs(alloc, replace))
        return false;

    return true;
}

template bool ConvertToStringPolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool ConvertToStringPolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool ConvertToStringPolicy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);

template <unsigned Op>
bool
BooleanPolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MDefinition* in = def->getOperand(Op);
    if (in->type() == MIRType::Boolean)
        return true;

    MUnbox* replace = MUnbox::New(alloc, in, MIRType::Boolean, MUnbox::Fallible);
    def->block()->insertBefore(def, replace);
    def->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool BooleanPolicy<3>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
UnboxedInt32Policy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MDefinition* in = def->getOperand(Op);
    if (in->type() == MIRType::Int32)
        return true;

    MUnbox* replace = MUnbox::New(alloc, in, MIRType::Int32, MUnbox::Fallible);
    def->block()->insertBefore(def, replace);
    def->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool UnboxedInt32Policy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool UnboxedInt32Policy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool UnboxedInt32Policy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool UnboxedInt32Policy<3>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
ConvertToInt32Policy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MDefinition* in = def->getOperand(Op);
    if (in->type() == MIRType::Int32)
        return true;

    auto* replace = MToNumberInt32::New(alloc, in);
    def->block()->insertBefore(def, replace);
    def->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool ConvertToInt32Policy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
TruncateToInt32Policy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MDefinition* in = def->getOperand(Op);
    if (in->type() == MIRType::Int32)
        return true;

    MTruncateToInt32* replace = MTruncateToInt32::New(alloc, in);
    def->block()->insertBefore(def, replace);
    def->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool TruncateToInt32Policy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool TruncateToInt32Policy<3>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
DoublePolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MDefinition* in = def->getOperand(Op);
    if (in->type() == MIRType::Double || in->type() == MIRType::SinCosDouble)
        return true;

    MToDouble* replace = MToDouble::New(alloc, in);
    def->block()->insertBefore(def, replace);
    def->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool DoublePolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool DoublePolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
Float32Policy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MDefinition* in = def->getOperand(Op);
    if (in->type() == MIRType::Float32)
        return true;

    MToFloat32* replace = MToFloat32::New(alloc, in);
    def->block()->insertBefore(def, replace);
    def->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool Float32Policy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool Float32Policy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool Float32Policy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
FloatingPointPolicy<Op>::adjustInputs(TempAllocator& alloc, MInstruction* def)
{
    MIRType policyType = def->typePolicySpecialization();
    if (policyType == MIRType::Double)
        return DoublePolicy<Op>::staticAdjustInputs(alloc, def);
    return Float32Policy<Op>::staticAdjustInputs(alloc, def);
}

template bool FloatingPointPolicy<0>::adjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
NoFloatPolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def)
{
    EnsureOperandNotFloat32(alloc, def, Op);
    return true;
}

template bool NoFloatPolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool NoFloatPolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool NoFloatPolicy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool NoFloatPolicy<3>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned FirstOp>
bool
NoFloatPolicyAfter<FirstOp>::adjustInputs(TempAllocator& alloc, MInstruction* def)
{
    for (size_t op = FirstOp, e = def->numOperands(); op < e; op++)
        EnsureOperandNotFloat32(alloc, def, op);
    return true;
}

template bool NoFloatPolicyAfter<0>::adjustInputs(TempAllocator& alloc, MInstruction* def);
template bool NoFloatPolicyAfter<1>::adjustInputs(TempAllocator& alloc, MInstruction* def);
template bool NoFloatPolicyAfter<2>::adjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
SimdScalarPolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));
    MIRType laneType = SimdTypeToLaneType(ins->type());

    MDefinition* in = ins->getOperand(Op);

    // A vector with boolean lanes requires Int32 inputs that have already been
    // converted to 0/-1.
    // We can't insert a MIRType::Boolean lane directly - it requires conversion.
    if (laneType == MIRType::Boolean) {
        MOZ_ASSERT(in->type() == MIRType::Int32, "Boolean SIMD vector requires Int32 lanes.");
        return true;
    }

    if (in->type() == laneType)
        return true;

    MInstruction* replace;
    if (laneType == MIRType::Int32) {
        replace = MTruncateToInt32::New(alloc, in);
    } else {
        MOZ_ASSERT(laneType == MIRType::Float32);
        replace = MToFloat32::New(alloc, in);
    }

    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool SimdScalarPolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool SimdScalarPolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool SimdScalarPolicy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);
template bool SimdScalarPolicy<3>::staticAdjustInputs(TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool
BoxPolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MDefinition* in = ins->getOperand(Op);
    if (in->type() == MIRType::Value)
        return true;

    ins->replaceOperand(Op, BoxAt(alloc, ins, in));
    return true;
}

template bool BoxPolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool BoxPolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool BoxPolicy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);

template <unsigned Op, MIRType Type>
bool
BoxExceptPolicy<Op, Type>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MDefinition* in = ins->getOperand(Op);
    if (in->type() == Type)
        return true;
    return BoxPolicy<Op>::staticAdjustInputs(alloc, ins);
}

template bool BoxExceptPolicy<0, MIRType::Object>::staticAdjustInputs(TempAllocator& alloc,
                                                                     MInstruction* ins);

template <unsigned Op>
bool
CacheIdPolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MDefinition* in = ins->getOperand(Op);
    switch (in->type()) {
      case MIRType::Int32:
      case MIRType::String:
      case MIRType::Symbol:
        return true;
      default:
        return BoxPolicy<Op>::staticAdjustInputs(alloc, ins);
    }
}

template bool CacheIdPolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool CacheIdPolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);

bool
ToDoublePolicy::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MOZ_ASSERT(ins->isToDouble() || ins->isToFloat32());

    MDefinition* in = ins->getOperand(0);
    MToFPInstruction::ConversionKind conversion;
    if (ins->isToDouble())
        conversion = ins->toToDouble()->conversion();
    else
        conversion = ins->toToFloat32()->conversion();

    switch (in->type()) {
      case MIRType::Int32:
      case MIRType::Float32:
      case MIRType::Double:
      case MIRType::Value:
        // No need for boxing for these types.
        return true;
      case MIRType::Null:
        // No need for boxing, when we will convert.
        if (conversion == MToFPInstruction::NonStringPrimitives)
            return true;
        break;
      case MIRType::Undefined:
      case MIRType::Boolean:
        // No need for boxing, when we will convert.
        if (conversion == MToFPInstruction::NonStringPrimitives)
            return true;
        if (conversion == MToFPInstruction::NonNullNonStringPrimitives)
            return true;
        break;
      case MIRType::Object:
      case MIRType::String:
      case MIRType::Symbol:
        // Objects might be effectful. Symbols give TypeError.
        break;
      default:
        break;
    }

    in = BoxAt(alloc, ins, in);
    ins->replaceOperand(0, in);
    return true;
}

bool
ToInt32Policy::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MOZ_ASSERT(ins->isToNumberInt32() || ins->isTruncateToInt32());

    IntConversionInputKind conversion = IntConversionInputKind::Any;
    if (ins->isToNumberInt32())
        conversion = ins->toToNumberInt32()->conversion();

    MDefinition* in = ins->getOperand(0);
    switch (in->type()) {
      case MIRType::Int32:
      case MIRType::Float32:
      case MIRType::Double:
      case MIRType::Value:
        // No need for boxing for these types.
        return true;
      case MIRType::Undefined:
        // No need for boxing when truncating.
        if (ins->isTruncateToInt32())
            return true;
        break;
      case MIRType::Null:
        // No need for boxing, when we will convert.
        if (conversion == IntConversionInputKind::Any)
            return true;
        break;
      case MIRType::Boolean:
        // No need for boxing, when we will convert.
        if (conversion == IntConversionInputKind::Any)
            return true;
        if (conversion == IntConversionInputKind::NumbersOrBoolsOnly)
            return true;
        break;
      case MIRType::Object:
      case MIRType::String:
      case MIRType::Symbol:
        // Objects might be effectful. Symbols give TypeError.
        break;
      default:
        break;
    }

    in = BoxAt(alloc, ins, in);
    ins->replaceOperand(0, in);
    return true;
}

bool
ToStringPolicy::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MOZ_ASSERT(ins->isToString());

    MIRType type = ins->getOperand(0)->type();
    if (type == MIRType::Object || type == MIRType::Symbol) {
        ins->replaceOperand(0, BoxAt(alloc, ins, ins->getOperand(0)));
        return true;
    }

    // TODO remove the following line once 966957 has landed
    EnsureOperandNotFloat32(alloc, ins, 0);

    return true;
}

template <unsigned Op>
bool
ObjectPolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MDefinition* in = ins->getOperand(Op);
    if (in->type() == MIRType::Object || in->type() == MIRType::Slots ||
        in->type() == MIRType::Elements)
    {
        return true;
    }

    MUnbox* replace = MUnbox::New(alloc, in, MIRType::Object, MUnbox::Fallible);
    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(Op, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
}

template bool ObjectPolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool ObjectPolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool ObjectPolicy<2>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool ObjectPolicy<3>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);

template <unsigned Op>
bool
SimdSameAsReturnedTypePolicy<Op>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MOZ_ASSERT(ins->type() == ins->getOperand(Op)->type());
    return true;
}

template bool
SimdSameAsReturnedTypePolicy<0>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);
template bool
SimdSameAsReturnedTypePolicy<1>::staticAdjustInputs(TempAllocator& alloc, MInstruction* ins);

bool
SimdAllPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    for (unsigned i = 0, e = ins->numOperands(); i < e; i++)
        MOZ_ASSERT(ins->getOperand(i)->type() == ins->typePolicySpecialization());
    return true;
}

template <unsigned Op>
bool
SimdPolicy<Op>::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MOZ_ASSERT(ins->typePolicySpecialization() == ins->getOperand(Op)->type());
    return true;
}

template bool
SimdPolicy<0>::adjustInputs(TempAllocator& alloc, MInstruction* ins);

bool
SimdShufflePolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MSimdGeneralShuffle* s = ins->toSimdGeneralShuffle();

    for (unsigned i = 0; i < s->numVectors(); i++)
        MOZ_ASSERT(ins->getOperand(i)->type() == ins->typePolicySpecialization());

    // Next inputs are the lanes, which need to be int32
    for (unsigned i = 0; i < s->numLanes(); i++) {
        MDefinition* in = ins->getOperand(s->numVectors() + i);
        if (in->type() == MIRType::Int32)
            continue;

        auto* replace = MToNumberInt32::New(alloc, in, IntConversionInputKind::NumbersOnly);
        ins->block()->insertBefore(ins, replace);
        ins->replaceOperand(s->numVectors() + i, replace);
        if (!replace->typePolicy()->adjustInputs(alloc, replace))
            return false;
    }

    return true;
}

bool
SimdSelectPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    // First input is the mask, which has to be a boolean.
    MOZ_ASSERT(IsBooleanSimdType(ins->getOperand(0)->type()));

    // Next inputs are the two vectors of a particular type.
    for (unsigned i = 1; i < 3; i++)
        MOZ_ASSERT(ins->getOperand(i)->type() == ins->typePolicySpecialization());

    return true;
}

bool
CallPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MCall* call = ins->toCall();

    MDefinition* func = call->getFunction();
    if (func->type() != MIRType::Object) {
        MInstruction* unbox = MUnbox::New(alloc, func, MIRType::Object, MUnbox::Fallible);
        call->block()->insertBefore(call, unbox);
        call->replaceFunction(unbox);

        if (!unbox->typePolicy()->adjustInputs(alloc, unbox))
            return false;
    }

    for (uint32_t i = 0; i < call->numStackArgs(); i++) {
        if (!alloc.ensureBallast())
            return false;
        EnsureOperandNotFloat32(alloc, call, MCall::IndexOfStackArg(i));
    }

    return true;
}

bool
CallSetElementPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    // The first operand should be an object.
    if (!SingleObjectPolicy::staticAdjustInputs(alloc, ins))
        return false;

    // Box the index and value operands.
    for (size_t i = 1, e = ins->numOperands(); i < e; i++) {
        MDefinition* in = ins->getOperand(i);
        if (in->type() == MIRType::Value)
            continue;
        ins->replaceOperand(i, BoxAt(alloc, ins, in));
    }
    return true;
}

bool
InstanceOfPolicy::adjustInputs(TempAllocator& alloc, MInstruction* def)
{
    // Box first operand if it isn't object
    if (def->getOperand(0)->type() != MIRType::Object)
        if (!BoxPolicy<0>::staticAdjustInputs(alloc, def))
            return false;

    return true;
}

bool
StoreUnboxedScalarPolicy::adjustValueInput(TempAllocator& alloc, MInstruction* ins,
                                           Scalar::Type writeType, MDefinition* value,
                                           int valueOperand)
{
    // Storing a SIMD value requires a valueOperand that has already been
    // SimdUnboxed. See IonBuilder::inlineSimdStore(()
    if (Scalar::isSimdType(writeType)) {
        MOZ_ASSERT(IsSimdType(value->type()));
        return true;
    }

    MDefinition* curValue = value;
    // First, ensure the value is int32, boolean, double or Value.
    // The conversion is based on TypedArrayObjectTemplate::setElementTail.
    switch (value->type()) {
      case MIRType::Int32:
      case MIRType::Double:
      case MIRType::Float32:
      case MIRType::Boolean:
      case MIRType::Value:
        break;
      case MIRType::Null:
        value->setImplicitlyUsedUnchecked();
        value = MConstant::New(alloc, Int32Value(0));
        ins->block()->insertBefore(ins, value->toInstruction());
        break;
      case MIRType::Undefined:
        value->setImplicitlyUsedUnchecked();
        value = MConstant::New(alloc, DoubleNaNValue());
        ins->block()->insertBefore(ins, value->toInstruction());
        break;
      case MIRType::Object:
      case MIRType::String:
      case MIRType::Symbol:
        value = BoxAt(alloc, ins, value);
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }

    if (value != curValue) {
        ins->replaceOperand(valueOperand, value);
        curValue = value;
    }

    MOZ_ASSERT(value->type() == MIRType::Int32 ||
               value->type() == MIRType::Boolean ||
               value->type() == MIRType::Double ||
               value->type() == MIRType::Float32 ||
               value->type() == MIRType::Value);

    switch (writeType) {
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Int16:
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
        if (value->type() != MIRType::Int32) {
            value = MTruncateToInt32::New(alloc, value);
            ins->block()->insertBefore(ins, value->toInstruction());
        }
        break;
      case Scalar::Uint8Clamped:
        // IonBuilder should have inserted ClampToUint8.
        MOZ_ASSERT(value->type() == MIRType::Int32);
        break;
      case Scalar::Float32:
        if (value->type() != MIRType::Float32) {
            value = MToFloat32::New(alloc, value);
            ins->block()->insertBefore(ins, value->toInstruction());
        }
        break;
      case Scalar::Float64:
        if (value->type() != MIRType::Double) {
            value = MToDouble::New(alloc, value);
            ins->block()->insertBefore(ins, value->toInstruction());
        }
        break;
      default:
        MOZ_CRASH("Invalid array type");
    }

    if (value != curValue)
        ins->replaceOperand(valueOperand, value);

    return true;
}

bool
StoreUnboxedScalarPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    if (!SingleObjectPolicy::staticAdjustInputs(alloc, ins))
        return false;

    MStoreUnboxedScalar* store = ins->toStoreUnboxedScalar();
    MOZ_ASSERT(IsValidElementsType(store->elements(), store->offsetAdjustment()));
    MOZ_ASSERT(store->index()->type() == MIRType::Int32);

    return adjustValueInput(alloc, store, store->writeType(), store->value(), 2);
}

bool
StoreTypedArrayHolePolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MStoreTypedArrayElementHole* store = ins->toStoreTypedArrayElementHole();
    MOZ_ASSERT(store->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(store->index()->type() == MIRType::Int32);
    MOZ_ASSERT(store->length()->type() == MIRType::Int32);

    return StoreUnboxedScalarPolicy::adjustValueInput(alloc, ins, store->arrayType(), store->value(), 3);
}

bool
StoreUnboxedObjectOrNullPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    if (!ObjectPolicy<0>::staticAdjustInputs(alloc, ins))
        return false;

    if (!ObjectPolicy<3>::staticAdjustInputs(alloc, ins))
        return false;

    // Change the value input to a ToObjectOrNull instruction if it might be
    // a non-null primitive. Insert a post barrier for the instruction's object
    // and whatever its new value is, unless the value is definitely null.
    MStoreUnboxedObjectOrNull* store = ins->toStoreUnboxedObjectOrNull();

    MOZ_ASSERT(store->typedObj()->type() == MIRType::Object);

    MDefinition* value = store->value();
    if (value->type() == MIRType::Object ||
        value->type() == MIRType::Null ||
        value->type() == MIRType::ObjectOrNull)
    {
        if (value->type() != MIRType::Null) {
            MInstruction* barrier = MPostWriteBarrier::New(alloc, store->typedObj(), value);
            store->block()->insertBefore(store, barrier);
        }
        return true;
    }

    MToObjectOrNull* replace = MToObjectOrNull::New(alloc, value);
    store->block()->insertBefore(store, replace);
    store->setValue(replace);

    if (!BoxPolicy<0>::staticAdjustInputs(alloc, replace))
        return false;

    MInstruction* barrier = MPostWriteBarrier::New(alloc, store->typedObj(), replace);
    store->block()->insertBefore(store, barrier);

    return true;
}

bool
StoreUnboxedStringPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    if (!ObjectPolicy<0>::staticAdjustInputs(alloc, ins))
        return false;

    // Change the value input to a ToString instruction if it might be
    // a non-null primitive.
    if (!ConvertToStringPolicy<2>::staticAdjustInputs(alloc, ins))
        return false;

    if (!ObjectPolicy<3>::staticAdjustInputs(alloc, ins))
        return false;

    // Insert a post barrier for the instruction's object and whatever its new
    // value is.
    MStoreUnboxedString* store = ins->toStoreUnboxedString();

    MOZ_ASSERT(store->typedObj()->type() == MIRType::Object);

    MDefinition* value = store->value();
    MOZ_ASSERT(value->type() == MIRType::String);
    MInstruction* barrier = MPostWriteBarrier::New(alloc, store->typedObj(), value);
    store->block()->insertBefore(store, barrier);
    return true;
}

bool
ClampPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MDefinition* in = ins->toClampToUint8()->input();

    switch (in->type()) {
      case MIRType::Int32:
      case MIRType::Double:
      case MIRType::Value:
        break;
      default:
          ins->replaceOperand(0, BoxAt(alloc, ins, in));
        break;
    }

    return true;
}

bool
FilterTypeSetPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins)
{
    MOZ_ASSERT(ins->numOperands() == 1);
    MIRType inputType = ins->getOperand(0)->type();
    MIRType outputType = ins->type();

    // Special case when output is a Float32, but input isn't.
    if (outputType == MIRType::Float32 && inputType != MIRType::Float32) {
        // Create a MToFloat32 to add between the MFilterTypeSet and
        // its uses.
        MInstruction* replace = MToFloat32::New(alloc, ins);
        ins->justReplaceAllUsesWithExcept(replace);
        ins->block()->insertAfter(ins, replace);

        // Reset the type to not MIRType::Float32
        // Note: setResultType shouldn't happen in TypePolicies,
        //       Here it is fine, since there is just one use we just
        //       added ourself. And the resulting type after MToFloat32
        //       equals the original type.
        ins->setResultType(ins->resultTypeSet()->getKnownMIRType());
        outputType = ins->type();

        // Do the type analysis
        if (!replace->typePolicy()->adjustInputs(alloc, replace))
            return false;

        // Fall through to let the MFilterTypeSet adjust its input based
        // on its new type.
    }

    // Input and output type are already in accordance.
    if (inputType == outputType)
        return true;

    // Output is a value, box the input.
    if (outputType == MIRType::Value) {
        MOZ_ASSERT(inputType != MIRType::Value);
        ins->replaceOperand(0, BoxAt(alloc, ins, ins->getOperand(0)));
        return true;
    }

    // The outputType should be a subset of the inputType else we are in code
    // that has never executed yet. Bail to see the new type (if that hasn't
    // happened yet).
    if (inputType != MIRType::Value) {
        MBail* bail = MBail::New(alloc);
        ins->block()->insertBefore(ins, bail);
        bail->setDependency(ins->dependency());
        ins->setDependency(bail);
        ins->replaceOperand(0, BoxAt(alloc, ins, ins->getOperand(0)));
    }

    // We can't unbox a value to null/undefined/lazyargs. So keep output
    // also a value.
    // Note: Using setResultType shouldn't be done in TypePolicies,
    //       Here it is fine, since the type barrier has no uses.
    if (IsNullOrUndefined(outputType) || outputType == MIRType::MagicOptimizedArguments) {
        MOZ_ASSERT(!ins->hasDefUses());
        ins->setResultType(MIRType::Value);
        return true;
    }

    // Unbox / propagate the right type.
    MUnbox::Mode mode = MUnbox::Infallible;
    MInstruction* replace = MUnbox::New(alloc, ins->getOperand(0), ins->type(), mode);

    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(0, replace);
    if (!replace->typePolicy()->adjustInputs(alloc, replace))
        return false;

    // Carry over the dependency the MFilterTypeSet had.
    replace->setDependency(ins->dependency());

    return true;
}

// Lists of all TypePolicy specializations which are used by MIR Instructions.
#define TYPE_POLICY_LIST(_)                     \
    _(ArithPolicy)                              \
    _(BitwisePolicy)                            \
    _(BoxInputsPolicy)                          \
    _(CallPolicy)                               \
    _(CallSetElementPolicy)                     \
    _(ClampPolicy)                              \
    _(ComparePolicy)                            \
    _(FilterTypeSetPolicy)                      \
    _(InstanceOfPolicy)                         \
    _(PowPolicy)                                \
    _(SameValuePolicy)                          \
    _(SimdAllPolicy)                            \
    _(SimdSelectPolicy)                         \
    _(SimdShufflePolicy)                        \
    _(StoreTypedArrayHolePolicy)                \
    _(StoreUnboxedScalarPolicy)                 \
    _(StoreUnboxedObjectOrNullPolicy)           \
    _(StoreUnboxedStringPolicy)                 \
    _(TestPolicy)                               \
    _(AllDoublePolicy)                          \
    _(ToDoublePolicy)                           \
    _(ToInt32Policy)                            \
    _(ToStringPolicy)                           \
    _(TypeBarrierPolicy)

#define TEMPLATE_TYPE_POLICY_LIST(_)                                                                          \
    _(BoxExceptPolicy<0, MIRType::Object>)                                                                    \
    _(BoxPolicy<0>)                                                                                           \
    _(ConvertToInt32Policy<0>)                                                                                \
    _(ConvertToStringPolicy<0>)                                                                               \
    _(ConvertToStringPolicy<2>)                                                                               \
    _(DoublePolicy<0>)                                                                                        \
    _(FloatingPointPolicy<0>)                                                                                 \
    _(UnboxedInt32Policy<0>)                                                                                  \
    _(UnboxedInt32Policy<1>)                                                                                  \
    _(MixPolicy<ObjectPolicy<0>, StringPolicy<1>, BoxPolicy<2> >)                                             \
    _(MixPolicy<ObjectPolicy<0>, BoxPolicy<1>, BoxPolicy<2> >)                                                \
    _(MixPolicy<ObjectPolicy<0>, BoxPolicy<1>, ObjectPolicy<2> >)                                             \
    _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>, BoxPolicy<2> >)                                       \
    _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>, UnboxedInt32Policy<2> >)                              \
    _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>, TruncateToInt32Policy<2> >)                           \
    _(MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>, BoxPolicy<2> >)                                             \
    _(MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>, UnboxedInt32Policy<2> >)                                    \
    _(MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>, ObjectPolicy<2> >)                                          \
    _(MixPolicy<StringPolicy<0>, UnboxedInt32Policy<1>, UnboxedInt32Policy<2>>)                               \
    _(MixPolicy<StringPolicy<0>, ObjectPolicy<1>, StringPolicy<2> >)                                          \
    _(MixPolicy<StringPolicy<0>, StringPolicy<1>, StringPolicy<2> >)                                          \
    _(MixPolicy<ObjectPolicy<0>, StringPolicy<1>, UnboxedInt32Policy<2>>)                                     \
    _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>, UnboxedInt32Policy<2>, UnboxedInt32Policy<3>>)        \
    _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>, TruncateToInt32Policy<2>, TruncateToInt32Policy<3> >) \
    _(MixPolicy<ObjectPolicy<0>, CacheIdPolicy<1>, NoFloatPolicy<2>>)                                         \
    _(MixPolicy<SimdScalarPolicy<0>, SimdScalarPolicy<1>, SimdScalarPolicy<2>, SimdScalarPolicy<3> >)         \
    _(MixPolicy<ObjectPolicy<0>, BoxExceptPolicy<1, MIRType::Object>, CacheIdPolicy<2>>)                      \
    _(MixPolicy<BoxPolicy<0>, ObjectPolicy<1> >)                                                              \
    _(MixPolicy<ConvertToStringPolicy<0>, ConvertToStringPolicy<1> >)                                         \
    _(MixPolicy<ConvertToStringPolicy<0>, ObjectPolicy<1> >)                                                  \
    _(MixPolicy<DoublePolicy<0>, DoublePolicy<1> >)                                                           \
    _(MixPolicy<UnboxedInt32Policy<0>, UnboxedInt32Policy<1> >)                                               \
    _(MixPolicy<ObjectPolicy<0>, BoxPolicy<1> >)                                                              \
    _(MixPolicy<BoxExceptPolicy<0, MIRType::Object>, CacheIdPolicy<1>>)                                       \
    _(MixPolicy<CacheIdPolicy<0>, ObjectPolicy<1> >)                                                          \
    _(MixPolicy<ObjectPolicy<0>, ConvertToStringPolicy<1> >)                                                  \
    _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1> >)                                                     \
    _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<2> >)                                                     \
    _(MixPolicy<ObjectPolicy<0>, NoFloatPolicy<1> >)                                                          \
    _(MixPolicy<ObjectPolicy<0>, NoFloatPolicy<2> >)                                                          \
    _(MixPolicy<ObjectPolicy<0>, NoFloatPolicy<3> >)                                                          \
    _(MixPolicy<ObjectPolicy<0>, ObjectPolicy<1> >)                                                           \
    _(MixPolicy<ObjectPolicy<0>, StringPolicy<1> >)                                                           \
    _(MixPolicy<ObjectPolicy<0>, ConvertToStringPolicy<2> >)                                                  \
    _(MixPolicy<ObjectPolicy<1>, ConvertToStringPolicy<0> >)                                                  \
    _(MixPolicy<SimdSameAsReturnedTypePolicy<0>, SimdSameAsReturnedTypePolicy<1> >)                           \
    _(MixPolicy<SimdSameAsReturnedTypePolicy<0>, SimdScalarPolicy<1> >)                                       \
    _(MixPolicy<StringPolicy<0>, UnboxedInt32Policy<1> >)                                                     \
    _(MixPolicy<StringPolicy<0>, StringPolicy<1> >)                                                           \
    _(MixPolicy<BoxPolicy<0>, BoxPolicy<1> >)                                                                 \
    _(NoFloatPolicy<0>)                                                                                       \
    _(NoFloatPolicyAfter<0>)                                                                                  \
    _(NoFloatPolicyAfter<1>)                                                                                  \
    _(NoFloatPolicyAfter<2>)                                                                                  \
    _(ObjectPolicy<0>)                                                                                        \
    _(ObjectPolicy<1>)                                                                                        \
    _(ObjectPolicy<3>)                                                                                        \
    _(SimdPolicy<0>)                                                                                          \
    _(SimdSameAsReturnedTypePolicy<0>)                                                                        \
    _(SimdScalarPolicy<0>)                                                                                    \
    _(StringPolicy<0>)


namespace js {
namespace jit {

// Define for all used TypePolicy specialization, the definition for
// |TypePolicy::Data::thisTypePolicy|.  This function returns one constant
// instance of the TypePolicy which is shared among all MIR Instructions of the
// same type.
//
// This Macro use __VA_ARGS__ to account for commas of template parameters.
#define DEFINE_TYPE_POLICY_SINGLETON_INSTANCES_(...)    \
    TypePolicy *                                        \
    __VA_ARGS__::Data::thisTypePolicy()                 \
    {                                                   \
        static __VA_ARGS__ singletonType;               \
        return &singletonType;                          \
    }

    TYPE_POLICY_LIST(DEFINE_TYPE_POLICY_SINGLETON_INSTANCES_)
    TEMPLATE_TYPE_POLICY_LIST(template<> DEFINE_TYPE_POLICY_SINGLETON_INSTANCES_)
#undef DEFINE_TYPE_POLICY_SINGLETON_INSTANCES_

} // namespace jit
} // namespace js

namespace {

// For extra-good measure in case an unqualified use is ever introduced.  (The
// main use in the macro below is explicitly qualified so as not to consult
// this scope and find this function.)
inline TypePolicy*
thisTypePolicy() = delete;

static MIRType
thisTypeSpecialization()
{
    MOZ_CRASH("TypeSpecialization lacks definition of thisTypeSpecialization.");
}

} // namespace

// For each MIR Instruction, this macro define the |typePolicy| method which is
// using the |thisTypePolicy| method.  The |thisTypePolicy| method is either a
// member of the MIR Instruction, such as with MGetElementCache, a member
// inherited from the TypePolicy::Data structure, or a member inherited from
// NoTypePolicy if the MIR instruction has no type policy.
#define DEFINE_MIR_TYPEPOLICY_MEMBERS_(op)      \
    TypePolicy *                                \
    js::jit::M##op::typePolicy()                \
    {                                           \
        return M##op::thisTypePolicy();         \
    }                                           \
                                                \
    MIRType                                     \
    js::jit::M##op::typePolicySpecialization()  \
    {                                           \
        return thisTypeSpecialization();        \
    }

    MIR_OPCODE_LIST(DEFINE_MIR_TYPEPOLICY_MEMBERS_)
#undef DEFINE_MIR_TYPEPOLICY_MEMBERS_
