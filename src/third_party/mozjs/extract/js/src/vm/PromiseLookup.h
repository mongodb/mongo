/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PromiseLookup_h
#define vm_PromiseLookup_h

#include "mozilla/Attributes.h"  // MOZ_NON_TEMPORARY_CLASS, MOZ_INIT_OUTSIDE_CTOR

#include <stdint.h>  // uint8_t, uint32_t

#include "js/CallArgs.h"  // JSNative

struct JS_PUBLIC_API JSContext;

class JSFunction;

namespace js {

class NativeObject;
class PromiseObject;
class Shape;

class MOZ_NON_TEMPORARY_CLASS PromiseLookup final {
  // clang-format off
    /*
     * A PromiseLookup holds the following:
     *
     *  Promise's shape (promiseConstructorShape_)
     *       To ensure that Promise has not been modified.
     *
     *  Promise.prototype's shape (promiseProtoShape_)
     *      To ensure that Promise.prototype has not been modified.
     *
     *  Promise's slot number for the @@species getter
     *  (promiseSpeciesGetterSlot_)
     *      To quickly retrieve the @@species getter for Promise.
     *
     *  Promise's slot number for resolve (promiseResolveSlot_)
     *      To quickly retrieve the Promise.resolve function.
     *
     *  Promise.prototype's slot number for constructor (promiseProtoConstructorSlot_)
     *      To quickly retrieve the Promise.prototype.constructor property.
     *
     *  Promise.prototype's slot number for then (promiseProtoThenSlot_)
     *      To quickly retrieve the Promise.prototype.then function.
     *
     * MOZ_INIT_OUTSIDE_CTOR fields below are set in |initialize()|.  The
     * constructor only initializes a |state_| field, that defines whether the
     * other fields are accessible.
     */
  // clang-format on

  // Shape of matching Promise object.
  MOZ_INIT_OUTSIDE_CTOR Shape* promiseConstructorShape_;

  // Shape of matching Promise.prototype object.
  MOZ_INIT_OUTSIDE_CTOR Shape* promiseProtoShape_;

  // Slot number for the @@species property on the Promise constructor.
  MOZ_INIT_OUTSIDE_CTOR uint32_t promiseSpeciesGetterSlot_;

  // Slots Promise.resolve, Promise.prototype.constructor, and
  // Promise.prototype.then.
  MOZ_INIT_OUTSIDE_CTOR uint32_t promiseResolveSlot_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t promiseProtoConstructorSlot_;
  MOZ_INIT_OUTSIDE_CTOR uint32_t promiseProtoThenSlot_;

  enum class State : uint8_t {
    // Flags marking the lazy initialization of the above fields.
    Uninitialized,
    Initialized,

    // The disabled flag is set when we don't want to try optimizing
    // anymore because core objects were changed.
    Disabled
  };

  State state_ = State::Uninitialized;

  // Initialize the internal fields.
  //
  // The cache is successfully initialized iff
  // 1. Promise and Promise.prototype classes are initialized.
  // 2. Promise.prototype.constructor is equal to Promise.
  // 3. Promise.prototype.then is the original `then` function.
  // 4. Promise[@@species] is the original @@species getter.
  // 5. Promise.resolve is the original `resolve` function.
  void initialize(JSContext* cx);

  // Reset the cache.
  void reset();

  // Check if the global promise-related objects have not been messed with
  // in a way that would disable this cache.
  bool isPromiseStateStillSane(JSContext* cx);

  // Flags to control whether or not ensureInitialized() is allowed to
  // reinitialize the cache when the Promise state is no longer sane.
  enum class Reinitialize : bool { Allowed, Disallowed };

  // Return true if the lookup cache is properly initialized for usage.
  bool ensureInitialized(JSContext* cx, Reinitialize reinitialize);

  // Return true if the prototype of the given Promise object is
  // Promise.prototype and the object doesn't shadow properties from
  // Promise.prototype.
  bool hasDefaultProtoAndNoShadowedProperties(JSContext* cx,
                                              PromiseObject* promise);

  // Return true if the given Promise object uses the default @@species,
  // "constructor", and "then" properties.
  bool isDefaultInstance(JSContext* cx, PromiseObject* promise,
                         Reinitialize reinitialize);

  // Return the built-in Promise constructor or null if not yet initialized.
  static JSFunction* getPromiseConstructor(JSContext* cx);

  // Return the built-in Promise prototype or null if not yet initialized.
  static NativeObject* getPromisePrototype(JSContext* cx);

  // Return true if the slot contains the given native.
  static bool isDataPropertyNative(JSContext* cx, NativeObject* obj,
                                   uint32_t slot, JSNative native);

  // Return true if the accessor shape contains the given native.
  static bool isAccessorPropertyNative(JSContext* cx, NativeObject* holder,
                                       uint32_t getterSlot, JSNative native);

 public:
  /** Construct a |PromiseSpeciesLookup| in the uninitialized state. */
  PromiseLookup() { reset(); }

  // Return true if the Promise constructor and Promise.prototype still use
  // the default built-in functions.
  bool isDefaultPromiseState(JSContext* cx);

  // Return true if the given Promise object uses the default @@species,
  // "constructor", and "then" properties.
  bool isDefaultInstance(JSContext* cx, PromiseObject* promise) {
    return isDefaultInstance(cx, promise, Reinitialize::Allowed);
  }

  // Return true if the given Promise object uses the default @@species,
  // "constructor", and "then" properties.
  bool isDefaultInstanceWhenPromiseStateIsSane(JSContext* cx,
                                               PromiseObject* promise) {
    return isDefaultInstance(cx, promise, Reinitialize::Disallowed);
  }

  // Purge the cache and all info associated with it.
  void purge() {
    if (state_ == State::Initialized) {
      reset();
    }
  }
};

}  // namespace js

#endif  // vm_PromiseLookup_h
