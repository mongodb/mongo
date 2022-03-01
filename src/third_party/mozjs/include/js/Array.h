/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Array-related operations. */

#ifndef js_Array_h
#define js_Array_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

namespace JS {

class HandleValueArray;

/**
 * Create an Array from the current realm with the given contents.
 */
extern JS_PUBLIC_API JSObject* NewArrayObject(JSContext* cx,
                                              const HandleValueArray& contents);

/**
 * Create an Array from the current realm with the given length and allocate
 * memory for all its elements.  (The elements nonetheless will not exist as
 * properties on the returned array until values have been assigned to them.)
 */
extern JS_PUBLIC_API JSObject* NewArrayObject(JSContext* cx, size_t length);

/**
 * Determine whether |value| is an Array object or a wrapper around one.  (An
 * ES6 proxy whose target is an Array object, e.g.
 * |var target = [], handler = {}; Proxy.revocable(target, handler).proxy|, is
 * not considered to be an Array.)
 *
 * On success set |*isArray| accordingly and return true; on failure return
 * false.
 */
extern JS_PUBLIC_API bool IsArrayObject(JSContext* cx, Handle<Value> value,
                                        bool* isArray);

/**
 * Determine whether |obj| is an Array object or a wrapper around one.  (An
 * ES6 proxy whose target is an Array object, e.g.
 * |var target = [], handler = {}; Proxy.revocable(target, handler).proxy|, is
 * not considered to be an Array.)
 *
 * On success set |*isArray| accordingly and return true; on failure return
 * false.
 */
extern JS_PUBLIC_API bool IsArrayObject(JSContext* cx, Handle<JSObject*> obj,
                                        bool* isArray);

/**
 * Store |*lengthp = ToLength(obj.length)| and return true on success, else
 * return false.
 *
 * If the length does not fit in |uint32_t|, an exception is reported and false
 * is returned.
 *
 * |ToLength| converts its input to an integer usable to index an
 * array-like object.
 *
 * If |obj| is an Array, this overall operation is the same as getting
 * |obj.length|.
 */
extern JS_PUBLIC_API bool GetArrayLength(JSContext* cx, Handle<JSObject*> obj,
                                         uint32_t* lengthp);

/**
 * Perform |obj.length = length| as if in strict mode code, with a fast path for
 * the case where |obj| is an Array.
 *
 * This operation is exactly and only assigning to a "length" property.  In
 * general, it can invoke an existing "length" setter, throw if the property is
 * non-writable, or do anything else a property-set operation might do.
 */
extern JS_PUBLIC_API bool SetArrayLength(JSContext* cx, Handle<JSObject*> obj,
                                         uint32_t length);

/**
 * The answer to a successful query as to whether an object is an Array per
 * ES6's internal |IsArray| operation (as exposed by |Array.isArray|).
 */
enum class IsArrayAnswer { Array, NotArray, RevokedProxy };

/**
 * ES6 7.2.2.
 *
 * Returns false on failure, otherwise returns true and sets |*isArray|
 * indicating whether the object passes ECMAScript's IsArray test.  This is the
 * same test performed by |Array.isArray|.
 *
 * This is NOT the same as asking whether |obj| is an Array or a wrapper around
 * one.  If |obj| is a proxy created by |Proxy.revocable()| and has been
 * revoked, or if |obj| is a proxy whose target (at any number of hops) is a
 * revoked proxy, this method throws a TypeError and returns false.
 */
extern JS_PUBLIC_API bool IsArray(JSContext* cx, Handle<JSObject*> obj,
                                  bool* isArray);

/**
 * Identical to IsArray above, but the nature of the object (if successfully
 * determined) is communicated via |*answer|.  In particular this method
 * returns true and sets |*answer = IsArrayAnswer::RevokedProxy| when called on
 * a revoked proxy.
 *
 * Most users will want the overload above, not this one.
 */
extern JS_PUBLIC_API bool IsArray(JSContext* cx, Handle<JSObject*> obj,
                                  IsArrayAnswer* answer);

}  // namespace JS

#endif  // js_Array_h
