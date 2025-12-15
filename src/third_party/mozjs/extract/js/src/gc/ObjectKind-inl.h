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

inline constexpr AllocKind slotsToThingKind[] = {
    // clang-format off
    /*  0 */ AllocKind::OBJECT0,  AllocKind::OBJECT2,  AllocKind::OBJECT2,  AllocKind::OBJECT4,
    /*  4 */ AllocKind::OBJECT4,  AllocKind::OBJECT8,  AllocKind::OBJECT8,  AllocKind::OBJECT8,
    /*  8 */ AllocKind::OBJECT8,  AllocKind::OBJECT12, AllocKind::OBJECT12, AllocKind::OBJECT12,
    /* 12 */ AllocKind::OBJECT12, AllocKind::OBJECT16, AllocKind::OBJECT16, AllocKind::OBJECT16,
    /* 16 */ AllocKind::OBJECT16
    // clang-format on
};

extern const uint32_t slotsToAllocKindBytes[];

static constexpr uint32_t MaxGCObjectFixedSlots = std::size(slotsToThingKind);

/* Get the best kind to use when making an object with the given slot count. */
static constexpr AllocKind GetGCObjectKind(size_t numSlots) {
  if (numSlots >= std::size(slotsToThingKind)) {
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
  return numSlots < std::size(slotsToThingKind);
}

/* As for GetGCObjectKind, but for dense array allocation. */
static constexpr AllocKind GetGCArrayKind(size_t numElements) {
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
  MOZ_ASSERT(numFixedSlots < std::size(slotsToThingKind));
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
static constexpr size_t GetGCKindSlots(AllocKind thingKind) {
  // Using a switch in hopes that thingKind will usually be a compile-time
  // constant.
  switch (thingKind) {
    case AllocKind::OBJECT0:
    case AllocKind::OBJECT0_FOREGROUND:
    case AllocKind::OBJECT0_BACKGROUND:
      return 0;
    case AllocKind::OBJECT2:
    case AllocKind::OBJECT2_FOREGROUND:
    case AllocKind::OBJECT2_BACKGROUND:
      return 2;
    case AllocKind::FUNCTION:
    case AllocKind::OBJECT4:
    case AllocKind::OBJECT4_FOREGROUND:
    case AllocKind::OBJECT4_BACKGROUND:
      return 4;
    case AllocKind::FUNCTION_EXTENDED:
      return 7;
    case AllocKind::OBJECT8:
    case AllocKind::OBJECT8_FOREGROUND:
    case AllocKind::OBJECT8_BACKGROUND:
      return 8;
    case AllocKind::OBJECT12:
    case AllocKind::OBJECT12_FOREGROUND:
    case AllocKind::OBJECT12_BACKGROUND:
      return 12;
    case AllocKind::OBJECT16:
    case AllocKind::OBJECT16_FOREGROUND:
    case AllocKind::OBJECT16_BACKGROUND:
      return 16;
    default:
      MOZ_CRASH("Bad object alloc kind");
  }
}

static inline size_t GetGCKindBytes(AllocKind thingKind) {
  return sizeof(JSObject_Slots0) + GetGCKindSlots(thingKind) * sizeof(Value);
}

static inline FinalizeKind GetObjectFinalizeKind(const JSClass* clasp) {
  if (!clasp->hasFinalize()) {
    MOZ_ASSERT((clasp->flags & JSCLASS_FOREGROUND_FINALIZE) == 0);
    MOZ_ASSERT((clasp->flags & JSCLASS_BACKGROUND_FINALIZE) == 0);
    return FinalizeKind::None;
  }

  if (clasp->flags & JSCLASS_BACKGROUND_FINALIZE) {
    MOZ_ASSERT((clasp->flags & JSCLASS_FOREGROUND_FINALIZE) == 0);
    return FinalizeKind::Background;
  }

  MOZ_ASSERT(clasp->flags & JSCLASS_FOREGROUND_FINALIZE);
  return FinalizeKind::Foreground;
}

static inline AllocKind GetFinalizedAllocKind(AllocKind kind,
                                              FinalizeKind finalizeKind) {
  MOZ_ASSERT(kind != AllocKind::FUNCTION &&
             kind != AllocKind::FUNCTION_EXTENDED);
  MOZ_ASSERT(IsObjectAllocKind(kind));
  MOZ_ASSERT(!IsFinalizedKind(kind));

  AllocKind newKind = AllocKind(size_t(kind) + size_t(finalizeKind));
  MOZ_ASSERT(IsObjectAllocKind(newKind));
  MOZ_ASSERT(GetGCKindSlots(newKind) == GetGCKindSlots(kind));
  MOZ_ASSERT_IF(finalizeKind == FinalizeKind::None, !IsFinalizedKind(newKind));
  MOZ_ASSERT_IF(finalizeKind == FinalizeKind::Foreground,
                IsForegroundFinalized(newKind));
  MOZ_ASSERT_IF(finalizeKind == FinalizeKind::Background,
                IsBackgroundFinalized(newKind));

  return newKind;
}

static inline AllocKind GetFinalizedAllocKindForClass(AllocKind kind,
                                                      const JSClass* clasp) {
  return GetFinalizedAllocKind(kind, GetObjectFinalizeKind(clasp));
}

}  // namespace gc
}  // namespace js

#endif  // gc_ObjectKind_inl_h
