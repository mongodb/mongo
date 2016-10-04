/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Heap_h
#define gc_Heap_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/PodOperations.h"

#include <stddef.h>
#include <stdint.h>

#include "jsfriendapi.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "jsutil.h"

#include "ds/BitArray.h"
#include "gc/Memory.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/TracingAPI.h"

struct JSRuntime;

namespace JS {
namespace shadow {
struct Runtime;
} // namespace shadow
} // namespace JS

namespace js {

class AutoLockGC;
class FreeOp;

#ifdef DEBUG
extern bool
RuntimeFromMainThreadIsHeapMajorCollecting(JS::shadow::Zone* shadowZone);

// Barriers can't be triggered during backend Ion compilation, which may run on
// a helper thread.
extern bool
CurrentThreadIsIonCompiling();
#endif

extern bool
UnmarkGrayCellRecursively(gc::Cell* cell, JS::TraceKind kind);

extern void
TraceManuallyBarrieredGenericPointerEdge(JSTracer* trc, gc::Cell** thingp, const char* name);

namespace gc {

struct Arena;
class ArenaList;
class SortedArenaList;
struct ArenaHeader;
struct Chunk;

/*
 * This flag allows an allocation site to request a specific heap based upon the
 * estimated lifetime or lifetime requirements of objects allocated from that
 * site.
 */
enum InitialHeap {
    DefaultHeap,
    TenuredHeap
};

/* The GC allocation kinds. */
// FIXME: uint8_t would make more sense for the underlying type, but causes
// miscompilations in GCC (fixed in 4.8.5 and 4.9.3). See also bug 1143966.
enum class AllocKind {
    FIRST,
    OBJECT_FIRST = FIRST,
    FUNCTION = FIRST,
    FUNCTION_EXTENDED,
    OBJECT0,
    OBJECT0_BACKGROUND,
    OBJECT2,
    OBJECT2_BACKGROUND,
    OBJECT4,
    OBJECT4_BACKGROUND,
    OBJECT8,
    OBJECT8_BACKGROUND,
    OBJECT12,
    OBJECT12_BACKGROUND,
    OBJECT16,
    OBJECT16_BACKGROUND,
    OBJECT_LIMIT,
    OBJECT_LAST = OBJECT_LIMIT - 1,
    SCRIPT,
    LAZY_SCRIPT,
    SHAPE,
    ACCESSOR_SHAPE,
    BASE_SHAPE,
    OBJECT_GROUP,
    FAT_INLINE_STRING,
    STRING,
    EXTERNAL_STRING,
    SYMBOL,
    JITCODE,
    LIMIT,
    LAST = LIMIT - 1
};

static_assert(int(AllocKind::FIRST) == 0, "Various places depend on AllocKind starting at 0, "
                                          "please audit them carefully!");
static_assert(int(AllocKind::OBJECT_FIRST) == 0, "Various places depend on AllocKind::OBJECT_FIRST "
                                                 "being 0, please audit them carefully!");

inline bool
IsObjectAllocKind(AllocKind kind)
{
    return kind >= AllocKind::OBJECT_FIRST && kind <= AllocKind::OBJECT_LAST;
}

inline bool
IsValidAllocKind(AllocKind kind)
{
    return kind >= AllocKind::FIRST && kind <= AllocKind::LAST;
}

inline bool IsAllocKind(AllocKind kind)
{
    return kind >= AllocKind::FIRST && kind <= AllocKind::LIMIT;
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all alloc kinds.
inline decltype(mozilla::MakeEnumeratedRange<int>(AllocKind::FIRST, AllocKind::LIMIT))
AllAllocKinds()
{
    return mozilla::MakeEnumeratedRange<int>(AllocKind::FIRST, AllocKind::LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all object alloc kinds.
inline decltype(mozilla::MakeEnumeratedRange<int>(AllocKind::OBJECT_FIRST, AllocKind::OBJECT_LIMIT))
ObjectAllocKinds()
{
    return mozilla::MakeEnumeratedRange<int>(AllocKind::OBJECT_FIRST, AllocKind::OBJECT_LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over alloc kinds from |first| to |limit|, exclusive.
inline decltype(mozilla::MakeEnumeratedRange<int>(AllocKind::FIRST, AllocKind::LIMIT))
SomeAllocKinds(AllocKind first = AllocKind::FIRST, AllocKind limit = AllocKind::LIMIT)
{
    MOZ_ASSERT(IsAllocKind(first), "|first| is not a valid AllocKind!");
    MOZ_ASSERT(IsAllocKind(limit), "|limit| is not a valid AllocKind!");
    return mozilla::MakeEnumeratedRange<int>(first, limit);
}

// AllAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular alloc kind.
template<typename ValueType> using AllAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, AllocKind::LIMIT, ValueType>;

// ObjectAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular object alloc kind.
template<typename ValueType> using ObjectAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, AllocKind::OBJECT_LIMIT, ValueType>;

static inline JS::TraceKind
MapAllocToTraceKind(AllocKind kind)
{
    static const JS::TraceKind map[] = {
        JS::TraceKind::Object,       /* AllocKind::FUNCTION */
        JS::TraceKind::Object,       /* AllocKind::FUNCTION_EXTENDED */
        JS::TraceKind::Object,       /* AllocKind::OBJECT0 */
        JS::TraceKind::Object,       /* AllocKind::OBJECT0_BACKGROUND */
        JS::TraceKind::Object,       /* AllocKind::OBJECT2 */
        JS::TraceKind::Object,       /* AllocKind::OBJECT2_BACKGROUND */
        JS::TraceKind::Object,       /* AllocKind::OBJECT4 */
        JS::TraceKind::Object,       /* AllocKind::OBJECT4_BACKGROUND */
        JS::TraceKind::Object,       /* AllocKind::OBJECT8 */
        JS::TraceKind::Object,       /* AllocKind::OBJECT8_BACKGROUND */
        JS::TraceKind::Object,       /* AllocKind::OBJECT12 */
        JS::TraceKind::Object,       /* AllocKind::OBJECT12_BACKGROUND */
        JS::TraceKind::Object,       /* AllocKind::OBJECT16 */
        JS::TraceKind::Object,       /* AllocKind::OBJECT16_BACKGROUND */
        JS::TraceKind::Script,       /* AllocKind::SCRIPT */
        JS::TraceKind::LazyScript,   /* AllocKind::LAZY_SCRIPT */
        JS::TraceKind::Shape,        /* AllocKind::SHAPE */
        JS::TraceKind::Shape,        /* AllocKind::ACCESSOR_SHAPE */
        JS::TraceKind::BaseShape,    /* AllocKind::BASE_SHAPE */
        JS::TraceKind::ObjectGroup,  /* AllocKind::OBJECT_GROUP */
        JS::TraceKind::String,       /* AllocKind::FAT_INLINE_STRING */
        JS::TraceKind::String,       /* AllocKind::STRING */
        JS::TraceKind::String,       /* AllocKind::EXTERNAL_STRING */
        JS::TraceKind::Symbol,       /* AllocKind::SYMBOL */
        JS::TraceKind::JitCode,      /* AllocKind::JITCODE */
    };

    static_assert(MOZ_ARRAY_LENGTH(map) == size_t(AllocKind::LIMIT),
                  "AllocKind-to-TraceKind mapping must be in sync");
    return map[size_t(kind)];
}

/*
 * This must be an upper bound, but we do not need the least upper bound, so
 * we just exclude non-background objects.
 */
static const size_t MAX_BACKGROUND_FINALIZE_KINDS =
    size_t(AllocKind::LIMIT) - size_t(AllocKind::OBJECT_LIMIT) / 2;

class TenuredCell;

// A GC cell is the base class for all GC things.
struct Cell
{
  public:
    MOZ_ALWAYS_INLINE bool isTenured() const { return !IsInsideNursery(this); }
    MOZ_ALWAYS_INLINE const TenuredCell& asTenured() const;
    MOZ_ALWAYS_INLINE TenuredCell& asTenured();

    inline JSRuntime* runtimeFromMainThread() const;
    inline JS::shadow::Runtime* shadowRuntimeFromMainThread() const;

    // Note: Unrestricted access to the runtime of a GC thing from an arbitrary
    // thread can easily lead to races. Use this method very carefully.
    inline JSRuntime* runtimeFromAnyThread() const;
    inline JS::shadow::Runtime* shadowRuntimeFromAnyThread() const;

    // May be overridden by GC thing kinds that have a compartment pointer.
    inline JSCompartment* maybeCompartment() const { return nullptr; }

    inline StoreBuffer* storeBuffer() const;

    inline JS::TraceKind getTraceKind() const;

    static MOZ_ALWAYS_INLINE bool needWriteBarrierPre(JS::Zone* zone);

#ifdef DEBUG
    inline bool isAligned() const;
#endif

  protected:
    inline uintptr_t address() const;
    inline Chunk* chunk() const;
};

// A GC TenuredCell gets behaviors that are valid for things in the Tenured
// heap, such as access to the arena header and mark bits.
class TenuredCell : public Cell
{
  public:
    // Construct a TenuredCell from a void*, making various sanity assertions.
    static MOZ_ALWAYS_INLINE TenuredCell* fromPointer(void* ptr);
    static MOZ_ALWAYS_INLINE const TenuredCell* fromPointer(const void* ptr);

    // Mark bit management.
    MOZ_ALWAYS_INLINE bool isMarked(uint32_t color = BLACK) const;
    MOZ_ALWAYS_INLINE bool markIfUnmarked(uint32_t color = BLACK) const;
    MOZ_ALWAYS_INLINE void unmark(uint32_t color) const;
    MOZ_ALWAYS_INLINE void copyMarkBitsFrom(const TenuredCell* src);

    // Note: this is in TenuredCell because JSObject subclasses are sometimes
    // used tagged.
    static MOZ_ALWAYS_INLINE bool isNullLike(const Cell* thing) { return !thing; }

    // Access to the arena header.
    inline ArenaHeader* arenaHeader() const;
    inline AllocKind getAllocKind() const;
    inline JS::TraceKind getTraceKind() const;
    inline JS::Zone* zone() const;
    inline JS::Zone* zoneFromAnyThread() const;
    inline bool isInsideZone(JS::Zone* zone) const;

    MOZ_ALWAYS_INLINE JS::shadow::Zone* shadowZone() const {
        return JS::shadow::Zone::asShadowZone(zone());
    }
    MOZ_ALWAYS_INLINE JS::shadow::Zone* shadowZoneFromAnyThread() const {
        return JS::shadow::Zone::asShadowZone(zoneFromAnyThread());
    }

    static MOZ_ALWAYS_INLINE void readBarrier(TenuredCell* thing);
    static MOZ_ALWAYS_INLINE void writeBarrierPre(TenuredCell* thing);

    static MOZ_ALWAYS_INLINE void writeBarrierPost(void* cellp, TenuredCell* prior,
                                                   TenuredCell* next);

#ifdef DEBUG
    inline bool isAligned() const;
#endif
};

/* Cells are aligned to CellShift, so the largest tagged null pointer is: */
const uintptr_t LargestTaggedNullCellPointer = (1 << CellShift) - 1;

/*
 * The mark bitmap has one bit per each GC cell. For multi-cell GC things this
 * wastes space but allows to avoid expensive devisions by thing's size when
 * accessing the bitmap. In addition this allows to use some bits for colored
 * marking during the cycle GC.
 */
const size_t ArenaCellCount = size_t(1) << (ArenaShift - CellShift);
const size_t ArenaBitmapBits = ArenaCellCount;
const size_t ArenaBitmapBytes = ArenaBitmapBits / 8;
const size_t ArenaBitmapWords = ArenaBitmapBits / JS_BITS_PER_WORD;

/*
 * A FreeSpan represents a contiguous sequence of free cells in an Arena. It
 * can take two forms.
 *
 * - In an empty span, |first| and |last| are both zero.
 *
 * - In a non-empty span, |first| is the address of the first free thing in the
 *   span, and |last| is the address of the last free thing in the span.
 *   Furthermore, the memory pointed to by |last| holds a FreeSpan structure
 *   that points to the next span (which may be empty); this works because
 *   sizeof(FreeSpan) is less than the smallest thingSize.
 */
class FreeSpan
{
    friend class ArenaCellIterImpl;
    friend class CompactFreeSpan;
    friend class FreeList;

    uintptr_t   first;
    uintptr_t   last;

  public:
    // This inits just |first| and |last|; if the span is non-empty it doesn't
    // do anything with the next span stored at |last|.
    void initBoundsUnchecked(uintptr_t first, uintptr_t last) {
        this->first = first;
        this->last = last;
    }

    void initBounds(uintptr_t first, uintptr_t last) {
        initBoundsUnchecked(first, last);
        checkSpan();
    }

    void initAsEmpty() {
        first = 0;
        last = 0;
        MOZ_ASSERT(isEmpty());
    }

    // This sets |first| and |last|, and also sets the next span stored at
    // |last| as empty. (As a result, |firstArg| and |lastArg| cannot represent
    // an empty span.)
    void initFinal(uintptr_t firstArg, uintptr_t lastArg, size_t thingSize) {
        first = firstArg;
        last = lastArg;
        FreeSpan* lastSpan = reinterpret_cast<FreeSpan*>(last);
        lastSpan->initAsEmpty();
        MOZ_ASSERT(!isEmpty());
        checkSpan(thingSize);
    }

    bool isEmpty() const {
        checkSpan();
        return !first;
    }

    static size_t offsetOfFirst() {
        return offsetof(FreeSpan, first);
    }

    static size_t offsetOfLast() {
        return offsetof(FreeSpan, last);
    }

    // Like nextSpan(), but no checking of the following span is done.
    FreeSpan* nextSpanUnchecked() const {
        return reinterpret_cast<FreeSpan*>(last);
    }

    const FreeSpan* nextSpan() const {
        MOZ_ASSERT(!isEmpty());
        return nextSpanUnchecked();
    }

    uintptr_t arenaAddress() const {
        MOZ_ASSERT(!isEmpty());
        return first & ~ArenaMask;
    }

#ifdef DEBUG
    bool isWithinArena(uintptr_t arenaAddr) const {
        MOZ_ASSERT(!(arenaAddr & ArenaMask));
        MOZ_ASSERT(!isEmpty());
        return arenaAddress() == arenaAddr;
    }
#endif

    size_t length(size_t thingSize) const {
        checkSpan();
        MOZ_ASSERT((last - first) % thingSize == 0);
        return (last - first) / thingSize + 1;
    }

    bool inFreeList(uintptr_t thing) {
        for (const FreeSpan* span = this; !span->isEmpty(); span = span->nextSpan()) {
            /* If the thing comes before the current span, it's not free. */
            if (thing < span->first)
                return false;

            /* If we find it before the end of the span, it's free. */
            if (thing <= span->last)
                return true;
        }
        return false;
    }

  private:
    // Some callers can pass in |thingSize| easily, and we can do stronger
    // checking in that case.
    void checkSpan(size_t thingSize = 0) const {
#ifdef DEBUG
        if (!first || !last) {
            MOZ_ASSERT(!first && !last);
            // An empty span.
            return;
        }

        // |first| and |last| must be ordered appropriately, belong to the same
        // arena, and be suitably aligned.
        MOZ_ASSERT(first <= last);
        MOZ_ASSERT((first & ~ArenaMask) == (last & ~ArenaMask));
        MOZ_ASSERT((last - first) % (thingSize ? thingSize : CellSize) == 0);

        // If there's a following span, it must be from the same arena, it must
        // have a higher address, and the gap must be at least 2*thingSize.
        FreeSpan* next = reinterpret_cast<FreeSpan*>(last);
        if (next->first) {
            MOZ_ASSERT(next->last);
            MOZ_ASSERT((first & ~ArenaMask) == (next->first & ~ArenaMask));
            MOZ_ASSERT(thingSize
                       ? last + 2 * thingSize <= next->first
                       : last < next->first);
        }
#endif
    }
};

class CompactFreeSpan
{
    uint16_t firstOffset_;
    uint16_t lastOffset_;

  public:
    CompactFreeSpan(size_t firstOffset, size_t lastOffset)
      : firstOffset_(firstOffset)
      , lastOffset_(lastOffset)
    {}

    void initAsEmpty() {
        firstOffset_ = 0;
        lastOffset_ = 0;
    }

    bool operator==(const CompactFreeSpan& other) const {
        return firstOffset_ == other.firstOffset_ &&
               lastOffset_  == other.lastOffset_;
    }

    void compact(FreeSpan span) {
        if (span.isEmpty()) {
            initAsEmpty();
        } else {
            static_assert(ArenaShift < 16, "Check that we can pack offsets into uint16_t.");
            uintptr_t arenaAddr = span.arenaAddress();
            firstOffset_ = span.first - arenaAddr;
            lastOffset_  = span.last  - arenaAddr;
        }
    }

    bool isEmpty() const {
        MOZ_ASSERT(!!firstOffset_ == !!lastOffset_);
        return !firstOffset_;
    }

    FreeSpan decompact(uintptr_t arenaAddr) const {
        MOZ_ASSERT(!(arenaAddr & ArenaMask));
        FreeSpan decodedSpan;
        if (isEmpty()) {
            decodedSpan.initAsEmpty();
        } else {
            MOZ_ASSERT(firstOffset_ <= lastOffset_);
            MOZ_ASSERT(lastOffset_ < ArenaSize);
            decodedSpan.initBounds(arenaAddr + firstOffset_, arenaAddr + lastOffset_);
        }
        return decodedSpan;
    }
};

class FreeList
{
    // Although |head| is private, it is exposed to the JITs via the
    // offsetOf{First,Last}() and addressOfFirstLast() methods below.
    // Therefore, any change in the representation of |head| will require
    // updating the relevant JIT code.
    FreeSpan head;

  public:
    FreeList() {}

    static size_t offsetOfFirst() {
        return offsetof(FreeList, head) + offsetof(FreeSpan, first);
    }

    static size_t offsetOfLast() {
        return offsetof(FreeList, head) + offsetof(FreeSpan, last);
    }

    void* addressOfFirst() const {
        return (void*)&head.first;
    }

    void* addressOfLast() const {
        return (void*)&head.last;
    }

    void initAsEmpty() {
        head.initAsEmpty();
    }

    FreeSpan* getHead() { return &head; }
    void setHead(FreeSpan* span) { head = *span; }

    bool isEmpty() const {
        return head.isEmpty();
    }

#ifdef DEBUG
    uintptr_t arenaAddress() const {
        MOZ_ASSERT(!isEmpty());
        return head.arenaAddress();
    }
#endif

    ArenaHeader* arenaHeader() const {
        MOZ_ASSERT(!isEmpty());
        return reinterpret_cast<ArenaHeader*>(head.arenaAddress());
    }

#ifdef DEBUG
    bool isSameNonEmptySpan(const FreeSpan& another) const {
        MOZ_ASSERT(!isEmpty());
        MOZ_ASSERT(!another.isEmpty());
        return head.first == another.first && head.last == another.last;
    }
#endif

    MOZ_ALWAYS_INLINE TenuredCell* allocate(size_t thingSize) {
        MOZ_ASSERT(thingSize % CellSize == 0);
        head.checkSpan(thingSize);
        uintptr_t thing = head.first;
        if (thing < head.last) {
            // We have two or more things in the free list head, so we can do a
            // simple bump-allocate.
            head.first = thing + thingSize;
        } else if (MOZ_LIKELY(thing)) {
            // We have one thing in the free list head. Use it, but first
            // update the free list head to point to the subseqent span (which
            // may be empty).
            setHead(reinterpret_cast<FreeSpan*>(thing));
        } else {
            // The free list head is empty.
            return nullptr;
        }
        head.checkSpan(thingSize);
        JS_EXTRA_POISON(reinterpret_cast<void*>(thing), JS_ALLOCATED_TENURED_PATTERN, thingSize);
        MemProfiler::SampleTenured(reinterpret_cast<void*>(thing), thingSize);
        return reinterpret_cast<TenuredCell*>(thing);
    }
};

/* Every arena has a header. */
struct ArenaHeader
{
    friend struct FreeLists;

    JS::Zone* zone;

    /*
     * ArenaHeader::next has two purposes: when unallocated, it points to the
     * next available Arena's header. When allocated, it points to the next
     * arena of the same size class and compartment.
     */
    ArenaHeader* next;

  private:
    /*
     * The first span of free things in the arena. We encode it as a
     * CompactFreeSpan rather than a FreeSpan to minimize the header size.
     */
    CompactFreeSpan firstFreeSpan;

    /*
     * One of AllocKind constants or AllocKind::LIMIT when the arena does not
     * contain any GC things and is on the list of empty arenas in the GC
     * chunk.
     *
     * We use 8 bits for the allocKind so the compiler can use byte-level memory
     * instructions to access it.
     */
    size_t allocKind : 8;

    /*
     * When collecting we sometimes need to keep an auxillary list of arenas,
     * for which we use the following fields.  This happens for several reasons:
     *
     * When recursive marking uses too much stack the marking is delayed and the
     * corresponding arenas are put into a stack. To distinguish the bottom of
     * the stack from the arenas not present in the stack we use the
     * markOverflow flag to tag arenas on the stack.
     *
     * Delayed marking is also used for arenas that we allocate into during an
     * incremental GC. In this case, we intend to mark all the objects in the
     * arena, and it's faster to do this marking in bulk.
     *
     * When sweeping we keep track of which arenas have been allocated since the
     * end of the mark phase.  This allows us to tell whether a pointer to an
     * unmarked object is yet to be finalized or has already been reallocated.
     * We set the allocatedDuringIncremental flag for this and clear it at the
     * end of the sweep phase.
     *
     * To minimize the ArenaHeader size we record the next linkage as
     * arenaAddress() >> ArenaShift and pack it with the allocKind field and the
     * flags.
     */
  public:
    size_t       hasDelayedMarking : 1;
    size_t       allocatedDuringIncremental : 1;
    size_t       markOverflow : 1;
    size_t       auxNextLink : JS_BITS_PER_WORD - 8 - 1 - 1 - 1;
    static_assert(ArenaShift >= 8 + 1 + 1 + 1,
                  "ArenaHeader::auxNextLink packing assumes that ArenaShift has enough bits to "
                  "cover allocKind and hasDelayedMarking.");

    inline uintptr_t address() const;
    inline Chunk* chunk() const;

    bool allocated() const {
        MOZ_ASSERT(IsAllocKind(AllocKind(allocKind)));
        return IsValidAllocKind(AllocKind(allocKind));
    }

    void init(JS::Zone* zoneArg, AllocKind kind) {
        MOZ_ASSERT(!allocated());
        MOZ_ASSERT(!markOverflow);
        MOZ_ASSERT(!allocatedDuringIncremental);
        MOZ_ASSERT(!hasDelayedMarking);
        zone = zoneArg;

        static_assert(size_t(AllocKind::LIMIT) <= 255,
            "We must be able to fit the allockind into uint8_t.");
        allocKind = size_t(kind);

        /*
         * The firstFreeSpan is initially marked as empty (and thus the arena
         * is marked as full). See allocateFromArenaInline().
         */
        firstFreeSpan.initAsEmpty();
    }

    void setAsNotAllocated() {
        allocKind = size_t(AllocKind::LIMIT);
        markOverflow = 0;
        allocatedDuringIncremental = 0;
        hasDelayedMarking = 0;
        auxNextLink = 0;
    }

    inline uintptr_t arenaAddress() const;
    inline Arena* getArena();

    AllocKind getAllocKind() const {
        MOZ_ASSERT(allocated());
        return AllocKind(allocKind);
    }

    inline size_t getThingSize() const;

    bool hasFreeThings() const {
        return !firstFreeSpan.isEmpty();
    }

    inline bool isEmpty() const;

    void setAsFullyUsed() {
        firstFreeSpan.initAsEmpty();
    }

    inline FreeSpan getFirstFreeSpan() const;
    inline void setFirstFreeSpan(const FreeSpan* span);

#ifdef DEBUG
    void checkSynchronizedWithFreeList() const;
#endif

    inline ArenaHeader* getNextDelayedMarking() const;
    inline void setNextDelayedMarking(ArenaHeader* aheader);
    inline void unsetDelayedMarking();

    inline ArenaHeader* getNextAllocDuringSweep() const;
    inline void setNextAllocDuringSweep(ArenaHeader* aheader);
    inline void unsetAllocDuringSweep();

    inline void setNextArenaToUpdate(ArenaHeader* aheader);
    inline ArenaHeader* getNextArenaToUpdateAndUnlink();

    void unmarkAll();

    size_t countUsedCells();
    size_t countFreeCells();
};
static_assert(ArenaZoneOffset == offsetof(ArenaHeader, zone),
              "The hardcoded API zone offset must match the actual offset.");

struct Arena
{
    /*
     * Layout of an arena:
     * An arena is 4K in size and 4K-aligned. It starts with the ArenaHeader
     * descriptor followed by some pad bytes. The remainder of the arena is
     * filled with the array of T things. The pad bytes ensure that the thing
     * array ends exactly at the end of the arena.
     *
     * +-------------+-----+----+----+-----+----+
     * | ArenaHeader | pad | T0 | T1 | ... | Tn |
     * +-------------+-----+----+----+-----+----+
     *
     * <----------------------------------------> = ArenaSize bytes
     * <-------------------> = first thing offset
     */
    ArenaHeader aheader;
    uint8_t     data[ArenaSize - sizeof(ArenaHeader)];

  private:
    static JS_FRIEND_DATA(const uint32_t) ThingSizes[];
    static JS_FRIEND_DATA(const uint32_t) FirstThingOffsets[];

  public:
    static void staticAsserts();

    static size_t thingSize(AllocKind kind) {
        return ThingSizes[size_t(kind)];
    }

    static size_t firstThingOffset(AllocKind kind) {
        return FirstThingOffsets[size_t(kind)];
    }

    static size_t thingsPerArena(size_t thingSize) {
        MOZ_ASSERT(thingSize % CellSize == 0);

        /* We should be able to fit FreeSpan in any GC thing. */
        MOZ_ASSERT(thingSize >= sizeof(FreeSpan));

        return (ArenaSize - sizeof(ArenaHeader)) / thingSize;
    }

    static size_t thingsSpan(size_t thingSize) {
        return thingsPerArena(thingSize) * thingSize;
    }

    static bool isAligned(uintptr_t thing, size_t thingSize) {
        /* Things ends at the arena end. */
        uintptr_t tailOffset = (ArenaSize - thing) & ArenaMask;
        return tailOffset % thingSize == 0;
    }

    uintptr_t address() const {
        return aheader.address();
    }

    uintptr_t thingsStart(AllocKind thingKind) {
        return address() + firstThingOffset(thingKind);
    }

    uintptr_t thingsEnd() {
        return address() + ArenaSize;
    }

    void setAsFullyUnused(AllocKind thingKind);

    template <typename T>
    size_t finalize(FreeOp* fop, AllocKind thingKind, size_t thingSize);
};

static_assert(sizeof(Arena) == ArenaSize, "The hardcoded arena size must match the struct size.");

inline size_t
ArenaHeader::getThingSize() const
{
    MOZ_ASSERT(allocated());
    return Arena::thingSize(getAllocKind());
}

/*
 * The tail of the chunk info is shared between all chunks in the system, both
 * nursery and tenured. This structure is locatable from any GC pointer by
 * aligning to 1MiB.
 */
struct ChunkTrailer
{
    /* Construct a Nursery ChunkTrailer. */
    ChunkTrailer(JSRuntime* rt, StoreBuffer* sb)
      : location(gc::ChunkLocationBitNursery), storeBuffer(sb), runtime(rt)
    {}

    /* Construct a Tenured heap ChunkTrailer. */
    explicit ChunkTrailer(JSRuntime* rt)
      : location(gc::ChunkLocationBitTenuredHeap), storeBuffer(nullptr), runtime(rt)
    {}

  public:
    /* The index the chunk in the nursery, or LocationTenuredHeap. */
    uint32_t        location;
    uint32_t        padding;

    /* The store buffer for writes to things in this chunk or nullptr. */
    StoreBuffer*    storeBuffer;

    /* This provides quick access to the runtime from absolutely anywhere. */
    JSRuntime*      runtime;
};

static_assert(sizeof(ChunkTrailer) == ChunkTrailerSize,
              "ChunkTrailer size must match the API defined size.");

/* The chunk header (located at the end of the chunk to preserve arena alignment). */
struct ChunkInfo
{
    void init() {
        next = prev = nullptr;
        age = 0;
    }

  private:
    friend class ChunkPool;
    Chunk*          next;
    Chunk*          prev;

  public:
    /* Free arenas are linked together with aheader.next. */
    ArenaHeader*    freeArenasHead;

#if JS_BITS_PER_WORD == 32
    /*
     * Calculating sizes and offsets is simpler if sizeof(ChunkInfo) is
     * architecture-independent.
     */
    char            padding[20];
#endif

    /*
     * Decommitted arenas are tracked by a bitmap in the chunk header. We use
     * this offset to start our search iteration close to a decommitted arena
     * that we can allocate.
     */
    uint32_t        lastDecommittedArenaOffset;

    /* Number of free arenas, either committed or decommitted. */
    uint32_t        numArenasFree;

    /* Number of free, committed arenas. */
    uint32_t        numArenasFreeCommitted;

    /* Number of GC cycles this chunk has survived. */
    uint32_t        age;

    /* Information shared by all Chunk types. */
    ChunkTrailer    trailer;
};

/*
 * Calculating ArenasPerChunk:
 *
 * In order to figure out how many Arenas will fit in a chunk, we need to know
 * how much extra space is available after we allocate the header data. This
 * is a problem because the header size depends on the number of arenas in the
 * chunk. The two dependent fields are bitmap and decommittedArenas.
 *
 * For the mark bitmap, we know that each arena will use a fixed number of full
 * bytes: ArenaBitmapBytes. The full size of the header data is this number
 * multiplied by the eventual number of arenas we have in the header. We,
 * conceptually, distribute this header data among the individual arenas and do
 * not include it in the header. This way we do not have to worry about its
 * variable size: it gets attached to the variable number we are computing.
 *
 * For the decommitted arena bitmap, we only have 1 bit per arena, so this
 * technique will not work. Instead, we observe that we do not have enough
 * header info to fill 8 full arenas: it is currently 4 on 64bit, less on
 * 32bit. Thus, with current numbers, we need 64 bytes for decommittedArenas.
 * This will not become 63 bytes unless we double the data required in the
 * header. Therefore, we just compute the number of bytes required to track
 * every possible arena and do not worry about slop bits, since there are too
 * few to usefully allocate.
 *
 * To actually compute the number of arenas we can allocate in a chunk, we
 * divide the amount of available space less the header info (not including
 * the mark bitmap which is distributed into the arena size) by the size of
 * the arena (with the mark bitmap bytes it uses).
 */
const size_t BytesPerArenaWithHeader = ArenaSize + ArenaBitmapBytes;
const size_t ChunkDecommitBitmapBytes = ChunkSize / ArenaSize / JS_BITS_PER_BYTE;
const size_t ChunkBytesAvailable = ChunkSize - sizeof(ChunkInfo) - ChunkDecommitBitmapBytes;
const size_t ArenasPerChunk = ChunkBytesAvailable / BytesPerArenaWithHeader;

#ifdef JS_GC_SMALL_CHUNK_SIZE
static_assert(ArenasPerChunk == 62, "Do not accidentally change our heap's density.");
#else
static_assert(ArenasPerChunk == 252, "Do not accidentally change our heap's density.");
#endif

/* A chunk bitmap contains enough mark bits for all the cells in a chunk. */
struct ChunkBitmap
{
    volatile uintptr_t bitmap[ArenaBitmapWords * ArenasPerChunk];

  public:
    ChunkBitmap() { }

    MOZ_ALWAYS_INLINE void getMarkWordAndMask(const Cell* cell, uint32_t color,
                                              uintptr_t** wordp, uintptr_t* maskp)
    {
        detail::GetGCThingMarkWordAndMask(uintptr_t(cell), color, wordp, maskp);
    }

    MOZ_ALWAYS_INLINE MOZ_TSAN_BLACKLIST bool isMarked(const Cell* cell, uint32_t color) {
        uintptr_t* word, mask;
        getMarkWordAndMask(cell, color, &word, &mask);
        return *word & mask;
    }

    MOZ_ALWAYS_INLINE bool markIfUnmarked(const Cell* cell, uint32_t color) {
        uintptr_t* word, mask;
        getMarkWordAndMask(cell, BLACK, &word, &mask);
        if (*word & mask)
            return false;
        *word |= mask;
        if (color != BLACK) {
            /*
             * We use getMarkWordAndMask to recalculate both mask and word as
             * doing just mask << color may overflow the mask.
             */
            getMarkWordAndMask(cell, color, &word, &mask);
            if (*word & mask)
                return false;
            *word |= mask;
        }
        return true;
    }

    MOZ_ALWAYS_INLINE void unmark(const Cell* cell, uint32_t color) {
        uintptr_t* word, mask;
        getMarkWordAndMask(cell, color, &word, &mask);
        *word &= ~mask;
    }

    MOZ_ALWAYS_INLINE void copyMarkBit(Cell* dst, const TenuredCell* src, uint32_t color) {
        uintptr_t* word, mask;
        getMarkWordAndMask(dst, color, &word, &mask);
        *word = (*word & ~mask) | (src->isMarked(color) ? mask : 0);
    }

    void clear() {
        memset((void*)bitmap, 0, sizeof(bitmap));
    }

    uintptr_t* arenaBits(ArenaHeader* aheader) {
        static_assert(ArenaBitmapBits == ArenaBitmapWords * JS_BITS_PER_WORD,
                      "We assume that the part of the bitmap corresponding to the arena "
                      "has the exact number of words so we do not need to deal with a word "
                      "that covers bits from two arenas.");

        uintptr_t* word, unused;
        getMarkWordAndMask(reinterpret_cast<Cell*>(aheader->address()), BLACK, &word, &unused);
        return word;
    }
};

static_assert(ArenaBitmapBytes * ArenasPerChunk == sizeof(ChunkBitmap),
              "Ensure our ChunkBitmap actually covers all arenas.");
static_assert(js::gc::ChunkMarkBitmapBits == ArenaBitmapBits * ArenasPerChunk,
              "Ensure that the mark bitmap has the right number of bits.");

typedef BitArray<ArenasPerChunk> PerArenaBitmap;

const size_t ChunkPadSize = ChunkSize
                            - (sizeof(Arena) * ArenasPerChunk)
                            - sizeof(ChunkBitmap)
                            - sizeof(PerArenaBitmap)
                            - sizeof(ChunkInfo);
static_assert(ChunkPadSize < BytesPerArenaWithHeader,
              "If the chunk padding is larger than an arena, we should have one more arena.");

/*
 * Chunks contain arenas and associated data structures (mark bitmap, delayed
 * marking state).
 */
struct Chunk
{
    Arena           arenas[ArenasPerChunk];

    /* Pad to full size to ensure cache alignment of ChunkInfo. */
    uint8_t         padding[ChunkPadSize];

    ChunkBitmap     bitmap;
    PerArenaBitmap  decommittedArenas;
    ChunkInfo       info;

    static Chunk* fromAddress(uintptr_t addr) {
        addr &= ~ChunkMask;
        return reinterpret_cast<Chunk*>(addr);
    }

    static bool withinValidRange(uintptr_t addr) {
        uintptr_t offset = addr & ChunkMask;
        return Chunk::fromAddress(addr)->isNurseryChunk()
               ? offset < ChunkSize - sizeof(ChunkTrailer)
               : offset < ArenasPerChunk * ArenaSize;
    }

    static size_t arenaIndex(uintptr_t addr) {
        MOZ_ASSERT(!Chunk::fromAddress(addr)->isNurseryChunk());
        MOZ_ASSERT(withinValidRange(addr));
        return (addr & ChunkMask) >> ArenaShift;
    }

    uintptr_t address() const {
        uintptr_t addr = reinterpret_cast<uintptr_t>(this);
        MOZ_ASSERT(!(addr & ChunkMask));
        return addr;
    }

    bool unused() const {
        return info.numArenasFree == ArenasPerChunk;
    }

    bool hasAvailableArenas() const {
        return info.numArenasFree != 0;
    }

    bool isNurseryChunk() const {
        return info.trailer.storeBuffer;
    }

    ArenaHeader* allocateArena(JSRuntime* rt, JS::Zone* zone, AllocKind kind,
                               const AutoLockGC& lock);

    void releaseArena(JSRuntime* rt, ArenaHeader* aheader, const AutoLockGC& lock);
    void recycleArena(ArenaHeader* aheader, SortedArenaList& dest, AllocKind thingKind,
                      size_t thingsPerArena);

    bool decommitOneFreeArena(JSRuntime* rt, AutoLockGC& lock);
    void decommitAllArenasWithoutUnlocking(const AutoLockGC& lock);

    static Chunk* allocate(JSRuntime* rt);

  private:
    inline void init(JSRuntime* rt);

    void decommitAllArenas(JSRuntime* rt);

    /* Search for a decommitted arena to allocate. */
    unsigned findDecommittedArenaOffset();
    ArenaHeader* fetchNextDecommittedArena();

    void addArenaToFreeList(JSRuntime* rt, ArenaHeader* aheader);
    void addArenaToDecommittedList(JSRuntime* rt, const ArenaHeader* aheader);

    void updateChunkListAfterAlloc(JSRuntime* rt, const AutoLockGC& lock);
    void updateChunkListAfterFree(JSRuntime* rt, const AutoLockGC& lock);

  public:
    /* Unlink and return the freeArenasHead. */
    inline ArenaHeader* fetchNextFreeArena(JSRuntime* rt);
};

static_assert(sizeof(Chunk) == ChunkSize,
              "Ensure the hardcoded chunk size definition actually matches the struct.");
static_assert(js::gc::ChunkMarkBitmapOffset == offsetof(Chunk, bitmap),
              "The hardcoded API bitmap offset must match the actual offset.");
static_assert(js::gc::ChunkRuntimeOffset == offsetof(Chunk, info) +
                                            offsetof(ChunkInfo, trailer) +
                                            offsetof(ChunkTrailer, runtime),
              "The hardcoded API runtime offset must match the actual offset.");
static_assert(js::gc::ChunkLocationOffset == offsetof(Chunk, info) +
                                             offsetof(ChunkInfo, trailer) +
                                             offsetof(ChunkTrailer, location),
              "The hardcoded API location offset must match the actual offset.");

/*
 * Tracks the used sizes for owned heap data and automatically maintains the
 * memory usage relationship between GCRuntime and Zones.
 */
class HeapUsage
{
    /*
     * A heap usage that contains our parent's heap usage, or null if this is
     * the top-level usage container.
     */
    HeapUsage* parent_;

    /*
     * The approximate number of bytes in use on the GC heap, to the nearest
     * ArenaSize. This does not include any malloc data. It also does not
     * include not-actively-used addresses that are still reserved at the OS
     * level for GC usage. It is atomic because it is updated by both the main
     * and GC helper threads.
     */
    mozilla::Atomic<size_t, mozilla::ReleaseAcquire> gcBytes_;

  public:
    explicit HeapUsage(HeapUsage* parent)
      : parent_(parent),
        gcBytes_(0)
    {}

    size_t gcBytes() const { return gcBytes_; }

    void addGCArena() {
        gcBytes_ += ArenaSize;
        if (parent_)
            parent_->addGCArena();
    }
    void removeGCArena() {
        MOZ_ASSERT(gcBytes_ >= ArenaSize);
        gcBytes_ -= ArenaSize;
        if (parent_)
            parent_->removeGCArena();
    }

    /* Pair to adoptArenas. Adopts the attendant usage statistics. */
    void adopt(HeapUsage& other) {
        gcBytes_ += other.gcBytes_;
        other.gcBytes_ = 0;
    }
};

inline uintptr_t
ArenaHeader::address() const
{
    uintptr_t addr = reinterpret_cast<uintptr_t>(this);
    MOZ_ASSERT(addr);
    MOZ_ASSERT(!(addr & ArenaMask));
    MOZ_ASSERT(Chunk::withinValidRange(addr));
    return addr;
}

inline Chunk*
ArenaHeader::chunk() const
{
    return Chunk::fromAddress(address());
}

inline uintptr_t
ArenaHeader::arenaAddress() const
{
    return address();
}

inline Arena*
ArenaHeader::getArena()
{
    return reinterpret_cast<Arena*>(arenaAddress());
}

inline bool
ArenaHeader::isEmpty() const
{
    /* Arena is empty if its first span covers the whole arena. */
    MOZ_ASSERT(allocated());
    size_t firstThingOffset = Arena::firstThingOffset(getAllocKind());
    size_t lastThingOffset = ArenaSize - getThingSize();
    const CompactFreeSpan emptyCompactSpan(firstThingOffset, lastThingOffset);
    return firstFreeSpan == emptyCompactSpan;
}

FreeSpan
ArenaHeader::getFirstFreeSpan() const
{
#ifdef DEBUG
    checkSynchronizedWithFreeList();
#endif
    return firstFreeSpan.decompact(arenaAddress());
}

void
ArenaHeader::setFirstFreeSpan(const FreeSpan* span)
{
    MOZ_ASSERT_IF(!span->isEmpty(), span->isWithinArena(arenaAddress()));
    firstFreeSpan.compact(*span);
}

inline ArenaHeader*
ArenaHeader::getNextDelayedMarking() const
{
    MOZ_ASSERT(hasDelayedMarking);
    return &reinterpret_cast<Arena*>(auxNextLink << ArenaShift)->aheader;
}

inline void
ArenaHeader::setNextDelayedMarking(ArenaHeader* aheader)
{
    MOZ_ASSERT(!(uintptr_t(aheader) & ArenaMask));
    MOZ_ASSERT(!auxNextLink && !hasDelayedMarking);
    hasDelayedMarking = 1;
    if (aheader)
        auxNextLink = aheader->arenaAddress() >> ArenaShift;
}

inline void
ArenaHeader::unsetDelayedMarking()
{
    MOZ_ASSERT(hasDelayedMarking);
    hasDelayedMarking = 0;
    auxNextLink = 0;
}

inline ArenaHeader*
ArenaHeader::getNextAllocDuringSweep() const
{
    MOZ_ASSERT(allocatedDuringIncremental);
    return &reinterpret_cast<Arena*>(auxNextLink << ArenaShift)->aheader;
}

inline void
ArenaHeader::setNextAllocDuringSweep(ArenaHeader* aheader)
{
    MOZ_ASSERT(!auxNextLink && !allocatedDuringIncremental);
    allocatedDuringIncremental = 1;
    if (aheader)
        auxNextLink = aheader->arenaAddress() >> ArenaShift;
}

inline void
ArenaHeader::unsetAllocDuringSweep()
{
    MOZ_ASSERT(allocatedDuringIncremental);
    allocatedDuringIncremental = 0;
    auxNextLink = 0;
}

inline ArenaHeader*
ArenaHeader::getNextArenaToUpdateAndUnlink()
{
    MOZ_ASSERT(!hasDelayedMarking && !allocatedDuringIncremental && !markOverflow);
    ArenaHeader* next = &reinterpret_cast<Arena*>(auxNextLink << ArenaShift)->aheader;
    auxNextLink = 0;
    return next;
}

inline void
ArenaHeader::setNextArenaToUpdate(ArenaHeader* aheader)
{
    MOZ_ASSERT(!hasDelayedMarking && !allocatedDuringIncremental && !markOverflow);
    MOZ_ASSERT(!auxNextLink);
    auxNextLink = aheader->arenaAddress() >> ArenaShift;
}

static void
AssertValidColor(const TenuredCell* thing, uint32_t color)
{
#ifdef DEBUG
    ArenaHeader* aheader = thing->arenaHeader();
    MOZ_ASSERT(color < aheader->getThingSize() / CellSize);
#endif
}

MOZ_ALWAYS_INLINE const TenuredCell&
Cell::asTenured() const
{
    MOZ_ASSERT(isTenured());
    return *static_cast<const TenuredCell*>(this);
}

MOZ_ALWAYS_INLINE TenuredCell&
Cell::asTenured()
{
    MOZ_ASSERT(isTenured());
    return *static_cast<TenuredCell*>(this);
}

inline JSRuntime*
Cell::runtimeFromMainThread() const
{
    JSRuntime* rt = chunk()->info.trailer.runtime;
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    return rt;
}

inline JS::shadow::Runtime*
Cell::shadowRuntimeFromMainThread() const
{
    return reinterpret_cast<JS::shadow::Runtime*>(runtimeFromMainThread());
}

inline JSRuntime*
Cell::runtimeFromAnyThread() const
{
    return chunk()->info.trailer.runtime;
}

inline JS::shadow::Runtime*
Cell::shadowRuntimeFromAnyThread() const
{
    return reinterpret_cast<JS::shadow::Runtime*>(runtimeFromAnyThread());
}

inline uintptr_t
Cell::address() const
{
    uintptr_t addr = uintptr_t(this);
    MOZ_ASSERT(addr % CellSize == 0);
    MOZ_ASSERT(Chunk::withinValidRange(addr));
    return addr;
}

Chunk*
Cell::chunk() const
{
    uintptr_t addr = uintptr_t(this);
    MOZ_ASSERT(addr % CellSize == 0);
    addr &= ~ChunkMask;
    return reinterpret_cast<Chunk*>(addr);
}

inline StoreBuffer*
Cell::storeBuffer() const
{
    return chunk()->info.trailer.storeBuffer;
}

inline JS::TraceKind
Cell::getTraceKind() const
{
    return isTenured() ? asTenured().getTraceKind() : JS::TraceKind::Object;
}

inline bool
InFreeList(ArenaHeader* aheader, void* thing)
{
    if (!aheader->hasFreeThings())
        return false;

    FreeSpan firstSpan(aheader->getFirstFreeSpan());
    uintptr_t addr = reinterpret_cast<uintptr_t>(thing);

    MOZ_ASSERT(Arena::isAligned(addr, aheader->getThingSize()));

    return firstSpan.inFreeList(addr);
}

/* static */ MOZ_ALWAYS_INLINE bool
Cell::needWriteBarrierPre(JS::Zone* zone) {
    return JS::shadow::Zone::asShadowZone(zone)->needsIncrementalBarrier();
}

/* static */ MOZ_ALWAYS_INLINE TenuredCell*
TenuredCell::fromPointer(void* ptr)
{
    MOZ_ASSERT(static_cast<TenuredCell*>(ptr)->isTenured());
    return static_cast<TenuredCell*>(ptr);
}

/* static */ MOZ_ALWAYS_INLINE const TenuredCell*
TenuredCell::fromPointer(const void* ptr)
{
    MOZ_ASSERT(static_cast<const TenuredCell*>(ptr)->isTenured());
    return static_cast<const TenuredCell*>(ptr);
}

bool
TenuredCell::isMarked(uint32_t color /* = BLACK */) const
{
    MOZ_ASSERT(arenaHeader()->allocated());
    AssertValidColor(this, color);
    return chunk()->bitmap.isMarked(this, color);
}

bool
TenuredCell::markIfUnmarked(uint32_t color /* = BLACK */) const
{
    AssertValidColor(this, color);
    return chunk()->bitmap.markIfUnmarked(this, color);
}

void
TenuredCell::unmark(uint32_t color) const
{
    MOZ_ASSERT(color != BLACK);
    AssertValidColor(this, color);
    chunk()->bitmap.unmark(this, color);
}

void
TenuredCell::copyMarkBitsFrom(const TenuredCell* src)
{
    ChunkBitmap& bitmap = chunk()->bitmap;
    bitmap.copyMarkBit(this, src, BLACK);
    bitmap.copyMarkBit(this, src, GRAY);
}

inline ArenaHeader*
TenuredCell::arenaHeader() const
{
    MOZ_ASSERT(isTenured());
    uintptr_t addr = address();
    addr &= ~ArenaMask;
    return reinterpret_cast<ArenaHeader*>(addr);
}

AllocKind
TenuredCell::getAllocKind() const
{
    return arenaHeader()->getAllocKind();
}

JS::TraceKind
TenuredCell::getTraceKind() const
{
    return MapAllocToTraceKind(getAllocKind());
}

JS::Zone*
TenuredCell::zone() const
{
    JS::Zone* zone = arenaHeader()->zone;
    MOZ_ASSERT(CurrentThreadCanAccessZone(zone));
    return zone;
}

JS::Zone*
TenuredCell::zoneFromAnyThread() const
{
    return arenaHeader()->zone;
}

bool
TenuredCell::isInsideZone(JS::Zone* zone) const
{
    return zone == arenaHeader()->zone;
}

/* static */ MOZ_ALWAYS_INLINE void
TenuredCell::readBarrier(TenuredCell* thing)
{
    MOZ_ASSERT(!CurrentThreadIsIonCompiling());
    MOZ_ASSERT(!isNullLike(thing));
    if (thing->shadowRuntimeFromAnyThread()->isHeapBusy())
        return;
    MOZ_ASSERT_IF(CurrentThreadCanAccessRuntime(thing->runtimeFromAnyThread()),
                  !thing->shadowRuntimeFromAnyThread()->isHeapCollecting());

    JS::shadow::Zone* shadowZone = thing->shadowZoneFromAnyThread();
    MOZ_ASSERT_IF(!CurrentThreadCanAccessRuntime(thing->runtimeFromAnyThread()),
                  !shadowZone->needsIncrementalBarrier());

    if (shadowZone->needsIncrementalBarrier()) {
        MOZ_ASSERT(!RuntimeFromMainThreadIsHeapMajorCollecting(shadowZone));
        Cell* tmp = thing;
        TraceManuallyBarrieredGenericPointerEdge(shadowZone->barrierTracer(), &tmp, "read barrier");
        MOZ_ASSERT(tmp == thing);
    }
    if (thing->isMarked(GRAY))
        UnmarkGrayCellRecursively(thing, thing->getTraceKind());
}

/* static */ MOZ_ALWAYS_INLINE void
TenuredCell::writeBarrierPre(TenuredCell* thing)
{
    MOZ_ASSERT(!CurrentThreadIsIonCompiling());
    MOZ_ASSERT_IF(thing, !isNullLike(thing));
    if (!thing || thing->shadowRuntimeFromAnyThread()->isHeapBusy())
        return;

    JS::shadow::Zone* shadowZone = thing->shadowZoneFromAnyThread();
    if (shadowZone->needsIncrementalBarrier()) {
        MOZ_ASSERT(!RuntimeFromMainThreadIsHeapMajorCollecting(shadowZone));
        Cell* tmp = thing;
        TraceManuallyBarrieredGenericPointerEdge(shadowZone->barrierTracer(), &tmp, "pre barrier");
        MOZ_ASSERT(tmp == thing);
    }
}

static MOZ_ALWAYS_INLINE void
AssertValidToSkipBarrier(TenuredCell* thing)
{
    MOZ_ASSERT(!IsInsideNursery(thing));
    MOZ_ASSERT_IF(thing, MapAllocToTraceKind(thing->getAllocKind()) != JS::TraceKind::Object);
}

/* static */ MOZ_ALWAYS_INLINE void
TenuredCell::writeBarrierPost(void* cellp, TenuredCell* prior, TenuredCell* next)
{
    AssertValidToSkipBarrier(next);
}

#ifdef DEBUG
bool
Cell::isAligned() const
{
    if (!isTenured())
        return true;
    return asTenured().isAligned();
}

bool
TenuredCell::isAligned() const
{
    return Arena::isAligned(address(), arenaHeader()->getThingSize());
}
#endif

} /* namespace gc */
} /* namespace js */

#endif /* gc_Heap_h */
