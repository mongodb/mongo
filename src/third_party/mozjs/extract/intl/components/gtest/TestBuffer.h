/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef intl_components_gtest_TestBuffer_h_
#define intl_components_gtest_TestBuffer_h_

#include <string_view>
#include "mozilla/DebugOnly.h"
#include "mozilla/Vector.h"

namespace mozilla::intl {

/**
 * A test buffer for interfacing with unified intl classes.
 * Closely resembles the FormatBuffer class, but without
 * JavaScript-specific implementation details.
 */
template <typename C, size_t inlineCapacity = 0>
class TestBuffer {
 public:
  using CharType = C;

  // Only allow moves, and not copies, as this class owns the mozilla::Vector.
  TestBuffer(TestBuffer&& other) noexcept = default;
  TestBuffer& operator=(TestBuffer&& other) noexcept = default;

  explicit TestBuffer(const size_t aSize = 0) { reserve(aSize); }

  /**
   * Ensures the buffer has enough space to accommodate |aSize| elemtns.
   */
  bool reserve(const size_t aSize) { return mBuffer.reserve(aSize); }

  /**
   * Returns the raw data inside the buffer.
   */
  CharType* data() { return mBuffer.begin(); }

  /**
   * Returns the count of elements in written to the buffer.
   */
  size_t length() const { return mBuffer.length(); }

  /**
   * Returns the buffer's overall capacity.
   */
  size_t capacity() const { return mBuffer.capacity(); }

  /**
   * Resizes the buffer to the given amount of written elements.
   * This is necessary because the buffer gets written to across
   * FFI boundaries, so this needs to happen in a separate step.
   */
  void written(size_t aAmount) {
    MOZ_ASSERT(aAmount <= mBuffer.capacity());
    mozilla::DebugOnly<bool> result = mBuffer.resizeUninitialized(aAmount);
    MOZ_ASSERT(result);
  }

  /**
   * Get a string view into the buffer, which is useful for test assertions.
   */
  template <typename C2>
  std::basic_string_view<const C2> get_string_view() {
    return std::basic_string_view<const C2>(reinterpret_cast<const C2*>(data()),
                                            length());
  }

  Vector<C, inlineCapacity> mBuffer{};
};

}  // namespace mozilla::intl

#endif
