/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Writable stream default controller abstract operations. */

#ifndef builtin_streams_WritableStreamDefaultControllerOperations_h
#define builtin_streams_WritableStreamDefaultControllerOperations_h

#include "jstypes.h"        // JS_PUBLIC_API
#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;

namespace js {

class WritableStream;
class WritableStreamDefaultController;

extern JSObject* WritableStreamControllerAbortSteps(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController,
    JS::Handle<JS::Value> reason);

[[nodiscard]] extern bool WritableStreamControllerErrorSteps(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController);

[[nodiscard]] extern bool WritableStreamControllerStartHandler(JSContext* cx,
                                                               unsigned argc,
                                                               JS::Value* vp);

[[nodiscard]] extern bool WritableStreamControllerStartFailedHandler(
    JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Characterizes the family of algorithms, (startAlgorithm, writeAlgorithm,
 * closeAlgorithm, abortAlgorithm), associated with a writable stream.
 *
 * See the comment on SetUpWritableStreamDefaultController().
 */
enum class SinkAlgorithms {
  Script,
  Transform,
};

[[nodiscard]] extern bool SetUpWritableStreamDefaultController(
    JSContext* cx, JS::Handle<WritableStream*> stream,
    SinkAlgorithms algorithms, JS::Handle<JS::Value> underlyingSink,
    JS::Handle<JS::Value> writeMethod, JS::Handle<JS::Value> closeMethod,
    JS::Handle<JS::Value> abortMethod, double highWaterMark,
    JS::Handle<JS::Value> size);

[[nodiscard]] extern bool
SetUpWritableStreamDefaultControllerFromUnderlyingSink(
    JSContext* cx, JS::Handle<WritableStream*> stream,
    JS::Handle<JS::Value> underlyingSink, double highWaterMark,
    JS::Handle<JS::Value> sizeAlgorithm);

extern void WritableStreamDefaultControllerClearAlgorithms(
    WritableStreamDefaultController* unwrappedController);

[[nodiscard]] extern bool WritableStreamDefaultControllerClose(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController);

[[nodiscard]] extern bool WritableStreamDefaultControllerGetChunkSize(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController,
    JS::Handle<JS::Value> chunk, JS::MutableHandle<JS::Value> returnValue);

extern double WritableStreamDefaultControllerGetDesiredSize(
    const WritableStreamDefaultController* controller);

[[nodiscard]] extern bool WritableStreamDefaultControllerWrite(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController,
    JS::Handle<JS::Value> chunk, JS::Handle<JS::Value> chunkSize);

[[nodiscard]] extern bool WritableStreamDefaultControllerErrorIfNeeded(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController,
    JS::Handle<JS::Value> error);

[[nodiscard]] extern bool WritableStreamDefaultControllerProcessClose(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController);

[[nodiscard]] extern bool WritableStreamDefaultControllerProcessWrite(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController,
    JS::Handle<JS::Value> chunk);

extern bool WritableStreamDefaultControllerGetBackpressure(
    const WritableStreamDefaultController* unwrappedController);

[[nodiscard]] extern bool WritableStreamDefaultControllerError(
    JSContext* cx,
    JS::Handle<WritableStreamDefaultController*> unwrappedController,
    JS::Handle<JS::Value> error);

}  // namespace js

#endif  // builtin_streams_WritableStreamDefaultControllerOperations_h
