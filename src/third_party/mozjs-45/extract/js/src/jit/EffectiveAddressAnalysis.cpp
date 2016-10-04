/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/EffectiveAddressAnalysis.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace jit;

static void
AnalyzeLsh(TempAllocator& alloc, MLsh* lsh)
{
    if (lsh->specialization() != MIRType_Int32)
        return;

    if (lsh->isRecoveredOnBailout())
        return;

    MDefinition* index = lsh->lhs();
    MOZ_ASSERT(index->type() == MIRType_Int32);

    MDefinition* shift = lsh->rhs();
    if (!shift->isConstantValue())
        return;

    Value shiftValue = shift->constantValue();
    if (!shiftValue.isInt32() || !IsShiftInScaleRange(shiftValue.toInt32()))
        return;

    Scale scale = ShiftToScale(shiftValue.toInt32());

    int32_t displacement = 0;
    MInstruction* last = lsh;
    MDefinition* base = nullptr;
    while (true) {
        if (!last->hasOneUse())
            break;

        MUseIterator use = last->usesBegin();
        if (!use->consumer()->isDefinition() || !use->consumer()->toDefinition()->isAdd())
            break;

        MAdd* add = use->consumer()->toDefinition()->toAdd();
        if (add->specialization() != MIRType_Int32 || !add->isTruncated())
            break;

        MDefinition* other = add->getOperand(1 - add->indexOf(*use));

        if (other->isConstantValue()) {
            displacement += other->constantValue().toInt32();
        } else {
            if (base)
                break;
            base = other;
        }

        last = add;
        if (last->isRecoveredOnBailout())
            return;
    }

    if (!base) {
        uint32_t elemSize = 1 << ScaleToShift(scale);
        if (displacement % elemSize != 0)
            return;

        if (!last->hasOneUse())
            return;

        MUseIterator use = last->usesBegin();
        if (!use->consumer()->isDefinition() || !use->consumer()->toDefinition()->isBitAnd())
            return;

        MBitAnd* bitAnd = use->consumer()->toDefinition()->toBitAnd();
        if (bitAnd->isRecoveredOnBailout())
            return;

        MDefinition* other = bitAnd->getOperand(1 - bitAnd->indexOf(*use));
        if (!other->isConstantValue() || !other->constantValue().isInt32())
            return;

        uint32_t bitsClearedByShift = elemSize - 1;
        uint32_t bitsClearedByMask = ~uint32_t(other->constantValue().toInt32());
        if ((bitsClearedByShift & bitsClearedByMask) != bitsClearedByMask)
            return;

        bitAnd->replaceAllUsesWith(last);
        return;
    }

    if (base->isRecoveredOnBailout())
        return;

    MEffectiveAddress* eaddr = MEffectiveAddress::New(alloc, base, index, scale, displacement);
    last->replaceAllUsesWith(eaddr);
    last->block()->insertAfter(last, eaddr);
}

template<typename MAsmJSHeapAccessType>
bool
EffectiveAddressAnalysis::tryAddDisplacement(MAsmJSHeapAccessType* ins, int32_t o)
{
    // Compute the new offset. Check for overflow and negative. In theory it
    // ought to be possible to support negative offsets, but it'd require
    // more elaborate bounds checking mechanisms than we currently have.
    MOZ_ASSERT(ins->offset() >= 0);
    int32_t newOffset = uint32_t(ins->offset()) + o;
    if (newOffset < 0)
        return false;

    // Compute the new offset to the end of the access. Check for overflow
    // and negative here also.
    int32_t newEnd = uint32_t(newOffset) + ins->byteSize();
    if (newEnd < 0)
        return false;
    MOZ_ASSERT(uint32_t(newEnd) >= uint32_t(newOffset));

    // Determine the range of valid offsets which can be folded into this
    // instruction and check whether our computed offset is within that range.
    size_t range = mir_->foldableOffsetRange(ins);
    if (size_t(newEnd) > range)
        return false;

    // Everything checks out. This is the new offset.
    ins->setOffset(newOffset);
    return true;
}

template<typename MAsmJSHeapAccessType>
void
EffectiveAddressAnalysis::analyzeAsmHeapAccess(MAsmJSHeapAccessType* ins)
{
    MDefinition* ptr = ins->ptr();

    if (ptr->isConstantValue()) {
        // Look for heap[i] where i is a constant offset, and fold the offset.
        // By doing the folding now, we simplify the task of codegen; the offset
        // is always the address mode immediate. This also allows it to avoid
        // a situation where the sum of a constant pointer value and a non-zero
        // offset doesn't actually fit into the address mode immediate.
        int32_t imm = ptr->constantValue().toInt32();
        if (imm != 0 && tryAddDisplacement(ins, imm)) {
            MInstruction* zero = MConstant::New(graph_.alloc(), Int32Value(0));
            ins->block()->insertBefore(ins, zero);
            ins->replacePtr(zero);
        }
    } else if (ptr->isAdd()) {
        // Look for heap[a+i] where i is a constant offset, and fold the offset.
        // Alignment masks have already been moved out of the way by the
        // Alignment Mask Analysis pass.
        MDefinition* op0 = ptr->toAdd()->getOperand(0);
        MDefinition* op1 = ptr->toAdd()->getOperand(1);
        if (op0->isConstantValue())
            mozilla::Swap(op0, op1);
        if (op1->isConstantValue()) {
            int32_t imm = op1->constantValue().toInt32();
            if (tryAddDisplacement(ins, imm))
                ins->replacePtr(op0);
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
//   asmload((x << {0,1,2,3}) & mask)            (where mask is redundant with shift)
//   asmload(((x << {0,1,2,3}) + imm32) & mask)  (where mask is redundant with shift + imm32)
// into a single asmload instruction (and for asmstore too).
//
// Additionally, we should consider the general forms:
//   truncate(x + y + imm32)
//   truncate((y << {0,1,2,3}) + imm32)
bool
EffectiveAddressAnalysis::analyze()
{
    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        for (MInstructionIterator i = block->begin(); i != block->end(); i++) {
            // Note that we don't check for MAsmJSCompareExchangeHeap
            // or MAsmJSAtomicBinopHeap, because the backend and the OOB
            // mechanism don't support non-zero offsets for them yet.
            if (i->isLsh())
                AnalyzeLsh(graph_.alloc(), i->toLsh());
            else if (i->isAsmJSLoadHeap())
                analyzeAsmHeapAccess(i->toAsmJSLoadHeap());
            else if (i->isAsmJSStoreHeap())
                analyzeAsmHeapAccess(i->toAsmJSStoreHeap());
        }
    }
    return true;
}
