/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AliasAnalysis.h"

#include <stdio.h>

#include "jit/AliasAnalysisShared.h"
#include "jit/Ion.h"
#include "jit/IonBuilder.h"
#include "jit/JitSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"

#include "vm/Printer.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

class LoopAliasInfo : public TempObject
{
  private:
    LoopAliasInfo* outer_;
    MBasicBlock* loopHeader_;
    MInstructionVector invariantLoads_;

  public:
    LoopAliasInfo(TempAllocator& alloc, LoopAliasInfo* outer, MBasicBlock* loopHeader)
      : outer_(outer), loopHeader_(loopHeader), invariantLoads_(alloc)
    { }

    MBasicBlock* loopHeader() const {
        return loopHeader_;
    }
    LoopAliasInfo* outer() const {
        return outer_;
    }
    bool addInvariantLoad(MInstruction* ins) {
        return invariantLoads_.append(ins);
    }
    const MInstructionVector& invariantLoads() const {
        return invariantLoads_;
    }
    MInstruction* firstInstruction() const {
        return *loopHeader_->begin();
    }
};

} // namespace jit
} // namespace js

AliasAnalysis::AliasAnalysis(MIRGenerator* mir, MIRGraph& graph)
  : AliasAnalysisShared(mir, graph),
    loop_(nullptr)
{
}

// Whether there might be a path from src to dest, excluding loop backedges. This is
// approximate and really ought to depend on precomputed reachability information.
static inline bool
BlockMightReach(MBasicBlock* src, MBasicBlock* dest)
{
    while (src->id() <= dest->id()) {
        if (src == dest)
            return true;
        switch (src->numSuccessors()) {
          case 0:
            return false;
          case 1: {
            MBasicBlock* successor = src->getSuccessor(0);
            if (successor->id() <= src->id())
                return true; // Don't iloop.
            src = successor;
            break;
          }
          default:
            return true;
        }
    }
    return false;
}

static void
IonSpewDependency(MInstruction* load, MInstruction* store, const char* verb, const char* reason)
{
    if (!JitSpewEnabled(JitSpew_Alias))
        return;

    Fprinter& out = JitSpewPrinter();
    out.printf("Load ");
    load->printName(out);
    out.printf(" %s on store ", verb);
    store->printName(out);
    out.printf(" (%s)\n", reason);
}

static void
IonSpewAliasInfo(const char* pre, MInstruction* ins, const char* post)
{
    if (!JitSpewEnabled(JitSpew_Alias))
        return;

    Fprinter& out = JitSpewPrinter();
    out.printf("%s ", pre);
    ins->printName(out);
    out.printf(" %s\n", post);
}

// This pass annotates every load instruction with the last store instruction
// on which it depends. The algorithm is optimistic in that it ignores explicit
// dependencies and only considers loads and stores.
//
// Loads inside loops only have an implicit dependency on a store before the
// loop header if no instruction inside the loop body aliases it. To calculate
// this efficiently, we maintain a list of maybe-invariant loads and the combined
// alias set for all stores inside the loop. When we see the loop's backedge, this
// information is used to mark every load we wrongly assumed to be loop invariant as
// having an implicit dependency on the last instruction of the loop header, so that
// it's never moved before the loop header.
//
// The algorithm depends on the invariant that both control instructions and effectful
// instructions (stores) are never hoisted.
bool
AliasAnalysis::analyze()
{
    Vector<MInstructionVector, AliasSet::NumCategories, JitAllocPolicy> stores(alloc());

    // Initialize to the first instruction.
    MInstruction* firstIns = *graph_.entryBlock()->begin();
    for (unsigned i = 0; i < AliasSet::NumCategories; i++) {
        MInstructionVector defs(alloc());
        if (!defs.append(firstIns))
            return false;
        if (!stores.append(Move(defs)))
            return false;
    }

    // Type analysis may have inserted new instructions. Since this pass depends
    // on the instruction number ordering, all instructions are renumbered.
    uint32_t newId = 0;

    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        if (mir->shouldCancel("Alias Analysis (main loop)"))
            return false;

        if (block->isLoopHeader()) {
            JitSpew(JitSpew_Alias, "Processing loop header %d", block->id());
            loop_ = new(alloc().fallible()) LoopAliasInfo(alloc(), loop_, *block);
            if (!loop_)
                return false;
        }

        for (MPhiIterator def(block->phisBegin()), end(block->phisEnd()); def != end; ++def)
            def->setId(newId++);

        for (MInstructionIterator def(block->begin()), end(block->begin(block->lastIns()));
             def != end;
             ++def)
        {
            def->setId(newId++);

            AliasSet set = def->getAliasSet();
            if (set.isNone())
                continue;

            // For the purposes of alias analysis, all recoverable operations
            // are treated as effect free as the memory represented by these
            // operations cannot be aliased by others.
            if (def->canRecoverOnBailout())
                continue;

            if (set.isStore()) {
                for (AliasSetIterator iter(set); iter; iter++) {
                    if (!stores[*iter].append(*def))
                        return false;
                }

                if (JitSpewEnabled(JitSpew_Alias)) {
                    Fprinter& out = JitSpewPrinter();
                    out.printf("Processing store ");
                    def->printName(out);
                    out.printf(" (flags %x)\n", set.flags());
                }
            } else {
                // Find the most recent store on which this instruction depends.
                MInstruction* lastStore = firstIns;

                for (AliasSetIterator iter(set); iter; iter++) {
                    MInstructionVector& aliasedStores = stores[*iter];
                    for (int i = aliasedStores.length() - 1; i >= 0; i--) {
                        MInstruction* store = aliasedStores[i];
                        if (genericMightAlias(*def, store) != MDefinition::AliasType::NoAlias &&
                            def->mightAlias(store) != MDefinition::AliasType::NoAlias &&
                            BlockMightReach(store->block(), *block))
                        {
                            if (lastStore->id() < store->id())
                                lastStore = store;
                            break;
                        }
                    }
                }

                def->setDependency(lastStore);
                IonSpewDependency(*def, lastStore, "depends", "");

                // If the last store was before the current loop, we assume this load
                // is loop invariant. If a later instruction writes to the same location,
                // we will fix this at the end of the loop.
                if (loop_ && lastStore->id() < loop_->firstInstruction()->id()) {
                    if (!loop_->addInvariantLoad(*def))
                        return false;
                }
            }
        }

        // Renumber the last instruction, as the analysis depends on this and the order.
        block->lastIns()->setId(newId++);

        if (block->isLoopBackedge()) {
            MOZ_ASSERT(loop_->loopHeader() == block->loopHeaderOfBackedge());
            JitSpew(JitSpew_Alias, "Processing loop backedge %d (header %d)", block->id(),
                    loop_->loopHeader()->id());
            LoopAliasInfo* outerLoop = loop_->outer();
            MInstruction* firstLoopIns = *loop_->loopHeader()->begin();

            const MInstructionVector& invariant = loop_->invariantLoads();

            for (unsigned i = 0; i < invariant.length(); i++) {
                MInstruction* ins = invariant[i];
                AliasSet set = ins->getAliasSet();
                MOZ_ASSERT(set.isLoad());

                bool hasAlias = false;
                for (AliasSetIterator iter(set); iter; iter++) {
                    MInstructionVector& aliasedStores = stores[*iter];
                    for (int i = aliasedStores.length() - 1;; i--) {
                        MInstruction* store = aliasedStores[i];
                        if (store->id() < firstLoopIns->id())
                            break;
                        if (genericMightAlias(ins, store) != MDefinition::AliasType::NoAlias &&
                            ins->mightAlias(store) != MDefinition::AliasType::NoAlias)
                        {
                            hasAlias = true;
                            IonSpewDependency(ins, store, "aliases", "store in loop body");
                            break;
                        }
                    }
                    if (hasAlias)
                        break;
                }

                if (hasAlias) {
                    // This instruction depends on stores inside the loop body. Mark it as having a
                    // dependency on the last instruction of the loop header. The last instruction is a
                    // control instruction and these are never hoisted.
                    MControlInstruction* controlIns = loop_->loopHeader()->lastIns();
                    IonSpewDependency(ins, controlIns, "depends", "due to stores in loop body");
                    ins->setDependency(controlIns);
                } else {
                    IonSpewAliasInfo("Load", ins, "does not depend on any stores in this loop");

                    if (outerLoop && ins->dependency()->id() < outerLoop->firstInstruction()->id()) {
                        IonSpewAliasInfo("Load", ins, "may be invariant in outer loop");
                        if (!outerLoop->addInvariantLoad(ins))
                            return false;
                    }
                }
            }
            loop_ = loop_->outer();
        }
    }

    spewDependencyList();

    MOZ_ASSERT(loop_ == nullptr);
    return true;
}
