/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal helper functions for getting the AllocKind used to allocate a
 * JSObject and related information.
 */

#ifndef gc_ObjectKind_inl_h
#define gc_ObjectKind_inl_h

#include "util/Memory.h"
#include "vm/NativeObject.h"

namespace js {
namespace gc {

/* Capacity for slotsToThingKind */
const size_t SLOTS_TO_THING_KIND_LIMIT = 17;
extern const AllocKind slotsToThingKind[];
extern const uint32_t slotsToAllocKindBytes[];

/* Get the best kind to use when making an object with the given slot count. */
static inline AllocKind GetGCObjectKind(size_t numSlots) {
  if (numSlots >= SLOTS_TO_THING_KIND_LIMIT) {
    return AllocKind::OBJECT16;
  }
  return slotsToThingKind[numSlots];
}

static inline AllocKind GetGCObjectKind(const JSClass* clasp) {
  MOZ_ASSERT(!clasp->isProxyObject(),
             "Proxies should use GetProxyGCObjectKind");
  MOZ_ASSERT(!clasp->isJSFunction());

  uint32_t nslots = JSCLASS_RESERVED_SLOTS(clasp);
  return GetGCObjectKind(nslots);
}

static constexpr bool CanUseFixedElementsForArray(size_t numElements) {
  if (numElements > NativeObject::MAX_DENSE_ELEMENTS_COUNT) {
    return false;
  }
  size_t numSlots = numElements + ObjectElements::VALUES_PER_HEADER;
  return numSlots < SLOTS_TO_THING_KIND_LIMIT;
}

/* As for GetGCObjectKind, but for dense array allocation. */
static inline AllocKind GetGCArrayKind(size_t numElements) {
  /*
   * Dense arrays can use their fixed slots to hold their elements array
   * (less two Values worth of ObjectElements header), but if more than the
   * maximum number of fixed slots is needed then the fixed slots will be
   * unused.
   */
  static_assert(ObjectElements::VALUES_PER_HEADER == 2);
  if (!CanUseFixedElementsForArray(numElements)) {
    return AllocKind::OBJECT2;
  }
  return slotsToThingKind[numElements + ObjectElements::VALUES_PER_HEADER];
}

static inline AllocKind GetGCObjectFixedSlotsKind(size_t numFixedSlots) {
  MOZ_ASSERT(numFixedSlots < SLOTS_TO_THING_KIND_LIMIT);
  return slotsToThingKind[numFixedSlots];
}

// Get the best kind to use when allocating an object that needs a specific
// number of bytes.
static inline AllocKind GetGCObjectKindForBytes(size_t nbytes) {
  MOZ_ASSERT(nbytes <= JSObject::MAX_BYTE_SIZE);

  if (nbytes <= sizeof(NativeObject)) {
    return AllocKind::OBJECT0;
  }
  nbytes -= sizeof(NativeObject);

  size_t dataSlots = AlignBytes(nbytes, sizeof(Value)) / sizeof(Value);
  MOZ_ASSERT(nbytes <= dataSlots * sizeof(Value));
  return GetGCObjectKind(dataSlots);
}

/* Get the number of fixed slots and initial capacity associated with a kind. */
static constexpr inline size_t GetGCKindSlots(AllocKind thingKind) {
  // Using a switch in hopes that thingKind will usually be a compile-time
  // constant.
  switch (thingKind) {
    case AllocKind::OBJECT0:
    case AllocKind::OBJECT0_BACKGROUND:
      return 0;
    case AllocKind::OBJECT2:
    case AllocKind::OBJECT2_BACKGROUND:
      return 2;
    case AllocKind::FUNCTION:
    case AllocKind::OBJECT4:
    case AllocKind::OBJECT4_BACKGROUND:
      return 4;
    case AllocKind::FUNCTION_EXTENDED:
      return 7;
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

static inline size_t GetGCKindBytes(AllocKind thingKind) {
  return sizeof(JSObject_Slots0) + GetGCKindSlots(thingKind) * sizeof(Value);
}

static inline bool CanUseBackgroundAllocKind(const JSClass* clasp) {
  return !clasp->hasFinalize() || (clasp->flags & JSCLASS_BACKGROUND_FINALIZE);
}

static inline bool CanChangeToBackgroundAllocKind(AllocKind kind,
                                                  const JSClass* clasp) {
  // If a foreground alloc kind is specified but the class has no finalizer or a
  // finalizer that is safe to call on a different thread, we can change the
  // alloc kind to one which is finalized on a background thread.
  //
  // For example, AllocKind::OBJECT0 calls the finalizer on the main thread, and
  // AllocKind::OBJECT0_BACKGROUND calls the finalizer on the a helper thread.

  MOZ_ASSERT(IsObjectAllocKind(kind));

  if (IsBackgroundFinalized(kind)) {
    return false;  // This kind is already a background finalized kind.
  }

  return CanUseBackgroundAllocKind(clasp);
}

static inline AllocKind ForegroundToBackgroundAllocKind(AllocKind fgKind) {
  MOZ_ASSERT(IsObjectAllocKind(fgKind));
  MOZ_ASSERT(IsForegroundFinalized(fgKind));

  // For objects, each background alloc kind is defined just after the
  // corresponding foreground alloc kind so we can convert between them by
  // incrementing or decrementing as appropriate.
  AllocKind bgKind = AllocKind(size_t(fgKind) + 1);

  MOZ_ASSERT(IsObjectAllocKind(bgKind));
  MOZ_ASSERT(IsBackgroundFinalized(bgKind));
  MOZ_ASSERT(GetGCKindSlots(bgKind) == GetGCKindSlots(fgKind));

  return bgKind;
}

}  // namespace gc
}  // namespace js

#endif  // gc_ObjectKind_inl_h
