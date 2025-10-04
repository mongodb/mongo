/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Window and WindowProxy.
 *
 * For silly obscure reasons embedders are better off not knowing, the web wants
 * every global object to exist as two linked components: a Window component
 * that stores global variables and appears in environment chains but can't be
 * directly referred to by any script, and a WindowProxy component that
 * intermediates access to its Window that *can* be directly referred to by
 * script.  (Thus the global |window| and |globalThis| properties, |this| in
 * global code, the value of |(function() { return this; })()| in non-strict
 * mode code, and similar values are WindowProxy objects, not Windows.)
 *
 * Maintaining an invariant of never exposing a Window to script requires
 * substituting in its WindowProxy in a variety of apparently arbitrary (but
 * actually *very* carefully and nervously selected) places throughout the
 * engine and indeed the universe.
 *
 * This header defines functions that let embeddings convert from a WindowProxy
 * to its Window and vice versa.
 *
 * If you're not embedding SpiderMonkey in a web browser, you can almost
 * certainly ignore this header.
 */

#ifndef js_friend_WindowProxy_h
#define js_friend_WindowProxy_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Class.h"       // JSCLASS_IS_GLOBAL
#include "js/Object.h"      // JS::GetClass
#include "js/RootingAPI.h"  // JS::Handle

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace js {

/**
 * Tell the JS engine which Class is used for WindowProxy objects. Used by the
 * functions below.
 */
extern JS_PUBLIC_API void SetWindowProxyClass(JSContext* cx,
                                              const JSClass* clasp);

/**
 * Associates a WindowProxy with a Window (global object). `windowProxy` must
 * have the Class set by SetWindowProxyClass.
 */
extern JS_PUBLIC_API void SetWindowProxy(JSContext* cx,
                                         JS::Handle<JSObject*> global,
                                         JS::Handle<JSObject*> windowProxy);

namespace detail {

extern JS_PUBLIC_API bool IsWindowSlow(JSObject* obj);

extern JS_PUBLIC_API JSObject* ToWindowProxyIfWindowSlow(JSObject* obj);

}  // namespace detail

/**
 * Returns true iff `obj` is a global object with an associated WindowProxy,
 * see SetWindowProxy.
 */
inline bool IsWindow(JSObject* obj) {
  if (JS::GetClass(obj)->flags & JSCLASS_IS_GLOBAL) {
    return detail::IsWindowSlow(obj);
  }
  return false;
}

/**
 * Returns true iff `obj` has the WindowProxy Class (see SetWindowProxyClass).
 */
extern JS_PUBLIC_API bool IsWindowProxy(JSObject* obj);

/**
 * If `obj` is a Window, get its associated WindowProxy (or a CCW or dead
 * wrapper if the page was navigated away from), else return `obj`. This
 * function is infallible and never returns nullptr.
 */
MOZ_ALWAYS_INLINE JSObject* ToWindowProxyIfWindow(JSObject* obj) {
  if (JS::GetClass(obj)->flags & JSCLASS_IS_GLOBAL) {
    return detail::ToWindowProxyIfWindowSlow(obj);
  }
  return obj;
}

/**
 * If `obj` is a WindowProxy, get its associated Window (the compartment's
 * global), else return `obj`. This function is infallible and never returns
 * nullptr.
 */
extern JS_PUBLIC_API JSObject* ToWindowIfWindowProxy(JSObject* obj);

}  // namespace js

#endif  // js_friend_WindowProxy_h
