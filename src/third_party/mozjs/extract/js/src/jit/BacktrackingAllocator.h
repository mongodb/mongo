/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BacktrackingAllocator_h
#define jit_BacktrackingAllocator_h

#include "mozilla/Array.h"
#include "mozilla/Attributes.h"

#include "ds/PriorityQueue.h"
#include "ds/SplayTree.h"
#include "jit/RegisterAllocator.h"
#include "jit/StackSlotAllocator.h"

// Gives better traces in Nightly/debug builds (could be EARLY_BETA_OR_EARLIER)
#if defined(NIGHTLY_BUILD) || defined(DEBUG)
#define AVOID_INLINE_FOR_DEBUGGING MOZ_NEVER_INLINE
#else
#define AVOID_INLINE_FOR_DEBUGGING
#endif

// Backtracking priority queue based register allocator based on that described
// in the following blog post:
//
// http://blog.llvm.org/2011/09/greedy-register-allocation-in-llvm-30.html

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

    MOZ_MUST_USE bool merge(const Requirement& newRequirement) {
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

    void dump() const;

  private:
    Kind kind_;
    LAllocation allocation_;
    CodePosition position_;
};

struct UsePosition : public TempObject,
                     public InlineForwardListNode<UsePosition>
{
  private:
    // Packed LUse* with a copy of the LUse::Policy value, in order to avoid
    // making cache misses while reaching out to the policy value.
    uintptr_t use_;

    void setUse(LUse* use) {
        // Assert that we can safely pack the LUse policy in the last 2 bits of
        // the LUse pointer.
        static_assert((LUse::ANY | LUse::REGISTER | LUse::FIXED | LUse::KEEPALIVE) <= 0x3,
                      "Cannot pack the LUse::Policy value on 32 bits architectures.");

        // RECOVERED_INPUT is used by snapshots and ignored when building the
        // liveness information. Thus we can safely assume that no such value
        // would be seen.
        MOZ_ASSERT(use->policy() != LUse::RECOVERED_INPUT);
        use_ = uintptr_t(use) | (use->policy() & 0x3);
    }

  public:
    CodePosition pos;

    LUse* use() const {
        return reinterpret_cast<LUse*>(use_ & ~0x3);
    }

    LUse::Policy usePolicy() const {
        LUse::Policy policy = LUse::Policy(use_ & 0x3);
        MOZ_ASSERT(use()->policy() == policy);
        return policy;
    }

    UsePosition(LUse* use, CodePosition pos) :
        pos(pos)
    {
        // Verify that the usedAtStart() flag is consistent with the
        // subposition. For now ignore fixed registers, because they
        // are handled specially around calls.
        MOZ_ASSERT_IF(!use->isFixedRegister(),
                      pos.subpos() == (use->usedAtStart()
                                       ? CodePosition::INPUT
                                       : CodePosition::OUTPUT));
        setUse(use);
    }
};

typedef InlineForwardListIterator<UsePosition> UsePositionIterator;

// Backtracking allocator data structures overview.
//
// LiveRange: A continuous range of positions where a virtual register is live.
// LiveBundle: A set of LiveRanges which do not overlap.
// VirtualRegister: A set of all LiveRanges used for some LDefinition.
//
// The allocator first performs a liveness ananlysis on the LIR graph which
// constructs LiveRanges for each VirtualRegister, determining where the
// registers are live.
//
// The ranges are then bundled together according to heuristics, and placed on
// the allocation queue.
//
// As bundles are removed from the allocation queue, we attempt to find a
// physical register or stack slot allocation for all ranges in the removed
// bundle, possibly evicting already-allocated bundles. See processBundle()
// for details.
//
// If we are not able to allocate a bundle, it is split according to heuristics
// into two or more smaller bundles which cover all the ranges of the original.
// These smaller bundles are then allocated independently.

class LiveBundle;

class LiveRange : public TempObject
{
  public:
    // Linked lists are used to keep track of the ranges in each LiveBundle and
    // VirtualRegister. Since a LiveRange may be in two lists simultaneously, use
    // these auxiliary classes to keep things straight.
    class BundleLink : public InlineForwardListNode<BundleLink> {};
    class RegisterLink : public InlineForwardListNode<RegisterLink> {};

    typedef InlineForwardListIterator<BundleLink> BundleLinkIterator;
    typedef InlineForwardListIterator<RegisterLink> RegisterLinkIterator;

    // Links in the lists in LiveBundle and VirtualRegister.
    BundleLink bundleLink;
    RegisterLink registerLink;

    static LiveRange* get(BundleLink* link) {
        return reinterpret_cast<LiveRange*>(reinterpret_cast<uint8_t*>(link) -
                                            offsetof(LiveRange, bundleLink));
    }
    static LiveRange* get(RegisterLink* link) {
        return reinterpret_cast<LiveRange*>(reinterpret_cast<uint8_t*>(link) -
                                            offsetof(LiveRange, registerLink));
    }

    struct Range
    {
        // The beginning of this range, inclusive.
        CodePosition from;

        // The end of this range, exclusive.
        CodePosition to;

        Range() {}

        Range(CodePosition from, CodePosition to)
          : from(from), to(to)
        {
            MOZ_ASSERT(!empty());
        }

        bool empty() {
            MOZ_ASSERT(from <= to);
            return from == to;
        }
    };

  private:
    // The virtual register this range is for, or zero if this does not have a
    // virtual register (for example, it is in the callRanges bundle).
    uint32_t vreg_;

    // The bundle containing this range, null if liveness information is being
    // constructed and we haven't started allocating bundles yet.
    LiveBundle* bundle_;

    // The code positions in this range.
    Range range_;

    // All uses of the virtual register in this range, ordered by location.
    InlineForwardList<UsePosition> uses_;

    // Total spill weight that calculate from all the uses' policy. Because the
    // use's policy can't be changed after initialization, we can update the
    // weight whenever a use is added to or remove from this range. This way, we
    // don't need to iterate all the uses every time computeSpillWeight() is
    // called.
    size_t usesSpillWeight_;

    // Number of uses that have policy LUse::FIXED.
    uint32_t numFixedUses_;

    // Whether this range contains the virtual register's definition.
    bool hasDefinition_;

    LiveRange(uint32_t vreg, Range range)
      : vreg_(vreg), bundle_(nullptr), range_(range), usesSpillWeight_(0),
        numFixedUses_(0), hasDefinition_(false)

    {
        MOZ_ASSERT(!range.empty());
    }

    void noteAddedUse(UsePosition* use);
    void noteRemovedUse(UsePosition* use);

  public:
    static LiveRange* FallibleNew(TempAllocator& alloc, uint32_t vreg,
                                  CodePosition from, CodePosition to)
    {
        return new(alloc.fallible()) LiveRange(vreg, Range(from, to));
    }

    uint32_t vreg() const {
        MOZ_ASSERT(hasVreg());
        return vreg_;
    }
    bool hasVreg() const {
        return vreg_ != 0;
    }

    LiveBundle* bundle() const {
        return bundle_;
    }

    CodePosition from() const {
        return range_.from;
    }
    CodePosition to() const {
        return range_.to;
    }
    bool covers(CodePosition pos) const {
        return pos >= from() && pos < to();
    }

    // Whether this range wholly contains other.
    bool contains(LiveRange* other) const;

    // Intersect this range with other, returning the subranges of this
    // that are before, inside, or after other.
    void intersect(LiveRange* other, Range* pre, Range* inside, Range* post) const;

    // Whether this range has any intersection with other.
    bool intersects(LiveRange* other) const;

    UsePositionIterator usesBegin() const {
        return uses_.begin();
    }
    UsePosition* lastUse() const {
        return uses_.back();
    }
    bool hasUses() const {
        return !!usesBegin();
    }
    UsePosition* popUse();

    bool hasDefinition() const {
        return hasDefinition_;
    }

    void setFrom(CodePosition from) {
        range_.from = from;
        MOZ_ASSERT(!range_.empty());
    }
    void setTo(CodePosition to) {
        range_.to = to;
        MOZ_ASSERT(!range_.empty());
    }

    void setBundle(LiveBundle* bundle) {
        bundle_ = bundle;
    }

    void addUse(UsePosition* use);
    void distributeUses(LiveRange* other);

    void setHasDefinition() {
        MOZ_ASSERT(!hasDefinition_);
        hasDefinition_ = true;
    }

    size_t usesSpillWeight() {
        return usesSpillWeight_;
    }
    uint32_t numFixedUses() {
        return numFixedUses_;
    }

#ifdef JS_JITSPEW
    // Return a string describing this range.
    UniqueChars toString() const;
#endif

    // Comparator for use in range splay trees.
    static int compare(LiveRange* v0, LiveRange* v1) {
        // LiveRange includes 'from' but excludes 'to'.
        if (v0->to() <= v1->from())
            return -1;
        if (v0->from() >= v1->to())
            return 1;
        return 0;
    }
};

// Tracks information about bundles that should all be spilled to the same
// physical location. At the beginning of allocation, each bundle has its own
// spill set. As bundles are split, the new smaller bundles continue to use the
// same spill set.
class SpillSet : public TempObject
{
    // All bundles with this spill set which have been spilled. All bundles in
    // this list will be given the same physical slot.
    Vector<LiveBundle*, 1, JitAllocPolicy> list_;

    explicit SpillSet(TempAllocator& alloc)
      : list_(alloc)
    { }

  public:
    static SpillSet* New(TempAllocator& alloc) {
        return new(alloc) SpillSet(alloc);
    }

    MOZ_MUST_USE bool addSpilledBundle(LiveBundle* bundle) {
        return list_.append(bundle);
    }
    size_t numSpilledBundles() const {
        return list_.length();
    }
    LiveBundle* spilledBundle(size_t i) const {
        return list_[i];
    }

    void setAllocation(LAllocation alloc);
};

// A set of live ranges which are all pairwise disjoint. The register allocator
// attempts to find allocations for an entire bundle, and if it fails the
// bundle will be broken into smaller ones which are allocated independently.
class LiveBundle : public TempObject
{
    // Set to use if this bundle or one it is split into is spilled.
    SpillSet* spill_;

    // All the ranges in this set, ordered by location.
    InlineForwardList<LiveRange::BundleLink> ranges_;

    // Allocation to use for ranges in this set, bogus if unallocated or spilled
    // and not yet given a physical stack slot.
    LAllocation alloc_;

    // Bundle which entirely contains this one and has no register uses. This
    // may or may not be spilled by the allocator, but it can be spilled and
    // will not be split.
    LiveBundle* spillParent_;

    LiveBundle(SpillSet* spill, LiveBundle* spillParent)
      : spill_(spill), spillParent_(spillParent)
    { }

  public:
    static LiveBundle* FallibleNew(TempAllocator& alloc, SpillSet* spill, LiveBundle* spillParent)
    {
        return new(alloc.fallible()) LiveBundle(spill, spillParent);
    }

    SpillSet* spillSet() const {
        return spill_;
    }
    void setSpillSet(SpillSet* spill) {
        spill_ = spill;
    }

    LiveRange::BundleLinkIterator rangesBegin() const {
        return ranges_.begin();
    }
    bool hasRanges() const {
        return !!rangesBegin();
    }
    LiveRange* firstRange() const {
        return LiveRange::get(*rangesBegin());
    }
    LiveRange* lastRange() const {
        return LiveRange::get(ranges_.back());
    }
    LiveRange* rangeFor(CodePosition pos) const;
    void removeRange(LiveRange* range);
    void removeRangeAndIncrementIterator(LiveRange::BundleLinkIterator& iter) {
        ranges_.removeAndIncrement(iter);
    }
    void addRange(LiveRange* range);
    MOZ_MUST_USE bool addRange(TempAllocator& alloc, uint32_t vreg,
                               CodePosition from, CodePosition to);
    MOZ_MUST_USE bool addRangeAndDistributeUses(TempAllocator& alloc, LiveRange* oldRange,
                                                CodePosition from, CodePosition to);
    LiveRange* popFirstRange();
#ifdef DEBUG
    size_t numRanges() const;
#endif

    LAllocation allocation() const {
        return alloc_;
    }
    void setAllocation(LAllocation alloc) {
        alloc_ = alloc;
    }

    LiveBundle* spillParent() const {
        return spillParent_;
    }

#ifdef JS_JITSPEW
    // Return a string describing this bundle.
    UniqueChars toString() const;
#endif
};

// Information about the allocation for a virtual register.
class VirtualRegister
{
    // Instruction which defines this register.
    LNode* ins_;

    // Definition in the instruction for this register.
    LDefinition* def_;

    // All live ranges for this register. These may overlap each other, and are
    // ordered by their start position.
    InlineForwardList<LiveRange::RegisterLink> ranges_;

    // Whether def_ is a temp or an output.
    bool isTemp_;

    // Whether this vreg is an input for some phi. This use is not reflected in
    // any range on the vreg.
    bool usedByPhi_;

    // If this register's definition is MUST_REUSE_INPUT, whether a copy must
    // be introduced before the definition that relaxes the policy.
    bool mustCopyInput_;

    void operator=(const VirtualRegister&) = delete;
    VirtualRegister(const VirtualRegister&) = delete;

  public:
    explicit VirtualRegister()
    {
        // Note: This class is zeroed before it is constructed.
    }

    void init(LNode* ins, LDefinition* def, bool isTemp) {
        MOZ_ASSERT(!ins_);
        ins_ = ins;
        def_ = def;
        isTemp_ = isTemp;
    }

    LNode* ins() const {
        return ins_;
    }
    LDefinition* def() const {
        return def_;
    }
    LDefinition::Type type() const {
        return def()->type();
    }
    uint32_t vreg() const {
        return def()->virtualRegister();
    }
    bool isCompatible(const AnyRegister& r) const {
        return def_->isCompatibleReg(r);
    }
    bool isCompatible(const VirtualRegister& vr) const {
        return def_->isCompatibleDef(*vr.def_);
    }
    bool isTemp() const {
        return isTemp_;
    }

    void setUsedByPhi() {
        usedByPhi_ = true;
    }
    bool usedByPhi() {
        return usedByPhi_;
    }

    void setMustCopyInput() {
        mustCopyInput_ = true;
    }
    bool mustCopyInput() {
        return mustCopyInput_;
    }

    LiveRange::RegisterLinkIterator rangesBegin() const {
        return ranges_.begin();
    }
    LiveRange::RegisterLinkIterator rangesBegin(LiveRange* range) const {
        return ranges_.begin(&range->registerLink);
    }
    bool hasRanges() const {
        return !!rangesBegin();
    }
    LiveRange* firstRange() const {
        return LiveRange::get(*rangesBegin());
    }
    LiveRange* lastRange() const {
        return LiveRange::get(ranges_.back());
    }
    LiveRange* rangeFor(CodePosition pos, bool preferRegister = false) const;
    void removeRange(LiveRange* range);
    void addRange(LiveRange* range);

    void removeRangeAndIncrement(LiveRange::RegisterLinkIterator& iter) {
        ranges_.removeAndIncrement(iter);
    }

    LiveBundle* firstBundle() const {
        return firstRange()->bundle();
    }

    MOZ_MUST_USE bool addInitialRange(TempAllocator& alloc, CodePosition from, CodePosition to,
                                      size_t* numRanges);
    void addInitialUse(UsePosition* use);
    void setInitialDefinition(CodePosition from);
};

// A sequence of code positions, for tellings BacktrackingAllocator::splitAt
// where to split.
typedef js::Vector<CodePosition, 4, SystemAllocPolicy> SplitPositionVector;

class BacktrackingAllocator : protected RegisterAllocator
{
    friend class C1Spewer;
    friend class JSONSpewer;

    // This flag is set when testing new allocator modifications.
    bool testbed;

    BitSet* liveIn;
    FixedList<VirtualRegister> vregs;

    // Allocation state.
    StackSlotAllocator stackSlotAllocator;

    // Priority queue element: a bundle and the associated priority.
    struct QueueItem
    {
        LiveBundle* bundle;

        QueueItem(LiveBundle* bundle, size_t priority)
          : bundle(bundle), priority_(priority)
        {}

        static size_t priority(const QueueItem& v) {
            return v.priority_;
        }

      private:
        size_t priority_;
    };

    PriorityQueue<QueueItem, QueueItem, 0, SystemAllocPolicy> allocationQueue;

    typedef SplayTree<LiveRange*, LiveRange> LiveRangeSet;

    // Each physical register is associated with the set of ranges over which
    // that register is currently allocated.
    struct PhysicalRegister {
        bool allocatable;
        AnyRegister reg;
        LiveRangeSet allocations;

        PhysicalRegister() : allocatable(false) {}
    };
    mozilla::Array<PhysicalRegister, AnyRegister::Total> registers;

    // Ranges of code which are considered to be hot, for which good allocation
    // should be prioritized.
    LiveRangeSet hotcode;

    struct CallRange : public TempObject, public InlineListNode<CallRange> {
        LiveRange::Range range;

        CallRange(CodePosition from, CodePosition to)
          : range(from, to)
        {}

        // Comparator for use in splay tree.
        static int compare(CallRange* v0, CallRange* v1) {
            if (v0->range.to <= v1->range.from)
                return -1;
            if (v0->range.from >= v1->range.to)
                return 1;
            return 0;
        }
    };

    // Ranges where all registers must be spilled due to call instructions.
    typedef InlineList<CallRange> CallRangeList;
    CallRangeList callRangesList;
    SplayTree<CallRange*, CallRange> callRanges;

    // Information about an allocated stack slot.
    struct SpillSlot : public TempObject, public InlineForwardListNode<SpillSlot> {
        LStackSlot alloc;
        LiveRangeSet allocated;

        SpillSlot(uint32_t slot, LifoAlloc* alloc)
          : alloc(slot), allocated(alloc)
        {}
    };
    typedef InlineForwardList<SpillSlot> SpillSlotList;

    // All allocated slots of each width.
    SpillSlotList normalSlots, doubleSlots, quadSlots;

    Vector<LiveBundle*, 4, SystemAllocPolicy> spilledBundles;

  public:
    BacktrackingAllocator(MIRGenerator* mir, LIRGenerator* lir, LIRGraph& graph, bool testbed)
      : RegisterAllocator(mir, lir, graph),
        testbed(testbed),
        liveIn(nullptr),
        callRanges(nullptr)
    { }

    MOZ_MUST_USE bool go();

    static size_t SpillWeightFromUsePolicy(LUse::Policy policy) {
        switch (policy) {
        case LUse::ANY:
            return 1000;

        case LUse::REGISTER:
        case LUse::FIXED:
            return 2000;

        default:
            return 0;
        }
    }

  private:

    typedef Vector<LiveRange*, 4, SystemAllocPolicy> LiveRangeVector;
    typedef Vector<LiveBundle*, 4, SystemAllocPolicy> LiveBundleVector;

    // Liveness methods.
    MOZ_MUST_USE bool init();
    MOZ_MUST_USE bool buildLivenessInfo();

    MOZ_MUST_USE bool addInitialFixedRange(AnyRegister reg, CodePosition from, CodePosition to);

    VirtualRegister& vreg(const LDefinition* def) {
        return vregs[def->virtualRegister()];
    }
    VirtualRegister& vreg(const LAllocation* alloc) {
        MOZ_ASSERT(alloc->isUse());
        return vregs[alloc->toUse()->virtualRegister()];
    }

    // Allocation methods.
    MOZ_MUST_USE bool tryMergeBundles(LiveBundle* bundle0, LiveBundle* bundle1);
    MOZ_MUST_USE bool tryMergeReusedRegister(VirtualRegister& def, VirtualRegister& input);
    MOZ_MUST_USE bool mergeAndQueueRegisters();
    MOZ_MUST_USE bool tryAllocateFixed(LiveBundle* bundle, Requirement requirement,
                                       bool* success, bool* pfixed, LiveBundleVector& conflicting);
    MOZ_MUST_USE bool tryAllocateNonFixed(LiveBundle* bundle, Requirement requirement,
                                          Requirement hint, bool* success, bool* pfixed,
                                          LiveBundleVector& conflicting);
    MOZ_MUST_USE bool processBundle(MIRGenerator* mir, LiveBundle* bundle);
    MOZ_MUST_USE bool computeRequirement(LiveBundle* bundle, Requirement *prequirement,
                                         Requirement *phint);
    MOZ_MUST_USE bool tryAllocateRegister(PhysicalRegister& r, LiveBundle* bundle, bool* success,
                                          bool* pfixed, LiveBundleVector& conflicting);
    MOZ_MUST_USE bool evictBundle(LiveBundle* bundle);
    MOZ_MUST_USE bool splitAndRequeueBundles(LiveBundle* bundle,
                                             const LiveBundleVector& newBundles);
    MOZ_MUST_USE bool spill(LiveBundle* bundle);
    AVOID_INLINE_FOR_DEBUGGING MOZ_MUST_USE bool tryAllocatingRegistersForSpillBundles();

    bool isReusedInput(LUse* use, LNode* ins, bool considerCopy);
    bool isRegisterUse(UsePosition* use, LNode* ins, bool considerCopy = false);
    bool isRegisterDefinition(LiveRange* range);
    MOZ_MUST_USE bool pickStackSlot(SpillSet* spill);
    MOZ_MUST_USE bool insertAllRanges(LiveRangeSet& set, LiveBundle* bundle);

    // Reification methods.
    AVOID_INLINE_FOR_DEBUGGING MOZ_MUST_USE bool pickStackSlots();
    AVOID_INLINE_FOR_DEBUGGING MOZ_MUST_USE bool resolveControlFlow();
    AVOID_INLINE_FOR_DEBUGGING MOZ_MUST_USE bool reifyAllocations();
    AVOID_INLINE_FOR_DEBUGGING MOZ_MUST_USE bool populateSafepoints();
    AVOID_INLINE_FOR_DEBUGGING MOZ_MUST_USE bool annotateMoveGroups();
    AVOID_INLINE_FOR_DEBUGGING MOZ_MUST_USE bool deadRange(LiveRange* range);
    size_t findFirstNonCallSafepoint(CodePosition from);
    size_t findFirstSafepoint(CodePosition pos, size_t startFrom);
    void addLiveRegistersForRange(VirtualRegister& reg, LiveRange* range);

    MOZ_MUST_USE bool addMove(LMoveGroup* moves, LiveRange* from, LiveRange* to,
                              LDefinition::Type type) {
        LAllocation fromAlloc = from->bundle()->allocation();
        LAllocation toAlloc = to->bundle()->allocation();
        MOZ_ASSERT(fromAlloc != toAlloc);
        return moves->add(fromAlloc, toAlloc, type);
    }

    MOZ_MUST_USE bool moveInput(LInstruction* ins, LiveRange* from, LiveRange* to,
                                LDefinition::Type type) {
        if (from->bundle()->allocation() == to->bundle()->allocation())
            return true;
        LMoveGroup* moves = getInputMoveGroup(ins);
        return addMove(moves, from, to, type);
    }

    MOZ_MUST_USE bool moveAfter(LInstruction* ins, LiveRange* from, LiveRange* to,
                                LDefinition::Type type) {
        if (from->bundle()->allocation() == to->bundle()->allocation())
            return true;
        LMoveGroup* moves = getMoveGroupAfter(ins);
        return addMove(moves, from, to, type);
    }

    MOZ_MUST_USE bool moveAtExit(LBlock* block, LiveRange* from, LiveRange* to,
                                 LDefinition::Type type) {
        if (from->bundle()->allocation() == to->bundle()->allocation())
            return true;
        LMoveGroup* moves = block->getExitMoveGroup(alloc());
        return addMove(moves, from, to, type);
    }

    MOZ_MUST_USE bool moveAtEntry(LBlock* block, LiveRange* from, LiveRange* to,
                                  LDefinition::Type type) {
        if (from->bundle()->allocation() == to->bundle()->allocation())
            return true;
        LMoveGroup* moves = block->getEntryMoveGroup(alloc());
        return addMove(moves, from, to, type);
    }

    // Debugging methods.
    void dumpAllocations();

    struct PrintLiveRange;

    bool minimalDef(LiveRange* range, LNode* ins);
    bool minimalUse(LiveRange* range, UsePosition* use);
    bool minimalBundle(LiveBundle* bundle, bool* pfixed = nullptr);

    // Heuristic methods.

    size_t computePriority(LiveBundle* bundle);
    size_t computeSpillWeight(LiveBundle* bundle);

    size_t maximumSpillWeight(const LiveBundleVector& bundles);

    MOZ_MUST_USE bool chooseBundleSplit(LiveBundle* bundle, bool fixed, LiveBundle* conflict);

    MOZ_MUST_USE bool splitAt(LiveBundle* bundle, const SplitPositionVector& splitPositions);
    MOZ_MUST_USE bool trySplitAcrossHotcode(LiveBundle* bundle, bool* success);
    MOZ_MUST_USE bool trySplitAfterLastRegisterUse(LiveBundle* bundle, LiveBundle* conflict,
                                                   bool* success);
    MOZ_MUST_USE bool trySplitBeforeFirstRegisterUse(LiveBundle* bundle, LiveBundle* conflict,
                                                     bool* success);
    MOZ_MUST_USE bool splitAcrossCalls(LiveBundle* bundle);

    bool compilingWasm() {
        return mir->info().compilingWasm();
    }

    void dumpVregs();
};

} // namespace jit
} // namespace js

#endif /* jit_BacktrackingAllocator_h */
