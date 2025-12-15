/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/UnrollLoops.h"

#include "mozilla/Maybe.h"

#include <stdio.h>

#include "jit/DominatorTree.h"
#include "jit/IonAnalysis.h"
#include "jit/MIRGraph.h"

namespace js {
namespace jit {

// [SMDOC] Loop unroller implementation summary
//
// This is a simple loop unroller, intended (initially at least) to handle only
// wasm loops, with the aims of amortizing the per-iteration interrupt check
// cost, and of lifting an initial iteration outside the loop so as to
// facilitate subsequent optimizations.  Unrolling and peeling can be selected
// independently, so the available choices are: peeling only, unrolling only,
// or both peeling and unrolling.
//
// The flow of control (for a single function) is roughly:
//
// (0) The top level function is UnrollLoops.
//
// (1) UnrollLoops calls FindInitialCandidates to identify "initial candidate"
//     loops in the function.  These are innermost loops.  Non-innermost loops
//     cannot be unrolled/peeled.
//
// (2) FindInitialCandidates counts blocks and value defs in each initial
//     candidate, creating an initial score which increases with loop size.
//
// (3) Back in UnrollLoops, initial candidates whose initial score does not
//     exceed InitialCandidateThreshold proceed to the next stage in the
//     process: they are passed to function AnalyzeLoop.
//
// (4) AnalyzeLoop, as called from UnrollLoops, checks many structural
//     invariants on each loop, since the core unroll machinery depends on
//     those invariants.  It also makes a final determination for each loop, as
//     to whether the loop should be peeled, unrolled, peeled and unrolled, or
//     be left unchanged.  In the latter case it returns one of a few reasons
//     why the loop is to be left unchanged.
//
// (5) AnalyzeLoop also identifies, for a loop, its basic blocks, the set of
//     blocks it jumps to when exiting, and the values defined in the loop
//     which are used afterwards.
//
// (6) Back in UnrollLoops, we now know which loops are suitable for
//     processing, what processing is to happen to them, and what their blocks
//     are.  This is used to construct and initialize a `struct UnrollState`
//     for each loop candidate that made it this far.
//
// (7) Before a loop can be unrolled, single-argument phi nodes need to be
//     added to its exit target blocks, to "catch" values defined in the loop
//     that are used afterwards.  This is a limited conversion into what is
//     called "Loop-Closed SSA form" in the GCC/Clang literature.  UnrollLoops
//     calls AddClosingPhisForLoop to get this done.  This is done for all
//     loops before proceeding further.
//
// (8) Finally, we get to the core of the unrolling: the loops are handed to
//     UnrollAndOrPeelLoop in turn.  This has extensive comments; a summary
//     of the actions is:
//     - blocks, phis and normal instructions are cloned so as to create
//       the extra loop body copies.
//     - the new blocks are added to the loop's UnrollState::blockTable.
//     - during cloning, value uses are remapped to the correct iteration
//       using an MDefinitionRemapper.
//     - also during cloning, the new values are parked in a ValueTable for
//       later reference.
//     - control flow edges and predecessor vectors in the new and original
//       block sets are fixed up, using RemapEdgeSource and
//       RemapEdgeDestination.
//     - header block phi nodes are fixed up in both the body copies
//       and the original.
//     - new exiting blocks are created, so as to maintain the invariant
//       that there are no critical edges.
//     - for exit target blocks, both their predecessor arrays and phi nodes
//       (as installed by AddClosingPhisForLoop) are augmented to handle
//       the new exit edges.
//     - Wasm interrupt checks in all but the last body copy are nop'd out.
//     - If peeling is required, the back edge of the unrolled loop is changed
//       so it points at the second body copy, not the first (the original).
//     - Finally, the new blocks are installed in the MIRGraph.
//
// (9) Back in UnrollLoops, once all loops have been processed,
//     some function-level fixups are done:
//     - the blocks are renumbered
//     - the dominator tree is rebuilt
//
// UnrollLoops now returns to OptimizeMIR, handing back an indication of how
// many loops were processed.  OptimizeMIR will tidy up the resulting function
// by re-running GVN, empty block removal, and possibly other transformations.

// The core logic in UnrollAndOrPeelLoop is heavily commented but still
// nearly incomprehensible.  For debugging and modification it is helpful to
// use the Dump{Block,Value}Table* routines provided.

// The logic below functions without reference to block IDs or value IDs.

// Much of the logic uses simple scans over vectors (classes BlockVector,
// Matrix, SimpleSet, MDefinitionRemapper) rather than more complex data
// structures.  The loops we process are small, which keeps the costs of these
// scans low.  The InitialCandidateThreshold value used limits most processed
// loops to about 6 blocks and 30-ish value definitions.

// The idiom `blockTable.rowContains(0, block)` in the logic below means
// "is `block` in the original loop?"

// =====================================================================
//
// Tunables

// As a crude first pass for filtering out unsuitable candidates, top level
// function UnrollLoops calculates a size-related value for each candidate,
// with lower values indicating more desirability for unrolling/peeling.
// Candidates with a score above this value will not be unrolled.  Reducing the
// value therefore reduces the aggressiveness of the unroller.
const float InitialCandidateThreshold = 100.0;

// On further analysis of candidates that pass the above test, we have more
// detailed limitations on size.  The specified action will not be taken for a
// loop if the associated metric exceeds the constants here.  Hence increasing
// these values increases the aggressiveness of the unroller, but only to point
// where the `InitialCandidateThreshold` would have cut it off.

// Loops with more than this number of basic blocks or values (phis and normal
// definitions) will be excluded from peeling.  The actual numbers are pretty
// much pulled out of a hat.
const size_t MaxBlocksForPeel = 12;
const size_t MaxValuesForPeel = 150;

// Loops with more than this number of blocks or values will be excluded from
// unrolling.  Unrolling adds multiple copies of the loop body, whereas peeling
// adds only one, so it seems prudent to make unrolling less aggressive than
// peeling.
const size_t MaxBlocksForUnroll = 10;
const size_t MaxValuesForUnroll = 100;

// =====================================================================
//
// BlockVector, a vector of MBasicBlock*s.

using BlockVector = mozilla::Vector<MBasicBlock*, 32, SystemAllocPolicy>;

static bool BlockVectorContains(const BlockVector& vec,
                                const MBasicBlock* block) {
  for (const MBasicBlock* b : vec) {
    if (b == block) {
      return true;
    }
  }
  return false;
}

// =====================================================================
//
// Matrix, a 2-D array of pointers

template <typename T, int N, typename AP>
class Matrix {
  static_assert(std::is_pointer<T>::value);
  mozilla::Vector<T, N, AP> vec_;
  size_t size1_ = 0;
  size_t size2_ = 0;
  inline size_t index(size_t ix1, size_t ix2) const {
    MOZ_ASSERT(ix1 < size1_ && ix2 < size2_);
    return ix1 * size2_ + ix2;
  }

 public:
  [[nodiscard]] bool init(size_t size1, size_t size2) {
    // Not already init'd.
    MOZ_ASSERT(size1_ == 0 && size2_ == 0 && vec_.empty());
    // This would be pointless.
    MOZ_ASSERT(size1 > 0 && size2 > 0);
    // So we can safely multiply them.  InitialCandidateThreshold ensures we'll
    // never have to process a loop anywhere near this big.
    MOZ_RELEASE_ASSERT(size1 <= 1000 && size2 <= 1000);
    size1_ = size1;
    size2_ = size2;
    if (!vec_.resize(size1 * size2)) {
      return false;
    }
    // Resizing also zero-initialises all the `vec_` entries.
    return true;
  }

  inline size_t size1() const {
    MOZ_ASSERT(size1_ * size2_ == vec_.length());
    return size1_;
  }
  inline size_t size2() const {
    MOZ_ASSERT(size1_ * size2_ == vec_.length());
    return size2_;
  }
  inline T get(size_t ix1, size_t ix2) const { return vec_[index(ix1, ix2)]; }
  inline void set(size_t ix1, size_t ix2, T value) {
    vec_[index(ix1, ix2)] = value;
  }

  // Does the matrix slice `[ix1, ..]` contain `value`?
  inline bool rowContains(size_t ix1, const T value) const {
    return findInRow(ix1, value).isSome();
  }

  // Find the column number (ix2) such that entry `[ix1, ix2] == value`.
  mozilla::Maybe<size_t> findInRow(size_t ix1, const T value) const {
    for (size_t ix2 = 0; ix2 < size2_; ix2++) {
      if (get(ix1, ix2) == value) {
        return mozilla::Some(ix2);
      }
    }
    return mozilla::Nothing();
  }
};

using BlockTable = Matrix<MBasicBlock*, 32, SystemAllocPolicy>;
using ValueTable = Matrix<MDefinition*, 128, SystemAllocPolicy>;

// =====================================================================
//
// Debug printing

#ifdef JS_JITSPEW

// For debugging, dump specific rows (loop body copies) of a block table.
static void DumpBlockTableRows(const BlockTable& table, int32_t firstCix,
                               int32_t lastCix, const char* tag) {
  Fprinter& printer(JitSpewPrinter());
  printer.printf("<<<< %s\n", tag);
  for (int32_t cix = firstCix; cix <= lastCix; cix++) {
    if (cix == 0) {
      printer.printf("  -------- Original --------\n");
    } else {
      printer.printf("  -------- Copy %u --------\n", cix);
    }
    for (uint32_t bix = 0; bix < table.size2(); bix++) {
      DumpMIRBlock(printer, table.get(uint32_t(cix), bix),
                   /*showHashedPointers=*/true);
    }
  }
  printer.printf(">>>>\n");
}

// Dump an entire block table.
static void DumpBlockTable(const BlockTable& table, const char* tag) {
  DumpBlockTableRows(table, 0, int32_t(table.size1()) - 1, tag);
}

// Dump just the original loop body in a block table.
static void DumpBlockTableRowZero(const BlockTable& table, const char* tag) {
  DumpBlockTableRows(table, 0, 0, tag);
}

// Dump a value table.
static void DumpValueTable(const ValueTable& table, const char* tag) {
  Fprinter& printer(JitSpewPrinter());
  printer.printf("<<<< %s\n", tag);
  for (uint32_t cix = 0; cix < table.size1(); cix++) {
    if (cix == 0) {
      printer.printf("  -------- Original --------\n");
    } else {
      printer.printf("  -------- Copy %u --------\n", cix);
    }
    for (uint32_t vix = 0; vix < table.size2(); vix++) {
      printer.printf("    ");
      DumpMIRDefinition(printer, table.get(cix, vix),
                        /*showHashedPointers=*/true);
      printer.printf("\n");
    }
  }
  printer.printf(">>>>\n");
}

#endif  // JS_JITSPEW

// =====================================================================
//
// SimpleSet, a set of pointers

template <typename T, int N, typename AP>
class SimpleSet {
  static_assert(std::is_pointer<T>::value);
  mozilla::Vector<T, N, AP> vec_;

 public:
  [[nodiscard]] bool copy(const SimpleSet<T, N, AP>& other) {
    vec_.clear();
    for (T elem : other.vec_) {
      if (!vec_.append(elem)) {
        return false;
      }
    }
    return true;
  }
  bool contains(T t) const {
    for (auto* existing : vec_) {
      if (existing == t) {
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] bool add(T t) {
    MOZ_ASSERT(t);
    return contains(t) ? true : vec_.append(t);
  }
  bool empty() const { return vec_.empty(); }
  size_t size() const { return vec_.length(); }
  T get(size_t ix) const { return vec_[ix]; }
};

using BlockSet = SimpleSet<MBasicBlock*, 8, SystemAllocPolicy>;
using ValueSet = SimpleSet<MDefinition*, 64, SystemAllocPolicy>;

// =====================================================================
//
// MDefinitionRemapper, a very simple MDefinition*-remapping class.

class MDefinitionRemapper {
  using Pair = std::pair<MDefinition*, MDefinition*>;
  mozilla::Vector<Pair, 32, SystemAllocPolicy> pairs;

 public:
  MDefinitionRemapper() {}
  // Register `original` as a key in the mapper, and map it to itself.
  [[nodiscard]] bool enregister(MDefinition* original) {
    MOZ_ASSERT(original);
    for (auto& p : pairs) {
      (void)p;
      MOZ_ASSERT(p.first != original);  // not mapped at all
      MOZ_ASSERT(p.second == p.first);  // no `update` calls happened yet
    }
    return pairs.append(std::pair(original, original));
  }
  // Look up the binding for `original` in the mapper.
  MDefinition* lookup(const MDefinition* original) const {
    MOZ_ASSERT(original);
    MDefinition* res = nullptr;
    for (auto& p : pairs) {
      if (p.first == original) {
        res = p.second;
        break;
      }
    }
    return res;
  }
  // Update the mapper so as to map `original` to `replacement`.  `original`
  // must have previously been registered with ::enregister.
  void update(const MDefinition* original, MDefinition* replacement) {
    MOZ_ASSERT(original && replacement);
    MOZ_ASSERT(original != replacement);
    for (auto& p : pairs) {
      if (p.first == original) {
        p.second = replacement;
        return;
      }
    }
    MOZ_CRASH();  // asked to update a non-existent key
  }
};

// =====================================================================
//
// MakeReplacementInstruction / MakeReplacementPhi

// Make a cloned version of `ins`, but with its arguments remapped by `mapper`.
static MInstruction* MakeReplacementInstruction(
    TempAllocator& alloc, const MDefinitionRemapper& mapper,
    const MInstruction* ins) {
  MDefinitionVector inputs(alloc);
  for (size_t i = 0; i < ins->numOperands(); i++) {
    MDefinition* old = ins->getOperand(i);
    MDefinition* replacement = mapper.lookup(old);
    if (!replacement) {
      // The mapper didn't map it, which means that `old` is a value that's not
      // defined in the loop body.  So leave it unchanged.
      replacement = old;
    }
    if (!inputs.append(replacement)) {
      return nullptr;
    }
  }
  return ins->clone(alloc, inputs);
}

// Make a cloned version of `phi`, but with its arguments remapped by `mapper`.
static MPhi* MakeReplacementPhi(TempAllocator& alloc,
                                const MDefinitionRemapper& mapper,
                                const MPhi* phi) {
  MDefinitionVector inputs(alloc);
  for (size_t i = 0; i < phi->numOperands(); i++) {
    MDefinition* old = phi->getOperand(i);
    MDefinition* replacement = mapper.lookup(old);
    if (!replacement) {
      replacement = old;
    }
    if (!inputs.append(replacement)) {
      return nullptr;
    }
  }
  return phi->clone(alloc, inputs);
}

// =====================================================================
//
// UnrollState

enum class UnrollMode { Peel, Unroll, PeelAndUnroll };

struct UnrollState {
  // What should happen to the specified loop.
  const UnrollMode mode;

  // The central data structure: BlockTable, a 2-D array of block pointers.
  //
  // To create copies of the original loop body, we need to copy the original
  // blocks and fix up the inter-block edges and phis appropriately.  This
  // edge-fixup is done without reference to the block IDs; instead it is done
  // purely on the basis of the MBasicBlock* values themselves.  Hence it does
  // not make any assumption about the ordering of the block ID values.
  //
  // The block copies are managed using a 2-D array of block pointers, via
  // which we can uniformly access the original and new blocks.  The array is
  // indexed by `(copy index, block index in copy)`.  Both indices are
  // zero-based.  The sequencing of blocks implied by "block index in copy" is
  // the sequence produced by the RPO iterator as it enumerates the blocks in
  // the loop.
  //
  // Per the above, note that "block index" (`bix` in the code) is a zero-based
  // index into the RPO sequence of blocks in the loop and is not to be
  // confused with the pre-existing concept of "block ID".
  //
  // Note also that the required unroll factor (including that for the peeled
  // copy, if any) is equal to the first dimension of `blockTable`, and the
  // number of blocks in the original loop is implied by the second dimension
  // of `blockTable`.  Hence neither value needs to be stored explicitly.
  BlockTable blockTable;

  // The set of blocks arrived at (directly) by exiting branches in the
  // original loop.  FIXME: try to make const
  /*const*/ BlockSet exitTargetBlocks;

  // The set of values (MDefinition*s) defined in the original loop and used
  // afterwards.
  /*const*/ ValueSet exitingValues;

  explicit UnrollState(UnrollMode mode) : mode(mode) {}

  bool doPeeling() const {
    return mode == UnrollMode::Peel || mode == UnrollMode::PeelAndUnroll;
  }
  bool doUnrolling() const {
    return mode == UnrollMode::Unroll || mode == UnrollMode::PeelAndUnroll;
  }
};

// =====================================================================
//
// AnalyzeLoop and AnalysisResult

// Summarises the result of a detailed analysis of a candidate loop, producing
// either a recommendation about what to do with it, or a reason why it should
// be left unchanged.
enum class AnalysisResult {
  // OOM during analysis
  OOM,

  // The chosen transformation for the loop ..
  Peel,
  Unroll,
  PeelAndUnroll,

  // .. or .. reasons why the loop can't be unrolled:
  //
  // The unrolling/peeling machinery below relies on the fact that MIR loops
  // are heavily constrained in their structure.  AnalyzeLoop checks the
  // relevant invariants carefully, with this value indicating non-compliance.
  // Top level function UnrollLoops will MOZ_CRASH if this happens.
  BadInvariants,
  // Cannot be handled by the unroller: if the loop defines value(s) that are
  // used after the loop, then we can only handle the case where it has one
  // exiting branch.
  TooComplex,
  // The loop contains MIR nodes that are uncloneable
  Uncloneable,
  // The loop has too many blocks or values
  TooLarge,
  // The loop is otherwise unsuitable:
  // * contains a call or table switch
  // * is an infinite loop
  Unsuitable
};

#ifdef JS_JITSPEW
static const char* Name_of_AnalysisResult(AnalysisResult res) {
  switch (res) {
    case AnalysisResult::OOM:
      return "OOM";
    case AnalysisResult::Peel:
      return "Peel";
    case AnalysisResult::Unroll:
      return "Unroll";
    case AnalysisResult::PeelAndUnroll:
      return "PeelAndUnroll";
    case AnalysisResult::BadInvariants:
      return "BadInvariants";
    case AnalysisResult::TooComplex:
      return "TooComplex";
    case AnalysisResult::Uncloneable:
      return "Uncloneable";
    case AnalysisResult::TooLarge:
      return "TooLarge";
    case AnalysisResult::Unsuitable:
      return "Unsuitable";
    default:
      MOZ_CRASH();
  }
}
#endif

// Examine the original loop in `originalBlocks` for unrolling suitability,
// and, if acceptable, collect auxiliary information:
//
// * the set of blocks that are direct exit targets for the loop
// * the set of values defined in the loop AND used afterwards

static AnalysisResult AnalyzeLoop(const BlockVector& originalBlocks,
                                  BlockSet* exitTargetBlocks,
                                  ValueSet* exitingValues) {
  MOZ_ASSERT(exitTargetBlocks->empty());
  MOZ_ASSERT(exitingValues->empty());

  // ==== BEGIN check invariants on the loop structure ====

  // The unroller/peeler below depends on these invariants.
  // In summary (where nBlocks == originalBlocks.length()):
  //
  // * At least 2 blocks in original loop (1 for header phis + possible insns,
  //   >= 0 for "ordinary" blocks, 1 for the backedge jump)
  //
  // * originalBlocks[0] is a loop header
  // * originalBlocks[nBlocks-1] == originalBlocks[0]->backedge
  //
  // * all blocks in original loop have at least one insn
  // * all blocks in original loop have a control insn as their last insn
  // * all blocks in original loop have non-control insns for all other insns
  // * all blocks in original loop have loop depth > 0
  //
  // * none of originalBlocks[0 .. nBlocks-2] (inclusive) jumps to header
  //
  // * originalBlocks[nBlocks-1] jumps unconditionally to header
  //
  // * original header node has 2 preds
  // * original header node pred[0] is from outside the loop, and
  //   pred[1] is from inside the loop.
  //
  // * original header node phis all have 2 args
  // * original header node phis have arg[0] from outside the loop
  //   (because pred[0] is the loop-entry edge)
  // * original header node phis have their arg[1] as
  //   -- (almost always) defined in the loop -- loop carried variable
  //   -- (very occasionally) defined outside the loop -- in which case
  //      this is just a vanilla phi, nothing to do with the loop.
  //   Hence there is nothing to check for phi arg[1]s, but leaving this
  //   here for clarity.
  //
  // Aside: how can a loop head phi not depend on a value defined within the
  // loop?  Consider the strange-but-valid case
  //
  //    x = 5 ; loop { .. x = 7; .. }
  //
  // The loop head phi will merge the initial value (5) and the loop-defined
  // value in the usual way.  Imagine now the loop is LICMd; the loop defined
  // value `x = 7;` is observed to be invariant and so is lifted out of the
  // loop.  Hence the loop head phi will have both args defined outside the
  // loop.

  // * At least 2 blocks in original loop (1 for header phis + possible insns,
  //   >= 0 for "ordinary" blocks, 1 for the backedge jump)
  const size_t numBlocksInOriginal = originalBlocks.length();
  if (numBlocksInOriginal < 2) {
    return AnalysisResult::BadInvariants;
  }

  // * originalBlocks[0] is a loop header
  // * originalBlocks[nBlocks-1] == originalBlocks[0]->backedge
  {
    MBasicBlock* header = originalBlocks[0];
    if (!header->isLoopHeader() ||
        header->backedge() != originalBlocks[numBlocksInOriginal - 1]) {
      return AnalysisResult::BadInvariants;
    }
  }

  // * all blocks in original loop have at least one insn
  // * all blocks in original loop have a control insn as their last insn
  // * all blocks in original loop have non-control insns for all other insns
  // * all blocks in original loop have loop depth > 0
  for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
    MBasicBlock* block = originalBlocks[bix];
    // This is a bit lame, but ..
    size_t numInsns = 0;
    for (MInstructionIterator insnIter(block->begin());
         insnIter != block->end(); insnIter++) {
      numInsns++;
    }
    if (numInsns < 1) {
      return AnalysisResult::BadInvariants;
    }
    bool ok = true;
    size_t curInsn = 0;
    for (MInstructionIterator insnIter(block->begin());
         insnIter != block->end(); insnIter++) {
      MInstruction* insn = *insnIter;
      if (curInsn == numInsns - 1) {
        ok = ok && insn->isControlInstruction();
      } else {
        ok = ok && !insn->isControlInstruction();
      }
      curInsn++;
    }
    MOZ_ASSERT(curInsn == numInsns);  // internal logic
    ok = ok && block->loopDepth() > 0;
    if (!ok) {
      return AnalysisResult::BadInvariants;
    }
  }

  // * none of originalBlocks[0 .. nBlocks-2] (inclusive) jumps to header
  {
    MBasicBlock* header = originalBlocks[0];
    for (uint32_t bix = 0; bix < numBlocksInOriginal - 1; bix++) {
      MBasicBlock* block = originalBlocks[bix];
      if (!block->hasLastIns()) {
        return AnalysisResult::BadInvariants;
      }
      MControlInstruction* lastIns = block->lastIns();
      for (size_t i = 0; i < lastIns->numSuccessors(); i++) {
        if (lastIns->getSuccessor(i) == header) {
          return AnalysisResult::BadInvariants;
        }
      }
    }
  }

  // originalBlocks[nBlocks-1] jumps unconditionally to header
  {
    MBasicBlock* header = originalBlocks[0];
    MBasicBlock* backedge = originalBlocks[numBlocksInOriginal - 1];
    if (!backedge->hasLastIns()) {
      return AnalysisResult::BadInvariants;
    }
    MControlInstruction* lastIns = backedge->lastIns();
    if (!lastIns->isGoto() || lastIns->toGoto()->target() != header) {
      return AnalysisResult::BadInvariants;
    }
  }

  // * original header node has 2 preds
  // * original header node pred[0] is from outside the loop.
  //   pred[1] is from inside the loop.
  {
    MBasicBlock* header = originalBlocks[0];
    if (header->numPredecessors() != 2 ||
        BlockVectorContains(originalBlocks, header->getPredecessor(0)) ||
        !BlockVectorContains(originalBlocks, header->getPredecessor(1))) {
      return AnalysisResult::BadInvariants;
    }
  }

  // * original header node phis all have 2 args
  // * original header node phis have arg[0] from outside the loop
  //   (because pred[0] is the loop-entry edge)
  // * original header node phis have their arg[1] as
  //   -- (almost always) defined in the loop -- loop carried variable
  //   -- (very occasionally) defined outside the loop -- in which case
  //      this is just a vanilla phi, nothing to do with the loop.
  //   Hence there is nothing to check for phi arg[1]s, but leaving this
  //   here for clarity.
  {
    MBasicBlock* header = originalBlocks[0];
    for (MPhiIterator phiIter(header->phisBegin());
         phiIter != header->phisEnd(); phiIter++) {
      MPhi* phi = *phiIter;
      if (phi->numOperands() != 2 ||
          BlockVectorContains(originalBlocks, phi->getOperand(0)->block())) {
        return AnalysisResult::BadInvariants;
      }
    }
  }

  // ==== END check invariants on the loop structure ====

  // Check that all the insns are cloneable.
  for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
    MBasicBlock* block = originalBlocks[bix];
    for (MInstructionIterator insIter(block->begin()); insIter != block->end();
         insIter++) {
      MInstruction* ins = *insIter;
      if (ins->isWasmCallCatchable() || ins->isWasmCallUncatchable() ||
          ins->isWasmReturnCall() || ins->isTableSwitch()
          /* TODO: other node kinds unsuitable for unrolling? */) {
        return AnalysisResult::Unsuitable;
      }
      if (!ins->canClone()) {
        // `ins->opName()` will show the name of the insn kind, if you want to
        // see it
        return AnalysisResult::Uncloneable;
      }
    }
  }

  // More analysis: make up a set of blocks that are not in the loop, but which
  // are jumped to from within the loop.  We will need this later.
  for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
    MBasicBlock* block = originalBlocks[bix];
    MControlInstruction* lastIns = block->lastIns();  // asserted safe above
    for (size_t i = 0; i < lastIns->numSuccessors(); i++) {
      MBasicBlock* succ = lastIns->getSuccessor(i);
      // See if `succ` is any of the blocks in the (original) loop.  If not,
      // add it to `exitTargetBlocks`.
      if (!BlockVectorContains(originalBlocks, succ)) {
        if (!exitTargetBlocks->add(succ)) {
          return AnalysisResult::OOM;
        }
      }
    }
  }

  // If the loop has zero exits, we consider it sufficiently mutant to ignore
  // (it's an infinite loop?)
  if (exitTargetBlocks->empty()) {
    return AnalysisResult::Unsuitable;
  }

  // Even more analysis: make up a set of MDefinition*s that are defined in the
  // original loop and which are used after the loop.  We will later need to
  // add phi nodes that generate the correct values for these, since, after
  // unrolling, these values will have multiple definition points (one per
  // unrolled iteration).  While we're at it, count the number of values
  // defined in the original loop.
  size_t numValuesInOriginal = 0;
  for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
    MBasicBlock* block = originalBlocks[bix];
    // Check all phi nodes ..
    for (MPhiIterator phiIter(block->phisBegin()); phiIter != block->phisEnd();
         phiIter++) {
      numValuesInOriginal++;
      MPhi* phi = *phiIter;
      bool usedAfterLoop = false;
      // Is the value of `phi` used after the loop?
      for (MUseIterator useIter(phi->usesBegin()); useIter != phi->usesEnd();
           useIter++) {
        MUse* use = *useIter;
        if (!BlockVectorContains(originalBlocks, use->consumer()->block())) {
          usedAfterLoop = true;
          break;
        }
      }
      if (usedAfterLoop && !exitingValues->add(phi)) {
        return AnalysisResult::OOM;
      }
    }
    // .. and all normal nodes ..
    for (MInstructionIterator insnIter(block->begin());
         insnIter != block->end(); insnIter++) {
      numValuesInOriginal++;
      MInstruction* insn = *insnIter;
      bool usedAfterLoop = false;
      // Is the value of `insn` used after the loop?
      for (MUseIterator useIter(insn->usesBegin()); useIter != insn->usesEnd();
           useIter++) {
        MUse* use = *useIter;
        if (!BlockVectorContains(originalBlocks, use->consumer()->block())) {
          usedAfterLoop = true;
          break;
        }
      }
      if (usedAfterLoop && !exitingValues->add(insn)) {
        return AnalysisResult::OOM;
      }
    }
  }

  // Due to limitations in AddClosingPhisForLoop (see comments there),
  // currently we can only unroll loops where, either:
  //
  // * no value defined in the loop is used afterwards, in which case we handle
  //   any number of exits from the loop.
  //
  // * one or more values defined in the loop is used afterwards, in which case
  //   there must be one exit point from the loop.
  //
  // IOW we can't handle multiple exits *and* values defined in the loop.
  if (exitTargetBlocks->size() >= 2 && exitingValues->size() > 0) {
    return AnalysisResult::TooComplex;
  }

  // There is an invariant that a loop exit target block does not contain any
  // phis.  This is really just the no-critical-edges invariant.
  //
  // By definition, a loop exit block has at least one successor in the loop
  // and at least one successor outside the loop.  If the loop exit target has
  // phis, that would imply it has multiple predecessors.  But then the edge
  // would be critical, because it connects a block with multiple successors to
  // a block with multiple predecessors.
  //
  // So enforce that; it simplifies the addition of loop-closing phis that will
  // happen shortly.
  for (size_t i = 0; i < exitTargetBlocks->size(); i++) {
    MBasicBlock* exitTargetBlock = exitTargetBlocks->get(i);
    if (!exitTargetBlock->phisEmpty()) {
      return AnalysisResult::BadInvariants;
    }
    // Also we need this, since we'll be adding single-argument phis to this
    // block during loop-closing.
    if (exitTargetBlock->numPredecessors() != 1) {
      return AnalysisResult::BadInvariants;
    }
  }

  // At this point, we've worked through all invariant- and structural- reasons
  // why we can't/shouldn't unroll/peel this loop.  Now we need to decide which
  // (or both) of those actions should happen.
  bool peel = true;
  bool unroll = true;
  if (numBlocksInOriginal > MaxBlocksForPeel ||
      numValuesInOriginal > MaxValuesForPeel) {
    peel = false;
  }
  if (numBlocksInOriginal > MaxBlocksForUnroll ||
      numValuesInOriginal > MaxValuesForUnroll) {
    unroll = false;
  }

  if (peel) {
    return unroll ? AnalysisResult::PeelAndUnroll : AnalysisResult::Peel;
  } else {
    return unroll ? AnalysisResult::Unroll : AnalysisResult::TooLarge;
  }
}

// =====================================================================
//
// AddClosingPhisForLoop

// Add "loop-closing phis" for values defined in a loop and used afterwards.
// In GCC parlance, the resulting form is called LCSSA (Loop-Closed SSA).  This
// is a very limited intervention in that it can only handle a single exit in a
// loop, thereby restricting the places where the phis should go to the
// one-and-only exit block, and avoiding the need to compute iterated dominance
// frontiers.

[[nodiscard]]
static bool AddClosingPhisForLoop(TempAllocator& alloc,
                                  const UnrollState& state) {
  // If no values exit the loop, we don't care how many exit target blocks
  // there are, and need to make no changes.
  if (state.exitingValues.empty()) {
    return true;
  }

  // Ensured by AnalyzeLoop
  MOZ_ASSERT(state.exitTargetBlocks.size() == 1);
  MBasicBlock* targetBlock = state.exitTargetBlocks.get(0);
  // Ensured by AnalyzeLoop.  Required because we're adding single-arg phis
  // to `targetBlock`.
  MOZ_ASSERT(targetBlock->numPredecessors() == 1);

  for (size_t i = 0; i < state.exitingValues.size(); i++) {
    MDefinition* exitingValue = state.exitingValues.get(i);
    // In `targetBlock`, add a single-argument phi node that "catches"
    // `exitingValue`, and change all uses of `exitingValue` to be the phi
    // node instead.
    MPhi* phi = MPhi::New(alloc, exitingValue->type());
    if (!phi) {
      return false;
    }
    targetBlock->addPhi(phi);
    // Change all uses of `exitingValue` outside of the loop into uses of
    // `phi`.
    for (MUseIterator useIter(exitingValue->usesBegin()),
         useIterEnd(exitingValue->usesEnd());
         useIter != useIterEnd;
         /**/) {
      MUse* use = *useIter++;
      if (state.blockTable.rowContains(0, use->consumer()->block())) {
        continue;
      }
      MOZ_ASSERT(use->consumer());
      use->replaceProducer(phi);
    }
    // Finally, make `phi` be the one-and-only user of `exitingValue`.
    if (!phi->addInputFallible(exitingValue)) {
      return false;
    }
  }

  // Invariant that should hold at this point: for each exiting value, all uses
  // of it outside the loop should only be phi nodes.
  for (size_t i = 0; i < state.exitingValues.size(); i++) {
    MDefinition* exitingValue = state.exitingValues.get(i);
    for (MUseIterator useIter(exitingValue->usesBegin());
         useIter != exitingValue->usesEnd(); useIter++) {
      mozilla::DebugOnly<MUse*> use = *useIter;
      MOZ_ASSERT_IF(!state.blockTable.rowContains(0, use->consumer()->block()),
                    use->consumer()->isDefinition() &&
                        use->consumer()->toDefinition()->isPhi());
    }
  }

  return true;
}

// =====================================================================
//
// UnrollAndOrPeelLoop

[[nodiscard]]
static bool UnrollAndOrPeelLoop(MIRGraph& graph, UnrollState& state) {
  // Prerequisites (assumed):
  // * AnalyzeLoop has approved this loop for peeling and/or unrolling
  // * AddClosingPhisForLoop has been called for it
  //
  // We expect `state` to be ready to go, with the caveat that only the first
  // row of entries in `state.blockTable` have been filled in (these are the
  // original loop blocks).  All other entries are null and will be filled in
  // by this routine.
  //
  // Almost all the logic here is to do with unrolling.  That's because peeling
  // (done far below) can be done by a trivial modification of the loop
  // resulting from unrolling: make the backedge jump to the second copy of the
  // loop body rather than the first.  That leaves the first copy (that is, the
  // original loop body) in a "will only be executed once" state, as we desire.

  // We need this so often it's worth pulling out specially.
  const MBasicBlock* originalHeader = state.blockTable.get(0, 0);
  MOZ_ASSERT(originalHeader->isLoopHeader());

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_UnrollDetails)) {
    Fprinter& printer(JitSpewPrinter());
    printer.printf(
        "<<<< ORIGINAL FUNCTION (after LCSSA-ification of chosen loops)\n");
    DumpMIRGraph(printer, graph, /*showHashedPointers=*/true);
    printer.printf(">>>>\n");
  }
#endif

  // Decide on the total unroll factor.  "Copy #0" is the original loop, so the
  // number of new copies is `unrollFactor - 1`.  For example, if the original
  // loop was `loop { B; }` then with `unrollFactor == 3`, the unrolled loop
  // will be `loop { B; B; B; }`.  Note that the value acquired here includes
  // the copy needed for peeling, if that has been requested.
  const uint32_t unrollFactor = state.blockTable.size1();
  // The logic below will fail if this doesn't hold.
  MOZ_ASSERT(unrollFactor >= 2);

  // The number of blocks in the original loop.
  const uint32_t numBlocksInOriginal = state.blockTable.size2();

  // The number of values (phis and normal values) in the original loop.
  uint32_t numValuesInOriginal = 0;
  for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
    MBasicBlock* block = state.blockTable.get(0, bix);
    // This is very feeble, but I can't think of anything better.
    for (MPhiIterator phiIter(block->phisBegin()); phiIter != block->phisEnd();
         phiIter++) {
      numValuesInOriginal++;
    }
    for (MInstructionIterator insnIter(block->begin());
         insnIter != block->end(); insnIter++) {
      numValuesInOriginal++;
    }
  }
  // `numValuesInOriginal` is const after this point.

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_UnrollDetails)) {
    DumpBlockTableRowZero(state.blockTable, "ORIGINAL LOOP");
    Fprinter& printer(JitSpewPrinter());
    printer.printf("<<<< EXIT TARGET BLOCKS: ");
    for (size_t i = 0; i < state.exitTargetBlocks.size(); i++) {
      MBasicBlock* targetBlock = state.exitTargetBlocks.get(i);
      DumpMIRBlockID(printer, targetBlock, /*showHashedPointers=*/true);
      printer.printf(" ");
    }
    printer.printf(">>>>\n");
    printer.printf("<<<< EXITING VALUES: ");
    for (size_t i = 0; i < state.exitingValues.size(); i++) {
      MDefinition* exitingValue = state.exitingValues.get(i);
      DumpMIRDefinitionID(printer, exitingValue, /*showHashedPointers=*/true);
      printer.printf(" ");
    }
    printer.printf(">>>>\n");
  }
#endif

  // At this point, `state.blockTable` row zero contains the original blocks,
  // and all other rows are nulled out.

  // Set up the value table.  This is a structure similar to
  // `state.blockTable`: a 2-D array of values.  This is local to this routine
  // rather than being part of `state` because it is only needed here, and can
  // take quite a lot of space.
  //
  // As with the block table, the main (actually, only) purpose of the value
  // table is to make it possible to find equivalent definitions in different
  // iterations.  Also as above, "value index" (`vix` in the code) is a
  // zero-based index into the sequence of values (including phis) in the loop
  // as generated by the RPO traversal of the loop's blocks.

  ValueTable valueTable;
  if (!valueTable.init(unrollFactor, numValuesInOriginal)) {
    return false;
  }

  // Copy the original values into row zero of `valueTable`, parking them
  // end-to-end, without regard to block boundaries.  All other rows remain
  // nulled out for now.
  {
    uint32_t vix = 0;
    for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
      MBasicBlock* block = state.blockTable.get(0, bix);
      for (MPhiIterator phiIter(block->phisBegin());
           phiIter != block->phisEnd(); phiIter++) {
        valueTable.set(0, vix, *phiIter);
        vix++;
      }
      for (MInstructionIterator insnIter(block->begin());
           insnIter != block->end(); insnIter++) {
        valueTable.set(0, vix, *insnIter);
        vix++;
      }
    }
    MOZ_ASSERT(vix == numValuesInOriginal);
  }

  // Set up the value remapper.  This maps MDefinitions* in the original body
  // to their "current" definition in new bodies.  The only allowed keys are
  // MDefinition*s from the original body.  To enforce this, the keys have to
  // be "registered" before use.
  MDefinitionRemapper mapper;
  for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
    MBasicBlock* originalBlock = state.blockTable.get(0, bix);
    // Create initial entries in `mapper` for both the normal insns and phi
    // nodes in `originalBlock`.
    for (MPhiIterator phiIter(originalBlock->phisBegin());
         phiIter != originalBlock->phisEnd(); phiIter++) {
      MPhi* originalPhi = *phiIter;
      if (!mapper.enregister(originalPhi)) {
        return false;
      }
    }
    for (MInstructionIterator insnIter(originalBlock->begin());
         insnIter != originalBlock->end(); insnIter++) {
      MInstruction* originalInsn = *insnIter;
      if (!mapper.enregister(originalInsn)) {
        return false;
      }
    }
  }

  // Now we begin with the very core of the unrolling activity.

  // Create new empty blocks for copy indices 1 .. (unrollFactor-1)
  const CompileInfo& info = originalHeader->info();
  for (uint32_t cix = 1; cix < unrollFactor; cix++) {
    for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
      MBasicBlock* empty = MBasicBlock::New(graph, info, /*pred=*/nullptr,
                                            MBasicBlock::Kind::NORMAL);
      if (!empty) {
        return false;
      }
      empty->setLoopDepth(state.blockTable.get(0, bix)->loopDepth());
      // We don't bother to set the block's ID, because the logic below not
      // depend on it. In any case the blocks will get renumbered at the end of
      // UnrollLoops, so it doesn't matter what we choose here.  Unrolling
      // still works even if we don't set the ID to anything.
      MOZ_ASSERT(!state.blockTable.get(cix, bix));
      state.blockTable.set(cix, bix, empty);
    }
  }

  // Copy verbatim, blocks in the original loop, into our new copies.
  // For each copy of the loop ..
  for (uint32_t cix = 1; cix < unrollFactor; cix++) {
    // For each block in the original copy ..
    uint32_t vix = 0;
    for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
      MBasicBlock* originalBlock = state.blockTable.get(0, bix);
      MBasicBlock* clonedBlock = state.blockTable.get(cix, bix);

      // Clone the phi nodes, but don't update the mapper until all original
      // arguments have been looked up.  This is required because the phi nodes
      // collectively implement a parallel assignment; there is no implied
      // sequentiality.
      mozilla::Vector<std::pair<const MPhi*, MPhi*>, 16, SystemAllocPolicy>
          mapperUpdates;
      for (MPhiIterator phiIter(originalBlock->phisBegin());
           phiIter != originalBlock->phisEnd(); phiIter++) {
        const MPhi* originalPhi = *phiIter;
        MPhi* clonedPhi =
            MakeReplacementPhi(graph.alloc(), mapper, originalPhi);
        if (!clonedPhi) {
          return false;
        }
        clonedBlock->addPhi(clonedPhi);
        MOZ_ASSERT(!valueTable.get(cix, vix));
        valueTable.set(cix, vix, clonedPhi);
        vix++;
        if (!mapperUpdates.append(std::pair(originalPhi, clonedPhi))) {
          return false;
        }
      }
      for (auto& p : mapperUpdates) {
        // Update the value mapper.  The `originalPhi` values must be part of
        // the original loop body and so must already have a key in `mapper`.
        mapper.update(p.first, p.second);
      }

      // Cloning the instructions is simpler, since we can incrementally update
      // the mapper.
      for (MInstructionIterator insnIter(originalBlock->begin());
           insnIter != originalBlock->end(); insnIter++) {
        const MInstruction* originalInsn = *insnIter;
        MInstruction* clonedInsn =
            MakeReplacementInstruction(graph.alloc(), mapper, originalInsn);
        if (!clonedInsn) {
          return false;
        }
        clonedBlock->insertAtEnd(clonedInsn);
        MOZ_ASSERT(!valueTable.get(cix, vix));
        valueTable.set(cix, vix, clonedInsn);
        vix++;
        // Update the value mapper.  `originalInsn` must be part of the
        // original loop body and so must already have a key in `mapper`.
        mapper.update(originalInsn, clonedInsn);
      }

      // Clone the block's predecessor array
      MOZ_ASSERT(clonedBlock->numPredecessors() == 0);
      for (uint32_t i = 0; i < originalBlock->numPredecessors(); i++) {
        MBasicBlock* pred = originalBlock->getPredecessor(i);
        if (!clonedBlock->appendPredecessor(pred)) {
          return false;
        }
      }
    }

    MOZ_ASSERT(vix == numValuesInOriginal);

    // Check we didn't mess up the valueTable ordering relative to the original
    // (best effort assertion).
    for (vix = 0; vix < numValuesInOriginal; vix++) {
      MOZ_ASSERT(valueTable.get(cix, vix)->op() ==
                 valueTable.get(0, vix)->op());
    }
  }

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_UnrollDetails)) {
    DumpValueTable(valueTable, "VALUE TABLE (final)");
  }
#endif

  // Fix up (remap) inter-block edges in both the original and copies, using
  // uniform indexing as above (original = index 0, copies = indices >= 1):

  // For a block with copy-index N, inspect each edge.  Determine the role the
  // edge-destination (target) plays in the original copy:
  //
  // * backedge
  //     -> remap to header node of copy (N + 1) % unrollFactor
  //        (so, generates a backedge in the last copy)
  // * internal jump within the original body
  //     -> remap to corresponding block in copy N
  // * jump to after the original body
  //     -> unchanged (it is an exiting edge)
  // * jump to before the original body
  //     -> this can't happen
  //        (but if it did, it would be just another exit block)
  auto RemapEdgeDestination =
      [=, &state](uint32_t cix, MBasicBlock* oldDestination) -> MBasicBlock* {
    // `oldDestination` is the target of a control-flow insn in the original
    // loop.  Figure out what its replacement should be, for copy index `cix`
    // (with `cix == 0` meaning the original copy).

    // If it is a back edge, remap to the header node of the next copy, with
    // wraparound for the last copy.
    if (oldDestination == state.blockTable.get(0, 0)) {
      MOZ_ASSERT(oldDestination == originalHeader);
      return state.blockTable.get((cix + 1) % unrollFactor, 0);
    }

    // If it is to some other node within the original body, remap to the
    // corresponding node in this copy.
    for (uint32_t i = 1; i < numBlocksInOriginal; i++) {
      if (oldDestination == state.blockTable.get(0, i)) {
        return state.blockTable.get(cix, i);
      }
    }

    // The target of the edge is not in the original loop.  Either it's to
    // after the loop (an exiting branch) or it is to before the loop, although
    // that would be bizarre and probably illegal.  Either way, leave it
    // unchanged.
    return oldDestination;
  };

  // Similarly, for an edge that claims to come from `oldSource` in the
  // original loop, determine where it should come from if we imagine that it
  // applies to copy index `cix`.
  auto RemapEdgeSource = [=, &state](uint32_t cix,
                                     MBasicBlock* oldSource) -> MBasicBlock* {
    if (oldSource == state.blockTable.get(0, numBlocksInOriginal - 1)) {
      // Source is backedge node in the original loop.  Remap to the backedge
      // node in the previous iteration.
      MOZ_ASSERT(oldSource == originalHeader->backedge());
      return state.blockTable.get(cix == 0 ? (unrollFactor - 1) : (cix - 1),
                                  numBlocksInOriginal - 1);
    }
    for (uint32_t i = 0; i < numBlocksInOriginal - 1; i++) {
      if (oldSource == state.blockTable.get(0, i)) {
        // Source is a non-backedge node in the original loop.  Remap it to
        // the equivalent node in this copy.
        return state.blockTable.get(cix, i);
      }
    }
    // Source is not in the original loop.  Leave unchanged.
    return oldSource;
  };

  // Work through all control-flow instructions in all copies and remap their
  // edges.
  // For each copy of the loop, including the original ..
  for (uint32_t cix = 0; cix < unrollFactor; cix++) {
    // For each block in the copy ..
    for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
      MBasicBlock* b = state.blockTable.get(cix, bix);
      MControlInstruction* lastI = b->lastIns();
      if (lastI->isGoto()) {
        MGoto* insn = lastI->toGoto();
        insn->setTarget(RemapEdgeDestination(cix, insn->target()));
      } else if (lastI->isTest()) {
        MTest* insn = lastI->toTest();
        insn->setIfTrue(RemapEdgeDestination(cix, insn->ifTrue()));
        insn->setIfFalse(RemapEdgeDestination(cix, insn->ifFalse()));
      } else {
        // The other existing control instructions are MTableSwitch,
        // MUnreachable, MWasmTrap, MWasmReturn, and MWasmReturnVoid.
        // MTableSwitch is ruled out above.  The others end a block without a
        // successor, which means that they can't be loop blocks
        // (MarkLoopBlocks marks blocks that are predecessors of the back
        // edge.)  So we don't expect to see them here.
        MOZ_CRASH();
      }
    }
  }

  // Also we need to remap within the each block's predecessors vector.
  // We have to do this backwards to stop RemapEdgeSource from asserting.
  // For each copy of the loop, including the original ..
  for (int32_t cix = unrollFactor - 1; cix >= 0; cix--) {
    // For each block in the copy ..
    for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
      MBasicBlock* block = state.blockTable.get(cix, bix);
      for (uint32_t i = 0; i < block->numPredecessors(); i++) {
        MBasicBlock* oldPred = block->getPredecessor(i);
        MBasicBlock* newPred = RemapEdgeSource(cix, oldPred);
        block->setPredecessor(i, newPred);
      }
    }
  }

  // Clean up the header node copies (but not for the original copy) since they
  // all still claim to have two predecessors, the first of which is the entry
  // edge from outside the loop.  This is no longer true, so delete that
  // predecessor.  Remove the corresponding Phi args too.  This leaves
  // one-argument phis, meh.
  for (uint32_t cix = 1; cix < unrollFactor; cix++) {
    MBasicBlock* block = state.blockTable.get(cix, 0);
    MOZ_ASSERT(block->numPredecessors() == 2);  // from AnalyzeLoop
    block->erasePredecessor(0);
    MOZ_ASSERT(block->numPredecessors() == 1);  // duh
    for (MPhiIterator phiIter(block->phisBegin()); phiIter != block->phisEnd();
         phiIter++) {
      MPhi* phi = *phiIter;
      MOZ_ASSERT(phi->numOperands() == 2);  // from AnalyzeLoop
      phi->removeOperand(0);
      MOZ_ASSERT(phi->numOperands() == 1);
    }
  }

  // Fix up the phi nodes in the original header; the previous step did not do
  // that.  Specifically, the second arg in each phi -- which are values that
  // flow in from the backedge block -- need to be the ones from the last body
  // copy; but currently they are from the original copy.  At this point, the
  // `mapper` should be able to tell us the correct value.
  for (MPhiIterator phiIter(originalHeader->phisBegin());
       phiIter != originalHeader->phisEnd(); phiIter++) {
    MPhi* phi = *phiIter;
    MOZ_ASSERT(phi->numOperands() == 2);  // from AnalyzeLoop
    // phi arg[0] is defined before the loop.  Ensured by AnalyzeLoop.
    MOZ_ASSERT(!state.blockTable.rowContains(0, phi->getOperand(0)->block()));
    // phi arg[1] is almost always defined in the loop (hence it is a
    // loop-carried value) but is very occasionally defined before the loop
    // (so it is unrelated to the loop).  Proceed with maximum paranoia.
    MDefinition* operand1 = phi->getOperand(1);
    MDefinition* replacement = mapper.lookup(operand1);
    // If we didn't find a replacement (unlikely but possible) then
    // `operand1` must be defined before the loop.
    MOZ_ASSERT((replacement == nullptr) ==
               !state.blockTable.rowContains(0, operand1->block()));
    if (replacement) {
      phi->replaceOperand(1, replacement);
    }
  }

  // Now we need to fix up the exit target blocks, by adding sources of exiting
  // edges to their `predecessors_` vectors and updating their phi nodes
  // accordingly.  Recall that those phi nodes were previously installed in
  // these blocks by AddClosingPhisForLoop.
  //
  // Currently the exit target blocks mention only predecessors in the original
  // loop, but they also need to mention equivalent predecessors in each of the
  // loop copies.
  for (uint32_t cix = 0; cix < unrollFactor; cix++) {
    for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
      MBasicBlock* block = state.blockTable.get(cix, bix);
      MControlInstruction* lastIns = block->lastIns();
      for (size_t i = 0; i < lastIns->numSuccessors(); i++) {
        MBasicBlock* succ = lastIns->getSuccessor(i);
        if (!state.exitTargetBlocks.contains(succ)) {
          // This isn't an exiting edge -- not interesting.
          continue;
        }
        // If `cix` is zero (the original body), we expect `succ` to already
        // list `block` as a predecessor.  If `cix` is nonzero (a body copy)
        // then we expect `succ` *not* to list it, and we must add it.
        mozilla::DebugOnly<bool> found = false;
        for (uint32_t j = 0; j < succ->numPredecessors(); j++) {
          if (block == succ->getPredecessor(j)) {
            found = true;
            break;
          }
        }
        MOZ_ASSERT((cix == 0) == found);
        if (cix > 0) {
          if (!succ->appendPredecessor(block)) {
            return false;
          }
          for (MPhiIterator phiIter(succ->phisBegin());
               phiIter != succ->phisEnd(); phiIter++) {
            MPhi* phi = *phiIter;
            // "+ 1" because we just added a predecessor
            MOZ_ASSERT(phi->numOperands() + 1 == succ->numPredecessors());
            MDefinition* exitingValue = phi->getOperand(0);
            MOZ_ASSERT(state.exitingValues.contains(exitingValue));
            // `exitingValue` is defined in the original loop body.  We need to
            // find the equivalent value in copy `cix` so we can add it as an
            // argument to the phi.  This bit is the one-and-only reason for
            // `valueTable`s existence.
            mozilla::Maybe<size_t> colIndex =
                valueTable.findInRow(0, exitingValue);
            MOZ_ASSERT(colIndex.isSome());  // We must find it!
            MDefinition* equivValue = valueTable.get(cix, colIndex.value());
            if (!phi->addInputFallible(equivValue)) {
              return false;
            }
          }
        }
      }
    }
  }

  // At this point the control flow is correct.  But we may now have critical
  // edges where none existed before.  Consider a loop {Block1, Block2} with an
  // exit target Block9:
  //
  //   Block1
  //     stuff1
  //     Goto Block2
  //   Block2
  //     stuff2
  //     Test true->Block1(== continue) false->Block9(== break)
  //   ...
  //   Block9
  //     this is a loop-exit target
  //     preds = Block2
  //
  // After factor-of-2 unrolling:
  //
  //   Block1
  //     stuff1
  //     Goto Block2
  //   Block2
  //     stuff2
  //     Test true->Block3(== continue) false->Block9(== break)
  //   Block3
  //     stuff3
  //     Goto Block4
  //   Block4
  //     stuff4
  //     Test true->Block1(== continue) false->Block9(== break)
  //   ...
  //   Block9
  //     preds = Block2, Block4
  //
  // Now the exiting edges (Block2 to Block9 and Block4 to Block9) are critical.
  //
  // The obvious fix is to insert a new block on each exiting edge.

  mozilla::Vector<MBasicBlock*, 16, SystemAllocPolicy> splitterBlocks;
  for (uint32_t cix = 0; cix < unrollFactor; cix++) {
    for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
      MBasicBlock* block = state.blockTable.get(cix, bix);
      // Inspect all successors of `block`.  If any are exiting edges, create a
      // new block and route through that instead.
      MControlInstruction* lastIns = block->lastIns();
      for (size_t i = 0; i < lastIns->numSuccessors(); i++) {
        MBasicBlock* succ = lastIns->getSuccessor(i);
        if (!state.exitTargetBlocks.contains(succ)) {
          // This isn't an exiting edge -- not interesting.
          continue;
        }
        // Invent a new block.
        MBasicBlock* splitter =
            MBasicBlock::New(graph, info, block, MBasicBlock::Kind::SPLIT_EDGE);
        if (!splitter || !splitterBlocks.append(splitter)) {
          return false;
        }
        splitter->setLoopDepth(succ->loopDepth());
        // As with empty-block creation earlier in this routine, we don't
        // bother to set the block's ID since none of the logic depends on it,
        // and it will eventually be renumbered anyway by UnrollLoops.
        MGoto* jump = MGoto::New(graph.alloc(), succ);
        if (!jump) {
          return false;
        }
        splitter->insertAtEnd(jump);
        // For the target, fix up the `predecessor_` entry.
        mozilla::DebugOnly<bool> found = false;
        for (uint32_t j = 0; j < succ->numPredecessors(); j++) {
          if (succ->getPredecessor(j) == block) {
            succ->setPredecessor(j, splitter);
            found = true;
            break;
          }
        }
        // Finally, reroute the jump to `succ` via `splitter`.
        lastIns->replaceSuccessor(i, splitter);
        MOZ_ASSERT(found);
      }
    }
  }

  // Now the control flow is correct *and* we don't have any critical edges.
  // Fix up the `successorWithPhis_` and `positionInPhiSuccessor_` fields in
  // both the unrolled loop and the splitter blocks, thusly:
  //
  // For all blocks `B` in the unrolled loop, and for all splitter blocks, fix
  // up the `B.successorWithPhis_` and `B.positionInPhiSuccessor_` fields.
  // Each block may have at maximum one successor that has phi nodes, in which
  // case `B.successorWithPhis_` should point at that block.  And
  // `B.positionInPhiSuccessor_` should indicate the index that successor's
  // `predecessors_` vector, of B.
  {
    // Make up a list of blocks to fix up, then process them.
    mozilla::Vector<MBasicBlock*, 32 + 16, SystemAllocPolicy> workList;
    for (uint32_t cix = 0; cix < unrollFactor; cix++) {
      for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
        MBasicBlock* block = state.blockTable.get(cix, bix);
        if (!workList.append(block)) {
          return false;
        }
      }
    }
    for (MBasicBlock* block : splitterBlocks) {
      if (!workList.append(block)) {
        return false;
      }
    }
    // Now process them.
    for (MBasicBlock* block : workList) {
      // Inspect `block`s successors.
      MBasicBlock* succWithPhis = nullptr;
      MControlInstruction* lastIns = block->lastIns();
      // Determine `succWithPhis`, if one exists.
      for (size_t i = 0; i < lastIns->numSuccessors(); i++) {
        MBasicBlock* succ = lastIns->getSuccessor(i);
        if (succ->phisEmpty()) {
          continue;
        }
        MOZ_ASSERT(succ);
        // `succ` is a successor to `block` that has phis.  So we can't already
        // have seen a (different) successor.
        if (!succWithPhis) {
          succWithPhis = succ;
        } else if (succWithPhis == succ) {
          // ignore duplicate successors
        } else {
          // More than one successor with phis.  Structural invariants
          // invalidated.  Give up.
          MOZ_CRASH();
        }
      }
      // Annotate `block`.
      if (!succWithPhis) {
        block->setSuccessorWithPhis(nullptr, 0);
      } else {
        MOZ_ASSERT(!succWithPhis->phisEmpty());
        // Figure out which of `succWithPhis`s predecessors `block` is.
        // Should we assert `succWithPhis->predecessors_` is duplicate-free?
        uint32_t predIx = UINT32_MAX;  // invalid
        for (uint32_t i = 0; i < succWithPhis->numPredecessors(); i++) {
          if (succWithPhis->getPredecessor(i) == block) {
            predIx = i;
            break;
          }
        }
        MOZ_ASSERT(predIx != UINT32_MAX);
        block->setSuccessorWithPhis(succWithPhis, predIx);
      }
    }
  }

  // Find and remove the MWasmInterruptCheck in all but the last iteration.
  // We don't assume that there is an interrupt check, since
  // wasm::FunctionCompiler::fillArray, at least, generates a loop with no
  // check.
  for (uint32_t cix = 0; cix < unrollFactor - 1; cix++) {
    for (uint32_t vix = 0; vix < numValuesInOriginal; vix++) {
      MDefinition* ins = valueTable.get(cix, vix);
      if (!ins->isWasmInterruptCheck()) {
        continue;
      }
      MWasmInterruptCheck* ic = ins->toWasmInterruptCheck();
      ic->block()->discard(ic);
      valueTable.set(cix, vix, nullptr);
    }
  }

  // We're all done with unrolling.  If peeling was also requested, make a
  // minor modification to the unrolled loop: change the backedge so that,
  // instead of jumping to the first body copy, make it jump to the second body
  // copy.  This leaves the first body copy in a
  // will-only-ever-be-executed-once state, hence achieving peeling.
  if (state.doPeeling()) {
    MBasicBlock* backedge =
        state.blockTable.get(unrollFactor - 1, numBlocksInOriginal - 1);
    MBasicBlock* header0 = state.blockTable.get(0, 0);
    MBasicBlock* header1 = state.blockTable.get(1, 0);
    MOZ_ASSERT(header0 == originalHeader);

    // Fix up the back edge, to make it jump to the second copy of the loop
    // body
    MOZ_ASSERT(backedge->hasLastIns());  // should always hold (AnalyzeLoop)
    MOZ_ASSERT(backedge->lastIns()->isGoto());  // AnalyzeLoop ensures
    MGoto* backedgeG = backedge->lastIns()->toGoto();
    MOZ_ASSERT(backedgeG->target() == header0);  // AnalyzeLoop ensures
    backedgeG->setTarget(header1);
    header0->clearLoopHeader();
    header1->setLoopHeader();
    for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
      MBasicBlock* originalBlock = state.blockTable.get(0, bix);
      MOZ_ASSERT(originalBlock->loopDepth() > 0);
      originalBlock->setLoopDepth(originalBlock->loopDepth() - 1);
    }

    // Fix up the preds of the original header
    MOZ_ASSERT(header0->numPredecessors() == 2);
    MOZ_ASSERT(header0->getPredecessor(1) == backedge);
    header0->erasePredecessor(1);

    // Fix up the preds of the new header
    MOZ_ASSERT(header1->numPredecessors() == 1);
    if (!header1->appendPredecessor(backedge)) {
      return false;
    }

    // Fix up phis of both headers, by moving the second operand of each phi of
    // `header0` to be the second operand of the corresponding phi in
    // `header1`.
    MPhiIterator phiIter0(header0->phisBegin());
    MPhiIterator phiIter1(header1->phisBegin());
    while (true) {
      bool finished0 = phiIter0 == header0->phisEnd();
      mozilla::DebugOnly<bool> finished1 = phiIter1 == header1->phisEnd();
      // The two blocks must have the same number of phis.
      MOZ_ASSERT(finished0 == finished1);
      if (finished0) {
        break;
      }
      MPhi* phi0 = *phiIter0;
      MPhi* phi1 = *phiIter1;
      MOZ_ASSERT(phi0->numOperands() == 2);
      MOZ_ASSERT(phi1->numOperands() == 1);
      MDefinition* phi0arg1 = phi0->getOperand(1);
      phi0->removeOperand(1);
      if (!phi1->addInputFallible(phi0arg1)) {
        return false;
      }
      phiIter0++;
      phiIter1++;
    }

    // Fix the succ-w-phis entry for `backEdge`.  In very rare circumstances,
    // the loop may have no header phis, so this must be conditional.
    if (backedge->successorWithPhis()) {
      MOZ_ASSERT(backedge->successorWithPhis() == header0);
      MOZ_ASSERT(backedge->positionInPhiSuccessor() == 1);
      backedge->setSuccessorWithPhis(header1, 1);
    }
  }

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_UnrollDetails)) {
    DumpBlockTable(state.blockTable, "LOOP BLOCKS (final)");
    Fprinter& printer(JitSpewPrinter());
    printer.printf("<<<< SPLITTER BLOCKS\n");
    for (MBasicBlock* block : splitterBlocks) {
      DumpMIRBlock(printer, block, /*showHashedPointers=*/true);
    }
    printer.printf(">>>>\n");
  }
#endif

  // Add the new blocks to the graph.
  {
    // We want to add them to the end of the existing loop.
    MBasicBlock* cursor = state.blockTable.get(0, numBlocksInOriginal - 1);
    for (uint32_t cix = 1; cix < unrollFactor; cix++) {
      for (uint32_t bix = 0; bix < numBlocksInOriginal; bix++) {
        MBasicBlock* addMe = state.blockTable.get(cix, bix);
        graph.insertBlockAfter(cursor, addMe);
        cursor = addMe;
      }
    }
    for (MBasicBlock* addMe : splitterBlocks) {
      graph.insertBlockAfter(cursor, addMe);
      cursor = addMe;
    }
  }

  // UnrollLoops will renumber the blocks and redo the dominator tree once all
  // loops have been unrolled.

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_UnrollDetails)) {
    Fprinter& printer(JitSpewPrinter());
    printer.printf("<<<< FUNCTION AFTER UNROLLING\n");
    DumpMIRGraph(printer, graph, /*showHashedPointers=*/true);
    printer.printf(">>>>\n");
  }
#endif

  return true;
}

// =====================================================================
//
// FindInitialCandidates, which finds "initial candidates" for
// unrolling/peeling.  See SMDOC at the top of this file.

struct InitialCandidate {
  MBasicBlock* header;
  uint32_t numBlocks;
  float score;
  bool valid;
  InitialCandidate(MBasicBlock* header, uint32_t numBlocks)
      : header(header), numBlocks(numBlocks), score(0.0), valid(true) {}
};

using InitialCandidateVector =
    mozilla::Vector<InitialCandidate, 64, SystemAllocPolicy>;

[[nodiscard]]
bool FindInitialCandidates(MIRGraph& graph,
                           InitialCandidateVector& initialCandidates) {
  // Find initial candidate loops, and place them in `initialCandidates`.
  MOZ_ASSERT(initialCandidates.empty());

  // First, using a single scan over the blocks, find blocks that are the
  // headers of innermost loops.  This uses the same logic to find innermost
  // loops as BacktrackingAllocator::init.
  //
  // The general idea is: we traverse the entire graph in RPO order, but ignore
  // all blocks except loop headers and loop backedge blocks.  At a header we
  // make a note of the backedge block it points at.  Because the loops are
  // properly nested, this means that we will ignore backedges for
  // non-innermost loops, and, so, when we do come to a block that matches
  // `backedge`, we know we've identified an innermost loop.
  //
  // At the same time, release-assert that the blocks are numbered
  // sequentially.  We need this for the loop-non-overlapping check below.
  mozilla::Vector<MBasicBlock*, 128, SystemAllocPolicy> initialHeaders;
  {
    MBasicBlock* backedge = nullptr;
    uint32_t expectedNextId = 0;
    for (auto rpoIter(graph.rpoBegin()), rpoIterEnd(graph.rpoEnd());
         rpoIter != rpoIterEnd; ++rpoIter) {
      MBasicBlock* block = *rpoIter;
      MOZ_RELEASE_ASSERT(block->id() == expectedNextId);
      expectedNextId++;

      if (block->isLoopHeader()) {
        backedge = block->backedge();
      }
      if (block == backedge) {
        if (!initialHeaders.append(block->loopHeaderOfBackedge())) {
          return false;
        }
      }
    }
  }

  // Now use MarkLoopBlocks to check each potential loop in more detail, and,
  // for each non-empty, non-OSR one, create an entry in initialCandidates for
  // it.
  for (MBasicBlock* header : initialHeaders) {
    MOZ_ASSERT(header->isLoopHeader());

    bool hasOsrEntry;
    size_t numBlocks = MarkLoopBlocks(graph, header, &hasOsrEntry);
    if (numBlocks == 0) {
      // Bogus; skip.
      continue;
    }
    UnmarkLoopBlocks(graph, header);
    if (hasOsrEntry) {
      // Unhandled; skip.
      continue;
    }

    MOZ_RELEASE_ASSERT(numBlocks <= UINT32_MAX);
    if (!initialCandidates.append(InitialCandidate(header, numBlocks))) {
      return false;
    }
  }

  // Sort the loops by header ID.
  std::sort(initialCandidates.begin(), initialCandidates.end(),
            [](const InitialCandidate& cand1, const InitialCandidate& cand2) {
              return cand1.header->id() < cand2.header->id();
            });

  // Now we can conveniently assert non-overlappingness.  Note that this
  // depends on the property that each loop is a sequential sequence of block
  // IDs, as we release-asserted above.  Presence of an overlap might indicate
  // that a non-innermost loop made it this far.
  for (size_t i = 1; i < initialCandidates.length(); i++) {
    MOZ_RELEASE_ASSERT(initialCandidates[i - 1].header->id() +
                           initialCandidates[i - 1].numBlocks <=
                       initialCandidates[i].header->id());
  }

  // Heuristics: calculate a "score" for each loop, indicating our desire to
  // unroll it.  **Lower** values indicate a higher desirability for unrolling.
  //
  // A loop gets a lower score if:
  // * it is small
  // * it has few blocks
  // * it is deeply nested (currently ignored)

  for (InitialCandidate& cand : initialCandidates) {
    uint32_t nBlocks = cand.numBlocks;
    uint32_t nInsns = 0;
    uint32_t nPhis = 0;

    // Visit each block in the loop
    mozilla::DebugOnly<uint32_t> numBlocksVisited = 0;
    const MBasicBlock* backedge = cand.header->backedge();

    for (auto i(graph.rpoBegin(cand.header)); /**/; ++i) {
      MOZ_ASSERT(i != graph.rpoEnd(),
                 "UnrollLoops: loop blocks overrun graph end");
      MBasicBlock* block = *i;
      numBlocksVisited++;
      for (MInstructionIterator iter(block->begin()); iter != block->end();
           iter++) {
        nInsns++;
      }
      for (MPhiIterator iter(block->phisBegin()); iter != block->phisEnd();
           iter++) {
        nPhis++;
      }

      if (block == backedge) {
        break;
      }
    }

    MOZ_ASSERT(numBlocksVisited == nBlocks);

    cand.score =
        10.0 * float(nBlocks) + 2.0 * float(nInsns) + 1.0 * float(nPhis);
  }

  // Sort the loops by `score`.
  std::sort(initialCandidates.begin(), initialCandidates.end(),
            [](const InitialCandidate& cand1, const InitialCandidate& cand2) {
              return cand1.score < cand2.score;
            });

  return true;
}

// =====================================================================
//
// UnrollLoops, the top level of the unroller.

bool UnrollLoops(const MIRGenerator* mir, MIRGraph& graph, bool* changed) {
  *changed = false;

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_Unroll)) {
    JitSpew(JitSpew_Unroll, "BEGIN UnrollLoops");
  }
#endif

  // Make up a vector of initial candidates, which are innermost loops only,
  // and not too big.

  InitialCandidateVector initialCandidates;
  if (!FindInitialCandidates(graph, initialCandidates)) {
    return false;
  }

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_Unroll)) {
    for (const InitialCandidate& cand : initialCandidates) {
      MOZ_ASSERT(cand.valid);
      JitSpew(JitSpew_Unroll, "   initial cand:  score=%-5.1f  blocks=%u-%u",
              cand.score, cand.header->id(),
              cand.header->id() + cand.numBlocks - 1);
    }
  }
#endif

  // Perform a more expensive analysis of the candidates, leading to a further
  // reduction in the set.  For those remaining, an UnrollState is created and
  // initialized.  Also a decision is made whether to unroll only, peel only,
  // or both.

  mozilla::Vector<UnrollState, 32, SystemAllocPolicy> unrollStates;

  for (const InitialCandidate& cand : initialCandidates) {
    if (cand.score > InitialCandidateThreshold) {
      // Give up.  We just sorted them by `score`, so the rest will be even
      // more undesirable for unrolling.
      break;
    }

    // Park the loop's blocks in a vector, so we can hand it off to
    // AnalyzeLoops for examination.  This is the place where the original
    // block sequence, as seen by the entire rest of the machinery, is fixed.
    BlockVector originalBlocks;

    const MBasicBlock* backedge = cand.header->backedge();

    for (auto blockIter(graph.rpoBegin(cand.header)); /**/; ++blockIter) {
      MOZ_ASSERT(blockIter != graph.rpoEnd());
      MBasicBlock* block = *blockIter;
      if (!originalBlocks.append(block)) {
        return false;
      }
      if (block == backedge) {
        break;
      }
    }

    // Analyze the candidate in detail.  If unrolling and/or peeling is
    // approved, this also collects up for us, the loop's target block set and
    // exiting value set.
    BlockSet exitTargetBlocks;
    ValueSet exitingValues;

    AnalysisResult res =
        AnalyzeLoop(originalBlocks, &exitTargetBlocks, &exitingValues);

#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Unroll)) {
      MOZ_ASSERT(cand.valid);
      JitSpew(JitSpew_Unroll, "  %13s:  score=%-5.1f  blocks=%u-%u",
              Name_of_AnalysisResult(res), cand.score, cand.header->id(),
              cand.header->id() + cand.numBlocks - 1);
    }
#endif

    switch (res) {
      case AnalysisResult::OOM:
        return false;
      case AnalysisResult::Peel:
      case AnalysisResult::Unroll:
      case AnalysisResult::PeelAndUnroll:
        break;
      case AnalysisResult::BadInvariants:
        MOZ_CRASH("js::jit::UnrollLoops: unexpected incoming MIR");
      case AnalysisResult::TooComplex:
      case AnalysisResult::Uncloneable:
      case AnalysisResult::TooLarge:
      case AnalysisResult::Unsuitable:
        // Ignore this loop
        continue;
      default:
        MOZ_CRASH();
    }

    // The candidate has been accepted for unrolling and/or peeling.  Make an
    // initial UnrollState for it and add it to our collection thereof.

    UnrollMode mode;
    if (res == AnalysisResult::Peel) {
      mode = UnrollMode::Peel;
    } else if (res == AnalysisResult::Unroll) {
      mode = UnrollMode::Unroll;
    } else if (res == AnalysisResult::PeelAndUnroll) {
      mode = UnrollMode::PeelAndUnroll;
    } else {
      MOZ_CRASH();
    }

    if (!unrollStates.emplaceBack(mode)) {
      return false;
    }
    UnrollState* state = &unrollStates.back();
    if (!state->exitTargetBlocks.copy(exitTargetBlocks) ||
        !state->exitingValues.copy(exitingValues)) {
      return false;
    }

    // Ignoring peeling, how many times should we unroll a loop?
    // For example, if the original loop was `loop { B; }` then with
    // `basicUnrollingFactor = 3`, the unrolled loop will be `loop { B; B; B;
    // }`.  If peeling is also requested then we will unroll one more time than
    // this, giving overall result `B; loop { B; B; B; }`.
    uint32_t basicUnrollingFactor = JS::Prefs::wasm_unroll_factor();
    if (basicUnrollingFactor < 2) {
      // It needs to be at least 2, else we're not unrolling at all.
      basicUnrollingFactor = 2;
    } else if (basicUnrollingFactor > 8) {
      // Unrolling gives rapidly diminishing marginal gains.  Even beyond 4
      // there's little benefit to be had.
      basicUnrollingFactor = 8;
    }

    // Decide on the total number of duplicates of the loop body we'll need
    // (including the original).
    uint32_t unrollingFactor;
    if (res == AnalysisResult::Peel) {
      // 1 for the peeled iteration,
      // 1 for the loop (since we are not unrolling it)
      unrollingFactor = 1 + 1;
    } else if (res == AnalysisResult::Unroll) {
      // No peeled iteration, we unroll only
      unrollingFactor = 0 + basicUnrollingFactor;
    } else if (res == AnalysisResult::PeelAndUnroll) {
      // 1 peeled iteration, and we unroll the loop
      unrollingFactor = 1 + basicUnrollingFactor;
    } else {
      MOZ_CRASH();
    }

    // Set up UnrollState::blockTable and copy in the initial loop blocks.
    if (!state->blockTable.init(unrollingFactor, originalBlocks.length())) {
      return false;
    }
    for (uint32_t bix = 0; bix < originalBlocks.length(); bix++) {
      state->blockTable.set(0, bix, originalBlocks[bix]);
    }
  }

  // Now, finally, we have an initialized vector of UnrollStates ready to go.
  // After this point, `originalBlocks` is no longer used.  Also, because of
  // the checking done in AnalyzeLoop, we know the transformations can't fail
  // (apart from OOMing).

  // Add "loop-closing phis" for values defined in the loop and used
  // afterwards.  See comments on AddClosingPhisForLoop.
  for (const UnrollState& state : unrollStates) {
    if (!AddClosingPhisForLoop(graph.alloc(), state)) {
      return false;
    }
  }

  // Actually do the unrolling and/or peeling.
  uint32_t numLoopsPeeled = 0;
  uint32_t numLoopsUnrolled = 0;
  uint32_t numLoopsPeeledAndUnrolled = 0;
  for (UnrollState& state : unrollStates) {
    if (!UnrollAndOrPeelLoop(graph, state)) {
      return false;
    }
    // Update stats.
    switch (state.mode) {
      case UnrollMode::Peel:
        numLoopsPeeled++;
        break;
      case UnrollMode::Unroll:
        numLoopsUnrolled++;
        break;
      case UnrollMode::PeelAndUnroll:
        numLoopsPeeledAndUnrolled++;
        break;
      default:
        MOZ_CRASH();
    }
  }

  // Do function-level fixups, if anything changed.
  if (!unrollStates.empty()) {
    RenumberBlocks(graph);
    ClearDominatorTree(graph);
    if (!BuildDominatorTree(mir, graph)) {
      return false;
    }
  }

  uint32_t numLoopsChanged =
      numLoopsPeeled + numLoopsUnrolled + numLoopsPeeledAndUnrolled;

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_Unroll)) {
    if (numLoopsChanged == 0) {
      JitSpew(JitSpew_Unroll, "END   UnrollLoops");
    } else {
      JitSpew(JitSpew_Unroll,
              "END UnrollLoops, %u processed (P=%u, U=%u, P&U=%u)",
              numLoopsChanged, numLoopsPeeled, numLoopsUnrolled,
              numLoopsPeeledAndUnrolled);
    }
  }
#endif

  *changed = numLoopsChanged > 0;
  return true;
}

}  // namespace jit
}  // namespace js
