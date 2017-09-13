/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "proxy/DeadObjectProxy.h"

#include "jsapi.h"
#include "jsfun.h" // XXXefaust Bug 1064662

#include "vm/ProxyObject.h"

using namespace js;
using namespace js::gc;

static void
ReportDead(JSContext *cx)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
}

bool
DeadObjectProxy::getOwnPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::ownPropertyKeys(JSContext* cx, HandleObject wrapper,
                                 AutoIdVector& props) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::delete_(JSContext* cx, HandleObject wrapper, HandleId id,
                         ObjectOpResult& result) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::enumerate(JSContext* cx, HandleObject wrapper, MutableHandleObject objp) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::getPrototype(JSContext* cx, HandleObject proxy, MutableHandleObject protop) const
{
    protop.set(nullptr);
    return true;
}

bool
DeadObjectProxy::preventExtensions(JSContext* cx, HandleObject proxy, ObjectOpResult& result) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const
{
    // This is kind of meaningless, but dead-object semantics aside,
    // [[Extensible]] always being true is consistent with other proxy types.
    *extensible = true;
    return true;
}

bool
DeadObjectProxy::call(JSContext* cx, HandleObject wrapper, const CallArgs& args) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::construct(JSContext* cx, HandleObject wrapper, const CallArgs& args) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                             bool* bp) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::getBuiltinClass(JSContext* cx, HandleObject proxy,
                                 ESClassValue* classValue) const
{
    ReportDead(cx);
    return false;
}

bool
DeadObjectProxy::isArray(JSContext* cx, HandleObject obj, JS::IsArrayAnswer* answer) const
{
    ReportDead(cx);
    return false;
}

const char*
DeadObjectProxy::className(JSContext* cx, HandleObject wrapper) const
{
    return "DeadObject";
}

JSString*
DeadObjectProxy::fun_toString(JSContext* cx, HandleObject proxy, unsigned indent) const
{
    ReportDead(cx);
    return nullptr;
}

bool
DeadObjectProxy::regexp_toShared(JSContext* cx, HandleObject proxy, RegExpGuard* g) const
{
    ReportDead(cx);
    return false;
}

const char DeadObjectProxy::family = 0;
const DeadObjectProxy DeadObjectProxy::singleton;

bool
js::IsDeadProxyObject(JSObject* obj)
{
    return IsDerivedProxyObject(obj, &DeadObjectProxy::singleton);
}
