/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_StaticStrings_h
#define vm_StaticStrings_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_ALWAYS_INLINE
#include "mozilla/TextUtils.h"  // mozilla::{IsAsciiDigit, IsAsciiLowercaseAlpha, IsAsciiUppercaseAlpha}

#include <stddef.h>     // size_t
#include <stdint.h>     // int32_t, uint32_t
#include <type_traits>  // std::is_same_v

#include "jstypes.h"  // JS_PUBLIC_API, js::Bit, js::BitMask

#include "js/TypeDecls.h"  // JS::Latin1Char

struct JS_PUBLIC_API JSContext;

class JSAtom;
class JSLinearString;
class JSString;

namespace js {

namespace frontend {
class ParserAtomsTable;
class TaggedParserAtomIndex;
class WellKnownParserAtoms;
struct CompilationAtomCache;
}  // namespace frontend

namespace jit {
class MacroAssembler;
}  // namespace jit

class StaticStrings {
  // NOTE: The WellKnownParserAtoms rely on these tables and may need to be
  //       update if these tables are changed.
  friend class js::frontend::ParserAtomsTable;
  friend class js::frontend::TaggedParserAtomIndex;
  friend class js::frontend::WellKnownParserAtoms;
  friend struct js::frontend::CompilationAtomCache;

  friend class js::jit::MacroAssembler;

 private:
  // Strings matches `[A-Za-z0-9$_]{2}` pattern.
  // Store each character in 6 bits.
  // See fromSmallChar/toSmallChar for the mapping.
  static constexpr size_t SMALL_CHAR_BITS = 6;
  static constexpr size_t SMALL_CHAR_MASK = js::BitMask(SMALL_CHAR_BITS);

  // To optimize ASCII -> small char, allocate a table.
  static constexpr size_t SMALL_CHAR_TABLE_SIZE = 128U;
  static constexpr size_t NUM_SMALL_CHARS = js::Bit(SMALL_CHAR_BITS);
  static constexpr size_t NUM_LENGTH2_ENTRIES =
      NUM_SMALL_CHARS * NUM_SMALL_CHARS;

 public:
  /* We keep these public for the JITs. */
  static const size_t UNIT_STATIC_LIMIT = 256U;
  static const size_t INT_STATIC_LIMIT = 256U;

 private:
  JSAtom* length2StaticTable[NUM_LENGTH2_ENTRIES] = {};  // zeroes
  JSAtom* unitStaticTable[UNIT_STATIC_LIMIT] = {};       // zeroes
  JSAtom* intStaticTable[INT_STATIC_LIMIT] = {};         // zeroes

 public:
  StaticStrings() = default;

  bool init(JSContext* cx);

  static bool hasUint(uint32_t u) { return u < INT_STATIC_LIMIT; }

  JSAtom* getUint(uint32_t u) {
    MOZ_ASSERT(hasUint(u));
    return intStaticTable[u];
  }

  static bool hasInt(int32_t i) { return uint32_t(i) < INT_STATIC_LIMIT; }

  JSAtom* getInt(int32_t i) {
    MOZ_ASSERT(hasInt(i));
    return getUint(uint32_t(i));
  }

  static bool hasUnit(char16_t c) { return c < UNIT_STATIC_LIMIT; }

  JSAtom* getUnit(char16_t c) {
    MOZ_ASSERT(hasUnit(c));
    return unitStaticTable[c];
  }

  /* May not return atom, returns null on (reported) failure. */
  inline JSLinearString* getUnitString(JSContext* cx, char16_t c);

  /* May not return atom, returns null on (reported) failure. */
  inline JSLinearString* getUnitStringForElement(JSContext* cx, JSString* str,
                                                 size_t index);

  /* May not return atom, returns null on (reported) failure. */
  inline JSLinearString* getUnitStringForElement(JSContext* cx,
                                                 JSLinearString* str,
                                                 size_t index);

  template <typename CharT>
  static bool isStatic(const CharT* chars, size_t len);

  /* Return null if no static atom exists for the given (chars, length). */
  template <typename CharT>
  MOZ_ALWAYS_INLINE JSAtom* lookup(const CharT* chars, size_t length) {
    static_assert(std::is_same_v<CharT, JS::Latin1Char> ||
                      std::is_same_v<CharT, char16_t>,
                  "for understandability, |chars| must be one of a few "
                  "identified types");

    switch (length) {
      case 1: {
        char16_t c = chars[0];
        if (c < UNIT_STATIC_LIMIT) {
          return getUnit(c);
        }
        return nullptr;
      }
      case 2:
        if (fitsInSmallChar(chars[0]) && fitsInSmallChar(chars[1])) {
          return getLength2(chars[0], chars[1]);
        }
        return nullptr;
      case 3:
        /*
         * Here we know that JSString::intStringTable covers only 256 (or at
         * least not 1000 or more) chars. We rely on order here to resolve the
         * unit vs. int string/length-2 string atom identity issue by giving
         * priority to unit strings for "0" through "9" and length-2 strings for
         * "10" through "99".
         */
        int i;
        if (fitsInLength3Static(chars[0], chars[1], chars[2], &i)) {
          return getInt(i);
        }
        return nullptr;
    }

    return nullptr;
  }

  MOZ_ALWAYS_INLINE JSAtom* lookup(const char* chars, size_t length) {
    // Collapse calls for |const char*| into |const Latin1Char char*| to avoid
    // excess instantiations.
    return lookup(reinterpret_cast<const JS::Latin1Char*>(chars), length);
  }

 private:
  using SmallChar = uint8_t;

  struct SmallCharTable {
    SmallChar storage[SMALL_CHAR_TABLE_SIZE];

    constexpr SmallChar& operator[](size_t idx) { return storage[idx]; }
    constexpr const SmallChar& operator[](size_t idx) const {
      return storage[idx];
    }
  };

  static const SmallChar INVALID_SMALL_CHAR = -1;

  static bool fitsInSmallChar(char16_t c) {
    return c < SMALL_CHAR_TABLE_SIZE &&
           toSmallCharTable[c] != INVALID_SMALL_CHAR;
  }

  template <typename CharT>
  static bool fitsInLength3Static(CharT c1, CharT c2, CharT c3, int* i) {
    static_assert(INT_STATIC_LIMIT <= 299,
                  "static int strings assumed below to be at most "
                  "three digits where the first digit is either 1 or 2");
    if ('1' <= c1 && c1 < '3' && '0' <= c2 && c2 <= '9' && '0' <= c3 &&
        c3 <= '9') {
      *i = (c1 - '0') * 100 + (c2 - '0') * 10 + (c3 - '0');

      if (unsigned(*i) < INT_STATIC_LIMIT) {
        return true;
      }
    }
    return false;
  }

  static constexpr JS::Latin1Char fromSmallChar(SmallChar c);

  static constexpr SmallChar toSmallChar(uint32_t c);

  static constexpr SmallCharTable createSmallCharTable();

  static const SmallCharTable toSmallCharTable;

  static constexpr JS::Latin1Char firstCharOfLength2(size_t s) {
    return fromSmallChar(s >> SMALL_CHAR_BITS);
  }
  static constexpr JS::Latin1Char secondCharOfLength2(size_t s) {
    return fromSmallChar(s & SMALL_CHAR_MASK);
  }

  static constexpr JS::Latin1Char firstCharOfLength3(uint32_t i) {
    return '0' + (i / 100);
  }
  static constexpr JS::Latin1Char secondCharOfLength3(uint32_t i) {
    return '0' + ((i / 10) % 10);
  }
  static constexpr JS::Latin1Char thirdCharOfLength3(uint32_t i) {
    return '0' + (i % 10);
  }

  static MOZ_ALWAYS_INLINE size_t getLength2Index(char16_t c1, char16_t c2) {
    MOZ_ASSERT(fitsInSmallChar(c1));
    MOZ_ASSERT(fitsInSmallChar(c2));
    return (size_t(toSmallCharTable[c1]) << SMALL_CHAR_BITS) +
           toSmallCharTable[c2];
  }

  // Same as getLength2Index, but withtout runtime assertion,
  // this should be used only for known static string.
  static constexpr size_t getLength2IndexStatic(char c1, char c2) {
    return (size_t(toSmallChar(c1)) << SMALL_CHAR_BITS) + toSmallChar(c2);
  }

  MOZ_ALWAYS_INLINE JSAtom* getLength2FromIndex(size_t index) {
    return length2StaticTable[index];
  }

  MOZ_ALWAYS_INLINE JSAtom* getLength2(char16_t c1, char16_t c2) {
    return getLength2FromIndex(getLength2Index(c1, c2));
  }
};

/*
 * Declare length-2 strings. We only store strings where both characters are
 * alphanumeric. The lower 10 short chars are the numerals, the next 26 are
 * the lowercase letters, and the next 26 are the uppercase letters.
 */

constexpr JS::Latin1Char StaticStrings::fromSmallChar(SmallChar c) {
  if (c < 10) {
    return c + '0';
  }
  if (c < 36) {
    return c + 'a' - 10;
  }
  if (c < 62) {
    return c + 'A' - 36;
  }
  if (c == 62) {
    return '$';
  }
  return '_';
}

constexpr StaticStrings::SmallChar StaticStrings::toSmallChar(uint32_t c) {
  if (mozilla::IsAsciiDigit(c)) {
    return c - '0';
  }
  if (mozilla::IsAsciiLowercaseAlpha(c)) {
    return c - 'a' + 10;
  }
  if (mozilla::IsAsciiUppercaseAlpha(c)) {
    return c - 'A' + 36;
  }
  if (c == '$') {
    return 62;
  }
  if (c == '_') {
    return 63;
  }
  return StaticStrings::INVALID_SMALL_CHAR;
}

}  // namespace js

#endif /* vm_StaticStrings_h */
