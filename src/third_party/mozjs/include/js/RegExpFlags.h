/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Regular expression flags. */

#ifndef js_RegExpFlags_h
#define js_RegExpFlags_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_IMPLICIT

#include <ostream>   // ostream
#include <stdint.h>  // uint8_t

namespace JS {

/**
 * Regular expression flag values, suitable for initializing a collection of
 * regular expression flags as defined below in |RegExpFlags|.  Flags are listed
 * in alphabetical order by syntax -- /d, /g, /i, /m, /s, /u, /v, /y.
 */
class RegExpFlag {
  // WARNING TO SPIDERMONKEY HACKERS (embedders must assume these values can
  // change):
  //
  // Flag-bit values appear in XDR and structured clone data formats, so none of
  // these values can be changed (including to assign values in numerically
  // ascending order) unless you also add a translation layer.

 public:
  /**
   * Add .indices property to the match result, i.e. /d
   */
  static constexpr uint8_t HasIndices = 0b0100'0000;

  /**
   * Act globally and find *all* matches (rather than stopping after just the
   * first one), i.e. /g.
   */
  static constexpr uint8_t Global = 0b0000'0010;

  /**
   * Interpret regular expression source text case-insensitively by folding
   * uppercase letters to lowercase, i.e. /i.
   */
  static constexpr uint8_t IgnoreCase = 0b0000'0001;

  /** Treat ^ and $ as begin and end of line, i.e. /m. */
  static constexpr uint8_t Multiline = 0b0000'0100;

  /* Allow . to match newline characters, i.e. /s. */
  static constexpr uint8_t DotAll = 0b0010'0000;

  /** Use Unicode semantics, i.e. /u. */
  static constexpr uint8_t Unicode = 0b0001'0000;

  /** Use Unicode Sets semantics, i.e. /v. */
  static constexpr uint8_t UnicodeSets = 0b1000'0000;

  /** Only match starting from <regular expression>.lastIndex, i.e. /y. */
  static constexpr uint8_t Sticky = 0b0000'1000;

  /** No regular expression flags. */
  static constexpr uint8_t NoFlags = 0b0000'0000;

  /** All regular expression flags. */
  static constexpr uint8_t AllFlags = 0b1111'1111;
};

/**
 * A collection of regular expression flags.  Individual flag values may be
 * combined into a collection using bitwise operators.
 */
class RegExpFlags {
 public:
  using Flag = uint8_t;

 private:
  Flag flags_ = 0;

 public:
  RegExpFlags() = default;

  MOZ_IMPLICIT RegExpFlags(Flag flags) : flags_(flags) {
    MOZ_ASSERT((flags & RegExpFlag::AllFlags) == flags,
               "flags must not contain unrecognized flags");
  }

  RegExpFlags(const RegExpFlags&) = default;
  RegExpFlags& operator=(const RegExpFlags&) = default;

  bool operator==(const RegExpFlags& other) const {
    return flags_ == other.flags_;
  }

  bool operator!=(const RegExpFlags& other) const { return !(*this == other); }

  RegExpFlags& operator&=(const RegExpFlags& rhs) {
    flags_ &= rhs.flags_;
    return *this;
  }

  RegExpFlags& operator|=(const RegExpFlags& rhs) {
    flags_ |= rhs.flags_;
    return *this;
  }

  RegExpFlags operator&(Flag flag) const { return RegExpFlags(flags_ & flag); }

  RegExpFlags operator|(Flag flag) const { return RegExpFlags(flags_ | flag); }

  RegExpFlags operator^(Flag flag) const { return RegExpFlags(flags_ ^ flag); }

  RegExpFlags operator~() const {
    return RegExpFlags(~flags_ & RegExpFlag::AllFlags);
  }

  bool hasIndices() const { return flags_ & RegExpFlag::HasIndices; }
  bool global() const { return flags_ & RegExpFlag::Global; }
  bool ignoreCase() const { return flags_ & RegExpFlag::IgnoreCase; }
  bool multiline() const { return flags_ & RegExpFlag::Multiline; }
  bool dotAll() const { return flags_ & RegExpFlag::DotAll; }
  bool unicode() const { return flags_ & RegExpFlag::Unicode; }
  bool unicodeSets() const { return flags_ & RegExpFlag::UnicodeSets; }
  bool sticky() const { return flags_ & RegExpFlag::Sticky; }

  explicit operator bool() const { return flags_ != 0; }

  Flag value() const { return flags_; }
  constexpr operator Flag() const { return flags_; }

  void set(Flag flags, bool value) {
    if (value) {
      flags_ |= flags;
    } else {
      flags_ &= ~flags;
    }
  }
};

inline RegExpFlags& operator&=(RegExpFlags& flags, RegExpFlags::Flag flag) {
  flags = flags & flag;
  return flags;
}

inline RegExpFlags& operator|=(RegExpFlags& flags, RegExpFlags::Flag flag) {
  flags = flags | flag;
  return flags;
}

inline RegExpFlags& operator^=(RegExpFlags& flags, RegExpFlags::Flag flag) {
  flags = flags ^ flag;
  return flags;
}

inline RegExpFlags operator&(const RegExpFlags& lhs, const RegExpFlags& rhs) {
  RegExpFlags result = lhs;
  result &= rhs;
  return result;
}

inline RegExpFlags operator|(const RegExpFlags& lhs, const RegExpFlags& rhs) {
  RegExpFlags result = lhs;
  result |= rhs;
  return result;
}

inline bool MaybeParseRegExpFlag(char c, RegExpFlags::Flag* flag) {
  switch (c) {
    case 'd':
      *flag = RegExpFlag::HasIndices;
      return true;
    case 'g':
      *flag = RegExpFlag::Global;
      return true;
    case 'i':
      *flag = RegExpFlag::IgnoreCase;
      return true;
    case 'm':
      *flag = RegExpFlag::Multiline;
      return true;
    case 's':
      *flag = RegExpFlag::DotAll;
      return true;
    case 'u':
      *flag = RegExpFlag::Unicode;
      return true;
    case 'v':
      *flag = RegExpFlag::UnicodeSets;
      return true;
    case 'y':
      *flag = RegExpFlag::Sticky;
      return true;
    default:
      return false;
  }
}

std::ostream& operator<<(std::ostream& os, RegExpFlags flags);

}  // namespace JS

#endif  // js_RegExpFlags_h
