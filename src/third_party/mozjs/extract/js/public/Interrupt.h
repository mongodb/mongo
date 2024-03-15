/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Interrupt_h
#define js_Interrupt_h

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

using JSInterruptCallback = bool (*)(JSContext*);

extern JS_PUBLIC_API bool JS_CheckForInterrupt(JSContext* cx);

/*
 * These functions allow setting an interrupt callback that will be called
 * from the JS thread some time after any thread triggered the callback using
 * JS_RequestInterruptCallback(cx).
 *
 * To schedule the GC and for other activities the engine internally triggers
 * interrupt callbacks. The embedding should thus not rely on callbacks being
 * triggered through the external API only.
 *
 * Important note: Additional callbacks can occur inside the callback handler
 * if it re-enters the JS engine. The embedding must ensure that the callback
 * is disconnected before attempting such re-entry.
 */
extern JS_PUBLIC_API bool JS_AddInterruptCallback(JSContext* cx,
                                                  JSInterruptCallback callback);

extern JS_PUBLIC_API bool JS_DisableInterruptCallback(JSContext* cx);

extern JS_PUBLIC_API void JS_ResetInterruptCallback(JSContext* cx, bool enable);

extern JS_PUBLIC_API void JS_RequestInterruptCallback(JSContext* cx);

extern JS_PUBLIC_API void JS_RequestInterruptCallbackCanWait(JSContext* cx);

#endif  // js_Interrupt_h
