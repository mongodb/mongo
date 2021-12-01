/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/EffectiveAddressAnalysis.h"

#include "jsnum.h"

#include "jit/IonAnalysis.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace jit;

static void
AnalyzeLsh(TempAllocator& alloc, MLsh* lsh)
{
    if (lsh->specialization() != MIRType::Int32)
        return;

    if (lsh->isRecoveredOnBailout())
        return;

    MDefinition* index = lsh->lhs();
    MOZ_ASSERT(index->type() == MIRType::Int32);

    MConstant* shiftValue = lsh->rhs()->maybeConstantValue();
    if (!shiftValue)
        return;

    if (shiftValue->type() != MIRType::Int32 || !IsShiftInScaleRange(shiftValue->toInt32()))
        return;

    Scale scale = ShiftToScale(shiftValue->toInt32());

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
        if (add->specialization() != MIRType::Int32 || !add->isTruncated())
            break;

        MDefinition* other = add->getOperand(1 - add->indexOf(*use));

        if (MConstant* otherConst = other->maybeConstantValue()) {
            displacement += otherConst->toInt32();
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
        MConstant* otherConst = other->maybeConstantValue();
        if (!otherConst || otherConst->type() != MIRType::Int32)
            return;

        uint32_t bitsClearedByShift = elemSize - 1;
        uint32_t bitsClearedByMask = ~uint32_t(otherConst->toInt32());
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
static void
AnalyzeLoadUnboxedScalar(MLoadUnboxedScalar* load)
{
    if (load->isRecoveredOnBailout())
        return;

    if (!load->getOperand(1)->isAdd())
        return;

    JitSpew(JitSpew_EAA, "analyze: %s%u", load->opName(), load->id());

    MAdd* add = load->getOperand(1)->toAdd();

    if (add->specialization() != MIRType::Int32 || !add->hasUses() ||
        add->truncateKind() != MDefinition::TruncateKind::Truncate)
    {
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
    if (!SafeMul(constant->maybeConstantValue()->toInt32(), storageSize, &c2))
        return;

    int32_t offset = 0;
    if (!SafeAdd(c1, c2, &offset))
        return;

    JitSpew(JitSpew_EAA, "set offset: %d + %d = %d on: %s%u", c1, c2, offset,
            load->opName(), load->id());
    load->setOffsetAdjustment(offset);
    load->replaceOperand(1, node);

    if (!add->hasLiveDefUses() && DeadIfUnused(add) && add->canRecoverOnBailout()) {
        JitSpew(JitSpew_EAA, "mark as recovered on bailout: %s%u",
                add->opName(), add->id());
        add->setRecoveredOnBailoutUnchecked();
    }
}

template<typename AsmJSMemoryAccess>
bool
EffectiveAddressAnalysis::tryAddDisplacement(AsmJSMemoryAccess* ins, int32_t o)
{
#ifdef WASM_HUGE_MEMORY
    // Compute the new offset. Check for overflow.
    uint32_t oldOffset = ins->offset();
    uint32_t newOffset = oldOffset + o;
    if (o < 0 ? (newOffset >= oldOffset) : (newOffset < oldOffset))
        return false;

    // The offset must ultimately be written into the offset immediate of a load
    // or store instruction so don't allow folding of the offset is bigger.
    if (newOffset >= wasm::OffsetGuardLimit)
        return false;

    // Everything checks out. This is the new offset.
    ins->setOffset(newOffset);
    return true;
#else
    return false;
#endif
}

template<typename AsmJSMemoryAccess>
void
EffectiveAddressAnalysis::analyzeAsmJSHeapAccess(AsmJSMemoryAccess* ins)
{
    MDefinition* base = ins->base();

    if (base->isConstant()) {
        // Look for heap[i] where i is a constant offset, and fold the offset.
        // By doing the folding now, we simplify the task of codegen; the offset
        // is always the address mode immediate. This also allows it to avoid
        // a situation where the sum of a constant pointer value and a non-zero
        // offset doesn't actually fit into the address mode immediate.
        int32_t imm = base->toConstant()->toInt32();
        if (imm != 0 && tryAddDisplacement(ins, imm)) {
            MInstruction* zero = MConstant::New(graph_.alloc(), Int32Value(0));
            ins->block()->insertBefore(ins, zero);
            ins->replaceBase(zero);
        }

        // If the index is within the minimum heap length, we can optimize
        // away the bounds check.
        if (imm >= 0) {
            int32_t end = (uint32_t)imm + ins->byteSize();
            if (end >= imm && (uint32_t)end <= mir_->minWasmHeapLength())
                 ins->removeBoundsCheck();
        }
    } else if (base->isAdd()) {
        // Look for heap[a+i] where i is a constant offset, and fold the offset.
        // Alignment masks have already been moved out of the way by the
        // Alignment Mask Analysis pass.
        MDefinition* op0 = base->toAdd()->getOperand(0);
        MDefinition* op1 = base->toAdd()->getOperand(1);
        if (op0->isConstant())
            mozilla::Swap(op0, op1);
        if (op1->isConstant()) {
            int32_t imm = op1->toConstant()->toInt32();
            if (tryAddDisplacement(ins, imm))
                ins->replaceBase(op0);
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
            if (!graph_.alloc().ensureBallast())
                return false;

            // Note that we don't check for MWasmCompareExchangeHeap
            // or MWasmAtomicBinopHeap, because the backend and the OOB
            // mechanism don't support non-zero offsets for them yet
            // (TODO bug 1254935).
            if (i->isLsh())
                AnalyzeLsh(graph_.alloc(), i->toLsh());
            else if (i->isLoadUnboxedScalar())
                AnalyzeLoadUnboxedScalar(i->toLoadUnboxedScalar());
            else if (i->isAsmJSLoadHeap())
                analyzeAsmJSHeapAccess(i->toAsmJSLoadHeap());
            else if (i->isAsmJSStoreHeap())
                analyzeAsmJSHeapAccess(i->toAsmJSStoreHeap());
        }
    }
    return true;
}
