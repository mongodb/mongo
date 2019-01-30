/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/FlowAliasAnalysis.h"

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

class LoopInfo : public TempObject
{
  private:
    LoopInfo* outer_;
    MBasicBlock* loopHeader_;
    MDefinitionVector loopinvariant_;

  public:
    LoopInfo(TempAllocator& alloc, LoopInfo* outer, MBasicBlock* loopHeader)
      : outer_(outer), loopHeader_(loopHeader), loopinvariant_(alloc)
    { }

    MBasicBlock* loopHeader() const {
        return loopHeader_;
    }
    LoopInfo* outer() const {
        return outer_;
    }
    MDefinitionVector& loopinvariant() {
        return loopinvariant_;
    }
};

static bool
KeepBlock(MBasicBlock *block)
{
    // Any block that is predecessor to a loopheader need to be kept.
    // We need it to process possible loop invariant loads.
    if (block->numSuccessors() == 1 && block->getSuccessor(0)->isLoopHeader())
        return true;

#ifdef DEBUG
    // We assume a predecessor to a loopheader has one successor.
    for (size_t i = 0; i < block->numSuccessors(); i++)
        MOZ_ASSERT(!block->getSuccessor(i)->isLoopHeader());
#endif

    return false;
}

class GraphStoreInfo : public TempObject
{
    // The current BlockStoreInfo while iterating the block untill,
    // it contains the store info at the end of the block.
    BlockStoreInfo* current_;

    // Vector with pointer to BlockStoreInfo at the end of the block for every block.
    // Only keeping the info during iteration if needed, else contains nullptr.
    GraphStoreVector stores_;

    // All BlockStoreInfo's that aren't needed anymore and can be reused.
    GraphStoreVector empty_;

  public:
    explicit GraphStoreInfo(TempAllocator& alloc)
      : current_(nullptr),
        stores_(alloc),
        empty_(alloc)
    { }

    bool reserve(size_t num) {
        return stores_.appendN(nullptr, num);
    }

    BlockStoreInfo& current() {
        return *current_;
    }

    void unsetCurrent() {
        current_ = nullptr;
    }

    BlockStoreInfo* newCurrent(TempAllocator& alloc, MBasicBlock* block) {
        BlockStoreInfo *info = nullptr;
        if (empty_.length() != 0) {
            info = empty_.popCopy();
        } else {
            info = (BlockStoreInfo*) alloc.allocate(sizeof(BlockStoreInfo));
            if (!info)
                return nullptr;
            new(info) BlockStoreInfo(alloc);
        }

        stores_[block->id()] = info;
        current_ = info;
        return current_;
    }

    void swap(MBasicBlock* block1, MBasicBlock* block2) {
        BlockStoreInfo* info = stores_[block1->id()];
        stores_[block1->id()] = stores_[block2->id()];
        stores_[block2->id()] = info;
        if (stores_[block1->id()] == current_)
            current_ = stores_[block2->id()];
        else if (stores_[block2->id()] == current_)
            current_ = stores_[block1->id()];
    }

    bool maybeFreePredecessorBlocks(MBasicBlock* block) {
        for (size_t i=0; i < block->numPredecessors(); i++) {

            // For some blocks we cannot free the store info.
            if (KeepBlock(block->getPredecessor(i)))
                continue;

            // Check the given block is the last successor.
            bool release = true;
            for (size_t j = 0; j < block->getPredecessor(i)->numSuccessors(); j++) {
                if (block->getPredecessor(i)->getSuccessor(j)->id() > block->id()) {
                    release = false;
                    break;
                }
            }
            if (release) {
                BlockStoreInfo *info = stores_[block->getPredecessor(i)->id()];
                if (!empty_.append(info))
                    return false;
                info->clear();

                stores_[block->getPredecessor(i)->id()] = nullptr;
            }
        }
        return true;
    }

    BlockStoreInfo& get(MBasicBlock* block) {
        MOZ_ASSERT(stores_[block->id()] != current_);
        return *stores_[block->id()];
    }
};

} // namespace jit
} // namespace js


FlowAliasAnalysis::FlowAliasAnalysis(MIRGenerator* mir, MIRGraph& graph)
  : AliasAnalysisShared(mir, graph),
    loop_(nullptr),
    output_(graph_.alloc()),
    worklist_(graph_.alloc())
{
    stores_ = new(graph_.alloc()) GraphStoreInfo(graph_.alloc());
}

template <typename T>
static bool
AppendToWorklist(MDefinitionVector& worklist, T& stores)
{
    if (!worklist.reserve(worklist.length() + stores.length()))
        return false;

    for (size_t j = 0; j < stores.length(); j++) {
        MOZ_ASSERT(stores[j]);
        if (stores[j]->isInWorklist())
            continue;

        worklist.infallibleAppend(stores[j]);
        stores[j]->setInWorklist();
    }
    return true;
}

static void
SetNotInWorkList(MDefinitionVector& worklist)
{
    for (size_t item = 0; item < worklist.length(); item++)
        worklist[item]->setNotInWorklistUnchecked();
}

static bool
LoadAliasesStore(MDefinition* load, MDefinition* store)
{
    // Always alias first instruction of graph.
    if (store->id() == 0)
        return true;

    // Default to alias control instructions which indicates loops.
    // Control instructions are special, since we need to determine
    // if it aliases anything in the full loop. Which we do lateron.
    if (store->isControlInstruction())
        return true;

    // Check if the alias categories alias eachother.
    if ((load->getAliasSet() & store->getAliasSet()).isNone())
        return false;

    // On any operation that has a specific alias category we can use TI to know
    // the objects operating on don't intersect.
    MDefinition::AliasType mightAlias = AliasAnalysisShared::genericMightAlias(load, store);
    if (mightAlias == MDefinition::AliasType::NoAlias)
        return false;

    // Check if the instruction might alias eachother.
    mightAlias = load->mightAlias(store);
    if (mightAlias == MDefinition::AliasType::NoAlias)
        return false;

    return true;
}

#ifdef JS_JITSPEW
static void
DumpAliasSet(AliasSet set)
{
    Fprinter &print = JitSpewPrinter();

    if (set.flags() == AliasSet::Any) {
        print.printf("Any");
        return;
    }

    bool first = true;
    for (AliasSetIterator iter(set); iter; iter++) {
        if (!first)
            print.printf(", ");
        print.printf("%s", AliasSet::Name(*iter));
        first = false;
    }
}
#endif

#ifdef JS_JITSPEW
static void
DumpStoreList(BlockStoreInfo& stores)
{
    Fprinter &print = JitSpewPrinter();
    if (stores.length() == 0) {
        print.printf("empty");
        return;
    }
    bool first = true;
    for (size_t i = 0; i < stores.length(); i++) {
        if (!first)
            print.printf(", ");
        if (!stores[i]) {
            print.printf("nullptr");
            continue;
        }
        MOZ_ASSERT(stores[i]->isControlInstruction() ||
                   stores[i]->getAliasSet().isStore() ||
                   stores[i]->id() == 0);
        MDefinition::PrintOpcodeName(print, stores[i]->op());
        print.printf("%d", stores[i]->id());
        first = false;
    }
}
#endif

static void
DumpAnalyzeStart()
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Alias) || JitSpewEnabled(JitSpew_AliasSummaries)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpewEnabled(JitSpew_Alias) ? JitSpew_Alias : JitSpew_AliasSummaries);
        print.printf("Running Alias Analysis on graph\n");
    }
#endif
}

static void
DumpBlockStart(MBasicBlock* block)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Alias) || JitSpewEnabled(JitSpew_AliasSummaries)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpewEnabled(JitSpew_Alias)?JitSpew_Alias:JitSpew_AliasSummaries);
        if (block->isLoopHeader())
            print.printf(" Visiting block %d (loopheader)\n", block->id());
        else
            print.printf(" Visiting block %d\n", block->id());
    }
#endif
}

static void
DumpProcessingDeferredLoads(MBasicBlock* loopHeader)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Alias)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpew_Alias);
        print.printf(" Process deferred loads of loop %d\n", loopHeader->id());
    }
#endif
}

static void
DumpBlockSummary(MBasicBlock* block, BlockStoreInfo& blockInfo)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_AliasSummaries)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpew_AliasSummaries);
        print.printf("  Store at end of block: ");
        DumpStoreList(blockInfo);
        print.printf("\n");
    }
#endif
}

static void
DumpStore(MDefinition* store)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Alias)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpew_Alias);
        print.printf("  Store ");
        store->PrintOpcodeName(print, store->op());
        print.printf("%d with flags (", store->id());
        DumpAliasSet(store->getAliasSet());
        print.printf(")\n");
    }
#endif
}

static void
DumpLoad(MDefinition* load)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Alias)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpew_Alias);
        print.printf("  Load ");
        load->PrintOpcodeName(print, load->op());
        print.printf("%d", load->id());
        print.printf(" with flag (");
        DumpAliasSet(load->getAliasSet());
        print.printf(")\n");
    }
#endif
}

static void
DumpLoadOutcome(MDefinition* load, MDefinitionVector& stores, bool defer)
{
#ifdef JS_JITSPEW
    // Spew what we did.
    if (JitSpewEnabled(JitSpew_Alias)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpew_Alias);
        print.printf("   Marked depending on ");
        DumpStoreList(stores);
        if (defer)
            print.printf(" deferred");
        print.printf("\n");
    }
#endif
}

static void
DumpLoopInvariant(MDefinition* load, MBasicBlock* loopheader, bool loopinvariant,
                  MDefinitionVector& loopInvariantDependency)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Alias)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpew_Alias);
        if (!loopinvariant) {
            print.printf("   Determine not loop invariant to loop %d.\n", loopheader->id());
        } else {
            print.printf("   Determine loop invariant to loop %d. Dependendy is now: ", loopheader->id());
            DumpStoreList(loopInvariantDependency);
            print.printf("\n");
        }
    }
#endif
}

static void
DumpImprovement(MDefinition *load, MDefinitionVector& input, MDefinitionVector& output)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Alias)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpew_Alias);
        print.printf("   Improve dependency from %d", load->id());
        DumpStoreList(input);
        print.printf(" to ");
        DumpStoreList(output);
        print.printf("\n");
    }
#endif
}

// Flow Sensitive Alias Analysis.
//
// This pass annotates every load instructions with the last store instruction
// on which it depends in their dependency_ field. For loop variant instructions
// this will depend on the control instruction in the specific loop it cannot
// get hoisted out (if there is no store between start loopheader and
// instruction).
//
// We visit the graph in RPO and keep track of the last stores in that block.
// Upon entering a block we merge the stores information of the predecessors.
// Only loopheaders are different, since we eagerly make it depend on the
// control instruction of the loopheader.
//
// During the iteration of a block we keep a running store dependeny list.
// At the end of the iteration, this will contain the last stores
// (which we keep for successors).
//
// When encountering a store or load we do:
// - Store: we update the current block store info and put a StoreDependency
//          to create a store-chain.
//
// - Load: we take the current block store dependency info and improve that by
//         following the store-chain when encountering not aliasing store. Upon
//         encountering a control instruction (indicates loop) it solely depends on
//         we defer until the loop has been examined.
//
// The algorithm depends on the invariant that both control instructions and effectful
// instructions (stores) are never hoisted.

bool
FlowAliasAnalysis::analyze()
{
    DumpAnalyzeStart();

    // Type analysis may have inserted new instructions. Since this pass depends
    // on the instruction number ordering, all instructions are renumbered.
    uint32_t newId = 0;

    if (!stores_->reserve(graph_.numBlocks()))
        return false;

    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        if (mir->shouldCancel("Alias Analysis (main loop)"))
            return false;

        DumpBlockStart(*block);

        if (!computeBlockStores(*block))
            return false;
        if (!stores_->maybeFreePredecessorBlocks(*block))
            return false;

        for (MPhiIterator def(block->phisBegin()), end(block->phisEnd()); def != end; ++def)
            def->setId(newId++);

        BlockStoreInfo& blockInfo = stores_->current();

        // When the store dependencies is empty it means we have a disconnected
        // graph. Those blocks will never get reached but it is only fixed up
        // after GVN. Don't run AA on those blocks.
        if (blockInfo.length() == 0)
            continue;

        if (block->isLoopHeader())
            loop_ = new(alloc()) LoopInfo(alloc(), loop_, *block);

        for (MInstructionIterator def(block->begin()), end(block->begin(block->lastIns()));
                def != end;
                ++def)
        {
            def->setId(newId++);

            // For the purposes of alias analysis, all recoverable operations
            // are treated as effect free as the memory represented by these
            // operations cannot be aliased by others.
            if (def->canRecoverOnBailout())
                continue;

            AliasSet set = def->getAliasSet();
            if (set.isStore()) {
                if (!processStore(blockInfo, *def))
                    return false;
            } else if (set.isLoad()) {
                if (!processLoad(blockInfo, *def))
                    return false;
            }
        }

        block->lastIns()->setId(newId++);

        if (block->isLoopBackedge()) {
            stores_->unsetCurrent();

            LoopInfo* info = loop_;
            loop_ = loop_->outer();

            if (!processDeferredLoads(info))
                return false;
        }

        DumpBlockSummary(*block, blockInfo);
    }

    spewDependencyList();
    return true;
}

bool
FlowAliasAnalysis::processStore(BlockStoreInfo& blockInfo, MDefinition* store)
{
    // Compute and set dependency information.
    if (!saveStoreDependency(store, blockInfo))
        return false;

    // Update the block store dependency vector.
    blockInfo.clear();
    if (!blockInfo.append(store))
        return false;

    // Spew what we did.
    DumpStore(store);
    return true;
}

bool
FlowAliasAnalysis::processLoad(BlockStoreInfo& blockInfo, MDefinition* load)
{
    DumpLoad(load);

    // Compute dependency information.
    MDefinitionVector& dependencies = blockInfo;
    if (!improveDependency(load, dependencies, output_))
        return false;
    saveLoadDependency(load, output_);

    // If possible defer when better loop information is present.
    if (deferImproveDependency(output_)) {
        if (!loop_->loopinvariant().append(load))
            return false;

        DumpLoadOutcome(load, output_, /* defer = */ true);
        return true;
    }

    DumpLoadOutcome(load, output_, /* defer = */ false);
    return true;
}

bool
FlowAliasAnalysis::processDeferredLoads(LoopInfo* info)
{
    DumpProcessingDeferredLoads(info->loopHeader());
    MOZ_ASSERT(loopIsFinished(info->loopHeader()));

    for (size_t i = 0; i < info->loopinvariant().length(); i++) {
        MDefinition* load = info->loopinvariant()[i];

        DumpLoad(load);

        // Defer load again when this loop still isn't finished yet.
        if (!loopIsFinished(load->dependency()->block())) {
            MOZ_ASSERT(loop_);
            if (!loop_->loopinvariant().append(load))
                return false;

            DumpLoadOutcome(load, output_, /* defer = */ true);
            continue;
        }

        MOZ_ASSERT(load->dependency()->block() == info->loopHeader());
        MDefinition* store = load->dependency();
        load->setDependency(nullptr);

        // Test if this load is loop invariant and if it is,
        // take the dependencies of non-backedge predecessors of the loop header.
        bool loopinvariant;
        if (!isLoopInvariant(load, store, &loopinvariant))
            return false;
        MDefinitionVector &loopInvariantDependency =
            stores_->get(store->block()->loopPredecessor());

        DumpLoopInvariant(load, info->loopHeader(), /* loopinvariant = */ loopinvariant,
                          loopInvariantDependency);

        // When the store dependencies is empty it means we have a disconnected
        // graph. Those blocks will never get reached but it is only fixed up
        // after GVN. Don't improve dependency for those loads.
        if (loopInvariantDependency.length() == 0) {
            load->setDependency(store);
            continue;
        }

        if (loopinvariant) {
            if (!improveDependency(load, loopInvariantDependency, output_))
                return false;
            saveLoadDependency(load, output_);

            // If possible defer when better loop information is present.
            if (deferImproveDependency(output_)) {
                if (!loop_->loopinvariant().append(load))
                    return false;

                DumpLoadOutcome(load, output_, /* defer = */ true);
            } else {
                DumpLoadOutcome(load, output_, /* defer = */ false);
            }

        } else {
            load->setDependency(store);

#ifdef JS_JITSPEW
            output_.clear();
            if (!output_.append(store))
                return false;
            DumpLoadOutcome(load, output_, /* defer = */ false);
#endif
        }

    }
    return true;
}

// Given a load instruction and an initial store dependency list,
// find the most accurate store dependency list.
bool
FlowAliasAnalysis::improveDependency(MDefinition* load, MDefinitionVector& inputStores,
                                     MDefinitionVector& outputStores)
{
    MOZ_ASSERT(inputStores.length() > 0);
    outputStores.clear();
    if (!outputStores.appendAll(inputStores))
        return false;

    bool improved = false;
    bool adjusted = true;
    while (adjusted) {
        adjusted = false;
        if (!improveNonAliasedStores(load, outputStores, outputStores, &improved))
            return false;
        MOZ_ASSERT(outputStores.length() != 0);
        if (!improveStoresInFinishedLoops(load, outputStores, &adjusted))
            return false;
        if (adjusted)
            improved = true;
    }

    if (improved)
        DumpImprovement(load, inputStores, outputStores);

    return true;
}

// For every real store dependencies, follow the chain of stores to find the
// unique set of 'might alias' store dependencies.
bool
FlowAliasAnalysis::improveNonAliasedStores(MDefinition* load, MDefinitionVector& inputStores,
                                           MDefinitionVector& outputStores, bool* improved,
                                           bool onlyControlInstructions)
{
    MOZ_ASSERT(worklist_.length() == 0);
    if (!AppendToWorklist(worklist_, inputStores))
        return false;
    outputStores.clear();

    for (size_t i = 0; i < worklist_.length(); i++) {
        MOZ_ASSERT(worklist_[i]);

        if (!LoadAliasesStore(load, worklist_[i])) {
            StoreDependency* dep = worklist_[i]->storeDependency();
            MOZ_ASSERT(dep);
            MOZ_ASSERT(dep->get().length() > 0);

            if (!AppendToWorklist(worklist_, dep->get()))
                return false;

            *improved = true;
            continue;
        }

        if (onlyControlInstructions && !worklist_[i]->isControlInstruction()) {
            outputStores.clear();
            break;
        }
        if (!outputStores.append(worklist_[i]))
            return false;
    }

    SetNotInWorkList(worklist_);
    worklist_.clear();
    return true;
}

// Given a load instruction and an initial store dependency list,
// find the most accurate store dependency list with only control instructions.
// Returns an empty output list, when there was a non control instructions
// that couldn't get improved to a control instruction.
bool
FlowAliasAnalysis::improveLoopDependency(MDefinition* load, MDefinitionVector& inputStores,
                                         MDefinitionVector& outputStores)
{
    outputStores.clear();
    if (!outputStores.appendAll(inputStores))
        return false;

    bool improved = false;
    bool adjusted = true;
    while (adjusted) {
        adjusted = false;
        if (!improveNonAliasedStores(load, outputStores, outputStores, &improved,
                                      /* onlyControlInstructions = */ true))
        {
            return false;
        }
        if (outputStores.length() == 0)
            return true;
        if (!improveStoresInFinishedLoops(load, outputStores, &adjusted))
            return false;
        if (adjusted)
            improved = true;
    }

    if (improved)
        DumpImprovement(load, inputStores, outputStores);

    return true;
}

// For every control instruction in the output we find out if the load is loop
// invariant to that loop. When it is, improve the output dependency store,
// by pointing to the stores before the loop.
bool
FlowAliasAnalysis::improveStoresInFinishedLoops(MDefinition* load, MDefinitionVector& stores,
                                                bool* improved)
{
    for (size_t i = 0; i < stores.length(); i++) {
        if (!stores[i]->isControlInstruction())
            continue;
        if (!stores[i]->block()->isLoopHeader())
            continue;

        MOZ_ASSERT(!stores[i]->storeDependency());

        if (!loopIsFinished(stores[i]->block()))
            continue;

        if (load->dependency() == stores[i])
            continue;

        bool loopinvariant;
        if (!isLoopInvariant(load, stores[i], &loopinvariant))
            return false;
        if (!loopinvariant)
            continue;

        MBasicBlock* pred = stores[i]->block()->loopPredecessor();
        BlockStoreInfo& predInfo = stores_->get(pred);

        MOZ_ASSERT(predInfo.length() > 0);
        stores[i] = predInfo[0];
        for (size_t j = 1; j < predInfo.length(); j++) {
            if (!stores.append(predInfo[j]))
                return false;
        }

        *improved = true;
    }

    return true;
}

bool
FlowAliasAnalysis::deferImproveDependency(MDefinitionVector& stores)
{
    // Look if the store depends only on 1 non finished loop.
    // In that case we will defer until that loop has finished.
    return loop_ && stores.length() == 1 &&
           stores[0]->isControlInstruction() &&
           stores[0]->block()->isLoopHeader() &&
           !loopIsFinished(stores[0]->block());
}

void
FlowAliasAnalysis::saveLoadDependency(MDefinition* load, MDefinitionVector& dependencies)
{
    MOZ_ASSERT(dependencies.length() > 0);

    // For now we only save the last store before the load for other passes.
    // That means the store with the maximum id.
    MDefinition* max = dependencies[0];
    MDefinition* maxNonControl = nullptr;
    for (size_t i = 0; i < dependencies.length(); i++) {
        MDefinition* ins = dependencies[i];
        if (max->id() < ins->id())
            max = ins;
        if (!ins->isControlInstruction()) {
            if (!maxNonControl || maxNonControl->id() < ins->id())
                maxNonControl = ins;
        }
    }

    // For loop variant loads with no stores between loopheader and the load,
    // the control instruction of the loop header is returned.
    // That id is higher than any store inside the loopheader itself.
    // Fix for dependency on item in loopheader, but before the "test".
    // Which would assume it depends on the loop itself.
    if (maxNonControl != max && maxNonControl) {
        if (maxNonControl->block() == max->block())
            max = maxNonControl;
    }

    load->setDependency(max);
}

bool
FlowAliasAnalysis::saveStoreDependency(MDefinition* ins, BlockStoreInfo& prevStores)
{
    // To form a store dependency chain, we store the previous last dependencies
    // in the current store.

    StoreDependency* dependency = new(alloc().fallible()) StoreDependency(alloc());
    if (!dependency)
        return false;
    if (!dependency->init(prevStores))
        return false;

    ins->setStoreDependency(dependency);
    return true;
}

// Returns if loop has been processed
// and has complete backedge stores information.
bool
FlowAliasAnalysis::loopIsFinished(MBasicBlock* loopheader)
{
    MOZ_ASSERT(loopheader->isLoopHeader());

    if (!loop_)
        return true;

    return loopheader->backedge()->id() <
           loop_->loopHeader()->backedge()->id();
}


// Determines if a load is loop invariant.
//
// Get the last store dependencies of the backedge of the loop and follow
// the store chain until finding the aliased stores. Make sure the computed
// aliased stores is only the loop control instruction or control instructions
// of loops it is also loop invariant. Only in that case the load is
// definitely loop invariant.
bool
FlowAliasAnalysis::isLoopInvariant(MDefinition* load, MDefinition* store, bool* loopinvariant)
{
    MOZ_ASSERT(store->isControlInstruction());
    MOZ_ASSERT(!store->storeDependency());
    MOZ_ASSERT(store->block()->isLoopHeader());
    MOZ_ASSERT(loopIsFinished(store->block()));

    *loopinvariant = false;
    MBasicBlock* backedge = store->block()->backedge();
    MDefinitionVector output(alloc());

    // To make sure the improve dependency stops at this loop,
    // set the loop control instruction as dependency.
    MDefinition* olddep = load->dependency();
    load->setDependency(store);
    if (!improveLoopDependency(load, stores_->get(backedge), output))
        return false;
    load->setDependency(olddep);

    if (output.length() == 0)
        return true;

    for (size_t i = 0; i < output.length(); i++) {
        if (output[i]->storeDependency())
            return true;

        if (!output[i]->isControlInstruction())
            return true;
        if (!output[i]->block()->isLoopHeader())
            return true;

        if (output[i] == store)
            continue;

        return true;
    }

    *loopinvariant = true;
    return true;
}

// Compute the store dependencies at the start of this MBasicBlock.
bool
FlowAliasAnalysis::computeBlockStores(MBasicBlock* block)
{
    BlockStoreInfo* blockInfo = stores_->newCurrent(alloc(), block);
    if (!blockInfo)
        return false;

    // First and osr block depends on the first instruction.
    if (block == graph_.entryBlock() || block == graph_.osrBlock()) {
        MDefinition* firstIns = *block->begin();
        if (!blockInfo->append(firstIns))
            return false;
        return true;
    }

    // For loopheaders we take the loopheaders control instruction.
    // That is not moveable and easy is to detect.
    if (block->isLoopHeader()) {
        if (!blockInfo->append(block->lastIns()))
            return false;
        return true;
    }


    // Optimization for consecutive blocks.
    if (block->numPredecessors() == 1) {
        MBasicBlock* pred = block->getPredecessor(0);
        if (pred->numSuccessors() == 1) {
            stores_->swap(block, pred);
            return true;
        }
        MOZ_ASSERT (pred->numSuccessors() > 1);
        BlockStoreInfo& predInfo = stores_->get(pred);
        return blockInfo->appendAll(predInfo);
    }

    // Heuristic: in most cases having more than 5 predecessors,
    // increases the number of dependencies too much to still be able
    // to do an optimization. Therefore don't do the merge work.
    // For simplicity we take an non-dominant always existing instruction.
    // That way we cannot accidentally move instructions depending on it.
    if (block->numPredecessors() > 5) {
        if (!blockInfo->append(block->getPredecessor(0)->lastIns()))
            return false;
        return true;
    }

    // Merging of multiple predecessors.
    for (size_t pred = 0; pred < block->numPredecessors(); pred++) {
        BlockStoreInfo& predInfo = stores_->get(block->getPredecessor(pred));
        if (!AppendToWorklist(*blockInfo, predInfo))
            return false;
    }
    SetNotInWorkList(*blockInfo);

    return true;
}
