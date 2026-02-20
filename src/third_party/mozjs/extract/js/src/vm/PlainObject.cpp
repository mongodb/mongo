/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS object implementation.
 */

#include "vm/PlainObject-inl.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jspubtd.h"  // JSProto_Object

#include "ds/IdValuePair.h"  // js::IdValuePair
#include "gc/AllocKind.h"    // js::gc::AllocKind
#include "vm/JSContext.h"    // JSContext
#include "vm/JSFunction.h"   // JSFunction
#include "vm/JSObject.h"     // JSObject, js::GetPrototypeFromConstructor
#include "vm/TaggedProto.h"  // js::TaggedProto

#include "vm/JSFunction-inl.h"
#include "vm/JSObject-inl.h"  // js::NewObjectWithGroup, js::NewObjectGCKind

using namespace js;

using JS::Handle;
using JS::Rooted;

static MOZ_ALWAYS_INLINE SharedShape* GetPlainObjectShapeWithProto(
    JSContext* cx, JSObject* proto, gc::AllocKind kind) {
  MOZ_ASSERT(JSCLASS_RESERVED_SLOTS(&PlainObject::class_) == 0,
             "all slots can be used for properties");
  uint32_t nfixed = GetGCKindSlots(kind);
  return SharedShape::getInitialShape(cx, &PlainObject::class_, cx->realm(),
                                      TaggedProto(proto), nfixed);
}

SharedShape* js::ThisShapeForFunction(JSContext* cx, Handle<JSFunction*> callee,
                                      Handle<JSObject*> newTarget) {
  MOZ_ASSERT(cx->realm() == callee->realm());
  MOZ_ASSERT(!callee->constructorNeedsUninitializedThis());

  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromConstructor(cx, newTarget, JSProto_Object, &proto)) {
    return nullptr;
  }

  js::gc::AllocKind allocKind = NewObjectGCKind();
  if (!JSFunction::getAllocKindForThis(cx, callee, allocKind)) {
    return nullptr;
  }

  SharedShape* res;
  if (proto && proto != cx->global()->maybeGetPrototype(JSProto_Object)) {
    res = GetPlainObjectShapeWithProto(cx, proto, allocKind);
  } else {
    res = GlobalObject::getPlainObjectShapeWithDefaultProto(cx, allocKind);
  }

  MOZ_ASSERT_IF(res, res->realm() == callee->realm());

  return res;
}

#ifdef DEBUG
void PlainObject::assertHasNoNonWritableOrAccessorPropExclProto() const {
  // Check the most recent MaxCount properties to not slow down debug builds too
  // much.
  static constexpr size_t MaxCount = 8;

  size_t count = 0;
  PropertyName* protoName = runtimeFromMainThread()->commonNames->proto_;

  for (ShapePropertyIter<NoGC> iter(shape()); !iter.done(); iter++) {
    // __proto__ is always allowed.
    if (iter->key().isAtom(protoName)) {
      continue;
    }

    MOZ_ASSERT(iter->isDataProperty());
    MOZ_ASSERT(iter->writable());

    count++;
    if (count > MaxCount) {
      return;
    }
  }
}
#endif

// static
PlainObject* PlainObject::createWithTemplateFromDifferentRealm(
    JSContext* cx, Handle<PlainObject*> templateObject) {
  MOZ_ASSERT(cx->realm() != templateObject->realm(),
             "Use createWithTemplate() for same-realm objects");

  // Currently only implemented for null-proto.
  MOZ_ASSERT(templateObject->staticPrototype() == nullptr);

  // The object mustn't be in dictionary mode.
  MOZ_ASSERT(!templateObject->shape()->isDictionary());

  TaggedProto proto = TaggedProto(nullptr);
  SharedShape* templateShape = templateObject->sharedShape();
  Rooted<SharedPropMap*> map(cx, templateShape->propMap());

  Rooted<SharedShape*> shape(
      cx, SharedShape::getInitialOrPropMapShape(
              cx, &PlainObject::class_, cx->realm(), proto,
              templateShape->numFixedSlots(), map,
              templateShape->propMapLength(), templateShape->objectFlags()));
  if (!shape) {
    return nullptr;
  }
  return createWithShape(cx, shape);
}

// static
SharedShape* GlobalObject::createPlainObjectShapeWithDefaultProto(
    JSContext* cx, gc::AllocKind kind) {
  PlainObjectSlotsKind slotsKind = PlainObjectSlotsKindFromAllocKind(kind);
  GCPtr<SharedShape*>& shapeRef =
      cx->global()->data().plainObjectShapesWithDefaultProto[slotsKind];
  MOZ_ASSERT(!shapeRef);

  JSObject* proto = &cx->global()->getObjectPrototype();
  SharedShape* shape = GetPlainObjectShapeWithProto(cx, proto, kind);
  if (!shape) {
    return nullptr;
  }

  shapeRef.init(shape);
  return shape;
}

PlainObject* js::NewPlainObject(JSContext* cx, NewObjectKind newKind) {
  constexpr gc::AllocKind allocKind = gc::AllocKind::OBJECT0;
  MOZ_ASSERT(gc::GetGCObjectKind(&PlainObject::class_) == allocKind);

  Rooted<SharedShape*> shape(
      cx, GlobalObject::getPlainObjectShapeWithDefaultProto(cx, allocKind));
  if (!shape) {
    return nullptr;
  }

  return PlainObject::createWithShape(cx, shape, allocKind, newKind);
}

PlainObject* js::NewPlainObjectWithAllocKind(JSContext* cx,
                                             gc::AllocKind allocKind,
                                             NewObjectKind newKind) {
  Rooted<SharedShape*> shape(
      cx, GlobalObject::getPlainObjectShapeWithDefaultProto(cx, allocKind));
  if (!shape) {
    return nullptr;
  }

  return PlainObject::createWithShape(cx, shape, allocKind, newKind);
}

PlainObject* js::NewPlainObjectWithProto(JSContext* cx, HandleObject proto,
                                         NewObjectKind newKind) {
  // Use a faster path if |proto| is %Object.prototype% (the common case).
  if (proto && proto == cx->global()->maybeGetPrototype(JSProto_Object)) {
    return NewPlainObject(cx, newKind);
  }

  constexpr gc::AllocKind allocKind = gc::AllocKind::OBJECT0;
  MOZ_ASSERT(gc::GetGCObjectKind(&PlainObject::class_) == allocKind);

  Rooted<SharedShape*> shape(
      cx, GetPlainObjectShapeWithProto(cx, proto, allocKind));
  if (!shape) {
    return nullptr;
  }

  return PlainObject::createWithShape(cx, shape, allocKind, newKind);
}

PlainObject* js::NewPlainObjectWithProtoAndAllocKind(JSContext* cx,
                                                     HandleObject proto,
                                                     gc::AllocKind allocKind,
                                                     NewObjectKind newKind) {
  // Use a faster path if |proto| is %Object.prototype% (the common case).
  if (proto && proto == cx->global()->maybeGetPrototype(JSProto_Object)) {
    return NewPlainObjectWithAllocKind(cx, allocKind, newKind);
  }

  Rooted<SharedShape*> shape(
      cx, GetPlainObjectShapeWithProto(cx, proto, allocKind));
  if (!shape) {
    return nullptr;
  }

  return PlainObject::createWithShape(cx, shape, allocKind, newKind);
}

void js::NewPlainObjectWithPropsCache::add(SharedShape* shape) {
  MOZ_ASSERT(shape);
  MOZ_ASSERT(shape->slotSpan() > 0);
  for (size_t i = NumEntries - 1; i > 0; i--) {
    entries_[i] = entries_[i - 1];
  }
  entries_[0] = shape;
}

static bool ShapeMatches(Handle<IdValueVector> properties, SharedShape* shape) {
  if (shape->slotSpan() != properties.length()) {
    return false;
  }
  SharedShapePropertyIter<NoGC> iter(shape);
  for (size_t i = properties.length(); i > 0; i--) {
    MOZ_ASSERT(iter->isDataProperty());
    MOZ_ASSERT(iter->flags() == PropertyFlags::defaultDataPropFlags);
    if (properties[i - 1].get().id != iter->key()) {
      return false;
    }
    iter++;
  }
  MOZ_ASSERT(iter.done());
  return true;
}

SharedShape* js::NewPlainObjectWithPropsCache::lookup(
    Handle<IdValueVector> properties) const {
  for (size_t i = 0; i < NumEntries; i++) {
    SharedShape* shape = entries_[i];
    if (shape && ShapeMatches(properties, shape)) {
      return shape;
    }
  }
  return nullptr;
}

enum class KeysKind { UniqueNames, Unknown };

template <KeysKind Kind>
static PlainObject* NewPlainObjectWithProperties(
    JSContext* cx, Handle<IdValueVector> properties, NewObjectKind newKind) {
  auto& cache = cx->realm()->newPlainObjectWithPropsCache;

  // If we recently created an object with these properties, we can use that
  // Shape directly.
  if (SharedShape* shape = cache.lookup(properties)) {
    Rooted<SharedShape*> shapeRoot(cx, shape);
    PlainObject* obj = PlainObject::createWithShape(cx, shapeRoot, newKind);
    if (!obj) {
      return nullptr;
    }
    MOZ_ASSERT(obj->slotSpan() == properties.length());
    for (size_t i = 0; i < properties.length(); i++) {
      obj->initSlot(i, properties[i].get().value);
    }
    return obj;
  }

  gc::AllocKind allocKind = gc::GetGCObjectKind(properties.length());
  Rooted<PlainObject*> obj(cx,
                           NewPlainObjectWithAllocKind(cx, allocKind, newKind));
  if (!obj) {
    return nullptr;
  }

  if (properties.empty()) {
    return obj;
  }

  Rooted<PropertyKey> key(cx);
  Rooted<Value> value(cx);
  bool canCache = true;

  for (const auto& prop : properties) {
    key = prop.id;
    value = prop.value;

    // Integer keys may need to be stored in dense elements. This is uncommon so
    // just fall back to NativeDefineDataProperty.
    if constexpr (Kind == KeysKind::Unknown) {
      if (MOZ_UNLIKELY(key.isInt())) {
        canCache = false;
        if (!NativeDefineDataProperty(cx, obj, key, value, JSPROP_ENUMERATE)) {
          return nullptr;
        }
        continue;
      }
    }

    MOZ_ASSERT(key.isAtom() || key.isSymbol());

    // Check for duplicate keys. In this case we must overwrite the earlier
    // property value.
    if constexpr (Kind == KeysKind::UniqueNames) {
      MOZ_ASSERT(!obj->containsPure(key));
    } else {
      mozilla::Maybe<PropertyInfo> prop = obj->lookup(cx, key);
      if (MOZ_UNLIKELY(prop)) {
        canCache = false;
        MOZ_ASSERT(prop->isDataProperty());
        obj->setSlot(prop->slot(), value);
        continue;
      }
    }

    if (!AddDataPropertyToPlainObject(cx, obj, key, value)) {
      return nullptr;
    }
  }

  if (canCache && !obj->inDictionaryMode()) {
    MOZ_ASSERT(obj->getDenseInitializedLength() == 0);
    MOZ_ASSERT(obj->slotSpan() == properties.length());
    cache.add(obj->sharedShape());
  }

  return obj;
}

PlainObject* js::NewPlainObjectWithUniqueNames(JSContext* cx,
                                               Handle<IdValueVector> properties,
                                               NewObjectKind newKind) {
  return NewPlainObjectWithProperties<KeysKind::UniqueNames>(cx, properties,
                                                             newKind);
}

PlainObject* js::NewPlainObjectWithMaybeDuplicateKeys(
    JSContext* cx, Handle<IdValueVector> properties, NewObjectKind newKind) {
  return NewPlainObjectWithProperties<KeysKind::Unknown>(cx, properties,
                                                         newKind);
}
