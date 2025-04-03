/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_CheckIsObjectKind_h
#define vm_CheckIsObjectKind_h

#include <stdint.h>  // uint8_t

namespace js {

enum class CheckIsObjectKind : uint8_t {
  IteratorNext,
  IteratorReturn,
  IteratorThrow,
  GetIterator,
  GetAsyncIterator
};

}  // namespace js

#endif /* vm_CheckIsObjectKind_h */
