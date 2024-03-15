/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=4 sw=2 et tw=80 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Nursery_inl_h
#define gc_Nursery_inl_h

#include "gc/Nursery.h"

#include "gc/RelocationOverlay.h"
#include "js/TracingAPI.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"

#include "vm/JSContext-inl.h"

inline JSRuntime* js::Nursery::runtime() const { return gc->rt; }

template <typename T>
bool js::Nursery::isInside(const SharedMem<T>& p) const {
  return isInside(p.unwrap(/*safe - used for value in comparison above*/));
}

MOZ_ALWAYS_INLINE /* static */ bool js::Nursery::getForwardedPointer(
    js::gc::Cell** ref) {
  js::gc::Cell* cell = (*ref);
  MOZ_ASSERT(IsInsideNursery(cell));
  if (!cell->isForwarded()) {
    return false;
  }
  const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(cell);
  *ref = overlay->forwardingAddress();
  return true;
}

inline void js::Nursery::maybeSetForwardingPointer(JSTracer* trc, void* oldData,
                                                   void* newData, bool direct) {
  if (trc->isTenuringTracer()) {
    setForwardingPointerWhileTenuring(oldData, newData, direct);
  }
}

inline void js::Nursery::setForwardingPointerWhileTenuring(void* oldData,
                                                           void* newData,
                                                           bool direct) {
  if (isInside(oldData)) {
    setForwardingPointer(oldData, newData, direct);
  }
}

inline void js::Nursery::setSlotsForwardingPointer(HeapSlot* oldSlots,
                                                   HeapSlot* newSlots,
                                                   uint32_t nslots) {
  // Slot arrays always have enough space for a forwarding pointer, since the
  // number of slots is never zero.
  MOZ_ASSERT(nslots > 0);
  setDirectForwardingPointer(oldSlots, newSlots);
}

inline void js::Nursery::setElementsForwardingPointer(ObjectElements* oldHeader,
                                                      ObjectElements* newHeader,
                                                      uint32_t capacity) {
  // Only use a direct forwarding pointer if there is enough space for one.
  setForwardingPointer(oldHeader->elements(), newHeader->elements(),
                       capacity > 0);
}

inline void js::Nursery::setForwardingPointer(void* oldData, void* newData,
                                              bool direct) {
  if (direct) {
    setDirectForwardingPointer(oldData, newData);
    return;
  }

  setIndirectForwardingPointer(oldData, newData);
}

inline void js::Nursery::setDirectForwardingPointer(void* oldData,
                                                    void* newData) {
  MOZ_ASSERT(isInside(oldData));
  MOZ_ASSERT(!isInside(newData));

  new (oldData) BufferRelocationOverlay{newData};
}

namespace js {

// The allocation methods below will not run the garbage collector. If the
// nursery cannot accomodate the allocation, the malloc heap will be used
// instead.

template <typename T>
static inline T* AllocateObjectBuffer(JSContext* cx, uint32_t count) {
  size_t nbytes = RoundUp(count * sizeof(T), sizeof(Value));
  auto* buffer =
      static_cast<T*>(cx->nursery().allocateBuffer(cx->zone(), nbytes));
  if (!buffer) {
    ReportOutOfMemory(cx);
  }
  return buffer;
}

template <typename T>
static inline T* AllocateObjectBuffer(JSContext* cx, JSObject* obj,
                                      uint32_t count) {
  MOZ_ASSERT(cx->isMainThreadContext());

  size_t nbytes = RoundUp(count * sizeof(T), sizeof(Value));
  auto* buffer =
      static_cast<T*>(cx->nursery().allocateBuffer(cx->zone(), obj, nbytes));
  if (!buffer) {
    ReportOutOfMemory(cx);
  }
  return buffer;
}

// If this returns null then the old buffer will be left alone.
template <typename T>
static inline T* ReallocateObjectBuffer(JSContext* cx, JSObject* obj,
                                        T* oldBuffer, uint32_t oldCount,
                                        uint32_t newCount) {
  MOZ_ASSERT(cx->isMainThreadContext());

  T* buffer = static_cast<T*>(cx->nursery().reallocateBuffer(
      obj->zone(), obj, oldBuffer, oldCount * sizeof(T), newCount * sizeof(T)));
  if (!buffer) {
    ReportOutOfMemory(cx);
  }

  return buffer;
}

static inline JS::BigInt::Digit* AllocateBigIntDigits(JSContext* cx,
                                                      JS::BigInt* bi,
                                                      uint32_t length) {
  MOZ_ASSERT(cx->isMainThreadContext());

  size_t nbytes = RoundUp(length * sizeof(JS::BigInt::Digit), sizeof(Value));
  auto* digits =
      static_cast<JS::BigInt::Digit*>(cx->nursery().allocateBuffer(bi, nbytes));
  if (!digits) {
    ReportOutOfMemory(cx);
  }

  return digits;
}

static inline JS::BigInt::Digit* ReallocateBigIntDigits(
    JSContext* cx, JS::BigInt* bi, JS::BigInt::Digit* oldDigits,
    uint32_t oldLength, uint32_t newLength) {
  MOZ_ASSERT(cx->isMainThreadContext());

  size_t oldBytes =
      RoundUp(oldLength * sizeof(JS::BigInt::Digit), sizeof(Value));
  size_t newBytes =
      RoundUp(newLength * sizeof(JS::BigInt::Digit), sizeof(Value));

  auto* digits = static_cast<JS::BigInt::Digit*>(cx->nursery().reallocateBuffer(
      bi->zone(), bi, oldDigits, oldBytes, newBytes));
  if (!digits) {
    ReportOutOfMemory(cx);
  }

  return digits;
}

}  // namespace js

#endif /* gc_Nursery_inl_h */
