/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_CommonFunctions_h
#define builtin_intl_CommonFunctions_h

#include "mozilla/Assertions.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

#include "js/RootingAPI.h"
#include "js/Vector.h"
#include "unicode/utypes.h"
#include "vm/StringType.h"

struct UFormattedValue;

namespace mozilla::intl {
enum class ICUError : uint8_t;
}

namespace js {

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
 * Initialize an existing object as an Intl.* object using the named
 * self-hosted function.  This is only for a few old Intl.* constructors, for
 * legacy reasons -- new ones should use the function above instead.
 */
extern bool LegacyInitializeObject(JSContext* cx, JS::Handle<JSObject*> obj,
                                   JS::Handle<PropertyName*> initializer,
                                   JS::Handle<JS::Value> thisValue,
                                   JS::Handle<JS::Value> locales,
                                   JS::Handle<JS::Value> options,
                                   DateTimeFormatOptions dtfOptions,
                                   JS::MutableHandle<JS::Value> result);

/**
 * Returns the object holding the internal properties for obj.
 */
extern JSObject* GetInternalsObject(JSContext* cx, JS::Handle<JSObject*> obj);

/** Report an Intl internal error not directly tied to a spec step. */
extern void ReportInternalError(JSContext* cx);

/** Report an Intl internal error not directly tied to a spec step. */
extern void ReportInternalError(JSContext* cx, mozilla::intl::ICUError error);

static inline bool StringsAreEqual(const char* s1, const char* s2) {
  return !strcmp(s1, s2);
}

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

static inline const char* IcuLocale(const char* locale) {
  if (StringsAreEqual(locale, "und")) {
    return "";  // ICU root locale
  }

  return locale;
}

extern UniqueChars EncodeLocale(JSContext* cx, JSString* locale);

// Starting with ICU 59, UChar defaults to char16_t.
static_assert(
    std::is_same_v<UChar, char16_t>,
    "SpiderMonkey doesn't support redefining UChar to a different type");

// The inline capacity we use for a Vector<char16_t>.  Use this to ensure that
// our uses of ICU string functions, below and elsewhere, will try to fill the
// buffer's entire inline capacity before growing it and heap-allocating.
constexpr size_t INITIAL_CHAR_BUFFER_SIZE = 32;

template <typename ICUStringFunction, typename CharT, size_t InlineCapacity>
static int32_t CallICU(JSContext* cx, const ICUStringFunction& strFn,
                       Vector<CharT, InlineCapacity>& chars) {
  MOZ_ASSERT(chars.length() >= InlineCapacity);

  UErrorCode status = U_ZERO_ERROR;
  int32_t size = strFn(chars.begin(), chars.length(), &status);
  if (status == U_BUFFER_OVERFLOW_ERROR) {
    MOZ_ASSERT(size >= 0);

    // Some ICU functions (e.g. uloc_getDisplayName) return one less character
    // than the actual minimum size when U_BUFFER_OVERFLOW_ERROR is raised,
    // resulting in later reporting U_STRING_NOT_TERMINATED_WARNING. So add plus
    // one here and then assert U_STRING_NOT_TERMINATED_WARNING isn't raised.
    size++;

    if (!chars.resize(size_t(size))) {
      return -1;
    }
    status = U_ZERO_ERROR;
    size = strFn(chars.begin(), size, &status);

    MOZ_ASSERT(status != U_STRING_NOT_TERMINATED_WARNING);
  }
  if (U_FAILURE(status)) {
    ReportInternalError(cx);
    return -1;
  }

  MOZ_ASSERT(size >= 0);
  return size;
}

template <typename ICUStringFunction>
static JSString* CallICU(JSContext* cx, const ICUStringFunction& strFn) {
  Vector<char16_t, INITIAL_CHAR_BUFFER_SIZE> chars(cx);
  MOZ_ALWAYS_TRUE(chars.resize(INITIAL_CHAR_BUFFER_SIZE));

  int32_t size = CallICU(cx, strFn, chars);
  if (size < 0) {
    return nullptr;
  }

  return NewStringCopyN<CanGC>(cx, chars.begin(), size_t(size));
}

void AddICUCellMemory(JSObject* obj, size_t nbytes);

void RemoveICUCellMemory(JSFreeOp* fop, JSObject* obj, size_t nbytes);

JSString* FormattedValueToString(JSContext* cx,
                                 const UFormattedValue* formattedValue);

}  // namespace intl

}  // namespace js

#endif /* builtin_intl_CommonFunctions_h */
