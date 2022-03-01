/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BytecodeIterator_inl_h
#define vm_BytecodeIterator_inl_h

#include "vm/BytecodeIterator.h"

#include "vm/JSScript.h"

namespace js {

inline BytecodeIterator::BytecodeIterator(const JSScript* script)
    : current_(script, script->code()) {}

// AllBytecodesIterable

inline BytecodeIterator AllBytecodesIterable::begin() {
  return BytecodeIterator(script_);
}

inline BytecodeIterator AllBytecodesIterable::end() {
  return BytecodeIterator(BytecodeLocation(script_, script_->codeEnd()));
}

// BytecodeLocationRange

inline BytecodeIterator BytecodeLocationRange::begin() {
  return BytecodeIterator(beginLoc_);
}

inline BytecodeIterator BytecodeLocationRange::end() {
  return BytecodeIterator(endLoc_);
}

}  // namespace js
#endif
