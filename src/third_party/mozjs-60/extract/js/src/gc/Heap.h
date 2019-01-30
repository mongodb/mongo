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
#include "mozilla/DebugOnly.h"
#include "mozilla/PodOperations.h"

#include <stddef.h>
#include <stdint.h>

#include "jsfriendapi.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "jsutil.h"

#include "ds/BitArray.h"
#include "gc/AllocKind.h"
#include "gc/GCEnum.h"
#include "gc/Memory.h"
#include "js/HeapAPI.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"

#include "vm/Printer.h"

namespace js {

class AutoLockGC;
class AutoLockGCBgAlloc;
class FreeOp;

namespace gc {

class Arena;
class ArenaCellSet;
class ArenaList;
class SortedArenaList;
class TenuredCell;
struct Chunk;

/*
 * This flag allows an allocation site to request a specific heap based upon the
 * estimated lifetime or lifetime requirements of objects allocated from that
 * site.
 */
enum InitialHeap : uint8_t {
    DefaultHeap,
    TenuredHeap
};

/* Cells are aligned to CellAlignShift, so the largest tagged null pointer is: */
const uintptr_t LargestTaggedNullCellPointer = (1 << CellAlignShift) - 1;

/*
 * The minimum cell size ends up as twice the cell alignment because the mark
 * bitmap contains one bit per CellBytesPerMarkBit bytes (which is equal to
 * CellAlignBytes) and we need two mark bits per cell.
 */
const size_t MarkBitsPerCell = 2;
const size_t MinCellSize = CellBytesPerMarkBit * MarkBitsPerCell;

constexpr size_t
DivideAndRoundUp(size_t numerator, size_t divisor) {
    return (numerator + divisor - 1) / divisor;
}

static_assert(ArenaSize % CellAlignBytes == 0,
              "Arena size must be a multiple of cell alignment");

/*
 * The mark bitmap has one bit per each possible cell start position. This
 * wastes some space for larger GC things but allows us to avoid division by the
 * cell's size when accessing the bitmap.
 */
const size_t ArenaBitmapBits = ArenaSize / CellBytesPerMarkBit;
const size_t ArenaBitmapBytes = DivideAndRoundUp(ArenaBitmapBits, 8);
const size_t ArenaBitmapWords = DivideAndRoundUp(ArenaBitmapBits, JS_BITS_PER_WORD);

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
    friend class Arena;
    friend class ArenaCellIterImpl;
    friend class ArenaFreeCellIter;

    uint16_t first;
    uint16_t last;

  public:
    // This inits just |first| and |last|; if the span is non-empty it doesn't
    // do anything with the next span stored at |last|.
    void initBounds(uintptr_t firstArg, uintptr_t lastArg, const Arena* arena) {
        checkRange(firstArg, lastArg, arena);
        first = firstArg;
        last = lastArg;
    }

    void initAsEmpty() {
        first = 0;
        last = 0;
    }

    // This sets |first| and |last|, and also sets the next span stored at
    // |last| as empty. (As a result, |firstArg| and |lastArg| cannot represent
    // an empty span.)
    void initFinal(uintptr_t firstArg, uintptr_t lastArg, const Arena* arena) {
        initBounds(firstArg, lastArg, arena);
        FreeSpan* last = nextSpanUnchecked(arena);
        last->initAsEmpty();
        checkSpan(arena);
    }

    bool isEmpty() const {
        return !first;
    }

    Arena* getArenaUnchecked() { return reinterpret_cast<Arena*>(this); }
    inline Arena* getArena();

    static size_t offsetOfFirst() {
        return offsetof(FreeSpan, first);
    }

    static size_t offsetOfLast() {
        return offsetof(FreeSpan, last);
    }

    // Like nextSpan(), but no checking of the following span is done.
    FreeSpan* nextSpanUnchecked(const Arena* arena) const {
        MOZ_ASSERT(arena && !isEmpty());
        return reinterpret_cast<FreeSpan*>(uintptr_t(arena) + last);
    }

    const FreeSpan* nextSpan(const Arena* arena) const {
        checkSpan(arena);
        return nextSpanUnchecked(arena);
    }

    MOZ_ALWAYS_INLINE TenuredCell* allocate(size_t thingSize) {
        // Eschew the usual checks, because this might be the placeholder span.
        // If this is somehow an invalid, non-empty span, checkSpan() will catch it.
        Arena* arena = getArenaUnchecked();
        checkSpan(arena);
        uintptr_t thing = uintptr_t(arena) + first;
        if (first < last) {
            // We have space for at least two more things, so do a simple bump-allocate.
            first += thingSize;
        } else if (MOZ_LIKELY(first)) {
            // The last space points to the next free span (which may be empty).
            const FreeSpan* next = nextSpan(arena);
            first = next->first;
            last = next->last;
        } else {
            return nullptr; // The span is empty.
        }
        checkSpan(arena);
        JS_EXTRA_POISON(reinterpret_cast<void*>(thing), JS_ALLOCATED_TENURED_PATTERN, thingSize);
        return reinterpret_cast<TenuredCell*>(thing);
    }

    inline void checkSpan(const Arena* arena) const;
    inline void checkRange(uintptr_t first, uintptr_t last, const Arena* arena) const;
};

/*
 * Arenas are the allocation units of the tenured heap in the GC. An arena
 * is 4kiB in size and 4kiB-aligned. It starts with several header fields
 * followed by some bytes of padding. The remainder of the arena is filled
 * with GC things of a particular AllocKind. The padding ensures that the
 * GC thing array ends exactly at the end of the arena:
 *
 * <----------------------------------------------> = ArenaSize bytes
 * +---------------+---------+----+----+-----+----+
 * | header fields | padding | T0 | T1 | ... | Tn |
 * +---------------+---------+----+----+-----+----+
 * <-------------------------> = first thing offset
 */
class Arena
{
    static JS_FRIEND_DATA(const uint32_t) ThingSizes[];
    static JS_FRIEND_DATA(const uint32_t) FirstThingOffsets[];
    static JS_FRIEND_DATA(const uint32_t) ThingsPerArena[];

    /*
     * The first span of free things in the arena. Most of these spans are
     * stored as offsets in free regions of the data array, and most operations
     * on FreeSpans take an Arena pointer for safety. However, the FreeSpans
     * used for allocation are stored here, at the start of an Arena, and use
     * their own address to grab the next span within the same Arena.
     */
    FreeSpan firstFreeSpan;

  public:
    /*
     * The zone that this Arena is contained within, when allocated. The offset
     * of this field must match the ArenaZoneOffset stored in js/HeapAPI.h,
     * as is statically asserted below.
     */
    JS::Zone* zone;

    /*
     * Arena::next has two purposes: when unallocated, it points to the next
     * available Arena. When allocated, it points to the next Arena in the same
     * zone and with the same alloc kind.
     */
    Arena* next;

  private:
    /*
     * One of the AllocKind constants or AllocKind::LIMIT when the arena does
     * not contain any GC things and is on the list of empty arenas in the GC
     * chunk.
     *
     * We use 8 bits for the alloc kind so the compiler can use byte-level
     * memory instructions to access it.
     */
    size_t allocKind : 8;

  public:
    /*
     * When collecting we sometimes need to keep an auxillary list of arenas,
     * for which we use the following fields. This happens for several reasons:
     *
     * When recursive marking uses too much stack, the marking is delayed and
     * the corresponding arenas are put into a stack. To distinguish the bottom
     * of the stack from the arenas not present in the stack we use the
     * markOverflow flag to tag arenas on the stack.
     *
     * To minimize the size of the header fields we record the next linkage as
     * address() >> ArenaShift and pack it with the allocKind and the flags.
     */
    size_t hasDelayedMarking : 1;
    size_t markOverflow : 1;
    size_t auxNextLink : JS_BITS_PER_WORD - 8 - 1 - 1;
    static_assert(ArenaShift >= 8 + 1 + 1,
                  "Arena::auxNextLink packing assumes that ArenaShift has "
                  "enough bits to cover allocKind and hasDelayedMarking.");

  private:
    union {
        /*
         * For arenas in zones other than the atoms zone, if non-null, points
         * to an ArenaCellSet that represents the set of cells in this arena
         * that are in the nursery's store buffer.
         */
        ArenaCellSet* bufferedCells_;

        /*
         * For arenas in the atoms zone, the starting index into zone atom
         * marking bitmaps (see AtomMarking.h) of the things in this zone.
         * Atoms never refer to nursery things, so no store buffer index is
         * needed.
         */
        size_t atomBitmapStart_;
    };
  public:

    /*
     * The size of data should be |ArenaSize - offsetof(data)|, but the offset
     * is not yet known to the compiler, so we do it by hand. |firstFreeSpan|
     * takes up 8 bytes on 64-bit due to alignment requirements; the rest are
     * obvious. This constant is stored in js/HeapAPI.h.
     */
    uint8_t data[ArenaSize - ArenaHeaderSize];

    void init(JS::Zone* zoneArg, AllocKind kind);

    // Sets |firstFreeSpan| to the Arena's entire valid range, and
    // also sets the next span stored at |firstFreeSpan.last| as empty.
    void setAsFullyUnused() {
        AllocKind kind = getAllocKind();
        firstFreeSpan.first = firstThingOffset(kind);
        firstFreeSpan.last = lastThingOffset(kind);
        FreeSpan* last = firstFreeSpan.nextSpanUnchecked(this);
        last->initAsEmpty();
    }

    // Initialize an arena to its unallocated state. For arenas that were
    // previously allocated for some zone, use release() instead.
    void setAsNotAllocated() {
        firstFreeSpan.initAsEmpty();
        zone = nullptr;
        allocKind = size_t(AllocKind::LIMIT);
        hasDelayedMarking = 0;
        markOverflow = 0;
        auxNextLink = 0;
        bufferedCells_ = nullptr;
    }

    // Return an allocated arena to its unallocated state.
    inline void release();

    uintptr_t address() const {
        checkAddress();
        return uintptr_t(this);
    }

    inline void checkAddress() const;

    inline Chunk* chunk() const;

    bool allocated() const {
        MOZ_ASSERT(IsAllocKind(AllocKind(allocKind)));
        return IsValidAllocKind(AllocKind(allocKind));
    }

    AllocKind getAllocKind() const {
        MOZ_ASSERT(allocated());
        return AllocKind(allocKind);
    }

    FreeSpan* getFirstFreeSpan() { return &firstFreeSpan; }

    static size_t thingSize(AllocKind kind) { return ThingSizes[size_t(kind)]; }
    static size_t thingsPerArena(AllocKind kind) { return ThingsPerArena[size_t(kind)]; }
    static size_t thingsSpan(AllocKind kind) { return thingsPerArena(kind) * thingSize(kind); }

    static size_t firstThingOffset(AllocKind kind) { return FirstThingOffsets[size_t(kind)]; }
    static size_t lastThingOffset(AllocKind kind) { return ArenaSize - thingSize(kind); }

    size_t getThingSize() const { return thingSize(getAllocKind()); }
    size_t getThingsPerArena() const { return thingsPerArena(getAllocKind()); }
    size_t getThingsSpan() const { return getThingsPerArena() * getThingSize(); }
    size_t getFirstThingOffset() const { return firstThingOffset(getAllocKind()); }

    uintptr_t thingsStart() const { return address() + getFirstThingOffset(); }
    uintptr_t thingsEnd() const { return address() + ArenaSize; }

    bool isEmpty() const {
        // Arena is empty if its first span covers the whole arena.
        firstFreeSpan.checkSpan(this);
        AllocKind kind = getAllocKind();
        return firstFreeSpan.first == firstThingOffset(kind) &&
               firstFreeSpan.last == lastThingOffset(kind);
    }

    bool hasFreeThings() const { return !firstFreeSpan.isEmpty(); }

    size_t numFreeThings(size_t thingSize) const {
        firstFreeSpan.checkSpan(this);
        size_t numFree = 0;
        const FreeSpan* span = &firstFreeSpan;
        for (; !span->isEmpty(); span = span->nextSpan(this))
            numFree += (span->last - span->first) / thingSize + 1;
        return numFree;
    }

    size_t countFreeCells() { return numFreeThings(getThingSize()); }
    size_t countUsedCells() { return getThingsPerArena() - countFreeCells(); }

    bool inFreeList(uintptr_t thing) {
        uintptr_t base = address();
        const FreeSpan* span = &firstFreeSpan;
        for (; !span->isEmpty(); span = span->nextSpan(this)) {
            /* If the thing comes before the current span, it's not free. */
            if (thing < base + span->first)
                return false;

            /* If we find it before the end of the span, it's free. */
            if (thing <= base + span->last)
                return true;
        }
        return false;
    }

    static bool isAligned(uintptr_t thing, size_t thingSize) {
        /* Things ends at the arena end. */
        uintptr_t tailOffset = ArenaSize - (thing & ArenaMask);
        return tailOffset % thingSize == 0;
    }

    Arena* getNextDelayedMarking() const {
        MOZ_ASSERT(hasDelayedMarking);
        return reinterpret_cast<Arena*>(auxNextLink << ArenaShift);
    }

    void setNextDelayedMarking(Arena* arena) {
        MOZ_ASSERT(!(uintptr_t(arena) & ArenaMask));
        MOZ_ASSERT(!auxNextLink && !hasDelayedMarking);
        hasDelayedMarking = 1;
        if (arena)
            auxNextLink = arena->address() >> ArenaShift;
    }

    void unsetDelayedMarking() {
        MOZ_ASSERT(hasDelayedMarking);
        hasDelayedMarking = 0;
        auxNextLink = 0;
    }

    inline ArenaCellSet*& bufferedCells();
    inline size_t& atomBitmapStart();

    template <typename T>
    size_t finalize(FreeOp* fop, AllocKind thingKind, size_t thingSize);

    static void staticAsserts();

    void unmarkAll();
    void unmarkPreMarkedFreeCells();

#ifdef DEBUG
    void checkNoMarkedFreeCells();
#endif
};

static_assert(ArenaZoneOffset == offsetof(Arena, zone),
              "The hardcoded API zone offset must match the actual offset.");

static_assert(sizeof(Arena) == ArenaSize,
              "ArenaSize must match the actual size of the Arena structure.");

static_assert(offsetof(Arena, data) == ArenaHeaderSize,
              "ArenaHeaderSize must match the actual size of the header fields.");

inline Arena*
FreeSpan::getArena()
{
    Arena* arena = getArenaUnchecked();
    arena->checkAddress();
    return arena;
}

inline void
FreeSpan::checkSpan(const Arena* arena) const
{
#ifdef DEBUG
    if (!first) {
        MOZ_ASSERT(!first && !last);
        return;
    }

    arena->checkAddress();
    checkRange(first, last, arena);

    // If there's a following span, it must have a higher address,
    // and the gap must be at least 2 * thingSize.
    const FreeSpan* next = nextSpanUnchecked(arena);
    if (next->first) {
        checkRange(next->first, next->last, arena);
        size_t thingSize = arena->getThingSize();
        MOZ_ASSERT(last + 2 * thingSize <= next->first);
    }
#endif
}

inline void
FreeSpan::checkRange(uintptr_t first, uintptr_t last, const Arena* arena) const
{
#ifdef DEBUG
    MOZ_ASSERT(arena);
    MOZ_ASSERT(first <= last);
    AllocKind thingKind = arena->getAllocKind();
    MOZ_ASSERT(first >= Arena::firstThingOffset(thingKind));
    MOZ_ASSERT(last <= Arena::lastThingOffset(thingKind));
    MOZ_ASSERT((last - first) % Arena::thingSize(thingKind) == 0);
#endif
}

/*
 * The tail of the chunk info is shared between all chunks in the system, both
 * nursery and tenured. This structure is locatable from any GC pointer by
 * aligning to 1MiB.
 */
struct ChunkTrailer
{
    // Construct a Nursery ChunkTrailer.
    ChunkTrailer(JSRuntime* rt, StoreBuffer* sb)
      : location(ChunkLocation::Nursery), storeBuffer(sb), runtime(rt)
    {}

    // Construct a Tenured heap ChunkTrailer.
    explicit ChunkTrailer(JSRuntime* rt)
      : location(ChunkLocation::TenuredHeap), storeBuffer(nullptr), runtime(rt)
    {}

  public:
    // The index of the chunk in the nursery, or LocationTenuredHeap.
    ChunkLocation   location;
    uint32_t        padding;

    // The store buffer for pointers from tenured things to things in this
    // chunk. Will be non-null only for nursery chunks.
    StoreBuffer*    storeBuffer;

    // Provide quick access to the runtime from absolutely anywhere.
    JSRuntime*      runtime;
};

static_assert(sizeof(ChunkTrailer) == ChunkTrailerSize,
              "ChunkTrailer size must match the API defined size.");

/* The chunk header (located at the end of the chunk to preserve arena alignment). */
struct ChunkInfo
{
    void init() {
        next = prev = nullptr;
    }

  private:
    friend class ChunkPool;
    Chunk*          next;
    Chunk*          prev;

  public:
    /* Free arenas are linked together with arena.next. */
    Arena*          freeArenasHead;

#if JS_BITS_PER_WORD == 32
    /*
     * Calculating sizes and offsets is simpler if sizeof(ChunkInfo) is
     * architecture-independent.
     */
    char            padding[24];
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
const size_t ChunkBytesAvailable = ChunkSize - sizeof(ChunkTrailer) - sizeof(ChunkInfo) - ChunkDecommitBitmapBytes;
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

    MOZ_ALWAYS_INLINE void getMarkWordAndMask(const TenuredCell* cell, ColorBit colorBit,
                                              uintptr_t** wordp, uintptr_t* maskp)
    {
        MOZ_ASSERT(size_t(colorBit) < MarkBitsPerCell);
        detail::GetGCThingMarkWordAndMask(uintptr_t(cell), colorBit, wordp, maskp);
    }

    MOZ_ALWAYS_INLINE MOZ_TSAN_BLACKLIST bool markBit(const TenuredCell* cell, ColorBit colorBit) {
        uintptr_t* word, mask;
        getMarkWordAndMask(cell, colorBit, &word, &mask);
        return *word & mask;
    }

    MOZ_ALWAYS_INLINE MOZ_TSAN_BLACKLIST bool isMarkedAny(const TenuredCell* cell) {
        return markBit(cell, ColorBit::BlackBit) || markBit(cell, ColorBit::GrayOrBlackBit);
    }

    MOZ_ALWAYS_INLINE MOZ_TSAN_BLACKLIST bool isMarkedBlack(const TenuredCell* cell) {
        return markBit(cell, ColorBit::BlackBit);
    }

    MOZ_ALWAYS_INLINE MOZ_TSAN_BLACKLIST bool isMarkedGray(const TenuredCell* cell) {
        return !markBit(cell, ColorBit::BlackBit) && markBit(cell, ColorBit::GrayOrBlackBit);
    }

    // The return value indicates if the cell went from unmarked to marked.
    MOZ_ALWAYS_INLINE bool markIfUnmarked(const TenuredCell* cell, MarkColor color) {
        uintptr_t* word, mask;
        getMarkWordAndMask(cell, ColorBit::BlackBit, &word, &mask);
        if (*word & mask)
            return false;
        if (color == MarkColor::Black) {
            *word |= mask;
        } else {
            /*
             * We use getMarkWordAndMask to recalculate both mask and word as
             * doing just mask << color may overflow the mask.
             */
            getMarkWordAndMask(cell, ColorBit::GrayOrBlackBit, &word, &mask);
            if (*word & mask)
                return false;
            *word |= mask;
        }
        return true;
    }

    MOZ_ALWAYS_INLINE void markBlack(const TenuredCell* cell) {
        uintptr_t* word, mask;
        getMarkWordAndMask(cell, ColorBit::BlackBit, &word, &mask);
        *word |= mask;
    }

    MOZ_ALWAYS_INLINE void copyMarkBit(TenuredCell* dst, const TenuredCell* src,
                                       ColorBit colorBit) {
        uintptr_t* srcWord, srcMask;
        getMarkWordAndMask(src, colorBit, &srcWord, &srcMask);

        uintptr_t* dstWord, dstMask;
        getMarkWordAndMask(dst, colorBit, &dstWord, &dstMask);

        *dstWord &= ~dstMask;
        if (*srcWord & srcMask)
            *dstWord |= dstMask;
    }

    MOZ_ALWAYS_INLINE void unmark(const TenuredCell* cell) {
        uintptr_t* word, mask;
        getMarkWordAndMask(cell, ColorBit::BlackBit, &word, &mask);
        *word &= ~mask;
        getMarkWordAndMask(cell, ColorBit::GrayOrBlackBit, &word, &mask);
        *word &= ~mask;
    }

    void clear() {
        memset((void*)bitmap, 0, sizeof(bitmap));
    }

    uintptr_t* arenaBits(Arena* arena) {
        static_assert(ArenaBitmapBits == ArenaBitmapWords * JS_BITS_PER_WORD,
                      "We assume that the part of the bitmap corresponding to the arena "
                      "has the exact number of words so we do not need to deal with a word "
                      "that covers bits from two arenas.");

        uintptr_t* word, unused;
        getMarkWordAndMask(reinterpret_cast<TenuredCell*>(arena->address()),
                           ColorBit::BlackBit, &word, &unused);
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
                            - sizeof(ChunkInfo)
                            - sizeof(ChunkTrailer);
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
    ChunkTrailer    trailer;

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
        return trailer.storeBuffer;
    }

    Arena* allocateArena(JSRuntime* rt, JS::Zone* zone, AllocKind kind, const AutoLockGC& lock);

    void releaseArena(JSRuntime* rt, Arena* arena, const AutoLockGC& lock);
    void recycleArena(Arena* arena, SortedArenaList& dest, size_t thingsPerArena);

    MOZ_MUST_USE bool decommitOneFreeArena(JSRuntime* rt, AutoLockGC& lock);
    void decommitAllArenasWithoutUnlocking(const AutoLockGC& lock);

    static Chunk* allocate(JSRuntime* rt);
    void init(JSRuntime* rt);

  private:
    void decommitAllArenas();

    /* Search for a decommitted arena to allocate. */
    unsigned findDecommittedArenaOffset();
    Arena* fetchNextDecommittedArena();

    void addArenaToFreeList(JSRuntime* rt, Arena* arena);
    void addArenaToDecommittedList(const Arena* arena);

    void updateChunkListAfterAlloc(JSRuntime* rt, const AutoLockGC& lock);
    void updateChunkListAfterFree(JSRuntime* rt, const AutoLockGC& lock);

  public:
    /* Unlink and return the freeArenasHead. */
    Arena* fetchNextFreeArena(JSRuntime* rt);
};

static_assert(sizeof(Chunk) == ChunkSize,
              "Ensure the hardcoded chunk size definition actually matches the struct.");
static_assert(js::gc::ChunkMarkBitmapOffset == offsetof(Chunk, bitmap),
              "The hardcoded API bitmap offset must match the actual offset.");
static_assert(js::gc::ChunkRuntimeOffset == offsetof(Chunk, trailer) +
                                            offsetof(ChunkTrailer, runtime),
              "The hardcoded API runtime offset must match the actual offset.");
static_assert(js::gc::ChunkLocationOffset == offsetof(Chunk, trailer) +
                                             offsetof(ChunkTrailer, location),
              "The hardcoded API location offset must match the actual offset.");
static_assert(js::gc::ChunkStoreBufferOffset == offsetof(Chunk, trailer) +
                                                offsetof(ChunkTrailer, storeBuffer),
              "The hardcoded API storeBuffer offset must match the actual offset.");

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
    HeapUsage* const parent_;

    /*
     * The approximate number of bytes in use on the GC heap, to the nearest
     * ArenaSize. This does not include any malloc data. It also does not
     * include not-actively-used addresses that are still reserved at the OS
     * level for GC usage. It is atomic because it is updated by both the active
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

inline void
Arena::checkAddress() const
{
    mozilla::DebugOnly<uintptr_t> addr = uintptr_t(this);
    MOZ_ASSERT(addr);
    MOZ_ASSERT(!(addr & ArenaMask));
    MOZ_ASSERT(Chunk::withinValidRange(addr));
}

inline Chunk*
Arena::chunk() const
{
    return Chunk::fromAddress(address());
}

inline bool
InFreeList(Arena* arena, void* thing)
{
    uintptr_t addr = reinterpret_cast<uintptr_t>(thing);
    MOZ_ASSERT(Arena::isAligned(addr, arena->getThingSize()));
    return arena->inFreeList(addr);
}

static const int32_t ChunkLocationOffsetFromLastByte =
    int32_t(gc::ChunkLocationOffset) - int32_t(gc::ChunkMask);
static const int32_t ChunkStoreBufferOffsetFromLastByte =
    int32_t(gc::ChunkStoreBufferOffset) - int32_t(gc::ChunkMask);

} /* namespace gc */

namespace debug {

// Utility functions meant to be called from an interactive debugger.
enum class MarkInfo : int {
    BLACK = 0,
    GRAY = 1,
    UNMARKED = -1,
    NURSERY = -2,
};

// Get the mark color for a cell, in a way easily usable from a debugger.
MOZ_NEVER_INLINE MarkInfo
GetMarkInfo(js::gc::Cell* cell);

// Sample usage from gdb:
//
//   (gdb) p $word = js::debug::GetMarkWordAddress(obj)
//   $1 = (uintptr_t *) 0x7fa56d5fe360
//   (gdb) p/x $mask = js::debug::GetMarkMask(obj, js::gc::GRAY)
//   $2 = 0x200000000
//   (gdb) watch *$word
//   Hardware watchpoint 7: *$word
//   (gdb) cond 7 *$word & $mask
//   (gdb) cont
//
// Note that this is *not* a watchpoint on a single bit. It is a watchpoint on
// the whole word, which will trigger whenever the word changes and the
// selected bit is set after the change.
//
// So if the bit changing is the desired one, this is exactly what you want.
// But if a different bit changes (either set or cleared), you may still stop
// execution if the $mask bit happened to already be set. gdb does not expose
// enough information to restrict the watchpoint to just a single bit.

// Return the address of the word containing the mark bits for the given cell,
// or nullptr if the cell is in the nursery.
MOZ_NEVER_INLINE uintptr_t*
GetMarkWordAddress(js::gc::Cell* cell);

// Return the mask for the given cell and color bit, or 0 if the cell is in the
// nursery.
MOZ_NEVER_INLINE uintptr_t
GetMarkMask(js::gc::Cell* cell, uint32_t colorBit);

} /* namespace debug */
} /* namespace js */

#endif /* gc_Heap_h */
