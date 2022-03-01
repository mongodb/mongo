/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Functions and structures related to locale-sensitive behavior, including
 * exposure of the default locale (used by operations like toLocaleString).
 */

#ifndef js_LocaleSensitive_h
#define js_LocaleSensitive_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::Handle, JS::MutableHandle
#include "js/Utility.h"     // JS::UniqueChars
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;
struct JS_PUBLIC_API JSRuntime;
class JS_PUBLIC_API JSString;

/**
 * Set the default locale for the ECMAScript Internationalization API
 * (Intl.Collator, Intl.NumberFormat, Intl.DateTimeFormat, and others that will
 * arise as time passes).  (Note that the Internationalization API encourages
 * clients to specify their own locales; this default locale is only used when
 * no locale is specified, e.g. calling a toLocaleString function without
 * passing a locale argument to it.)
 *
 * The locale string remains owned by the caller.
 */
extern JS_PUBLIC_API bool JS_SetDefaultLocale(JSRuntime* rt,
                                              const char* locale);

/**
 * Return a copy of the default locale for the ECMAScript Internationalization
 * API (and for various ECMAScript functions that will invoke it).  The locale
 * is retrieved from the |JSRuntime| that corresponds to |cx|.
 *
 * XXX Bug 1483961 means it's difficult to interpret the meaning of a null
 *     return value for the time being, and we should fix this!
 */
extern JS_PUBLIC_API JS::UniqueChars JS_GetDefaultLocale(JSContext* cx);

/** Reset the default locale to OS defaults. */
extern JS_PUBLIC_API void JS_ResetDefaultLocale(JSRuntime* rt);

using JSLocaleToUpperCase = bool (*)(JSContext* cx, JS::Handle<JSString*> src,
                                     JS::MutableHandle<JS::Value> rval);

using JSLocaleToLowerCase = bool (*)(JSContext* cx, JS::Handle<JSString*> src,
                                     JS::MutableHandle<JS::Value> rval);

using JSLocaleCompare = bool (*)(JSContext* cx, JS::Handle<JSString*> src1,
                                 JS::Handle<JSString*> src2,
                                 JS::MutableHandle<JS::Value> rval);

using JSLocaleToUnicode = bool (*)(JSContext* cx, const char* src,
                                   JS::MutableHandle<JS::Value> rval);

/**
 * A suite of locale-specific string conversion and error message callbacks
 * used to implement locale-sensitive behaviors (such as those performed by
 * the various toLocaleString and toLocale{Date,Time}String functions).
 *
 * If SpiderMonkey is compiled --with-intl-api, then #if JS_HAS_INTL_API.  In
 * this case, SpiderMonkey itself will implement ECMA-402-compliant behavior by
 * calling on ICU, and none of the fields in this struct will ever be used.
 * (You'll still be able to call the get/set-callbacks functions; they just
 * won't affect JavaScript semantics.)
 */
struct JSLocaleCallbacks {
  JSLocaleToUpperCase localeToUpperCase;
  JSLocaleToLowerCase localeToLowerCase;
  JSLocaleCompare localeCompare;
  JSLocaleToUnicode localeToUnicode;
};

/**
 * Set locale callbacks to be used in builds not compiled --with-intl-api.
 * |callbacks| must persist as long as the |JSRuntime|.  Pass |nullptr| to
 * restore default behavior.
 */
extern JS_PUBLIC_API void JS_SetLocaleCallbacks(
    JSRuntime* rt, const JSLocaleCallbacks* callbacks);

/**
 * Return the current locale callbacks, which may be nullptr.
 */
extern JS_PUBLIC_API const JSLocaleCallbacks* JS_GetLocaleCallbacks(
    JSRuntime* rt);

#endif /* js_LocaleSensitive_h */
