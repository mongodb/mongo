/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/EffectiveAddressAnalysis.h"

#include "jit/IonAnalysis.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"
#include "util/CheckedArithmetic.h"

using namespace js;
using namespace jit;

static void AnalyzeLsh(TempAllocator& alloc, MLsh* lsh) {
  if (lsh->type() != MIRType::Int32) {
    return;
  }

  if (lsh->isRecoveredOnBailout()) {
    return;
  }

  MDefinition* index = lsh->lhs();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  MConstant* shiftValue = lsh->rhs()->maybeConstantValue();
  if (!shiftValue) {
    return;
  }

  if (shiftValue->type() != MIRType::Int32 ||
      !IsShiftInScaleRange(shiftValue->toInt32())) {
    return;
  }

  Scale scale = ShiftToScale(shiftValue->toInt32());

  int32_t displacement = 0;
  MInstruction* last = lsh;
  MDefinition* base = nullptr;
  while (true) {
    if (!last->hasOneUse()) {
      break;
    }

    MUseIterator use = last->usesBegin();
    if (!use->consumer()->isDefinition() ||
        !use->consumer()->toDefinition()->isAdd()) {
      break;
    }

    MAdd* add = use->consumer()->toDefinition()->toAdd();
    if (add->type() != MIRType::Int32 || !add->isTruncated()) {
      break;
    }

    MDefinition* other = add->getOperand(1 - add->indexOf(*use));

    if (MConstant* otherConst = other->maybeConstantValue()) {
      displacement += otherConst->toInt32();
    } else {
      if (base) {
        break;
      }
      base = other;
    }

    last = add;
    if (last->isRecoveredOnBailout()) {
      return;
    }
  }

  if (!base) {
    uint32_t elemSize = 1 << ScaleToShift(scale);
    if (displacement % elemSize != 0) {
      return;
    }

    if (!last->hasOneUse()) {
      return;
    }

    MUseIterator use = last->usesBegin();
    if (!use->consumer()->isDefinition() ||
        !use->consumer()->toDefinition()->isBitAnd()) {
      return;
    }

    MBitAnd* bitAnd = use->consumer()->toDefinition()->toBitAnd();
    if (bitAnd->isRecoveredOnBailout()) {
      return;
    }

    MDefinition* other = bitAnd->getOperand(1 - bitAnd->indexOf(*use));
    MConstant* otherConst = other->maybeConstantValue();
    if (!otherConst || otherConst->type() != MIRType::Int32) {
      return;
    }

    uint32_t bitsClearedByShift = elemSize - 1;
    uint32_t bitsClearedByMask = ~uint32_t(otherConst->toInt32());
    if ((bitsClearedByShift & bitsClearedByMask) != bitsClearedByMask) {
      return;
    }

    bitAnd->replaceAllUsesWith(last);
    return;
  }

  if (base->isRecoveredOnBailout()) {
    return;
  }

  MEffectiveAddress* eaddr =
      MEffectiveAddress::New(alloc, base, index, scale, displacement);
  last->replaceAllUsesWith(eaddr);
  last->block()->insertAfter(last, eaddr);
}

// Transform:
//
//   [AddI]
//   addl       $9, %esi
//   [LoadUnboxedScalar]
//   movsd      0x0(%rbx,%rsi,8), %xmm4
//
// into:
//
//   [LoadUnboxedScalar]
//   movsd      0x48(%rbx,%rsi,8), %xmm4
//
// This is possible when the AddI is only used by the LoadUnboxedScalar opcode.
static void AnalyzeLoadUnboxedScalar(MLoadUnboxedScalar* load) {
  if (load->isRecoveredOnBailout()) {
    return;
  }

  if (!load->getOperand(1)->isAdd()) {
    return;
  }

  JitSpew(JitSpew_EAA, "analyze: %s%u", load->opName(), load->id());

  MAdd* add = load->getOperand(1)->toAdd();

  if (add->type() != MIRType::Int32 || !add->hasUses() ||
      add->truncateKind() != TruncateKind::Truncate) {
    return;
  }

  MDefinition* lhs = add->lhs();
  MDefinition* rhs = add->rhs();
  MDefinition* constant = nullptr;
  MDefinition* node = nullptr;

  if (lhs->isConstant()) {
    constant = lhs;
    node = rhs;
  } else if (rhs->isConstant()) {
    constant = rhs;
    node = lhs;
  } else
    return;

  MOZ_ASSERT(constant->type() == MIRType::Int32);

  size_t storageSize = Scalar::byteSize(load->storageType());
  int32_t c1 = load->offsetAdjustment();
  int32_t c2 = 0;
  if (!SafeMul(constant->maybeConstantValue()->toInt32(), storageSize, &c2)) {
    return;
  }

  int32_t offset = 0;
  if (!SafeAdd(c1, c2, &offset)) {
    return;
  }

  JitSpew(JitSpew_EAA, "set offset: %d + %d = %d on: %s%u", c1, c2, offset,
          load->opName(), load->id());
  load->setOffsetAdjustment(offset);
  load->replaceOperand(1, node);

  if (!add->hasLiveDefUses() && DeadIfUnused(add) &&
      add->canRecoverOnBailout()) {
    JitSpew(JitSpew_EAA, "mark as recovered on bailout: %s%u", add->opName(),
            add->id());
    add->setRecoveredOnBailoutUnchecked();
  }
}

template <typename AsmJSMemoryAccess>
void EffectiveAddressAnalysis::analyzeAsmJSHeapAccess(AsmJSMemoryAccess* ins) {
  MDefinition* base = ins->base();

  if (base->isConstant()) {
    // If the index is within the minimum heap length, we can optimize away the
    // bounds check.  Asm.js accesses always have an int32 base, the memory is
    // always a memory32.
    int32_t imm = base->toConstant()->toInt32();
    if (imm >= 0) {
      int32_t end = (uint32_t)imm + ins->byteSize();
      if (end >= imm && (uint32_t)end <= mir_->minWasmMemory0Length()) {
        ins->removeBoundsCheck();
      }
    }
  }
}

// This analysis converts patterns of the form:
//   truncate(x + (y << {0,1,2,3}))
//   truncate(x + (y << {0,1,2,3}) + imm32)
// into a single lea instruction, and patterns of the form:
//   asmload(x + imm32)
//   asmload(x << {0,1,2,3})
//   asmload((x << {0,1,2,3}) + imm32)
//   asmload((x << {0,1,2,3}) & mask)            (where mask is redundant
//                                                with shift)
//   asmload(((x << {0,1,2,3}) + imm32) & mask)  (where mask is redundant
//                                                with shift + imm32)
// into a single asmload instruction (and for asmstore too).
//
// Additionally, we should consider the general forms:
//   truncate(x + y + imm32)
//   truncate((y << {0,1,2,3}) + imm32)
bool EffectiveAddressAnalysis::analyze() {
  JitSpew(JitSpew_EAA, "Begin");
  for (ReversePostorderIterator block(graph_.rpoBegin());
       block != graph_.rpoEnd(); block++) {
    for (MInstructionIterator i = block->begin(); i != block->end(); i++) {
      if (!graph_.alloc().ensureBallast()) {
        return false;
      }

      // Note that we don't check for MWasmCompareExchangeHeap
      // or MWasmAtomicBinopHeap, because the backend and the OOB
      // mechanism don't support non-zero offsets for them yet
      // (TODO bug 1254935).
      if (i->isLsh()) {
        AnalyzeLsh(graph_.alloc(), i->toLsh());
      } else if (i->isLoadUnboxedScalar()) {
        AnalyzeLoadUnboxedScalar(i->toLoadUnboxedScalar());
      } else if (i->isAsmJSLoadHeap()) {
        analyzeAsmJSHeapAccess(i->toAsmJSLoadHeap());
      } else if (i->isAsmJSStoreHeap()) {
        analyzeAsmJSHeapAccess(i->toAsmJSStoreHeap());
      }
    }
  }
  return true;
}
