/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Shape_inl_h
#define vm_Shape_inl_h

#include "vm/Shape.h"

#include "vm/JSObject.h"
#include "vm/PropertyResult.h"

#include "gc/GCContext-inl.h"
#include "gc/Marking-inl.h"
#include "vm/PropMap-inl.h"

namespace js {

template <class ObjectSubclass>
/* static */ inline bool SharedShape::ensureInitialCustomShape(
    JSContext* cx, Handle<ObjectSubclass*> obj) {
  static_assert(std::is_base_of_v<JSObject, ObjectSubclass>,
                "ObjectSubclass must be a subclass of JSObject");

  // If the provided object has a non-empty shape, it was given the cached
  // initial shape when created: nothing to do.
  if (!obj->empty()) {
    return true;
  }

  // Ensure the initial shape isn't collected under assignInitialShape, to
  // simplify insertInitialShape.
  Rooted<Shape*> emptyShape(cx, obj->shape());

  // If no initial shape was assigned, do so.
  Rooted<SharedShape*> shape(cx, ObjectSubclass::assignInitialShape(cx, obj));
  if (!shape) {
    return false;
  }
  MOZ_ASSERT(!obj->empty());

  // Cache the initial shape, so that future instances will begin life with that
  // shape.
  SharedShape::insertInitialShape(cx, shape);
  return true;
}

MOZ_ALWAYS_INLINE PropMap* NativeShape::lookup(JSContext* cx, PropertyKey key,
                                               uint32_t* index) {
  uint32_t len = propMapLength();
  return len > 0 ? propMap_->lookup(cx, len, key, index) : nullptr;
}

MOZ_ALWAYS_INLINE PropMap* NativeShape::lookupPure(PropertyKey key,
                                                   uint32_t* index) {
  uint32_t len = propMapLength();
  return len > 0 ? propMap_->lookupPure(len, key, index) : nullptr;
}

inline void Shape::purgeCache(JS::GCContext* gcx) {
  if (cache_.isShapeSetForAdd()) {
    gcx->delete_(this, cache_.toShapeSetForAdd(), MemoryUse::ShapeSetForAdd);
  }
  cache_.setNone();
}

inline void Shape::finalize(JS::GCContext* gcx) {
  if (!cache_.isNone()) {
    purgeCache(gcx);
  }
  if (isWasmGC()) {
    asWasmGC().finalize(gcx);
  }
}

inline void WasmGCShape::init() { recGroup_->AddRef(); }

inline void WasmGCShape::finalize(JS::GCContext* gcx) { recGroup_->Release(); }

inline SharedPropMap* SharedShape::propMapMaybeForwarded() const {
  MOZ_ASSERT(isShared());
  PropMap* propMap = propMap_;
  return propMap ? MaybeForwarded(propMap)->asShared() : nullptr;
}

static inline JS::PropertyAttributes GetPropertyAttributes(
    JSObject* obj, PropertyResult prop) {
  MOZ_ASSERT(obj->is<NativeObject>());

  if (prop.isDenseElement()) {
    return obj->as<NativeObject>().getElementsHeader()->elementAttributes();
  }
  if (prop.isTypedArrayElement()) {
    return {JS::PropertyAttribute::Configurable,
            JS::PropertyAttribute::Enumerable, JS::PropertyAttribute::Writable};
  }

  return prop.propertyInfo().propAttributes();
}

} /* namespace js */

#endif /* vm_Shape_inl_h */
