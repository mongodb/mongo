/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_ArenaList_inl_h
#define gc_ArenaList_inl_h

#include "gc/ArenaList.h"

#include "gc/Heap.h"
#include "gc/Zone.h"

void js::gc::SortedArenaListSegment::append(Arena* arena) {
  MOZ_ASSERT(arena);
  MOZ_ASSERT_IF(head, head->getAllocKind() == arena->getAllocKind());
  *tailp = arena;
  tailp = &arena->next;
}

inline js::gc::ArenaList::ArenaList() { clear(); }

inline js::gc::ArenaList::ArenaList(ArenaList&& other) { moveFrom(other); }

inline js::gc::ArenaList::~ArenaList() { MOZ_ASSERT(isEmpty()); }

void js::gc::ArenaList::moveFrom(ArenaList& other) {
  other.check();

  head_ = other.head_;
  cursorp_ = other.isCursorAtHead() ? &head_ : other.cursorp_;
  other.clear();

  check();
}

js::gc::ArenaList& js::gc::ArenaList::operator=(ArenaList&& other) {
  MOZ_ASSERT(isEmpty());
  moveFrom(other);
  return *this;
}

inline js::gc::ArenaList::ArenaList(const SortedArenaListSegment& segment) {
  head_ = segment.head;
  cursorp_ = segment.isEmpty() ? &head_ : segment.tailp;
  check();
}

// This does checking just of |head_| and |cursorp_|.
void js::gc::ArenaList::check() const {
#ifdef DEBUG
  // If the list is empty, it must have this form.
  MOZ_ASSERT_IF(!head_, cursorp_ == &head_);

  // If there's an arena following the cursor, it must not be full.
  Arena* cursor = *cursorp_;
  MOZ_ASSERT_IF(cursor, cursor->hasFreeThings());
#endif
}

void js::gc::ArenaList::clear() {
  head_ = nullptr;
  cursorp_ = &head_;
  check();
}

bool js::gc::ArenaList::isEmpty() const {
  check();
  return !head_;
}

js::gc::Arena* js::gc::ArenaList::head() const {
  check();
  return head_;
}

bool js::gc::ArenaList::isCursorAtHead() const {
  check();
  return cursorp_ == &head_;
}

bool js::gc::ArenaList::isCursorAtEnd() const {
  check();
  return !*cursorp_;
}

js::gc::Arena* js::gc::ArenaList::arenaAfterCursor() const {
  check();
  return *cursorp_;
}

js::gc::Arena* js::gc::ArenaList::takeNextArena() {
  check();
  Arena* arena = *cursorp_;
  if (!arena) {
    return nullptr;
  }
  cursorp_ = &arena->next;
  check();
  return arena;
}

void js::gc::ArenaList::insertAtCursor(Arena* a) {
  check();
  a->next = *cursorp_;
  *cursorp_ = a;
  // At this point, the cursor is sitting before |a|. Move it after |a|
  // if necessary.
  if (!a->hasFreeThings()) {
    cursorp_ = &a->next;
  }
  check();
}

void js::gc::ArenaList::insertBeforeCursor(Arena* a) {
  check();
  a->next = *cursorp_;
  *cursorp_ = a;
  cursorp_ = &a->next;
  check();
}

js::gc::ArenaList& js::gc::ArenaList::insertListWithCursorAtEnd(
    ArenaList& other) {
  check();
  other.check();
  MOZ_ASSERT(other.isCursorAtEnd());

  if (other.isEmpty()) {
    return *this;
  }

  // Insert the full arenas of |other| after those of |this|.
  *other.cursorp_ = *cursorp_;
  *cursorp_ = other.head_;
  cursorp_ = other.cursorp_;
  check();

  other.clear();
  return *this;
}

js::gc::Arena* js::gc::ArenaList::takeFirstArena() {
  check();
  Arena* arena = head_;
  if (!arena) {
    return nullptr;
  }

  head_ = arena->next;
  if (cursorp_ == &arena->next) {
    cursorp_ = &head_;
  }

  check();
  return arena;
}

js::gc::SortedArenaList::SortedArenaList(size_t thingsPerArena) {
  reset(thingsPerArena);
}

void js::gc::SortedArenaList::setThingsPerArena(size_t thingsPerArena) {
  MOZ_ASSERT(thingsPerArena && thingsPerArena <= MaxThingsPerArena);
  thingsPerArena_ = thingsPerArena;
}

void js::gc::SortedArenaList::reset(size_t thingsPerArena) {
  setThingsPerArena(thingsPerArena);
  // Initialize the segments.
  for (size_t i = 0; i <= thingsPerArena; ++i) {
    segments[i].clear();
  }
}

void js::gc::SortedArenaList::insertAt(Arena* arena, size_t nfree) {
  MOZ_ASSERT(nfree <= thingsPerArena_);
  segments[nfree].append(arena);
}

void js::gc::SortedArenaList::extractEmpty(Arena** empty) {
  SortedArenaListSegment& segment = segments[thingsPerArena_];
  if (segment.head) {
    *segment.tailp = *empty;
    *empty = segment.head;
    segment.clear();
  }
}

js::gc::ArenaList js::gc::SortedArenaList::toArenaList() {
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

#ifdef DEBUG

bool js::gc::FreeLists::allEmpty() const {
  for (auto i : AllAllocKinds()) {
    if (!isEmpty(i)) {
      return false;
    }
  }
  return true;
}

bool js::gc::FreeLists::isEmpty(AllocKind kind) const {
  return freeLists_[kind]->isEmpty();
}

#endif

void js::gc::FreeLists::clear() {
  for (auto i : AllAllocKinds()) {
#ifdef DEBUG
    auto old = freeLists_[i];
    if (!old->isEmpty()) {
      old->getArena()->checkNoMarkedFreeCells();
    }
#endif
    freeLists_[i] = &emptySentinel;
  }
}

js::gc::TenuredCell* js::gc::FreeLists::allocate(AllocKind kind) {
  return freeLists_[kind]->allocate(Arena::thingSize(kind));
}

void js::gc::FreeLists::unmarkPreMarkedFreeCells(AllocKind kind) {
  FreeSpan* freeSpan = freeLists_[kind];
  if (!freeSpan->isEmpty()) {
    freeSpan->getArena()->unmarkPreMarkedFreeCells();
  }
}

JSRuntime* js::gc::ArenaLists::runtime() {
  return zone_->runtimeFromMainThread();
}

JSRuntime* js::gc::ArenaLists::runtimeFromAnyThread() {
  return zone_->runtimeFromAnyThread();
}

js::gc::Arena* js::gc::ArenaLists::getFirstArena(AllocKind thingKind) const {
  return arenaList(thingKind).head();
}

js::gc::Arena* js::gc::ArenaLists::getFirstCollectingArena(
    AllocKind thingKind) const {
  return collectingArenaList(thingKind).head();
}

js::gc::Arena* js::gc::ArenaLists::getFirstSweptArena(
    AllocKind thingKind) const {
  if (thingKind != incrementalSweptArenaKind.ref()) {
    return nullptr;
  }
  return incrementalSweptArenas.ref().head();
}

js::gc::Arena* js::gc::ArenaLists::getArenaAfterCursor(
    AllocKind thingKind) const {
  return arenaList(thingKind).arenaAfterCursor();
}

bool js::gc::ArenaLists::arenaListsAreEmpty() const {
  for (auto i : AllAllocKinds()) {
    /*
     * The arena cannot be empty if the background finalization is not yet
     * done.
     */
    if (concurrentUse(i) == ConcurrentUse::BackgroundFinalize) {
      return false;
    }
    if (!arenaList(i).isEmpty()) {
      return false;
    }
  }
  return true;
}

bool js::gc::ArenaLists::doneBackgroundFinalize(AllocKind kind) const {
  return concurrentUse(kind) != ConcurrentUse::BackgroundFinalize;
}

bool js::gc::ArenaLists::needBackgroundFinalizeWait(AllocKind kind) const {
  return concurrentUse(kind) == ConcurrentUse::BackgroundFinalize;
}

void js::gc::ArenaLists::clearFreeLists() { freeLists().clear(); }

MOZ_ALWAYS_INLINE js::gc::TenuredCell* js::gc::ArenaLists::allocateFromFreeList(
    AllocKind thingKind) {
  return freeLists().allocate(thingKind);
}

void js::gc::ArenaLists::unmarkPreMarkedFreeCells() {
  for (auto i : AllAllocKinds()) {
    freeLists().unmarkPreMarkedFreeCells(i);
  }
}

void js::gc::ArenaLists::checkEmptyFreeLists() {
  MOZ_ASSERT(freeLists().allEmpty());
}

void js::gc::ArenaLists::checkEmptyArenaLists() {
#ifdef DEBUG
  for (auto i : AllAllocKinds()) {
    checkEmptyArenaList(i);
  }
#endif
}

#endif  // gc_ArenaList_inl_h
