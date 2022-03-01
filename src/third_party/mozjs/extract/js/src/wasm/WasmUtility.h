/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_utility_h
#define wasm_utility_h

#include <algorithm>
namespace js {
namespace wasm {

template <class Container1, class Container2>
static inline bool EqualContainers(const Container1& lhs,
                                   const Container2& rhs) {
  return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

}  // namespace wasm
}  // namespace js

#endif  // wasm_utility_h
