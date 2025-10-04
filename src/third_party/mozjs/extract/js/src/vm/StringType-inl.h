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

#include "gc/GCEnum.h"
#include "gc/MaybeRooted.h"
#include "gc/StoreBuffer.h"
#include "js/UniquePtr.h"
#include "vm/StaticStrings.h"

#include "gc/GCContext-inl.h"
#include "gc/Marking-inl.h"
#include "gc/StoreBuffer-inl.h"
#include "vm/JSContext-inl.h"

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
  MOZ_ASSERT(JSAtom::lengthFitsInline<CharT>(len));
  if constexpr (js::ThinInlineAtom::EverInstantiated) {
    if (js::ThinInlineAtom::lengthFits<CharT>(len)) {
      return cx->newCell<js::ThinInlineAtom, js::NoGC>(len, chars, hash);
    }
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

// Create a thin inline string if possible, and a fat inline string if not.
template <AllowGC allowGC, typename CharT, size_t N>
static MOZ_ALWAYS_INLINE JSInlineString* NewInlineString(
    JSContext* cx, const CharT (&chars)[N], size_t len,
    js::gc::Heap heap = js::gc::Heap::Default) {
  MOZ_ASSERT(len <= N);

  /*
   * Don't bother trying to find a static atom; measurement shows that not
   * many get here (for one, Atomize is catching them).
   */

  CharT* storage;
  JSInlineString* str = AllocateInlineString<allowGC>(cx, len, &storage, heap);
  if (!str) {
    return nullptr;
  }

  if (JSThinInlineString::lengthFits<CharT>(len)) {
    constexpr size_t MaxLength = std::is_same_v<CharT, Latin1Char>
                                     ? JSThinInlineString::MAX_LENGTH_LATIN1
                                     : JSThinInlineString::MAX_LENGTH_TWO_BYTE;

    // memcpy with a constant length can be optimized more easily by compilers.
    constexpr size_t toCopy = std::min(N, MaxLength) * sizeof(CharT);
    std::memcpy(storage, chars, toCopy);
  } else {
    constexpr size_t MaxLength = std::is_same_v<CharT, Latin1Char>
                                     ? JSFatInlineString::MAX_LENGTH_LATIN1
                                     : JSFatInlineString::MAX_LENGTH_TWO_BYTE;

    // memcpy with a constant length can be optimized more easily by compilers.
    constexpr size_t toCopy = std::min(N, MaxLength) * sizeof(CharT);
    std::memcpy(storage, chars, toCopy);
  }
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

template <typename CharT>
JSString::OwnedChars<CharT>::OwnedChars(CharT* chars, size_t length,
                                        bool isMalloced, bool needsFree)
    : needsFree_(chars && needsFree), isMalloced_(chars && isMalloced) {
  // Set needsFree_ and isMalloced_ to false if chars is nullptr because Span
  // silently turns nullptrs into small bogus integer values for Rust
  // compatibility. This prevents passing bogus pointers into free() or
  // registerMallocedBuffer().
  MOZ_ASSERT_IF(length == 0,
                chars == nullptr);  // Disallow zero-length strings.
  MOZ_ASSERT_IF(needsFree_, isMalloced_);
  if (chars) {
    MOZ_ASSERT(isMalloced_ == !js::TlsContext.get()->nursery().isInside(chars));
  } else {
    MOZ_ASSERT(!isMalloced_);
    MOZ_ASSERT(!needsFree_);
  }

  chars_ = mozilla::Span<CharT>(chars, chars ? length : 0);
}

template <typename CharT>
JSString::OwnedChars<CharT>::OwnedChars(JSString::OwnedChars<CharT>&& other)
    : OwnedChars(other.chars_.Length() ? other.chars_.data() : nullptr,
                 other.chars_.Length(), other.isMalloced_, other.needsFree_) {
  // Span returns an invalid but nonzero pointer when constructed with
  // nullptr, so test the length and normalize to nullptr, above. That means
  // this class cannot store a zero-length non-null pointer. Assert in the
  // CharT* constructor if anything tries to.

  // Do not release until now so that other.needsFree_ is valid during
  // construction.
  other.release();
}

template <typename CharT>
CharT* JSString::OwnedChars<CharT>::release() {
  needsFree_ = false;
  return chars_.data();
}

template <typename CharT>
void JSString::OwnedChars<CharT>::reset() {
  if (needsFree_) {
    js_free(chars_.data());
    needsFree_ = false;
  }
}

template <typename CharT>
void JSString::OwnedChars<CharT>::ensureNonNursery() {
  if (isMalloced() || !data()) {
    return;
  }

  js::AutoEnterOOMUnsafeRegion oomUnsafe;
  CharT* oldPtr = data();
  size_t length = chars_.Length();
  CharT* ptr = js_pod_arena_malloc<CharT>(js::StringBufferArena, length);
  if (!ptr) {
    oomUnsafe.crash(chars_.size(), "moving nursery buffer to heap");
  }
  mozilla::PodCopy(ptr, oldPtr, length);
  chars_ = mozilla::Span<CharT>(ptr, length);
  isMalloced_ = needsFree_ = true;
}

template <typename CharT>
JSString::OwnedChars<CharT>::OwnedChars(
    js::UniquePtr<CharT[], JS::FreePolicy>&& chars, size_t length,
    bool isMalloced)
    : OwnedChars(chars.release(), length, isMalloced, true) {}

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

bool JSString::ownsMallocedChars() const {
  if (!hasOutOfLineChars()) {
    return false;
  }

  js::gc::StoreBuffer* sb = storeBuffer();
  if (!sb) {
    // Tenured strings always own out-of-line chars.
    return true;
  }

  // Return whether the chars are malloced. Note: this allows the data to be in
  // a different nursery chunk than the Cell itself, at the performance cost of
  // iterating over all chunks.
  return !sb->nursery().isInside(asLinear().nonInlineCharsRaw());
}

template <typename CharT>
inline size_t JSLinearString::maybeMallocCharsOnPromotion(
    js::Nursery* nursery) {
  const void** chars;
  if constexpr (std::is_same_v<CharT, char16_t>) {
    chars = reinterpret_cast<const void**>(&d.s.u2.nonInlineCharsTwoByte);
  } else {
    chars = reinterpret_cast<const void**>(&d.s.u2.nonInlineCharsLatin1);
  }

  size_t nbytes = length() * sizeof(CharT);
  if (nursery->maybeMoveBufferOnPromotion(const_cast<void**>(chars), this,
                                          nbytes, js::MemoryUse::StringContents,
                                          js::StringBufferArena) ==
      js::Nursery::BufferMoved) {
    return nbytes;
  }

  return 0;
}

inline size_t JSLinearString::allocSize() const {
  MOZ_ASSERT(ownsMallocedChars());

  size_t charSize =
      hasLatin1Chars() ? sizeof(JS::Latin1Char) : sizeof(char16_t);
  size_t count = isExtensible() ? asExtensible().capacity() : length();
  return count * charSize;
}

inline size_t JSString::allocSize() const {
  return ownsMallocedChars() ? asLinear().allocSize() : 0;
}

inline JSRope::JSRope(JSString* left, JSString* right, size_t length) {
  // JITs expect rope children aren't empty.
  MOZ_ASSERT(!left->empty() && !right->empty());

  // |length| must be the sum of the length of both child nodes.
  MOZ_ASSERT(left->length() + right->length() == length);

  // |isLatin1| is set when both children are guaranteed to contain only Latin-1
  // characters. Note that flattening either rope child can clear the Latin-1
  // flag of that child, so it's possible that a Latin-1 rope can end up with
  // both children being two-byte (dependent) strings.
  bool isLatin1 = left->hasLatin1Chars() && right->hasLatin1Chars();

  // Do not try to make a rope that could fit inline.
  MOZ_ASSERT_IF(!isLatin1, !JSInlineString::lengthFits<char16_t>(length));
  MOZ_ASSERT_IF(isLatin1, !JSInlineString::lengthFits<JS::Latin1Char>(length));

  if (isLatin1) {
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
  base->setDependedOn();
  d.s.u3.base = base;
  if (isTenured() && !base->isTenured()) {
    base->storeBuffer()->putWholeCell(this);
  }
}

MOZ_ALWAYS_INLINE JSLinearString* JSDependentString::new_(
    JSContext* cx, JSLinearString* baseArg, size_t start, size_t length,
    js::gc::Heap heap) {
  // Do not try to make a dependent string that could fit inline.
  MOZ_ASSERT_IF(baseArg->hasTwoByteChars(),
                !JSInlineString::lengthFits<char16_t>(length));
  MOZ_ASSERT_IF(!baseArg->hasTwoByteChars(),
                !JSInlineString::lengthFits<JS::Latin1Char>(length));

  /*
   * Try to avoid long chains of dependent strings. We can't avoid these
   * entirely, however, due to how ropes are flattened.
   */
  if (baseArg->isDependent()) {
    start += baseArg->asDependent().baseOffset();
    baseArg = baseArg->asDependent().base();
  }

  MOZ_ASSERT(start + length <= baseArg->length());

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

template <typename CharT>
inline JSLinearString::JSLinearString(
    JS::MutableHandle<JSString::OwnedChars<CharT>> chars) {
  // Note that it is possible that the chars may have been moved from the
  // nursery to the malloc heap when allocating the Cell that this constructor
  // is initializing.
  MOZ_ASSERT(chars.data());
  checkStringCharsArena(chars.data());
  if (isTenured()) {
    chars.ensureNonNursery();
  }
  if constexpr (std::is_same_v<CharT, char16_t>) {
    setLengthAndFlags(chars.length(), INIT_LINEAR_FLAGS);
    d.s.u2.nonInlineCharsTwoByte = chars.data();
  } else {
    setLengthAndFlags(chars.length(), INIT_LINEAR_FLAGS | LATIN1_CHARS_BIT);
    d.s.u2.nonInlineCharsLatin1 = chars.data();
  }
}

void JSLinearString::disownCharsBecauseError() {
  setLengthAndFlags(0, INIT_LINEAR_FLAGS | LATIN1_CHARS_BIT);
  d.s.u2.nonInlineCharsLatin1 = nullptr;
}

inline JSLinearString* JSDependentString::rootBaseDuringMinorGC() {
  JSLinearString* root = this;
  while (MaybeForwarded(root)->hasBase()) {
    if (root->isForwarded()) {
      root = js::gc::StringRelocationOverlay::fromCell(root)
                 ->savedNurseryBaseOrRelocOverlay();
    } else {
      // Possibly nursery or tenured string (not an overlay).
      root = root->nurseryBaseOrRelocOverlay();
    }
  }
  return root;
}

template <js::AllowGC allowGC, typename CharT>
MOZ_ALWAYS_INLINE JSLinearString* JSLinearString::new_(
    JSContext* cx, JS::MutableHandle<JSString::OwnedChars<CharT>> chars,
    js::gc::Heap heap) {
  if (MOZ_UNLIKELY(!validateLengthInternal<allowGC>(cx, chars.length()))) {
    return nullptr;
  }

  return newValidLength<allowGC>(cx, chars, heap);
}

template <js::AllowGC allowGC, typename CharT>
MOZ_ALWAYS_INLINE JSLinearString* JSLinearString::newValidLength(
    JSContext* cx, JS::MutableHandle<JSString::OwnedChars<CharT>> chars,
    js::gc::Heap heap) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  MOZ_ASSERT(!JSInlineString::lengthFits<CharT>(chars.length()));
  JSLinearString* str = cx->newCell<JSLinearString, allowGC>(heap, chars);
  if (!str) {
    return nullptr;
  }

  if (!str->isTenured()) {
    // If the following registration fails, the string is partially initialized
    // and must be made valid, or its finalizer may attempt to free
    // uninitialized memory.
    if (chars.isMalloced() &&
        !cx->nursery().registerMallocedBuffer(chars.data(), chars.size())) {
      str->disownCharsBecauseError();
      if (allowGC) {
        ReportOutOfMemory(cx);
      }
      return nullptr;
    }
  } else {
    cx->zone()->addCellMemory(str, chars.size(), js::MemoryUse::StringContents);
  }

  // Either the tenured Cell or the nursery's registry owns the chars now.
  chars.release();

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

// String characters are movable in the following cases:
//
// 1. Inline nursery strings (moved during promotion)
// 2. Nursery strings with nursery chars (moved during promotion)
// 3. Nursery strings that are deduplicated (moved during promotion)
// 4. Inline tenured strings (moved during compaction)
//
// This method does not consider #3, because if this method returns true and the
// caller does not want the characters to move, it can fix them in place by
// setting the nondeduplicatable bit. (If the bit were already taken into
// consideration, then the caller wouldn't know whether the movability is
// "fixable" or not. If it is *only* movable because of the lack of the bit
// being set, then it is fixable by setting the bit.)
bool JSLinearString::hasMovableChars() const {
  const JSLinearString* topBase = this;
  while (topBase->hasBase()) {
    topBase = topBase->base();
  }
  if (topBase->isInline()) {
    return true;
  }
  if (topBase->isTenured()) {
    return false;
  }
  return topBase->storeBuffer()->nursery().isInside(
      topBase->nonInlineCharsRaw());
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

inline JSExternalString::JSExternalString(
    const JS::Latin1Char* chars, size_t length,
    const JSExternalStringCallbacks* callbacks) {
  MOZ_ASSERT(callbacks);
  setLengthAndFlags(length, EXTERNAL_FLAGS | LATIN1_CHARS_BIT);
  d.s.u2.nonInlineCharsLatin1 = chars;
  d.s.u3.externalCallbacks = callbacks;
}

template <typename CharT>
/* static */
MOZ_ALWAYS_INLINE JSExternalString* JSExternalString::newImpl(
    JSContext* cx, const CharT* chars, size_t length,
    const JSExternalStringCallbacks* callbacks) {
  if (MOZ_UNLIKELY(!validateLength(cx, length))) {
    return nullptr;
  }
  auto* str = cx->newCell<JSExternalString>(chars, length, callbacks);

  if (!str) {
    return nullptr;
  }
  size_t nbytes = length * sizeof(CharT);

  MOZ_ASSERT(str->isTenured());
  js::AddCellMemory(str, nbytes, js::MemoryUse::StringContents);

  return str;
}

/* static */
MOZ_ALWAYS_INLINE JSExternalString* JSExternalString::new_(
    JSContext* cx, const JS::Latin1Char* chars, size_t length,
    const JSExternalStringCallbacks* callbacks) {
  return newImpl(cx, chars, length, callbacks);
}

/* static */
MOZ_ALWAYS_INLINE JSExternalString* JSExternalString::new_(
    JSContext* cx, const char16_t* chars, size_t length,
    const JSExternalStringCallbacks* callbacks) {
  return newImpl(cx, chars, length, callbacks);
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

#ifndef JS_64BIT
inline js::ThinInlineAtom::ThinInlineAtom(size_t length, JS::Latin1Char** chars,
                                          js::HashNumber hash)
    : NormalAtom(hash) {
  setLengthAndFlags(length,
                    INIT_THIN_INLINE_FLAGS | LATIN1_CHARS_BIT | ATOM_BIT);
  *chars = d.inlineStorageLatin1;
}

inline js::ThinInlineAtom::ThinInlineAtom(size_t length, char16_t** chars,
                                          js::HashNumber hash)
    : NormalAtom(hash) {
  setLengthAndFlags(length, INIT_THIN_INLINE_FLAGS | ATOM_BIT);
  *chars = d.inlineStorageTwoByte;
}
#endif

inline js::FatInlineAtom::FatInlineAtom(size_t length, JS::Latin1Char** chars,
                                        js::HashNumber hash)
    : hash_(hash) {
  MOZ_ASSERT(lengthFits<JS::Latin1Char>(length));
  setLengthAndFlags(length,
                    INIT_FAT_INLINE_FLAGS | LATIN1_CHARS_BIT | ATOM_BIT);
  *chars = d.inlineStorageLatin1;
}

inline js::FatInlineAtom::FatInlineAtom(size_t length, char16_t** chars,
                                        js::HashNumber hash)
    : hash_(hash) {
  MOZ_ASSERT(lengthFits<char16_t>(length));
  setLengthAndFlags(length, INIT_FAT_INLINE_FLAGS | ATOM_BIT);
  *chars = d.inlineStorageTwoByte;
}

inline JSLinearString* js::StaticStrings::getUnitString(JSContext* cx,
                                                        char16_t c) {
  if (c < UNIT_STATIC_LIMIT) {
    return getUnit(c);
  }
  return js::NewInlineString<CanGC>(cx, {c}, 1);
}

inline JSLinearString* js::StaticStrings::getUnitStringForElement(
    JSContext* cx, JSString* str, size_t index) {
  MOZ_ASSERT(index < str->length());

  char16_t c;
  if (!str->getChar(cx, index, &c)) {
    return nullptr;
  }
  return getUnitString(cx, c);
}

inline JSLinearString* js::StaticStrings::getUnitStringForElement(
    JSContext* cx, JSLinearString* str, size_t index) {
  MOZ_ASSERT(index < str->length());

  char16_t c = str->latin1OrTwoByteChar(index);
  return getUnitString(cx, c);
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

  if (hasLatin1Chars()) {
    size_t nbytes = length() * sizeof(JS::Latin1Char);
    gcx->removeCellMemory(this, nbytes, js::MemoryUse::StringContents);

    callbacks()->finalize(const_cast<JS::Latin1Char*>(rawLatin1Chars()));
  } else {
    size_t nbytes = length() * sizeof(char16_t);
    gcx->removeCellMemory(this, nbytes, js::MemoryUse::StringContents);

    callbacks()->finalize(const_cast<char16_t*>(rawTwoByteChars()));
  }
}

#endif /* vm_StringType_inl_h */
