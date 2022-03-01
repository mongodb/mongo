/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Regular expression-related operations. */

#ifndef js_RegExp_h
#define js_RegExp_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RegExpFlags.h"  // JS::RegExpFlags
#include "js/TypeDecls.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSString;

namespace JS {

/**
 * Create a new RegExp for the given Latin-1-encoded bytes and flags.
 */
extern JS_PUBLIC_API JSObject* NewRegExpObject(JSContext* cx, const char* bytes,
                                               size_t length,
                                               RegExpFlags flags);

/**
 * Create a new RegExp for the given source and flags.
 */
extern JS_PUBLIC_API JSObject* NewUCRegExpObject(JSContext* cx,
                                                 const char16_t* chars,
                                                 size_t length,
                                                 RegExpFlags flags);

extern JS_PUBLIC_API bool SetRegExpInput(JSContext* cx, Handle<JSObject*> obj,
                                         Handle<JSString*> input);

extern JS_PUBLIC_API bool ClearRegExpStatics(JSContext* cx,
                                             Handle<JSObject*> obj);

extern JS_PUBLIC_API bool ExecuteRegExp(JSContext* cx, Handle<JSObject*> obj,
                                        Handle<JSObject*> reobj,
                                        const char16_t* chars, size_t length,
                                        size_t* indexp, bool test,
                                        MutableHandle<Value> rval);

/* RegExp interface for clients without a global object. */

extern JS_PUBLIC_API bool ExecuteRegExpNoStatics(
    JSContext* cx, Handle<JSObject*> reobj, const char16_t* chars,
    size_t length, size_t* indexp, bool test, MutableHandle<Value> rval);

/**
 * On success, returns true, setting |*isRegExp| to true if |obj| is a RegExp
 * object or a wrapper around one, or to false if not.  Returns false on
 * failure.
 *
 * This method returns true with |*isRegExp == false| when passed an ES6 proxy
 * whose target is a RegExp, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API bool ObjectIsRegExp(JSContext* cx, Handle<JSObject*> obj,
                                         bool* isRegExp);

/**
 * Given a RegExp object (or a wrapper around one), return the set of all
 * JS::RegExpFlag::* for it.
 */
extern JS_PUBLIC_API RegExpFlags GetRegExpFlags(JSContext* cx,
                                                Handle<JSObject*> obj);

/**
 * Return the source text for a RegExp object (or a wrapper around one), or null
 * on failure.
 */
extern JS_PUBLIC_API JSString* GetRegExpSource(JSContext* cx,
                                               Handle<JSObject*> obj);
/**
 * Check whether the given source is a valid regexp. If the regexp parses
 * successfully, returns true and sets |error| to undefined. If the regexp
 * has a syntax error, returns true, sets |error| to that error object, and
 * clears the exception. Returns false on OOM or over-recursion.
 */
extern JS_PUBLIC_API bool CheckRegExpSyntax(JSContext* cx,
                                            const char16_t* chars,
                                            size_t length, RegExpFlags flags,
                                            MutableHandle<Value> error);

}  // namespace JS

#endif  // js_RegExp_h
