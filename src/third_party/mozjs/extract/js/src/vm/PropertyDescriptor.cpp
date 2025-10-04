/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/PropertyDescriptor.h"

#include "mozilla/Maybe.h"  // mozilla::Maybe

#include <stddef.h>  // size_t
#include <string.h>  // strlen

#include "jstypes.h"              // JS_PUBLIC_API
#include "js/Context.h"           // js::AssertHeapIsIdle
#include "js/Id.h"                // jsid
#include "js/RootingAPI.h"        // JS::Rooted, JS::Handle, JS::MutableHandle
#include "vm/JSAtomUtils.h"       // Atomize, AtomizeChars
#include "vm/JSContext.h"         // JSContext, CHECK_THREAD
#include "vm/JSObject.h"          // JSObject
#include "vm/ObjectOperations.h"  // GetOwnPropertyDescriptor
#include "vm/StringType.h"        // JSAtom

#include "vm/JSAtomUtils-inl.h"  // AtomToId
#include "vm/JSContext-inl.h"    // JSContext::check

using namespace js;

JS_PUBLIC_API bool JS_GetOwnPropertyDescriptorById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);
  return GetOwnPropertyDescriptor(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetOwnPropertyDescriptor(
    JSContext* cx, JS::Handle<JSObject*> obj, const char* name,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_GetOwnPropertyDescriptorById(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetOwnUCPropertyDescriptor(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) {
  JSAtom* atom = AtomizeChars(cx, name, namelen);
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_GetOwnPropertyDescriptorById(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetPropertyDescriptorById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc,
    JS::MutableHandle<JSObject*> holder) {
  cx->check(obj, id);
  return GetPropertyDescriptor(cx, obj, id, desc, holder);
}

JS_PUBLIC_API bool JS_GetPropertyDescriptor(
    JSContext* cx, JS::Handle<JSObject*> obj, const char* name,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc,
    JS::MutableHandle<JSObject*> holder) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_GetPropertyDescriptorById(cx, obj, id, desc, holder);
}

JS_PUBLIC_API bool JS_GetUCPropertyDescriptor(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc,
    JS::MutableHandle<JSObject*> holder) {
  JSAtom* atom = AtomizeChars(cx, name, namelen);
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_GetPropertyDescriptorById(cx, obj, id, desc, holder);
}
