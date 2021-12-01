/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Proxy.h"

#include "mozilla/Attributes.h"

#include <string.h>

#include "jsapi.h"

#include "js/Wrapper.h"
#include "proxy/DeadObjectProxy.h"
#include "proxy/ScriptedProxyHandler.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/WrapperObject.h"

#include "gc/Marking-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::gc;

void
js::AutoEnterPolicy::reportErrorIfExceptionIsNotPending(JSContext* cx, jsid id)
{
    if (JS_IsExceptionPending(cx))
        return;

    if (JSID_IS_VOID(id)) {
        ReportAccessDenied(cx);
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
        prev = cx->enteredPolicy;
        cx->enteredPolicy = this;
    }
}

void
js::AutoEnterPolicy::recordLeave()
{
    if (enteredProxy) {
        MOZ_ASSERT(context->enteredPolicy == this);
        context->enteredPolicy = prev;
    }
}

JS_FRIEND_API(void)
js::assertEnteredPolicy(JSContext* cx, JSObject* proxy, jsid id,
                        BaseProxyHandler::Action act)
{
    MOZ_ASSERT(proxy->is<ProxyObject>());
    MOZ_ASSERT(cx->enteredPolicy);
    MOZ_ASSERT(cx->enteredPolicy->enteredProxy->get() == proxy);
    MOZ_ASSERT(cx->enteredPolicy->enteredId->get() == id);
    MOZ_ASSERT(cx->enteredPolicy->enteredAction & act);
}
#endif

bool
Proxy::getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                             MutableHandle<PropertyDescriptor> desc)
{
    if (!CheckRecursionLimit(cx))
        return false;

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
    if (!CheckRecursionLimit(cx))
        return false;
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
    if (!CheckRecursionLimit(cx))
        return false;
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
    if (!CheckRecursionLimit(cx))
        return false;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE, BaseProxyHandler::ENUMERATE, true);
    if (!policy.allowed())
        return policy.returnValue();
    return proxy->as<ProxyObject>().handler()->ownPropertyKeys(cx, proxy, props);
}

bool
Proxy::delete_(JSContext* cx, HandleObject proxy, HandleId id, ObjectOpResult& result)
{
    if (!CheckRecursionLimit(cx))
        return false;
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
        if (unique) {
            if (!uniqueOthers.append(others[i]))
                return false;
        }
    }
    return base.appendAll(uniqueOthers);
}

/* static */ bool
Proxy::getPrototype(JSContext* cx, HandleObject proxy, MutableHandleObject proto)
{
    MOZ_ASSERT(proxy->hasDynamicPrototype());
    if (!CheckRecursionLimit(cx))
        return false;
    return proxy->as<ProxyObject>().handler()->getPrototype(cx, proxy, proto);
}

/* static */ bool
Proxy::setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto, ObjectOpResult& result)
{
    MOZ_ASSERT(proxy->hasDynamicPrototype());
    if (!CheckRecursionLimit(cx))
        return false;
    return proxy->as<ProxyObject>().handler()->setPrototype(cx, proxy, proto, result);
}

/* static */ bool
Proxy::getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy, bool* isOrdinary,
                              MutableHandleObject proto)
{
    if (!CheckRecursionLimit(cx))
        return false;
    return proxy->as<ProxyObject>().handler()->getPrototypeIfOrdinary(cx, proxy, isOrdinary,
                                                                      proto);
}

/* static */ bool
Proxy::setImmutablePrototype(JSContext* cx, HandleObject proxy, bool* succeeded)
{
    if (!CheckRecursionLimit(cx))
        return false;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    return handler->setImmutablePrototype(cx, proxy, succeeded);
}

/* static */ bool
Proxy::preventExtensions(JSContext* cx, HandleObject proxy, ObjectOpResult& result)
{
    if (!CheckRecursionLimit(cx))
        return false;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    return handler->preventExtensions(cx, proxy, result);
}

/* static */ bool
Proxy::isExtensible(JSContext* cx, HandleObject proxy, bool* extensible)
{
    if (!CheckRecursionLimit(cx))
        return false;
    return proxy->as<ProxyObject>().handler()->isExtensible(cx, proxy, extensible);
}

bool
Proxy::has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp)
{
    if (!CheckRecursionLimit(cx))
        return false;
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
js::ProxyHas(JSContext* cx, HandleObject proxy, HandleValue idVal, MutableHandleValue result)
{
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, idVal, &id))
        return false;

    bool has;
    if (!Proxy::has(cx, proxy, id, &has))
        return false;

    result.setBoolean(has);
    return true;
}

bool
Proxy::hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp)
{
    if (!CheckRecursionLimit(cx))
        return false;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    *bp = false; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return handler->hasOwn(cx, proxy, id, bp);
}

bool
js::ProxyHasOwn(JSContext* cx, HandleObject proxy, HandleValue idVal, MutableHandleValue result)
{
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, idVal, &id))
        return false;

    bool hasOwn;
    if (!Proxy::hasOwn(cx, proxy, id, &hasOwn))
        return false;

    result.setBoolean(hasOwn);
    return true;
}

static MOZ_ALWAYS_INLINE Value
ValueToWindowProxyIfWindow(const Value& v, JSObject* proxy)
{
    if (v.isObject() && v != ObjectValue(*proxy))
        return ObjectValue(*ToWindowProxyIfWindow(&v.toObject()));
    return v;
}

MOZ_ALWAYS_INLINE bool
Proxy::getInternal(JSContext* cx, HandleObject proxy, HandleValue receiver,
                   HandleId id, MutableHandleValue vp)
{
    MOZ_ASSERT_IF(receiver.isObject(), !IsWindow(&receiver.toObject()));

    if (!CheckRecursionLimit(cx))
        return false;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    vp.setUndefined(); // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();

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
Proxy::get(JSContext* cx, HandleObject proxy, HandleValue receiver_, HandleId id,
           MutableHandleValue vp)
{
    // Use the WindowProxy as receiver if receiver_ is a Window. Proxy handlers
    // shouldn't have to know about the Window/WindowProxy distinction.
    RootedValue receiver(cx, ValueToWindowProxyIfWindow(receiver_, proxy));
    return getInternal(cx, proxy, receiver, id, vp);
}

bool
js::ProxyGetProperty(JSContext* cx, HandleObject proxy, HandleId id, MutableHandleValue vp)
{
    RootedValue receiver(cx, ObjectValue(*proxy));
    return Proxy::getInternal(cx, proxy, receiver, id, vp);
}

bool
js::ProxyGetPropertyByValue(JSContext* cx, HandleObject proxy, HandleValue idVal,
                            MutableHandleValue vp)
{
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, idVal, &id))
        return false;

    RootedValue receiver(cx, ObjectValue(*proxy));
    return Proxy::getInternal(cx, proxy, receiver, id, vp);
}

MOZ_ALWAYS_INLINE bool
Proxy::setInternal(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                   HandleValue receiver, ObjectOpResult& result)
{
    MOZ_ASSERT_IF(receiver.isObject(), !IsWindow(&receiver.toObject()));

    if (!CheckRecursionLimit(cx))
        return false;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
    if (!policy.allowed()) {
        if (!policy.returnValue())
            return false;
        return result.succeed();
    }

    // Special case. See the comment on BaseProxyHandler::mHasPrototype.
    if (handler->hasPrototype())
        return handler->BaseProxyHandler::set(cx, proxy, id, v, receiver, result);

    return handler->set(cx, proxy, id, v, receiver, result);
}

bool
Proxy::set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v, HandleValue receiver_,
           ObjectOpResult& result)
{
    // Use the WindowProxy as receiver if receiver_ is a Window. Proxy handlers
    // shouldn't have to know about the Window/WindowProxy distinction.
    RootedValue receiver(cx, ValueToWindowProxyIfWindow(receiver_, proxy));
    return setInternal(cx, proxy, id, v, receiver, result);
}

bool
js::ProxySetProperty(JSContext* cx, HandleObject proxy, HandleId id, HandleValue val, bool strict)
{
    ObjectOpResult result;
    RootedValue receiver(cx, ObjectValue(*proxy));
    if (!Proxy::setInternal(cx, proxy, id, val, receiver, result))
        return false;
    return result.checkStrictErrorOrWarning(cx, proxy, id, strict);
}

bool
js::ProxySetPropertyByValue(JSContext* cx, HandleObject proxy, HandleValue idVal, HandleValue val,
                            bool strict)
{
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, idVal, &id))
        return false;

    ObjectOpResult result;
    RootedValue receiver(cx, ObjectValue(*proxy));
    if (!Proxy::setInternal(cx, proxy, id, val, receiver, result))
        return false;
    return result.checkStrictErrorOrWarning(cx, proxy, id, strict);
}

bool
Proxy::getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy, AutoIdVector& props)
{
    if (!CheckRecursionLimit(cx))
        return false;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE, BaseProxyHandler::ENUMERATE, true);
    if (!policy.allowed())
        return policy.returnValue();
    return handler->getOwnEnumerablePropertyKeys(cx, proxy, props);
}

JSObject*
Proxy::enumerate(JSContext* cx, HandleObject proxy)
{
    if (!CheckRecursionLimit(cx))
        return nullptr;

    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    if (handler->hasPrototype()) {
        AutoIdVector props(cx);
        if (!Proxy::getOwnEnumerablePropertyKeys(cx, proxy, props))
            return nullptr;

        RootedObject proto(cx);
        if (!GetPrototype(cx, proxy, &proto))
            return nullptr;
        if (!proto)
            return EnumeratedIdVectorToIterator(cx, proxy, props);
        assertSameCompartment(cx, proxy, proto);

        AutoIdVector protoProps(cx);
        if (!GetPropertyKeys(cx, proto, 0, &protoProps))
            return nullptr;
        if (!AppendUnique(cx, props, protoProps))
            return nullptr;
        return EnumeratedIdVectorToIterator(cx, proxy, props);
    }

    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE,
                           BaseProxyHandler::ENUMERATE, true);

    // If the policy denies access but wants us to return true, we need
    // to hand a valid (empty) iterator object to the caller.
    if (!policy.allowed()) {
        if (!policy.returnValue())
            return nullptr;
        return NewEmptyPropertyIterator(cx);
    }
    return handler->enumerate(cx, proxy);
}

bool
Proxy::call(JSContext* cx, HandleObject proxy, const CallArgs& args)
{
    if (!CheckRecursionLimit(cx))
        return false;
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
    if (!CheckRecursionLimit(cx))
        return false;
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
    if (!CheckRecursionLimit(cx))
        return false;
    RootedObject proxy(cx, &args.thisv().toObject());
    // Note - we don't enter a policy here because our security architecture
    // guards against nativeCall by overriding the trap itself in the right
    // circumstances.
    return proxy->as<ProxyObject>().handler()->nativeCall(cx, test, impl, args);
}

bool
Proxy::hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v, bool* bp)
{
    if (!CheckRecursionLimit(cx))
        return false;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    *bp = false; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return proxy->as<ProxyObject>().handler()->hasInstance(cx, proxy, v, bp);
}

bool
Proxy::getBuiltinClass(JSContext* cx, HandleObject proxy, ESClass* cls)
{
    if (!CheckRecursionLimit(cx))
        return false;
    return proxy->as<ProxyObject>().handler()->getBuiltinClass(cx, proxy, cls);
}

bool
Proxy::isArray(JSContext* cx, HandleObject proxy, JS::IsArrayAnswer* answer)
{
    if (!CheckRecursionLimit(cx))
        return false;
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
Proxy::fun_toString(JSContext* cx, HandleObject proxy, bool isToSource)
{
    if (!CheckRecursionLimit(cx))
        return nullptr;
    const BaseProxyHandler* handler = proxy->as<ProxyObject>().handler();
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOIDHANDLE,
                           BaseProxyHandler::GET, /* mayThrow = */ false);
    // Do the safe thing if the policy rejects.
    if (!policy.allowed())
        return handler->BaseProxyHandler::fun_toString(cx, proxy, isToSource);
    return handler->fun_toString(cx, proxy, isToSource);
}

RegExpShared*
Proxy::regexp_toShared(JSContext* cx, HandleObject proxy)
{
    if (!CheckRecursionLimit(cx))
        return nullptr;
    return proxy->as<ProxyObject>().handler()->regexp_toShared(cx, proxy);
}

bool
Proxy::boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp)
{
    if (!CheckRecursionLimit(cx))
        return false;
    return proxy->as<ProxyObject>().handler()->boxedValue_unbox(cx, proxy, vp);
}

JSObject * const TaggedProto::LazyProto = reinterpret_cast<JSObject*>(0x1);

/* static */ bool
Proxy::getElements(JSContext* cx, HandleObject proxy, uint32_t begin, uint32_t end,
                   ElementAdder* adder)
{
    if (!CheckRecursionLimit(cx))
        return false;
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

static bool
proxy_LookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                     MutableHandleObject objp, MutableHandle<JS::PropertyResult> propp)
{
    bool found;
    if (!Proxy::has(cx, obj, id, &found))
        return false;

    if (found) {
        propp.setNonNativeProperty();
        objp.set(obj);
    } else {
        propp.setNotFound();
        objp.set(nullptr);
    }
    return true;
}

static bool
proxy_DeleteProperty(JSContext* cx, HandleObject obj, HandleId id, ObjectOpResult& result)
{
    if (!Proxy::delete_(cx, obj, id, result))
        return false;
    return SuppressDeletedProperty(cx, obj, id); // XXX is this necessary?
}

/* static */ void
ProxyObject::traceEdgeToTarget(JSTracer* trc, ProxyObject* obj)
{
    TraceCrossCompartmentEdge(trc, obj, obj->slotOfPrivate(), "proxy target");
}

/* static */ void
ProxyObject::trace(JSTracer* trc, JSObject* obj)
{
    ProxyObject* proxy = &obj->as<ProxyObject>();

    TraceEdge(trc, proxy->shapePtr(), "ProxyObject_shape");

#ifdef DEBUG
    if (TlsContext.get()->isStrictProxyCheckingEnabled() && proxy->is<WrapperObject>()) {
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

    traceEdgeToTarget(trc, proxy);

    size_t nreserved = proxy->numReservedSlots();
    for (size_t i = 0; i < nreserved; i++) {
        /*
         * The GC can use the second reserved slot to link the cross compartment
         * wrappers into a linked list, in which case we don't want to trace it.
         */
        if (proxy->is<CrossCompartmentWrapperObject>() &&
            i == CrossCompartmentWrapperObject::GrayLinkReservedSlot)
        {
            continue;
        }
        TraceEdge(trc, proxy->reservedSlotPtr(i), "proxy_reserved");
    }

    Proxy::trace(trc, obj);
}

JSObject*
js::proxy_WeakmapKeyDelegate(JSObject* obj)
{
    MOZ_ASSERT(obj->is<ProxyObject>());
    return obj->as<ProxyObject>().handler()->weakmapKeyDelegate(obj);
}

static void
proxy_Finalize(FreeOp* fop, JSObject* obj)
{
    // Suppress a bogus warning about finalize().
    JS::AutoSuppressGCAnalysis nogc;

    MOZ_ASSERT(obj->is<ProxyObject>());
    obj->as<ProxyObject>().handler()->finalize(fop, obj);

    if (!obj->as<ProxyObject>().usingInlineValueArray())
        js_free(js::detail::GetProxyDataLayout(obj)->values());
}

size_t
js::proxy_ObjectMoved(JSObject* obj, JSObject* old)
{
    ProxyObject& proxy = obj->as<ProxyObject>();

    if (IsInsideNursery(old)) {
        // Objects in the nursery are never swapped so the proxy must have an
        // inline ProxyValueArray.
        MOZ_ASSERT(old->as<ProxyObject>().usingInlineValueArray());
        proxy.setInlineValueArray();
    }

    return proxy.handler()->objectMoved(obj, old);
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

const ClassOps js::ProxyClassOps = {
    nullptr,                 /* addProperty */
    nullptr,                 /* delProperty */
    nullptr,                 /* enumerate   */
    nullptr,                 /* newEnumerate */
    nullptr,                 /* resolve     */
    nullptr,                 /* mayResolve  */
    proxy_Finalize,          /* finalize    */
    nullptr,                 /* call        */
    Proxy::hasInstance,      /* hasInstance */
    nullptr,                 /* construct   */
    ProxyObject::trace,      /* trace       */
};

const ClassExtension js::ProxyClassExtension = {
    proxy_WeakmapKeyDelegate,
    proxy_ObjectMoved
};

const ObjectOps js::ProxyObjectOps = {
    proxy_LookupProperty,
    Proxy::defineProperty,
    Proxy::has,
    Proxy::get,
    Proxy::set,
    Proxy::getOwnPropertyDescriptor,
    proxy_DeleteProperty,
    Proxy::getElements,
    Proxy::fun_toString
};

const Class js::ProxyObject::proxyClass =
    PROXY_CLASS_DEF("Proxy",
                    JSCLASS_HAS_CACHED_PROTO(JSProto_Proxy) |
                    JSCLASS_HAS_RESERVED_SLOTS(2));

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
ProxyObject::renew(const BaseProxyHandler* handler, const Value& priv)
{
    MOZ_ASSERT(!IsInsideNursery(this));
    MOZ_ASSERT_IF(IsCrossCompartmentWrapper(this), IsDeadProxyObject(this));
    MOZ_ASSERT(getClass() == &ProxyObject::proxyClass);
    MOZ_ASSERT(!IsWindowProxy(this));
    MOZ_ASSERT(hasDynamicPrototype());

    setHandler(handler);
    setCrossCompartmentPrivate(priv);
    for (size_t i = 0; i < numReservedSlots(); i++)
        setReservedSlot(i, UndefinedValue());
}

JS_FRIEND_API(JSObject*)
js::InitProxyClass(JSContext* cx, HandleObject obj)
{
    static const JSFunctionSpec static_methods[] = {
        JS_FN("revocable",      proxy_revocable,       2, 0),
        JS_FS_END
    };

    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    RootedFunction ctor(cx);
    ctor = GlobalObject::createConstructor(cx, proxy, cx->names().Proxy, 2);
    if (!ctor)
        return nullptr;

    if (!JS_DefineFunctions(cx, ctor, static_methods))
        return nullptr;
    if (!JS_DefineProperty(cx, obj, "Proxy", ctor, JSPROP_RESOLVING))
        return nullptr;

    global->setConstructor(JSProto_Proxy, ObjectValue(*ctor));
    return ctor;
}
