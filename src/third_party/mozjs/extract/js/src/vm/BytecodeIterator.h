/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BytecodeIterator_h
#define vm_BytecodeIterator_h

#include "vm/BytecodeLocation.h"

namespace js {

class BytecodeIterator {
  BytecodeLocation current_;

 public:
  inline explicit BytecodeIterator(const JSScript* script);

  explicit BytecodeIterator(BytecodeLocation loc) : current_(loc) {}

  BytecodeIterator& operator=(const BytecodeIterator&) = default;

  bool operator==(const BytecodeIterator& other) const {
    return other.current_ == current_;
  }

  bool operator!=(const BytecodeIterator& other) const {
    return !(other.current_ == current_);
  }

  const BytecodeLocation& operator*() const { return current_; }

  const BytecodeLocation* operator->() const { return &current_; }

  // Pre-increment
  BytecodeIterator& operator++() {
    current_ = current_.next();
    return *this;
  }

  // Post-increment
  BytecodeIterator operator++(int) {
    BytecodeIterator previous(*this);
    current_ = current_.next();
    return previous;
  }
};

// Given a JSScript, allow the construction of a range based for-loop
// that will visit all script locations in that script.
class AllBytecodesIterable {
  const JSScript* script_;

 public:
  explicit AllBytecodesIterable(const JSScript* script) : script_(script) {}

  BytecodeIterator begin();
  BytecodeIterator end();
};

// Construct a range based iterator that will visit all bytecode locations
// between two given bytecode locations.
// `beginLoc_` is the bytecode location where the iterator will start, and
// `endLoc_` is the bytecode location where the iterator will end.
class BytecodeLocationRange {
  BytecodeLocation beginLoc_;
  BytecodeLocation endLoc_;

 public:
  explicit BytecodeLocationRange(BytecodeLocation beginLoc,
                                 BytecodeLocation endLoc)
      : beginLoc_(beginLoc), endLoc_(endLoc) {
#ifdef DEBUG
    MOZ_ASSERT(beginLoc.hasSameScript(endLoc));
#endif
  }

  BytecodeIterator begin();
  BytecodeIterator end();
};

}  // namespace js

#endif
