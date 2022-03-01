/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/CodeGenerator-x86-shared.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jsmath.h"

#include "jit/CodeGenerator.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitRuntime.h"
#include "jit/RangeAnalysis.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "util/DifferentialTesting.h"
#include "vm/TraceLogging.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Abs;
using mozilla::DebugOnly;
using mozilla::FloorLog2;
using mozilla::NegativeInfinity;

using JS::GenericNaN;

namespace js {
namespace jit {

CodeGeneratorX86Shared::CodeGeneratorX86Shared(MIRGenerator* gen,
                                               LIRGraph* graph,
                                               MacroAssembler* masm)
    : CodeGeneratorShared(gen, graph, masm) {}

#ifdef JS_PUNBOX64
Operand CodeGeneratorX86Shared::ToOperandOrRegister64(
    const LInt64Allocation input) {
  return ToOperand(input.value());
}
#else
Register64 CodeGeneratorX86Shared::ToOperandOrRegister64(
    const LInt64Allocation input) {
  return ToRegister64(input);
}
#endif

void OutOfLineBailout::accept(CodeGeneratorX86Shared* codegen) {
  codegen->visitOutOfLineBailout(this);
}

void CodeGeneratorX86Shared::emitBranch(Assembler::Condition cond,
                                        MBasicBlock* mirTrue,
                                        MBasicBlock* mirFalse,
                                        Assembler::NaNCond ifNaN) {
  if (ifNaN == Assembler::NaN_IsFalse) {
    jumpToBlock(mirFalse, Assembler::Parity);
  } else if (ifNaN == Assembler::NaN_IsTrue) {
    jumpToBlock(mirTrue, Assembler::Parity);
  }

  if (isNextBlock(mirFalse->lir())) {
    jumpToBlock(mirTrue, cond);
  } else {
    jumpToBlock(mirFalse, Assembler::InvertCondition(cond));
    jumpToBlock(mirTrue);
  }
}

void CodeGenerator::visitDouble(LDouble* ins) {
  const LDefinition* out = ins->getDef(0);
  masm.loadConstantDouble(ins->getDouble(), ToFloatRegister(out));
}

void CodeGenerator::visitFloat32(LFloat32* ins) {
  const LDefinition* out = ins->getDef(0);
  masm.loadConstantFloat32(ins->getFloat(), ToFloatRegister(out));
}

void CodeGenerator::visitTestIAndBranch(LTestIAndBranch* test) {
  Register input = ToRegister(test->input());
  masm.test32(input, input);
  emitBranch(Assembler::NonZero, test->ifTrue(), test->ifFalse());
}

void CodeGenerator::visitTestDAndBranch(LTestDAndBranch* test) {
  const LAllocation* opd = test->input();

  // vucomisd flags:
  //             Z  P  C
  //            ---------
  //      NaN    1  1  1
  //        >    0  0  0
  //        <    0  0  1
  //        =    1  0  0
  //
  // NaN is falsey, so comparing against 0 and then using the Z flag is
  // enough to determine which branch to take.
  ScratchDoubleScope scratch(masm);
  masm.zeroDouble(scratch);
  masm.vucomisd(scratch, ToFloatRegister(opd));
  emitBranch(Assembler::NotEqual, test->ifTrue(), test->ifFalse());
}

void CodeGenerator::visitTestFAndBranch(LTestFAndBranch* test) {
  const LAllocation* opd = test->input();
  // vucomiss flags are the same as doubles; see comment above
  {
    ScratchFloat32Scope scratch(masm);
    masm.zeroFloat32(scratch);
    masm.vucomiss(scratch, ToFloatRegister(opd));
  }
  emitBranch(Assembler::NotEqual, test->ifTrue(), test->ifFalse());
}

void CodeGenerator::visitBitAndAndBranch(LBitAndAndBranch* baab) {
  if (baab->right()->isConstant()) {
    masm.test32(ToRegister(baab->left()), Imm32(ToInt32(baab->right())));
  } else {
    masm.test32(ToRegister(baab->left()), ToRegister(baab->right()));
  }
  emitBranch(baab->cond(), baab->ifTrue(), baab->ifFalse());
}

void CodeGeneratorX86Shared::emitCompare(MCompare::CompareType type,
                                         const LAllocation* left,
                                         const LAllocation* right) {
#ifdef JS_CODEGEN_X64
  if (type == MCompare::Compare_Object || type == MCompare::Compare_Symbol ||
      type == MCompare::Compare_UIntPtr) {
    if (right->isConstant()) {
      MOZ_ASSERT(type == MCompare::Compare_UIntPtr);
      masm.cmpPtr(ToRegister(left), Imm32(ToInt32(right)));
    } else {
      masm.cmpPtr(ToRegister(left), ToOperand(right));
    }
    return;
  }
#endif

  if (right->isConstant()) {
    masm.cmp32(ToRegister(left), Imm32(ToInt32(right)));
  } else {
    masm.cmp32(ToRegister(left), ToOperand(right));
  }
}

void CodeGenerator::visitCompare(LCompare* comp) {
  MCompare* mir = comp->mir();
  emitCompare(mir->compareType(), comp->left(), comp->right());
  masm.emitSet(JSOpToCondition(mir->compareType(), comp->jsop()),
               ToRegister(comp->output()));
}

void CodeGenerator::visitCompareAndBranch(LCompareAndBranch* comp) {
  MCompare* mir = comp->cmpMir();
  emitCompare(mir->compareType(), comp->left(), comp->right());
  Assembler::Condition cond = JSOpToCondition(mir->compareType(), comp->jsop());
  emitBranch(cond, comp->ifTrue(), comp->ifFalse());
}

void CodeGenerator::visitCompareD(LCompareD* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());

  Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
  if (comp->mir()->operandsAreNeverNaN()) {
    nanCond = Assembler::NaN_HandledByCond;
  }

  masm.compareDouble(cond, lhs, rhs);
  masm.emitSet(Assembler::ConditionFromDoubleCondition(cond),
               ToRegister(comp->output()), nanCond);
}

void CodeGenerator::visitCompareF(LCompareF* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());

  Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
  if (comp->mir()->operandsAreNeverNaN()) {
    nanCond = Assembler::NaN_HandledByCond;
  }

  masm.compareFloat(cond, lhs, rhs);
  masm.emitSet(Assembler::ConditionFromDoubleCondition(cond),
               ToRegister(comp->output()), nanCond);
}

void CodeGenerator::visitNotI(LNotI* ins) {
  masm.cmp32(ToRegister(ins->input()), Imm32(0));
  masm.emitSet(Assembler::Equal, ToRegister(ins->output()));
}

void CodeGenerator::visitNotD(LNotD* ins) {
  FloatRegister opd = ToFloatRegister(ins->input());

  // Not returns true if the input is a NaN. We don't have to worry about
  // it if we know the input is never NaN though.
  Assembler::NaNCond nanCond = Assembler::NaN_IsTrue;
  if (ins->mir()->operandIsNeverNaN()) {
    nanCond = Assembler::NaN_HandledByCond;
  }

  ScratchDoubleScope scratch(masm);
  masm.zeroDouble(scratch);
  masm.compareDouble(Assembler::DoubleEqualOrUnordered, opd, scratch);
  masm.emitSet(Assembler::Equal, ToRegister(ins->output()), nanCond);
}

void CodeGenerator::visitNotF(LNotF* ins) {
  FloatRegister opd = ToFloatRegister(ins->input());

  // Not returns true if the input is a NaN. We don't have to worry about
  // it if we know the input is never NaN though.
  Assembler::NaNCond nanCond = Assembler::NaN_IsTrue;
  if (ins->mir()->operandIsNeverNaN()) {
    nanCond = Assembler::NaN_HandledByCond;
  }

  ScratchFloat32Scope scratch(masm);
  masm.zeroFloat32(scratch);
  masm.compareFloat(Assembler::DoubleEqualOrUnordered, opd, scratch);
  masm.emitSet(Assembler::Equal, ToRegister(ins->output()), nanCond);
}

void CodeGenerator::visitCompareDAndBranch(LCompareDAndBranch* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond =
      JSOpToDoubleCondition(comp->cmpMir()->jsop());

  Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
  if (comp->cmpMir()->operandsAreNeverNaN()) {
    nanCond = Assembler::NaN_HandledByCond;
  }

  masm.compareDouble(cond, lhs, rhs);
  emitBranch(Assembler::ConditionFromDoubleCondition(cond), comp->ifTrue(),
             comp->ifFalse(), nanCond);
}

void CodeGenerator::visitCompareFAndBranch(LCompareFAndBranch* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond =
      JSOpToDoubleCondition(comp->cmpMir()->jsop());

  Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
  if (comp->cmpMir()->operandsAreNeverNaN()) {
    nanCond = Assembler::NaN_HandledByCond;
  }

  masm.compareFloat(cond, lhs, rhs);
  emitBranch(Assembler::ConditionFromDoubleCondition(cond), comp->ifTrue(),
             comp->ifFalse(), nanCond);
}

void CodeGenerator::visitWasmStackArg(LWasmStackArg* ins) {
  const MWasmStackArg* mir = ins->mir();
  Address dst(StackPointer, mir->spOffset());
  if (ins->arg()->isConstant()) {
    masm.storePtr(ImmWord(ToInt32(ins->arg())), dst);
  } else if (ins->arg()->isGeneralReg()) {
    masm.storePtr(ToRegister(ins->arg()), dst);
  } else {
    switch (mir->input()->type()) {
      case MIRType::Double:
        masm.storeDouble(ToFloatRegister(ins->arg()), dst);
        return;
      case MIRType::Float32:
        masm.storeFloat32(ToFloatRegister(ins->arg()), dst);
        return;
#ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
        masm.storeUnalignedSimd128(ToFloatRegister(ins->arg()), dst);
        return;
#endif
      default:
        break;
    }
    MOZ_CRASH("unexpected mir type in WasmStackArg");
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
  Operand falseExpr = ToOperand(ins->falseExpr());

  masm.test32(cond, cond);

  if (mirType == MIRType::Int32 || mirType == MIRType::RefOrNull) {
    Register out = ToRegister(ins->output());
    MOZ_ASSERT(ToRegister(ins->trueExpr()) == out,
               "true expr input is reused for output");
    if (mirType == MIRType::Int32) {
      masm.cmovz32(falseExpr, out);
    } else {
      masm.cmovzPtr(falseExpr, out);
    }
    return;
  }

  FloatRegister out = ToFloatRegister(ins->output());
  MOZ_ASSERT(ToFloatRegister(ins->trueExpr()) == out,
             "true expr input is reused for output");

  Label done;
  masm.j(Assembler::NonZero, &done);

  if (mirType == MIRType::Float32) {
    if (falseExpr.kind() == Operand::FPREG) {
      masm.moveFloat32(ToFloatRegister(ins->falseExpr()), out);
    } else {
      masm.loadFloat32(falseExpr, out);
    }
  } else if (mirType == MIRType::Double) {
    if (falseExpr.kind() == Operand::FPREG) {
      masm.moveDouble(ToFloatRegister(ins->falseExpr()), out);
    } else {
      masm.loadDouble(falseExpr, out);
    }
  } else if (mirType == MIRType::Simd128) {
    if (falseExpr.kind() == Operand::FPREG) {
      masm.moveSimd128(ToFloatRegister(ins->falseExpr()), out);
    } else {
      masm.loadUnalignedSimd128(falseExpr, out);
    }
  } else {
    MOZ_CRASH("unhandled type in visitWasmSelect!");
  }

  masm.bind(&done);
}

void CodeGenerator::visitWasmCompareAndSelect(LWasmCompareAndSelect* ins) {
  emitWasmCompareAndSelect(ins);
}

void CodeGenerator::visitWasmReinterpret(LWasmReinterpret* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  MWasmReinterpret* ins = lir->mir();

  MIRType to = ins->type();
#ifdef DEBUG
  MIRType from = ins->input()->type();
#endif

  switch (to) {
    case MIRType::Int32:
      MOZ_ASSERT(from == MIRType::Float32);
      masm.vmovd(ToFloatRegister(lir->input()), ToRegister(lir->output()));
      break;
    case MIRType::Float32:
      MOZ_ASSERT(from == MIRType::Int32);
      masm.vmovd(ToRegister(lir->input()), ToFloatRegister(lir->output()));
      break;
    case MIRType::Double:
    case MIRType::Int64:
      MOZ_CRASH("not handled by this LIR opcode");
    default:
      MOZ_CRASH("unexpected WasmReinterpret");
  }
}

void CodeGenerator::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins) {
  const MAsmJSLoadHeap* mir = ins->mir();
  MOZ_ASSERT(mir->access().offset() == 0);

  const LAllocation* ptr = ins->ptr();
  const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();
  AnyRegister out = ToAnyRegister(ins->output());

  Scalar::Type accessType = mir->accessType();

  OutOfLineLoadTypedArrayOutOfBounds* ool = nullptr;
  if (mir->needsBoundsCheck()) {
    ool = new (alloc()) OutOfLineLoadTypedArrayOutOfBounds(out, accessType);
    addOutOfLineCode(ool, mir);

    masm.wasmBoundsCheck32(Assembler::AboveOrEqual, ToRegister(ptr),
                           ToRegister(boundsCheckLimit), ool->entry());
  }

  Operand srcAddr = toMemoryAccessOperand(ins, 0);
  masm.wasmLoad(mir->access(), srcAddr, out);

  if (ool) {
    masm.bind(ool->rejoin());
  }
}

void CodeGeneratorX86Shared::visitOutOfLineLoadTypedArrayOutOfBounds(
    OutOfLineLoadTypedArrayOutOfBounds* ool) {
  switch (ool->viewType()) {
    case Scalar::Int64:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::Simd128:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected array type");
    case Scalar::Float32:
      masm.loadConstantFloat32(float(GenericNaN()), ool->dest().fpu());
      break;
    case Scalar::Float64:
      masm.loadConstantDouble(GenericNaN(), ool->dest().fpu());
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Uint8Clamped:
      Register destReg = ool->dest().gpr();
      masm.mov(ImmWord(0), destReg);
      break;
  }
  masm.jmp(ool->rejoin());
}

void CodeGenerator::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins) {
  const MAsmJSStoreHeap* mir = ins->mir();

  const LAllocation* ptr = ins->ptr();
  const LAllocation* value = ins->value();
  const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();

  Scalar::Type accessType = mir->accessType();
  canonicalizeIfDeterministic(accessType, value);

  Label rejoin;
  if (mir->needsBoundsCheck()) {
    masm.wasmBoundsCheck32(Assembler::AboveOrEqual, ToRegister(ptr),
                           ToRegister(boundsCheckLimit), &rejoin);
  }

  Operand dstAddr = toMemoryAccessOperand(ins, 0);
  masm.wasmStore(mir->access(), ToAnyRegister(value), dstAddr);

  if (rejoin.used()) {
    masm.bind(&rejoin);
  }
}

void CodeGenerator::visitWasmAddOffset(LWasmAddOffset* lir) {
  MWasmAddOffset* mir = lir->mir();
  Register base = ToRegister(lir->base());
  Register out = ToRegister(lir->output());

  if (base != out) {
    masm.move32(base, out);
  }
  masm.add32(Imm32(mir->offset()), out);

  Label ok;
  masm.j(Assembler::CarryClear, &ok);
  masm.wasmTrap(wasm::Trap::OutOfBounds, mir->bytecodeOffset());
  masm.bind(&ok);
}

void CodeGenerator::visitWasmTruncateToInt32(LWasmTruncateToInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register output = ToRegister(lir->output());

  MWasmTruncateToInt32* mir = lir->mir();
  MIRType inputType = mir->input()->type();

  MOZ_ASSERT(inputType == MIRType::Double || inputType == MIRType::Float32);

  auto* ool = new (alloc()) OutOfLineWasmTruncateCheck(mir, input, output);
  addOutOfLineCode(ool, mir);

  Label* oolEntry = ool->entry();
  if (mir->isUnsigned()) {
    if (inputType == MIRType::Double) {
      masm.wasmTruncateDoubleToUInt32(input, output, mir->isSaturating(),
                                      oolEntry);
    } else if (inputType == MIRType::Float32) {
      masm.wasmTruncateFloat32ToUInt32(input, output, mir->isSaturating(),
                                       oolEntry);
    } else {
      MOZ_CRASH("unexpected type");
    }
    if (mir->isSaturating()) {
      masm.bind(ool->rejoin());
    }
    return;
  }

  if (inputType == MIRType::Double) {
    masm.wasmTruncateDoubleToInt32(input, output, mir->isSaturating(),
                                   oolEntry);
  } else if (inputType == MIRType::Float32) {
    masm.wasmTruncateFloat32ToInt32(input, output, mir->isSaturating(),
                                    oolEntry);
  } else {
    MOZ_CRASH("unexpected type");
  }

  masm.bind(ool->rejoin());
}

bool CodeGeneratorX86Shared::generateOutOfLineCode() {
  if (!CodeGeneratorShared::generateOutOfLineCode()) {
    return false;
  }

  if (deoptLabel_.used()) {
    // All non-table-based bailouts will go here.
    masm.bind(&deoptLabel_);

    // Push the frame size, so the handler can recover the IonScript.
    masm.push(Imm32(frameSize()));

    TrampolinePtr handler = gen->jitRuntime()->getGenericBailoutHandler();
    masm.jump(handler);
  }

  return !masm.oom();
}

class BailoutJump {
  Assembler::Condition cond_;

 public:
  explicit BailoutJump(Assembler::Condition cond) : cond_(cond) {}
#ifdef JS_CODEGEN_X86
  void operator()(MacroAssembler& masm, uint8_t* code) const {
    masm.j(cond_, ImmPtr(code), RelocationKind::HARDCODED);
  }
#endif
  void operator()(MacroAssembler& masm, Label* label) const {
    masm.j(cond_, label);
  }
};

class BailoutLabel {
  Label* label_;

 public:
  explicit BailoutLabel(Label* label) : label_(label) {}
#ifdef JS_CODEGEN_X86
  void operator()(MacroAssembler& masm, uint8_t* code) const {
    masm.retarget(label_, ImmPtr(code), RelocationKind::HARDCODED);
  }
#endif
  void operator()(MacroAssembler& masm, Label* label) const {
    masm.retarget(label_, label);
  }
};

template <typename T>
void CodeGeneratorX86Shared::bailout(const T& binder, LSnapshot* snapshot) {
  encode(snapshot);

  // Though the assembler doesn't track all frame pushes, at least make sure
  // the known value makes sense. We can't use bailout tables if the stack
  // isn't properly aligned to the static frame size.
  MOZ_ASSERT_IF(frameClass_ != FrameSizeClass::None() && deoptTable_,
                frameClass_.frameSize() == masm.framePushed());

#ifdef JS_CODEGEN_X86
  // On x64, bailout tables are pointless, because 16 extra bytes are
  // reserved per external jump, whereas it takes only 10 bytes to encode a
  // a non-table based bailout.
  if (assignBailoutId(snapshot)) {
    binder(masm, deoptTable_->value +
                     snapshot->bailoutId() * BAILOUT_TABLE_ENTRY_SIZE);
    return;
  }
#endif

  // We could not use a jump table, either because all bailout IDs were
  // reserved, or a jump table is not optimal for this frame size or
  // platform. Whatever, we will generate a lazy bailout.
  //
  // All bailout code is associated with the bytecodeSite of the block we are
  // bailing out from.
  InlineScriptTree* tree = snapshot->mir()->block()->trackedTree();
  OutOfLineBailout* ool = new (alloc()) OutOfLineBailout(snapshot);
  addOutOfLineCode(ool,
                   new (alloc()) BytecodeSite(tree, tree->script()->code()));

  binder(masm, ool->entry());
}

void CodeGeneratorX86Shared::bailoutIf(Assembler::Condition condition,
                                       LSnapshot* snapshot) {
  bailout(BailoutJump(condition), snapshot);
}

void CodeGeneratorX86Shared::bailoutIf(Assembler::DoubleCondition condition,
                                       LSnapshot* snapshot) {
  MOZ_ASSERT(Assembler::NaNCondFromDoubleCondition(condition) ==
             Assembler::NaN_HandledByCond);
  bailoutIf(Assembler::ConditionFromDoubleCondition(condition), snapshot);
}

void CodeGeneratorX86Shared::bailoutFrom(Label* label, LSnapshot* snapshot) {
  MOZ_ASSERT_IF(!masm.oom(), label->used() && !label->bound());
  bailout(BailoutLabel(label), snapshot);
}

void CodeGeneratorX86Shared::bailout(LSnapshot* snapshot) {
  Label label;
  masm.jump(&label);
  bailoutFrom(&label, snapshot);
}

void CodeGeneratorX86Shared::visitOutOfLineBailout(OutOfLineBailout* ool) {
  masm.push(Imm32(ool->snapshot()->snapshotOffset()));
  masm.jmp(&deoptLabel_);
}

void CodeGenerator::visitMinMaxD(LMinMaxD* ins) {
  FloatRegister first = ToFloatRegister(ins->first());
  FloatRegister second = ToFloatRegister(ins->second());
#ifdef DEBUG
  FloatRegister output = ToFloatRegister(ins->output());
  MOZ_ASSERT(first == output);
#endif

  bool handleNaN = !ins->mir()->range() || ins->mir()->range()->canBeNaN();

  if (ins->mir()->isMax()) {
    masm.maxDouble(second, first, handleNaN);
  } else {
    masm.minDouble(second, first, handleNaN);
  }
}

void CodeGenerator::visitMinMaxF(LMinMaxF* ins) {
  FloatRegister first = ToFloatRegister(ins->first());
  FloatRegister second = ToFloatRegister(ins->second());
#ifdef DEBUG
  FloatRegister output = ToFloatRegister(ins->output());
  MOZ_ASSERT(first == output);
#endif

  bool handleNaN = !ins->mir()->range() || ins->mir()->range()->canBeNaN();

  if (ins->mir()->isMax()) {
    masm.maxFloat32(second, first, handleNaN);
  } else {
    masm.minFloat32(second, first, handleNaN);
  }
}

void CodeGenerator::visitClzI(LClzI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());
  bool knownNotZero = ins->mir()->operandIsNeverZero();

  masm.clz32(input, output, knownNotZero);
}

void CodeGenerator::visitCtzI(LCtzI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());
  bool knownNotZero = ins->mir()->operandIsNeverZero();

  masm.ctz32(input, output, knownNotZero);
}

void CodeGenerator::visitPopcntI(LPopcntI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());
  Register temp =
      ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());

  masm.popcnt32(input, output, temp);
}

void CodeGenerator::visitPowHalfD(LPowHalfD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());

  ScratchDoubleScope scratch(masm);

  Label done, sqrt;

  if (!ins->mir()->operandIsNeverNegativeInfinity()) {
    // Branch if not -Infinity.
    masm.loadConstantDouble(NegativeInfinity<double>(), scratch);

    Assembler::DoubleCondition cond = Assembler::DoubleNotEqualOrUnordered;
    if (ins->mir()->operandIsNeverNaN()) {
      cond = Assembler::DoubleNotEqual;
    }
    masm.branchDouble(cond, input, scratch, &sqrt);

    // Math.pow(-Infinity, 0.5) == Infinity.
    masm.zeroDouble(output);
    masm.subDouble(scratch, output);
    masm.jump(&done);

    masm.bind(&sqrt);
  }

  if (!ins->mir()->operandIsNeverNegativeZero()) {
    // Math.pow(-0, 0.5) == 0 == Math.pow(0, 0.5).
    // Adding 0 converts any -0 to 0.
    masm.zeroDouble(scratch);
    masm.addDouble(input, scratch);
    masm.vsqrtsd(scratch, output, output);
  } else {
    masm.vsqrtsd(input, output, output);
  }

  masm.bind(&done);
}

class OutOfLineUndoALUOperation
    : public OutOfLineCodeBase<CodeGeneratorX86Shared> {
  LInstruction* ins_;

 public:
  explicit OutOfLineUndoALUOperation(LInstruction* ins) : ins_(ins) {}

  virtual void accept(CodeGeneratorX86Shared* codegen) override {
    codegen->visitOutOfLineUndoALUOperation(this);
  }
  LInstruction* ins() const { return ins_; }
};

void CodeGenerator::visitAddI(LAddI* ins) {
  if (ins->rhs()->isConstant()) {
    masm.addl(Imm32(ToInt32(ins->rhs())), ToOperand(ins->lhs()));
  } else {
    masm.addl(ToOperand(ins->rhs()), ToRegister(ins->lhs()));
  }

  if (ins->snapshot()) {
    if (ins->recoversInput()) {
      OutOfLineUndoALUOperation* ool =
          new (alloc()) OutOfLineUndoALUOperation(ins);
      addOutOfLineCode(ool, ins->mir());
      masm.j(Assembler::Overflow, ool->entry());
    } else {
      bailoutIf(Assembler::Overflow, ins->snapshot());
    }
  }
}

void CodeGenerator::visitAddI64(LAddI64* lir) {
  const LInt64Allocation lhs = lir->getInt64Operand(LAddI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LAddI64::Rhs);

  MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

  if (IsConstant(rhs)) {
    masm.add64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
    return;
  }

  masm.add64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
}

void CodeGenerator::visitSubI(LSubI* ins) {
  if (ins->rhs()->isConstant()) {
    masm.subl(Imm32(ToInt32(ins->rhs())), ToOperand(ins->lhs()));
  } else {
    masm.subl(ToOperand(ins->rhs()), ToRegister(ins->lhs()));
  }

  if (ins->snapshot()) {
    if (ins->recoversInput()) {
      OutOfLineUndoALUOperation* ool =
          new (alloc()) OutOfLineUndoALUOperation(ins);
      addOutOfLineCode(ool, ins->mir());
      masm.j(Assembler::Overflow, ool->entry());
    } else {
      bailoutIf(Assembler::Overflow, ins->snapshot());
    }
  }
}

void CodeGenerator::visitSubI64(LSubI64* lir) {
  const LInt64Allocation lhs = lir->getInt64Operand(LSubI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LSubI64::Rhs);

  MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

  if (IsConstant(rhs)) {
    masm.sub64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
    return;
  }

  masm.sub64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
}

void CodeGeneratorX86Shared::visitOutOfLineUndoALUOperation(
    OutOfLineUndoALUOperation* ool) {
  LInstruction* ins = ool->ins();
  Register reg = ToRegister(ins->getDef(0));

  DebugOnly<LAllocation*> lhs = ins->getOperand(0);
  LAllocation* rhs = ins->getOperand(1);

  MOZ_ASSERT(reg == ToRegister(lhs));
  MOZ_ASSERT_IF(rhs->isGeneralReg(), reg != ToRegister(rhs));

  // Undo the effect of the ALU operation, which was performed on the output
  // register and overflowed. Writing to the output register clobbered an
  // input reg, and the original value of the input needs to be recovered
  // to satisfy the constraint imposed by any RECOVERED_INPUT operands to
  // the bailout snapshot.

  if (rhs->isConstant()) {
    Imm32 constant(ToInt32(rhs));
    if (ins->isAddI()) {
      masm.subl(constant, reg);
    } else {
      masm.addl(constant, reg);
    }
  } else {
    if (ins->isAddI()) {
      masm.subl(ToOperand(rhs), reg);
    } else {
      masm.addl(ToOperand(rhs), reg);
    }
  }

  bailout(ool->ins()->snapshot());
}

class MulNegativeZeroCheck : public OutOfLineCodeBase<CodeGeneratorX86Shared> {
  LMulI* ins_;

 public:
  explicit MulNegativeZeroCheck(LMulI* ins) : ins_(ins) {}

  virtual void accept(CodeGeneratorX86Shared* codegen) override {
    codegen->visitMulNegativeZeroCheck(this);
  }
  LMulI* ins() const { return ins_; }
};

void CodeGenerator::visitMulI(LMulI* ins) {
  const LAllocation* lhs = ins->lhs();
  const LAllocation* rhs = ins->rhs();
  MMul* mul = ins->mir();
  MOZ_ASSERT_IF(mul->mode() == MMul::Integer,
                !mul->canBeNegativeZero() && !mul->canOverflow());

  if (rhs->isConstant()) {
    // Bailout on -0.0
    int32_t constant = ToInt32(rhs);
    if (mul->canBeNegativeZero() && constant <= 0) {
      Assembler::Condition bailoutCond =
          (constant == 0) ? Assembler::Signed : Assembler::Equal;
      masm.test32(ToRegister(lhs), ToRegister(lhs));
      bailoutIf(bailoutCond, ins->snapshot());
    }

    switch (constant) {
      case -1:
        masm.negl(ToOperand(lhs));
        break;
      case 0:
        masm.xorl(ToOperand(lhs), ToRegister(lhs));
        return;  // escape overflow check;
      case 1:
        // nop
        return;  // escape overflow check;
      case 2:
        masm.addl(ToOperand(lhs), ToRegister(lhs));
        break;
      default:
        if (!mul->canOverflow() && constant > 0) {
          // Use shift if cannot overflow and constant is power of 2
          int32_t shift = FloorLog2(constant);
          if ((1 << shift) == constant) {
            masm.shll(Imm32(shift), ToRegister(lhs));
            return;
          }
        }
        masm.imull(Imm32(ToInt32(rhs)), ToRegister(lhs));
    }

    // Bailout on overflow
    if (mul->canOverflow()) {
      bailoutIf(Assembler::Overflow, ins->snapshot());
    }
  } else {
    masm.imull(ToOperand(rhs), ToRegister(lhs));

    // Bailout on overflow
    if (mul->canOverflow()) {
      bailoutIf(Assembler::Overflow, ins->snapshot());
    }

    if (mul->canBeNegativeZero()) {
      // Jump to an OOL path if the result is 0.
      MulNegativeZeroCheck* ool = new (alloc()) MulNegativeZeroCheck(ins);
      addOutOfLineCode(ool, mul);

      masm.test32(ToRegister(lhs), ToRegister(lhs));
      masm.j(Assembler::Zero, ool->entry());
      masm.bind(ool->rejoin());
    }
  }
}

void CodeGenerator::visitMulI64(LMulI64* lir) {
  const LInt64Allocation lhs = lir->getInt64Operand(LMulI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LMulI64::Rhs);

  MOZ_ASSERT(ToRegister64(lhs) == ToOutRegister64(lir));

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
        masm.add64(ToRegister64(lhs), ToRegister64(lhs));
        return;
      default:
        if (constant > 0) {
          // Use shift if constant is power of 2.
          int32_t shift = mozilla::FloorLog2(constant);
          if (int64_t(1) << shift == constant) {
            masm.lshift64(Imm32(shift), ToRegister64(lhs));
            return;
          }
        }
        Register temp = ToTempRegisterOrInvalid(lir->temp());
        masm.mul64(Imm64(constant), ToRegister64(lhs), temp);
    }
  } else {
    Register temp = ToTempRegisterOrInvalid(lir->temp());
    masm.mul64(ToOperandOrRegister64(rhs), ToRegister64(lhs), temp);
  }
}

class ReturnZero : public OutOfLineCodeBase<CodeGeneratorX86Shared> {
  Register reg_;

 public:
  explicit ReturnZero(Register reg) : reg_(reg) {}

  virtual void accept(CodeGeneratorX86Shared* codegen) override {
    codegen->visitReturnZero(this);
  }
  Register reg() const { return reg_; }
};

void CodeGeneratorX86Shared::visitReturnZero(ReturnZero* ool) {
  masm.mov(ImmWord(0), ool->reg());
  masm.jmp(ool->rejoin());
}

void CodeGenerator::visitUDivOrMod(LUDivOrMod* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());

  MOZ_ASSERT_IF(lhs != rhs, rhs != eax);
  MOZ_ASSERT(rhs != edx);
  MOZ_ASSERT_IF(output == eax, ToRegister(ins->remainder()) == edx);

  ReturnZero* ool = nullptr;

  // Put the lhs in eax.
  if (lhs != eax) {
    masm.mov(lhs, eax);
  }

  // Prevent divide by zero.
  if (ins->canBeDivideByZero()) {
    masm.test32(rhs, rhs);
    if (ins->mir()->isTruncated()) {
      if (ins->trapOnError()) {
        Label nonZero;
        masm.j(Assembler::NonZero, &nonZero);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, ins->bytecodeOffset());
        masm.bind(&nonZero);
      } else {
        ool = new (alloc()) ReturnZero(output);
        masm.j(Assembler::Zero, ool->entry());
      }
    } else {
      bailoutIf(Assembler::Zero, ins->snapshot());
    }
  }

  // Zero extend the lhs into edx to make (edx:eax), since udiv is 64-bit.
  masm.mov(ImmWord(0), edx);
  masm.udiv(rhs);

  // If the remainder is > 0, bailout since this must be a double.
  if (ins->mir()->isDiv() && !ins->mir()->toDiv()->canTruncateRemainder()) {
    Register remainder = ToRegister(ins->remainder());
    masm.test32(remainder, remainder);
    bailoutIf(Assembler::NonZero, ins->snapshot());
  }

  // Unsigned div or mod can return a value that's not a signed int32.
  // If our users aren't expecting that, bail.
  if (!ins->mir()->isTruncated()) {
    masm.test32(output, output);
    bailoutIf(Assembler::Signed, ins->snapshot());
  }

  if (ool) {
    addOutOfLineCode(ool, ins->mir());
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitUDivOrModConstant(LUDivOrModConstant* ins) {
  Register lhs = ToRegister(ins->numerator());
  Register output = ToRegister(ins->output());
  uint32_t d = ins->denominator();

  // This emits the division answer into edx or the modulus answer into eax.
  MOZ_ASSERT(output == eax || output == edx);
  MOZ_ASSERT(lhs != eax && lhs != edx);
  bool isDiv = (output == edx);

  if (d == 0) {
    if (ins->mir()->isTruncated()) {
      if (ins->trapOnError()) {
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, ins->bytecodeOffset());
      } else {
        masm.xorl(output, output);
      }
    } else {
      bailout(ins->snapshot());
    }
    return;
  }

  // The denominator isn't a power of 2 (see LDivPowTwoI and LModPowTwoI).
  MOZ_ASSERT((d & (d - 1)) != 0);

  ReciprocalMulConstants rmc = computeDivisionConstants(d, /* maxLog = */ 32);

  // We first compute (M * n) >> 32, where M = rmc.multiplier.
  masm.movl(Imm32(rmc.multiplier), eax);
  masm.umull(lhs);
  if (rmc.multiplier > UINT32_MAX) {
    // M >= 2^32 and shift == 0 is impossible, as d >= 2 implies that
    // ((M * n) >> (32 + shift)) >= n > floor(n/d) whenever n >= d,
    // contradicting the proof of correctness in computeDivisionConstants.
    MOZ_ASSERT(rmc.shiftAmount > 0);
    MOZ_ASSERT(rmc.multiplier < (int64_t(1) << 33));

    // We actually computed edx = ((uint32_t(M) * n) >> 32) instead. Since
    // (M * n) >> (32 + shift) is the same as (edx + n) >> shift, we can
    // correct for the overflow. This case is a bit trickier than the signed
    // case, though, as the (edx + n) addition itself can overflow; however,
    // note that (edx + n) >> shift == (((n - edx) >> 1) + edx) >> (shift - 1),
    // which is overflow-free. See Hacker's Delight, section 10-8 for details.

    // Compute (n - edx) >> 1 into eax.
    masm.movl(lhs, eax);
    masm.subl(edx, eax);
    masm.shrl(Imm32(1), eax);

    // Finish the computation.
    masm.addl(eax, edx);
    masm.shrl(Imm32(rmc.shiftAmount - 1), edx);
  } else {
    masm.shrl(Imm32(rmc.shiftAmount), edx);
  }

  // We now have the truncated division value in edx. If we're
  // computing a modulus or checking whether the division resulted
  // in an integer, we need to multiply the obtained value by d and
  // finish the computation/check.
  if (!isDiv) {
    masm.imull(Imm32(d), edx, edx);
    masm.movl(lhs, eax);
    masm.subl(edx, eax);

    // The final result of the modulus op, just computed above by the
    // sub instruction, can be a number in the range [2^31, 2^32). If
    // this is the case and the modulus is not truncated, we must bail
    // out.
    if (!ins->mir()->isTruncated()) {
      bailoutIf(Assembler::Signed, ins->snapshot());
    }
  } else if (!ins->mir()->isTruncated()) {
    masm.imull(Imm32(d), edx, eax);
    masm.cmpl(lhs, eax);
    bailoutIf(Assembler::NotEqual, ins->snapshot());
  }
}

void CodeGeneratorX86Shared::visitMulNegativeZeroCheck(
    MulNegativeZeroCheck* ool) {
  LMulI* ins = ool->ins();
  Register result = ToRegister(ins->output());
  Operand lhsCopy = ToOperand(ins->lhsCopy());
  Operand rhs = ToOperand(ins->rhs());
  MOZ_ASSERT_IF(lhsCopy.kind() == Operand::REG, lhsCopy.reg() != result.code());

  // Result is -0 if lhs or rhs is negative.
  masm.movl(lhsCopy, result);
  masm.orl(rhs, result);
  bailoutIf(Assembler::Signed, ins->snapshot());

  masm.mov(ImmWord(0), result);
  masm.jmp(ool->rejoin());
}

void CodeGenerator::visitDivPowTwoI(LDivPowTwoI* ins) {
  Register lhs = ToRegister(ins->numerator());
  DebugOnly<Register> output = ToRegister(ins->output());

  int32_t shift = ins->shift();
  bool negativeDivisor = ins->negativeDivisor();
  MDiv* mir = ins->mir();

  // We use defineReuseInput so these should always be the same, which is
  // convenient since all of our instructions here are two-address.
  MOZ_ASSERT(lhs == output);

  if (!mir->isTruncated() && negativeDivisor) {
    // 0 divided by a negative number must return a double.
    masm.test32(lhs, lhs);
    bailoutIf(Assembler::Zero, ins->snapshot());
  }

  if (shift) {
    if (!mir->isTruncated()) {
      // If the remainder is != 0, bailout since this must be a double.
      masm.test32(lhs, Imm32(UINT32_MAX >> (32 - shift)));
      bailoutIf(Assembler::NonZero, ins->snapshot());
    }

    if (mir->isUnsigned()) {
      masm.shrl(Imm32(shift), lhs);
    } else {
      // Adjust the value so that shifting produces a correctly
      // rounded result when the numerator is negative. See 10-1
      // "Signed Division by a Known Power of 2" in Henry
      // S. Warren, Jr.'s Hacker's Delight.
      if (mir->canBeNegativeDividend() && mir->isTruncated()) {
        // Note: There is no need to execute this code, which handles how to
        // round the signed integer division towards 0, if we previously bailed
        // due to a non-zero remainder.
        Register lhsCopy = ToRegister(ins->numeratorCopy());
        MOZ_ASSERT(lhsCopy != lhs);
        if (shift > 1) {
          // Copy the sign bit of the numerator. (= (2^32 - 1) or 0)
          masm.sarl(Imm32(31), lhs);
        }
        // Divide by 2^(32 - shift)
        // i.e. (= (2^32 - 1) / 2^(32 - shift) or 0)
        // i.e. (= (2^shift - 1) or 0)
        masm.shrl(Imm32(32 - shift), lhs);
        // If signed, make any 1 bit below the shifted bits to bubble up, such
        // that once shifted the value would be rounded towards 0.
        masm.addl(lhsCopy, lhs);
      }
      masm.sarl(Imm32(shift), lhs);

      if (negativeDivisor) {
        masm.negl(lhs);
      }
    }
    return;
  }

  if (negativeDivisor) {
    // INT32_MIN / -1 overflows.
    masm.negl(lhs);
    if (!mir->isTruncated()) {
      bailoutIf(Assembler::Overflow, ins->snapshot());
    } else if (mir->trapOnError()) {
      Label ok;
      masm.j(Assembler::NoOverflow, &ok);
      masm.wasmTrap(wasm::Trap::IntegerOverflow, mir->bytecodeOffset());
      masm.bind(&ok);
    }
  } else if (mir->isUnsigned() && !mir->isTruncated()) {
    // Unsigned division by 1 can overflow if output is not
    // truncated.
    masm.test32(lhs, lhs);
    bailoutIf(Assembler::Signed, ins->snapshot());
  }
}

void CodeGenerator::visitDivOrModConstantI(LDivOrModConstantI* ins) {
  Register lhs = ToRegister(ins->numerator());
  Register output = ToRegister(ins->output());
  int32_t d = ins->denominator();

  // This emits the division answer into edx or the modulus answer into eax.
  MOZ_ASSERT(output == eax || output == edx);
  MOZ_ASSERT(lhs != eax && lhs != edx);
  bool isDiv = (output == edx);

  // The absolute value of the denominator isn't a power of 2 (see LDivPowTwoI
  // and LModPowTwoI).
  MOZ_ASSERT((Abs(d) & (Abs(d) - 1)) != 0);

  // We will first divide by Abs(d), and negate the answer if d is negative.
  // If desired, this can be avoided by generalizing computeDivisionConstants.
  ReciprocalMulConstants rmc =
      computeDivisionConstants(Abs(d), /* maxLog = */ 31);

  // We first compute (M * n) >> 32, where M = rmc.multiplier.
  masm.movl(Imm32(rmc.multiplier), eax);
  masm.imull(lhs);
  if (rmc.multiplier > INT32_MAX) {
    MOZ_ASSERT(rmc.multiplier < (int64_t(1) << 32));

    // We actually computed edx = ((int32_t(M) * n) >> 32) instead. Since
    // (M * n) >> 32 is the same as (edx + n), we can correct for the overflow.
    // (edx + n) can't overflow, as n and edx have opposite signs because
    // int32_t(M) is negative.
    masm.addl(lhs, edx);
  }
  // (M * n) >> (32 + shift) is the truncated division answer if n is
  // non-negative, as proved in the comments of computeDivisionConstants. We
  // must add 1 later if n is negative to get the right answer in all cases.
  masm.sarl(Imm32(rmc.shiftAmount), edx);

  // We'll subtract -1 instead of adding 1, because (n < 0 ? -1 : 0) can be
  // computed with just a sign-extending shift of 31 bits.
  if (ins->canBeNegativeDividend()) {
    masm.movl(lhs, eax);
    masm.sarl(Imm32(31), eax);
    masm.subl(eax, edx);
  }

  // After this, edx contains the correct truncated division result.
  if (d < 0) {
    masm.negl(edx);
  }

  if (!isDiv) {
    masm.imull(Imm32(-d), edx, eax);
    masm.addl(lhs, eax);
  }

  if (!ins->mir()->isTruncated()) {
    if (isDiv) {
      // This is a division op. Multiply the obtained value by d to check if
      // the correct answer is an integer. This cannot overflow, since |d| > 1.
      masm.imull(Imm32(d), edx, eax);
      masm.cmp32(lhs, eax);
      bailoutIf(Assembler::NotEqual, ins->snapshot());

      // If lhs is zero and the divisor is negative, the answer should have
      // been -0.
      if (d < 0) {
        masm.test32(lhs, lhs);
        bailoutIf(Assembler::Zero, ins->snapshot());
      }
    } else if (ins->canBeNegativeDividend()) {
      // This is a mod op. If the computed value is zero and lhs
      // is negative, the answer should have been -0.
      Label done;

      masm.cmp32(lhs, Imm32(0));
      masm.j(Assembler::GreaterThanOrEqual, &done);

      masm.test32(eax, eax);
      bailoutIf(Assembler::Zero, ins->snapshot());

      masm.bind(&done);
    }
  }
}

void CodeGenerator::visitDivI(LDivI* ins) {
  Register remainder = ToRegister(ins->remainder());
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());

  MDiv* mir = ins->mir();

  MOZ_ASSERT_IF(lhs != rhs, rhs != eax);
  MOZ_ASSERT(rhs != edx);
  MOZ_ASSERT(remainder == edx);
  MOZ_ASSERT(output == eax);

  Label done;
  ReturnZero* ool = nullptr;

  // Put the lhs in eax, for either the negative overflow case or the regular
  // divide case.
  if (lhs != eax) {
    masm.mov(lhs, eax);
  }

  // Handle divide by zero.
  if (mir->canBeDivideByZero()) {
    masm.test32(rhs, rhs);
    if (mir->trapOnError()) {
      Label nonZero;
      masm.j(Assembler::NonZero, &nonZero);
      masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
      masm.bind(&nonZero);
    } else if (mir->canTruncateInfinities()) {
      // Truncated division by zero is zero (Infinity|0 == 0)
      if (!ool) {
        ool = new (alloc()) ReturnZero(output);
      }
      masm.j(Assembler::Zero, ool->entry());
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Zero, ins->snapshot());
    }
  }

  // Handle an integer overflow exception from -2147483648 / -1.
  if (mir->canBeNegativeOverflow()) {
    Label notOverflow;
    masm.cmp32(lhs, Imm32(INT32_MIN));
    masm.j(Assembler::NotEqual, &notOverflow);
    masm.cmp32(rhs, Imm32(-1));
    if (mir->trapOnError()) {
      masm.j(Assembler::NotEqual, &notOverflow);
      masm.wasmTrap(wasm::Trap::IntegerOverflow, mir->bytecodeOffset());
    } else if (mir->canTruncateOverflow()) {
      // (-INT32_MIN)|0 == INT32_MIN and INT32_MIN is already in the
      // output register (lhs == eax).
      masm.j(Assembler::Equal, &done);
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Equal, ins->snapshot());
    }
    masm.bind(&notOverflow);
  }

  // Handle negative 0.
  if (!mir->canTruncateNegativeZero() && mir->canBeNegativeZero()) {
    Label nonzero;
    masm.test32(lhs, lhs);
    masm.j(Assembler::NonZero, &nonzero);
    masm.cmp32(rhs, Imm32(0));
    bailoutIf(Assembler::LessThan, ins->snapshot());
    masm.bind(&nonzero);
  }

  // Sign extend the lhs into edx to make (edx:eax), since idiv is 64-bit.
  if (lhs != eax) {
    masm.mov(lhs, eax);
  }
  masm.cdq();
  masm.idiv(rhs);

  if (!mir->canTruncateRemainder()) {
    // If the remainder is > 0, bailout since this must be a double.
    masm.test32(remainder, remainder);
    bailoutIf(Assembler::NonZero, ins->snapshot());
  }

  masm.bind(&done);

  if (ool) {
    addOutOfLineCode(ool, mir);
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitModPowTwoI(LModPowTwoI* ins) {
  Register lhs = ToRegister(ins->getOperand(0));
  int32_t shift = ins->shift();

  Label negative;

  if (!ins->mir()->isUnsigned() && ins->mir()->canBeNegativeDividend()) {
    // Switch based on sign of the lhs.
    // Positive numbers are just a bitmask
    masm.branchTest32(Assembler::Signed, lhs, lhs, &negative);
  }

  masm.andl(Imm32((uint32_t(1) << shift) - 1), lhs);

  if (!ins->mir()->isUnsigned() && ins->mir()->canBeNegativeDividend()) {
    Label done;
    masm.jump(&done);

    // Negative numbers need a negate, bitmask, negate
    masm.bind(&negative);

    // Unlike in the visitModI case, we are not computing the mod by means of a
    // division. Therefore, the divisor = -1 case isn't problematic (the andl
    // always returns 0, which is what we expect).
    //
    // The negl instruction overflows if lhs == INT32_MIN, but this is also not
    // a problem: shift is at most 31, and so the andl also always returns 0.
    masm.negl(lhs);
    masm.andl(Imm32((uint32_t(1) << shift) - 1), lhs);
    masm.negl(lhs);

    // Since a%b has the same sign as b, and a is negative in this branch,
    // an answer of 0 means the correct result is actually -0. Bail out.
    if (!ins->mir()->isTruncated()) {
      bailoutIf(Assembler::Zero, ins->snapshot());
    }
    masm.bind(&done);
  }
}

class ModOverflowCheck : public OutOfLineCodeBase<CodeGeneratorX86Shared> {
  Label done_;
  LModI* ins_;
  Register rhs_;

 public:
  explicit ModOverflowCheck(LModI* ins, Register rhs) : ins_(ins), rhs_(rhs) {}

  virtual void accept(CodeGeneratorX86Shared* codegen) override {
    codegen->visitModOverflowCheck(this);
  }
  Label* done() { return &done_; }
  LModI* ins() const { return ins_; }
  Register rhs() const { return rhs_; }
};

void CodeGeneratorX86Shared::visitModOverflowCheck(ModOverflowCheck* ool) {
  masm.cmp32(ool->rhs(), Imm32(-1));
  if (ool->ins()->mir()->isTruncated()) {
    masm.j(Assembler::NotEqual, ool->rejoin());
    masm.mov(ImmWord(0), edx);
    masm.jmp(ool->done());
  } else {
    bailoutIf(Assembler::Equal, ool->ins()->snapshot());
    masm.jmp(ool->rejoin());
  }
}

void CodeGenerator::visitModI(LModI* ins) {
  Register remainder = ToRegister(ins->remainder());
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());

  // Required to use idiv.
  MOZ_ASSERT_IF(lhs != rhs, rhs != eax);
  MOZ_ASSERT(rhs != edx);
  MOZ_ASSERT(remainder == edx);
  MOZ_ASSERT(ToRegister(ins->getTemp(0)) == eax);

  Label done;
  ReturnZero* ool = nullptr;
  ModOverflowCheck* overflow = nullptr;

  // Set up eax in preparation for doing a div.
  if (lhs != eax) {
    masm.mov(lhs, eax);
  }

  MMod* mir = ins->mir();

  // Prevent divide by zero.
  if (mir->canBeDivideByZero()) {
    masm.test32(rhs, rhs);
    if (mir->isTruncated()) {
      if (mir->trapOnError()) {
        Label nonZero;
        masm.j(Assembler::NonZero, &nonZero);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
        masm.bind(&nonZero);
      } else {
        if (!ool) {
          ool = new (alloc()) ReturnZero(edx);
        }
        masm.j(Assembler::Zero, ool->entry());
      }
    } else {
      bailoutIf(Assembler::Zero, ins->snapshot());
    }
  }

  Label negative;

  // Switch based on sign of the lhs.
  if (mir->canBeNegativeDividend()) {
    masm.branchTest32(Assembler::Signed, lhs, lhs, &negative);
  }

  // If lhs >= 0 then remainder = lhs % rhs. The remainder must be positive.
  {
    // Check if rhs is a power-of-two.
    if (mir->canBePowerOfTwoDivisor()) {
      MOZ_ASSERT(rhs != remainder);

      // Rhs y is a power-of-two if (y & (y-1)) == 0. Note that if
      // y is any negative number other than INT32_MIN, both y and
      // y-1 will have the sign bit set so these are never optimized
      // as powers-of-two. If y is INT32_MIN, y-1 will be INT32_MAX
      // and because lhs >= 0 at this point, lhs & INT32_MAX returns
      // the correct value.
      Label notPowerOfTwo;
      masm.mov(rhs, remainder);
      masm.subl(Imm32(1), remainder);
      masm.branchTest32(Assembler::NonZero, remainder, rhs, &notPowerOfTwo);
      {
        masm.andl(lhs, remainder);
        masm.jmp(&done);
      }
      masm.bind(&notPowerOfTwo);
    }

    // Since lhs >= 0, the sign-extension will be 0
    masm.mov(ImmWord(0), edx);
    masm.idiv(rhs);
  }

  // Otherwise, we have to beware of two special cases:
  if (mir->canBeNegativeDividend()) {
    masm.jump(&done);

    masm.bind(&negative);

    // Prevent an integer overflow exception from -2147483648 % -1
    Label notmin;
    masm.cmp32(lhs, Imm32(INT32_MIN));
    overflow = new (alloc()) ModOverflowCheck(ins, rhs);
    masm.j(Assembler::Equal, overflow->entry());
    masm.bind(overflow->rejoin());
    masm.cdq();
    masm.idiv(rhs);

    if (!mir->isTruncated()) {
      // A remainder of 0 means that the rval must be -0, which is a double.
      masm.test32(remainder, remainder);
      bailoutIf(Assembler::Zero, ins->snapshot());
    }
  }

  masm.bind(&done);

  if (overflow) {
    addOutOfLineCode(overflow, mir);
    masm.bind(overflow->done());
  }

  if (ool) {
    addOutOfLineCode(ool, mir);
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitBitNotI(LBitNotI* ins) {
  const LAllocation* input = ins->getOperand(0);
  MOZ_ASSERT(!input->isConstant());

  masm.notl(ToOperand(input));
}

void CodeGenerator::visitBitOpI(LBitOpI* ins) {
  const LAllocation* lhs = ins->getOperand(0);
  const LAllocation* rhs = ins->getOperand(1);

  switch (ins->bitop()) {
    case JSOp::BitOr:
      if (rhs->isConstant()) {
        masm.orl(Imm32(ToInt32(rhs)), ToOperand(lhs));
      } else {
        masm.orl(ToOperand(rhs), ToRegister(lhs));
      }
      break;
    case JSOp::BitXor:
      if (rhs->isConstant()) {
        masm.xorl(Imm32(ToInt32(rhs)), ToOperand(lhs));
      } else {
        masm.xorl(ToOperand(rhs), ToRegister(lhs));
      }
      break;
    case JSOp::BitAnd:
      if (rhs->isConstant()) {
        masm.andl(Imm32(ToInt32(rhs)), ToOperand(lhs));
      } else {
        masm.andl(ToOperand(rhs), ToRegister(lhs));
      }
      break;
    default:
      MOZ_CRASH("unexpected binary opcode");
  }
}

void CodeGenerator::visitBitOpI64(LBitOpI64* lir) {
  const LInt64Allocation lhs = lir->getInt64Operand(LBitOpI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LBitOpI64::Rhs);

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

  if (rhs->isConstant()) {
    int32_t shift = ToInt32(rhs) & 0x1F;
    switch (ins->bitop()) {
      case JSOp::Lsh:
        if (shift) {
          masm.lshift32(Imm32(shift), lhs);
        }
        break;
      case JSOp::Rsh:
        if (shift) {
          masm.rshift32Arithmetic(Imm32(shift), lhs);
        }
        break;
      case JSOp::Ursh:
        if (shift) {
          masm.rshift32(Imm32(shift), lhs);
        } else if (ins->mir()->toUrsh()->fallible()) {
          // x >>> 0 can overflow.
          masm.test32(lhs, lhs);
          bailoutIf(Assembler::Signed, ins->snapshot());
        }
        break;
      default:
        MOZ_CRASH("Unexpected shift op");
    }
  } else {
    Register shift = ToRegister(rhs);
    switch (ins->bitop()) {
      case JSOp::Lsh:
        masm.lshift32(shift, lhs);
        break;
      case JSOp::Rsh:
        masm.rshift32Arithmetic(shift, lhs);
        break;
      case JSOp::Ursh:
        masm.rshift32(shift, lhs);
        if (ins->mir()->toUrsh()->fallible()) {
          // x >>> 0 can overflow.
          masm.test32(lhs, lhs);
          bailoutIf(Assembler::Signed, ins->snapshot());
        }
        break;
      default:
        MOZ_CRASH("Unexpected shift op");
    }
  }
}

void CodeGenerator::visitShiftI64(LShiftI64* lir) {
  const LInt64Allocation lhs = lir->getInt64Operand(LShiftI64::Lhs);
  LAllocation* rhs = lir->getOperand(LShiftI64::Rhs);

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

  Register shift = ToRegister(rhs);
#ifdef JS_CODEGEN_X86
  MOZ_ASSERT(shift == ecx);
#endif
  switch (lir->bitop()) {
    case JSOp::Lsh:
      masm.lshift64(shift, ToRegister64(lhs));
      break;
    case JSOp::Rsh:
      masm.rshift64Arithmetic(shift, ToRegister64(lhs));
      break;
    case JSOp::Ursh:
      masm.rshift64(shift, ToRegister64(lhs));
      break;
    default:
      MOZ_CRASH("Unexpected shift op");
  }
}

void CodeGenerator::visitUrshD(LUrshD* ins) {
  Register lhs = ToRegister(ins->lhs());
  MOZ_ASSERT(ToRegister(ins->temp()) == lhs);

  const LAllocation* rhs = ins->rhs();
  FloatRegister out = ToFloatRegister(ins->output());

  if (rhs->isConstant()) {
    int32_t shift = ToInt32(rhs) & 0x1F;
    if (shift) {
      masm.shrl(Imm32(shift), lhs);
    }
  } else {
    Register shift = ToRegister(rhs);
    masm.rshift32(shift, lhs);
  }

  masm.convertUInt32ToDouble(lhs, out);
}

Operand CodeGeneratorX86Shared::ToOperand(const LAllocation& a) {
  if (a.isGeneralReg()) {
    return Operand(a.toGeneralReg()->reg());
  }
  if (a.isFloatReg()) {
    return Operand(a.toFloatReg()->reg());
  }
  return Operand(ToAddress(a));
}

Operand CodeGeneratorX86Shared::ToOperand(const LAllocation* a) {
  return ToOperand(*a);
}

Operand CodeGeneratorX86Shared::ToOperand(const LDefinition* def) {
  return ToOperand(def->output());
}

MoveOperand CodeGeneratorX86Shared::toMoveOperand(LAllocation a) const {
  if (a.isGeneralReg()) {
    return MoveOperand(ToRegister(a));
  }
  if (a.isFloatReg()) {
    return MoveOperand(ToFloatRegister(a));
  }
  MoveOperand::Kind kind =
      a.isStackArea() ? MoveOperand::EFFECTIVE_ADDRESS : MoveOperand::MEMORY;
  return MoveOperand(ToAddress(a), kind);
}

class OutOfLineTableSwitch : public OutOfLineCodeBase<CodeGeneratorX86Shared> {
  MTableSwitch* mir_;
  CodeLabel jumpLabel_;

  void accept(CodeGeneratorX86Shared* codegen) override {
    codegen->visitOutOfLineTableSwitch(this);
  }

 public:
  explicit OutOfLineTableSwitch(MTableSwitch* mir) : mir_(mir) {}

  MTableSwitch* mir() const { return mir_; }

  CodeLabel* jumpLabel() { return &jumpLabel_; }
};

void CodeGeneratorX86Shared::visitOutOfLineTableSwitch(
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

void CodeGeneratorX86Shared::emitTableSwitchDispatch(MTableSwitch* mir,
                                                     Register index,
                                                     Register base) {
  Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

  // Lower value with low value
  if (mir->low() != 0) {
    masm.subl(Imm32(mir->low()), index);
  }

  // Jump to default case if input is out of range
  int32_t cases = mir->numCases();
  masm.cmp32(index, Imm32(cases));
  masm.j(AssemblerX86Shared::AboveOrEqual, defaultcase);

  // To fill in the CodeLabels for the case entries, we need to first
  // generate the case entries (we don't yet know their offsets in the
  // instruction stream).
  OutOfLineTableSwitch* ool = new (alloc()) OutOfLineTableSwitch(mir);
  addOutOfLineCode(ool, mir);

  // Compute the position where a pointer to the right case stands.
  masm.mov(ool->jumpLabel(), base);
  BaseIndex pointer(base, index, ScalePointer);

  // Jump to the right case
  masm.branchToComputedAddress(pointer);
}

void CodeGenerator::visitMathD(LMathD* math) {
  FloatRegister lhs = ToFloatRegister(math->lhs());
  Operand rhs = ToOperand(math->rhs());
  FloatRegister output = ToFloatRegister(math->output());

  switch (math->jsop()) {
    case JSOp::Add:
      masm.vaddsd(rhs, lhs, output);
      break;
    case JSOp::Sub:
      masm.vsubsd(rhs, lhs, output);
      break;
    case JSOp::Mul:
      masm.vmulsd(rhs, lhs, output);
      break;
    case JSOp::Div:
      masm.vdivsd(rhs, lhs, output);
      break;
    default:
      MOZ_CRASH("unexpected opcode");
  }
}

void CodeGenerator::visitMathF(LMathF* math) {
  FloatRegister lhs = ToFloatRegister(math->lhs());
  Operand rhs = ToOperand(math->rhs());
  FloatRegister output = ToFloatRegister(math->output());

  switch (math->jsop()) {
    case JSOp::Add:
      masm.vaddss(rhs, lhs, output);
      break;
    case JSOp::Sub:
      masm.vsubss(rhs, lhs, output);
      break;
    case JSOp::Mul:
      masm.vmulss(rhs, lhs, output);
      break;
    case JSOp::Div:
      masm.vdivss(rhs, lhs, output);
      break;
    default:
      MOZ_CRASH("unexpected opcode");
  }
}

void CodeGenerator::visitNearbyInt(LNearbyInt* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  FloatRegister output = ToFloatRegister(lir->output());

  RoundingMode roundingMode = lir->mir()->roundingMode();
  masm.nearbyIntDouble(roundingMode, input, output);
}

void CodeGenerator::visitNearbyIntF(LNearbyIntF* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  FloatRegister output = ToFloatRegister(lir->output());

  RoundingMode roundingMode = lir->mir()->roundingMode();
  masm.nearbyIntFloat32(roundingMode, input, output);
}

void CodeGenerator::visitEffectiveAddress(LEffectiveAddress* ins) {
  const MEffectiveAddress* mir = ins->mir();
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  Register output = ToRegister(ins->output());
  masm.leal(Operand(base, index, mir->scale(), mir->displacement()), output);
}

void CodeGeneratorX86Shared::generateInvalidateEpilogue() {
  // Ensure that there is enough space in the buffer for the OsiPoint
  // patching to occur. Otherwise, we could overwrite the invalidation
  // epilogue.
  for (size_t i = 0; i < sizeof(void*); i += Assembler::NopSize()) {
    masm.nop();
  }

  masm.bind(&invalidate_);

  // Push the Ion script onto the stack (when we determine what that pointer
  // is).
  invalidateEpilogueData_ = masm.pushWithPatch(ImmWord(uintptr_t(-1)));

  // Jump to the invalidator which will replace the current frame.
  TrampolinePtr thunk = gen->jitRuntime()->getInvalidationThunk();
  masm.jump(thunk);
}

void CodeGenerator::visitNegI(LNegI* ins) {
  Register input = ToRegister(ins->input());
  MOZ_ASSERT(input == ToRegister(ins->output()));

  masm.neg32(input);
}

void CodeGenerator::visitNegI64(LNegI64* ins) {
  Register64 input = ToRegister64(ins->getInt64Operand(0));
  MOZ_ASSERT(input == ToOutRegister64(ins));
  masm.neg64(input);
}

void CodeGenerator::visitNegD(LNegD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  MOZ_ASSERT(input == ToFloatRegister(ins->output()));

  masm.negateDouble(input);
}

void CodeGenerator::visitNegF(LNegF* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  MOZ_ASSERT(input == ToFloatRegister(ins->output()));

  masm.negateFloat(input);
}

void CodeGenerator::visitCompareExchangeTypedArrayElement(
    LCompareExchangeTypedArrayElement* lir) {
  Register elements = ToRegister(lir->elements());
  AnyRegister output = ToAnyRegister(lir->output());
  Register temp =
      lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

  Register oldval = ToRegister(lir->oldval());
  Register newval = ToRegister(lir->newval());

  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.compareExchangeJS(arrayType, Synchronization::Full(), dest, oldval,
                           newval, temp, output);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.compareExchangeJS(arrayType, Synchronization::Full(), dest, oldval,
                           newval, temp, output);
  }
}

void CodeGenerator::visitAtomicExchangeTypedArrayElement(
    LAtomicExchangeTypedArrayElement* lir) {
  Register elements = ToRegister(lir->elements());
  AnyRegister output = ToAnyRegister(lir->output());
  Register temp =
      lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

  Register value = ToRegister(lir->value());

  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicExchangeJS(arrayType, Synchronization::Full(), dest, value, temp,
                          output);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicExchangeJS(arrayType, Synchronization::Full(), dest, value, temp,
                          output);
  }
}

template <typename T>
static inline void AtomicBinopToTypedArray(MacroAssembler& masm, AtomicOp op,
                                           Scalar::Type arrayType,
                                           const LAllocation* value,
                                           const T& mem, Register temp1,
                                           Register temp2, AnyRegister output) {
  if (value->isConstant()) {
    masm.atomicFetchOpJS(arrayType, Synchronization::Full(), op,
                         Imm32(ToInt32(value)), mem, temp1, temp2, output);
  } else {
    masm.atomicFetchOpJS(arrayType, Synchronization::Full(), op,
                         ToRegister(value), mem, temp1, temp2, output);
  }
}

void CodeGenerator::visitAtomicTypedArrayElementBinop(
    LAtomicTypedArrayElementBinop* lir) {
  MOZ_ASSERT(!lir->mir()->isForEffect());

  AnyRegister output = ToAnyRegister(lir->output());
  Register elements = ToRegister(lir->elements());
  Register temp1 =
      lir->temp1()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp1());
  Register temp2 =
      lir->temp2()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp2());
  const LAllocation* value = lir->value();

  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address mem = ToAddress(elements, lir->index(), arrayType);
    AtomicBinopToTypedArray(masm, lir->mir()->operation(), arrayType, value,
                            mem, temp1, temp2, output);
  } else {
    BaseIndex mem(elements, ToRegister(lir->index()),
                  ScaleFromScalarType(arrayType));
    AtomicBinopToTypedArray(masm, lir->mir()->operation(), arrayType, value,
                            mem, temp1, temp2, output);
  }
}

template <typename T>
static inline void AtomicBinopToTypedArray(MacroAssembler& masm,
                                           Scalar::Type arrayType, AtomicOp op,
                                           const LAllocation* value,
                                           const T& mem) {
  if (value->isConstant()) {
    masm.atomicEffectOpJS(arrayType, Synchronization::Full(), op,
                          Imm32(ToInt32(value)), mem, InvalidReg);
  } else {
    masm.atomicEffectOpJS(arrayType, Synchronization::Full(), op,
                          ToRegister(value), mem, InvalidReg);
  }
}

void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect(
    LAtomicTypedArrayElementBinopForEffect* lir) {
  MOZ_ASSERT(lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  const LAllocation* value = lir->value();
  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address mem = ToAddress(elements, lir->index(), arrayType);
    AtomicBinopToTypedArray(masm, arrayType, lir->mir()->operation(), value,
                            mem);
  } else {
    BaseIndex mem(elements, ToRegister(lir->index()),
                  ScaleFromScalarType(arrayType));
    AtomicBinopToTypedArray(masm, arrayType, lir->mir()->operation(), value,
                            mem);
  }
}

void CodeGenerator::visitMemoryBarrier(LMemoryBarrier* ins) {
  if (ins->type() & MembarStoreLoad) {
    masm.storeLoadFence();
  }
}

void CodeGeneratorX86Shared::visitOutOfLineWasmTruncateCheck(
    OutOfLineWasmTruncateCheck* ool) {
  FloatRegister input = ool->input();
  Register output = ool->output();
  Register64 output64 = ool->output64();
  MIRType fromType = ool->fromType();
  MIRType toType = ool->toType();
  Label* oolRejoin = ool->rejoin();
  TruncFlags flags = ool->flags();
  wasm::BytecodeOffset off = ool->bytecodeOffset();

  if (fromType == MIRType::Float32) {
    if (toType == MIRType::Int32) {
      masm.oolWasmTruncateCheckF32ToI32(input, output, flags, off, oolRejoin);
    } else if (toType == MIRType::Int64) {
      masm.oolWasmTruncateCheckF32ToI64(input, output64, flags, off, oolRejoin);
    } else {
      MOZ_CRASH("unexpected type");
    }
  } else if (fromType == MIRType::Double) {
    if (toType == MIRType::Int32) {
      masm.oolWasmTruncateCheckF64ToI32(input, output, flags, off, oolRejoin);
    } else if (toType == MIRType::Int64) {
      masm.oolWasmTruncateCheckF64ToI64(input, output64, flags, off, oolRejoin);
    } else {
      MOZ_CRASH("unexpected type");
    }
  } else {
    MOZ_CRASH("unexpected type");
  }
}

void CodeGeneratorX86Shared::canonicalizeIfDeterministic(
    Scalar::Type type, const LAllocation* value) {
#ifdef DEBUG
  if (!js::SupportDifferentialTesting()) {
    return;
  }

  switch (type) {
    case Scalar::Float32: {
      FloatRegister in = ToFloatRegister(value);
      masm.canonicalizeFloatIfDeterministic(in);
      break;
    }
    case Scalar::Float64: {
      FloatRegister in = ToFloatRegister(value);
      masm.canonicalizeDoubleIfDeterministic(in);
      break;
    }
    default: {
      // Other types don't need canonicalization.
      break;
    }
  }
#endif  // DEBUG
}

template <typename T>
Operand CodeGeneratorX86Shared::toMemoryAccessOperand(T* lir, int32_t disp) {
  const LAllocation* ptr = lir->ptr();
#ifdef JS_CODEGEN_X86
  const LAllocation* memoryBase = lir->memoryBase();
  Operand destAddr = ptr->isBogus() ? Operand(ToRegister(memoryBase), disp)
                                    : Operand(ToRegister(memoryBase),
                                              ToRegister(ptr), TimesOne, disp);
#else
  Operand destAddr = ptr->isBogus()
                         ? Operand(HeapReg, disp)
                         : Operand(HeapReg, ToRegister(ptr), TimesOne, disp);
#endif
  return destAddr;
}

void CodeGenerator::visitCopySignF(LCopySignF* lir) {
  FloatRegister lhs = ToFloatRegister(lir->getOperand(0));
  FloatRegister rhs = ToFloatRegister(lir->getOperand(1));

  FloatRegister out = ToFloatRegister(lir->output());

  if (lhs == rhs) {
    if (lhs != out) {
      masm.moveFloat32(lhs, out);
    }
    return;
  }

  masm.copySignFloat32(lhs, rhs, out);
}

void CodeGenerator::visitCopySignD(LCopySignD* lir) {
  FloatRegister lhs = ToFloatRegister(lir->getOperand(0));
  FloatRegister rhs = ToFloatRegister(lir->getOperand(1));

  FloatRegister out = ToFloatRegister(lir->output());

  if (lhs == rhs) {
    if (lhs != out) {
      masm.moveDouble(lhs, out);
    }
    return;
  }

  masm.copySignDouble(lhs, rhs, out);
}

void CodeGenerator::visitRotateI64(LRotateI64* lir) {
  MRotate* mir = lir->mir();
  LAllocation* count = lir->count();

  Register64 input = ToRegister64(lir->input());
  Register64 output = ToOutRegister64(lir);
  Register temp = ToTempRegisterOrInvalid(lir->temp());

  MOZ_ASSERT(input == output);

  if (count->isConstant()) {
    int32_t c = int32_t(count->toConstant()->toInt64() & 0x3F);
    if (!c) {
      return;
    }
    if (mir->isLeftRotate()) {
      masm.rotateLeft64(Imm32(c), input, output, temp);
    } else {
      masm.rotateRight64(Imm32(c), input, output, temp);
    }
  } else {
    if (mir->isLeftRotate()) {
      masm.rotateLeft64(ToRegister(count), input, output, temp);
    } else {
      masm.rotateRight64(ToRegister(count), input, output, temp);
    }
  }
}

void CodeGenerator::visitPopcntI64(LPopcntI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);
  Register temp = InvalidReg;
  if (!AssemblerX86Shared::HasPOPCNT()) {
    temp = ToRegister(lir->getTemp(0));
  }

  masm.popcnt64(input, output, temp);
}

void CodeGenerator::visitSimd128(LSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  const LDefinition* out = ins->getDef(0);
  masm.loadConstantSimd128(ins->getSimd128(), ToFloatRegister(out));
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmBitselectSimd128(LWasmBitselectSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister lhsDest = ToFloatRegister(ins->lhsDest());
  FloatRegister rhs = ToFloatRegister(ins->rhs());
  FloatRegister control = ToFloatRegister(ins->control());
  FloatRegister temp = ToFloatRegister(ins->temp());
  masm.bitwiseSelectSimd128(control, lhsDest, rhs, lhsDest, temp);
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmBinarySimd128(LWasmBinarySimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister lhsDest = ToFloatRegister(ins->lhsDest());
  FloatRegister rhs = ToFloatRegister(ins->rhs());
  FloatRegister temp1 = ToTempFloatRegisterOrInvalid(ins->getTemp(0));
  FloatRegister temp2 = ToTempFloatRegisterOrInvalid(ins->getTemp(1));

  MOZ_ASSERT(ToFloatRegister(ins->output()) == lhsDest);

  switch (ins->simdOp()) {
    case wasm::SimdOp::V128And:
      masm.bitwiseAndSimd128(rhs, lhsDest);
      break;
    case wasm::SimdOp::V128Or:
      masm.bitwiseOrSimd128(rhs, lhsDest);
      break;
    case wasm::SimdOp::V128Xor:
      masm.bitwiseXorSimd128(rhs, lhsDest);
      break;
    case wasm::SimdOp::V128AndNot:
      // x86/x64 specific: The CPU provides ~A & B.  The operands were swapped
      // during lowering, and we'll compute A & ~B here as desired.
      masm.bitwiseNotAndSimd128(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16AvgrU:
      masm.unsignedAverageInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8AvgrU:
      masm.unsignedAverageInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16Add:
      masm.addInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16AddSaturateS:
      masm.addSatInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16AddSaturateU:
      masm.unsignedAddSatInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16Sub:
      masm.subInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16SubSaturateS:
      masm.subSatInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16SubSaturateU:
      masm.unsignedSubSatInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16MinS:
      masm.minInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16MinU:
      masm.unsignedMinInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16MaxS:
      masm.maxInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16MaxU:
      masm.unsignedMaxInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Add:
      masm.addInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8AddSaturateS:
      masm.addSatInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8AddSaturateU:
      masm.unsignedAddSatInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Sub:
      masm.subInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8SubSaturateS:
      masm.subSatInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8SubSaturateU:
      masm.unsignedSubSatInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Mul:
      masm.mulInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8MinS:
      masm.minInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8MinU:
      masm.unsignedMinInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8MaxS:
      masm.maxInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8MaxU:
      masm.unsignedMaxInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Add:
      masm.addInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Sub:
      masm.subInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Mul:
      masm.mulInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4MinS:
      masm.minInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4MinU:
      masm.unsignedMinInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4MaxS:
      masm.maxInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4MaxU:
      masm.unsignedMaxInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2Add:
      masm.addInt64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2Sub:
      masm.subInt64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2Mul:
      masm.mulInt64x2(lhsDest, rhs, lhsDest, temp1);
      break;
    case wasm::SimdOp::F32x4Add:
      masm.addFloat32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Sub:
      masm.subFloat32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Mul:
      masm.mulFloat32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Div:
      masm.divFloat32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Min:
      masm.minFloat32x4(rhs, lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::F32x4Max:
      masm.maxFloat32x4(rhs, lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::F64x2Add:
      masm.addFloat64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Sub:
      masm.subFloat64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Mul:
      masm.mulFloat64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Div:
      masm.divFloat64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Min:
      masm.minFloat64x2(rhs, lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::F64x2Max:
      masm.maxFloat64x2(rhs, lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::V8x16Swizzle:
      masm.swizzleInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16NarrowSI16x8:
      masm.narrowInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16NarrowUI16x8:
      masm.unsignedNarrowInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8NarrowSI32x4:
      masm.narrowInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8NarrowUI32x4:
      masm.unsignedNarrowInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16Eq:
      masm.compareInt8x16(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16Ne:
      masm.compareInt8x16(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16LtS:
      masm.compareInt8x16(Assembler::LessThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16GtS:
      masm.compareInt8x16(Assembler::GreaterThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16LeS:
      masm.compareInt8x16(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16GeS:
      masm.compareInt8x16(Assembler::GreaterThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16LtU:
      masm.compareInt8x16(Assembler::Below, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16GtU:
      masm.compareInt8x16(Assembler::Above, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16LeU:
      masm.compareInt8x16(Assembler::BelowOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16GeU:
      masm.compareInt8x16(Assembler::AboveOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Eq:
      masm.compareInt16x8(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Ne:
      masm.compareInt16x8(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8LtS:
      masm.compareInt16x8(Assembler::LessThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8GtS:
      masm.compareInt16x8(Assembler::GreaterThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8LeS:
      masm.compareInt16x8(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8GeS:
      masm.compareInt16x8(Assembler::GreaterThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8LtU:
      masm.compareInt16x8(Assembler::Below, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8GtU:
      masm.compareInt16x8(Assembler::Above, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8LeU:
      masm.compareInt16x8(Assembler::BelowOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8GeU:
      masm.compareInt16x8(Assembler::AboveOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Eq:
      masm.compareInt32x4(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Ne:
      masm.compareInt32x4(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4LtS:
      masm.compareInt32x4(Assembler::LessThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4GtS:
      masm.compareInt32x4(Assembler::GreaterThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4LeS:
      masm.compareInt32x4(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4GeS:
      masm.compareInt32x4(Assembler::GreaterThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4LtU:
      masm.compareInt32x4(Assembler::Below, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4GtU:
      masm.compareInt32x4(Assembler::Above, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4LeU:
      masm.compareInt32x4(Assembler::BelowOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4GeU:
      masm.compareInt32x4(Assembler::AboveOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2Eq:
      masm.compareForEqualityInt64x2(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2Ne:
      masm.compareForEqualityInt64x2(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2LtS:
      masm.compareForOrderingInt64x2(Assembler::LessThan, rhs, lhsDest, temp1,
                                     temp2);
      break;
    case wasm::SimdOp::I64x2GtS:
      masm.compareForOrderingInt64x2(Assembler::GreaterThan, rhs, lhsDest,
                                     temp1, temp2);
      break;
    case wasm::SimdOp::I64x2LeS:
      masm.compareForOrderingInt64x2(Assembler::LessThanOrEqual, rhs, lhsDest,
                                     temp1, temp2);
      break;
    case wasm::SimdOp::I64x2GeS:
      masm.compareForOrderingInt64x2(Assembler::GreaterThanOrEqual, rhs,
                                     lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::F32x4Eq:
      masm.compareFloat32x4(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Ne:
      masm.compareFloat32x4(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Lt:
      masm.compareFloat32x4(Assembler::LessThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Le:
      masm.compareFloat32x4(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Eq:
      masm.compareFloat64x2(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Ne:
      masm.compareFloat64x2(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Lt:
      masm.compareFloat64x2(Assembler::LessThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Le:
      masm.compareFloat64x2(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4PMax:
      // `lhsDest` is actually rhsDest, and `rhs` is actually lhs
      masm.pseudoMaxFloat32x4(lhsDest, rhs);
      break;
    case wasm::SimdOp::F32x4PMin:
      // `lhsDest` is actually rhsDest, and `rhs` is actually lhs
      masm.pseudoMinFloat32x4(lhsDest, rhs);
      break;
    case wasm::SimdOp::F64x2PMax:
      // `lhsDest` is actually rhsDest, and `rhs` is actually lhs
      masm.pseudoMaxFloat64x2(lhsDest, rhs);
      break;
    case wasm::SimdOp::F64x2PMin:
      // `lhsDest` is actually rhsDest, and `rhs` is actually lhs
      masm.pseudoMinFloat64x2(lhsDest, rhs);
      break;
    case wasm::SimdOp::I32x4DotSI16x8:
      masm.widenDotInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8ExtMulLowSI8x16:
      masm.extMulLowInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8ExtMulHighSI8x16:
      masm.extMulHighInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8ExtMulLowUI8x16:
      masm.unsignedExtMulLowInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8ExtMulHighUI8x16:
      masm.unsignedExtMulHighInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4ExtMulLowSI16x8:
      masm.extMulLowInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4ExtMulHighSI16x8:
      masm.extMulHighInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4ExtMulLowUI16x8:
      masm.unsignedExtMulLowInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4ExtMulHighUI16x8:
      masm.unsignedExtMulHighInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2ExtMulLowSI32x4:
      masm.extMulLowInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2ExtMulHighSI32x4:
      masm.extMulHighInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2ExtMulLowUI32x4:
      masm.unsignedExtMulLowInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2ExtMulHighUI32x4:
      masm.unsignedExtMulHighInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Q15MulrSatS:
      masm.q15MulrSatInt16x8(rhs, lhsDest);
      break;
#  ifdef ENABLE_WASM_SIMD_WORMHOLE
    case wasm::SimdOp::MozWHSELFTEST:
      masm.loadConstantSimd128(wasm::WormholeSignature(), lhsDest);
      break;
    case wasm::SimdOp::MozWHPMADDUBSW:
      masm.vpmaddubsw(rhs, lhsDest, lhsDest);
      break;
    case wasm::SimdOp::MozWHPMADDWD:
      masm.vpmaddwd(Operand(rhs), lhsDest, lhsDest);
      break;
#  endif
    default:
      MOZ_CRASH("Binary SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmBinarySimd128WithConstant(
    LWasmBinarySimd128WithConstant* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister lhsDest = ToFloatRegister(ins->lhsDest());
  const SimdConstant& rhs = ins->rhs();

  MOZ_ASSERT(ToFloatRegister(ins->output()) == lhsDest);

  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16Add:
      masm.addInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Add:
      masm.addInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Add:
      masm.addInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2Add:
      masm.addInt64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16Sub:
      masm.subInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Sub:
      masm.subInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Sub:
      masm.subInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I64x2Sub:
      masm.subInt64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Mul:
      masm.mulInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Mul:
      masm.mulInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16AddSaturateS:
      masm.addSatInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16AddSaturateU:
      masm.unsignedAddSatInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8AddSaturateS:
      masm.addSatInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8AddSaturateU:
      masm.unsignedAddSatInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16SubSaturateS:
      masm.subSatInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16SubSaturateU:
      masm.unsignedSubSatInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8SubSaturateS:
      masm.subSatInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8SubSaturateU:
      masm.unsignedSubSatInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16MinS:
      masm.minInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16MinU:
      masm.unsignedMinInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8MinS:
      masm.minInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8MinU:
      masm.unsignedMinInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4MinS:
      masm.minInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4MinU:
      masm.unsignedMinInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16MaxS:
      masm.maxInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16MaxU:
      masm.unsignedMaxInt8x16(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8MaxS:
      masm.maxInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8MaxU:
      masm.unsignedMaxInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4MaxS:
      masm.maxInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4MaxU:
      masm.unsignedMaxInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::V128And:
      masm.bitwiseAndSimd128(rhs, lhsDest);
      break;
    case wasm::SimdOp::V128Or:
      masm.bitwiseOrSimd128(rhs, lhsDest);
      break;
    case wasm::SimdOp::V128Xor:
      masm.bitwiseXorSimd128(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16Eq:
      masm.compareInt8x16(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16Ne:
      masm.compareInt8x16(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16GtS:
      masm.compareInt8x16(Assembler::GreaterThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16LeS:
      masm.compareInt8x16(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Eq:
      masm.compareInt16x8(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8Ne:
      masm.compareInt16x8(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8GtS:
      masm.compareInt16x8(Assembler::GreaterThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8LeS:
      masm.compareInt16x8(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Eq:
      masm.compareInt32x4(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4Ne:
      masm.compareInt32x4(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4GtS:
      masm.compareInt32x4(Assembler::GreaterThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4LeS:
      masm.compareInt32x4(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Eq:
      masm.compareFloat32x4(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Ne:
      masm.compareFloat32x4(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Lt:
      masm.compareFloat32x4(Assembler::LessThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Le:
      masm.compareFloat32x4(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Eq:
      masm.compareFloat64x2(Assembler::Equal, rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Ne:
      masm.compareFloat64x2(Assembler::NotEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Lt:
      masm.compareFloat64x2(Assembler::LessThan, rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Le:
      masm.compareFloat64x2(Assembler::LessThanOrEqual, rhs, lhsDest);
      break;
    case wasm::SimdOp::I32x4DotSI16x8:
      masm.widenDotInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Add:
      masm.addFloat32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Add:
      masm.addFloat64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Sub:
      masm.subFloat32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Sub:
      masm.subFloat64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Div:
      masm.divFloat32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Div:
      masm.divFloat64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::F32x4Mul:
      masm.mulFloat32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::F64x2Mul:
      masm.mulFloat64x2(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16NarrowSI16x8:
      masm.narrowInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I8x16NarrowUI16x8:
      masm.unsignedNarrowInt16x8(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8NarrowSI32x4:
      masm.narrowInt32x4(rhs, lhsDest);
      break;
    case wasm::SimdOp::I16x8NarrowUI32x4:
      masm.unsignedNarrowInt32x4(rhs, lhsDest);
      break;
    default:
      MOZ_CRASH("Binary SimdOp with constant not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmVariableShiftSimd128(
    LWasmVariableShiftSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister lhsDest = ToFloatRegister(ins->lhsDest());
  Register rhs = ToRegister(ins->rhs());
  Register temp1 = ToTempRegisterOrInvalid(ins->getTemp(0));
  FloatRegister temp2 = ToTempFloatRegisterOrInvalid(ins->getTemp(1));

  MOZ_ASSERT(ToFloatRegister(ins->output()) == lhsDest);

  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16Shl:
      masm.leftShiftInt8x16(rhs, lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::I8x16ShrS:
      masm.rightShiftInt8x16(rhs, lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::I8x16ShrU:
      masm.unsignedRightShiftInt8x16(rhs, lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::I16x8Shl:
      masm.leftShiftInt16x8(rhs, lhsDest, temp1);
      break;
    case wasm::SimdOp::I16x8ShrS:
      masm.rightShiftInt16x8(rhs, lhsDest, temp1);
      break;
    case wasm::SimdOp::I16x8ShrU:
      masm.unsignedRightShiftInt16x8(rhs, lhsDest, temp1);
      break;
    case wasm::SimdOp::I32x4Shl:
      masm.leftShiftInt32x4(rhs, lhsDest, temp1);
      break;
    case wasm::SimdOp::I32x4ShrS:
      masm.rightShiftInt32x4(rhs, lhsDest, temp1);
      break;
    case wasm::SimdOp::I32x4ShrU:
      masm.unsignedRightShiftInt32x4(rhs, lhsDest, temp1);
      break;
    case wasm::SimdOp::I64x2Shl:
      masm.leftShiftInt64x2(rhs, lhsDest, temp1);
      break;
    case wasm::SimdOp::I64x2ShrS:
      masm.rightShiftInt64x2(rhs, lhsDest, temp1, temp2);
      break;
    case wasm::SimdOp::I64x2ShrU:
      masm.unsignedRightShiftInt64x2(rhs, lhsDest, temp1);
      break;
    default:
      MOZ_CRASH("Shift SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmConstantShiftSimd128(
    LWasmConstantShiftSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister src = ToFloatRegister(ins->src());
  FloatRegister dest = ToFloatRegister(ins->output());
  int32_t shift = ins->shift();

  if (shift == 0) {
    if (src != dest) {
      masm.moveSimd128(src, dest);
    }
    return;
  }

  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16Shl:
      masm.leftShiftInt8x16(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I8x16ShrS:
      masm.rightShiftInt8x16(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I8x16ShrU:
      masm.unsignedRightShiftInt8x16(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I16x8Shl:
      masm.leftShiftInt16x8(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I16x8ShrS:
      masm.rightShiftInt16x8(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I16x8ShrU:
      masm.unsignedRightShiftInt16x8(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I32x4Shl:
      masm.leftShiftInt32x4(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I32x4ShrS:
      masm.rightShiftInt32x4(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I32x4ShrU:
      masm.unsignedRightShiftInt32x4(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I64x2Shl:
      masm.leftShiftInt64x2(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I64x2ShrS:
      masm.rightShiftInt64x2(Imm32(shift), src, dest);
      break;
    case wasm::SimdOp::I64x2ShrU:
      masm.unsignedRightShiftInt64x2(Imm32(shift), src, dest);
      break;
    default:
      MOZ_CRASH("Shift SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmSignReplicationSimd128(
    LWasmSignReplicationSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister src = ToFloatRegister(ins->src());
  FloatRegister dest = ToFloatRegister(ins->output());

  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16ShrS:
      masm.signReplicationInt8x16(src, dest);
      break;
    case wasm::SimdOp::I16x8ShrS:
      masm.signReplicationInt16x8(src, dest);
      break;
    case wasm::SimdOp::I32x4ShrS:
      masm.signReplicationInt32x4(src, dest);
      break;
    case wasm::SimdOp::I64x2ShrS:
      masm.signReplicationInt64x2(src, dest);
      break;
    default:
      MOZ_CRASH("Shift SimdOp unsupported sign replication optimization");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmShuffleSimd128(LWasmShuffleSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister lhsDest = ToFloatRegister(ins->lhsDest());
  FloatRegister rhs = ToFloatRegister(ins->rhs());
  SimdConstant control = ins->control();
  switch (ins->op()) {
    case LWasmShuffleSimd128::BLEND_8x16: {
      masm.blendInt8x16(reinterpret_cast<const uint8_t*>(control.asInt8x16()),
                        lhsDest, rhs, lhsDest, ToFloatRegister(ins->temp()));
      break;
    }
    case LWasmShuffleSimd128::BLEND_16x8: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.blendInt16x8(reinterpret_cast<const uint16_t*>(control.asInt16x8()),
                        lhsDest, rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::CONCAT_RIGHT_SHIFT_8x16: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      int8_t count = 16 - control.asInt8x16()[0];
      MOZ_ASSERT(count > 0, "Should have been a MOVE operation");
      masm.concatAndRightShiftSimd128(rhs, lhsDest, count);
      break;
    }
    case LWasmShuffleSimd128::INTERLEAVE_HIGH_8x16: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.interleaveHighInt8x16(rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::INTERLEAVE_HIGH_16x8: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.interleaveHighInt16x8(rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::INTERLEAVE_HIGH_32x4: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.interleaveHighInt32x4(rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::INTERLEAVE_HIGH_64x2: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.interleaveHighInt64x2(rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::INTERLEAVE_LOW_8x16: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.interleaveLowInt8x16(rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::INTERLEAVE_LOW_16x8: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.interleaveLowInt16x8(rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::INTERLEAVE_LOW_32x4: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.interleaveLowInt32x4(rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::INTERLEAVE_LOW_64x2: {
      MOZ_ASSERT(ins->temp()->isBogusTemp());
      masm.interleaveLowInt64x2(rhs, lhsDest);
      break;
    }
    case LWasmShuffleSimd128::SHUFFLE_BLEND_8x16: {
      masm.shuffleInt8x16(reinterpret_cast<const uint8_t*>(control.asInt8x16()),
                          rhs, lhsDest);
      break;
    }
    default: {
      MOZ_CRASH("Unsupported SIMD shuffle operation");
    }
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

#ifdef ENABLE_WASM_SIMD

enum PermuteX64I16x8Action : uint16_t {
  UNAVAILABLE = 0,
  SWAP_QWORDS = 1,  // Swap qwords first
  PERM_LOW = 2,     // Permute low qword by control_[0..3]
  PERM_HIGH = 4     // Permute high qword by control_[4..7]
};

// Skip lanes that equal v starting at i, returning the index just beyond the
// last of those.  There is no requirement that the initial lanes[i] == v.
template <typename T>
static int ScanConstant(const T* lanes, int v, int i) {
  int len = int(16 / sizeof(T));
  MOZ_ASSERT(i <= len);
  while (i < len && lanes[i] == v) {
    i++;
  }
  return i;
}

// Apply a transformation to each lane value.
template <typename T>
static void MapLanes(T* result, const T* input, int (*f)(int)) {
  int len = int(16 / sizeof(T));
  for (int i = 0; i < len; i++) {
    result[i] = f(input[i]);
  }
}

// Recognize part of an identity permutation starting at start, with
// the first value of the permutation expected to be bias.
template <typename T>
static bool IsIdentity(const T* lanes, int start, int len, int bias) {
  if (lanes[start] != bias) {
    return false;
  }
  for (int i = start + 1; i < start + len; i++) {
    if (lanes[i] != lanes[i - 1] + 1) {
      return false;
    }
  }
  return true;
}

// We can permute by words if the mask is reducible to a word mask, but the x64
// lowering is only efficient if we can permute the high and low quadwords
// separately, possibly after swapping quadwords.
static PermuteX64I16x8Action CalculateX64Permute16x8(SimdConstant* control) {
  const SimdConstant::I16x8& lanes = control->asInt16x8();
  SimdConstant::I16x8 mapped;
  MapLanes(mapped, lanes, [](int x) -> int { return x < 4 ? 0 : 1; });
  int i = ScanConstant(mapped, mapped[0], 0);
  if (i != 4) {
    return PermuteX64I16x8Action::UNAVAILABLE;
  }
  i = ScanConstant(mapped, mapped[4], 4);
  if (i != 8) {
    return PermuteX64I16x8Action::UNAVAILABLE;
  }
  // Now compute the operation bits.  `mapped` holds the adjusted lane mask.
  memcpy(mapped, lanes, sizeof(mapped));
  uint16_t op = 0;
  if (mapped[0] > mapped[4]) {
    op |= PermuteX64I16x8Action::SWAP_QWORDS;
  }
  for (auto& m : mapped) {
    m &= 3;
  }
  if (!IsIdentity(mapped, 0, 4, 0)) {
    op |= PermuteX64I16x8Action::PERM_LOW;
  }
  if (!IsIdentity(mapped, 4, 4, 0)) {
    op |= PermuteX64I16x8Action::PERM_HIGH;
  }
  MOZ_ASSERT(op != PermuteX64I16x8Action::UNAVAILABLE);
  *control = SimdConstant::CreateX8(mapped);
  return (PermuteX64I16x8Action)op;
}

#endif

void CodeGenerator::visitWasmPermuteSimd128(LWasmPermuteSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister src = ToFloatRegister(ins->src());
  FloatRegister dest = ToFloatRegister(ins->output());
  SimdConstant control = ins->control();
  switch (ins->op()) {
    // For broadcast, would MOVDDUP be better than PSHUFD for the last step?
    case LWasmPermuteSimd128::BROADCAST_8x16: {
      const SimdConstant::I8x16& mask = control.asInt8x16();
      int8_t source = mask[0];
      if (src != dest) {
        masm.moveSimd128(src, dest);
      }
      if (source < 8) {
        masm.interleaveLowInt8x16(dest, dest);
      } else {
        masm.interleaveHighInt8x16(dest, dest);
        source -= 8;
      }
      uint16_t v = uint16_t(source & 3);
      uint16_t wordMask[4] = {v, v, v, v};
      if (source < 4) {
        masm.permuteLowInt16x8(wordMask, dest, dest);
        uint32_t dwordMask[4] = {0, 0, 0, 0};
        masm.permuteInt32x4(dwordMask, dest, dest);
      } else {
        masm.permuteHighInt16x8(wordMask, dest, dest);
        uint32_t dwordMask[4] = {2, 2, 2, 2};
        masm.permuteInt32x4(dwordMask, dest, dest);
      }
      break;
    }
    case LWasmPermuteSimd128::BROADCAST_16x8: {
      const SimdConstant::I16x8& mask = control.asInt16x8();
      int16_t source = mask[0];
      uint16_t v = uint16_t(source & 3);
      uint16_t wordMask[4] = {v, v, v, v};
      if (source < 4) {
        masm.permuteLowInt16x8(wordMask, src, dest);
        uint32_t dwordMask[4] = {0, 0, 0, 0};
        masm.permuteInt32x4(dwordMask, dest, dest);
      } else {
        masm.permuteHighInt16x8(wordMask, src, dest);
        uint32_t dwordMask[4] = {2, 2, 2, 2};
        masm.permuteInt32x4(dwordMask, dest, dest);
      }
      break;
    }
    case LWasmPermuteSimd128::MOVE: {
      masm.moveSimd128(src, dest);
      break;
    }
    case LWasmPermuteSimd128::PERMUTE_8x16: {
      const SimdConstant::I8x16& mask = control.asInt8x16();
#  ifdef DEBUG
      DebugOnly<int> i;
      for (i = 0; i < 16 && mask[i] == i; i++) {
      }
      MOZ_ASSERT(i < 16, "Should have been a MOVE operation");
#  endif
      masm.permuteInt8x16(reinterpret_cast<const uint8_t*>(mask), src, dest);
      break;
    }
    case LWasmPermuteSimd128::PERMUTE_16x8: {
#  ifdef DEBUG
      const SimdConstant::I16x8& mask = control.asInt16x8();
      DebugOnly<int> i;
      for (i = 0; i < 8 && mask[i] == i; i++) {
      }
      MOZ_ASSERT(i < 8, "Should have been a MOVE operation");
#  endif
      PermuteX64I16x8Action op = CalculateX64Permute16x8(&control);
      if (op != PermuteX64I16x8Action::UNAVAILABLE) {
        const SimdConstant::I16x8& mask = control.asInt16x8();
        if (op & PermuteX64I16x8Action::SWAP_QWORDS) {
          uint32_t dwordMask[4] = {2, 3, 0, 1};
          masm.permuteInt32x4(dwordMask, src, dest);
          src = dest;
        }
        if (op & PermuteX64I16x8Action::PERM_LOW) {
          masm.permuteLowInt16x8(reinterpret_cast<const uint16_t*>(mask) + 0,
                                 src, dest);
          src = dest;
        }
        if (op & PermuteX64I16x8Action::PERM_HIGH) {
          masm.permuteHighInt16x8(reinterpret_cast<const uint16_t*>(mask) + 4,
                                  src, dest);
          src = dest;
        }
      } else {
        const SimdConstant::I16x8& wmask = control.asInt16x8();
        uint8_t mask[16];
        for (unsigned i = 0; i < 16; i += 2) {
          mask[i] = wmask[i / 2] * 2;
          mask[i + 1] = wmask[i / 2] * 2 + 1;
        }
        masm.permuteInt8x16(mask, src, dest);
      }
      break;
    }
    case LWasmPermuteSimd128::PERMUTE_32x4: {
      const SimdConstant::I32x4& mask = control.asInt32x4();
#  ifdef DEBUG
      DebugOnly<int> i;
      for (i = 0; i < 4 && mask[i] == i; i++) {
      }
      MOZ_ASSERT(i < 4, "Should have been a MOVE operation");
#  endif
      masm.permuteInt32x4(reinterpret_cast<const uint32_t*>(mask), src, dest);
      break;
    }
    case LWasmPermuteSimd128::ROTATE_RIGHT_8x16: {
      if (src != dest) {
        masm.moveSimd128(src, dest);
      }
      int8_t count = control.asInt8x16()[0];
      MOZ_ASSERT(count > 0, "Should have been a MOVE operation");
      masm.concatAndRightShiftSimd128(dest, dest, count);
      break;
    }
    case LWasmPermuteSimd128::SHIFT_LEFT_8x16: {
      int8_t count = control.asInt8x16()[0];
      MOZ_ASSERT(count > 0, "Should have been a MOVE operation");
      masm.leftShiftSimd128(Imm32(count), src, dest);
      break;
    }
    case LWasmPermuteSimd128::SHIFT_RIGHT_8x16: {
      int8_t count = control.asInt8x16()[0];
      MOZ_ASSERT(count > 0, "Should have been a MOVE operation");
      masm.rightShiftSimd128(Imm32(count), src, dest);
      break;
    }
    default: {
      MOZ_CRASH("Unsupported SIMD permutation operation");
    }
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmReplaceLaneSimd128(LWasmReplaceLaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister lhsDest = ToFloatRegister(ins->lhsDest());
  const LAllocation* rhs = ins->rhs();
  uint32_t laneIndex = ins->laneIndex();

  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16ReplaceLane:
      masm.replaceLaneInt8x16(laneIndex, ToRegister(rhs), lhsDest);
      break;
    case wasm::SimdOp::I16x8ReplaceLane:
      masm.replaceLaneInt16x8(laneIndex, ToRegister(rhs), lhsDest);
      break;
    case wasm::SimdOp::I32x4ReplaceLane:
      masm.replaceLaneInt32x4(laneIndex, ToRegister(rhs), lhsDest);
      break;
    case wasm::SimdOp::F32x4ReplaceLane:
      masm.replaceLaneFloat32x4(laneIndex, ToFloatRegister(rhs), lhsDest);
      break;
    case wasm::SimdOp::F64x2ReplaceLane:
      masm.replaceLaneFloat64x2(laneIndex, ToFloatRegister(rhs), lhsDest);
      break;
    default:
      MOZ_CRASH("ReplaceLane SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmReplaceInt64LaneSimd128(
    LWasmReplaceInt64LaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  MOZ_RELEASE_ASSERT(ins->simdOp() == wasm::SimdOp::I64x2ReplaceLane);
  masm.replaceLaneInt64x2(ins->laneIndex(), ToRegister64(ins->rhs()),
                          ToFloatRegister(ins->lhsDest()));
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmScalarToSimd128(LWasmScalarToSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister dest = ToFloatRegister(ins->output());

  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16Splat:
      masm.splatX16(ToRegister(ins->src()), dest);
      break;
    case wasm::SimdOp::I16x8Splat:
      masm.splatX8(ToRegister(ins->src()), dest);
      break;
    case wasm::SimdOp::I32x4Splat:
      masm.splatX4(ToRegister(ins->src()), dest);
      break;
    case wasm::SimdOp::F32x4Splat:
      masm.splatX4(ToFloatRegister(ins->src()), dest);
      break;
    case wasm::SimdOp::F64x2Splat:
      masm.splatX2(ToFloatRegister(ins->src()), dest);
      break;
    default:
      MOZ_CRASH("ScalarToSimd128 SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmInt64ToSimd128(LWasmInt64ToSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  Register64 src = ToRegister64(ins->src());
  FloatRegister dest = ToFloatRegister(ins->output());

  switch (ins->simdOp()) {
    case wasm::SimdOp::I64x2Splat:
      masm.splatX2(src, dest);
      break;
    case wasm::SimdOp::I16x8LoadS8x8:
      masm.moveGPR64ToDouble(src, dest);
      masm.widenLowInt8x16(dest, dest);
      break;
    case wasm::SimdOp::I16x8LoadU8x8:
      masm.moveGPR64ToDouble(src, dest);
      masm.unsignedWidenLowInt8x16(dest, dest);
      break;
    case wasm::SimdOp::I32x4LoadS16x4:
      masm.moveGPR64ToDouble(src, dest);
      masm.widenLowInt16x8(dest, dest);
      break;
    case wasm::SimdOp::I32x4LoadU16x4:
      masm.moveGPR64ToDouble(src, dest);
      masm.unsignedWidenLowInt16x8(dest, dest);
      break;
    case wasm::SimdOp::I64x2LoadS32x2:
      masm.moveGPR64ToDouble(src, dest);
      masm.widenLowInt32x4(dest, dest);
      break;
    case wasm::SimdOp::I64x2LoadU32x2:
      masm.moveGPR64ToDouble(src, dest);
      masm.unsignedWidenLowInt32x4(dest, dest);
      break;
    default:
      MOZ_CRASH("Int64ToSimd128 SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmUnarySimd128(LWasmUnarySimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister src = ToFloatRegister(ins->src());
  FloatRegister dest = ToFloatRegister(ins->output());

  switch (ins->simdOp()) {
    case wasm::SimdOp::I8x16Neg:
      masm.negInt8x16(src, dest);
      break;
    case wasm::SimdOp::I16x8Neg:
      masm.negInt16x8(src, dest);
      break;
    case wasm::SimdOp::I16x8WidenLowSI8x16:
      masm.widenLowInt8x16(src, dest);
      break;
    case wasm::SimdOp::I16x8WidenHighSI8x16:
      masm.widenHighInt8x16(src, dest);
      break;
    case wasm::SimdOp::I16x8WidenLowUI8x16:
      masm.unsignedWidenLowInt8x16(src, dest);
      break;
    case wasm::SimdOp::I16x8WidenHighUI8x16:
      masm.unsignedWidenHighInt8x16(src, dest);
      break;
    case wasm::SimdOp::I32x4Neg:
      masm.negInt32x4(src, dest);
      break;
    case wasm::SimdOp::I32x4WidenLowSI16x8:
      masm.widenLowInt16x8(src, dest);
      break;
    case wasm::SimdOp::I32x4WidenHighSI16x8:
      masm.widenHighInt16x8(src, dest);
      break;
    case wasm::SimdOp::I32x4WidenLowUI16x8:
      masm.unsignedWidenLowInt16x8(src, dest);
      break;
    case wasm::SimdOp::I32x4WidenHighUI16x8:
      masm.unsignedWidenHighInt16x8(src, dest);
      break;
    case wasm::SimdOp::I32x4TruncSSatF32x4:
      masm.truncSatFloat32x4ToInt32x4(src, dest);
      break;
    case wasm::SimdOp::I32x4TruncUSatF32x4:
      masm.unsignedTruncSatFloat32x4ToInt32x4(src, dest,
                                              ToFloatRegister(ins->temp()));
      break;
    case wasm::SimdOp::I64x2Neg:
      masm.negInt64x2(src, dest);
      break;
    case wasm::SimdOp::I64x2WidenLowSI32x4:
      masm.widenLowInt32x4(src, dest);
      break;
    case wasm::SimdOp::I64x2WidenHighSI32x4:
      masm.widenHighInt32x4(src, dest);
      break;
    case wasm::SimdOp::I64x2WidenLowUI32x4:
      masm.unsignedWidenLowInt32x4(src, dest);
      break;
    case wasm::SimdOp::I64x2WidenHighUI32x4:
      masm.unsignedWidenHighInt32x4(src, dest);
      break;
    case wasm::SimdOp::F32x4Abs:
      masm.absFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F32x4Neg:
      masm.negFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F32x4Sqrt:
      masm.sqrtFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F32x4ConvertSI32x4:
      masm.convertInt32x4ToFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F32x4ConvertUI32x4:
      masm.unsignedConvertInt32x4ToFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F64x2Abs:
      masm.absFloat64x2(src, dest);
      break;
    case wasm::SimdOp::F64x2Neg:
      masm.negFloat64x2(src, dest);
      break;
    case wasm::SimdOp::F64x2Sqrt:
      masm.sqrtFloat64x2(src, dest);
      break;
    case wasm::SimdOp::V128Not:
      masm.bitwiseNotSimd128(src, dest);
      break;
    case wasm::SimdOp::I8x16Popcnt:
      masm.popcntInt8x16(src, dest, ToFloatRegister(ins->temp()));
      break;
    case wasm::SimdOp::I8x16Abs:
      masm.absInt8x16(src, dest);
      break;
    case wasm::SimdOp::I16x8Abs:
      masm.absInt16x8(src, dest);
      break;
    case wasm::SimdOp::I32x4Abs:
      masm.absInt32x4(src, dest);
      break;
    case wasm::SimdOp::I64x2Abs:
      masm.absInt64x2(src, dest);
      break;
    case wasm::SimdOp::F32x4Ceil:
      masm.ceilFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F32x4Floor:
      masm.floorFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F32x4Trunc:
      masm.truncFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F32x4Nearest:
      masm.nearestFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F64x2Ceil:
      masm.ceilFloat64x2(src, dest);
      break;
    case wasm::SimdOp::F64x2Floor:
      masm.floorFloat64x2(src, dest);
      break;
    case wasm::SimdOp::F64x2Trunc:
      masm.truncFloat64x2(src, dest);
      break;
    case wasm::SimdOp::F64x2Nearest:
      masm.nearestFloat64x2(src, dest);
      break;
    case wasm::SimdOp::F32x4DemoteF64x2Zero:
      masm.convertFloat64x2ToFloat32x4(src, dest);
      break;
    case wasm::SimdOp::F64x2PromoteLowF32x4:
      masm.convertFloat32x4ToFloat64x2(src, dest);
      break;
    case wasm::SimdOp::F64x2ConvertLowI32x4S:
      masm.convertInt32x4ToFloat64x2(src, dest);
      break;
    case wasm::SimdOp::F64x2ConvertLowI32x4U:
      masm.unsignedConvertInt32x4ToFloat64x2(src, dest);
      break;
    case wasm::SimdOp::I32x4TruncSatF64x2SZero:
      masm.truncSatFloat64x2ToInt32x4(src, dest, ToFloatRegister(ins->temp()));
      break;
    case wasm::SimdOp::I32x4TruncSatF64x2UZero:
      masm.unsignedTruncSatFloat64x2ToInt32x4(src, dest,
                                              ToFloatRegister(ins->temp()));
      break;
    case wasm::SimdOp::I16x8ExtAddPairwiseI8x16S:
      masm.extAddPairwiseInt8x16(src, dest);
      break;
    case wasm::SimdOp::I16x8ExtAddPairwiseI8x16U:
      masm.unsignedExtAddPairwiseInt8x16(src, dest);
      break;
    case wasm::SimdOp::I32x4ExtAddPairwiseI16x8S:
      masm.extAddPairwiseInt16x8(src, dest);
      break;
    case wasm::SimdOp::I32x4ExtAddPairwiseI16x8U:
      masm.unsignedExtAddPairwiseInt16x8(src, dest);
      break;
    default:
      MOZ_CRASH("Unary SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmReduceSimd128(LWasmReduceSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister src = ToFloatRegister(ins->src());
  const LDefinition* dest = ins->output();
  uint32_t imm = ins->imm();

  switch (ins->simdOp()) {
    case wasm::SimdOp::V128AnyTrue:
      masm.anyTrueSimd128(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I8x16AllTrue:
      masm.allTrueInt8x16(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I16x8AllTrue:
      masm.allTrueInt16x8(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I32x4AllTrue:
      masm.allTrueInt32x4(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I64x2AllTrue:
      masm.allTrueInt64x2(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I8x16Bitmask:
      masm.bitmaskInt8x16(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I16x8Bitmask:
      masm.bitmaskInt16x8(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I32x4Bitmask:
      masm.bitmaskInt32x4(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I64x2Bitmask:
      masm.bitmaskInt64x2(src, ToRegister(dest));
      break;
    case wasm::SimdOp::I8x16ExtractLaneS:
      masm.extractLaneInt8x16(imm, src, ToRegister(dest));
      break;
    case wasm::SimdOp::I8x16ExtractLaneU:
      masm.unsignedExtractLaneInt8x16(imm, src, ToRegister(dest));
      break;
    case wasm::SimdOp::I16x8ExtractLaneS:
      masm.extractLaneInt16x8(imm, src, ToRegister(dest));
      break;
    case wasm::SimdOp::I16x8ExtractLaneU:
      masm.unsignedExtractLaneInt16x8(imm, src, ToRegister(dest));
      break;
    case wasm::SimdOp::I32x4ExtractLane:
      masm.extractLaneInt32x4(imm, src, ToRegister(dest));
      break;
    case wasm::SimdOp::F32x4ExtractLane:
      masm.extractLaneFloat32x4(imm, src, ToFloatRegister(dest));
      break;
    case wasm::SimdOp::F64x2ExtractLane:
      masm.extractLaneFloat64x2(imm, src, ToFloatRegister(dest));
      break;
    default:
      MOZ_CRASH("Reduce SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmReduceAndBranchSimd128(
    LWasmReduceAndBranchSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister src = ToFloatRegister(ins->src());

  switch (ins->simdOp()) {
    case wasm::SimdOp::V128AnyTrue:
      // Set the zero flag if all of the lanes are zero, and branch on that.
      masm.vptest(src, src);
      emitBranch(Assembler::NotEqual, ins->ifTrue(), ins->ifFalse());
      break;
    case wasm::SimdOp::I8x16AllTrue:
    case wasm::SimdOp::I16x8AllTrue:
    case wasm::SimdOp::I32x4AllTrue:
    case wasm::SimdOp::I64x2AllTrue: {
      // Compare all lanes to zero, set the zero flag if none of the lanes are
      // zero, and branch on that.
      ScratchSimd128Scope tmp(masm);
      masm.vpxor(tmp, tmp, tmp);
      switch (ins->simdOp()) {
        case wasm::SimdOp::I8x16AllTrue:
          masm.vpcmpeqb(Operand(src), tmp, tmp);
          break;
        case wasm::SimdOp::I16x8AllTrue:
          masm.vpcmpeqw(Operand(src), tmp, tmp);
          break;
        case wasm::SimdOp::I32x4AllTrue:
          masm.vpcmpeqd(Operand(src), tmp, tmp);
          break;
        case wasm::SimdOp::I64x2AllTrue:
          masm.vpcmpeqq(Operand(src), tmp, tmp);
          break;
        default:
          MOZ_CRASH();
      }
      masm.vptest(tmp, tmp);
      emitBranch(Assembler::Equal, ins->ifTrue(), ins->ifFalse());
      break;
    }
    case wasm::SimdOp::I16x8Bitmask: {
      masm.bitwiseTestSimd128(SimdConstant::SplatX8(0x8000), src);
      emitBranch(Assembler::NotEqual, ins->ifTrue(), ins->ifFalse());
      break;
    }
    default:
      MOZ_CRASH("Reduce-and-branch SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmReduceSimd128ToInt64(
    LWasmReduceSimd128ToInt64* ins) {
#ifdef ENABLE_WASM_SIMD
  FloatRegister src = ToFloatRegister(ins->src());
  Register64 dest = ToOutRegister64(ins);
  uint32_t imm = ins->imm();

  switch (ins->simdOp()) {
    case wasm::SimdOp::I64x2ExtractLane:
      masm.extractLaneInt64x2(imm, src, dest);
      break;
    default:
      MOZ_CRASH("Reduce SimdOp not implemented");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmLoadLaneSimd128(LWasmLoadLaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  const MWasmLoadLaneSimd128* mir = ins->mir();
  const wasm::MemoryAccessDesc& access = mir->access();

  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < masm.wasmMaxOffsetGuardLimit());

  const LAllocation* value = ins->src();
  Operand srcAddr = toMemoryAccessOperand(ins, offset);

  masm.append(access, masm.size());
  switch (ins->laneSize()) {
    case 1: {
      masm.vpinsrb(ins->laneIndex(), srcAddr, ToFloatRegister(value),
                   ToFloatRegister(value));
      break;
    }
    case 2: {
      masm.vpinsrw(ins->laneIndex(), srcAddr, ToFloatRegister(value),
                   ToFloatRegister(value));
      break;
    }
    case 4: {
      masm.vinsertps(ins->laneIndex() << 4, srcAddr, ToFloatRegister(value),
                     ToFloatRegister(value));
      break;
    }
    case 8: {
      if (ins->laneIndex() == 0) {
        masm.vmovlps(srcAddr, ToFloatRegister(value), ToFloatRegister(value));
      } else {
        masm.vmovhps(srcAddr, ToFloatRegister(value), ToFloatRegister(value));
      }
      break;
    }
    default:
      MOZ_CRASH("Unsupported load lane size");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

void CodeGenerator::visitWasmStoreLaneSimd128(LWasmStoreLaneSimd128* ins) {
#ifdef ENABLE_WASM_SIMD
  const MWasmStoreLaneSimd128* mir = ins->mir();
  const wasm::MemoryAccessDesc& access = mir->access();

  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < masm.wasmMaxOffsetGuardLimit());

  const LAllocation* src = ins->src();
  Operand destAddr = toMemoryAccessOperand(ins, offset);

  masm.append(access, masm.size());
  switch (ins->laneSize()) {
    case 1: {
      masm.vpextrb(ins->laneIndex(), ToFloatRegister(src), destAddr);
      break;
    }
    case 2: {
      masm.vpextrw(ins->laneIndex(), ToFloatRegister(src), destAddr);
      break;
    }
    case 4: {
      unsigned lane = ins->laneIndex();
      if (lane == 0) {
        masm.vmovss(ToFloatRegister(src), destAddr);
      } else {
        masm.vextractps(lane, ToFloatRegister(src), destAddr);
      }
      break;
    }
    case 8: {
      if (ins->laneIndex() == 0) {
        masm.vmovlps(ToFloatRegister(src), destAddr);
      } else {
        masm.vmovhps(ToFloatRegister(src), destAddr);
      }
      break;
    }
    default:
      MOZ_CRASH("Unsupported store lane size");
  }
#else
  MOZ_CRASH("No SIMD");
#endif
}

}  // namespace jit
}  // namespace js
