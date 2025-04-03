/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x64/CodeGenerator-x64.h"

#include "jit/CodeGenerator.h"
#include "jit/MIR.h"
#include "js/ScalarType.h"  // js::Scalar::Type

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

CodeGeneratorX64::CodeGeneratorX64(MIRGenerator* gen, LIRGraph* graph,
                                   MacroAssembler* masm)
    : CodeGeneratorX86Shared(gen, graph, masm) {}

ValueOperand CodeGeneratorX64::ToValue(LInstruction* ins, size_t pos) {
  return ValueOperand(ToRegister(ins->getOperand(pos)));
}

ValueOperand CodeGeneratorX64::ToTempValue(LInstruction* ins, size_t pos) {
  return ValueOperand(ToRegister(ins->getTemp(pos)));
}

Operand CodeGeneratorX64::ToOperand64(const LInt64Allocation& a64) {
  const LAllocation& a = a64.value();
  MOZ_ASSERT(!a.isFloatReg());
  if (a.isGeneralReg()) {
    return Operand(a.toGeneralReg()->reg());
  }
  return Operand(ToAddress(a));
}

void CodeGenerator::visitValue(LValue* value) {
  ValueOperand result = ToOutValue(value);
  masm.moveValue(value->value(), result);
}

void CodeGenerator::visitBox(LBox* box) {
  const LAllocation* in = box->getOperand(0);
  ValueOperand result = ToOutValue(box);

  masm.moveValue(TypedOrValueRegister(box->type(), ToAnyRegister(in)), result);

  if (JitOptions.spectreValueMasking && IsFloatingPointType(box->type())) {
    ScratchRegisterScope scratch(masm);
    masm.movePtr(ImmWord(JSVAL_SHIFTED_TAG_MAX_DOUBLE), scratch);
    masm.cmpPtrMovePtr(Assembler::Below, scratch, result.valueReg(), scratch,
                       result.valueReg());
  }
}

void CodeGenerator::visitUnbox(LUnbox* unbox) {
  MUnbox* mir = unbox->mir();

  Register result = ToRegister(unbox->output());

  if (mir->fallible()) {
    const ValueOperand value = ToValue(unbox, LUnbox::Input);
    Label bail;
    switch (mir->type()) {
      case MIRType::Int32:
        masm.fallibleUnboxInt32(value, result, &bail);
        break;
      case MIRType::Boolean:
        masm.fallibleUnboxBoolean(value, result, &bail);
        break;
      case MIRType::Object:
        masm.fallibleUnboxObject(value, result, &bail);
        break;
      case MIRType::String:
        masm.fallibleUnboxString(value, result, &bail);
        break;
      case MIRType::Symbol:
        masm.fallibleUnboxSymbol(value, result, &bail);
        break;
      case MIRType::BigInt:
        masm.fallibleUnboxBigInt(value, result, &bail);
        break;
      default:
        MOZ_CRASH("Given MIRType cannot be unboxed.");
    }
    bailoutFrom(&bail, unbox->snapshot());
    return;
  }

  // Infallible unbox.

  Operand input = ToOperand(unbox->getOperand(LUnbox::Input));

#ifdef DEBUG
  // Assert the types match.
  JSValueTag tag = MIRTypeToTag(mir->type());
  Label ok;
  masm.splitTag(input, ScratchReg);
  masm.branch32(Assembler::Equal, ScratchReg, Imm32(tag), &ok);
  masm.assumeUnreachable("Infallible unbox type mismatch");
  masm.bind(&ok);
#endif

  switch (mir->type()) {
    case MIRType::Int32:
      masm.unboxInt32(input, result);
      break;
    case MIRType::Boolean:
      masm.unboxBoolean(input, result);
      break;
    case MIRType::Object:
      masm.unboxObject(input, result);
      break;
    case MIRType::String:
      masm.unboxString(input, result);
      break;
    case MIRType::Symbol:
      masm.unboxSymbol(input, result);
      break;
    case MIRType::BigInt:
      masm.unboxBigInt(input, result);
      break;
    default:
      MOZ_CRASH("Given MIRType cannot be unboxed.");
  }
}

void CodeGenerator::visitCompareI64(LCompareI64* lir) {
  MCompare* mir = lir->mir();
  MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
             mir->compareType() == MCompare::Compare_UInt64);

  const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
  Register lhsReg = ToRegister64(lhs).reg;
  Register output = ToRegister(lir->output());

  if (IsConstant(rhs)) {
    masm.cmpPtr(lhsReg, ImmWord(ToInt64(rhs)));
  } else {
    masm.cmpPtr(lhsReg, ToOperand64(rhs));
  }

  bool isSigned = mir->compareType() == MCompare::Compare_Int64;
  masm.emitSet(JSOpToCondition(lir->jsop(), isSigned), output);
}

void CodeGenerator::visitCompareI64AndBranch(LCompareI64AndBranch* lir) {
  MCompare* mir = lir->cmpMir();
  MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
             mir->compareType() == MCompare::Compare_UInt64);

  LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
  LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
  Register lhsReg = ToRegister64(lhs).reg;

  if (IsConstant(rhs)) {
    masm.cmpPtr(lhsReg, ImmWord(ToInt64(rhs)));
  } else {
    masm.cmpPtr(lhsReg, ToOperand64(rhs));
  }

  bool isSigned = mir->compareType() == MCompare::Compare_Int64;
  emitBranch(JSOpToCondition(lir->jsop(), isSigned), lir->ifTrue(),
             lir->ifFalse());
}

void CodeGenerator::visitBitAndAndBranch(LBitAndAndBranch* baab) {
  Register regL = ToRegister(baab->left());
  if (baab->is64()) {
    if (baab->right()->isConstant()) {
      masm.test64(regL, Imm64(ToInt64(baab->right())));
    } else {
      masm.test64(regL, ToRegister(baab->right()));
    }
  } else {
    if (baab->right()->isConstant()) {
      masm.test32(regL, Imm32(ToInt32(baab->right())));
    } else {
      masm.test32(regL, ToRegister(baab->right()));
    }
  }
  emitBranch(baab->cond(), baab->ifTrue(), baab->ifFalse());
}

void CodeGenerator::visitDivOrModI64(LDivOrModI64* lir) {
  Register lhs = ToRegister(lir->lhs());
  Register rhs = ToRegister(lir->rhs());
  Register output = ToRegister(lir->output());

  MOZ_ASSERT_IF(lhs != rhs, rhs != rax);
  MOZ_ASSERT(rhs != rdx);
  MOZ_ASSERT_IF(output == rax, ToRegister(lir->remainder()) == rdx);
  MOZ_ASSERT_IF(output == rdx, ToRegister(lir->remainder()) == rax);

  Label done;

  // Put the lhs in rax.
  if (lhs != rax) {
    masm.mov(lhs, rax);
  }

  // Handle divide by zero.
  if (lir->canBeDivideByZero()) {
    Label nonZero;
    masm.branchTestPtr(Assembler::NonZero, rhs, rhs, &nonZero);
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
    masm.bind(&nonZero);
  }

  // Handle an integer overflow exception from INT64_MIN / -1.
  if (lir->canBeNegativeOverflow()) {
    Label notOverflow;
    masm.branchPtr(Assembler::NotEqual, lhs, ImmWord(INT64_MIN), &notOverflow);
    masm.branchPtr(Assembler::NotEqual, rhs, ImmWord(-1), &notOverflow);
    if (lir->mir()->isMod()) {
      masm.xorl(output, output);
    } else {
      masm.wasmTrap(wasm::Trap::IntegerOverflow, lir->bytecodeOffset());
    }
    masm.jump(&done);
    masm.bind(&notOverflow);
  }

  // Sign extend the lhs into rdx to make rdx:rax.
  masm.cqo();
  masm.idivq(rhs);

  masm.bind(&done);
}

void CodeGenerator::visitUDivOrModI64(LUDivOrModI64* lir) {
  Register lhs = ToRegister(lir->lhs());
  Register rhs = ToRegister(lir->rhs());

  DebugOnly<Register> output = ToRegister(lir->output());
  MOZ_ASSERT_IF(lhs != rhs, rhs != rax);
  MOZ_ASSERT(rhs != rdx);
  MOZ_ASSERT_IF(output.value == rax, ToRegister(lir->remainder()) == rdx);
  MOZ_ASSERT_IF(output.value == rdx, ToRegister(lir->remainder()) == rax);

  // Put the lhs in rax.
  if (lhs != rax) {
    masm.mov(lhs, rax);
  }

  Label done;

  // Prevent divide by zero.
  if (lir->canBeDivideByZero()) {
    Label nonZero;
    masm.branchTestPtr(Assembler::NonZero, rhs, rhs, &nonZero);
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
    masm.bind(&nonZero);
  }

  // Zero extend the lhs into rdx to make (rdx:rax).
  masm.xorl(rdx, rdx);
  masm.udivq(rhs);

  masm.bind(&done);
}

void CodeGeneratorX64::emitBigIntDiv(LBigIntDiv* ins, Register dividend,
                                     Register divisor, Register output,
                                     Label* fail) {
  // Callers handle division by zero and integer overflow.

  MOZ_ASSERT(dividend == rax);
  MOZ_ASSERT(output == rdx);

  // Sign extend the lhs into rdx to make rdx:rax.
  masm.cqo();

  masm.idivq(divisor);

  // Create and return the result.
  masm.newGCBigInt(output, divisor, initialBigIntHeap(), fail);
  masm.initializeBigInt(output, dividend);
}

void CodeGeneratorX64::emitBigIntMod(LBigIntMod* ins, Register dividend,
                                     Register divisor, Register output,
                                     Label* fail) {
  // Callers handle division by zero and integer overflow.

  MOZ_ASSERT(dividend == rax);
  MOZ_ASSERT(output == rdx);

  // Sign extend the lhs into rdx to make rdx:rax.
  masm.cqo();

  masm.idivq(divisor);

  // Move the remainder from rdx.
  masm.movq(output, dividend);

  // Create and return the result.
  masm.newGCBigInt(output, divisor, initialBigIntHeap(), fail);
  masm.initializeBigInt(output, dividend);
}

void CodeGenerator::visitAtomicLoad64(LAtomicLoad64* lir) {
  Register elements = ToRegister(lir->elements());
  Register temp = ToRegister(lir->temp());
  Register64 temp64 = ToRegister64(lir->temp64());
  Register out = ToRegister(lir->output());

  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

  // NOTE: the generated code must match the assembly code in gen_load in
  // GenerateAtomicOperations.py
  auto sync = Synchronization::Load();

  masm.memoryBarrierBefore(sync);
  if (lir->index()->isConstant()) {
    Address source =
        ToAddress(elements, lir->index(), storageType, mir->offsetAdjustment());
    masm.load64(source, temp64);
  } else {
    BaseIndex source(elements, ToRegister(lir->index()),
                     ScaleFromScalarType(storageType), mir->offsetAdjustment());
    masm.load64(source, temp64);
  }
  masm.memoryBarrierAfter(sync);

  emitCreateBigInt(lir, storageType, temp64, out, temp);
}

void CodeGenerator::visitAtomicStore64(LAtomicStore64* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());

  Scalar::Type writeType = lir->mir()->writeType();

  masm.loadBigInt64(value, temp1);

  // NOTE: the generated code must match the assembly code in gen_store in
  // GenerateAtomicOperations.py
  auto sync = Synchronization::Store();

  masm.memoryBarrierBefore(sync);
  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), writeType);
    masm.store64(temp1, dest);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(writeType));
    masm.store64(temp1, dest);
  }
  masm.memoryBarrierAfter(sync);
}

void CodeGenerator::visitCompareExchangeTypedArrayElement64(
    LCompareExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register oldval = ToRegister(lir->oldval());
  Register newval = ToRegister(lir->newval());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = ToRegister64(lir->temp2());
  Register out = ToRegister(lir->output());

  MOZ_ASSERT(temp1.reg == rax);

  Scalar::Type arrayType = lir->mir()->arrayType();

  masm.loadBigInt64(oldval, temp1);
  masm.loadBigInt64(newval, temp2);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.compareExchange64(Synchronization::Full(), dest, temp1, temp2, temp1);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.compareExchange64(Synchronization::Full(), dest, temp1, temp2, temp1);
  }

  emitCreateBigInt(lir, arrayType, temp1, out, temp2.reg);
}

void CodeGenerator::visitAtomicExchangeTypedArrayElement64(
    LAtomicExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register out = ToRegister(lir->output());

  Scalar::Type arrayType = lir->mir()->arrayType();

  masm.loadBigInt64(value, temp1);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicExchange64(Synchronization::Full(), dest, temp1, temp1);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicExchange64(Synchronization::Full(), dest, temp1, temp1);
  }

  emitCreateBigInt(lir, arrayType, temp1, out, temp2);
}

void CodeGenerator::visitAtomicTypedArrayElementBinop64(
    LAtomicTypedArrayElementBinop64* lir) {
  MOZ_ASSERT(!lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = ToRegister64(lir->temp2());
  Register out = ToRegister(lir->output());

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  masm.loadBigInt64(value, temp1);

  Register64 fetchTemp = Register64(out);
  Register64 fetchOut = temp2;
  Register createTemp = temp1.reg;

  // Add and Sub don't need |fetchTemp| and can save a `mov` when the value and
  // output register are equal to each other.
  if (atomicOp == AtomicFetchAddOp || atomicOp == AtomicFetchSubOp) {
    fetchTemp = Register64::Invalid();
    fetchOut = temp1;
    createTemp = temp2.reg;
  } else {
    MOZ_ASSERT(temp2.reg == rax);
  }

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, temp1, dest,
                         fetchTemp, fetchOut);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, temp1, dest,
                         fetchTemp, fetchOut);
  }

  emitCreateBigInt(lir, arrayType, fetchOut, out, createTemp);
}

void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect64(
    LAtomicTypedArrayElementBinopForEffect64* lir) {
  MOZ_ASSERT(lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  masm.loadBigInt64(value, temp1);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicEffectOp64(Synchronization::Full(), atomicOp, temp1, dest);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicEffectOp64(Synchronization::Full(), atomicOp, temp1, dest);
  }
}

void CodeGenerator::visitWasmSelectI64(LWasmSelectI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);

  Register cond = ToRegister(lir->condExpr());

  Operand falseExpr = ToOperandOrRegister64(lir->falseExpr());

  Register64 out = ToOutRegister64(lir);
  MOZ_ASSERT(ToRegister64(lir->trueExpr()) == out,
             "true expr is reused for input");

  masm.test32(cond, cond);
  masm.cmovzq(falseExpr, out.reg);
}

// We expect to handle only the cases: compare is {U,}Int{32,64}, and select
// is {U,}Int{32,64}, independently.  Some values may be stack allocated, and
// the "true" input is reused for the output.
void CodeGenerator::visitWasmCompareAndSelect(LWasmCompareAndSelect* ins) {
  bool cmpIs32bit = ins->compareType() == MCompare::Compare_Int32 ||
                    ins->compareType() == MCompare::Compare_UInt32;
  bool cmpIs64bit = ins->compareType() == MCompare::Compare_Int64 ||
                    ins->compareType() == MCompare::Compare_UInt64;
  bool selIs32bit = ins->mir()->type() == MIRType::Int32;
  bool selIs64bit = ins->mir()->type() == MIRType::Int64;

  // Throw out unhandled cases
  MOZ_RELEASE_ASSERT(
      cmpIs32bit != cmpIs64bit && selIs32bit != selIs64bit,
      "CodeGenerator::visitWasmCompareAndSelect: unexpected types");

  using C = Assembler::Condition;
  using R = Register;
  using A = const Address&;

  // Identify macroassembler methods to generate instructions, based on the
  // type of the comparison and the select.  This avoids having to duplicate
  // the code-generation tree below 4 times.  These assignments to
  // `cmpMove_CRRRR` et al are unambiguous as a result of the combination of
  // the template parameters and the 5 argument types ((C, R, R, R, R) etc).
  void (MacroAssembler::*cmpMove_CRRRR)(C, R, R, R, R) = nullptr;
  void (MacroAssembler::*cmpMove_CRARR)(C, R, A, R, R) = nullptr;
  void (MacroAssembler::*cmpLoad_CRRAR)(C, R, R, A, R) = nullptr;
  void (MacroAssembler::*cmpLoad_CRAAR)(C, R, A, A, R) = nullptr;

  if (cmpIs32bit) {
    if (selIs32bit) {
      cmpMove_CRRRR = &MacroAssemblerX64::cmpMove<32, 32>;
      cmpMove_CRARR = &MacroAssemblerX64::cmpMove<32, 32>;
      cmpLoad_CRRAR = &MacroAssemblerX64::cmpLoad<32, 32>;
      cmpLoad_CRAAR = &MacroAssemblerX64::cmpLoad<32, 32>;
    } else {
      cmpMove_CRRRR = &MacroAssemblerX64::cmpMove<32, 64>;
      cmpMove_CRARR = &MacroAssemblerX64::cmpMove<32, 64>;
      cmpLoad_CRRAR = &MacroAssemblerX64::cmpLoad<32, 64>;
      cmpLoad_CRAAR = &MacroAssemblerX64::cmpLoad<32, 64>;
    }
  } else {
    if (selIs32bit) {
      cmpMove_CRRRR = &MacroAssemblerX64::cmpMove<64, 32>;
      cmpMove_CRARR = &MacroAssemblerX64::cmpMove<64, 32>;
      cmpLoad_CRRAR = &MacroAssemblerX64::cmpLoad<64, 32>;
      cmpLoad_CRAAR = &MacroAssemblerX64::cmpLoad<64, 32>;
    } else {
      cmpMove_CRRRR = &MacroAssemblerX64::cmpMove<64, 64>;
      cmpMove_CRARR = &MacroAssemblerX64::cmpMove<64, 64>;
      cmpLoad_CRRAR = &MacroAssemblerX64::cmpLoad<64, 64>;
      cmpLoad_CRAAR = &MacroAssemblerX64::cmpLoad<64, 64>;
    }
  }

  Register trueExprAndDest = ToRegister(ins->output());
  MOZ_ASSERT(ToRegister(ins->ifTrueExpr()) == trueExprAndDest,
             "true expr input is reused for output");

  Assembler::Condition cond = Assembler::InvertCondition(
      JSOpToCondition(ins->compareType(), ins->jsop()));
  const LAllocation* rhs = ins->rightExpr();
  const LAllocation* falseExpr = ins->ifFalseExpr();
  Register lhs = ToRegister(ins->leftExpr());

  // We generate one of four cmp+cmov pairings, depending on whether one of
  // the cmp args and one of the cmov args is in memory or a register.
  if (rhs->isRegister()) {
    if (falseExpr->isRegister()) {
      (masm.*cmpMove_CRRRR)(cond, lhs, ToRegister(rhs), ToRegister(falseExpr),
                            trueExprAndDest);
    } else {
      (masm.*cmpLoad_CRRAR)(cond, lhs, ToRegister(rhs), ToAddress(falseExpr),
                            trueExprAndDest);
    }
  } else {
    if (falseExpr->isRegister()) {
      (masm.*cmpMove_CRARR)(cond, lhs, ToAddress(rhs), ToRegister(falseExpr),
                            trueExprAndDest);
    } else {
      (masm.*cmpLoad_CRAAR)(cond, lhs, ToAddress(rhs), ToAddress(falseExpr),
                            trueExprAndDest);
    }
  }
}

void CodeGenerator::visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Double);
  MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Int64);
  masm.vmovq(ToRegister(lir->input()), ToFloatRegister(lir->output()));
}

void CodeGenerator::visitWasmReinterpretToI64(LWasmReinterpretToI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);
  MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Double);
  masm.vmovq(ToFloatRegister(lir->input()), ToRegister(lir->output()));
}

void CodeGenerator::visitWasmUint32ToDouble(LWasmUint32ToDouble* lir) {
  masm.convertUInt32ToDouble(ToRegister(lir->input()),
                             ToFloatRegister(lir->output()));
}

void CodeGenerator::visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir) {
  masm.convertUInt32ToFloat32(ToRegister(lir->input()),
                              ToFloatRegister(lir->output()));
}

void CodeGeneratorX64::wasmStore(const wasm::MemoryAccessDesc& access,
                                 const LAllocation* value, Operand dstAddr) {
  if (value->isConstant()) {
    masm.memoryBarrierBefore(access.sync());

    const MConstant* mir = value->toConstant();
    Imm32 cst =
        Imm32(mir->type() == MIRType::Int32 ? mir->toInt32() : mir->toInt64());

    masm.append(access, masm.size());
    switch (access.type()) {
      case Scalar::Int8:
      case Scalar::Uint8:
        masm.movb(cst, dstAddr);
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        masm.movw(cst, dstAddr);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        masm.movl(cst, dstAddr);
        break;
      case Scalar::Int64:
      case Scalar::Simd128:
      case Scalar::Float32:
      case Scalar::Float64:
      case Scalar::Uint8Clamped:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
    }

    masm.memoryBarrierAfter(access.sync());
  } else {
    masm.wasmStore(access, ToAnyRegister(value), dstAddr);
  }
}

void CodeGenerator::visitWasmHeapBase(LWasmHeapBase* ins) {
  MOZ_ASSERT(ins->instance()->isBogus());
  masm.movePtr(HeapReg, ToRegister(ins->output()));
}

template <typename T>
void CodeGeneratorX64::emitWasmLoad(T* ins) {
  const MWasmLoad* mir = ins->mir();

  uint32_t offset = mir->access().offset();
  MOZ_ASSERT(offset < masm.wasmMaxOffsetGuardLimit());

  // ptr is a GPR and is either a 32-bit value zero-extended to 64-bit, or a
  // true 64-bit value.
  const LAllocation* ptr = ins->ptr();
  Operand srcAddr = ptr->isBogus()
                        ? Operand(HeapReg, offset)
                        : Operand(HeapReg, ToRegister(ptr), TimesOne, offset);

  if (mir->type() == MIRType::Int64) {
    masm.wasmLoadI64(mir->access(), srcAddr, ToOutRegister64(ins));
  } else {
    masm.wasmLoad(mir->access(), srcAddr, ToAnyRegister(ins->output()));
  }
}

void CodeGenerator::visitWasmLoad(LWasmLoad* ins) { emitWasmLoad(ins); }

void CodeGenerator::visitWasmLoadI64(LWasmLoadI64* ins) { emitWasmLoad(ins); }

template <typename T>
void CodeGeneratorX64::emitWasmStore(T* ins) {
  const MWasmStore* mir = ins->mir();
  const wasm::MemoryAccessDesc& access = mir->access();

  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < masm.wasmMaxOffsetGuardLimit());

  const LAllocation* value = ins->getOperand(ins->ValueIndex);
  const LAllocation* ptr = ins->ptr();
  Operand dstAddr = ptr->isBogus()
                        ? Operand(HeapReg, offset)
                        : Operand(HeapReg, ToRegister(ptr), TimesOne, offset);

  wasmStore(access, value, dstAddr);
}

void CodeGenerator::visitWasmStore(LWasmStore* ins) { emitWasmStore(ins); }

void CodeGenerator::visitWasmStoreI64(LWasmStoreI64* ins) {
  MOZ_CRASH("Unused on this platform");
}

void CodeGenerator::visitWasmCompareExchangeHeap(
    LWasmCompareExchangeHeap* ins) {
  MWasmCompareExchangeHeap* mir = ins->mir();

  Register ptr = ToRegister(ins->ptr());
  Register oldval = ToRegister(ins->oldValue());
  Register newval = ToRegister(ins->newValue());
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  Scalar::Type accessType = mir->access().type();
  BaseIndex srcAddr(HeapReg, ptr, TimesOne, mir->access().offset());

  if (accessType == Scalar::Int64) {
    masm.wasmCompareExchange64(mir->access(), srcAddr, Register64(oldval),
                               Register64(newval), ToOutRegister64(ins));
  } else {
    masm.wasmCompareExchange(mir->access(), srcAddr, oldval, newval,
                             ToRegister(ins->output()));
  }
}

void CodeGenerator::visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins) {
  MWasmAtomicExchangeHeap* mir = ins->mir();

  Register ptr = ToRegister(ins->ptr());
  Register value = ToRegister(ins->value());
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  Scalar::Type accessType = mir->access().type();

  BaseIndex srcAddr(HeapReg, ptr, TimesOne, mir->access().offset());

  if (accessType == Scalar::Int64) {
    masm.wasmAtomicExchange64(mir->access(), srcAddr, Register64(value),
                              ToOutRegister64(ins));
  } else {
    masm.wasmAtomicExchange(mir->access(), srcAddr, value,
                            ToRegister(ins->output()));
  }
}

void CodeGenerator::visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins) {
  MWasmAtomicBinopHeap* mir = ins->mir();
  MOZ_ASSERT(mir->hasUses());

  Register ptr = ToRegister(ins->ptr());
  const LAllocation* value = ins->value();
  Register temp =
      ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());
  Register output = ToRegister(ins->output());
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  Scalar::Type accessType = mir->access().type();
  if (accessType == Scalar::Uint32) {
    accessType = Scalar::Int32;
  }

  AtomicOp op = mir->operation();
  BaseIndex srcAddr(HeapReg, ptr, TimesOne, mir->access().offset());

  if (accessType == Scalar::Int64) {
    Register64 val = Register64(ToRegister(value));
    Register64 out = Register64(output);
    Register64 tmp = Register64(temp);
    masm.wasmAtomicFetchOp64(mir->access(), op, val, srcAddr, tmp, out);
  } else if (value->isConstant()) {
    masm.wasmAtomicFetchOp(mir->access(), op, Imm32(ToInt32(value)), srcAddr,
                           temp, output);
  } else {
    masm.wasmAtomicFetchOp(mir->access(), op, ToRegister(value), srcAddr, temp,
                           output);
  }
}

void CodeGenerator::visitWasmAtomicBinopHeapForEffect(
    LWasmAtomicBinopHeapForEffect* ins) {
  MWasmAtomicBinopHeap* mir = ins->mir();
  MOZ_ASSERT(!mir->hasUses());

  Register ptr = ToRegister(ins->ptr());
  const LAllocation* value = ins->value();
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  Scalar::Type accessType = mir->access().type();
  AtomicOp op = mir->operation();

  BaseIndex srcAddr(HeapReg, ptr, TimesOne, mir->access().offset());

  if (accessType == Scalar::Int64) {
    Register64 val = Register64(ToRegister(value));
    masm.wasmAtomicEffectOp64(mir->access(), op, val, srcAddr);
  } else if (value->isConstant()) {
    Imm32 c(0);
    if (value->toConstant()->type() == MIRType::Int64) {
      c = Imm32(ToInt64(value));
    } else {
      c = Imm32(ToInt32(value));
    }
    masm.wasmAtomicEffectOp(mir->access(), op, c, srcAddr, InvalidReg);
  } else {
    masm.wasmAtomicEffectOp(mir->access(), op, ToRegister(value), srcAddr,
                            InvalidReg);
  }
}

void CodeGenerator::visitTruncateDToInt32(LTruncateDToInt32* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  Register output = ToRegister(ins->output());

  // On x64, branchTruncateDouble uses vcvttsd2sq. Unlike the x86
  // implementation, this should handle most doubles and we can just
  // call a stub if it fails.
  emitTruncateDouble(input, output, ins->mir());
}

void CodeGenerator::visitWasmBuiltinTruncateDToInt32(
    LWasmBuiltinTruncateDToInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->getOperand(0));
  Register output = ToRegister(lir->getDef(0));

  emitTruncateDouble(input, output, lir->mir());
}

void CodeGenerator::visitWasmBuiltinTruncateFToInt32(
    LWasmBuiltinTruncateFToInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->getOperand(0));
  Register output = ToRegister(lir->getDef(0));

  emitTruncateFloat32(input, output, lir->mir());
}

void CodeGenerator::visitTruncateFToInt32(LTruncateFToInt32* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  Register output = ToRegister(ins->output());

  // On x64, branchTruncateFloat32 uses vcvttss2sq. Unlike the x86
  // implementation, this should handle most floats and we can just
  // call a stub if it fails.
  emitTruncateFloat32(input, output, ins->mir());
}

void CodeGenerator::visitWrapInt64ToInt32(LWrapInt64ToInt32* lir) {
  const LAllocation* input = lir->getOperand(0);
  Register output = ToRegister(lir->output());

  if (lir->mir()->bottomHalf()) {
    masm.movl(ToOperand(input), output);
  } else {
    MOZ_CRASH("Not implemented.");
  }
}

void CodeGenerator::visitExtendInt32ToInt64(LExtendInt32ToInt64* lir) {
  const LAllocation* input = lir->getOperand(0);
  Register output = ToRegister(lir->output());

  if (lir->mir()->isUnsigned()) {
    masm.movl(ToOperand(input), output);
  } else {
    masm.movslq(ToOperand(input), output);
  }
}

void CodeGenerator::visitWasmExtendU32Index(LWasmExtendU32Index* lir) {
  // Generates no code on this platform because the input is assumed to have
  // canonical form.
  Register output = ToRegister(lir->output());
  MOZ_ASSERT(ToRegister(lir->input()) == output);
  masm.debugAssertCanonicalInt32(output);
}

void CodeGenerator::visitWasmWrapU32Index(LWasmWrapU32Index* lir) {
  // Generates no code on this platform because the input is assumed to have
  // canonical form.
  Register output = ToRegister(lir->output());
  MOZ_ASSERT(ToRegister(lir->input()) == output);
  masm.debugAssertCanonicalInt32(output);
}

void CodeGenerator::visitSignExtendInt64(LSignExtendInt64* ins) {
  Register64 input = ToRegister64(ins->getInt64Operand(0));
  Register64 output = ToOutRegister64(ins);
  switch (ins->mode()) {
    case MSignExtendInt64::Byte:
      masm.movsbq(Operand(input.reg), output.reg);
      break;
    case MSignExtendInt64::Half:
      masm.movswq(Operand(input.reg), output.reg);
      break;
    case MSignExtendInt64::Word:
      masm.movslq(Operand(input.reg), output.reg);
      break;
  }
}

void CodeGenerator::visitWasmTruncateToInt64(LWasmTruncateToInt64* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register64 output = ToOutRegister64(lir);

  MWasmTruncateToInt64* mir = lir->mir();
  MIRType inputType = mir->input()->type();

  MOZ_ASSERT(inputType == MIRType::Double || inputType == MIRType::Float32);

  auto* ool = new (alloc()) OutOfLineWasmTruncateCheck(mir, input, output);
  addOutOfLineCode(ool, mir);

  FloatRegister temp =
      mir->isUnsigned() ? ToFloatRegister(lir->temp()) : InvalidFloatReg;

  Label* oolEntry = ool->entry();
  Label* oolRejoin = ool->rejoin();
  bool isSaturating = mir->isSaturating();
  if (inputType == MIRType::Double) {
    if (mir->isUnsigned()) {
      masm.wasmTruncateDoubleToUInt64(input, output, isSaturating, oolEntry,
                                      oolRejoin, temp);
    } else {
      masm.wasmTruncateDoubleToInt64(input, output, isSaturating, oolEntry,
                                     oolRejoin, temp);
    }
  } else {
    if (mir->isUnsigned()) {
      masm.wasmTruncateFloat32ToUInt64(input, output, isSaturating, oolEntry,
                                       oolRejoin, temp);
    } else {
      masm.wasmTruncateFloat32ToInt64(input, output, isSaturating, oolEntry,
                                      oolRejoin, temp);
    }
  }
}

void CodeGenerator::visitInt64ToFloatingPoint(LInt64ToFloatingPoint* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  FloatRegister output = ToFloatRegister(lir->output());

  MInt64ToFloatingPoint* mir = lir->mir();
  bool isUnsigned = mir->isUnsigned();

  MIRType outputType = mir->type();
  MOZ_ASSERT(outputType == MIRType::Double || outputType == MIRType::Float32);
  MOZ_ASSERT(isUnsigned == !lir->getTemp(0)->isBogusTemp());

  if (outputType == MIRType::Double) {
    if (isUnsigned) {
      masm.convertUInt64ToDouble(input, output, ToRegister(lir->getTemp(0)));
    } else {
      masm.convertInt64ToDouble(input, output);
    }
  } else {
    if (isUnsigned) {
      masm.convertUInt64ToFloat32(input, output, ToRegister(lir->getTemp(0)));
    } else {
      masm.convertInt64ToFloat32(input, output);
    }
  }
}

void CodeGenerator::visitNotI64(LNotI64* lir) {
  masm.cmpq(Imm32(0), ToRegister(lir->input()));
  masm.emitSet(Assembler::Equal, ToRegister(lir->output()));
}

void CodeGenerator::visitClzI64(LClzI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);
  masm.clz64(input, output.reg);
}

void CodeGenerator::visitCtzI64(LCtzI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);
  masm.ctz64(input, output.reg);
}

void CodeGenerator::visitBitNotI64(LBitNotI64* ins) {
  const LAllocation* input = ins->getOperand(0);
  MOZ_ASSERT(!input->isConstant());
  Register inputR = ToRegister(input);
  MOZ_ASSERT(inputR == ToRegister(ins->output()));
  masm.notq(inputR);
}

void CodeGenerator::visitTestI64AndBranch(LTestI64AndBranch* lir) {
  Register input = ToRegister(lir->input());
  masm.testq(input, input);
  emitBranch(Assembler::NonZero, lir->ifTrue(), lir->ifFalse());
}
