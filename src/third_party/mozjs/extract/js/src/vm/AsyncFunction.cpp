/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/AsyncFunction.h"

#include "mozilla/Maybe.h"

#include "builtin/ModuleObject.h"
#include "builtin/Promise.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/GeneratorObject.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/NativeObject.h"
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/Realm.h"
#include "vm/SelfHosting.h"

#include "vm/JSObject-inl.h"

using namespace js;

using mozilla::Maybe;

static JSObject* CreateAsyncFunction(JSContext* cx, JSProtoKey key) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateFunctionConstructor(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  HandlePropertyName name = cx->names().AsyncFunction;
  return NewFunctionWithProto(cx, AsyncFunctionConstructor, 1,
                              FunctionFlags::NATIVE_CTOR, nullptr, name, proto,
                              gc::AllocKind::FUNCTION, TenuredObject);
}

static JSObject* CreateAsyncFunctionPrototype(JSContext* cx, JSProtoKey key) {
  return NewTenuredObjectWithFunctionPrototype(cx, cx->global());
}

static bool AsyncFunctionClassFinish(JSContext* cx, HandleObject asyncFunction,
                                     HandleObject asyncFunctionProto) {
  // Change the "constructor" property to non-writable before adding any other
  // properties, so it's still the last property and can be modified without a
  // dictionary-mode transition.
  MOZ_ASSERT(asyncFunctionProto->as<NativeObject>().getLastProperty().key() ==
             NameToId(cx->names().constructor));
  MOZ_ASSERT(!asyncFunctionProto->as<NativeObject>().inDictionaryMode());

  RootedValue asyncFunctionVal(cx, ObjectValue(*asyncFunction));
  if (!DefineDataProperty(cx, asyncFunctionProto, cx->names().constructor,
                          asyncFunctionVal, JSPROP_READONLY)) {
    return false;
  }
  MOZ_ASSERT(!asyncFunctionProto->as<NativeObject>().inDictionaryMode());

  return DefineToStringTag(cx, asyncFunctionProto, cx->names().AsyncFunction);
}

static const ClassSpec AsyncFunctionClassSpec = {
    CreateAsyncFunction,
    CreateAsyncFunctionPrototype,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    AsyncFunctionClassFinish,
    ClassSpec::DontDefineConstructor};

const JSClass js::AsyncFunctionClass = {"AsyncFunction", 0, JS_NULL_CLASS_OPS,
                                        &AsyncFunctionClassSpec};

enum class ResumeKind { Normal, Throw };

// ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
// 6.2.3.1.1 Await Fulfilled Functions
// 6.2.3.1.2 Await Rejected Functions
static bool AsyncFunctionResume(JSContext* cx,
                                Handle<AsyncFunctionGeneratorObject*> generator,
                                ResumeKind kind, HandleValue valueOrReason) {
  // We're enqueuing the promise job for Await before suspending the execution
  // of the async function. So when either the debugger or OOM errors terminate
  // the execution after JSOp::AsyncAwait, but before JSOp::Await, we're in an
  // inconsistent state, because we don't have a resume index set and therefore
  // don't know where to resume the async function. Return here in that case.
  if (generator->isClosed()) {
    return true;
  }

  // The debugger sets the async function's generator object into the "running"
  // state while firing debugger events to ensure the debugger can't re-enter
  // the async function, cf. |AutoSetGeneratorRunning| in Debugger.cpp. Catch
  // this case here by checking if the generator is already runnning.
  if (generator->isRunning()) {
    return true;
  }

  Rooted<PromiseObject*> resultPromise(cx, generator->promise());

  RootedObject stack(cx);
  Maybe<JS::AutoSetAsyncStackForNewCalls> asyncStack;
  if (JSObject* allocationSite = resultPromise->allocationSite()) {
    // The promise is created within the activation of the async function, so
    // use the parent frame as the starting point for async stacks.
    stack = allocationSite->as<SavedFrame>().getParent();
    if (stack) {
      asyncStack.emplace(
          cx, stack, "async",
          JS::AutoSetAsyncStackForNewCalls::AsyncCallKind::EXPLICIT);
    }
  }

  MOZ_ASSERT(generator->isSuspended(),
             "non-suspended generator when resuming async function");

  // Execution context switching is handled in generator.
  HandlePropertyName funName = kind == ResumeKind::Normal
                                   ? cx->names().AsyncFunctionNext
                                   : cx->names().AsyncFunctionThrow;
  FixedInvokeArgs<1> args(cx);
  args[0].set(valueOrReason);
  RootedValue generatorOrValue(cx, ObjectValue(*generator));
  if (!CallSelfHostedFunction(cx, funName, generatorOrValue, args,
                              &generatorOrValue)) {
    if (!generator->isClosed()) {
      generator->setClosed();
    }

    // Handle the OOM case mentioned above.
    if (resultPromise->state() == JS::PromiseState::Pending &&
        cx->isExceptionPending()) {
      RootedValue exn(cx);
      if (!GetAndClearException(cx, &exn)) {
        return false;
      }
      return AsyncFunctionThrown(cx, resultPromise, exn);
    }
    return false;
  }

  MOZ_ASSERT_IF(generator->isClosed(), generatorOrValue.isObject());
  MOZ_ASSERT_IF(generator->isClosed(),
                &generatorOrValue.toObject() == resultPromise);
  MOZ_ASSERT_IF(!generator->isClosed(), generator->isAfterAwait());

  return true;
}

// ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
// 6.2.3.1.1 Await Fulfilled Functions
[[nodiscard]] bool js::AsyncFunctionAwaitedFulfilled(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> generator,
    HandleValue value) {
  return AsyncFunctionResume(cx, generator, ResumeKind::Normal, value);
}

// ES2020 draft rev a09fc232c137800dbf51b6204f37fdede4ba1646
// 6.2.3.1.2 Await Rejected Functions
[[nodiscard]] bool js::AsyncFunctionAwaitedRejected(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> generator,
    HandleValue reason) {
  return AsyncFunctionResume(cx, generator, ResumeKind::Throw, reason);
}

JSObject* js::AsyncFunctionResolve(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> generator,
    HandleValue valueOrReason, AsyncFunctionResolveKind resolveKind) {
  Rooted<PromiseObject*> promise(cx, generator->promise());
  if (resolveKind == AsyncFunctionResolveKind::Fulfill) {
    if (!AsyncFunctionReturned(cx, promise, valueOrReason)) {
      return nullptr;
    }
  } else {
    if (!AsyncFunctionThrown(cx, promise, valueOrReason)) {
      return nullptr;
    }
  }
  return promise;
}

const JSClass AsyncFunctionGeneratorObject::class_ = {
    "AsyncFunctionGenerator",
    JSCLASS_HAS_RESERVED_SLOTS(AsyncFunctionGeneratorObject::RESERVED_SLOTS),
    &classOps_,
};

const JSClassOps AsyncFunctionGeneratorObject::classOps_ = {
    nullptr,                                   // addProperty
    nullptr,                                   // delProperty
    nullptr,                                   // enumerate
    nullptr,                                   // newEnumerate
    nullptr,                                   // resolve
    nullptr,                                   // mayResolve
    nullptr,                                   // finalize
    nullptr,                                   // call
    nullptr,                                   // hasInstance
    nullptr,                                   // construct
    CallTraceMethod<AbstractGeneratorObject>,  // trace
};

AsyncFunctionGeneratorObject* AsyncFunctionGeneratorObject::create(
    JSContext* cx, HandleFunction fun) {
  MOZ_ASSERT(fun->isAsync() && !fun->isGenerator());

  Rooted<PromiseObject*> resultPromise(cx, CreatePromiseObjectForAsync(cx));
  if (!resultPromise) {
    return nullptr;
  }

  auto* obj = NewBuiltinClassInstance<AsyncFunctionGeneratorObject>(cx);
  if (!obj) {
    return nullptr;
  }
  obj->initFixedSlot(PROMISE_SLOT, ObjectValue(*resultPromise));

  // Starts in the running state.
  obj->setResumeIndex(AbstractGeneratorObject::RESUME_INDEX_RUNNING);

  return obj;
}

JSFunction* NewHandler(JSContext* cx, Native handler,
                       JS::Handle<JSObject*> target) {
  cx->check(target);

  JS::Handle<PropertyName*> funName = cx->names().empty;
  JS::Rooted<JSFunction*> handlerFun(
      cx, NewNativeFunction(cx, handler, 0, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!handlerFun) {
    return nullptr;
  }
  handlerFun->setExtendedSlot(FunctionExtended::MODULE_SLOT,
                              JS::ObjectValue(*target));
  return handlerFun;
}

AsyncFunctionGeneratorObject* AsyncFunctionGeneratorObject::create(
    JSContext* cx, HandleModuleObject module) {
  // TODO: Module is currently hitching a ride with
  // AsyncFunctionGeneratorObject. The reason for this is we have some work in
  // the JITs that make use of this object when we hit AsyncAwait bytecode. At
  // the same time, top level await shares a lot of it's implementation with
  // AsyncFunction. I am not sure if the best thing to do here is inherit,
  // override, or do something else. Comments appreciated.
  MOZ_ASSERT(module->script()->isAsync());

  Rooted<PromiseObject*> resultPromise(cx, CreatePromiseObjectForAsync(cx));
  if (!resultPromise) {
    return nullptr;
  }

  Rooted<AsyncFunctionGeneratorObject*> obj(
      cx, NewBuiltinClassInstance<AsyncFunctionGeneratorObject>(cx));
  if (!obj) {
    return nullptr;
  }
  obj->initFixedSlot(PROMISE_SLOT, ObjectValue(*resultPromise));

  RootedObject onFulfilled(
      cx, NewHandler(cx, AsyncModuleExecutionFulfilledHandler, module));
  if (!onFulfilled) {
    return nullptr;
  }

  RootedObject onRejected(
      cx, NewHandler(cx, AsyncModuleExecutionRejectedHandler, module));
  if (!onRejected) {
    return nullptr;
  }

  if (!JS::AddPromiseReactionsIgnoringUnhandledRejection(
          cx, resultPromise, onFulfilled, onRejected)) {
    return nullptr;
  }

  // Starts in the running state.
  obj->setResumeIndex(AbstractGeneratorObject::RESUME_INDEX_RUNNING);

  return obj;
}
