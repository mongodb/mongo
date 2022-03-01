/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Queue-with-sizes operations. */

#ifndef builtin_streams_QueueWithSizes_h
#define builtin_streams_QueueWithSizes_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jstypes.h"        // JS_PUBLIC_API
#include "js/RootingAPI.h"  // JS::{,Mutable}Handle
#include "js/Value.h"       // JS::Value
#include "vm/List.h"        // js::ListObject

struct JS_PUBLIC_API JSContext;

namespace js {

class StreamController;

/**
 * Streams spec, 6.2.1. DequeueValue ( container ) nothrow
 */
[[nodiscard]] extern bool DequeueValue(
    JSContext* cx, JS::Handle<StreamController*> unwrappedContainer,
    JS::MutableHandle<JS::Value> chunk);

/**
 * Streams spec, 6.2.1. DequeueValue ( container ) nothrow
 * when the dequeued value is ignored.
 */
extern void DequeueValue(StreamController* unwrappedContainer, JSContext* cx);

/**
 * Streams spec, 6.2.2. EnqueueValueWithSize ( container, value, size ) throws
 */
[[nodiscard]] extern bool EnqueueValueWithSize(
    JSContext* cx, JS::Handle<StreamController*> unwrappedContainer,
    JS::Handle<JS::Value> value, JS::Handle<JS::Value> sizeVal);

/**
 * Streams spec, 6.2.4. ResetQueue ( container ) nothrow
 */
[[nodiscard]] extern bool ResetQueue(
    JSContext* cx, JS::Handle<StreamController*> unwrappedContainer);

inline bool QueueIsEmpty(ListObject* unwrappedQueue) {
  if (unwrappedQueue->isEmpty()) {
    return true;
  }

  MOZ_ASSERT((unwrappedQueue->length() % 2) == 0,
             "queue-with-sizes must consist of (value, size) element pairs and "
             "so must have even length");
  return false;
}

}  // namespace js

#endif  // builtin_streams_QueueWithSizes_h
