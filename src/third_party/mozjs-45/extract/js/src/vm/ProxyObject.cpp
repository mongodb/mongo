/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ProxyObject.h"

#include "jscompartment.h"
#include "jsobjinlines.h"

using namespace js;

/* static */ ProxyObject*
ProxyObject::New(JSContext* cx, const BaseProxyHandler* handler, HandleValue priv, TaggedProto proto_,
                 const ProxyOptions& options)
{
    Rooted<TaggedProto> proto(cx, proto_);

    const Class* clasp = options.clasp();

    MOZ_ASSERT(isValidProxyClass(clasp));
    MOZ_ASSERT(clasp->shouldDelayMetadataCallback());
    MOZ_ASSERT_IF(proto.isObject(), cx->compartment() == proto.toObject()->compartment());

    /*
     * Eagerly mark properties unknown for proxies, so we don't try to track
     * their properties and so that we don't need to walk the compartment if
     * their prototype changes later.  But don't do this for DOM proxies,
     * because we want to be able to keep track of them in typesets in useful
     * ways.
     */
    if (proto.isObject() && !options.singleton() && !clasp->isDOMClass()) {
        RootedObject protoObj(cx, proto.toObject());
        if (!JSObject::setNewGroupUnknown(cx, clasp, protoObj))
            return nullptr;
    }

    NewObjectKind newKind = options.singleton() ? SingletonObject : GenericObject;
    gc::AllocKind allocKind = gc::GetGCObjectKind(clasp);

    if (handler->finalizeInBackground(priv))
        allocKind = GetBackgroundAllocKind(allocKind);

    ProxyValueArray* values = cx->zone()->new_<ProxyValueArray>();
    if (!values) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    AutoSetNewObjectMetadata metadata(cx);
    // Note: this will initialize the object's |data| to strange values, but we
    // will immediately overwrite those below.
    RootedObject obj(cx, NewObjectWithGivenTaggedProto(cx, clasp, proto, allocKind,
                                                       newKind));
    if (!obj) {
        js_free(values);
        return nullptr;
    }

    Rooted<ProxyObject*> proxy(cx, &obj->as<ProxyObject>());

    proxy->data.values = values;
    proxy->data.handler = handler;

    proxy->setCrossCompartmentPrivate(priv);

    /* Don't track types of properties of non-DOM and non-singleton proxies. */
    if (newKind != SingletonObject && !clasp->isDOMClass())
        MarkObjectGroupUnknownProperties(cx, proxy->group());

    return proxy;
}

void
ProxyObject::setCrossCompartmentPrivate(const Value& priv)
{
    *slotOfPrivate() = priv;
}

void
ProxyObject::setSameCompartmentPrivate(const Value& priv)
{
    MOZ_ASSERT(IsObjectValueInCompartment(priv, compartment()));
    *slotOfPrivate() = priv;
}

void
ProxyObject::nuke(const BaseProxyHandler* handler)
{
    setSameCompartmentPrivate(NullValue());
    for (size_t i = 0; i < PROXY_EXTRA_SLOTS; i++)
        SetProxyExtra(this, i, NullValue());

    /* Restore the handler as requested after nuking. */
    setHandler(handler);
}

JS_FRIEND_API(void)
js::SetValueInProxy(Value* slot, const Value& value)
{
    // Slots in proxies are not HeapValues, so do a cast whenever assigning
    // values to them which might trigger a barrier.
    *reinterpret_cast<HeapValue*>(slot) = value;
}
