/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JSON serialization and deserialization operations.
 */

#ifndef js_JSON_h
#define js_JSON_h

#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

using JSONWriteCallback = bool (*)(const char16_t* buf, uint32_t len,
                                   void* data);

/**
 * Performs the JSON.stringify operation, as specified by ECMAScript, except
 * writing stringified data by repeated calls of |callback|, with each such
 * call passed |data| as argument.
 */
extern JS_PUBLIC_API bool JS_Stringify(JSContext* cx,
                                       JS::MutableHandle<JS::Value> value,
                                       JS::Handle<JSObject*> replacer,
                                       JS::Handle<JS::Value> space,
                                       JSONWriteCallback callback, void* data);

namespace JS {

/**
 * An API akin to JS_Stringify but with the goal of not having observable
 * side-effects when the stringification is performed.  This means it does not
 * allow a replacer or a custom space and has the following constraints on its
 * input:
 *
 * 1) The input must be a plain object or array, not an abitrary value.
 * 2) Every value in the graph reached by the algorithm starting with this
 *    object must be one of the following: null, undefined, a string (NOT a
 *    string object!), a boolean, a finite number (i.e. no NaN or Infinity or
 *    -Infinity), a plain object with no accessor properties, or an Array with
 *    no holes.
 *
 * The actual behavior differs from JS_Stringify only in asserting the above and
 * NOT attempting to get the "toJSON" property from things, since that could
 * clearly have side-effects.
 */
extern JS_PUBLIC_API bool ToJSONMaybeSafely(JSContext* cx,
                                            JS::Handle<JSObject*> input,
                                            JSONWriteCallback callback,
                                            void* data);

} /* namespace JS */

/**
 * Performs the JSON.parse operation as specified by ECMAScript.
 */
extern JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, const char16_t* chars,
                                       uint32_t len,
                                       JS::MutableHandle<JS::Value> vp);

/**
 * Performs the JSON.parse operation as specified by ECMAScript.
 */
extern JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, JS::Handle<JSString*> str,
                                       JS::MutableHandle<JS::Value> vp);

/**
 * Performs the JSON.parse operation as specified by ECMAScript, using the
 * given |reviver| argument as the corresponding optional argument to that
 * function.
 */
extern JS_PUBLIC_API bool JS_ParseJSONWithReviver(
    JSContext* cx, const char16_t* chars, uint32_t len,
    JS::Handle<JS::Value> reviver, JS::MutableHandle<JS::Value> vp);

/**
 * Performs the JSON.parse operation as specified by ECMAScript, using the
 * given |reviver| argument as the corresponding optional argument to that
 * function.
 */
extern JS_PUBLIC_API bool JS_ParseJSONWithReviver(
    JSContext* cx, JS::Handle<JSString*> str, JS::Handle<JS::Value> reviver,
    JS::MutableHandle<JS::Value> vp);

#endif /* js_JSON_h */
