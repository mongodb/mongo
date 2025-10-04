/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ScalarReplacement.h"

#include "jit/IonAnalysis.h"
#include "jit/JitSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"
#include "jit/WarpBuilderShared.h"
#include "js/Vector.h"
#include "vm/ArgumentsObject.h"

#include "gc/ObjectKind-inl.h"

namespace js {
namespace jit {

template <typename MemoryView>
class EmulateStateOf {
 private:
  using BlockState = typename MemoryView::BlockState;

  MIRGenerator* mir_;
  MIRGraph& graph_;

  // Block state at the entrance of all basic blocks.
  Vector<BlockState*, 8, SystemAllocPolicy> states_;

 public:
  EmulateStateOf(MIRGenerator* mir, MIRGraph& graph)
      : mir_(mir), graph_(graph) {}

  bool run(MemoryView& view);
};

template <typename MemoryView>
bool EmulateStateOf<MemoryView>::run(MemoryView& view) {
  // Initialize the current block state of each block to an unknown state.
  if (!states_.appendN(nullptr, graph_.numBlocks())) {
    return false;
  }

  // Initialize the first block which needs to be traversed in RPO.
  MBasicBlock* startBlock = view.startingBlock();
  if (!view.initStartingState(&states_[startBlock->id()])) {
    return false;
  }

  // Iterate over each basic block which has a valid entry state, and merge
  // the state in the successor blocks.
  for (ReversePostorderIterator block = graph_.rpoBegin(startBlock);
       block != graph_.rpoEnd(); block++) {
    if (mir_->shouldCancel(MemoryView::phaseName)) {
      return false;
    }

    // Get the block state as the result of the merge of all predecessors
    // which have already been visited in RPO.  This means that backedges
    // are not yet merged into the loop.
    BlockState* state = states_[block->id()];
    if (!state) {
      continue;
    }
    view.setEntryBlockState(state);

    // Iterates over resume points, phi and instructions.
    for (MNodeIterator iter(*block); iter;) {
      // Increment the iterator before visiting the instruction, as the
      // visit function might discard itself from the basic block.
      MNode* ins = *iter++;
      if (ins->isDefinition()) {
        MDefinition* def = ins->toDefinition();
        switch (def->op()) {
#define MIR_OP(op)                 \
  case MDefinition::Opcode::op:    \
    view.visit##op(def->to##op()); \
    break;
          MIR_OPCODE_LIST(MIR_OP)
#undef MIR_OP
        }
      } else {
        view.visitResumePoint(ins->toResumePoint());
      }
      if (!graph_.alloc().ensureBallast()) {
        return false;
      }
      if (view.oom()) {
        return false;
      }
    }

    // For each successor, merge the current state into the state of the
    // successors.
    for (size_t s = 0; s < block->numSuccessors(); s++) {
      MBasicBlock* succ = block->getSuccessor(s);
      if (!view.mergeIntoSuccessorState(*block, succ, &states_[succ->id()])) {
        return false;
      }
    }
  }

  states_.clear();
  return true;
}

static inline bool IsOptimizableObjectInstruction(MInstruction* ins) {
  return ins->isNewObject() || ins->isNewPlainObject() ||
         ins->isNewCallObject() || ins->isNewIterator();
}

static bool PhiOperandEqualTo(MDefinition* operand, MInstruction* newObject) {
  if (operand == newObject) {
    return true;
  }

  switch (operand->op()) {
    case MDefinition::Opcode::GuardShape:
      return PhiOperandEqualTo(operand->toGuardShape()->input(), newObject);

    case MDefinition::Opcode::GuardToClass:
      return PhiOperandEqualTo(operand->toGuardToClass()->input(), newObject);

    case MDefinition::Opcode::CheckIsObj:
      return PhiOperandEqualTo(operand->toCheckIsObj()->input(), newObject);

    case MDefinition::Opcode::Unbox:
      return PhiOperandEqualTo(operand->toUnbox()->input(), newObject);

    default:
      return false;
  }
}

// Return true if all phi operands are equal to |newObject|.
static bool PhiOperandsEqualTo(MPhi* phi, MInstruction* newObject) {
  MOZ_ASSERT(IsOptimizableObjectInstruction(newObject));

  for (size_t i = 0, e = phi->numOperands(); i < e; i++) {
    if (!PhiOperandEqualTo(phi->getOperand(i), newObject)) {
      return false;
    }
  }
  return true;
}

static bool IsObjectEscaped(MDefinition* ins, MInstruction* newObject,
                            const Shape* shapeDefault = nullptr);

// Returns False if the lambda is not escaped and if it is optimizable by
// ScalarReplacementOfObject.
static bool IsLambdaEscaped(MInstruction* ins, MInstruction* lambda,
                            MInstruction* newObject, const Shape* shape) {
  MOZ_ASSERT(lambda->isLambda() || lambda->isFunctionWithProto());
  MOZ_ASSERT(IsOptimizableObjectInstruction(newObject));
  JitSpewDef(JitSpew_Escape, "Check lambda\n", ins);
  JitSpewIndent spewIndent(JitSpew_Escape);

  // The scope chain is not escaped if none of the Lambdas which are
  // capturing it are escaped.
  for (MUseIterator i(ins->usesBegin()); i != ins->usesEnd(); i++) {
    MNode* consumer = (*i)->consumer();
    if (!consumer->isDefinition()) {
      // Cannot optimize if it is observable from fun.arguments or others.
      if (!consumer->toResumePoint()->isRecoverableOperand(*i)) {
        JitSpew(JitSpew_Escape, "Observable lambda cannot be recovered");
        return true;
      }
      continue;
    }

    MDefinition* def = consumer->toDefinition();
    switch (def->op()) {
      case MDefinition::Opcode::GuardToFunction: {
        auto* guard = def->toGuardToFunction();
        if (IsLambdaEscaped(guard, lambda, newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::GuardFunctionScript: {
        auto* guard = def->toGuardFunctionScript();
        BaseScript* actual;
        if (lambda->isLambda()) {
          actual = lambda->toLambda()->templateFunction()->baseScript();
        } else {
          actual = lambda->toFunctionWithProto()->function()->baseScript();
        }
        if (actual != guard->expected()) {
          JitSpewDef(JitSpew_Escape, "has a non-matching script guard\n",
                     guard);
          return true;
        }
        if (IsLambdaEscaped(guard, lambda, newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::FunctionEnvironment: {
        if (IsObjectEscaped(def->toFunctionEnvironment(), newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      default:
        JitSpewDef(JitSpew_Escape, "is escaped by\n", def);
        return true;
    }
  }
  JitSpew(JitSpew_Escape, "Lambda is not escaped");
  return false;
}

static bool IsLambdaEscaped(MInstruction* lambda, MInstruction* newObject,
                            const Shape* shape) {
  return IsLambdaEscaped(lambda, lambda, newObject, shape);
}

// Returns False if the object is not escaped and if it is optimizable by
// ScalarReplacementOfObject.
//
// For the moment, this code is dumb as it only supports objects which are not
// changing shape.
static bool IsObjectEscaped(MDefinition* ins, MInstruction* newObject,
                            const Shape* shapeDefault) {
  MOZ_ASSERT(ins->type() == MIRType::Object || ins->isPhi());
  MOZ_ASSERT(IsOptimizableObjectInstruction(newObject));

  JitSpewDef(JitSpew_Escape, "Check object\n", ins);
  JitSpewIndent spewIndent(JitSpew_Escape);

  const Shape* shape = shapeDefault;
  if (!shape) {
    if (ins->isNewPlainObject()) {
      shape = ins->toNewPlainObject()->shape();
    } else if (JSObject* templateObj = MObjectState::templateObjectOf(ins)) {
      shape = templateObj->shape();
    }
  }

  if (!shape) {
    JitSpew(JitSpew_Escape, "No shape defined.");
    return true;
  }

  // Check if the object is escaped. If the object is not the first argument
  // of either a known Store / Load, then we consider it as escaped. This is a
  // cheap and conservative escape analysis.
  for (MUseIterator i(ins->usesBegin()); i != ins->usesEnd(); i++) {
    MNode* consumer = (*i)->consumer();
    if (!consumer->isDefinition()) {
      // Cannot optimize if it is observable from fun.arguments or others.
      if (!consumer->toResumePoint()->isRecoverableOperand(*i)) {
        JitSpew(JitSpew_Escape, "Observable object cannot be recovered");
        return true;
      }
      continue;
    }

    MDefinition* def = consumer->toDefinition();
    switch (def->op()) {
      case MDefinition::Opcode::StoreFixedSlot:
      case MDefinition::Opcode::LoadFixedSlot:
        // Not escaped if it is the first argument.
        if (def->indexOf(*i) == 0) {
          break;
        }

        JitSpewDef(JitSpew_Escape, "is escaped by\n", def);
        return true;

      case MDefinition::Opcode::PostWriteBarrier:
        break;

      case MDefinition::Opcode::Slots: {
#ifdef DEBUG
        // Assert that MSlots are only used by MStoreDynamicSlot and
        // MLoadDynamicSlot.
        MSlots* ins = def->toSlots();
        MOZ_ASSERT(ins->object() != 0);
        for (MUseIterator i(ins->usesBegin()); i != ins->usesEnd(); i++) {
          // toDefinition should normally never fail, since they don't get
          // captured by resume points.
          MDefinition* def = (*i)->consumer()->toDefinition();
          MOZ_ASSERT(def->op() == MDefinition::Opcode::StoreDynamicSlot ||
                     def->op() == MDefinition::Opcode::LoadDynamicSlot);
        }
#endif
        break;
      }

      case MDefinition::Opcode::GuardShape: {
        MGuardShape* guard = def->toGuardShape();
        if (shape != guard->shape()) {
          JitSpewDef(JitSpew_Escape, "has a non-matching guard shape\n", guard);
          return true;
        }
        if (IsObjectEscaped(def->toInstruction(), newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::GuardToClass: {
        MGuardToClass* guard = def->toGuardToClass();
        if (!shape || shape->getObjectClass() != guard->getClass()) {
          JitSpewDef(JitSpew_Escape, "has a non-matching class guard\n", guard);
          return true;
        }
        if (IsObjectEscaped(def->toInstruction(), newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::CheckIsObj: {
        if (IsObjectEscaped(def->toInstruction(), newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::Unbox: {
        if (def->type() != MIRType::Object) {
          JitSpewDef(JitSpew_Escape, "has an invalid unbox\n", def);
          return true;
        }
        if (IsObjectEscaped(def->toInstruction(), newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::Lambda:
      case MDefinition::Opcode::FunctionWithProto: {
        if (IsLambdaEscaped(def->toInstruction(), newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::Phi: {
        auto* phi = def->toPhi();
        if (!PhiOperandsEqualTo(phi, newObject)) {
          JitSpewDef(JitSpew_Escape, "has different phi operands\n", def);
          return true;
        }
        if (IsObjectEscaped(phi, newObject, shape)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::Compare: {
        bool canFold;
        if (!def->toCompare()->tryFold(&canFold)) {
          JitSpewDef(JitSpew_Escape, "has an unsupported compare\n", def);
          return true;
        }
        break;
      }

      // Doesn't escape the object.
      case MDefinition::Opcode::IsObject:
        break;

      // This instruction is a no-op used to verify that scalar replacement
      // is working as expected in jit-test.
      case MDefinition::Opcode::AssertRecoveredOnBailout:
        break;

      // This is just a special flavor of constant which lets us optimize
      // out some guards in certain circumstances. We'll turn this into a
      // regular constant later.
      case MDefinition::Opcode::ConstantProto:
        break;

      // We definitely don't need barriers for objects that don't exist.
      case MDefinition::Opcode::AssertCanElidePostWriteBarrier:
        break;

      default:
        JitSpewDef(JitSpew_Escape, "is escaped by\n", def);
        return true;
    }
  }

  JitSpew(JitSpew_Escape, "Object is not escaped");
  return false;
}

class ObjectMemoryView : public MDefinitionVisitorDefaultNoop {
 public:
  using BlockState = MObjectState;
  static const char phaseName[];

 private:
  TempAllocator& alloc_;
  MConstant* undefinedVal_;
  MInstruction* obj_;
  MBasicBlock* startBlock_;
  BlockState* state_;

  // Used to improve the memory usage by sharing common modification.
  const MResumePoint* lastResumePoint_;

  bool oom_;

 public:
  ObjectMemoryView(TempAllocator& alloc, MInstruction* obj);

  MBasicBlock* startingBlock();
  bool initStartingState(BlockState** pState);

  void setEntryBlockState(BlockState* state);
  bool mergeIntoSuccessorState(MBasicBlock* curr, MBasicBlock* succ,
                               BlockState** pSuccState);

#ifdef DEBUG
  void assertSuccess();
#else
  void assertSuccess() {}
#endif

  bool oom() const { return oom_; }

 private:
  MDefinition* functionForCallObject(MDefinition* ins);

 public:
  void visitResumePoint(MResumePoint* rp);
  void visitObjectState(MObjectState* ins);
  void visitStoreFixedSlot(MStoreFixedSlot* ins);
  void visitLoadFixedSlot(MLoadFixedSlot* ins);
  void visitPostWriteBarrier(MPostWriteBarrier* ins);
  void visitStoreDynamicSlot(MStoreDynamicSlot* ins);
  void visitLoadDynamicSlot(MLoadDynamicSlot* ins);
  void visitGuardShape(MGuardShape* ins);
  void visitGuardToClass(MGuardToClass* ins);
  void visitCheckIsObj(MCheckIsObj* ins);
  void visitUnbox(MUnbox* ins);
  void visitFunctionEnvironment(MFunctionEnvironment* ins);
  void visitGuardToFunction(MGuardToFunction* ins);
  void visitGuardFunctionScript(MGuardFunctionScript* ins);
  void visitLambda(MLambda* ins);
  void visitFunctionWithProto(MFunctionWithProto* ins);
  void visitPhi(MPhi* ins);
  void visitCompare(MCompare* ins);
  void visitConstantProto(MConstantProto* ins);
  void visitIsObject(MIsObject* ins);
  void visitAssertCanElidePostWriteBarrier(
      MAssertCanElidePostWriteBarrier* ins);
};

/* static */ const char ObjectMemoryView::phaseName[] =
    "Scalar Replacement of Object";

ObjectMemoryView::ObjectMemoryView(TempAllocator& alloc, MInstruction* obj)
    : alloc_(alloc),
      undefinedVal_(nullptr),
      obj_(obj),
      startBlock_(obj->block()),
      state_(nullptr),
      lastResumePoint_(nullptr),
      oom_(false) {
  // Annotate snapshots RValue such that we recover the store first.
  obj_->setIncompleteObject();

  // Annotate the instruction such that we do not replace it by a
  // Magic(JS_OPTIMIZED_OUT) in case of removed uses.
  obj_->setImplicitlyUsedUnchecked();
}

MBasicBlock* ObjectMemoryView::startingBlock() { return startBlock_; }

bool ObjectMemoryView::initStartingState(BlockState** pState) {
  // Uninitialized slots have an "undefined" value.
  undefinedVal_ = MConstant::New(alloc_, UndefinedValue());
  startBlock_->insertBefore(obj_, undefinedVal_);

  // Create a new block state and insert at it at the location of the new
  // object.
  BlockState* state = BlockState::New(alloc_, obj_);
  if (!state) {
    return false;
  }

  startBlock_->insertAfter(obj_, state);

  // Initialize the properties of the object state.
  state->initFromTemplateObject(alloc_, undefinedVal_);

  // Hold out of resume point until it is visited.
  state->setInWorklist();

  *pState = state;
  return true;
}

void ObjectMemoryView::setEntryBlockState(BlockState* state) { state_ = state; }

bool ObjectMemoryView::mergeIntoSuccessorState(MBasicBlock* curr,
                                               MBasicBlock* succ,
                                               BlockState** pSuccState) {
  BlockState* succState = *pSuccState;

  // When a block has no state yet, create an empty one for the
  // successor.
  if (!succState) {
    // If the successor is not dominated then the object cannot flow
    // in this basic block without a Phi.  We know that no Phi exist
    // in non-dominated successors as the conservative escaped
    // analysis fails otherwise.  Such condition can succeed if the
    // successor is a join at the end of a if-block and the object
    // only exists within the branch.
    if (!startBlock_->dominates(succ)) {
      return true;
    }

    // If there is only one predecessor, carry over the last state of the
    // block to the successor.  As the block state is immutable, if the
    // current block has multiple successors, they will share the same entry
    // state.
    if (succ->numPredecessors() <= 1 || !state_->numSlots()) {
      *pSuccState = state_;
      return true;
    }

    // If we have multiple predecessors, then we allocate one Phi node for
    // each predecessor, and create a new block state which only has phi
    // nodes.  These would later be removed by the removal of redundant phi
    // nodes.
    succState = BlockState::Copy(alloc_, state_);
    if (!succState) {
      return false;
    }

    size_t numPreds = succ->numPredecessors();
    for (size_t slot = 0; slot < state_->numSlots(); slot++) {
      MPhi* phi = MPhi::New(alloc_.fallible());
      if (!phi || !phi->reserveLength(numPreds)) {
        return false;
      }

      // Fill the input of the successors Phi with undefined
      // values, and each block later fills the Phi inputs.
      for (size_t p = 0; p < numPreds; p++) {
        phi->addInput(undefinedVal_);
      }

      // Add Phi in the list of Phis of the basic block.
      succ->addPhi(phi);
      succState->setSlot(slot, phi);
    }

    // Insert the newly created block state instruction at the beginning
    // of the successor block, after all the phi nodes.  Note that it
    // would be captured by the entry resume point of the successor
    // block.
    succ->insertBefore(succ->safeInsertTop(), succState);
    *pSuccState = succState;
  }

  MOZ_ASSERT_IF(succ == startBlock_, startBlock_->isLoopHeader());
  if (succ->numPredecessors() > 1 && succState->numSlots() &&
      succ != startBlock_) {
    // We need to re-compute successorWithPhis as the previous EliminatePhis
    // phase might have removed all the Phis from the successor block.
    size_t currIndex;
    MOZ_ASSERT(!succ->phisEmpty());
    if (curr->successorWithPhis()) {
      MOZ_ASSERT(curr->successorWithPhis() == succ);
      currIndex = curr->positionInPhiSuccessor();
    } else {
      currIndex = succ->indexForPredecessor(curr);
      curr->setSuccessorWithPhis(succ, currIndex);
    }
    MOZ_ASSERT(succ->getPredecessor(currIndex) == curr);

    // Copy the current slot states to the index of current block in all the
    // Phi created during the first visit of the successor.
    for (size_t slot = 0; slot < state_->numSlots(); slot++) {
      MPhi* phi = succState->getSlot(slot)->toPhi();
      phi->replaceOperand(currIndex, state_->getSlot(slot));
    }
  }

  return true;
}

#ifdef DEBUG
void ObjectMemoryView::assertSuccess() {
  for (MUseIterator i(obj_->usesBegin()); i != obj_->usesEnd(); i++) {
    MNode* ins = (*i)->consumer();
    MDefinition* def = nullptr;

    // Resume points have been replaced by the object state.
    if (ins->isResumePoint() ||
        (def = ins->toDefinition())->isRecoveredOnBailout()) {
      MOZ_ASSERT(obj_->isIncompleteObject());
      continue;
    }

    // The only remaining uses would be removed by DCE, which will also
    // recover the object on bailouts.
    MOZ_ASSERT(def->isSlots() || def->isLambda() || def->isFunctionWithProto());
    MOZ_ASSERT(!def->hasDefUses());
  }
}
#endif

void ObjectMemoryView::visitResumePoint(MResumePoint* rp) {
  // As long as the MObjectState is not yet seen next to the allocation, we do
  // not patch the resume point to recover the side effects.
  if (!state_->isInWorklist()) {
    rp->addStore(alloc_, state_, lastResumePoint_);
    lastResumePoint_ = rp;
  }
}

void ObjectMemoryView::visitObjectState(MObjectState* ins) {
  if (ins->isInWorklist()) {
    ins->setNotInWorklist();
  }
}

void ObjectMemoryView::visitStoreFixedSlot(MStoreFixedSlot* ins) {
  // Skip stores made on other objects.
  if (ins->object() != obj_) {
    return;
  }

  // Clone the state and update the slot value.
  if (state_->hasFixedSlot(ins->slot())) {
    state_ = BlockState::Copy(alloc_, state_);
    if (!state_) {
      oom_ = true;
      return;
    }

    state_->setFixedSlot(ins->slot(), ins->value());
    ins->block()->insertBefore(ins->toInstruction(), state_);
  } else {
    // UnsafeSetReserveSlot can access baked-in slots which are guarded by
    // conditions, which are not seen by the escape analysis.
    MBail* bailout = MBail::New(alloc_, BailoutKind::Inevitable);
    ins->block()->insertBefore(ins, bailout);
  }

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitLoadFixedSlot(MLoadFixedSlot* ins) {
  // Skip loads made on other objects.
  if (ins->object() != obj_) {
    return;
  }

  // Replace load by the slot value.
  if (state_->hasFixedSlot(ins->slot())) {
    ins->replaceAllUsesWith(state_->getFixedSlot(ins->slot()));
  } else {
    // UnsafeGetReserveSlot can access baked-in slots which are guarded by
    // conditions, which are not seen by the escape analysis.
    MBail* bailout = MBail::New(alloc_, BailoutKind::Inevitable);
    ins->block()->insertBefore(ins, bailout);
    ins->replaceAllUsesWith(undefinedVal_);
  }

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitPostWriteBarrier(MPostWriteBarrier* ins) {
  // Skip loads made on other objects.
  if (ins->object() != obj_) {
    return;
  }

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitStoreDynamicSlot(MStoreDynamicSlot* ins) {
  // Skip stores made on other objects.
  MSlots* slots = ins->slots()->toSlots();
  if (slots->object() != obj_) {
    // Guard objects are replaced when they are visited.
    MOZ_ASSERT(!slots->object()->isGuardShape() ||
               slots->object()->toGuardShape()->object() != obj_);
    return;
  }

  // Clone the state and update the slot value.
  if (state_->hasDynamicSlot(ins->slot())) {
    state_ = BlockState::Copy(alloc_, state_);
    if (!state_) {
      oom_ = true;
      return;
    }

    state_->setDynamicSlot(ins->slot(), ins->value());
    ins->block()->insertBefore(ins->toInstruction(), state_);
  } else {
    // UnsafeSetReserveSlot can access baked-in slots which are guarded by
    // conditions, which are not seen by the escape analysis.
    MBail* bailout = MBail::New(alloc_, BailoutKind::Inevitable);
    ins->block()->insertBefore(ins, bailout);
  }

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitLoadDynamicSlot(MLoadDynamicSlot* ins) {
  // Skip loads made on other objects.
  MSlots* slots = ins->slots()->toSlots();
  if (slots->object() != obj_) {
    // Guard objects are replaced when they are visited.
    MOZ_ASSERT(!slots->object()->isGuardShape() ||
               slots->object()->toGuardShape()->object() != obj_);
    return;
  }

  // Replace load by the slot value.
  if (state_->hasDynamicSlot(ins->slot())) {
    ins->replaceAllUsesWith(state_->getDynamicSlot(ins->slot()));
  } else {
    // UnsafeGetReserveSlot can access baked-in slots which are guarded by
    // conditions, which are not seen by the escape analysis.
    MBail* bailout = MBail::New(alloc_, BailoutKind::Inevitable);
    ins->block()->insertBefore(ins, bailout);
    ins->replaceAllUsesWith(undefinedVal_);
  }

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitGuardShape(MGuardShape* ins) {
  // Skip guards on other objects.
  if (ins->object() != obj_) {
    return;
  }

  // Replace the guard by its object.
  ins->replaceAllUsesWith(obj_);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitGuardToClass(MGuardToClass* ins) {
  // Skip guards on other objects.
  if (ins->object() != obj_) {
    return;
  }

  // Replace the guard by its object.
  ins->replaceAllUsesWith(obj_);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitCheckIsObj(MCheckIsObj* ins) {
  // Skip checks on other objects.
  if (ins->input() != obj_) {
    return;
  }

  // Replace the check by its object.
  ins->replaceAllUsesWith(obj_);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitUnbox(MUnbox* ins) {
  // Skip unrelated unboxes.
  if (ins->input() != obj_) {
    return;
  }
  MOZ_ASSERT(ins->type() == MIRType::Object);

  // Replace the unbox with the object.
  ins->replaceAllUsesWith(obj_);

  // Remove the unbox.
  ins->block()->discard(ins);
}

MDefinition* ObjectMemoryView::functionForCallObject(MDefinition* ins) {
  // Return early when we don't replace MNewCallObject.
  if (!obj_->isNewCallObject()) {
    return nullptr;
  }

  // Unwrap instructions until we found either MLambda or MFunctionWithProto.
  // Return the function instruction if their environment chain matches the
  // MNewCallObject we're about to replace.
  while (true) {
    switch (ins->op()) {
      case MDefinition::Opcode::Lambda: {
        if (ins->toLambda()->environmentChain() == obj_) {
          return ins;
        }
        return nullptr;
      }
      case MDefinition::Opcode::FunctionWithProto: {
        if (ins->toFunctionWithProto()->environmentChain() == obj_) {
          return ins;
        }
        return nullptr;
      }
      case MDefinition::Opcode::FunctionEnvironment:
        ins = ins->toFunctionEnvironment()->function();
        break;
      case MDefinition::Opcode::GuardToFunction:
        ins = ins->toGuardToFunction()->object();
        break;
      case MDefinition::Opcode::GuardFunctionScript:
        ins = ins->toGuardFunctionScript()->function();
        break;
      default:
        return nullptr;
    }
  }
}

void ObjectMemoryView::visitFunctionEnvironment(MFunctionEnvironment* ins) {
  // Skip function environment which are not aliases of the NewCallObject.
  if (!functionForCallObject(ins)) {
    return;
  }

  // Replace the function environment by the scope chain of the lambda.
  ins->replaceAllUsesWith(obj_);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitGuardToFunction(MGuardToFunction* ins) {
  // Skip guards on other objects.
  auto* function = functionForCallObject(ins);
  if (!function) {
    return;
  }

  // Replace the guard by its object.
  ins->replaceAllUsesWith(function);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitGuardFunctionScript(MGuardFunctionScript* ins) {
  // Skip guards on other objects.
  auto* function = functionForCallObject(ins);
  if (!function) {
    return;
  }

  // Replace the guard by its object.
  ins->replaceAllUsesWith(function);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitLambda(MLambda* ins) {
  if (ins->environmentChain() != obj_) {
    return;
  }

  // In order to recover the lambda we need to recover the scope chain, as the
  // lambda is holding it.
  ins->setIncompleteObject();
}

void ObjectMemoryView::visitFunctionWithProto(MFunctionWithProto* ins) {
  if (ins->environmentChain() != obj_) {
    return;
  }

  ins->setIncompleteObject();
}

void ObjectMemoryView::visitPhi(MPhi* ins) {
  // Skip phis on other objects.
  if (!PhiOperandsEqualTo(ins, obj_)) {
    return;
  }

  // Replace the phi by its object.
  ins->replaceAllUsesWith(obj_);

  // Remove original instruction.
  ins->block()->discardPhi(ins);
}

void ObjectMemoryView::visitCompare(MCompare* ins) {
  // Skip unrelated comparisons.
  if (ins->lhs() != obj_ && ins->rhs() != obj_) {
    return;
  }

  bool folded;
  MOZ_ALWAYS_TRUE(ins->tryFold(&folded));

  auto* cst = MConstant::New(alloc_, BooleanValue(folded));
  ins->block()->insertBefore(ins, cst);

  // Replace the comparison with a constant.
  ins->replaceAllUsesWith(cst);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitConstantProto(MConstantProto* ins) {
  if (ins->getReceiverObject() != obj_) {
    return;
  }

  auto* cst = ins->protoObject();
  ins->replaceAllUsesWith(cst);
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitIsObject(MIsObject* ins) {
  // Skip unrelated tests.
  if (ins->input() != obj_) {
    return;
  }

  auto* cst = MConstant::New(alloc_, BooleanValue(true));
  ins->block()->insertBefore(ins, cst);

  // Replace the test with a constant.
  ins->replaceAllUsesWith(cst);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ObjectMemoryView::visitAssertCanElidePostWriteBarrier(
    MAssertCanElidePostWriteBarrier* ins) {
  if (ins->object() != obj_) {
    return;
  }

  ins->block()->discard(ins);
}

static bool IndexOf(MDefinition* ins, int32_t* res) {
  MOZ_ASSERT(ins->isLoadElement() || ins->isStoreElement());
  MDefinition* indexDef = ins->getOperand(1);  // ins->index();
  if (indexDef->isSpectreMaskIndex()) {
    indexDef = indexDef->toSpectreMaskIndex()->index();
  }
  if (indexDef->isBoundsCheck()) {
    indexDef = indexDef->toBoundsCheck()->index();
  }
  if (indexDef->isToNumberInt32()) {
    indexDef = indexDef->toToNumberInt32()->getOperand(0);
  }
  MConstant* indexDefConst = indexDef->maybeConstantValue();
  if (!indexDefConst || indexDefConst->type() != MIRType::Int32) {
    return false;
  }
  *res = indexDefConst->toInt32();
  return true;
}

static inline bool IsOptimizableArrayInstruction(MInstruction* ins) {
  return ins->isNewArray() || ins->isNewArrayObject();
}

// We don't support storing holes when doing scalar replacement, so any
// optimizable MNewArrayObject instruction is guaranteed to be packed.
static inline bool IsPackedArray(MInstruction* ins) {
  return ins->isNewArrayObject();
}

// Returns False if the elements is not escaped and if it is optimizable by
// ScalarReplacementOfArray.
static bool IsElementEscaped(MDefinition* def, MInstruction* newArray,
                             uint32_t arraySize) {
  MOZ_ASSERT(def->isElements());
  MOZ_ASSERT(IsOptimizableArrayInstruction(newArray));

  JitSpewDef(JitSpew_Escape, "Check elements\n", def);
  JitSpewIndent spewIndent(JitSpew_Escape);

  for (MUseIterator i(def->usesBegin()); i != def->usesEnd(); i++) {
    // The MIRType::Elements cannot be captured in a resume point as
    // it does not represent a value allocation.
    MDefinition* access = (*i)->consumer()->toDefinition();

    switch (access->op()) {
      case MDefinition::Opcode::LoadElement: {
        MOZ_ASSERT(access->toLoadElement()->elements() == def);

        // If the index is not a constant then this index can alias
        // all others. We do not handle this case.
        int32_t index;
        if (!IndexOf(access, &index)) {
          JitSpewDef(JitSpew_Escape,
                     "has a load element with a non-trivial index\n", access);
          return true;
        }
        if (index < 0 || arraySize <= uint32_t(index)) {
          JitSpewDef(JitSpew_Escape,
                     "has a load element with an out-of-bound index\n", access);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::StoreElement: {
        MStoreElement* storeElem = access->toStoreElement();
        MOZ_ASSERT(storeElem->elements() == def);

        // StoreElement must bail out if it stores to a hole, in case
        // there is a setter on the prototype chain. If this StoreElement
        // might store to a hole, we can't scalar-replace it.
        if (storeElem->needsHoleCheck()) {
          JitSpewDef(JitSpew_Escape, "has a store element with a hole check\n",
                     storeElem);
          return true;
        }

        // If the index is not a constant then this index can alias
        // all others. We do not handle this case.
        int32_t index;
        if (!IndexOf(storeElem, &index)) {
          JitSpewDef(JitSpew_Escape,
                     "has a store element with a non-trivial index\n",
                     storeElem);
          return true;
        }
        if (index < 0 || arraySize <= uint32_t(index)) {
          JitSpewDef(JitSpew_Escape,
                     "has a store element with an out-of-bound index\n",
                     storeElem);
          return true;
        }

        // Dense element holes are written using MStoreHoleValueElement instead
        // of MStoreElement.
        MOZ_ASSERT(storeElem->value()->type() != MIRType::MagicHole);
        break;
      }

      case MDefinition::Opcode::SetInitializedLength:
        MOZ_ASSERT(access->toSetInitializedLength()->elements() == def);
        break;

      case MDefinition::Opcode::InitializedLength:
        MOZ_ASSERT(access->toInitializedLength()->elements() == def);
        break;

      case MDefinition::Opcode::ArrayLength:
        MOZ_ASSERT(access->toArrayLength()->elements() == def);
        break;

      case MDefinition::Opcode::ApplyArray:
        MOZ_ASSERT(access->toApplyArray()->getElements() == def);
        if (!IsPackedArray(newArray)) {
          JitSpewDef(JitSpew_Escape, "is not guaranteed to be packed\n",
                     access);
          return true;
        }
        break;

      case MDefinition::Opcode::ConstructArray:
        MOZ_ASSERT(access->toConstructArray()->getElements() == def);
        if (!IsPackedArray(newArray)) {
          JitSpewDef(JitSpew_Escape, "is not guaranteed to be packed\n",
                     access);
          return true;
        }
        break;

      default:
        JitSpewDef(JitSpew_Escape, "is escaped by\n", access);
        return true;
    }
  }
  JitSpew(JitSpew_Escape, "Elements is not escaped");
  return false;
}

// Returns False if the array is not escaped and if it is optimizable by
// ScalarReplacementOfArray.
//
// For the moment, this code is dumb as it only supports arrays which are not
// changing length, with only access with known constants.
static bool IsArrayEscaped(MInstruction* ins, MInstruction* newArray) {
  MOZ_ASSERT(ins->type() == MIRType::Object);
  MOZ_ASSERT(IsOptimizableArrayInstruction(newArray));

  JitSpewDef(JitSpew_Escape, "Check array\n", ins);
  JitSpewIndent spewIndent(JitSpew_Escape);

  const Shape* shape;
  uint32_t length;
  if (newArray->isNewArrayObject()) {
    length = newArray->toNewArrayObject()->length();
    shape = newArray->toNewArrayObject()->shape();
  } else {
    length = newArray->toNewArray()->length();
    JSObject* templateObject = newArray->toNewArray()->templateObject();
    if (!templateObject) {
      JitSpew(JitSpew_Escape, "No template object defined.");
      return true;
    }
    shape = templateObject->shape();
  }

  if (length >= 16) {
    JitSpew(JitSpew_Escape, "Array has too many elements");
    return true;
  }

  // Check if the object is escaped. If the object is not the first argument
  // of either a known Store / Load, then we consider it as escaped. This is a
  // cheap and conservative escape analysis.
  for (MUseIterator i(ins->usesBegin()); i != ins->usesEnd(); i++) {
    MNode* consumer = (*i)->consumer();
    if (!consumer->isDefinition()) {
      // Cannot optimize if it is observable from fun.arguments or others.
      if (!consumer->toResumePoint()->isRecoverableOperand(*i)) {
        JitSpew(JitSpew_Escape, "Observable array cannot be recovered");
        return true;
      }
      continue;
    }

    MDefinition* def = consumer->toDefinition();
    switch (def->op()) {
      case MDefinition::Opcode::Elements: {
        MElements* elem = def->toElements();
        MOZ_ASSERT(elem->object() == ins);
        if (IsElementEscaped(elem, newArray, length)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", elem);
          return true;
        }

        break;
      }

      case MDefinition::Opcode::GuardShape: {
        MGuardShape* guard = def->toGuardShape();
        if (shape != guard->shape()) {
          JitSpewDef(JitSpew_Escape, "has a non-matching guard shape\n", guard);
          return true;
        }
        if (IsArrayEscaped(guard, newArray)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }

        break;
      }

      case MDefinition::Opcode::GuardToClass: {
        MGuardToClass* guard = def->toGuardToClass();
        if (shape->getObjectClass() != guard->getClass()) {
          JitSpewDef(JitSpew_Escape, "has a non-matching class guard\n", guard);
          return true;
        }
        if (IsArrayEscaped(guard, newArray)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }

        break;
      }

      case MDefinition::Opcode::GuardArrayIsPacked: {
        auto* guard = def->toGuardArrayIsPacked();
        if (!IsPackedArray(newArray)) {
          JitSpewDef(JitSpew_Escape, "is not guaranteed to be packed\n", def);
          return true;
        }
        if (IsArrayEscaped(guard, newArray)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::Unbox: {
        if (def->type() != MIRType::Object) {
          JitSpewDef(JitSpew_Escape, "has an invalid unbox\n", def);
          return true;
        }
        if (IsArrayEscaped(def->toInstruction(), newArray)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      // This instruction is supported for |JSOp::OptimizeSpreadCall|.
      case MDefinition::Opcode::Compare: {
        bool canFold;
        if (!def->toCompare()->tryFold(&canFold)) {
          JitSpewDef(JitSpew_Escape, "has an unsupported compare\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::PostWriteBarrier:
      case MDefinition::Opcode::PostWriteElementBarrier:
        break;

      // This instruction is a no-op used to verify that scalar replacement
      // is working as expected in jit-test.
      case MDefinition::Opcode::AssertRecoveredOnBailout:
        break;

      default:
        JitSpewDef(JitSpew_Escape, "is escaped by\n", def);
        return true;
    }
  }

  JitSpew(JitSpew_Escape, "Array is not escaped");
  return false;
}

// This class replaces every MStoreElement and MSetInitializedLength by an
// MArrayState which emulates the content of the array. All MLoadElement,
// MInitializedLength and MArrayLength are replaced by the corresponding value.
//
// In order to restore the value of the array correctly in case of bailouts, we
// replace all reference of the allocation by the MArrayState definition.
class ArrayMemoryView : public MDefinitionVisitorDefaultNoop {
 public:
  using BlockState = MArrayState;
  static const char* phaseName;

 private:
  TempAllocator& alloc_;
  MConstant* undefinedVal_;
  MConstant* length_;
  MInstruction* arr_;
  MBasicBlock* startBlock_;
  BlockState* state_;

  // Used to improve the memory usage by sharing common modification.
  const MResumePoint* lastResumePoint_;

  bool oom_;

 public:
  ArrayMemoryView(TempAllocator& alloc, MInstruction* arr);

  MBasicBlock* startingBlock();
  bool initStartingState(BlockState** pState);

  void setEntryBlockState(BlockState* state);
  bool mergeIntoSuccessorState(MBasicBlock* curr, MBasicBlock* succ,
                               BlockState** pSuccState);

#ifdef DEBUG
  void assertSuccess();
#else
  void assertSuccess() {}
#endif

  bool oom() const { return oom_; }

 private:
  bool isArrayStateElements(MDefinition* elements);
  void discardInstruction(MInstruction* ins, MDefinition* elements);

 public:
  void visitResumePoint(MResumePoint* rp);
  void visitArrayState(MArrayState* ins);
  void visitStoreElement(MStoreElement* ins);
  void visitLoadElement(MLoadElement* ins);
  void visitSetInitializedLength(MSetInitializedLength* ins);
  void visitInitializedLength(MInitializedLength* ins);
  void visitArrayLength(MArrayLength* ins);
  void visitPostWriteBarrier(MPostWriteBarrier* ins);
  void visitPostWriteElementBarrier(MPostWriteElementBarrier* ins);
  void visitGuardShape(MGuardShape* ins);
  void visitGuardToClass(MGuardToClass* ins);
  void visitGuardArrayIsPacked(MGuardArrayIsPacked* ins);
  void visitUnbox(MUnbox* ins);
  void visitCompare(MCompare* ins);
  void visitApplyArray(MApplyArray* ins);
  void visitConstructArray(MConstructArray* ins);
};

const char* ArrayMemoryView::phaseName = "Scalar Replacement of Array";

ArrayMemoryView::ArrayMemoryView(TempAllocator& alloc, MInstruction* arr)
    : alloc_(alloc),
      undefinedVal_(nullptr),
      length_(nullptr),
      arr_(arr),
      startBlock_(arr->block()),
      state_(nullptr),
      lastResumePoint_(nullptr),
      oom_(false) {
  // Annotate snapshots RValue such that we recover the store first.
  arr_->setIncompleteObject();

  // Annotate the instruction such that we do not replace it by a
  // Magic(JS_OPTIMIZED_OUT) in case of removed uses.
  arr_->setImplicitlyUsedUnchecked();
}

MBasicBlock* ArrayMemoryView::startingBlock() { return startBlock_; }

bool ArrayMemoryView::initStartingState(BlockState** pState) {
  // Uninitialized elements have an "undefined" value.
  undefinedVal_ = MConstant::New(alloc_, UndefinedValue());
  MConstant* initLength = MConstant::New(alloc_, Int32Value(0));
  arr_->block()->insertBefore(arr_, undefinedVal_);
  arr_->block()->insertBefore(arr_, initLength);

  // Create a new block state and insert at it at the location of the new array.
  BlockState* state = BlockState::New(alloc_, arr_, initLength);
  if (!state) {
    return false;
  }

  startBlock_->insertAfter(arr_, state);

  // Initialize the elements of the array state.
  state->initFromTemplateObject(alloc_, undefinedVal_);

  // Hold out of resume point until it is visited.
  state->setInWorklist();

  *pState = state;
  return true;
}

void ArrayMemoryView::setEntryBlockState(BlockState* state) { state_ = state; }

bool ArrayMemoryView::mergeIntoSuccessorState(MBasicBlock* curr,
                                              MBasicBlock* succ,
                                              BlockState** pSuccState) {
  BlockState* succState = *pSuccState;

  // When a block has no state yet, create an empty one for the
  // successor.
  if (!succState) {
    // If the successor is not dominated then the array cannot flow
    // in this basic block without a Phi.  We know that no Phi exist
    // in non-dominated successors as the conservative escaped
    // analysis fails otherwise.  Such condition can succeed if the
    // successor is a join at the end of a if-block and the array
    // only exists within the branch.
    if (!startBlock_->dominates(succ)) {
      return true;
    }

    // If there is only one predecessor, carry over the last state of the
    // block to the successor.  As the block state is immutable, if the
    // current block has multiple successors, they will share the same entry
    // state.
    if (succ->numPredecessors() <= 1 || !state_->numElements()) {
      *pSuccState = state_;
      return true;
    }

    // If we have multiple predecessors, then we allocate one Phi node for
    // each predecessor, and create a new block state which only has phi
    // nodes.  These would later be removed by the removal of redundant phi
    // nodes.
    succState = BlockState::Copy(alloc_, state_);
    if (!succState) {
      return false;
    }

    size_t numPreds = succ->numPredecessors();
    for (size_t index = 0; index < state_->numElements(); index++) {
      MPhi* phi = MPhi::New(alloc_.fallible());
      if (!phi || !phi->reserveLength(numPreds)) {
        return false;
      }

      // Fill the input of the successors Phi with undefined
      // values, and each block later fills the Phi inputs.
      for (size_t p = 0; p < numPreds; p++) {
        phi->addInput(undefinedVal_);
      }

      // Add Phi in the list of Phis of the basic block.
      succ->addPhi(phi);
      succState->setElement(index, phi);
    }

    // Insert the newly created block state instruction at the beginning
    // of the successor block, after all the phi nodes.  Note that it
    // would be captured by the entry resume point of the successor
    // block.
    succ->insertBefore(succ->safeInsertTop(), succState);
    *pSuccState = succState;
  }

  MOZ_ASSERT_IF(succ == startBlock_, startBlock_->isLoopHeader());
  if (succ->numPredecessors() > 1 && succState->numElements() &&
      succ != startBlock_) {
    // We need to re-compute successorWithPhis as the previous EliminatePhis
    // phase might have removed all the Phis from the successor block.
    size_t currIndex;
    MOZ_ASSERT(!succ->phisEmpty());
    if (curr->successorWithPhis()) {
      MOZ_ASSERT(curr->successorWithPhis() == succ);
      currIndex = curr->positionInPhiSuccessor();
    } else {
      currIndex = succ->indexForPredecessor(curr);
      curr->setSuccessorWithPhis(succ, currIndex);
    }
    MOZ_ASSERT(succ->getPredecessor(currIndex) == curr);

    // Copy the current element states to the index of current block in all
    // the Phi created during the first visit of the successor.
    for (size_t index = 0; index < state_->numElements(); index++) {
      MPhi* phi = succState->getElement(index)->toPhi();
      phi->replaceOperand(currIndex, state_->getElement(index));
    }
  }

  return true;
}

#ifdef DEBUG
void ArrayMemoryView::assertSuccess() { MOZ_ASSERT(!arr_->hasLiveDefUses()); }
#endif

void ArrayMemoryView::visitResumePoint(MResumePoint* rp) {
  // As long as the MArrayState is not yet seen next to the allocation, we do
  // not patch the resume point to recover the side effects.
  if (!state_->isInWorklist()) {
    rp->addStore(alloc_, state_, lastResumePoint_);
    lastResumePoint_ = rp;
  }
}

void ArrayMemoryView::visitArrayState(MArrayState* ins) {
  if (ins->isInWorklist()) {
    ins->setNotInWorklist();
  }
}

bool ArrayMemoryView::isArrayStateElements(MDefinition* elements) {
  return elements->isElements() && elements->toElements()->object() == arr_;
}

void ArrayMemoryView::discardInstruction(MInstruction* ins,
                                         MDefinition* elements) {
  MOZ_ASSERT(elements->isElements());
  ins->block()->discard(ins);
  if (!elements->hasLiveDefUses()) {
    elements->block()->discard(elements->toInstruction());
  }
}

void ArrayMemoryView::visitStoreElement(MStoreElement* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->elements();
  if (!isArrayStateElements(elements)) {
    return;
  }

  // Register value of the setter in the state.
  int32_t index;
  MOZ_ALWAYS_TRUE(IndexOf(ins, &index));
  state_ = BlockState::Copy(alloc_, state_);
  if (!state_) {
    oom_ = true;
    return;
  }

  state_->setElement(index, ins->value());
  ins->block()->insertBefore(ins, state_);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

void ArrayMemoryView::visitLoadElement(MLoadElement* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->elements();
  if (!isArrayStateElements(elements)) {
    return;
  }

  // Replace by the value contained at the index.
  int32_t index;
  MOZ_ALWAYS_TRUE(IndexOf(ins, &index));

  // The only way to store a hole value in a new array is with
  // StoreHoleValueElement, which IsElementEscaped does not allow.
  // Therefore, we do not have to do a hole check.
  MDefinition* element = state_->getElement(index);
  MOZ_ASSERT(element->type() != MIRType::MagicHole);

  ins->replaceAllUsesWith(element);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

void ArrayMemoryView::visitSetInitializedLength(MSetInitializedLength* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->elements();
  if (!isArrayStateElements(elements)) {
    return;
  }

  // Replace by the new initialized length.  Note that the argument of
  // MSetInitializedLength is the last index and not the initialized length.
  // To obtain the length, we need to add 1 to it, and thus we need to create
  // a new constant that we register in the ArrayState.
  state_ = BlockState::Copy(alloc_, state_);
  if (!state_) {
    oom_ = true;
    return;
  }

  int32_t initLengthValue = ins->index()->maybeConstantValue()->toInt32() + 1;
  MConstant* initLength = MConstant::New(alloc_, Int32Value(initLengthValue));
  ins->block()->insertBefore(ins, initLength);
  ins->block()->insertBefore(ins, state_);
  state_->setInitializedLength(initLength);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

void ArrayMemoryView::visitInitializedLength(MInitializedLength* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->elements();
  if (!isArrayStateElements(elements)) {
    return;
  }

  // Replace by the value of the length.
  ins->replaceAllUsesWith(state_->initializedLength());

  // Remove original instruction.
  discardInstruction(ins, elements);
}

void ArrayMemoryView::visitArrayLength(MArrayLength* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->elements();
  if (!isArrayStateElements(elements)) {
    return;
  }

  // Replace by the value of the length.
  if (!length_) {
    length_ = MConstant::New(alloc_, Int32Value(state_->numElements()));
    arr_->block()->insertBefore(arr_, length_);
  }
  ins->replaceAllUsesWith(length_);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

void ArrayMemoryView::visitPostWriteBarrier(MPostWriteBarrier* ins) {
  // Skip barriers on other objects.
  if (ins->object() != arr_) {
    return;
  }

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArrayMemoryView::visitPostWriteElementBarrier(
    MPostWriteElementBarrier* ins) {
  // Skip barriers on other objects.
  if (ins->object() != arr_) {
    return;
  }

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArrayMemoryView::visitGuardShape(MGuardShape* ins) {
  // Skip guards on other objects.
  if (ins->object() != arr_) {
    return;
  }

  // Replace the guard by its object.
  ins->replaceAllUsesWith(arr_);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArrayMemoryView::visitGuardToClass(MGuardToClass* ins) {
  // Skip guards on other objects.
  if (ins->object() != arr_) {
    return;
  }

  // Replace the guard by its object.
  ins->replaceAllUsesWith(arr_);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArrayMemoryView::visitGuardArrayIsPacked(MGuardArrayIsPacked* ins) {
  // Skip guards on other objects.
  if (ins->array() != arr_) {
    return;
  }

  // Replace the guard by its object.
  ins->replaceAllUsesWith(arr_);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArrayMemoryView::visitUnbox(MUnbox* ins) {
  // Skip unrelated unboxes.
  if (ins->getOperand(0) != arr_) {
    return;
  }
  MOZ_ASSERT(ins->type() == MIRType::Object);

  // Replace the unbox with the array object.
  ins->replaceAllUsesWith(arr_);

  // Remove the unbox.
  ins->block()->discard(ins);
}

void ArrayMemoryView::visitCompare(MCompare* ins) {
  // Skip unrelated comparisons.
  if (ins->lhs() != arr_ && ins->rhs() != arr_) {
    return;
  }

  bool folded;
  MOZ_ALWAYS_TRUE(ins->tryFold(&folded));

  auto* cst = MConstant::New(alloc_, BooleanValue(folded));
  ins->block()->insertBefore(ins, cst);

  // Replace the comparison with a constant.
  ins->replaceAllUsesWith(cst);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArrayMemoryView::visitApplyArray(MApplyArray* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->getElements();
  if (!isArrayStateElements(elements)) {
    return;
  }

  uint32_t numElements = state_->numElements();

  CallInfo callInfo(alloc_, /*constructing=*/false, ins->ignoresReturnValue());
  if (!callInfo.initForApplyArray(ins->getFunction(), ins->getThis(),
                                  numElements)) {
    oom_ = true;
    return;
  }

  for (uint32_t i = 0; i < numElements; i++) {
    auto* element = state_->getElement(i);
    MOZ_ASSERT(element->type() != MIRType::MagicHole);

    callInfo.initArg(i, element);
  }

  auto addUndefined = [this]() { return undefinedVal_; };

  bool needsThisCheck = false;
  bool isDOMCall = false;
  auto* call = MakeCall(alloc_, addUndefined, callInfo, needsThisCheck,
                        ins->getSingleTarget(), isDOMCall);
  if (!call) {
    oom_ = true;
    return;
  }
  if (!ins->maybeCrossRealm()) {
    call->setNotCrossRealm();
  }

  ins->block()->insertBefore(ins, call);
  ins->replaceAllUsesWith(call);

  call->stealResumePoint(ins);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

void ArrayMemoryView::visitConstructArray(MConstructArray* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->getElements();
  if (!isArrayStateElements(elements)) {
    return;
  }

  uint32_t numElements = state_->numElements();

  CallInfo callInfo(alloc_, /*constructing=*/true, ins->ignoresReturnValue());
  if (!callInfo.initForConstructArray(ins->getFunction(), ins->getThis(),
                                      ins->getNewTarget(), numElements)) {
    oom_ = true;
    return;
  }

  for (uint32_t i = 0; i < numElements; i++) {
    auto* element = state_->getElement(i);
    MOZ_ASSERT(element->type() != MIRType::MagicHole);

    callInfo.initArg(i, element);
  }

  auto addUndefined = [this]() { return undefinedVal_; };

  bool needsThisCheck = ins->needsThisCheck();
  bool isDOMCall = false;
  auto* call = MakeCall(alloc_, addUndefined, callInfo, needsThisCheck,
                        ins->getSingleTarget(), isDOMCall);
  if (!call) {
    oom_ = true;
    return;
  }
  if (!ins->maybeCrossRealm()) {
    call->setNotCrossRealm();
  }

  ins->block()->insertBefore(ins, call);
  ins->replaceAllUsesWith(call);

  call->stealResumePoint(ins);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

static inline bool IsOptimizableArgumentsInstruction(MInstruction* ins) {
  return ins->isCreateArgumentsObject() ||
         ins->isCreateInlinedArgumentsObject();
}

class ArgumentsReplacer : public MDefinitionVisitorDefaultNoop {
 private:
  MIRGenerator* mir_;
  MIRGraph& graph_;
  MInstruction* args_;

  bool oom_ = false;

  TempAllocator& alloc() { return graph_.alloc(); }

  bool isInlinedArguments() const {
    return args_->isCreateInlinedArgumentsObject();
  }

  MNewArrayObject* inlineArgsArray(MInstruction* ins, Shape* shape,
                                   uint32_t begin, uint32_t count);

  void visitGuardToClass(MGuardToClass* ins);
  void visitGuardProto(MGuardProto* ins);
  void visitGuardArgumentsObjectFlags(MGuardArgumentsObjectFlags* ins);
  void visitUnbox(MUnbox* ins);
  void visitGetArgumentsObjectArg(MGetArgumentsObjectArg* ins);
  void visitLoadArgumentsObjectArg(MLoadArgumentsObjectArg* ins);
  void visitLoadArgumentsObjectArgHole(MLoadArgumentsObjectArgHole* ins);
  void visitInArgumentsObjectArg(MInArgumentsObjectArg* ins);
  void visitArgumentsObjectLength(MArgumentsObjectLength* ins);
  void visitApplyArgsObj(MApplyArgsObj* ins);
  void visitArrayFromArgumentsObject(MArrayFromArgumentsObject* ins);
  void visitArgumentsSlice(MArgumentsSlice* ins);
  void visitLoadFixedSlot(MLoadFixedSlot* ins);

  bool oom() const { return oom_; }

 public:
  ArgumentsReplacer(MIRGenerator* mir, MIRGraph& graph, MInstruction* args)
      : mir_(mir), graph_(graph), args_(args) {
    MOZ_ASSERT(IsOptimizableArgumentsInstruction(args_));
  }

  bool escapes(MInstruction* ins, bool guardedForMapped = false);
  bool run();
  void assertSuccess();
};

// Returns false if the arguments object does not escape.
bool ArgumentsReplacer::escapes(MInstruction* ins, bool guardedForMapped) {
  MOZ_ASSERT(ins->type() == MIRType::Object);

  JitSpewDef(JitSpew_Escape, "Check arguments object\n", ins);
  JitSpewIndent spewIndent(JitSpew_Escape);

  // We can replace inlined arguments in scripts with OSR entries, but
  // the outermost arguments object has already been allocated before
  // we enter via OSR and can't be replaced.
  if (ins->isCreateArgumentsObject() && graph_.osrBlock()) {
    JitSpew(JitSpew_Escape, "Can't replace outermost OSR arguments");
    return true;
  }

  // Check all uses to see whether they can be supported without
  // allocating an ArgumentsObject.
  for (MUseIterator i(ins->usesBegin()); i != ins->usesEnd(); i++) {
    MNode* consumer = (*i)->consumer();

    // If a resume point can observe this instruction, we can only optimize
    // if it is recoverable.
    if (consumer->isResumePoint()) {
      if (!consumer->toResumePoint()->isRecoverableOperand(*i)) {
        JitSpew(JitSpew_Escape, "Observable args object cannot be recovered");
        return true;
      }
      continue;
    }

    MDefinition* def = consumer->toDefinition();
    switch (def->op()) {
      case MDefinition::Opcode::GuardToClass: {
        MGuardToClass* guard = def->toGuardToClass();
        if (!guard->isArgumentsObjectClass()) {
          JitSpewDef(JitSpew_Escape, "has a non-matching class guard\n", guard);
          return true;
        }
        bool isMapped = guard->getClass() == &MappedArgumentsObject::class_;
        if (escapes(guard, isMapped)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::GuardProto: {
        if (escapes(def->toInstruction(), guardedForMapped)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::GuardArgumentsObjectFlags: {
        if (escapes(def->toInstruction(), guardedForMapped)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::Unbox: {
        if (def->type() != MIRType::Object) {
          JitSpewDef(JitSpew_Escape, "has an invalid unbox\n", def);
          return true;
        }
        if (escapes(def->toInstruction())) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::LoadFixedSlot: {
        MLoadFixedSlot* load = def->toLoadFixedSlot();

        // We can replace arguments.callee.
        if (load->slot() == ArgumentsObject::CALLEE_SLOT) {
          MOZ_ASSERT(guardedForMapped);
          continue;
        }
        JitSpew(JitSpew_Escape, "is escaped by unsupported LoadFixedSlot\n");
        return true;
      }

      case MDefinition::Opcode::ApplyArgsObj: {
        if (ins == def->toApplyArgsObj()->getThis()) {
          JitSpew(JitSpew_Escape, "is escaped as |this| arg of ApplyArgsObj\n");
          return true;
        }
        MOZ_ASSERT(ins == def->toApplyArgsObj()->getArgsObj());
        break;
      }

      // This is a replaceable consumer.
      case MDefinition::Opcode::ArgumentsObjectLength:
      case MDefinition::Opcode::GetArgumentsObjectArg:
      case MDefinition::Opcode::LoadArgumentsObjectArg:
      case MDefinition::Opcode::LoadArgumentsObjectArgHole:
      case MDefinition::Opcode::InArgumentsObjectArg:
      case MDefinition::Opcode::ArrayFromArgumentsObject:
      case MDefinition::Opcode::ArgumentsSlice:
        break;

      // This instruction is a no-op used to test that scalar replacement
      // is working as expected.
      case MDefinition::Opcode::AssertRecoveredOnBailout:
        break;

      default:
        JitSpewDef(JitSpew_Escape, "is escaped by\n", def);
        return true;
    }
  }

  JitSpew(JitSpew_Escape, "ArgumentsObject is not escaped");
  return false;
}

// Replacing the arguments object is simpler than replacing an object
// or array, because the arguments object does not change state.
bool ArgumentsReplacer::run() {
  MBasicBlock* startBlock = args_->block();

  // Iterate over each basic block.
  for (ReversePostorderIterator block = graph_.rpoBegin(startBlock);
       block != graph_.rpoEnd(); block++) {
    if (mir_->shouldCancel("Scalar replacement of Arguments Object")) {
      return false;
    }

    // Iterates over phis and instructions.
    // We do not have to visit resume points. Any resume points that capture
    // the argument object will be handled by the Sink pass.
    for (MDefinitionIterator iter(*block); iter;) {
      // Increment the iterator before visiting the instruction, as the
      // visit function might discard itself from the basic block.
      MDefinition* def = *iter++;
      switch (def->op()) {
#define MIR_OP(op)              \
  case MDefinition::Opcode::op: \
    visit##op(def->to##op());   \
    break;
        MIR_OPCODE_LIST(MIR_OP)
#undef MIR_OP
      }
      if (!graph_.alloc().ensureBallast()) {
        return false;
      }
      if (oom()) {
        return false;
      }
    }
  }

  assertSuccess();
  return true;
}

void ArgumentsReplacer::assertSuccess() {
  MOZ_ASSERT(args_->canRecoverOnBailout());
  MOZ_ASSERT(!args_->hasLiveDefUses());
}

void ArgumentsReplacer::visitGuardToClass(MGuardToClass* ins) {
  // Skip guards on other objects.
  if (ins->object() != args_) {
    return;
  }
  MOZ_ASSERT(ins->isArgumentsObjectClass());

  // Replace the guard with the args object.
  ins->replaceAllUsesWith(args_);

  // Remove the guard.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitGuardProto(MGuardProto* ins) {
  // Skip guards on other objects.
  if (ins->object() != args_) {
    return;
  }

  // The prototype can only be changed through explicit operations, for example
  // by calling |Reflect.setPrototype|. We have already determined that the args
  // object doesn't escape, so its prototype can't be mutated.

  // Replace the guard with the args object.
  ins->replaceAllUsesWith(args_);

  // Remove the guard.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitGuardArgumentsObjectFlags(
    MGuardArgumentsObjectFlags* ins) {
  // Skip other arguments objects.
  if (ins->argsObject() != args_) {
    return;
  }

#ifdef DEBUG
  // Each *_OVERRIDDEN_BIT can only be set by setting or deleting a
  // property of the args object. We have already determined that the
  // args object doesn't escape, so its properties can't be mutated.
  //
  // FORWARDED_ARGUMENTS_BIT is set if any mapped argument is closed
  // over, which is an immutable property of the script. Because we
  // are replacing the args object for a known script, we can check
  // the flag once, which is done when we first attach the CacheIR,
  // and rely on it.  (Note that this wouldn't be true if we didn't
  // know the origin of args_, because it could be passed in from
  // another function.)
  uint32_t supportedBits = ArgumentsObject::LENGTH_OVERRIDDEN_BIT |
                           ArgumentsObject::ITERATOR_OVERRIDDEN_BIT |
                           ArgumentsObject::ELEMENT_OVERRIDDEN_BIT |
                           ArgumentsObject::CALLEE_OVERRIDDEN_BIT |
                           ArgumentsObject::FORWARDED_ARGUMENTS_BIT;

  MOZ_ASSERT((ins->flags() & ~supportedBits) == 0);
  MOZ_ASSERT_IF(ins->flags() & ArgumentsObject::FORWARDED_ARGUMENTS_BIT,
                !args_->block()->info().anyFormalIsForwarded());
#endif

  // Replace the guard with the args object.
  ins->replaceAllUsesWith(args_);

  // Remove the guard.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitUnbox(MUnbox* ins) {
  // Skip unrelated unboxes.
  if (ins->getOperand(0) != args_) {
    return;
  }
  MOZ_ASSERT(ins->type() == MIRType::Object);

  // Replace the unbox with the args object.
  ins->replaceAllUsesWith(args_);

  // Remove the unbox.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitGetArgumentsObjectArg(
    MGetArgumentsObjectArg* ins) {
  // Skip other arguments objects.
  if (ins->argsObject() != args_) {
    return;
  }

  // We don't support setting arguments in ArgumentsReplacer::escapes,
  // so we can load the initial value of the argument without worrying
  // about it being stale.
  MDefinition* getArg;
  if (isInlinedArguments()) {
    // Inlined frames have direct access to the actual arguments.
    auto* actualArgs = args_->toCreateInlinedArgumentsObject();
    if (ins->argno() < actualArgs->numActuals()) {
      getArg = actualArgs->getArg(ins->argno());
    } else {
      // Omitted arguments are not mapped to the arguments object, and
      // will always be undefined.
      auto* undef = MConstant::New(alloc(), UndefinedValue());
      ins->block()->insertBefore(ins, undef);
      getArg = undef;
    }
  } else {
    // Load the argument from the frame.
    auto* index = MConstant::New(alloc(), Int32Value(ins->argno()));
    ins->block()->insertBefore(ins, index);

    auto* loadArg = MGetFrameArgument::New(alloc(), index);
    ins->block()->insertBefore(ins, loadArg);
    getArg = loadArg;
  }
  ins->replaceAllUsesWith(getArg);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitLoadArgumentsObjectArg(
    MLoadArgumentsObjectArg* ins) {
  // Skip other arguments objects.
  if (ins->argsObject() != args_) {
    return;
  }

  MDefinition* index = ins->index();

  MInstruction* loadArg;
  if (isInlinedArguments()) {
    auto* actualArgs = args_->toCreateInlinedArgumentsObject();

    // Insert bounds check.
    auto* length =
        MConstant::New(alloc(), Int32Value(actualArgs->numActuals()));
    ins->block()->insertBefore(ins, length);

    MInstruction* check = MBoundsCheck::New(alloc(), index, length);
    check->setBailoutKind(ins->bailoutKind());
    ins->block()->insertBefore(ins, check);

    if (mir_->outerInfo().hadBoundsCheckBailout()) {
      check->setNotMovable();
    }

    loadArg = MGetInlinedArgument::New(alloc(), check, actualArgs);
    if (!loadArg) {
      oom_ = true;
      return;
    }
  } else {
    // Insert bounds check.
    auto* length = MArgumentsLength::New(alloc());
    ins->block()->insertBefore(ins, length);

    MInstruction* check = MBoundsCheck::New(alloc(), index, length);
    check->setBailoutKind(ins->bailoutKind());
    ins->block()->insertBefore(ins, check);

    if (mir_->outerInfo().hadBoundsCheckBailout()) {
      check->setNotMovable();
    }

    if (JitOptions.spectreIndexMasking) {
      check = MSpectreMaskIndex::New(alloc(), check, length);
      ins->block()->insertBefore(ins, check);
    }

    loadArg = MGetFrameArgument::New(alloc(), check);
  }
  ins->block()->insertBefore(ins, loadArg);
  ins->replaceAllUsesWith(loadArg);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitLoadArgumentsObjectArgHole(
    MLoadArgumentsObjectArgHole* ins) {
  // Skip other arguments objects.
  if (ins->argsObject() != args_) {
    return;
  }

  MDefinition* index = ins->index();

  MInstruction* loadArg;
  if (isInlinedArguments()) {
    auto* actualArgs = args_->toCreateInlinedArgumentsObject();

    loadArg = MGetInlinedArgumentHole::New(alloc(), index, actualArgs);
    if (!loadArg) {
      oom_ = true;
      return;
    }
  } else {
    auto* length = MArgumentsLength::New(alloc());
    ins->block()->insertBefore(ins, length);

    loadArg = MGetFrameArgumentHole::New(alloc(), index, length);
  }
  loadArg->setBailoutKind(ins->bailoutKind());
  ins->block()->insertBefore(ins, loadArg);
  ins->replaceAllUsesWith(loadArg);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitInArgumentsObjectArg(MInArgumentsObjectArg* ins) {
  // Skip other arguments objects.
  if (ins->argsObject() != args_) {
    return;
  }

  MDefinition* index = ins->index();

  // Ensure the index is non-negative.
  auto* guardedIndex = MGuardInt32IsNonNegative::New(alloc(), index);
  guardedIndex->setBailoutKind(ins->bailoutKind());
  ins->block()->insertBefore(ins, guardedIndex);

  MInstruction* length;
  if (isInlinedArguments()) {
    uint32_t argc = args_->toCreateInlinedArgumentsObject()->numActuals();
    length = MConstant::New(alloc(), Int32Value(argc));
  } else {
    length = MArgumentsLength::New(alloc());
  }
  ins->block()->insertBefore(ins, length);

  auto* compare = MCompare::New(alloc(), guardedIndex, length, JSOp::Lt,
                                MCompare::Compare_Int32);
  ins->block()->insertBefore(ins, compare);
  ins->replaceAllUsesWith(compare);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitArgumentsObjectLength(
    MArgumentsObjectLength* ins) {
  // Skip other arguments objects.
  if (ins->argsObject() != args_) {
    return;
  }

  MInstruction* length;
  if (isInlinedArguments()) {
    uint32_t argc = args_->toCreateInlinedArgumentsObject()->numActuals();
    length = MConstant::New(alloc(), Int32Value(argc));
  } else {
    length = MArgumentsLength::New(alloc());
  }
  ins->block()->insertBefore(ins, length);
  ins->replaceAllUsesWith(length);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitApplyArgsObj(MApplyArgsObj* ins) {
  // Skip other arguments objects.
  if (ins->getArgsObj() != args_) {
    return;
  }

  MInstruction* newIns;
  if (isInlinedArguments()) {
    auto* actualArgs = args_->toCreateInlinedArgumentsObject();
    CallInfo callInfo(alloc(), /*constructing=*/false,
                      ins->ignoresReturnValue());

    callInfo.initForApplyInlinedArgs(ins->getFunction(), ins->getThis(),
                                     actualArgs->numActuals());
    for (uint32_t i = 0; i < actualArgs->numActuals(); i++) {
      callInfo.initArg(i, actualArgs->getArg(i));
    }

    auto addUndefined = [this, &ins]() -> MConstant* {
      MConstant* undef = MConstant::New(alloc(), UndefinedValue());
      ins->block()->insertBefore(ins, undef);
      return undef;
    };

    bool needsThisCheck = false;
    bool isDOMCall = false;
    auto* call = MakeCall(alloc(), addUndefined, callInfo, needsThisCheck,
                          ins->getSingleTarget(), isDOMCall);
    if (!call) {
      oom_ = true;
      return;
    }
    if (!ins->maybeCrossRealm()) {
      call->setNotCrossRealm();
    }
    newIns = call;
  } else {
    auto* numArgs = MArgumentsLength::New(alloc());
    ins->block()->insertBefore(ins, numArgs);

    // TODO: Should we rename MApplyArgs?
    auto* apply = MApplyArgs::New(alloc(), ins->getSingleTarget(),
                                  ins->getFunction(), numArgs, ins->getThis());
    apply->setBailoutKind(ins->bailoutKind());
    if (!ins->maybeCrossRealm()) {
      apply->setNotCrossRealm();
    }
    if (ins->ignoresReturnValue()) {
      apply->setIgnoresReturnValue();
    }
    newIns = apply;
  }

  ins->block()->insertBefore(ins, newIns);
  ins->replaceAllUsesWith(newIns);

  newIns->stealResumePoint(ins);
  ins->block()->discard(ins);
}

MNewArrayObject* ArgumentsReplacer::inlineArgsArray(MInstruction* ins,
                                                    Shape* shape,
                                                    uint32_t begin,
                                                    uint32_t count) {
  auto* actualArgs = args_->toCreateInlinedArgumentsObject();

  // Contrary to |WarpBuilder::build_Rest()|, we can always create
  // MNewArrayObject, because we're guaranteed to have a shape and all
  // arguments can be stored into fixed elements.
  static_assert(
      gc::CanUseFixedElementsForArray(ArgumentsObject::MaxInlinedArgs));

  gc::Heap heap = gc::Heap::Default;

  // Allocate an array of the correct size.
  auto* shapeConstant = MConstant::NewShape(alloc(), shape);
  ins->block()->insertBefore(ins, shapeConstant);

  auto* newArray = MNewArrayObject::New(alloc(), shapeConstant, count, heap);
  ins->block()->insertBefore(ins, newArray);

  if (count) {
    auto* elements = MElements::New(alloc(), newArray);
    ins->block()->insertBefore(ins, elements);

    MConstant* index = nullptr;
    for (uint32_t i = 0; i < count; i++) {
      index = MConstant::New(alloc(), Int32Value(i));
      ins->block()->insertBefore(ins, index);

      MDefinition* arg = actualArgs->getArg(begin + i);
      auto* store = MStoreElement::NewUnbarriered(alloc(), elements, index, arg,
                                                  /* needsHoleCheck = */ false);
      ins->block()->insertBefore(ins, store);

      auto* barrier = MPostWriteBarrier::New(alloc(), newArray, arg);
      ins->block()->insertBefore(ins, barrier);
    }

    auto* initLength = MSetInitializedLength::New(alloc(), elements, index);
    ins->block()->insertBefore(ins, initLength);
  }

  return newArray;
}

void ArgumentsReplacer::visitArrayFromArgumentsObject(
    MArrayFromArgumentsObject* ins) {
  // Skip other arguments objects.
  if (ins->argsObject() != args_) {
    return;
  }

  // We can only replace `arguments` because we've verified that the `arguments`
  // object hasn't been modified in any way. This implies that the arguments
  // stored in the stack frame haven't been changed either.
  //
  // The idea to replace `arguments` in spread calls `f(...arguments)` is now as
  // follows:
  // We replace |MArrayFromArgumentsObject| with the identical instructions we
  // emit when building a rest-array object, cf. |WarpBuilder::build_Rest()|. In
  // a next step, scalar replacement will then replace these new instructions
  // themselves.

  Shape* shape = ins->shape();
  MOZ_ASSERT(shape);

  MDefinition* replacement;
  if (isInlinedArguments()) {
    auto* actualArgs = args_->toCreateInlinedArgumentsObject();
    uint32_t numActuals = actualArgs->numActuals();
    MOZ_ASSERT(numActuals <= ArgumentsObject::MaxInlinedArgs);

    replacement = inlineArgsArray(ins, shape, 0, numActuals);
  } else {
    // We can use |MRest| to read all arguments, because we've guaranteed that
    // the arguments stored in the stack frame haven't changed; see the comment
    // at the start of this method.

    auto* numActuals = MArgumentsLength::New(alloc());
    ins->block()->insertBefore(ins, numActuals);

    // Set |numFormals| to zero to read all arguments, including any formals.
    uint32_t numFormals = 0;

    auto* rest = MRest::New(alloc(), numActuals, numFormals, shape);
    ins->block()->insertBefore(ins, rest);

    replacement = rest;
  }

  ins->replaceAllUsesWith(replacement);

  // Remove original instruction.
  ins->block()->discard(ins);
}

static uint32_t NormalizeSlice(MDefinition* def, uint32_t length) {
  int32_t value = def->toConstant()->toInt32();
  if (value < 0) {
    return std::max(int32_t(uint32_t(value) + length), 0);
  }
  return std::min(uint32_t(value), length);
}

void ArgumentsReplacer::visitArgumentsSlice(MArgumentsSlice* ins) {
  // Skip other arguments objects.
  if (ins->object() != args_) {
    return;
  }

  // Optimise the common pattern |Array.prototype.slice.call(arguments, begin)|,
  // where |begin| is a non-negative, constant int32.
  //
  // An absent end-index is replaced by |arguments.length|, so we try to match
  // |Array.prototype.slice.call(arguments, begin, arguments.length)|.
  if (isInlinedArguments()) {
    // When this is an inlined arguments, |arguments.length| has been replaced
    // by a constant.
    if (ins->begin()->isConstant() && ins->end()->isConstant()) {
      auto* actualArgs = args_->toCreateInlinedArgumentsObject();
      uint32_t numActuals = actualArgs->numActuals();
      MOZ_ASSERT(numActuals <= ArgumentsObject::MaxInlinedArgs);

      uint32_t begin = NormalizeSlice(ins->begin(), numActuals);
      uint32_t end = NormalizeSlice(ins->end(), numActuals);
      uint32_t count = end > begin ? end - begin : 0;
      MOZ_ASSERT(count <= numActuals);

      Shape* shape = ins->templateObj()->shape();
      auto* newArray = inlineArgsArray(ins, shape, begin, count);

      ins->replaceAllUsesWith(newArray);

      // Remove original instruction.
      ins->block()->discard(ins);
      return;
    }
  } else {
    // Otherwise |arguments.length| is emitted as MArgumentsLength.
    if (ins->begin()->isConstant() && ins->end()->isArgumentsLength()) {
      int32_t begin = ins->begin()->toConstant()->toInt32();
      if (begin >= 0) {
        auto* numActuals = MArgumentsLength::New(alloc());
        ins->block()->insertBefore(ins, numActuals);

        // Set |numFormals| to read all arguments starting at |begin|.
        uint32_t numFormals = begin;

        Shape* shape = ins->templateObj()->shape();

        // Use MRest because it can be scalar replaced, which enables further
        // optimizations.
        auto* rest = MRest::New(alloc(), numActuals, numFormals, shape);
        ins->block()->insertBefore(ins, rest);

        ins->replaceAllUsesWith(rest);

        // Remove original instruction.
        ins->block()->discard(ins);
        return;
      }
    }
  }

  MInstruction* numArgs;
  if (isInlinedArguments()) {
    uint32_t argc = args_->toCreateInlinedArgumentsObject()->numActuals();
    numArgs = MConstant::New(alloc(), Int32Value(argc));
  } else {
    numArgs = MArgumentsLength::New(alloc());
  }
  ins->block()->insertBefore(ins, numArgs);

  auto* begin = MNormalizeSliceTerm::New(alloc(), ins->begin(), numArgs);
  ins->block()->insertBefore(ins, begin);

  auto* end = MNormalizeSliceTerm::New(alloc(), ins->end(), numArgs);
  ins->block()->insertBefore(ins, end);

  bool isMax = false;
  auto* beginMin = MMinMax::New(alloc(), begin, end, MIRType::Int32, isMax);
  ins->block()->insertBefore(ins, beginMin);

  // Safe to truncate because both operands are positive and end >= beginMin.
  auto* count = MSub::New(alloc(), end, beginMin, MIRType::Int32);
  count->setTruncateKind(TruncateKind::Truncate);
  ins->block()->insertBefore(ins, count);

  MInstruction* replacement;
  if (isInlinedArguments()) {
    auto* actualArgs = args_->toCreateInlinedArgumentsObject();
    replacement =
        MInlineArgumentsSlice::New(alloc(), beginMin, count, actualArgs,
                                   ins->templateObj(), ins->initialHeap());
    if (!replacement) {
      oom_ = true;
      return;
    }
  } else {
    replacement = MFrameArgumentsSlice::New(
        alloc(), beginMin, count, ins->templateObj(), ins->initialHeap());
  }
  ins->block()->insertBefore(ins, replacement);

  ins->replaceAllUsesWith(replacement);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void ArgumentsReplacer::visitLoadFixedSlot(MLoadFixedSlot* ins) {
  // Skip other arguments objects.
  if (ins->object() != args_) {
    return;
  }

  MOZ_ASSERT(ins->slot() == ArgumentsObject::CALLEE_SLOT);

  MDefinition* replacement;
  if (isInlinedArguments()) {
    replacement = args_->toCreateInlinedArgumentsObject()->getCallee();
  } else {
    auto* callee = MCallee::New(alloc());
    ins->block()->insertBefore(ins, callee);
    replacement = callee;
  }
  ins->replaceAllUsesWith(replacement);

  // Remove original instruction.
  ins->block()->discard(ins);
}

static inline bool IsOptimizableRestInstruction(MInstruction* ins) {
  return ins->isRest();
}

class RestReplacer : public MDefinitionVisitorDefaultNoop {
 private:
  MIRGenerator* mir_;
  MIRGraph& graph_;
  MInstruction* rest_;

  TempAllocator& alloc() { return graph_.alloc(); }
  MRest* rest() const { return rest_->toRest(); }

  bool isRestElements(MDefinition* elements);
  void discardInstruction(MInstruction* ins, MDefinition* elements);
  MDefinition* restLength(MInstruction* ins);
  void visitLength(MInstruction* ins, MDefinition* elements);

  void visitGuardToClass(MGuardToClass* ins);
  void visitGuardShape(MGuardShape* ins);
  void visitGuardArrayIsPacked(MGuardArrayIsPacked* ins);
  void visitUnbox(MUnbox* ins);
  void visitCompare(MCompare* ins);
  void visitLoadElement(MLoadElement* ins);
  void visitArrayLength(MArrayLength* ins);
  void visitInitializedLength(MInitializedLength* ins);
  void visitApplyArray(MApplyArray* ins);
  void visitConstructArray(MConstructArray* ins);

  bool escapes(MElements* ins);

 public:
  RestReplacer(MIRGenerator* mir, MIRGraph& graph, MInstruction* rest)
      : mir_(mir), graph_(graph), rest_(rest) {
    MOZ_ASSERT(IsOptimizableRestInstruction(rest_));
  }

  bool escapes(MInstruction* ins);
  bool run();
  void assertSuccess();
};

// Returns false if the rest array object does not escape.
bool RestReplacer::escapes(MInstruction* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Object);

  JitSpewDef(JitSpew_Escape, "Check rest array\n", ins);
  JitSpewIndent spewIndent(JitSpew_Escape);

  // We can replace rest arrays in scripts with OSR entries, but the outermost
  // rest object has already been allocated before we enter via OSR and can't be
  // replaced.
  // See also the same restriction when replacing |arguments|.
  if (graph_.osrBlock()) {
    JitSpew(JitSpew_Escape, "Can't replace outermost OSR rest array");
    return true;
  }

  // Check all uses to see whether they can be supported without allocating an
  // ArrayObject for the rest parameter.
  for (MUseIterator i(ins->usesBegin()); i != ins->usesEnd(); i++) {
    MNode* consumer = (*i)->consumer();

    // If a resume point can observe this instruction, we can only optimize
    // if it is recoverable.
    if (consumer->isResumePoint()) {
      if (!consumer->toResumePoint()->isRecoverableOperand(*i)) {
        JitSpew(JitSpew_Escape, "Observable rest array cannot be recovered");
        return true;
      }
      continue;
    }

    MDefinition* def = consumer->toDefinition();
    switch (def->op()) {
      case MDefinition::Opcode::Elements: {
        auto* elem = def->toElements();
        MOZ_ASSERT(elem->object() == ins);
        if (escapes(elem)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::GuardShape: {
        const Shape* shape = rest()->shape();
        if (!shape) {
          JitSpew(JitSpew_Escape, "No shape defined.");
          return true;
        }

        auto* guard = def->toGuardShape();
        if (shape != guard->shape()) {
          JitSpewDef(JitSpew_Escape, "has a non-matching guard shape\n", def);
          return true;
        }
        if (escapes(guard)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::GuardToClass: {
        auto* guard = def->toGuardToClass();
        if (guard->getClass() != &ArrayObject::class_) {
          JitSpewDef(JitSpew_Escape, "has a non-matching class guard\n", def);
          return true;
        }
        if (escapes(guard)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::GuardArrayIsPacked: {
        // Rest arrays are always packed as long as they aren't modified.
        auto* guard = def->toGuardArrayIsPacked();
        if (escapes(guard)) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      case MDefinition::Opcode::Unbox: {
        if (def->type() != MIRType::Object) {
          JitSpewDef(JitSpew_Escape, "has an invalid unbox\n", def);
          return true;
        }
        if (escapes(def->toInstruction())) {
          JitSpewDef(JitSpew_Escape, "is indirectly escaped by\n", def);
          return true;
        }
        break;
      }

      // This instruction is supported for |JSOp::OptimizeSpreadCall|.
      case MDefinition::Opcode::Compare: {
        bool canFold;
        if (!def->toCompare()->tryFold(&canFold)) {
          JitSpewDef(JitSpew_Escape, "has an unsupported compare\n", def);
          return true;
        }
        break;
      }

      // This instruction is a no-op used to test that scalar replacement is
      // working as expected.
      case MDefinition::Opcode::AssertRecoveredOnBailout:
        break;

      default:
        JitSpewDef(JitSpew_Escape, "is escaped by\n", def);
        return true;
    }
  }

  JitSpew(JitSpew_Escape, "Rest array object is not escaped");
  return false;
}

bool RestReplacer::escapes(MElements* ins) {
  JitSpewDef(JitSpew_Escape, "Check rest array elements\n", ins);
  JitSpewIndent spewIndent(JitSpew_Escape);

  for (MUseIterator i(ins->usesBegin()); i != ins->usesEnd(); i++) {
    // The MIRType::Elements cannot be captured in a resume point as it does not
    // represent a value allocation.
    MDefinition* def = (*i)->consumer()->toDefinition();

    switch (def->op()) {
      case MDefinition::Opcode::LoadElement:
        MOZ_ASSERT(def->toLoadElement()->elements() == ins);
        break;

      case MDefinition::Opcode::ArrayLength:
        MOZ_ASSERT(def->toArrayLength()->elements() == ins);
        break;

      case MDefinition::Opcode::InitializedLength:
        MOZ_ASSERT(def->toInitializedLength()->elements() == ins);
        break;

      case MDefinition::Opcode::ApplyArray:
        MOZ_ASSERT(def->toApplyArray()->getElements() == ins);
        break;

      case MDefinition::Opcode::ConstructArray:
        MOZ_ASSERT(def->toConstructArray()->getElements() == ins);
        break;

      default:
        JitSpewDef(JitSpew_Escape, "is escaped by\n", def);
        return true;
    }
  }

  JitSpew(JitSpew_Escape, "Rest array object is not escaped");
  return false;
}

// Replacing the rest array object is simpler than replacing an object or array,
// because the rest array object does not change state.
bool RestReplacer::run() {
  MBasicBlock* startBlock = rest_->block();

  // Iterate over each basic block.
  for (ReversePostorderIterator block = graph_.rpoBegin(startBlock);
       block != graph_.rpoEnd(); block++) {
    if (mir_->shouldCancel("Scalar replacement of rest array object")) {
      return false;
    }

    // Iterates over phis and instructions.
    // We do not have to visit resume points. Any resume points that capture the
    // rest array object will be handled by the Sink pass.
    for (MDefinitionIterator iter(*block); iter;) {
      // Increment the iterator before visiting the instruction, as the visit
      // function might discard itself from the basic block.
      MDefinition* def = *iter++;
      switch (def->op()) {
#define MIR_OP(op)              \
  case MDefinition::Opcode::op: \
    visit##op(def->to##op());   \
    break;
        MIR_OPCODE_LIST(MIR_OP)
#undef MIR_OP
      }
      if (!graph_.alloc().ensureBallast()) {
        return false;
      }
    }
  }

  assertSuccess();
  return true;
}

void RestReplacer::assertSuccess() {
  MOZ_ASSERT(rest_->canRecoverOnBailout());
  MOZ_ASSERT(!rest_->hasLiveDefUses());
}

bool RestReplacer::isRestElements(MDefinition* elements) {
  return elements->isElements() && elements->toElements()->object() == rest_;
}

void RestReplacer::discardInstruction(MInstruction* ins,
                                      MDefinition* elements) {
  MOZ_ASSERT(elements->isElements());
  ins->block()->discard(ins);
  if (!elements->hasLiveDefUses()) {
    elements->block()->discard(elements->toInstruction());
  }
}

void RestReplacer::visitGuardToClass(MGuardToClass* ins) {
  // Skip guards on other objects.
  if (ins->object() != rest_) {
    return;
  }
  MOZ_ASSERT(ins->getClass() == &ArrayObject::class_);

  // Replace the guard with the array object.
  ins->replaceAllUsesWith(rest_);

  // Remove the guard.
  ins->block()->discard(ins);
}

void RestReplacer::visitGuardShape(MGuardShape* ins) {
  // Skip guards on other objects.
  if (ins->object() != rest_) {
    return;
  }

  // Replace the guard with the array object.
  ins->replaceAllUsesWith(rest_);

  // Remove the guard.
  ins->block()->discard(ins);
}

void RestReplacer::visitGuardArrayIsPacked(MGuardArrayIsPacked* ins) {
  // Skip guards on other objects.
  if (ins->array() != rest_) {
    return;
  }

  // Replace the guard by its object.
  ins->replaceAllUsesWith(rest_);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void RestReplacer::visitUnbox(MUnbox* ins) {
  // Skip unrelated unboxes.
  if (ins->input() != rest_) {
    return;
  }
  MOZ_ASSERT(ins->type() == MIRType::Object);

  // Replace the unbox with the array object.
  ins->replaceAllUsesWith(rest_);

  // Remove the unbox.
  ins->block()->discard(ins);
}

void RestReplacer::visitCompare(MCompare* ins) {
  // Skip unrelated comparisons.
  if (ins->lhs() != rest_ && ins->rhs() != rest_) {
    return;
  }

  bool folded;
  MOZ_ALWAYS_TRUE(ins->tryFold(&folded));

  auto* cst = MConstant::New(alloc(), BooleanValue(folded));
  ins->block()->insertBefore(ins, cst);

  // Replace the comparison with a constant.
  ins->replaceAllUsesWith(cst);

  // Remove original instruction.
  ins->block()->discard(ins);
}

void RestReplacer::visitLoadElement(MLoadElement* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->elements();
  if (!isRestElements(elements)) {
    return;
  }

  MDefinition* index = ins->index();

  // Adjust the index to skip any extra formals.
  if (uint32_t formals = rest()->numFormals()) {
    auto* numFormals = MConstant::New(alloc(), Int32Value(formals));
    ins->block()->insertBefore(ins, numFormals);

    auto* add = MAdd::New(alloc(), index, numFormals, TruncateKind::Truncate);
    ins->block()->insertBefore(ins, add);

    index = add;
  }

  auto* loadArg = MGetFrameArgument::New(alloc(), index);

  ins->block()->insertBefore(ins, loadArg);
  ins->replaceAllUsesWith(loadArg);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

MDefinition* RestReplacer::restLength(MInstruction* ins) {
  // Compute |Math.max(numActuals - numFormals, 0)| for the rest array length.

  auto* numActuals = rest()->numActuals();

  if (uint32_t formals = rest()->numFormals()) {
    auto* numFormals = MConstant::New(alloc(), Int32Value(formals));
    ins->block()->insertBefore(ins, numFormals);

    auto* length = MSub::New(alloc(), numActuals, numFormals, MIRType::Int32);
    length->setTruncateKind(TruncateKind::Truncate);
    ins->block()->insertBefore(ins, length);

    auto* zero = MConstant::New(alloc(), Int32Value(0));
    ins->block()->insertBefore(ins, zero);

    bool isMax = true;
    auto* minmax = MMinMax::New(alloc(), length, zero, MIRType::Int32, isMax);
    ins->block()->insertBefore(ins, minmax);

    return minmax;
  }

  return numActuals;
}

void RestReplacer::visitLength(MInstruction* ins, MDefinition* elements) {
  MOZ_ASSERT(ins->isArrayLength() || ins->isInitializedLength());

  // Skip other array objects.
  if (!isRestElements(elements)) {
    return;
  }

  MDefinition* replacement = restLength(ins);

  ins->replaceAllUsesWith(replacement);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

void RestReplacer::visitArrayLength(MArrayLength* ins) {
  visitLength(ins, ins->elements());
}

void RestReplacer::visitInitializedLength(MInitializedLength* ins) {
  // The initialized length of a rest array is equal to its length.
  visitLength(ins, ins->elements());
}

void RestReplacer::visitApplyArray(MApplyArray* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->getElements();
  if (!isRestElements(elements)) {
    return;
  }

  auto* numActuals = restLength(ins);

  auto* apply =
      MApplyArgs::New(alloc(), ins->getSingleTarget(), ins->getFunction(),
                      numActuals, ins->getThis(), rest()->numFormals());
  apply->setBailoutKind(ins->bailoutKind());
  if (!ins->maybeCrossRealm()) {
    apply->setNotCrossRealm();
  }
  if (ins->ignoresReturnValue()) {
    apply->setIgnoresReturnValue();
  }
  ins->block()->insertBefore(ins, apply);

  ins->replaceAllUsesWith(apply);

  apply->stealResumePoint(ins);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

void RestReplacer::visitConstructArray(MConstructArray* ins) {
  // Skip other array objects.
  MDefinition* elements = ins->getElements();
  if (!isRestElements(elements)) {
    return;
  }

  auto* numActuals = restLength(ins);

  auto* construct = MConstructArgs::New(
      alloc(), ins->getSingleTarget(), ins->getFunction(), numActuals,
      ins->getThis(), ins->getNewTarget(), rest()->numFormals());
  construct->setBailoutKind(ins->bailoutKind());
  if (!ins->maybeCrossRealm()) {
    construct->setNotCrossRealm();
  }

  ins->block()->insertBefore(ins, construct);
  ins->replaceAllUsesWith(construct);

  construct->stealResumePoint(ins);

  // Remove original instruction.
  discardInstruction(ins, elements);
}

bool ScalarReplacement(MIRGenerator* mir, MIRGraph& graph) {
  JitSpew(JitSpew_Escape, "Begin (ScalarReplacement)");

  EmulateStateOf<ObjectMemoryView> replaceObject(mir, graph);
  EmulateStateOf<ArrayMemoryView> replaceArray(mir, graph);
  bool addedPhi = false;

  for (ReversePostorderIterator block = graph.rpoBegin();
       block != graph.rpoEnd(); block++) {
    if (mir->shouldCancel("Scalar Replacement (main loop)")) {
      return false;
    }

    for (MInstructionIterator ins = block->begin(); ins != block->end();
         ins++) {
      if (IsOptimizableObjectInstruction(*ins) &&
          !IsObjectEscaped(*ins, *ins)) {
        ObjectMemoryView view(graph.alloc(), *ins);
        if (!replaceObject.run(view)) {
          return false;
        }
        view.assertSuccess();
        addedPhi = true;
        continue;
      }

      if (IsOptimizableArrayInstruction(*ins) && !IsArrayEscaped(*ins, *ins)) {
        ArrayMemoryView view(graph.alloc(), *ins);
        if (!replaceArray.run(view)) {
          return false;
        }
        view.assertSuccess();
        addedPhi = true;
        continue;
      }

      if (IsOptimizableArgumentsInstruction(*ins)) {
        ArgumentsReplacer replacer(mir, graph, *ins);
        if (replacer.escapes(*ins)) {
          continue;
        }
        if (!replacer.run()) {
          return false;
        }
        continue;
      }

      if (IsOptimizableRestInstruction(*ins)) {
        RestReplacer replacer(mir, graph, *ins);
        if (replacer.escapes(*ins)) {
          continue;
        }
        if (!replacer.run()) {
          return false;
        }
        continue;
      }
    }
  }

  if (addedPhi) {
    // Phis added by Scalar Replacement are only redundant Phis which are
    // not directly captured by any resume point but only by the MDefinition
    // state. The conservative observability only focuses on Phis which are
    // not used as resume points operands.
    AssertExtendedGraphCoherency(graph);
    if (!EliminatePhis(mir, graph, ConservativeObservability)) {
      return false;
    }
  }

  return true;
}

} /* namespace jit */
} /* namespace js */
