/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "util/StringBuilder.h"

#include "mozilla/Latin1.h"
#include "mozilla/Range.h"

#include <algorithm>

#include "frontend/ParserAtom.h"  // frontend::{ParserAtomsTable, TaggedParserAtomIndex
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "vm/BigIntType.h"
#include "vm/StaticStrings.h"

#include "vm/JSObject-inl.h"
#include "vm/StringType-inl.h"

using namespace js;

template <typename CharT, class Buffer>
static CharT* ExtractWellSized(Buffer& cb) {
  size_t capacity = cb.capacity();
  size_t length = cb.length();
  StringBuilderAllocPolicy allocPolicy = cb.allocPolicy();

  CharT* buf = cb.extractOrCopyRawBuffer();
  if (!buf) {
    return nullptr;
  }

  // For medium/big buffers, avoid wasting more than 1/4 of the memory. Very
  // small strings will not reach here because they will have been stored in a
  // JSInlineString. Don't bother shrinking the allocation unless at least 80
  // bytes will be saved, which is a somewhat arbitrary number (though it does
  // correspond to a mozjemalloc size class.)
  MOZ_ASSERT(capacity >= length);
  constexpr size_t minCharsToReclaim = 80 / sizeof(CharT);
  if (capacity - length >= minCharsToReclaim &&
      capacity - length > capacity / 4) {
    CharT* tmp = allocPolicy.pod_realloc<CharT>(buf, capacity, length);
    if (!tmp) {
      allocPolicy.free_(buf);
      return nullptr;
    }
    buf = tmp;
  }

  return buf;
}

char16_t* StringBuilder::stealChars() {
  // stealChars shouldn't be used with JSStringBuilder because JSStringBuilder
  // reserves space for the header bytes in the vector.
  MOZ_RELEASE_ASSERT(numHeaderChars_ == 0);

  if (isLatin1() && !inflateChars()) {
    return nullptr;
  }

  return ExtractWellSized<char16_t>(twoByteChars());
}

bool StringBuilder::inflateChars() {
  MOZ_ASSERT(isLatin1());

  TwoByteCharBuffer twoByte(latin1Chars().allocPolicy());

  // Note: each char16_t is two bytes, so we need to change the number of header
  // characters.
  MOZ_ASSERT(numHeaderChars_ == 0 ||
             numHeaderChars_ == numHeaderChars<Latin1Char>());
  MOZ_ASSERT(latin1Chars().length() >= numHeaderChars_);
  size_t numHeaderCharsNew =
      numHeaderChars_ > 0 ? numHeaderChars<char16_t>() : 0;

  /*
   * Note: we don't use Vector::capacity() because it always returns a
   * value >= sInlineCapacity. Since Latin1CharBuffer::sInlineCapacity >
   * TwoByteCharBuffer::sInlineCapacitychars, we'd always malloc here.
   */
  size_t reserved = reservedExclHeader_ + numHeaderChars_;
  size_t capacity = std::max(reserved, latin1Chars().length());
  capacity = capacity - numHeaderChars_ + numHeaderCharsNew;
  if (!twoByte.reserve(capacity)) {
    return false;
  }

  twoByte.infallibleAppendN('\0', numHeaderCharsNew);

  auto charsSource = mozilla::AsChars(latin1Chars()).From(numHeaderChars_);
  twoByte.infallibleGrowByUninitialized(charsSource.Length());

  auto charsDest = mozilla::Span<char16_t>(twoByte).From(numHeaderCharsNew);
  mozilla::ConvertLatin1toUtf16(charsSource, charsDest);

  MOZ_ASSERT(twoByte.length() == numHeaderCharsNew + length());

  cb.destroy();
  cb.construct<TwoByteCharBuffer>(std::move(twoByte));
  numHeaderChars_ = numHeaderCharsNew;
  return true;
}

bool StringBuilder::append(const frontend::ParserAtomsTable& parserAtoms,
                           frontend::TaggedParserAtomIndex atom) {
  return parserAtoms.appendTo(*this, atom);
}

template <typename CharT>
JSLinearString* StringBuilder::finishStringInternal(JSContext* cx,
                                                    gc::Heap heap) {
  // The Vector must include space for the mozilla::StringBuffer header.
  MOZ_ASSERT(numHeaderChars_ == numHeaderChars<CharT>());
#ifdef DEBUG
  auto isZeroChar = [](CharT c) { return c == '\0'; };
  MOZ_ASSERT(std::all_of(chars<CharT>().begin(),
                         chars<CharT>().begin() + numHeaderChars_, isZeroChar));
#endif

  size_t len = length();

  if (JSAtom* staticStr = cx->staticStrings().lookup(begin<CharT>(), len)) {
    return staticStr;
  }

  if (JSInlineString::lengthFits<CharT>(len)) {
    mozilla::Range<const CharT> range(begin<CharT>(), len);
    return NewInlineString<CanGC>(cx, range);
  }

  // Use NewStringCopyNDontDeflate if the string is too short for a buffer,
  // because:
  //
  //  (1) If the string is very short and fits in the Vector's inline storage,
  //      we can potentially nursery-allocate the characters and avoid a malloc
  //      call.
  //  (2) ExtractWellSized often performs a realloc because we over-allocate in
  //      StringBufferAllocPolicy. After this we'd still have to move/copy the
  //      characters in memory to discard the space we reserved for the
  //      mozilla::StringBuffer header. Because we have to copy the characters
  //      anyway, use NewStringCopyNDontDeflate instead where we can allocate in
  //      the nursery.
  if (len < JSString::MIN_BYTES_FOR_BUFFER / sizeof(CharT)) {
    return NewStringCopyNDontDeflate<CanGC>(cx, begin<CharT>(), len, heap);
  }

  if (MOZ_UNLIKELY(!mozilla::StringBuffer::IsValidLength<CharT>(len))) {
    ReportAllocationOverflow(cx);
    return nullptr;
  }

  // mozilla::StringBuffer requires a null terminator.
  auto& charsWithHeader = chars<CharT>();
  if (!charsWithHeader.append('\0')) {
    return nullptr;
  }

  CharT* mem = ExtractWellSized<CharT>(charsWithHeader);
  if (!mem) {
    return nullptr;
  }
  // The Vector is now empty and may be used again, so re-reserve space for
  // the header.
  MOZ_ASSERT(charsWithHeader.empty());
  MOZ_ALWAYS_TRUE(charsWithHeader.appendN('\0', numHeaderChars_));

  // Initialize the StringBuffer header.
  RefPtr<mozilla::StringBuffer> buffer =
      mozilla::StringBuffer::ConstructInPlace(mem, (len + 1) * sizeof(CharT));
  MOZ_ASSERT(buffer->Data() == mem + numHeaderChars_,
             "chars are where mozilla::StringBuffer expects them");
  MOZ_ASSERT(static_cast<CharT*>(buffer->Data())[len] == '\0',
             "StringBuffer must be null-terminated");

  Rooted<JSString::OwnedChars<CharT>> owned(cx, std::move(buffer), len);
  return JSLinearString::new_<CanGC, CharT>(cx, &owned, heap);
}

JSLinearString* JSStringBuilder::finishString(gc::Heap heap) {
  MOZ_ASSERT(maybeCx_);

  size_t len = length();
  if (len == 0) {
    return maybeCx_->names().empty_;
  }

  if (MOZ_UNLIKELY(!JSString::validateLength(maybeCx_, len))) {
    return nullptr;
  }

  static_assert(JSFatInlineString::MAX_LENGTH_TWO_BYTE <
                TwoByteCharBuffer::InlineLength);
  static_assert(JSFatInlineString::MAX_LENGTH_LATIN1 <
                Latin1CharBuffer::InlineLength);

  return isLatin1() ? finishStringInternal<Latin1Char>(maybeCx_, heap)
                    : finishStringInternal<char16_t>(maybeCx_, heap);
}

JSAtom* StringBuilder::finishAtom() {
  MOZ_ASSERT(maybeCx_);

  size_t len = length();
  if (len == 0) {
    return maybeCx_->names().empty_;
  }

  JSAtom* atom = isLatin1() ? AtomizeChars(maybeCx_, rawLatin1Begin(), len)
                            : AtomizeChars(maybeCx_, rawTwoByteBegin(), len);
  clear();
  return atom;
}

frontend::TaggedParserAtomIndex StringBuilder::finishParserAtom(
    frontend::ParserAtomsTable& parserAtoms, FrontendContext* fc) {
  size_t len = length();
  if (len == 0) {
    return frontend::TaggedParserAtomIndex::WellKnown::empty();
  }

  auto result = isLatin1()
                    ? parserAtoms.internLatin1(fc, rawLatin1Begin(), len)
                    : parserAtoms.internChar16(fc, rawTwoByteBegin(), len);
  clear();
  return result;
}

bool js::ValueToStringBuilderSlow(JSContext* cx, const Value& arg,
                                  StringBuilder& sb) {
  RootedValue v(cx, arg);
  if (!ToPrimitive(cx, JSTYPE_STRING, &v)) {
    return false;
  }

  if (v.isString()) {
    return sb.append(v.toString());
  }
  if (v.isNumber()) {
    return NumberValueToStringBuilder(v, sb);
  }
  if (v.isBoolean()) {
    return BooleanToStringBuilder(v.toBoolean(), sb);
  }
  if (v.isNull()) {
    return sb.append(cx->names().null);
  }
  if (v.isSymbol()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SYMBOL_TO_STRING);
    return false;
  }
  if (v.isBigInt()) {
    RootedBigInt i(cx, v.toBigInt());
    JSLinearString* str = BigInt::toString<CanGC>(cx, i, 10);
    if (!str) {
      return false;
    }
    return sb.append(str);
  }
  MOZ_ASSERT(v.isUndefined());
  return sb.append(cx->names().undefined);
}
