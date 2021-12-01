/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/AsyncFunction.h"

#include "mozilla/Maybe.h"

#include "builtin/Promise.h"
#include "vm/GeneratorObject.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSCompartment.h"
#include "vm/SelfHosting.h"

using namespace js;
using namespace js::gc;

using mozilla::Maybe;

/* static */ bool
GlobalObject::initAsyncFunction(JSContext* cx, Handle<GlobalObject*> global)
{
    if (global->getReservedSlot(ASYNC_FUNCTION_PROTO).isObject())
        return true;

    RootedObject asyncFunctionProto(cx, NewSingletonObjectWithFunctionPrototype(cx, global));
    if (!asyncFunctionProto)
        return false;

    if (!DefineToStringTag(cx, asyncFunctionProto, cx->names().AsyncFunction))
        return false;

    RootedValue function(cx, global->getConstructor(JSProto_Function));
    if (!function.toObjectOrNull())
        return false;
    RootedObject proto(cx, &function.toObject());
    RootedAtom name(cx, cx->names().AsyncFunction);
    RootedObject asyncFunction(cx, NewFunctionWithProto(cx, AsyncFunctionConstructor, 1,
                                                        JSFunction::NATIVE_CTOR, nullptr, name,
                                                        proto));
    if (!asyncFunction)
        return false;
    if (!LinkConstructorAndPrototype(cx, asyncFunction, asyncFunctionProto,
                                     JSPROP_PERMANENT | JSPROP_READONLY, JSPROP_READONLY))
    {
        return false;
    }

    global->setReservedSlot(ASYNC_FUNCTION, ObjectValue(*asyncFunction));
    global->setReservedSlot(ASYNC_FUNCTION_PROTO, ObjectValue(*asyncFunctionProto));
    return true;
}

static MOZ_MUST_USE bool AsyncFunctionStart(JSContext* cx, Handle<PromiseObject*> resultPromise,
                                            HandleValue generatorVal);

#define UNWRAPPED_ASYNC_WRAPPED_SLOT 1
#define WRAPPED_ASYNC_UNWRAPPED_SLOT 0

// Async Functions proposal 1.1.8 and 1.2.14.
static bool
WrappedAsyncFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedFunction wrapped(cx, &args.callee().as<JSFunction>());
    RootedValue unwrappedVal(cx, wrapped->getExtendedSlot(WRAPPED_ASYNC_UNWRAPPED_SLOT));
    RootedFunction unwrapped(cx, &unwrappedVal.toObject().as<JSFunction>());
    RootedValue thisValue(cx, args.thisv());

    // Step 2.
    // Also does a part of 2.2 steps 1-2.
    RootedValue generatorVal(cx);
    InvokeArgs args2(cx);
    if (!args2.init(cx, argc))
        return false;
    for (size_t i = 0, len = argc; i < len; i++)
        args2[i].set(args[i]);
    if (Call(cx, unwrappedVal, thisValue, args2, &generatorVal)) {
        // Step 1.
        Rooted<PromiseObject*> resultPromise(cx, CreatePromiseObjectForAsync(cx, generatorVal));
        if (!resultPromise)
            return false;

        // Step 3.
        if (!AsyncFunctionStart(cx, resultPromise, generatorVal))
            return false;

        // Step 5.
        args.rval().setObject(*resultPromise);
        return true;
    }

    if (!cx->isExceptionPending())
        return false;

    // Steps 1, 4.
    RootedValue exc(cx);
    if (!GetAndClearException(cx, &exc))
        return false;
    RootedObject rejectPromise(cx, PromiseObject::unforgeableReject(cx, exc));
    if (!rejectPromise)
        return false;

    // Step 5.
    args.rval().setObject(*rejectPromise);
    return true;
}

// Async Functions proposal 2.1 steps 1, 3 (partially).
// In the spec it creates a function, but we create 2 functions `unwrapped` and
// `wrapped`.  `unwrapped` is a generator that corresponds to
//  the async function's body, replacing `await` with `yield`.  `wrapped` is a
// function that is visible to the outside, and handles yielded values.
JSObject*
js::WrapAsyncFunctionWithProto(JSContext* cx, HandleFunction unwrapped, HandleObject proto)
{
    MOZ_ASSERT(unwrapped->isAsync());
    MOZ_ASSERT(proto, "We need an explicit prototype to avoid the default"
                      "%FunctionPrototype% fallback in NewFunctionWithProto().");

    // Create a new function with AsyncFunctionPrototype, reusing the name and
    // the length of `unwrapped`.

    RootedAtom funName(cx, unwrapped->explicitName());
    uint16_t length;
    if (!JSFunction::getLength(cx, unwrapped, &length))
        return nullptr;

    // Steps 3 (partially).
    RootedFunction wrapped(cx, NewFunctionWithProto(cx, WrappedAsyncFunction, length,
                                                    JSFunction::NATIVE_FUN, nullptr,
                                                    funName, proto,
                                                    AllocKind::FUNCTION_EXTENDED));
    if (!wrapped)
        return nullptr;

    if (unwrapped->hasCompileTimeName())
        wrapped->setCompileTimeName(unwrapped->compileTimeName());

    // Link them to each other to make GetWrappedAsyncFunction and
    // GetUnwrappedAsyncFunction work.
    unwrapped->setExtendedSlot(UNWRAPPED_ASYNC_WRAPPED_SLOT, ObjectValue(*wrapped));
    wrapped->setExtendedSlot(WRAPPED_ASYNC_UNWRAPPED_SLOT, ObjectValue(*unwrapped));

    return wrapped;
}

JSObject*
js::WrapAsyncFunction(JSContext* cx, HandleFunction unwrapped)
{
    RootedObject proto(cx, GlobalObject::getOrCreateAsyncFunctionPrototype(cx, cx->global()));
    if (!proto)
        return nullptr;

    return WrapAsyncFunctionWithProto(cx, unwrapped, proto);
}

enum class ResumeKind {
    Normal,
    Throw
};

// Async Functions proposal 2.2 steps 3.f, 3.g.
// Async Functions proposal 2.2 steps 3.d-e, 3.g.
// Implemented in js/src/builtin/Promise.cpp

// Async Functions proposal 2.2 steps 3-8, 2.4 steps 2-7, 2.5 steps 2-7.
static bool
AsyncFunctionResume(JSContext* cx, Handle<PromiseObject*> resultPromise, HandleValue generatorVal,
                    ResumeKind kind, HandleValue valueOrReason)
{
    RootedObject stack(cx, resultPromise->allocationSite());
    Maybe<JS::AutoSetAsyncStackForNewCalls> asyncStack;
    if (stack) {
        asyncStack.emplace(cx, stack, "async",
                           JS::AutoSetAsyncStackForNewCalls::AsyncCallKind::EXPLICIT);
    }

    // Execution context switching is handled in generator.
    HandlePropertyName funName = kind == ResumeKind::Normal
                                 ? cx->names().GeneratorNext
                                 : cx->names().GeneratorThrow;
    FixedInvokeArgs<1> args(cx);
    args[0].set(valueOrReason);
    RootedValue value(cx);
    if (!CallSelfHostedFunction(cx, funName, generatorVal, args, &value))
        return AsyncFunctionThrown(cx, resultPromise);

    if (generatorVal.toObject().as<GeneratorObject>().isAfterAwait())
        return AsyncFunctionAwait(cx, resultPromise, value);

    return AsyncFunctionReturned(cx, resultPromise, value);
}

// Async Functions proposal 2.2 steps 3-8.
static MOZ_MUST_USE bool
AsyncFunctionStart(JSContext* cx, Handle<PromiseObject*> resultPromise, HandleValue generatorVal)
{
    return AsyncFunctionResume(cx, resultPromise, generatorVal, ResumeKind::Normal, UndefinedHandleValue);
}

// Async Functions proposal 2.3 steps 1-8.
// Implemented in js/src/builtin/Promise.cpp

// Async Functions proposal 2.4.
MOZ_MUST_USE bool
js::AsyncFunctionAwaitedFulfilled(JSContext* cx, Handle<PromiseObject*> resultPromise,
                                  HandleValue generatorVal, HandleValue value)
{
    // Step 1 (implicit).

    // Steps 2-7.
    return AsyncFunctionResume(cx, resultPromise, generatorVal, ResumeKind::Normal, value);
}

// Async Functions proposal 2.5.
MOZ_MUST_USE bool
js::AsyncFunctionAwaitedRejected(JSContext* cx, Handle<PromiseObject*> resultPromise,
                                 HandleValue generatorVal, HandleValue reason)
{
    // Step 1 (implicit).

    // Step 2-7.
    return AsyncFunctionResume(cx, resultPromise, generatorVal, ResumeKind::Throw, reason);
}

JSFunction*
js::GetWrappedAsyncFunction(JSFunction* unwrapped)
{
    MOZ_ASSERT(unwrapped->isAsync());
    return &unwrapped->getExtendedSlot(UNWRAPPED_ASYNC_WRAPPED_SLOT).toObject().as<JSFunction>();
}

JSFunction*
js::GetUnwrappedAsyncFunction(JSFunction* wrapped)
{
    MOZ_ASSERT(IsWrappedAsyncFunction(wrapped));
    JSFunction* unwrapped = &wrapped->getExtendedSlot(WRAPPED_ASYNC_UNWRAPPED_SLOT).toObject().as<JSFunction>();
    MOZ_ASSERT(unwrapped->isAsync());
    return unwrapped;
}

bool
js::IsWrappedAsyncFunction(JSFunction* fun)
{
    return fun->maybeNative() == WrappedAsyncFunction;
}
