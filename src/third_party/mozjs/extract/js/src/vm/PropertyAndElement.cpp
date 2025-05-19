/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/PropertyAndElement.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "jsfriendapi.h"  // js::GetPropertyKeys, JSITER_OWNONLY
#include "jstypes.h"      // JS_PUBLIC_API

#include "js/CallArgs.h"            // JSNative
#include "js/Class.h"               // JS::ObjectOpResult
#include "js/Context.h"             // AssertHeapIsIdle
#include "js/GCVector.h"            // JS::GCVector, JS::RootedVector
#include "js/Id.h"                  // JS::PropertyKey, jsid
#include "js/PropertyDescriptor.h"  // JS::PropertyDescriptor, JSPROP_READONLY
#include "js/PropertySpec.h"        // JSNativeWrapper
#include "js/RootingAPI.h"          // JS::Rooted, JS::Handle, JS::MutableHandle
#include "js/Value.h"               // JS::Value, JS::*Value
#include "vm/FunctionPrefixKind.h"  // js::FunctionPrefixKind
#include "vm/GlobalObject.h"        // js::GlobalObject
#include "vm/JSAtomUtils.h"         // js::Atomize, js::AtomizeChars
#include "vm/JSContext.h"           // JSContext, CHECK_THREAD
#include "vm/JSFunction.h"          // js::IdToFunctionName, js::DefineFunction
#include "vm/JSObject.h"            // JSObject, js::DefineFunctions
#include "vm/ObjectOperations.h"  // js::DefineProperty, js::DefineDataProperty, js::HasOwnProperty
#include "vm/PropertyResult.h"  // js::PropertyResult
#include "vm/StringType.h"      // JSAtom, js::PropertyName

#include "vm/JSAtomUtils-inl.h"       // js::AtomToId, js::IndexToId
#include "vm/JSContext-inl.h"         // JSContext::check
#include "vm/JSObject-inl.h"          // js::NewBuiltinClassInstance
#include "vm/NativeObject-inl.h"      // js::NativeLookupOwnPropertyNoResolve
#include "vm/ObjectOperations-inl.h"  // js::GetProperty, js::GetElement, js::SetProperty, js::HasProperty, js::DeleteProperty, js::DeleteElement

using namespace js;

static bool DefinePropertyByDescriptor(JSContext* cx, JS::Handle<JSObject*> obj,
                                       JS::Handle<jsid> id,
                                       JS::Handle<JS::PropertyDescriptor> desc,
                                       JS::ObjectOpResult& result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, desc);
  return js::DefineProperty(cx, obj, id, desc, result);
}

JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JS::PropertyDescriptor> desc, JS::ObjectOpResult& result) {
  return ::DefinePropertyByDescriptor(cx, obj, id, desc, result);
}

JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JS::PropertyDescriptor> desc) {
  JS::ObjectOpResult result;
  return ::DefinePropertyByDescriptor(cx, obj, id, desc, result) &&
         result.checkStrict(cx, obj, id);
}

static bool DefineDataPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                   JS::Handle<jsid> id,
                                   JS::Handle<JS::Value> value,
                                   unsigned attrs) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, value);

  return js::DefineDataProperty(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::Handle<JS::Value> value,
                                         unsigned attrs) {
  return ::DefineDataPropertyById(cx, obj, id, value, attrs);
}

static bool DefineAccessorPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                       JS::Handle<jsid> id,
                                       JS::Handle<JSObject*> getter,
                                       JS::Handle<JSObject*> setter,
                                       unsigned attrs) {
  // JSPROP_READONLY has no meaning when accessors are involved. Ideally we'd
  // throw if this happens, but we've accepted it for long enough that it's
  // not worth trying to make callers change their ways. Just flip it off on
  // its way through the API layer so that we can enforce this internally.
  attrs &= ~JSPROP_READONLY;

  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, getter, setter);

  return js::DefineAccessorProperty(cx, obj, id, getter, setter, attrs);
}

static bool DefineAccessorPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                       JS::Handle<jsid> id,
                                       const JSNativeWrapper& get,
                                       const JSNativeWrapper& set,
                                       unsigned attrs) {
  // Getter/setter are both possibly-null JSNatives. Wrap them in JSFunctions.

  // Use unprefixed name with LAZY_ACCESSOR_NAME flag, to avoid calculating
  // the accessor name, which is less likely to be used.
  JS::Rooted<JSAtom*> atom(cx, IdToFunctionName(cx, id));
  if (!atom) {
    return false;
  }

  JS::Rooted<JSFunction*> getter(cx);
  if (get.op) {
    getter = NewNativeFunction(cx, get.op, 0, atom, gc::AllocKind::FUNCTION,
                               TenuredObject,
                               FunctionFlags::NATIVE_GETTER_WITH_LAZY_NAME);
    if (!getter) {
      return false;
    }

    if (get.info) {
      getter->setJitInfo(get.info);
    }
  }

  JS::Rooted<JSFunction*> setter(cx);
  if (set.op) {
    setter = NewNativeFunction(cx, set.op, 1, atom, gc::AllocKind::FUNCTION,
                               TenuredObject,
                               FunctionFlags::NATIVE_SETTER_WITH_LAZY_NAME);
    if (!setter) {
      return false;
    }

    if (set.info) {
      setter->setJitInfo(set.info);
    }
  }

  return ::DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

/*
 * Wrapper functions to create wrappers with no corresponding JSJitInfo from API
 * function arguments.
 */
static JSNativeWrapper NativeOpWrapper(Native native) {
  JSNativeWrapper ret;
  ret.op = native;
  ret.info = nullptr;
  return ret;
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id, JSNative getter,
                                         JSNative setter, unsigned attrs) {
  return ::DefineAccessorPropertyById(cx, obj, id, ::NativeOpWrapper(getter),
                                      ::NativeOpWrapper(setter), attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::Handle<JSObject*> getter,
                                         JS::Handle<JSObject*> setter,
                                         unsigned attrs) {
  return ::DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::Handle<JSObject*> valueArg,
                                         unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*valueArg));
  return ::DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         HandleString valueArg,
                                         unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::StringValue(valueArg));
  return ::DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id, int32_t valueArg,
                                         unsigned attrs) {
  JS::Value value = JS::Int32Value(valueArg);
  return ::DefineDataPropertyById(
      cx, obj, id, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id, uint32_t valueArg,
                                         unsigned attrs) {
  JS::Value value = JS::NumberValue(valueArg);
  return ::DefineDataPropertyById(
      cx, obj, id, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id, double valueArg,
                                         unsigned attrs) {
  JS::Value value = JS::NumberValue(valueArg);
  return ::DefineDataPropertyById(
      cx, obj, id, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

static bool DefineDataProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                               const char* name, JS::Handle<JS::Value> value,
                               unsigned attrs) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));

  return ::DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name,
                                     JS::Handle<JS::Value> value,
                                     unsigned attrs) {
  return ::DefineDataProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, JSNative getter,
                                     JSNative setter, unsigned attrs) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return ::DefineAccessorPropertyById(cx, obj, id, ::NativeOpWrapper(getter),
                                      ::NativeOpWrapper(setter), attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name,
                                     JS::Handle<JSObject*> getter,
                                     JS::Handle<JSObject*> setter,
                                     unsigned attrs) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));

  return ::DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name,
                                     JS::Handle<JSObject*> valueArg,
                                     unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*valueArg));
  return ::DefineDataProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, HandleString valueArg,
                                     unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::StringValue(valueArg));
  return ::DefineDataProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, int32_t valueArg,
                                     unsigned attrs) {
  JS::Value value = JS::Int32Value(valueArg);
  return ::DefineDataProperty(
      cx, obj, name, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, uint32_t valueArg,
                                     unsigned attrs) {
  JS::Value value = JS::NumberValue(valueArg);
  return ::DefineDataProperty(
      cx, obj, name, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, double valueArg,
                                     unsigned attrs) {
  JS::Value value = JS::NumberValue(valueArg);
  return ::DefineDataProperty(
      cx, obj, name, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

#define AUTO_NAMELEN(s, n) (((n) == (size_t)-1) ? js_strlen(s) : (n))

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       JS::Handle<JS::PropertyDescriptor> desc,
                                       JS::ObjectOpResult& result) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return ::DefinePropertyByDescriptor(cx, obj, id, desc, result);
}

JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JS::Handle<JS::PropertyDescriptor> desc) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  JS::ObjectOpResult result;
  return ::DefinePropertyByDescriptor(cx, obj, id, desc, result) &&
         result.checkStrict(cx, obj, id);
}

static bool DefineUCDataProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                 const char16_t* name, size_t namelen,
                                 JS::Handle<JS::Value> value, unsigned attrs) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return ::DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       JS::Handle<JS::Value> value,
                                       unsigned attrs) {
  return ::DefineUCDataProperty(cx, obj, name, namelen, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       JS::Handle<JSObject*> getter,
                                       JS::Handle<JSObject*> setter,
                                       unsigned attrs) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return ::DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       JS::Handle<JSObject*> valueArg,
                                       unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*valueArg));
  return ::DefineUCDataProperty(cx, obj, name, namelen, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       HandleString valueArg, unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::StringValue(valueArg));
  return ::DefineUCDataProperty(cx, obj, name, namelen, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       int32_t valueArg, unsigned attrs) {
  JS::Value value = JS::Int32Value(valueArg);
  return ::DefineUCDataProperty(
      cx, obj, name, namelen, JS::Handle<JS::Value>::fromMarkedLocation(&value),
      attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       uint32_t valueArg, unsigned attrs) {
  JS::Value value = JS::NumberValue(valueArg);
  return ::DefineUCDataProperty(
      cx, obj, name, namelen, JS::Handle<JS::Value>::fromMarkedLocation(&value),
      attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       double valueArg, unsigned attrs) {
  JS::Value value = JS::NumberValue(valueArg);
  return ::DefineUCDataProperty(
      cx, obj, name, namelen, JS::Handle<JS::Value>::fromMarkedLocation(&value),
      attrs);
}

extern bool PropertySpecNameToId(JSContext* cx, JSPropertySpec::Name name,
                                 MutableHandleId id);

static bool DefineSelfHostedProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     JS::Handle<jsid> id,
                                     const char* getterName,
                                     const char* setterName, unsigned attrs) {
  JSAtom* getterNameAtom = Atomize(cx, getterName, strlen(getterName));
  if (!getterNameAtom) {
    return false;
  }
  JS::Rooted<PropertyName*> getterNameName(cx,
                                           getterNameAtom->asPropertyName());

  JS::Rooted<JSAtom*> name(cx, IdToFunctionName(cx, id));
  if (!name) {
    return false;
  }

  JS::Rooted<JS::Value> getterValue(cx);
  if (!GlobalObject::getSelfHostedFunction(cx, cx->global(), getterNameName,
                                           name, 0, &getterValue)) {
    return false;
  }
  MOZ_ASSERT(getterValue.isObject() && getterValue.toObject().is<JSFunction>());
  JS::Rooted<JSFunction*> getterFunc(cx,
                                     &getterValue.toObject().as<JSFunction>());

  JS::Rooted<JSFunction*> setterFunc(cx);
  if (setterName) {
    JSAtom* setterNameAtom = Atomize(cx, setterName, strlen(setterName));
    if (!setterNameAtom) {
      return false;
    }
    JS::Rooted<PropertyName*> setterNameName(cx,
                                             setterNameAtom->asPropertyName());

    JS::Rooted<JS::Value> setterValue(cx);
    if (!GlobalObject::getSelfHostedFunction(cx, cx->global(), setterNameName,
                                             name, 1, &setterValue)) {
      return false;
    }
    MOZ_ASSERT(setterValue.isObject() &&
               setterValue.toObject().is<JSFunction>());
    setterFunc = &setterValue.toObject().as<JSFunction>();
  }

  return ::DefineAccessorPropertyById(cx, obj, id, getterFunc, setterFunc,
                                      attrs);
}

static bool DefineDataElement(JSContext* cx, JS::Handle<JSObject*> obj,
                              uint32_t index, JS::Handle<JS::Value> value,
                              unsigned attrs) {
  cx->check(obj, value);
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JS::Rooted<jsid> id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return ::DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index, JS::Handle<JS::Value> value,
                                    unsigned attrs) {
  return ::DefineDataElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index,
                                    JS::Handle<JSObject*> getter,
                                    JS::Handle<JSObject*> setter,
                                    unsigned attrs) {
  JS::Rooted<jsid> id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return ::DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index,
                                    JS::Handle<JSObject*> valueArg,
                                    unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*valueArg));
  return ::DefineDataElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index, HandleString valueArg,
                                    unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::StringValue(valueArg));
  return ::DefineDataElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index, int32_t valueArg,
                                    unsigned attrs) {
  JS::Value value = JS::Int32Value(valueArg);
  return ::DefineDataElement(
      cx, obj, index, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index, uint32_t valueArg,
                                    unsigned attrs) {
  JS::Value value = JS::NumberValue(valueArg);
  return ::DefineDataElement(
      cx, obj, index, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index, double valueArg,
                                    unsigned attrs) {
  JS::Value value = JS::NumberValue(valueArg);
  return ::DefineDataElement(
      cx, obj, index, JS::Handle<JS::Value>::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_HasPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                      JS::Handle<jsid> id, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  return js::HasProperty(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                  const char* name, bool* foundp) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_HasPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                    const char16_t* name, size_t namelen,
                                    bool* foundp) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_HasPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JS::Rooted<jsid> id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return JS_HasPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasOwnPropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  return js::HasOwnProperty(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasOwnProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, bool* foundp) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_HasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_ForwardGetPropertyTo(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           JS::Handle<jsid> id,
                                           JS::Handle<JS::Value> receiver,
                                           JS::MutableHandle<JS::Value> vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, receiver);

  return js::GetProperty(cx, obj, receiver, id, vp);
}

JS_PUBLIC_API bool JS_ForwardGetElementTo(JSContext* cx,
                                          JS::Handle<JSObject*> obj,
                                          uint32_t index,
                                          JS::Handle<JSObject*> receiver,
                                          JS::MutableHandle<JS::Value> vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  return js::GetElement(cx, obj, receiver, index, vp);
}

JS_PUBLIC_API bool JS_GetPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                      JS::Handle<jsid> id,
                                      JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
  return JS_ForwardGetPropertyTo(cx, obj, id, receiver, vp);
}

JS_PUBLIC_API bool JS_GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                  const char* name,
                                  JS::MutableHandle<JS::Value> vp) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_GetPropertyById(cx, obj, id, vp);
}

JS_PUBLIC_API bool JS_GetUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                    const char16_t* name, size_t namelen,
                                    JS::MutableHandle<JS::Value> vp) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_GetPropertyById(cx, obj, id, vp);
}

JS_PUBLIC_API bool JS_GetElement(JSContext* cx, JS::Handle<JSObject*> objArg,
                                 uint32_t index,
                                 JS::MutableHandle<JS::Value> vp) {
  return JS_ForwardGetElementTo(cx, objArg, index, objArg, vp);
}

JS_PUBLIC_API bool JS_ForwardSetPropertyTo(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           JS::Handle<jsid> id,
                                           JS::Handle<JS::Value> v,
                                           JS::Handle<JS::Value> receiver,
                                           JS::ObjectOpResult& result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, v, receiver);

  return js::SetProperty(cx, obj, id, v, receiver, result);
}

JS_PUBLIC_API bool JS_SetPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                      JS::Handle<jsid> id,
                                      JS::Handle<JS::Value> v) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, v);

  JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
  JS::ObjectOpResult ignored;
  return js::SetProperty(cx, obj, id, v, receiver, ignored);
}

JS_PUBLIC_API bool JS_SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                  const char* name, JS::Handle<JS::Value> v) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_SetPropertyById(cx, obj, id, v);
}

JS_PUBLIC_API bool JS_SetUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                    const char16_t* name, size_t namelen,
                                    JS::Handle<JS::Value> v) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_SetPropertyById(cx, obj, id, v);
}

static bool SetElement(JSContext* cx, JS::Handle<JSObject*> obj, uint32_t index,
                       JS::Handle<JS::Value> v) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, v);

  JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
  JS::ObjectOpResult ignored;
  return js::SetElement(cx, obj, index, v, receiver, ignored);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, JS::Handle<JS::Value> v) {
  return ::SetElement(cx, obj, index, v);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, JS::Handle<JSObject*> v) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectOrNullValue(v));
  return ::SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, HandleString v) {
  JS::Rooted<JS::Value> value(cx, JS::StringValue(v));
  return ::SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, int32_t v) {
  JS::Rooted<JS::Value> value(cx, JS::NumberValue(v));
  return ::SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, uint32_t v) {
  JS::Rooted<JS::Value> value(cx, JS::NumberValue(v));
  return ::SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, double v) {
  JS::Rooted<JS::Value> value(cx, JS::NumberValue(v));
  return ::SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::ObjectOpResult& result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  return js::DeleteProperty(cx, obj, id, result);
}

JS_PUBLIC_API bool JS_DeleteProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name,
                                     JS::ObjectOpResult& result) {
  CHECK_THREAD(cx);
  cx->check(obj);

  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return js::DeleteProperty(cx, obj, id, result);
}

JS_PUBLIC_API bool JS_DeleteUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       JS::ObjectOpResult& result) {
  CHECK_THREAD(cx);
  cx->check(obj);

  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return js::DeleteProperty(cx, obj, id, result);
}

JS_PUBLIC_API bool JS_DeleteElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index,
                                    JS::ObjectOpResult& result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  return js::DeleteElement(cx, obj, index, result);
}

JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id) {
  JS::ObjectOpResult ignored;
  return JS_DeletePropertyById(cx, obj, id, ignored);
}

JS_PUBLIC_API bool JS_DeleteProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name) {
  JS::ObjectOpResult ignored;
  return JS_DeleteProperty(cx, obj, name, ignored);
}

JS_PUBLIC_API bool JS_DeleteElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index) {
  JS::ObjectOpResult ignored;
  return JS_DeleteElement(cx, obj, index, ignored);
}

JS_PUBLIC_API bool JS_Enumerate(JSContext* cx, JS::Handle<JSObject*> obj,
                                JS::MutableHandle<IdVector> props) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, props);
  MOZ_ASSERT(props.empty());

  JS::RootedVector<JS::PropertyKey> ids(cx);
  if (!js::GetPropertyKeys(cx, obj, JSITER_OWNONLY, &ids)) {
    return false;
  }

  return props.append(ids.begin(), ids.end());
}

JS_PUBLIC_API JSObject* JS_DefineObject(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        const char* name, const JSClass* clasp,
                                        unsigned attrs) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  JS::Rooted<JSObject*> nobj(cx);
  if (!clasp) {
    // Default class is Object.
    nobj = NewPlainObject(cx);
  } else {
    nobj = NewBuiltinClassInstance(cx, clasp);
  }
  if (!nobj) {
    return nullptr;
  }

  JS::Rooted<JS::Value> nobjValue(cx, JS::ObjectValue(*nobj));
  if (!::DefineDataProperty(cx, obj, name, nobjValue, attrs)) {
    return nullptr;
  }

  return nobj;
}

JS_PUBLIC_API bool JS_DefineProperties(JSContext* cx, JS::Handle<JSObject*> obj,
                                       const JSPropertySpec* ps) {
  JS::Rooted<jsid> id(cx);

  for (; ps->name; ps++) {
    if (!PropertySpecNameToId(cx, ps->name, &id)) {
      return false;
    }

    if (ShouldIgnorePropertyDefinition(cx, StandardProtoKeyOrNull(obj), id)) {
      continue;
    }

    if (ps->isAccessor()) {
      if (ps->isSelfHosted()) {
        if (!::DefineSelfHostedProperty(
                cx, obj, id, ps->u.accessors.getter.selfHosted.funname,
                ps->u.accessors.setter.selfHosted.funname, ps->attributes())) {
          return false;
        }
      } else {
        if (!::DefineAccessorPropertyById(
                cx, obj, id, ps->u.accessors.getter.native,
                ps->u.accessors.setter.native, ps->attributes())) {
          return false;
        }
      }
    } else {
      JS::Rooted<JS::Value> v(cx);
      if (!ps->getValue(cx, &v)) {
        return false;
      }

      if (!::DefineDataPropertyById(cx, obj, id, v, ps->attributes())) {
        return false;
      }
    }
  }
  return true;
}

JS_PUBLIC_API bool JS_AlreadyHasOwnPropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  if (!obj->is<NativeObject>()) {
    return js::HasOwnProperty(cx, obj, id, foundp);
  }

  PropertyResult prop;
  if (!NativeLookupOwnPropertyNoResolve(cx, &obj->as<NativeObject>(), id,
                                        &prop)) {
    return false;
  }
  *foundp = prop.isFound();
  return true;
}

JS_PUBLIC_API bool JS_AlreadyHasOwnProperty(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name, bool* foundp) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_AlreadyHasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_AlreadyHasOwnUCProperty(JSContext* cx,
                                              JS::Handle<JSObject*> obj,
                                              const char16_t* name,
                                              size_t namelen, bool* foundp) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  JS::Rooted<jsid> id(cx, AtomToId(atom));
  return JS_AlreadyHasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_AlreadyHasOwnElement(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           uint32_t index, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JS::Rooted<jsid> id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return JS_AlreadyHasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_DefineFunctions(JSContext* cx, JS::Handle<JSObject*> obj,
                                      const JSFunctionSpec* fs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  return js::DefineFunctions(cx, obj, fs);
}

JS_PUBLIC_API JSFunction* JS_DefineFunction(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name, JSNative call,
                                            unsigned nargs, unsigned attrs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return nullptr;
  }
  Rooted<jsid> id(cx, AtomToId(atom));
  return js::DefineFunction(cx, obj, id, call, nargs, attrs);
}

JS_PUBLIC_API JSFunction* JS_DefineUCFunction(JSContext* cx,
                                              JS::Handle<JSObject*> obj,
                                              const char16_t* name,
                                              size_t namelen, JSNative call,
                                              unsigned nargs, unsigned attrs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return nullptr;
  }
  Rooted<jsid> id(cx, AtomToId(atom));
  return js::DefineFunction(cx, obj, id, call, nargs, attrs);
}

JS_PUBLIC_API JSFunction* JS_DefineFunctionById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                JSNative call, unsigned nargs,
                                                unsigned attrs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);
  return js::DefineFunction(cx, obj, id, call, nargs, attrs);
}
