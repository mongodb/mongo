/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_IonAssemblerBufferWithConstantPools_h
#define jit_shared_IonAssemblerBufferWithConstantPools_h

#include "mozilla/DebugOnly.h"

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
// The pools are associated with the AssemblerBuffer buffer slices, with at most
// one pool per slice. When a pool needs to be dumped, the current slice is
// finished, and this is referred to as a 'perforation' below. The pool data is
// stored separately from the buffer slice data during assembly and is
// concatenated together with the instructions when finishing the buffer. The
// parent AssemblerBuffer slices are extended by the BufferSliceTail class below
// to include the pool information. The pool guard instructions (the branches
// around the pools) are included in the buffer slices.
//
// To illustrate.  Without the pools there are just slices of instructions. The
// slice size may vary - they need not be completely full.
//
// -------------     -----------     ------------     -----------
// | Slice 1   | <-> | Slice 2 | <-> | Slice 3  | <-> | Slice 4 |
// -------------     -----------     ------------     -----------
//
// The pools are only placed at the end of a slice and not all slices will have
// a pool. There will generally be a guard instruction (G below) jumping around
// a pool and these guards are included in the slices.
//
// --------------     -------------     ---------------     -------------
// | Slice 1  G | <-> | Slice 2 G | <-> | Slice 3     | <-> | Slice 4 G |
// --------------     -------------     ---------------     -------------
//             |                 |                                     |
//            Pool 1            Pool 2                                Pool 3
//
// When finishing the buffer, the slices and pools are concatenated into one
// code vector, see executableCopy().
//
// --------------------------------------------------------------------
// | Slice 1 G  Pool 1  Slice 2 G  Pool 2  Slice 3  Slice 4 G  Pool 3 |
// --------------------------------------------------------------------
//
// The buffer nextOffset() returns the cumulative size of the buffer slices,
// that is the instructions including the pool guard instructions, but excluding
// the content of the pools.
//
// The PoolInfo structure stores the final actual position of the end of each
// pool and these positions are cumulatively built as pools are dumped. It also
// stores the buffer nextOffset() position at the start of the pool including
// guards. The difference between these gives the cumulative size of pools
// dumped which is used to map an index returned by nextOffset() to its final
// actual position, see poolSizeBefore().
//
// Before inserting instructions, or pool entries, it is necessary to determine
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

namespace js {
namespace jit {

typedef Vector<BufferOffset, 512, OldJitAllocPolicy> LoadOffsets;

// The allocation unit size for pools.
typedef int32_t PoolAllocUnit;

struct Pool
  : public OldJitAllocPolicy
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

    // The number of PoolAllocUnit sized entires used from the poolData vector.
    unsigned numEntries_;
    // The available size of the poolData vector, in PoolAllocUnits.
    unsigned buffSize;
    // The content of the pool entries.
    PoolAllocUnit* poolData_;

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
    LoadOffsets loadOffsets;

    explicit Pool(size_t maxOffset, unsigned bias, LifoAlloc& lifoAlloc)
        : maxOffset_(maxOffset), bias_(bias), numEntries_(0), buffSize(8),
          poolData_(lifoAlloc.newArrayUninitialized<PoolAllocUnit>(buffSize)),
          limitingUser(), limitingUsee(INT_MIN), loadOffsets()
    {
    }
    static const unsigned Garbage = 0xa5a5a5a5;
    Pool() : maxOffset_(Garbage), bias_(Garbage)
    {
    }

    PoolAllocUnit* poolData() const {
        return poolData_;
    }

    unsigned numEntries() const {
        return numEntries_;
    }

    size_t getPoolSize() const {
        return numEntries_ * sizeof(PoolAllocUnit);
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
        ptrdiff_t newRange = numEntries_ * sizeof(PoolAllocUnit) - nextInst.getOffset();
        if (!limitingUser.assigned() || newRange > oldRange) {
            // We have a new largest range!
            limitingUser = nextInst;
            limitingUsee = numEntries_;
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

    unsigned insertEntry(unsigned num, uint8_t* data, BufferOffset off, LifoAlloc& lifoAlloc) {
        if (numEntries_ + num >= buffSize) {
            // Grow the poolData_ vector.
            buffSize *= 2;
            PoolAllocUnit* tmp = lifoAlloc.newArrayUninitialized<PoolAllocUnit>(buffSize);
            if (poolData_ == nullptr) {
                buffSize = 0;
                return -1;
            }
            mozilla::PodCopy(tmp, poolData_, numEntries_);
            poolData_ = tmp;
        }
        mozilla::PodCopy(&poolData_[numEntries_], (PoolAllocUnit*)data, num);
        loadOffsets.append(off.getOffset());
        unsigned ret = numEntries_;
        numEntries_ += num;
        return ret;
    }

    bool reset(LifoAlloc& a) {
        numEntries_ = 0;
        buffSize = 8;
        poolData_ = static_cast<PoolAllocUnit*>(a.alloc(buffSize * sizeof(PoolAllocUnit)));
        if (poolData_ == nullptr)
            return false;

        new (&loadOffsets) LoadOffsets;

        limitingUser = BufferOffset();
        limitingUsee = -1;
        return true;
    }

};


template <size_t SliceSize, size_t InstSize>
struct BufferSliceTail : public BufferSlice<SliceSize> {
  private:
    // Bit vector to record which instructions in the slice have a branch, so
    // that they can be patched when the final positions are known.
    mozilla::Array<uint8_t, (SliceSize / InstSize) / 8> isBranch_;
  public:
    Pool* pool;
    // Flag when the last instruction in the slice is a 'natural' pool guard. A
    // natural pool guard is a branch in the code that was not explicitly added
    // to branch around the pool. For now an explict guard branch is always
    // emitted, so this will always be false.
    bool isNatural : 1;
    BufferSliceTail* getNext() const {
        return (BufferSliceTail*)this->next_;
    }
    explicit BufferSliceTail() : pool(nullptr), isNatural(true) {
        static_assert(SliceSize % (8 * InstSize) == 0, "SliceSize must be a multple of 8 * InstSize.");
        mozilla::PodArrayZero(isBranch_);
    }
    void markNextAsBranch() {
        // The caller is expected to ensure that the nodeSize_ < SliceSize. See
        // the assembler's markNextAsBranch() method which firstly creates a new
        // slice if necessary.
        MOZ_ASSERT(this->nodeSize_ % InstSize == 0);
        MOZ_ASSERT(this->nodeSize_ < SliceSize);
        size_t idx = this->nodeSize_ / InstSize;
        isBranch_[idx >> 3] |= 1 << (idx & 0x7);
    }
    bool isBranch(unsigned idx) const {
        MOZ_ASSERT(idx < this->nodeSize_ / InstSize);
        return (isBranch_[idx >> 3] >> (idx & 0x7)) & 1;
    }
    bool isNextBranch() const {
        size_t size = this->nodeSize_;
        MOZ_ASSERT(size < SliceSize);
        return isBranch(size / InstSize);
    }
};


// The InstSize is the sizeof(Inst) but is needed here because the buffer is
// defined before the Instruction.
template <size_t SliceSize, size_t InstSize, class Inst, class Asm>
struct AssemblerBufferWithConstantPools : public AssemblerBuffer<SliceSize, Inst> {
  private:
    // The PoolEntry index counter. Each PoolEntry is given a unique index,
    // counting up from zero, and these can be mapped back to the actual pool
    // entry offset after finishing the buffer, see poolEntryOffset().
    size_t poolEntryCount;
  public:
    class PoolEntry {
        size_t index_;
      public:
        explicit PoolEntry(size_t index) : index_(index) {
        }
        PoolEntry() : index_(-1) {
        }
        size_t index() const {
            return index_;
        }
    };

  private:
    typedef BufferSliceTail<SliceSize, InstSize> BufferSlice;
    typedef AssemblerBuffer<SliceSize, Inst> Parent;

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
        // The number of instructions before the start of the pool. This is a
        // buffer offset, excluding the pools, but including the guards.
        size_t offset;
        // The size of the pool, including padding, but excluding the guard.
        unsigned size;
        // The actual offset of the end of the last pool, in bytes from the
        // beginning of the buffer. The difference between the offset and the
        // finalPos gives the cumulative number of bytes allocated to pools so
        // far. This is used when mapping a buffer offset to its actual offset;
        // the PoolInfo is found using the offset and then the difference can be
        // applied to convert to the actual offset.
        size_t finalPos;
        // Link back to the BufferSlice after which this pool is to be inserted.
        BufferSlice* slice;
    };
    // The number of times we've dumped a pool, and the number of Pool Info
    // elements used in the poolInfo vector below.
    unsigned numDumps_;
    // The number of PoolInfo elements available in the poolInfo vector.
    size_t poolInfoSize_;
    // Pointer to a vector of PoolInfo structures. Grown as needed.
    PoolInfo* poolInfo_;

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
    // The buffer slices are in a double linked list. Pointers to the head and
    // tail of this list:
    BufferSlice* getHead() const {
        return (BufferSlice*)this->head;
    }
    BufferSlice* getTail() const {
        return (BufferSlice*)this->tail;
    }

    virtual BufferSlice* newSlice(LifoAlloc& a) {
        BufferSlice* slice = a.new_<BufferSlice>();
        if (!slice) {
            this->m_oom = true;
            return nullptr;
        }
        return slice;
    }

  public:
    AssemblerBufferWithConstantPools(unsigned guardSize, unsigned headerSize,
                                     size_t instBufferAlign, size_t poolMaxOffset,
                                     unsigned pcBias, uint32_t alignFillInst, uint32_t nopFillInst,
                                     unsigned nopFill = 0)
        : poolEntryCount(0), guardSize_(guardSize), headerSize_(headerSize),
          poolMaxOffset_(poolMaxOffset), pcBias_(pcBias),
          instBufferAlign_(instBufferAlign),
          numDumps_(0), poolInfoSize_(8), poolInfo_(nullptr),
          canNotPlacePool_(false), alignFillInst_(alignFillInst),
          nopFillInst_(nopFillInst), nopFill_(nopFill), inhibitNops_(false),
          id(-1)
    {
    }

    // We need to wait until an AutoJitContextAlloc is created by the
    // MacroAssembler before allocating any space.
    void initWithAllocator() {
        poolInfo_ = this->lifoAlloc_.template newArrayUninitialized<PoolInfo>(poolInfoSize_);

        new (&pool_) Pool (poolMaxOffset_, pcBias_, this->lifoAlloc_);
        if (pool_.poolData() == nullptr)
            this->fail_oom();
    }

  private:
    const PoolInfo& getLastPoolInfo() const {
        // Return the PoolInfo for the last pool dumped, or a zero PoolInfo
        // structure if none have been dumped.
        static const PoolInfo nil = {0, 0, 0, nullptr};

        if (numDumps_ > 0)
            return poolInfo_[numDumps_ - 1];

        return nil;
    }

    size_t sizeExcludingCurrentPool() const {
        // Return the actual size of the buffer, excluding the current pending
        // pool.
        size_t codeEnd = this->nextOffset().getOffset();
        // Convert to an actual final offset, using the cumulative pool size
        // noted in the PoolInfo.
        PoolInfo pi = getLastPoolInfo();
        return codeEnd + (pi.finalPos - pi.offset);
    }

  public:
    size_t size() const {
        // Return the current actual size of the buffer. This is only accurate
        // if there are no pending pool entries to dump, check.
        MOZ_ASSERT(pool_.numEntries() == 0);
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

    void markNextAsBranch() {
        // If the previous thing inserted was the last instruction of the node,
        // then whoops, we want to mark the first instruction of the next node.
        this->ensureSpace(InstSize);
        MOZ_ASSERT(this->getTail() != nullptr);
        this->getTail()->markNextAsBranch();
    }

    bool isNextBranch() const {
        MOZ_ASSERT(this->getTail() != nullptr);
        return this->getTail()->isNextBranch();
    }

    int insertEntryForwards(unsigned numInst, unsigned numPoolEntries, uint8_t* inst, uint8_t* data) {
        size_t nextOffset = sizeExcludingCurrentPool();
        size_t poolOffset = nextOffset + (numInst + guardSize_ + headerSize_) * InstSize;

        // Perform the necessary range checks.

        // If inserting pool entries then find a new limiter before we do the
        // range check.
        if (numPoolEntries)
            pool_.updateLimiter(BufferOffset(nextOffset));

        if (pool_.checkFull(poolOffset)) {
            if (numPoolEntries)
                JitSpew(JitSpew_Pools, "[%d] Inserting pool entry caused a spill", id);
            else
                JitSpew(JitSpew_Pools, "[%d] Inserting instruction(%d) caused a spill", id,
                        sizeExcludingCurrentPool());

            finishPool();
            if (this->oom())
                return uint32_t(-1);
            return insertEntryForwards(numInst, numPoolEntries, inst, data);
        }
        if (numPoolEntries)
            return pool_.insertEntry(numPoolEntries, data, this->nextOffset(), this->lifoAlloc_);

        // The pool entry index is returned above when allocating an entry, but
        // when not allocating an entry a dummy value is returned - it is not
        // expected to be used by the caller.
        return UINT_MAX;
    }

  public:
    BufferOffset allocEntry(size_t numInst, unsigned numPoolEntries,
                            uint8_t* inst, uint8_t* data, PoolEntry* pe = nullptr,
                            bool markAsBranch = false) {
        // The alloction of pool entries is not supported in a no-pool region,
        // check.
        MOZ_ASSERT_IF(numPoolEntries, !canNotPlacePool_);

        if (this->oom() && !this->bail())
            return BufferOffset();

        insertNopFill();

#ifdef DEBUG
        if (numPoolEntries) {
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
        // Now to get an instruction to write.
        PoolEntry retPE;
        if (numPoolEntries) {
            if (this->oom())
                return BufferOffset();
            JitSpew(JitSpew_Pools, "[%d] Entry has index %u, offset %u", id, index,
                    sizeExcludingCurrentPool());
            Asm::InsertIndexIntoTag(inst, index);
            // Figure out the offset within the pool entries.
            retPE = PoolEntry(poolEntryCount);
            poolEntryCount += numPoolEntries;
        }
        // Now inst is a valid thing to insert into the instruction stream.
        if (pe != nullptr)
            *pe = retPE;
        if (markAsBranch)
            markNextAsBranch();
        return this->putBlob(numInst * InstSize, inst);
    }

    BufferOffset putInt(uint32_t value, bool markAsBranch = false) {
        return allocEntry(1, 0, (uint8_t*)&value, nullptr, nullptr, markAsBranch);
    }

  private:
    PoolInfo getPoolData(BufferSlice* perforatedSlice, size_t perfOffset) const {
        PoolInfo pi = getLastPoolInfo();
        size_t prevOffset = pi.offset;
        size_t prevEnd = pi.finalPos;
        // Calculate the offset of the start of this pool.
        size_t initOffset = perfOffset + (prevEnd - prevOffset);
        size_t finOffset = initOffset;
        if (pool_.numEntries() != 0) {
            finOffset += headerSize_ * InstSize;
            finOffset += pool_.getPoolSize();
        }

        PoolInfo ret;
        ret.offset = perfOffset;
        ret.size = finOffset - initOffset;
        ret.finalPos = finOffset;
        ret.slice = perforatedSlice;
        return ret;
    }

    void finishPool() {
        JitSpew(JitSpew_Pools, "[%d] Attempting to finish pool %d with %d entries.", id,
                numDumps_, pool_.numEntries());

        if (pool_.numEntries() == 0) {
            // If there is no data in the pool being dumped, don't dump anything.
            JitSpew(JitSpew_Pools, "[%d] Aborting because the pool is empty", id);
            return;
        }

        // Should not be placing a pool in a no-pool region, check.
        MOZ_ASSERT(!canNotPlacePool_);

        // Dump the pool with a guard branch around the pool.
        BufferOffset branch = this->nextOffset();
        // Mark and emit the guard branch.
        markNextAsBranch();
        this->putBlob(guardSize_ * InstSize, nullptr);
        BufferOffset afterPool = this->nextOffset();
        Asm::WritePoolGuard(branch, this->getInst(branch), afterPool);

        // Perforate the buffer which finishes the current slice and allocates a
        // new slice. This is necessary because Pools are always placed after
        // the end of a slice.
        BufferSlice* perforatedSlice = getTail();
        BufferOffset perforation = this->nextOffset();
        Parent::perforate();
        perforatedSlice->isNatural = false;
        JitSpew(JitSpew_Pools, "[%d] Adding a perforation at offset %d", id, perforation.getOffset());

        // With the pool's final position determined it is now possible to patch
        // the instructions that reference entries in this pool, and this is
        // done incrementally as each pool is finished.
        size_t poolOffset = perforation.getOffset();
        // The cumulative pool size.
        PoolInfo pi = getLastPoolInfo();
        size_t magicAlign = pi.finalPos - pi.offset;
        // Convert poolOffset to it's actual final offset.
        poolOffset += magicAlign;
        // Account for the header.
        poolOffset += headerSize_ * InstSize;

        unsigned idx = 0;
        for (BufferOffset* iter = pool_.loadOffsets.begin();
             iter != pool_.loadOffsets.end();
             ++iter, ++idx)
        {
            // All entries should be before the pool.
            MOZ_ASSERT(iter->getOffset() < perforation.getOffset());

            // Everything here is known so we can safely do the necessary
            // substitutions.
            Inst* inst = this->getInst(*iter);
            size_t codeOffset = poolOffset - iter->getOffset();

            // That is, PatchConstantPoolLoad wants to be handed the address of
            // the pool entry that is being loaded.  We need to do a non-trivial
            // amount of math here, since the pool that we've made does not
            // actually reside there in memory.
            JitSpew(JitSpew_Pools, "[%d] Fixing entry %d offset to %u", id, idx,
                    codeOffset - magicAlign);
            Asm::PatchConstantPoolLoad(inst, (uint8_t*)inst + codeOffset - magicAlign);
        }

        // Record the pool info.
        if (numDumps_ >= poolInfoSize_) {
            // Grow the poolInfo vector.
            poolInfoSize_ *= 2;
            PoolInfo* tmp = this->lifoAlloc_.template newArrayUninitialized<PoolInfo>(poolInfoSize_);
            if (tmp == nullptr) {
                this->fail_oom();
                return;
            }
            mozilla::PodCopy(tmp, poolInfo_, numDumps_);
            poolInfo_ = tmp;
        }
        PoolInfo newPoolInfo = getPoolData(perforatedSlice, perforation.getOffset());
        MOZ_ASSERT(numDumps_ < poolInfoSize_);
        poolInfo_[numDumps_] = newPoolInfo;
        numDumps_++;

        // Bind the current pool to the perforation point, copying the pool
        // state into the BufferSlice.
        Pool** tmp = &perforatedSlice->pool;
        *tmp = static_cast<Pool*>(this->lifoAlloc_.alloc(sizeof(Pool)));
        if (tmp == nullptr) {
            this->fail_oom();
            return;
        }
        mozilla::PodCopy(*tmp, &pool_, 1);

        // Reset everything to the state that it was in when we started.
        if (!pool_.reset(this->lifoAlloc_)) {
            this->fail_oom();
            return;
        }
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
        size_t poolOffset = sizeExcludingCurrentPool() + (maxInst + guardSize_ + headerSize_) * InstSize;

        if (pool_.checkFull(poolOffset)) {
            JitSpew(JitSpew_Pools, "[%d] No-Pool instruction(%d) caused a spill.", id,
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

    size_t poolSizeBefore(size_t offset) const {
        // Linear search of the poolInfo to find the pool before the given
        // buffer offset, and then return the cumulative size of the pools
        // before this offset. This is used to convert a buffer offset to its
        // final actual offset.
        unsigned cur = 0;
        while (cur < numDumps_ && poolInfo_[cur].offset <= offset)
            cur++;
        if (cur == 0)
            return 0;
        return poolInfo_[cur - 1].finalPos - poolInfo_[cur - 1].offset;
    }

    void align(unsigned alignment)
    {
        // Restrict the alignment to a power of two for now.
        MOZ_ASSERT(IsPowerOfTwo(alignment));

        // A pool many need to be dumped at this point, so insert NOP fill here.
        insertNopFill();

        // Check if the code position can be aligned without dumping a pool.
        unsigned requiredFill = sizeExcludingCurrentPool() & (alignment - 1);
        if (requiredFill == 0)
            return;
        requiredFill = alignment - requiredFill;

        // Add an InstSize because it is probably not useful for a pool to be
        // dumped at the aligned code position.
        uint32_t poolOffset = sizeExcludingCurrentPool() + requiredFill
                              + (1 + guardSize_ + headerSize_) * InstSize;
        if (pool_.checkFull(poolOffset)) {
            // Alignment would cause a pool dump, so dump the pool now.
            JitSpew(JitSpew_Pools, "[%d] Alignment of %d at %d caused a spill.", id, alignment,
                    sizeExcludingCurrentPool());
            finishPool();
        }

        inhibitNops_ = true;
        while (sizeExcludingCurrentPool() & (alignment - 1))
            putInt(alignFillInst_);
        inhibitNops_ = false;
    }

  private:
    void patchBranch(Inst* i, unsigned curpool, BufferOffset branch) {
        const Inst* ci = i;
        ptrdiff_t offset = Asm::GetBranchOffset(ci);
        // If the offset is 0, then there is nothing to do.
        if (offset == 0)
            return;
        unsigned destOffset = branch.getOffset() + offset;
        if (offset > 0) {
            while (curpool < numDumps_ && poolInfo_[curpool].offset <= destOffset) {
                offset += poolInfo_[curpool].size;
                curpool++;
            }
        } else {
            // Ignore the pool that comes next, since this is a backwards
            // branch.
            for (int p = curpool - 1; p >= 0 && poolInfo_[p].offset > destOffset; p--)
                offset -= poolInfo_[p].size;
        }
        Asm::RetargetNearBranch(i, offset, false);
    }

  public:
    void executableCopy(uint8_t* dest_) {
        if (this->oom())
            return;
        // The pools should have all been flushed, check.
        MOZ_ASSERT(pool_.numEntries() == 0);
        // Expecting the dest_ to already be aligned to instBufferAlign_, check.
        MOZ_ASSERT(uintptr_t(dest_) == ((uintptr_t(dest_) + instBufferAlign_ - 1) & ~(instBufferAlign_ - 1)));
        // Assuming the Instruction size is 4 bytes, check.
        static_assert(InstSize == sizeof(uint32_t), "Assuming instruction size is 4 bytes");
        uint32_t* dest = (uint32_t*)dest_;
        unsigned curIndex = 0;
        size_t curInstOffset = 0;
        for (BufferSlice* cur = getHead(); cur != nullptr; cur = cur->getNext()) {
            uint32_t* src = (uint32_t*)&cur->instructions;
            unsigned numInsts = cur->size() / InstSize;
            for (unsigned idx = 0; idx < numInsts; idx++, curInstOffset += InstSize) {
                // Is the current instruction a branch?
                if (cur->isBranch(idx)) {
                    // It's a branch, fix up the branchiness!
                    patchBranch((Inst*)&src[idx], curIndex, BufferOffset(curInstOffset));
                }
                dest[idx] = src[idx];
            }
            dest += numInsts;
            Pool* curPool = cur->pool;
            if (curPool != nullptr) {
                // Have the repatcher move on to the next pool.
                curIndex++;
                // Copying the pool into place.
                uint8_t* poolDest = (uint8_t*)dest;
                Asm::WritePoolHeader(poolDest, curPool, cur->isNatural);
                poolDest += headerSize_ * InstSize;

                memcpy(poolDest, curPool->poolData(), curPool->getPoolSize());
                poolDest += curPool->getPoolSize();

                dest = (uint32_t*)poolDest;
            }
        }
    }

  public:
    size_t poolEntryOffset(PoolEntry pe) const {
        // Linear search of all the pools to locate the given PoolEntry's
        // PoolInfo and to return the actual offset of the pool entry in the
        // finished code object.
        size_t offset = pe.index() * sizeof(PoolAllocUnit);
        for (unsigned poolNum = 0; poolNum < numDumps_; poolNum++) {
            PoolInfo* pi = &poolInfo_[poolNum];
            unsigned size = pi->slice->pool->getPoolSize();
            if (size > offset)
                return pi->finalPos - pi->size + headerSize_ * InstSize + offset;
            offset -= size;
        }
        MOZ_CRASH("Entry is not in a pool");
    }
};

} // ion
} // js
#endif /* jit_shared_IonAssemblerBufferWithConstantPools_h */
