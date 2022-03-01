/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* General readable stream abstract operations. */

#ifndef builtin_streams_ReadableStreamOperations_h
#define builtin_streams_ReadableStreamOperations_h

#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value

class JS_PUBLIC_API JSObject;

namespace js {

class PromiseObject;
class ReadableStream;
class ReadableStreamDefaultController;
class TeeState;
class WritableStream;

[[nodiscard]] extern PromiseObject* ReadableStreamTee_Pull(
    JSContext* cx, JS::Handle<TeeState*> unwrappedTeeState);

[[nodiscard]] extern JSObject* ReadableStreamTee_Cancel(
    JSContext* cx, JS::Handle<TeeState*> unwrappedTeeState,
    JS::Handle<ReadableStreamDefaultController*> unwrappedBranch,
    JS::Handle<JS::Value> reason);

[[nodiscard]] extern bool ReadableStreamTee(
    JSContext* cx, JS::Handle<ReadableStream*> unwrappedStream,
    bool cloneForBranch2, JS::MutableHandle<ReadableStream*> branch1Stream,
    JS::MutableHandle<ReadableStream*> branch2Stream);

[[nodiscard]] extern PromiseObject* ReadableStreamPipeTo(
    JSContext* cx, JS::Handle<ReadableStream*> unwrappedSource,
    JS::Handle<WritableStream*> unwrappedDest, bool preventClose,
    bool preventAbort, bool preventCancel, JS::Handle<JSObject*> signal);

}  // namespace js

#endif  // builtin_streams_ReadableStreamOperations_h
