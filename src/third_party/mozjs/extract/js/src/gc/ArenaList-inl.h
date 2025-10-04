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

inline js::gc::ArenaList::ArenaList(Arena* head, Arena* arenaBeforeCursor)
    : head_(head),
      cursorp_(arenaBeforeCursor ? &arenaBeforeCursor->next : &head_) {
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
  arena->next = nullptr;
  if (cursorp_ == &arena->next) {
    cursorp_ = &head_;
  }

  check();

  return arena;
}

js::gc::SortedArenaList::SortedArenaList(js::gc::AllocKind allocKind)
    : thingsPerArena_(Arena::thingsPerArena(allocKind)) {
#ifdef DEBUG
  MOZ_ASSERT(thingsPerArena_ <= MaxThingsPerArena);
  MOZ_ASSERT(emptyIndex() < BucketCount);
  allocKind_ = allocKind;
#endif
}

size_t js::gc::SortedArenaList::index(size_t nfree, bool* frontOut) const {
  // Get the bucket index to use for arenas with |nfree| free things and set
  // |frontOut| to indicate whether to prepend or append to the bucket.

  MOZ_ASSERT(nfree <= thingsPerArena_);

  // Full arenas go in the first bucket on their own.
  if (nfree == 0) {
    *frontOut = false;
    return 0;
  }

  // Empty arenas go in the last bucket on their own.
  if (nfree == thingsPerArena_) {
    *frontOut = false;
    return emptyIndex();
  }

  // All other arenas are alternately added to the front and back of successive
  // buckets as |nfree| increases.
  *frontOut = (nfree % 2) != 0;
  size_t index = (nfree + 1) / 2;
  MOZ_ASSERT(index != 0);
  MOZ_ASSERT(index != emptyIndex());
  return index;
}

size_t js::gc::SortedArenaList::emptyIndex() const {
  // Get the bucket index to use for empty arenas. This must have its own
  // bucket so they can be removed with extractEmptyTo.
  return bucketsUsed() - 1;
}

size_t js::gc::SortedArenaList::bucketsUsed() const {
  // Get the total number of buckets used for the current alloc kind.
  return HowMany(thingsPerArena_ - 1, 2) + 2;
}

void js::gc::SortedArenaList::insertAt(Arena* arena, size_t nfree) {
  MOZ_ASSERT(!isConvertedToArenaList);
  MOZ_ASSERT(nfree <= thingsPerArena_);

  bool front;
  size_t i = index(nfree, &front);
  MOZ_ASSERT(i < BucketCount);
  if (front) {
    buckets[i].pushFront(arena);
  } else {
    buckets[i].pushBack(arena);
  }
}

void js::gc::SortedArenaList::extractEmptyTo(Arena** destListHeadPtr) {
  MOZ_ASSERT(!isConvertedToArenaList);
  check();

  Bucket& bucket = buckets[emptyIndex()];
  if (!bucket.isEmpty()) {
    Arena* tail = *destListHeadPtr;
    Arena* bucketLast = bucket.last();
    *destListHeadPtr = bucket.release();
    bucketLast->next = tail;
  }

  MOZ_ASSERT(bucket.isEmpty());
}

js::gc::ArenaList js::gc::SortedArenaList::convertToArenaList(
    Arena* maybeBucketLastOut[BucketCount]) {
#ifdef DEBUG
  MOZ_ASSERT(!isConvertedToArenaList);
  isConvertedToArenaList = true;
  check();
#endif

  if (maybeBucketLastOut) {
    for (size_t i = 0; i < BucketCount; i++) {
      maybeBucketLastOut[i] = buckets[i].last();
    }
  }

  // The cursor of the returned ArenaList needs to be between the last full
  // arena and the first arena with space. Record that here.
  Arena* lastFullArena = buckets[0].last();

  Bucket result;
  for (size_t i = 0; i < bucketsUsed(); ++i) {
    result.append(std::move(buckets[i]));
  }

  return ArenaList(result.release(), lastFullArena);
}

void js::gc::SortedArenaList::restoreFromArenaList(
    ArenaList& list, Arena* bucketLast[BucketCount]) {
#ifdef DEBUG
  MOZ_ASSERT(isConvertedToArenaList);
  isConvertedToArenaList = false;
#endif

  // Group the ArenaList elements into SinglyLinkedList buckets, where the
  // boundaries between buckets are retrieved from |bucketLast|.

  Arena* remaining = list.head();
  list.clear();

  for (size_t i = 0; i < bucketsUsed(); i++) {
    MOZ_ASSERT(buckets[i].isEmpty());
    if (bucketLast[i]) {
      Arena* first = remaining;
      Arena* last = bucketLast[i];
      remaining = last->next;
      last->next = nullptr;
      new (&buckets[i]) Bucket(first, last);
    }
  }

  check();
}

void js::gc::SortedArenaList::check() const {
#ifdef DEBUG
  const auto& fullBucket = buckets[0];
  for (auto arena = fullBucket.iter(); !arena.done(); arena.next()) {
    MOZ_ASSERT(arena->getAllocKind() == allocKind());
    MOZ_ASSERT(!arena->hasFreeThings());
  }

  for (size_t i = 1; i < emptyIndex(); i++) {
    const auto& bucket = buckets[i];
    size_t lastFree = 0;
    for (auto arena = bucket.iter(); !arena.done(); arena.next()) {
      MOZ_ASSERT(arena->getAllocKind() == allocKind());
      size_t nfree = arena->countFreeCells();
      MOZ_ASSERT(nfree == i * 2 - 1 || nfree == i * 2);
      MOZ_ASSERT(nfree >= lastFree);
      lastFree = nfree;
    }
  }

  const auto& emptyBucket = buckets[emptyIndex()];
  for (auto arena = emptyBucket.iter(); !arena.done(); arena.next()) {
    MOZ_ASSERT(arena->getAllocKind() == allocKind());
    MOZ_ASSERT(arena->isEmpty());
  }

  for (size_t i = emptyIndex() + 1; i < BucketCount; i++) {
    MOZ_ASSERT(buckets[i].isEmpty());
  }
#endif
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
