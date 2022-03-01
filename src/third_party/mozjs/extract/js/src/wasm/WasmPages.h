/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_pages_h
#define wasm_pages_h

#include "mozilla/CheckedInt.h"

#include <stdint.h>

namespace js {
namespace wasm {

// Pages is a typed unit representing a multiple of wasm::PageSize. We
// generally use pages as the unit of length when representing linear memory
// lengths so as to avoid overflow when the specified initial or maximum pages
// would overflow the native word size.
//
// Modules may specify pages up to 2^48 inclusive and so Pages is 64-bit on all
// platforms.
//
// We represent byte lengths using the native word size, as it is assumed that
// consumers of this API will only need byte lengths once it is time to
// allocate memory, at which point the pages will be checked against the
// implementation limits `MaxMemory32Pages()` and will then be guaranteed to
// fit in a native word.
struct Pages {
 private:
  // Pages are specified by limit fields, which in general may be up to 2^48,
  // so we must use uint64_t here.
  uint64_t value_;

 public:
  constexpr explicit Pages(uint64_t value) : value_(value) {}

  // Get the wrapped page value. Only use this if you must, prefer to use or
  // add new APIs to Page.
  uint64_t value() const { return value_; }

  // Converts from a byte length to pages, assuming that the length is an
  // exact multiple of the page size.
  static Pages fromByteLengthExact(size_t byteLength) {
    MOZ_ASSERT(byteLength % PageSize == 0);
    return Pages(byteLength / PageSize);
  }

  // Return whether the page length may overflow when converted to a byte
  // length in the native word size.
  bool hasByteLength() const {
    mozilla::CheckedInt<size_t> length(value_);
    length *= PageSize;
    return length.isValid();
  }

  // Converts from pages to byte length in the native word size. Users must
  // check for overflow, or be assured else-how that overflow cannot happen.
  size_t byteLength() const {
    mozilla::CheckedInt<size_t> length(value_);
    length *= PageSize;
    return length.value();
  }

  // Increment this pages by delta and return whether the resulting value
  // did not overflow. If there is no overflow, then this is set to the
  // resulting value.
  bool checkedIncrement(Pages delta) {
    mozilla::CheckedInt<uint64_t> newValue = value_;
    newValue += delta.value_;
    if (!newValue.isValid()) {
      return false;
    }
    value_ = newValue.value();
    return true;
  }

  // Implement pass-through comparison operators so that Pages can be compared.

  bool operator==(Pages other) const { return value_ == other.value_; }
  bool operator!=(Pages other) const { return value_ != other.value_; }
  bool operator<=(Pages other) const { return value_ <= other.value_; }
  bool operator<(Pages other) const { return value_ < other.value_; }
  bool operator>=(Pages other) const { return value_ >= other.value_; }
  bool operator>(Pages other) const { return value_ > other.value_; }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_pages_h
