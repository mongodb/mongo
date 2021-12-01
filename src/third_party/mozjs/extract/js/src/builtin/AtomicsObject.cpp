/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS Atomics pseudo-module.
 *
 * See "Spec: JavaScript Shared Memory, Atomics, and Locks" for the
 * full specification.
 *
 * In addition to what is specified there, we throw an Error object if
 * the futex API hooks have not been installed on the runtime.
 * Essentially that is an implementation error at a higher level.
 *
 *
 * Note on the current implementation of atomic operations.
 *
 * The Mozilla atomics are not sufficient to implement these APIs
 * because we need to support 8-bit, 16-bit, and 32-bit data: the
 * Mozilla atomics only support 32-bit data.
 *
 * At the moment we include mozilla/Atomics.h, which will define
 * MOZ_HAVE_CXX11_ATOMICS and include <atomic> if we have C++11
 * atomics.
 *
 * If MOZ_HAVE_CXX11_ATOMICS is set we'll use C++11 atomics.
 *
 * Otherwise, if the compiler has them we'll fall back on gcc/Clang
 * intrinsics.
 *
 * Otherwise, if we're on VC++2012, we'll use C++11 atomics even if
 * MOZ_HAVE_CXX11_ATOMICS is not defined.  The compiler has the
 * atomics but they are disabled in Mozilla due to a performance bug.
 * That performance bug does not affect the Atomics code.  See
 * mozilla/Atomics.h for further comments on that bug.
 *
 * Otherwise, if we're on VC++2010 or VC++2008, we'll emulate the
 * gcc/Clang intrinsics with simple code below using the VC++
 * intrinsics, like the VC++2012 solution this is a stopgap since
 * we're about to start using VC++2013 anyway.
 *
 * If none of those options are available then the build must disable
 * shared memory, or compilation will fail with a predictable error.
 */

#include "builtin/AtomicsObject.h"

#include "mozilla/Atomics.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Unused.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsnum.h"

#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "js/Class.h"
#include "vm/GlobalObject.h"
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmInstance.h"

#include "vm/JSObject-inl.h"

using namespace js;

const Class AtomicsObject::class_ = {
    "Atomics",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Atomics)
};

static bool
ReportBadArrayType(JSContext* cx)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_ATOMICS_BAD_ARRAY);
    return false;
}

static bool
ReportOutOfRange(JSContext* cx)
{
    // Use JSMSG_BAD_INDEX here, it is what ToIndex uses for some cases that it
    // reports directly.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
    return false;
}

static bool
GetSharedTypedArray(JSContext* cx, HandleValue v,
                    MutableHandle<TypedArrayObject*> viewp)
{
    if (!v.isObject())
        return ReportBadArrayType(cx);
    if (!v.toObject().is<TypedArrayObject>())
        return ReportBadArrayType(cx);
    viewp.set(&v.toObject().as<TypedArrayObject>());
    if (!viewp->isSharedMemory())
        return ReportBadArrayType(cx);
    return true;
}

static bool
GetTypedArrayIndex(JSContext* cx, HandleValue v, Handle<TypedArrayObject*> view, uint32_t* offset)
{
    uint64_t index;
    if (!ToIndex(cx, v, &index))
        return false;
    if (index >= view->length())
        return ReportOutOfRange(cx);
    *offset = uint32_t(index);
    return true;
}

static int32_t
CompareExchange(Scalar::Type viewType, int32_t oldCandidate, int32_t newCandidate,
                SharedMem<void*> viewData, uint32_t offset, bool* badArrayType = nullptr)
{
    switch (viewType) {
      case Scalar::Int8: {
        int8_t oldval = (int8_t)oldCandidate;
        int8_t newval = (int8_t)newCandidate;
        oldval = jit::AtomicOperations::compareExchangeSeqCst(viewData.cast<int8_t*>() + offset,
                                                              oldval, newval);
        return oldval;
      }
      case Scalar::Uint8: {
        uint8_t oldval = (uint8_t)oldCandidate;
        uint8_t newval = (uint8_t)newCandidate;
        oldval = jit::AtomicOperations::compareExchangeSeqCst(viewData.cast<uint8_t*>() + offset,
                                                              oldval, newval);
        return oldval;
      }
      case Scalar::Int16: {
        int16_t oldval = (int16_t)oldCandidate;
        int16_t newval = (int16_t)newCandidate;
        oldval = jit::AtomicOperations::compareExchangeSeqCst(viewData.cast<int16_t*>() + offset,
                                                              oldval, newval);
        return oldval;
      }
      case Scalar::Uint16: {
        uint16_t oldval = (uint16_t)oldCandidate;
        uint16_t newval = (uint16_t)newCandidate;
        oldval = jit::AtomicOperations::compareExchangeSeqCst(viewData.cast<uint16_t*>() + offset,
                                                              oldval, newval);
        return oldval;
      }
      case Scalar::Int32: {
        int32_t oldval = oldCandidate;
        int32_t newval = newCandidate;
        oldval = jit::AtomicOperations::compareExchangeSeqCst(viewData.cast<int32_t*>() + offset,
                                                              oldval, newval);
        return oldval;
      }
      case Scalar::Uint32: {
        uint32_t oldval = (uint32_t)oldCandidate;
        uint32_t newval = (uint32_t)newCandidate;
        oldval = jit::AtomicOperations::compareExchangeSeqCst(viewData.cast<uint32_t*>() + offset,
                                                              oldval, newval);
        return (int32_t)oldval;
      }
      default:
        if (badArrayType)
            *badArrayType = true;
        return 0;
    }
}

bool
js::atomics_compareExchange(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue objv = args.get(0);
    HandleValue idxv = args.get(1);
    HandleValue oldv = args.get(2);
    HandleValue newv = args.get(3);
    MutableHandleValue r = args.rval();

    Rooted<TypedArrayObject*> view(cx, nullptr);
    if (!GetSharedTypedArray(cx, objv, &view))
        return false;
    uint32_t offset;
    if (!GetTypedArrayIndex(cx, idxv, view, &offset))
        return false;
    int32_t oldCandidate;
    if (!ToInt32(cx, oldv, &oldCandidate))
        return false;
    int32_t newCandidate;
    if (!ToInt32(cx, newv, &newCandidate))
        return false;

    bool badType = false;
    int32_t result = CompareExchange(view->type(), oldCandidate, newCandidate,
                                     view->viewDataShared(), offset, &badType);

    if (badType)
        return ReportBadArrayType(cx);

    if (view->type() == Scalar::Uint32)
        r.setNumber((double)(uint32_t)result);
    else
        r.setInt32(result);
    return true;
}

bool
js::atomics_load(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue objv = args.get(0);
    HandleValue idxv = args.get(1);
    MutableHandleValue r = args.rval();

    Rooted<TypedArrayObject*> view(cx, nullptr);
    if (!GetSharedTypedArray(cx, objv, &view))
        return false;
    uint32_t offset;
    if (!GetTypedArrayIndex(cx, idxv, view, &offset))
        return false;

    SharedMem<void*> viewData = view->viewDataShared();
    switch (view->type()) {
      case Scalar::Uint8: {
        uint8_t v = jit::AtomicOperations::loadSeqCst(viewData.cast<uint8_t*>() + offset);
        r.setInt32(v);
        return true;
      }
      case Scalar::Int8: {
        int8_t v = jit::AtomicOperations::loadSeqCst(viewData.cast<uint8_t*>() + offset);
        r.setInt32(v);
        return true;
      }
      case Scalar::Int16: {
        int16_t v = jit::AtomicOperations::loadSeqCst(viewData.cast<int16_t*>() + offset);
        r.setInt32(v);
        return true;
      }
      case Scalar::Uint16: {
        uint16_t v = jit::AtomicOperations::loadSeqCst(viewData.cast<uint16_t*>() + offset);
        r.setInt32(v);
        return true;
      }
      case Scalar::Int32: {
        int32_t v = jit::AtomicOperations::loadSeqCst(viewData.cast<int32_t*>() + offset);
        r.setInt32(v);
        return true;
      }
      case Scalar::Uint32: {
        uint32_t v = jit::AtomicOperations::loadSeqCst(viewData.cast<uint32_t*>() + offset);
        r.setNumber(v);
        return true;
      }
      default:
        return ReportBadArrayType(cx);
    }
}

enum XchgStoreOp {
    DoExchange,
    DoStore
};

template<XchgStoreOp op>
static int32_t
ExchangeOrStore(Scalar::Type viewType, int32_t numberValue, SharedMem<void*> viewData,
                uint32_t offset, bool* badArrayType = nullptr)
{
#define INT_OP(ptr, value)                                         \
    JS_BEGIN_MACRO                                                 \
    if (op == DoStore)                                             \
        jit::AtomicOperations::storeSeqCst(ptr, value);            \
    else                                                           \
        value = jit::AtomicOperations::exchangeSeqCst(ptr, value); \
    JS_END_MACRO

    switch (viewType) {
      case Scalar::Int8: {
        int8_t value = (int8_t)numberValue;
        INT_OP(viewData.cast<int8_t*>() + offset, value);
        return value;
      }
      case Scalar::Uint8: {
        uint8_t value = (uint8_t)numberValue;
        INT_OP(viewData.cast<uint8_t*>() + offset, value);
        return value;
      }
      case Scalar::Int16: {
        int16_t value = (int16_t)numberValue;
        INT_OP(viewData.cast<int16_t*>() + offset, value);
        return value;
      }
      case Scalar::Uint16: {
        uint16_t value = (uint16_t)numberValue;
        INT_OP(viewData.cast<uint16_t*>() + offset, value);
        return value;
      }
      case Scalar::Int32: {
        int32_t value = numberValue;
        INT_OP(viewData.cast<int32_t*>() + offset, value);
        return value;
      }
      case Scalar::Uint32: {
        uint32_t value = (uint32_t)numberValue;
        INT_OP(viewData.cast<uint32_t*>() + offset, value);
        return (int32_t)value;
      }
      default:
        if (badArrayType)
            *badArrayType = true;
        return 0;
    }
#undef INT_OP
}

template<XchgStoreOp op>
static bool
ExchangeOrStore(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue objv = args.get(0);
    HandleValue idxv = args.get(1);
    HandleValue valv = args.get(2);
    MutableHandleValue r = args.rval();

    Rooted<TypedArrayObject*> view(cx, nullptr);
    if (!GetSharedTypedArray(cx, objv, &view))
        return false;
    uint32_t offset;
    if (!GetTypedArrayIndex(cx, idxv, view, &offset))
        return false;
    double integerValue;
    if (!ToInteger(cx, valv, &integerValue))
        return false;

    bool badType = false;
    int32_t result = ExchangeOrStore<op>(view->type(), JS::ToInt32(integerValue),
                                         view->viewDataShared(), offset, &badType);

    if (badType)
        return ReportBadArrayType(cx);

    if (op == DoStore)
        r.setNumber(integerValue);
    else if (view->type() == Scalar::Uint32)
        r.setNumber((double)(uint32_t)result);
    else
        r.setInt32(result);
    return true;
}

bool
js::atomics_store(JSContext* cx, unsigned argc, Value* vp)
{
    return ExchangeOrStore<DoStore>(cx, argc, vp);
}

bool
js::atomics_exchange(JSContext* cx, unsigned argc, Value* vp)
{
    return ExchangeOrStore<DoExchange>(cx, argc, vp);
}

template<typename T>
static bool
AtomicsBinop(JSContext* cx, HandleValue objv, HandleValue idxv, HandleValue valv,
             MutableHandleValue r)
{
    Rooted<TypedArrayObject*> view(cx, nullptr);
    if (!GetSharedTypedArray(cx, objv, &view))
        return false;
    uint32_t offset;
    if (!GetTypedArrayIndex(cx, idxv, view, &offset))
        return false;
    int32_t numberValue;
    if (!ToInt32(cx, valv, &numberValue))
        return false;

    SharedMem<void*> viewData = view->viewDataShared();
    switch (view->type()) {
      case Scalar::Int8: {
        int8_t v = (int8_t)numberValue;
        r.setInt32(T::operate(viewData.cast<int8_t*>() + offset, v));
        return true;
      }
      case Scalar::Uint8: {
        uint8_t v = (uint8_t)numberValue;
        r.setInt32(T::operate(viewData.cast<uint8_t*>() + offset, v));
        return true;
      }
      case Scalar::Int16: {
        int16_t v = (int16_t)numberValue;
        r.setInt32(T::operate(viewData.cast<int16_t*>() + offset, v));
        return true;
      }
      case Scalar::Uint16: {
        uint16_t v = (uint16_t)numberValue;
        r.setInt32(T::operate(viewData.cast<uint16_t*>() + offset, v));
        return true;
      }
      case Scalar::Int32: {
        int32_t v = numberValue;
        r.setInt32(T::operate(viewData.cast<int32_t*>() + offset, v));
        return true;
      }
      case Scalar::Uint32: {
        uint32_t v = (uint32_t)numberValue;
        r.setNumber((double)T::operate(viewData.cast<uint32_t*>() + offset, v));
        return true;
      }
      default:
        return ReportBadArrayType(cx);
    }
}

#define INTEGRAL_TYPES_FOR_EACH(NAME) \
    static int8_t operate(SharedMem<int8_t*> addr, int8_t v) { return NAME(addr, v); } \
    static uint8_t operate(SharedMem<uint8_t*> addr, uint8_t v) { return NAME(addr, v); } \
    static int16_t operate(SharedMem<int16_t*> addr, int16_t v) { return NAME(addr, v); } \
    static uint16_t operate(SharedMem<uint16_t*> addr, uint16_t v) { return NAME(addr, v); } \
    static int32_t operate(SharedMem<int32_t*> addr, int32_t v) { return NAME(addr, v); } \
    static uint32_t operate(SharedMem<uint32_t*> addr, uint32_t v) { return NAME(addr, v); }

class PerformAdd
{
public:
    INTEGRAL_TYPES_FOR_EACH(jit::AtomicOperations::fetchAddSeqCst)
    static int32_t perform(int32_t x, int32_t y) { return x + y; }
};

bool
js::atomics_add(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return AtomicsBinop<PerformAdd>(cx, args.get(0), args.get(1), args.get(2), args.rval());
}

class PerformSub
{
public:
    INTEGRAL_TYPES_FOR_EACH(jit::AtomicOperations::fetchSubSeqCst)
    static int32_t perform(int32_t x, int32_t y) { return x - y; }
};

bool
js::atomics_sub(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return AtomicsBinop<PerformSub>(cx, args.get(0), args.get(1), args.get(2), args.rval());
}

class PerformAnd
{
public:
    INTEGRAL_TYPES_FOR_EACH(jit::AtomicOperations::fetchAndSeqCst)
    static int32_t perform(int32_t x, int32_t y) { return x & y; }
};

bool
js::atomics_and(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return AtomicsBinop<PerformAnd>(cx, args.get(0), args.get(1), args.get(2), args.rval());
}

class PerformOr
{
public:
    INTEGRAL_TYPES_FOR_EACH(jit::AtomicOperations::fetchOrSeqCst)
    static int32_t perform(int32_t x, int32_t y) { return x | y; }
};

bool
js::atomics_or(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return AtomicsBinop<PerformOr>(cx, args.get(0), args.get(1), args.get(2), args.rval());
}

class PerformXor
{
public:
    INTEGRAL_TYPES_FOR_EACH(jit::AtomicOperations::fetchXorSeqCst)
    static int32_t perform(int32_t x, int32_t y) { return x ^ y; }
};

bool
js::atomics_xor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return AtomicsBinop<PerformXor>(cx, args.get(0), args.get(1), args.get(2), args.rval());
}

bool
js::atomics_isLockFree(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue v = args.get(0);
    int32_t size;
    if (v.isInt32()) {
        size = v.toInt32();
    } else {
        double dsize;
        if (!ToInteger(cx, v, &dsize))
            return false;
        if (!mozilla::NumberIsInt32(dsize, &size)) {
            args.rval().setBoolean(false);
            return true;
        }
    }
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

class FutexWaiter
{
  public:
    FutexWaiter(uint32_t offset, JSContext* cx)
      : offset(offset),
        cx(cx),
        lower_pri(nullptr),
        back(nullptr)
    {
    }

    uint32_t    offset;                 // int32 element index within the SharedArrayBuffer
    JSContext* cx;                      // The waiting thread
    FutexWaiter* lower_pri;             // Lower priority nodes in circular doubly-linked list of waiters
    FutexWaiter* back;                  // Other direction
};

class AutoLockFutexAPI
{
    // We have to wrap this in a Maybe because of the way loading
    // mozilla::Atomic pointers works.
    mozilla::Maybe<js::UniqueLock<js::Mutex>> unique_;

  public:
    AutoLockFutexAPI() {
        js::Mutex* lock = FutexThread::lock_;
        unique_.emplace(*lock);
    }

    ~AutoLockFutexAPI() {
        unique_.reset();
    }

    js::UniqueLock<js::Mutex>& unique() { return *unique_; }
};

} // namespace js

template<typename T>
static FutexThread::WaitResult
AtomicsWait(JSContext* cx, SharedArrayRawBuffer* sarb, uint32_t byteOffset, T value,
            const mozilla::Maybe<mozilla::TimeDuration>& timeout)
{
    // Validation and other guards should ensure that this does not happen.
    MOZ_ASSERT(sarb, "wait is only applicable to shared memory");

    if (!cx->fx.canWait()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_ATOMICS_WAIT_NOT_ALLOWED);
        return FutexThread::WaitResult::Error;
    }

    SharedMem<T*> addr = sarb->dataPointerShared().cast<T*>() + (byteOffset / sizeof(T));

    // This lock also protects the "waiters" field on SharedArrayRawBuffer,
    // and it provides the necessary memory fence.
    AutoLockFutexAPI lock;

    if (jit::AtomicOperations::loadSafeWhenRacy(addr) != value)
        return FutexThread::WaitResult::NotEqual;

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
        if (sarb->waiters() == &w)
            sarb->setWaiters(w.lower_pri);
    }

    return retval;
}

FutexThread::WaitResult
js::atomics_wait_impl(JSContext* cx, SharedArrayRawBuffer* sarb, uint32_t byteOffset,
                      int32_t value, const mozilla::Maybe<mozilla::TimeDuration>& timeout)
{
    return AtomicsWait(cx, sarb, byteOffset, value, timeout);
}

FutexThread::WaitResult
js::atomics_wait_impl(JSContext* cx, SharedArrayRawBuffer* sarb, uint32_t byteOffset,
                      int64_t value, const mozilla::Maybe<mozilla::TimeDuration>& timeout)
{
    return AtomicsWait(cx, sarb, byteOffset, value, timeout);
}

bool
js::atomics_wait(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue objv = args.get(0);
    HandleValue idxv = args.get(1);
    HandleValue valv = args.get(2);
    HandleValue timeoutv = args.get(3);
    MutableHandleValue r = args.rval();

    Rooted<TypedArrayObject*> view(cx, nullptr);
    if (!GetSharedTypedArray(cx, objv, &view))
        return false;
    if (view->type() != Scalar::Int32)
        return ReportBadArrayType(cx);
    uint32_t offset;
    if (!GetTypedArrayIndex(cx, idxv, view, &offset))
        return false;
    int32_t value;
    if (!ToInt32(cx, valv, &value))
        return false;
    mozilla::Maybe<mozilla::TimeDuration> timeout;
    if (!timeoutv.isUndefined()) {
        double timeout_ms;
        if (!ToNumber(cx, timeoutv, &timeout_ms))
            return false;
        if (!mozilla::IsNaN(timeout_ms)) {
            if (timeout_ms < 0)
                timeout = mozilla::Some(mozilla::TimeDuration::FromSeconds(0.0));
            else if (!mozilla::IsInfinite(timeout_ms))
                timeout = mozilla::Some(mozilla::TimeDuration::FromMilliseconds(timeout_ms));
        }
    }

    Rooted<SharedArrayBufferObject*> sab(cx, view->bufferShared());
    // The computation will not overflow because range checks have been
    // performed.
    uint32_t byteOffset = offset * sizeof(int32_t) +
                          (view->viewDataShared().cast<uint8_t*>().unwrap(/* arithmetic */) -
                           sab->dataPointerShared().unwrap(/* arithmetic */));

    switch (atomics_wait_impl(cx, sab->rawBufferObject(), byteOffset, value, timeout)) {
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

int64_t
js::atomics_wake_impl(SharedArrayRawBuffer* sarb, uint32_t byteOffset, int64_t count)
{
    // Validation should ensure this does not happen.
    MOZ_ASSERT(sarb, "wake is only applicable to shared memory");

    AutoLockFutexAPI lock;

    int64_t woken = 0;

    FutexWaiter* waiters = sarb->waiters();
    if (waiters && count) {
        FutexWaiter* iter = waiters;
        do {
            FutexWaiter* c = iter;
            iter = iter->lower_pri;
            if (c->offset != byteOffset || !c->cx->fx.isWaiting())
                continue;
            c->cx->fx.wake(FutexThread::WakeExplicit);
            // Overflow will be a problem only in two cases:
            // (1) 128-bit systems with substantially more than 2^64 bytes of
            //     memory per process, and a very lightweight
            //     Atomics.waitAsync().  Obviously a future problem.
            // (2) Bugs.
            MOZ_RELEASE_ASSERT(woken < INT64_MAX);
            ++woken;
            if (count > 0)
                --count;
        } while (count && iter != waiters);
    }

    return woken;
}

bool
js::atomics_wake(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue objv = args.get(0);
    HandleValue idxv = args.get(1);
    HandleValue countv = args.get(2);
    MutableHandleValue r = args.rval();

    Rooted<TypedArrayObject*> view(cx, nullptr);
    if (!GetSharedTypedArray(cx, objv, &view))
        return false;
    if (view->type() != Scalar::Int32)
        return ReportBadArrayType(cx);
    uint32_t offset;
    if (!GetTypedArrayIndex(cx, idxv, view, &offset))
        return false;
    int64_t count;
    if (countv.isUndefined()) {
        count = -1;
    } else {
        double dcount;
        if (!ToInteger(cx, countv, &dcount))
            return false;
        if (dcount < 0.0)
            dcount = 0.0;
        count = dcount > INT64_MAX ? -1 : int64_t(dcount);
    }

    Rooted<SharedArrayBufferObject*> sab(cx, view->bufferShared());
    // The computation will not overflow because range checks have been
    // performed.
    uint32_t byteOffset = offset * sizeof(int32_t) +
                          (view->viewDataShared().cast<uint8_t*>().unwrap(/* arithmetic */) -
                           sab->dataPointerShared().unwrap(/* arithmetic */));

    r.setNumber(double(atomics_wake_impl(sab->rawBufferObject(), byteOffset, count)));

    return true;
}

/* static */ bool
js::FutexThread::initialize()
{
    MOZ_ASSERT(!lock_);
    lock_ = js_new<js::Mutex>(mutexid::FutexThread);
    return lock_ != nullptr;
}

/* static */ void
js::FutexThread::destroy()
{
    if (lock_) {
        js::Mutex* lock = lock_;
        js_delete(lock);
        lock_ = nullptr;
    }
}

/* static */ void
js::FutexThread::lock()
{
    // Load the atomic pointer.
    js::Mutex* lock = lock_;

    lock->lock();
}

/* static */ mozilla::Atomic<js::Mutex*> FutexThread::lock_;

/* static */ void
js::FutexThread::unlock()
{
    // Load the atomic pointer.
    js::Mutex* lock = lock_;

    lock->unlock();
}

js::FutexThread::FutexThread()
  : cond_(nullptr),
    state_(Idle),
    canWait_(false)
{
}

bool
js::FutexThread::initInstance()
{
    MOZ_ASSERT(lock_);
    cond_ = js_new<js::ConditionVariable>();
    return cond_ != nullptr;
}

void
js::FutexThread::destroyInstance()
{
    if (cond_)
        js_delete(cond_);
}

bool
js::FutexThread::isWaiting()
{
    // When a worker is awoken for an interrupt it goes into state
    // WaitingNotifiedForInterrupt for a short time before it actually
    // wakes up and goes into WaitingInterrupted.  In those states the
    // worker is still waiting, and if an explicit wake arrives the
    // worker transitions to Woken.  See further comments in
    // FutexThread::wait().
    return state_ == Waiting || state_ == WaitingInterrupted || state_ == WaitingNotifiedForInterrupt;
}

FutexThread::WaitResult
js::FutexThread::wait(JSContext* cx, js::UniqueLock<js::Mutex>& locked,
                      const mozilla::Maybe<mozilla::TimeDuration>& timeout)
{
    MOZ_ASSERT(&cx->fx == this);
    MOZ_ASSERT(cx->fx.canWait());
    MOZ_ASSERT(state_ == Idle || state_ == WaitingInterrupted);

    // Disallow waiting when a runtime is processing an interrupt.
    // See explanation below.

    if (state_ == WaitingInterrupted) {
        UnlockGuard<Mutex> unlock(locked);
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_ATOMICS_WAIT_NOT_ALLOWED);
        return WaitResult::Error;
    }

    // Go back to Idle after returning.
    auto onFinish = mozilla::MakeScopeExit([&] {
        state_ = Idle;
    });

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
            if (finalEnd < sliceEnd)
                sliceEnd = finalEnd;
            return sliceEnd;
        });

        state_ = Waiting;

        if (isTimed) {
            mozilla::Unused << cond_->wait_until(locked, *sliceEnd);
        } else {
            cond_->wait(locked);
        }

        switch (state_) {
          case FutexThread::Waiting:
            // Timeout or spurious wakeup.
            if (isTimed) {
                auto now = mozilla::TimeStamp::Now();
                if (now >= *finalEnd)
                    return WaitResult::TimedOut;
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
            //   handler returns.  A wake() call that reaches the
            //   runtime during the interrupt sets state_ to Woken.
            //
            // - It is in principle possible for wait() to be
            //   reentered on the same thread/runtime and waiting on the
            //   same location and to yet again be interrupted and enter
            //   the interrupt handler.  In this case, it is important
            //   that when another agent wakes waiters, all waiters using
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
                if (!cx->handleInterrupt())
                    return WaitResult::Error;
            }
            if (state_ == Woken)
                return WaitResult::OK;
            break;

          default:
            MOZ_CRASH("Bad FutexState in wait()");
        }
    }
}

void
js::FutexThread::wake(WakeReason reason)
{
    MOZ_ASSERT(isWaiting());

    if ((state_ == WaitingInterrupted || state_ == WaitingNotifiedForInterrupt) && reason == WakeExplicit) {
        state_ = Woken;
        return;
    }
    switch (reason) {
      case WakeExplicit:
        state_ = Woken;
        break;
      case WakeForJSInterrupt:
        if (state_ == WaitingNotifiedForInterrupt)
            return;
        state_ = WaitingNotifiedForInterrupt;
        break;
      default:
        MOZ_CRASH("bad WakeReason in FutexThread::wake()");
    }
    cond_->notify_all();
}

const JSFunctionSpec AtomicsMethods[] = {
    JS_INLINABLE_FN("compareExchange",    atomics_compareExchange,    4,0, AtomicsCompareExchange),
    JS_INLINABLE_FN("load",               atomics_load,               2,0, AtomicsLoad),
    JS_INLINABLE_FN("store",              atomics_store,              3,0, AtomicsStore),
    JS_INLINABLE_FN("exchange",           atomics_exchange,           3,0, AtomicsExchange),
    JS_INLINABLE_FN("add",                atomics_add,                3,0, AtomicsAdd),
    JS_INLINABLE_FN("sub",                atomics_sub,                3,0, AtomicsSub),
    JS_INLINABLE_FN("and",                atomics_and,                3,0, AtomicsAnd),
    JS_INLINABLE_FN("or",                 atomics_or,                 3,0, AtomicsOr),
    JS_INLINABLE_FN("xor",                atomics_xor,                3,0, AtomicsXor),
    JS_INLINABLE_FN("isLockFree",         atomics_isLockFree,         1,0, AtomicsIsLockFree),
    JS_FN("wait",                         atomics_wait,               4,0),
    JS_FN("wake",                         atomics_wake,               3,0),
    JS_FS_END
};

JSObject*
AtomicsObject::initClass(JSContext* cx, Handle<GlobalObject*> global)
{
    // Create Atomics Object.
    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!objProto)
        return nullptr;
    RootedObject Atomics(cx, NewObjectWithGivenProto(cx, &AtomicsObject::class_, objProto,
                                                     SingletonObject));
    if (!Atomics)
        return nullptr;

    if (!JS_DefineFunctions(cx, Atomics, AtomicsMethods))
        return nullptr;
    if (!DefineToStringTag(cx, Atomics, cx->names().Atomics))
        return nullptr;

    RootedValue AtomicsValue(cx, ObjectValue(*Atomics));

    // Everything is set up, install Atomics on the global object.
    if (!DefineDataProperty(cx, global, cx->names().Atomics, AtomicsValue, JSPROP_RESOLVING))
        return nullptr;

    global->setConstructor(JSProto_Atomics, AtomicsValue);
    return Atomics;
}

JSObject*
js::InitAtomicsClass(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->is<GlobalObject>());
    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    return AtomicsObject::initClass(cx, global);
}

#undef CXX11_ATOMICS
#undef GNU_ATOMICS
