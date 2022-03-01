/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Readable stream default controller abstract operations. */

#ifndef builtin_streams_ReadableStreamDefaultControllerOperations_h
#define builtin_streams_ReadableStreamDefaultControllerOperations_h

#include "jstypes.h"        // JS_PUBLIC_API
#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;

namespace js {

class ReadableStream;
class ReadableStreamController;
class ReadableStreamDefaultController;

[[nodiscard]] extern bool ReadableStreamDefaultControllerEnqueue(
    JSContext* cx,
    JS::Handle<ReadableStreamDefaultController*> unwrappedController,
    JS::Handle<JS::Value> chunk);

[[nodiscard]] extern bool ReadableStreamControllerError(
    JSContext* cx, JS::Handle<ReadableStreamController*> unwrappedController,
    JS::Handle<JS::Value> e);

[[nodiscard]] extern bool ReadableStreamDefaultControllerClose(
    JSContext* cx,
    JS::Handle<ReadableStreamDefaultController*> unwrappedController);

[[nodiscard]] extern double ReadableStreamControllerGetDesiredSizeUnchecked(
    ReadableStreamController* controller);

[[nodiscard]] extern bool ReadableStreamControllerCallPullIfNeeded(
    JSContext* cx, JS::Handle<ReadableStreamController*> unwrappedController);

extern void ReadableStreamControllerClearAlgorithms(
    JS::Handle<ReadableStreamController*> controller);

/**
 * Characterizes the family of algorithms, (startAlgorithm, pullAlgorithm,
 * cancelAlgorithm), associated with a readable stream.
 *
 * See the comment on SetUpReadableStreamDefaultController().
 */
enum class SourceAlgorithms {
  Script,
  Tee,
};

[[nodiscard]] extern bool SetUpReadableStreamDefaultController(
    JSContext* cx, JS::Handle<ReadableStream*> stream,
    SourceAlgorithms sourceAlgorithms, JS::Handle<JS::Value> underlyingSource,
    JS::Handle<JS::Value> pullMethod, JS::Handle<JS::Value> cancelMethod,
    double highWaterMark, JS::Handle<JS::Value> size);

[[nodiscard]] extern bool
SetUpReadableStreamDefaultControllerFromUnderlyingSource(
    JSContext* cx, JS::Handle<ReadableStream*> stream,
    JS::Handle<JS::Value> underlyingSource, double highWaterMark,
    JS::Handle<JS::Value> sizeAlgorithm);

}  // namespace js

#endif  // builtin_streams_ReadableStreamDefaultControllerOperations_h
