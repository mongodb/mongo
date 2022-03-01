/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Class WritableStream. */

#ifndef builtin_streams_WritableStream_inl_h
#define builtin_streams_WritableStream_inl_h

#include "builtin/streams/WritableStream.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jstypes.h"  // JS_PUBLIC_API

#include "builtin/streams/WritableStreamDefaultWriter.h"  // js::WritableStreamDefaultWriter
#include "js/RootingAPI.h"                                // JS::Handle
#include "js/Value.h"                                     // JS::{,Object}Value

#include "vm/Compartment-inl.h"  // js::UnwrapInternalSlot

struct JS_PUBLIC_API JSContext;

namespace js {

/**
 * Returns the writer associated with the given stream.
 *
 * Must only be called on WritableStreams that already have a writer
 * associated with them.
 *
 * If the writer is a wrapper, it will be unwrapped, so the result might not be
 * an object from the currently active compartment.
 */
[[nodiscard]] inline WritableStreamDefaultWriter* UnwrapWriterFromStream(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream) {
  MOZ_ASSERT(unwrappedStream->hasWriter());
  return UnwrapInternalSlot<WritableStreamDefaultWriter>(
      cx, unwrappedStream, WritableStream::Slot_Writer);
}

}  // namespace js

#endif  // builtin_streams_WritableStream_inl_h
