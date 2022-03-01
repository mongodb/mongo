/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Queue-with-sizes operations. */

#ifndef builtin_streams_QueueWithSizes_inl_h
#define builtin_streams_QueueWithSizes_inl_h

#include "builtin/streams/QueueWithSizes.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value
#include "vm/List.h"        // js::ListObject

#include "vm/List-inl.h"  // js::ListObject::*

struct JS_PUBLIC_API JSContext;

namespace js {

namespace detail {

// The *internal* representation of a queue-with-sizes is a List of even length
// where elements (2 * n, 2 * n + 1) represent the nth (value, size) element in
// the queue.

inline JS::Value QueueFirstValue(ListObject* unwrappedQueue) {
  MOZ_ASSERT(!unwrappedQueue->isEmpty(),
             "can't examine first value in an empty queue-with-sizes");
  MOZ_ASSERT((unwrappedQueue->length() % 2) == 0,
             "queue-with-sizes must consist of (value, size) element pairs and "
             "so must have even length");
  return unwrappedQueue->get(0);
}

inline double QueueFirstSize(ListObject* unwrappedQueue) {
  MOZ_ASSERT(!unwrappedQueue->isEmpty(),
             "can't examine first value in an empty queue-with-sizes");
  MOZ_ASSERT((unwrappedQueue->length() % 2) == 0,
             "queue-with-sizes must consist of (value, size) element pairs and "
             "so must have even length");
  return unwrappedQueue->get(1).toDouble();
}

inline void QueueRemoveFirstValueAndSize(ListObject* unwrappedQueue,
                                         JSContext* cx) {
  MOZ_ASSERT(!unwrappedQueue->isEmpty(),
             "can't remove first value from an empty queue-with-sizes");
  MOZ_ASSERT((unwrappedQueue->length() % 2) == 0,
             "queue-with-sizes must consist of (value, size) element pairs and "
             "so must have even length");
  unwrappedQueue->popFirstPair(cx);
}

[[nodiscard]] inline bool QueueAppendValueAndSize(
    JSContext* cx, JS::Handle<ListObject*> unwrappedQueue,
    JS::Handle<JS::Value> value, double size) {
  return unwrappedQueue->appendValueAndSize(cx, value, size);
}

}  // namespace detail

/**
 * Streams spec, 6.2.3. PeekQueueValue ( container ) nothrow
 */
inline JS::Value PeekQueueValue(ListObject* queue) {
  return detail::QueueFirstValue(queue);
}

}  // namespace js

#endif  // builtin_streams_QueueWithSizes_inl_h
