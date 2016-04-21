/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "proxy/ScriptedIndirectProxyHandler.h"

#include "jsapi.h"
#include "jscntxt.h"

#include "jscntxtinlines.h"
#include "jsobjinlines.h"

using namespace js;

static bool
GetFundamentalTrap(JSContext* cx, HandleObject handler, HandlePropertyName name,
                   MutableHandleValue fvalp)
{
    JS_CHECK_RECURSION(cx, return false);

    return GetProperty(cx, handler, handler, name, fvalp);
}

static bool
GetDerivedTrap(JSContext* cx, HandleObject handler, HandlePropertyName name,
               MutableHandleValue fvalp)
{
    MOZ_ASSERT(name == cx->names().has ||
               name == cx->names().hasOwn ||
               name == cx->names().get ||
               name == cx->names().set ||
               name == cx->names().keys ||
               name == cx->names().iterate);

    return GetProperty(cx, handler, handler, name, fvalp);
}

static bool
Trap(JSContext* cx, HandleObject handler, HandleValue fval, unsigned argc, Value* argv,
     MutableHandleValue rval)
{
    return Invoke(cx, ObjectValue(*handler), fval, argc, argv, rval);
}

static bool
Trap1(JSContext* cx, HandleObject handler, HandleValue fval, HandleId id, MutableHandleValue rval)
{
    if (!IdToStringOrSymbol(cx, id, rval))
        return false;
    return Trap(cx, handler, fval, 1, rval.address(), rval);
}

static bool
Trap2(JSContext* cx, HandleObject handler, HandleValue fval, HandleId id, Value v_,
      MutableHandleValue rval)
{
    RootedValue v(cx, v_);
    if (!IdToStringOrSymbol(cx, id, rval))
        return false;
    JS::AutoValueArray<2> argv(cx);
    argv[0].set(rval);
    argv[1].set(v);
    return Trap(cx, handler, fval, 2, argv.begin(), rval);
}

static bool
IndicatePropertyNotFound(MutableHandle<PropertyDescriptor> desc)
{
    desc.object().set(nullptr);
    return true;
}

static bool
ValueToBool(HandleValue v, bool* bp)
{
    *bp = ToBoolean(v);
    return true;
}

static bool
ArrayToIdVector(JSContext* cx, const Value& array, AutoIdVector& props)
{
    MOZ_ASSERT(props.length() == 0);

    if (array.isPrimitive())
        return true;

    RootedObject obj(cx, &array.toObject());
    uint32_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    RootedValue v(cx);
    for (uint32_t n = 0; n < length; ++n) {
        if (!CheckForInterrupt(cx))
            return false;
        if (!GetElement(cx, obj, obj, n, &v))
            return false;
        RootedId id(cx);
        if (!ValueToId<CanGC>(cx, v, &id))
            return false;
        if (!props.append(id))
            return false;
    }

    return true;
}

namespace {

/*
 * Old-style indirect proxies allow callers to specify distinct scripted
 * [[Call]] and [[Construct]] traps. We use an intermediate object so that we
 * can stash this information in a single reserved slot on the proxy object.
 *
 * Note - Currently this is slightly unnecesary, because we actually have 2
 * extra slots, neither of which are used for ScriptedIndirectProxy. But we're
 * eventually moving towards eliminating one of those slots, and so we don't
 * want to add a dependency here.
 */
static const Class CallConstructHolder = {
    "CallConstructHolder",
    JSCLASS_HAS_RESERVED_SLOTS(2) | JSCLASS_IS_ANONYMOUS
};

} /* anonymous namespace */

// This variable exists solely to provide a unique address for use as an identifier.
const char ScriptedIndirectProxyHandler::family = 0;

bool
ScriptedIndirectProxyHandler::preventExtensions(JSContext* cx, HandleObject proxy,
                                                ObjectOpResult& result) const
{
    // Scripted indirect proxies don't support extensibility changes.
    return result.fail(JSMSG_CANT_CHANGE_EXTENSIBILITY);
}

bool
ScriptedIndirectProxyHandler::isExtensible(JSContext* cx, HandleObject proxy,
                                           bool* extensible) const
{
    // Scripted indirect proxies don't support extensibility changes.
    *extensible = true;
    return true;
}

static bool
ReturnedValueMustNotBePrimitive(JSContext* cx, HandleObject proxy, JSAtom* atom, const Value& v)
{
    if (v.isPrimitive()) {
        JSAutoByteString bytes;
        if (AtomToPrintableString(cx, atom, &bytes)) {
            RootedValue val(cx, ObjectOrNullValue(proxy));
            ReportValueError2(cx, JSMSG_BAD_TRAP_RETURN_VALUE,
                              JSDVG_SEARCH_STACK, val, nullptr, bytes.ptr());
        }
        return false;
    }
    return true;
}

static JSObject*
GetIndirectProxyHandlerObject(JSObject* proxy)
{
    return proxy->as<ProxyObject>().private_().toObjectOrNull();
}

bool
ScriptedIndirectProxyHandler::getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                                    MutableHandle<PropertyDescriptor> desc) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().getPropertyDescriptor, &fval) &&
           Trap1(cx, handler, fval, id, &value) &&
           ((value.isUndefined() && IndicatePropertyNotFound(desc)) ||
            (ReturnedValueMustNotBePrimitive(cx, proxy, cx->names().getPropertyDescriptor, value) &&
             ObjectToCompletePropertyDescriptor(cx, proxy, value, desc)));
}

bool
ScriptedIndirectProxyHandler::getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                                       MutableHandle<PropertyDescriptor> desc) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().getOwnPropertyDescriptor, &fval) &&
           Trap1(cx, handler, fval, id, &value) &&
           ((value.isUndefined() && IndicatePropertyNotFound(desc)) ||
            (ReturnedValueMustNotBePrimitive(cx, proxy, cx->names().getPropertyDescriptor, value) &&
             ObjectToCompletePropertyDescriptor(cx, proxy, value, desc)));
}

bool
ScriptedIndirectProxyHandler::defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                                             Handle<PropertyDescriptor> desc,
                                             ObjectOpResult& result) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().defineProperty, &fval) &&
           FromPropertyDescriptorToObject(cx, desc, &value) &&
           Trap2(cx, handler, fval, id, value, &value) &&
           result.succeed();
}

bool
ScriptedIndirectProxyHandler::ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                              AutoIdVector& props) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().getOwnPropertyNames, &fval) &&
           Trap(cx, handler, fval, 0, nullptr, &value) &&
           ArrayToIdVector(cx, value, props);
}

bool
ScriptedIndirectProxyHandler::delete_(JSContext* cx, HandleObject proxy, HandleId id,
                                      ObjectOpResult& result) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    if (!GetFundamentalTrap(cx, handler, cx->names().delete_, &fval))
        return false;
    if (!Trap1(cx, handler, fval, id, &value))
        return false;

    if (ToBoolean(value))
        result.succeed();
    else
        result.fail(JSMSG_PROXY_DELETE_RETURNED_FALSE);
    return true;
}

bool
ScriptedIndirectProxyHandler::enumerate(JSContext* cx, HandleObject proxy,
                                        MutableHandleObject objp) const
{
    // The hook that is called "enumerate" in the spec, used to be "iterate"
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue value(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().iterate, &value))
        return false;
    if (!IsCallable(value))
        return BaseProxyHandler::enumerate(cx, proxy, objp);

    RootedValue rval(cx);
    if (!Trap(cx, handler, value, 0, nullptr, &rval))
        return false;
    if (!ReturnedValueMustNotBePrimitive(cx, proxy, cx->names().iterate, rval))
        return false;
    objp.set(&rval.toObject());
    return true;
}

bool
ScriptedIndirectProxyHandler::has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().has, &fval))
        return false;
    if (!IsCallable(fval))
        return BaseProxyHandler::has(cx, proxy, id, bp);
    return Trap1(cx, handler, fval, id, &value) &&
           ValueToBool(value, bp);
}

bool
ScriptedIndirectProxyHandler::hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().hasOwn, &fval))
        return false;
    if (!IsCallable(fval))
        return BaseProxyHandler::hasOwn(cx, proxy, id, bp);
    return Trap1(cx, handler, fval, id, &value) &&
           ValueToBool(value, bp);
}

bool
ScriptedIndirectProxyHandler::get(JSContext* cx, HandleObject proxy, HandleValue receiver,
                                  HandleId id, MutableHandleValue vp) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue idv(cx);
    if (!IdToStringOrSymbol(cx, id, &idv))
        return false;
    JS::AutoValueArray<2> argv(cx);
    argv[0].set(receiver);
    argv[1].set(idv);
    RootedValue fval(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().get, &fval))
        return false;
    if (!IsCallable(fval))
        return BaseProxyHandler::get(cx, proxy, receiver, id, vp);
    return Trap(cx, handler, fval, 2, argv.begin(), vp);
}

bool
ScriptedIndirectProxyHandler::set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                                  HandleValue receiver, ObjectOpResult& result) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue idv(cx);
    if (!IdToStringOrSymbol(cx, id, &idv))
        return false;
    JS::AutoValueArray<3> argv(cx);
    argv[0].set(receiver);
    argv[1].set(idv);
    argv[2].set(v);
    RootedValue fval(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().set, &fval))
        return false;
    if (!IsCallable(fval))
        return derivedSet(cx, proxy, id, v, receiver, result);
    if (!Trap(cx, handler, fval, 3, argv.begin(), &idv))
        return false;
    return result.succeed();
}

static bool
CallSetter(JSContext* cx, HandleValue receiver, HandleId id, SetterOp op, unsigned attrs,
           HandleValue v, ObjectOpResult& result)
{
    if (attrs & JSPROP_SETTER) {
        RootedValue fval(cx, CastAsObjectJsval(op));
        if (!InvokeSetter(cx, receiver, fval, v))
            return false;
        return result.succeed();
    }

    if (attrs & JSPROP_GETTER)
        return result.fail(JSMSG_GETTER_ONLY);

    if (!receiver.isObject())
        return result.fail(JSMSG_SET_NON_OBJECT_RECEIVER);
    RootedObject receiverObj(cx, &receiver.toObject());

    if (!op)
        return result.succeed();

    RootedValue valCopy(cx, v);
    return CallJSSetterOp(cx, op, receiverObj, id, &valCopy, result);
}

bool
ScriptedIndirectProxyHandler::derivedSet(JSContext* cx, HandleObject proxy, HandleId id,
                                         HandleValue v, HandleValue receiver,
                                         ObjectOpResult& result) const
{
    // Find an own or inherited property. The code here is strange for maximum
    // backward compatibility with earlier code written before ES6 and before
    // SetPropertyIgnoringNamedGetter.
    //
    // As of March 2015, testing/specialpowers/content/specialpowersAPI.js
    // depends on the call to getPropertyDescriptor below, because it does
    // support inherited setters but makes no attempt to provide a meaningful
    // prototype chain.

    Rooted<PropertyDescriptor> desc(cx);
    if (!getOwnPropertyDescriptor(cx, proxy, id, &desc))
        return false;
    bool descIsOwn = desc.object() != nullptr;
    if (!descIsOwn) {
        if (!getPropertyDescriptor(cx, proxy, id, &desc))
            return false;
    }

    MOZ_ASSERT_IF(descIsOwn, desc.object());
    if (desc.object()) {
        MOZ_ASSERT(desc.getter() != JS_PropertyStub);
        MOZ_ASSERT(desc.setter() != JS_StrictPropertyStub);

        // Check for read-only properties.
        if (desc.isDataDescriptor() && !desc.writable())
            return result.fail(descIsOwn ? JSMSG_READ_ONLY : JSMSG_CANT_REDEFINE_PROP);

        if (desc.hasSetterObject() || desc.setter()) {
            if (!CallSetter(cx, receiver, id, desc.setter(), desc.attributes(), v, result))
                return false;
            if (!result)
                return true;
            if (!proxy->is<ProxyObject>() ||
                proxy->as<ProxyObject>().handler() != this ||
                desc.isShared())
            {
                return result.succeed();
            }
        }
        desc.value().set(v);

        if (descIsOwn) {
            MOZ_ASSERT(desc.object() == proxy);
            return this->defineProperty(cx, proxy, id, desc, result);
        }
    } else {
        desc.setDataDescriptor(v, JSPROP_ENUMERATE);
    }

    if (!receiver.isObject())
        return result.fail(JSMSG_SET_NON_OBJECT_RECEIVER);
    RootedObject receiverObj(cx, &receiver.toObject());
    return DefineProperty(cx, receiverObj, id, desc, result);
}

bool
ScriptedIndirectProxyHandler::getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                                           AutoIdVector& props) const
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue value(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().keys, &value))
        return false;
    if (!IsCallable(value))
        return BaseProxyHandler::getOwnEnumerablePropertyKeys(cx, proxy, props);
    return Trap(cx, handler, value, 0, nullptr, &value) &&
           ArrayToIdVector(cx, value, props);
}

bool
ScriptedIndirectProxyHandler::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                                         const CallArgs& args) const
{
    return BaseProxyHandler::nativeCall(cx, test, impl, args);
}

JSString*
ScriptedIndirectProxyHandler::fun_toString(JSContext* cx, HandleObject proxy, unsigned indent) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, GET);
    if (!proxy->isCallable()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                             JSMSG_INCOMPATIBLE_PROTO,
                             js_Function_str, js_toString_str,
                             "object");
        return nullptr;
    }
    RootedObject obj(cx, &proxy->as<ProxyObject>().extra(0).toObject().as<NativeObject>().getReservedSlot(0).toObject());
    return fun_toStringHelper(cx, obj, indent);
}

const ScriptedIndirectProxyHandler ScriptedIndirectProxyHandler::singleton;

bool
CallableScriptedIndirectProxyHandler::call(JSContext* cx, HandleObject proxy, const CallArgs& args) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, CALL);
    RootedObject ccHolder(cx, &proxy->as<ProxyObject>().extra(0).toObject());
    MOZ_ASSERT(ccHolder->getClass() == &CallConstructHolder);
    RootedValue call(cx, ccHolder->as<NativeObject>().getReservedSlot(0));
    MOZ_ASSERT(call.isObject() && call.toObject().isCallable());
    return Invoke(cx, args.thisv(), call, args.length(), args.array(), args.rval());
}

bool
CallableScriptedIndirectProxyHandler::construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const
{
    assertEnteredPolicy(cx, proxy, JSID_VOID, CALL);

    RootedObject ccHolder(cx, &proxy->as<ProxyObject>().extra(0).toObject());
    MOZ_ASSERT(ccHolder->getClass() == &CallConstructHolder);

    RootedValue construct(cx, ccHolder->as<NativeObject>().getReservedSlot(1));

    // We could enforce this at proxy creation time, but lipstick on a pig.
    // Plus, let's delay in-the-field bustage as long as possible.
    if (!IsConstructor(construct)) {
        ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, construct, nullptr);
        return false;
    }

    ConstructArgs cargs(cx);
    if (!FillArgumentsFromArraylike(cx, cargs, args))
        return false;

    return Construct(cx, construct, cargs, args.newTarget(), args.rval());
}

const CallableScriptedIndirectProxyHandler CallableScriptedIndirectProxyHandler::singleton;

bool
js::proxy_create(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             "create", "0", "s");
        return false;
    }
    JSObject* handler = NonNullObject(cx, args[0]);
    if (!handler)
        return false;
    JSObject* proto;
    if (args.get(1).isObject()) {
        proto = &args[1].toObject();
    } else {
        MOZ_ASSERT(IsFunctionObject(&args.callee()));
        proto = nullptr;
    }
    RootedValue priv(cx, ObjectValue(*handler));
    JSObject* proxy = NewProxyObject(cx, &ScriptedIndirectProxyHandler::singleton,
                                     priv, proto);
    if (!proxy)
        return false;

    args.rval().setObject(*proxy);
    return true;
}

bool
js::proxy_createFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 2) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             "createFunction", "1", "");
        return false;
    }
    RootedObject handler(cx, NonNullObject(cx, args[0]));
    if (!handler)
        return false;
    RootedObject proto(cx, args.callee().global().getOrCreateFunctionPrototype(cx));
    if (!proto)
        return false;

    RootedObject call(cx, ValueToCallable(cx, args[1], args.length() - 2));
    if (!call)
        return false;
    RootedObject construct(cx, nullptr);
    if (args.length() > 2) {
        construct = ValueToCallable(cx, args[2], args.length() - 3);
        if (!construct)
            return false;
    } else {
        construct = call;
    }

    // Stash the call and construct traps on a holder object that we can stick
    // in a slot on the proxy.
    RootedObject ccHolder(cx, JS_NewObjectWithGivenProto(cx, Jsvalify(&CallConstructHolder),
                                                         nullptr));
    if (!ccHolder)
        return false;
    ccHolder->as<NativeObject>().setReservedSlot(0, ObjectValue(*call));
    ccHolder->as<NativeObject>().setReservedSlot(1, ObjectValue(*construct));

    RootedValue priv(cx, ObjectValue(*handler));
    JSObject* proxy =
        NewProxyObject(cx, &CallableScriptedIndirectProxyHandler::singleton,
                       priv, proto);
    if (!proxy)
        return false;
    proxy->as<ProxyObject>().setExtra(0, ObjectValue(*ccHolder));

    args.rval().setObject(*proxy);
    return true;
}
