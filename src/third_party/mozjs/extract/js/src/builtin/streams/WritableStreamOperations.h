/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Writable stream abstract operations. */

#ifndef builtin_streams_WritableStreamOperations_h
#define builtin_streams_WritableStreamOperations_h

#include "jstypes.h"        // JS_PUBLIC_API
#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;

namespace js {

class PromiseObject;
class WritableStream;

extern JSObject* WritableStreamAbort(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream,
    JS::Handle<JS::Value> reason);

extern JSObject* WritableStreamClose(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream);

[[nodiscard]] extern PromiseObject* WritableStreamAddWriteRequest(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream);

[[nodiscard]] extern bool WritableStreamDealWithRejection(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream,
    JS::Handle<JS::Value> error);

[[nodiscard]] extern bool WritableStreamStartErroring(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream,
    JS::Handle<JS::Value> reason);

[[nodiscard]] extern bool WritableStreamFinishErroring(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream);

[[nodiscard]] extern bool WritableStreamFinishInFlightWrite(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream);

[[nodiscard]] extern bool WritableStreamFinishInFlightWriteWithError(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream,
    JS::Handle<JS::Value> error);

[[nodiscard]] extern bool WritableStreamFinishInFlightClose(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream);

[[nodiscard]] extern bool WritableStreamFinishInFlightCloseWithError(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream,
    JS::Handle<JS::Value> error);

extern bool WritableStreamCloseQueuedOrInFlight(
    const WritableStream* unwrappedStream);

extern void WritableStreamMarkCloseRequestInFlight(
    WritableStream* unwrappedStream);

extern void WritableStreamMarkFirstWriteRequestInFlight(
    WritableStream* unwrappedStream);

[[nodiscard]] extern bool WritableStreamRejectCloseAndClosedPromiseIfNeeded(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream);

[[nodiscard]] extern bool WritableStreamUpdateBackpressure(
    JSContext* cx, JS::Handle<WritableStream*> unwrappedStream,
    bool backpressure);

}  // namespace js

#endif  // builtin_streams_WritableStreamOperations_h
