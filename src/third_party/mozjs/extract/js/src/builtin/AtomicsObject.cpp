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

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsnum.h"

#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "js/Class.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/Result.h"
#include "vm/GlobalObject.h"
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmInstance.h"

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
  // Step 1 (implicit).

  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  size_t length = typedArray->length();

  // Step 2.
  uint64_t accessIndex;
  if (!ToIndex(cx, requestIndex, &accessIndex)) {
    return false;
  }

  // Steps 3-5.
  if (accessIndex >= length) {
    return ReportOutOfRange(cx);
  }

  // Step 6.
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
  if (typedArray->hasDetachedBuffer()) {
    ReportDetachedArrayBuffer(cx);
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

// Represents one waiting worker.
//
// The type is declared opaque in SharedArrayObject.h.  Instances of
// js::FutexWaiter are stack-allocated and linked onto a list across a
// call to FutexThread::wait().
//
// The 'waiters' field of the SharedArrayRawBuffer points to the highest
// priority waiter in the list, and lower priority nodes are linked through
// the 'lower_pri' field.  The 'back' field goes the other direction.
// The list is circular, so the 'lower_pri' field of the lowest priority
// node points to the first node in the list.  The list has no dedicated
// header node.

class FutexWaiter {
 public:
  FutexWaiter(size_t offset, JSContext* cx)
      : offset(offset), cx(cx), lower_pri(nullptr), back(nullptr) {}

  size_t offset;           // int32 element index within the SharedArrayBuffer
  JSContext* cx;           // The waiting thread
  FutexWaiter* lower_pri;  // Lower priority nodes in circular doubly-linked
                           // list of waiters
  FutexWaiter* back;       // Other direction
};

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

}  // namespace js

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.11 Atomics.wait ( typedArray, index, value, timeout ), steps 8-9, 14-25.
template <typename T>
static FutexThread::WaitResult AtomicsWait(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, T value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout) {
  // Validation and other guards should ensure that this does not happen.
  MOZ_ASSERT(sarb, "wait is only applicable to shared memory");

  // Steps 8-9.
  if (!cx->fx.canWait()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ATOMICS_WAIT_NOT_ALLOWED);
    return FutexThread::WaitResult::Error;
  }

  SharedMem<T*> addr =
      sarb->dataPointerShared().cast<T*>() + (byteOffset / sizeof(T));

  // Steps 15 (reordered), 17.a and 23 (through destructor).
  // This lock also protects the "waiters" field on SharedArrayRawBuffer,
  // and it provides the necessary memory fence.
  AutoLockFutexAPI lock;

  // Steps 16-17.
  if (jit::AtomicOperations::loadSafeWhenRacy(addr) != value) {
    return FutexThread::WaitResult::NotEqual;
  }

  // Steps 14, 18-22.
  FutexWaiter w(byteOffset, cx);
  if (FutexWaiter* waiters = sarb->waiters()) {
    w.lower_pri = waiters;
    w.back = waiters->back;
    waiters->back->lower_pri = &w;
    waiters->back = &w;
  } else {
    w.lower_pri = w.back = &w;
    sarb->setWaiters(&w);
  }

  FutexThread::WaitResult retval = cx->fx.wait(cx, lock.unique(), timeout);

  if (w.lower_pri == &w) {
    sarb->setWaiters(nullptr);
  } else {
    w.lower_pri->back = w.back;
    w.back->lower_pri = w.lower_pri;
    if (sarb->waiters() == &w) {
      sarb->setWaiters(w.lower_pri);
    }
  }

  // Steps 24-25.
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

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.11 Atomics.wait ( typedArray, index, value, timeout ), steps 6-25.
template <typename T>
static bool DoAtomicsWait(JSContext* cx,
                          Handle<TypedArrayObject*> unwrappedTypedArray,
                          size_t index, T value, HandleValue timeoutv,
                          MutableHandleValue r) {
  mozilla::Maybe<mozilla::TimeDuration> timeout;
  if (!timeoutv.isUndefined()) {
    // Step 6.
    double timeout_ms;
    if (!ToNumber(cx, timeoutv, &timeout_ms)) {
      return false;
    }

    // Step 7.
    if (!mozilla::IsNaN(timeout_ms)) {
      if (timeout_ms < 0) {
        timeout = mozilla::Some(mozilla::TimeDuration::FromSeconds(0.0));
      } else if (!mozilla::IsInfinite(timeout_ms)) {
        timeout =
            mozilla::Some(mozilla::TimeDuration::FromMilliseconds(timeout_ms));
      }
    }
  }

  // Step 10.
  Rooted<SharedArrayBufferObject*> unwrappedSab(
      cx, unwrappedTypedArray->bufferShared());

  // Step 11.
  size_t offset = unwrappedTypedArray->byteOffset();

  // Steps 12-13.
  // The computation will not overflow because range checks have been
  // performed.
  size_t indexedPosition = index * sizeof(T) + offset;

  // Steps 8-9, 14-25.
  switch (atomics_wait_impl(cx, unwrappedSab->rawBufferObject(),
                            indexedPosition, value, timeout)) {
    case FutexThread::WaitResult::NotEqual:
      r.setString(cx->names().futexNotEqual);
      return true;
    case FutexThread::WaitResult::OK:
      r.setString(cx->names().futexOK);
      return true;
    case FutexThread::WaitResult::TimedOut:
      r.setString(cx->names().futexTimedOut);
      return true;
    case FutexThread::WaitResult::Error:
      return false;
    default:
      MOZ_CRASH("Should not happen");
  }
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.11 Atomics.wait ( typedArray, index, value, timeout )
static bool atomics_wait(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue objv = args.get(0);
  HandleValue index = args.get(1);
  HandleValue valv = args.get(2);
  HandleValue timeoutv = args.get(3);
  MutableHandleValue r = args.rval();

  // Step 1.
  Rooted<TypedArrayObject*> unwrappedTypedArray(cx);
  if (!ValidateIntegerTypedArray(cx, objv, true, &unwrappedTypedArray)) {
    return false;
  }
  MOZ_ASSERT(unwrappedTypedArray->type() == Scalar::Int32 ||
             unwrappedTypedArray->type() == Scalar::BigInt64);

  // https://github.com/tc39/ecma262/pull/1908
  if (!unwrappedTypedArray->isSharedMemory()) {
    return ReportBadArrayType(cx);
  }

  // Step 2.
  size_t intIndex;
  if (!ValidateAtomicAccess(cx, unwrappedTypedArray, index, &intIndex)) {
    return false;
  }

  if (unwrappedTypedArray->type() == Scalar::Int32) {
    // Step 5.
    int32_t value;
    if (!ToInt32(cx, valv, &value)) {
      return false;
    }

    // Steps 6-25.
    return DoAtomicsWait(cx, unwrappedTypedArray, intIndex, value, timeoutv, r);
  }

  MOZ_ASSERT(unwrappedTypedArray->type() == Scalar::BigInt64);

  // Step 4.
  RootedBigInt value(cx, ToBigInt(cx, valv));
  if (!value) {
    return false;
  }

  // Steps 6-25.
  return DoAtomicsWait(cx, unwrappedTypedArray, intIndex,
                       BigInt::toInt64(value), timeoutv, r);
}

// ES2021 draft rev bd868f20b8c574ad6689fba014b62a1dba819e56
// 24.4.12 Atomics.notify ( typedArray, index, count ), steps 10-16.
int64_t js::atomics_notify_impl(SharedArrayRawBuffer* sarb, size_t byteOffset,
                                int64_t count) {
  // Validation should ensure this does not happen.
  MOZ_ASSERT(sarb, "notify is only applicable to shared memory");

  // Steps 12 (reordered), 15 (through destructor).
  AutoLockFutexAPI lock;

  // Step 11 (reordered).
  int64_t woken = 0;

  // Steps 10, 13-14.
  FutexWaiter* waiters = sarb->waiters();
  if (waiters && count) {
    FutexWaiter* iter = waiters;
    do {
      FutexWaiter* c = iter;
      iter = iter->lower_pri;
      if (c->offset != byteOffset || !c->cx->fx.isWaiting()) {
        continue;
      }
      c->cx->fx.notify(FutexThread::NotifyExplicit);
      // Overflow will be a problem only in two cases:
      // (1) 128-bit systems with substantially more than 2^64 bytes of
      //     memory per process, and a very lightweight
      //     Atomics.waitAsync().  Obviously a future problem.
      // (2) Bugs.
      MOZ_RELEASE_ASSERT(woken < INT64_MAX);
      ++woken;
      if (count > 0) {
        --count;
      }
    } while (count && iter != waiters);
  }

  // Step 16.
  return woken;
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
  size_t offset = unwrappedTypedArray->byteOffset();

  // Steps 7-9.
  // The computation will not overflow because range checks have been
  // performed.
  size_t elementSize = Scalar::byteSize(unwrappedTypedArray->type());
  size_t indexedPosition = intIndex * elementSize + offset;

  // Steps 10-16.
  r.setNumber(double(atomics_notify_impl(unwrappedSab->rawBufferObject(),
                                         indexedPosition, count)));

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
    UnlockGuard<Mutex> unlock(locked);
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
          UnlockGuard<Mutex> unlock(locked);
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
    JS_FN("notify", atomics_notify, 3, 0),
    JS_FN("wake", atomics_notify, 3, 0),  // Legacy name
    JS_FS_END};

static const JSPropertySpec AtomicsProperties[] = {
    JS_STRING_SYM_PS(toStringTag, "Atomics", JSPROP_READONLY), JS_PS_END};

static JSObject* CreateAtomicsObject(JSContext* cx, JSProtoKey key) {
  Handle<GlobalObject*> global = cx->global();
  RootedObject proto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
  if (!proto) {
    return nullptr;
  }
  return NewTenuredObjectWithGivenProto(cx, &AtomicsObject::class_, proto);
}

static const ClassSpec AtomicsClassSpec = {CreateAtomicsObject, nullptr,
                                           AtomicsMethods, AtomicsProperties};

const JSClass AtomicsObject::class_ = {
    "Atomics", JSCLASS_HAS_CACHED_PROTO(JSProto_Atomics), JS_NULL_CLASS_OPS,
    &AtomicsClassSpec};
