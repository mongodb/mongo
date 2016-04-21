/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Object_h
#define builtin_Object_h

#include "jsapi.h"

#include "vm/NativeObject.h"

namespace JS {
class CallArgs;
class Value;
} // namespace JS

namespace js {

// Object constructor native. Exposed only so the JIT can know its address.
bool
obj_construct(JSContext* cx, unsigned argc, JS::Value* vp);

bool
obj_propertyIsEnumerable(JSContext* cx, unsigned argc, Value* vp);

bool
obj_valueOf(JSContext* cx, unsigned argc, JS::Value* vp);

PlainObject*
ObjectCreateImpl(JSContext* cx, HandleObject proto, NewObjectKind newKind = GenericObject,
                 HandleObjectGroup group = nullptr);

PlainObject*
ObjectCreateWithTemplate(JSContext* cx, HandlePlainObject templateObj);

// Object methods exposed so they can be installed in the self-hosting global.
bool
obj_create(JSContext* cx, unsigned argc, JS::Value* vp);

bool
obj_defineProperty(JSContext* cx, unsigned argc, JS::Value* vp);

bool
obj_getOwnPropertyNames(JSContext* cx, unsigned argc, JS::Value* vp);

bool
obj_getOwnPropertyDescriptor(JSContext* cx, unsigned argc, JS::Value* vp);

bool
obj_getPrototypeOf(JSContext* cx, unsigned argc, JS::Value* vp);

bool
obj_hasOwnProperty(JSContext* cx, unsigned argc, JS::Value* vp);

bool
obj_isExtensible(JSContext* cx, unsigned argc, JS::Value* vp);

bool
obj_toString(JSContext* cx, unsigned argc, JS::Value* vp);

// Exposed so SelfHosting.cpp can use it in the OwnPropertyKeys intrinsic
bool
GetOwnPropertyKeys(JSContext* cx, const JS::CallArgs& args, unsigned flags);

/*
 * Like IdToValue, but convert int jsids to strings. This is used when
 * exposing a jsid to script for Object.getOwnProperty{Names,Symbols}
 * or scriptable proxy traps.
 */
bool
IdToStringOrSymbol(JSContext* cx, JS::HandleId id, JS::MutableHandleValue result);

#if JS_HAS_TOSOURCE
// Object.prototype.toSource. Function.prototype.toSource and uneval use this.
JSString*
ObjectToSource(JSContext* cx, JS::HandleObject obj);
#endif // JS_HAS_TOSOURCE

extern bool
WatchHandler(JSContext* cx, JSObject* obj, jsid id, JS::Value old,
             JS::Value* nvp, void* closure);

} /* namespace js */

#endif /* builtin_Object_h */
