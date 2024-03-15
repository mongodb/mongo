/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Structured representation of Unicode locale IDs used with Intl functions. */

#ifndef builtin_intl_LanguageTag_h
#define builtin_intl_LanguageTag_h

#include "mozilla/intl/Locale.h"
#include "mozilla/Span.h"

#include "js/Result.h"
#include "js/RootingAPI.h"

struct JS_PUBLIC_API JSContext;
class JSLinearString;
class JS_PUBLIC_API JSString;
class JS_PUBLIC_API JSTracer;

namespace js {

namespace intl {

/**
 * Parse a string Unicode BCP 47 locale identifier. If successful, store in
 * |result| and return true. Otherwise return false.
 */
[[nodiscard]] bool ParseLocale(JSContext* cx, JS::Handle<JSLinearString*> str,
                               mozilla::intl::Locale& result);

/**
 * Parse a string as a standalone |language| tag. If |str| is a standalone
 * language tag, store it in |result| and return true. Otherwise return false.
 */
[[nodiscard]] bool ParseStandaloneLanguageTag(
    JS::Handle<JSLinearString*> str, mozilla::intl::LanguageSubtag& result);

/**
 * Parse a string as a standalone |script| tag. If |str| is a standalone script
 * tag, store it in |result| and return true. Otherwise return false.
 */
[[nodiscard]] bool ParseStandaloneScriptTag(
    JS::Handle<JSLinearString*> str, mozilla::intl::ScriptSubtag& result);

/**
 * Parse a string as a standalone |region| tag. If |str| is a standalone region
 * tag, store it in |result| and return true. Otherwise return false.
 */
[[nodiscard]] bool ParseStandaloneRegionTag(
    JS::Handle<JSLinearString*> str, mozilla::intl::RegionSubtag& result);

/**
 * Parse a string as an ISO-639 language code. Return |nullptr| in the result if
 * the input could not be parsed or the canonical form of the resulting language
 * tag contains more than a single language subtag.
 */
JS::Result<JSString*> ParseStandaloneISO639LanguageTag(
    JSContext* cx, JS::Handle<JSLinearString*> str);

class UnicodeExtensionKeyword final {
  char key_[mozilla::intl::LanguageTagLimits::UnicodeKeyLength];
  JSLinearString* type_;

 public:
  using UnicodeKey =
      const char (&)[mozilla::intl::LanguageTagLimits::UnicodeKeyLength + 1];
  using UnicodeKeySpan =
      mozilla::Span<const char,
                    mozilla::intl::LanguageTagLimits::UnicodeKeyLength>;

  UnicodeExtensionKeyword(UnicodeKey key, JSLinearString* type)
      : key_{key[0], key[1]}, type_(type) {}

  UnicodeKeySpan key() const { return {key_, sizeof(key_)}; }
  JSLinearString* type() const { return type_; }

  void trace(JSTracer* trc);
};

[[nodiscard]] extern bool ApplyUnicodeExtensionToTag(
    JSContext* cx, mozilla::intl::Locale& tag,
    JS::HandleVector<UnicodeExtensionKeyword> keywords);

}  // namespace intl

}  // namespace js

#endif /* builtin_intl_LanguageTag_h */
