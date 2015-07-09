/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsapi.h"
#include "jswrapper.h"

#include "jsatominlines.h"

using namespace js;

template <class Base>
bool
SecurityWrapper<Base>::enter(JSContext* cx, HandleObject wrapper, HandleId id,
                             Wrapper::Action act, bool* bp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    *bp = false;
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                                  CallArgs args) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::setPrototypeOf(JSContext* cx, HandleObject wrapper,
                                      HandleObject proto, bool* bp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::setImmutablePrototype(JSContext* cx, HandleObject wrapper,
                                             bool* succeeded) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::preventExtensions(JSContext* cx, HandleObject wrapper,
                                         bool* succeeded) const
{
    // Just like BaseProxyHandler, SecurityWrappers claim by default to always
    // be extensible, so as not to leak information about the state of the
    // underlying wrapped thing.
    *succeeded = false;
    return true;
}

template <class Base>
bool
SecurityWrapper<Base>::isExtensible(JSContext* cx, HandleObject wrapper, bool* extensible) const
{
    // See above.
    *extensible = true;
    return true;
}

// For security wrappers, we run the OrdinaryToPrimitive algorithm on the wrapper
// itself, which means that the existing security policy on operations like
// toString() will take effect and do the right thing here.
template <class Base>
bool
SecurityWrapper<Base>::defaultValue(JSContext* cx, HandleObject wrapper,
                                    JSType hint, MutableHandleValue vp) const
{
    return OrdinaryToPrimitive(cx, wrapper, hint, vp);
}

template <class Base>
bool
SecurityWrapper<Base>::objectClassIs(HandleObject obj, ESClassValue classValue, JSContext* cx) const
{
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::regexp_toShared(JSContext* cx, HandleObject obj, RegExpGuard* g) const
{
    return Base::regexp_toShared(cx, obj, g);
}

template <class Base>
bool
SecurityWrapper<Base>::boxedValue_unbox(JSContext* cx, HandleObject obj, MutableHandleValue vp) const
{
    vp.setUndefined();
    return true;
}

template <class Base>
bool
SecurityWrapper<Base>::defineProperty(JSContext* cx, HandleObject wrapper,
                                      HandleId id, MutableHandle<PropertyDescriptor> desc) const
{
    if (desc.getter() || desc.setter()) {
        RootedValue idVal(cx, IdToValue(id));
        JSString* str = ValueToSource(cx, idVal);
        if (!str)
            return false;
        AutoStableStringChars chars(cx);
        const char16_t* prop = nullptr;
        if (str->ensureFlat(cx) && chars.initTwoByte(cx, str))
            prop = chars.twoByteChars();
        JS_ReportErrorNumberUC(cx, js_GetErrorMessage, nullptr,
                               JSMSG_ACCESSOR_DEF_DENIED, prop);
        return false;
    }

    return Base::defineProperty(cx, wrapper, id, desc);
}

template <class Base>
bool
SecurityWrapper<Base>::watch(JSContext* cx, HandleObject proxy,
                             HandleId id, HandleObject callable) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::unwatch(JSContext* cx, HandleObject proxy,
                               HandleId id) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}


template class js::SecurityWrapper<Wrapper>;
template class js::SecurityWrapper<CrossCompartmentWrapper>;
