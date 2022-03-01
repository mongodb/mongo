/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // uint8_t, uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/ArrayBufferMaybeShared.h"
#include "vm/ArrayBufferObject.h"  // js::ArrayBufferObject
#include "vm/JSObject.h"           // JSObject
#include "vm/SharedArrayObject.h"  // js::SharedArrayBufferObject
#include "vm/SharedMem.h"          // SharedMem

using namespace js;

JS_PUBLIC_API bool JS::IsArrayBufferObjectMaybeShared(JSObject* obj) {
  return obj->canUnwrapAs<ArrayBufferObjectMaybeShared>();
}

JS_PUBLIC_API JSObject* JS::UnwrapArrayBufferMaybeShared(JSObject* obj) {
  return obj->maybeUnwrapIf<ArrayBufferObjectMaybeShared>();
}

JS_PUBLIC_API void JS::GetArrayBufferMaybeSharedLengthAndData(
    JSObject* obj, size_t* length, bool* isSharedMemory, uint8_t** data) {
  MOZ_ASSERT(obj->is<ArrayBufferObjectMaybeShared>());

  if (obj->is<SharedArrayBufferObject>()) {
    auto* buffer = &obj->as<SharedArrayBufferObject>();
    *length = buffer->byteLength();
    *data = buffer->dataPointerShared().unwrap();
    *isSharedMemory = true;
  } else {
    auto* buffer = &obj->as<ArrayBufferObject>();
    *length = buffer->byteLength();
    *data = buffer->dataPointer();
    *isSharedMemory = false;
  }
}

JS_PUBLIC_API uint8_t* JS::GetArrayBufferMaybeSharedData(
    JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&) {
  MOZ_ASSERT(obj->maybeUnwrapIf<ArrayBufferObjectMaybeShared>());

  if (ArrayBufferObject* aobj = obj->maybeUnwrapIf<ArrayBufferObject>()) {
    *isSharedMemory = false;
    return aobj->dataPointer();
  } else if (SharedArrayBufferObject* saobj =
                 obj->maybeUnwrapIf<SharedArrayBufferObject>()) {
    *isSharedMemory = true;
    return saobj->dataPointerShared().unwrap();
  }

  return nullptr;
}

JS_PUBLIC_API bool JS::IsLargeArrayBufferMaybeShared(JSObject* obj) {
#ifdef JS_64BIT
  obj = UnwrapArrayBufferMaybeShared(obj);
  MOZ_ASSERT(obj);
  size_t len = obj->is<ArrayBufferObject>()
                   ? obj->as<ArrayBufferObject>().byteLength()
                   : obj->as<SharedArrayBufferObject>().byteLength();
  return len > ArrayBufferObject::MaxByteLengthForSmallBuffer;
#else
  // Large ArrayBuffers are not supported on 32-bit.
  MOZ_ASSERT(ArrayBufferObject::maxBufferByteLength() ==
             ArrayBufferObject::MaxByteLengthForSmallBuffer);
  return false;
#endif
}
