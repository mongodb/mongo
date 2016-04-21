/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "proxy/ScriptedDirectProxyHandler.h"

#include "jsapi.h"

#include "jsobjinlines.h"
#include "vm/NativeObject-inl.h"

#include "vm/NativeObject-inl.h"

using namespace js;

using JS::IsArrayAnswer;
using mozilla::ArrayLength;

static inline bool
IsDataDescriptor(const PropertyDescriptor& desc)
{
    return desc.obj && !(desc.attrs & (JSPROP_GETTER | JSPROP_SETTER));
}

static inline bool
IsAccessorDescriptor(const PropertyDescriptor& desc)
{
    return desc.obj && desc.attrs & (JSPROP_GETTER | JSPROP_SETTER);
}

// ES6 (5 April 2014) ValidateAndApplyPropertyDescriptor(O, P, Extensible, Desc, Current)
// Since we are actually performing 9.1.6.2 IsCompatiblePropertyDescriptor(Extensible, Desc,
// Current), some parameters are omitted.
static bool
ValidatePropertyDescriptor(JSContext* cx, bool extensible, Handle<PropertyDescriptor> desc,
                           Handle<PropertyDescriptor> current, bool* bp)
{
    // step 2
    if (!current.object()) {
        // Since |O| is always undefined, substeps c and d fall away.
        *bp = extensible;
        return true;
    }

    // step 3
    if (!desc.hasValue() && !desc.hasWritable() &&
        !desc.hasGetterObject() && !desc.hasSetterObject() &&
        !desc.hasEnumerable() && !desc.hasConfigurable())
    {
        *bp = true;
        return true;
    }

    // step 4
    if ((!desc.hasWritable() ||
         (current.hasWritable() && desc.writable() == current.writable())) &&
        (!desc.hasGetterObject() || desc.getter() == current.getter()) &&
        (!desc.hasSetterObject() || desc.setter() == current.setter()) &&
        (!desc.hasEnumerable() || desc.enumerable() == current.enumerable()) &&
        (!desc.hasConfigurable() || desc.configurable() == current.configurable()))
    {
        if (!desc.hasValue()) {
            *bp = true;
            return true;
        }
        bool same = false;
        if (!SameValue(cx, desc.value(), current.value(), &same))
            return false;
        if (same) {
            *bp = true;
            return true;
        }
    }

    // step 5
    if (!current.configurable()) {
        if (desc.hasConfigurable() && desc.configurable()) {
            *bp = false;
            return true;
        }

        if (desc.hasEnumerable() && desc.enumerable() != current.enumerable()) {
            *bp = false;
            return true;
        }
    }

    // step 6
    if (desc.isGenericDescriptor()) {
        *bp = true;
        return true;
    }

    // step 7a
    if (current.isDataDescriptor() != desc.isDataDescriptor()) {
        *bp = current.configurable();
        return true;
    }

    // step 8
    if (current.isDataDescriptor()) {
        MOZ_ASSERT(desc.isDataDescriptor()); // by step 7a
        if (!current.configurable() && !current.writable()) {
            if (desc.hasWritable() && desc.writable()) {
                *bp = false;
                return true;
            }

            if (desc.hasValue()) {
                bool same;
                if (!SameValue(cx, desc.value(), current.value(), &same))
                    return false;
                if (!same) {
                    *bp = false;
                    return true;
                }
            }
        }

        *bp = true;
        return true;
    }

    // step 9
    MOZ_ASSERT(current.isAccessorDescriptor()); // by step 8
    MOZ_ASSERT(desc.isAccessorDescriptor()); // by step 7a
    *bp = (current.configurable() ||
           ((!desc.hasSetterObject() || desc.setter() == current.setter()) &&
            (!desc.hasGetterObject() || desc.getter() == current.getter())));
    return true;
}

// Get the [[ProxyHandler]] of a scripted direct proxy.
static JSObject*
GetDirectProxyHandlerObject(JSObject* proxy)
{
    MOZ_ASSERT(proxy->as<ProxyObject>().handler() == &ScriptedDirectProxyHandler::singleton);
    return proxy->as<ProxyObject>().extra(ScriptedDirectProxyHandler::HANDLER_EXTRA).toObjectOrNull();
}

static inline void
ReportInvalidTrapResult(JSContext* cx, JSObject* proxy, JSAtom* atom)
{
    RootedValue v(cx, ObjectOrNullValue(proxy));
    JSAutoByteString bytes;
    if (!AtomToPrintableString(cx, atom, &bytes))
        return;
    ReportValueError2(cx, JSMSG_INVALID_TRAP_RESULT, JSDVG_IGNORE_STACK, v,
                      nullptr, bytes.ptr());
}

// ES6 implements both getPrototype and setPrototype traps. We don't have them yet (see bug
// 888969). For now, use these, to account for proxy revocation.
bool
ScriptedDirectProxyHandler::getPrototype(JSContext* cx, HandleObject proxy,
                                         MutableHandleObject protop) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    // Though handler is used elsewhere, spec mandates that both get set to null.
    if (!target) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    return GetPrototype(cx, target, protop);
}

bool
ScriptedDirectProxyHandler::setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                                         ObjectOpResult& result) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    if (!target) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    return SetPrototype(cx, target, proto, result);
}

// Not yet part of ES6, but hopefully to be standards-tracked -- and needed to
// handle revoked proxies in any event.
bool
ScriptedDirectProxyHandler::setImmutablePrototype(JSContext* cx, HandleObject proxy,
                                                  bool* succeeded) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    if (!target) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    return SetImmutablePrototype(cx, target, succeeded);
}

// ES6 draft rev 32 (2 Feb 2015) 9.5.4 Proxy.[[PreventExtensions]]()
bool
ScriptedDirectProxyHandler::preventExtensions(JSContext* cx, HandleObject proxy,
                                              ObjectOpResult& result) const
{
    // Steps 1-3.
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // Step 4.
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // Steps 5-6.
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().preventExtensions, &trap))
        return false;

    // Step 7.
    if (trap.isUndefined())
        return PreventExtensions(cx, target, result);

    // Steps 8-9.
    Value argv[] = {
        ObjectValue(*target)
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // Steps 10-11.
    if (ToBoolean(trapResult)) {
        bool extensible;
        if (!IsExtensible(cx, target, &extensible))
            return false;
        if (extensible) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                 JSMSG_CANT_REPORT_AS_NON_EXTENSIBLE);
            return false;
        }
        return result.succeed();
    }
    return result.fail(JSMSG_PROXY_PREVENTEXTENSIONS_RETURNED_FALSE);
}

// ES6 (5 April, 2014) 9.5.3 Proxy.[[IsExtensible]]()
bool
ScriptedDirectProxyHandler::isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const
{
    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 3
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // step 4-5
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().isExtensible, &trap))
        return false;

    // step 6
    if (trap.isUndefined())
        return IsExtensible(cx, target, extensible);

    // step 7, 9
    Value argv[] = {
        ObjectValue(*target)
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // step 8
    bool booleanTrapResult = ToBoolean(trapResult);

    // step 10-11
    bool targetResult;
    if (!IsExtensible(cx, target, &targetResult))
        return false;

    // step 12
    if (targetResult != booleanTrapResult) {
       JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_EXTENSIBILITY);
       return false;
    }

    // step 13
    *extensible = booleanTrapResult;
    return true;
}

// ES6 (5 April 2014) 9.5.5 Proxy.[[GetOwnProperty]](P)
bool
ScriptedDirectProxyHandler::getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                                     MutableHandle<PropertyDescriptor> desc) const
{
    // step 2
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 3
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 4
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // step 5-6
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().getOwnPropertyDescriptor, &trap))
        return false;

    // step 7
    if (trap.isUndefined())
        return GetOwnPropertyDescriptor(cx, target, id, desc);

    // step 8-9
    RootedValue propKey(cx);
    if (!IdToStringOrSymbol(cx, id, &propKey))
        return false;

    Value argv[] = {
        ObjectValue(*target),
        propKey
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // step 10
    if (!trapResult.isUndefined() && !trapResult.isObject()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_GETOWN_OBJORUNDEF);
        return false;
    }

    //step 11-12
    Rooted<PropertyDescriptor> targetDesc(cx);
    if (!GetOwnPropertyDescriptor(cx, target, id, &targetDesc))
        return false;

    // step 13
    if (trapResult.isUndefined()) {
        // substep a
        if (!targetDesc.object()) {
            desc.object().set(nullptr);
            return true;
        }

        // substep b
        if (!targetDesc.configurable()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_NC_AS_NE);
            return false;
        }

        // substep c-e
        bool extensibleTarget;
        if (!IsExtensible(cx, target, &extensibleTarget))
            return false;
        if (!extensibleTarget) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_E_AS_NE);
            return false;
        }

        // substep f
        desc.object().set(nullptr);
        return true;
    }

    // step 14-15
    bool extensibleTarget;
    if (!IsExtensible(cx, target, &extensibleTarget))
        return false;

    // step 16-17
    Rooted<PropertyDescriptor> resultDesc(cx);
    if (!ToPropertyDescriptor(cx, trapResult, true, &resultDesc))
        return false;

    // step 18
    CompletePropertyDescriptor(&resultDesc);

    // step 19
    bool valid;
    if (!ValidatePropertyDescriptor(cx, extensibleTarget, resultDesc, targetDesc, &valid))
        return false;

    // step 20
    if (!valid) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_INVALID);
        return false;
    }

    // step 21
    if (!resultDesc.configurable()) {
        if (!targetDesc.object()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_NE_AS_NC);
            return false;
        }

        if (targetDesc.configurable()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_C_AS_NC);
            return false;
        }
    }

    // step 22
    desc.set(resultDesc);
    desc.object().set(proxy);
    return true;
}

// ES6 draft rev 31 (15 Jan 2015) 9.5.6 Proxy.[[DefineOwnProperty]](P, Desc)
bool
ScriptedDirectProxyHandler::defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                                           Handle<PropertyDescriptor> desc,
                                           ObjectOpResult& result) const
{
    // steps 2-4
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 5
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // steps 6-7
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().defineProperty, &trap))
        return false;

    // step 8
    if (trap.isUndefined())
        return DefineProperty(cx, target, id, desc, result);

    // step 9
    RootedValue descObj(cx);
    if (!FromPropertyDescriptorToObject(cx, desc, &descObj))
        return false;

    // steps 10-11
    RootedValue propKey(cx);
    if (!IdToStringOrSymbol(cx, id, &propKey))
        return false;

    Value argv[] = {
        ObjectValue(*target),
        propKey,
        descObj
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // step 12
    if (!ToBoolean(trapResult))
        return result.fail(JSMSG_PROXY_DEFINE_RETURNED_FALSE);

    // step 13-14
    Rooted<PropertyDescriptor> targetDesc(cx);
    if (!GetOwnPropertyDescriptor(cx, target, id, &targetDesc))
        return false;

    // step 15-16
    bool extensibleTarget;
    if (!IsExtensible(cx, target, &extensibleTarget))
        return false;

    // step 17-18
    bool settingConfigFalse = desc.hasConfigurable() && !desc.configurable();
    if (!targetDesc.object()) {
        // step 19.a
        if (!extensibleTarget) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_DEFINE_NEW);
            return false;
        }
        // step 19.b
        if (settingConfigFalse) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_DEFINE_NE_AS_NC);
            return false;
        }
    } else {
        // step 20
        bool valid;
        if (!ValidatePropertyDescriptor(cx, extensibleTarget, desc, targetDesc, &valid))
            return false;
        if (!valid || (settingConfigFalse && targetDesc.configurable())) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_DEFINE_INVALID);
            return false;
        }
    }

    // step 21
    return result.succeed();
}

// ES6 7.3.17 But elementTypes is is fixed to symbol/string.
static bool
CreateFilteredListFromArrayLike(JSContext* cx, HandleValue v, AutoIdVector& props)
{
    // Step 3.
    RootedObject obj(cx, NonNullObject(cx, v));
    if (!obj)
        return false;

    // Steps 4-5.
    uint32_t len;
    if (!GetLengthProperty(cx, obj, &len))
        return false;

    // Steps 6-8.
    RootedValue next(cx);
    RootedId id(cx);
    for (uint32_t index = 0; index < len; index++) {
        if (!GetElement(cx, obj, obj, index, &next))
            return false;

        if (!next.isString() && !next.isSymbol()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_ONWKEYS_STR_SYM);
            return false;
        }

        // Unobservable for strings/symbols.
        if (!ValueToId<CanGC>(cx, next, &id))
            return false;

        if (!props.append(id))
            return false;
    }

    // Step 9.
    return true;
}


// ES6 9.5.12 Proxy.[[OwnPropertyKeys]]()
bool
ScriptedDirectProxyHandler::ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                            AutoIdVector& props) const
{
    // Step 1.
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // Step 2.
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }
    // Step 3. Superfluous assertion.

    // Step 4.
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // Steps 5-6.
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().ownKeys, &trap))
        return false;

    // Step 7.
    if (trap.isUndefined())
        return GetPropertyKeys(cx, target, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &props);

    // Step 8.
    Value argv[] = {
        ObjectValue(*target)
    };
    RootedValue trapResultArray(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResultArray))
        return false;

    // Steps 9-10.
    AutoIdVector trapResult(cx);
    if (!CreateFilteredListFromArrayLike(cx, trapResultArray, trapResult))
        return false;

    // Steps 11-12.
    bool extensibleTarget;
    if (!IsExtensible(cx, target, &extensibleTarget))
        return false;

    // Steps 13-14.
    AutoIdVector targetKeys(cx);
    if (!GetPropertyKeys(cx, target, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &targetKeys))
        return false;

    // Step 15. Superfluous assertion.

    // Steps 16-17.
    AutoIdVector targetConfigurableKeys(cx);
    AutoIdVector targetNonconfigurableKeys(cx);

    // Step 18.
    Rooted<PropertyDescriptor> desc(cx);
    for (size_t i = 0; i < targetKeys.length(); ++i) {
        // Steps a-b.
        if (!GetOwnPropertyDescriptor(cx, target, targetKeys[i], &desc))
            return false;

        // Steps c-d.
        if (desc.object() && !desc.configurable()) {
            if (!targetNonconfigurableKeys.append(targetKeys[i]))
                return false;
        } else {
            if (!targetConfigurableKeys.append(targetKeys[i]))
                return false;
        }
    }

    // Step 19.
    if (extensibleTarget && targetNonconfigurableKeys.empty())
        return props.appendAll(trapResult);

    // Step 20.
    AutoIdVector uncheckedResultKeys(cx);
    if (!uncheckedResultKeys.appendAll(trapResult))
        return false;

    // Step 21.
    for (size_t i = 0; i < targetNonconfigurableKeys.length(); ++i) {
        RootedId key(cx, targetNonconfigurableKeys[i]);
        MOZ_ASSERT(key != JSID_VOID);

        bool found = false;
        for (size_t j = 0; j < uncheckedResultKeys.length(); ++j) {
            if (key == uncheckedResultKeys[j]) {
                uncheckedResultKeys[j].set(JSID_VOID);
                found = true;
                break;
            }
        }

        if (!found) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_SKIP_NC);
            return false;
        }
    }

    // Step 22.
    if (extensibleTarget)
        return props.appendAll(trapResult);

    // Step 23.
    for (size_t i = 0; i < targetConfigurableKeys.length(); ++i) {
        RootedId key(cx, targetConfigurableKeys[i]);
        MOZ_ASSERT(key != JSID_VOID);

        bool found = false;
        for (size_t j = 0; j < uncheckedResultKeys.length(); ++j) {
            if (key == uncheckedResultKeys[j]) {
                uncheckedResultKeys[j].set(JSID_VOID);
                found = true;
                break;
            }
        }

        if (!found) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_E_AS_NE);
            return false;
        }
    }

    // Step 24.
    for (size_t i = 0; i < uncheckedResultKeys.length(); ++i) {
        if (uncheckedResultKeys[i].get() != JSID_VOID) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_NEW);
            return false;
        }
    }

    // Step 25.
    return props.appendAll(trapResult);
}

// ES6 draft rev 32 (2 Feb 2014) 9.5.10 Proxy.[[Delete]](P)
bool
ScriptedDirectProxyHandler::delete_(JSContext* cx, HandleObject proxy, HandleId id,
                                    ObjectOpResult& result) const
{
    // step 2
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 3
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // steps 4-5
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // steps 6-7
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().deleteProperty, &trap))
        return false;

    // step 8
    if (trap.isUndefined())
        return DeleteProperty(cx, target, id, result);

    // steps 9-10
    RootedValue value(cx);
    if (!IdToStringOrSymbol(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectValue(*target),
        value
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // step 11
    if (!ToBoolean(trapResult))
        return result.fail(JSMSG_PROXY_DELETE_RETURNED_FALSE);

    // steps 12-13
    Rooted<PropertyDescriptor> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, target, id, &desc))
        return false;

    // step 14-15
    if (desc.object() && !desc.configurable()) {
        RootedValue v(cx, IdToValue(id));
        ReportValueError(cx, JSMSG_CANT_DELETE, JSDVG_IGNORE_STACK, v, nullptr);
        return false;
    }

    // step 16
    return result.succeed();
}

// ES6 (14 October, 2014) 9.5.11 Proxy.[[Enumerate]]
bool
ScriptedDirectProxyHandler::enumerate(JSContext* cx, HandleObject proxy,
                                      MutableHandleObject objp) const
{
    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 3: unnecessary assert
    // step 4
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // step 5-6
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().enumerate, &trap))
        return false;

    // step 7
    if (trap.isUndefined())
        return GetIterator(cx, target, 0, objp);

    // step 8-9
    Value argv[] = {
        ObjectOrNullValue(target)
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // step 10
    if (trapResult.isPrimitive()) {
        ReportInvalidTrapResult(cx, proxy, cx->names().enumerate);
        return false;
    }

    // step 11
    objp.set(&trapResult.toObject());
    return true;
}

// ES6 (22 May, 2014) 9.5.7 Proxy.[[HasProperty]](P)
bool
ScriptedDirectProxyHandler::has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const
{
    // step 2
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 3
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 4
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // step 5-6
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().has, &trap))
        return false;

    // step 7
    if (trap.isUndefined())
        return HasProperty(cx, target, id, bp);

    // step 8,10
    RootedValue value(cx);
    if (!IdToStringOrSymbol(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectOrNullValue(target),
        value
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // step 9
    bool success = ToBoolean(trapResult);

    // step 11
    if (!success) {
        Rooted<PropertyDescriptor> desc(cx);
        if (!GetOwnPropertyDescriptor(cx, target, id, &desc))
            return false;

        if (desc.object()) {
            if (!desc.configurable()) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_NC_AS_NE);
                return false;
            }

            bool extensible;
            if (!IsExtensible(cx, target, &extensible))
                return false;
            if (!extensible) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_REPORT_E_AS_NE);
                return false;
            }
        }
    }

    // step 12
    *bp = success;
    return true;
}

// ES6 (22 May, 2014) 9.5.8 Proxy.[[GetP]](P, Receiver)
bool
ScriptedDirectProxyHandler::get(JSContext* cx, HandleObject proxy, HandleValue receiver,
                                HandleId id, MutableHandleValue vp) const
{
    // step 2
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 3
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 4
    RootedObject target(cx, proxy->as<ProxyObject>().target());

    // step 5-6
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().get, &trap))
        return false;

    // step 7
    if (trap.isUndefined())
        return GetProperty(cx, target, receiver, id, vp);

    // step 8-9
    RootedValue value(cx);
    if (!IdToStringOrSymbol(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectOrNullValue(target),
        value,
        receiver
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // step 10-11
    Rooted<PropertyDescriptor> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, target, id, &desc))
        return false;

    // step 12
    if (desc.object()) {
        if (desc.isDataDescriptor() && !desc.configurable() && !desc.writable()) {
            bool same;
            if (!SameValue(cx, trapResult, desc.value(), &same))
                return false;
            if (!same) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MUST_REPORT_SAME_VALUE);
                return false;
            }
        }

        if (desc.isAccessorDescriptor() && !desc.configurable() && desc.getterObject() == nullptr) {
            if (!trapResult.isUndefined()) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MUST_REPORT_UNDEFINED);
                return false;
            }
        }
    }

    // step 13
    vp.set(trapResult);
    return true;
}

// ES6 draft rev 32 (2015 Feb 2) 9.5.9 Proxy.[[Set]](P, V, Receiver)
bool
ScriptedDirectProxyHandler::set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                                HandleValue receiver, ObjectOpResult& result) const
{
    // step 2-3 (Steps 1 and 4 are irrelevant assertions.)
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 5-7
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().set, &trap))
        return false;

    // step 8
    if (trap.isUndefined())
        return SetProperty(cx, target, id, v, receiver, result);

    // step 9-10
    RootedValue value(cx);
    if (!IdToStringOrSymbol(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectOrNullValue(target),
        value,
        v.get(),
        receiver.get()
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, ArrayLength(argv), argv, &trapResult))
        return false;

    // step 11
    if (!ToBoolean(trapResult))
        return result.fail(JSMSG_PROXY_SET_RETURNED_FALSE);

    // step 12-13
    Rooted<PropertyDescriptor> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, target, id, &desc))
        return false;

    // step 14
    if (desc.object()) {
        if (desc.isDataDescriptor() && !desc.configurable() && !desc.writable()) {
            bool same;
            if (!SameValue(cx, v, desc.value(), &same))
                return false;
            if (!same) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_SET_NW_NC);
                return false;
            }
        }

        if (desc.isAccessorDescriptor() && !desc.configurable() && desc.setterObject() == nullptr) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_SET_WO_SETTER);
            return false;
        }
    }

    // step 15
    return result.succeed();
}

// ES6 (22 May, 2014) 9.5.13 Proxy.[[Call]]
bool
ScriptedDirectProxyHandler::call(JSContext* cx, HandleObject proxy, const CallArgs& args) const
{
    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 3
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    MOZ_ASSERT(target->isCallable());

    // step 7
    RootedObject argsArray(cx, NewDenseCopiedArray(cx, args.length(), args.array()));
    if (!argsArray)
        return false;

    // step 4-5
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().apply, &trap))
        return false;

    // step 6
    if (trap.isUndefined()) {
        RootedValue targetv(cx, ObjectValue(*target));
        return Invoke(cx, args.thisv(), targetv, args.length(), args.array(), args.rval());
    }

    // step 8
    Value argv[] = {
        ObjectValue(*target),
        args.thisv(),
        ObjectValue(*argsArray)
    };
    RootedValue thisValue(cx, ObjectValue(*handler));
    return Invoke(cx, thisValue, trap, ArrayLength(argv), argv, args.rval());
}

// ES6 (22 May, 2014) 9.5.14 Proxy.[[Construct]]
bool
ScriptedDirectProxyHandler::construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const
{
    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    if (!handler) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    // step 3
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    MOZ_ASSERT(target->isConstructor());

    // step 7
    RootedObject argsArray(cx, NewDenseCopiedArray(cx, args.length(), args.array()));
    if (!argsArray)
        return false;

    // step 4-5
    RootedValue trap(cx);
    if (!GetProperty(cx, handler, handler, cx->names().construct, &trap))
        return false;

    // step 6
    if (trap.isUndefined()) {
        ConstructArgs cargs(cx);
        if (!FillArgumentsFromArraylike(cx, cargs, args))
            return false;

        RootedValue targetv(cx, ObjectValue(*target));
        return Construct(cx, targetv, cargs, args.newTarget(), args.rval());
    }

    // step 8-9
    Value constructArgv[] = {
        ObjectValue(*target),
        ObjectValue(*argsArray),
        args.newTarget()
    };
    RootedValue thisValue(cx, ObjectValue(*handler));
    if (!Invoke(cx, thisValue, trap, ArrayLength(constructArgv), constructArgv, args.rval()))
        return false;

    // step 10
    if (!args.rval().isObject()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_CONSTRUCT_OBJECT);
        return false;
    }
    return true;
}

bool
ScriptedDirectProxyHandler::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                                       const CallArgs& args) const
{
    ReportIncompatible(cx, args);
    return false;
}

bool
ScriptedDirectProxyHandler::hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                                        bool* bp) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    if (!target) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    return HasInstance(cx, target, v, bp);
}

bool
ScriptedDirectProxyHandler::getBuiltinClass(JSContext* cx, HandleObject proxy,
                                            ESClassValue* classValue) const
{
    *classValue = ESClass_Other;
    return true;
}

bool
ScriptedDirectProxyHandler::isArray(JSContext* cx, HandleObject proxy,
                                    IsArrayAnswer* answer) const
{
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    if (target)
        return JS::IsArray(cx, target, answer);

    *answer = IsArrayAnswer::RevokedProxy;
    return true;
}

const char*
ScriptedDirectProxyHandler::className(JSContext* cx, HandleObject proxy) const
{
    // Right now the caller is not prepared to handle failures.
    RootedObject target(cx, proxy->as<ProxyObject>().target());
    if (!target)
        return BaseProxyHandler::className(cx, proxy);

    return GetObjectClassName(cx, target);
}
JSString*
ScriptedDirectProxyHandler::fun_toString(JSContext* cx, HandleObject proxy,
                                         unsigned indent) const
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                         js_Function_str, js_toString_str, "object");
    return nullptr;
}

bool
ScriptedDirectProxyHandler::regexp_toShared(JSContext* cx, HandleObject proxy,
                                            RegExpGuard* g) const
{
    MOZ_CRASH("Should not end up in ScriptedDirectProxyHandler::regexp_toShared");
    return false;
}

bool
ScriptedDirectProxyHandler::boxedValue_unbox(JSContext* cx, HandleObject proxy,
                                             MutableHandleValue vp) const
{
    MOZ_CRASH("Should not end up in ScriptedDirectProxyHandler::boxedValue_unbox");
    return false;
}

bool
ScriptedDirectProxyHandler::isCallable(JSObject* obj) const
{
    MOZ_ASSERT(obj->as<ProxyObject>().handler() == &ScriptedDirectProxyHandler::singleton);
    uint32_t callConstruct = obj->as<ProxyObject>().extra(IS_CALLCONSTRUCT_EXTRA).toPrivateUint32();
    return !!(callConstruct & IS_CALLABLE);
}

bool
ScriptedDirectProxyHandler::isConstructor(JSObject* obj) const
{
    MOZ_ASSERT(obj->as<ProxyObject>().handler() == &ScriptedDirectProxyHandler::singleton);
    uint32_t callConstruct = obj->as<ProxyObject>().extra(IS_CALLCONSTRUCT_EXTRA).toPrivateUint32();
    return !!(callConstruct & IS_CONSTRUCTOR);
}

const char ScriptedDirectProxyHandler::family = 0;
const ScriptedDirectProxyHandler ScriptedDirectProxyHandler::singleton;

bool
IsRevokedScriptedProxy(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    return obj && IsScriptedProxy(obj) && !obj->as<ProxyObject>().target();
}

// ES6 draft rc4 9.5.15.
static bool
NewScriptedProxy(JSContext* cx, CallArgs& args, const char* callerName)
{
    if (args.length() < 2) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             callerName, "1", "s");
        return false;
    }

    // Step 1.
    RootedObject target(cx, NonNullObject(cx, args[0]));
    if (!target)
        return false;

    // Step 2.
    if (IsRevokedScriptedProxy(target)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_ARG_REVOKED, "1");
        return false;
    }

    // Step 3.
    RootedObject handler(cx, NonNullObject(cx, args[1]));
    if (!handler)
        return false;

    // Step 4.
    if (IsRevokedScriptedProxy(handler)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_ARG_REVOKED, "2");
        return false;
    }

    // Steps 5-6, and 8 (reordered).
    RootedValue priv(cx, ObjectValue(*target));
    JSObject* proxy_ =
        NewProxyObject(cx, &ScriptedDirectProxyHandler::singleton,
                       priv, TaggedProto::LazyProto);
    if (!proxy_)
        return false;

    // Step 9 (reordered).
    Rooted<ProxyObject*> proxy(cx, &proxy_->as<ProxyObject>());
    proxy->setExtra(ScriptedDirectProxyHandler::HANDLER_EXTRA, ObjectValue(*handler));

    // Step 7, Assign [[Call]] and [[Construct]].
    uint32_t callable = target->isCallable() ? ScriptedDirectProxyHandler::IS_CALLABLE : 0;
    uint32_t constructor = target->isConstructor() ? ScriptedDirectProxyHandler::IS_CONSTRUCTOR : 0;
    proxy->as<ProxyObject>().setExtra(ScriptedDirectProxyHandler::IS_CALLCONSTRUCT_EXTRA,
                                      PrivateUint32Value(callable | constructor));

    // Step 10.
    args.rval().setObject(*proxy);
    return true;
}

bool
js::proxy(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "Proxy"))
        return false;

    return NewScriptedProxy(cx, args, "Proxy");
}

static bool
RevokeProxy(JSContext* cx, unsigned argc, Value* vp)
{
    CallReceiver rec = CallReceiverFromVp(vp);

    RootedFunction func(cx, &rec.callee().as<JSFunction>());
    RootedObject p(cx, func->getExtendedSlot(ScriptedDirectProxyHandler::REVOKE_SLOT).toObjectOrNull());

    if (p) {
        func->setExtendedSlot(ScriptedDirectProxyHandler::REVOKE_SLOT, NullValue());

        MOZ_ASSERT(p->is<ProxyObject>());

        p->as<ProxyObject>().setSameCompartmentPrivate(NullValue());
        p->as<ProxyObject>().setExtra(ScriptedDirectProxyHandler::HANDLER_EXTRA, NullValue());
    }

    rec.rval().setUndefined();
    return true;
}

bool
js::proxy_revocable(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!NewScriptedProxy(cx, args, "Proxy.revocable"))
        return false;

    RootedValue proxyVal(cx, args.rval());
    MOZ_ASSERT(proxyVal.toObject().is<ProxyObject>());

    RootedObject revoker(cx, NewFunctionByIdWithReserved(cx, RevokeProxy, 0, 0,
                         AtomToId(cx->names().revoke)));
    if (!revoker)
        return false;

    revoker->as<JSFunction>().initExtendedSlot(ScriptedDirectProxyHandler::REVOKE_SLOT, proxyVal);

    RootedPlainObject result(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!result)
        return false;

    RootedValue revokeVal(cx, ObjectValue(*revoker));
    if (!DefineProperty(cx, result, cx->names().proxy, proxyVal) ||
        !DefineProperty(cx, result, cx->names().revoke, revokeVal))
    {
        return false;
    }

    args.rval().setObject(*result);
    return true;
}
