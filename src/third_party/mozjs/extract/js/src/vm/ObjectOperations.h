/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Fundamental operations on objects. */

#ifndef vm_ObjectOperations_h
#define vm_ObjectOperations_h

#include "mozilla/Attributes.h"  // MOZ_ALWAYS_INLINE
#include "mozilla/Maybe.h"

#include <stdint.h>  // uint32_t

#include "js/Id.h"  // INT_TO_JSID, jsid, JSID_INT_MAX, SYMBOL_TO_JSID
#include "js/PropertyDescriptor.h"  // JSPROP_ENUMERATE, JS::PropertyDescriptor
#include "js/RootingAPI.h"          // JS::Handle, JS::MutableHandle, JS::Rooted
#include "js/TypeDecls.h"           // fwd-decl: JSContext, Symbol, Value
#include "vm/StringType.h"          // js::NameToId

namespace JS {
class ObjectOpResult;
}

namespace js {

class PropertyResult;

// The functions below are the fundamental operations on objects. See the
// comment about "Standard internal methods" in jsapi.h.

/*
 * ES6 [[GetPrototypeOf]]. Get obj's prototype, storing it in protop.
 *
 * If obj is definitely not a proxy, the infallible obj->getProto() can be used
 * instead. See the comment on JSObject::getTaggedProto().
 */
inline bool GetPrototype(JSContext* cx, JS::Handle<JSObject*> obj,
                         JS::MutableHandle<JSObject*> protop);

/*
 * ES6 [[SetPrototypeOf]]. Change obj's prototype to proto.
 *
 * Returns false on error, success of operation in *result. For example, if
 * obj is not extensible, its prototype is fixed. js::SetPrototype will return
 * true, because no exception is thrown for this; but *result will be false.
 */
extern bool SetPrototype(JSContext* cx, JS::Handle<JSObject*> obj,
                         JS::Handle<JSObject*> proto,
                         JS::ObjectOpResult& result);

/* Convenience function: like the above, but throw on failure. */
extern bool SetPrototype(JSContext* cx, JS::Handle<JSObject*> obj,
                         JS::Handle<JSObject*> proto);

/*
 * ES6 [[IsExtensible]]. Extensible objects can have new properties defined on
 * them. Inextensible objects can't, and their [[Prototype]] slot is fixed as
 * well.
 */
inline bool IsExtensible(JSContext* cx, JS::Handle<JSObject*> obj,
                         bool* extensible);

/*
 * ES6 [[PreventExtensions]]. Attempt to change the [[Extensible]] bit on |obj|
 * to false.  Indicate success or failure through the |result| outparam, or
 * actual error through the return value.
 */
extern bool PreventExtensions(JSContext* cx, JS::Handle<JSObject*> obj,
                              JS::ObjectOpResult& result);

/* Convenience function. As above, but throw on failure. */
extern bool PreventExtensions(JSContext* cx, JS::Handle<JSObject*> obj);

/*
 * ES6 [[GetOwnProperty]]. Get a description of one of obj's own properties.
 *
 * If no such property exists on obj, desc will be Nothing().
 */
extern bool GetOwnPropertyDescriptor(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc);

/* ES6 [[DefineOwnProperty]]. Define a property on obj. */
extern bool DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                           JS::Handle<jsid> id,
                           Handle<JS::PropertyDescriptor> desc,
                           JS::ObjectOpResult& result);

/*
 * When the 'result' out-param is omitted, the behavior is the same as above,
 * except that any failure results in a TypeError.
 */
extern bool DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                           JS::Handle<jsid> id,
                           JS::Handle<JS::PropertyDescriptor> desc);

extern bool DefineAccessorProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                   JS::Handle<jsid> id,
                                   JS::Handle<JSObject*> getter,
                                   JS::Handle<JSObject*> setter, unsigned attrs,
                                   JS::ObjectOpResult& result);

extern bool DefineDataProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                               JS::Handle<jsid> id, JS::Handle<JS::Value> value,
                               unsigned attrs, JS::ObjectOpResult& result);

extern bool DefineAccessorProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                   JS::Handle<jsid> id,
                                   JS::Handle<JSObject*> getter,
                                   JS::Handle<JSObject*> setter,
                                   unsigned attrs = JSPROP_ENUMERATE);

extern bool DefineDataProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                               JS::Handle<jsid> id, JS::Handle<JS::Value> value,
                               unsigned attrs = JSPROP_ENUMERATE);

extern bool DefineDataProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                               PropertyName* name, JS::Handle<JS::Value> value,
                               unsigned attrs = JSPROP_ENUMERATE);

extern bool DefineDataElement(JSContext* cx, JS::Handle<JSObject*> obj,
                              uint32_t index, JS::Handle<JS::Value> value,
                              unsigned attrs = JSPROP_ENUMERATE);

/*
 * ES6 [[Has]]. Set *foundp to true if `id in obj` (that is, if obj has an own
 * or inherited property obj[id]), false otherwise.
 */
inline bool HasProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<jsid> id, bool* foundp);

inline bool HasProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        PropertyName* name, bool* foundp);

/*
 * ES6 [[Get]]. Get the value of the property `obj[id]`, or undefined if no
 * such property exists.
 *
 * Typically obj == receiver; if obj != receiver then the caller is most likely
 * a proxy using GetProperty to finish a property get that started out as
 * `receiver[id]`, and we've already searched the prototype chain up to `obj`.
 */
inline bool GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<JS::Value> receiver, JS::Handle<jsid> id,
                        JS::MutableHandle<JS::Value> vp);

inline bool GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<JS::Value> receiver, PropertyName* name,
                        JS::MutableHandle<JS::Value> vp);

inline bool GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<JSObject*> receiver, JS::Handle<jsid> id,
                        JS::MutableHandle<JS::Value> vp);

inline bool GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<JSObject*> receiver, PropertyName* name,
                        JS::MutableHandle<JS::Value> vp);

inline bool GetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                       JS::Handle<JS::Value> receiver, uint32_t index,
                       JS::MutableHandle<JS::Value> vp);

inline bool GetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                       JS::Handle<JSObject*> receiver, uint32_t index,
                       JS::MutableHandle<JS::Value> vp);

inline bool GetPropertyNoGC(JSContext* cx, JSObject* obj,
                            const JS::Value& receiver, jsid id, JS::Value* vp);

inline bool GetPropertyNoGC(JSContext* cx, JSObject* obj,
                            const JS::Value& receiver, PropertyName* name,
                            JS::Value* vp);

inline bool GetElementNoGC(JSContext* cx, JSObject* obj,
                           const JS::Value& receiver, uint32_t index,
                           JS::Value* vp);

// Returns whether |obj| or an object on its proto chain may have an interesting
// symbol property (see JSObject::hasInterestingSymbolProperty). If it returns
// true, *holder is set to the object that may have this property.
MOZ_ALWAYS_INLINE bool MaybeHasInterestingSymbolProperty(
    JSContext* cx, JSObject* obj, JS::Symbol* symbol,
    JSObject** holder = nullptr);

// Like GetProperty but optimized for interesting symbol properties like
// @@toStringTag.
MOZ_ALWAYS_INLINE bool GetInterestingSymbolProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Symbol* sym,
    JS::MutableHandle<JS::Value> vp);

/*
 * ES6 [[Set]]. Carry out the assignment `obj[id] = v`.
 *
 * The `receiver` argument has to do with how [[Set]] interacts with the
 * prototype chain and proxies. It's hard to explain and ES6 doesn't really
 * try. Long story short, if you just want bog-standard assignment, pass
 * `ObjectValue(*obj)` as receiver. Or better, use one of the signatures that
 * doesn't have a receiver parameter.
 *
 * Callers pass obj != receiver e.g. when a proxy is involved, obj is the
 * proxy's target, and the proxy is using SetProperty to finish an assignment
 * that started out as `receiver[id] = v`, by delegating it to obj.
 */
inline bool SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<jsid> id, JS::Handle<JS::Value> v,
                        JS::Handle<JS::Value> receiver,
                        JS::ObjectOpResult& result);

inline bool SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<jsid> id, JS::Handle<JS::Value> v);

inline bool SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        PropertyName* name, JS::Handle<JS::Value> v,
                        JS::Handle<JS::Value> receiver,
                        JS::ObjectOpResult& result);

inline bool SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        PropertyName* name, JS::Handle<JS::Value> v);

inline bool SetElement(JSContext* cx, JS::Handle<JSObject*> obj, uint32_t index,
                       JS::Handle<JS::Value> v, JS::Handle<JS::Value> receiver,
                       JS::ObjectOpResult& result);

/*
 * ES6 draft rev 31 (15 Jan 2015) 7.3.3 Put (O, P, V, Throw), except that on
 * success, the spec says this is supposed to return a boolean value, which we
 * don't bother doing.
 */
inline bool PutProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<jsid> id, JS::Handle<JS::Value> v,
                        bool strict);

/*
 * ES6 [[Delete]]. Equivalent to the JS code `delete obj[id]`.
 */
inline bool DeleteProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                           JS::Handle<jsid> id, JS::ObjectOpResult& result);

inline bool DeleteElement(JSContext* cx, JS::Handle<JSObject*> obj,
                          uint32_t index, JS::ObjectOpResult& result);

/*** SpiderMonkey nonstandard internal methods ******************************/

/**
 * If |obj| (underneath any functionally-transparent wrapper proxies) has as
 * its [[GetPrototypeOf]] trap the ordinary [[GetPrototypeOf]] behavior defined
 * for ordinary objects, set |*isOrdinary = true| and store |obj|'s prototype
 * in |result|.  Otherwise set |*isOrdinary = false|.  In case of error, both
 * outparams have unspecified value.
 */
extern bool GetPrototypeIfOrdinary(JSContext* cx, JS::Handle<JSObject*> obj,
                                   bool* isOrdinary,
                                   JS::MutableHandle<JSObject*> protop);

/*
 * Attempt to make |obj|'s [[Prototype]] immutable, such that subsequently
 * trying to change it will not work.  If an internal error occurred,
 * returns false.  Otherwise, |*succeeded| is set to true iff |obj|'s
 * [[Prototype]] is now immutable.
 */
extern bool SetImmutablePrototype(JSContext* cx, JS::Handle<JSObject*> obj,
                                  bool* succeeded);

/*
 * Deprecated. Finds a PropertyDescriptor somewhere along the prototype chain,
 * similar to GetOwnPropertyDescriptor. |holder| indicates on which object the
 * property was found.
 */
extern bool GetPropertyDescriptor(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc,
    JS::MutableHandle<JSObject*> holder);

/*
 * Deprecated. A version of HasProperty that also returns the object on which
 * the property was found (but that information is unreliable for proxies), and
 * the Shape of the property, if native.
 */
extern bool LookupProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                           JS::Handle<jsid> id,
                           JS::MutableHandle<JSObject*> objp,
                           PropertyResult* propp);

inline bool LookupProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                           PropertyName* name,
                           JS::MutableHandle<JSObject*> objp,
                           PropertyResult* propp) {
  JS::Rooted<jsid> id(cx, NameToId(name));
  return LookupProperty(cx, obj, id, objp, propp);
}

/* Set *result to tell whether obj has an own property with the given id. */
extern bool HasOwnProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                           JS::Handle<jsid> id, bool* result);

} /* namespace js */

#endif /* vm_ObjectOperations_h */
