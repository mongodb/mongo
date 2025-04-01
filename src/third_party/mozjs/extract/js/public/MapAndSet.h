/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Maps and Sets.
 */

#ifndef js_MapAndSet_h
#define js_MapAndSet_h

#include "jspubtd.h"

namespace JS {

/*
 * Map
 */
extern JS_PUBLIC_API JSObject* NewMapObject(JSContext* cx);

extern JS_PUBLIC_API uint32_t MapSize(JSContext* cx, HandleObject obj);

extern JS_PUBLIC_API bool MapGet(JSContext* cx, HandleObject obj,
                                 HandleValue key, MutableHandleValue rval);

extern JS_PUBLIC_API bool MapHas(JSContext* cx, HandleObject obj,
                                 HandleValue key, bool* rval);

extern JS_PUBLIC_API bool MapSet(JSContext* cx, HandleObject obj,
                                 HandleValue key, HandleValue val);

extern JS_PUBLIC_API bool MapDelete(JSContext* cx, HandleObject obj,
                                    HandleValue key, bool* rval);

extern JS_PUBLIC_API bool MapClear(JSContext* cx, HandleObject obj);

extern JS_PUBLIC_API bool MapKeys(JSContext* cx, HandleObject obj,
                                  MutableHandleValue rval);

extern JS_PUBLIC_API bool MapValues(JSContext* cx, HandleObject obj,
                                    MutableHandleValue rval);

extern JS_PUBLIC_API bool MapEntries(JSContext* cx, HandleObject obj,
                                     MutableHandleValue rval);

extern JS_PUBLIC_API bool MapForEach(JSContext* cx, HandleObject obj,
                                     HandleValue callbackFn,
                                     HandleValue thisVal);

/*
 * Set
 */
extern JS_PUBLIC_API JSObject* NewSetObject(JSContext* cx);

extern JS_PUBLIC_API uint32_t SetSize(JSContext* cx, HandleObject obj);

extern JS_PUBLIC_API bool SetHas(JSContext* cx, HandleObject obj,
                                 HandleValue key, bool* rval);

extern JS_PUBLIC_API bool SetDelete(JSContext* cx, HandleObject obj,
                                    HandleValue key, bool* rval);

extern JS_PUBLIC_API bool SetAdd(JSContext* cx, HandleObject obj,
                                 HandleValue key);

extern JS_PUBLIC_API bool SetClear(JSContext* cx, HandleObject obj);

extern JS_PUBLIC_API bool SetKeys(JSContext* cx, HandleObject obj,
                                  MutableHandleValue rval);

extern JS_PUBLIC_API bool SetValues(JSContext* cx, HandleObject obj,
                                    MutableHandleValue rval);

extern JS_PUBLIC_API bool SetEntries(JSContext* cx, HandleObject obj,
                                     MutableHandleValue rval);

extern JS_PUBLIC_API bool SetForEach(JSContext* cx, HandleObject obj,
                                     HandleValue callbackFn,
                                     HandleValue thisVal);

}  // namespace JS

#endif  // js_MapAndSet_h
