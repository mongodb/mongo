/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ValueNumbering_h
#define jit_ValueNumbering_h

#include "jit/JitAllocPolicy.h"
#include "js/HashTable.h"

namespace js {
namespace jit {

class MDefinition;
class MBasicBlock;
class MIRGraph;
class MPhi;
class MIRGenerator;
class MResumePoint;

class ValueNumberer
{
    // Value numbering data.
    class VisibleValues
    {
        // Hash policy for ValueSet.
        struct ValueHasher
        {
            typedef const MDefinition* Lookup;
            typedef MDefinition* Key;
            static HashNumber hash(Lookup ins);
            static bool match(Key k, Lookup l);
            static void rekey(Key& k, Key newKey);
        };

        typedef HashSet<MDefinition*, ValueHasher, JitAllocPolicy> ValueSet;

        ValueSet set_;        // Set of visible values

      public:
        explicit VisibleValues(TempAllocator& alloc);
        MOZ_MUST_USE bool init();

        typedef ValueSet::Ptr Ptr;
        typedef ValueSet::AddPtr AddPtr;

        Ptr findLeader(const MDefinition* def) const;
        AddPtr findLeaderForAdd(MDefinition* def);
        MOZ_MUST_USE bool add(AddPtr p, MDefinition* def);
        void overwrite(AddPtr p, MDefinition* def);
        void forget(const MDefinition* def);
        void clear();
#ifdef DEBUG
        bool has(const MDefinition* def) const;
#endif
    };

    typedef Vector<MBasicBlock*, 4, JitAllocPolicy> BlockWorklist;
    typedef Vector<MDefinition*, 4, JitAllocPolicy> DefWorklist;

    MIRGenerator* const mir_;
    MIRGraph& graph_;
    VisibleValues values_;            // Numbered values
    DefWorklist deadDefs_;            // Worklist for deleting values
    BlockWorklist remainingBlocks_;   // Blocks remaining with fewer preds
    MDefinition* nextDef_;            // The next definition; don't discard
    size_t totalNumVisited_;          // The number of blocks visited
    bool rerun_;                      // Should we run another GVN iteration?
    bool blocksRemoved_;              // Have any blocks been removed?
    bool updateAliasAnalysis_;        // Do we care about AliasAnalysis?
    bool dependenciesBroken_;         // Have we broken AliasAnalysis?
    bool hasOSRFixups_;               // Have we created any OSR fixup blocks?

    enum UseRemovedOption {
        DontSetUseRemoved,
        SetUseRemoved
    };

    MOZ_MUST_USE bool handleUseReleased(MDefinition* def, UseRemovedOption useRemovedOption);
    MOZ_MUST_USE bool discardDefsRecursively(MDefinition* def);
    MOZ_MUST_USE bool releaseResumePointOperands(MResumePoint* resume);
    MOZ_MUST_USE bool releaseAndRemovePhiOperands(MPhi* phi);
    MOZ_MUST_USE bool releaseOperands(MDefinition* def);
    MOZ_MUST_USE bool discardDef(MDefinition* def);
    MOZ_MUST_USE bool processDeadDefs();

    MOZ_MUST_USE bool fixupOSROnlyLoop(MBasicBlock* block, MBasicBlock* backedge);
    MOZ_MUST_USE bool removePredecessorAndDoDCE(MBasicBlock* block, MBasicBlock* pred,
                                                size_t predIndex);
    MOZ_MUST_USE bool removePredecessorAndCleanUp(MBasicBlock* block, MBasicBlock* pred);

    MDefinition* simplified(MDefinition* def) const;
    MDefinition* leader(MDefinition* def);
    bool hasLeader(const MPhi* phi, const MBasicBlock* phiBlock) const;
    bool loopHasOptimizablePhi(MBasicBlock* header) const;

    MOZ_MUST_USE bool visitDefinition(MDefinition* def);
    MOZ_MUST_USE bool visitControlInstruction(MBasicBlock* block);
    MOZ_MUST_USE bool visitUnreachableBlock(MBasicBlock* block);
    MOZ_MUST_USE bool visitBlock(MBasicBlock* block);
    MOZ_MUST_USE bool visitDominatorTree(MBasicBlock* root);
    MOZ_MUST_USE bool visitGraph();

    MOZ_MUST_USE bool insertOSRFixups();
    MOZ_MUST_USE bool cleanupOSRFixups();

  public:
    ValueNumberer(MIRGenerator* mir, MIRGraph& graph);
    MOZ_MUST_USE bool init();

    enum UpdateAliasAnalysisFlag {
        DontUpdateAliasAnalysis,
        UpdateAliasAnalysis
    };

    // Optimize the graph, performing expression simplification and
    // canonicalization, eliminating statically fully-redundant expressions,
    // deleting dead instructions, and removing unreachable blocks.
    MOZ_MUST_USE bool run(UpdateAliasAnalysisFlag updateAliasAnalysis);
};

} // namespace jit
} // namespace js

#endif /* jit_ValueNumbering_h */
