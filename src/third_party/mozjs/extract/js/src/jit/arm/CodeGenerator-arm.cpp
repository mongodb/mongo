/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/CodeGenerator-arm.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"

#include <iterator>

#include "jsnum.h"

#include "jit/CodeGenerator.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitRuntime.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "js/Conversions.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "vm/Shape.h"
#include "vm/TraceLogging.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using JS::GenericNaN;
using JS::ToInt32;
using mozilla::DebugOnly;
using mozilla::FloorLog2;
using mozilla::NegativeInfinity;

// shared
CodeGeneratorARM::CodeGeneratorARM(MIRGenerator* gen, LIRGraph* graph,
                                   MacroAssembler* masm)
    : CodeGeneratorShared(gen, graph, masm) {}

Register64 CodeGeneratorARM::ToOperandOrRegister64(
    const LInt64Allocation input) {
  return ToRegister64(input);
}

void CodeGeneratorARM::emitBranch(Assembler::Condition cond,
                                  MBasicBlock* mirTrue, MBasicBlock* mirFalse) {
  if (isNextBlock(mirFalse->lir())) {
    jumpToBlock(mirTrue, cond);
  } else {
    jumpToBlock(mirFalse, Assembler::InvertCondition(cond));
    jumpToBlock(mirTrue);
  }
}

void OutOfLineBailout::accept(CodeGeneratorARM* codegen) {
  codegen->visitOutOfLineBailout(this);
}

void CodeGenerator::visitTestIAndBranch(LTestIAndBranch* test) {
  const LAllocation* opd = test->getOperand(0);
  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  // Test the operand
  masm.as_cmp(ToRegister(opd), Imm8(0));

  if (isNextBlock(ifFalse->lir())) {
    jumpToBlock(ifTrue, Assembler::NonZero);
  } else if (isNextBlock(ifTrue->lir())) {
    jumpToBlock(ifFalse, Assembler::Zero);
  } else {
    jumpToBlock(ifFalse, Assembler::Zero);
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitCompare(LCompare* comp) {
  Assembler::Condition cond =
      JSOpToCondition(comp->mir()->compareType(), comp->jsop());
  const LAllocation* left = comp->getOperand(0);
  const LAllocation* right = comp->getOperand(1);
  const LDefinition* def = comp->getDef(0);

  ScratchRegisterScope scratch(masm);

  if (right->isConstant()) {
    masm.ma_cmp(ToRegister(left), Imm32(ToInt32(right)), scratch);
  } else if (right->isRegister()) {
    masm.ma_cmp(ToRegister(left), ToRegister(right));
  } else {
    SecondScratchRegisterScope scratch2(masm);
    masm.ma_cmp(ToRegister(left), Operand(ToAddress(right)), scratch, scratch2);
  }
  masm.ma_mov(Imm32(0), ToRegister(def));
  masm.ma_mov(Imm32(1), ToRegister(def), cond);
}

void CodeGenerator::visitCompareAndBranch(LCompareAndBranch* comp) {
  Assembler::Condition cond =
      JSOpToCondition(comp->cmpMir()->compareType(), comp->jsop());
  const LAllocation* left = comp->left();
  const LAllocation* right = comp->right();

  ScratchRegisterScope scratch(masm);

  if (right->isConstant()) {
    masm.ma_cmp(ToRegister(left), Imm32(ToInt32(right)), scratch);
  } else if (right->isRegister()) {
    masm.ma_cmp(ToRegister(left), ToRegister(right));
  } else {
    SecondScratchRegisterScope scratch2(masm);
    masm.ma_cmp(ToRegister(left), Operand(ToAddress(right)), scratch, scratch2);
  }
  emitBranch(cond, comp->ifTrue(), comp->ifFalse());
}

bool CodeGeneratorARM::generateOutOfLineCode() {
  if (!CodeGeneratorShared::generateOutOfLineCode()) {
    return false;
  }

  if (deoptLabel_.used()) {
    // All non-table-based bailouts will go here.
    masm.bind(&deoptLabel_);

    // Push the frame size, so the handler can recover the IonScript.
    masm.ma_mov(Imm32(frameSize()), lr);

    TrampolinePtr handler = gen->jitRuntime()->getGenericBailoutHandler();
    masm.jump(handler);
  }

  return !masm.oom();
}

void CodeGeneratorARM::bailoutIf(Assembler::Condition condition,
                                 LSnapshot* snapshot) {
  encode(snapshot);

  // Though the assembler doesn't track all frame pushes, at least make sure
  // the known value makes sense. We can't use bailout tables if the stack
  // isn't properly aligned to the static frame size.
  MOZ_ASSERT_IF(frameClass_ != FrameSizeClass::None(),
                frameClass_.frameSize() == masm.framePushed());

  if (assignBailoutId(snapshot)) {
    uint8_t* bailoutTable = Assembler::BailoutTableStart(deoptTable_->value);
    uint8_t* code =
        bailoutTable + snapshot->bailoutId() * BAILOUT_TABLE_ENTRY_SIZE;
    masm.ma_b(code, condition);
    return;
  }

  // We could not use a jump table, either because all bailout IDs were
  // reserved, or a jump table is not optimal for this frame size or
  // platform. Whatever, we will generate a lazy bailout.
  InlineScriptTree* tree = snapshot->mir()->block()->trackedTree();
  OutOfLineBailout* ool =
      new (alloc()) OutOfLineBailout(snapshot, masm.framePushed());

  // All bailout code is associated with the bytecodeSite of the block we are
  // bailing out from.
  addOutOfLineCode(ool,
                   new (alloc()) BytecodeSite(tree, tree->script()->code()));

  masm.ma_b(ool->entry(), condition);
}

void CodeGeneratorARM::bailoutFrom(Label* label, LSnapshot* snapshot) {
  MOZ_ASSERT_IF(!masm.oom(), label->used());
  MOZ_ASSERT_IF(!masm.oom(), !label->bound());

  encode(snapshot);

  // Though the assembler doesn't track all frame pushes, at least make sure
  // the known value makes sense. We can't use bailout tables if the stack
  // isn't properly aligned to the static frame size.
  MOZ_ASSERT_IF(frameClass_ != FrameSizeClass::None(),
                frameClass_.frameSize() == masm.framePushed());

  // On ARM we don't use a bailout table.
  InlineScriptTree* tree = snapshot->mir()->block()->trackedTree();
  OutOfLineBailout* ool =
      new (alloc()) OutOfLineBailout(snapshot, masm.framePushed());

  // All bailout code is associated with the bytecodeSite of the block we are
  // bailing out from.
  addOutOfLineCode(ool,
                   new (alloc()) BytecodeSite(tree, tree->script()->code()));

  masm.retarget(label, ool->entry());
}

void CodeGeneratorARM::bailout(LSnapshot* snapshot) {
  Label label;
  masm.ma_b(&label);
  bailoutFrom(&label, snapshot);
}

void CodeGeneratorARM::visitOutOfLineBailout(OutOfLineBailout* ool) {
  ScratchRegisterScope scratch(masm);
  masm.ma_mov(Imm32(ool->snapshot()->snapshotOffset()), scratch);
  masm.ma_push(scratch);  // BailoutStack::padding_
  masm.ma_push(scratch);  // BailoutStack::snapshotOffset_
  masm.ma_b(&deoptLabel_);
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
  const LAllocation* lhs = ins->getOperand(0);
  const LAllocation* rhs = ins->getOperand(1);
  const LDefinition* dest = ins->getDef(0);

  ScratchRegisterScope scratch(masm);

  if (rhs->isConstant()) {
    masm.ma_add(ToRegister(lhs), Imm32(ToInt32(rhs)), ToRegister(dest), scratch,
                SetCC);
  } else if (rhs->isRegister()) {
    masm.ma_add(ToRegister(lhs), ToRegister(rhs), ToRegister(dest), SetCC);
  } else {
    masm.ma_add(ToRegister(lhs), Operand(ToAddress(rhs)), ToRegister(dest),
                SetCC);
  }

  if (ins->snapshot()) {
    bailoutIf(Assembler::Overflow, ins->snapshot());
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
  const LAllocation* lhs = ins->getOperand(0);
  const LAllocation* rhs = ins->getOperand(1);
  const LDefinition* dest = ins->getDef(0);

  ScratchRegisterScope scratch(masm);

  if (rhs->isConstant()) {
    masm.ma_sub(ToRegister(lhs), Imm32(ToInt32(rhs)), ToRegister(dest), scratch,
                SetCC);
  } else if (rhs->isRegister()) {
    masm.ma_sub(ToRegister(lhs), ToRegister(rhs), ToRegister(dest), SetCC);
  } else {
    masm.ma_sub(ToRegister(lhs), Operand(ToAddress(rhs)), ToRegister(dest),
                SetCC);
  }

  if (ins->snapshot()) {
    bailoutIf(Assembler::Overflow, ins->snapshot());
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

void CodeGenerator::visitMulI(LMulI* ins) {
  const LAllocation* lhs = ins->getOperand(0);
  const LAllocation* rhs = ins->getOperand(1);
  const LDefinition* dest = ins->getDef(0);
  MMul* mul = ins->mir();
  MOZ_ASSERT_IF(mul->mode() == MMul::Integer,
                !mul->canBeNegativeZero() && !mul->canOverflow());

  if (rhs->isConstant()) {
    // Bailout when this condition is met.
    Assembler::Condition c = Assembler::Overflow;
    // Bailout on -0.0
    int32_t constant = ToInt32(rhs);
    if (mul->canBeNegativeZero() && constant <= 0) {
      Assembler::Condition bailoutCond =
          (constant == 0) ? Assembler::LessThan : Assembler::Equal;
      masm.as_cmp(ToRegister(lhs), Imm8(0));
      bailoutIf(bailoutCond, ins->snapshot());
    }
    // TODO: move these to ma_mul.
    switch (constant) {
      case -1:
        masm.as_rsb(ToRegister(dest), ToRegister(lhs), Imm8(0), SetCC);
        break;
      case 0:
        masm.ma_mov(Imm32(0), ToRegister(dest));
        return;  // Escape overflow check;
      case 1:
        // Nop
        masm.ma_mov(ToRegister(lhs), ToRegister(dest));
        return;  // Escape overflow check;
      case 2:
        masm.ma_add(ToRegister(lhs), ToRegister(lhs), ToRegister(dest), SetCC);
        // Overflow is handled later.
        break;
      default: {
        bool handled = false;
        if (constant > 0) {
          // Try shift and add sequences for a positive constant.
          if (!mul->canOverflow()) {
            // If it cannot overflow, we can do lots of optimizations.
            Register src = ToRegister(lhs);
            uint32_t shift = FloorLog2(constant);
            uint32_t rest = constant - (1 << shift);
            // See if the constant has one bit set, meaning it can be
            // encoded as a bitshift.
            if ((1 << shift) == constant) {
              masm.ma_lsl(Imm32(shift), src, ToRegister(dest));
              handled = true;
            } else {
              // If the constant cannot be encoded as (1 << C1), see
              // if it can be encoded as (1 << C1) | (1 << C2), which
              // can be computed using an add and a shift.
              uint32_t shift_rest = FloorLog2(rest);
              if ((1u << shift_rest) == rest) {
                masm.as_add(ToRegister(dest), src,
                            lsl(src, shift - shift_rest));
                if (shift_rest != 0) {
                  masm.ma_lsl(Imm32(shift_rest), ToRegister(dest),
                              ToRegister(dest));
                }
                handled = true;
              }
            }
          } else if (ToRegister(lhs) != ToRegister(dest)) {
            // To stay on the safe side, only optimize things that are a
            // power of 2.

            uint32_t shift = FloorLog2(constant);
            if ((1 << shift) == constant) {
              // dest = lhs * pow(2,shift)
              masm.ma_lsl(Imm32(shift), ToRegister(lhs), ToRegister(dest));
              // At runtime, check (lhs == dest >> shift), if this
              // does not hold, some bits were lost due to overflow,
              // and the computation should be resumed as a double.
              masm.as_cmp(ToRegister(lhs), asr(ToRegister(dest), shift));
              c = Assembler::NotEqual;
              handled = true;
            }
          }
        }

        if (!handled) {
          ScratchRegisterScope scratch(masm);
          if (mul->canOverflow()) {
            c = masm.ma_check_mul(ToRegister(lhs), Imm32(ToInt32(rhs)),
                                  ToRegister(dest), scratch, c);
          } else {
            masm.ma_mul(ToRegister(lhs), Imm32(ToInt32(rhs)), ToRegister(dest),
                        scratch);
          }
        }
      }
    }
    // Bailout on overflow.
    if (mul->canOverflow()) {
      bailoutIf(c, ins->snapshot());
    }
  } else {
    Assembler::Condition c = Assembler::Overflow;

    if (mul->canOverflow()) {
      ScratchRegisterScope scratch(masm);
      c = masm.ma_check_mul(ToRegister(lhs), ToRegister(rhs), ToRegister(dest),
                            scratch, c);
    } else {
      masm.ma_mul(ToRegister(lhs), ToRegister(rhs), ToRegister(dest));
    }

    // Bailout on overflow.
    if (mul->canOverflow()) {
      bailoutIf(c, ins->snapshot());
    }

    if (mul->canBeNegativeZero()) {
      Label done;
      masm.as_cmp(ToRegister(dest), Imm8(0));
      masm.ma_b(&done, Assembler::NotEqual);

      // Result is -0 if lhs or rhs is negative.
      masm.ma_cmn(ToRegister(lhs), ToRegister(rhs));
      bailoutIf(Assembler::Signed, ins->snapshot());

      masm.bind(&done);
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

void CodeGeneratorARM::divICommon(MDiv* mir, Register lhs, Register rhs,
                                  Register output, LSnapshot* snapshot,
                                  Label& done) {
  ScratchRegisterScope scratch(masm);

  if (mir->canBeNegativeOverflow()) {
    // Handle INT32_MIN / -1;
    // The integer division will give INT32_MIN, but we want -(double)INT32_MIN.

    // Sets EQ if lhs == INT32_MIN.
    masm.ma_cmp(lhs, Imm32(INT32_MIN), scratch);
    // If EQ (LHS == INT32_MIN), sets EQ if rhs == -1.
    masm.ma_cmp(rhs, Imm32(-1), scratch, Assembler::Equal);
    if (mir->canTruncateOverflow()) {
      if (mir->trapOnError()) {
        Label ok;
        masm.ma_b(&ok, Assembler::NotEqual);
        masm.wasmTrap(wasm::Trap::IntegerOverflow, mir->bytecodeOffset());
        masm.bind(&ok);
      } else {
        // (-INT32_MIN)|0 = INT32_MIN
        Label skip;
        masm.ma_b(&skip, Assembler::NotEqual);
        masm.ma_mov(Imm32(INT32_MIN), output);
        masm.ma_b(&done);
        masm.bind(&skip);
      }
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Equal, snapshot);
    }
  }

  // Handle divide by zero.
  if (mir->canBeDivideByZero()) {
    masm.as_cmp(rhs, Imm8(0));
    if (mir->canTruncateInfinities()) {
      if (mir->trapOnError()) {
        Label nonZero;
        masm.ma_b(&nonZero, Assembler::NotEqual);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
        masm.bind(&nonZero);
      } else {
        // Infinity|0 == 0
        Label skip;
        masm.ma_b(&skip, Assembler::NotEqual);
        masm.ma_mov(Imm32(0), output);
        masm.ma_b(&done);
        masm.bind(&skip);
      }
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Equal, snapshot);
    }
  }

  // Handle negative 0.
  if (!mir->canTruncateNegativeZero() && mir->canBeNegativeZero()) {
    Label nonzero;
    masm.as_cmp(lhs, Imm8(0));
    masm.ma_b(&nonzero, Assembler::NotEqual);
    masm.as_cmp(rhs, Imm8(0));
    MOZ_ASSERT(mir->fallible());
    bailoutIf(Assembler::LessThan, snapshot);
    masm.bind(&nonzero);
  }
}

void CodeGenerator::visitDivI(LDivI* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register temp = ToRegister(ins->getTemp(0));
  Register output = ToRegister(ins->output());
  MDiv* mir = ins->mir();

  Label done;
  divICommon(mir, lhs, rhs, output, ins->snapshot(), done);

  if (mir->canTruncateRemainder()) {
    masm.ma_sdiv(lhs, rhs, output);
  } else {
    {
      ScratchRegisterScope scratch(masm);
      masm.ma_sdiv(lhs, rhs, temp);
      masm.ma_mul(temp, rhs, scratch);
      masm.ma_cmp(lhs, scratch);
    }
    bailoutIf(Assembler::NotEqual, ins->snapshot());
    masm.ma_mov(temp, output);
  }

  masm.bind(&done);
}

extern "C" {
extern MOZ_EXPORT int64_t __aeabi_idivmod(int, int);
extern MOZ_EXPORT int64_t __aeabi_uidivmod(int, int);
}

void CodeGenerator::visitSoftDivI(LSoftDivI* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());
  MDiv* mir = ins->mir();

  Label done;
  divICommon(mir, lhs, rhs, output, ins->snapshot(), done);

  if (gen->compilingWasm()) {
    masm.Push(WasmTlsReg);
    int32_t framePushedAfterTls = masm.framePushed();
    masm.setupWasmABICall();
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
    int32_t tlsOffset = masm.framePushed() - framePushedAfterTls;
    masm.callWithABI(mir->bytecodeOffset(),
                     wasm::SymbolicAddress::aeabi_idivmod,
                     mozilla::Some(tlsOffset));
    masm.Pop(WasmTlsReg);
  } else {
    using Fn = int64_t (*)(int, int);
    masm.setupAlignedABICall();
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
    masm.callWithABI<Fn, __aeabi_idivmod>(
        MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);
  }

  // idivmod returns the quotient in r0, and the remainder in r1.
  if (!mir->canTruncateRemainder()) {
    MOZ_ASSERT(mir->fallible());
    masm.as_cmp(r1, Imm8(0));
    bailoutIf(Assembler::NonZero, ins->snapshot());
  }

  masm.bind(&done);
}

void CodeGenerator::visitDivPowTwoI(LDivPowTwoI* ins) {
  MDiv* mir = ins->mir();
  Register lhs = ToRegister(ins->numerator());
  Register output = ToRegister(ins->output());
  int32_t shift = ins->shift();

  if (shift == 0) {
    masm.ma_mov(lhs, output);
    return;
  }

  if (!mir->isTruncated()) {
    // If the remainder is != 0, bailout since this must be a double.
    {
      // The bailout code also needs the scratch register.
      // Here it is only used as a dummy target to set CC flags.
      ScratchRegisterScope scratch(masm);
      masm.as_mov(scratch, lsl(lhs, 32 - shift), SetCC);
    }
    bailoutIf(Assembler::NonZero, ins->snapshot());
  }

  if (!mir->canBeNegativeDividend()) {
    // Numerator is unsigned, so needs no adjusting. Do the shift.
    masm.as_mov(output, asr(lhs, shift));
    return;
  }

  // Adjust the value so that shifting produces a correctly rounded result
  // when the numerator is negative. See 10-1 "Signed Division by a Known
  // Power of 2" in Henry S. Warren, Jr.'s Hacker's Delight.
  ScratchRegisterScope scratch(masm);

  if (shift > 1) {
    masm.as_mov(scratch, asr(lhs, 31));
    masm.as_add(scratch, lhs, lsr(scratch, 32 - shift));
  } else {
    masm.as_add(scratch, lhs, lsr(lhs, 32 - shift));
  }

  // Do the shift.
  masm.as_mov(output, asr(scratch, shift));
}

void CodeGeneratorARM::modICommon(MMod* mir, Register lhs, Register rhs,
                                  Register output, LSnapshot* snapshot,
                                  Label& done) {
  // X % 0 is bad because it will give garbage (or abort), when it should give
  // NaN.

  if (mir->canBeDivideByZero()) {
    masm.as_cmp(rhs, Imm8(0));
    if (mir->isTruncated()) {
      Label nonZero;
      masm.ma_b(&nonZero, Assembler::NotEqual);
      if (mir->trapOnError()) {
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
      } else {
        // NaN|0 == 0
        masm.ma_mov(Imm32(0), output);
        masm.ma_b(&done);
      }
      masm.bind(&nonZero);
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Equal, snapshot);
    }
  }
}

void CodeGenerator::visitModI(LModI* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());
  MMod* mir = ins->mir();

  // Contrary to other architectures (notably x86) INT_MIN % -1 doesn't need to
  // be handled separately. |ma_smod| computes the remainder using the |SDIV|
  // and the |MLS| instructions. On overflow, |SDIV| truncates the result to
  // 32-bit and returns INT_MIN, see ARM Architecture Reference Manual, SDIV
  // instruction.
  //
  //   mls(INT_MIN, sdiv(INT_MIN, -1), -1)
  // = INT_MIN - (sdiv(INT_MIN, -1) * -1)
  // = INT_MIN - (INT_MIN * -1)
  // = INT_MIN - INT_MIN
  // = 0
  //
  // And a zero remainder with a negative dividend is already handled below.

  Label done;
  modICommon(mir, lhs, rhs, output, ins->snapshot(), done);

  {
    ScratchRegisterScope scratch(masm);
    masm.ma_smod(lhs, rhs, output, scratch);
  }

  // If X%Y == 0 and X < 0, then we *actually* wanted to return -0.0.
  if (mir->canBeNegativeDividend()) {
    if (mir->isTruncated()) {
      // -0.0|0 == 0
    } else {
      MOZ_ASSERT(mir->fallible());
      // See if X < 0
      masm.as_cmp(output, Imm8(0));
      masm.ma_b(&done, Assembler::NotEqual);
      masm.as_cmp(lhs, Imm8(0));
      bailoutIf(Assembler::Signed, ins->snapshot());
    }
  }

  masm.bind(&done);
}

void CodeGenerator::visitSoftModI(LSoftModI* ins) {
  // Extract the registers from this instruction.
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());
  Register callTemp = ToRegister(ins->callTemp());
  MMod* mir = ins->mir();
  Label done;

  // Save the lhs in case we end up with a 0 that should be a -0.0 because lhs <
  // 0.
  MOZ_ASSERT(callTemp != lhs);
  MOZ_ASSERT(callTemp != rhs);
  masm.ma_mov(lhs, callTemp);

  // Prevent INT_MIN % -1.
  //
  // |aeabi_idivmod| is allowed to return any arbitrary value when called with
  // |(INT_MIN, -1)|, see "Run-time ABI for the ARM architecture manual". Most
  // implementations perform a non-trapping signed integer division and
  // return the expected result, i.e. INT_MIN. But since we can't rely on this
  // behavior, handle this case separately here.
  if (mir->canBeNegativeDividend()) {
    {
      ScratchRegisterScope scratch(masm);
      // Sets EQ if lhs == INT_MIN
      masm.ma_cmp(lhs, Imm32(INT_MIN), scratch);
      // If EQ (LHS == INT_MIN), sets EQ if rhs == -1
      masm.ma_cmp(rhs, Imm32(-1), scratch, Assembler::Equal);
    }
    if (mir->isTruncated()) {
      // (INT_MIN % -1)|0 == 0
      Label skip;
      masm.ma_b(&skip, Assembler::NotEqual);
      masm.ma_mov(Imm32(0), output);
      masm.ma_b(&done);
      masm.bind(&skip);
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Equal, ins->snapshot());
    }
  }

  modICommon(mir, lhs, rhs, output, ins->snapshot(), done);

  if (gen->compilingWasm()) {
    masm.Push(WasmTlsReg);
    int32_t framePushedAfterTls = masm.framePushed();
    masm.setupWasmABICall();
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
    int32_t tlsOffset = masm.framePushed() - framePushedAfterTls;
    masm.callWithABI(mir->bytecodeOffset(),
                     wasm::SymbolicAddress::aeabi_idivmod,
                     mozilla::Some(tlsOffset));
    masm.Pop(WasmTlsReg);
  } else {
    using Fn = int64_t (*)(int, int);
    masm.setupAlignedABICall();
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
    masm.callWithABI<Fn, __aeabi_idivmod>(
        MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);
  }

  MOZ_ASSERT(r1 != output);
  masm.move32(r1, output);

  // If X%Y == 0 and X < 0, then we *actually* wanted to return -0.0
  if (mir->canBeNegativeDividend()) {
    if (mir->isTruncated()) {
      // -0.0|0 == 0
    } else {
      MOZ_ASSERT(mir->fallible());
      // See if X < 0
      masm.as_cmp(output, Imm8(0));
      masm.ma_b(&done, Assembler::NotEqual);
      masm.as_cmp(callTemp, Imm8(0));
      bailoutIf(Assembler::Signed, ins->snapshot());
    }
  }

  masm.bind(&done);
}

void CodeGenerator::visitModPowTwoI(LModPowTwoI* ins) {
  Register in = ToRegister(ins->getOperand(0));
  Register out = ToRegister(ins->getDef(0));
  MMod* mir = ins->mir();
  Label fin;
  // bug 739870, jbramley has a different sequence that may help with speed
  // here.

  masm.ma_mov(in, out, SetCC);
  masm.ma_b(&fin, Assembler::Zero);
  masm.as_rsb(out, out, Imm8(0), LeaveCC, Assembler::Signed);
  {
    ScratchRegisterScope scratch(masm);
    masm.ma_and(Imm32((1 << ins->shift()) - 1), out, scratch);
  }
  masm.as_rsb(out, out, Imm8(0), SetCC, Assembler::Signed);
  if (mir->canBeNegativeDividend()) {
    if (!mir->isTruncated()) {
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Zero, ins->snapshot());
    } else {
      // -0|0 == 0
    }
  }
  masm.bind(&fin);
}

void CodeGenerator::visitModMaskI(LModMaskI* ins) {
  Register src = ToRegister(ins->getOperand(0));
  Register dest = ToRegister(ins->getDef(0));
  Register tmp1 = ToRegister(ins->getTemp(0));
  Register tmp2 = ToRegister(ins->getTemp(1));
  MMod* mir = ins->mir();

  ScratchRegisterScope scratch(masm);
  SecondScratchRegisterScope scratch2(masm);

  masm.ma_mod_mask(src, dest, tmp1, tmp2, scratch, scratch2, ins->shift());

  if (mir->canBeNegativeDividend()) {
    if (!mir->isTruncated()) {
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Zero, ins->snapshot());
    } else {
      // -0|0 == 0
    }
  }
}

void CodeGeneratorARM::emitBigIntDiv(LBigIntDiv* ins, Register dividend,
                                     Register divisor, Register output,
                                     Label* fail) {
  // Callers handle division by zero and integer overflow.

  if (HasIDIV()) {
    masm.ma_sdiv(dividend, divisor, /* result= */ dividend);

    // Create and return the result.
    masm.newGCBigInt(output, divisor, fail, bigIntsCanBeInNursery());
    masm.initializeBigInt(output, dividend);

    return;
  }

  // idivmod returns the quotient in r0, and the remainder in r1.
  MOZ_ASSERT(dividend == r0);
  MOZ_ASSERT(divisor == r1);

  LiveRegisterSet volatileRegs = liveVolatileRegs(ins);
  volatileRegs.takeUnchecked(dividend);
  volatileRegs.takeUnchecked(divisor);
  volatileRegs.takeUnchecked(output);

  masm.PushRegsInMask(volatileRegs);

  using Fn = int64_t (*)(int, int);
  masm.setupUnalignedABICall(output);
  masm.passABIArg(dividend);
  masm.passABIArg(divisor);
  masm.callWithABI<Fn, __aeabi_idivmod>(MoveOp::GENERAL,
                                        CheckUnsafeCallWithABI::DontCheckOther);

  masm.PopRegsInMask(volatileRegs);

  // Create and return the result.
  masm.newGCBigInt(output, divisor, fail, bigIntsCanBeInNursery());
  masm.initializeBigInt(output, dividend);
}

void CodeGeneratorARM::emitBigIntMod(LBigIntMod* ins, Register dividend,
                                     Register divisor, Register output,
                                     Label* fail) {
  // Callers handle division by zero and integer overflow.

  if (HasIDIV()) {
    {
      ScratchRegisterScope scratch(masm);
      masm.ma_smod(dividend, divisor, /* result= */ dividend, scratch);
    }

    // Create and return the result.
    masm.newGCBigInt(output, divisor, fail, bigIntsCanBeInNursery());
    masm.initializeBigInt(output, dividend);

    return;
  }

  // idivmod returns the quotient in r0, and the remainder in r1.
  MOZ_ASSERT(dividend == r0);
  MOZ_ASSERT(divisor == r1);

  LiveRegisterSet volatileRegs = liveVolatileRegs(ins);
  volatileRegs.takeUnchecked(dividend);
  volatileRegs.takeUnchecked(divisor);
  volatileRegs.takeUnchecked(output);

  masm.PushRegsInMask(volatileRegs);

  using Fn = int64_t (*)(int, int);
  masm.setupUnalignedABICall(output);
  masm.passABIArg(dividend);
  masm.passABIArg(divisor);
  masm.callWithABI<Fn, __aeabi_idivmod>(MoveOp::GENERAL,
                                        CheckUnsafeCallWithABI::DontCheckOther);

  masm.PopRegsInMask(volatileRegs);

  // Create and return the result.
  masm.newGCBigInt(output, dividend, fail, bigIntsCanBeInNursery());
  masm.initializeBigInt(output, divisor);
}

void CodeGenerator::visitBitNotI(LBitNotI* ins) {
  const LAllocation* input = ins->getOperand(0);
  const LDefinition* dest = ins->getDef(0);
  // This will not actually be true on arm. We can not an imm8m in order to
  // get a wider range of numbers
  MOZ_ASSERT(!input->isConstant());

  masm.ma_mvn(ToRegister(input), ToRegister(dest));
}

void CodeGenerator::visitBitOpI(LBitOpI* ins) {
  const LAllocation* lhs = ins->getOperand(0);
  const LAllocation* rhs = ins->getOperand(1);
  const LDefinition* dest = ins->getDef(0);

  ScratchRegisterScope scratch(masm);

  // All of these bitops should be either imm32's, or integer registers.
  switch (ins->bitop()) {
    case JSOp::BitOr:
      if (rhs->isConstant()) {
        masm.ma_orr(Imm32(ToInt32(rhs)), ToRegister(lhs), ToRegister(dest),
                    scratch);
      } else {
        masm.ma_orr(ToRegister(rhs), ToRegister(lhs), ToRegister(dest));
      }
      break;
    case JSOp::BitXor:
      if (rhs->isConstant()) {
        masm.ma_eor(Imm32(ToInt32(rhs)), ToRegister(lhs), ToRegister(dest),
                    scratch);
      } else {
        masm.ma_eor(ToRegister(rhs), ToRegister(lhs), ToRegister(dest));
      }
      break;
    case JSOp::BitAnd:
      if (rhs->isConstant()) {
        masm.ma_and(Imm32(ToInt32(rhs)), ToRegister(lhs), ToRegister(dest),
                    scratch);
      } else {
        masm.ma_and(ToRegister(rhs), ToRegister(lhs), ToRegister(dest));
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
          masm.ma_lsl(Imm32(shift), lhs, dest);
        } else {
          masm.ma_mov(lhs, dest);
        }
        break;
      case JSOp::Rsh:
        if (shift) {
          masm.ma_asr(Imm32(shift), lhs, dest);
        } else {
          masm.ma_mov(lhs, dest);
        }
        break;
      case JSOp::Ursh:
        if (shift) {
          masm.ma_lsr(Imm32(shift), lhs, dest);
        } else {
          // x >>> 0 can overflow.
          masm.ma_mov(lhs, dest);
          if (ins->mir()->toUrsh()->fallible()) {
            masm.as_cmp(dest, Imm8(0));
            bailoutIf(Assembler::LessThan, ins->snapshot());
          }
        }
        break;
      default:
        MOZ_CRASH("Unexpected shift op");
    }
  } else {
    // The shift amounts should be AND'ed into the 0-31 range since arm
    // shifts by the lower byte of the register (it will attempt to shift by
    // 250 if you ask it to).
    masm.as_and(dest, ToRegister(rhs), Imm8(0x1F));

    switch (ins->bitop()) {
      case JSOp::Lsh:
        masm.ma_lsl(dest, lhs, dest);
        break;
      case JSOp::Rsh:
        masm.ma_asr(dest, lhs, dest);
        break;
      case JSOp::Ursh:
        masm.ma_lsr(dest, lhs, dest);
        if (ins->mir()->toUrsh()->fallible()) {
          // x >>> 0 can overflow.
          masm.as_cmp(dest, Imm8(0));
          bailoutIf(Assembler::LessThan, ins->snapshot());
        }
        break;
      default:
        MOZ_CRASH("Unexpected shift op");
    }
  }
}

void CodeGenerator::visitUrshD(LUrshD* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register temp = ToRegister(ins->temp());

  const LAllocation* rhs = ins->rhs();
  FloatRegister out = ToFloatRegister(ins->output());

  if (rhs->isConstant()) {
    int32_t shift = ToInt32(rhs) & 0x1F;
    if (shift) {
      masm.ma_lsr(Imm32(shift), lhs, temp);
    } else {
      masm.ma_mov(lhs, temp);
    }
  } else {
    masm.as_and(temp, ToRegister(rhs), Imm8(0x1F));
    masm.ma_lsr(temp, lhs, temp);
  }

  masm.convertUInt32ToDouble(temp, out);
}

void CodeGenerator::visitClzI(LClzI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.clz32(input, output, /* knownNotZero = */ false);
}

void CodeGenerator::visitCtzI(LCtzI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.ctz32(input, output, /* knownNotZero = */ false);
}

void CodeGenerator::visitPopcntI(LPopcntI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  Register tmp = ToRegister(ins->temp());

  masm.popcnt32(input, output, tmp);
}

void CodeGenerator::visitPowHalfD(LPowHalfD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());
  ScratchDoubleScope scratch(masm);

  Label done;

  // Masm.pow(-Infinity, 0.5) == Infinity.
  masm.loadConstantDouble(NegativeInfinity<double>(), scratch);
  masm.compareDouble(input, scratch);
  masm.ma_vneg(scratch, output, Assembler::Equal);
  masm.ma_b(&done, Assembler::Equal);

  // Math.pow(-0, 0.5) == 0 == Math.pow(0, 0.5).
  // Adding 0 converts any -0 to 0.
  masm.loadConstantDouble(0.0, scratch);
  masm.ma_vadd(scratch, input, output);
  masm.ma_vsqrt(output, output);

  masm.bind(&done);
}

MoveOperand CodeGeneratorARM::toMoveOperand(LAllocation a) const {
  if (a.isGeneralReg()) {
    return MoveOperand(ToRegister(a));
  }
  if (a.isFloatReg()) {
    return MoveOperand(ToFloatRegister(a));
  }
  MoveOperand::Kind kind =
      a.isStackArea() ? MoveOperand::EFFECTIVE_ADDRESS : MoveOperand::MEMORY;
  Address addr = ToAddress(a);
  MOZ_ASSERT((addr.offset & 3) == 0);
  return MoveOperand(addr, kind);
}

class js::jit::OutOfLineTableSwitch
    : public OutOfLineCodeBase<CodeGeneratorARM> {
  MTableSwitch* mir_;
  Vector<CodeLabel, 8, JitAllocPolicy> codeLabels_;

  void accept(CodeGeneratorARM* codegen) override {
    codegen->visitOutOfLineTableSwitch(this);
  }

 public:
  OutOfLineTableSwitch(TempAllocator& alloc, MTableSwitch* mir)
      : mir_(mir), codeLabels_(alloc) {}

  MTableSwitch* mir() const { return mir_; }

  bool addCodeLabel(CodeLabel label) { return codeLabels_.append(label); }
  CodeLabel codeLabel(unsigned i) { return codeLabels_[i]; }
};

void CodeGeneratorARM::visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool) {
  MTableSwitch* mir = ool->mir();

  size_t numCases = mir->numCases();
  for (size_t i = 0; i < numCases; i++) {
    LBlock* caseblock =
        skipTrivialBlocks(mir->getCase(numCases - 1 - i))->lir();
    Label* caseheader = caseblock->label();
    uint32_t caseoffset = caseheader->offset();

    // The entries of the jump table need to be absolute addresses and thus
    // must be patched after codegen is finished.
    CodeLabel cl = ool->codeLabel(i);
    cl.target()->bind(caseoffset);
    masm.addCodeLabel(cl);
  }
}

void CodeGeneratorARM::emitTableSwitchDispatch(MTableSwitch* mir,
                                               Register index, Register base) {
  // The code generated by this is utter hax.
  // The end result looks something like:
  // SUBS index, input, #base
  // RSBSPL index, index, #max
  // LDRPL pc, pc, index lsl 2
  // B default

  // If the range of targets in N through M, we first subtract off the lowest
  // case (N), which both shifts the arguments into the range 0 to (M - N)
  // with and sets the MInus flag if the argument was out of range on the low
  // end.

  // Then we a reverse subtract with the size of the jump table, which will
  // reverse the order of range (It is size through 0, rather than 0 through
  // size). The main purpose of this is that we set the same flag as the lower
  // bound check for the upper bound check. Lastly, we do this conditionally
  // on the previous check succeeding.

  // Then we conditionally load the pc offset by the (reversed) index (times
  // the address size) into the pc, which branches to the correct case. NOTE:
  // when we go to read the pc, the value that we get back is the pc of the
  // current instruction *PLUS 8*. This means that ldr foo, [pc, +0] reads
  // $pc+8. In other words, there is an empty word after the branch into the
  // switch table before the table actually starts. Since the only other
  // unhandled case is the default case (both out of range high and out of
  // range low) I then insert a branch to default case into the extra slot,
  // which ensures we don't attempt to execute the address table.
  Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

  ScratchRegisterScope scratch(masm);

  int32_t cases = mir->numCases();
  // Lower value with low value.
  masm.ma_sub(index, Imm32(mir->low()), index, scratch, SetCC);
  masm.ma_rsb(index, Imm32(cases - 1), index, scratch, SetCC,
              Assembler::NotSigned);
  // Inhibit pools within the following sequence because we are indexing into
  // a pc relative table. The region will have one instruction for ma_ldr, one
  // for ma_b, and each table case takes one word.
  AutoForbidPoolsAndNops afp(&masm, 1 + 1 + cases);
  masm.ma_ldr(DTRAddr(pc, DtrRegImmShift(index, LSL, 2)), pc, Offset,
              Assembler::NotSigned);
  masm.ma_b(defaultcase);

  // To fill in the CodeLabels for the case entries, we need to first generate
  // the case entries (we don't yet know their offsets in the instruction
  // stream).
  OutOfLineTableSwitch* ool = new (alloc()) OutOfLineTableSwitch(alloc(), mir);
  for (int32_t i = 0; i < cases; i++) {
    CodeLabel cl;
    masm.writeCodePointer(&cl);
    masm.propagateOOM(ool->addCodeLabel(cl));
  }
  addOutOfLineCode(ool, mir);
}

void CodeGenerator::visitMathD(LMathD* math) {
  FloatRegister src1 = ToFloatRegister(math->getOperand(0));
  FloatRegister src2 = ToFloatRegister(math->getOperand(1));
  FloatRegister output = ToFloatRegister(math->getDef(0));

  switch (math->jsop()) {
    case JSOp::Add:
      masm.ma_vadd(src1, src2, output);
      break;
    case JSOp::Sub:
      masm.ma_vsub(src1, src2, output);
      break;
    case JSOp::Mul:
      masm.ma_vmul(src1, src2, output);
      break;
    case JSOp::Div:
      masm.ma_vdiv(src1, src2, output);
      break;
    default:
      MOZ_CRASH("unexpected opcode");
  }
}

void CodeGenerator::visitMathF(LMathF* math) {
  FloatRegister src1 = ToFloatRegister(math->getOperand(0));
  FloatRegister src2 = ToFloatRegister(math->getOperand(1));
  FloatRegister output = ToFloatRegister(math->getDef(0));

  switch (math->jsop()) {
    case JSOp::Add:
      masm.ma_vadd_f32(src1, src2, output);
      break;
    case JSOp::Sub:
      masm.ma_vsub_f32(src1, src2, output);
      break;
    case JSOp::Mul:
      masm.ma_vmul_f32(src1, src2, output);
      break;
    case JSOp::Div:
      masm.ma_vdiv_f32(src1, src2, output);
      break;
    default:
      MOZ_CRASH("unexpected opcode");
  }
}

void CodeGenerator::visitTruncateDToInt32(LTruncateDToInt32* ins) {
  emitTruncateDouble(ToFloatRegister(ins->input()), ToRegister(ins->output()),
                     ins->mir());
}

void CodeGenerator::visitWasmBuiltinTruncateDToInt32(
    LWasmBuiltinTruncateDToInt32* ins) {
  emitTruncateDouble(ToFloatRegister(ins->getOperand(0)),
                     ToRegister(ins->getDef(0)), ins->mir());
}

void CodeGenerator::visitTruncateFToInt32(LTruncateFToInt32* ins) {
  emitTruncateFloat32(ToFloatRegister(ins->input()), ToRegister(ins->output()),
                      ins->mir());
}

void CodeGenerator::visitWasmBuiltinTruncateFToInt32(
    LWasmBuiltinTruncateFToInt32* ins) {
  emitTruncateFloat32(ToFloatRegister(ins->getOperand(0)),
                      ToRegister(ins->getDef(0)), ins->mir());
}

static const uint32_t FrameSizes[] = {128, 256, 512, 1024};

FrameSizeClass FrameSizeClass::FromDepth(uint32_t frameDepth) {
  for (uint32_t i = 0; i < std::size(FrameSizes); i++) {
    if (frameDepth < FrameSizes[i]) {
      return FrameSizeClass(i);
    }
  }

  return FrameSizeClass::None();
}

FrameSizeClass FrameSizeClass::ClassLimit() {
  return FrameSizeClass(std::size(FrameSizes));
}

uint32_t FrameSizeClass::frameSize() const {
  MOZ_ASSERT(class_ != NO_FRAME_SIZE_CLASS_ID);
  MOZ_ASSERT(class_ < std::size(FrameSizes));

  return FrameSizes[class_];
}

ValueOperand CodeGeneratorARM::ToValue(LInstruction* ins, size_t pos) {
  Register typeReg = ToRegister(ins->getOperand(pos + TYPE_INDEX));
  Register payloadReg = ToRegister(ins->getOperand(pos + PAYLOAD_INDEX));
  return ValueOperand(typeReg, payloadReg);
}

ValueOperand CodeGeneratorARM::ToTempValue(LInstruction* ins, size_t pos) {
  Register typeReg = ToRegister(ins->getTemp(pos + TYPE_INDEX));
  Register payloadReg = ToRegister(ins->getTemp(pos + PAYLOAD_INDEX));
  return ValueOperand(typeReg, payloadReg);
}

void CodeGenerator::visitValue(LValue* value) {
  const ValueOperand out = ToOutValue(value);

  masm.moveValue(value->value(), out);
}

void CodeGenerator::visitBox(LBox* box) {
  const LDefinition* type = box->getDef(TYPE_INDEX);

  MOZ_ASSERT(!box->getOperand(0)->isConstant());

  // On arm, the input operand and the output payload have the same virtual
  // register. All that needs to be written is the type tag for the type
  // definition.
  masm.ma_mov(Imm32(MIRTypeToTag(box->type())), ToRegister(type));
}

void CodeGenerator::visitBoxFloatingPoint(LBoxFloatingPoint* box) {
  const AnyRegister in = ToAnyRegister(box->getOperand(0));
  const ValueOperand out = ToOutValue(box);

  masm.moveValue(TypedOrValueRegister(box->type(), in), out);
}

void CodeGenerator::visitUnbox(LUnbox* unbox) {
  // Note that for unbox, the type and payload indexes are switched on the
  // inputs.
  MUnbox* mir = unbox->mir();
  Register type = ToRegister(unbox->type());
  Register payload = ToRegister(unbox->payload());
  Register output = ToRegister(unbox->output());

  mozilla::Maybe<ScratchRegisterScope> scratch;
  scratch.emplace(masm);

  JSValueTag tag = MIRTypeToTag(mir->type());
  if (mir->fallible()) {
    masm.ma_cmp(type, Imm32(tag), *scratch);
    bailoutIf(Assembler::NotEqual, unbox->snapshot());
  } else {
#ifdef DEBUG
    Label ok;
    masm.ma_cmp(type, Imm32(tag), *scratch);
    masm.ma_b(&ok, Assembler::Equal);
    scratch.reset();
    masm.assumeUnreachable("Infallible unbox type mismatch");
    masm.bind(&ok);
#endif
  }

  // Note: If spectreValueMasking is disabled, then this instruction will
  // default to a no-op as long as the lowering allocate the same register for
  // the output and the payload.
  masm.unboxNonDouble(ValueOperand(type, payload), output,
                      ValueTypeFromMIRType(mir->type()));
}

void CodeGenerator::visitDouble(LDouble* ins) {
  const LDefinition* out = ins->getDef(0);
  masm.loadConstantDouble(ins->getDouble(), ToFloatRegister(out));
}

void CodeGenerator::visitFloat32(LFloat32* ins) {
  const LDefinition* out = ins->getDef(0);
  masm.loadConstantFloat32(ins->getFloat(), ToFloatRegister(out));
}

void CodeGeneratorARM::splitTagForTest(const ValueOperand& value,
                                       ScratchTagScope& tag) {
  MOZ_ASSERT(value.typeReg() == tag);
}

void CodeGenerator::visitTestDAndBranch(LTestDAndBranch* test) {
  const LAllocation* opd = test->input();
  masm.ma_vcmpz(ToFloatRegister(opd));
  masm.as_vmrs(pc);

  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();
  // If the compare set the 0 bit, then the result is definitely false.
  jumpToBlock(ifFalse, Assembler::Zero);
  // It is also false if one of the operands is NAN, which is shown as
  // Overflow.
  jumpToBlock(ifFalse, Assembler::Overflow);
  jumpToBlock(ifTrue);
}

void CodeGenerator::visitTestFAndBranch(LTestFAndBranch* test) {
  const LAllocation* opd = test->input();
  masm.ma_vcmpz_f32(ToFloatRegister(opd));
  masm.as_vmrs(pc);

  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();
  // If the compare set the 0 bit, then the result is definitely false.
  jumpToBlock(ifFalse, Assembler::Zero);
  // It is also false if one of the operands is NAN, which is shown as
  // Overflow.
  jumpToBlock(ifFalse, Assembler::Overflow);
  jumpToBlock(ifTrue);
}

void CodeGenerator::visitCompareD(LCompareD* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());
  masm.compareDouble(lhs, rhs);
  masm.emitSet(Assembler::ConditionFromDoubleCondition(cond),
               ToRegister(comp->output()));
}

void CodeGenerator::visitCompareF(LCompareF* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());
  masm.compareFloat(lhs, rhs);
  masm.emitSet(Assembler::ConditionFromDoubleCondition(cond),
               ToRegister(comp->output()));
}

void CodeGenerator::visitCompareDAndBranch(LCompareDAndBranch* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond =
      JSOpToDoubleCondition(comp->cmpMir()->jsop());
  masm.compareDouble(lhs, rhs);
  emitBranch(Assembler::ConditionFromDoubleCondition(cond), comp->ifTrue(),
             comp->ifFalse());
}

void CodeGenerator::visitCompareFAndBranch(LCompareFAndBranch* comp) {
  FloatRegister lhs = ToFloatRegister(comp->left());
  FloatRegister rhs = ToFloatRegister(comp->right());

  Assembler::DoubleCondition cond =
      JSOpToDoubleCondition(comp->cmpMir()->jsop());
  masm.compareFloat(lhs, rhs);
  emitBranch(Assembler::ConditionFromDoubleCondition(cond), comp->ifTrue(),
             comp->ifFalse());
}

void CodeGenerator::visitBitAndAndBranch(LBitAndAndBranch* baab) {
  ScratchRegisterScope scratch(masm);
  if (baab->right()->isConstant()) {
    masm.ma_tst(ToRegister(baab->left()), Imm32(ToInt32(baab->right())),
                scratch);
  } else {
    masm.ma_tst(ToRegister(baab->left()), ToRegister(baab->right()));
  }
  emitBranch(baab->cond(), baab->ifTrue(), baab->ifFalse());
}

// See ../CodeGenerator.cpp for more information.
void CodeGenerator::visitWasmRegisterResult(LWasmRegisterResult* lir) {}

void CodeGenerator::visitWasmUint32ToDouble(LWasmUint32ToDouble* lir) {
  masm.convertUInt32ToDouble(ToRegister(lir->input()),
                             ToFloatRegister(lir->output()));
}

void CodeGenerator::visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir) {
  masm.convertUInt32ToFloat32(ToRegister(lir->input()),
                              ToFloatRegister(lir->output()));
}

void CodeGenerator::visitNotI(LNotI* ins) {
  // It is hard to optimize !x, so just do it the basic way for now.
  masm.as_cmp(ToRegister(ins->input()), Imm8(0));
  masm.emitSet(Assembler::Equal, ToRegister(ins->output()));
}

void CodeGenerator::visitNotI64(LNotI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register output = ToRegister(lir->output());

  masm.ma_orr(input.low, input.high, output);
  masm.as_cmp(output, Imm8(0));
  masm.emitSet(Assembler::Equal, output);
}

void CodeGenerator::visitNotD(LNotD* ins) {
  // Since this operation is not, we want to set a bit if the double is
  // falsey, which means 0.0, -0.0 or NaN. When comparing with 0, an input of
  // 0 will set the Z bit (30) and NaN will set the V bit (28) of the APSR.
  FloatRegister opd = ToFloatRegister(ins->input());
  Register dest = ToRegister(ins->output());

  // Do the compare.
  masm.ma_vcmpz(opd);
  // TODO There are three variations here to compare performance-wise.
  bool nocond = true;
  if (nocond) {
    // Load the value into the dest register.
    masm.as_vmrs(dest);
    masm.ma_lsr(Imm32(28), dest, dest);
    // 28 + 2 = 30
    masm.ma_alu(dest, lsr(dest, 2), dest, OpOrr);
    masm.as_and(dest, dest, Imm8(1));
  } else {
    masm.as_vmrs(pc);
    masm.ma_mov(Imm32(0), dest);
    masm.ma_mov(Imm32(1), dest, Assembler::Equal);
    masm.ma_mov(Imm32(1), dest, Assembler::Overflow);
  }
}

void CodeGenerator::visitNotF(LNotF* ins) {
  // Since this operation is not, we want to set a bit if the double is
  // falsey, which means 0.0, -0.0 or NaN. When comparing with 0, an input of
  // 0 will set the Z bit (30) and NaN will set the V bit (28) of the APSR.
  FloatRegister opd = ToFloatRegister(ins->input());
  Register dest = ToRegister(ins->output());

  // Do the compare.
  masm.ma_vcmpz_f32(opd);
  // TODO There are three variations here to compare performance-wise.
  bool nocond = true;
  if (nocond) {
    // Load the value into the dest register.
    masm.as_vmrs(dest);
    masm.ma_lsr(Imm32(28), dest, dest);
    // 28 + 2 = 30
    masm.ma_alu(dest, lsr(dest, 2), dest, OpOrr);
    masm.as_and(dest, dest, Imm8(1));
  } else {
    masm.as_vmrs(pc);
    masm.ma_mov(Imm32(0), dest);
    masm.ma_mov(Imm32(1), dest, Assembler::Equal);
    masm.ma_mov(Imm32(1), dest, Assembler::Overflow);
  }
}

void CodeGeneratorARM::generateInvalidateEpilogue() {
  // Ensure that there is enough space in the buffer for the OsiPoint patching
  // to occur. Otherwise, we could overwrite the invalidation epilogue.
  for (size_t i = 0; i < sizeof(void*); i += Assembler::NopSize()) {
    masm.nop();
  }

  masm.bind(&invalidate_);

  // Push the return address of the point that we bailed out at onto the stack.
  masm.Push(lr);

  // Push the Ion script onto the stack (when we determine what that pointer
  // is).
  invalidateEpilogueData_ = masm.pushWithPatch(ImmWord(uintptr_t(-1)));

  // Jump to the invalidator which will replace the current frame.
  TrampolinePtr thunk = gen->jitRuntime()->getInvalidationThunk();
  masm.jump(thunk);
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

void CodeGenerator::visitAtomicTypedArrayElementBinop(
    LAtomicTypedArrayElementBinop* lir) {
  MOZ_ASSERT(!lir->mir()->isForEffect());

  AnyRegister output = ToAnyRegister(lir->output());
  Register elements = ToRegister(lir->elements());
  Register flagTemp = ToRegister(lir->temp1());
  Register outTemp =
      lir->temp2()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp2());
  Register value = ToRegister(lir->value());

  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address mem = ToAddress(elements, lir->index(), arrayType);
    masm.atomicFetchOpJS(arrayType, Synchronization::Full(),
                         lir->mir()->operation(), value, mem, flagTemp, outTemp,
                         output);
  } else {
    BaseIndex mem(elements, ToRegister(lir->index()),
                  ScaleFromScalarType(arrayType));
    masm.atomicFetchOpJS(arrayType, Synchronization::Full(),
                         lir->mir()->operation(), value, mem, flagTemp, outTemp,
                         output);
  }
}

void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect(
    LAtomicTypedArrayElementBinopForEffect* lir) {
  MOZ_ASSERT(lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register flagTemp = ToRegister(lir->flagTemp());
  Register value = ToRegister(lir->value());
  Scalar::Type arrayType = lir->mir()->arrayType();

  if (lir->index()->isConstant()) {
    Address mem = ToAddress(elements, lir->index(), arrayType);
    masm.atomicEffectOpJS(arrayType, Synchronization::Full(),
                          lir->mir()->operation(), value, mem, flagTemp);
  } else {
    BaseIndex mem(elements, ToRegister(lir->index()),
                  ScaleFromScalarType(arrayType));
    masm.atomicEffectOpJS(arrayType, Synchronization::Full(),
                          lir->mir()->operation(), value, mem, flagTemp);
  }
}

void CodeGenerator::visitAtomicLoad64(LAtomicLoad64* lir) {
  Register elements = ToRegister(lir->elements());
  Register temp = ToRegister(lir->temp());
  Register64 temp64 = ToRegister64(lir->temp64());
  Register out = ToRegister(lir->output());

  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

  if (lir->index()->isConstant()) {
    Address source =
        ToAddress(elements, lir->index(), storageType, mir->offsetAdjustment());
    masm.atomicLoad64(Synchronization::Load(), source, temp64);
  } else {
    BaseIndex source(elements, ToRegister(lir->index()),
                     ScaleFromScalarType(storageType), mir->offsetAdjustment());
    masm.atomicLoad64(Synchronization::Load(), source, temp64);
  }

  emitCreateBigInt(lir, storageType, temp64, out, temp);
}

void CodeGenerator::visitAtomicStore64(LAtomicStore64* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = ToRegister64(lir->temp2());

  Scalar::Type writeType = lir->mir()->writeType();

  masm.loadBigInt64(value, temp1);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), writeType);
    masm.atomicStore64(Synchronization::Store(), dest, temp1, temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(writeType));
    masm.atomicStore64(Synchronization::Store(), dest, temp1, temp2);
  }
}

void CodeGenerator::visitCompareExchangeTypedArrayElement64(
    LCompareExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register oldval = ToRegister(lir->oldval());
  Register newval = ToRegister(lir->newval());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = ToRegister64(lir->temp2());
  Register64 temp3 = ToRegister64(lir->temp3());
  Register out = ToRegister(lir->output());

  Scalar::Type arrayType = lir->mir()->arrayType();

  masm.loadBigInt64(oldval, temp1);
  masm.loadBigInt64(newval, temp2);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.compareExchange64(Synchronization::Full(), dest, temp1, temp2, temp3);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.compareExchange64(Synchronization::Full(), dest, temp1, temp2, temp3);
  }

  emitCreateBigInt(lir, arrayType, temp3, out, temp1.scratchReg());
}

void CodeGenerator::visitAtomicExchangeTypedArrayElement64(
    LAtomicExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register temp2 = ToRegister(lir->temp2());
  Register out = ToRegister(lir->output());
  Register64 temp64 = Register64(temp2, out);

  Scalar::Type arrayType = lir->mir()->arrayType();

  masm.loadBigInt64(value, temp64);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicExchange64(Synchronization::Full(), dest, temp64, temp1);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicExchange64(Synchronization::Full(), dest, temp64, temp1);
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
  Register64 temp3 = ToRegister64(lir->temp3());
  Register out = ToRegister(lir->output());

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  masm.loadBigInt64(value, temp1);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, temp1, dest, temp2,
                         temp3);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, temp1, dest, temp2,
                         temp3);
  }

  emitCreateBigInt(lir, arrayType, temp3, out, temp2.scratchReg());
}

void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect64(
    LAtomicTypedArrayElementBinopForEffect64* lir) {
  MOZ_ASSERT(lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = ToRegister64(lir->temp2());

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  masm.loadBigInt64(value, temp1);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicEffectOp64(Synchronization::Full(), atomicOp, temp1, dest,
                          temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicEffectOp64(Synchronization::Full(), atomicOp, temp1, dest,
                          temp2);
  }
}

void CodeGenerator::visitWasmSelect(LWasmSelect* ins) {
  MIRType mirType = ins->mir()->type();

  Register cond = ToRegister(ins->condExpr());
  masm.as_cmp(cond, Imm8(0));

  if (mirType == MIRType::Int32 || mirType == MIRType::RefOrNull) {
    Register falseExpr = ToRegister(ins->falseExpr());
    Register out = ToRegister(ins->output());
    MOZ_ASSERT(ToRegister(ins->trueExpr()) == out,
               "true expr input is reused for output");
    masm.ma_mov(falseExpr, out, LeaveCC, Assembler::Zero);
    return;
  }

  FloatRegister out = ToFloatRegister(ins->output());
  MOZ_ASSERT(ToFloatRegister(ins->trueExpr()) == out,
             "true expr input is reused for output");

  FloatRegister falseExpr = ToFloatRegister(ins->falseExpr());

  if (mirType == MIRType::Double) {
    masm.moveDouble(falseExpr, out, Assembler::Zero);
  } else if (mirType == MIRType::Float32) {
    masm.moveFloat32(falseExpr, out, Assembler::Zero);
  } else {
    MOZ_CRASH("unhandled type in visitWasmSelect!");
  }
}

void CodeGenerator::visitWasmCompareAndSelect(LWasmCompareAndSelect* ins) {
  emitWasmCompareAndSelect(ins);
}

void CodeGenerator::visitWasmReinterpret(LWasmReinterpret* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  MWasmReinterpret* ins = lir->mir();

  MIRType to = ins->type();
  DebugOnly<MIRType> from = ins->input()->type();

  switch (to) {
    case MIRType::Int32:
      MOZ_ASSERT(static_cast<MIRType>(from) == MIRType::Float32);
      masm.ma_vxfer(ToFloatRegister(lir->input()), ToRegister(lir->output()));
      break;
    case MIRType::Float32:
      MOZ_ASSERT(static_cast<MIRType>(from) == MIRType::Int32);
      masm.ma_vxfer(ToRegister(lir->input()), ToFloatRegister(lir->output()));
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

  const LAllocation* ptr = ins->ptr();
  const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();

  bool isSigned;
  int size;
  bool isFloat = false;
  switch (mir->accessType()) {
    case Scalar::Int8:
      isSigned = true;
      size = 8;
      break;
    case Scalar::Uint8:
      isSigned = false;
      size = 8;
      break;
    case Scalar::Int16:
      isSigned = true;
      size = 16;
      break;
    case Scalar::Uint16:
      isSigned = false;
      size = 16;
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      isSigned = true;
      size = 32;
      break;
    case Scalar::Float64:
      isFloat = true;
      size = 64;
      break;
    case Scalar::Float32:
      isFloat = true;
      size = 32;
      break;
    default:
      MOZ_CRASH("unexpected array type");
  }

  if (ptr->isConstant()) {
    MOZ_ASSERT(!mir->needsBoundsCheck());
    int32_t ptrImm = ptr->toConstant()->toInt32();
    MOZ_ASSERT(ptrImm >= 0);
    if (isFloat) {
      ScratchRegisterScope scratch(masm);
      VFPRegister vd(ToFloatRegister(ins->output()));
      if (size == 32) {
        masm.ma_vldr(Address(HeapReg, ptrImm), vd.singleOverlay(), scratch,
                     Assembler::Always);
      } else {
        masm.ma_vldr(Address(HeapReg, ptrImm), vd, scratch, Assembler::Always);
      }
    } else {
      ScratchRegisterScope scratch(masm);
      masm.ma_dataTransferN(IsLoad, size, isSigned, HeapReg, Imm32(ptrImm),
                            ToRegister(ins->output()), scratch, Offset,
                            Assembler::Always);
    }
  } else {
    Register ptrReg = ToRegister(ptr);
    if (isFloat) {
      FloatRegister output = ToFloatRegister(ins->output());
      if (size == 32) {
        output = output.singleOverlay();
      }

      Assembler::Condition cond = Assembler::Always;
      if (mir->needsBoundsCheck()) {
        Register boundsCheckLimitReg = ToRegister(boundsCheckLimit);
        masm.as_cmp(ptrReg, O2Reg(boundsCheckLimitReg));
        if (size == 32) {
          masm.ma_vimm_f32(GenericNaN(), output, Assembler::AboveOrEqual);
        } else {
          masm.ma_vimm(GenericNaN(), output, Assembler::AboveOrEqual);
        }
        cond = Assembler::Below;
      }

      ScratchRegisterScope scratch(masm);
      masm.ma_vldr(output, HeapReg, ptrReg, scratch, 0, cond);
    } else {
      Register output = ToRegister(ins->output());

      Assembler::Condition cond = Assembler::Always;
      if (mir->needsBoundsCheck()) {
        Register boundsCheckLimitReg = ToRegister(boundsCheckLimit);
        masm.as_cmp(ptrReg, O2Reg(boundsCheckLimitReg));
        masm.ma_mov(Imm32(0), output, Assembler::AboveOrEqual);
        cond = Assembler::Below;
      }

      ScratchRegisterScope scratch(masm);
      masm.ma_dataTransferN(IsLoad, size, isSigned, HeapReg, ptrReg, output,
                            scratch, Offset, cond);
    }
  }
}

void CodeGenerator::visitWasmHeapBase(LWasmHeapBase* ins) {
  MOZ_ASSERT(ins->tlsPtr()->isBogus());
  masm.movePtr(HeapReg, ToRegister(ins->output()));
}

template <typename T>
void CodeGeneratorARM::emitWasmLoad(T* lir) {
  const MWasmLoad* mir = lir->mir();
  MIRType resultType = mir->type();
  Register ptr;

  if (mir->access().offset() || mir->access().type() == Scalar::Int64) {
    ptr = ToRegister(lir->ptrCopy());
  } else {
    MOZ_ASSERT(lir->ptrCopy()->isBogusTemp());
    ptr = ToRegister(lir->ptr());
  }

  if (resultType == MIRType::Int64) {
    masm.wasmLoadI64(mir->access(), HeapReg, ptr, ptr, ToOutRegister64(lir));
  } else {
    masm.wasmLoad(mir->access(), HeapReg, ptr, ptr,
                  ToAnyRegister(lir->output()));
  }
}

void CodeGenerator::visitWasmLoad(LWasmLoad* lir) { emitWasmLoad(lir); }

void CodeGenerator::visitWasmLoadI64(LWasmLoadI64* lir) { emitWasmLoad(lir); }

void CodeGenerator::visitWasmAddOffset(LWasmAddOffset* lir) {
  MWasmAddOffset* mir = lir->mir();
  Register base = ToRegister(lir->base());
  Register out = ToRegister(lir->output());

  ScratchRegisterScope scratch(masm);
  masm.ma_add(base, Imm32(mir->offset()), out, scratch, SetCC);

  Label ok;
  masm.ma_b(&ok, Assembler::CarryClear);
  masm.wasmTrap(wasm::Trap::OutOfBounds, mir->bytecodeOffset());
  masm.bind(&ok);
}

template <typename T>
void CodeGeneratorARM::emitWasmStore(T* lir) {
  const MWasmStore* mir = lir->mir();
  Scalar::Type accessType = mir->access().type();
  Register ptr;

  // Maybe add the offset.
  if (mir->access().offset() || accessType == Scalar::Int64) {
    ptr = ToRegister(lir->ptrCopy());
  } else {
    MOZ_ASSERT(lir->ptrCopy()->isBogusTemp());
    ptr = ToRegister(lir->ptr());
  }

  if (accessType == Scalar::Int64) {
    masm.wasmStoreI64(mir->access(),
                      ToRegister64(lir->getInt64Operand(lir->ValueIndex)),
                      HeapReg, ptr, ptr);
  } else {
    masm.wasmStore(mir->access(),
                   ToAnyRegister(lir->getOperand(lir->ValueIndex)), HeapReg,
                   ptr, ptr);
  }
}

void CodeGenerator::visitWasmStore(LWasmStore* lir) { emitWasmStore(lir); }

void CodeGenerator::visitWasmStoreI64(LWasmStoreI64* lir) {
  emitWasmStore(lir);
}

void CodeGenerator::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins) {
  const MAsmJSStoreHeap* mir = ins->mir();

  const LAllocation* ptr = ins->ptr();
  const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();

  bool isSigned;
  int size;
  bool isFloat = false;
  switch (mir->accessType()) {
    case Scalar::Int8:
    case Scalar::Uint8:
      isSigned = false;
      size = 8;
      break;
    case Scalar::Int16:
    case Scalar::Uint16:
      isSigned = false;
      size = 16;
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      isSigned = true;
      size = 32;
      break;
    case Scalar::Float64:
      isFloat = true;
      size = 64;
      break;
    case Scalar::Float32:
      isFloat = true;
      size = 32;
      break;
    default:
      MOZ_CRASH("unexpected array type");
  }

  if (ptr->isConstant()) {
    MOZ_ASSERT(!mir->needsBoundsCheck());
    int32_t ptrImm = ptr->toConstant()->toInt32();
    MOZ_ASSERT(ptrImm >= 0);
    if (isFloat) {
      VFPRegister vd(ToFloatRegister(ins->value()));
      Address addr(HeapReg, ptrImm);
      if (size == 32) {
        masm.storeFloat32(vd, addr);
      } else {
        masm.storeDouble(vd, addr);
      }
    } else {
      ScratchRegisterScope scratch(masm);
      masm.ma_dataTransferN(IsStore, size, isSigned, HeapReg, Imm32(ptrImm),
                            ToRegister(ins->value()), scratch, Offset,
                            Assembler::Always);
    }
  } else {
    Register ptrReg = ToRegister(ptr);

    Assembler::Condition cond = Assembler::Always;
    if (mir->needsBoundsCheck()) {
      Register boundsCheckLimitReg = ToRegister(boundsCheckLimit);
      masm.as_cmp(ptrReg, O2Reg(boundsCheckLimitReg));
      cond = Assembler::Below;
    }

    if (isFloat) {
      ScratchRegisterScope scratch(masm);
      FloatRegister value = ToFloatRegister(ins->value());
      if (size == 32) {
        value = value.singleOverlay();
      }

      masm.ma_vstr(value, HeapReg, ptrReg, scratch, 0, Assembler::Below);
    } else {
      ScratchRegisterScope scratch(masm);
      Register value = ToRegister(ins->value());
      masm.ma_dataTransferN(IsStore, size, isSigned, HeapReg, ptrReg, value,
                            scratch, Offset, cond);
    }
  }
}

void CodeGenerator::visitWasmCompareExchangeHeap(
    LWasmCompareExchangeHeap* ins) {
  MWasmCompareExchangeHeap* mir = ins->mir();

  const LAllocation* ptr = ins->ptr();
  Register ptrReg = ToRegister(ptr);
  BaseIndex srcAddr(HeapReg, ptrReg, TimesOne, mir->access().offset());

  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  Register oldval = ToRegister(ins->oldValue());
  Register newval = ToRegister(ins->newValue());
  Register out = ToRegister(ins->output());

  masm.wasmCompareExchange(mir->access(), srcAddr, oldval, newval, out);
}

void CodeGenerator::visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins) {
  MWasmAtomicExchangeHeap* mir = ins->mir();

  Register ptrReg = ToRegister(ins->ptr());
  Register value = ToRegister(ins->value());
  Register output = ToRegister(ins->output());
  BaseIndex srcAddr(HeapReg, ptrReg, TimesOne, mir->access().offset());
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  masm.wasmAtomicExchange(mir->access(), srcAddr, value, output);
}

void CodeGenerator::visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins) {
  MWasmAtomicBinopHeap* mir = ins->mir();
  MOZ_ASSERT(mir->hasUses());

  Register ptrReg = ToRegister(ins->ptr());
  Register flagTemp = ToRegister(ins->flagTemp());
  Register output = ToRegister(ins->output());
  const LAllocation* value = ins->value();
  AtomicOp op = mir->operation();
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  BaseIndex srcAddr(HeapReg, ptrReg, TimesOne, mir->access().offset());
  masm.wasmAtomicFetchOp(mir->access(), op, ToRegister(value), srcAddr,
                         flagTemp, output);
}

void CodeGenerator::visitWasmAtomicBinopHeapForEffect(
    LWasmAtomicBinopHeapForEffect* ins) {
  MWasmAtomicBinopHeap* mir = ins->mir();
  MOZ_ASSERT(!mir->hasUses());

  Register ptrReg = ToRegister(ins->ptr());
  Register flagTemp = ToRegister(ins->flagTemp());
  const LAllocation* value = ins->value();
  AtomicOp op = mir->operation();
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  BaseIndex srcAddr(HeapReg, ptrReg, TimesOne, mir->access().offset());
  masm.wasmAtomicEffectOp(mir->access(), op, ToRegister(value), srcAddr,
                          flagTemp);
}

void CodeGenerator::visitWasmStackArg(LWasmStackArg* ins) {
  const MWasmStackArg* mir = ins->mir();
  Address dst(StackPointer, mir->spOffset());
  ScratchRegisterScope scratch(masm);
  SecondScratchRegisterScope scratch2(masm);

  if (ins->arg()->isConstant()) {
    masm.ma_mov(Imm32(ToInt32(ins->arg())), scratch);
    masm.ma_str(scratch, dst, scratch2);
  } else {
    if (ins->arg()->isGeneralReg()) {
      masm.ma_str(ToRegister(ins->arg()), dst, scratch);
    } else {
      masm.ma_vstr(ToFloatRegister(ins->arg()), dst, scratch);
    }
  }
}

void CodeGenerator::visitUDiv(LUDiv* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());

  Label done;
  generateUDivModZeroCheck(rhs, output, &done, ins->snapshot(), ins->mir());

  masm.ma_udiv(lhs, rhs, output);

  // Check for large unsigned result - represent as double.
  if (!ins->mir()->isTruncated()) {
    MOZ_ASSERT(ins->mir()->fallible());
    masm.as_cmp(output, Imm8(0));
    bailoutIf(Assembler::LessThan, ins->snapshot());
  }

  // Check for non-zero remainder if not truncating to int.
  if (!ins->mir()->canTruncateRemainder()) {
    MOZ_ASSERT(ins->mir()->fallible());
    {
      ScratchRegisterScope scratch(masm);
      masm.ma_mul(rhs, output, scratch);
      masm.ma_cmp(scratch, lhs);
    }
    bailoutIf(Assembler::NotEqual, ins->snapshot());
  }

  if (done.used()) {
    masm.bind(&done);
  }
}

void CodeGenerator::visitUMod(LUMod* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());

  Label done;
  generateUDivModZeroCheck(rhs, output, &done, ins->snapshot(), ins->mir());

  {
    ScratchRegisterScope scratch(masm);
    masm.ma_umod(lhs, rhs, output, scratch);
  }

  // Check for large unsigned result - represent as double.
  if (!ins->mir()->isTruncated()) {
    MOZ_ASSERT(ins->mir()->fallible());
    masm.as_cmp(output, Imm8(0));
    bailoutIf(Assembler::LessThan, ins->snapshot());
  }

  if (done.used()) {
    masm.bind(&done);
  }
}

template <class T>
void CodeGeneratorARM::generateUDivModZeroCheck(Register rhs, Register output,
                                                Label* done,
                                                LSnapshot* snapshot, T* mir) {
  if (!mir) {
    return;
  }
  if (mir->canBeDivideByZero()) {
    masm.as_cmp(rhs, Imm8(0));
    if (mir->isTruncated()) {
      if (mir->trapOnError()) {
        Label nonZero;
        masm.ma_b(&nonZero, Assembler::NotEqual);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
        masm.bind(&nonZero);
      } else {
        Label skip;
        masm.ma_b(&skip, Assembler::NotEqual);
        // Infinity|0 == 0
        masm.ma_mov(Imm32(0), output);
        masm.ma_b(done);
        masm.bind(&skip);
      }
    } else {
      // Bailout for divide by zero
      MOZ_ASSERT(mir->fallible());
      bailoutIf(Assembler::Equal, snapshot);
    }
  }
}

void CodeGenerator::visitSoftUDivOrMod(LSoftUDivOrMod* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register output = ToRegister(ins->output());

  MOZ_ASSERT(lhs == r0);
  MOZ_ASSERT(rhs == r1);
  MOZ_ASSERT(output == r0);

  Label done;
  MDiv* div = ins->mir()->isDiv() ? ins->mir()->toDiv() : nullptr;
  MMod* mod = !div ? ins->mir()->toMod() : nullptr;

  generateUDivModZeroCheck(rhs, output, &done, ins->snapshot(), div);
  generateUDivModZeroCheck(rhs, output, &done, ins->snapshot(), mod);

  if (gen->compilingWasm()) {
    masm.Push(WasmTlsReg);
    int32_t framePushedAfterTls = masm.framePushed();
    masm.setupWasmABICall();
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
    wasm::BytecodeOffset bytecodeOffset =
        (div ? div->bytecodeOffset() : mod->bytecodeOffset());
    int32_t tlsOffset = masm.framePushed() - framePushedAfterTls;
    masm.callWithABI(bytecodeOffset, wasm::SymbolicAddress::aeabi_uidivmod,
                     mozilla::Some(tlsOffset));
    masm.Pop(WasmTlsReg);
  } else {
    using Fn = int64_t (*)(int, int);
    masm.setupAlignedABICall();
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
    masm.callWithABI<Fn, __aeabi_uidivmod>(
        MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);
  }

  if (mod) {
    MOZ_ASSERT(output == r0, "output should not be r1 for mod");
    masm.move32(r1, output);
  }

  // uidivmod returns the quotient in r0, and the remainder in r1.
  if (div && !div->canTruncateRemainder()) {
    MOZ_ASSERT(div->fallible());
    masm.as_cmp(r1, Imm8(0));
    bailoutIf(Assembler::NonZero, ins->snapshot());
  }

  // Bailout for big unsigned results
  if ((div && !div->isTruncated()) || (mod && !mod->isTruncated())) {
    DebugOnly<bool> isFallible =
        (div && div->fallible()) || (mod && mod->fallible());
    MOZ_ASSERT(isFallible);
    masm.as_cmp(output, Imm8(0));
    bailoutIf(Assembler::LessThan, ins->snapshot());
  }

  masm.bind(&done);
}

void CodeGenerator::visitEffectiveAddress(LEffectiveAddress* ins) {
  const MEffectiveAddress* mir = ins->mir();
  Register base = ToRegister(ins->base());
  Register index = ToRegister(ins->index());
  Register output = ToRegister(ins->output());

  ScratchRegisterScope scratch(masm);

  masm.as_add(output, base, lsl(index, mir->scale()));
  masm.ma_add(Imm32(mir->displacement()), output, scratch);
}

void CodeGenerator::visitNegI(LNegI* ins) {
  Register input = ToRegister(ins->input());
  masm.ma_neg(input, ToRegister(ins->output()));
}

void CodeGenerator::visitNegI64(LNegI64* ins) {
  Register64 input = ToRegister64(ins->getInt64Operand(0));
  MOZ_ASSERT(input == ToOutRegister64(ins));
  masm.neg64(input);
}

void CodeGenerator::visitNegD(LNegD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  masm.ma_vneg(input, ToFloatRegister(ins->output()));
}

void CodeGenerator::visitNegF(LNegF* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  masm.ma_vneg_f32(input, ToFloatRegister(ins->output()));
}

void CodeGenerator::visitMemoryBarrier(LMemoryBarrier* ins) {
  masm.memoryBarrier(ins->type());
}

void CodeGenerator::visitWasmTruncateToInt32(LWasmTruncateToInt32* lir) {
  auto input = ToFloatRegister(lir->input());
  auto output = ToRegister(lir->output());

  MWasmTruncateToInt32* mir = lir->mir();
  MIRType fromType = mir->input()->type();

  OutOfLineWasmTruncateCheck* ool = nullptr;
  Label* oolEntry = nullptr;
  if (!lir->mir()->isSaturating()) {
    ool = new (alloc())
        OutOfLineWasmTruncateCheck(mir, input, Register::Invalid());
    addOutOfLineCode(ool, mir);
    oolEntry = ool->entry();
  }

  masm.wasmTruncateToInt32(input, output, fromType, mir->isUnsigned(),
                           mir->isSaturating(), oolEntry);

  if (!lir->mir()->isSaturating()) {
    masm.bind(ool->rejoin());
  }
}

void CodeGenerator::visitWasmTruncateToInt64(LWasmTruncateToInt64* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  MOZ_ASSERT(ToRegister(lir->tls()) == WasmTlsReg);
  masm.Push(WasmTlsReg);
  int32_t framePushedAfterTls = masm.framePushed();

  FloatRegister input = ToFloatRegister(lir->input());
  FloatRegister inputDouble = input;
  Register64 output = ToOutRegister64(lir);

  MWasmBuiltinTruncateToInt64* mir = lir->mir();
  MIRType fromType = mir->input()->type();

  OutOfLineWasmTruncateCheck* ool = nullptr;
  if (!lir->mir()->isSaturating()) {
    ool = new (alloc())
        OutOfLineWasmTruncateCheck(mir, input, Register64::Invalid());
    addOutOfLineCode(ool, mir);
  }

  ScratchDoubleScope fpscratch(masm);
  if (fromType == MIRType::Float32) {
    inputDouble = fpscratch;
    masm.convertFloat32ToDouble(input, inputDouble);
  }

  masm.Push(input);

  masm.setupWasmABICall();
  masm.passABIArg(inputDouble, MoveOp::DOUBLE);

  int32_t tlsOffset = masm.framePushed() - framePushedAfterTls;
  if (lir->mir()->isSaturating()) {
    if (lir->mir()->isUnsigned()) {
      masm.callWithABI(mir->bytecodeOffset(),
                       wasm::SymbolicAddress::SaturatingTruncateDoubleToUint64,
                       mozilla::Some(tlsOffset));
    } else {
      masm.callWithABI(mir->bytecodeOffset(),
                       wasm::SymbolicAddress::SaturatingTruncateDoubleToInt64,
                       mozilla::Some(tlsOffset));
    }
  } else {
    if (lir->mir()->isUnsigned()) {
      masm.callWithABI(mir->bytecodeOffset(),
                       wasm::SymbolicAddress::TruncateDoubleToUint64,
                       mozilla::Some(tlsOffset));
    } else {
      masm.callWithABI(mir->bytecodeOffset(),
                       wasm::SymbolicAddress::TruncateDoubleToInt64,
                       mozilla::Some(tlsOffset));
    }
  }

  masm.Pop(input);
  masm.Pop(WasmTlsReg);

  // TruncateDoubleTo{UI,I}nt64 returns 0x8000000000000000 to indicate
  // exceptional results, so check for that and produce the appropriate
  // traps. The Saturating form always returns a normal value and never
  // needs traps.
  if (!lir->mir()->isSaturating()) {
    ScratchRegisterScope scratch(masm);
    masm.ma_cmp(output.high, Imm32(0x80000000), scratch);
    masm.as_cmp(output.low, Imm8(0x00000000), Assembler::Equal);
    masm.ma_b(ool->entry(), Assembler::Equal);

    masm.bind(ool->rejoin());
  }

  MOZ_ASSERT(ReturnReg64 == output);
}

void CodeGeneratorARM::visitOutOfLineWasmTruncateCheck(
    OutOfLineWasmTruncateCheck* ool) {
  // On ARM, saturating truncation codegen handles saturating itself rather than
  // relying on out-of-line fixup code.
  if (ool->isSaturating()) {
    return;
  }

  masm.outOfLineWasmTruncateToIntCheck(ool->input(), ool->fromType(),
                                       ool->toType(), ool->isUnsigned(),
                                       ool->rejoin(), ool->bytecodeOffset());
}

void CodeGenerator::visitInt64ToFloatingPointCall(
    LInt64ToFloatingPointCall* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  MOZ_ASSERT(ToRegister(lir->getOperand(LInt64ToFloatingPointCall::Tls)) ==
             WasmTlsReg);
  masm.Push(WasmTlsReg);
  int32_t framePushedAfterTls = masm.framePushed();

  Register64 input = ToRegister64(lir->getInt64Operand(0));

  MBuiltinInt64ToFloatingPoint* mir = lir->mir();
  MIRType toType = mir->type();

  masm.setupWasmABICall();
  masm.passABIArg(input.high);
  masm.passABIArg(input.low);

  bool isUnsigned = mir->isUnsigned();
  wasm::SymbolicAddress callee =
      toType == MIRType::Float32
          ? (isUnsigned ? wasm::SymbolicAddress::Uint64ToFloat32
                        : wasm::SymbolicAddress::Int64ToFloat32)
          : (isUnsigned ? wasm::SymbolicAddress::Uint64ToDouble
                        : wasm::SymbolicAddress::Int64ToDouble);

  int32_t tlsOffset = masm.framePushed() - framePushedAfterTls;
  MoveOp::Type result =
      toType == MIRType::Float32 ? MoveOp::FLOAT32 : MoveOp::DOUBLE;
  masm.callWithABI(mir->bytecodeOffset(), callee, mozilla::Some(tlsOffset),
                   result);

  DebugOnly<FloatRegister> output(ToFloatRegister(lir->output()));
  MOZ_ASSERT_IF(toType == MIRType::Double, output.value == ReturnDoubleReg);
  MOZ_ASSERT_IF(toType == MIRType::Float32, output.value == ReturnFloat32Reg);

  masm.Pop(WasmTlsReg);
}

void CodeGenerator::visitCopySignF(LCopySignF* ins) {
  FloatRegister lhs = ToFloatRegister(ins->getOperand(0));
  FloatRegister rhs = ToFloatRegister(ins->getOperand(1));
  FloatRegister output = ToFloatRegister(ins->getDef(0));

  Register lhsi = ToRegister(ins->getTemp(0));
  Register rhsi = ToRegister(ins->getTemp(1));

  masm.ma_vxfer(lhs, lhsi);
  masm.ma_vxfer(rhs, rhsi);

  ScratchRegisterScope scratch(masm);

  // Clear lhs's sign.
  masm.ma_and(Imm32(INT32_MAX), lhsi, lhsi, scratch);

  // Keep rhs's sign.
  masm.ma_and(Imm32(INT32_MIN), rhsi, rhsi, scratch);

  // Combine.
  masm.ma_orr(lhsi, rhsi, rhsi);

  masm.ma_vxfer(rhsi, output);
}

void CodeGenerator::visitCopySignD(LCopySignD* ins) {
  FloatRegister lhs = ToFloatRegister(ins->getOperand(0));
  FloatRegister rhs = ToFloatRegister(ins->getOperand(1));
  FloatRegister output = ToFloatRegister(ins->getDef(0));

  Register lhsi = ToRegister(ins->getTemp(0));
  Register rhsi = ToRegister(ins->getTemp(1));

  // Manipulate high words of double inputs.
  masm.as_vxfer(lhsi, InvalidReg, lhs, Assembler::FloatToCore,
                Assembler::Always, 1);
  masm.as_vxfer(rhsi, InvalidReg, rhs, Assembler::FloatToCore,
                Assembler::Always, 1);

  ScratchRegisterScope scratch(masm);

  // Clear lhs's sign.
  masm.ma_and(Imm32(INT32_MAX), lhsi, lhsi, scratch);

  // Keep rhs's sign.
  masm.ma_and(Imm32(INT32_MIN), rhsi, rhsi, scratch);

  // Combine.
  masm.ma_orr(lhsi, rhsi, rhsi);

  // Reconstruct the output.
  masm.as_vxfer(lhsi, InvalidReg, lhs, Assembler::FloatToCore,
                Assembler::Always, 0);
  masm.ma_vxfer(lhsi, rhsi, output);
}

void CodeGenerator::visitWrapInt64ToInt32(LWrapInt64ToInt32* lir) {
  const LInt64Allocation& input = lir->getInt64Operand(0);
  Register output = ToRegister(lir->output());

  if (lir->mir()->bottomHalf()) {
    masm.move32(ToRegister(input.low()), output);
  } else {
    masm.move32(ToRegister(input.high()), output);
  }
}

void CodeGenerator::visitExtendInt32ToInt64(LExtendInt32ToInt64* lir) {
  Register64 output = ToOutRegister64(lir);
  MOZ_ASSERT(ToRegister(lir->input()) == output.low);

  if (lir->mir()->isUnsigned()) {
    masm.ma_mov(Imm32(0), output.high);
  } else {
    masm.ma_asr(Imm32(31), output.low, output.high);
  }
}

void CodeGenerator::visitSignExtendInt64(LSignExtendInt64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);
  switch (lir->mode()) {
    case MSignExtendInt64::Byte:
      masm.move8SignExtend(input.low, output.low);
      break;
    case MSignExtendInt64::Half:
      masm.move16SignExtend(input.low, output.low);
      break;
    case MSignExtendInt64::Word:
      masm.move32(input.low, output.low);
      break;
  }
  masm.ma_asr(Imm32(31), output.low, output.high);
}

void CodeGenerator::visitWasmExtendU32Index(LWasmExtendU32Index*) {
  MOZ_CRASH("64-bit only");
}

void CodeGenerator::visitWasmWrapU32Index(LWasmWrapU32Index*) {
  MOZ_CRASH("64-bit only");
}

void CodeGenerator::visitDivOrModI64(LDivOrModI64* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  MOZ_ASSERT(ToRegister(lir->getOperand(LDivOrModI64::Tls)) == WasmTlsReg);
  masm.Push(WasmTlsReg);
  int32_t framePushedAfterTls = masm.framePushed();

  Register64 lhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Lhs));
  Register64 rhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Rhs));
  Register64 output = ToOutRegister64(lir);

  MOZ_ASSERT(output == ReturnReg64);

  Label done;

  // Handle divide by zero.
  if (lir->canBeDivideByZero()) {
    Label nonZero;
    // We can use WasmTlsReg as temp register because we preserved it before.
    masm.branchTest64(Assembler::NonZero, rhs, rhs, WasmTlsReg, &nonZero);
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
    masm.bind(&nonZero);
  }

  auto* mir = lir->mir();

  // Handle an integer overflow exception from INT64_MIN / -1.
  if (lir->canBeNegativeOverflow()) {
    Label notmin;
    masm.branch64(Assembler::NotEqual, lhs, Imm64(INT64_MIN), &notmin);
    masm.branch64(Assembler::NotEqual, rhs, Imm64(-1), &notmin);
    if (mir->isWasmBuiltinModI64()) {
      masm.xor64(output, output);
    } else {
      masm.wasmTrap(wasm::Trap::IntegerOverflow, lir->bytecodeOffset());
    }
    masm.jump(&done);
    masm.bind(&notmin);
  }

  masm.setupWasmABICall();
  masm.passABIArg(lhs.high);
  masm.passABIArg(lhs.low);
  masm.passABIArg(rhs.high);
  masm.passABIArg(rhs.low);

  int32_t tlsOffset = masm.framePushed() - framePushedAfterTls;
  if (mir->isWasmBuiltinModI64()) {
    masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::ModI64,
                     mozilla::Some(tlsOffset));
  } else {
    masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::DivI64,
                     mozilla::Some(tlsOffset));
  }

  MOZ_ASSERT(ReturnReg64 == output);

  masm.bind(&done);
  masm.Pop(WasmTlsReg);
}

void CodeGenerator::visitUDivOrModI64(LUDivOrModI64* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  MOZ_ASSERT(ToRegister(lir->getOperand(LDivOrModI64::Tls)) == WasmTlsReg);
  masm.Push(WasmTlsReg);
  int32_t framePushedAfterTls = masm.framePushed();

  Register64 lhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Lhs));
  Register64 rhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Rhs));

  MOZ_ASSERT(ToOutRegister64(lir) == ReturnReg64);

  // Prevent divide by zero.
  if (lir->canBeDivideByZero()) {
    Label nonZero;
    // We can use WasmTlsReg as temp register because we preserved it before.
    masm.branchTest64(Assembler::NonZero, rhs, rhs, WasmTlsReg, &nonZero);
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
    masm.bind(&nonZero);
  }

  masm.setupWasmABICall();
  masm.passABIArg(lhs.high);
  masm.passABIArg(lhs.low);
  masm.passABIArg(rhs.high);
  masm.passABIArg(rhs.low);

  MDefinition* mir = lir->mir();
  int32_t tlsOffset = masm.framePushed() - framePushedAfterTls;
  if (mir->isWasmBuiltinModI64()) {
    masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::UModI64,
                     mozilla::Some(tlsOffset));
  } else {
    masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::UDivI64,
                     mozilla::Some(tlsOffset));
  }
  masm.Pop(WasmTlsReg);
}

void CodeGenerator::visitCompareI64(LCompareI64* lir) {
  MCompare* mir = lir->mir();
  MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
             mir->compareType() == MCompare::Compare_UInt64);

  const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
  Register64 lhsRegs = ToRegister64(lhs);
  Register output = ToRegister(lir->output());

  bool isSigned = mir->compareType() == MCompare::Compare_Int64;
  Assembler::Condition condition = JSOpToCondition(lir->jsop(), isSigned);
  Label done;

  masm.move32(Imm32(1), output);

  if (IsConstant(rhs)) {
    Imm64 imm = Imm64(ToInt64(rhs));
    masm.branch64(condition, lhsRegs, imm, &done);
  } else {
    Register64 rhsRegs = ToRegister64(rhs);
    masm.branch64(condition, lhsRegs, rhsRegs, &done);
  }

  masm.move32(Imm32(0), output);
  masm.bind(&done);
}

void CodeGenerator::visitCompareI64AndBranch(LCompareI64AndBranch* lir) {
  MCompare* mir = lir->cmpMir();
  MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
             mir->compareType() == MCompare::Compare_UInt64);

  const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
  Register64 lhsRegs = ToRegister64(lhs);

  bool isSigned = mir->compareType() == MCompare::Compare_Int64;
  Assembler::Condition condition = JSOpToCondition(lir->jsop(), isSigned);

  Label* trueLabel = getJumpLabelForBranch(lir->ifTrue());
  Label* falseLabel = getJumpLabelForBranch(lir->ifFalse());

  if (isNextBlock(lir->ifFalse()->lir())) {
    falseLabel = nullptr;
  } else if (isNextBlock(lir->ifTrue()->lir())) {
    condition = Assembler::InvertCondition(condition);
    trueLabel = falseLabel;
    falseLabel = nullptr;
  }

  if (IsConstant(rhs)) {
    Imm64 imm = Imm64(ToInt64(rhs));
    masm.branch64(condition, lhsRegs, imm, trueLabel, falseLabel);
  } else {
    Register64 rhsRegs = ToRegister64(rhs);
    masm.branch64(condition, lhsRegs, rhsRegs, trueLabel, falseLabel);
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

void CodeGenerator::visitRotateI64(LRotateI64* lir) {
  MRotate* mir = lir->mir();
  LAllocation* count = lir->count();

  Register64 input = ToRegister64(lir->input());
  Register64 output = ToOutRegister64(lir);
  Register temp = ToTempRegisterOrInvalid(lir->temp());

  if (count->isConstant()) {
    int32_t c = int32_t(count->toConstant()->toInt64() & 0x3F);
    if (!c) {
      masm.move64(input, output);
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

void CodeGenerator::visitWasmStackArgI64(LWasmStackArgI64* ins) {
  const MWasmStackArg* mir = ins->mir();
  Address dst(StackPointer, mir->spOffset());
  if (IsConstant(ins->arg())) {
    masm.store64(Imm64(ToInt64(ins->arg())), dst);
  } else {
    masm.store64(ToRegister64(ins->arg()), dst);
  }
}

void CodeGenerator::visitWasmSelectI64(LWasmSelectI64* lir) {
  Register cond = ToRegister(lir->condExpr());
  const LInt64Allocation falseExpr = lir->falseExpr();

  Register64 out = ToOutRegister64(lir);
  MOZ_ASSERT(ToRegister64(lir->trueExpr()) == out,
             "true expr is reused for input");

  masm.as_cmp(cond, Imm8(0));
  if (falseExpr.low().isRegister()) {
    masm.ma_mov(ToRegister(falseExpr.low()), out.low, LeaveCC,
                Assembler::Equal);
    masm.ma_mov(ToRegister(falseExpr.high()), out.high, LeaveCC,
                Assembler::Equal);
  } else {
    ScratchRegisterScope scratch(masm);
    masm.ma_ldr(ToAddress(falseExpr.low()), out.low, scratch, Offset,
                Assembler::Equal);
    masm.ma_ldr(ToAddress(falseExpr.high()), out.high, scratch, Offset,
                Assembler::Equal);
  }
}

void CodeGenerator::visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Double);
  MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Int64);
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  FloatRegister output = ToFloatRegister(lir->output());

  masm.ma_vxfer(input.low, input.high, output);
}

void CodeGenerator::visitWasmReinterpretToI64(LWasmReinterpretToI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);
  MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Double);
  FloatRegister input = ToFloatRegister(lir->getOperand(0));
  Register64 output = ToOutRegister64(lir);

  masm.ma_vxfer(input, output.low, output.high);
}

void CodeGenerator::visitPopcntI64(LPopcntI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);
  Register temp = ToRegister(lir->getTemp(0));

  masm.popcnt64(input, output, temp);
}

void CodeGenerator::visitClzI64(LClzI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);

  masm.clz64(input, output.low);
  masm.move32(Imm32(0), output.high);
}

void CodeGenerator::visitCtzI64(LCtzI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);

  masm.ctz64(input, output.low);
  masm.move32(Imm32(0), output.high);
}

void CodeGenerator::visitTestI64AndBranch(LTestI64AndBranch* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));

  masm.as_cmp(input.high, Imm8(0));
  jumpToBlock(lir->ifTrue(), Assembler::NonZero);
  masm.as_cmp(input.low, Imm8(0));
  emitBranch(Assembler::NonZero, lir->ifTrue(), lir->ifFalse());
}

void CodeGenerator::visitWasmAtomicLoadI64(LWasmAtomicLoadI64* lir) {
  Register ptr = ToRegister(lir->ptr());
  Register64 output = ToOutRegister64(lir);
  Register64 tmp(InvalidReg, InvalidReg);

  BaseIndex addr(HeapReg, ptr, TimesOne, lir->mir()->access().offset());
  masm.wasmAtomicLoad64(lir->mir()->access(), addr, tmp, output);
}

void CodeGenerator::visitWasmAtomicStoreI64(LWasmAtomicStoreI64* lir) {
  Register ptr = ToRegister(lir->ptr());
  Register64 value = ToRegister64(lir->value());
  Register64 tmp(ToRegister(lir->tmpHigh()), ToRegister(lir->tmpLow()));

  BaseIndex addr(HeapReg, ptr, TimesOne, lir->mir()->access().offset());
  masm.wasmAtomicExchange64(lir->mir()->access(), addr, value, tmp);
}

void CodeGenerator::visitWasmCompareExchangeI64(LWasmCompareExchangeI64* lir) {
  Register ptr = ToRegister(lir->ptr());
  Register64 expected = ToRegister64(lir->expected());
  Register64 replacement = ToRegister64(lir->replacement());
  Register64 out = ToOutRegister64(lir);

  BaseIndex addr(HeapReg, ptr, TimesOne, lir->mir()->access().offset());
  masm.wasmCompareExchange64(lir->mir()->access(), addr, expected, replacement,
                             out);
}

void CodeGenerator::visitWasmAtomicBinopI64(LWasmAtomicBinopI64* lir) {
  Register ptr = ToRegister(lir->ptr());
  Register64 value = ToRegister64(lir->value());
  Register64 out = ToOutRegister64(lir);

  BaseIndex addr(HeapReg, ptr, TimesOne, lir->access().offset());
  Register64 tmp(ToRegister(lir->tmpHigh()), ToRegister(lir->tmpLow()));
  masm.wasmAtomicFetchOp64(lir->access(), lir->operation(), value, addr, tmp,
                           out);
}

void CodeGenerator::visitWasmAtomicExchangeI64(LWasmAtomicExchangeI64* lir) {
  Register ptr = ToRegister(lir->ptr());
  Register64 value = ToRegister64(lir->value());
  Register64 out = ToOutRegister64(lir);

  BaseIndex addr(HeapReg, ptr, TimesOne, lir->access().offset());
  masm.wasmAtomicExchange64(lir->access(), addr, value, out);
}

void CodeGenerator::visitNearbyInt(LNearbyInt*) { MOZ_CRASH("NYI"); }

void CodeGenerator::visitNearbyIntF(LNearbyIntF*) { MOZ_CRASH("NYI"); }

void CodeGenerator::visitSimd128(LSimd128* ins) { MOZ_CRASH("No SIMD"); }

void CodeGenerator::visitWasmBitselectSimd128(LWasmBitselectSimd128* ins) {
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
