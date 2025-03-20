/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "proxy/ScriptedProxyHandler.h"

#include "mozilla/Maybe.h"

#include "jsapi.h"

#include "builtin/Object.h"
#include "js/CallAndConstruct.h"  // JS::Construct, JS::IsCallable
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertyDescriptor.h"    // JS::FromPropertyDescriptor
#include "vm/EqualityOperations.h"    // js::SameValue
#include "vm/Interpreter.h"           // js::Call
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using JS::IsArrayAnswer;

using mozilla::Maybe;

// ES2022 rev 33fe30f9a6b0dc81826f2f217167a89c025779a0
// IsCompatiblePropertyDescriptor. BUT that method just calls
// ValidateAndApplyPropertyDescriptor with two additional constant arguments.
// Therefore step numbering is from the latter method, and resulting dead code
// has been removed.

// If an exception should be thrown, we will set errorDetails.
static bool IsCompatiblePropertyDescriptor(
    JSContext* cx, bool extensible, Handle<PropertyDescriptor> desc,
    Handle<Maybe<PropertyDescriptor>> current, const char** errorDetails) {
  // precondition:  we won't set details if checks pass, so it must be null
  // here.
  MOZ_ASSERT(*errorDetails == nullptr);

  // Step 2.
  if (current.isNothing()) {
    // Step 2.a-b,e.  As |O| is always undefined, steps 2.c-d fall away.
    if (!extensible) {
      static const char DETAILS_NOT_EXTENSIBLE[] =
          "proxy can't report an extensible object as non-extensible";
      *errorDetails = DETAILS_NOT_EXTENSIBLE;
    }
    return true;
  }

  current->assertComplete();

  // Step 3.
  if (!desc.hasValue() && !desc.hasWritable() && !desc.hasGetter() &&
      !desc.hasSetter() && !desc.hasEnumerable() && !desc.hasConfigurable()) {
    return true;
  }

  // Step 4.
  if (!current->configurable()) {
    // Step 4.a.
    if (desc.hasConfigurable() && desc.configurable()) {
      static const char DETAILS_CANT_REPORT_NC_AS_C[] =
          "proxy can't report an existing non-configurable property as "
          "configurable";
      *errorDetails = DETAILS_CANT_REPORT_NC_AS_C;
      return true;
    }

    // Step 4.b.
    if (desc.hasEnumerable() && desc.enumerable() != current->enumerable()) {
      static const char DETAILS_ENUM_DIFFERENT[] =
          "proxy can't report a different 'enumerable' from target when target "
          "is not configurable";
      *errorDetails = DETAILS_ENUM_DIFFERENT;
      return true;
    }
  }

  // Step 5.
  if (desc.isGenericDescriptor()) {
    return true;
  }

  // Step 6.
  if (current->isDataDescriptor() != desc.isDataDescriptor()) {
    // Steps 6.a., 10.  As |O| is always undefined, steps 6.b-c fall away.
    if (!current->configurable()) {
      static const char DETAILS_CURRENT_NC_DIFF_TYPE[] =
          "proxy can't report a different descriptor type when target is not "
          "configurable";
      *errorDetails = DETAILS_CURRENT_NC_DIFF_TYPE;
    }
    return true;
  }

  // Step 7.
  if (current->isDataDescriptor()) {
    MOZ_ASSERT(desc.isDataDescriptor());  // by step 6
    // Step 7.a.
    if (!current->configurable() && !current->writable()) {
      // Step 7.a.i.
      if (desc.hasWritable() && desc.writable()) {
        static const char DETAILS_CANT_REPORT_NW_AS_W[] =
            "proxy can't report a non-configurable, non-writable property as "
            "writable";
        *errorDetails = DETAILS_CANT_REPORT_NW_AS_W;
        return true;
      }

      // Step 7.a.ii.
      if (desc.hasValue()) {
        RootedValue value(cx, current->value());
        bool same;
        if (!SameValue(cx, desc.value(), value, &same)) {
          return false;
        }
        if (!same) {
          static const char DETAILS_DIFFERENT_VALUE[] =
              "proxy must report the same value for the non-writable, "
              "non-configurable property";
          *errorDetails = DETAILS_DIFFERENT_VALUE;
          return true;
        }
      }
    }

    // Step 7.a.ii, 10.
    return true;
  }

  // Step 8.

  // Step 8.a.
  MOZ_ASSERT(current->isAccessorDescriptor());  // by step 7
  MOZ_ASSERT(desc.isAccessorDescriptor());      // by step 6

  // Step 8.b.
  if (current->configurable()) {
    return true;
  }
  // Steps 8.b.i-ii.
  if (desc.hasSetter() && desc.setter() != current->setter()) {
    static const char DETAILS_SETTERS_DIFFERENT[] =
        "proxy can't report different setters for a currently non-configurable "
        "property";
    *errorDetails = DETAILS_SETTERS_DIFFERENT;
  } else if (desc.hasGetter() && desc.getter() != current->getter()) {
    static const char DETAILS_GETTERS_DIFFERENT[] =
        "proxy can't report different getters for a currently non-configurable "
        "property";
    *errorDetails = DETAILS_GETTERS_DIFFERENT;
  }

  // Step 9.
  // |O| is always undefined.

  // Step 10.
  return true;
}

// Get the [[ProxyHandler]] of a scripted proxy.
/* static */
JSObject* ScriptedProxyHandler::handlerObject(const JSObject* proxy) {
  MOZ_ASSERT(proxy->as<ProxyObject>().handler() ==
             &ScriptedProxyHandler::singleton);
  return proxy->as<ProxyObject>()
      .reservedSlot(ScriptedProxyHandler::HANDLER_EXTRA)
      .toObjectOrNull();
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 7.3.9 GetMethod, reimplemented for proxy handler trap-getting to produce
// better error messages.
static bool GetProxyTrap(JSContext* cx, HandleObject handler,
                         Handle<PropertyName*> name, MutableHandleValue func) {
  // Steps 2, 5.
  if (!GetProperty(cx, handler, handler, name, func)) {
    return false;
  }

  // Step 3.
  if (func.isUndefined()) {
    return true;
  }

  if (func.isNull()) {
    func.setUndefined();
    return true;
  }

  // Step 4.
  if (!IsCallable(func)) {
    UniqueChars bytes = EncodeAscii(cx, name);
    if (!bytes) {
      return false;
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_TRAP,
                              bytes.get());
    return false;
  }

  return true;
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.1 Proxy.[[GetPrototypeOf]].
bool ScriptedProxyHandler::getPrototype(JSContext* cx, HandleObject proxy,
                                        MutableHandleObject protop) const {
  // Steps 1-3.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 4.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 5.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().getPrototypeOf, &trap)) {
    return false;
  }

  // Step 6.
  if (trap.isUndefined()) {
    return GetPrototype(cx, target, protop);
  }

  // Step 7.
  RootedValue handlerProto(cx);
  {
    FixedInvokeArgs<1> args(cx);

    args[0].setObject(*target);

    handlerProto.setObject(*handler);

    if (!js::Call(cx, trap, handlerProto, args, &handlerProto)) {
      return false;
    }
  }

  // Step 8.
  if (!handlerProto.isObjectOrNull()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_GETPROTOTYPEOF_TRAP_RETURN);
    return false;
  }

  // Step 9.
  bool extensibleTarget;
  if (!IsExtensible(cx, target, &extensibleTarget)) {
    return false;
  }

  // Step 10.
  if (extensibleTarget) {
    protop.set(handlerProto.toObjectOrNull());
    return true;
  }

  // Step 11.
  RootedObject targetProto(cx);
  if (!GetPrototype(cx, target, &targetProto)) {
    return false;
  }

  // Step 12.
  if (handlerProto.toObjectOrNull() != targetProto) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCONSISTENT_GETPROTOTYPEOF_TRAP);
    return false;
  }

  // Step 13.
  protop.set(handlerProto.toObjectOrNull());
  return true;
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.2 Proxy.[[SetPrototypeOf]].
bool ScriptedProxyHandler::setPrototype(JSContext* cx, HandleObject proxy,
                                        HandleObject proto,
                                        ObjectOpResult& result) const {
  // Steps 1-4.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 5.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 6.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().setPrototypeOf, &trap)) {
    return false;
  }

  // Step 7.
  if (trap.isUndefined()) {
    return SetPrototype(cx, target, proto, result);
  }

  // Step 8.
  bool booleanTrapResult;
  {
    FixedInvokeArgs<2> args(cx);

    args[0].setObject(*target);
    args[1].setObjectOrNull(proto);

    RootedValue hval(cx, ObjectValue(*handler));
    if (!js::Call(cx, trap, hval, args, &hval)) {
      return false;
    }

    booleanTrapResult = ToBoolean(hval);
  }

  // Step 9.
  if (!booleanTrapResult) {
    return result.fail(JSMSG_PROXY_SETPROTOTYPEOF_RETURNED_FALSE);
  }

  // Step 10.
  bool extensibleTarget;
  if (!IsExtensible(cx, target, &extensibleTarget)) {
    return false;
  }

  // Step 11.
  if (extensibleTarget) {
    return result.succeed();
  }

  // Step 12.
  RootedObject targetProto(cx);
  if (!GetPrototype(cx, target, &targetProto)) {
    return false;
  }

  // Step 13.
  if (proto != targetProto) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCONSISTENT_SETPROTOTYPEOF_TRAP);
    return false;
  }

  // Step 14.
  return result.succeed();
}

bool ScriptedProxyHandler::getPrototypeIfOrdinary(
    JSContext* cx, HandleObject proxy, bool* isOrdinary,
    MutableHandleObject protop) const {
  *isOrdinary = false;
  return true;
}

// Not yet part of ES6, but hopefully to be standards-tracked -- and needed to
// handle revoked proxies in any event.
bool ScriptedProxyHandler::setImmutablePrototype(JSContext* cx,
                                                 HandleObject proxy,
                                                 bool* succeeded) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  if (!target) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  return SetImmutablePrototype(cx, target, succeeded);
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.4 Proxy.[[PreventExtensions]]()
bool ScriptedProxyHandler::preventExtensions(JSContext* cx, HandleObject proxy,
                                             ObjectOpResult& result) const {
  // Steps 1-3.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 4.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 5.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().preventExtensions, &trap)) {
    return false;
  }

  // Step 6.
  if (trap.isUndefined()) {
    return PreventExtensions(cx, target, result);
  }

  // Step 7.
  bool booleanTrapResult;
  {
    RootedValue arg(cx, ObjectValue(*target));
    RootedValue trapResult(cx);
    if (!Call(cx, trap, handler, arg, &trapResult)) {
      return false;
    }

    booleanTrapResult = ToBoolean(trapResult);
  }

  // Step 8.
  if (booleanTrapResult) {
    // Step 8a.
    bool targetIsExtensible;
    if (!IsExtensible(cx, target, &targetIsExtensible)) {
      return false;
    }

    if (targetIsExtensible) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_CANT_REPORT_AS_NON_EXTENSIBLE);
      return false;
    }

    // Step 9.
    return result.succeed();
  }

  // Also step 9.
  return result.fail(JSMSG_PROXY_PREVENTEXTENSIONS_RETURNED_FALSE);
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.3 Proxy.[[IsExtensible]]()
bool ScriptedProxyHandler::isExtensible(JSContext* cx, HandleObject proxy,
                                        bool* extensible) const {
  // Steps 1-3.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 4.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 5.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().isExtensible, &trap)) {
    return false;
  }

  // Step 6.
  if (trap.isUndefined()) {
    return IsExtensible(cx, target, extensible);
  }

  // Step 7.
  bool booleanTrapResult;
  {
    RootedValue arg(cx, ObjectValue(*target));
    RootedValue trapResult(cx);
    if (!Call(cx, trap, handler, arg, &trapResult)) {
      return false;
    }

    booleanTrapResult = ToBoolean(trapResult);
  }

  // Steps 8.
  bool targetResult;
  if (!IsExtensible(cx, target, &targetResult)) {
    return false;
  }

  // Step 9.
  if (targetResult != booleanTrapResult) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_EXTENSIBILITY);
    return false;
  }

  // Step 10.
  *extensible = booleanTrapResult;
  return true;
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.5 Proxy.[[GetOwnProperty]](P)
bool ScriptedProxyHandler::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject proxy, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
  // Steps 2-4.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 5.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 6.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().getOwnPropertyDescriptor, &trap)) {
    return false;
  }

  // Step 7.
  if (trap.isUndefined()) {
    return GetOwnPropertyDescriptor(cx, target, id, desc);
  }

  // Step 8.
  RootedValue propKey(cx);
  if (!IdToStringOrSymbol(cx, id, &propKey)) {
    return false;
  }

  RootedValue trapResult(cx);
  RootedValue targetVal(cx, ObjectValue(*target));
  if (!Call(cx, trap, handler, targetVal, propKey, &trapResult)) {
    return false;
  }

  // Step 9.
  if (!trapResult.isUndefined() && !trapResult.isObject()) {
    return js::Throw(cx, id, JSMSG_PROXY_GETOWN_OBJORUNDEF);
  }

  // Step 10.
  Rooted<Maybe<PropertyDescriptor>> targetDesc(cx);
  if (!GetOwnPropertyDescriptor(cx, target, id, &targetDesc)) {
    return false;
  }

  // Step 11.
  if (trapResult.isUndefined()) {
    // Step 11a.
    if (targetDesc.isNothing()) {
      desc.reset();
      return true;
    }

    // Step 11b.
    if (!targetDesc->configurable()) {
      return js::Throw(cx, id, JSMSG_CANT_REPORT_NC_AS_NE);
    }

    // Steps 11c-d.
    bool extensibleTarget;
    if (!IsExtensible(cx, target, &extensibleTarget)) {
      return false;
    }

    // Step 11e.
    if (!extensibleTarget) {
      return js::Throw(cx, id, JSMSG_CANT_REPORT_E_AS_NE);
    }

    // Step 11f.
    desc.reset();
    return true;
  }

  // Step 12.
  bool extensibleTarget;
  if (!IsExtensible(cx, target, &extensibleTarget)) {
    return false;
  }

  // Step 13.
  Rooted<PropertyDescriptor> resultDesc(cx);
  if (!ToPropertyDescriptor(cx, trapResult, true, &resultDesc)) {
    return false;
  }

  // Step 14.
  CompletePropertyDescriptor(&resultDesc);

  // Step 15.
  const char* errorDetails = nullptr;
  if (!IsCompatiblePropertyDescriptor(cx, extensibleTarget, resultDesc,
                                      targetDesc, &errorDetails))
    return false;

  // Step 16.
  if (errorDetails) {
    return js::Throw(cx, id, JSMSG_CANT_REPORT_INVALID, errorDetails);
  }

  // Step 17.
  if (!resultDesc.configurable()) {
    if (targetDesc.isNothing()) {
      return js::Throw(cx, id, JSMSG_CANT_REPORT_NE_AS_NC);
    }

    if (targetDesc->configurable()) {
      return js::Throw(cx, id, JSMSG_CANT_REPORT_C_AS_NC);
    }

    if (resultDesc.hasWritable() && !resultDesc.writable()) {
      if (targetDesc->writable()) {
        return js::Throw(cx, id, JSMSG_CANT_REPORT_W_AS_NW);
      }
    }
  }

  // Step 18.
  desc.set(mozilla::Some(resultDesc.get()));
  return true;
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.6 Proxy.[[DefineOwnProperty]](P, Desc)
bool ScriptedProxyHandler::defineProperty(JSContext* cx, HandleObject proxy,
                                          HandleId id,
                                          Handle<PropertyDescriptor> desc,
                                          ObjectOpResult& result) const {
  // Steps 2-4.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 5.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 6.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().defineProperty, &trap)) {
    return false;
  }

  // Step 7.
  if (trap.isUndefined()) {
    return DefineProperty(cx, target, id, desc, result);
  }

  // Step 8.
  RootedValue descObj(cx);
  if (!FromPropertyDescriptorToObject(cx, desc, &descObj)) {
    return false;
  }

  // Step 9.
  RootedValue propKey(cx);
  if (!IdToStringOrSymbol(cx, id, &propKey)) {
    return false;
  }

  RootedValue trapResult(cx);
  {
    FixedInvokeArgs<3> args(cx);

    args[0].setObject(*target);
    args[1].set(propKey);
    args[2].set(descObj);

    RootedValue thisv(cx, ObjectValue(*handler));
    if (!Call(cx, trap, thisv, args, &trapResult)) {
      return false;
    }
  }

  // Step 10.
  if (!ToBoolean(trapResult)) {
    return result.fail(JSMSG_PROXY_DEFINE_RETURNED_FALSE);
  }

  // Step 11.
  Rooted<Maybe<PropertyDescriptor>> targetDesc(cx);
  if (!GetOwnPropertyDescriptor(cx, target, id, &targetDesc)) {
    return false;
  }

  // Step 12.
  bool extensibleTarget;
  if (!IsExtensible(cx, target, &extensibleTarget)) {
    return false;
  }

  // Steps 13-14.
  bool settingConfigFalse = desc.hasConfigurable() && !desc.configurable();

  // Steps 15-16.
  if (targetDesc.isNothing()) {
    // Step 15a.
    if (!extensibleTarget) {
      return js::Throw(cx, id, JSMSG_CANT_DEFINE_NEW);
    }

    // Step 15b.
    if (settingConfigFalse) {
      return js::Throw(cx, id, JSMSG_CANT_DEFINE_NE_AS_NC);
    }
  } else {
    // Step 16a.
    const char* errorDetails = nullptr;
    if (!IsCompatiblePropertyDescriptor(cx, extensibleTarget, desc, targetDesc,
                                        &errorDetails))
      return false;

    if (errorDetails) {
      return js::Throw(cx, id, JSMSG_CANT_DEFINE_INVALID, errorDetails);
    }

    // Step 16b.
    if (settingConfigFalse && targetDesc->configurable()) {
      static const char DETAILS_CANT_REPORT_C_AS_NC[] =
          "proxy can't define an existing configurable property as "
          "non-configurable";
      return js::Throw(cx, id, JSMSG_CANT_DEFINE_INVALID,
                       DETAILS_CANT_REPORT_C_AS_NC);
    }

    if (targetDesc->isDataDescriptor() && !targetDesc->configurable() &&
        targetDesc->writable()) {
      if (desc.hasWritable() && !desc.writable()) {
        static const char DETAILS_CANT_DEFINE_NW[] =
            "proxy can't define an existing non-configurable writable property "
            "as non-writable";
        return js::Throw(cx, id, JSMSG_CANT_DEFINE_INVALID,
                         DETAILS_CANT_DEFINE_NW);
      }
    }
  }

  // Step 17.
  return result.succeed();
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 7.3.17 CreateListFromArrayLike with elementTypes fixed to symbol/string.
static bool CreateFilteredListFromArrayLike(JSContext* cx, HandleValue v,
                                            MutableHandleIdVector props) {
  // Step 2.
  RootedObject obj(cx, RequireObject(cx, JSMSG_OBJECT_REQUIRED_RET_OWNKEYS,
                                     JSDVG_IGNORE_STACK, v));
  if (!obj) {
    return false;
  }

  // Step 3.
  uint64_t len;
  if (!GetLengthProperty(cx, obj, &len)) {
    return false;
  }

  // Steps 4-6.
  RootedValue next(cx);
  RootedId id(cx);
  uint64_t index = 0;
  while (index < len) {
    // Steps 6a-b.
    if (!GetElementLargeIndex(cx, obj, obj, index, &next)) {
      return false;
    }

    // Step 6c.
    if (!next.isString() && !next.isSymbol()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_OWNKEYS_STR_SYM);
      return false;
    }

    if (!PrimitiveValueToId<CanGC>(cx, next, &id)) {
      return false;
    }

    // Step 6d.
    if (!props.append(id)) {
      return false;
    }

    // Step 6e.
    index++;
  }

  // Step 7.
  return true;
}

// ES2018 draft rev aab1ea3bd4d03c85d6f4a91503b4169346ab7271
// 9.5.11 Proxy.[[OwnPropertyKeys]]()
bool ScriptedProxyHandler::ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                           MutableHandleIdVector props) const {
  // Steps 1-3.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 4.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 5.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().ownKeys, &trap)) {
    return false;
  }

  // Step 6.
  if (trap.isUndefined()) {
    return GetPropertyKeys(
        cx, target, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, props);
  }

  // Step 7.
  RootedValue trapResultArray(cx);
  RootedValue targetVal(cx, ObjectValue(*target));
  if (!Call(cx, trap, handler, targetVal, &trapResultArray)) {
    return false;
  }

  // Step 8.
  RootedIdVector trapResult(cx);
  if (!CreateFilteredListFromArrayLike(cx, trapResultArray, &trapResult)) {
    return false;
  }

  // Steps 9, 18.
  Rooted<GCHashSet<jsid>> uncheckedResultKeys(
      cx, GCHashSet<jsid>(cx, trapResult.length()));

  for (size_t i = 0, len = trapResult.length(); i < len; i++) {
    MOZ_ASSERT(!trapResult[i].isVoid());

    auto ptr = uncheckedResultKeys.lookupForAdd(trapResult[i]);
    if (ptr) {
      return js::Throw(cx, trapResult[i], JSMSG_OWNKEYS_DUPLICATE);
    }

    if (!uncheckedResultKeys.add(ptr, trapResult[i])) {
      return false;
    }
  }

  // Step 10.
  bool extensibleTarget;
  if (!IsExtensible(cx, target, &extensibleTarget)) {
    return false;
  }

  // Steps 11-13.
  RootedIdVector targetKeys(cx);
  if (!GetPropertyKeys(cx, target,
                       JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS,
                       &targetKeys)) {
    return false;
  }

  // Steps 14-15.
  RootedIdVector targetConfigurableKeys(cx);
  RootedIdVector targetNonconfigurableKeys(cx);

  // Step 16.
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  for (size_t i = 0; i < targetKeys.length(); ++i) {
    // Step 16.a.
    if (!GetOwnPropertyDescriptor(cx, target, targetKeys[i], &desc)) {
      return false;
    }

    // Steps 16.b-c.
    if (desc.isSome() && !desc->configurable()) {
      if (!targetNonconfigurableKeys.append(targetKeys[i])) {
        return false;
      }
    } else {
      if (!targetConfigurableKeys.append(targetKeys[i])) {
        return false;
      }
    }
  }

  // Step 17.
  if (extensibleTarget && targetNonconfigurableKeys.empty()) {
    return props.appendAll(std::move(trapResult));
  }

  // Step 19.
  for (size_t i = 0; i < targetNonconfigurableKeys.length(); ++i) {
    MOZ_ASSERT(!targetNonconfigurableKeys[i].isVoid());

    auto ptr = uncheckedResultKeys.lookup(targetNonconfigurableKeys[i]);

    // Step 19.a.
    if (!ptr) {
      return js::Throw(cx, targetNonconfigurableKeys[i], JSMSG_CANT_SKIP_NC);
    }

    // Step 19.b.
    uncheckedResultKeys.remove(ptr);
  }

  // Step 20.
  if (extensibleTarget) {
    return props.appendAll(std::move(trapResult));
  }

  // Step 21.
  for (size_t i = 0; i < targetConfigurableKeys.length(); ++i) {
    MOZ_ASSERT(!targetConfigurableKeys[i].isVoid());

    auto ptr = uncheckedResultKeys.lookup(targetConfigurableKeys[i]);

    // Step 21.a.
    if (!ptr) {
      return js::Throw(cx, targetConfigurableKeys[i],
                       JSMSG_CANT_REPORT_E_AS_NE);
    }

    // Step 21.b.
    uncheckedResultKeys.remove(ptr);
  }

  // Step 22.
  if (!uncheckedResultKeys.empty()) {
    RootedId id(cx, uncheckedResultKeys.all().front());
    return js::Throw(cx, id, JSMSG_CANT_REPORT_NEW);
  }

  // Step 23.
  return props.appendAll(std::move(trapResult));
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.10 Proxy.[[Delete]](P)
bool ScriptedProxyHandler::delete_(JSContext* cx, HandleObject proxy,
                                   HandleId id, ObjectOpResult& result) const {
  // Steps 2-4.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 5.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 6.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().deleteProperty, &trap)) {
    return false;
  }

  // Step 7.
  if (trap.isUndefined()) {
    return DeleteProperty(cx, target, id, result);
  }

  // Step 8.
  bool booleanTrapResult;
  {
    RootedValue value(cx);
    if (!IdToStringOrSymbol(cx, id, &value)) {
      return false;
    }

    RootedValue targetVal(cx, ObjectValue(*target));
    RootedValue trapResult(cx);
    if (!Call(cx, trap, handler, targetVal, value, &trapResult)) {
      return false;
    }

    booleanTrapResult = ToBoolean(trapResult);
  }

  // Step 9.
  if (!booleanTrapResult) {
    return result.fail(JSMSG_PROXY_DELETE_RETURNED_FALSE);
  }

  // Step 10.
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, target, id, &desc)) {
    return false;
  }

  // Step 11.
  if (desc.isNothing()) {
    return result.succeed();
  }

  // Step 12.
  if (!desc->configurable()) {
    return Throw(cx, id, JSMSG_CANT_DELETE);
  }

  bool extensible;
  if (!IsExtensible(cx, target, &extensible)) {
    return false;
  }

  if (!extensible) {
    return Throw(cx, id, JSMSG_CANT_DELETE_NON_EXTENSIBLE);
  }

  // Step 13.
  return result.succeed();
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.7 Proxy.[[HasProperty]](P)
bool ScriptedProxyHandler::has(JSContext* cx, HandleObject proxy, HandleId id,
                               bool* bp) const {
  // Steps 2-4.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 5.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 6.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().has, &trap)) {
    return false;
  }

  // Step 7.
  if (trap.isUndefined()) {
    return HasProperty(cx, target, id, bp);
  }

  // Step 8.
  RootedValue value(cx);
  if (!IdToStringOrSymbol(cx, id, &value)) {
    return false;
  }

  RootedValue trapResult(cx);
  RootedValue targetVal(cx, ObjectValue(*target));
  if (!Call(cx, trap, handler, targetVal, value, &trapResult)) {
    return false;
  }

  bool booleanTrapResult = ToBoolean(trapResult);

  // Step 9.
  if (!booleanTrapResult) {
    // Step 9a.
    Rooted<Maybe<PropertyDescriptor>> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, target, id, &desc)) {
      return false;
    }

    // Step 9b.
    if (desc.isSome()) {
      // Step 9b(i).
      if (!desc->configurable()) {
        return js::Throw(cx, id, JSMSG_CANT_REPORT_NC_AS_NE);
      }

      // Step 9b(ii).
      bool extensible;
      if (!IsExtensible(cx, target, &extensible)) {
        return false;
      }

      // Step 9b(iii).
      if (!extensible) {
        return js::Throw(cx, id, JSMSG_CANT_REPORT_E_AS_NE);
      }
    }
  }

  // Step 10.
  *bp = booleanTrapResult;
  return true;
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.8 Proxy.[[GetP]](P, Receiver)
bool ScriptedProxyHandler::get(JSContext* cx, HandleObject proxy,
                               HandleValue receiver, HandleId id,
                               MutableHandleValue vp) const {
  // Steps 2-4.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 5.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Steps 6.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().get, &trap)) {
    return false;
  }

  // Step 7.
  if (trap.isUndefined()) {
    return GetProperty(cx, target, receiver, id, vp);
  }

  // Step 8.
  RootedValue value(cx);
  if (!IdToStringOrSymbol(cx, id, &value)) {
    return false;
  }

  RootedValue trapResult(cx);
  {
    FixedInvokeArgs<3> args(cx);

    args[0].setObject(*target);
    args[1].set(value);
    args[2].set(receiver);

    RootedValue thisv(cx, ObjectValue(*handler));
    if (!Call(cx, trap, thisv, args, &trapResult)) {
      return false;
    }
  }

  // Steps 9 and 10.
  GetTrapValidationResult validation =
      checkGetTrapResult(cx, target, id, trapResult);
  if (validation != GetTrapValidationResult::OK) {
    reportGetTrapValidationError(cx, id, validation);
    return false;
  }

  // Step 11.
  vp.set(trapResult);
  return true;
}

void ScriptedProxyHandler::reportGetTrapValidationError(
    JSContext* cx, HandleId id, GetTrapValidationResult validation) {
  switch (validation) {
    case GetTrapValidationResult::MustReportSameValue:
      js::Throw(cx, id, JSMSG_MUST_REPORT_SAME_VALUE);
      return;
    case GetTrapValidationResult::MustReportUndefined:
      js::Throw(cx, id, JSMSG_MUST_REPORT_SAME_VALUE);
      return;
    case GetTrapValidationResult::Exception:
      return;
    case GetTrapValidationResult::OK:
      MOZ_CRASH("unreachable");
  }
}

ScriptedProxyHandler::GetTrapValidationResult
ScriptedProxyHandler::checkGetTrapResult(JSContext* cx, HandleObject target,
                                         HandleId id, HandleValue trapResult) {
  // Step 9.
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, target, id, &desc)) {
    return GetTrapValidationResult::Exception;
  }

  // Step 10.
  if (desc.isSome()) {
    // Step 10a.
    if (desc->isDataDescriptor() && !desc->configurable() &&
        !desc->writable()) {
      RootedValue value(cx, desc->value());
      bool same;
      if (!SameValue(cx, trapResult, value, &same)) {
        return GetTrapValidationResult::Exception;
      }

      if (!same) {
        return GetTrapValidationResult::MustReportSameValue;
      }
    }

    // Step 10b.
    if (desc->isAccessorDescriptor() && !desc->configurable() &&
        (desc->getter() == nullptr) && !trapResult.isUndefined()) {
      return GetTrapValidationResult::MustReportUndefined;
    }
  }

  return GetTrapValidationResult::OK;
}

// ES8 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 9.5.9 Proxy.[[Set]](P, V, Receiver)
bool ScriptedProxyHandler::set(JSContext* cx, HandleObject proxy, HandleId id,
                               HandleValue v, HandleValue receiver,
                               ObjectOpResult& result) const {
  // Steps 2-4.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 5.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);

  // Step 6.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().set, &trap)) {
    return false;
  }

  // Step 7.
  if (trap.isUndefined()) {
    return SetProperty(cx, target, id, v, receiver, result);
  }

  // Step 8.
  RootedValue value(cx);
  if (!IdToStringOrSymbol(cx, id, &value)) {
    return false;
  }

  RootedValue trapResult(cx);
  {
    FixedInvokeArgs<4> args(cx);

    args[0].setObject(*target);
    args[1].set(value);
    args[2].set(v);
    args[3].set(receiver);

    RootedValue thisv(cx, ObjectValue(*handler));
    if (!Call(cx, trap, thisv, args, &trapResult)) {
      return false;
    }
  }

  // Step 9.
  if (!ToBoolean(trapResult)) {
    return result.fail(JSMSG_PROXY_SET_RETURNED_FALSE);
  }

  // Step 10.
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, target, id, &desc)) {
    return false;
  }

  // Step 11.
  if (desc.isSome()) {
    // Step 11a.
    if (desc->isDataDescriptor() && !desc->configurable() &&
        !desc->writable()) {
      RootedValue value(cx, desc->value());
      bool same;
      if (!SameValue(cx, v, value, &same)) {
        return false;
      }
      if (!same) {
        return js::Throw(cx, id, JSMSG_CANT_SET_NW_NC);
      }
    }

    // Step 11b.
    if (desc->isAccessorDescriptor() && !desc->configurable() &&
        desc->setter() == nullptr) {
      return js::Throw(cx, id, JSMSG_CANT_SET_WO_SETTER);
    }
  }

  // Step 12.
  return result.succeed();
}

// ES7 0c1bd3004329336774cbc90de727cd0cf5f11e93 9.5.13 Proxy.[[Call]]
bool ScriptedProxyHandler::call(JSContext* cx, HandleObject proxy,
                                const CallArgs& args) const {
  // Steps 1-3.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 4.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);
  MOZ_ASSERT(target->isCallable());

  // Step 5.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().apply, &trap)) {
    return false;
  }

  // Step 6.
  if (trap.isUndefined()) {
    InvokeArgs iargs(cx);
    if (!FillArgumentsFromArraylike(cx, iargs, args)) {
      return false;
    }

    RootedValue fval(cx, ObjectValue(*target));
    return js::Call(cx, fval, args.thisv(), iargs, args.rval());
  }

  // Step 7.
  RootedObject argArray(cx,
                        NewDenseCopiedArray(cx, args.length(), args.array()));
  if (!argArray) {
    return false;
  }

  // Step 8.
  FixedInvokeArgs<3> iargs(cx);

  iargs[0].setObject(*target);
  iargs[1].set(args.thisv());
  iargs[2].setObject(*argArray);

  RootedValue thisv(cx, ObjectValue(*handler));
  return js::Call(cx, trap, thisv, iargs, args.rval());
}

// ES7 0c1bd3004329336774cbc90de727cd0cf5f11e93 9.5.14 Proxy.[[Construct]]
bool ScriptedProxyHandler::construct(JSContext* cx, HandleObject proxy,
                                     const CallArgs& args) const {
  // Steps 1-3.
  RootedObject handler(cx, ScriptedProxyHandler::handlerObject(proxy));
  if (!handler) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_REVOKED);
    return false;
  }

  // Step 4.
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  MOZ_ASSERT(target);
  MOZ_ASSERT(target->isConstructor());

  // Step 5.
  RootedValue trap(cx);
  if (!GetProxyTrap(cx, handler, cx->names().construct, &trap)) {
    return false;
  }

  // Step 6.
  if (trap.isUndefined()) {
    ConstructArgs cargs(cx);
    if (!FillArgumentsFromArraylike(cx, cargs, args)) {
      return false;
    }

    RootedValue targetv(cx, ObjectValue(*target));
    RootedObject obj(cx);
    if (!Construct(cx, targetv, cargs, args.newTarget(), &obj)) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 7.
  RootedObject argArray(cx,
                        NewDenseCopiedArray(cx, args.length(), args.array()));
  if (!argArray) {
    return false;
  }

  // Steps 8, 10.
  {
    FixedInvokeArgs<3> iargs(cx);

    iargs[0].setObject(*target);
    iargs[1].setObject(*argArray);
    iargs[2].set(args.newTarget());

    RootedValue thisv(cx, ObjectValue(*handler));
    if (!Call(cx, trap, thisv, iargs, args.rval())) {
      return false;
    }
  }

  // Step 9.
  if (!args.rval().isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PROXY_CONSTRUCT_OBJECT);
    return false;
  }

  return true;
}

bool ScriptedProxyHandler::nativeCall(JSContext* cx, IsAcceptableThis test,
                                      NativeImpl impl,
                                      const CallArgs& args) const {
  ReportIncompatible(cx, args);
  return false;
}

bool ScriptedProxyHandler::getBuiltinClass(JSContext* cx, HandleObject proxy,
                                           ESClass* cls) const {
  *cls = ESClass::Other;
  return true;
}

bool ScriptedProxyHandler::isArray(JSContext* cx, HandleObject proxy,
                                   IsArrayAnswer* answer) const {
  RootedObject target(cx, proxy->as<ProxyObject>().target());
  if (target) {
    return JS::IsArray(cx, target, answer);
  }

  *answer = IsArrayAnswer::RevokedProxy;
  return true;
}

const char* ScriptedProxyHandler::className(JSContext* cx,
                                            HandleObject proxy) const {
  // Right now the caller is not prepared to handle failures.
  return BaseProxyHandler::className(cx, proxy);
}

JSString* ScriptedProxyHandler::fun_toString(JSContext* cx, HandleObject proxy,
                                             bool isToSource) const {
  // The BaseProxyHandler has the desired behavior: Throw for non-callable,
  // otherwise return [native code].
  return BaseProxyHandler::fun_toString(cx, proxy, isToSource);
}

RegExpShared* ScriptedProxyHandler::regexp_toShared(JSContext* cx,
                                                    HandleObject proxy) const {
  MOZ_CRASH("Should not end up in ScriptedProxyHandler::regexp_toShared");
}

bool ScriptedProxyHandler::boxedValue_unbox(JSContext* cx, HandleObject proxy,
                                            MutableHandleValue vp) const {
  MOZ_CRASH("Should not end up in ScriptedProxyHandler::boxedValue_unbox");
  return false;
}

bool ScriptedProxyHandler::isCallable(JSObject* obj) const {
  MOZ_ASSERT(obj->as<ProxyObject>().handler() ==
             &ScriptedProxyHandler::singleton);
  uint32_t callConstruct = obj->as<ProxyObject>()
                               .reservedSlot(IS_CALLCONSTRUCT_EXTRA)
                               .toPrivateUint32();
  return !!(callConstruct & IS_CALLABLE);
}

bool ScriptedProxyHandler::isConstructor(JSObject* obj) const {
  MOZ_ASSERT(obj->as<ProxyObject>().handler() ==
             &ScriptedProxyHandler::singleton);
  uint32_t callConstruct = obj->as<ProxyObject>()
                               .reservedSlot(IS_CALLCONSTRUCT_EXTRA)
                               .toPrivateUint32();
  return !!(callConstruct & IS_CONSTRUCTOR);
}

const char ScriptedProxyHandler::family = 0;
const ScriptedProxyHandler ScriptedProxyHandler::singleton;

// ES2021 rev c21b280a2c46e92decf3efeca9e9da35d5b9f622
// Including the changes from: https://github.com/tc39/ecma262/pull/1814
// 9.5.14 ProxyCreate.
static bool ProxyCreate(JSContext* cx, CallArgs& args, const char* callerName) {
  if (!args.requireAtLeast(cx, callerName, 2)) {
    return false;
  }

  // Step 1.
  RootedObject target(cx,
                      RequireObjectArg(cx, "`target`", callerName, args[0]));
  if (!target) {
    return false;
  }

  // Step 2.
  RootedObject handler(cx,
                       RequireObjectArg(cx, "`handler`", callerName, args[1]));
  if (!handler) {
    return false;
  }

  // Steps 3-4, 6.
  RootedValue priv(cx, ObjectValue(*target));
  JSObject* proxy_ = NewProxyObject(cx, &ScriptedProxyHandler::singleton, priv,
                                    TaggedProto::LazyProto);
  if (!proxy_) {
    return false;
  }

  // Step 7 (reordered).
  Rooted<ProxyObject*> proxy(cx, &proxy_->as<ProxyObject>());
  proxy->setReservedSlot(ScriptedProxyHandler::HANDLER_EXTRA,
                         ObjectValue(*handler));

  // Step 5.
  uint32_t callable =
      target->isCallable() ? ScriptedProxyHandler::IS_CALLABLE : 0;
  uint32_t constructor =
      target->isConstructor() ? ScriptedProxyHandler::IS_CONSTRUCTOR : 0;
  proxy->setReservedSlot(ScriptedProxyHandler::IS_CALLCONSTRUCT_EXTRA,
                         PrivateUint32Value(callable | constructor));

  // Step 8.
  args.rval().setObject(*proxy);
  return true;
}

bool js::proxy(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "Proxy")) {
    return false;
  }

  return ProxyCreate(cx, args, "Proxy");
}

static bool RevokeProxy(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedFunction func(cx, &args.callee().as<JSFunction>());
  RootedObject p(cx, func->getExtendedSlot(ScriptedProxyHandler::REVOKE_SLOT)
                         .toObjectOrNull());

  if (p) {
    func->setExtendedSlot(ScriptedProxyHandler::REVOKE_SLOT, NullValue());

    MOZ_ASSERT(p->is<ProxyObject>());

    p->as<ProxyObject>().setSameCompartmentPrivate(NullValue());
    p->as<ProxyObject>().setReservedSlot(ScriptedProxyHandler::HANDLER_EXTRA,
                                         NullValue());
  }

  args.rval().setUndefined();
  return true;
}

bool js::proxy_revocable(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ProxyCreate(cx, args, "Proxy.revocable")) {
    return false;
  }

  RootedValue proxyVal(cx, args.rval());
  MOZ_ASSERT(proxyVal.toObject().is<ProxyObject>());

  RootedFunction revoker(
      cx, NewNativeFunction(cx, RevokeProxy, 0, nullptr,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!revoker) {
    return false;
  }

  revoker->initExtendedSlot(ScriptedProxyHandler::REVOKE_SLOT, proxyVal);

  Rooted<PlainObject*> result(cx, NewPlainObject(cx));
  if (!result) {
    return false;
  }

  RootedValue revokeVal(cx, ObjectValue(*revoker));
  if (!DefineDataProperty(cx, result, cx->names().proxy, proxyVal) ||
      !DefineDataProperty(cx, result, cx->names().revoke, revokeVal)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}
