/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/PromiseLookup.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jspubtd.h"  // JSProto_*

#include "builtin/Promise.h"  // js::Promise_then, js::Promise_static_resolve, js::Promise_static_species
#include "js/HeapAPI.h"  // js::gc::IsInsideNursery
#include "js/Id.h"       // JS::PropertyKey
#include "js/Value.h"    // JS::Value, JS::ObjectValue
#include "util/Poison.h"  // js::AlwaysPoison, JS_RESET_VALUE_PATTERN, MemCheckKind
#include "vm/GlobalObject.h"  // js::GlobalObject
#include "vm/JSContext.h"     // JSContext
#include "vm/JSFunction.h"    // JSFunction
#include "vm/JSObject.h"      // JSObject
#include "vm/NativeObject.h"  // js::NativeObject
#include "vm/Runtime.h"       // js::WellKnownSymbols
#include "vm/Shape.h"         // js::Shape

#include "vm/JSObject-inl.h"  // js::IsFunctionObject, js::IsNativeFunction

using JS::ObjectValue;
using JS::Value;

using js::NativeObject;

JSFunction* js::PromiseLookup::getPromiseConstructor(JSContext* cx) {
  JSObject* obj = cx->global()->maybeGetConstructor(JSProto_Promise);
  return obj ? &obj->as<JSFunction>() : nullptr;
}

NativeObject* js::PromiseLookup::getPromisePrototype(JSContext* cx) {
  JSObject* obj = cx->global()->maybeGetPrototype(JSProto_Promise);
  return obj ? &obj->as<NativeObject>() : nullptr;
}

bool js::PromiseLookup::isDataPropertyNative(JSContext* cx, NativeObject* obj,
                                             uint32_t slot, JSNative native) {
  JSFunction* fun;
  if (!IsFunctionObject(obj->getSlot(slot), &fun)) {
    return false;
  }
  return fun->maybeNative() == native && fun->realm() == cx->realm();
}

bool js::PromiseLookup::isAccessorPropertyNative(JSContext* cx,
                                                 NativeObject* holder,
                                                 uint32_t getterSlot,
                                                 JSNative native) {
  JSObject* getter = holder->getGetter(getterSlot);
  return getter && IsNativeFunction(getter, native) &&
         getter->as<JSFunction>().realm() == cx->realm();
}

void js::PromiseLookup::initialize(JSContext* cx) {
  MOZ_ASSERT(state_ == State::Uninitialized);

  // Get the canonical Promise.prototype.
  NativeObject* promiseProto = getPromisePrototype(cx);

  // Check condition 1:
  // Leave the cache uninitialized if the Promise class itself is not yet
  // initialized.
  if (!promiseProto) {
    return;
  }

  // Get the canonical Promise constructor.
  JSFunction* promiseCtor = getPromiseConstructor(cx);
  MOZ_ASSERT(promiseCtor,
             "The Promise constructor is initialized iff Promise.prototype is "
             "initialized");

  // Shortcut returns below means Promise[@@species] will never be
  // optimizable, set to disabled now, and clear it later when we succeed.
  state_ = State::Disabled;

  // Check condition 2:
  // Look up Promise.prototype.constructor and ensure it's a data property.
  mozilla::Maybe<PropertyInfo> ctorProp =
      promiseProto->lookup(cx, cx->names().constructor);
  if (ctorProp.isNothing() || !ctorProp->isDataProperty()) {
    return;
  }

  // Get the referred value, and ensure it holds the canonical Promise
  // constructor.
  JSFunction* ctorFun;
  if (!IsFunctionObject(promiseProto->getSlot(ctorProp->slot()), &ctorFun)) {
    return;
  }
  if (ctorFun != promiseCtor) {
    return;
  }

  // Check condition 3:
  // Look up Promise.prototype.then and ensure it's a data property.
  mozilla::Maybe<PropertyInfo> thenProp =
      promiseProto->lookup(cx, cx->names().then);
  if (thenProp.isNothing() || !thenProp->isDataProperty()) {
    return;
  }

  // Get the referred value, and ensure it holds the canonical "then"
  // function.
  if (!isDataPropertyNative(cx, promiseProto, thenProp->slot(), Promise_then)) {
    return;
  }

  // Check condition 4:
  // Look up the '@@species' value on Promise.
  mozilla::Maybe<PropertyInfo> speciesProp = promiseCtor->lookup(
      cx, PropertyKey::Symbol(cx->wellKnownSymbols().species));
  if (speciesProp.isNothing() || !promiseCtor->hasGetter(*speciesProp)) {
    return;
  }

  // Get the referred value, ensure it holds the canonical Promise[@@species]
  // function.
  uint32_t speciesGetterSlot = speciesProp->slot();
  if (!isAccessorPropertyNative(cx, promiseCtor, speciesGetterSlot,
                                Promise_static_species)) {
    return;
  }

  // Check condition 5:
  // Look up Promise.resolve and ensure it's a data property.
  mozilla::Maybe<PropertyInfo> resolveProp =
      promiseCtor->lookup(cx, cx->names().resolve);
  if (resolveProp.isNothing() || !resolveProp->isDataProperty()) {
    return;
  }

  // Get the referred value, and ensure it holds the canonical "resolve"
  // function.
  if (!isDataPropertyNative(cx, promiseCtor, resolveProp->slot(),
                            Promise_static_resolve)) {
    return;
  }

  // Store raw pointers below. This is okay to do here, because all objects
  // are in the tenured heap.
  MOZ_ASSERT(!gc::IsInsideNursery(promiseCtor->shape()));
  MOZ_ASSERT(!gc::IsInsideNursery(promiseProto->shape()));

  state_ = State::Initialized;
  promiseConstructorShape_ = promiseCtor->shape();
  promiseProtoShape_ = promiseProto->shape();
  promiseSpeciesGetterSlot_ = speciesGetterSlot;
  promiseResolveSlot_ = resolveProp->slot();
  promiseProtoConstructorSlot_ = ctorProp->slot();
  promiseProtoThenSlot_ = thenProp->slot();
}

void js::PromiseLookup::reset() {
  AlwaysPoison(this, JS_RESET_VALUE_PATTERN, sizeof(*this),
               MemCheckKind::MakeUndefined);
  state_ = State::Uninitialized;
}

bool js::PromiseLookup::isPromiseStateStillSane(JSContext* cx) {
  MOZ_ASSERT(state_ == State::Initialized);

  NativeObject* promiseProto = getPromisePrototype(cx);
  MOZ_ASSERT(promiseProto);

  NativeObject* promiseCtor = getPromiseConstructor(cx);
  MOZ_ASSERT(promiseCtor);

  // Ensure that Promise.prototype still has the expected shape.
  if (promiseProto->shape() != promiseProtoShape_) {
    return false;
  }

  // Ensure that Promise still has the expected shape.
  if (promiseCtor->shape() != promiseConstructorShape_) {
    return false;
  }

  // Ensure that Promise.prototype.constructor is the canonical constructor.
  if (promiseProto->getSlot(promiseProtoConstructorSlot_) !=
      ObjectValue(*promiseCtor)) {
    return false;
  }

  // Ensure that Promise.prototype.then is the canonical "then" function.
  if (!isDataPropertyNative(cx, promiseProto, promiseProtoThenSlot_,
                            Promise_then)) {
    return false;
  }

  // Ensure the species getter contains the canonical @@species function.
  if (!isAccessorPropertyNative(cx, promiseCtor, promiseSpeciesGetterSlot_,
                                Promise_static_species)) {
    return false;
  }

  // Ensure that Promise.resolve is the canonical "resolve" function.
  if (!isDataPropertyNative(cx, promiseCtor, promiseResolveSlot_,
                            Promise_static_resolve)) {
    return false;
  }

  return true;
}

bool js::PromiseLookup::ensureInitialized(JSContext* cx,
                                          Reinitialize reinitialize) {
  if (state_ == State::Uninitialized) {
    // If the cache is not initialized, initialize it.
    initialize(cx);
  } else if (state_ == State::Initialized) {
    if (reinitialize == Reinitialize::Allowed) {
      if (!isPromiseStateStillSane(cx)) {
        // If the promise state is no longer sane, reinitialize.
        reset();
        initialize(cx);
      }
    } else {
      // When we're not allowed to reinitialize, the promise state must
      // still be sane if the cache is already initialized.
      MOZ_ASSERT(isPromiseStateStillSane(cx));
    }
  }

  // If the cache is disabled or still uninitialized, don't bother trying to
  // optimize.
  if (state_ != State::Initialized) {
    return false;
  }

  // By the time we get here, we should have a sane promise state.
  MOZ_ASSERT(isPromiseStateStillSane(cx));

  return true;
}

bool js::PromiseLookup::isDefaultPromiseState(JSContext* cx) {
  // Promise and Promise.prototype are in their default states iff the
  // lookup cache was successfully initialized.
  return ensureInitialized(cx, Reinitialize::Allowed);
}

bool js::PromiseLookup::hasDefaultProtoAndNoShadowedProperties(
    JSContext* cx, PromiseObject* promise) {
  // Ensure |promise|'s prototype is the actual Promise.prototype.
  if (promise->staticPrototype() != getPromisePrototype(cx)) {
    return false;
  }

  // Ensure |promise| doesn't define any own properties. This serves as a
  // quick check to make sure |promise| doesn't define an own "constructor"
  // or "then" property which may shadow Promise.prototype.constructor or
  // Promise.prototype.then.
  return promise->empty();
}

bool js::PromiseLookup::isDefaultInstance(JSContext* cx, PromiseObject* promise,
                                          Reinitialize reinitialize) {
  // Promise and Promise.prototype must be in their default states.
  if (!ensureInitialized(cx, reinitialize)) {
    return false;
  }

  // The object uses the default properties from Promise.prototype.
  return hasDefaultProtoAndNoShadowedProperties(cx, promise);
}
