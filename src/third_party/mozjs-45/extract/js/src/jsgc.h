/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS Garbage Collector. */

#ifndef jsgc_h
#define jsgc_h

#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TypeTraits.h"

#include "jslock.h"

#include "js/GCAPI.h"
#include "js/SliceBudget.h"
#include "js/Vector.h"

#include "vm/NativeObject.h"

namespace js {

unsigned GetCPUCount();

enum ThreadType
{
    MainThread,
    BackgroundThread
};

namespace gcstats {
struct Statistics;
} // namespace gcstats

class Nursery;

namespace gc {

struct FinalizePhase;

enum State {
    NO_INCREMENTAL,
    MARK_ROOTS,
    MARK,
    SWEEP,
    COMPACT
};

// Expand the given macro D for each valid GC reference type.
#define FOR_EACH_GC_POINTER_TYPE(D) \
    D(AccessorShape*) \
    D(BaseShape*) \
    D(UnownedBaseShape*) \
    D(jit::JitCode*) \
    D(NativeObject*) \
    D(ArrayObject*) \
    D(ArgumentsObject*) \
    D(ArrayBufferObject*) \
    D(ArrayBufferObjectMaybeShared*) \
    D(ArrayBufferViewObject*) \
    D(DebugScopeObject*) \
    D(GlobalObject*) \
    D(JSObject*) \
    D(JSFunction*) \
    D(ModuleObject*) \
    D(ModuleEnvironmentObject*) \
    D(ModuleNamespaceObject*) \
    D(NestedScopeObject*) \
    D(PlainObject*) \
    D(SavedFrame*) \
    D(ScopeObject*) \
    D(ScriptSourceObject*) \
    D(SharedArrayBufferObject*) \
    D(ImportEntryObject*) \
    D(ExportEntryObject*) \
    D(JSScript*) \
    D(LazyScript*) \
    D(Shape*) \
    D(JSAtom*) \
    D(JSString*) \
    D(JSFlatString*) \
    D(JSLinearString*) \
    D(PropertyName*) \
    D(JS::Symbol*) \
    D(js::ObjectGroup*) \
    D(Value) \
    D(jsid) \
    D(TaggedProto)

/* Map from C++ type to alloc kind. JSObject does not have a 1:1 mapping, so must use Arena::thingSize. */
template <typename T> struct MapTypeToFinalizeKind {};
template <> struct MapTypeToFinalizeKind<JSScript>          { static const AllocKind kind = AllocKind::SCRIPT; };
template <> struct MapTypeToFinalizeKind<LazyScript>        { static const AllocKind kind = AllocKind::LAZY_SCRIPT; };
template <> struct MapTypeToFinalizeKind<Shape>             { static const AllocKind kind = AllocKind::SHAPE; };
template <> struct MapTypeToFinalizeKind<AccessorShape>     { static const AllocKind kind = AllocKind::ACCESSOR_SHAPE; };
template <> struct MapTypeToFinalizeKind<BaseShape>         { static const AllocKind kind = AllocKind::BASE_SHAPE; };
template <> struct MapTypeToFinalizeKind<ObjectGroup>       { static const AllocKind kind = AllocKind::OBJECT_GROUP; };
template <> struct MapTypeToFinalizeKind<JSFatInlineString> { static const AllocKind kind = AllocKind::FAT_INLINE_STRING; };
template <> struct MapTypeToFinalizeKind<JSString>          { static const AllocKind kind = AllocKind::STRING; };
template <> struct MapTypeToFinalizeKind<JSExternalString>  { static const AllocKind kind = AllocKind::EXTERNAL_STRING; };
template <> struct MapTypeToFinalizeKind<JS::Symbol>        { static const AllocKind kind = AllocKind::SYMBOL; };
template <> struct MapTypeToFinalizeKind<jit::JitCode>      { static const AllocKind kind = AllocKind::JITCODE; };

template <typename T> struct ParticipatesInCC {};
#define EXPAND_PARTICIPATES_IN_CC(_, type, addToCCKind) \
    template <> struct ParticipatesInCC<type> { static const bool value = addToCCKind; };
JS_FOR_EACH_TRACEKIND(EXPAND_PARTICIPATES_IN_CC)
#undef EXPAND_PARTICIPATES_IN_CC

static inline bool
IsNurseryAllocable(AllocKind kind)
{
    MOZ_ASSERT(IsValidAllocKind(kind));
    static const bool map[] = {
        true,      /* AllocKind::FUNCTION */
        true,      /* AllocKind::FUNCTION_EXTENDED */
        false,     /* AllocKind::OBJECT0 */
        true,      /* AllocKind::OBJECT0_BACKGROUND */
        false,     /* AllocKind::OBJECT2 */
        true,      /* AllocKind::OBJECT2_BACKGROUND */
        false,     /* AllocKind::OBJECT4 */
        true,      /* AllocKind::OBJECT4_BACKGROUND */
        false,     /* AllocKind::OBJECT8 */
        true,      /* AllocKind::OBJECT8_BACKGROUND */
        false,     /* AllocKind::OBJECT12 */
        true,      /* AllocKind::OBJECT12_BACKGROUND */
        false,     /* AllocKind::OBJECT16 */
        true,      /* AllocKind::OBJECT16_BACKGROUND */
        false,     /* AllocKind::SCRIPT */
        false,     /* AllocKind::LAZY_SCRIPT */
        false,     /* AllocKind::SHAPE */
        false,     /* AllocKind::ACCESSOR_SHAPE */
        false,     /* AllocKind::BASE_SHAPE */
        false,     /* AllocKind::OBJECT_GROUP */
        false,     /* AllocKind::FAT_INLINE_STRING */
        false,     /* AllocKind::STRING */
        false,     /* AllocKind::EXTERNAL_STRING */
        false,     /* AllocKind::SYMBOL */
        false,     /* AllocKind::JITCODE */
    };
    JS_STATIC_ASSERT(JS_ARRAY_LENGTH(map) == size_t(AllocKind::LIMIT));
    return map[size_t(kind)];
}

static inline bool
IsBackgroundFinalized(AllocKind kind)
{
    MOZ_ASSERT(IsValidAllocKind(kind));
    static const bool map[] = {
        true,      /* AllocKind::FUNCTION */
        true,      /* AllocKind::FUNCTION_EXTENDED */
        false,     /* AllocKind::OBJECT0 */
        true,      /* AllocKind::OBJECT0_BACKGROUND */
        false,     /* AllocKind::OBJECT2 */
        true,      /* AllocKind::OBJECT2_BACKGROUND */
        false,     /* AllocKind::OBJECT4 */
        true,      /* AllocKind::OBJECT4_BACKGROUND */
        false,     /* AllocKind::OBJECT8 */
        true,      /* AllocKind::OBJECT8_BACKGROUND */
        false,     /* AllocKind::OBJECT12 */
        true,      /* AllocKind::OBJECT12_BACKGROUND */
        false,     /* AllocKind::OBJECT16 */
        true,      /* AllocKind::OBJECT16_BACKGROUND */
        false,     /* AllocKind::SCRIPT */
        false,     /* AllocKind::LAZY_SCRIPT */
        true,      /* AllocKind::SHAPE */
        true,      /* AllocKind::ACCESSOR_SHAPE */
        true,      /* AllocKind::BASE_SHAPE */
        true,      /* AllocKind::OBJECT_GROUP */
        true,      /* AllocKind::FAT_INLINE_STRING */
        true,      /* AllocKind::STRING */
        false,     /* AllocKind::EXTERNAL_STRING */
        true,      /* AllocKind::SYMBOL */
        false,     /* AllocKind::JITCODE */
    };
    JS_STATIC_ASSERT(JS_ARRAY_LENGTH(map) == size_t(AllocKind::LIMIT));
    return map[size_t(kind)];
}

static inline bool
CanBeFinalizedInBackground(AllocKind kind, const Class* clasp)
{
    MOZ_ASSERT(IsObjectAllocKind(kind));
    /* If the class has no finalizer or a finalizer that is safe to call on
     * a different thread, we change the alloc kind. For example,
     * AllocKind::OBJECT0 calls the finalizer on the main thread,
     * AllocKind::OBJECT0_BACKGROUND calls the finalizer on the gcHelperThread.
     * IsBackgroundFinalized is called to prevent recursively incrementing
     * the alloc kind; kind may already be a background finalize kind.
     */
    return (!IsBackgroundFinalized(kind) &&
            (!clasp->finalize || (clasp->flags & JSCLASS_BACKGROUND_FINALIZE)));
}

/* Capacity for slotsToThingKind */
const size_t SLOTS_TO_THING_KIND_LIMIT = 17;

extern const AllocKind slotsToThingKind[];

/* Get the best kind to use when making an object with the given slot count. */
static inline AllocKind
GetGCObjectKind(size_t numSlots)
{
    if (numSlots >= SLOTS_TO_THING_KIND_LIMIT)
        return AllocKind::OBJECT16;
    return slotsToThingKind[numSlots];
}

/* As for GetGCObjectKind, but for dense array allocation. */
static inline AllocKind
GetGCArrayKind(size_t numElements)
{
    /*
     * Dense arrays can use their fixed slots to hold their elements array
     * (less two Values worth of ObjectElements header), but if more than the
     * maximum number of fixed slots is needed then the fixed slots will be
     * unused.
     */
    JS_STATIC_ASSERT(ObjectElements::VALUES_PER_HEADER == 2);
    if (numElements > NativeObject::MAX_DENSE_ELEMENTS_COUNT ||
        numElements + ObjectElements::VALUES_PER_HEADER >= SLOTS_TO_THING_KIND_LIMIT)
    {
        return AllocKind::OBJECT2;
    }
    return slotsToThingKind[numElements + ObjectElements::VALUES_PER_HEADER];
}

static inline AllocKind
GetGCObjectFixedSlotsKind(size_t numFixedSlots)
{
    MOZ_ASSERT(numFixedSlots < SLOTS_TO_THING_KIND_LIMIT);
    return slotsToThingKind[numFixedSlots];
}

// Get the best kind to use when allocating an object that needs a specific
// number of bytes.
static inline AllocKind
GetGCObjectKindForBytes(size_t nbytes)
{
    MOZ_ASSERT(nbytes <= JSObject::MAX_BYTE_SIZE);

    if (nbytes <= sizeof(NativeObject))
        return AllocKind::OBJECT0;
    nbytes -= sizeof(NativeObject);

    size_t dataSlots = AlignBytes(nbytes, sizeof(Value)) / sizeof(Value);
    MOZ_ASSERT(nbytes <= dataSlots * sizeof(Value));
    return GetGCObjectKind(dataSlots);
}

static inline AllocKind
GetBackgroundAllocKind(AllocKind kind)
{
    MOZ_ASSERT(!IsBackgroundFinalized(kind));
    MOZ_ASSERT(IsObjectAllocKind(kind));
    return AllocKind(size_t(kind) + 1);
}

/* Get the number of fixed slots and initial capacity associated with a kind. */
static inline size_t
GetGCKindSlots(AllocKind thingKind)
{
    /* Using a switch in hopes that thingKind will usually be a compile-time constant. */
    switch (thingKind) {
      case AllocKind::FUNCTION:
      case AllocKind::OBJECT0:
      case AllocKind::OBJECT0_BACKGROUND:
        return 0;
      case AllocKind::FUNCTION_EXTENDED:
      case AllocKind::OBJECT2:
      case AllocKind::OBJECT2_BACKGROUND:
        return 2;
      case AllocKind::OBJECT4:
      case AllocKind::OBJECT4_BACKGROUND:
        return 4;
      case AllocKind::OBJECT8:
      case AllocKind::OBJECT8_BACKGROUND:
        return 8;
      case AllocKind::OBJECT12:
      case AllocKind::OBJECT12_BACKGROUND:
        return 12;
      case AllocKind::OBJECT16:
      case AllocKind::OBJECT16_BACKGROUND:
        return 16;
      default:
        MOZ_CRASH("Bad object alloc kind");
    }
}

static inline size_t
GetGCKindSlots(AllocKind thingKind, const Class* clasp)
{
    size_t nslots = GetGCKindSlots(thingKind);

    /* An object's private data uses the space taken by its last fixed slot. */
    if (clasp->flags & JSCLASS_HAS_PRIVATE) {
        MOZ_ASSERT(nslots > 0);
        nslots--;
    }

    /*
     * Functions have a larger alloc kind than AllocKind::OBJECT to reserve
     * space for the extra fields in JSFunction, but have no fixed slots.
     */
    if (clasp == FunctionClassPtr)
        nslots = 0;

    return nslots;
}

static inline size_t
GetGCKindBytes(AllocKind thingKind)
{
    return sizeof(JSObject_Slots0) + GetGCKindSlots(thingKind) * sizeof(Value);
}

// Class to assist in triggering background chunk allocation. This cannot be done
// while holding the GC or worker thread state lock due to lock ordering issues.
// As a result, the triggering is delayed using this class until neither of the
// above locks is held.
class AutoMaybeStartBackgroundAllocation;

/*
 * A single segment of a SortedArenaList. Each segment has a head and a tail,
 * which track the start and end of a segment for O(1) append and concatenation.
 */
struct SortedArenaListSegment
{
    ArenaHeader* head;
    ArenaHeader** tailp;

    void clear() {
        head = nullptr;
        tailp = &head;
    }

    bool isEmpty() const {
        return tailp == &head;
    }

    // Appends |aheader| to this segment.
    void append(ArenaHeader* aheader) {
        MOZ_ASSERT(aheader);
        MOZ_ASSERT_IF(head, head->getAllocKind() == aheader->getAllocKind());
        *tailp = aheader;
        tailp = &aheader->next;
    }

    // Points the tail of this segment at |aheader|, which may be null. Note
    // that this does not change the tail itself, but merely which arena
    // follows it. This essentially turns the tail into a cursor (see also the
    // description of ArenaList), but from the perspective of a SortedArenaList
    // this makes no difference.
    void linkTo(ArenaHeader* aheader) {
        *tailp = aheader;
    }
};

/*
 * Arena lists have a head and a cursor. The cursor conceptually lies on arena
 * boundaries, i.e. before the first arena, between two arenas, or after the
 * last arena.
 *
 * Normally the arena following the cursor is the first arena in the list with
 * some free things and all arenas before the cursor are fully allocated. (And
 * if the cursor is at the end of the list, then all the arenas are full.)
 *
 * However, the arena currently being allocated from is considered full while
 * its list of free spans is moved into the freeList. Therefore, during GC or
 * cell enumeration, when an unallocated freeList is moved back to the arena,
 * we can see an arena with some free cells before the cursor.
 *
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
    ArenaHeader*    head_;
    ArenaHeader**   cursorp_;

    void copy(const ArenaList& other) {
        other.check();
        head_ = other.head_;
        cursorp_ = other.isCursorAtHead() ? &head_ : other.cursorp_;
        check();
    }

  public:
    ArenaList() {
        clear();
    }

    ArenaList(const ArenaList& other) {
        copy(other);
    }

    ArenaList& operator=(const ArenaList& other) {
        copy(other);
        return *this;
    }

    explicit ArenaList(const SortedArenaListSegment& segment) {
        head_ = segment.head;
        cursorp_ = segment.isEmpty() ? &head_ : segment.tailp;
        check();
    }

    // This does checking just of |head_| and |cursorp_|.
    void check() const {
#ifdef DEBUG
        // If the list is empty, it must have this form.
        MOZ_ASSERT_IF(!head_, cursorp_ == &head_);

        // If there's an arena following the cursor, it must not be full.
        ArenaHeader* cursor = *cursorp_;
        MOZ_ASSERT_IF(cursor, cursor->hasFreeThings());
#endif
    }

    void clear() {
        head_ = nullptr;
        cursorp_ = &head_;
        check();
    }

    ArenaList copyAndClear() {
        ArenaList result = *this;
        clear();
        return result;
    }

    bool isEmpty() const {
        check();
        return !head_;
    }

    // This returns nullptr if the list is empty.
    ArenaHeader* head() const {
        check();
        return head_;
    }

    bool isCursorAtHead() const {
        check();
        return cursorp_ == &head_;
    }

    bool isCursorAtEnd() const {
        check();
        return !*cursorp_;
    }

    // This can return nullptr.
    ArenaHeader* arenaAfterCursor() const {
        check();
        return *cursorp_;
    }

    // This returns the arena after the cursor and moves the cursor past it.
    ArenaHeader* takeNextArena() {
        check();
        ArenaHeader* aheader = *cursorp_;
        if (!aheader)
            return nullptr;
        cursorp_ = &aheader->next;
        check();
        return aheader;
    }

    // This does two things.
    // - Inserts |a| at the cursor.
    // - Leaves the cursor sitting just before |a|, if |a| is not full, or just
    //   after |a|, if |a| is full.
    //
    void insertAtCursor(ArenaHeader* a) {
        check();
        a->next = *cursorp_;
        *cursorp_ = a;
        // At this point, the cursor is sitting before |a|. Move it after |a|
        // if necessary.
        if (!a->hasFreeThings())
            cursorp_ = &a->next;
        check();
    }

    // This inserts |other|, which must be full, at the cursor of |this|.
    ArenaList& insertListWithCursorAtEnd(const ArenaList& other) {
        check();
        other.check();
        MOZ_ASSERT(other.isCursorAtEnd());
        if (other.isCursorAtHead())
            return *this;
        // Insert the full arenas of |other| after those of |this|.
        *other.cursorp_ = *cursorp_;
        *cursorp_ = other.head_;
        cursorp_ = other.cursorp_;
        check();
        return *this;
    }

    ArenaHeader* removeRemainingArenas(ArenaHeader** arenap);
    ArenaHeader** pickArenasToRelocate(size_t& arenaTotalOut, size_t& relocTotalOut);
    ArenaHeader* relocateArenas(ArenaHeader* toRelocate, ArenaHeader* relocated,
                                SliceBudget& sliceBudget, gcstats::Statistics& stats);
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
    static const size_t MaxThingsPerArena = (ArenaSize - sizeof(ArenaHeader)) / MinThingSize;

    size_t thingsPerArena_;
    SortedArenaListSegment segments[MaxThingsPerArena + 1];

    // Convenience functions to get the nth head and tail.
    ArenaHeader* headAt(size_t n) { return segments[n].head; }
    ArenaHeader** tailAt(size_t n) { return segments[n].tailp; }

  public:
    explicit SortedArenaList(size_t thingsPerArena = MaxThingsPerArena) {
        reset(thingsPerArena);
    }

    void setThingsPerArena(size_t thingsPerArena) {
        MOZ_ASSERT(thingsPerArena && thingsPerArena <= MaxThingsPerArena);
        thingsPerArena_ = thingsPerArena;
    }

    // Resets the first |thingsPerArena| segments of this list for further use.
    void reset(size_t thingsPerArena = MaxThingsPerArena) {
        setThingsPerArena(thingsPerArena);
        // Initialize the segments.
        for (size_t i = 0; i <= thingsPerArena; ++i)
            segments[i].clear();
    }

    // Inserts a header, which has room for |nfree| more things, in its segment.
    void insertAt(ArenaHeader* aheader, size_t nfree) {
        MOZ_ASSERT(nfree <= thingsPerArena_);
        segments[nfree].append(aheader);
    }

    // Remove all empty arenas, inserting them as a linked list.
    void extractEmpty(ArenaHeader** empty) {
        SortedArenaListSegment& segment = segments[thingsPerArena_];
        if (segment.head) {
            *segment.tailp = *empty;
            *empty = segment.head;
            segment.clear();
        }
    }

    // Links up the tail of each non-empty segment to the head of the next
    // non-empty segment, creating a contiguous list that is returned as an
    // ArenaList. This is not a destructive operation: neither the head nor tail
    // of any segment is modified. However, note that the ArenaHeaders in the
    // resulting ArenaList should be treated as read-only unless the
    // SortedArenaList is no longer needed: inserting or removing arenas would
    // invalidate the SortedArenaList.
    ArenaList toArenaList() {
        // Link the non-empty segment tails up to the non-empty segment heads.
        size_t tailIndex = 0;
        for (size_t headIndex = 1; headIndex <= thingsPerArena_; ++headIndex) {
            if (headAt(headIndex)) {
                segments[tailIndex].linkTo(headAt(headIndex));
                tailIndex = headIndex;
            }
        }
        // Point the tail of the final non-empty segment at null. Note that if
        // the list is empty, this will just set segments[0].head to null.
        segments[tailIndex].linkTo(nullptr);
        // Create an ArenaList with head and cursor set to the head and tail of
        // the first segment (if that segment is empty, only the head is used).
        return ArenaList(segments[0]);
    }
};

class ArenaLists
{
    JSRuntime* runtime_;

    /*
     * For each arena kind its free list is represented as the first span with
     * free things. Initially all the spans are initialized as empty. After we
     * find a new arena with available things we move its first free span into
     * the list and set the arena as fully allocated. way we do not need to
     * update the arena header after the initial allocation. When starting the
     * GC we only move the head of the of the list of spans back to the arena
     * only for the arena that was not fully allocated.
     */
    AllAllocKindArray<FreeList> freeLists;

    AllAllocKindArray<ArenaList> arenaLists;

    enum BackgroundFinalizeStateEnum { BFS_DONE, BFS_RUN };

    typedef mozilla::Atomic<BackgroundFinalizeStateEnum, mozilla::ReleaseAcquire>
        BackgroundFinalizeState;

    /* The current background finalization state, accessed atomically. */
    AllAllocKindArray<BackgroundFinalizeState> backgroundFinalizeState;

    /* For each arena kind, a list of arenas remaining to be swept. */
    AllAllocKindArray<ArenaHeader*> arenaListsToSweep;

    /* During incremental sweeping, a list of the arenas already swept. */
    AllocKind incrementalSweptArenaKind;
    ArenaList incrementalSweptArenas;

    // Arena lists which have yet to be swept, but need additional foreground
    // processing before they are swept.
    ArenaHeader* gcShapeArenasToUpdate;
    ArenaHeader* gcAccessorShapeArenasToUpdate;
    ArenaHeader* gcScriptArenasToUpdate;
    ArenaHeader* gcObjectGroupArenasToUpdate;

    // While sweeping type information, these lists save the arenas for the
    // objects which have already been finalized in the foreground (which must
    // happen at the beginning of the GC), so that type sweeping can determine
    // which of the object pointers are marked.
    ObjectAllocKindArray<ArenaList> savedObjectArenas;
    ArenaHeader* savedEmptyObjectArenas;

  public:
    explicit ArenaLists(JSRuntime* rt) : runtime_(rt) {
        for (auto i : AllAllocKinds())
            freeLists[i].initAsEmpty();
        for (auto i : AllAllocKinds())
            backgroundFinalizeState[i] = BFS_DONE;
        for (auto i : AllAllocKinds())
            arenaListsToSweep[i] = nullptr;
        incrementalSweptArenaKind = AllocKind::LIMIT;
        gcShapeArenasToUpdate = nullptr;
        gcAccessorShapeArenasToUpdate = nullptr;
        gcScriptArenasToUpdate = nullptr;
        gcObjectGroupArenasToUpdate = nullptr;
        savedEmptyObjectArenas = nullptr;
    }

    ~ArenaLists();

    static uintptr_t getFreeListOffset(AllocKind thingKind) {
        uintptr_t offset = offsetof(ArenaLists, freeLists);
        return offset + size_t(thingKind) * sizeof(FreeList);
    }

    const FreeList* getFreeList(AllocKind thingKind) const {
        return &freeLists[thingKind];
    }

    ArenaHeader* getFirstArena(AllocKind thingKind) const {
        return arenaLists[thingKind].head();
    }

    ArenaHeader* getFirstArenaToSweep(AllocKind thingKind) const {
        return arenaListsToSweep[thingKind];
    }

    ArenaHeader* getFirstSweptArena(AllocKind thingKind) const {
        if (thingKind != incrementalSweptArenaKind)
            return nullptr;
        return incrementalSweptArenas.head();
    }

    ArenaHeader* getArenaAfterCursor(AllocKind thingKind) const {
        return arenaLists[thingKind].arenaAfterCursor();
    }

    bool arenaListsAreEmpty() const {
        for (auto i : AllAllocKinds()) {
            /*
             * The arena cannot be empty if the background finalization is not yet
             * done.
             */
            if (backgroundFinalizeState[i] != BFS_DONE)
                return false;
            if (!arenaLists[i].isEmpty())
                return false;
        }
        return true;
    }

    void unmarkAll() {
        for (auto i : AllAllocKinds()) {
            /* The background finalization must have stopped at this point. */
            MOZ_ASSERT(backgroundFinalizeState[i] == BFS_DONE);
            for (ArenaHeader* aheader = arenaLists[i].head(); aheader; aheader = aheader->next)
                aheader->unmarkAll();
        }
    }

    bool doneBackgroundFinalize(AllocKind kind) const {
        return backgroundFinalizeState[kind] == BFS_DONE;
    }

    bool needBackgroundFinalizeWait(AllocKind kind) const {
        return backgroundFinalizeState[kind] != BFS_DONE;
    }

    /*
     * Return the free list back to the arena so the GC finalization will not
     * run the finalizers over unitialized bytes from free things.
     */
    void purge() {
        for (auto i : AllAllocKinds())
            purge(i);
    }

    void purge(AllocKind i) {
        FreeList* freeList = &freeLists[i];
        if (!freeList->isEmpty()) {
            ArenaHeader* aheader = freeList->arenaHeader();
            aheader->setFirstFreeSpan(freeList->getHead());
            freeList->initAsEmpty();
        }
    }

    inline void prepareForIncrementalGC(JSRuntime* rt);

    /*
     * Temporarily copy the free list heads to the arenas so the code can see
     * the proper value in ArenaHeader::freeList when accessing the latter
     * outside the GC.
     */
    void copyFreeListsToArenas() {
        for (auto i : AllAllocKinds())
            copyFreeListToArena(i);
    }

    void copyFreeListToArena(AllocKind thingKind) {
        FreeList* freeList = &freeLists[thingKind];
        if (!freeList->isEmpty()) {
            ArenaHeader* aheader = freeList->arenaHeader();
            MOZ_ASSERT(!aheader->hasFreeThings());
            aheader->setFirstFreeSpan(freeList->getHead());
        }
    }

    /*
     * Clear the free lists in arenas that were temporarily set there using
     * copyToArenas.
     */
    void clearFreeListsInArenas() {
        for (auto i : AllAllocKinds())
            clearFreeListInArena(i);
    }

    void clearFreeListInArena(AllocKind kind) {
        FreeList* freeList = &freeLists[kind];
        if (!freeList->isEmpty()) {
            ArenaHeader* aheader = freeList->arenaHeader();
            MOZ_ASSERT(freeList->isSameNonEmptySpan(aheader->getFirstFreeSpan()));
            aheader->setAsFullyUsed();
        }
    }

    /*
     * Check that the free list is either empty or were synchronized with the
     * arena using copyToArena().
     */
    bool isSynchronizedFreeList(AllocKind kind) {
        FreeList* freeList = &freeLists[kind];
        if (freeList->isEmpty())
            return true;
        ArenaHeader* aheader = freeList->arenaHeader();
        if (aheader->hasFreeThings()) {
            /*
             * If the arena has a free list, it must be the same as one in
             * lists.
             */
            MOZ_ASSERT(freeList->isSameNonEmptySpan(aheader->getFirstFreeSpan()));
            return true;
        }
        return false;
    }

    /* Check if |aheader|'s arena is in use. */
    bool arenaIsInUse(ArenaHeader* aheader, AllocKind kind) const {
        MOZ_ASSERT(aheader);
        const FreeList& freeList = freeLists[kind];
        if (freeList.isEmpty())
            return false;
        return aheader == freeList.arenaHeader();
    }

    MOZ_ALWAYS_INLINE TenuredCell* allocateFromFreeList(AllocKind thingKind, size_t thingSize) {
        return freeLists[thingKind].allocate(thingSize);
    }

    /*
     * Moves all arenas from |fromArenaLists| into |this|.
     */
    void adoptArenas(JSRuntime* runtime, ArenaLists* fromArenaLists);

    /* True if the ArenaHeader in question is found in this ArenaLists */
    bool containsArena(JSRuntime* runtime, ArenaHeader* arenaHeader);

    void checkEmptyFreeLists() {
#ifdef DEBUG
        for (auto i : AllAllocKinds())
            checkEmptyFreeList(i);
#endif
    }

    void checkEmptyFreeList(AllocKind kind) {
        MOZ_ASSERT(freeLists[kind].isEmpty());
    }

    bool relocateArenas(Zone* zone, ArenaHeader*& relocatedListOut, JS::gcreason::Reason reason,
                        SliceBudget& sliceBudget, gcstats::Statistics& stats);

    void queueForegroundObjectsForSweep(FreeOp* fop);
    void queueForegroundThingsForSweep(FreeOp* fop);

    void mergeForegroundSweptObjectArenas();

    bool foregroundFinalize(FreeOp* fop, AllocKind thingKind, SliceBudget& sliceBudget,
                            SortedArenaList& sweepList);
    static void backgroundFinalize(FreeOp* fop, ArenaHeader* listHead, ArenaHeader** empty);

    // When finalizing arenas, whether to keep empty arenas on the list or
    // release them immediately.
    enum KeepArenasEnum {
        RELEASE_ARENAS,
        KEEP_ARENAS
    };

  private:
    inline void finalizeNow(FreeOp* fop, const FinalizePhase& phase);
    inline void queueForForegroundSweep(FreeOp* fop, const FinalizePhase& phase);
    inline void queueForBackgroundSweep(FreeOp* fop, const FinalizePhase& phase);

    inline void finalizeNow(FreeOp* fop, AllocKind thingKind,
                            KeepArenasEnum keepArenas, ArenaHeader** empty = nullptr);
    inline void forceFinalizeNow(FreeOp* fop, AllocKind thingKind,
                                 KeepArenasEnum keepArenas, ArenaHeader** empty = nullptr);
    inline void queueForForegroundSweep(FreeOp* fop, AllocKind thingKind);
    inline void queueForBackgroundSweep(FreeOp* fop, AllocKind thingKind);
    inline void mergeSweptArenas(AllocKind thingKind);

    TenuredCell* allocateFromArena(JS::Zone* zone, AllocKind thingKind,
                                   AutoMaybeStartBackgroundAllocation& maybeStartBGAlloc);

    enum ArenaAllocMode { HasFreeThings = true, IsEmpty = false };
    template <ArenaAllocMode hasFreeThings>
    TenuredCell* allocateFromArenaInner(JS::Zone* zone, ArenaHeader* aheader, AllocKind kind);

    inline void normalizeBackgroundFinalizeState(AllocKind thingKind);

    friend class GCRuntime;
    friend class js::Nursery;
    friend class js::TenuringTracer;
};

/* The number of GC cycles an empty chunk can survive before been released. */
const size_t MAX_EMPTY_CHUNK_AGE = 4;

} /* namespace gc */

extern bool
InitGC(JSRuntime* rt, uint32_t maxbytes);

extern void
FinishGC(JSRuntime* rt);

class InterpreterFrame;

extern void
MarkCompartmentActive(js::InterpreterFrame* fp);

extern void
TraceRuntime(JSTracer* trc);

extern void
ReleaseAllJITCode(FreeOp* op);

extern void
PrepareForDebugGC(JSRuntime* rt);

/* Functions for managing cross compartment gray pointers. */

extern void
DelayCrossCompartmentGrayMarking(JSObject* src);

extern void
NotifyGCNukeWrapper(JSObject* o);

extern unsigned
NotifyGCPreSwap(JSObject* a, JSObject* b);

extern void
NotifyGCPostSwap(JSObject* a, JSObject* b, unsigned preResult);

/*
 * Helper state for use when JS helper threads sweep and allocate GC thing kinds
 * that can be swept and allocated off the main thread.
 *
 * In non-threadsafe builds, all actual sweeping and allocation is performed
 * on the main thread, but GCHelperState encapsulates this from clients as
 * much as possible.
 */
class GCHelperState
{
    enum State {
        IDLE,
        SWEEPING
    };

    // Associated runtime.
    JSRuntime* const rt;

    // Condvar for notifying the main thread when work has finished. This is
    // associated with the runtime's GC lock --- the worker thread state
    // condvars can't be used here due to lock ordering issues.
    PRCondVar* done;

    // Activity for the helper to do, protected by the GC lock.
    State state_;

    // Thread which work is being performed on, or null.
    PRThread* thread;

    void startBackgroundThread(State newState);
    void waitForBackgroundThread();

    State state();
    void setState(State state);

    bool shrinkFlag;

    friend class js::gc::ArenaLists;

    static void freeElementsAndArray(void** array, void** end) {
        MOZ_ASSERT(array <= end);
        for (void** p = array; p != end; ++p)
            js_free(*p);
        js_free(array);
    }

    void doSweep(AutoLockGC& lock);

  public:
    explicit GCHelperState(JSRuntime* rt)
      : rt(rt),
        done(nullptr),
        state_(IDLE),
        thread(nullptr),
        shrinkFlag(false)
    { }

    bool init();
    void finish();

    void work();

    void maybeStartBackgroundSweep(const AutoLockGC& lock);
    void startBackgroundShrink(const AutoLockGC& lock);

    /* Must be called without the GC lock taken. */
    void waitBackgroundSweepEnd();

    bool onBackgroundThread();

    /*
     * Outside the GC lock may give true answer when in fact the sweeping has
     * been done.
     */
    bool isBackgroundSweeping() const {
        return state_ == SWEEPING;
    }

    bool shouldShrink() const {
        MOZ_ASSERT(isBackgroundSweeping());
        return shrinkFlag;
    }
};

// A generic task used to dispatch work to the helper thread system.
// Users should derive from GCParallelTask add what data they need and
// override |run|.
class GCParallelTask
{
    // The state of the parallel computation.
    enum TaskState {
        NotStarted,
        Dispatched,
        Finished,
    } state;

    // Amount of time this task took to execute.
    uint64_t duration_;

  protected:
    // A flag to signal a request for early completion of the off-thread task.
    mozilla::Atomic<bool> cancel_;

    virtual void run() = 0;

  public:
    GCParallelTask() : state(NotStarted), duration_(0) {}

    // Derived classes must override this to ensure that join() gets called
    // before members get destructed.
    virtual ~GCParallelTask();

    // Time spent in the most recent invocation of this task.
    int64_t duration() const { return duration_; }

    // The simple interface to a parallel task works exactly like pthreads.
    bool start();
    void join();

    // If multiple tasks are to be started or joined at once, it is more
    // efficient to take the helper thread lock once and use these methods.
    bool startWithLockHeld();
    void joinWithLockHeld();

    // Instead of dispatching to a helper, run the task on the main thread.
    void runFromMainThread(JSRuntime* rt);

    // Dispatch a cancelation request.
    enum CancelMode { CancelNoWait, CancelAndWait};
    void cancel(CancelMode mode = CancelNoWait) {
        cancel_ = true;
        if (mode == CancelAndWait)
            join();
    }

    // Check if a task is actively running.
    bool isRunning() const;

    // This should be friended to HelperThread, but cannot be because it
    // would introduce several circular dependencies.
  public:
    virtual void runFromHelperThread();
};

typedef void (*IterateChunkCallback)(JSRuntime* rt, void* data, gc::Chunk* chunk);
typedef void (*IterateZoneCallback)(JSRuntime* rt, void* data, JS::Zone* zone);
typedef void (*IterateArenaCallback)(JSRuntime* rt, void* data, gc::Arena* arena,
                                     JS::TraceKind traceKind, size_t thingSize);
typedef void (*IterateCellCallback)(JSRuntime* rt, void* data, void* thing,
                                    JS::TraceKind traceKind, size_t thingSize);

/*
 * This function calls |zoneCallback| on every zone, |compartmentCallback| on
 * every compartment, |arenaCallback| on every in-use arena, and |cellCallback|
 * on every in-use cell in the GC heap.
 */
extern void
IterateZonesCompartmentsArenasCells(JSRuntime* rt, void* data,
                                    IterateZoneCallback zoneCallback,
                                    JSIterateCompartmentCallback compartmentCallback,
                                    IterateArenaCallback arenaCallback,
                                    IterateCellCallback cellCallback);

/*
 * This function is like IterateZonesCompartmentsArenasCells, but does it for a
 * single zone.
 */
extern void
IterateZoneCompartmentsArenasCells(JSRuntime* rt, Zone* zone, void* data,
                                   IterateZoneCallback zoneCallback,
                                   JSIterateCompartmentCallback compartmentCallback,
                                   IterateArenaCallback arenaCallback,
                                   IterateCellCallback cellCallback);

/*
 * Invoke chunkCallback on every in-use chunk.
 */
extern void
IterateChunks(JSRuntime* rt, void* data, IterateChunkCallback chunkCallback);

typedef void (*IterateScriptCallback)(JSRuntime* rt, void* data, JSScript* script);

/*
 * Invoke scriptCallback on every in-use script for
 * the given compartment or for all compartments if it is null.
 */
extern void
IterateScripts(JSRuntime* rt, JSCompartment* compartment,
               void* data, IterateScriptCallback scriptCallback);

extern void
FinalizeStringRT(JSRuntime* rt, JSString* str);

JSCompartment*
NewCompartment(JSContext* cx, JS::Zone* zone, JSPrincipals* principals,
               const JS::CompartmentOptions& options);

namespace gc {

/*
 * Merge all contents of source into target. This can only be used if source is
 * the only compartment in its zone.
 */
void
MergeCompartments(JSCompartment* source, JSCompartment* target);

/*
 * This structure overlays a Cell in the Nursery and re-purposes its memory
 * for managing the Nursery collection process.
 */
class RelocationOverlay
{
    /* The low bit is set so this should never equal a normal pointer. */
    static const uintptr_t Relocated = uintptr_t(0xbad0bad1);

    // Arrange the fields of the RelocationOverlay so that JSObject's group
    // pointer is not overwritten during compacting.

    /* A list entry to track all relocated things. */
    RelocationOverlay* next_;

    /* Set to Relocated when moved. */
    uintptr_t magic_;

    /* The location |this| was moved to. */
    Cell* newLocation_;

  public:
    static RelocationOverlay* fromCell(Cell* cell) {
        return reinterpret_cast<RelocationOverlay*>(cell);
    }

    bool isForwarded() const {
        return magic_ == Relocated;
    }

    Cell* forwardingAddress() const {
        MOZ_ASSERT(isForwarded());
        return newLocation_;
    }

    void forwardTo(Cell* cell) {
        MOZ_ASSERT(!isForwarded());
        static_assert(offsetof(JSObject, group_) == offsetof(RelocationOverlay, next_),
                      "next pointer and group should be at same location, "
                      "so that group is not overwritten during compacting");
        newLocation_ = cell;
        magic_ = Relocated;
    }

    RelocationOverlay*& nextRef() {
        MOZ_ASSERT(isForwarded());
        return next_;
    }

    RelocationOverlay* next() const {
        MOZ_ASSERT(isForwarded());
        return next_;
    }

    static bool isCellForwarded(Cell* cell) {
        return fromCell(cell)->isForwarded();
    }
};

/* Functions for checking and updating things that might be moved by compacting GC. */

template <typename T>
struct MightBeForwarded
{
    static_assert(mozilla::IsBaseOf<Cell, T>::value,
                  "T must derive from Cell");
    static_assert(!mozilla::IsSame<Cell, T>::value && !mozilla::IsSame<TenuredCell, T>::value,
                  "T must not be Cell or TenuredCell");

    static const bool value = mozilla::IsBaseOf<JSObject, T>::value;
};

template <typename T>
inline bool
IsForwarded(T* t)
{
    RelocationOverlay* overlay = RelocationOverlay::fromCell(t);
    if (!MightBeForwarded<T>::value) {
        MOZ_ASSERT(!overlay->isForwarded());
        return false;
    }

    return overlay->isForwarded();
}

struct IsForwardedFunctor : public BoolDefaultAdaptor<Value, false> {
    template <typename T> bool operator()(T* t) { return IsForwarded(t); }
};

inline bool
IsForwarded(const JS::Value& value)
{
    return DispatchTyped(IsForwardedFunctor(), value);
}

template <typename T>
inline T*
Forwarded(T* t)
{
    RelocationOverlay* overlay = RelocationOverlay::fromCell(t);
    MOZ_ASSERT(overlay->isForwarded());
    return reinterpret_cast<T*>(overlay->forwardingAddress());
}

struct ForwardedFunctor : public IdentityDefaultAdaptor<Value> {
    template <typename T> inline Value operator()(T* t) {
        return js::gc::RewrapTaggedPointer<Value, T*>::wrap(Forwarded(t));
    }
};

inline Value
Forwarded(const JS::Value& value)
{
    return DispatchTyped(ForwardedFunctor(), value);
}

template <typename T>
inline T
MaybeForwarded(T t)
{
    return IsForwarded(t) ? Forwarded(t) : t;
}

#ifdef JSGC_HASH_TABLE_CHECKS

template <typename T>
inline void
CheckGCThingAfterMovingGC(T* t)
{
    if (t) {
        MOZ_RELEASE_ASSERT(!IsInsideNursery(t));
        MOZ_RELEASE_ASSERT(!RelocationOverlay::isCellForwarded(t));
    }
}

template <typename T>
inline void
CheckGCThingAfterMovingGC(const ReadBarriered<T*>& t)
{
    CheckGCThingAfterMovingGC(t.unbarrieredGet());
}

struct CheckValueAfterMovingGCFunctor : public VoidDefaultAdaptor<Value> {
    template <typename T> void operator()(T* t) { CheckGCThingAfterMovingGC(t); }
};

inline void
CheckValueAfterMovingGC(const JS::Value& value)
{
    DispatchTyped(CheckValueAfterMovingGCFunctor(), value);
}

#endif // JSGC_HASH_TABLE_CHECKS

const int ZealPokeValue = 1;
const int ZealAllocValue = 2;
const int ZealFrameGCValue = 3;
const int ZealVerifierPreValue = 4;
const int ZealFrameVerifierPreValue = 5;
const int ZealStackRootingValue = 6;
const int ZealGenerationalGCValue = 7;
const int ZealIncrementalRootsThenFinish = 8;
const int ZealIncrementalMarkAllThenFinish = 9;
const int ZealIncrementalMultipleSlices = 10;
const int ZealCheckHashTablesOnMinorGC = 13;
const int ZealCompactValue = 14;
const int ZealLimit = 14;

enum VerifierType {
    PreBarrierVerifier
};

#ifdef JS_GC_ZEAL

extern const char* ZealModeHelpText;

/* Check that write barriers have been used correctly. See jsgc.cpp. */
void
VerifyBarriers(JSRuntime* rt, VerifierType type);

void
MaybeVerifyBarriers(JSContext* cx, bool always = false);

#else

static inline void
VerifyBarriers(JSRuntime* rt, VerifierType type)
{
}

static inline void
MaybeVerifyBarriers(JSContext* cx, bool always = false)
{
}

#endif

/*
 * Instances of this class set the |JSRuntime::suppressGC| flag for the duration
 * that they are live. Use of this class is highly discouraged. Please carefully
 * read the comment in vm/Runtime.h above |suppressGC| and take all appropriate
 * precautions before instantiating this class.
 */
class MOZ_RAII AutoSuppressGC
{
    int32_t& suppressGC_;

  public:
    explicit AutoSuppressGC(ExclusiveContext* cx);
    explicit AutoSuppressGC(JSCompartment* comp);
    explicit AutoSuppressGC(JSRuntime* rt);

    ~AutoSuppressGC()
    {
        suppressGC_--;
    }
};

// A singly linked list of zones.
class ZoneList
{
    static Zone * const End;

    Zone* head;
    Zone* tail;

  public:
    ZoneList();
    ~ZoneList();

    bool isEmpty() const;
    Zone* front() const;

    void append(Zone* zone);
    void transferFrom(ZoneList& other);
    void removeFront();
    void clear();

  private:
    explicit ZoneList(Zone* singleZone);
    void check() const;

    ZoneList(const ZoneList& other) = delete;
    ZoneList& operator=(const ZoneList& other) = delete;
};

JSObject*
NewMemoryStatisticsObject(JSContext* cx);

} /* namespace gc */

#ifdef DEBUG
/* Use this to avoid assertions when manipulating the wrapper map. */
class MOZ_RAII AutoDisableProxyCheck
{
    gc::GCRuntime& gc;

  public:
    explicit AutoDisableProxyCheck(JSRuntime* rt);
    ~AutoDisableProxyCheck();
};
#else
struct MOZ_RAII AutoDisableProxyCheck
{
    explicit AutoDisableProxyCheck(JSRuntime* rt) {}
};
#endif

struct MOZ_RAII AutoDisableCompactingGC
{
    explicit AutoDisableCompactingGC(JSRuntime* rt);
    ~AutoDisableCompactingGC();

  private:
    gc::GCRuntime& gc;
};

void
PurgeJITCaches(JS::Zone* zone);

// This is the same as IsInsideNursery, but not inlined.
bool
UninlinedIsInsideNursery(const gc::Cell* cell);

} /* namespace js */

#endif /* jsgc_h */
