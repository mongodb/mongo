/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/ObjectWithStashedPointer.h"
#include "jsapi.h"         // JS_NewObject
#include "vm/JSContext.h"  // CHECK_THREAD

namespace JS::detail {

static const size_t DATA_SLOT = 0;
static const size_t FREE_FUNC_SLOT = 1;

static void ObjectWithUntypedStashedPointerFinalize(JS::GCContext*,
                                                    JSObject* obj) {
  AutoAssertNoGC nogc;

  void* freeFunc = GetMaybePtrFromReservedSlot<void>(obj, FREE_FUNC_SLOT);
  void* data = GetMaybePtrFromReservedSlot<void>(obj, DATA_SLOT);

  reinterpret_cast<UntypedFreeFunction>(freeFunc)(data);
}

static const JSClassOps classOps = {
    nullptr,  // addProperty
    nullptr,  // delProperty
    nullptr,  // enumerate
    nullptr,  // newEnumerate
    nullptr,  // resolve
    nullptr,  // mayResolve
    ObjectWithUntypedStashedPointerFinalize,
    nullptr,  // call
    nullptr,  // construct
    nullptr,  // trace
};

static const JSClass DataOnlyClass = {
    "Object",
    JSCLASS_HAS_RESERVED_SLOTS(1),
};

static const JSClass FreeFuncClass = {
    "Object",
    JSCLASS_HAS_RESERVED_SLOTS(2) | JSCLASS_FOREGROUND_FINALIZE,
    &classOps,
};

JSObject* NewObjectWithUntypedStashedPointer(JSContext* cx, void* ptr,
                                             UntypedFreeFunction freeFunc) {
  if (!freeFunc) {
    JSObject* retval = JS_NewObject(cx, &DataOnlyClass);
    if (!retval) {
      return nullptr;
    }
    JS::SetReservedSlot(retval, DATA_SLOT, JS::PrivateValue(ptr));
    return retval;
  }

  JSObject* retval = JS_NewObject(cx, &FreeFuncClass);
  if (!retval) {
    return nullptr;
  }
  JS::SetReservedSlot(retval, DATA_SLOT, JS::PrivateValue(ptr));
  JS::SetReservedSlot(retval, FREE_FUNC_SLOT,
                      JS::PrivateValue(reinterpret_cast<void*>(freeFunc)));
  return retval;
}

void* ObjectGetUntypedStashedPointer(JSContext* cx, JSObject* obj) {
  MOZ_ASSERT(
      obj->getClass() == &FreeFuncClass || obj->getClass() == &DataOnlyClass,
      "wrong type of object");
  return JS::GetMaybePtrFromReservedSlot<void>(obj, DATA_SLOT);
}

}  // namespace JS::detail
