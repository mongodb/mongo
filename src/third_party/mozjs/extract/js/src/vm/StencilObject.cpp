/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/StencilObject.h"

#include "mozilla/Assertions.h"     // MOZ_ASSERT
#include "mozilla/PodOperations.h"  // mozilla::PodCopy

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, INT32_MAX

#include "jsapi.h"           // JS_NewObject
#include "js/Class.h"        // JSClassOps, JSClass, JSCLASS_*
#include "js/ErrorReport.h"  // JS_ReportErrorASCII
#include "js/experimental/JSStencil.h"  // JS::Stencil, JS::StencilAddRef, JS::StencilRelease
#include "js/RootingAPI.h"  // JS::Rooted
#include "js/Utility.h"     // js_free
#include "vm/JSContext.h"   // JSContext
#include "vm/JSObject.h"    // JSObject

using namespace js;

/*static */ const JSClassOps StencilObject::classOps_ = {
    nullptr,                  // addProperty
    nullptr,                  // delProperty
    nullptr,                  // enumerate
    nullptr,                  // newEnumerate
    nullptr,                  // resolve
    nullptr,                  // mayResolve
    StencilObject::finalize,  // finalize
    nullptr,                  // call
    nullptr,                  // construct
    nullptr,                  // trace
};

/*static */ const JSClass StencilObject::class_ = {
    "StencilObject",
    JSCLASS_HAS_RESERVED_SLOTS(StencilObject::ReservedSlots) |
        JSCLASS_BACKGROUND_FINALIZE,
    &StencilObject::classOps_,
};

bool StencilObject::hasStencil() const {
  // The stencil may not be present yet if we GC during initialization.
  return !getReservedSlot(StencilSlot).isUndefined();
}

JS::Stencil* StencilObject::stencil() const {
  void* ptr = getReservedSlot(StencilSlot).toPrivate();
  MOZ_ASSERT(ptr);
  return static_cast<JS::Stencil*>(ptr);
}

/* static */ StencilObject* StencilObject::create(JSContext* cx,
                                                  RefPtr<JS::Stencil> stencil) {
  JS::Rooted<JSObject*> obj(cx, JS_NewObject(cx, &class_));
  if (!obj) {
    return nullptr;
  }

  obj->as<StencilObject>().setReservedSlot(
      StencilSlot, PrivateValue(stencil.forget().take()));

  return &obj->as<StencilObject>();
}

/* static */ void StencilObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  if (obj->as<StencilObject>().hasStencil()) {
    JS::StencilRelease(obj->as<StencilObject>().stencil());
  }
}

/*static */ const JSClassOps StencilXDRBufferObject::classOps_ = {
    nullptr,                           // addProperty
    nullptr,                           // delProperty
    nullptr,                           // enumerate
    nullptr,                           // newEnumerate
    nullptr,                           // resolve
    nullptr,                           // mayResolve
    StencilXDRBufferObject::finalize,  // finalize
    nullptr,                           // call
    nullptr,                           // construct
    nullptr,                           // trace
};

/*static */ const JSClass StencilXDRBufferObject::class_ = {
    "StencilXDRBufferObject",
    JSCLASS_HAS_RESERVED_SLOTS(StencilXDRBufferObject::ReservedSlots) |
        JSCLASS_BACKGROUND_FINALIZE,
    &StencilXDRBufferObject::classOps_,
};

bool StencilXDRBufferObject::hasBuffer() const {
  // The stencil may not be present yet if we GC during initialization.
  return !getReservedSlot(BufferSlot).isUndefined();
}

const uint8_t* StencilXDRBufferObject::buffer() const {
  void* ptr = getReservedSlot(BufferSlot).toPrivate();
  MOZ_ASSERT(ptr);
  return static_cast<const uint8_t*>(ptr);
}

uint8_t* StencilXDRBufferObject::writableBuffer() {
  void* ptr = getReservedSlot(BufferSlot).toPrivate();
  MOZ_ASSERT(ptr);
  return static_cast<uint8_t*>(ptr);
}

size_t StencilXDRBufferObject::bufferLength() const {
  return getReservedSlot(LengthSlot).toInt32();
}

/* static */ StencilXDRBufferObject* StencilXDRBufferObject::create(
    JSContext* cx, uint8_t* buffer, size_t length) {
  if (length >= INT32_MAX) {
    JS_ReportErrorASCII(cx, "XDR buffer is too long");
    return nullptr;
  }

  JS::Rooted<JSObject*> obj(cx, JS_NewObject(cx, &class_));
  if (!obj) {
    return nullptr;
  }

  auto ownedBuffer = cx->make_pod_array<uint8_t>(length);
  if (!ownedBuffer) {
    return nullptr;
  }

  mozilla::PodCopy(ownedBuffer.get(), buffer, length);

  obj->as<StencilXDRBufferObject>().setReservedSlot(
      BufferSlot, PrivateValue(ownedBuffer.release()));
  obj->as<StencilXDRBufferObject>().setReservedSlot(LengthSlot,
                                                    Int32Value(length));

  return &obj->as<StencilXDRBufferObject>();
}

/* static */ void StencilXDRBufferObject::finalize(JS::GCContext* gcx,
                                                   JSObject* obj) {
  if (obj->as<StencilXDRBufferObject>().hasBuffer()) {
    js_free(obj->as<StencilXDRBufferObject>().writableBuffer());
  }
}
