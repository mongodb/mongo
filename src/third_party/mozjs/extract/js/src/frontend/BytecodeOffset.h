/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeOffset_h
#define frontend_BytecodeOffset_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/CheckedInt.h"  // mozilla::CheckedInt

#include <stddef.h>  // ptrdiff_t

namespace js {
namespace frontend {

class BytecodeOffsetDiff;

// The offset inside bytecode.
class BytecodeOffset {
 private:
  static const ptrdiff_t INVALID_OFFSET = -1;

  ptrdiff_t value_ = 0;

  struct Invalid {};
  explicit constexpr BytecodeOffset(Invalid) : value_(INVALID_OFFSET) {}

 public:
  constexpr BytecodeOffset() = default;

  explicit BytecodeOffset(ptrdiff_t value) : value_(value) {
    MOZ_ASSERT(value >= 0);
  }

  static constexpr BytecodeOffset invalidOffset() {
    return BytecodeOffset(Invalid());
  }

  bool operator==(const BytecodeOffset& rhs) const {
    return value_ == rhs.value_;
  }

  bool operator!=(const BytecodeOffset& rhs) const { return !(*this == rhs); }

  inline BytecodeOffsetDiff operator-(const BytecodeOffset& rhs) const;
  inline BytecodeOffset operator+(const BytecodeOffsetDiff& diff) const;

  inline BytecodeOffset& operator+=(const BytecodeOffsetDiff& diff);
  inline BytecodeOffset& operator-=(const BytecodeOffsetDiff& diff);

  bool operator<(const BytecodeOffset& rhs) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(rhs.valid());
    return value_ < rhs.value_;
  }
  bool operator<=(const BytecodeOffset& rhs) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(rhs.valid());
    return value_ <= rhs.value_;
  }
  bool operator>(const BytecodeOffset& rhs) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(rhs.valid());
    return value_ > rhs.value_;
  }
  bool operator>=(const BytecodeOffset& rhs) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(rhs.valid());
    return value_ >= rhs.value_;
  }

  ptrdiff_t value() const { return value_; }
  uint32_t toUint32() const {
    MOZ_ASSERT(size_t(uint32_t(value_)) == size_t(value_));
    return uint32_t(value_);
  }

  bool valid() const { return value_ != INVALID_OFFSET; }
};

class BytecodeOffsetDiff {
 private:
  friend class BytecodeOffset;

  ptrdiff_t value_ = 0;

 public:
  constexpr BytecodeOffsetDiff() = default;

  explicit constexpr BytecodeOffsetDiff(ptrdiff_t value_) : value_(value_) {}

  bool operator==(const BytecodeOffsetDiff& rhs) const {
    return value_ == rhs.value_;
  }

  bool operator!=(const BytecodeOffsetDiff& rhs) const {
    return !(*this == rhs);
  }

  BytecodeOffsetDiff operator+(const BytecodeOffsetDiff& rhs) const {
    mozilla::CheckedInt<ptrdiff_t> result = value_;
    result += rhs.value_;
    return BytecodeOffsetDiff(result.value());
  }

  ptrdiff_t value() const { return value_; }
  uint32_t toUint32() const {
    MOZ_ASSERT(size_t(uint32_t(value_)) == size_t(value_));
    return uint32_t(value_);
  }
};

inline BytecodeOffsetDiff BytecodeOffset::operator-(
    const BytecodeOffset& rhs) const {
  MOZ_ASSERT(valid());
  MOZ_ASSERT(rhs.valid());
  mozilla::CheckedInt<ptrdiff_t> result = value_;
  result -= rhs.value_;
  return BytecodeOffsetDiff(result.value());
}

inline BytecodeOffset BytecodeOffset::operator+(
    const BytecodeOffsetDiff& diff) const {
  MOZ_ASSERT(valid());
  mozilla::CheckedInt<ptrdiff_t> result = value_;
  result += diff.value_;
  return BytecodeOffset(result.value());
}

inline BytecodeOffset& BytecodeOffset::operator+=(
    const BytecodeOffsetDiff& diff) {
  MOZ_ASSERT(valid());
  mozilla::CheckedInt<ptrdiff_t> result = value_;
  result += diff.value_;
  value_ = result.value();
  return *this;
}

inline BytecodeOffset& BytecodeOffset::operator-=(
    const BytecodeOffsetDiff& diff) {
  MOZ_ASSERT(valid());
  mozilla::CheckedInt<ptrdiff_t> result = value_;
  result -= diff.value_;
  value_ = result.value();
  return *this;
}

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_BytecodeOffset_h */
