/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS Atomics pseudo-module.
 *
 * See chapter 24.4 "The Atomics Object" and chapter 27 "Memory Model" in
 * ECMAScript 2021 for the full specification.
 */

#include "builtin/AtomicsObject.h"

#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"

#include "jsnum.h"

#include "builtin/Promise.h"
#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "js/Class.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/Result.h"
#include "js/WaitCallbacks.h"
#include "vm/GlobalObject.h"
#include "vm/HelperThreads.h"                 // AutoLockHelperThreadState
#include "vm/OffThreadPromiseRuntimeState.h"  // OffthreadPromiseTask
#include "vm/TypedArrayObject.h"

#include "vm/Compartment-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

static bool ReportBadArrayType(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_ATOMICS_BAD_ARRAY);
  return false;
}

static bool ReportDetachedArrayBuffer(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TYPED_ARRAY_DETACHED);
  return false;
}

static bool ReportResizedArrayBuffer(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TYPED_ARRAY_RESIZED_BOUNDS);
  return false;
}

static bool ReportOutOfRange(JSContext* cx) {
  // Use JSMSG_BAD_INDEX here, it is what ToIndex uses for some cases that it
  // reports directly.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
  return false;
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// Plus: https://github.com/tc39/ecma262/pull/1908
// 24.4.1.1 ValidateIntegerTypedArray ( typedArray [ , waitable ] )
static bool ValidateIntegerTypedArray(
    JSContext* cx, HandleValue typedArray, bool waitable,
    MutableHandle<TypedArrayObject*> unwrappedTypedArray) {
  // Step 1 (implicit).

  // Step 2.
  auto* unwrapped = UnwrapAndTypeCheckValue<TypedArrayObject>(
      cx, typedArray, [cx]() { ReportBadArrayType(cx); });
  if (!unwrapped) {
    return false;
  }

  if (unwrapped->hasDetachedBuffer()) {
    return ReportDetachedArrayBuffer(cx);
  }

  // Steps 3-6.
  if (waitable) {
    switch (unwrapped->type()) {
      case Scalar::Int32:
      case Scalar::BigInt64:
        break;
      default:
        return ReportBadArrayType(cx);
    }
  } else {
    switch (unwrapped->type()) {
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Int16:
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
        break;
      default:
        return ReportBadArrayType(cx);
    }
  }

  // Steps 7-9 (modified to return the TypedArray).
  unwrappedTypedArray.set(unwrapped);
  return true;
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.1.2 ValidateAtomicAccess ( typedArray, requestIndex )
static bool ValidateAtomicAccess(JSContext* cx,
                                 Handle<TypedArrayObject*> typedArray,
                                 HandleValue requestIndex, size_t* index) {
  MOZ_ASSERT(!typedArray->hasDetachedBuffer());

  // Steps 1-2.
  mozilla::Maybe<size_t> length = typedArray->length();
  if (!length) {
    // ValidateIntegerTypedArray doesn't check for out-of-bounds in our
    // implementation, so we have to handle this case here.
    return ReportResizedArrayBuffer(cx);
  }

  // Steps 3-4.
  uint64_t accessIndex;
  if (!ToIndex(cx, requestIndex, &accessIndex)) {
    return false;
  }

  // Step 5.
  if (accessIndex >= *length) {
    return ReportOutOfRange(cx);
  }

  // Steps 6-9.
  *index = size_t(accessIndex);
  return true;
}

template <typename T>
struct ArrayOps {
  using Type = T;

  static JS::Result<T> convertValue(JSContext* cx, HandleValue v) {
    int32_t n;
    if (!ToInt32(cx, v, &n)) {
      return cx->alreadyReportedError();
    }
    return static_cast<T>(n);
  }

  static JS::Result<T> convertValue(JSContext* cx, HandleValue v,
                                    MutableHandleValue result) {
    double d;
    if (!ToInteger(cx, v, &d)) {
      return cx->alreadyReportedError();
    }
    result.setNumber(d);
    return static_cast<T>(JS::ToInt32(d));
  }

  static JS::Result<> storeResult(JSContext* cx, T v,
                                  MutableHandleValue result) {
    result.setInt32(v);
    return Ok();
  }
};

template <>
JS::Result<> ArrayOps<uint32_t>::storeResult(JSContext* cx, uint32_t v,
                                             MutableHandleValue result) {
  // Always double typed so that the JITs can assume the types are stable.
  result.setDouble(v);
  return Ok();
}

template <>
struct ArrayOps<int64_t> {
  using Type = int64_t;

  static JS::Result<int64_t> convertValue(JSContext* cx, HandleValue v) {
    BigInt* bi = ToBigInt(cx, v);
    if (!bi) {
      return cx->alreadyReportedError();
    }
    return BigInt::toInt64(bi);
  }

  static JS::Result<int64_t> convertValue(JSContext* cx, HandleValue v,
                                          MutableHandleValue result) {
    BigInt* bi = ToBigInt(cx, v);
    if (!bi) {
      return cx->alreadyReportedError();
    }
    result.setBigInt(bi);
    return BigInt::toInt64(bi);
  }

  static JS::Result<> storeResult(JSContext* cx, int64_t v,
                                  MutableHandleValue result) {
    BigInt* bi = BigInt::createFromInt64(cx, v);
    if (!bi) {
      return cx->alreadyReportedError();
    }
    result.setBigInt(bi);
    return Ok();
  }
};

template <>
struct ArrayOps<uint64_t> {
  using Type = uint64_t;

  static JS::Result<uint64_t> convertValue(JSContext* cx, HandleValue v) {
    BigInt* bi = ToBigInt(cx, v);
    if (!bi) {
      return cx->alreadyReportedError();
    }
    return BigInt::toUint64(bi);
  }

  static JS::Result<uint64_t> convertValue(JSContext* cx, HandleValue v,
                                           MutableHandleValue result) {
    BigInt* bi = ToBigInt(cx, v);
    if (!bi) {
      return cx->alreadyReportedError();
    }
    result.setBigInt(bi);
    return BigInt::toUint64(bi);
  }

  static JS::Result<> storeResult(JSContext* cx, uint64_t v,
                                  MutableHandleValue result) {
    BigInt* bi = BigInt::createFromUint64(cx, v);
    if (!bi) {
      return cx->alreadyReportedError();
    }
    result.setBigInt(bi);
    return Ok();
  }
};

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.1.11 AtomicReadModifyWrite ( typedArray, index, value, op ), steps 1-2.
// 24.4.1.12 AtomicLoad ( typedArray, index ), steps 1-2.
// 24.4.4 Atomics.compareExchange ( typedArray, index, ... ), steps 1-2.
// 24.4.9 Atomics.store ( typedArray, index, value ), steps 1-2.
template <typename Op>
bool AtomicAccess(JSContext* cx, HandleValue obj, HandleValue index, Op op) {
  // Step 1.
  Rooted<TypedArrayObject*> unwrappedTypedArray(cx);
  if (!ValidateIntegerTypedArray(cx, obj, false, &unwrappedTypedArray)) {
    return false;
  }

  // Step 2.
  size_t intIndex;
  if (!ValidateAtomicAccess(cx, unwrappedTypedArray, index, &intIndex)) {
    return false;
  }

  switch (unwrappedTypedArray->type()) {
    case Scalar::Int8:
      return op(ArrayOps<int8_t>{}, unwrappedTypedArray, intIndex);
    case Scalar::Uint8:
      return op(ArrayOps<uint8_t>{}, unwrappedTypedArray, intIndex);
    case Scalar::Int16:
      return op(ArrayOps<int16_t>{}, unwrappedTypedArray, intIndex);
    case Scalar::Uint16:
      return op(ArrayOps<uint16_t>{}, unwrappedTypedArray, intIndex);
    case Scalar::Int32:
      return op(ArrayOps<int32_t>{}, unwrappedTypedArray, intIndex);
    case Scalar::Uint32:
      return op(ArrayOps<uint32_t>{}, unwrappedTypedArray, intIndex);
    case Scalar::BigInt64:
      return op(ArrayOps<int64_t>{}, unwrappedTypedArray, intIndex);
    case Scalar::BigUint64:
      return op(ArrayOps<uint64_t>{}, unwrappedTypedArray, intIndex);
    case Scalar::Float16:
    case Scalar::Float32:
    case Scalar::Float64:
    case Scalar::Uint8Clamped:
    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      break;
  }
  MOZ_CRASH("Unsupported TypedArray type");
}

template <typename T>
static SharedMem<T*> TypedArrayData(JSContext* cx, TypedArrayObject* typedArray,
                                    size_t index) {
  // RevalidateAtomicAccess, steps 1-3.
  mozilla::Maybe<size_t> length = typedArray->length();

  // RevalidateAtomicAccess, step 4.
  if (!length) {
    ReportDetachedArrayBuffer(cx);
    return {};
  }

  // RevalidateAtomicAccess, step 5.
  if (index >= *length) {
    ReportOutOfRange(cx);
    return {};
  }

  SharedMem<void*> typedArrayData = typedArray->dataPointerEither();
  return typedArrayData.cast<T*>() + index;
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.4 Atomics.compareExchange ( typedArray, index, expectedValue,
//                                  replacementValue )
static bool atomics_compareExchange(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue typedArray = args.get(0);
  HandleValue index = args.get(1);

  return AtomicAccess(
      cx, typedArray, index,
      [cx, &args](auto ops, Handle<TypedArrayObject*> unwrappedTypedArray,
                  size_t index) {
        using T = typename decltype(ops)::Type;

        HandleValue expectedValue = args.get(2);
        HandleValue replacementValue = args.get(3);

        T oldval;
        JS_TRY_VAR_OR_RETURN_FALSE(cx, oldval,
                                   ops.convertValue(cx, expectedValue));

        T newval;
        JS_TRY_VAR_OR_RETURN_FALSE(cx, newval,
                                   ops.convertValue(cx, replacementValue));

        SharedMem<T*> addr = TypedArrayData<T>(cx, unwrappedTypedArray, index);
        if (!addr) {
          return false;
        }

        oldval =
            jit::AtomicOperations::compareExchangeSeqCst(addr, oldval, newval);

        JS_TRY_OR_RETURN_FALSE(cx, ops.storeResult(cx, oldval, args.rval()));
        return true;
      });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.7 Atomics.load ( typedArray, index )
static bool atomics_load(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue typedArray = args.get(0);
  HandleValue index = args.get(1);

  return AtomicAccess(
      cx, typedArray, index,
      [cx, &args](auto ops, Handle<TypedArrayObject*> unwrappedTypedArray,
                  size_t index) {
        using T = typename decltype(ops)::Type;

        SharedMem<T*> addr = TypedArrayData<T>(cx, unwrappedTypedArray, index);
        if (!addr) {
          return false;
        }

        T v = jit::AtomicOperations::loadSeqCst(addr);

        JS_TRY_OR_RETURN_FALSE(cx, ops.storeResult(cx, v, args.rval()));
        return true;
      });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.9 Atomics.store ( typedArray, index, value )
static bool atomics_store(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue typedArray = args.get(0);
  HandleValue index = args.get(1);

  return AtomicAccess(
      cx, typedArray, index,
      [cx, &args](auto ops, Handle<TypedArrayObject*> unwrappedTypedArray,
                  size_t index) {
        using T = typename decltype(ops)::Type;

        HandleValue value = args.get(2);

        T v;
        JS_TRY_VAR_OR_RETURN_FALSE(cx, v,
                                   ops.convertValue(cx, value, args.rval()));

        SharedMem<T*> addr = TypedArrayData<T>(cx, unwrappedTypedArray, index);
        if (!addr) {
          return false;
        }

        jit::AtomicOperations::storeSeqCst(addr, v);
        return true;
      });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.1.11 AtomicReadModifyWrite ( typedArray, index, value, op )
template <typename AtomicOp>
static bool AtomicReadModifyWrite(JSContext* cx, const CallArgs& args,
                                  AtomicOp op) {
  HandleValue typedArray = args.get(0);
  HandleValue index = args.get(1);

  return AtomicAccess(
      cx, typedArray, index,
      [cx, &args, op](auto ops, Handle<TypedArrayObject*> unwrappedTypedArray,
                      size_t index) {
        using T = typename decltype(ops)::Type;

        HandleValue value = args.get(2);

        T v;
        JS_TRY_VAR_OR_RETURN_FALSE(cx, v, ops.convertValue(cx, value));

        SharedMem<T*> addr = TypedArrayData<T>(cx, unwrappedTypedArray, index);
        if (!addr) {
          return false;
        }

        v = op(addr, v);

        JS_TRY_OR_RETURN_FALSE(cx, ops.storeResult(cx, v, args.rval()));
        return true;
      });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.5 Atomics.exchange ( typedArray, index, value )
static bool atomics_exchange(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  return AtomicReadModifyWrite(cx, args, [](auto addr, auto val) {
    return jit::AtomicOperations::exchangeSeqCst(addr, val);
  });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.2 Atomics.add ( typedArray, index, value )
static bool atomics_add(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  return AtomicReadModifyWrite(cx, args, [](auto addr, auto val) {
    return jit::AtomicOperations::fetchAddSeqCst(addr, val);
  });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.10 Atomics.sub ( typedArray, index, value )
static bool atomics_sub(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  return AtomicReadModifyWrite(cx, args, [](auto addr, auto val) {
    return jit::AtomicOperations::fetchSubSeqCst(addr, val);
  });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.3 Atomics.and ( typedArray, index, value )
static bool atomics_and(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  return AtomicReadModifyWrite(cx, args, [](auto addr, auto val) {
    return jit::AtomicOperations::fetchAndSeqCst(addr, val);
  });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.8 Atomics.or ( typedArray, index, value )
static bool atomics_or(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  return AtomicReadModifyWrite(cx, args, [](auto addr, auto val) {
    return jit::AtomicOperations::fetchOrSeqCst(addr, val);
  });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.13 Atomics.xor ( typedArray, index, value )
static bool atomics_xor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  return AtomicReadModifyWrite(cx, args, [](auto addr, auto val) {
    return jit::AtomicOperations::fetchXorSeqCst(addr, val);
  });
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.6 Atomics.isLockFree ( size )
static bool atomics_isLockFree(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue v = args.get(0);

  // Step 1.
  int32_t size;
  if (v.isInt32()) {
    size = v.toInt32();
  } else {
    double dsize;
    if (!ToInteger(cx, v, &dsize)) {
      return false;
    }

    // Step 7 (non-integer case only).
    if (!mozilla::NumberEqualsInt32(dsize, &size)) {
      args.rval().setBoolean(false);
      return true;
    }
  }

  // Steps 2-7.
  args.rval().setBoolean(jit::AtomicOperations::isLockfreeJS(size));
  return true;
}

namespace js {

/*
 * [SMDOC] Atomics.wait, Atomics.waitAsync, and Atomics.notify
 *
 * `wait`, `waitAsync`, and `notify` are provided as low-level primitives for
 * thread synchronization. The primary use case is to take code that looks like
 * this:
 *
 *     const ValueIndex = 0;
 *     const FlagIndex = 1;
 *
 *     THREAD A:
 *       // Write a value.
 *       Atomics.store(sharedBuffer, ValueIndex, value);
 *       // Update a flag to indicate that the value was written.
 *       Atomics.store(sharedBuffer, FlagIndex, 1);
 *
 *     THREAD B:
 *       // Busy-wait for the flag to be updated.
 *       while (Atomics.load(sharedBuffer, FlagIndex) == 0) {}
 *       // Load the value.
 *       let value = Atomics.load(sharedBuffer, ValueIndex);
 *
 * ...and replace the busy-wait:
 *
 *     THREAD A':
 *       // Write the value and update the flag.
 *       Atomics.store(sharedBuffer, ValueIndex, value);
 *       Atomics.store(sharedBuffer, FlagIndex, 1);
 *       // Notify that the flag has been written.
 *       Atomics.notify(sharedBuffer, FlagIndex);
 *
 *     THREAD B':
 *       // Wait until the flag is notified.
 *       // If it's already non-zero, no wait occurs.
 *       Atomics.wait(sharedBuffer, FlagIndex, 0);
 *       // Load the value.
 *       let value = Atomics.load(sharedBuffer, ValueIndex);
 *
 * `wait` puts the calling thread to sleep until it is notified (or an optional
 * timeout expires). This can't be used on the main thread.
 *
 * `waitAsync` instead creates a Promise which will be resolved when the
 * position is notified (or an optional timeout expires).
 *
 * When `wait` or `waitAsync` is called, a waiter is created and registered with
 * the SharedArrayBuffer. Waiter instances for a SharedArrayRawBuffer are
 * connected in a circular doubly-linked list, containing both sync and async
 * waiters. Sync waiters are stack allocated in the stack frame of the waiting
 * thread. Async waiters are heap-allocated. The `waiters` field of the
 * SharedArrayRawBuffer is a dedicated list head node for the list. Waiters are
 * awoken in a first-in-first-out order. The `next` field of the list head node
 * points to the highest priority waiter. The `prev` field points to the lowest
 * priority waiter. This list is traversed when `notify` is called to find the
 * waiters that should be woken up.
 *
 * Synchronous waits are implemented using a per-context condition variable. See
 * FutexThread::wait.
 *
 * Asynchronous waits are more complicated, particularly with respect to
 * timeouts. In addition to the AsyncFutexWaiter that is added to the list of
 * waiters, we also create:
 *
 *   1. A Promise object to return to the caller. The promise will be resolved
 *      when the waiter is notified or times out.
 *   2. A WaitAsyncNotifyTask (derived from OffThreadPromiseTask) wrapping that
 *      promise. `notify` can be called from any thread, but the promise must be
 *      resolved on the thread that owns it. To resolve the promise, we dispatch
 *      the task to enqueue a promise resolution task in the target's event
 *      loop. The notify task is stored in the AsyncFutexWaiter.
 *   3. If there is a non-zero timeout, a WaitAsyncTimeoutTask (derived from
 *      JS::Dispatchable) containing a pointer to the async waiter. We dispatch
 *      this task to the embedding's event loop, with a delay. When the timeout
 *      expires and the task runs, if the promise has not yet been resolved, we
 *      resolve it with "timed-out".
 *
 * `waitAsync` Lifetimes
 * ---------------------
 *           ┌─────┐
 *           │ SAB │
 *           └─────┘
 *        ┌────► ◄────┐ bi-directional linked list
 *        │           │
 *        ▼           ▼
 *      *waiter      *waiter
 *        ▲           ▲
 *        │           │
 *        └───► *  ◄──┘
 *              │
 *      ┌───────▼────────┐
 *      │AsyncFutexWaiter│ ◄───────────┐
 *      └────────────────┘             │
 *              │                      │
 *              │ borrow               │ borrow
 *              ▼                      ▼
 *      ┌────────────────────┐       ┌───────────────────┐
 *      │WaitAsyncTimeoutTask│       │WaitAsyncNotifyTask│ ◄─────┐
 *      └────────────────────┘       └───┬───────────────┘       │
 *              ▲                        │             ▲         │
 *              │                        │             │         │ (transfered)
 *              │ own                    ▼             │         │ own
 *      ┌───────────────────────────┐ ┌─────────────┐  │ ┌─────────────────────┐
 *      │DelayedJSDispatchaleHandler│ │PromiseObject│  │ │JSDispatchableHandler│
 *      └───────────────────────────┘ └─────────────┘  │ └─────────────────────┘
 *              ▲                        ▲             │
 *     ┌────────┼────────────────────────┼──────┐      │
 *     │ ┌──────┴───────┐           ┌────┴────┐ │      │ own (initialized)
 *     │ │TimeoutManager│           │JSContext┼─┼──────┘
 *     │ └──────────────┘           └─────────┘ │   Cancellable List
 *     │                                        │
 *     │     Runtime (MainThread or Worker)     │
 *     └────────────────────────────────────────┘
 *
 *
 * The data representing an async wait is divided between the JSContext in which
 * it was created and the SharedArrayBuffer being waited on. There are three
 * potential components:
 *
 * A) The AsyncFutexWaiter itself (shared by the SharedArrayRawBuffer,
 *    WaitAsyncNotifyTask, and WaitAsyncTimeoutTask if it exists). It
 *    will be cleaned up manually.
 * B) The corresponding WaitAsyncNotifyTask (owned by the JS::Context). It
 *    destroys itself on run.
 * C) The WaitAsyncTimeoutTask (owned by the embedding's job queue). It
 *    destroys itself on run.
 *
 * WaitAsyncNotifyTask and WaitAsyncTimeoutTask (if it exists) delete
 * themselves. When either task is run or destroyed, they also trigger the
 * destruction and unlinking of the AsyncFutexWaiter. There are
 * four scenarios:
 *
 * 1. A call to `Atomics.notify` notifies the waiter (atomics_notify_impl)
 *    from another thread.
 *    A) The async waiter is removed from the list.
 *    B) The notify task is removed from OffThreadPromiseRuntimeState's
 *       cancelable list and is dispatched to resolve the promise with "ok".
 *       The task then destroys itself.
 *    C) The WaitAsyncTimeoutTask is disabled. It will fire and do nothing.
 *       See AsyncFutexWaiter::maybeCancelTimeout in atomics_notify_impl.
 *    D) The async waiter is destroyed.
 *
 * 2. A call to `Atomics.notify` notifies the waiter (atomics_notify_impl)
 *    from the same thread.
 *    A) The async waiter is removed from the list.
 *    B) The notify task is cancelled. The promise is extracted and resolved
 *        directly.
 *    C) The WaitAsyncTimeoutTask is disabled. It will fire and do nothing.
 *       See AsyncFutexWaiter::maybeCancelTimeout in atomics_notify_impl.
 *    D) The async waiter is destroyed.
 *
 * 3. The timeout expires without notification (WaitAsyncTimeoutTask::run)
 *    A) The async waiter is removed from the list.
 *    B) The notify task is dispatched to resolve the promise with "timed-out"
 *       and destroys itself..
 *    C) The timeout task is running and will be destroyed when it's done.
 *    D) The async waiter is destroyed.
 *
 * 4. The context is destroyed (OffThreadPromiseRuntimeState::shutdown):
 *    A) The async waiter is removed and destroyed by
 *       WaitAsyncNotifyTask::prepareForCancel.
 *    B) The notify task is cancelled and destroyed by
 *       OffThreadPromiseRuntimeState::shutdown.
 *    C) The WaitAsyncTimeoutTask is disabled.
 *       See AsyncFutexWaiter::maybeCancelTimeout in prepareForCancel.
 *
 * 5. The SharedArrayBuffer is collected by the GC (~FutexWaiterListHead)
 *    A) Async waiters without timeouts can no longer resolve. They are removed.
 *    B) If no timeout task exists, the notify task is dispatched and
 *       destroys itself, without resolving the promise.
 *    C) If there is an enqueued timeout, the waiter can still be resolved.
 *       In this case it will not be destroyed until it times out.
 *
 * The UniquePtr can be thought of as a "runnable handle" that gives exclusive
 * access to executing a runnable by a given owner. The runnable will still
 * delete itself (via js_delete, see OffThreadPromiseRuntimeState.cpp
 * implementation of OffThreadPromiseTask::run). If somehow the UniquePtr is not
 * passed to embedding code that will run the code, the task is released from
 * the pointer. We then use the list of raw pointers in
 * OffThreadPromiseRuntimeState's cancellable and dead lists are used to
 * identify which were never dispatched, and which failed to dispatch, and clear
 * them when the engine has an opportunity to do so (i.e. shutdown).
 */

class WaitAsyncNotifyTask;
class WaitAsyncTimeoutTask;

class AutoLockFutexAPI {
  // We have to wrap this in a Maybe because of the way loading
  // mozilla::Atomic pointers works.
  mozilla::Maybe<js::UniqueLock<js::Mutex>> unique_;

 public:
  AutoLockFutexAPI() {
    js::Mutex* lock = FutexThread::lock_;
    unique_.emplace(*lock);
  }

  ~AutoLockFutexAPI() { unique_.reset(); }

  js::UniqueLock<js::Mutex>& unique() { return *unique_; }
};

// Represents one waiter. This is the abstract base class for SyncFutexWaiter
// and AsyncFutexWaiter.
class FutexWaiter : public FutexWaiterListNode {
 protected:
  FutexWaiter(JSContext* cx, size_t offset, FutexWaiterKind kind)
      : FutexWaiterListNode(kind), offset_(offset), cx_(cx) {}

  size_t offset_;  // Element index within the SharedArrayBuffer
  JSContext* cx_;  // The thread that called `wait` or `waitAsync`.

 public:
  bool isSync() const { return kind_ == FutexWaiterKind::Sync; }
  SyncFutexWaiter* asSync() {
    MOZ_ASSERT(isSync());
    return reinterpret_cast<SyncFutexWaiter*>(this);
  }

  bool isAsync() const { return kind_ == FutexWaiterKind::Async; }
  AsyncFutexWaiter* asAsync() {
    MOZ_ASSERT(isAsync());
    return reinterpret_cast<AsyncFutexWaiter*>(this);
  }
  size_t offset() const { return offset_; }
  JSContext* cx() { return cx_; }
};

// Represents a worker blocked while calling |Atomics.wait|.
// Instances of js::SyncFutexWaiter are stack-allocated and linked
// onto the waiter list across a call to FutexThread::wait().
// When this waiter is notified, the worker will resume execution.
class MOZ_STACK_CLASS SyncFutexWaiter : public FutexWaiter {
 public:
  SyncFutexWaiter(JSContext* cx, size_t offset)
      : FutexWaiter(cx, offset, FutexWaiterKind::Sync) {}
};

// Represents a waiter asynchronously waiting after calling |Atomics.waitAsync|.
// Instances of js::AsyncFutexWaiter are heap-allocated.
// When this waiter is notified, the promise it holds will be resolved.
class AsyncFutexWaiter : public FutexWaiter {
 public:
  AsyncFutexWaiter(JSContext* cx, size_t offset)
      : FutexWaiter(cx, offset, FutexWaiterKind::Async) {}

  WaitAsyncNotifyTask* notifyTask() { return notifyTask_; }

  void setNotifyTask(WaitAsyncNotifyTask* task) {
    MOZ_ASSERT(!notifyTask_);
    notifyTask_ = task;
  }

  void setTimeoutTask(WaitAsyncTimeoutTask* task) {
    MOZ_ASSERT(!timeoutTask_);
    timeoutTask_ = task;
  }

  bool hasTimeout() const { return !!timeoutTask_; }
  WaitAsyncTimeoutTask* timeoutTask() const { return timeoutTask_; }

  void maybeClearTimeout(AutoLockFutexAPI& lock);

 private:
  // Both of these pointers are borrowed pointers. The notifyTask is owned by
  // the runtime's cancellable list, while the timeout task (if it exists) is
  // owned by the embedding's timeout manager.
  WaitAsyncNotifyTask* notifyTask_ = nullptr;
  WaitAsyncTimeoutTask* timeoutTask_ = nullptr;
};

// When an async waiter from a different context is notified, this
// task is queued to resolve the promise on the thread to which it
// belongs.
//
// WaitAsyncNotifyTask (derived from OffThreadPromiseTask) is wrapping that
// promise. `Atomics.notify` can be called from any thread, but the promise must
// be resolved on the thread that owns it. To resolve the promise, we dispatch
// the task to enqueue a promise resolution task in the target's event
// loop.
//
// See [SMDOC] Atomics.wait for more details.
class WaitAsyncNotifyTask : public OffThreadPromiseTask {
 public:
  enum class Result { Ok, TimedOut, Dead };

 private:
  Result result_ = Result::Ok;

  // A back-edge to the waiter so that it can be cleaned up when the
  // Notify Task is dispatched and destroyed.
  AsyncFutexWaiter* waiter_ = nullptr;

 public:
  WaitAsyncNotifyTask(JSContext* cx, Handle<PromiseObject*> promise)
      : OffThreadPromiseTask(cx, promise) {}

  void setWaiter(AsyncFutexWaiter* waiter) {
    MOZ_ASSERT(!waiter_);
    waiter_ = waiter;
  }

  void setResult(Result result, AutoLockFutexAPI& lock) { result_ = result; }

  bool resolve(JSContext* cx, Handle<PromiseObject*> promise) override {
    RootedValue resultMsg(cx);
    switch (result_) {
      case Result::Ok:
        resultMsg = StringValue(cx->names().ok);
        break;
      case Result::TimedOut:
        resultMsg = StringValue(cx->names().timed_out_);
        break;
      case Result::Dead:
        // The underlying SharedArrayBuffer is no longer reachable, and no
        // timeout is associated with this waiter. The promise will never
        // resolve. There's nothing to do here.
        return true;
    }
    return PromiseObject::resolve(cx, promise, resultMsg);
  }

  void prepareForCancel() override;
};

// WaitAsyncNotifyTask (derived from OffThreadPromiseTask) is wrapping that
// promise. `notify` can be called from any thread, but the promise must be
// resolved on the thread that owns it. To resolve the promise, we dispatch
// the task to enqueue a promise resolution task in the target's event
// loop.
//
// See [SMDOC] Atomics.wait for more details.
class WaitAsyncTimeoutTask : public JS::Dispatchable {
  AsyncFutexWaiter* waiter_;

 public:
  explicit WaitAsyncTimeoutTask(AsyncFutexWaiter* waiter) : waiter_(waiter) {
    MOZ_ASSERT(waiter_);
  }

  void clear(AutoLockFutexAPI&) { waiter_ = nullptr; }
  bool cleared(AutoLockFutexAPI&) { return !waiter_; }

  void run(JSContext*, MaybeShuttingDown maybeshuttingdown) final;
  void transferToRuntime() final;
};

}  // namespace js

// https://tc39.es/ecma262/#sec-addwaiter
static void AddWaiter(SharedArrayRawBuffer* sarb, FutexWaiter* node,
                      AutoLockFutexAPI&) {
  FutexWaiterListNode* listHead = sarb->waiters();

  // Step 3: Append waiterRecord to WL.[[Waiters]].
  node->setNext(listHead);
  node->setPrev(listHead->prev());
  listHead->prev()->setNext(node);
  listHead->setPrev(node);
}

// https://tc39.es/ecma262/#sec-removewaiter
static void RemoveWaiterImpl(FutexWaiterListNode* node, AutoLockFutexAPI&) {
  if (!node->prev()) {
    MOZ_ASSERT(!node->next());
    return;
  }

  node->prev()->setNext(node->next());
  node->next()->setPrev(node->prev());

  node->setNext(nullptr);
  node->setPrev(nullptr);
}

// Sync waiters are stack allocated and can simply be removed from the list.
static void RemoveSyncWaiter(SyncFutexWaiter* waiter, AutoLockFutexAPI& lock) {
  RemoveWaiterImpl(waiter, lock);
}

// Async waiters are heap allocated. After removing the waiter, the caller
// is responsible for freeing it. Return the waiter to help enforce this.
[[nodiscard]] AsyncFutexWaiter* RemoveAsyncWaiter(AsyncFutexWaiter* waiter,
                                                  AutoLockFutexAPI& lock) {
  RemoveWaiterImpl(waiter, lock);
  return waiter;
}

FutexWaiterListHead::~FutexWaiterListHead() {
  // Cleanup steps from 5. in SMDOC for Atomics.waitAsync
  // When a SharedArrayRawBuffer is no longer reachable, the contents of its
  // waiters list can no longer be notified. However, they can still resolve if
  // they have an associated timeout. When the list head goes away, we walk
  // through the remaining waiters and clean up the ones that don't have
  // timeouts. We leave the remaining waiters in a free-floating linked list;
  // they will remove themselves as the timeouts fire or the associated runtime
  // shuts down.
  AutoLockHelperThreadState helperLock;
  AutoLockFutexAPI lock;

  FutexWaiterListNode* iter = next();
  while (iter != this) {
    // All remaining FutexWaiters must be async. A sync waiter can only exist if
    // a thread is waiting, and that thread must have a reference to the shared
    // array buffer it's waiting on, so that buffer can't be freed.

    AsyncFutexWaiter* removedWaiter =
        RemoveAsyncWaiter(iter->toWaiter()->asAsync(), lock);
    iter = iter->next();

    if (removedWaiter->hasTimeout()) {
      // If a timeout task exists, assert that the timeout task can still access
      // it. This will allow it to clean it up when it runs.  See the comment in
      // WaitAsyncTimeoutTask::run() or the the SMDOC in this file.
      MOZ_ASSERT(removedWaiter->timeoutTask()->cleared(lock));
      continue;
    }
    // In the case that a timeout task does not exist, the two live raw
    // pointers at this point are WaitAsyncNotifyTask and the
    // AsyncFutexWaiter. We can clean them up here as there is no way to
    // notify them without the SAB or without waiting for the shutdown of the
    // JS::Context. In order to do this, we store the removed waiter in a
    // unique ptr, so that it is cleared after this function, and dispatch and
    // destroy the notify task.
    UniquePtr<AsyncFutexWaiter> ownedWaiter(removedWaiter);
    WaitAsyncNotifyTask* task = ownedWaiter->notifyTask();
    task->setResult(WaitAsyncNotifyTask::Result::Dead, lock);
    task->removeFromCancellableListAndDispatch(helperLock);
  }

  RemoveWaiterImpl(this, lock);
}

// Creates an object to use as the return value of Atomics.waitAsync.
static PlainObject* CreateAsyncResultObject(JSContext* cx, bool async,
                                            HandleValue promiseOrString) {
  Rooted<PlainObject*> resultObject(cx, NewPlainObject(cx));
  if (!resultObject) {
    return nullptr;
  }

  RootedValue isAsync(cx, BooleanValue(async));
  if (!NativeDefineDataProperty(cx, resultObject, cx->names().async, isAsync,
                                JSPROP_ENUMERATE)) {
    return nullptr;
  }

  MOZ_ASSERT_IF(!async, promiseOrString.isString());
  MOZ_ASSERT_IF(async, promiseOrString.isObject() &&
                           promiseOrString.toObject().is<PromiseObject>());
  if (!NativeDefineDataProperty(cx, resultObject, cx->names().value,
                                promiseOrString, JSPROP_ENUMERATE)) {
    return nullptr;
  }

  return resultObject;
}

void WaitAsyncNotifyTask::prepareForCancel() {
  AutoLockFutexAPI lock;
  UniquePtr<AsyncFutexWaiter> waiter(RemoveAsyncWaiter(waiter_, lock));
  waiter->maybeClearTimeout(lock);
}

void WaitAsyncTimeoutTask::run(JSContext* cx,
                               MaybeShuttingDown maybeShuttingDown) {
  AutoLockHelperThreadState helperLock;
  AutoLockFutexAPI lock;

  // If the waiter was notified while this task was enqueued, do nothing.
  if (cleared(lock)) {
    js_delete(this);
    return;
  }

  // Cleanup steps from 3. and 5. lifecycle in SMDOC for Atomics.waitAsync
  // Take ownership of the async waiter, so that it will be freed
  // when we return.
  UniquePtr<AsyncFutexWaiter> asyncWaiter(RemoveAsyncWaiter(waiter_, lock));

  // Dispatch a task to resolve the promise with value "timed-out".
  WaitAsyncNotifyTask* task = asyncWaiter->notifyTask();
  task->setResult(WaitAsyncNotifyTask::Result::TimedOut, lock);
  task->removeFromCancellableListAndDispatch(helperLock);
  js_delete(this);
}

void WaitAsyncTimeoutTask::transferToRuntime() {
  // Clear and delete. Clearing this task will result in the cancellable
  // notify task being cleaned up on shutdown, as it can no longer be triggered.
  // In as sense, the task "transfered" for cleanup is the notify task.
  {
    AutoLockFutexAPI lock;
    clear(lock);
  }
  // As we are not managing any state, the runtime is not tracking this task,
  // and we have nothing to run, we can delete.
  js_delete(this);
}

void AsyncFutexWaiter::maybeClearTimeout(AutoLockFutexAPI& lock) {
  if (timeoutTask_) {
    timeoutTask_->clear(lock);
  }
}

// DoWait Steps 17-31
// https://tc39.es/ecma262/#sec-dowait
template <typename T>
static FutexThread::WaitResult AtomicsWaitAsyncCriticalSection(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, T value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout,
    Handle<PromiseObject*> promise) {
  // Step 17: Enter critical section.
  // We need to initialize an OffThreadPromiseTask inside this critical section.
  // To avoid deadlock, we claim the helper thread lock first.
  AutoLockHelperThreadState helperThreadLock;
  AutoLockFutexAPI futexLock;

  // Steps 18-20:
  SharedMem<T*> addr =
      sarb->dataPointerShared().cast<T*>() + (byteOffset / sizeof(T));
  if (jit::AtomicOperations::loadSafeWhenRacy(addr) != value) {
    return FutexThread::WaitResult::NotEqual;
  }

  // Step 21
  bool hasTimeout = timeout.isSome();
  if (hasTimeout && timeout.value().IsZero()) {
    return FutexThread::WaitResult::TimedOut;
  }

  // Steps 22-30
  // To handle potential failures, we split this up into two phases:
  // First, we allocate everything: the notify task, the waiter, and
  // (if necessary) the timeout task. The allocations are managed
  // using unique pointers, which will free them on failure. This
  // phase has no external side-effects.

  // Second, we transfer ownership of the allocations to the right places:
  // the waiter owns the notify task, the shared array buffer owns the waiter,
  // and the event loop owns the timeout task. This phase is infallible.
  auto notifyTask = js::MakeUnique<WaitAsyncNotifyTask>(cx, promise);
  if (!notifyTask) {
    JS_ReportOutOfMemory(cx);
    return FutexThread::WaitResult::Error;
  }
  auto waiter = js::MakeUnique<AsyncFutexWaiter>(cx, byteOffset);
  if (!waiter) {
    JS_ReportOutOfMemory(cx);
    return FutexThread::WaitResult::Error;
  }

  notifyTask->setWaiter(waiter.get());
  waiter->setNotifyTask(notifyTask.get());

  UniquePtr<WaitAsyncTimeoutTask> timeoutTask;
  if (hasTimeout) {
    timeoutTask = js::MakeUnique<WaitAsyncTimeoutTask>(waiter.get());
    if (!timeoutTask) {
      JS_ReportOutOfMemory(cx);
      return FutexThread::WaitResult::Error;
    }
    waiter->setTimeoutTask(timeoutTask.get());
  }

  // This is the last fallible operation. If it fails, all allocations
  // will be freed. init has no side-effects if it fails.
  if (!js::OffThreadPromiseTask::InitCancellable(cx, helperThreadLock,
                                                 std::move(notifyTask))) {
    return FutexThread::WaitResult::Error;
  }

  // Below this point, everything is infallible.
  AddWaiter(sarb, waiter.release(), futexLock);

  if (hasTimeout) {
    MOZ_ASSERT(!!timeoutTask);
    OffThreadPromiseRuntimeState& state =
        cx->runtime()->offThreadPromiseState.ref();
    // We are not tracking the dispatch of the timeout task using the
    // OffThreadPromiseRuntimeState, so we ignore the return value. If this
    // fails, the embeddings should call transferToRuntime on timeoutTask
    // which will clear itself, and set the notify task to be cleaned on
    // shutdown.
    (void)state.delayedDispatchToEventLoop(std::move(timeoutTask),
                                           timeout.value().ToMilliseconds());
  }

  // Step 31: Leave critical section.
  return FutexThread::WaitResult::OK;
}

// DoWait steps 12-35
// https://tc39.es/ecma262/#sec-dowait
// This implements the mode=ASYNC case.
template <typename T>
static PlainObject* AtomicsWaitAsync(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, T value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout) {
  // Step 16a.
  Rooted<PromiseObject*> promiseObject(
      cx, CreatePromiseObjectWithoutResolutionFunctions(cx));
  if (!promiseObject) {
    return nullptr;
  }

  // Steps 17-31
  switch (AtomicsWaitAsyncCriticalSection(cx, sarb, byteOffset, value, timeout,
                                          promiseObject)) {
    case FutexThread::WaitResult::NotEqual: {
      // Steps 16b, 20c-e
      RootedValue msg(cx, StringValue(cx->names().not_equal_));
      return CreateAsyncResultObject(cx, false, msg);
    }
    case FutexThread::WaitResult::TimedOut: {
      // Steps 16b, 21c-e
      RootedValue msg(cx, StringValue(cx->names().timed_out_));
      return CreateAsyncResultObject(cx, false, msg);
    }
    case FutexThread::WaitResult::Error:
      return nullptr;
    case FutexThread::WaitResult::OK:
      break;
  }

  // Steps 15b, 33-35
  RootedValue objectValue(cx, ObjectValue(*promiseObject));
  return CreateAsyncResultObject(cx, true, objectValue);
}

// DoWait steps 12-32
// https://tc39.es/ecma262/#sec-dowait
// This implements the mode=SYNC case.
template <typename T>
static FutexThread::WaitResult AtomicsWait(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, T value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout) {
  // Validation and other guards should ensure that this does not happen.
  MOZ_ASSERT(sarb, "wait is only applicable to shared memory");

  SharedMem<T*> addr =
      sarb->dataPointerShared().cast<T*>() + (byteOffset / sizeof(T));

  // Steps 17 and 31 (through destructor).
  // This lock also protects the "waiters" field on SharedArrayRawBuffer,
  // and it provides the necessary memory fence.
  AutoLockFutexAPI lock;

  // Steps 18-20.
  if (jit::AtomicOperations::loadSafeWhenRacy(addr) != value) {
    return FutexThread::WaitResult::NotEqual;
  }

  // Steps 14, 22-27
  SyncFutexWaiter w(cx, byteOffset);

  // Steps 28-29
  AddWaiter(sarb, &w, lock);
  FutexThread::WaitResult retval = cx->fx.wait(cx, lock.unique(), timeout);
  RemoveSyncWaiter(&w, lock);

  // Step 32
  return retval;
}

FutexThread::WaitResult js::atomics_wait_impl(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, int32_t value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout) {
  return AtomicsWait(cx, sarb, byteOffset, value, timeout);
}

FutexThread::WaitResult js::atomics_wait_impl(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, int64_t value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout) {
  return AtomicsWait(cx, sarb, byteOffset, value, timeout);
}

PlainObject* js::atomics_wait_async_impl(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, int32_t value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout) {
  return AtomicsWaitAsync(cx, sarb, byteOffset, value, timeout);
}

PlainObject* js::atomics_wait_async_impl(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, int64_t value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout) {
  return AtomicsWaitAsync(cx, sarb, byteOffset, value, timeout);
}

// https://tc39.es/ecma262/#sec-dowait
// DoWait ( mode, typedArray, index, value, timeout ), steps 8-35.
template <typename T>
static bool DoAtomicsWait(JSContext* cx, bool isAsync,
                          Handle<TypedArrayObject*> unwrappedTypedArray,
                          size_t index, T value, HandleValue timeoutv,
                          MutableHandleValue r) {
  mozilla::Maybe<mozilla::TimeDuration> timeout;
  if (!timeoutv.isUndefined()) {
    // Step 8.
    double timeout_ms;
    if (!ToNumber(cx, timeoutv, &timeout_ms)) {
      return false;
    }

    // Step 9.
    if (!std::isnan(timeout_ms)) {
      if (timeout_ms < 0) {
        timeout = mozilla::Some(mozilla::TimeDuration::FromSeconds(0.0));
      } else if (!std::isinf(timeout_ms)) {
        timeout =
            mozilla::Some(mozilla::TimeDuration::FromMilliseconds(timeout_ms));
      }
    }
  }

  // Step 10.
  if (!isAsync && !cx->fx.canWait()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ATOMICS_WAIT_NOT_ALLOWED);
    return false;
  }

  // Step 11.
  Rooted<SharedArrayBufferObject*> unwrappedSab(
      cx, unwrappedTypedArray->bufferShared());

  // Step 12
  mozilla::Maybe<size_t> offset = unwrappedTypedArray->byteOffset();
  MOZ_ASSERT(
      offset,
      "offset can't become invalid because shared buffers can only grow");

  // Step 13.
  // The computation will not overflow because range checks have been
  // performed.
  size_t byteIndexInBuffer = index * sizeof(T) + *offset;

  // Steps 14-35.
  if (isAsync) {
    PlainObject* resultObject = atomics_wait_async_impl(
        cx, unwrappedSab->rawBufferObject(), byteIndexInBuffer, value, timeout);
    if (!resultObject) {
      return false;
    }
    r.setObject(*resultObject);
    return true;
  }

  switch (atomics_wait_impl(cx, unwrappedSab->rawBufferObject(),
                            byteIndexInBuffer, value, timeout)) {
    case FutexThread::WaitResult::NotEqual:
      r.setString(cx->names().not_equal_);
      return true;
    case FutexThread::WaitResult::OK:
      r.setString(cx->names().ok);
      return true;
    case FutexThread::WaitResult::TimedOut:
      r.setString(cx->names().timed_out_);
      return true;
    case FutexThread::WaitResult::Error:
      return false;
    default:
      MOZ_CRASH("Should not happen");
  }
}

// https://tc39.es/ecma262/#sec-dowait
// DoWait ( mode, typedArray, index, value, timeout )
static bool DoWait(JSContext* cx, bool isAsync, HandleValue objv,
                   HandleValue index, HandleValue valv, HandleValue timeoutv,
                   MutableHandleValue r) {
  // Steps 1-2.
  Rooted<TypedArrayObject*> unwrappedTypedArray(cx);
  if (!ValidateIntegerTypedArray(cx, objv, true, &unwrappedTypedArray)) {
    return false;
  }
  MOZ_ASSERT(unwrappedTypedArray->type() == Scalar::Int32 ||
             unwrappedTypedArray->type() == Scalar::BigInt64);

  // Step 3
  if (!unwrappedTypedArray->isSharedMemory()) {
    return ReportBadArrayType(cx);
  }

  // Step 4.
  size_t intIndex;
  if (!ValidateAtomicAccess(cx, unwrappedTypedArray, index, &intIndex)) {
    return false;
  }

  // Step 5
  if (unwrappedTypedArray->type() == Scalar::Int32) {
    // Step 7.
    int32_t value;
    if (!ToInt32(cx, valv, &value)) {
      return false;
    }

    // Steps 8-35.
    return DoAtomicsWait(cx, isAsync, unwrappedTypedArray, intIndex, value,
                         timeoutv, r);
  }

  MOZ_ASSERT(unwrappedTypedArray->type() == Scalar::BigInt64);

  // Step 6.
  RootedBigInt value(cx, ToBigInt(cx, valv));
  if (!value) {
    return false;
  }

  // Steps 8-35.
  return DoAtomicsWait(cx, isAsync, unwrappedTypedArray, intIndex,
                       BigInt::toInt64(value), timeoutv, r);
}

// 24.4.11 Atomics.wait ( typedArray, index, value, timeout )
// https://tc39.es/ecma262/#sec-atomics.wait
static bool atomics_wait(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue objv = args.get(0);
  HandleValue index = args.get(1);
  HandleValue valv = args.get(2);
  HandleValue timeoutv = args.get(3);
  MutableHandleValue r = args.rval();

  return DoWait(cx, /*isAsync = */ false, objv, index, valv, timeoutv, r);
}

// Atomics.waitAsync ( typedArray, index, value, timeout )
// https://tc39.es/ecma262/#sec-atomics.waitasync
static bool atomics_wait_async(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue objv = args.get(0);
  HandleValue index = args.get(1);
  HandleValue valv = args.get(2);
  HandleValue timeoutv = args.get(3);
  MutableHandleValue r = args.rval();

  return DoWait(cx, /*isAsync = */ true, objv, index, valv, timeoutv, r);
}

// Atomics.notify ( typedArray, index, count ), steps 8-13.
// https://tc39.es/ecma262/#sec-atomics.notify
bool js::atomics_notify_impl(JSContext* cx, SharedArrayRawBuffer* sarb,
                             size_t byteOffset, int64_t count, int64_t* woken) {
  MOZ_ASSERT(woken);

  // Validation should ensure this does not happen.
  MOZ_ASSERT(sarb, "notify is only applicable to shared memory");

  // Step 8
  *woken = 0;

  Rooted<GCVector<PromiseObject*>> promisesToResolve(
      cx, GCVector<PromiseObject*>(cx));
  {
    // Steps 9, 12 (through destructor).
    AutoLockHelperThreadState helperLock;
    AutoLockFutexAPI lock;
    // Steps 10-11
    FutexWaiterListNode* waiterListHead = sarb->waiters();
    FutexWaiterListNode* iter = waiterListHead->next();
    while (count && iter != waiterListHead) {
      FutexWaiter* waiter = iter->toWaiter();
      iter = iter->next();
      if (byteOffset != waiter->offset()) {
        continue;
      }
      if (waiter->isSync()) {
        // For sync waits, the context to notify is currently sleeping.
        // We notify that context (unless it's already been notified by
        // another thread).
        if (!waiter->cx()->fx.isWaiting()) {
          continue;
        }
        waiter->cx()->fx.notify(FutexThread::NotifyExplicit);
      } else {
        // For async waits, we resolve a promise.

        // Steps to clean up case 1. and 2. in SMDOC for Atomics.waitAsync
        // Take ownership of the async waiter, so that it will be
        // freed at the end of this block.
        UniquePtr<AsyncFutexWaiter> asyncWaiter(
            RemoveAsyncWaiter(waiter->asAsync(), lock));
        asyncWaiter->maybeClearTimeout(lock);
        // If we are notifying a waiter that was created by the current
        // context, we resolve the promise directly instead of dispatching
        // a task to the event loop.
        OffThreadPromiseTask* task = asyncWaiter->notifyTask();
        if (waiter->cx() == cx) {
          // Add the promise to a list to resolve as soon as we've left the
          // critical section.
          PromiseObject* promise =
              OffThreadPromiseTask::ExtractAndForget(task, helperLock);
          if (!promisesToResolve.append(promise)) {
            return false;
          }
        } else {
          // Dispatch a task to resolve the promise with value "ok".
          task->removeFromCancellableListAndDispatch(helperLock);
        }
      }
      // Overflow will be a problem only in two cases:
      // (1) 128-bit systems with substantially more than 2^64 bytes of
      //     memory per process, and a very lightweight
      //     Atomics.waitAsync().  Obviously a future problem.
      // (2) Bugs.
      MOZ_RELEASE_ASSERT(*woken < INT64_MAX);
      (*woken)++;
      if (count > 0) {
        --count;
      }
    }
  }

  // Step 10 (reordered)
  // We resolve same-thread promises after we've left the critical section to
  // avoid mutex ordering problems.
  RootedValue resultMsg(cx, StringValue(cx->names().ok));
  for (uint32_t i = 0; i < promisesToResolve.length(); i++) {
    if (!PromiseObject::resolve(cx, promisesToResolve[i], resultMsg)) {
      MOZ_ASSERT(cx->isThrowingOutOfMemory() || cx->isThrowingOverRecursed());
      return false;
    }
  }

  // Step 13.
  return true;
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.12 Atomics.notify ( typedArray, index, count )
static bool atomics_notify(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue objv = args.get(0);
  HandleValue index = args.get(1);
  HandleValue countv = args.get(2);
  MutableHandleValue r = args.rval();

  // Step 1.
  Rooted<TypedArrayObject*> unwrappedTypedArray(cx);
  if (!ValidateIntegerTypedArray(cx, objv, true, &unwrappedTypedArray)) {
    return false;
  }
  MOZ_ASSERT(unwrappedTypedArray->type() == Scalar::Int32 ||
             unwrappedTypedArray->type() == Scalar::BigInt64);

  // Step 2.
  size_t intIndex;
  if (!ValidateAtomicAccess(cx, unwrappedTypedArray, index, &intIndex)) {
    return false;
  }

  // Steps 3-4.
  int64_t count;
  if (countv.isUndefined()) {
    count = -1;
  } else {
    double dcount;
    if (!ToInteger(cx, countv, &dcount)) {
      return false;
    }
    if (dcount < 0.0) {
      dcount = 0.0;
    }
    count = dcount < double(1ULL << 63) ? int64_t(dcount) : -1;
  }

  // https://github.com/tc39/ecma262/pull/1908
  if (!unwrappedTypedArray->isSharedMemory()) {
    r.setInt32(0);
    return true;
  }

  // Step 5.
  Rooted<SharedArrayBufferObject*> unwrappedSab(
      cx, unwrappedTypedArray->bufferShared());

  // Step 6.
  mozilla::Maybe<size_t> offset = unwrappedTypedArray->byteOffset();
  MOZ_ASSERT(
      offset,
      "offset can't become invalid because shared buffers can only grow");

  // Steps 7-9.
  // The computation will not overflow because range checks have been
  // performed.
  size_t elementSize = Scalar::byteSize(unwrappedTypedArray->type());
  size_t indexedPosition = intIndex * elementSize + *offset;

  // Steps 10-16.

  int64_t woken = 0;
  if (!atomics_notify_impl(cx, unwrappedSab->rawBufferObject(), indexedPosition,
                           count, &woken)) {
    return false;
  }

  r.setNumber(double(woken));

  return true;
}

/**
 * Atomics.pause ( [ N ] )
 *
 * https://tc39.es/proposal-atomics-microwait/
 */
static bool atomics_pause(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (args.hasDefined(0)) {
    if (!args[0].isNumber() || !IsInteger(args[0].toNumber())) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_ATOMICS_PAUSE_BAD_COUNT);
      return false;
    }
  }

  // Step 2.
  //
  // We ignore the iteration count when not inlining this operation.
  jit::AtomicOperations::pause();

  // Step 3.
  args.rval().setUndefined();
  return true;
}

/* static */
bool js::FutexThread::initialize() {
  MOZ_ASSERT(!lock_);
  lock_ = js_new<js::Mutex>(mutexid::FutexThread);
  return lock_ != nullptr;
}

/* static */
void js::FutexThread::destroy() {
  if (lock_) {
    js::Mutex* lock = lock_;
    js_delete(lock);
    lock_ = nullptr;
  }
}

/* static */
void js::FutexThread::lock() {
  // Load the atomic pointer.
  js::Mutex* lock = lock_;

  lock->lock();
}

/* static */ mozilla::Atomic<js::Mutex*, mozilla::SequentiallyConsistent>
    FutexThread::lock_;

/* static */
void js::FutexThread::unlock() {
  // Load the atomic pointer.
  js::Mutex* lock = lock_;

  lock->unlock();
}

js::FutexThread::FutexThread()
    : cond_(nullptr), state_(Idle), canWait_(false) {}

bool js::FutexThread::initInstance() {
  MOZ_ASSERT(lock_);
  cond_ = js_new<js::ConditionVariable>();
  return cond_ != nullptr;
}

void js::FutexThread::destroyInstance() {
  if (cond_) {
    js_delete(cond_);
  }
}

bool js::FutexThread::isWaiting() {
  // When a worker is awoken for an interrupt it goes into state
  // WaitingNotifiedForInterrupt for a short time before it actually
  // wakes up and goes into WaitingInterrupted.  In those states the
  // worker is still waiting, and if an explicit notify arrives the
  // worker transitions to Woken.  See further comments in
  // FutexThread::wait().
  return state_ == Waiting || state_ == WaitingInterrupted ||
         state_ == WaitingNotifiedForInterrupt;
}

FutexThread::WaitResult js::FutexThread::wait(
    JSContext* cx, js::UniqueLock<js::Mutex>& locked,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout) {
  MOZ_ASSERT(&cx->fx == this);
  MOZ_ASSERT(cx->fx.canWait());
  MOZ_ASSERT(state_ == Idle || state_ == WaitingInterrupted);

  // Disallow waiting when a runtime is processing an interrupt.
  // See explanation below.

  if (state_ == WaitingInterrupted) {
    UnlockGuard unlock(locked);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ATOMICS_WAIT_NOT_ALLOWED);
    return WaitResult::Error;
  }

  // Go back to Idle after returning.
  auto onFinish = mozilla::MakeScopeExit([&] { state_ = Idle; });

  const bool isTimed = timeout.isSome();

  auto finalEnd = timeout.map([](const mozilla::TimeDuration& timeout) {
    return mozilla::TimeStamp::Now() + timeout;
  });

  // 4000s is about the longest timeout slice that is guaranteed to
  // work cross-platform.
  auto maxSlice = mozilla::TimeDuration::FromSeconds(4000.0);

  for (;;) {
    // If we are doing a timed wait, calculate the end time for this wait
    // slice.
    auto sliceEnd = finalEnd.map([&](mozilla::TimeStamp& finalEnd) {
      auto sliceEnd = mozilla::TimeStamp::Now() + maxSlice;
      if (finalEnd < sliceEnd) {
        sliceEnd = finalEnd;
      }
      return sliceEnd;
    });

    state_ = Waiting;

    MOZ_ASSERT((cx->runtime()->beforeWaitCallback == nullptr) ==
               (cx->runtime()->afterWaitCallback == nullptr));
    mozilla::DebugOnly<bool> callbacksPresent =
        cx->runtime()->beforeWaitCallback != nullptr;

    void* cookie = nullptr;
    uint8_t clientMemory[JS::WAIT_CALLBACK_CLIENT_MAXMEM];
    if (cx->runtime()->beforeWaitCallback) {
      cookie = (*cx->runtime()->beforeWaitCallback)(clientMemory);
    }

    if (isTimed) {
      (void)cond_->wait_until(locked, *sliceEnd);
    } else {
      cond_->wait(locked);
    }

    MOZ_ASSERT((cx->runtime()->afterWaitCallback != nullptr) ==
               callbacksPresent);
    if (cx->runtime()->afterWaitCallback) {
      (*cx->runtime()->afterWaitCallback)(cookie);
    }

    switch (state_) {
      case FutexThread::Waiting:
        // Timeout or spurious wakeup.
        if (isTimed) {
          auto now = mozilla::TimeStamp::Now();
          if (now >= *finalEnd) {
            return WaitResult::TimedOut;
          }
        }
        break;

      case FutexThread::Woken:
        return WaitResult::OK;

      case FutexThread::WaitingNotifiedForInterrupt:
        // The interrupt handler may reenter the engine.  In that case
        // there are two complications:
        //
        // - The waiting thread is not actually waiting on the
        //   condition variable so we have to record that it
        //   should be woken when the interrupt handler returns.
        //   To that end, we flag the thread as interrupted around
        //   the interrupt and check state_ when the interrupt
        //   handler returns.  A notify() call that reaches the
        //   runtime during the interrupt sets state_ to Woken.
        //
        // - It is in principle possible for wait() to be
        //   reentered on the same thread/runtime and waiting on the
        //   same location and to yet again be interrupted and enter
        //   the interrupt handler.  In this case, it is important
        //   that when another agent notifies waiters, all waiters using
        //   the same runtime on the same location are woken in LIFO
        //   order; FIFO may be the required order, but FIFO would
        //   fail to wake up the innermost call.  Interrupts are
        //   outside any spec anyway.  Also, several such suspended
        //   waiters may be woken at a time.
        //
        //   For the time being we disallow waiting from within code
        //   that runs from within an interrupt handler; this may
        //   occasionally (very rarely) be surprising but is
        //   expedient.  Other solutions exist, see bug #1131943.  The
        //   code that performs the check is above, at the head of
        //   this function.

        state_ = WaitingInterrupted;
        {
          UnlockGuard unlock(locked);
          if (!cx->handleInterrupt()) {
            return WaitResult::Error;
          }
        }
        if (state_ == Woken) {
          return WaitResult::OK;
        }
        break;

      default:
        MOZ_CRASH("Bad FutexState in wait()");
    }
  }
}

void js::FutexThread::notify(NotifyReason reason) {
  MOZ_ASSERT(isWaiting());

  if ((state_ == WaitingInterrupted || state_ == WaitingNotifiedForInterrupt) &&
      reason == NotifyExplicit) {
    state_ = Woken;
    return;
  }
  switch (reason) {
    case NotifyExplicit:
      state_ = Woken;
      break;
    case NotifyForJSInterrupt:
      if (state_ == WaitingNotifiedForInterrupt) {
        return;
      }
      state_ = WaitingNotifiedForInterrupt;
      break;
    default:
      MOZ_CRASH("bad NotifyReason in FutexThread::notify()");
  }
  cond_->notify_all();
}

const JSFunctionSpec AtomicsMethods[] = {
    JS_INLINABLE_FN("compareExchange", atomics_compareExchange, 4, 0,
                    AtomicsCompareExchange),
    JS_INLINABLE_FN("load", atomics_load, 2, 0, AtomicsLoad),
    JS_INLINABLE_FN("store", atomics_store, 3, 0, AtomicsStore),
    JS_INLINABLE_FN("exchange", atomics_exchange, 3, 0, AtomicsExchange),
    JS_INLINABLE_FN("add", atomics_add, 3, 0, AtomicsAdd),
    JS_INLINABLE_FN("sub", atomics_sub, 3, 0, AtomicsSub),
    JS_INLINABLE_FN("and", atomics_and, 3, 0, AtomicsAnd),
    JS_INLINABLE_FN("or", atomics_or, 3, 0, AtomicsOr),
    JS_INLINABLE_FN("xor", atomics_xor, 3, 0, AtomicsXor),
    JS_INLINABLE_FN("isLockFree", atomics_isLockFree, 1, 0, AtomicsIsLockFree),
    JS_FN("wait", atomics_wait, 4, 0),
    JS_FN("waitAsync", atomics_wait_async, 4, 0),
    JS_FN("notify", atomics_notify, 3, 0),
    JS_FN("wake", atomics_notify, 3, 0),  // Legacy name
    JS_INLINABLE_FN("pause", atomics_pause, 0, 0, AtomicsPause),
    JS_FS_END,
};

static const JSPropertySpec AtomicsProperties[] = {
    JS_STRING_SYM_PS(toStringTag, "Atomics", JSPROP_READONLY),
    JS_PS_END,
};

static JSObject* CreateAtomicsObject(JSContext* cx, JSProtoKey key) {
  RootedObject proto(cx, &cx->global()->getObjectPrototype());
  return NewTenuredObjectWithGivenProto(cx, &AtomicsObject::class_, proto);
}

static const ClassSpec AtomicsClassSpec = {
    CreateAtomicsObject,
    nullptr,
    AtomicsMethods,
    AtomicsProperties,
};

const JSClass AtomicsObject::class_ = {
    "Atomics",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Atomics),
    JS_NULL_CLASS_OPS,
    &AtomicsClassSpec,
};
