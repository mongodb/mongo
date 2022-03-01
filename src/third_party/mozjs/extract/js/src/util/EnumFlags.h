/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_EnumFlags_h
#define util_EnumFlags_h

#include "mozilla/Attributes.h"

#include <initializer_list>
#include <type_traits>

namespace js {

// Wrapper type for flags fields based on an enum type.
//
// EnumFlags does not (implicitly) convert to/from the underlying integer type.
// That can be supported for specific flags fields by deriving from this class
// and implementing an extra constructor and/or |operator FieldType|.
//
// Note: this type is similar to mfbt/EnumSet.h, but has the same size in debug
// and release builds so is more appropriate for core data structures where we
// don't want this size difference.
template <typename EnumType>
class EnumFlags {
 protected:
  // Use the enum's underlying type for the flags field. This makes JIT accesses
  // more predictable and simplifies the implementation of this class.
  static_assert(std::is_enum_v<EnumType>);
  using FieldType = std::underlying_type_t<EnumType>;
  static_assert(std::is_unsigned_v<FieldType>);

  FieldType flags_ = 0;

  explicit constexpr EnumFlags(FieldType rawFlags) : flags_(rawFlags) {}

 public:
  constexpr EnumFlags() = default;

  constexpr MOZ_IMPLICIT EnumFlags(std::initializer_list<EnumType> list) {
    for (EnumType flag : list) {
      setFlag(flag);
    }
  }

  constexpr bool hasFlag(EnumType flag) const {
    return flags_ & static_cast<FieldType>(flag);
  }
  constexpr void setFlag(EnumType flag) {
    flags_ |= static_cast<FieldType>(flag);
  }
  constexpr void clearFlag(EnumType flag) {
    flags_ &= ~static_cast<FieldType>(flag);
  }
  constexpr void setFlag(EnumType flag, bool b) {
    if (b) {
      setFlag(flag);
    } else {
      clearFlag(flag);
    }
  }

  constexpr bool isEmpty() const { return flags_ == 0; }

  constexpr FieldType toRaw() const { return flags_; }
  void setRaw(FieldType flag) { flags_ = flag; }

  constexpr bool operator==(const EnumFlags& other) const {
    return flags_ == other.flags_;
  }
  constexpr bool operator!=(const EnumFlags& other) const {
    return flags_ != other.flags_;
  }
};

}  // namespace js

#endif  // util_EnumFlags_h
