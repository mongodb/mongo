/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ValueNumbering_h
#define jit_ValueNumbering_h

#include "jit/JitAllocPolicy.h"
#include "js/HashTable.h"
#include "js/Vector.h"

namespace js {
namespace jit {

class MDefinition;
class MBasicBlock;
class MIRGraph;
class MPhi;
class MIRGenerator;
class MResumePoint;

class ValueNumberer {
  // Value numbering data.
  class VisibleValues {
    // Hash policy for ValueSet.
    struct ValueHasher {
      using Lookup = const MDefinition*;
      using Key = MDefinition*;
      static HashNumber hash(Lookup ins);
      static bool match(Key k, Lookup l);
      static void rekey(Key& k, Key newKey);
    };

    using ValueSet = HashSet<MDefinition*, ValueHasher, JitAllocPolicy>;

    ValueSet set_;  // Set of visible values

   public:
    explicit VisibleValues(TempAllocator& alloc);

    using Ptr = ValueSet::Ptr;
    using AddPtr = ValueSet::AddPtr;

    Ptr findLeader(const MDefinition* def) const;
    AddPtr findLeaderForAdd(MDefinition* def);
    [[nodiscard]] bool add(AddPtr p, MDefinition* def);
    void overwrite(AddPtr p, MDefinition* def);
    void forget(const MDefinition* def);
    void clear();
#ifdef DEBUG
    bool has(const MDefinition* def) const;
#endif
  };

  using BlockWorklist = Vector<MBasicBlock*, 4, JitAllocPolicy>;
  using DefWorklist = Vector<MDefinition*, 4, JitAllocPolicy>;

  const MIRGenerator* const mir_;
  MIRGraph& graph_;
  VisibleValues values_;           // Numbered values
  DefWorklist deadDefs_;           // Worklist for deleting values
  BlockWorklist remainingBlocks_;  // Blocks remaining with fewer preds
  MDefinition* nextDef_;           // The next definition; don't discard
  size_t totalNumVisited_;         // The number of blocks visited
  bool rerun_;                     // Should we run another GVN iteration?
  bool blocksRemoved_;             // Have any blocks been removed?
  bool updateAliasAnalysis_;       // Do we care about AliasAnalysis?
  bool dependenciesBroken_;        // Have we broken AliasAnalysis?
  bool hasOSRFixups_;              // Have we created any OSR fixup blocks?

  enum ImplicitUseOption { DontSetImplicitUse, SetImplicitUse };
  enum class AllowEffectful : bool { No, Yes };

  [[nodiscard]] bool handleUseReleased(MDefinition* def,
                                       ImplicitUseOption implicitUseOption);
  [[nodiscard]] bool discardDefsRecursively(
      MDefinition* def, AllowEffectful allowEffectful = AllowEffectful::No);
  [[nodiscard]] bool releaseResumePointOperands(MResumePoint* resume);
  [[nodiscard]] bool releaseAndRemovePhiOperands(MPhi* phi);
  [[nodiscard]] bool releaseOperands(MDefinition* def);
  [[nodiscard]] bool discardDef(
      MDefinition* def, AllowEffectful allowEffectful = AllowEffectful::No);
  [[nodiscard]] bool processDeadDefs();

  [[nodiscard]] bool fixupOSROnlyLoop(MBasicBlock* block);
  [[nodiscard]] bool removePredecessorAndDoDCE(MBasicBlock* block,
                                               MBasicBlock* pred,
                                               size_t predIndex);
  [[nodiscard]] bool removePredecessorAndCleanUp(MBasicBlock* block,
                                                 MBasicBlock* pred);

  MDefinition* simplified(MDefinition* def) const;
  MDefinition* leader(MDefinition* def);
  bool hasLeader(const MPhi* phi, const MBasicBlock* phiBlock) const;
  bool loopHasOptimizablePhi(MBasicBlock* header) const;

  [[nodiscard]] bool visitDefinition(MDefinition* def);
  [[nodiscard]] bool visitControlInstruction(MBasicBlock* block);
  [[nodiscard]] bool visitUnreachableBlock(MBasicBlock* block);
  [[nodiscard]] bool visitBlock(MBasicBlock* block);
  [[nodiscard]] bool visitDominatorTree(MBasicBlock* root);
  [[nodiscard]] bool visitGraph();

  [[nodiscard]] bool insertOSRFixups();
  [[nodiscard]] bool cleanupOSRFixups();

 public:
  ValueNumberer(const MIRGenerator* mir, MIRGraph& graph);

  enum UpdateAliasAnalysisFlag { DontUpdateAliasAnalysis, UpdateAliasAnalysis };

  // Optimize the graph, performing expression simplification and
  // canonicalization, eliminating statically fully-redundant expressions,
  // deleting dead instructions, and removing unreachable blocks.
  [[nodiscard]] bool run(UpdateAliasAnalysisFlag updateAliasAnalysis);
};

}  // namespace jit
}  // namespace js

#endif /* jit_ValueNumbering_h */
