/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Stream_h
#define builtin_Stream_h

#include "jstypes.h"        // JS_PUBLIC_API
#include "js/RootingAPI.h"  // JS::Handle

struct JS_PUBLIC_API JSContext;

namespace js {

class PromiseObject;
class ReadableByteStreamController;
class ReadableStreamController;

[[nodiscard]] extern bool ReadableByteStreamControllerClearPendingPullIntos(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController);

[[nodiscard]] extern bool ReadableByteStreamControllerClose(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController);

[[nodiscard]] extern PromiseObject* ReadableStreamControllerPullSteps(
    JSContext* cx, JS::Handle<ReadableStreamController*> unwrappedController);

}  // namespace js

#endif /* builtin_Stream_h */
