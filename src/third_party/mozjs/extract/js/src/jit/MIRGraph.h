/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MIRGraph_h
#define jit_MIRGraph_h

// This file declares the data structures used to build a control-flow graph
// containing MIR.

#include "jit/CompileInfo.h"
#include "jit/FixedList.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitAllocPolicy.h"
#include "jit/MIR.h"

namespace js {
namespace jit {

class MBasicBlock;
class MIRGraph;
class MStart;

class MDefinitionIterator;

using MInstructionIterator = InlineListIterator<MInstruction>;
using MInstructionReverseIterator = InlineListReverseIterator<MInstruction>;
using MPhiIterator = InlineListIterator<MPhi>;

#ifdef DEBUG
typedef InlineForwardListIterator<MResumePoint> MResumePointIterator;
#endif

class LBlock;

class MBasicBlock : public TempObject, public InlineListNode<MBasicBlock> {
 public:
  enum Kind {
    NORMAL,
    PENDING_LOOP_HEADER,
    LOOP_HEADER,
    SPLIT_EDGE,
    FAKE_LOOP_PRED,
    INTERNAL,
    DEAD
  };

 private:
  MBasicBlock(MIRGraph& graph, const CompileInfo& info, BytecodeSite* site,
              Kind kind);
  [[nodiscard]] bool init();
  void copySlots(MBasicBlock* from);
  [[nodiscard]] bool inherit(TempAllocator& alloc, size_t stackDepth,
                             MBasicBlock* maybePred, uint32_t popped);

  // This block cannot be reached by any means.
  bool unreachable_ = false;

  // This block will unconditionally bail out.
  bool alwaysBails_ = false;

  // Will be used for branch hinting in wasm.
  wasm::BranchHint branchHint_ = wasm::BranchHint::Invalid;

  // Pushes a copy of a local variable or argument.
  void pushVariable(uint32_t slot) { push(slots_[slot]); }

  // Sets a variable slot to the top of the stack, correctly creating copies
  // as needed.
  void setVariable(uint32_t slot) {
    MOZ_ASSERT(stackPosition_ > info_.firstStackSlot());
    setSlot(slot, slots_[stackPosition_ - 1]);
  }

  enum ReferencesType {
    RefType_None = 0,

    // Assert that the instruction is unused.
    RefType_AssertNoUses = 1 << 0,

    // Discard the operands of the resume point / instructions if the
    // following flag are given too.
    RefType_DiscardOperands = 1 << 1,
    RefType_DiscardResumePoint = 1 << 2,
    RefType_DiscardInstruction = 1 << 3,

    // Discard operands of the instruction and its resume point.
    RefType_DefaultNoAssert = RefType_DiscardOperands |
                              RefType_DiscardResumePoint |
                              RefType_DiscardInstruction,

    // Discard everything and assert that the instruction is not used.
    RefType_Default = RefType_AssertNoUses | RefType_DefaultNoAssert,

    // Discard resume point operands only, without discarding the operands
    // of the current instruction.  Asserts that the instruction is unused.
    RefType_IgnoreOperands = RefType_AssertNoUses | RefType_DiscardOperands |
                             RefType_DiscardResumePoint
  };

  void discardResumePoint(MResumePoint* rp,
                          ReferencesType refType = RefType_Default);
  void removeResumePoint(MResumePoint* rp);

  // Remove all references to an instruction such that it can be removed from
  // the list of instruction, without keeping any dangling pointer to it. This
  // includes the operands of the instruction, and the resume point if
  // present.
  void prepareForDiscard(MInstruction* ins,
                         ReferencesType refType = RefType_Default);

 public:
  ///////////////////////////////////////////////////////
  ////////// BEGIN GRAPH BUILDING INSTRUCTIONS //////////
  ///////////////////////////////////////////////////////

  // Creates a new basic block for a MIR generator. If |pred| is not nullptr,
  // its slots and stack depth are initialized from |pred|.
  static MBasicBlock* New(MIRGraph& graph, size_t stackDepth,
                          const CompileInfo& info, MBasicBlock* maybePred,
                          BytecodeSite* site, Kind kind);
  static MBasicBlock* New(MIRGraph& graph, const CompileInfo& info,
                          MBasicBlock* pred, Kind kind);
  static MBasicBlock* NewPopN(MIRGraph& graph, const CompileInfo& info,
                              MBasicBlock* pred, BytecodeSite* site, Kind kind,
                              uint32_t popn);
  static MBasicBlock* NewPendingLoopHeader(MIRGraph& graph,
                                           const CompileInfo& info,
                                           MBasicBlock* pred,
                                           BytecodeSite* site);
  static MBasicBlock* NewSplitEdge(MIRGraph& graph, MBasicBlock* pred,
                                   size_t predEdgeIdx, MBasicBlock* succ);
  static MBasicBlock* NewFakeLoopPredecessor(MIRGraph& graph,
                                             MBasicBlock* header);

  // Create a new basic block for internal control flow not present in the
  // original CFG.
  static MBasicBlock* NewInternal(MIRGraph& graph, MBasicBlock* orig,
                                  MResumePoint* activeResumePoint);

  bool dominates(const MBasicBlock* other) const {
    return other->domIndex() - domIndex() < numDominated();
  }

  void setId(uint32_t id) { id_ = id; }

  // Mark this block (and only this block) as unreachable.
  void setUnreachable() {
    MOZ_ASSERT(!unreachable_);
    setUnreachableUnchecked();
  }
  void setUnreachableUnchecked() { unreachable_ = true; }
  bool unreachable() const { return unreachable_; }

  void setAlwaysBails() { alwaysBails_ = true; }
  bool alwaysBails() const { return alwaysBails_; }

  // Move the definition to the top of the stack.
  void pick(int32_t depth);

  // Move the top of the stack definition under the depth-th stack value.
  void unpick(int32_t depth);

  // Exchange 2 stack slots at the defined depth
  void swapAt(int32_t depth);

  // Note: most of the methods below are hot. Do not un-inline them without
  // measuring the impact.

  // Gets the instruction associated with various slot types.
  MDefinition* peek(int32_t depth) {
    MOZ_ASSERT(depth < 0);
    MOZ_ASSERT(stackPosition_ + depth >= info_.firstStackSlot());
    return peekUnchecked(depth);
  }

  MDefinition* peekUnchecked(int32_t depth) {
    MOZ_ASSERT(depth < 0);
    return getSlot(stackPosition_ + depth);
  }

  MDefinition* environmentChain();
  MDefinition* argumentsObject();

  // Increase the number of slots available
  [[nodiscard]] bool increaseSlots(size_t num);
  [[nodiscard]] bool ensureHasSlots(size_t num);

  // Initializes a slot value; must not be called for normal stack
  // operations, as it will not create new SSA names for copies.
  void initSlot(uint32_t slot, MDefinition* ins) {
    slots_[slot] = ins;
    if (entryResumePoint()) {
      entryResumePoint()->initOperand(slot, ins);
    }
  }

  // Sets the instruction associated with various slot types. The
  // instruction must lie at the top of the stack.
  void setLocal(uint32_t local) { setVariable(info_.localSlot(local)); }
  void setArg(uint32_t arg) { setVariable(info_.argSlot(arg)); }
  void setSlot(uint32_t slot, MDefinition* ins) { slots_[slot] = ins; }

  // Tracks an instruction as being pushed onto the operand stack.
  void push(MDefinition* ins) {
    MOZ_ASSERT(stackPosition_ < nslots());
    slots_[stackPosition_++] = ins;
  }
  void pushArg(uint32_t arg) { pushVariable(info_.argSlot(arg)); }
  void pushArgUnchecked(uint32_t arg) {
    pushVariable(info_.argSlotUnchecked(arg));
  }
  void pushLocal(uint32_t local) { pushVariable(info_.localSlot(local)); }
  void pushSlot(uint32_t slot) { pushVariable(slot); }
  void setEnvironmentChain(MDefinition* ins);
  void setArgumentsObject(MDefinition* ins);

  // Returns the top of the stack, then decrements the virtual stack pointer.
  MDefinition* pop() {
    MOZ_ASSERT(stackPosition_ > info_.firstStackSlot());
    return slots_[--stackPosition_];
  }
  void popn(uint32_t n) {
    MOZ_ASSERT(stackPosition_ - n >= info_.firstStackSlot());
    MOZ_ASSERT(stackPosition_ >= stackPosition_ - n);
    stackPosition_ -= n;
  }

  // Adds an instruction to this block's instruction list.
  inline void add(MInstruction* ins);

  // Marks the last instruction of the block; no further instructions
  // can be added.
  void end(MControlInstruction* ins) {
    MOZ_ASSERT(!hasLastIns());  // Existing control instructions should be
                                // removed first.
    MOZ_ASSERT(ins);
    add(ins);
  }

  // Adds a phi instruction, but does not set successorWithPhis.
  void addPhi(MPhi* phi);

  // Adds a resume point to this block.
  void addResumePoint(MResumePoint* resume) {
#ifdef DEBUG
    resumePoints_.pushFront(resume);
#endif
  }

  // Discard pre-allocated resume point.
  void discardPreAllocatedResumePoint(MResumePoint* resume) {
    MOZ_ASSERT(!resume->instruction());
    discardResumePoint(resume);
  }

  // Adds a predecessor. Every predecessor must have the same exit stack
  // depth as the entry state to this block. Adding a predecessor
  // automatically creates phi nodes and rewrites uses as needed.
  [[nodiscard]] bool addPredecessor(TempAllocator& alloc, MBasicBlock* pred);
  [[nodiscard]] bool addPredecessorPopN(TempAllocator& alloc, MBasicBlock* pred,
                                        uint32_t popped);

  // Add a predecessor which won't introduce any new phis to this block.
  // This may be called after the contents of this block have been built.
  [[nodiscard]] bool addPredecessorSameInputsAs(MBasicBlock* pred,
                                                MBasicBlock* existingPred);

  // Stranger utilities used for inlining.
  [[nodiscard]] bool addPredecessorWithoutPhis(MBasicBlock* pred);
  void inheritSlots(MBasicBlock* parent);
  [[nodiscard]] bool initEntrySlots(TempAllocator& alloc);

  // Replaces an edge for a given block with a new block. This is
  // used for critical edge splitting.
  //
  // Note: If successorWithPhis is set, you must not be replacing it.
  void replacePredecessor(MBasicBlock* old, MBasicBlock* split);
  void replaceSuccessor(size_t pos, MBasicBlock* split);

  // Removes `pred` from the predecessor list. If this block defines phis,
  // removes the entry for `pred` and updates the indices of later entries.
  // This may introduce redundant phis if the new block has fewer
  // than two predecessors.
  void removePredecessor(MBasicBlock* pred);

  // A version of removePredecessor which expects that phi operands to
  // |pred| have already been removed.
  void removePredecessorWithoutPhiOperands(MBasicBlock* pred, size_t predIndex);

  // Resets all the dominator info so that it can be recomputed.
  void clearDominatorInfo();

  // Sets a back edge. This places phi nodes and rewrites instructions within
  // the current loop as necessary.
  [[nodiscard]] bool setBackedge(MBasicBlock* block);
  [[nodiscard]] bool setBackedgeWasm(MBasicBlock* block, size_t paramCount);

  // Resets a LOOP_HEADER block to a NORMAL block.  This is needed when
  // optimizations remove the backedge.
  void clearLoopHeader();

  // Sets a block to a LOOP_HEADER block, with newBackedge as its backedge.
  // This is needed when optimizations remove the normal entry to a loop
  // with multiple entries.
  void setLoopHeader(MBasicBlock* newBackedge);

  // Propagates backedge slots into phis operands of the loop header.
  [[nodiscard]] bool inheritPhisFromBackedge(MBasicBlock* backedge);

  void insertBefore(MInstruction* at, MInstruction* ins);
  void insertAfter(MInstruction* at, MInstruction* ins);

  void insertAtEnd(MInstruction* ins);

  // Move an instruction. Movement may cross block boundaries.
  void moveBefore(MInstruction* at, MInstruction* ins);

  enum IgnoreTop { IgnoreNone = 0, IgnoreRecover = 1 << 0 };

  // Locate the top of the |block|, where it is safe to insert a new
  // instruction.
  MInstruction* safeInsertTop(MDefinition* ins = nullptr,
                              IgnoreTop ignore = IgnoreNone);

  // Removes an instruction with the intention to discard it.
  void discard(MInstruction* ins);
  void discardLastIns();
  void discardAllInstructions();
  void discardAllInstructionsStartingAt(MInstructionIterator iter);
  void discardAllPhis();
  void discardAllResumePoints(bool discardEntry = true);
  void clear();

  // Splits this block in two at a given instruction, inserting a new control
  // flow diamond with |ins| in the slow path, |fastpath| in the other, and
  // |condition| determining which path to take.
  bool wrapInstructionInFastpath(MInstruction* ins, MInstruction* fastpath,
                                 MInstruction* condition);

  void moveOuterResumePointTo(MBasicBlock* dest);

  // Move an instruction from this block to a block that has not yet been
  // terminated.
  void moveToNewBlock(MInstruction* ins, MBasicBlock* dst);

  // Same as |void discard(MInstruction* ins)| but assuming that
  // all operands are already discarded.
  void discardIgnoreOperands(MInstruction* ins);

  // Discards a phi instruction and updates predecessor successorWithPhis.
  void discardPhi(MPhi* phi);

  // Some instruction which are guarding against some MIRType value, or
  // against a type expectation should be considered as removing a potenatial
  // branch where the guard does not hold.  We need to register such
  // instructions in order to do destructive optimizations correctly, such as
  // Range Analysis.
  void flagOperandsOfPrunedBranches(MInstruction* ins);

  // Mark this block as having been removed from the graph.
  void markAsDead() {
    MOZ_ASSERT(kind_ != DEAD);
    kind_ = DEAD;
  }

  ///////////////////////////////////////////////////////
  /////////// END GRAPH BUILDING INSTRUCTIONS ///////////
  ///////////////////////////////////////////////////////

  MIRGraph& graph() { return graph_; }
  const CompileInfo& info() const { return info_; }
  jsbytecode* pc() const { return trackedSite_->pc(); }
  jsbytecode* entryPC() const { return entryResumePoint()->pc(); }
  uint32_t nslots() const { return slots_.length(); }
  uint32_t id() const { return id_; }
  uint32_t numPredecessors() const { return predecessors_.length(); }

  bool branchHintingUnlikely() const {
    return branchHint_ == wasm::BranchHint::Unlikely;
  }
  bool branchHintingLikely() const {
    return branchHint_ == wasm::BranchHint::Likely;
  }

  void setBranchHinting(wasm::BranchHint value) { branchHint_ = value; }

  uint32_t domIndex() const {
    MOZ_ASSERT(!isDead());
    return domIndex_;
  }
  void setDomIndex(uint32_t d) { domIndex_ = d; }

  MBasicBlock* getPredecessor(uint32_t i) const { return predecessors_[i]; }
  size_t indexForPredecessor(MBasicBlock* block) const {
    // This should only be called before critical edge splitting.
    MOZ_ASSERT(!block->successorWithPhis());

    for (size_t i = 0; i < predecessors_.length(); i++) {
      if (predecessors_[i] == block) {
        return i;
      }
    }
    MOZ_CRASH();
  }
  bool hasAnyIns() const { return !instructions_.empty(); }
  bool hasLastIns() const {
    return hasAnyIns() && instructions_.rbegin()->isControlInstruction();
  }
  MControlInstruction* lastIns() const {
    MOZ_ASSERT(hasLastIns());
    return instructions_.rbegin()->toControlInstruction();
  }
  // Find or allocate an optimized out constant.
  MConstant* optimizedOutConstant(TempAllocator& alloc);
  MPhiIterator phisBegin() const { return phis_.begin(); }
  MPhiIterator phisBegin(MPhi* at) const { return phis_.begin(at); }
  MPhiIterator phisEnd() const { return phis_.end(); }
  bool phisEmpty() const { return phis_.empty(); }
#ifdef DEBUG
  MResumePointIterator resumePointsBegin() const {
    return resumePoints_.begin();
  }
  MResumePointIterator resumePointsEnd() const { return resumePoints_.end(); }
  bool resumePointsEmpty() const { return resumePoints_.empty(); }
#endif
  MInstructionIterator begin() { return instructions_.begin(); }
  MInstructionIterator begin(MInstruction* at) {
    MOZ_ASSERT(at->block() == this);
    return instructions_.begin(at);
  }
  MInstructionIterator end() { return instructions_.end(); }
  MInstructionReverseIterator rbegin() { return instructions_.rbegin(); }
  MInstructionReverseIterator rbegin(MInstruction* at) {
    MOZ_ASSERT(at->block() == this);
    return instructions_.rbegin(at);
  }
  MInstructionReverseIterator rend() { return instructions_.rend(); }

  bool isLoopHeader() const { return kind_ == LOOP_HEADER; }
  bool isPendingLoopHeader() const { return kind_ == PENDING_LOOP_HEADER; }

  bool hasUniqueBackedge() const {
    MOZ_ASSERT(isLoopHeader());
    MOZ_ASSERT(numPredecessors() >= 1);
    if (numPredecessors() == 1 || numPredecessors() == 2) {
      return true;
    }
    if (numPredecessors() == 3) {
      // fixup block added by NewFakeLoopPredecessor
      return getPredecessor(1)->numPredecessors() == 0;
    }
    return false;
  }
  MBasicBlock* backedge() const {
    MOZ_ASSERT(hasUniqueBackedge());
    return getPredecessor(numPredecessors() - 1);
  }
  MBasicBlock* loopHeaderOfBackedge() const {
    MOZ_ASSERT(isLoopBackedge());
    return getSuccessor(numSuccessors() - 1);
  }
  MBasicBlock* loopPredecessor() const {
    MOZ_ASSERT(isLoopHeader());
    return getPredecessor(0);
  }
  bool isLoopBackedge() const {
    if (!numSuccessors()) {
      return false;
    }
    MBasicBlock* lastSuccessor = getSuccessor(numSuccessors() - 1);
    return lastSuccessor->isLoopHeader() &&
           lastSuccessor->hasUniqueBackedge() &&
           lastSuccessor->backedge() == this;
  }
  bool isSplitEdge() const { return kind_ == SPLIT_EDGE; }
  bool isDead() const { return kind_ == DEAD; }
  bool isFakeLoopPred() const { return kind_ == FAKE_LOOP_PRED; }

  uint32_t stackDepth() const { return stackPosition_; }
  bool isMarked() const { return mark_; }
  void mark() {
    MOZ_ASSERT(!mark_, "Marking already-marked block");
    markUnchecked();
  }
  void markUnchecked() { mark_ = true; }
  void unmark() {
    MOZ_ASSERT(mark_, "Unarking unmarked block");
    unmarkUnchecked();
  }
  void unmarkUnchecked() { mark_ = false; }

  MBasicBlock* immediateDominator() const { return immediateDominator_; }

  void setImmediateDominator(MBasicBlock* dom) { immediateDominator_ = dom; }

  MTest* immediateDominatorBranch(BranchDirection* pdirection);

  size_t numImmediatelyDominatedBlocks() const {
    return immediatelyDominated_.length();
  }

  MBasicBlock* getImmediatelyDominatedBlock(size_t i) const {
    return immediatelyDominated_[i];
  }

  MBasicBlock** immediatelyDominatedBlocksBegin() {
    return immediatelyDominated_.begin();
  }

  MBasicBlock** immediatelyDominatedBlocksEnd() {
    return immediatelyDominated_.end();
  }

  // Return the number of blocks dominated by this block. All blocks
  // dominate at least themselves, so this will always be non-zero.
  size_t numDominated() const {
    MOZ_ASSERT(numDominated_ != 0);
    return numDominated_;
  }

  void addNumDominated(size_t n) { numDominated_ += n; }

  // Add |child| to this block's immediately-dominated set.
  bool addImmediatelyDominatedBlock(MBasicBlock* child);

  // Remove |child| from this block's immediately-dominated set.
  void removeImmediatelyDominatedBlock(MBasicBlock* child);

  // This function retrieves the internal instruction associated with a
  // slot, and should not be used for normal stack operations. It is an
  // internal helper that is also used to enhance spew.
  MDefinition* getSlot(uint32_t index) {
    MOZ_ASSERT(index < stackPosition_);
    return slots_[index];
  }

  MResumePoint* entryResumePoint() const { return entryResumePoint_; }
  void setEntryResumePoint(MResumePoint* rp) { entryResumePoint_ = rp; }
  void clearEntryResumePoint() {
    discardResumePoint(entryResumePoint_);
    entryResumePoint_ = nullptr;
  }
  MResumePoint* outerResumePoint() const { return outerResumePoint_; }
  void setOuterResumePoint(MResumePoint* outer) {
    MOZ_ASSERT(!outerResumePoint_);
    outerResumePoint_ = outer;
  }
  void clearOuterResumePoint() {
    discardResumePoint(outerResumePoint_);
    outerResumePoint_ = nullptr;
  }
  MResumePoint* callerResumePoint() const { return callerResumePoint_; }
  void setCallerResumePoint(MResumePoint* caller) {
    callerResumePoint_ = caller;
  }

  LBlock* lir() const { return lir_; }
  void assignLir(LBlock* lir) {
    MOZ_ASSERT(!lir_);
    lir_ = lir;
  }

  MBasicBlock* successorWithPhis() const { return successorWithPhis_; }
  uint32_t positionInPhiSuccessor() const {
    MOZ_ASSERT(successorWithPhis());
    return positionInPhiSuccessor_;
  }
  void setSuccessorWithPhis(MBasicBlock* successor, uint32_t id) {
    successorWithPhis_ = successor;
    positionInPhiSuccessor_ = id;
  }
  void clearSuccessorWithPhis() { successorWithPhis_ = nullptr; }
  size_t numSuccessors() const {
    MOZ_ASSERT(lastIns());
    return lastIns()->numSuccessors();
  }
  MBasicBlock* getSuccessor(size_t index) const {
    MOZ_ASSERT(lastIns());
    return lastIns()->getSuccessor(index);
  }
  MBasicBlock* getSingleSuccessor() const {
    MOZ_ASSERT(numSuccessors() == 1);
    return getSuccessor(0);
  }
  size_t getSuccessorIndex(MBasicBlock*) const;
  size_t getPredecessorIndex(MBasicBlock*) const;

  void setLoopDepth(uint32_t loopDepth) { loopDepth_ = loopDepth; }
  uint32_t loopDepth() const { return loopDepth_; }

  void dumpStack(GenericPrinter& out);
  void dumpStack();

  void dump(GenericPrinter& out);
  void dump();

  void updateTrackedSite(BytecodeSite* site) {
    MOZ_ASSERT(site->tree() == trackedSite_->tree());
    trackedSite_ = site;
  }
  BytecodeSite* trackedSite() const { return trackedSite_; }
  InlineScriptTree* trackedTree() const { return trackedSite_->tree(); }

  // Find the previous resume point that would be used if this instruction
  // bails out.
  MResumePoint* activeResumePoint(MInstruction* ins);

 private:
  MIRGraph& graph_;
  const CompileInfo& info_;  // Each block originates from a particular script.
  InlineList<MInstruction> instructions_;
  Vector<MBasicBlock*, 1, JitAllocPolicy> predecessors_;
  InlineList<MPhi> phis_;
  FixedList<MDefinition*> slots_;
  uint32_t stackPosition_;
  uint32_t id_;
  uint32_t domIndex_;  // Index in the dominator tree.
  uint32_t numDominated_;
  LBlock* lir_;

  // Copy of a dominator block's outerResumePoint_ which holds the state of
  // caller frame at the time of the call. If not null, this implies that this
  // basic block corresponds to an inlined script.
  MResumePoint* callerResumePoint_;

  // Resume point holding baseline-like frame for the PC corresponding to the
  // entry of this basic block.
  MResumePoint* entryResumePoint_;

  // Resume point holding baseline-like frame for the PC corresponding to the
  // beginning of the call-site which is being inlined after this block.
  MResumePoint* outerResumePoint_;

#ifdef DEBUG
  // Unordered list used to verify that all the resume points which are
  // registered are correctly removed when a basic block is removed.
  InlineForwardList<MResumePoint> resumePoints_;
#endif

  MBasicBlock* successorWithPhis_;
  uint32_t positionInPhiSuccessor_;
  uint32_t loopDepth_;
  Kind kind_ : 8;

  // Utility mark for traversal algorithms.
  bool mark_;

  Vector<MBasicBlock*, 1, JitAllocPolicy> immediatelyDominated_;
  MBasicBlock* immediateDominator_;

  // Track bailouts by storing the current pc in MIR instruction added at
  // this cycle. This is also used for tracking calls and optimizations when
  // profiling.
  BytecodeSite* trackedSite_;
};

using MBasicBlockIterator = InlineListIterator<MBasicBlock>;
using ReversePostorderIterator = InlineListIterator<MBasicBlock>;
using PostorderIterator = InlineListReverseIterator<MBasicBlock>;

typedef Vector<MBasicBlock*, 1, JitAllocPolicy> MIRGraphReturns;

class MIRGraph {
  InlineList<MBasicBlock> blocks_;
  TempAllocator* alloc_;
  MIRGraphReturns* returnAccumulator_;
  uint32_t blockIdGen_;
  uint32_t idGen_;
  MBasicBlock* osrBlock_;

  size_t numBlocks_;
  bool hasTryBlock_;

  InlineList<MPhi> phiFreeList_;
  size_t phiFreeListLength_;

 public:
  explicit MIRGraph(TempAllocator* alloc)
      : alloc_(alloc),
        returnAccumulator_(nullptr),
        blockIdGen_(0),
        idGen_(0),
        osrBlock_(nullptr),
        numBlocks_(0),
        hasTryBlock_(false),
        phiFreeListLength_(0) {}

  TempAllocator& alloc() const { return *alloc_; }

  void addBlock(MBasicBlock* block);
  void insertBlockAfter(MBasicBlock* at, MBasicBlock* block);
  void insertBlockBefore(MBasicBlock* at, MBasicBlock* block);

  void unmarkBlocks();

  void setReturnAccumulator(MIRGraphReturns* accum) {
    returnAccumulator_ = accum;
  }
  MIRGraphReturns* returnAccumulator() const { return returnAccumulator_; }

  [[nodiscard]] bool addReturn(MBasicBlock* returnBlock) {
    if (!returnAccumulator_) {
      return true;
    }

    return returnAccumulator_->append(returnBlock);
  }

  MBasicBlock* entryBlock() { return *blocks_.begin(); }
  MBasicBlockIterator begin() { return blocks_.begin(); }
  MBasicBlockIterator begin(MBasicBlock* at) { return blocks_.begin(at); }
  MBasicBlockIterator end() { return blocks_.end(); }
  PostorderIterator poBegin() { return blocks_.rbegin(); }
  PostorderIterator poBegin(MBasicBlock* at) { return blocks_.rbegin(at); }
  PostorderIterator poEnd() { return blocks_.rend(); }
  ReversePostorderIterator rpoBegin() { return blocks_.begin(); }
  ReversePostorderIterator rpoBegin(MBasicBlock* at) {
    return blocks_.begin(at);
  }
  ReversePostorderIterator rpoEnd() { return blocks_.end(); }
  void removeBlock(MBasicBlock* block);
  void moveBlockToEnd(MBasicBlock* block) {
    blocks_.remove(block);
    MOZ_ASSERT_IF(!blocks_.empty(), block->id());
    blocks_.pushBack(block);
  }
  void moveBlockBefore(MBasicBlock* at, MBasicBlock* block) {
    MOZ_ASSERT(block->id());
    blocks_.remove(block);
    blocks_.insertBefore(at, block);
  }
  void moveBlockAfter(MBasicBlock* at, MBasicBlock* block) {
    MOZ_ASSERT(block->id());
    blocks_.remove(block);
    blocks_.insertAfter(at, block);
  }
  size_t numBlocks() const { return numBlocks_; }
  uint32_t numBlockIds() const { return blockIdGen_; }
  void allocDefinitionId(MDefinition* ins) { ins->setId(idGen_++); }
  uint32_t getNumInstructionIds() { return idGen_; }
  MResumePoint* entryResumePoint() { return entryBlock()->entryResumePoint(); }

  void setOsrBlock(MBasicBlock* osrBlock) {
    MOZ_ASSERT(!osrBlock_);
    osrBlock_ = osrBlock;
  }
  MBasicBlock* osrBlock() const { return osrBlock_; }

  MBasicBlock* osrPreHeaderBlock() const {
    return osrBlock() ? osrBlock()->getSingleSuccessor() : nullptr;
  }

  bool hasTryBlock() const { return hasTryBlock_; }
  void setHasTryBlock() { hasTryBlock_ = true; }

  void dump(GenericPrinter& out);
  void dump();

  void addPhiToFreeList(MPhi* phi) {
    phiFreeList_.pushBack(phi);
    phiFreeListLength_++;
  }
  size_t phiFreeListLength() const { return phiFreeListLength_; }
  MPhi* takePhiFromFreeList() {
    MOZ_ASSERT(phiFreeListLength_ > 0);
    phiFreeListLength_--;
    return phiFreeList_.popBack();
  }

  void removeFakeLoopPredecessors();

#ifdef DEBUG
  // Dominators can't be built after we remove fake loop predecessors.
 private:
  bool canBuildDominators_ = true;

 public:
  bool canBuildDominators() const { return canBuildDominators_; }
#endif
};

class MDefinitionIterator {
  friend class MBasicBlock;
  friend class MNodeIterator;

 private:
  MBasicBlock* block_;
  MPhiIterator phiIter_;
  MInstructionIterator iter_;

  bool atPhi() const { return phiIter_ != block_->phisEnd(); }

  MDefinition* getIns() {
    if (atPhi()) {
      return *phiIter_;
    }
    return *iter_;
  }

  bool more() const { return atPhi() || (*iter_) != block_->lastIns(); }

 public:
  explicit MDefinitionIterator(MBasicBlock* block)
      : block_(block), phiIter_(block->phisBegin()), iter_(block->begin()) {}

  MDefinitionIterator operator++() {
    MOZ_ASSERT(more());
    if (atPhi()) {
      ++phiIter_;
    } else {
      ++iter_;
    }
    return *this;
  }

  MDefinitionIterator operator++(int) {
    MDefinitionIterator old(*this);
    operator++();
    return old;
  }

  explicit operator bool() const { return more(); }

  MDefinition* operator*() { return getIns(); }

  MDefinition* operator->() { return getIns(); }
};

// Iterates on all resume points, phis, and instructions of a MBasicBlock.
// Resume points are visited as long as they have not been discarded.
class MNodeIterator {
 private:
  // If this is non-null, the resume point that we will visit next (unless
  // it has been discarded). Initialized to the entry resume point.
  // Otherwise, resume point of the most recently visited instruction.
  MResumePoint* resumePoint_;

  mozilla::DebugOnly<MInstruction*> lastInstruction_ = nullptr;

  // Definition iterator which is one step ahead when visiting resume points.
  // This is in order to avoid incrementing the iterator while it is settled
  // on a discarded instruction.
  MDefinitionIterator defIter_;

  MBasicBlock* block() const { return defIter_.block_; }

  bool atResumePoint() const {
    MOZ_ASSERT_IF(lastInstruction_ && !lastInstruction_->isDiscarded(),
                  lastInstruction_->resumePoint() == resumePoint_);
    return resumePoint_ && !resumePoint_->isDiscarded();
  }

  MNode* getNode() {
    if (atResumePoint()) {
      return resumePoint_;
    }
    return *defIter_;
  }

  void next() {
    if (!atResumePoint()) {
      if (defIter_->isInstruction()) {
        resumePoint_ = defIter_->toInstruction()->resumePoint();
        lastInstruction_ = defIter_->toInstruction();
      }
      defIter_++;
    } else {
      resumePoint_ = nullptr;
      lastInstruction_ = nullptr;
    }
  }

  bool more() const { return defIter_ || atResumePoint(); }

 public:
  explicit MNodeIterator(MBasicBlock* block)
      : resumePoint_(block->entryResumePoint()), defIter_(block) {
    MOZ_ASSERT(bool(block->entryResumePoint()) == atResumePoint());
  }

  MNodeIterator operator++(int) {
    MNodeIterator old(*this);
    if (more()) {
      next();
    }
    return old;
  }

  explicit operator bool() const { return more(); }

  MNode* operator*() { return getNode(); }

  MNode* operator->() { return getNode(); }
};

void MBasicBlock::add(MInstruction* ins) {
  MOZ_ASSERT(!hasLastIns());
  ins->setInstructionBlock(this, trackedSite_);
  graph().allocDefinitionId(ins);
  instructions_.pushBack(ins);
}

}  // namespace jit
}  // namespace js

#endif /* jit_MIRGraph_h */
