/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Shadow definition of |JS::Symbol| innards.  Do not use this directly! */

#ifndef js_shadow_Symbol_h
#define js_shadow_Symbol_h

#include <stdint.h>  // uint32_t

namespace js {
namespace gc {
struct Cell;
}  // namespace gc
}  // namespace js

namespace JS {

namespace shadow {

struct Symbol {
  void* _1;
  uint32_t code_;
  static constexpr uint32_t WellKnownAPILimit = 0x80000000;

  static bool isWellKnownSymbol(const js::gc::Cell* cell) {
    return reinterpret_cast<const Symbol*>(cell)->code_ < WellKnownAPILimit;
  }
};

}  // namespace shadow

}  // namespace JS

#endif  // js_shadow_Symbol_h
