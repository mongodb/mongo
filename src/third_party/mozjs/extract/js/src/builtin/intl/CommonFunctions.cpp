/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Operations used to implement multiple Intl.* classes. */

#include "builtin/intl/CommonFunctions.h"

#include "mozilla/Assertions.h"

#include "jsfriendapi.h" // for GetErrorMessage, JSMSG_INTERNAL_INTL_ERROR

#include "js/Value.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/SelfHosting.h"
#include "vm/Stack.h"

#include "vm/JSObject-inl.h"

bool
js::intl::InitializeObject(JSContext* cx, JS::Handle<JSObject*> obj,
                           JS::Handle<PropertyName*> initializer, JS::Handle<JS::Value> locales,
                           JS::Handle<JS::Value> options)
{
    FixedInvokeArgs<3> args(cx);

    args[0].setObject(*obj);
    args[1].set(locales);
    args[2].set(options);

    RootedValue ignored(cx);
    if (!CallSelfHostedFunction(cx, initializer, JS::NullHandleValue, args, &ignored))
        return false;

    MOZ_ASSERT(ignored.isUndefined(),
               "Unexpected return value from non-legacy Intl object initializer");
    return true;
}

bool
js::intl::LegacyInitializeObject(JSContext* cx, JS::Handle<JSObject*> obj,
                                 JS::Handle<PropertyName*> initializer,
                                 JS::Handle<JS::Value> thisValue, JS::Handle<JS::Value> locales,
                                 JS::Handle<JS::Value> options, DateTimeFormatOptions dtfOptions,
                                 JS::MutableHandle<JS::Value> result)
{
    FixedInvokeArgs<5> args(cx);

    args[0].setObject(*obj);
    args[1].set(thisValue);
    args[2].set(locales);
    args[3].set(options);
    args[4].setBoolean(dtfOptions == DateTimeFormatOptions::EnableMozExtensions);

    if (!CallSelfHostedFunction(cx, initializer, NullHandleValue, args, result))
        return false;

    MOZ_ASSERT(result.isObject(), "Legacy Intl object initializer must return an object");
    return true;
}

JSObject*
js::intl::GetInternalsObject(JSContext* cx, JS::Handle<JSObject*> obj)
{
    FixedInvokeArgs<1> args(cx);

    args[0].setObject(*obj);

    RootedValue v(cx);
    if (!js::CallSelfHostedFunction(cx, cx->names().getInternals, NullHandleValue, args, &v))
        return nullptr;

    return &v.toObject();
}

void
js::intl::ReportInternalError(JSContext* cx)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INTERNAL_INTL_ERROR);
}

bool
js::intl::GetAvailableLocales(JSContext* cx, CountAvailable countAvailable,
                              GetAvailable getAvailable, JS::MutableHandle<JS::Value> result)
{
    RootedObject locales(cx, NewObjectWithGivenProto<PlainObject>(cx, nullptr));
    if (!locales)
        return false;

#if ENABLE_INTL_API
    RootedAtom a(cx);
    uint32_t count = countAvailable();
    for (uint32_t i = 0; i < count; i++) {
        const char* locale = getAvailable(i);
        auto lang = DuplicateString(cx, locale);
        if (!lang)
            return false;
        char* p;
        while ((p = strchr(lang.get(), '_')))
            *p = '-';
        a = Atomize(cx, lang.get(), strlen(lang.get()));
        if (!a)
            return false;
        if (!DefineDataProperty(cx, locales, a->asPropertyName(), TrueHandleValue))
            return false;
    }
#endif

    result.setObject(*locales);
    return true;
}
