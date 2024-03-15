/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TypedIndex_h
#define frontend_TypedIndex_h

#include <cstdint>
#include <stddef.h>

namespace js {
namespace frontend {

// TypedIndex allows discrimination in variants between different
// index types. Used as a typesafe index for various stencil arrays.
template <typename Tag>
struct TypedIndex {
  TypedIndex() = default;
  constexpr explicit TypedIndex(uint32_t index) : index(index){};

  uint32_t index = 0;

  // For Vector::operator[]
  operator size_t() const { return index; }

  TypedIndex& operator=(size_t idx) {
    index = idx;
    return *this;
  }

  bool operator<(TypedIndex other) const { return index < other.index; }
  bool operator<=(TypedIndex other) const { return index <= other.index; }
  bool operator>(TypedIndex other) const { return index > other.index; }
  bool operator>=(TypedIndex other) const { return index >= other.index; }
};

}  // namespace frontend
}  // namespace js

#endif
