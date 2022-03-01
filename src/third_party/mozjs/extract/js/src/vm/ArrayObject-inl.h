/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayObject_inl_h
#define vm_ArrayObject_inl_h

#include "vm/ArrayObject.h"

#include "gc/Allocator.h"
#include "gc/GCProbes.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"  // js::GetElement

namespace js {

/* static */ inline ArrayObject* ArrayObject::createArrayInternal(
    JSContext* cx, gc::AllocKind kind, gc::InitialHeap heap, HandleShape shape,
    AutoSetNewObjectMetadata&, gc::AllocSite* site) {
  const JSClass* clasp = shape->getObjectClass();
  MOZ_ASSERT(shape);
  MOZ_ASSERT(clasp == &ArrayObject::class_);
  MOZ_ASSERT(clasp->isNativeObject());
  MOZ_ASSERT_IF(clasp->hasFinalize(), heap == gc::TenuredHeap);

  // Arrays can use their fixed slots to store elements, so can't have shapes
  // which allow named properties to be stored in the fixed slots.
  MOZ_ASSERT(shape->numFixedSlots() == 0);

  size_t nDynamicSlots = calculateDynamicSlots(0, shape->slotSpan(), clasp);
  JSObject* obj =
      js::AllocateObject(cx, kind, nDynamicSlots, heap, clasp, site);
  if (!obj) {
    return nullptr;
  }

  ArrayObject* aobj = static_cast<ArrayObject*>(obj);
  aobj->initShape(shape);
  // NOTE: Dynamic slots are created internally by Allocate<JSObject>.
  if (!nDynamicSlots) {
    aobj->initEmptyDynamicSlots();
  }

  MOZ_ASSERT(clasp->shouldDelayMetadataBuilder());
  cx->realm()->setObjectPendingMetadata(cx, aobj);

  return aobj;
}

/* static */ inline ArrayObject* ArrayObject::finishCreateArray(
    ArrayObject* obj, HandleShape shape, AutoSetNewObjectMetadata& metadata) {
  size_t span = shape->slotSpan();
  if (span) {
    obj->initializeSlotRange(0, span);
  }

  gc::gcprobes::CreateObject(obj);

  return obj;
}

/* static */ inline ArrayObject* ArrayObject::createArray(
    JSContext* cx, gc::AllocKind kind, gc::InitialHeap heap, HandleShape shape,
    uint32_t length, AutoSetNewObjectMetadata& metadata, gc::AllocSite* site) {
  ArrayObject* obj = createArrayInternal(cx, kind, heap, shape, metadata, site);
  if (!obj) {
    return nullptr;
  }

  uint32_t capacity =
      gc::GetGCKindSlots(kind) - ObjectElements::VALUES_PER_HEADER;

  obj->setFixedElements();
  new (obj->getElementsHeader()) ObjectElements(capacity, length);

  return finishCreateArray(obj, shape, metadata);
}

}  // namespace js

#endif  // vm_ArrayObject_inl_h
