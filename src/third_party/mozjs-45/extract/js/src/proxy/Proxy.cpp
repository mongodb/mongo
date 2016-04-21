/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Proxy.h"

#include <string.h>

#include "jsapi.h"
#include "jscntxt.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jswrapper.h"

#include "gc/Marking.h"
#include "proxy/DeadObjectProxy.h"
#include "proxy/ScriptedDirectProxyHandler.h"
#include "proxy/ScriptedIndirectProxyHandler.h"
#include "vm/WrapperObject.h"

#include "jsatominlines.h"
#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::gc;

void
js::AutoEnterPolicy::reportErrorIfExceptionIsNotPending(JSContext* cx, jsid id)
{
    if (JS_IsExceptionPending(cx))
        return;

    if (JSID_IS_VOID(id)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                             JSMSG_OBJECT_ACCESS_DENIED);
    } else {
        RootedValue idVal(cx, IdToValue(id));
        JSString* str = ValueToSource(cx, idVal);
        if (!str) {
            return;
        }
        AutoStableStringChars chars(cx);
        const char16_t* prop = nullptr;
        if (str->ensureFlat(cx) && chars.initTwoByte(cx, str))
            prop = chars.twoByteChars();

        JS_ReportErrorNumberUC(cx, GetErrorMessage, nullptr, JSMSG_PROPERTY_ACCESS_DENIED,
                               prop);
    }
}

#ifdef DEBUG
void
js::AutoEnterPolicy::recordEnter(JSContext* cx, HandleObject proxy, HandleId id, Action act)
{
    if (allowed()) {
        context = cx;
        enteredProxy.emplace(proxy);
        enteredId.emplace(id);
        enteredAction = act;
        prev = cx->runtime()->enteredPolicy;
        cx->runtime()->enteredPolicy = this;
    }
}

void
js::AutoEnterPolicy::recordLeave()
{
    if (enteredProxy) {
        MOZ_ASSERT(context->runtime()->enteredPolicy == this);
        context->runtime()->enteredPolicy = prev;
    }
}

JS_FRIEND_API(void)
js::assertEnteredPolicy(JSContext* cx, JSObject* proxy, jsid id,
                        BaseProxyHandler::Action act)
{
    MOZ_ASSERT(proxy->is<ProxyObject>());
    MOZ_ASSERT(cx->runtime()->enteredPolicy);
    MOZ_ASSERT(cx->runtime()->enteredPolicy->enteredProxy->get() == proxy);
    MOZ_ASSERT(cx->runtime()->enteredPolicy->enteredId->get() == id);
    MOZ_ASSERT(cx->runtime()->enteredPolicy->enteredAction & act);
}
#endif

bool
Proxy::getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                             MutableHandle<PropertyDescriptor> desc)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    desc.object().set(nullptr); // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET_PROPERTY_DESCRIPTOR, true);
    if (!policy.allowed())
        return policy.returnValue();

    // Special case. See the comment on BaseProxyHandler::mHasPrototype.
    if (handler->hasPrototype())
        return handler->BaseProxyHandler::getPropertyDescriptor(cx, proxy, id, desc);

    return handler->getPropertyDescriptor(cx, proxy, id, desc);
}

bool
Proxy::getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                MutableHandle<PropertyDescriptor> desc)
{
    JS_CHECK_RECURSION(cx, return false);

    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    desc.object().set(nullptr); // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET_PROPERTY_DESCRIPTOR, true);
    if (!policy.allowed())
        return policy.returnValue();
    return handler->getOwnPropertyDescriptor(cx, proxy, id, desc);
}

bool
Proxy::defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                      Handle<PropertyDescriptor> desc, ObjectOpResult& result)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
    if (!policy.allowed()) {
        if (!policy.returnValue())
            return false;
        return result.succeed();
    }
    return proxy->as<ProxyObject>().handler()->defineProperty(cx, proxy, id, desc, result);
}

bool
Proxy::ownPropertyKeys(JSContext* cx, HandleObject proxy, AutoIdVector& props)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE, BaseProxyHandler::ENUMERATE, true);
    if (!policy.allowed())
        return policy.returnValue();
    return proxy->as<ProxyObject>().handler()->ownPropertyKeys(cx, proxy, props);
}

bool
Proxy::delete_(JSContext* cx, HandleObject proxy, HandleId id, ObjectOpResult& result)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
    if (!policy.allowed()) {
        bool ok = policy.returnValue();
        if (ok)
            result.succeed();
        return ok;
    }
    return proxy->as<ProxyObject>().handler()->delete_(cx, proxy, id, result);
}

JS_FRIEND_API(bool)
js::AppendUnique(JSContext* cx, AutoIdVector& base, AutoIdVector& others)
{
    AutoIdVector uniqueOthers(cx);
    if (!uniqueOthers.reserve(others.length()))
        return false;
    for (size_t i = 0; i < others.length(); ++i) {
        bool unique = true;
        for (size_t j = 0; j < base.length(); ++j) {
            if (others[i].get() == base[j]) {
                unique = false;
                break;
            }
        }
        if (unique)
            uniqueOthers.append(others[i]);
    }
    return base.appendAll(uniqueOthers);
}

/* static */ bool
Proxy::getPrototype(JSContext* cx, HandleObject proxy, MutableHandleObject proto)
{
    MOZ_ASSERT(proxy->hasLazyPrototype());
    JS_CHECK_RECURSION(cx, return false);
    return proxy->as<ProxyObject>().handler()->getPrototype(cx, proxy, proto);
}

/* static */ bool
Proxy::setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto, ObjectOpResult& result)
{
    MOZ_ASSERT(proxy->hasLazyPrototype());
    JS_CHECK_RECURSION(cx, return false);
    return proxy->as<ProxyObject>().handler()->setPrototype(cx, proxy, proto, result);
}

/* static */ bool
Proxy::setImmutablePrototype(JSContext* cx, HandleObject proxy, bool* succeeded)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    return handler->setImmutablePrototype(cx, proxy, succeeded);
}

/* static */ bool
Proxy::preventExtensions(JSContext* cx, HandleObject proxy, ObjectOpResult& result)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    return handler->preventExtensions(cx, proxy, result);
}

/* static */ bool
Proxy::isExtensible(JSContext* cx, HandleObject proxy, bool* extensible)
{
    JS_CHECK_RECURSION(cx, return false);
    return proxy->as<ProxyObject>().handler()->isExtensible(cx, proxy, extensible);
}

bool
Proxy::has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    *bp = false; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();

    if (handler->hasPrototype()) {
        if (!handler->hasOwn(cx, proxy, id, bp))
            return false;
        if (*bp)
            return true;

        RootedObject proto(cx);
        if (!GetPrototype(cx, proxy, &proto))
            return false;
        if (!proto)
            return true;

        return HasProperty(cx, proto, id, bp);
    }

    return handler->has(cx, proxy, id, bp);
}

bool
Proxy::hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    *bp = false; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return handler->hasOwn(cx, proxy, id, bp);
}

static Value
ValueToWindowProxyIfWindow(Value v)
{
    if (v.isObject())
        return ObjectValue(*ToWindowProxyIfWindow(&v.toObject()));
    return v;
}

bool
Proxy::get(JSContext* cx, HandleObject proxy, HandleValue receiver_, HandleId id,
           MutableHandleValue vp)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    vp.setUndefined(); // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();

    // Use the WindowProxy as receiver if receiver_ is a Window. Proxy handlers
    // shouldn't have to know about the Window/WindowProxy distinction.
    RootedValue receiver(cx, ValueToWindowProxyIfWindow(receiver_));

    if (handler->hasPrototype()) {
        bool own;
        if (!handler->hasOwn(cx, proxy, id, &own))
            return false;
        if (!own) {
            RootedObject proto(cx);
            if (!GetPrototype(cx, proxy, &proto))
                return false;
            if (!proto)
                return true;
            return GetProperty(cx, proto, receiver, id, vp);
        }
    }

    return handler->get(cx, proxy, receiver, id, vp);
}

bool
Proxy::set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v, HandleValue receiver_,
           ObjectOpResult& result)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
    if (!policy.allowed()) {
        if (!policy.returnValue())
            return false;
        return result.succeed();
    }

    // Use the WindowProxy as receiver if receiver_ is a Window. Proxy handlers
    // shouldn't have to know about the Window/WindowProxy distinction.
    RootedValue receiver(cx, ValueToWindowProxyIfWindow(receiver_));

    // Special case. See the comment on BaseProxyHandler::mHasPrototype.
    if (handler->hasPrototype())
        return handler->BaseProxyHandler::set(cx, proxy, id, v, receiver, result);

    return handler->set(cx, proxy, id, v, receiver, result);
}

bool
Proxy::getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy, AutoIdVector& props)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE, BaseProxyHandler::ENUMERATE, true);
    if (!policy.allowed())
        return policy.returnValue();
    return handler->getOwnEnumerablePropertyKeys(cx, proxy, props);
}

bool
Proxy::enumerate(JSContext* cx, HandleObject proxy, MutableHandleObject objp)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    objp.set(nullptr); // default result if we refuse to perform this action

    if (handler->hasPrototype()) {
        AutoIdVector props(cx);
        if (!Proxy::getOwnEnumerablePropertyKeys(cx, proxy, props))
            return false;

        RootedObject proto(cx);
        if (!GetPrototype(cx, proxy, &proto))
            return false;
        if (!proto)
            return EnumeratedIdVectorToIterator(cx, proxy, 0, props, objp);
        assertSameCompartment(cx, proxy, proto);

        AutoIdVector protoProps(cx);
        return GetPropertyKeys(cx, proto, 0, &protoProps) &&
               AppendUnique(cx, props, protoProps) &&
               EnumeratedIdVectorToIterator(cx, proxy, 0, props, objp);
    }

    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE,
                           BaseProxyHandler::ENUMERATE, true);

    // If the policy denies access but wants us to return true, we need
    // to hand a valid (empty) iterator object to the caller.
    if (!policy.allowed()) {
        return policy.returnValue() &&
            NewEmptyPropertyIterator(cx, 0, objp);
    }
    return handler->enumerate(cx, proxy, objp);
}

bool
Proxy::call(JSContext* cx, HandleObject proxy, const CallArgs& args)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();

    // Because vp[0] is JS_CALLEE on the way in and JS_RVAL on the way out, we
    // can only set our default value once we're sure that we're not calling the
    // trap.
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE,
                           BaseProxyHandler::CALL, true);
    if (!policy.allowed()) {
        args.rval().setUndefined();
        return policy.returnValue();
    }

    return handler->call(cx, proxy, args);
}

bool
Proxy::construct(JSContext* cx, HandleObject proxy, const CallArgs& args)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();

    // Because vp[0] is JS_CALLEE on the way in and JS_RVAL on the way out, we
    // can only set our default value once we're sure that we're not calling the
    // trap.
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE,
                           BaseProxyHandler::CALL, true);
    if (!policy.allowed()) {
        args.rval().setUndefined();
        return policy.returnValue();
    }

    return handler->construct(cx, proxy, args);
}

bool
Proxy::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl, const CallArgs& args)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, &args.thisv().toObject());
    // Note - we don't enter a policy here because our security architecture
    // guards against nativeCall by overriding the trap itself in the right
    // circumstances.
    return proxy->as<ProxyObject>().handler()->nativeCall(cx, test, impl, args);
}

bool
Proxy::hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v, bool* bp)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    *bp = false; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return proxy->as<ProxyObject>().handler()->hasInstance(cx, proxy, v, bp);
}

bool
Proxy::getBuiltinClass(JSContext* cx, HandleObject proxy, ESClassValue* classValue)
{
    JS_CHECK_RECURSION(cx, return false);
    return proxy->as<ProxyObject>().handler()->getBuiltinClass(cx, proxy, classValue);
}

bool
Proxy::isArray(JSContext* cx, HandleObject proxy, JS::IsArrayAnswer* answer)
{
    return proxy->as<ProxyObject>().handler()->isArray(cx, proxy, answer);
}

const char*
Proxy::className(JSContext* cx, HandleObject proxy)
{
    // Check for unbounded recursion, but don't signal an error; className
    // needs to be infallible.
    int stackDummy;
    if (!JS_CHECK_STACK_SIZE(GetNativeStackLimit(cx), &stackDummy))
        return "too much recursion";

    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE,
                           BaseProxyHandler::GET, /* mayThrow = */ false);
    // Do the safe thing if the policy rejects.
    if (!policy.allowed()) {
        return handler->BaseProxyHandler::className(cx, proxy);
    }
    return handler->className(cx, proxy);
}

JSString*
Proxy::fun_toString(JSContext* cx, HandleObject proxy, unsigned indent)
{
    JS_CHECK_RECURSION(cx, return nullptr);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE,
                           BaseProxyHandler::GET, /* mayThrow = */ false);
    // Do the safe thing if the policy rejects.
    if (!policy.allowed())
        return handler->BaseProxyHandler::fun_toString(cx, proxy, indent);
    return handler->fun_toString(cx, proxy, indent);
}

bool
Proxy::regexp_toShared(JSContext* cx, HandleObject proxy, RegExpGuard* g)
{
    JS_CHECK_RECURSION(cx, return false);
    return proxy->as<ProxyObject>().handler()->regexp_toShared(cx, proxy, g);
}

bool
Proxy::boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp)
{
    JS_CHECK_RECURSION(cx, return false);
    return proxy->as<ProxyObject>().handler()->boxedValue_unbox(cx, proxy, vp);
}

JSObject * const TaggedProto::LazyProto = reinterpret_cast<JSObject*>(0x1);

/* static */ bool
Proxy::watch(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, JS::HandleObject callable)
{
    JS_CHECK_RECURSION(cx, return false);
    return proxy->as<ProxyObject>().handler()->watch(cx, proxy, id, callable);
}

/* static */ bool
Proxy::unwatch(JSContext* cx, JS::HandleObject proxy, JS::HandleId id)
{
    JS_CHECK_RECURSION(cx, return false);
    return proxy->as<ProxyObject>().handler()->unwatch(cx, proxy, id);
}

/* static */ bool
Proxy::getElements(JSContext* cx, HandleObject proxy, uint32_t begin, uint32_t end,
                   ElementAdder* adder)
{
    JS_CHECK_RECURSION(cx, return false);
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE, BaseProxyHandler::GET,
                           /* mayThrow = */ true);
    if (!policy.allowed()) {
        if (policy.returnValue()) {
            MOZ_ASSERT(!cx->isExceptionPending());
            return js::GetElementsWithAdder(cx, proxy, proxy, begin, end, adder);
        }
        return false;
    }
    return handler->getElements(cx, proxy, begin, end, adder);
}

/* static */ void
Proxy::trace(JSTracer* trc, JSObject* proxy)
{
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    handler->trace(trc, proxy);
}

bool
js::proxy_LookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                         MutableHandleObject objp, MutableHandleShape propp)
{
    bool found;
    if (!Proxy::has(cx, obj, id, &found))
        return false;

    if (found) {
        MarkNonNativePropertyFound<CanGC>(propp);
        objp.set(obj);
    } else {
        objp.set(nullptr);
        propp.set(nullptr);
    }
    return true;
}

bool
js::proxy_DefineProperty(JSContext* cx, HandleObject obj, HandleId id,
                         Handle<JSPropertyDescriptor> desc,
                         ObjectOpResult& result)
{
    return Proxy::defineProperty(cx, obj, id, desc, result);
}

bool
js::proxy_HasProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* foundp)
{
    return Proxy::has(cx, obj, id, foundp);
}

bool
js::proxy_GetProperty(JSContext* cx, HandleObject obj, HandleValue receiver, HandleId id,
                      MutableHandleValue vp)
{
    return Proxy::get(cx, obj, receiver, id, vp);
}

bool
js::proxy_SetProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                      HandleValue receiver, ObjectOpResult& result)
{
    return Proxy::set(cx, obj, id, v, receiver, result);
}

bool
js::proxy_GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                   MutableHandle<JSPropertyDescriptor> desc)
{
    return Proxy::getOwnPropertyDescriptor(cx, obj, id, desc);
}

bool
js::proxy_DeleteProperty(JSContext* cx, HandleObject obj, HandleId id, ObjectOpResult& result)
{
    if (!Proxy::delete_(cx, obj, id, result))
        return false;
    return SuppressDeletedProperty(cx, obj, id); // XXX is this necessary?
}

void
js::proxy_Trace(JSTracer* trc, JSObject* obj)
{
    MOZ_ASSERT(obj->is<ProxyObject>());
    ProxyObject::trace(trc, obj);
}

/* static */ void
ProxyObject::trace(JSTracer* trc, JSObject* obj)
{
    ProxyObject* proxy = &obj->as<ProxyObject>();

    TraceEdge(trc, &proxy->shape, "ProxyObject_shape");

#ifdef DEBUG
    if (trc->runtime()->gc.isStrictProxyCheckingEnabled() && proxy->is<WrapperObject>()) {
        JSObject* referent = MaybeForwarded(proxy->target());
        if (referent->compartment() != proxy->compartment()) {
            /*
             * Assert that this proxy is tracked in the wrapper map. We maintain
             * the invariant that the wrapped object is the key in the wrapper map.
             */
            Value key = ObjectValue(*referent);
            WrapperMap::Ptr p = proxy->compartment()->lookupWrapper(key);
            MOZ_ASSERT(p);
            MOZ_ASSERT(*p->value().unsafeGet() == ObjectValue(*proxy));
        }
    }
#endif

    // Note: If you add new slots here, make sure to change
    // nuke() to cope.
    TraceCrossCompartmentEdge(trc, obj, proxy->slotOfPrivate(), "private");
    TraceEdge(trc, proxy->slotOfExtra(0), "extra0");

    /*
     * The GC can use the second reserved slot to link the cross compartment
     * wrappers into a linked list, in which case we don't want to trace it.
     */
    if (!proxy->is<CrossCompartmentWrapperObject>())
        TraceEdge(trc, proxy->slotOfExtra(1), "extra1");

    Proxy::trace(trc, obj);
}

JSObject*
js::proxy_WeakmapKeyDelegate(JSObject* obj)
{
    MOZ_ASSERT(obj->is<ProxyObject>());
    return obj->as<ProxyObject>().handler()->weakmapKeyDelegate(obj);
}

void
js::proxy_Finalize(FreeOp* fop, JSObject* obj)
{
    // Suppress a bogus warning about finalize().
    JS::AutoSuppressGCAnalysis nogc;

    MOZ_ASSERT(obj->is<ProxyObject>());
    obj->as<ProxyObject>().handler()->finalize(fop, obj);
    js_free(GetProxyDataLayout(obj)->values);
}

void
js::proxy_ObjectMoved(JSObject* obj, const JSObject* old)
{
    MOZ_ASSERT(obj->is<ProxyObject>());
    obj->as<ProxyObject>().handler()->objectMoved(obj, old);
}

bool
js::proxy_HasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v, bool* bp)
{
    bool b;
    if (!Proxy::hasInstance(cx, proxy, v, &b))
        return false;
    *bp = !!b;
    return true;
}

bool
js::proxy_Call(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject proxy(cx, &args.callee());
    MOZ_ASSERT(proxy->is<ProxyObject>());
    return Proxy::call(cx, proxy, args);
}

bool
js::proxy_Construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject proxy(cx, &args.callee());
    MOZ_ASSERT(proxy->is<ProxyObject>());
    return Proxy::construct(cx, proxy, args);
}

bool
js::proxy_Watch(JSContext* cx, HandleObject obj, HandleId id, HandleObject callable)
{
    return Proxy::watch(cx, obj, id, callable);
}

bool
js::proxy_Unwatch(JSContext* cx, HandleObject obj, HandleId id)
{
    return Proxy::unwatch(cx, obj, id);
}

bool
js::proxy_GetElements(JSContext* cx, HandleObject proxy, uint32_t begin, uint32_t end,
                      ElementAdder* adder)
{
    return Proxy::getElements(cx, proxy, begin, end, adder);
}

JSString*
js::proxy_FunToString(JSContext* cx, HandleObject proxy, unsigned indent)
{
    return Proxy::fun_toString(cx, proxy, indent);
}

const Class js::ProxyObject::proxyClass =
    PROXY_CLASS_DEF("Proxy", JSCLASS_HAS_CACHED_PROTO(JSProto_Proxy));

const Class* const js::ProxyClassPtr = &js::ProxyObject::proxyClass;

JS_FRIEND_API(JSObject*)
js::NewProxyObject(JSContext* cx, const BaseProxyHandler* handler, HandleValue priv, JSObject* proto_,
                   const ProxyOptions& options)
{
    if (options.lazyProto()) {
        MOZ_ASSERT(!proto_);
        proto_ = TaggedProto::LazyProto;
    }

    return ProxyObject::New(cx, handler, priv, TaggedProto(proto_), options);
}

void
ProxyObject::renew(JSContext* cx, const BaseProxyHandler* handler, Value priv)
{
    MOZ_ASSERT_IF(IsCrossCompartmentWrapper(this), IsDeadProxyObject(this));
    MOZ_ASSERT(getClass() == &ProxyObject::proxyClass);
    MOZ_ASSERT(!IsWindowProxy(this));
    MOZ_ASSERT(hasLazyPrototype());

    setHandler(handler);
    setCrossCompartmentPrivate(priv);
    setExtra(0, UndefinedValue());
    setExtra(1, UndefinedValue());
}

JS_FRIEND_API(JSObject*)
js::InitProxyClass(JSContext* cx, HandleObject obj)
{
    static const JSFunctionSpec static_methods[] = {
        JS_FN("create",         proxy_create,          2, 0),
        JS_FN("createFunction", proxy_createFunction,  3, 0),
        JS_FN("revocable",      proxy_revocable,       2, 0),
        JS_FS_END
    };

    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    RootedFunction ctor(cx);
    ctor = global->createConstructor(cx, proxy, cx->names().Proxy, 2);
    if (!ctor)
        return nullptr;

    if (!JS_DefineFunctions(cx, ctor, static_methods))
        return nullptr;
    if (!JS_DefineProperty(cx, obj, "Proxy", ctor, JSPROP_RESOLVING, JS_STUBGETTER, JS_STUBSETTER))
        return nullptr;

    global->setConstructor(JSProto_Proxy, ObjectValue(*ctor));
    return ctor;
}
