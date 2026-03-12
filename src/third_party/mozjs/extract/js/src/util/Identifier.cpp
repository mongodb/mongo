/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "util/Identifier.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stddef.h>  // size_t

#include "js/GCAPI.h"      // JS::AutoCheckCannotGC
#include "js/TypeDecls.h"  // JS::Latin1Char
#include "util/Unicode.h"  // unicode::{IsIdentifierStart, IsIdentifierPart, IsIdentifierStartASCII, IsIdentifierPartASCII, IsLeadSurrogate, IsTrailSurrogate, UTF16Decode}
#include "vm/StringType.h"  // JSLinearString

using namespace js;

bool js::IsIdentifier(const JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  MOZ_ASSERT(str);
  if (str->hasLatin1Chars()) {
    return IsIdentifier(str->latin1Chars(nogc), str->length());
  }
  return IsIdentifier(str->twoByteChars(nogc), str->length());
}

bool js::IsIdentifierNameOrPrivateName(const JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  MOZ_ASSERT(str);
  if (str->hasLatin1Chars()) {
    return IsIdentifierNameOrPrivateName(str->latin1Chars(nogc), str->length());
  }
  return IsIdentifierNameOrPrivateName(str->twoByteChars(nogc), str->length());
}

bool js::IsIdentifier(const JS::Latin1Char* chars, size_t length) {
  if (length == 0) {
    return false;
  }

  if (!unicode::IsIdentifierStart(char16_t(*chars))) {
    return false;
  }

  const JS::Latin1Char* end = chars + length;
  while (++chars != end) {
    if (!unicode::IsIdentifierPart(char16_t(*chars))) {
      return false;
    }
  }

  return true;
}

bool js::IsIdentifierASCII(char c) {
  return unicode::IsIdentifierStartASCII(c);
}

bool js::IsIdentifierASCII(char c1, char c2) {
  return unicode::IsIdentifierStartASCII(c1) &&
         unicode::IsIdentifierPartASCII(c2);
}

bool js::IsIdentifierNameOrPrivateName(const JS::Latin1Char* chars,
                                       size_t length) {
  if (length == 0) {
    return false;
  }

  // Skip over any private name marker.
  if (*chars == '#') {
    ++chars;
    --length;
  }

  return IsIdentifier(chars, length);
}

static char32_t GetSingleCodePoint(const char16_t** p, const char16_t* end) {
  using namespace js;

  if (MOZ_UNLIKELY(unicode::IsLeadSurrogate(**p)) && *p + 1 < end) {
    char16_t lead = **p;
    char16_t maybeTrail = *(*p + 1);
    if (unicode::IsTrailSurrogate(maybeTrail)) {
      *p += 2;
      return unicode::UTF16Decode(lead, maybeTrail);
    }
  }

  char32_t codePoint = **p;
  (*p)++;
  return codePoint;
}

bool js::IsIdentifier(const char16_t* chars, size_t length) {
  if (length == 0) {
    return false;
  }

  const char16_t* p = chars;
  const char16_t* end = chars + length;
  char32_t codePoint;

  codePoint = GetSingleCodePoint(&p, end);
  if (!unicode::IsIdentifierStart(codePoint)) {
    return false;
  }

  while (p < end) {
    codePoint = GetSingleCodePoint(&p, end);
    if (!unicode::IsIdentifierPart(codePoint)) {
      return false;
    }
  }

  return true;
}

bool js::IsIdentifierNameOrPrivateName(const char16_t* chars, size_t length) {
  if (length == 0) {
    return false;
  }

  const char16_t* p = chars;
  const char16_t* end = chars + length;
  char32_t codePoint;

  codePoint = GetSingleCodePoint(&p, end);

  // Skip over any private name marker.
  if (codePoint == '#') {
    // The identifier part of a private name mustn't be empty.
    if (length == 1) {
      return false;
    }

    codePoint = GetSingleCodePoint(&p, end);
  }

  if (!unicode::IsIdentifierStart(codePoint)) {
    return false;
  }

  while (p < end) {
    codePoint = GetSingleCodePoint(&p, end);
    if (!unicode::IsIdentifierPart(codePoint)) {
      return false;
    }
  }

  return true;
}
