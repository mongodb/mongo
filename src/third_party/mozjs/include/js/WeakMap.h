/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Weak Maps.
 */

#ifndef js_WeakMap_h
#define js_WeakMap_h

#include "jspubtd.h"

namespace JS {

extern JS_PUBLIC_API JSObject* NewWeakMapObject(JSContext* cx);

extern JS_PUBLIC_API bool IsWeakMapObject(JSObject* obj);

extern JS_PUBLIC_API bool GetWeakMapEntry(JSContext* cx,
                                          JS::HandleObject mapObj,
                                          JS::HandleObject key,
                                          JS::MutableHandleValue val);

extern JS_PUBLIC_API bool SetWeakMapEntry(JSContext* cx,
                                          JS::HandleObject mapObj,
                                          JS::HandleObject key,
                                          JS::HandleValue val);

}  // namespace JS

#endif  // js_WeakMap_h
