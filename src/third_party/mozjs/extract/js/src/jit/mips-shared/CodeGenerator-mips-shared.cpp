/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips-shared/CodeGenerator-mips-shared.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jsnum.h"

#include "jit/CodeGenerator.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitRuntime.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "js/Conversions.h"
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
CodeGeneratorMIPSShared::CodeGeneratorMIPSShared(MIRGenerator* gen,
                                                 LIRGraph* graph,
                                                 MacroAssembler* masm)
    : CodeGeneratorShared(gen, graph, masm) {}

Operand CodeGeneratorMIPSShared::ToOperand(const LAllocation& a) {
  if (a.isGeneralReg()) {
    return Operand(a.toGeneralReg()->reg());
  }
  if (a.isFloatReg()) {
    return Operand(a.toFloatReg()->reg());
  }
  return Operand(ToAddress(a));
}

Operand CodeGeneratorMIPSShared::ToOperand(const LAllocation* a) {
  return ToOperand(*a);
}

Operand CodeGeneratorMIPSShared::ToOperand(const LDefinition* def) {
  return ToOperand(def->output());
}

#ifdef JS_PUNBOX64
Operand CodeGeneratorMIPSShared::ToOperandOrRegister64(
    const LInt64Allocation input) {
  return ToOperand(input.value());
}
#else
Register64 CodeGeneratorMIPSShared::ToOperandOrRegister64(
    const LInt64Allocation input) {
  return ToRegister64(input);
}
#endif

void CodeGeneratorMIPSShared::branchToBlock(Assembler::FloatFormat fmt,
                                            FloatRegister lhs,
                                            FloatRegister rhs, MBasicBlock* mir,
                                            Assembler::DoubleCondition cond) {
  // Skip past trivial blocks.
  Label* label = skipTrivialBlocks(mir)->lir()->label();
  if (fmt == Assembler::DoubleFloat) {
    masm.branchDouble(cond, lhs, rhs, label);
  } else {
    masm.branchFloat(cond, lhs, rhs, label);
  }
}

FrameSizeClass FrameSizeClass::FromDepth(uint32_t frameDepth) {
  return FrameSizeClass::None();
}

FrameSizeClass FrameSizeClass::ClassLimit() { return FrameSizeClass(0); }

uint32_t FrameSizeClass::frameSize() const {
  MOZ_CRASH("MIPS does not use frame size classes");
}

void OutOfLineBailout::accept(CodeGeneratorMIPSShared* codegen) {
  codegen->visitOutOfLineBailout(this);
}

void CodeGenerator::visitTestIAndBranch(LTestIAndBranch* test) {
  const LAllocation* opd = test->getOperand(0);
  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  emitBranch(ToRegister(opd), Imm32(0), Assembler::NonZero, ifTrue, ifFalse);
}

void CodeGenerator::visitCompare(LCompare* comp) {
  MCompare* mir = comp->mir();
  Assembler::Condition cond = JSOpToCondition(mir->compareType(), comp->jsop());
  const LAllocation* left = comp->getOperand(0);
  const LAllocation* right = comp->getOperand(1);
  const LDefinition* def = comp->getDef(0);

#ifdef JS_CODEGEN_MIPS64
  if (mir->compareType() == MCompare::Compare_Object ||
      mir->compareType() == MCompare::Compare_Symbol ||
      mir->compareType() == MCompare::Compare_UIntPtr) {
    if (right->isConstant()) {
      MOZ_ASSERT(mir->compareType() == MCompare::Compare_UIntPtr);
      masm.cmpPtrSet(cond, ToRegister(left), Imm32(ToInt32(right)),
                     ToRegister(def));
    } else if (right->isGeneralReg()) {
      masm.cmpPtrSet(cond, ToRegister(left), ToRegister(right),
                     ToRegister(def));
    } else {
      masm.cmpPtrSet(cond, ToRegister(left), ToAddress(right), ToRegister(def));
    }
    return;
  }
#endif

  if (right->isConstant()) {
    masm.cmp32Set(cond, ToRegister(left), Imm32(ToInt32(right)),
                  ToRegister(def));
  } else if (right->isGeneralReg()) {
    masm.cmp32Set(cond, ToRegister(left), ToRegister(right), ToRegister(def));
  } else {
    masm.cmp32Set(cond, ToRegister(left), ToAddress(right), ToRegister(def));
  }
}

void CodeGenerator::visitCompareAndBranch(LCompareAndBranch* comp) {
  MCompare* mir = comp->cmpMir();
  Assembler::Condition cond = JSOpToCondition(mir->compareType(), comp->jsop());

#ifdef JS_CODEGEN_MIPS64
  if (mir->compareType() == MCompare::Compare_Object ||
      mir->compareType() == MCompare::Compare_Symbol ||
      mir->compareType() == MCompare::Compare_UIntPtr) {
    if (comp->right()->isConstant()) {
      MOZ_ASSERT(mir->compareType() == MCompare::Compare_UIntPtr);
      emitBranch(ToRegister(comp->left()), Imm32(ToInt32(comp->right())), cond,
                 comp->ifTrue(), comp->ifFalse());
    } else if (comp->right()->isGeneralReg()) {
      emitBranch(ToRegister(comp->left()), ToRegister(comp->right()), cond,
                 comp->ifTrue(), comp->ifFalse());
    } else {
      masm.loadPtr(ToAddress(comp->right()), ScratchRegister);
      emitBranch(ToRegister(comp->left()), ScratchRegister, cond,
                 comp->ifTrue(), comp->ifFalse());
    }
    return;
  }
#endif

  if (comp->right()->isConstant()) {
    emitBranch(ToRegister(comp->left()), Imm32(ToInt32(comp->right())), cond,
               comp->ifTrue(), comp->ifFalse());
  } else if (comp->right()->isGeneralReg()) {
    emitBranch(ToRegister(comp->left()), ToRegister(comp->right()), cond,
               comp->ifTrue(), comp->ifFalse());
  } else {
    masm.load32(ToAddress(comp->right()), ScratchRegister);
    emitBranch(ToRegister(comp->left()), ScratchRegister, cond, comp->ifTrue(),
               comp->ifFalse());
  }
}

bool CodeGeneratorMIPSShared::generateOutOfLineCode() {
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

void CodeGeneratorMIPSShared::bailoutFrom(Label* label, LSnapshot* snapshot) {
  MOZ_ASSERT_IF(!masm.oom(), label->used());
  MOZ_ASSERT_IF(!masm.oom(), !label->bound());

  encode(snapshot);

  // Though the assembler doesn't track all frame pushes, at least make sure
  // the known value makes sense. We can't use bailout tables if the stack
  // isn't properly aligned to the static frame size.
  MOZ_ASSERT_IF(frameClass_ != FrameSizeClass::None(),
                frameClass_.frameSize() == masm.framePushed());

  // We don't use table bailouts because retargeting is easier this way.
  InlineScriptTree* tree = snapshot->mir()->block()->trackedTree();
  OutOfLineBailout* ool =
      new (alloc()) OutOfLineBailout(snapshot, masm.framePushed());
  addOutOfLineCode(ool,
                   new (alloc()) BytecodeSite(tree, tree->script()->code()));

  masm.retarget(label, ool->entry());
}

void CodeGeneratorMIPSShared::bailout(LSnapshot* snapshot) {
  Label label;
  masm.jump(&label);
  bailoutFrom(&label, snapshot);
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

  MOZ_ASSERT(rhs->isConstant() || rhs->isGeneralReg());

  // If there is no snapshot, we don't need to check for overflow
  if (!ins->snapshot()) {
    if (rhs->isConstant()) {
      masm.ma_addu(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
    } else {
      masm.as_addu(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
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

  MOZ_ASSERT(rhs->isConstant() || rhs->isGeneralReg());

  // If there is no snapshot, we don't need to check for overflow
  if (!ins->snapshot()) {
    if (rhs->isConstant()) {
      masm.ma_subu(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
    } else {
      masm.as_subu(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
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

        masm.ma_negu(dest, src);
        break;
      case 0:
        masm.move32(Imm32(0), dest);
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
          masm.as_addu(dest, src, src);
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
            masm.ma_sll(dest, src, Imm32(shift));
            return;
          }

          // If the constant cannot be encoded as (1<<C1), see if it can
          // be encoded as (1<<C1) | (1<<C2), which can be computed
          // using an add and a shift.
          uint32_t shift_rest = FloorLog2(rest);
          if (src != dest && (1u << shift_rest) == rest) {
            masm.ma_sll(dest, src, Imm32(shift - shift_rest));
            masm.add32(src, dest);
            if (shift_rest != 0) {
              masm.ma_sll(dest, dest, Imm32(shift_rest));
            }
            return;
          }
        }

        if (mul->canOverflow() && (constant > 0) && (src != dest)) {
          // To stay on the safe side, only optimize things that are a
          // power of 2.

          if ((1 << shift) == constant) {
            // dest = lhs * pow(2, shift)
            masm.ma_sll(dest, src, Imm32(shift));
            // At runtime, check (lhs == dest >> shift), if this does
            // not hold, some bits were lost due to overflow, and the
            // computation should be resumed as a double.
            masm.ma_sra(ScratchRegister, dest, Imm32(shift));
            bailoutCmp32(Assembler::NotEqual, src, ScratchRegister,
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
      masm.as_mul(dest, ToRegister(lhs), ToRegister(rhs));
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
  const LInt64Allocation lhs = lir->getInt64Operand(LMulI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LMulI64::Rhs);
  const Register64 output = ToOutRegister64(lir);

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
      default:
        if (constant > 0) {
          if (mozilla::IsPowerOfTwo(static_cast<uint32_t>(constant + 1))) {
            masm.move64(ToRegister64(lhs), output);
            masm.lshift64(Imm32(FloorLog2(constant + 1)), output);
            masm.sub64(ToRegister64(lhs), output);
            return;
          } else if (mozilla::IsPowerOfTwo(
                         static_cast<uint32_t>(constant - 1))) {
            masm.move64(ToRegister64(lhs), output);
            masm.lshift64(Imm32(FloorLog2(constant - 1u)), output);
            masm.add64(ToRegister64(lhs), output);
            return;
          }
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

void CodeGenerator::visitDivI(LDivI* ins) {
  // Extract the registers from this instruction
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register dest = ToRegister(ins->output());
  Register temp = ToRegister(ins->getTemp(0));
  MDiv* mir = ins->mir();

  Label done;

  // Handle divide by zero.
  if (mir->canBeDivideByZero()) {
    if (mir->trapOnError()) {
      Label nonZero;
      masm.ma_b(rhs, rhs, &nonZero, Assembler::NonZero);
      masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
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
      masm.wasmTrap(wasm::Trap::IntegerOverflow, mir->bytecodeOffset());
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
#ifdef MIPSR6
    masm.as_div(dest, lhs, rhs);
#else
    masm.as_div(lhs, rhs);
    masm.as_mflo(dest);
#endif
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
  Register tmp = ToRegister(ins->getTemp(0));
  int32_t shift = ins->shift();

  if (shift != 0) {
    MDiv* mir = ins->mir();
    if (!mir->isTruncated()) {
      // If the remainder is going to be != 0, bailout since this must
      // be a double.
      masm.ma_sll(tmp, lhs, Imm32(32 - shift));
      bailoutCmp32(Assembler::NonZero, tmp, tmp, ins->snapshot());
    }

    if (!mir->canBeNegativeDividend()) {
      // Numerator is unsigned, so needs no adjusting. Do the shift.
      masm.ma_sra(dest, lhs, Imm32(shift));
      return;
    }

    // Adjust the value so that shifting produces a correctly rounded result
    // when the numerator is negative. See 10-1 "Signed Division by a Known
    // Power of 2" in Henry S. Warren, Jr.'s Hacker's Delight.
    if (shift > 1) {
      masm.ma_sra(tmp, lhs, Imm32(31));
      masm.ma_srl(tmp, tmp, Imm32(32 - shift));
      masm.add32(lhs, tmp);
    } else {
      masm.ma_srl(tmp, lhs, Imm32(32 - shift));
      masm.add32(lhs, tmp);
    }

    // Do the shift.
    masm.ma_sra(dest, tmp, Imm32(shift));
  } else {
    masm.move32(lhs, dest);
  }
}

void CodeGenerator::visitModI(LModI* ins) {
  // Extract the registers from this instruction
  Register lhs = ToRegister(ins->lhs());
  Register rhs = ToRegister(ins->rhs());
  Register dest = ToRegister(ins->output());
  Register callTemp = ToRegister(ins->callTemp());
  MMod* mir = ins->mir();
  Label done, prevent;

  masm.move32(lhs, callTemp);

  // Prevent INT_MIN % -1;
  // The integer division will give INT_MIN, but we want -(double)INT_MIN.
  if (mir->canBeNegativeDividend()) {
    masm.ma_b(lhs, Imm32(INT_MIN), &prevent, Assembler::NotEqual, ShortJump);
    if (mir->isTruncated()) {
      // (INT_MIN % -1)|0 == 0
      Label skip;
      masm.ma_b(rhs, Imm32(-1), &skip, Assembler::NotEqual, ShortJump);
      masm.move32(Imm32(0), dest);
      masm.ma_b(&done, ShortJump);
      masm.bind(&skip);
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutCmp32(Assembler::Equal, rhs, Imm32(-1), ins->snapshot());
    }
    masm.bind(&prevent);
  }

  // 0/X (with X < 0) is bad because both of these values *should* be
  // doubles, and the result should be -0.0, which cannot be represented in
  // integers. X/0 is bad because it will give garbage (or abort), when it
  // should give either \infty, -\infty or NAN.

  // Prevent 0 / X (with X < 0) and X / 0
  // testing X / Y.  Compare Y with 0.
  // There are three cases: (Y < 0), (Y == 0) and (Y > 0)
  // If (Y < 0), then we compare X with 0, and bail if X == 0
  // If (Y == 0), then we simply want to bail.
  // if (Y > 0), we don't bail.

  if (mir->canBeDivideByZero()) {
    if (mir->isTruncated()) {
      if (mir->trapOnError()) {
        Label nonZero;
        masm.ma_b(rhs, rhs, &nonZero, Assembler::NonZero);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
        masm.bind(&nonZero);
      } else {
        Label skip;
        masm.ma_b(rhs, Imm32(0), &skip, Assembler::NotEqual, ShortJump);
        masm.move32(Imm32(0), dest);
        masm.ma_b(&done, ShortJump);
        masm.bind(&skip);
      }
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutCmp32(Assembler::Equal, rhs, Imm32(0), ins->snapshot());
    }
  }

  if (mir->canBeNegativeDividend()) {
    Label notNegative;
    masm.ma_b(rhs, Imm32(0), &notNegative, Assembler::GreaterThan, ShortJump);
    if (mir->isTruncated()) {
      // NaN|0 == 0 and (0 % -X)|0 == 0
      Label skip;
      masm.ma_b(lhs, Imm32(0), &skip, Assembler::NotEqual, ShortJump);
      masm.move32(Imm32(0), dest);
      masm.ma_b(&done, ShortJump);
      masm.bind(&skip);
    } else {
      MOZ_ASSERT(mir->fallible());
      bailoutCmp32(Assembler::Equal, lhs, Imm32(0), ins->snapshot());
    }
    masm.bind(&notNegative);
  }
#ifdef MIPSR6
  masm.as_mod(dest, lhs, rhs);
#else
  masm.as_div(lhs, rhs);
  masm.as_mfhi(dest);
#endif

  // If X%Y == 0 and X < 0, then we *actually* wanted to return -0.0
  if (mir->canBeNegativeDividend()) {
    if (mir->isTruncated()) {
      // -0.0|0 == 0
    } else {
      MOZ_ASSERT(mir->fallible());
      // See if X < 0
      masm.ma_b(dest, Imm32(0), &done, Assembler::NotEqual, ShortJump);
      bailoutCmp32(Assembler::Signed, callTemp, Imm32(0), ins->snapshot());
    }
  }
  masm.bind(&done);
}

void CodeGenerator::visitModPowTwoI(LModPowTwoI* ins) {
  Register in = ToRegister(ins->getOperand(0));
  Register out = ToRegister(ins->getDef(0));
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
  Register src = ToRegister(ins->getOperand(0));
  Register dest = ToRegister(ins->getDef(0));
  Register tmp0 = ToRegister(ins->getTemp(0));
  Register tmp1 = ToRegister(ins->getTemp(1));
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
  const LAllocation* input = ins->getOperand(0);
  const LDefinition* dest = ins->getDef(0);
  MOZ_ASSERT(!input->isConstant());

  masm.ma_not(ToRegister(dest), ToRegister(input));
}

void CodeGenerator::visitBitOpI(LBitOpI* ins) {
  const LAllocation* lhs = ins->getOperand(0);
  const LAllocation* rhs = ins->getOperand(1);
  const LDefinition* dest = ins->getDef(0);
  // all of these bitops should be either imm32's, or integer registers.
  switch (ins->bitop()) {
    case JSOp::BitOr:
      if (rhs->isConstant()) {
        masm.ma_or(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
      } else {
        masm.as_or(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
      }
      break;
    case JSOp::BitXor:
      if (rhs->isConstant()) {
        masm.ma_xor(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
      } else {
        masm.as_xor(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
      }
      break;
    case JSOp::BitAnd:
      if (rhs->isConstant()) {
        masm.ma_and(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
      } else {
        masm.as_and(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
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
  Register dest = ToRegister(ins->output());

  if (rhs->isConstant()) {
    int32_t shift = ToInt32(rhs) & 0x1F;
    switch (ins->bitop()) {
      case JSOp::Lsh:
        if (shift) {
          masm.ma_sll(dest, lhs, Imm32(shift));
        } else {
          masm.move32(lhs, dest);
        }
        break;
      case JSOp::Rsh:
        if (shift) {
          masm.ma_sra(dest, lhs, Imm32(shift));
        } else {
          masm.move32(lhs, dest);
        }
        break;
      case JSOp::Ursh:
        if (shift) {
          masm.ma_srl(dest, lhs, Imm32(shift));
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
        masm.ma_sll(dest, lhs, dest);
        break;
      case JSOp::Rsh:
        masm.ma_sra(dest, lhs, dest);
        break;
      case JSOp::Ursh:
        masm.ma_srl(dest, lhs, dest);
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

void CodeGenerator::visitRotateI64(LRotateI64* lir) {
  MRotate* mir = lir->mir();
  LAllocation* count = lir->count();

  Register64 input = ToRegister64(lir->input());
  Register64 output = ToOutRegister64(lir);
  Register temp = ToTempRegisterOrInvalid(lir->temp());

#ifdef JS_CODEGEN_MIPS64
  MOZ_ASSERT(input == output);
#endif

  if (count->isConstant()) {
    int32_t c = int32_t(count->toConstant()->toInt64() & 0x3F);
    if (!c) {
#ifdef JS_CODEGEN_MIPS32
      masm.move64(input, output);
#endif
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

void CodeGenerator::visitUrshD(LUrshD* ins) {
  Register lhs = ToRegister(ins->lhs());
  Register temp = ToRegister(ins->temp());

  const LAllocation* rhs = ins->rhs();
  FloatRegister out = ToFloatRegister(ins->output());

  if (rhs->isConstant()) {
    masm.ma_srl(temp, lhs, Imm32(ToInt32(rhs)));
  } else {
    masm.ma_srl(temp, lhs, ToRegister(rhs));
  }

  masm.convertUInt32ToDouble(temp, out);
}

void CodeGenerator::visitClzI(LClzI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.as_clz(output, input);
}

void CodeGenerator::visitCtzI(LCtzI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.ma_ctz(output, input);
}

void CodeGenerator::visitPopcntI(LPopcntI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());
  Register tmp = ToRegister(ins->temp());

  masm.popcnt32(input, output, tmp);
}

void CodeGenerator::visitPopcntI64(LPopcntI64* ins) {
  Register64 input = ToRegister64(ins->getInt64Operand(0));
  Register64 output = ToOutRegister64(ins);
  Register tmp = ToRegister(ins->getTemp(0));

  masm.popcnt64(input, output, tmp);
}

void CodeGenerator::visitPowHalfD(LPowHalfD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());

  Label done, skip;

  // Masm.pow(-Infinity, 0.5) == Infinity.
  masm.loadConstantDouble(NegativeInfinity<double>(), ScratchDoubleReg);
  masm.ma_bc1d(input, ScratchDoubleReg, &skip,
               Assembler::DoubleNotEqualOrUnordered, ShortJump);
  masm.as_negd(output, ScratchDoubleReg);
  masm.ma_b(&done, ShortJump);

  masm.bind(&skip);
  // Math.pow(-0, 0.5) == 0 == Math.pow(0, 0.5).
  // Adding 0 converts any -0 to 0.
  masm.loadConstantDouble(0.0, ScratchDoubleReg);
  masm.as_addd(output, input, ScratchDoubleReg);
  masm.as_sqrtd(output, output);

  masm.bind(&done);
}

MoveOperand CodeGeneratorMIPSShared::toMoveOperand(LAllocation a) const {
  if (a.isGeneralReg()) {
    return MoveOperand(ToRegister(a));
  }
  if (a.isFloatReg()) {
    return MoveOperand(ToFloatRegister(a));
  }
  MoveOperand::Kind kind =
      a.isStackArea() ? MoveOperand::EFFECTIVE_ADDRESS : MoveOperand::MEMORY;
  Address address = ToAddress(a);
  MOZ_ASSERT((address.offset & 3) == 0);
  return MoveOperand(address, kind);
}

void CodeGenerator::visitMathD(LMathD* math) {
  FloatRegister src1 = ToFloatRegister(math->getOperand(0));
  FloatRegister src2 = ToFloatRegister(math->getOperand(1));
  FloatRegister output = ToFloatRegister(math->getDef(0));

  switch (math->jsop()) {
    case JSOp::Add:
      masm.as_addd(output, src1, src2);
      break;
    case JSOp::Sub:
      masm.as_subd(output, src1, src2);
      break;
    case JSOp::Mul:
      masm.as_muld(output, src1, src2);
      break;
    case JSOp::Div:
      masm.as_divd(output, src1, src2);
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
      masm.as_adds(output, src1, src2);
      break;
    case JSOp::Sub:
      masm.as_subs(output, src1, src2);
      break;
    case JSOp::Mul:
      masm.as_muls(output, src1, src2);
      break;
    case JSOp::Div:
      masm.as_divs(output, src1, src2);
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
  emitTruncateDouble(ToFloatRegister(lir->getOperand(0)),
                     ToRegister(lir->getDef(0)), lir->mir());
}

void CodeGenerator::visitWasmBuiltinTruncateFToInt32(
    LWasmBuiltinTruncateFToInt32* lir) {
  emitTruncateFloat32(ToFloatRegister(lir->getOperand(0)),
                      ToRegister(lir->getDef(0)), lir->mir());
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

void CodeGeneratorMIPSShared::visitOutOfLineBailout(OutOfLineBailout* ool) {
  // Push snapshotOffset and make sure stack is aligned.
  masm.subPtr(Imm32(sizeof(Value)), StackPointer);
  masm.storePtr(ImmWord(ool->snapshot()->snapshotOffset()),
                Address(StackPointer, 0));

  masm.jump(&deoptLabel_);
}

void CodeGeneratorMIPSShared::visitOutOfLineWasmTruncateCheck(
    OutOfLineWasmTruncateCheck* ool) {
  if (ool->toType() == MIRType::Int32) {
    masm.outOfLineWasmTruncateToInt32Check(
        ool->input(), ool->output(), ool->fromType(), ool->flags(),
        ool->rejoin(), ool->bytecodeOffset());
  } else {
    MOZ_ASSERT(ool->toType() == MIRType::Int64);
    masm.outOfLineWasmTruncateToInt64Check(
        ool->input(), ool->output64(), ool->fromType(), ool->flags(),
        ool->rejoin(), ool->bytecodeOffset());
  }
}

void CodeGenerator::visitCopySignF(LCopySignF* ins) {
  FloatRegister lhs = ToFloatRegister(ins->getOperand(0));
  FloatRegister rhs = ToFloatRegister(ins->getOperand(1));
  FloatRegister output = ToFloatRegister(ins->getDef(0));

  Register lhsi = ToRegister(ins->getTemp(0));
  Register rhsi = ToRegister(ins->getTemp(1));

  masm.moveFromFloat32(lhs, lhsi);
  masm.moveFromFloat32(rhs, rhsi);

  // Combine.
  masm.ma_ins(rhsi, lhsi, 0, 31);

  masm.moveToFloat32(rhsi, output);
}

void CodeGenerator::visitCopySignD(LCopySignD* ins) {
  FloatRegister lhs = ToFloatRegister(ins->getOperand(0));
  FloatRegister rhs = ToFloatRegister(ins->getOperand(1));
  FloatRegister output = ToFloatRegister(ins->getDef(0));

  Register lhsi = ToRegister(ins->getTemp(0));
  Register rhsi = ToRegister(ins->getTemp(1));

  // Manipulate high words of double inputs.
  masm.moveFromDoubleHi(lhs, lhsi);
  masm.moveFromDoubleHi(rhs, rhsi);

  // Combine.
  masm.ma_ins(rhsi, lhsi, 0, 31);

  masm.moveToDoubleHi(rhsi, output);
}

void CodeGenerator::visitValue(LValue* value) {
  const ValueOperand out = ToOutValue(value);

  masm.moveValue(value->value(), out);
}

void CodeGenerator::visitDouble(LDouble* ins) {
  const LDefinition* out = ins->getDef(0);

  masm.loadConstantDouble(ins->getDouble(), ToFloatRegister(out));
}

void CodeGenerator::visitFloat32(LFloat32* ins) {
  const LDefinition* out = ins->getDef(0);
  masm.loadConstantFloat32(ins->getFloat(), ToFloatRegister(out));
}

void CodeGenerator::visitTestDAndBranch(LTestDAndBranch* test) {
  FloatRegister input = ToFloatRegister(test->input());

  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  masm.loadConstantDouble(0.0, ScratchDoubleReg);
  // If 0, or NaN, the result is false.

  if (isNextBlock(ifFalse->lir())) {
    branchToBlock(Assembler::DoubleFloat, input, ScratchDoubleReg, ifTrue,
                  Assembler::DoubleNotEqual);
  } else {
    branchToBlock(Assembler::DoubleFloat, input, ScratchDoubleReg, ifFalse,
                  Assembler::DoubleEqualOrUnordered);
    jumpToBlock(ifTrue);
  }
}

void CodeGenerator::visitTestFAndBranch(LTestFAndBranch* test) {
  FloatRegister input = ToFloatRegister(test->input());

  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  masm.loadConstantFloat32(0.0f, ScratchFloat32Reg);
  // If 0, or NaN, the result is false.

  if (isNextBlock(ifFalse->lir())) {
    branchToBlock(Assembler::SingleFloat, input, ScratchFloat32Reg, ifTrue,
                  Assembler::DoubleNotEqual);
  } else {
    branchToBlock(Assembler::SingleFloat, input, ScratchFloat32Reg, ifFalse,
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

void CodeGenerator::visitBitAndAndBranch(LBitAndAndBranch* lir) {
  if (lir->right()->isConstant()) {
    masm.ma_and(ScratchRegister, ToRegister(lir->left()),
                Imm32(ToInt32(lir->right())));
  } else {
    masm.as_and(ScratchRegister, ToRegister(lir->left()),
                ToRegister(lir->right()));
  }
  emitBranch(ScratchRegister, ScratchRegister, lir->cond(), lir->ifTrue(),
             lir->ifFalse());
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
  masm.cmp32Set(Assembler::Equal, ToRegister(ins->input()), Imm32(0),
                ToRegister(ins->output()));
}

void CodeGenerator::visitNotD(LNotD* ins) {
  // Since this operation is not, we want to set a bit if
  // the double is falsey, which means 0.0, -0.0 or NaN.
  FloatRegister in = ToFloatRegister(ins->input());
  Register dest = ToRegister(ins->output());

  masm.loadConstantDouble(0.0, ScratchDoubleReg);
  masm.ma_cmp_set_double(dest, in, ScratchDoubleReg,
                         Assembler::DoubleEqualOrUnordered);
}

void CodeGenerator::visitNotF(LNotF* ins) {
  // Since this operation is not, we want to set a bit if
  // the float32 is falsey, which means 0.0, -0.0 or NaN.
  FloatRegister in = ToFloatRegister(ins->input());
  Register dest = ToRegister(ins->output());

  masm.loadConstantFloat32(0.0f, ScratchFloat32Reg);
  masm.ma_cmp_set_float32(dest, in, ScratchFloat32Reg,
                          Assembler::DoubleEqualOrUnordered);
}

void CodeGenerator::visitMemoryBarrier(LMemoryBarrier* ins) {
  masm.memoryBarrier(ins->type());
}

void CodeGeneratorMIPSShared::generateInvalidateEpilogue() {
  // Ensure that there is enough space in the buffer for the OsiPoint
  // patching to occur. Otherwise, we could overwrite the invalidation
  // epilogue.
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

class js::jit::OutOfLineTableSwitch
    : public OutOfLineCodeBase<CodeGeneratorMIPSShared> {
  MTableSwitch* mir_;
  CodeLabel jumpLabel_;

  void accept(CodeGeneratorMIPSShared* codegen) {
    codegen->visitOutOfLineTableSwitch(this);
  }

 public:
  OutOfLineTableSwitch(MTableSwitch* mir) : mir_(mir) {}

  MTableSwitch* mir() const { return mir_; }

  CodeLabel* jumpLabel() { return &jumpLabel_; }
};

void CodeGeneratorMIPSShared::visitOutOfLineTableSwitch(
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

void CodeGeneratorMIPSShared::emitTableSwitchDispatch(MTableSwitch* mir,
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

void CodeGenerator::visitWasmHeapBase(LWasmHeapBase* ins) {
  MOZ_ASSERT(ins->tlsPtr()->isBogus());
  masm.movePtr(HeapReg, ToRegister(ins->output()));
}

template <typename T>
void CodeGeneratorMIPSShared::emitWasmLoad(T* lir) {
  const MWasmLoad* mir = lir->mir();

  Register ptrScratch = InvalidReg;
  if (!lir->ptrCopy()->isBogusTemp()) {
    ptrScratch = ToRegister(lir->ptrCopy());
  }

  if (IsUnaligned(mir->access())) {
    if (IsFloatingPointType(mir->type())) {
      masm.wasmUnalignedLoadFP(mir->access(), HeapReg, ToRegister(lir->ptr()),
                               ptrScratch, ToFloatRegister(lir->output()),
                               ToRegister(lir->getTemp(1)), InvalidReg,
                               InvalidReg);
    } else {
      masm.wasmUnalignedLoad(mir->access(), HeapReg, ToRegister(lir->ptr()),
                             ptrScratch, ToRegister(lir->output()),
                             ToRegister(lir->getTemp(1)));
    }
  } else {
    masm.wasmLoad(mir->access(), HeapReg, ToRegister(lir->ptr()), ptrScratch,
                  ToAnyRegister(lir->output()));
  }
}

void CodeGenerator::visitWasmLoad(LWasmLoad* lir) { emitWasmLoad(lir); }

void CodeGenerator::visitWasmUnalignedLoad(LWasmUnalignedLoad* lir) {
  emitWasmLoad(lir);
}

template <typename T>
void CodeGeneratorMIPSShared::emitWasmStore(T* lir) {
  const MWasmStore* mir = lir->mir();

  Register ptrScratch = InvalidReg;
  if (!lir->ptrCopy()->isBogusTemp()) {
    ptrScratch = ToRegister(lir->ptrCopy());
  }

  if (IsUnaligned(mir->access())) {
    if (mir->access().type() == Scalar::Float32 ||
        mir->access().type() == Scalar::Float64) {
      masm.wasmUnalignedStoreFP(mir->access(), ToFloatRegister(lir->value()),
                                HeapReg, ToRegister(lir->ptr()), ptrScratch,
                                ToRegister(lir->getTemp(1)));
    } else {
      masm.wasmUnalignedStore(mir->access(), ToRegister(lir->value()), HeapReg,
                              ToRegister(lir->ptr()), ptrScratch,
                              ToRegister(lir->getTemp(1)));
    }
  } else {
    masm.wasmStore(mir->access(), ToAnyRegister(lir->value()), HeapReg,
                   ToRegister(lir->ptr()), ptrScratch);
  }
}

void CodeGenerator::visitWasmStore(LWasmStore* lir) { emitWasmStore(lir); }

void CodeGenerator::visitWasmUnalignedStore(LWasmUnalignedStore* lir) {
  emitWasmStore(lir);
}

void CodeGenerator::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins) {
  const MAsmJSLoadHeap* mir = ins->mir();
  const LAllocation* ptr = ins->ptr();
  const LDefinition* out = ins->output();
  const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();

  bool isSigned;
  int size;
  bool isFloat = false;
  switch (mir->access().type()) {
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
      isSigned = true;
      size = 32;
      break;
    case Scalar::Uint32:
      isSigned = false;
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
      if (size == 32) {
        masm.loadFloat32(Address(HeapReg, ptrImm), ToFloatRegister(out));
      } else {
        masm.loadDouble(Address(HeapReg, ptrImm), ToFloatRegister(out));
      }
    } else {
      masm.ma_load(ToRegister(out), Address(HeapReg, ptrImm),
                   static_cast<LoadStoreSize>(size),
                   isSigned ? SignExtend : ZeroExtend);
    }
    return;
  }

  Register ptrReg = ToRegister(ptr);

  if (!mir->needsBoundsCheck()) {
    if (isFloat) {
      if (size == 32) {
        masm.loadFloat32(BaseIndex(HeapReg, ptrReg, TimesOne),
                         ToFloatRegister(out));
      } else {
        masm.loadDouble(BaseIndex(HeapReg, ptrReg, TimesOne),
                        ToFloatRegister(out));
      }
    } else {
      masm.ma_load(ToRegister(out), BaseIndex(HeapReg, ptrReg, TimesOne),
                   static_cast<LoadStoreSize>(size),
                   isSigned ? SignExtend : ZeroExtend);
    }
    return;
  }

  Label done, outOfRange;
  masm.wasmBoundsCheck32(Assembler::AboveOrEqual, ptrReg,
                         ToRegister(boundsCheckLimit), &outOfRange);
  // Offset is ok, let's load value.
  if (isFloat) {
    if (size == 32) {
      masm.loadFloat32(BaseIndex(HeapReg, ptrReg, TimesOne),
                       ToFloatRegister(out));
    } else {
      masm.loadDouble(BaseIndex(HeapReg, ptrReg, TimesOne),
                      ToFloatRegister(out));
    }
  } else {
    masm.ma_load(ToRegister(out), BaseIndex(HeapReg, ptrReg, TimesOne),
                 static_cast<LoadStoreSize>(size),
                 isSigned ? SignExtend : ZeroExtend);
  }
  masm.ma_b(&done, ShortJump);
  masm.bind(&outOfRange);
  // Offset is out of range. Load default values.
  if (isFloat) {
    if (size == 32) {
      masm.loadConstantFloat32(float(GenericNaN()), ToFloatRegister(out));
    } else {
      masm.loadConstantDouble(GenericNaN(), ToFloatRegister(out));
    }
  } else {
    masm.move32(Imm32(0), ToRegister(out));
  }
  masm.bind(&done);
}

void CodeGenerator::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins) {
  const MAsmJSStoreHeap* mir = ins->mir();
  const LAllocation* value = ins->value();
  const LAllocation* ptr = ins->ptr();
  const LAllocation* boundsCheckLimit = ins->boundsCheckLimit();

  bool isSigned;
  int size;
  bool isFloat = false;
  switch (mir->access().type()) {
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
      isSigned = true;
      size = 32;
      break;
    case Scalar::Uint32:
      isSigned = false;
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
      FloatRegister freg = ToFloatRegister(value);
      Address addr(HeapReg, ptrImm);
      if (size == 32) {
        masm.storeFloat32(freg, addr);
      } else {
        masm.storeDouble(freg, addr);
      }
    } else {
      masm.ma_store(ToRegister(value), Address(HeapReg, ptrImm),
                    static_cast<LoadStoreSize>(size),
                    isSigned ? SignExtend : ZeroExtend);
    }
    return;
  }

  Register ptrReg = ToRegister(ptr);
  Address dstAddr(ptrReg, 0);

  if (!mir->needsBoundsCheck()) {
    if (isFloat) {
      FloatRegister freg = ToFloatRegister(value);
      BaseIndex bi(HeapReg, ptrReg, TimesOne);
      if (size == 32) {
        masm.storeFloat32(freg, bi);
      } else {
        masm.storeDouble(freg, bi);
      }
    } else {
      masm.ma_store(ToRegister(value), BaseIndex(HeapReg, ptrReg, TimesOne),
                    static_cast<LoadStoreSize>(size),
                    isSigned ? SignExtend : ZeroExtend);
    }
    return;
  }

  Label outOfRange;
  masm.wasmBoundsCheck32(Assembler::AboveOrEqual, ptrReg,
                         ToRegister(boundsCheckLimit), &outOfRange);

  // Offset is ok, let's store value.
  if (isFloat) {
    if (size == 32) {
      masm.storeFloat32(ToFloatRegister(value),
                        BaseIndex(HeapReg, ptrReg, TimesOne));
    } else
      masm.storeDouble(ToFloatRegister(value),
                       BaseIndex(HeapReg, ptrReg, TimesOne));
  } else {
    masm.ma_store(ToRegister(value), BaseIndex(HeapReg, ptrReg, TimesOne),
                  static_cast<LoadStoreSize>(size),
                  isSigned ? SignExtend : ZeroExtend);
  }

  masm.bind(&outOfRange);
}

void CodeGenerator::visitWasmCompareExchangeHeap(
    LWasmCompareExchangeHeap* ins) {
  MWasmCompareExchangeHeap* mir = ins->mir();
  Register ptrReg = ToRegister(ins->ptr());
  BaseIndex srcAddr(HeapReg, ptrReg, TimesOne, mir->access().offset());
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  Register oldval = ToRegister(ins->oldValue());
  Register newval = ToRegister(ins->newValue());
  Register valueTemp = ToTempRegisterOrInvalid(ins->valueTemp());
  Register offsetTemp = ToTempRegisterOrInvalid(ins->offsetTemp());
  Register maskTemp = ToTempRegisterOrInvalid(ins->maskTemp());

  masm.wasmCompareExchange(mir->access(), srcAddr, oldval, newval, valueTemp,
                           offsetTemp, maskTemp, ToRegister(ins->output()));
}

void CodeGenerator::visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins) {
  MWasmAtomicExchangeHeap* mir = ins->mir();
  Register ptrReg = ToRegister(ins->ptr());
  Register value = ToRegister(ins->value());
  BaseIndex srcAddr(HeapReg, ptrReg, TimesOne, mir->access().offset());
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  Register valueTemp = ToTempRegisterOrInvalid(ins->valueTemp());
  Register offsetTemp = ToTempRegisterOrInvalid(ins->offsetTemp());
  Register maskTemp = ToTempRegisterOrInvalid(ins->maskTemp());

  masm.wasmAtomicExchange(mir->access(), srcAddr, value, valueTemp, offsetTemp,
                          maskTemp, ToRegister(ins->output()));
}

void CodeGenerator::visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins) {
  MOZ_ASSERT(ins->mir()->hasUses());
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  MWasmAtomicBinopHeap* mir = ins->mir();
  Register ptrReg = ToRegister(ins->ptr());
  Register valueTemp = ToTempRegisterOrInvalid(ins->valueTemp());
  Register offsetTemp = ToTempRegisterOrInvalid(ins->offsetTemp());
  Register maskTemp = ToTempRegisterOrInvalid(ins->maskTemp());

  BaseIndex srcAddr(HeapReg, ptrReg, TimesOne, mir->access().offset());

  masm.wasmAtomicFetchOp(mir->access(), mir->operation(),
                         ToRegister(ins->value()), srcAddr, valueTemp,
                         offsetTemp, maskTemp, ToRegister(ins->output()));
}

void CodeGenerator::visitWasmAtomicBinopHeapForEffect(
    LWasmAtomicBinopHeapForEffect* ins) {
  MOZ_ASSERT(!ins->mir()->hasUses());
  MOZ_ASSERT(ins->addrTemp()->isBogusTemp());

  MWasmAtomicBinopHeap* mir = ins->mir();
  Register ptrReg = ToRegister(ins->ptr());
  Register valueTemp = ToTempRegisterOrInvalid(ins->valueTemp());
  Register offsetTemp = ToTempRegisterOrInvalid(ins->offsetTemp());
  Register maskTemp = ToTempRegisterOrInvalid(ins->maskTemp());

  BaseIndex srcAddr(HeapReg, ptrReg, TimesOne, mir->access().offset());
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
      masm.storeDouble(ToFloatRegister(ins->arg()).doubleOverlay(),
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

  if (mirType == MIRType::Int32 || mirType == MIRType::RefOrNull) {
    Register out = ToRegister(ins->output());
    MOZ_ASSERT(ToRegister(ins->trueExpr()) == out,
               "true expr input is reused for output");
    if (falseExpr->isRegister()) {
      masm.as_movz(out, ToRegister(falseExpr), cond);
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
      masm.as_movz(Assembler::SingleFloat, out, ToFloatRegister(falseExpr),
                   cond);
    } else if (mirType == MIRType::Double) {
      masm.as_movz(Assembler::DoubleFloat, out, ToFloatRegister(falseExpr),
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
      MOZ_ASSERT(from == MIRType::Float32);
      masm.as_mfc1(ToRegister(lir->output()), ToFloatRegister(lir->input()));
      break;
    case MIRType::Float32:
      MOZ_ASSERT(from == MIRType::Int32);
      masm.as_mtc1(ToRegister(lir->input()), ToFloatRegister(lir->output()));
      break;
    case MIRType::Double:
    case MIRType::Int64:
      MOZ_CRASH("not handled by this LIR opcode");
    default:
      MOZ_CRASH("unexpected WasmReinterpret");
  }
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
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, ins->bytecodeOffset());
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

#ifdef MIPSR6
  masm.as_modu(output, lhs, rhs);
#else
  masm.as_divu(lhs, rhs);
  masm.as_mfhi(output);
#endif

  // If the remainder is > 0, bailout since this must be a double.
  if (ins->mir()->isDiv()) {
    if (!ins->mir()->toDiv()->canTruncateRemainder()) {
      bailoutCmp32(Assembler::NonZero, output, output, ins->snapshot());
    }
    // Get quotient
#ifdef MIPSR6
    masm.as_divu(output, lhs, rhs);
#else
    masm.as_mflo(output);
#endif
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

  BaseIndex address(base, index, mir->scale(), mir->displacement());
  masm.computeEffectiveAddress(address, output);
}

void CodeGenerator::visitNegI(LNegI* ins) {
  Register input = ToRegister(ins->input());
  Register output = ToRegister(ins->output());

  masm.ma_negu(output, input);
}

void CodeGenerator::visitNegI64(LNegI64* ins) {
  Register64 input = ToRegister64(ins->getInt64Operand(0));
  MOZ_ASSERT(input == ToOutRegister64(ins));
  masm.neg64(input);
}

void CodeGenerator::visitNegD(LNegD* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());

  masm.as_negd(output, input);
}

void CodeGenerator::visitNegF(LNegF* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  FloatRegister output = ToFloatRegister(ins->output());

  masm.as_negs(output, input);
}

void CodeGenerator::visitWasmAddOffset(LWasmAddOffset* lir) {
  MWasmAddOffset* mir = lir->mir();
  Register base = ToRegister(lir->base());
  Register out = ToRegister(lir->output());

  Label ok;
  masm.ma_add32TestCarry(Assembler::CarryClear, out, base, Imm32(mir->offset()),
                         &ok);
  masm.wasmTrap(wasm::Trap::OutOfBounds, mir->bytecodeOffset());
  masm.bind(&ok);
}

void CodeGenerator::visitAtomicTypedArrayElementBinop(
    LAtomicTypedArrayElementBinop* lir) {
  MOZ_ASSERT(!lir->mir()->isForEffect());

  AnyRegister output = ToAnyRegister(lir->output());
  Register elements = ToRegister(lir->elements());
  Register outTemp = ToTempRegisterOrInvalid(lir->temp2());
  Register valueTemp = ToTempRegisterOrInvalid(lir->valueTemp());
  Register offsetTemp = ToTempRegisterOrInvalid(lir->offsetTemp());
  Register maskTemp = ToTempRegisterOrInvalid(lir->maskTemp());
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
  Register valueTemp = ToTempRegisterOrInvalid(lir->valueTemp());
  Register offsetTemp = ToTempRegisterOrInvalid(lir->offsetTemp());
  Register maskTemp = ToTempRegisterOrInvalid(lir->maskTemp());
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
  Register outTemp = ToTempRegisterOrInvalid(lir->temp());

  Register oldval = ToRegister(lir->oldval());
  Register newval = ToRegister(lir->newval());
  Register valueTemp = ToTempRegisterOrInvalid(lir->valueTemp());
  Register offsetTemp = ToTempRegisterOrInvalid(lir->offsetTemp());
  Register maskTemp = ToTempRegisterOrInvalid(lir->maskTemp());

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
  Register outTemp = ToTempRegisterOrInvalid(lir->temp());

  Register value = ToRegister(lir->value());
  Register valueTemp = ToTempRegisterOrInvalid(lir->valueTemp());
  Register offsetTemp = ToTempRegisterOrInvalid(lir->offsetTemp());
  Register maskTemp = ToTempRegisterOrInvalid(lir->maskTemp());

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
  Register oldval = ToRegister(lir->oldval());
  Register newval = ToRegister(lir->newval());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = ToRegister64(lir->temp2());
  Register out = ToRegister(lir->output());
  Register64 tempOut(out);

  Scalar::Type arrayType = lir->mir()->arrayType();

  masm.loadBigInt64(oldval, temp1);
  masm.loadBigInt64(newval, tempOut);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.compareExchange64(Synchronization::Full(), dest, temp1, tempOut,
                           temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.compareExchange64(Synchronization::Full(), dest, temp1, tempOut,
                           temp2);
  }

  emitCreateBigInt(lir, arrayType, temp2, out, temp1.scratchReg());
}

void CodeGenerator::visitAtomicExchangeTypedArrayElement64(
    LAtomicExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = Register64(ToRegister(lir->temp2()));
  Register out = ToRegister(lir->output());

  Scalar::Type arrayType = lir->mir()->arrayType();

  masm.loadBigInt64(value, temp1);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicExchange64(Synchronization::Full(), dest, temp1, temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicExchange64(Synchronization::Full(), dest, temp1, temp2);
  }

  emitCreateBigInt(lir, arrayType, temp2, out, temp1.scratchReg());
}

void CodeGenerator::visitAtomicTypedArrayElementBinop64(
    LAtomicTypedArrayElementBinop64* lir) {
  MOZ_ASSERT(lir->mir()->hasUses());

  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = ToRegister64(lir->temp2());
  Register out = ToRegister(lir->output());
  Register64 tempOut = Register64(out);

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  masm.loadBigInt64(value, temp1);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, temp1, dest,
                         tempOut, temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, temp1, dest,
                         tempOut, temp2);
  }

  emitCreateBigInt(lir, arrayType, temp2, out, temp1.scratchReg());
}

void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect64(
    LAtomicTypedArrayElementBinopForEffect64* lir) {
  MOZ_ASSERT(!lir->mir()->hasUses());

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

void CodeGenerator::visitWasmCompareExchangeI64(LWasmCompareExchangeI64* lir) {
  Register ptr = ToRegister(lir->ptr());
  Register64 oldValue = ToRegister64(lir->oldValue());
  Register64 newValue = ToRegister64(lir->newValue());
  Register64 output = ToOutRegister64(lir);
  uint32_t offset = lir->mir()->access().offset();

  BaseIndex addr(HeapReg, ptr, TimesOne, offset);
  masm.wasmCompareExchange64(lir->mir()->access(), addr, oldValue, newValue,
                             output);
}

void CodeGenerator::visitWasmAtomicExchangeI64(LWasmAtomicExchangeI64* lir) {
  Register ptr = ToRegister(lir->ptr());
  Register64 value = ToRegister64(lir->value());
  Register64 output = ToOutRegister64(lir);
  uint32_t offset = lir->mir()->access().offset();

  BaseIndex addr(HeapReg, ptr, TimesOne, offset);
  masm.wasmAtomicExchange64(lir->mir()->access(), addr, value, output);
}

void CodeGenerator::visitWasmAtomicBinopI64(LWasmAtomicBinopI64* lir) {
  Register ptr = ToRegister(lir->ptr());
  Register64 value = ToRegister64(lir->value());
  Register64 output = ToOutRegister64(lir);
#ifdef JS_CODEGEN_MIPS32
  Register64 temp(ToRegister(lir->getTemp(0)), ToRegister(lir->getTemp(1)));
#else
  Register64 temp(ToRegister(lir->getTemp(0)));
#endif
  uint32_t offset = lir->mir()->access().offset();

  BaseIndex addr(HeapReg, ptr, TimesOne, offset);

  masm.wasmAtomicFetchOp64(lir->mir()->access(), lir->mir()->operation(), value,
                           addr, temp, output);
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
