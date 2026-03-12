/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/DominatorTree.h"

#include <utility>

#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

namespace js::jit {

// Implementation of the Semi-NCA algorithm, used to compute the immediate
// dominator block for each block in the MIR graph. The same algorithm is used
// by LLVM.
//
// Semi-NCA is described in this dissertation:
//
//   Linear-Time Algorithms for Dominators and Related Problems
//   Loukas Georgiadis, Princeton University, November 2005, pp. 21-23:
//   https://www.cs.princeton.edu/techreports/2005/737.pdf or
//   ftp://ftp.cs.princeton.edu/reports/2005/737.pdf
//
// The code is also inspired by the implementation from the Julia compiler:
//
// https://github.com/JuliaLang/julia/blob/a73ba3bab7ddbf087bb64ef8d236923d8d7f0051/base/compiler/ssair/domtree.jl
//
// And the LLVM version:
//
// https://github.com/llvm/llvm-project/blob/bcd65ba6129bea92485432fdd09874bc3fc6671e/llvm/include/llvm/Support/GenericDomTreeConstruction.h
//
// At a high level, the algorithm has the following phases:
//
//  1) Depth-First Search (DFS) to assign a new 'DFS-pre-number' id to each
//     basic block. See initStateAndRenumberBlocks.
//  2) Compute semi-dominators. The first loop in computeDominators.
//  3) Compute immediate dominators. The second loop in computeDominators.
//  4) Store the immediate dominators in the MIR graph. The third loop in
//     computeDominators.
//
// Future work: the algorithm can be extended to update the dominator tree more
// efficiently after inserting or removing edges, instead of recomputing the
// whole graph. This is called Dynamic SNCA and both Julia and LLVM implement
// this.
class MOZ_RAII SemiNCA {
  const MIRGenerator* mir_;
  MIRGraph& graph_;

  // State associated with each basic block. Note that the paper uses separate
  // arrays for these fields: label[v] is stored as state_[v].label in our code.
  struct BlockState {
    MBasicBlock* block = nullptr;
    uint32_t ancestor = 0;
    uint32_t label = 0;
    uint32_t semi = 0;
    uint32_t idom = 0;
  };
  using BlockStateVector = Vector<BlockState, 8, BackgroundSystemAllocPolicy>;
  BlockStateVector state_;

  // Stack for |compress|. This is allocated here instead of in |compress| to
  // avoid extra malloc/free calls.
  using CompressStack = Vector<uint32_t, 16, BackgroundSystemAllocPolicy>;
  CompressStack compressStack_;

  [[nodiscard]] bool initStateAndRenumberBlocks();
  [[nodiscard]] bool compress(uint32_t v, uint32_t lastLinked);

 public:
  SemiNCA(const MIRGenerator* mir, MIRGraph& graph)
      : mir_(mir), graph_(graph) {}

  [[nodiscard]] bool computeDominators();
};

}  // namespace js::jit

bool SemiNCA::initStateAndRenumberBlocks() {
  MOZ_ASSERT(state_.empty());

  // MIR graphs can have multiple root blocks when OSR is used, but the
  // algorithm assumes the graph has a single root block. Use a virtual root
  // block (with id 0) that has the actual root blocks as successors. At the
  // end, if a block has this virtual root node as immediate dominator, we mark
  // it self-dominating.
  size_t nblocks = 1 /* virtual root */ + graph_.numBlocks();
  if (!state_.growBy(nblocks)) {
    return false;
  }

  using BlockAndParent = std::pair<MBasicBlock*, uint32_t>;
  Vector<BlockAndParent, 16, BackgroundSystemAllocPolicy> worklist;

  // Append all root blocks to the work list.
  constexpr size_t RootId = 0;
  if (!worklist.emplaceBack(graph_.entryBlock(), RootId)) {
    return false;
  }

  if (MBasicBlock* osrBlock = graph_.osrBlock()) {
    if (!worklist.emplaceBack(osrBlock, RootId)) {
      return false;
    }

    // If OSR is used, the graph can contain blocks marked unreachable. These
    // blocks aren't reachable from the other roots, so treat them as additional
    // roots to make sure we visit (and initialize state for) all blocks in the
    // graph.
    for (MBasicBlock* block : graph_) {
      if (block->unreachable()) {
        if (!worklist.emplaceBack(block, RootId)) {
          return false;
        }
      }
    }
  }

  // Visit all blocks in DFS order and re-number them. Also initialize the
  // BlockState for each block.
  uint32_t id = 1;
  while (!worklist.empty()) {
    auto [block, parent] = worklist.popCopy();
    block->mark();
    block->setId(id);
    state_[id] = {
        .block = block, .ancestor = parent, .label = id, .idom = parent};

    for (size_t i = 0, n = block->numSuccessors(); i < n; i++) {
      MBasicBlock* succ = block->getSuccessor(i);
      if (succ->isMarked()) {
        continue;
      }
      if (!worklist.emplaceBack(succ, id)) {
        return false;
      }
    }

    id++;
  }

  MOZ_ASSERT(id == nblocks, "should have visited all blocks");
  return true;
}

bool SemiNCA::compress(uint32_t v, uint32_t lastLinked) {
  // Iterative implementation of snca_compress in Figure 2.8 of the paper, but
  // with the optimization described in section 2.2.1 to compare against
  // lastLinked instead of checking the ancestor is 0.
  //
  // Walk up the ancestor chain from |v| until we reach the last node we already
  // processed. Then walk back to |v| and compress this path.

  if (state_[v].ancestor < lastLinked) {
    return true;
  }

  // Push v and all its ancestor nodes that we've processed on the stack, except
  // for the last one.
  MOZ_ASSERT(compressStack_.empty());
  do {
    if (!compressStack_.append(v)) {
      return false;
    }
    v = state_[v].ancestor;
  } while (state_[v].ancestor >= lastLinked);

  // Now walk the ancestor chain back to the node where we started and perform
  // path compression.
  uint32_t root = state_[v].ancestor;
  do {
    uint32_t u = v;
    v = compressStack_.popCopy();
    MOZ_ASSERT(u < v);
    MOZ_ASSERT(state_[v].ancestor == u);
    MOZ_ASSERT(u >= lastLinked);

    // The meat of the snca_compress function in the paper.
    if (state_[u].label < state_[v].label) {
      state_[v].label = state_[u].label;
    }
    state_[v].ancestor = root;
  } while (!compressStack_.empty());

  return true;
}

bool SemiNCA::computeDominators() {
  if (!initStateAndRenumberBlocks()) {
    return false;
  }

  size_t nblocks = state_.length();

  // The following two loops implement the SNCA algorithm in Figure 2.8 of the
  // paper.

  // Compute semi-dominators.
  for (size_t w = nblocks - 1; w > 0; w--) {
    if (mir_->shouldCancel("SemiNCA computeDominators")) {
      return false;
    }

    MBasicBlock* block = state_[w].block;
    uint32_t semiW = state_[w].ancestor;

    // All nodes with id >= lastLinked have already been processed and are
    // eligible for path compression. See also the comment in |compress|.
    uint32_t lastLinked = w + 1;

    for (size_t i = 0, n = block->numPredecessors(); i < n; i++) {
      MBasicBlock* pred = block->getPredecessor(i);
      uint32_t v = pred->id();
      if (v >= lastLinked) {
        // Attempt path compression. Note that this |v >= lastLinked| check
        // isn't strictly required and isn't in the paper, because it's implied
        // by the first check in |compress|, but this is more efficient.
        if (!compress(v, lastLinked)) {
          return false;
        }
      }
      semiW = std::min(semiW, state_[v].label);
    }

    state_[w].semi = semiW;
    state_[w].label = semiW;
  }

  // Compute immediate dominators.
  for (size_t v = 1; v < nblocks; v++) {
    auto& blockState = state_[v];
    uint32_t idom = blockState.idom;
    while (idom > blockState.semi) {
      idom = state_[idom].idom;
    }
    blockState.idom = idom;
  }

  // Set immediate dominators for all blocks in the MIR graph. Also unmark all
  // blocks (|initStateAndRenumberBlocks| marked them) and restore their
  // original IDs.
  uint32_t id = 0;
  for (ReversePostorderIterator block(graph_.rpoBegin());
       block != graph_.rpoEnd(); block++) {
    auto& state = state_[block->id()];
    if (state.idom == 0) {
      // If a block is dominated by the virtual root node, it's self-dominating.
      block->setImmediateDominator(*block);
    } else {
      block->setImmediateDominator(state_[state.idom].block);
    }
    block->unmark();
    block->setId(id++);
  }

  // Done!
  return true;
}

static bool ComputeImmediateDominators(const MIRGenerator* mir,
                                       MIRGraph& graph) {
  SemiNCA semiNCA(mir, graph);
  return semiNCA.computeDominators();
}

bool jit::BuildDominatorTree(const MIRGenerator* mir, MIRGraph& graph) {
  MOZ_ASSERT(graph.canBuildDominators());

  if (!ComputeImmediateDominators(mir, graph)) {
    return false;
  }

  Vector<MBasicBlock*, 4, JitAllocPolicy> worklist(graph.alloc());

  // Traversing through the graph in post-order means that every non-phi use
  // of a definition is visited before the def itself. Since a def
  // dominates its uses, by the time we reach a particular
  // block, we have processed all of its dominated children, so
  // block->numDominated() is accurate.
  for (PostorderIterator i(graph.poBegin()); i != graph.poEnd(); i++) {
    MBasicBlock* child = *i;
    MBasicBlock* parent = child->immediateDominator();

    // Dominance is defined such that blocks always dominate themselves.
    child->addNumDominated(1);

    // If the block only self-dominates, it has no definite parent.
    // Add it to the worklist as a root for pre-order traversal.
    // This includes all roots. Order does not matter.
    if (child == parent) {
      if (!worklist.append(child)) {
        return false;
      }
      continue;
    }

    if (!parent->addImmediatelyDominatedBlock(child)) {
      return false;
    }

    parent->addNumDominated(child->numDominated());
  }

#ifdef DEBUG
  // If compiling with OSR, many blocks will self-dominate.
  // Without OSR, there is only one root block which dominates all.
  if (!graph.osrBlock()) {
    MOZ_ASSERT(graph.entryBlock()->numDominated() == graph.numBlocks());
  }
#endif
  // Now, iterate through the dominator tree in pre-order and annotate every
  // block with its index in the traversal.
  size_t index = 0;
  while (!worklist.empty()) {
    MBasicBlock* block = worklist.popCopy();
    block->setDomIndex(index);

    if (!worklist.append(block->immediatelyDominatedBlocksBegin(),
                         block->immediatelyDominatedBlocksEnd())) {
      return false;
    }
    index++;
  }

  return true;
}

void jit::ClearDominatorTree(MIRGraph& graph) {
  for (MBasicBlockIterator iter = graph.begin(); iter != graph.end(); iter++) {
    iter->clearDominatorInfo();
  }
}
