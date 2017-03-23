/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsexn.h"
#include "jswrapper.h"

#include "vm/ErrorObject.h"
#include "vm/WrapperObject.h"

#include "jsobjinlines.h"

using namespace js;

JSObject*
Wrapper::New(JSContext* cx, JSObject* obj, const Wrapper* handler,
             const WrapperOptions& options)
{
    RootedValue priv(cx, ObjectValue(*obj));
    return NewProxyObject(cx, handler, priv, options.proto(), options);
}

JSObject*
Wrapper::Renew(JSContext* cx, JSObject* existing, JSObject* obj, const Wrapper* handler)
{
    existing->as<ProxyObject>().renew(cx, handler, ObjectValue(*obj));
    return existing;
}

const Wrapper*
Wrapper::wrapperHandler(JSObject* wrapper)
{
    MOZ_ASSERT(wrapper->is<WrapperObject>());
    return static_cast<const Wrapper*>(wrapper->as<ProxyObject>().handler());
}

JSObject*
Wrapper::wrappedObject(JSObject* wrapper)
{
    MOZ_ASSERT(wrapper->is<WrapperObject>());
    return wrapper->as<ProxyObject>().target();
}

bool
Wrapper::isConstructor(JSObject* obj) const
{
    // For now, all wrappers are constructable if they are callable. We will want to eventually
    // decouple this behavior, but none of the Wrapper infrastructure is currently prepared for
    // that.
    return isCallable(obj);
}

JS_FRIEND_API(JSObject*)
js::UncheckedUnwrap(JSObject* wrapped, bool stopAtWindowProxy, unsigned* flagsp)
{
    unsigned flags = 0;
    while (true) {
        if (!wrapped->is<WrapperObject>() ||
            MOZ_UNLIKELY(stopAtWindowProxy && IsWindowProxy(wrapped)))
        {
            break;
        }
        flags |= Wrapper::wrapperHandler(wrapped)->flags();
        wrapped = wrapped->as<ProxyObject>().private_().toObjectOrNull();

        // This can be called from DirectProxyHandler::weakmapKeyDelegate() on a
        // wrapper whose referent has been moved while it is still unmarked.
        if (wrapped)
            wrapped = MaybeForwarded(wrapped);
    }
    if (flagsp)
        *flagsp = flags;
    return wrapped;
}

JS_FRIEND_API(JSObject*)
js::CheckedUnwrap(JSObject* obj, bool stopAtWindowProxy)
{
    while (true) {
        JSObject* wrapper = obj;
        obj = UnwrapOneChecked(obj, stopAtWindowProxy);
        if (!obj || obj == wrapper)
            return obj;
    }
}

JS_FRIEND_API(JSObject*)
js::UnwrapOneChecked(JSObject* obj, bool stopAtWindowProxy)
{
    if (!obj->is<WrapperObject>() ||
        MOZ_UNLIKELY(IsWindowProxy(obj) && stopAtWindowProxy))
    {
        return obj;
    }

    const Wrapper* handler = Wrapper::wrapperHandler(obj);
    return handler->hasSecurityPolicy() ? nullptr : Wrapper::wrappedObject(obj);
}

const char Wrapper::family = 0;
const Wrapper Wrapper::singleton((unsigned)0);
const Wrapper Wrapper::singletonWithPrototype((unsigned)0, true);
JSObject* Wrapper::defaultProto = TaggedProto::LazyProto;

/* Compartments. */

extern JSObject*
js::TransparentObjectWrapper(JSContext* cx, HandleObject existing, HandleObject obj)
{
    // Allow wrapping outer window proxies.
    MOZ_ASSERT(!obj->is<WrapperObject>() || IsWindowProxy(obj));
    return Wrapper::New(cx, obj, &CrossCompartmentWrapper::singleton);
}

ErrorCopier::~ErrorCopier()
{
    JSContext* cx = ac->context()->asJSContext();
    if (ac->origin() != cx->compartment() && cx->isExceptionPending()) {
        RootedValue exc(cx);
        if (cx->getPendingException(&exc) && exc.isObject() && exc.toObject().is<ErrorObject>()) {
            cx->clearPendingException();
            ac.reset();
            Rooted<ErrorObject*> errObj(cx, &exc.toObject().as<ErrorObject>());
            JSObject* copyobj = CopyErrorObject(cx, errObj);
            if (copyobj)
                cx->setPendingException(ObjectValue(*copyobj));
        }
    }
}

bool Wrapper::finalizeInBackground(Value priv) const
{
    if (!priv.isObject())
        return true;

    /*
     * Make the 'background-finalized-ness' of the wrapper the same as the
     * wrapped object, to allow transplanting between them.
     *
     * If the wrapped object is in the nursery then we know it doesn't have a
     * finalizer, and so background finalization is ok.
     */
    if (IsInsideNursery(&priv.toObject()))
        return true;
    return IsBackgroundFinalized(priv.toObject().asTenured().getAllocKind());
}
