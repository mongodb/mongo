/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_UsingHint_h
#define vm_UsingHint_h

#include <stdint.h>  // uint8_t

namespace js {

// Explicit Resource Management Proposal
//
// DisposableResource Record [[Hint]] Field
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposableresource-records
enum class UsingHint : uint8_t {
  // sync-dispose
  Sync,
  // async-dispose
  Async,
};

}  // namespace js

#endif /* vm_UsingHint_h */
