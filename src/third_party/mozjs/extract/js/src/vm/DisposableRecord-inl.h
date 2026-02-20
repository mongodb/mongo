/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_DisposableRecord_inl_h
#define vm_DisposableRecord_inl_h

#include "vm/DisposableRecord.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

inline /* static */ js::DisposableRecordObject*
js::DisposableRecordObject::create(JSContext* cx, JS::Handle<JS::Value> value,
                                   JS::Handle<JS::Value> method,
                                   UsingHint hint) {
  JS::Rooted<DisposableRecordObject*> obj(
      cx, NewObjectWithGivenProto<DisposableRecordObject>(cx, nullptr));
  if (!obj) {
    return nullptr;
  }

  obj->initReservedSlot(VALUE_SLOT, value);
  obj->initReservedSlot(METHOD_SLOT, method);
  obj->initReservedSlot(HINT_SLOT, JS::Int32Value(int32_t(hint)));

  if (!SharedShape::ensureInitialCustomShape<DisposableRecordObject>(cx, obj)) {
    return nullptr;
  }

  return obj;
}

#endif  // vm_DisposableRecord_inl_h
