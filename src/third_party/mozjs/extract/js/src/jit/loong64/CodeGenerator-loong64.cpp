/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/loong64/CodeGenerator-loong64.h"

#include "mozilla/MathAlgorithms.h"

#include "jsnum.h"

#include "jit/CodeGenerator.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitRuntime.h"
#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "vm/Shape.h"

#include "jit/shared/CodeGenerator-shared-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using JS::GenericNaN;
using mozilla::FloorLog2;
using mozilla::NegativeInfinity;

// shared
CodeGeneratorLOONG64::CodeGeneratorLOONG64(
    MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm,
    const wasm::CodeMetadata* wasmCodeMeta)
    : CodeGeneratorShared(gen, graph, masm, wasmCodeMeta) {}

Operand CodeGeneratorLOONG64::ToOperand(const LAllocation& a) {
  if (a.isGeneralReg()) {
    return Operand(a.toGeneralReg()->reg());
  }
  if (a.isFloatReg()) {
    return Operand(a.toFloatReg()->reg());
  }
  return Operand(ToAddress(a));
}

Operand CodeGeneratorLOONG64::ToOperand(const LAllocation* a) {
  return ToOperand(*a);
}

Operand CodeGeneratorLOONG64::ToOperand(const LDefinition* def) {
  return ToOperand(def->output());
}

#ifdef JS_PUNBOX64
Operand CodeGeneratorLOONG64::ToOperandOrRegister64(
    const LInt64Allocation& input) {
  return ToOperand(input.value());
}
#else
Register64 CodeGeneratorLOONG64::ToOperandOrRegister64(
    const LInt64Allocation& input) {
  return ToRegister64(input);
}
#endif

void CodeGeneratorLOONG64::branchToBlock(Assembler::FloatFormat fmt,
                                         FloatRegister lhs, FloatRegister rhs,
                                         MBasicBlock* mir,
                                         Assembler::DoubleCondition cond) {
  // Skip past trivial blocks.
  Label* label = skipTrivialBlocks(mir)->lir()->label();
  if (fmt == Assembler::DoubleFloat) {
    masm.branchDouble(cond, lhs, rhs, label);
  } else {
    masm.branchFloat(cond, lhs, rhs, label);
  }
}

MoveOperand CodeGeneratorLOONG64::toMoveOperand(LAllocation a) const {
  if (a.isGeneralReg()) {
    return MoveOperand(ToRegister(a));
  }
  if (a.isFloatReg()) {
    return MoveOperand(ToFloatRegister(a));
  }
  MoveOperand::Kind kind = a.isStackArea() ? MoveOperand::Kind::EffectiveAddress
                                           : MoveOperand::Kind::Memory;
  Address address = ToAddress(a);
  MOZ_ASSERT((address.offset & 3) == 0);

  return MoveOperand(address, kind);
}

void CodeGeneratorLOONG64::bailoutFrom(Label* label, LSnapshot* snapshot) {
  MOZ_ASSERT_IF(!masm.oom(), label->used());
  MOZ_ASSERT_IF(!masm.oom(), !label->bound());

  encode(snapshot);

  InlineScriptTree* tree = snapshot->mir()->block()->trackedTree();
  auto* ool = new (alloc()) LambdaOutOfLineCode([=](OutOfLineCode& ool) {
    // Push snapshotOffset and make sure stack is aligned.
    masm.subPtr(Imm32(sizeof(Value)), StackPointer);
    masm.storePtr(ImmWord(snapshot->snapshotOffset()),
                  Address(StackPointer, 0));

    masm.jump(&deoptLabel_);
  });
  addOutOfLineCode(ool,
                   new (alloc()) BytecodeSite(tree, tree->script()->code()));

  masm.retarget(label, ool->entry());
}

void CodeGeneratorLOONG64::bailout(LSnapshot* snapshot) {
  Label label;
  masm.jump(&label);
  bailoutFrom(&label, snapshot);
}

bool CodeGeneratorLOONG64::generateOutOfLineCode() {
  if (!CodeGeneratorShared::generateOutOfLineCode()) {
    return false;
  }

  if (deoptLabel_.used()) {
    // All non-table-based bailouts will go here.
    masm.bind(&deoptLabel_);

    // Push the frame size, so the handler can recover the IonScript.
    // Frame size is stored in 'ra' and pushed by GenerateBailoutThunk
    // We have to use 'ra' because generateBailoutTable will implicitly do
    // the same.
    masm.move32(Imm32(frameSize()), ra);

    TrampolinePtr handler = gen->jitRuntime()->getGenericBailoutHandler();
    masm.jump(handler);
  }

  return !masm.oom();
}

class js::jit::OutOfLineTableSwitch
    : public OutOfLineCodeBase<CodeGeneratorLOONG64> {
  MTableSwitch* mir_;
  CodeLabel jumpLabel_;

  void accept(CodeGeneratorLOONG64* codegen) {
    codegen->visitOutOfLineTableSwitch(this);
  }

 public:
  OutOfLineTableSwitch(MTableSwitch* mir) : mir_(mir) {}

  MTableSwitch* mir() const { return mir_; }

  CodeLabel* jumpLabel() { return &jumpLabel_; }
};

void CodeGeneratorLOONG64::emitTableSwitchDispatch(MTableSwitch* mir,
                                                   Register index,
                                                   Register base) {
  Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

  // Lower value with low value
  if (mir->low() != 0) {
    masm.subPtr(Imm32(mir->low()), index);
  }

  // Jump to default case if input is out of range
  int32_t cases = mir->numCases();
  masm.branchPtr(Assembler::AboveOrEqual, index, ImmWord(cases), defaultcase);

  // To fill in the CodeLabels for the case entries, we need to first
  // generate the case entries (we don't yet know their offsets in the
  // instruction stream).
  OutOfLineTableSwitch* ool = new (alloc()) OutOfLineTableSwitch(mir);
  addOutOfLineCode(ool, mir);

  // Compute the position where a pointer to the right case stands.
  masm.ma_li(base, ool->jumpLabel());

  BaseIndex pointer(base, index, ScalePointer);

  // Jump to the right case
  masm.branchToComputedAddress(pointer);
}

template <typename T>
void CodeGeneratorLOONG64::emitWasmLoad(T* lir) {
  const MWasmLoad* mir = lir->mir();
  SecondScratchRegisterScope scratch2(masm);

  Register memoryBase = ToRegister(lir->memoryBase());
  Register ptr = ToRegister(lir->ptr());
  Register ptrScratch = ToTempRegisterOrInvalid(lir->temp0());

  if (mir->base()->type() == MIRType::Int32) {
    masm.move32To64ZeroExtend(ptr, Register64(scratch2));
    ptr = scratch2;
    ptrScratch = ptrScratch != InvalidReg ? scratch2 : InvalidReg;
  }

  // ptr is a GPR and is either a 32-bit value zero-extended to 64-bit, or a
  // true 64-bit value.
  masm.wasmLoad(mir->access(), memoryBase, ptr, ptrScratch,
                ToAnyRegister(lir->output()));
}

template <typename T>
void CodeGeneratorLOONG64::emitWasmStore(T* lir) {
  const MWasmStore* mir = lir->mir();
  SecondScratchRegisterScope scratch2(masm);

  Register memoryBase = ToRegister(lir->memoryBase());
  Register ptr = ToRegister(lir->ptr());
  Register ptrScratch = ToTempRegisterOrInvalid(lir->temp0());

  if (mir->base()->type() == MIRType::Int32) {
    masm.move32To64ZeroExtend(ptr, Register64(scratch2));
    ptr = scratch2;
    ptrScratch = ptrScratch != InvalidReg ? scratch2 : InvalidReg;
  }

  // ptr is a GPR and is either a 32-bit value zero-extended to 64-bit, or a
  // true 64-bit value.
  masm.wasmStore(mir->access(), ToAnyRegister(lir->value()), memoryBase, ptr,
                 ptrScratch);
}

void CodeGeneratorLOONG64::generateInvalidateEpilogue() {
  // Ensure that there is enough space in the buffer for the OsiPoint
  // patching to occur. Otherwise, we could overwrite the invalidation
  // epilogue
  for (size_t i = 0; i < sizeof(void*); i += Assembler::NopSize()) {
    masm.nop();
  }

  masm.bind(&invalidate_);

  // Push the return address of the point that we bailed out at to the stack
  masm.Push(ra);

  // Push the Ion script onto the stack (when we determine what that
  // pointer is).
  invalidateEpilogueData_ = masm.pushWithPatch(ImmWord(uintptr_t(-1)));

  // Jump to the invalidator which will replace the current frame.
  TrampolinePtr thunk = gen->jitRuntime()->getInvalidationThunk();

  masm.jump(thunk);
}

void CodeGeneratorLOONG64::visitOutOfLineTableSwitch(
    OutOfLineTableSwitch* ool) {
  MTableSwitch* mir = ool->mir();

  masm.haltingAlign(sizeof(void*));
  masm.bind(ool->jumpLabel());
  masm.addCodeLabel(*ool->jumpLabel());

  for (size_t i = 0; i < mir->numCases(); i++) {
    LBlock* caseblock = skipTrivialBlocks(mir->getCase(i))->lir();
    Label* caseheader = caseblock->label();
    uint32_t caseoffset = caseheader->offset();

    // The entries of the jump table need to be absolute addresses and thus
    // must be patched after codegen is finished.
    CodeLabel cl;
    masm.writeCodePointer(&cl);
    cl.target()->bind(caseoffset);
    masm.addCodeLabel(cl);
  }
}

void CodeGeneratorLOONG64::visitOutOfLineWasmTruncateCheck(
    OutOfLineWasmTruncateCheck* ool) {
  if (ool->toType() == MIRType::Int32) {
    masm.outOfLineWasmTruncateToInt32Check(ool->input(), ool->output(),
                                           ool->fromType(), ool->flags(),
                                           ool->rejoin(), ool->trapSiteDesc());
  } else {
    MOZ_ASSERT(ool->toType() == MIRType::Int64);
    masm.outOfLineWasmTruncateToInt64Check(ool->input(), ool->output64(),
                                           ool->fromType(), ool->flags(),
                                           ool->rejoin(), ool->trapSiteDesc());
  }
}

void CodeGenerator::visitBox(LBox* box) {
  const LAllocation* in = box->payload();
  ValueOperand result = ToOutValue(box);

  masm.moveValue(TypedOrValueRegister(box->type(), ToAnyRegister(in)), result);
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

  LAllocation* input = unbox->getOperand(LUnbox::Input);
  if (input->isGeneralReg()) {
    Register inputReg = ToRegister(input);
    switch (mir->type()) {
      case MIRType::Int32:
        masm.unboxInt32(inputReg, result);
        break;
      case MIRType::Boolean:
        masm.unboxBoolean(inputReg, result);
        break;
      case MIRType::Object:
        masm.unboxObject(inputReg, result);
        break;
      case MIRType::String:
        masm.unboxString(inputReg, result);
        break;
      case MIRType::Symbol:
        masm.unboxSymbol(inputReg, result);
        break;
      case MIRType::BigInt:
        masm.unboxBigInt(inputReg, result);
        break;
      default:
        MOZ_CRASH("Given MIRType cannot be unboxed.");
    }
    return;
  }

  Address inputAddr = ToAddress(input);
  switch (mir->type()) {
    case MIRType::Int32:
      masm.unboxInt32(inputAddr, result);
      break;
    case MIRType::Boolean:
      masm.unboxBoolean(inputAddr, result);
      break;
    case MIRType::Object:
      masm.unboxObject(inputAddr, result);
      break;
    case MIRType::String:
      masm.unboxString(inputAddr, result);
      break;
    case MIRType::Symbol:
      masm.unboxSymbol(inputAddr, result);
      break;
    case MIRType::BigInt:
      masm.unboxBigInt(inputAddr, result);
      break;
    default:
      MOZ_CRASH("Given MIRType cannot be unboxed.");
  }
}

void CodeGenerator::visitDivOrModI64(LDivOrModI64* lir) {
  Register lhs = ToRegister(lir->lhs());
  Register rhs = ToRegister(lir->rhs());
  Register output = ToRegister(lir->output());

  Label done;

  // Handle divide by zero.
  if (lir->canBeDivideByZero()) {
    Label nonZero;
    masm.ma_b(rhs, rhs, &nonZero, Assembler::NonZero);
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->trapSiteDesc());
    masm.bind(&nonZero);
  }

  // Handle an integer overflow exception from INT64_MIN / -1.
  if (lir->canBeNegativeOverflow()) {
    Label notOverflow;
    masm.branchPtr(Assembler::NotEqual, lhs, ImmWord(INT64_MIN), &notOverflow);
    masm.branchPtr(Assembler::NotEqual, rhs, ImmWord(-1), &notOverflow);
    if (lir->mir()->isMod()) {
      masm.as_xor(output, output, output);
    } else {
      masm.wasmTrap(wasm::Trap::IntegerOverflow, lir->trapSiteDesc());
    }
    masm.jump(&done);
    masm.bind(&notOverflow);
  }

  if (lir->mir()->isMod()) {
    masm.as_mod_d(output, lhs, rhs);
  } else {
    masm.as_div_d(output, lhs, rhs);
  }

  masm.bind(&done);
}

void CodeGenerator::visitUDivOrModI64(LUDivOrModI64* lir) {
  Register lhs = ToRegister(lir->lhs());
  Register rhs = ToRegister(lir->rhs());
  Register output = ToRegister(lir->output());

  Label done;

  // Prevent divide by zero.
  if (lir->canBeDivideByZero()) {
    Label nonZero;
    masm.ma_b(rhs, rhs, &nonZero, Assembler::NonZero);
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->trapSiteDesc());
    masm.bind(&nonZero);
  }

  if (lir->mir()->isMod()) {
    masm.as_mod_du(output, lhs, rhs);
  } else {
    masm.as_div_du(output, lhs, rhs);
  }

  masm.bind(&done);
}

void CodeGeneratorLOONG64::emitBigIntPtrDiv(LBigIntPtrDiv* ins,
                                            Register dividend, Register divisor,
                                            Register output) {
  // Callers handle division by zero and integer overflow.
  masm.as_div_d(/* result= */ output, dividend, divisor);
}

void CodeGeneratorLOONG64::emitBigIntPtrMod(LBigIntPtrMod* ins,
                                            Register dividend, Register divisor,
                                            Register output) {
  // Callers handle division by zero and integer overflow.
  masm.as_mod_d(/* result= */ output, dividend, divisor);
}

void CodeGenerator::visitWasmLoadI64(LWasmLoadI64* lir) {
  const MWasmLoad* mir = lir->mir();

  Register memoryBase = ToRegister(lir->memoryBase());
  Register ptrScratch = ToTempRegisterOrInvalid(lir->temp0());

  Register ptrReg = ToRegister(lir->ptr());
  if (mir->base()->type() == MIRType::Int32) {
    // See comment in visitWasmLoad re the type of 'base'.
    masm.move32ZeroExtendToPtr(ptrReg, ptrReg);
  }

  masm.wasmLoadI64(mir->access(), memoryBase, ptrReg, ptrScratch,
                   ToOutRegister64(lir));
}

void CodeGenerator::visitWasmStoreI64(LWasmStoreI64* lir) {
  const MWasmStore* mir = lir->mir();

  Register memoryBase = ToRegister(lir->memoryBase());
  Register ptrScratch = ToTempRegisterOrInvalid(lir->temp0());

  Register ptrReg = ToRegister(lir->ptr());
  if (mir->base()->type() == MIRType::Int32) {
    // See comment in visitWasmLoad re the type of 'base'.
    masm.move32ZeroExtendToPtr(ptrReg, ptrReg);
  }

  masm.wasmStoreI64(mir->access(), ToRegister64(lir->value()), memoryBase,
                    ptrReg, ptrScratch);
}

void CodeGenerator::visitWasmSelectI64(LWasmSelectI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);

  Register cond = ToRegister(lir->condExpr());
  LInt64Allocation falseExpr = lir->falseExpr();

  Register64 out = ToOutRegister64(lir);
  MOZ_ASSERT(ToRegister64(lir->trueExpr()) == out,
             "true expr is reused for input");

  if (falseExpr.value().isGeneralReg()) {
    masm.moveIfZero(out.reg, ToRegister(falseExpr.value()), cond);
  } else {
    Label done;
    masm.ma_b(cond, cond, &done, Assembler::NonZero, ShortJump);
    masm.loadPtr(ToAddress(falseExpr.value()), out.reg);
    masm.bind(&done);
  }
}

void CodeGenerator::visitExtendInt32ToInt64(LExtendInt32ToInt64* lir) {
  const LAllocation* input = lir->input();
  Register output = ToRegister(lir->output());

  if (lir->mir()->isUnsigned()) {
    masm.as_bstrpick_d(output, ToRegister(input), 31, 0);
  } else {
    masm.as_slli_w(output, ToRegister(input), 0);
  }
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

void CodeGenerator::visitSignExtendInt64(LSignExtendInt64* lir) {
  Register64 input = ToRegister64(lir->input());
  Register64 output = ToOutRegister64(lir);
  switch (lir->mir()->mode()) {
    case MSignExtendInt64::Byte:
      masm.move32To64SignExtend(input.reg, output);
      masm.move8SignExtend(output.reg, output.reg);
      break;
    case MSignExtendInt64::Half:
      masm.move32To64SignExtend(input.reg, output);
      masm.move16SignExtend(output.reg, output.reg);
      break;
    case MSignExtendInt64::Word:
      masm.move32To64SignExtend(input.reg, output);
      break;
  }
}

void CodeGenerator::visitWasmExtendU32Index(LWasmExtendU32Index* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  MOZ_ASSERT(input == output);
  masm.move32To64ZeroExtend(input, Register64(output));
}

void CodeGenerator::visitWasmWrapU32Index(LWasmWrapU32Index* lir) {
  Register input = ToRegister(lir->input());
  Register output = ToRegister(lir->output());
  MOZ_ASSERT(input == output);
  masm.move64To32(Register64(input), output);
}

void CodeGenerator::visitWasmTruncateToInt64(LWasmTruncateToInt64* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register64 output = ToOutRegister64(lir);

  MWasmTruncateToInt64* mir = lir->mir();
  MIRType fromType = mir->input()->type();

  MOZ_ASSERT(fromType == MIRType::Double || fromType == MIRType::Float32);

  auto* ool = new (alloc()) OutOfLineWasmTruncateCheck(mir, input, output);
  addOutOfLineCode(ool, mir);

  Label* oolEntry = ool->entry();
  Label* oolRejoin = ool->rejoin();
  bool isSaturating = mir->isSaturating();

  if (fromType == MIRType::Double) {
    if (mir->isUnsigned()) {
      masm.wasmTruncateDoubleToUInt64(input, output, isSaturating, oolEntry,
                                      oolRejoin, InvalidFloatReg);
    } else {
      masm.wasmTruncateDoubleToInt64(input, output, isSaturating, oolEntry,
                                     oolRejoin, InvalidFloatReg);
    }
  } else {
    if (mir->isUnsigned()) {
      masm.wasmTruncateFloat32ToUInt64(input, output, isSaturating, oolEntry,
                                       oolRejoin, InvalidFloatReg);
    } else {
      masm.wasmTruncateFloat32ToInt64(input, output, isSaturating, oolEntry,
                                      oolRejoin, InvalidFloatReg);
    }
  }
}

void CodeGenerator::visitInt64ToFloatingPoint(LInt64ToFloatingPoint* lir) {
  Register64 input = ToRegister64(lir->input());
  FloatRegister output = ToFloatRegister(lir->output());

  MIRType outputType = lir->mir()->type();
  MOZ_ASSERT(outputType == MIRType::Double || outputType == MIRType::Float32);

  if (outputType == MIRType::Double) {
    if (lir->mir()->isUnsigned()) {
      masm.convertUInt64ToDouble(input, output, Register::Invalid());
    } else {
      masm.convertInt64ToDouble(input, output);
    }
  } else {
    if (lir->mir()->isUnsigned()) {
      masm.convertUInt64ToFloat32(input, output, Register::Invalid());
    } else {
      masm.convertInt64ToFloat32(input, output);
    }
  }
}

void CodeGenerator::visitMinMaxD(LMinMaxD* ins) {
  FloatRegister first = ToFloatRegister(ins->first());
  FloatRegister second = ToFloatRegister(ins->second());

  MOZ_ASSERT(first == ToFloatRegister(ins->output()));

  if (ins->mir()->isMax()) {
    masm.maxDouble(second, first, true);
  } else {
    masm.minDouble(second, first, true);
  }
}

void CodeGenerator::visitMinMaxF(LMinMaxF* ins) {
  FloatRegister first = ToFloatRegister(ins->first());
  FloatRegister second = ToFloatRegister(ins->second());

  MOZ_ASSERT(first == ToFloatRegister(ins->output()));

  if (ins->mir()->isMax()) {
    masm.maxFloat32(second, first, true);
  } else {
    masm.minFloat32(second, first, true);
  }
}

void CodeGenerator::visitAddI(LAddI* ins) {
  const LAllocation* lhs = ins->lhs();
  const LAllocation* rhs = ins->rhs();
  const LDefinition* dest = ins->output();

  MOZ_ASSERT(rhs->isConstant() || rhs->isGeneralReg());

  // If there is no snapshot, we don't need to check for overflow
  if (!ins->snapshot()) {
    if (rhs->isConstant()) {
      masm.ma_add_w(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
    } else {
      masm.as_add_w(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
    }
    return;
  }

  Label overflow;
  if (rhs->isConstant()) {
    masm.ma_add32TestOverflow(ToRegister(dest), ToRegister(lhs),
                              Imm32(ToInt32(rhs)), &overflow);
  } else {
    masm.ma_add32TestOverflow(ToRegister(dest), ToRegister(lhs),
                              ToRegister(rhs), &overflow);
  }

  bailoutFrom(&overflow, ins->snapshot());
}

void CodeGenerator::visitAddI64(LAddI64* lir) {
  LInt64Allocation lhs = lir->lhs();
  LInt64Allocation rhs = lir->rhs();

  MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

  if (IsConstant(rhs)) {
    masm.add64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
    return;
  }

  masm.add64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
}

void CodeGenerator::visitSubI(LSubI* ins) {
  const LAllocation* lhs = ins->lhs();
  const LAllocation* rhs = ins->rhs();
  const LDefinition* dest = ins->output();

  MOZ_ASSERT(rhs->isConstant() || rhs->isGeneralReg());

  // If there is no snapshot, we don't need to check for overflow

  if (!ins->snapshot()) {
    if (rhs->isConstant()) {
      masm.ma_sub_w(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
    } else {
      masm.as_sub_w(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
    }
    return;
  }

  Label overflow;
  if (rhs->isConstant()) {
    masm.ma_sub32TestOverflow(ToRegister(dest), ToRegister(lhs),
                              Imm32(ToInt32(rhs)), &overflow);
  } else {
    masm.ma_sub32TestOverflow(ToRegister(dest), ToRegister(lhs),
                              ToRegister(rhs), &overflow);
  }

  bailoutFrom(&overflow, ins->snapshot());
}

void CodeGenerator::visitSubI64(LSubI64* lir) {
  LInt64Allocation lhs = lir->lhs();
  LInt64Allocation rhs = lir->rhs();

  MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

  if (IsConstant(rhs)) {
    masm.sub64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
    return;
  }

  masm.sub64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
}

void CodeGenerator::visitMulI(LMulI* ins) {
  const LAllocation* lhs = ins->lhs();
  const LAllocation* rhs = ins->rhs();
  Register dest = ToRegister(ins->output());
  MMul* mul = ins->mir();

  MOZ_ASSERT_IF(mul->mode() == MMul::Integer,
                !mul->canBeNegativeZero() && !mul->canOverflow());

  if (rhs->isConstant()) {
    int32_t constant = ToInt32(rhs);
    Register src = ToRegister(lhs);

    // Bailout on -0.0
    if (mul->canBeNegativeZero() && constant <= 0) {
      Assembler::Condition cond =
          (constant == 0) ? Assembler::LessThan : Assembler::Equal;
      bailoutCmp32(cond, src, Imm32(0), ins->snapshot());
    }

    switch (constant) {
      case -1:
        if (mul->canOverflow()) {
          bailoutCmp32(Assembler::Equal, src, Imm32(INT32_MIN),
                       ins->snapshot());
        }

        masm.as_sub_w(dest, zero, src);
        break;
      case 0:
        masm.move32(zero, dest);
        break;
      case 1:
        masm.move32(src, dest);
        break;
      case 2:
        if (mul->canOverflow()) {
          Label mulTwoOverflow;
          masm.ma_add32TestOverflow(dest, src, src, &mulTwoOverflow);

          bailoutFrom(&mulTwoOverflow, ins->snapshot());
        } else {
          masm.as_add_w(dest, src, src);
        }
        break;
      default:
        uint32_t shift = FloorLog2(constant);

        if (!mul->canOverflow() && (constant > 0)) {
          // If it cannot overflow, we can do lots of optimizations.
          uint32_t rest = constant - (1 << shift);

          // See if the constant has one bit set, meaning it can be
          // encoded as a bitshift.
          if ((1 << shift) == constant) {
            masm.as_slli_w(dest, src, shift % 32);
            return;
          }

          // If the constant cannot be encoded as (1<<C1), see if it can
          // be encoded as (1<<C1) | (1<<C2), which can be computed
          // using an add and a shift.
          uint32_t shift_rest = FloorLog2(rest);
          if (src != dest && (1u << shift_rest) == rest) {
            masm.as_slli_w(dest, src, (shift - shift_rest) % 32);
            masm.add32(src, dest);
            if (shift_rest != 0) {
              masm.as_slli_w(dest, dest, shift_rest % 32);
            }
            return;
          }
        }

        if (mul->canOverflow() && (constant > 0) && (src != dest)) {
          // To stay on the safe side, only optimize things that are a
          // power of 2.

          if ((1 << shift) == constant) {
            ScratchRegisterScope scratch(masm);
            // dest = lhs * pow(2, shift)
            masm.as_slli_w(dest, src, shift % 32);
            // At runtime, check (lhs == dest >> shift), if this does
            // not hold, some bits were lost due to overflow, and the
            // computation should be resumed as a double.
            masm.as_srai_w(scratch, dest, shift % 32);
            bailoutCmp32(Assembler::NotEqual, src, Register(scratch),
                         ins->snapshot());
            return;
          }
        }

        if (mul->canOverflow()) {
          Label mulConstOverflow;
          masm.ma_mul32TestOverflow(dest, ToRegister(lhs), Imm32(ToInt32(rhs)),
                                    &mulConstOverflow);

          bailoutFrom(&mulConstOverflow, ins->snapshot());
        } else {
          masm.ma_mul(dest, src, Imm32(ToInt32(rhs)));
        }
        break;
    }
  } else {
    Label multRegOverflow;

    if (mul->canOverflow()) {
      masm.ma_mul32TestOverflow(dest, ToRegister(lhs), ToRegister(rhs),
                                &multRegOverflow);
      bailoutFrom(&multRegOverflow, ins->snapshot());
    } else {
      masm.as_mul_w(dest, ToRegister(lhs), ToRegister(rhs));
    }

    if (mul->canBeNegativeZero()) {
      Label done;
      masm.ma_b(dest, dest, &done, Assembler::NonZero, ShortJump);

      // Result is -0 if lhs or rhs is negative.
      // In that case result must be double value so bailout
      Register scratch = SecondScratchReg;
      masm.as_or(scratch, ToRegister(lhs), ToRegister(rhs));
      bailoutCmp32(Assembler::Signed, scratch, scratch, ins->snapshot());

      masm.bind(&done);
    }
  }
}

void CodeGenerator::visitMulI64(LMulI64* lir) {
  LInt64Allocation lhs = lir->lhs();
  LInt64Allocation rhs = lir->rhs();
  Register64 output = ToOutRegister64(lir);
  MOZ_ASSERT(ToRegister64(lhs) == output);

  if (IsConstant(rhs)) {
    int64_t constant = ToInt64(rhs);
    switch (constant) {
      case -1:
        masm.neg64(ToRegister64(lhs));
        return;
      case 0:
        masm.xor64(ToRegister64(lhs), ToRegister64(lhs));
        return;
      case 1:
        // nop
        return;
      case 2:
        masm.as_add_d(output.reg, ToRegister64(lhs).reg, ToRegister64(lhs).reg);
        return;
      default:
        if (constant > 0) {
          if (mozilla::IsPowerOfTwo(static_cast<uint64_t>(constant + 1))) {
            ScratchRegisterScope scratch(masm);
            masm.movePtr(ToRegister64(lhs).reg, scratch);
            masm.as_slli_d(output.reg, ToRegister64(lhs).reg,
                           FloorLog2(constant + 1));
            masm.sub64(scratch, output);
            return;
          } else if (mozilla::IsPowerOfTwo(
                         static_cast<uint64_t>(constant - 1))) {
            int32_t shift = mozilla::FloorLog2(constant - 1);
            if (shift < 5) {
              masm.as_alsl_d(output.reg, ToRegister64(lhs).reg,
                             ToRegister64(lhs).reg, shift - 1);
            } else {
              ScratchRegisterScope scratch(masm);
              masm.movePtr(ToRegister64(lhs).reg, scratch);
              masm.as_slli_d(output.reg, ToRegister64(lhs).reg, shift);
              masm.add64(scratch, output);
            }
            return;
          }
          // Use shift if constant is power of 2.
          int32_t shift = mozilla::FloorLog2(constant);
          if (int64_t(1) << shift == constant) {
            masm.lshift64(Imm32(shift), ToRegister64(lhs));
            return;
          }
        }
        Register temp = ToTempRegisterOrInvalid(lir->temp0());
        masm.mul64(Imm64(constant), ToRegister64(lhs), temp);
    }
  } else {
    Register temp = ToTempRegisterOrInvalid(lir->temp0());
    masm.mul64(ToOperandOrRegister64(rhs), ToRegister64(lhs), temp);
  }
}

void CodeGenerator::visitDivI(LDivI* ins) {
  // Extract the registers from this instruction
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register dest = ToRegister(ins->output());
  Register temp = ToRegister(ins->temp0());
  MDiv* mir = ins->mir();

  Label done;

  // Handle divide by zero.
  if (mir->canBeDivideByZero()) {
    if (mir->trapOnError()) {
      Label nonZero;
      masm.ma_b(rhs, rhs, &nonZero, Assembler::NonZero);
      masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->trapSiteDesc());
      masm.bind(&nonZero);
    } else if (mir->canTruncateInfinities()) {
      // Truncated division by zero is zero (Infinity|0 == 0)
      Label notzero;
      masm.ma_b(rhs, rhs, &notzero, Assembler::NonZero, ShortJump);
      masm.move32(Imm32(0), dest);
      masm.ma_b(&done, ShortJump);
      masm.bind(&notzero);
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutCmp32(Assembler::Zero, rhs, rhs, ins->snapshot());
    }
  }

  // Handle an integer overflow exception from -2147483648 / -1.
  if (mir->canBeNegativeOverflow()) {
    Label notMinInt;
    masm.move32(Imm32(INT32_MIN), temp);
    masm.ma_b(lhs, temp, &notMinInt, Assembler::NotEqual, ShortJump);

    masm.move32(Imm32(-1), temp);
    if (mir->trapOnError()) {
      Label ok;
      masm.ma_b(rhs, temp, &ok, Assembler::NotEqual);
      masm.wasmTrap(wasm::Trap::IntegerOverflow, mir->trapSiteDesc());
      masm.bind(&ok);
    } else if (mir->canTruncateOverflow()) {
      // (-INT32_MIN)|0 == INT32_MIN
      Label skip;
      masm.ma_b(rhs, temp, &skip, Assembler::NotEqual, ShortJump);
      masm.move32(Imm32(INT32_MIN), dest);
      masm.ma_b(&done, ShortJump);
      masm.bind(&skip);
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutCmp32(Assembler::Equal, rhs, temp, ins->snapshot());
    }
    masm.bind(&notMinInt);
  }

  // Handle negative 0. (0/-Y)
  if (!mir->canTruncateNegativeZero() && mir->canBeNegativeZero()) {
    Label nonzero;
    masm.ma_b(lhs, lhs, &nonzero, Assembler::NonZero, ShortJump);
    bailoutCmp32(Assembler::LessThan, rhs, Imm32(0), ins->snapshot());
    masm.bind(&nonzero);
  }
  // Note: above safety checks could not be verified as Ion seems to be
  // smarter and requires double arithmetic in such cases.

  // All regular. Lets call div.
  if (mir->canTruncateRemainder()) {
    masm.as_div_w(dest, lhs, rhs);
  } else {
    MOZ_ASSERT(mir->fallible());

    Label remainderNonZero;
    masm.ma_div_branch_overflow(dest, lhs, rhs, &remainderNonZero);
    bailoutFrom(&remainderNonZero, ins->snapshot());
  }

  masm.bind(&done);
}

void CodeGenerator::visitDivPowTwoI(LDivPowTwoI* ins) {
  Register lhs = ToRegister(ins->numerator());
  Register dest = ToRegister(ins->output());
  Register tmp = ToRegister(ins->temp0());
  int32_t shift = ins->shift();

  if (shift != 0) {
    MDiv* mir = ins->mir();
    if (!mir->isTruncated()) {
      // If the remainder is going to be != 0, bailout since this must
      // be a double.
      masm.as_slli_w(tmp, lhs, (32 - shift) % 32);
      bailoutCmp32(Assembler::NonZero, tmp, tmp, ins->snapshot());
    }

    if (!mir->canBeNegativeDividend()) {
      // Numerator is unsigned, so needs no adjusting. Do the shift.
      masm.as_srai_w(dest, lhs, shift % 32);
      return;
    }

    // Adjust the value so that shifting produces a correctly rounded result
    // when the numerator is negative. See 10-1 "Signed Division by a Known
    // Power of 2" in Henry S. Warren, Jr.'s Hacker's Delight.
    if (shift > 1) {
      masm.as_srai_w(tmp, lhs, 31);
      masm.as_srli_w(tmp, tmp, (32 - shift) % 32);
      masm.add32(lhs, tmp);
    } else {
      masm.as_srli_w(tmp, lhs, (32 - shift) % 32);
      masm.add32(lhs, tmp);
    }

    // Do the shift.
    masm.as_srai_w(dest, tmp, shift % 32);
  } else {
    masm.move32(lhs, dest);
  }
}

void CodeGenerator::visitModI(LModI* ins) {
  // Extract the registers from this instruction
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register dest = ToRegister(ins->output());
  MOZ_ASSERT(rhs != dest && lhs != dest);
  MMod* mir = ins->mir();
  Label done;

  // Prevent divide by zero.
  if (mir->canBeDivideByZero()) {
    if (mir->isTruncated()) {
      if (mir->trapOnError()) {
        Label nonZero;
        masm.ma_b(rhs, rhs, &nonZero, Assembler::NonZero, ShortJump);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->trapSiteDesc());
        masm.bind(&nonZero);
      } else {
        // Truncated division by zero yields integer zero.
        masm.move32(rhs, dest);
        masm.ma_b(rhs, rhs, &done, Assembler::Zero, ShortJump);
      }
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutCmp32(Assembler::Equal, rhs, Imm32(0), ins->snapshot());
    }
  }

  masm.as_mod_w(dest, lhs, rhs);

  // If X%Y == 0 and X < 0, then we *actually* wanted to return -0.0
  if (mir->canBeNegativeDividend() && !mir->isTruncated()) {
    MOZ_ASSERT(mir->fallible());
    masm.ma_b(dest, Imm32(0), &done, Assembler::NotEqual, ShortJump);
    bailoutCmp32(Assembler::Signed, lhs, Imm32(0), ins->snapshot());
  }
  masm.bind(&done);
}

void CodeGenerator::visitModPowTwoI(LModPowTwoI* ins) {
  Register in = ToRegister(ins->input());
  Register out = ToRegister(ins->output());
  MMod* mir = ins->mir();
  Label negative, done;

  masm.move32(in, out);
  masm.ma_b(in, in, &done, Assembler::Zero, ShortJump);
  // Switch based on sign of the lhs.
  // Positive numbers are just a bitmask
  masm.ma_b(in, in, &negative, Assembler::Signed, ShortJump);
  {
    masm.and32(Imm32((1 << ins->shift()) - 1), out);
    masm.ma_b(&done, ShortJump);
  }

  // Negative numbers need a negate, bitmask, negate
  {
    masm.bind(&negative);
    masm.neg32(out);
    masm.and32(Imm32((1 << ins->shift()) - 1), out);
    masm.neg32(out);
  }
  if (mir->canBeNegativeDividend()) {
    if (!mir->isTruncated()) {
      MOZ_ASSERT(mir->fallible());
      bailoutCmp32(Assembler::Equal, out, zero, ins->snapshot());
    } else {
      // -0|0 == 0
    }
  }
  masm.bind(&done);
}

void CodeGenerator::visitModMaskI(LModMaskI* ins) {
  Register src = ToRegister(ins->input());
  Register dest = ToRegister(ins->output());
  Register tmp0 = ToRegister(ins->temp0());
  Register tmp1 = ToRegister(ins->temp1());
  MMod* mir = ins->mir();

  if (!mir->isTruncated() && mir->canBeNegativeDividend()) {
    MOZ_ASSERT(mir->fallible());

    Label bail;
    masm.ma_mod_mask(src, dest, tmp0, tmp1, ins->shift(), &bail);
    bailoutFrom(&bail, ins->snapshot());
  } else {
    masm.ma_mod_mask(src, dest, tmp0, tmp1, ins->shift(), nullptr);
  }
}

void CodeGenerator::visitBitNotI(LBitNotI* ins) {
  const LAllocation* input = ins->input();
  const LDefinition* dest = ins->output();
  MOZ_ASSERT(!input->isConstant());

  masm.as_nor(ToRegister(dest), ToRegister(input), zero);
}

void CodeGenerator::visitBitNotI64(LBitNotI64* ins) {
  LInt64Allocation input = ins->input();
  MOZ_ASSERT(!IsConstant(input));
  Register64 inputReg = ToRegister64(input);
  MOZ_ASSERT(inputReg == ToOutRegister64(ins));
  masm.as_nor(inputReg.reg, inputReg.reg, zero);
}

void CodeGenerator::visitBitOpI(LBitOpI* ins) {
  const LAllocation* lhs = ins->lhs();
  const LAllocation* rhs = ins->rhs();
  const LDefinition* dest = ins->output();
  // all of these bitops should be either imm32's, or integer registers.
  switch (ins->bitop()) {
    case JSOp::BitOr:
      if (rhs->isConstant()) {
        masm.ma_or(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)),
                   true);
      } else {
        masm.as_or(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
        masm.as_slli_w(ToRegister(dest), ToRegister(dest), 0);
      }
      break;
    case JSOp::BitXor:
      if (rhs->isConstant()) {
        masm.ma_xor(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)),
                    true);
      } else {
        masm.as_xor(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
        masm.as_slli_w(ToRegister(dest), ToRegister(dest), 0);
      }
      break;
    case JSOp::BitAnd:
      if (rhs->isConstant()) {
        masm.ma_and(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)),
                    true);
      } else {
        masm.as_and(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
        masm.as_slli_w(ToRegister(dest), ToRegister(dest), 0);
      }
      break;
    default:
      MOZ_CRASH("unexpected binary opcode");
  }
}

void CodeGenerator::visitBitOpI64(LBitOpI64* lir) {
  LInt64Allocation lhs = lir->lhs();
  LInt64Allocation rhs = lir->rhs();

  MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

  switch (lir->bitop()) {
    case JSOp::BitOr:
      if (IsConstant(rhs)) {
        masm.or64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
      } else {
        masm.or64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
      }
      break;
    case JSOp::BitXor:
      if (IsConstant(rhs)) {
        masm.xor64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
      } else {
        masm.xor64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
      }
      break;
    case JSOp::BitAnd:
      if (IsConstant(rhs)) {
        masm.and64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
      } else {
        masm.and64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
      }
      break;
    default:
      MOZ_CRASH("unexpected binary opcode");
  }
}

void CodeGenerator::visitShiftI(LShiftI* ins) {
  Register lhs = ToRegister(ins->lhs());
  const LAllocation* rhs = ins->rhs();
  Register dest = ToRegister(ins->output());

  if (rhs->isConstant()) {
    int32_t shift = ToInt32(rhs) & 0x1F;
    switch (ins->bitop()) {
      case JSOp::Lsh:
        if (shift) {
          masm.as_slli_w(dest, lhs, shift % 32);
        } else {
          masm.move32(lhs, dest);
        }
        break;
      case JSOp::Rsh:
        if (shift) {
          masm.as_srai_w(dest, lhs, shift % 32);
        } else {
          masm.move32(lhs, dest);
        }
        break;
      case JSOp::Ursh:
        if (shift) {
          masm.as_srli_w(dest, lhs, shift % 32);
        } else {
          // x >>> 0 can overflow.
          if (ins->mir()->toUrsh()->fallible()) {
            bailoutCmp32(Assembler::LessThan, lhs, Imm32(0), ins->snapshot());
          }
          masm.move32(lhs, dest);
        }
        break;
      default:
        MOZ_CRASH("Unexpected shift op");
    }
  } else {
    // The shift amounts should be AND'ed into the 0-31 range
    masm.ma_and(dest, ToRegister(rhs), Imm32(0x1F));

    switch (ins->bitop()) {
      case JSOp::Lsh:
        masm.as_sll_w(dest, lhs, dest);
        break;
      case JSOp::Rsh:
        masm.as_sra_w(dest, lhs, dest);
        break;
      case JSOp::Ursh:
        masm.as_srl_w(dest, lhs, dest);
        if (ins->mir()->toUrsh()->fallible()) {
          // x >>> 0 can overflow.
          bailoutCmp32(Assembler::LessThan, dest, Imm32(0), ins->snapshot());
        }
        break;
      default:
        MOZ_CRASH("Unexpected shift op");
    }
  }
}

void CodeGenerator::visitShiftI64(LShiftI64* lir) {
  LInt64Allocation lhs = lir->lhs();
  const LAllocation* rhs = lir->rhs();

  MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

  if (rhs->isConstant()) {
    int32_t shift = int32_t(rhs->toConstant()->toInt64() & 0x3F);
    switch (lir->bitop()) {
      case JSOp::Lsh:
        if (shift) {
          masm.lshift64(Imm32(shift), ToRegister64(lhs));
        }
        break;
      case JSOp::Rsh:
        if (shift) {
          masm.rshift64Arithmetic(Imm32(shift), ToRegister64(lhs));
        }
        break;
      case JSOp::Ursh:
        if (shift) {
          masm.rshift64(Imm32(shift), ToRegister64(lhs));
        }
        break;
      default:
        MOZ_CRASH("Unexpected shift op");
    }
    return;
  }

  switch (lir->bitop()) {
    case JSOp::Lsh:
      masm.lshift64(ToRegister(rhs), ToRegister64(lhs));
      break;
    case JSOp::Rsh:
      masm.rshift64Arithmetic(ToRegister(rhs), ToRegister64(lhs));
      break;
    case JSOp::Ursh:
      masm.rshift64(ToRegister(rhs), ToRegister64(lhs));
      break;
    default:
      MOZ_CRASH("Unexpected shift op");
  }
}

void CodeGenerator::visitUrshD(LUrshD* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register temp = ToRegister(ins->temp0());

  const LAllocation* rhs = ins->rhs();
  FloatRegister out = ToFloatRegister(ins->output());

  if (rhs->isConstant()) {
    masm.as_srli_w(temp, lhs, ToInt32(rhs) % 32);
  } else {
    masm.as_srl_w(temp, lhs, ToRegister(rhs));
  }

  masm.convertUInt32ToDouble(temp, out);
}

void CodeGenerator::visitPowHalfD(LPowHalfD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());
  ScratchDoubleScope fpscratch(masm);

  Label done, skip;

  // Masm.pow(-Infinity, 0.5) == Infinity.
  masm.loadConstantDouble(NegativeInfinity<double>(), fpscratch);
  masm.ma_bc_d(input, fpscratch, &skip, Assembler::DoubleNotEqualOrUnordered,
               ShortJump);
  masm.as_fneg_d(output, fpscratch);
  masm.ma_b(&done, ShortJump);

  masm.bind(&skip);
  // Math.pow(-0, 0.5) == 0 == Math.pow(0, 0.5).
  // Adding 0 converts any -0 to 0.
  masm.loadConstantDouble(0.0, fpscratch);
  masm.as_fadd_d(output, input, fpscratch);
  masm.as_fsqrt_d(output, output);

  masm.bind(&done);
}

void CodeGenerator::visitMathD(LMathD* math) {
  FloatRegister src1 = ToFloatRegister(math->lhs());
  FloatRegister src2 = ToFloatRegister(math->rhs());
  FloatRegister output = ToFloatRegister(math->output());

  switch (math->jsop()) {
    case JSOp::Add:
      masm.as_fadd_d(output, src1, src2);
      break;
    case JSOp::Sub:
      masm.as_fsub_d(output, src1, src2);
      break;
    case JSOp::Mul:
      masm.as_fmul_d(output, src1, src2);
      break;
    case JSOp::Div:
      masm.as_fdiv_d(output, src1, src2);
      break;
    default:
      MOZ_CRASH("unexpected opcode");
  }
}

void CodeGenerator::visitMathF(LMathF* math) {
  FloatRegister src1 = ToFloatRegister(math->lhs());
  FloatRegister src2 = ToFloatRegister(math->rhs());
  FloatRegister output = ToFloatRegister(math->output());

  switch (math->jsop()) {
    case JSOp::Add:
      masm.as_fadd_s(output, src1, src2);
      break;
    case JSOp::Sub:
      masm.as_fsub_s(output, src1, src2);
      break;
    case JSOp::Mul:
      masm.as_fmul_s(output, src1, src2);
      break;
    case JSOp::Div:
      masm.as_fdiv_s(output, src1, src2);
      break;
    default:
      MOZ_CRASH("unexpected opcode");
  }
}

void CodeGenerator::visitTruncateDToInt32(LTruncateDToInt32* ins) {
  emitTruncateDouble(ToFloatRegister(ins->input()), ToRegister(ins->output()),
                     ins->mir());
}

void CodeGenerator::visitTruncateFToInt32(LTruncateFToInt32* ins) {
  emitTruncateFloat32(ToFloatRegister(ins->input()), ToRegister(ins->output()),
                      ins->mir());
}

void CodeGenerator::visitWasmBuiltinTruncateDToInt32(
    LWasmBuiltinTruncateDToInt32* lir) {
  emitTruncateDouble(ToFloatRegister(lir->input()), ToRegister(lir->output()),
                     lir->mir());
}

void CodeGenerator::visitWasmBuiltinTruncateFToInt32(
    LWasmBuiltinTruncateFToInt32* lir) {
  emitTruncateFloat32(ToFloatRegister(lir->input()), ToRegister(lir->output()),
                      lir->mir());
}

void CodeGenerator::visitWasmTruncateToInt32(LWasmTruncateToInt32* lir) {
  auto input = ToFloatRegister(lir->input());
  auto output = ToRegister(lir->output());

  MWasmTruncateToInt32* mir = lir->mir();
  MIRType fromType = mir->input()->type();

  MOZ_ASSERT(fromType == MIRType::Double || fromType == MIRType::Float32);

  auto* ool = new (alloc()) OutOfLineWasmTruncateCheck(mir, input, output);
  addOutOfLineCode(ool, mir);

  Label* oolEntry = ool->entry();
  if (mir->isUnsigned()) {
    if (fromType == MIRType::Double) {
      masm.wasmTruncateDoubleToUInt32(input, output, mir->isSaturating(),
                                      oolEntry);
    } else if (fromType == MIRType::Float32) {
      masm.wasmTruncateFloat32ToUInt32(input, output, mir->isSaturating(),
                                       oolEntry);
    } else {
      MOZ_CRASH("unexpected type");
    }

    masm.bind(ool->rejoin());
    return;
  }

  if (fromType == MIRType::Double) {
    masm.wasmTruncateDoubleToInt32(input, output, mir->isSaturating(),
                                   oolEntry);
  } else if (fromType == MIRType::Float32) {
    masm.wasmTruncateFloat32ToInt32(input, output, mir->isSaturating(),
                                    oolEntry);
  } else {
    MOZ_CRASH("unexpected type");
  }

  masm.bind(ool->rejoin());
}

void CodeGenerator::visitCopySignF(LCopySignF* ins) {
  FloatRegister lhs = ToFloatRegister(ins->lhs());
  FloatRegister rhs = ToFloatRegister(ins->rhs());
  FloatRegister output = ToFloatRegister(ins->output());

  Register lhsi = ToRegister(ins->temp0());
  Register rhsi = ToRegister(ins->temp1());

  masm.moveFromFloat32(lhs, lhsi);
  masm.moveFromFloat32(rhs, rhsi);

  // Combine.
  masm.as_bstrins_w(rhsi, lhsi, 30, 0);

  masm.moveToFloat32(rhsi, output);
}

void CodeGenerator::visitCopySignD(LCopySignD* ins) {
  FloatRegister lhs = ToFloatRegister(ins->lhs());
  FloatRegister rhs = ToFloatRegister(ins->rhs());
  FloatRegister output = ToFloatRegister(ins->output());

  Register lhsi = ToRegister(ins->temp0());
  Register rhsi = ToRegister(ins->temp1());

  // Manipulate high words of double inputs.
  masm.moveFromDoubleHi(lhs, lhsi);
  masm.moveFromDoubleHi(rhs, rhsi);

  // Combine.
  masm.as_bstrins_w(rhsi, lhsi, 30, 0);

  masm.moveToDoubleHi(rhsi, output);
}

void CodeGenerator::visitTestDAndBranch(LTestDAndBranch* test) {
  FloatRegister input = ToFloatRegister(test->input());
  ScratchDoubleScope fpscratch(masm);

  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  masm.loadConstantDouble(0.0, fpscratch);
  // If 0, or NaN, the result is false.
  if (isNextBlock(ifFalse->lir())) {
    branchToBlock(Assembler::DoubleFloat, input, fpscratch, ifTrue,
                  Assembler::DoubleNotEqual);
  } else {
    branchToBlock(Assembler::DoubleFloat, input, fpscratch, ifFalse,
                  Assembler::DoubleEqualOrUnordered);
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitTestFAndBranch(LTestFAndBranch* test) {
  FloatRegister input = ToFloatRegister(test->input());
  ScratchFloat32Scope fpscratch(masm);

  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  masm.loadConstantFloat32(0.0f, fpscratch);
  // If 0, or NaN, the result is false.

  if (isNextBlock(ifFalse->lir())) {
    branchToBlock(Assembler::SingleFloat, input, fpscratch, ifTrue,
                  Assembler::DoubleNotEqual);
  } else {
    branchToBlock(Assembler::SingleFloat, input, fpscratch, ifFalse,
                  Assembler::DoubleEqualOrUnordered);
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitCompareD(LCompareD* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());
  Register dest = ToRegister(comp->output());

  Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());
  masm.ma_cmp_set_double(dest, lhs, rhs, cond);
}

void CodeGenerator::visitCompareF(LCompareF* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());
  Register dest = ToRegister(comp->output());

  Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());
  masm.ma_cmp_set_float32(dest, lhs, rhs, cond);
}

void CodeGenerator::visitCompareDAndBranch(LCompareDAndBranch* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond =
      JSOpToDoubleCondition(comp->cmpMir()->jsop());
  MBasicBlock* ifTrue = comp->ifTrue();
  MBasicBlock* ifFalse = comp->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    branchToBlock(Assembler::DoubleFloat, lhs, rhs, ifTrue, cond);
  } else {
    branchToBlock(Assembler::DoubleFloat, lhs, rhs, ifFalse,
                  Assembler::InvertCondition(cond));
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitCompareFAndBranch(LCompareFAndBranch* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond =
      JSOpToDoubleCondition(comp->cmpMir()->jsop());
  MBasicBlock* ifTrue = comp->ifTrue();
  MBasicBlock* ifFalse = comp->ifFalse();

  if (isNextBlock(ifFalse->lir())) {
    branchToBlock(Assembler::SingleFloat, lhs, rhs, ifTrue, cond);
  } else {
    branchToBlock(Assembler::SingleFloat, lhs, rhs, ifFalse,
                  Assembler::InvertCondition(cond));
    jumpToBlock(ifTrue);
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

void CodeGenerator::visitNotD(LNotD* ins) {
  // Since this operation is not, we want to set a bit if
  // the double is falsey, which means 0.0, -0.0 or NaN.
  FloatRegister in = ToFloatRegister(ins->input());
  Register dest = ToRegister(ins->output());
  ScratchDoubleScope fpscratch(masm);

  masm.loadConstantDouble(0.0, fpscratch);
  masm.ma_cmp_set_double(dest, in, fpscratch,
                         Assembler::DoubleEqualOrUnordered);
}

void CodeGenerator::visitNotF(LNotF* ins) {
  // Since this operation is not, we want to set a bit if
  // the float32 is falsey, which means 0.0, -0.0 or NaN.
  FloatRegister in = ToFloatRegister(ins->input());
  Register dest = ToRegister(ins->output());
  ScratchFloat32Scope fpscratch(masm);

  masm.loadConstantFloat32(0.0f, fpscratch);
  masm.ma_cmp_set_float32(dest, in, fpscratch,
                          Assembler::DoubleEqualOrUnordered);
}

void CodeGenerator::visitWasmLoad(LWasmLoad* lir) { emitWasmLoad(lir); }

void CodeGenerator::visitWasmStore(LWasmStore* lir) { emitWasmStore(lir); }

void CodeGenerator::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins) {
  const MAsmJSLoadHeap* mir = ins->mir();
  MOZ_ASSERT(!mir->hasMemoryBase());

  const LAllocation* ptr = ins->ptr();
  const LDefinition* output = ins->output();
  const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();

  Register ptrReg = ToRegister(ptr);
  Scalar::Type accessType = mir->accessType();
  bool isFloat = accessType == Scalar::Float32 || accessType == Scalar::Float64;
  Label done;

  if (mir->needsBoundsCheck()) {
    Label boundsCheckPassed;
    Register boundsCheckLimitReg = ToRegister(boundsCheckLimit);
    masm.wasmBoundsCheck32(Assembler::Below, ptrReg, boundsCheckLimitReg,
                           &boundsCheckPassed);
    // Return a default value in case of a bounds-check failure.
    if (isFloat) {
      if (accessType == Scalar::Float32) {
        masm.loadConstantFloat32(GenericNaN(), ToFloatRegister(output));
      } else {
        masm.loadConstantDouble(GenericNaN(), ToFloatRegister(output));
      }
    } else {
      masm.mov(zero, ToRegister(output));
    }
    masm.jump(&done);
    masm.bind(&boundsCheckPassed);
  }

  // TODO(loong64): zero-extend index in asm.js?
  SecondScratchRegisterScope scratch2(masm);
  masm.move32To64ZeroExtend(ptrReg, Register64(scratch2));

  switch (accessType) {
    case Scalar::Int8:
      masm.as_ldx_b(ToRegister(output), HeapReg, scratch2);
      break;
    case Scalar::Uint8:
      masm.as_ldx_bu(ToRegister(output), HeapReg, scratch2);
      break;
    case Scalar::Int16:
      masm.as_ldx_h(ToRegister(output), HeapReg, scratch2);
      break;
    case Scalar::Uint16:
      masm.as_ldx_hu(ToRegister(output), HeapReg, scratch2);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      masm.as_ldx_w(ToRegister(output), HeapReg, scratch2);
      break;
    case Scalar::Float64:
      masm.as_fldx_d(ToFloatRegister(output), HeapReg, scratch2);
      break;
    case Scalar::Float32:
      masm.as_fldx_s(ToFloatRegister(output), HeapReg, scratch2);
      break;
    default:
      MOZ_CRASH("unexpected array type");
  }

  if (done.used()) {
    masm.bind(&done);
  }
}

void CodeGenerator::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins) {
  const MAsmJSStoreHeap* mir = ins->mir();
  MOZ_ASSERT(!mir->hasMemoryBase());

  const LAllocation* value = ins->value();
  const LAllocation* ptr = ins->ptr();
  const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();

  Register ptrReg = ToRegister(ptr);

  Label done;
  if (mir->needsBoundsCheck()) {
    Register boundsCheckLimitReg = ToRegister(boundsCheckLimit);
    masm.wasmBoundsCheck32(Assembler::AboveOrEqual, ptrReg, boundsCheckLimitReg,
                           &done);
  }

  // TODO(loong64): zero-extend index in asm.js?
  SecondScratchRegisterScope scratch2(masm);
  masm.move32To64ZeroExtend(ptrReg, Register64(scratch2));

  switch (mir->accessType()) {
    case Scalar::Int8:
    case Scalar::Uint8:
      masm.as_stx_b(ToRegister(value), HeapReg, scratch2);
      break;
    case Scalar::Int16:
    case Scalar::Uint16:
      masm.as_stx_h(ToRegister(value), HeapReg, scratch2);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      masm.as_stx_w(ToRegister(value), HeapReg, scratch2);
      break;
    case Scalar::Float64:
      masm.as_fstx_d(ToFloatRegister(value), HeapReg, scratch2);
      break;
    case Scalar::Float32:
      masm.as_fstx_s(ToFloatRegister(value), HeapReg, scratch2);
      break;
    default:
      MOZ_CRASH("unexpected array type");
  }

  if (done.used()) {
    masm.bind(&done);
  }
}

void CodeGenerator::visitWasmCompareExchangeHeap(
    LWasmCompareExchangeHeap* ins) {
  MWasmCompareExchangeHeap* mir = ins->mir();
  Register memoryBase = ToRegister(ins->memoryBase());
  Register ptrReg = ToRegister(ins->ptr());
  BaseIndex srcAddr(memoryBase, ptrReg, TimesOne, mir->access().offset32());

  Register oldval = ToRegister(ins->oldValue());
  Register newval = ToRegister(ins->newValue());
  Register valueTemp = ToTempRegisterOrInvalid(ins->temp0());
  Register offsetTemp = ToTempRegisterOrInvalid(ins->temp1());
  Register maskTemp = ToTempRegisterOrInvalid(ins->temp2());

  masm.wasmCompareExchange(mir->access(), srcAddr, oldval, newval, valueTemp,
                           offsetTemp, maskTemp, ToRegister(ins->output()));
}

void CodeGenerator::visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins) {
  MWasmAtomicExchangeHeap* mir = ins->mir();
  Register memoryBase = ToRegister(ins->memoryBase());
  Register ptrReg = ToRegister(ins->ptr());
  Register value = ToRegister(ins->value());
  BaseIndex srcAddr(memoryBase, ptrReg, TimesOne, mir->access().offset32());

  Register valueTemp = ToTempRegisterOrInvalid(ins->temp0());
  Register offsetTemp = ToTempRegisterOrInvalid(ins->temp1());
  Register maskTemp = ToTempRegisterOrInvalid(ins->temp2());

  masm.wasmAtomicExchange(mir->access(), srcAddr, value, valueTemp, offsetTemp,
                          maskTemp, ToRegister(ins->output()));
}

void CodeGenerator::visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins) {
  MOZ_ASSERT(ins->mir()->hasUses());

  MWasmAtomicBinopHeap* mir = ins->mir();
  Register memoryBase = ToRegister(ins->memoryBase());
  Register ptrReg = ToRegister(ins->ptr());
  Register valueTemp = ToTempRegisterOrInvalid(ins->temp0());
  Register offsetTemp = ToTempRegisterOrInvalid(ins->temp1());
  Register maskTemp = ToTempRegisterOrInvalid(ins->temp2());

  BaseIndex srcAddr(memoryBase, ptrReg, TimesOne, mir->access().offset32());

  masm.wasmAtomicFetchOp(mir->access(), mir->operation(),
                         ToRegister(ins->value()), srcAddr, valueTemp,
                         offsetTemp, maskTemp, ToRegister(ins->output()));
}

void CodeGenerator::visitWasmAtomicBinopHeapForEffect(
    LWasmAtomicBinopHeapForEffect* ins) {
  MOZ_ASSERT(!ins->mir()->hasUses());

  MWasmAtomicBinopHeap* mir = ins->mir();
  Register memoryBase = ToRegister(ins->memoryBase());
  Register ptrReg = ToRegister(ins->ptr());
  Register valueTemp = ToTempRegisterOrInvalid(ins->temp0());
  Register offsetTemp = ToTempRegisterOrInvalid(ins->temp1());
  Register maskTemp = ToTempRegisterOrInvalid(ins->temp2());

  BaseIndex srcAddr(memoryBase, ptrReg, TimesOne, mir->access().offset32());
  masm.wasmAtomicEffectOp(mir->access(), mir->operation(),
                          ToRegister(ins->value()), srcAddr, valueTemp,
                          offsetTemp, maskTemp);
}

void CodeGenerator::visitWasmStackArg(LWasmStackArg* ins) {
  const MWasmStackArg* mir = ins->mir();
  if (ins->arg()->isConstant()) {
    masm.storePtr(ImmWord(ToInt32(ins->arg())),
                  Address(StackPointer, mir->spOffset()));
  } else {
    if (ins->arg()->isGeneralReg()) {
      masm.storePtr(ToRegister(ins->arg()),
                    Address(StackPointer, mir->spOffset()));
    } else if (mir->input()->type() == MIRType::Double) {
      masm.storeDouble(ToFloatRegister(ins->arg()),
                       Address(StackPointer, mir->spOffset()));
    } else {
      masm.storeFloat32(ToFloatRegister(ins->arg()),
                        Address(StackPointer, mir->spOffset()));
    }
  }
}

void CodeGenerator::visitWasmStackArgI64(LWasmStackArgI64* ins) {
  const MWasmStackArg* mir = ins->mir();
  Address dst(StackPointer, mir->spOffset());
  if (IsConstant(ins->arg())) {
    masm.store64(Imm64(ToInt64(ins->arg())), dst);
  } else {
    masm.store64(ToRegister64(ins->arg()), dst);
  }
}

void CodeGenerator::visitWasmSelect(LWasmSelect* ins) {
  MIRType mirType = ins->mir()->type();

  Register cond = ToRegister(ins->condExpr());
  const LAllocation* falseExpr = ins->falseExpr();

  if (mirType == MIRType::Int32 || mirType == MIRType::WasmAnyRef) {
    Register out = ToRegister(ins->output());
    MOZ_ASSERT(ToRegister(ins->trueExpr()) == out,
               "true expr input is reused for output");
    if (falseExpr->isGeneralReg()) {
      masm.moveIfZero(out, ToRegister(falseExpr), cond);
    } else {
      masm.cmp32Load32(Assembler::Zero, cond, cond, ToAddress(falseExpr), out);
    }
    return;
  }

  FloatRegister out = ToFloatRegister(ins->output());
  MOZ_ASSERT(ToFloatRegister(ins->trueExpr()) == out,
             "true expr input is reused for output");

  if (falseExpr->isFloatReg()) {
    if (mirType == MIRType::Float32) {
      masm.ma_fmovz(Assembler::SingleFloat, out, ToFloatRegister(falseExpr),
                    cond);
    } else if (mirType == MIRType::Double) {
      masm.ma_fmovz(Assembler::DoubleFloat, out, ToFloatRegister(falseExpr),
                    cond);
    } else {
      MOZ_CRASH("unhandled type in visitWasmSelect!");
    }
  } else {
    Label done;
    masm.ma_b(cond, cond, &done, Assembler::NonZero, ShortJump);

    if (mirType == MIRType::Float32) {
      masm.loadFloat32(ToAddress(falseExpr), out);
    } else if (mirType == MIRType::Double) {
      masm.loadDouble(ToAddress(falseExpr), out);
    } else {
      MOZ_CRASH("unhandled type in visitWasmSelect!");
    }

    masm.bind(&done);
  }
}

// We expect to handle only the case where compare is {U,}Int32 and select is
// {U,}Int32, and the "true" input is reused for the output.
void CodeGenerator::visitWasmCompareAndSelect(LWasmCompareAndSelect* ins) {
  bool cmpIs32bit = ins->compareType() == MCompare::Compare_Int32 ||
                    ins->compareType() == MCompare::Compare_UInt32;
  bool selIs32bit = ins->mir()->type() == MIRType::Int32;

  MOZ_RELEASE_ASSERT(
      cmpIs32bit && selIs32bit,
      "CodeGenerator::visitWasmCompareAndSelect: unexpected types");

  Register trueExprAndDest = ToRegister(ins->output());
  MOZ_ASSERT(ToRegister(ins->ifTrueExpr()) == trueExprAndDest,
             "true expr input is reused for output");

  Assembler::Condition cond = Assembler::InvertCondition(
      JSOpToCondition(ins->compareType(), ins->jsop()));
  const LAllocation* rhs = ins->rightExpr();
  const LAllocation* falseExpr = ins->ifFalseExpr();
  Register lhs = ToRegister(ins->leftExpr());

  masm.cmp32Move32(cond, lhs, ToRegister(rhs), ToRegister(falseExpr),
                   trueExprAndDest);
}

void CodeGenerator::visitUDivOrMod(LUDivOrMod* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());
  Label done;

  // Prevent divide by zero.
  if (ins->canBeDivideByZero()) {
    if (ins->mir()->isTruncated()) {
      if (ins->trapOnError()) {
        Label nonZero;
        masm.ma_b(rhs, rhs, &nonZero, Assembler::NonZero);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, ins->trapSiteDesc());
        masm.bind(&nonZero);
      } else {
        // Infinity|0 == 0
        Label notzero;
        masm.ma_b(rhs, rhs, &notzero, Assembler::NonZero, ShortJump);
        masm.move32(Imm32(0), output);
        masm.ma_b(&done, ShortJump);
        masm.bind(&notzero);
      }
    } else {
      bailoutCmp32(Assembler::Equal, rhs, Imm32(0), ins->snapshot());
    }
  }

  masm.as_mod_wu(output, lhs, rhs);

  // If the remainder is > 0, bailout since this must be a double.
  if (ins->mir()->isDiv()) {
    if (!ins->mir()->toDiv()->canTruncateRemainder()) {
      bailoutCmp32(Assembler::NonZero, output, output, ins->snapshot());
    }
    // Get quotient
    masm.as_div_wu(output, lhs, rhs);
  }

  if (!ins->mir()->isTruncated()) {
    bailoutCmp32(Assembler::LessThan, output, Imm32(0), ins->snapshot());
  }

  masm.bind(&done);
}

void CodeGenerator::visitEffectiveAddress(LEffectiveAddress* ins) {
  const MEffectiveAddress* mir = ins->mir();
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  Register output = ToRegister(ins->output());
  int32_t shift = Imm32::ShiftOf(mir->scale()).value;

  if (shift) {
    MOZ_ASSERT(shift <= 4);
    masm.as_alsl_w(output, index, base, shift - 1);
  } else {
    masm.as_add_w(output, base, index);
  }
  masm.ma_add_w(output, output, Imm32(mir->displacement()));
}

void CodeGenerator::visitNegI(LNegI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.as_sub_w(output, zero, input);
}

void CodeGenerator::visitNegI64(LNegI64* ins) {
  Register64 input = ToRegister64(ins->input());
  MOZ_ASSERT(input == ToOutRegister64(ins));
  masm.neg64(input);
}

void CodeGenerator::visitNegD(LNegD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());

  masm.as_fneg_d(output, input);
}

void CodeGenerator::visitNegF(LNegF* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());

  masm.as_fneg_s(output, input);
}

void CodeGenerator::visitWasmAddOffset(LWasmAddOffset* lir) {
  MWasmAddOffset* mir = lir->mir();
  Register base = ToRegister(lir->base());
  Register out = ToRegister(lir->output());

  Label ok;
  masm.ma_add32TestCarry(Assembler::CarryClear, out, base, Imm32(mir->offset()),
                         &ok);
  masm.wasmTrap(wasm::Trap::OutOfBounds, mir->trapSiteDesc());
  masm.bind(&ok);
}

void CodeGenerator::visitWasmAddOffset64(LWasmAddOffset64* lir) {
  MWasmAddOffset* mir = lir->mir();
  Register64 base = ToRegister64(lir->base());
  Register64 out = ToOutRegister64(lir);

  Label ok;
  masm.ma_addPtrTestCarry(Assembler::CarryClear, out.reg, base.reg,
                          ImmWord(mir->offset()), &ok);
  masm.wasmTrap(wasm::Trap::OutOfBounds, mir->trapSiteDesc());
  masm.bind(&ok);
}

void CodeGenerator::visitAtomicTypedArrayElementBinop(
    LAtomicTypedArrayElementBinop* lir) {
  MOZ_ASSERT(!lir->mir()->isForEffect());

  AnyRegister output = ToAnyRegister(lir->output());
  Register elements = ToRegister(lir->elements());
  Register outTemp = ToTempRegisterOrInvalid(lir->temp0());
  Register valueTemp = ToTempRegisterOrInvalid(lir->temp1());
  Register offsetTemp = ToTempRegisterOrInvalid(lir->temp2());
  Register maskTemp = ToTempRegisterOrInvalid(lir->temp3());
  Register value = ToRegister(lir->value());
  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address mem = ToAddress(elements, lir->index(), arrayType);
    masm.atomicFetchOpJS(arrayType, Synchronization::Full(),
                         lir->mir()->operation(), value, mem, valueTemp,
                         offsetTemp, maskTemp, outTemp, output);
  } else {
    BaseIndex mem(elements, ToRegister(lir->index()),
                  ScaleFromScalarType(arrayType));
    masm.atomicFetchOpJS(arrayType, Synchronization::Full(),
                         lir->mir()->operation(), value, mem, valueTemp,
                         offsetTemp, maskTemp, outTemp, output);
  }
}

void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect(
    LAtomicTypedArrayElementBinopForEffect* lir) {
  MOZ_ASSERT(lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register valueTemp = ToTempRegisterOrInvalid(lir->temp0());
  Register offsetTemp = ToTempRegisterOrInvalid(lir->temp1());
  Register maskTemp = ToTempRegisterOrInvalid(lir->temp2());
  Register value = ToRegister(lir->value());
  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address mem = ToAddress(elements, lir->index(), arrayType);
    masm.atomicEffectOpJS(arrayType, Synchronization::Full(),
                          lir->mir()->operation(), value, mem, valueTemp,
                          offsetTemp, maskTemp);
  } else {
    BaseIndex mem(elements, ToRegister(lir->index()),
                  ScaleFromScalarType(arrayType));
    masm.atomicEffectOpJS(arrayType, Synchronization::Full(),
                          lir->mir()->operation(), value, mem, valueTemp,
                          offsetTemp, maskTemp);
  }
}

void CodeGenerator::visitCompareExchangeTypedArrayElement(
    LCompareExchangeTypedArrayElement* lir) {
  Register elements = ToRegister(lir->elements());
  AnyRegister output = ToAnyRegister(lir->output());
  Register outTemp = ToTempRegisterOrInvalid(lir->temp0());

  Register oldval = ToRegister(lir->oldval());
  Register newval = ToRegister(lir->newval());
  Register valueTemp = ToTempRegisterOrInvalid(lir->temp1());
  Register offsetTemp = ToTempRegisterOrInvalid(lir->temp2());
  Register maskTemp = ToTempRegisterOrInvalid(lir->temp3());
  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.compareExchangeJS(arrayType, Synchronization::Full(), dest, oldval,
                           newval, valueTemp, offsetTemp, maskTemp, outTemp,
                           output);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.compareExchangeJS(arrayType, Synchronization::Full(), dest, oldval,
                           newval, valueTemp, offsetTemp, maskTemp, outTemp,
                           output);
  }
}

void CodeGenerator::visitAtomicExchangeTypedArrayElement(
    LAtomicExchangeTypedArrayElement* lir) {
  Register elements = ToRegister(lir->elements());
  AnyRegister output = ToAnyRegister(lir->output());
  Register outTemp = ToTempRegisterOrInvalid(lir->temp0());

  Register value = ToRegister(lir->value());
  Register valueTemp = ToTempRegisterOrInvalid(lir->temp1());
  Register offsetTemp = ToTempRegisterOrInvalid(lir->temp2());
  Register maskTemp = ToTempRegisterOrInvalid(lir->temp3());
  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicExchangeJS(arrayType, Synchronization::Full(), dest, value,
                          valueTemp, offsetTemp, maskTemp, outTemp, output);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicExchangeJS(arrayType, Synchronization::Full(), dest, value,
                          valueTemp, offsetTemp, maskTemp, outTemp, output);
  }
}

void CodeGenerator::visitCompareExchangeTypedArrayElement64(
    LCompareExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register64 oldval = ToRegister64(lir->oldval());
  Register64 newval = ToRegister64(lir->newval());
  Register64 out = ToOutRegister64(lir);
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
  MOZ_ASSERT(lir->mir()->hasUses());

  Register elements = ToRegister(lir->elements());
  Register64 value = ToRegister64(lir->value());
  Register64 temp = ToRegister64(lir->temp0());
  Register64 out = ToOutRegister64(lir);

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

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
  MOZ_ASSERT(!lir->mir()->hasUses());

  Register elements = ToRegister(lir->elements());
  Register64 value = ToRegister64(lir->value());
  Register64 temp = ToRegister64(lir->temp0());

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicEffectOp64(Synchronization::Full(), atomicOp, value, dest, temp);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicEffectOp64(Synchronization::Full(), atomicOp, value, dest, temp);
  }
}

void CodeGenerator::visitAtomicLoad64(LAtomicLoad64* lir) {
  Register elements = ToRegister(lir->elements());
  Register64 out = ToOutRegister64(lir);
  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

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

void CodeGenerator::visitWasmCompareExchangeI64(LWasmCompareExchangeI64* lir) {
  Register memoryBase = ToRegister(lir->memoryBase());
  Register ptr = ToRegister(lir->ptr());
  Register64 oldValue = ToRegister64(lir->oldValue());
  Register64 newValue = ToRegister64(lir->newValue());
  Register64 output = ToOutRegister64(lir);
  uint32_t offset = lir->mir()->access().offset32();

  BaseIndex addr(memoryBase, ptr, TimesOne, offset);
  masm.wasmCompareExchange64(lir->mir()->access(), addr, oldValue, newValue,
                             output);
}

void CodeGenerator::visitWasmAtomicExchangeI64(LWasmAtomicExchangeI64* lir) {
  Register memoryBase = ToRegister(lir->memoryBase());
  Register ptr = ToRegister(lir->ptr());
  Register64 value = ToRegister64(lir->value());
  Register64 output = ToOutRegister64(lir);
  uint32_t offset = lir->mir()->access().offset32();

  BaseIndex addr(memoryBase, ptr, TimesOne, offset);
  masm.wasmAtomicExchange64(lir->mir()->access(), addr, value, output);
}

void CodeGenerator::visitWasmAtomicBinopI64(LWasmAtomicBinopI64* lir) {
  Register memoryBase = ToRegister(lir->memoryBase());
  Register ptr = ToRegister(lir->ptr());
  Register64 value = ToRegister64(lir->value());
  Register64 output = ToOutRegister64(lir);
  Register64 temp = ToRegister64(lir->temp0());
  uint32_t offset = lir->mir()->access().offset32();

  BaseIndex addr(memoryBase, ptr, TimesOne, offset);

  masm.wasmAtomicFetchOp64(lir->mir()->access(), lir->mir()->operation(), value,
                           addr, temp, output);
}

void CodeGenerator::visitNearbyInt(LNearbyInt*) { MOZ_CRASH("NYI"); }

void CodeGenerator::visitNearbyIntF(LNearbyIntF*) { MOZ_CRASH("NYI"); }

void CodeGenerator::visitSimd128(LSimd128* ins) { MOZ_CRASH("No SIMD"); }

void CodeGenerator::visitWasmTernarySimd128(LWasmTernarySimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmBinarySimd128(LWasmBinarySimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmBinarySimd128WithConstant(
    LWasmBinarySimd128WithConstant* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmVariableShiftSimd128(
    LWasmVariableShiftSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmConstantShiftSimd128(
    LWasmConstantShiftSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmSignReplicationSimd128(
    LWasmSignReplicationSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmShuffleSimd128(LWasmShuffleSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmPermuteSimd128(LWasmPermuteSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmReplaceLaneSimd128(LWasmReplaceLaneSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmReplaceInt64LaneSimd128(
    LWasmReplaceInt64LaneSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmScalarToSimd128(LWasmScalarToSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmInt64ToSimd128(LWasmInt64ToSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmUnarySimd128(LWasmUnarySimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmReduceSimd128(LWasmReduceSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmReduceAndBranchSimd128(
    LWasmReduceAndBranchSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmReduceSimd128ToInt64(
    LWasmReduceSimd128ToInt64* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmLoadLaneSimd128(LWasmLoadLaneSimd128* ins) {
  MOZ_CRASH("No SIMD");
}

void CodeGenerator::visitWasmStoreLaneSimd128(LWasmStoreLaneSimd128* ins) {
  MOZ_CRASH("No SIMD");
}
