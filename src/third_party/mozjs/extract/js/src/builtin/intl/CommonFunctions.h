/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_CommonFunctions_h
#define builtin_intl_CommonFunctions_h

#include "mozilla/Assertions.h"
#include "mozilla/TypeTraits.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "builtin/intl/ICUStubs.h"
#include "js/RootingAPI.h"
#include "js/Vector.h"
#include "vm/StringType.h"

namespace js {

namespace intl {

/**
 * Initialize a new Intl.* object using the named self-hosted function.
 */
extern bool
InitializeObject(JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<PropertyName*> initializer,
                 JS::Handle<JS::Value> locales, JS::Handle<JS::Value> options);

enum class DateTimeFormatOptions
{
    Standard,
    EnableMozExtensions,
};

/**
 * Initialize an existing object as an Intl.* object using the named
 * self-hosted function.  This is only for a few old Intl.* constructors, for
 * legacy reasons -- new ones should use the function above instead.
 */
extern bool
LegacyInitializeObject(JSContext* cx, JS::Handle<JSObject*> obj,
                       JS::Handle<PropertyName*> initializer, JS::Handle<JS::Value> thisValue,
                       JS::Handle<JS::Value> locales, JS::Handle<JS::Value> options,
                       DateTimeFormatOptions dtfOptions, JS::MutableHandle<JS::Value> result);

/**
 * Returns the object holding the internal properties for obj.
 */
extern JSObject*
GetInternalsObject(JSContext* cx, JS::Handle<JSObject*> obj);

/** Report an Intl internal error not directly tied to a spec step. */
extern void
ReportInternalError(JSContext* cx);

static inline bool
StringsAreEqual(const char* s1, const char* s2)
{
    return !strcmp(s1, s2);
}

static inline const char*
IcuLocale(const char* locale)
{
    if (StringsAreEqual(locale, "und"))
        return ""; // ICU root locale

    return locale;
}

// Starting with ICU 59, UChar defaults to char16_t.
static_assert(mozilla::IsSame<UChar, char16_t>::value,
              "SpiderMonkey doesn't support redefining UChar to a different type");

// The inline capacity we use for a Vector<char16_t>.  Use this to ensure that
// our uses of ICU string functions, below and elsewhere, will try to fill the
// buffer's entire inline capacity before growing it and heap-allocating.
constexpr size_t INITIAL_CHAR_BUFFER_SIZE = 32;

template <typename ICUStringFunction, size_t InlineCapacity>
static int32_t
CallICU(JSContext* cx, const ICUStringFunction& strFn, Vector<char16_t, InlineCapacity>& chars)
{
    MOZ_ASSERT(chars.length() == 0);
    MOZ_ALWAYS_TRUE(chars.resize(InlineCapacity));

    UErrorCode status = U_ZERO_ERROR;
    int32_t size = strFn(chars.begin(), InlineCapacity, &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        MOZ_ASSERT(size >= 0);
        if (!chars.resize(size_t(size)))
            return -1;
        status = U_ZERO_ERROR;
        strFn(chars.begin(), size, &status);
    }
    if (U_FAILURE(status)) {
        ReportInternalError(cx);
        return -1;
    }

    MOZ_ASSERT(size >= 0);
    return size;
}

template <typename ICUStringFunction>
static JSString*
CallICU(JSContext* cx, const ICUStringFunction& strFn)
{
    Vector<char16_t, INITIAL_CHAR_BUFFER_SIZE> chars(cx);

    int32_t size = CallICU(cx, strFn, chars);
    if (size < 0)
        return nullptr;

    return NewStringCopyN<CanGC>(cx, chars.begin(), size_t(size));
}

// CountAvailable and GetAvailable describe the signatures used for ICU API
// to determine available locales for various functionality.
using CountAvailable = int32_t (*)();
using GetAvailable = const char* (*)(int32_t localeIndex);

/**
 * Return an object whose own property names are the locales indicated as
 * available by |countAvailable| that provides an overall count, and by
 * |getAvailable| that when called passing a number less than that count,
 * returns the corresponding locale as a borrowed string.  For example:
 *
 *   RootedValue v(cx);
 *   if (!GetAvailableLocales(cx, unum_countAvailable, unum_getAvailable, &v))
 *       return false;
 */
extern bool
GetAvailableLocales(JSContext* cx, CountAvailable countAvailable, GetAvailable getAvailable,
                    JS::MutableHandle<JS::Value> result);

} // namespace intl

} // namespace js

#endif /* builtin_intl_CommonFunctions_h */
