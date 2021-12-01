/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_AsyncFunction_h
#define vm_AsyncFunction_h

#include "vm/JSContext.h"
#include "vm/JSObject.h"

namespace js {

JSFunction*
GetWrappedAsyncFunction(JSFunction* unwrapped);

JSFunction*
GetUnwrappedAsyncFunction(JSFunction* wrapped);

bool
IsWrappedAsyncFunction(JSFunction* fun);

JSObject*
WrapAsyncFunctionWithProto(JSContext* cx, HandleFunction unwrapped, HandleObject proto);

JSObject*
WrapAsyncFunction(JSContext* cx, HandleFunction unwrapped);

MOZ_MUST_USE bool
AsyncFunctionAwaitedFulfilled(JSContext* cx, Handle<PromiseObject*> resultPromise,
                              HandleValue generatorVal, HandleValue value);

MOZ_MUST_USE bool
AsyncFunctionAwaitedRejected(JSContext* cx, Handle<PromiseObject*> resultPromise,
                             HandleValue generatorVal, HandleValue reason);

} // namespace js

#endif /* vm_AsyncFunction_h */
