/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PlainObject_inl_h
#define vm_PlainObject_inl_h

#include "vm/PlainObject.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_ASSERT_IF
#include "mozilla/Attributes.h"  // MOZ_ALWAYS_INLINE

#include "gc/Allocator.h"     // js::gc::InitialHeap
#include "js/RootingAPI.h"    // JS::Handle, JS::Rooted, JS::MutableHandle
#include "js/Value.h"         // JS::Value, JS_IS_CONSTRUCTING
#include "vm/JSFunction.h"    // JSFunction
#include "vm/JSObject.h"      // js::GenericObject, js::NewObjectKind
#include "vm/NativeObject.h"  // js::NativeObject::create
#include "vm/Shape.h"         // js::Shape

#include "gc/ObjectKind-inl.h"  // js::gc::GetGCObjectKind
#include "vm/JSObject-inl.h"  // js::GetInitialHeap, js::NewBuiltinClassInstance
#include "vm/NativeObject-inl.h"  // js::NativeObject::{create,setLastProperty}

/* static */ inline JS::Result<js::PlainObject*, JS::OOM>
js::PlainObject::createWithShape(JSContext* cx, JS::Handle<Shape*> shape) {
  MOZ_ASSERT(shape->getObjectClass() == &PlainObject::class_);
  gc::InitialHeap heap = GetInitialHeap(GenericObject, &PlainObject::class_);

  gc::AllocKind kind = gc::GetGCObjectKind(shape->numFixedSlots());
  MOZ_ASSERT(gc::CanChangeToBackgroundAllocKind(kind, shape->getObjectClass()));
  kind = gc::ForegroundToBackgroundAllocKind(kind);

  return NativeObject::create(cx, kind, heap, shape).map([](NativeObject* obj) {
    return &obj->as<PlainObject>();
  });
}

/* static */ inline JS::Result<js::PlainObject*, JS::OOM>
js::PlainObject::createWithTemplate(JSContext* cx,
                                    JS::Handle<PlainObject*> templateObject) {
  JS::Rooted<Shape*> shape(cx, templateObject->shape());
  return createWithShape(cx, shape);
}

inline js::gc::AllocKind js::PlainObject::allocKindForTenure() const {
  gc::AllocKind kind = gc::GetGCObjectFixedSlotsKind(numFixedSlots());
  MOZ_ASSERT(!gc::IsBackgroundFinalized(kind));
  MOZ_ASSERT(gc::CanChangeToBackgroundAllocKind(kind, getClass()));
  return gc::ForegroundToBackgroundAllocKind(kind);
}

namespace js {

// Create an object based on a template object created for either a NewObject
// bytecode op or for a constructor call.
static inline PlainObject* CopyTemplateObject(
    JSContext* cx, JS::Handle<PlainObject*> baseobj,
    NewObjectKind newKind = GenericObject) {
  MOZ_ASSERT(!baseobj->inDictionaryMode());

  gc::AllocKind allocKind =
      gc::GetGCObjectFixedSlotsKind(baseobj->numFixedSlots());
  allocKind = gc::ForegroundToBackgroundAllocKind(allocKind);
  MOZ_ASSERT_IF(baseobj->isTenured(),
                allocKind == baseobj->asTenured().getAllocKind());
  RootedObject proto(cx, baseobj->staticPrototype());
  JS::Rooted<PlainObject*> obj(cx, NewObjectWithGivenProtoAndKinds<PlainObject>(
                                       cx, proto, allocKind, newKind));
  if (!obj) {
    return nullptr;
  }

  if (!obj->setShapeAndUpdateSlots(cx, baseobj->shape())) {
    return nullptr;
  }

  return obj;
}

static MOZ_ALWAYS_INLINE bool CreateThis(JSContext* cx,
                                         JS::Handle<JSFunction*> callee,
                                         JS::Handle<JSObject*> newTarget,
                                         NewObjectKind newKind,
                                         JS::MutableHandle<JS::Value> thisv) {
  if (callee->constructorNeedsUninitializedThis()) {
    thisv.setMagic(JS_UNINITIALIZED_LEXICAL);
    return true;
  }

  MOZ_ASSERT(thisv.isMagic(JS_IS_CONSTRUCTING));

  PlainObject* obj = CreateThisForFunction(cx, callee, newTarget, newKind);
  if (!obj) {
    return false;
  }

  MOZ_ASSERT(obj->nonCCWRealm() == callee->realm());
  thisv.setObject(*obj);
  return true;
}

}  // namespace js

#endif  // vm_PlainObject_inl_h
