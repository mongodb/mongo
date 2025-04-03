/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Shadow definition of |JSString| innards.  Don't use this directly! */

#ifndef js_shadow_String_h
#define js_shadow_String_h

#include <stdint.h>  // uint32_t, uintptr_t

#include "jstypes.h"  // JS_PUBLIC_API, js::Bit, js::BitMask, JS_BITS_PER_WORD

#include "js/TypeDecls.h"  // JS::Latin1Char

class JS_PUBLIC_API JSAtom;
struct JSExternalStringCallbacks;
class JSLinearString;
class JS_PUBLIC_API JSString;

namespace js {
namespace gc {
struct Cell;
}  // namespace gc
}  // namespace js

namespace JS {

namespace shadow {

struct String {
  static constexpr uint32_t ATOM_BIT = js::Bit(3);
  static constexpr uint32_t LINEAR_BIT = js::Bit(4);
  static constexpr uint32_t INLINE_CHARS_BIT = js::Bit(6);
  static constexpr uint32_t LATIN1_CHARS_BIT = js::Bit(9);
  static constexpr uint32_t EXTERNAL_FLAGS = LINEAR_BIT | js::Bit(8);
  static constexpr uint32_t TYPE_FLAGS_MASK = js::BitMask(9) - js::BitMask(3);
  static constexpr uint32_t PERMANENT_ATOM_MASK = ATOM_BIT | js::Bit(8);

  uintptr_t flags_;
#if JS_BITS_PER_WORD == 32
  uint32_t length_;
#endif

  union {
    const JS::Latin1Char* nonInlineCharsLatin1;
    const char16_t* nonInlineCharsTwoByte;
    JS::Latin1Char inlineStorageLatin1[1];
    char16_t inlineStorageTwoByte[1];
  };
  const JSExternalStringCallbacks* externalCallbacks;

  uint32_t flags() const { return static_cast<uint32_t>(flags_); }
  uint32_t length() const {
#if JS_BITS_PER_WORD == 32
    return length_;
#else
    return static_cast<uint32_t>(flags_ >> 32);
#endif
  }

  static bool isPermanentAtom(const js::gc::Cell* cell) {
    uint32_t flags = reinterpret_cast<const String*>(cell)->flags();
    return (flags & PERMANENT_ATOM_MASK) == PERMANENT_ATOM_MASK;
  }

  bool isLinear() const { return flags() & LINEAR_BIT; }
  bool hasLatin1Chars() const { return flags() & LATIN1_CHARS_BIT; }

  // For hot code, prefer other type queries.
  bool isExternal() const {
    return (flags() & TYPE_FLAGS_MASK) == EXTERNAL_FLAGS;
  }

  const JS::Latin1Char* latin1LinearChars() const {
    MOZ_ASSERT(isLinear());
    MOZ_ASSERT(hasLatin1Chars());
    return (flags() & String::INLINE_CHARS_BIT) ? inlineStorageLatin1
                                                : nonInlineCharsLatin1;
  }

  const char16_t* twoByteLinearChars() const {
    MOZ_ASSERT(isLinear());
    MOZ_ASSERT(!hasLatin1Chars());
    return (flags() & String::INLINE_CHARS_BIT) ? inlineStorageTwoByte
                                                : nonInlineCharsTwoByte;
  }
};

inline const String* AsShadowString(const JSString* str) {
  return reinterpret_cast<const String*>(str);
}

inline String* AsShadowString(JSString* str) {
  return reinterpret_cast<String*>(str);
}

inline const String* AsShadowString(const JSLinearString* str) {
  return reinterpret_cast<const String*>(str);
}

inline String* AsShadowString(JSLinearString* str) {
  return reinterpret_cast<String*>(str);
}

inline const String* AsShadowString(const JSAtom* str) {
  return reinterpret_cast<const String*>(str);
}

inline String* AsShadowString(JSAtom* str) {
  return reinterpret_cast<String*>(str);
}

}  // namespace shadow

}  // namespace JS

#endif  // js_shadow_String_h
