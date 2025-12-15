/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/RawJSONObject.h"
#include "js/PropertyDescriptor.h"

#include "vm/JSObject-inl.h"

using namespace js;

const JSClass RawJSONObject::class_ = {
    "RawJSON",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount),
};

/* static */
RawJSONObject* RawJSONObject::create(JSContext* cx,
                                     Handle<JSString*> jsonString) {
  // https://tc39.es/proposal-json-parse-with-source/#sec-json.rawjson
  // Step 5
  Rooted<RawJSONObject*> obj(
      cx, NewObjectWithGivenProto<RawJSONObject>(cx, nullptr));
  if (!obj) {
    return nullptr;
  }
  // Step 6
  Rooted<PropertyKey> id(cx, NameToId(cx->names().rawJSON));
  Rooted<Value> jsonStringVal(cx, StringValue(jsonString));
  if (!NativeDefineDataProperty(cx, obj, id, jsonStringVal, JSPROP_ENUMERATE)) {
    return nullptr;
  }
  return obj;
}

JSString* RawJSONObject::rawJSON(JSContext* cx) {
  // RawJSONObjects are frozen on creation, so must always have a rawJSON string
  // property.
  PropertyKey id(NameToId(cx->names().rawJSON));
  JS::Value vp;
  MOZ_ALWAYS_TRUE(GetPropertyNoGC(cx, this, ObjectValue(*this), id, &vp));
  MOZ_ASSERT(vp.isString());
  return vp.toString();
}
