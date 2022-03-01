/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Fundamental operations on objects. */

#ifndef vm_ObjectOperations_inl_h
#define vm_ObjectOperations_inl_h

#include "vm/ObjectOperations.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_ALWAYS_INLINE
#include "mozilla/Likely.h"      // MOZ_UNLIKELY

#include <stdint.h>  // uint32_t

#include "jsapi.h"  // JSPROP_ENUMERATE, JS::PropertyDescriptor

#include "js/Class.h"  // js::{Delete,Get,Has}PropertyOp, JSMayResolveOp, JS::ObjectOpResult
#include "js/GCAPI.h"         // JS::AutoSuppressGCAnalysis
#include "js/Id.h"            // INT_TO_JSID, jsid, JSID_INT_MAX, SYMBOL_TO_JSID
#include "js/RootingAPI.h"    // JS::Handle, JS::MutableHandle, JS::Rooted
#include "js/Value.h"         // JS::ObjectValue, JS::Value
#include "proxy/Proxy.h"      // js::Proxy
#include "vm/JSContext.h"     // JSContext
#include "vm/JSObject.h"      // JSObject
#include "vm/NativeObject.h"  // js::NativeObject, js::Native{Get,Has,Set}Property, js::NativeGetPropertyNoGC, js::Qualified
#include "vm/ProxyObject.h"   // js::ProxyObject
#include "vm/StringType.h"    // js::NameToId
#include "vm/SymbolType.h"    // JS::Symbol

#include "vm/JSAtom-inl.h"  // js::IndexToId

namespace js {

// The functions below are the fundamental operations on objects. See the
// comment about "Standard internal methods" in jsapi.h.

/*
 * ES6 [[GetPrototypeOf]]. Get obj's prototype, storing it in protop.
 *
 * If obj is definitely not a proxy, the infallible obj->getProto() can be used
 * instead. See the comment on JSObject::getTaggedProto().
 */
inline bool GetPrototype(JSContext* cx, JS::Handle<JSObject*> obj,
                         JS::MutableHandle<JSObject*> protop) {
  if (obj->hasDynamicPrototype()) {
    MOZ_ASSERT(obj->is<ProxyObject>());
    return Proxy::getPrototype(cx, obj, protop);
  }

  protop.set(obj->staticPrototype());
  return true;
}

/*
 * ES6 [[IsExtensible]]. Extensible objects can have new properties defined on
 * them. Inextensible objects can't, and their [[Prototype]] slot is fixed as
 * well.
 */
inline bool IsExtensible(JSContext* cx, JS::Handle<JSObject*> obj,
                         bool* extensible) {
  if (obj->is<ProxyObject>()) {
    MOZ_ASSERT(!cx->isHelperThreadContext());
    return Proxy::isExtensible(cx, obj, extensible);
  }

  *extensible = obj->nonProxyIsExtensible();

  // If the following assertion fails, there's somewhere else a missing
  // call to shrinkCapacityToInitializedLength() which needs to be found and
  // fixed.
  MOZ_ASSERT_IF(obj->is<NativeObject>() && !*extensible,
                obj->as<NativeObject>().getDenseInitializedLength() ==
                    obj->as<NativeObject>().getDenseCapacity());
  return true;
}

/*
 * ES6 [[Has]]. Set *foundp to true if `id in obj` (that is, if obj has an own
 * or inherited property obj[id]), false otherwise.
 */
inline bool HasProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<jsid> id, bool* foundp) {
  if (HasPropertyOp op = obj->getOpsHasProperty()) {
    return op(cx, obj, id, foundp);
  }

  return NativeHasProperty(cx, obj.as<NativeObject>(), id, foundp);
}

inline bool HasProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        PropertyName* name, bool* foundp) {
  JS::Rooted<jsid> id(cx, NameToId(name));
  return HasProperty(cx, obj, id, foundp);
}

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
                        JS::MutableHandle<JS::Value> vp) {
  if (GetPropertyOp op = obj->getOpsGetProperty()) {
    return op(cx, obj, receiver, id, vp);
  }

  return NativeGetProperty(cx, obj.as<NativeObject>(), receiver, id, vp);
}

inline bool GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<JS::Value> receiver, PropertyName* name,
                        JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<jsid> id(cx, NameToId(name));
  return GetProperty(cx, obj, receiver, id, vp);
}

inline bool GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<JSObject*> receiver, JS::Handle<jsid> id,
                        JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<JS::Value> receiverValue(cx, JS::ObjectValue(*receiver));
  return GetProperty(cx, obj, receiverValue, id, vp);
}

inline bool GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<JSObject*> receiver, PropertyName* name,
                        JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<JS::Value> receiverValue(cx, JS::ObjectValue(*receiver));
  return GetProperty(cx, obj, receiverValue, name, vp);
}

inline bool GetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                       JS::Handle<JS::Value> receiver, uint32_t index,
                       JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<jsid> id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }

  return GetProperty(cx, obj, receiver, id, vp);
}

inline bool GetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                       JS::Handle<JSObject*> receiver, uint32_t index,
                       JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<JS::Value> receiverValue(cx, JS::ObjectValue(*receiver));
  return GetElement(cx, obj, receiverValue, index, vp);
}

inline bool GetElementLargeIndex(JSContext* cx, JS::Handle<JSObject*> obj,
                                 JS::Handle<JSObject*> receiver, uint64_t index,
                                 JS::MutableHandle<JS::Value> vp) {
  MOZ_ASSERT(index < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

  if (MOZ_LIKELY(index <= UINT32_MAX)) {
    return GetElement(cx, obj, receiver, uint32_t(index), vp);
  }

  RootedValue tmp(cx, DoubleValue(index));
  RootedId id(cx);
  if (!PrimitiveValueToId<CanGC>(cx, tmp, &id)) {
    return false;
  }

  return GetProperty(cx, obj, obj, id, vp);
}

inline bool GetPropertyNoGC(JSContext* cx, JSObject* obj,
                            const JS::Value& receiver, jsid id, JS::Value* vp) {
  if (obj->getOpsGetProperty()) {
    return false;
  }

  return NativeGetPropertyNoGC(cx, &obj->as<NativeObject>(), receiver, id, vp);
}

inline bool GetPropertyNoGC(JSContext* cx, JSObject* obj,
                            const JS::Value& receiver, PropertyName* name,
                            JS::Value* vp) {
  return GetPropertyNoGC(cx, obj, receiver, NameToId(name), vp);
}

inline bool GetElementNoGC(JSContext* cx, JSObject* obj,
                           const JS::Value& receiver, uint32_t index,
                           JS::Value* vp) {
  if (obj->getOpsGetProperty()) {
    return false;
  }

  if (index > JSID_INT_MAX) {
    return false;
  }

  return GetPropertyNoGC(cx, obj, receiver, INT_TO_JSID(index), vp);
}

static MOZ_ALWAYS_INLINE bool ClassMayResolveId(const JSAtomState& names,
                                                const JSClass* clasp, jsid id,
                                                JSObject* maybeObj) {
  MOZ_ASSERT_IF(maybeObj, maybeObj->getClass() == clasp);

  if (!clasp->getResolve()) {
    // Sanity check: we should only have a mayResolve hook if we have a
    // resolve hook.
    MOZ_ASSERT(!clasp->getMayResolve(),
               "Class with mayResolve hook but no resolve hook");
    return false;
  }

  if (JSMayResolveOp mayResolve = clasp->getMayResolve()) {
    // Tell the analysis our mayResolve hooks won't trigger GC.
    JS::AutoSuppressGCAnalysis nogc;
    if (!mayResolve(names, id, maybeObj)) {
      return false;
    }
  }

  return true;
}

// Returns whether |obj| or an object on its proto chain may have an interesting
// symbol property (see JSObject::hasInterestingSymbolProperty). If it returns
// true, *holder is set to the object that may have this property.
MOZ_ALWAYS_INLINE bool MaybeHasInterestingSymbolProperty(
    JSContext* cx, JSObject* obj, JS::Symbol* symbol,
    JSObject** holder /* = nullptr */) {
  MOZ_ASSERT(symbol->isInterestingSymbol());

  jsid id = SYMBOL_TO_JSID(symbol);
  do {
    if (obj->maybeHasInterestingSymbolProperty() ||
        MOZ_UNLIKELY(
            ClassMayResolveId(cx->names(), obj->getClass(), id, obj))) {
      if (holder) {
        *holder = obj;
      }
      return true;
    }
    obj = obj->staticPrototype();
  } while (obj);

  return false;
}

// Like GetProperty but optimized for interesting symbol properties like
// @@toStringTag.
MOZ_ALWAYS_INLINE bool GetInterestingSymbolProperty(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Symbol* sym,
    JS::MutableHandle<JS::Value> vp) {
  JSObject* holder;
  if (!MaybeHasInterestingSymbolProperty(cx, obj, sym, &holder)) {
#ifdef DEBUG
    JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
    JS::Rooted<jsid> id(cx, SYMBOL_TO_JSID(sym));
    if (!GetProperty(cx, obj, receiver, id, vp)) {
      return false;
    }
    MOZ_ASSERT(vp.isUndefined());
#endif

    vp.setUndefined();
    return true;
  }

  JS::Rooted<JSObject*> holderRoot(cx, holder);
  JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
  JS::Rooted<jsid> id(cx, SYMBOL_TO_JSID(sym));
  return GetProperty(cx, holderRoot, receiver, id, vp);
}

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
                        JS::ObjectOpResult& result) {
  if (obj->getOpsSetProperty()) {
    return JSObject::nonNativeSetProperty(cx, obj, id, v, receiver, result);
  }

  return NativeSetProperty<Qualified>(cx, obj.as<NativeObject>(), id, v,
                                      receiver, result);
}

inline bool SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<jsid> id, JS::Handle<JS::Value> v) {
  JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
  JS::ObjectOpResult result;
  return SetProperty(cx, obj, id, v, receiver, result) &&
         result.checkStrict(cx, obj, id);
}

inline bool SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        PropertyName* name, JS::Handle<JS::Value> v,
                        JS::Handle<JS::Value> receiver,
                        JS::ObjectOpResult& result) {
  JS::Rooted<jsid> id(cx, NameToId(name));
  return SetProperty(cx, obj, id, v, receiver, result);
}

inline bool SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        PropertyName* name, JS::Handle<JS::Value> v) {
  JS::Rooted<jsid> id(cx, NameToId(name));
  JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
  JS::ObjectOpResult result;
  return SetProperty(cx, obj, id, v, receiver, result) &&
         result.checkStrict(cx, obj, id);
}

inline bool SetElement(JSContext* cx, JS::Handle<JSObject*> obj, uint32_t index,
                       JS::Handle<JS::Value> v, JS::Handle<JS::Value> receiver,
                       JS::ObjectOpResult& result) {
  if (obj->getOpsSetProperty()) {
    return JSObject::nonNativeSetElement(cx, obj, index, v, receiver, result);
  }

  return NativeSetElement(cx, obj.as<NativeObject>(), index, v, receiver,
                          result);
}

/*
 * ES6 draft rev 31 (15 Jan 2015) 7.3.3 Put (O, P, V, Throw), except that on
 * success, the spec says this is supposed to return a boolean value, which we
 * don't bother doing.
 */
inline bool PutProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                        JS::Handle<jsid> id, JS::Handle<JS::Value> v,
                        bool strict) {
  JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
  JS::ObjectOpResult result;
  return SetProperty(cx, obj, id, v, receiver, result) &&
         result.checkStrictModeError(cx, obj, id, strict);
}

/*
 * ES6 [[Delete]]. Equivalent to the JS code `delete obj[id]`.
 */
inline bool DeleteProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                           JS::Handle<jsid> id, JS::ObjectOpResult& result) {
  if (DeletePropertyOp op = obj->getOpsDeleteProperty()) {
    return op(cx, obj, id, result);
  }

  return NativeDeleteProperty(cx, obj.as<NativeObject>(), id, result);
}

inline bool DeleteElement(JSContext* cx, JS::Handle<JSObject*> obj,
                          uint32_t index, JS::ObjectOpResult& result) {
  JS::Rooted<jsid> id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }

  return DeleteProperty(cx, obj, id, result);
}

} /* namespace js */

#endif /* vm_ObjectOperations_inl_h */
