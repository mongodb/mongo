/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/intl/LanguageTag.h"

#include "mozilla/intl/Locale.h"
#include "mozilla/Span.h"

#include "builtin/intl/StringAsciiChars.h"
#include "gc/Tracer.h"
#include "vm/JSContext.h"

namespace js {
namespace intl {

[[nodiscard]] bool ParseLocale(JSContext* cx, Handle<JSLinearString*> str,
                               mozilla::intl::Locale& result) {
  if (StringIsAscii(str)) {
    intl::StringAsciiChars chars(str);
    if (!chars.init(cx)) {
      return false;
    }

    if (mozilla::intl::LocaleParser::TryParse(chars, result).isOk()) {
      return true;
    }
  }

  if (UniqueChars localeChars = QuoteString(cx, str, '"')) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INVALID_LANGUAGE_TAG, localeChars.get());
  }
  return false;
}

bool ParseStandaloneLanguageTag(Handle<JSLinearString*> str,
                                mozilla::intl::LanguageSubtag& result) {
  // Tell the analysis the |IsStructurallyValidLanguageTag| function can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  if (str->hasLatin1Chars()) {
    if (!mozilla::intl::IsStructurallyValidLanguageTag<Latin1Char>(
            str->latin1Range(nogc))) {
      return false;
    }
    result.Set<Latin1Char>(str->latin1Range(nogc));
  } else {
    if (!mozilla::intl::IsStructurallyValidLanguageTag<char16_t>(
            str->twoByteRange(nogc))) {
      return false;
    }
    result.Set<char16_t>(str->twoByteRange(nogc));
  }
  return true;
}

bool ParseStandaloneScriptTag(Handle<JSLinearString*> str,
                              mozilla::intl::ScriptSubtag& result) {
  // Tell the analysis the |IsStructurallyValidScriptTag| function can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  if (str->hasLatin1Chars()) {
    if (!mozilla::intl::IsStructurallyValidScriptTag<Latin1Char>(
            str->latin1Range(nogc))) {
      return false;
    }
    result.Set<Latin1Char>(str->latin1Range(nogc));
  } else {
    if (!mozilla::intl::IsStructurallyValidScriptTag<char16_t>(
            str->twoByteRange(nogc))) {
      return false;
    }
    result.Set<char16_t>(str->twoByteRange(nogc));
  }
  return true;
}

bool ParseStandaloneRegionTag(Handle<JSLinearString*> str,
                              mozilla::intl::RegionSubtag& result) {
  // Tell the analysis the |IsStructurallyValidRegionTag| function can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  if (str->hasLatin1Chars()) {
    if (!mozilla::intl::IsStructurallyValidRegionTag<Latin1Char>(
            str->latin1Range(nogc))) {
      return false;
    }
    result.Set<Latin1Char>(str->latin1Range(nogc));
  } else {
    if (!mozilla::intl::IsStructurallyValidRegionTag<char16_t>(
            str->twoByteRange(nogc))) {
      return false;
    }
    result.Set<char16_t>(str->twoByteRange(nogc));
  }
  return true;
}

template <typename CharT>
static bool IsAsciiLowercaseAlpha(mozilla::Span<const CharT> span) {
  // Tell the analysis the |std::all_of| function can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  const CharT* ptr = span.data();
  size_t length = span.size();
  return std::all_of(ptr, ptr + length, mozilla::IsAsciiLowercaseAlpha<CharT>);
}

static bool IsAsciiLowercaseAlpha(JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return IsAsciiLowercaseAlpha<Latin1Char>(str->latin1Range(nogc));
  }
  return IsAsciiLowercaseAlpha<char16_t>(str->twoByteRange(nogc));
}

template <typename CharT>
static bool IsAsciiAlpha(mozilla::Span<const CharT> span) {
  // Tell the analysis the |std::all_of| function can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  const CharT* ptr = span.data();
  size_t length = span.size();
  return std::all_of(ptr, ptr + length, mozilla::IsAsciiAlpha<CharT>);
}

static bool IsAsciiAlpha(JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return IsAsciiAlpha<Latin1Char>(str->latin1Range(nogc));
  }
  return IsAsciiAlpha<char16_t>(str->twoByteRange(nogc));
}

JS::Result<JSString*> ParseStandaloneISO639LanguageTag(
    JSContext* cx, Handle<JSLinearString*> str) {
  // ISO-639 language codes contain either two or three characters.
  size_t length = str->length();
  if (length != 2 && length != 3) {
    return nullptr;
  }

  // We can directly the return the input below if it's in the correct case.
  bool isLowerCase = IsAsciiLowercaseAlpha(str);
  if (!isLowerCase) {
    // Must be an ASCII alpha string.
    if (!IsAsciiAlpha(str)) {
      return nullptr;
    }
  }

  mozilla::intl::LanguageSubtag languageTag;
  if (str->hasLatin1Chars()) {
    JS::AutoCheckCannotGC nogc;
    languageTag.Set<Latin1Char>(str->latin1Range(nogc));
  } else {
    JS::AutoCheckCannotGC nogc;
    languageTag.Set<char16_t>(str->twoByteRange(nogc));
  }

  if (!isLowerCase) {
    // The language subtag is canonicalized to lower case.
    languageTag.ToLowerCase();
  }

  // Reject the input if the canonical tag contains more than just a single
  // language subtag.
  if (mozilla::intl::Locale::ComplexLanguageMapping(languageTag)) {
    return nullptr;
  }

  // Take care to replace deprecated subtags with their preferred values.
  JSString* result;
  if (mozilla::intl::Locale::LanguageMapping(languageTag) || !isLowerCase) {
    result = NewStringCopy<CanGC>(cx, languageTag.Span());
  } else {
    result = str;
  }
  if (!result) {
    return cx->alreadyReportedOOM();
  }
  return result;
}

void js::intl::UnicodeExtensionKeyword::trace(JSTracer* trc) {
  TraceRoot(trc, &type_, "UnicodeExtensionKeyword::type");
}

}  // namespace intl
}  // namespace js
