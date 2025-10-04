/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_FormatBuffer_h
#define builtin_intl_FormatBuffer_h

#include "mozilla/Assertions.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"

#include <stddef.h>
#include <stdint.h>

#include "js/AllocPolicy.h"
#include "js/CharacterEncoding.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "vm/StringType.h"

namespace js::intl {

/**
 * A buffer for formatting unified intl data.
 */
template <typename CharT, size_t MinInlineCapacity = 0,
          class AllocPolicy = TempAllocPolicy>
class FormatBuffer {
 public:
  using CharType = CharT;

  // Allow move constructors, but not copy constructors, as this class owns a
  // js::Vector.
  FormatBuffer(FormatBuffer&& other) noexcept = default;
  FormatBuffer& operator=(FormatBuffer&& other) noexcept = default;

  explicit FormatBuffer(AllocPolicy aP = AllocPolicy())
      : buffer_(std::move(aP)) {
    // The initial capacity matches the requested minimum inline capacity, as
    // long as it doesn't exceed |Vector::kMaxInlineBytes / sizeof(CharT)|. If
    // this assertion should ever fail, either reduce |MinInlineCapacity| or
    // make the FormatBuffer initialization fallible.
    MOZ_ASSERT(buffer_.capacity() == MinInlineCapacity);
    if constexpr (MinInlineCapacity > 0) {
      // Ensure the full capacity is marked as reserved.
      //
      // Reserving the minimum inline capacity can never fail, even when
      // simulating OOM.
      MOZ_ALWAYS_TRUE(buffer_.reserve(MinInlineCapacity));
    }
  }

  // Implicitly convert to a Span.
  operator mozilla::Span<CharType>() { return buffer_; }
  operator mozilla::Span<const CharType>() const { return buffer_; }

  /**
   * Ensures the buffer has enough space to accommodate |size| elements.
   */
  [[nodiscard]] bool reserve(size_t size) {
    // Call |reserve| a second time to ensure its full capacity is marked as
    // reserved.
    return buffer_.reserve(size) && buffer_.reserve(buffer_.capacity());
  }

  /**
   * Returns the raw data inside the buffer.
   */
  CharType* data() { return buffer_.begin(); }

  /**
   * Returns the count of elements written into the buffer.
   */
  size_t length() const { return buffer_.length(); }

  /**
   * Returns the buffer's overall capacity.
   */
  size_t capacity() const { return buffer_.capacity(); }

  /**
   * Resizes the buffer to the given amount of written elements.
   */
  void written(size_t amount) {
    MOZ_ASSERT(amount <= buffer_.capacity());
    // This sets |buffer_|'s internal size so that it matches how much was
    // written. This is necessary because the write happens across FFI
    // boundaries.
    size_t curLength = length();
    if (amount > curLength) {
      buffer_.infallibleGrowByUninitialized(amount - curLength);
    } else {
      buffer_.shrinkBy(curLength - amount);
    }
  }

  /**
   * Copies the buffer's data to a JSString.
   *
   * TODO(#1715842) - This should be more explicit on needing to handle OOM
   * errors. In this case it returns a nullptr that must be checked, but it may
   * not be obvious.
   */
  JSLinearString* toString(JSContext* cx) const {
    if constexpr (std::is_same_v<CharT, uint8_t> ||
                  std::is_same_v<CharT, unsigned char> ||
                  std::is_same_v<CharT, char>) {
      // Handle the UTF-8 encoding case.
      return NewStringCopyUTF8N(
          cx, JS::UTF8Chars(buffer_.begin(), buffer_.length()));
    } else {
      // Handle the UTF-16 encoding case.
      static_assert(std::is_same_v<CharT, char16_t>);
      return NewStringCopyN<CanGC>(cx, buffer_.begin(), buffer_.length());
    }
  }

  /**
   * Copies the buffer's data to a JSString. The buffer must contain only
   * ASCII characters.
   */
  JSLinearString* toAsciiString(JSContext* cx) const {
    static_assert(std::is_same_v<CharT, char>);

    MOZ_ASSERT(mozilla::IsAscii(buffer_));
    return NewStringCopyN<CanGC>(cx, buffer_.begin(), buffer_.length());
  }

  /**
   * Extract this buffer's content as a null-terminated string.
   */
  UniquePtr<CharType[], JS::FreePolicy> extractStringZ() {
    // Adding the NUL character on an already null-terminated string is likely
    // an error. If there's ever a valid use case which triggers this assertion,
    // we should change the below code to only conditionally add '\0'.
    MOZ_ASSERT_IF(!buffer_.empty(), buffer_.end()[-1] != '\0');

    if (!buffer_.append('\0')) {
      return nullptr;
    }
    return UniquePtr<CharType[], JS::FreePolicy>(
        buffer_.extractOrCopyRawBuffer());
  }

 private:
  js::Vector<CharT, MinInlineCapacity, AllocPolicy> buffer_;
};

}  // namespace js::intl

#endif /* builtin_intl_FormatBuffer_h */
