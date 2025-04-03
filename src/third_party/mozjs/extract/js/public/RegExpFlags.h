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

#include <stdint.h>  // uint8_t

namespace JS {

/**
 * Regular expression flag values, suitable for initializing a collection of
 * regular expression flags as defined below in |RegExpFlags|.  Flags are listed
 * in alphabetical order by syntax -- /d, /g, /i, /m, /s, /u, /y.
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
  static constexpr uint8_t HasIndices = 0b100'0000;

  /**
   * Act globally and find *all* matches (rather than stopping after just the
   * first one), i.e. /g.
   */
  static constexpr uint8_t Global = 0b000'0010;

  /**
   * Interpret regular expression source text case-insensitively by folding
   * uppercase letters to lowercase, i.e. /i.
   */
  static constexpr uint8_t IgnoreCase = 0b000'0001;

  /** Treat ^ and $ as begin and end of line, i.e. /m. */
  static constexpr uint8_t Multiline = 0b000'0100;

  /* Allow . to match newline characters, i.e. /s. */
  static constexpr uint8_t DotAll = 0b010'0000;

  /** Use Unicode semantics, i.e. /u. */
  static constexpr uint8_t Unicode = 0b001'0000;

  /** Only match starting from <regular expression>.lastIndex, i.e. /y. */
  static constexpr uint8_t Sticky = 0b000'1000;

  /** No regular expression flags. */
  static constexpr uint8_t NoFlags = 0b000'0000;

  /** All regular expression flags. */
  static constexpr uint8_t AllFlags = 0b111'1111;
};

/**
 * A collection of regular expression flags.  Individual flag values may be
 * combined into a collection using bitwise operators.
 */
class RegExpFlags {
 public:
  using Flag = uint8_t;

 private:
  Flag flags_;

 public:
  RegExpFlags() = default;

  MOZ_IMPLICIT RegExpFlags(Flag flags) : flags_(flags) {
    MOZ_ASSERT((flags & RegExpFlag::AllFlags) == flags,
               "flags must not contain unrecognized flags");
  }

  RegExpFlags(const RegExpFlags&) = default;

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
  bool sticky() const { return flags_ & RegExpFlag::Sticky; }

  explicit operator bool() const { return flags_ != 0; }

  Flag value() const { return flags_; }
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
  return lhs;
}

inline RegExpFlags operator|(const RegExpFlags& lhs, const RegExpFlags& rhs) {
  RegExpFlags result = lhs;
  result |= rhs;
  return result;
}

}  // namespace JS

#endif  // js_RegExpFlags_h
