/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_Locale_h
#define builtin_intl_Locale_h

#include <stdint.h>

#include "js/Class.h"
#include "vm/NativeObject.h"

namespace js {

class LocaleObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t LANGUAGE_TAG_SLOT = 0;
  static constexpr uint32_t BASENAME_SLOT = 1;
  static constexpr uint32_t UNICODE_EXTENSION_SLOT = 2;
  static constexpr uint32_t SLOT_COUNT = 3;

  /**
   * Returns the complete language tag, including any extensions and privateuse
   * subtags.
   */
  JSString* languageTag() const {
    return getFixedSlot(LANGUAGE_TAG_SLOT).toString();
  }

  /**
   * Returns the basename subtags, i.e. excluding any extensions and privateuse
   * subtags.
   */
  JSString* baseName() const { return getFixedSlot(BASENAME_SLOT).toString(); }

  const Value& unicodeExtension() const {
    return getFixedSlot(UNICODE_EXTENSION_SLOT);
  }

 private:
  static const ClassSpec classSpec_;
};

[[nodiscard]] extern bool intl_ValidateAndCanonicalizeLanguageTag(JSContext* cx,
                                                                  unsigned argc,
                                                                  Value* vp);

[[nodiscard]] extern bool intl_TryValidateAndCanonicalizeLanguageTag(
    JSContext* cx, unsigned argc, Value* vp);

[[nodiscard]] extern bool intl_ValidateAndCanonicalizeUnicodeExtensionType(
    JSContext* cx, unsigned argc, Value* vp);

}  // namespace js

#endif /* builtin_intl_Locale_h */
