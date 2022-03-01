/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Stream teeing state. */

#include "builtin/streams/TeeState.h"

#include "builtin/streams/ReadableStream.h"  // js::ReadableStream
#include "js/Class.h"          // JSClass, JSCLASS_HAS_RESERVED_SLOTS
#include "js/RootingAPI.h"     // JS::Handle, JS::Rooted
#include "vm/JSContext.h"      // JSContext
#include "vm/PromiseObject.h"  // js::PromiseObject

#include "vm/JSObject-inl.h"  // js::NewBuiltinClassInstance

using js::ReadableStream;
using js::TeeState;

using JS::Handle;
using JS::Int32Value;
using JS::ObjectValue;
using JS::Rooted;

/* static */ TeeState* TeeState::create(
    JSContext* cx, Handle<ReadableStream*> unwrappedStream) {
  Rooted<TeeState*> state(cx, NewBuiltinClassInstance<TeeState>(cx));
  if (!state) {
    return nullptr;
  }

  Rooted<PromiseObject*> cancelPromise(
      cx, PromiseObject::createSkippingExecutor(cx));
  if (!cancelPromise) {
    return nullptr;
  }

  state->setFixedSlot(Slot_Flags, Int32Value(0));
  state->setFixedSlot(Slot_CancelPromise, ObjectValue(*cancelPromise));
  Rooted<JSObject*> wrappedStream(cx, unwrappedStream);
  if (!cx->compartment()->wrap(cx, &wrappedStream)) {
    return nullptr;
  }
  state->setFixedSlot(Slot_Stream, JS::ObjectValue(*wrappedStream));

  return state;
}

const JSClass TeeState::class_ = {"TeeState",
                                  JSCLASS_HAS_RESERVED_SLOTS(SlotCount)};
