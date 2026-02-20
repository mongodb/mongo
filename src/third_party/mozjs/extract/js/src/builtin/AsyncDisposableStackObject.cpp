/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/AsyncDisposableStackObject.h"

#include "vm/UsingHint.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

/* static */ AsyncDisposableStackObject* AsyncDisposableStackObject::create(
    JSContext* cx, JS::Handle<JSObject*> proto,
    JS::Handle<JS::Value>
        initialDisposeCapability /* = JS::UndefinedHandleValue */) {
  AsyncDisposableStackObject* obj =
      NewObjectWithClassProto<AsyncDisposableStackObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  MOZ_ASSERT(initialDisposeCapability.isUndefined() ||
             initialDisposeCapability.isObject());
  MOZ_ASSERT_IF(initialDisposeCapability.isObject(),
                initialDisposeCapability.toObject().is<ArrayObject>());

  obj->initReservedSlot(
      AsyncDisposableStackObject::DISPOSABLE_RESOURCE_STACK_SLOT,
      initialDisposeCapability);
  obj->initReservedSlot(
      AsyncDisposableStackObject::STATE_SLOT,
      JS::Int32Value(
          int32_t(AsyncDisposableStackObject::DisposableState::Pending)));

  return obj;
}

/**
 * Explicit Resource Management Proposal
 *
 * 27.4.1.1 AsyncDisposableStack ( )
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-asyncdisposablestack
 */
/* static */ bool AsyncDisposableStackObject::construct(JSContext* cx,
                                                        unsigned argc,
                                                        JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. If NewTarget is undefined, throw a TypeError exception.
  if (!ThrowIfNotConstructing(cx, args, "AsyncDisposableStack")) {
    return false;
  }

  // Step 2. Let asyncDisposableStack be ?
  // OrdinaryCreateFromConstructor(NewTarget,
  // "%AsyncDisposableStack.prototype%", « [[AsyncDisposableState]],
  // [[DisposeCapability]] »).
  // Step 3. Set asyncDisposableStack.[[AsyncDisposableState]] to pending.
  // Step 4. Set asyncDisposableStack.[[DisposeCapability]] to
  // NewDisposeCapability().
  JS::Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(
          cx, args, JSProto_AsyncDisposableStack, &proto)) {
    return false;
  }

  AsyncDisposableStackObject* obj =
      AsyncDisposableStackObject::create(cx, proto);
  if (!obj) {
    return false;
  }

  // Step 5. Return asyncDisposableStack.
  args.rval().setObject(*obj);
  return true;
}

/* static */ bool AsyncDisposableStackObject::is(JS::Handle<JS::Value> val) {
  return val.isObject() && val.toObject().is<AsyncDisposableStackObject>();
}

/**
 * Explicit Resource Management Proposal
 *
 * 27.4.3.6 AsyncDisposableStack.prototype.use ( value )
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-asyncdisposablestack.prototype.use
 */
/* static */ bool AsyncDisposableStackObject::use_impl(
    JSContext* cx, const JS::CallArgs& args) {
  // Step 1. Let asyncDisposableStack be the this value.
  JS::Rooted<AsyncDisposableStackObject*> asyncDisposableStack(
      cx, &args.thisv().toObject().as<AsyncDisposableStackObject>());

  // Step 2. Perform ? RequireInternalSlot(asyncDisposableStack,
  // [[AsyncDisposableState]]).
  // (done by caller)
  // Step 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw
  // a ReferenceError exception.
  if (asyncDisposableStack->state() == DisposableState::Disposed) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DISPOSABLE_STACK_DISPOSED);
    return false;
  }

  // Step 4. Perform ?
  // AddDisposableResource(asyncDisposableStack.[[DisposeCapability]], value,
  // async-dispose).
  JS::Rooted<ArrayObject*> disposeCapability(
      cx, GetOrCreateDisposeCapability(cx, asyncDisposableStack));
  if (!disposeCapability) {
    return false;
  }

  JS::Rooted<JS::Value> val(cx, args.get(0));
  if (!AddDisposableResource(cx, disposeCapability, val, UsingHint::Async)) {
    return false;
  }

  // Step 5. Return value.
  args.rval().set(val);
  return true;
}

/* static */ bool AsyncDisposableStackObject::use(JSContext* cx, unsigned argc,
                                                  JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);
  return JS::CallNonGenericMethod<is, use_impl>(cx, args);
}

/**
 * Explicit Resource Management Proposal
 *
 * 27.4.3.4 get AsyncDisposableStack.prototype.disposed
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-get-asyncdisposablestack.prototype.disposed
 */
/* static */ bool AsyncDisposableStackObject::disposed_impl(
    JSContext* cx, const JS::CallArgs& args) {
  // Step 1. Let disposableStack be the this value.
  JS::Rooted<AsyncDisposableStackObject*> disposableStack(
      cx, &args.thisv().toObject().as<AsyncDisposableStackObject>());

  // Step 2. Perform ? RequireInternalSlot(disposableStack,
  // [[DisposableState]]).
  // (done by caller)
  // Step 3. If disposableStack.[[DisposableState]] is disposed, return true.
  // Step 4. Otherwise, return false.
  args.rval().setBoolean(disposableStack->state() == DisposableState::Disposed);
  return true;
}

/* static */ bool AsyncDisposableStackObject::disposed(JSContext* cx,
                                                       unsigned argc,
                                                       JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);
  return JS::CallNonGenericMethod<is, disposed_impl>(cx, args);
}

/**
 * Explicit Resource Management Proposal
 *
 * 27.4.3.5 AsyncDisposableStack.prototype.move ( )
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-asyncdisposablestack.prototype.move
 */
/* static */ bool AsyncDisposableStackObject::move_impl(
    JSContext* cx, const JS::CallArgs& args) {
  // Step 1. Let asyncDisposableStack be the this value.
  JS::Rooted<AsyncDisposableStackObject*> asyncDisposableStack(
      cx, &args.thisv().toObject().as<AsyncDisposableStackObject>());

  // Step 2. Perform ? RequireInternalSlot(asyncDisposableStack,
  // [[AsyncDisposableState]]).
  // (done by caller)
  // Step 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw
  // a ReferenceError exception.
  if (asyncDisposableStack->state() == DisposableState::Disposed) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DISPOSABLE_STACK_DISPOSED);
    return false;
  }

  // Step 4. Let newAsyncDisposableStack be ?
  // OrdinaryCreateFromConstructor(%AsyncDisposableStack%,
  // "%AsyncDisposableStack.prototype%", « [[AsyncDisposableState]],
  // [[DisposeCapability]] »).
  // Step 5. Set newAsyncDisposableStack.[[AsyncDisposableState]] to pending.
  // Step 6. Set newAsyncDisposableStack.[[DisposeCapability]] to
  // asyncDisposableStack.[[DisposeCapability]].
  JS::Rooted<JS::Value> existingDisposeCapability(
      cx, asyncDisposableStack->getReservedSlot(
              AsyncDisposableStackObject::DISPOSABLE_RESOURCE_STACK_SLOT));
  AsyncDisposableStackObject* newAsyncDisposableStack =
      AsyncDisposableStackObject::create(cx, nullptr,
                                         existingDisposeCapability);
  if (!newAsyncDisposableStack) {
    return false;
  }

  // Step 7. Set asyncDisposableStack.[[DisposeCapability]] to
  // NewDisposeCapability().
  asyncDisposableStack->clearDisposableResourceStack();

  // Step 8. Set asyncDisposableStack.[[AsyncDisposableState]] to disposed.
  asyncDisposableStack->setState(DisposableState::Disposed);

  // Step 9. Return newAsyncDisposableStack.
  args.rval().setObject(*newAsyncDisposableStack);
  return true;
}

/* static */ bool AsyncDisposableStackObject::move(JSContext* cx, unsigned argc,
                                                   JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);
  return JS::CallNonGenericMethod<is, move_impl>(cx, args);
}

/**
 * Explicit Resource Management Proposal
 *
 * 27.4.3.2 AsyncDisposableStack.prototype.defer ( onDisposeAsync )
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-asyncdisposablestack.prototype.defer
 */
/* static */ bool AsyncDisposableStackObject::defer_impl(
    JSContext* cx, const JS::CallArgs& args) {
  // Step 1. Let asyncDisposableStack be the this value.
  JS::Rooted<AsyncDisposableStackObject*> asyncDisposableStack(
      cx, &args.thisv().toObject().as<AsyncDisposableStackObject>());

  // Step 2. Perform ? RequireInternalSlot(asyncDisposableStack,
  // [[AsyncDisposableState]]).
  // (done by caller)
  // Step 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed,
  // throw a ReferenceError exception.
  if (asyncDisposableStack->state() == DisposableState::Disposed) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DISPOSABLE_STACK_DISPOSED);
    return false;
  }

  // Step 4. If IsCallable(onDisposeAsync) is false, throw a TypeError
  // exception.
  JS::Handle<JS::Value> onDisposeAsync = args.get(0);
  if (!ThrowIfOnDisposeNotCallable(cx, onDisposeAsync)) {
    return false;
  }

  // Step 5. Perform ?
  // AddDisposableResource(asyncDisposableStack.[[DisposeCapability]],
  // undefined, async-dispose, onDisposeAsync).
  JS::Rooted<ArrayObject*> disposeCapability(
      cx, GetOrCreateDisposeCapability(cx, asyncDisposableStack));
  if (!disposeCapability) {
    return false;
  }

  if (!AddDisposableResource(cx, disposeCapability, JS::UndefinedHandleValue,
                             UsingHint::Async, onDisposeAsync)) {
    return false;
  }

  // Step 6. Return undefined.
  args.rval().setUndefined();
  return true;
}

/* static */ bool AsyncDisposableStackObject::defer(JSContext* cx,
                                                    unsigned argc,
                                                    JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);
  return JS::CallNonGenericMethod<is, defer_impl>(cx, args);
}

/**
 * Explicit Resource Management Proposal
 *
 * 27.4.3.1 AsyncDisposableStack.prototype.adopt ( value, onDisposeAsync )
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-asyncdisposablestack.prototype.adopt
 */
/* static */ bool AsyncDisposableStackObject::adopt_impl(
    JSContext* cx, const JS::CallArgs& args) {
  // Step 1. Let asyncDisposableStack be the this value.
  JS::Rooted<AsyncDisposableStackObject*> asyncDisposableStack(
      cx, &args.thisv().toObject().as<AsyncDisposableStackObject>());

  // Step 2. Perform ? RequireInternalSlot(asyncDisposableStack,
  // [[AsyncDisposableState]]).
  // (done by caller)
  // Step 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw
  // a ReferenceError exception.
  if (asyncDisposableStack->state() == DisposableState::Disposed) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DISPOSABLE_STACK_DISPOSED);
    return false;
  }

  // Step 4. If IsCallable(onDisposeAsync) is false, throw a TypeError
  // exception.
  JS::Handle<JS::Value> onDisposeAsync = args.get(1);
  if (!ThrowIfOnDisposeNotCallable(cx, onDisposeAsync)) {
    return false;
  }

  // Step 5. Let closure be a new Abstract Closure with no parameters that
  // captures value and onDisposeAsync and performs the following steps when
  // called:
  // Step 5.a. (see AdoptClosure)
  // Step 6. Let F be CreateBuiltinFunction(closure, 0, "", « »).
  JS::Handle<PropertyName*> funName = cx->names().empty_;
  JS::Rooted<JSFunction*> F(
      cx, NewNativeFunction(cx, AdoptClosure, 0, funName,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!F) {
    return false;
  }
  JS::Handle<JS::Value> value = args.get(0);
  F->initExtendedSlot(AdoptClosureSlot_ValueSlot, value);
  F->initExtendedSlot(AdoptClosureSlot_OnDisposeSlot, onDisposeAsync);

  // Step 7. Perform ?
  // AddDisposableResource(asyncDisposableStack.[[DisposeCapability]],
  // undefined, async-dispose, F).
  JS::Rooted<ArrayObject*> disposeCapability(
      cx, GetOrCreateDisposeCapability(cx, asyncDisposableStack));
  if (!disposeCapability) {
    return false;
  }

  JS::Rooted<JS::Value> FVal(cx, ObjectValue(*F));
  if (!AddDisposableResource(cx, disposeCapability, JS::UndefinedHandleValue,
                             UsingHint::Async, FVal)) {
    return false;
  }

  // Step 8. Return value.
  args.rval().set(value);
  return true;
}

/* static */ bool AsyncDisposableStackObject::adopt(JSContext* cx,
                                                    unsigned argc,
                                                    JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);
  return JS::CallNonGenericMethod<is, adopt_impl>(cx, args);
}

const JSPropertySpec AsyncDisposableStackObject::properties[] = {
    JS_STRING_SYM_PS(toStringTag, "AsyncDisposableStack", JSPROP_READONLY),
    JS_PSG("disposed", disposed, 0),
    JS_PS_END,
};

const JSFunctionSpec AsyncDisposableStackObject::methods[] = {
    JS_FN("adopt", AsyncDisposableStackObject::adopt, 2, 0),
    JS_FN("defer", AsyncDisposableStackObject::defer, 1, 0),
    JS_SELF_HOSTED_FN("disposeAsync", "$AsyncDisposableStackDisposeAsync", 0,
                      0),
    JS_FN("move", AsyncDisposableStackObject::move, 0, 0),
    JS_FN("use", AsyncDisposableStackObject::use, 1, 0),
    JS_SELF_HOSTED_SYM_FN(asyncDispose, "$AsyncDisposableStackDisposeAsync", 0,
                          0),
    JS_FS_END,
};

const ClassSpec AsyncDisposableStackObject::classSpec_ = {
    GenericCreateConstructor<AsyncDisposableStackObject::construct, 0,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<AsyncDisposableStackObject>,
    nullptr,
    nullptr,
    AsyncDisposableStackObject::methods,
    AsyncDisposableStackObject::properties,
    nullptr,
};

const JSClass AsyncDisposableStackObject::class_ = {
    "AsyncDisposableStack",
    JSCLASS_HAS_RESERVED_SLOTS(AsyncDisposableStackObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_AsyncDisposableStack),
    JS_NULL_CLASS_OPS,
    &AsyncDisposableStackObject::classSpec_,
};

const JSClass AsyncDisposableStackObject::protoClass_ = {
    "AsyncDisposableStack.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_AsyncDisposableStack),
    JS_NULL_CLASS_OPS,
    &AsyncDisposableStackObject::classSpec_,
};
