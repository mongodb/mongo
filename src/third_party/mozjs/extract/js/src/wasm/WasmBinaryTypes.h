/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_WasmBinaryTypes_h
#define wasm_WasmBinaryTypes_h

#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/Vector.h"

#include "js/AllocPolicy.h"

#include "wasm/WasmSerialize.h"
#include "wasm/WasmShareable.h"

namespace js {
namespace wasm {

using BytecodeSpan = mozilla::Span<const uint8_t>;

// This struct captures a range of bytecode.
struct BytecodeRange {
  BytecodeRange() = default;

  // Infallible constructor for when we're sure the start and size are valid.
  BytecodeRange(uint32_t start, uint32_t size) : start(start), end(start) {
    mozilla::CheckedUint32 checkedEnd(start);
    checkedEnd += size;
    MOZ_RELEASE_ASSERT(checkedEnd.isValid());
    end = checkedEnd.value();
  }

  // Fallible constructor for when there could be overflow.
  [[nodiscard]] static bool fromStartAndSize(uint32_t start, uint32_t size,
                                             BytecodeRange* range) {
    mozilla::CheckedUint32 checkedEnd(start);
    checkedEnd += size;
    if (!checkedEnd.isValid()) {
      return false;
    }
    range->start = start;
    range->end = checkedEnd.value();
    return true;
  }

  uint32_t start = 0;
  uint32_t end = 0;

  WASM_CHECK_CACHEABLE_POD(start, end);

  uint32_t size() const { return end - start; }

  bool isEmpty() const { return start == end; }

  // Returns whether a range is a non-strict subset of this range.
  bool contains(const BytecodeRange& other) const {
    return other.start >= start && other.end <= end;
  }

  // Returns whether an offset is contained in this range.
  bool containsOffset(uint32_t bytecodeOffset) const {
    return bytecodeOffset >= start && bytecodeOffset < end;
  }

  // Compare where an offset falls relative to this range. This returns `0` if
  // it is contained in this range, `-1` if it falls before the range, and `1`
  // if it is after the range.
  int compareOffset(uint32_t bytecodeOffset) const {
    if (containsOffset(bytecodeOffset)) {
      return 0;
    }
    if (bytecodeOffset < start) {
      return -1;
    }
    MOZ_ASSERT(bytecodeOffset >= end);
    return 1;
  }

  bool operator==(const BytecodeRange& rhs) const {
    return start == rhs.start && end == rhs.end;
  }

  // Returns a range that represents `this` relative to `other`. `this` must
  // be wholly contained in `other`, no partial overlap is allowed.
  BytecodeRange relativeTo(const BytecodeRange& other) const {
    MOZ_RELEASE_ASSERT(other.contains(*this));
    return BytecodeRange(start - other.start, size());
  }

  // Gets the span that this range represents from a span of bytecode.
  BytecodeSpan toSpan(BytecodeSpan bytecode) const {
    MOZ_RELEASE_ASSERT(end <= bytecode.size());
    return BytecodeSpan(bytecode.begin() + start, bytecode.begin() + end);
  }

  // Gets the span that this range represents from a vector of bytecode.
  BytecodeSpan toSpan(const ShareableBytes& bytecode) const {
    MOZ_RELEASE_ASSERT(end <= bytecode.length());
    return BytecodeSpan(bytecode.begin() + start, bytecode.begin() + end);
  }
};

WASM_DECLARE_CACHEABLE_POD(BytecodeRange);

using MaybeBytecodeRange = mozilla::Maybe<BytecodeRange>;
using BytecodeRangeVector =
    mozilla::Vector<BytecodeRange, 0, SystemAllocPolicy>;

}  // namespace wasm
}  // namespace js

#endif /* wasm_WasmBinaryTypes_h */
