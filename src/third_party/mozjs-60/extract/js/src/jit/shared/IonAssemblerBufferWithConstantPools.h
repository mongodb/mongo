/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_IonAssemblerBufferWithConstantPools_h
#define jit_shared_IonAssemblerBufferWithConstantPools_h

#include "mozilla/MathAlgorithms.h"

#include <algorithm>

#include "jit/JitSpewer.h"
#include "jit/shared/IonAssemblerBuffer.h"

// This code extends the AssemblerBuffer to support the pooling of values loaded
// using program-counter relative addressing modes. This is necessary with the
// ARM instruction set because it has a fixed instruction size that can not
// encode all values as immediate arguments in instructions. Pooling the values
// allows the values to be placed in large chunks which minimizes the number of
// forced branches around them in the code. This is used for loading floating
// point constants, for loading 32 bit constants on the ARMv6, for absolute
// branch targets, and in future will be needed for large branches on the ARMv6.
//
// For simplicity of the implementation, the constant pools are always placed
// after the loads referencing them. When a new constant pool load is added to
// the assembler buffer, a corresponding pool entry is added to the current
// pending pool. The finishPool() method copies the current pending pool entries
// into the assembler buffer at the current offset and patches the pending
// constant pool load instructions.
//
// Before inserting instructions or pool entries, it is necessary to determine
// if doing so would place a pending pool entry out of reach of an instruction,
// and if so then the pool must firstly be dumped. With the allocation algorithm
// used below, the recalculation of all the distances between instructions and
// their pool entries can be avoided by noting that there will be a limiting
// instruction and pool entry pair that does not change when inserting more
// instructions. Adding more instructions makes the same increase to the
// distance, between instructions and their pool entries, for all such
// pairs. This pair is recorded as the limiter, and it is updated when new pool
// entries are added, see updateLimiter()
//
// The pools consist of: a guard instruction that branches around the pool, a
// header word that helps identify a pool in the instruction stream, and then
// the pool entries allocated in units of words. The guard instruction could be
// omitted if control does not reach the pool, and this is referred to as a
// natural guard below, but for simplicity the guard branch is always
// emitted. The pool header is an identifiable word that in combination with the
// guard uniquely identifies a pool in the instruction stream. The header also
// encodes the pool size and a flag indicating if the guard is natural. It is
// possible to iterate through the code instructions skipping or examining the
// pools. E.g. it might be necessary to skip pools when search for, or patching,
// an instruction sequence.
//
// It is often required to keep a reference to a pool entry, to patch it after
// the buffer is finished. Each pool entry is assigned a unique index, counting
// up from zero (see the poolEntryCount slot below). These can be mapped back to
// the offset of the pool entry in the finished buffer, see poolEntryOffset().
//
// The code supports no-pool regions, and for these the size of the region, in
// instructions, must be supplied. This size is used to determine if inserting
// the instructions would place a pool entry out of range, and if so then a pool
// is firstly flushed. The DEBUG code checks that the emitted code is within the
// supplied size to detect programming errors. See enterNoPool() and
// leaveNoPool().

// The only planned instruction sets that require inline constant pools are the
// ARM, ARM64, and MIPS, and these all have fixed 32-bit sized instructions so
// for simplicity the code below is specialized for fixed 32-bit sized
// instructions and makes no attempt to support variable length
// instructions. The base assembler buffer which supports variable width
// instruction is used by the x86 and x64 backends.

// The AssemblerBufferWithConstantPools template class uses static callbacks to
// the provided Asm template argument class:
//
// void Asm::InsertIndexIntoTag(uint8_t* load_, uint32_t index)
//
//   When allocEntry() is called to add a constant pool load with an associated
//   constant pool entry, this callback is called to encode the index of the
//   allocated constant pool entry into the load instruction.
//
//   After the constant pool has been placed, PatchConstantPoolLoad() is called
//   to update the load instruction with the right load offset.
//
// void Asm::WritePoolGuard(BufferOffset branch,
//                          Instruction* dest,
//                          BufferOffset afterPool)
//
//   Write out the constant pool guard branch before emitting the pool.
//
//   branch
//     Offset of the guard branch in the buffer.
//
//   dest
//     Pointer into the buffer where the guard branch should be emitted. (Same
//     as getInst(branch)). Space for guardSize_ instructions has been reserved.
//
//   afterPool
//     Offset of the first instruction after the constant pool. This includes
//     both pool entries and branch veneers added after the pool data.
//
// void Asm::WritePoolHeader(uint8_t* start, Pool* p, bool isNatural)
//
//   Write out the pool header which follows the guard branch.
//
// void Asm::PatchConstantPoolLoad(void* loadAddr, void* constPoolAddr)
//
//   Re-encode a load of a constant pool entry after the location of the
//   constant pool is known.
//
//   The load instruction at loadAddr was previously passed to
//   InsertIndexIntoTag(). The constPoolAddr is the final address of the
//   constant pool in the assembler buffer.
//
// void Asm::PatchShortRangeBranchToVeneer(AssemblerBufferWithConstantPools*,
//                                         unsigned rangeIdx,
//                                         BufferOffset deadline,
//                                         BufferOffset veneer)
//
//   Patch a short-range branch to jump through a veneer before it goes out of
//   range.
//
//   rangeIdx, deadline
//     These arguments were previously passed to registerBranchDeadline(). It is
//     assumed that PatchShortRangeBranchToVeneer() knows how to compute the
//     offset of the short-range branch from this information.
//
//   veneer
//     Space for a branch veneer, guaranteed to be <= deadline. At this
//     position, guardSize_ * InstSize bytes are allocated. They should be
//     initialized to the proper unconditional branch instruction.
//
//   Unbound branches to the same unbound label are organized as a linked list:
//
//     Label::offset -> Branch1 -> Branch2 -> Branch3 -> nil
//
//   This callback should insert a new veneer branch into the list:
//
//     Label::offset -> Branch1 -> Branch2 -> Veneer -> Branch3 -> nil
//
//   When Assembler::bind() rewrites the branches with the real label offset, it
//   probably has to bind Branch2 to target the veneer branch instead of jumping
//   straight to the label.

namespace js {
namespace jit {

// BranchDeadlineSet - Keep track of pending branch deadlines.
//
// Some architectures like arm and arm64 have branch instructions with limited
// range. When assembling a forward branch, it is not always known if the final
// target label will be in range of the branch instruction.
//
// The BranchDeadlineSet data structure is used to keep track of the set of
// pending forward branches. It supports the following fast operations:
//
// 1. Get the earliest deadline in the set.
// 2. Add a new branch deadline.
// 3. Remove a branch deadline.
//
// Architectures may have different branch encodings with different ranges. Each
// supported range is assigned a small integer starting at 0. This data
// structure does not care about the actual range of branch instructions, just
// the latest buffer offset that can be reached - the deadline offset.
//
// Branched are stored as (rangeIdx, deadline) tuples. The target-specific code
// can compute the location of the branch itself from this information. This
// data structure does not need to know.
//
template <unsigned NumRanges>
class BranchDeadlineSet
{
    // Maintain a list of pending deadlines for each range separately.
    //
    // The offsets in each vector are always kept in ascending order.
    //
    // Because we have a separate vector for different ranges, as forward
    // branches are added to the assembler buffer, their deadlines will
    // always be appended to the vector corresponding to their range.
    //
    // When binding labels, we expect a more-or-less LIFO order of branch
    // resolutions. This would always hold if we had strictly structured control
    // flow.
    //
    // We allow branch deadlines to be added and removed in any order, but
    // performance is best in the expected case of near LIFO order.
    //
    typedef Vector<BufferOffset, 8, LifoAllocPolicy<Fallible>> RangeVector;

    // We really just want "RangeVector deadline_[NumRanges];", but each vector
    // needs to be initialized with a LifoAlloc, and C++ doesn't bend that way.
    //
    // Use raw aligned storage instead and explicitly construct NumRanges
    // vectors in our constructor.
    mozilla::AlignedStorage2<RangeVector[NumRanges]> deadlineStorage_;

    // Always access the range vectors through this method.
    RangeVector& vectorForRange(unsigned rangeIdx)
    {
        MOZ_ASSERT(rangeIdx < NumRanges, "Invalid branch range index");
        return (*deadlineStorage_.addr())[rangeIdx];
    }

    const RangeVector& vectorForRange(unsigned rangeIdx) const
    {
        MOZ_ASSERT(rangeIdx < NumRanges, "Invalid branch range index");
        return (*deadlineStorage_.addr())[rangeIdx];
    }

    // Maintain a precomputed earliest deadline at all times.
    // This is unassigned only when all deadline vectors are empty.
    BufferOffset earliest_;

    // The range vector owning earliest_. Uninitialized when empty.
    unsigned earliestRange_;

    // Recompute the earliest deadline after it's been invalidated.
    void recomputeEarliest()
    {
        earliest_ = BufferOffset();
        for (unsigned r = 0; r < NumRanges; r++) {
            auto& vec = vectorForRange(r);
            if (!vec.empty() && (!earliest_.assigned() || vec[0] < earliest_)) {
                earliest_ = vec[0];
                earliestRange_ = r;
            }
        }
    }

    // Update the earliest deadline if needed after inserting (rangeIdx,
    // deadline). Always return true for convenience:
    // return insert() && updateEarliest().
    bool updateEarliest(unsigned rangeIdx, BufferOffset deadline)
    {
        if (!earliest_.assigned() || deadline < earliest_) {
            earliest_ = deadline;
            earliestRange_ = rangeIdx;
        }
        return true;
    }

  public:
    explicit BranchDeadlineSet(LifoAlloc& alloc)
    {
        // Manually construct vectors in the uninitialized aligned storage.
        // This is because C++ arrays can otherwise only be constructed with
        // the default constructor.
        for (unsigned r = 0; r < NumRanges; r++)
            new (&vectorForRange(r)) RangeVector(alloc);
    }

    ~BranchDeadlineSet()
    {
        // Aligned storage doesn't destruct its contents automatically.
        for (unsigned r = 0; r < NumRanges; r++)
            vectorForRange(r).~RangeVector();
    }

    // Is this set completely empty?
    bool empty() const { return !earliest_.assigned(); }

    // Get the total number of deadlines in the set.
    size_t size() const
    {
        size_t count = 0;
        for (unsigned r = 0; r < NumRanges; r++)
            count += vectorForRange(r).length();
        return count;
    }

    // Get the number of deadlines for the range with the most elements.
    size_t maxRangeSize() const
    {
        size_t count = 0;
        for (unsigned r = 0; r < NumRanges; r++)
            count = std::max(count, vectorForRange(r).length());
        return count;
    }

    // Get the first deadline that is still in the set.
    BufferOffset earliestDeadline() const
    {
        MOZ_ASSERT(!empty());
        return earliest_;
    }

    // Get the range index corresponding to earliestDeadlineRange().
    unsigned earliestDeadlineRange() const
    {
        MOZ_ASSERT(!empty());
        return earliestRange_;
    }

    // Add a (rangeIdx, deadline) tuple to the set.
    //
    // It is assumed that this tuple is not already in the set.
    // This function performs best id the added deadline is later than any
    // existing deadline for the same range index.
    //
    // Return true if the tuple was added, false if the tuple could not be added
    // because of an OOM error.
    bool addDeadline(unsigned rangeIdx, BufferOffset deadline)
    {
        MOZ_ASSERT(deadline.assigned(), "Can only store assigned buffer offsets");
        // This is the vector where deadline should be saved.
        auto& vec = vectorForRange(rangeIdx);

        // Fast case: Simple append to the relevant array. This never affects
        // the earliest deadline.
        if (!vec.empty() && vec.back() < deadline)
            return vec.append(deadline);

        // Fast case: First entry to the vector. We need to update earliest_.
        if (vec.empty())
            return vec.append(deadline) && updateEarliest(rangeIdx, deadline);

        return addDeadlineSlow(rangeIdx, deadline);
    }

  private:
    // General case of addDeadline. This is split into two functions such that
    // the common case in addDeadline can be inlined while this part probably
    // won't inline.
    bool addDeadlineSlow(unsigned rangeIdx, BufferOffset deadline)
    {
        auto& vec = vectorForRange(rangeIdx);

        // Inserting into the middle of the vector. Use a log time binary search
        // and a linear time insert().
        // Is it worthwhile special-casing the empty vector?
        auto at = std::lower_bound(vec.begin(), vec.end(), deadline);
        MOZ_ASSERT(at == vec.end() || *at != deadline, "Cannot insert duplicate deadlines");
        return vec.insert(at, deadline) && updateEarliest(rangeIdx, deadline);
    }

  public:
    // Remove a deadline from the set.
    // If (rangeIdx, deadline) is not in the set, nothing happens.
    void removeDeadline(unsigned rangeIdx, BufferOffset deadline)
    {
        auto& vec = vectorForRange(rangeIdx);

        if (vec.empty())
            return;

        if (deadline == vec.back()) {
            // Expected fast case: Structured control flow causes forward
            // branches to be bound in reverse order.
            vec.popBack();
        } else {
            // Slow case: Binary search + linear erase.
            auto where = std::lower_bound(vec.begin(), vec.end(), deadline);
            if (where == vec.end() || *where != deadline)
                return;
            vec.erase(where);
        }
        if (deadline == earliest_)
            recomputeEarliest();
    }
};

// Specialization for architectures that don't need to track short-range
// branches.
template <>
class BranchDeadlineSet<0u>
{
  public:
    explicit BranchDeadlineSet(LifoAlloc& alloc) {}
    bool empty() const { return true; }
    size_t size() const { return 0; }
    size_t maxRangeSize() const { return 0; }
    BufferOffset earliestDeadline() const { MOZ_CRASH(); }
    unsigned earliestDeadlineRange() const { MOZ_CRASH(); }
    bool addDeadline(unsigned rangeIdx, BufferOffset deadline) { MOZ_CRASH(); }
    void removeDeadline(unsigned rangeIdx, BufferOffset deadline) { MOZ_CRASH(); }
};

// The allocation unit size for pools.
typedef int32_t PoolAllocUnit;

// Hysteresis given to short-range branches.
//
// If any short-range branches will go out of range in the next N bytes,
// generate a veneer for them in the current pool. The hysteresis prevents the
// creation of many tiny constant pools for branch veneers.
const size_t ShortRangeBranchHysteresis = 128;

struct Pool
{
  private:
    // The maximum program-counter relative offset below which the instruction
    // set can encode. Different classes of intructions might support different
    // ranges but for simplicity the minimum is used here, and for the ARM this
    // is constrained to 1024 by the float load instructions.
    const size_t maxOffset_;
    // An offset to apply to program-counter relative offsets. The ARM has a
    // bias of 8.
    const unsigned bias_;

    // The content of the pool entries.
    Vector<PoolAllocUnit, 8, LifoAllocPolicy<Fallible>> poolData_;

    // Flag that tracks OOM conditions. This is set after any append failed.
    bool oom_;

    // The limiting instruction and pool-entry pair. The instruction program
    // counter relative offset of this limiting instruction will go out of range
    // first as the pool position moves forward. It is more efficient to track
    // just this limiting pair than to recheck all offsets when testing if the
    // pool needs to be dumped.
    //
    // 1. The actual offset of the limiting instruction referencing the limiting
    // pool entry.
    BufferOffset limitingUser;
    // 2. The pool entry index of the limiting pool entry.
    unsigned limitingUsee;

  public:
    // A record of the code offset of instructions that reference pool
    // entries. These instructions need to be patched when the actual position
    // of the instructions and pools are known, and for the code below this
    // occurs when each pool is finished, see finishPool().
    Vector<BufferOffset, 8, LifoAllocPolicy<Fallible>> loadOffsets;

    // Create a Pool. Don't allocate anything from lifoAloc, just capture its reference.
    explicit Pool(size_t maxOffset, unsigned bias, LifoAlloc& lifoAlloc)
      : maxOffset_(maxOffset),
        bias_(bias),
        poolData_(lifoAlloc),
        oom_(false),
        limitingUser(),
        limitingUsee(INT_MIN),
        loadOffsets(lifoAlloc)
    {
    }

    // If poolData() returns nullptr then oom_ will also be true.
    const PoolAllocUnit* poolData() const {
        return poolData_.begin();
    }

    unsigned numEntries() const {
        return poolData_.length();
    }

    size_t getPoolSize() const {
        return numEntries() * sizeof(PoolAllocUnit);
    }

    bool oom() const {
        return oom_;
    }

    // Update the instruction/pool-entry pair that limits the position of the
    // pool. The nextInst is the actual offset of the new instruction being
    // allocated.
    //
    // This is comparing the offsets, see checkFull() below for the equation,
    // but common expressions on both sides have been canceled from the ranges
    // being compared. Notably, the poolOffset cancels out, so the limiting pair
    // does not depend on where the pool is placed.
    void updateLimiter(BufferOffset nextInst) {
        ptrdiff_t oldRange = limitingUsee * sizeof(PoolAllocUnit) - limitingUser.getOffset();
        ptrdiff_t newRange = getPoolSize() - nextInst.getOffset();
        if (!limitingUser.assigned() || newRange > oldRange) {
            // We have a new largest range!
            limitingUser = nextInst;
            limitingUsee = numEntries();
        }
    }

    // Check if inserting a pool at the actual offset poolOffset would place
    // pool entries out of reach. This is called before inserting instructions
    // to check that doing so would not push pool entries out of reach, and if
    // so then the pool would need to be firstly dumped. The poolOffset is the
    // first word of the pool, after the guard and header and alignment fill.
    bool checkFull(size_t poolOffset) const {
        // Not full if there are no uses.
        if (!limitingUser.assigned())
            return false;
        size_t offset = poolOffset + limitingUsee * sizeof(PoolAllocUnit)
                        - (limitingUser.getOffset() + bias_);
        return offset >= maxOffset_;
    }

    static const unsigned OOM_FAIL = unsigned(-1);

    unsigned insertEntry(unsigned num, uint8_t* data, BufferOffset off, LifoAlloc& lifoAlloc) {
        if (oom_)
            return OOM_FAIL;
        unsigned ret = numEntries();
        if (!poolData_.append((PoolAllocUnit*)data, num) || !loadOffsets.append(off)) {
            oom_ = true;
            return OOM_FAIL;
        }
        return ret;
    }

    void reset() {
        poolData_.clear();
        loadOffsets.clear();

        limitingUser = BufferOffset();
        limitingUsee = -1;
    }
};


// Template arguments:
//
// SliceSize
//   Number of bytes in each allocated BufferSlice. See
//   AssemblerBuffer::SliceSize.
//
// InstSize
//   Size in bytes of the fixed-size instructions. This should be equal to
//   sizeof(Inst). This is only needed here because the buffer is defined before
//   the Instruction.
//
// Inst
//   The actual type used to represent instructions. This is only really used as
//   the return type of the getInst() method.
//
// Asm
//   Class defining the needed static callback functions. See documentation of
//   the Asm::* callbacks above.
//
// NumShortBranchRanges
//   The number of short branch ranges to support. This can be 0 if no support
//   for tracking short range branches is needed. The
//   AssemblerBufferWithConstantPools class does not need to know what the range
//   of branches is - it deals in branch 'deadlines' which is the last buffer
//   position that a short-range forward branch can reach. It is assumed that
//   the Asm class is able to find the actual branch instruction given a
//   (range-index, deadline) pair.
//
//
template <size_t SliceSize, size_t InstSize, class Inst, class Asm,
          unsigned NumShortBranchRanges = 0>
struct AssemblerBufferWithConstantPools : public AssemblerBuffer<SliceSize, Inst>
{
  private:
    // The PoolEntry index counter. Each PoolEntry is given a unique index,
    // counting up from zero, and these can be mapped back to the actual pool
    // entry offset after finishing the buffer, see poolEntryOffset().
    size_t poolEntryCount;

  public:
    class PoolEntry
    {
        size_t index_;

      public:
        explicit PoolEntry(size_t index)
          : index_(index)
        { }

        PoolEntry()
          : index_(-1)
        { }

        size_t index() const {
            return index_;
        }
    };

  private:
    typedef AssemblerBuffer<SliceSize, Inst> Parent;
    using typename Parent::Slice;

    // The size of a pool guard, in instructions. A branch around the pool.
    const unsigned guardSize_;
    // The size of the header that is put at the beginning of a full pool, in
    // instruction sized units.
    const unsigned headerSize_;

    // The maximum pc relative offset encoded in instructions that reference
    // pool entries. This is generally set to the maximum offset that can be
    // encoded by the instructions, but for testing can be lowered to affect the
    // pool placement and frequency of pool placement.
    const size_t poolMaxOffset_;

    // The bias on pc relative addressing mode offsets, in units of bytes. The
    // ARM has a bias of 8 bytes.
    const unsigned pcBias_;

    // The current working pool. Copied out as needed before resetting.
    Pool pool_;

    // The buffer should be aligned to this address.
    const size_t instBufferAlign_;

    struct PoolInfo {
        // The index of the first entry in this pool.
        // Pool entries are numbered uniquely across all pools, starting from 0.
        unsigned firstEntryIndex;

        // The location of this pool's first entry in the main assembler buffer.
        // Note that the pool guard and header come before this offset which
        // points directly at the data.
        BufferOffset offset;

        explicit PoolInfo(unsigned index, BufferOffset data)
          : firstEntryIndex(index)
          , offset(data)
        {
        }
    };

    // Info for each pool that has already been dumped. This does not include
    // any entries in pool_.
    Vector<PoolInfo, 8, LifoAllocPolicy<Fallible>> poolInfo_;

    // Set of short-range forward branches that have not yet been bound.
    // We may need to insert veneers if the final label turns out to be out of
    // range.
    //
    // This set stores (rangeIdx, deadline) pairs instead of the actual branch
    // locations.
    BranchDeadlineSet<NumShortBranchRanges> branchDeadlines_;

    // When true dumping pools is inhibited.
    bool canNotPlacePool_;

#ifdef DEBUG
    // State for validating the 'maxInst' argument to enterNoPool().
    // The buffer offset when entering the no-pool region.
    size_t canNotPlacePoolStartOffset_;
    // The maximum number of word sized instructions declared for the no-pool
    // region.
    size_t canNotPlacePoolMaxInst_;
#endif

    // Instruction to use for alignment fill.
    const uint32_t alignFillInst_;

    // Insert a number of NOP instructions between each requested instruction at
    // all locations at which a pool can potentially spill. This is useful for
    // checking that instruction locations are correctly referenced and/or
    // followed.
    const uint32_t nopFillInst_;
    const unsigned nopFill_;
    // For inhibiting the insertion of fill NOPs in the dynamic context in which
    // they are being inserted.
    bool inhibitNops_;

  public:
    // A unique id within each JitContext, to identify pools in the debug
    // spew. Set by the MacroAssembler, see getNextAssemblerId().
    int id;

  private:
    // The buffer slices are in a double linked list.
    Slice* getHead() const {
        return this->head;
    }
    Slice* getTail() const {
        return this->tail;
    }

  public:
    // Create an assembler buffer.
    // Note that this constructor is not allowed to actually allocate memory from this->lifoAlloc_
    // because the MacroAssembler constructor has not yet created an AutoJitContextAlloc.
    AssemblerBufferWithConstantPools(unsigned guardSize, unsigned headerSize,
                                     size_t instBufferAlign, size_t poolMaxOffset,
                                     unsigned pcBias, uint32_t alignFillInst, uint32_t nopFillInst,
                                     unsigned nopFill = 0)
      : poolEntryCount(0),
        guardSize_(guardSize),
        headerSize_(headerSize),
        poolMaxOffset_(poolMaxOffset),
        pcBias_(pcBias),
        pool_(poolMaxOffset, pcBias, this->lifoAlloc_),
        instBufferAlign_(instBufferAlign),
        poolInfo_(this->lifoAlloc_),
        branchDeadlines_(this->lifoAlloc_),
        canNotPlacePool_(false),
#ifdef DEBUG
        canNotPlacePoolStartOffset_(0),
        canNotPlacePoolMaxInst_(0),
#endif
        alignFillInst_(alignFillInst),
        nopFillInst_(nopFillInst),
        nopFill_(nopFill),
        inhibitNops_(false),
        id(-1)
    { }

    // We need to wait until an AutoJitContextAlloc is created by the
    // MacroAssembler before allocating any space.
    void initWithAllocator() {
        // We hand out references to lifoAlloc_ in the constructor.
        // Check that no allocations were made then.
        MOZ_ASSERT(this->lifoAlloc_.isEmpty(), "Illegal LIFO allocations before AutoJitContextAlloc");
    }

  private:
    size_t sizeExcludingCurrentPool() const {
        // Return the actual size of the buffer, excluding the current pending
        // pool.
        return this->nextOffset().getOffset();
    }

  public:
    size_t size() const {
        // Return the current actual size of the buffer. This is only accurate
        // if there are no pending pool entries to dump, check.
        MOZ_ASSERT_IF(!this->oom(), pool_.numEntries() == 0);
        return sizeExcludingCurrentPool();
    }

  private:
    void insertNopFill() {
        // Insert fill for testing.
        if (nopFill_ > 0 && !inhibitNops_ && !canNotPlacePool_) {
            inhibitNops_ = true;

            // Fill using a branch-nop rather than a NOP so this can be
            // distinguished and skipped.
            for (size_t i = 0; i < nopFill_; i++)
                putInt(nopFillInst_);

            inhibitNops_ = false;
        }
    }

    static const unsigned OOM_FAIL = unsigned(-1);
    static const unsigned DUMMY_INDEX = unsigned(-2);

    // Check if it is possible to add numInst instructions and numPoolEntries
    // constant pool entries without needing to flush the current pool.
    bool hasSpaceForInsts(unsigned numInsts, unsigned numPoolEntries) const
    {
        size_t nextOffset = sizeExcludingCurrentPool();
        // Earliest starting offset for the current pool after adding numInsts.
        // This is the beginning of the pool entries proper, after inserting a
        // guard branch + pool header.
        size_t poolOffset = nextOffset + (numInsts + guardSize_ + headerSize_) * InstSize;

        // Any constant pool loads that would go out of range?
        if (pool_.checkFull(poolOffset))
            return false;

        // Any short-range branch that would go out of range?
        if (!branchDeadlines_.empty()) {
            size_t deadline = branchDeadlines_.earliestDeadline().getOffset();
            size_t poolEnd =
              poolOffset + pool_.getPoolSize() + numPoolEntries * sizeof(PoolAllocUnit);

            // When NumShortBranchRanges > 1, is is possible for branch deadlines to expire faster
            // than we can insert veneers. Suppose branches are 4 bytes each, we could have the
            // following deadline set:
            //
            //   Range 0: 40, 44, 48
            //   Range 1: 44, 48
            //
            // It is not good enough to start inserting veneers at the 40 deadline; we would not be
            // able to create veneers for the second 44 deadline. Instead, we need to start at 32:
            //
            //   32: veneer(40)
            //   36: veneer(44)
            //   40: veneer(44)
            //   44: veneer(48)
            //   48: veneer(48)
            //
            // This is a pretty conservative solution to the problem: If we begin at the earliest
            // deadline, we can always emit all veneers for the range that currently has the most
            // pending deadlines. That may not leave room for veneers for the remaining ranges, so
            // reserve space for those secondary range veneers assuming the worst case deadlines.

            // Total pending secondary range veneer size.
            size_t secondaryVeneers =
              guardSize_ * (branchDeadlines_.size() - branchDeadlines_.maxRangeSize());

            if (deadline < poolEnd + secondaryVeneers)
                return false;
        }

        return true;
    }

    unsigned insertEntryForwards(unsigned numInst, unsigned numPoolEntries, uint8_t* inst, uint8_t* data) {
        // If inserting pool entries then find a new limiter before we do the
        // range check.
        if (numPoolEntries)
            pool_.updateLimiter(BufferOffset(sizeExcludingCurrentPool()));

        if (!hasSpaceForInsts(numInst, numPoolEntries)) {
            if (numPoolEntries)
                JitSpew(JitSpew_Pools, "[%d] Inserting pool entry caused a spill", id);
            else
                JitSpew(JitSpew_Pools, "[%d] Inserting instruction(%zu) caused a spill", id,
                        sizeExcludingCurrentPool());

            finishPool();
            if (this->oom())
                return OOM_FAIL;
            return insertEntryForwards(numInst, numPoolEntries, inst, data);
        }
        if (numPoolEntries) {
            unsigned result = pool_.insertEntry(numPoolEntries, data, this->nextOffset(), this->lifoAlloc_);
            if (result == Pool::OOM_FAIL) {
                this->fail_oom();
                return OOM_FAIL;
            }
            return result;
        }

        // The pool entry index is returned above when allocating an entry, but
        // when not allocating an entry a dummy value is returned - it is not
        // expected to be used by the caller.
        return DUMMY_INDEX;
    }

  public:
    // Get the next buffer offset where an instruction would be inserted.
    // This may flush the current constant pool before returning nextOffset().
    BufferOffset nextInstrOffset()
    {
        if (!hasSpaceForInsts(/* numInsts= */ 1, /* numPoolEntries= */ 0)) {
            JitSpew(JitSpew_Pools, "[%d] nextInstrOffset @ %d caused a constant pool spill", id,
                    this->nextOffset().getOffset());
            finishPool();
        }
        return this->nextOffset();
    }

    MOZ_NEVER_INLINE
    BufferOffset allocEntry(size_t numInst, unsigned numPoolEntries,
                            uint8_t* inst, uint8_t* data, PoolEntry* pe = nullptr)
    {
        // The allocation of pool entries is not supported in a no-pool region,
        // check.
        MOZ_ASSERT_IF(numPoolEntries, !canNotPlacePool_);

        if (this->oom() && !this->bail())
            return BufferOffset();

        insertNopFill();

#ifdef JS_JITSPEW
        if (numPoolEntries && JitSpewEnabled(JitSpew_Pools)) {
            JitSpew(JitSpew_Pools, "[%d] Inserting %d entries into pool", id, numPoolEntries);
            JitSpewStart(JitSpew_Pools, "[%d] data is: 0x", id);
            size_t length = numPoolEntries * sizeof(PoolAllocUnit);
            for (unsigned idx = 0; idx < length; idx++) {
                JitSpewCont(JitSpew_Pools, "%02x", data[length - idx - 1]);
                if (((idx & 3) == 3) && (idx + 1 != length))
                    JitSpewCont(JitSpew_Pools, "_");
            }
            JitSpewFin(JitSpew_Pools);
        }
#endif

        // Insert the pool value.
        unsigned index = insertEntryForwards(numInst, numPoolEntries, inst, data);
        if (this->oom())
            return BufferOffset();

        // Now to get an instruction to write.
        PoolEntry retPE;
        if (numPoolEntries) {
            JitSpew(JitSpew_Pools, "[%d] Entry has index %u, offset %zu", id, index,
                    sizeExcludingCurrentPool());
            Asm::InsertIndexIntoTag(inst, index);
            // Figure out the offset within the pool entries.
            retPE = PoolEntry(poolEntryCount);
            poolEntryCount += numPoolEntries;
        }
        // Now inst is a valid thing to insert into the instruction stream.
        if (pe != nullptr)
            *pe = retPE;
        return this->putBytes(numInst * InstSize, inst);
    }


    // putInt is the workhorse for the assembler and higher-level buffer
    // abstractions: it places one instruction into the instruction stream.
    // Under normal circumstances putInt should just check that the constant
    // pool does not need to be flushed, that there is space for the single word
    // of the instruction, and write that word and update the buffer pointer.
    //
    // To do better here we need a status variable that handles both nopFill_
    // and capacity, so that we can quickly know whether to go the slow path.
    // That could be a variable that has the remaining number of simple
    // instructions that can be inserted before a more expensive check,
    // which is set to zero when nopFill_ is set.
    //
    // We assume that we don't have to check this->oom() if there is space to
    // insert a plain instruction; there will always come a later time when it will be
    // checked anyway.

    MOZ_ALWAYS_INLINE
    BufferOffset putInt(uint32_t value) {
        if (nopFill_ || !hasSpaceForInsts(/* numInsts= */ 1, /* numPoolEntries= */ 0))
            return allocEntry(1, 0, (uint8_t*)&value, nullptr, nullptr);

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        return this->putU32Aligned(value);
#else
        return this->AssemblerBuffer<SliceSize, Inst>::putInt(value);
#endif
    }

    // Register a short-range branch deadline.
    //
    // After inserting a short-range forward branch, call this method to
    // register the branch 'deadline' which is the last buffer offset that the
    // branch instruction can reach.
    //
    // When the branch is bound to a destination label, call
    // unregisterBranchDeadline() to stop tracking this branch,
    //
    // If the assembled code is about to exceed the registered branch deadline,
    // and unregisterBranchDeadline() has not yet been called, an
    // instruction-sized constant pool entry is allocated before the branch
    // deadline.
    //
    // rangeIdx
    //   A number < NumShortBranchRanges identifying the range of the branch.
    //
    // deadline
    //   The highest buffer offset the the short-range branch can reach
    //   directly.
    //
    void registerBranchDeadline(unsigned rangeIdx, BufferOffset deadline)
    {
        if (!this->oom() && !branchDeadlines_.addDeadline(rangeIdx, deadline))
            this->fail_oom();
    }

    // Un-register a short-range branch deadline.
    //
    // When a short-range branch has been successfully bound to its destination
    // label, call this function to stop traching the branch.
    //
    // The (rangeIdx, deadline) pair must be previously registered.
    //
    void unregisterBranchDeadline(unsigned rangeIdx, BufferOffset deadline)
    {
        if (!this->oom())
            branchDeadlines_.removeDeadline(rangeIdx, deadline);
    }

  private:
    // Are any short-range branches about to expire?
    bool hasExpirableShortRangeBranches() const
    {
        if (branchDeadlines_.empty())
            return false;

        // Include branches that would expire in the next N bytes.
        // The hysteresis avoids the needless creation of many tiny constant
        // pools.
        return this->nextOffset().getOffset() + ShortRangeBranchHysteresis >
               size_t(branchDeadlines_.earliestDeadline().getOffset());
    }

    void finishPool() {
        JitSpew(JitSpew_Pools, "[%d] Attempting to finish pool %zu with %u entries.",
                id, poolInfo_.length(), pool_.numEntries());

        if (pool_.numEntries() == 0 && !hasExpirableShortRangeBranches()) {
            // If there is no data in the pool being dumped, don't dump anything.
            JitSpew(JitSpew_Pools, "[%d] Aborting because the pool is empty", id);
            return;
        }

        // Should not be placing a pool in a no-pool region, check.
        MOZ_ASSERT(!canNotPlacePool_);

        // Dump the pool with a guard branch around the pool.
        BufferOffset guard = this->putBytes(guardSize_ * InstSize, nullptr);
        BufferOffset header = this->putBytes(headerSize_ * InstSize, nullptr);
        BufferOffset data =
          this->putBytesLarge(pool_.getPoolSize(), (const uint8_t*)pool_.poolData());
        if (this->oom())
            return;

        // Now generate branch veneers for any short-range branches that are
        // about to expire.
        while (hasExpirableShortRangeBranches()) {
            unsigned rangeIdx = branchDeadlines_.earliestDeadlineRange();
            BufferOffset deadline = branchDeadlines_.earliestDeadline();

            // Stop tracking this branch. The Asm callback below may register
            // new branches to track.
            branchDeadlines_.removeDeadline(rangeIdx, deadline);

            // Make room for the veneer. Same as a pool guard branch.
            BufferOffset veneer = this->putBytes(guardSize_ * InstSize, nullptr);
            if (this->oom())
                return;

            // Fix the branch so it targets the veneer.
            // The Asm class knows how to find the original branch given the
            // (rangeIdx, deadline) pair.
            Asm::PatchShortRangeBranchToVeneer(this, rangeIdx, deadline, veneer);
        }

        // We only reserved space for the guard branch and pool header.
        // Fill them in.
        BufferOffset afterPool = this->nextOffset();
        Asm::WritePoolGuard(guard, this->getInst(guard), afterPool);
        Asm::WritePoolHeader((uint8_t*)this->getInst(header), &pool_, false);

        // With the pool's final position determined it is now possible to patch
        // the instructions that reference entries in this pool, and this is
        // done incrementally as each pool is finished.
        size_t poolOffset = data.getOffset();

        unsigned idx = 0;
        for (BufferOffset* iter = pool_.loadOffsets.begin();
             iter != pool_.loadOffsets.end();
             ++iter, ++idx)
        {
            // All entries should be before the pool.
            MOZ_ASSERT(iter->getOffset() < guard.getOffset());

            // Everything here is known so we can safely do the necessary
            // substitutions.
            Inst* inst = this->getInst(*iter);
            size_t codeOffset = poolOffset - iter->getOffset();

            // That is, PatchConstantPoolLoad wants to be handed the address of
            // the pool entry that is being loaded.  We need to do a non-trivial
            // amount of math here, since the pool that we've made does not
            // actually reside there in memory.
            JitSpew(JitSpew_Pools, "[%d] Fixing entry %d offset to %zu", id, idx, codeOffset);
            Asm::PatchConstantPoolLoad(inst, (uint8_t*)inst + codeOffset);
        }

        // Record the pool info.
        unsigned firstEntry = poolEntryCount - pool_.numEntries();
        if (!poolInfo_.append(PoolInfo(firstEntry, data))) {
            this->fail_oom();
            return;
        }

        // Reset everything to the state that it was in when we started.
        pool_.reset();
    }

  public:
    void flushPool() {
        if (this->oom())
            return;
        JitSpew(JitSpew_Pools, "[%d] Requesting a pool flush", id);
        finishPool();
    }

    void enterNoPool(size_t maxInst) {
        // Don't allow re-entry.
        MOZ_ASSERT(!canNotPlacePool_);
        insertNopFill();

        // Check if the pool will spill by adding maxInst instructions, and if
        // so then finish the pool before entering the no-pool region. It is
        // assumed that no pool entries are allocated in a no-pool region and
        // this is asserted when allocating entries.
        if (!hasSpaceForInsts(maxInst, 0)) {
            JitSpew(JitSpew_Pools, "[%d] No-Pool instruction(%zu) caused a spill.", id,
                    sizeExcludingCurrentPool());
            finishPool();
        }

#ifdef DEBUG
        // Record the buffer position to allow validating maxInst when leaving
        // the region.
        canNotPlacePoolStartOffset_ = this->nextOffset().getOffset();
        canNotPlacePoolMaxInst_ = maxInst;
#endif

        canNotPlacePool_ = true;
    }

    void leaveNoPool() {
        MOZ_ASSERT(canNotPlacePool_);
        canNotPlacePool_ = false;

        // Validate the maxInst argument supplied to enterNoPool().
        MOZ_ASSERT(this->nextOffset().getOffset() - canNotPlacePoolStartOffset_ <= canNotPlacePoolMaxInst_ * InstSize);
    }

    void align(unsigned alignment) {
        MOZ_ASSERT(mozilla::IsPowerOfTwo(alignment));
        MOZ_ASSERT(alignment >= InstSize);

        // A pool many need to be dumped at this point, so insert NOP fill here.
        insertNopFill();

        // Check if the code position can be aligned without dumping a pool.
        unsigned requiredFill = sizeExcludingCurrentPool() & (alignment - 1);
        if (requiredFill == 0)
            return;
        requiredFill = alignment - requiredFill;

        // Add an InstSize because it is probably not useful for a pool to be
        // dumped at the aligned code position.
        if (!hasSpaceForInsts(requiredFill / InstSize + 1, 0)) {
            // Alignment would cause a pool dump, so dump the pool now.
            JitSpew(JitSpew_Pools, "[%d] Alignment of %d at %zu caused a spill.",
                    id, alignment, sizeExcludingCurrentPool());
            finishPool();
        }

        inhibitNops_ = true;
        while ((sizeExcludingCurrentPool() & (alignment - 1)) && !this->oom())
            putInt(alignFillInst_);
        inhibitNops_ = false;
    }

  public:
    void executableCopy(uint8_t* dest) {
        if (this->oom())
            return;
        // The pools should have all been flushed, check.
        MOZ_ASSERT(pool_.numEntries() == 0);
        for (Slice* cur = getHead(); cur != nullptr; cur = cur->getNext()) {
            memcpy(dest, &cur->instructions[0], cur->length());
            dest += cur->length();
        }
    }

    bool appendRawCode(const uint8_t* code, size_t numBytes) {
        if (this->oom())
            return false;
        // The pools should have all been flushed, check.
        MOZ_ASSERT(pool_.numEntries() == 0);
        while (numBytes > SliceSize) {
            this->putBytes(SliceSize, code);
            numBytes -= SliceSize;
            code += SliceSize;
        }
        this->putBytes(numBytes, code);
        return !this->oom();
    }

  public:
    size_t poolEntryOffset(PoolEntry pe) const {
        MOZ_ASSERT(pe.index() < poolEntryCount - pool_.numEntries(),
                   "Invalid pool entry, or not flushed yet.");
        // Find the pool containing pe.index().
        // The array is sorted, so we can use a binary search.
        auto b = poolInfo_.begin(), e = poolInfo_.end();
        // A note on asymmetric types in the upper_bound comparator:
        // http://permalink.gmane.org/gmane.comp.compilers.clang.devel/10101
        auto i = std::upper_bound(b, e, pe.index(), [](size_t value, const PoolInfo& entry) {
            return value < entry.firstEntryIndex;
        });
        // Since upper_bound finds the first pool greater than pe,
        // we want the previous one which is the last one less than or equal.
        MOZ_ASSERT(i != b, "PoolInfo not sorted or empty?");
        --i;
        // The i iterator now points to the pool containing pe.index.
        MOZ_ASSERT(i->firstEntryIndex <= pe.index() &&
                   (i + 1 == e || (i + 1)->firstEntryIndex > pe.index()));
        // Compute the byte offset into the pool.
        unsigned relativeIndex = pe.index() - i->firstEntryIndex;
        return i->offset.getOffset() + relativeIndex * sizeof(PoolAllocUnit);
    }
};

} // namespace ion
} // namespace js

#endif // jit_shared_IonAssemblerBufferWithConstantPools_h
