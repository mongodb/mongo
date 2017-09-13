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
#include "mozilla/FloatingPoint.h"

#include "jsapi.h"
#include "jsfriendapi.h"

#include "asmjs/AsmJSModule.h"
#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "js/Class.h"
#include "vm/GlobalObject.h"
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"

#include "jsobjinlines.h"

using namespace js;

const Class AtomicsObject::class_ = {
    "Atomics",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Atomics)
};

static bool
ReportBadArrayType(JSContext* cx)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_ATOMICS_BAD_ARRAY);
    return false;
}

static bool
ReportOutOfRange(JSContext* cx)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_ATOMICS_BAD_INDEX);
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
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, v, &id))
        return false;
    uint64_t index;
    if (!IsTypedArrayIndex(id, &index) || index >= view->length())
        return ReportOutOfRange(cx);
    *offset = (uint32_t)index;
    return true;
}

bool
js::atomics_fence(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    jit::AtomicOperations::fenceSeqCst();
    args.rval().setUndefined();
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
    int32_t numberValue;
    if (!ToInt32(cx, valv, &numberValue))
        return false;

    bool badType = false;
    int32_t result = ExchangeOrStore<op>(view->type(), numberValue, view->viewDataShared(), offset,
                                         &badType);

    if (badType)
        return ReportBadArrayType(cx);

    if (view->type() == Scalar::Uint32)
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
    if (!v.isInt32()) {
        args.rval().setBoolean(false);
        return true;
    }
    args.rval().setBoolean(jit::AtomicOperations::isLockfree(v.toInt32()));
    return true;
}

// asm.js callouts for platforms that do not have non-word-sized
// atomics where we don't want to inline the logic for the atomics.
//
// Memory will always be shared since the callouts are only called from
// code that checks that the memory is shared.
//
// To test this, either run on eg Raspberry Pi Model 1, or invoke the ARM
// simulator build with ARMHWCAP=vfp set.  Do not set any other flags; other
// vfp/neon flags force ARMv7 to be set.

static void
GetCurrentAsmJSHeap(SharedMem<void*>* heap, size_t* length)
{
    JSRuntime* rt = js::TlsPerThreadData.get()->runtimeFromMainThread();
    AsmJSModule& mod = rt->asmJSActivationStack()->module();
    *heap = mod.maybeHeap().cast<void*>();
    *length = mod.heapLength();
}

int32_t
js::atomics_add_asm_callout(int32_t vt, int32_t offset, int32_t value)
{
    SharedMem<void*> heap;
    size_t heapLength;
    GetCurrentAsmJSHeap(&heap, &heapLength);
    if (size_t(offset) >= heapLength)
        return 0;
    switch (Scalar::Type(vt)) {
      case Scalar::Int8:
        return PerformAdd::operate(heap.cast<int8_t*>() + offset, value);
      case Scalar::Uint8:
        return PerformAdd::operate(heap.cast<uint8_t*>() + offset, value);
      case Scalar::Int16:
        return PerformAdd::operate(heap.cast<int16_t*>() + (offset >> 1), value);
      case Scalar::Uint16:
        return PerformAdd::operate(heap.cast<uint16_t*>() + (offset >> 1), value);
      default:
        MOZ_CRASH("Invalid size");
    }
}

int32_t
js::atomics_sub_asm_callout(int32_t vt, int32_t offset, int32_t value)
{
    SharedMem<void*> heap;
    size_t heapLength;
    GetCurrentAsmJSHeap(&heap, &heapLength);
    if (size_t(offset) >= heapLength)
        return 0;
    switch (Scalar::Type(vt)) {
      case Scalar::Int8:
        return PerformSub::operate(heap.cast<int8_t*>() + offset, value);
      case Scalar::Uint8:
        return PerformSub::operate(heap.cast<uint8_t*>() + offset, value);
      case Scalar::Int16:
        return PerformSub::operate(heap.cast<int16_t*>() + (offset >> 1), value);
      case Scalar::Uint16:
        return PerformSub::operate(heap.cast<uint16_t*>() + (offset >> 1), value);
      default:
        MOZ_CRASH("Invalid size");
    }
}

int32_t
js::atomics_and_asm_callout(int32_t vt, int32_t offset, int32_t value)
{
    SharedMem<void*> heap;
    size_t heapLength;
    GetCurrentAsmJSHeap(&heap, &heapLength);
    if (size_t(offset) >= heapLength)
        return 0;
    switch (Scalar::Type(vt)) {
      case Scalar::Int8:
        return PerformAnd::operate(heap.cast<int8_t*>() + offset, value);
      case Scalar::Uint8:
        return PerformAnd::operate(heap.cast<uint8_t*>() + offset, value);
      case Scalar::Int16:
        return PerformAnd::operate(heap.cast<int16_t*>() + (offset >> 1), value);
      case Scalar::Uint16:
        return PerformAnd::operate(heap.cast<uint16_t*>() + (offset >> 1), value);
      default:
        MOZ_CRASH("Invalid size");
    }
}

int32_t
js::atomics_or_asm_callout(int32_t vt, int32_t offset, int32_t value)
{
    SharedMem<void*> heap;
    size_t heapLength;
    GetCurrentAsmJSHeap(&heap, &heapLength);
    if (size_t(offset) >= heapLength)
        return 0;
    switch (Scalar::Type(vt)) {
      case Scalar::Int8:
        return PerformOr::operate(heap.cast<int8_t*>() + offset, value);
      case Scalar::Uint8:
        return PerformOr::operate(heap.cast<uint8_t*>() + offset, value);
      case Scalar::Int16:
        return PerformOr::operate(heap.cast<int16_t*>() + (offset >> 1), value);
      case Scalar::Uint16:
        return PerformOr::operate(heap.cast<uint16_t*>() + (offset >> 1), value);
      default:
        MOZ_CRASH("Invalid size");
    }
}

int32_t
js::atomics_xor_asm_callout(int32_t vt, int32_t offset, int32_t value)
{
    SharedMem<void*> heap;
    size_t heapLength;
    GetCurrentAsmJSHeap(&heap, &heapLength);
    if (size_t(offset) >= heapLength)
        return 0;
    switch (Scalar::Type(vt)) {
      case Scalar::Int8:
        return PerformXor::operate(heap.cast<int8_t*>() + offset, value);
      case Scalar::Uint8:
        return PerformXor::operate(heap.cast<uint8_t*>() + offset, value);
      case Scalar::Int16:
        return PerformXor::operate(heap.cast<int16_t*>() + (offset >> 1), value);
      case Scalar::Uint16:
        return PerformXor::operate(heap.cast<uint16_t*>() + (offset >> 1), value);
      default:
        MOZ_CRASH("Invalid size");
    }
}

int32_t
js::atomics_xchg_asm_callout(int32_t vt, int32_t offset, int32_t value)
{
    SharedMem<void*> heap;
    size_t heapLength;
    GetCurrentAsmJSHeap(&heap, &heapLength);
    if (size_t(offset) >= heapLength)
        return 0;
    switch (Scalar::Type(vt)) {
      case Scalar::Int8:
        return ExchangeOrStore<DoExchange>(Scalar::Int8, value, heap, offset);
      case Scalar::Uint8:
        return ExchangeOrStore<DoExchange>(Scalar::Uint8, value, heap, offset);
      case Scalar::Int16:
        return ExchangeOrStore<DoExchange>(Scalar::Int16, value, heap, offset>>1);
      case Scalar::Uint16:
        return ExchangeOrStore<DoExchange>(Scalar::Uint16, value, heap, offset>>1);
      default:
        MOZ_CRASH("Invalid size");
    }
}

int32_t
js::atomics_cmpxchg_asm_callout(int32_t vt, int32_t offset, int32_t oldval, int32_t newval)
{
    SharedMem<void*> heap;
    size_t heapLength;
    GetCurrentAsmJSHeap(&heap, &heapLength);
    if (size_t(offset) >= heapLength)
        return 0;
    switch (Scalar::Type(vt)) {
      case Scalar::Int8:
        return CompareExchange(Scalar::Int8, oldval, newval, heap, offset);
      case Scalar::Uint8:
        return CompareExchange(Scalar::Uint8, oldval, newval, heap, offset);
      case Scalar::Int16:
        return CompareExchange(Scalar::Int16, oldval, newval, heap, offset>>1);
      case Scalar::Uint16:
        return CompareExchange(Scalar::Uint16, oldval, newval, heap, offset>>1);
      default:
        MOZ_CRASH("Invalid size");
    }
}

namespace js {

// Represents one waiting worker.
//
// The type is declared opaque in SharedArrayObject.h.  Instances of
// js::FutexWaiter are stack-allocated and linked onto a list across a
// call to FutexRuntime::wait().
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
    FutexWaiter(uint32_t offset, JSRuntime* rt)
      : offset(offset),
        rt(rt),
        lower_pri(nullptr),
        back(nullptr)
    {
    }

    uint32_t    offset;                 // int32 element index within the SharedArrayBuffer
    JSRuntime*  rt;                    // The runtime of the waiter
    FutexWaiter* lower_pri;             // Lower priority nodes in circular doubly-linked list of waiters
    FutexWaiter* back;                  // Other direction
};

class AutoLockFutexAPI
{
  public:
    AutoLockFutexAPI() {
        FutexRuntime::lock();
    }
    ~AutoLockFutexAPI() {
        FutexRuntime::unlock();
    }
};

class AutoUnlockFutexAPI
{
  public:
    AutoUnlockFutexAPI() {
        FutexRuntime::unlock();
    }
    ~AutoUnlockFutexAPI() {
        FutexRuntime::lock();
    }
};

} // namespace js

bool
js::atomics_futexWait(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue objv = args.get(0);
    HandleValue idxv = args.get(1);
    HandleValue valv = args.get(2);
    HandleValue timeoutv = args.get(3);
    MutableHandleValue r = args.rval();

    JSRuntime* rt = cx->runtime();

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
    double timeout_ms;
    if (timeoutv.isUndefined()) {
        timeout_ms = mozilla::PositiveInfinity<double>();
    } else {
        if (!ToNumber(cx, timeoutv, &timeout_ms))
            return false;
        if (mozilla::IsNaN(timeout_ms))
            timeout_ms = mozilla::PositiveInfinity<double>();
        else if (timeout_ms < 0)
            timeout_ms = 0;
    }

    // This lock also protects the "waiters" field on SharedArrayRawBuffer,
    // and it provides the necessary memory fence.
    AutoLockFutexAPI lock;

    SharedMem<int32_t*>(addr) = view->viewDataShared().cast<int32_t*>() + offset;
    if (jit::AtomicOperations::loadSafeWhenRacy(addr) != value) {
        r.setInt32(AtomicsObject::FutexNotequal);
        return true;
    }

    Rooted<SharedArrayBufferObject*> sab(cx, view->bufferShared());
    SharedArrayRawBuffer* sarb = sab->rawBufferObject();

    FutexWaiter w(offset, rt);
    if (FutexWaiter* waiters = sarb->waiters()) {
        w.lower_pri = waiters;
        w.back = waiters->back;
        waiters->back->lower_pri = &w;
        waiters->back = &w;
    } else {
        w.lower_pri = w.back = &w;
        sarb->setWaiters(&w);
    }

    AtomicsObject::FutexWaitResult result = AtomicsObject::FutexOK;
    bool retval = rt->fx.wait(cx, timeout_ms, &result);
    if (retval)
        r.setInt32(result);

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

bool
js::atomics_futexWake(JSContext* cx, unsigned argc, Value* vp)
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
    double count;
    if (!ToInteger(cx, countv, &count))
        return false;
    if (count < 0)
        count = 0;

    AutoLockFutexAPI lock;

    Rooted<SharedArrayBufferObject*> sab(cx, view->bufferShared());
    SharedArrayRawBuffer* sarb = sab->rawBufferObject();
    int32_t woken = 0;

    FutexWaiter* waiters = sarb->waiters();
    if (waiters && count > 0) {
        FutexWaiter* iter = waiters;
        do {
            FutexWaiter* c = iter;
            iter = iter->lower_pri;
            if (c->offset != offset || !c->rt->fx.isWaiting())
                continue;
            c->rt->fx.wake(FutexRuntime::WakeExplicit);
            ++woken;
            --count;
        } while (count > 0 && iter != waiters);
    }

    r.setInt32(woken);
    return true;
}

bool
js::atomics_futexWakeOrRequeue(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue objv = args.get(0);
    HandleValue idx1v = args.get(1);
    HandleValue countv = args.get(2);
    HandleValue valv = args.get(3);
    HandleValue idx2v = args.get(4);
    MutableHandleValue r = args.rval();

    Rooted<TypedArrayObject*> view(cx, nullptr);
    if (!GetSharedTypedArray(cx, objv, &view))
        return false;
    if (view->type() != Scalar::Int32)
        return ReportBadArrayType(cx);
    uint32_t offset1;
    if (!GetTypedArrayIndex(cx, idx1v, view, &offset1))
        return false;
    double count;
    if (!ToInteger(cx, countv, &count))
        return false;
    if (count < 0)
        count = 0;
    int32_t value;
    if (!ToInt32(cx, valv, &value))
        return false;
    uint32_t offset2;
    if (!GetTypedArrayIndex(cx, idx2v, view, &offset2))
        return false;

    AutoLockFutexAPI lock;

    SharedMem<int32_t*> addr = view->viewDataShared().cast<int32_t*>() + offset1;
    if (jit::AtomicOperations::loadSafeWhenRacy(addr) != value) {
        r.setInt32(AtomicsObject::FutexNotequal);
        return true;
    }

    Rooted<SharedArrayBufferObject*> sab(cx, view->bufferShared());
    SharedArrayRawBuffer* sarb = sab->rawBufferObject();

    // Walk the list of waiters looking for those waiting on offset1.
    // Wake some and requeue the others.  There may already be other
    // waiters on offset2, so those that are requeued must be moved to
    // the back of the list.  Offset1 may equal offset2.  The list's
    // first node may change, and the list may be emptied out by the
    // operation.

    FutexWaiter* waiters = sarb->waiters();
    if (!waiters) {
        r.setInt32(0);
        return true;
    }

    int32_t woken = 0;
    FutexWaiter whead((uint32_t)-1, nullptr); // Header node for waiters
    FutexWaiter* first = waiters;
    FutexWaiter* last = waiters->back;
    whead.lower_pri = first;
    whead.back = last;
    first->back = &whead;
    last->lower_pri = &whead;

    FutexWaiter rhead((uint32_t)-1, nullptr); // Header node for requeued
    rhead.lower_pri = rhead.back = &rhead;

    FutexWaiter* iter = whead.lower_pri;
    while (iter != &whead) {
        FutexWaiter* c = iter;
        iter = iter->lower_pri;
        if (c->offset != offset1 || !c->rt->fx.isWaiting())
            continue;
        if (count > 0) {
            c->rt->fx.wake(FutexRuntime::WakeExplicit);
            ++woken;
            --count;
        } else {
            c->offset = offset2;

            // Remove the node from the waiters list.
            c->back->lower_pri = c->lower_pri;
            c->lower_pri->back = c->back;

            // Insert the node at the back of the requeuers list.
            c->lower_pri = &rhead;
            c->back = rhead.back;
            rhead.back->lower_pri = c;
            rhead.back = c;
        }
    }

    // If there are any requeuers, append them to the waiters.
    if (rhead.lower_pri != &rhead) {
        whead.back->lower_pri = rhead.lower_pri;
        rhead.lower_pri->back = whead.back;

        whead.back = rhead.back;
        rhead.back->lower_pri = &whead;
    }

    // Make the final list and install it.
    waiters = nullptr;
    if (whead.lower_pri != &whead) {
        whead.back->lower_pri = whead.lower_pri;
        whead.lower_pri->back = whead.back;
        waiters = whead.lower_pri;
    }
    sarb->setWaiters(waiters);

    r.setInt32(woken);
    return true;
}

/* static */ bool
js::FutexRuntime::initialize()
{
    MOZ_ASSERT(!lock_);
    lock_ = PR_NewLock();
    return lock_ != nullptr;
}

/* static */ void
js::FutexRuntime::destroy()
{
    if (lock_) {
        PR_DestroyLock(lock_);
        lock_ = nullptr;
    }
}

/* static */ void
js::FutexRuntime::lock()
{
    PR_Lock(lock_);
#ifdef DEBUG
    MOZ_ASSERT(!lockHolder_);
    lockHolder_ = PR_GetCurrentThread();
#endif
}

/* static */ mozilla::Atomic<PRLock*> FutexRuntime::lock_;

#ifdef DEBUG
/* static */ mozilla::Atomic<PRThread*> FutexRuntime::lockHolder_;
#endif

/* static */ void
js::FutexRuntime::unlock()
{
#ifdef DEBUG
    MOZ_ASSERT(lockHolder_ == PR_GetCurrentThread());
    lockHolder_ = nullptr;
#endif
    PR_Unlock(lock_);
}

js::FutexRuntime::FutexRuntime()
  : cond_(nullptr),
    state_(Idle)
{
}

bool
js::FutexRuntime::initInstance()
{
    MOZ_ASSERT(lock_);
    cond_ = PR_NewCondVar(lock_);
    return cond_ != nullptr;
}

void
js::FutexRuntime::destroyInstance()
{
    if (cond_)
        PR_DestroyCondVar(cond_);
}

bool
js::FutexRuntime::isWaiting()
{
    // When a worker is awoken for an interrupt it goes into state
    // WaitingNotifiedForInterrupt for a short time before it actually
    // wakes up and goes into WaitingInterrupted.  In those states the
    // worker is still waiting, and if an explicit wake arrives the
    // worker transitions to Woken.  See further comments in
    // FutexRuntime::wait().
    return state_ == Waiting || state_ == WaitingInterrupted || state_ == WaitingNotifiedForInterrupt;
}

bool
js::FutexRuntime::wait(JSContext* cx, double timeout_ms, AtomicsObject::FutexWaitResult* result)
{
    MOZ_ASSERT(&cx->runtime()->fx == this);
    MOZ_ASSERT(lockHolder_ == PR_GetCurrentThread());
    MOZ_ASSERT(state_ == Idle || state_ == WaitingInterrupted);

    // Disallow waiting when a runtime is processing an interrupt.
    // See explanation below.

    if (state_ == WaitingInterrupted) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_ATOMICS_WAIT_NOT_ALLOWED);
        return false;
    }

    const bool timed = !mozilla::IsInfinite(timeout_ms);

    // Reject the timeout if it is not exactly representable.  2e50 ms = 2e53 us = 6e39 years.

    if (timed && timeout_ms > 2e50) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_ATOMICS_TOO_LONG);
        return false;
    }

    // Times and intervals are in microseconds.

    const uint64_t finalEnd = timed ? PRMJ_Now() + (uint64_t)ceil(timeout_ms * 1000.0) : 0;

    // 4000s is about the longest timeout slice that is guaranteed to
    // work cross-platform.

    const uint64_t maxSlice = 4000000000LLU;
    bool retval = true;

    for (;;) {
        uint64_t sliceStart = 0;
        uint32_t timeout = PR_INTERVAL_NO_TIMEOUT;
        if (timed) {
            sliceStart = PRMJ_Now();
            uint64_t timeLeft = finalEnd > sliceStart ? finalEnd - sliceStart : 0;
            timeout = PR_MicrosecondsToInterval((uint32_t)Min(timeLeft, maxSlice));
        }
        state_ = Waiting;
#ifdef DEBUG
        PRThread* holder = lockHolder_;
        lockHolder_ = nullptr;
#endif
        JS_ALWAYS_TRUE(PR_WaitCondVar(cond_, timeout) == PR_SUCCESS);
#ifdef DEBUG
        lockHolder_ = holder;
#endif
        switch (state_) {
          case FutexRuntime::Waiting:
            // Timeout or spurious wakeup.
            if (timed) {
                uint64_t now = PRMJ_Now();
                if (now >= finalEnd) {
                    *result = AtomicsObject::FutexTimedout;
                    goto finished;
                }
            }
            break;

          case FutexRuntime::Woken:
            *result = AtomicsObject::FutexOK;
            goto finished;

          case FutexRuntime::WaitingNotifiedForInterrupt:
            // The interrupt handler may reenter the engine.  In that case
            // there are two complications:
            //
            // - The waiting thread is not actually waiting on the
            //   condition variable so we have to record that it
            //   should be woken when the interrupt handler returns.
            //   To that end, we flag the thread as interrupted around
            //   the interrupt and check state_ when the interrupt
            //   handler returns.  A futexWake() call that reaches the
            //   runtime during the interrupt sets state_ to Woken.
            //
            // - It is in principle possible for futexWait() to be
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
                AutoUnlockFutexAPI unlock;
                retval = cx->runtime()->handleInterrupt(cx);
            }
            if (!retval)
                goto finished;
            if (state_ == Woken) {
                *result = AtomicsObject::FutexOK;
                goto finished;
            }
            break;

          default:
            MOZ_CRASH();
        }
    }
finished:
    state_ = Idle;
    return retval;
}

void
js::FutexRuntime::wake(WakeReason reason)
{
    MOZ_ASSERT(lockHolder_ == PR_GetCurrentThread());
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
        MOZ_CRASH();
    }
    PR_NotifyCondVar(cond_);
}

const JSFunctionSpec AtomicsMethods[] = {
    JS_INLINABLE_FN("compareExchange",    atomics_compareExchange,    4,0, AtomicsCompareExchange),
    JS_INLINABLE_FN("load",               atomics_load,               2,0, AtomicsLoad),
    JS_INLINABLE_FN("store",              atomics_store,              3,0, AtomicsStore),
    JS_INLINABLE_FN("exchange",           atomics_exchange,           3,0, AtomicsExchange),
    JS_INLINABLE_FN("fence",              atomics_fence,              0,0, AtomicsFence),
    JS_INLINABLE_FN("add",                atomics_add,                3,0, AtomicsAdd),
    JS_INLINABLE_FN("sub",                atomics_sub,                3,0, AtomicsSub),
    JS_INLINABLE_FN("and",                atomics_and,                3,0, AtomicsAnd),
    JS_INLINABLE_FN("or",                 atomics_or,                 3,0, AtomicsOr),
    JS_INLINABLE_FN("xor",                atomics_xor,                3,0, AtomicsXor),
    JS_INLINABLE_FN("isLockFree",         atomics_isLockFree,         1,0, AtomicsIsLockFree),
    JS_FN("futexWait",                    atomics_futexWait,          4,0),
    JS_FN("futexWake",                    atomics_futexWake,          3,0),
    JS_FN("futexWakeOrRequeue",           atomics_futexWakeOrRequeue, 5,0),
    JS_FS_END
};

static const JSConstDoubleSpec AtomicsConstants[] = {
    {"OK",       AtomicsObject::FutexOK},
    {"TIMEDOUT", AtomicsObject::FutexTimedout},
    {"NOTEQUAL", AtomicsObject::FutexNotequal},
    {0,          0}
};

JSObject*
AtomicsObject::initClass(JSContext* cx, Handle<GlobalObject*> global)
{
    // Create Atomics Object.
    RootedObject objProto(cx, global->getOrCreateObjectPrototype(cx));
    if (!objProto)
        return nullptr;
    RootedObject Atomics(cx, NewObjectWithGivenProto(cx, &AtomicsObject::class_, objProto,
                                                     SingletonObject));
    if (!Atomics)
        return nullptr;

    if (!JS_DefineFunctions(cx, Atomics, AtomicsMethods))
        return nullptr;
    if (!JS_DefineConstDoubles(cx, Atomics, AtomicsConstants))
        return nullptr;

    RootedValue AtomicsValue(cx, ObjectValue(*Atomics));

    // Everything is set up, install Atomics on the global object.
    if (!DefineProperty(cx, global, cx->names().Atomics, AtomicsValue, nullptr, nullptr,
                        JSPROP_RESOLVING))
    {
        return nullptr;
    }

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
