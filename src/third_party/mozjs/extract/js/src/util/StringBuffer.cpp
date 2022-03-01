/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "util/StringBuffer.h"

#include "mozilla/Latin1.h"
#include "mozilla/Range.h"

#include <algorithm>

#include "frontend/ParserAtom.h"  // frontend::{ParserAtomsTable, TaggedParserAtomIndex
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "vm/JSObject-inl.h"
#include "vm/StringType-inl.h"

using namespace js;

template <typename CharT, class Buffer>
static CharT* ExtractWellSized(Buffer& cb) {
  size_t capacity = cb.capacity();
  size_t length = cb.length();
  StringBufferAllocPolicy allocPolicy = cb.allocPolicy();

  CharT* buf = cb.extractOrCopyRawBuffer();
  if (!buf) {
    return nullptr;
  }

  /* For medium/big buffers, avoid wasting more than 1/4 of the memory. */
  MOZ_ASSERT(capacity >= length);
  if (length > Buffer::sMaxInlineStorage && capacity - length > length / 4) {
    CharT* tmp = allocPolicy.pod_realloc<CharT>(buf, capacity, length + 1);
    if (!tmp) {
      allocPolicy.free_(buf);
      return nullptr;
    }
    buf = tmp;
  }

  return buf;
}

char16_t* StringBuffer::stealChars() {
  if (isLatin1() && !inflateChars()) {
    return nullptr;
  }

  return ExtractWellSized<char16_t>(twoByteChars());
}

bool StringBuffer::inflateChars() {
  MOZ_ASSERT(isLatin1());

  TwoByteCharBuffer twoByte(StringBufferAllocPolicy{cx_, arenaId_});

  /*
   * Note: we don't use Vector::capacity() because it always returns a
   * value >= sInlineCapacity. Since Latin1CharBuffer::sInlineCapacity >
   * TwoByteCharBuffer::sInlineCapacitychars, we'd always malloc here.
   */
  size_t capacity = std::max(reserved_, latin1Chars().length());
  if (!twoByte.reserve(capacity)) {
    return false;
  }

  twoByte.infallibleGrowByUninitialized(latin1Chars().length());

  mozilla::ConvertLatin1toUtf16(mozilla::AsChars(latin1Chars()), twoByte);

  cb.destroy();
  cb.construct<TwoByteCharBuffer>(std::move(twoByte));
  return true;
}

bool StringBuffer::append(const frontend::ParserAtomsTable& parserAtoms,
                          frontend::TaggedParserAtomIndex atom) {
  return parserAtoms.appendTo(*this, atom);
}

template <typename CharT>
JSLinearString* StringBuffer::finishStringInternal(JSContext* cx) {
  size_t len = length();

  if (JSAtom* staticStr = cx->staticStrings().lookup(begin<CharT>(), len)) {
    return staticStr;
  }

  if (JSInlineString::lengthFits<CharT>(len)) {
    mozilla::Range<const CharT> range(begin<CharT>(), len);
    return NewInlineString<CanGC>(cx, range);
  }

  UniquePtr<CharT[], JS::FreePolicy> buf(
      ExtractWellSized<CharT>(chars<CharT>()));

  if (!buf) {
    return nullptr;
  }

  JSLinearString* str = NewStringDontDeflate<CanGC>(cx, std::move(buf), len);
  if (!str) {
    return nullptr;
  }

  return str;
}

JSLinearString* JSStringBuilder::finishString() {
  size_t len = length();
  if (len == 0) {
    return cx_->names().empty;
  }

  if (!JSString::validateLength(cx_, len)) {
    return nullptr;
  }

  static_assert(JSFatInlineString::MAX_LENGTH_TWO_BYTE <
                TwoByteCharBuffer::InlineLength);
  static_assert(JSFatInlineString::MAX_LENGTH_LATIN1 <
                Latin1CharBuffer::InlineLength);

  return isLatin1() ? finishStringInternal<Latin1Char>(cx_)
                    : finishStringInternal<char16_t>(cx_);
}

JSAtom* StringBuffer::finishAtom() {
  size_t len = length();
  if (len == 0) {
    return cx_->names().empty;
  }

  if (isLatin1()) {
    JSAtom* atom = AtomizeChars(cx_, latin1Chars().begin(), len);
    latin1Chars().clear();
    return atom;
  }

  JSAtom* atom = AtomizeChars(cx_, twoByteChars().begin(), len);
  twoByteChars().clear();
  return atom;
}

frontend::TaggedParserAtomIndex StringBuffer::finishParserAtom(
    frontend::ParserAtomsTable& parserAtoms) {
  size_t len = length();
  if (len == 0) {
    return frontend::TaggedParserAtomIndex::WellKnown::empty();
  }

  if (isLatin1()) {
    auto result = parserAtoms.internLatin1(cx_, latin1Chars().begin(), len);
    latin1Chars().clear();
    return result;
  }

  auto result = parserAtoms.internChar16(cx_, twoByteChars().begin(), len);
  twoByteChars().clear();
  return result;
}

bool js::ValueToStringBufferSlow(JSContext* cx, const Value& arg,
                                 StringBuffer& sb) {
  RootedValue v(cx, arg);
  if (!ToPrimitive(cx, JSTYPE_STRING, &v)) {
    return false;
  }

  if (v.isString()) {
    return sb.append(v.toString());
  }
  if (v.isNumber()) {
    return NumberValueToStringBuffer(cx, v, sb);
  }
  if (v.isBoolean()) {
    return BooleanToStringBuffer(v.toBoolean(), sb);
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
