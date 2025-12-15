/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_StringBuilder_h
#define util_StringBuilder_h

#include "mozilla/CheckedInt.h"
#include "mozilla/MaybeOneOf.h"
#include "mozilla/Utf8.h"

#include "frontend/FrontendContext.h"
#include "js/Vector.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"

namespace js {

class FrontendContext;

namespace frontend {
class ParserAtomsTable;
class TaggedParserAtomIndex;
}  // namespace frontend

namespace detail {

// GrowEltsAggressively will multiply the space by a factor of 8 on overflow, to
// avoid very expensive memcpys for large strings (eg giant toJSON output for
// sessionstore.js). Drop back to the normal expansion policy once the buffer
// hits 128MB.
static constexpr size_t AggressiveLimit = 128 << 20;

template <size_t EltSize>
inline size_t GrowEltsAggressively(size_t aOldElts, size_t aIncr) {
  mozilla::CheckedInt<size_t> required =
      mozilla::CheckedInt<size_t>(aOldElts) + aIncr;
  if (!(required * 2).isValid()) {
    return 0;
  }
  required = mozilla::RoundUpPow2(required.value());
  required *= 8;
  if (!(required * EltSize).isValid() || required.value() > AggressiveLimit) {
    // Fall back to doubling behavior if the aggressive growth fails or gets too
    // big.
    return mozilla::detail::GrowEltsByDoubling<EltSize>(aOldElts, aIncr);
  }
  return required.value();
};

}  // namespace detail

class StringBuilderAllocPolicy {
  TempAllocPolicy impl_;
  const arena_id_t& arenaId_;

 public:
  StringBuilderAllocPolicy(FrontendContext* fc, const arena_id_t& arenaId)
      : impl_(fc), arenaId_(arenaId) {}

  StringBuilderAllocPolicy(JSContext* cx, const arena_id_t& arenaId)
      : impl_(cx), arenaId_(arenaId) {}

  template <typename T>
  T* maybe_pod_malloc(size_t numElems) {
    return impl_.maybe_pod_arena_malloc<T>(arenaId_, numElems);
  }
  template <typename T>
  T* maybe_pod_calloc(size_t numElems) {
    return impl_.maybe_pod_arena_calloc<T>(arenaId_, numElems);
  }
  template <typename T>
  T* maybe_pod_realloc(T* p, size_t oldSize, size_t newSize) {
    return impl_.maybe_pod_arena_realloc<T>(arenaId_, p, oldSize, newSize);
  }
  template <typename T>
  T* pod_malloc(size_t numElems) {
    return impl_.pod_arena_malloc<T>(arenaId_, numElems);
  }
  template <typename T>
  T* pod_calloc(size_t numElems) {
    return impl_.pod_arena_calloc<T>(arenaId_, numElems);
  }
  template <typename T>
  T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
    return impl_.pod_arena_realloc<T>(arenaId_, p, oldSize, newSize);
  }
  template <typename T>
  void free_(T* p, size_t numElems = 0) {
    impl_.free_(p, numElems);
  }
  void reportAllocOverflow() const { impl_.reportAllocOverflow(); }
  bool checkSimulatedOOM() const { return impl_.checkSimulatedOOM(); }

  // See ComputeGrowth in mfbt/Vector.h.
  template <size_t EltSize>
  static size_t computeGrowth(size_t aOldElts, size_t aIncr) {
    return detail::GrowEltsAggressively<EltSize>(aOldElts, aIncr);
  }
};

/*
 * String builder that eagerly checks for over-allocation past the maximum
 * string length.
 *
 * Any operation which would exceed the maximum string length causes an
 * exception report on the context and results in a failed return value.
 *
 * Well-sized extractions (which waste no more than 1/4 of their char
 * buffer space) are guaranteed for strings built by this interface.
 * See |extractWellSized|.
 */
class StringBuilder {
 protected:
  template <typename CharT>
  using BufferType =
      Vector<CharT, 80 / sizeof(CharT), StringBuilderAllocPolicy>;

  /*
   * The Vector's buffer may be either stolen or copied, so we need to use
   * TempAllocPolicy and account for the memory manually when stealing.
   */
  using Latin1CharBuffer = BufferType<Latin1Char>;
  using TwoByteCharBuffer = BufferType<char16_t>;

  JSContext* maybeCx_ = nullptr;

  // cb starts out as a Latin1CharBuffer. When a TwoByte char is appended,
  // inflateChars() constructs a TwoByteCharBuffer and copies the Latin1 chars.
  //
  // Note that this buffer can include extra zero bytes for the string buffer
  // header. See numHeaderChars_ below.
  mozilla::MaybeOneOf<Latin1CharBuffer, TwoByteCharBuffer> cb;

  // Number of reserve()'d chars, see inflateChars. Does not include
  // numHeaderChars_.
  size_t reservedExclHeader_ = 0;

  // The number of '\0' characters prepended by JSStringBuilder to reserve space
  // for the mozilla::StringBuffer header. This is always non-zero for
  // JSStringBuilder and zero otherwise.
  //
  // Note that this is an implementation detail: public methods such as |begin|
  // and |length| return the actual string contents/length without these extra
  // characters.
  uint8_t numHeaderChars_ = 0;

  StringBuilder(const StringBuilder& other) = delete;
  void operator=(const StringBuilder& other) = delete;

  // Returns the number of characters to prepend to reserve enough space for the
  // mozilla::StringBuffer header.
  template <typename CharT>
  static constexpr size_t numHeaderChars() {
    static_assert(sizeof(mozilla::StringBuffer) % sizeof(CharT) == 0);
    return sizeof(mozilla::StringBuffer) / sizeof(CharT);
  }

  template <typename CharT>
  MOZ_ALWAYS_INLINE bool isCharType() const {
    return cb.constructed<BufferType<CharT>>();
  }

  MOZ_ALWAYS_INLINE bool isLatin1() const { return isCharType<Latin1Char>(); }

  MOZ_ALWAYS_INLINE bool isTwoByte() const { return isCharType<char16_t>(); }

  template <typename CharT>
  MOZ_ALWAYS_INLINE BufferType<CharT>& chars() {
    MOZ_ASSERT(isCharType<CharT>());
    return cb.ref<BufferType<CharT>>();
  }

  template <typename CharT>
  MOZ_ALWAYS_INLINE const BufferType<CharT>& chars() const {
    MOZ_ASSERT(isCharType<CharT>());
    return cb.ref<BufferType<CharT>>();
  }

  MOZ_ALWAYS_INLINE TwoByteCharBuffer& twoByteChars() {
    return chars<char16_t>();
  }

  MOZ_ALWAYS_INLINE const TwoByteCharBuffer& twoByteChars() const {
    return chars<char16_t>();
  }

  MOZ_ALWAYS_INLINE Latin1CharBuffer& latin1Chars() {
    return chars<Latin1Char>();
  }

  MOZ_ALWAYS_INLINE const Latin1CharBuffer& latin1Chars() const {
    return chars<Latin1Char>();
  }

  [[nodiscard]] bool inflateChars();

  template <typename CharT>
  JSLinearString* finishStringInternal(JSContext* cx, gc::Heap heap);

 public:
  explicit StringBuilder(JSContext* cx,
                         const arena_id_t& arenaId = js::MallocArena)
      : maybeCx_(cx) {
    MOZ_ASSERT(cx);
    cb.construct<Latin1CharBuffer>(StringBuilderAllocPolicy{cx, arenaId});
  }

  // This constructor should only be used if the methods related to the
  // following are not used, because they require a JSContext:
  //   * JSString
  //   * JSAtom
  //   * mozilla::Utf8Unit
  explicit StringBuilder(FrontendContext* fc,
                         const arena_id_t& arenaId = js::MallocArena) {
    MOZ_ASSERT(fc);
    cb.construct<Latin1CharBuffer>(StringBuilderAllocPolicy{fc, arenaId});
  }

  void clear() { shrinkTo(0); }

  [[nodiscard]] bool reserve(size_t len) {
    auto lenWithHeader = mozilla::CheckedInt<size_t>(len) + numHeaderChars_;
    if (MOZ_UNLIKELY(!lenWithHeader.isValid())) {
      ReportAllocationOverflow(maybeCx_);
      return false;
    }
    if (len > reservedExclHeader_) {
      reservedExclHeader_ = len;
    }
    return isLatin1() ? latin1Chars().reserve(lenWithHeader.value())
                      : twoByteChars().reserve(lenWithHeader.value());
  }
  [[nodiscard]] bool growByUninitialized(size_t incr) {
    return isLatin1() ? latin1Chars().growByUninitialized(incr)
                      : twoByteChars().growByUninitialized(incr);
  }
  void shrinkTo(size_t newLength) {
    // Note: this can't overflow because the new length must be <= the current
    // length.
    newLength += numHeaderChars_;
    if (isLatin1()) {
      latin1Chars().shrinkTo(newLength);
    } else {
      twoByteChars().shrinkTo(newLength);
    }
  }
  bool empty() const { return length() == 0; }
  size_t length() const {
    size_t len = isLatin1() ? latin1Chars().length() : twoByteChars().length();
    MOZ_ASSERT(len >= numHeaderChars_);
    return len - numHeaderChars_;
  }
  char16_t getChar(size_t idx) const {
    idx += numHeaderChars_;
    return isLatin1() ? latin1Chars()[idx] : twoByteChars()[idx];
  }

  [[nodiscard]] bool ensureTwoByteChars() {
    return isTwoByte() || inflateChars();
  }

  [[nodiscard]] bool append(const char16_t c) {
    if (isLatin1()) {
      if (c <= JSString::MAX_LATIN1_CHAR) {
        return latin1Chars().append(Latin1Char(c));
      }
      if (!inflateChars()) {
        return false;
      }
    }
    return twoByteChars().append(c);
  }
  [[nodiscard]] bool append(Latin1Char c) {
    return isLatin1() ? latin1Chars().append(c) : twoByteChars().append(c);
  }
  [[nodiscard]] bool append(char c) { return append(Latin1Char(c)); }

  [[nodiscard]] inline bool append(const char16_t* begin, const char16_t* end);

  [[nodiscard]] bool append(const char16_t* chars, size_t len) {
    return append(chars, chars + len);
  }

  [[nodiscard]] bool append(const Latin1Char* begin, const Latin1Char* end) {
    return isLatin1() ? latin1Chars().append(begin, end)
                      : twoByteChars().append(begin, end);
  }
  [[nodiscard]] bool append(const Latin1Char* chars, size_t len) {
    return append(chars, chars + len);
  }

  /**
   * Interpret the provided count of UTF-8 code units as UTF-8, and append
   * the represented code points to this.  If the code units contain invalid
   * UTF-8, leave the internal buffer in a consistent but unspecified state,
   * report an error, and return false.
   */
  [[nodiscard]] bool append(const mozilla::Utf8Unit* units, size_t len);

  [[nodiscard]] bool append(const JS::ConstCharPtr chars, size_t len) {
    return append(chars.get(), chars.get() + len);
  }
  [[nodiscard]] bool appendN(Latin1Char c, size_t n) {
    return isLatin1() ? latin1Chars().appendN(c, n)
                      : twoByteChars().appendN(c, n);
  }

  [[nodiscard]] inline bool append(JSString* str);
  [[nodiscard]] inline bool append(const JSLinearString* str);
  [[nodiscard]] inline bool appendSubstring(JSString* base, size_t off,
                                            size_t len);
  [[nodiscard]] inline bool appendSubstring(const JSLinearString* base,
                                            size_t off, size_t len);
  [[nodiscard]] bool append(const frontend::ParserAtomsTable& parserAtoms,
                            frontend::TaggedParserAtomIndex atom);

  [[nodiscard]] bool append(const char* chars, size_t len) {
    return append(reinterpret_cast<const Latin1Char*>(chars), len);
  }

  template <size_t ArrayLength>
  [[nodiscard]] bool append(const char (&array)[ArrayLength]) {
    return append(array, ArrayLength - 1); /* No trailing '\0'. */
  }

  /* Infallible variants usable when the corresponding space is reserved. */
  void infallibleAppend(Latin1Char c) {
    if (isLatin1()) {
      latin1Chars().infallibleAppend(c);
    } else {
      twoByteChars().infallibleAppend(c);
    }
  }
  void infallibleAppend(char c) { infallibleAppend(Latin1Char(c)); }
  void infallibleAppend(const Latin1Char* chars, size_t len) {
    if (isLatin1()) {
      latin1Chars().infallibleAppend(chars, len);
    } else {
      twoByteChars().infallibleAppend(chars, len);
    }
  }
  void infallibleAppend(const char* chars, size_t len) {
    infallibleAppend(reinterpret_cast<const Latin1Char*>(chars), len);
  }

  void infallibleAppendSubstring(const JSLinearString* base, size_t off,
                                 size_t len);

  /*
   * Because inflation is fallible, these methods should only be used after
   * calling ensureTwoByteChars().
   */
  void infallibleAppend(const char16_t* chars, size_t len) {
    twoByteChars().infallibleAppend(chars, len);
  }
  void infallibleAppend(char16_t c) { twoByteChars().infallibleAppend(c); }

  bool isUnderlyingBufferLatin1() const { return isLatin1(); }

  template <typename CharT>
  CharT* begin() {
    MOZ_ASSERT(chars<CharT>().length() >= numHeaderChars_);
    return chars<CharT>().begin() + numHeaderChars_;
  }

  template <typename CharT>
  CharT* end() {
    return chars<CharT>().end();
  }

  template <typename CharT>
  const CharT* begin() const {
    MOZ_ASSERT(chars<CharT>().length() >= numHeaderChars_);
    return chars<CharT>().begin() + numHeaderChars_;
  }

  template <typename CharT>
  const CharT* end() const {
    return chars<CharT>().end();
  }

  char16_t* rawTwoByteBegin() { return begin<char16_t>(); }
  char16_t* rawTwoByteEnd() { return end<char16_t>(); }
  const char16_t* rawTwoByteBegin() const { return begin<char16_t>(); }
  const char16_t* rawTwoByteEnd() const { return end<char16_t>(); }

  Latin1Char* rawLatin1Begin() { return begin<Latin1Char>(); }
  Latin1Char* rawLatin1End() { return end<Latin1Char>(); }
  const Latin1Char* rawLatin1Begin() const { return begin<Latin1Char>(); }
  const Latin1Char* rawLatin1End() const { return end<Latin1Char>(); }

  /* Identical to finishString() except that an atom is created. */
  JSAtom* finishAtom();
  frontend::TaggedParserAtomIndex finishParserAtom(
      frontend::ParserAtomsTable& parserAtoms, FrontendContext* fc);

  /*
   * Creates a raw string from the characters in this buffer.  The string is
   * exactly the characters in this buffer (inflated to TwoByte), it is *not*
   * null-terminated unless the last appended character was '\0'.
   */
  char16_t* stealChars();
};

// Like StringBuilder, but uses StringBufferArena for the characters.
class JSStringBuilder : public StringBuilder {
 public:
  explicit JSStringBuilder(JSContext* cx)
      : StringBuilder(cx, js::StringBufferArena) {
    // Reserve space for the mozilla::StringBuffer header.
    numHeaderChars_ = numHeaderChars<Latin1Char>();
    MOZ_ALWAYS_TRUE(latin1Chars().appendN('\0', numHeaderChars_));
  }

  /*
   * Creates a string from the characters in this buffer, then (regardless
   * whether string creation succeeded or failed) empties the buffer.
   *
   * Returns nullptr if string creation failed.
   */
  JSLinearString* finishString(gc::Heap heap = gc::Heap::Default);
};

inline bool StringBuilder::append(const char16_t* begin, const char16_t* end) {
  MOZ_ASSERT(begin <= end);
  if (isLatin1()) {
    while (true) {
      if (begin >= end) {
        return true;
      }
      if (*begin > JSString::MAX_LATIN1_CHAR) {
        break;
      }
      if (!latin1Chars().append(*begin)) {
        return false;
      }
      ++begin;
    }
    if (!inflateChars()) {
      return false;
    }
  }
  return twoByteChars().append(begin, end);
}

inline bool StringBuilder::append(const JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  if (isLatin1()) {
    if (str->hasLatin1Chars()) {
      return latin1Chars().append(str->latin1Chars(nogc), str->length());
    }
    if (!inflateChars()) {
      return false;
    }
  }
  return str->hasLatin1Chars()
             ? twoByteChars().append(str->latin1Chars(nogc), str->length())
             : twoByteChars().append(str->twoByteChars(nogc), str->length());
}

inline void StringBuilder::infallibleAppendSubstring(const JSLinearString* base,
                                                     size_t off, size_t len) {
  MOZ_ASSERT(off + len <= base->length());
  MOZ_ASSERT_IF(base->hasTwoByteChars(), isTwoByte());

  JS::AutoCheckCannotGC nogc;
  if (base->hasLatin1Chars()) {
    infallibleAppend(base->latin1Chars(nogc) + off, len);
  } else {
    infallibleAppend(base->twoByteChars(nogc) + off, len);
  }
}

inline bool StringBuilder::appendSubstring(const JSLinearString* base,
                                           size_t off, size_t len) {
  MOZ_ASSERT(off + len <= base->length());

  JS::AutoCheckCannotGC nogc;
  if (isLatin1()) {
    if (base->hasLatin1Chars()) {
      return latin1Chars().append(base->latin1Chars(nogc) + off, len);
    }
    if (!inflateChars()) {
      return false;
    }
  }
  return base->hasLatin1Chars()
             ? twoByteChars().append(base->latin1Chars(nogc) + off, len)
             : twoByteChars().append(base->twoByteChars(nogc) + off, len);
}

inline bool StringBuilder::appendSubstring(JSString* base, size_t off,
                                           size_t len) {
  MOZ_ASSERT(maybeCx_);

  JSLinearString* linear = base->ensureLinear(maybeCx_);
  if (!linear) {
    return false;
  }

  return appendSubstring(linear, off, len);
}

inline bool StringBuilder::append(JSString* str) {
  MOZ_ASSERT(maybeCx_);

  JSLinearString* linear = str->ensureLinear(maybeCx_);
  if (!linear) {
    return false;
  }

  return append(linear);
}

/* ES5 9.8 ToString, appending the result to the string builder. */
extern bool ValueToStringBuilderSlow(JSContext* cx, const Value& v,
                                     StringBuilder& sb);

inline bool ValueToStringBuilder(JSContext* cx, const Value& v,
                                 StringBuilder& sb) {
  if (v.isString()) {
    return sb.append(v.toString());
  }

  return ValueToStringBuilderSlow(cx, v, sb);
}

/* ES5 9.8 ToString for booleans, appending the result to the string builder. */
inline bool BooleanToStringBuilder(bool b, StringBuilder& sb) {
  return b ? sb.append("true") : sb.append("false");
}

} /* namespace js */

#endif /* util_StringBuilder_h */
