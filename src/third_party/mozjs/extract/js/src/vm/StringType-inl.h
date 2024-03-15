/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_StringType_inl_h
#define vm_StringType_inl_h

#include "vm/StringType.h"

#include "mozilla/PodOperations.h"
#include "mozilla/Range.h"

#include "gc/Allocator.h"
#include "gc/MaybeRooted.h"
#include "gc/StoreBuffer.h"
#include "js/UniquePtr.h"
#include "vm/JSContext.h"
#include "vm/StaticStrings.h"

#include "gc/GCContext-inl.h"
#include "gc/StoreBuffer-inl.h"

namespace js {

// Allocate a thin inline string if possible, and a fat inline string if not.
template <AllowGC allowGC, typename CharT>
static MOZ_ALWAYS_INLINE JSInlineString* AllocateInlineString(
    JSContext* cx, size_t len, CharT** chars, js::gc::Heap heap) {
  MOZ_ASSERT(JSInlineString::lengthFits<CharT>(len));

  if (JSThinInlineString::lengthFits<CharT>(len)) {
    return cx->newCell<JSThinInlineString, allowGC>(heap, len, chars);
  }
  return cx->newCell<JSFatInlineString, allowGC>(heap, len, chars);
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* AllocateInlineAtom(JSContext* cx, size_t len,
                                                    CharT** chars,
                                                    js::HashNumber hash) {
  MOZ_ASSERT(JSInlineString::lengthFits<CharT>(len));

  if (JSThinInlineString::lengthFits<CharT>(len)) {
    return cx->newCell<js::NormalAtom, js::NoGC>(len, chars, hash);
  }
  return cx->newCell<js::FatInlineAtom, js::NoGC>(len, chars, hash);
}

// Create a thin inline string if possible, and a fat inline string if not.
template <AllowGC allowGC, typename CharT>
static MOZ_ALWAYS_INLINE JSInlineString* NewInlineString(
    JSContext* cx, mozilla::Range<const CharT> chars,
    js::gc::Heap heap = js::gc::Heap::Default) {
  /*
   * Don't bother trying to find a static atom; measurement shows that not
   * many get here (for one, Atomize is catching them).
   */

  size_t len = chars.length();
  CharT* storage;
  JSInlineString* str = AllocateInlineString<allowGC>(cx, len, &storage, heap);
  if (!str) {
    return nullptr;
  }

  mozilla::PodCopy(storage, chars.begin().get(), len);
  return str;
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* NewInlineAtom(JSContext* cx,
                                               const CharT* chars,
                                               size_t length,
                                               js::HashNumber hash) {
  CharT* storage;
  JSAtom* str = AllocateInlineAtom(cx, length, &storage, hash);
  if (!str) {
    return nullptr;
  }

  mozilla::PodCopy(storage, chars, length);
  return str;
}

// Create a thin inline string if possible, and a fat inline string if not.
template <typename CharT>
static MOZ_ALWAYS_INLINE JSInlineString* NewInlineString(
    JSContext* cx, Handle<JSLinearString*> base, size_t start, size_t length,
    js::gc::Heap heap) {
  MOZ_ASSERT(JSInlineString::lengthFits<CharT>(length));

  CharT* chars;
  JSInlineString* s = AllocateInlineString<CanGC>(cx, length, &chars, heap);
  if (!s) {
    return nullptr;
  }

  JS::AutoCheckCannotGC nogc;
  mozilla::PodCopy(chars, base->chars<CharT>(nogc) + start, length);
  return s;
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSLinearString* TryEmptyOrStaticString(
    JSContext* cx, const CharT* chars, size_t n) {
  // Measurements on popular websites indicate empty strings are pretty common
  // and most strings with length 1 or 2 are in the StaticStrings table. For
  // length 3 strings that's only about 1%, so we check n <= 2.
  if (n <= 2) {
    if (n == 0) {
      return cx->emptyString();
    }

    if (JSLinearString* str = cx->staticStrings().lookup(chars, n)) {
      return str;
    }
  }

  return nullptr;
}

} /* namespace js */

MOZ_ALWAYS_INLINE bool JSString::validateLength(JSContext* maybecx,
                                                size_t length) {
  return validateLengthInternal<js::CanGC>(maybecx, length);
}

template <js::AllowGC allowGC>
MOZ_ALWAYS_INLINE bool JSString::validateLengthInternal(JSContext* maybecx,
                                                        size_t length) {
  if (MOZ_UNLIKELY(length > JSString::MAX_LENGTH)) {
    if constexpr (allowGC) {
      js::ReportOversizedAllocation(maybecx, JSMSG_ALLOC_OVERFLOW);
    }
    return false;
  }

  return true;
}

template <>
MOZ_ALWAYS_INLINE const char16_t* JSString::nonInlineCharsRaw() const {
  return d.s.u2.nonInlineCharsTwoByte;
}

template <>
MOZ_ALWAYS_INLINE const JS::Latin1Char* JSString::nonInlineCharsRaw() const {
  return d.s.u2.nonInlineCharsLatin1;
}

inline JSRope::JSRope(JSString* left, JSString* right, size_t length) {
  // JITs expect rope children aren't empty.
  MOZ_ASSERT(!left->empty() && !right->empty());

  if (left->hasLatin1Chars() && right->hasLatin1Chars()) {
    setLengthAndFlags(length, INIT_ROPE_FLAGS | LATIN1_CHARS_BIT);
  } else {
    setLengthAndFlags(length, INIT_ROPE_FLAGS);
  }
  d.s.u2.left = left;
  d.s.u3.right = right;

  // Post-barrier by inserting into the whole cell buffer if either
  // this -> left or this -> right is a tenured -> nursery edge.
  if (isTenured()) {
    js::gc::StoreBuffer* sb = left->storeBuffer();
    if (!sb) {
      sb = right->storeBuffer();
    }
    if (sb) {
      sb->putWholeCell(this);
    }
  }
}

template <js::AllowGC allowGC>
MOZ_ALWAYS_INLINE JSRope* JSRope::new_(
    JSContext* cx,
    typename js::MaybeRooted<JSString*, allowGC>::HandleType left,
    typename js::MaybeRooted<JSString*, allowGC>::HandleType right,
    size_t length, js::gc::Heap heap) {
  if (MOZ_UNLIKELY(!validateLengthInternal<allowGC>(cx, length))) {
    return nullptr;
  }
  return cx->newCell<JSRope, allowGC>(heap, left, right, length);
}

inline JSDependentString::JSDependentString(JSLinearString* base, size_t start,
                                            size_t length) {
  MOZ_ASSERT(start + length <= base->length());
  JS::AutoCheckCannotGC nogc;
  if (base->hasLatin1Chars()) {
    setLengthAndFlags(length, INIT_DEPENDENT_FLAGS | LATIN1_CHARS_BIT);
    d.s.u2.nonInlineCharsLatin1 = base->latin1Chars(nogc) + start;
  } else {
    setLengthAndFlags(length, INIT_DEPENDENT_FLAGS);
    d.s.u2.nonInlineCharsTwoByte = base->twoByteChars(nogc) + start;
  }
  d.s.u3.base = base;
  if (isTenured() && !base->isTenured()) {
    base->storeBuffer()->putWholeCell(this);
  }
}

MOZ_ALWAYS_INLINE JSLinearString* JSDependentString::new_(
    JSContext* cx, JSLinearString* baseArg, size_t start, size_t length,
    js::gc::Heap heap) {
  /*
   * Try to avoid long chains of dependent strings. We can't avoid these
   * entirely, however, due to how ropes are flattened.
   */
  if (baseArg->isDependent()) {
    start += baseArg->asDependent().baseOffset();
    baseArg = baseArg->asDependent().base();
  }

  MOZ_ASSERT(start + length <= baseArg->length());

  /*
   * Do not create a string dependent on inline chars from another string,
   * both to avoid the awkward moving-GC hazard this introduces and because it
   * is more efficient to immediately undepend here.
   */
  bool useInline = baseArg->hasTwoByteChars()
                       ? JSInlineString::lengthFits<char16_t>(length)
                       : JSInlineString::lengthFits<JS::Latin1Char>(length);
  if (useInline) {
    JS::Rooted<JSLinearString*> base(cx, baseArg);
    return baseArg->hasLatin1Chars()
               ? js::NewInlineString<JS::Latin1Char>(cx, base, start, length,
                                                     heap)
               : js::NewInlineString<char16_t>(cx, base, start, length, heap);
  }

  JSDependentString* str =
      cx->newCell<JSDependentString, js::NoGC>(heap, baseArg, start, length);
  if (str) {
    return str;
  }

  JS::Rooted<JSLinearString*> base(cx, baseArg);
  return cx->newCell<JSDependentString>(heap, base, start, length);
}

inline JSLinearString::JSLinearString(const char16_t* chars, size_t length) {
  setLengthAndFlags(length, INIT_LINEAR_FLAGS);
  // Check that the new buffer is located in the StringBufferArena
  checkStringCharsArena(chars);
  d.s.u2.nonInlineCharsTwoByte = chars;
}

inline JSLinearString::JSLinearString(const JS::Latin1Char* chars,
                                      size_t length) {
  setLengthAndFlags(length, INIT_LINEAR_FLAGS | LATIN1_CHARS_BIT);
  // Check that the new buffer is located in the StringBufferArena
  checkStringCharsArena(chars);
  d.s.u2.nonInlineCharsLatin1 = chars;
}

void JSLinearString::disownCharsBecauseError() {
  setLengthAndFlags(0, INIT_LINEAR_FLAGS | LATIN1_CHARS_BIT);
  d.s.u2.nonInlineCharsLatin1 = nullptr;
}

template <js::AllowGC allowGC, typename CharT>
MOZ_ALWAYS_INLINE JSLinearString* JSLinearString::new_(
    JSContext* cx, js::UniquePtr<CharT[], JS::FreePolicy> chars, size_t length,
    js::gc::Heap heap) {
  if (MOZ_UNLIKELY(!validateLengthInternal<allowGC>(cx, length))) {
    return nullptr;
  }

  return newValidLength<allowGC>(cx, std::move(chars), length, heap);
}

template <js::AllowGC allowGC, typename CharT>
MOZ_ALWAYS_INLINE JSLinearString* JSLinearString::newValidLength(
    JSContext* cx, js::UniquePtr<CharT[], JS::FreePolicy> chars, size_t length,
    js::gc::Heap heap) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  JSLinearString* str =
      cx->newCell<JSLinearString, allowGC>(heap, chars.get(), length);
  if (!str) {
    return nullptr;
  }

  if (!str->isTenured()) {
    // If the following registration fails, the string is partially initialized
    // and must be made valid, or its finalizer may attempt to free
    // uninitialized memory.
    if (!cx->runtime()->gc.nursery().registerMallocedBuffer(
            chars.get(), length * sizeof(CharT))) {
      str->disownCharsBecauseError();
      if (allowGC) {
        ReportOutOfMemory(cx);
      }
      return nullptr;
    }
  } else {
    // This can happen off the main thread for the atoms zone.
    cx->zone()->addCellMemory(str, length * sizeof(CharT),
                              js::MemoryUse::StringContents);
  }

  (void)chars.release();
  return str;
}

template <typename CharT>
MOZ_ALWAYS_INLINE JSAtom* JSAtom::newValidLength(
    JSContext* cx, js::UniquePtr<CharT[], JS::FreePolicy> chars, size_t length,
    js::HashNumber hash) {
  MOZ_ASSERT(validateLength(cx, length));
  MOZ_ASSERT(cx->zone()->isAtomsZone());
  JSAtom* str =
      cx->newCell<js::NormalAtom, js::NoGC>(chars.get(), length, hash);
  if (!str) {
    return nullptr;
  }
  (void)chars.release();

  MOZ_ASSERT(str->isTenured());
  cx->zone()->addCellMemory(str, length * sizeof(CharT),
                            js::MemoryUse::StringContents);

  return str;
}

inline js::PropertyName* JSLinearString::toPropertyName(JSContext* cx) {
#ifdef DEBUG
  uint32_t dummy;
  MOZ_ASSERT(!isIndex(&dummy));
#endif
  if (isAtom()) {
    return asAtom().asPropertyName();
  }
  JSAtom* atom = js::AtomizeString(cx, this);
  if (!atom) {
    return nullptr;
  }
  return atom->asPropertyName();
}

template <js::AllowGC allowGC>
MOZ_ALWAYS_INLINE JSThinInlineString* JSThinInlineString::new_(
    JSContext* cx, js::gc::Heap heap) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  return cx->newCell<JSThinInlineString, allowGC>(heap);
}

template <js::AllowGC allowGC>
MOZ_ALWAYS_INLINE JSFatInlineString* JSFatInlineString::new_(
    JSContext* cx, js::gc::Heap heap) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  return cx->newCell<JSFatInlineString, allowGC>(heap);
}

inline JSThinInlineString::JSThinInlineString(size_t length,
                                              JS::Latin1Char** chars) {
  MOZ_ASSERT(lengthFits<JS::Latin1Char>(length));
  setLengthAndFlags(length, INIT_THIN_INLINE_FLAGS | LATIN1_CHARS_BIT);
  *chars = d.inlineStorageLatin1;
}

inline JSThinInlineString::JSThinInlineString(size_t length, char16_t** chars) {
  MOZ_ASSERT(lengthFits<char16_t>(length));
  setLengthAndFlags(length, INIT_THIN_INLINE_FLAGS);
  *chars = d.inlineStorageTwoByte;
}

inline JSFatInlineString::JSFatInlineString(size_t length,
                                            JS::Latin1Char** chars) {
  MOZ_ASSERT(lengthFits<JS::Latin1Char>(length));
  setLengthAndFlags(length, INIT_FAT_INLINE_FLAGS | LATIN1_CHARS_BIT);
  *chars = d.inlineStorageLatin1;
}

inline JSFatInlineString::JSFatInlineString(size_t length, char16_t** chars) {
  MOZ_ASSERT(lengthFits<char16_t>(length));
  setLengthAndFlags(length, INIT_FAT_INLINE_FLAGS);
  *chars = d.inlineStorageTwoByte;
}

inline JSExternalString::JSExternalString(
    const char16_t* chars, size_t length,
    const JSExternalStringCallbacks* callbacks) {
  MOZ_ASSERT(callbacks);
  setLengthAndFlags(length, EXTERNAL_FLAGS);
  d.s.u2.nonInlineCharsTwoByte = chars;
  d.s.u3.externalCallbacks = callbacks;
}

MOZ_ALWAYS_INLINE JSExternalString* JSExternalString::new_(
    JSContext* cx, const char16_t* chars, size_t length,
    const JSExternalStringCallbacks* callbacks) {
  if (MOZ_UNLIKELY(!validateLength(cx, length))) {
    return nullptr;
  }
  auto* str = cx->newCell<JSExternalString>(chars, length, callbacks);
  if (!str) {
    return nullptr;
  }
  size_t nbytes = length * sizeof(char16_t);

  MOZ_ASSERT(str->isTenured());
  js::AddCellMemory(str, nbytes, js::MemoryUse::StringContents);

  return str;
}

inline js::NormalAtom::NormalAtom(size_t length, JS::Latin1Char** chars,
                                  js::HashNumber hash)
    : hash_(hash) {
  MOZ_ASSERT(JSInlineString::lengthFits<JS::Latin1Char>(length));
  setLengthAndFlags(length,
                    INIT_THIN_INLINE_FLAGS | LATIN1_CHARS_BIT | ATOM_BIT);
  *chars = d.inlineStorageLatin1;
}

inline js::NormalAtom::NormalAtom(size_t length, char16_t** chars,
                                  js::HashNumber hash)
    : hash_(hash) {
  MOZ_ASSERT(JSInlineString::lengthFits<char16_t>(length));
  setLengthAndFlags(length, INIT_THIN_INLINE_FLAGS | ATOM_BIT);
  *chars = d.inlineStorageTwoByte;
}

inline js::NormalAtom::NormalAtom(const char16_t* chars, size_t length,
                                  js::HashNumber hash)
    : hash_(hash) {
  setLengthAndFlags(length, INIT_LINEAR_FLAGS | ATOM_BIT);
  // Check that the new buffer is located in the StringBufferArena
  checkStringCharsArena(chars);
  d.s.u2.nonInlineCharsTwoByte = chars;
}

inline js::NormalAtom::NormalAtom(const JS::Latin1Char* chars, size_t length,
                                  js::HashNumber hash)
    : hash_(hash) {
  setLengthAndFlags(length, INIT_LINEAR_FLAGS | LATIN1_CHARS_BIT | ATOM_BIT);
  // Check that the new buffer is located in the StringBufferArena
  checkStringCharsArena(chars);
  d.s.u2.nonInlineCharsLatin1 = chars;
}

inline js::FatInlineAtom::FatInlineAtom(size_t length, JS::Latin1Char** chars,
                                        js::HashNumber hash)
    : hash_(hash) {
  MOZ_ASSERT(JSFatInlineString::lengthFits<JS::Latin1Char>(length));
  setLengthAndFlags(length,
                    INIT_FAT_INLINE_FLAGS | LATIN1_CHARS_BIT | ATOM_BIT);
  *chars = d.inlineStorageLatin1;
}

inline js::FatInlineAtom::FatInlineAtom(size_t length, char16_t** chars,
                                        js::HashNumber hash)
    : hash_(hash) {
  MOZ_ASSERT(JSFatInlineString::lengthFits<char16_t>(length));
  setLengthAndFlags(length, INIT_FAT_INLINE_FLAGS | ATOM_BIT);
  *chars = d.inlineStorageTwoByte;
}

inline JSLinearString* js::StaticStrings::getUnitStringForElement(
    JSContext* cx, JSString* str, size_t index) {
  MOZ_ASSERT(index < str->length());

  char16_t c;
  if (!str->getChar(cx, index, &c)) {
    return nullptr;
  }
  if (c < UNIT_STATIC_LIMIT) {
    return getUnit(c);
  }
  return js::NewInlineString<CanGC>(cx, mozilla::Range<const char16_t>(&c, 1),
                                    js::gc::Heap::Default);
}

MOZ_ALWAYS_INLINE void JSString::finalize(JS::GCContext* gcx) {
  /* FatInline strings are in a different arena. */
  MOZ_ASSERT(getAllocKind() != js::gc::AllocKind::FAT_INLINE_STRING);
  MOZ_ASSERT(getAllocKind() != js::gc::AllocKind::FAT_INLINE_ATOM);

  if (isLinear()) {
    asLinear().finalize(gcx);
  } else {
    MOZ_ASSERT(isRope());
  }
}

inline void JSLinearString::finalize(JS::GCContext* gcx) {
  MOZ_ASSERT(getAllocKind() != js::gc::AllocKind::FAT_INLINE_STRING);
  MOZ_ASSERT(getAllocKind() != js::gc::AllocKind::FAT_INLINE_ATOM);

  if (!isInline() && !isDependent()) {
    gcx->free_(this, nonInlineCharsRaw(), allocSize(),
               js::MemoryUse::StringContents);
  }
}

inline void JSFatInlineString::finalize(JS::GCContext* gcx) {
  MOZ_ASSERT(getAllocKind() == js::gc::AllocKind::FAT_INLINE_STRING);
  MOZ_ASSERT(isInline());

  // Nothing to do.
}

inline void js::FatInlineAtom::finalize(JS::GCContext* gcx) {
  MOZ_ASSERT(JSString::isAtom());
  MOZ_ASSERT(getAllocKind() == js::gc::AllocKind::FAT_INLINE_ATOM);

  // Nothing to do.
}

inline void JSExternalString::finalize(JS::GCContext* gcx) {
  MOZ_ASSERT(JSString::isExternal());

  size_t nbytes = length() * sizeof(char16_t);
  gcx->removeCellMemory(this, nbytes, js::MemoryUse::StringContents);

  callbacks()->finalize(const_cast<char16_t*>(rawTwoByteChars()));
}

#endif /* vm_StringType_inl_h */
