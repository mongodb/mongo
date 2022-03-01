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

#include "js/Debug.h"
#include "js/experimental/JitInfo.h"  // JSJitGetterOp, JSJitInfo
#include "js/ForOfIterator.h"         // JS::ForOfIterator
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "vm/ArrayObject.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/ErrorObject.h"
#include "vm/GeneratorObject.h"
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
  return (now - mozilla::TimeStamp::ProcessCreation()).ToMilliseconds();
}

enum PromiseHandler {
  PromiseHandlerIdentity = 0,
  PromiseHandlerThrower,

  // ES 2018 draft 25.5.5.4-5.
  PromiseHandlerAsyncFunctionAwaitedFulfilled,
  PromiseHandlerAsyncFunctionAwaitedRejected,

  // ES2019 draft rev 49b781ec80117b60f73327ef3054703a3111e40c
  // 6.2.3.1.1 Await Fulfilled Functions
  // 6.2.3.1.2 Await Rejected Functions
  PromiseHandlerAsyncGeneratorAwaitedFulfilled,
  PromiseHandlerAsyncGeneratorAwaitedRejected,

  // ES2019 draft rev 49b781ec80117b60f73327ef3054703a3111e40c
  // 25.5.3.5.1 AsyncGeneratorResumeNext Return Processor Fulfilled Functions
  // 25.5.3.5.2 AsyncGeneratorResumeNext Return Processor Rejected Functions
  PromiseHandlerAsyncGeneratorResumeNextReturnFulfilled,
  PromiseHandlerAsyncGeneratorResumeNextReturnRejected,

  // ES2019 draft rev 49b781ec80117b60f73327ef3054703a3111e40c
  // 25.5.3.7 AsyncGeneratorYield, steps 8.c-e.
  PromiseHandlerAsyncGeneratorYieldReturnAwaitedFulfilled,
  PromiseHandlerAsyncGeneratorYieldReturnAwaitedRejected,

  // ES2019 draft rev 49b781ec80117b60f73327ef3054703a3111e40c
  // 25.1.4.2.5 Async-from-Sync Iterator Value Unwrap Functions
  //
  // Async-from-Sync iterator handlers take the resolved value and create new
  // iterator objects.  To do so it needs to forward whether the iterator is
  // done. In spec, this is achieved via the [[Done]] internal slot. We
  // enumerate both true and false cases here.
  PromiseHandlerAsyncFromSyncIteratorValueUnwrapDone,
  PromiseHandlerAsyncFromSyncIteratorValueUnwrapNotDone,

  // One past the maximum allowed PromiseHandler value.
  PromiseHandlerLimit
};

enum ResolutionMode { ResolveMode, RejectMode };

enum ResolveFunctionSlots {
  ResolveFunctionSlot_Promise = 0,
  ResolveFunctionSlot_RejectFunction,
};

enum RejectFunctionSlots {
  RejectFunctionSlot_Promise = 0,
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

  HandleArrayObject unwrappedArray() const {
    return HandleArrayObject::fromMarkedLocation(&elements().unwrappedArray);
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

    HandleArrayObject arrayObj = unwrappedArray();
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
                                HandleSavedFrame unwrappedRejectionStack) {
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
    return debugInfo->resolutionSite();
  }
  return nullptr;
}

/**
 * Wrapper for GetAndClearExceptionAndStack that handles cases where
 * no exception is pending, but an error occurred.
 * This can be the case if an OOM was encountered while throwing the error.
 */
static bool MaybeGetAndClearExceptionAndStack(JSContext* cx,
                                              MutableHandleValue rval,
                                              MutableHandleSavedFrame stack) {
  if (!cx->isExceptionPending()) {
    return false;
  }

  return GetAndClearExceptionAndStack(cx, rval, stack);
}

[[nodiscard]] static bool RunRejectFunction(
    JSContext* cx, HandleObject onRejectedFunc, HandleValue result,
    HandleObject promiseObj, HandleSavedFrame unwrappedRejectionStack,
    UnhandledRejectionBehavior behavior);

// ES2016, 25.4.1.1.1, Steps 1.a-b.
// Extracting all of this internal spec algorithm into a helper function would
// be tedious, so the check in step 1 and the entirety of step 2 aren't
// included.
static bool AbruptRejectPromise(JSContext* cx, CallArgs& args,
                                HandleObject promiseObj, HandleObject reject) {
  // Step 1.a.
  RootedValue reason(cx);
  RootedSavedFrame stack(cx);
  if (!MaybeGetAndClearExceptionAndStack(cx, &reason, &stack)) {
    return false;
  }

  if (!RunRejectFunction(cx, reject, reason, promiseObj, stack,
                         UnhandledRejectionBehavior::Report)) {
    return false;
  }

  // Step 1.b.
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

// ES2016, 25.4.1.2.
class PromiseReactionRecord : public NativeObject {
  static constexpr uint32_t REACTION_FLAG_RESOLVED = 0x1;
  static constexpr uint32_t REACTION_FLAG_FULFILLED = 0x2;
  static constexpr uint32_t REACTION_FLAG_DEFAULT_RESOLVING_HANDLER = 0x4;
  static constexpr uint32_t REACTION_FLAG_ASYNC_FUNCTION = 0x8;
  static constexpr uint32_t REACTION_FLAG_ASYNC_GENERATOR = 0x10;
  static constexpr uint32_t REACTION_FLAG_DEBUGGER_DUMMY = 0x20;
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
  void setIsAsyncGenerator(AsyncGeneratorObject* asyncGenObj) {
    setFlagOnInitialState(REACTION_FLAG_ASYNC_GENERATOR);
    setFixedSlot(ReactionRecordSlot_GeneratorOrPromiseToResolve,
                 ObjectValue(*asyncGenObj));
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

// ES2016, 25.4.1.3.
[[nodiscard]] static MOZ_ALWAYS_INLINE bool CreateResolvingFunctions(
    JSContext* cx, HandleObject promise, MutableHandleObject resolveFn,
    MutableHandleObject rejectFn) {
  HandlePropertyName funName = cx->names().empty;
  resolveFn.set(NewNativeFunction(cx, ResolvePromiseFunction, 1, funName,
                                  gc::AllocKind::FUNCTION_EXTENDED,
                                  GenericObject));
  if (!resolveFn) {
    return false;
  }

  rejectFn.set(NewNativeFunction(cx, RejectPromiseFunction, 1, funName,
                                 gc::AllocKind::FUNCTION_EXTENDED,
                                 GenericObject));
  if (!rejectFn) {
    return false;
  }

  JSFunction* resolveFun = &resolveFn->as<JSFunction>();
  JSFunction* rejectFun = &rejectFn->as<JSFunction>();

  resolveFun->initExtendedSlot(ResolveFunctionSlot_Promise,
                               ObjectValue(*promise));
  resolveFun->initExtendedSlot(ResolveFunctionSlot_RejectFunction,
                               ObjectValue(*rejectFun));

  rejectFun->initExtendedSlot(RejectFunctionSlot_Promise,
                              ObjectValue(*promise));
  rejectFun->initExtendedSlot(RejectFunctionSlot_ResolveFunction,
                              ObjectValue(*resolveFun));

  return true;
}

static void ClearResolutionFunctionSlots(JSFunction* resolutionFun);

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

// ES2016, 25.4.1.7.
[[nodiscard]] static bool RejectMaybeWrappedPromise(
    JSContext* cx, HandleObject promiseObj, HandleValue reason,
    HandleSavedFrame unwrappedRejectionStack);

// ES2016, 25.4.1.7.
[[nodiscard]] static bool RejectPromiseInternal(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue reason,
    HandleSavedFrame unwrappedRejectionStack = nullptr);

// ES2016, 25.4.1.3.1.
static bool RejectPromiseFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JSFunction* reject = &args.callee().as<JSFunction>();
  HandleValue reasonVal = args.get(0);

  // Steps 1-2.
  const Value& promiseVal = reject->getExtendedSlot(RejectFunctionSlot_Promise);

  // Steps 3-4.
  // If the Promise isn't available anymore, it has been resolved and the
  // reference to it removed to make it eligible for collection.
  if (promiseVal.isUndefined()) {
    args.rval().setUndefined();
    return true;
  }

  // Store the promise value in |promise| before ClearResolutionFunctionSlots
  // removes the reference.
  RootedObject promise(cx, &promiseVal.toObject());

  // Step 5.
  // Here, we only remove the Promise reference from the resolution
  // functions. Actually marking it as fulfilled/rejected happens later.
  ClearResolutionFunctionSlots(reject);

  // In some cases the Promise reference on the resolution function won't
  // have been removed during resolution, so we need to check that here,
  // too.
  if (IsSettledMaybeWrappedPromise(promise)) {
    args.rval().setUndefined();
    return true;
  }

  // Step 6.
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
                              MutableHandleValue rval, bool rvalUsed);

// ES2016, 25.4.1.3.2, steps 6-13.
[[nodiscard]] static bool ResolvePromiseInternal(JSContext* cx,
                                                 HandleObject promise,
                                                 HandleValue resolutionVal) {
  cx->check(promise, resolutionVal);
  MOZ_ASSERT(!IsSettledMaybeWrappedPromise(promise));

  // Step 7 (reordered).
  if (!resolutionVal.isObject()) {
    return FulfillMaybeWrappedPromise(cx, promise, resolutionVal);
  }

  RootedObject resolution(cx, &resolutionVal.toObject());

  // Step 6.
  if (resolution == promise) {
    // Step 6.a.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANNOT_RESOLVE_PROMISE_WITH_ITSELF);
    RootedValue selfResolutionError(cx);
    RootedSavedFrame stack(cx);
    if (!MaybeGetAndClearExceptionAndStack(cx, &selfResolutionError, &stack)) {
      return false;
    }

    // Step 6.b.
    return RejectMaybeWrappedPromise(cx, promise, selfResolutionError, stack);
  }

  // Step 8.
  RootedValue thenVal(cx);
  bool status =
      GetProperty(cx, resolution, resolution, cx->names().then, &thenVal);

  RootedValue error(cx);
  RootedSavedFrame errorStack(cx);
  if (!status) {
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

  // Step 9.
  if (!status) {
    return RejectMaybeWrappedPromise(cx, promise, error, errorStack);
  }

  // Step 10 (implicit).

  // Step 11.
  if (!IsCallable(thenVal)) {
    return FulfillMaybeWrappedPromise(cx, promise, resolutionVal);
  }

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

  // Step 12.
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

  // Step 13.
  return true;
}

// ES2016, 25.4.1.3.2.
static bool ResolvePromiseFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JSFunction* resolve = &args.callee().as<JSFunction>();
  HandleValue resolutionVal = args.get(0);

  // Steps 3-4 (reordered).
  // We use the reference to the reject function as a signal for whether
  // the resolve or reject function was already called, at which point
  // the references on each of the functions are cleared.
  if (!resolve->getExtendedSlot(ResolveFunctionSlot_RejectFunction)
           .isObject()) {
    args.rval().setUndefined();
    return true;
  }

  // Steps 1-2 (reordered).
  RootedObject promise(
      cx, &resolve->getExtendedSlot(ResolveFunctionSlot_Promise).toObject());

  // Step 5.
  // Here, we only remove the Promise reference from the resolution
  // functions. Actually marking it as fulfilled/rejected happens later.
  ClearResolutionFunctionSlots(resolve);

  // In some cases the Promise reference on the resolution function won't
  // have been removed during resolution, so we need to check that here,
  // too.
  if (IsSettledMaybeWrappedPromise(promise)) {
    args.rval().setUndefined();
    return true;
  }

  // Steps 6-13.
  if (!ResolvePromiseInternal(cx, promise, resolutionVal)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

static bool PromiseReactionJob(JSContext* cx, unsigned argc, Value* vp);

/**
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

  cx->check(handlerArg);
  reaction->setTargetStateAndHandlerArg(targetState, handlerArg);

  RootedValue reactionVal(cx, ObjectValue(*reaction));
  RootedValue handler(cx, reaction->handler());

  // If we have a handler callback, we enter that handler's compartment so
  // that the promise reaction job function is created in that compartment.
  // That guarantees that the embedding ends up with the right entry global.
  // This is relevant for some html APIs like fetch that derive information
  // from said global.
  mozilla::Maybe<AutoRealm> ar2;
  if (handler.isObject()) {
    // The unwrapping has to be unchecked because we specifically want to
    // be able to use handlers with wrappers that would only allow calls.
    // E.g., it's ok to have a handler from a chrome compartment in a
    // reaction to a content compartment's Promise instance.
    JSObject* handlerObj = UncheckedUnwrap(&handler.toObject());
    MOZ_ASSERT(handlerObj);
    ar2.emplace(cx, handlerObj);

    // We need to wrap the reaction to store it on the job function.
    if (!cx->compartment()->wrap(cx, &reactionVal)) {
      return false;
    }
  }

  // Create the JS function to call when the job is triggered.
  HandlePropertyName funName = cx->names().empty;
  RootedFunction job(
      cx, NewNativeFunction(cx, PromiseReactionJob, 0, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!job) {
    return false;
  }

  // Store the reaction on the reaction job.
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

// ES2016, Commoned-out implementation of 25.4.1.4. and 25.4.1.7.
//
// This method takes an additional optional |unwrappedRejectionStack| parameter,
// which is only used for debugging purposes.
// It allows callers to to pass in the stack of some exception which
// triggered the rejection of the promise.
[[nodiscard]] static bool ResolvePromise(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue valueOrReason,
    JS::PromiseState state,
    HandleSavedFrame unwrappedRejectionStack = nullptr) {
  // Step 1.
  MOZ_ASSERT(promise->state() == JS::PromiseState::Pending);
  MOZ_ASSERT(state == JS::PromiseState::Fulfilled ||
             state == JS::PromiseState::Rejected);
  MOZ_ASSERT_IF(unwrappedRejectionStack, state == JS::PromiseState::Rejected);

  // Step 2.
  // We only have one list of reactions for both resolution types. So
  // instead of getting the right list of reactions, we determine the
  // resolution type to retrieve the right information from the
  // reaction records.
  RootedValue reactionsVal(cx, promise->reactions());

  // Steps 3-5.
  // The same slot is used for the reactions list and the result, so setting
  // the result also removes the reactions list.
  promise->setFixedSlot(PromiseSlot_ReactionsOrResult, valueOrReason);

  // Step 6.
  int32_t flags = promise->flags();
  flags |= PROMISE_FLAG_RESOLVED;
  if (state == JS::PromiseState::Fulfilled) {
    flags |= PROMISE_FLAG_FULFILLED;
  }
  promise->setFixedSlot(PromiseSlot_Flags, Int32Value(flags));

  // Also null out the resolve/reject functions so they can be GC'd.
  promise->setFixedSlot(PromiseSlot_RejectFunction, UndefinedValue());

  // Now that everything else is done, do the things the debugger needs.
  // Step 7 of RejectPromise implemented in onSettled.
  PromiseObject::onSettled(cx, promise, unwrappedRejectionStack);

  // Step 7 of FulfillPromise.
  // Step 8 of RejectPromise.
  return TriggerPromiseReactions(cx, reactionsVal, state, valueOrReason);
}

// ES2016, 25.4.1.7.
[[nodiscard]] static bool RejectPromiseInternal(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue reason,
    HandleSavedFrame unwrappedRejectionStack) {
  return ResolvePromise(cx, promise, reason, JS::PromiseState::Rejected,
                        unwrappedRejectionStack);
}

// ES2016, 25.4.1.4.
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

[[nodiscard]] static PromiseObject*
CreatePromiseObjectWithoutResolutionFunctions(JSContext* cx) {
  PromiseObject* promise = CreatePromiseObjectInternal(cx);
  if (!promise) {
    return nullptr;
  }

  AddPromiseFlags(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS);
  return promise;
}

[[nodiscard]] static PromiseObject* CreatePromiseWithDefaultResolutionFunctions(
    JSContext* cx, MutableHandleObject resolve, MutableHandleObject reject) {
  // ES2016, 25.4.3.1., as if called with GetCapabilitiesExecutor as the
  // executor argument.

  // Steps 1-2 (Not applicable).

  // Steps 3-7.
  Rooted<PromiseObject*> promise(cx, CreatePromiseObjectInternal(cx));
  if (!promise) {
    return nullptr;
  }

  // Step 8.
  if (!CreateResolvingFunctions(cx, promise, resolve, reject)) {
    return nullptr;
  }

  promise->setFixedSlot(PromiseSlot_RejectFunction, ObjectValue(*reject));

  // Steps 9-10 (Not applicable).

  // Step 11.
  return promise;
}

// ES2016, 25.4.1.5.
[[nodiscard]] static bool NewPromiseCapability(
    JSContext* cx, HandleObject C, MutableHandle<PromiseCapability> capability,
    bool canOmitResolutionFunctions) {
  RootedValue cVal(cx, ObjectValue(*C));

  // Steps 1-2.
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

    capability.promise().set(promise);
    return true;
  }

  // Step 3 (omitted).

  // Step 4.
  HandlePropertyName funName = cx->names().empty;
  RootedFunction executor(
      cx, NewNativeFunction(cx, GetCapabilitiesExecutor, 2, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!executor) {
    return false;
  }

  // Step 5 (omitted).

  // Step 6.
  FixedConstructArgs<1> cargs(cx);
  cargs[0].setObject(*executor);
  if (!Construct(cx, cVal, cargs, cVal, capability.promise())) {
    return false;
  }

  // Step 7.
  const Value& resolveVal =
      executor->getExtendedSlot(GetCapabilitiesExecutorSlots_Resolve);
  if (!IsCallable(resolveVal)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROMISE_RESOLVE_FUNCTION_NOT_CALLABLE);
    return false;
  }

  // Step 8.
  const Value& rejectVal =
      executor->getExtendedSlot(GetCapabilitiesExecutorSlots_Reject);
  if (!IsCallable(rejectVal)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROMISE_REJECT_FUNCTION_NOT_CALLABLE);
    return false;
  }

  // Step 9 (well, the equivalent for all of promiseCapabilities' fields.)
  capability.resolve().set(&resolveVal.toObject());
  capability.reject().set(&rejectVal.toObject());

  // Step 10.
  return true;
}

// ES2016, 25.4.1.5.1.
static bool GetCapabilitiesExecutor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSFunction* F = &args.callee().as<JSFunction>();

  // Steps 1-2 (implicit).

  // Steps 3-4.
  if (!F->getExtendedSlot(GetCapabilitiesExecutorSlots_Resolve).isUndefined() ||
      !F->getExtendedSlot(GetCapabilitiesExecutorSlots_Reject).isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROMISE_CAPABILITY_HAS_SOMETHING_ALREADY);
    return false;
  }

  // Step 5.
  F->setExtendedSlot(GetCapabilitiesExecutorSlots_Resolve, args.get(0));

  // Step 6.
  F->setExtendedSlot(GetCapabilitiesExecutorSlots_Reject, args.get(1));

  // Step 7.
  args.rval().setUndefined();
  return true;
}

// ES2016, 25.4.1.7.
[[nodiscard]] static bool RejectMaybeWrappedPromise(
    JSContext* cx, HandleObject promiseObj, HandleValue reason_,
    HandleSavedFrame unwrappedRejectionStack) {
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

  HandleNativeObject reactionsList = reactions.as<NativeObject>();
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

// ES2016, 25.4.1.8.
[[nodiscard]] static bool TriggerPromiseReactions(JSContext* cx,
                                                  HandleValue reactionsVal,
                                                  JS::PromiseState state,
                                                  HandleValue valueOrReason) {
  MOZ_ASSERT(state == JS::PromiseState::Fulfilled ||
             state == JS::PromiseState::Rejected);

  return ForEachReaction(cx, reactionsVal, [&](MutableHandleObject reaction) {
    return EnqueuePromiseReactionJob(cx, reaction, valueOrReason, state);
  });
}

[[nodiscard]] static bool RunFulfillFunction(JSContext* cx,
                                             HandleObject onFulfilledFunc,
                                             HandleValue result,
                                             HandleObject promiseObj);

// Implements PromiseReactionJob optimized for the case when the reaction
// handler is one of the default resolving functions as created by the
// CreateResolvingFunctions abstract operation.
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
  RootedSavedFrame unwrappedRejectionStack(cx);
  if (promiseToResolve->state() == JS::PromiseState::Pending) {
    RootedValue argument(cx, reaction->handlerArg());

    // Step 6.
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

  // Steps 7-9.
  RootedObject promiseObj(cx, reaction->promise());
  RootedObject callee(cx);
  if (resolutionMode == ResolveMode) {
    callee =
        reaction->getFixedSlot(ReactionRecordSlot_Resolve).toObjectOrNull();

    return RunFulfillFunction(cx, callee, handlerResult, promiseObj);
  }

  callee = reaction->getFixedSlot(ReactionRecordSlot_Reject).toObjectOrNull();

  return RunRejectFunction(cx, callee, handlerResult, promiseObj,
                           unwrappedRejectionStack,
                           reaction->unhandledRejectionBehavior());
}

[[nodiscard]] static bool AsyncFunctionPromiseReactionJob(
    JSContext* cx, Handle<PromiseReactionRecord*> reaction) {
  MOZ_ASSERT(reaction->isAsyncFunction());

  int32_t handler = reaction->handler().toInt32();
  RootedValue argument(cx, reaction->handlerArg());
  Rooted<AsyncFunctionGeneratorObject*> generator(
      cx, reaction->asyncFunctionGenerator());

  // Await's handlers don't return a value, nor throw any exceptions.
  // They fail only on OOM.

  if (handler == PromiseHandlerAsyncFunctionAwaitedFulfilled) {
    return AsyncFunctionAwaitedFulfilled(cx, generator, argument);
  }

  MOZ_ASSERT(handler == PromiseHandlerAsyncFunctionAwaitedRejected);
  return AsyncFunctionAwaitedRejected(cx, generator, argument);
}

[[nodiscard]] static bool AsyncGeneratorPromiseReactionJob(
    JSContext* cx, Handle<PromiseReactionRecord*> reaction) {
  MOZ_ASSERT(reaction->isAsyncGenerator());

  RootedValue argument(cx, reaction->handlerArg());
  Rooted<AsyncGeneratorObject*> asyncGenObj(cx, reaction->asyncGenerator());

  // Await's handlers don't return a value, nor throw any exceptions.
  // They fail only on OOM.
  switch (int32_t handler = reaction->handler().toInt32(); handler) {
    // ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
    // 6.2.3.1.1 Await Fulfilled Functions
    case PromiseHandlerAsyncGeneratorAwaitedFulfilled: {
      MOZ_ASSERT(asyncGenObj->isExecuting(),
                 "Await fulfilled when not in 'Executing' state");

      return AsyncGeneratorAwaitedFulfilled(cx, asyncGenObj, argument);
    }

    // ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
    // 6.2.3.1.2 Await Rejected Functions
    case PromiseHandlerAsyncGeneratorAwaitedRejected: {
      MOZ_ASSERT(asyncGenObj->isExecuting(),
                 "Await rejected when not in 'Executing' state");

      return AsyncGeneratorAwaitedRejected(cx, asyncGenObj, argument);
    }

    // ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
    // 25.5.3.5.1 AsyncGeneratorResumeNext Return Processor Fulfilled Functions
    case PromiseHandlerAsyncGeneratorResumeNextReturnFulfilled: {
      MOZ_ASSERT(asyncGenObj->isAwaitingReturn(),
                 "AsyncGeneratorResumeNext-Return fulfilled when not in "
                 "'AwaitingReturn' state");

      // Steps 1-2.
      asyncGenObj->setCompleted();

      // Step 3.
      return AsyncGeneratorResolve(cx, asyncGenObj, argument, true);
    }

    // ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
    // 25.5.3.5.2 AsyncGeneratorResumeNext Return Processor Rejected Functions
    case PromiseHandlerAsyncGeneratorResumeNextReturnRejected: {
      MOZ_ASSERT(asyncGenObj->isAwaitingReturn(),
                 "AsyncGeneratorResumeNext-Return rejected when not in "
                 "'AwaitingReturn' state");

      // Steps 1-2.
      asyncGenObj->setCompleted();

      // Step 3.
      return AsyncGeneratorReject(cx, asyncGenObj, argument);
    }

    // ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
    // 25.5.3.7 AsyncGeneratorYield
    case PromiseHandlerAsyncGeneratorYieldReturnAwaitedFulfilled: {
      MOZ_ASSERT(asyncGenObj->isAwaitingYieldReturn(),
                 "YieldReturn-Await fulfilled when not in "
                 "'AwaitingYieldReturn' state");

      // We're using a separate 'AwaitingYieldReturn' state when awaiting a
      // return completion in yield expressions, whereas the spec uses the
      // 'Executing' state all along. So we now need to transition into the
      // 'Executing' state.
      asyncGenObj->setExecuting();

      // Steps 8.d-e.
      return AsyncGeneratorYieldReturnAwaitedFulfilled(cx, asyncGenObj,
                                                       argument);
    }

    // ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
    // 25.5.3.7 AsyncGeneratorYield
    case PromiseHandlerAsyncGeneratorYieldReturnAwaitedRejected: {
      MOZ_ASSERT(
          asyncGenObj->isAwaitingYieldReturn(),
          "YieldReturn-Await rejected when not in 'AwaitingYieldReturn' state");

      // We're using a separate 'AwaitingYieldReturn' state when awaiting a
      // return completion in yield expressions, whereas the spec uses the
      // 'Executing' state all along. So we now need to transition into the
      // 'Executing' state.
      asyncGenObj->setExecuting();

      // Step 8.c.
      return AsyncGeneratorYieldReturnAwaitedRejected(cx, asyncGenObj,
                                                      argument);
    }

    default:
      MOZ_CRASH("Bad handler in AsyncGeneratorPromiseReactionJob");
  }
}

// ES2016, 25.4.2.1.
/**
 * Callback triggering the fulfill/reject reaction for a resolved Promise,
 * to be invoked by the embedding during its processing of the Promise job
 * queue.
 *
 * See
 * http://www.ecma-international.org/ecma-262/7.0/index.html#sec-jobs-and-job-queues
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

  // Steps 1-2.
  Handle<PromiseReactionRecord*> reaction =
      reactionObj.as<PromiseReactionRecord>();
  if (reaction->isDefaultResolvingHandler()) {
    return DefaultResolvingPromiseReactionJob(cx, reaction);
  }
  if (reaction->isAsyncFunction()) {
    return AsyncFunctionPromiseReactionJob(cx, reaction);
  }
  if (reaction->isAsyncGenerator()) {
    return AsyncGeneratorPromiseReactionJob(cx, reaction);
  }
  if (reaction->isDebuggerDummy()) {
    return true;
  }

  // Step 3.
  RootedValue handlerVal(cx, reaction->handler());

  RootedValue argument(cx, reaction->handlerArg());

  RootedValue handlerResult(cx);
  ResolutionMode resolutionMode = ResolveMode;

  RootedSavedFrame unwrappedRejectionStack(cx);

  // Steps 4-6.
  if (handlerVal.isInt32()) {
    int32_t handlerNum = handlerVal.toInt32();

    // Step 4.
    if (handlerNum == PromiseHandlerIdentity) {
      handlerResult = argument;
    } else if (handlerNum == PromiseHandlerThrower) {
      // Step 5.
      resolutionMode = RejectMode;
      handlerResult = argument;
    } else {
      MOZ_ASSERT(
          handlerNum == PromiseHandlerAsyncFromSyncIteratorValueUnwrapDone ||
          handlerNum == PromiseHandlerAsyncFromSyncIteratorValueUnwrapNotDone);

      bool done =
          handlerNum == PromiseHandlerAsyncFromSyncIteratorValueUnwrapDone;
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

    // Step 6.
    if (!Call(cx, handlerVal, UndefinedHandleValue, argument, &handlerResult)) {
      resolutionMode = RejectMode;
      if (!MaybeGetAndClearExceptionAndStack(cx, &handlerResult,
                                             &unwrappedRejectionStack)) {
        return false;
      }
    }
  }

  // Steps 7-9.
  RootedObject promiseObj(cx, reaction->promise());
  RootedObject callee(cx);
  if (resolutionMode == ResolveMode) {
    callee =
        reaction->getFixedSlot(ReactionRecordSlot_Resolve).toObjectOrNull();

    return RunFulfillFunction(cx, callee, handlerResult, promiseObj);
  }

  callee = reaction->getFixedSlot(ReactionRecordSlot_Reject).toObjectOrNull();

  return RunRejectFunction(cx, callee, handlerResult, promiseObj,
                           unwrappedRejectionStack,
                           reaction->unhandledRejectionBehavior());
}

// ES2016, 25.4.2.2.
/**
 * Callback for resolving a thenable, to be invoked by the embedding during
 * its processing of the Promise job queue.
 *
 * See https://tc39.github.io/ecma262/#sec-jobs-and-job-queues
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
  MOZ_ASSERT(!IsWrapper(&then.toObject()));
  RootedNativeObject jobArgs(cx, &job->getExtendedSlot(ThenableJobSlot_JobData)
                                      .toObject()
                                      .as<NativeObject>());

  RootedObject promise(
      cx, &jobArgs->getDenseElement(ThenableJobDataIndex_Promise).toObject());
  RootedValue thenable(cx,
                       jobArgs->getDenseElement(ThenableJobDataIndex_Thenable));

  // Step 1.
  RootedObject resolveFn(cx);
  RootedObject rejectFn(cx);
  if (!CreateResolvingFunctions(cx, promise, &resolveFn, &rejectFn)) {
    return false;
  }

  // Step 2.
  FixedInvokeArgs<2> args2(cx);
  args2[0].setObject(*resolveFn);
  args2[1].setObject(*rejectFn);

  // In difference to the usual pattern, we return immediately on success.
  RootedValue rval(cx);
  if (Call(cx, then, thenable, args2, &rval)) {
    return true;
  }

  // Steps 3-4.
  // Can't pass stack to a JS function.
  RootedSavedFrame stack(cx);
  if (!MaybeGetAndClearExceptionAndStack(cx, &rval, &stack)) {
    return false;
  }

  RootedValue rejectVal(cx, ObjectValue(*rejectFn));
  return Call(cx, rejectVal, UndefinedHandleValue, rval, &rval);
}

[[nodiscard]] static bool OriginalPromiseThenWithoutSettleHandlers(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseObject*> promiseToResolve);

/**
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

  // Step 1 (Skipped).

  // Step 2.
  // In difference to the usual pattern, we return immediately on success.
  if (OriginalPromiseThenWithoutSettleHandlers(cx, thenable.as<PromiseObject>(),
                                               promise.as<PromiseObject>())) {
    return true;
  }

  // Steps 3-4.
  RootedValue exception(cx);
  RootedSavedFrame stack(cx);
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

  return RejectPromiseInternal(cx, promise.as<PromiseObject>(), exception,
                               stack);
}

/**
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

  // We enter the `then` callable's compartment so that the job function is
  // created in that compartment.
  // That guarantees that the embedding ends up with the right entry global.
  // This is relevant for some html APIs like fetch that derive information
  // from said global.
  RootedObject then(cx, CheckedUnwrapStatic(&thenVal.toObject()));
  AutoRealm ar(cx, then);

  // Wrap the `promiseToResolve` and `thenable` arguments.
  if (!cx->compartment()->wrap(cx, &promiseToResolve)) {
    return false;
  }

  MOZ_ASSERT(thenable.isObject());
  if (!cx->compartment()->wrap(cx, &thenable)) {
    return false;
  }

  HandlePropertyName funName = cx->names().empty;
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
  RootedArrayObject data(
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
  return cx->runtime()->enqueuePromiseJob(cx, job, promise, incumbentGlobal);
}

/**
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

  HandlePropertyName funName = cx->names().empty;
  RootedFunction job(
      cx, NewNativeFunction(cx, PromiseResolveBuiltinThenableJob, 0, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!job) {
    return false;
  }

  // Store the promise and the thenable on the reaction job.
  job->setExtendedSlot(BuiltinThenableJobSlot_Promise,
                       ObjectValue(*promiseToResolve));
  job->setExtendedSlot(BuiltinThenableJobSlot_Thenable, ObjectValue(*thenable));

  Rooted<GlobalObject*> incumbentGlobal(cx,
                                        cx->runtime()->getIncumbentGlobal(cx));
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

static void ClearResolutionFunctionSlots(JSFunction* resolutionFun) {
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
}

// ES2016, 25.4.3.1. steps 3-7.
[[nodiscard]] static MOZ_ALWAYS_INLINE PromiseObject*
CreatePromiseObjectInternal(JSContext* cx, HandleObject proto /* = nullptr */,
                            bool protoIsWrapped /* = false */,
                            bool informDebugger /* = true */) {
  // Step 3.
  // Enter the unwrapped proto's compartment, if that's different from
  // the current one.
  // All state stored in a Promise's fixed slots must be created in the
  // same compartment, so we get all of that out of the way here.
  // (Except for the resolution functions, which are created below.)
  mozilla::Maybe<AutoRealm> ar;
  if (protoIsWrapped) {
    ar.emplace(cx, proto);
  }

  PromiseObject* promise = NewObjectWithClassProto<PromiseObject>(cx, proto);
  if (!promise) {
    return nullptr;
  }

  // Step 4.
  promise->initFixedSlot(PromiseSlot_Flags, Int32Value(0));

  // Steps 5-6.
  // Omitted, we allocate our single list of reaction records lazily.

  // Step 7.
  // Implicit, the handled flag is unset by default.

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

// ES2016, 25.4.3.1.
static bool PromiseConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Promise")) {
    return false;
  }

  // Step 2.
  HandleValue executorVal = args.get(0);
  if (!IsCallable(executorVal)) {
    return ReportIsNotFunction(cx, executorVal);
  }
  RootedObject executor(cx, &executorVal.toObject());

  // Steps 3-10.
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

// ES2016, 25.4.3.1. steps 3-11.
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

  // Step 8.
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

  // Step 9.
  bool success;
  {
    FixedInvokeArgs<2> args(cx);
    args[0].setObject(*resolveFn);
    args[1].setObject(*rejectFn);

    RootedValue calleeOrRval(cx, ObjectValue(*executor));
    success = Call(cx, calleeOrRval, UndefinedHandleValue, args, &calleeOrRval);
  }

  // Step 10.
  if (!success) {
    RootedValue exceptionVal(cx);
    // Can't pass stack to a JS function.
    RootedSavedFrame stack(cx);
    if (!MaybeGetAndClearExceptionAndStack(cx, &exceptionVal, &stack)) {
      return nullptr;
    }

    RootedValue calleeOrRval(cx, ObjectValue(*rejectFn));
    if (!Call(cx, calleeOrRval, UndefinedHandleValue, exceptionVal,
              &calleeOrRval)) {
      return nullptr;
    }
  }

  // Let the Debugger know about this Promise.
  DebugAPI::onNewPromise(cx, promise);

  // Step 11.
  return promise;
}

// ES2016, 25.4.3.1. skipping creation of resolution functions and executor
// function invocation.
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

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
//
// Unified implementation of
// 25.6.4.1 Promise.all ( iterable )
// 25.6.4.2 Promise.allSettled ( iterable )
// 25.6.4.4 Promise.race ( iterable )
//
// Promise.any (Stage 3 proposal)
// https://tc39.es/proposal-promise-any/
//
// Promise.any ( iterable )
[[nodiscard]] static bool CommonPromiseCombinator(JSContext* cx, CallArgs& args,
                                                  CombinatorKind kind) {
  HandleValue iterable = args.get(0);

  // Step 2 (moved from NewPromiseCapability, step 1).
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

  // Step 1.
  RootedObject C(cx, &CVal.toObject());

  // Step 2.
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
      // 25.6.4.1, step 3.
      // 25.6.4.2, step 3.
      // 25.6.4.4, step 3.
      if (!GetProperty(cx, C, C, cx->names().resolve, &promiseResolve)) {
        return AbruptRejectPromise(cx, args, promiseCapability);
      }

      // 25.6.4.1, step 4.
      // 25.6.4.2, step 4.
      // 25.6.4.4, step 4.
      if (!IsCallable(promiseResolve)) {
        ReportIsNotFunction(cx, promiseResolve);
        return AbruptRejectPromise(cx, args, promiseCapability);
      }
    }
  }

  // Steps 3-4.
  PromiseForOfIterator iter(cx);
  if (!iter.init(iterable, JS::ForOfIterator::AllowNonIterable)) {
    return AbruptRejectPromise(cx, args, promiseCapability);
  }

  if (!iter.valueIsIterable()) {
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

  // Step 5.
  bool done, result;
  switch (kind) {
    case CombinatorKind::All:
      result = PerformPromiseAll(cx, iter, C, promiseCapability, promiseResolve,
                                 &done);
      break;
    case CombinatorKind::AllSettled:
      result = PerformPromiseAllSettled(cx, iter, C, promiseCapability,
                                        promiseResolve, &done);
      break;
    case CombinatorKind::Any:
      result = PerformPromiseAny(cx, iter, C, promiseCapability, promiseResolve,
                                 &done);
      break;
    case CombinatorKind::Race:
      result = PerformPromiseRace(cx, iter, C, promiseCapability,
                                  promiseResolve, &done);
      break;
  }

  // Step 6.
  if (!result) {
    // Step 6.a.
    if (!done) {
      iter.closeThrow();
    }

    // Step 6.b.
    return AbruptRejectPromise(cx, args, promiseCapability);
  }

  // Step 7.
  args.rval().setObject(*promiseCapability.promise());
  return true;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 25.6.4.1 Promise.all ( iterable )
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

// Unforgeable version of ES2016, 25.4.4.1.
[[nodiscard]] JSObject* js::GetWaitForAllPromise(
    JSContext* cx, JS::HandleObjectVector promises) {
#ifdef DEBUG
  for (size_t i = 0, len = promises.length(); i < len; i++) {
    JSObject* obj = promises[i];
    cx->check(obj);
    MOZ_ASSERT(UncheckedUnwrap(obj)->is<PromiseObject>());
  }
#endif

  // Step 1.
  RootedObject C(cx,
                 GlobalObject::getOrCreatePromiseConstructor(cx, cx->global()));
  if (!C) {
    return nullptr;
  }

  // Step 2 (omitted).

  // Step 3.
  Rooted<PromiseCapability> resultCapability(cx);
  if (!NewPromiseCapability(cx, C, &resultCapability, false)) {
    return nullptr;
  }

  // Steps 4-6 (omitted).

  // Step 7.
  // Implemented as an inlined, simplied version of ES2016 25.4.4.1.1,
  // PerformPromiseAll.
  {
    uint32_t promiseCount = promises.length();
    // Sub-steps 1-2 (omitted).

    // Sub-step 3.
    Rooted<PromiseCombinatorElements> values(cx);
    {
      auto* valuesArray = NewDenseFullyAllocatedArray(cx, promiseCount);
      if (!valuesArray) {
        return nullptr;
      }
      valuesArray->ensureDenseInitializedLength(0, promiseCount);

      values.initialize(valuesArray);
    }

    // Sub-step 4.
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

    // Sub-step 5 (inline in loop-header below).

    // Sub-step 6.
    for (uint32_t index = 0; index < promiseCount; index++) {
      // Steps a-c (omitted).
      // Step d (implemented after the loop).
      // Steps e-g (omitted).

      // Step h.
      values.unwrappedArray()->setDenseElement(index, UndefinedHandleValue);

      // Step i, vastly simplified.
      RootedObject nextPromiseObj(cx, promises[index]);

      // Steps j-o.
      JSFunction* resolveFunc = NewPromiseCombinatorElementFunction(
          cx, PromiseAllResolveElementFunction, dataHolder, index);
      if (!resolveFunc) {
        return nullptr;
      }

      // Step p.
      dataHolder->increaseRemainingCount();

      // Step q, very roughly.
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

      // Step r (inline in loop-header).
    }

    // Sub-step d.i (implicit).
    // Sub-step d.ii.
    int32_t remainingCount = dataHolder->decreaseRemainingCount();

    // Sub-step d.iii-iv.
    if (remainingCount == 0) {
      if (!ResolvePromiseInternal(cx, resultCapability.promise(),
                                  values.value())) {
        return nullptr;
      }
    }
  }

  // Step 8 (omitted).

  // Step 9.
  return resultCapability.promise();
}

[[nodiscard]] static bool RunFulfillFunction(JSContext* cx,
                                             HandleObject onFulfilledFunc,
                                             HandleValue result,
                                             HandleObject promiseObj) {
  cx->check(onFulfilledFunc);
  cx->check(result);
  cx->check(promiseObj);

  // If |onFulfilledFunc| couldn't be optimized away, just call it.
  if (onFulfilledFunc) {
    RootedValue calleeOrRval(cx, ObjectValue(*onFulfilledFunc));
    return Call(cx, calleeOrRval, UndefinedHandleValue, result, &calleeOrRval);
  }

  // The promise itself may be optimized away.  If so, we're done.
  if (!promiseObj) {
    return true;
  }

  // Resolve the promise only if it's still pending.
  Handle<PromiseObject*> promise = promiseObj.as<PromiseObject>();
  if (promise->state() != JS::PromiseState::Pending) {
    return true;
  }

  // If the promise has a default resolution function, perform its steps.
  if (PromiseHasAnyFlag(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS)) {
    return ResolvePromiseInternal(cx, promise, result);
  }

  // Otherwise we're done.
  return true;
}

[[nodiscard]] static bool RunRejectFunction(
    JSContext* cx, HandleObject onRejectedFunc, HandleValue result,
    HandleObject promiseObj, HandleSavedFrame unwrappedRejectionStack,
    UnhandledRejectionBehavior behavior) {
  cx->check(onRejectedFunc);
  cx->check(result);
  cx->check(promiseObj);

  // If |onRejectedFunc| couldn't be optimized away, just call it.
  if (onRejectedFunc) {
    RootedValue calleeOrRval(cx, ObjectValue(*onRejectedFunc));
    return Call(cx, calleeOrRval, UndefinedHandleValue, result, &calleeOrRval);
  }

  // The promise itself may be optimized away.
  if (!promiseObj) {
    // Do nothing if unhandled rejections are to be ignored.
    if (behavior == UnhandledRejectionBehavior::Ignore) {
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

    return RejectPromiseInternal(cx, temporaryPromise, result,
                                 unwrappedRejectionStack);
  }

  // Reject the promise only if it's still pending.
  Handle<PromiseObject*> promise = promiseObj.as<PromiseObject>();
  if (promise->state() != JS::PromiseState::Pending) {
    return true;
  }

  // If the promise has a default rejection function, perform its steps.
  if (PromiseHasAnyFlag(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS)) {
    return RejectPromiseInternal(cx, promise, result, unwrappedRejectionStack);
  }

  // Otherwise we're done.
  return true;
}

[[nodiscard]] static JSObject* CommonStaticResolveRejectImpl(
    JSContext* cx, HandleValue thisVal, HandleValue argVal,
    ResolutionMode mode);

static bool IsPromiseSpecies(JSContext* cx, JSFunction* species);

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
// 25.6.4.1.1 Runtime Semantics: PerformPromiseAll, steps 5-6 and step 8.
// 25.6.4.2.1 Runtime Semantics: PerformPromiseAllSettled, steps 5-6 and step 8.
// 25.6.4.4.1 Runtime Semantics: PerformPromiseRace, steps 3-5.
//
// Promise.any (Stage 3 proposal)
// https://tc39.es/proposal-promise-any/
// Runtime Semantics: PerformPromiseAny, steps 6-8.
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

  while (true) {
    // Steps a-c, e-g.
    RootedValue& nextValue = nextValueOrNextPromise;
    if (!iterator.next(&nextValue, done)) {
      // Steps b, f.
      *done = true;

      // Steps c, g.
      return false;
    }

    // Step d.
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

        // 25.6.4.1.1, step 8.i.
        // 25.6.4.3.1, step 5.h.
        // Promise.resolve is a no-op for the default case.
        MOZ_ASSERT(&nextPromise.toObject() == nextValuePromise);

        // `nextPromise` uses the built-in `then` function.
        getThen = false;
      } else {
        // Need to revalidate the Promise state in the next iteration,
        // because CommonStaticResolveRejectImpl may have modified it.
        validatePromiseState = true;

        // 25.6.4.1.1, step 8.i.
        // 25.6.4.3.1, step 5.h.
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

      // 25.6.4.1.1, step 8.i.
      // 25.6.4.3.1, step 5.h.
      // Inline the call to Promise.resolve.
      JSObject* res =
          CommonStaticResolveRejectImpl(cx, CVal, nextValue, ResolveMode);
      if (!res) {
        return false;
      }

      nextPromise.setObject(*res);
    } else {
      // 25.6.4.1.1, step 8.i.
      // 25.6.4.3.1, step 5.h.
      if (!Call(cx, promiseResolve, CVal, nextValue, &nextPromise)) {
        return false;
      }
    }

    // Get the resolving functions for this iteration.
    // 25.6.4.1.1, steps 8.j-q.
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

    // 25.6.4.1.1, step 8.r.
    // 25.6.4.3.1, step 5.i.
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

      // 25.6.5.4, step 3.
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
          !PromiseHasAnyFlag(resultPromise->as<PromiseObject>(),
                             PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS)) {
        thenCapability.promise().set(resultPromise);
        addToDependent = false;
      } else {
        // 25.6.5.4, step 4.
        if (!NewPromiseCapability(cx, thenSpecies, &thenCapability, true)) {
          return false;
        }
      }

      // 25.6.5.4, step 5.
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

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
// 25.6.4.1.2 Promise.all Resolve Element Functions
// 25.6.4.2.2 Promise.allSettled Resolve Element Functions
// 25.6.4.2.3 Promise.allSettled Reject Element Functions
//
// Common implementation for Promise combinator element functions to check if
// they've already been called.
static bool PromiseCombinatorElementFunctionAlreadyCalled(
    const CallArgs& args, MutableHandle<PromiseCombinatorDataHolder*> data,
    uint32_t* index) {
  // Step 1.
  JSFunction* fn = &args.callee().as<JSFunction>();

  // Step 2.
  const Value& dataVal =
      fn->getExtendedSlot(PromiseCombinatorElementFunctionSlot_Data);

  // Step 3.
  // We use the existence of the data holder as a signal for whether the Promise
  // combinator element function was already called. Upon resolution, it's reset
  // to `undefined`.
  if (dataVal.isUndefined()) {
    return true;
  }

  data.set(&dataVal.toObject().as<PromiseCombinatorDataHolder>());

  // Step 4.
  fn->setExtendedSlot(PromiseCombinatorElementFunctionSlot_Data,
                      UndefinedValue());

  // Step 5.
  int32_t idx =
      fn->getExtendedSlot(PromiseCombinatorElementFunctionSlot_ElementIndex)
          .toInt32();
  MOZ_ASSERT(idx >= 0);
  *index = uint32_t(idx);

  return false;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 25.6.4.1.1 PerformPromiseAll (iteratorRecord, constructor, resultCapability)
[[nodiscard]] static bool PerformPromiseAll(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done) {
  *done = false;

  // Step 1.
  MOZ_ASSERT(C->isConstructor());

  // Step 2 (omitted).

  // Step 3.
  Rooted<PromiseCombinatorElements> values(cx);
  if (!NewPromiseCombinatorElements(cx, resultCapability, &values)) {
    return false;
  }

  // Step 4.
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

  // Step 7.
  uint32_t index = 0;

  auto getResolveAndReject = [cx, &resultCapability, &values, &dataHolder,
                              &index](MutableHandleValue resolveFunVal,
                                      MutableHandleValue rejectFunVal) {
    // Step 8.h.
    if (!values.pushUndefined(cx)) {
      return false;
    }

    // Steps 8.j-p.
    JSFunction* resolveFunc = NewPromiseCombinatorElementFunction(
        cx, PromiseAllResolveElementFunction, dataHolder, index);
    if (!resolveFunc) {
      return false;
    }

    // Step 8.q.
    dataHolder->increaseRemainingCount();

    // Step 8.s.
    index++;
    MOZ_ASSERT(index > 0);

    resolveFunVal.setObject(*resolveFunc);
    rejectFunVal.setObject(*resultCapability.reject());
    return true;
  };

  // Steps 5-6 and 8.
  if (!CommonPerformPromiseCombinator(
          cx, iterator, C, resultCapability.promise(), promiseResolve, done,
          true, getResolveAndReject)) {
    return false;
  }

  // Step 8.d.ii.
  int32_t remainingCount = dataHolder->decreaseRemainingCount();

  // Steps 8.d.iii-iv.
  if (remainingCount == 0) {
    return RunFulfillFunction(cx, resultCapability.resolve(), values.value(),
                              resultCapability.promise());
  }

  return true;
}

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
// 25.6.4.1.2 Promise.all Resolve Element Functions
static bool PromiseAllResolveElementFunction(JSContext* cx, unsigned argc,
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
  Rooted<PromiseCombinatorElements> values(cx);
  if (!GetPromiseCombinatorElements(cx, data, &values)) {
    return false;
  }

  // Step 7 (moved under step 11).
  // Step 8 (moved to step 10).

  // Step 9.
  if (!values.setElement(cx, index, xVal)) {
    return false;
  }

  // Steps 8,10.
  uint32_t remainingCount = data->decreaseRemainingCount();

  // Step 11.
  if (remainingCount == 0) {
    // Step 11.a. (Omitted, happened in PerformPromiseAll.)
    // Step 11.b.

    // Step 7 (Adapted to work with PromiseCombinatorDataHolder's layout).
    RootedObject resolveAllFun(cx, data->resolveOrRejectObj());
    RootedObject promiseObj(cx, data->promiseObj());
    if (!RunFulfillFunction(cx, resolveAllFun, values.value(), promiseObj)) {
      return false;
    }
  }

  // Step 12.
  args.rval().setUndefined();
  return true;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 25.6.4.3 Promise.race ( iterable )
static bool Promise_static_race(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CommonPromiseCombinator(cx, args, CombinatorKind::Race);
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 25.6.4.3.1 PerformPromiseRace (iteratorRecord, constructor, resultCapability)
[[nodiscard]] static bool PerformPromiseRace(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done) {
  *done = false;

  // Step 1.
  MOZ_ASSERT(C->isConstructor());

  // Step 2 (omitted).

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

  // Steps 3-5.
  return CommonPerformPromiseCombinator(
      cx, iterator, C, resultCapability.promise(), promiseResolve, done,
      isDefaultResolveFn, getResolveAndReject);
}

enum class PromiseAllSettledElementFunctionKind { Resolve, Reject };

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
// 25.6.4.2 Promise.allSettled ( iterable )
//
// Promise.allSettled Resolve Element Functions
// Promise.allSettled Reject Element Functions
template <PromiseAllSettledElementFunctionKind Kind>
static bool PromiseAllSettledElementFunction(JSContext* cx, unsigned argc,
                                             Value* vp);

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
// 25.6.4.2 Promise.allSettled ( iterable )
//
// Promise.allSettled ( iterable )
static bool Promise_static_allSettled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CommonPromiseCombinator(cx, args, CombinatorKind::AllSettled);
}

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
// 25.6.4.2 Promise.allSettled ( iterable )
//
// PerformPromiseAllSettled ( iteratorRecord, constructor, resultCapability )
[[nodiscard]] static bool PerformPromiseAllSettled(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done) {
  *done = false;

  // Step 1.
  MOZ_ASSERT(C->isConstructor());

  // Step 2 (omitted).

  // Step 3.
  Rooted<PromiseCombinatorElements> values(cx);
  if (!NewPromiseCombinatorElements(cx, resultCapability, &values)) {
    return false;
  }

  // Step 4.
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

  // Step 7.
  uint32_t index = 0;

  auto getResolveAndReject = [cx, &values, &dataHolder, &index](
                                 MutableHandleValue resolveFunVal,
                                 MutableHandleValue rejectFunVal) {
    // Step 8.h.
    if (!values.pushUndefined(cx)) {
      return false;
    }

    auto PromiseAllSettledResolveElementFunction =
        PromiseAllSettledElementFunction<
            PromiseAllSettledElementFunctionKind::Resolve>;
    auto PromiseAllSettledRejectElementFunction =
        PromiseAllSettledElementFunction<
            PromiseAllSettledElementFunctionKind::Reject>;

    // Steps 8.j-q.
    JSFunction* resolveFunc = NewPromiseCombinatorElementFunction(
        cx, PromiseAllSettledResolveElementFunction, dataHolder, index);
    if (!resolveFunc) {
      return false;
    }
    resolveFunVal.setObject(*resolveFunc);

    // Steps 8.r-x.
    JSFunction* rejectFunc = NewPromiseCombinatorElementFunction(
        cx, PromiseAllSettledRejectElementFunction, dataHolder, index);
    if (!rejectFunc) {
      return false;
    }
    rejectFunVal.setObject(*rejectFunc);

    // Step 8.y.
    dataHolder->increaseRemainingCount();

    // Step 8.aa.
    index++;
    MOZ_ASSERT(index > 0);

    return true;
  };

  // Steps 5-6 and 8.
  if (!CommonPerformPromiseCombinator(
          cx, iterator, C, resultCapability.promise(), promiseResolve, done,
          true, getResolveAndReject)) {
    return false;
  }

  // Step 8.d.ii.
  int32_t remainingCount = dataHolder->decreaseRemainingCount();

  // Steps 8.d.iii-iv.
  if (remainingCount == 0) {
    return RunFulfillFunction(cx, resultCapability.resolve(), values.value(),
                              resultCapability.promise());
  }

  return true;
}

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
// 25.6.4.2.2 Promise.allSettled Resolve Element Functions
// 25.6.4.2.3 Promise.allSettled Reject Element Functions
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

  // Step 6.
  Rooted<PromiseCombinatorElements> values(cx);
  if (!GetPromiseCombinatorElements(cx, data, &values)) {
    return false;
  }

  // Steps 2-3.
  // The already-called check above only handles the case when |this| function
  // is called repeatedly, so we still need to check if the other pair of this
  // resolving function was already called:
  // We use the element value as a signal for whether the Promise was already
  // fulfilled. Upon resolution, it's set to the result object created below.
  if (!values.unwrappedArray()->getDenseElement(index).isUndefined()) {
    args.rval().setUndefined();
    return true;
  }

  // Steps 7-8 (moved below).

  // Step 9.
  RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
  if (!obj) {
    return false;
  }

  // Step 10.
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

  // Step 11.
  if (Kind == PromiseAllSettledElementFunctionKind::Resolve) {
    id = NameToId(cx->names().value);
  } else {
    id = NameToId(cx->names().reason);
  }
  if (!NativeDefineDataProperty(cx, obj, id, valueOrReason, JSPROP_ENUMERATE)) {
    return false;
  }

  // Steps 4, 12.
  RootedValue objVal(cx, ObjectValue(*obj));
  if (!values.setElement(cx, index, objVal)) {
    return false;
  }

  // Steps 8, 13.
  uint32_t remainingCount = data->decreaseRemainingCount();

  // Step 14.
  if (remainingCount == 0) {
    // Step 14.a. (Omitted, happened in PerformPromiseAllSettled.)
    // Step 14.b.

    // Step 7 (Adapted to work with PromiseCombinatorDataHolder's layout).
    RootedObject resolveAllFun(cx, data->resolveOrRejectObj());
    RootedObject promiseObj(cx, data->promiseObj());
    if (!RunFulfillFunction(cx, resolveAllFun, values.value(), promiseObj)) {
      return false;
    }
  }

  // Step 15.
  args.rval().setUndefined();
  return true;
}

// Promise.any (Stage 3 proposal)
// https://tc39.es/proposal-promise-any/
//
// Promise.any ( iterable )
static bool Promise_static_any(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CommonPromiseCombinator(cx, args, CombinatorKind::Any);
}

// Promise.any (Stage 3 proposal)
// https://tc39.es/proposal-promise-any/
//
// Promise.any Reject Element Functions
static bool PromiseAnyRejectElementFunction(JSContext* cx, unsigned argc,
                                            Value* vp);

// Promise.any (Stage 3 proposal)
// https://tc39.es/proposal-promise-any/
//
// ThrowAggregateError ( errors )
static void ThrowAggregateError(JSContext* cx,
                                Handle<PromiseCombinatorElements> errors,
                                HandleObject promise);

// Promise.any (Stage 3 proposal)
// https://tc39.es/proposal-promise-any/
//
// PerformPromiseAny ( iteratorRecord, constructor, resultCapability )
[[nodiscard]] static bool PerformPromiseAny(
    JSContext* cx, PromiseForOfIterator& iterator, HandleObject C,
    Handle<PromiseCapability> resultCapability, HandleValue promiseResolve,
    bool* done) {
  *done = false;

  // Step 1.
  MOZ_ASSERT(C->isConstructor());

  // Step 2 (omitted).

  // Step 3.
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

  // Step 5.
  uint32_t index = 0;

  auto getResolveAndReject = [cx, &resultCapability, &errors, &dataHolder,
                              &index](MutableHandleValue resolveFunVal,
                                      MutableHandleValue rejectFunVal) {
    // Step 8.h.
    if (!errors.pushUndefined(cx)) {
      return false;
    }

    // Steps 8.j-p.
    JSFunction* rejectFunc = NewPromiseCombinatorElementFunction(
        cx, PromiseAnyRejectElementFunction, dataHolder, index);
    if (!rejectFunc) {
      return false;
    }

    // Step 8.q.
    dataHolder->increaseRemainingCount();

    // Step 8.s.
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

  // Steps 6-8.
  if (!CommonPerformPromiseCombinator(
          cx, iterator, C, resultCapability.promise(), promiseResolve, done,
          isDefaultResolveFn, getResolveAndReject)) {
    return false;
  }

  // Step 8.d.ii.
  int32_t remainingCount = dataHolder->decreaseRemainingCount();

  // Step 8.d.iii.
  if (remainingCount == 0) {
    ThrowAggregateError(cx, errors, resultCapability.promise());
    return false;
  }

  // Step 8.d.iv.
  return true;
}

// Promise.any (Stage 3 proposal)
// https://tc39.es/proposal-promise-any/
//
// Promise.any Reject Element Functions
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
    RootedSavedFrame stack(cx);
    if (!MaybeGetAndClearExceptionAndStack(cx, &reason, &stack)) {
      return false;
    }

    if (!RunRejectFunction(cx, rejectFun, reason, promiseObj, stack,
                           UnhandledRejectionBehavior::Report)) {
      return false;
    }
  }

  // Step 12.
  args.rval().setUndefined();
  return true;
}

// Promise.any (Stage 3 proposal)
// https://tc39.es/proposal-promise-any/
//
// ThrowAggregateError ( errors )
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

  // AutoSetAsyncStackForNewCalls requires a new activation before it takes
  // effect, so call into the self-hosting helper to set-up new call frames.
  RootedValue error(cx);
  if (!GetAggregateError(cx, JSMSG_PROMISE_ANY_REJECTION, &error)) {
    return;
  }

  // |error| isn't guaranteed to be an AggregateError in case of OOM or stack
  // overflow.
  RootedSavedFrame stack(cx);
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

  cx->setPendingException(error, stack);
}

// https://tc39.github.io/ecma262/#sec-promise.reject
//
// Unified implementation of
// 25.6.4.4 Promise.reject ( r )
// 25.6.4.5 Promise.resolve ( x )
// 25.6.4.5.1 PromiseResolve ( C, x )
[[nodiscard]] static JSObject* CommonStaticResolveRejectImpl(
    JSContext* cx, HandleValue thisVal, HandleValue argVal,
    ResolutionMode mode) {
  // Steps 1-2 of Promise.reject and Promise.resolve.
  // Step 1: Let C be the this value.
  // Step 2: If Type(C) is not Object, throw a TypeError exception.
  if (!thisVal.isObject()) {
    const char* msg = mode == ResolveMode ? "Receiver of Promise.resolve call"
                                          : "Receiver of Promise.reject call";
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, msg);
    return nullptr;
  }
  RootedObject C(cx, &thisVal.toObject());

  // Promise.resolve, step 3: Return ? PromiseResolve(C, x).
  // PromiseResolve, step 1: Assert: Type(C) is Object (implicit).
  // PromiseResolve, step 2: If IsPromise(x) is true, then
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
    if (isPromise) {
      // Step 2.a: Let xConstructor be ? Get(x, "constructor").
      RootedValue ctorVal(cx);
      if (!GetProperty(cx, xObj, xObj, cx->names().constructor, &ctorVal)) {
        return nullptr;
      }

      // Step 2.b: If SameValue(xConstructor, C) is true, return x.
      if (ctorVal == thisVal) {
        return xObj;
      }
    }
  }

  // The step numbers below happen to be the same for PromiseResolve and
  // Promise.reject.
  // Step 3: Let promiseCapability be ? NewPromiseCapability(C).
  Rooted<PromiseCapability> capability(cx);
  if (!NewPromiseCapability(cx, C, &capability, true)) {
    return nullptr;
  }

  HandleObject promise = capability.promise();
  if (mode == ResolveMode) {
    // PromiseResolve, step 4:
    //      Perform ? Call(promiseCapability.[[Resolve]], undefined, « x »).
    if (!RunFulfillFunction(cx, capability.resolve(), argVal, promise)) {
      return nullptr;
    }
  } else {
    // Promise.reject, step 4:
    //      Perform ? Call(promiseCapability.[[Reject]], undefined, « r »).
    if (!RunRejectFunction(cx, capability.reject(), argVal, promise, nullptr,
                           UnhandledRejectionBehavior::Report)) {
      return nullptr;
    }
  }

  // Step 5: Return promiseCapability.[[Promise]].
  return promise;
}

[[nodiscard]] JSObject* js::PromiseResolve(JSContext* cx,
                                           HandleObject constructor,
                                           HandleValue value) {
  RootedValue C(cx, ObjectValue(*constructor));
  return CommonStaticResolveRejectImpl(cx, C, value, ResolveMode);
}

/**
 * ES2016, 25.4.4.4, Promise.reject.
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
 * Unforgeable version of ES2016, 25.4.4.4, Promise.reject.
 */
/* static */
PromiseObject* PromiseObject::unforgeableReject(JSContext* cx,
                                                HandleValue value) {
  cx->check(value);

  Rooted<PromiseObject*> promise(
      cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
  if (!promise) {
    return nullptr;
  }

  MOZ_ASSERT(promise->state() == JS::PromiseState::Pending);
  MOZ_ASSERT(
      PromiseHasAnyFlag(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS));

  if (!RejectPromiseInternal(cx, promise, value)) {
    return nullptr;
  }

  return promise;
}

/**
 * ES2016, 25.4.4.5, Promise.resolve.
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
 * Unforgeable version of ES2016, 25.4.4.5, Promise.resolve.
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
 * Unforgeable version of ES2016, 25.4.4.5, Promise.resolve(value), where
 * value is guaranteed not to be a promise.
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

  Rooted<PromiseObject*> promise(
      cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
  if (!promise) {
    return nullptr;
  }

  MOZ_ASSERT(promise->state() == JS::PromiseState::Pending);
  MOZ_ASSERT(
      PromiseHasAnyFlag(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS));

  if (!ResolvePromiseInternal(cx, promise, value)) {
    return nullptr;
  }

  return promise;
}

/**
 * ES2016, 25.4.4.6 get Promise [ @@species ]
 */
bool js::Promise_static_species(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: Return the this value.
  args.rval().set(args.thisv());
  return true;
}

// ES2016, 25.4.5.1, implemented in Promise.js.

enum class IncumbentGlobalObject {
  // Do not use the incumbent global, this is a special case used by the
  // debugger.
  No,

  // Use incumbent global, this is the normal operation.
  Yes
};

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
                    onFulfilled.toInt32() < PromiseHandlerLimit);

  // Ensure the onRejected handler has the expected type.
  MOZ_ASSERT(onRejected.isInt32() || onRejected.isObjectOrNull());
  MOZ_ASSERT_IF(onRejected.isObject(), IsCallable(onRejected));
  MOZ_ASSERT_IF(
      onRejected.isInt32(),
      0 <= onRejected.toInt32() && onRejected.toInt32() < PromiseHandlerLimit);

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

  reaction->setFixedSlot(ReactionRecordSlot_Promise,
                         ObjectOrNullValue(resultCapability.promise()));
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

static bool PromiseThenNewPromiseCapability(
    JSContext* cx, HandleObject promiseObj,
    CreateDependentPromise createDependent,
    MutableHandle<PromiseCapability> resultCapability) {
  // Step 3.
  RootedObject C(cx, SpeciesConstructor(cx, promiseObj, JSProto_Promise,
                                        IsPromiseSpecies));
  if (!C) {
    return false;
  }

  if (createDependent == CreateDependentPromise::Always ||
      !IsNativeFunction(C, PromiseConstructor)) {
    // Step 4.
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
  }

  return true;
}

// ES2016, 25.4.5.3., steps 3-5.
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

  // Steps 3-4.
  Rooted<PromiseObject*> newPromise(
      cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
  if (!newPromise) {
    return nullptr;
  }
  newPromise->copyUserInteractionFlagsFrom(*unwrappedPromise);

  Rooted<PromiseCapability> resultCapability(cx);
  resultCapability.promise().set(newPromise);

  // Step 5.
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

[[nodiscard]] static bool OriginalPromiseThenWithoutSettleHandlers(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseObject*> promiseToResolve) {
  cx->check(promise);

  // Steps 3-4.
  Rooted<PromiseCapability> resultCapability(cx);
  if (!PromiseThenNewPromiseCapability(
          cx, promise, CreateDependentPromise::SkipIfCtorUnobservable,
          &resultCapability)) {
    return false;
  }

  // Step 5.
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

  RootedValue onFulfilled(cx, onFulfilled_
                                  ? ObjectValue(*onFulfilled_)
                                  : Int32Value(PromiseHandlerIdentity));

  RootedValue onRejected(cx, onRejected_ ? ObjectValue(*onRejected_)
                                         : Int32Value(PromiseHandlerThrower));

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

// ES2016, 25.4.5.3., steps 3-5.
static bool OriginalPromiseThenBuiltin(JSContext* cx, HandleValue promiseVal,
                                       HandleValue onFulfilled,
                                       HandleValue onRejected,
                                       MutableHandleValue rval, bool rvalUsed) {
  cx->check(promiseVal, onFulfilled, onRejected);
  MOZ_ASSERT(CanCallOriginalPromiseThenBuiltin(cx, promiseVal));

  Rooted<PromiseObject*> promise(cx,
                                 &promiseVal.toObject().as<PromiseObject>());

  // Steps 3-4.
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

  // Step 5.
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

// ES 2018 draft 14.6.11 and 14.7.14 step 1.
[[nodiscard]] PromiseObject* js::CreatePromiseObjectForAsync(JSContext* cx) {
  // Step 1.
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

[[nodiscard]] static PromiseObject* CreatePromiseObjectForAsyncGenerator(
    JSContext* cx) {
  PromiseObject* promise = CreatePromiseObjectWithoutResolutionFunctions(cx);
  if (!promise) {
    return nullptr;
  }

  AddPromiseFlags(*promise, PROMISE_FLAG_ASYNC);
  return promise;
}

// ES2019 draft rev 7428c89bef626548084cd4e697a19ece7168f24c
// 25.7.5.1 AsyncFunctionStart, steps 3.f-g.
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

  return RejectPromiseInternal(cx, resultPromise, reason);
}

// ES2019 draft rev 7428c89bef626548084cd4e697a19ece7168f24c
// 25.7.5.1 AsyncFunctionStart, steps 3.d-e, 3.g.
[[nodiscard]] bool js::AsyncFunctionReturned(
    JSContext* cx, Handle<PromiseObject*> resultPromise, HandleValue value) {
  return ResolvePromiseInternal(cx, resultPromise, value);
}

// https://tc39.github.io/ecma262/#await
//
// Helper function that performs 6.2.3.1 Await(promise) steps 2 and 9.
// The same steps are also used in a few other places in the spec.
template <typename T>
[[nodiscard]] static bool InternalAwait(JSContext* cx, HandleValue value,
                                        HandleObject resultPromise,
                                        PromiseHandler onFulfilled,
                                        PromiseHandler onRejected,
                                        T extraStep) {
  // Step 2: Let promise be ? PromiseResolve(%Promise%, « value »).
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

  // Steps 3-8 of the spec create onFulfilled and onRejected functions.

  // Step 9: Perform ! PerformPromiseThen(promise, onFulfilled, onRejected).
  RootedValue onFulfilledValue(cx, Int32Value(onFulfilled));
  RootedValue onRejectedValue(cx, Int32Value(onRejected));
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

// https://tc39.github.io/ecma262/#await
//
// 6.2.3.1 Await(promise) steps 2-10 when the running execution context is
// evaluating an `await` expression in an async function.
[[nodiscard]] JSObject* js::AsyncFunctionAwait(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> genObj,
    HandleValue value) {
  auto extra = [&](Handle<PromiseReactionRecord*> reaction) {
    reaction->setIsAsyncFunction(genObj);
  };
  if (!InternalAwait(cx, value, nullptr,
                     PromiseHandlerAsyncFunctionAwaitedFulfilled,
                     PromiseHandlerAsyncFunctionAwaitedRejected, extra)) {
    return nullptr;
  }
  return genObj->promise();
}

// 6.2.3.1 Await(promise) steps 2-10 when the running execution context is
// evaluating an `await` expression in an async generator.
[[nodiscard]] bool js::AsyncGeneratorAwait(
    JSContext* cx, Handle<AsyncGeneratorObject*> asyncGenObj,
    HandleValue value) {
  auto extra = [&](Handle<PromiseReactionRecord*> reaction) {
    reaction->setIsAsyncGenerator(asyncGenObj);
  };
  return InternalAwait(cx, value, nullptr,
                       PromiseHandlerAsyncGeneratorAwaitedFulfilled,
                       PromiseHandlerAsyncGeneratorAwaitedRejected, extra);
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
      done ? PromiseHandlerAsyncFromSyncIteratorValueUnwrapDone
           : PromiseHandlerAsyncFromSyncIteratorValueUnwrapNotDone;
  PromiseHandler onRejected = PromiseHandlerThrower;

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

enum class ResumeNextKind { Enqueue, Reject, Resolve };

[[nodiscard]] static bool AsyncGeneratorResumeNext(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, ResumeNextKind kind,
    HandleValue valueOrException = UndefinedHandleValue, bool done = false);

// 25.5.3.3 AsyncGeneratorResolve ( generator, value, done )
[[nodiscard]] bool js::AsyncGeneratorResolve(
    JSContext* cx, Handle<AsyncGeneratorObject*> asyncGenObj, HandleValue value,
    bool done) {
  return AsyncGeneratorResumeNext(cx, asyncGenObj, ResumeNextKind::Resolve,
                                  value, done);
}

// 25.5.3.4 AsyncGeneratorReject ( generator, exception )
[[nodiscard]] bool js::AsyncGeneratorReject(
    JSContext* cx, Handle<AsyncGeneratorObject*> asyncGenObj,
    HandleValue exception) {
  return AsyncGeneratorResumeNext(cx, asyncGenObj, ResumeNextKind::Reject,
                                  exception);
}

// Unified implementation of:
// 25.5.3.3 AsyncGeneratorResolve ( generator, value, done )
// 25.5.3.4 AsyncGeneratorReject ( generator, exception )
// 25.5.3.5 AsyncGeneratorResumeNext ( generator )
[[nodiscard]] static bool AsyncGeneratorResumeNext(
    JSContext* cx, Handle<AsyncGeneratorObject*> generator, ResumeNextKind kind,
    HandleValue valueOrException_ /* = UndefinedHandleValue */,
    bool done /* = false */) {
  RootedValue valueOrException(cx, valueOrException_);

  // Many paths through the algorithm end in recursive tail-calls.
  // We implement these with a loop.
  while (true) {
    switch (kind) {
      case ResumeNextKind::Enqueue:
        // No further action required.
        break;
      case ResumeNextKind::Reject: {
        // 25.5.3.4 AsyncGeneratorReject ( generator, exception )
        HandleValue exception = valueOrException;

        // Step 1: Assert: generator is an AsyncGenerator instance (implicit).
        // Step 2: Let queue be generator.[[AsyncGeneratorQueue]].
        // Step 3: Assert: queue is not an empty List.
        MOZ_ASSERT(!generator->isQueueEmpty());

        // Step 4: Remove the first element from queue and let next be the value
        //         of that element.
        AsyncGeneratorRequest* request =
            AsyncGeneratorObject::dequeueRequest(cx, generator);
        if (!request) {
          return false;
        }

        // Step 5: Let promiseCapability be next.[[Capability]].
        Rooted<PromiseObject*> resultPromise(cx, request->promise());

        generator->cacheRequest(request);

        // Step 6: Perform ! Call(promiseCapability.[[Reject]], undefined,
        //                        « exception »).
        if (!RejectPromiseInternal(cx, resultPromise, exception)) {
          return false;
        }

        // Step 7: Perform ! AsyncGeneratorResumeNext(generator).
        // Step 8: Return undefined.
        break;
      }
      case ResumeNextKind::Resolve: {
        // 25.5.3.3 AsyncGeneratorResolve ( generator, value, done )
        HandleValue value = valueOrException;

        // Step 1: Assert: generator is an AsyncGenerator instance (implicit).
        // Step 2: Let queue be generator.[[AsyncGeneratorQueue]].
        // Step 3: Assert: queue is not an empty List.
        MOZ_ASSERT(!generator->isQueueEmpty());

        // Step 4: Remove the first element from queue and let next be the value
        //         of that element.
        AsyncGeneratorRequest* request =
            AsyncGeneratorObject::dequeueRequest(cx, generator);
        if (!request) {
          return false;
        }

        // Step 5: Let promiseCapability be next.[[Capability]].
        Rooted<PromiseObject*> resultPromise(cx, request->promise());

        generator->cacheRequest(request);

        // Step 6: Let iteratorResult be ! CreateIterResultObject(value, done).
        JSObject* resultObj = CreateIterResultObject(cx, value, done);
        if (!resultObj) {
          return false;
        }

        RootedValue resultValue(cx, ObjectValue(*resultObj));

        // Step 7: Perform ! Call(promiseCapability.[[Resolve]], undefined,
        //                        « iteratorResult »).
        if (!ResolvePromiseInternal(cx, resultPromise, resultValue)) {
          return false;
        }

        // Step 8: Perform ! AsyncGeneratorResumeNext(generator).
        // Step 9: Return undefined.
        break;
      }
    }

    // 25.5.3.5 AsyncGeneratorResumeNext ( generator )
    // Step 1: Assert: generator is an AsyncGenerator instance (implicit).
    // Step 2: Let state be generator.[[AsyncGeneratorState]] (implicit).
    // Step 3: Assert: state is not "executing".
    MOZ_ASSERT(!generator->isExecuting());
    MOZ_ASSERT(!generator->isAwaitingYieldReturn());

    // Step 4: If state is "awaiting-return", return undefined.
    if (generator->isAwaitingReturn()) {
      return true;
    }

    // Step 5: Let queue be generator.[[AsyncGeneratorQueue]].
    // Step 6: If queue is an empty List, return undefined.
    if (generator->isQueueEmpty()) {
      return true;
    }

    // Step 7: Let next be the value of the first element of queue.
    // Step 8: Assert: next is an AsyncGeneratorRequest record.
    Rooted<AsyncGeneratorRequest*> request(
        cx, AsyncGeneratorObject::peekRequest(generator));
    if (!request) {
      return false;
    }

    // Step 9: Let completion be next.[[Completion]].
    CompletionKind completionKind = request->completionKind();

    // Step 10: If completion is an abrupt completion, then
    if (completionKind != CompletionKind::Normal) {
      // Step 10.a: If state is "suspendedStart", then
      if (generator->isSuspendedStart()) {
        // Step 10.a.i: Set generator.[[AsyncGeneratorState]] to "completed".
        // Step 10.a.ii: Set state to "completed".
        generator->setCompleted();
      }

      // Step 10.b: If state is "completed", then
      if (generator->isCompleted()) {
        RootedValue value(cx, request->completionValue());

        // Step 10.b.i: If completion.[[Type]] is return, then
        if (completionKind == CompletionKind::Return) {
          // Step 10.b.i.1: Set generator.[[AsyncGeneratorState]] to
          //                "awaiting-return".
          generator->setAwaitingReturn();

          // (reordered)
          // Step 10.b.i.3: Let stepsFulfilled be the algorithm steps defined in
          //                AsyncGeneratorResumeNext Return Processor Fulfilled
          //                Functions.
          // Step 10.b.i.4: Let onFulfilled be CreateBuiltinFunction(
          //                stepsFulfilled, « [[Generator]] »).
          // Step 10.b.i.5: Set onFulfilled.[[Generator]] to generator.
          // Step 10.b.i.6: Let stepsRejected be the algorithm steps defined in
          //                AsyncGeneratorResumeNext Return Processor Rejected
          //                Functions.
          // Step 10.b.i.7: Let onRejected be CreateBuiltinFunction(
          //                stepsRejected, « [[Generator]] »).
          // Step 10.b.i.8: Set onRejected.[[Generator]] to generator.
          //
          const PromiseHandler onFulfilled =
              PromiseHandlerAsyncGeneratorResumeNextReturnFulfilled;
          const PromiseHandler onRejected =
              PromiseHandlerAsyncGeneratorResumeNextReturnRejected;

          // These steps are nearly identical to some steps in Await;
          // InternalAwait() implements the idiom.
          //
          // Step 10.b.i.2: Let promise be ? PromiseResolve(%Promise%,
          //                « _completion_.[[Value]] »).
          // Step 10.b.i.9: Perform ! PerformPromiseThen(promise, onFulfilled,
          //                                             onRejected).
          // Step 10.b.i.10: Return undefined.
          auto extra = [&](Handle<PromiseReactionRecord*> reaction) {
            reaction->setIsAsyncGenerator(generator);
          };
          return InternalAwait(cx, value, nullptr, onFulfilled, onRejected,
                               extra);
        }

        // Step 10.b.ii: Else,

        // Step 10.b.ii.1: Assert: completion.[[Type]] is throw.
        MOZ_ASSERT(completionKind == CompletionKind::Throw);

        // Step 10.b.ii.2: Perform ! AsyncGeneratorReject(generator,
        //                 completion.[[Value]]).
        // Step 10.b.ii.3: Return undefined.
        kind = ResumeNextKind::Reject;
        valueOrException.set(value);
        continue;
      }
    } else if (generator->isCompleted()) {
      // Step 11: Else if state is "completed", return
      //          ! AsyncGeneratorResolve(generator, undefined, true).
      kind = ResumeNextKind::Resolve;
      valueOrException.setUndefined();
      done = true;
      continue;
    }

    // Step 12: Assert: state is either "suspendedStart" or "suspendedYield".
    MOZ_ASSERT(generator->isSuspendedStart() || generator->isSuspendedYield());

    RootedValue argument(cx, request->completionValue());

    if (completionKind == CompletionKind::Return) {
      // 25.5.3.7 AsyncGeneratorYield steps 8.b-e.
      // Since we don't have the place that handles return from yield
      // inside the generator, handle the case here, with extra state
      // State_AwaitingYieldReturn.
      generator->setAwaitingYieldReturn();

      const PromiseHandler onFulfilled =
          PromiseHandlerAsyncGeneratorYieldReturnAwaitedFulfilled;
      const PromiseHandler onRejected =
          PromiseHandlerAsyncGeneratorYieldReturnAwaitedRejected;

      auto extra = [&](Handle<PromiseReactionRecord*> reaction) {
        reaction->setIsAsyncGenerator(generator);
      };
      return InternalAwait(cx, argument, nullptr, onFulfilled, onRejected,
                           extra);
    }

    // Step 16 (reordered): Set generator.[[AsyncGeneratorState]] to
    //                      "executing".
    generator->setExecuting();

    // Steps 13-15, 17-21.
    return AsyncGeneratorResume(cx, generator, completionKind, argument);
  }
}

// 25.5.3.6 AsyncGeneratorEnqueue ( generator, completion )
[[nodiscard]] bool js::AsyncGeneratorEnqueue(JSContext* cx,
                                             HandleValue asyncGenVal,
                                             CompletionKind completionKind,
                                             HandleValue completionValue,
                                             MutableHandleValue result) {
  // Step 1 (implicit).

  // Step 3.
  if (!asyncGenVal.isObject() ||
      !asyncGenVal.toObject().canUnwrapAs<AsyncGeneratorObject>()) {
    // Step 2.
    Rooted<PromiseObject*> resultPromise(
        cx, CreatePromiseObjectForAsyncGenerator(cx));
    if (!resultPromise) {
      return false;
    }

    // Step 3.a.
    RootedValue badGeneratorError(cx);
    if (!GetTypeError(cx, JSMSG_NOT_AN_ASYNC_GENERATOR, &badGeneratorError)) {
      return false;
    }

    // Step 3.b.
    if (!RejectPromiseInternal(cx, resultPromise, badGeneratorError)) {
      return false;
    }

    // Step 3.c.
    result.setObject(*resultPromise);
    return true;
  }

  Rooted<AsyncGeneratorObject*> asyncGenObj(
      cx, &asyncGenVal.toObject().unwrapAs<AsyncGeneratorObject>());

  bool wrapResult = false;
  {
    // The |resultPromise| must be same-compartment with |asyncGenObj|, because
    // it is stored in AsyncGeneratorRequest, which in turn is stored in a
    // reserved slot of |asyncGenObj|.
    // So we first enter the realm of |asyncGenObj|, then create the result
    // promise and resume the generator, and finally wrap the result promise to
    // match the original compartment.

    mozilla::Maybe<AutoRealm> ar;
    RootedValue completionVal(cx, completionValue);
    if (asyncGenObj->compartment() != cx->compartment()) {
      ar.emplace(cx, asyncGenObj);
      wrapResult = true;

      if (!cx->compartment()->wrap(cx, &completionVal)) {
        return false;
      }
    }

    // Step 2.
    Rooted<PromiseObject*> resultPromise(
        cx, CreatePromiseObjectForAsyncGenerator(cx));
    if (!resultPromise) {
      return false;
    }

    // Step 5 (reordered).
    Rooted<AsyncGeneratorRequest*> request(
        cx, AsyncGeneratorObject::createRequest(cx, asyncGenObj, completionKind,
                                                completionVal, resultPromise));
    if (!request) {
      return false;
    }

    // Steps 4, 6.
    if (!AsyncGeneratorObject::enqueueRequest(cx, asyncGenObj, request)) {
      return false;
    }

    // Step 7.
    if (!asyncGenObj->isExecuting() && !asyncGenObj->isAwaitingYieldReturn()) {
      // Step 8.
      if (!AsyncGeneratorResumeNext(cx, asyncGenObj, ResumeNextKind::Enqueue)) {
        return false;
      }
    }

    // Step 9.
    result.setObject(*resultPromise);
  }

  return !wrapResult || cx->compartment()->wrap(cx, result);
}

static bool Promise_catch_impl(JSContext* cx, unsigned argc, Value* vp,
                               bool rvalUsed) {
  CallArgs args = CallArgsFromVp(argc, vp);

  HandleValue thisVal = args.thisv();
  HandleValue onFulfilled = UndefinedHandleValue;
  HandleValue onRejected = args.get(0);

  // Fast path when the default Promise state is intact.
  if (CanCallOriginalPromiseThenBuiltin(cx, thisVal)) {
    return OriginalPromiseThenBuiltin(cx, thisVal, onFulfilled, onRejected,
                                      args.rval(), rvalUsed);
  }

  // Step 1.
  RootedValue thenVal(cx);
  if (!GetProperty(cx, thisVal, cx->names().then, &thenVal)) {
    return false;
  }

  if (IsNativeFunction(thenVal, &Promise_then) &&
      thenVal.toObject().nonCCWRealm() == cx->realm()) {
    return Promise_then_impl(cx, thisVal, onFulfilled, onRejected, args.rval(),
                             rvalUsed);
  }

  return Call(cx, thenVal, thisVal, UndefinedHandleValue, onRejected,
              args.rval());
}

static MOZ_ALWAYS_INLINE bool IsPromiseThenOrCatchRetValImplicitlyUsed(
    JSContext* cx) {
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

// ES2016, 25.4.5.3.
static bool Promise_catch_noRetVal(JSContext* cx, unsigned argc, Value* vp) {
  return Promise_catch_impl(cx, argc, vp,
                            IsPromiseThenOrCatchRetValImplicitlyUsed(cx));
}

// ES2016, 25.4.5.3.
static bool Promise_catch(JSContext* cx, unsigned argc, Value* vp) {
  return Promise_catch_impl(cx, argc, vp, true);
}

static bool Promise_then_impl(JSContext* cx, HandleValue promiseVal,
                              HandleValue onFulfilled, HandleValue onRejected,
                              MutableHandleValue rval, bool rvalUsed) {
  // Step 1 (implicit).
  // Step 2.
  if (!promiseVal.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED,
                              "Receiver of Promise.prototype.then call");
    return false;
  }

  // Fast path when the default Promise state is intact.
  if (CanCallOriginalPromiseThenBuiltin(cx, promiseVal)) {
    return OriginalPromiseThenBuiltin(cx, promiseVal, onFulfilled, onRejected,
                                      rval, rvalUsed);
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

  // Steps 3-4.
  CreateDependentPromise createDependent =
      rvalUsed ? CreateDependentPromise::Always
               : CreateDependentPromise::SkipIfCtorUnobservable;
  Rooted<PromiseCapability> resultCapability(cx);
  if (!PromiseThenNewPromiseCapability(cx, promiseObj, createDependent,
                                       &resultCapability)) {
    return false;
  }

  // Step 5.
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

// ES2016, 25.4.5.3.
bool Promise_then_noRetVal(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return Promise_then_impl(cx, args.thisv(), args.get(0), args.get(1),
                           args.rval(),
                           IsPromiseThenOrCatchRetValImplicitlyUsed(cx));
}

// ES2016, 25.4.5.3.
bool js::Promise_then(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return Promise_then_impl(cx, args.thisv(), args.get(0), args.get(1),
                           args.rval(), true);
}

// ES2016, 25.4.5.3.1.
[[nodiscard]] static bool PerformPromiseThen(
    JSContext* cx, Handle<PromiseObject*> promise, HandleValue onFulfilled_,
    HandleValue onRejected_, Handle<PromiseCapability> resultCapability) {
  // Step 1 (implicit).
  // Step 2 (implicit).

  // Step 3.
  RootedValue onFulfilled(cx, onFulfilled_);
  if (!IsCallable(onFulfilled)) {
    onFulfilled = Int32Value(PromiseHandlerIdentity);
  }

  // Step 4.
  RootedValue onRejected(cx, onRejected_);
  if (!IsCallable(onRejected)) {
    onRejected = Int32Value(PromiseHandlerThrower);
  }

  // Step 7.
  Rooted<PromiseReactionRecord*> reaction(
      cx, NewReactionRecord(cx, resultCapability, onFulfilled, onRejected,
                            IncumbentGlobalObject::Yes));
  if (!reaction) {
    return false;
  }

  return PerformPromiseThenWithReaction(cx, promise, reaction);
}

[[nodiscard]] static bool PerformPromiseThenWithoutSettleHandlers(
    JSContext* cx, Handle<PromiseObject*> promise,
    Handle<PromiseObject*> promiseToResolve,
    Handle<PromiseCapability> resultCapability) {
  // Step 1 (implicit).
  // Step 2 (implicit).

  // Step 3.
  HandleValue onFulfilled = NullHandleValue;

  // Step 4.
  HandleValue onRejected = NullHandleValue;

  // Step 7.
  Rooted<PromiseReactionRecord*> reaction(
      cx, NewReactionRecord(cx, resultCapability, onFulfilled, onRejected,
                            IncumbentGlobalObject::Yes));
  if (!reaction) {
    return false;
  }

  reaction->setIsDefaultResolvingHandler(promiseToResolve);

  return PerformPromiseThenWithReaction(cx, promise, reaction);
}

// https://tc39.github.io/ecma262/#sec-performpromisethen
// 25.6.5.4.1 PerformPromiseThen steps 8-11.
[[nodiscard]] static bool PerformPromiseThenWithReaction(
    JSContext* cx, Handle<PromiseObject*> unwrappedPromise,
    Handle<PromiseReactionRecord*> reaction) {
  // Step 8: If promise.[[PromiseState]] is "pending", then
  JS::PromiseState state = unwrappedPromise->state();
  int32_t flags = unwrappedPromise->flags();
  if (state == JS::PromiseState::Pending) {
    // Step 8.a: Append fulfillReaction as the last element of the List that is
    //           promise.[[PromiseFulfillReactions]].
    // Step 8.b: Append rejectReaction as the last element of the List that is
    //           promise.[[PromiseRejectReactions]].
    //
    // Instead of creating separate reaction records for fulfillment and
    // rejection, we create a combined record. All places we use the record
    // can handle that.
    if (!AddPromiseReaction(cx, unwrappedPromise, reaction)) {
      return false;
    }
  }

  // Steps 9-10. In the spec, step 9 runs if the promise is fulfilled, step 10
  // if it is rejected. This implementation unifies the two paths.
  else {
    // Step 10.a.
    MOZ_ASSERT_IF(state != JS::PromiseState::Fulfilled,
                  state == JS::PromiseState::Rejected);

    // Step 9.a: Let value be promise.[[PromiseResult]].
    // Step 10.b: Let reason be promise.[[PromiseResult]].
    RootedValue valueOrReason(cx, unwrappedPromise->valueOrReason());

    // We might be operating on a promise from another compartment. In that
    // case, we need to wrap the result/reason value before using it.
    if (!cx->compartment()->wrap(cx, &valueOrReason)) {
      return false;
    }

    // Step 10.c: If promise.[[PromiseIsHandled]] is false,
    //            perform HostPromiseRejectionTracker(promise, "handle").
    if (state == JS::PromiseState::Rejected &&
        !(flags & PROMISE_FLAG_HANDLED)) {
      cx->runtime()->removeUnhandledRejectedPromise(cx, unwrappedPromise);
    }

    // Step 9.b: Perform EnqueueJob("PromiseJobs", PromiseReactionJob,
    //                              « fulfillReaction, value »).
    // Step 10.d: Perform EnqueueJob("PromiseJobs", PromiseReactionJob,
    //                               « rejectReaction, reason »).
    if (!EnqueuePromiseReactionJob(cx, reaction, valueOrReason, state)) {
      return false;
    }
  }

  // Step 11: Set promise.[[PromiseIsHandled]] to true.
  unwrappedPromise->setHandled();

  return true;
}

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

  // 25.4.5.3.1 steps 7.a,b.
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
    HandleNativeObject reactions = reactionsObj.as<NativeObject>();
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

/* static */
bool PromiseObject::resolve(JSContext* cx, Handle<PromiseObject*> promise,
                            HandleValue resolutionValue) {
  MOZ_ASSERT(!PromiseHasAnyFlag(*promise, PROMISE_FLAG_ASYNC));
  if (promise->state() != JS::PromiseState::Pending) {
    return true;
  }

  if (PromiseHasAnyFlag(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS)) {
    return ResolvePromiseInternal(cx, promise, resolutionValue);
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

/* static */
bool PromiseObject::reject(JSContext* cx, Handle<PromiseObject*> promise,
                           HandleValue rejectionValue) {
  MOZ_ASSERT(!PromiseHasAnyFlag(*promise, PROMISE_FLAG_ASYNC));
  if (promise->state() != JS::PromiseState::Pending) {
    return true;
  }

  if (PromiseHasAnyFlag(*promise, PROMISE_FLAG_DEFAULT_RESOLVING_FUNCTIONS)) {
    return ResolvePromise(cx, promise, rejectionValue,
                          JS::PromiseState::Rejected);
  }

  RootedValue funVal(cx, promise->getFixedSlot(PromiseSlot_RejectFunction));
  MOZ_ASSERT(IsCallable(funVal));

  RootedValue dummy(cx);
  return Call(cx, funVal, UndefinedHandleValue, rejectionValue, &dummy);
}

/* static */
void PromiseObject::onSettled(JSContext* cx, Handle<PromiseObject*> promise,
                              HandleSavedFrame unwrappedRejectionStack) {
  PromiseDebugInfo::setResolutionInfo(cx, promise, unwrappedRejectionStack);

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
