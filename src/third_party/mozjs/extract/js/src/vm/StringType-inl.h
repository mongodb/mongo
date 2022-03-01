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
#include "gc/Marking.h"
#include "gc/MaybeRooted.h"
#include "gc/StoreBuffer.h"
#include "js/UniquePtr.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"

#include "gc/FreeOp-inl.h"
#include "gc/StoreBuffer-inl.h"

namespace js {

// Allocate a thin inline string if possible, and a fat inline string if not.
template <AllowGC allowGC, typename CharT>
static MOZ_ALWAYS_INLINE JSInlineString* AllocateInlineString(
    JSContext* cx, size_t len, CharT** chars, js::gc::InitialHeap heap) {
  MOZ_ASSERT(JSInlineString::lengthFits<CharT>(len));

  if (JSThinInlineString::lengthFits<CharT>(len)) {
    JSThinInlineString* str = JSThinInlineString::new_<allowGC>(cx, heap);
    if (!str) {
      return nullptr;
    }
    *chars = str->init<CharT>(len);
    return str;
  }

  JSFatInlineString* str = JSFatInlineString::new_<allowGC>(cx, heap);
  if (!str) {
    return nullptr;
  }
  *chars = str->init<CharT>(len);
  return str;
}

// Create a thin inline string if possible, and a fat inline string if not.
template <AllowGC allowGC, typename CharT>
static MOZ_ALWAYS_INLINE JSInlineString* NewInlineString(
    JSContext* cx, mozilla::Range<const CharT> chars,
    js::gc::InitialHeap heap = js::gc::DefaultHeap) {
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

// Create a thin inline string if possible, and a fat inline string if not.
template <typename CharT>
static MOZ_ALWAYS_INLINE JSInlineString* NewInlineString(
    JSContext* cx, HandleLinearString base, size_t start, size_t length,
    js::gc::InitialHeap heap) {
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

template <typename Chars>
static MOZ_ALWAYS_INLINE JSLinearString* TryEmptyOrStaticString(JSContext* cx,
                                                                Chars chars,
                                                                size_t n) {
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

template <typename CharT, typename = std::enable_if_t<!std::is_const_v<CharT>>>
static MOZ_ALWAYS_INLINE JSLinearString* TryEmptyOrStaticString(JSContext* cx,
                                                                CharT* chars,
                                                                size_t n) {
  return TryEmptyOrStaticString(cx, const_cast<const CharT*>(chars), n);
}

} /* namespace js */

MOZ_ALWAYS_INLINE bool JSString::validateLength(JSContext* maybecx,
                                                size_t length) {
  if (MOZ_UNLIKELY(length > JSString::MAX_LENGTH)) {
    js::ReportAllocationOverflow(maybecx);
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

MOZ_ALWAYS_INLINE void JSRope::init(JSContext* cx, JSString* left,
                                    JSString* right, size_t length) {
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
    size_t length, js::gc::InitialHeap heap) {
  if (!validateLength(cx, length)) {
    return nullptr;
  }
  JSRope* str = js::AllocateString<JSRope, allowGC>(cx, heap);
  if (!str) {
    return nullptr;
  }
  str->init(cx, left, right, length);
  return str;
}

MOZ_ALWAYS_INLINE void JSDependentString::init(JSContext* cx,
                                               JSLinearString* base,
                                               size_t start, size_t length) {
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
    js::gc::InitialHeap heap) {
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
    js::RootedLinearString base(cx, baseArg);
    return baseArg->hasLatin1Chars()
               ? js::NewInlineString<JS::Latin1Char>(cx, base, start, length,
                                                     heap)
               : js::NewInlineString<char16_t>(cx, base, start, length, heap);
  }

  JSDependentString* str =
      js::AllocateString<JSDependentString, js::NoGC>(cx, heap);
  if (str) {
    str->init(cx, baseArg, start, length);
    return str;
  }

  js::RootedLinearString base(cx, baseArg);

  str = js::AllocateString<JSDependentString>(cx, heap);
  if (!str) {
    return nullptr;
  }
  str->init(cx, base, start, length);
  return str;
}

MOZ_ALWAYS_INLINE void JSLinearString::init(const char16_t* chars,
                                            size_t length) {
  setLengthAndFlags(length, INIT_LINEAR_FLAGS);
  // Check that the new buffer is located in the StringBufferArena
  checkStringCharsArena(chars);
  d.s.u2.nonInlineCharsTwoByte = chars;
}

MOZ_ALWAYS_INLINE void JSLinearString::init(const JS::Latin1Char* chars,
                                            size_t length) {
  setLengthAndFlags(length, INIT_LINEAR_FLAGS | LATIN1_CHARS_BIT);
  // Check that the new buffer is located in the StringBufferArena
  checkStringCharsArena(chars);
  d.s.u2.nonInlineCharsLatin1 = chars;
}

template <js::AllowGC allowGC, typename CharT>
MOZ_ALWAYS_INLINE JSLinearString* JSLinearString::new_(
    JSContext* cx, js::UniquePtr<CharT[], JS::FreePolicy> chars, size_t length,
    js::gc::InitialHeap heap) {
  if (!validateLength(cx, length)) {
    return nullptr;
  }

  JSLinearString* str;
  if (cx->zone()->isAtomsZone()) {
    str = js::Allocate<js::NormalAtom, allowGC>(cx);
  } else {
    str = js::AllocateString<JSLinearString, allowGC>(cx, heap);
  }
  if (!str) {
    return nullptr;
  }

  if (!str->isTenured()) {
    // If the following registration fails, the string is partially initialized
    // and must be made valid, or its finalizer may attempt to free
    // uninitialized memory.
    if (!cx->runtime()->gc.nursery().registerMallocedBuffer(
            chars.get(), length * sizeof(CharT))) {
      str->init(static_cast<JS::Latin1Char*>(nullptr), 0);
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

  str->init(chars.release(), length);
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
    JSContext* cx, js::gc::InitialHeap heap) {
  if (cx->zone()->isAtomsZone()) {
    return (JSThinInlineString*)(js::Allocate<js::NormalAtom, allowGC>(cx));
  }

  return js::AllocateString<JSThinInlineString, allowGC>(cx, heap);
}

template <js::AllowGC allowGC>
MOZ_ALWAYS_INLINE JSFatInlineString* JSFatInlineString::new_(
    JSContext* cx, js::gc::InitialHeap heap) {
  if (cx->zone()->isAtomsZone()) {
    return (JSFatInlineString*)(js::Allocate<js::FatInlineAtom, allowGC>(cx));
  }

  return js::AllocateString<JSFatInlineString, allowGC>(cx, heap);
}

template <>
MOZ_ALWAYS_INLINE JS::Latin1Char* JSThinInlineString::init<JS::Latin1Char>(
    size_t length) {
  MOZ_ASSERT(lengthFits<JS::Latin1Char>(length));
  setLengthAndFlags(length, INIT_THIN_INLINE_FLAGS | LATIN1_CHARS_BIT);
  return d.inlineStorageLatin1;
}

template <>
MOZ_ALWAYS_INLINE char16_t* JSThinInlineString::init<char16_t>(size_t length) {
  MOZ_ASSERT(lengthFits<char16_t>(length));
  setLengthAndFlags(length, INIT_THIN_INLINE_FLAGS);
  return d.inlineStorageTwoByte;
}

template <>
MOZ_ALWAYS_INLINE JS::Latin1Char* JSFatInlineString::init<JS::Latin1Char>(
    size_t length) {
  MOZ_ASSERT(lengthFits<JS::Latin1Char>(length));
  setLengthAndFlags(length, INIT_FAT_INLINE_FLAGS | LATIN1_CHARS_BIT);
  return d.inlineStorageLatin1;
}

template <>
MOZ_ALWAYS_INLINE char16_t* JSFatInlineString::init<char16_t>(size_t length) {
  MOZ_ASSERT(lengthFits<char16_t>(length));
  setLengthAndFlags(length, INIT_FAT_INLINE_FLAGS);
  return d.inlineStorageTwoByte;
}

MOZ_ALWAYS_INLINE void JSExternalString::init(
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
  if (!validateLength(cx, length)) {
    return nullptr;
  }
  JSExternalString* str = js::Allocate<JSExternalString>(cx);
  if (!str) {
    return nullptr;
  }
  str->init(chars, length, callbacks);
  size_t nbytes = length * sizeof(char16_t);

  MOZ_ASSERT(str->isTenured());
  js::AddCellMemory(str, nbytes, js::MemoryUse::StringContents);

  return str;
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
                                    js::gc::DefaultHeap);
}

MOZ_ALWAYS_INLINE void JSString::finalize(JSFreeOp* fop) {
  /* FatInline strings are in a different arena. */
  MOZ_ASSERT(getAllocKind() != js::gc::AllocKind::FAT_INLINE_STRING);
  MOZ_ASSERT(getAllocKind() != js::gc::AllocKind::FAT_INLINE_ATOM);

  if (isLinear()) {
    asLinear().finalize(fop);
  } else {
    MOZ_ASSERT(isRope());
  }
}

inline void JSLinearString::finalize(JSFreeOp* fop) {
  MOZ_ASSERT(getAllocKind() != js::gc::AllocKind::FAT_INLINE_STRING);
  MOZ_ASSERT(getAllocKind() != js::gc::AllocKind::FAT_INLINE_ATOM);

  if (!isInline() && !isDependent()) {
    fop->free_(this, nonInlineCharsRaw(), allocSize(),
               js::MemoryUse::StringContents);
  }
}

inline void JSFatInlineString::finalize(JSFreeOp* fop) {
  MOZ_ASSERT(getAllocKind() == js::gc::AllocKind::FAT_INLINE_STRING);
  MOZ_ASSERT(isInline());

  // Nothing to do.
}

inline void js::FatInlineAtom::finalize(JSFreeOp* fop) {
  MOZ_ASSERT(JSString::isAtom());
  MOZ_ASSERT(getAllocKind() == js::gc::AllocKind::FAT_INLINE_ATOM);

  // Nothing to do.
}

inline void JSExternalString::finalize(JSFreeOp* fop) {
  MOZ_ASSERT(JSString::isExternal());

  size_t nbytes = length() * sizeof(char16_t);
  fop->removeCellMemory(this, nbytes, js::MemoryUse::StringContents);

  callbacks()->finalize(const_cast<char16_t*>(rawTwoByteChars()));
}

#endif /* vm_StringType_inl_h */
