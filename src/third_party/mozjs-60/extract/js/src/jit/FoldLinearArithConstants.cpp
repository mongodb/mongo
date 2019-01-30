/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/FoldLinearArithConstants.h"

#include "jit/IonAnalysis.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace jit;

namespace js {
namespace jit {

// Mark this node and its children as RecoveredOnBailout when they are not used.
// The marked nodes will be removed during DCE. Marking as RecoveredOnBailout is
// necessary because the Sink pass is ran before this pass.
static void
markNodesAsRecoveredOnBailout(MDefinition* def)
{
    if (def->hasLiveDefUses() || !DeadIfUnused(def) || !def->canRecoverOnBailout())
        return;

    JitSpew(JitSpew_FLAC, "mark as recovered on bailout: %s%u", def->opName(), def->id());
    def->setRecoveredOnBailoutUnchecked();

    // Recursively mark nodes that do not have multiple uses. This loop is
    // necessary because a node could be an unused right shift zero or an
    // unused add, and both need to be marked as RecoveredOnBailout.
    for (size_t i = 0; i < def->numOperands(); i++)
        markNodesAsRecoveredOnBailout(def->getOperand(i));
}

// Fold AddIs with one variable and two or more constants into one AddI.
static void
AnalyzeAdd(TempAllocator& alloc, MAdd* add)
{
    if (add->specialization() != MIRType::Int32 || add->isRecoveredOnBailout())
        return;

    if (!add->hasUses())
        return;

    JitSpew(JitSpew_FLAC, "analyze add: %s%u", add->opName(), add->id());

    SimpleLinearSum sum = ExtractLinearSum(add);
    if (sum.constant == 0 || !sum.term)
        return;

    // Determine which operand is the constant.
    int idx = add->getOperand(0)->isConstant() ? 0 : 1 ;
    if (add->getOperand(idx)->isConstant()) {
        // Do not replace an add where the outcome is the same add instruction.
        MOZ_ASSERT(add->getOperand(idx)->toConstant()->type() == MIRType::Int32);
        if (sum.term == add->getOperand(1 - idx) ||
            sum.constant == add->getOperand(idx)->toConstant()->toInt32())
        {
            return;
        }
    }

    MInstruction* rhs = MConstant::New(alloc, Int32Value(sum.constant));
    add->block()->insertBefore(add, rhs);

    MAdd* addNew = MAdd::New(alloc, sum.term, rhs, MIRType::Int32, add->truncateKind());

    add->replaceAllLiveUsesWith(addNew);
    add->block()->insertBefore(add, addNew);
    JitSpew(JitSpew_FLAC, "replaced with: %s%u", addNew->opName(), addNew->id());
    JitSpew(JitSpew_FLAC, "and constant: %s%u (%d)", rhs->opName(), rhs->id(), sum.constant);

    // Mark the stale nodes as RecoveredOnBailout since the Sink pass has
    // been run before this pass. DCE will then remove the unused nodes.
    markNodesAsRecoveredOnBailout(add);
}

bool
FoldLinearArithConstants(MIRGenerator* mir, MIRGraph& graph)
{
    for (PostorderIterator block(graph.poBegin()); block != graph.poEnd(); block++) {
        if (mir->shouldCancel("Fold Linear Arithmetic Constants (main loop)"))
            return false;

        for (MInstructionIterator i = block->begin(); i != block->end(); i++) {
            if (!graph.alloc().ensureBallast())
                return false;

            if (mir->shouldCancel("Fold Linear Arithmetic Constants (inner loop)"))
                return false;

            if (i->isAdd())
                AnalyzeAdd(graph.alloc(), i->toAdd());
        }
    }
    return true;
}

} /* namespace jit */
} /* namespace js */
