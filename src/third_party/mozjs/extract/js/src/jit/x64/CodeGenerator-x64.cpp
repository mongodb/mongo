/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x64/CodeGenerator-x64.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"

#include "jit/CodeGenerator.h"
#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "js/ScalarType.h"  // js::Scalar::Type

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

CodeGeneratorX64::CodeGeneratorX64(MIRGenerator* gen, LIRGraph* graph,
                                   MacroAssembler* masm,
                                   const wasm::CodeMetadata* wasmCodeMeta)
    : CodeGeneratorX86Shared(gen, graph, masm, wasmCodeMeta) {}

Operand CodeGeneratorX64::ToOperand64(const LInt64Allocation& a64) {
  const LAllocation& a = a64.value();
  MOZ_ASSERT(!a.isFloatReg());
  if (a.isGeneralReg()) {
    return Operand(a.toGeneralReg()->reg());
  }
  return Operand(ToAddress(a));
}

void CodeGenerator::visitBox(LBox* box) {
  const LAllocation* in = box->payload();
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
    ValueOperand value = ToValue(unbox->input());
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
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->trapSiteDesc());
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
      masm.wasmTrap(wasm::Trap::IntegerOverflow, lir->trapSiteDesc());
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
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->trapSiteDesc());
    masm.bind(&nonZero);
  }

  // Zero extend the lhs into rdx to make (rdx:rax).
  masm.xorl(rdx, rdx);
  masm.udivq(rhs);

  masm.bind(&done);
}

void CodeGeneratorX64::emitBigIntPtrDiv(LBigIntPtrDiv* ins, Register dividend,
                                        Register divisor, Register output) {
  // Callers handle division by zero and integer overflow.

  MOZ_ASSERT(ToRegister(ins->temp0()) == rdx);
  MOZ_ASSERT(output == rax);

  if (dividend != rax) {
    masm.movePtr(dividend, rax);
  }

  // Sign extend the lhs into rdx to make rdx:rax.
  masm.cqo();

  masm.idivq(divisor);
}

void CodeGeneratorX64::emitBigIntPtrMod(LBigIntPtrMod* ins, Register dividend,
                                        Register divisor, Register output) {
  // Callers handle division by zero and integer overflow.

  MOZ_ASSERT(dividend == rax);
  MOZ_ASSERT(output == rdx);

  // Sign extend the lhs into rdx to make rdx:rax.
  masm.cqo();

  masm.idivq(divisor);
}

void CodeGenerator::visitAtomicLoad64(LAtomicLoad64* lir) {
  Register elements = ToRegister(lir->elements());
  Register64 out = ToOutRegister64(lir);

  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

  // NOTE: the generated code must match the assembly code in gen_load in
  // GenerateAtomicOperations.py
  auto sync = Synchronization::Load();

  masm.memoryBarrierBefore(sync);
  if (lir->index()->isConstant()) {
    Address source =
        ToAddress(elements, lir->index(), storageType, mir->offsetAdjustment());
    masm.load64(source, out);
  } else {
    BaseIndex source(elements, ToRegister(lir->index()),
                     ScaleFromScalarType(storageType), mir->offsetAdjustment());
    masm.load64(source, out);
  }
  masm.memoryBarrierAfter(sync);
}

void CodeGenerator::visitAtomicStore64(LAtomicStore64* lir) {
  Register elements = ToRegister(lir->elements());
  Register64 value = ToRegister64(lir->value());

  Scalar::Type writeType = lir->mir()->writeType();

  // NOTE: the generated code must match the assembly code in gen_store in
  // GenerateAtomicOperations.py
  auto sync = Synchronization::Store();

  masm.memoryBarrierBefore(sync);
  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), writeType);
    masm.store64(value, dest);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(writeType));
    masm.store64(value, dest);
  }
  masm.memoryBarrierAfter(sync);
}

void CodeGenerator::visitCompareExchangeTypedArrayElement64(
    LCompareExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register64 oldval = ToRegister64(lir->oldval());
  Register64 newval = ToRegister64(lir->newval());
  Register64 out = ToOutRegister64(lir);

  MOZ_ASSERT(out.reg == rax);

  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.compareExchange64(Synchronization::Full(), dest, oldval, newval, out);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.compareExchange64(Synchronization::Full(), dest, oldval, newval, out);
  }
}

void CodeGenerator::visitAtomicExchangeTypedArrayElement64(
    LAtomicExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register64 value = ToRegister64(lir->value());
  Register64 out = ToOutRegister64(lir);

  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicExchange64(Synchronization::Full(), dest, value, out);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicExchange64(Synchronization::Full(), dest, value, out);
  }
}

void CodeGenerator::visitAtomicTypedArrayElementBinop64(
    LAtomicTypedArrayElementBinop64* lir) {
  MOZ_ASSERT(!lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register64 value = ToRegister64(lir->value());
  Register64 temp = ToTempRegister64OrInvalid(lir->temp0());
  Register64 out = ToOutRegister64(lir);

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  // Add and Sub don't need |temp| and can save a `mov` when the value and
  // output register are equal to each other.
  if (atomicOp == AtomicOp::Add || atomicOp == AtomicOp::Sub) {
    MOZ_ASSERT(temp == Register64::Invalid());
    MOZ_ASSERT(value == out);
  } else {
    MOZ_ASSERT(out.reg == rax);
  }

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, value, dest, temp,
                         out);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, value, dest, temp,
                         out);
  }
}

void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect64(
    LAtomicTypedArrayElementBinopForEffect64* lir) {
  MOZ_ASSERT(lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register64 value = ToRegister64(lir->value());

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicEffectOp64(Synchronization::Full(), atomicOp, value, dest);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicEffectOp64(Synchronization::Full(), atomicOp, value, dest);
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
  if (rhs->isGeneralReg()) {
    if (falseExpr->isGeneralReg()) {
      (masm.*cmpMove_CRRRR)(cond, lhs, ToRegister(rhs), ToRegister(falseExpr),
                            trueExprAndDest);
    } else {
      (masm.*cmpLoad_CRRAR)(cond, lhs, ToRegister(rhs), ToAddress(falseExpr),
                            trueExprAndDest);
    }
  } else {
    if (falseExpr->isGeneralReg()) {
      (masm.*cmpMove_CRARR)(cond, lhs, ToAddress(rhs), ToRegister(falseExpr),
                            trueExprAndDest);
    } else {
      (masm.*cmpLoad_CRAAR)(cond, lhs, ToAddress(rhs), ToAddress(falseExpr),
                            trueExprAndDest);
    }
  }
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

    switch (access.type()) {
      case Scalar::Int8:
      case Scalar::Uint8:
        masm.append(access, wasm::TrapMachineInsn::Store8,
                    FaultingCodeOffset(masm.currentOffset()));
        masm.movb(cst, dstAddr);
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        masm.append(access, wasm::TrapMachineInsn::Store16,
                    FaultingCodeOffset(masm.currentOffset()));
        masm.movw(cst, dstAddr);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        masm.append(access, wasm::TrapMachineInsn::Store32,
                    FaultingCodeOffset(masm.currentOffset()));
        masm.movl(cst, dstAddr);
        break;
      case Scalar::Int64:
        MOZ_ASSERT_IF(mir->type() == MIRType::Int64,
                      mozilla::CheckedInt32(mir->toInt64()).isValid());
        masm.append(access, wasm::TrapMachineInsn::Store64,
                    FaultingCodeOffset(masm.currentOffset()));
        masm.movq(cst, dstAddr);
        break;
      case Scalar::Simd128:
      case Scalar::Float16:
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

template <typename T>
void CodeGeneratorX64::emitWasmLoad(T* ins) {
  const MWasmLoad* mir = ins->mir();

  mir->access().assertOffsetInGuardPages();
  uint32_t offset = mir->access().offset32();

  // ptr is a GPR and is either a 32-bit value zero-extended to 64-bit, or a
  // true 64-bit value.
  const LAllocation* ptr = ins->ptr();
  Register memoryBase = ToRegister(ins->memoryBase());
  Operand srcAddr =
      ptr->isBogus() ? Operand(memoryBase, offset)
                     : Operand(memoryBase, ToRegister(ptr), TimesOne, offset);

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

  mir->access().assertOffsetInGuardPages();
  uint32_t offset = access.offset32();

  const LAllocation* value = ins->value();
  const LAllocation* ptr = ins->ptr();
  Register memoryBase = ToRegister(ins->memoryBase());
  Operand dstAddr =
      ptr->isBogus() ? Operand(memoryBase, offset)
                     : Operand(memoryBase, ToRegister(ptr), TimesOne, offset);

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
  Register memoryBase = ToRegister(ins->memoryBase());

  Scalar::Type accessType = mir->access().type();
  BaseIndex srcAddr(memoryBase, ptr, TimesOne, mir->access().offset32());

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
  Register memoryBase = ToRegister(ins->memoryBase());

  Scalar::Type accessType = mir->access().type();

  BaseIndex srcAddr(memoryBase, ptr, TimesOne, mir->access().offset32());

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
  Register memoryBase = ToRegister(ins->memoryBase());
  const LAllocation* value = ins->value();
  Register temp = ToTempRegisterOrInvalid(ins->temp0());
  Register output = ToRegister(ins->output());

  Scalar::Type accessType = mir->access().type();
  if (accessType == Scalar::Uint32) {
    accessType = Scalar::Int32;
  }

  AtomicOp op = mir->operation();
  BaseIndex srcAddr(memoryBase, ptr, TimesOne, mir->access().offset32());

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
  Register memoryBase = ToRegister(ins->memoryBase());
  const LAllocation* value = ins->value();

  Scalar::Type accessType = mir->access().type();
  AtomicOp op = mir->operation();

  BaseIndex srcAddr(memoryBase, ptr, TimesOne, mir->access().offset32());

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

class js::jit::OutOfLineTruncate : public OutOfLineCodeBase<CodeGeneratorX64> {
  FloatRegister input_;
  Register output_;
  Register temp_;

 public:
  OutOfLineTruncate(FloatRegister input, Register output, Register temp)
      : input_(input), output_(output), temp_(temp) {}

  void accept(CodeGeneratorX64* codegen) override {
    codegen->visitOutOfLineTruncate(this);
  }

  FloatRegister input() const { return input_; }
  Register output() const { return output_; }
  Register temp() const { return temp_; }
};

void CodeGeneratorX64::visitOutOfLineTruncate(OutOfLineTruncate* ool) {
  FloatRegister input = ool->input();
  Register output = ool->output();
  Register temp = ool->temp();

  // Inline implementation of `JS::ToInt32(double)` for double values whose
  // exponent is ≥63.

#ifdef DEBUG
  Label ok;
  masm.branchTruncateDoubleMaybeModUint32(input, output, &ok);
  masm.assumeUnreachable("OOL path only used when vcvttsd2sq failed");
  masm.bind(&ok);
#endif

  constexpr uint32_t ShiftedExponentBits =
      mozilla::FloatingPoint<double>::kExponentBits >>
      mozilla::FloatingPoint<double>::kExponentShift;
  static_assert(ShiftedExponentBits == 0x7ff);

  constexpr uint32_t ExponentBiasAndShift =
      mozilla::FloatingPoint<double>::kExponentBias +
      mozilla::FloatingPoint<double>::kExponentShift;
  static_assert(ExponentBiasAndShift == (1023 + 52));

  constexpr size_t ResultWidth = CHAR_BIT * sizeof(int32_t);

  // Extract the bit representation of |input|.
  masm.moveDoubleToGPR64(input, Register64(output));

  // Extract the exponent.
  masm.rshiftPtr(Imm32(mozilla::FloatingPoint<double>::kExponentShift), output,
                 temp);
  masm.and32(Imm32(ShiftedExponentBits), temp);
#ifdef DEBUG
  // The biased exponent must be at least `1023 + 63`, because otherwise
  // vcvttsd2sq wouldn't have failed.
  constexpr uint32_t MinBiasedExponent =
      mozilla::FloatingPoint<double>::kExponentBias + 63;

  Label exponentOk;
  masm.branch32(Assembler::GreaterThanOrEqual, temp, Imm32(MinBiasedExponent),
                &exponentOk);
  masm.assumeUnreachable("exponent is greater-than-or-equals to 63");
  masm.bind(&exponentOk);
#endif
  masm.sub32(Imm32(ExponentBiasAndShift), temp);

  // If the exponent is greater than or equal to |ResultWidth|, the number is
  // either infinite, NaN, or too large to have lower-order bits. We have to
  // return zero in this case.
  {
    ScratchRegisterScope scratch(masm);
    masm.movePtr(ImmWord(0), scratch);
    masm.cmp32MovePtr(Assembler::AboveOrEqual, temp, Imm32(ResultWidth),
                      scratch, output);
  }

  // Negate if the sign bit is set.
  {
    ScratchRegisterScope scratch(masm);
    masm.movePtr(output, scratch);
    masm.negPtr(scratch);
    masm.testPtr(output, output);
    masm.cmovCCq(Assembler::Signed, scratch, output);
  }

  // The significand contains the bits that will determine the final result.
  // Shift those bits left by the exponent value in |temp|.
  masm.lshift32(temp, output);

  // Return from OOL path.
  masm.jump(ool->rejoin());
}

void CodeGenerator::visitTruncateDToInt32(LTruncateDToInt32* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  Register output = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp0());

  auto* ool = new (alloc()) OutOfLineTruncate(input, output, temp);
  addOutOfLineCode(ool, ins->mir());

  masm.branchTruncateDoubleMaybeModUint32(input, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitWasmBuiltinTruncateDToInt32(
    LWasmBuiltinTruncateDToInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());
  Register temp = ToRegister(lir->temp0());
  MOZ_ASSERT(lir->instance()->isBogus(), "instance not used for x64");

  auto* ool = new (alloc()) OutOfLineTruncate(input, output, temp);
  addOutOfLineCode(ool, lir->mir());

  masm.branchTruncateDoubleMaybeModUint32(input, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitWasmBuiltinTruncateFToInt32(
    LWasmBuiltinTruncateFToInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());
  MOZ_ASSERT(lir->instance()->isBogus(), "instance not used for x64");

  masm.truncateFloat32ModUint32(input, output);
}

void CodeGenerator::visitTruncateFToInt32(LTruncateFToInt32* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.truncateFloat32ModUint32(input, output);
}

void CodeGenerator::visitWrapInt64ToInt32(LWrapInt64ToInt32* lir) {
  LInt64Allocation input = lir->input();
  Register output = ToRegister(lir->output());

  if (lir->mir()->bottomHalf()) {
    if (input.value().isMemory()) {
      masm.load32(ToAddress(input), output);
    } else {
      masm.move64To32(ToRegister64(input), output);
    }
  } else {
    MOZ_CRASH("Not implemented.");
  }
}

void CodeGenerator::visitExtendInt32ToInt64(LExtendInt32ToInt64* lir) {
  const LAllocation* input = lir->input();
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
  Register64 input = ToRegister64(ins->input());
  Register64 output = ToOutRegister64(ins);
  switch (ins->mir()->mode()) {
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
      mir->isUnsigned() ? ToFloatRegister(lir->temp0()) : InvalidFloatReg;

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
  Register64 input = ToRegister64(lir->input());
  FloatRegister output = ToFloatRegister(lir->output());
  Register temp = ToTempRegisterOrInvalid(lir->temp0());

  MInt64ToFloatingPoint* mir = lir->mir();
  bool isUnsigned = mir->isUnsigned();

  MIRType outputType = mir->type();
  MOZ_ASSERT(outputType == MIRType::Double || outputType == MIRType::Float32);
  MOZ_ASSERT(isUnsigned == (temp != InvalidReg));

  if (outputType == MIRType::Double) {
    if (isUnsigned) {
      masm.convertUInt64ToDouble(input, output, temp);
    } else {
      masm.convertInt64ToDouble(input, output);
    }
  } else {
    if (isUnsigned) {
      masm.convertUInt64ToFloat32(input, output, temp);
    } else {
      masm.convertInt64ToFloat32(input, output);
    }
  }
}

void CodeGenerator::visitBitNotI64(LBitNotI64* ins) {
  LInt64Allocation input = ins->input();
  MOZ_ASSERT(!IsConstant(input));
  Register64 inputR = ToRegister64(input);
  MOZ_ASSERT(inputR == ToOutRegister64(ins));
  masm.notq(inputR.reg);
}
