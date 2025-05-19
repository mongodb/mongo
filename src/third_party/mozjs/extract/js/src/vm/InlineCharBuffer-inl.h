/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_InlineCharBuffer_inl_h
#define vm_InlineCharBuffer_inl_h

#include "vm/JSAtomUtils.h"

#include "vm/StringType-inl.h"

namespace js {

template <typename CharT>
struct MaximumInlineLength;

template <>
struct MaximumInlineLength<Latin1Char> {
  static constexpr size_t value = JSFatInlineString::MAX_LENGTH_LATIN1;
};

template <>
struct MaximumInlineLength<char16_t> {
  static constexpr size_t value = JSFatInlineString::MAX_LENGTH_TWO_BYTE;
};

// Character buffer class used for ToLowerCase and ToUpperCase operations, as
// well as other string operations where the final string length is known in
// advance.
//
// Case conversion operations normally return a string with the same length as
// the input string. To avoid over-allocation, we optimistically allocate an
// array with same size as the input string and only when we detect special
// casing characters, which can change the output string length, we reallocate
// the output buffer to the final string length.
//
// As a further mean to improve runtime performance, the character buffer
// contains an inline storage, so we don't need to heap-allocate an array when
// a JSInlineString will be used for the output string.
//
// Why not use mozilla::Vector instead? mozilla::Vector doesn't provide enough
// fine-grained control to avoid over-allocation when (re)allocating for exact
// buffer sizes. This led to visible performance regressions in µ-benchmarks.
template <typename CharT>
class MOZ_NON_PARAM InlineCharBuffer {
  static constexpr size_t InlineCapacity = MaximumInlineLength<CharT>::value;

  CharT inlineStorage[InlineCapacity];
  UniquePtr<CharT[], JS::FreePolicy> heapStorage;

#ifdef DEBUG
  // In debug mode, we keep track of the requested string lengths to ensure
  // all character buffer methods are called in the correct order and with
  // the expected argument values.
  size_t lastRequestedLength = 0;

  void assertValidRequest(size_t expectedLastLength, size_t length) {
    MOZ_ASSERT(length >= expectedLastLength, "cannot shrink requested length");
    MOZ_ASSERT(lastRequestedLength == expectedLastLength);
    lastRequestedLength = length;
  }
#else
  void assertValidRequest(size_t expectedLastLength, size_t length) {}
#endif

 public:
  CharT* get() { return heapStorage ? heapStorage.get() : inlineStorage; }

  bool maybeAlloc(JSContext* cx, size_t length) {
    assertValidRequest(0, length);

    if (length <= InlineCapacity) {
      return true;
    }

    MOZ_ASSERT(!heapStorage, "heap storage already allocated");
    heapStorage =
        cx->make_pod_arena_array<CharT>(js::StringBufferArena, length);
    return !!heapStorage;
  }

  bool maybeRealloc(JSContext* cx, size_t oldLength, size_t newLength) {
    assertValidRequest(oldLength, newLength);

    if (newLength <= InlineCapacity) {
      return true;
    }

    if (!heapStorage) {
      heapStorage =
          cx->make_pod_arena_array<CharT>(js::StringBufferArena, newLength);
      if (!heapStorage) {
        return false;
      }

      MOZ_ASSERT(oldLength <= InlineCapacity);
      mozilla::PodCopy(heapStorage.get(), inlineStorage, oldLength);
      return true;
    }

    CharT* oldChars = heapStorage.release();
    CharT* newChars = cx->pod_arena_realloc(js::StringBufferArena, oldChars,
                                            oldLength, newLength);
    if (!newChars) {
      js_free(oldChars);
      return false;
    }

    heapStorage.reset(newChars);
    return true;
  }

  JSString* toStringDontDeflate(JSContext* cx, size_t length,
                                js::gc::Heap heap = js::gc::Heap::Default) {
    MOZ_ASSERT(length == lastRequestedLength);

    if (JSInlineString::lengthFits<CharT>(length)) {
      MOZ_ASSERT(
          !heapStorage,
          "expected only inline storage when length fits in inline string");

      if (JSString* str = TryEmptyOrStaticString(cx, inlineStorage, length)) {
        return str;
      }

      mozilla::Range<const CharT> range(inlineStorage, length);
      return NewInlineString<CanGC>(cx, range, heap);
    }

    MOZ_ASSERT(heapStorage,
               "heap storage was not allocated for non-inline string");

    return NewStringDontDeflate<CanGC>(cx, std::move(heapStorage), length,
                                       heap);
  }

  JSString* toString(JSContext* cx, size_t length,
                     js::gc::Heap heap = js::gc::Heap::Default) {
    MOZ_ASSERT(length == lastRequestedLength);

    if (JSInlineString::lengthFits<CharT>(length)) {
      MOZ_ASSERT(
          !heapStorage,
          "expected only inline storage when length fits in inline string");

      return NewStringCopyN<CanGC>(cx, inlineStorage, length, heap);
    }

    MOZ_ASSERT(heapStorage,
               "heap storage was not allocated for non-inline string");

    return NewString<CanGC>(cx, std::move(heapStorage), length, heap);
  }

  JSAtom* toAtom(JSContext* cx, size_t length) {
    MOZ_ASSERT(length == lastRequestedLength);
    return AtomizeChars(cx, get(), length);
  }
};

} /* namespace js */

#endif /* vm_InlineCharBuffer_inl_h */
