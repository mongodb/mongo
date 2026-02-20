/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_CommonFunctions_h
#define builtin_intl_CommonFunctions_h

#include <stddef.h>
#include <stdint.h>

#include "js/RootingAPI.h"
#include "js/Utility.h"

namespace mozilla::intl {
enum class ICUError : uint8_t;
}

namespace js {

class PropertyName;

namespace intl {

/**
 * Initialize a new Intl.* object using the named self-hosted function.
 */
extern bool InitializeObject(JSContext* cx, JS::Handle<JSObject*> obj,
                             JS::Handle<PropertyName*> initializer,
                             JS::Handle<JS::Value> locales,
                             JS::Handle<JS::Value> options);

enum class DateTimeFormatOptions {
  Standard,
  EnableMozExtensions,
};

/**
 * Initialize an existing object as an Intl.DateTimeFormat object.
 */
extern bool InitializeDateTimeFormatObject(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<JS::Value> thisValue,
    JS::Handle<JS::Value> locales, JS::Handle<JS::Value> options,
    JS::Handle<JSString*> required, JS::Handle<JSString*> defaults,
    JS::Handle<JS::Value> toLocaleStringTimeZone,
    DateTimeFormatOptions dtfOptions, JS::MutableHandle<JS::Value> result);

/**
 * Initialize an existing object as an Intl.NumberFormat object.
 */
extern bool InitializeNumberFormatObject(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<JS::Value> thisValue,
                                         JS::Handle<JS::Value> locales,
                                         JS::Handle<JS::Value> options,
                                         JS::MutableHandle<JS::Value> result);

/**
 * Returns the object holding the internal properties for obj.
 */
extern JSObject* GetInternalsObject(JSContext* cx, JS::Handle<JSObject*> obj);

/** Report an Intl internal error not directly tied to a spec step. */
extern void ReportInternalError(JSContext* cx);

/** Report an Intl internal error not directly tied to a spec step. */
extern void ReportInternalError(JSContext* cx, mozilla::intl::ICUError error);

/**
 * The last-ditch locale is used if none of the available locales satisfies a
 * request. "en-GB" is used based on the assumptions that English is the most
 * common second language, that both en-GB and en-US are normally available in
 * an implementation, and that en-GB is more representative of the English used
 * in other locales.
 */
static inline const char* LastDitchLocale() { return "en-GB"; }

/**
 * Certain old, commonly-used language tags that lack a script, are expected to
 * nonetheless imply one. This object maps these old-style tags to modern
 * equivalents.
 */
struct OldStyleLanguageTagMapping {
  const char* const oldStyle;
  const char* const modernStyle;

  // Provide a constructor to catch missing initializers in the mappings array.
  constexpr OldStyleLanguageTagMapping(const char* oldStyle,
                                       const char* modernStyle)
      : oldStyle(oldStyle), modernStyle(modernStyle) {}
};

extern const OldStyleLanguageTagMapping oldStyleLanguageTagMappings[5];

extern JS::UniqueChars EncodeLocale(JSContext* cx, JSString* locale);

// The inline capacity we use for a Vector<char16_t>.  Use this to ensure that
// our uses of ICU string functions, below and elsewhere, will try to fill the
// buffer's entire inline capacity before growing it and heap-allocating.
constexpr size_t INITIAL_CHAR_BUFFER_SIZE = 32;

void AddICUCellMemory(JSObject* obj, size_t nbytes);

void RemoveICUCellMemory(JSObject* obj, size_t nbytes);

void RemoveICUCellMemory(JS::GCContext* gcx, JSObject* obj, size_t nbytes);

JSLinearString* CanonicalizeTimeZone(JSContext* cx,
                                     JS::Handle<JSString*> timeZone);
}  // namespace intl

}  // namespace js

#endif /* builtin_intl_CommonFunctions_h */
