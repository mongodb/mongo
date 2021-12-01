/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal definitions of ArenaList and associated heap data structures.
 */

#ifndef gc_ArenaList_h
#define gc_ArenaList_h

#include "gc/AllocKind.h"
#include "js/SliceBudget.h"
#include "js/TypeDecls.h"
#include "threading/ProtectedData.h"

namespace js {

class FreeOp;
class Nursery;
class TenuringTracer;

namespace gcstats {
struct Statistics;
}

namespace gc {

class Arena;
struct FinalizePhase;
class FreeSpan;
class TenuredCell;

/*
 * A single segment of a SortedArenaList. Each segment has a head and a tail,
 * which track the start and end of a segment for O(1) append and concatenation.
 */
struct SortedArenaListSegment
{
    Arena* head;
    Arena** tailp;

    void clear() {
        head = nullptr;
        tailp = &head;
    }

    bool isEmpty() const {
        return tailp == &head;
    }

    // Appends |arena| to this segment.
    inline void append(Arena* arena);

    // Points the tail of this segment at |arena|, which may be null. Note
    // that this does not change the tail itself, but merely which arena
    // follows it. This essentially turns the tail into a cursor (see also the
    // description of ArenaList), but from the perspective of a SortedArenaList
    // this makes no difference.
    void linkTo(Arena* arena) {
        *tailp = arena;
    }
};

/*
 * Arena lists have a head and a cursor. The cursor conceptually lies on arena
 * boundaries, i.e. before the first arena, between two arenas, or after the
 * last arena.
 *
 * Arenas are usually sorted in order of increasing free space, with the cursor
 * following the Arena currently being allocated from. This ordering should not
 * be treated as an invariant, however, as the free lists may be cleared,
 * leaving arenas previously used for allocation partially full. Sorting order
 * is restored during sweeping.

 * Arenas following the cursor should not be full.
 */
class ArenaList {
    // The cursor is implemented via an indirect pointer, |cursorp_|, to allow
    // for efficient list insertion at the cursor point and other list
    // manipulations.
    //
    // - If the list is empty: |head| is null, |cursorp_| points to |head|, and
    //   therefore |*cursorp_| is null.
    //
    // - If the list is not empty: |head| is non-null, and...
    //
    //   - If the cursor is at the start of the list: |cursorp_| points to
    //     |head|, and therefore |*cursorp_| points to the first arena.
    //
    //   - If cursor is at the end of the list: |cursorp_| points to the |next|
    //     field of the last arena, and therefore |*cursorp_| is null.
    //
    //   - If the cursor is at neither the start nor the end of the list:
    //     |cursorp_| points to the |next| field of the arena preceding the
    //     cursor, and therefore |*cursorp_| points to the arena following the
    //     cursor.
    //
    // |cursorp_| is never null.
    //
    Arena* head_;
    Arena** cursorp_;

    inline void copy(const ArenaList& other);

  public:
    inline ArenaList();
    inline ArenaList(const ArenaList& other);

    inline ArenaList& operator=(const ArenaList& other);

    inline explicit ArenaList(const SortedArenaListSegment& segment);

    inline void check() const;

    inline void clear();
    inline ArenaList copyAndClear();
    inline bool isEmpty() const;

    // This returns nullptr if the list is empty.
    inline Arena* head() const;

    inline bool isCursorAtHead() const;
    inline bool isCursorAtEnd() const;

    inline void moveCursorToEnd();

    // This can return nullptr.
    inline Arena* arenaAfterCursor() const;

    // This returns the arena after the cursor and moves the cursor past it.
    inline Arena* takeNextArena();

    // This does two things.
    // - Inserts |a| at the cursor.
    // - Leaves the cursor sitting just before |a|, if |a| is not full, or just
    //   after |a|, if |a| is full.
    inline void insertAtCursor(Arena* a);

    // Inserts |a| at the cursor, then moves the cursor past it.
    inline void insertBeforeCursor(Arena* a);

    // This inserts |other|, which must be full, at the cursor of |this|.
    inline ArenaList& insertListWithCursorAtEnd(const ArenaList& other);

    Arena* removeRemainingArenas(Arena** arenap);
    Arena** pickArenasToRelocate(size_t& arenaTotalOut, size_t& relocTotalOut);
    Arena* relocateArenas(Arena* toRelocate, Arena* relocated,
                          js::SliceBudget& sliceBudget, gcstats::Statistics& stats);
};

/*
 * A class that holds arenas in sorted order by appending arenas to specific
 * segments. Each segment has a head and a tail, which can be linked up to
 * other segments to create a contiguous ArenaList.
 */
class SortedArenaList
{
  public:
    // The minimum size, in bytes, of a GC thing.
    static const size_t MinThingSize = 16;

    static_assert(ArenaSize <= 4096, "When increasing the Arena size, please consider how"\
                                     " this will affect the size of a SortedArenaList.");

    static_assert(MinThingSize >= 16, "When decreasing the minimum thing size, please consider"\
                                      " how this will affect the size of a SortedArenaList.");

  private:
    // The maximum number of GC things that an arena can hold.
    static const size_t MaxThingsPerArena = (ArenaSize - ArenaHeaderSize) / MinThingSize;

    size_t thingsPerArena_;
    SortedArenaListSegment segments[MaxThingsPerArena + 1];

    // Convenience functions to get the nth head and tail.
    Arena* headAt(size_t n) { return segments[n].head; }
    Arena** tailAt(size_t n) { return segments[n].tailp; }

  public:
    inline explicit SortedArenaList(size_t thingsPerArena = MaxThingsPerArena);

    inline void setThingsPerArena(size_t thingsPerArena);

    // Resets the first |thingsPerArena| segments of this list for further use.
    inline void reset(size_t thingsPerArena = MaxThingsPerArena);

    // Inserts an arena, which has room for |nfree| more things, in its segment.
    inline void insertAt(Arena* arena, size_t nfree);

    // Remove all empty arenas, inserting them as a linked list.
    inline void extractEmpty(Arena** empty);

    // Links up the tail of each non-empty segment to the head of the next
    // non-empty segment, creating a contiguous list that is returned as an
    // ArenaList. This is not a destructive operation: neither the head nor tail
    // of any segment is modified. However, note that the Arenas in the
    // resulting ArenaList should be treated as read-only unless the
    // SortedArenaList is no longer needed: inserting or removing arenas would
    // invalidate the SortedArenaList.
    inline ArenaList toArenaList();
};

enum class ShouldCheckThresholds
{
    DontCheckThresholds = 0,
    CheckThresholds = 1
};

class ArenaLists
{
    JSRuntime* const runtime_;

    /*
     * For each arena kind its free list is represented as the first span with
     * free things. Initially all the spans are initialized as empty. After we
     * find a new arena with available things we move its first free span into
     * the list and set the arena as fully allocated. way we do not need to
     * update the arena after the initial allocation. When starting the
     * GC we only move the head of the of the list of spans back to the arena
     * only for the arena that was not fully allocated.
     */
    ZoneGroupData<AllAllocKindArray<FreeSpan*>> freeLists_;
    AllAllocKindArray<FreeSpan*>& freeLists() { return freeLists_.ref(); }
    const AllAllocKindArray<FreeSpan*>& freeLists() const { return freeLists_.ref(); }

    FreeSpan* freeList(AllocKind i) const { return freeLists()[i]; }

    inline void setFreeList(AllocKind i, FreeSpan* span);
    inline void clearFreeList(AllocKind i);

    // Because the JITs can allocate from the free lists, they cannot be null.
    // We use a placeholder FreeSpan that is empty (and wihout an associated
    // Arena) so the JITs can fall back gracefully.
    static FreeSpan placeholder;

    ZoneGroupOrGCTaskData<AllAllocKindArray<ArenaList>> arenaLists_;
    ArenaList& arenaLists(AllocKind i) { return arenaLists_.ref()[i]; }
    const ArenaList& arenaLists(AllocKind i) const { return arenaLists_.ref()[i]; }

    enum BackgroundFinalizeStateEnum { BFS_DONE, BFS_RUN };

    typedef mozilla::Atomic<BackgroundFinalizeStateEnum, mozilla::SequentiallyConsistent>
        BackgroundFinalizeState;

    /* The current background finalization state, accessed atomically. */
    UnprotectedData<AllAllocKindArray<BackgroundFinalizeState>> backgroundFinalizeState_;
    BackgroundFinalizeState& backgroundFinalizeState(AllocKind i) { return backgroundFinalizeState_.ref()[i]; }
    const BackgroundFinalizeState& backgroundFinalizeState(AllocKind i) const { return backgroundFinalizeState_.ref()[i]; }

    /* For each arena kind, a list of arenas remaining to be swept. */
    ActiveThreadOrGCTaskData<AllAllocKindArray<Arena*>> arenaListsToSweep_;
    Arena*& arenaListsToSweep(AllocKind i) { return arenaListsToSweep_.ref()[i]; }
    Arena* arenaListsToSweep(AllocKind i) const { return arenaListsToSweep_.ref()[i]; }

    /* During incremental sweeping, a list of the arenas already swept. */
    ZoneGroupOrGCTaskData<AllocKind> incrementalSweptArenaKind;
    ZoneGroupOrGCTaskData<ArenaList> incrementalSweptArenas;

    // Arena lists which have yet to be swept, but need additional foreground
    // processing before they are swept.
    ZoneGroupData<Arena*> gcShapeArenasToUpdate;
    ZoneGroupData<Arena*> gcAccessorShapeArenasToUpdate;
    ZoneGroupData<Arena*> gcScriptArenasToUpdate;
    ZoneGroupData<Arena*> gcObjectGroupArenasToUpdate;

    // The list of empty arenas which are collected during sweep phase and released at the end of
    // sweeping every sweep group.
    ZoneGroupData<Arena*> savedEmptyArenas;

  public:
    explicit ArenaLists(JSRuntime* rt, ZoneGroup* group);
    ~ArenaLists();

    const void* addressOfFreeList(AllocKind thingKind) const {
        return reinterpret_cast<const void*>(&freeLists_.refNoCheck()[thingKind]);
    }

    inline Arena* getFirstArena(AllocKind thingKind) const;
    inline Arena* getFirstArenaToSweep(AllocKind thingKind) const;
    inline Arena* getFirstSweptArena(AllocKind thingKind) const;
    inline Arena* getArenaAfterCursor(AllocKind thingKind) const;

    inline bool arenaListsAreEmpty() const;

    inline void unmarkAll();

    inline bool doneBackgroundFinalize(AllocKind kind) const;
    inline bool needBackgroundFinalizeWait(AllocKind kind) const;

    /* Clear the free lists so we won't try to allocate from swept arenas. */
    inline void clearFreeLists();

    inline void unmarkPreMarkedFreeCells();

    /* Check if this arena is in use. */
    inline bool arenaIsInUse(Arena* arena, AllocKind kind) const;

    MOZ_ALWAYS_INLINE TenuredCell* allocateFromFreeList(AllocKind thingKind, size_t thingSize);

    /* Moves all arenas from |fromArenaLists| into |this|. */
    void adoptArenas(JSRuntime* runtime, ArenaLists* fromArenaLists, bool targetZoneIsCollecting);

    /* True if the Arena in question is found in this ArenaLists */
    bool containsArena(JSRuntime* runtime, Arena* arena);

    inline void checkEmptyFreeLists();
    inline bool checkEmptyArenaLists();
    inline void checkEmptyFreeList(AllocKind kind);

    bool checkEmptyArenaList(AllocKind kind);

    bool relocateArenas(JS::Zone* zone, Arena*& relocatedListOut, JS::gcreason::Reason reason,
                        js::SliceBudget& sliceBudget, gcstats::Statistics& stats);

    void queueForegroundObjectsForSweep(FreeOp* fop);
    void queueForegroundThingsForSweep();

    void releaseForegroundSweptEmptyArenas();

    bool foregroundFinalize(FreeOp* fop, AllocKind thingKind, js::SliceBudget& sliceBudget,
                            SortedArenaList& sweepList);
    static void backgroundFinalize(FreeOp* fop, Arena* listHead, Arena** empty);

    // When finalizing arenas, whether to keep empty arenas on the list or
    // release them immediately.
    enum KeepArenasEnum {
        RELEASE_ARENAS,
        KEEP_ARENAS
    };

  private:
    inline void queueForForegroundSweep(FreeOp* fop, const FinalizePhase& phase);
    inline void queueForBackgroundSweep(FreeOp* fop, const FinalizePhase& phase);
    inline void queueForForegroundSweep(AllocKind thingKind);
    inline void queueForBackgroundSweep(AllocKind thingKind);

    TenuredCell* allocateFromArena(JS::Zone* zone, AllocKind thingKind,
                                   ShouldCheckThresholds checkThresholds);
    inline TenuredCell* allocateFromArenaInner(JS::Zone* zone, Arena* arena, AllocKind kind);

    friend class GCRuntime;
    friend class js::Nursery;
    friend class js::TenuringTracer;
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_ArenaList_h */

