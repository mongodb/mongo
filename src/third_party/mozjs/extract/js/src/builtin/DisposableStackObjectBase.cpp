/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/DisposableStackObjectBase.h"

#include "builtin/Array.h"
#include "vm/ArrayObject.h"
#include "vm/Interpreter.h"

#include "vm/DisposableRecord-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

/**
 * Explicit Resource Management Proposal
 *
 * 27.4.3.1 AsyncDisposableStack.prototype.adopt ( value, onDisposeAsync )
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-asyncdisposablestack.prototype.adopt
 * Step 5.a
 * 27.3.3.1 DisposableStack.prototype.adopt ( value, onDispose )
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposablestack.prototype.adopt
 * Step 5.a
 */
bool js::AdoptClosure(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);

  JS::Rooted<JSFunction*> callee(cx, &args.callee().as<JSFunction>());
  JS::Rooted<JS::Value> value(
      cx, callee->getExtendedSlot(AdoptClosureSlot_ValueSlot));
  JS::Rooted<JS::Value> onDispose(
      cx, callee->getExtendedSlot(AdoptClosureSlot_OnDisposeSlot));

  // Step 5.a. Return ? Call(onDispose, undefined, « value »).
  return Call(cx, onDispose, JS::UndefinedHandleValue, value, args.rval());
}

bool js::ThrowIfOnDisposeNotCallable(JSContext* cx,
                                     JS::Handle<JS::Value> onDispose) {
  if (IsCallable(onDispose)) {
    return true;
  }

  JS::UniqueChars bytes =
      DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, onDispose, nullptr);
  if (!bytes) {
    return false;
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_NOT_FUNCTION,
                           bytes.get());

  return false;
}

// Explicit Resource Management Proposal
// CreateDisposableResource ( V, hint [ , method ] )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-createdisposableresource
// Steps 1, 3.
bool js::CreateDisposableResource(JSContext* cx, JS::Handle<JS::Value> objVal,
                                  UsingHint hint,
                                  JS::MutableHandle<JS::Value> result) {
  // Step 1. If method is not present, then
  // (implicit)
  JS::Rooted<JS::Value> method(cx);
  JS::Rooted<JS::Value> object(cx);
  // Step 1.a. If V is either null or undefined, then
  if (objVal.isNullOrUndefined()) {
    // Step 1.a.i. Set V to undefined.
    // Step 1.a.ii. Set method to undefined.
    object.setUndefined();
    method.setUndefined();
  } else {
    // Step 1.b. Else,
    // Step 1.b.i. If V is not an Object, throw a TypeError exception.
    if (!objVal.isObject()) {
      return ThrowCheckIsObject(cx, CheckIsObjectKind::Disposable);
    }

    // Step 1.b.ii. Set method to ? GetDisposeMethod(V, hint).
    // Step 1.b.iii. If method is undefined, throw a TypeError exception.
    object.set(objVal);
    if (!GetDisposeMethod(cx, object, hint, &method)) {
      return false;
    }
  }

  // Step 3. Return the
  //         DisposableResource Record { [[ResourceValue]]: V, [[Hint]]: hint,
  //         [[DisposeMethod]]: method }.
  DisposableRecordObject* disposableRecord =
      DisposableRecordObject::create(cx, object, method, hint);
  if (!disposableRecord) {
    return false;
  }
  result.set(ObjectValue(*disposableRecord));

  return true;
}

// Explicit Resource Management Proposal
// CreateDisposableResource ( V, hint [ , method ] )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-createdisposableresource
// Steps 2, 3.
bool js::CreateDisposableResource(JSContext* cx, JS::Handle<JS::Value> obj,
                                  UsingHint hint,
                                  JS::Handle<JS::Value> methodVal,
                                  JS::MutableHandle<JS::Value> result) {
  JS::Rooted<JS::Value> method(cx);
  JS::Rooted<JS::Value> object(cx);

  // Step 2. Else,
  // Step 2.a. If IsCallable(method) is false, throw a TypeError exception.
  if (!IsCallable(methodVal)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DISPOSE_NOT_CALLABLE);
    return false;
  }
  object.set(obj);
  method.set(methodVal);

  // Step 3. Return the
  //         DisposableResource Record { [[ResourceValue]]: V, [[Hint]]: hint,
  //         [[DisposeMethod]]: method }.
  DisposableRecordObject* disposableRecord =
      DisposableRecordObject::create(cx, object, method, hint);
  if (!disposableRecord) {
    return false;
  }
  result.set(ObjectValue(*disposableRecord));

  return true;
}

// Explicit Resource Management Proposal
// 7.5.4 AddDisposableResource ( disposeCapability, V, hint [ , method ] )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-adddisposableresource
// Steps 1, 3.
bool js::AddDisposableResource(JSContext* cx,
                               JS::Handle<ArrayObject*> disposeCapability,
                               JS::Handle<JS::Value> val, UsingHint hint) {
  JS::Rooted<JS::Value> resource(cx);

  // Step 1. If method is not present, then
  // (implicit)
  // Step 1.a. If V is either null or undefined and hint is sync-dispose,
  // return unused.
  if (val.isNullOrUndefined() && hint == UsingHint::Sync) {
    return true;
  }

  // Step 1.c. Let resource be ? CreateDisposableResource(V, hint).
  if (!CreateDisposableResource(cx, val, hint, &resource)) {
    return false;
  }

  // Step 3. Append resource to disposeCapability.[[DisposableResourceStack]].
  return NewbornArrayPush(cx, disposeCapability, resource);
}

// Explicit Resource Management Proposal
// 7.5.4 AddDisposableResource ( disposeCapability, V, hint [ , method ] )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-adddisposableresource
bool js::AddDisposableResource(JSContext* cx,
                               JS::Handle<ArrayObject*> disposeCapability,
                               JS::Handle<JS::Value> val, UsingHint hint,
                               JS::Handle<JS::Value> methodVal) {
  JS::Rooted<JS::Value> resource(cx);
  // Step 2. Else,
  // Step 2.a. Assert: V is undefined.
  MOZ_ASSERT(val.isUndefined());

  // Step 2.b. Let resource be ? CreateDisposableResource(undefined, hint,
  // method).
  if (!CreateDisposableResource(cx, val, hint, methodVal, &resource)) {
    return false;
  }
  // Step 3. Append resource to disposeCapability.[[DisposableResourceStack]].
  return NewbornArrayPush(cx, disposeCapability, resource);
}

// Explicit Resource Management Proposal
// GetDisposeMethod ( V, hint )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-getdisposemethod
bool js::GetDisposeMethod(JSContext* cx, JS::Handle<JS::Value> objVal,
                          UsingHint hint,
                          JS::MutableHandle<JS::Value> disposeMethod) {
  switch (hint) {
    case UsingHint::Async: {
      // Step 1. If hint is async-dispose, then
      // Step 1.a. Let method be ? GetMethod(V, @@asyncDispose).
      // GetMethod throws TypeError if method is not callable
      // this is handled below at the end of the function.
      JS::Rooted<JS::PropertyKey> idAsync(
          cx, PropertyKey::Symbol(cx->wellKnownSymbols().asyncDispose));
      JS::Rooted<JSObject*> obj(cx, &objVal.toObject());

      if (!GetProperty(cx, obj, obj, idAsync, disposeMethod)) {
        return false;
      }

      // Step 1.b. If method is undefined, then
      // GetMethod returns undefined if the function is null but
      // since we do not do the conversion here we check for
      // null or undefined here.
      if (disposeMethod.isNullOrUndefined()) {
        // Step 1.b.i. Set method to ? GetMethod(V, @@dispose).
        JS::Rooted<JS::PropertyKey> idSync(
            cx, PropertyKey::Symbol(cx->wellKnownSymbols().dispose));
        JS::Rooted<JS::Value> syncDisposeMethod(cx);
        if (!GetProperty(cx, obj, obj, idSync, &syncDisposeMethod)) {
          return false;
        }

        if (!syncDisposeMethod.isNullOrUndefined()) {
          // Step 1.b.ii. If method is not undefined, then
          if (!IsCallable(syncDisposeMethod)) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_DISPOSE_NOT_CALLABLE);
            return false;
          }

          // Step 1.b.ii.1. Let closure be a new Abstract Closure with no
          // parameters that captures method and performs the following steps
          // when called:
          // Steps 1.b.ii.1.a-f: See SyncDisposalClosure
          // Step 1.b.ii.3. Return CreateBuiltinFunction(closure, 0, "", « »).
          JS::Handle<PropertyName*> funName = cx->names().empty_;
          JSFunction* asyncWrapper = NewNativeFunction(
              cx, SyncDisposalClosure, 0, funName,
              gc::AllocKind::FUNCTION_EXTENDED, GenericObject);
          if (!asyncWrapper) {
            return false;
          }
          asyncWrapper->initExtendedSlot(
              uint8_t(SyncDisposalClosureSlots::Method), syncDisposeMethod);
          disposeMethod.set(JS::ObjectValue(*asyncWrapper));
        }
      }

      break;
    }

    case UsingHint::Sync: {
      // Step 2. Else,
      // Step 2.a. Let method be ? GetMethod(V, @@dispose).
      JS::Rooted<JS::PropertyKey> id(
          cx, PropertyKey::Symbol(cx->wellKnownSymbols().dispose));
      JS::Rooted<JSObject*> obj(cx, &objVal.toObject());

      if (!GetProperty(cx, obj, obj, id, disposeMethod)) {
        return false;
      }

      break;
    }
    default:
      MOZ_CRASH("Invalid UsingHint");
  }

  // CreateDisposableResource ( V, hint [ , method ] )
  // https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-createdisposableresource
  //
  // Step 1.b.iii. If method is undefined, throw a TypeError exception.
  if (disposeMethod.isNullOrUndefined() || !IsCallable(disposeMethod)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DISPOSE_NOT_CALLABLE);
    return false;
  }

  return true;
}

/* static */ ArrayObject*
DisposableStackObjectBase::GetOrCreateDisposeCapability(
    JSContext* cx, JS::Handle<DisposableStackObjectBase*> obj) {
  ArrayObject* disposablesList = nullptr;

  if (obj->isDisposableResourceStackEmpty()) {
    disposablesList = NewDenseEmptyArray(cx);
    if (!disposablesList) {
      return nullptr;
    }
    obj->setReservedSlot(DISPOSABLE_RESOURCE_STACK_SLOT,
                         ObjectValue(*disposablesList));
  } else {
    disposablesList = obj->nonEmptyDisposableResourceStack();
  }

  return disposablesList;
}

bool DisposableStackObjectBase::isDisposableResourceStackEmpty() const {
  return getReservedSlot(DISPOSABLE_RESOURCE_STACK_SLOT).isUndefined();
}

void DisposableStackObjectBase::clearDisposableResourceStack() {
  setReservedSlot(DISPOSABLE_RESOURCE_STACK_SLOT, JS::UndefinedValue());
}

ArrayObject* DisposableStackObjectBase::nonEmptyDisposableResourceStack()
    const {
  MOZ_ASSERT(!isDisposableResourceStackEmpty());
  return &getReservedSlot(DISPOSABLE_RESOURCE_STACK_SLOT)
              .toObject()
              .as<ArrayObject>();
}

DisposableStackObjectBase::DisposableState DisposableStackObjectBase::state()
    const {
  return DisposableState(uint8_t(getReservedSlot(STATE_SLOT).toInt32()));
}

void DisposableStackObjectBase::setState(DisposableState state) {
  setReservedSlot(STATE_SLOT, JS::Int32Value(int32_t(state)));
}
