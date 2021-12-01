/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/Lowering-shared-inl.h"

#include "jit/LIR.h"
#include "jit/MIR.h"

#include "vm/SymbolType.h"

using namespace js;
using namespace jit;

bool
LIRGeneratorShared::ShouldReorderCommutative(MDefinition* lhs, MDefinition* rhs, MInstruction* ins)
{
    // lhs and rhs are used by the commutative operator.
    MOZ_ASSERT(lhs->hasDefUses());
    MOZ_ASSERT(rhs->hasDefUses());

    // Ensure that if there is a constant, then it is in rhs.
    if (rhs->isConstant())
        return false;
    if (lhs->isConstant())
        return true;

    // Since clobbering binary operations clobber the left operand, prefer a
    // non-constant lhs operand with no further uses. To be fully precise, we
    // should check whether this is the *last* use, but checking hasOneDefUse()
    // is a decent approximation which doesn't require any extra analysis.
    bool rhsSingleUse = rhs->hasOneDefUse();
    bool lhsSingleUse = lhs->hasOneDefUse();
    if (rhsSingleUse) {
        if (!lhsSingleUse)
            return true;
    } else {
        if (lhsSingleUse)
            return false;
    }

    // If this is a reduction-style computation, such as
    //
    //   sum = 0;
    //   for (...)
    //      sum += ...;
    //
    // put the phi on the left to promote coalescing. This is fairly specific.
    if (rhsSingleUse &&
        rhs->isPhi() &&
        rhs->block()->isLoopHeader() &&
        ins == rhs->toPhi()->getLoopBackedgeOperand())
    {
        return true;
    }

    return false;
}

void
LIRGeneratorShared::ReorderCommutative(MDefinition** lhsp, MDefinition** rhsp, MInstruction* ins)
{
    MDefinition* lhs = *lhsp;
    MDefinition* rhs = *rhsp;

    if (ShouldReorderCommutative(lhs, rhs, ins)) {
        *rhsp = lhs;
        *lhsp = rhs;
    }
}

void
LIRGeneratorShared::visitConstant(MConstant* ins)
{
    if (!IsFloatingPointType(ins->type()) && ins->canEmitAtUses()) {
        emitAtUses(ins);
        return;
    }

    switch (ins->type()) {
      case MIRType::Double:
        define(new(alloc()) LDouble(ins->toDouble()), ins);
        break;
      case MIRType::Float32:
        define(new(alloc()) LFloat32(ins->toFloat32()), ins);
        break;
      case MIRType::Boolean:
        define(new(alloc()) LInteger(ins->toBoolean()), ins);
        break;
      case MIRType::Int32:
        define(new(alloc()) LInteger(ins->toInt32()), ins);
        break;
      case MIRType::Int64:
        defineInt64(new(alloc()) LInteger64(ins->toInt64()), ins);
        break;
      case MIRType::String:
        define(new(alloc()) LPointer(ins->toString()), ins);
        break;
      case MIRType::Symbol:
        define(new(alloc()) LPointer(ins->toSymbol()), ins);
        break;
      case MIRType::Object:
        define(new(alloc()) LPointer(&ins->toObject()), ins);
        break;
      default:
        // Constants of special types (undefined, null) should never flow into
        // here directly. Operations blindly consuming them require a Box.
        MOZ_CRASH("unexpected constant type");
    }
}

void
LIRGeneratorShared::visitWasmFloatConstant(MWasmFloatConstant* ins)
{
    switch (ins->type()) {
      case MIRType::Double:
        define(new(alloc()) LDouble(ins->toDouble()), ins);
        break;
      case MIRType::Float32:
        define(new(alloc()) LFloat32(ins->toFloat32()), ins);
        break;
      default:
        MOZ_CRASH("unexpected constant type");
    }
}

void
LIRGeneratorShared::defineTypedPhi(MPhi* phi, size_t lirIndex)
{
    LPhi* lir = current->getPhi(lirIndex);

    uint32_t vreg = getVirtualRegister();

    phi->setVirtualRegister(vreg);
    lir->setDef(0, LDefinition(vreg, LDefinition::TypeFrom(phi->type())));
    annotate(lir);
}

void
LIRGeneratorShared::lowerTypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex)
{
    MDefinition* operand = phi->getOperand(inputPosition);
    LPhi* lir = block->getPhi(lirIndex);
    lir->setOperand(inputPosition, LUse(operand->virtualRegister(), LUse::ANY));
}

LRecoverInfo*
LIRGeneratorShared::getRecoverInfo(MResumePoint* rp)
{
    if (cachedRecoverInfo_ && cachedRecoverInfo_->mir() == rp)
        return cachedRecoverInfo_;

    LRecoverInfo* recoverInfo = LRecoverInfo::New(gen, rp);
    if (!recoverInfo)
        return nullptr;

    cachedRecoverInfo_ = recoverInfo;
    return recoverInfo;
}

#ifdef DEBUG
bool
LRecoverInfo::OperandIter::canOptimizeOutIfUnused()
{
    MDefinition* ins = **this;

    // We check ins->type() in addition to ins->isUnused() because
    // EliminateDeadResumePointOperands may replace nodes with the constant
    // MagicValue(JS_OPTIMIZED_OUT).
    if ((ins->isUnused() || ins->type() == MIRType::MagicOptimizedOut) &&
        (*it_)->isResumePoint())
    {
        return !(*it_)->toResumePoint()->isObservableOperand(op_);
    }

    return true;
}
#endif

#ifdef JS_NUNBOX32
LSnapshot*
LIRGeneratorShared::buildSnapshot(LInstruction* ins, MResumePoint* rp, BailoutKind kind)
{
    LRecoverInfo* recoverInfo = getRecoverInfo(rp);
    if (!recoverInfo)
        return nullptr;

    LSnapshot* snapshot = LSnapshot::New(gen, recoverInfo, kind);
    if (!snapshot)
        return nullptr;

    size_t index = 0;
    for (LRecoverInfo::OperandIter it(recoverInfo); !it; ++it) {
        // Check that optimized out operands are in eliminable slots.
        MOZ_ASSERT(it.canOptimizeOutIfUnused());

        MDefinition* ins = *it;

        if (ins->isRecoveredOnBailout())
            continue;

        LAllocation* type = snapshot->typeOfSlot(index);
        LAllocation* payload = snapshot->payloadOfSlot(index);
        ++index;

        if (ins->isBox())
            ins = ins->toBox()->getOperand(0);

        // Guards should never be eliminated.
        MOZ_ASSERT_IF(ins->isUnused(), !ins->isGuard());

        // Snapshot operands other than constants should never be
        // emitted-at-uses. Try-catch support depends on there being no
        // code between an instruction and the LOsiPoint that follows it.
        MOZ_ASSERT_IF(!ins->isConstant(), !ins->isEmittedAtUses());

        // The register allocation will fill these fields in with actual
        // register/stack assignments. During code generation, we can restore
        // interpreter state with the given information. Note that for
        // constants, including known types, we record a dummy placeholder,
        // since we can recover the same information, much cleaner, from MIR.
        if (ins->isConstant() || ins->isUnused()) {
            *type = LAllocation();
            *payload = LAllocation();
        } else if (ins->type() != MIRType::Value) {
            *type = LAllocation();
            *payload = use(ins, LUse(LUse::KEEPALIVE));
        } else {
            *type = useType(ins, LUse::KEEPALIVE);
            *payload = usePayload(ins, LUse::KEEPALIVE);
        }
    }

    return snapshot;
}

#elif JS_PUNBOX64

LSnapshot*
LIRGeneratorShared::buildSnapshot(LInstruction* ins, MResumePoint* rp, BailoutKind kind)
{
    LRecoverInfo* recoverInfo = getRecoverInfo(rp);
    if (!recoverInfo)
        return nullptr;

    LSnapshot* snapshot = LSnapshot::New(gen, recoverInfo, kind);
    if (!snapshot)
        return nullptr;

    size_t index = 0;
    for (LRecoverInfo::OperandIter it(recoverInfo); !it; ++it) {
        // Check that optimized out operands are in eliminable slots.
        MOZ_ASSERT(it.canOptimizeOutIfUnused());

        MDefinition* def = *it;

        if (def->isRecoveredOnBailout())
            continue;

        if (def->isBox())
            def = def->toBox()->getOperand(0);

        // Guards should never be eliminated.
        MOZ_ASSERT_IF(def->isUnused(), !def->isGuard());

        // Snapshot operands other than constants should never be
        // emitted-at-uses. Try-catch support depends on there being no
        // code between an instruction and the LOsiPoint that follows it.
        MOZ_ASSERT_IF(!def->isConstant(), !def->isEmittedAtUses());

        LAllocation* a = snapshot->getEntry(index++);

        if (def->isUnused()) {
            *a = LAllocation();
            continue;
        }

        *a = useKeepaliveOrConstant(def);
    }

    return snapshot;
}
#endif

void
LIRGeneratorShared::assignSnapshot(LInstruction* ins, BailoutKind kind)
{
    // assignSnapshot must be called before define/add, since
    // it may add new instructions for emitted-at-use operands.
    MOZ_ASSERT(ins->id() == 0);

    LSnapshot* snapshot = buildSnapshot(ins, lastResumePoint_, kind);
    if (snapshot)
        ins->assignSnapshot(snapshot);
    else
        abort(AbortReason::Alloc, "buildSnapshot failed");
}

void
LIRGeneratorShared::assignSafepoint(LInstruction* ins, MInstruction* mir, BailoutKind kind)
{
    MOZ_ASSERT(!osiPoint_);
    MOZ_ASSERT(!ins->safepoint());

    ins->initSafepoint(alloc());

    MResumePoint* mrp = mir->resumePoint() ? mir->resumePoint() : lastResumePoint_;
    LSnapshot* postSnapshot = buildSnapshot(ins, mrp, kind);
    if (!postSnapshot) {
        abort(AbortReason::Alloc, "buildSnapshot failed");
        return;
    }

    osiPoint_ = new(alloc()) LOsiPoint(ins->safepoint(), postSnapshot);

    if (!lirGraph_.noteNeedsSafepoint(ins))
        abort(AbortReason::Alloc, "noteNeedsSafepoint failed");
}

