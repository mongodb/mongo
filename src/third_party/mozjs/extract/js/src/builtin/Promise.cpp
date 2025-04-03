/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Promise.h"

#include "mozilla/Atomics.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "jsapi.h"
#include "jsexn.h"
#include "jsfriendapi.h"

#include "js/CallAndConstruct.h"      // JS::Construct, JS::IsCallable
#include "js/experimental/JitInfo.h"  // JSJitGetterOp, JSJitInfo
#include "js/ForOfIterator.h"         // JS::ForOfIterator
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/Stack.h"
#include "vm/ArrayObject.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/CompletionKind.h"
#include "vm/ErrorObject.h"
#include "vm/ErrorReporting.h"
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"    // js::PlainObject
#include "vm/PromiseLookup.h"  // js::PromiseLookup
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseSlot_*
#include "vm/SelfHosting.h"
#include "vm/Warnings.h"  // js::WarnNumberASCII

#include "debugger/DebugAPI-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/ErrorObject-inl.h"
#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

static double MillisecondsSinceStartup() {
  auto now = mozilla::TimeStamp::Now();
  return (now - mozilla::TimeStamp::FirstTimeStamp()).ToMilliseconds();
}

enum ResolutionMode { ResolveMode, RejectMode };

/**
 * ES2023 draft rev 714fa3dd1e8237ae9c666146270f81880089eca5
 *
 * Promise Resolve Functions
 * https://tc39.es/ecma262/#sec-promise-resolve-functions
 */
enum ResolveFunctionSlots {
  // NOTE: All slot represent [[AlreadyResolved]].[[Value]].
  //
  // The spec creates single record for [[AlreadyResolved]] and shares it
  // between Promise Resolve Function and Promise Reject Function.
  //
  //   Step 1. Let alreadyResolved be the Record { [[Value]]: false }.
  //   ...
  //   Step 6. Set resolve.[[AlreadyResolved]] to alreadyResolved.
  //   ...
  //   Step 11. Set reject.[[AlreadyResolved]] to alreadyResolved.
  //
  // We implement it by clearing all slots, both in
  // Promise Resolve Function and Promise Reject Function at the same time.
  //
  // If none of slots are undefined, [[AlreadyResolved]].[[Value]] is false.
  // If all slot are undefined, [[AlreadyResolved]].[[Value]] is true.

  // [[Promise]] slot.
  // A possibly-wrapped promise.
  ResolveFunctionSlot_Promise = 0,

  // The corresponding Promise Reject Function.
  ResolveFunctionSlot_RejectFunction,
};

/**
 * ES2023 draft rev 714fa3dd1e8237ae9c666146270f81880089eca5
 *
 * Promise Reject Functions
 * https://tc39.es/ecma262/#sec-promise-reject-functions
 */
enum RejectFunctionSlots {
  // [[Promise]] slot.
  // A possibly-wrapped promise.
  RejectFunctionSlot_Promise = 0,

  // The corresponding Promise Resolve Function.
  RejectFunctionSlot_ResolveFunction,
};

enum PromiseCombinatorElementFunctionSlots {
  PromiseCombinatorElementFunctionSlot_Data = 0,
  PromiseCombinatorElementFunctionSlot_ElementIndex,
};

enum ReactionJobSlots {
  ReactionJobSlot_ReactionRecord = 0,
};

enum ThenableJobSlots {
  // The handler to use as the Promise reaction. It is a callable object
  // that's guaranteed to be from the same compartment as the
  // PromiseReactionJob.
  ThenableJobSlot_Handler = 0,

  // JobData - a, potentially CCW-wrapped, dense list containing data
  // required for proper execution of the reaction.
  ThenableJobSlot_JobData,
};

enum ThenableJobDataIndices {
  // The Promise to resolve using the given thenable.
  ThenableJobDataIndex_Promise = 0,

  // The thenable to use as the receiver when calling the `then` function.
  ThenableJobDataIndex_Thenable,

  ThenableJobDataLength,
};

enum BuiltinThenableJobSlots {
  // The Promise to resolve using the given thenable.
  BuiltinThenableJobSlot_Promise = 0,

  // The thenable to use as the receiver when calling the built-in `then`
  // function.
  BuiltinThenableJobSlot_Thenable,
};

struct PromiseCapability {
  JSObject* promise = nullptr;
  JSObject* resolve = nullptr;
  JSObject* reject = nullptr;

  PromiseCapability() = default;

  void trace(JSTracer* trc);
};

void PromiseCapability::trace(JSTracer* trc) {
  if (promise) {
    TraceRoot(trc, &promise, "PromiseCapability::promise");
  }
  if (resolve) {
    TraceRoot(trc, &resolve, "PromiseCapability::resolve");
  }
  if (reject) {
    TraceRoot(trc, &reject, "PromiseCapability::reject");
  }
}

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<PromiseCapability, Wrapper> {
  const PromiseCapability& capability() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  HandleObject promise() const {
    return HandleObject::fromMarkedLocation(&capability().promise);
  }
  HandleObject resolve() const {
    return HandleObject::fromMarkedLocation(&capability().resolve);
  }
  HandleObject reject() const {
    return HandleObject::fromMarkedLocation(&capability().reject);
  }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<PromiseCapability, Wrapper>
    : public WrappedPtrOperations<PromiseCapability, Wrapper> {
  PromiseCapability& capability() { return static_cast<Wrapper*>(this)->get(); }

 public:
  MutableHandleObject promise() {
    return MutableHandleObject::fromMarkedLocation(&capability().promise);
  }
  MutableHandleObject resolve() {
    return MutableHandleObject::fromMarkedLocation(&capability().resolve);
  }
  MutableHandleObject reject() {
    return MutableHandleObject::fromMarkedLocation(&capability().reject);
  }
};

}  // namespace js

struct PromiseCombinatorElements;

class PromiseCombinatorDataHolder : public NativeObject {
  enum {
    Slot_Promise = 0,
    Slot_RemainingElements,
    Slot_ValuesArray,
    Slot_ResolveOrRejectFunction,
    SlotsCount,
  };

 public:
  static const JSClass class_;
  JSObject* promiseObj() { return &getFixedSlot(Slot_Promise).toObject(); }
  JSObject* resolveOrRejectObj() {
    return &getFixedSlot(Slot_ResolveOrRejectFunction).toObject();
  }
  Value valuesArray() { return getFixedSlot(Slot_ValuesArray); }
  int32_t remainingCount() {
    return getFixedSlot(Slot_RemainingElements).toInt32();
  }
  int32_t increaseRemainingCount() {
    int32_t remainingCount = getFixedSlot(Slot_RemainingElements).toInt32();
    remainingCount++;
    setFixedSlot(Slot_RemainingElements, Int32Value(remainingCount));
    return remainingCount;
  }
  int32_t decreaseRemainingCount() {
    int32_t remainingCount = getFixedSlot(Slot_RemainingElements).toInt32();
    remainingCount--;
    MOZ_ASSERT(remainingCount >= 0, "unpaired calls to decreaseRemainingCount");
    setFixedSlot(Slot_RemainingElements, Int32Value(remainingCount));
    return remainingCount;
  }

  static PromiseCombinatorDataHolder* New(
      JSContext* cx, HandleObject resultPromise,
      Handle<PromiseCombinatorElements> elements, HandleObject resolveOrReject);
};

const JSClass PromiseCombinatorDataHolder::class_ = {
    "PromiseCombinatorDataHolder", JSCLASS_HAS_RESERVED_SLOTS(SlotsCount)};

// Smart pointer to the "F.[[Values]]" part of the state of a Promise.all or
// Promise.allSettled invocation, or the "F.[[Errors]]" part of the state of a
// Promise.any invocation. Copes with compartment issues when setting an
// element.
struct MOZ_STACK_CLASS PromiseCombinatorElements final {
  // Object value holding the elements array. The object can be a wrapper.
  Value value;

  // Unwrapped elements array. May not belong to the current compartment!
  ArrayObject* unwrappedArray = nullptr;

  // Set to true if the |setElement| method needs to wrap its input value.
  bool setElementNeedsWrapping = false;

  PromiseCombinatorElements() = default;

  void trace(JSTracer* trc);
};

void PromiseCombinatorElements::trace(JSTracer* trc) {
  TraceRoot(trc, &value, "PromiseCombinatorElements::value");
  if (unwrappedArray) {
    TraceRoot(trc, &unwrappedArray,
              "PromiseCombinatorElements::unwrappedArray");
  }
}

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<PromiseCombinatorElements, Wrapper> {
  const PromiseCombinatorElements& elements() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  HandleValue value() const {
    return HandleValue::fromMarkedLocation(&elements().value);
  }

  Handle<ArrayObject*> unwrappedArray() const {
    return Handle<ArrayObject*>::fromMarkedLocation(&elements().unwrappedArray);
  }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<PromiseCombinatorElements, Wrapper>
    : public WrappedPtrOperations<PromiseCombinatorElements, Wrapper> {
  PromiseCombinatorElements& elements() {
    return static_cast<Wrapper*>(this)->get();
  }

 public:
  MutableHandleValue value() {
    return MutableHandleValue::fromMarkedLocation(&elements().value);
  }

  MutableHandle<ArrayObject*> unwrappedArray() {
    return MutableHandle<ArrayObject*>::fromMarkedLocation(
        &elements().unwrappedArray);
  }

  void initialize(ArrayObject* arrayObj) {
    unwrappedArray().set(arrayObj);
    value().setObject(*arrayObj);

    // |needsWrapping| isn't tracked here, because all modifications on the
    // initial elements don't require any wrapping.
  }

  void initialize(PromiseCombinatorDataHolder* data, ArrayObject* arrayObj,
                  bool needsWrapping) {
    unwrappedArray().set(arrayObj);
    value().set(data->valuesArray());
    elements().setElementNeedsWrapping = needsWrapping;
  }

  [[nodiscard]] bool pushUndefined(JSContext* cx) {
    // Helper for the AutoRealm we need to work with |array|. We mostly do this
    // for performance; we could go ahead and do the define via a cross-
    // compartment proxy instead...
    AutoRealm ar(cx, unwrappedArray());

    Handle<ArrayObject*> arrayObj = unwrappedArray();
    return js::NewbornArrayPush(cx, arrayObj, UndefinedValue());
  }

  // `Promise.all` Resolve Element Functions
  // Step 9. Set values[index] to x.
  //
  // `Promise.allSettled` Resolve Element Functions
  // `Promise.allSettled` Reject Element Functions
  // Step 12. Set values[index] to obj.
  //
  // `Promise.any` Reject Element Functions
  // Step 9. Set errors[index] to x.
  //
  // These handler functions are always created in the compartment of the
  // Promise.all/allSettled/any function, which isn't necessarily the same
  // compartment as unwrappedArray as explained in NewPromiseCombinatorElements.
  // So before storing |val| we may need to enter unwrappedArray's compartment.
  [[nodiscard]] bool setElement(JSContext* cx, uint32_t index,
                                HandleValue val) {
    // The index is guaranteed to be initialized to `undefined`.
    MOZ_ASSERT(unwrappedArray()->getDenseElement(index).isUndefined());

    if (elements().setElementNeedsWrapping) {
      AutoRealm ar(cx, unwrappedArray());

      RootedValue rootedVal(cx, val);
      if (!cx->compartment()->wrap(cx, &rootedVal)) {
        return false;
      }
      unwrappedArray()->setDenseElement(index, rootedVal);
    } else {
      unwrappedArray()->setDenseElement(index, val);
    }
    return true;
  }
};

}  // namespace js

PromiseCombinatorDataHolder* PromiseCombinatorDataHolder::New(
    JSContext* cx, HandleObject resultPromise,
    Handle<PromiseCombinatorElements> elements, HandleObject resolveOrReject) {
  auto* dataHolder = NewBuiltinClassInstance<PromiseCombinatorDataHolder>(cx);
  if (!dataHolder) {
    return nullptr;
  }

  cx->check(resultPromise);
  cx->check(elements.value());
  cx->check(resolveOrReject);

  dataHolder->setFixedSlot(Slot_Promise, ObjectValue(*resultPromise));
  dataHolder->setFixedSlot(Slot_RemainingElements, Int32Value(1));
  dataHolder->setFixedSlot(Slot_ValuesArray, elements.value());
  dataHolder->setFixedSlot(Slot_ResolveOrRejectFunction,
                           ObjectValue(*resolveOrReject));
  return dataHolder;
}

namespace {
// Generator used by PromiseObject::getID.
mozilla::Atomic<uint64_t> gIDGenerator(0);
}  // namespace

class PromiseDebugInfo : public NativeObject {
 private:
  enum Slots {
    Slot_AllocationSite,
    Slot_ResolutionSite,
    Slot_AllocationTime,
    Slot_ResolutionTime,
    Slot_Id,
    SlotCount
  };

 public:
  static const JSClass class_;
  static PromiseDebugInfo* create(JSContext* cx,
                                  Handle<PromiseObject*> promise) {
    Rooted<PromiseDebugInfo*> debugInfo(
        cx, NewBuiltinClassInstance<PromiseDebugInfo>(cx));
    if (!debugInfo) {
      return nullptr;
    }

    RootedObject stack(cx);
    if (!JS::CaptureCurrentStack(cx, &stack,
                                 JS::StackCapture(JS::AllFrames()))) {
      return nullptr;
    }
    debugInfo->setFixedSlot(Slot_AllocationSite, ObjectOrNullValue(stack));
    debugInfo->setFixedSlot(Slot_ResolutionSite, NullValue());
    debugInfo->setFixedSlot(Slot_AllocationTime,
                            DoubleValue(MillisecondsSinceStartup()));
    debugInfo->setFixedSlot(Slot_ResolutionTime, NumberValue(0));
    promise->setFixedSlot(PromiseSlot_DebugInfo, ObjectValue(*debugInfo));

    return debugInfo;
  }

  static PromiseDebugInfo* FromPromise(PromiseObject* promise) {
    Value val = promise->getFixedSlot(PromiseSlot_DebugInfo);
    if (val.isObject()) {
      return &val.toObject().as<PromiseDebugInfo>();
    }
    return nullptr;
  }

  /**
   * Returns the given PromiseObject's process-unique ID.
   * The ID is lazily assigned when first queried, and then either stored
   * in the DebugInfo slot if no debug info was recorded for this Promise,
   * or in the Id slot of the DebugInfo object.
   */
  static uint64_t id(PromiseObject* promise) {
    Value idVal(promise->getFixedSlot(PromiseSlot_DebugInfo));
    if (idVal.isUndefined()) {
      idVal.setDouble(++gIDGenerator);
      promise->setFixedSlot(PromiseSlot_DebugInfo, idVal);
    } else if (idVal.isObject()) {
      PromiseDebugInfo* debugInfo = FromPromise(promise);
      idVal = debugInfo->getFixedSlot(Slot_Id);
      if (idVal.isUndefined()) {
        idVal.setDouble(++gIDGenerator);
        debugInfo->setFixedSlot(Slot_Id, idVal);
      }
    }
    return uint64_t(idVal.toNumber());
  }

  double allocationTime() {
    return getFixedSlot(Slot_AllocationTime).toNumber();
  }
  double resolutionTime() {
    return getFixedSlot(Slot_ResolutionTime).toNumber();
  }
  JSObject* allocationSite() {
    return getFixedSlot(Slot_AllocationSite).toObjectOrNull();
  }
  JSObject* resolutionSite() {
    return getFixedSlot(Slot_ResolutionSite).toObjectOrNull();
  }

  // The |unwrappedRejectionStack| parameter should only be set on promise
  // rejections and should be the stack of the exception that caused the promise
  // to be rejected. If the |unwrappedRejectionStack| is null, the current stack
  // will be used instead. This is also the default behavior for fulfilled
  // promises.
  static void setResolutionInfo(JSContext* cx, Handle<PromiseObject*> promise,
                                Handle<SavedFrame*> unwrappedRejectionStack) {
    MOZ_ASSERT_IF(unwrappedRejectionStack,
                  promise->state() == JS::PromiseState::Rejected);

    if (!JS::IsAsyncStackCaptureEnabledForRealm(cx)) {
      return;
    }

    // If async stacks weren't enabled and the Promise's global wasn't a
    // debuggee when the Promise was created, we won't have a debugInfo
    // object. We still want to capture the resolution stack, so we
    // create the object now and change it's slots' values around a bit.
    Rooted<PromiseDebugInfo*> debugInfo(cx, FromPromise(promise));
    if (!debugInfo) {
      RootedValue idVal(cx, promise->getFixedSlot(PromiseSlot_DebugInfo));
      debugInfo = create(cx, promise);
      if (!debugInfo) {
        cx->clearPendingException();
        return;
      }

      // The current stack was stored in the AllocationSite slot, move
      // it to ResolutionSite as that's what it really is.
      debugInfo->setFixedSlot(Slot_ResolutionSite,
                              debugInfo->getFixedSlot(Slot_AllocationSite));
      debugInfo->setFixedSlot(Slot_AllocationSite, NullValue());

      // There's no good default for a missing AllocationTime, so
      // instead of resetting that, ensure that it's the same as
      // ResolutionTime, so that the diff shows as 0, which isn't great,
      // but bearable.
      debugInfo->setFixedSlot(Slot_ResolutionTime,
                              debugInfo->getFixedSlot(Slot_AllocationTime));

      // The Promise's ID might've been queried earlier, in which case
      // it's stored in the DebugInfo slot. We saved that earlier, so
      // now we can store it in the right place (or leave it as
      // undefined if it wasn't ever initialized.)
      debugInfo->setFixedSlot(Slot_Id, idVal);
      return;
    }

    RootedObject stack(cx, unwrappedRejectionStack);
    if (stack) {
      // The exception stack is always unwrapped so it might be in
      // a different compartment.
      if (!cx->compartment()->wrap(cx, &stack)) {
        cx->clearPendingException();
        return;
      }
    } else {
      if (!JS::CaptureCurrentStack(cx, &stack,
                                   JS::StackCapture(JS::AllFrames()))) {
        cx->clearPendingException();
        return;
      }
    }

    debugInfo->setFixedSlot(Slot_ResolutionSite, ObjectOrNullValue(stack));
    debugInfo->setFixedSlot(Slot_ResolutionTime,
                            DoubleValue(MillisecondsSinceStartup()));
  }
};

const JSClass PromiseDebugInfo::class_ = {
    "PromiseDebugInfo", JSCLASS_HAS_RESERVED_SLOTS(SlotCount)};

double PromiseObject::allocationTime() {
  auto debugInfo = PromiseDebugInfo::FromPromise(this);
  if (debugInfo) {
    return debugInfo->allocationTime();
  }
  return 0;
}

double PromiseObject::resolutionTime() {
  auto debugInfo = PromiseDebugInfo::FromPromise(this);
  if (debugInfo) {
    return debugInfo->resolutionTime();
  }
  return 0;
}

JSObject* PromiseObject::allocationSite() {
  auto debugInfo = PromiseDebugInfo::FromPromise(this);
  if (debugInfo) {
    return debugInfo->allocationSite();
  }
  return nullptr;
}

JSObject* PromiseObject::resolutionSite() {
  auto debugInfo = PromiseDebugInfo::FromPromise(this);
  if (debugInfo) {
    JSObject* site = debugInfo->resolutionSite();
    if (site && !JS_IsDeadWrapper(site)) {
      MOZ_ASSERT(UncheckedUnwrap(site)->is<SavedFrame>());
      return site;
    }
  }
  return nullptr;
}

/**
 * Wrapper for GetAndClearExceptionAndStack that handles cases where
 * no exception is pending, but an error occurred.
 * This can be the case if an OOM was encountered while throwing the error.
 */
static bool MaybeGetAndClearExceptionAndStack(
    JSContext* cx, MutableHandleValue rval, MutableHandle<SavedFrame*> stack) {
  if (!cx->isExceptionPending()) {
    return false;
  }

  return GetAndClearExceptionAndStack(cx, rval, stack);
}

[[nodiscard]] static bool CallPromiseRejectFunction(
    JSContext* cx, HandleObject rejectFun, HandleValue reason,
    HandleObject promiseObj, Handle<SavedFrame*> unwrappedRejectionStack,
    UnhandledRejectionBehavior behavior);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * IfAbruptRejectPromise ( value, capability )
 * https://tc39.es/ecma262/#sec-ifabruptrejectpromise
 *
 * Steps 1.a-b.
 *
 * Extracting all of this internal spec algorithm into a helper function would
 * be tedious, so the check in step 1 and the entirety of step 2 aren't
 * included.
 */
static bool AbruptRejectPromise(JSContext* cx, CallArgs& args,
                                HandleObject promiseObj, HandleObject reject) {
  // Step 1.a. Perform
  //           ? Call(capability.[[Reject]], undefined, « value.[[Value]] »).
  RootedValue reason(cx);
  Rooted<SavedFrame*> stack(cx);
  if (!MaybeGetAndClearExceptionAndStack(cx, &reason, &stack)) {
    return false;
  }

  if (!CallPromiseRejectFunction(cx, reject, reason, promiseObj, stack,
                                 UnhandledRejectionBehavior::Report)) {
    return false;
  }

  // Step 1.b. Return capability.[[Promise]].
  args.rval().setObject(*promiseObj);
  return true;
}

static bool AbruptRejectPromise(JSContext* cx, CallArgs& args,
                                Handle<PromiseCapability> capability) {
  return AbruptRejectPromise(cx, args, capability.promise(),
                             capability.reject());
}

enum ReactionRecordSlots {
  // This is the promise-like object that gets resolved with the result of this
  // reaction, if any. If this reaction record was created with .then or .catch,
  // this is the promise that .then or .catch returned.
  //
  // The spec says that a PromiseReaction record has a [[Capability]] field
  // whose value is either undefined or a PromiseCapability record, but we just
  // store the PromiseCapability's fields directly in this object. This is the
  // capability's [[Promise]] field; its [[Resolve]] and [[Reject]] fields are
  // stored in ReactionRecordSlot_Resolve and ReactionRecordSlot_Reject.
  //
  // This can be 'null' in reaction records created for a few situations:
  //
  // - When you resolve one promise to another. When you pass a promise P1 to
  //   the 'fulfill' function of a promise P2, so that resolving P1 resolves P2
  //   in the same way, P1 gets a reaction record with the
  //   REACTION_FLAG_DEFAULT_RESOLVING_HANDLER flag set and whose
  //   ReactionRecordSlot_GeneratorOrPromiseToResolve slot holds P2.
  //
  // - When you await a promise. When an async function or generator awaits a
  //   value V, then the await expression generates an internal promise P,
  //   resolves it to V, and then gives P a reaction record with the
  //   REACTION_FLAG_ASYNC_FUNCTION or REACTION_FLAG_ASYNC_GENERATOR flag set
  //   and whose ReactionRecordSlot_GeneratorOrPromiseToResolve slot holds the
  //   generator object. (Typically V is a promise, so resolving P to V gives V
  //   a REACTION_FLAGS_DEFAULT_RESOLVING_HANDLER reaction record as described
  //   above.)
  //
  // - When JS::AddPromiseReactions{,IgnoringUnhandledRejection} cause the
  //   reaction to be created.  (These functions act as if they had created a
  //   promise to invoke the appropriate provided reaction function, without
  //   actually allocating a promise for them.)
  ReactionRecordSlot_Promise = 0,

  // The [[Handler]] field(s) of a PromiseReaction record. We create a
  // single reaction record for fulfillment and rejection, therefore our
  // PromiseReaction implementation needs two [[Handler]] fields.
  //
  // The slot value is either a callable object, an integer constant from
  // the |PromiseHandler| enum, or null. If the value is null, either the
  // REACTION_FLAG_DEBUGGER_DUMMY or the
  // REACTION_FLAG_DEFAULT_RESOLVING_HANDLER flag must be set.
  //
  // After setting the target state for a PromiseReaction, the slot of the
  // no longer used handler gets reused to store the argument of the active
  // handler.
  ReactionRecordSlot_OnFulfilled,
  ReactionRecordSlot_OnRejectedArg = ReactionRecordSlot_OnFulfilled,
  ReactionRecordSlot_OnRejected,
  ReactionRecordSlot_OnFulfilledArg = ReactionRecordSlot_OnRejected,

  // The functions to resolve or reject the promise. Matches the
  // [[Capability]].[[Resolve]] and [[Capability]].[[Reject]] fields from
  // the spec.
  //
  // The slot values are either callable objects or null, but the latter
  // case is only allowed if the promise is either a built-in Promise object
  // or null.
  ReactionRecordSlot_Resolve,
  ReactionRecordSlot_Reject,

  // The incumbent global for this reaction record. Can be null.
  ReactionRecordSlot_IncumbentGlobalObject,

  // Bitmask of the REACTION_FLAG values.
  ReactionRecordSlot_Flags,

  // Additional slot to store extra data for specific reaction record types.
  //
  // - When the REACTION_FLAG_ASYNC_FUNCTION flag is set, this slot stores
  //   the (internal) generator object for this promise reaction.
  // - When the REACTION_FLAG_ASYNC_GENERATOR flag is set, this slot stores
  //   the async generator object for this promise reaction.
  // - When the REACTION_FLAG_DEFAULT_RESOLVING_HANDLER flag is set, this
  //   slot stores the promise to resolve when conceptually "calling" the
  //   OnFulfilled or OnRejected handlers.
  ReactionRecordSlot_GeneratorOrPromiseToResolve,

  ReactionRecordSlots,
};

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PromiseReaction Records
 * https://tc39.es/ecma262/#sec-promisereaction-records
 */
class PromiseReactionRecord : public NativeObject {
  // If this flag is set, this reaction record is already enqueued to the
  // job queue, and the spec's [[Type]] field is represented by
  // REACTION_FLAG_FULFILLED flag.
  //
  // If this flag isn't yet set, [[Type]] field is undefined.
  static constexpr uint32_t REACTION_FLAG_RESOLVED = 0x1;

  // This bit is valid only when REACTION_FLAG_RESOLVED flag is set.
  //
  // If this flag is set, [[Type]] field is Fulfill.
  // If this flag isn't set, [[Type]] field is Reject.
  static constexpr uint32_t REACTION_FLAG_FULFILLED = 0x2;

  // If this flag is set, this reaction record is created for resolving
  // one promise P1 to another promise P2, and
  // ReactionRecordSlot_GeneratorOrPromiseToResolve slot holds P2.
  static constexpr uint32_t REACTION_FLAG_DEFAULT_RESOLVING_HANDLER = 0x4;

  // If this flag is set, this reaction record is created for async function
  // and ReactionRecordSlot_GeneratorOrPromiseToResolve slot holds
  // internal generator object of the async function.
  static constexpr uint32_t REACTION_FLAG_ASYNC_FUNCTION = 0x8;

  // If this flag is set, this reaction record is created for async generator
  // and ReactionRecordSlot_GeneratorOrPromiseToResolve slot holds
  // the async generator object of the async generator.
  static constexpr uint32_t REACTION_FLAG_ASYNC_GENERATOR = 0x10;

  // If this flag is set, this reaction record is created only for providing
  // information to debugger.
  static constexpr uint32_t REACTION_FLAG_DEBUGGER_DUMMY = 0x20;

  // This bit is valid only when the promise object is optimized out
  // for the reaction.
  //
  // If this flag is set, unhandled rejection should be ignored.
  // Otherwise, promise object should be created on-demand for unhandled
  // rejection.
  static constexpr uint32_t REACTION_FLAG_IGNORE_UNHANDLED_REJECTION = 0x40;

  void setFlagOnInitialState(uint32_t flag) {
    int32_t flags = this->flags();
    MOZ_ASSERT(flags == 0, "Can't modify with non-default flags");
    flags |= flag;
    setFixedSlot(ReactionRecordSlot_Flags, Int32Value(flags));
  }

  uint32_t handlerSlot() {
    MOZ_ASSERT(targetState() != JS::PromiseState::Pending);
    return targetState() == JS::PromiseState::Fulfilled
               ? ReactionRecordSlot_OnFulfilled
               : ReactionRecordSlot_OnRejected;
  }

  uint32_t handlerArgSlot() {
    MOZ_ASSERT(targetState() != JS::PromiseState::Pending);
    return targetState() == JS::PromiseState::Fulfilled
               ? ReactionRecordSlot_OnFulfilledArg
               : ReactionRecordSlot_OnRejectedArg;
  }

 public:
  static const JSClass class_;

  JSObject* promise() {
    return getFixedSlot(ReactionRecordSlot_Promise).toObjectOrNull();
  }

  int32_t flags() const {
    return getFixedSlot(ReactionRecordSlot_Flags).toInt32();
  }

  JS::PromiseState targetState() {
    int32_t flags = this->flags();
    if (!(flags & REACTION_FLAG_RESOLVED)) {
      return JS::PromiseState::Pending;
    }
    return flags & REACTION_FLAG_FULFILLED ? JS::PromiseState::Fulfilled
                                           : JS::PromiseState::Rejected;
  }
  void setTargetStateAndHandlerArg(JS::PromiseState state, const Value& arg) {
    MOZ_ASSERT(targetState() == JS::PromiseState::Pending);
    MOZ_ASSERT(state != JS::PromiseState::Pending,
               "Can't revert a reaction to pending.");

    int32_t flags = this->flags();
    flags |= REACTION_FLAG_RESOLVED;
    if (state == JS::PromiseState::Fulfilled) {
      flags |= REACTION_FLAG_FULFILLED;
    }

    setFixedSlot(ReactionRecordSlot_Flags, Int32Value(flags));
    setFixedSlot(handlerArgSlot(), arg);
  }

  void setShouldIgnoreUnhandledRejection() {
    setFlagOnInitialState(REACTION_FLAG_IGNORE_UNHANDLED_REJECTION);
  }
  UnhandledRejectionBehavior unhandledRejectionBehavior() const {
    int32_t flags = this->flags();
    return (flags & REACTION_FLAG_IGNORE_UNHANDLED_REJECTION)
               ? UnhandledRejectionBehavior::Ignore
               : UnhandledRejectionBehavior::Report;
  }

  void setIsDefaultResolvingHandler(PromiseObject* promiseToResolve) {
    setFlagOnInitialState(REACTION_FLAG_DEFAULT_RESOLVING_HANDLER);
    setFixedSlot(ReactionRecordSlot_GeneratorOrPromiseToResolve,
                 ObjectValue(*promiseToResolve));
  }
  bool isDefaultResolvingHandler() {
    int32_t flags = this->flags();
    return flags & REACTION_FLAG_DEFAULT_RESOLVING_HANDLER;
  }
  PromiseObject* defaultResolvingPromise() {
    MOZ_ASSERT(isDefaultResolvingHandler());
    const Value& promiseToResolve =
        getFixedSlot(ReactionRecordSlot_GeneratorOrPromiseToResolve);
    return &promiseToResolve.toObject().as<PromiseObject>();
  }

  void setIsAsyncFunction(AsyncFunctionGeneratorObject* genObj) {
    setFlagOnInitialState(REACTION_FLAG_ASYNC_FUNCTION);
    setFixedSlot(ReactionRecordSlot_GeneratorOrPromiseToResolve,
                 ObjectValue(*genObj));
  }
  bool isAsyncFunction() {
    int32_t flags = this->flags();
    return flags & REACTION_FLAG_ASYNC_FUNCTION;
  }
  AsyncFunctionGeneratorObject* asyncFunctionGenerator() {
    MOZ_ASSERT(isAsyncFunction());
    const Value& generator =
        getFixedSlot(ReactionRecordSlot_GeneratorOrPromiseToResolve);
    return &generator.toObject().as<AsyncFunctionGeneratorObject>();
  }

  void setIsAsyncGenerator(AsyncGeneratorObject* generator) {
    setFlagOnInitialState(REACTION_FLAG_ASYNC_GENERATOR);
    setFixedSlot(ReactionRecordSlot_GeneratorOrPromiseToResolve,
                 ObjectValue(*generator));
  }
  bool isAsyncGenerator() {
    int32_t flags = this->flags();
    return flags & REACTION_FLAG_ASYNC_GENERATOR;
  }
  AsyncGeneratorObject* asyncGenerator() {
    MOZ_ASSERT(isAsyncGenerator());
    const Value& generator =
        getFixedSlot(ReactionRecordSlot_GeneratorOrPromiseToResolve);
    return &generator.toObject().as<AsyncGeneratorObject>();
  }

  void setIsDebuggerDummy() {
    setFlagOnInitialState(REACTION_FLAG_DEBUGGER_DUMMY);
  }
  bool isDebuggerDummy() {
    int32_t flags = this->flags();
    return flags & REACTION_FLAG_DEBUGGER_DUMMY;
  }

  Value handler() {
    MOZ_ASSERT(targetState() != JS::PromiseState::Pending);
    return getFixedSlot(handlerSlot());
  }
  Value handlerArg() {
    MOZ_ASSERT(targetState() != JS::PromiseState::Pending);
    return getFixedSlot(handlerArgSlot());
  }

  JSObject* getAndClearIncumbentGlobalObject() {
    JSObject* obj =
        getFixedSlot(ReactionRecordSlot_IncumbentGlobalObject).toObjectOrNull();
    setFixedSlot(ReactionRecordSlot_IncumbentGlobalObject, UndefinedValue());
    return obj;
  }
};

const JSClass PromiseReactionRecord::class_ = {
    "PromiseReactionRecord", JSCLASS_HAS_RESERVED_SLOTS(ReactionRecordSlots)};

static void AddPromiseFlags(PromiseObject& promise, int32_t flag) {
  int32_t flags = promise.flags();
  promise.setFixedSlot(PromiseSlot_Flags, Int32Value(flags | flag));
}

static void RemovePromiseFlags(PromiseObject& promise, int32_t flag) {
  int32_t flags = promise.flags();
  promise.setFixedSlot(PromiseSlot_Flags, Int32Value(flags & ~flag));
}

static bool PromiseHasAnyFlag(PromiseObject& promise, int32_t flag) {
  return promise.flags() & flag;
}

static bool ResolvePromiseFunction(JSContext* cx, unsigned argc, Value* vp);
static bool RejectPromiseFunction(JSContext* cx, unsigned argc, Value* vp);

static JSFunction* GetResolveFunctionFromReject(JSFunction* reject);
static JSFunction* GetRejectFunctionFromResolve(JSFunction* resolve);

#ifdef DEBUG

/**
 * Returns Promise Resolve Function's [[AlreadyResolved]].[[Value]].
 */
static bool IsAlreadyResolvedMaybeWrappedResolveFunction(
    JSObject* resolveFunObj) {
  if (IsWrapper(resolveFunObj)) {
    resolveFunObj = UncheckedUnwrap(resolveFunObj);
  }

  JSFunction* resolveFun = &resolveFunObj->as<JSFunction>();
  MOZ_ASSERT(resolveFun->maybeNative() == ResolvePromiseFunction);

  bool alreadyResolved =
      resolveFun->getExtendedSlot(ResolveFunctionSlot_Promise).isUndefined();

  // Other slots should agree.
  if (alreadyResolved) {
    MOZ_ASSERT(resolveFun->getExtendedSlot(ResolveFunctionSlot_RejectFunction)
                   .isUndefined());
  } else {
    JSFunction* rejectFun = GetRejectFunctionFromResolve(resolveFun);
    MOZ_ASSERT(
        !rejectFun->getExtendedSlot(RejectFunctionSlot_Promise).isUndefined());
    MOZ_ASSERT(!rejectFun->getExtendedSlot(RejectFunctionSlot_ResolveFunction)
                    .isUndefined());
  }

  return alreadyResolved;
}

/**
 * Returns Promise Reject Function's [[AlreadyResolved]].[[Value]].
 */
static bool IsAlreadyResolvedMaybeWrappedRejectFunction(
    JSObject* rejectFunObj) {
  if (IsWrapper(rejectFunObj)) {
    rejectFunObj = UncheckedUnwrap(rejectFunObj);
  }

  JSFunction* rejectFun = &rejectFunObj->as<JSFunction>();
  MOZ_ASSERT(rejectFun->maybeNative() == RejectPromiseFunction);

  bool alreadyResolved =
      rejectFun->getExtendedSlot(RejectFunctionSlot_Promise).isUndefined();

  // Other slots should agree.
  if (alreadyResolved) {
    MOZ_ASSERT(rejectFun->getExtendedSlot(RejectFunctionSlot_ResolveFunction)
                   .isUndefined());
  } else {
    JSFunction* resolveFun = GetResolveFunctionFromReject(rejectFun);
    MOZ_ASSERT(!resolveFun->getExtendedSlot(ResolveFunctionSlot_Promise)
                    .isUndefined());
    MOZ_ASSERT(!resolveFun->getExtendedSlot(ResolveFunctionSlot_RejectFunction)
                    .isUndefined());
  }

  return alreadyResolved;
}

#endif  // DEBUG

/**
 * Set Promise Resolve Function's and Promise Reject Function's
 * [[AlreadyResolved]].[[Value]] to true.
 *
 * `resolutionFun` can be either of them.
 */
static void SetAlreadyResolvedResolutionFunction(JSFunction* resolutionFun) {
  JSFunction* resolve;
  JSFunction* reject;
  if (resolutionFun->maybeNative() == ResolvePromiseFunction) {
    resolve = resolutionFun;
    reject = GetRejectFunctionFromResolve(resolutionFun);
  } else {
    resolve = GetResolveFunctionFromReject(resolutionFun);
    reject = resolutionFun;
  }

  resolve->setExtendedSlot(ResolveFunctionSlot_Promise, UndefinedValue());
  resolve->setExtendedSlot(ResolveFunctionSlot_RejectFunction,
                           UndefinedValue());

  reject->setExtendedSlot(RejectFunctionSlot_Promise, UndefinedValue());
  reject->setExtendedSlot(RejectFunctionSlot_ResolveFunction, UndefinedValue());

  MOZ_ASSERT(IsAlreadyResolvedMaybeWrappedResolveFunction(resolve));
  MOZ_ASSERT(IsAlreadyResolvedMaybeWrappedRejectFunction(reject));
}

/**
 * Returns true if given promise is created by
 * CreatePromiseObjectWithoutResolutionFunctions.
 */
bool js::IsPromiseWithDefaultResolvingFunction(PromiseObject* promise) {
  return PromiseHasAnyFlag(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS);
}

/**
 * Returns Promise Resolve Function's [[AlreadyResolved]].[[Value]] for
 * a promise created by CreatePromiseObjectWithoutResolutionFunctions.
 */
static bool IsAlreadyResolvedPromiseWithDefaultResolvingFunction(
    PromiseObject* promise) {
  MOZ_ASSERT(IsPromiseWithDefaultResolvingFunction(promise));

  if (promise->as<PromiseObject>().state() != JS::PromiseState::Pending) {
    MOZ_ASSERT(PromiseHasAnyFlag(
        *promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS_ALREADY_RESOLVED));
    return true;
  }

  return PromiseHasAnyFlag(
      *promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS_ALREADY_RESOLVED);
}

/**
 * Set Promise Resolve Function's [[AlreadyResolved]].[[Value]] to true for
 * a promise created by CreatePromiseObjectWithoutResolutionFunctions.
 */
void js::SetAlreadyResolvedPromiseWithDefaultResolvingFunction(
    PromiseObject* promise) {
  MOZ_ASSERT(IsPromiseWithDefaultResolvingFunction(promise));

  promise->setFixedSlot(
      PromiseSlot_Flags,
      JS::Int32Value(
          promise->flags() |
          PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS_ALREADY_RESOLVED));
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * CreateResolvingFunctions ( promise )
 * https://tc39.es/ecma262/#sec-createresolvingfunctions
 */
[[nodiscard]] static MOZ_ALWAYS_INLINE bool CreateResolvingFunctions(
    JSContext* cx, HandleObject promise, MutableHandleObject resolveFn,
    MutableHandleObject rejectFn) {
  // Step 1. Let alreadyResolved be the Record { [[Value]]: false }.
  // (implicit, see steps 5-6, 10-11 below)

  // Step 2. Let stepsResolve be the algorithm steps defined in Promise Resolve
  //         Functions.
  // Step 3. Let lengthResolve be the number of non-optional parameters of the
  //         function definition in Promise Resolve Functions.
  // Step 4. Let resolve be
  //         ! CreateBuiltinFunction(stepsResolve, lengthResolve, "",
  //                                 « [[Promise]], [[AlreadyResolved]] »).
  Handle<PropertyName*> funName = cx->names().empty;
  resolveFn.set(NewNativeFunction(cx, ResolvePromiseFunction, 1, funName,
                                  gc::AllocKind::FUNCTION_EXTENDED,
                                  GenericObject));
  if (!resolveFn) {
    return false;
  }

  // Step 7. Let stepsReject be the algorithm steps defined in Promise Reject
  //         Functions.
  // Step 8. Let lengthReject be the number of non-optional parameters of the
  //         function definition in Promise Reject Functions.
  // Step 9. Let reject be
  //         ! CreateBuiltinFunction(stepsReject, lengthReject, "",
  //                                 « [[Promise]], [[AlreadyResolved]] »).
  rejectFn.set(NewNativeFunction(cx, RejectPromiseFunction, 1, funName,
                                 gc::AllocKind::FUNCTION_EXTENDED,
                                 GenericObject));
  if (!rejectFn) {
    return false;
  }

  JSFunction* resolveFun = &resolveFn->as<JSFunction>();
  JSFunction* rejectFun = &rejectFn->as<JSFunction>();

  // Step 5. Set resolve.[[Promise]] to promise.
  // Step 6. Set resolve.[[AlreadyResolved]] to alreadyResolved.
  //
  // NOTE: We use these references as [[AlreadyResolved]].[[Value]].
  //       See the comment in ResolveFunctionSlots for more details.
  resolveFun->initExtendedSlot(ResolveFunctionSlot_Promise,
                               ObjectValue(*promise));
  resolveFun->initExtendedSlot(ResolveFunctionSlot_RejectFunction,
                               ObjectValue(*rejectFun));

  // Step 10. Set reject.[[Promise]] to promise.
  // Step 11. Set reject.[[AlreadyResolved]] to alreadyResolved.
  //
  // NOTE: We use these references as [[AlreadyResolved]].[[Value]].
  //       See the comment in ResolveFunctionSlots for more details.
  rejectFun->initExtendedSlot(RejectFunctionSlot_Promise,
                              ObjectValue(*promise));
  rejectFun->initExtendedSlot(RejectFunctionSlot_ResolveFunction,
                              ObjectValue(*resolveFun));

  MOZ_ASSERT(!IsAlreadyResolvedMaybeWrappedResolveFunction(resolveFun));
  MOZ_ASSERT(!IsAlreadyResolvedMaybeWrappedRejectFunction(rejectFun));

  // Step 12. Return the Record { [[Resolve]]: resolve, [[Reject]]: reject }.
  return true;
}

static bool IsSettledMaybeWrappedPromise(JSObject* promise) {
  if (IsProxy(promise)) {
    promise = UncheckedUnwrap(promise);

    // Caller needs to handle dead wrappers.
    if (JS_IsDeadWrapper(promise)) {
      return false;
    }
  }

  return promise->as<PromiseObject>().state() != JS::PromiseState::Pending;
}

[[nodiscard]] static bool RejectMaybeWrappedPromise(
    JSContext* cx, HandleObject promiseObj, HandleValue reason,
    Handle<SavedFrame*> unwrappedRejectionStack);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise Reject Functions
 * https://tc39.es/ecma262/#sec-promise-reject-functions
 */
static bool RejectPromiseFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JSFunction* reject = &args.callee().as<JSFunction>();
  HandleValue reasonVal = args.get(0);

  // Step 1. Let F be the active function object.
  // Step 2. Assert: F has a [[Promise]] internal slot whose value is an Object.
  // (implicit)

  // Step 3. Let promise be F.[[Promise]].
  const Value& promiseVal = reject->getExtendedSlot(RejectFunctionSlot_Promise);

  // Step 4. Let alreadyResolved be F.[[AlreadyResolved]].
  // Step 5. If alreadyResolved.[[Value]] is true, return undefined.
  //
  // If the Promise isn't available anymore, it has been resolved and the
  // reference to it removed to make it eligible for collection.
  bool alreadyResolved = promiseVal.isUndefined();
  MOZ_ASSERT(IsAlreadyResolvedMaybeWrappedRejectFunction(reject) ==
             alreadyResolved);
  if (alreadyResolved) {
    args.rval().setUndefined();
    return true;
  }

  RootedObject promise(cx, &promiseVal.toObject());

  // Step 6. Set alreadyResolved.[[Value]] to true.
  SetAlreadyResolvedResolutionFunction(reject);

  // In some cases the Promise reference on the resolution function won't
  // have been removed during resolution, so we need to check that here,
  // too.
  if (IsSettledMaybeWrappedPromise(promise)) {
    args.rval().setUndefined();
    return true;
  }

  // Step 7. Return RejectPromise(promise, reason).
  if (!RejectMaybeWrappedPromise(cx, promise, reasonVal, nullptr)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

[[nodiscard]] static bool FulfillMaybeWrappedPromise(JSContext* cx,
                                                     HandleObject promiseObj,
                                                     HandleValue value_);

[[nodiscard]] static bool EnqueuePromiseResolveThenableJob(
    JSContext* cx, HandleValue promiseToResolve, HandleValue thenable,
    HandleValue thenVal);

[[nodiscard]] static bool EnqueuePromiseResolveThenableBuiltinJob(
    JSContext* cx, HandleObject promiseToResolve, HandleObject thenable);

static bool Promise_then_impl(JSContext* cx, HandleValue promiseVal,
                              HandleValue onFulfilled, HandleValue onRejected,
                              MutableHandleValue rval, bool rvalExplicitlyUsed);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise Resolve Functions
 * https://tc39.es/ecma262/#sec-promise-resolve-functions
 *
 * Steps 7-15.
 */
[[nodiscard]] bool js::ResolvePromiseInternal(
    JSContext* cx, JS::Handle<JSObject*> promise,
    JS::Handle<JS::Value> resolutionVal) {
  cx->check(promise, resolutionVal);
  MOZ_ASSERT(!IsSettledMaybeWrappedPromise(promise));

  // (reordered)
  // Step 8. If Type(resolution) is not Object, then
  if (!resolutionVal.isObject()) {
    // Step 8.a. Return FulfillPromise(promise, resolution).
    return FulfillMaybeWrappedPromise(cx, promise, resolutionVal);
  }

  RootedObject resolution(cx, &resolutionVal.toObject());

  // Step 7. If SameValue(resolution, promise) is true, then
  if (resolution == promise) {
    // Step 7.a. Let selfResolutionError be a newly created TypeError object.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANNOT_RESOLVE_PROMISE_WITH_ITSELF);
    RootedValue selfResolutionError(cx);
    Rooted<SavedFrame*> stack(cx);
    if (!MaybeGetAndClearExceptionAndStack(cx, &selfResolutionError, &stack)) {
      return false;
    }

    // Step 7.b. Return RejectPromise(promise, selfResolutionError).
    return RejectMaybeWrappedPromise(cx, promise, selfResolutionError, stack);
  }

  // Step 9. Let then be Get(resolution, "then").
  RootedValue thenVal(cx);
  bool status =
      GetProperty(cx, resolution, resolution, cx->names().then, &thenVal);

  RootedValue error(cx);
  Rooted<SavedFrame*> errorStack(cx);

  // Step 10. If then is an abrupt completion, then
  if (!status) {
    // Get the `then.[[Value]]` value used in the step 10.a.
    if (!MaybeGetAndClearExceptionAndStack(cx, &error, &errorStack)) {
      return false;
    }
  }

  // Testing functions allow to directly settle a promise without going
  // through the resolving functions. In that case the normal bookkeeping to
  // ensure only pending promises can be resolved doesn't apply and we need
  // to manually check for already settled promises. The exception is simply
  // dropped when this case happens.
  if (IsSettledMaybeWrappedPromise(promise)) {
    return true;
  }

  // Step 10. If then is an abrupt completion, then
  if (!status) {
    // Step 10.a. Return RejectPromise(promise, then.[[Value]]).
    return RejectMaybeWrappedPromise(cx, promise, error, errorStack);
  }

  // Step 11. Let thenAction be then.[[Value]].
  // (implicit)

  // Step 12. If IsCallable(thenAction) is false, then
  if (!IsCallable(thenVal)) {
    // Step 12.a. Return FulfillPromise(promise, resolution).
    return FulfillMaybeWrappedPromise(cx, promise, resolutionVal);
  }

  // Step 13. Let thenJobCallback be HostMakeJobCallback(thenAction).
  // (implicit)

  // Step 14. Let job be
  //          NewPromiseResolveThenableJob(promise, resolution,
  //                                       thenJobCallback).
  // Step 15. Perform HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).

  // If the resolution object is a built-in Promise object and the
  // `then` property is the original Promise.prototype.then function
  // from the current realm, we skip storing/calling it.
  // Additionally we require that |promise| itself is also a built-in
  // Promise object, so the fast path doesn't need to cope with wrappers.
  bool isBuiltinThen = false;
  if (resolution->is<PromiseObject>() && promise->is<PromiseObject>() &&
      IsNativeFunction(thenVal, Promise_then) &&
      thenVal.toObject().as<JSFunction>().realm() == cx->realm()) {
    isBuiltinThen = true;
  }

  if (!isBuiltinThen) {
    RootedValue promiseVal(cx, ObjectValue(*promise));
    if (!EnqueuePromiseResolveThenableJob(cx, promiseVal, resolutionVal,
                                          thenVal)) {
      return false;
    }
  } else {
    if (!EnqueuePromiseResolveThenableBuiltinJob(cx, promise, resolution)) {
      return false;
    }
  }

  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise Resolve Functions
 * https://tc39.es/ecma262/#sec-promise-resolve-functions
 */
static bool ResolvePromiseFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Let F be the active function object.
  // Step 2. Assert: F has a [[Promise]] internal slot whose value is an Object.
  // (implicit)

  JSFunction* resolve = &args.callee().as<JSFunction>();
  HandleValue resolutionVal = args.get(0);

  // Step 3. Let promise be F.[[Promise]].
  const Value& promiseVal =
      resolve->getExtendedSlot(ResolveFunctionSlot_Promise);

  // Step 4. Let alreadyResolved be F.[[AlreadyResolved]].
  // Step 5. If alreadyResolved.[[Value]] is true, return undefined.
  //
  // NOTE: We use the reference to the reject function as [[AlreadyResolved]].
  bool alreadyResolved = promiseVal.isUndefined();
  MOZ_ASSERT(IsAlreadyResolvedMaybeWrappedResolveFunction(resolve) ==
             alreadyResolved);
  if (alreadyResolved) {
    args.rval().setUndefined();
    return true;
  }

  RootedObject promise(cx, &promiseVal.toObject());

  // Step 6. Set alreadyResolved.[[Value]] to true.
  SetAlreadyResolvedResolutionFunction(resolve);

  // In some cases the Promise reference on the resolution function won't
  // have been removed during resolution, so we need to check that here,
  // too.
  if (IsSettledMaybeWrappedPromise(promise)) {
    args.rval().setUndefined();
    return true;
  }

  // Steps 7-15.
  if (!ResolvePromiseInternal(cx, promise, resolutionVal)) {
    return false;
  }

  // Step 16. Return undefined.
  args.rval().setUndefined();
  return true;
}

static bool PromiseReactionJob(JSContext* cx, unsigned argc, Value* vp);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * NewPromiseReactionJob ( reaction, argument )
 * https://tc39.es/ecma262/#sec-newpromisereactionjob
 * HostEnqueuePromiseJob ( job, realm )
 * https://tc39.es/ecma262/#sec-hostenqueuepromisejob
 *
 * Tells the embedding to enqueue a Promise reaction job, based on
 * three parameters:
 * reactionObj - The reaction record.
 * handlerArg_ - The first and only argument to pass to the handler invoked by
 *              the job. This will be stored on the reaction record.
 * targetState - The PromiseState this reaction job targets. This decides
 *               whether the onFulfilled or onRejected handler is called.
 */
[[nodiscard]] static bool EnqueuePromiseReactionJob(
    JSContext* cx, HandleObject reactionObj, HandleValue handlerArg_,
    JS::PromiseState targetState) {
  MOZ_ASSERT(targetState == JS::PromiseState::Fulfilled ||
             targetState == JS::PromiseState::Rejected);

  // The reaction might have been stored on a Promise from another
  // compartment, which means it would've been wrapped in a CCW.
  // To properly handle that case here, unwrap it and enter its
  // compartment, where the job creation should take place anyway.
  Rooted<PromiseReactionRecord*> reaction(cx);
  RootedValue handlerArg(cx, handlerArg_);
  mozilla::Maybe<AutoRealm> ar;
  if (!IsProxy(reactionObj)) {
    MOZ_RELEASE_ASSERT(reactionObj->is<PromiseReactionRecord>());
    reaction = &reactionObj->as<PromiseReactionRecord>();
    if (cx->realm() != reaction->realm()) {
      // If the compartment has multiple realms, create the job in the
      // reaction's realm. This is consistent with the code in the else-branch
      // and avoids problems with running jobs against a dying global (Gecko
      // drops such jobs).
      ar.emplace(cx, reaction);
    }
  } else {
    JSObject* unwrappedReactionObj = UncheckedUnwrap(reactionObj);
    if (JS_IsDeadWrapper(unwrappedReactionObj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }
    reaction = &unwrappedReactionObj->as<PromiseReactionRecord>();
    MOZ_RELEASE_ASSERT(reaction->is<PromiseReactionRecord>());
    ar.emplace(cx, reaction);
    if (!cx->compartment()->wrap(cx, &handlerArg)) {
      return false;
    }
  }

  // Must not enqueue a reaction job more than once.
  MOZ_ASSERT(reaction->targetState() == JS::PromiseState::Pending);

  // NOTE: Instead of capturing reaction and arguments separately in the
  //       Job Abstract Closure below, store arguments (= handlerArg) in
  //       reaction object and capture it.
  //       Also, set reaction.[[Type]] is represented by targetState here.
  cx->check(handlerArg);
  reaction->setTargetStateAndHandlerArg(targetState, handlerArg);

  RootedValue reactionVal(cx, ObjectValue(*reaction));
  RootedValue handler(cx, reaction->handler());

  // NewPromiseReactionJob
  // Step 2. Let handlerRealm be null.
  // NOTE: Instead of passing job and realm separately, we use the job's
  //       JSFunction object's realm as the job's realm.
  //       So we should enter the handlerRealm before creating the job function.
  //
  // GetFunctionRealm performed inside AutoFunctionOrCurrentRealm uses checked
  // unwrap and it can hit permission error if there's a security wrapper, and
  // in that case the reaction job is created in the current realm, instead of
  // the target function's realm.
  //
  // If this reaction crosses chrome/content boundary, and the security
  // wrapper would allow "call" operation, it still works inside the
  // reaction job.
  //
  // This behavior is observable only when the job belonging to the content
  // realm stops working (*1, *2), and it won't matter in practice.
  //
  // *1: "we can run script" performed inside HostEnqueuePromiseJob
  //     in HTML spec
  //       https://html.spec.whatwg.org/#hostenqueuepromisejob
  //       https://html.spec.whatwg.org/#check-if-we-can-run-script
  //       https://html.spec.whatwg.org/#fully-active
  // *2: nsIGlobalObject::IsDying performed inside PromiseJobRunnable::Run
  //     in our implementation
  mozilla::Maybe<AutoFunctionOrCurrentRealm> ar2;

  // NewPromiseReactionJob
  // Step 3. If reaction.[[Handler]] is not empty, then
  if (handler.isObject()) {
    // Step 3.a. Let getHandlerRealmResult be
    //           GetFunctionRealm(reaction.[[Handler]].[[Callback]]).
    // Step 3.b. If getHandlerRealmResult is a normal completion,
    //           set handlerRealm to getHandlerRealmResult.[[Value]].
    // Step 3.c. Else, set handlerRealm to the current Realm Record.
    // Step 3.d. NOTE: handlerRealm is never null unless the handler is
    //           undefined. When the handler is a revoked Proxy and no
    //           ECMAScript code runs, handlerRealm is used to create error
    //           objects.
    RootedObject handlerObj(cx, &handler.toObject());
    ar2.emplace(cx, handlerObj);

    // We need to wrap the reaction to store it on the job function.
    if (!cx->compartment()->wrap(cx, &reactionVal)) {
      return false;
    }
  }

  // NewPromiseReactionJob
  // Step 1. Let job be a new Job Abstract Closure with no parameters that
  //         captures reaction and argument and performs the following steps
  //         when called:
  Handle<PropertyName*> funName = cx->names().empty;
  RootedFunction job(
      cx, NewNativeFunction(cx, PromiseReactionJob, 0, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!job) {
    return false;
  }

  job->setExtendedSlot(ReactionJobSlot_ReactionRecord, reactionVal);

  // When using JS::AddPromiseReactions{,IgnoringUnHandledRejection}, no actual
  // promise is created, so we might not have one here.
  // Additionally, we might have an object here that isn't an instance of
  // Promise. This can happen if content overrides the value of
  // Promise[@@species] (or invokes Promise#then on a Promise subclass
  // instance with a non-default @@species value on the constructor) with a
  // function that returns objects that're not Promise (subclass) instances.
  // In that case, we just pretend we didn't have an object in the first
  // place.
  // If after all this we do have an object, wrap it in case we entered the
  // handler's compartment above, because we should pass objects from a
  // single compartment to the enqueuePromiseJob callback.
  RootedObject promise(cx, reaction->promise());
  if (promise) {
    if (promise->is<PromiseObject>()) {
      if (!cx->compartment()->wrap(cx, &promise)) {
        return false;
      }
    } else if (IsWrapper(promise)) {
      // `promise` can be already-wrapped promise object at this point.
      JSObject* unwrappedPromise = UncheckedUnwrap(promise);
      if (unwrappedPromise->is<PromiseObject>()) {
        if (!cx->compartment()->wrap(cx, &promise)) {
          return false;
        }
      } else {
        promise = nullptr;
      }
    } else {
      promise = nullptr;
    }
  }

  // Using objectFromIncumbentGlobal, we can derive the incumbent global by
  // unwrapping and then getting the global. This is very convoluted, but
  // much better than having to store the original global as a private value
  // because we couldn't wrap it to store it as a normal JS value.
  Rooted<GlobalObject*> global(cx);
  if (JSObject* objectFromIncumbentGlobal =
          reaction->getAndClearIncumbentGlobalObject()) {
    objectFromIncumbentGlobal = CheckedUnwrapStatic(objectFromIncumbentGlobal);
    MOZ_ASSERT(objectFromIncumbentGlobal);
    global = &objectFromIncumbentGlobal->nonCCWGlobal();
  }

  // HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).
  //
  // Note: the global we pass here might be from a different compartment
  // than job and promise. While it's somewhat unusual to pass objects
  // from multiple compartments, in this case we specifically need the
  // global to be unwrapped because wrapping and unwrapping aren't
  // necessarily symmetric for globals.
  return cx->runtime()->enqueuePromiseJob(cx, job, promise, global);
}

[[nodiscard]] static bool TriggerPromiseReactions(JSContext* cx,
                                                  HandleValue reactionsVal,
                                                  JS::PromiseState state,
                                                  HandleValue valueOrReason);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * FulfillPromise ( promise, value )
 * https://tc39.es/ecma262/#sec-fulfillpromise
 * RejectPromise ( promise, reason )
 * https://tc39.es/ecma262/#sec-rejectpromise
 *
 * This method takes an additional optional |unwrappedRejectionStack| parameter,
 * which is only used for debugging purposes.
 * It allows callers to to pass in the stack of some exception which
 * triggered the rejection of the promise.
 */
[[nodiscard]] static bool ResolvePromise(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue valueOrReason,
    JS::PromiseState state,
    Handle<SavedFrame*> unwrappedRejectionStack = nullptr) {
  // Step 1. Assert: The value of promise.[[PromiseState]] is pending.
  MOZ_ASSERT(promise->state() == JS::PromiseState::Pending);
  MOZ_ASSERT(state == JS::PromiseState::Fulfilled ||
             state == JS::PromiseState::Rejected);
  MOZ_ASSERT_IF(unwrappedRejectionStack, state == JS::PromiseState::Rejected);

  // FulfillPromise
  // Step 2. Let reactions be promise.[[PromiseFulfillReactions]].
  // RejectPromise
  // Step 2. Let reactions be promise.[[PromiseRejectReactions]].
  //
  // We only have one list of reactions for both resolution types. So
  // instead of getting the right list of reactions, we determine the
  // resolution type to retrieve the right information from the
  // reaction records.
  RootedValue reactionsVal(cx, promise->reactions());

  // FulfillPromise
  // Step 3. Set promise.[[PromiseResult]] to value.
  // RejectPromise
  // Step 3. Set promise.[[PromiseResult]] to reason.
  //
  // Step 4. Set promise.[[PromiseFulfillReactions]] to undefined.
  // Step 5. Set promise.[[PromiseRejectReactions]] to undefined.
  //
  // The same slot is used for the reactions list and the result, so setting
  // the result also removes the reactions list.
  promise->setFixedSlot(PromiseSlot_ReactionsOrResult, valueOrReason);

  // FulfillPromise
  // Step 6. Set promise.[[PromiseState]] to fulfilled.
  // RejectPromise
  // Step 6. Set promise.[[PromiseState]] to rejected.
  int32_t flags = promise->flags();
  flags |= PROMISE_FLAG_RESOLVED;
  if (state == JS::PromiseState::Fulfilled) {
    flags |= PROMISE_FLAG_FULFILLED;
  }
  promise->setFixedSlot(PromiseSlot_Flags, Int32Value(flags));

  // Also null out the resolve/reject functions so they can be GC'd.
  promise->setFixedSlot(PromiseSlot_RejectFunction, UndefinedValue());

  // Now that everything else is done, do the things the debugger needs.

  // RejectPromise
  // Step 7. If promise.[[PromiseIsHandled]] is false, perform
  //         HostPromiseRejectionTracker(promise, "reject").
  PromiseObject::onSettled(cx, promise, unwrappedRejectionStack);

  // FulfillPromise
  // Step 7. Return TriggerPromiseReactions(reactions, value).
  // RejectPromise
  // Step 8. Return TriggerPromiseReactions(reactions, reason).
  return TriggerPromiseReactions(cx, reactionsVal, state, valueOrReason);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * RejectPromise ( promise, reason )
 * https://tc39.es/ecma262/#sec-rejectpromise
 */
[[nodiscard]] bool js::RejectPromiseInternal(
    JSContext* cx, JS::Handle<PromiseObject*> promise,
    JS::Handle<JS::Value> reason,
    JS::Handle<SavedFrame*> unwrappedRejectionStack /* = nullptr */) {
  return ResolvePromise(cx, promise, reason, JS::PromiseState::Rejected,
                        unwrappedRejectionStack);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * FulfillPromise ( promise, value )
 * https://tc39.es/ecma262/#sec-fulfillpromise
 */
[[nodiscard]] static bool FulfillMaybeWrappedPromise(JSContext* cx,
                                                     HandleObject promiseObj,
                                                     HandleValue value_) {
  Rooted<PromiseObject*> promise(cx);
  RootedValue value(cx, value_);

  mozilla::Maybe<AutoRealm> ar;
  if (!IsProxy(promiseObj)) {
    promise = &promiseObj->as<PromiseObject>();
  } else {
    JSObject* unwrappedPromiseObj = UncheckedUnwrap(promiseObj);
    if (JS_IsDeadWrapper(unwrappedPromiseObj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }
    promise = &unwrappedPromiseObj->as<PromiseObject>();
    ar.emplace(cx, promise);
    if (!cx->compartment()->wrap(cx, &value)) {
      return false;
    }
  }

  return ResolvePromise(cx, promise, value, JS::PromiseState::Fulfilled);
}

static bool GetCapabilitiesExecutor(JSContext* cx, unsigned argc, Value* vp);
static bool PromiseConstructor(JSContext* cx, unsigned argc, Value* vp);
[[nodiscard]] static PromiseObject* CreatePromiseObjectInternal(
    JSContext* cx, HandleObject proto = nullptr, bool protoIsWrapped = false,
    bool informDebugger = true);

enum GetCapabilitiesExecutorSlots {
  GetCapabilitiesExecutorSlots_Resolve,
  GetCapabilitiesExecutorSlots_Reject
};

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise ( executor )
 * https://tc39.es/ecma262/#sec-promise-executor
 */
[[nodiscard]] static PromiseObject*
CreatePromiseObjectWithoutResolutionFunctions(JSContext* cx) {
  // Steps 3-7.
  PromiseObject* promise = CreatePromiseObjectInternal(cx);
  if (!promise) {
    return nullptr;
  }

  AddPromiseFlags(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS);

  // Step 11. Return promise.
  return promise;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise ( executor )
 * https://tc39.es/ecma262/#sec-promise-executor
 *
 * As if called with GetCapabilitiesExecutor as the executor argument.
 */
[[nodiscard]] static PromiseObject* CreatePromiseWithDefaultResolutionFunctions(
    JSContext* cx, MutableHandleObject resolve, MutableHandleObject reject) {
  // Steps 3-7.
  Rooted<PromiseObject*> promise(cx, CreatePromiseObjectInternal(cx));
  if (!promise) {
    return nullptr;
  }

  // Step 8. Let resolvingFunctions be CreateResolvingFunctions(promise).
  if (!CreateResolvingFunctions(cx, promise, resolve, reject)) {
    return nullptr;
  }

  promise->setFixedSlot(PromiseSlot_RejectFunction, ObjectValue(*reject));

  // Step 11. Return promise.
  return promise;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * NewPromiseCapability ( C )
 * https://tc39.es/ecma262/#sec-newpromisecapability
 */
[[nodiscard]] static bool NewPromiseCapability(
    JSContext* cx, HandleObject C, MutableHandle<PromiseCapability> capability,
    bool canOmitResolutionFunctions) {
  RootedValue cVal(cx, ObjectValue(*C));

  // Step 1. If IsConstructor(C) is false, throw a TypeError exception.
  // Step 2. NOTE: C is assumed to be a constructor function that supports the
  // parameter conventions of the Promise constructor (see 27.2.3.1).
  if (!IsConstructor(C)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_SEARCH_STACK, cVal,
                     nullptr);
    return false;
  }

  // If we'd call the original Promise constructor and know that the
  // resolve/reject functions won't ever escape to content, we can skip
  // creating and calling the executor function and instead return a Promise
  // marked as having default resolve/reject functions.
  //
  // This can't be used in Promise.all and Promise.race because we have to
  // pass the reject (and resolve, in the race case) function to thenables
  // in the list passed to all/race, which (potentially) means exposing them
  // to content.
  //
  // For Promise.all and Promise.race we can only optimize away the creation
  // of the GetCapabilitiesExecutor function, and directly allocate the
  // result promise instead of invoking the Promise constructor.
  if (IsNativeFunction(cVal, PromiseConstructor) &&
      cVal.toObject().nonCCWRealm() == cx->realm()) {
    PromiseObject* promise;
    if (canOmitResolutionFunctions) {
      promise = CreatePromiseObjectWithoutResolutionFunctions(cx);
    } else {
      promise = CreatePromiseWithDefaultResolutionFunctions(
          cx, capability.resolve(), capability.reject());
    }
    if (!promise) {
      return false;
    }

    // Step 3. Let promiseCapability be the PromiseCapability Record
    //         { [[Promise]]: undefined, [[Resolve]]: undefined,
    //           [[Reject]]: undefined }.
    capability.promise().set(promise);

    // Step 10. Return promiseCapability.
    return true;
  }

  // Step 4. Let executorClosure be a new Abstract Closure with parameters
  //         (resolve, reject) that captures promiseCapability and performs the
  //         following steps when called:
  Handle<PropertyName*> funName = cx->names().empty;
  RootedFunction executor(
      cx, NewNativeFunction(cx, GetCapabilitiesExecutor, 2, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!executor) {
    return false;
  }

  // Step 5. Let executor be
  //         ! CreateBuiltinFunction(executorClosure, 2, "", « »).
  // (omitted)

  // Step 6. Let promise be ? Construct(C, « executor »).
  // Step 9. Set promiseCapability.[[Promise]] to promise.
  FixedConstructArgs<1> cargs(cx);
  cargs[0].setObject(*executor);
  if (!Construct(cx, cVal, cargs, cVal, capability.promise())) {
    return false;
  }

  // Step 7. If IsCallable(promiseCapability.[[Resolve]]) is false,
  //         throw a TypeError exception.
  const Value& resolveVal =
      executor->getExtendedSlot(GetCapabilitiesExecutorSlots_Resolve);
  if (!IsCallable(resolveVal)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROMISE_RESOLVE_FUNCTION_NOT_CALLABLE);
    return false;
  }

  // Step 8. If IsCallable(promiseCapability.[[Reject]]) is false,
  //         throw a TypeError exception.
  const Value& rejectVal =
      executor->getExtendedSlot(GetCapabilitiesExecutorSlots_Reject);
  if (!IsCallable(rejectVal)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROMISE_REJECT_FUNCTION_NOT_CALLABLE);
    return false;
  }

  // (reordered)
  // Step 3. Let promiseCapability be the PromiseCapability Record
  //         { [[Promise]]: undefined, [[Resolve]]: undefined,
  //           [[Reject]]: undefined }.
  capability.resolve().set(&resolveVal.toObject());
  capability.reject().set(&rejectVal.toObject());

  // Step 10. Return promiseCapability.
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * NewPromiseCapability ( C )
 * https://tc39.es/ecma262/#sec-newpromisecapability
 *
 * Steps 4.a-e.
 */
static bool GetCapabilitiesExecutor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSFunction* F = &args.callee().as<JSFunction>();

  // Step 4.a. If promiseCapability.[[Resolve]] is not undefined,
  //           throw a TypeError exception.
  // Step 4.b. If promiseCapability.[[Reject]] is not undefined,
  //           throw a TypeError exception.
  if (!F->getExtendedSlot(GetCapabilitiesExecutorSlots_Resolve).isUndefined() ||
      !F->getExtendedSlot(GetCapabilitiesExecutorSlots_Reject).isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROMISE_CAPABILITY_HAS_SOMETHING_ALREADY);
    return false;
  }

  // Step 4.c. Set promiseCapability.[[Resolve]] to resolve.
  F->setExtendedSlot(GetCapabilitiesExecutorSlots_Resolve, args.get(0));

  // Step 4.d. Set promiseCapability.[[Reject]] to reject.
  F->setExtendedSlot(GetCapabilitiesExecutorSlots_Reject, args.get(1));

  // Step 4.e. Return undefined.
  args.rval().setUndefined();
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * RejectPromise ( promise, reason )
 * https://tc39.es/ecma262/#sec-rejectpromise
 */
[[nodiscard]] static bool RejectMaybeWrappedPromise(
    JSContext* cx, HandleObject promiseObj, HandleValue reason_,
    Handle<SavedFrame*> unwrappedRejectionStack) {
  Rooted<PromiseObject*> promise(cx);
  RootedValue reason(cx, reason_);

  mozilla::Maybe<AutoRealm> ar;
  if (!IsProxy(promiseObj)) {
    promise = &promiseObj->as<PromiseObject>();
  } else {
    JSObject* unwrappedPromiseObj = UncheckedUnwrap(promiseObj);
    if (JS_IsDeadWrapper(unwrappedPromiseObj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }
    promise = &unwrappedPromiseObj->as<PromiseObject>();
    ar.emplace(cx, promise);

    // The rejection reason might've been created in a compartment with higher
    // privileges than the Promise's. In that case, object-type rejection
    // values might be wrapped into a wrapper that throws whenever the
    // Promise's reaction handler wants to do anything useful with it. To
    // avoid that situation, we synthesize a generic error that doesn't
    // expose any privileged information but can safely be used in the
    // rejection handler.
    if (!cx->compartment()->wrap(cx, &reason)) {
      return false;
    }
    if (reason.isObject() && !CheckedUnwrapStatic(&reason.toObject())) {
      // Report the existing reason, so we don't just drop it on the
      // floor.
      JSObject* realReason = UncheckedUnwrap(&reason.toObject());
      RootedValue realReasonVal(cx, ObjectValue(*realReason));
      Rooted<GlobalObject*> realGlobal(cx, &realReason->nonCCWGlobal());
      ReportErrorToGlobal(cx, realGlobal, realReasonVal);

      // Async stacks are only properly adopted if there's at least one
      // interpreter frame active right now. If a thenable job with a
      // throwing `then` function got us here, that'll not be the case,
      // so we add one by throwing the error from self-hosted code.
      if (!GetInternalError(cx, JSMSG_PROMISE_ERROR_IN_WRAPPED_REJECTION_REASON,
                            &reason)) {
        return false;
      }
    }
  }

  return ResolvePromise(cx, promise, reason, JS::PromiseState::Rejected,
                        unwrappedRejectionStack);
}

// Apply f to a mutable handle on each member of a collection of reactions, like
// that stored in PromiseSlot_ReactionsOrResult on a pending promise. When the
// reaction record is wrapped, we pass the wrapper, without dereferencing it. If
// f returns false, then we stop the iteration immediately and return false.
// Otherwise, we return true.
//
// There are several different representations for collections:
//
// - We represent an empty collection of reactions as an 'undefined' value.
//
// - We represent a collection containing a single reaction simply as the given
//   PromiseReactionRecord object, possibly wrapped.
//
// - We represent a collection of two or more reactions as a dense array of
//   possibly-wrapped PromiseReactionRecords.
//
template <typename F>
static bool ForEachReaction(JSContext* cx, HandleValue reactionsVal, F f) {
  if (reactionsVal.isUndefined()) {
    return true;
  }

  RootedObject reactions(cx, &reactionsVal.toObject());
  RootedObject reaction(cx);

  if (reactions->is<PromiseReactionRecord>() || IsWrapper(reactions) ||
      JS_IsDeadWrapper(reactions)) {
    return f(&reactions);
  }

  Handle<NativeObject*> reactionsList = reactions.as<NativeObject>();
  uint32_t reactionsCount = reactionsList->getDenseInitializedLength();
  MOZ_ASSERT(reactionsCount > 1, "Reactions list should be created lazily");

  for (uint32_t i = 0; i < reactionsCount; i++) {
    const Value& reactionVal = reactionsList->getDenseElement(i);
    MOZ_RELEASE_ASSERT(reactionVal.isObject());
    reaction = &reactionVal.toObject();
    if (!f(&reaction)) {
      return false;
    }
  }

  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * TriggerPromiseReactions ( reactions, argument )
 * https://tc39.es/ecma262/#sec-triggerpromisereactions
 */
[[nodiscard]] static bool TriggerPromiseReactions(JSContext* cx,
                                                  HandleValue reactionsVal,
                                                  JS::PromiseState state,
                                                  HandleValue valueOrReason) {
  MOZ_ASSERT(state == JS::PromiseState::Fulfilled ||
             state == JS::PromiseState::Rejected);

  // Step 1. For each element reaction of reactions, do
  // Step 2. Return undefined.
  return ForEachReaction(cx, reactionsVal, [&](MutableHandleObject reaction) {
    // Step 1.a. Let job be NewPromiseReactionJob(reaction, argument).
    // Step 1.b. Perform HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).
    return EnqueuePromiseReactionJob(cx, reaction, valueOrReason, state);
  });
}

[[nodiscard]] static bool CallPromiseResolveFunction(JSContext* cx,
                                                     HandleObject resolveFun,
                                                     HandleValue value,
                                                     HandleObject promiseObj);

/**
 * ES2023 draft rev 714fa3dd1e8237ae9c666146270f81880089eca5
 *
 * NewPromiseReactionJob ( reaction, argument )
 * https://tc39.es/ecma262/#sec-newpromisereactionjob
 *
 * Step 1.
 *
 * Implements PromiseReactionJob optimized for the case when the reaction
 * handler is one of the default resolving functions as created by the
 * CreateResolvingFunctions abstract operation.
 */
[[nodiscard]] static bool DefaultResolvingPromiseReactionJob(
    JSContext* cx, Handle<PromiseReactionRecord*> reaction) {
  MOZ_ASSERT(reaction->targetState() != JS::PromiseState::Pending);

  Rooted<PromiseObject*> promiseToResolve(cx,
                                          reaction->defaultResolvingPromise());

  // Testing functions allow to directly settle a promise without going
  // through the resolving functions. In that case the normal bookkeeping to
  // ensure only pending promises can be resolved doesn't apply and we need
  // to manually check for already settled promises. We still call
  // Run{Fulfill,Reject}Function for consistency with PromiseReactionJob.
  ResolutionMode resolutionMode = ResolveMode;
  RootedValue handlerResult(cx, UndefinedValue());
  Rooted<SavedFrame*> unwrappedRejectionStack(cx);
  if (promiseToResolve->state() == JS::PromiseState::Pending) {
    RootedValue argument(cx, reaction->handlerArg());

    // Step 1.e. Else, let handlerResult be
    //           Completion(HostCallJobCallback(handler, undefined,
    //                                          « argument »)).
    bool ok;
    if (reaction->targetState() == JS::PromiseState::Fulfilled) {
      ok = ResolvePromiseInternal(cx, promiseToResolve, argument);
    } else {
      ok = RejectPromiseInternal(cx, promiseToResolve, argument);
    }

    if (!ok) {
      resolutionMode = RejectMode;
      if (!MaybeGetAndClearExceptionAndStack(cx, &handlerResult,
                                             &unwrappedRejectionStack)) {
        return false;
      }
    }
  }

  // Steps 1.f-i.
  RootedObject promiseObj(cx, reaction->promise());
  RootedObject callee(cx);
  if (resolutionMode == ResolveMode) {
    callee =
        reaction->getFixedSlot(ReactionRecordSlot_Resolve).toObjectOrNull();

    return CallPromiseResolveFunction(cx, callee, handlerResult, promiseObj);
  }

  callee = reaction->getFixedSlot(ReactionRecordSlot_Reject).toObjectOrNull();

  return CallPromiseRejectFunction(cx, callee, handlerResult, promiseObj,
                                   unwrappedRejectionStack,
                                   reaction->unhandledRejectionBehavior());
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Await in async function
 * https://tc39.es/ecma262/#await
 *
 * Step 3. fulfilledClosure Abstract Closure.
 * Step 5. rejectedClosure Abstract Closure.
 */
[[nodiscard]] static bool AsyncFunctionPromiseReactionJob(
    JSContext* cx, Handle<PromiseReactionRecord*> reaction) {
  MOZ_ASSERT(reaction->isAsyncFunction());

  auto handler = static_cast<PromiseHandler>(reaction->handler().toInt32());
  RootedValue argument(cx, reaction->handlerArg());
  Rooted<AsyncFunctionGeneratorObject*> generator(
      cx, reaction->asyncFunctionGenerator());

  // Await's handlers don't return a value, nor throw any exceptions.
  // They fail only on OOM.

  if (handler == PromiseHandler::AsyncFunctionAwaitedFulfilled) {
    // Step 3. fulfilledClosure Abstract Closure.
    return AsyncFunctionAwaitedFulfilled(cx, generator, argument);
  }

  // Step 5. rejectedClosure Abstract Closure.
  MOZ_ASSERT(handler == PromiseHandler::AsyncFunctionAwaitedRejected);
  return AsyncFunctionAwaitedRejected(cx, generator, argument);
}

/**
 * ES2023 draft rev 714fa3dd1e8237ae9c666146270f81880089eca5
 *
 * NewPromiseReactionJob ( reaction, argument )
 * https://tc39.es/ecma262/#sec-newpromisereactionjob
 *
 * Step 1.
 *
 * Callback triggering the fulfill/reject reaction for a resolved Promise,
 * to be invoked by the embedding during its processing of the Promise job
 * queue.
 *
 * A PromiseReactionJob is set as the native function of an extended
 * JSFunction object, with all information required for the job's
 * execution stored in in a reaction record in its first extended slot.
 */
static bool PromiseReactionJob(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedFunction job(cx, &args.callee().as<JSFunction>());

  // Promise reactions don't return any value.
  args.rval().setUndefined();

  RootedObject reactionObj(
      cx, &job->getExtendedSlot(ReactionJobSlot_ReactionRecord).toObject());

  // To ensure that the embedding ends up with the right entry global, we're
  // guaranteeing that the reaction job function gets created in the same
  // compartment as the handler function. That's not necessarily the global
  // that the job was triggered from, though.
  // We can find the triggering global via the job's reaction record. To go
  // back, we check if the reaction is a wrapper and if so, unwrap it and
  // enter its compartment.
  mozilla::Maybe<AutoRealm> ar;
  if (!IsProxy(reactionObj)) {
    MOZ_RELEASE_ASSERT(reactionObj->is<PromiseReactionRecord>());
  } else {
    reactionObj = UncheckedUnwrap(reactionObj);
    if (JS_IsDeadWrapper(reactionObj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }
    MOZ_RELEASE_ASSERT(reactionObj->is<PromiseReactionRecord>());
    ar.emplace(cx, reactionObj);
  }

  // Optimized/special cases.
  Handle<PromiseReactionRecord*> reaction =
      reactionObj.as<PromiseReactionRecord>();
  if (reaction->isDefaultResolvingHandler()) {
    return DefaultResolvingPromiseReactionJob(cx, reaction);
  }
  if (reaction->isAsyncFunction()) {
    return AsyncFunctionPromiseReactionJob(cx, reaction);
  }
  if (reaction->isAsyncGenerator()) {
    RootedValue argument(cx, reaction->handlerArg());
    Rooted<AsyncGeneratorObject*> generator(cx, reaction->asyncGenerator());
    auto handler = static_cast<PromiseHandler>(reaction->handler().toInt32());
    return AsyncGeneratorPromiseReactionJob(cx, handler, generator, argument);
  }
  if (reaction->isDebuggerDummy()) {
    return true;
  }

  // Step 1.a. Let promiseCapability be reaction.[[Capability]].
  // (implicit)

  // Step 1.c. Let handler be reaction.[[Handler]].
  RootedValue handlerVal(cx, reaction->handler());

  RootedValue argument(cx, reaction->handlerArg());

  RootedValue handlerResult(cx);
  ResolutionMode resolutionMode = ResolveMode;

  Rooted<SavedFrame*> unwrappedRejectionStack(cx);

  // Step 1.d. If handler is empty, then
  if (handlerVal.isInt32()) {
    // Step 1.b. Let type be reaction.[[Type]].
    // (reordered)
    auto handlerNum = static_cast<PromiseHandler>(handlerVal.toInt32());

    // Step 1.d.i. If type is Fulfill, let handlerResult be
    //             NormalCompletion(argument).
    if (handlerNum == PromiseHandler::Identity) {
      handlerResult = argument;
    } else if (handlerNum == PromiseHandler::Thrower) {
      // Step 1.d.ii. Else,
      // Step 1.d.ii.1. Assert: type is Reject.
      // Step 1.d.ii.2. Let handlerResult be ThrowCompletion(argument).
      resolutionMode = RejectMode;
      handlerResult = argument;
    } else {
      // Special case for Async-from-Sync Iterator.

      MOZ_ASSERT(handlerNum ==
                     PromiseHandler::AsyncFromSyncIteratorValueUnwrapDone ||
                 handlerNum ==
                     PromiseHandler::AsyncFromSyncIteratorValueUnwrapNotDone);

      bool done =
          handlerNum == PromiseHandler::AsyncFromSyncIteratorValueUnwrapDone;
      // 25.1.4.2.5 Async-from-Sync Iterator Value Unwrap Functions, steps 1-2.
      PlainObject* resultObj = CreateIterResultObject(cx, argument, done);
      if (!resultObj) {
        return false;
      }

      handlerResult = ObjectValue(*resultObj);
    }
  } else {
    MOZ_ASSERT(handlerVal.isObject());
    MOZ_ASSERT(IsCallable(handlerVal));

    // Step 1.e. Else, let handlerResult be
    //           Completion(HostCallJobCallback(handler, undefined,
    //                                          « argument »)).
    if (!Call(cx, handlerVal, UndefinedHandleValue, argument, &handlerResult)) {
      resolutionMode = RejectMode;
      if (!MaybeGetAndClearExceptionAndStack(cx, &handlerResult,
                                             &unwrappedRejectionStack)) {
        return false;
      }
    }
  }

  // Steps 1.f-i.
  RootedObject promiseObj(cx, reaction->promise());
  RootedObject callee(cx);
  if (resolutionMode == ResolveMode) {
    callee =
        reaction->getFixedSlot(ReactionRecordSlot_Resolve).toObjectOrNull();

    return CallPromiseResolveFunction(cx, callee, handlerResult, promiseObj);
  }

  callee = reaction->getFixedSlot(ReactionRecordSlot_Reject).toObjectOrNull();

  return CallPromiseRejectFunction(cx, callee, handlerResult, promiseObj,
                                   unwrappedRejectionStack,
                                   reaction->unhandledRejectionBehavior());
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * NewPromiseResolveThenableJob ( promiseToResolve, thenable, then )
 * https://tc39.es/ecma262/#sec-newpromiseresolvethenablejob
 *
 * Steps 1.a-d.
 *
 * A PromiseResolveThenableJob is set as the native function of an extended
 * JSFunction object, with all information required for the job's
 * execution stored in the function's extended slots.
 *
 * Usage of the function's extended slots is described in the ThenableJobSlots
 * enum.
 */
static bool PromiseResolveThenableJob(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedFunction job(cx, &args.callee().as<JSFunction>());
  RootedValue then(cx, job->getExtendedSlot(ThenableJobSlot_Handler));
  MOZ_ASSERT(then.isObject());
  Rooted<NativeObject*> jobArgs(cx,
                                &job->getExtendedSlot(ThenableJobSlot_JobData)
                                     .toObject()
                                     .as<NativeObject>());

  RootedObject promise(
      cx, &jobArgs->getDenseElement(ThenableJobDataIndex_Promise).toObject());
  RootedValue thenable(cx,
                       jobArgs->getDenseElement(ThenableJobDataIndex_Thenable));

  // Step 1.a. Let resolvingFunctions be
  //           CreateResolvingFunctions(promiseToResolve).
  RootedObject resolveFn(cx);
  RootedObject rejectFn(cx);
  if (!CreateResolvingFunctions(cx, promise, &resolveFn, &rejectFn)) {
    return false;
  }

  // Step 1.b. Let thenCallResult be
  //           HostCallJobCallback(then, thenable,
  //                               « resolvingFunctions.[[Resolve]],
  //                                 resolvingFunctions.[[Reject]] »).
  FixedInvokeArgs<2> args2(cx);
  args2[0].setObject(*resolveFn);
  args2[1].setObject(*rejectFn);

  // In difference to the usual pattern, we return immediately on success.
  RootedValue rval(cx);
  if (Call(cx, then, thenable, args2, &rval)) {
    // Step 1.d. Return Completion(thenCallResult).
    return true;
  }

  // Step 1.c. If thenCallResult is an abrupt completion, then

  Rooted<SavedFrame*> stack(cx);
  if (!MaybeGetAndClearExceptionAndStack(cx, &rval, &stack)) {
    return false;
  }

  // Step 1.c.i. Let status be
  //             Call(resolvingFunctions.[[Reject]], undefined,
  //                  « thenCallResult.[[Value]] »).
  // Step 1.c.ii. Return Completion(status).
  RootedValue rejectVal(cx, ObjectValue(*rejectFn));
  return Call(cx, rejectVal, UndefinedHandleValue, rval, &rval);
}

[[nodiscard]] static bool OriginalPromiseThenWithoutSettleHandlers(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseObject*> promiseToResolve);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * NewPromiseResolveThenableJob ( promiseToResolve, thenable, then )
 * https://tc39.es/ecma262/#sec-newpromiseresolvethenablejob
 *
 * Step 1.a-d.
 *
 * Specialization of PromiseResolveThenableJob when the `thenable` is a
 * built-in Promise object and the `then` property is the built-in
 * `Promise.prototype.then` function.
 *
 * A PromiseResolveBuiltinThenableJob is set as the native function of an
 * extended JSFunction object, with all information required for the job's
 * execution stored in the function's extended slots.
 *
 * Usage of the function's extended slots is described in the
 * BuiltinThenableJobSlots enum.
 */
static bool PromiseResolveBuiltinThenableJob(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedFunction job(cx, &args.callee().as<JSFunction>());
  RootedObject promise(
      cx, &job->getExtendedSlot(BuiltinThenableJobSlot_Promise).toObject());
  RootedObject thenable(
      cx, &job->getExtendedSlot(BuiltinThenableJobSlot_Thenable).toObject());

  cx->check(promise, thenable);
  MOZ_ASSERT(promise->is<PromiseObject>());
  MOZ_ASSERT(thenable->is<PromiseObject>());

  // Step 1.a. Let resolvingFunctions be
  //           CreateResolvingFunctions(promiseToResolve).
  // (skipped)

  // Step 1.b. Let thenCallResult be HostCallJobCallback(
  //             then, thenable,
  //             « resolvingFunctions.[[Resolve]],
  //               resolvingFunctions.[[Reject]] »).
  //
  // NOTE: In difference to the usual pattern, we return immediately on success.
  if (OriginalPromiseThenWithoutSettleHandlers(cx, thenable.as<PromiseObject>(),
                                               promise.as<PromiseObject>())) {
    // Step 1.d. Return Completion(thenCallResult).
    return true;
  }

  // Step 1.c. If thenCallResult is an abrupt completion, then
  RootedValue exception(cx);
  Rooted<SavedFrame*> stack(cx);
  if (!MaybeGetAndClearExceptionAndStack(cx, &exception, &stack)) {
    return false;
  }

  // Testing functions allow to directly settle a promise without going
  // through the resolving functions. In that case the normal bookkeeping to
  // ensure only pending promises can be resolved doesn't apply and we need
  // to manually check for already settled promises. The exception is simply
  // dropped when this case happens.
  if (promise->as<PromiseObject>().state() != JS::PromiseState::Pending) {
    return true;
  }

  // Step 1.c.i. Let status be
  //             Call(resolvingFunctions.[[Reject]], undefined,
  //                  « thenCallResult.[[Value]] »).
  // Step 1.c.ii. Return Completion(status).
  return RejectPromiseInternal(cx, promise.as<PromiseObject>(), exception,
                               stack);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * NewPromiseResolveThenableJob ( promiseToResolve, thenable, then )
 * https://tc39.es/ecma262/#sec-newpromiseresolvethenablejob
 * HostEnqueuePromiseJob ( job, realm )
 * https://tc39.es/ecma262/#sec-hostenqueuepromisejob
 *
 * Tells the embedding to enqueue a Promise resolve thenable job, based on
 * three parameters:
 * promiseToResolve_ - The promise to resolve, obviously.
 * thenable_ - The thenable to resolve the Promise with.
 * thenVal - The `then` function to invoke with the `thenable` as the receiver.
 */
[[nodiscard]] static bool EnqueuePromiseResolveThenableJob(
    JSContext* cx, HandleValue promiseToResolve_, HandleValue thenable_,
    HandleValue thenVal) {
  // Need to re-root these to enable wrapping them below.
  RootedValue promiseToResolve(cx, promiseToResolve_);
  RootedValue thenable(cx, thenable_);

  // Step 2. Let getThenRealmResult be GetFunctionRealm(then.[[Callback]]).
  // Step 3. If getThenRealmResult is a normal completion, let thenRealm be
  //         getThenRealmResult.[[Value]].
  // Step 4. Else, let thenRealm be the current Realm Record.
  // Step 5. NOTE: thenRealm is never null. When then.[[Callback]] is a revoked
  //         Proxy and no code runs, thenRealm is used to create error objects.
  //
  // NOTE: Instead of passing job and realm separately, we use the job's
  //       JSFunction object's realm as the job's realm.
  //       So we should enter the thenRealm before creating the job function.
  //
  // GetFunctionRealm performed inside AutoFunctionOrCurrentRealm uses checked
  // unwrap and this is fine given the behavior difference (see the comment
  // around AutoFunctionOrCurrentRealm usage in EnqueuePromiseReactionJob for
  // more details) is observable only when the `thenable` is from content realm
  // and `then` is from chrome realm, that shouldn't happen in practice.
  //
  // NOTE: If `thenable` is also from chrome realm, accessing `then` silently
  //       fails and it returns `undefined`, and that case doesn't reach here.
  RootedObject then(cx, &thenVal.toObject());
  AutoFunctionOrCurrentRealm ar(cx, then);
  if (then->maybeCCWRealm() != cx->realm()) {
    if (!cx->compartment()->wrap(cx, &then)) {
      return false;
    }
  }

  // Wrap the `promiseToResolve` and `thenable` arguments.
  if (!cx->compartment()->wrap(cx, &promiseToResolve)) {
    return false;
  }

  MOZ_ASSERT(thenable.isObject());
  if (!cx->compartment()->wrap(cx, &thenable)) {
    return false;
  }

  // Step 1. Let job be a new Job Abstract Closure with no parameters that
  //         captures promiseToResolve, thenable, and then and performs the
  //         following steps when called:
  Handle<PropertyName*> funName = cx->names().empty;
  RootedFunction job(
      cx, NewNativeFunction(cx, PromiseResolveThenableJob, 0, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!job) {
    return false;
  }

  // Store the `then` function on the callback.
  job->setExtendedSlot(ThenableJobSlot_Handler, ObjectValue(*then));

  // Create a dense array to hold the data needed for the reaction job to
  // work.
  // The layout is described in the ThenableJobDataIndices enum.
  Rooted<ArrayObject*> data(
      cx, NewDenseFullyAllocatedArray(cx, ThenableJobDataLength));
  if (!data) {
    return false;
  }

  // Set the `promiseToResolve` and `thenable` arguments.
  data->setDenseInitializedLength(ThenableJobDataLength);
  data->initDenseElement(ThenableJobDataIndex_Promise, promiseToResolve);
  data->initDenseElement(ThenableJobDataIndex_Thenable, thenable);

  // Store the data array on the reaction job.
  job->setExtendedSlot(ThenableJobSlot_JobData, ObjectValue(*data));

  // At this point the promise is guaranteed to be wrapped into the job's
  // compartment.
  RootedObject promise(cx, &promiseToResolve.toObject());

  Rooted<GlobalObject*> incumbentGlobal(cx,
                                        cx->runtime()->getIncumbentGlobal(cx));

  // Step X. HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).
  return cx->runtime()->enqueuePromiseJob(cx, job, promise, incumbentGlobal);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * NewPromiseResolveThenableJob ( promiseToResolve, thenable, then )
 * https://tc39.es/ecma262/#sec-newpromiseresolvethenablejob
 * HostEnqueuePromiseJob ( job, realm )
 * https://tc39.es/ecma262/#sec-hostenqueuepromisejob
 *
 * Tells the embedding to enqueue a Promise resolve thenable built-in job,
 * based on two parameters:
 * promiseToResolve - The promise to resolve, obviously.
 * thenable - The thenable to resolve the Promise with.
 */
[[nodiscard]] static bool EnqueuePromiseResolveThenableBuiltinJob(
    JSContext* cx, HandleObject promiseToResolve, HandleObject thenable) {
  cx->check(promiseToResolve, thenable);
  MOZ_ASSERT(promiseToResolve->is<PromiseObject>());
  MOZ_ASSERT(thenable->is<PromiseObject>());

  // Step 1. Let job be a new Job Abstract Closure with no parameters that
  //         captures promiseToResolve, thenable, and then and performs the
  //         following steps when called:
  Handle<PropertyName*> funName = cx->names().empty;
  RootedFunction job(
      cx, NewNativeFunction(cx, PromiseResolveBuiltinThenableJob, 0, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!job) {
    return false;
  }

  // Steps 2-5.
  // (implicit)
  // `then` is built-in Promise.prototype.then in the current realm.,
  // thus `thenRealm` is also current realm, and we have nothing to do here.

  // Store the promise and the thenable on the reaction job.
  job->setExtendedSlot(BuiltinThenableJobSlot_Promise,
                       ObjectValue(*promiseToResolve));
  job->setExtendedSlot(BuiltinThenableJobSlot_Thenable, ObjectValue(*thenable));

  Rooted<GlobalObject*> incumbentGlobal(cx,
                                        cx->runtime()->getIncumbentGlobal(cx));

  // HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).
  return cx->runtime()->enqueuePromiseJob(cx, job, promiseToResolve,
                                          incumbentGlobal);
}

[[nodiscard]] static bool AddDummyPromiseReactionForDebugger(
    JSContext* cx, Handle<PromiseObject*> promise,
    HandleObject dependentPromise);

[[nodiscard]] static bool AddPromiseReaction(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseReactionRecord*> reaction);

static JSFunction* GetResolveFunctionFromReject(JSFunction* reject) {
  MOZ_ASSERT(reject->maybeNative() == RejectPromiseFunction);
  Value resolveFunVal =
      reject->getExtendedSlot(RejectFunctionSlot_ResolveFunction);
  MOZ_ASSERT(IsNativeFunction(resolveFunVal, ResolvePromiseFunction));
  return &resolveFunVal.toObject().as<JSFunction>();
}

static JSFunction* GetRejectFunctionFromResolve(JSFunction* resolve) {
  MOZ_ASSERT(resolve->maybeNative() == ResolvePromiseFunction);
  Value rejectFunVal =
      resolve->getExtendedSlot(ResolveFunctionSlot_RejectFunction);
  MOZ_ASSERT(IsNativeFunction(rejectFunVal, RejectPromiseFunction));
  return &rejectFunVal.toObject().as<JSFunction>();
}

static JSFunction* GetResolveFunctionFromPromise(PromiseObject* promise) {
  Value rejectFunVal = promise->getFixedSlot(PromiseSlot_RejectFunction);
  if (rejectFunVal.isUndefined()) {
    return nullptr;
  }
  JSObject* rejectFunObj = &rejectFunVal.toObject();

  // We can safely unwrap it because all we want is to get the resolve
  // function.
  if (IsWrapper(rejectFunObj)) {
    rejectFunObj = UncheckedUnwrap(rejectFunObj);
  }

  if (!rejectFunObj->is<JSFunction>()) {
    return nullptr;
  }

  JSFunction* rejectFun = &rejectFunObj->as<JSFunction>();

  // Only the original RejectPromiseFunction has a reference to the resolve
  // function.
  if (rejectFun->maybeNative() != &RejectPromiseFunction) {
    return nullptr;
  }

  // The reject function was already called and cleared its resolve-function
  // extended slot.
  if (rejectFun->getExtendedSlot(RejectFunctionSlot_ResolveFunction)
          .isUndefined()) {
    return nullptr;
  }

  return GetResolveFunctionFromReject(rejectFun);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise ( executor )
 * https://tc39.es/ecma262/#sec-promise-executor
 *
 * Steps 3-7.
 */
[[nodiscard]] static MOZ_ALWAYS_INLINE PromiseObject*
CreatePromiseObjectInternal(JSContext* cx, HandleObject proto /* = nullptr */,
                            bool protoIsWrapped /* = false */,
                            bool informDebugger /* = true */) {
  // Enter the unwrapped proto's compartment, if that's different from
  // the current one.
  // All state stored in a Promise's fixed slots must be created in the
  // same compartment, so we get all of that out of the way here.
  // (Except for the resolution functions, which are created below.)
  mozilla::Maybe<AutoRealm> ar;
  if (protoIsWrapped) {
    ar.emplace(cx, proto);
  }

  // Step 3. Let promise be
  //         ? OrdinaryCreateFromConstructor(
  //             NewTarget, "%Promise.prototype%",
  //             « [[PromiseState]], [[PromiseResult]],
  //               [[PromiseFulfillReactions]], [[PromiseRejectReactions]],
  //               [[PromiseIsHandled]] »).
  PromiseObject* promise = NewObjectWithClassProto<PromiseObject>(cx, proto);
  if (!promise) {
    return nullptr;
  }

  // Step 4. Set promise.[[PromiseState]] to pending.
  promise->initFixedSlot(PromiseSlot_Flags, Int32Value(0));

  // Step 5. Set promise.[[PromiseFulfillReactions]] to a new empty List.
  // Step 6. Set promise.[[PromiseRejectReactions]] to a new empty List.
  // (omitted)
  // We allocate our single list of reaction records lazily.

  // Step 7. Set promise.[[PromiseIsHandled]] to false.
  // (implicit)
  // The handled flag is unset by default.

  if (MOZ_LIKELY(!JS::IsAsyncStackCaptureEnabledForRealm(cx))) {
    return promise;
  }

  // Store an allocation stack so we can later figure out what the
  // control flow was for some unexpected results. Frightfully expensive,
  // but oh well.

  Rooted<PromiseObject*> promiseRoot(cx, promise);

  PromiseDebugInfo* debugInfo = PromiseDebugInfo::create(cx, promiseRoot);
  if (!debugInfo) {
    return nullptr;
  }

  // Let the Debugger know about this Promise.
  if (informDebugger) {
    DebugAPI::onNewPromise(cx, promiseRoot);
  }

  return promiseRoot;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise ( executor )
 * https://tc39.es/ecma262/#sec-promise-executor
 */
static bool PromiseConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. If NewTarget is undefined, throw a TypeError exception.
  if (!ThrowIfNotConstructing(cx, args, "Promise")) {
    return false;
  }

  // Step 2. If IsCallable(executor) is false, throw a TypeError exception.
  HandleValue executorVal = args.get(0);
  if (!IsCallable(executorVal)) {
    return ReportIsNotFunction(cx, executorVal);
  }
  RootedObject executor(cx, &executorVal.toObject());

  RootedObject newTarget(cx, &args.newTarget().toObject());

  // If the constructor is called via an Xray wrapper, then the newTarget
  // hasn't been unwrapped. We want that because, while the actual instance
  // should be created in the target compartment, the constructor's code
  // should run in the wrapper's compartment.
  //
  // This is so that the resolve and reject callbacks get created in the
  // wrapper's compartment, which is required for code in that compartment
  // to freely interact with it, and, e.g., pass objects as arguments, which
  // it wouldn't be able to if the callbacks were themselves wrapped in Xray
  // wrappers.
  //
  // At the same time, just creating the Promise itself in the wrapper's
  // compartment wouldn't be helpful: if the wrapper forbids interactions
  // with objects except for specific actions, such as calling them, then
  // the code we want to expose it to can't actually treat it as a Promise:
  // calling .then on it would throw, for example.
  //
  // Another scenario where it's important to create the Promise in a
  // different compartment from the resolution functions is when we want to
  // give non-privileged code a Promise resolved with the result of a
  // Promise from privileged code; as a return value of a JS-implemented
  // API, say. If the resolution functions were unprivileged, then resolving
  // with a privileged Promise would cause `resolve` to attempt accessing
  // .then on the passed Promise, which would throw an exception, so we'd
  // just end up with a rejected Promise. Really, we want to chain the two
  // Promises, with the unprivileged one resolved with the resolution of the
  // privileged one.

  bool needsWrapping = false;
  RootedObject proto(cx);
  if (IsWrapper(newTarget)) {
    JSObject* unwrappedNewTarget = CheckedUnwrapStatic(newTarget);
    MOZ_ASSERT(unwrappedNewTarget);
    MOZ_ASSERT(unwrappedNewTarget != newTarget);

    newTarget = unwrappedNewTarget;
    {
      AutoRealm ar(cx, newTarget);
      Handle<GlobalObject*> global = cx->global();
      JSObject* promiseCtor =
          GlobalObject::getOrCreatePromiseConstructor(cx, global);
      if (!promiseCtor) {
        return false;
      }

      // Promise subclasses don't get the special Xray treatment, so
      // we only need to do the complex wrapping and unwrapping scheme
      // described above for instances of Promise itself.
      if (newTarget == promiseCtor) {
        needsWrapping = true;
        proto = GlobalObject::getOrCreatePromisePrototype(cx, cx->global());
        if (!proto) {
          return false;
        }
      }
    }
  }

  if (needsWrapping) {
    if (!cx->compartment()->wrap(cx, &proto)) {
      return false;
    }
  } else {
    if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Promise,
                                            &proto)) {
      return false;
    }
  }
  PromiseObject* promise =
      PromiseObject::create(cx, executor, proto, needsWrapping);
  if (!promise) {
    return false;
  }

  // Step 11.
  args.rval().setObject(*promise);
  if (needsWrapping) {
    return cx->compartment()->wrap(cx, args.rval());
  }
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise ( executor )
 * https://tc39.es/ecma262/#sec-promise-executor
 *
 * Steps 3-11.
 */
/* static */
PromiseObject* PromiseObject::create(JSContext* cx, HandleObject executor,
                                     HandleObject proto /* = nullptr */,
                                     bool needsWrapping /* = false */) {
  MOZ_ASSERT(executor->isCallable());

  RootedObject usedProto(cx, proto);
  // If the proto is wrapped, that means the current function is running
  // with a different compartment active from the one the Promise instance
  // is to be created in.
  // See the comment in PromiseConstructor for details.
  if (needsWrapping) {
    MOZ_ASSERT(proto);
    usedProto = CheckedUnwrapStatic(proto);
    if (!usedProto) {
      ReportAccessDenied(cx);
      return nullptr;
    }
  }

  // Steps 3-7.
  Rooted<PromiseObject*> promise(
      cx, CreatePromiseObjectInternal(cx, usedProto, needsWrapping, false));
  if (!promise) {
    return nullptr;
  }

  RootedObject promiseObj(cx, promise);
  if (needsWrapping && !cx->compartment()->wrap(cx, &promiseObj)) {
    return nullptr;
  }

  // Step 8. Let resolvingFunctions be CreateResolvingFunctions(promise).
  //
  // The resolving functions are created in the compartment active when the
  // (maybe wrapped) Promise constructor was called. They contain checks and
  // can unwrap the Promise if required.
  RootedObject resolveFn(cx);
  RootedObject rejectFn(cx);
  if (!CreateResolvingFunctions(cx, promiseObj, &resolveFn, &rejectFn)) {
    return nullptr;
  }

  // Need to wrap the resolution functions before storing them on the Promise.
  MOZ_ASSERT(promise->getFixedSlot(PromiseSlot_RejectFunction).isUndefined(),
             "Slot must be undefined so initFixedSlot can be used");
  if (needsWrapping) {
    AutoRealm ar(cx, promise);
    RootedObject wrappedRejectFn(cx, rejectFn);
    if (!cx->compartment()->wrap(cx, &wrappedRejectFn)) {
      return nullptr;
    }
    promise->initFixedSlot(PromiseSlot_RejectFunction,
                           ObjectValue(*wrappedRejectFn));
  } else {
    promise->initFixedSlot(PromiseSlot_RejectFunction, ObjectValue(*rejectFn));
  }

  // Step 9. Let completion be
  //         Call(executor, undefined, « resolvingFunctions.[[Resolve]],
  //                                     resolvingFunctions.[[Reject]] »).
  bool success;
  {
    FixedInvokeArgs<2> args(cx);
    args[0].setObject(*resolveFn);
    args[1].setObject(*rejectFn);

    RootedValue calleeOrRval(cx, ObjectValue(*executor));
    success = Call(cx, calleeOrRval, UndefinedHandleValue, args, &calleeOrRval);
  }

  // Step 10. If completion is an abrupt completion, then
  if (!success) {
    RootedValue exceptionVal(cx);
    Rooted<SavedFrame*> stack(cx);
    if (!MaybeGetAndClearExceptionAndStack(cx, &exceptionVal, &stack)) {
      return nullptr;
    }

    // Step 10.a. Perform
    //            ? Call(resolvingFunctions.[[Reject]], undefined,
    //                   « completion.[[Value]] »).
    RootedValue calleeOrRval(cx, ObjectValue(*rejectFn));
    if (!Call(cx, calleeOrRval, UndefinedHandleValue, exceptionVal,
              &calleeOrRval)) {
      return nullptr;
    }
  }

  // Let the Debugger know about this Promise.
  DebugAPI::onNewPromise(cx, promise);

  // Step 11. Return promise.
  return promise;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise ( executor )
 * https://tc39.es/ecma262/#sec-promise-executor
 *
 * skipping creation of resolution functions and executor function invocation.
 */
/* static */
PromiseObject* PromiseObject::createSkippingExecutor(JSContext* cx) {
  return CreatePromiseObjectWithoutResolutionFunctions(cx);
}

class MOZ_STACK_CLASS PromiseForOfIterator : public JS::ForOfIterator {
 public:
  using JS::ForOfIterator::ForOfIterator;

  bool isOptimizedDenseArrayIteration() {
    MOZ_ASSERT(valueIsIterable());
    return index != NOT_ARRAY && IsPackedArray(iterator);
  }
};

[[nodiscard]] static bool PerformPromiseAll(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done);

[[nodiscard]] static bool PerformPromiseAllSettled(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done);

[[nodiscard]] static bool PerformPromiseAny(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done);

[[nodiscard]] static bool PerformPromiseRace(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done);

enum class CombinatorKind { All, AllSettled, Any, Race };

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Unified implementation of
 *
 * Promise.all ( iterable )
 * https://tc39.es/ecma262/#sec-promise.all
 * Promise.allSettled ( iterable )
 * https://tc39.es/ecma262/#sec-promise.allsettled
 * Promise.race ( iterable )
 * https://tc39.es/ecma262/#sec-promise.race
 * Promise.any ( iterable )
 * https://tc39.es/ecma262/#sec-promise.any
 * GetPromiseResolve ( promiseConstructor )
 * https://tc39.es/ecma262/#sec-getpromiseresolve
 */
[[nodiscard]] static bool CommonPromiseCombinator(JSContext* cx, CallArgs& args,
                                                  CombinatorKind kind) {
  HandleValue iterable = args.get(0);

  // Step 2. Let promiseCapability be ? NewPromiseCapability(C).
  // (moved from NewPromiseCapability, step 1).
  HandleValue CVal = args.thisv();
  if (!CVal.isObject()) {
    const char* message;
    switch (kind) {
      case CombinatorKind::All:
        message = "Receiver of Promise.all call";
        break;
      case CombinatorKind::AllSettled:
        message = "Receiver of Promise.allSettled call";
        break;
      case CombinatorKind::Any:
        message = "Receiver of Promise.any call";
        break;
      case CombinatorKind::Race:
        message = "Receiver of Promise.race call";
        break;
    }
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, message);
    return false;
  }

  // Step 1. Let C be the this value.
  RootedObject C(cx, &CVal.toObject());

  // Step 2. Let promiseCapability be ? NewPromiseCapability(C).
  Rooted<PromiseCapability> promiseCapability(cx);
  if (!NewPromiseCapability(cx, C, &promiseCapability, false)) {
    return false;
  }

  RootedValue promiseResolve(cx, UndefinedValue());
  {
    JSObject* promiseCtor =
        GlobalObject::getOrCreatePromiseConstructor(cx, cx->global());
    if (!promiseCtor) {
      return false;
    }

    PromiseLookup& promiseLookup = cx->realm()->promiseLookup;
    if (C != promiseCtor || !promiseLookup.isDefaultPromiseState(cx)) {
      // Step 3. Let promiseResolve be GetPromiseResolve(C).

      // GetPromiseResolve
      // Step 1. Let promiseResolve be ? Get(promiseConstructor, "resolve").
      if (!GetProperty(cx, C, C, cx->names().resolve, &promiseResolve)) {
        // Step 4. IfAbruptRejectPromise(promiseResolve, promiseCapability).
        return AbruptRejectPromise(cx, args, promiseCapability);
      }

      // GetPromiseResolve
      // Step 2. If IsCallable(promiseResolve) is false,
      //         throw a TypeError exception.
      if (!IsCallable(promiseResolve)) {
        ReportIsNotFunction(cx, promiseResolve);

        // Step 4. IfAbruptRejectPromise(promiseResolve, promiseCapability).
        return AbruptRejectPromise(cx, args, promiseCapability);
      }
    }
  }

  // Step 5. Let iteratorRecord be GetIterator(iterable).
  PromiseForOfIterator iter(cx);
  if (!iter.init(iterable, JS::ForOfIterator::AllowNonIterable)) {
    // Step 6. IfAbruptRejectPromise(iteratorRecord, promiseCapability).
    return AbruptRejectPromise(cx, args, promiseCapability);
  }

  if (!iter.valueIsIterable()) {
    // Step 6. IfAbruptRejectPromise(iteratorRecord, promiseCapability).
    const char* message;
    switch (kind) {
      case CombinatorKind::All:
        message = "Argument of Promise.all";
        break;
      case CombinatorKind::AllSettled:
        message = "Argument of Promise.allSettled";
        break;
      case CombinatorKind::Any:
        message = "Argument of Promise.any";
        break;
      case CombinatorKind::Race:
        message = "Argument of Promise.race";
        break;
    }
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_ITERABLE,
                              message);
    return AbruptRejectPromise(cx, args, promiseCapability);
  }

  bool done, result;
  switch (kind) {
    case CombinatorKind::All:
      // Promise.all
      // Step 7. Let result be
      //         PerformPromiseAll(iteratorRecord, C, promiseCapability,
      //                           promiseResolve).
      result = PerformPromiseAll(cx, iter, C, promiseCapability, promiseResolve,
                                 &done);
      break;
    case CombinatorKind::AllSettled:
      // Promise.allSettled
      // Step 7. Let result be
      //         PerformPromiseAllSettled(iteratorRecord, C, promiseCapability,
      //                                  promiseResolve).
      result = PerformPromiseAllSettled(cx, iter, C, promiseCapability,
                                        promiseResolve, &done);
      break;
    case CombinatorKind::Any:
      // Promise.any
      // Step 7. Let result be
      //         PerformPromiseAny(iteratorRecord, C, promiseCapability,
      //                           promiseResolve).
      result = PerformPromiseAny(cx, iter, C, promiseCapability, promiseResolve,
                                 &done);
      break;
    case CombinatorKind::Race:
      // Promise.race
      // Step 7. Let result be
      //         PerformPromiseRace(iteratorRecord, C, promiseCapability,
      //                            promiseResolve).
      result = PerformPromiseRace(cx, iter, C, promiseCapability,
                                  promiseResolve, &done);
      break;
  }

  // Step 8. If result is an abrupt completion, then
  if (!result) {
    // Step 8.a. If iteratorRecord.[[Done]] is false,
    //           set result to IteratorClose(iteratorRecord, result).
    if (!done) {
      iter.closeThrow();
    }

    // Step 8.b. IfAbruptRejectPromise(result, promiseCapability).
    return AbruptRejectPromise(cx, args, promiseCapability);
  }

  // Step 9. Return Completion(result).
  args.rval().setObject(*promiseCapability.promise());
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.all ( iterable )
 * https://tc39.es/ecma262/#sec-promise.all
 */
static bool Promise_static_all(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CommonPromiseCombinator(cx, args, CombinatorKind::All);
}

[[nodiscard]] static bool PerformPromiseThen(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue onFulfilled_,
    HandleValue onRejected_, Handle<PromiseCapability> resultCapability);

[[nodiscard]] static bool PerformPromiseThenWithoutSettleHandlers(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseObject*> promiseToResolve,
    Handle<PromiseCapability> resultCapability);

static JSFunction* NewPromiseCombinatorElementFunction(
    JSContext* cx, Native native,
    Handle<PromiseCombinatorDataHolder*> dataHolder, uint32_t index);

static bool PromiseAllResolveElementFunction(JSContext* cx, unsigned argc,
                                             Value* vp);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.all ( iterable )
 * https://tc39.es/ecma262/#sec-promise.all
 * PerformPromiseAll ( iteratorRecord, constructor, resultCapability,
 *                     promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiseall
 *
 * Unforgeable version.
 */
[[nodiscard]] JSObject* js::GetWaitForAllPromise(
    JSContext* cx, JS::HandleObjectVector promises) {
#ifdef DEBUG
  for (size_t i = 0, len = promises.length(); i < len; i++) {
    JSObject* obj = promises[i];
    cx->check(obj);
    MOZ_ASSERT(UncheckedUnwrap(obj)->is<PromiseObject>());
  }
#endif

  // Step 1. Let C be the this value.
  RootedObject C(cx,
                 GlobalObject::getOrCreatePromiseConstructor(cx, cx->global()));
  if (!C) {
    return nullptr;
  }

  // Step 2. Let promiseCapability be ? NewPromiseCapability(C).
  Rooted<PromiseCapability> resultCapability(cx);
  if (!NewPromiseCapability(cx, C, &resultCapability, false)) {
    return nullptr;
  }

  // Steps 3-6 for iterator and iteratorRecord.
  // (omitted)

  // Step 7. Let result be
  //         PerformPromiseAll(iteratorRecord, C, promiseCapability,
  //                           promiseResolve).
  //
  // Implemented as an inlined, simplied version of PerformPromiseAll.
  {
    uint32_t promiseCount = promises.length();
    // PerformPromiseAll

    // Step 1. Let values be a new empty List.
    Rooted<PromiseCombinatorElements> values(cx);
    {
      auto* valuesArray = NewDenseFullyAllocatedArray(cx, promiseCount);
      if (!valuesArray) {
        return nullptr;
      }
      valuesArray->ensureDenseInitializedLength(0, promiseCount);

      values.initialize(valuesArray);
    }

    // Step 2. Let remainingElementsCount be the Record { [[Value]]: 1 }.
    //
    // Create our data holder that holds all the things shared across
    // every step of the iterator.  In particular, this holds the
    // remainingElementsCount (as an integer reserved slot), the array of
    // values, and the resolve function from our PromiseCapability.
    Rooted<PromiseCombinatorDataHolder*> dataHolder(cx);
    dataHolder = PromiseCombinatorDataHolder::New(
        cx, resultCapability.promise(), values, resultCapability.resolve());
    if (!dataHolder) {
      return nullptr;
    }

    // Call PerformPromiseThen with resolve and reject set to nullptr.
    Rooted<PromiseCapability> resultCapabilityWithoutResolving(cx);
    resultCapabilityWithoutResolving.promise().set(resultCapability.promise());

    // Step 3. Let index be 0.
    // Step 4. Repeat,
    // Step 4.t. Set index to index + 1.
    for (uint32_t index = 0; index < promiseCount; index++) {
      // Steps 4.a-c for IteratorStep.
      // (omitted)

      // Step 4.d. (implemented after the loop).

      // Steps 4.e-g for IteratorValue
      // (omitted)

      // Step 4.h. Append undefined to values.
      values.unwrappedArray()->setDenseElement(index, UndefinedHandleValue);

      // Step 4.i. Let nextPromise be
      //           ? Call(promiseResolve, constructor, « nextValue »).
      RootedObject nextPromiseObj(cx, promises[index]);

      // Steps 4.j-q.
      JSFunction* resolveFunc = NewPromiseCombinatorElementFunction(
          cx, PromiseAllResolveElementFunction, dataHolder, index);
      if (!resolveFunc) {
        return nullptr;
      }

      // Step 4.r. Set remainingElementsCount.[[Value]] to
      //           remainingElementsCount.[[Value]] + 1.
      dataHolder->increaseRemainingCount();

      // Step 4.s. Perform
      //           ? Invoke(nextPromise, "then",
      //                    « onFulfilled, resultCapability.[[Reject]] »).
      RootedValue resolveFunVal(cx, ObjectValue(*resolveFunc));
      RootedValue rejectFunVal(cx, ObjectValue(*resultCapability.reject()));
      Rooted<PromiseObject*> nextPromise(cx);

      // GetWaitForAllPromise is used internally only and must not
      // trigger content-observable effects when registering a reaction.
      // It's also meant to work on wrapped Promises, potentially from
      // compartments with principals inaccessible from the current
      // compartment. To make that work, it unwraps promises with
      // UncheckedUnwrap,
      nextPromise = &UncheckedUnwrap(nextPromiseObj)->as<PromiseObject>();

      if (!PerformPromiseThen(cx, nextPromise, resolveFunVal, rejectFunVal,
                              resultCapabilityWithoutResolving)) {
        return nullptr;
      }
    }

    // Step 4.d.i. Set iteratorRecord.[[Done]] to true.
    // (implicit)

    // Step 4.d.ii. Set remainingElementsCount.[[Value]] to
    //              remainingElementsCount.[[Value]] - 1.
    int32_t remainingCount = dataHolder->decreaseRemainingCount();

    // Step 4.d.iii.If remainingElementsCount.[[Value]] is 0, then
    if (remainingCount == 0) {
      // Step 4.d.iii.1. Let valuesArray be ! CreateArrayFromList(values).
      // (already performed)

      // Step 4.d.iii.2. Perform
      //                 ? Call(resultCapability.[[Resolve]], undefined,
      //                        « valuesArray »).
      if (!ResolvePromiseInternal(cx, resultCapability.promise(),
                                  values.value())) {
        return nullptr;
      }
    }
  }

  // Step 4.d.iv. Return resultCapability.[[Promise]].
  return resultCapability.promise();
}

static bool CallDefaultPromiseResolveFunction(JSContext* cx,
                                              Handle<PromiseObject*> promise,
                                              HandleValue resolutionValue);
static bool CallDefaultPromiseRejectFunction(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue rejectionValue,
    JS::Handle<SavedFrame*> unwrappedRejectionStack = nullptr);

/**
 * Perform Call(promiseCapability.[[Resolve]], undefined ,« value ») given
 * promiseCapability = { promiseObj, resolveFun }.
 *
 * Also,
 *
 * ES2023 draft rev 714fa3dd1e8237ae9c666146270f81880089eca5
 *
 * NewPromiseReactionJob ( reaction, argument )
 * https://tc39.es/ecma262/#sec-newpromisereactionjob
 *
 * Steps 1.f-i. "type is Fulfill" case.
 */
[[nodiscard]] static bool CallPromiseResolveFunction(JSContext* cx,
                                                     HandleObject resolveFun,
                                                     HandleValue value,
                                                     HandleObject promiseObj) {
  cx->check(resolveFun);
  cx->check(value);
  cx->check(promiseObj);

  // NewPromiseReactionJob
  // Step 1.g. Assert: promiseCapability is a PromiseCapability Record.
  // (implicit)

  if (resolveFun) {
    // NewPromiseReactionJob
    // Step 1.h. If handlerResult is an abrupt completion, then
    //           (handled in CallPromiseRejectFunction)
    // Step 1.i. Else,
    // Step 1.i.i. Return
    //             ? Call(promiseCapability.[[Resolve]], undefined,
    //                    « handlerResult.[[Value]] »).
    RootedValue calleeOrRval(cx, ObjectValue(*resolveFun));
    return Call(cx, calleeOrRval, UndefinedHandleValue, value, &calleeOrRval);
  }

  // `promiseObj` can be optimized away if it's known to be unused.
  //
  // NewPromiseReactionJob
  // Step f. If promiseCapability is undefined, then
  // (reordered)
  //
  // NOTE: "promiseCapability is undefined" case is represented by
  //       `resolveFun == nullptr && promiseObj == nullptr`.
  if (!promiseObj) {
    // NewPromiseReactionJob
    // Step f.i. Assert: handlerResult is not an abrupt completion.
    // (implicit)

    // Step f.ii. Return empty.
    return true;
  }

  // NewPromiseReactionJob
  // Step 1.h. If handlerResult is an abrupt completion, then
  //           (handled in CallPromiseRejectFunction)
  // Step 1.i. Else,
  // Step 1.i.i. Return
  //             ? Call(promiseCapability.[[Resolve]], undefined,
  //                    « handlerResult.[[Value]] »).
  Handle<PromiseObject*> promise = promiseObj.as<PromiseObject>();
  if (IsPromiseWithDefaultResolvingFunction(promise)) {
    return CallDefaultPromiseResolveFunction(cx, promise, value);
  }

  // This case is used by resultCapabilityWithoutResolving in
  // GetWaitForAllPromise, and nothing should be done.

  return true;
}

/**
 * Perform Call(promiseCapability.[[Reject]], undefined ,« reason ») given
 * promiseCapability = { promiseObj, rejectFun }.
 *
 * Also,
 *
 * ES2023 draft rev 714fa3dd1e8237ae9c666146270f81880089eca5
 *
 * NewPromiseReactionJob ( reaction, argument )
 * https://tc39.es/ecma262/#sec-newpromisereactionjob
 *
 * Steps 1.g-i. "type is Reject" case.
 */
[[nodiscard]] static bool CallPromiseRejectFunction(
    JSContext* cx, HandleObject rejectFun, HandleValue reason,
    HandleObject promiseObj, Handle<SavedFrame*> unwrappedRejectionStack,
    UnhandledRejectionBehavior behavior) {
  cx->check(rejectFun);
  cx->check(reason);
  cx->check(promiseObj);

  // NewPromiseReactionJob
  // Step 1.g. Assert: promiseCapability is a PromiseCapability Record.
  // (implicit)

  if (rejectFun) {
    // NewPromiseReactionJob
    // Step 1.h. If handlerResult is an abrupt completion, then
    // Step 1.h.i. Return
    //             ? Call(promiseCapability.[[Reject]], undefined,
    //                    « handlerResult.[[Value]] »).
    RootedValue calleeOrRval(cx, ObjectValue(*rejectFun));
    return Call(cx, calleeOrRval, UndefinedHandleValue, reason, &calleeOrRval);
  }

  // NewPromiseReactionJob
  // See the comment in CallPromiseResolveFunction for promiseCapability field
  //
  // Step f. If promiseCapability is undefined, then
  // Step f.i. Assert: handlerResult is not an abrupt completion.
  //
  // The spec doesn't allow promiseCapability to be undefined for reject case,
  // but `promiseObj` can be optimized away if it's known to be unused.
  if (!promiseObj) {
    if (behavior == UnhandledRejectionBehavior::Ignore) {
      // Do nothing if unhandled rejections are to be ignored.
      return true;
    }

    // Otherwise create and reject a promise on the fly.  The promise's
    // allocation time will be wrong.  So it goes.
    Rooted<PromiseObject*> temporaryPromise(
        cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
    if (!temporaryPromise) {
      cx->clearPendingException();
      return true;
    }

    // NewPromiseReactionJob
    // Step 1.h. If handlerResult is an abrupt completion, then
    // Step 1.h.i. Return
    //             ? Call(promiseCapability.[[Reject]], undefined,
    //                    « handlerResult.[[Value]] »).
    return RejectPromiseInternal(cx, temporaryPromise, reason,
                                 unwrappedRejectionStack);
  }

  // NewPromiseReactionJob
  // Step 1.h. If handlerResult is an abrupt completion, then
  // Step 1.h.i. Return
  //             ? Call(promiseCapability.[[Reject]], undefined,
  //                    « handlerResult.[[Value]] »).
  Handle<PromiseObject*> promise = promiseObj.as<PromiseObject>();
  if (IsPromiseWithDefaultResolvingFunction(promise)) {
    return CallDefaultPromiseRejectFunction(cx, promise, reason,
                                            unwrappedRejectionStack);
  }

  // This case is used by resultCapabilityWithoutResolving in
  // GetWaitForAllPromise, and nothing should be done.

  return true;
}

[[nodiscard]] static JSObject* CommonStaticResolveRejectImpl(
    JSContext* cx, HandleValue thisVal, HandleValue argVal,
    ResolutionMode mode);

static bool IsPromiseSpecies(JSContext* cx, JSFunction* species);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Unified implementation of
 *
 * PerformPromiseAll ( iteratorRecord, constructor, resultCapability,
 *                     promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiseall
 * PerformPromiseAllSettled ( iteratorRecord, constructor, resultCapability,
 *                            promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiseallsettled
 * PerformPromiseRace ( iteratorRecord, constructor, resultCapability,
 *                      promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiserace
 * PerformPromiseAny ( iteratorRecord, constructor, resultCapability,
 *                     promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiseany
 *
 * Promise.prototype.then ( onFulfilled, onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.then
 */
template <typename T>
[[nodiscard]] static bool CommonPerformPromiseCombinator(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    HandleObject resultPromise, HandleValue promiseResolve, bool* done,
    bool resolveReturnsUndefined, T getResolveAndReject) {
  RootedObject promiseCtor(
      cx, GlobalObject::getOrCreatePromiseConstructor(cx, cx->global()));
  if (!promiseCtor) {
    return false;
  }

  // Optimized dense array iteration ensures no side-effects take place
  // during the iteration.
  bool iterationMayHaveSideEffects = !iterator.isOptimizedDenseArrayIteration();

  PromiseLookup& promiseLookup = cx->realm()->promiseLookup;

  // Try to optimize when the Promise object is in its default state, guarded
  // by |C == promiseCtor| because we can only perform this optimization
  // for the builtin Promise constructor.
  bool isDefaultPromiseState =
      C == promiseCtor && promiseLookup.isDefaultPromiseState(cx);
  bool validatePromiseState = iterationMayHaveSideEffects;

  RootedValue CVal(cx, ObjectValue(*C));
  RootedValue resolveFunVal(cx);
  RootedValue rejectFunVal(cx);

  // We're reusing rooted variables in the loop below, so we don't need to
  // declare a gazillion different rooted variables here. Rooted variables
  // which are reused include "Or" in their name.
  RootedValue nextValueOrNextPromise(cx);
  RootedObject nextPromiseObj(cx);
  RootedValue thenVal(cx);
  RootedObject thenSpeciesOrBlockedPromise(cx);
  Rooted<PromiseCapability> thenCapability(cx);

  // PerformPromiseAll, PerformPromiseAllSettled, PerformPromiseAny
  // Step 4.
  // PerformPromiseRace
  // Step 1.
  while (true) {
    // Step a. Let next be IteratorStep(iteratorRecord).
    // Step b. If next is an abrupt completion, set iteratorRecord.[[Done]] to
    //         true.
    // Step c. ReturnIfAbrupt(next).
    // Step e. Let nextValue be IteratorValue(next).
    // Step f. If nextValue is an abrupt completion, set iteratorRecord.[[Done]]
    //         to true.
    // Step g. ReturnIfAbrupt(nextValue).
    RootedValue& nextValue = nextValueOrNextPromise;
    if (!iterator.next(&nextValue, done)) {
      *done = true;
      return false;
    }

    // Step d. If next is false, then
    if (*done) {
      return true;
    }

    // Set to false when we can skip the [[Get]] for "then" and instead
    // use the built-in Promise.prototype.then function.
    bool getThen = true;

    if (isDefaultPromiseState && validatePromiseState) {
      isDefaultPromiseState = promiseLookup.isDefaultPromiseState(cx);
    }

    RootedValue& nextPromise = nextValueOrNextPromise;
    if (isDefaultPromiseState) {
      PromiseObject* nextValuePromise = nullptr;
      if (nextValue.isObject() && nextValue.toObject().is<PromiseObject>()) {
        nextValuePromise = &nextValue.toObject().as<PromiseObject>();
      }

      if (nextValuePromise &&
          promiseLookup.isDefaultInstanceWhenPromiseStateIsSane(
              cx, nextValuePromise)) {
        // The below steps don't produce any side-effects, so we can
        // skip the Promise state revalidation in the next iteration
        // when the iterator itself also doesn't produce any
        // side-effects.
        validatePromiseState = iterationMayHaveSideEffects;

        // Step {i, h}. Let nextPromise be
        //              ? Call(promiseResolve, constructor, « nextValue »).
        // Promise.resolve is a no-op for the default case.
        MOZ_ASSERT(&nextPromise.toObject() == nextValuePromise);

        // `nextPromise` uses the built-in `then` function.
        getThen = false;
      } else {
        // Need to revalidate the Promise state in the next iteration,
        // because CommonStaticResolveRejectImpl may have modified it.
        validatePromiseState = true;

        // Step {i, h}. Let nextPromise be
        //              ? Call(promiseResolve, constructor, « nextValue »).
        // Inline the call to Promise.resolve.
        JSObject* res =
            CommonStaticResolveRejectImpl(cx, CVal, nextValue, ResolveMode);
        if (!res) {
          return false;
        }

        nextPromise.setObject(*res);
      }
    } else if (promiseResolve.isUndefined()) {
      // |promiseResolve| is undefined when the Promise constructor was
      // initially in its default state, i.e. if it had been retrieved, it would
      // have been set to |Promise.resolve|.

      // Step {i, h}. Let nextPromise be
      //              ? Call(promiseResolve, constructor, « nextValue »).
      // Inline the call to Promise.resolve.
      JSObject* res =
          CommonStaticResolveRejectImpl(cx, CVal, nextValue, ResolveMode);
      if (!res) {
        return false;
      }

      nextPromise.setObject(*res);
    } else {
      // Step {i, h}. Let nextPromise be
      //              ? Call(promiseResolve, constructor, « nextValue »).
      if (!Call(cx, promiseResolve, CVal, nextValue, &nextPromise)) {
        return false;
      }
    }

    // Get the resolving functions for this iteration.
    // PerformPromiseAll
    // Steps j-r.
    // PerformPromiseAllSettled
    // Steps j-aa.
    // PerformPromiseRace
    // Step i.
    // PerformPromiseAny
    // Steps j-q.
    if (!getResolveAndReject(&resolveFunVal, &rejectFunVal)) {
      return false;
    }

    // Call |nextPromise.then| with the provided hooks and add
    // |resultPromise| to the list of dependent promises.
    //
    // If |nextPromise.then| is the original |Promise.prototype.then|
    // function and the call to |nextPromise.then| would use the original
    // |Promise| constructor to create the resulting promise, we skip the
    // call to |nextPromise.then| and thus creating a new promise that
    // would not be observable by content.

    // PerformPromiseAll
    // Step s. Perform
    //         ? Invoke(nextPromise, "then",
    //                  « onFulfilled, resultCapability.[[Reject]] »).
    // PerformPromiseAllSettled
    // Step ab. Perform
    //          ? Invoke(nextPromise, "then", « onFulfilled, onRejected »).
    // PerformPromiseRace
    // Step i. Perform
    //         ? Invoke(nextPromise, "then",
    //                  « resultCapability.[[Resolve]],
    //                    resultCapability.[[Reject]] »).
    // PerformPromiseAny
    // Step s. Perform
    //         ? Invoke(nextPromise, "then",
    //                  « resultCapability.[[Resolve]], onRejected »).
    nextPromiseObj = ToObject(cx, nextPromise);
    if (!nextPromiseObj) {
      return false;
    }

    bool isBuiltinThen;
    if (getThen) {
      // We don't use the Promise lookup cache here, because this code
      // is only called when we had a lookup cache miss, so it's likely
      // we'd get another cache miss when trying to use the cache here.
      if (!GetProperty(cx, nextPromiseObj, nextPromise, cx->names().then,
                       &thenVal)) {
        return false;
      }

      // |nextPromise| is an unwrapped Promise, and |then| is the
      // original |Promise.prototype.then|, inline it here.
      isBuiltinThen = nextPromiseObj->is<PromiseObject>() &&
                      IsNativeFunction(thenVal, Promise_then);
    } else {
      isBuiltinThen = true;
    }

    // By default, the blocked promise is added as an extra entry to the
    // rejected promises list.
    bool addToDependent = true;

    if (isBuiltinThen) {
      MOZ_ASSERT(nextPromise.isObject());
      MOZ_ASSERT(&nextPromise.toObject() == nextPromiseObj);

      // Promise.prototype.then
      // Step 3. Let C be ? SpeciesConstructor(promise, %Promise%).
      RootedObject& thenSpecies = thenSpeciesOrBlockedPromise;
      if (getThen) {
        thenSpecies = SpeciesConstructor(cx, nextPromiseObj, JSProto_Promise,
                                         IsPromiseSpecies);
        if (!thenSpecies) {
          return false;
        }
      } else {
        thenSpecies = promiseCtor;
      }

      // The fast path here and the one in NewPromiseCapability may not
      // set the resolve and reject handlers, so we need to clear the
      // fields in case they were set in the previous iteration.
      thenCapability.resolve().set(nullptr);
      thenCapability.reject().set(nullptr);

      // Skip the creation of a built-in Promise object if:
      // 1. `thenSpecies` is the built-in Promise constructor.
      // 2. `resolveFun` doesn't return an object, which ensures no side effects
      //    occur in ResolvePromiseInternal.
      // 3. The result promise is a built-in Promise object.
      // 4. The result promise doesn't use the default resolving functions,
      //    which in turn means Run{Fulfill,Reject}Function when called from
      //    PromiseReactionJob won't try to resolve the promise.
      if (thenSpecies == promiseCtor && resolveReturnsUndefined &&
          resultPromise->is<PromiseObject>() &&
          !IsPromiseWithDefaultResolvingFunction(
              &resultPromise->as<PromiseObject>())) {
        thenCapability.promise().set(resultPromise);
        addToDependent = false;
      } else {
        // Promise.prototype.then
        // Step 4. Let resultCapability be ? NewPromiseCapability(C).
        if (!NewPromiseCapability(cx, thenSpecies, &thenCapability, true)) {
          return false;
        }
      }

      // Promise.prototype.then
      // Step 5. Return
      //         PerformPromiseThen(promise, onFulfilled, onRejected,
      //                            resultCapability).
      Handle<PromiseObject*> promise = nextPromiseObj.as<PromiseObject>();
      if (!PerformPromiseThen(cx, promise, resolveFunVal, rejectFunVal,
                              thenCapability)) {
        return false;
      }
    } else {
      // Optimization failed, do the normal call.
      RootedValue& ignored = thenVal;
      if (!Call(cx, thenVal, nextPromise, resolveFunVal, rejectFunVal,
                &ignored)) {
        return false;
      }

      // In case the value to depend on isn't an object at all, there's
      // nothing more to do here: we can only add reactions to Promise
      // objects (potentially after unwrapping them), and non-object
      // values can't be Promise objects. This can happen if Promise.all
      // is called on an object with a `resolve` method that returns
      // primitives.
      if (!nextPromise.isObject()) {
        addToDependent = false;
      }
    }

    // Adds |resultPromise| to the list of dependent promises.
    if (addToDependent) {
      // The object created by the |promise.then| call or the inlined
      // version of it above is visible to content (either because
      // |promise.then| was overridden by content and could leak it,
      // or because a constructor other than the original value of
      // |Promise| was used to create it). To have both that object and
      // |resultPromise| show up as dependent promises in the debugger,
      // add a dummy reaction to the list of reject reactions that
      // contains |resultPromise|, but otherwise does nothing.
      RootedObject& blockedPromise = thenSpeciesOrBlockedPromise;
      blockedPromise = resultPromise;

      mozilla::Maybe<AutoRealm> ar;
      if (IsProxy(nextPromiseObj)) {
        nextPromiseObj = CheckedUnwrapStatic(nextPromiseObj);
        if (!nextPromiseObj) {
          ReportAccessDenied(cx);
          return false;
        }
        if (JS_IsDeadWrapper(nextPromiseObj)) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_DEAD_OBJECT);
          return false;
        }
        ar.emplace(cx, nextPromiseObj);
        if (!cx->compartment()->wrap(cx, &blockedPromise)) {
          return false;
        }
      }

      // If either the object to depend on (`nextPromiseObj`) or the
      // object that gets blocked (`resultPromise`) isn't a,
      // maybe-wrapped, Promise instance, we ignore it. All this does is
      // lose some small amount of debug information in scenarios that
      // are highly unlikely to occur in useful code.
      if (nextPromiseObj->is<PromiseObject>() &&
          resultPromise->is<PromiseObject>()) {
        Handle<PromiseObject*> promise = nextPromiseObj.as<PromiseObject>();
        if (!AddDummyPromiseReactionForDebugger(cx, promise, blockedPromise)) {
          return false;
        }
      }
    }
  }
}

// Create the elements for the Promise combinators Promise.all and
// Promise.allSettled.
[[nodiscard]] static bool NewPromiseCombinatorElements(
    JSContext* cx, Handle<PromiseCapability> resultCapability,
    MutableHandle<PromiseCombinatorElements> elements) {
  // We have to be very careful about which compartments we create things for
  // the Promise combinators. In particular, we have to maintain the invariant
  // that anything stored in a reserved slot is same-compartment with the object
  // whose reserved slot it's in. But we want to create the values array in the
  // compartment of the result capability's Promise, because that array can get
  // exposed as the Promise's resolution value to code that has access to the
  // Promise (in particular code from that compartment), and that should work,
  // even if the Promise compartment is less-privileged than our caller
  // compartment.
  //
  // So the plan is as follows: Create the values array in the promise
  // compartment. Create the promise resolving functions and the data holder in
  // our current compartment, i.e. the compartment of the Promise combinator
  // function. Store a cross-compartment wrapper to the values array in the
  // holder. This should be OK because the only things we hand the promise
  // resolving functions to are the "then" calls we do and in the case when the
  // Promise's compartment is not the current compartment those are happening
  // over Xrays anyway, which means they get the canonical "then" function and
  // content can't see our promise resolving functions.

  if (IsWrapper(resultCapability.promise())) {
    JSObject* unwrappedPromiseObj =
        CheckedUnwrapStatic(resultCapability.promise());
    MOZ_ASSERT(unwrappedPromiseObj);

    {
      AutoRealm ar(cx, unwrappedPromiseObj);
      auto* array = NewDenseEmptyArray(cx);
      if (!array) {
        return false;
      }
      elements.initialize(array);
    }

    if (!cx->compartment()->wrap(cx, elements.value())) {
      return false;
    }
  } else {
    auto* array = NewDenseEmptyArray(cx);
    if (!array) {
      return false;
    }

    elements.initialize(array);
  }
  return true;
}

// Retrieve the combinator elements from the data holder.
[[nodiscard]] static bool GetPromiseCombinatorElements(
    JSContext* cx, Handle<PromiseCombinatorDataHolder*> data,
    MutableHandle<PromiseCombinatorElements> elements) {
  bool needsWrapping = false;
  JSObject* valuesObj = &data->valuesArray().toObject();
  if (IsProxy(valuesObj)) {
    // See comment for NewPromiseCombinatorElements for why we unwrap here.
    valuesObj = UncheckedUnwrap(valuesObj);

    if (JS_IsDeadWrapper(valuesObj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }

    needsWrapping = true;
  }

  elements.initialize(data, &valuesObj->as<ArrayObject>(), needsWrapping);
  return true;
}

static JSFunction* NewPromiseCombinatorElementFunction(
    JSContext* cx, Native native,
    Handle<PromiseCombinatorDataHolder*> dataHolder, uint32_t index) {
  JSFunction* fn = NewNativeFunction(
      cx, native, 1, nullptr, gc::AllocKind::FUNCTION_EXTENDED, GenericObject);
  if (!fn) {
    return nullptr;
  }

  fn->setExtendedSlot(PromiseCombinatorElementFunctionSlot_Data,
                      ObjectValue(*dataHolder));
  fn->setExtendedSlot(PromiseCombinatorElementFunctionSlot_ElementIndex,
                      Int32Value(index));
  return fn;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Unified implementation of
 *
 * Promise.all Resolve Element Functions
 * https://tc39.es/ecma262/#sec-promise.all-resolve-element-functions
 *
 * Steps 1-4.
 *
 * Promise.allSettled Resolve Element Functions
 * https://tc39.es/ecma262/#sec-promise.allsettled-resolve-element-functions
 *
 * Steps 1-5.
 *
 * Promise.allSettled Reject Element Functions
 * https://tc39.es/ecma262/#sec-promise.allsettled-reject-element-functions
 *
 * Steps 1-5.
 *
 * Common implementation for Promise combinator element functions to check if
 * they've already been called.
 */
static bool PromiseCombinatorElementFunctionAlreadyCalled(
    const CallArgs& args, MutableHandle<PromiseCombinatorDataHolder*> data,
    uint32_t* index) {
  // Step 1. Let F be the active function object.
  JSFunction* fn = &args.callee().as<JSFunction>();

  // Promise.all functions
  // Step 2. If F.[[AlreadyCalled]] is true, return undefined.
  // Promise.allSettled functions
  // Step 2. Let alreadyCalled be F.[[AlreadyCalled]].
  // Step 3. If alreadyCalled.[[Value]] is true, return undefined.
  //
  // We use the existence of the data holder as a signal for whether the Promise
  // combinator element function was already called. Upon resolution, it's reset
  // to `undefined`.
  const Value& dataVal =
      fn->getExtendedSlot(PromiseCombinatorElementFunctionSlot_Data);
  if (dataVal.isUndefined()) {
    return true;
  }

  data.set(&dataVal.toObject().as<PromiseCombinatorDataHolder>());

  // Promise.all functions
  // Step 3. Set F.[[AlreadyCalled]] to true.
  // Promise.allSettled functions
  // Step 4. Set alreadyCalled.[[Value]] to true.
  fn->setExtendedSlot(PromiseCombinatorElementFunctionSlot_Data,
                      UndefinedValue());

  // Promise.all functions
  // Step 4. Let index be F.[[Index]].
  // Promise.allSettled functions
  // Step 5. Let index be F.[[Index]].
  int32_t idx =
      fn->getExtendedSlot(PromiseCombinatorElementFunctionSlot_ElementIndex)
          .toInt32();
  MOZ_ASSERT(idx >= 0);
  *index = uint32_t(idx);

  return false;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseAll ( iteratorRecord, constructor, resultCapability,
 *                     promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiseall
 */
[[nodiscard]] static bool PerformPromiseAll(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done) {
  *done = false;

  MOZ_ASSERT(C->isConstructor());

  // Step 1. Let values be a new empty List.
  Rooted<PromiseCombinatorElements> values(cx);
  if (!NewPromiseCombinatorElements(cx, resultCapability, &values)) {
    return false;
  }

  // Step 2. Let remainingElementsCount be the Record { [[Value]]: 1 }.
  //
  // Create our data holder that holds all the things shared across
  // every step of the iterator.  In particular, this holds the
  // remainingElementsCount (as an integer reserved slot), the array of
  // values, and the resolve function from our PromiseCapability.
  Rooted<PromiseCombinatorDataHolder*> dataHolder(cx);
  dataHolder = PromiseCombinatorDataHolder::New(
      cx, resultCapability.promise(), values, resultCapability.resolve());
  if (!dataHolder) {
    return false;
  }

  // Step 3. Let index be 0.
  uint32_t index = 0;

  auto getResolveAndReject = [cx, &resultCapability, &values, &dataHolder,
                              &index](MutableHandleValue resolveFunVal,
                                      MutableHandleValue rejectFunVal) {
    // Step 4.h. Append undefined to values.
    if (!values.pushUndefined(cx)) {
      return false;
    }

    // Steps 4.j-q.
    JSFunction* resolveFunc = NewPromiseCombinatorElementFunction(
        cx, PromiseAllResolveElementFunction, dataHolder, index);
    if (!resolveFunc) {
      return false;
    }

    // Step 4.r. Set remainingElementsCount.[[Value]] to
    //           remainingElementsCount.[[Value]] + 1.
    dataHolder->increaseRemainingCount();

    // Step 4.t. Set index to index + 1.
    index++;
    MOZ_ASSERT(index > 0);

    resolveFunVal.setObject(*resolveFunc);
    rejectFunVal.setObject(*resultCapability.reject());
    return true;
  };

  // Steps 4.
  if (!CommonPerformPromiseCombinator(
          cx, iterator, C, resultCapability.promise(), promiseResolve, done,
          true, getResolveAndReject)) {
    return false;
  }

  // Step 4.d.ii. Set remainingElementsCount.[[Value]] to
  //              remainingElementsCount.[[Value]] - 1.
  int32_t remainingCount = dataHolder->decreaseRemainingCount();

  // Step 4.d.iii. If remainingElementsCount.[[Value]] is 0, then
  if (remainingCount == 0) {
    // Step 4.d.iii.1. Let valuesArray be ! CreateArrayFromList(values).
    // (already performed)

    // Step 4.d.iii.2. Perform
    //                 ? Call(resultCapability.[[Resolve]], undefined,
    //                        « valuesArray »).
    return CallPromiseResolveFunction(cx, resultCapability.resolve(),
                                      values.value(),
                                      resultCapability.promise());
  }

  // Step 4.d.iv. Return resultCapability.[[Promise]].
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.all Resolve Element Functions
 * https://tc39.es/ecma262/#sec-promise.all-resolve-element-functions
 */
static bool PromiseAllResolveElementFunction(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue xVal = args.get(0);

  // Steps 1-4.
  Rooted<PromiseCombinatorDataHolder*> data(cx);
  uint32_t index;
  if (PromiseCombinatorElementFunctionAlreadyCalled(args, &data, &index)) {
    args.rval().setUndefined();
    return true;
  }

  // Step 5. Let values be F.[[Values]].
  Rooted<PromiseCombinatorElements> values(cx);
  if (!GetPromiseCombinatorElements(cx, data, &values)) {
    return false;
  }

  // Step 8. Set values[index] to x.
  if (!values.setElement(cx, index, xVal)) {
    return false;
  }

  // (reordered)
  // Step 7. Let remainingElementsCount be F.[[RemainingElements]].
  //
  // Step 9. Set remainingElementsCount.[[Value]] to
  //         remainingElementsCount.[[Value]] - 1.
  uint32_t remainingCount = data->decreaseRemainingCount();

  // Step 10. If remainingElementsCount.[[Value]] is 0, then
  if (remainingCount == 0) {
    // Step 10.a. Let valuesArray be ! CreateArrayFromList(values).
    // (already performed)

    // (reordered)
    // Step 6. Let promiseCapability be F.[[Capability]].
    //
    // Step 10.b. Return
    //            ? Call(promiseCapability.[[Resolve]], undefined,
    //                   « valuesArray »).
    RootedObject resolveAllFun(cx, data->resolveOrRejectObj());
    RootedObject promiseObj(cx, data->promiseObj());
    if (!CallPromiseResolveFunction(cx, resolveAllFun, values.value(),
                                    promiseObj)) {
      return false;
    }
  }

  // Step 11. Return undefined.
  args.rval().setUndefined();
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.race ( iterable )
 * https://tc39.es/ecma262/#sec-promise.race
 */
static bool Promise_static_race(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CommonPromiseCombinator(cx, args, CombinatorKind::Race);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseRace ( iteratorRecord, constructor, resultCapability,
 *                      promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiserace
 */
[[nodiscard]] static bool PerformPromiseRace(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done) {
  *done = false;

  MOZ_ASSERT(C->isConstructor());

  // BlockOnPromise fast path requires the passed onFulfilled function
  // doesn't return an object value, because otherwise the skipped promise
  // creation is detectable due to missing property lookups.
  bool isDefaultResolveFn =
      IsNativeFunction(resultCapability.resolve(), ResolvePromiseFunction);

  auto getResolveAndReject = [&resultCapability](
                                 MutableHandleValue resolveFunVal,
                                 MutableHandleValue rejectFunVal) {
    resolveFunVal.setObject(*resultCapability.resolve());
    rejectFunVal.setObject(*resultCapability.reject());
    return true;
  };

  // Step 1.
  return CommonPerformPromiseCombinator(
      cx, iterator, C, resultCapability.promise(), promiseResolve, done,
      isDefaultResolveFn, getResolveAndReject);
}

enum class PromiseAllSettledElementFunctionKind { Resolve, Reject };

template <PromiseAllSettledElementFunctionKind Kind>
static bool PromiseAllSettledElementFunction(JSContext* cx, unsigned argc,
                                             Value* vp);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.allSettled ( iterable )
 * https://tc39.es/ecma262/#sec-promise.allsettled
 */
static bool Promise_static_allSettled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CommonPromiseCombinator(cx, args, CombinatorKind::AllSettled);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseAllSettled ( iteratorRecord, constructor, resultCapability,
 *                            promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiseallsettled
 */
[[nodiscard]] static bool PerformPromiseAllSettled(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done) {
  *done = false;

  MOZ_ASSERT(C->isConstructor());

  // Step 1. Let values be a new empty List.
  Rooted<PromiseCombinatorElements> values(cx);
  if (!NewPromiseCombinatorElements(cx, resultCapability, &values)) {
    return false;
  }

  // Step 2. Let remainingElementsCount be the Record { [[Value]]: 1 }.
  //
  // Create our data holder that holds all the things shared across every step
  // of the iterator. In particular, this holds the remainingElementsCount
  // (as an integer reserved slot), the array of values, and the resolve
  // function from our PromiseCapability.
  Rooted<PromiseCombinatorDataHolder*> dataHolder(cx);
  dataHolder = PromiseCombinatorDataHolder::New(
      cx, resultCapability.promise(), values, resultCapability.resolve());
  if (!dataHolder) {
    return false;
  }

  // Step 3. Let index be 0.
  uint32_t index = 0;

  auto getResolveAndReject = [cx, &values, &dataHolder, &index](
                                 MutableHandleValue resolveFunVal,
                                 MutableHandleValue rejectFunVal) {
    // Step 4.h. Append undefined to values.
    if (!values.pushUndefined(cx)) {
      return false;
    }

    auto PromiseAllSettledResolveElementFunction =
        PromiseAllSettledElementFunction<
            PromiseAllSettledElementFunctionKind::Resolve>;
    auto PromiseAllSettledRejectElementFunction =
        PromiseAllSettledElementFunction<
            PromiseAllSettledElementFunctionKind::Reject>;

    // Steps 4.j-r.
    JSFunction* resolveFunc = NewPromiseCombinatorElementFunction(
        cx, PromiseAllSettledResolveElementFunction, dataHolder, index);
    if (!resolveFunc) {
      return false;
    }
    resolveFunVal.setObject(*resolveFunc);

    // Steps 4.s-z.
    JSFunction* rejectFunc = NewPromiseCombinatorElementFunction(
        cx, PromiseAllSettledRejectElementFunction, dataHolder, index);
    if (!rejectFunc) {
      return false;
    }
    rejectFunVal.setObject(*rejectFunc);

    // Step 4.aa. Set remainingElementsCount.[[Value]] to
    //            remainingElementsCount.[[Value]] + 1.
    dataHolder->increaseRemainingCount();

    // Step 4.ac. Set index to index + 1.
    index++;
    MOZ_ASSERT(index > 0);

    return true;
  };

  // Steps 4.
  if (!CommonPerformPromiseCombinator(
          cx, iterator, C, resultCapability.promise(), promiseResolve, done,
          true, getResolveAndReject)) {
    return false;
  }

  // Step 4.d.ii. Set remainingElementsCount.[[Value]] to
  //              remainingElementsCount.[[Value]] - 1.
  int32_t remainingCount = dataHolder->decreaseRemainingCount();

  // Step 4.d.iii. If remainingElementsCount.[[Value]] is 0, then
  if (remainingCount == 0) {
    // Step 4.d.iii.1. Let valuesArray be ! CreateArrayFromList(values).
    // (already performed)

    // Step 4.d.iii.2. Perform
    //                 ? Call(resultCapability.[[Resolve]], undefined,
    //                        « valuesArray »).
    return CallPromiseResolveFunction(cx, resultCapability.resolve(),
                                      values.value(),
                                      resultCapability.promise());
  }

  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Unified implementation of
 *
 * Promise.allSettled Resolve Element Functions
 * https://tc39.es/ecma262/#sec-promise.allsettled-resolve-element-functions
 * Promise.allSettled Reject Element Functions
 * https://tc39.es/ecma262/#sec-promise.allsettled-reject-element-functions
 */
template <PromiseAllSettledElementFunctionKind Kind>
static bool PromiseAllSettledElementFunction(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue valueOrReason = args.get(0);

  // Steps 1-5.
  Rooted<PromiseCombinatorDataHolder*> data(cx);
  uint32_t index;
  if (PromiseCombinatorElementFunctionAlreadyCalled(args, &data, &index)) {
    args.rval().setUndefined();
    return true;
  }

  // Step 6. Let values be F.[[Values]].
  Rooted<PromiseCombinatorElements> values(cx);
  if (!GetPromiseCombinatorElements(cx, data, &values)) {
    return false;
  }

  // Step 2. Let alreadyCalled be F.[[AlreadyCalled]].
  // Step 3. If alreadyCalled.[[Value]] is true, return undefined.
  //
  // The already-called check above only handles the case when |this| function
  // is called repeatedly, so we still need to check if the other pair of this
  // resolving function was already called:
  // We use the element value as a signal for whether the Promise was already
  // fulfilled. Upon resolution, it's set to the result object created below.
  if (!values.unwrappedArray()->getDenseElement(index).isUndefined()) {
    args.rval().setUndefined();
    return true;
  }

  // Step 9. Let obj be ! OrdinaryObjectCreate(%Object.prototype%).
  Rooted<PlainObject*> obj(cx, NewPlainObject(cx));
  if (!obj) {
    return false;
  }

  // Promise.allSettled Resolve Element Functions
  // Step 10. Perform ! CreateDataPropertyOrThrow(obj, "status", "fulfilled").
  // Promise.allSettled Reject Element Functions
  // Step 10. Perform ! CreateDataPropertyOrThrow(obj, "status", "rejected").
  RootedId id(cx, NameToId(cx->names().status));
  RootedValue statusValue(cx);
  if (Kind == PromiseAllSettledElementFunctionKind::Resolve) {
    statusValue.setString(cx->names().fulfilled);
  } else {
    statusValue.setString(cx->names().rejected);
  }
  if (!NativeDefineDataProperty(cx, obj, id, statusValue, JSPROP_ENUMERATE)) {
    return false;
  }

  // Promise.allSettled Resolve Element Functions
  // Step 11. Perform ! CreateDataPropertyOrThrow(obj, "value", x).
  // Promise.allSettled Reject Element Functions
  // Step 11. Perform ! CreateDataPropertyOrThrow(obj, "reason", x).
  if (Kind == PromiseAllSettledElementFunctionKind::Resolve) {
    id = NameToId(cx->names().value);
  } else {
    id = NameToId(cx->names().reason);
  }
  if (!NativeDefineDataProperty(cx, obj, id, valueOrReason, JSPROP_ENUMERATE)) {
    return false;
  }

  // Step 12. Set values[index] to obj.
  RootedValue objVal(cx, ObjectValue(*obj));
  if (!values.setElement(cx, index, objVal)) {
    return false;
  }

  // (reordered)
  // Step 8. Let remainingElementsCount be F.[[RemainingElements]].
  //
  // Step 13. Set remainingElementsCount.[[Value]] to
  // remainingElementsCount.[[Value]] - 1.
  uint32_t remainingCount = data->decreaseRemainingCount();

  // Step 14. If remainingElementsCount.[[Value]] is 0, then
  if (remainingCount == 0) {
    // Step 14.a. Let valuesArray be ! CreateArrayFromList(values).
    // (already performed)

    // (reordered)
    // Step 7. Let promiseCapability be F.[[Capability]].
    //
    // Step 14.b. Return
    //            ? Call(promiseCapability.[[Resolve]], undefined,
    //                   « valuesArray »).
    RootedObject resolveAllFun(cx, data->resolveOrRejectObj());
    RootedObject promiseObj(cx, data->promiseObj());
    if (!CallPromiseResolveFunction(cx, resolveAllFun, values.value(),
                                    promiseObj)) {
      return false;
    }
  }

  // Step 15. Return undefined.
  args.rval().setUndefined();
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.any ( iterable )
 * https://tc39.es/ecma262/#sec-promise.any
 */
static bool Promise_static_any(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CommonPromiseCombinator(cx, args, CombinatorKind::Any);
}

static bool PromiseAnyRejectElementFunction(JSContext* cx, unsigned argc,
                                            Value* vp);

static void ThrowAggregateError(JSContext* cx,
                                Handle<PromiseCombinatorElements> errors,
                                HandleObject promise);

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.any ( iterable )
 * https://tc39.es/ecma262/#sec-promise.any
 * PerformPromiseAny ( iteratorRecord, constructor, resultCapability,
 *                     promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiseany
 */
[[nodiscard]] static bool PerformPromiseAny(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done) {
  *done = false;

  // Step 1. Let C be the this value.
  MOZ_ASSERT(C->isConstructor());

  // Step 2. Let promiseCapability be ? NewPromiseCapability(C).
  // (omitted).

  // Step 3. Let promiseResolve be GetPromiseResolve(C).
  Rooted<PromiseCombinatorElements> errors(cx);
  if (!NewPromiseCombinatorElements(cx, resultCapability, &errors)) {
    return false;
  }

  // Step 4.
  // Create our data holder that holds all the things shared across every step
  // of the iterator. In particular, this holds the remainingElementsCount (as
  // an integer reserved slot), the array of errors, and the reject function
  // from our PromiseCapability.
  Rooted<PromiseCombinatorDataHolder*> dataHolder(cx);
  dataHolder = PromiseCombinatorDataHolder::New(
      cx, resultCapability.promise(), errors, resultCapability.reject());
  if (!dataHolder) {
    return false;
  }

  // PerformPromiseAny
  // Step 3. Let index be 0.
  uint32_t index = 0;

  auto getResolveAndReject = [cx, &resultCapability, &errors, &dataHolder,
                              &index](MutableHandleValue resolveFunVal,
                                      MutableHandleValue rejectFunVal) {
    // Step 4.h. Append undefined to errors.
    if (!errors.pushUndefined(cx)) {
      return false;
    }

    // Steps 4.j-q.
    JSFunction* rejectFunc = NewPromiseCombinatorElementFunction(
        cx, PromiseAnyRejectElementFunction, dataHolder, index);
    if (!rejectFunc) {
      return false;
    }

    // Step 4.r. Set remainingElementsCount.[[Value]] to
    //           remainingElementsCount.[[Value]] + 1.
    dataHolder->increaseRemainingCount();

    // Step 4.t. Set index to index + 1.
    index++;
    MOZ_ASSERT(index > 0);

    resolveFunVal.setObject(*resultCapability.resolve());
    rejectFunVal.setObject(*rejectFunc);
    return true;
  };

  // BlockOnPromise fast path requires the passed onFulfilled function doesn't
  // return an object value, because otherwise the skipped promise creation is
  // detectable due to missing property lookups.
  bool isDefaultResolveFn =
      IsNativeFunction(resultCapability.resolve(), ResolvePromiseFunction);

  // Steps 4.
  if (!CommonPerformPromiseCombinator(
          cx, iterator, C, resultCapability.promise(), promiseResolve, done,
          isDefaultResolveFn, getResolveAndReject)) {
    return false;
  }

  // Step 4.d.ii. Set remainingElementsCount.[[Value]] to
  //              remainingElementsCount.[[Value]] - 1.
  int32_t remainingCount = dataHolder->decreaseRemainingCount();

  // Step 4.d.iii. If remainingElementsCount.[[Value]] is 0, then
  if (remainingCount == 0) {
    ThrowAggregateError(cx, errors, resultCapability.promise());
    return false;
  }

  // Step 4.d.iv. Return resultCapability.[[Promise]].
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.any Reject Element Functions
 * https://tc39.es/ecma262/#sec-promise.any-reject-element-functions
 */
static bool PromiseAnyRejectElementFunction(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue xVal = args.get(0);

  // Steps 1-5.
  Rooted<PromiseCombinatorDataHolder*> data(cx);
  uint32_t index;
  if (PromiseCombinatorElementFunctionAlreadyCalled(args, &data, &index)) {
    args.rval().setUndefined();
    return true;
  }

  // Step 6.
  Rooted<PromiseCombinatorElements> errors(cx);
  if (!GetPromiseCombinatorElements(cx, data, &errors)) {
    return false;
  }

  // Step 9.
  if (!errors.setElement(cx, index, xVal)) {
    return false;
  }

  // Steps 8, 10.
  uint32_t remainingCount = data->decreaseRemainingCount();

  // Step 11.
  if (remainingCount == 0) {
    // Step 7 (Adapted to work with PromiseCombinatorDataHolder's layout).
    RootedObject rejectFun(cx, data->resolveOrRejectObj());
    RootedObject promiseObj(cx, data->promiseObj());

    ThrowAggregateError(cx, errors, promiseObj);

    RootedValue reason(cx);
    Rooted<SavedFrame*> stack(cx);
    if (!MaybeGetAndClearExceptionAndStack(cx, &reason, &stack)) {
      return false;
    }

    if (!CallPromiseRejectFunction(cx, rejectFun, reason, promiseObj, stack,
                                   UnhandledRejectionBehavior::Report)) {
      return false;
    }
  }

  // Step 12.
  args.rval().setUndefined();
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseAny ( iteratorRecord, constructor, resultCapability,
 *                     promiseResolve )
 * https://tc39.es/ecma262/#sec-performpromiseany
 *
 * Steps 4.d.iii.1-3
 */
static void ThrowAggregateError(JSContext* cx,
                                Handle<PromiseCombinatorElements> errors,
                                HandleObject promise) {
  MOZ_ASSERT(!cx->isExceptionPending());

  // Create the AggregateError in the same realm as the array object.
  AutoRealm ar(cx, errors.unwrappedArray());

  RootedObject allocationSite(cx);
  mozilla::Maybe<JS::AutoSetAsyncStackForNewCalls> asyncStack;

  // Provide a more useful error stack if possible: This function is typically
  // called from Promise job queue, which doesn't have any JS frames on the
  // stack. So when we create the AggregateError below, its stack property will
  // be set to the empty string, which makes it harder to debug the error cause.
  // To avoid this situation set-up an async stack based on the Promise
  // allocation site, which should point to calling site of |Promise.any|.
  if (promise->is<PromiseObject>()) {
    allocationSite = promise->as<PromiseObject>().allocationSite();
    if (allocationSite) {
      asyncStack.emplace(
          cx, allocationSite, "Promise.any",
          JS::AutoSetAsyncStackForNewCalls::AsyncCallKind::IMPLICIT);
    }
  }

  // Step 4.d.iii.1. Let error be a newly created AggregateError object.
  //
  // AutoSetAsyncStackForNewCalls requires a new activation before it takes
  // effect, so call into the self-hosting helper to set-up new call frames.
  RootedValue error(cx);
  if (!GetAggregateError(cx, JSMSG_PROMISE_ANY_REJECTION, &error)) {
    return;
  }

  // Step 4.d.iii.2. Perform ! DefinePropertyOrThrow(
  //                   error, "errors", PropertyDescriptor {
  //                     [[Configurable]]: true, [[Enumerable]]: false,
  //                     [[Writable]]: true,
  //                     [[Value]]: ! CreateArrayFromList(errors) }).
  //
  // |error| isn't guaranteed to be an AggregateError in case of OOM or stack
  // overflow.
  Rooted<SavedFrame*> stack(cx);
  if (error.isObject() && error.toObject().is<ErrorObject>()) {
    Rooted<ErrorObject*> errorObj(cx, &error.toObject().as<ErrorObject>());
    if (errorObj->type() == JSEXN_AGGREGATEERR) {
      RootedValue errorsVal(cx, JS::ObjectValue(*errors.unwrappedArray()));
      if (!NativeDefineDataProperty(cx, errorObj, cx->names().errors, errorsVal,
                                    0)) {
        return;
      }

      // Adopt the existing saved frames when present.
      if (JSObject* errorStack = errorObj->stack()) {
        stack = &errorStack->as<SavedFrame>();
      }
    }
  }

  // Step 4.d.iii.3. Return ThrowCompletion(error).
  cx->setPendingException(error, stack);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Unified implementation of
 *
 * Promise.reject ( r )
 * https://tc39.es/ecma262/#sec-promise.reject
 * NewPromiseCapability ( C )
 * https://tc39.es/ecma262/#sec-newpromisecapability
 * Promise.resolve ( x )
 * https://tc39.es/ecma262/#sec-promise.resolve
 * PromiseResolve ( C, x )
 * https://tc39.es/ecma262/#sec-promise-resolve
 */
[[nodiscard]] static JSObject* CommonStaticResolveRejectImpl(
    JSContext* cx, HandleValue thisVal, HandleValue argVal,
    ResolutionMode mode) {
  // Promise.reject
  // Step 1. Let C be the this value.
  // Step 2. Let promiseCapability be ? NewPromiseCapability(C).
  //
  // Promise.reject => NewPromiseCapability
  // Step 1. If IsConstructor(C) is false, throw a TypeError exception.
  //
  // Promise.resolve
  // Step 1. Let C be the this value.
  // Step 2. If Type(C) is not Object, throw a TypeError exception.
  if (!thisVal.isObject()) {
    const char* msg = mode == ResolveMode ? "Receiver of Promise.resolve call"
                                          : "Receiver of Promise.reject call";
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, msg);
    return nullptr;
  }
  RootedObject C(cx, &thisVal.toObject());

  // Promise.resolve
  // Step 3. Return ? PromiseResolve(C, x).
  //
  // PromiseResolve
  // Step 1. Assert: Type(C) is Object.
  // (implicit)
  if (mode == ResolveMode && argVal.isObject()) {
    RootedObject xObj(cx, &argVal.toObject());
    bool isPromise = false;
    if (xObj->is<PromiseObject>()) {
      isPromise = true;
    } else if (IsWrapper(xObj)) {
      // Treat instances of Promise from other compartments as Promises
      // here, too.
      // It's important to do the GetProperty for the `constructor`
      // below through the wrapper, because wrappers can change the
      // outcome, so instead of unwrapping and then performing the
      // GetProperty, just check here and then operate on the original
      // object again.
      if (xObj->canUnwrapAs<PromiseObject>()) {
        isPromise = true;
      }
    }

    // PromiseResolve
    // Step 2. If IsPromise(x) is true, then
    if (isPromise) {
      // Step 2.a. Let xConstructor be ? Get(x, "constructor").
      RootedValue ctorVal(cx);
      if (!GetProperty(cx, xObj, xObj, cx->names().constructor, &ctorVal)) {
        return nullptr;
      }

      // Step 2.b. If SameValue(xConstructor, C) is true, return x.
      if (ctorVal == thisVal) {
        return xObj;
      }
    }
  }

  // Promise.reject
  // Step 2. Let promiseCapability be ? NewPromiseCapability(C).
  // PromiseResolve
  // Step 3. Let promiseCapability be ? NewPromiseCapability(C).
  Rooted<PromiseCapability> capability(cx);
  if (!NewPromiseCapability(cx, C, &capability, true)) {
    return nullptr;
  }

  HandleObject promise = capability.promise();
  if (mode == ResolveMode) {
    // PromiseResolve
    // Step 4. Perform ? Call(promiseCapability.[[Resolve]], undefined, « x »).
    if (!CallPromiseResolveFunction(cx, capability.resolve(), argVal,
                                    promise)) {
      return nullptr;
    }
  } else {
    // Promise.reject
    // Step 3. Perform ? Call(promiseCapability.[[Reject]], undefined, « r »).
    if (!CallPromiseRejectFunction(cx, capability.reject(), argVal, promise,
                                   nullptr,
                                   UnhandledRejectionBehavior::Report)) {
      return nullptr;
    }
  }

  // Promise.reject
  // Step 4. Return promiseCapability.[[Promise]].
  // PromiseResolve
  // Step 5. Return promiseCapability.[[Promise]].
  return promise;
}

[[nodiscard]] JSObject* js::PromiseResolve(JSContext* cx,
                                           HandleObject constructor,
                                           HandleValue value) {
  RootedValue C(cx, ObjectValue(*constructor));
  return CommonStaticResolveRejectImpl(cx, C, value, ResolveMode);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.reject ( r )
 * https://tc39.es/ecma262/#sec-promise.reject
 */
static bool Promise_reject(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue thisVal = args.thisv();
  HandleValue argVal = args.get(0);
  JSObject* result =
      CommonStaticResolveRejectImpl(cx, thisVal, argVal, RejectMode);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.reject ( r )
 * https://tc39.es/ecma262/#sec-promise.reject
 *
 * Unforgeable version.
 */
/* static */
PromiseObject* PromiseObject::unforgeableReject(JSContext* cx,
                                                HandleValue value) {
  cx->check(value);

  // Step 1. Let C be the this value.
  // Step 2. Let promiseCapability be ? NewPromiseCapability(C).
  Rooted<PromiseObject*> promise(
      cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
  if (!promise) {
    return nullptr;
  }

  MOZ_ASSERT(promise->state() == JS::PromiseState::Pending);
  MOZ_ASSERT(IsPromiseWithDefaultResolvingFunction(promise));

  // Step 3. Perform ? Call(promiseCapability.[[Reject]], undefined, « r »).
  if (!RejectPromiseInternal(cx, promise, value)) {
    return nullptr;
  }

  // Step 4. Return promiseCapability.[[Promise]].
  return promise;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.resolve ( x )
 * https://tc39.es/ecma262/#sec-promise.resolve
 */
bool js::Promise_static_resolve(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue thisVal = args.thisv();
  HandleValue argVal = args.get(0);
  JSObject* result =
      CommonStaticResolveRejectImpl(cx, thisVal, argVal, ResolveMode);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.resolve ( x )
 * https://tc39.es/ecma262/#sec-promise.resolve
 *
 * Unforgeable version.
 */
/* static */
JSObject* PromiseObject::unforgeableResolve(JSContext* cx, HandleValue value) {
  JSObject* promiseCtor = JS::GetPromiseConstructor(cx);
  if (!promiseCtor) {
    return nullptr;
  }
  RootedValue cVal(cx, ObjectValue(*promiseCtor));
  return CommonStaticResolveRejectImpl(cx, cVal, value, ResolveMode);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.resolve ( x )
 * https://tc39.es/ecma262/#sec-promise.resolve
 * PromiseResolve ( C, x )
 * https://tc39.es/ecma262/#sec-promise-resolve
 *
 * Unforgeable version, where `x` is guaranteed not to be a promise.
 */
/* static */
PromiseObject* PromiseObject::unforgeableResolveWithNonPromise(
    JSContext* cx, HandleValue value) {
  cx->check(value);

#ifdef DEBUG
  auto IsPromise = [](HandleValue value) {
    if (!value.isObject()) {
      return false;
    }

    JSObject* obj = &value.toObject();
    if (obj->is<PromiseObject>()) {
      return true;
    }

    if (!IsWrapper(obj)) {
      return false;
    }

    return obj->canUnwrapAs<PromiseObject>();
  };
  MOZ_ASSERT(!IsPromise(value), "must use unforgeableResolve with this value");
#endif

  // Promise.resolve
  // Step 3. Return ? PromiseResolve(C, x).

  // PromiseResolve
  // Step 2. Let promiseCapability be ? NewPromiseCapability(C).
  Rooted<PromiseObject*> promise(
      cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
  if (!promise) {
    return nullptr;
  }

  MOZ_ASSERT(promise->state() == JS::PromiseState::Pending);
  MOZ_ASSERT(IsPromiseWithDefaultResolvingFunction(promise));

  // PromiseResolve
  // Step 3. Perform ? Call(promiseCapability.[[Resolve]], undefined, « x »).
  if (!ResolvePromiseInternal(cx, promise, value)) {
    return nullptr;
  }

  // PromiseResolve
  // Step 4. Return promiseCapability.[[Promise]].
  return promise;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * get Promise [ @@species ]
 * https://tc39.es/ecma262/#sec-get-promise-@@species
 */
bool js::Promise_static_species(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Return the this value.
  args.rval().set(args.thisv());
  return true;
}

enum class IncumbentGlobalObject {
  // Do not use the incumbent global, this is a special case used by the
  // debugger.
  No,

  // Use incumbent global, this is the normal operation.
  Yes
};

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseThen ( promise, onFulfilled, onRejected
 *                      [ , resultCapability ] )
 * https://tc39.es/ecma262/#sec-performpromisethen
 *
 * Steps 7-8 for creating PromiseReaction record.
 * We use single object for both fulfillReaction and rejectReaction.
 */
static PromiseReactionRecord* NewReactionRecord(
    JSContext* cx, Handle<PromiseCapability> resultCapability,
    HandleValue onFulfilled, HandleValue onRejected,
    IncumbentGlobalObject incumbentGlobalObjectOption) {
#ifdef DEBUG
  if (resultCapability.promise()) {
    if (incumbentGlobalObjectOption == IncumbentGlobalObject::Yes) {
      if (resultCapability.promise()->is<PromiseObject>()) {
        // If `resultCapability.promise` is a Promise object,
        // `resultCapability.{resolve,reject}` may be optimized out,
        // but if they're not, they should be callable.
        MOZ_ASSERT_IF(resultCapability.resolve(),
                      IsCallable(resultCapability.resolve()));
        MOZ_ASSERT_IF(resultCapability.reject(),
                      IsCallable(resultCapability.reject()));
      } else {
        // If `resultCapability.promise` is a non-Promise object
        // (including wrapped Promise object),
        // `resultCapability.{resolve,reject}` should be callable.
        MOZ_ASSERT(resultCapability.resolve());
        MOZ_ASSERT(IsCallable(resultCapability.resolve()));
        MOZ_ASSERT(resultCapability.reject());
        MOZ_ASSERT(IsCallable(resultCapability.reject()));
      }
    } else {
      // For debugger usage, `resultCapability.promise` should be a
      // maybe-wrapped Promise object. The other fields are not used.
      //
      // This is the only case where we allow `resolve` and `reject` to
      // be null when the `promise` field is not a PromiseObject.
      JSObject* unwrappedPromise = UncheckedUnwrap(resultCapability.promise());
      MOZ_ASSERT(unwrappedPromise->is<PromiseObject>());
      MOZ_ASSERT(!resultCapability.resolve());
      MOZ_ASSERT(!resultCapability.reject());
    }
  } else {
    // `resultCapability.promise` is null for the following cases:
    //   * resulting Promise is known to be unused
    //   * Async Function
    //   * Async Generator
    // In any case, other fields are also not used.
    MOZ_ASSERT(!resultCapability.resolve());
    MOZ_ASSERT(!resultCapability.reject());
    MOZ_ASSERT(incumbentGlobalObjectOption == IncumbentGlobalObject::Yes);
  }
#endif

  // Ensure the onFulfilled handler has the expected type.
  MOZ_ASSERT(onFulfilled.isInt32() || onFulfilled.isObjectOrNull());
  MOZ_ASSERT_IF(onFulfilled.isObject(), IsCallable(onFulfilled));
  MOZ_ASSERT_IF(onFulfilled.isInt32(),
                0 <= onFulfilled.toInt32() &&
                    onFulfilled.toInt32() < int32_t(PromiseHandler::Limit));

  // Ensure the onRejected handler has the expected type.
  MOZ_ASSERT(onRejected.isInt32() || onRejected.isObjectOrNull());
  MOZ_ASSERT_IF(onRejected.isObject(), IsCallable(onRejected));
  MOZ_ASSERT_IF(onRejected.isInt32(),
                0 <= onRejected.toInt32() &&
                    onRejected.toInt32() < int32_t(PromiseHandler::Limit));

  // Handlers must either both be present or both be absent.
  MOZ_ASSERT(onFulfilled.isNull() == onRejected.isNull());

  RootedObject incumbentGlobalObject(cx);
  if (incumbentGlobalObjectOption == IncumbentGlobalObject::Yes) {
    if (!GetObjectFromIncumbentGlobal(cx, &incumbentGlobalObject)) {
      return nullptr;
    }
  }

  PromiseReactionRecord* reaction =
      NewBuiltinClassInstance<PromiseReactionRecord>(cx);
  if (!reaction) {
    return nullptr;
  }

  cx->check(resultCapability.promise());
  cx->check(onFulfilled);
  cx->check(onRejected);
  cx->check(resultCapability.resolve());
  cx->check(resultCapability.reject());
  cx->check(incumbentGlobalObject);

  // Step 7. Let fulfillReaction be the PromiseReaction
  //         { [[Capability]]: resultCapability, [[Type]]: Fulfill,
  //           [[Handler]]: onFulfilledJobCallback }.
  // Step 8. Let rejectReaction be the PromiseReaction
  //         { [[Capability]]: resultCapability, [[Type]]: Reject,
  //           [[Handler]]: onRejectedJobCallback }.

  // See comments for ReactionRecordSlots for the relation between
  // spec record fields and PromiseReactionRecord slots.
  reaction->setFixedSlot(ReactionRecordSlot_Promise,
                         ObjectOrNullValue(resultCapability.promise()));
  // We set [[Type]] in EnqueuePromiseReactionJob, by calling
  // setTargetStateAndHandlerArg.
  reaction->setFixedSlot(ReactionRecordSlot_Flags, Int32Value(0));
  reaction->setFixedSlot(ReactionRecordSlot_OnFulfilled, onFulfilled);
  reaction->setFixedSlot(ReactionRecordSlot_OnRejected, onRejected);
  reaction->setFixedSlot(ReactionRecordSlot_Resolve,
                         ObjectOrNullValue(resultCapability.resolve()));
  reaction->setFixedSlot(ReactionRecordSlot_Reject,
                         ObjectOrNullValue(resultCapability.reject()));
  reaction->setFixedSlot(ReactionRecordSlot_IncumbentGlobalObject,
                         ObjectOrNullValue(incumbentGlobalObject));

  return reaction;
}

static bool IsPromiseSpecies(JSContext* cx, JSFunction* species) {
  return species->maybeNative() == Promise_static_species;
}

// Whether to create a promise as the return value of Promise#{then,catch}.
// If the return value is known to be unused, and if the operation is known
// to be unobservable, we can skip creating the promise.
enum class CreateDependentPromise { Always, SkipIfCtorUnobservable };

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.then ( onFulfilled, onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.then
 *
 * Steps 3-4.
 */
static bool PromiseThenNewPromiseCapability(
    JSContext* cx, HandleObject promiseObj,
    CreateDependentPromise createDependent,
    MutableHandle<PromiseCapability> resultCapability) {
  // Step 3. Let C be ? SpeciesConstructor(promise, %Promise%).
  RootedObject C(cx, SpeciesConstructor(cx, promiseObj, JSProto_Promise,
                                        IsPromiseSpecies));
  if (!C) {
    return false;
  }

  if (createDependent != CreateDependentPromise::Always &&
      IsNativeFunction(C, PromiseConstructor)) {
    return true;
  }

  // Step 4. Let resultCapability be ? NewPromiseCapability(C).
  if (!NewPromiseCapability(cx, C, resultCapability, true)) {
    return false;
  }

  RootedObject unwrappedPromise(cx, promiseObj);
  if (IsWrapper(promiseObj)) {
    unwrappedPromise = UncheckedUnwrap(promiseObj);
  }
  RootedObject unwrappedNewPromise(cx, resultCapability.promise());
  if (IsWrapper(resultCapability.promise())) {
    unwrappedNewPromise = UncheckedUnwrap(resultCapability.promise());
  }
  if (unwrappedPromise->is<PromiseObject>() &&
      unwrappedNewPromise->is<PromiseObject>()) {
    unwrappedNewPromise->as<PromiseObject>().copyUserInteractionFlagsFrom(
        *unwrappedPromise.as<PromiseObject>());
  }

  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.then ( onFulfilled, onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.then
 *
 * Steps 3-5.
 */
[[nodiscard]] PromiseObject* js::OriginalPromiseThen(JSContext* cx,
                                                     HandleObject promiseObj,
                                                     HandleObject onFulfilled,
                                                     HandleObject onRejected) {
  cx->check(promiseObj);
  cx->check(onFulfilled);
  cx->check(onRejected);

  RootedValue promiseVal(cx, ObjectValue(*promiseObj));
  Rooted<PromiseObject*> unwrappedPromise(
      cx,
      UnwrapAndTypeCheckValue<PromiseObject>(cx, promiseVal, [cx, promiseObj] {
        JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr,
                                   JSMSG_INCOMPATIBLE_PROTO, "Promise", "then",
                                   promiseObj->getClass()->name);
      }));
  if (!unwrappedPromise) {
    return nullptr;
  }

  // Step 3. Let C be ? SpeciesConstructor(promise, %Promise%).
  // Step 4. Let resultCapability be ? NewPromiseCapability(C).
  Rooted<PromiseObject*> newPromise(
      cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
  if (!newPromise) {
    return nullptr;
  }
  newPromise->copyUserInteractionFlagsFrom(*unwrappedPromise);

  Rooted<PromiseCapability> resultCapability(cx);
  resultCapability.promise().set(newPromise);

  // Step 5. Return PerformPromiseThen(promise, onFulfilled, onRejected,
  //                                   resultCapability).
  {
    RootedValue onFulfilledVal(cx, ObjectOrNullValue(onFulfilled));
    RootedValue onRejectedVal(cx, ObjectOrNullValue(onRejected));
    if (!PerformPromiseThen(cx, unwrappedPromise, onFulfilledVal, onRejectedVal,
                            resultCapability)) {
      return nullptr;
    }
  }

  return newPromise;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.then ( onFulfilled, onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.then
 *
 * Steps 3-5.
 */
[[nodiscard]] static bool OriginalPromiseThenWithoutSettleHandlers(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseObject*> promiseToResolve) {
  cx->check(promise);

  // Step 3. Let C be ? SpeciesConstructor(promise, %Promise%).
  // Step 4. Let resultCapability be ? NewPromiseCapability(C).
  Rooted<PromiseCapability> resultCapability(cx);
  if (!PromiseThenNewPromiseCapability(
          cx, promise, CreateDependentPromise::SkipIfCtorUnobservable,
          &resultCapability)) {
    return false;
  }

  // Step 5. Return PerformPromiseThen(promise, onFulfilled, onRejected,
  //                                   resultCapability).
  return PerformPromiseThenWithoutSettleHandlers(cx, promise, promiseToResolve,
                                                 resultCapability);
}

[[nodiscard]] static bool PerformPromiseThenWithReaction(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseReactionRecord*> reaction);

[[nodiscard]] bool js::ReactToUnwrappedPromise(
    JSContext* cx, Handle<PromiseObject*> unwrappedPromise,
    HandleObject onFulfilled_, HandleObject onRejected_,
    UnhandledRejectionBehavior behavior) {
  cx->check(onFulfilled_);
  cx->check(onRejected_);

  MOZ_ASSERT_IF(onFulfilled_, IsCallable(onFulfilled_));
  MOZ_ASSERT_IF(onRejected_, IsCallable(onRejected_));

  RootedValue onFulfilled(
      cx, onFulfilled_ ? ObjectValue(*onFulfilled_)
                       : Int32Value(int32_t(PromiseHandler::Identity)));

  RootedValue onRejected(
      cx, onRejected_ ? ObjectValue(*onRejected_)
                      : Int32Value(int32_t(PromiseHandler::Thrower)));

  Rooted<PromiseCapability> resultCapability(cx);
  MOZ_ASSERT(!resultCapability.promise());

  Rooted<PromiseReactionRecord*> reaction(
      cx, NewReactionRecord(cx, resultCapability, onFulfilled, onRejected,
                            IncumbentGlobalObject::Yes));
  if (!reaction) {
    return false;
  }

  if (behavior == UnhandledRejectionBehavior::Ignore) {
    reaction->setShouldIgnoreUnhandledRejection();
  }

  return PerformPromiseThenWithReaction(cx, unwrappedPromise, reaction);
}

static bool CanCallOriginalPromiseThenBuiltin(JSContext* cx,
                                              HandleValue promise) {
  return promise.isObject() && promise.toObject().is<PromiseObject>() &&
         cx->realm()->promiseLookup.isDefaultInstance(
             cx, &promise.toObject().as<PromiseObject>());
}

static MOZ_ALWAYS_INLINE bool IsPromiseThenOrCatchRetValImplicitlyUsed(
    JSContext* cx, PromiseObject* promise) {
  // Embedding requires the return value of then/catch as
  // `enqueuePromiseJob` parameter, to propaggate the user-interaction.
  // We cannot optimize out the return value if the flag is set by embedding.
  if (promise->requiresUserInteractionHandling()) {
    return true;
  }

  // The returned promise of Promise#then and Promise#catch contains
  // stack info if async stack is enabled.  Even if their return value is not
  // used explicitly in the script, the stack info is observable in devtools
  // and profilers.  We shouldn't apply the optimization not to allocate the
  // returned Promise object if the it's implicitly used by them.
  if (!cx->options().asyncStack()) {
    return false;
  }

  // If devtools is opened, the current realm will become debuggee.
  if (cx->realm()->isDebuggee()) {
    return true;
  }

  // There are 2 profilers, and they can be independently enabled.
  if (cx->runtime()->geckoProfiler().enabled()) {
    return true;
  }
  if (JS::IsProfileTimelineRecordingEnabled()) {
    return true;
  }

  // The stack is also observable from Error#stack, but we don't care since
  // it's nonstandard feature.
  return false;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.then ( onFulfilled, onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.then
 *
 * Steps 3-5.
 */
static bool OriginalPromiseThenBuiltin(JSContext* cx, HandleValue promiseVal,
                                       HandleValue onFulfilled,
                                       HandleValue onRejected,
                                       MutableHandleValue rval,
                                       bool rvalExplicitlyUsed) {
  cx->check(promiseVal, onFulfilled, onRejected);
  MOZ_ASSERT(CanCallOriginalPromiseThenBuiltin(cx, promiseVal));

  Rooted<PromiseObject*> promise(cx,
                                 &promiseVal.toObject().as<PromiseObject>());

  bool rvalUsed = rvalExplicitlyUsed ||
                  IsPromiseThenOrCatchRetValImplicitlyUsed(cx, promise);

  // Step 3. Let C be ? SpeciesConstructor(promise, %Promise%).
  // Step 4. Let resultCapability be ? NewPromiseCapability(C).
  Rooted<PromiseCapability> resultCapability(cx);
  if (rvalUsed) {
    PromiseObject* resultPromise =
        CreatePromiseObjectWithoutResolutionFunctions(cx);
    if (!resultPromise) {
      return false;
    }

    resultPromise->copyUserInteractionFlagsFrom(
        promiseVal.toObject().as<PromiseObject>());
    resultCapability.promise().set(resultPromise);
  }

  // Step 5. Return PerformPromiseThen(promise, onFulfilled, onRejected,
  //                                   resultCapability).
  if (!PerformPromiseThen(cx, promise, onFulfilled, onRejected,
                          resultCapability)) {
    return false;
  }

  if (rvalUsed) {
    rval.setObject(*resultCapability.promise());
  } else {
    rval.setUndefined();
  }
  return true;
}

[[nodiscard]] bool js::RejectPromiseWithPendingError(
    JSContext* cx, Handle<PromiseObject*> promise) {
  cx->check(promise);

  if (!cx->isExceptionPending()) {
    // Reject the promise, but also propagate this uncatchable error.
    (void)PromiseObject::reject(cx, promise, UndefinedHandleValue);
    return false;
  }

  RootedValue exn(cx);
  if (!GetAndClearException(cx, &exn)) {
    return false;
  }
  return PromiseObject::reject(cx, promise, exn);
}

// Some async/await functions are implemented here instead of
// js/src/builtin/AsyncFunction.cpp, to call Promise internal functions.

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Runtime Semantics: EvaluateAsyncFunctionBody
 * AsyncFunctionBody : FunctionBody
 * https://tc39.es/ecma262/#sec-runtime-semantics-evaluateasyncfunctionbody
 *
 * Runtime Semantics: EvaluateAsyncConciseBody
 * AsyncConciseBody : ExpressionBody
 * https://tc39.es/ecma262/#sec-runtime-semantics-evaluateasyncconcisebody
 */
[[nodiscard]] PromiseObject* js::CreatePromiseObjectForAsync(JSContext* cx) {
  // Step 1. Let promiseCapability be ! NewPromiseCapability(%Promise%).
  PromiseObject* promise = CreatePromiseObjectWithoutResolutionFunctions(cx);
  if (!promise) {
    return nullptr;
  }

  AddPromiseFlags(*promise, PROMISE_FLAG_ASYNC);
  return promise;
}

bool js::IsPromiseForAsyncFunctionOrGenerator(JSObject* promise) {
  return promise->is<PromiseObject>() &&
         PromiseHasAnyFlag(promise->as<PromiseObject>(), PROMISE_FLAG_ASYNC);
}

[[nodiscard]] PromiseObject* js::CreatePromiseObjectForAsyncGenerator(
    JSContext* cx) {
  PromiseObject* promise = CreatePromiseObjectWithoutResolutionFunctions(cx);
  if (!promise) {
    return nullptr;
  }

  AddPromiseFlags(*promise, PROMISE_FLAG_ASYNC);
  return promise;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * AsyncFunctionStart ( promiseCapability, asyncFunctionBody )
 * https://tc39.es/ecma262/#sec-async-functions-abstract-operations-async-function-start
 *
 * Steps 4.f-g.
 */
[[nodiscard]] bool js::AsyncFunctionThrown(JSContext* cx,
                                           Handle<PromiseObject*> resultPromise,
                                           HandleValue reason) {
  if (resultPromise->state() != JS::PromiseState::Pending) {
    // OOM after resolving promise.
    // Report a warning and ignore the result.
    if (!WarnNumberASCII(cx, JSMSG_UNHANDLABLE_PROMISE_REJECTION_WARNING)) {
      if (cx->isExceptionPending()) {
        cx->clearPendingException();
      }
    }
    return true;
  }

  // Step 4.f. Else,
  // Step 4.f.i. Assert: result.[[Type]] is throw.
  // Step 4.f.ii. Perform
  //              ! Call(promiseCapability.[[Reject]], undefined,
  //                 « result.[[Value]] »).
  // Step 4.g. Return.
  return RejectPromiseInternal(cx, resultPromise, reason);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * AsyncFunctionStart ( promiseCapability, asyncFunctionBody )
 * https://tc39.es/ecma262/#sec-async-functions-abstract-operations-async-function-start
 *
 * Steps 4.e, 4.g.
 */
[[nodiscard]] bool js::AsyncFunctionReturned(
    JSContext* cx, Handle<PromiseObject*> resultPromise, HandleValue value) {
  // Step 4.e. Else if result.[[Type]] is return, then
  // Step 4.e.i. Perform
  //             ! Call(promiseCapability.[[Resolve]], undefined,
  //                    « result.[[Value]] »).
  return ResolvePromiseInternal(cx, resultPromise, value);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Await
 * https://tc39.github.io/ecma262/#await
 *
 * Helper function that performs Await(promise) steps 2-7.
 * The same steps are also used in a few other places in the spec.
 */
template <typename T>
[[nodiscard]] static bool InternalAwait(JSContext* cx, HandleValue value,
                                        HandleObject resultPromise,
                                        PromiseHandler onFulfilled,
                                        PromiseHandler onRejected,
                                        T extraStep) {
  // Step 2. Let promise be ? PromiseResolve(%Promise%, value).
  RootedObject promise(cx, PromiseObject::unforgeableResolve(cx, value));
  if (!promise) {
    return false;
  }

  // This downcast is safe because unforgeableResolve either returns `value`
  // (only if it is already a possibly-wrapped promise) or creates a new
  // promise using the Promise constructor.
  Rooted<PromiseObject*> unwrappedPromise(
      cx, UnwrapAndDowncastObject<PromiseObject>(cx, promise));
  if (!unwrappedPromise) {
    return false;
  }

  // Steps 3-6 for creating onFulfilled/onRejected are done by caller.

  // Step 7. Perform ! PerformPromiseThen(promise, onFulfilled, onRejected).
  RootedValue onFulfilledValue(cx, Int32Value(int32_t(onFulfilled)));
  RootedValue onRejectedValue(cx, Int32Value(int32_t(onRejected)));
  Rooted<PromiseCapability> resultCapability(cx);
  resultCapability.promise().set(resultPromise);
  Rooted<PromiseReactionRecord*> reaction(
      cx, NewReactionRecord(cx, resultCapability, onFulfilledValue,
                            onRejectedValue, IncumbentGlobalObject::Yes));
  if (!reaction) {
    return false;
  }
  extraStep(reaction);
  return PerformPromiseThenWithReaction(cx, unwrappedPromise, reaction);
}

[[nodiscard]] bool js::InternalAsyncGeneratorAwait(
    JSContext* cx, JS::Handle<AsyncGeneratorObject*> generator,
    JS::Handle<JS::Value> value, PromiseHandler onFulfilled,
    PromiseHandler onRejected) {
  auto extra = [&](Handle<PromiseReactionRecord*> reaction) {
    reaction->setIsAsyncGenerator(generator);
  };
  return InternalAwait(cx, value, nullptr, onFulfilled, onRejected, extra);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Await
 * https://tc39.es/ecma262/#await
 */
[[nodiscard]] JSObject* js::AsyncFunctionAwait(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> genObj,
    HandleValue value) {
  auto extra = [&](Handle<PromiseReactionRecord*> reaction) {
    reaction->setIsAsyncFunction(genObj);
  };
  if (!InternalAwait(cx, value, nullptr,
                     PromiseHandler::AsyncFunctionAwaitedFulfilled,
                     PromiseHandler::AsyncFunctionAwaitedRejected, extra)) {
    return nullptr;
  }
  return genObj->promise();
}

// https://tc39.github.io/ecma262/#sec-%asyncfromsynciteratorprototype%.next
// 25.1.4.2.1 %AsyncFromSyncIteratorPrototype%.next
// 25.1.4.2.2 %AsyncFromSyncIteratorPrototype%.return
// 25.1.4.2.3 %AsyncFromSyncIteratorPrototype%.throw
bool js::AsyncFromSyncIteratorMethod(JSContext* cx, CallArgs& args,
                                     CompletionKind completionKind) {
  // Step 1: Let O be the this value.
  HandleValue thisVal = args.thisv();

  // Step 2: Let promiseCapability be ! NewPromiseCapability(%Promise%).
  Rooted<PromiseObject*> resultPromise(
      cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
  if (!resultPromise) {
    return false;
  }

  // Step 3: If Type(O) is not Object, or if O does not have a
  //         [[SyncIteratorRecord]] internal slot, then
  if (!thisVal.isObject() ||
      !thisVal.toObject().is<AsyncFromSyncIteratorObject>()) {
    // NB: See https://github.com/tc39/proposal-async-iteration/issues/105
    // for why this check shouldn't be necessary as long as we can ensure
    // the Async-from-Sync iterator can't be accessed directly by user
    // code.

    // Step 3.a: Let invalidIteratorError be a newly created TypeError object.
    RootedValue badGeneratorError(cx);
    if (!GetTypeError(cx, JSMSG_NOT_AN_ASYNC_ITERATOR, &badGeneratorError)) {
      return false;
    }

    // Step 3.b: Perform ! Call(promiseCapability.[[Reject]], undefined,
    //                          « invalidIteratorError »).
    if (!RejectPromiseInternal(cx, resultPromise, badGeneratorError)) {
      return false;
    }

    // Step 3.c: Return promiseCapability.[[Promise]].
    args.rval().setObject(*resultPromise);
    return true;
  }

  Rooted<AsyncFromSyncIteratorObject*> asyncIter(
      cx, &thisVal.toObject().as<AsyncFromSyncIteratorObject>());

  // Step 4: Let syncIteratorRecord be O.[[SyncIteratorRecord]].
  RootedObject iter(cx, asyncIter->iterator());

  RootedValue func(cx);
  if (completionKind == CompletionKind::Normal) {
    // next() preparing for steps 5-6.
    func.set(asyncIter->nextMethod());
  } else if (completionKind == CompletionKind::Return) {
    // return() steps 5-7.
    // Step 5: Let return be GetMethod(syncIterator, "return").
    // Step 6: IfAbruptRejectPromise(return, promiseCapability).
    if (!GetProperty(cx, iter, iter, cx->names().return_, &func)) {
      return AbruptRejectPromise(cx, args, resultPromise, nullptr);
    }

    // Step 7: If return is undefined, then
    // (Note: GetMethod contains a step that changes `null` to `undefined`;
    // we omit that step above, and check for `null` here instead.)
    if (func.isNullOrUndefined()) {
      // Step 7.a: Let iterResult be ! CreateIterResultObject(value, true).
      PlainObject* resultObj = CreateIterResultObject(cx, args.get(0), true);
      if (!resultObj) {
        return AbruptRejectPromise(cx, args, resultPromise, nullptr);
      }

      RootedValue resultVal(cx, ObjectValue(*resultObj));

      // Step 7.b: Perform ! Call(promiseCapability.[[Resolve]], undefined,
      //                          « iterResult »).
      if (!ResolvePromiseInternal(cx, resultPromise, resultVal)) {
        return AbruptRejectPromise(cx, args, resultPromise, nullptr);
      }

      // Step 7.c: Return promiseCapability.[[Promise]].
      args.rval().setObject(*resultPromise);
      return true;
    }
  } else {
    // noexcept(true) steps 5-7.
    MOZ_ASSERT(completionKind == CompletionKind::Throw);

    // Step 5: Let throw be GetMethod(syncIterator, "throw").
    // Step 6: IfAbruptRejectPromise(throw, promiseCapability).
    if (!GetProperty(cx, iter, iter, cx->names().throw_, &func)) {
      return AbruptRejectPromise(cx, args, resultPromise, nullptr);
    }

    // Step 7: If throw is undefined, then
    // (Note: GetMethod contains a step that changes `null` to `undefined`;
    // we omit that step above, and check for `null` here instead.)
    if (func.isNullOrUndefined()) {
      // Step 7.a: Perform ! Call(promiseCapability.[[Reject]], undefined, «
      // value »).
      if (!RejectPromiseInternal(cx, resultPromise, args.get(0))) {
        return AbruptRejectPromise(cx, args, resultPromise, nullptr);
      }

      // Step 7.b: Return promiseCapability.[[Promise]].
      args.rval().setObject(*resultPromise);
      return true;
    }
  }

  // next() steps 5-6.
  //     Step 5: Let result be IteratorNext(syncIteratorRecord, value).
  //     Step 6: IfAbruptRejectPromise(result, promiseCapability).
  // return/throw() steps 8-9.
  //     Step 8: Let result be Call(throw, syncIterator, « value »).
  //     Step 9: IfAbruptRejectPromise(result, promiseCapability).
  //
  // Including the changes from: https://github.com/tc39/ecma262/pull/1776
  RootedValue iterVal(cx, ObjectValue(*iter));
  RootedValue resultVal(cx);
  bool ok;
  if (args.length() == 0) {
    ok = Call(cx, func, iterVal, &resultVal);
  } else {
    ok = Call(cx, func, iterVal, args[0], &resultVal);
  }
  if (!ok) {
    return AbruptRejectPromise(cx, args, resultPromise, nullptr);
  }

  // next() step 5 -> IteratorNext Step 3:
  //     If Type(result) is not Object, throw a TypeError exception.
  // Followed by IfAbruptRejectPromise in step 6.
  //
  // return/throw() Step 10: If Type(result) is not Object, then
  //     Step 10.a: Perform ! Call(promiseCapability.[[Reject]], undefined,
  //                               « a newly created TypeError object »).
  //     Step 10.b: Return promiseCapability.[[Promise]].
  if (!resultVal.isObject()) {
    CheckIsObjectKind kind;
    switch (completionKind) {
      case CompletionKind::Normal:
        kind = CheckIsObjectKind::IteratorNext;
        break;
      case CompletionKind::Throw:
        kind = CheckIsObjectKind::IteratorThrow;
        break;
      case CompletionKind::Return:
        kind = CheckIsObjectKind::IteratorReturn;
        break;
    }
    MOZ_ALWAYS_FALSE(ThrowCheckIsObject(cx, kind));
    return AbruptRejectPromise(cx, args, resultPromise, nullptr);
  }

  RootedObject resultObj(cx, &resultVal.toObject());

  // next() Step 7, return/throw() Step 11: Return
  //     ! AsyncFromSyncIteratorContinuation(result, promiseCapability).
  //
  // The step numbers below are for
  // 25.1.4.4 AsyncFromSyncIteratorContinuation ( result, promiseCapability ).

  // Step 1: Let done be IteratorComplete(result).
  // Step 2: IfAbruptRejectPromise(done, promiseCapability).
  RootedValue doneVal(cx);
  if (!GetProperty(cx, resultObj, resultObj, cx->names().done, &doneVal)) {
    return AbruptRejectPromise(cx, args, resultPromise, nullptr);
  }
  bool done = ToBoolean(doneVal);

  // Step 3: Let value be IteratorValue(result).
  // Step 4: IfAbruptRejectPromise(value, promiseCapability).
  RootedValue value(cx);
  if (!GetProperty(cx, resultObj, resultObj, cx->names().value, &value)) {
    return AbruptRejectPromise(cx, args, resultPromise, nullptr);
  }

  // Step numbers below include the changes in
  // <https://github.com/tc39/ecma262/pull/1470>, which inserted a new step 6.
  //
  // Steps 7-9 (reordered).
  // Step 7: Let steps be the algorithm steps defined in Async-from-Sync
  //         Iterator Value Unwrap Functions.
  // Step 8: Let onFulfilled be CreateBuiltinFunction(steps, « [[Done]] »).
  // Step 9: Set onFulfilled.[[Done]] to done.
  PromiseHandler onFulfilled =
      done ? PromiseHandler::AsyncFromSyncIteratorValueUnwrapDone
           : PromiseHandler::AsyncFromSyncIteratorValueUnwrapNotDone;
  PromiseHandler onRejected = PromiseHandler::Thrower;

  // Steps 5 and 10 are identical to some steps in Await; we have a utility
  // function InternalAwait() that implements the idiom.
  //
  // Step 5: Let valueWrapper be PromiseResolve(%Promise%, « value »).
  // Step 6: IfAbruptRejectPromise(valueWrapper, promiseCapability).
  // Step 10: Perform ! PerformPromiseThen(valueWrapper, onFulfilled,
  //                                      undefined, promiseCapability).
  auto extra = [](Handle<PromiseReactionRecord*> reaction) {};
  if (!InternalAwait(cx, value, resultPromise, onFulfilled, onRejected,
                     extra)) {
    return AbruptRejectPromise(cx, args, resultPromise, nullptr);
  }

  // Step 11: Return promiseCapability.[[Promise]].
  args.rval().setObject(*resultPromise);
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.catch ( onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.catch
 */
static bool Promise_catch_impl(JSContext* cx, unsigned argc, Value* vp,
                               bool rvalExplicitlyUsed) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Let promise be the this value.
  HandleValue thisVal = args.thisv();
  HandleValue onFulfilled = UndefinedHandleValue;
  HandleValue onRejected = args.get(0);

  // Fast path when the default Promise state is intact.
  if (CanCallOriginalPromiseThenBuiltin(cx, thisVal)) {
    return OriginalPromiseThenBuiltin(cx, thisVal, onFulfilled, onRejected,
                                      args.rval(), rvalExplicitlyUsed);
  }

  // Step 2. Return ? Invoke(promise, "then", « undefined, onRejected »).
  RootedValue thenVal(cx);
  if (!GetProperty(cx, thisVal, cx->names().then, &thenVal)) {
    return false;
  }

  if (IsNativeFunction(thenVal, &Promise_then) &&
      thenVal.toObject().nonCCWRealm() == cx->realm()) {
    return Promise_then_impl(cx, thisVal, onFulfilled, onRejected, args.rval(),
                             rvalExplicitlyUsed);
  }

  return Call(cx, thenVal, thisVal, UndefinedHandleValue, onRejected,
              args.rval());
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.catch ( onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.catch
 */
static bool Promise_catch_noRetVal(JSContext* cx, unsigned argc, Value* vp) {
  return Promise_catch_impl(cx, argc, vp, false);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.catch ( onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.catch
 */
static bool Promise_catch(JSContext* cx, unsigned argc, Value* vp) {
  return Promise_catch_impl(cx, argc, vp, true);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.then ( onFulfilled, onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.then
 */
static bool Promise_then_impl(JSContext* cx, HandleValue promiseVal,
                              HandleValue onFulfilled, HandleValue onRejected,
                              MutableHandleValue rval,
                              bool rvalExplicitlyUsed) {
  // Step 1. Let promise be the this value.
  // (implicit)

  // Step 2. If IsPromise(promise) is false, throw a TypeError exception.
  if (!promiseVal.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED,
                              "Receiver of Promise.prototype.then call");
    return false;
  }

  // Fast path when the default Promise state is intact.
  if (CanCallOriginalPromiseThenBuiltin(cx, promiseVal)) {
    // Steps 3-5.
    return OriginalPromiseThenBuiltin(cx, promiseVal, onFulfilled, onRejected,
                                      rval, rvalExplicitlyUsed);
  }

  RootedObject promiseObj(cx, &promiseVal.toObject());
  Rooted<PromiseObject*> unwrappedPromise(
      cx,
      UnwrapAndTypeCheckValue<PromiseObject>(cx, promiseVal, [cx, &promiseVal] {
        JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr,
                                   JSMSG_INCOMPATIBLE_PROTO, "Promise", "then",
                                   InformalValueTypeName(promiseVal));
      }));
  if (!unwrappedPromise) {
    return false;
  }

  bool rvalUsed =
      rvalExplicitlyUsed ||
      IsPromiseThenOrCatchRetValImplicitlyUsed(cx, unwrappedPromise);

  // Step 3. Let C be ? SpeciesConstructor(promise, %Promise%).
  // Step 4. Let resultCapability be ? NewPromiseCapability(C).
  CreateDependentPromise createDependent =
      rvalUsed ? CreateDependentPromise::Always
               : CreateDependentPromise::SkipIfCtorUnobservable;
  Rooted<PromiseCapability> resultCapability(cx);
  if (!PromiseThenNewPromiseCapability(cx, promiseObj, createDependent,
                                       &resultCapability)) {
    return false;
  }

  // Step 5. Return PerformPromiseThen(promise, onFulfilled, onRejected,
  //                                   resultCapability).
  if (!PerformPromiseThen(cx, unwrappedPromise, onFulfilled, onRejected,
                          resultCapability)) {
    return false;
  }

  if (rvalUsed) {
    rval.setObject(*resultCapability.promise());
  } else {
    rval.setUndefined();
  }
  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.then ( onFulfilled, onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.then
 */
bool Promise_then_noRetVal(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return Promise_then_impl(cx, args.thisv(), args.get(0), args.get(1),
                           args.rval(), false);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * Promise.prototype.then ( onFulfilled, onRejected )
 * https://tc39.es/ecma262/#sec-promise.prototype.then
 */
bool js::Promise_then(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return Promise_then_impl(cx, args.thisv(), args.get(0), args.get(1),
                           args.rval(), true);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseThen ( promise, onFulfilled, onRejected
 *                      [ , resultCapability ] )
 * https://tc39.es/ecma262/#sec-performpromisethen
 *
 * Steps 1-12.
 */
[[nodiscard]] static bool PerformPromiseThen(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue onFulfilled_,
    HandleValue onRejected_, Handle<PromiseCapability> resultCapability) {
  // Step 1. Assert: IsPromise(promise) is true.
  // Step 2. If resultCapability is not present, then
  // Step 2. a. Set resultCapability to undefined.
  // (implicit)

  // (reordered)
  // Step 4. Else,
  // Step 4. a. Let onFulfilledJobCallback be HostMakeJobCallback(onFulfilled).
  RootedValue onFulfilled(cx, onFulfilled_);

  // Step 3. If IsCallable(onFulfilled) is false, then
  if (!IsCallable(onFulfilled)) {
    // Step 3. a. Let onFulfilledJobCallback be empty.
    onFulfilled = Int32Value(int32_t(PromiseHandler::Identity));
  }

  // (reordered)
  // Step 6. Else,
  // Step 6. a. Let onRejectedJobCallback be HostMakeJobCallback(onRejected).
  RootedValue onRejected(cx, onRejected_);

  // Step 5. If IsCallable(onRejected) is false, then
  if (!IsCallable(onRejected)) {
    // Step 5. a. Let onRejectedJobCallback be empty.
    onRejected = Int32Value(int32_t(PromiseHandler::Thrower));
  }

  // Step 7. Let fulfillReaction be the PromiseReaction
  //         { [[Capability]]: resultCapability, [[Type]]: Fulfill,
  //           [[Handler]]: onFulfilledJobCallback }.
  // Step 8. Let rejectReaction be the PromiseReaction
  //         { [[Capability]]: resultCapability, [[Type]]: Reject,
  //           [[Handler]]: onRejectedJobCallback }.
  //
  // NOTE: We use single object for both reactions.
  Rooted<PromiseReactionRecord*> reaction(
      cx, NewReactionRecord(cx, resultCapability, onFulfilled, onRejected,
                            IncumbentGlobalObject::Yes));
  if (!reaction) {
    return false;
  }

  // Steps 9-14.
  return PerformPromiseThenWithReaction(cx, promise, reaction);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseThen ( promise, onFulfilled, onRejected
 *                      [ , resultCapability ] )
 * https://tc39.es/ecma262/#sec-performpromisethen
 */
[[nodiscard]] static bool PerformPromiseThenWithoutSettleHandlers(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseObject*> promiseToResolve,
    Handle<PromiseCapability> resultCapability) {
  // Step 1. Assert: IsPromise(promise) is true.
  // Step 2. If resultCapability is not present, then
  // (implicit)

  // Step 3. If IsCallable(onFulfilled) is false, then
  // Step 3.a. Let onFulfilledJobCallback be empty.
  HandleValue onFulfilled = NullHandleValue;

  // Step 5. If IsCallable(onRejected) is false, then
  // Step 5.a. Let onRejectedJobCallback be empty.
  HandleValue onRejected = NullHandleValue;

  // Step 7. Let fulfillReaction be the PromiseReaction
  //         { [[Capability]]: resultCapability, [[Type]]: Fulfill,
  //           [[Handler]]: onFulfilledJobCallback }.
  // Step 8. Let rejectReaction be the PromiseReaction
  //         { [[Capability]]: resultCapability, [[Type]]: Reject,
  //           [[Handler]]: onRejectedJobCallback }.
  Rooted<PromiseReactionRecord*> reaction(
      cx, NewReactionRecord(cx, resultCapability, onFulfilled, onRejected,
                            IncumbentGlobalObject::Yes));
  if (!reaction) {
    return false;
  }

  reaction->setIsDefaultResolvingHandler(promiseToResolve);

  // Steps 9-12.
  return PerformPromiseThenWithReaction(cx, promise, reaction);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseThen ( promise, onFulfilled, onRejected
 *                      [ , resultCapability ] )
 * https://tc39.github.io/ecma262/#sec-performpromisethen
 *
 * Steps 9-12.
 */
[[nodiscard]] static bool PerformPromiseThenWithReaction(
    JSContext* cx, Handle<PromiseObject*> unwrappedPromise,
    Handle<PromiseReactionRecord*> reaction) {
  // Step 9. If promise.[[PromiseState]] is pending, then
  JS::PromiseState state = unwrappedPromise->state();
  int32_t flags = unwrappedPromise->flags();
  if (state == JS::PromiseState::Pending) {
    // Step 9.a. Append fulfillReaction as the last element of the List that is
    //           promise.[[PromiseFulfillReactions]].
    // Step 9.b. Append rejectReaction as the last element of the List that is
    //           promise.[[PromiseRejectReactions]].
    //
    // Instead of creating separate reaction records for fulfillment and
    // rejection, we create a combined record. All places we use the record
    // can handle that.
    if (!AddPromiseReaction(cx, unwrappedPromise, reaction)) {
      return false;
    }
  }

  // Steps 10-11.
  else {
    // Step 11.a. Assert: The value of promise.[[PromiseState]] is rejected.
    MOZ_ASSERT_IF(state != JS::PromiseState::Fulfilled,
                  state == JS::PromiseState::Rejected);

    // Step 10.a. Let value be promise.[[PromiseResult]].
    // Step 11.b. Let reason be promise.[[PromiseResult]].
    RootedValue valueOrReason(cx, unwrappedPromise->valueOrReason());

    // We might be operating on a promise from another compartment. In that
    // case, we need to wrap the result/reason value before using it.
    if (!cx->compartment()->wrap(cx, &valueOrReason)) {
      return false;
    }

    // Step 11.c. If promise.[[PromiseIsHandled]] is false,
    //            perform HostPromiseRejectionTracker(promise, "handle").
    if (state == JS::PromiseState::Rejected &&
        !(flags & PROMISE_FLAG_HANDLED)) {
      cx->runtime()->removeUnhandledRejectedPromise(cx, unwrappedPromise);
    }

    // Step 10.b. Let fulfillJob be
    //            NewPromiseReactionJob(fulfillReaction, value).
    // Step 10.c. Perform HostEnqueuePromiseJob(fulfillJob.[[Job]],
    //                                          fulfillJob.[[Realm]]).
    // Step 11.d. Let rejectJob be
    //            NewPromiseReactionJob(rejectReaction, reason).
    // Step 11.e. Perform HostEnqueuePromiseJob(rejectJob.[[Job]],
    //                                          rejectJob.[[Realm]]).
    if (!EnqueuePromiseReactionJob(cx, reaction, valueOrReason, state)) {
      return false;
    }
  }

  // Step 12. Set promise.[[PromiseIsHandled]] to true.
  unwrappedPromise->setHandled();

  return true;
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * PerformPromiseThen ( promise, onFulfilled, onRejected
 *                      [ , resultCapability ] )
 * https://tc39.github.io/ecma262/#sec-performpromisethen
 *
 * Steps 9.a-b.
 */
[[nodiscard]] static bool AddPromiseReaction(
    JSContext* cx, Handle<PromiseObject*> unwrappedPromise,
    Handle<PromiseReactionRecord*> reaction) {
  MOZ_RELEASE_ASSERT(reaction->is<PromiseReactionRecord>());
  RootedValue reactionVal(cx, ObjectValue(*reaction));

  // The code that creates Promise reactions can handle wrapped Promises,
  // unwrapping them as needed. That means that the `promise` and `reaction`
  // objects we have here aren't necessarily from the same compartment. In
  // order to store the reaction on the promise, we have to ensure that it
  // is properly wrapped.
  mozilla::Maybe<AutoRealm> ar;
  if (unwrappedPromise->compartment() != cx->compartment()) {
    ar.emplace(cx, unwrappedPromise);
    if (!cx->compartment()->wrap(cx, &reactionVal)) {
      return false;
    }
  }
  Handle<PromiseObject*> promise = unwrappedPromise;

  // Step 9.a. Append fulfillReaction as the last element of the List that is
  //           promise.[[PromiseFulfillReactions]].
  // Step 9.b. Append rejectReaction as the last element of the List that is
  //           promise.[[PromiseRejectReactions]].
  RootedValue reactionsVal(cx, promise->reactions());

  if (reactionsVal.isUndefined()) {
    // If no reactions existed so far, just store the reaction record directly.
    promise->setFixedSlot(PromiseSlot_ReactionsOrResult, reactionVal);
    return true;
  }

  RootedObject reactionsObj(cx, &reactionsVal.toObject());

  // If only a single reaction exists, it's stored directly instead of in a
  // list. In that case, `reactionsObj` might be a wrapper, which we can
  // always safely unwrap.
  if (IsProxy(reactionsObj)) {
    reactionsObj = UncheckedUnwrap(reactionsObj);
    if (JS_IsDeadWrapper(reactionsObj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }
    MOZ_RELEASE_ASSERT(reactionsObj->is<PromiseReactionRecord>());
  }

  if (reactionsObj->is<PromiseReactionRecord>()) {
    // If a single reaction existed so far, create a list and store the
    // old and the new reaction in it.
    ArrayObject* reactions = NewDenseFullyAllocatedArray(cx, 2);
    if (!reactions) {
      return false;
    }

    reactions->setDenseInitializedLength(2);
    reactions->initDenseElement(0, reactionsVal);
    reactions->initDenseElement(1, reactionVal);

    promise->setFixedSlot(PromiseSlot_ReactionsOrResult,
                          ObjectValue(*reactions));
  } else {
    // Otherwise, just store the new reaction.
    MOZ_RELEASE_ASSERT(reactionsObj->is<NativeObject>());
    Handle<NativeObject*> reactions = reactionsObj.as<NativeObject>();
    uint32_t len = reactions->getDenseInitializedLength();
    DenseElementResult result = reactions->ensureDenseElements(cx, len, 1);
    if (result != DenseElementResult::Success) {
      MOZ_ASSERT(result == DenseElementResult::Failure);
      return false;
    }
    reactions->setDenseElement(len, reactionVal);
  }

  return true;
}

[[nodiscard]] static bool AddDummyPromiseReactionForDebugger(
    JSContext* cx, Handle<PromiseObject*> promise,
    HandleObject dependentPromise) {
  if (promise->state() != JS::PromiseState::Pending) {
    return true;
  }

  // `dependentPromise` should be a maybe-wrapped Promise.
  MOZ_ASSERT(UncheckedUnwrap(dependentPromise)->is<PromiseObject>());

  // Leave resolve and reject as null.
  Rooted<PromiseCapability> capability(cx);
  capability.promise().set(dependentPromise);

  Rooted<PromiseReactionRecord*> reaction(
      cx, NewReactionRecord(cx, capability, NullHandleValue, NullHandleValue,
                            IncumbentGlobalObject::No));
  if (!reaction) {
    return false;
  }

  reaction->setIsDebuggerDummy();

  return AddPromiseReaction(cx, promise, reaction);
}

uint64_t PromiseObject::getID() { return PromiseDebugInfo::id(this); }

double PromiseObject::lifetime() {
  return MillisecondsSinceStartup() - allocationTime();
}

/**
 * Returns all promises that directly depend on this one. That means those
 * created by calling `then` on this promise, or the promise returned by
 * `Promise.all(iterable)` or `Promise.race(iterable)`, with this promise
 * being a member of the passed-in `iterable`.
 *
 * Per spec, we should have separate lists of reaction records for the
 * fulfill and reject cases. As an optimization, we have only one of those,
 * containing the required data for both cases. So we just walk that list
 * and extract the dependent promises from all reaction records.
 */
bool PromiseObject::dependentPromises(JSContext* cx,
                                      MutableHandle<GCVector<Value>> values) {
  if (state() != JS::PromiseState::Pending) {
    return true;
  }

  uint32_t valuesIndex = 0;
  RootedValue reactionsVal(cx, reactions());

  return ForEachReaction(cx, reactionsVal, [&](MutableHandleObject obj) {
    if (IsProxy(obj)) {
      obj.set(UncheckedUnwrap(obj));
    }

    if (JS_IsDeadWrapper(obj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }

    MOZ_RELEASE_ASSERT(obj->is<PromiseReactionRecord>());
    Rooted<PromiseReactionRecord*> reaction(cx,
                                            &obj->as<PromiseReactionRecord>());

    // Not all reactions have a Promise on them.
    RootedObject promiseObj(cx, reaction->promise());
    if (promiseObj) {
      if (!values.growBy(1)) {
        return false;
      }

      values[valuesIndex++].setObject(*promiseObj);
    }
    return true;
  });
}

bool PromiseObject::forEachReactionRecord(
    JSContext* cx, PromiseReactionRecordBuilder& builder) {
  if (state() != JS::PromiseState::Pending) {
    // Promise was resolved, so no reaction records are present.
    return true;
  }

  RootedValue reactionsVal(cx, reactions());
  if (reactionsVal.isNullOrUndefined()) {
    // No reaction records are attached to this promise.
    return true;
  }

  return ForEachReaction(cx, reactionsVal, [&](MutableHandleObject obj) {
    if (IsProxy(obj)) {
      obj.set(UncheckedUnwrap(obj));
    }

    if (JS_IsDeadWrapper(obj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return false;
    }

    Rooted<PromiseReactionRecord*> reaction(cx,
                                            &obj->as<PromiseReactionRecord>());
    MOZ_ASSERT(reaction->targetState() == JS::PromiseState::Pending);

    if (reaction->isAsyncFunction()) {
      Rooted<AsyncFunctionGeneratorObject*> generator(
          cx, reaction->asyncFunctionGenerator());
      if (!builder.asyncFunction(cx, generator)) {
        return false;
      }
    } else if (reaction->isAsyncGenerator()) {
      Rooted<AsyncGeneratorObject*> generator(cx, reaction->asyncGenerator());
      if (!builder.asyncGenerator(cx, generator)) {
        return false;
      }
    } else if (reaction->isDefaultResolvingHandler()) {
      Rooted<PromiseObject*> promise(cx, reaction->defaultResolvingPromise());
      if (!builder.direct(cx, promise)) {
        return false;
      }
    } else {
      RootedObject resolve(cx);
      RootedObject reject(cx);
      RootedObject result(cx, reaction->promise());

      Value v = reaction->getFixedSlot(ReactionRecordSlot_OnFulfilled);
      if (v.isObject()) {
        resolve = &v.toObject();
      }

      v = reaction->getFixedSlot(ReactionRecordSlot_OnRejected);
      if (v.isObject()) {
        reject = &v.toObject();
      }

      if (!builder.then(cx, resolve, reject, result)) {
        return false;
      }
    }

    return true;
  });
}

/**
 * ES2023 draft rev 714fa3dd1e8237ae9c666146270f81880089eca5
 *
 * Promise Reject Functions
 * https://tc39.es/ecma262/#sec-promise-reject-functions
 */
static bool CallDefaultPromiseResolveFunction(JSContext* cx,
                                              Handle<PromiseObject*> promise,
                                              HandleValue resolutionValue) {
  MOZ_ASSERT(IsPromiseWithDefaultResolvingFunction(promise));

  // Steps 1-3.
  // (implicit)

  // Step 4. Let alreadyResolved be F.[[AlreadyResolved]].
  // Step 5. If alreadyResolved.[[Value]] is true, return undefined.
  if (IsAlreadyResolvedPromiseWithDefaultResolvingFunction(promise)) {
    return true;
  }

  // Step 6. Set alreadyResolved.[[Value]] to true.
  SetAlreadyResolvedPromiseWithDefaultResolvingFunction(promise);

  // Steps 7-15.
  // (implicit) Step 16. Return undefined.
  return ResolvePromiseInternal(cx, promise, resolutionValue);
}

/* static */
bool PromiseObject::resolve(JSContext* cx, Handle<PromiseObject*> promise,
                            HandleValue resolutionValue) {
  MOZ_ASSERT(!PromiseHasAnyFlag(*promise, PROMISE_FLAG_ASYNC));
  if (promise->state() != JS::PromiseState::Pending) {
    return true;
  }

  if (IsPromiseWithDefaultResolvingFunction(promise)) {
    return CallDefaultPromiseResolveFunction(cx, promise, resolutionValue);
  }

  JSFunction* resolveFun = GetResolveFunctionFromPromise(promise);
  if (!resolveFun) {
    return true;
  }

  RootedValue funVal(cx, ObjectValue(*resolveFun));

  // For xray'd Promises, the resolve fun may have been created in another
  // compartment. For the call below to work in that case, wrap the
  // function into the current compartment.
  if (!cx->compartment()->wrap(cx, &funVal)) {
    return false;
  }

  RootedValue dummy(cx);
  return Call(cx, funVal, UndefinedHandleValue, resolutionValue, &dummy);
}

/**
 * ES2023 draft rev 714fa3dd1e8237ae9c666146270f81880089eca5
 *
 * Promise Reject Functions
 * https://tc39.es/ecma262/#sec-promise-reject-functions
 */
static bool CallDefaultPromiseRejectFunction(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue rejectionValue,
    JS::Handle<SavedFrame*> unwrappedRejectionStack /* = nullptr */) {
  MOZ_ASSERT(IsPromiseWithDefaultResolvingFunction(promise));

  // Steps 1-3.
  // (implicit)

  // Step 4. Let alreadyResolved be F.[[AlreadyResolved]].
  // Step 5. If alreadyResolved.[[Value]] is true, return undefined.
  if (IsAlreadyResolvedPromiseWithDefaultResolvingFunction(promise)) {
    return true;
  }

  // Step 6. Set alreadyResolved.[[Value]] to true.
  SetAlreadyResolvedPromiseWithDefaultResolvingFunction(promise);

  return RejectPromiseInternal(cx, promise, rejectionValue,
                               unwrappedRejectionStack);
}

/* static */
bool PromiseObject::reject(JSContext* cx, Handle<PromiseObject*> promise,
                           HandleValue rejectionValue) {
  MOZ_ASSERT(!PromiseHasAnyFlag(*promise, PROMISE_FLAG_ASYNC));
  if (promise->state() != JS::PromiseState::Pending) {
    return true;
  }

  if (IsPromiseWithDefaultResolvingFunction(promise)) {
    return CallDefaultPromiseRejectFunction(cx, promise, rejectionValue);
  }

  RootedValue funVal(cx, promise->getFixedSlot(PromiseSlot_RejectFunction));
  MOZ_ASSERT(IsCallable(funVal));

  RootedValue dummy(cx);
  return Call(cx, funVal, UndefinedHandleValue, rejectionValue, &dummy);
}

/**
 * ES2022 draft rev d03c1ec6e235a5180fa772b6178727c17974cb14
 *
 * RejectPromise ( promise, reason )
 * https://tc39.es/ecma262/#sec-rejectpromise
 *
 * Step 7.
 */
/* static */
void PromiseObject::onSettled(JSContext* cx, Handle<PromiseObject*> promise,
                              Handle<SavedFrame*> unwrappedRejectionStack) {
  PromiseDebugInfo::setResolutionInfo(cx, promise, unwrappedRejectionStack);

  // Step 7. If promise.[[PromiseIsHandled]] is false, perform
  //         HostPromiseRejectionTracker(promise, "reject").
  if (promise->state() == JS::PromiseState::Rejected &&
      promise->isUnhandled()) {
    cx->runtime()->addUnhandledRejectedPromise(cx, promise);
  }

  DebugAPI::onPromiseSettled(cx, promise);
}

void PromiseObject::setRequiresUserInteractionHandling(bool state) {
  if (state) {
    AddPromiseFlags(*this, PROMISE_FLAG_REQUIRES_USER_INTERACTION_HANDLING);
  } else {
    RemovePromiseFlags(*this, PROMISE_FLAG_REQUIRES_USER_INTERACTION_HANDLING);
  }
}

void PromiseObject::setHadUserInteractionUponCreation(bool state) {
  if (state) {
    AddPromiseFlags(*this, PROMISE_FLAG_HAD_USER_INTERACTION_UPON_CREATION);
  } else {
    RemovePromiseFlags(*this, PROMISE_FLAG_HAD_USER_INTERACTION_UPON_CREATION);
  }
}

void PromiseObject::copyUserInteractionFlagsFrom(PromiseObject& rhs) {
  setRequiresUserInteractionHandling(rhs.requiresUserInteractionHandling());
  setHadUserInteractionUponCreation(rhs.hadUserInteractionUponCreation());
}

// We can skip `await` with an already resolved value only if the current frame
// is the topmost JS frame and the current job is the last job in the job queue.
// This guarantees that any new job enqueued in the current turn will be
// executed immediately after the current job.
//
// Currently we only support skipping jobs when the async function is resumed
// at least once.
[[nodiscard]] static bool IsTopMostAsyncFunctionCall(JSContext* cx) {
  FrameIter iter(cx);

  // The current frame should be the async function.
  if (iter.done()) {
    return false;
  }

  if (!iter.isFunctionFrame() && iter.isModuleFrame()) {
    // The iterator is not a function frame, it is a module frame.
    // Ignore this optimization for now.
    return true;
  }

  MOZ_ASSERT(iter.calleeTemplate()->isAsync());

#ifdef DEBUG
  bool isGenerator = iter.calleeTemplate()->isGenerator();
#endif

  ++iter;

  // The parent frame should be the `next` function of the generator that is
  // internally called in AsyncFunctionResume resp. AsyncGeneratorResume.
  if (iter.done()) {
    return false;
  }
  // The initial call into an async function can happen from top-level code, so
  // the parent frame isn't required to be a function frame. Contrary to that,
  // the parent frame for an async generator function is always a function
  // frame, because async generators can't directly fall through to an `await`
  // expression from their initial call.
  if (!iter.isFunctionFrame()) {
    MOZ_ASSERT(!isGenerator);
    return false;
  }

  // Always skip InterpretGeneratorResume if present.
  JSFunction* fun = iter.calleeTemplate();
  if (IsSelfHostedFunctionWithName(fun, cx->names().InterpretGeneratorResume)) {
    ++iter;

    if (iter.done()) {
      return false;
    }

    MOZ_ASSERT(iter.isFunctionFrame());
    fun = iter.calleeTemplate();
  }

  if (!IsSelfHostedFunctionWithName(fun, cx->names().AsyncFunctionNext) &&
      !IsSelfHostedFunctionWithName(fun, cx->names().AsyncGeneratorNext)) {
    return false;
  }

  ++iter;

  // There should be no more frames.
  if (iter.done()) {
    return true;
  }

  return false;
}

[[nodiscard]] bool js::CanSkipAwait(JSContext* cx, HandleValue val,
                                    bool* canSkip) {
  if (!cx->canSkipEnqueuingJobs) {
    *canSkip = false;
    return true;
  }

  if (!IsTopMostAsyncFunctionCall(cx)) {
    *canSkip = false;
    return true;
  }

  // Primitive values cannot be 'thenables', so we can trivially skip the
  // await operation.
  if (!val.isObject()) {
    *canSkip = true;
    return true;
  }

  JSObject* obj = &val.toObject();
  if (!obj->is<PromiseObject>()) {
    *canSkip = false;
    return true;
  }

  PromiseObject* promise = &obj->as<PromiseObject>();

  if (promise->state() == JS::PromiseState::Pending) {
    *canSkip = false;
    return true;
  }

  PromiseLookup& promiseLookup = cx->realm()->promiseLookup;
  if (!promiseLookup.isDefaultInstance(cx, promise)) {
    *canSkip = false;
    return true;
  }

  if (promise->state() == JS::PromiseState::Rejected) {
    // We don't optimize rejected Promises for now.
    *canSkip = false;
    return true;
  }

  *canSkip = true;
  return true;
}

[[nodiscard]] bool js::ExtractAwaitValue(JSContext* cx, HandleValue val,
                                         MutableHandleValue resolved) {
// Ensure all callers of this are jumping past the
// extract if it's not possible to extract.
#ifdef DEBUG
  bool canSkip;
  if (!CanSkipAwait(cx, val, &canSkip)) {
    return false;
  }
  MOZ_ASSERT(canSkip == true);
#endif

  // Primitive values cannot be 'thenables', so we can trivially skip the
  // await operation.
  if (!val.isObject()) {
    resolved.set(val);
    return true;
  }

  JSObject* obj = &val.toObject();
  PromiseObject* promise = &obj->as<PromiseObject>();
  resolved.set(promise->value());

  return true;
}

JS::AutoDebuggerJobQueueInterruption::AutoDebuggerJobQueueInterruption()
    : cx(nullptr) {}

JS::AutoDebuggerJobQueueInterruption::~AutoDebuggerJobQueueInterruption() {
  MOZ_ASSERT_IF(initialized(), cx->jobQueue->empty());
}

bool JS::AutoDebuggerJobQueueInterruption::init(JSContext* cx) {
  MOZ_ASSERT(cx->jobQueue);
  this->cx = cx;
  saved = cx->jobQueue->saveJobQueue(cx);
  return !!saved;
}

void JS::AutoDebuggerJobQueueInterruption::runJobs() {
  JS::AutoSaveExceptionState ases(cx);
  cx->jobQueue->runJobs(cx);
}

const JSJitInfo promise_then_info = {
    {(JSJitGetterOp)Promise_then_noRetVal},
    {0}, /* unused */
    {0}, /* unused */
    JSJitInfo::IgnoresReturnValueNative,
    JSJitInfo::AliasEverything,
    JSVAL_TYPE_UNDEFINED,
};

const JSJitInfo promise_catch_info = {
    {(JSJitGetterOp)Promise_catch_noRetVal},
    {0}, /* unused */
    {0}, /* unused */
    JSJitInfo::IgnoresReturnValueNative,
    JSJitInfo::AliasEverything,
    JSVAL_TYPE_UNDEFINED,
};

static const JSFunctionSpec promise_methods[] = {
    JS_FNINFO("then", js::Promise_then, &promise_then_info, 2, 0),
    JS_FNINFO("catch", Promise_catch, &promise_catch_info, 1, 0),
    JS_SELF_HOSTED_FN("finally", "Promise_finally", 1, 0), JS_FS_END};

static const JSPropertySpec promise_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Promise", JSPROP_READONLY), JS_PS_END};

static const JSFunctionSpec promise_static_methods[] = {
    JS_FN("all", Promise_static_all, 1, 0),
    JS_FN("allSettled", Promise_static_allSettled, 1, 0),
    JS_FN("any", Promise_static_any, 1, 0),
    JS_FN("race", Promise_static_race, 1, 0),
    JS_FN("reject", Promise_reject, 1, 0),
    JS_FN("resolve", js::Promise_static_resolve, 1, 0),
    JS_FS_END};

static const JSPropertySpec promise_static_properties[] = {
    JS_SYM_GET(species, js::Promise_static_species, 0), JS_PS_END};

static const ClassSpec PromiseObjectClassSpec = {
    GenericCreateConstructor<PromiseConstructor, 1, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<PromiseObject>,
    promise_static_methods,
    promise_static_properties,
    promise_methods,
    promise_properties};

const JSClass PromiseObject::class_ = {
    "Promise",
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Promise) |
        JSCLASS_HAS_XRAYED_CONSTRUCTOR,
    JS_NULL_CLASS_OPS, &PromiseObjectClassSpec};

const JSClass PromiseObject::protoClass_ = {
    "Promise.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_Promise),
    JS_NULL_CLASS_OPS, &PromiseObjectClassSpec};
