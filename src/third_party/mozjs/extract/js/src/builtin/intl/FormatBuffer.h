/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_FormatBuffer_h
#define builtin_intl_FormatBuffer_h

#include "mozilla/Assertions.h"
#include "mozilla/Range.h"
#include "mozilla/Span.h"

#include <stddef.h>
#include <stdint.h>

#include "gc/Allocator.h"
#include "js/AllocPolicy.h"
#include "js/TypeDecls.h"
#include "js/Vector.h"
#include "vm/StringType.h"

namespace js::intl {

/**
 * A buffer for formatting unified intl data.
 */
template <typename CharT, size_t MinInlineCapacity = 0>
class FormatBuffer {
 public:
  using CharType = CharT;

  // Allow move constructors, but not copy constructors, as this class owns a
  // js::Vector.
  FormatBuffer(FormatBuffer&& other) noexcept = default;
  FormatBuffer& operator=(FormatBuffer&& other) noexcept = default;

  explicit FormatBuffer(JSContext* cx) : cx_(cx), buffer_(cx) {
    MOZ_ASSERT(cx);
  }

  // Implicitly convert to a Span.
  operator mozilla::Span<CharType>() { return buffer_; }
  operator mozilla::Span<const CharType>() const { return buffer_; }

  /**
   * Ensures the buffer has enough space to accommodate |size| elements.
   */
  [[nodiscard]] bool reserve(size_t size) { return buffer_.reserve(size); }

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
    mozilla::DebugOnly<bool> result = buffer_.resizeUninitialized(amount);
    MOZ_ASSERT(result);
  }

  /**
   * Copies the buffer's data to a JSString.
   *
   * TODO(#1715842) - This should be more explicit on needing to handle OOM
   * errors. In this case it returns a nullptr that must be checked, but it may
   * not be obvious.
   */
  JSString* toString() const {
    if constexpr (std::is_same_v<CharT, uint8_t> ||
                  std::is_same_v<CharT, unsigned char> ||
                  std::is_same_v<CharT, char>) {
      // Handle the UTF-8 encoding case.
      return NewStringCopyUTF8N<CanGC>(
          cx_, mozilla::Range(reinterpret_cast<unsigned char>(buffer_.begin()),
                              buffer_.length()));
    } else {
      // Handle the UTF-16 encoding case.
      static_assert(std::is_same_v<CharT, char16_t>);
      return NewStringCopyN<CanGC>(cx_, buffer_.begin(), buffer_.length());
    }
  }

 private:
  JSContext* cx_;
  js::Vector<CharT, MinInlineCapacity> buffer_;
};

}  // namespace js::intl

#endif /* builtin_intl_FormatBuffer_h */
