/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BacktrackingAllocator_h
#define jit_BacktrackingAllocator_h

#include "mozilla/Array.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include "ds/AvlTree.h"
#include "ds/PriorityQueue.h"
#include "jit/RegisterAllocator.h"
#include "jit/SparseBitSet.h"
#include "jit/StackSlotAllocator.h"

// Gives better traces in Nightly/debug builds (could be EARLY_BETA_OR_EARLIER)
#if defined(NIGHTLY_BUILD) || defined(DEBUG)
#  define AVOID_INLINE_FOR_DEBUGGING MOZ_NEVER_INLINE
#else
#  define AVOID_INLINE_FOR_DEBUGGING
#endif

// Backtracking priority queue based register allocator based on that described
// in the following blog post:
//
// http://blog.llvm.org/2011/09/greedy-register-allocation-in-llvm-30.html

namespace js {
namespace jit {

class Requirement {
 public:
  enum Kind { NONE, REGISTER, FIXED };

  Requirement() : kind_(NONE) {}

  explicit Requirement(Kind kind) : kind_(kind) {
    // FIXED has a dedicated constructor.
    MOZ_ASSERT(kind != FIXED);
  }

  explicit Requirement(LAllocation fixed) : kind_(FIXED), allocation_(fixed) {
    MOZ_ASSERT(!fixed.isBogus() && !fixed.isUse());
  }

  Kind kind() const { return kind_; }

  LAllocation allocation() const {
    MOZ_ASSERT(!allocation_.isBogus() && !allocation_.isUse());
    return allocation_;
  }

  [[nodiscard]] bool merge(const Requirement& newRequirement) {
    // Merge newRequirement with any existing requirement, returning false
    // if the new and old requirements conflict.

    if (newRequirement.kind() == Requirement::FIXED) {
      if (kind() == Requirement::FIXED) {
        return newRequirement.allocation() == allocation();
      }
      *this = newRequirement;
      return true;
    }

    MOZ_ASSERT(newRequirement.kind() == Requirement::REGISTER);
    if (kind() == Requirement::FIXED) {
      return allocation().isAnyRegister();
    }

    *this = newRequirement;
    return true;
  }

 private:
  Kind kind_;
  LAllocation allocation_;
};

struct UsePosition : public TempObject,
                     public InlineForwardListNode<UsePosition> {
 private:
  // A UsePosition is an LUse* with a CodePosition.  UsePosition also has an
  // optimization that allows access to the associated LUse::Policy without
  // dereferencing memory: the policy is encoded in the low bits of the LUse*.
  //
  // Note however that because LUse* is uintptr_t-aligned, on 32-bit systems
  // there are only 4 encodable values, for more than 4 use policies; in that
  // case we allocate the common LUse::ANY, LUse::REGISTER, and LUse::FIXED use
  // policies to tags, and use tag 0x3 to indicate that dereferencing the LUse
  // is necessary to get the policy (KEEPALIVE or STACK, in that case).
  uintptr_t use_;
  static_assert(LUse::ANY < 0x3,
                "LUse::ANY can be represented in low tag on 32-bit systems");
  static_assert(LUse::REGISTER < 0x3,
                "LUse::REGISTER can be represented in tag on 32-bit systems");
  static_assert(LUse::FIXED < 0x3,
                "LUse::FIXED can be represented in tag on 32-bit systems");

  static constexpr uintptr_t PolicyMask = sizeof(uintptr_t) - 1;
  static constexpr uintptr_t UseMask = ~PolicyMask;

  void setUse(LUse* use) {
    // RECOVERED_INPUT is used by snapshots and ignored when building the
    // liveness information. Thus we can safely assume that no such value
    // would be seen.
    MOZ_ASSERT(use->policy() != LUse::RECOVERED_INPUT);

    uintptr_t policyBits = use->policy();
#ifndef JS_64BIT
    // On a 32-bit machine, LUse::KEEPALIVE and LUse::STACK are accessed by
    // dereferencing the use pointer.
    if (policyBits >= PolicyMask) {
      policyBits = PolicyMask;
    }
#endif
    use_ = uintptr_t(use) | policyBits;
    MOZ_ASSERT(use->policy() == usePolicy());
  }

 public:
  CodePosition pos;

  LUse* use() const { return reinterpret_cast<LUse*>(use_ & UseMask); }

  LUse::Policy usePolicy() const {
    uintptr_t bits = use_ & PolicyMask;
#ifndef JS_64BIT
    // On 32-bit machines, reach out to memory if it's LUse::KEEPALIVE or
    // LUse::STACK.
    if (bits == PolicyMask) {
      return use()->policy();
    }
#endif
    LUse::Policy policy = LUse::Policy(bits);
    MOZ_ASSERT(use()->policy() == policy);
    return policy;
  }

  UsePosition(LUse* use, CodePosition pos) : pos(pos) {
    // Verify that the usedAtStart() flag is consistent with the
    // subposition. For now ignore fixed registers, because they
    // are handled specially around calls.
    MOZ_ASSERT_IF(!use->isFixedRegister(),
                  pos.subpos() == (use->usedAtStart() ? CodePosition::INPUT
                                                      : CodePosition::OUTPUT));
    setUse(use);
  }
};

using UsePositionIterator = InlineForwardListIterator<UsePosition>;

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
class VirtualRegister;

class LiveRange : public TempObject, public InlineForwardListNode<LiveRange> {
 public:
  struct Range {
    // The beginning of this range, inclusive.
    CodePosition from;

    // The end of this range, exclusive.
    CodePosition to;

    Range() = default;

    Range(CodePosition from, CodePosition to) : from(from), to(to) {
      MOZ_ASSERT(!empty());
    }

    bool empty() {
      MOZ_ASSERT(from <= to);
      return from == to;
    }
  };

 private:
  // The virtual register this range is for, or nullptr if this does not have a
  // virtual register (for example, it is in the callRanges bundle).
  VirtualRegister* vreg_;

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

  LiveRange(VirtualRegister* vreg, Range range)
      : vreg_(vreg),
        bundle_(nullptr),
        range_(range),
        usesSpillWeight_(0),
        numFixedUses_(0),
        hasDefinition_(false)

  {
    MOZ_ASSERT(!range.empty());
  }

  void noteAddedUse(UsePosition* use);
  void noteRemovedUse(UsePosition* use);

 public:
  static LiveRange* FallibleNew(TempAllocator& alloc, VirtualRegister* vreg,
                                CodePosition from, CodePosition to) {
    return new (alloc.fallible()) LiveRange(vreg, Range(from, to));
  }

  VirtualRegister& vreg() const {
    MOZ_ASSERT(hasVreg());
    return *vreg_;
  }
  bool hasVreg() const { return vreg_ != nullptr; }

  LiveBundle* bundle() const { return bundle_; }

  CodePosition from() const { return range_.from; }
  CodePosition to() const { return range_.to; }
  bool covers(CodePosition pos) const { return pos >= from() && pos < to(); }

  // Whether this range wholly contains other.
  bool contains(LiveRange* other) const;

  // Intersect this range with other, returning the subranges of this
  // that are before, inside, or after other.
  void intersect(LiveRange* other, Range* pre, Range* inside,
                 Range* post) const;

  // Whether this range has any intersection with other.
  bool intersects(LiveRange* other) const;

  UsePositionIterator usesBegin() const { return uses_.begin(); }
  UsePosition* lastUse() const { return uses_.back(); }
  bool hasUses() const { return !!usesBegin(); }
  UsePosition* popUse();

  bool hasDefinition() const { return hasDefinition_; }

  void setFrom(CodePosition from) {
    range_.from = from;
    MOZ_ASSERT(!range_.empty());
  }
  void setTo(CodePosition to) {
    range_.to = to;
    MOZ_ASSERT(!range_.empty());
  }

  void setBundle(LiveBundle* bundle) { bundle_ = bundle; }

  void addUse(UsePosition* use);

  void tryToMoveDefAndUsesInto(LiveRange* other);
  void moveAllUsesToTheEndOf(LiveRange* other);

  void setHasDefinition() {
    MOZ_ASSERT(!hasDefinition_);
    hasDefinition_ = true;
  }

  size_t usesSpillWeight() { return usesSpillWeight_; }
  uint32_t numFixedUses() { return numFixedUses_; }

#ifdef JS_JITSPEW
  // Return a string describing this range.
  UniqueChars toString() const;
#endif

  // Comparator for use in AVL trees.
  static int compare(LiveRange* v0, LiveRange* v1) {
    // The denoted range includes 'from' but excludes 'to'.
    if (v0->to() <= v1->from()) {
      return -1;
    }
    if (v0->from() >= v1->to()) {
      return 1;
    }
    return 0;
  }
};

// LiveRangePlus is a simple wrapper around a LiveRange*.  It caches the
// LiveRange*'s `.range_.from` and `.range_.to` CodePositions.  The only
// purpose of this is to avoid some cache misses that would otherwise occur
// when comparing those fields in an AvlTree<LiveRange*, ..>.  This measurably
// speeds up the allocator in some cases.  See bug 1814204.

class LiveRangePlus {
  // The LiveRange we're wrapping.
  LiveRange* liveRange_;
  // Cached versions of liveRange_->range_.from and lr->range_.to
  CodePosition from_;
  CodePosition to_;

 public:
  explicit LiveRangePlus(LiveRange* lr)
      : liveRange_(lr), from_(lr->from()), to_(lr->to()) {}
  LiveRangePlus() : liveRange_(nullptr) {}
  ~LiveRangePlus() {
    MOZ_ASSERT(liveRange_ ? from_ == liveRange_->from()
                          : from_ == CodePosition());
    MOZ_ASSERT(liveRange_ ? to_ == liveRange_->to() : to_ == CodePosition());
  }

  LiveRange* liveRange() const { return liveRange_; }

  // Comparator for use in AVL trees.
  static int compare(const LiveRangePlus& lrp0, const LiveRangePlus& lrp1) {
    // The denoted range includes 'from' but excludes 'to'.
    if (lrp0.to_ <= lrp1.from_) {
      return -1;
    }
    if (lrp0.from_ >= lrp1.to_) {
      return 1;
    }
    return 0;
  }
};

// Make sure there's no alignment holes or vtable present.  Per bug 1814204,
// it's important that this structure is as small as possible.
static_assert(sizeof(LiveRangePlus) ==
              sizeof(LiveRange*) + 2 * sizeof(CodePosition));

// Tracks information about bundles that should all be spilled to the same
// physical location. At the beginning of allocation, each bundle has its own
// spill set. As bundles are split, the new smaller bundles continue to use the
// same spill set.
class SpillSet : public TempObject {
  // All bundles with this spill set which have been spilled. All bundles in
  // this list will be given the same physical slot.
  Vector<LiveBundle*, 1, JitAllocPolicy> list_;

  explicit SpillSet(TempAllocator& alloc) : list_(alloc) {}

 public:
  static SpillSet* New(TempAllocator& alloc) {
    return new (alloc) SpillSet(alloc);
  }

  [[nodiscard]] bool addSpilledBundle(LiveBundle* bundle) {
    return list_.append(bundle);
  }
  size_t numSpilledBundles() const { return list_.length(); }
  LiveBundle* spilledBundle(size_t i) const { return list_[i]; }

  void setAllocation(LAllocation alloc);
};

// A set of live ranges which are all pairwise disjoint. The register allocator
// attempts to find allocations for an entire bundle, and if it fails the
// bundle will be broken into smaller ones which are allocated independently.
class LiveBundle : public TempObject {
  // Set to use if this bundle or one it is split into is spilled.
  SpillSet* spill_;

  // All the ranges in this set, ordered by location.
  InlineForwardList<LiveRange> ranges_;

  // Allocation to use for ranges in this set, bogus if unallocated or spilled
  // and not yet given a physical stack slot.
  LAllocation alloc_;

  // Bundle which entirely contains this one and has no register uses. This
  // may or may not be spilled by the allocator, but it can be spilled and
  // will not be split.
  LiveBundle* spillParent_;

  // This is used for debug-printing bundles.  It gives them an
  // identifiable identity in the debug output, which they otherwise wouldn't
  // have.  It's also used for sorting VirtualRegister's live ranges; see the
  // comment in VirtualRegister::sortRanges.
  const uint32_t id_;

  LiveBundle(SpillSet* spill, LiveBundle* spillParent, uint32_t id)
      : spill_(spill), spillParent_(spillParent), id_(id) {}

 public:
  static LiveBundle* FallibleNew(TempAllocator& alloc, SpillSet* spill,
                                 LiveBundle* spillParent, uint32_t id) {
    return new (alloc.fallible()) LiveBundle(spill, spillParent, id);
  }

  using RangeIterator = InlineForwardListIterator<LiveRange>;

  SpillSet* spillSet() const { return spill_; }
  void setSpillSet(SpillSet* spill) { spill_ = spill; }

  RangeIterator rangesBegin() const { return ranges_.begin(); }
  RangeIterator rangesBegin(LiveRange* range) const {
    return ranges_.begin(range);
  }
  bool hasRanges() const { return !!rangesBegin(); }
  LiveRange* firstRange() const { return *rangesBegin(); }
  LiveRange* lastRange() const { return ranges_.back(); }
  LiveRange* rangeFor(CodePosition pos) const;
  void removeRange(LiveRange* range);
  void removeRangeAndIncrementIterator(RangeIterator& iter) {
    ranges_.removeAndIncrement(iter);
  }
  void removeAllRangesFromVirtualRegisters();
  void addRange(LiveRange* range, LiveRange* startAt = nullptr);
  void addRangeAtEnd(LiveRange* range);
  [[nodiscard]] bool addRangeAtEnd(TempAllocator& alloc, VirtualRegister* vreg,
                                   CodePosition from, CodePosition to);
  [[nodiscard]] bool addRangeAndDistributeUses(TempAllocator& alloc,
                                               LiveRange* oldRange,
                                               CodePosition from,
                                               CodePosition to);
  LiveRange* popFirstRange();
#ifdef DEBUG
  size_t numRanges() const;
#endif

  LAllocation allocation() const { return alloc_; }
  void setAllocation(LAllocation alloc) { alloc_ = alloc; }

  LiveBundle* spillParent() const { return spillParent_; }

  uint32_t id() const { return id_; }

#ifdef JS_JITSPEW
  // Return a string describing this bundle.
  UniqueChars toString() const;
#endif
};

// Information about a control flow edge to resolve (by inserting a move from
// predecessor range to successor range) in createMoveGroupsForControlFlowEdges.
struct ControlFlowEdge {
  // The predecessor and successor sides of this edge.
  LBlock* predecessor;
  LBlock* successor;

  // Live range that covers the successor block.
  LiveRange* successorRange;

  // Exit position of the predecessor block. This is |exitOf(predecessor)| but
  // cached here.
  CodePosition predecessorExit;

  ControlFlowEdge(LBlock* predecessor, LBlock* successor,
                  LiveRange* successorRange, CodePosition predecessorExit)
      : predecessor(predecessor),
        successor(successor),
        successorRange(successorRange),
        predecessorExit(predecessorExit) {
    MOZ_ASSERT(predecessor != successor);
  }
};
using ControlFlowEdgeVector =
    Vector<ControlFlowEdge, 8, BackgroundSystemAllocPolicy>;

// Information about the allocation for a virtual register.
class VirtualRegister {
 public:
  // Note: most virtual registers have <= 4 live ranges (at least 95% on x64 for
  // a few very large Wasm modules).
  using RangeVector = Vector<LiveRange*, 4, BackgroundSystemAllocPolicy>;
  class RangeIterator;

 private:
  // Instruction which defines this register.
  LNode* ins_ = nullptr;

  // Definition in the instruction for this register.
  LDefinition* def_ = nullptr;

  // All live ranges for this register. These may overlap each other.
  // If |rangesSorted_| is true, then these are ordered by their start position
  // in descending order.
  RangeVector ranges_;

  // Whether def_ is a temp or an output.
  bool isTemp_ = false;

  // Whether this vreg is an input for some phi. This use is not reflected in
  // any range on the vreg.
  bool usedByPhi_ = false;

  // If this register's definition is MUST_REUSE_INPUT, whether a copy must
  // be introduced before the definition that relaxes the policy.
  bool mustCopyInput_ = false;

  // If true, the |ranges_| vector is guaranteed to be sorted.
  bool rangesSorted_ = true;

  void operator=(const VirtualRegister&) = delete;
  VirtualRegister(const VirtualRegister&) = delete;

#ifdef DEBUG
  void assertRangesSorted() const;
#else
  void assertRangesSorted() const {}
#endif

  const RangeVector& sortedRanges() const {
    assertRangesSorted();
    return ranges_;
  }

 public:
  VirtualRegister() = default;

  void init(LNode* ins, LDefinition* def, bool isTemp) {
    MOZ_ASSERT(!ins_);
    ins_ = ins;
    def_ = def;
    isTemp_ = isTemp;
  }

  LNode* ins() const { return ins_; }
  LDefinition* def() const { return def_; }
  LDefinition::Type type() const { return def()->type(); }
  uint32_t vreg() const { return def()->virtualRegister(); }
  bool isCompatible(const AnyRegister& r) const {
    return def_->isCompatibleReg(r);
  }
  bool isCompatible(const VirtualRegister& vr) const {
    return def_->isCompatibleDef(*vr.def_);
  }
  bool isTemp() const { return isTemp_; }

  void setUsedByPhi() { usedByPhi_ = true; }
  bool usedByPhi() { return usedByPhi_; }

  void setMustCopyInput() { mustCopyInput_ = true; }
  bool mustCopyInput() { return mustCopyInput_; }

  bool hasRanges() const { return !ranges_.empty(); }
  LiveRange* firstRange() const {
    assertRangesSorted();
    return ranges_.back();
  }
  LiveRange* lastRange() const {
    assertRangesSorted();
    return ranges_[0];
  }
  LiveRange* rangeFor(CodePosition pos, bool preferRegister = false) const;
  void sortRanges();

  void removeFirstRange(RangeIterator& iter);
  void removeRangesForBundle(LiveBundle* bundle);
  template <typename Pred>
  void removeRangesIf(Pred&& pred);

  [[nodiscard]] bool replaceLastRangeLinear(LiveRange* old, LiveRange* newPre,
                                            LiveRange* newPost);

  [[nodiscard]] bool addRange(LiveRange* range);

  LiveBundle* firstBundle() const { return firstRange()->bundle(); }

  [[nodiscard]] bool addInitialRange(TempAllocator& alloc, CodePosition from,
                                     CodePosition to);
  void addInitialUse(UsePosition* use);
  void setInitialDefinition(CodePosition from);

  // Iterator visiting a VirtualRegister's live ranges in order of increasing
  // start position. Because the ranges are sorted in descending order, this
  // iterates over the vector from index |length - 1| to 0.
  class MOZ_RAII RangeIterator {
    const RangeVector& ranges_;
#ifdef DEBUG
    const VirtualRegister& reg_;
#endif
    // if |pos_| is 0, the iterator is done. Else, |pos_ - 1| is the index of
    // the range that will be returned by |*iter|.
    size_t pos_;

   public:
    explicit RangeIterator(const VirtualRegister& reg)
        : ranges_(reg.sortedRanges()),
#ifdef DEBUG
          reg_(reg),
#endif
          pos_(ranges_.length()) {
    }
    RangeIterator(const VirtualRegister& reg, size_t index)
        : ranges_(reg.sortedRanges()),
#ifdef DEBUG
          reg_(reg),
#endif
          pos_(index + 1) {
      MOZ_ASSERT(index < ranges_.length());
    }

#ifdef DEBUG
    ~RangeIterator() {
      // Ranges should stay sorted during iteration.
      reg_.assertRangesSorted();
    }
#endif

    RangeIterator(RangeIterator&) = delete;
    void operator=(RangeIterator&) = delete;

    bool done() const { return pos_ == 0; }

    explicit operator bool() const { return !done(); }

    LiveRange* operator*() const {
      MOZ_ASSERT(!done());
      return ranges_[pos_ - 1];
    }
    LiveRange* operator->() { return operator*(); }

    size_t index() const {
      MOZ_ASSERT(!done());
      return pos_ - 1;
    }

    void operator++(int) {
      MOZ_ASSERT(!done());
      pos_--;
    }
  };
};

// A sequence of code positions, for tellings BacktrackingAllocator::splitAt
// where to split.
using SplitPositionVector =
    js::Vector<CodePosition, 4, BackgroundSystemAllocPolicy>;

class BacktrackingAllocator : protected RegisterAllocator {
  friend class JSONSpewer;

  // Computed data
  InstructionDataMap insData;
  Vector<CodePosition, 12, SystemAllocPolicy> entryPositions;
  Vector<CodePosition, 12, SystemAllocPolicy> exitPositions;

  using VirtualRegBitSet = SparseBitSet<BackgroundSystemAllocPolicy>;
  Vector<VirtualRegBitSet, 0, JitAllocPolicy> liveIn;
  Vector<VirtualRegister, 0, JitAllocPolicy> vregs;

  // Allocation state.
  StackSlotAllocator stackSlotAllocator;

  // List of all instructions with a safepoint. The order is the same as the
  // order of the instructions in the LIR graph.
  Vector<LInstruction*, 0, JitAllocPolicy> safepoints_;

  // List of all non-call instructions with a safepoint. The order is the same
  // as the order of the instructions in the LIR graph.
  Vector<LInstruction*, 0, JitAllocPolicy> nonCallSafepoints_;

  // Priority queue element: a bundle and the associated priority.
  struct QueueItem {
    LiveBundle* bundle;

    QueueItem(LiveBundle* bundle, size_t priority)
        : bundle(bundle), priority_(priority) {}

    static bool higherPriority(const QueueItem& a, const QueueItem& b) {
      return a.priority_ > b.priority_;
    }

   private:
    size_t priority_;
  };

  PriorityQueue<QueueItem, QueueItem, 0, BackgroundSystemAllocPolicy>
      allocationQueue;

  // This is a set of LiveRange.  They must be non-overlapping.  Attempts
  // to add an overlapping range will cause AvlTree::insert to MOZ_CRASH().
  using LiveRangeSet = AvlTree<LiveRange*, LiveRange>;

  // The same, but for LiveRangePlus.  See comments on LiveRangePlus.
  using LiveRangePlusSet = AvlTree<LiveRangePlus, LiveRangePlus>;

  // Each physical register is associated with the set of ranges over which
  // that register is currently allocated.
  struct PhysicalRegister {
    bool allocatable;
    AnyRegister reg;
    LiveRangePlusSet allocations;

    PhysicalRegister() : allocatable(false) {}
  };
  mozilla::Array<PhysicalRegister, AnyRegister::Total> registers;

  // Ranges of code which are considered to be hot, for which good allocation
  // should be prioritized.
  LiveRangeSet hotcode;

  // Output positions of all call instructions (where all registers must be
  // spilled). This vector is sorted in ascending order and doesn't contain
  // duplicate values.
  Vector<CodePosition, 16, BackgroundSystemAllocPolicy> callPositions;

  // Information about an allocated stack slot.
  struct SpillSlot : public TempObject,
                     public InlineForwardListNode<SpillSlot> {
    LStackSlot alloc;
    LiveRangePlusSet allocated;

    SpillSlot(uint32_t slot, LStackSlot::Width width, LifoAlloc* alloc)
        : alloc(slot, width), allocated(alloc) {}
  };
  using SpillSlotList = InlineForwardList<SpillSlot>;

  // All allocated slots of each width.
  SpillSlotList normalSlots, doubleSlots, quadSlots;

  Vector<LiveBundle*, 4, BackgroundSystemAllocPolicy> spilledBundles;

  // The bundle id that will be used for the next LiveBundle that's allocated.
  uint32_t nextBundleId_ = 0;

  using LiveBundleVector = Vector<LiveBundle*, 4, BackgroundSystemAllocPolicy>;

  // Misc accessors
  bool compilingWasm() { return mir->outerInfo().compilingWasm(); }
  VirtualRegister& vreg(const LDefinition* def) {
    return vregs[def->virtualRegister()];
  }
  VirtualRegister& vreg(const LAllocation* alloc) {
    MOZ_ASSERT(alloc->isUse());
    return vregs[alloc->toUse()->virtualRegister()];
  }

  uint32_t getNextBundleId() { return nextBundleId_++; }

  CodePosition entryOf(const LBlock* block) {
    return entryPositions[block->mir()->id()];
  }
  CodePosition exitOf(const LBlock* block) {
    return exitPositions[block->mir()->id()];
  }

  // Atomic group helper.  See comments in BacktrackingAllocator.cpp.
  CodePosition minimalDefEnd(LNode* ins) const;

  // Helpers for creating and adding MoveGroups
  [[nodiscard]] bool addMove(LMoveGroup* moves, LiveRange* from, LiveRange* to,
                             LDefinition::Type type) {
    LAllocation fromAlloc = from->bundle()->allocation();
    LAllocation toAlloc = to->bundle()->allocation();
    MOZ_ASSERT(fromAlloc != toAlloc);
    return moves->add(fromAlloc, toAlloc, type);
  }

  [[nodiscard]] bool moveInput(LInstruction* ins, LiveRange* from,
                               LiveRange* to, LDefinition::Type type) {
    if (from->bundle()->allocation() == to->bundle()->allocation()) {
      return true;
    }
    LMoveGroup* moves = getInputMoveGroup(ins);
    return addMove(moves, from, to, type);
  }

  [[nodiscard]] bool moveAfter(LInstruction* ins, LiveRange* from,
                               LiveRange* to, LDefinition::Type type) {
    if (from->bundle()->allocation() == to->bundle()->allocation()) {
      return true;
    }
    LMoveGroup* moves = getMoveGroupAfter(ins);
    return addMove(moves, from, to, type);
  }

  [[nodiscard]] bool moveAtExit(LBlock* block, LiveRange* from, LiveRange* to,
                                LDefinition::Type type) {
    if (from->bundle()->allocation() == to->bundle()->allocation()) {
      return true;
    }
    LMoveGroup* moves = block->getExitMoveGroup(alloc());
    return addMove(moves, from, to, type);
  }

  [[nodiscard]] bool moveAtEntry(LBlock* block, LiveRange* from, LiveRange* to,
                                 LDefinition::Type type) {
    if (from->bundle()->allocation() == to->bundle()->allocation()) {
      return true;
    }
    LMoveGroup* moves = block->getEntryMoveGroup(alloc());
    return addMove(moves, from, to, type);
  }

  // Out-of-line methods, in the same sequence as in BacktrackingAllocator.cpp.

  // Misc helpers: queries about uses
  bool isReusedInput(LUse* use, LNode* ins, bool considerCopy);
  bool isRegisterUse(UsePosition* use, LNode* ins, bool considerCopy = false);
  bool isRegisterDefinition(LiveRange* range);

  // Misc helpers: atomic LIR groups
  // (these are all in the parent class, RegisterAllocator)

  // Misc helpers: computation of bundle priorities and spill weights
  size_t computePriority(LiveBundle* bundle);
  bool minimalDef(LiveRange* range, LNode* ins);
  bool minimalUse(LiveRange* range, UsePosition* use);
  bool minimalBundle(LiveBundle* bundle, bool* pfixed = nullptr);
  size_t computeSpillWeight(LiveBundle* bundle);
  size_t maximumSpillWeight(const LiveBundleVector& bundles);

  // Initialization of the allocator
  [[nodiscard]] bool init();

  // Liveness analysis
  [[nodiscard]] bool addInitialFixedRange(AnyRegister reg, CodePosition from,
                                          CodePosition to);
  [[nodiscard]] bool buildLivenessInfo();

  // Call positions.
  mozilla::Maybe<size_t> lookupFirstCallPositionInRange(CodePosition from,
                                                        CodePosition to);

  // Merging and queueing of LiveRange groups
  void tryMergeBundles(LiveBundle* bundle0, LiveBundle* bundle1);
  void allocateStackDefinition(VirtualRegister& reg);
  [[nodiscard]] bool tryMergeReusedRegister(VirtualRegister& def,
                                            VirtualRegister& input);
  [[nodiscard]] bool mergeAndQueueRegisters();

  // Implementation of splitting decisions, but not the making of those
  // decisions
  [[nodiscard]] bool updateVirtualRegisterListsThenRequeueBundles(
      LiveBundle* bundle, const LiveBundleVector& newBundles);

  // Implementation of splitting decisions, but not the making of those
  // decisions
  [[nodiscard]] bool splitAt(LiveBundle* bundle,
                             const SplitPositionVector& splitPositions);

  // Creation of splitting decisions, but not their implementation
  [[nodiscard]] bool splitAcrossCalls(LiveBundle* bundle);
  [[nodiscard]] bool trySplitAcrossHotcode(LiveBundle* bundle, bool* success);
  [[nodiscard]] bool trySplitAfterLastRegisterUse(LiveBundle* bundle,
                                                  LiveBundle* conflict,
                                                  bool* success);
  [[nodiscard]] bool trySplitBeforeFirstRegisterUse(LiveBundle* bundle,
                                                    LiveBundle* conflict,
                                                    bool* success);

  // The top level driver for the splitting machinery
  [[nodiscard]] bool chooseBundleSplit(LiveBundle* bundle, bool hasCall,
                                       LiveBundle* conflict);

  // Bundle allocation
  [[nodiscard]] bool computeRequirement(LiveBundle* bundle,
                                        Requirement* prequirement,
                                        Requirement* phint);
  [[nodiscard]] bool tryAllocateRegister(PhysicalRegister& r,
                                         LiveBundle* bundle, bool* success,
                                         bool* hasCall,
                                         LiveBundleVector& conflicting);
  [[nodiscard]] bool tryAllocateAnyRegister(LiveBundle* bundle, bool* success,
                                            bool* hasCall,
                                            LiveBundleVector& conflicting);
  [[nodiscard]] bool evictBundle(LiveBundle* bundle);
  [[nodiscard]] bool tryAllocateFixed(LiveBundle* bundle,
                                      Requirement requirement, bool* success,
                                      bool* hasCall,
                                      LiveBundleVector& conflicting);
  [[nodiscard]] bool tryAllocateNonFixed(LiveBundle* bundle,
                                         Requirement requirement,
                                         Requirement hint, bool* success,
                                         bool* hasCall,
                                         LiveBundleVector& conflicting);
  [[nodiscard]] bool processBundle(const MIRGenerator* mir, LiveBundle* bundle);
  [[nodiscard]] bool spill(LiveBundle* bundle);
  [[nodiscard]] AVOID_INLINE_FOR_DEBUGGING bool
  tryAllocatingRegistersForSpillBundles();

  // Rewriting of the LIR after bundle processing is done
  [[nodiscard]] bool insertAllRanges(LiveRangePlusSet& set, LiveBundle* bundle);
  void sortVirtualRegisterRanges();
  [[nodiscard]] bool pickStackSlot(SpillSet* spill);
  [[nodiscard]] AVOID_INLINE_FOR_DEBUGGING bool pickStackSlots();
  [[nodiscard]] bool moveAtEdge(LBlock* predecessor, LBlock* successor,
                                LiveRange* from, LiveRange* to,
                                LDefinition::Type type);
  void removeDeadRanges(VirtualRegister& reg);
  [[nodiscard]] bool createMoveGroupsForControlFlowEdges(
      const VirtualRegister& reg, const ControlFlowEdgeVector& edges);
  [[nodiscard]] AVOID_INLINE_FOR_DEBUGGING bool
  createMoveGroupsFromLiveRangeTransitions();
  size_t findFirstNonCallSafepoint(CodePosition pos, size_t startFrom);
  void addLiveRegistersForRange(VirtualRegister& reg, LiveRange* range,
                                size_t* firstNonCallSafepoint);
  [[nodiscard]] AVOID_INLINE_FOR_DEBUGGING bool installAllocationsInLIR();
  size_t findFirstSafepoint(CodePosition pos, size_t startFrom);
  [[nodiscard]] AVOID_INLINE_FOR_DEBUGGING bool populateSafepoints();
  [[nodiscard]] AVOID_INLINE_FOR_DEBUGGING bool annotateMoveGroups();

  // Debug-printing support
#ifdef JS_JITSPEW
  void dumpLiveRangesByVReg(const char* who);
  void dumpLiveRangesByBundle(const char* who);
  void dumpAllocations();
#endif

  // Top level of the register allocation machinery, and the only externally
  // visible bit.
 public:
  BacktrackingAllocator(MIRGenerator* mir, LIRGenerator* lir, LIRGraph& graph)
      : RegisterAllocator(mir, lir, graph),
        liveIn(mir->alloc()),
        vregs(mir->alloc()),
        safepoints_(mir->alloc()),
        nonCallSafepoints_(mir->alloc()) {}

  [[nodiscard]] bool go();
};

}  // namespace jit
}  // namespace js

#endif /* jit_BacktrackingAllocator_h */
