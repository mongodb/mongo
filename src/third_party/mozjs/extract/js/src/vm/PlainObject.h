/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PlainObject_h
#define vm_PlainObject_h

#include "gc/AllocKind.h"     // js::gc::AllocKind
#include "js/Class.h"         // JSClass
#include "js/RootingAPI.h"    // JS::Handle
#include "vm/JSObject.h"      // js::NewObjectKind
#include "vm/NativeObject.h"  // js::NativeObject

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSFunction;
class JS_PUBLIC_API JSObject;

namespace js {

struct IdValuePair;

// Object class for plain native objects created using '{}' object literals,
// 'new Object()', 'Object.create', etc.
class PlainObject : public NativeObject {
 public:
  static const JSClass class_;

 private:
#ifdef DEBUG
  void assertHasNoNonWritableOrAccessorPropExclProto() const;
#endif

 public:
  static inline js::PlainObject* createWithShape(JSContext* cx,
                                                 JS::Handle<SharedShape*> shape,
                                                 gc::AllocKind kind,
                                                 NewObjectKind newKind);

  static inline js::PlainObject* createWithShape(
      JSContext* cx, JS::Handle<SharedShape*> shape,
      NewObjectKind newKind = GenericObject);

  static inline PlainObject* createWithTemplate(
      JSContext* cx, JS::Handle<PlainObject*> templateObject);

  static js::PlainObject* createWithTemplateFromDifferentRealm(
      JSContext* cx, JS::Handle<PlainObject*> templateObject);

  /* Return the allocKind we would use if we were to tenure this object. */
  inline gc::AllocKind allocKindForTenure() const;

  bool hasNonWritableOrAccessorPropExclProto() const {
    if (hasFlag(ObjectFlag::HasNonWritableOrAccessorPropExclProto)) {
      return true;
    }
#ifdef DEBUG
    assertHasNoNonWritableOrAccessorPropExclProto();
#endif
    return false;
  }
};

// Specializations of 7.3.23 CopyDataProperties(...) for NativeObjects.
extern bool CopyDataPropertiesNative(JSContext* cx,
                                     JS::Handle<PlainObject*> target,
                                     JS::Handle<NativeObject*> from,
                                     JS::Handle<PlainObject*> excludedItems,
                                     bool* optimized);

// Specialized call to get the shape to use when creating |this| for a known
// function callee.
extern SharedShape* ThisShapeForFunction(JSContext* cx,
                                         JS::Handle<JSFunction*> callee,
                                         JS::Handle<JSObject*> newTarget);

// Create a new PlainObject with %Object.prototype% as prototype.
extern PlainObject* NewPlainObject(JSContext* cx,
                                   NewObjectKind newKind = GenericObject);

// Like NewPlainObject, but uses the given AllocKind. This allows creating an
// object with fixed slots available for properties.
extern PlainObject* NewPlainObjectWithAllocKind(
    JSContext* cx, gc::AllocKind allocKind,
    NewObjectKind newKind = GenericObject);

// Create a new PlainObject with the given |proto| as prototype.
extern PlainObject* NewPlainObjectWithProto(
    JSContext* cx, HandleObject proto, NewObjectKind newKind = GenericObject);

// Like NewPlainObjectWithProto, but uses the given AllocKind. This allows
// creating an object with fixed slots available for properties.
extern PlainObject* NewPlainObjectWithProtoAndAllocKind(
    JSContext* cx, HandleObject proto, gc::AllocKind allocKind,
    NewObjectKind newKind = GenericObject);

// Create a plain object with the given properties. The list must not contain
// duplicate keys or integer keys.
extern PlainObject* NewPlainObjectWithUniqueNames(JSContext* cx,
                                                  IdValuePair* properties,
                                                  size_t nproperties);

// Create a plain object with the given properties. The list may contain integer
// keys or duplicate keys.
extern PlainObject* NewPlainObjectWithMaybeDuplicateKeys(
    JSContext* cx, IdValuePair* properties, size_t nproperties);

}  // namespace js

#endif  // vm_PlainObject_h
