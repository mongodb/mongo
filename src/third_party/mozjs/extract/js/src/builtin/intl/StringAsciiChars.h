/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_StringAsciiChars_h
#define builtin_intl_StringAsciiChars_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"

#include <stddef.h>

#include "js/GCAPI.h"
#include "js/TypeDecls.h"
#include "js/Vector.h"

#include "vm/StringType.h"

namespace js::intl {

/**
 * String view of an ASCII-only string.
 *
 * This holds a reference to a JSLinearString and can produce a string view
 * into that string. If the string is represented by Latin1 characters, the
 * span is returned directly. If the string is represented by UTF-16
 * characters, it copies the char16_t characters into a char array, and then
 * returns a span based on the copy.
 *
 * This allows us to avoid copying for the common use case that the ASCII
 * characters are represented in Latin1.
 */
class MOZ_STACK_CLASS StringAsciiChars final {
  // When copying string characters, use this many bytes of inline storage.
  static const size_t InlineCapacity = 24;

  JS::AutoCheckCannotGC nogc_;

  const JSLinearString* str_;

  mozilla::Maybe<Vector<Latin1Char, InlineCapacity>> ownChars_;

 public:
  explicit StringAsciiChars(const JSLinearString* str) : str_(str) {
    MOZ_ASSERT(StringIsAscii(str));
  }

  operator mozilla::Span<const char>() const {
    if (str_->hasLatin1Chars()) {
      return mozilla::AsChars(str_->latin1Range(nogc_));
    }
    return mozilla::AsChars(mozilla::Span<const Latin1Char>(*ownChars_));
  }

  [[nodiscard]] bool init(JSContext* cx) {
    if (str_->hasLatin1Chars()) {
      return true;
    }

    ownChars_.emplace(cx);
    if (!ownChars_->resize(str_->length())) {
      return false;
    }

    js::CopyChars(ownChars_->begin(), *str_);

    return true;
  }
};

}  // namespace js::intl

#endif  // builtin_intl_StringAsciiChars_h
