/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_GlobalObject_h
#define js_GlobalObject_h

#include "mozilla/Attributes.h"

#include "jstypes.h"

#include "js/TypeDecls.h"

class JS_PUBLIC_API JSTracer;

struct JSClassOps;

extern JS_PUBLIC_API bool JS_IsGlobalObject(JSObject* obj);

namespace JS {

class JS_PUBLIC_API RealmOptions;

/**
 * Get the current realm's global. Returns nullptr if no realm has been
 * entered.
 */
extern JS_PUBLIC_API JSObject* CurrentGlobalOrNull(JSContext* cx);

/**
 * Get the global object associated with an object's realm. The object must not
 * be a cross-compartment wrapper (because CCWs are shared by all realms in the
 * compartment).
 */
extern JS_PUBLIC_API JSObject* GetNonCCWObjectGlobal(JSObject* obj);

/**
 * During global creation, we fire notifications to callbacks registered
 * via the Debugger API. These callbacks are arbitrary script, and can touch
 * the global in arbitrary ways. When that happens, the global should not be
 * in a half-baked state. But this creates a problem for consumers that need
 * to set slots on the global to put it in a consistent state.
 *
 * This API provides a way for consumers to set slots atomically (immediately
 * after the global is created), before any debugger hooks are fired. It's
 * unfortunately on the clunky side, but that's the way the cookie crumbles.
 *
 * If callers have no additional state on the global to set up, they may pass
 * |FireOnNewGlobalHook| to JS_NewGlobalObject, which causes that function to
 * fire the hook as its final act before returning. Otherwise, callers should
 * pass |DontFireOnNewGlobalHook|, which means that they are responsible for
 * invoking JS_FireOnNewGlobalObject upon successfully creating the global. If
 * an error occurs and the operation aborts, callers should skip firing the
 * hook. But otherwise, callers must take care to fire the hook exactly once
 * before compiling any script in the global's scope (we have assertions in
 * place to enforce this). This lets us be sure that debugger clients never miss
 * breakpoints.
 */
enum OnNewGlobalHookOption { FireOnNewGlobalHook, DontFireOnNewGlobalHook };

}  // namespace JS

extern JS_PUBLIC_API JSObject* JS_NewGlobalObject(
    JSContext* cx, const JSClass* clasp, JSPrincipals* principals,
    JS::OnNewGlobalHookOption hookOption, const JS::RealmOptions& options);
/**
 * Spidermonkey does not have a good way of keeping track of what compartments
 * should be marked on their own. We can mark the roots unconditionally, but
 * marking GC things only relevant in live compartments is hard. To mitigate
 * this, we create a static trace hook, installed on each global object, from
 * which we can be sure the compartment is relevant, and mark it.
 *
 * It is still possible to specify custom trace hooks for global object classes.
 * They can be provided via the RealmOptions passed to JS_NewGlobalObject.
 */
extern JS_PUBLIC_API void JS_GlobalObjectTraceHook(JSTracer* trc,
                                                   JSObject* global);

namespace JS {

/**
 * This allows easily constructing a global object without having to deal with
 * JSClassOps, forgetting to add JS_GlobalObjectTraceHook, or forgetting to call
 * JS::InitRealmStandardClasses(). Example:
 *
 *     const JSClass globalClass = { "MyGlobal", JSCLASS_GLOBAL_FLAGS,
 *         &JS::DefaultGlobalClassOps };
 *     JS_NewGlobalObject(cx, &globalClass, ...);
 */
extern JS_PUBLIC_DATA const JSClassOps DefaultGlobalClassOps;

}  // namespace JS

extern JS_PUBLIC_API void JS_FireOnNewGlobalObject(JSContext* cx,
                                                   JS::HandleObject global);

#endif  // js_GlobalObject_h
