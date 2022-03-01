/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PlainObject_h
#define vm_PlainObject_h

#include "gc/AllocKind.h"     // js::gc::AllocKind
#include "js/Class.h"         // JSClass
#include "js/Result.h"        // JS::OOM, JS::Result
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

  static inline JS::Result<PlainObject*, JS::OOM> createWithShape(
      JSContext* cx, JS::Handle<Shape*> shape);

 public:
  static inline JS::Result<PlainObject*, JS::OOM> createWithTemplate(
      JSContext* cx, JS::Handle<PlainObject*> templateObject);

  static JS::Result<PlainObject*, JS::OOM> createWithTemplateFromDifferentRealm(
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

// Specialized call for constructing |this| with a known function callee.
extern PlainObject* CreateThisForFunction(JSContext* cx,
                                          JS::Handle<JSFunction*> callee,
                                          JS::Handle<JSObject*> newTarget,
                                          NewObjectKind newKind);

extern PlainObject* NewPlainObjectWithProperties(JSContext* cx,
                                                 IdValuePair* properties,
                                                 size_t nproperties,
                                                 NewObjectKind newKind);

}  // namespace js

#endif  // vm_PlainObject_h
