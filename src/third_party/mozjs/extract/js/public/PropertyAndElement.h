/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Property and element API. */

#ifndef js_PropertyAndElement_h
#define js_PropertyAndElement_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CallArgs.h"    // JSNative
#include "js/GCVector.h"    // JS::GCVector
#include "js/Id.h"          // jsid
#include "js/RootingAPI.h"  // JS::Handle, JS::MutableHandle

struct JSContext;
class JSFunction;
class JSObject;
class JSString;

namespace JS {

class ObjectOpResult;
class JS_PUBLIC_API PropertyDescriptor;

using IdVector = JS::GCVector<jsid>;

} /* namespace JS */

/**
 * Define a property on obj.
 *
 * This function uses JS::ObjectOpResult to indicate conditions that ES6
 * specifies as non-error failures. This is inconvenient at best, so use this
 * function only if you are implementing a proxy handler's defineProperty()
 * method. For all other purposes, use one of the many DefineProperty functions
 * below that throw an exception in all failure cases.
 *
 * Implements: ES6 [[DefineOwnProperty]] internal method.
 */
extern JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JS::PropertyDescriptor> desc, JS::ObjectOpResult& result);

/**
 * Define a property on obj, throwing a TypeError if the attempt fails.
 * This is the C++ equivalent of `Object.defineProperty(obj, id, desc)`.
 */
extern JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                JS::Handle<JS::Value> value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JSNative getter, JSNative setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JSObject*> getter, JS::Handle<JSObject*> setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                JS::Handle<JSObject*> value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                JS::Handle<JSString*> value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                int32_t value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                uint32_t value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                double value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name,
                                            JS::Handle<JS::Value> value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name, JSNative getter,
                                            JSNative setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, const char* name,
    JS::Handle<JSObject*> getter, JS::Handle<JSObject*> setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name,
                                            JS::Handle<JSObject*> value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name,
                                            JS::Handle<JSString*> value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name, int32_t value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name, uint32_t value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name, double value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JS::Handle<JS::PropertyDescriptor> desc,
    JS::ObjectOpResult& result);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JS::Handle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JS::Handle<JS::Value> value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JS::Handle<JSObject*> getter, JS::Handle<JSObject*> setter,
    unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JS::Handle<JSObject*> value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JS::Handle<JSString*> value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx,
                                              JS::Handle<JSObject*> obj,
                                              const char16_t* name,
                                              size_t namelen, int32_t value,
                                              unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx,
                                              JS::Handle<JSObject*> obj,
                                              const char16_t* name,
                                              size_t namelen, uint32_t value,
                                              unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx,
                                              JS::Handle<JSObject*> obj,
                                              const char16_t* name,
                                              size_t namelen, double value,
                                              unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index,
                                           JS::Handle<JS::Value> value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(
    JSContext* cx, JS::Handle<JSObject*> obj, uint32_t index,
    JS::Handle<JSObject*> getter, JS::Handle<JSObject*> setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index,
                                           JS::Handle<JSObject*> value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index,
                                           JS::Handle<JSString*> value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index, int32_t value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index, uint32_t value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index, double value,
                                           unsigned attrs);

/**
 * Compute the expression `id in obj`.
 *
 * If obj has an own or inherited property obj[id], set *foundp = true and
 * return true. If not, set *foundp = false and return true. On error, return
 * false with an exception pending.
 *
 * Implements: ES6 [[Has]] internal method.
 */
extern JS_PUBLIC_API bool JS_HasPropertyById(JSContext* cx,
                                             JS::Handle<JSObject*> obj,
                                             JS::Handle<jsid> id, bool* foundp);

extern JS_PUBLIC_API bool JS_HasProperty(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         const char* name, bool* foundp);

extern JS_PUBLIC_API bool JS_HasUCProperty(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           const char16_t* name, size_t namelen,
                                           bool* vp);

extern JS_PUBLIC_API bool JS_HasElement(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        uint32_t index, bool* foundp);

/**
 * Determine whether obj has an own property with the key `id`.
 *
 * Implements: ES6 7.3.11 HasOwnProperty(O, P).
 */
extern JS_PUBLIC_API bool JS_HasOwnPropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                bool* foundp);

extern JS_PUBLIC_API bool JS_HasOwnProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name, bool* foundp);

/**
 * Get the value of the property `obj[id]`, or undefined if no such property
 * exists. This is the C++ equivalent of `vp = Reflect.get(obj, id, receiver)`.
 *
 * Most callers don't need the `receiver` argument. Consider using
 * JS_GetProperty instead. (But if you're implementing a proxy handler's set()
 * method, it's often correct to call this function and pass the receiver
 * through.)
 *
 * Implements: ES6 [[Get]] internal method.
 */
extern JS_PUBLIC_API bool JS_ForwardGetPropertyTo(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JS::Value> receiver, JS::MutableHandleValue vp);

extern JS_PUBLIC_API bool JS_ForwardGetElementTo(JSContext* cx,
                                                 JS::Handle<JSObject*> obj,
                                                 uint32_t index,
                                                 JS::Handle<JSObject*> receiver,
                                                 JS::MutableHandleValue vp);

/**
 * Get the value of the property `obj[id]`, or undefined if no such property
 * exists. The result is stored in vp.
 *
 * Implements: ES6 7.3.1 Get(O, P).
 */
extern JS_PUBLIC_API bool JS_GetPropertyById(JSContext* cx,
                                             JS::Handle<JSObject*> obj,
                                             JS::Handle<jsid> id,
                                             JS::MutableHandleValue vp);

extern JS_PUBLIC_API bool JS_GetProperty(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         const char* name,
                                         JS::MutableHandleValue vp);

extern JS_PUBLIC_API bool JS_GetUCProperty(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           const char16_t* name, size_t namelen,
                                           JS::MutableHandleValue vp);

extern JS_PUBLIC_API bool JS_GetElement(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        uint32_t index,
                                        JS::MutableHandleValue vp);

/**
 * Perform the same property assignment as `Reflect.set(obj, id, v, receiver)`.
 *
 * This function has a `receiver` argument that most callers don't need.
 * Consider using JS_SetProperty instead.
 *
 * Implements: ES6 [[Set]] internal method.
 */
extern JS_PUBLIC_API bool JS_ForwardSetPropertyTo(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JS::Value> v, JS::Handle<JS::Value> receiver,
    JS::ObjectOpResult& result);

/**
 * Perform the assignment `obj[id] = v`.
 *
 * This function performs non-strict assignment, so if the property is
 * read-only, nothing happens and no error is thrown.
 */
extern JS_PUBLIC_API bool JS_SetPropertyById(JSContext* cx,
                                             JS::Handle<JSObject*> obj,
                                             JS::Handle<jsid> id,
                                             JS::Handle<JS::Value> v);

extern JS_PUBLIC_API bool JS_SetProperty(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         const char* name,
                                         JS::Handle<JS::Value> v);

extern JS_PUBLIC_API bool JS_SetUCProperty(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           const char16_t* name, size_t namelen,
                                           JS::Handle<JS::Value> v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        uint32_t index,
                                        JS::Handle<JS::Value> v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        uint32_t index,
                                        JS::Handle<JSObject*> v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        uint32_t index,
                                        JS::Handle<JSString*> v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        uint32_t index, int32_t v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        uint32_t index, uint32_t v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        uint32_t index, double v);

/**
 * Delete a property. This is the C++ equivalent of
 * `result = Reflect.deleteProperty(obj, id)`.
 *
 * This function has a `result` out parameter that most callers don't need.
 * Unless you can pass through an ObjectOpResult provided by your caller, it's
 * probably best to use the JS_DeletePropertyById signature with just 3
 * arguments.
 *
 * Implements: ES6 [[Delete]] internal method.
 */
extern JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                JS::ObjectOpResult& result);

extern JS_PUBLIC_API bool JS_DeleteProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name,
                                            JS::ObjectOpResult& result);

extern JS_PUBLIC_API bool JS_DeleteUCProperty(JSContext* cx,
                                              JS::Handle<JSObject*> obj,
                                              const char16_t* name,
                                              size_t namelen,
                                              JS::ObjectOpResult& result);

extern JS_PUBLIC_API bool JS_DeleteElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index,
                                           JS::ObjectOpResult& result);

/**
 * Delete a property, ignoring strict failures. This is the C++ equivalent of
 * the JS `delete obj[id]` in non-strict mode code.
 */
extern JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id);

extern JS_PUBLIC_API bool JS_DeleteProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name);

extern JS_PUBLIC_API bool JS_DeleteElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index);

/**
 * Get an array of the non-symbol enumerable properties of obj.
 * This function is roughly equivalent to:
 *
 *     var result = [];
 *     for (key in obj) {
 *         result.push(key);
 *     }
 *     return result;
 *
 * This is the closest thing we currently have to the ES6 [[Enumerate]]
 * internal method.
 *
 * The array of ids returned by JS_Enumerate must be rooted to protect its
 * contents from garbage collection. Use JS::Rooted<JS::IdVector>.
 */
extern JS_PUBLIC_API bool JS_Enumerate(JSContext* cx, JS::Handle<JSObject*> obj,
                                       JS::MutableHandle<JS::IdVector> props);

/*** Other property-defining functions **************************************/

extern JS_PUBLIC_API JSObject* JS_DefineObject(JSContext* cx,
                                               JS::Handle<JSObject*> obj,
                                               const char* name,
                                               const JSClass* clasp = nullptr,
                                               unsigned attrs = 0);

extern JS_PUBLIC_API bool JS_DefineProperties(JSContext* cx,
                                              JS::Handle<JSObject*> obj,
                                              const JSPropertySpec* ps);

/* * */

extern JS_PUBLIC_API bool JS_AlreadyHasOwnPropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    bool* foundp);

extern JS_PUBLIC_API bool JS_AlreadyHasOwnProperty(JSContext* cx,
                                                   JS::Handle<JSObject*> obj,
                                                   const char* name,
                                                   bool* foundp);

extern JS_PUBLIC_API bool JS_AlreadyHasOwnUCProperty(JSContext* cx,
                                                     JS::Handle<JSObject*> obj,
                                                     const char16_t* name,
                                                     size_t namelen,
                                                     bool* foundp);

extern JS_PUBLIC_API bool JS_AlreadyHasOwnElement(JSContext* cx,
                                                  JS::Handle<JSObject*> obj,
                                                  uint32_t index, bool* foundp);

extern JS_PUBLIC_API bool JS_DefineFunctions(JSContext* cx,
                                             JS::Handle<JSObject*> obj,
                                             const JSFunctionSpec* fs);

extern JS_PUBLIC_API JSFunction* JS_DefineFunction(
    JSContext* cx, JS::Handle<JSObject*> obj, const char* name, JSNative call,
    unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API JSFunction* JS_DefineUCFunction(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JSNative call, unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API JSFunction* JS_DefineFunctionById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JSNative call, unsigned nargs, unsigned attrs);

#endif /* js_PropertyAndElement_h */
