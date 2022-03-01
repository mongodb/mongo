/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ReadableStream pipe-to operation captured state. */

#ifndef builtin_streams_PipeToState_inl_h
#define builtin_streams_PipeToState_inl_h

#include "builtin/streams/PipeToState.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::Handle
#include "vm/JSContext.h"   // JSContext
#include "vm/Runtime.h"     // JSRuntime

#include "vm/Compartment-inl.h"  // js::UnwrapAndDowncastValue
#include "vm/JSContext-inl.h"    // JSContext::check

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace js {

/**
 * Returns the unwrapped |AbortSignal| instance associated with a given pipe-to
 * operation.
 *
 * The pipe-to operation must be known to have had an |AbortSignal| associated
 * with it.
 *
 * If the signal is a wrapper, it will be unwrapped, so the result might not be
 * an object from the currently active compartment.
 */
[[nodiscard]] inline JSObject* UnwrapSignalFromPipeToState(
    JSContext* cx, JS::Handle<PipeToState*> pipeToState) {
  cx->check(pipeToState);

  MOZ_ASSERT(pipeToState->hasSignal());
  return UnwrapAndDowncastValue(
      cx, pipeToState->getFixedSlot(PipeToState::Slot_Signal),
      cx->runtime()->maybeAbortSignalClass());
}

}  // namespace js

#endif  // builtin_streams_PipeToState_inl_h
