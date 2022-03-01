/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/TypePolicy.h"

#include "jit/JitAllocPolicy.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "js/ScalarType.h"  // js::Scalar::Type

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

static void EnsureOperandNotFloat32(TempAllocator& alloc, MInstruction* def,
                                    unsigned op) {
  MDefinition* in = def->getOperand(op);
  if (in->type() == MIRType::Float32) {
    MToDouble* replace = MToDouble::New(alloc, in);
    def->block()->insertBefore(def, replace);
    if (def->isRecoveredOnBailout()) {
      replace->setRecoveredOnBailout();
    }
    def->replaceOperand(op, replace);
  }
}

template <class T>
[[nodiscard]] static bool ConvertOperand(TempAllocator& alloc,
                                         MInstruction* def, unsigned op,
                                         MIRType expected) {
  MDefinition* in = def->getOperand(op);
  if (in->type() == expected) {
    return true;
  }

  auto* replace = T::New(alloc, in);
  def->block()->insertBefore(def, replace);
  def->replaceOperand(op, replace);

  return replace->typePolicy()->adjustInputs(alloc, replace);
}

[[nodiscard]] static bool UnboxOperand(TempAllocator& alloc, MInstruction* def,
                                       unsigned op, MIRType expected) {
  MDefinition* in = def->getOperand(op);
  if (in->type() == expected) {
    return true;
  }

  auto* replace = MUnbox::New(alloc, in, expected, MUnbox::Fallible);
  replace->setBailoutKind(BailoutKind::TypePolicy);
  def->block()->insertBefore(def, replace);
  def->replaceOperand(op, replace);

  return replace->typePolicy()->adjustInputs(alloc, replace);
}

MDefinition* js::jit::AlwaysBoxAt(TempAllocator& alloc, MInstruction* at,
                                  MDefinition* operand) {
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

static MDefinition* BoxAt(TempAllocator& alloc, MInstruction* at,
                          MDefinition* operand) {
  if (operand->isUnbox()) {
    return operand->toUnbox()->input();
  }
  return AlwaysBoxAt(alloc, at, operand);
}

bool BoxInputsPolicy::staticAdjustInputs(TempAllocator& alloc,
                                         MInstruction* ins) {
  for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
    MDefinition* in = ins->getOperand(i);
    if (in->type() == MIRType::Value) {
      continue;
    }
    ins->replaceOperand(i, BoxAt(alloc, ins, in));
  }
  return true;
}

bool ArithPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins) const {
  MOZ_ASSERT(IsNumberType(ins->type()));
  MOZ_ASSERT(ins->type() == MIRType::Double || ins->type() == MIRType::Int32 ||
             ins->type() == MIRType::Float32);

  for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
    MDefinition* in = ins->getOperand(i);
    if (in->type() == ins->type()) {
      continue;
    }

    MInstruction* replace;

    if (ins->type() == MIRType::Double) {
      replace = MToDouble::New(alloc, in);
    } else if (ins->type() == MIRType::Float32) {
      replace = MToFloat32::New(alloc, in);
    } else {
      replace = MToNumberInt32::New(alloc, in);
    }

    replace->setBailoutKind(BailoutKind::TypePolicy);
    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(i, replace);

    if (!replace->typePolicy()->adjustInputs(alloc, replace)) {
      return false;
    }
  }

  return true;
}

bool BigIntArithPolicy::adjustInputs(TempAllocator& alloc,
                                     MInstruction* ins) const {
  MOZ_ASSERT(ins->type() == MIRType::BigInt);

  for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
    if (!ConvertOperand<MToBigInt>(alloc, ins, i, MIRType::BigInt)) {
      return false;
    }
  }

  return true;
}

bool AllDoublePolicy::staticAdjustInputs(TempAllocator& alloc,
                                         MInstruction* ins) {
  for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
    MDefinition* in = ins->getOperand(i);
    if (in->type() == MIRType::Double) {
      continue;
    }

    if (!alloc.ensureBallast()) {
      return false;
    }
    MInstruction* replace = MToDouble::New(alloc, in);

    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(i, replace);

    if (!replace->typePolicy()->adjustInputs(alloc, replace)) {
      return false;
    }
  }

  return true;
}

bool ComparePolicy::adjustInputs(TempAllocator& alloc,
                                 MInstruction* def) const {
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

  auto replaceOperand = [&](size_t index, auto* replace) {
    def->block()->insertBefore(def, replace);
    def->replaceOperand(index, replace);
    return replace->typePolicy()->adjustInputs(alloc, replace);
  };

  if (compare->compareType() == MCompare::Compare_Undefined ||
      compare->compareType() == MCompare::Compare_Null) {
    // Nothing to do for undefined and null, lowering handles all types.
    return true;
  }

  if (compare->compareType() == MCompare::Compare_UIntPtr) {
    MOZ_ASSERT(compare->lhs()->type() == MIRType::IntPtr);
    MOZ_ASSERT(compare->rhs()->type() == MIRType::IntPtr);
    return true;
  }

  // Compare_BigInt_Int32 specialization is done for "BigInt <cmp> Int32".
  // Compare_BigInt_Double specialization is done for "BigInt <cmp> Double".
  // Compare_BigInt_String specialization is done for "BigInt <cmp> String".
  if (compare->compareType() == MCompare::Compare_BigInt_Int32 ||
      compare->compareType() == MCompare::Compare_BigInt_Double ||
      compare->compareType() == MCompare::Compare_BigInt_String) {
    if (MDefinition* in = def->getOperand(0); in->type() != MIRType::BigInt) {
      auto* replace =
          MUnbox::New(alloc, in, MIRType::BigInt, MUnbox::Infallible);
      if (!replaceOperand(0, replace)) {
        return false;
      }
    }

    MDefinition* in = def->getOperand(1);

    MInstruction* replace = nullptr;
    if (compare->compareType() == MCompare::Compare_BigInt_Int32) {
      if (in->type() != MIRType::Int32) {
        replace = MToNumberInt32::New(
            alloc, in, IntConversionInputKind::NumbersOrBoolsOnly);
      }
    } else if (compare->compareType() == MCompare::Compare_BigInt_Double) {
      if (in->type() != MIRType::Double) {
        replace = MToDouble::New(alloc, in, MToFPInstruction::NumbersOnly);
      }
    } else {
      MOZ_ASSERT(compare->compareType() == MCompare::Compare_BigInt_String);
      if (in->type() != MIRType::String) {
        replace = MUnbox::New(alloc, in, MIRType::String, MUnbox::Infallible);
      }
    }

    if (replace) {
      if (!replaceOperand(1, replace)) {
        return false;
      }
    }

    return true;
  }

  // Convert all inputs to the right input type
  MIRType type = compare->inputType();
  MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Double ||
             type == MIRType::Float32 || type == MIRType::Object ||
             type == MIRType::String || type == MIRType::Symbol ||
             type == MIRType::BigInt);
  for (size_t i = 0; i < 2; i++) {
    MDefinition* in = def->getOperand(i);
    if (in->type() == type) {
      continue;
    }

    MInstruction* replace;

    switch (type) {
      case MIRType::Double:
        replace = MToDouble::New(alloc, in, MToFPInstruction::NumbersOnly);
        break;
      case MIRType::Float32:
        replace = MToFloat32::New(alloc, in, MToFPInstruction::NumbersOnly);
        break;
      case MIRType::Int32: {
        IntConversionInputKind convert = IntConversionInputKind::NumbersOnly;
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
      case MIRType::BigInt:
        replace = MUnbox::New(alloc, in, MIRType::BigInt, MUnbox::Infallible);
        break;
      default:
        MOZ_CRASH("Unknown compare specialization");
    }

    if (!replaceOperand(i, replace)) {
      return false;
    }
  }

  return true;
}

bool TestPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins) const {
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
    case MIRType::BigInt:
    case MIRType::Object:
      break;

    case MIRType::String: {
      MStringLength* length = MStringLength::New(alloc, op);
      ins->block()->insertBefore(ins, length);
      ins->replaceOperand(0, length);
      break;
    }

    default:
      MOZ_ASSERT(IsMagicType(op->type()));
      ins->replaceOperand(0, BoxAt(alloc, ins, op));
      break;
  }
  return true;
}

bool BitwisePolicy::adjustInputs(TempAllocator& alloc,
                                 MInstruction* ins) const {
  MOZ_ASSERT(ins->type() == MIRType::Int32 || ins->type() == MIRType::Double);

  // This policy works for both unary and binary bitwise operations.
  for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
    if (!ConvertOperand<MTruncateToInt32>(alloc, ins, i, MIRType::Int32)) {
      return false;
    }
  }

  return true;
}

bool PowPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins) const {
  MOZ_ASSERT(ins->type() == MIRType::Int32 || ins->type() == MIRType::Double);

  if (ins->type() == MIRType::Int32) {
    // Both operands must be int32.
    return UnboxedInt32Policy<0>::staticAdjustInputs(alloc, ins) &&
           UnboxedInt32Policy<1>::staticAdjustInputs(alloc, ins);
  }

  // Otherwise, input must be a double.
  if (!DoublePolicy<0>::staticAdjustInputs(alloc, ins)) {
    return false;
  }

  // Power may be an int32 or a double. Integers receive a faster path.
  MDefinition* power = ins->toPow()->power();
  if (power->isToDouble()) {
    MDefinition* input = power->toToDouble()->input();
    if (input->type() == MIRType::Int32) {
      ins->replaceOperand(1, input);
      return true;
    }
  }
  return DoublePolicy<1>::staticAdjustInputs(alloc, ins);
}

bool SignPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins) const {
  MOZ_ASSERT(ins->isSign());
  MIRType specialization = ins->typePolicySpecialization();

  // MSign is specialized for int32 input types.
  if (specialization == MIRType::Int32) {
    return UnboxedInt32Policy<0>::staticAdjustInputs(alloc, ins);
  }

  // Otherwise convert input to double.
  MOZ_ASSERT(IsFloatingPointType(specialization));
  return DoublePolicy<0>::staticAdjustInputs(alloc, ins);
}

template <unsigned Op>
bool SymbolPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                          MInstruction* ins) {
  return UnboxOperand(alloc, ins, Op, MIRType::Symbol);
}

template bool SymbolPolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);

template <unsigned Op>
bool StringPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                          MInstruction* ins) {
  return UnboxOperand(alloc, ins, Op, MIRType::String);
}

template bool StringPolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);
template bool StringPolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);
template bool StringPolicy<2>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);

template <unsigned Op>
bool ConvertToStringPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* ins) {
  MDefinition* in = ins->getOperand(Op);
  if (in->type() == MIRType::String) {
    return true;
  }

  MToString* replace =
      MToString::New(alloc, in, MToString::SideEffectHandling::Bailout);
  ins->block()->insertBefore(ins, replace);
  ins->replaceOperand(Op, replace);

  return ToStringPolicy::staticAdjustInputs(alloc, replace);
}

template bool ConvertToStringPolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                           MInstruction* ins);
template bool ConvertToStringPolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                           MInstruction* ins);
template bool ConvertToStringPolicy<2>::staticAdjustInputs(TempAllocator& alloc,
                                                           MInstruction* ins);

template <unsigned Op>
bool BigIntPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                          MInstruction* ins) {
  return UnboxOperand(alloc, ins, Op, MIRType::BigInt);
}

template bool BigIntPolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);

template <unsigned Op>
bool UnboxedInt32Policy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                                MInstruction* def) {
  return UnboxOperand(alloc, def, Op, MIRType::Int32);
}

template bool UnboxedInt32Policy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                        MInstruction* def);
template bool UnboxedInt32Policy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                        MInstruction* def);
template bool UnboxedInt32Policy<2>::staticAdjustInputs(TempAllocator& alloc,
                                                        MInstruction* def);
template bool UnboxedInt32Policy<3>::staticAdjustInputs(TempAllocator& alloc,
                                                        MInstruction* def);

template <unsigned Op>
bool Int32OrIntPtrPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                                 MInstruction* def) {
  MDefinition* in = def->getOperand(Op);
  if (in->type() == MIRType::IntPtr) {
    return true;
  }

  return UnboxedInt32Policy<Op>::staticAdjustInputs(alloc, def);
}

template bool Int32OrIntPtrPolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                         MInstruction* def);
template bool Int32OrIntPtrPolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                         MInstruction* def);

template <unsigned Op>
bool ConvertToInt32Policy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* def) {
  return ConvertOperand<MToNumberInt32>(alloc, def, Op, MIRType::Int32);
}

template bool ConvertToInt32Policy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                          MInstruction* def);

template <unsigned Op>
bool TruncateToInt32OrToBigIntPolicy<Op>::staticAdjustInputs(
    TempAllocator& alloc, MInstruction* def) {
  MOZ_ASSERT(def->isCompareExchangeTypedArrayElement() ||
             def->isAtomicExchangeTypedArrayElement() ||
             def->isAtomicTypedArrayElementBinop());

  Scalar::Type type;
  if (def->isCompareExchangeTypedArrayElement()) {
    type = def->toCompareExchangeTypedArrayElement()->arrayType();
  } else if (def->isAtomicExchangeTypedArrayElement()) {
    type = def->toAtomicExchangeTypedArrayElement()->arrayType();
  } else {
    type = def->toAtomicTypedArrayElementBinop()->arrayType();
  }

  if (Scalar::isBigIntType(type)) {
    return ConvertOperand<MToBigInt>(alloc, def, Op, MIRType::BigInt);
  }
  return ConvertOperand<MTruncateToInt32>(alloc, def, Op, MIRType::Int32);
}

template bool TruncateToInt32OrToBigIntPolicy<2>::staticAdjustInputs(
    TempAllocator& alloc, MInstruction* def);
template bool TruncateToInt32OrToBigIntPolicy<3>::staticAdjustInputs(
    TempAllocator& alloc, MInstruction* def);

template <unsigned Op>
bool DoublePolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                          MInstruction* def) {
  return ConvertOperand<MToDouble>(alloc, def, Op, MIRType::Double);
}

template bool DoublePolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* def);
template bool DoublePolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* def);

template <unsigned Op>
bool Float32Policy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                           MInstruction* def) {
  return ConvertOperand<MToFloat32>(alloc, def, Op, MIRType::Float32);
}

template bool Float32Policy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* def);
template bool Float32Policy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* def);
template bool Float32Policy<2>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* def);

template <unsigned Op>
bool FloatingPointPolicy<Op>::adjustInputs(TempAllocator& alloc,
                                           MInstruction* def) const {
  MIRType policyType = def->typePolicySpecialization();
  if (policyType == MIRType::Double) {
    return DoublePolicy<Op>::staticAdjustInputs(alloc, def);
  }
  return Float32Policy<Op>::staticAdjustInputs(alloc, def);
}

template bool FloatingPointPolicy<0>::adjustInputs(TempAllocator& alloc,
                                                   MInstruction* def) const;

template <unsigned Op>
bool NoFloatPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                           MInstruction* def) {
  EnsureOperandNotFloat32(alloc, def, Op);
  return true;
}

template bool NoFloatPolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* def);
template bool NoFloatPolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* def);
template bool NoFloatPolicy<2>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* def);
template bool NoFloatPolicy<3>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* def);

template <unsigned FirstOp>
bool NoFloatPolicyAfter<FirstOp>::staticAdjustInputs(TempAllocator& alloc,
                                                     MInstruction* def) {
  for (size_t op = FirstOp, e = def->numOperands(); op < e; op++) {
    EnsureOperandNotFloat32(alloc, def, op);
  }
  return true;
}

template bool NoFloatPolicyAfter<0>::staticAdjustInputs(TempAllocator& alloc,
                                                        MInstruction* def);
template bool NoFloatPolicyAfter<1>::staticAdjustInputs(TempAllocator& alloc,
                                                        MInstruction* def);
template bool NoFloatPolicyAfter<2>::staticAdjustInputs(TempAllocator& alloc,
                                                        MInstruction* def);

template <unsigned Op>
bool BoxPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                       MInstruction* ins) {
  MDefinition* in = ins->getOperand(Op);
  if (in->type() == MIRType::Value) {
    return true;
  }

  ins->replaceOperand(Op, BoxAt(alloc, ins, in));
  return true;
}

template bool BoxPolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                               MInstruction* ins);
template bool BoxPolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                               MInstruction* ins);
template bool BoxPolicy<2>::staticAdjustInputs(TempAllocator& alloc,
                                               MInstruction* ins);

template <unsigned Op, MIRType Type>
bool BoxExceptPolicy<Op, Type>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* ins) {
  MDefinition* in = ins->getOperand(Op);
  if (in->type() == Type) {
    return true;
  }
  return BoxPolicy<Op>::staticAdjustInputs(alloc, ins);
}

template bool BoxExceptPolicy<0, MIRType::Object>::staticAdjustInputs(
    TempAllocator& alloc, MInstruction* ins);

template <unsigned Op>
bool CacheIdPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                           MInstruction* ins) {
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

template bool CacheIdPolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* ins);
template bool CacheIdPolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                   MInstruction* ins);

bool ToDoublePolicy::staticAdjustInputs(TempAllocator& alloc,
                                        MInstruction* ins) {
  MOZ_ASSERT(ins->isToDouble() || ins->isToFloat32());

  MDefinition* in = ins->getOperand(0);
  MToFPInstruction::ConversionKind conversion;
  if (ins->isToDouble()) {
    conversion = ins->toToDouble()->conversion();
  } else {
    conversion = ins->toToFloat32()->conversion();
  }

  switch (in->type()) {
    case MIRType::Int32:
    case MIRType::Float32:
    case MIRType::Double:
    case MIRType::Value:
      // No need for boxing for these types.
      return true;
    case MIRType::Null:
      // No need for boxing, when we will convert.
      if (conversion == MToFPInstruction::NonStringPrimitives) {
        return true;
      }
      break;
    case MIRType::Undefined:
    case MIRType::Boolean:
      // No need for boxing, when we will convert.
      if (conversion == MToFPInstruction::NonStringPrimitives) {
        return true;
      }
      break;
    case MIRType::Object:
    case MIRType::String:
    case MIRType::Symbol:
    case MIRType::BigInt:
      // Objects might be effectful. Symbols and BigInts give TypeError.
      break;
    default:
      break;
  }

  in = BoxAt(alloc, ins, in);
  ins->replaceOperand(0, in);
  return true;
}

bool ToInt32Policy::staticAdjustInputs(TempAllocator& alloc,
                                       MInstruction* ins) {
  MOZ_ASSERT(ins->isToNumberInt32() || ins->isTruncateToInt32() ||
             ins->isToIntegerInt32());

  IntConversionInputKind conversion = IntConversionInputKind::Any;
  if (ins->isToNumberInt32()) {
    conversion = ins->toToNumberInt32()->conversion();
  }

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
      // Also no need for boxing when performing ToInteger, because
      // ToInteger(undefined) = ToInteger(NaN) = 0.
      if (ins->isTruncateToInt32() || ins->isToIntegerInt32()) {
        return true;
      }
      break;
    case MIRType::Null:
      // No need for boxing, when we will convert.
      if (conversion == IntConversionInputKind::Any) {
        return true;
      }
      break;
    case MIRType::Boolean:
      // No need for boxing, when we will convert.
      if (conversion == IntConversionInputKind::Any) {
        return true;
      }
      if (conversion == IntConversionInputKind::NumbersOrBoolsOnly) {
        return true;
      }
      break;
    case MIRType::Object:
    case MIRType::String:
    case MIRType::Symbol:
    case MIRType::BigInt:
      // Objects might be effectful. Symbols and BigInts give TypeError.
      break;
    default:
      break;
  }

  in = BoxAt(alloc, ins, in);
  ins->replaceOperand(0, in);
  return true;
}

bool ToBigIntPolicy::staticAdjustInputs(TempAllocator& alloc,
                                        MInstruction* ins) {
  MOZ_ASSERT(ins->isToBigInt());

  MDefinition* in = ins->getOperand(0);
  switch (in->type()) {
    case MIRType::BigInt:
    case MIRType::Value:
      // No need for boxing for these types.
      return true;
    default:
      // Any other types need to be boxed.
      break;
  }

  in = BoxAt(alloc, ins, in);
  ins->replaceOperand(0, in);
  return true;
}

bool ToStringPolicy::staticAdjustInputs(TempAllocator& alloc,
                                        MInstruction* ins) {
  MOZ_ASSERT(ins->isToString());

  MIRType type = ins->getOperand(0)->type();
  if (type == MIRType::Object || type == MIRType::Symbol ||
      type == MIRType::BigInt) {
    ins->replaceOperand(0, BoxAt(alloc, ins, ins->getOperand(0)));
    return true;
  }

  // TODO remove the following line once 966957 has landed
  EnsureOperandNotFloat32(alloc, ins, 0);

  return true;
}

bool ToInt64Policy::staticAdjustInputs(TempAllocator& alloc,
                                       MInstruction* ins) {
  MOZ_ASSERT(ins->isToInt64());

  MDefinition* input = ins->getOperand(0);
  MIRType type = input->type();

  switch (type) {
    case MIRType::BigInt: {
      auto* replace = MTruncateBigIntToInt64::New(alloc, input);
      ins->block()->insertBefore(ins, replace);
      ins->replaceOperand(0, replace);
      break;
    }
    // No need for boxing for these types, because they are handled specially
    // when this instruction is lowered to LIR.
    case MIRType::Boolean:
    case MIRType::String:
    case MIRType::Int64:
    case MIRType::Value:
      break;
    default:
      ins->replaceOperand(0, BoxAt(alloc, ins, ins->getOperand(0)));
      break;
  }

  return true;
}

template <unsigned Op>
bool ObjectPolicy<Op>::staticAdjustInputs(TempAllocator& alloc,
                                          MInstruction* ins) {
  MOZ_ASSERT(ins->getOperand(Op)->type() != MIRType::Slots);
  MOZ_ASSERT(ins->getOperand(Op)->type() != MIRType::Elements);

  return UnboxOperand(alloc, ins, Op, MIRType::Object);
}

template bool ObjectPolicy<0>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);
template bool ObjectPolicy<1>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);
template bool ObjectPolicy<2>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);
template bool ObjectPolicy<3>::staticAdjustInputs(TempAllocator& alloc,
                                                  MInstruction* ins);

bool CallPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins) const {
  MCall* call = ins->toCall();

  MDefinition* func = call->getCallee();
  if (func->type() != MIRType::Object) {
    MInstruction* unbox =
        MUnbox::New(alloc, func, MIRType::Object, MUnbox::Fallible);
    unbox->setBailoutKind(BailoutKind::TypePolicy);
    call->block()->insertBefore(call, unbox);
    call->replaceCallee(unbox);

    if (!unbox->typePolicy()->adjustInputs(alloc, unbox)) {
      return false;
    }
  }

  for (uint32_t i = 0; i < call->numStackArgs(); i++) {
    if (!alloc.ensureBallast()) {
      return false;
    }
    EnsureOperandNotFloat32(alloc, call, MCall::IndexOfStackArg(i));
  }

  return true;
}

bool CallSetElementPolicy::adjustInputs(TempAllocator& alloc,
                                        MInstruction* ins) const {
  // The first operand should be an object.
  if (!SingleObjectPolicy::staticAdjustInputs(alloc, ins)) {
    return false;
  }

  // Box the index and value operands.
  for (size_t i = 1, e = ins->numOperands(); i < e; i++) {
    MDefinition* in = ins->getOperand(i);
    if (in->type() == MIRType::Value) {
      continue;
    }
    ins->replaceOperand(i, BoxAt(alloc, ins, in));
  }
  return true;
}

bool StoreUnboxedScalarPolicy::adjustValueInput(TempAllocator& alloc,
                                                MInstruction* ins,
                                                Scalar::Type writeType,
                                                MDefinition* value,
                                                int valueOperand) {
  if (Scalar::isBigIntType(writeType)) {
    if (value->type() == MIRType::BigInt) {
      return true;
    }

    auto* replace = MToBigInt::New(alloc, value);
    ins->block()->insertBefore(ins, replace);
    ins->replaceOperand(valueOperand, replace);

    return replace->typePolicy()->adjustInputs(alloc, replace);
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
      value = MConstant::New(alloc, JS::NaNValue());
      ins->block()->insertBefore(ins, value->toInstruction());
      break;
    case MIRType::Object:
    case MIRType::String:
    case MIRType::Symbol:
    case MIRType::BigInt:
      value = BoxAt(alloc, ins, value);
      break;
    default:
      MOZ_CRASH("Unexpected type");
  }

  if (value != curValue) {
    ins->replaceOperand(valueOperand, value);
    curValue = value;
  }

  MOZ_ASSERT(
      value->type() == MIRType::Int32 || value->type() == MIRType::Boolean ||
      value->type() == MIRType::Double || value->type() == MIRType::Float32 ||
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
      // The transpiler should have inserted MClampToUint8.
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

  if (value != curValue) {
    ins->replaceOperand(valueOperand, value);
  }

  return true;
}

bool StoreUnboxedScalarPolicy::adjustInputs(TempAllocator& alloc,
                                            MInstruction* ins) const {
  MStoreUnboxedScalar* store = ins->toStoreUnboxedScalar();
  MOZ_ASSERT(store->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(store->index()->type() == MIRType::IntPtr);

  return adjustValueInput(alloc, store, store->writeType(), store->value(), 2);
}

bool StoreDataViewElementPolicy::adjustInputs(TempAllocator& alloc,
                                              MInstruction* ins) const {
  auto* store = ins->toStoreDataViewElement();
  MOZ_ASSERT(store->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(store->index()->type() == MIRType::IntPtr);
  MOZ_ASSERT(store->littleEndian()->type() == MIRType::Boolean);

  return StoreUnboxedScalarPolicy::adjustValueInput(
      alloc, ins, store->writeType(), store->value(), 2);
}

bool StoreTypedArrayHolePolicy::adjustInputs(TempAllocator& alloc,
                                             MInstruction* ins) const {
  MStoreTypedArrayElementHole* store = ins->toStoreTypedArrayElementHole();
  MOZ_ASSERT(store->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(store->index()->type() == MIRType::IntPtr);
  MOZ_ASSERT(store->length()->type() == MIRType::IntPtr);

  return StoreUnboxedScalarPolicy::adjustValueInput(
      alloc, ins, store->arrayType(), store->value(), 3);
}

bool ClampPolicy::adjustInputs(TempAllocator& alloc, MInstruction* ins) const {
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

// Lists of all TypePolicy specializations which are used by MIR Instructions.
#define TYPE_POLICY_LIST(_)     \
  _(AllDoublePolicy)            \
  _(ArithPolicy)                \
  _(BigIntArithPolicy)          \
  _(BitwisePolicy)              \
  _(BoxInputsPolicy)            \
  _(CallPolicy)                 \
  _(CallSetElementPolicy)       \
  _(ClampPolicy)                \
  _(ComparePolicy)              \
  _(PowPolicy)                  \
  _(SignPolicy)                 \
  _(StoreDataViewElementPolicy) \
  _(StoreTypedArrayHolePolicy)  \
  _(StoreUnboxedScalarPolicy)   \
  _(TestPolicy)                 \
  _(ToDoublePolicy)             \
  _(ToInt32Policy)              \
  _(ToBigIntPolicy)             \
  _(ToStringPolicy)             \
  _(ToInt64Policy)

#define TEMPLATE_TYPE_POLICY_LIST(_)                                          \
  _(BoxExceptPolicy<0, MIRType::Object>)                                      \
  _(BoxPolicy<0>)                                                             \
  _(ConvertToInt32Policy<0>)                                                  \
  _(ConvertToStringPolicy<0>)                                                 \
  _(ConvertToStringPolicy<2>)                                                 \
  _(DoublePolicy<0>)                                                          \
  _(FloatingPointPolicy<0>)                                                   \
  _(UnboxedInt32Policy<0>)                                                    \
  _(UnboxedInt32Policy<1>)                                                    \
  _(TruncateToInt32OrToBigIntPolicy<2>)                                       \
  _(MixPolicy<ObjectPolicy<0>, StringPolicy<1>, BoxPolicy<2>>)                \
  _(MixPolicy<ObjectPolicy<0>, BoxPolicy<1>, BoxPolicy<2>>)                   \
  _(MixPolicy<ObjectPolicy<0>, BoxPolicy<1>, ObjectPolicy<2>>)                \
  _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>, BoxPolicy<2>>)          \
  _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>, UnboxedInt32Policy<2>>) \
  _(MixPolicy<ObjectPolicy<0>, BoxPolicy<2>>)                                 \
  _(MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>, UnboxedInt32Policy<2>>)       \
  _(MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>, ObjectPolicy<2>>)             \
  _(MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>, BoxPolicy<2>>)                \
  _(MixPolicy<StringPolicy<0>, UnboxedInt32Policy<1>, UnboxedInt32Policy<2>>) \
  _(MixPolicy<StringPolicy<0>, ObjectPolicy<1>, StringPolicy<2>>)             \
  _(MixPolicy<StringPolicy<0>, StringPolicy<1>, StringPolicy<2>>)             \
  _(MixPolicy<ObjectPolicy<0>, StringPolicy<1>, UnboxedInt32Policy<2>>)       \
  _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>, UnboxedInt32Policy<2>,  \
              UnboxedInt32Policy<3>>)                                         \
  _(MixPolicy<TruncateToInt32OrToBigIntPolicy<2>,                             \
              TruncateToInt32OrToBigIntPolicy<3>>)                            \
  _(MixPolicy<ObjectPolicy<0>, CacheIdPolicy<1>, NoFloatPolicy<2>>)           \
  _(MixPolicy<ObjectPolicy<0>, BoxExceptPolicy<1, MIRType::Object>,           \
              CacheIdPolicy<2>>)                                              \
  _(MixPolicy<BoxPolicy<0>, ObjectPolicy<1>>)                                 \
  _(MixPolicy<ConvertToStringPolicy<0>, ConvertToStringPolicy<1>>)            \
  _(MixPolicy<ConvertToStringPolicy<0>, ObjectPolicy<1>>)                     \
  _(MixPolicy<DoublePolicy<0>, DoublePolicy<1>>)                              \
  _(MixPolicy<UnboxedInt32Policy<0>, UnboxedInt32Policy<1>>)                  \
  _(MixPolicy<Int32OrIntPtrPolicy<0>, Int32OrIntPtrPolicy<1>>)                \
  _(MixPolicy<ObjectPolicy<0>, BoxPolicy<1>>)                                 \
  _(MixPolicy<BoxExceptPolicy<0, MIRType::Object>, CacheIdPolicy<1>>)         \
  _(MixPolicy<CacheIdPolicy<0>, ObjectPolicy<1>>)                             \
  _(MixPolicy<ObjectPolicy<0>, ConvertToStringPolicy<1>>)                     \
  _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>>)                        \
  _(MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<2>>)                        \
  _(MixPolicy<ObjectPolicy<0>, NoFloatPolicy<1>>)                             \
  _(MixPolicy<ObjectPolicy<0>, NoFloatPolicy<2>>)                             \
  _(MixPolicy<ObjectPolicy<0>, NoFloatPolicy<3>>)                             \
  _(MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>>)                              \
  _(MixPolicy<ObjectPolicy<0>, StringPolicy<1>>)                              \
  _(MixPolicy<ObjectPolicy<0>, ConvertToStringPolicy<2>>)                     \
  _(MixPolicy<ObjectPolicy<1>, ConvertToStringPolicy<0>>)                     \
  _(MixPolicy<StringPolicy<0>, UnboxedInt32Policy<1>>)                        \
  _(MixPolicy<StringPolicy<0>, StringPolicy<1>>)                              \
  _(MixPolicy<BoxPolicy<0>, BoxPolicy<1>>)                                    \
  _(MixPolicy<ObjectPolicy<0>, BoxPolicy<2>, ObjectPolicy<3>>)                \
  _(MixPolicy<BoxExceptPolicy<0, MIRType::Object>, ObjectPolicy<1>>)          \
  _(MixPolicy<UnboxedInt32Policy<0>, BigIntPolicy<1>>)                        \
  _(MixPolicy<UnboxedInt32Policy<0>, NoFloatPolicyAfter<1>>)                  \
  _(NoFloatPolicy<0>)                                                         \
  _(NoFloatPolicy<1>)                                                         \
  _(NoFloatPolicy<2>)                                                         \
  _(NoFloatPolicyAfter<0>)                                                    \
  _(NoFloatPolicyAfter<1>)                                                    \
  _(NoFloatPolicyAfter<2>)                                                    \
  _(ObjectPolicy<0>)                                                          \
  _(ObjectPolicy<1>)                                                          \
  _(ObjectPolicy<3>)                                                          \
  _(StringPolicy<0>)                                                          \
  _(SymbolPolicy<0>)

namespace js {
namespace jit {

// Define for all used TypePolicy specialization, the definition for
// |TypePolicy::Data::thisTypePolicy|.  This function returns one constant
// instance of the TypePolicy which is shared among all MIR Instructions of the
// same type.
//
// This Macro use __VA_ARGS__ to account for commas of template parameters.
#define DEFINE_TYPE_POLICY_SINGLETON_INSTANCES_(...)      \
  const TypePolicy* __VA_ARGS__::Data::thisTypePolicy() { \
    static constexpr __VA_ARGS__ singletonType;           \
    return &singletonType;                                \
  }

TYPE_POLICY_LIST(DEFINE_TYPE_POLICY_SINGLETON_INSTANCES_)
TEMPLATE_TYPE_POLICY_LIST(template <> DEFINE_TYPE_POLICY_SINGLETON_INSTANCES_)
#undef DEFINE_TYPE_POLICY_SINGLETON_INSTANCES_

}  // namespace jit
}  // namespace js

namespace {

// For extra-good measure in case an unqualified use is ever introduced.  (The
// main use in the macro below is explicitly qualified so as not to consult
// this scope and find this function.)
inline TypePolicy* thisTypePolicy() = delete;

static MIRType thisTypeSpecialization() {
  MOZ_CRASH("TypeSpecialization lacks definition of thisTypeSpecialization.");
}

}  // namespace

// For each MIR Instruction, this macro define the |typePolicy| method which is
// using the |thisTypePolicy| method.  The |thisTypePolicy| method is either a
// member of the MIR Instruction, such as with MGetElementCache, a member
// inherited from the TypePolicy::Data structure, or a member inherited from
// NoTypePolicy if the MIR instruction has no type policy.
#define DEFINE_MIR_TYPEPOLICY_MEMBERS_(op)             \
  const TypePolicy* js::jit::M##op::typePolicy() {     \
    return M##op::thisTypePolicy();                    \
  }                                                    \
                                                       \
  MIRType js::jit::M##op::typePolicySpecialization() { \
    return thisTypeSpecialization();                   \
  }

MIR_OPCODE_LIST(DEFINE_MIR_TYPEPOLICY_MEMBERS_)
#undef DEFINE_MIR_TYPEPOLICY_MEMBERS_
