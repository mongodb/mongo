/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MIRGraph.h"

#include "jit/CompileInfo.h"
#include "jit/InlineScriptTree.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/JitSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"

using namespace js;
using namespace js::jit;

MIRGenerator::MIRGenerator(CompileRealm* realm,
                           const JitCompileOptions& options,
                           TempAllocator* alloc, MIRGraph* graph,
                           const CompileInfo* info,
                           const OptimizationInfo* optimizationInfo)
    : realm(realm),
      runtime(realm ? realm->runtime() : nullptr),
      outerInfo_(info),
      optimizationInfo_(optimizationInfo),
      alloc_(alloc),
      graph_(graph),
      offThreadStatus_(Ok()),
      cancelBuild_(false),
      wasmMaxStackArgBytes_(0),
      needsOverrecursedCheck_(false),
      needsStaticStackAlignment_(false),
      instrumentedProfiling_(false),
      instrumentedProfilingIsCached_(false),
      stringsCanBeInNursery_(realm ? realm->zone()->canNurseryAllocateStrings()
                                   : false),
      bigIntsCanBeInNursery_(realm ? realm->zone()->canNurseryAllocateBigInts()
                                   : false),
      minWasmMemory0Length_(0),
      options(options),
      gs_(alloc) {}

bool MIRGenerator::licmEnabled() const {
  return optimizationInfo().licmEnabled() && !disableLICM_ &&
         !outerInfo().hadLICMInvalidation();
}

bool MIRGenerator::branchHintingEnabled() const {
  return outerInfo().branchHintingEnabled();
}

mozilla::GenericErrorResult<AbortReason> MIRGenerator::abort(AbortReason r) {
  if (JitSpewEnabled(JitSpew_IonAbort)) {
    switch (r) {
      case AbortReason::Alloc:
        JitSpew(JitSpew_IonAbort, "AbortReason::Alloc");
        break;
      case AbortReason::Disable:
        JitSpew(JitSpew_IonAbort, "AbortReason::Disable");
        break;
      case AbortReason::Error:
        JitSpew(JitSpew_IonAbort, "AbortReason::Error");
        break;
      case AbortReason::NoAbort:
        MOZ_CRASH("Abort with AbortReason::NoAbort");
        break;
    }
  }
  return Err(std::move(r));
}

mozilla::GenericErrorResult<AbortReason> MIRGenerator::abortFmt(
    AbortReason r, const char* message, va_list ap) {
  JitSpewVA(JitSpew_IonAbort, message, ap);
  return Err(std::move(r));
}

mozilla::GenericErrorResult<AbortReason> MIRGenerator::abort(
    AbortReason r, const char* message, ...) {
  va_list ap;
  va_start(ap, message);
  auto forward = abortFmt(r, message, ap);
  va_end(ap);
  return forward;
}

void MIRGraph::addBlock(MBasicBlock* block) {
  MOZ_ASSERT(block);
  block->setId(blockIdGen_++);
  blocks_.pushBack(block);
  numBlocks_++;
}

void MIRGraph::insertBlockAfter(MBasicBlock* at, MBasicBlock* block) {
  block->setId(blockIdGen_++);
  blocks_.insertAfter(at, block);
  numBlocks_++;
}

void MIRGraph::insertBlockBefore(MBasicBlock* at, MBasicBlock* block) {
  block->setId(blockIdGen_++);
  blocks_.insertBefore(at, block);
  numBlocks_++;
}

void MIRGraph::removeBlock(MBasicBlock* block) {
  // Remove a block from the graph. It will also cleanup the block.

  if (block == osrBlock_) {
    osrBlock_ = nullptr;
  }

  if (returnAccumulator_) {
    size_t i = 0;
    while (i < returnAccumulator_->length()) {
      if ((*returnAccumulator_)[i] == block) {
        returnAccumulator_->erase(returnAccumulator_->begin() + i);
      } else {
        i++;
      }
    }
  }

  block->clear();
  block->markAsDead();

  if (block->isInList()) {
    blocks_.remove(block);
    numBlocks_--;
  }
}

void MIRGraph::unmarkBlocks() {
  for (MBasicBlockIterator i(blocks_.begin()); i != blocks_.end(); i++) {
    i->unmark();
  }
}

MBasicBlock* MBasicBlock::New(MIRGraph& graph, size_t stackDepth,
                              const CompileInfo& info, MBasicBlock* maybePred,
                              BytecodeSite* site, Kind kind) {
  MOZ_ASSERT(site->pc() != nullptr);

  MBasicBlock* block = new (graph.alloc()) MBasicBlock(graph, info, site, kind);
  if (!block->init()) {
    return nullptr;
  }

  if (!block->inherit(graph.alloc(), stackDepth, maybePred, 0)) {
    return nullptr;
  }

  return block;
}

MBasicBlock* MBasicBlock::NewPopN(MIRGraph& graph, const CompileInfo& info,
                                  MBasicBlock* pred, BytecodeSite* site,
                                  Kind kind, uint32_t popped) {
  MOZ_ASSERT(site->pc() != nullptr);

  MBasicBlock* block = new (graph.alloc()) MBasicBlock(graph, info, site, kind);
  if (!block->init()) {
    return nullptr;
  }

  if (!block->inherit(graph.alloc(), pred->stackDepth(), pred, popped)) {
    return nullptr;
  }

  return block;
}

MBasicBlock* MBasicBlock::NewPendingLoopHeader(MIRGraph& graph,
                                               const CompileInfo& info,
                                               MBasicBlock* pred,
                                               BytecodeSite* site) {
  MOZ_ASSERT(site->pc() != nullptr);

  MBasicBlock* block =
      new (graph.alloc()) MBasicBlock(graph, info, site, PENDING_LOOP_HEADER);
  if (!block->init()) {
    return nullptr;
  }

  if (!block->inherit(graph.alloc(), pred->stackDepth(), pred, 0)) {
    return nullptr;
  }

  return block;
}

MBasicBlock* MBasicBlock::NewSplitEdge(MIRGraph& graph, MBasicBlock* pred,
                                       size_t predEdgeIdx, MBasicBlock* succ) {
  MBasicBlock* split = nullptr;
  if (!succ->pc()) {
    // The predecessor does not have a PC, this is a Wasm compilation.
    split = MBasicBlock::New(graph, succ->info(), pred, SPLIT_EDGE);
    if (!split) {
      return nullptr;
    }

    // Insert the split edge block in-between.
    split->end(MGoto::New(graph.alloc(), succ));
  } else {
    // The predecessor has a PC, this is a Warp compilation.
    MResumePoint* succEntry = succ->entryResumePoint();

    BytecodeSite* site =
        new (graph.alloc()) BytecodeSite(succ->trackedTree(), succEntry->pc());
    split =
        new (graph.alloc()) MBasicBlock(graph, succ->info(), site, SPLIT_EDGE);

    if (!split->init()) {
      return nullptr;
    }

    // A split edge is used to simplify the graph to avoid having a
    // predecessor with multiple successors as well as a successor with
    // multiple predecessors.  As instructions can be moved in this
    // split-edge block, we need to give this block a resume point. To do
    // so, we copy the entry resume points of the successor and filter the
    // phis to keep inputs from the current edge.

    // Propagate the caller resume point from the inherited block.
    split->callerResumePoint_ = succ->callerResumePoint();

    // Split-edge are created after the interpreter stack emulation. Thus,
    // there is no need for creating slots.
    split->stackPosition_ = succEntry->stackDepth();

    // Create a resume point using our initial stack position.
    MResumePoint* splitEntry = new (graph.alloc())
        MResumePoint(split, succEntry->pc(), ResumeMode::ResumeAt);
    if (!splitEntry->init(graph.alloc())) {
      return nullptr;
    }
    split->entryResumePoint_ = splitEntry;

    // Insert the split edge block in-between.
    split->end(MGoto::New(graph.alloc(), succ));

    // The target entry resume point might have phi operands, keep the
    // operands of the phi coming from our edge.
    size_t succEdgeIdx = succ->indexForPredecessor(pred);

    for (size_t i = 0, e = splitEntry->numOperands(); i < e; i++) {
      MDefinition* def = succEntry->getOperand(i);
      // This early in the pipeline, we have no recover instructions in
      // any entry resume point.
      if (def->block() == succ) {
        if (def->isPhi()) {
          def = def->toPhi()->getOperand(succEdgeIdx);
        } else {
          // The phi-operand may already have been optimized out.
          MOZ_ASSERT(def->isConstant());
          MOZ_ASSERT(def->type() == MIRType::MagicOptimizedOut);

          def = split->optimizedOutConstant(graph.alloc());
        }
      }

      splitEntry->initOperand(i, def);
    }

    // This is done in the New variant for wasm, so we cannot keep this
    // line below, where the rest of the graph is modified.
    if (!split->predecessors_.append(pred)) {
      return nullptr;
    }
  }

  split->setLoopDepth(succ->loopDepth());

  graph.insertBlockAfter(pred, split);

  pred->replaceSuccessor(predEdgeIdx, split);
  succ->replacePredecessor(pred, split);
  return split;
}

void MBasicBlock::moveToNewBlock(MInstruction* ins, MBasicBlock* dst) {
  MOZ_ASSERT(ins->block() == this);
  MOZ_ASSERT(!dst->hasLastIns());
  instructions_.remove(ins);
  ins->setInstructionBlock(dst, dst->trackedSite());
  if (MResumePoint* rp = ins->resumePoint()) {
    removeResumePoint(rp);
    dst->addResumePoint(rp);
    rp->setBlock(dst);
  }
  dst->instructions_.pushBack(ins);
}

void MBasicBlock::moveOuterResumePointTo(MBasicBlock* dest) {
  if (MResumePoint* outer = outerResumePoint()) {
    removeResumePoint(outer);
    outerResumePoint_ = nullptr;
    dest->setOuterResumePoint(outer);
    dest->addResumePoint(outer);
    outer->setBlock(dest);
  }
}

bool MBasicBlock::wrapInstructionInFastpath(MInstruction* ins,
                                            MInstruction* fastpath,
                                            MInstruction* condition) {
  MOZ_ASSERT(ins->block() == this);
  MOZ_ASSERT(!ins->isControlInstruction());

  MInstructionIterator rest(begin(ins));
  rest++;

  MResumePoint* resumeBeforeIns = activeResumePoint(ins);
  MResumePoint* resumeAfterIns = activeResumePoint(*rest);

  // Create the join block.
  MBasicBlock* join = MBasicBlock::NewInternal(graph_, this, resumeAfterIns);
  if (!join) {
    return false;
  }

  // Update the successors of the current block.
  for (uint32_t i = 0; i < numSuccessors(); i++) {
    getSuccessor(i)->replacePredecessor(this, join);
  }
  if (successorWithPhis()) {
    join->setSuccessorWithPhis(successorWithPhis(), positionInPhiSuccessor());
    clearSuccessorWithPhis();
  }

  // Copy all instructions after |ins| into the join block.
  while (rest != end()) {
    MInstruction* ins = *rest++;
    moveToNewBlock(ins, join);
  }
  MOZ_ASSERT(!hasLastIns());
  MOZ_ASSERT(join->hasLastIns());

  graph_.insertBlockAfter(this, join);

  // Create the fast path block.
  MBasicBlock* fastpathBlock =
      MBasicBlock::NewInternal(graph_, this, resumeBeforeIns);
  if (!fastpathBlock) {
    return false;
  }
  graph_.insertBlockAfter(this, fastpathBlock);
  fastpathBlock->add(fastpath);
  fastpathBlock->end(MGoto::New(graph_.alloc(), join));

  // Create the slowpath block.
  MBasicBlock* slowpathBlock =
      MBasicBlock::NewInternal(graph_, this, resumeBeforeIns);
  if (!slowpathBlock) {
    return false;
  }
  graph_.insertBlockAfter(fastpathBlock, slowpathBlock);
  moveToNewBlock(ins, slowpathBlock);
  slowpathBlock->end(MGoto::New(graph_.alloc(), join));

  // Connect current block to fastpath and slowpath.
  add(condition);
  end(MTest::New(graph_.alloc(), condition, fastpathBlock, slowpathBlock));

  // Update predecessors.
  if (!fastpathBlock->addPredecessorWithoutPhis(this) ||
      !slowpathBlock->addPredecessorWithoutPhis(this) ||
      !join->addPredecessorWithoutPhis(fastpathBlock) ||
      !join->addPredecessorWithoutPhis(slowpathBlock)) {
    return false;
  }

  if (ins->hasUses()) {
    // Insert phi.
    MPhi* phi = MPhi::New(graph_.alloc());
    if (!phi->reserveLength(2)) {
      return false;
    }
    phi->addInput(fastpath);
    fastpathBlock->setSuccessorWithPhis(join, 0);
    phi->addInput(ins);
    slowpathBlock->setSuccessorWithPhis(join, 1);
    join->addPhi(phi);

    for (MUseIterator i(ins->usesBegin()), e(ins->usesEnd()); i != e;) {
      MUse* use = *i++;
      if (use->consumer() != phi && use->consumer() != ins->resumePoint()) {
        use->replaceProducer(phi);
      }
    }
  }

  moveOuterResumePointTo(join);

  return true;
}

MBasicBlock* MBasicBlock::NewInternal(MIRGraph& graph, MBasicBlock* orig,
                                      MResumePoint* resumePoint) {
  jsbytecode* pc = IsResumeAfter(resumePoint->mode())
                       ? GetNextPc(resumePoint->pc())
                       : resumePoint->pc();

  BytecodeSite* site =
      new (graph.alloc()) BytecodeSite(orig->trackedTree(), pc);
  MBasicBlock* block =
      new (graph.alloc()) MBasicBlock(graph, orig->info(), site, INTERNAL);
  if (!block->init()) {
    return nullptr;
  }

  // Propagate the caller resume point from the original block.
  block->callerResumePoint_ = orig->callerResumePoint();

  // Copy the resume point.
  block->stackPosition_ = resumePoint->stackDepth();
  MResumePoint* entryResumePoint =
      new (graph.alloc()) MResumePoint(block, pc, ResumeMode::ResumeAt);
  if (!entryResumePoint->init(graph.alloc())) {
    return nullptr;
  }
  for (size_t i = 0; i < resumePoint->stackDepth(); i++) {
    entryResumePoint->initOperand(i, resumePoint->getOperand(i));
  }
  block->entryResumePoint_ = entryResumePoint;

  block->setLoopDepth(orig->loopDepth());

  return block;
}

MBasicBlock* MBasicBlock::New(MIRGraph& graph, const CompileInfo& info,
                              MBasicBlock* pred, Kind kind) {
  BytecodeSite* site = new (graph.alloc()) BytecodeSite();
  MBasicBlock* block = new (graph.alloc()) MBasicBlock(graph, info, site, kind);
  if (!block->init()) {
    return nullptr;
  }

  if (pred) {
    block->stackPosition_ = pred->stackPosition_;

    if (block->kind_ == PENDING_LOOP_HEADER) {
      size_t nphis = block->stackPosition_;

      size_t nfree = graph.phiFreeListLength();

      TempAllocator& alloc = graph.alloc();
      MPhi* phis = nullptr;
      if (nphis > nfree) {
        phis = alloc.allocateArray<MPhi>(nphis - nfree);
        if (!phis) {
          return nullptr;
        }
      }

      // Note: Phis are inserted in the same order as the slots.
      for (size_t i = 0; i < nphis; i++) {
        MDefinition* predSlot = pred->getSlot(i);

        MOZ_ASSERT(predSlot->type() != MIRType::Value);

        MPhi* phi;
        if (i < nfree) {
          phi = graph.takePhiFromFreeList();
        } else {
          phi = phis + (i - nfree);
        }
        new (phi) MPhi(alloc, predSlot->type());

        phi->addInlineInput(predSlot);

        // Add append Phis in the block.
        block->addPhi(phi);
        block->setSlot(i, phi);
      }
    } else {
      if (!block->ensureHasSlots(0)) {
        return nullptr;
      }
      block->copySlots(pred);
    }

    if (!block->predecessors_.append(pred)) {
      return nullptr;
    }
  }

  return block;
}

// Create an empty and unreachable block which jumps to |header|. Used
// when the normal entry into a loop is removed (but the loop is still
// reachable due to OSR) to preserve the invariant that every loop
// header has two predecessors, which is needed for building the
// dominator tree. The new block is inserted immediately before the
// header, which preserves the graph ordering (post-order/RPO). These
// blocks will all be removed before lowering.
MBasicBlock* MBasicBlock::NewFakeLoopPredecessor(MIRGraph& graph,
                                                 MBasicBlock* header) {
  MOZ_ASSERT(graph.osrBlock());

  MBasicBlock* backedge = header->backedge();
  MBasicBlock* fake = MBasicBlock::New(graph, header->info(), nullptr,
                                       MBasicBlock::FAKE_LOOP_PRED);
  if (!fake) {
    return nullptr;
  }

  graph.insertBlockBefore(header, fake);
  fake->setUnreachable();

  // Create fake defs to use as inputs for any phis in |header|.
  for (MPhiIterator iter(header->phisBegin()), end(header->phisEnd());
       iter != end; ++iter) {
    if (!graph.alloc().ensureBallast()) {
      return nullptr;
    }
    MPhi* phi = *iter;
    auto* fakeDef = MUnreachableResult::New(graph.alloc(), phi->type());
    fake->add(fakeDef);
    if (!phi->addInputSlow(fakeDef)) {
      return nullptr;
    }
  }

  fake->end(MGoto::New(graph.alloc(), header));

  if (!header->addPredecessorWithoutPhis(fake)) {
    return nullptr;
  }

  // The backedge is always the last predecessor, but we have added a
  // new pred. Restore |backedge| as |header|'s loop backedge.
  header->clearLoopHeader();
  header->setLoopHeader(backedge);

  return fake;
}

void MIRGraph::removeFakeLoopPredecessors() {
  MOZ_ASSERT(osrBlock());
  size_t id = 0;
  for (ReversePostorderIterator it = rpoBegin(); it != rpoEnd();) {
    MBasicBlock* block = *it++;
    if (block->isFakeLoopPred()) {
      MOZ_ASSERT(block->unreachable());
      MBasicBlock* succ = block->getSingleSuccessor();
      succ->removePredecessor(block);
      removeBlock(block);
    } else {
      block->setId(id++);
    }
  }
#ifdef DEBUG
  canBuildDominators_ = false;
#endif
}

MBasicBlock::MBasicBlock(MIRGraph& graph, const CompileInfo& info,
                         BytecodeSite* site, Kind kind)
    : graph_(graph),
      info_(info),
      predecessors_(graph.alloc()),
      stackPosition_(info_.firstStackSlot()),
      id_(0),
      domIndex_(0),
      numDominated_(0),
      lir_(nullptr),
      callerResumePoint_(nullptr),
      entryResumePoint_(nullptr),
      outerResumePoint_(nullptr),
      successorWithPhis_(nullptr),
      positionInPhiSuccessor_(0),
      loopDepth_(0),
      kind_(kind),
      mark_(false),
      immediatelyDominated_(graph.alloc()),
      immediateDominator_(nullptr),
      trackedSite_(site) {
  MOZ_ASSERT(trackedSite_, "trackedSite_ is non-nullptr");
}

bool MBasicBlock::init() { return slots_.init(graph_.alloc(), info_.nslots()); }

bool MBasicBlock::increaseSlots(size_t num) {
  return slots_.growBy(graph_.alloc(), num);
}

bool MBasicBlock::ensureHasSlots(size_t num) {
  size_t depth = stackDepth() + num;
  if (depth > nslots()) {
    if (!increaseSlots(depth - nslots())) {
      return false;
    }
  }
  return true;
}

void MBasicBlock::copySlots(MBasicBlock* from) {
  MOZ_ASSERT(stackPosition_ <= from->stackPosition_);
  MOZ_ASSERT(stackPosition_ <= nslots());

  MDefinition** thisSlots = slots_.begin();
  MDefinition** fromSlots = from->slots_.begin();
  for (size_t i = 0, e = stackPosition_; i < e; ++i) {
    thisSlots[i] = fromSlots[i];
  }
}

bool MBasicBlock::inherit(TempAllocator& alloc, size_t stackDepth,
                          MBasicBlock* maybePred, uint32_t popped) {
  MOZ_ASSERT_IF(maybePred, maybePred->stackDepth() == stackDepth);

  MOZ_ASSERT(stackDepth >= popped);
  stackDepth -= popped;
  stackPosition_ = stackDepth;

  if (maybePred && kind_ != PENDING_LOOP_HEADER) {
    copySlots(maybePred);
  }

  MOZ_ASSERT(info_.nslots() >= stackPosition_);
  MOZ_ASSERT(!entryResumePoint_);

  // Propagate the caller resume point from the inherited block.
  callerResumePoint_ = maybePred ? maybePred->callerResumePoint() : nullptr;

  // Create a resume point using our initial stack state.
  entryResumePoint_ =
      new (alloc) MResumePoint(this, pc(), ResumeMode::ResumeAt);
  if (!entryResumePoint_->init(alloc)) {
    return false;
  }

  if (maybePred) {
    if (!predecessors_.append(maybePred)) {
      return false;
    }

    if (kind_ == PENDING_LOOP_HEADER) {
      for (size_t i = 0; i < stackDepth; i++) {
        MPhi* phi = MPhi::New(alloc.fallible());
        if (!phi) {
          return false;
        }
        phi->addInlineInput(maybePred->getSlot(i));
        addPhi(phi);
        setSlot(i, phi);
        entryResumePoint()->initOperand(i, phi);
      }
    } else {
      for (size_t i = 0; i < stackDepth; i++) {
        entryResumePoint()->initOperand(i, getSlot(i));
      }
    }
  } else {
    /*
     * Don't leave the operands uninitialized for the caller, as it may not
     * initialize them later on.
     */
    for (size_t i = 0; i < stackDepth; i++) {
      entryResumePoint()->clearOperand(i);
    }
  }

  return true;
}

void MBasicBlock::inheritSlots(MBasicBlock* parent) {
  stackPosition_ = parent->stackPosition_;
  copySlots(parent);
}

bool MBasicBlock::initEntrySlots(TempAllocator& alloc) {
  // Remove the previous resume point.
  discardResumePoint(entryResumePoint_);

  // Create a resume point using our initial stack state.
  entryResumePoint_ =
      MResumePoint::New(alloc, this, pc(), ResumeMode::ResumeAt);
  if (!entryResumePoint_) {
    return false;
  }
  return true;
}

MDefinition* MBasicBlock::environmentChain() {
  return getSlot(info().environmentChainSlot());
}

MDefinition* MBasicBlock::argumentsObject() {
  return getSlot(info().argsObjSlot());
}

void MBasicBlock::setEnvironmentChain(MDefinition* scopeObj) {
  setSlot(info().environmentChainSlot(), scopeObj);
}

void MBasicBlock::setArgumentsObject(MDefinition* argsObj) {
  setSlot(info().argsObjSlot(), argsObj);
}

void MBasicBlock::pick(int32_t depth) {
  // pick takes a value and moves it to the top.
  // pick(-2):
  //   A B C D E
  //   A B D C E [ swapAt(-2) ]
  //   A B D E C [ swapAt(-1) ]
  for (; depth < 0; depth++) {
    swapAt(depth);
  }
}

void MBasicBlock::unpick(int32_t depth) {
  // unpick takes the value on top of the stack and moves it under the depth-th
  // element;
  // unpick(-2):
  //   A B C D E
  //   A B C E D [ swapAt(-1) ]
  //   A B E C D [ swapAt(-2) ]
  for (int32_t n = -1; n >= depth; n--) {
    swapAt(n);
  }
}

void MBasicBlock::swapAt(int32_t depth) {
  uint32_t lhsDepth = stackPosition_ + depth - 1;
  uint32_t rhsDepth = stackPosition_ + depth;

  MDefinition* temp = slots_[lhsDepth];
  slots_[lhsDepth] = slots_[rhsDepth];
  slots_[rhsDepth] = temp;
}

void MBasicBlock::discardLastIns() { discard(lastIns()); }

MConstant* MBasicBlock::optimizedOutConstant(TempAllocator& alloc) {
  // If the first instruction is a MConstant(MagicValue(JS_OPTIMIZED_OUT))
  // then reuse it.
  MInstruction* ins = *begin();
  if (ins->type() == MIRType::MagicOptimizedOut) {
    return ins->toConstant();
  }

  MConstant* constant = MConstant::New(alloc, MagicValue(JS_OPTIMIZED_OUT));
  insertBefore(ins, constant);
  return constant;
}

void MBasicBlock::moveBefore(MInstruction* at, MInstruction* ins) {
  // Remove |ins| from the current block.
  MOZ_ASSERT(ins->block() == this);
  instructions_.remove(ins);

  // Insert into new block, which may be distinct.
  // Uses and operands are untouched.
  ins->setInstructionBlock(at->block(), at->trackedSite());
  at->block()->instructions_.insertBefore(at, ins);
}

MInstruction* MBasicBlock::safeInsertTop(MDefinition* ins, IgnoreTop ignore) {
  MOZ_ASSERT(graph().osrBlock() != this,
             "We are not supposed to add any instruction in OSR blocks.");

  // Beta nodes and interrupt checks are required to be located at the
  // beginnings of basic blocks, so we must insert new instructions after any
  // such instructions.
  MInstructionIterator insertIter =
      !ins || ins->isPhi() ? begin() : begin(ins->toInstruction());
  while (insertIter->isBeta() || insertIter->isInterruptCheck() ||
         insertIter->isConstant() || insertIter->isParameter() ||
         (!(ignore & IgnoreRecover) && insertIter->isRecoveredOnBailout())) {
    insertIter++;
  }

  return *insertIter;
}

void MBasicBlock::discardResumePoint(
    MResumePoint* rp, ReferencesType refType /* = RefType_Default */) {
  if (refType & RefType_DiscardOperands) {
    rp->releaseUses();
  }
  rp->setDiscarded();
  removeResumePoint(rp);
}

void MBasicBlock::removeResumePoint(MResumePoint* rp) {
#ifdef DEBUG
  MResumePointIterator iter = resumePointsBegin();
  while (*iter != rp) {
    // We should reach it before reaching the end.
    MOZ_ASSERT(iter != resumePointsEnd());
    iter++;
  }
  resumePoints_.removeAt(iter);
#endif
}

void MBasicBlock::prepareForDiscard(
    MInstruction* ins, ReferencesType refType /* = RefType_Default */) {
  // Only remove instructions from the same basic block.  This is needed for
  // correctly removing the resume point if any.
  MOZ_ASSERT(ins->block() == this);

  MResumePoint* rp = ins->resumePoint();
  if ((refType & RefType_DiscardResumePoint) && rp) {
    discardResumePoint(rp, refType);
  }

  // We need to assert that instructions have no uses after removing the their
  // resume points operands as they could be captured by their own resume
  // point.
  MOZ_ASSERT_IF(refType & RefType_AssertNoUses, !ins->hasUses());

  const uint32_t InstructionOperands =
      RefType_DiscardOperands | RefType_DiscardInstruction;
  if ((refType & InstructionOperands) == InstructionOperands) {
    for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
      ins->releaseOperand(i);
    }
  }

  ins->setDiscarded();
}

void MBasicBlock::discard(MInstruction* ins) {
  prepareForDiscard(ins);
  instructions_.remove(ins);
}

void MBasicBlock::discardIgnoreOperands(MInstruction* ins) {
#ifdef DEBUG
  for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
    MOZ_ASSERT(!ins->hasOperand(i));
  }
#endif

  prepareForDiscard(ins, RefType_IgnoreOperands);
  instructions_.remove(ins);
}

void MBasicBlock::discardAllInstructions() {
  MInstructionIterator iter = begin();
  discardAllInstructionsStartingAt(iter);
}

void MBasicBlock::discardAllInstructionsStartingAt(MInstructionIterator iter) {
  while (iter != end()) {
    // Discard operands and resume point operands and flag the instruction
    // as discarded.  Also we do not assert that we have no uses as blocks
    // might be removed in reverse post order.
    MInstruction* ins = *iter++;
    prepareForDiscard(ins, RefType_DefaultNoAssert);
    instructions_.remove(ins);
  }
}

void MBasicBlock::discardAllPhis() {
  for (MPhiIterator iter = phisBegin(); iter != phisEnd(); iter++) {
    iter->removeAllOperands();
  }

  for (MBasicBlock** pred = predecessors_.begin(); pred != predecessors_.end();
       pred++) {
    (*pred)->clearSuccessorWithPhis();
  }

  phis_.clear();
}

void MBasicBlock::discardAllResumePoints(bool discardEntry) {
  if (outerResumePoint_) {
    clearOuterResumePoint();
  }

  if (discardEntry && entryResumePoint_) {
    clearEntryResumePoint();
  }

#ifdef DEBUG
  if (!entryResumePoint()) {
    MOZ_ASSERT(resumePointsEmpty());
  } else {
    MResumePointIterator iter(resumePointsBegin());
    MOZ_ASSERT(iter != resumePointsEnd());
    iter++;
    MOZ_ASSERT(iter == resumePointsEnd());
  }
#endif
}

void MBasicBlock::clear() {
  discardAllInstructions();
  discardAllResumePoints();
  discardAllPhis();
}

void MBasicBlock::insertBefore(MInstruction* at, MInstruction* ins) {
  MOZ_ASSERT(at->block() == this);
  ins->setInstructionBlock(this, at->trackedSite());
  graph().allocDefinitionId(ins);
  instructions_.insertBefore(at, ins);
}

void MBasicBlock::insertAfter(MInstruction* at, MInstruction* ins) {
  MOZ_ASSERT(at->block() == this);
  ins->setInstructionBlock(this, at->trackedSite());
  graph().allocDefinitionId(ins);
  instructions_.insertAfter(at, ins);
}

void MBasicBlock::insertAtEnd(MInstruction* ins) {
  if (hasLastIns()) {
    insertBefore(lastIns(), ins);
  } else {
    add(ins);
  }
}

void MBasicBlock::addPhi(MPhi* phi) {
  phis_.pushBack(phi);
  phi->setPhiBlock(this);
  graph().allocDefinitionId(phi);
}

void MBasicBlock::discardPhi(MPhi* phi) {
  MOZ_ASSERT(!phis_.empty());

  phi->removeAllOperands();
  phi->setDiscarded();

  phis_.remove(phi);

  if (phis_.empty()) {
    for (MBasicBlock* pred : predecessors_) {
      pred->clearSuccessorWithPhis();
    }
  }
}

MResumePoint* MBasicBlock::activeResumePoint(MInstruction* ins) {
  for (MInstructionReverseIterator iter = rbegin(ins); iter != rend(); iter++) {
    if (iter->resumePoint() && *iter != ins) {
      return iter->resumePoint();
    }
  }

  // If none, take the entry resume point.
  return entryResumePoint();
}

void MBasicBlock::flagOperandsOfPrunedBranches(MInstruction* ins) {
  MResumePoint* rp = activeResumePoint(ins);

  // The only blocks which do not have any entryResumePoint in Ion, are the
  // SplitEdge blocks.  SplitEdge blocks only have a Goto instruction before
  // Range Analysis phase.  In adjustInputs, we are manipulating instructions
  // which have a TypePolicy.  So, as a Goto has no operand and no type
  // policy, the entry resume point should exist.
  MOZ_ASSERT(rp);

  // Flag all operands as being potentially used.
  while (rp) {
    for (size_t i = 0, end = rp->numOperands(); i < end; i++) {
      rp->getOperand(i)->setImplicitlyUsedUnchecked();
    }
    rp = rp->caller();
  }
}

bool MBasicBlock::addPredecessor(TempAllocator& alloc, MBasicBlock* pred) {
  return addPredecessorPopN(alloc, pred, 0);
}

bool MBasicBlock::addPredecessorPopN(TempAllocator& alloc, MBasicBlock* pred,
                                     uint32_t popped) {
  MOZ_ASSERT(pred);
  MOZ_ASSERT(predecessors_.length() > 0);

  // Predecessors must be finished, and at the correct stack depth.
  MOZ_ASSERT(pred->hasLastIns());
  MOZ_ASSERT(pred->stackPosition_ == stackPosition_ + popped);

  for (uint32_t i = 0, e = stackPosition_; i < e; ++i) {
    MDefinition* mine = getSlot(i);
    MDefinition* other = pred->getSlot(i);

    if (mine != other) {
      MIRType phiType = mine->type();
      if (phiType != other->type()) {
        phiType = MIRType::Value;
      }

      // If the current instruction is a phi, and it was created in this
      // basic block, then we have already placed this phi and should
      // instead append to its operands.
      if (mine->isPhi() && mine->block() == this) {
        MOZ_ASSERT(predecessors_.length());
        MOZ_ASSERT(!mine->hasDefUses(),
                   "should only change type of newly created phis");
        mine->setResultType(phiType);
        if (!mine->toPhi()->addInputSlow(other)) {
          return false;
        }
      } else {
        // Otherwise, create a new phi node.
        MPhi* phi = MPhi::New(alloc.fallible(), phiType);
        if (!phi) {
          return false;
        }
        addPhi(phi);

        // Prime the phi for each predecessor, so input(x) comes from
        // predecessor(x).
        if (!phi->reserveLength(predecessors_.length() + 1)) {
          return false;
        }

        for (size_t j = 0, numPreds = predecessors_.length(); j < numPreds;
             ++j) {
          MOZ_ASSERT(predecessors_[j]->getSlot(i) == mine);
          phi->addInput(mine);
        }
        phi->addInput(other);

        setSlot(i, phi);
        if (entryResumePoint()) {
          entryResumePoint()->replaceOperand(i, phi);
        }
      }
    }
  }

  return predecessors_.append(pred);
}

bool MBasicBlock::addPredecessorSameInputsAs(MBasicBlock* pred,
                                             MBasicBlock* existingPred) {
  MOZ_ASSERT(pred);
  MOZ_ASSERT(predecessors_.length() > 0);

  // Predecessors must be finished, and at the correct stack depth.
  MOZ_ASSERT(pred->hasLastIns());
  MOZ_ASSERT(!pred->successorWithPhis());

  if (!phisEmpty()) {
    size_t existingPosition = indexForPredecessor(existingPred);
    for (MPhiIterator iter = phisBegin(); iter != phisEnd(); iter++) {
      if (!iter->addInputSlow(iter->getOperand(existingPosition))) {
        return false;
      }
    }
  }

  if (!predecessors_.append(pred)) {
    return false;
  }
  return true;
}

bool MBasicBlock::addPredecessorWithoutPhis(MBasicBlock* pred) {
  // Predecessors must be finished.
  MOZ_ASSERT(pred && pred->hasLastIns());
  return predecessors_.append(pred);
}

bool MBasicBlock::addImmediatelyDominatedBlock(MBasicBlock* child) {
  return immediatelyDominated_.append(child);
}

void MBasicBlock::removeImmediatelyDominatedBlock(MBasicBlock* child) {
  for (size_t i = 0;; ++i) {
    MOZ_ASSERT(i < immediatelyDominated_.length(),
               "Dominated block to remove not present");
    if (immediatelyDominated_[i] == child) {
      immediatelyDominated_[i] = immediatelyDominated_.back();
      immediatelyDominated_.popBack();
      return;
    }
  }
}

bool MBasicBlock::setBackedge(MBasicBlock* pred) {
  // Predecessors must be finished, and at the correct stack depth.
  MOZ_ASSERT(hasLastIns());
  MOZ_ASSERT(pred->hasLastIns());
  MOZ_ASSERT(pred->stackDepth() == entryResumePoint()->stackDepth());

  // We must be a pending loop header
  MOZ_ASSERT(kind_ == PENDING_LOOP_HEADER);

  // Add exit definitions to each corresponding phi at the entry.
  if (!inheritPhisFromBackedge(pred)) {
    return false;
  }

  // We are now a loop header proper
  kind_ = LOOP_HEADER;

  return predecessors_.append(pred);
}

bool MBasicBlock::setBackedgeWasm(MBasicBlock* pred, size_t paramCount) {
  // Predecessors must be finished, and at the correct stack depth.
  MOZ_ASSERT(hasLastIns());
  MOZ_ASSERT(pred->hasLastIns());
  MOZ_ASSERT(stackDepth() + paramCount == pred->stackDepth());

  // We must be a pending loop header
  MOZ_ASSERT(kind_ == PENDING_LOOP_HEADER);

  // Add exit definitions to each corresponding phi at the entry.
  // Note: Phis are inserted in the same order as the slots. (see
  // MBasicBlock::New)
  size_t slot = 0;
  for (MPhiIterator phi = phisBegin(); phi != phisEnd(); phi++, slot++) {
    MPhi* entryDef = *phi;
    MDefinition* exitDef = pred->getSlot(slot);

    // Assert that we already placed phis for each slot.
    MOZ_ASSERT(entryDef->block() == this);

    // Assert that the phi already has the correct type.
    MOZ_ASSERT(entryDef->type() == exitDef->type());
    MOZ_ASSERT(entryDef->type() != MIRType::Value);

    if (entryDef == exitDef) {
      // If the exit def is the same as the entry def, make a redundant
      // phi. Since loop headers have exactly two incoming edges, we
      // know that that's just the first input.
      //
      // Note that we eliminate later rather than now, to avoid any
      // weirdness around pending continue edges which might still hold
      // onto phis.
      exitDef = entryDef->getOperand(0);
    }

    // Phis always have room for 2 operands, so this can't fail.
    MOZ_ASSERT(phi->numOperands() == 1);
    entryDef->addInlineInput(exitDef);

    // Two cases here: phis that correspond to locals, and phis that correspond
    // to loop parameters.  Only phis for locals go in slots.
    if (slot < stackDepth()) {
      setSlot(slot, entryDef);
    }
  }

  // We are now a loop header proper
  kind_ = LOOP_HEADER;

  return predecessors_.append(pred);
}

void MBasicBlock::clearLoopHeader() {
  MOZ_ASSERT(isLoopHeader());
  kind_ = NORMAL;
}

void MBasicBlock::setLoopHeader(MBasicBlock* newBackedge) {
  MOZ_ASSERT(!isLoopHeader());
  kind_ = LOOP_HEADER;

  size_t numPreds = numPredecessors();
  MOZ_ASSERT(numPreds != 0);

  size_t lastIndex = numPreds - 1;
  size_t oldIndex = 0;
  for (;; ++oldIndex) {
    MOZ_ASSERT(oldIndex < numPreds);
    MBasicBlock* pred = getPredecessor(oldIndex);
    if (pred == newBackedge) {
      break;
    }
  }

  // Set the loop backedge to be the last element in predecessors_.
  std::swap(predecessors_[oldIndex], predecessors_[lastIndex]);

  // If we have phis, reorder their operands accordingly.
  if (!phisEmpty()) {
    getPredecessor(oldIndex)->setSuccessorWithPhis(this, oldIndex);
    getPredecessor(lastIndex)->setSuccessorWithPhis(this, lastIndex);
    for (MPhiIterator iter(phisBegin()), end(phisEnd()); iter != end; ++iter) {
      MPhi* phi = *iter;
      MDefinition* last = phi->getOperand(oldIndex);
      MDefinition* old = phi->getOperand(lastIndex);
      phi->replaceOperand(oldIndex, old);
      phi->replaceOperand(lastIndex, last);
    }
  }

  MOZ_ASSERT(newBackedge->loopHeaderOfBackedge() == this);
  MOZ_ASSERT(backedge() == newBackedge);
}

size_t MBasicBlock::getSuccessorIndex(MBasicBlock* block) const {
  MOZ_ASSERT(lastIns());
  for (size_t i = 0; i < numSuccessors(); i++) {
    if (getSuccessor(i) == block) {
      return i;
    }
  }
  MOZ_CRASH("Invalid successor");
}

size_t MBasicBlock::getPredecessorIndex(MBasicBlock* block) const {
  for (size_t i = 0, e = numPredecessors(); i < e; ++i) {
    if (getPredecessor(i) == block) {
      return i;
    }
  }
  MOZ_CRASH("Invalid predecessor");
}

void MBasicBlock::replaceSuccessor(size_t pos, MBasicBlock* split) {
  MOZ_ASSERT(lastIns());

  // Note, during split-critical-edges, successors-with-phis is not yet set.
  // During PAA, this case is handled before we enter.
  MOZ_ASSERT_IF(successorWithPhis_, successorWithPhis_ != getSuccessor(pos));

  lastIns()->replaceSuccessor(pos, split);
}

void MBasicBlock::replacePredecessor(MBasicBlock* old, MBasicBlock* split) {
  for (size_t i = 0; i < numPredecessors(); i++) {
    if (getPredecessor(i) == old) {
      predecessors_[i] = split;

#ifdef DEBUG
      // The same block should not appear twice in the predecessor list.
      for (size_t j = i; j < numPredecessors(); j++) {
        MOZ_ASSERT(predecessors_[j] != old);
      }
#endif

      return;
    }
  }

  MOZ_CRASH("predecessor was not found");
}

void MBasicBlock::clearDominatorInfo() {
  setImmediateDominator(nullptr);
  immediatelyDominated_.clear();
  numDominated_ = 0;
}

void MBasicBlock::removePredecessorWithoutPhiOperands(MBasicBlock* pred,
                                                      size_t predIndex) {
  // If we're removing the last backedge, this is no longer a loop.
  if (isLoopHeader() && hasUniqueBackedge() && backedge() == pred) {
    clearLoopHeader();
  }

  // Adjust phis.  Note that this can leave redundant phis behind.
  // Don't adjust successorWithPhis() if we haven't constructed this
  // information yet.
  if (pred->successorWithPhis()) {
    MOZ_ASSERT(pred->positionInPhiSuccessor() == predIndex);
    pred->clearSuccessorWithPhis();
    for (size_t j = predIndex + 1; j < numPredecessors(); j++) {
      getPredecessor(j)->setSuccessorWithPhis(this, j - 1);
    }
  }

  // Remove from pred list.
  predecessors_.erase(predecessors_.begin() + predIndex);
}

void MBasicBlock::removePredecessor(MBasicBlock* pred) {
  size_t predIndex = getPredecessorIndex(pred);

  // Remove the phi operands.
  for (MPhiIterator iter(phisBegin()), end(phisEnd()); iter != end; ++iter) {
    iter->removeOperand(predIndex);
  }

  // Now we can call the underlying function, which expects that phi
  // operands have been removed.
  removePredecessorWithoutPhiOperands(pred, predIndex);
}

bool MBasicBlock::inheritPhisFromBackedge(MBasicBlock* backedge) {
  // We must be a pending loop header
  MOZ_ASSERT(kind_ == PENDING_LOOP_HEADER);

  size_t stackDepth = entryResumePoint()->stackDepth();
  for (size_t slot = 0; slot < stackDepth; slot++) {
    // Get the value stack-slot of the back edge.
    MDefinition* exitDef = backedge->getSlot(slot);

    // Get the value of the loop header.
    MDefinition* loopDef = entryResumePoint()->getOperand(slot);
    if (loopDef->block() != this) {
      // If we are finishing a pending loop header, then we need to ensure
      // that all operands are phis. This is usualy the case, except for
      // object/arrays build with generators, in which case we share the
      // same allocations across all blocks.
      MOZ_ASSERT(loopDef->block()->id() < id());
      MOZ_ASSERT(loopDef == exitDef);
      continue;
    }

    // Phis are allocated by NewPendingLoopHeader.
    MPhi* entryDef = loopDef->toPhi();
    MOZ_ASSERT(entryDef->block() == this);

    if (entryDef == exitDef) {
      // If the exit def is the same as the entry def, make a redundant
      // phi. Since loop headers have exactly two incoming edges, we
      // know that that's just the first input.
      //
      // Note that we eliminate later rather than now, to avoid any
      // weirdness around pending continue edges which might still hold
      // onto phis.
      exitDef = entryDef->getOperand(0);
    }

    if (!entryDef->addInputSlow(exitDef)) {
      return false;
    }
  }

  return true;
}

MTest* MBasicBlock::immediateDominatorBranch(BranchDirection* pdirection) {
  *pdirection = FALSE_BRANCH;

  if (numPredecessors() != 1) {
    return nullptr;
  }

  MBasicBlock* dom = immediateDominator();
  if (dom != getPredecessor(0)) {
    return nullptr;
  }

  // Look for a trailing MTest branching to this block.
  MInstruction* ins = dom->lastIns();
  if (ins->isTest()) {
    MTest* test = ins->toTest();

    MOZ_ASSERT(test->ifTrue() == this || test->ifFalse() == this);
    if (test->ifTrue() == this && test->ifFalse() == this) {
      return nullptr;
    }

    *pdirection = (test->ifTrue() == this) ? TRUE_BRANCH : FALSE_BRANCH;
    return test;
  }

  return nullptr;
}

void MBasicBlock::dumpStack(GenericPrinter& out) {
#ifdef DEBUG
  out.printf(" %-3s %-16s %-6s %-10s\n", "#", "name", "copyOf", "first/next");
  out.printf("-------------------------------------------\n");
  for (uint32_t i = 0; i < stackPosition_; i++) {
    out.printf(" %-3u", i);
    out.printf(" %-16p\n", (void*)slots_[i]);
  }
#endif
}

void MBasicBlock::dumpStack() {
  Fprinter out(stderr);
  dumpStack(out);
  out.finish();
}

void MIRGraph::dump(GenericPrinter& out) {
#ifdef JS_JITSPEW
  for (MBasicBlockIterator iter(begin()); iter != end(); iter++) {
    iter->dump(out);
    out.printf("\n");
  }
#endif
}

void MIRGraph::dump() {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}

void MBasicBlock::dump(GenericPrinter& out) {
#ifdef JS_JITSPEW
  out.printf("block%u:%s%s%s\n", id(), isLoopHeader() ? " (loop header)" : "",
             unreachable() ? " (unreachable)" : "",
             isMarked() ? " (marked)" : "");
  if (MResumePoint* resume = entryResumePoint()) {
    resume->dump(out);
  }
  for (MPhiIterator iter(phisBegin()); iter != phisEnd(); iter++) {
    iter->dump(out);
  }
  for (MInstructionIterator iter(begin()); iter != end(); iter++) {
    iter->dump(out);
  }
#endif
}

void MBasicBlock::dump() {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}
