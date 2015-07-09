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

bool
DeadObjectProxy::getPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                       MutableHandle<PropertyDescriptor> desc) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::getOwnPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                MutableHandle<PropertyDescriptor> desc) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::ownPropertyKeys(JSContext* cx, HandleObject wrapper,
                                 AutoIdVector& props) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::delete_(JSContext* cx, HandleObject wrapper, HandleId id, bool* bp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::enumerate(JSContext* cx, HandleObject wrapper, MutableHandleObject objp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::getPrototypeOf(JSContext* cx, HandleObject proxy, MutableHandleObject protop) const
{
    protop.set(nullptr);
    return true;
}

bool
DeadObjectProxy::preventExtensions(JSContext* cx, HandleObject proxy, bool* succeeded) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
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
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::construct(JSContext* cx, HandleObject wrapper, const CallArgs& args) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            CallArgs args) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                             bool* bp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::objectClassIs(HandleObject obj, ESClassValue classValue, JSContext* cx) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
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
    return nullptr;
}

bool
DeadObjectProxy::regexp_toShared(JSContext* cx, HandleObject proxy, RegExpGuard* g) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::defaultValue(JSContext* cx, HandleObject obj, JSType hint,
                              MutableHandleValue vp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

const char DeadObjectProxy::family = 0;
const DeadObjectProxy DeadObjectProxy::singleton;


bool
js::IsDeadProxyObject(JSObject* obj)
{
    return obj->is<ProxyObject>() &&
           obj->as<ProxyObject>().handler() == &DeadObjectProxy::singleton;
}
