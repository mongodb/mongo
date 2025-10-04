/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// [SMDOC] Column numbers
//
// Inside SpiderMonkey, column numbers are represented as 1-origin 32-bit
// unsigned integers. Some parts of the engine use the highest bit of a column
// number as a tag to indicate Wasm frame.
//
// These classes help clarifying the origin of the column number, and also
// figuring out whether the column number uses the wasm's tag or not, and also
// help converting between them.
//
// Also these classes support converting from 0-origin column number.
//
// In a 0-origin context, column 0 is the first character of the line.
// In a 1-origin context, column 1 is the first character of the line,
// for example:
//
//           function foo() { ... }
//           ^              ^
// 0-origin: 0              15
// 1-origin: 1              16

#ifndef js_ColumnNumber_h
#define js_ColumnNumber_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_IMPLICIT

#include <limits>    // std::numeric_limits
#include <stdint.h>  // uint32_t

namespace JS {

// Wasm function index.
//
// This class is used as parameter or return type of
// TaggedColumnNumberOneOrigin class below.
struct WasmFunctionIndex {
  // TaggedColumnNumberOneOrigin uses the highest bit as a tag.
  static constexpr uint32_t Limit = std::numeric_limits<int32_t>::max() / 2;

  // For wasm frames, the function index is returned as the column with the
  // high bit set. In paths that format error stacks into strings, this
  // information can be used to synthesize a proper wasm frame. But when raw
  // column numbers are handed out, we just fix them to the first column to
  // avoid confusion.
  static constexpr uint32_t DefaultBinarySourceColumnNumberOneOrigin = 1;

 private:
  uint32_t value_ = 0;

 public:
  constexpr WasmFunctionIndex() = default;
  constexpr WasmFunctionIndex(const WasmFunctionIndex& other) = default;

  inline explicit WasmFunctionIndex(uint32_t value) : value_(value) {
    MOZ_ASSERT(valid());
  }

  uint32_t value() const { return value_; }

  bool valid() const { return value_ <= Limit; }
};

// The offset between 2 column numbers.
struct ColumnNumberOffset {
 private:
  int32_t value_ = 0;

 public:
  constexpr ColumnNumberOffset() = default;
  constexpr ColumnNumberOffset(const ColumnNumberOffset& other) = default;

  inline explicit ColumnNumberOffset(int32_t value) : value_(value) {}

  static constexpr ColumnNumberOffset zero() { return ColumnNumberOffset(); }

  bool operator==(const ColumnNumberOffset& rhs) const {
    return value_ == rhs.value_;
  }

  bool operator!=(const ColumnNumberOffset& rhs) const {
    return !(*this == rhs);
  }

  int32_t value() const { return value_; }
};

// The positive offset from certain column number.
struct ColumnNumberUnsignedOffset {
 private:
  uint32_t value_ = 0;

 public:
  constexpr ColumnNumberUnsignedOffset() = default;
  constexpr ColumnNumberUnsignedOffset(
      const ColumnNumberUnsignedOffset& other) = default;

  inline explicit ColumnNumberUnsignedOffset(uint32_t value) : value_(value) {}

  static constexpr ColumnNumberUnsignedOffset zero() {
    return ColumnNumberUnsignedOffset();
  }

  ColumnNumberUnsignedOffset operator+(
      const ColumnNumberUnsignedOffset& offset) const {
    return ColumnNumberUnsignedOffset(value_ + offset.value());
  }

  ColumnNumberUnsignedOffset& operator+=(
      const ColumnNumberUnsignedOffset& offset) {
    value_ += offset.value();
    return *this;
  }

  bool operator==(const ColumnNumberUnsignedOffset& rhs) const {
    return value_ == rhs.value_;
  }

  bool operator!=(const ColumnNumberUnsignedOffset& rhs) const {
    return !(*this == rhs);
  }

  uint32_t value() const { return value_; }

  uint32_t* addressOfValueForTranscode() { return &value_; }
};

struct TaggedColumnNumberOneOrigin;

namespace detail {

// Shared implementation of {,Limited}ColumnNumberOneOrigin classes.
//
// LimitValue being 0 means there's no limit.
template <uint32_t LimitValue = 0>
struct MaybeLimitedColumnNumber {
 public:
  static constexpr uint32_t OriginValue = 1;

 protected:
  uint32_t value_ = OriginValue;

  friend struct ::JS::TaggedColumnNumberOneOrigin;

 public:
  constexpr MaybeLimitedColumnNumber() = default;
  MaybeLimitedColumnNumber(const MaybeLimitedColumnNumber& other) = default;
  MaybeLimitedColumnNumber& operator=(const MaybeLimitedColumnNumber& other) =
      default;

  explicit MaybeLimitedColumnNumber(uint32_t value) : value_(value) {
    MOZ_ASSERT(valid());
  }

  bool operator==(const MaybeLimitedColumnNumber<LimitValue>& rhs) const {
    return value_ == rhs.value_;
  }

  bool operator!=(const MaybeLimitedColumnNumber<LimitValue>& rhs) const {
    return !(*this == rhs);
  }

  MaybeLimitedColumnNumber<LimitValue> operator+(
      const ColumnNumberOffset& offset) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(ptrdiff_t(value_) + offset.value() >= 0);
    return MaybeLimitedColumnNumber<LimitValue>(value_ + offset.value());
  }

  MaybeLimitedColumnNumber<LimitValue> operator+(
      const ColumnNumberUnsignedOffset& offset) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(ptrdiff_t(value_) + offset.value() >= 0);
    return MaybeLimitedColumnNumber<LimitValue>(value_ + offset.value());
  }

  MaybeLimitedColumnNumber<LimitValue> operator-(
      const ColumnNumberOffset& offset) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(ptrdiff_t(value_) - offset.value() >= 0);
    return MaybeLimitedColumnNumber<LimitValue>(value_ - offset.value());
  }
  ColumnNumberOffset operator-(
      const MaybeLimitedColumnNumber<LimitValue>& other) const {
    MOZ_ASSERT(valid());
    return ColumnNumberOffset(int32_t(value_) - int32_t(other.value_));
  }

  MaybeLimitedColumnNumber<LimitValue>& operator+=(
      const ColumnNumberOffset& offset) {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(ptrdiff_t(value_) + offset.value() >= 0);
    value_ += offset.value();
    MOZ_ASSERT(valid());
    return *this;
  }
  MaybeLimitedColumnNumber<LimitValue>& operator-=(
      const ColumnNumberOffset& offset) {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(ptrdiff_t(value_) - offset.value() >= 0);
    value_ -= offset.value();
    MOZ_ASSERT(valid());
    return *this;
  }

  bool operator<(const MaybeLimitedColumnNumber<LimitValue>& rhs) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(rhs.valid());
    return value_ < rhs.value_;
  }
  bool operator<=(const MaybeLimitedColumnNumber<LimitValue>& rhs) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(rhs.valid());
    return value_ <= rhs.value_;
  }
  bool operator>(const MaybeLimitedColumnNumber<LimitValue>& rhs) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(rhs.valid());
    return value_ > rhs.value_;
  }
  bool operator>=(const MaybeLimitedColumnNumber<LimitValue>& rhs) const {
    MOZ_ASSERT(valid());
    MOZ_ASSERT(rhs.valid());
    return value_ >= rhs.value_;
  }

  uint32_t oneOriginValue() const {
    MOZ_ASSERT(valid());

    return value_;
  }

  uint32_t* addressOfValueForTranscode() { return &value_; }

  bool valid() const {
    if constexpr (LimitValue == 0) {
      return true;
    }

    MOZ_ASSERT(value_ != 0);

    return value_ <= LimitValue;
  }
};

// See the comment for LimitedColumnNumberOneOrigin below
static constexpr uint32_t ColumnNumberOneOriginLimit =
    std::numeric_limits<int32_t>::max() / 2;

}  // namespace detail

// Column number in 1-origin with 31-bit limit.
//
// Various parts of the engine requires the column number be represented in
// 31 bits.
//
// See:
//  * TaggedColumnNumberOneOrigin
//  * TokenStreamAnyChars::checkOptions
//  * SourceNotes::isRepresentable
//  * WasmFrameIter::computeLine
struct LimitedColumnNumberOneOrigin : public detail::MaybeLimitedColumnNumber<
                                          detail::ColumnNumberOneOriginLimit> {
 private:
  using Base =
      detail::MaybeLimitedColumnNumber<detail::ColumnNumberOneOriginLimit>;

 public:
  static constexpr uint32_t Limit = detail::ColumnNumberOneOriginLimit;

  static_assert(uint32_t(Limit + Limit) > Limit,
                "Adding Limit should not overflow");

  using Base::Base;

  LimitedColumnNumberOneOrigin() = default;
  LimitedColumnNumberOneOrigin(const LimitedColumnNumberOneOrigin& other) =
      default;
  MOZ_IMPLICIT LimitedColumnNumberOneOrigin(const Base& other) : Base(other) {}

  static LimitedColumnNumberOneOrigin limit() {
    return LimitedColumnNumberOneOrigin(Limit);
  }

  static LimitedColumnNumberOneOrigin fromUnlimited(uint32_t value) {
    if (value > Limit) {
      return LimitedColumnNumberOneOrigin(Limit);
    }
    return LimitedColumnNumberOneOrigin(value);
  }
  static LimitedColumnNumberOneOrigin fromUnlimited(
      const MaybeLimitedColumnNumber<0>& value) {
    return fromUnlimited(value.oneOriginValue());
  }

  static LimitedColumnNumberOneOrigin fromZeroOrigin(uint32_t value) {
    return LimitedColumnNumberOneOrigin(value + 1);
  }
};

// Column number in 1-origin.
struct ColumnNumberOneOrigin : public detail::MaybeLimitedColumnNumber<0> {
 private:
  using Base = detail::MaybeLimitedColumnNumber<0>;

 public:
  using Base::Base;
  using Base::operator=;

  ColumnNumberOneOrigin() = default;
  ColumnNumberOneOrigin(const ColumnNumberOneOrigin& other) = default;
  ColumnNumberOneOrigin& operator=(ColumnNumberOneOrigin&) = default;

  MOZ_IMPLICIT ColumnNumberOneOrigin(const Base& other) : Base(other) {}

  explicit ColumnNumberOneOrigin(const LimitedColumnNumberOneOrigin& other)
      : Base(other.oneOriginValue()) {}

  static ColumnNumberOneOrigin fromZeroOrigin(uint32_t value) {
    return ColumnNumberOneOrigin(value + 1);
  }
};

// Either LimitedColumnNumberOneOrigin, or WasmFunctionIndex.
//
// In order to pass the Wasm frame's (url, bytecode-offset, func-index) tuple
// through the existing (url, line, column) tuple, it tags the highest bit of
// the column to indicate "this is a wasm frame".
//
// When knowing clients see this bit, they shall render the tuple
// (url, line, column|bit) as "url:wasm-function[column]:0xline" according
// to the WebAssembly Web API's Developer-Facing Display Conventions.
//   https://webassembly.github.io/spec/web-api/index.html#conventions
// The wasm bytecode offset continues to be passed as the JS line to avoid
// breaking existing devtools code written when this used to be the case.
//
// 0b0YYYYYYY_YYYYYYYY_YYYYYYYY_YYYYYYYY LimitedColumnNumberOneOrigin
// 0b1YYYYYYY_YYYYYYYY_YYYYYYYY_YYYYYYYY WasmFunctionIndex
//
// The tagged colum number shouldn't escape the JS engine except for the
// following places:
//   * SavedFrame API which can directly access WASM frame's info
//   * ubi::Node API which can also directly access WASM frame's info
struct TaggedColumnNumberOneOrigin {
  static constexpr uint32_t WasmFunctionTag = 1u << 31;

  static_assert((WasmFunctionIndex::Limit & WasmFunctionTag) == 0);
  static_assert((LimitedColumnNumberOneOrigin::Limit & WasmFunctionTag) == 0);

 protected:
  uint32_t value_ = LimitedColumnNumberOneOrigin::OriginValue;

  explicit TaggedColumnNumberOneOrigin(uint32_t value) : value_(value) {}

 public:
  constexpr TaggedColumnNumberOneOrigin() = default;
  TaggedColumnNumberOneOrigin(const TaggedColumnNumberOneOrigin& other) =
      default;

  explicit TaggedColumnNumberOneOrigin(
      const LimitedColumnNumberOneOrigin& other)
      : value_(other.value_) {
    MOZ_ASSERT(isLimitedColumnNumber());
  }
  explicit TaggedColumnNumberOneOrigin(const WasmFunctionIndex& other)
      : value_(other.value() | WasmFunctionTag) {
    MOZ_ASSERT(isWasmFunctionIndex());
  }

  static TaggedColumnNumberOneOrigin fromRaw(uint32_t value) {
    return TaggedColumnNumberOneOrigin(value);
  }

  static TaggedColumnNumberOneOrigin forDifferentialTesting() {
    return TaggedColumnNumberOneOrigin(LimitedColumnNumberOneOrigin());
  }

  bool operator==(const TaggedColumnNumberOneOrigin& rhs) const {
    return value_ == rhs.value_;
  }

  bool operator!=(const TaggedColumnNumberOneOrigin& rhs) const {
    return !(*this == rhs);
  }

  bool isLimitedColumnNumber() const { return !isWasmFunctionIndex(); }

  bool isWasmFunctionIndex() const { return !!(value_ & WasmFunctionTag); }

  LimitedColumnNumberOneOrigin toLimitedColumnNumber() const {
    MOZ_ASSERT(isLimitedColumnNumber());
    return LimitedColumnNumberOneOrigin(value_);
  }

  WasmFunctionIndex toWasmFunctionIndex() const {
    MOZ_ASSERT(isWasmFunctionIndex());
    return WasmFunctionIndex(value_ & ~WasmFunctionTag);
  }

  uint32_t oneOriginValue() const {
    return isWasmFunctionIndex()
               ? WasmFunctionIndex::DefaultBinarySourceColumnNumberOneOrigin
               : toLimitedColumnNumber().oneOriginValue();
  }

  uint32_t rawValue() const { return value_; }

  uint32_t* addressOfValueForTranscode() { return &value_; }
};

}  // namespace JS

#endif /* js_ColumnNumber_h */
