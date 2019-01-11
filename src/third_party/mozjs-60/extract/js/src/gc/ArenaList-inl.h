/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_ArenaList_inl_h
#define gc_ArenaList_inl_h

#include "gc/ArenaList.h"

#include "gc/Heap.h"

void
js::gc::SortedArenaListSegment::append(Arena* arena)
{
    MOZ_ASSERT(arena);
    MOZ_ASSERT_IF(head, head->getAllocKind() == arena->getAllocKind());
    *tailp = arena;
    tailp = &arena->next;
}

inline
js::gc::ArenaList::ArenaList()
{
    clear();
}

void
js::gc::ArenaList::copy(const ArenaList& other)
{
    other.check();
    head_ = other.head_;
    cursorp_ = other.isCursorAtHead() ? &head_ : other.cursorp_;
    check();
}

inline
js::gc::ArenaList::ArenaList(const ArenaList& other)
{
    copy(other);
}

js::gc::ArenaList&
js::gc::ArenaList::operator=(const ArenaList& other)
{
    copy(other);
    return *this;
}

inline
js::gc::ArenaList::ArenaList(const SortedArenaListSegment& segment)
{
    head_ = segment.head;
    cursorp_ = segment.isEmpty() ? &head_ : segment.tailp;
    check();
}

// This does checking just of |head_| and |cursorp_|.
void
js::gc::ArenaList::check() const
{
#ifdef DEBUG
    // If the list is empty, it must have this form.
    MOZ_ASSERT_IF(!head_, cursorp_ == &head_);

    // If there's an arena following the cursor, it must not be full.
    Arena* cursor = *cursorp_;
    MOZ_ASSERT_IF(cursor, cursor->hasFreeThings());
#endif
}

void
js::gc::ArenaList::clear()
{
    head_ = nullptr;
    cursorp_ = &head_;
    check();
}

js::gc::ArenaList
js::gc::ArenaList::copyAndClear()
{
    ArenaList result = *this;
    clear();
    return result;
}

bool
js::gc::ArenaList::isEmpty() const
{
    check();
    return !head_;
}

js::gc::Arena*
js::gc::ArenaList::head() const
{
    check();
    return head_;
}

bool
js::gc::ArenaList::isCursorAtHead() const
{
    check();
    return cursorp_ == &head_;
}

bool
js::gc::ArenaList::isCursorAtEnd() const
{
    check();
    return !*cursorp_;
}

void
js::gc::ArenaList::moveCursorToEnd()
{
    while (!isCursorAtEnd())
        cursorp_ = &(*cursorp_)->next;
}

js::gc::Arena*
js::gc::ArenaList::arenaAfterCursor() const
{
    check();
    return *cursorp_;
}

js::gc::Arena*
js::gc::ArenaList::takeNextArena()
{
    check();
    Arena* arena = *cursorp_;
    if (!arena)
        return nullptr;
    cursorp_ = &arena->next;
    check();
    return arena;
}

void
js::gc::ArenaList::insertAtCursor(Arena* a)
{
    check();
    a->next = *cursorp_;
    *cursorp_ = a;
    // At this point, the cursor is sitting before |a|. Move it after |a|
    // if necessary.
    if (!a->hasFreeThings())
        cursorp_ = &a->next;
    check();
}

void
js::gc::ArenaList::insertBeforeCursor(Arena* a)
{
    check();
    a->next = *cursorp_;
    *cursorp_ = a;
    cursorp_ = &a->next;
    check();
}

js::gc::ArenaList&
js::gc::ArenaList::insertListWithCursorAtEnd(const ArenaList& other)
{
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

js::gc::SortedArenaList::SortedArenaList(size_t thingsPerArena)
{
    reset(thingsPerArena);
}

void
js::gc::SortedArenaList::setThingsPerArena(size_t thingsPerArena)
{
    MOZ_ASSERT(thingsPerArena && thingsPerArena <= MaxThingsPerArena);
    thingsPerArena_ = thingsPerArena;
}

void
js::gc::SortedArenaList::reset(size_t thingsPerArena)
{
    setThingsPerArena(thingsPerArena);
    // Initialize the segments.
    for (size_t i = 0; i <= thingsPerArena; ++i)
        segments[i].clear();
}

void
js::gc::SortedArenaList::insertAt(Arena* arena, size_t nfree)
{
    MOZ_ASSERT(nfree <= thingsPerArena_);
    segments[nfree].append(arena);
}

void
js::gc::SortedArenaList::extractEmpty(Arena** empty)
{
    SortedArenaListSegment& segment = segments[thingsPerArena_];
    if (segment.head) {
        *segment.tailp = *empty;
        *empty = segment.head;
        segment.clear();
    }
}

js::gc::ArenaList
js::gc::SortedArenaList::toArenaList()
{
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

void
js::gc::ArenaLists::setFreeList(AllocKind i, FreeSpan* span)
{
#ifdef DEBUG
    auto old = freeList(i);
    if (!old->isEmpty())
        old->getArena()->checkNoMarkedFreeCells();
#endif
    freeLists()[i] = span;
}

void
js::gc::ArenaLists::clearFreeList(AllocKind i)
{
#ifdef DEBUG
    auto old = freeList(i);
    if (!old->isEmpty())
        old->getArena()->checkNoMarkedFreeCells();
#endif
    freeLists()[i] = &placeholder;
}

js::gc::Arena*
js::gc::ArenaLists::getFirstArena(AllocKind thingKind) const
{
    return arenaLists(thingKind).head();
}

js::gc::Arena*
js::gc::ArenaLists::getFirstArenaToSweep(AllocKind thingKind) const
{
    return arenaListsToSweep(thingKind);
}

js::gc::Arena*
js::gc::ArenaLists::getFirstSweptArena(AllocKind thingKind) const
{
    if (thingKind != incrementalSweptArenaKind.ref())
        return nullptr;
    return incrementalSweptArenas.ref().head();
}

js::gc::Arena*
js::gc::ArenaLists::getArenaAfterCursor(AllocKind thingKind) const
{
    return arenaLists(thingKind).arenaAfterCursor();
}

bool
js::gc::ArenaLists::arenaListsAreEmpty() const
{
    for (auto i : AllAllocKinds()) {
        /*
         * The arena cannot be empty if the background finalization is not yet
         * done.
         */
        if (backgroundFinalizeState(i) != BFS_DONE)
            return false;
        if (!arenaLists(i).isEmpty())
            return false;
    }
    return true;
}

void
js::gc::ArenaLists::unmarkAll()
{
    for (auto i : AllAllocKinds()) {
        /* The background finalization must have stopped at this point. */
        MOZ_ASSERT(backgroundFinalizeState(i) == BFS_DONE);
        for (Arena* arena = arenaLists(i).head(); arena; arena = arena->next)
            arena->unmarkAll();
    }
}

bool
js::gc::ArenaLists::doneBackgroundFinalize(AllocKind kind) const
{
    return backgroundFinalizeState(kind) == BFS_DONE;
}

bool
js::gc::ArenaLists::needBackgroundFinalizeWait(AllocKind kind) const
{
    return backgroundFinalizeState(kind) != BFS_DONE;
}

void
js::gc::ArenaLists::clearFreeLists()
{
    for (auto i : AllAllocKinds())
        clearFreeList(i);
}

bool
js::gc::ArenaLists::arenaIsInUse(Arena* arena, AllocKind kind) const
{
    MOZ_ASSERT(arena);
    return arena == freeList(kind)->getArenaUnchecked();
}

MOZ_ALWAYS_INLINE js::gc::TenuredCell*
js::gc::ArenaLists::allocateFromFreeList(AllocKind thingKind, size_t thingSize)
{
    return freeList(thingKind)->allocate(thingSize);
}

void
js::gc::ArenaLists::checkEmptyFreeLists()
{
#ifdef DEBUG
    for (auto i : AllAllocKinds())
        checkEmptyFreeList(i);
#endif
}

bool
js::gc::ArenaLists::checkEmptyArenaLists()
{
    bool empty = true;
#ifdef DEBUG
    for (auto i : AllAllocKinds()) {
        if (!checkEmptyArenaList(i))
            empty = false;
    }
#endif
    return empty;
}

void
js::gc::ArenaLists::checkEmptyFreeList(AllocKind kind)
{
    MOZ_ASSERT(freeList(kind)->isEmpty());
}

#endif // gc_ArenaList_inl_h
