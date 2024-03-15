/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PromiseObject_h
#define vm_PromiseObject_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // int32_t, uint64_t

#include "js/Class.h"       // JSClass
#include "js/Promise.h"     // JS::PromiseState
#include "js/RootingAPI.h"  // JS::{,Mutable}Handle
#include "js/Value.h"  // JS::Value, JS::Int32Value, JS::UndefinedHandleValue
#include "vm/NativeObject.h"  // js::NativeObject

class JS_PUBLIC_API JSObject;

namespace js {

class SavedFrame;

enum PromiseSlots {
  // Int32 value with PROMISE_FLAG_* flags below.
  PromiseSlot_Flags = 0,

  // * if this promise is pending, reaction objects
  //     * undefined if there's no reaction
  //     * maybe-wrapped PromiseReactionRecord if there's only one reacion
  //     * dense array if there are two or more more reactions
  // * if this promise is fulfilled, the resolution value
  // * if this promise is rejected, the reason for the rejection
  PromiseSlot_ReactionsOrResult,

  // * if this promise is pending, resolve/reject functions.
  //   This slot holds only the reject function. The resolve function is
  //   reachable from the reject function's extended slot.
  // * if this promise is either fulfilled or rejected, undefined
  PromiseSlot_RejectFunction,

  // Promise object's debug info, which is created on demand.
  // * if this promise has no debug info, undefined
  // * if this promise contains only its process-unique ID, the ID's number
  //   value
  // * otherwise a PromiseDebugInfo object
  PromiseSlot_DebugInfo,

  PromiseSlots,
};

// This promise is either fulfilled or rejected.
// If this flag is not set, this promise is pending.
#define PROMISE_FLAG_RESOLVED 0x1

// If this flag and PROMISE_FLAG_RESOLVED are set, this promise is fulfilled.
// If only PROMISE_FLAG_RESOLVED is set, this promise is rejected.
#define PROMISE_FLAG_FULFILLED 0x2

// Indicates the promise has ever had a fulfillment or rejection handler;
// used in unhandled rejection tracking.
#define PROMISE_FLAG_HANDLED 0x4

// This promise uses the default resolving functions.
// The PromiseSlot_RejectFunction slot is not used.
#define PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS 0x08

// This promise's Promise Resolve Function's [[AlreadyResolved]].[[Value]] is
// set to true.
//
// Valid only for promises with PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS.
// For promises without PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS, Promise
// Resolve/Reject Function's "Promise" slot represents the value.
#define PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS_ALREADY_RESOLVED 0x10

// This promise is either the return value of an async function invocation or
// an async generator's method.
#define PROMISE_FLAG_ASYNC 0x20

// This promise knows how to propagate information required to keep track of
// whether an activation behavior was in progress when the original promise in
// the promise chain was created.  This is a concept defined in the HTML spec:
// https://html.spec.whatwg.org/multipage/interaction.html#triggered-by-user-activation
// It is used by the embedder in order to request SpiderMonkey to keep track of
// this information in a Promise, and also to propagate it to newly created
// promises while processing Promise#then.
#define PROMISE_FLAG_REQUIRES_USER_INTERACTION_HANDLING 0x40

// This flag indicates whether an activation behavior was in progress when the
// original promise in the promise chain was created.  Activation behavior is a
// concept defined by the HTML spec:
// https://html.spec.whatwg.org/multipage/interaction.html#triggered-by-user-activation
// This flag is only effective when the
// PROMISE_FLAG_REQUIRES_USER_INTERACTION_HANDLING is set.
#define PROMISE_FLAG_HAD_USER_INTERACTION_UPON_CREATION 0x80

struct PromiseReactionRecordBuilder;

class PromiseObject : public NativeObject {
 public:
  static const unsigned RESERVED_SLOTS = PromiseSlots;
  static const JSClass class_;
  static const JSClass protoClass_;
  static PromiseObject* create(JSContext* cx, JS::Handle<JSObject*> executor,
                               JS::Handle<JSObject*> proto = nullptr,
                               bool needsWrapping = false);

  static PromiseObject* createSkippingExecutor(JSContext* cx);

  // Create an instance of the original Promise binding, rejected with the given
  // value.
  static PromiseObject* unforgeableReject(JSContext* cx,
                                          JS::Handle<JS::Value> value);

  // Create an instance of the original Promise binding, resolved with the given
  // value.
  //
  // However, if |value| is itself a promise -- including from another realm --
  // |value| itself will in some circumstances be returned.  This sadly means
  // this function must return |JSObject*| and can't return |PromiseObject*|.
  static JSObject* unforgeableResolve(JSContext* cx,
                                      JS::Handle<JS::Value> value);

  // Create an instance of the original Promise binding, resolved with the given
  // value *that is not a promise* -- from this realm/compartment or from any
  // other.
  //
  // If you don't know for certain that your value will never be a promise, use
  // |PromiseObject::unforgeableResolve| instead.
  //
  // Use |PromiseResolvedWithUndefined| (defined below) if your value is always
  // |undefined|.
  static PromiseObject* unforgeableResolveWithNonPromise(
      JSContext* cx, JS::Handle<JS::Value> value);

  int32_t flags() { return getFixedSlot(PromiseSlot_Flags).toInt32(); }

  void setHandled() {
    setFixedSlot(PromiseSlot_Flags,
                 JS::Int32Value(flags() | PROMISE_FLAG_HANDLED));
  }

  JS::PromiseState state() {
    int32_t flags = this->flags();
    if (!(flags & PROMISE_FLAG_RESOLVED)) {
      MOZ_ASSERT(!(flags & PROMISE_FLAG_FULFILLED));
      return JS::PromiseState::Pending;
    }
    if (flags & PROMISE_FLAG_FULFILLED) {
      return JS::PromiseState::Fulfilled;
    }
    return JS::PromiseState::Rejected;
  }

  JS::Value reactions() {
    MOZ_ASSERT(state() == JS::PromiseState::Pending);
    return getFixedSlot(PromiseSlot_ReactionsOrResult);
  }

  JS::Value value() {
    MOZ_ASSERT(state() == JS::PromiseState::Fulfilled);
    return getFixedSlot(PromiseSlot_ReactionsOrResult);
  }

  JS::Value reason() {
    MOZ_ASSERT(state() == JS::PromiseState::Rejected);
    return getFixedSlot(PromiseSlot_ReactionsOrResult);
  }

  JS::Value valueOrReason() {
    MOZ_ASSERT(state() != JS::PromiseState::Pending);
    return getFixedSlot(PromiseSlot_ReactionsOrResult);
  }

  [[nodiscard]] static bool resolve(JSContext* cx,
                                    JS::Handle<PromiseObject*> promise,
                                    JS::Handle<JS::Value> resolutionValue);
  [[nodiscard]] static bool reject(JSContext* cx,
                                   JS::Handle<PromiseObject*> promise,
                                   JS::Handle<JS::Value> rejectionValue);

  static void onSettled(JSContext* cx, JS::Handle<PromiseObject*> promise,
                        JS::Handle<js::SavedFrame*> rejectionStack);

  double allocationTime();
  double resolutionTime();
  JSObject* allocationSite();
  JSObject* resolutionSite();
  double lifetime();
  double timeToResolution() {
    MOZ_ASSERT(state() != JS::PromiseState::Pending);
    return resolutionTime() - allocationTime();
  }

  [[nodiscard]] bool dependentPromises(
      JSContext* cx, JS::MutableHandle<GCVector<Value>> values);

  // Return the process-unique ID of this promise. Only used by the debugger.
  uint64_t getID();

  // Apply 'builder' to each reaction record in this promise's list. Used only
  // by the Debugger API.
  //
  // The context cx need not be same-compartment with this promise. (In typical
  // use, cx is in a debugger compartment, and this promise is in a debuggee
  // compartment.) This function presents data to builder exactly as it appears
  // in the reaction records, so the values passed to builder methods could
  // potentially be cross-compartment with both cx and this promise.
  //
  // If this function encounters an error, it will report it to 'cx' and return
  // false. If a builder call returns false, iteration stops, and this function
  // returns false; the build should set an error on 'cx' as appropriate.
  // Otherwise, this function returns true.
  [[nodiscard]] bool forEachReactionRecord(
      JSContext* cx, PromiseReactionRecordBuilder& builder);

  bool isUnhandled() {
    MOZ_ASSERT(state() == JS::PromiseState::Rejected);
    return !(flags() & PROMISE_FLAG_HANDLED);
  }

  bool requiresUserInteractionHandling() {
    return (flags() & PROMISE_FLAG_REQUIRES_USER_INTERACTION_HANDLING);
  }

  void setRequiresUserInteractionHandling(bool state);

  bool hadUserInteractionUponCreation() {
    return (flags() & PROMISE_FLAG_HAD_USER_INTERACTION_UPON_CREATION);
  }

  void setHadUserInteractionUponCreation(bool state);

  void copyUserInteractionFlagsFrom(PromiseObject& rhs);
};

/**
 * Create an instance of the original Promise binding, resolved with the value
 * |undefined|.
 */
inline PromiseObject* PromiseResolvedWithUndefined(JSContext* cx) {
  return PromiseObject::unforgeableResolveWithNonPromise(
      cx, JS::UndefinedHandleValue);
}

}  // namespace js

#endif  // vm_PromiseObject_h
