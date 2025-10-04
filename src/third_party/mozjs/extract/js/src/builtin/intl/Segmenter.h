/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_Segmenter_h
#define builtin_intl_Segmenter_h

#include <stdint.h>
#include <type_traits>

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

struct JS_PUBLIC_API JSContext;
class JSString;

namespace JS {
class GCContext;
}

namespace js {

enum class SegmenterGranularity : int8_t { Grapheme, Word, Sentence };

class SegmenterObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t LOCALE_SLOT = 1;
  static constexpr uint32_t GRANULARITY_SLOT = 2;
  static constexpr uint32_t SEGMENTER_SLOT = 3;
  static constexpr uint32_t SLOT_COUNT = 4;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  JSString* getLocale() const {
    const auto& slot = getFixedSlot(LOCALE_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return slot.toString();
  }

  void setLocale(JSString* locale) {
    setFixedSlot(LOCALE_SLOT, StringValue(locale));
  }

  SegmenterGranularity getGranularity() const {
    const auto& slot = getFixedSlot(GRANULARITY_SLOT);
    if (slot.isUndefined()) {
      return SegmenterGranularity::Grapheme;
    }
    return static_cast<SegmenterGranularity>(slot.toInt32());
  }

  void setGranularity(SegmenterGranularity granularity) {
    setFixedSlot(GRANULARITY_SLOT,
                 Int32Value(static_cast<int32_t>(granularity)));
  }

  void* getSegmenter() const {
    const auto& slot = getFixedSlot(SEGMENTER_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return slot.toPrivate();
  }

  void setSegmenter(void* brk) {
    setFixedSlot(SEGMENTER_SLOT, PrivateValue(brk));
  }

 private:
  static const ClassSpec classSpec_;
  static const JSClassOps classOps_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

class SegmentsStringChars final {
  uintptr_t tagged_ = 0;

  enum Tag {
    Latin1 = 0,
    TwoByte = 1,

    TagMask = TwoByte,
  };

  static uintptr_t toTagged(const void* chars, Tag tag) {
    MOZ_ASSERT(chars != nullptr, "can't tag nullptr");

    auto ptr = reinterpret_cast<uintptr_t>(chars);
    MOZ_ASSERT((ptr & TagMask) == 0, "pointer already tagged");

    return ptr | tag;
  }

  Tag tag() const { return static_cast<Tag>(tagged_ & TagMask); }

  uintptr_t untagged() const { return tagged_ & ~TagMask; }

  explicit SegmentsStringChars(const void* taggedChars)
      : tagged_(reinterpret_cast<uintptr_t>(taggedChars)) {}

 public:
  SegmentsStringChars() = default;

  explicit SegmentsStringChars(const JS::Latin1Char* chars)
      : tagged_(toTagged(chars, Latin1)) {}

  explicit SegmentsStringChars(const char16_t* chars)
      : tagged_(toTagged(chars, TwoByte)) {}

  static auto fromTagged(const void* taggedChars) {
    return SegmentsStringChars{taggedChars};
  }

  explicit operator bool() const { return tagged_ != 0; }

  template <typename CharT>
  bool has() const {
    if constexpr (std::is_same_v<CharT, JS::Latin1Char>) {
      return tag() == Latin1;
    } else {
      static_assert(std::is_same_v<CharT, char16_t>);
      return tag() == TwoByte;
    }
  }

  template <typename CharT>
  CharT* data() const {
    MOZ_ASSERT(has<CharT>());
    return reinterpret_cast<CharT*>(untagged());
  }

  uintptr_t tagged() const { return tagged_; }
};

class SegmentsObject : public NativeObject {
 public:
  static const JSClass class_;

  static constexpr uint32_t SEGMENTER_SLOT = 0;
  static constexpr uint32_t STRING_SLOT = 1;
  static constexpr uint32_t STRING_CHARS_SLOT = 2;
  static constexpr uint32_t INDEX_SLOT = 3;
  static constexpr uint32_t GRANULARITY_SLOT = 4;
  static constexpr uint32_t BREAK_ITERATOR_SLOT = 5;
  static constexpr uint32_t SLOT_COUNT = 6;

  static_assert(STRING_SLOT == INTL_SEGMENTS_STRING_SLOT,
                "STRING_SLOT must match self-hosting define for string slot");

  SegmenterObject* getSegmenter() const {
    const auto& slot = getFixedSlot(SEGMENTER_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return &slot.toObject().as<SegmenterObject>();
  }

  void setSegmenter(SegmenterObject* segmenter) {
    setFixedSlot(SEGMENTER_SLOT, ObjectValue(*segmenter));
  }

  JSString* getString() const {
    const auto& slot = getFixedSlot(STRING_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return slot.toString();
  }

  void setString(JSString* str) { setFixedSlot(STRING_SLOT, StringValue(str)); }

  bool hasStringChars() const {
    return !getFixedSlot(STRING_CHARS_SLOT).isUndefined();
  }

  SegmentsStringChars getStringChars() const {
    const auto& slot = getFixedSlot(STRING_CHARS_SLOT);
    if (slot.isUndefined()) {
      return SegmentsStringChars{};
    }
    return SegmentsStringChars::fromTagged(slot.toPrivate());
  }

  void setStringChars(SegmentsStringChars chars) {
    setFixedSlot(STRING_CHARS_SLOT, PrivateValue(chars.tagged()));
  }

  bool hasLatin1StringChars() const {
    MOZ_ASSERT(hasStringChars());
    return getStringChars().has<JS::Latin1Char>();
  }

  int32_t getIndex() const {
    const auto& slot = getFixedSlot(INDEX_SLOT);
    if (slot.isUndefined()) {
      return 0;
    }
    return slot.toInt32();
  }

  void setIndex(int32_t index) { setFixedSlot(INDEX_SLOT, Int32Value(index)); }

  SegmenterGranularity getGranularity() const {
    const auto& slot = getFixedSlot(GRANULARITY_SLOT);
    if (slot.isUndefined()) {
      return SegmenterGranularity::Grapheme;
    }
    return static_cast<SegmenterGranularity>(slot.toInt32());
  }

  void setGranularity(SegmenterGranularity granularity) {
    setFixedSlot(GRANULARITY_SLOT,
                 Int32Value(static_cast<int32_t>(granularity)));
  }

  void* getBreakIterator() const {
    const auto& slot = getFixedSlot(BREAK_ITERATOR_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return slot.toPrivate();
  }

  void setBreakIterator(void* brk) {
    setFixedSlot(BREAK_ITERATOR_SLOT, PrivateValue(brk));
  }

 private:
  static const JSClassOps classOps_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

class SegmentIteratorObject : public NativeObject {
 public:
  static const JSClass class_;

  static constexpr uint32_t SEGMENTER_SLOT = 0;
  static constexpr uint32_t STRING_SLOT = 1;
  static constexpr uint32_t STRING_CHARS_SLOT = 2;
  static constexpr uint32_t INDEX_SLOT = 3;
  static constexpr uint32_t GRANULARITY_SLOT = 4;
  static constexpr uint32_t BREAK_ITERATOR_SLOT = 5;
  static constexpr uint32_t SLOT_COUNT = 6;

  static_assert(STRING_SLOT == INTL_SEGMENT_ITERATOR_STRING_SLOT,
                "STRING_SLOT must match self-hosting define for string slot");

  static_assert(INDEX_SLOT == INTL_SEGMENT_ITERATOR_INDEX_SLOT,
                "INDEX_SLOT must match self-hosting define for index slot");

  SegmenterObject* getSegmenter() const {
    const auto& slot = getFixedSlot(SEGMENTER_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return &slot.toObject().as<SegmenterObject>();
  }

  void setSegmenter(SegmenterObject* segmenter) {
    setFixedSlot(SEGMENTER_SLOT, ObjectOrNullValue(segmenter));
  }

  JSString* getString() const {
    const auto& slot = getFixedSlot(STRING_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return slot.toString();
  }

  void setString(JSString* str) { setFixedSlot(STRING_SLOT, StringValue(str)); }

  bool hasStringChars() const {
    return !getFixedSlot(STRING_CHARS_SLOT).isUndefined();
  }

  SegmentsStringChars getStringChars() const {
    const auto& slot = getFixedSlot(STRING_CHARS_SLOT);
    if (slot.isUndefined()) {
      return SegmentsStringChars{};
    }
    return SegmentsStringChars::fromTagged(slot.toPrivate());
  }

  void setStringChars(SegmentsStringChars chars) {
    setFixedSlot(STRING_CHARS_SLOT, PrivateValue(chars.tagged()));
  }

  bool hasLatin1StringChars() const {
    MOZ_ASSERT(hasStringChars());
    return getStringChars().has<JS::Latin1Char>();
  }

  int32_t getIndex() const {
    const auto& slot = getFixedSlot(INDEX_SLOT);
    if (slot.isUndefined()) {
      return 0;
    }
    return slot.toInt32();
  }

  void setIndex(int32_t index) { setFixedSlot(INDEX_SLOT, Int32Value(index)); }

  SegmenterGranularity getGranularity() const {
    const auto& slot = getFixedSlot(GRANULARITY_SLOT);
    if (slot.isUndefined()) {
      return SegmenterGranularity::Grapheme;
    }
    return static_cast<SegmenterGranularity>(slot.toInt32());
  }

  void setGranularity(SegmenterGranularity granularity) {
    setFixedSlot(GRANULARITY_SLOT,
                 Int32Value(static_cast<int32_t>(granularity)));
  }

  void* getBreakIterator() const {
    const auto& slot = getFixedSlot(BREAK_ITERATOR_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return slot.toPrivate();
  }

  void setBreakIterator(void* brk) {
    setFixedSlot(BREAK_ITERATOR_SLOT, PrivateValue(brk));
  }

 private:
  static const JSClassOps classOps_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

/**
 * Create a new Segments object.
 *
 * Usage: segment = intl_CreateSegmentsObject(segmenter, string)
 */
[[nodiscard]] extern bool intl_CreateSegmentsObject(JSContext* cx,
                                                    unsigned argc, Value* vp);

/**
 * Create a new Segment Iterator object.
 *
 * Usage: iterator = intl_CreateSegmentIterator(segments)
 */
[[nodiscard]] extern bool intl_CreateSegmentIterator(JSContext* cx,
                                                     unsigned argc, Value* vp);

/**
 * Find the next and the preceding segment boundaries for the given index. The
 * index must be a valid string index within the segmenter string.
 *
 * Return a three-element array object `[startIndex, endIndex, wordLike]`, where
 * `wordLike` is either a boolean or undefined for non-word segmenters.
 *
 * Usage: boundaries = intl_FindSegmentBoundaries(segments, index)
 */
[[nodiscard]] extern bool intl_FindSegmentBoundaries(JSContext* cx,
                                                     unsigned argc, Value* vp);

/**
 * Find the next segment boundaries starting from the current iterator index.
 * The iterator mustn't have been completed.
 *
 * Return a three-element array object `[startIndex, endIndex, wordLike]`, where
 * `wordLike` is either a boolean or undefined for non-word segmenters.
 *
 * Usage: boundaries = intl_FindNextSegmentBoundaries(iterator)
 */
[[nodiscard]] extern bool intl_FindNextSegmentBoundaries(JSContext* cx,
                                                         unsigned argc,
                                                         Value* vp);

}  // namespace js

#endif /* builtin_intl_Segmenter_h */
