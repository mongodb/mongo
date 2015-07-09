/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_LiveRangeAllocator_h
#define jit_LiveRangeAllocator_h

#include "mozilla/Array.h"
#include "mozilla/DebugOnly.h"

#include "jit/RegisterAllocator.h"
#include "jit/StackSlotAllocator.h"

// Common structures and functions used by register allocators that operate on
// virtual register live ranges.

namespace js {
namespace jit {

class Requirement
{
  public:
    enum Kind {
        NONE,
        REGISTER,
        FIXED,
        MUST_REUSE_INPUT
    };

    Requirement()
      : kind_(NONE)
    { }

    explicit Requirement(Kind kind)
      : kind_(kind)
    {
        // These have dedicated constructors.
        MOZ_ASSERT(kind != FIXED && kind != MUST_REUSE_INPUT);
    }

    Requirement(Kind kind, CodePosition at)
      : kind_(kind),
        position_(at)
    {
        // These have dedicated constructors.
        MOZ_ASSERT(kind != FIXED && kind != MUST_REUSE_INPUT);
    }

    explicit Requirement(LAllocation fixed)
      : kind_(FIXED),
        allocation_(fixed)
    {
        MOZ_ASSERT(!fixed.isBogus() && !fixed.isUse());
    }

    // Only useful as a hint, encodes where the fixed requirement is used to
    // avoid allocating a fixed register too early.
    Requirement(LAllocation fixed, CodePosition at)
      : kind_(FIXED),
        allocation_(fixed),
        position_(at)
    {
        MOZ_ASSERT(!fixed.isBogus() && !fixed.isUse());
    }

    Requirement(uint32_t vreg, CodePosition at)
      : kind_(MUST_REUSE_INPUT),
        allocation_(LUse(vreg, LUse::ANY)),
        position_(at)
    { }

    Kind kind() const {
        return kind_;
    }

    LAllocation allocation() const {
        MOZ_ASSERT(!allocation_.isBogus() && !allocation_.isUse());
        return allocation_;
    }

    uint32_t virtualRegister() const {
        MOZ_ASSERT(allocation_.isUse());
        MOZ_ASSERT(kind() == MUST_REUSE_INPUT);
        return allocation_.toUse()->virtualRegister();
    }

    CodePosition pos() const {
        return position_;
    }

    int priority() const;

    bool mergeRequirement(const Requirement& newRequirement) {
        // Merge newRequirement with any existing requirement, returning false
        // if the new and old requirements conflict.
        MOZ_ASSERT(newRequirement.kind() != Requirement::MUST_REUSE_INPUT);

        if (newRequirement.kind() == Requirement::FIXED) {
            if (kind() == Requirement::FIXED)
                return newRequirement.allocation() == allocation();
            *this = newRequirement;
            return true;
        }

        MOZ_ASSERT(newRequirement.kind() == Requirement::REGISTER);
        if (kind() == Requirement::FIXED)
            return allocation().isRegister();

        *this = newRequirement;
        return true;
    }

    // Return a string describing this requirement. This is not re-entrant!
    const char* toString() const;

    void dump() const;

  private:
    Kind kind_;
    LAllocation allocation_;
    CodePosition position_;
};

struct UsePosition : public TempObject,
                     public InlineForwardListNode<UsePosition>
{
    LUse* use;
    CodePosition pos;

    UsePosition(LUse* use, CodePosition pos) :
        use(use),
        pos(pos)
    {
        // Verify that the usedAtStart() flag is consistent with the
        // subposition. For now ignore fixed registers, because they
        // are handled specially around calls.
        MOZ_ASSERT_IF(!use->isFixedRegister(),
                      pos.subpos() == (use->usedAtStart()
                                       ? CodePosition::INPUT
                                       : CodePosition::OUTPUT));
    }
};

typedef InlineForwardListIterator<UsePosition> UsePositionIterator;

static inline bool
UseCompatibleWith(const LUse* use, LAllocation alloc)
{
    switch (use->policy()) {
      case LUse::ANY:
      case LUse::KEEPALIVE:
        return alloc.isRegister() || alloc.isMemory();
      case LUse::REGISTER:
        return alloc.isRegister();
      case LUse::FIXED:
          // Fixed uses are handled using fixed intervals. The
          // UsePosition is only used as hint.
        return alloc.isRegister();
      default:
        MOZ_CRASH("Unknown use policy");
    }
}

#ifdef DEBUG

static inline bool
DefinitionCompatibleWith(LNode* ins, const LDefinition* def, LAllocation alloc)
{
    if (ins->isPhi()) {
        if (def->isFloatReg())
            return alloc.isFloatReg() || alloc.isStackSlot();
        return alloc.isGeneralReg() || alloc.isStackSlot();
    }

    switch (def->policy()) {
      case LDefinition::REGISTER:
        if (!alloc.isRegister())
            return false;
        return alloc.isFloatReg() == def->isFloatReg();
      case LDefinition::FIXED:
        return alloc == *def->output();
      case LDefinition::MUST_REUSE_INPUT:
        if (!alloc.isRegister() || !ins->numOperands())
            return false;
        return alloc == *ins->getOperand(def->getReusedInput());
      default:
        MOZ_CRASH("Unknown definition policy");
    }
}

#endif // DEBUG

static inline LDefinition*
FindReusingDefinition(LNode* ins, LAllocation* alloc)
{
    for (size_t i = 0; i < ins->numDefs(); i++) {
        LDefinition* def = ins->getDef(i);
        if (def->policy() == LDefinition::MUST_REUSE_INPUT &&
            ins->getOperand(def->getReusedInput()) == alloc)
            return def;
    }
    for (size_t i = 0; i < ins->numTemps(); i++) {
        LDefinition* def = ins->getTemp(i);
        if (def->policy() == LDefinition::MUST_REUSE_INPUT &&
            ins->getOperand(def->getReusedInput()) == alloc)
            return def;
    }
    return nullptr;
}

/*
 * A live interval is a set of disjoint ranges of code positions where a
 * virtual register is live. Register allocation operates on these intervals,
 * splitting them as necessary and assigning allocations to them as it runs.
 */
class LiveInterval
  : public InlineListNode<LiveInterval>,
    public TempObject
{
  public:
    /*
     * A range is a contiguous sequence of CodePositions where the virtual
     * register associated with this interval is live.
     */
    struct Range {
        Range()
          : from(),
            to()
        { }
        Range(CodePosition f, CodePosition t)
          : from(f),
            to(t)
        {
            MOZ_ASSERT(from < to);
        }

        // The beginning of this range, inclusive.
        CodePosition from;

        // The end of this range, exclusive.
        CodePosition to;

        bool empty() const {
            return from >= to;
        }

        // Whether this range wholly contains other.
        bool contains(const Range* other) const;

        // Intersect this range with other, returning the subranges of this
        // that are before, inside, or after other.
        void intersect(const Range* other, Range* pre, Range* inside, Range* post) const;

        // Return a string describing this range. This is not re-entrant!
        const char* toString() const;

        void dump() const;
    };

  private:
    Vector<Range, 1, JitAllocPolicy> ranges_;
    LAllocation alloc_;
    LiveInterval* spillInterval_;
    uint32_t vreg_;
    uint32_t index_;
    Requirement requirement_;
    Requirement hint_;
    InlineForwardList<UsePosition> uses_;
    size_t lastProcessedRange_;

    LiveInterval(TempAllocator& alloc, uint32_t vreg, uint32_t index)
      : ranges_(alloc),
        spillInterval_(nullptr),
        vreg_(vreg),
        index_(index),
        lastProcessedRange_(size_t(-1))
    { }

    LiveInterval(TempAllocator& alloc, uint32_t index)
      : ranges_(alloc),
        spillInterval_(nullptr),
        vreg_(UINT32_MAX),
        index_(index),
        lastProcessedRange_(size_t(-1))
    { }

  public:
    static LiveInterval* New(TempAllocator& alloc, uint32_t vreg, uint32_t index) {
        return new(alloc) LiveInterval(alloc, vreg, index);
    }
    static LiveInterval* New(TempAllocator& alloc, uint32_t index) {
        return new(alloc) LiveInterval(alloc, index);
    }

    bool addRange(CodePosition from, CodePosition to);
    bool addRangeAtHead(CodePosition from, CodePosition to);
    void setFrom(CodePosition from);
    CodePosition intersect(LiveInterval* other);
    bool covers(CodePosition pos);
    CodePosition nextCoveredAfter(CodePosition pos);

    CodePosition start() const {
        MOZ_ASSERT(!ranges_.empty());
        return ranges_.back().from;
    }

    CodePosition end() const {
        MOZ_ASSERT(!ranges_.empty());
        return ranges_.begin()->to;
    }

    size_t numRanges() const {
        return ranges_.length();
    }
    const Range* getRange(size_t i) const {
        return &ranges_[i];
    }
    void setLastProcessedRange(size_t range, mozilla::DebugOnly<CodePosition> pos) {
        // If the range starts after pos, we may not be able to use
        // it in the next lastProcessedRangeIfValid call.
        MOZ_ASSERT(ranges_[range].from <= pos);
        lastProcessedRange_ = range;
    }
    size_t lastProcessedRangeIfValid(CodePosition pos) const {
        if (lastProcessedRange_ < ranges_.length() && ranges_[lastProcessedRange_].from <= pos)
            return lastProcessedRange_;
        return ranges_.length() - 1;
    }

    LAllocation* getAllocation() {
        return &alloc_;
    }
    void setAllocation(LAllocation alloc) {
        alloc_ = alloc;
    }
    void setSpillInterval(LiveInterval* spill) {
        spillInterval_ = spill;
    }
    LiveInterval* spillInterval() {
        return spillInterval_;
    }
    bool hasVreg() const {
        return vreg_ != UINT32_MAX;
    }
    uint32_t vreg() const {
        MOZ_ASSERT(hasVreg());
        return vreg_;
    }
    uint32_t index() const {
        return index_;
    }
    void setIndex(uint32_t index) {
        index_ = index;
    }
    const Requirement* requirement() const {
        return &requirement_;
    }
    void setRequirement(const Requirement& requirement) {
        // A MUST_REUSE_INPUT requirement complicates regalloc too much; it
        // should only be used as hint.
        MOZ_ASSERT(requirement.kind() != Requirement::MUST_REUSE_INPUT);
        requirement_ = requirement;
    }
    bool addRequirement(const Requirement& newRequirement) {
        return requirement_.mergeRequirement(newRequirement);
    }
    void addHint(const Requirement& newHint) {
        // Unlike addRequirement, here in addHint we ignore merge failures,
        // because these are just hints.
        hint_.mergeRequirement(newHint);
    }
    const Requirement* hint() const {
        return &hint_;
    }
    void setHint(const Requirement& hint) {
        hint_ = hint;
    }
    bool isSpill() const {
        return alloc_.isStackSlot();
    }
    bool splitFrom(CodePosition pos, LiveInterval* after);

    void addUse(UsePosition* use);
    void addUseAtEnd(UsePosition* use);
    UsePosition* popUse();
    UsePosition* nextUseAfter(CodePosition pos);
    CodePosition nextUsePosAfter(CodePosition pos);
    CodePosition firstIncompatibleUse(LAllocation alloc);

    UsePositionIterator usesBegin() const {
        return uses_.begin();
    }

    UsePositionIterator usesEnd() const {
        return uses_.end();
    }

    bool usesEmpty() const {
        return uses_.empty();
    }

    UsePosition* usesBack() {
        return uses_.back();
    }

#ifdef DEBUG
    void validateRanges();
#endif

    // Return a string describing the ranges in this LiveInterval. This is
    // not re-entrant!
    const char* rangesToString() const;

    // Return a string describing this LiveInterval. This is not re-entrant!
    const char* toString() const;

    void dump() const;
};

/*
 * Represents all of the register allocation state associated with a virtual
 * register, including all associated intervals and pointers to relevant LIR
 * structures.
 */
class VirtualRegister
{
    LNode* ins_;
    LDefinition* def_;
    Vector<LiveInterval*, 1, JitAllocPolicy> intervals_;

    // Whether def_ is a temp or an output.
    bool isTemp_ : 1;

    void operator=(const VirtualRegister&) = delete;
    VirtualRegister(const VirtualRegister&) = delete;

  protected:
    explicit VirtualRegister(TempAllocator& alloc)
      : intervals_(alloc)
    {}

  public:
    bool init(TempAllocator& alloc, LNode* ins, LDefinition* def,
              bool isTemp)
    {
        MOZ_ASSERT(ins && !ins_);
        ins_ = ins;
        def_ = def;
        isTemp_ = isTemp;
        LiveInterval* initial = LiveInterval::New(alloc, def->virtualRegister(), 0);
        if (!initial)
            return false;
        return intervals_.append(initial);
    }
    LBlock* block() {
        return ins_->block();
    }
    LNode* ins() {
        return ins_;
    }
    LDefinition* def() const {
        return def_;
    }
    LDefinition::Type type() const {
        return def()->type();
    }
    bool isTemp() const {
        return isTemp_;
    }
    size_t numIntervals() const {
        return intervals_.length();
    }
    LiveInterval* getInterval(size_t i) const {
        return intervals_[i];
    }
    LiveInterval* lastInterval() const {
        MOZ_ASSERT(numIntervals() > 0);
        return getInterval(numIntervals() - 1);
    }
    void replaceInterval(LiveInterval* old, LiveInterval* interval) {
        MOZ_ASSERT(intervals_[old->index()] == old);
        interval->setIndex(old->index());
        intervals_[old->index()] = interval;
    }
    bool addInterval(LiveInterval* interval) {
        MOZ_ASSERT(interval->numRanges());
        MOZ_ASSERT(interval->vreg() != 0);

        // Preserve ascending order for faster lookups.
        LiveInterval** found = nullptr;
        LiveInterval** i;
        for (i = intervals_.begin(); i != intervals_.end(); i++) {
            if (!found && interval->start() < (*i)->start())
                found = i;
            if (found)
                (*i)->setIndex((*i)->index() + 1);
        }
        if (!found)
            found = intervals_.end();
        interval->setIndex(found - intervals_.begin());
        return intervals_.insert(found, interval);
    }
    void removeInterval(LiveInterval* interval) {
        intervals_.erase(intervals_.begin() + interval->index());
        for (size_t i = interval->index(), e = intervals_.length(); i < e; ++i)
            intervals_[i]->setIndex(i);
        interval->setIndex(-1);
    }
    bool isFloatReg() const {
        return def_->isFloatReg();
    }
    bool isCompatibleReg(const AnyRegister& r) const {
        return def_->isCompatibleReg(r);
    }
    bool isCompatibleVReg(const VirtualRegister& vr) const {
        return def_->isCompatibleDef(*vr.def_);
    }

    LiveInterval* intervalFor(CodePosition pos);
    LiveInterval* getFirstInterval();
};

// Index of the virtual registers in a graph. VREG is a subclass of
// VirtualRegister extended with any allocator specific state for the vreg.
template <typename VREG>
class VirtualRegisterMap
{
  private:
    FixedList<VREG> vregs_;

    void operator=(const VirtualRegisterMap&) = delete;
    VirtualRegisterMap(const VirtualRegisterMap&) = delete;

  public:
    VirtualRegisterMap()
      : vregs_()
    { }

    bool init(MIRGenerator* gen, uint32_t numVregs) {
        if (!vregs_.init(gen->alloc(), numVregs))
            return false;
        memset(&vregs_[0], 0, sizeof(VREG) * numVregs);
        TempAllocator& alloc = gen->alloc();
        for (uint32_t i = 0; i < numVregs; i++)
            new(&vregs_[i]) VREG(alloc);
        return true;
    }
    VREG& operator[](unsigned int index) {
        return vregs_[index];
    }
    VREG& operator[](const LAllocation* alloc) {
        MOZ_ASSERT(alloc->isUse());
        return vregs_[alloc->toUse()->virtualRegister()];
    }
    VREG& operator[](const LDefinition* def) {
        return vregs_[def->virtualRegister()];
    }
    uint32_t numVirtualRegisters() const {
        return vregs_.length();
    }
};

static inline bool
IsNunbox(VirtualRegister* vreg)
{
#ifdef JS_NUNBOX32
    return vreg->type() == LDefinition::TYPE ||
           vreg->type() == LDefinition::PAYLOAD;
#else
    return false;
#endif
}

static inline bool
IsSlotsOrElements(VirtualRegister* vreg)
{
    return vreg->type() == LDefinition::SLOTS;
}

static inline bool
IsTraceable(VirtualRegister* reg)
{
    if (reg->type() == LDefinition::OBJECT)
        return true;
#ifdef JS_PUNBOX64
    if (reg->type() == LDefinition::BOX)
        return true;
#endif
    return false;
}

typedef InlineList<LiveInterval>::iterator IntervalIterator;
typedef InlineList<LiveInterval>::reverse_iterator IntervalReverseIterator;

// The forLSRA parameter indicates whether the underlying allocator is LSRA.
// This changes the generated live ranges in various ways: inserting additional
// fixed uses of registers, and shifting the boundaries of live ranges by small
// amounts. This exists because different allocators handle live ranges
// differently; ideally, they would all treat live ranges in the same way.
template <typename VREG, bool forLSRA>
class LiveRangeAllocator : protected RegisterAllocator
{
  protected:
    // Computed inforamtion
    BitSet* liveIn;
    VirtualRegisterMap<VREG> vregs;
    mozilla::Array<LiveInterval*, AnyRegister::Total> fixedIntervals;

    // Union of all ranges in fixedIntervals, used to quickly determine
    // whether an interval intersects with a fixed register.
    LiveInterval* fixedIntervalsUnion;

    // Allocation state
    StackSlotAllocator stackSlotAllocator;

    LiveRangeAllocator(MIRGenerator* mir, LIRGenerator* lir, LIRGraph& graph)
      : RegisterAllocator(mir, lir, graph),
        liveIn(nullptr),
        fixedIntervalsUnion(nullptr)
    {
    }

    bool buildLivenessInfo();

    bool init();

    bool addFixedRangeAtHead(AnyRegister reg, CodePosition from, CodePosition to) {
        if (!fixedIntervals[reg.code()]->addRangeAtHead(from, to))
            return false;
        return fixedIntervalsUnion->addRangeAtHead(from, to);
    }

    void validateVirtualRegisters()
    {
#ifdef DEBUG
        if (!js_JitOptions.checkGraphConsistency)
            return;

        for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
            VirtualRegister* reg = &vregs[i];

            LiveInterval* prev = nullptr;
            for (size_t j = 0; j < reg->numIntervals(); j++) {
                LiveInterval* interval = reg->getInterval(j);
                MOZ_ASSERT(interval->vreg() == i);
                MOZ_ASSERT(interval->index() == j);

                if (interval->numRanges() == 0)
                    continue;

                MOZ_ASSERT_IF(prev, prev->end() <= interval->start());
                interval->validateRanges();

                prev = interval;
            }
        }
#endif
    }

#ifdef JS_NUNBOX32
    VREG* otherHalfOfNunbox(VirtualRegister* vreg) {
        signed offset = OffsetToOtherHalfOfNunbox(vreg->type());
        VREG* other = &vregs[vreg->def()->virtualRegister() + offset];
        AssertTypesFormANunbox(vreg->type(), other->type());
        return other;
    }
#endif

    bool addMove(LMoveGroup* moves, LiveInterval* from, LiveInterval* to, LDefinition::Type type) {
        MOZ_ASSERT(*from->getAllocation() != *to->getAllocation());
        return moves->add(from->getAllocation(), to->getAllocation(), type);
    }

    bool moveInput(LInstruction* ins, LiveInterval* from, LiveInterval* to, LDefinition::Type type) {
        if (*from->getAllocation() == *to->getAllocation())
            return true;
        LMoveGroup* moves = getInputMoveGroup(ins);
        return addMove(moves, from, to, type);
    }

    bool moveAfter(LInstruction* ins, LiveInterval* from, LiveInterval* to, LDefinition::Type type) {
        if (*from->getAllocation() == *to->getAllocation())
            return true;
        LMoveGroup* moves = getMoveGroupAfter(ins);
        return addMove(moves, from, to, type);
    }

    bool moveAtExit(LBlock* block, LiveInterval* from, LiveInterval* to, LDefinition::Type type) {
        if (*from->getAllocation() == *to->getAllocation())
            return true;
        LMoveGroup* moves = block->getExitMoveGroup(alloc());
        return addMove(moves, from, to, type);
    }

    bool moveAtEntry(LBlock* block, LiveInterval* from, LiveInterval* to, LDefinition::Type type) {
        if (*from->getAllocation() == *to->getAllocation())
            return true;
        LMoveGroup* moves = block->getEntryMoveGroup(alloc());
        return addMove(moves, from, to, type);
    }

    size_t findFirstNonCallSafepoint(CodePosition from) const
    {
        size_t i = 0;
        for (; i < graph.numNonCallSafepoints(); i++) {
            const LInstruction* ins = graph.getNonCallSafepoint(i);
            if (from <= inputOf(ins))
                break;
        }
        return i;
    }

    void addLiveRegistersForInterval(VirtualRegister* reg, LiveInterval* interval)
    {
        // Fill in the live register sets for all non-call safepoints.
        LAllocation* a = interval->getAllocation();
        if (!a->isRegister())
            return;

        // Don't add output registers to the safepoint.
        CodePosition start = interval->start();
        if (interval->index() == 0 && !reg->isTemp()) {
#ifdef CHECK_OSIPOINT_REGISTERS
            // We don't add the output register to the safepoint,
            // but it still might get added as one of the inputs.
            // So eagerly add this reg to the safepoint clobbered registers.
            if (reg->ins()->isInstruction()) {
                if (LSafepoint* safepoint = reg->ins()->toInstruction()->safepoint())
                    safepoint->addClobberedRegister(a->toRegister());
            }
#endif
            start = start.next();
        }

        size_t i = findFirstNonCallSafepoint(start);
        for (; i < graph.numNonCallSafepoints(); i++) {
            LInstruction* ins = graph.getNonCallSafepoint(i);
            CodePosition pos = inputOf(ins);

            // Safepoints are sorted, so we can shortcut out of this loop
            // if we go out of range.
            if (interval->end() <= pos)
                break;

            if (!interval->covers(pos))
                continue;

            LSafepoint* safepoint = ins->safepoint();
            safepoint->addLiveRegister(a->toRegister());

#ifdef CHECK_OSIPOINT_REGISTERS
            if (reg->isTemp())
                safepoint->addClobberedRegister(a->toRegister());
#endif
        }
    }

    // Finds the first safepoint that is within range of an interval.
    size_t findFirstSafepoint(const LiveInterval* interval, size_t startFrom) const
    {
        size_t i = startFrom;
        for (; i < graph.numSafepoints(); i++) {
            LInstruction* ins = graph.getSafepoint(i);
            if (interval->start() <= inputOf(ins))
                break;
        }
        return i;
    }

    void dumpVregs();
};

} // namespace jit
} // namespace js

#endif /* jit_LiveRangeAllocator_h */
