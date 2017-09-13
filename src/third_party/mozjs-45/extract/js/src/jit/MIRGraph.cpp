/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MIRGraph.h"

#include "asmjs/AsmJSValidate.h"
#include "jit/BytecodeAnalysis.h"
#include "jit/Ion.h"
#include "jit/JitSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"

using namespace js;
using namespace js::jit;
using mozilla::Swap;

MIRGenerator::MIRGenerator(CompileCompartment* compartment, const JitCompileOptions& options,
                           TempAllocator* alloc, MIRGraph* graph, const CompileInfo* info,
                           const OptimizationInfo* optimizationInfo,
                           bool usesSignalHandlersForAsmJSOOB)
  : compartment(compartment),
    info_(info),
    optimizationInfo_(optimizationInfo),
    alloc_(alloc),
    graph_(graph),
    abortReason_(AbortReason_NoAbort),
    shouldForceAbort_(false),
    abortedPreliminaryGroups_(*alloc_),
    error_(false),
    pauseBuild_(nullptr),
    cancelBuild_(false),
    maxAsmJSStackArgBytes_(0),
    performsCall_(false),
    usesSimd_(false),
    usesSimdCached_(false),
    modifiesFrameArguments_(false),
    instrumentedProfiling_(false),
    instrumentedProfilingIsCached_(false),
    safeForMinorGC_(true),
#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
    usesSignalHandlersForAsmJSOOB_(usesSignalHandlersForAsmJSOOB),
#endif
    options(options),
    gs_(alloc)
{ }

bool
MIRGenerator::usesSimd()
{
    if (usesSimdCached_)
        return usesSimd_;

    usesSimdCached_ = true;
    for (ReversePostorderIterator block = graph_->rpoBegin(),
                                  end   = graph_->rpoEnd();
         block != end;
         block++)
    {
        // It's fine to use MInstructionIterator here because we don't have to
        // worry about Phis, since any reachable phi (or phi cycle) will have at
        // least one instruction as an input.
        for (MInstructionIterator inst = block->begin(); inst != block->end(); inst++) {
            // Instructions that have SIMD inputs but not a SIMD type are fine
            // to ignore, as their inputs are also reached at some point. By
            // induction, at least one instruction with a SIMD type is reached
            // at some point.
            if (IsSimdType(inst->type())) {
                MOZ_ASSERT(SupportsSimd);
                usesSimd_ = true;
                return true;
            }
        }
    }
    usesSimd_ = false;
    return false;
}

bool
MIRGenerator::abortFmt(const char* message, va_list ap)
{
    JitSpewVA(JitSpew_IonAbort, message, ap);
    error_ = true;
    return false;
}

bool
MIRGenerator::abort(const char* message, ...)
{
    va_list ap;
    va_start(ap, message);
    abortFmt(message, ap);
    va_end(ap);
    return false;
}

void
MIRGenerator::addAbortedPreliminaryGroup(ObjectGroup* group)
{
    for (size_t i = 0; i < abortedPreliminaryGroups_.length(); i++) {
        if (group == abortedPreliminaryGroups_[i])
            return;
    }
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!abortedPreliminaryGroups_.append(group))
        oomUnsafe.crash("addAbortedPreliminaryGroup");
}

bool
MIRGenerator::needsAsmJSBoundsCheckBranch(const MAsmJSHeapAccess* access) const
{
    // A heap access needs a bounds-check branch if we're not relying on signal
    // handlers to catch errors, and if it's not proven to be within bounds.
    // We use signal-handlers on x64, but on x86 there isn't enough address
    // space for a guard region.  Also, on x64 the atomic loads and stores
    // can't (yet) use the signal handlers.
#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
    if (usesSignalHandlersForAsmJSOOB_ && !access->isAtomicAccess())
        return false;
#endif
    return access->needsBoundsCheck();
}

size_t
MIRGenerator::foldableOffsetRange(const MAsmJSHeapAccess* access) const
{
    // This determines whether it's ok to fold up to AsmJSImmediateSize
    // offsets, instead of just AsmJSCheckedImmediateSize.

#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
    // With signal-handler OOB handling, we reserve guard space for the full
    // immediate size.
    if (usesSignalHandlersForAsmJSOOB_)
        return AsmJSImmediateRange;
#endif

    // On 32-bit platforms, if we've proven the access is in bounds after
    // 32-bit wrapping, we can fold full offsets because they're added with
    // 32-bit arithmetic.
    if (sizeof(intptr_t) == sizeof(int32_t) && !access->needsBoundsCheck())
        return AsmJSImmediateRange;

    // Otherwise, only allow the checked size. This is always less than the
    // minimum heap length, and allows explicit bounds checks to fold in the
    // offset without overflow.
    return AsmJSCheckedImmediateRange;
}

void
MIRGraph::addBlock(MBasicBlock* block)
{
    MOZ_ASSERT(block);
    block->setId(blockIdGen_++);
    blocks_.pushBack(block);
    numBlocks_++;
}

void
MIRGraph::insertBlockAfter(MBasicBlock* at, MBasicBlock* block)
{
    block->setId(blockIdGen_++);
    blocks_.insertAfter(at, block);
    numBlocks_++;
}

void
MIRGraph::insertBlockBefore(MBasicBlock* at, MBasicBlock* block)
{
    block->setId(blockIdGen_++);
    blocks_.insertBefore(at, block);
    numBlocks_++;
}

void
MIRGraph::renumberBlocksAfter(MBasicBlock* at)
{
    MBasicBlockIterator iter = begin(at);
    iter++;

    uint32_t id = at->id();
    for (; iter != end(); iter++)
        iter->setId(++id);
}

void
MIRGraph::removeBlocksAfter(MBasicBlock* start)
{
    MBasicBlockIterator iter(begin());
    iter++;
    while (iter != end()) {
        MBasicBlock* block = *iter;
        iter++;

        if (block->id() <= start->id())
            continue;

        removeBlock(block);
    }
}

void
MIRGraph::removeBlock(MBasicBlock* block)
{
    // Remove a block from the graph. It will also cleanup the block.

    if (block == osrBlock_)
        osrBlock_ = nullptr;

    if (returnAccumulator_) {
        size_t i = 0;
        while (i < returnAccumulator_->length()) {
            if ((*returnAccumulator_)[i] == block)
                returnAccumulator_->erase(returnAccumulator_->begin() + i);
            else
                i++;
        }
    }

    block->discardAllInstructions();
    block->discardAllResumePoints();

    // Note: phis are disconnected from the rest of the graph, but are not
    // removed entirely. If the block being removed is a loop header then
    // IonBuilder may need to access these phis to more quickly converge on the
    // possible types in the graph. See IonBuilder::analyzeNewLoopTypes.
    block->discardAllPhiOperands();

    block->markAsDead();
    blocks_.remove(block);
    numBlocks_--;
}

void
MIRGraph::removeBlockIncludingPhis(MBasicBlock* block)
{
    // removeBlock doesn't clear phis because of IonBuilder constraints. Here,
    // we want to totally clear everything.
    removeBlock(block);
    block->discardAllPhis();
}

void
MIRGraph::unmarkBlocks()
{
    for (MBasicBlockIterator i(blocks_.begin()); i != blocks_.end(); i++)
        i->unmark();
}

MBasicBlock*
MBasicBlock::New(MIRGraph& graph, BytecodeAnalysis* analysis, const CompileInfo& info,
                 MBasicBlock* pred, BytecodeSite* site, Kind kind)
{
    MOZ_ASSERT(site->pc() != nullptr);

    MBasicBlock* block = new(graph.alloc()) MBasicBlock(graph, info, site, kind);
    if (!block->init())
        return nullptr;

    if (!block->inherit(graph.alloc(), analysis, pred, 0))
        return nullptr;

    return block;
}

MBasicBlock*
MBasicBlock::NewPopN(MIRGraph& graph, const CompileInfo& info,
                     MBasicBlock* pred, BytecodeSite* site, Kind kind, uint32_t popped)
{
    MBasicBlock* block = new(graph.alloc()) MBasicBlock(graph, info, site, kind);
    if (!block->init())
        return nullptr;

    if (!block->inherit(graph.alloc(), nullptr, pred, popped))
        return nullptr;

    return block;
}

MBasicBlock*
MBasicBlock::NewWithResumePoint(MIRGraph& graph, const CompileInfo& info,
                                MBasicBlock* pred, BytecodeSite* site,
                                MResumePoint* resumePoint)
{
    MBasicBlock* block = new(graph.alloc()) MBasicBlock(graph, info, site, NORMAL);

    MOZ_ASSERT(!resumePoint->instruction());
    resumePoint->block()->discardResumePoint(resumePoint, RefType_None);
    resumePoint->block_ = block;
    block->addResumePoint(resumePoint);
    block->entryResumePoint_ = resumePoint;

    if (!block->init())
        return nullptr;

    if (!block->inheritResumePoint(pred))
        return nullptr;

    return block;
}

MBasicBlock*
MBasicBlock::NewPendingLoopHeader(MIRGraph& graph, const CompileInfo& info,
                                  MBasicBlock* pred, BytecodeSite* site,
                                  unsigned stackPhiCount)
{
    MOZ_ASSERT(site->pc() != nullptr);

    MBasicBlock* block = new(graph.alloc()) MBasicBlock(graph, info, site, PENDING_LOOP_HEADER);
    if (!block->init())
        return nullptr;

    if (!block->inherit(graph.alloc(), nullptr, pred, 0, stackPhiCount))
        return nullptr;

    return block;
}

MBasicBlock*
MBasicBlock::NewSplitEdge(MIRGraph& graph, const CompileInfo& info, MBasicBlock* pred)
{
    return pred->pc()
           ? MBasicBlock::New(graph, nullptr, info, pred,
                              new(graph.alloc()) BytecodeSite(pred->trackedTree(), pred->pc()),
                              SPLIT_EDGE)
           : MBasicBlock::NewAsmJS(graph, info, pred, SPLIT_EDGE);
}

MBasicBlock*
MBasicBlock::NewAsmJS(MIRGraph& graph, const CompileInfo& info, MBasicBlock* pred, Kind kind)
{
    BytecodeSite* site = new(graph.alloc()) BytecodeSite();
    MBasicBlock* block = new(graph.alloc()) MBasicBlock(graph, info, site, kind);
    if (!block->init())
        return nullptr;

    if (pred) {
        block->stackPosition_ = pred->stackPosition_;

        if (block->kind_ == PENDING_LOOP_HEADER) {
            size_t nphis = block->stackPosition_;

            size_t nfree = graph.phiFreeListLength();

            TempAllocator& alloc = graph.alloc();
            MPhi* phis = nullptr;
            if (nphis > nfree) {
                phis = alloc.allocateArray<MPhi>(nphis - nfree);
                if (!phis)
                    return nullptr;
            }

            // Note: Phis are inserted in the same order as the slots.
            for (size_t i = 0; i < nphis; i++) {
                MDefinition* predSlot = pred->getSlot(i);

                MOZ_ASSERT(predSlot->type() != MIRType_Value);

                MPhi* phi;
                if (i < nfree)
                    phi = graph.takePhiFromFreeList();
                else
                    phi = phis + (i - nfree);
                new(phi) MPhi(alloc, predSlot->type());

                phi->addInput(predSlot);

                // Add append Phis in the block.
                block->addPhi(phi);
                block->setSlot(i, phi);
            }
        } else {
            block->copySlots(pred);
        }

        if (!block->predecessors_.append(pred))
            return nullptr;
    }

    return block;
}

MBasicBlock::MBasicBlock(MIRGraph& graph, const CompileInfo& info, BytecodeSite* site, Kind kind)
  : unreachable_(false),
    graph_(graph),
    info_(info),
    predecessors_(graph.alloc()),
    stackPosition_(info_.firstStackSlot()),
    numDominated_(0),
    pc_(site->pc()),
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
    trackedSite_(site),
    hitCount_(0),
    hitState_(HitState::NotDefined)
#if defined (JS_ION_PERF)
    , lineno_(0u),
    columnIndex_(0u)
#endif
{
}

bool
MBasicBlock::init()
{
    return slots_.init(graph_.alloc(), info_.nslots());
}

bool
MBasicBlock::increaseSlots(size_t num)
{
    return slots_.growBy(graph_.alloc(), num);
}

bool
MBasicBlock::ensureHasSlots(size_t num)
{
    size_t depth = stackDepth() + num;
    if (depth > nslots()) {
        if (!increaseSlots(depth - nslots()))
            return false;
    }
    return true;
}

void
MBasicBlock::copySlots(MBasicBlock* from)
{
    MOZ_ASSERT(stackPosition_ <= from->stackPosition_);

    MDefinition** thisSlots = slots_.begin();
    MDefinition** fromSlots = from->slots_.begin();
    for (size_t i = 0, e = stackPosition_; i < e; ++i)
        thisSlots[i] = fromSlots[i];
}

bool
MBasicBlock::inherit(TempAllocator& alloc, BytecodeAnalysis* analysis, MBasicBlock* pred,
                     uint32_t popped, unsigned stackPhiCount)
{
    if (pred) {
        stackPosition_ = pred->stackPosition_;
        MOZ_ASSERT(stackPosition_ >= popped);
        stackPosition_ -= popped;
        if (kind_ != PENDING_LOOP_HEADER)
            copySlots(pred);
    } else {
        uint32_t stackDepth = analysis->info(pc()).stackDepth;
        stackPosition_ = info().firstStackSlot() + stackDepth;
        MOZ_ASSERT(stackPosition_ >= popped);
        stackPosition_ -= popped;
    }

    MOZ_ASSERT(info_.nslots() >= stackPosition_);
    MOZ_ASSERT(!entryResumePoint_);

    // Propagate the caller resume point from the inherited block.
    callerResumePoint_ = pred ? pred->callerResumePoint() : nullptr;

    // Create a resume point using our initial stack state.
    entryResumePoint_ = new(alloc) MResumePoint(this, pc(), MResumePoint::ResumeAt);
    if (!entryResumePoint_->init(alloc))
        return false;

    if (pred) {
        if (!predecessors_.append(pred))
            return false;

        if (kind_ == PENDING_LOOP_HEADER) {
            size_t i = 0;
            for (i = 0; i < info().firstStackSlot(); i++) {
                MPhi* phi = MPhi::New(alloc);
                phi->addInput(pred->getSlot(i));
                addPhi(phi);
                setSlot(i, phi);
                entryResumePoint()->initOperand(i, phi);
            }

            MOZ_ASSERT(stackPhiCount <= stackDepth());
            MOZ_ASSERT(info().firstStackSlot() <= stackDepth() - stackPhiCount);

            // Avoid creating new phis for stack values that aren't part of the
            // loop.  Note that for loop headers that can OSR, all values on the
            // stack are part of the loop.
            for (; i < stackDepth() - stackPhiCount; i++) {
                MDefinition* val = pred->getSlot(i);
                setSlot(i, val);
                entryResumePoint()->initOperand(i, val);
            }

            for (; i < stackDepth(); i++) {
                MPhi* phi = MPhi::New(alloc);
                phi->addInput(pred->getSlot(i));
                addPhi(phi);
                setSlot(i, phi);
                entryResumePoint()->initOperand(i, phi);
            }
        } else {
            for (size_t i = 0; i < stackDepth(); i++)
                entryResumePoint()->initOperand(i, getSlot(i));
        }
    } else {
        /*
         * Don't leave the operands uninitialized for the caller, as it may not
         * initialize them later on.
         */
        for (size_t i = 0; i < stackDepth(); i++)
            entryResumePoint()->clearOperand(i);
    }

    return true;
}

bool
MBasicBlock::inheritResumePoint(MBasicBlock* pred)
{
    // Copy slots from the resume point.
    stackPosition_ = entryResumePoint_->stackDepth();
    for (uint32_t i = 0; i < stackPosition_; i++)
        slots_[i] = entryResumePoint_->getOperand(i);

    MOZ_ASSERT(info_.nslots() >= stackPosition_);
    MOZ_ASSERT(kind_ != PENDING_LOOP_HEADER);
    MOZ_ASSERT(pred != nullptr);

    callerResumePoint_ = pred->callerResumePoint();

    if (!predecessors_.append(pred))
        return false;

    return true;
}

void
MBasicBlock::inheritSlots(MBasicBlock* parent)
{
    stackPosition_ = parent->stackPosition_;
    copySlots(parent);
}

bool
MBasicBlock::initEntrySlots(TempAllocator& alloc)
{
    // Remove the previous resume point.
    discardResumePoint(entryResumePoint_);

    // Create a resume point using our initial stack state.
    entryResumePoint_ = MResumePoint::New(alloc, this, pc(), MResumePoint::ResumeAt);
    if (!entryResumePoint_)
        return false;
    return true;
}

MDefinition*
MBasicBlock::getSlot(uint32_t index)
{
    MOZ_ASSERT(index < stackPosition_);
    return slots_[index];
}

void
MBasicBlock::initSlot(uint32_t slot, MDefinition* ins)
{
    slots_[slot] = ins;
    if (entryResumePoint())
        entryResumePoint()->initOperand(slot, ins);
}

void
MBasicBlock::shimmySlots(int discardDepth)
{
    // Move all slots above the given depth down by one,
    // overwriting the MDefinition at discardDepth.

    MOZ_ASSERT(discardDepth < 0);
    MOZ_ASSERT(stackPosition_ + discardDepth >= info_.firstStackSlot());

    for (int i = discardDepth; i < -1; i++)
        slots_[stackPosition_ + i] = slots_[stackPosition_ + i + 1];

    --stackPosition_;
}

bool
MBasicBlock::linkOsrValues(MStart* start)
{
    MOZ_ASSERT(start->startType() == MStart::StartType_Osr);

    MResumePoint* res = start->resumePoint();

    for (uint32_t i = 0; i < stackDepth(); i++) {
        MDefinition* def = slots_[i];
        MInstruction* cloneRp = nullptr;
        if (i == info().scopeChainSlot()) {
            if (def->isOsrScopeChain())
                cloneRp = def->toOsrScopeChain();
        } else if (i == info().returnValueSlot()) {
            if (def->isOsrReturnValue())
                cloneRp = def->toOsrReturnValue();
        } else if (info().hasArguments() && i == info().argsObjSlot()) {
            MOZ_ASSERT(def->isConstant() || def->isOsrArgumentsObject());
            MOZ_ASSERT_IF(def->isConstant(), def->toConstant()->value() == UndefinedValue());
            if (def->isOsrArgumentsObject())
                cloneRp = def->toOsrArgumentsObject();
        } else {
            MOZ_ASSERT(def->isOsrValue() || def->isGetArgumentsObjectArg() || def->isConstant() ||
                       def->isParameter());

            // A constant Undefined can show up here for an argument slot when
            // the function has an arguments object, but the argument in
            // question is stored on the scope chain.
            MOZ_ASSERT_IF(def->isConstant(), def->toConstant()->value() == UndefinedValue());

            if (def->isOsrValue())
                cloneRp = def->toOsrValue();
            else if (def->isGetArgumentsObjectArg())
                cloneRp = def->toGetArgumentsObjectArg();
            else if (def->isParameter())
                cloneRp = def->toParameter();
        }

        if (cloneRp) {
            MResumePoint* clone = MResumePoint::Copy(graph().alloc(), res);
            if (!clone)
                return false;
            cloneRp->setResumePoint(clone);
        }
    }

    return true;
}

void
MBasicBlock::setSlot(uint32_t slot, MDefinition* ins)
{
    slots_[slot] = ins;
}

void
MBasicBlock::setVariable(uint32_t index)
{
    MOZ_ASSERT(stackPosition_ > info_.firstStackSlot());
    setSlot(index, slots_[stackPosition_ - 1]);
}

void
MBasicBlock::setArg(uint32_t arg)
{
    setVariable(info_.argSlot(arg));
}

void
MBasicBlock::setLocal(uint32_t local)
{
    setVariable(info_.localSlot(local));
}

void
MBasicBlock::setSlot(uint32_t slot)
{
    setVariable(slot);
}

void
MBasicBlock::rewriteSlot(uint32_t slot, MDefinition* ins)
{
    setSlot(slot, ins);
}

void
MBasicBlock::rewriteAtDepth(int32_t depth, MDefinition* ins)
{
    MOZ_ASSERT(depth < 0);
    MOZ_ASSERT(stackPosition_ + depth >= info_.firstStackSlot());
    rewriteSlot(stackPosition_ + depth, ins);
}

void
MBasicBlock::push(MDefinition* ins)
{
    MOZ_ASSERT(stackPosition_ < nslots());
    slots_[stackPosition_++] = ins;
}

void
MBasicBlock::pushVariable(uint32_t slot)
{
    push(slots_[slot]);
}

void
MBasicBlock::pushArg(uint32_t arg)
{
    pushVariable(info_.argSlot(arg));
}

void
MBasicBlock::pushLocal(uint32_t local)
{
    pushVariable(info_.localSlot(local));
}

void
MBasicBlock::pushSlot(uint32_t slot)
{
    pushVariable(slot);
}

MDefinition*
MBasicBlock::pop()
{
    MOZ_ASSERT(stackPosition_ > info_.firstStackSlot());
    return slots_[--stackPosition_];
}

void
MBasicBlock::popn(uint32_t n)
{
    MOZ_ASSERT(stackPosition_ - n >= info_.firstStackSlot());
    MOZ_ASSERT(stackPosition_ >= stackPosition_ - n);
    stackPosition_ -= n;
}

MDefinition*
MBasicBlock::scopeChain()
{
    return getSlot(info().scopeChainSlot());
}

MDefinition*
MBasicBlock::argumentsObject()
{
    return getSlot(info().argsObjSlot());
}

void
MBasicBlock::setScopeChain(MDefinition* scopeObj)
{
    setSlot(info().scopeChainSlot(), scopeObj);
}

void
MBasicBlock::setArgumentsObject(MDefinition* argsObj)
{
    setSlot(info().argsObjSlot(), argsObj);
}

void
MBasicBlock::pick(int32_t depth)
{
    // pick take an element and move it to the top.
    // pick(-2):
    //   A B C D E
    //   A B D C E [ swapAt(-2) ]
    //   A B D E C [ swapAt(-1) ]
    for (; depth < 0; depth++)
        swapAt(depth);
}

void
MBasicBlock::swapAt(int32_t depth)
{
    uint32_t lhsDepth = stackPosition_ + depth - 1;
    uint32_t rhsDepth = stackPosition_ + depth;

    MDefinition* temp = slots_[lhsDepth];
    slots_[lhsDepth] = slots_[rhsDepth];
    slots_[rhsDepth] = temp;
}

MDefinition*
MBasicBlock::peek(int32_t depth)
{
    MOZ_ASSERT(depth < 0);
    MOZ_ASSERT(stackPosition_ + depth >= info_.firstStackSlot());
    return getSlot(stackPosition_ + depth);
}

void
MBasicBlock::discardLastIns()
{
    discard(lastIns());
}

MConstant*
MBasicBlock::optimizedOutConstant(TempAllocator& alloc)
{
    // If the first instruction is a MConstant(MagicValue(JS_OPTIMIZED_OUT))
    // then reuse it.
    MInstruction* ins = *begin();
    if (ins->type() == MIRType_MagicOptimizedOut)
        return ins->toConstant();

    MConstant* constant = MConstant::New(alloc, MagicValue(JS_OPTIMIZED_OUT));
    insertBefore(ins, constant);
    return constant;
}

void
MBasicBlock::addFromElsewhere(MInstruction* ins)
{
    MOZ_ASSERT(ins->block() != this);

    // Remove |ins| from its containing block.
    ins->block()->instructions_.remove(ins);

    // Add it to this block.
    add(ins);
}

void
MBasicBlock::moveBefore(MInstruction* at, MInstruction* ins)
{
    // Remove |ins| from the current block.
    MOZ_ASSERT(ins->block() == this);
    instructions_.remove(ins);

    // Insert into new block, which may be distinct.
    // Uses and operands are untouched.
    ins->setBlock(at->block());
    at->block()->instructions_.insertBefore(at, ins);
    ins->setTrackedSite(at->trackedSite());
}

MInstruction*
MBasicBlock::safeInsertTop(MDefinition* ins, IgnoreTop ignore)
{
    // Beta nodes and interrupt checks are required to be located at the
    // beginnings of basic blocks, so we must insert new instructions after any
    // such instructions.
    MInstructionIterator insertIter = !ins || ins->isPhi()
                                    ? begin()
                                    : begin(ins->toInstruction());
    while (insertIter->isBeta() ||
           insertIter->isInterruptCheck() ||
           insertIter->isConstant() ||
           (!(ignore & IgnoreRecover) && insertIter->isRecoveredOnBailout()))
    {
        insertIter++;
    }

    return *insertIter;
}

void
MBasicBlock::discardResumePoint(MResumePoint* rp, ReferencesType refType /* = RefType_Default */)
{
    if (refType & RefType_DiscardOperands)
        rp->releaseUses();
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

void
MBasicBlock::prepareForDiscard(MInstruction* ins, ReferencesType refType /* = RefType_Default */)
{
    // Only remove instructions from the same basic block.  This is needed for
    // correctly removing the resume point if any.
    MOZ_ASSERT(ins->block() == this);

    MResumePoint* rp = ins->resumePoint();
    if ((refType & RefType_DiscardResumePoint) && rp)
        discardResumePoint(rp, refType);

    // We need to assert that instructions have no uses after removing the their
    // resume points operands as they could be captured by their own resume
    // point.
    MOZ_ASSERT_IF(refType & RefType_AssertNoUses, !ins->hasUses());

    const uint32_t InstructionOperands = RefType_DiscardOperands | RefType_DiscardInstruction;
    if ((refType & InstructionOperands) == InstructionOperands) {
        for (size_t i = 0, e = ins->numOperands(); i < e; i++)
            ins->releaseOperand(i);
    }

    ins->setDiscarded();
}

void
MBasicBlock::discard(MInstruction* ins)
{
    prepareForDiscard(ins);
    instructions_.remove(ins);
}

void
MBasicBlock::discardIgnoreOperands(MInstruction* ins)
{
#ifdef DEBUG
    for (size_t i = 0, e = ins->numOperands(); i < e; i++)
        MOZ_ASSERT(!ins->hasOperand(i));
#endif

    prepareForDiscard(ins, RefType_IgnoreOperands);
    instructions_.remove(ins);
}

void
MBasicBlock::discardDef(MDefinition* at)
{
    if (at->isPhi())
        at->block_->discardPhi(at->toPhi());
    else
        at->block_->discard(at->toInstruction());
}

void
MBasicBlock::discardAllInstructions()
{
    MInstructionIterator iter = begin();
    discardAllInstructionsStartingAt(iter);
}

void
MBasicBlock::discardAllInstructionsStartingAt(MInstructionIterator iter)
{
    while (iter != end()) {
        // Discard operands and resume point operands and flag the instruction
        // as discarded.  Also we do not assert that we have no uses as blocks
        // might be removed in reverse post order.
        MInstruction* ins = *iter++;
        prepareForDiscard(ins, RefType_DefaultNoAssert);
        instructions_.remove(ins);
    }
}

void
MBasicBlock::discardAllPhiOperands()
{
    for (MPhiIterator iter = phisBegin(); iter != phisEnd(); iter++)
        iter->removeAllOperands();

    for (MBasicBlock** pred = predecessors_.begin(); pred != predecessors_.end(); pred++)
        (*pred)->clearSuccessorWithPhis();
}

void
MBasicBlock::discardAllPhis()
{
    discardAllPhiOperands();
    phis_.clear();
}

void
MBasicBlock::discardAllResumePoints(bool discardEntry)
{
    if (outerResumePoint_)
        clearOuterResumePoint();

    if (discardEntry && entryResumePoint_)
        clearEntryResumePoint();

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

void
MBasicBlock::insertBefore(MInstruction* at, MInstruction* ins)
{
    MOZ_ASSERT(at->block() == this);
    ins->setBlock(this);
    graph().allocDefinitionId(ins);
    instructions_.insertBefore(at, ins);
    ins->setTrackedSite(at->trackedSite());
}

void
MBasicBlock::insertAfter(MInstruction* at, MInstruction* ins)
{
    MOZ_ASSERT(at->block() == this);
    ins->setBlock(this);
    graph().allocDefinitionId(ins);
    instructions_.insertAfter(at, ins);
    ins->setTrackedSite(at->trackedSite());
}

void
MBasicBlock::insertAtEnd(MInstruction* ins)
{
    if (hasLastIns())
        insertBefore(lastIns(), ins);
    else
        add(ins);
}

void
MBasicBlock::add(MInstruction* ins)
{
    MOZ_ASSERT(!hasLastIns());
    ins->setBlock(this);
    graph().allocDefinitionId(ins);
    instructions_.pushBack(ins);
    ins->setTrackedSite(trackedSite_);
}

void
MBasicBlock::end(MControlInstruction* ins)
{
    MOZ_ASSERT(!hasLastIns()); // Existing control instructions should be removed first.
    MOZ_ASSERT(ins);
    add(ins);
}

void
MBasicBlock::addPhi(MPhi* phi)
{
    phis_.pushBack(phi);
    phi->setBlock(this);
    graph().allocDefinitionId(phi);
}

void
MBasicBlock::discardPhi(MPhi* phi)
{
    MOZ_ASSERT(!phis_.empty());

    phi->removeAllOperands();
    phi->setDiscarded();

    phis_.remove(phi);

    if (phis_.empty()) {
        for (MBasicBlock* pred : predecessors_)
            pred->clearSuccessorWithPhis();
    }
}

void
MBasicBlock::flagOperandsOfPrunedBranches(MInstruction* ins)
{
    // Find the previous resume point which would be used for bailing out.
    MResumePoint* rp = nullptr;
    for (MInstructionReverseIterator iter = rbegin(ins); iter != rend(); iter++) {
        rp = iter->resumePoint();
        if (rp)
            break;
    }

    // If none, take the entry resume point.
    if (!rp)
        rp = entryResumePoint();

    // The only blocks which do not have any entryResumePoint in Ion, are the
    // SplitEdge blocks.  SplitEdge blocks only have a Goto instruction before
    // Range Analysis phase.  In adjustInputs, we are manipulating instructions
    // which have a TypePolicy.  So, as a Goto has no operand and no type
    // policy, the entry resume point should exists.
    MOZ_ASSERT(rp);

    // Flag all operand as being potentially used.
    while (rp) {
        for (size_t i = 0, end = rp->numOperands(); i < end; i++)
            rp->getOperand(i)->setUseRemovedUnchecked();
        rp = rp->caller();
    }
}

bool
MBasicBlock::addPredecessor(TempAllocator& alloc, MBasicBlock* pred)
{
    return addPredecessorPopN(alloc, pred, 0);
}

bool
MBasicBlock::addPredecessorPopN(TempAllocator& alloc, MBasicBlock* pred, uint32_t popped)
{
    MOZ_ASSERT(pred);
    MOZ_ASSERT(predecessors_.length() > 0);

    // Predecessors must be finished, and at the correct stack depth.
    MOZ_ASSERT(pred->hasLastIns());
    MOZ_ASSERT(pred->stackPosition_ == stackPosition_ + popped);

    for (uint32_t i = 0, e = stackPosition_; i < e; ++i) {
        MDefinition* mine = getSlot(i);
        MDefinition* other = pred->getSlot(i);

        if (mine != other) {
            // If the current instruction is a phi, and it was created in this
            // basic block, then we have already placed this phi and should
            // instead append to its operands.
            if (mine->isPhi() && mine->block() == this) {
                MOZ_ASSERT(predecessors_.length());
                if (!mine->toPhi()->addInputSlow(other))
                    return false;
            } else {
                // Otherwise, create a new phi node.
                MPhi* phi;
                if (mine->type() == other->type())
                    phi = MPhi::New(alloc, mine->type());
                else
                    phi = MPhi::New(alloc);
                addPhi(phi);

                // Prime the phi for each predecessor, so input(x) comes from
                // predecessor(x).
                if (!phi->reserveLength(predecessors_.length() + 1))
                    return false;

                for (size_t j = 0, numPreds = predecessors_.length(); j < numPreds; ++j) {
                    MOZ_ASSERT(predecessors_[j]->getSlot(i) == mine);
                    phi->addInput(mine);
                }
                phi->addInput(other);

                setSlot(i, phi);
                if (entryResumePoint())
                    entryResumePoint()->replaceOperand(i, phi);
            }
        }
    }

    return predecessors_.append(pred);
}

void
MBasicBlock::addPredecessorSameInputsAs(MBasicBlock* pred, MBasicBlock* existingPred)
{
    MOZ_ASSERT(pred);
    MOZ_ASSERT(predecessors_.length() > 0);

    // Predecessors must be finished, and at the correct stack depth.
    MOZ_ASSERT(pred->hasLastIns());
    MOZ_ASSERT(!pred->successorWithPhis());

    AutoEnterOOMUnsafeRegion oomUnsafe;

    if (!phisEmpty()) {
        size_t existingPosition = indexForPredecessor(existingPred);
        for (MPhiIterator iter = phisBegin(); iter != phisEnd(); iter++) {
            if (!iter->addInputSlow(iter->getOperand(existingPosition)))
                oomUnsafe.crash("MBasicBlock::addPredecessorAdjustPhis");
        }
    }

    if (!predecessors_.append(pred))
        oomUnsafe.crash("MBasicBlock::addPredecessorAdjustPhis");
}

bool
MBasicBlock::addPredecessorWithoutPhis(MBasicBlock* pred)
{
    // Predecessors must be finished.
    MOZ_ASSERT(pred && pred->hasLastIns());
    return predecessors_.append(pred);
}

bool
MBasicBlock::addImmediatelyDominatedBlock(MBasicBlock* child)
{
    return immediatelyDominated_.append(child);
}

void
MBasicBlock::removeImmediatelyDominatedBlock(MBasicBlock* child)
{
    for (size_t i = 0; ; ++i) {
        MOZ_ASSERT(i < immediatelyDominated_.length(),
                   "Dominated block to remove not present");
        if (immediatelyDominated_[i] == child) {
            immediatelyDominated_[i] = immediatelyDominated_.back();
            immediatelyDominated_.popBack();
            return;
        }
    }
}

void
MBasicBlock::assertUsesAreNotWithin(MUseIterator use, MUseIterator end)
{
#ifdef DEBUG
    for (; use != end; use++) {
        MOZ_ASSERT_IF(use->consumer()->isDefinition(),
                      use->consumer()->toDefinition()->block()->id() < id());
    }
#endif
}

AbortReason
MBasicBlock::setBackedge(MBasicBlock* pred)
{
    // Predecessors must be finished, and at the correct stack depth.
    MOZ_ASSERT(hasLastIns());
    MOZ_ASSERT(pred->hasLastIns());
    MOZ_ASSERT(pred->stackDepth() == entryResumePoint()->stackDepth());

    // We must be a pending loop header
    MOZ_ASSERT(kind_ == PENDING_LOOP_HEADER);

    bool hadTypeChange = false;

    // Add exit definitions to each corresponding phi at the entry.
    if (!inheritPhisFromBackedge(pred, &hadTypeChange))
        return AbortReason_Alloc;

    if (hadTypeChange) {
        for (MPhiIterator phi = phisBegin(); phi != phisEnd(); phi++)
            phi->removeOperand(phi->numOperands() - 1);
        return AbortReason_Disable;
    }

    // We are now a loop header proper
    kind_ = LOOP_HEADER;

    if (!predecessors_.append(pred))
        return AbortReason_Alloc;

    return AbortReason_NoAbort;
}

bool
MBasicBlock::setBackedgeAsmJS(MBasicBlock* pred)
{
    // Predecessors must be finished, and at the correct stack depth.
    MOZ_ASSERT(hasLastIns());
    MOZ_ASSERT(pred->hasLastIns());
    MOZ_ASSERT(stackDepth() == pred->stackDepth());

    // We must be a pending loop header
    MOZ_ASSERT(kind_ == PENDING_LOOP_HEADER);

    // Add exit definitions to each corresponding phi at the entry.
    // Note: Phis are inserted in the same order as the slots. (see
    // MBasicBlock::NewAsmJS)
    size_t slot = 0;
    for (MPhiIterator phi = phisBegin(); phi != phisEnd(); phi++, slot++) {
        MPhi* entryDef = *phi;
        MDefinition* exitDef = pred->getSlot(slot);

        // Assert that we already placed phis for each slot.
        MOZ_ASSERT(entryDef->block() == this);

        // Assert that the phi already has the correct type.
        MOZ_ASSERT(entryDef->type() == exitDef->type());
        MOZ_ASSERT(entryDef->type() != MIRType_Value);

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

        // Phis always have room for 2 operands, so we can use addInput.
        entryDef->addInput(exitDef);

        MOZ_ASSERT(slot < pred->stackDepth());
        setSlot(slot, entryDef);
    }

    // We are now a loop header proper
    kind_ = LOOP_HEADER;

    return predecessors_.append(pred);
}

void
MBasicBlock::clearLoopHeader()
{
    MOZ_ASSERT(isLoopHeader());
    kind_ = NORMAL;
}

void
MBasicBlock::setLoopHeader(MBasicBlock* newBackedge)
{
    MOZ_ASSERT(!isLoopHeader());
    kind_ = LOOP_HEADER;

    size_t numPreds = numPredecessors();
    MOZ_ASSERT(numPreds != 0);

    size_t lastIndex = numPreds - 1;
    size_t oldIndex = 0;
    for (; ; ++oldIndex) {
        MOZ_ASSERT(oldIndex < numPreds);
        MBasicBlock* pred = getPredecessor(oldIndex);
        if (pred == newBackedge)
            break;
    }

    // Set the loop backedge to be the last element in predecessors_.
    Swap(predecessors_[oldIndex], predecessors_[lastIndex]);

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

size_t
MBasicBlock::numSuccessors() const
{
    MOZ_ASSERT(lastIns());
    return lastIns()->numSuccessors();
}

MBasicBlock*
MBasicBlock::getSuccessor(size_t index) const
{
    MOZ_ASSERT(lastIns());
    return lastIns()->getSuccessor(index);
}

size_t
MBasicBlock::getSuccessorIndex(MBasicBlock* block) const
{
    MOZ_ASSERT(lastIns());
    for (size_t i = 0; i < numSuccessors(); i++) {
        if (getSuccessor(i) == block)
            return i;
    }
    MOZ_CRASH("Invalid successor");
}

size_t
MBasicBlock::getPredecessorIndex(MBasicBlock* block) const
{
    for (size_t i = 0, e = numPredecessors(); i < e; ++i) {
        if (getPredecessor(i) == block)
            return i;
    }
    MOZ_CRASH("Invalid predecessor");
}

void
MBasicBlock::replaceSuccessor(size_t pos, MBasicBlock* split)
{
    MOZ_ASSERT(lastIns());

    // Note, during split-critical-edges, successors-with-phis is not yet set.
    // During PAA, this case is handled before we enter.
    MOZ_ASSERT_IF(successorWithPhis_, successorWithPhis_ != getSuccessor(pos));

    lastIns()->replaceSuccessor(pos, split);
}

void
MBasicBlock::replacePredecessor(MBasicBlock* old, MBasicBlock* split)
{
    for (size_t i = 0; i < numPredecessors(); i++) {
        if (getPredecessor(i) == old) {
            predecessors_[i] = split;

#ifdef DEBUG
            // The same block should not appear twice in the predecessor list.
            for (size_t j = i; j < numPredecessors(); j++)
                MOZ_ASSERT(predecessors_[j] != old);
#endif

            return;
        }
    }

    MOZ_CRASH("predecessor was not found");
}

void
MBasicBlock::clearDominatorInfo()
{
    setImmediateDominator(nullptr);
    immediatelyDominated_.clear();
    numDominated_ = 0;
}

void
MBasicBlock::removePredecessorWithoutPhiOperands(MBasicBlock* pred, size_t predIndex)
{
    // If we're removing the last backedge, this is no longer a loop.
    if (isLoopHeader() && hasUniqueBackedge() && backedge() == pred)
        clearLoopHeader();

    // Adjust phis.  Note that this can leave redundant phis behind.
    // Don't adjust successorWithPhis() if we haven't constructed this
    // information yet.
    if (pred->successorWithPhis()) {
        MOZ_ASSERT(pred->positionInPhiSuccessor() == predIndex);
        pred->clearSuccessorWithPhis();
        for (size_t j = predIndex+1; j < numPredecessors(); j++)
            getPredecessor(j)->setSuccessorWithPhis(this, j - 1);
    }

    // Remove from pred list.
    predecessors_.erase(predecessors_.begin() + predIndex);
}

void
MBasicBlock::removePredecessor(MBasicBlock* pred)
{
    size_t predIndex = getPredecessorIndex(pred);

    // Remove the phi operands.
    for (MPhiIterator iter(phisBegin()), end(phisEnd()); iter != end; ++iter)
        iter->removeOperand(predIndex);

    // Now we can call the underlying function, which expects that phi
    // operands have been removed.
    removePredecessorWithoutPhiOperands(pred, predIndex);
}

void
MBasicBlock::inheritPhis(MBasicBlock* header)
{
    MResumePoint* headerRp = header->entryResumePoint();
    size_t stackDepth = headerRp->stackDepth();
    for (size_t slot = 0; slot < stackDepth; slot++) {
        MDefinition* exitDef = getSlot(slot);
        MDefinition* loopDef = headerRp->getOperand(slot);
        if (loopDef->block() != header) {
            MOZ_ASSERT(loopDef->block()->id() < header->id());
            MOZ_ASSERT(loopDef == exitDef);
            continue;
        }

        // Phis are allocated by NewPendingLoopHeader.
        MPhi* phi = loopDef->toPhi();
        MOZ_ASSERT(phi->numOperands() == 2);

        // The entry definition is always the leftmost input to the phi.
        MDefinition* entryDef = phi->getOperand(0);

        if (entryDef != exitDef)
            continue;

        // If the entryDef is the same as exitDef, then we must propagate the
        // phi down to this successor. This chance was missed as part of
        // setBackedge() because exits are not captured in resume points.
        setSlot(slot, phi);
    }
}

bool
MBasicBlock::inheritPhisFromBackedge(MBasicBlock* backedge, bool* hadTypeChange)
{
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

        bool typeChange = false;

        if (!entryDef->addInputSlow(exitDef))
            return false;
        if (!entryDef->checkForTypeChange(exitDef, &typeChange))
            return false;
        *hadTypeChange |= typeChange;
        setSlot(slot, entryDef);
    }

    return true;
}

bool
MBasicBlock::specializePhis()
{
    for (MPhiIterator iter = phisBegin(); iter != phisEnd(); iter++) {
        MPhi* phi = *iter;
        if (!phi->specializeType())
            return false;
    }
    return true;
}

MTest*
MBasicBlock::immediateDominatorBranch(BranchDirection* pdirection)
{
    *pdirection = FALSE_BRANCH;

    if (numPredecessors() != 1)
        return nullptr;

    MBasicBlock* dom = immediateDominator();
    if (dom != getPredecessor(0))
        return nullptr;

    // Look for a trailing MTest branching to this block.
    MInstruction* ins = dom->lastIns();
    if (ins->isTest()) {
        MTest* test = ins->toTest();

        MOZ_ASSERT(test->ifTrue() == this || test->ifFalse() == this);
        if (test->ifTrue() == this && test->ifFalse() == this)
            return nullptr;

        *pdirection = (test->ifTrue() == this) ? TRUE_BRANCH : FALSE_BRANCH;
        return test;
    }

    return nullptr;
}

void
MBasicBlock::dumpStack(GenericPrinter& out)
{
#ifdef DEBUG
    out.printf(" %-3s %-16s %-6s %-10s\n", "#", "name", "copyOf", "first/next");
    out.printf("-------------------------------------------\n");
    for (uint32_t i = 0; i < stackPosition_; i++) {
        out.printf(" %-3d", i);
        out.printf(" %-16p\n", (void*)slots_[i]);
    }
#endif
}

void
MBasicBlock::dumpStack()
{
    Fprinter out(stderr);
    dumpStack(out);
    out.finish();
}

void
MIRGraph::dump(GenericPrinter& out)
{
#ifdef DEBUG
    for (MBasicBlockIterator iter(begin()); iter != end(); iter++) {
        iter->dump(out);
        out.printf("\n");
    }
#endif
}

void
MIRGraph::dump()
{
    Fprinter out(stderr);
    dump(out);
    out.finish();
}

void
MBasicBlock::dump(GenericPrinter& out)
{
#ifdef DEBUG
    out.printf("block%u:%s%s%s\n", id(),
               isLoopHeader() ? " (loop header)" : "",
               unreachable() ? " (unreachable)" : "",
               isMarked() ? " (marked)" : "");
    if (MResumePoint* resume = entryResumePoint())
        resume->dump(out);
    for (MPhiIterator iter(phisBegin()); iter != phisEnd(); iter++)
        iter->dump(out);
    for (MInstructionIterator iter(begin()); iter != end(); iter++)
        iter->dump(out);
#endif
}

void
MBasicBlock::dump()
{
    Fprinter out(stderr);
    dump(out);
    out.finish();
}
