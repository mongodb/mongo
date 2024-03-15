/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file forward-defines Irregexp classes that need to be visible
// to the rest of Spidermonkey and re-exports them into js::irregexp.

#ifndef regexp_RegExpTypes_h
#define regexp_RegExpTypes_h

#include "js/UniquePtr.h"

namespace js {
class MatchPairs;
}

namespace v8 {
namespace internal {

class ByteArrayData {
 public:
  uint32_t length;
  uint8_t* data();

  uint8_t get(uint32_t index) {
    MOZ_ASSERT(index < length);
    return data()[index];
  }
  void set(uint32_t index, uint8_t val) {
    MOZ_ASSERT(index < length);
    data()[index] = val;
  }

  // Used for FixedIntegerArray.
  template <typename T>
  T getTyped(uint32_t index);
  template <typename T>
  void setTyped(uint32_t index, T value);

 private:
  template <typename T>
  T* typedData();
};

class Isolate;
class RegExpStack;
class RegExpStackScope;

struct InputOutputData {
  const void* inputStart;
  const void* inputEnd;

  // Index into inputStart (in chars) at which to begin matching.
  size_t startIndex;

  js::MatchPairs* matches;

  template <typename CharT>
  InputOutputData(const CharT* inputStart, const CharT* inputEnd,
                  size_t startIndex, js::MatchPairs* matches)
      : inputStart(inputStart),
        inputEnd(inputEnd),
        startIndex(startIndex),
        matches(matches) {}

  // Note: return int32_t instead of size_t to prevent signed => unsigned
  // conversions in caller functions.
  static constexpr int32_t offsetOfInputStart() {
    return int32_t(offsetof(InputOutputData, inputStart));
  }
  static constexpr int32_t offsetOfInputEnd() {
    return int32_t(offsetof(InputOutputData, inputEnd));
  }
  static constexpr int32_t offsetOfStartIndex() {
    return int32_t(offsetof(InputOutputData, startIndex));
  }
  static constexpr int32_t offsetOfMatches() {
    return int32_t(offsetof(InputOutputData, matches));
  }
};

}  // namespace internal
}  // namespace v8

namespace js {
namespace irregexp {

using Isolate = v8::internal::Isolate;
using RegExpStack = v8::internal::RegExpStack;
using RegExpStackScope = v8::internal::RegExpStackScope;
using ByteArrayData = v8::internal::ByteArrayData;
using ByteArray = js::UniquePtr<v8::internal::ByteArrayData, JS::FreePolicy>;
using InputOutputData = v8::internal::InputOutputData;

}  // namespace irregexp
}  // namespace js

#endif  // regexp_RegExpTypes_h
